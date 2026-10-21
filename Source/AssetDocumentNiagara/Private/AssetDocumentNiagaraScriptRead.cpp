// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Script graph rows (module / function / dynamic input assets and scratch pads, design §3.5) and parameter
// collection rows. Every script row carries "scope": "" for the asset's own script, "<emitter>/<scratch pad>"
// or "/<scratch pad>" (system level) for scratch pads.
//   script       {scope, usage, settings, versions?}        settings: sparse FVersionedNiagaraScriptData of the
//                exposed version; versions (versioned scripts only): [{Number, Guid, Exposed?, ChangeDescription?, Deprecated?, DeprecationMessage?, Visible?}]
//   parameter    {scope, name, type, default?, default_mode?, default_binding?, static_switch_default?, meta}
//   node         {scope, id, action, x, y, version?, comment?, properties?, pins?}   (custom HLSL text: properties.CustomHlsl)
//   link         {scope, from "Id:Pin", to "Id:Pin"}
//   pin_default  {scope, pin "Id:Pin", value}
// The script asset is read at its exposed version (as the script editor opens it). Node ids are object
// names (as Material expressions); rows are sorted so a duplicate or a reload reads identically.
// Parameter collections: parameter {name (friendly), type, value? | data_interface? | object?}.

#include "AssetDocumentNiagaraPrimitives.h"
#include "AssetDocumentNiagaraDynamicPins.h"

#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_Niagara.h"
#include "NiagaraDataInterface.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeAssignment.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOp.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraNodeReadDataSet.h"
#include "NiagaraNodeWriteDataSet.h"
#include "NiagaraParameterCollection.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraVariableMetaData.h"

