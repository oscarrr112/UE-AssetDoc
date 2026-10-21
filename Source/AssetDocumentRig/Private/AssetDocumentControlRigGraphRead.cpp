// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Graph records of rig.controlrig.read.v1 (design §3.3, §3.5): the event graphs (models), the local function library
// and the collapsed graphs inside them, one record per node, keyed by the model's own node names. Injected nodes
// (variable bindings) are pin state, not nodes; aggregate nodes are one node (their inner chain is derived).

#include "AssetDocumentControlRig.h"

#include "AssetDocumentGraphValue.h"
#include "ControlRigBlueprintLegacy.h"
#include "Misc/EngineVersionComparison.h"
#include "RigVMModel/Nodes/RigVMAggregateNode.h"
#include "RigVMModel/Nodes/RigVMCollapseNode.h"
#include "RigVMModel/Nodes/RigVMCommentNode.h"
#include "RigVMModel/Nodes/RigVMDispatchNode.h"
#include "RigVMModel/Nodes/RigVMEnumNode.h"
#include "RigVMModel/Nodes/RigVMFunctionEntryNode.h"
#include "RigVMModel/Nodes/RigVMFunctionReferenceNode.h"
#include "RigVMModel/Nodes/RigVMFunctionReturnNode.h"
#include "RigVMModel/Nodes/RigVMInvokeEntryNode.h"
#include "RigVMModel/Nodes/RigVMRerouteNode.h"
#include "RigVMModel/Nodes/RigVMTemplateNode.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/Nodes/RigVMVariableNode.h"
#include "RigVMModel/RigVMFunctionLibrary.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMLink.h"
#include "RigVMModel/RigVMPin.h"
#include "RigVMCore/RigVMDispatchFactory.h"

namespace UE::AssetDocument::Graph::Rig::ControlRig
{
namespace ControlRigGraphReadLocal
{
double Clean(double Value)
{
	return Value == 0.0 ? 0.0 : Value;
}

FValue Pair(const FVector2D& Value)
{
	return MakeArray({MakeNumber(Clean(ShortestFloat(Value.X))), MakeNumber(Clean(ShortestFloat(Value.Y)))});
}

FValue Color(const FLinearColor& Value)
{
	FObjectBuilder Object;
	Object.Add(TEXT("R"), MakeNumber(Clean(ShortestFloat(Value.R))));
	Object.Add(TEXT("G"), MakeNumber(Clean(ShortestFloat(Value.G))));
	Object.Add(TEXT("B"), MakeNumber(Clean(ShortestFloat(Value.B))));
	Object.Add(TEXT("A"), MakeNumber(Clean(ShortestFloat(Value.A))));
	return Object.Build();
}

// The user-set node colour (NodeColorType UserDefined); protected on URigVMNode, read through reflection.
bool GetUserColor(const URigVMNode& Node, FLinearColor& OutColor)
{
	static const FStructProperty* ColorProperty = FindFProperty<FStructProperty>(URigVMNode::StaticClass(), TEXT("NodeColor"));
	static const FProperty* TypeProperty = FindFProperty<FProperty>(URigVMNode::StaticClass(), TEXT("NodeColorType"));
	if (ColorProperty == nullptr || TypeProperty == nullptr)
	{
		return false;
	}
	const uint8 Type = *TypeProperty->ContainerPtrToValuePtr<uint8>(&Node);
	if (Type != static_cast<uint8>(ERigVMNodeColorType::UserDefined))
	{
		return false;
	}
	OutColor = *ColorProperty->ContainerPtrToValuePtr<FLinearColor>(&Node);
	return true;
}

// A pin some engines give a public function for each member variable its graph reads (an input variable). The editor
// adds it itself, and a reference binds it to a variable of the calling rig: the document writes that as the reference's
// VariableMap, not as a pin of the function or the reference.
bool IsInputVariablePin(const URigVMPin& Pin)
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	return Pin.GetParentPin() == nullptr && Pin.IsDefinedAsInputVariable();
#else
	return false;
#endif
}

bool HasBoundSubPin(const URigVMPin& Pin)
{
	for (const URigVMPin* SubPin : Pin.GetSubPins())
	{
		if (SubPin->IsBoundToVariable() || HasBoundSubPin(*SubPin))
		{
			return true;
		}
	}
	return false;
}

bool IsSourceNode(const URigVMNode* Node)
{
	return Node != nullptr && !Node->IsInjected();
}

class FGraphReader
{
public:
	FGraphReader(UControlRigBlueprint& InBlueprint, bool bInInspect, FTextValues& InValues, TArray<FValue>& InRecords)
		: Blueprint(InBlueprint), bInspect(bInInspect), Values(InValues), Records(InRecords)
		, LocalPackage(InBlueprint.GetPackage()->GetName())
	{
	}

