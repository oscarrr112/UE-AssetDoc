// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Blueprint apply safety (rollback, Play-In-Editor refusal, variable type change dialog, dependency
// compile).
//
// Fixture: an Enhanced Input action IA_Jump, a component Blueprint BPC_Core
// (variable Health, function GetHealthValue), a Character Blueprint BP_Base holding BPC_Core as SCS
// component Combat and calling into it, and BP_Player, a child of BP_Base with an Enhanced Input action
// event (IA_Jump Triggered -> Jumps += 1), a component-bound event on its own sphere Trigger and an
// override of the inherited Combat. Everything is saved, unloaded and loaded back (an editor restart).
//
//   EnhancedInputRollback: failing applies of BP_Player (a Text pin given DefaultValue, a wrong component
//     property name) roll back without poisoning the package, before and after a successful apply.
//   PieRefusedBeforeMutation: an apply while Play-In-Editor runs is refused before anything is touched.
//   DependencyCompileKeepsDependentsClean: applying BPC_Core leaves BP_Base / BP_Player saved, so the
//     next apply of BP_Player is not refused as an unsaved target.
//   VariableTypeChange: a member / local variable type change used by nodes does not wait on the
//     editor's "Change Variable Type" confirmation.
//   DialogAnswered: a dialog an apply cannot avoid (interface removal with an unloaded child Blueprint)
//     is answered with its default and reported, never waited on.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/BlueprintSupport.h"
#include "Editor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/DynamicBlueprintBinding.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "PackageTools.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentBlueprintApplySafetyTests
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

	UObject* GetObject() const { return FSoftObjectPath(ObjectPath).ResolveObject(); }
	UBlueprint* Get() const { return Cast<UBlueprint>(GetObject()); }
	UPackage* Package() const { return FindPackage(nullptr, *PackageName); }
	bool IsDirty() const { const UPackage* Found = Package(); return Found != nullptr && Found->IsDirty(); }
};

bool HasCode(const FAssetDocumentResult& Result, const TCHAR* Code)
{
	return Result.Diagnostics.ContainsByPredicate([Code](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == Code; });
}

bool IsTrashName(const FString& Name)
{
	return Name.StartsWith(TEXT("TRASHCLASS_")) || Name.StartsWith(TEXT("REINST_")) || Name.StartsWith(TEXT("TRASH_"));
}

// The engine's unattended-modal handling logs a warning plus an error-level stack dump
// (FDebug::DumpStackTraceToLog); that is the expected evidence of an answered dialog.
void ExpectAnsweredDialogLog(FAutomationTestBase& Test)
{
	Test.AddExpectedMessage(TEXT("unattended script mode"), ELogVerbosity::All, EAutomationExpectedMessageFlags::Contains, 0, false);
	Test.AddExpectedMessage(TEXT("FDebug::DumpStackTrace"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, -1, false);
	Test.AddExpectedMessage(TEXT("[Callstack]"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, -1, false);
	Test.AddExpectedMessage(TEXT(""), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Exact, -1, false);
}

struct FFixtureBase
{
	FAutomationTestBase& Test;
	const FString Folder;
	// Copies: the fixtures' own FAsset members are gone when this destructor runs.
	TArray<FAsset> Owned;

	explicit FFixtureBase(FAutomationTestBase& InTest)
		: Test(InTest)
		, Folder(FString(TEXT("AssetDocumentP64/")) + FGuid::NewGuid().ToString(EGuidFormats::Digits))
	{
	}

	~FFixtureBase()
	{
		TArray<UPackage*> Packages;
		for (const FAsset& Asset : Owned)
		{
			if (UPackage* Package = Asset.Package())
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
		for (const FAsset& Asset : Owned)
		{
			IFileManager::Get().Delete(*Asset.PackageFile, false, true);
			IFileManager::Get().Delete(*Asset.SidecarPath, false, true);
			Files.Add(Asset.PackageFile);
		}
		FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles(Files);
		if (!Owned.IsEmpty())
		{
			IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Owned[0].SidecarPath), false, true);
		}
	}

	FAssetDocumentResult Apply(const FAsset& Asset, const FString& Json, const FString& Stage) const
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
		return Result;
	}

	FAssetDocumentResult Validate(const FAsset& Asset, const FString& Json, const FString& Stage) const
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Asset.SidecarPath), true);
		FFileHelper::SaveStringToFile(Json, *Asset.SidecarPath);
		FAssetDocumentValidateRequest Request;
		Request.FilePath = Asset.SidecarPath;
		const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Validate(Request);
		Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *Result.Message));
		for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
		{
			Test.AddInfo(FString::Printf(TEXT("%s: [%s] %s %s %s"), *Stage, *Diagnostic.Severity, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
		}
		return Result;
	}

	bool ApplyOk(const FAsset& Asset, const FString& Json, const FString& Stage) const
	{
		const FAssetDocumentResult Result = Apply(Asset, Json, Stage);
		return Test.TestTrue(*FString::Printf(TEXT("%s: applies"), *Stage), Result.IsSuccess());
	}

	// Unloads the packages and loads Load back from disk (an editor restart).
	bool Reload(const TArray<const FAsset*>& Assets, const FAsset& Load) const
	{
		TArray<UPackage*> Packages;
		for (const FAsset* Asset : Assets)
		{
			UPackage* Package = Asset->Package();
			if (!Test.TestNotNull(*FString::Printf(TEXT("%s exists"), *Asset->Name), Package)
				|| !Test.TestFalse(*FString::Printf(TEXT("%s is saved"), *Asset->Name), Package->IsDirty()))
			{
				return false;
			}
			Packages.Add(Package);
		}
		UPackageTools::UnloadPackages(Packages);
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		for (const FAsset* Asset : Assets)
		{
			if (!Test.TestNull(*FString::Printf(TEXT("%s is unloaded"), *Asset->Name), Asset->GetObject()))
			{
				return false;
			}
		}
		return Test.TestNotNull(*FString::Printf(TEXT("%s loads back from disk"), *Load.Name), LoadObject<UObject>(nullptr, *Load.ObjectPath));
	}
};

