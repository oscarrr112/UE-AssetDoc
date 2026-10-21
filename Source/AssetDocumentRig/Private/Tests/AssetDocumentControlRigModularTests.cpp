// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Control Rig phase CR4 (docs/architecture/asset-document-controlrig-design.md §7.2: modular rigs).
// UEAssetDoc.ControlRig.ModularFidelity: §6.2 on CR_Modular_Biped and CR_Modular_Car (built with the
//   editor's calls: the factory with ModularRig, AddModule, the primary connector connected with auto-resolve as a
//   drop does, a config override, a variable binding): Create from the extract; equal document, module model,
//   connections and runtime pose; unchanged re-apply no-op; an Update; reload stable; timings.
// UEAssetDoc.ControlRig.ModularFreeEdits: §6.3 modular edits (delete a module, add an Arm and connect
//   it, change a config value, bind a variable, swap a module class, reorder) made through the controller calls of the
//   Module Assembly panel on one duplicate and through a document on another (the secondary connections from
//   Inspect's ModularRigSuggestions.AutoConnect): equal document, model and pose.
// UEAssetDoc.ControlRig.ModularCreate: the from-scratch flow: a modular rig created on a mesh
//   (FactoryContext) with modules and primary connections, then AutoConnect accepted; equals the editor-built rig.
// UEAssetDoc.ControlRig.ModularRollback: refused connections, unknown modules / classes / connectors
//   roll back with candidates; refuse -> GC -> edit the same module again; undo history around a human module edit.
// UEAssetDoc.ControlRig.ModularDependents: editing a rig module while a modular rig that uses it has
//   unsaved changes is refused; otherwise the apply notes the loaded dependents.

#include "AssetDocumentIKRigTestSupport.h"
#include "AssetDocumentControlRig.h"

#include "ControlRig.h"
#include "ControlRigBlueprintLegacy.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ControlRigOverride.h"
#include "ModularRig.h"
#include "ModularRigController.h"
#include "ModularRigModel.h"
#include "ModularRigRuleManager.h"
#include "Rigs/RigHierarchy.h"
#include "Runtime/AssetDocumentUndoHistory.h"
#include "ScopedTransaction.h"
#include "Units/Execution/RigUnit_PrepareForExecution.h"
#include "Utils/AssetDocumentJsonKeys.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentControlRigTests
{
UControlRigBlueprint* MakeControlRig(FAutomationTestBase& Test, const AssetDocumentIKRigTests::FTarget& Target, const TCHAR* MeshPath, bool bModular);
} // namespace AssetDocumentControlRigTests

namespace AssetDocumentControlRigModularTests
{
using namespace AssetDocumentIKRigTests;

const TCHAR* const ControlRigClassPath = TEXT("/Script/ControlRigDeveloper.ControlRigBlueprint");
const TCHAR* const BipedMesh = TEXT("/ControlRigModules/Modules/Meshes/SKM_Biped_Template.SKM_Biped_Template");
const TCHAR* const CarMesh = TEXT("/ControlRigModules/Modules/Meshes/SKM_Car_Template.SKM_Car_Template");

TSharedPtr<FJsonObject> ForTarget(const TSharedPtr<FJsonObject>& Document, const FTarget& Target, const TCHAR* Action)
{
	const TSharedPtr<FJsonObject> Copy = Parse(Serialize(Document));
	Copy->SetStringField(TEXT("Target"), Target.PackageName);
	Copy->SetStringField(TEXT("Action"), Action);
	return Copy;
}

// A JSON literal (outside TEXT(): MSVC does not take raw strings with commas as macro arguments).
TSharedPtr<FJsonObject> Json(const ANSICHAR* Text)
{
	return Parse(FString(UTF8_TO_TCHAR(Text)));
}

TSharedPtr<FJsonValue> Sorted(const TSharedPtr<FJsonValue>& Value)
{
	if (Value.IsValid() && Value->Type == EJson::Object)
	{
		TArray<FString> Keys;
		AssetDocumentServerJson::GetKeys(*Value->AsObject(), Keys);
		Keys.Sort();
		const TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
		for (const FString& Key : Keys)
		{
			Copy->SetField(Key, Sorted(AssetDocumentServerJson::FindFieldRef(*Value->AsObject(), Key)));
		}
		return MakeShared<FJsonValueObject>(Copy);
	}
	if (Value.IsValid() && Value->Type == EJson::Array)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
		{
			Items.Add(Sorted(Item));
		}
		return MakeShared<FJsonValueArray>(Items);
	}
	return Value;
}

// A document as a rig's state, to compare two rigs or a document with the rig it made: keys sorted, without Target
// and Action, and without the asset variant's Guid (each new asset gets its own).
FString Comparable(const TSharedPtr<FJsonObject>& Document)
{
	if (!Document.IsValid())
	{
		return FString();
	}
	const TSharedPtr<FJsonObject> Copy = Parse(Serialize(Document));
	Copy->RemoveField(TEXT("Target"));
	Copy->RemoveField(TEXT("Action"));
	const TSharedPtr<FJsonObject>* Properties = nullptr;
	const TSharedPtr<FJsonObject>* Variant = nullptr;
	if (Copy->TryGetObjectField(TEXT("Properties"), Properties) && (*Properties)->TryGetObjectField(TEXT("AssetVariant"), Variant))
	{
		(*Variant)->RemoveField(TEXT("Guid"));
	}
	return Serialize(Sorted(MakeShared<FJsonValueObject>(Copy))->AsObject());
}

bool ApplyOk(FAutomationTestBase& Test, const FTarget& Target, const TSharedPtr<FJsonObject>& Document, const FString& Stage,
	FAssetDocumentResult* OutResult = nullptr)
{
	const FAssetDocumentResult Result = Apply(Target, Document);
	if (OutResult != nullptr)
	{
		*OutResult = Result;
	}
	if (!Test.TestTrue(*FString::Printf(TEXT("%s applies"), *Stage), Result.IsSuccess()))
	{
		Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *DiagnosticsText(Result)));
		return false;
	}
	return true;
}

// The Finger module's VM reports empty-array reads while its connectors are not resolved yet (in the editor as here,
// whenever a Finger module is added and constructed before it is connected).
void ExpectFingerErrors(FAutomationTestBase& Test)
{
	Test.AddExpectedMessagePlain(TEXT("out of bounds (count 0)"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, -1);
}

FString StatusName(const FAssetDocumentResult& Extracted)
{
	return Extracted.Compile.IsValid() ? Extracted.Compile->GetStringField(TEXT("Status")) : FString(TEXT("-"));
}

// Refused at a document path, nothing changed: the rig extracts as before and its package is clean. Returns the result.
FAssetDocumentResult ExpectRefused(FAutomationTestBase& Test, const FTarget& Target, const TSharedPtr<FJsonObject>& Document, const TCHAR* Code,
	const FString& Stage)
{
	const FString Before = Serialize(ExtractResult(Target.ObjectPath).Payload);
	const FAssetDocumentResult Result = Apply(Target, Document);
	Test.TestFalse(*FString::Printf(TEXT("%s is refused"), *Stage), Result.IsSuccess());
	Test.TestTrue(*FString::Printf(TEXT("%s: %s (got %s)"), *Stage, Code, *DiagnosticsText(Result)), HasCode(Result, Code));
	Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *DiagnosticsText(Result)));
	const FString After = Serialize(ExtractResult(Target.ObjectPath).Payload);
	if (!Test.TestEqual(*FString::Printf(TEXT("%s: the rig is unchanged"), *Stage), After, Before))
	{
		Test.AddInfo(FirstDifference(Before, After));
	}
	if (const UObject* Asset = Target.Get())
	{
		Test.TestFalse(*FString::Printf(TEXT("%s: package clean"), *Stage), Asset->GetOutermost()->IsDirty());
	}
	return Result;
}

