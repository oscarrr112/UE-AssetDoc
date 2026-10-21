// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Applying a component Blueprint must leave the Actor Blueprints that hold it as an SCS
// component (and their children) exactly as an editor compile of the component Blueprint would:
// every component template on the current generated class (never REINST_ / TRASHCLASS_ / a
// transient class), no stale class-default snapshot in the dependents' extraction, and a spawned
// actor whose component is live and ticks.
//
// Fixture: component Blueprint BPC_X (Tick event; a function that casts its
// owner to BP_A and reads BP_A.Comp: the cycle), Actor Blueprint BP_A with SCS component Comp of
// class BPC_X, child BP_B of BP_A. Everything is saved, unloaded and loaded back from disk, as an
// editor restart would. Then:
//   (a) BPC_X applied twice (two pin-default edits), then the same document again (a no-op);
//   (b) BPC_X applied once, then BP_B;
//   NoCycle: (a) and (b) on a BPC_X without the cast function.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "PackageTools.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentBlueprintDependentTemplateTests
{
struct FAsset
{
	FString Name;
	FString PackageName;
	FString ObjectPath;
	FString ClassPath;
	FString PackageFile;
	FString SidecarPath;

	FAsset(const FString& Folder, const TCHAR* InName)
		: Name(InName)
	{
		PackageName = FString(TEXT("/Game/")) / Folder / Name;
		ObjectPath = PackageName + TEXT(".") + Name;
		ClassPath = PackageName + TEXT(".") + Name + TEXT("_C");
		PackageFile = FPaths::ConvertRelativePathToFull(
			FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()));
		SidecarPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectContentDir(), Folder, Name + TEXT(".assetdoc.json")));
	}

	UBlueprint* Get() const { return Cast<UBlueprint>(FSoftObjectPath(ObjectPath).ResolveObject()); }
	UBlueprint* Load() const { return LoadObject<UBlueprint>(nullptr, *ObjectPath); }
	UClass* Generated() const { const UBlueprint* Blueprint = Get(); return Blueprint ? Blueprint->GeneratedClass.Get() : nullptr; }
};

struct FFixture
{
	FAutomationTestBase& Test;
	const FString Folder;
	const FAsset X;
	const FAsset A;
	const FAsset B;
	bool bCycle = true;
	// Richer setup: BP_A is a Character whose graph calls into Comp, BP_B overrides the
	// inherited Comp.
	bool bRich = false;
	int32 PrintRevision = 1;
	int32 ChildRevision = 1;

	FFixture(FAutomationTestBase& InTest, bool bInCycle, bool bInRich = false)
		: Test(InTest)
		, Folder(FString(TEXT("AssetDocumentP63/")) + FGuid::NewGuid().ToString(EGuidFormats::Digits))
		, X(Folder, TEXT("BPC_X"))
		, A(Folder, TEXT("BP_A"))
		, B(Folder, TEXT("BP_B"))
		, bCycle(bInCycle)
		, bRich(bInRich)
	{
	}

