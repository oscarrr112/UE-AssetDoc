// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Blueprint documents as an AI writes them, through the production Service:
// - UBlueprint.Authoring.ConversionLink: an int output linked into Branch's bool Condition (the editor joins
//   them only through a conversion node) is refused by validate and by apply with Graph.ConnectNeedsConversion
//   at the link, naming the conversion node's action and the two links through it in document terms; the
//   document written as the message says applies.
// - UBlueprint.Authoring.InstancedClassDefaults: a camera shake's RootShakePattern listing a default value
//   (Scale 1.0) next to a changed one validates, applies, diffs clean and re-applies (sparse over the instanced
//   object's class defaults, as extract writes it).
// - UBlueprint.Authoring.FunctionRenameDiff: a function renamed in the document keeping its GraphGuid diffs as
//   "renamed from <old name>" (not as a missing and an unexpected function) and applies as that rename.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentUBlueprintAuthoringTests
{
const TCHAR* Folder = TEXT("AssetDocumentUBlueprintAuthoring");

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

	explicit FTarget(const TCHAR* Prefix)
		: AssetName(FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12)))
	{
		PackageName = FString::Printf(TEXT("/Game/%s/%s"), Folder, *AssetName);
		ObjectPath = PackageName + TEXT(".") + AssetName;
		PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		SidecarPath = FPaths::Combine(FPaths::ProjectContentDir(), Folder, AssetName + TEXT(".assetdoc.json"));
	}

	UBlueprint* Get() const { return Cast<UBlueprint>(FSoftObjectPath(ObjectPath).ResolveObject()); }

	void Cleanup() const
	{
		if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting && !GEditor->IsTransactionActive())
		{
			GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentUBlueprintAuthoringTests", "Cleanup", "Blueprint authoring test cleanup"));
		}
		if (UBlueprint* Blueprint = Get())
		{
			if (IFileManager::Get().FileExists(*PackageFile))
			{
				UPackageTools::UnloadPackages({Blueprint->GetOutermost()});
			}
			else
			{
				Blueprint->GetOutermost()->ClearDirtyFlag();
				ObjectTools::DeleteObjects({Blueprint}, false, ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
			}
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		if (IFileManager::Get().FileExists(*PackageFile))
		{
			IFileManager::Get().Delete(*PackageFile, false, true);
			IAssetRegistry::GetChecked().ScanModifiedAssetFiles({PackageFile});
		}
		IFileManager::Get().Delete(*SidecarPath, false, true);
	}
};

TSharedPtr<FJsonObject> Parse(const FString& Text)
{
	TSharedPtr<FJsonObject> Object;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object);
	return Object;
}

FString Serialize(const TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	if (Object.IsValid())
	{
		const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}
	return Text;
}

FString Summary(const FAssetDocumentResult& Result)
{
	TArray<FString> Lines;
	Lines.Add(Result.Message);
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Lines.Add(FString::Printf(TEXT("[%s] %s %s"), *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
	}
	return FString::Join(Lines, TEXT("\n  "));
}

const FAssetDocumentDiagnostic* FindDiagnostic(const FAssetDocumentResult& Result, const TCHAR* Code)
{
	return Result.Diagnostics.FindByPredicate([Code](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == Code; });
}

// Target, Class and Action around a Body (JSON text).
FString Document(const FTarget& Target, const TCHAR* Action, const FString& Body)
{
	return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"%s\",\"Body\":%s}"),
		*Target.PackageName, Action, *Body);
}

FAssetDocumentResult Validate(const FString& Json)
{
	FAssetDocumentValidateRequest Request;
	Request.Document = Parse(Json);
	return Service().Validate(Request);
}

FAssetDocumentResult Apply(const FTarget& Target, const FString& Json)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target.SidecarPath), true);
	FFileHelper::SaveStringToFile(Json, *Target.SidecarPath);
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Target.SidecarPath;
	Request.bSaveAsset = true;
	return Service().ApplyFile(Request);
}

