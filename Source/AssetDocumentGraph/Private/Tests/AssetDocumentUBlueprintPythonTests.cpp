// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// End-to-end UBlueprint through the production Service and the Python Profile
// (profile.ublueprint.v2): one ApplyFile creates and populates an Actor Blueprint
// (variables, a function with parameters using Branch + PrintString + Multiply, a
// custom event, a component hierarchy with overridden properties, an interface,
// class defaults); Diff/Extract agree with the document; a second apply edits links,
// defaults and variables; re-applying the extraction changes nothing; failures in
// the middle of Apply and at compile roll back to a Blueprint that compiles and
// extracts identically; the saved package reloads from disk to the same document;
// a failed create leaves nothing behind.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Engine/InheritableComponentHandler.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "HAL/FileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
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
#include "UObject/Interface.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentUBlueprintPythonTests
{
const TCHAR* Folder = TEXT("/Game/AssetDocumentUBlueprintPython");

struct FTarget
{
	FString AssetName;
	FString PackageName;
	FString ObjectPath;
	FString PackageFile;
	FString SidecarPath;

	explicit FTarget(const TCHAR* Prefix)
		: AssetName(FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits)))
	{
		PackageName = FString(Folder) / AssetName;
		ObjectPath = PackageName + TEXT(".") + AssetName;
		PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		SidecarPath = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("AssetDocumentUBlueprintPython"), AssetName + TEXT(".assetdoc.json"));
	}

	UBlueprint* Get() const { return Cast<UBlueprint>(FSoftObjectPath(ObjectPath).ResolveObject()); }

	// Unloads (saved) or deletes (unsaved) the Blueprint and its generated class, then removes files.
	void Cleanup() const
	{
		if (UBlueprint* Blueprint = Get())
		{
			if (IFileManager::Get().FileExists(*PackageFile))
			{
				UPackageTools::UnloadPackages({Blueprint->GetOutermost()});
			}
			else
			{
				ObjectTools::DeleteObjects({Blueprint}, false, ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
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

TSharedPtr<FJsonObject> Parse(const FString& Json)
{
	TSharedPtr<FJsonObject> Object;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object);
	return Object;
}

void LogResult(FAutomationTestBase& Test, const TCHAR* Stage, const FAssetDocumentResult& Result)
{
	Test.AddInfo(FString::Printf(TEXT("%s: %s"), Stage, *Result.Message));
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Test.AddInfo(FString::Printf(TEXT("%s: [%s] %s %s"), Stage, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
	}
}

FAssetDocumentResult Apply(FAutomationTestBase& Test, const FTarget& Target, const FString& Json, const TCHAR* Stage)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target.SidecarPath), true);
	FFileHelper::SaveStringToFile(Json, *Target.SidecarPath);
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Target.SidecarPath;
	Request.bSaveAsset = true;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
	LogResult(Test, Stage, Result);
	return Result;
}

TSharedPtr<FJsonObject> Extract(FAutomationTestBase& Test, const FTarget& Target, const TCHAR* Stage)
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = Target.ObjectPath;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Extract(Request);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: extract succeeds"), Stage), Result.IsSuccess() && Result.Payload.IsValid()))
	{
		LogResult(Test, Stage, Result);
		return nullptr;
	}
	return Result.Payload;
}

// Service Diff of the authored document against the live asset reports no change.
void TestNoDiff(FAutomationTestBase& Test, const FString& Json, const TCHAR* Stage)
{
	FAssetDocumentDiffRequest Request;
	Request.Document = Parse(Json);
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Diff(Request);
	const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
	const bool bOk = Result.IsSuccess() && Result.Payload.IsValid() && Result.Payload->TryGetArrayField(TEXT("changed"), Changed);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: diff succeeds"), Stage), bOk))
	{
		LogResult(Test, Stage, Result);
		return;
	}
	if (!Test.TestEqual(*FString::Printf(TEXT("%s: live asset equals the document"), Stage), Changed->Num(), 0))
	{
		Test.AddInfo(Serialize(Result.Payload));
	}
}

