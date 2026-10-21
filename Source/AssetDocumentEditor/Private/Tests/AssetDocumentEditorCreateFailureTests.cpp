// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// A Create that fails at any phase after the target was made leaves nothing behind and does not
// poison the atomic framework (a failed Create's residue made later requests fail
// with Atomic.Poisoned / RollbackCreated.ResidueDetected). For several asset classes (a generic
// asset, a User Defined Enum, a Material, a Blueprint, a Niagara System and Script) the production Service applies the class's
// Create template while FAssetDocumentRuntimeTestHooks fails it at one point:
//   - the coordinator's Apply, Repair and Persist phases (the target exists in memory only);
//   - each persistence step that writes: staging the sidecar, preparing the journal, installing the
//     package header and the sidecar, advancing the sync state, marking durable completion;
//   - the coordinator's Verify phase (the package and sidecar are installed on disk; the
//     sidecar keeps the installed canonical text of the same document);
//   - the post-commit cleanup, which must not fail the apply (it is deferred).
// After each failure: no object, package, registry entry or package file, the sidecar file is the
// document as written, no poisoning or rollback residue diagnostic, and the same Create then applies.
//
// GenericRuntime.Rollback.CreateFailureCompile: a Create whose own content fails to compile (a Niagara
// module whose custom HLSL node uses an undefined symbol; a Material whose texture sample has no
// texture) fails, the created asset is deleted (no RollbackCreated.DeleteRejected /
// Atomic.Poisoned.RollbackFailed: N4 saw that on a failed Niagara script Create), and the corrected
// document then creates the asset.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "Runtime/AssetDocumentRuntimeTestHooks.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorCreateFailureTests
{
const TCHAR* Folder = TEXT("AssetDocumentCreateFailure");

FAssetDocumentService& Service()
{
	return FAssetDocumentModule::Get().GetService();
}

struct FTarget
{
	FString AssetName;
	FString PackageName;
	FString ObjectPath;
	FString PackageFile;
	FString SidecarPath;

	explicit FTarget(const FString& Prefix)
		: AssetName(FString::Printf(TEXT("%s_%s"), *Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits)))
	{
		PackageName = FString::Printf(TEXT("/Game/%s/%s"), Folder, *AssetName);
		ObjectPath = PackageName + TEXT(".") + AssetName;
		PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		SidecarPath = FPaths::Combine(FPaths::ProjectContentDir(), Folder, AssetName + TEXT(".assetdoc.json"));
	}

	// Where the target is still present: "object, package, ..." (empty when absent everywhere).
	FString Residue() const
	{
		TArray<FString> Left;
		if (FSoftObjectPath(ObjectPath).ResolveObject() != nullptr) { Left.Add(TEXT("object")); }
		if (UPackage* Package = FindPackage(nullptr, *PackageName))
		{
			Left.Add(Package->IsDirty() ? TEXT("dirty package") : TEXT("package"));
		}
		if (FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid())
		{
			Left.Add(TEXT("asset registry entry"));
		}
		if (IFileManager::Get().FileExists(*PackageFile)) { Left.Add(TEXT("package file")); }
		return FString::Join(Left, TEXT(", "));
	}

	void Cleanup() const
	{
		if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting && !GEditor->IsTransactionActive()
			&& GEditor->Trans->GetQueueLength() > 0)
		{
			GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentCreateFailureTests", "Cleanup", "Create failure test cleanup"));
		}
		if (UObject* Asset = FSoftObjectPath(ObjectPath).ResolveObject())
		{
			Asset->GetOutermost()->SetDirtyFlag(false);
			if (IFileManager::Get().FileExists(*PackageFile))
			{
				UPackageTools::UnloadPackages({Asset->GetOutermost()});
			}
			else
			{
				ObjectTools::DeleteObjects({Asset}, false, ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
			}
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		if (IFileManager::Get().FileExists(*PackageFile))
		{
			IFileManager::Get().Delete(*PackageFile, false, true);
			FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({PackageFile});
		}
		IFileManager::Get().Delete(*SidecarPath, false, true);
	}
};

struct FFailurePoint
{
	const TCHAR* Name;
	TOptional<EAssetDocumentRuntimeRequestPhase> Phase;
	const TCHAR* Event;
	bool bApplySucceeds;
	// The failure comes after durable completion: the sidecar was installed in canonical form
	// (same document, the writer's formatting is not kept).
	bool bAfterDurableCompletion = false;
};

FString Serialize(const TSharedRef<FJsonObject>& Object)
{
	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	FJsonSerializer::Serialize(Object, Writer);
	return Text;
}

bool SameDocument(const FString& Text, const TSharedRef<FJsonObject>& Document)
{
	TSharedPtr<FJsonObject> Parsed;
	return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Parsed) && Parsed.IsValid()
		&& FJsonValue::CompareEqual(FJsonValueObject(Parsed), FJsonValueObject(Document));
}

FAssetDocumentResult ApplyText(FAutomationTestBase& Test, const FTarget& Target, const FString& Json, const FString& Stage)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target.SidecarPath), true);
	Test.TestTrue(*FString::Printf(TEXT("%s writes the sidecar"), *Stage), FFileHelper::SaveStringToFile(Json, *Target.SidecarPath));
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Target.SidecarPath;
	Request.bSaveAsset = true;
	return Service().ApplyFile(Request);
}

