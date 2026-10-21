// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Graph ops of rig.controlrig.edit.v1 (docs/architecture/asset-document-controlrig-design.md §3.3, §3.5, §4.1, phases
// CR2 / CR3): event graphs (models) through the asset (AddModel / RemoveModel / the client's RenameModel, what My
// Blueprint's "+", delete and rename do) and the nodes of every graph (event graphs, function bodies, collapse graphs)
// with their links, pin values, bindings, pin types, aggregate pins, traits and function variable maps through
// URigVMController, the controller every graph panel, spawner and Details callback of the Control Rig editor uses.
// Undo and Python print are off (the edit primitive's own transaction covers the call); controller reporting is on
// for each op so refusals (ReportError) fail the op with the controller's message.
//
// Every op names its graph (FGraphTarget): an event graph by document name ("Rig" = the default model), or a scope
// as the read names it (["Graphs", g] / ["Functions", f], then ["Nodes", <collapse node>, "Collapse"]*). The interface
// nodes of a function or collapse graph are the nodes "Entry" and "Return". Ops:
//   graph.add {name}  graph.remove {name}  graph.rename {name, to}
//   node.add {graph, name, entry, deprecated?}   entry: one kind key (Unit (+Method), Template, Variable (+Setter;
//       a member variable, or a local variable / input of the function), Reroute, Enum, InvokeEvent, Comment (+Size,
//       Color), Function (a local function, or "<package>.<asset>:<function>": another rig's public function),
//       Collapse (an empty collapse node; its pins and nodes are later ops)), Position, Defaults (top-level unit pins,
//       set at creation); deprecated: "warn" adds a deprecated unit with a note, otherwise it is refused
//       (ControlRig.DeprecatedUnit)
//   node.remove {graph, name}  node.rename {graph, name, to}
//   node.set {graph, name, position?, size?, title?, color?, comment?, font_size?, bubble?, color_bubble?, expanded?}
//   node.types {graph, name, types}       resolve wildcard pins ({pin: type}, in order)
//   node.aggregate {graph, name, pins}    aggregate operand pins (added / removed to match)
//   node.traits {graph, name, traits}     {trait name: {Type}} (added / removed to match)
//   node.remap {graph, name, map}         a function node's variable map {inner: member variable} (complete)
//   link.add {graph, from, to}  link.break {graph, from, to}
//   pin.default {graph, pin, value}       null resets the pin to the node's default
//   pin.bind {graph, pin, variable}       "" unbinds
//   pin.array {graph, pin, size}

#include "AssetDocumentControlRig.h"
#include "AssetDocumentNameSuggestions.h"

#include "ControlRigBlueprintLegacy.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/EngineVersionComparison.h"
#include "RigVMCore/RigVMDispatchFactory.h"
#include "RigVMCore/RigVMExternalVariable.h"
#include "RigVMCore/RigVMRegistry.h"
#include "RigVMCore/RigVMStruct.h"
#include "RigVMCore/RigVMStructUpgradeInfo.h"
#include "RigVMCore/RigVMTemplate.h"
#include "RigVMDeveloperTypeUtils.h"
#include "RigVMModel/Nodes/RigVMAggregateNode.h"
#include "RigVMModel/Nodes/RigVMCollapseNode.h"
#include "RigVMModel/Nodes/RigVMCommentNode.h"
#include "RigVMModel/Nodes/RigVMFunctionReferenceNode.h"
#include "RigVMModel/Nodes/RigVMLibraryNode.h"
#include "RigVMModel/Nodes/RigVMTemplateNode.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/RigVMClient.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMFunctionLibrary.h"
#include "RigVMCore/RigVMGraphFunctionDefinition.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMNode.h"
#include "RigVMModel/RigVMPin.h"
#include "RigVMModel/RigVMSchema.h"
#include "RigVMTypeUtils.h"
#include "UObject/StructOnScope.h"
#include "Units/RigUnit.h"

namespace UE::AssetDocument::Graph::Rig::ControlRig
{
FGraphEditState::FGraphEditState(UControlRigBlueprint& InBlueprint)
	: Blueprint(InBlueprint)
{
}

FGraphEditState::~FGraphEditState() = default;

bool SetFunctionVariableMapping(URigVMController& Controller, URigVMFunctionReferenceNode& Reference, FName Inner, FName Outer,
	bool bSetupUndoRedo)
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	// Newer function nodes show each remapped variable as a pin bound to the member variable.
	const URigVMPin* Pin = Reference.FindRootPinByName(Inner);
	if (Pin == nullptr)
	{
		return false;
	}
	return Outer.IsNone() ? Controller.UnbindPinFromVariable(Pin->GetPinPath(), bSetupUndoRedo)
		: Controller.BindPinToVariable(Pin->GetPinPath(), Outer.ToString(), bSetupUndoRedo);
#else
	return Controller.SetRemappedVariable(&Reference, Inner, Outer, bSetupUndoRedo);
#endif
}

namespace
{
const TCHAR* const DefaultGraphName = TEXT("Rig");
const TCHAR* const GraphModelPrefix = TEXT("RigVMModel");

FRigVMClient& ClientOf(UControlRigBlueprint& Blueprint)
{
	return *static_cast<URigVMBlueprint&>(Blueprint).GetRigVMClient();
}

// The document names of the rig's event graphs, for not-found messages.
FString GraphCandidates(UControlRigBlueprint& Blueprint, const FString& Wanted)
{
	FRigVMClient& Client = ClientOf(Blueprint);
	const URigVMGraph* Default = Client.GetDefaultModel();
	TArray<FString> Names;
	for (URigVMGraph* Model : Client.GetModels())
	{
		if (Model != nullptr)
		{
			Names.Add(GraphName(*Model, Default));
		}
	}
	return UE::AssetDocument::DescribeNameCandidates(Wanted, Names, TEXT("Graphs"));
}

URigVMGraph* FindModel(UControlRigBlueprint& Blueprint, const FString& Name)
{
	FRigVMClient& Client = ClientOf(Blueprint);
	const URigVMGraph* Default = Client.GetDefaultModel();
	for (URigVMGraph* Model : Client.GetModels())
	{
		if (Model != nullptr && GraphName(*Model, Default) == Name)
		{
			return Model;
		}
	}
	return nullptr;
}

// The graph a scope names: ["Graphs", <model>] or ["Functions", <function>], then ["Nodes", <collapse node>,
// "Collapse"]* (an aggregate node's graph is derived from the node, not a scope). Null with OutError otherwise.
URigVMGraph* ResolveScope(UControlRigBlueprint& Blueprint, const TArray<FString>& Scope, FString& OutError)
{
	URigVMGraph* Graph = nullptr;
	if (Scope.Num() >= 2 && Scope[0] == TEXT("Graphs"))
	{
		Graph = FindModel(Blueprint, Scope[1]);
		if (Graph == nullptr)
		{
			OutError = FString::Printf(TEXT("The rig has no graph '%s'.%s"), *Scope[1], *GraphCandidates(Blueprint, Scope[1]));
			return nullptr;
		}
	}
	else if (Scope.Num() >= 2 && Scope[0] == TEXT("Functions"))
	{
		const URigVMFunctionLibrary* Library = static_cast<URigVMBlueprint&>(Blueprint).GetLocalFunctionLibrary();
		URigVMLibraryNode* Function = Library != nullptr ? Library->FindFunction(FName(*Scope[1])) : nullptr;
		Graph = Function != nullptr ? Function->GetContainedGraph() : nullptr;
		if (Graph == nullptr)
		{
			OutError = FString::Printf(TEXT("The rig has no function '%s'."), *Scope[1]);
			return nullptr;
		}
	}
	else
	{
		OutError = TEXT("A graph scope starts with [\"Graphs\", <graph>] or [\"Functions\", <function>].");
		return nullptr;
	}
	for (int32 Index = 2; Index < Scope.Num(); Index += 3)
	{
		if (Index + 2 >= Scope.Num() || Scope[Index] != TEXT("Nodes") || Scope[Index + 2] != TEXT("Collapse"))
		{
			OutError = FString::Printf(TEXT("'%s' is not a graph scope (nested graphs are \"Nodes\", <collapse node>, \"Collapse\")."),
				*FString::Join(Scope, TEXT("/")));
			return nullptr;
		}
		const URigVMCollapseNode* Collapse = Cast<URigVMCollapseNode>(Graph->FindNodeByName(FName(*Scope[Index + 1])));
		if (Collapse == nullptr || Collapse->IsA<URigVMAggregateNode>() || Collapse->GetContainedGraph() == nullptr)
		{
			OutError = FString::Printf(TEXT("%s has no collapse node '%s'."), *FString::Join(TArrayView<const FString>(Scope.GetData(), Index), TEXT("/")),
				*Scope[Index + 1]);
			return nullptr;
		}
		Graph = Collapse->GetContainedGraph();
	}
	return Graph;
}

FString Joined(const TArray<FString>& Errors)
{
	return FString::Join(Errors, TEXT(" | "));
}

// " Close: ..." / " Nodes: ..." for a node name the graph does not have.
FString NodeCandidates(const URigVMGraph& Graph, const FString& Wanted)
{
	TArray<FString> Names;
	for (const URigVMNode* Node : Graph.GetNodes())
	{
		if (Node != nullptr && !Node->IsInjected())
		{
			Names.Add(Node->GetName());
		}
	}
	return UE::AssetDocument::DescribeNameCandidates(Wanted, Names, TEXT("Nodes"));
}

void CollectPinPaths(const URigVMPin& Pin, TArray<FString>& Out)
{
	Out.Add(Pin.GetPinPath());
	for (const URigVMPin* Sub : Pin.GetSubPins())
	{
		if (Sub != nullptr)
		{
			CollectPinPaths(*Sub, Out);
		}
	}
}

// For a pin path the graph does not have: the close pin paths of its node and the node's pins, or the close node
// names when the node itself is missing.
FString PinCandidates(const URigVMGraph& Graph, const FString& Path)
{
	FString NodeName = Path;
	FString Rest;
	Path.Split(TEXT("."), &NodeName, &Rest);
	const URigVMNode* Node = Graph.FindNodeByName(FName(*NodeName));
	if (Node == nullptr || Node->IsInjected())
	{
		return FString::Printf(TEXT(" The graph has no node '%s'.%s"), *NodeName, *NodeCandidates(Graph, NodeName));
	}
	TArray<FString> Paths;
	TArray<FString> Top;
	for (const URigVMPin* Pin : Node->GetPins())
	{
		if (Pin != nullptr)
		{
			Top.Add(Pin->GetName());
			CollectPinPaths(*Pin, Paths);
		}
	}
	const TArray<FString> Close = UE::AssetDocument::SuggestCloseNames(Path, Paths);
	FString Text = Close.IsEmpty() ? FString() : FString::Printf(TEXT(" Close: %s."), *FString::Join(Close, TEXT(", ")));
	return Text + FString::Printf(TEXT(" Pins of '%s': %s."), *NodeName, Top.IsEmpty() ? TEXT("none") : *FString::Join(Top, TEXT(", ")));
}

URigVMNode* RequireNode(FReader& Op, const FGraphTarget& Target, const FString& Name, const TCHAR* Field = TEXT("name"))
{
	URigVMNode* Node = Target.Graph->FindNodeByName(FName(*Name));
	if (Node == nullptr || Node->IsInjected())
	{
		Op.Fail(TEXT("ControlRig.NodeNotFound"), FString::Printf(TEXT("Graph '%s' has no node '%s'.%s"), *Target.GraphName, *Name,
			*NodeCandidates(*Target.Graph, Name)), Field);
		return nullptr;
	}
	return Node;
}

URigVMPin* RequirePin(FReader& Op, const FGraphTarget& Target, const FString& Path, const TCHAR* Field)
{
	URigVMPin* Pin = Target.Graph->FindPin(Path);
	if (Pin == nullptr)
	{
		Op.Fail(TEXT("ControlRig.PinNotFound"), FString::Printf(TEXT("Graph '%s' has no pin '%s' (pins are <node>.<pin>[.<sub pin>]).%s"),
			*Target.GraphName, *Path, *PinCandidates(*Target.Graph, Path)), Field);
	}
	return Pin;
}

bool ReadPair(FReader& Op, const TCHAR* Field, FVector2D& Out)
{
	const FValue* Value = Op.Find(Field);
	if (Value == nullptr || Value->Kind != EAssetDocumentRuntimeValueKind::Array || Value->Elements.Num() != 2)
	{
		return Op.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("%s is [x, y]."), Field), Field);
	}
	double Parts[2] = {0.0, 0.0};
	for (int32 Index = 0; Index < 2; ++Index)
	{
		const FValue& Item = Value->Elements[Index];
		if (Item.Kind == EAssetDocumentRuntimeValueKind::Number)
		{
			Parts[Index] = Item.NumberValue;
		}
		else if (Item.Kind == EAssetDocumentRuntimeValueKind::Integer)
		{
			Parts[Index] = static_cast<double>(Item.IntegerValue);
		}
		else
		{
			return Op.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("%s is [x, y] numbers."), Field), Field);
		}
	}
	Out = FVector2D(Parts[0], Parts[1]);
	return true;
}