bool CompilesClean(const UBlueprint* Blueprint)
{
	return Blueprint != nullptr && (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
}

USCS_Node* FindNode(const UBlueprint* Blueprint, const TCHAR* Name)
{
	return Blueprint && Blueprint->SimpleConstructionScript ? Blueprint->SimpleConstructionScript->FindSCSNode(Name) : nullptr;
}

bool SavePackage(UObject* Asset)
{
	UPackage* Package = Asset->GetOutermost();
	const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	return UPackage::SavePackage(Package, Asset, *File, Args);
}

// A saved, empty Blueprint Interface the document implements.
UBlueprint* CreateInterface(const FTarget& Target)
{
	UPackage* Package = CreatePackage(*Target.PackageName);
	UBlueprint* Interface = FKismetEditorUtilities::CreateBlueprint(UInterface::StaticClass(), Package, FName(*Target.AssetName),
		BPTYPE_Interface, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (Interface == nullptr)
	{
		return nullptr;
	}
	FAssetRegistryModule::AssetCreated(Interface);
	FKismetEditorUtilities::CompileBlueprint(Interface);
	return SavePackage(Interface) ? Interface : nullptr;
}

FString Document(const FTarget& Target, const TCHAR* Action, const FString& Interface, const FString& Body)
{
	return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"%s\",")
		TEXT("\"Properties\":{\"BlueprintDescription\":\"Door made by AssetDocument\"},")
		TEXT("\"Body\":{\"ParentClass\":\"/Script/Engine.Actor\",\"ImplementedInterfaces\":[\"%s\"],%s}}"),
		*Target.PackageName, Action, *Interface, *Body);
}