	bool Read()
	{
		const FRigVMClient* Client = static_cast<URigVMBlueprint&>(Blueprint).GetRigVMClient();
		if (Client == nullptr)
		{
			return true;
		}
		const URigVMGraph* Default = Client->GetDefaultModel();
		for (const URigVMGraph* Model : Client->GetModels())
		{
			if (Model == nullptr)
			{
				continue;
			}
			const FString Name = GraphName(*Model, Default);
			FObjectBuilder Record;
			Record.AddString(TEXT("kind"), TEXT("graph"));
			Record.AddString(TEXT("name"), Name);
			Records.Add(Record.Build());
			if (!ReadGraph(*Model, {TEXT("Graphs"), Name}))
			{
				return false;
			}
		}
		if (const URigVMFunctionLibrary* Library = Client->GetFunctionLibrary())
		{
			for (URigVMLibraryNode* Function : Library->GetFunctions())
			{
				if (Function != nullptr && !ReadFunction(*Function))
				{
					return false;
				}
			}
		}
		return true;
	}

private:
	UControlRigBlueprint& Blueprint;
	bool bInspect;
	// The full pin types of the node being read when its PinTypes are written sparse (AddPinTypes).
	FValue PendingFullTypes;
	bool bPendingFullTypes = false;
	bool bSparseTypes = true;
	FTextValues& Values;
	TArray<FValue>& Records;
	FString LocalPackage;

	FValue EncodeText(const URigVMPin& Pin, const FString& Text)
	{
		FValue Value;
		FString Error;
		if (Values.Encode(Pin.GetCPPType(), Pin.GetCPPTypeObject(), Text, Value, Error))
		{
			return Value;
		}
		// Export text the typed codec cannot read stays text (the model's own form).
		return MakeString(Text);
	}

	FValue PinDescription(const URigVMPin& Pin, bool bWithDefault)
	{
		FObjectBuilder Entry;
		Entry.AddString(TEXT("Name"), Pin.GetName());
		Entry.AddString(TEXT("Type"), TypeName(Pin.GetCPPType(), Pin.GetCPPTypeObject()));
		if (Pin.GetDirection() == ERigVMPinDirection::IO)
		{
			Entry.AddString(TEXT("Direction"), TEXT("IO"));
		}
		if (bWithDefault && !Pin.IsExecuteContext() && !IsValuelessType(Pin.GetCPPType(), Pin.GetCPPTypeObject()))
		{
			const FString Text = Pin.GetDefaultValue();
			if (!Text.IsEmpty() && !Values.Same(Pin.GetCPPType(), Pin.GetCPPTypeObject(), Text, FString()))
			{
				Entry.Add(TEXT("Default"), EncodeText(Pin, Text));
			}
		}
		return Entry.Build();
	}

	// Exposed pins of a function / collapse node in pin order: {Inputs, Outputs}. Execute pins are inputs with Direction
	// IO (a mutable function's ExecuteContext, or the pin a collapse kept the collapsed execute pin's name for). With
	// bWithDefaults (functions) each pin's default value: what the function's details panel shows and its references
	// start with (a collapse node's input values are its node Defaults).
	void AddInterface(const URigVMNode& Node, FObjectBuilder& Entry, bool bWithDefaults)
	{
		TArray<FValue> Inputs;
		TArray<FValue> Outputs;
		for (const URigVMPin* Pin : Node.GetPins())
		{
			if (IsInputVariablePin(*Pin))
			{
				continue;
			}
			if (Pin->GetDirection() == ERigVMPinDirection::Output)
			{
				Outputs.Add(PinDescription(*Pin, bWithDefaults));
			}
			else if (Pin->GetDirection() == ERigVMPinDirection::Input || Pin->GetDirection() == ERigVMPinDirection::IO)
			{
				Inputs.Add(PinDescription(*Pin, bWithDefaults));
			}
		}
		Entry.Add(TEXT("Inputs"), MakeArray(MoveTemp(Inputs)));
		Entry.Add(TEXT("Outputs"), MakeArray(MoveTemp(Outputs)));
	}