bool ReadColor(FReader& Op, const TCHAR* Field, FLinearColor& Out)
{
	const FValue* Value = Op.Find(Field);
	FString Error = TEXT("not an object");
	Out = FLinearColor::White;
	if (Value == nullptr || Value->Kind != EAssetDocumentRuntimeValueKind::Object
		|| !DecodeStructFields(*TBaseStructure<FLinearColor>::Get(), &Out, *Value, [](const FProperty&) { return false; }, Error))
	{
		return Op.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("%s is {R, G, B, A}: %s"), Field, *Error), Field);
	}
	return true;
}

// ---- units ------------------------------------------------------------------------------------------------------

// Unit structs by short name (rebuilt when a name is not found: a plugin may have loaded more).
UScriptStruct* FindUnit(FReader& Op, const FString& Name, const TCHAR* Field)
{
	if (Name.StartsWith(TEXT("/")))
	{
		UScriptStruct* Struct = RigVMTypeUtils::FindObjectFromCPPTypeObjectPath<UScriptStruct>(Name);
		if (Struct == nullptr || !Struct->IsChildOf(FRigVMStruct::StaticStruct()))
		{
			Op.Fail(TEXT("ControlRig.UnknownUnit"), FString::Printf(TEXT("'%s' is not a RigVM unit struct (list_types kind rigvm_node)."), *Name), Field);
			return nullptr;
		}
		return Struct;
	}
	static TMap<FString, TArray<UScriptStruct*>> ByName;
	TArray<UScriptStruct*>* Found = ByName.Find(Name);
	if (Found == nullptr)
	{
		ByName.Reset();
		for (UScriptStruct* Struct : GetUnitStructs())
		{
			ByName.FindOrAdd(Struct->GetName()).Add(Struct);
		}
		Found = ByName.Find(Name);
	}
	if (Found == nullptr || Found->IsEmpty())
	{
		TArray<FString> Names;
		ByName.GetKeys(Names);
		const TArray<FString> Close = UE::AssetDocument::SuggestCloseNames(Name, Names);
		Op.Fail(TEXT("ControlRig.UnknownUnit"), FString::Printf(TEXT("No RigVM unit is named '%s' (list_types kind rigvm_node lists them).%s"), *Name,
			Close.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" Close: %s."), *FString::Join(Close, TEXT(", ")))), Field);
		return nullptr;
	}
	if (Found->Num() > 1)
	{
		Op.Fail(TEXT("ControlRig.AmbiguousUnit"), FString::Printf(TEXT("Several units are named '%s'; write the path: %s."), *Name,
			*FString::JoinBy(*Found, TEXT(", "), [](const UScriptStruct* Struct) { return Struct->GetPathName(); })), Field);
		return nullptr;
	}
	return (*Found)[0];
}

// The struct the editor's upgrade of a deprecated unit makes (FRigVMStruct::GetUpgradeInfo), "" when none.
FString Replacement(UScriptStruct& Struct)
{
	FStructOnScope Scope(&Struct);
	const FRigVMStructUpgradeInfo Info = reinterpret_cast<const FRigVMStruct*>(Scope.GetStructMemory())->GetUpgradeInfo();
	return Info.GetNewStruct() != nullptr ? UnitName(*Info.GetNewStruct()) : FString();
}

// Top-level pin values written into the unit's defaults (what AddUnitNodeWithDefaults takes, as a paste or the
// Python export does). A value that does not decode is left to the later pin.default op, which reports it.
// Only user events are made this way: their name is a transient field that no pin write can set before the node
// exists, and two new events would share the default name. Other units are spawned as the palette does (pins keep the
// palette's value texts) and take their values from pin.default ops, as the pin widgets set them: the compiler shares
// one literal between pins of equal value text, so the text forms decide the VM's literal memory.
// FRigVMFunction_UserDefinedEvent (not exported by RigVM) or a struct derived from it.
bool IsUserEvent(const UScriptStruct& Struct)
{
	for (const UStruct* Current = &Struct; Current != nullptr; Current = Current->GetSuperStruct())
	{
		if (Current->GetPathName() == TEXT("/Script/RigVM.RigVMFunction_UserDefinedEvent"))
		{
			return true;
		}
	}
	return false;
}

TUniquePtr<FStructOnScope> DecodeUnitDefaults(UScriptStruct& Struct, const FValue* Defaults)
{
	if (Defaults == nullptr || Defaults->Kind != EAssetDocumentRuntimeValueKind::Object || Defaults->ObjectKeys.IsEmpty())
	{
		return nullptr;
	}
	TUniquePtr<FStructOnScope> Scope = MakeUnique<FStructOnScope>(&Struct);
	bool bAny = false;
	for (int32 Index = 0; Index < Defaults->ObjectKeys.Num(); ++Index)
	{
		const FProperty* Property = Struct.FindPropertyByName(FName(*Defaults->ObjectKeys[Index]));
		if (Property == nullptr || Property->ArrayDim != 1)
		{
			continue;
		}
		void* Scratch = FMemory::Malloc(Property->GetSize(), Property->GetMinAlignment());
		Property->InitializeValue(Scratch);
		Property->CopyCompleteValue(Scratch, Property->ContainerPtrToValuePtr<void>(Scope->GetStructMemory()));
		FString Error;
		if (DecodeProperty(*Property, Scratch, Defaults->Elements[Index], Error))
		{
			Property->CopyCompleteValue(Property->ContainerPtrToValuePtr<void>(Scope->GetStructMemory()), Scratch);
			bAny = true;
		}
		Property->DestroyValue(Scratch);
		FMemory::Free(Scratch);
	}
	if (!bAny)
	{
		Scope.Reset();
	}
	return Scope;
}

// ---- function references and collapse nodes ------------------------------------------------------------------

// The public functions of the Control Rig at ObjectPath ("<package>.<asset>"), for messages.
FString PublicFunctionsOf(const FString& ObjectPath)
{
	const UControlRigBlueprint* Host = LoadObject<UControlRigBlueprint>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (Host == nullptr)
	{
		return FString::Printf(TEXT("%s is not a Control Rig"), *ObjectPath);
	}
	TArray<FString> Names;
	if (const URigVMFunctionLibrary* Library = static_cast<const URigVMBlueprint*>(Host)->GetLocalFunctionLibrary())
	{
		for (const URigVMLibraryNode* Function : Library->GetFunctions())
		{
			if (Function != nullptr && Library->IsFunctionPublic(Function->GetFName()))
			{
				Names.Add(Function->GetName());
			}
		}
	}
	Names.Sort();
	return Names.IsEmpty() ? FString::Printf(TEXT("%s has no public functions"), *ObjectPath)
		: FString::Printf(TEXT("the public functions of %s: %s"), *ObjectPath, *FString::Join(Names, TEXT(", ")));
}

// What the palette's function entry spawns (FRigVMFunctionRefNodeSpawner): a local function by name, another rig's
// public function as "<package>.<asset>:<function>" (its header from the host, AddFunctionReferenceNodeFromDescription).
// A private function of another rig, which the palette does not offer and the controller does not place ("Function is
// not available for placement in another graph host"), is refused: only a rig duplicated from its host keeps calling one
// (MoverExamples' CR_Mannequin_Body calls the template rig's).
URigVMNode* AddFunctionReference(FGraphEditState& State, FReader& Entry, URigVMController& Controller, const FString& Function,
	const FVector2D& Position, const FString& Name)
{
	FString Host;
	FString FunctionName;
	if (!Function.Split(TEXT(":"), &Host, &FunctionName, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
	{
		const URigVMFunctionLibrary* Library = static_cast<URigVMBlueprint&>(State.Blueprint).GetLocalFunctionLibrary();
		URigVMLibraryNode* Definition = Library != nullptr ? Library->FindFunction(FName(*Function)) : nullptr;
		if (Definition == nullptr)
		{
			Entry.Fail(TEXT("ControlRig.FunctionNotFound"), FString::Printf(TEXT("The rig has no function '%s' (list it in Functions; another "
				"rig's public function is \"<package>.<asset>:<function>\")."), *Function), TEXT("Function"));
			return nullptr;
		}
		return Controller.AddFunctionReferenceNode(Definition, Position, Name, false, false);
	}
	if (Host == State.Blueprint.GetPathName())
	{
		Entry.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("'%s' is this rig: name its own functions without the asset path."),
			*Function), TEXT("Function"));
		return nullptr;
	}
	// The function host of a Control Rig is its generated class; the asset path resolves too.
	for (const FString& Candidate : {Host + TEXT("_C"), Host})
	{
		bool bPublic = false;
		FString Error;
		const FRigVMGraphFunctionHeader Header = FRigVMGraphFunctionHeader::FindGraphFunctionHeader(FSoftObjectPath(Candidate), FName(*FunctionName),
			&bPublic, &Error);
		if (!Header.IsValid())
		{
			continue;
		}
		if (!bPublic)
		{
			Entry.Fail(TEXT("ControlRig.EditRefused"), FString::Printf(TEXT("Function '%s' of %s is private: the editor places another rig's "
				"public functions only (%s)."), *FunctionName, *Host, *PublicFunctionsOf(Host)), TEXT("Function"));
			return nullptr;
		}
		return Controller.AddFunctionReferenceNodeFromDescription(Header, Position, Name, false, false);
	}
	Entry.Fail(TEXT("ControlRig.FunctionNotFound"), FString::Printf(TEXT("'%s' names no function (%s)."), *Function, *PublicFunctionsOf(Host)),
		TEXT("Function"));
	return nullptr;
}