void Report(FAutomationTestBase& Test, const FString& Stage, const FAssetDocumentResult& Result)
{
	Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *Result.Message));
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Test.AddInfo(FString::Printf(TEXT("%s: [%s] %s %s"), *Stage, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
	}
}

bool HasCodePrefix(const FAssetDocumentResult& Result, const TCHAR* Prefix)
{
	return Result.Diagnostics.ContainsByPredicate([Prefix](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Code.StartsWith(Prefix);
	});
}

TSharedPtr<FJsonObject> Parse(const FString& Text)
{
	TSharedPtr<FJsonObject> Object;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object);
	return Object;
}

// The extract of an existing target, as a document.
TSharedPtr<FJsonObject> ExtractDocument(FAutomationTestBase& Test, const FTarget& Target, const FString& Stage)
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = Target.ObjectPath;
	const FAssetDocumentResult Result = Service().Extract(Request);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: extract succeeds"), *Stage), Result.IsSuccess() && Result.Payload.IsValid()))
	{
		Report(Test, Stage, Result);
		return nullptr;
	}
	return Result.Payload;
}

// The first node of a graph region ({"Nodes": [{"Id", "Action", ...}]}) with Action.
TSharedPtr<FJsonObject> NodeWithAction(const TSharedPtr<FJsonObject>& Graph, const TCHAR* Action)
{
	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	if (Graph.IsValid() && Graph->TryGetArrayField(TEXT("Nodes"), Nodes))
	{
		for (const TSharedPtr<FJsonValue>& Node : *Nodes)
		{
			const TSharedPtr<FJsonObject> Object = Node->AsObject();
			FString NodeAction;
			if (Object.IsValid() && Object->TryGetStringField(TEXT("Action"), NodeAction) && NodeAction == Action)
			{
				return Object;
			}
		}
	}
	return nullptr;
}

TSharedRef<FJsonObject> Link(const FString& From, const FString& To)
{
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("From"), From);
	Out->SetStringField(TEXT("To"), To);
	return Out;
}