	// Category, keywords and description of a function or collapse node (the graph's details panel).
	static void AddHeader(const URigVMLibraryNode& Node, FObjectBuilder& Entry)
	{
		if (!Node.GetNodeCategory().IsEmpty())
		{
			Entry.AddString(TEXT("Category"), Node.GetNodeCategory());
		}
		if (!Node.GetNodeKeywords().IsEmpty())
		{
			Entry.AddString(TEXT("Keywords"), Node.GetNodeKeywords());
		}
		if (!Node.GetNodeDescription().IsEmpty())
		{
			Entry.AddString(TEXT("Description"), Node.GetNodeDescription());
		}
	}

	// A graph's own local variables. The editor gives them to functions only; collapse graphs made by older engines
	// (a function localized into a collapse node) may still hold some, which their variable nodes read.
	void AddLocals(const URigVMGraph& Graph, FObjectBuilder& Entry)
	{
		TArray<FValue> Locals;
		for (const FRigVMGraphVariableDescription& Local : Graph.GetLocalVariables(false))
		{
			FObjectBuilder Variable;
			Variable.Add(TEXT("Name"), MakeName(Local.Name));
			Variable.AddString(TEXT("Type"), TypeName(Local.CPPType, Local.CPPTypeObject));
			// Omitted when it reads as its type's default (an empty array's "()" and no text are the same value).
			if (!Local.DefaultValue.IsEmpty() && !Values.Same(Local.CPPType, Local.CPPTypeObject, Local.DefaultValue, FString()))
			{
				FValue Default;
				FString Error;
				if (!Values.Encode(Local.CPPType, Local.CPPTypeObject, Local.DefaultValue, Default, Error))
				{
					Default = MakeString(Local.DefaultValue);
				}
				Variable.Add(TEXT("Default"), MoveTemp(Default));
			}
			Locals.Add(Variable.Build());
		}
		if (!Locals.IsEmpty())
		{
			Entry.Add(TEXT("LocalVariables"), MakeArray(MoveTemp(Locals)));
		}
	}

	bool ReadFunction(URigVMLibraryNode& Function)
	{
		FObjectBuilder Entry;
		if (Blueprint.IsFunctionPublic(Function.GetFName()))
		{
			Entry.AddBool(TEXT("Public"), true);
		}
		AddHeader(Function, Entry);
		FLinearColor UserColor;
		if (GetUserColor(Function, UserColor))
		{
			Entry.Add(TEXT("Color"), Color(UserColor));
		}
		AddInterface(Function, Entry, true);
		const URigVMGraph* Body = Function.GetContainedGraph();
		if (Body != nullptr)
		{
			AddLocals(*Body, Entry);
		}
		FObjectBuilder Record;
		Record.AddString(TEXT("kind"), TEXT("function"));
		Record.AddString(TEXT("name"), Function.GetName());
		Record.Add(TEXT("entry"), Entry.Build());
		Records.Add(Record.Build());
		return Body == nullptr || ReadGraph(*Body, {TEXT("Functions"), Function.GetName()});
	}

	bool ReadGraph(const URigVMGraph& Graph, const TArray<FString>& Scope)
	{
		for (URigVMNode* Node : Graph.GetNodes())
		{
			if (Node == nullptr || Node->IsInjected())
			{
				continue;
			}
			if (Node->IsA<URigVMFunctionEntryNode>() || Node->IsA<URigVMFunctionReturnNode>())
			{
				// The interface nodes: their position, the values and links of the Return node's pins (the outputs) and
				// expanded pins.
				FObjectBuilder Entry;
				Entry.Add(TEXT("Position"), Pair(Node->GetPosition()));
				AddPinState(*Node, Entry);
				FObjectBuilder Record;
				Record.AddString(TEXT("kind"), TEXT("interface"));
				Record.Add(TEXT("scope"), MakeStringArray(Scope));
				Record.AddString(TEXT("name"), Node->IsA<URigVMFunctionEntryNode>() ? TEXT("Entry") : TEXT("Return"));
				Record.Add(TEXT("entry"), Entry.Build());
				Records.Add(Record.Build());
				continue;
			}
			if (!ReadNode(*Node, Scope))
			{
				return false;
			}
		}
		return true;
	}