	~FFixture()
	{
		TArray<UPackage*> Packages;
		for (const FAsset* Asset : {&B, &A, &X})
		{
			if (UPackage* Package = FindPackage(nullptr, *Asset->PackageName))
			{
				Packages.Add(Package);
			}
		}
		if (!Packages.IsEmpty())
		{
			for (UPackage* Package : Packages)
			{
				Package->SetDirtyFlag(false);
			}
			UPackageTools::UnloadPackages(Packages);
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		TArray<FString> Files;
		for (const FAsset* Asset : {&B, &A, &X})
		{
			IFileManager::Get().Delete(*Asset->PackageFile, false, true);
			IFileManager::Get().Delete(*Asset->SidecarPath, false, true);
			Files.Add(Asset->PackageFile);
		}
		FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles(Files);
		IFileManager::Get().DeleteDirectory(*FPaths::GetPath(X.SidecarPath), false, true);
	}

	// ---- documents ----------------------------------------------------------------------------

	FString ComponentDocument(const TCHAR* Action, bool bWithCast, bool bFailing = false) const
	{
		// A Text pin given a DefaultValue fails the post-apply check: the apply mutates and
		// compiles, then rolls back.
		const FString Failing = !bFailing ? FString() : FString(TEXT(
			",{\"Id\":\"Txt\",\"Action\":\"K2:CallFunction:/Script/Engine.KismetTextLibrary:Conv_TextToString\","
			"\"PinOverrides\":[{\"Pin\":\"InText\",\"DefaultValue\":\"boom\"}]}"));
		// Tick increments TickCount and prints (the pin default the edits change). The cast
		// function reads BP_A.Comp through the owner cast to BP_A (the cycle).
		const FString Tick = FString::Printf(TEXT(
			"{\"Name\":\"EventGraph\",\"Nodes\":["
			"{\"Id\":\"Tick\",\"Action\":\"K2:Event:/Script/Engine.ActorComponent:ReceiveTick\"},"
			"{\"Id\":\"GetCount\",\"Action\":\"K2:Get:TickCount\"},"
			"{\"Id\":\"Inc\",\"Action\":\"K2:CallFunction:/Script/Engine.KismetMathLibrary:Add_IntInt\",\"PinOverrides\":[{\"Pin\":\"B\",\"DefaultValue\":\"1\"}]},"
			"{\"Id\":\"SetCount\",\"Action\":\"K2:Set:TickCount\"},"
			"{\"Id\":\"Say\",\"Action\":\"K2:PrintString\",\"PinOverrides\":[{\"Pin\":\"InString\",\"DefaultValue\":\"tick-%d\"},"
			"{\"Pin\":\"bPrintToScreen\",\"DefaultValue\":\"false\"},{\"Pin\":\"bPrintToLog\",\"DefaultValue\":\"false\"}]}%s],"
			"\"Links\":["
			"{\"From\":\"Tick:then\",\"To\":\"SetCount:execute\"},"
			"{\"From\":\"GetCount:TickCount\",\"To\":\"Inc:A\"},"
			"{\"From\":\"Inc:ReturnValue\",\"To\":\"SetCount:TickCount\"},"
			"{\"From\":\"SetCount:then\",\"To\":\"Say:execute\"}]}"), PrintRevision, *Failing);
		const FString Functions = !bWithCast ? FString() : FString::Printf(TEXT(
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
		return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"%s\","
			"\"Body\":{\"ParentClass\":\"/Script/Engine.ActorComponent\","
			"\"Variables\":[{\"Name\":\"TickCount\",\"Type\":\"int\",\"DefaultValue\":0}],"
			"\"UbergraphPages\":[%s]%s}}"), *X.PackageName, Action, *Tick, *Functions);
	}

	FString ActorDocument(const TCHAR* Action, bool bWithGraph = false) const
	{
		// BeginPlay calls Comp.FindOwnComp (BP_A's graph depends on BPC_X's function).
		const FString Graph = !bWithGraph ? FString() : FString::Printf(TEXT(
			",\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":["
			"{\"Id\":\"Begin\",\"Action\":\"K2:Event:/Script/Engine.Actor:ReceiveBeginPlay\"},"
			"{\"Id\":\"GetComp\",\"Action\":\"K2:Get:Comp\"},"
			"{\"Id\":\"Call\",\"Action\":\"K2:CallFunction:%s:FindOwnComp\"}],"
			"\"Links\":["
			"{\"From\":\"Begin:then\",\"To\":\"Call:execute\"},"
			"{\"From\":\"GetComp:Comp\",\"To\":\"Call:self\"}]}]"), *X.ClassPath);
		return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"%s\","
			"\"Body\":{\"ParentClass\":\"%s\",\"Components\":[{\"Name\":\"Comp\",\"Class\":\"%s\"}]%s}}"),
			*A.PackageName, Action, bRich ? TEXT("/Script/Engine.Character") : TEXT("/Script/Engine.Actor"), *X.ClassPath, *Graph);
	}

