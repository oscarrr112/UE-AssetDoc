// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// UEAssetDoc.Niagara.Stack.*: module stack free edits (N3), document path vs the Niagara editor's own
// API on two duplicates of one system. The editor copy is edited with the stack view models the way the stack
// widgets do it; its extract (Set Parameters names masked) is applied to the second copy; both documents must be
// equal, the apply clears undo history that references the system, re-applying is a no-op and the saved package
// reloads to the same document.
//   EditorEquivalence  DirectionalBurst: a module added with the group's "+" (add action), a module deleted, a module
//                      moved (drag and drop), disabled, version changed, renamed, given a note; inputs: local, linked
//                      to a new user parameter (the link adds it), nested dynamic input, expression, data interface
//                      Details, reset; a Set Parameters module with a target and its value; event handler Details.
//   Stages             a GPU emitter: simulation stages added from the "+ Stage" menu, renamed, Details, moved (drag
//                      and drop), disabled, deleted, a module added inside a stage.
//   SetParameters      targets added to and removed from a saved Set Parameters module (the node regenerates its
//                      function script), and a failed edit of it rolls back in memory.
//   NestedInputs       inputs the module's parameter hierarchy nests under another input (InitializeParticle's
//                      "Lifetime Min" under "Lifetime Mode") are read and written like the others; a link to a
//                      user parameter of another type is refused (Niagara.LinkTypeMismatch).

#include "AssetDocumentNiagaraTestEditing.h"