// ---- Fixture --------------------------------------------------------------------------

enum class EPlayerFailure : uint8
{
	None,
	// A Text pin given DefaultValue (DefaultTextValue is the right field): the post-apply check fails.
	TextPinDefault,
	// "CollisionProfileName" is not a property of the component (it lives in BodyInstance).
	ComponentPropertyName,
};

struct FPlayerFixture : FFixtureBase
{
	const FAsset Input;
	const FAsset Core;
	const FAsset Base;
	const FAsset Player;
	int32 Jumps = 1;
	bool bCoreStamina = false;

	explicit FPlayerFixture(FAutomationTestBase& InTest)
		: FFixtureBase(InTest)
		, Input(Folder, TEXT("IA_Jump"))
		, Core(Folder, TEXT("BPC_Core"))
		, Base(Folder, TEXT("BP_Base"))
		, Player(Folder, TEXT("BP_Player"))
	{
		Owned = {Player, Base, Core, Input};
	}

	FString InputDocument() const
	{
		return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/EnhancedInput.InputAction\","
			"\"Action\":\"Create\",\"AssetType\":\"GenericAsset\",\"Properties\":{\"ValueType\":\"Boolean\"}}"), *Input.PackageName);
	}

	FString CoreDocument(const TCHAR* Action) const
	{
		const FString Stamina = bCoreStamina ? FString(TEXT(",{\"Name\":\"Stamina\",\"Type\":\"float\",\"DefaultValue\":1.5}")) : FString();
		return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"%s\","
			"\"Body\":{\"ParentClass\":\"/Script/Engine.ActorComponent\","
			"\"Variables\":[{\"Name\":\"Health\",\"Type\":\"int\",\"DefaultValue\":%d}%s],"
			"\"FunctionGraphs\":[{\"Name\":\"GetHealthValue\",\"Signature\":{\"Outputs\":[{\"Name\":\"Value\",\"Type\":\"int\"}]},"
			"\"Nodes\":[{\"Id\":\"Get\",\"Action\":\"K2:Get:Health\"}],"
			"\"Links\":[{\"From\":\"Entry:then\",\"To\":\"Result:execute\"},{\"From\":\"Get:Health\",\"To\":\"Result:Value\"}]}]}}"),
			*Core.PackageName, Action, bCoreStamina ? 12 : 10, *Stamina);
	}

	FString BaseDocument(const TCHAR* Action) const
	{
		return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"%s\","
			"\"Body\":{\"ParentClass\":\"/Script/Engine.Character\",\"Components\":[{\"Name\":\"Combat\",\"Class\":\"%s\"}],"
			"\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":["
			"{\"Id\":\"Begin\",\"Action\":\"K2:BeginPlay\"},"
			"{\"Id\":\"GetCombat\",\"Action\":\"K2:Get:Combat\"},"
			"{\"Id\":\"Call\",\"Action\":\"K2:CallFunction:%s:GetHealthValue\"}],"
			"\"Links\":[{\"From\":\"Begin:then\",\"To\":\"Call:execute\"},{\"From\":\"GetCombat:Combat\",\"To\":\"Call:self\"}]}]}}"),
			*Base.PackageName, Action, *Core.ClassPath, *Core.ClassPath);
	}

	FString PlayerDocument(const TCHAR* Action, EPlayerFailure Failure = EPlayerFailure::None) const
	{
		const FString TriggerProperties = Failure == EPlayerFailure::ComponentPropertyName
			? FString(TEXT(",\"Properties\":{\"CollisionProfileName\":\"NoCollision\"}")) : FString();
		const FString FailingNode = Failure == EPlayerFailure::TextPinDefault ? FString(TEXT(
			",{\"Id\":\"Txt\",\"Action\":\"K2:CallFunction:/Script/Engine.KismetTextLibrary:Conv_TextToString\","
			"\"PinOverrides\":[{\"Pin\":\"InText\",\"DefaultValue\":\"boom\"}]}")) : FString();
		return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"%s\","
			"\"Body\":{\"ParentClass\":\"%s\","
			"\"Variables\":[{\"Name\":\"Jumps\",\"Type\":\"int\",\"DefaultValue\":%d}],"
			"\"Components\":[{\"Name\":\"Trigger\",\"Class\":\"/Script/Engine.SphereComponent\",\"AttachTo\":\"CapsuleComponent\"%s},"
			"{\"Name\":\"Combat\",\"Scope\":\"Inherited\",\"Properties\":{\"Health\":5}}],"
			"\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":["
			"{\"Id\":\"Jump\",\"Action\":\"K2:EnhancedInputAction:%s\"},"
			"{\"Id\":\"GetJumps\",\"Action\":\"K2:Get:Jumps\"},"
			"{\"Id\":\"Inc\",\"Action\":\"K2:CallFunction:/Script/Engine.KismetMathLibrary:Add_IntInt\",\"PinOverrides\":[{\"Pin\":\"B\",\"DefaultValue\":\"1\"}]},"
			"{\"Id\":\"SetJumps\",\"Action\":\"K2:Set:Jumps\"},"
			"{\"Id\":\"Overlap\",\"Action\":\"K2:ComponentBoundEvent:/Script/Engine.PrimitiveComponent:OnComponentBeginOverlap@Trigger\"},"
			"{\"Id\":\"GetCombat\",\"Action\":\"K2:VariableGet:%s:Combat\"},"
			"{\"Id\":\"Health\",\"Action\":\"K2:CallFunction:%s:GetHealthValue\"}%s],"
			"\"Links\":[{\"From\":\"Jump:Triggered\",\"To\":\"SetJumps:execute\"},{\"From\":\"GetJumps:Jumps\",\"To\":\"Inc:A\"},"
			"{\"From\":\"Inc:ReturnValue\",\"To\":\"SetJumps:Jumps\"},{\"From\":\"Overlap:then\",\"To\":\"Health:execute\"},"
			"{\"From\":\"GetCombat:Combat\",\"To\":\"Health:self\"}]}]}}"),
			*Player.PackageName, Action, *Base.ClassPath, Jumps, *TriggerProperties, *Input.ObjectPath, *Base.ClassPath, *Core.ClassPath, *FailingNode);
	}

	bool Build()
	{
		if (!ApplyOk(Input, InputDocument(), TEXT("create IA_Jump"))
			|| !ApplyOk(Core, CoreDocument(TEXT("Create")), TEXT("create BPC_Core"))
			|| !ApplyOk(Base, BaseDocument(TEXT("Create")), TEXT("create BP_Base"))
			|| !ApplyOk(Player, PlayerDocument(TEXT("Create")), TEXT("create BP_Player")))
		{
			return false;
		}
		// IA_Jump stays loaded (the editor's input subsystems keep the action referenced).
		return Reload({&Player, &Base, &Core}, Player)
			&& Test.TestNotNull(TEXT("BP_Base is loaded"), Base.Get()) && Test.TestNotNull(TEXT("BPC_Core is loaded"), Core.Get());
	}

	int32 PlayerJumpsDefault() const
	{
		const UBlueprint* Blueprint = Player.Get();
		UClass* Class = Blueprint ? Blueprint->GeneratedClass.Get() : nullptr;
		const FIntProperty* Property = Class ? FindFProperty<FIntProperty>(Class, TEXT("Jumps")) : nullptr;
		return Property != nullptr ? Property->GetPropertyValue_InContainer(Class->GetDefaultObject()) : INDEX_NONE;
	}

	// BP_Player's generated class holds its Enhanced Input and component-bound event bindings, both on the
	// current class (never moved to a TRASHCLASS_), the Blueprint compiles and nothing in its package is an
	// instance of an outdated class.
	void CheckPlayer(const FString& Stage) const
	{
		const UBlueprint* Blueprint = Player.Get();
		if (!Test.TestNotNull(*FString::Printf(TEXT("%s: BP_Player is loaded"), *Stage), Blueprint))
		{
			return;
		}
		Test.TestTrue(*FString::Printf(TEXT("%s: BP_Player compiles"), *Stage),
			Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
		const UBlueprintGeneratedClass* Class = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
		if (!Test.TestNotNull(*FString::Printf(TEXT("%s: BP_Player has a generated class"), *Stage), Class))
		{
			return;
		}
		Test.TestFalse(*FString::Printf(TEXT("%s: BP_Player generated class is current"), *Stage),
			IsTrashName(Class->GetName()) || Class->HasAnyClassFlags(CLASS_NewerVersionExists));
		bool bEnhancedInput = false;
		bool bComponentBound = false;
		for (const UDynamicBlueprintBinding* Binding : Class->DynamicBindingObjects)
		{
			if (!Test.TestTrue(*FString::Printf(TEXT("%s: binding %s is on the current class"), *Stage, *GetPathNameSafe(Binding)),
				Binding != nullptr && IsValid(Binding) && Binding->GetOuter() == Class))
			{
				continue;
			}
			bEnhancedInput |= Binding->GetClass()->GetName() == TEXT("EnhancedInputActionDelegateBinding");
			bComponentBound |= Binding->GetClass()->GetName() == TEXT("ComponentDelegateBinding");
		}
		Test.TestTrue(*FString::Printf(TEXT("%s: BP_Player has its Enhanced Input action binding"), *Stage), bEnhancedInput);
		Test.TestTrue(*FString::Printf(TEXT("%s: BP_Player has its component-bound event binding"), *Stage), bComponentBound);
		TArray<FString> Stale;
		ForEachObjectWithPackage(Blueprint->GetOutermost(), [&Stale](UObject* Object)
		{
			for (const UObject* Current = Object; Current != nullptr; Current = Current->GetOuter())
			{
				if (IsTrashName(Current->GetName()) || IsTrashName(Current->GetClass()->GetName()))
				{
					Stale.Add(Object->GetPathName());
					break;
				}
			}
			return true;
		});
		if (!Test.TestEqual(*FString::Printf(TEXT("%s: BP_Player holds no TRASHCLASS_/REINST_ object"), *Stage), Stale.Num(), 0))
		{
			for (const FString& Entry : Stale)
			{
				Test.AddInfo(FString::Printf(TEXT("%s: stale %s"), *Stage, *Entry));
			}
		}
	}

	// A failed apply: rolled back (the previous Jumps default, saved) and never poisoned.
	void CheckRolledBack(const FAssetDocumentResult& Result, int32 ExpectedJumps, const FString& Stage) const
	{
		Test.TestFalse(*FString::Printf(TEXT("%s: the apply fails"), *Stage), Result.IsSuccess());
		for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
		{
			Test.TestFalse(*FString::Printf(TEXT("%s: no rollback failure (%s)"), *Stage, *Diagnostic.Code),
				Diagnostic.Code.StartsWith(TEXT("PackageSnapshot.")) || Diagnostic.Code.StartsWith(TEXT("Atomic.Poisoned"))
				|| Diagnostic.Code == TEXT("Graph.BlueprintRebuildFailed"));
		}
		Test.TestEqual(*FString::Printf(TEXT("%s: Jumps default is the pre-apply value"), *Stage), PlayerJumpsDefault(), ExpectedJumps);
		Test.TestFalse(*FString::Printf(TEXT("%s: BP_Player stays saved"), *Stage), Player.IsDirty());
		CheckPlayer(Stage);
	}

	void FailPlayer(EPlayerFailure Failure, const FString& Stage)
	{
		const int32 Before = Jumps;
		++Jumps;
		const FAssetDocumentResult Result = Apply(Player, PlayerDocument(TEXT("Update"), Failure), Stage);
		Jumps = Before;
		CheckRolledBack(Result, Before, Stage);
	}

	bool ApplyPlayer(const FString& Stage)
	{
		++Jumps;
		const bool bOk = ApplyOk(Player, PlayerDocument(TEXT("Update")), Stage);
		Test.TestEqual(*FString::Printf(TEXT("%s: Jumps default"), *Stage), PlayerJumpsDefault(), Jumps);
		CheckPlayer(Stage);
		return bOk;
	}
};

