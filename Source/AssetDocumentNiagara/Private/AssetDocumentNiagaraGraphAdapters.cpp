// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Niagara adapters of the shared graph primitives (provider "graph", AssetDocumentGraph):
//   schema adapter  UEdGraphSchema_Niagara graphs (script assets and scratch pads): actions are the graph's
//                   right-click menu (UEdGraphSchema_Niagara::GetGraphActions) and spawn through the menu
//                   action's own CreateNode; deletion is the script graph editor's Delete; node settings are the
//                   node Details panel, the pin menu of nodes with an Add pin and the function call version menu.
//   script host     a module / function / dynamic input asset: its graph at the exposed version, path "Graph".
//   system host     a system's scratch pad graphs: "/<scratch pad>" (system) and "<emitter>/<scratch pad>".
// Hosts never compile: fx.niagara.compile.v1 is the Compile / Apply of the script and system editors.
//
// Node identity: the node's object name (as Material expressions). graph.query extras carry
//   {object, properties (sparse Details values), pins ({Inputs, Outputs} of the pins the user added), version}
// and graph.node.property.set.v1 (container "node") takes these settings:
//   Name                  rename the node object (a new node is renamed to its document id)
//   Pins                  {Inputs?, Outputs?}: [{Name, Type?}]; the pins a direction lists, in order (drop a
//                         parameter onto the node / the Add pin, the pin menu's Remove and Move)
//   Version               a function call's script version (guid or "M.m"; "" = the exposed one): the version menu
//   Properties/<name>     a Details value of the node (PreEditChange / write / PostEditChangeProperty)

#include "AssetDocumentNiagaraPrimitives.h"
#include "AssetDocumentNiagaraDynamicPins.h"

#include "AssetDocumentGraphAdapters.h"
#include "AssetDocumentGraphValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_Niagara.h"
#include "INiagaraEditorTypeUtilities.h"
#include "Misc/Crc.h"
#include "NiagaraActions.h"
#include "NiagaraConstants.h"
#include "NiagaraDataInterface.h"
#include "NiagaraEmitter.h"
#include "NiagaraEditorModule.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOp.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraNodeWithDynamicPins.h"
#include "NiagaraScratchPadContainer.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraSystem.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UpgradeNiagaraScriptResults.h"

#define LOCTEXT_NAMESPACE "AssetDocumentNiagaraGraph"