// A new, empty collapse node: what the editor's Collapse Nodes makes, without nodes in it (its interface pins and
// body are the later ops'). CollapseNodes needs a node to collapse: a transient comment at the node's position, removed
// from the new graph again.
URigVMNode* AddEmptyCollapse(FGraphEditState& State, FReader& Entry, const FGraphTarget& Target, const FVector2D& Position, const FString& Name)
{
	URigVMController& Controller = *Target.Controller;
	const URigVMCommentNode* Seed = Controller.AddCommentNode(FString(), Position, FVector2D(400.0, 300.0), FLinearColor::Black, TEXT("CollapseSeed"),
		false, false);
	if (Seed == nullptr)
	{
		Entry.Fail(TEXT("ControlRig.EditFailed"), TEXT("The collapse node could not be made."), TEXT("Collapse"));
		return nullptr;
	}
	const FName SeedName = Seed->GetFName();
	URigVMCollapseNode* Collapse = Controller.CollapseNodes(TArray<FName>{SeedName}, Name, false, false, false);
	if (Collapse == nullptr || Collapse->GetContainedGraph() == nullptr)
	{
		Controller.RemoveNodeByName(SeedName, false, false);
		Entry.Fail(TEXT("ControlRig.EditFailed"), TEXT("The collapse node could not be made."), TEXT("Collapse"));
		return nullptr;
	}
	URigVMController* Inner = static_cast<URigVMBlueprint&>(State.Blueprint).GetOrCreateController(Collapse->GetContainedGraph());
	FName Moved = NAME_None;
	for (const URigVMNode* Node : Collapse->GetContainedGraph()->GetNodes())
	{
		if (Node != nullptr && Node->IsA<URigVMCommentNode>())
		{
			Moved = Node->GetFName();
		}
	}
	if (Inner == nullptr || Moved.IsNone() || !Inner->RemoveNodeByName(Moved, false, false))
	{
		Entry.Fail(TEXT("ControlRig.EditFailed"), TEXT("The new collapse node's graph could not be emptied."), TEXT("Collapse"));
		return nullptr;
	}
	return Collapse;
}

// ---- graphs ---------------------------------------------------------------------------------------------------

bool OpGraphAdd(FGraphEditState& State, FReader& Op)
{
	FString Name;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name")}) || !Op.String(TEXT("name"), Name))
	{
		return false;
	}
	if (Name.IsEmpty() || Name == DefaultGraphName || FindModel(State.Blueprint, Name) != nullptr)
	{
		return Op.Fail(TEXT("ControlRig.NameTaken"), FString::Printf(TEXT("The rig already has a graph '%s'."), *Name), TEXT("name"));
	}
	URigVMGraph* Model = static_cast<URigVMBlueprint&>(State.Blueprint).AddModel(Name, false, false);
	if (!CheckReported(State, Op, FString::Printf(TEXT("Adding graph '%s'"), *Name)))
	{
		return false;
	}
	if (Model == nullptr)
	{
		return Op.Fail(TEXT("ControlRig.EditFailed"), FString::Printf(TEXT("Graph '%s' could not be added."), *Name), TEXT("name"));
	}
	State.bChanged = true;
	const FString Added = GraphName(*Model, ClientOf(State.Blueprint).GetDefaultModel());
	if (Added != Name)
	{
		return Op.Fail(TEXT("ControlRig.InvalidGraphName"), FString::Printf(TEXT("The new graph was named '%s', not '%s' (graph names are "
			"unique object names: letters, digits, spaces, '_')."), *Added, *Name), TEXT("name"));
	}
	return true;
}

bool OpGraphRemove(FGraphEditState& State, FReader& Op)
{
	FString Name;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name")}) || !Op.String(TEXT("name"), Name))
	{
		return false;
	}
	URigVMGraph* Model = FindModel(State.Blueprint, Name);
	if (Model == nullptr)
	{
		return Op.Fail(TEXT("ControlRig.GraphNotFound"), FString::Printf(TEXT("The rig has no graph '%s'.%s"), *Name,
			*GraphCandidates(State.Blueprint, Name)), TEXT("name"));
	}
	if (Name == DefaultGraphName)
	{
		return Op.Fail(TEXT("ControlRig.InvalidBody"), TEXT("The Rig graph is the default graph and cannot be removed."), TEXT("name"));
	}
	const bool bRemoved = static_cast<URigVMBlueprint&>(State.Blueprint).RemoveModel(Model->GetNodePath(), false, false);
	if (!CheckReported(State, Op, FString::Printf(TEXT("Removing graph '%s'"), *Name)))
	{
		return false;
	}
	if (!bRemoved)
	{
		return Op.Fail(TEXT("ControlRig.EditFailed"), FString::Printf(TEXT("Graph '%s' could not be removed."), *Name), TEXT("name"));
	}
	State.bChanged = true;
	return true;
}

bool OpGraphRename(FGraphEditState& State, FReader& Op)
{
	FString Name;
	FString To;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("to")}) || !Op.String(TEXT("name"), Name) || !Op.String(TEXT("to"), To))
	{
		return false;
	}
	URigVMGraph* Model = FindModel(State.Blueprint, Name);
	if (Model == nullptr)
	{
		return Op.Fail(TEXT("ControlRig.GraphNotFound"), FString::Printf(TEXT("The rig has no graph '%s'.%s"), *Name,
			*GraphCandidates(State.Blueprint, Name)), TEXT("name"));
	}
	if (Name == DefaultGraphName || To == DefaultGraphName)
	{
		return Op.Fail(TEXT("ControlRig.InvalidBody"), TEXT("The Rig graph is the default graph: it keeps its name."), TEXT("to"));
	}
	if (To.IsEmpty() || FindModel(State.Blueprint, To) != nullptr)
	{
		return Op.Fail(TEXT("ControlRig.NameTaken"), FString::Printf(TEXT("The rig already has a graph '%s'."), *To), TEXT("to"));
	}
	// What My Blueprint's rename does (URigVMEdGraphSchema::TryRenameGraph -> RenameGraph), on the client without its
	// own undo entry.
	const FName Wanted(*FString::Printf(TEXT("%s %s"), GraphModelPrefix, *To));
	const FName Result = ClientOf(State.Blueprint).RenameModel(Model->GetNodePath(), Wanted, false);
	if (!CheckReported(State, Op, FString::Printf(TEXT("Renaming graph '%s'"), *Name)))
	{
		return false;
	}
	State.bChanged = true;
	if (Result != Wanted)
	{
		return Op.Fail(TEXT("ControlRig.InvalidGraphName"), FString::Printf(TEXT("Graph '%s' could not be renamed to '%s' (it was named '%s')."),
			*Name, *To, *Result.ToString()), TEXT("to"));
	}
	return true;
}

// ---- nodes ----------------------------------------------------------------------------------------------------