// ---- variable type fixture ----------------------------------------------------------------------

struct FVariableTypeFixture : FFixtureBase
{
	const FAsset Target;

	explicit FVariableTypeFixture(FAutomationTestBase& InTest)
		: FFixtureBase(InTest)
		, Target(Folder, TEXT("BP_FxUser"))
	{
		Owned = {Target};
	}

	// Member variable Fx and local variable LocalFx, each read by a node that calls Activate on it.
	FString Document(const TCHAR* Action, const TCHAR* ComponentClass) const
	{
		return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"%s\","
			"\"Body\":{\"ParentClass\":\"/Script/Engine.Actor\","
			"\"Variables\":[{\"Name\":\"Fx\",\"Type\":\"object:%s\"}],"
			"\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":["
			"{\"Id\":\"Begin\",\"Action\":\"K2:BeginPlay\"},"
			"{\"Id\":\"GetFx\",\"Action\":\"K2:Get:Fx\"},"
			"{\"Id\":\"Act\",\"Action\":\"K2:CallFunction:/Script/Engine.ActorComponent:Activate\"}],"
			"\"Links\":[{\"From\":\"Begin:then\",\"To\":\"Act:execute\"},{\"From\":\"GetFx:Fx\",\"To\":\"Act:self\"}]}],"
			"\"FunctionGraphs\":[{\"Name\":\"UseLocal\",\"LocalVariables\":[{\"Name\":\"LocalFx\",\"Type\":\"object:%s\"}],"
			"\"Nodes\":[{\"Id\":\"GetLocal\",\"Action\":\"K2:VariableGet:%s:UseLocal:LocalFx\"},"
			"{\"Id\":\"Act\",\"Action\":\"K2:CallFunction:/Script/Engine.ActorComponent:Activate\"}],"
			"\"Links\":[{\"From\":\"Entry:then\",\"To\":\"Act:execute\"},{\"From\":\"GetLocal:LocalFx\",\"To\":\"Act:self\"}]}]}}"),
			*Target.PackageName, Action, ComponentClass, ComponentClass, *Target.ClassPath);
	}
};

