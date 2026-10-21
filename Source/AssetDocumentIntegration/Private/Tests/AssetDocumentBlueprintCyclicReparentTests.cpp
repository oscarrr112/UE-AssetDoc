// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// KI-1 on the production path: reparenting both sides of a Blueprint cycle. The fixture is a component cycle: a component
// Blueprint BPC_X whose function casts its owner to the Actor Blueprint BP_A and reads BP_A.Comp, and BP_A, which holds
// Comp of class BPC_X as an SCS component. Each side is applied through the Service, once failing after its edits (a
// Text pin given a DefaultValue fails the post-apply check: the apply mutates, compiles and rolls back), then
// successfully: BPC_X reparented to BPC_New, BP_A reparented to BP_New with a new variable. Verify reads the staged copy
// as saved (not compiled on load): the apply succeeds, both Blueprints are compiled and saved as the editor leaves
// them, and the rollback leaves them as before.

#if WITH_DEV_AUTOMATION_TESTS
#include "Tests/AssetDocumentSkeletonTestHarness.h"
#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "PackageTools.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UObjectIterator.h"

namespace AssetDocumentBlueprintCyclicReparentTestsLocal
{
using AssetDocumentSkeletonTests::Bytes;
using AssetDocumentSkeletonTests::PackageFile;

struct FCycleAsset
{
	FString Name;
	FString PackageName;
	FString ObjectPath;
	FString ClassPath;
	FString Sidecar;

	FCycleAsset(const FString& Root, const TCHAR* InName)
		: Name(InName)
		, PackageName(Root / InName)
		, ObjectPath(PackageName + TEXT(".") + InName)
		, ClassPath(PackageName + TEXT(".") + InName + TEXT("_C"))
		, Sidecar(FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(PackageName, TEXT(".assetdoc.json"))))
	{
	}

	UBlueprint* Get() const { return Cast<UBlueprint>(FSoftObjectPath(ObjectPath).ResolveObject()); }
};