bool OpNodeAdd(FGraphEditState& State, FReader& Op, int32 OpIndex)
{
	FGraphTarget Target;
	FString Name;
	const FValue* EntryValue = nullptr;
	FString Deprecated;
	if (!Op.OnlyFields({TEXT("op"), TEXT("graph"), TEXT("name"), TEXT("entry"), TEXT("deprecated")}) || !ReadGraphField(State, Op, Target)
		|| !Op.String(TEXT("name"), Name) || !Op.Object(TEXT("entry"), EntryValue) || !Op.String(TEXT("deprecated"), Deprecated, false))
	{
		return false;
	}
	FReader Entry = Op.Child(TEXT("entry"), *EntryValue);
	if (Name.IsEmpty())
	{
		return Op.Fail(TEXT("ControlRig.InvalidNodeName"), TEXT("A node name is not empty."), TEXT("name"));
	}
	if (Target.Graph->FindNodeByName(FName(*Name)) != nullptr)
	{
		return Op.Fail(TEXT("ControlRig.NameTaken"), FString::Printf(TEXT("Graph '%s' already has a node '%s'."), *Target.GraphName, *Name), TEXT("name"));
	}
	if (const URigVMSchema* Schema = Target.Controller->GetSchema())
	{
		const FString Valid = Schema->GetValidNodeName(Target.Graph, Name);
		if (Valid != Name)
		{
			return Op.Fail(TEXT("ControlRig.InvalidNodeName"), FString::Printf(TEXT("'%s' is not a node name the graph keeps (it would be "
				"named '%s'): use letters, digits, spaces and '_'."), *Name, *Valid), TEXT("name"));
		}
	}
	FVector2D Position = FVector2D::ZeroVector;
	if (Entry.Has(TEXT("Position")) && !ReadPair(Entry, TEXT("Position"), Position))
	{
		return false;
	}
	const FReportingScope Reporting(*Target.Controller);
	URigVMController& Controller = *Target.Controller;
	URigVMNode* Node = nullptr;
	FString Text;
	TArray<URigVMNode*> Before = Target.Graph->GetNodes();
	if (Entry.Has(TEXT("Unit")))
	{
		if (!Entry.String(TEXT("Unit"), Text))
		{
			return false;
		}
		UScriptStruct* Struct = FindUnit(Entry, Text, TEXT("Unit"));
		if (Struct == nullptr)
		{
			return false;
		}
		FString Method = FRigUnit::GetMethodName().ToString();
		if (!Entry.String(TEXT("Method"), Method, false))
		{
			return false;
		}
		if (Struct->HasMetaData(FRigVMStruct::DeprecatedMetaName))
		{
			const FString Next = Replacement(*Struct);
			const FString Message = FString::Printf(TEXT("%s is deprecated%s"), *UnitName(*Struct),
				Next.IsEmpty() ? TEXT(" (the node palette no longer offers it)") : *FString::Printf(TEXT("; use %s"), *Next));
			if (Deprecated != TEXT("warn"))
			{
				return Entry.Fail(TEXT("ControlRig.DeprecatedUnit"), Message + TEXT(". New nodes of deprecated units are not added, as in the "
					"editor's palette."), TEXT("Unit"));
			}
			FObjectBuilder Note;
			Note.AddInt(TEXT("op"), OpIndex);
			Note.AddString(TEXT("field"), TEXT("entry/Unit"));
			Note.AddString(TEXT("code"), TEXT("ControlRig.DeprecatedUnit"));
			Note.AddString(TEXT("message"), Message + TEXT("; the node is kept as the document lists it."));
			State.Notes.Add(Note.Build());
		}
		// The schema decides what the palette offers in this rig (connector units such as Get Candidates only in rig
		// modules); AddUnitNode refuses the rest without a message.
		const FRigVMFunction* UnitFunction = FRigVMRegistry::Get().FindFunction(Struct, *Method);
		const URigVMSchema* Schema = Controller.GetSchema();
		if (UnitFunction != nullptr && Schema != nullptr && !Schema->SupportsUnitFunction(&Controller, UnitFunction))
		{
			return Entry.Fail(TEXT("ControlRig.EditRefused"), FString::Printf(TEXT("The editor does not offer %s in this rig (connector "
				"units such as Get Candidates are for rig modules only)."), *UnitName(*Struct)), TEXT("Unit"));
		}
		const TUniquePtr<FStructOnScope> Defaults = IsUserEvent(*Struct) ? DecodeUnitDefaults(*Struct, Entry.Find(TEXT("Defaults"))) : nullptr;
		if (Defaults.IsValid())
		{
			Node = Controller.AddUnitNodeWithDefaults(Struct, FRigStructScope(*Defaults), FName(*Method), Position, Name, false, false);
		}
		else
		{
			Node = Controller.AddUnitNode(Struct, FName(*Method), Position, Name, false, false);
		}
		// A unit of a template (Exponential: float and double units) may resolve to another permutation of it; the node
		// takes the unit the document names, as a node loaded with it holds it.
		const URigVMUnitNode* UnitNode = Cast<URigVMUnitNode>(Node);
		const FRigVMTemplate* UnitTemplate = UnitFunction != nullptr ? UnitFunction->GetTemplate() : nullptr;
		if (UnitNode != nullptr && UnitTemplate != nullptr && UnitNode->GetScriptStruct() != Struct)
		{
			const int32 Permutation = UnitTemplate->FindPermutation(UnitFunction);
			if (Permutation != INDEX_NONE)
			{
				Controller.FullyResolveTemplateNode(Cast<URigVMTemplateNode>(Node), Permutation, false);
			}
		}
	}
	else if (Entry.Has(TEXT("Template")))
	{
		if (!Entry.String(TEXT("Template"), Text))
		{
			return false;
		}
		FName Notation(*Text);
		if (const FRigVMDispatchFactory* Factory = FRigVMRegistry::Get().FindDispatchFactory(FName(*Text)))
		{
			if (const FRigVMTemplate* Template = Factory->GetTemplate())
			{
				Notation = Template->GetNotation();
			}
		}
		if (FRigVMRegistry::Get().FindTemplate(Notation) == nullptr)
		{
			return Entry.Fail(TEXT("ControlRig.UnknownTemplate"), FString::Printf(TEXT("'%s' is not a template notation or dispatch factory "
				"(list_types kind rigvm_node)."), *Text), TEXT("Template"));
		}
		Node = Controller.AddTemplateNode(Notation, Position, Name, false, false);
	}
	else if (Entry.Has(TEXT("Variable")))
	{
		bool bSetter = false;
		if (!Entry.String(TEXT("Variable"), Text) || !Entry.Bool(TEXT("Setter"), bSetter))
		{
			return false;
		}
		// A local variable or input argument of the function the graph is in (My Blueprint's Local Variables / the
		// function's inputs), else a member variable.
		FString CPPType;
		UObject* TypeObject = nullptr;
		bool bLocal = false;
		for (const URigVMGraph* Scope = Target.Graph; Scope != nullptr && !bLocal && !Scope->IsA<URigVMFunctionLibrary>(); Scope = Scope->GetParentGraph())
		{
			for (const FRigVMGraphVariableDescription& Local : Scope->GetLocalVariables(true))
			{
				if (Local.Name == FName(*Text))
				{
					CPPType = Local.CPPType;
					TypeObject = Local.CPPTypeObject;
					bLocal = true;
					break;
				}
			}
		}
		if (!bLocal)
		{
			const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(&State.Blueprint, FName(*Text));
			if (Index == INDEX_NONE)
			{
				return Entry.Fail(TEXT("ControlRig.VariableNotFound"), FString::Printf(TEXT("The rig has no member variable '%s' (list it in "
					"Variables)%s."), *Text, Target.Graph->IsRootGraph() ? TEXT("") : TEXT(", and the function has no local variable or input of "
					"that name")), TEXT("Variable"));
			}
			const FRigVMExternalVariable Variable = RigVMTypeUtils::ExternalVariableFromBPVariableDescription(State.Blueprint.NewVariables[Index]);
			if (!RigVMTypeUtils::CPPTypeFromExternalVariable(Variable, CPPType, &TypeObject))
			{
				return Entry.Fail(TEXT("ControlRig.UnsupportedVariableType"), FString::Printf(TEXT("Member variable '%s' has no RigVM type."), *Text),
					TEXT("Variable"));
			}
		}
		Node = Controller.AddVariableNode(FName(*Text), CPPType, TypeObject, !bSetter, FString(), Position, Name, false, false);
	}
	else if (Entry.Has(TEXT("Reroute")))
	{
		FString CPPType;
		UObject* TypeObject = nullptr;
		if (!Entry.String(TEXT("Reroute"), Text))
		{
			return false;
		}
		if (!ParseTypeName(Text, CPPType, TypeObject))
		{
			return Entry.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("'%s' is not a RigVM type (list_types kind rigvm_type)."), *Text),
				TEXT("Reroute"));
		}
		Node = Controller.AddFreeRerouteNode(CPPType, TypeObject != nullptr ? FName(*TypeObject->GetPathName()) : NAME_None, false, NAME_None,
			FString(), Position, Name, false);
	}
	else if (Entry.Has(TEXT("Enum")))
	{
		if (!Entry.String(TEXT("Enum"), Text))
		{
			return false;
		}
		Node = Controller.AddEnumNode(FName(*Text), Position, Name, false, false);
	}
	else if (Entry.Has(TEXT("InvokeEvent")))
	{
		if (!Entry.String(TEXT("InvokeEvent"), Text))
		{
			return false;
		}
		Node = Controller.AddInvokeEntryNode(FName(*Text), Position, Name, false, false);
	}
	else if (Entry.Has(TEXT("Comment")))
	{
		FVector2D Size(400.0, 300.0);
		FLinearColor Color = FLinearColor::Black;
		if (!Entry.String(TEXT("Comment"), Text) || (Entry.Has(TEXT("Size")) && !ReadPair(Entry, TEXT("Size"), Size))
			|| (Entry.Has(TEXT("Color")) && !ReadColor(Entry, TEXT("Color"), Color)))
		{
			return false;
		}
		Node = Controller.AddCommentNode(Text, Position, Size, Color, Name, false, false);
	}
	else if (Entry.Has(TEXT("Function")))
	{
		if (!Entry.String(TEXT("Function"), Text))
		{
			return false;
		}
		Node = AddFunctionReference(State, Entry, Controller, Text, Position, Name);
		if (Node == nullptr)
		{
			return false;
		}
	}
	else if (Entry.Has(TEXT("Collapse")))
	{
		Node = AddEmptyCollapse(State, Entry, Target, Position, Name);
		if (Node == nullptr)
		{
			return false;
		}
	}
	else
	{
		return Entry.Fail(TEXT("ControlRig.InvalidBody"), TEXT("A node has one kind: Unit, Template, Variable, Reroute, Enum, InvokeEvent, "
			"Comment, Function or Collapse."));
	}
	if (!CheckReported(State, Op, FString::Printf(TEXT("Adding node '%s'"), *Name), TEXT("ControlRig.EditRefused"), TEXT("entry")))
	{
		return false;
	}
	if (Node == nullptr)
	{
		return Op.Fail(TEXT("ControlRig.EditFailed"), FString::Printf(TEXT("Node '%s' could not be added."), *Name), TEXT("entry"));
	}
	if (Before.Contains(Node))
	{
		// AddUnitNode returns the existing event node of an event that can exist only once.
		return Op.Fail(TEXT("ControlRig.EventAlreadyExists"), FString::Printf(TEXT("The rig already has this event as node '%s'; an event "
			"node exists once."), *Node->GetName()), TEXT("entry"));
	}
	State.bChanged = true;
	if (Node->GetName() != Name)
	{
		return Op.Fail(TEXT("ControlRig.NameTaken"), FString::Printf(TEXT("The new node was named '%s', not '%s'."), *Node->GetName(), *Name),
			TEXT("name"));
	}
	// Pin expansion is the document's Expanded (set with the values): a new node starts with none, not with the pins
	// its type expands by default, so that a document without Expanded makes the node it describes.
	TArray<FString> Expanded;
	TArray<URigVMPin*> Pending = Node->GetPins();
	while (!Pending.IsEmpty())
	{
		const URigVMPin* Pin = Pending.Pop(EAllowShrinking::No);
		if (Pin->IsExpanded())
		{
			Expanded.Add(Pin->GetPinPath());
		}
		Pending.Append(Pin->GetSubPins());
	}
	for (const FString& Path : Expanded)
	{
		Controller.SetPinExpansion(Path, false, false, false);
	}
	return true;
}

bool OpNodeRemove(FGraphEditState& State, FReader& Op)
{
	FGraphTarget Target;
	FString Name;
	if (!Op.OnlyFields({TEXT("op"), TEXT("graph"), TEXT("name")}) || !ReadGraphField(State, Op, Target) || !Op.String(TEXT("name"), Name)
		|| RequireNode(Op, Target, Name) == nullptr)
	{
		return false;
	}
	const FReportingScope Reporting(*Target.Controller);
	const bool bRemoved = Target.Controller->RemoveNodeByName(FName(*Name), false, false);
	if (!CheckReported(State, Op, FString::Printf(TEXT("Removing node '%s'"), *Name)))
	{
		return false;
	}
	if (!bRemoved)
	{
		return Op.Fail(TEXT("ControlRig.EditFailed"), FString::Printf(TEXT("Node '%s' could not be removed."), *Name), TEXT("name"));
	}
	State.bChanged = true;
	return true;
}