// ---- dialog fixture ---------------------------------------------------------------------------

struct FInterfaceFixture : FFixtureBase
{
	const FAsset Interface;
	const FAsset Parent;
	const FAsset Child;

	explicit FInterfaceFixture(FAutomationTestBase& InTest)
		: FFixtureBase(InTest)
		, Interface(Folder, TEXT("BPI_Ping"))
		, Parent(Folder, TEXT("BP_Pinged"))
		, Child(Folder, TEXT("BP_PingedChild"))
	{
		Owned = {Child, Parent, Interface};
	}

	FString InterfaceDocument() const
	{
		return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"Create\","
			"\"Body\":{\"BlueprintType\":\"Interface\"}}"), *Interface.PackageName);
	}

	FString ParentDocument(const TCHAR* Action, bool bImplements) const
	{
		return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"%s\","
			"\"Body\":{\"ParentClass\":\"/Script/Engine.Actor\",\"ImplementedInterfaces\":[%s]}}"),
			*Parent.PackageName, Action, bImplements ? *FString::Printf(TEXT("\"%s\""), *Interface.ClassPath) : TEXT(""));
	}

	FString ChildDocument() const
	{
		return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"Create\","
			"\"Body\":{\"ParentClass\":\"%s\"}}"), *Child.PackageName, *Parent.ClassPath);
	}
};