FString ModuleAsset(const TCHAR* Class)
{
	return FString::Printf(TEXT("/ControlRigModules/Modules56/%s.%s"), Class, Class);
}

FString ModuleClassPath(const TCHAR* Class)
{
	return ModuleAsset(Class) + TEXT("_C");
}

TSubclassOf<UControlRig> LoadModuleClass(const FString& AssetPath)
{
	const UControlRigBlueprint* Module = LoadObject<UControlRigBlueprint>(nullptr, *AssetPath);
	return Module != nullptr ? TSubclassOf<UControlRig>(Module->GeneratedClass.Get()) : TSubclassOf<UControlRig>();
}

// The Module Assembly panel's view: the debugged instance, constructed (the editor's preview constructs every tick).
UModularRig* ConstructPreview(UControlRigBlueprint& Blueprint)
{
	if (Blueprint.UBlueprint::GetObjectBeingDebugged() == nullptr)
	{
		Blueprint.RecompileModularRig();
	}
	UModularRig* Debugged = Cast<UModularRig>(Blueprint.UBlueprint::GetObjectBeingDebugged());
	if (Debugged != nullptr)
	{
		Debugged->RequestConstruction();
		Debugged->Execute(FRigUnit_PrepareForExecution::EventName);
	}
	return Debugged;
}

// What a drop of the module on Hint does: the primary connector's match named Hint (else the one containing it, else
// the single or default match), connected with auto-resolve of the other connectors.
bool ConnectPrimary(FAutomationTestBase& Test, UControlRigBlueprint& Blueprint, const FName Module, const TCHAR* Hint, FString* OutTarget = nullptr)
{
	UModularRigController* Controller = Blueprint.GetModularRigController();
	UModularRig* Debugged = ConstructPreview(Blueprint);
	const FRigModuleReference* Reference = Controller != nullptr ? Controller->FindModule(Module) : nullptr;
	URigHierarchy* Hierarchy = Debugged != nullptr ? Debugged->GetHierarchy() : nullptr;
	const FRigConnectorElement* Primary = Reference != nullptr && Hierarchy != nullptr ? Reference->FindPrimaryConnector(Hierarchy) : nullptr;
	if (!Test.TestNotNull(*FString::Printf(TEXT("%s: primary connector"), *Module.ToString()), Primary))
	{
		return false;
	}
	// A copy: connecting reconstructs the preview, which frees the connector element.
	const FRigElementKey PrimaryKey = Primary->GetKey();
	const FModularRigResolveResult Result = Hierarchy->GetRuleManager()->FindMatches(Primary, Debugged->FindModule(Module), Debugged->GetElementKeyRedirector());
	FRigElementKey Pick;
	TArray<FString> Names;
	for (const FRigElementResolveResult& Match : Result.GetMatches())
	{
		Names.Add(Match.GetKey().ToString());
		if (!Pick.IsValid() && Match.GetKey().Name.ToString() == Hint)
		{
			Pick = Match.GetKey();
		}
	}
	for (const FRigElementResolveResult& Match : Result.GetMatches())
	{
		if (!Pick.IsValid() && Match.GetKey().Name.ToString().Contains(Hint, ESearchCase::CaseSensitive))
		{
			Pick = Match.GetKey();
		}
	}
	if (!Pick.IsValid())
	{
		Test.AddInfo(FString::Printf(TEXT("%s: no match for '%s' among %s"), *Module.ToString(), Hint, *FString::Join(Names, TEXT(" ")).Left(800)));
		if (Result.GetMatches().Num() == 1)
		{
			Pick = Result.GetMatches()[0].GetKey();
		}
		else if (const FRigElementResolveResult* Default = Result.GetDefaultMatch())
		{
			Pick = Default->GetKey();
		}
	}
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: a target for the primary connector"), *Module.ToString()), Pick.IsValid()))
	{
		return false;
	}
	if (OutTarget != nullptr)
	{
		*OutTarget = Pick.ToString();
	}
	return Test.TestTrue(*FString::Printf(TEXT("%s: %s -> %s"), *Module.ToString(), *PrimaryKey.ToString(), *Pick.ToString()),
		Controller->ConnectConnectorToElement(PrimaryKey, Pick, false, true, true));
}

struct FModulePlan
{
	const TCHAR* Name;
	const TCHAR* Class; // a Modules56 module, null for the factory's Root
	const TCHAR* Parent;
	const TCHAR* Hint;
};

bool AddPlannedModule(FAutomationTestBase& Test, UControlRigBlueprint& Blueprint, const FModulePlan& Plan)
{
	UModularRigController* Controller = Blueprint.GetModularRigController();
	if (Plan.Class != nullptr)
	{
		const TSubclassOf<UControlRig> Class = LoadModuleClass(ModuleAsset(Plan.Class));
		if (!Test.TestTrue(*FString::Printf(TEXT("module class %s"), Plan.Class), Class.Get() != nullptr)
			|| !Test.TestEqual(*FString::Printf(TEXT("module %s added"), Plan.Name), Controller->AddModule(Plan.Name, Class, Plan.Parent, false), FName(Plan.Name)))
		{
			return false;
		}
	}
	return ConnectPrimary(Test, Blueprint, Plan.Name, Plan.Hint);
}