	FString FunctionName(const URigVMFunctionReferenceNode& Reference) const
	{
		const FRigVMGraphFunctionIdentifier Identifier = Reference.GetFunctionIdentifier();
		const FString Name = Identifier.GetFunctionName();
		const FString HostPackage = Identifier.HostObject.GetLongPackageName();
		if (HostPackage.IsEmpty() || HostPackage == LocalPackage)
		{
			return Name;
		}
		FString Asset = Identifier.HostObject.GetAssetName();
		Asset.RemoveFromEnd(TEXT("_C"), ESearchCase::CaseSensitive);
		return FString::Printf(TEXT("%s.%s:%s"), *HostPackage, *Asset, *Name);
	}

	// Template pin types of a resolved template / dispatch node, in pin order: only the ones that determine the
	// others (the template resolves the rest when they are set, as the editor's pin type menu does): pins of one type
	// in every permutation are left out, and the pins after which the permutation is decided (an Array Get At Index
	// with Array set has its Element). The full map of the non-wildcard pins goes to PendingFullTypes (Diff / Apply
	// compare an authored type of a pin left out against it).
	void AddPinTypes(const URigVMTemplateNode& Node, FObjectBuilder& Entry)
	{
		const FRigVMTemplate* Template = Node.GetTemplate();
		if (Template == nullptr)
		{
			return;
		}
		FObjectBuilder Full;
		FRigVMTemplate::FTypeMap Resolved;
		TArray<const URigVMPin*> Deciding;
		for (const URigVMPin* Pin : Node.GetPins())
		{
			const FRigVMTemplateArgument* Argument = Pin->IsExecuteContext() || Pin->IsWildCard() ? nullptr : Template->FindArgument(Pin->GetFName());
			if (Argument == nullptr)
			{
				continue;
			}
			Full.AddString(Pin->GetName(), TypeName(Pin->GetCPPType(), Pin->GetCPPTypeObject()));
			Resolved.Add(Pin->GetFName(), Pin->GetTypeIndex());
			if (!Argument->IsSingleton())
			{
				Deciding.Add(Pin);
			}
		}
		if (Resolved.IsEmpty())
		{
			return;
		}
		if (!bSparseTypes)
		{
			Entry.Add(TEXT("PinTypes"), Full.Build());
			return;
		}
		auto Decides = [Template, &Resolved](const FRigVMTemplate::FTypeMap& Chosen)
		{
			FRigVMTemplate::FTypeMap Types = Chosen;
			int32 Permutation = INDEX_NONE;
			if (!Template->FullyResolve(Types, Permutation))
			{
				return false;
			}
			for (const TPair<FName, TRigVMTypeIndex>& Pair : Resolved)
			{
				if (Types.FindRef(Pair.Key) != Pair.Value)
				{
					return false;
				}
			}
			return true;
		};
		FRigVMTemplate::FTypeMap Chosen;
		FObjectBuilder Types;
		bool bAny = false;
		bool bDecided = false;
		for (const URigVMPin* Pin : Deciding)
		{
			if (Decides(Chosen))
			{
				bDecided = true;
				break;
			}
			Chosen.Add(Pin->GetFName(), Pin->GetTypeIndex());
			Types.AddString(Pin->GetName(), TypeName(Pin->GetCPPType(), Pin->GetCPPTypeObject()));
			bAny = true;
		}
		bDecided = bDecided || Decides(Chosen);
		if (!bDecided)
		{
			// Not decided by the template (a partly resolved node): every non-wildcard pin, as the node has them.
			Entry.Add(TEXT("PinTypes"), Full.Build());
			return;
		}
		if (bAny)
		{
			Entry.Add(TEXT("PinTypes"), Types.Build());
		}
		PendingFullTypes = Full.Build();
		bPendingFullTypes = true;
	}

	// The kind keys of a unit / template / dispatch node.
	void AddTemplateKind(const URigVMTemplateNode& Node, FObjectBuilder& Entry)
	{
		if (const URigVMDispatchNode* Dispatch = Cast<URigVMDispatchNode>(&Node))
		{
			const FRigVMDispatchFactory* Factory = Dispatch->GetFactory();
			Entry.AddString(TEXT("Template"), Factory != nullptr ? Factory->GetFactoryName().ToString() : Node.GetNotation().ToString());
			AddPinTypes(Node, Entry);
			return;
		}
		const UScriptStruct* Struct = Node.GetScriptStruct();
		if (Struct == nullptr && Node.GetTemplate() != nullptr)
		{
			// A unit node a link left between permutations (a float Exponential linked from a float pin also fits the
			// double one) is resolved by its pin types when it compiles: read the unit those types pick.
			const TArray<int32> Permutations = Node.GetResolvedPermutationIndices(false);
			const FRigVMFunction* Function = Permutations.Num() == 1 ? Node.GetTemplate()->GetPermutation(Permutations[0]) : nullptr;
			Struct = Function != nullptr ? Function->Struct : nullptr;
		}
		if (Struct != nullptr)
		{
			Entry.AddString(TEXT("Unit"), UnitName(*Struct));
			const FName Method = Node.GetMethodName();
			if (!Method.IsNone() && Method != TEXT("Execute"))
			{
				Entry.Add(TEXT("Method"), MakeName(Method));
			}
			return;
		}
		Entry.AddString(TEXT("Template"), Node.GetNotation().ToString());
		AddPinTypes(Node, Entry);
	}