const FEdGraphPinType* FindLocalVariableType(UBlueprint& Blueprint, FName Graph, FName Variable)
{
	for (UEdGraph* FunctionGraph : Blueprint.FunctionGraphs)
	{
		if (FunctionGraph == nullptr || FunctionGraph->GetFName() != Graph)
		{
			continue;
		}
		TArray<UK2Node_FunctionEntry*> Entries;
		FunctionGraph->GetNodesOfClass(Entries);
		for (const UK2Node_FunctionEntry* Entry : Entries)
		{
			for (const FBPVariableDescription& Local : Entry->LocalVariables)
			{
				if (Local.VarName == Variable)
				{
					return &Local.VarType;
				}
			}
		}
	}
	return nullptr;
}

// Every Get node of Variable in Blueprint has its value pin wired (the type change kept the links).
bool AreVariableGetsLinked(UBlueprint& Blueprint, FName Variable)
{
	TArray<UK2Node_VariableGet*> Gets;
	FBlueprintEditorUtils::GetAllNodesOfClass(&Blueprint, Gets);
	int32 Found = 0;
	for (const UK2Node_VariableGet* Get : Gets)
	{
		if (Get->GetVarName() != Variable)
		{
			continue;
		}
		++Found;
		const UEdGraphPin* Value = Get->FindPin(Variable);
		if (Value == nullptr || Value->LinkedTo.IsEmpty())
		{
			return false;
		}
	}
	return Found > 0;
}
} // namespace AssetDocumentBlueprintApplySafetyTests