struct FCycle
{
	FAutomationTestBase& Test;
	const FString Root = TEXT("/Game/AssetDocumentKI1/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	// Component side: BPC_Old <- BPC_New, BPC_X (parent BPC_Old). Actor side: BP_Old <- BP_New, BP_A (parent BP_Old).
	const FCycleAsset ComponentOld{Root, TEXT("BPC_Old")};
	const FCycleAsset ComponentNew{Root, TEXT("BPC_New")};
	const FCycleAsset X{Root, TEXT("BPC_X")};
	const FCycleAsset ActorOld{Root, TEXT("BP_Old")};
	const FCycleAsset ActorNew{Root, TEXT("BP_New")};
	const FCycleAsset A{Root, TEXT("BP_A")};

	explicit FCycle(FAutomationTestBase& InTest) : Test(InTest) {}

	TArray<const FCycleAsset*> All() const { return {&A, &X, &ActorNew, &ActorOld, &ComponentNew, &ComponentOld}; }

	~FCycle()
	{
		TArray<UPackage*> Packages;
		for (const FCycleAsset* Asset : All())
		{
			if (UPackage* Package = FindPackage(nullptr, *Asset->PackageName))
			{
				Package->SetDirtyFlag(false);
				Packages.Add(Package);
			}
		}
		if (!Packages.IsEmpty())
		{
			UPackageTools::UnloadPackages(Packages);
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		TArray<FString> Files;
		for (const FCycleAsset* Asset : All())
		{
			const FString File = FPaths::ConvertRelativePathToFull(PackageFile(Asset->ObjectPath));
			IFileManager::Get().Delete(*File, false, true);
			IFileManager::Get().Delete(*Asset->Sidecar, false, true);
			Files.Add(File);
		}
		FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles(Files);
		IFileManager::Get().DeleteDirectory(*FPaths::GetPath(X.Sidecar), false, true);
	}

	FAssetDocumentResult Apply(const FCycleAsset& Asset, const FString& Json, const TCHAR* Stage) const
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Asset.Sidecar), true);
		FFileHelper::SaveStringToFile(Json, *Asset.Sidecar);
		FAssetDocumentApplyFileRequest Request;
		Request.FilePath = Asset.Sidecar;
		Request.bSaveAsset = true;
		const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
		for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
		{
			Test.AddInfo(FString::Printf(TEXT("%s: %s %s %s"), Stage, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
		}
		return Result;
	}

	bool ServiceApply(const FCycleAsset& Asset, const FString& Json, const TCHAR* Stage) const
	{
		return Test.TestTrue(*FString::Printf(TEXT("%s applies"), Stage), Apply(Asset, Json, Stage).IsSuccess());
	}

	// A node whose Text pin is given a DefaultValue: the post-apply check fails after the edits ran.
	static FString FailingEventGraph()
	{
		return TEXT(",\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":[{\"Id\":\"Txt\","
			"\"Action\":\"K2:CallFunction:/Script/Engine.KismetTextLibrary:Conv_TextToString\","
			"\"PinOverrides\":[{\"Pin\":\"InText\",\"DefaultValue\":\"boom\"}]}]}]");
	}

	static FString Document(const FCycleAsset& Asset, const TCHAR* Action, const FString& Body)
	{
		return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"%s\",\"Body\":{%s}}"),
			*Asset.PackageName, Action, *Body);
	}

	FString ComponentBody(bool bWithCast, const FCycleAsset* Parent = nullptr) const
	{
		const FString Function = !bWithCast ? FString() : FString::Printf(TEXT(
			",\"FunctionGraphs\":[{\"Name\":\"FindOwnComp\","
			"\"Signature\":{\"Outputs\":[{\"Name\":\"Found\",\"Type\":\"object:/Script/Engine.ActorComponent\"}]},"
			"\"Nodes\":["
			"{\"Id\":\"Owner\",\"Action\":\"K2:CallFunction:/Script/Engine.ActorComponent:GetOwner\"},"
			"{\"Id\":\"Cast\",\"Action\":\"K2:DynamicCast:%s\"},"
			"{\"Id\":\"GetComp\",\"Action\":\"K2:Get:%s:Comp\"}],"
			"\"Links\":["
			"{\"From\":\"Entry:then\",\"To\":\"Cast:execute\"},"
			"{\"From\":\"Owner:ReturnValue\",\"To\":\"Cast:Object\"},"
			"{\"From\":\"Cast:AsBP A\",\"To\":\"GetComp:self\"},"
			"{\"From\":\"Cast:then\",\"To\":\"Result:execute\"},"
			"{\"From\":\"GetComp:Comp\",\"To\":\"Result:Found\"}]}]"), *A.ClassPath, *A.ClassPath);
		return FString::Printf(TEXT("\"ParentClass\":\"%s\"%s"), *(Parent ? Parent : &ComponentOld)->ClassPath, *Function);
	}

	// Creates the six Blueprints and closes the cycle, all saved; unloads them and loads BP_A back from disk (an editor
	// restart), which loads BPC_X and the parents with it.
	bool Build()
	{
		if (!ServiceApply(ComponentOld, Document(ComponentOld, TEXT("Create"), TEXT("\"ParentClass\":\"/Script/Engine.ActorComponent\"")), TEXT("create BPC_Old"))
			|| !ServiceApply(ComponentNew, Document(ComponentNew, TEXT("Create"), FString::Printf(TEXT("\"ParentClass\":\"%s\""), *ComponentOld.ClassPath)), TEXT("create BPC_New"))
			|| !ServiceApply(ActorOld, Document(ActorOld, TEXT("Create"), TEXT("\"ParentClass\":\"/Script/Engine.Actor\"")), TEXT("create BP_Old"))
			|| !ServiceApply(ActorNew, Document(ActorNew, TEXT("Create"), FString::Printf(TEXT("\"ParentClass\":\"%s\""), *ActorOld.ClassPath)), TEXT("create BP_New"))
			|| !ServiceApply(X, Document(X, TEXT("Create"), ComponentBody(false)), TEXT("create BPC_X"))
			|| !ServiceApply(A, Document(A, TEXT("Create"), ActorBody(ActorOld, false)), TEXT("create BP_A"))
			|| !ServiceApply(X, Document(X, TEXT("Update"), ComponentBody(true)), TEXT("close the cycle")))
		{
			return false;
		}
		TArray<UPackage*> Packages;
		for (const FCycleAsset* Asset : All())
		{
			UBlueprint* Blueprint = Asset->Get();
			if (!Test.TestNotNull(*FString::Printf(TEXT("%s exists"), *Asset->Name), Blueprint)
				|| !Test.TestFalse(*FString::Printf(TEXT("%s is saved"), *Asset->Name), Blueprint->GetOutermost()->IsDirty()))
			{
				return false;
			}
			Packages.Add(Blueprint->GetOutermost());
		}
		UPackageTools::UnloadPackages(Packages);
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		FAssetRegistryModule::GetRegistry().SearchAllAssets(true);
		for (const FCycleAsset* Asset : All())
		{
			if (!LoadObject<UBlueprint>(nullptr, *Asset->ObjectPath))
			{
				Test.AddError(FString::Printf(TEXT("%s loads back from disk"), *Asset->Name));
				return false;
			}
		}
		return Check(TEXT("loaded from disk"), false, false);
	}

	// BP_A's body: the parent, Comp of class BPC_X and, with bVariable, one int32 variable.
	FString ActorBody(const FCycleAsset& Parent, bool bVariable) const
	{
		const FString Variable = !bVariable ? FString()
			: FString(TEXT(",\"Variables\":[{\"Name\":\"ComposedValue\",\"Type\":\"int\",\"Category\":\"Composed\",\"DefaultValue\":73}]"));
		return FString::Printf(TEXT("\"ParentClass\":\"%s\",\"Components\":[{\"Name\":\"Comp\",\"Class\":\"%s\"}]%s"),
			*Parent.ClassPath, *X.ClassPath, *Variable);
	}

	// Both sides of the cycle compiled (as the editor leaves them after a compile), saved, BP_A's Comp template on the
	// current BPC_X class; bXReparented / bAReparented: which applies took effect.
	bool Check(const TCHAR* Stage, bool bXReparented, bool bAReparented) const
	{
		bool bOk = true;
		for (const FCycleAsset* Asset : {&X, &A})
		{
			const UBlueprint* Blueprint = Asset->Get();
			if (!Test.TestNotNull(*FString::Printf(TEXT("%s: %s is loaded"), Stage, *Asset->Name), Blueprint))
			{
				return false;
			}
			bOk &= Test.TestTrue(*FString::Printf(TEXT("%s: %s compiles (status %d)"), Stage, *Asset->Name, static_cast<int32>(Blueprint->Status)),
				Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
			bOk &= Test.TestFalse(*FString::Printf(TEXT("%s: %s is saved"), Stage, *Asset->Name), Blueprint->GetOutermost()->IsDirty());
		}
		const UBlueprint* BlueprintX = X.Get();
		const UBlueprint* BlueprintA = A.Get();
		bOk &= Test.TestEqual(*FString::Printf(TEXT("%s: BPC_X parent"), Stage), GetPathNameSafe(BlueprintX->ParentClass),
			bXReparented ? ComponentNew.ClassPath : ComponentOld.ClassPath);
		bOk &= Test.TestEqual(*FString::Printf(TEXT("%s: BP_A parent"), Stage), GetPathNameSafe(BlueprintA->ParentClass),
			bAReparented ? ActorNew.ClassPath : ActorOld.ClassPath);
		bOk &= Test.TestEqual(*FString::Printf(TEXT("%s: BPC_X variables"), Stage), BlueprintX->NewVariables.Num(), 0);
		bOk &= Test.TestEqual(*FString::Printf(TEXT("%s: BP_A variables"), Stage), BlueprintA->NewVariables.Num(), bAReparented ? 1 : 0);
		const USCS_Node* Node = BlueprintA->SimpleConstructionScript ? BlueprintA->SimpleConstructionScript->FindSCSNode(TEXT("Comp")) : nullptr;
		const UClass* Current = BlueprintX->GeneratedClass;
		bOk &= Test.TestTrue(*FString::Printf(TEXT("%s: BP_A's Comp template is of the current BPC_X class"), Stage),
			Node != nullptr && Node->ComponentClass == Current && Node->ComponentTemplate != nullptr && Node->ComponentTemplate->GetClass() == Current);
		// Nothing of the staged copies stays loaded.
		for (TObjectIterator<UBlueprint> It; It; ++It)
		{
			if (It->GetOutermost()->GetName().StartsWith(TEXT("/Temp/")) && (It->GetName() == X.Name || It->GetName() == A.Name))
			{
				Test.AddInfo(FString::Printf(TEXT("%s: staged copy still loaded: %s"), Stage, *It->GetPathName()));
			}
		}
		return bOk;
	}

	// One Service apply of Target's update Body; bFailing adds the failing node, and the package must stay unchanged.
	bool ReparentApply(const FCycleAsset& Target, const FString& Body, bool bFailing, const TCHAR* Stage) const
	{
		const TArray<uint8> Before = Bytes(PackageFile(Target.ObjectPath));
		const FAssetDocumentResult Result = Apply(Target, Document(Target, TEXT("Update"), Body + (bFailing ? FailingEventGraph() : FString())), Stage);
		bool bOk = Test.TestEqual(*FString::Printf(TEXT("%s: apply result"), Stage), Result.IsSuccess(), !bFailing);
		bOk &= Test.TestEqual(*FString::Printf(TEXT("%s: package bytes %s"), Stage, bFailing ? TEXT("unchanged") : TEXT("changed")),
			Before == Bytes(PackageFile(Target.ObjectPath)), bFailing);
		return bOk;
	}
};
} // namespace AssetDocumentBlueprintCyclicReparentTestsLocal

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintCyclicReparentTest,
	"UEAssetDoc.Blueprint.CyclicReparent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBlueprintCyclicReparentTest::RunTest(const FString&)
{
	using namespace AssetDocumentBlueprintCyclicReparentTestsLocal;
	FCycle Cycle(*this);
	if (!Cycle.Build())
	{
		return false;
	}
	// The component side, failing after its edits, then applied.
	const FString ComponentUpdate = Cycle.ComponentBody(true, &Cycle.ComponentNew);
	Cycle.ReparentApply(Cycle.X, ComponentUpdate, true, TEXT("BPC_X failing apply"));
	Cycle.Check(TEXT("after the BPC_X rollback"), false, false);
	Cycle.ReparentApply(Cycle.X, ComponentUpdate, false, TEXT("BPC_X apply"));
	Cycle.Check(TEXT("after the BPC_X apply"), true, false);
	// The Actor side, failing, then applied.
	const FString ActorUpdate = Cycle.ActorBody(Cycle.ActorNew, true);
	Cycle.ReparentApply(Cycle.A, ActorUpdate, true, TEXT("BP_A failing apply"));
	Cycle.Check(TEXT("after the BP_A rollback"), true, false);
	Cycle.ReparentApply(Cycle.A, ActorUpdate, false, TEXT("BP_A apply"));
	Cycle.Check(TEXT("after the BP_A apply"), true, true);
	return !HasAnyErrors();
}
#endif