// A Create of Target that fails with a compile error (code prefix CompileCode) must leave nothing and
// not poison; the corrected document then creates the asset.
void TestFailedCompileCreate(FAutomationTestBase& Test, const FString& Label, const FTarget& Target,
	const TSharedRef<FJsonObject>& Broken, const TSharedRef<FJsonObject>& Fixed, const TCHAR* CompileCode)
{
	const FAssetDocumentResult Failed = ApplyText(Test, Target, Serialize(Broken), Label);
	Report(Test, Label, Failed);
	Test.TestFalse(*FString::Printf(TEXT("%s: the Create fails"), *Label), Failed.IsSuccess());
	Test.TestTrue(*FString::Printf(TEXT("%s: the failure is the compile error (%s)"), *Label, CompileCode), HasCodePrefix(Failed, CompileCode));
	Test.TestFalse(*FString::Printf(TEXT("%s: no poisoning"), *Label), HasCodePrefix(Failed, TEXT("Atomic.Poisoned")));
	Test.TestFalse(*FString::Printf(TEXT("%s: no rollback residue"), *Label),
		HasCodePrefix(Failed, TEXT("RollbackCreated.")) || HasCodePrefix(Failed, TEXT("CutoverRollback.")));
	Test.TestEqual(*FString::Printf(TEXT("%s: nothing is left"), *Label), Target.Residue(), FString());
	const FAssetDocumentResult Retried = ApplyText(Test, Target, Serialize(Fixed), Label + TEXT(" corrected"));
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: the corrected Create then applies"), *Label), Retried.IsSuccess()))
	{
		Report(Test, Label + TEXT(" corrected"), Retried);
	}
}
} // namespace AssetDocumentEditorCreateFailureTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentEditorCreateFailureLeavesNothingTest,
	"UEAssetDoc.GenericRuntime.Rollback.CreateFailureLeavesNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEditorCreateFailureLeavesNothingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorCreateFailureTests;
	ON_SCOPE_EXIT { FAssetDocumentRuntimeTestHooks::Reset(); };
	const TArray<FFailurePoint> Points = {
		{TEXT("Apply"), EAssetDocumentRuntimeRequestPhase::Apply, nullptr, false},
		{TEXT("Repair"), EAssetDocumentRuntimeRequestPhase::Repair, nullptr, false},
		{TEXT("Persist"), EAssetDocumentRuntimeRequestPhase::Persist, nullptr, false},
		{TEXT("StageSidecar"), {}, TEXT("StageSidecar"), false},
		{TEXT("PrepareJournal"), {}, TEXT("PrepareJournal"), false},
		{TEXT("InstallPackageHeader"), {}, TEXT("InstallPackageHeader"), false},
		{TEXT("InstallSidecar"), {}, TEXT("InstallSidecar"), false},
		{TEXT("AdvanceSync"), {}, TEXT("AdvanceSync"), false},
		{TEXT("MarkDurableComplete"), {}, TEXT("MarkDurableComplete"), false},
		{TEXT("Verify"), EAssetDocumentRuntimeRequestPhase::Verify, nullptr, false, true},
		{TEXT("Cleanup"), {}, TEXT("Cleanup"), true},
	};
	const TArray<TPair<const TCHAR*, const TCHAR*>> Classes = {
		{TEXT("Curve"), TEXT("/Script/Engine.CurveFloat")},
		{TEXT("Enum"), TEXT("/Script/Engine.UserDefinedEnum")},
		{TEXT("Material"), TEXT("/Script/Engine.Material")},
		{TEXT("Blueprint"), TEXT("/Script/Engine.Blueprint")},
		{TEXT("Niagara"), TEXT("/Script/Niagara.NiagaraSystem")},
		{TEXT("NiagaraScript"), TEXT("/Script/Niagara.NiagaraScript")},
	};
	for (const TPair<const TCHAR*, const TCHAR*>& Class : Classes)
	{
		FAssetDocumentTemplateRequest TemplateRequest;
		TemplateRequest.Class = Class.Value;
		TemplateRequest.Target = FString::Printf(TEXT("/Game/%s/Template"), Folder);
		const FAssetDocumentResult Template = Service().CreateTemplate(TemplateRequest);
		if (!TestTrue(FString::Printf(TEXT("%s template"), Class.Key), Template.IsSuccess() && Template.Payload.IsValid()))
		{
			Report(*this, FString::Printf(TEXT("%s template"), Class.Key), Template);
			continue;
		}
		for (const FFailurePoint& Point : Points)
		{
			const FString Stage = FString::Printf(TEXT("%s at %s"), Class.Key, Point.Name);
			const FTarget Target(FString::Printf(TEXT("%s_%s"), Class.Key, Point.Name));
			ON_SCOPE_EXIT { Target.Cleanup(); };
			const TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
			Document->Values = Template.Payload->Values;
			Document->SetStringField(TEXT("Target"), Target.PackageName);
			Document->SetStringField(TEXT("Action"), TEXT("Create"));
			const FString Json = Serialize(Document);

			if (Point.Phase.IsSet())
			{
				FAssetDocumentRuntimeTestHooks::FailNextRequestAtPhase(Point.Phase.GetValue());
			}
			else
			{
				FAssetDocumentRuntimeTestHooks::FailNextPersistenceAtEvent(Point.Event);
			}
			const FAssetDocumentResult Failed = ApplyText(*this, Target, Json, Stage);
			FAssetDocumentRuntimeTestHooks::Reset();
			Report(*this, Stage, Failed);
			TestFalse(*FString::Printf(TEXT("%s: no poisoning"), *Stage), HasCodePrefix(Failed, TEXT("Atomic.Poisoned")));
			TestFalse(*FString::Printf(TEXT("%s: no rollback residue"), *Stage),
				HasCodePrefix(Failed, TEXT("RollbackCreated.")) || HasCodePrefix(Failed, TEXT("CutoverRollback.")));
			if (Point.bApplySucceeds)
			{
				TestTrue(*FString::Printf(TEXT("%s: the apply succeeds"), *Stage), Failed.IsSuccess());
				continue;
			}
			TestFalse(*FString::Printf(TEXT("%s: the apply fails"), *Stage), Failed.IsSuccess());
			TestEqual(*FString::Printf(TEXT("%s: nothing is left"), *Stage), Target.Residue(), FString());
			FString SidecarText;
			FFileHelper::LoadFileToString(SidecarText, *Target.SidecarPath);
			if (Point.bAfterDurableCompletion)
			{
				// The installed canonical sidecar: a Create is written back as CreateOrUpdate.
				const TSharedRef<FJsonObject> Canonical = MakeShared<FJsonObject>();
				Canonical->Values = Document->Values;
				Canonical->SetStringField(TEXT("Action"), TEXT("CreateOrUpdate"));
				TestTrue(*FString::Printf(TEXT("%s: the sidecar is the canonical document"), *Stage), SameDocument(SidecarText, Canonical));
			}
			else
			{
				TestEqual(*FString::Printf(TEXT("%s: the sidecar is the document as written"), *Stage), SidecarText, Json);
			}

			const FAssetDocumentResult Retried = ApplyText(*this, Target, Json, Stage + TEXT(" retry"));
			if (!TestTrue(*FString::Printf(TEXT("%s: the same Create then applies"), *Stage), Retried.IsSuccess()))
			{
				Report(*this, Stage + TEXT(" retry"), Retried);
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentEditorCreateFailureCompileTest,
	"UEAssetDoc.GenericRuntime.Rollback.CreateFailureCompile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEditorCreateFailureCompileTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorCreateFailureTests;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	// The custom HLSL below does not compile: engines whose Vector VM compile is a shader compile also log the shader
	// compiler's errors as an error of their own.
	AddExpectedError(TEXT("errors encountered compiling Vector VM shaders:[\\s\\S]*AssetDocumentUndefinedSymbol"), EAutomationExpectedErrorFlags::Contains, 0);
#endif
	// Niagara module: a module made by a Create document gives the factory graph (input map -> Map Get /
	// Map Set -> output); a custom HLSL node is wired between them as in the script editor.
	{
		const FTarget Source(TEXT("NiagaraModuleSource"));
		const FTarget Target(TEXT("NiagaraModuleCompile"));
		ON_SCOPE_EXIT
		{
			Target.Cleanup();
			Source.Cleanup();
		};
		const FString SourceJson = FString::Printf(TEXT(R"json({"SchemaVersion": 1, "Target": "%s",
			"Class": "/Script/Niagara.NiagaraScript", "Action": "Create", "Definitions": {}, "Properties": {},
			"Body": {"Usage": "Module"}})json"), *Source.PackageName);
		const FAssetDocumentResult Created = ApplyText(*this, Source, SourceJson, TEXT("module source"));
		if (!TestTrue(TEXT("a module is created from a document"), Created.IsSuccess()))
		{
			Report(*this, TEXT("module source"), Created);
			return true;
		}
		const TSharedPtr<FJsonObject> Extracted = ExtractDocument(*this, Source, TEXT("module source"));
		const TSharedPtr<FJsonObject>* Body = nullptr;
		const TSharedPtr<FJsonObject>* GraphField = nullptr;
		if (!Extracted.IsValid() || !Extracted->TryGetObjectField(TEXT("Body"), Body) || !(*Body)->TryGetObjectField(TEXT("Graph"), GraphField))
		{
			AddError(TEXT("the module extract has Body.Graph"));
			return true;
		}
		const TSharedPtr<FJsonObject> Get = NodeWithAction(*GraphField, TEXT("Niagara:MapGet"));
		const TSharedPtr<FJsonObject> Set = NodeWithAction(*GraphField, TEXT("Niagara:MapSet"));
		if (!TestTrue(TEXT("the new module has a Map Get and a Map Set"), Get.IsValid() && Set.IsValid()))
		{
			return true;
		}
		const FString GetId = Get->GetStringField(TEXT("Id"));
		const FString SetId = Set->GetStringField(TEXT("Id"));
		auto Module = [&](const TCHAR* Hlsl)
		{
			const TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
			Document->Values = Parse(Serialize(Extracted.ToSharedRef()))->Values;
			Document->SetStringField(TEXT("Target"), Target.PackageName);
			Document->SetStringField(TEXT("Action"), TEXT("Create"));
			const TSharedPtr<FJsonObject> Graph = Document->GetObjectField(TEXT("Body"))->GetObjectField(TEXT("Graph"));
			NodeWithAction(Graph, TEXT("Niagara:MapGet"))->SetObjectField(TEXT("Pins"),
				Parse(TEXT(R"json({"Outputs": [{"Name": "Engine.Time", "Type": "float"}]})json")));
			NodeWithAction(Graph, TEXT("Niagara:MapSet"))->SetObjectField(TEXT("Pins"),
				Parse(TEXT(R"json({"Inputs": [{"Name": "Particles.Position", "Type": "Position"}]})json")));
			const TSharedPtr<FJsonObject> Node = Parse(TEXT(R"json({"Id": "Drift", "Action": "Niagara:CustomHlsl", "Position": [300, 200],
				"Pins": {"Inputs": [{"Name": "Time", "Type": "float"}], "Outputs": [{"Name": "Position", "Type": "Position"}]},
				"Properties": {"CustomHlsl": ""}})json"));
			Node->GetObjectField(TEXT("Properties"))->SetStringField(TEXT("CustomHlsl"), Hlsl);
			TArray<TSharedPtr<FJsonValue>> Nodes = Graph->GetArrayField(TEXT("Nodes"));
			Nodes.Add(MakeShared<FJsonValueObject>(Node));
			Graph->SetArrayField(TEXT("Nodes"), Nodes);
			TArray<TSharedPtr<FJsonValue>> Links = Graph->GetArrayField(TEXT("Links"));
			Links.Add(MakeShared<FJsonValueObject>(Link(GetId + TEXT(":Engine.Time"), TEXT("Drift:Time"))));
			Links.Add(MakeShared<FJsonValueObject>(Link(TEXT("Drift:Position"), SetId + TEXT(":Particles.Position"))));
			Graph->SetArrayField(TEXT("Links"), Links);
			return Document;
		};
		TestFailedCompileCreate(*this, TEXT("Niagara module"), Target,
			Module(TEXT("Position = float3(Time, 0, 0) * AssetDocumentUndefinedSymbol;")),
			Module(TEXT("Position = float3(Time, 0, 0);")), TEXT("Niagara."));
	}
	// Material: a Texture Sample without a texture feeding Base Color (the editor's compile error
	// "Missing input texture"), corrected to a constant color.
	{
		const FTarget Target(TEXT("MaterialCompile"));
		ON_SCOPE_EXIT { Target.Cleanup(); };
		AddExpectedMessagePlain(TEXT("Failed to compile Material"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
		auto Material = [&Target](const TCHAR* Expression, const TCHAR* Source)
		{
			return Parse(FString::Printf(TEXT(R"json({"SchemaVersion": 1, "Target": "%s",
				"Class": "/Script/Engine.Material", "Action": "Create", "Definitions": {}, "Properties": {},
				"Body": {"Expressions": {"Color": %s}, "Outputs": {"BaseColor": "%s"}}})json"),
				*Target.PackageName, Expression, Source)).ToSharedRef();
		};
		TestFailedCompileCreate(*this, TEXT("Material"), Target,
			Material(TEXT(R"json({"Class": "TextureSample", "Position": [-400, 0]})json"), TEXT("Color.RGB")),
			Material(TEXT(R"json({"Class": "Constant3Vector", "Position": [-400, 0], "Properties": {"Constant": {"R": 1, "G": 0, "B": 0, "A": 0}}})json"), TEXT("Color")),
			TEXT("Material."));
	}
	return true;
}

namespace AssetDocumentEditorCreateFailureTests
{
// A Blackboard (BB_Enemy): an Object key and Vector keys in the shapes an AI writes
// ({} and the bUseDefaultValue form), plus a Rotator key with a partial default.
FString BlackboardDocument(const FTarget& Target, const TCHAR* HomeX)
{
	return FString::Printf(TEXT(R"json({"SchemaVersion": 1, "Target": "%s", "Class": "/Script/AIModule.BlackboardData",
		"Action": "CreateOrUpdate", "Definitions": {}, "Properties": {}, "Body": {"Parent": null, "Keys": [
		{"Name": "TargetActor", "Category": "", "Description": "", "bInstanceSynced": false,
		 "KeyTypeClass": {"Kind": "ClassRef", "Path": "/Script/AIModule.BlackboardKeyType_Object"},
		 "KeyTypeProperties": {"BaseClass": {"Kind": "ClassRef", "Path": "/Script/Engine.Actor"}, "DefaultValue": null}},
		{"Name": "PatrolLocation", "Category": "", "Description": "", "bInstanceSynced": false,
		 "KeyTypeClass": {"Kind": "ClassRef", "Path": "/Script/AIModule.BlackboardKeyType_Vector"}, "KeyTypeProperties": {}},
		{"Name": "HomeLocation", "Category": "Nav", "Description": "", "bInstanceSynced": false,
		 "KeyTypeClass": {"Kind": "ClassRef", "Path": "/Script/AIModule.BlackboardKeyType_Vector"},
		 "KeyTypeProperties": {"DefaultValue": {"X": %s, "Y": -20, "Z": 30.5}, "bUseDefaultValue": true}},
		{"Name": "Facing", "Category": "", "Description": "", "bInstanceSynced": false,
		 "KeyTypeClass": {"Kind": "ClassRef", "Path": "/Script/AIModule.BlackboardKeyType_Rotator"},
		 "KeyTypeProperties": {"DefaultValue": {"Yaw": 90}}}]}})json"), *Target.PackageName, HomeX);
}

TSharedPtr<FJsonObject> KeyProperties(const TSharedPtr<FJsonObject>& Document, const TCHAR* Name)
{
	const TSharedPtr<FJsonObject>* Body = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
	if (Document.IsValid() && Document->TryGetObjectField(TEXT("Body"), Body) && (*Body)->TryGetArrayField(TEXT("Keys"), Keys))
	{
		for (const TSharedPtr<FJsonValue>& Key : *Keys)
		{
			const TSharedPtr<FJsonObject> Object = Key->AsObject();
			if (Object.IsValid() && Object->GetStringField(TEXT("Name")) == Name)
			{
				return Object->GetObjectField(TEXT("KeyTypeProperties"));
			}
		}
	}
	return nullptr;
}

FString Compact(const TSharedPtr<FJsonObject>& Object)
{
	if (!Object.IsValid())
	{
		return TEXT("<missing>");
	}
	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	return Text;
}
} // namespace AssetDocumentEditorCreateFailureTests

// A Blackboard Create with a Vector key failed (AIProvider.Blackboard.EntryMaterializationUnavailable)
// and its rollback left the asset behind (RollbackCreated.ResidueDetected, Atomic.Poisoned.*), so the path
// could not be used again. Vector/Rotator keys now create, extract, re-apply as a no-op and update.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentEditorBlackboardVectorKeysTest,
	"UEAssetDoc.AI.Blackboard.VectorRotatorKeysDocument",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEditorBlackboardVectorKeysTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorCreateFailureTests;
	const FTarget Target(TEXT("BB_Enemy"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	// The trial's failure mode: a key type rejects its KeyTypeProperties after it was staged in the new
	// Blackboard. The failed Create must leave nothing (the staged key type kept the asset alive) and
	// the path must stay usable.
	{
		const FString Broken = BlackboardDocument(Target, TEXT("10")).Replace(TEXT(R"("KeyTypeProperties": {}},)"),
			TEXT(R"("KeyTypeProperties": {}}, {"Name": "Ammo", "Category": "", "Description": "", "bInstanceSynced": false,
			"KeyTypeClass": {"Kind": "ClassRef", "Path": "/Script/AIModule.BlackboardKeyType_Int"}, "KeyTypeProperties": {}},)"));
		TestNotEqual(TEXT("the broken document differs"), Broken, BlackboardDocument(Target, TEXT("10")));
		const FAssetDocumentResult Failed = ApplyText(*this, Target, Broken, TEXT("failed create"));
		Report(*this, TEXT("failed create"), Failed);
		TestFalse(TEXT("an incomplete Int key fails the Create"), Failed.IsSuccess());
		TestFalse(TEXT("failed create: no poisoning"), HasCodePrefix(Failed, TEXT("Atomic.Poisoned")));
		TestFalse(TEXT("failed create: no rollback residue"), HasCodePrefix(Failed, TEXT("RollbackCreated.")));
		TestEqual(TEXT("failed create: nothing is left"), Target.Residue(), FString());
	}
	const FAssetDocumentResult Created = ApplyText(*this, Target, BlackboardDocument(Target, TEXT("10")), TEXT("create"));
	if (!TestTrue(TEXT("the Blackboard with Vector and Rotator keys is created"), Created.IsSuccess()))
	{
		Report(*this, TEXT("create"), Created);
		return true;
	}
	const TSharedPtr<FJsonObject> Extracted = ExtractDocument(*this, Target, TEXT("create"));
	TestEqual(TEXT("an empty Vector key has no default"), Compact(KeyProperties(Extracted, TEXT("PatrolLocation"))),
		FString(TEXT(R"({"DefaultValue":null})")));
	TestEqual(TEXT("a Vector default reads back"), Compact(KeyProperties(Extracted, TEXT("HomeLocation"))),
		FString(TEXT(R"({"DefaultValue":{"X":10.0,"Y":-20.0,"Z":30.5}})")));
	TestEqual(TEXT("a partial Rotator default is completed"), Compact(KeyProperties(Extracted, TEXT("Facing"))),
		FString(TEXT(R"({"DefaultValue":{"Pitch":0.0,"Roll":0.0,"Yaw":90.0}})")));

	const FAssetDocumentResult Again = ApplyText(*this, Target, BlackboardDocument(Target, TEXT("10")), TEXT("re-apply"));
	if (!TestTrue(TEXT("re-applying the same document succeeds"), Again.IsSuccess()))
	{
		Report(*this, TEXT("re-apply"), Again);
	}
	if (Extracted.IsValid())
	{
		const TSharedRef<FJsonObject> Roundtrip = MakeShared<FJsonObject>();
		Roundtrip->Values = Extracted->Values;
		const FAssetDocumentResult Extract = ApplyText(*this, Target, Serialize(Roundtrip), TEXT("apply extract"));
		if (!TestTrue(TEXT("applying the extract succeeds"), Extract.IsSuccess()))
		{
			Report(*this, TEXT("apply extract"), Extract);
		}
	}
	const FAssetDocumentResult Updated = ApplyText(*this, Target, BlackboardDocument(Target, TEXT("-5")), TEXT("update"));
	if (!TestTrue(TEXT("changing a Vector default applies"), Updated.IsSuccess()))
	{
		Report(*this, TEXT("update"), Updated);
		return true;
	}
	TestEqual(TEXT("the changed Vector default reads back"),
		Compact(KeyProperties(ExtractDocument(*this, Target, TEXT("update")), TEXT("HomeLocation"))),
		FString(TEXT(R"({"DefaultValue":{"X":-5.0,"Y":-20.0,"Z":30.5}})")));
	return true;
}

namespace AssetDocumentEditorCreateFailureTests
{
FString ParentReference(const FTarget& Parent)
{
	return Parent.ObjectPath.IsEmpty() ? FString(TEXT("null"))
		: FString::Printf(TEXT(R"({"Kind": "AssetRef", "Path": "%s"})"), *Parent.ObjectPath);
}

// A parent Blackboard with an Object key and an instance-synced Bool key.
FString ParentBlackboardDocument(const FTarget& Target)
{
	return FString::Printf(TEXT(R"json({"SchemaVersion": 1, "Target": "%s", "Class": "/Script/AIModule.BlackboardData",
		"Action": "CreateOrUpdate", "Definitions": {}, "Properties": {}, "Body": {"Parent": null, "Keys": [
		{"Name": "TargetActor", "Category": "Smoke", "Description": "Inherited target actor", "bInstanceSynced": false,
		 "KeyTypeClass": {"Kind": "ClassRef", "Path": "/Script/AIModule.BlackboardKeyType_Object"},
		 "KeyTypeProperties": {"BaseClass": {"Kind": "ClassRef", "Path": "/Script/Engine.Actor"}, "DefaultValue": null}},
		{"Name": "HasTarget", "Category": "Smoke", "Description": "Inherited target gate", "bInstanceSynced": true,
		 "KeyTypeClass": {"Kind": "ClassRef", "Path": "/Script/AIModule.BlackboardKeyType_Bool"},
		 "KeyTypeProperties": {"bDefaultValue": false}}]}})json"), *Target.PackageName);
}

// A Blackboard with a parent and one key of each kind with a value: Vector, instance-synced Name, Float, Class, Enum.
FString ChildBlackboardDocument(const FTarget& Target, const FTarget& Parent)
{
	return FString::Printf(TEXT(R"json({"SchemaVersion": 1, "Target": "%s", "Class": "/Script/AIModule.BlackboardData",
		"Action": "CreateOrUpdate", "Definitions": {}, "Properties": {}, "Body": {"Parent": %s, "Keys": [
		{"Name": "MoveLocation", "Category": "Smoke", "Description": "Local move destination", "bInstanceSynced": false,
		 "KeyTypeClass": {"Kind": "ClassRef", "Path": "/Script/AIModule.BlackboardKeyType_Vector"},
		 "KeyTypeProperties": {"DefaultValue": {"X": 10, "Y": -20, "Z": 30}}},
		{"Name": "AlertName", "Category": "Smoke", "Description": "Local alert label", "bInstanceSynced": true,
		 "KeyTypeClass": {"Kind": "ClassRef", "Path": "/Script/AIModule.BlackboardKeyType_Name"},
		 "KeyTypeProperties": {"DefaultValue": "Alert"}},
		{"Name": "Speed", "Category": "Smoke", "Description": "Local movement speed", "bInstanceSynced": false,
		 "KeyTypeClass": {"Kind": "ClassRef", "Path": "/Script/AIModule.BlackboardKeyType_Float"},
		 "KeyTypeProperties": {"DefaultValue": 3.5}},
		{"Name": "TargetClass", "Category": "Smoke", "Description": "Local actor class", "bInstanceSynced": false,
		 "KeyTypeClass": {"Kind": "ClassRef", "Path": "/Script/AIModule.BlackboardKeyType_Class"},
		 "KeyTypeProperties": {"BaseClass": {"Kind": "ClassRef", "Path": "/Script/Engine.Actor"},
		 "DefaultValue": {"Kind": "ClassRef", "Path": "/Script/Engine.Actor"}}},
		{"Name": "Mode", "Category": "Smoke", "Description": "Local native enum", "bInstanceSynced": false,
		 "KeyTypeClass": {"Kind": "ClassRef", "Path": "/Script/AIModule.BlackboardKeyType_Enum"},
		 "KeyTypeProperties": {"DefaultValue": "Set", "EnumName": "/Script/AIModule.EBasicKeyOperation",
		 "EnumType": {"Kind": "AssetRef", "Path": "/Script/AIModule.EBasicKeyOperation"}}}]}})json"),
		*Target.PackageName, *ParentReference(Parent));
}

// The names of a Blackboard's entries in one of its FBlackboardEntry arrays (Keys: its own, ParentKeys: inherited).
TArray<FString> BlackboardEntryNames(const FTarget& Target, const TCHAR* ArrayName)
{
	TArray<FString> Names;
	const UObject* Blackboard = FSoftObjectPath(Target.ObjectPath).ResolveObject();
	const FArrayProperty* Array = Blackboard != nullptr ? FindFProperty<FArrayProperty>(Blackboard->GetClass(), ArrayName) : nullptr;
	const FStructProperty* Entry = Array != nullptr ? CastField<FStructProperty>(Array->Inner) : nullptr;
	const FNameProperty* EntryName = Entry != nullptr ? FindFProperty<FNameProperty>(Entry->Struct, TEXT("EntryName")) : nullptr;
	if (EntryName == nullptr)
	{
		Names.Add(TEXT("<unreadable>"));
		return Names;
	}
	FScriptArrayHelper Helper(Array, Array->ContainerPtrToValuePtr<void>(Blackboard));
	for (int32 Index = 0; Index < Helper.Num(); ++Index)
	{
		Names.Add(EntryName->GetPropertyValue_InContainer(Helper.GetRawPtr(Index)).ToString());
	}
	return Names;
}

FString BodyField(const TSharedPtr<FJsonObject>& Document, const TCHAR* Field)
{
	const TSharedPtr<FJsonObject>* Body = nullptr;
	if (!Document.IsValid() || !Document->TryGetObjectField(TEXT("Body"), Body))
	{
		return TEXT("<missing>");
	}
	const TSharedPtr<FJsonValue> Value = (*Body)->TryGetField(Field);
	if (!Value.IsValid())
	{
		return TEXT("<missing>");
	}
	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	FJsonSerializer::Serialize(Value, FString(), Writer);
	return Text;
}
} // namespace AssetDocumentEditorCreateFailureTests

// A Blackboard with a Parent, extracted and applied to a new Target, is created as a copy: validate and apply agree,
// the copy has the same Parent and its own keys, and inherits the parent's keys.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentEditorBlackboardParentCopyTest,
	"UEAssetDoc.AI.Blackboard.ParentCopyDocument",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEditorBlackboardParentCopyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorCreateFailureTests;
	const FTarget Parent(TEXT("BB_Parent"));
	const FTarget Source(TEXT("BB_Main"));
	const FTarget Copy(TEXT("BB_MainCopy"));
	ON_SCOPE_EXIT { Copy.Cleanup(); Source.Cleanup(); Parent.Cleanup(); };
	const FAssetDocumentResult ParentCreated = ApplyText(*this, Parent, ParentBlackboardDocument(Parent), TEXT("parent"));
	const FAssetDocumentResult SourceCreated = ApplyText(*this, Source, ChildBlackboardDocument(Source, Parent), TEXT("source"));
	Report(*this, TEXT("parent"), ParentCreated);
	Report(*this, TEXT("source"), SourceCreated);
	if (!TestTrue(TEXT("the parent Blackboard is created"), ParentCreated.IsSuccess())
		|| !TestTrue(TEXT("the Blackboard with a Parent is created"), SourceCreated.IsSuccess()))
	{
		return true;
	}
	const TSharedPtr<FJsonObject> Extracted = ExtractDocument(*this, Source, TEXT("source"));
	if (!Extracted.IsValid())
	{
		return true;
	}
	const TSharedRef<FJsonObject> CopyDocument = MakeShared<FJsonObject>();
	CopyDocument->Values = Extracted->Values;
	CopyDocument->SetStringField(TEXT("Target"), Copy.PackageName);

	FAssetDocumentValidateRequest Validate;
	Validate.Document = CopyDocument;
	const FAssetDocumentResult Validated = Service().Validate(Validate);
	Report(*this, TEXT("validate copy"), Validated);
	TestTrue(TEXT("validate accepts the copy"), Validated.IsSuccess());
	TestTrue(TEXT("validate says the apply creates the copy"), Validated.Message.Contains(TEXT("would create")));

	const FAssetDocumentResult Created = ApplyText(*this, Copy, Serialize(CopyDocument), TEXT("copy"));
	Report(*this, TEXT("copy"), Created);
	if (!TestTrue(TEXT("applying the extract to a new Target creates the copy"), Created.IsSuccess()))
	{
		return true;
	}
	const TSharedPtr<FJsonObject> CopyExtract = ExtractDocument(*this, Copy, TEXT("copy"));
	TestEqual(TEXT("the copy has the same Parent"), BodyField(CopyExtract, TEXT("Parent")), BodyField(Extracted, TEXT("Parent")));
	TestEqual(TEXT("the copy has the same own keys"), BodyField(CopyExtract, TEXT("Keys")), BodyField(Extracted, TEXT("Keys")));
	TestEqual(TEXT("the copy's own keys"), FString::Join(BlackboardEntryNames(Copy, TEXT("Keys")), TEXT(",")),
		FString(TEXT("MoveLocation,AlertName,Speed,TargetClass,Mode")));
	TestEqual(TEXT("the copy inherits the parent's keys"), FString::Join(BlackboardEntryNames(Copy, TEXT("ParentKeys")), TEXT(",")),
		FString::Join(BlackboardEntryNames(Parent, TEXT("Keys")), TEXT(",")));

	const FAssetDocumentResult Again = ApplyText(*this, Copy, Serialize(CopyDocument), TEXT("copy again"));
	if (!TestTrue(TEXT("re-applying the copy succeeds"), Again.IsSuccess()))
	{
		Report(*this, TEXT("copy again"), Again);
	}
	return true;
}

// The CreateFailureLeavesNothing check for Creates with content written by profile primitives (a failed
// content Create must also leave the path reusable): the target fails after its content was applied.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentEditorContentCreateFailureTest,
	"UEAssetDoc.GenericRuntime.Rollback.ContentCreateFailureLeavesNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEditorContentCreateFailureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorCreateFailureTests;
	ON_SCOPE_EXIT { FAssetDocumentRuntimeTestHooks::Reset(); };
	const TArray<FFailurePoint> Points = {
		{TEXT("Persist"), EAssetDocumentRuntimeRequestPhase::Persist, nullptr, false},
		{TEXT("InstallPackageHeader"), {}, TEXT("InstallPackageHeader"), false},
		{TEXT("Verify"), EAssetDocumentRuntimeRequestPhase::Verify, nullptr, false, true},
	};
	struct FContent
	{
		const TCHAR* Label;
		TFunction<FString(const FTarget&)> Document;
	};
	const TArray<FContent> Contents = {
		{TEXT("Blackboard"), [](const FTarget& Target) { return BlackboardDocument(Target, TEXT("10")); }},
		{TEXT("Enum"), [](const FTarget& Target)
		{
			return FString::Printf(TEXT(R"json({"SchemaVersion": 1, "Target": "%s", "Class": "/Script/Engine.UserDefinedEnum",
				"Action": "Create", "Definitions": {}, "Body": {"Enumerators": [
				{"Name": "Idle", "Tooltip": "Waiting"}, {"Name": "Chase"}]}})json"), *Target.PackageName);
		}},
		{TEXT("Blueprint"), [](const FTarget& Target)
		{
			return FString::Printf(TEXT(R"json({"SchemaVersion": 1, "Target": "%s", "Class": "/Script/Engine.Blueprint",
				"Action": "Create", "Definitions": {}, "Properties": {"BlueprintDescription": "Enemy"},
				"Body": {"ParentClass": "/Script/Engine.Actor", "Variables": [{"Name": "Health", "Type": "float", "DefaultValue": 100.0}],
				"UbergraphPages": [{"Name": "EventGraph", "Nodes": [{"Id": "Begin", "Action": "K2:BeginPlay"},
				{"Id": "Print", "Action": "K2:PrintString"}],
				"Links": [{"From": "Begin:then", "To": "Print:execute"}]}]}})json"), *Target.PackageName);
		}},
	};
	for (const FContent& Content : Contents)
	{
		for (const FFailurePoint& Point : Points)
		{
			const FString Stage = FString::Printf(TEXT("%s at %s"), Content.Label, Point.Name);
			const FTarget Target(FString::Printf(TEXT("%s_%s"), Content.Label, Point.Name));
			ON_SCOPE_EXIT { Target.Cleanup(); };
			const FString Json = Content.Document(Target);
			if (Point.Phase.IsSet())
			{
				FAssetDocumentRuntimeTestHooks::FailNextRequestAtPhase(Point.Phase.GetValue());
			}
			else
			{
				FAssetDocumentRuntimeTestHooks::FailNextPersistenceAtEvent(Point.Event);
			}
			const FAssetDocumentResult Failed = ApplyText(*this, Target, Json, Stage);
			FAssetDocumentRuntimeTestHooks::Reset();
			Report(*this, Stage, Failed);
			TestFalse(*FString::Printf(TEXT("%s: the apply fails"), *Stage), Failed.IsSuccess());
			TestFalse(*FString::Printf(TEXT("%s: no poisoning"), *Stage), HasCodePrefix(Failed, TEXT("Atomic.Poisoned")));
			TestFalse(*FString::Printf(TEXT("%s: no rollback residue"), *Stage),
				HasCodePrefix(Failed, TEXT("RollbackCreated.")) || HasCodePrefix(Failed, TEXT("CutoverRollback.")));
			TestEqual(*FString::Printf(TEXT("%s: nothing is left"), *Stage), Target.Residue(), FString());
			const FAssetDocumentResult Retried = ApplyText(*this, Target, Json, Stage + TEXT(" retry"));
			if (!TestTrue(*FString::Printf(TEXT("%s: the same Create then applies"), *Stage), Retried.IsSuccess()))
			{
				Report(*this, Stage + TEXT(" retry"), Retried);
			}
		}
	}
	return true;
}
#endif