// Every failed apply of a Blueprint with Enhanced Input action events failed its
// rollback (EnhancedInputActionDelegateBinding_N on TRASHCLASS_BP_Player_*, PackageSnapshot.RestoreFailed,
// Atomic.Poisoned.RollbackFailed) and the Blueprint needed an editor restart.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintApplySafetyEnhancedInputRollbackTest,
	"UEAssetDoc.Blueprint.ApplySafety.EnhancedInputRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBlueprintApplySafetyEnhancedInputRollbackTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentBlueprintApplySafetyTests;
	FPlayerFixture Fixture(*this);
	if (!Fixture.Build())
	{
		return false;
	}
	Fixture.CheckPlayer(TEXT("loaded from disk"));
	Fixture.FailPlayer(EPlayerFailure::TextPinDefault, TEXT("(1) Text pin DefaultValue"));
	Fixture.FailPlayer(EPlayerFailure::ComponentPropertyName, TEXT("(2) wrong component property name"));
	// The rolled-back Blueprint takes the next apply without a reload.
	Fixture.ApplyPlayer(TEXT("(3) valid apply after the failures"));
	// A failed apply after a successful one in the same session.
	Fixture.FailPlayer(EPlayerFailure::TextPinDefault, TEXT("(4) failing apply after a successful one"));
	Fixture.ApplyPlayer(TEXT("(5) valid apply again"));
	return !HasAnyErrors();
}

// An apply while Play-In-Editor runs is refused before anything is touched: no package snapshot is
// restored, nothing compiles, the asset stays saved. Validate says the same. Another Profile with a PIE
// refusal (User Defined Enum, data provider) is refused the same way.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintApplySafetyPieTest,
	"UEAssetDoc.Blueprint.ApplySafety.PieRefusedBeforeMutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBlueprintApplySafetyPieTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentBlueprintApplySafetyTests;
	FPlayerFixture Fixture(*this);
	const FAsset Enum(Fixture.Folder, TEXT("E_Mode"));
	Fixture.Owned.Add(Enum);
	const auto EnumDocument = [&Enum](const TCHAR* Action, const TCHAR* Second)
	{
		return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.UserDefinedEnum\",\"Action\":\"%s\","
			"\"Body\":{\"Enumerators\":[{\"Name\":\"Melee\"},{\"Name\":\"%s\"}]}}"), *Enum.PackageName, Action, Second);
	};
	if (!Fixture.Build() || !Fixture.ApplyOk(Enum, EnumDocument(TEXT("Create"), TEXT("Ranged")), TEXT("create E_Mode"))
		|| !TestNotNull(TEXT("GEditor"), GEditor))
	{
		return false;
	}
	UBlueprint* Blueprint = Fixture.Player.Get();
	int32 Compiles = 0;
	const FDelegateHandle Compiled = Blueprint->OnCompiled().AddLambda([&Compiles](UBlueprint*) { ++Compiles; });
	ON_SCOPE_EXIT
	{
		if (UBlueprint* Current = Fixture.Player.Get())
		{
			Current->OnCompiled().Remove(Compiled);
		}
	};
	{
		// A play world as Play-In-Editor publishes it (GEditor->PlayWorld).
		UWorld* PlayWorld = UWorld::CreateWorld(EWorldType::PIE, false, TEXT("AssetDocumentP64PlayWorld"));
		if (!TestNotNull(TEXT("play world"), PlayWorld) || !TestNull(TEXT("no play session is running"), GEditor->PlayWorld.Get()))
		{
			return false;
		}
		GEditor->PlayWorld = PlayWorld;
		ON_SCOPE_EXIT
		{
			GEditor->PlayWorld = nullptr;
			PlayWorld->DestroyWorld(false);
		};
		const int32 Before = Fixture.Jumps;
		++Fixture.Jumps;
		const FString Document = Fixture.PlayerDocument(TEXT("Update"));
		const FAssetDocumentResult Validated = Fixture.Validate(Fixture.Player, Document, TEXT("validate during PIE"));
		TestFalse(TEXT("validate during PIE fails"), Validated.IsSuccess());
		TestTrue(TEXT("validate during PIE reports Graph.PieActive"), HasCode(Validated, TEXT("Graph.PieActive")));
		const FAssetDocumentResult Result = Fixture.Apply(Fixture.Player, Document, TEXT("apply during PIE"));
		TestTrue(TEXT("apply during PIE reports Graph.PieActive"), HasCode(Result, TEXT("Graph.PieActive")));
		Fixture.CheckRolledBack(Result, Before, TEXT("apply during PIE"));
		TestEqual(TEXT("nothing compiled BP_Player during PIE (no mutation, no snapshot restore)"), Compiles, 0);
		Fixture.Jumps = Before;

		const FAssetDocumentResult EnumResult = Fixture.Apply(Enum, EnumDocument(TEXT("Update"), TEXT("Magic")), TEXT("enum apply during PIE"));
		TestFalse(TEXT("enum apply during PIE fails"), EnumResult.IsSuccess());
		TestTrue(TEXT("enum apply during PIE reports Graph.PieActive"), HasCode(EnumResult, TEXT("Graph.PieActive")));
		TestFalse(TEXT("E_Mode stays saved"), Enum.IsDirty());
		for (const FAssetDocumentDiagnostic& Diagnostic : EnumResult.Diagnostics)
		{
			TestFalse(*FString::Printf(TEXT("enum apply during PIE: no rollback (%s)"), *Diagnostic.Code),
				Diagnostic.Code.StartsWith(TEXT("PackageSnapshot.")) || Diagnostic.Code.StartsWith(TEXT("Atomic.")));
		}
	}
	Fixture.ApplyPlayer(TEXT("apply after PIE ended"));
	return !HasAnyErrors();
}