#include "NiagaraDataInterfaceCurveBase.h"
#include "NiagaraMessages.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraSimulationStageBase.h"
#include "ViewModels/Stack/NiagaraStackEmitterPropertiesGroup.h"
#include "ViewModels/Stack/NiagaraStackObject.h"
#include "ViewModels/Stack/NiagaraStackScriptItemGroup.h"
#include "ViewModels/Stack/NiagaraStackSimulationStageGroup.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentNiagaraTests
{
namespace
{
UNiagaraStackEntry* RootOf(UNiagaraStackViewModel* Stack)
{
	UNiagaraStackEntry* Root = Stack != nullptr ? Stack->GetRootEntry() : nullptr;
	if (Root != nullptr)
	{
		Root->RefreshChildren();
	}
	return Root;
}

UNiagaraStackScriptItemGroup* ScriptGroup(UNiagaraStackEntry& Root, ENiagaraScriptUsage Usage)
{
	Root.RefreshChildren();
	TArray<UNiagaraStackScriptItemGroup*> Groups;
	Root.GetUnfilteredChildrenOfType(Groups);
	for (UNiagaraStackScriptItemGroup* Group : Groups)
	{
		if (Group->GetScriptOutputNode() != nullptr && Group->GetScriptOutputNode()->GetUsage() == Usage)
		{
			return Group;
		}
	}
	return nullptr;
}

UNiagaraStackModuleItem* ModuleNamed(UNiagaraStackEntry& Root, const FString& Name)
{
	Root.RefreshChildren();
	TArray<UNiagaraStackModuleItem*> Modules;
	Root.GetUnfilteredChildrenOfType(Modules, true);
	for (UNiagaraStackModuleItem* Module : Modules)
	{
		if (Module->GetModuleNode().GetFunctionName() == Name)
		{
			return Module;
		}
	}
	return nullptr;
}

UNiagaraStackFunctionInput* InputNamed(FAutomationTestBase& Test, UNiagaraStackModuleItem* Module, const FString& Name)
{
	if (Module == nullptr)
	{
		Test.AddError(FString::Printf(TEXT("no module for the input '%s'"), *Name));
		return nullptr;
	}
	Module->RefreshChildren();
	const TArray<UNiagaraStackFunctionInput*> Inputs = UE::AssetDocument::Niagara::ModuleInputs(*Module);
	TArray<FString> Names;
	for (UNiagaraStackFunctionInput* Input : Inputs)
	{
		const FString Handle = Input->GetInputParameterHandle().GetName().ToString();
		if (Handle == Name || Input->GetDisplayName().ToString() == Name)
		{
			return Input;
		}
		Names.Add(Handle);
	}
	Test.AddError(FString::Printf(TEXT("%s has no input '%s' (inputs: %s)"), *Module->GetModuleNode().GetFunctionName(), *Name,
		*FString::Join(Names, TEXT(", "))));
	return nullptr;
}

UNiagaraStackFunctionInput* ChildInputNamed(UNiagaraStackFunctionInput& Input, const FString& Name)
{
	Input.RefreshChildren();
	for (UNiagaraStackFunctionInput* Child : UE::AssetDocument::Niagara::DynamicInputInputs(Input))
	{
		if (Child->GetInputParameterHandle().GetName().ToString() == Name || Child->GetDisplayName().ToString() == Name)
		{
			return Child;
		}
	}
	return nullptr;
}

// The group's "+" menu: the first action Pick accepts.
bool RunAddAction(UNiagaraStackItemGroup& Group, TFunctionRef<bool(const INiagaraStackItemGroupAddAction&)> Pick)
{
	INiagaraStackItemGroupAddUtilities* AddUtilities = Group.GetAddUtilities();
	if (AddUtilities == nullptr)
	{
		return false;
	}
	TArray<TSharedRef<INiagaraStackItemGroupAddAction>> Actions;
	AddUtilities->GenerateAddActions(Actions, FNiagaraStackItemGroupAddOptions());
	for (const TSharedRef<INiagaraStackItemGroupAddAction>& Action : Actions)
	{
		if (Pick(*Action))
		{
			AddUtilities->ExecuteAddAction(Action, INDEX_NONE);
			return true;
		}
	}
	return false;
}

// A module script asset of the "+" menu (its description ends with the asset path).
bool AddModuleLikeTheEditor(UNiagaraStackItemGroup& Group, const FString& ScriptPath)
{
	return RunAddAction(Group, [&ScriptPath](const INiagaraStackItemGroupAddAction& Action)
	{
		const FString Description = Action.GetDescription().ToString();
		return Description.EndsWith(TEXT("Path: ") + ScriptPath) || Description.Contains(TEXT("Path: ") + ScriptPath + TEXT("\n"));
	});
}

TSharedRef<FStructOnScope> LocalValue(const UNiagaraStackFunctionInput& Input)
{
	return MakeShared<FStructOnScope>(Input.GetInputType().GetScriptStruct());
}

void SetFloat(UNiagaraStackFunctionInput* Input, float Value)
{
	if (Input != nullptr)
	{
		const TSharedRef<FStructOnScope> Struct = LocalValue(*Input);
		*reinterpret_cast<float*>(Struct->GetStructMemory()) = Value;
		Input->SetLocalValue(Struct);
	}
}

UNiagaraStackEmitterPropertiesGroup* PropertiesGroupOf(UNiagaraStackEntry& Root)
{
	TArray<UNiagaraStackEmitterPropertiesGroup*> Groups;
	Root.GetUnfilteredChildrenOfType(Groups);
	return Groups.IsEmpty() ? nullptr : Groups[0];
}

// The emitter's "+ Stage" menu: a simulation stage class (keywords = class name).
bool AddStageLikeTheEditor(UNiagaraStackEntry& Root, const UClass& StageClass)
{
	UNiagaraStackEmitterPropertiesGroup* Group = PropertiesGroupOf(Root);
	return Group != nullptr && RunAddAction(*Group, [&StageClass](const INiagaraStackItemGroupAddAction& Action)
	{
		return Action.GetCategories().Contains(TEXT("Simulation Stages")) && Action.GetKeywords().ToString() == StageClass.GetName();
	});
}

TArray<UNiagaraStackSimulationStageGroup*> StageGroups(UNiagaraStackEntry& Root)
{
	Root.RefreshChildren();
	TArray<UNiagaraStackSimulationStageGroup*> Groups;
	Root.GetUnfilteredChildrenOfType(Groups);
	return Groups;
}

UNiagaraStackSimulationStageGroup* StageNamed(UNiagaraStackEntry& Root, const FString& Name)
{
	for (UNiagaraStackSimulationStageGroup* Group : StageGroups(Root))
	{
		if (Group->GetSimulationStage() != nullptr && Group->GetSimulationStage()->SimulationStageName.ToString() == Name)
		{
			return Group;
		}
	}
	return nullptr;
}

// Drag and drop of an entry onto (zone) another.
bool DropLikeTheEditor(UNiagaraStackEntry& Dragged, UNiagaraStackEntry& Target, EItemDropZone Zone)
{
	TArray<UNiagaraStackEntry*> Entries;
	Entries.Add(&Dragged);
	const UNiagaraStackEntry::FDropRequest Request(MakeShared<FNiagaraStackEntryDragDropOp>(Entries), Zone, UNiagaraStackEntry::EDragOptions::None,
		UNiagaraStackEntry::EDropOptions::Overview);
	const TOptional<UNiagaraStackEntry::FDropRequestResponse> Can = Target.CanDrop(Request);
	if (!Can.IsSet() || !Can->DropZone.IsSet())
	{
		return false;
	}
	Target.Drop(Request);
	return true;
}

// Step 2 of every scenario: the editor copy's extract applied to the document copy.
void CompareWithDocument(FAutomationTestBase& Test, const FTarget& ByEditor, const FTarget& ByDocument, UNiagaraSystem& EditorSystem, const FString& Example)
{
	CompileAndWait(EditorSystem);
	Test.TestEqual(TEXT("the editor's edits compile"), CompileErrors(EditorSystem), FString());
	Test.TestTrue(TEXT("editor copy saved"), SaveAsset(EditorSystem, ByEditor.PackageFile));
	const TSharedPtr<FJsonObject> Expected = Extract(Test, ByEditor, TEXT("editor result"));
	if (!Expected.IsValid())
	{
		return;
	}
	const TSharedPtr<FJsonObject> Document = Retargeted(Expected, ByDocument, TEXT("Update"));
	Test.AddInfo(FString::Printf(TEXT("NiagaraExample %s %s"), *Example, *Serialize(Document).Left(12000)));
	const FAssetDocumentResult Applied = ApplyDocument(Test, ByDocument, Document, TEXT("document"));
	if (!Test.TestTrue(TEXT("the document applies"), Applied.IsSuccess()))
	{
		return;
	}
	Test.TestTrue(TEXT("the apply saves the system"), Applied.bSavedAsset);
	Test.TestFalse(TEXT("after the apply: no undo history references the system"), UE::AssetDocument::UndoHistory::ReferencesPackage(ByDocument.PackageName));
	const TSharedPtr<FJsonObject> Actual = Extract(Test, ByDocument, TEXT("document result"));
	TestTextEqual(Test, TEXT("document path == editor path"), MaskEditorNames(Content(Actual)), MaskEditorNames(Content(Expected)));
	TestNoOpReapply(Test, ByDocument, TEXT("document copy"));
	TestReload(Test, ByDocument, TEXT("document copy"));
}
} // namespace
} // namespace AssetDocumentNiagaraTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraStackEditorEquivalenceTest, "UEAssetDoc.Niagara.Stack.EditorEquivalence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraStackEditorEquivalenceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraTests;
	UObject* Source = LoadObject<UObject>(nullptr, DirectionalBurstSystem);
	UNiagaraScript* RandomRange = LoadObject<UNiagaraScript>(nullptr, TEXT("/Niagara/DynamicInputs/UniformRange/V2/RandomRangeFloat.RandomRangeFloat"));
	if (!TestNotNull(TEXT("template system loads"), Source) || !TestNotNull(TEXT("dynamic input script loads"), RandomRange))
	{
		return false;
	}
	const FTarget ByEditor(TEXT("NS_StackEditor"));
	const FTarget ByDocument(TEXT("NS_StackDocument"));
	ON_SCOPE_EXIT
	{
		ByDocument.Cleanup();
		ByEditor.Cleanup();
	};
	UNiagaraSystem* EditorSystem = Cast<UNiagaraSystem>(DuplicateTo(*Source, ByEditor));
	UNiagaraSystem* DocumentSystem = Cast<UNiagaraSystem>(DuplicateTo(*Source, ByDocument));
	if (!TestNotNull(TEXT("editor copy"), EditorSystem) || !TestNotNull(TEXT("document copy"), DocumentSystem))
	{
		return false;
	}
	EditorSystem->WaitForCompilationComplete(false, false);
	DocumentSystem->WaitForCompilationComplete(false, false);

	// 1. The editor's own API on the first copy.
	{
		FAssetDocumentEditorNonInteractiveScope NonInteractive;
		const TSharedRef<FNiagaraSystemViewModel> ViewModel = MakeViewModel(*EditorSystem);
		const TSharedPtr<FNiagaraEmitterHandleViewModel> Burst = HandleNamed(*ViewModel, TEXT("DirectionalBurst"));
		const TSharedPtr<FNiagaraEmitterHandleViewModel> Ribbon = HandleNamed(*ViewModel, TEXT("LocationBasedRibbon"));
		UNiagaraStackEntry* Root = Burst.IsValid() ? RootOf(Burst->GetEmitterStackViewModel()) : nullptr;
		if (!TestTrue(TEXT("template emitters present"), Root != nullptr && Ribbon.IsValid()))
		{
			return false;
		}
		UNiagaraStackScriptItemGroup* ParticleUpdate = ScriptGroup(*Root, ENiagaraScriptUsage::ParticleUpdateScript);
		if (!TestNotNull(TEXT("Particle Update group"), ParticleUpdate))
		{
			return false;
		}
		// Module list: "+" for a module asset, delete, drag and drop, the checkbox, the version menu, rename, note.
		TestTrue(TEXT("editor: Curl Noise Force added from the + menu"),
			AddModuleLikeTheEditor(*ParticleUpdate, TEXT("/Niagara/Modules/Update/Forces/V2/CurlNoiseForce.CurlNoiseForce")));
		UNiagaraStackModuleItem* Gravity = ModuleNamed(*Root, TEXT("GravityForce"));
		FText Why;
		if (TestTrue(TEXT("editor: GravityForce deletable"), Gravity != nullptr && Gravity->TestCanDeleteWithMessage(Why)))
		{
			Gravity->Delete();
		}
		UNiagaraStackModuleItem* ScaleColor = ModuleNamed(*Root, TEXT("ScaleColor"));
		UNiagaraStackModuleItem* SpriteSize = ModuleNamed(*Root, TEXT("ScaleSpriteSizeBySpeed"));
		TestTrue(TEXT("editor: ScaleColor dropped below ScaleSpriteSizeBySpeed"), ScaleColor != nullptr && SpriteSize != nullptr
			&& DropLikeTheEditor(*ScaleColor, *SpriteSize, EItemDropZone::BelowItem));
		if (UNiagaraStackModuleItem* Disabled = ModuleNamed(*Root, TEXT("ScaleSpriteSizeBySpeed")))
		{
			Disabled->SetIsEnabled(false);
		}
		if (UNiagaraStackModuleItem* Drag = ModuleNamed(*Root, TEXT("Drag")))
		{
			const FGuid Current = Drag->GetModuleNode().SelectedScriptVersion;
			FGuid Other;
			for (const FNiagaraAssetVersion& Version : Drag->GetModuleNode().FunctionScript->GetAllAvailableVersions())
			{
				if (!Other.IsValid() && Version.VersionGuid != Current && Version.bIsVisibleInVersionSelector)
				{
					Other = Version.VersionGuid;
				}
			}
			if (Other.IsValid())
			{
				Drag->ChangeScriptVersion(Other);
			}
			else
			{
				AddWarning(TEXT("Drag has a single version in this install: the version menu was not exercised."));
			}
		}
		if (UNiagaraStackModuleItem* Cone = ModuleNamed(*Root, TEXT("AddVelocityInCone")))
		{
			Cone->OnRenamed(FText::FromString(TEXT("Cone Push")));
		}
		if (UNiagaraStackModuleItem* Initialize = ModuleNamed(*Root, TEXT("InitializeParticle")))
		{
			FNiagaraStackNoteData Note;
			Note.MessageHeader = FText::FromString(TEXT("Spawn"));
			Note.Message = FText::FromString(TEXT("Random lifetime and mass."));
			Initialize->SetStackNoteData(Note);
			// Reset arrow.
			if (UNiagaraStackFunctionInput* Mass = InputNamed(*this, Initialize, TEXT("Mass Mode")))
			{
				TestTrue(TEXT("editor: Mass Mode resettable"), Mass->CanReset());
				Mass->Reset();
			}
		}
		// Inputs: local value, link (adds the user parameter), nested dynamic input, expression, data interface Details.
		if (UNiagaraStackFunctionInput* Count = InputNamed(*this, ModuleNamed(*Root, TEXT("SpawnBurst_Instantaneous")), TEXT("Spawn Count")))
		{
			const TSharedRef<FStructOnScope> Value = LocalValue(*Count);
			*reinterpret_cast<int32*>(Value->GetStructMemory()) = 35;
			Count->SetLocalValue(Value);
		}
		if (UNiagaraStackFunctionInput* Strength = InputNamed(*this, ModuleNamed(*Root, TEXT("CurlNoiseForce")), TEXT("Noise Strength")))
		{
			Strength->SetLinkedParameterValue(FNiagaraVariableBase(Strength->GetInputType(), TEXT("User.Turbulence")));
		}
		UNiagaraStackModuleItem* ConeModule = ModuleNamed(*Root, TEXT("AddVelocityInCone"));
		if (UNiagaraStackFunctionInput* Angle = InputNamed(*this, ConeModule, TEXT("Cone Angle")))
		{
			Angle->SetDynamicInput(RandomRange);
			UNiagaraStackFunctionInput* Minimum = ChildInputNamed(*Angle, TEXT("Minimum"));
			if (TestNotNull(TEXT("editor: the dynamic input's Minimum"), Minimum))
			{
				SetFloat(Minimum, 20.0f);
			}
		}
		if (UNiagaraStackFunctionInput* Along = InputNamed(*this, ConeModule, TEXT("Velocity Distribution Along Cone Axis")))
		{
			Along->SetCustomExpression(TEXT("0.25 + 0.5"));
		}
		if (UNiagaraStackFunctionInput* Curve = InputNamed(*this, ModuleNamed(*Root, TEXT("ScaleSpriteSizeBySpeed")), TEXT("Scale Factor Curve")))
		{
			UNiagaraDataInterfaceCurveBase* Interface = Cast<UNiagaraDataInterfaceCurveBase>(Curve->GetDataValueObject());
			if (TestNotNull(TEXT("editor: the curve data interface"), Interface))
			{
				// The Details panel of the input's data interface: Modify, PreEditChange, write, PostEditChangeProperty,
				// then the stack object's NotifyPostChange.
				FBoolProperty* Property = CastField<FBoolProperty>(Interface->GetClass()->FindPropertyByName(TEXT("bOptimizeLUT")));
				if (TestNotNull(TEXT("editor: bOptimizeLUT"), Property))
				{
					Interface->Modify();
					Interface->PreEditChange(Property);
					Property->SetPropertyValue_InContainer(Interface, !Property->GetPropertyValue_InContainer(Interface));
					FPropertyChangedEvent Changed(Property, EPropertyChangeType::ValueSet);
					Interface->PostEditChangeProperty(Changed);
					if (UNiagaraStackObject* Details = Curve->GetChildDataObject())
					{
						Details->NotifyPostChange(Changed, Property);
					}
				}
			}
		}
		// Set Parameters: "Set new or existing parameter directly", the module's "+" for a target, its value.
		const TSet<FString> Before = [Root]()
		{
			TSet<FString> Names;
			TArray<UNiagaraStackModuleItem*> Modules;
			Root->GetUnfilteredChildrenOfType(Modules, true);
			for (const UNiagaraStackModuleItem* Module : Modules)
			{
				Names.Add(Module->GetModuleNode().GetFunctionName());
			}
			return Names;
		}();
		TestTrue(TEXT("editor: Set Parameters added"), RunAddAction(*ParticleUpdate, [](const INiagaraStackItemGroupAddAction& Action)
		{
			return Action.GetDisplayName().ToString() == TEXT("Set new or existing parameter directly");
		}));
		UNiagaraStackModuleItem* Assignment = nullptr;
		{
			Root->RefreshChildren();
			TArray<UNiagaraStackModuleItem*> Modules;
			Root->GetUnfilteredChildrenOfType(Modules, true);
			for (UNiagaraStackModuleItem* Module : Modules)
			{
				if (!Before.Contains(Module->GetModuleNode().GetFunctionName()))
				{
					Assignment = Module;
				}
			}
		}
		if (TestNotNull(TEXT("editor: the new Set Parameters module"), Assignment))
		{
			const FNiagaraVariable Target(FNiagaraTypeDefinition::GetColorDef(), TEXT("Particles.Color"));
			TestTrue(TEXT("editor: Particles.Color can be set"), Assignment->CanAddInput(Target));
			Assignment->AddInput(Target);
			TArray<UNiagaraStackFunctionInput*> Targets;
			Assignment->RefreshChildren();
			Assignment->GetParameterInputs(Targets);
			if (TestEqual(TEXT("editor: one target"), Targets.Num(), 1))
			{
				const TSharedRef<FStructOnScope> Value = LocalValue(*Targets[0]);
				*reinterpret_cast<FLinearColor*>(Value->GetStructMemory()) = FLinearColor(1.0f, 0.5f, 0.25f, 1.0f);
				Targets[0]->SetLocalValue(Value);
			}
		}
		// Event handler Details (the handler's Details panel writes the handler list back and notifies the emitter).
		{
			const FVersionedNiagaraEmitter Emitter = Ribbon->GetEmitterViewModel()->GetEmitter();
			FVersionedNiagaraEmitterData* Data = Emitter.GetEmitterData();
			if (TestTrue(TEXT("editor: the ribbon has an event handler"), Data != nullptr && !Data->GetEventHandlers().IsEmpty()))
			{
				Emitter.Emitter->Modify();
				Data->EventHandlerScriptProps[0].MaxEventsPerFrame = 7;
				FPropertyChangedEvent Changed(FVersionedNiagaraEmitterData::StaticStruct()->FindPropertyByName(TEXT("EventHandlerScriptProps")),
					EPropertyChangeType::ValueSet);
				Emitter.Emitter->PostEditChangeVersionedProperty(Changed, Emitter.Version);
			}
		}
		TestFalse(TEXT("editor: no dialog"), NonInteractive.WasInteractionRefused());
	}
	CompareWithDocument(*this, ByEditor, ByDocument, *EditorSystem, TEXT("stack edit document"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraStackStagesTest, "UEAssetDoc.Niagara.Stack.Stages",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraStackStagesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraTests;
	const FTarget Base(TEXT("NS_StageBase"));
	const FTarget ByEditor(TEXT("NS_StageEditor"));
	const FTarget ByDocument(TEXT("NS_StageDocument"));
	ON_SCOPE_EXIT
	{
		ByDocument.Cleanup();
		ByEditor.Cleanup();
		Base.Cleanup();
	};
	// A GPU emitter with one stage, written as a document.
	const FString Json = FString::Printf(TEXT(R"json({
  "SchemaVersion": 1, "Target": "%s", "Class": "/Script/Niagara.NiagaraSystem", "Action": "Create", "Definitions": {},
  "Body": {"Emitters": [{"Name": "Grid", "From": "/Niagara/DefaultAssets/Templates/Emitters/Fountain",
    "Properties": {"SimTarget": "GPUComputeSim", "CalculateBoundsMode": "Fixed"},
    "SimulationStages": [{"Name": "Old", "Class": "Generic"}]}]}
})json"), *Base.PackageName);
	const FAssetDocumentResult Created = Apply(*this, Base, Json, TEXT("base"));
	UObject* BaseSystem = Base.Get();
	if (!TestTrue(TEXT("the base system is created"), Created.IsSuccess() && BaseSystem != nullptr))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> BaseDocument = Extract(*this, Base, TEXT("base"));
	TestEqual(TEXT("the base has its stage"), List(Named(List(Child(BaseDocument, TEXT("Body")), TEXT("Emitters")), TEXT("Grid")), TEXT("SimulationStages")).Num(), 1);
	UNiagaraSystem* EditorSystem = Cast<UNiagaraSystem>(DuplicateTo(*BaseSystem, ByEditor));
	UNiagaraSystem* DocumentSystem = Cast<UNiagaraSystem>(DuplicateTo(*BaseSystem, ByDocument));
	if (!TestNotNull(TEXT("editor copy"), EditorSystem) || !TestNotNull(TEXT("document copy"), DocumentSystem))
	{
		return false;
	}
	EditorSystem->WaitForCompilationComplete(false, false);
	DocumentSystem->WaitForCompilationComplete(false, false);
	{
		FAssetDocumentEditorNonInteractiveScope NonInteractive;
		const TSharedRef<FNiagaraSystemViewModel> ViewModel = MakeViewModel(*EditorSystem);
		const TSharedPtr<FNiagaraEmitterHandleViewModel> Grid = HandleNamed(*ViewModel, TEXT("Grid"));
		UNiagaraStackEntry* Root = Grid.IsValid() ? RootOf(Grid->GetEmitterStackViewModel()) : nullptr;
		if (!TestNotNull(TEXT("Grid stack"), Root))
		{
			return false;
		}
		// Two stages from the "+ Stage" menu, named in their Details.
		for (const TCHAR* Name : {TEXT("Settle"), TEXT("Relax")})
		{
			const TArray<UNiagaraStackSimulationStageGroup*> Before = StageGroups(*Root);
			if (!TestTrue(TEXT("editor: a Generic stage added"), AddStageLikeTheEditor(*Root, *UNiagaraSimulationStageGeneric::StaticClass())))
			{
				return false;
			}
			for (UNiagaraStackSimulationStageGroup* Group : StageGroups(*Root))
			{
				UNiagaraSimulationStageBase* Stage = Group->GetSimulationStage();
				if (!Before.Contains(Group) && Stage != nullptr)
				{
					DetailsEdit(*Stage, TEXT("SimulationStageName"), [Stage, Name]() { Stage->SimulationStageName = Name; });
				}
			}
		}
		// Details of one stage, "Relax" dragged above "Settle" and disabled, "Old" deleted.
		if (UNiagaraStackSimulationStageGroup* Settle = StageNamed(*Root, TEXT("Settle")))
		{
			UNiagaraSimulationStageGeneric* Stage = Cast<UNiagaraSimulationStageGeneric>(Settle->GetSimulationStage());
			if (TestNotNull(TEXT("editor: Settle is a Generic stage"), Stage))
			{
				DetailsEdit(*Stage, TEXT("ExecuteBehavior"), [Stage]() { Stage->ExecuteBehavior = ENiagaraSimStageExecuteBehavior::NotOnSimulationReset; });
			}
			// A module inside the stage, from its "+" menu.
			bool bAdded = false;
			for (const TCHAR* Script : {TEXT("/Niagara/Modules/Update/Forces/Drag.Drag"), TEXT("/Niagara/Modules/Update/Forces/GravityForce.GravityForce"),
					TEXT("/Niagara/Modules/Solvers/SolveForcesAndVelocity.SolveForcesAndVelocity")})
			{
				bAdded = bAdded || AddModuleLikeTheEditor(*Settle, Script);
			}
			TestTrue(TEXT("editor: a module added inside the stage"), bAdded);
		}
		UNiagaraStackSimulationStageGroup* Relax = StageNamed(*Root, TEXT("Relax"));
		UNiagaraStackSimulationStageGroup* Settle = StageNamed(*Root, TEXT("Settle"));
		TestTrue(TEXT("editor: Relax dropped above Settle"), Relax != nullptr && Settle != nullptr
			&& DropLikeTheEditor(*Relax, *Settle, EItemDropZone::AboveItem));
		if (UNiagaraStackSimulationStageGroup* Moved = StageNamed(*Root, TEXT("Relax")))
		{
			Moved->SetIsEnabled(false);
		}
		UNiagaraStackSimulationStageGroup* Old = StageNamed(*Root, TEXT("Old"));
		FText Why;
		if (TestTrue(TEXT("editor: Old deletable"), Old != nullptr && Old->TestCanDeleteWithMessage(Why)))
		{
			Old->Delete();
		}
		TestFalse(TEXT("editor: no dialog"), NonInteractive.WasInteractionRefused());
	}
	CompareWithDocument(*this, ByEditor, ByDocument, *EditorSystem, TEXT("stage edit document"));
	return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraStackSetParametersTest, "UEAssetDoc.Niagara.Stack.SetParameters",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraStackSetParametersTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraTests;
	ExpectVectorVMCompileErrors(*this, TEXT("AssetDocumentUndefinedSymbol"));
	const FTarget Target(TEXT("NS_SetParameters"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
	};
	const FString Json = FString::Printf(TEXT(R"json({
  "SchemaVersion": 1, "Target": "%s", "Class": "/Script/Niagara.NiagaraSystem", "Action": "Create", "Definitions": {},
  "Body": {"Emitters": [{"Name": "Spark", "From": "/Niagara/DefaultAssets/Templates/Emitters/Fountain"}]}
})json"), *Target.PackageName);
	if (!TestTrue(TEXT("the system is created"), Apply(*this, Target, Json, TEXT("base")).IsSuccess()))
	{
		return false;
	}
	// The Set Parameters module of the saved system (the last entry of Spark's ParticleSpawn with an Assign list).
	auto SetParameters = [](const TSharedPtr<FJsonObject>& Document) -> TSharedPtr<FJsonObject>
	{
		const TArray<TSharedPtr<FJsonValue>> Modules = List(Named(List(Child(Document, TEXT("Body")), TEXT("Emitters")), TEXT("Spark")),
			TEXT("ParticleSpawn"));
		for (int32 Index = Modules.Num() - 1; Index >= 0; --Index)
		{
			if (Modules[Index]->AsObject()->HasField(TEXT("Assign")))
			{
				return Modules[Index]->AsObject();
			}
		}
		return nullptr;
	};
	auto Edit = [this, &Target](const TCHAR* Stage, TFunctionRef<void(const TSharedPtr<FJsonObject>&, TArray<TSharedPtr<FJsonValue>>&)> Change)
	{
		const TSharedPtr<FJsonObject> Document = Clone(Extract(*this, Target, Stage));
		Document->SetStringField(TEXT("Action"), TEXT("Update"));
		const TSharedPtr<FJsonObject> Spark = Named(List(Child(Document, TEXT("Body")), TEXT("Emitters")), TEXT("Spark"));
		TArray<TSharedPtr<FJsonValue>> Modules = List(Spark, TEXT("ParticleSpawn"));
		Change(Document, Modules);
		Spark->SetArrayField(TEXT("ParticleSpawn"), Modules);
		return ApplyDocument(*this, Target, Document, Stage);
	};
	// 1. A new Set Parameters module with one target.
	const FAssetDocumentResult Added = Edit(TEXT("add"), [](const TSharedPtr<FJsonObject>&, TArray<TSharedPtr<FJsonValue>>& Modules)
	{
		Modules.Add(MakeShared<FJsonValueObject>(Parse(TEXT(R"json({"Name": "Tint",
  "Assign": [{"Target": "Particles.Color", "Type": "LinearColor"}],
  "Inputs": {"Particles.Color": {"R": 1, "G": 0.5, "B": 0.25, "A": 1}}})json"))));
	});
	if (!TestTrue(TEXT("a Set Parameters module is added"), Added.IsSuccess()))
	{
		return false;
	}
	// 2. A target added to the saved module: the node regenerates its function script under the same name, which the
	// apply's package snapshot must not hold (it is derived from the targets).
	const FAssetDocumentResult Grown = Edit(TEXT("add target"), [&SetParameters](const TSharedPtr<FJsonObject>& Document, TArray<TSharedPtr<FJsonValue>>&)
	{
		const TSharedPtr<FJsonObject> Module = SetParameters(Document);
		if (!Module.IsValid())
		{
			return;
		}
		TArray<TSharedPtr<FJsonValue>> Assign = Module->GetArrayField(TEXT("Assign"));
		Assign.Add(MakeShared<FJsonValueObject>(Parse(TEXT(R"json({"Target": "Particles.SpriteSize", "Type": "Vector2"})json"))));
		Module->SetArrayField(TEXT("Assign"), Assign);
		Child(Module, TEXT("Inputs"))->SetObjectField(TEXT("Particles.SpriteSize"), Parse(TEXT(R"json({"X": 5, "Y": 7})json")));
	});
	TestTrue(TEXT("a target is added to a saved Set Parameters module"), Grown.IsSuccess());
	const TSharedPtr<FJsonObject> Two = Extract(*this, Target, TEXT("two targets"));
	const TSharedPtr<FJsonObject> TwoModule = SetParameters(Two);
	TestEqual(TEXT("the module sets two targets"), TwoModule.IsValid() ? TwoModule->GetArrayField(TEXT("Assign")).Num() : 0, 2);
	const FString TwoText = Serialize(Two);
	// 3. A target removed together with an expression that does not compile: the apply rolls back in memory and the
	// regenerated function script follows the restored targets.
	const FAssetDocumentResult Broken = Edit(TEXT("broken"), [&SetParameters](const TSharedPtr<FJsonObject>& Document, TArray<TSharedPtr<FJsonValue>>&)
	{
		const TSharedPtr<FJsonObject> Module = SetParameters(Document);
		if (!Module.IsValid())
		{
			return;
		}
		TArray<TSharedPtr<FJsonValue>> Assign = Module->GetArrayField(TEXT("Assign"));
		Assign.RemoveAt(0);
		Module->SetArrayField(TEXT("Assign"), Assign);
		Child(Module, TEXT("Inputs"))->RemoveField(TEXT("Particles.Color"));
		Child(Module, TEXT("Inputs"))->SetObjectField(TEXT("Particles.SpriteSize"),
			Parse(TEXT(R"json({"Expression": "AssetDocumentUndefinedSymbol * 3.0"})json")));
	});
	TestTrue(TEXT("the broken edit fails to compile"), !Broken.IsSuccess() && HasDiagnostic(Broken, TEXT("Niagara.CompileFailed")));
	TestFalse(TEXT("the rollback restores in memory"), HasDiagnostic(Broken, TEXT("PackageSnapshot.ReloadedFromDisk")));
	TestTextEqual(*this, TEXT("the rollback restores both targets"), Serialize(Extract(*this, Target, TEXT("after rollback"))), TwoText);
	if (const UNiagaraSystem* System = Cast<UNiagaraSystem>(Target.Get()))
	{
		TestEqual(TEXT("the restored system compiles clean"), CompileErrors(*System), FString());
	}
	// 4. A target removed.
	const FAssetDocumentResult Shrunk = Edit(TEXT("remove target"), [&SetParameters](const TSharedPtr<FJsonObject>& Document, TArray<TSharedPtr<FJsonValue>>&)
	{
		const TSharedPtr<FJsonObject> Module = SetParameters(Document);
		if (!Module.IsValid())
		{
			return;
		}
		TArray<TSharedPtr<FJsonValue>> Assign = Module->GetArrayField(TEXT("Assign"));
		Assign.RemoveAt(0);
		Module->SetArrayField(TEXT("Assign"), Assign);
		Child(Module, TEXT("Inputs"))->RemoveField(TEXT("Particles.Color"));
	});
	TestTrue(TEXT("a target is removed"), Shrunk.IsSuccess());
	const TSharedPtr<FJsonObject> OneModule = SetParameters(Extract(*this, Target, TEXT("one target")));
	TestEqual(TEXT("the module sets one target"), OneModule.IsValid() ? OneModule->GetArrayField(TEXT("Assign")).Num() : 0, 1);
	TestNoOpReapply(*this, Target, TEXT("set parameters"));
	TestReload(*this, Target, TEXT("set parameters"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraStackNestedInputsTest, "UEAssetDoc.Niagara.Stack.NestedInputs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraStackNestedInputsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraTests;
	UObject* Source = LoadObject<UObject>(nullptr, DirectionalBurstSystem);
	const FTarget Copy(TEXT("NS_NestedInputs"));
	ON_SCOPE_EXIT
	{
		Copy.Cleanup();
	};
	UNiagaraSystem* System = Source != nullptr ? Cast<UNiagaraSystem>(DuplicateTo(*Source, Copy)) : nullptr;
	if (!TestNotNull(TEXT("copy"), System))
	{
		return false;
	}
	System->WaitForCompilationComplete(false, false);
	// The overridden inputs InitializeParticle's parameter hierarchy nests under its mode inputs, as the stack shows them.
	TArray<FString> Nested;
	{
		FAssetDocumentEditorNonInteractiveScope NonInteractive;
		const TSharedRef<FNiagaraSystemViewModel> ViewModel = MakeViewModel(*System);
		const TSharedPtr<FNiagaraEmitterHandleViewModel> Burst = HandleNamed(*ViewModel, TEXT("DirectionalBurst"));
		UNiagaraStackEntry* Root = Burst.IsValid() ? RootOf(Burst->GetEmitterStackViewModel()) : nullptr;
		UNiagaraStackModuleItem* Module = Root != nullptr ? ModuleNamed(*Root, TEXT("InitializeParticle")) : nullptr;
		if (!TestNotNull(TEXT("InitializeParticle"), Module))
		{
			return false;
		}
		TArray<UNiagaraStackFunctionInput*> Top;
		Module->GetParameterInputs(Top);
		for (UNiagaraStackFunctionInput* Input : UE::AssetDocument::Niagara::ModuleInputs(*Module))
		{
			if (!Top.Contains(Input) && Input->CanReset())
			{
				Nested.Add(Input->GetInputParameterHandle().GetName().ToString());
			}
		}
	}
	TestTrue(TEXT("DirectionalBurst overrides nested InitializeParticle inputs"), Nested.Num() > 0);
	auto Initialize = [](const TSharedPtr<FJsonObject>& Document)
	{
		return Named(List(Named(List(Child(Document, TEXT("Body")), TEXT("Emitters")), TEXT("DirectionalBurst")), TEXT("ParticleSpawn")),
			TEXT("InitializeParticle"));
	};
	const TSharedPtr<FJsonObject> Before = Extract(*this, Copy, TEXT("nested inputs"));
	const TSharedPtr<FJsonObject> Inputs = Child(Initialize(Before), TEXT("Inputs"));
	for (const FString& Name : Nested)
	{
		TestTrue(*FString::Printf(TEXT("the extract lists the nested input '%s'"), *Name), Inputs.IsValid() && Inputs->HasField(Name));
	}
	// A nested input set by document, with the mode that shows it (reset to its default), in one apply.
	const TSharedPtr<FJsonObject> Document = Clone(Before);
	Document->SetStringField(TEXT("Action"), TEXT("Update"));
	const TSharedPtr<FJsonObject> Wanted = Child(Initialize(Document), TEXT("Inputs"));
	if (!TestTrue(TEXT("InitializeParticle has Inputs"), Wanted.IsValid()))
	{
		return false;
	}
	Wanted->RemoveField(TEXT("Lifetime Min"));
	Wanted->RemoveField(TEXT("Lifetime Max"));
	Wanted->RemoveField(TEXT("Lifetime Mode")); // "Direct Set", the default
	Wanted->SetNumberField(TEXT("Lifetime"), 3.5);
	const FAssetDocumentResult Applied = ApplyDocument(*this, Copy, Document, TEXT("nested input"));
	TestTrue(TEXT("a nested input applies"), Applied.IsSuccess());
	const TSharedPtr<FJsonObject> After = Child(Initialize(Extract(*this, Copy, TEXT("after nested"))), TEXT("Inputs"));
	double Lifetime = 0.0;
	TestTrue(TEXT("Lifetime is 3.5"), After.IsValid() && After->TryGetNumberField(TEXT("Lifetime"), Lifetime) && FMath::IsNearlyEqual(Lifetime, 3.5));
	TestNoOpReapply(*this, Copy, TEXT("nested input"));
	// A link to a user parameter of another type is refused (the editor would add a second "User.Tint").
	const TSharedPtr<FJsonObject> Mismatch = Clone(Extract(*this, Copy, TEXT("before mismatch")));
	Mismatch->SetStringField(TEXT("Action"), TEXT("Update"));
	TArray<TSharedPtr<FJsonValue>> UserParameters = List(Child(Mismatch, TEXT("Body")), TEXT("UserParameters"));
	UserParameters.Add(MakeShared<FJsonValueObject>(Parse(TEXT(R"json({"Name": "User.Tint", "Type": "LinearColor", "Value": {"R": 1, "G": 0.5, "B": 0, "A": 1}})json"))));
	Child(Mismatch, TEXT("Body"))->SetArrayField(TEXT("UserParameters"), UserParameters);
	Child(Initialize(Mismatch), TEXT("Inputs"))->SetObjectField(TEXT("Lifetime"), Parse(TEXT(R"json({"Link": "User.Tint"})json")));
	const FAssetDocumentResult Refused = ApplyDocument(*this, Copy, Mismatch, TEXT("link type mismatch"));
	TestTrue(TEXT("a link to a user parameter of another type is refused"), !Refused.IsSuccess() && HasDiagnostic(Refused, TEXT("Niagara.LinkTypeMismatch")));
	return !HasAnyErrors();
}

#endif