// Variables, a function with parameters (Branch + PrintString + Multiply), a custom event,
// a component hierarchy with overridden properties and class defaults.
FString InitialBody(double Health, double Factor, bool bSecondVariable, const TCHAR* ReturnFrom)
{
	return FString::Printf(TEXT(
		"\"Variables\":["
		"{\"Name\":\"Health\",\"Type\":\"float\",\"DefaultValue\":%g,\"Category\":\"Stats\",\"InstanceEditable\":true},"
		"{\"Name\":\"bArmed\",\"Type\":\"bool\",\"DefaultValue\":true}%s],"
		"\"Components\":["
		"{\"Name\":\"Pivot\",\"Class\":\"/Script/Engine.SceneComponent\"},"
		"{\"Name\":\"Mesh\",\"Class\":\"/Script/Engine.StaticMeshComponent\",\"AttachTo\":\"Pivot\",\"Properties\":{\"bVisible\":false}},"
		"{\"Name\":\"Lamp\",\"Class\":\"/Script/Engine.PointLightComponent\",\"AttachTo\":\"Mesh\",\"Properties\":{\"Intensity\":1234.5}}],"
		"\"ClassDefaults\":{\"InitialLifeSpan\":7.5},"
		"\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":["
		"{\"Id\":\"OnHit\",\"Action\":\"K2:CustomEvent\",\"Name\":\"OnHit\"},"
		"{\"Id\":\"SayHit\",\"Action\":\"K2:PrintString\",\"PinOverrides\":[{\"Pin\":\"InString\",\"DefaultValue\":\"hit\"}]}],"
		"\"Links\":[{\"From\":\"OnHit:then\",\"To\":\"SayHit:execute\"}]}],"
		"\"FunctionGraphs\":[{\"Name\":\"ComputeDamage\","
		"\"Signature\":{\"Inputs\":[{\"Name\":\"Base\",\"Type\":\"float\"}],\"Outputs\":[{\"Name\":\"Damage\",\"Type\":\"float\"}]},"
		"\"Nodes\":["
		"{\"Id\":\"Armed\",\"Action\":\"K2:Get:bArmed\"},"
		"{\"Id\":\"Check\",\"Action\":\"K2:Branch\"},"
		"{\"Id\":\"Say\",\"Action\":\"K2:PrintString\",\"PinOverrides\":[{\"Pin\":\"InString\",\"DefaultValue\":\"armed\"}]},"
		"{\"Id\":\"Scale\",\"Action\":\"K2:Multiply\",\"PinOverrides\":[{\"Pin\":\"B\",\"DefaultValue\":\"%g\"}]}],"
		"\"Links\":["
		"{\"From\":\"Entry:then\",\"To\":\"Check:execute\"},"
		"{\"From\":\"Armed:bArmed\",\"To\":\"Check:Condition\"},"
		"{\"From\":\"Check:then\",\"To\":\"Say:execute\"},"
		"{\"From\":\"%s\",\"To\":\"Result:execute\"},"
		"{\"From\":\"Entry:Base\",\"To\":\"Scale:A\"},"
		"{\"From\":\"Scale:ReturnValue\",\"To\":\"Result:Damage\"}]}]"),
		Health, bSecondVariable ? TEXT(",{\"Name\":\"Count\",\"Type\":\"int\",\"DefaultValue\":3}") : TEXT(""), Factor, ReturnFrom);
}

bool ExtractEquals(FAutomationTestBase& Test, const FTarget& Target, const FString& Expected, const TCHAR* Stage)
{
	const TSharedPtr<FJsonObject> After = Extract(Test, Target, Stage);
	const FString Actual = Serialize(After);
	if (!Test.TestEqual(*FString::Printf(TEXT("%s: extraction is unchanged"), Stage), Actual, Expected))
	{
		Test.AddInfo(FString::Printf(TEXT("expected %s"), *Expected));
		Test.AddInfo(FString::Printf(TEXT("actual   %s"), *Actual));
		return false;
	}
	return true;
}
} // namespace AssetDocumentUBlueprintPythonTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUBlueprintPythonTest, "UEAssetDoc.UBlueprint.Python",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintPythonTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentUBlueprintPythonTests;
	const FTarget InterfaceTarget(TEXT("BPI_Use"));
	const FTarget Target(TEXT("BP_Door"));
	const FTarget FailedCreate(TEXT("BP_Failed"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
		FailedCreate.Cleanup();
		InterfaceTarget.Cleanup();
	};
	UBlueprint* Interface = CreateInterface(InterfaceTarget);
	if (!TestNotNull(TEXT("fixture interface is created and saved"), Interface))
	{
		return false;
	}
	const FString InterfaceClass = Interface->GeneratedClass->GetPathName();

	// 1. One apply creates and populates the Blueprint.
	const FString Created = Document(Target, TEXT("Create"), InterfaceClass, InitialBody(100.0, 2.0, true, TEXT("Say:then")));
	if (!TestTrue(TEXT("create applies"), Apply(*this, Target, Created, TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	UBlueprint* Blueprint = Target.Get();
	if (!TestNotNull(TEXT("Blueprint exists"), Blueprint))
	{
		return false;
	}
	TestTrue(TEXT("created Blueprint compiles"), CompilesClean(Blueprint));
	TestTrue(TEXT("package is saved"), IFileManager::Get().FileExists(*Target.PackageFile));
	TestEqual(TEXT("three variables"), Blueprint->NewVariables.Num(), 3);
	TestEqual(TEXT("interface implemented"), Blueprint->ImplementedInterfaces.Num(), 1);
	USCS_Node* Mesh = FindNode(Blueprint, TEXT("Mesh"));
	USCS_Node* Lamp = FindNode(Blueprint, TEXT("Lamp"));
	TestTrue(TEXT("Mesh is attached to Pivot"), Mesh != nullptr && Blueprint->SimpleConstructionScript->FindParentNode(Mesh) == FindNode(Blueprint, TEXT("Pivot")));
	TestTrue(TEXT("Lamp is attached to Mesh"), Lamp != nullptr && Blueprint->SimpleConstructionScript->FindParentNode(Lamp) == Mesh);
	UClass* Generated = Blueprint->GeneratedClass;
	TestTrue(TEXT("function compiled with parameters"), Generated && Generated->FindFunctionByName(TEXT("ComputeDamage"))
		&& FindFProperty<FProperty>(Generated->FindFunctionByName(TEXT("ComputeDamage")), TEXT("Base")));
	TestTrue(TEXT("custom event compiled"), Generated && Generated->FindFunctionByName(TEXT("OnHit")));
	TestTrue(TEXT("class default written"), Generated && FMath::IsNearlyEqual(Generated->GetDefaultObject<AActor>()->InitialLifeSpan, 7.5f));
	TestNoDiff(*this, Created, TEXT("after create"));

	// 2. Re-applying the extraction is a no-op.
	const TSharedPtr<FJsonObject> First = Extract(*this, Target, TEXT("first extract"));
	if (!First.IsValid())
	{
		return false;
	}
	First->SetStringField(TEXT("Action"), TEXT("Update"));
	const FString Extracted = Serialize(First);
	AddInfo(FString::Printf(TEXT("extracted: %s"), *Extracted));
	// A no-op: no recompile, no dirty package.
	TestFalse(TEXT("created package is saved clean"), Blueprint->GetOutermost()->IsDirty());
	const FDateTime SavedStamp = IFileManager::Get().GetTimeStamp(*Target.PackageFile);
	int32 Compiles = 0;
	const FDelegateHandle CompiledHandle = Blueprint->OnCompiled().AddLambda([&Compiles](UBlueprint*) { ++Compiles; });
	const FAssetDocumentResult Reapplied = Apply(*this, Target, Extracted, TEXT("Reapply"));
	Blueprint->OnCompiled().Remove(CompiledHandle);
	TestTrue(TEXT("extraction re-applies"), Reapplied.IsSuccess());
	TestEqual(TEXT("no-op re-apply does not recompile"), Compiles, 0);
	TestFalse(TEXT("no-op re-apply leaves the package clean"), Blueprint->GetOutermost()->IsDirty());
	TestFalse(TEXT("no-op re-apply reports no save"), Reapplied.bSavedAsset);
	TestEqual(TEXT("no-op re-apply does not rewrite the package file"), IFileManager::Get().GetTimeStamp(*Target.PackageFile), SavedStamp);
	First->SetStringField(TEXT("Action"), TEXT("CreateOrUpdate"));
	const FString Baseline = Serialize(First);
	ExtractEquals(*this, Target, Baseline, TEXT("after re-apply"));
	TestTrue(TEXT("same Blueprint object"), Target.Get() == Blueprint);

	// 2b. A layout-only edit of the extraction is not mistaken for a no-op.
	{
		const TSharedPtr<FJsonObject> Moved = Parse(Extracted);
		const TArray<TSharedPtr<FJsonValue>>& Pages = Moved->GetObjectField(TEXT("Body"))->GetArrayField(TEXT("UbergraphPages"));
		TSharedPtr<FJsonObject> Node;
		for (const TSharedPtr<FJsonValue>& Value : Pages[0]->AsObject()->GetArrayField(TEXT("Nodes")))
		{
			if (Value->AsObject()->HasField(TEXT("Position")) && Value->AsObject()->HasField(TEXT("NodeGuid")))
			{
				Node = Value->AsObject();
				break;
			}
		}
		if (TestTrue(TEXT("extracted EventGraph has a positioned node"), Node.IsValid()))
		{
			const TSharedPtr<FJsonObject> Position = Node->GetObjectField(TEXT("Position"));
			const int32 MovedX = static_cast<int32>(Position->GetNumberField(TEXT("X"))) + 160;
			Position->SetNumberField(TEXT("X"), MovedX);
			const FAssetDocumentResult MovedResult = Apply(*this, Target, Serialize(Moved), TEXT("Move"));
			TestTrue(TEXT("layout-only edit applies"), MovedResult.IsSuccess());
			TestTrue(TEXT("layout-only edit is saved"), MovedResult.bSavedAsset);
			FGuid NodeGuid;
			FGuid::Parse(Node->GetStringField(TEXT("NodeGuid")), NodeGuid);
			const UEdGraphNode* LiveNode = nullptr;
			for (const UEdGraph* Graph : Blueprint->UbergraphPages)
			{
				for (const UEdGraphNode* Candidate : Graph->Nodes)
				{
					LiveNode = Candidate && Candidate->NodeGuid == NodeGuid ? Candidate : LiveNode;
				}
			}
			TestTrue(TEXT("layout-only edit moved the node"), LiveNode != nullptr && LiveNode->NodePosX == MovedX);
		}
	}

	// 3. A second apply edits links, defaults and variables.
	const FString Edited = Document(Target, TEXT("Update"), InterfaceClass, InitialBody(150.0, 3.0, false, TEXT("Check:else")));
	if (TestTrue(TEXT("edit applies"), Apply(*this, Target, Edited, TEXT("Edit")).IsSuccess()))
	{
		TestNoDiff(*this, Edited, TEXT("after edit"));
		TestEqual(TEXT("Count was removed"), Blueprint->NewVariables.Num(), 2);
		TestTrue(TEXT("edited Blueprint compiles"), CompilesClean(Blueprint));
	}
	const TSharedPtr<FJsonObject> EditedExtract = Extract(*this, Target, TEXT("edited extract"));
	const FString EditedBaseline = Serialize(EditedExtract);

	// 4. Failure in the middle of Apply (variables already changed, then an invalid component).
	const FString MidApply = Document(Target, TEXT("Update"), InterfaceClass, TEXT(
		"\"Variables\":[{\"Name\":\"Shield\",\"Type\":\"int\",\"DefaultValue\":9}],"
		"\"Components\":[{\"Name\":\"Broken\",\"Class\":\"/Script/Engine.Actor\"}]"));
	TestFalse(TEXT("mid-apply failure fails"), Apply(*this, Target, MidApply, TEXT("MidApplyFailure")).IsSuccess());
	Blueprint = Target.Get();
	TestTrue(TEXT("rolled-back Blueprint compiles"), CompilesClean(Blueprint));
	TestTrue(TEXT("rolled-back package is clean"), Blueprint != nullptr && !Blueprint->GetOutermost()->IsDirty());
	ExtractEquals(*this, Target, EditedBaseline, TEXT("after mid-apply rollback"));

	// 5. Failure at compile: a ForEachLoop whose Array type is undetermined (editor compiler error).
	const FString CompileError = Document(Target, TEXT("Update"), InterfaceClass, TEXT(
		"\"Variables\":[{\"Name\":\"Health\",\"Type\":\"float\",\"DefaultValue\":1}],"
		"\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":["
		"{\"Id\":\"Begin\",\"Action\":\"K2:BeginPlay\"},"
		"{\"Id\":\"Loop\",\"Action\":\"K2:ForEachLoop\"}],"
		"\"Links\":[{\"From\":\"Begin:then\",\"To\":\"Loop:Exec\"}]}]"));
	const FAssetDocumentResult CompileFailed = Apply(*this, Target, CompileError, TEXT("CompileFailure"));
	TestFalse(TEXT("compile failure fails"), CompileFailed.IsSuccess());
	TestTrue(TEXT("failure is the compiler's"), CompileFailed.Diagnostics.ContainsByPredicate(
		[](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == TEXT("Graph.FinalizeFailed"); }));
	Blueprint = Target.Get();
	TestTrue(TEXT("Blueprint compiles after compile-failure rollback"), CompilesClean(Blueprint));
	ExtractEquals(*this, Target, EditedBaseline, TEXT("after compile rollback"));

	// 6. The saved package, unloaded and loaded back from disk, extracts to the same document.
	if (Blueprint != nullptr)
	{
		TestFalse(TEXT("package is clean before unload"), Blueprint->GetOutermost()->IsDirty());
		UPackageTools::UnloadPackages({Blueprint->GetOutermost()});
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		Blueprint = nullptr;
		TestNull(TEXT("Blueprint is unloaded"), Target.Get());
		ExtractEquals(*this, Target, EditedBaseline, TEXT("after reload from disk"));
		TestTrue(TEXT("Blueprint is loaded from disk again"), Target.Get() != nullptr);
		TestTrue(TEXT("reloaded Blueprint compiles"), CompilesClean(Target.Get()));
	}

	// 7. A failed create leaves no asset, package file or registry entry.
	const FString BadCreate = Document(FailedCreate, TEXT("Create"), InterfaceClass, TEXT(
		"\"Variables\":[{\"Name\":\"Health\",\"Type\":\"float\"}],"
		"\"Components\":[{\"Name\":\"Broken\",\"Class\":\"/Script/Engine.Actor\"}]"));
	TestFalse(TEXT("failed create fails"), Apply(*this, FailedCreate, BadCreate, TEXT("FailedCreate")).IsSuccess());
	TestNull(TEXT("no created Blueprint remains"), FailedCreate.Get());
	TestFalse(TEXT("no package file"), IFileManager::Get().FileExists(*FailedCreate.PackageFile));
	TestFalse(TEXT("no registry entry"),
		FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(FailedCreate.ObjectPath)).IsValid());
	return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUBlueprintPythonComponentOverridesTest,
	"UEAssetDoc.UBlueprint.PythonComponentOverrides",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// Inherited (parent Blueprint SCS) and native (C++ default subobject) component overrides
// through the Python Profile, including removing an override by omitting its row.
bool FAssetDocumentUBlueprintPythonComponentOverridesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentUBlueprintPythonTests;
	const FTarget Parent(TEXT("BP_Base"));
	const FTarget Child(TEXT("BP_Child"));
	const FTarget Pawn(TEXT("BP_Walker"));
	ON_SCOPE_EXIT
	{
		Child.Cleanup();
		Pawn.Cleanup();
		Parent.Cleanup();
	};
	auto Make = [](const FTarget& Target, const TCHAR* Action, const FString& ParentClass, const FString& Components)
	{
		return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"%s\",")
			TEXT("\"Body\":{\"ParentClass\":\"%s\",\"Components\":%s}}"), *Target.PackageName, Action, *ParentClass, *Components);
	};
	const FString ParentDoc = Make(Parent, TEXT("Create"), TEXT("/Script/Engine.Actor"),
		TEXT("[{\"Name\":\"BaseMesh\",\"Class\":\"/Script/Engine.StaticMeshComponent\"}]"));
	if (!TestTrue(TEXT("parent Blueprint is created"), Apply(*this, Parent, ParentDoc, TEXT("Parent")).IsSuccess()))
	{
		return false;
	}

	// Child: override an inherited component and attach an own component to it.
	const FString ChildDoc = Make(Child, TEXT("Create"), Parent.PackageName, TEXT(
		"[{\"Name\":\"BaseMesh\",\"Scope\":\"Inherited\",\"Properties\":{\"bVisible\":false}},"
		"{\"Name\":\"Extra\",\"Class\":\"/Script/Engine.SceneComponent\",\"AttachTo\":\"BaseMesh\"}]"));
	if (TestTrue(TEXT("child Blueprint is created"), Apply(*this, Child, ChildDoc, TEXT("Child")).IsSuccess()))
	{
		TestNoDiff(*this, ChildDoc, TEXT("child"));
		UBlueprint* Blueprint = Child.Get();
		UInheritableComponentHandler* Handler = Blueprint ? Blueprint->GetInheritableComponentHandler(false) : nullptr;
		USCS_Node* Inherited = Cast<UBlueprintGeneratedClass>(Parent.Get()->GeneratedClass)->SimpleConstructionScript->FindSCSNode(TEXT("BaseMesh"));
		const USceneComponent* Override = Handler && Inherited
			? Cast<USceneComponent>(Handler->GetOverridenComponentTemplate(FComponentKey(Inherited))) : nullptr;
		TestTrue(TEXT("inherited override template holds the value"), Override != nullptr && !Override->GetVisibleFlag());
		USCS_Node* Extra = FindNode(Blueprint, TEXT("Extra"));
		TestTrue(TEXT("own component is attached to the inherited one"), Extra != nullptr && Extra->ParentComponentOrVariableName == TEXT("BaseMesh"));
		// Omitting the inherited row reverts the override.
		const FString Reverted = Make(Child, TEXT("Update"), Parent.PackageName,
			TEXT("[{\"Name\":\"Extra\",\"Class\":\"/Script/Engine.SceneComponent\",\"AttachTo\":\"BaseMesh\"}]"));
		if (TestTrue(TEXT("override removal applies"), Apply(*this, Child, Reverted, TEXT("Revert")).IsSuccess()))
		{
			TestNoDiff(*this, Reverted, TEXT("reverted"));
			UBlueprint* AfterBlueprint = Child.Get();
			UInheritableComponentHandler* AfterHandler = AfterBlueprint ? AfterBlueprint->GetInheritableComponentHandler(false) : nullptr;
			const UActorComponent* After = AfterHandler ? AfterHandler->GetOverridenComponentTemplate(FComponentKey(Inherited)) : nullptr;
			TestTrue(TEXT("inherited override is gone"), After == nullptr || CastChecked<USceneComponent>(After)->GetVisibleFlag());
		}
	}

	// Native component of a C++ parent (Character.CharacterMovement).
	const FString PawnDoc = Make(Pawn, TEXT("Create"), TEXT("/Script/Engine.Character"),
		TEXT("[{\"Name\":\"CharacterMovement\",\"Scope\":\"Native\",\"Properties\":{\"MaxWalkSpeed\":321.0}}]"));
	if (TestTrue(TEXT("character Blueprint is created"), Apply(*this, Pawn, PawnDoc, TEXT("Native")).IsSuccess()))
	{
		TestNoDiff(*this, PawnDoc, TEXT("native"));
		const ACharacter* Defaults = Pawn.Get()->GeneratedClass->GetDefaultObject<ACharacter>();
		TestTrue(TEXT("native override is on the class defaults"), Defaults && Defaults->GetCharacterMovement()
			&& FMath::IsNearlyEqual(Defaults->GetCharacterMovement()->MaxWalkSpeed, 321.f));
		const TSharedPtr<FJsonObject> Extracted = Extract(*this, Pawn, TEXT("native extract"));
		AddInfo(FString::Printf(TEXT("native extract: %s"), *Serialize(Extracted)));
	}
	return !HasAnyErrors();
}
#endif