bool OpNodeRename(FGraphEditState& State, FReader& Op)
{
	FGraphTarget Target;
	FString Name;
	FString To;
	if (!Op.OnlyFields({TEXT("op"), TEXT("graph"), TEXT("name"), TEXT("to")}) || !ReadGraphField(State, Op, Target) || !Op.String(TEXT("name"), Name)
		|| !Op.String(TEXT("to"), To))
	{
		return false;
	}
	URigVMNode* Node = RequireNode(Op, Target, Name);
	if (Node == nullptr)
	{
		return false;
	}
	if (Target.Graph->FindNodeByName(FName(*To)) != nullptr)
	{
		return Op.Fail(TEXT("ControlRig.NameTaken"), FString::Printf(TEXT("Graph '%s' already has a node '%s'."), *Target.GraphName, *To), TEXT("to"));
	}
	const FReportingScope Reporting(*Target.Controller);
	Target.Controller->RenameNode(Node, FName(*To), false, false);
	if (!CheckReported(State, Op, FString::Printf(TEXT("Renaming node '%s'"), *Name)))
	{
		return false;
	}
	State.bChanged = true;
	if (Target.Graph->FindNodeByName(FName(*To)) == nullptr)
	{
		return Op.Fail(TEXT("ControlRig.InvalidNodeName"), FString::Printf(TEXT("Node '%s' could not be renamed to '%s'."), *Name, *To), TEXT("to"));
	}
	return true;
}

bool OpNodeSet(FGraphEditState& State, FReader& Op)
{
	FGraphTarget Target;
	FString Name;
	if (!Op.OnlyFields({TEXT("op"), TEXT("graph"), TEXT("name"), TEXT("position"), TEXT("size"), TEXT("title"), TEXT("color"), TEXT("comment"),
		TEXT("font_size"), TEXT("bubble"), TEXT("color_bubble"), TEXT("expanded")}) || !ReadGraphField(State, Op, Target) || !Op.String(TEXT("name"), Name))
	{
		return false;
	}
	URigVMNode* Node = RequireNode(Op, Target, Name);
	if (Node == nullptr)
	{
		return false;
	}
	// No reset to default exists for these in the editor (the Details panel and the controller take a value), so a
	// null is refused rather than dropped.
	for (const TCHAR* Field : {TEXT("position"), TEXT("size"), TEXT("title"), TEXT("color")})
	{
		if (!Op.RefuseNull(Field, TEXT("ControlRig.InvalidValue"), TEXT("give the value, or omit the field to keep the node's current one.")))
		{
			return false;
		}
	}
	const FReportingScope Reporting(*Target.Controller);
	URigVMController& Controller = *Target.Controller;
	const FName NodeName(*Name);
	if (Op.Has(TEXT("position")))
	{
		FVector2D Position;
		if (!ReadPair(Op, TEXT("position"), Position))
		{
			return false;
		}
		Controller.SetNodePositionByName(NodeName, Position, false, false, false);
		if (!CheckReported(State, Op, TEXT("Moving the node"), TEXT("ControlRig.EditRefused"), TEXT("position")))
		{
			return false;
		}
	}
	if (Op.Has(TEXT("size")))
	{
		FVector2D Size;
		if (!ReadPair(Op, TEXT("size"), Size))
		{
			return false;
		}
		Controller.SetNodeSizeByName(NodeName, Size, false, false, false);
		if (!CheckReported(State, Op, TEXT("Resizing the node"), TEXT("ControlRig.EditRefused"), TEXT("size")))
		{
			return false;
		}
	}
	if (Op.Has(TEXT("title")))
	{
		FString Title;
		if (!Op.String(TEXT("title"), Title))
		{
			return false;
		}
		if (!Node->SupportsRenaming())
		{
			return Op.Fail(TEXT("ControlRig.InvalidValue"), TEXT("Title is set on renamable nodes only (Constant, Make Struct); other nodes show "
				"their unit's name."), TEXT("title"));
		}
		if (!Controller.SetNodeTitle(Node, Title, false, false, false) || !CheckReported(State, Op, TEXT("The title"), TEXT("ControlRig.EditRefused"), TEXT("title")))
		{
			return Op.Fail(TEXT("ControlRig.InvalidValue"), TEXT("The node's title could not be set."), TEXT("title"));
		}
	}
	if (Op.Has(TEXT("color")))
	{
		FLinearColor Color;
		if (!ReadColor(Op, TEXT("color"), Color))
		{
			return false;
		}
		// URigVMSchema::CanRecolorNode: comments and library nodes (collapse / function) only.
		if (!Node->IsA<URigVMCommentNode>() && !Node->IsA<URigVMLibraryNode>())
		{
			return Op.Fail(TEXT("ControlRig.InvalidValue"), TEXT("Color is set on comments, collapse and function nodes only."), TEXT("color"));
		}
		if (!Node->GetNodeColor().Equals(Color, 0.001f))
		{
			Controller.SetNodeColor(Node, Color, false, false, false);
			if (!CheckReported(State, Op, TEXT("The color"), TEXT("ControlRig.InvalidValue"), TEXT("color")))
			{
				return false;
			}
		}
	}
	if (Op.Has(TEXT("comment")) || Op.Has(TEXT("font_size")) || Op.Has(TEXT("bubble")) || Op.Has(TEXT("color_bubble")))
	{
		URigVMCommentNode* Comment = Cast<URigVMCommentNode>(Node);
		if (Comment == nullptr)
		{
			return Op.Fail(TEXT("ControlRig.InvalidValue"), TEXT("Comment text, FontSize and Bubble belong to comment nodes."), TEXT("comment"));
		}
		FString Text = Comment->GetCommentText();
		int64 FontSize = Comment->GetCommentFontSize();
		bool bBubble = Comment->GetCommentBubbleVisible();
		bool bColorBubble = Comment->GetCommentColorBubble();
		if (!Op.String(TEXT("comment"), Text, false) || !Op.Int(TEXT("font_size"), FontSize, false) || !Op.Bool(TEXT("bubble"), bBubble)
			|| !Op.Bool(TEXT("color_bubble"), bColorBubble))
		{
			return false;
		}
		Controller.SetCommentText(Node, Text, static_cast<int32>(FontSize), bBubble, bColorBubble, false, false);
		if (!CheckReported(State, Op, TEXT("The comment"), TEXT("ControlRig.EditRefused"), TEXT("comment")))
		{
			return false;
		}
	}
	if (Op.Has(TEXT("expanded")))
	{
		const FValue* Expanded = nullptr;
		if (!Op.Object(TEXT("expanded"), Expanded))
		{
			return false;
		}
		for (int32 Index = 0; Index < Expanded->ObjectKeys.Num(); ++Index)
		{
			const FString Path = Name + TEXT(".") + Expanded->ObjectKeys[Index];
			const FString Field = FString::Printf(TEXT("expanded/%s"), *Expanded->ObjectKeys[Index]);
			if (RequirePin(Op, Target, Path, *Field) == nullptr)
			{
				return false;
			}
			Controller.SetPinExpansion(Path, Expanded->Elements[Index].Kind == EAssetDocumentRuntimeValueKind::Boolean && Expanded->Elements[Index].BooleanValue,
				false, false);
			if (!CheckReported(State, Op, FString::Printf(TEXT("Expanding %s"), *Path), TEXT("ControlRig.EditRefused"), *Field))
			{
				return false;
			}
		}
	}
	State.bChanged = true;
	return true;
}

// The registry maps a wanted type to a compatible one when the template has no permutation of it yet (double to float
// on an If, float to double on Exponential): a node loaded from disk keeps the permutation it was saved with. When the
// pin-by-pin resolve lands elsewhere, the node is resolved to the permutation of all the wanted types, as a loaded
// node is.
bool ResolvePermutation(URigVMController& Controller, URigVMNode* Node, const FRigVMTemplateTypeMap& Types)
{
	URigVMTemplateNode* TemplateNode = Cast<URigVMTemplateNode>(Node);
	const FRigVMTemplate* Template = TemplateNode != nullptr ? TemplateNode->GetTemplate() : nullptr;
	if (Template == nullptr || Types.Num() != Template->NumArguments())
	{
		return false;
	}
	const int32 Permutation = Template->FindPermutation(Types);
	return Permutation != INDEX_NONE && Controller.FullyResolveTemplateNode(TemplateNode, Permutation, false);
}

bool OpNodeTypes(FGraphEditState& State, FReader& Op)
{
	FGraphTarget Target;
	FString Name;
	const FValue* Types = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("graph"), TEXT("name"), TEXT("types")}) || !ReadGraphField(State, Op, Target) || !Op.String(TEXT("name"), Name)
		|| !Op.Object(TEXT("types"), Types) || RequireNode(Op, Target, Name) == nullptr)
	{
		return false;
	}
	const FReportingScope Reporting(*Target.Controller);
	FRigVMTemplateTypeMap TypeMap;
	TArray<FString> Missed;
	for (int32 Index = 0; Index < Types->ObjectKeys.Num(); ++Index)
	{
		const FString Path = Name + TEXT(".") + Types->ObjectKeys[Index];
		const FString Field = FString::Printf(TEXT("types/%s"), *Types->ObjectKeys[Index]);
		const FValue& Wanted = Types->Elements[Index];
		URigVMPin* Pin = RequirePin(Op, Target, Path, *Field);
		if (Pin == nullptr)
		{
			return false;
		}
		if (Wanted.Kind != EAssetDocumentRuntimeValueKind::String)
		{
			return Op.Fail(TEXT("ControlRig.InvalidValue"), TEXT("A pin type is a RigVM type name (list_types kind rigvm_type)."), *Field);
		}
		FString CPPType;
		UObject* TypeObject = nullptr;
		if (!ParseTypeName(Wanted.StringValue, CPPType, TypeObject))
		{
			return Op.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("'%s' is not a RigVM type (list_types kind rigvm_type)."),
				*Wanted.StringValue), *Field);
		}
		TypeMap.Add(FName(*Types->ObjectKeys[Index]), FRigVMRegistry::Get().GetTypeIndex(FName(*CPPType), TypeObject));
		if (TypeName(Pin->GetCPPType(), Pin->GetCPPTypeObject()) == Wanted.StringValue)
		{
			continue;
		}
		if (!Pin->IsWildCard() || !Missed.IsEmpty())
		{
			Missed.Add(Types->ObjectKeys[Index]);
			continue;
		}
		const bool bResolved = Target.Controller->ResolveWildCardPin(Path, CPPType, TypeObject != nullptr ? FName(*TypeObject->GetPathName()) : NAME_None,
			false, false);
		if (!CheckReported(State, Op, FString::Printf(TEXT("Resolving %s to %s"), *Path, *Wanted.StringValue), TEXT("ControlRig.InvalidValue"), *Field))
		{
			return false;
		}
		State.bChanged = State.bChanged || bResolved;
		Pin = Target.Graph->FindPin(Path);
		if (Pin == nullptr || TypeName(Pin->GetCPPType(), Pin->GetCPPTypeObject()) != Wanted.StringValue)
		{
			Missed.Add(Types->ObjectKeys[Index]);
		}
	}
	if (Missed.IsEmpty())
	{
		return true;
	}
	const bool bPermutation = ResolvePermutation(*Target.Controller, Target.Graph->FindNodeByName(FName(*Name)), TypeMap);
	if (!CheckReported(State, Op, FString::Printf(TEXT("Resolving %s"), *Name), TEXT("ControlRig.InvalidValue"), TEXT("types")))
	{
		return false;
	}
	State.bChanged = State.bChanged || bPermutation;
	for (int32 Index = 0; Index < Types->ObjectKeys.Num(); ++Index)
	{
		const FString Path = Name + TEXT(".") + Types->ObjectKeys[Index];
		const FString& Wanted = Types->Elements[Index].StringValue;
		const URigVMPin* Pin = Target.Graph->FindPin(Path);
		if (Pin == nullptr || TypeName(Pin->GetCPPType(), Pin->GetCPPTypeObject()) != Wanted)
		{
			return Op.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("%s cannot take type %s (the node's permutations do not "
				"have it with the other pin types; it is %s)."), *Path, *Wanted,
				Pin == nullptr ? TEXT("missing") : *TypeName(Pin->GetCPPType(), Pin->GetCPPTypeObject())),
				*FString::Printf(TEXT("types/%s"), *Types->ObjectKeys[Index]));
		}
	}
	return true;
}