namespace UE::AssetDocument::Niagara
{
namespace
{
const FName DynamicAddPinSubCategory(TEXT("DynamicAddPin"));

// Node classes private to NiagaraEditor, identified by class name (design §3.5 action keys).
const TMap<FString, FString>& PrivateNodeActions()
{
	static const TMap<FString, FString> Actions = {
		{TEXT("NiagaraNodeParameterMapGet"), TEXT("Niagara:MapGet")},
		{TEXT("NiagaraNodeParameterMapSet"), TEXT("Niagara:MapSet")},
		{TEXT("NiagaraNodeParameterMapFor"), TEXT("Niagara:MapFor")},
		{TEXT("NiagaraNodeParameterMapForWithContinue"), TEXT("Niagara:MapForWithContinue")},
		{TEXT("NiagaraNodeParameterMapForIndex"), TEXT("Niagara:MapForIndex")},
		{TEXT("NiagaraNodeStaticSwitch"), TEXT("Niagara:StaticSwitch")},
		{TEXT("NiagaraNodeIf"), TEXT("Niagara:If")},
		{TEXT("NiagaraNodeSelect"), TEXT("Niagara:Select")},
		{TEXT("NiagaraNodeUsageSelector"), TEXT("Niagara:UsageSelector")},
		{TEXT("NiagaraNodeSimTargetSelector"), TEXT("Niagara:SimTargetSelector")},
		{TEXT("NiagaraNodeConvert"), TEXT("Niagara:Convert")},
		{TEXT("NiagaraNodeReroute"), TEXT("Niagara:Reroute")},
		{TEXT("NiagaraNodeOutputTag"), TEXT("Niagara:OutputTag")},
	};
	return Actions;
}

FString ScriptPath(const UNiagaraScript* Script)
{
	if (Script == nullptr)
	{
		return FString();
	}
	return Script->IsAsset() ? Script->GetPathName() : FString(TEXT("ScratchPad:")) + Script->GetName();
}

FString PinRef(const UEdGraphPin& Pin)
{
	// A Map Get default value pin has no name: it is named after its output pin (direction input).
	const UEdGraphNode& Node = *Pin.GetOwningNode();
	const UEdGraphPin* Output = Pin.PinName.IsNone() ? MapGetOutputForDefault(Node, Pin) : nullptr;
	return Node.GetName() + TEXT(":") + (Output != nullptr ? Output->PinName : Pin.PinName).ToString();
}

// A Map Get default value pin whose value is the parameter's Default (read only) or that is hidden (default mode
// Binding / FailIfPreviouslyNotSet, engine parameters): derived, not a pin default of the graph.
bool IsDerivedDefaultPin(const UEdGraphPin& Pin)
{
	return Pin.PinName.IsNone() && MapGetOutputForDefault(*Pin.GetOwningNode(), Pin) != nullptr
		&& (Pin.bDefaultValueIsReadOnly || Pin.bHidden);
}

} // namespace

const TArray<FName>& NodePropertySkip()
{
	static const TArray<FName> Skip = {TEXT("NodePosX"), TEXT("NodePosY"), TEXT("NodeWidth"), TEXT("NodeHeight"),
		TEXT("FunctionScript"), TEXT("SelectedScriptVersion"), TEXT("OpName"), TEXT("NodeComment")};
	return Skip;
}

FString NodeActionKey(const UEdGraphNode& Node)
{
	if (Cast<UNiagaraNodeAssignment>(&Node) != nullptr)
	{
		return TEXT("Niagara:Assignment");
	}
	if (Cast<UNiagaraNodeCustomHlsl>(&Node) != nullptr)
	{
		return TEXT("Niagara:CustomHlsl");
	}
	if (const UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(&Node))
	{
		if (Call->FunctionScript != nullptr)
		{
			return FString(TEXT("Niagara:Function:")) + ScriptPath(Call->FunctionScript);
		}
		// A data interface's member function (the menu of a data interface pin): the class and the function.
		const FNiagaraFunctionSignature& Signature = Call->Signature;
		if (Signature.bMemberFunction && Signature.Inputs.Num() > 0 && Signature.Inputs[0].GetType().IsDataInterface()
			&& Signature.Inputs[0].GetType().GetClass() != nullptr)
		{
			return FString::Printf(TEXT("Niagara:DataInterfaceFunction:%s:%s"), *Signature.Inputs[0].GetType().GetClass()->GetPathName(),
				*Signature.Name.ToString());
		}
		return FString(TEXT("Niagara:Function:Signature:")) + Signature.Name.ToString();
	}
	if (const UNiagaraNodeOp* Op = Cast<UNiagaraNodeOp>(&Node))
	{
		return FString(TEXT("Niagara:Op:")) + Op->OpName.ToString();
	}
	if (const UNiagaraNodeInput* Input = Cast<UNiagaraNodeInput>(&Node))
	{
		return FString(TEXT("Niagara:Input:")) + Input->Input.GetName().ToString();
	}
	if (Cast<UNiagaraNodeOutput>(&Node) != nullptr)
	{
		return TEXT("Niagara:Output");
	}
	if (Cast<UNiagaraNodeReadDataSet>(&Node) != nullptr)
	{
		return TEXT("Niagara:ReadDataSet");
	}
	if (Cast<UNiagaraNodeWriteDataSet>(&Node) != nullptr)
	{
		return TEXT("Niagara:WriteDataSet");
	}
	if (Cast<UEdGraphNode_Comment>(&Node) != nullptr)
	{
		return TEXT("Niagara:Comment");
	}
	if (const FString* Action = PrivateNodeActions().Find(Node.GetClass()->GetName()))
	{
		return *Action;
	}
	return FString(TEXT("Class:")) + Node.GetClass()->GetPathName();
}

bool IsDynamicAddPin(const UEdGraphPin& Pin)
{
	return Pin.PinType.PinSubCategory == DynamicAddPinSubCategory;
}

bool HasDynamicAddPin(const UEdGraphNode& Node)
{
	for (const UEdGraphPin* Pin : Node.Pins)
	{
		if (Pin != nullptr && IsDynamicAddPin(*Pin))
		{
			return true;
		}
	}
	return false;
}

namespace NiagaraScriptReadLocal
{
// The node a new node of Node's kind starts as (the Details NodeDetails reports are the differences from it): a new
// comment takes the Graph Editor settings' colour, move mode and bubble (PostPlacedNewNode); the graph menu's Static
// Switch is a new node with auto refresh on (UEdGraphSchema_Niagara::GetGraphActions); any other node is its class
// default. Transient templates are marked as garbage by the caller (bOutTransient).
const UEdGraphNode* NodeTemplate(const UEdGraphNode& Node, bool& bOutTransient)
{
	bOutTransient = false;
	if (Node.IsA<UEdGraphNode_Comment>())
	{
		UEdGraphNode_Comment* Template = NewObject<UEdGraphNode_Comment>(GetTransientPackage(), Node.GetClass(), NAME_None, RF_Transient);
		Template->PostPlacedNewNode();
		bOutTransient = true;
		return Template;
	}
	if (Node.GetClass()->GetFName() == TEXT("NiagaraNodeStaticSwitch"))
	{
		UEdGraphNode* Template = NewObject<UEdGraphNode>(GetTransientPackage(), Node.GetClass(), NAME_None, RF_Transient);
		const FStructProperty* Data = FindFProperty<FStructProperty>(Node.GetClass(), TEXT("SwitchTypeData"));
		const FBoolProperty* AutoRefresh = Data != nullptr ? FindFProperty<FBoolProperty>(Data->Struct, TEXT("bAutoRefreshEnabled")) : nullptr;
		if (AutoRefresh != nullptr)
		{
			AutoRefresh->SetPropertyValue_InContainer(Data->ContainerPtrToValuePtr<void>(Template), true);
		}
		bOutTransient = true;
		return Template;
	}
	return Cast<UEdGraphNode>(Node.GetClass()->GetDefaultObject());
}
} // namespace NiagaraScriptReadLocal

FValue NodeDetailDefaults(const UEdGraphNode& Node)
{
	using namespace NiagaraScriptReadLocal;
	bool bTransient = false;
	const UEdGraphNode* Template = NodeTemplate(Node, bTransient);
	FValue Defaults = Template != nullptr ? SparseProperties(*Node.GetClass(), Template, nullptr, NodePropertySkip()) : Graph::MakeEmptyObject();
	if (bTransient)
	{
		const_cast<UEdGraphNode*>(Template)->MarkAsGarbage();
	}
	// The facts NodeDetails adds: a new call propagates no static switch; a new switch has the class's parameter name.
	if (Node.IsA<UNiagaraNodeFunctionCall>() && !Defaults.ObjectKeys.Contains(TEXT("PropagatedStaticSwitches")))
	{
		Defaults.ObjectKeys.Add(TEXT("PropagatedStaticSwitches"));
		Defaults.Elements.Add(Graph::MakeArray({}));
	}
	if (Node.GetClass()->GetFName() == TEXT("NiagaraNodeStaticSwitch") && !Defaults.ObjectKeys.Contains(TEXT("InputParameterName")))
	{
		if (const FNameProperty* Name = FindFProperty<FNameProperty>(Node.GetClass(), TEXT("InputParameterName")))
		{
			Defaults.ObjectKeys.Add(TEXT("InputParameterName"));
			Defaults.Elements.Add(Graph::MakeString(Name->GetPropertyValue_InContainer(Node.GetClass()->GetDefaultObject()).ToString()));
		}
	}
	return Defaults;
}

TArray<FName> ScriptSettingsSkip()
{
	return {TEXT("Version"), TEXT("VersionChangeDescription"), TEXT("bDeprecated"), TEXT("DeprecationMessage")};
}

FValue NodeDetails(const UEdGraphNode& Node)
{
	using namespace NiagaraScriptReadLocal;
	bool bTransient = false;
	const UEdGraphNode* Template = NodeTemplate(Node, bTransient);
	FValue Properties = SparseProperties(*Node.GetClass(), &Node, Template, NodePropertySkip());
	if (bTransient)
	{
		const_cast<UEdGraphNode*>(Template)->MarkAsGarbage();
	}
	if (const UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(&Node); Call != nullptr && !Call->PropagatedStaticSwitchParameters.IsEmpty())
	{
		// The Details' "Propagated Static Switch Values": the called script's switches the call passes up, with
		// their override names.
		TArray<FValue> Propagated;
		for (const FNiagaraPropagatedVariable& Item : Call->PropagatedStaticSwitchParameters)
		{
			FObjectBuilder Row;
			Row.AddString(TEXT("Name"), Item.SwitchParameter.GetName().ToString());
			if (!Item.PropagatedName.IsEmpty())
			{
				Row.AddString(TEXT("PropagatedName"), Item.PropagatedName);
			}
			Propagated.Add(Row.Build());
		}
		Properties.ObjectKeys.Add(TEXT("PropagatedStaticSwitches"));
		Properties.Elements.Add(Graph::MakeArray(MoveTemp(Propagated)));
	}
	if (Node.GetClass()->GetFName() == TEXT("NiagaraNodeStaticSwitch"))
	{
		const FNameProperty* Name = FindFProperty<FNameProperty>(Node.GetClass(), TEXT("InputParameterName"));
		const UObject* Defaults = Node.GetClass()->GetDefaultObject();
		if (Name != nullptr && Name->GetPropertyValue_InContainer(&Node) != Name->GetPropertyValue_InContainer(Defaults))
		{
			Properties.ObjectKeys.Add(TEXT("InputParameterName"));
			Properties.Elements.Add(Graph::MakeString(Name->GetPropertyValue_InContainer(&Node).ToString()));
		}
	}
	return Properties;
}

TArray<TPair<FName, EEdGraphPinDirection>> NumericSignaturePins(const UEdGraphNode& Node)
{
	const FNiagaraTypeDefinition& NumericType = FNiagaraTypeDefinition::GetGenericNumericDef();
	TArray<TPair<FName, EEdGraphPinDirection>> Found;
	if (const UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(&Node))
	{
		// A call of a function with Numeric inputs (its graph's input nodes): the call's pins of those inputs.
		if (const UNiagaraGraph* Called = Call->GetCalledGraph())
		{
			TArray<UNiagaraNodeInput*> Inputs;
			Called->GetNodesOfClass(Inputs);
			for (const UNiagaraNodeInput* Input : Inputs)
			{
				if (Input != nullptr && Input->Usage == ENiagaraInputNodeUsage::Parameter && Input->Input.GetType() == NumericType)
				{
					Found.AddUnique(TPair<FName, EEdGraphPinDirection>(Input->Input.GetName(), EGPD_Input));
				}
			}
		}
		return Found;
	}
	const UNiagaraNodeOp* Op = Cast<UNiagaraNodeOp>(&Node);
	if (Op == nullptr)
	{
		return Found;
	}
	// The op's signature (FNiagaraOpInfo, not exported): the pins a new node of that op makes, per op, once.
	static TMap<FName, TArray<TPair<FName, EEdGraphPinDirection>>> NumericPinsByOp;
	if (const TArray<TPair<FName, EEdGraphPinDirection>>* Cached = NumericPinsByOp.Find(Op->OpName))
	{
		return *Cached;
	}
	UNiagaraNodeOp* Template = NewObject<UNiagaraNodeOp>(GetTransientPackage(), NAME_None, RF_Transient);
	Template->OpName = Op->OpName;
	Template->AllocateDefaultPins();
	for (const UEdGraphPin* Pin : Template->Pins)
	{
		if (Pin != nullptr && UEdGraphSchema_Niagara::PinToTypeDefinition(Pin) == NumericType)
		{
			Found.Emplace(Pin->PinName, Pin->Direction);
		}
	}
	Template->MarkAsGarbage();
	NumericPinsByOp.Add(Op->OpName, Found);
	return Found;
}

bool HasConvertedNumericPins(const UEdGraphNode& Node)
{
	for (const TPair<FName, EEdGraphPinDirection>& Item : NumericSignaturePins(Node))
	{
		const UEdGraphPin* Pin = Node.FindPin(Item.Key, Item.Value);
		if (Pin != nullptr && UEdGraphSchema_Niagara::PinToTypeDefinition(Pin) != FNiagaraTypeDefinition::GetGenericNumericDef())
		{
			return true;
		}
	}
	return false;
}

// Pins of a node with user-added pins (Map Get / Set, custom HLSL, variadic ops): {Inputs, Outputs} of
// {Name, Type} in pin order.
FValue DynamicPins(const UEdGraphNode& Node)
{
	TArray<FValue> Inputs;
	TArray<FValue> Outputs;
	const UNiagaraNodeWithDynamicPins* DynamicNode = Cast<UNiagaraNodeWithDynamicPins>(&Node);
	// A function call's pins take no menu edits but the numeric ones' type.
	const TArray<TPair<FName, EEdGraphPinDirection>> Numeric = Node.IsA<UNiagaraNodeFunctionCall>()
		? NumericSignaturePins(Node) : TArray<TPair<FName, EEdGraphPinDirection>>();
	for (const UEdGraphPin* Pin : Node.Pins)
	{
		if (Pin != nullptr && !Numeric.IsEmpty())
		{
			if (Numeric.Contains(TPair<FName, EEdGraphPinDirection>(Pin->PinName, Pin->Direction.GetValue())) && !Pin->bHidden && !Pin->bOrphanedPin)
			{
				FObjectBuilder Row;
				Row.AddString(TEXT("Name"), Pin->PinName.ToString());
				Row.AddString(TEXT("Type"), TypeToken(UEdGraphSchema_Niagara::PinToTypeDefinition(Pin)));
				(Pin->Direction == EGPD_Input ? Inputs : Outputs).Add(Row.Build());
			}
			continue;
		}
		// The pins the node's pin menu can rename, move and remove (not the Add pins, the parameter map pins
		// or a Map Get's default value pins).
		if (DynamicNode == nullptr || Pin == nullptr || Pin->bHidden || Pin->bOrphanedPin
			|| !FDynamicPinsAccess::CallCanModifyPin(*DynamicNode, Pin))
		{
			continue;
		}
		FObjectBuilder Row;
		Row.AddString(TEXT("Name"), Pin->PinName.ToString());
		Row.AddString(TEXT("Type"), TypeToken(UEdGraphSchema_Niagara::PinToTypeDefinition(Pin)));
		(Pin->Direction == EGPD_Input ? Inputs : Outputs).Add(Row.Build());
	}
	FObjectBuilder Pins;
	Pins.Add(TEXT("Inputs"), Graph::MakeArray(MoveTemp(Inputs)));
	Pins.Add(TEXT("Outputs"), Graph::MakeArray(MoveTemp(Outputs)));
	return Pins.Build();
}

FString NodeVersion(const UEdGraphNode& Node)
{
	if (const UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(&Node))
	{
		if (Call->FunctionScript != nullptr && Call->FunctionScript->IsVersioningEnabled() && Call->SelectedScriptVersion.IsValid()
			&& Call->SelectedScriptVersion != Call->FunctionScript->GetExposedVersion().VersionGuid)
		{
			return Call->SelectedScriptVersion.ToString(EGuidFormats::DigitsWithHyphensLower);
		}
	}
	return FString();
}

namespace
{
FValue ScopedRow(const TCHAR* Kind, const FString& Scope)
{
	FObjectBuilder Row;
	Row.AddString(TEXT("kind"), Kind);
	Row.AddString(TEXT("scope"), Scope);
	return Row.Build();
}

void AddField(FValue& Object, const TCHAR* Key, FValue Value)
{
	Object.ObjectKeys.Add(Key);
	Object.Elements.Add(MoveTemp(Value));
}

void ReadParameters(const UNiagaraGraph& Graph, const FString& Scope, TArray<FValue>& OutRows)
{
	TArray<const UNiagaraScriptVariable*> Variables;
	for (const TPair<FNiagaraVariable, TObjectPtr<UNiagaraScriptVariable>>& Entry : Graph.GetAllMetaData())
	{
		if (Entry.Value != nullptr)
		{
			Variables.Add(Entry.Value.Get());
		}
	}
	Variables.Sort([](const UNiagaraScriptVariable& A, const UNiagaraScriptVariable& B)
	{
		return A.Variable.GetName().LexicalLess(B.Variable.GetName());
	});
	const FNiagaraVariableMetaData DefaultMeta;
	for (const UNiagaraScriptVariable* Variable : Variables)
	{
		FValue Row = ScopedRow(TEXT("parameter"), Scope);
		AddField(Row, TEXT("name"), Graph::MakeString(Variable->Variable.GetName().ToString()));
		AddField(Row, TEXT("type"), Graph::MakeString(TypeToken(Variable->Variable.GetType())));
		if (Variable->GetIsStaticSwitch())
		{
			AddField(Row, TEXT("static_switch_default"), Graph::MakeInt(Variable->GetStaticSwitchDefaultValue()));
		}
		if (Variable->DefaultMode != ENiagaraDefaultMode::Value)
		{
			AddField(Row, TEXT("default_mode"), Graph::MakeString(StaticEnum<ENiagaraDefaultMode>()->GetNameStringByValue(
				static_cast<int64>(Variable->DefaultMode))));
			if (Variable->DefaultMode == ENiagaraDefaultMode::Binding && Variable->DefaultBinding.IsValid())
			{
				AddField(Row, TEXT("default_binding"), Graph::MakeString(Variable->DefaultBinding.GetName().ToString()));
			}
		}
		else if (Variable->GetDefaultValueVariant().GetMode() == ENiagaraVariantMode::Bytes
			&& Variable->GetDefaultValueVariant().GetNumBytes() == Variable->Variable.GetType().GetSize())
		{
			FValue Value;
			if (EncodeValue(Variable->Variable.GetType(), Variable->GetDefaultValueData(), Value))
			{
				AddField(Row, TEXT("default"), MoveTemp(Value));
			}
		}
		FValue Meta = SparseProperties(*FNiagaraVariableMetaData::StaticStruct(), &Variable->Metadata, &DefaultMeta);
		if (Meta.ObjectKeys.Num() > 0)
		{
			AddField(Row, TEXT("meta"), MoveTemp(Meta));
		}
		OutRows.Add(MoveTemp(Row));
	}
}

void ReadGraph(const UNiagaraGraph& Graph, const FString& Scope, bool bNodeDefaults, TArray<FValue>& OutRows)
{
	TArray<const UEdGraphNode*> Nodes;
	for (const UEdGraphNode* Node : Graph.Nodes)
	{
		if (Node != nullptr)
		{
			Nodes.Add(Node);
		}
	}
	Nodes.Sort([](const UEdGraphNode& A, const UEdGraphNode& B) { return A.GetName() < B.GetName(); });
	TArray<FString> Links;
	TArray<TPair<FString, FString>> Defaults;
	for (const UEdGraphNode* Node : Nodes)
	{
		FValue Row = ScopedRow(TEXT("node"), Scope);
		AddField(Row, TEXT("id"), Graph::MakeString(Node->GetName()));
		AddField(Row, TEXT("action"), Graph::MakeString(NodeActionKey(*Node)));
		AddField(Row, TEXT("x"), Graph::MakeInt(Node->NodePosX));
		AddField(Row, TEXT("y"), Graph::MakeInt(Node->NodePosY));
		const FString NodeVersionText = NodeVersion(*Node);
		if (!NodeVersionText.IsEmpty())
		{
			AddField(Row, TEXT("version"), Graph::MakeString(NodeVersionText));
		}
		if (!Node->NodeComment.IsEmpty())
		{
			AddField(Row, TEXT("comment"), Graph::MakeString(Node->NodeComment));
		}
		FValue Properties = NodeDetails(*Node);
		if (Properties.ObjectKeys.Num() > 0)
		{
			AddField(Row, TEXT("properties"), MoveTemp(Properties));
		}
		if (bNodeDefaults)
		{
			AddField(Row, TEXT("property_defaults"), NodeDetailDefaults(*Node));
		}
		if (HasPinsSetting(*Node))
		{
			AddField(Row, TEXT("pins"), DynamicPins(*Node));
		}
		OutRows.Add(MoveTemp(Row));
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin == nullptr)
			{
				continue;
			}
			if (Pin->Direction == EGPD_Output)
			{
				for (const UEdGraphPin* Linked : Pin->LinkedTo)
				{
					if (Linked != nullptr && Linked->GetOwningNodeUnchecked() != nullptr)
					{
						Links.Add(PinRef(*Pin) + TEXT("\n") + PinRef(*Linked));
					}
				}
			}
			else if (Pin->LinkedTo.IsEmpty() && !Pin->bHidden && !Pin->DefaultValue.IsEmpty()
				&& Pin->DefaultValue != GeneratedPinDefault(*Pin) && !IsDerivedDefaultPin(*Pin))
			{
				Defaults.Emplace(PinRef(*Pin), Pin->DefaultValue);
			}
		}
	}
	Links.Sort();
	for (const FString& Link : Links)
	{
		FString From;
		FString To;
		Link.Split(TEXT("\n"), &From, &To);
		FValue Row = ScopedRow(TEXT("link"), Scope);
		AddField(Row, TEXT("from"), Graph::MakeString(From));
		AddField(Row, TEXT("to"), Graph::MakeString(To));
		OutRows.Add(MoveTemp(Row));
	}
	Defaults.Sort([](const TPair<FString, FString>& A, const TPair<FString, FString>& B) { return A.Key < B.Key; });
	for (const TPair<FString, FString>& Default : Defaults)
	{
		FValue Row = ScopedRow(TEXT("pin_default"), Scope);
		AddField(Row, TEXT("pin"), Graph::MakeString(Default.Key));
		AddField(Row, TEXT("value"), Graph::MakeString(Default.Value));
		OutRows.Add(MoveTemp(Row));
	}
}
} // namespace

