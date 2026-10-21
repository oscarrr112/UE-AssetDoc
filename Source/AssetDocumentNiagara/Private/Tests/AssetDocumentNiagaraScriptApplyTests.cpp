// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// N4 end-to-end tests: Niagara script assets (profile.niagara_script.v1) and scratch pads, through the production
// Service (the Python profile, graph.* primitives and fx.niagara.edit.v1) as the MCP tools send documents.
//   UEAssetDoc.Niagara.Script.RoundTrip       script tier (static switches, custom HLSL): each
//        asset's extract written as a Create document of a new script gives the same document; re-apply is a
//        no-op; save + reload is stable.
//   UEAssetDoc.Niagara.Script.FreeEdits       the same edits made through the editor's API on one
//        duplicate and through a document on another give the same document (settings, node layout and comment,
//        pin defaults, parameter metadata and static switch defaults).
//   UEAssetDoc.Niagara.Script.NewSettingsValue  a Settings value the asset did not have before
//        (the read leaves defaults out) is a Diff and applies; the default value itself is no difference.
//   UEAssetDoc.Niagara.Script.ManageScriptVariables  a graph edit that stops using a parameter
//        drops it as a reload would; the saved and reloaded script reads the same.
//   UEAssetDoc.Niagara.Script.ScriptVariableRule  the parameter-panel rule on the engine's module,
//        function and dynamic input scripts: a loaded graph (which the load already brought in line) is left as it is;
//        a missing entry of a referenced parameter is created and an entry nothing references is dropped; parameters
//        reached only through a default binding stay.
//   UEAssetDoc.Niagara.Script.Refusals        a new compile error fails at the node and rolls
//        back; usage is immutable; a parameter the graph does not use; the exposed version rule (refused without
//        EditExposedVersion, a new exposed version is fine); a dependent with unsaved changes refuses before any
//        change, a saved one is refreshed (Niagara.DependentsUpdated); undo history after an apply.
//   UEAssetDoc.Niagara.Script.StaticSwitchRename  a static switch renamed in its Details keeps its
//        parameter's Details, as the editor's rename does.
//   UEAssetDoc.Niagara.Script.OrbitAroundPoint a module written from a document (Map Get, custom
//        HLSL, Map Set) compiles and is used from a system's Particle Update stack.
//   UEAssetDoc.Niagara.ScratchPad.Systems     scratch pad systems (RenderTargetTexturePainter, the
//        stress tier when installed): re-apply is a no-op with the pads listed; a pad edited through the editor's
//        API on one duplicate and through a document on another reads the same; timings are logged.
//   UEAssetDoc.Niagara.ScratchPad.Authoring   a system scratch pad added, used from a stack,
//        renamed, edited and deleted by documents.

#include "AssetDocumentNiagaraTestEditing.h"

#include "Algo/AllOf.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_Niagara.h"
#include "NiagaraGraph.h"
#include "NiagaraConstants.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScratchPadContainer.h"
#include "NiagaraScriptSource.h"
#include "NiagaraScriptVariable.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentNiagaraTests
{
namespace
{
const TCHAR* const ScriptRoundTripTier[] = {
	TEXT("/Niagara/DefaultAssets/DefaultModule.DefaultModule"),
	TEXT("/Niagara/DefaultAssets/DefaultFunction.DefaultFunction"),
	TEXT("/Niagara/DefaultAssets/DefaultDynamicInputs.DefaultDynamicInputs"),
	TEXT("/Niagara/Modules/Update/Camera/CameraOffset.CameraOffset"),
	TEXT("/Niagara/DynamicInputs/Math/Abs_Float.Abs_Float"),
	TEXT("/Niagara/Functions/Rotation/MatrixToQuaternion.MatrixToQuaternion"),
	TEXT("/Niagara/DynamicInputs/Rotation/MakeQuaternion.MakeQuaternion"),
	TEXT("/Niagara/Modules/Spawn/Location/V2/ShapeLocation.ShapeLocation"),
	TEXT("/Niagara/DynamicInputs/Helpers/SimulationPosition.SimulationPosition"),
	TEXT("/Niagara/Functions/MakeScaleMatrix.MakeScaleMatrix"),
	TEXT("/Niagara/Modules/Update/Forces/AerodynamicDrag.AerodynamicDrag"),
	TEXT("/Niagara/Modules/Spawn/Location/V2/SkeletalMeshLocation.SkeletalMeshLocation"),
	TEXT("/Niagara/Modules/Collision/Collision.Collision"),
};
const TCHAR* const PadSystems[] = {
	TEXT("/Niagara/DefaultAssets/Templates/BehaviorExamples/RenderTargetTexturePainter.RenderTargetTexturePainter"),
	TEXT("/NiagaraFluids/Templates/Gas/2D/Systems/Grid2D_Gas_Explosion.Grid2D_Gas_Explosion"),
	TEXT("/WaterAdvanced/Niagara/Systems/Grid2D_SW_River.Grid2D_SW_River"),
	TEXT("/NiagaraFluids/Templates/Gas/3D/Systems/Grid3D_Gas_Fire.Grid3D_Gas_Fire"),
};
const TCHAR* const DefaultModule = TEXT("/Niagara/DefaultAssets/DefaultModule.DefaultModule");

UObject* LoadCorpus(const FString& Path)
{
	return FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Path)) ? LoadObject<UObject>(nullptr, *Path) : nullptr;
}

// Text of a document with one asset's paths mapped to another's.
FString Mapped(const TSharedPtr<FJsonObject>& Document, const FTarget& From, const FTarget& To)
{
	FString Text = Content(Document);
	Text.ReplaceInline(*From.ObjectPath, *To.ObjectPath, ESearchCase::CaseSensitive);
	Text.ReplaceInline(*From.PackageName, *To.PackageName, ESearchCase::CaseSensitive);
	Text.ReplaceInline(*From.AssetName, *To.AssetName, ESearchCase::CaseSensitive);
	return Text;
}

TSharedPtr<FJsonObject> Retext(const TSharedPtr<FJsonObject>& Document, const FTarget& From, const FTarget& To, const TCHAR* Action)
{
	FString Text = Serialize(Document);
	Text.ReplaceInline(*From.ObjectPath, *To.ObjectPath, ESearchCase::CaseSensitive);
	Text.ReplaceInline(*From.PackageName, *To.PackageName, ESearchCase::CaseSensitive);
	return Retargeted(Parse(Text), To, Action);
}

TSharedPtr<FJsonObject> Body(const TSharedPtr<FJsonObject>& Document)
{
	return Child(Document, TEXT("Body"));
}

TSharedPtr<FJsonObject> NodeWithAction(const TSharedPtr<FJsonObject>& Graph, const FString& Action)
{
	for (const TSharedPtr<FJsonValue>& Entry : List(Graph, TEXT("Nodes")))
	{
		const TSharedPtr<FJsonObject> Node = Entry->AsObject();
		if (Node->GetStringField(TEXT("Action")) == Action)
		{
			return Node;
		}
	}
	return nullptr;
}

UNiagaraGraph* EditGraph(UNiagaraScript& Script)
{
	return UE::AssetDocument::Niagara::ScriptEditGraph(Script);
}

FVersionedNiagaraScriptData* ExposedData(UNiagaraScript& Script)
{
	return Script.IsVersioningEnabled() ? Script.GetScriptData(Script.GetExposedVersion().VersionGuid) : Script.GetLatestScriptData();
}

UNiagaraScriptVariable* FirstVariable(UNiagaraGraph& Graph, bool bStaticSwitch)
{
	TArray<UNiagaraScriptVariable*> Found;
	for (const TPair<FNiagaraVariable, TObjectPtr<UNiagaraScriptVariable>>& Entry : Graph.GetAllMetaData())
	{
		if (Entry.Value != nullptr && Entry.Value->GetIsStaticSwitch() == bStaticSwitch
			&& Entry.Key.IsInNameSpace(FNiagaraConstants::ModuleNamespace))
		{
			Found.Add(Entry.Value.Get());
		}
	}
	Found.Sort([](const UNiagaraScriptVariable& A, const UNiagaraScriptVariable& B) { return A.Variable.GetName().LexicalLess(B.Variable.GetName()); });
	return Found.IsEmpty() ? nullptr : Found[0];
}