TArray<FString> AggregateNames(const URigVMNode& Node)
{
	TArray<FString> Names;
	for (const URigVMPin* Pin : Node.IsInputAggregate() ? Node.GetAggregateInputs() : Node.GetAggregateOutputs())
	{
		Names.Add(Pin->GetName());
	}
	return Names;
}

bool OpNodeAggregate(FGraphEditState& State, FReader& Op)
{
	FGraphTarget Target;
	FString Name;
	const FValue* Pins = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("graph"), TEXT("name"), TEXT("pins")}) || !ReadGraphField(State, Op, Target) || !Op.String(TEXT("name"), Name)
		|| !Op.Array(TEXT("pins"), Pins))
	{
		return false;
	}
	TArray<FString> Wanted;
	for (const FValue& Item : Pins->Elements)
	{
		if (Item.Kind != EAssetDocumentRuntimeValueKind::String || Item.StringValue.IsEmpty())
		{
			return Op.Fail(TEXT("ControlRig.InvalidValue"), TEXT("Aggregate is a list of pin names."), TEXT("pins"));
		}
		Wanted.Add(Item.StringValue);
	}
	const URigVMNode* Node = RequireNode(Op, Target, Name);
	if (Node == nullptr)
	{
		return false;
	}
	const FReportingScope Reporting(*Target.Controller);
	// The editor's "-" on an operand, then "+" for each missing one (the node becomes / stays an aggregate node with the
	// same name).
	for (const FString& Current : AggregateNames(*Node))
	{
		if (!Wanted.Contains(Current))
		{
			Target.Controller->RemoveAggregatePin(Name + TEXT(".") + Current, false, false);
			if (!CheckReported(State, Op, FString::Printf(TEXT("Removing operand %s"), *Current), TEXT("ControlRig.EditRefused"), TEXT("pins")))
			{
				return false;
			}
			State.bChanged = true;
			Node = RequireNode(Op, Target, Name);
			if (Node == nullptr)
			{
				return false;
			}
		}
	}
	for (const FString& Pin : Wanted)
	{
		if (!AggregateNames(*Node).Contains(Pin))
		{
			Target.Controller->AddAggregatePin(Name, Pin, FString(), false, false);
			if (!CheckReported(State, Op, FString::Printf(TEXT("Adding operand %s"), *Pin), TEXT("ControlRig.EditRefused"), TEXT("pins")))
			{
				return false;
			}
			State.bChanged = true;
			Node = RequireNode(Op, Target, Name);
			if (Node == nullptr)
			{
				return false;
			}
		}
	}
	const TArray<FString> After = AggregateNames(*Node);
	if (After != Wanted)
	{
		return Op.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("Node '%s' has the operands [%s], not [%s] (operands are added in "
			"order after the node's own pins)."), *Name, *FString::Join(After, TEXT(", ")), *FString::Join(Wanted, TEXT(", "))), TEXT("pins"));
	}
	return true;
}

bool OpNodeTraits(FGraphEditState& State, FReader& Op)
{
	FGraphTarget Target;
	FString Name;
	const FValue* Traits = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("graph"), TEXT("name"), TEXT("traits")}) || !ReadGraphField(State, Op, Target) || !Op.String(TEXT("name"), Name)
		|| !Op.Object(TEXT("traits"), Traits))
	{
		return false;
	}
	URigVMNode* Node = RequireNode(Op, Target, Name);
	if (Node == nullptr)
	{
		return false;
	}
	TMap<FString, FString> Wanted;
	for (int32 Index = 0; Index < Traits->ObjectKeys.Num(); ++Index)
	{
		const FValue* Type = Traits->Elements[Index].Kind == EAssetDocumentRuntimeValueKind::Object ? FindField(Traits->Elements[Index], TEXT("Type")) : nullptr;
		if (Type == nullptr || Type->Kind != EAssetDocumentRuntimeValueKind::String)
		{
			return Op.Fail(TEXT("ControlRig.InvalidValue"), TEXT("A trait is {Type: <trait struct path>}."), *FString::Printf(TEXT("traits/%s"),
				*Traits->ObjectKeys[Index]));
		}
		Wanted.Add(Traits->ObjectKeys[Index], Type->StringValue);
	}
	const FReportingScope Reporting(*Target.Controller);
	for (const URigVMPin* TraitPin : Node->GetTraitPins())
	{
		const FString* Type = Wanted.Find(TraitPin->GetName());
		if (Type == nullptr || *Type != PathOf(Node->GetTraitScriptStruct(TraitPin)))
		{
			const FName TraitName = TraitPin->GetFName();
			Target.Controller->RemoveTrait(FName(*Name), TraitName, false, false);
			if (!CheckReported(State, Op, FString::Printf(TEXT("Removing trait %s"), *TraitName.ToString()), TEXT("ControlRig.EditRefused"), TEXT("traits")))
			{
				return false;
			}
			State.bChanged = true;
		}
	}
	for (int32 Index = 0; Index < Traits->ObjectKeys.Num(); ++Index)
	{
		const FString& TraitName = Traits->ObjectKeys[Index];
		const FString Field = FString::Printf(TEXT("traits/%s"), *TraitName);
		Node = RequireNode(Op, Target, Name);
		if (Node == nullptr)
		{
			return false;
		}
		if (Node->FindPin(TraitName) != nullptr)
		{
			continue;
		}
		const FName Added = Target.Controller->AddTrait(FName(*Name), FName(*Wanted[TraitName]), FName(*TraitName), FString(), -1, false, false);
		if (!CheckReported(State, Op, FString::Printf(TEXT("Adding trait %s"), *TraitName), TEXT("ControlRig.EditRefused"), *Field))
		{
			return false;
		}
		State.bChanged = true;
		if (Added.ToString() != TraitName)
		{
			return Op.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("Trait %s (%s) could not be added%s."), *TraitName, *Wanted[TraitName],
				Added.IsNone() ? TEXT("") : *FString::Printf(TEXT(" (it was named %s)"), *Added.ToString())), *Field);
		}
	}
	return true;
}