	void AddKind(URigVMNode& Node, FObjectBuilder& Entry, bool& bOutCollapse)
	{
		bOutCollapse = false;
		if (const URigVMCommentNode* Comment = Cast<URigVMCommentNode>(&Node))
		{
			Entry.AddString(TEXT("Comment"), Comment->GetCommentText());
			return;
		}
		if (const URigVMRerouteNode* Reroute = Cast<URigVMRerouteNode>(&Node))
		{
			const URigVMPin* Value = Reroute->GetPins().IsEmpty() ? nullptr : Reroute->GetPins()[0];
			Entry.AddString(TEXT("Reroute"), Value != nullptr ? TypeName(Value->GetCPPType(), Value->GetCPPTypeObject()) : FString());
			return;
		}
		if (const URigVMEnumNode* Enum = Cast<URigVMEnumNode>(&Node))
		{
			Entry.AddString(TEXT("Enum"), PathOf(Enum->GetEnum()));
			return;
		}
		if (const URigVMInvokeEntryNode* Invoke = Cast<URigVMInvokeEntryNode>(&Node))
		{
			Entry.Add(TEXT("InvokeEvent"), MakeName(Invoke->GetEntryName()));
			return;
		}
		if (const URigVMVariableNode* Variable = Cast<URigVMVariableNode>(&Node))
		{
			Entry.Add(TEXT("Variable"), MakeName(Variable->GetVariableName()));
			if (!Variable->IsGetter())
			{
				Entry.AddBool(TEXT("Setter"), true);
			}
			return;
		}
		if (const URigVMFunctionReferenceNode* Reference = Cast<URigVMFunctionReferenceNode>(&Node))
		{
			Entry.AddString(TEXT("Function"), FunctionName(*Reference));
			const TMap<FName, FName>& Map = Reference->GetVariableMap();
			if (!Map.IsEmpty())
			{
				TArray<FName> Inner;
				Map.GetKeys(Inner);
				Inner.Sort(FNameLexicalLess());
				FObjectBuilder VariableMap;
				bool bAnyMapped = false;
				for (const FName& Name : Inner)
				{
					// An input variable pin the reference leaves unbound is listed without an outer variable.
					if (!Map.FindChecked(Name).IsNone())
					{
						VariableMap.Add(Name.ToString(), MakeName(Map.FindChecked(Name)));
						bAnyMapped = true;
					}
				}
				if (bAnyMapped)
				{
					Entry.Add(TEXT("VariableMap"), VariableMap.Build());
				}
			}
			return;
		}
		if (const URigVMAggregateNode* Aggregate = Cast<URigVMAggregateNode>(&Node))
		{
			if (const URigVMTemplateNode* Inner = Cast<URigVMTemplateNode>(Aggregate->GetFirstInnerNode()))
			{
				AddTemplateKind(*Inner, Entry);
			}
			else
			{
				Entry.AddString(TEXT("Unknown"), PathOf(Node.GetClass()));
			}
			TArray<FString> Names;
			for (const URigVMPin* Pin : Aggregate->IsInputAggregate() ? Aggregate->GetAggregateInputs() : Aggregate->GetAggregateOutputs())
			{
				Names.Add(Pin->GetName());
			}
			Entry.Add(TEXT("Aggregate"), MakeStringArray(Names));
			return;
		}
		if (const URigVMCollapseNode* Collapse = Cast<URigVMCollapseNode>(&Node))
		{
			FObjectBuilder Interface;
			AddHeader(*Collapse, Interface);
			AddInterface(*Collapse, Interface, false);
			if (const URigVMGraph* Contained = Collapse->GetContainedGraph())
			{
				AddLocals(*Contained, Interface);
			}
			Entry.Add(TEXT("Collapse"), Interface.Build());
			bOutCollapse = Collapse->GetContainedGraph() != nullptr;
			return;
		}
		if (const URigVMTemplateNode* Template = Cast<URigVMTemplateNode>(&Node))
		{
			AddTemplateKind(*Template, Entry);
			return;
		}
		Entry.AddString(TEXT("Unknown"), PathOf(Node.GetClass()));
	}