// The Content Browser's Control Rig with ParentClass ModularRig (module Root from the Control Rig settings) on Mesh,
// the planned modules dropped one after another, Extra, compiled and saved.
UControlRigBlueprint* BuildModular(FAutomationTestBase& Test, const FTarget& Target, const TCHAR* Mesh, TConstArrayView<FModulePlan> Plans,
	TFunctionRef<void(UControlRigBlueprint&)> Extra)
{
	UControlRigBlueprint* Blueprint = AssetDocumentControlRigTests::MakeControlRig(Test, Target, Mesh, true);
	if (Blueprint == nullptr || !Test.TestNotNull(TEXT("modular rig controller"), Blueprint->GetModularRigController()))
	{
		return nullptr;
	}
	// "Create Modular Rig" on the mesh (CreateControlRigFromSkeletalMeshOrSkeleton) sets the curve import source too.
	Blueprint->GetSourceCurveImport() = Blueprint->GetSourceHierarchyImport();
	for (const FModulePlan& Plan : Plans)
	{
		AddPlannedModule(Test, *Blueprint, Plan);
	}
	Extra(*Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	Test.TestTrue(TEXT("modular rig saved"), Save(*Blueprint));
	return Blueprint;
}

const FModulePlan BipedPlan[] = {
	{TEXT("Root"), nullptr, nullptr, TEXT("root")},
	{TEXT("Spine"), TEXT("Spine"), TEXT("Root"), TEXT("spine_01_socket")},
	{TEXT("Neck"), TEXT("Neck"), TEXT("Spine"), TEXT("neck_socket")},
	{TEXT("Arm_l"), TEXT("Arm"), TEXT("Spine"), TEXT("shoulder_l")},
	{TEXT("Arm_r"), TEXT("Arm"), TEXT("Spine"), TEXT("shoulder_r")},
	{TEXT("Leg_l"), TEXT("Leg"), TEXT("Spine"), TEXT("thigh_l_socket")},
	{TEXT("Leg_r"), TEXT("Leg"), TEXT("Spine"), TEXT("thigh_r_socket")},
	{TEXT("Foot_l"), TEXT("Foot"), TEXT("Leg_l"), TEXT("Leg_l/")},
	{TEXT("Foot_r"), TEXT("Foot"), TEXT("Leg_r"), TEXT("Leg_r/")},
	{TEXT("Thumb_l"), TEXT("Finger"), TEXT("Arm_l"), TEXT("thumb")},
	{TEXT("Index_l"), TEXT("Finger"), TEXT("Arm_l"), TEXT("index")},
	{TEXT("Middle_l"), TEXT("Finger"), TEXT("Arm_l"), TEXT("middle")},
	{TEXT("Ring_l"), TEXT("Finger"), TEXT("Arm_l"), TEXT("ring")},
	{TEXT("Pinky_l"), TEXT("Finger"), TEXT("Arm_l"), TEXT("pinky")},
};

// The module Details panel's config edit: an override of the module class's property, set from its text.
bool SetConfig(UControlRigBlueprint& Blueprint, const TCHAR* Module, const TCHAR* Path, const TCHAR* Value)
{
	const FRigModuleReference* Reference = Blueprint.GetModularRigModel().FindModule(Module);
	UClass* Class = Reference != nullptr ? UE::AssetDocument::Graph::Rig::ControlRig::LoadModuleClass(*Reference) : nullptr;
	if (Class == nullptr)
	{
		return false;
	}
	const FControlRigOverrideValue Override(Path, Class, FString(Value), Reference->Name);
	return Override.IsValid() && Blueprint.GetModularRigController()->SetConfigValueInModule(Reference->Name, Override, false);
}

// Design §6.1: CR_Modular_Biped, with one config override and one variable binding.
UControlRigBlueprint* MakeBiped(FAutomationTestBase& Test, const FTarget& Target)
{
	return BuildModular(Test, Target, BipedMesh, BipedPlan, [&Test](UControlRigBlueprint& Blueprint)
	{
		UModularRigController* Controller = Blueprint.GetModularRigController();
		Test.TestTrue(TEXT("config override"), SetConfig(Blueprint, TEXT("Spine"), TEXT("Body Control Adjustment"), TEXT("1.5")));
		URigVMBlueprint& RigVM = Blueprint;
		Test.TestEqual(TEXT("host variable"), RigVM.AddMemberVariable(TEXT("UseIK"), TEXT("bool"), true, false, TEXT("true")), FName(TEXT("UseIK")));
		FText Reason;
		if (Test.TestTrue(TEXT("binding allowed"), Controller->CanBindModuleVariable(TEXT("Arm_l"), TEXT("FK IK Default"), TEXT("UseIK"), Reason)))
		{
			Test.TestTrue(TEXT("variable binding"), Controller->BindModuleVariable(TEXT("Arm_l"), TEXT("FK IK Default"), TEXT("UseIK"), false));
		}
		else
		{
			Test.AddInfo(Reason.ToString());
		}
	});
}

const FModulePlan CarPlan[] = {
	{TEXT("Root"), nullptr, nullptr, TEXT("Root")},
	{TEXT("Wheel_FL"), TEXT("Wheel"), TEXT("Root"), TEXT("wheel_fr_l")},
	{TEXT("Wheel_FR"), TEXT("Wheel"), TEXT("Root"), TEXT("wheel_fr_r")},
	{TEXT("Wheel_BL"), TEXT("Wheel"), TEXT("Root"), TEXT("wheel_bk_l")},
	{TEXT("Wheel_BR"), TEXT("Wheel"), TEXT("Root"), TEXT("wheel_bk_r")},
	{TEXT("Suspension"), TEXT("Suspension"), TEXT("Root"), TEXT("wheel_fr_suspension_l")},
	{TEXT("Piston"), TEXT("Piston"), TEXT("Root"), TEXT("Damper_FL")},
};

UControlRigBlueprint* MakeCar(FAutomationTestBase& Test, const FTarget& Target)
{
	return BuildModular(Test, Target, CarMesh, CarPlan, [](UControlRigBlueprint&) {});
}

// The module model as the Module Assembly panel shows it: modules in order (parent, class, overrides, bindings) and
// connections, sorted.
FString ModelText(UControlRigBlueprint& Blueprint)
{
	TArray<FString> Lines;
	const FModularRigModel& Model = Blueprint.GetModularRigModel();
	for (const FRigModuleReference& Module : Model.Modules)
	{
		TArray<FString> Overrides;
		for (const FControlRigOverrideValue& Override : Module.ConfigOverrides)
		{
			// The value (its data, exported), and the text the override keeps for it (what it saves) must agree.
			FString Value = Override.ToString();
			if (const FProperty* Leaf = Override.GetLeafProperty(); Leaf != nullptr && Override.GetData() != nullptr)
			{
				FString Exported;
				Leaf->ExportText_Direct(Exported, Override.GetData(), nullptr, nullptr, PPF_None);
				const FControlRigOverrideValue FromText(Override.GetPath(), UE::AssetDocument::Graph::Rig::ControlRig::FindModuleClass(Module), Override.ToString(), Module.Name);
				Value = FromText.IsValid() && FromText.IdenticalValue(Override.GetData()) ? Exported
					: FString::Printf(TEXT("%s (kept text '%s')"), *Exported, *Override.ToString());
			}
			Overrides.Add(Override.GetPath() + TEXT("=") + Value);
		}
		Overrides.Sort();
		TArray<FString> Bindings;
		for (const TPair<FName, FString>& Binding : Module.Bindings)
		{
			Bindings.Add(Binding.Key.ToString() + TEXT("<-") + Binding.Value);
		}
		Bindings.Sort();
		Lines.Add(FString::Printf(TEXT("module %s parent=%s class=%s config{%s} bindings{%s}"), *Module.Name.ToString(), *Module.ParentModuleName.ToString(),
			*UE::AssetDocument::Graph::Rig::ControlRig::GetModuleClassPath(Module).ToString(), *FString::Join(Overrides, TEXT(";")), *FString::Join(Bindings, TEXT(";"))));
	}
	TArray<FString> Connections;
	for (const FModularRigSingleConnection& Connection : Model.Connections.GetConnectionList())
	{
		Connections.Add(FString::Printf(TEXT("connect %s -> %s"), *Connection.Connector.ToString(),
			*FString::JoinBy(Connection.Targets, TEXT(","), [](const FRigElementKey& Key) { return Key.ToString(); })));
	}
	Connections.Sort();
	Lines.Append(Connections);
	Lines.Add(FString::Printf(TEXT("auto resolve %d"), Blueprint.GetModularRigSettings().bAutoResolve ? 1 : 0));
	return FString::Join(Lines, TEXT("\n"));
}

// The runtime pose (§6.2 runtime): a new instance of the compiled rig, constructed and run once; every bone, null,
// control and socket's global transform and curve value, by key.
TMap<FString, TArray<double>> PoseOf(UControlRigBlueprint& Blueprint)
{
	TMap<FString, TArray<double>> Pose;
	UPackage* Package = Blueprint.GetOutermost();
	const bool bWasDirty = Package->IsDirty();
	FKismetEditorUtilities::CompileBlueprint(&Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
	Package->SetDirtyFlag(bWasDirty);
	UClass* Class = Blueprint.GeneratedClass;
	if (Class == nullptr || !Class->IsChildOf(UControlRig::StaticClass()))
	{
		return Pose;
	}
	TStrongObjectPtr<UControlRig> Rig(NewObject<UControlRig>(GetTransientPackage(), Class, NAME_None, RF_Transient));
	Rig->Initialize(true);
	Rig->RequestConstruction();
	Rig->Evaluate_AnyThread();
	Rig->Evaluate_AnyThread();
	URigHierarchy* Hierarchy = Rig->GetHierarchy();
	for (const FRigElementKey& Key : Hierarchy != nullptr ? Hierarchy->GetAllKeys(true) : TArray<FRigElementKey>())
	{
		if (Key.Type == ERigElementType::Curve)
		{
			Pose.Add(Key.ToString(), {static_cast<double>(Hierarchy->GetCurveValue(Key))});
		}
		else if (Key.Type == ERigElementType::Bone || Key.Type == ERigElementType::Null || Key.Type == ERigElementType::Control
			|| Key.Type == ERigElementType::Socket)
		{
			const FTransform Global = Hierarchy->GetGlobalTransform(Key);
			FQuat Q = Global.GetRotation();
			if (Q.W < 0.0)
			{
				Q = FQuat(-Q.X, -Q.Y, -Q.Z, -Q.W);
			}
			const FVector T = Global.GetTranslation();
			const FVector S = Global.GetScale3D();
			Pose.Add(Key.ToString(), {T.X, T.Y, T.Z, Q.X, Q.Y, Q.Z, Q.W, S.X, S.Y, S.Z});
		}
	}
	return Pose;
}

bool ComparePose(FAutomationTestBase& Test, const FString& Stage, const TMap<FString, TArray<double>>& Expected, const TMap<FString, TArray<double>>& Got)
{
	TArray<FString> Problems;
	for (const TPair<FString, TArray<double>>& Entry : Expected)
	{
		const TArray<double>* Other = Got.Find(Entry.Key);
		if (Other == nullptr)
		{
			Problems.Add(FString::Printf(TEXT("missing %s"), *Entry.Key));
			continue;
		}
		for (int32 Index = 0; Index < Entry.Value.Num() && Index < Other->Num(); ++Index)
		{
			if (FMath::Abs(Entry.Value[Index] - (*Other)[Index]) > 0.01)
			{
				Problems.Add(FString::Printf(TEXT("%s[%d] %.4f vs %.4f"), *Entry.Key, Index, Entry.Value[Index], (*Other)[Index]));
				break;
			}
		}
	}
	for (const TPair<FString, TArray<double>>& Entry : Got)
	{
		if (!Expected.Contains(Entry.Key))
		{
			Problems.Add(FString::Printf(TEXT("extra %s"), *Entry.Key));
		}
	}
	Test.AddInfo(FString::Printf(TEXT("%s: %d elements in the pose"), *Stage, Expected.Num()));
	if (!Test.TestEqual(*FString::Printf(TEXT("%s: equal runtime pose"), *Stage), Problems.Num(), 0))
	{
		Problems.SetNum(FMath::Min(Problems.Num(), 12));
		Test.AddInfo(FString::Join(Problems, TEXT("; ")));
		return false;
	}
	return Expected.Num() > 0;
}

bool CompareModel(FAutomationTestBase& Test, const FString& Stage, UControlRigBlueprint* Expected, UControlRigBlueprint* Got)
{
	if (Expected == nullptr || Got == nullptr)
	{
		return Test.TestTrue(*FString::Printf(TEXT("%s: both rigs loaded"), *Stage), false);
	}
	const FString ExpectedText = ModelText(*Expected);
	const FString GotText = ModelText(*Got);
	if (!Test.TestEqual(*FString::Printf(TEXT("%s: equal module model"), *Stage), GotText, ExpectedText))
	{
		Test.AddInfo(FirstDifference(ExpectedText, GotText));
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> ModuleEntry(const TSharedPtr<FJsonObject>& Body, const TCHAR* Name)
{
	const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
	if (Body.IsValid() && Body->TryGetArrayField(TEXT("Modules"), Modules))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Modules)
		{
			if (Value->AsObject()->GetStringField(TEXT("Name")) == Name)
			{
				return Value->AsObject();
			}
		}
	}
	return MakeShared<FJsonObject>();
}

TSharedPtr<FJsonObject> ConnectionsOf(const TSharedPtr<FJsonObject>& Document)
{
	const TSharedPtr<FJsonObject> Body = BodyOf(Document);
	if (!Body->HasTypedField<EJson::Object>(TEXT("Connections")))
	{
		Body->SetObjectField(TEXT("Connections"), MakeShared<FJsonObject>());
	}
	return Body->GetObjectField(TEXT("Connections"));
}

// Inspect's ModularRigSuggestions.AutoConnect accepted: its connections copied into Body.Connections. Returns how many.
int32 AcceptAutoConnect(FAutomationTestBase& Test, const FTarget& Target, const TSharedPtr<FJsonObject>& Document)
{
	const double Start = FPlatformTime::Seconds();
	const FAssetDocumentResult Inspected = Inspect(Target.ObjectPath);
	Test.AddInfo(FString::Printf(TEXT("Inspect %.0f ms"), Milliseconds(Start)));
	const TSharedPtr<FJsonObject>* Suggestions = nullptr;
	if (!Test.TestTrue(TEXT("Inspect has ModularRigSuggestions"), Inspected.IsSuccess() && Inspected.Payload.IsValid()
		&& Inspected.Payload->TryGetObjectField(TEXT("ModularRigSuggestions"), Suggestions)))
	{
		Test.AddInfo(DiagnosticsText(Inspected));
		return 0;
	}
	Test.AddInfo(FString::Printf(TEXT("ModularRigSuggestions: %s"), *Serialize(*Suggestions).Left(1500)));
	const TSharedPtr<FJsonObject> Connections = ConnectionsOf(Document);
	int32 Count = 0;
	for (const auto& Entry : (*Suggestions)->GetObjectField(TEXT("AutoConnect"))->Values)
	{
		Connections->SetField(Entry.Key, Entry.Value);
		++Count;
	}
	return Count;
}
} // namespace AssetDocumentControlRigModularTests

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigModularFidelityTest, "UEAssetDoc.ControlRig.ModularFidelity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigModularFidelityTest::RunTest(const FString& Parameters)
{
	const AssetDocumentIKRigTests::FModules56RootScope Modules56Root;
	using namespace AssetDocumentControlRigModularTests;
	ExpectFingerErrors(*this);
	FFixtures Fixtures;
	TArray<TPair<FString, const FTarget*>> Sources;
	double Start = FPlatformTime::Seconds();
	const FTarget& Biped = Fixtures.Add(TEXT("CR_Modular_Biped"));
	if (MakeBiped(*this, Biped) != nullptr)
	{
		AddInfo(FString::Printf(TEXT("CR_Modular_Biped built with the editor's calls in %.0f ms"), Milliseconds(Start)));
		Sources.Emplace(TEXT("CR_Modular_Biped"), &Biped);
	}
	Start = FPlatformTime::Seconds();
	const FTarget& Car = Fixtures.Add(TEXT("CR_Modular_Car"));
	if (MakeCar(*this, Car) != nullptr)
	{
		AddInfo(FString::Printf(TEXT("CR_Modular_Car built with the editor's calls in %.0f ms"), Milliseconds(Start)));
		Sources.Emplace(TEXT("CR_Modular_Car"), &Car);
	}
	for (const TPair<FString, const FTarget*>& Source : Sources)
	{
		const FString& Name = Source.Key;
		TestNotNull(*FString::Printf(TEXT("%s: reload"), *Name), Reload(*Source.Value));
		Start = FPlatformTime::Seconds();
		const FAssetDocumentResult Original = ExtractResult(Source.Value->ObjectPath);
		const double ExtractMs = Milliseconds(Start);
		if (!TestTrue(*FString::Printf(TEXT("%s extracts"), *Name), Original.IsSuccess() && Original.Payload.IsValid()))
		{
			continue;
		}
		AddInfo(FString::Printf(TEXT("%s document: %s"), *Name, *SerializePretty(BodyOf(Original.Payload)).Left(6000)));
		FString DiffText;
		TestEqual(*FString::Printf(TEXT("%s: Diff of the extraction is empty"), *Name), DiffCount(Original.Payload, DiffText), 0);
		const FTarget& Created = Fixtures.Add(TEXT("CR_ModularRecreated"));
		Start = FPlatformTime::Seconds();
		FAssetDocumentResult Applied;
		if (!ApplyOk(*this, Created, ForTarget(Original.Payload, Created, TEXT("Create")), Name + TEXT(" Create"), &Applied))
		{
			continue;
		}
		const double CreateMs = Milliseconds(Start);
		const FAssetDocumentResult Recreated = ExtractResult(Created.ObjectPath);
		if (!TestEqual(*FString::Printf(TEXT("%s: the created rig extracts as the source"), *Name), WithoutTarget(Recreated.Payload),
			WithoutTarget(Original.Payload)))
		{
			AddInfo(FirstDifference(WithoutTarget(Original.Payload), WithoutTarget(Recreated.Payload)));
		}
		UControlRigBlueprint* SourceRig = Cast<UControlRigBlueprint>(Source.Value->Get());
		UControlRigBlueprint* CreatedRig = Cast<UControlRigBlueprint>(Created.Get());
		CompareModel(*this, Name, SourceRig, CreatedRig);
		if (SourceRig != nullptr && CreatedRig != nullptr)
		{
			ComparePose(*this, Name, PoseOf(*SourceRig), PoseOf(*CreatedRig));
		}
		TestEqual(*FString::Printf(TEXT("%s: compile status"), *Name), StatusName(Recreated), StatusName(Original));
		Start = FPlatformTime::Seconds();
		const FAssetDocumentResult Again = Apply(Created, ForTarget(Recreated.Payload, Created, TEXT("Update")));
		const double ReapplyMs = Milliseconds(Start);
		TestTrue(*FString::Printf(TEXT("%s: unchanged re-apply succeeds (%s)"), *Name, *DiagnosticsText(Again)), Again.IsSuccess());
		TestFalse(*FString::Printf(TEXT("%s: unchanged re-apply is a no-op"), *Name), Again.bSavedAsset);
		// An Update that touches one module (a config value).
		const TSharedPtr<FJsonObject> Changed = ForTarget(Recreated.Payload, Created, TEXT("Update"));
		const TArray<TSharedPtr<FJsonValue>> Modules = BodyOf(Changed)->GetArrayField(TEXT("Modules"));
		const TSharedPtr<FJsonObject> Last = Modules.Last()->AsObject();
		TSharedPtr<FJsonObject> Config = Last->HasField(TEXT("Config")) ? Last->GetObjectField(TEXT("Config")) : MakeShared<FJsonObject>();
		Config->SetBoolField(Name.Contains(TEXT("Biped")) ? TEXT("Override Control Settings") : TEXT("Debug"), true);
		Last->SetObjectField(TEXT("Config"), Config);
		Start = FPlatformTime::Seconds();
		FAssetDocumentResult Updated;
		const bool bUpdated = ApplyOk(*this, Created, Changed, Name + TEXT(" Update (one config value)"), &Updated);
		const double UpdateMs = Milliseconds(Start);
		if (bUpdated)
		{
			const FString AfterUpdate = Comparable(Extract(*this, Created.ObjectPath, Name + TEXT(" after update")));
			if (!TestEqual(*FString::Printf(TEXT("%s: the update is the document"), *Name), AfterUpdate, Comparable(Changed)))
			{
				AddInfo(FirstDifference(Comparable(Changed), AfterUpdate));
			}
		}
		const FString Settled = WithoutTarget(ExtractResult(Created.ObjectPath).Payload);
		Start = FPlatformTime::Seconds();
		TestNotNull(*FString::Printf(TEXT("%s: reload"), *Name), Reload(Created));
		const double ReloadMs = Milliseconds(Start);
		TestEqual(*FString::Printf(TEXT("%s: equal after reload"), *Name), WithoutTarget(ExtractResult(Created.ObjectPath).Payload), Settled);
		AddInfo(FString::Printf(TEXT("PASS %s: extract %.0f ms, Create %.0f ms, unchanged re-apply %.0f ms, Update %.0f ms, reload %.0f ms, %d chars, status %s%s"),
			*Name, ExtractMs, CreateMs, ReapplyMs, UpdateMs, ReloadMs, Serialize(Original.Payload).Len(), *StatusName(Recreated),
			Applied.Diagnostics.IsEmpty() ? TEXT("") : *(TEXT("; apply notes: ") + DiagnosticsText(Applied).Left(400))));
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigModularFreeEditsTest, "UEAssetDoc.ControlRig.ModularFreeEdits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigModularFreeEditsTest::RunTest(const FString& Parameters)
{
	const AssetDocumentIKRigTests::FModules56RootScope Modules56Root;
	using namespace AssetDocumentControlRigModularTests;
	ExpectFingerErrors(*this);
	FFixtures Fixtures;
	const FTarget& Base = Fixtures.Add(TEXT("CR_Modular_Biped"));
	UControlRigBlueprint* BaseRig = MakeBiped(*this, Base);
	if (BaseRig == nullptr)
	{
		return false;
	}
	const FTarget& ByEditor = Fixtures.Add(TEXT("CR_ModularEditor"));
	const FTarget& ByDocument = Fixtures.Add(TEXT("CR_ModularDocument"));
	UControlRigBlueprint* EditorRig = Cast<UControlRigBlueprint>(Duplicate(*this, BaseRig, ByEditor));
	if (!TestNotNull(TEXT("editor duplicate"), EditorRig) || !TestNotNull(TEXT("document duplicate"), Duplicate(*this, BaseRig, ByDocument)))
	{
		return false;
	}
	const FString OldNeck = TEXT("/ControlRigModules/Modules/Neck.Neck");

	// The Module Assembly panel: delete Arm_r, drop an Arm on the right shoulder, change a Spine setting, bind the new
	// arm's FK/IK variable, swap Neck to the pre-5.6 Neck, move Leg_r before Leg_l.
	double Start = FPlatformTime::Seconds();
	UModularRigController* Controller = EditorRig->GetModularRigController();
	Controller->DeleteModule(TEXT("Arm_r"), false);
	TestNull(TEXT("Arm_r deleted"), Controller->FindModule(TEXT("Arm_r")));
	FString MirrorTarget;
	TestEqual(TEXT("Arm_mirror added"), Controller->AddModule(TEXT("Arm_mirror"), LoadModuleClass(ModuleAsset(TEXT("Arm"))), TEXT("Spine"), false), FName(TEXT("Arm_mirror")));
	ConnectPrimary(*this, *EditorRig, TEXT("Arm_mirror"), TEXT("shoulder_r"), &MirrorTarget);
	TestTrue(TEXT("config value"), SetConfig(*EditorRig, TEXT("Spine"), TEXT("Body Control Adjustment"), TEXT("2.5")));
	TestTrue(TEXT("binding"), Controller->BindModuleVariable(TEXT("Arm_mirror"), TEXT("FK IK Default"), TEXT("UseIK"), false));
	TestTrue(TEXT("swap Neck"), Controller->SwapModuleClass(TEXT("Neck"), LoadModuleClass(OldNeck), false));
	// ReorderModule's index is among the module's siblings (Spine's children in model order).
	TArray<FName> SpineChildren;
	for (const FRigModuleReference& Each : EditorRig->GetModularRigModel().Modules)
	{
		if (Each.ParentModuleName == TEXT("Spine"))
		{
			SpineChildren.Add(Each.Name);
		}
	}
	const int32 LegLIndex = SpineChildren.IndexOfByKey(FName(TEXT("Leg_l")));
	AddInfo(FString::Printf(TEXT("Spine's children: %s"), *FString::JoinBy(SpineChildren, TEXT(" "), [](const FName& Name) { return Name.ToString(); })));
	TestTrue(TEXT("reorder Leg_r"), LegLIndex != INDEX_NONE && Controller->ReorderModule(TEXT("Leg_r"), LegLIndex, false));
	FKismetEditorUtilities::CompileBlueprint(EditorRig);
	TestTrue(TEXT("editor edits saved"), Save(*EditorRig));
	const double EditorMs = Milliseconds(Start);

	// The same through documents: the module list and primary connection, then AutoConnect accepted.
	const TSharedPtr<FJsonObject> Before = Extract(*this, ByDocument.ObjectPath, TEXT("document side before"));
	const TSharedPtr<FJsonObject> Edited = ForTarget(Before, ByDocument, TEXT("Update"));
	const TSharedPtr<FJsonObject> Body = BodyOf(Edited);
	TArray<TSharedPtr<FJsonValue>> Modules;
	TSharedPtr<FJsonValue> LegR;
	int32 LegLSlot = INDEX_NONE;
	for (const TSharedPtr<FJsonValue>& Value : Body->GetArrayField(TEXT("Modules")))
	{
		const FString Name = Value->AsObject()->GetStringField(TEXT("Name"));
		if (Name == TEXT("Arm_r"))
		{
			continue;
		}
		if (Name == TEXT("Leg_r"))
		{
			LegR = Value;
			continue;
		}
		if (Name == TEXT("Leg_l"))
		{
			LegLSlot = Modules.Num();
		}
		Modules.Add(Value);
	}
	if (TestTrue(TEXT("Leg_l and Leg_r listed"), LegR.IsValid() && LegLSlot != INDEX_NONE))
	{
		Modules.Insert(LegR, LegLSlot);
	}
	const TSharedPtr<FJsonObject> Mirror = Json(R"J({"Name": "Arm_mirror", "Parent": "Spine", "Bindings": {"FK IK Default": "UseIK"}})J");
	Mirror->SetStringField(TEXT("Class"), ModuleClassPath(TEXT("Arm")));
	Modules.Add(MakeShared<FJsonValueObject>(Mirror));
	Body->SetArrayField(TEXT("Modules"), Modules);
	const TSharedPtr<FJsonObject> SpineEntry = ModuleEntry(Body, TEXT("Spine"));
	TSharedPtr<FJsonObject> SpineConfig = SpineEntry->HasTypedField<EJson::Object>(TEXT("Config")) ? SpineEntry->GetObjectField(TEXT("Config")) : MakeShared<FJsonObject>();
	SpineConfig->SetNumberField(TEXT("Body Control Adjustment"), 2.5);
	SpineEntry->SetObjectField(TEXT("Config"), SpineConfig);
	ModuleEntry(Body, TEXT("Neck"))->SetStringField(TEXT("Class"), OldNeck + TEXT("_C"));
	const TSharedPtr<FJsonObject> Connections = ConnectionsOf(Edited);
	TArray<FString> Keys;
	AssetDocumentServerJson::GetKeys(*Connections, Keys);
	for (const FString& Key : Keys)
	{
		if (Key.StartsWith(TEXT("Arm_r/")))
		{
			Connections->RemoveField(Key);
		}
	}
	Connections->SetStringField(TEXT("Arm_mirror/Arm Primary"), MirrorTarget);
	Start = FPlatformTime::Seconds();
	if (!ApplyOk(*this, ByDocument, Edited, TEXT("modular document edit")))
	{
		return false;
	}
	const double DocumentMs = Milliseconds(Start);
	const TSharedPtr<FJsonObject> Accepting = ForTarget(Extract(*this, ByDocument.ObjectPath, TEXT("after the module edit")), ByDocument, TEXT("Update"));
	const int32 Accepted = AcceptAutoConnect(*this, ByDocument, Accepting);
	TestTrue(TEXT("AutoConnect suggests the new arm's secondary connectors"), Accepted > 0);
	Start = FPlatformTime::Seconds();
	ApplyOk(*this, ByDocument, Accepting, TEXT("AutoConnect accepted"));
	const double AcceptMs = Milliseconds(Start);

	const FString EditorState = Comparable(Extract(*this, ByEditor.ObjectPath, TEXT("editor side")));
	const FString DocumentState = Comparable(Extract(*this, ByDocument.ObjectPath, TEXT("document side")));
	if (!TestEqual(TEXT("document edits equal the editor's"), DocumentState, EditorState))
	{
		AddInfo(FirstDifference(EditorState, DocumentState));
	}
	UControlRigBlueprint* DocumentRig = Cast<UControlRigBlueprint>(ByDocument.Get());
	EditorRig = Cast<UControlRigBlueprint>(ByEditor.Get());
	CompareModel(*this, TEXT("free edits"), EditorRig, DocumentRig);
	if (EditorRig != nullptr && DocumentRig != nullptr)
	{
		ComparePose(*this, TEXT("free edits"), PoseOf(*EditorRig), PoseOf(*DocumentRig));
	}
	AddInfo(FString::Printf(TEXT("editor calls %.0f ms; document apply %.0f ms; AutoConnect accept apply %.0f ms"), EditorMs, DocumentMs, AcceptMs));
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigModularCreateTest, "UEAssetDoc.ControlRig.ModularCreate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigModularCreateTest::RunTest(const FString& Parameters)
{
	const AssetDocumentIKRigTests::FModules56RootScope Modules56Root;
	using namespace AssetDocumentControlRigModularTests;
	FFixtures Fixtures;
	// The editor: a modular rig on the biped mesh, Spine dropped on Root's spine socket, Neck on Spine's neck socket.
	const FTarget& EditorBuilt = Fixtures.Add(TEXT("CR_ModularEditorBuilt"));
	const FModulePlan Plan[] = {BipedPlan[0], BipedPlan[1], BipedPlan[2]};
	UControlRigBlueprint* EditorRig = BuildModular(*this, EditorBuilt, BipedMesh, Plan, [](UControlRigBlueprint&) {});
	if (EditorRig == nullptr)
	{
		return false;
	}
	// The AI: a document naming the mesh (FactoryContext), the modules and the primary connections it knows.
	const FTarget& Target = Fixtures.Add(TEXT("CR_ModularFromScratch"));
	const TSharedPtr<FJsonObject> Document = Json(R"J({"SchemaVersion": 1, "Action": "Create", "Body": {"RigType": "Modular",
		"Modules": [{"Name": "Root", "Class": "/ControlRig/Modules/Modules56/Root.Root_C"},
			{"Name": "Spine", "Class": "/ControlRigModules/Modules56/Spine.Spine_C", "Parent": "Root"},
			{"Name": "Neck", "Class": "/ControlRigModules/Modules56/Neck.Neck_C", "Parent": "Spine"}],
		"Connections": {"Root/Root": "Bone(root)", "Spine/Spine Primary": "Socket(Root/spine_01_socket)"}}})J");
	Document->SetStringField(TEXT("Class"), ControlRigClassPath);
	Document->SetStringField(TEXT("Target"), Target.PackageName);
	Document->SetStringField(TEXT("FactoryContext"), BipedMesh);
	// Validate first (a trial on a transient rig): no asset is made.
	{
		FAssetDocumentValidateRequest Request;
		Request.Document = Document;
		const FAssetDocumentResult Validated = FAssetDocumentModule::Get().GetService().Validate(Request);
		TestTrue(*FString::Printf(TEXT("validate of the modular create (%s)"), *DiagnosticsText(Validated)), Validated.IsSuccess());
		TestNull(TEXT("validate made no asset"), Target.Get());
	}
	double Start = FPlatformTime::Seconds();
	if (!ApplyOk(*this, Target, Document, TEXT("modular create")))
	{
		return false;
	}
	const double CreateMs = Milliseconds(Start);
	// Rounds of Inspect: accept AutoConnect (the secondary connectors); Neck's primary connector targets a socket Spine
	// makes once its own connectors resolve, so it shows up among the Candidates in a later round.
	double AcceptMs = 0.0;
	bool bNeckOffered = false;
	for (int32 Round = 0; Round < 4; ++Round)
	{
		const TSharedPtr<FJsonObject> Accepting = ForTarget(Extract(*this, Target.ObjectPath, TEXT("round")), Target, TEXT("Update"));
		const double InspectStart = FPlatformTime::Seconds();
		const FAssetDocumentResult Inspected = Inspect(Target.ObjectPath);
		AddInfo(FString::Printf(TEXT("round %d Inspect %.0f ms"), Round, Milliseconds(InspectStart)));
		const TSharedPtr<FJsonObject>* Suggestions = nullptr;
		if (!TestTrue(TEXT("Inspect suggestions"), Inspected.Payload.IsValid() && Inspected.Payload->TryGetObjectField(TEXT("ModularRigSuggestions"), Suggestions)))
		{
			break;
		}
		AddInfo(FString::Printf(TEXT("round %d: %s"), Round, *Serialize(*Suggestions).Left(2000)));
		int32 Added = 0;
		for (const auto& Entry : (*Suggestions)->GetObjectField(TEXT("AutoConnect"))->Values)
		{
			ConnectionsOf(Accepting)->SetField(Entry.Key, Entry.Value);
			++Added;
		}
		const TSharedPtr<FJsonObject>* Candidates = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* NeckTargets = nullptr;
		if ((*Suggestions)->TryGetObjectField(TEXT("Candidates"), Candidates) && (*Candidates)->TryGetArrayField(TEXT("Neck/Neck Primary"), NeckTargets)
			&& NeckTargets->ContainsByPredicate([](const TSharedPtr<FJsonValue>& Value) { return Value->AsString() == TEXT("Socket(Spine/neck_socket)"); }))
		{
			ConnectionsOf(Accepting)->SetStringField(TEXT("Neck/Neck Primary"), TEXT("Socket(Spine/neck_socket)"));
			bNeckOffered = true;
			++Added;
		}
		if (Added == 0)
		{
			break;
		}
		Start = FPlatformTime::Seconds();
		if (!ApplyOk(*this, Target, Accepting, FString::Printf(TEXT("round %d accepted"), Round)))
		{
			break;
		}
		AcceptMs += Milliseconds(Start);
	}
	TestTrue(TEXT("Neck's primary connector was offered Spine's neck socket"), bNeckOffered);
	const FString Made = Comparable(Extract(*this, Target.ObjectPath, TEXT("from scratch")));
	const FString Expected = Comparable(Extract(*this, EditorBuilt.ObjectPath, TEXT("editor built")));
	if (!TestEqual(TEXT("the from-scratch rig equals the editor-built one"), Made, Expected))
	{
		AddInfo(FirstDifference(Expected, Made));
	}
	UControlRigBlueprint* MadeRig = Cast<UControlRigBlueprint>(Target.Get());
	UControlRigBlueprint* BuiltRig = Cast<UControlRigBlueprint>(EditorBuilt.Get());
	CompareModel(*this, TEXT("from scratch"), BuiltRig, MadeRig);
	if (MadeRig != nullptr && BuiltRig != nullptr)
	{
		ComparePose(*this, TEXT("from scratch"), PoseOf(*BuiltRig), PoseOf(*MadeRig));
	}
	AddInfo(FString::Printf(TEXT("from scratch: Create %.0f ms, AutoConnect accept %.0f ms; document %s"), CreateMs, AcceptMs, *Made.Left(3000)));
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigModularRollbackTest, "UEAssetDoc.ControlRig.ModularRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigModularRollbackTest::RunTest(const FString& Parameters)
{
	const AssetDocumentIKRigTests::FModules56RootScope Modules56Root;
	using namespace AssetDocumentControlRigModularTests;
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("CR_ModularRollback"));
	const FModulePlan Plan[] = {BipedPlan[0], BipedPlan[1], BipedPlan[2], BipedPlan[3]};
	if (BuildModular(*this, Target, BipedMesh, Plan, [](UControlRigBlueprint&) {}) == nullptr)
	{
		return false;
	}
	const TSharedPtr<FJsonObject> State = Extract(*this, Target.ObjectPath, TEXT("before"));

	// 1. A module added and configured, then a connection to an element the rig does not have: all rolled back; the
	// refusal lists close element names.
	{
		const TSharedPtr<FJsonObject> Failing = ForTarget(State, Target, TEXT("Update"));
		TArray<TSharedPtr<FJsonValue>> Modules = BodyOf(Failing)->GetArrayField(TEXT("Modules"));
		const TSharedPtr<FJsonObject> Added = Json(R"J({"Name": "Arm_r", "Parent": "Spine", "Config": {"Enable Softness": true}})J");
		Added->SetStringField(TEXT("Class"), ModuleClassPath(TEXT("Arm")));
		Modules.Add(MakeShared<FJsonValueObject>(Added));
		BodyOf(Failing)->SetArrayField(TEXT("Modules"), Modules);
		ConnectionsOf(Failing)->SetStringField(TEXT("Arm_r/Arm Primary"), TEXT("Socket(Spine/shoulder_rr)"));
		const FAssetDocumentResult Refused = ExpectRefused(*this, Target, Failing, TEXT("ControlRig.ConnectionRefused"), TEXT("refused connection"));
		TestTrue(TEXT("the refusal lists the close socket"), DiagnosticsText(Refused).Contains(TEXT("Socket(Spine/shoulder_r)")));
	}
	// 2. Unknown module class, a class that is not a rig module, an unknown connector, an unknown parent, a cycle.
	{
		const TSharedPtr<FJsonObject> Failing = ForTarget(State, Target, TEXT("Update"));
		ModuleEntry(BodyOf(Failing), TEXT("Arm_l"))->SetStringField(TEXT("Class"), TEXT("/ControlRigModules/Modules56/Armm.Armm_C"));
		ExpectRefused(*this, Target, Failing, TEXT("ControlRig.ClassNotFound"), TEXT("unknown module class"));
	}
	{
		const TSharedPtr<FJsonObject> Failing = ForTarget(State, Target, TEXT("Update"));
		ModuleEntry(BodyOf(Failing), TEXT("Arm_l"))->SetStringField(TEXT("Class"), TEXT("/Script/ControlRig.ControlRig"));
		ExpectRefused(*this, Target, Failing, TEXT("ControlRig.ClassNotARigModule"), TEXT("not a rig module"));
	}
	{
		const TSharedPtr<FJsonObject> Failing = ForTarget(State, Target, TEXT("Update"));
		ConnectionsOf(Failing)->SetStringField(TEXT("Arm_l/Hand Bonee"), TEXT("Bone(hand_l)"));
		const FAssetDocumentResult Refused = ExpectRefused(*this, Target, Failing, TEXT("ControlRig.ConnectorNotFound"), TEXT("unknown connector"));
		TestTrue(TEXT("the refusal names the close connector"), DiagnosticsText(Refused).Contains(TEXT("Arm_l/Hand Bone")));
	}
	{
		const TSharedPtr<FJsonObject> Failing = ForTarget(State, Target, TEXT("Update"));
		ModuleEntry(BodyOf(Failing), TEXT("Neck"))->SetStringField(TEXT("Parent"), TEXT("Spin"));
		const FAssetDocumentResult Refused = ExpectRefused(*this, Target, Failing, TEXT("ControlRig.ModuleNotFound"), TEXT("unknown parent"));
		TestTrue(TEXT("the refusal names Spine"), DiagnosticsText(Refused).Contains(TEXT("Spine")));
	}
	{
		const TSharedPtr<FJsonObject> Failing = ForTarget(State, Target, TEXT("Update"));
		ModuleEntry(BodyOf(Failing), TEXT("Neck"))->SetStringField(TEXT("Parent"), TEXT("Neck"));
		ExpectRefused(*this, Target, Failing, TEXT("ControlRig.ModuleCycle"), TEXT("own parent"));
	}
	// 3. Refuse -> GC -> the same module edited again (the rolled-back module's objects are gone).
	{
		TSharedPtr<FJsonObject> Config = MakeShared<FJsonObject>();
		Config->SetBoolField(TEXT("Enable Softness"), true);
		const TSharedPtr<FJsonObject> Failing = ForTarget(State, Target, TEXT("Update"));
		ModuleEntry(BodyOf(Failing), TEXT("Arm_l"))->SetObjectField(TEXT("Config"), Config);
		ConnectionsOf(Failing)->SetStringField(TEXT("Arm_l/Hand Bone"), TEXT("Bone(hand_ll)"));
		ExpectRefused(*this, Target, Failing, TEXT("ControlRig.ConnectionRefused"), TEXT("refused edit of Arm_l"));
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		const TSharedPtr<FJsonObject> Again = ForTarget(State, Target, TEXT("Update"));
		ModuleEntry(BodyOf(Again), TEXT("Arm_l"))->SetObjectField(TEXT("Config"), Config);
		if (ApplyOk(*this, Target, Again, TEXT("Arm_l edited again after the rollback and GC")))
		{
			TestEqual(TEXT("the edit is the document"), Comparable(Extract(*this, Target.ObjectPath, TEXT("edited again"))), Comparable(Again));
		}
	}
	// 4. Undo history: a human module edit (one transaction, saved), an AI apply, a failed AI apply.
	if (GEditor != nullptr && GEditor->Trans != nullptr)
	{
		ReleaseUndoHistory();
		{
			UControlRigBlueprint* Rig = Cast<UControlRigBlueprint>(Target.Get());
			const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentControlRigModularTests", "HumanAdd", "Add Module"));
			TestEqual(TEXT("human adds a module"), Rig->GetModularRigController()->AddModule(TEXT("Leg_l"), LoadModuleClass(ModuleAsset(TEXT("Leg"))),
				TEXT("Spine"), true), FName(TEXT("Leg_l")));
		}
		TestTrue(TEXT("human save"), UEditorLoadingAndSavingUtils::SavePackages({Target.Get()->GetOutermost()}, false));
		TestTrue(TEXT("the human edit is in the undo history"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
		const TSharedPtr<FJsonObject> Human = Extract(*this, Target.ObjectPath, TEXT("after the human edit"));
		const TSharedPtr<FJsonObject> AiEdit = ForTarget(Human, Target, TEXT("Update"));
		ConnectionsOf(AiEdit)->SetStringField(TEXT("Leg_l/Leg Primary"), TEXT("Socket(Spine/thigh_l_socket)"));
		if (ApplyOk(*this, Target, AiEdit, TEXT("AI connects the human's module")))
		{
			TestFalse(TEXT("after the AI edit no undo history references the rig"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
			const FString AfterEdit = Serialize(Extract(*this, Target.ObjectPath, TEXT("after AI edit")));
			int32 Undone = 0;
			while (Undone < 16 && GEditor->Trans->CanUndo() && GEditor->UndoTransaction())
			{
				++Undone;
			}
			TestEqual(TEXT("undo cannot reach the rig"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after undo"))), AfterEdit);
			const TSharedPtr<FJsonObject> Failing = ForTarget(Parse(AfterEdit), Target, TEXT("Update"));
			ConnectionsOf(Failing)->SetStringField(TEXT("Leg_l/Foot Bone"), TEXT("Bone(foot_ll)"));
			ExpectRefused(*this, Target, Failing, TEXT("ControlRig.ConnectionRefused"), TEXT("failed AI apply after the human edit"));
		}
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigModularDependentsTest, "UEAssetDoc.ControlRig.ModularDependents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigModularDependentsTest::RunTest(const FString& Parameters)
{
	const AssetDocumentIKRigTests::FModules56RootScope Modules56Root;
	using namespace AssetDocumentControlRigModularTests;
	FFixtures Fixtures;
	// A rig module of the project (a duplicate of the Constraint module) and a modular rig that uses it.
	const FTarget& Module = Fixtures.Add(TEXT("CR_ProjectModule"));
	const FTarget& User = Fixtures.Add(TEXT("CR_ModuleUser"));
	UControlRigBlueprint* ModuleRig = Cast<UControlRigBlueprint>(Duplicate(*this, LoadQuiet(ModuleAsset(TEXT("Constraint"))), Module));
	if (!TestNotNull(TEXT("project module"), ModuleRig))
	{
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(ModuleRig);
	Save(*ModuleRig);
	UControlRigBlueprint* UserRig = AssetDocumentControlRigTests::MakeControlRig(*this, User, BipedMesh, true);
	if (UserRig == nullptr)
	{
		return false;
	}
	TestEqual(TEXT("the project module is used"), UserRig->GetModularRigController()->AddModule(TEXT("Constraint"),
		TSubclassOf<UControlRig>(ModuleRig->GeneratedClass.Get()), TEXT("Root"), false), FName(TEXT("Constraint")));
	FKismetEditorUtilities::CompileBlueprint(UserRig);
	TestTrue(TEXT("user saved"), Save(*UserRig));

	const TSharedPtr<FJsonObject> State = Extract(*this, Module.ObjectPath, TEXT("module"));
	if (State == nullptr)
	{
		return false;
	}
	auto WithVariable = [&State, &Module](const TCHAR* Name)
	{
		const TSharedPtr<FJsonObject> Edited = ForTarget(State, Module, TEXT("Update"));
		const TArray<TSharedPtr<FJsonValue>>* Existing = nullptr;
		TArray<TSharedPtr<FJsonValue>> Variables;
		if (BodyOf(Edited)->TryGetArrayField(TEXT("Variables"), Existing))
		{
			Variables = *Existing;
		}
		const TSharedPtr<FJsonObject> Variable = MakeShared<FJsonObject>();
		Variable->SetStringField(TEXT("Name"), Name);
		Variable->SetStringField(TEXT("Type"), TEXT("float"));
		Variables.Add(MakeShared<FJsonValueObject>(Variable));
		BodyOf(Edited)->SetArrayField(TEXT("Variables"), Variables);
		return Edited;
	};
	// 1. The user rig has unsaved changes: the module edit is refused.
	UserRig = Cast<UControlRigBlueprint>(User.Get());
	UserRig->MarkPackageDirty();
	const FAssetDocumentResult Refused = ExpectRefused(*this, Module, WithVariable(TEXT("Extra")), TEXT("ControlRig.DependentHasUnsavedChanges"),
		TEXT("module edit with an unsaved dependent"));
	TestTrue(TEXT("the refusal names the dependent"), DiagnosticsText(Refused).Contains(User.PackageName));
	// 2. Saved: the edit applies and notes the dependent.
	UserRig->GetOutermost()->SetDirtyFlag(false);
	FAssetDocumentResult Applied;
	if (ApplyOk(*this, Module, WithVariable(TEXT("Extra")), TEXT("module edit with a saved dependent"), &Applied))
	{
		TestTrue(*FString::Printf(TEXT("dependents note (%s)"), *DiagnosticsText(Applied)), HasCode(Applied, TEXT("ControlRig.DependentsUpdated")));
	}
	if (UObject* Dependent = User.Get())
	{
		AddInfo(FString::Printf(TEXT("dependent after the module compiled: %s"), Dependent->GetOutermost()->IsDirty() ? TEXT("dirty") : TEXT("clean")));
		Dependent->GetOutermost()->SetDirtyFlag(false);
	}
	return true;
}

#endif