FValue ReadVersionList(FNiagaraVersionedObject& Versioned)
{
	const FGuid Exposed = Versioned.IsVersioningEnabled() ? Versioned.GetExposedVersion().VersionGuid : FGuid();
	TArray<FValue> Versions;
	for (const FNiagaraAssetVersion& AssetVersion : Versioned.GetAllAvailableVersions())
	{
		const TSharedPtr<FNiagaraVersionDataAccessor> Data = Versioned.GetVersionDataAccessor(AssetVersion.VersionGuid);
		FObjectBuilder VersionRow;
		VersionRow.AddString(TEXT("Number"), FString::Printf(TEXT("%d.%d"), AssetVersion.MajorVersion, AssetVersion.MinorVersion));
		VersionRow.AddString(TEXT("Guid"), AssetVersion.VersionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		if (AssetVersion.VersionGuid == Exposed)
		{
			VersionRow.AddBool(TEXT("Exposed"), true);
		}
		if (Data.IsValid() && !Data->GetVersionChangeDescription().IsEmpty())
		{
			VersionRow.AddString(TEXT("ChangeDescription"), Data->GetVersionChangeDescription().ToString());
		}
		if (Data.IsValid() && Data->IsDeprecated())
		{
			VersionRow.AddBool(TEXT("Deprecated"), true);
		}
		if (Data.IsValid() && !Data->GetDeprecationMessage().IsEmpty())
		{
			VersionRow.AddString(TEXT("DeprecationMessage"), Data->GetDeprecationMessage().ToString());
		}
		if (!AssetVersion.bIsVisibleInVersionSelector)
		{
			VersionRow.AddBool(TEXT("Visible"), false);
		}
		Versions.Add(VersionRow.Build());
	}
	return Graph::MakeArray(MoveTemp(Versions));
}

void ReadScriptRows(const UNiagaraScript& Script, const FString& Scope, const FReadOptions& Options, TArray<FValue>& OutRows)
{
	const FGuid Version = Script.IsVersioningEnabled() ? Script.GetExposedVersion().VersionGuid : FGuid();
	const FVersionedNiagaraScriptData* Data = Version.IsValid() ? Script.GetScriptData(Version) : Script.GetLatestScriptData();
	FValue Row = ScopedRow(TEXT("script"), Scope);
	AddField(Row, TEXT("usage"), Graph::MakeString(StaticEnum<ENiagaraScriptUsage>()->GetNameStringByValue(static_cast<int64>(Script.GetUsage()))));
	if (Data != nullptr)
	{
		const FVersionedNiagaraScriptData Defaults;
		AddField(Row, TEXT("settings"), SparseProperties(*FVersionedNiagaraScriptData::StaticStruct(), Data, &Defaults,
			ScriptSettingsSkip()));
	}
	if (Script.IsVersioningEnabled())
	{
		// The version menu: each version's number, guid and Details (non-default values only).
		AddField(Row, TEXT("versions"), ReadVersionList(const_cast<UNiagaraScript&>(Script)));
	}
	OutRows.Add(MoveTemp(Row));
	const UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Version.IsValid() ? Script.GetSource(Version) : Script.GetLatestSource());
	if (Source == nullptr || Source->NodeGraph == nullptr)
	{
		return;
	}
	ReadParameters(*Source->NodeGraph, Scope, OutRows);
	ReadGraph(*Source->NodeGraph, Scope, Options.bNodeDefaults, OutRows);
}