	// Values of the input pins that differ from the node's defaults (sparse). A pin with linked or bound sub-pins is
	// written per sub-pin; for an array pin the last element is always written (unless it is linked or bound), so the
	// document carries the array's size. True when Pin wrote an entry.
	bool CollectDefaults(const URigVMPin& Pin, FObjectBuilder& Defaults, bool& bAny)
	{
		const ERigVMPinDirection Direction = Pin.GetDirection();
		if ((Direction != ERigVMPinDirection::Input && Direction != ERigVMPinDirection::IO && Direction != ERigVMPinDirection::Visible)
			|| Pin.IsExecuteContext() || Pin.IsWildCard() || IsValuelessType(Pin.GetCPPType(), Pin.GetCPPTypeObject())
			|| Pin.IsBoundToVariable() || !Pin.GetSourceLinks(false).IsEmpty() || IsInputVariablePin(Pin))
		{
			return false;
		}
		if (!Pin.GetSourceLinks(true).IsEmpty() || HasBoundSubPin(Pin))
		{
			const TArray<URigVMPin*>& SubPins = Pin.GetSubPins();
			bool bWrote = false;
			bool bLastWritten = false;
			for (int32 Index = 0; Index < SubPins.Num(); ++Index)
			{
				const bool bSub = CollectDefaults(*SubPins[Index], Defaults, bAny);
				bWrote |= bSub;
				bLastWritten = bSub;
			}
			if (Pin.IsArray() && !SubPins.IsEmpty() && !bLastWritten)
			{
				const URigVMPin& Last = *SubPins.Last();
				if (Last.GetSourceLinks(true).IsEmpty() && !Last.IsBoundToVariable() && !HasBoundSubPin(Last))
				{
					Defaults.Add(Last.GetSegmentPath(true), EncodeText(Last, Last.GetDefaultValue()));
					bAny = true;
					bWrote = true;
				}
			}
			return bWrote;
		}
		const FString Current = Pin.GetDefaultValue();
		const FString Original = Pin.GetOriginalDefaultValue();
		// A pin without a text and one holding its type's default read as the same value but do not compile the same
		// (a variable setter's empty Value is not a False literal): a text where a new node has none is listed (not on
		// a function's Entry / Return, whose pins the interface edits give a text).
		const bool bTextWhereNoneIsNew = Original.IsEmpty() && !Current.IsEmpty() && !Pin.GetNode()->IsA<URigVMFunctionInterfaceNode>();
		if (!bTextWhereNoneIsNew && Values.Same(Pin.GetCPPType(), Pin.GetCPPTypeObject(), Current, Original))
		{
			return false;
		}
		Defaults.Add(Pin.GetSegmentPath(true), EncodeText(Pin, Current));
		bAny = true;
		return true;
	}

	static void CollectExpanded(const URigVMPin& Pin, TArray<FString>& Out)
	{
		// A pin linked as a whole shows no sub-pins; the expansion it kept from before the link (with the array elements
		// of its last value) is not the node's content.
		if (Pin.GetDirection() != ERigVMPinDirection::Output && !Pin.GetSourceLinks(false).IsEmpty())
		{
			return;
		}
		if (Pin.IsExpanded() && !Pin.GetSubPins().IsEmpty())
		{
			Out.Add(Pin.GetSegmentPath(true));
		}
		for (const URigVMPin* SubPin : Pin.GetSubPins())
		{
			CollectExpanded(*SubPin, Out);
		}
	}