FAssetDocumentResult Diff(const FString& Json)
{
	FAssetDocumentDiffRequest Request;
	Request.Document = Parse(Json);
	return Service().Diff(Request);
}

TSharedPtr<FJsonObject> Extract(FAutomationTestBase& Test, const FTarget& Target, const TCHAR* Stage)
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = Target.ObjectPath;
	const FAssetDocumentResult Result = Service().Extract(Request);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: extract succeeds (%s)"), Stage, *Summary(Result)), Result.IsSuccess() && Result.Payload.IsValid()))
	{
		return nullptr;
	}
	return Result.Payload;
}

bool Applies(FAutomationTestBase& Test, const FTarget& Target, const FString& Json, const TCHAR* Stage)
{
	const FAssetDocumentResult Validated = Validate(Json);
	const bool bValid = Test.TestTrue(*FString::Printf(TEXT("%s: validate passes (%s)"), Stage, *Summary(Validated)), Validated.IsSuccess());
	const FAssetDocumentResult Applied = Apply(Target, Json);
	return Test.TestTrue(*FString::Printf(TEXT("%s: apply succeeds (%s)"), Stage, *Summary(Applied)), Applied.IsSuccess()) && bValid;
}

TSharedPtr<FJsonObject> GraphNamed(const TSharedPtr<FJsonObject>& Body, const TCHAR* Section, const TCHAR* Name)
{
	const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
	if (Body.IsValid() && Body->TryGetArrayField(Section, Graphs))
	{
		for (const TSharedPtr<FJsonValue>& Graph : *Graphs)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			FString GraphName;
			if (Graph->TryGetObject(Object) && (*Object)->TryGetStringField(TEXT("Name"), GraphName) && GraphName == Name)
			{
				return *Object;
			}
		}
	}
	return nullptr;
}
} // namespace AssetDocumentUBlueprintAuthoringTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUBlueprintAuthoringConversionLinkTest,
	"UEAssetDoc.UBlueprint.Authoring.ConversionLink",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintAuthoringConversionLinkTest::RunTest(const FString&)
{
	using namespace AssetDocumentUBlueprintAuthoringTests;
	const FTarget Target(TEXT("BP_ConversionLink"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	// Extra nodes and links (each with a leading comma) of the function "Check": Entry -> Branch, and Rand.
	auto Body = [](const FString& Nodes, const FString& Links)
	{
		return FString(TEXT("{\"ParentClass\":\"/Script/Engine.Actor\",\"FunctionGraphs\":[{\"Name\":\"Check\",\"Nodes\":["))
			+ TEXT("{\"Id\":\"Branch\",\"Action\":\"K2:Branch\"},")
			+ TEXT("{\"Id\":\"Rand\",\"Action\":\"K2:CallFunction:/Script/Engine.KismetMathLibrary:RandomInteger\"}") + Nodes
			+ TEXT("],\"Links\":[{\"From\":\"Entry:then\",\"To\":\"Branch:execute\"}") + Links + TEXT("]}]}");
	};
	if (!Applies(*this, Target, Document(Target, TEXT("Create"), Body(TEXT(""), TEXT(""))), TEXT("create")))
	{
		return false;
	}
	const FString Before = Serialize(Extract(*this, Target, TEXT("before")));

	// The trial's mistake: an int output into Branch's bool Condition.
	const FString Wrong = Document(Target, TEXT("Update"), Body(TEXT(""), TEXT(",{\"From\":\"Rand:ReturnValue\",\"To\":\"Branch:Condition\"}")));
	const FString Node = TEXT("{\"Id\": \"Conv_IntToBool\", \"Action\": \"K2:CallFunction:/Script/Engine.KismetMathLibrary:Conv_IntToBool\"}");
	const FString Links = TEXT("{\"From\": \"Rand:ReturnValue\", \"To\": \"Conv_IntToBool:InInt\"} and {\"From\": \"Conv_IntToBool:ReturnValue\", \"To\": \"Branch:Condition\"}");
	for (const bool bApply : {false, true})
	{
		const TCHAR* Stage = bApply ? TEXT("apply") : TEXT("validate");
		const FAssetDocumentResult Result = bApply ? Apply(Target, Wrong) : Validate(Wrong);
		const FAssetDocumentDiagnostic* Diagnostic = FindDiagnostic(Result, TEXT("Graph.ConnectNeedsConversion"));
		TestFalse(*FString::Printf(TEXT("%s refuses the link"), Stage), Result.IsSuccess());
		if (!TestNotNull(*FString::Printf(TEXT("%s says Graph.ConnectNeedsConversion (%s)"), Stage, *Summary(Result)), Diagnostic))
		{
			continue;
		}
		TestEqual(*FString::Printf(TEXT("%s: at the link"), Stage), Diagnostic->Path, FString(TEXT("/Body/FunctionGraphs/0/Links/1")));
		TestTrue(*FString::Printf(TEXT("%s: names the conversion node (%s)"), Stage, *Diagnostic->Message), Diagnostic->Message.Contains(Node));
		TestTrue(*FString::Printf(TEXT("%s: names the links through it"), Stage), Diagnostic->Message.Contains(Links));
		TestTrue(*FString::Printf(TEXT("%s: names the pins and their types"), Stage),
			Diagnostic->Message.Contains(TEXT("Rand:ReturnValue (Integer) -> Branch:Condition (Boolean)")));
		TestFalse(*FString::Printf(TEXT("%s: no internal argument"), Stage), Diagnostic->Message.Contains(TEXT("allow_conversion_node")));
	}
	TestEqual(TEXT("the refused apply changed nothing"), Serialize(Extract(*this, Target, TEXT("after refusal"))), Before);

	// Written as the message says, it validates and applies.
	const FString Right = Document(Target, TEXT("Update"), Body(TEXT(",") + Node,
		TEXT(",{\"From\":\"Rand:ReturnValue\",\"To\":\"Conv_IntToBool:InInt\"},{\"From\":\"Conv_IntToBool:ReturnValue\",\"To\":\"Branch:Condition\"}")));
	if (Applies(*this, Target, Right, TEXT("through the conversion node")))
	{
		const FAssetDocumentResult Clean = Diff(Right);
		const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
		TestTrue(*FString::Printf(TEXT("the Blueprint is the document (%s)"), *Serialize(Clean.Payload)), Clean.IsSuccess()
			&& Clean.Payload.IsValid() && Clean.Payload->TryGetArrayField(TEXT("changed"), Changed) && Changed->Num() == 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUBlueprintAuthoringInstancedClassDefaultsTest,
	"UEAssetDoc.UBlueprint.Authoring.InstancedClassDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintAuthoringInstancedClassDefaultsTest::RunTest(const FString&)
{
	using namespace AssetDocumentUBlueprintAuthoringTests;
	const FTarget Target(TEXT("BP_SmallShake"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	// Scale 1.0 is the pattern's default; PlayRate is not.
	const FString Json = Document(Target, TEXT("Create"), TEXT("{\"ParentClass\":\"/Script/Engine.CameraShakeBase\",\"ClassDefaults\":{\"RootShakePattern\":")
		TEXT("{\"Class\":\"/Script/TemplateSequence.SequenceCameraShakePattern\",\"Properties\":{\"PlayRate\":2.0,\"Scale\":1.0}}}}"));
	if (!Applies(*this, Target, Json, TEXT("create")))
	{
		return false;
	}
	const FAssetDocumentResult Clean = Diff(Json);
	const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
	TestTrue(*FString::Printf(TEXT("diff lists no change (%s)"), *Serialize(Clean.Payload)), Clean.IsSuccess() && Clean.Payload.IsValid()
		&& Clean.Payload->TryGetArrayField(TEXT("changed"), Changed) && Changed->Num() == 0);
	const TSharedPtr<FJsonObject> Extracted = Extract(*this, Target, TEXT("after create"));
	const FString Text = Serialize(Extracted);
	TestTrue(*FString::Printf(TEXT("extract keeps the changed value (%s)"), *Text), Text.Contains(TEXT("\"PlayRate\":2")));
	TestFalse(TEXT("extract leaves the default out"), Text.Contains(TEXT("\"Scale\"")));
	const FString Update = Json.Replace(TEXT("\"Action\":\"Create\""), TEXT("\"Action\":\"Update\""));
	Applies(*this, Target, Update, TEXT("re-apply"));
	TestEqual(TEXT("the re-apply changed nothing"), Serialize(Extract(*this, Target, TEXT("after re-apply"))), Text);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUBlueprintAuthoringFunctionRenameDiffTest,
	"UEAssetDoc.UBlueprint.Authoring.FunctionRenameDiff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintAuthoringFunctionRenameDiffTest::RunTest(const FString&)
{
	using namespace AssetDocumentUBlueprintAuthoringTests;
	const FTarget Target(TEXT("BP_RenameDiff"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	if (!Applies(*this, Target, Document(Target, TEXT("Create"), TEXT("{\"ParentClass\":\"/Script/Engine.Actor\",\"FunctionGraphs\":[")
		TEXT("{\"Name\":\"OpenDoor\",\"Nodes\":[{\"Id\":\"Print\",\"Action\":\"K2:PrintString\"}],\"Links\":[{\"From\":\"Entry:then\",\"To\":\"Print:execute\"}]}]}")),
		TEXT("create")))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Extracted = Extract(*this, Target, TEXT("before rename"));
	const TSharedPtr<FJsonObject> Function = GraphNamed(Extracted.IsValid() ? Extracted->GetObjectField(TEXT("Body")) : nullptr,
		TEXT("FunctionGraphs"), TEXT("OpenDoor"));
	FString Guid;
	if (!TestTrue(TEXT("extract writes the function's GraphGuid"), Function.IsValid() && Function->TryGetStringField(TEXT("GraphGuid"), Guid)))
	{
		return false;
	}
	Extracted->SetStringField(TEXT("Action"), TEXT("Update"));
	Function->SetStringField(TEXT("Name"), TEXT("SwingDoor"));
	const FString Renamed = Serialize(Extracted);

	const FAssetDocumentResult Changes = Diff(Renamed);
	const FString Text = Serialize(Changes.Payload);
	TestTrue(*FString::Printf(TEXT("diff succeeds (%s)"), *Summary(Changes)), Changes.IsSuccess());
	TestTrue(*FString::Printf(TEXT("diff shows the rename (%s)"), *Text), Text.Contains(TEXT("\"SwingDoor\":\"renamed from OpenDoor\"")));
	TestFalse(TEXT("diff does not list the old function as unexpected"), Text.Contains(TEXT("unexpected")));
	TestFalse(TEXT("diff does not list the new function as missing"), Text.Contains(TEXT("missing")));

	if (Applies(*this, Target, Renamed, TEXT("rename")))
	{
		const TSharedPtr<FJsonObject> After = Extract(*this, Target, TEXT("after rename"));
		const TSharedPtr<FJsonObject> Swing = GraphNamed(After.IsValid() ? After->GetObjectField(TEXT("Body")) : nullptr,
			TEXT("FunctionGraphs"), TEXT("SwingDoor"));
		FString KeptGuid;
		TestTrue(TEXT("the function keeps its GraphGuid"), Swing.IsValid() && Swing->TryGetStringField(TEXT("GraphGuid"), KeptGuid) && KeptGuid == Guid);
		TestFalse(TEXT("the old name is gone"), GraphNamed(After.IsValid() ? After->GetObjectField(TEXT("Body")) : nullptr,
			TEXT("FunctionGraphs"), TEXT("OpenDoor")).IsValid());
	}
	return true;
}
#endif