void ReadParameterCollectionRows(const UNiagaraParameterCollection& Collection, TArray<FValue>& OutRows)
{
	const UNiagaraParameterCollectionInstance* Instance = Collection.GetDefaultInstance();
	// The namespace systems link the parameters under (NPC.<Namespace>.<name>; the editor keeps it unique).
	FObjectBuilder Header;
	Header.AddString(TEXT("kind"), TEXT("collection"));
	Header.AddString(TEXT("namespace"), Collection.GetNamespace().ToString());
	OutRows.Add(Header.Build());
	TArray<FNiagaraVariable> Parameters = Collection.GetParameters();
	// The names the collection editor shows: without the collection's namespace (which is unique per asset
	// and changes when the asset is duplicated).
	Parameters.Sort([&Collection](const FNiagaraVariable& A, const FNiagaraVariable& B)
	{
		return Collection.FriendlyNameFromParameterName(A.GetName()).LexicalLess(Collection.FriendlyNameFromParameterName(B.GetName()));
	});
	for (const FNiagaraVariable& Parameter : Parameters)
	{
		FObjectBuilder Row;
		Row.AddString(TEXT("kind"), TEXT("parameter"));
		Row.AddString(TEXT("name"), Collection.FriendlyNameFromParameterName(Parameter.GetName()).ToString());
		Row.AddString(TEXT("type"), TypeToken(Parameter.GetType()));
		if (Instance != nullptr && Instance->IsExternallyDriven(Parameter))
		{
			// Owned by the source material parameter collection (the editor shows it read only).
			Row.AddBool(TEXT("material"), true);
		}
		if (Instance != nullptr)
		{
			const FNiagaraParameterStore& Store = Instance->GetParameterStore();
			const int32 Offset = Store.IndexOf(Parameter);
			if (Offset != INDEX_NONE)
			{
				if (Parameter.GetType().IsDataInterface())
				{
					if (const UNiagaraDataInterface* DataInterface = Store.GetDataInterface(Offset))
					{
						Row.Add(TEXT("data_interface"), DescribeDataInterface(*DataInterface));
					}
				}
				else if (Parameter.GetType().IsUObject())
				{
					Row.AddString(TEXT("object"), ObjectPathOrEmpty(Store.GetUObject(Offset).Get()));
				}
				else
				{
					FValue Value;
					if (EncodeValue(Parameter.GetType(), Store.GetParameterData(Offset, Parameter.GetType()), Value))
					{
						Row.Add(TEXT("value"), MoveTemp(Value));
					}
				}
			}
		}
		OutRows.Add(Row.Build());
	}
}
} // namespace UE::AssetDocument::Niagara