// An unlinked, visible float input pin with a default (the first by node name): what a pin default edit changes.
UEdGraphPin* FloatDefaultPin(UNiagaraGraph& Graph)
{
	TArray<UEdGraphNode*> Nodes = Graph.Nodes;
	Nodes.Sort([](const UEdGraphNode& A, const UEdGraphNode& B) { return A.GetName() < B.GetName(); });
	for (UEdGraphNode* Node : Nodes)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction == EGPD_Input && !Pin->bHidden && Pin->LinkedTo.IsEmpty() && !Pin->bDefaultValueIsIgnored
				&& UEdGraphSchema_Niagara::PinToTypeDefinition(Pin) == FNiagaraTypeDefinition::GetFloatDef() && !Pin->DefaultValue.IsEmpty())
			{
				return Pin;
			}
		}
	}
	return nullptr;
}

// The script editor's edits made through its own API on Script: Details (Description), the first node moved and
// commented, a float pin default typed in, the first module parameter's description and the first static switch's
// default (the parameter panel's Details). Returns what was edited.
FString EditLikeTheEditor(UNiagaraScript& Script)
{
	TArray<FString> Done;
	UNiagaraGraph* Graph = EditGraph(Script);
	if (Graph == nullptr)
	{
		return FString();
	}
	if (FVersionedNiagaraScriptData* Data = ExposedData(Script))
	{
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentNiagaraTests", "Details", "Details"));
		Script.Modify();
		Data->Description = FText::FromString(TEXT("Edited by the free-edit test"));
		FProperty* Property = FindFProperty<FProperty>(FVersionedNiagaraScriptData::StaticStruct(), TEXT("Description"));
		FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
		Script.PostEditChangeVersionedProperty(Event, Data->Version.VersionGuid);
		Done.Add(TEXT("settings"));
	}
	TArray<UEdGraphNode*> Nodes = Graph->Nodes;
	Nodes.Sort([](const UEdGraphNode& A, const UEdGraphNode& B) { return A.GetName() < B.GetName(); });
	if (!Nodes.IsEmpty())
	{
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentNiagaraTests", "Move", "Move"));
		Nodes[0]->Modify();
		Nodes[0]->NodePosX += 64;
		Nodes[0]->NodePosY -= 32;
		Nodes[0]->NodeComment = TEXT("Moved by the free-edit test");
		Done.Add(TEXT("layout"));
	}
	if (UEdGraphPin* Pin = FloatDefaultPin(*Graph))
	{
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentNiagaraTests", "Default", "Default"));
		Pin->GetOwningNode()->Modify();
		GetDefault<UEdGraphSchema_Niagara>()->TrySetDefaultValue(*Pin, TEXT("2.5"));
		Done.Add(TEXT("pin default"));
	}
	if (UNiagaraScriptVariable* Variable = FirstVariable(*Graph, false))
	{
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentNiagaraTests", "Meta", "Meta"));
		Variable->Modify();
		Variable->Metadata.Description = FText::FromString(TEXT("Described by the free-edit test"));
		FProperty* Property = FindFProperty<FProperty>(UNiagaraScriptVariable::StaticClass(), TEXT("Metadata"));
		FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
		Variable->PostEditChangeProperty(Event);
		Done.Add(TEXT("parameter metadata"));
	}
	if (UNiagaraScriptVariable* Switch = FirstVariable(*Graph, true))
	{
		// FNiagaraScriptVariableDetails::OnStaticSwitchValueChanged.
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentNiagaraTests", "Switch", "Switch"));
		Switch->Modify();
		const int32 Value = Switch->GetStaticSwitchDefaultValue() == 0 ? 1 : 0;
		Switch->Variable.SetValue(FNiagaraInt32{Value});
		Switch->SetStaticSwitchDefaultValue(Value);
		FProperty* Property = FindFProperty<FProperty>(UNiagaraScriptVariable::StaticClass(), TEXT("Variable"));
		FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
		Switch->PostEditChangeProperty(Event);
		Done.Add(TEXT("static switch default"));
	}
	// The script editor's Apply: compile.
	Script.RequestCompile(Script.IsVersioningEnabled() ? Script.GetExposedVersion().VersionGuid : FGuid(), false);
	return FString::Join(Done, TEXT(", "));
}

void SaveTarget(const FTarget& Target)
{
	if (UObject* Asset = Target.Get())
	{
		SaveAsset(*Asset, Target.PackageFile);
	}
}