// Units injected into a node's pins (node.injections {pin: [{Unit, Method?, Input, Output, Name?, Defaults?}]}, the
// whole set): the pin menu's Add Visual Debug / Add Interpolate (AddInjectedNode, which moves the pin's links and value
// onto the unit), and Remove for a pin whose list changed. Values are set as on any pin.
bool OpNodeInjections(FGraphEditState& State, FReader& Op)
{
	FGraphTarget Target;
	FString Name;
	const FValue* Injections = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("graph"), TEXT("name"), TEXT("injections")}) || !ReadGraphField(State, Op, Target)
		|| !Op.String(TEXT("name"), Name) || !Op.Object(TEXT("injections"), Injections))
	{
		return false;
	}
	URigVMNode* Node = RequireNode(Op, Target, Name);
	if (Node == nullptr)
	{
		return false;
	}
	URigVMController& Controller = *Target.Controller;
	const FReportingScope Reporting(Controller);
	TArray<FString> PinNames;
	for (const URigVMPin* Pin : Node->GetPins())
	{
		for (const URigVMInjectionInfo* Injection : Pin->GetInjectedNodes())
		{
			if (Injection != nullptr && Cast<URigVMUnitNode>(Injection->Node) != nullptr)
			{
				PinNames.AddUnique(Pin->GetName());
			}
		}
	}
	for (const FString& Key : Injections->ObjectKeys)
	{
		PinNames.AddUnique(Key);
	}
	for (const FString& PinName : PinNames)
	{
		const FString Field = FString::Printf(TEXT("injections/%s"), *PinName);
		const int32 KeyIndex = Injections->ObjectKeys.IndexOfByKey(PinName);
		const FValue* List = KeyIndex != INDEX_NONE ? &Injections->Elements[KeyIndex] : nullptr;
		if (List != nullptr && List->Kind != EAssetDocumentRuntimeValueKind::Array)
		{
			return Op.Fail(TEXT("ControlRig.InvalidValue"), TEXT("A pin's injections are a list of {Unit, Input, Output, Name?, Defaults?}."), *Field);
		}
		URigVMPin* Pin = Node->FindPin(PinName);
		if (Pin == nullptr || !Pin->IsRootPin())
		{
			TArray<FString> Top;
			for (const URigVMPin* Each : Node->GetPins())
			{
				Top.Add(Each->GetName());
			}
			return Op.Fail(TEXT("ControlRig.PinNotFound"), FString::Printf(TEXT("Node '%s' has no pin '%s' (units are injected into a node's "
				"top-level pins).%s"), *Name, *PinName, *UE::AssetDocument::DescribeNameCandidates(PinName, Top, TEXT("Pins"))), *Field);
		}
		const FString PinPath = Pin->GetPinPath();
		const bool bAsInput = Pin->GetDirection() != ERigVMPinDirection::Output;
		struct FWanted
		{
			UScriptStruct* Struct = nullptr;
			FString Method;
			FString Input;
			FString Output;
			FString UnitName;
			const FValue* Defaults = nullptr;
			FString Field;
		};
		TArray<FWanted> Wanted;
		const int32 Count = List != nullptr ? List->Elements.Num() : 0;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			FWanted Item;
			Item.Field = FString::Printf(TEXT("%s/%d"), *Field, Index);
			FReader Spec = Op.Child(*Item.Field, List->Elements[Index]);
			FString Unit;
			if (!Spec.OnlyFields({TEXT("Unit"), TEXT("Method"), TEXT("Input"), TEXT("Output"), TEXT("Name"), TEXT("Defaults")})
				|| !Spec.String(TEXT("Unit"), Unit) || !Spec.String(TEXT("Input"), Item.Input) || !Spec.String(TEXT("Output"), Item.Output)
				|| !Spec.String(TEXT("Name"), Item.UnitName, false) || !Spec.Object(TEXT("Defaults"), Item.Defaults, false))
			{
				return false;
			}
			Item.Method = FRigUnit::GetMethodName().ToString();
			if (!Spec.String(TEXT("Method"), Item.Method, false))
			{
				return false;
			}
			Item.Struct = FindUnit(Spec, Unit, TEXT("Unit"));
			if (Item.Struct == nullptr)
			{
				return false;
			}
			Wanted.Add(MoveTemp(Item));
		}
		TArray<URigVMUnitNode*> Live;
		for (const URigVMInjectionInfo* Injection : Pin->GetInjectedNodes())
		{
			if (URigVMUnitNode* Unit = Injection != nullptr ? Cast<URigVMUnitNode>(Injection->Node) : nullptr)
			{
				Live.Add(Unit);
			}
		}
		bool bSame = Live.Num() == Wanted.Num();
		for (int32 Index = 0; bSame && Index < Live.Num(); ++Index)
		{
			const URigVMInjectionInfo* Injection = Live[Index]->GetInjectionInfo();
			bSame = Live[Index]->GetScriptStruct() == Wanted[Index].Struct && Live[Index]->GetMethodName().ToString() == Wanted[Index].Method
				&& (Wanted[Index].UnitName.IsEmpty() || Live[Index]->GetName() == Wanted[Index].UnitName) && Injection != nullptr
				&& (Injection->InputPin != nullptr ? Injection->InputPin->GetName() : FString()) == Wanted[Index].Input
				&& Injection->OutputPin != nullptr && Injection->OutputPin->GetName() == Wanted[Index].Output;
		}
		if (!bSame)
		{
			for (int32 Index = 0; Index < Live.Num(); ++Index)
			{
				Controller.RemoveInjectedNode(PinPath, bAsInput, false, false);
				if (!CheckReported(State, Op, FString::Printf(TEXT("Removing the unit injected into %s"), *PinPath), TEXT("ControlRig.EditRefused"), *Field))
				{
					return false;
				}
			}
			for (const FWanted& Item : Wanted)
			{
				const URigVMInjectionInfo* Added = Controller.AddInjectedNode(PinPath, bAsInput, Item.Struct, FName(*Item.Method), FName(*Item.Input),
					FName(*Item.Output), Item.UnitName, false, false);
				if (!CheckReported(State, Op, FString::Printf(TEXT("Injecting %s into %s"), *UnitName(*Item.Struct), *PinPath),
					TEXT("ControlRig.EditRefused"), *Item.Field))
				{
					return false;
				}
				if (Added == nullptr || Added->Node == nullptr || (!Item.UnitName.IsEmpty() && Added->Node->GetName() != Item.UnitName))
				{
					return Op.Fail(TEXT("ControlRig.EditRefused"), FString::Printf(TEXT("%s could not be injected into %s%s (its input and output "
						"pins take the pin's type; an array pin takes none)."), *UnitName(*Item.Struct), *PinPath,
						Added != nullptr && Added->Node != nullptr ? *FString::Printf(TEXT(" as '%s' (it was named '%s')"), *Item.UnitName,
						*Added->Node->GetName()) : TEXT("")), *Item.Field);
				}
			}
			State.bChanged = true;
		}
		// Values of the injected units, as the pin widgets set them; values not listed go back to the unit's.
		Live.Reset();
		for (const URigVMInjectionInfo* Injection : Pin->GetInjectedNodes())
		{
			if (URigVMUnitNode* Unit = Injection != nullptr ? Cast<URigVMUnitNode>(Injection->Node) : nullptr)
			{
				Live.Add(Unit);
			}
		}
		for (int32 Index = 0; Index < Live.Num() && Index < Wanted.Num(); ++Index)
		{
			const FValue* Defaults = Wanted[Index].Defaults;
			for (int32 Key = 0; Defaults != nullptr && Key < Defaults->ObjectKeys.Num(); ++Key)
			{
				const FString ValueField = FString::Printf(TEXT("%s/Defaults/%s"), *Wanted[Index].Field, *Defaults->ObjectKeys[Key]);
				URigVMPin* UnitPin = Live[Index]->FindPin(Defaults->ObjectKeys[Key]);
				if (UnitPin == nullptr)
				{
					return Op.Fail(TEXT("ControlRig.PinNotFound"), FString::Printf(TEXT("The injected %s has no pin '%s'."), *Live[Index]->GetName(),
						*Defaults->ObjectKeys[Key]), *ValueField);
				}
				if (!State.Values.IsValid())
				{
					State.Values = MakeUnique<FTextValues>();
				}
				FString Text;
				FString Error;
				if (!State.Values->Decode(UnitPin->GetCPPType(), UnitPin->GetCPPTypeObject(), UnitPin->GetDefaultValue(), Defaults->Elements[Key], Text, Error))
				{
					return Op.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("%s (%s): %s"), *UnitPin->GetPinPath(),
						*TypeName(UnitPin->GetCPPType(), UnitPin->GetCPPTypeObject()), *Error), *ValueField);
				}
				if (UnitPin->GetCPPType() == TEXT("bool"))
				{
					Text = Text.Equals(TEXT("True"), ESearchCase::IgnoreCase) ? TEXT("true") : TEXT("false");
				}
				if (State.Values->Same(UnitPin->GetCPPType(), UnitPin->GetCPPTypeObject(), Text, UnitPin->GetDefaultValue()))
				{
					continue;
				}
				Controller.SetPinDefaultValue(UnitPin->GetPinPath(), Text, true, false, false, false);
				if (!CheckReported(State, Op, FString::Printf(TEXT("The value of %s"), *UnitPin->GetPinPath()), TEXT("ControlRig.InvalidValue"), *ValueField))
				{
					return false;
				}
				State.bChanged = true;
			}
		}
	}
	return true;
}

// ---- links and pins ------------------------------------------------------------------------------------------

bool OpLink(FGraphEditState& State, FReader& Op, bool bAdd)
{
	FGraphTarget Target;
	FString From;
	FString To;
	if (!Op.OnlyFields({TEXT("op"), TEXT("graph"), TEXT("from"), TEXT("to")}) || !ReadGraphField(State, Op, Target) || !Op.String(TEXT("from"), From)
		|| !Op.String(TEXT("to"), To) || RequirePin(Op, Target, From, TEXT("from")) == nullptr || RequirePin(Op, Target, To, TEXT("to")) == nullptr)
	{
		return false;
	}
	const FReportingScope Reporting(*Target.Controller);
	const bool bDone = bAdd ? Target.Controller->AddLink(From, To, false, false) : Target.Controller->BreakLink(From, To, false, false);
	if (!CheckReported(State, Op, FString::Printf(TEXT("%s %s -> %s"), bAdd ? TEXT("Linking") : TEXT("Breaking the link"), *From, *To),
		bAdd ? TEXT("ControlRig.LinkRefused") : TEXT("ControlRig.EditRefused"), TEXT("to")))
	{
		return false;
	}
	if (!bDone)
	{
		return Op.Fail(bAdd ? TEXT("ControlRig.LinkRefused") : TEXT("ControlRig.EditFailed"), FString::Printf(TEXT("%s -> %s could not be %s."), *From, *To,
			bAdd ? TEXT("linked (incompatible pin types or directions)") : TEXT("unlinked")), TEXT("to"));
	}
	State.bChanged = true;
	return true;
}

bool OpPinDefault(FGraphEditState& State, FReader& Op)
{
	FGraphTarget Target;
	FString Path;
	if (!Op.OnlyFields({TEXT("op"), TEXT("graph"), TEXT("pin"), TEXT("value")}) || !ReadGraphField(State, Op, Target) || !Op.String(TEXT("pin"), Path))
	{
		return false;
	}
	URigVMPin* Pin = RequirePin(Op, Target, Path, TEXT("pin"));
	const FValue* Value = Op.Find(TEXT("value"));
	if (Pin == nullptr)
	{
		return false;
	}
	if (Value == nullptr)
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("pin.default takes a value (null resets)."), TEXT("value"));
	}
	const FReportingScope Reporting(*Target.Controller);
	bool bDone = false;
	if (Value->Kind == EAssetDocumentRuntimeValueKind::Null)
	{
		bDone = Target.Controller->ResetPinDefaultValue(Path, false, false);
	}
	else
	{
		if (!State.Values.IsValid())
		{
			State.Values = MakeUnique<FTextValues>();
		}
		FString Text;
		FString Error;
		if (!State.Values->Decode(Pin->GetCPPType(), Pin->GetCPPTypeObject(), Pin->GetDefaultValue(), *Value, Text, Error))
		{
			return Op.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("%s (%s): %s"), *Path, *TypeName(Pin->GetCPPType(),
				Pin->GetCPPTypeObject()), *Error), TEXT("value"));
		}
		if (Pin->GetCPPType() == TEXT("bool"))
		{
			// The bool pin widget's text (SGraphPinBool): the compiler shares literals by text, so "true" and "True"
			// are two literals.
			Text = Text.Equals(TEXT("True"), ESearchCase::IgnoreCase) ? TEXT("true") : TEXT("false");
		}
		bDone = Target.Controller->SetPinDefaultValue(Path, Text, true, false, false, false);
	}
	if (!CheckReported(State, Op, FString::Printf(TEXT("The value of %s"), *Path), TEXT("ControlRig.InvalidValue"), TEXT("value")))
	{
		return false;
	}
	if (!bDone && Value->Kind != EAssetDocumentRuntimeValueKind::Null)
	{
		return Op.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("%s does not take that value (a linked, output or "
			"variable-bound pin has no value)."), *Path), TEXT("value"));
	}
	State.bChanged = true;
	return true;
}