namespace UE::AssetDocument::Niagara
{
namespace
{
using FGraphValue = FAssetDocumentRuntimeValueDto;

const FName AddPinSubCategoryName(TEXT("DynamicAddPin"));

FString Crc8(const FString& Text)
{
	return FString::Printf(TEXT("%08X"), FCrc::StrCrc32(*Text));
}

const FGraphValue* Field(const FGraphValue& Object, const TCHAR* Key)
{
	if (Object.Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return nullptr;
	}
	const int32 Index = Object.ObjectKeys.IndexOfByKey(FString(Key));
	return Index == INDEX_NONE ? nullptr : &Object.Elements[Index];
}

// ---- actions ------------------------------------------------------------------------------------------

// The key of a menu action's node template. Function script templates name their script by path (the menu
// loads it on spawn); a data interface's member functions name the interface class and the function.
FString TemplateActionKey(const UEdGraphNode& Template)
{
	if (const UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(&Template))
	{
		if (Call->FunctionScript == nullptr && !Call->FunctionScriptAssetObjectPath.IsNone()
			&& Cast<UNiagaraNodeCustomHlsl>(&Template) == nullptr)
		{
			return FString(TEXT("Niagara:Function:")) + Call->FunctionScriptAssetObjectPath.ToString();
		}
	}
	return NodeActionKey(Template);
}

struct FNiagaraGraphAction
{
	FString Key;
	TSharedPtr<FNiagaraAction_NewNode> Action;
	TSharedPtr<FEdGraphSchemaAction> Comment;
};

// The graph's right-click menu (with FromPin: the menu of a wire dragged from that pin), plus the comment box
// (the graph's "Add Comment"). Templates live in OwnerOfTemporaries, which the caller keeps alive.
void CollectActions(const UEdGraph& Graph, const UEdGraphPin* FromPin, UEdGraph* OwnerOfTemporaries, TArray<FNiagaraGraphAction>& Out)
{
	Out.Reset();
	const UEdGraphSchema_Niagara* Schema = GetDefault<UEdGraphSchema_Niagara>();
	for (const TSharedPtr<FNiagaraAction_NewNode>& Action : Schema->GetGraphActions(&Graph, FromPin, OwnerOfTemporaries))
	{
		const UEdGraphNode* Template = Action.IsValid() ? Action->WeakNodeTemplate.Get() : nullptr;
		if (Template == nullptr)
		{
			continue;
		}
		FNiagaraGraphAction& Entry = Out.AddDefaulted_GetRef();
		Entry.Key = TemplateActionKey(*Template);
		Entry.Action = Action;
	}
	FNiagaraGraphAction& Comment = Out.AddDefaulted_GetRef();
	Comment.Key = TEXT("Niagara:Comment");
	Comment.Comment = MakeShared<FNiagaraSchemaAction_NewComment>();
	TMap<FString, int32> Counts;
	for (const FNiagaraGraphAction& Entry : Out)
	{
		++Counts.FindOrAdd(Entry.Key);
	}
	for (FNiagaraGraphAction& Entry : Out)
	{
		if (Counts.FindChecked(Entry.Key) > 1 && Entry.Action.IsValid())
		{
			FString Categories = FString::Join(Entry.Action->Categories, TEXT("|"));
			Entry.Key += TEXT("#") + Crc8(Categories + TEXT("|") + Entry.Action->DisplayName.ToString());
		}
	}
	Out.StableSort([](const FNiagaraGraphAction& A, const FNiagaraGraphAction& B) { return A.Key < B.Key; });
}

// A data interface member function the menu lists for a wire dragged from a data interface pin
// ("Niagara:DataInterfaceFunction:<class path>:<function>"), built as the menu builds it.
TSharedPtr<FNiagaraAction_NewNode> MakeDataInterfaceFunctionAction(const FString& Key, UEdGraph* OwnerOfTemporaries, FAssetDocumentGraphError& OutError)
{
	FString Rest = Key;
	if (!Rest.RemoveFromStart(TEXT("Niagara:DataInterfaceFunction:")))
	{
		return nullptr;
	}
	FString ClassPath;
	FString FunctionName;
	if (!Rest.Split(TEXT(":"), &ClassPath, &FunctionName, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
	{
		return nullptr;
	}
	const UClass* Class = FindObject<UClass>(nullptr, *ClassPath);
	const UNiagaraDataInterface* DataInterface = Class != nullptr ? Cast<UNiagaraDataInterface>(Class->GetDefaultObject()) : nullptr;
	if (DataInterface == nullptr)
	{
		OutError.Set(TEXT("Graph.ActionNotFound"), FString::Printf(TEXT("%s is not a data interface class."), *ClassPath));
		return nullptr;
	}
	TArray<FNiagaraFunctionSignature> Functions;
	const_cast<UNiagaraDataInterface*>(DataInterface)->GetFunctionSignatures(Functions);
	for (const FNiagaraFunctionSignature& Signature : Functions)
	{
		if (Signature.GetNameString() != FunctionName)
		{
			continue;
		}
		if (Signature.bSoftDeprecatedFunction || Signature.bHidden)
		{
			// Kept for the nodes of older assets; the menu does not offer it.
			OutError.Set(TEXT("Niagara.DeprecatedFunction"), FString::Printf(TEXT("%s.%s is deprecated: the menu no longer offers it (existing nodes keep it)."),
				*Class->GetName(), *FunctionName));
			return nullptr;
		}
		UNiagaraNodeFunctionCall* Template = NewObject<UNiagaraNodeFunctionCall>(OwnerOfTemporaries);
		TSharedPtr<FNiagaraAction_NewNode> Action = MakeShared<FNiagaraAction_NewNode>(
			FText::FromString(FName::NameToDisplayString(Signature.GetNameString(), false)),
			TArray<FString>{Class->GetDisplayNameText().ToString()}, TOptional<FNiagaraFavoritesActionData>(),
			Signature.GetDescription(), FText::GetEmpty());
		Action->WeakNodeTemplate = Template;
		Template->Signature = Signature;
		return Action;
	}
	OutError.Set(TEXT("Graph.ActionNotFound"), FString::Printf(TEXT("%s has no function %s."), *ClassPath, *FunctionName));
	return nullptr;
}

// A new input node ("Niagara:Input:<name>") the menu does not list under that name: the node a new parameter
// makes ("Add Parameter" / the pin menu's "Promote to Parameter": FNiagaraEditorUtilities::InitializeParameterInputNode,
// a parameter map parameter as module graphs offer it), named <name>. Its Details (Input: name and type, Usage,
// ExposureOptions, CallSortPriority) follow as node settings, as the user sets them in the node's Details.
TSharedPtr<FNiagaraAction_NewNode> MakeParameterInputAction(const FString& Key, UEdGraph* OwnerOfTemporaries)
{
	FString Name = Key;
	if (!Name.RemoveFromStart(TEXT("Niagara:Input:")) || Name.IsEmpty() || Name.Contains(TEXT("#")))
	{
		return nullptr;
	}
	UNiagaraNodeInput* Template = NewObject<UNiagaraNodeInput>(OwnerOfTemporaries);
	TSharedPtr<FNiagaraAction_NewNode> Action = MakeShared<FNiagaraAction_NewNode>(FText::FromString(Name), TArray<FString>(),
		TOptional<FNiagaraFavoritesActionData>(), FText::GetEmpty(), FText::GetEmpty());
	Action->WeakNodeTemplate = Template;
	Template->Usage = ENiagaraInputNodeUsage::Parameter;
	Template->Input.SetName(FName(*Name));
	Template->Input.SetType(FNiagaraTypeDefinition::GetParameterMapDef());
	FNiagaraEditorUtilities::ResetVariableToDefaultValue(Template->Input);
	return Action;
}

// ---- dynamic pins -------------------------------------------------------------------------------------

UEdGraphPin* FindAddPin(UEdGraphNode& Node, EEdGraphPinDirection Direction)
{
	for (UEdGraphPin* Pin : Node.Pins)
	{
		if (Pin != nullptr && Pin->Direction == Direction && Pin->PinType.PinCategory == UEdGraphSchema_Niagara::PinCategoryMisc
			&& Pin->PinType.PinSubCategory == AddPinSubCategoryName)
		{
			return Pin;
		}
	}
	return nullptr;
}

TArray<UEdGraphPin*> ModifiablePins(UNiagaraNodeWithDynamicPins& Node, EEdGraphPinDirection Direction)
{
	TArray<UEdGraphPin*> Pins;
	for (UEdGraphPin* Pin : Node.Pins)
	{
		if (Pin != nullptr && Pin->Direction == Direction && !Pin->bHidden && !Pin->bOrphanedPin
			&& FDynamicPinsAccess::CallCanModifyPin(Node, Pin))
		{
			Pins.Add(Pin);
		}
	}
	return Pins;
}

// UNiagaraNodeWithDynamicPins::RequestNewTypedPin (a parameter dropped onto a Map Get / Set, a typed pin picked
// from the Add pin's menu): the Add pin becomes the typed pin, a new Add pin follows, the node names it.
UEdGraphPin* RequestNewTypedPin(UNiagaraNodeWithDynamicPins& Node, EEdGraphPinDirection Direction, const FNiagaraTypeDefinition& Type, FName Name)
{
	const FScopedTransaction Transaction(LOCTEXT("NewPinAdded", "Added new pin"));
	Node.Modify();
	UEdGraphPin* AddPin = FindAddPin(Node, Direction);
	if (AddPin == nullptr)
	{
		return nullptr;
	}
	AddPin->Modify();
	AddPin->PinType = UEdGraphSchema_Niagara::TypeDefinitionToPinType(Type);
	if (Node.IsA<UNiagaraNodeOp>())
	{
		// An op's Add pin (the editor's RequestNewTypedPin without a name): a provisional "Input <n>", which the op
		// replaces with its next letter (OnNewTypedPinAdded). A provisional name equal to that letter would collide.
		TArray<UEdGraphPin*> Existing;
		for (UEdGraphPin* Pin : Node.Pins)
		{
			if (Pin != nullptr && Pin->Direction == Direction)
			{
				Existing.Add(Pin);
			}
		}
		AddPin->PinName = FName(*FString::Printf(TEXT("%s %d"), Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"), Existing.Num()));
	}
	else
	{
		AddPin->PinName = Name;
	}
	if (FDynamicPinsAccess::CallAllowDynamicPins(Node))
	{
		Node.CreatePin(Direction, FEdGraphPinType(UEdGraphSchema_Niagara::PinCategoryMisc, AddPinSubCategoryName, nullptr,
			EPinContainerType::None, false, FEdGraphTerminalType()), TEXT("Add"));
	}
	FDynamicPinsAccess::CallOnNewTypedPinAdded(Node, AddPin);
	Node.MarkNodeRequiresSynchronization(TEXT("AssetDocument: pin added"), true);
	return AddPin;
}

// The type a pin entry names, or the graph's parameter of that name (a Map Get / Set pin is a parameter).
bool PinType(const UEdGraphNode& Node, const FString& Name, const FGraphValue* TypeValue, FNiagaraTypeDefinition& OutType)
{
	if (TypeValue != nullptr)
	{
		return TypeValue->Kind == EAssetDocumentRuntimeValueKind::String && TypeFromToken(TypeValue->StringValue, OutType);
	}
	if (const UNiagaraGraph* Graph = Cast<UNiagaraGraph>(Node.GetGraph()))
	{
		for (const TPair<FNiagaraVariable, TObjectPtr<UNiagaraScriptVariable>>& Entry : Graph->GetAllMetaData())
		{
			if (Entry.Key.GetName().ToString() == Name)
			{
				OutType = Entry.Key.GetType();
				return true;
			}
		}
	}
	for (const FNiagaraVariable& Constant : FNiagaraConstants::GetEngineConstants())
	{
		if (Constant.GetName().ToString() == Name)
		{
			OutType = Constant.GetType();
			return true;
		}
	}
	return false;
}

struct FPinEntry
{
	FString Name;
	TOptional<FNiagaraTypeDefinition> Type;
};

bool ReadPinEntries(const UEdGraphNode& Node, const FGraphValue& List, const TCHAR* Direction, TArray<FPinEntry>& Out, FAssetDocumentGraphError& OutError)
{
	if (List.Kind != EAssetDocumentRuntimeValueKind::Array)
	{
		OutError.Set(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("Pins.%s is an array."), Direction), TEXT("/value"));
		return false;
	}
	for (int32 Index = 0; Index < List.Elements.Num(); ++Index)
	{
		const FGraphValue& Item = List.Elements[Index];
		FPinEntry& Entry = Out.AddDefaulted_GetRef();
		const FGraphValue* TypeValue = nullptr;
		if (Item.Kind == EAssetDocumentRuntimeValueKind::String)
		{
			Entry.Name = Item.StringValue;
		}
		else if (const FGraphValue* Name = Field(Item, TEXT("Name")); Name != nullptr && Name->Kind == EAssetDocumentRuntimeValueKind::String)
		{
			Entry.Name = Name->StringValue;
			TypeValue = Field(Item, TEXT("Type"));
		}
		if (Entry.Name.IsEmpty())
		{
			OutError.Set(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("Pins.%s[%d] needs a Name."), Direction, Index), TEXT("/value"));
			return false;
		}
		FNiagaraTypeDefinition Type;
		if (PinType(Node, Entry.Name, TypeValue, Type))
		{
			Entry.Type = Type;
		}
		else if (TypeValue != nullptr)
		{
			OutError.Set(TEXT("Niagara.UnknownType"), FString::Printf(TEXT("Pin '%s': '%s' is not a Niagara type."), *Entry.Name,
				TypeValue->Kind == EAssetDocumentRuntimeValueKind::String ? *TypeValue->StringValue : TEXT("?")), TEXT("/value"));
			return false;
		}
	}
	return true;
}

// The pin's type as the type menu shows it: definitions that differ only in flags the menu does not show (an enum
// selector read back from its pin) are the same type.
bool SamePinType(const FNiagaraTypeDefinition& Left, const FNiagaraTypeDefinition& Right)
{
	return Left == Right || TypeToken(Left) == TypeToken(Right);
}

bool SetPins(UNiagaraNodeWithDynamicPins& Node, EEdGraphPinDirection Direction, const TArray<FPinEntry>& Wanted, FAssetDocumentGraphError& OutError)
{
	const UEdGraphSchema_Niagara* Schema = GetDefault<UEdGraphSchema_Niagara>();
	// The pin's type menu (a pin the node lets the user retype and not remove, e.g. a Select node's selector):
	// UNiagaraNode::RequestNewPinType, which may rebuild the node's pins.
	for (int32 Pass = 0; Pass < 64; ++Pass)
	{
		UEdGraphPin* Retyped = nullptr;
		// A function call's numeric pins are not in its pin menu but take Convert Numeric To.
		TArray<UEdGraphPin*> Candidates = ModifiablePins(Node, Direction);
		if (Node.IsA<UNiagaraNodeFunctionCall>() && !Node.IsA<UNiagaraNodeCustomHlsl>())
		{
			for (const TPair<FName, EEdGraphPinDirection>& Item : NumericSignaturePins(Node))
			{
				if (UEdGraphPin* Pin = Item.Value == Direction ? Node.FindPin(Item.Key, Direction) : nullptr)
				{
					Candidates.AddUnique(Pin);
				}
			}
		}
		for (UEdGraphPin* Pin : Candidates)
		{
			const FPinEntry* Entry = Wanted.FindByPredicate([Pin](const FPinEntry& Item) { return Item.Name == Pin->PinName.ToString(); });
			// The unlinked numeric pin's "Convert Numeric To..." menu (UEdGraphSchema_Niagara::ConvertNumericPinToType).
			if (Entry != nullptr && Entry->Type.IsSet() && Schema->PinToTypeDefinition(Pin) == FNiagaraTypeDefinition::GetGenericNumericDef()
				&& Entry->Type.GetValue() != FNiagaraTypeDefinition::GetGenericNumericDef() && Pin->LinkedTo.Num() == 0 && FNiagaraTypeRegistry::GetNumericTypes().Contains(Entry->Type.GetValue()))
			{
				FScopedTransaction Transaction(LOCTEXT("ChangeNumericPinType", "Change Pin Type"));
				UNiagaraNode& NiagaraNode = Node;
				if (!NiagaraNode.ConvertNumericPinToType(Pin, Entry->Type.GetValue()))
				{
					Transaction.Cancel();
				}
				else if (Schema->PinToTypeDefinition(Pin) == Entry->Type.GetValue())
				{
					Retyped = Pin;
					break;
				}
			}
			if (Entry != nullptr && Entry->Type.IsSet() && !SamePinType(Entry->Type.GetValue(), Schema->PinToTypeDefinition(Pin))
				&& !FDynamicPinsAccess::CallCanRemovePin(Node, Pin) && Node.AllowExternalPinTypeChanges(Pin)
				&& Node.AllowNiagaraTypeForPinTypeChange(Entry->Type.GetValue(), Pin))
			{
				const FScopedTransaction Transaction(LOCTEXT("ChangePinType", "Change pin type"));
				Node.RequestNewPinType(Pin, Entry->Type.GetValue());
				Retyped = Pin;
				break;
			}
		}
		if (Retyped == nullptr)
		{
			break;
		}
	}
	// The pin menu's Remove pin: pins the list leaves out, or whose type it changes.
	for (UEdGraphPin* Pin : ModifiablePins(Node, Direction))
	{
		const FPinEntry* Entry = Wanted.FindByPredicate([Pin](const FPinEntry& Item) { return Item.Name == Pin->PinName.ToString(); });
		if (Entry != nullptr && (!Entry->Type.IsSet() || SamePinType(Entry->Type.GetValue(), Schema->PinToTypeDefinition(Pin))))
		{
			continue;
		}
		if (!FDynamicPinsAccess::CallCanRemovePin(Node, Pin))
		{
			OutError.Set(TEXT("Niagara.PinNotEditable"), Entry != nullptr
				? FString::Printf(TEXT("Pin '%s' cannot be removed, nor its type changed from %s to %s."), *Pin->PinName.ToString(),
					*TypeToken(Schema->PinToTypeDefinition(Pin)), *TypeToken(Entry->Type.GetValue()))
				: FString::Printf(TEXT("Pin '%s' cannot be removed."), *Pin->PinName.ToString()), TEXT("/value"));
			return false;
		}
		const FScopedTransaction Transaction(LOCTEXT("RemovePin", "Remove pin"));
		Node.Modify();
		FDynamicPinsAccess::CallRemoveDynamicPin(Node, Pin);
	}
	// Dropping the parameter onto the node (Map Get / Set) or picking the type from the Add pin's menu, then
	// the pin's name as the user types it.
	for (const FPinEntry& Entry : Wanted)
	{
		if (ModifiablePins(Node, Direction).ContainsByPredicate([&Entry](const UEdGraphPin* Pin) { return Pin->PinName.ToString() == Entry.Name; }))
		{
			continue;
		}
		// A function call's pins are its function's: only their numeric types change (above). (A custom HLSL node is a
		// function call with an Add pin.)
		if (Node.IsA<UNiagaraNodeFunctionCall>() && !Node.IsA<UNiagaraNodeCustomHlsl>())
		{
			const UEdGraphPin* Pin = Node.FindPin(FName(*Entry.Name), Direction);
			if (Pin == nullptr || (Entry.Type.IsSet() && Schema->PinToTypeDefinition(Pin) != Entry.Type.GetValue()))
			{
				OutError.Set(TEXT("Niagara.PinNotEditable"), Pin == nullptr
					? FString::Printf(TEXT("A function call takes no added pins ('%s')."), *Entry.Name)
					: FString::Printf(TEXT("Pin '%s' is not a numeric pin that converts to %s."), *Entry.Name, *TypeToken(Entry.Type.GetValue())), TEXT("/value"));
				return false;
			}
			continue;
		}
		if (!Entry.Type.IsSet())
		{
			OutError.Set(TEXT("Niagara.PinTypeRequired"), FString::Printf(TEXT(
				"Pin '%s' needs a Type: it is not a parameter of the graph (declare it in Parameters or give {Name, Type})."), *Entry.Name), TEXT("/value"));
			return false;
		}
		// A numeric type the Add pin does not offer itself: a Numeric pin (what an op's Add pin makes), then the pin's
		// "Convert Numeric To..." menu.
		const FNiagaraTypeDefinition& Numeric = FNiagaraTypeDefinition::GetGenericNumericDef();
		const bool bViaNumeric = FindAddPin(Node, Direction) != nullptr && !Node.AllowNiagaraTypeForAddPin(Entry.Type.GetValue(), Direction)
			&& Entry.Type.GetValue() != Numeric && FNiagaraTypeRegistry::GetNumericTypes().Contains(Entry.Type.GetValue())
			&& Node.AllowNiagaraTypeForAddPin(Numeric, Direction);
		if (FindAddPin(Node, Direction) == nullptr || (!bViaNumeric && !Node.AllowNiagaraTypeForAddPin(Entry.Type.GetValue(), Direction)))
		{
			OutError.Set(TEXT("Niagara.PinNotEditable"), FString::Printf(TEXT("The node takes no %s pin of type %s."),
				Direction == EGPD_Input ? TEXT("input") : TEXT("output"), *TypeToken(Entry.Type.GetValue())), TEXT("/value"));
			return false;
		}
		UEdGraphPin* Pin = RequestNewTypedPin(Node, Direction, bViaNumeric ? Numeric : Entry.Type.GetValue(), FName(*Entry.Name));
		if (Pin == nullptr)
		{
			OutError.Set(TEXT("Niagara.PinNotEditable"), FString::Printf(TEXT("Pin '%s' could not be added."), *Entry.Name), TEXT("/value"));
			return false;
		}
		// The pin's inline name box (SGraphPin name commit): verify, commit, and leave the pending rename.
		UNiagaraNode& NiagaraNode = Node;
		if (Pin->PinName.ToString() != Entry.Name)
		{
			FText Message;
			if (!NiagaraNode.VerifyEditablePinName(FText::FromString(Entry.Name), Message, Pin))
			{
				OutError.Set(TEXT("Niagara.InvalidPinName"), FString::Printf(TEXT("Pin name '%s': %s"), *Entry.Name, *Message.ToString()), TEXT("/value"));
				return false;
			}
			NiagaraNode.CommitEditablePinName(FText::FromString(Entry.Name), Pin);
		}
		NiagaraNode.CancelEditablePinName(FText::GetEmpty(), Pin);
		if (Pin->PinName.ToString() != Entry.Name)
		{
			OutError.Set(TEXT("Niagara.InvalidPinName"), FString::Printf(TEXT("The node named the pin '%s', not '%s'."),
				*Pin->PinName.ToString(), *Entry.Name), TEXT("/value"));
			return false;
		}
		if (bViaNumeric)
		{
			FScopedTransaction Transaction(LOCTEXT("ChangeNumericPinType", "Change Pin Type"));
			if (!NiagaraNode.ConvertNumericPinToType(Pin, Entry.Type.GetValue()))
			{
				Transaction.Cancel();
			}
			const UEdGraphPin* Converted = Node.FindPin(FName(*Entry.Name), Direction);
			if (Converted == nullptr || Schema->PinToTypeDefinition(Converted) != Entry.Type.GetValue())
			{
				OutError.Set(TEXT("Niagara.PinNotEditable"), FString::Printf(TEXT("Pin '%s' could not be converted to %s."), *Entry.Name,
					*TypeToken(Entry.Type.GetValue())), TEXT("/value"));
				return false;
			}
		}
	}
	// The pin menu's Move pin up / down.
	for (int32 Slot = 0; Slot < Wanted.Num(); ++Slot)
	{
		TArray<UEdGraphPin*> Current = ModifiablePins(Node, Direction);
		const int32 From = Current.IndexOfByPredicate([&](const UEdGraphPin* Pin) { return Pin->PinName.ToString() == Wanted[Slot].Name; });
		if (From == INDEX_NONE || From == Slot || !Current.IsValidIndex(Slot))
		{
			continue;
		}
		TArray<UEdGraphPin*> SameDirection;
		for (UEdGraphPin* Pin : Node.Pins)
		{
			if (Pin != nullptr && Pin->Direction == Direction)
			{
				SameDirection.Add(Pin);
			}
		}
		const int32 Amount = SameDirection.IndexOfByKey(Current[Slot]) - SameDirection.IndexOfByKey(Current[From]);
		if (!FDynamicPinsAccess::CallCanMovePin(Node, Current[From], Amount))
		{
			OutError.Set(TEXT("Niagara.PinNotEditable"), FString::Printf(TEXT("Pin '%s' cannot be moved."), *Wanted[Slot].Name), TEXT("/value"));
			return false;
		}
		const FScopedTransaction Transaction(LOCTEXT("MovePin", "Move pin"));
		FDynamicPinsAccess::CallMoveDynamicPin(Node, Current[From], Amount);
	}
	return true;
}

// ---- node settings ------------------------------------------------------------------------------------

bool RenameNodeObject(UEdGraphNode& Node, const FString& Name, FAssetDocumentGraphError& OutError)
{
	if (Node.GetName() == Name)
	{
		return true;
	}
	FText Reason;
	if (Name.IsEmpty() || !FName::IsValidXName(Name, INVALID_OBJECTNAME_CHARACTERS, &Reason))
	{
		OutError.Set(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("'%s' is not a valid node id: %s"), *Name, *Reason.ToString()), TEXT("/value"));
		return false;
	}
	UObject* Outer = Node.GetOuter();
	if (UObject* Blocker = StaticFindObjectFast(UObject::StaticClass(), Outer, FName(*Name), EFindObjectFlags::None))
	{
		// Another node (one this apply renames or removes next, or a removed node awaiting collection) holds
		// the name: it moves to a free one first.
		Blocker->Rename(*MakeUniqueObjectName(Outer, Blocker->GetClass()).ToString(), nullptr, REN_DontCreateRedirectors);
	}
	return Node.Rename(*Name, nullptr, REN_DontCreateRedirectors);
}

// A data interface function call shows its specifiers on the node (SNiagaraGraphNodeFunctionCallWithSpecifiers, made
// when the node is placed or the graph opens): it fills them from the function's signature and edits their values.
void InitializeFunctionSpecifiers(UEdGraphNode& Node)
{
	UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(&Node);
	if (Call != nullptr && Call->FunctionScript == nullptr && Call->FunctionSpecifiers.Num() == 0 && Call->Signature.FunctionSpecifiers.Num() > 0)
	{
		Call->FunctionSpecifiers = Call->Signature.FunctionSpecifiers;
	}
}

bool SetFunctionSpecifiers(UNiagaraNodeFunctionCall& Call, FProperty& Property, const FGraphValue& Value, FAssetDocumentGraphError& OutError)
{
	InitializeFunctionSpecifiers(Call);
	TMap<FName, FName> Wanted;
	FString Code;
	if (!DecodeProperty(Property, Value, &Wanted, &Call, Code))
	{
		OutError.Set(TEXT("Niagara.InvalidPropertyValue"), FString::Printf(TEXT("'FunctionSpecifiers' does not fit the property (%s)."), *Code), TEXT("/value"));
		return false;
	}
	TArray<FName> Offered;
	Call.FunctionSpecifiers.GetKeys(Offered);
	for (const TPair<FName, FName>& Entry : Wanted)
	{
		if (!Call.FunctionSpecifiers.Contains(Entry.Key))
		{
			OutError.Set(TEXT("Niagara.UnknownProperty"), FString::Printf(TEXT("The node has no specifier '%s' (it has: %s)."), *Entry.Key.ToString(),
				*FString::JoinBy(Offered, TEXT(", "), [](const FName Key) { return Key.ToString(); })), TEXT("/value"));
			return false;
		}
	}
	if (Wanted.Num() != Call.FunctionSpecifiers.Num())
	{
		OutError.Set(TEXT("Niagara.InvalidPropertyValue"), FString::Printf(TEXT("FunctionSpecifiers lists every specifier of the node: %s."),
			*FString::JoinBy(Offered, TEXT(", "), [](const FName Key) { return Key.ToString(); })), TEXT("/value"));
		return false;
	}
	bool bChanged = false;
	for (const TPair<FName, FName>& Entry : Wanted)
	{
		if (Call.FunctionSpecifiers.FindChecked(Entry.Key) != Entry.Value)
		{
			if (!bChanged)
			{
				Call.Modify();
				bChanged = true;
			}
			// SNiagaraFunctionSpecifier::OnValueNameCommitted.
			Call.FunctionSpecifiers.Add(Entry.Key, Entry.Value);
		}
	}
	if (bChanged)
	{
		Call.MarkNodeRequiresSynchronization(TEXT("FunctionSpecifierChanged"), true);
	}
	return true;
}

// The static switch Details' parameter name box (FNiagaraStaticSwitchNodeDetails::OnParameterNameCommited ->
// UNiagaraNodeStaticSwitch::ChangeSwitchParameterName -> UNiagaraGraph::RenameStaticSwitch, not exported): the renamed
// parameter keeps the old one's Details (a copy of its script variable with a new guid) unless the new name has a
// parameter already (merged: its Details win); the node takes the name and reallocates its pins (AllocateDefaultPins);
// the old name's parameter, unused now, leaves the list with the parameters' purge, as a reload does.
bool SetStaticSwitchName(UEdGraphNode& Node, const FGraphValue& Value, FAssetDocumentGraphError& OutError)
{
	FNameProperty* Property = FindFProperty<FNameProperty>(Node.GetClass(), TEXT("InputParameterName"));
	UNiagaraNode* NiagaraNode = Cast<UNiagaraNode>(&Node);
	if (Property == nullptr || NiagaraNode == nullptr)
	{
		OutError.Set(TEXT("Niagara.UnknownProperty"), TEXT("The static switch has no parameter name."), TEXT("/property_path"));
		return false;
	}
	const FString Text = Value.Kind == EAssetDocumentRuntimeValueKind::String ? Value.StringValue.TrimStartAndEnd() : FString();
	// FNiagaraStaticSwitchNodeDetails::OnVerifyParameterNameChanged: a name, not empty.
	if (Text.IsEmpty() || Text.Len() >= NAME_SIZE)
	{
		OutError.Set(TEXT("Niagara.InvalidPropertyValue"), TEXT("InputParameterName is the switch's parameter name (a non-empty string)."), TEXT("/value"));
		return false;
	}
	const FName NewName(*Text);
	if (Property->GetPropertyValue_InContainer(&Node) == NewName)
	{
		return true;
	}
	const FScopedTransaction Transaction(LOCTEXT("SetStaticSwitchName", "Set Static Switch Name"));
	Node.Modify();
	if (UEdGraph* Graph = Node.GetGraph())
	{
		Graph->Modify();
	}
	if (UNiagaraGraph* SwitchGraph = Cast<UNiagaraGraph>(Node.GetGraph()))
	{
		const FName OldName = Property->GetPropertyValue_InContainer(&Node);
		UNiagaraGraph::FScriptVariableMap& Variables = SwitchGraph->GetAllMetaData();
		UNiagaraScriptVariable* Original = nullptr;
		FNiagaraVariable Renamed;
		for (const TPair<FNiagaraVariable, TObjectPtr<UNiagaraScriptVariable>>& Pair : Variables)
		{
			if (Original == nullptr && Pair.Key.GetName() == OldName && Pair.Value != nullptr && Pair.Value->GetIsStaticSwitch())
			{
				Original = Pair.Value;
				Renamed = FNiagaraVariable(Pair.Key.GetType(), NewName);
			}
		}
		if (Original != nullptr && !Variables.Contains(Renamed))
		{
			UNiagaraScriptVariable* Copy = DuplicateObject<UNiagaraScriptVariable>(Original, SwitchGraph);
			Copy->SetFlags(RF_Transactional);
			Copy->Metadata.CreateNewGuid();
			Copy->Variable = Renamed;
			Variables.Add(Renamed, Copy);
		}
	}
	Node.PreEditChange(Property);
	Property->SetPropertyValue_InContainer(&Node, NewName);
	// UNiagaraNodeStaticSwitch::PostEditChangeProperty reallocates the pins on any member change.
	FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
	Node.PostEditChangeProperty(Event);
	NiagaraNode->MarkNodeRequiresSynchronization(TEXT("AssetDocument: static switch renamed"), true);
	return true;
}

// UNiagaraGraph::SetMetaData (not exported): a parameter's metadata, declaring the parameter when the graph has none.
void SetGraphMetaData(UNiagaraGraph& Graph, const FNiagaraVariable& Variable, const FNiagaraVariableMetaData& MetaData)
{
	UNiagaraGraph::FScriptVariableMap& Variables = Graph.GetAllMetaData();
	if (TObjectPtr<UNiagaraScriptVariable>* Found = Variables.Find(Variable))
	{
		if (*Found != nullptr)
		{
			(*Found)->Modify();
			(*Found)->Metadata = MetaData;
			if (!(*Found)->Metadata.GetVariableGuid().IsValid())
			{
				(*Found)->Metadata.CreateNewGuid();
			}
		}
		return;
	}
	Graph.Modify();
	UNiagaraScriptVariable* Created = NewObject<UNiagaraScriptVariable>(&Graph, FName(), RF_Transactional);
	Created->Init(Variable, MetaData);
	Created->SetIsStaticSwitch(Graph.FindStaticSwitchInputs().Contains(Variable));
	Variables.Add(Variable, Created);
}

// The function call Details' "Propagated Static Switch Values" (FNiagaraFunctionCallNodeDetails): per switch of the
// called script a checkbox (propagate it to this script's caller instead of setting it here) and an override name.
// The list is complete: switches left out are not propagated.
bool SetPropagatedStaticSwitches(UNiagaraNodeFunctionCall& Call, const FGraphValue& Value, FAssetDocumentGraphError& OutError)
{
	UNiagaraGraph* CalledGraph = Call.GetCalledGraph();
	UNiagaraGraph* NodeGraph = Cast<UNiagaraGraph>(Call.GetGraph());
	if (CalledGraph == nullptr || NodeGraph == nullptr || Value.Kind != EAssetDocumentRuntimeValueKind::Array)
	{
		OutError.Set(TEXT("Niagara.InvalidPropertyValue"), TEXT("PropagatedStaticSwitches is a list of {Name, PropagatedName?} of the called script's static switches."), TEXT("/value"));
		return false;
	}
	const TArray<FNiagaraVariable> SwitchInputs = CalledGraph->FindStaticSwitchInputs();
	TArray<TPair<FNiagaraVariable, FString>> Wanted;
	for (int32 Index = 0; Index < Value.Elements.Num(); ++Index)
	{
		const FGraphValue* Name = Field(Value.Elements[Index], TEXT("Name"));
		const FGraphValue* Override = Field(Value.Elements[Index], TEXT("PropagatedName"));
		const FNiagaraVariable* Switch = Name != nullptr && Name->Kind == EAssetDocumentRuntimeValueKind::String
			? SwitchInputs.FindByPredicate([Name](const FNiagaraVariable& Item) { return Item.GetName().ToString() == Name->StringValue; }) : nullptr;
		if (Switch == nullptr || (Override != nullptr && Override->Kind != EAssetDocumentRuntimeValueKind::String))
		{
			OutError.Set(TEXT("Niagara.InvalidPropertyValue"), FString::Printf(TEXT("PropagatedStaticSwitches[%d]: the called script has no static switch '%s'."), Index,
				Name != nullptr && Name->Kind == EAssetDocumentRuntimeValueKind::String ? *Name->StringValue : TEXT("?")), FString::Printf(TEXT("/value/%d"), Index));
			return false;
		}
		Wanted.Emplace(*Switch, Override != nullptr ? Override->StringValue : FString());
	}
	const FScopedTransaction Transaction(LOCTEXT("PropagateStaticSwitches", "Propagate Static Switch Values"));
	Call.Modify();
	// Unchecked boxes.
	for (int32 Index = Call.PropagatedStaticSwitchParameters.Num() - 1; Index >= 0; --Index)
	{
		const FNiagaraVariable Switch = Call.PropagatedStaticSwitchParameters[Index].SwitchParameter;
		if (!Wanted.ContainsByPredicate([&Switch](const TPair<FNiagaraVariable, FString>& Item) { return Item.Key == Switch; }))
		{
			Call.PropagatedStaticSwitchParameters.RemoveAt(Index);
		}
	}
	for (const TPair<FNiagaraVariable, FString>& Item : Wanted)
	{
		// Checked: the switch's metadata comes from the called script (CopyMetadataFromCalledGraph).
		// FindPropagatedVariable / RemovePropagatedVariable are not exported: the same search on the list.
		FNiagaraPropagatedVariable* Propagated = Call.PropagatedStaticSwitchParameters.FindByPredicate(
			[&Item](const FNiagaraPropagatedVariable& Entry) { return Entry.SwitchParameter == Item.Key; });
		if (Propagated == nullptr)
		{
			Call.PropagatedStaticSwitchParameters.Emplace(Item.Key);
			const TOptional<FNiagaraVariableMetaData> Original = CalledGraph->GetMetaData(Item.Key);
			if (!NodeGraph->GetMetaData(Item.Key).IsSet() && Original.IsSet())
			{
				SetGraphMetaData(*NodeGraph, Item.Key, Original.GetValue());
			}
			Propagated = &Call.PropagatedStaticSwitchParameters.Last();
		}
		// The override name box (CopyMetadataForNameOverride).
		if (Propagated->PropagatedName != Item.Value)
		{
			const FNiagaraVariable OldVariable = Propagated->ToVariable();
			Propagated->PropagatedName = Item.Value;
			const TOptional<FNiagaraVariableMetaData> Old = NodeGraph->GetMetaData(OldVariable);
			if (!NodeGraph->GetMetaData(Propagated->ToVariable()).IsSet() && Old.IsSet())
			{
				SetGraphMetaData(*NodeGraph, Propagated->ToVariable(), Old.GetValue());
			}
		}
	}
	Call.RefreshFromExternalChanges();
	NodeGraph->NotifyGraphChanged();
	return true;
}

bool SetDetailsProperty(UEdGraphNode& Node, const FString& Name, const FGraphValue& Value, FAssetDocumentGraphError& OutError)
{
	if (UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(&Node); Call != nullptr && Name == TEXT("PropagatedStaticSwitches"))
	{
		return SetPropagatedStaticSwitches(*Call, Value, OutError);
	}
	if (Name == TEXT("InputParameterName") && Node.GetClass()->GetFName() == TEXT("NiagaraNodeStaticSwitch"))
	{
		return SetStaticSwitchName(Node, Value, OutError);
	}
	FProperty* Property = FindEditableProperty(*Node.GetClass(), Name);
	if (UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(&Node); Call != nullptr && Name == TEXT("FunctionSpecifiers") && Property != nullptr)
	{
		return SetFunctionSpecifiers(*Call, *Property, Value, OutError);
	}
	// The output node's Outputs (shown, not editable, in its Details) are the script's outputs: its pins. They are
	// written as the New-asset factory writes them; the node then reallocates its pins (PostEditChangeProperty).
	const bool bOutputs = Node.IsA<UNiagaraNodeOutput>() && Name == TEXT("Outputs");
	if (Property == nullptr || NodePropertySkip().Contains(Property->GetFName()) || (Property->HasAnyPropertyFlags(CPF_EditConst) && !bOutputs))
	{
		OutError.Set(TEXT("Niagara.UnknownProperty"), FString::Printf(TEXT("%s has no editable property '%s'."), *Node.GetClass()->GetName(), *Name), TEXT("/property_path"));
		return false;
	}
	void* ValuePtr = Property->ContainerPtrToValuePtr<void>(&Node);
	bool bSame = false;
	FString Code;
	if (!DecodePropertyIsSame(*Property, Value, ValuePtr, &Node, bSame, Code))
	{
		OutError.Set(TEXT("Niagara.InvalidPropertyValue"), FString::Printf(TEXT("'%s' does not fit the property (%s)."), *Name, *Code), TEXT("/value"));
		return false;
	}
	if (bSame)
	{
		return true;
	}
	Node.Modify();
	Node.PreEditChange(Property);
	TArray<FName> OldOutputs;
	if (bOutputs)
	{
		for (const FNiagaraVariable& Output : Cast<UNiagaraNodeOutput>(&Node)->Outputs)
		{
			OldOutputs.Add(Output.GetName());
		}
	}
	DecodeProperty(*Property, Value, ValuePtr, &Node, Code);
	if (bOutputs)
	{
		// An output that goes away loses its links first, as the pin's "Remove pin" (UNiagaraNodeOutput::
		// RemoveOutputPin) does: reallocating the pins would otherwise keep a linked old pin as an orphan.
		for (const FName Old : OldOutputs)
		{
			const bool bKept = Cast<UNiagaraNodeOutput>(&Node)->Outputs.ContainsByPredicate([Old](const FNiagaraVariable& Output) { return Output.GetName() == Old; });
			UEdGraphPin* Pin = bKept ? nullptr : Node.FindPin(Old, EGPD_Input);
			if (Pin != nullptr)
			{
				Pin->BreakAllPinLinks(true);
			}
		}
	}
	FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
	Node.PostEditChangeProperty(Event);
	return true;
}

bool SetVersion(UEdGraphNode& Node, const FGraphValue& Value, FAssetDocumentGraphError& OutError)
{
	UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(&Node);
	if (Call == nullptr || Call->FunctionScript == nullptr || Value.Kind != EAssetDocumentRuntimeValueKind::String)
	{
		OutError.Set(TEXT("Graph.InvalidArgument"), TEXT("Version is a string on a function call node."), TEXT("/value"));
		return false;
	}
	UNiagaraScript* Script = Call->FunctionScript;
	FGuid Version;
	if (Value.StringValue.IsEmpty())
	{
		Version = Script->IsVersioningEnabled() ? Script->GetExposedVersion().VersionGuid : FGuid();
	}
	else if (!FGuid::Parse(Value.StringValue, Version))
	{
		// "M.m": the version menu lists versions by number.
		for (const FNiagaraAssetVersion& Available : Script->GetAllAvailableVersions())
		{
			if (FString::Printf(TEXT("%d.%d"), Available.MajorVersion, Available.MinorVersion) == Value.StringValue)
			{
				Version = Available.VersionGuid;
			}
		}
	}
	if (!Value.StringValue.IsEmpty() && (!Version.IsValid() || Script->GetScriptData(Version) == nullptr))
	{
		OutError.Set(TEXT("Niagara.UnknownVersion"), FString::Printf(TEXT("%s has no version %s."), *Script->GetPathName(), *Value.StringValue), TEXT("/value"));
		return false;
	}
	if (Call->SelectedScriptVersion == Version)
	{
		return true;
	}
	// FNiagaraFunctionCallNodeDetails::SwitchToVersion (the node's Details version menu): no Python upgrade script
	// (the change is made in the graph, not in the stack), then the node refreshes its pins from that version.
	const FScopedTransaction Transaction(LOCTEXT("ChangeVersion", "Changing script version"));
	FNiagaraScriptVersionUpgradeContext UpgradeContext;
	UpgradeContext.bSkipPythonScript = true;
	Call->ChangeScriptVersion(Version, UpgradeContext);
	Call->RefreshFromExternalChanges();
	return true;
}

class FNiagaraSchemaAdapter final : public IAssetDocumentGraphSchemaAdapter
{
public:
	virtual UClass* GetSchemaClass() const override
	{
		return UEdGraphSchema_Niagara::StaticClass();
	}

	virtual FString Tag(const UEdGraph& Graph) const override
	{
		return TEXT("Niagara");
	}

	virtual bool EnumerateActions(const FAssetDocumentGraphActionQuery& Query, TArray<FAssetDocumentGraphActionRecord>& OutActions,
		FAssetDocumentGraphError& OutError) const override
	{
		if (Query.Graph == nullptr)
		{
			OutError.Set(TEXT("Graph.GraphNotFound"), TEXT("No graph."));
			return false;
		}
		UEdGraph* Temporaries = NewObject<UEdGraph>(GetTransientPackage());
		TArray<FNiagaraGraphAction> Actions;
		CollectActions(*Query.Graph, Query.FromPin, Temporaries, Actions);
		for (const FNiagaraGraphAction& Action : Actions)
		{
			FAssetDocumentGraphActionRecord& Record = OutActions.AddDefaulted_GetRef();
			Record.ActionKey = Action.Key;
			if (Action.Action.IsValid())
			{
				Record.DisplayName = Action.Action->DisplayName.ToString();
				Record.Category = FString::Join(Action.Action->Categories, TEXT("|"));
				Record.Tooltip = Action.Action->ToolTip.ToString();
				Record.NodeClassPath = Action.Action->WeakNodeTemplate->GetClass()->GetPathName();
			}
			else
			{
				Record.DisplayName = TEXT("Comment");
				Record.NodeClassPath = UEdGraphNode_Comment::StaticClass()->GetPathName();
			}
			Record.bRequiresContext = Action.Key.StartsWith(TEXT("Niagara:DataInterfaceFunction:"));
		}
		return true;
	}

	virtual bool ResolveAndSpawn(const FAssetDocumentGraphSpawnRequest& Request, FAssetDocumentGraphSpawnResult& OutResult,
		FAssetDocumentGraphError& OutError) const override
	{
		UNiagaraGraph* Graph = Cast<UNiagaraGraph>(Request.Graph);
		if (Graph == nullptr)
		{
			OutError.Set(TEXT("Graph.GraphNotFound"), TEXT("Not a Niagara graph."));
			return false;
		}
		UEdGraph* Temporaries = NewObject<UEdGraph>(GetTransientPackage());
		TArray<FNiagaraGraphAction> Actions;
		CollectActions(*Graph, Request.FromPin, Temporaries, Actions);
		const FNiagaraGraphAction* Match = Actions.FindByPredicate([&Request](const FNiagaraGraphAction& Action) { return Action.Key == Request.ActionKey; });
		if (Match == nullptr && !Request.ActionKey.Contains(TEXT("#")))
		{
			// A node's own key names the node it makes; the menu may list it under several categories
			// ("<key>#<crc>"): those entries make the same node.
			const FString Prefix = Request.ActionKey + TEXT("#");
			Match = Actions.FindByPredicate([&Prefix](const FNiagaraGraphAction& Action) { return Action.Key.StartsWith(Prefix); });
		}
		TSharedPtr<FNiagaraAction_NewNode> Action = Match != nullptr ? Match->Action : nullptr;
		if (Match == nullptr && Request.ActionKey.StartsWith(TEXT("Niagara:DataInterfaceFunction:")))
		{
			Action = MakeDataInterfaceFunctionAction(Request.ActionKey, Temporaries, OutError);
			if (!Action.IsValid())
			{
				return false;
			}
		}
		else if (Match == nullptr && Request.ActionKey.StartsWith(TEXT("Niagara:Input:")))
		{
			Action = MakeParameterInputAction(Request.ActionKey, Temporaries);
		}
		if (Match == nullptr && !Action.IsValid())
		{
			OutError.Set(TEXT("Graph.ActionNotFound"), FString::Printf(TEXT("No action has key %s in graph %s."), *Request.ActionKey, *Graph->GetName()));
			return false;
		}
		const FVector2D Location = Request.Position.Get(FVector2D::ZeroVector);
		UEdGraphNode* Node = nullptr;
		if (Action.IsValid())
		{
			// FNiagaraAction_NewNode::CreateNode asks CanAddToGraph and shows a dialog when it fails: ask first.
			const UNiagaraNode* Template = Cast<UNiagaraNode>(Action->WeakNodeTemplate.Get());
			FString Reason;
			if (Template != nullptr && !Template->CanAddToGraph(Graph, Reason))
			{
				OutError.Set(TEXT("Niagara.NodeNotAllowed"), FString::Printf(TEXT("%s cannot be added to this graph: %s"), *Request.ActionKey, *Reason));
				return false;
			}
			Node = Action->CreateNode(Graph, Request.FromPin, Location, /*bSelectNewNode=*/false);
		}
		else
		{
			// FNiagaraSchemaAction_NewComment::PerformAction without its graph editor (which it asks for the bounds of
			// the selected nodes; nothing is selected here): the same template, spawned the same way.
			UEdGraphNode_Comment* Template = NewObject<UEdGraphNode_Comment>(Temporaries);
			Template->bCommentBubbleVisible_InDetailsPanel = false;
			Template->bCommentBubbleVisible = false;
			Template->bCommentBubblePinned = false;
			Node = FNiagaraSchemaAction_NewNode::SpawnNodeFromTemplate<UEdGraphNode_Comment>(Graph, Template,
				FVector2f(static_cast<float>(Location.X), static_cast<float>(Location.Y)), /*bSelectNewNode=*/false);
		}
		if (Node == nullptr)
		{
			OutError.Set(TEXT("Graph.ActionNotFound"), FString::Printf(TEXT("Action %s did not produce a node."), *Request.ActionKey));
			return false;
		}
		InitializeFunctionSpecifiers(*Node);
		OutResult.Node = Node;
		return true;
	}

	virtual void DescribeNode(const UEdGraphNode& Node, FAssetDocumentGraphNodeDescription& Out) const override
	{
		Out.ActionKey = NodeActionKey(Node);
		FObjectBuilder Extras;
		Extras.AddString(TEXT("object"), Node.GetName());
		FValue Properties = NodeDetails(Node);
		if (Properties.ObjectKeys.Num() > 0)
		{
			Extras.Add(TEXT("properties"), MoveTemp(Properties));
		}
		if (HasPinsSetting(Node))
		{
			Extras.Add(TEXT("pins"), DynamicPins(Node));
		}
		const FString Version = NodeVersion(Node);
		if (!Version.IsEmpty())
		{
			Extras.AddString(TEXT("version"), Version);
		}
		if (IsMapGet(Node))
		{
			// The unnamed default value input pins, in pin order: the output pin each belongs to (ResolvePinAlias).
			// "custom_default_pins": the ones the user edits in the graph (the parameter's default mode is Custom); the
			// others show the parameter's Default (read only) or are hidden.
			TArray<FString> DefaultPins;
			TArray<FString> CustomPins;
			for (const UEdGraphPin* Pin : Node.Pins)
			{
				if (Pin != nullptr && Pin->Direction == EGPD_Input && Pin->PinName.IsNone())
				{
					const UEdGraphPin* Output = MapGetOutputForDefault(Node, *Pin);
					DefaultPins.Add(Output != nullptr ? Output->PinName.ToString() : FString());
					if (Output != nullptr && !Pin->bDefaultValueIsReadOnly && !Pin->bHidden)
					{
						CustomPins.Add(Output->PinName.ToString());
					}
				}
			}
			Extras.Add(TEXT("default_pins"), Graph::MakeStringArray(DefaultPins));
			Extras.Add(TEXT("custom_default_pins"), Graph::MakeStringArray(CustomPins));
		}
		// Named input pins of older assets that hold their type's default with no autogenerated default recorded:
		// generated, not authored (as Body PinDefaults reads them).
		TArray<FString> TypeDefaultPins;
		for (const UEdGraphPin* Pin : Node.Pins)
		{
			if (Pin != nullptr && Pin->Direction == EGPD_Input && !Pin->PinName.IsNone() && Pin->AutogeneratedDefaultValue.IsEmpty()
				&& !Pin->DefaultValue.IsEmpty() && Pin->DefaultValue == GeneratedPinDefault(*Pin))
			{
				TypeDefaultPins.Add(Pin->PinName.ToString());
			}
		}
		if (!TypeDefaultPins.IsEmpty())
		{
			Extras.Add(TEXT("type_default_pins"), Graph::MakeStringArray(TypeDefaultPins));
		}
		Out.Extras = Extras.Build();
	}

	virtual UEdGraphPin* ResolvePinAlias(UEdGraphNode& Node, FName Name, EEdGraphPinDirection Direction) const override
	{
		if (Direction != EGPD_Input || !IsMapGet(Node))
		{
			return nullptr;
		}
		for (UEdGraphPin* Pin : Node.Pins)
		{
			if (Pin != nullptr && Pin->Direction == EGPD_Output && Pin->PinName == Name)
			{
				return MapGetDefaultPin(Node, *Pin);
			}
		}
		return nullptr;
	}

	virtual bool RemoveNode(UObject& Asset, UEdGraphNode& Node, FAssetDocumentGraphError& OutError) const override
	{
		// FNiagaraScriptGraphViewModel::DeleteSelectedNodes.
		if (!Node.CanUserDeleteNode())
		{
			OutError.Set(TEXT("Graph.NodeNotDeletable"), FString::Printf(TEXT("Node %s (%s) cannot be deleted by the user."),
				*Node.GetName(), *Node.GetClass()->GetName()));
			return false;
		}
		if (UEdGraph* Graph = Node.GetGraph())
		{
			Graph->Modify();
		}
		Node.Modify();
		Node.DestroyNode();
		return true;
	}

	virtual bool CanAddDynamicPin(const UEdGraphNode& Node) const override
	{
		return false; // pins are named: the "Pins" node setting adds them
	}

	// UEdGraphSchema_Niagara does not implement IsPinDefaultValid: the graph's pin widgets write the typed text
	// (TrySetDefaultValue). The text must read as a value of the pin's type, as the pin widget's own parse
	// (INiagaraEditorTypeUtilities::SetValueFromPinDefaultString); a type without pin-default utilities takes the
	// text as typed.
	virtual FString ValidatePinDefault(const UEdGraphPin& Pin, const FString& Value, UObject* Object, const FText& Text) const override
	{
		if (Object != nullptr || !Text.IsEmpty())
		{
			return TEXT("A Niagara pin's default is a value (kind value).");
		}
		const FNiagaraTypeDefinition Type = UEdGraphSchema_Niagara::PinToTypeDefinition(&Pin);
		if (!Type.IsValid())
		{
			return TEXT("The pin has no Niagara type.");
		}
		const TSharedPtr<INiagaraEditorTypeUtilities, ESPMode::ThreadSafe> Utilities = FNiagaraEditorModule::Get().GetTypeUtilities(Type);
		if (!Utilities.IsValid() || !Utilities->CanHandlePinDefaults())
		{
			return FString();
		}
		FNiagaraVariable Variable(Type, Pin.PinName);
		return Utilities->SetValueFromPinDefaultString(Value, Variable) ? FString()
			: FString::Printf(TEXT("'%s' is not a %s value."), *Value, *Type.GetNameText().ToString());
	}

	virtual bool SetNodeSetting(UObject& Asset, UEdGraphNode& Node, FStringView Path, const FAssetDocumentRuntimeValueDto& Value,
		bool& bOutHandled, FAssetDocumentGraphError& OutError) const override
	{
		bOutHandled = true;
		const FString Setting(Path);
		if (Setting == TEXT("Name"))
		{
			if (Value.Kind != EAssetDocumentRuntimeValueKind::String)
			{
				OutError.Set(TEXT("Graph.InvalidArgument"), TEXT("Name is a string."), TEXT("/value"));
				return false;
			}
			return RenameNodeObject(Node, Value.StringValue, OutError);
		}
		if (Setting == TEXT("Version"))
		{
			return SetVersion(Node, Value, OutError);
		}
		if (Setting == TEXT("Pins"))
		{
			UNiagaraNodeWithDynamicPins* DynamicNode = Cast<UNiagaraNodeWithDynamicPins>(&Node);
			// The numeric pins of an op or a function call take a type (Convert Numeric To) without an Add pin.
			if (DynamicNode == nullptr || !(HasDynamicAddPin(Node) || Node.IsA<UNiagaraNodeOp>() || Node.IsA<UNiagaraNodeFunctionCall>())
				|| Value.Kind != EAssetDocumentRuntimeValueKind::Object)
			{
				OutError.Set(TEXT("Niagara.PinNotEditable"), FString::Printf(TEXT("%s has no user-added pins."), *NodeActionKey(Node)), TEXT("/value"));
				return false;
			}
			// Outputs first: some nodes make input pins for their outputs (a Select node's "<output> if <case>").
			for (const TCHAR* Direction : {TEXT("Outputs"), TEXT("Inputs")})
			{
				const FGraphValue* List = Field(Value, Direction);
				TArray<FPinEntry> Entries;
				if (List != nullptr && (!ReadPinEntries(Node, *List, Direction, Entries, OutError)
					|| !SetPins(*DynamicNode, FCString::Strcmp(Direction, TEXT("Inputs")) == 0 ? EGPD_Input : EGPD_Output, Entries, OutError)))
				{
					return false;
				}
			}
			return true;
		}
		FString Property = Setting;
		if (Property.RemoveFromStart(TEXT("Properties/")))
		{
			return SetDetailsProperty(Node, Property, Value, OutError);
		}
		bOutHandled = false;
		return true;
	}
};

// ---- hosts --------------------------------------------------------------------------------------------

void AddScriptGraph(const UNiagaraScript* Script, const FString& Path, TArray<FAssetDocumentGraphInfo>& OutGraphs)
{
	if (UNiagaraGraph* Graph = Script != nullptr ? ScriptEditGraph(*Script) : nullptr)
	{
		FAssetDocumentGraphInfo& Info = OutGraphs.AddDefaulted_GetRef();
		Info.Graph = Graph;
		Info.Path = Path;
		Info.Role = TEXT("other");
	}
}

class FNiagaraScriptHostAdapter final : public IAssetDocumentGraphHostAdapter
{
public:
	virtual UClass* GetAssetClass() const override
	{
		return UNiagaraScript::StaticClass();
	}

	virtual void EnumerateGraphs(UObject& Asset, TArray<FAssetDocumentGraphInfo>& OutGraphs) const override
	{
		AddScriptGraph(Cast<UNiagaraScript>(&Asset), TEXT("Graph"), OutGraphs);
	}

	virtual bool Finalize(UObject& Asset, FAssetDocumentGraphFinalizeResult& OutResult, FAssetDocumentGraphError& OutError) const override
	{
		return true; // fx.niagara.compile.v1 compiles (the script editor's Apply)
	}
};

class FNiagaraSystemScratchPadHostAdapter final : public IAssetDocumentGraphHostAdapter
{
public:
	virtual UClass* GetAssetClass() const override
	{
		return UNiagaraSystem::StaticClass();
	}

	virtual void EnumerateGraphs(UObject& Asset, TArray<FAssetDocumentGraphInfo>& OutGraphs) const override
	{
		UNiagaraSystem* System = Cast<UNiagaraSystem>(&Asset);
		if (System == nullptr)
		{
			return;
		}
		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
			if (Data == nullptr || Data->ScratchPads == nullptr)
			{
				continue;
			}
			for (const UNiagaraScript* Script : Data->ScratchPads->Scripts)
			{
				if (Script != nullptr)
				{
					AddScriptGraph(Script, Handle.GetName().ToString() + TEXT("/") + Script->GetName(), OutGraphs);
				}
			}
		}
		for (const UNiagaraScript* Script : System->ScratchPadScripts)
		{
			if (Script != nullptr)
			{
				AddScriptGraph(Script, FString(TEXT("/")) + Script->GetName(), OutGraphs);
			}
		}
	}

	virtual bool Finalize(UObject& Asset, FAssetDocumentGraphFinalizeResult& OutResult, FAssetDocumentGraphError& OutError) const override
	{
		return true; // fx.niagara.edit.v1 applies the scratch pads, fx.niagara.compile.v1 compiles the system
	}
};

// An emitter asset's scratch pads (its exposed version's), at "<unique emitter name>/<scratch pad>" as reads name them.
// The graph edits go to the asset's own scratch pads; fx.niagara.edit.v1 then applies them in the emitter editor's
// working copy (refresh_scratch_pad) and writes it back.
class FNiagaraEmitterScratchPadHostAdapter final : public IAssetDocumentGraphHostAdapter
{
public:
	virtual UClass* GetAssetClass() const override
	{
		return UNiagaraEmitter::StaticClass();
	}