// A new module made by a Create document (the New-asset factory's graph), extracted.
TSharedPtr<FJsonObject> CreateModule(FAutomationTestBase& Test, const FTarget& Target, const TCHAR* Usage = TEXT("Module"))
{
	const TSharedPtr<FJsonObject> Document = Parse(FString::Printf(TEXT(R"json({"SchemaVersion": 1, "Target": "%s",
		"Class": "/Script/Niagara.NiagaraScript", "Action": "Create", "Definitions": {}, "Properties": {},
		"Body": {"Usage": "%s"}})json"), *Target.PackageName, Usage));
	const FAssetDocumentResult Created = ApplyDocument(Test, Target, Document, TEXT("create script"));
	if (!Test.TestTrue(TEXT("a script is created from a document"), Created.IsSuccess()))
	{
		return nullptr;
	}
	return Extract(Test, Target, TEXT("created script"));
}
} // namespace
} // namespace AssetDocumentNiagaraTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraScriptRoundTripTest, "UEAssetDoc.Niagara.Script.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraScriptRoundTripTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraTests;
	int32 Present = 0;
	for (const TCHAR* Path : ScriptRoundTripTier)
	{
		UObject* Source = LoadCorpus(Path);
		if (Source == nullptr)
		{
			AddInfo(FString::Printf(TEXT("NiagaraCorpus absent in this install: %s"), Path));
			continue;
		}
		++Present;
		const FString Label = FPackageName::ObjectPathToObjectName(FString(Path));
		const FTarget First(Label.Left(30));
		const FTarget Fresh(FString(TEXT("RT_")) + Label.Left(30));
		ON_SCOPE_EXIT
		{
			Fresh.Cleanup();
			First.Cleanup();
		};
		if (!TestNotNull(*FString::Printf(TEXT("%s: duplicated"), *Label), DuplicateTo(*Source, First)))
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Extracted = Extract(*this, First, Label);
		if (!Extracted.IsValid())
		{
			continue;
		}
		const double Start = FPlatformTime::Seconds();
		const FAssetDocumentResult Created = ApplyDocument(*this, Fresh, Retext(Extracted, First, Fresh, TEXT("Create")), Label + TEXT(" create"));
		const double CreateMs = Milliseconds(Start);
		// Nodes the graph menu no longer offers (the If node, hidden by the editor settings; the Usage Selector, which
		// the menu never lists; VM fast path calls, listed only with fx.AllowFastPathFunctionLibrary): the editor
		// cannot make them, nor can a document.
		TArray<FString> MenuLess;
		for (const TSharedPtr<FJsonValue>& Node : List(Child(Body(Extracted), TEXT("Graph")), TEXT("Nodes")))
		{
			const FString Action = Node->AsObject()->GetStringField(TEXT("Action"));
			if (Action == TEXT("Niagara:If") || Action == TEXT("Niagara:UsageSelector") || Action.StartsWith(TEXT("Niagara:Function:Signature:")))
			{
				MenuLess.AddUnique(Action);
			}
		}
		if (!MenuLess.IsEmpty())
		{
			TestTrue(*FString::Printf(TEXT("%s: a node the graph menu does not offer is refused (Graph.ActionNotFound)"), *Label),
				!Created.IsSuccess() && HasDiagnostic(Created, TEXT("Graph.ActionNotFound")));
			TestFalse(*FString::Printf(TEXT("%s: the refused create leaves no asset"), *Label), Fresh.Exists());
			AddInfo(FString::Printf(TEXT("%s: uses %s, which the graph menu does not offer (refused, not round tripped)"),
				*Label, *FString::Join(MenuLess, TEXT(", "))));
			continue;
		}
		if (!Created.IsSuccess() && HasDiagnostic(Created, TEXT("Niagara.InvalidPinName")))
		{
			// A pin name the editor no longer gives (a Select node output named after an older type name, e.g.
			// "Vector" where the Add pin now names it "Vector3f" and the pin cannot be renamed).
			TestFalse(*FString::Printf(TEXT("%s: the refused create leaves no asset"), *Label), Fresh.Exists());
			AddInfo(FString::Printf(TEXT("%s: has a pin name the editor no longer gives (refused, not round tripped): %s"),
				*Label, *Created.Message));
			UE_LOG(LogTemp, Display, TEXT("AssetDocumentNiagaraTests %s: legacy pin name, not round tripped"), *Label);
			continue;
		}
		if (!TestTrue(*FString::Printf(TEXT("%s: the extract creates a new script"), *Label), Created.IsSuccess()))
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Again = Extract(*this, Fresh, Label + TEXT(" created"));
		TestTextEqual(*this, FString::Printf(TEXT("%s: the created script reads as its document"), *Label), Mapped(Again, Fresh, First),
			Content(Extracted));
		const double ReapplyStart = FPlatformTime::Seconds();
		TestNoOpReapply(*this, Fresh, Label + TEXT(" created"));
		const double ReapplyMs = Milliseconds(ReapplyStart);
		TestReload(*this, Fresh, Label + TEXT(" created"));
		AddInfo(FString::Printf(TEXT("NiagaraTiming %s script roundtrip create=%.1fms reapply=%.1fms"), *Label, CreateMs, ReapplyMs));
	}
	TestTrue(TEXT("script tier present"), Present >= 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraScriptFreeEditsTest, "UEAssetDoc.Niagara.Script.FreeEdits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraScriptFreeEditsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraTests;
	for (const TCHAR* Path : {TEXT("/Niagara/Modules/Spawn/Location/V2/ShapeLocation.ShapeLocation"),
			TEXT("/Niagara/Modules/Update/Camera/CameraOffset.CameraOffset"), DefaultModule})
	{
		UObject* Source = LoadCorpus(Path);
		if (Source == nullptr)
		{
			continue;
		}
		const FString Label = FPackageName::ObjectPathToObjectName(FString(Path));
		const FTarget Editor(FString(TEXT("FE_E_")) + Label.Left(30));
		const FTarget Document(FString(TEXT("FE_D_")) + Label.Left(30));
		ON_SCOPE_EXIT
		{
			Editor.Cleanup();
			Document.Cleanup();
		};
		UNiagaraScript* EditorCopy = Cast<UNiagaraScript>(DuplicateTo(*Source, Editor));
		if (!TestNotNull(TEXT("editor copy"), EditorCopy) || !TestNotNull(TEXT("document copy"), DuplicateTo(*Source, Document)))
		{
			continue;
		}
		const FString Edits = EditLikeTheEditor(*EditorCopy);
		AddInfo(FString::Printf(TEXT("%s: editor edits: %s"), *Label, *Edits));
		SaveTarget(Editor);
		const TSharedPtr<FJsonObject> Edited = Extract(*this, Editor, Label + TEXT(" editor"));
		const TSharedPtr<FJsonObject> Wanted = Retext(Edited, Editor, Document, TEXT("Update"));
		if (Wanted.IsValid() && !List(Body(Wanted), TEXT("Versions")).IsEmpty())
		{
			// The script editor edits the exposed version of a versioned script in place: the document opts in.
			Body(Wanted)->SetBoolField(TEXT("EditExposedVersion"), true);
		}
		const double Start = FPlatformTime::Seconds();
		const FAssetDocumentResult Applied = ApplyDocument(*this, Document, Wanted, Label + TEXT(" document"));
		AddInfo(FString::Printf(TEXT("NiagaraTiming %s script free edit apply=%.1fms"), *Label, Milliseconds(Start)));
		if (!TestTrue(*FString::Printf(TEXT("%s: the editor's edits apply as a document"), *Label), Applied.IsSuccess()))
		{
			continue;
		}
		TestTextEqual(*this, FString::Printf(TEXT("%s: document and editor give the same script"), *Label),
			Mapped(Extract(*this, Document, Label + TEXT(" document")), Document, Editor), Content(Edited));
		TestNoOpReapply(*this, Document, Label + TEXT(" document"));
		TestReload(*this, Document, Label + TEXT(" document"));
		TestFalse(*FString::Printf(TEXT("%s: no undo history references the script after an apply"), *Label),
			UE::AssetDocument::UndoHistory::ReferencesPackage(Document.PackageName));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraScriptNewSettingsValueTest, "UEAssetDoc.Niagara.Script.NewSettingsValue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// The read lists only the Settings that differ from a new script's. A Description the module did not have
// before was compared with nothing, so the apply said "unchanged; nothing to apply" and dropped it. A value the
// read leaves out is now compared with the new script's default: a new value is a Diff and is applied, the
// default itself is not a difference.
bool FAssetDocumentNiagaraScriptNewSettingsValueTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraTests;
	UObject* Source = LoadCorpus(DefaultModule);
	if (!TestNotNull(TEXT("DefaultModule"), Source))
	{
		return false;
	}
	const FTarget Target(TEXT("NSV_DefaultModule"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
	};
	UNiagaraScript* Script = Cast<UNiagaraScript>(DuplicateTo(*Source, Target));
	if (!TestNotNull(TEXT("duplicate"), Script))
	{
		return false;
	}
	SaveTarget(Target);
	const TSharedPtr<FJsonObject> Extracted = Extract(*this, Target, TEXT("extract"));
	if (!TestTrue(TEXT("extract"), Extracted.IsValid()) || !TestNotNull(TEXT("Body"), Body(Extracted).Get()))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Settings = Child(Body(Extracted), TEXT("Settings"));
	if (!Settings.IsValid())
	{
		Settings = MakeShared<FJsonObject>();
		Body(Extracted)->SetObjectField(TEXT("Settings"), Settings);
	}
	TestFalse(TEXT("the read leaves the default Description out"), Settings->HasField(TEXT("Description")));

	// The Service's Diff payload lists the differences under "changed".
	auto Changed = [](const FAssetDocumentResult& Result)
	{
		const TArray<TSharedPtr<FJsonValue>>* Changes = nullptr;
		return Result.IsSuccess() && Result.Payload.IsValid() && Result.Payload->TryGetArrayField(TEXT("changed"), Changes)
			? Changes->Num() : INDEX_NONE;
	};
	const TSharedPtr<FJsonObject> AtDefault = Retargeted(Clone(Extracted), Target, TEXT("Update"));
	Child(Body(AtDefault), TEXT("Settings"))->SetStringField(TEXT("Description"), TEXT(""));
	const FAssetDocumentResult DefaultDiff = DiffDocument(AtDefault);
	TestTrue(TEXT("a listed value equal to the new script's is no difference"), Changed(DefaultDiff) == 0);

	const TCHAR* const Text = TEXT("Added by a document");
	const TSharedPtr<FJsonObject> Wanted = Retargeted(Clone(Extracted), Target, TEXT("Update"));
	Child(Body(Wanted), TEXT("Settings"))->SetStringField(TEXT("Description"), Text);
	const FAssetDocumentResult Diff = DiffDocument(Wanted);
	TestTrue(TEXT("a new Description is a difference"), Changed(Diff) > 0);
	const FAssetDocumentResult Applied = ApplyDocument(*this, Target, Wanted, TEXT("new Description"));
	TestTrue(TEXT("the new Description applies"), Applied.IsSuccess());
	TestTrue(TEXT("the apply saves the script"), Applied.bSavedAsset);
	Script = Cast<UNiagaraScript>(Target.Get());
	const FVersionedNiagaraScriptData* Data = Script != nullptr ? ExposedData(*Script) : nullptr;
	TestTrue(TEXT("the script has the Description"), Data != nullptr && Data->Description.ToString() == Text);
	TestNoOpReapply(*this, Target, TEXT("new Description"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraScriptManageVariablesTest, "UEAssetDoc.Niagara.Script.ManageScriptVariables",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraScriptManageVariablesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraTests;
	UObject* Source = LoadCorpus(TEXT("/Niagara/Modules/Update/Camera/CameraOffset.CameraOffset"));
	if (Source == nullptr)
	{
		return true;
	}
	const FTarget Copy(TEXT("MSV"));
	ON_SCOPE_EXIT
	{
		Copy.Cleanup();
	};
	if (!TestNotNull(TEXT("copy"), DuplicateTo(*Source, Copy)))
	{
		return true;
	}
	const TSharedPtr<FJsonObject> Document = Extract(*this, Copy, TEXT("before"));
	// Drop the last Map Get output pin whose parameter only that pin uses (its links go with it): the parameter
	// leaves the panel, as the next load would make it.
	const TSharedPtr<FJsonObject> Graph = Child(Body(Document), TEXT("Graph"));
	FString Dropped;
	for (const TSharedPtr<FJsonValue>& Entry : List(Graph, TEXT("Nodes")))
	{
		const TSharedPtr<FJsonObject> Node = Entry->AsObject();
		if (Node->GetStringField(TEXT("Action")) != TEXT("Niagara:MapGet") || !Child(Node, TEXT("Pins")).IsValid())
		{
			continue;
		}
		TArray<TSharedPtr<FJsonValue>> Outputs = List(Child(Node, TEXT("Pins")), TEXT("Outputs"));
		for (int32 Index = Outputs.Num() - 1; Index >= 0 && Dropped.IsEmpty(); --Index)
		{
			const TSharedPtr<FJsonObject> Pin = Outputs[Index]->AsObject();
			const FString Name = Pin.IsValid() ? Pin->GetStringField(TEXT("Name")) : Outputs[Index]->AsString();
			if (!Name.StartsWith(TEXT("Module.")))
			{
				continue;
			}
			const FString Endpoint = Node->GetStringField(TEXT("Id")) + TEXT(":") + Name;
			TArray<TSharedPtr<FJsonValue>> Links;
			for (const TSharedPtr<FJsonValue>& Link : List(Graph, TEXT("Links")))
			{
				if (Link->AsObject()->GetStringField(TEXT("From")) != Endpoint)
				{
					Links.Add(Link);
				}
			}
			Graph->SetArrayField(TEXT("Links"), Links);
			Outputs.RemoveAt(Index);
			Child(Node, TEXT("Pins"))->SetArrayField(TEXT("Outputs"), Outputs);
			Dropped = Name;
		}
		if (!Dropped.IsEmpty())
		{
			break;
		}
	}
	if (!TestFalse(TEXT("a module parameter pin to drop"), Dropped.IsEmpty()))
	{
		return true;
	}
	TArray<TSharedPtr<FJsonValue>> Kept;
	for (const TSharedPtr<FJsonValue>& Entry : List(Body(Document), TEXT("Parameters")))
	{
		if (Entry->AsObject()->GetStringField(TEXT("Name")) != Dropped)
		{
			Kept.Add(Entry);
		}
	}
	Body(Document)->SetArrayField(TEXT("Parameters"), Kept);
	const FAssetDocumentResult Applied = ApplyDocument(*this, Copy, Retargeted(Document, Copy, TEXT("Update")), TEXT("drop parameter"));
	AddInfo(FString::Printf(TEXT("dropped %s"), *Dropped));
	// The parameter is still used elsewhere when the compile or a remaining pin keeps it; either way the read is
	// what a reload gives.
	if (Applied.IsSuccess())
	{
		TestReload(*this, Copy, TEXT("after the parameter edit"));
	}
	else
	{
		AddInfo(TEXT("the edit does not compile (the parameter feeds required logic): rolled back"));
		TestTrue(TEXT("a failed edit is a compile failure"), HasDiagnostic(Applied, TEXT("Niagara.CompileFailed")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraScriptVariableRuleTest, "UEAssetDoc.Niagara.Script.ScriptVariableRule",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraScriptVariableRuleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FARFilter Filter;
	Filter.ClassPaths.Add(UNiagaraScript::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(TEXT("/Niagara"));
	Filter.bRecursivePaths = true;
	TArray<FAssetData> Found;
	FAssetRegistryModule::GetRegistry().GetAssets(Filter, Found);
	Found.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });
	constexpr int32 MaxScripts = 80;
	int32 Checked = 0;
	int32 BindingDefaults = 0;
	for (const FAssetData& Asset : Found)
	{
		if (Checked >= MaxScripts)
		{
			break;
		}
		const UNiagaraScript* Script = Cast<UNiagaraScript>(Asset.GetAsset());
		UNiagaraGraph* Loaded = Script != nullptr ? UE::AssetDocument::Niagara::ScriptEditGraph(*Script) : nullptr;
		if (Loaded == nullptr || !(Script->IsModuleScript() || Script->IsFunctionScript() || Script->IsDynamicInputScript()))
		{
			continue;
		}
		++Checked;
		// The rule runs on a copy of the graph so the engine's own assets stay untouched.
		UNiagaraGraph* Graph = DuplicateObject<UNiagaraGraph>(Loaded, GetTransientPackage());
		const FString Name = Asset.PackageName.ToString();
		UNiagaraGraph::FScriptVariableMap& Entries = Graph->GetAllMetaData();
		for (const TPair<FNiagaraVariable, TObjectPtr<UNiagaraScriptVariable>>& Entry : Entries)
		{
			BindingDefaults += Entry.Value != nullptr && Entry.Value->DefaultMode == ENiagaraDefaultMode::Binding ? 1 : 0;
		}
		TArray<FString> Removed, Created;
		const bool bChanged = UE::AssetDocument::Niagara::ManageScriptVariables(*Graph, &Removed, &Created);
		TestFalse(*FString::Printf(TEXT("%s: the loaded graph is left as it is (removed %s, created %s)"), *Name,
			*FString::Join(Removed, TEXT(", ")), *FString::Join(Created, TEXT(", "))), bChanged);

		// One referenced entry goes missing and an unreferenced one appears: the rule restores the first and drops the
		// second, and the panel is the loaded one again.
		TArray<FNiagaraVariable> Keys;
		Entries.GetKeys(Keys);
		const FNiagaraVariable* Referenced = Keys.FindByPredicate([&Entries](const FNiagaraVariable& Key)
		{
			const UNiagaraScriptVariable* Variable = Entries.FindRef(Key);
			// Not an entry whose own default binding is what keeps another parameter referenced.
			return Variable != nullptr && !Variable->GetIsStaticSwitch() && Variable->DefaultMode != ENiagaraDefaultMode::Binding;
		});
		if (Referenced == nullptr)
		{
			continue;
		}
		const FNiagaraVariable Restored = *Referenced;
		const FNiagaraVariable Stale(FNiagaraTypeDefinition::GetFloatDef(), TEXT("Module.UnreferencedByAnyPin"));
		Entries.Remove(Restored);
		UNiagaraScriptVariable* StaleEntry = NewObject<UNiagaraScriptVariable>(Graph, NAME_None, RF_Transactional);
		StaleEntry->Init(Stale, FNiagaraVariableMetaData());
		Entries.Add(Stale, StaleEntry);
		Removed.Reset();
		Created.Reset();
		TestTrue(*FString::Printf(TEXT("%s: the perturbed panel changes"), *Name),
			UE::AssetDocument::Niagara::ManageScriptVariables(*Graph, &Removed, &Created));
		TestTrue(*FString::Printf(TEXT("%s: the unreferenced entry is dropped (removed %s)"), *Name, *FString::Join(Removed, TEXT(", "))),
			Removed.Num() == 1 && Removed[0] == Stale.GetName().ToString());
		TestTrue(*FString::Printf(TEXT("%s: the referenced entry is created again (created %s)"), *Name, *FString::Join(Created, TEXT(", "))),
			Created.Num() == 1 && Created[0] == Restored.GetName().ToString());
		TArray<FNiagaraVariable> After;
		Entries.GetKeys(After);
		TestEqual(*FString::Printf(TEXT("%s: the same parameters as loaded"), *Name), TSet<FNiagaraVariable>(After).Num(), Keys.Num());
		TestTrue(*FString::Printf(TEXT("%s: every loaded parameter is back"), *Name), Algo::AllOf(Keys, [&Entries](const FNiagaraVariable& Key)
		{
			return Entries.Contains(Key);
		}));
		const UNiagaraScriptVariable* Recreated = Entries.FindRef(Restored);
		TestTrue(*FString::Printf(TEXT("%s: the recreated entry is no static switch"), *Name), Recreated != nullptr && !Recreated->GetIsStaticSwitch());
	}
	AddInfo(FString::Printf(TEXT("%d scripts, %d entries with a default binding"), Checked, BindingDefaults));
	TestTrue(TEXT("scripts were checked"), Checked > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraScriptRefusalsTest, "UEAssetDoc.Niagara.Script.Refusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraScriptRefusalsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraTests;
	const FTarget Module(TEXT("REF_Module"));
	const FTarget User(TEXT("REF_System"));
	ON_SCOPE_EXIT
	{
		User.Cleanup();
		Module.Cleanup();
	};
	const TSharedPtr<FJsonObject> Created = CreateModule(*this, Module);
	if (!Created.IsValid())
	{
		return true;
	}
	const FString Before = Content(Created);
	// Usage is immutable once the script exists.
	{
		const TSharedPtr<FJsonObject> Usage = Retargeted(Created, Module, TEXT("Update"));
		Body(Usage)->SetStringField(TEXT("Usage"), TEXT("Function"));
		const FAssetDocumentResult Refused = ApplyDocument(*this, Module, Usage, TEXT("usage"));
		TestTrue(TEXT("usage change refused"), !Refused.IsSuccess() && HasDiagnostic(Refused, TEXT("Niagara.UsageImmutable")));
	}
	// A parameter the graph does not use.
	{
		const TSharedPtr<FJsonObject> Unused = Retargeted(Created, Module, TEXT("Update"));
		TArray<TSharedPtr<FJsonValue>> Entries = List(Body(Unused), TEXT("Parameters"));
		Entries.Add(MakeShared<FJsonValueObject>(Parse(TEXT(R"json({"Name": "Module.NotInTheGraph", "Default": 1.0})json"))));
		Body(Unused)->SetArrayField(TEXT("Parameters"), Entries);
		const FAssetDocumentResult Refused = ApplyDocument(*this, Module, Unused, TEXT("unused parameter"));
		TestTrue(TEXT("unused parameter refused"), !Refused.IsSuccess() && HasDiagnostic(Refused, TEXT("Niagara.ParameterNotReferenced")));
	}
	// A custom HLSL node that does not compile: the apply fails at the node and rolls back.
	{
		const TSharedPtr<FJsonObject> Broken = Retargeted(Created, Module, TEXT("Update"));
		const TSharedPtr<FJsonObject> Graph = Child(Body(Broken), TEXT("Graph"));
		TArray<TSharedPtr<FJsonValue>> Nodes = List(Graph, TEXT("Nodes"));
		Nodes.Add(MakeShared<FJsonValueObject>(Parse(TEXT(R"json({"Id": "BadHlsl", "Action": "Niagara:CustomHlsl", "Position": [0, 400],
			"Pins": {"Inputs": [{"Name": "Amount", "Type": "float"}], "Outputs": [{"Name": "Result", "Type": "float"}]},
			"Properties": {"CustomHlsl": "Result = Amount * AssetDocumentUndefinedSymbol;"}})json"))));
		Graph->SetArrayField(TEXT("Nodes"), Nodes);
		const FAssetDocumentResult Refused = ApplyDocument(*this, Module, Broken, TEXT("bad hlsl"));
		const bool bFailed = !Refused.IsSuccess() && HasDiagnostic(Refused, TEXT("Niagara.CompileFailed"));
		// An unconnected custom HLSL node is not compiled; the error then needs the node wired in: either way the
		// asset is unchanged after a failure.
		AddInfo(FString::Printf(TEXT("bad hlsl: %s"), bFailed ? TEXT("compile failed") : TEXT("applied (unconnected node not compiled)")));
		if (bFailed)
		{
			TestTextEqual(*this, TEXT("rolled back"), Content(Extract(*this, Module, TEXT("after bad hlsl"))), Before);
		}
		else
		{
			ApplyDocument(*this, Module, Retargeted(Created, Module, TEXT("Update")), TEXT("restore"));
		}
	}
	// Versions: enable versioning (1.0), then the exposed version rule.
	{
		const TSharedPtr<FJsonObject> Versioned = Retargeted(Created, Module, TEXT("Update"));
		Body(Versioned)->SetArrayField(TEXT("Versions"), {MakeShared<FJsonValueObject>(Parse(TEXT(R"json({"Number": "1.0"})json")))});
		TestTrue(TEXT("versioning enabled by a document"), ApplyDocument(*this, Module, Versioned, TEXT("enable versioning")).IsSuccess());
		const TSharedPtr<FJsonObject> Now = Extract(*this, Module, TEXT("versioned"));
		TestEqual(TEXT("one version listed"), List(Body(Now), TEXT("Versions")).Num(), 1);
		const TSharedPtr<FJsonObject> Edit = Retargeted(Now, Module, TEXT("Update"));
		const TSharedPtr<FJsonObject> Settings = Child(Body(Edit), TEXT("Settings"));
		Settings->SetStringField(TEXT("Category"), TEXT("Edited"));
		const FAssetDocumentResult Refused = ApplyDocument(*this, Module, Edit, TEXT("edit exposed"));
		TestTrue(TEXT("editing the exposed version is refused"), !Refused.IsSuccess() && HasDiagnostic(Refused, TEXT("Niagara.ExposedVersionEdit")));
		TArray<TSharedPtr<FJsonValue>> Versions = List(Body(Edit), TEXT("Versions"));
		Versions[0]->AsObject()->RemoveField(TEXT("Exposed"));
		Versions.Add(MakeShared<FJsonValueObject>(Parse(TEXT(R"json({"Number": "1.1", "Exposed": true, "ChangeDescription": "Edited category"})json"))));
		Body(Edit)->SetArrayField(TEXT("Versions"), Versions);
		const FAssetDocumentResult NewVersion = ApplyDocument(*this, Module, Edit, TEXT("new exposed version"));
		TestTrue(TEXT("a new exposed version takes the edit"), NewVersion.IsSuccess());
		const TSharedPtr<FJsonObject> After = Extract(*this, Module, TEXT("after new version"));
		TestEqual(TEXT("two versions"), List(Body(After), TEXT("Versions")).Num(), 2);
		const TSharedPtr<FJsonObject> Exposed = Child(Body(After), TEXT("Settings"));
		TestTrue(TEXT("the exposed version has the edit"), Exposed.IsValid() && Exposed->GetStringField(TEXT("Category")) == TEXT("Edited"));
		const TSharedPtr<FJsonObject> OptIn = Retargeted(After, Module, TEXT("Update"));
		OptIn->GetObjectField(TEXT("Body"))->SetBoolField(TEXT("EditExposedVersion"), true);
		Child(Body(OptIn), TEXT("Settings"))->SetStringField(TEXT("Category"), TEXT("Edited again"));
		TestTrue(TEXT("EditExposedVersion edits the exposed version"), ApplyDocument(*this, Module, OptIn, TEXT("opt in")).IsSuccess());
		TestNoOpReapply(*this, Module, TEXT("versioned module"));
	}
	// Dependents: a system using the module.
	{
		const TSharedPtr<FJsonObject> System = Parse(FString::Printf(TEXT(R"json({"SchemaVersion": 1, "Target": "%s",
			"Class": "/Script/Niagara.NiagaraSystem", "Action": "Create", "Definitions": {}, "Properties": {},
			"Body": {"Emitters": [{"Name": "Sparks", "From": "Minimal",
				"ParticleUpdate": [{"Name": "%s", "Script": "%s"}]}]}})json"), *User.PackageName, *Module.AssetName, *Module.PackageName));
		if (!TestTrue(TEXT("a system uses the module"), ApplyDocument(*this, User, System, TEXT("user system")).IsSuccess()))
		{
			return true;
		}
		UObject* UserAsset = User.Get();
		if (UserAsset != nullptr)
		{
			UserAsset->GetOutermost()->MarkPackageDirty();
		}
		const TSharedPtr<FJsonObject> Now = Extract(*this, Module, TEXT("module with a user"));
		const TSharedPtr<FJsonObject> Edit = Retargeted(Now, Module, TEXT("Update"));
		Body(Edit)->SetBoolField(TEXT("EditExposedVersion"), true);
		Child(Body(Edit), TEXT("Settings"))->SetStringField(TEXT("Category"), TEXT("Dependents"));
		const FAssetDocumentResult Refused = ApplyDocument(*this, Module, Edit, TEXT("dirty dependent"));
		TestTrue(TEXT("a dependent with unsaved changes refuses"), !Refused.IsSuccess() && HasDiagnostic(Refused, TEXT("Niagara.DependentHasUnsavedChanges")));
		TestTextEqual(*this, TEXT("nothing changed"), Content(Extract(*this, Module, TEXT("after refusal"))), Content(Now));
		SaveTarget(User);
		const FAssetDocumentResult Applied = ApplyDocument(*this, Module, Edit, TEXT("saved dependent"));
		TestTrue(TEXT("a saved dependent is refreshed"), Applied.IsSuccess() && HasDiagnostic(Applied, TEXT("Niagara.DependentsUpdated")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraScriptOrbitTest, "UEAssetDoc.Niagara.Script.OrbitAroundPoint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraScriptOrbitTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraTests;
	const FTarget Module(TEXT("OrbitAroundPoint"));
	const FTarget System(TEXT("NS_Orbit"));
	ON_SCOPE_EXIT
	{
		System.Cleanup();
		Module.Cleanup();
	};
	const TSharedPtr<FJsonObject> Created = CreateModule(*this, Module);
	if (!Created.IsValid())
	{
		return true;
	}
	AddInfo(FString::Printf(TEXT("new module: %s"), *Serialize(Body(Created)).Left(4000)));
	// The module's graph from the factory: input map -> Map Get / Map Set -> output. Keep those nodes (by their
	// ids), add the parameters and a custom HLSL node, and wire it.
	const TSharedPtr<FJsonObject> Graph = Child(Body(Created), TEXT("Graph"));
	const TSharedPtr<FJsonObject> Get = NodeWithAction(Graph, TEXT("Niagara:MapGet"));
	const TSharedPtr<FJsonObject> Set = NodeWithAction(Graph, TEXT("Niagara:MapSet"));
	if (!TestTrue(TEXT("the new module has a Map Get and a Map Set"), Get.IsValid() && Set.IsValid()))
	{
		return true;
	}
	const FString GetId = Get->GetStringField(TEXT("Id"));
	const FString SetId = Set->GetStringField(TEXT("Id"));
	const TSharedPtr<FJsonObject> Document = Retargeted(Created, Module, TEXT("Update"));
	const TSharedPtr<FJsonObject> Out = Child(Body(Document), TEXT("Graph"));
	NodeWithAction(Out, TEXT("Niagara:MapGet"))->SetObjectField(TEXT("Pins"), Parse(TEXT(R"json({"Outputs": [
		{"Name": "Module.Center", "Type": "Position"}, {"Name": "Module.Radius", "Type": "float"},
		{"Name": "Module.Speed", "Type": "float"}, {"Name": "Engine.Time", "Type": "float"}]})json")));
	NodeWithAction(Out, TEXT("Niagara:MapSet"))->SetObjectField(TEXT("Pins"), Parse(TEXT(R"json({"Inputs": [
		{"Name": "Particles.Position", "Type": "Position"}]})json")));
	TArray<TSharedPtr<FJsonValue>> Nodes = List(Out, TEXT("Nodes"));
	Nodes.Add(MakeShared<FJsonValueObject>(Parse(TEXT(R"json({"Id": "Orbit", "Action": "Niagara:CustomHlsl", "Position": [300, 200],
		"Comment": "Circle around Center",
		"Pins": {"Inputs": [{"Name": "Center", "Type": "Position"}, {"Name": "Radius", "Type": "float"},
		                    {"Name": "Speed", "Type": "float"}, {"Name": "Time", "Type": "float"}],
		         "Outputs": [{"Name": "Position", "Type": "Position"}]},
		"Properties": {"CustomHlsl": "float Angle = Time * Speed;\nPosition = Center + float3(cos(Angle), sin(Angle), 0) * Radius;"}})json"))));
	Out->SetArrayField(TEXT("Nodes"), Nodes);
	TArray<TSharedPtr<FJsonValue>> Links = List(Out, TEXT("Links"));
	for (const TCHAR* Pair : {TEXT("Module.Center|Center"), TEXT("Module.Radius|Radius"), TEXT("Module.Speed|Speed"), TEXT("Engine.Time|Time")})
	{
		FString From;
		FString To;
		FString(Pair).Split(TEXT("|"), &From, &To);
		const TSharedPtr<FJsonObject> Link = MakeShared<FJsonObject>();
		Link->SetStringField(TEXT("From"), GetId + TEXT(":") + From);
		Link->SetStringField(TEXT("To"), FString(TEXT("Orbit:")) + To);
		Links.Add(MakeShared<FJsonValueObject>(Link));
	}
	const TSharedPtr<FJsonObject> Write = MakeShared<FJsonObject>();
	Write->SetStringField(TEXT("From"), TEXT("Orbit:Position"));
	Write->SetStringField(TEXT("To"), SetId + TEXT(":Particles.Position"));
	Links.Add(MakeShared<FJsonValueObject>(Write));
	Out->SetArrayField(TEXT("Links"), Links);
	Body(Document)->SetArrayField(TEXT("Parameters"), {
		MakeShared<FJsonValueObject>(Parse(TEXT(R"json({"Name": "Module.Radius", "Default": 100.0, "Meta": {"Description": "Orbit radius (cm)"}})json"))),
		MakeShared<FJsonValueObject>(Parse(TEXT(R"json({"Name": "Module.Speed", "Default": 2.0, "Meta": {"Description": "Radians per second"}})json")))});
	Child(Body(Document), TEXT("Settings"))->SetStringField(TEXT("Description"), TEXT("Moves particles on a circle around a point."));
	const double Start = FPlatformTime::Seconds();
	const FAssetDocumentResult Applied = ApplyDocument(*this, Module, Document, TEXT("orbit module"));
	AddInfo(FString::Printf(TEXT("NiagaraTiming OrbitAroundPoint module apply=%.1fms"), Milliseconds(Start)));
	if (!TestTrue(TEXT("OrbitAroundPoint applies and compiles"), Applied.IsSuccess()))
	{
		return true;
	}
	const TSharedPtr<FJsonObject> Written = Extract(*this, Module, TEXT("orbit module"));
	AddInfo(FString::Printf(TEXT("orbit module: %s"), *Serialize(Body(Written)).Left(6000)));
	TestNotNull(TEXT("Module.Radius is a parameter"), Named(List(Body(Written), TEXT("Parameters")), TEXT("Module.Radius")).Get());
	TestNoOpReapply(*this, Module, TEXT("orbit module"));
	TestReload(*this, Module, TEXT("orbit module"));
	const TSharedPtr<FJsonObject> UserDocument = Parse(FString::Printf(TEXT(R"json({"SchemaVersion": 1, "Target": "%s",
		"Class": "/Script/Niagara.NiagaraSystem", "Action": "Create", "Definitions": {}, "Properties": {},
		"Body": {"Emitters": [{"Name": "Orbiters", "From": "Minimal",
			"ParticleUpdate": [{"Name": "%s", "Script": "%s", "Inputs": {"Radius": 250.0, "Speed": 4.0}}]}]}})json"),
		*System.PackageName, *Module.AssetName, *Module.PackageName));
	const FAssetDocumentResult Used = ApplyDocument(*this, System, UserDocument, TEXT("orbit system"));
	TestTrue(TEXT("a system uses OrbitAroundPoint and compiles"), Used.IsSuccess());
	if (Used.IsSuccess())
	{
		const TSharedPtr<FJsonObject> UserExtract = Extract(*this, System, TEXT("orbit system"));
		AddInfo(FString::Printf(TEXT("orbit system: %s"), *Serialize(Body(UserExtract)).Left(3000)));
		TestNoOpReapply(*this, System, TEXT("orbit system"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraScratchPadSystemsTest, "UEAssetDoc.Niagara.ScratchPad.Systems",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraScratchPadSystemsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraTests;
	// Grid3D_Gas_Fire's scratch pad calls length2, which the Vector VM shader compile of some engines does not define.
	ExpectVectorVMCompileErrors(*this, TEXT("length2"));
	int32 Present = 0;
	for (const TCHAR* Path : PadSystems)
	{
		UObject* Source = LoadCorpus(Path);
		if (Source == nullptr)
		{
			AddInfo(FString::Printf(TEXT("NiagaraCorpus absent in this install: %s"), Path));
			continue;
		}
		++Present;
		const FString Label = FPackageName::ObjectPathToObjectName(FString(Path));
		const FTarget Editor(FString(TEXT("SP_E_")) + Label.Left(30));
		const FTarget Document(FString(TEXT("SP_D_")) + Label.Left(30));
		ON_SCOPE_EXIT
		{
			Editor.Cleanup();
			Document.Cleanup();
		};
		UNiagaraSystem* EditorCopy = Cast<UNiagaraSystem>(DuplicateTo(*Source, Editor));
		if (!TestNotNull(TEXT("editor copy"), EditorCopy) || !TestNotNull(TEXT("document copy"), DuplicateTo(*Source, Document)))
		{
			continue;
		}
		double ExtractMs = 0.0;
		const TSharedPtr<FJsonObject> Original = Extract(*this, Document, Label + TEXT(" original"), &ExtractMs);
		int32 Pads = List(Body(Original), TEXT("ScratchPads")).Num();
		for (const TSharedPtr<FJsonValue>& Emitter : List(Body(Original), TEXT("Emitters")))
		{
			Pads += List(Emitter->AsObject(), TEXT("ScratchPads")).Num();
		}
		AddInfo(FString::Printf(TEXT("%s: %d scratch pads (%d system level)"), *Label, Pads, List(Body(Original), TEXT("ScratchPads")).Num()));
		double Start = FPlatformTime::Seconds();
		TestNoOpReapply(*this, Document, Label);
		const double ReapplyMs = Milliseconds(Start);
		// The editor: the first scratch pad (system level, else the first emitter's) edited in place, then the
		// pad's Apply (its calls refresh) and the system's compile.
		UNiagaraScript* Pad = !EditorCopy->ScratchPadScripts.IsEmpty() ? EditorCopy->ScratchPadScripts[0].Get() : nullptr;
		for (const FNiagaraEmitterHandle& Handle : EditorCopy->GetEmitterHandles())
		{
			const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
			if (Pad == nullptr && Data != nullptr && Data->ScratchPads != nullptr && !Data->ScratchPads->Scripts.IsEmpty())
			{
				Pad = Data->ScratchPads->Scripts[0].Get();
			}
		}
		double EditMs = 0.0;
		if (Pad != nullptr)
		{
			const FString Edits = EditLikeTheEditor(*Pad);
			for (TObjectIterator<UNiagaraNodeFunctionCall> It; It; ++It)
			{
				if (It->FunctionScript == Pad)
				{
					It->RefreshFromExternalChanges();
					It->MarkNodeRequiresSynchronization(TEXT("ScratchPadChangesApplied"), true);
				}
			}
			CompileAndWait(*EditorCopy);
			SaveTarget(Editor);
			AddInfo(FString::Printf(TEXT("%s: editor edits on pad %s: %s"), *Label, *Pad->GetName(), *Edits));
			const TSharedPtr<FJsonObject> Edited = Extract(*this, Editor, Label + TEXT(" editor"));
			Start = FPlatformTime::Seconds();
			const FAssetDocumentResult Applied = ApplyDocument(*this, Document, Retext(Edited, Editor, Document, TEXT("Update")), Label + TEXT(" pad document"));
			EditMs = Milliseconds(Start);
			if (TestTrue(*FString::Printf(TEXT("%s: the pad edits apply as a document"), *Label), Applied.IsSuccess()))
			{
				TestTextEqual(*this, FString::Printf(TEXT("%s: document and editor give the same pads"), *Label),
					Mapped(Extract(*this, Document, Label + TEXT(" pad document")), Document, Editor), Content(Edited));
				TestNoOpReapply(*this, Document, Label + TEXT(" after pad edit"));
				TestReload(*this, Document, Label + TEXT(" after pad edit"));
			}
		}
		AddInfo(FString::Printf(TEXT("NiagaraTiming %s scratchpads=%d extract=%.1fms reapply=%.1fms padedit=%.1fms"), *Label, Pads, ExtractMs, ReapplyMs, EditMs));
	}
	TestTrue(TEXT("RenderTargetTexturePainter present"), Present >= 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraScratchPadAuthoringTest, "UEAssetDoc.Niagara.ScratchPad.Authoring",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraScratchPadAuthoringTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraTests;
	const FTarget System(TEXT("NS_Pads"));
	ON_SCOPE_EXIT
	{
		System.Cleanup();
	};
	// 1. Add a system scratch pad module and use it from Particle Update.
	const TSharedPtr<FJsonObject> Create = Parse(FString::Printf(TEXT(R"json({"SchemaVersion": 1, "Target": "%s",
		"Class": "/Script/Niagara.NiagaraSystem", "Action": "Create", "Definitions": {}, "Properties": {},
		"Body": {"ScratchPads": [{"Name": "Swirl", "Usage": "Module"}],
		         "Emitters": [{"Name": "Sparks", "From": "Minimal", "ParticleUpdate": [{"Name": "Swirl", "Script": "ScratchPad:Swirl"}]}]}})json"),
		*System.PackageName));
	const FAssetDocumentResult Created = ApplyDocument(*this, System, Create, TEXT("add pad"));
	if (!TestTrue(TEXT("a scratch pad is added and used"), Created.IsSuccess()))
	{
		return true;
	}
	const TSharedPtr<FJsonObject> First = Extract(*this, System, TEXT("with pad"));
	const TSharedPtr<FJsonObject> Pad = Named(List(Body(First), TEXT("ScratchPads")), TEXT("Swirl"));
	if (!TestTrue(TEXT("the pad is listed with its graph"), Pad.IsValid() && Child(Pad, TEXT("Graph")).IsValid()))
	{
		return true;
	}
	AddInfo(FString::Printf(TEXT("new pad: %s"), *Serialize(Pad).Left(4000)));
	TestNotNull(TEXT("the module uses the pad"), Named(List(Named(List(Body(First), TEXT("Emitters")), TEXT("Sparks")), TEXT("ParticleUpdate")), TEXT("Swirl")).Get());
	TestNoOpReapply(*this, System, TEXT("with pad"));
	// 2. Edit the pad: a description and a node comment; rename it. As the Scratch Pad panel's rename, the modules
	//    calling it take the new name too (FixFunctionInputsFromFunctionScriptRename).
	{
		const TSharedPtr<FJsonObject> Edit = Retargeted(First, System, TEXT("Update"));
		const TSharedPtr<FJsonObject> EditPad = Named(List(Body(Edit), TEXT("ScratchPads")), TEXT("Swirl"));
		EditPad->SetStringField(TEXT("Name"), TEXT("Swirl2"));
		EditPad->SetStringField(TEXT("RenamedFrom"), TEXT("Swirl"));
		Child(EditPad, TEXT("Settings"))->SetStringField(TEXT("Description"), TEXT("A swirling scratch pad"));
		const TArray<TSharedPtr<FJsonValue>> PadNodes = List(Child(EditPad, TEXT("Graph")), TEXT("Nodes"));
		if (!PadNodes.IsEmpty())
		{
			PadNodes[0]->AsObject()->SetStringField(TEXT("Comment"), TEXT("Edited by a document"));
		}
		const TSharedPtr<FJsonObject> Module = Named(List(Named(List(Body(Edit), TEXT("Emitters")), TEXT("Sparks")), TEXT("ParticleUpdate")), TEXT("Swirl"));
		Module->SetStringField(TEXT("Name"), TEXT("Swirl2"));
		Module->SetStringField(TEXT("Script"), TEXT("ScratchPad:Swirl2"));
		const FAssetDocumentResult Edited = ApplyDocument(*this, System, Edit, TEXT("edit pad"));
		TestTrue(TEXT("the pad is renamed and edited"), Edited.IsSuccess());
		const TSharedPtr<FJsonObject> After = Extract(*this, System, TEXT("edited pad"));
		const TSharedPtr<FJsonObject> Renamed = Named(List(Body(After), TEXT("ScratchPads")), TEXT("Swirl2"));
		TestTrue(TEXT("renamed pad has the description"), Renamed.IsValid()
			&& Child(Renamed, TEXT("Settings"))->GetStringField(TEXT("Description")) == TEXT("A swirling scratch pad"));
		const TSharedPtr<FJsonObject> Using = Named(List(Named(List(Body(After), TEXT("Emitters")), TEXT("Sparks")), TEXT("ParticleUpdate")), TEXT("Swirl2"));
		TestTrue(TEXT("the module follows the rename"), Using.IsValid() && Using->GetStringField(TEXT("Script")) == TEXT("ScratchPad:Swirl2"));
		TestNoOpReapply(*this, System, TEXT("edited pad"));
	}
	// 3. Remove the module and the pad.
	{
		const TSharedPtr<FJsonObject> Remove = Retargeted(Extract(*this, System, TEXT("before delete")), System, TEXT("Update"));
		Body(Remove)->SetArrayField(TEXT("ScratchPads"), {});
		Named(List(Body(Remove), TEXT("Emitters")), TEXT("Sparks"))->SetArrayField(TEXT("ParticleUpdate"), {});
		const FAssetDocumentResult Removed = ApplyDocument(*this, System, Remove, TEXT("delete pad"));
		TestTrue(TEXT("the pad and its module are deleted"), Removed.IsSuccess());
		const TSharedPtr<FJsonObject> After = Extract(*this, System, TEXT("deleted pad"));
		TestEqual(TEXT("no scratch pads left"), List(Body(After), TEXT("ScratchPads")).Num(), 0);
		TestNoOpReapply(*this, System, TEXT("deleted pad"));
		// Last: an apply after the package was reloaded from disk is not part of this test (see the N4 report).
		TestReload(*this, System, TEXT("deleted pad"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraScriptStaticSwitchRenameTest, "UEAssetDoc.Niagara.Script.StaticSwitchRename",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraScriptStaticSwitchRenameTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraTests;
	// A module whose static switches have Details (ShapeLocation's shape switches have descriptions and edit conditions).
	UObject* Source = LoadCorpus(TEXT("/Niagara/Modules/Spawn/Location/V2/ShapeLocation.ShapeLocation"));
	if (Source == nullptr)
	{
		return true;
	}
	const FTarget Copy(TEXT("SSR"));
	ON_SCOPE_EXIT
	{
		Copy.Cleanup();
	};
	if (!TestNotNull(TEXT("copy"), DuplicateTo(*Source, Copy)))
	{
		return true;
	}
	const TSharedPtr<FJsonObject> Document = Retargeted(Extract(*this, Copy, TEXT("before")), Copy, TEXT("Update"));
	const TArray<TSharedPtr<FJsonValue>> Before = List(Body(Document), TEXT("Parameters"));
	// The static switch whose parameter has the most Details.
	TSharedPtr<FJsonObject> Switch;
	TSharedPtr<FJsonObject> OldEntry;
	for (const TSharedPtr<FJsonValue>& Entry : List(Child(Body(Document), TEXT("Graph")), TEXT("Nodes")))
	{
		FString Name;
		const TSharedPtr<FJsonObject> Node = Entry->AsObject();
		const TSharedPtr<FJsonObject> NodeProperties = Child(Node, TEXT("Properties"));
		if (!NodeProperties.IsValid() || !NodeProperties->TryGetStringField(TEXT("InputParameterName"), Name))
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Parameter = Named(Before, Name);
		if (Parameter.IsValid() && (!OldEntry.IsValid() || Serialize(Parameter).Len() > Serialize(OldEntry).Len()))
		{
			Switch = Node;
			OldEntry = Parameter;
		}
	}
	if (!TestTrue(TEXT("the script has a static switch with a listed parameter"), Switch.IsValid()))
	{
		UE_LOG(LogTemp, Display, TEXT("AssetDocumentNiagaraTests parameters: %s"), *Serialize(Body(Document)).Left(6000));
		return true;
	}
	const FString OldName = OldEntry->GetStringField(TEXT("Name"));
	const FString NewName = OldName + TEXT("Renamed");
	// The static switch Details' parameter name box; Parameters left out (kept as the editor leaves them).
	Child(Switch, TEXT("Properties"))->SetStringField(TEXT("InputParameterName"), NewName);
	Body(Document)->RemoveField(TEXT("Parameters"));
	if (!List(Body(Document), TEXT("Versions")).IsEmpty())
	{
		Body(Document)->SetBoolField(TEXT("EditExposedVersion"), true);
	}
	const FAssetDocumentResult Applied = ApplyDocument(*this, Copy, Document, TEXT("rename"));
	if (!TestTrue(TEXT("the rename applies"), Applied.IsSuccess()))
	{
		return true;
	}
	const TArray<TSharedPtr<FJsonValue>> After = List(Body(Extract(*this, Copy, TEXT("after"))), TEXT("Parameters"));
	const TSharedPtr<FJsonObject> NewEntry = Named(After, NewName);
	TestFalse(TEXT("the old name leaves the list"), Named(After, OldName).IsValid());
	if (TestTrue(TEXT("the new name is listed"), NewEntry.IsValid()))
	{
		const TSharedPtr<FJsonObject> Expected = Clone(OldEntry);
		Expected->SetStringField(TEXT("Name"), NewName);
		TestTextEqual(*this, TEXT("the renamed switch keeps its parameter's Details (UNiagaraGraph::RenameStaticSwitch)"), Serialize(Clone(NewEntry)),
			Serialize(Expected));
	}
	TestNoOpReapply(*this, Copy, TEXT("renamed switch"));
	return true;
}
#endif