	// Units injected into the node's pins (the pin menu's Add Visual Debug / Add Interpolate, AddInjectedNode), per
	// root pin in injection order: {Unit, Method?, Input, Output, Name, Defaults?}.
	void AddInjections(const URigVMNode& Node, FObjectBuilder& Entry)
	{
		FObjectBuilder Injections;
		bool bAny = false;
		for (const URigVMPin* Pin : Node.GetPins())
		{
			TArray<FValue> List;
			for (const URigVMInjectionInfo* Injection : Pin->GetInjectedNodes())
			{
				URigVMUnitNode* Unit = Injection != nullptr ? Cast<URigVMUnitNode>(Injection->Node) : nullptr;
				if (Unit == nullptr || Injection->OutputPin == nullptr)
				{
					continue;
				}
				FObjectBuilder Spec;
				bool bCollapse = false;
				// An injected unit's types stay complete (their full map would land on the host node's record).
				bSparseTypes = false;
				AddKind(*Unit, Spec, bCollapse);
				bSparseTypes = true;
				Spec.AddString(TEXT("Name"), Unit->GetName());
				Spec.AddString(TEXT("Input"), Injection->InputPin != nullptr ? Injection->InputPin->GetName() : FString());
				Spec.AddString(TEXT("Output"), Injection->OutputPin->GetName());
				FObjectBuilder Defaults;
				bool bDefaults = false;
				for (const URigVMPin* UnitPin : Unit->GetPins())
				{
					CollectDefaults(*UnitPin, Defaults, bDefaults);
				}
				if (bDefaults)
				{
					Spec.Add(TEXT("Defaults"), Defaults.Build());
				}
				List.Add(Spec.Build());
			}
			if (!List.IsEmpty())
			{
				Injections.Add(Pin->GetName(), MakeArray(MoveTemp(List)));
				bAny = true;
			}
		}
		if (bAny)
		{
			Entry.Add(TEXT("Injections"), Injections.Build());
		}
	}

	// Defaults, Inputs (links into the node's pins from other source nodes), Bindings and Expanded of a node.
	void AddPinState(const URigVMNode& Node, FObjectBuilder& Entry)
	{
		FObjectBuilder Defaults;
		bool bDefaults = false;
		FObjectBuilder Inputs;
		bool bInputs = false;
		FObjectBuilder Bindings;
		bool bBindings = false;
		TArray<FString> Expanded;
		for (const URigVMPin* Pin : Node.GetPins())
		{
			CollectDefaults(*Pin, Defaults, bDefaults);
			CollectExpanded(*Pin, Expanded);
		}
		for (const URigVMPin* Pin : Node.GetAllPinsRecursively())
		{
			if (Pin->IsBoundToVariable() && !IsInputVariablePin(*Pin))
			{
				Bindings.AddString(Pin->GetSegmentPath(true), Pin->GetBoundVariablePath());
				bBindings = true;
			}
			// A link into a pin with injected units ends on the last one's input pin, and one from an output pin with
			// injected units starts on the last one's output pin: the editor draws (and the document writes) them on the
			// host pins.
			const URigVMPin* LinkPin = Pin->GetDirection() == ERigVMPinDirection::Output ? Pin : Pin->GetPinForLink();
			for (const URigVMLink* Link : LinkPin != nullptr ? LinkPin->GetSourceLinks(false) : TArray<URigVMLink*>())
			{
				const URigVMPin* Source = Link->GetSourcePin();
				Source = Source != nullptr ? Source->GetOriginalPinFromInjectedNode() : nullptr;
				if (Source == nullptr || !IsSourceNode(Source->GetNode()))
				{
					continue;
				}
				Inputs.AddString(Pin->GetSegmentPath(true), Source->GetNode()->GetName() + TEXT(".") + Source->GetSegmentPath(true));
				bInputs = true;
			}
		}
		if (bDefaults)
		{
			Entry.Add(TEXT("Defaults"), Defaults.Build());
		}
		if (bInputs)
		{
			Entry.Add(TEXT("Inputs"), Inputs.Build());
		}
		if (bBindings)
		{
			Entry.Add(TEXT("Bindings"), Bindings.Build());
		}
		AddInjections(Node, Entry);
		if (!Expanded.IsEmpty())
		{
			// A set of pins, sorted: a node made today can hold its pins in another order than an older one.
			Expanded.Sort();
			Entry.Add(TEXT("Expanded"), MakeStringArray(Expanded));
		}
	}