	virtual void EnumerateGraphs(UObject& Asset, TArray<FAssetDocumentGraphInfo>& OutGraphs) const override
	{
		UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(&Asset);
		if (Emitter == nullptr)
		{
			return;
		}
		const FVersionedNiagaraEmitterData* Data = Emitter->IsVersioningEnabled() ? Emitter->GetEmitterData(Emitter->GetExposedVersion().VersionGuid)
			: Emitter->GetLatestEmitterData();
		if (Data == nullptr || Data->ScratchPads == nullptr)
		{
			return;
		}
		for (const UNiagaraScript* Script : Data->ScratchPads->Scripts)
		{
			if (Script != nullptr)
			{
				AddScriptGraph(Script, Emitter->GetUniqueEmitterName() + TEXT("/") + Script->GetName(), OutGraphs);
			}
		}
	}

	virtual bool Finalize(UObject& Asset, FAssetDocumentGraphFinalizeResult& OutResult, FAssetDocumentGraphError& OutError) const override
	{
		return true; // fx.niagara.edit.v1 applies the scratch pads, fx.niagara.compile.v1 compiles the emitter
	}
};

TSharedPtr<IAssetDocumentGraphSchemaAdapter> GSchemaAdapter;
TSharedPtr<IAssetDocumentGraphHostAdapter> GScriptHost;
TSharedPtr<IAssetDocumentGraphHostAdapter> GSystemHost;
TSharedPtr<IAssetDocumentGraphHostAdapter> GEmitterHost;
} // namespace

void RegisterNiagaraGraphAdapters()
{
	GSchemaAdapter = MakeShared<FNiagaraSchemaAdapter>();
	GScriptHost = MakeShared<FNiagaraScriptHostAdapter>();
	GSystemHost = MakeShared<FNiagaraSystemScratchPadHostAdapter>();
	GEmitterHost = MakeShared<FNiagaraEmitterScratchPadHostAdapter>();
	RegisterAssetDocumentGraphSchemaAdapter(GSchemaAdapter.ToSharedRef());
	RegisterAssetDocumentGraphHostAdapter(GScriptHost.ToSharedRef());
	RegisterAssetDocumentGraphHostAdapter(GSystemHost.ToSharedRef());
	RegisterAssetDocumentGraphHostAdapter(GEmitterHost.ToSharedRef());
}

void UnregisterNiagaraGraphAdapters()
{
	if (GSchemaAdapter.IsValid())
	{
		UnregisterAssetDocumentGraphSchemaAdapter(GSchemaAdapter.ToSharedRef());
		UnregisterAssetDocumentGraphHostAdapter(GScriptHost.ToSharedRef());
		UnregisterAssetDocumentGraphHostAdapter(GSystemHost.ToSharedRef());
		UnregisterAssetDocumentGraphHostAdapter(GEmitterHost.ToSharedRef());
	}
	GSchemaAdapter.Reset();
	GScriptHost.Reset();
	GSystemHost.Reset();
	GEmitterHost.Reset();
}
} // namespace UE::AssetDocument::Niagara

#undef LOCTEXT_NAMESPACE