	FString ChildDocument(const TCHAR* Action) const
	{
		const FString Override = !bRich ? FString()
			: FString(TEXT(",\"Components\":[{\"Name\":\"Comp\",\"Scope\":\"Inherited\",\"Properties\":{\"TickCount\":5}}]"));
		return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"%s\","
			"\"Body\":{\"ParentClass\":\"%s\",\"Variables\":[{\"Name\":\"Extra\",\"Type\":\"int\",\"DefaultValue\":%d}]%s}}"),
			*B.PackageName, Action, *A.ClassPath, ChildRevision, *Override);
	}

	// ---- service ------------------------------------------------------------------------------

	static bool HasCode(const FAssetDocumentResult& Result, const TCHAR* Code)
	{
		return Result.Diagnostics.ContainsByPredicate([Code](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == Code; });
	}

	// bExpectRepair: the safety net must find (and repair) a stale template; otherwise the apply
	// itself must leave none to find.
	FAssetDocumentResult Apply(const FAsset& Asset, const FString& Json, const FString& Stage, bool bExpectRepair = false) const
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Asset.SidecarPath), true);
		FFileHelper::SaveStringToFile(Json, *Asset.SidecarPath);
		FAssetDocumentApplyFileRequest Request;
		Request.FilePath = Asset.SidecarPath;
		Request.bSaveAsset = true;
		const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
		Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *Result.Message));
		for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
		{
			Test.AddInfo(FString::Printf(TEXT("%s: [%s] %s %s %s"), *Stage, *Diagnostic.Severity, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
		}
		Test.TestFalse(*FString::Printf(TEXT("%s: no unrepairable stale template"), *Stage), HasCode(Result, TEXT("Graph.DependentTemplateStale")));
		Test.TestEqual(*FString::Printf(TEXT("%s: stale template repaired by the safety net"), *Stage),
			HasCode(Result, TEXT("Graph.DependentTemplateRepaired")), bExpectRepair);
		return Result;
	}

	bool ApplyOk(const FAsset& Asset, const FString& Json, const FString& Stage, bool bExpectRepair = false) const
	{
		const FAssetDocumentResult Result = Apply(Asset, Json, Stage, bExpectRepair);
		return Test.TestTrue(*FString::Printf(TEXT("%s: applies"), *Stage), Result.IsSuccess());
	}

	// Creates the three Blueprints (and the cycle), all saved, then unloads them and loads them
	// back from disk (an editor restart).
	bool Build()
	{
		if (!ApplyOk(X, ComponentDocument(TEXT("Create"), false), TEXT("create BPC_X"))
			|| !ApplyOk(A, ActorDocument(TEXT("Create")), TEXT("create BP_A"))
			|| !ApplyOk(B, ChildDocument(TEXT("Create")), TEXT("create BP_B")))
		{
			return false;
		}
		if (bCycle && !ApplyOk(X, ComponentDocument(TEXT("Update"), true), TEXT("close the cycle")))
		{
			return false;
		}
		if (bRich && bCycle && !ApplyOk(A, ActorDocument(TEXT("Update"), true), TEXT("BP_A calls into Comp")))
		{
			return false;
		}
		TArray<UPackage*> Packages;
		for (const FAsset* Asset : {&B, &A, &X})
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
		for (const FAsset* Asset : {&B, &A, &X})
		{
			if (!Test.TestNull(*FString::Printf(TEXT("%s is unloaded"), *Asset->Name), Asset->Get()))
			{
				return false;
			}
		}
		return Test.TestNotNull(TEXT("BP_B loads back from disk"), B.Load())
			&& Test.TestNotNull(TEXT("BP_A is loaded"), A.Get()) && Test.TestNotNull(TEXT("BPC_X is loaded"), X.Get());
	}

	// ---- checks -------------------------------------------------------------------------------

	static bool IsStaleClass(const UClass* Class, const UClass* Current)
	{
		if (Class == nullptr)
		{
			return true;
		}
		const FString Name = Class->GetName();
		return Class != Current || Class->HasAnyClassFlags(CLASS_NewerVersionExists)
			|| Name.StartsWith(TEXT("REINST_")) || Name.StartsWith(TEXT("TRASHCLASS_")) || Name.StartsWith(TEXT("SKEL_"))
			|| Class->GetOutermost() == GetTransientPackage();
	}

	static FString Describe(const UObject* Object)
	{
		return Object ? FString::Printf(TEXT("%s (class %s)"), *Object->GetPathName(), *GetPathNameSafe(Object->GetClass())) : TEXT("<null>");
	}

	void CheckTemplates(const FString& Stage) const
	{
		UClass* Current = X.Generated();
		if (!Test.TestNotNull(*FString::Printf(TEXT("%s: BPC_X has a generated class"), *Stage), Current))
		{
			return;
		}
		Test.TestFalse(*FString::Printf(TEXT("%s: BPC_X generated class is current"), *Stage), IsStaleClass(Current, Current));
		// Diagnostics: every live object of an outdated BPC_X class anywhere.
		for (TObjectIterator<UActorComponent> It; It; ++It)
		{
			if (It->GetClass()->GetName().Contains(TEXT("BPC_X")) && IsStaleClass(It->GetClass(), Current) && !It->HasAnyFlags(RF_ClassDefaultObject))
			{
				Test.AddInfo(FString::Printf(TEXT("%s: outdated-class component %s valid=%d"), *Stage, *Describe(*It), IsValid(*It) ? 1 : 0));
			}
		}
		// The SCS template of BP_A (also the template its generated class spawns from).
		const UBlueprint* BlueprintA = A.Get();
		USCS_Node* Node = BlueprintA && BlueprintA->SimpleConstructionScript ? BlueprintA->SimpleConstructionScript->FindSCSNode(TEXT("Comp")) : nullptr;
		if (Test.TestNotNull(*FString::Printf(TEXT("%s: BP_A has SCS node Comp"), *Stage), Node))
		{
			const UActorComponent* Template = Node->ComponentTemplate;
			if (!Test.TestTrue(*FString::Printf(TEXT("%s: BP_A Comp template class is the current BPC_X class"), *Stage),
				Template != nullptr && Node->ComponentClass == Current && !IsStaleClass(Template->GetClass(), Current)))
			{
				Test.AddInfo(FString::Printf(TEXT("%s: template %s, node class %s, current %s"), *Stage, *Describe(Template),
					*GetPathNameSafe(Node->ComponentClass), *GetPathNameSafe(Current)));
			}
		}
		// Every object in the dependents' packages that is an instance of an outdated class.
		for (const FAsset* Asset : {&A, &B})
		{
			const UBlueprint* Blueprint = Asset->Get();
			if (!Test.TestNotNull(*FString::Printf(TEXT("%s: %s is loaded"), *Stage, *Asset->Name), Blueprint))
			{
				continue;
			}
			TArray<FString> Stale;
			ForEachObjectWithPackage(Blueprint->GetOutermost(), [&Stale, Current](UObject* Object)
			{
				if (Object->IsA<UActorComponent>() && Object->GetClass()->GetName().Contains(TEXT("BPC_X"))
					&& IsStaleClass(Object->GetClass(), Current) && IsValid(Object) && !Object->HasAnyFlags(RF_ClassDefaultObject))
				{
					Stale.Add(Describe(Object));
				}
				return true;
			});
			if (const UInheritableComponentHandler* Handler = const_cast<UBlueprint*>(Blueprint)->GetInheritableComponentHandler(false))
			{
				TArray<UActorComponent*> Templates;
				Handler->GetAllTemplates(Templates);
				for (const UActorComponent* Template : Templates)
				{
					if (Template && Template->GetClass()->GetName().Contains(TEXT("BPC_X")) && IsStaleClass(Template->GetClass(), Current))
					{
						Stale.AddUnique(Describe(Template));
					}
				}
			}
			if (!Test.TestEqual(*FString::Printf(TEXT("%s: %s holds no BPC_X object of an outdated class"), *Stage, *Asset->Name), Stale.Num(), 0))
			{
				for (const FString& Entry : Stale)
				{
					Test.AddInfo(FString::Printf(TEXT("%s: stale %s"), *Stage, *Entry));
				}
			}
		}
	}

	void CheckExtract(const FString& Stage) const
	{
		FAssetDocumentExtractRequest Request;
		Request.AssetPath = A.ObjectPath;
		const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Extract(Request);
		const TSharedPtr<FJsonObject>* Body = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
		if (!Test.TestTrue(*FString::Printf(TEXT("%s: extract BP_A"), *Stage), Result.IsSuccess() && Result.Payload.IsValid()
			&& Result.Payload->TryGetObjectField(TEXT("Body"), Body) && (*Body)->TryGetArrayField(TEXT("Components"), Components)))
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Components)
		{
			const TSharedPtr<FJsonObject> Component = Value->AsObject();
			if (Component.IsValid() && Component->GetStringField(TEXT("Name")) == TEXT("Comp"))
			{
				FString Text;
				const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
				FJsonSerializer::Serialize(Component.ToSharedRef(), Writer);
				if (!Test.TestFalse(*FString::Printf(TEXT("%s: extract BP_A shows no Properties on Comp"), *Stage), Component->HasField(TEXT("Properties"))))
				{
					Test.AddInfo(FString::Printf(TEXT("%s: Comp = %s"), *Stage, *Text));
				}
				Test.TestEqual(*FString::Printf(TEXT("%s: extract BP_A Comp class"), *Stage), Component->GetStringField(TEXT("Class")), X.ClassPath);
				return;
			}
		}
		Test.AddError(FString::Printf(TEXT("%s: extract BP_A has no Comp"), *Stage));
	}

	// Spawns Asset in a game world that has begun play and ticks it: the Comp variable is set,
	// of the current class, and its Tick event runs.
	void CheckSpawn(const FAsset& Asset, const FString& Stage) const
	{
		UClass* ActorClass = Asset.Generated();
		UClass* Current = X.Generated();
		if (!Test.TestTrue(*FString::Printf(TEXT("%s: %s has a generated actor class"), *Stage, *Asset.Name), ActorClass && ActorClass->IsChildOf<AActor>()))
		{
			return;
		}
		const UWorld::InitializationValues Values = UWorld::InitializationValues().CreatePhysicsScene(false).ShouldSimulatePhysics(false)
			.CreateNavigation(false).CreateAISystem(false).AllowAudioPlayback(false);
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AssetDocumentP63World"), nullptr, true, ERHIFeatureLevel::Num, &Values);
		if (!Test.TestNotNull(*FString::Printf(TEXT("%s: game world"), *Stage), World))
		{
			return;
		}
		FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
		Context.SetCurrentWorld(World);
		ON_SCOPE_EXIT
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			World->RemoveFromRoot();
		};
		World->InitializeActorsForPlay(FURL());
		World->SetBegunPlay(true);
		FActorSpawnParameters Parameters;
		Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AActor* Actor = World->SpawnActor<AActor>(ActorClass, FTransform::Identity, Parameters);
		if (!Test.TestNotNull(*FString::Printf(TEXT("%s: %s spawns"), *Stage, *Asset.Name), Actor))
		{
			return;
		}
		const FObjectProperty* CompProperty = FindFProperty<FObjectProperty>(ActorClass, TEXT("Comp"));
		UActorComponent* Comp = CompProperty ? Cast<UActorComponent>(CompProperty->GetObjectPropertyValue_InContainer(Actor)) : nullptr;
		if (!Test.TestNotNull(*FString::Printf(TEXT("%s: spawned %s has its Comp variable set"), *Stage, *Asset.Name), Comp))
		{
			return;
		}
		if (!Test.TestFalse(*FString::Printf(TEXT("%s: spawned %s Comp is of the current BPC_X class"), *Stage, *Asset.Name), IsStaleClass(Comp->GetClass(), Current)))
		{
			Test.AddInfo(FString::Printf(TEXT("%s: spawned Comp %s"), *Stage, *Describe(Comp)));
			return;
		}
		Test.TestTrue(*FString::Printf(TEXT("%s: spawned %s Comp can tick"), *Stage, *Asset.Name), Comp->PrimaryComponentTick.bCanEverTick);
		for (int32 Frame = 0; Frame < 3; ++Frame)
		{
			World->Tick(LEVELTICK_All, 1.f / 30.f);
		}
		const FIntProperty* CountProperty = FindFProperty<FIntProperty>(Comp->GetClass(), TEXT("TickCount"));
		const int32 Count = CountProperty ? CountProperty->GetPropertyValue_InContainer(Comp) : -1;
		Test.TestTrue(*FString::Printf(TEXT("%s: spawned %s Comp ticks (TickCount %d)"), *Stage, *Asset.Name, Count), Count > 0);
	}

	void CheckAll(const FString& Stage) const
	{
		CheckTemplates(Stage);
		CheckExtract(Stage);
		CheckSpawn(A, Stage);
		CheckSpawn(B, Stage);
		for (const FAsset* Asset : {&X, &A, &B})
		{
			const UBlueprint* Blueprint = Asset->Get();
			Test.TestTrue(*FString::Printf(TEXT("%s: %s compiles"), *Stage, *Asset->Name),
				Blueprint && (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings));
		}
	}

	// ---- sequences ----------------------------------------------------------------------------

	bool ApplyComponentEdit(const FString& Stage)
	{
		++PrintRevision;
		const bool bOk = ApplyOk(X, ComponentDocument(TEXT("Update"), bCycle), Stage);
		CheckAll(Stage);
		return bOk;
	}

	// (a) The same component Blueprint applied twice, then the same document once more (the
	// sidecar watcher and an explicit apply of one document): the repeat is a no-op.
	void RunRepeatedComponentApply()
	{
		if (!ApplyComponentEdit(TEXT("(a) first BPC_X apply")) || !ApplyComponentEdit(TEXT("(a) second BPC_X apply")))
		{
			return;
		}
		const FAssetDocumentResult Repeat = Apply(X, ComponentDocument(TEXT("Update"), bCycle), TEXT("(a) repeated BPC_X apply"));
		Test.TestTrue(TEXT("(a) repeated apply of the unchanged document succeeds"), Repeat.IsSuccess());
		Test.TestFalse(TEXT("(a) repeated apply of the unchanged document saves nothing"), Repeat.bSavedAsset);
		CheckAll(TEXT("(a) repeated BPC_X apply"));
	}

	// (c) An apply of the component Blueprint that fails after its edit compiled: the rollback
	// restores the component Blueprint and the dependents the compile updated.
	void RunFailedComponentApply()
	{
		++PrintRevision;
		const FAssetDocumentResult Failed = Apply(X, ComponentDocument(TEXT("Update"), bCycle, true), TEXT("(c) failing BPC_X apply"));
		Test.TestFalse(TEXT("(c) the failing BPC_X apply fails"), Failed.IsSuccess());
		--PrintRevision;
		CheckAll(TEXT("(c) failing BPC_X apply"));
		ApplyComponentEdit(TEXT("(c) BPC_X apply after the failure"));
	}

	// (d) Safety net: BP_A's Comp template is put back on the class a compile of BPC_X replaced
	// (the state the dependent restore left before the fix). The next Blueprint apply reinstances
	// it, as the editor's compile would have, and says so.
	void RunStaleTemplateRepair()
	{
		UBlueprint* BlueprintA = A.Get();
		USCS_Node* Node = BlueprintA && BlueprintA->SimpleConstructionScript ? BlueprintA->SimpleConstructionScript->FindSCSNode(TEXT("Comp")) : nullptr;
		UBlueprint* BlueprintX = X.Get();
		if (!Test.TestNotNull(TEXT("(d) BP_A has SCS node Comp"), Node) || !Test.TestNotNull(TEXT("(d) BPC_X is loaded"), BlueprintX))
		{
			return;
		}
		const TWeakObjectPtr<UActorComponent> Original = Node->ComponentTemplate;
		FCompilerResultsLog Log;
		Log.bSilentMode = true;
		FKismetEditorUtilities::CompileBlueprint(BlueprintX, EBlueprintCompileOptions::SkipGarbageCollection, &Log);
		UActorComponent* Old = Original.Get(true);
		if (!Test.TestTrue(TEXT("(d) the compile reinstanced the template"), Old != nullptr && Old != Node->ComponentTemplate
			&& IsStaleClass(Old->GetClass(), X.Generated())))
		{
			return;
		}
		// What the pre-fix dependent restore did: the compile's new template detached, the old one
		// revived under the template name and written back into the node.
		UActorComponent* Replacement = Node->ComponentTemplate;
		UObject* Outer = Replacement->GetOuter();
		const FString Name = Replacement->GetName();
		constexpr ERenameFlags Flags = REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty;
		Replacement->Rename(nullptr, GetTransientPackage(), Flags);
		Replacement->MarkAsGarbage();
		Old->ClearGarbage();
		Old->Rename(*Name, Outer, Flags);
		Node->ComponentTemplate = Old;
		++ChildRevision;
		ApplyOk(B, ChildDocument(TEXT("Update")), TEXT("(d) BP_B apply repairs BP_A"), true);
		CheckAll(TEXT("(d) BP_B apply repairs BP_A"));
	}

	// (b) The component Blueprint, then the child Actor Blueprint.
	void RunComponentThenChildApply()
	{
		if (!ApplyComponentEdit(TEXT("(b) BPC_X apply")))
		{
			return;
		}
		++ChildRevision;
		ApplyOk(B, ChildDocument(TEXT("Update")), TEXT("(b) BP_B apply"));
		CheckAll(TEXT("(b) BP_B apply"));
	}
};
} // namespace AssetDocumentBlueprintDependentTemplateTests


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintDependentTemplatesRepeatedTest,
	"UEAssetDoc.Blueprint.DependentTemplates.CycleRepeatedComponentApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBlueprintDependentTemplatesRepeatedTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentBlueprintDependentTemplateTests;
	FFixture Fixture(*this, true);
	if (Fixture.Build())
	{
		Fixture.CheckAll(TEXT("loaded from disk"));
		Fixture.RunRepeatedComponentApply();
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintDependentTemplatesChildTest,
	"UEAssetDoc.Blueprint.DependentTemplates.CycleComponentThenChildApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBlueprintDependentTemplatesChildTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentBlueprintDependentTemplateTests;
	FFixture Fixture(*this, true);
	if (Fixture.Build())
	{
		Fixture.RunComponentThenChildApply();
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintDependentTemplatesNoCycleTest,
	"UEAssetDoc.Blueprint.DependentTemplates.NoCycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBlueprintDependentTemplatesNoCycleTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentBlueprintDependentTemplateTests;
	FFixture Fixture(*this, false);
	if (Fixture.Build())
	{
		Fixture.CheckAll(TEXT("loaded from disk"));
		Fixture.RunRepeatedComponentApply();
		Fixture.RunComponentThenChildApply();
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintDependentTemplatesRichTest,
	"UEAssetDoc.Blueprint.DependentTemplates.CycleCharacterOverride",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBlueprintDependentTemplatesRichTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentBlueprintDependentTemplateTests;
	FFixture Fixture(*this, true, true);
	if (Fixture.Build())
	{
		Fixture.CheckAll(TEXT("loaded from disk"));
		Fixture.RunRepeatedComponentApply();
		Fixture.RunComponentThenChildApply();
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintDependentTemplatesFailedTest,
	"UEAssetDoc.Blueprint.DependentTemplates.CycleFailedComponentApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBlueprintDependentTemplatesFailedTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentBlueprintDependentTemplateTests;
	FFixture Fixture(*this, true, true);
	if (Fixture.Build())
	{
		Fixture.RunFailedComponentApply();
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintDependentTemplatesRepairTest,
	"UEAssetDoc.Blueprint.DependentTemplates.SafetyNetRepairsStaleTemplate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBlueprintDependentTemplatesRepairTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentBlueprintDependentTemplateTests;
	FFixture Fixture(*this, false);
	if (Fixture.Build())
	{
		Fixture.RunStaleTemplateRepair();
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintDependentTemplatesFailedNoCycleTest,
	"UEAssetDoc.Blueprint.DependentTemplates.NoCycleFailedComponentApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBlueprintDependentTemplatesFailedNoCycleTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentBlueprintDependentTemplateTests;
	FFixture Fixture(*this, false);
	if (Fixture.Build())
	{
		Fixture.RunFailedComponentApply();
	}
	return !HasAnyErrors();
}
#endif