// Dependency-compile friction: after applying the component Blueprint (a structural change: a new
// variable), the dependent Character Blueprints must be as an editor compile leaves them (saved; the
// compilation manager restores each dependent's dirty flag), so the next apply of BP_Player is not
// refused with DirtyExistingAssetTransactionUnsupported.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintApplySafetyDependencyDirtTest,
	"UEAssetDoc.Blueprint.ApplySafety.DependencyCompileKeepsDependentsClean",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBlueprintApplySafetyDependencyDirtTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentBlueprintApplySafetyTests;
	FPlayerFixture Fixture(*this);
	if (!Fixture.Build())
	{
		return false;
	}
	// BP_Player placed in the open level: its BPC_Core component is reinstanced too.
	UWorld* EditorWorld = GEditor != nullptr ? GEditor->GetEditorWorldContext().World() : nullptr;
	UClass* PlayerClass = Fixture.Player.Get() != nullptr ? Fixture.Player.Get()->GeneratedClass.Get() : nullptr;
	if (!TestNotNull(TEXT("editor world"), EditorWorld) || !TestNotNull(TEXT("BP_Player class"), PlayerClass))
	{
		return false;
	}
	FActorSpawnParameters Spawn;
	Spawn.ObjectFlags = RF_Transient;
	Spawn.bNoFail = true;
	TWeakObjectPtr<AActor> Placed = EditorWorld->SpawnActor<AActor>(PlayerClass, FTransform::Identity, Spawn);
	ON_SCOPE_EXIT
	{
		if (AActor* Actor = Placed.Get())
		{
			Actor->Destroy();
		}
	};
	const bool bLevelWasDirty = EditorWorld->GetOutermost()->IsDirty();
	// BPC_Core gains a variable (a structural change), its Health default changes and BP_Player calls into it.
	Fixture.bCoreStamina = true;
	if (!Fixture.ApplyOk(Fixture.Core, Fixture.CoreDocument(TEXT("Update")), TEXT("BPC_Core gains a variable")))
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("level dirty before %d, after %d (the engine's reinstancing decides)"),
		bLevelWasDirty ? 1 : 0, EditorWorld->GetOutermost()->IsDirty() ? 1 : 0));
	TestFalse(TEXT("BP_Base stays saved after the BPC_Core apply"), Fixture.Base.IsDirty());
	TestFalse(TEXT("BP_Player stays saved after the BPC_Core apply"), Fixture.Player.IsDirty());
	Fixture.ApplyPlayer(TEXT("BP_Player apply after the BPC_Core apply"));
	// A real unsaved edit of the dependent is still refused.
	if (UBlueprint* Player = Fixture.Player.Get())
	{
		Player->BlueprintDescription = TEXT("unsaved user edit");
		Player->MarkPackageDirty();
		++Fixture.Jumps;
		const FAssetDocumentResult Refused = Fixture.Apply(Fixture.Player, Fixture.PlayerDocument(TEXT("Update")), TEXT("unsaved user edit"));
		--Fixture.Jumps;
		TestFalse(TEXT("an apply over a real unsaved edit is refused"), Refused.IsSuccess());
		TestTrue(TEXT("an apply over a real unsaved edit reports DirtyExistingAssetTransactionUnsupported"),
			HasCode(Refused, TEXT("DirtyExistingAssetTransactionUnsupported")) || Refused.Message.Contains(TEXT("DirtyExistingAssetTransactionUnsupported")));
	}
	return !HasAnyErrors();
}