bool OpPinBind(FGraphEditState& State, FReader& Op)
{
	FGraphTarget Target;
	FString Path;
	FString Variable;
	if (!Op.OnlyFields({TEXT("op"), TEXT("graph"), TEXT("pin"), TEXT("variable")}) || !ReadGraphField(State, Op, Target) || !Op.String(TEXT("pin"), Path)
		|| !Op.String(TEXT("variable"), Variable) || RequirePin(Op, Target, Path, TEXT("pin")) == nullptr)
	{
		return false;
	}
	const FReportingScope Reporting(*Target.Controller);
	const bool bDone = Variable.IsEmpty() ? Target.Controller->UnbindPinFromVariable(Path, false, false)
		: Target.Controller->BindPinToVariable(Path, Variable, false, false);
	if (!CheckReported(State, Op, FString::Printf(TEXT("Binding %s"), *Path), TEXT("ControlRig.BindingRefused"), TEXT("variable")))
	{
		return false;
	}
	if (!bDone)
	{
		return Op.Fail(TEXT("ControlRig.BindingRefused"), Variable.IsEmpty() ? FString::Printf(TEXT("%s could not be unbound."), *Path)
			: FString::Printf(TEXT("%s cannot be bound to '%s' (no such variable, or its type does not match the pin)."), *Path, *Variable),
			TEXT("variable"));
	}
	State.bChanged = true;
	return true;
}

bool OpPinArray(FGraphEditState& State, FReader& Op)
{
	FGraphTarget Target;
	FString Path;
	int64 Size = 0;
	if (!Op.OnlyFields({TEXT("op"), TEXT("graph"), TEXT("pin"), TEXT("size")}) || !ReadGraphField(State, Op, Target) || !Op.String(TEXT("pin"), Path)
		|| !Op.Int(TEXT("size"), Size))
	{
		return false;
	}
	URigVMPin* Pin = RequirePin(Op, Target, Path, TEXT("pin"));
	if (Pin == nullptr)
	{
		return false;
	}
	if (!Pin->IsArray() || Size < 0 || Size > 100000)
	{
		return Op.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("%s is not an array pin, or %lld is not an array size."), *Path, Size),
			TEXT("pin"));
	}
	if (Pin->GetArraySize() == Size)
	{
		return true;
	}
	const FReportingScope Reporting(*Target.Controller);
	Target.Controller->SetArrayPinSize(Path, static_cast<int32>(Size), FString(), false, false);
	if (!CheckReported(State, Op, FString::Printf(TEXT("Resizing %s"), *Path), TEXT("ControlRig.InvalidValue"), TEXT("size")))
	{
		return false;
	}
	Pin = Target.Graph->FindPin(Path);
	if (Pin == nullptr || Pin->GetArraySize() != Size)
	{
		return Op.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("%s could not be resized to %lld."), *Path, Size), TEXT("size"));
	}
	State.bChanged = true;
	return true;
}
// The variable map of a function reference (the node's Details "Variables" remapping: SetFunctionVariableMapping),
// complete: inner variables the map does not list are unmapped.
bool OpNodeRemap(FGraphEditState& State, FReader& Op)
{
	FGraphTarget Target;
	FString Name;
	const FValue* Map = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("graph"), TEXT("name"), TEXT("map")}) || !ReadGraphField(State, Op, Target) || !Op.String(TEXT("name"), Name)
		|| !Op.Object(TEXT("map"), Map))
	{
		return false;
	}
	URigVMFunctionReferenceNode* Reference = Cast<URigVMFunctionReferenceNode>(RequireNode(Op, Target, Name));
	if (Reference == nullptr)
	{
		return Op.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("'%s' is not a function node (VariableMap belongs to function "
			"nodes)."), *Name), TEXT("map"));
	}
	const FReportingScope Reporting(*Target.Controller);
	TMap<FName, FName> Wanted;
	for (int32 Index = 0; Index < Map->ObjectKeys.Num(); ++Index)
	{
		const FValue& Outer = Map->Elements[Index];
		if (Outer.Kind != EAssetDocumentRuntimeValueKind::String || Outer.StringValue.IsEmpty())
		{
			return Op.Fail(TEXT("ControlRig.InvalidValue"), TEXT("A variable map is {inner variable: member variable}."),
				*FString::Printf(TEXT("map/%s"), *Map->ObjectKeys[Index]));
		}
		Wanted.Add(FName(*Map->ObjectKeys[Index]), FName(*Outer.StringValue));
	}
	TArray<FName> Current;
	Reference->GetVariableMap().GetKeys(Current);
	for (const FName& Inner : Current)
	{
		if (!Wanted.Contains(Inner))
		{
			SetFunctionVariableMapping(*Target.Controller, *Reference, Inner, NAME_None, false);
			if (!CheckReported(State, Op, FString::Printf(TEXT("Unmapping '%s'"), *Inner.ToString()), TEXT("ControlRig.InvalidValue"), TEXT("map")))
			{
				return false;
			}
		}
	}
	for (const TPair<FName, FName>& Pair : Wanted)
	{
		if (Reference->GetOuterVariableName(Pair.Key) == Pair.Value)
		{
			continue;
		}
		const FString Field = FString::Printf(TEXT("map/%s"), *Pair.Key.ToString());
		const bool bSet = SetFunctionVariableMapping(*Target.Controller, *Reference, Pair.Key, Pair.Value, false);
		if (!CheckReported(State, Op, FString::Printf(TEXT("Mapping '%s' to '%s'"), *Pair.Key.ToString(), *Pair.Value.ToString()),
			TEXT("ControlRig.InvalidValue"), *Field))
		{
			return false;
		}
		if (!bSet)
		{
			return Op.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("Function node '%s' does not remap '%s' (it remaps the member "
				"variables its function reads, for a function of another rig)."), *Name, *Pair.Key.ToString()), *Field);
		}
	}
	State.bChanged = true;
	return true;
}

} // namespace

FReportingScope::FReportingScope(URigVMController& InController)
	: Controller(InController), bPrevious(InController.IsReportingEnabled())
{
	Controller.EnableReporting(true);
}

FReportingScope::~FReportingScope()
{
	Controller.EnableReporting(bPrevious);
}

bool CheckReported(FGraphEditState& State, FReader& Op, const FString& What, const TCHAR* Code, const TCHAR* Field)
{
	if (State.Errors == nullptr)
	{
		return true;
	}
	// The diagnostics AST a link builds (cycle check) reports a mutable node not yet wired to an event as an error. While
	// a graph is being wired, as in the editor, that is a transient compile diagnostic, not a refusal of the op.
	State.Errors->RemoveAll([](const FString& Message) { return Message.Contains(TEXT("is not linked to execution")); });
	if (State.Errors->IsEmpty())
	{
		return true;
	}
	const FString Messages = Joined(*State.Errors);
	State.Errors->Reset();
	if (Messages.Contains(TEXT("already exists in the graph")) || Messages.Contains(TEXT("can only contain one single")))
	{
		Code = TEXT("ControlRig.EventAlreadyExists");
	}
	return Op.Fail(Code, FString::Printf(TEXT("%s was refused by the Control Rig editor: %s"), *What, *Messages), Field);
}

URigVMController* GetLibraryController(UControlRigBlueprint& Blueprint)
{
	URigVMBlueprint& RigVM = Blueprint;
	URigVMFunctionLibrary* Library = RigVM.GetLocalFunctionLibrary();
	return Library != nullptr ? RigVM.GetOrCreateController(Library) : nullptr;
}

bool ReadGraphField(FGraphEditState& State, FReader& Op, FGraphTarget& Out)
{
	const FValue* Field = Op.Find(TEXT("graph"));
	if (Field != nullptr && Field->Kind == EAssetDocumentRuntimeValueKind::Array)
	{
		TArray<FString> Scope;
		for (const FValue& Item : Field->Elements)
		{
			if (Item.Kind != EAssetDocumentRuntimeValueKind::String)
			{
				return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("A graph scope is a list of names."), TEXT("graph"));
			}
			Scope.Add(Item.StringValue);
		}
		Out.GraphName = FString::Join(Scope, TEXT("/"));
		FString Error;
		Out.Graph = ResolveScope(State.Blueprint, Scope, Error);
		if (Out.Graph == nullptr)
		{
			return Op.Fail(TEXT("ControlRig.GraphNotFound"), Error, TEXT("graph"));
		}
	}
	else
	{
		if (!Op.String(TEXT("graph"), Out.GraphName))
		{
			return false;
		}
		Out.Graph = FindModel(State.Blueprint, Out.GraphName);
		if (Out.Graph == nullptr)
		{
			return Op.Fail(TEXT("ControlRig.GraphNotFound"), FString::Printf(TEXT("The rig has no graph '%s'.%s"), *Out.GraphName,
				*GraphCandidates(State.Blueprint, Out.GraphName)), TEXT("graph"));
		}
	}
	Out.Controller = static_cast<URigVMBlueprint&>(State.Blueprint).GetOrCreateController(Out.Graph);
	if (Out.Controller == nullptr)
	{
		return Op.Fail(TEXT("ControlRig.EditFailed"), FString::Printf(TEXT("Graph '%s' has no controller."), *Out.GraphName), TEXT("graph"));
	}
	return true;
}

bool IsGraphOp(const FString& Kind)
{
	return (Kind.StartsWith(TEXT("graph.")) && Kind != TEXT("graph.forwards_solve")) || Kind.StartsWith(TEXT("node.")) || Kind.StartsWith(TEXT("link."))
		|| Kind.StartsWith(TEXT("pin."));
}

bool RunGraphOp(FGraphEditState& State, FReader& Op, const FString& Kind, int32 OpIndex)
{
	if (Kind == TEXT("graph.add")) { return OpGraphAdd(State, Op); }
	if (Kind == TEXT("graph.remove")) { return OpGraphRemove(State, Op); }
	if (Kind == TEXT("graph.rename")) { return OpGraphRename(State, Op); }
	if (Kind == TEXT("node.add")) { return OpNodeAdd(State, Op, OpIndex); }
	if (Kind == TEXT("node.remove")) { return OpNodeRemove(State, Op); }
	if (Kind == TEXT("node.rename")) { return OpNodeRename(State, Op); }
	if (Kind == TEXT("node.set")) { return OpNodeSet(State, Op); }
	if (Kind == TEXT("node.types")) { return OpNodeTypes(State, Op); }
	if (Kind == TEXT("node.aggregate")) { return OpNodeAggregate(State, Op); }
	if (Kind == TEXT("node.traits")) { return OpNodeTraits(State, Op); }
	if (Kind == TEXT("node.injections")) { return OpNodeInjections(State, Op); }
	if (Kind == TEXT("node.remap")) { return OpNodeRemap(State, Op); }
	if (Kind == TEXT("link.add")) { return OpLink(State, Op, true); }
	if (Kind == TEXT("link.break")) { return OpLink(State, Op, false); }
	if (Kind == TEXT("pin.default")) { return OpPinDefault(State, Op); }
	if (Kind == TEXT("pin.bind")) { return OpPinBind(State, Op); }
	if (Kind == TEXT("pin.array")) { return OpPinArray(State, Op); }
	return Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("Unknown op '%s'."), *Kind), TEXT("op"));
}
} // namespace UE::AssetDocument::Graph::Rig::ControlRig