	bool ReadNode(URigVMNode& Node, const TArray<FString>& Scope)
	{
		FObjectBuilder Entry;
		bool bCollapse = false;
		bPendingFullTypes = false;
		AddKind(Node, Entry, bCollapse);
		Entry.Add(TEXT("Position"), Pair(Node.GetPosition()));

		if (const URigVMCommentNode* Comment = Cast<URigVMCommentNode>(&Node))
		{
			Entry.Add(TEXT("Size"), Pair(Node.GetSize()));
			if (Comment->GetCommentFontSize() != 18)
			{
				Entry.AddInt(TEXT("FontSize"), Comment->GetCommentFontSize());
			}
			if (Comment->GetCommentBubbleVisible())
			{
				Entry.AddBool(TEXT("Bubble"), true);
			}
			if (Comment->GetCommentColorBubble())
			{
				Entry.AddBool(TEXT("ColorBubble"), true);
			}
			Entry.Add(TEXT("Color"), Color(Node.GetNodeColor()));
		}
		else
		{
			// Units store their display name as NodeTitle when added; only renamable nodes (URigVMSchema::
			// CanSetNodeTitle: Constant, Make Struct) carry an authored title.
			if (Node.SupportsRenaming() && !Node.GetNodeTitleRaw().IsEmpty())
			{
				Entry.AddString(TEXT("Title"), Node.GetNodeTitleRaw());
			}
			FLinearColor UserColor;
			if (GetUserColor(Node, UserColor))
			{
				Entry.Add(TEXT("Color"), Color(UserColor));
			}
		}

		// Traits: trait pins with their struct.
		FObjectBuilder Traits;
		bool bTraits = false;
		for (const URigVMPin* TraitPin : Node.GetTraitPins())
		{
			FObjectBuilder Trait;
			Trait.AddString(TEXT("Type"), PathOf(Node.GetTraitScriptStruct(TraitPin)));
			Traits.Add(TraitPin->GetName(), Trait.Build());
			bTraits = true;
		}
		if (bTraits)
		{
			Entry.Add(TEXT("Traits"), Traits.Build());
		}

		AddPinState(Node, Entry);

		FObjectBuilder Record;
		Record.AddString(TEXT("kind"), TEXT("node"));
		Record.Add(TEXT("scope"), MakeStringArray(Scope));
		Record.AddString(TEXT("name"), Node.GetName());
		Record.Add(TEXT("entry"), Entry.Build());
		if (bPendingFullTypes)
		{
			Record.Add(TEXT("pin_types"), MoveTemp(PendingFullTypes));
			bPendingFullTypes = false;
		}
		if (bInspect)
		{
			TArray<FValue> Pins;
			for (const URigVMPin* Pin : Node.GetPins())
			{
				FObjectBuilder Description;
				Description.AddString(TEXT("Name"), Pin->GetName());
				Description.AddString(TEXT("Type"), TypeName(Pin->GetCPPType(), Pin->GetCPPTypeObject()));
				Description.AddString(TEXT("Direction"), DirectionName(static_cast<uint8>(Pin->GetDirection())));
				if (Pin->IsWildCard())
				{
					Description.AddBool(TEXT("Wildcard"), true);
				}
				if (Pin->IsTraitPin())
				{
					Description.AddBool(TEXT("Trait"), true);
				}
				Pins.Add(Description.Build());
			}
			Record.Add(TEXT("pins"), MakeArray(MoveTemp(Pins)));
		}
		Records.Add(Record.Build());

		if (bCollapse)
		{
			const URigVMCollapseNode& Collapse = static_cast<const URigVMCollapseNode&>(Node);
			TArray<FString> Inner = Scope;
			Inner.Add(TEXT("Nodes"));
			Inner.Add(Node.GetName());
			Inner.Add(TEXT("Collapse"));
			return ReadGraph(*Collapse.GetContainedGraph(), Inner);
		}
		return true;
	}
};
} // namespace

FString GraphName(const URigVMGraph& Model, const URigVMGraph* DefaultModel)
{
	static const TCHAR* const DefaultModelName = TEXT("RigVMModel");
	static const TCHAR* const ModelPrefix = TEXT("RigVMModel ");
	FString Name = Model.GetName();
	if (&Model == DefaultModel || Name == DefaultModelName)
	{
		return TEXT("Rig");
	}
	if (Name.StartsWith(ModelPrefix, ESearchCase::CaseSensitive))
	{
		Name.RightChopInline(FCString::Strlen(ModelPrefix));
	}
	return Name;
}

bool BuildGraphRecords(UControlRigBlueprint& Blueprint, bool bInspect, FTextValues& Values, FCall& Call, TArray<FValue>& Records)
{
	using namespace ControlRigGraphReadLocal;
	FGraphReader Reader(Blueprint, bInspect, Values, Records);
	return Reader.Read();
}
} // namespace UE::AssetDocument::Graph::Rig::ControlRig