// Changing the type of a member variable (and of a local variable) that nodes use asks "Change
// Variable Type: This could break connections..." in the editor. The document already states the type:
// the apply takes the editor's confirmed path and never shows (or waits on) the dialog.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintApplySafetyVariableTypeTest,
	"UEAssetDoc.Blueprint.ApplySafety.VariableTypeChange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBlueprintApplySafetyVariableTypeTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentBlueprintApplySafetyTests;
	FVariableTypeFixture Fixture(*this);
	if (!Fixture.ApplyOk(Fixture.Target, Fixture.Document(TEXT("Create"), TEXT("/Script/Engine.ParticleSystemComponent")), TEXT("create BP_FxUser")))
	{
		return false;
	}
	// A dialog the apply still opened is cancelled instead of hanging this test.
	TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
	const FAssetDocumentResult Result = Fixture.Apply(Fixture.Target,
		Fixture.Document(TEXT("Update"), TEXT("/Script/Engine.FXSystemComponent")), TEXT("change Fx / LocalFx types"));
	TestTrue(TEXT("the type change applies"), Result.IsSuccess());
	TestFalse(TEXT("no dialog was opened"), HasCode(Result, TEXT("GenericRuntime.EditorDialogAnswered")));
	UBlueprint* Blueprint = Fixture.Target.Get();
	if (!TestNotNull(TEXT("BP_FxUser is loaded"), Blueprint))
	{
		return false;
	}
	const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, TEXT("Fx"));
	TestTrue(TEXT("Fx is now an FXSystemComponent"), Index != INDEX_NONE
		&& Blueprint->NewVariables[Index].VarType.PinSubCategoryObject.Get() == FindObject<UClass>(nullptr, TEXT("/Script/Engine.FXSystemComponent")));
	const FEdGraphPinType* Local = FindLocalVariableType(*Blueprint, TEXT("UseLocal"), TEXT("LocalFx"));
	TestTrue(TEXT("LocalFx is now an FXSystemComponent"), Local != nullptr
		&& Local->PinSubCategoryObject.Get() == FindObject<UClass>(nullptr, TEXT("/Script/Engine.FXSystemComponent")));
	TestTrue(TEXT("the Fx getter keeps its link"), AreVariableGetsLinked(*Blueprint, TEXT("Fx")));
	TestTrue(TEXT("the LocalFx getter keeps its link"), AreVariableGetsLinked(*Blueprint, TEXT("LocalFx")));
	TestTrue(TEXT("BP_FxUser compiles"), Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
	return !HasAnyErrors();
}

// Generic guard: a dialog an apply cannot avoid is answered with its default (and reported); the request
// never waits for a click. Removing an interface asks "Load N unloaded child blueprints to fix up phantom
// references?" (FBlueprintEditorUtils::MarkBlueprintChildrenAsModified) while a child Blueprint is unloaded;
// the default (No) leaves the child alone, the interface is removed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintApplySafetyDialogTest,
	"UEAssetDoc.Blueprint.ApplySafety.DialogAnswered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBlueprintApplySafetyDialogTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentBlueprintApplySafetyTests;
	FInterfaceFixture Fixture(*this);
	if (!Fixture.ApplyOk(Fixture.Interface, Fixture.InterfaceDocument(), TEXT("create BPI_Ping"))
		|| !Fixture.ApplyOk(Fixture.Parent, Fixture.ParentDocument(TEXT("Create"), true), TEXT("create BP_Pinged"))
		|| !Fixture.ApplyOk(Fixture.Child, Fixture.ChildDocument(), TEXT("create BP_PingedChild")))
	{
		return false;
	}
	UPackage* ChildPackage = Fixture.Child.Package();
	if (!TestNotNull(TEXT("BP_PingedChild package"), ChildPackage) || !TestFalse(TEXT("BP_PingedChild is saved"), ChildPackage->IsDirty()))
	{
		return false;
	}
	UPackageTools::UnloadPackages(TArray<UPackage*>{ChildPackage});
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	if (!TestNull(TEXT("BP_PingedChild is unloaded"), Fixture.Child.GetObject()))
	{
		return false;
	}
	// The editor finds unloaded children through the Asset Registry (FBlueprintEditorUtils::GetChildrenOfBlueprint).
	IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
	Registry.ScanFilesSynchronous({Fixture.Child.PackageFile}, true);
	const FAssetData ChildData = Registry.GetAssetByObjectPath(FSoftObjectPath(Fixture.Child.ObjectPath), true);
	TestTrue(TEXT("the Asset Registry lists BP_PingedChild as a child of BP_Pinged"), ChildData.IsValid()
		&& ChildData.GetTagValueRef<FString>(FBlueprintTags::ParentClassPath).Contains(Fixture.Parent.ClassPath));
	ExpectAnsweredDialogLog(*this);
	const FAssetDocumentResult Result = Fixture.Apply(Fixture.Parent, Fixture.ParentDocument(TEXT("Update"), false), TEXT("remove the interface"));
	TestTrue(TEXT("the interface removal applies"), Result.IsSuccess());
	TestTrue(TEXT("the answered dialog is reported"), HasCode(Result, TEXT("GenericRuntime.EditorDialogAnswered")));
	const UBlueprint* Parent = Fixture.Parent.Get();
	TestTrue(TEXT("BP_Pinged no longer implements BPI_Ping"), Parent != nullptr && Parent->ImplementedInterfaces.IsEmpty());
	TestNull(TEXT("the default (No) did not load BP_PingedChild"), Fixture.Child.GetObject());
	return !HasAnyErrors();
}
#endif
