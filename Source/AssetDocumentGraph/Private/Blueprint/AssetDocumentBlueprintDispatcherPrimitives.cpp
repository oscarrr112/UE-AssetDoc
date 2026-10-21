// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Event dispatcher primitives (provider "blueprint"). A dispatcher is a PC_MCDelegate
// member variable plus its delegate signature graph; both are created and removed
// together the way the My Blueprint panel does (BlueprintRawEdits). The signature
// (Inputs) is edited on the signature graph's entry node through the K2 schema
// adapter's user-pin path (FBlueprintDelegateActionDetails uses the same
// FBaseBlueprintGraphActionDetails parameter editing). Replication is not offered for
// dispatchers by the editor (FBlueprintVarActionDetails::ReplicationEnabled).
//
//   blueprint.member.dispatcher.set.v1    {name, inputs?: [{name, type}], category?, tooltip?} -> {created?}
//   blueprint.member.dispatcher.remove.v1 {name}
//   query "dispatchers": [{name, guid, inputs: [{name, type}], category, tooltip}]

#include "Blueprint/AssetDocumentBlueprintMemberExtensions.h"
#include "Blueprint/AssetDocumentBlueprintMemberRawEdits.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/BlueprintEditorUtils.h"

namespace UE::AssetDocument::Graph::BlueprintDispatchers
{
namespace
{
bool IsDispatcher(const FBPVariableDescription& Variable)
{
	return Variable.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate;
}

UK2Node_FunctionEntry* SignatureEntry(UEdGraph& Graph)
{
	TArray<UK2Node_FunctionEntry*> Entries;
	Graph.GetNodesOfClass(Entries);
	return Entries.Num() > 0 ? Entries[0] : nullptr;
}

FValue EncodeInputs(UEdGraph* Graph)
{
	TArray<FValue> Inputs;
	if (UK2Node_FunctionEntry* Entry = Graph ? SignatureEntry(*Graph) : nullptr)
	{
		for (const TSharedPtr<FUserPinInfo>& Pin : Entry->UserDefinedPins)
		{
			if (Pin.IsValid())
			{
				Inputs.Add(FObjectBuilder().AddString(TEXT("name"), Pin->PinName.ToString())
					.Add(TEXT("type"), MakePinTypeValue(Pin->PinType)).Build());
			}
		}
	}
	return MakeArray(MoveTemp(Inputs));
}

bool RequireSupport(FCall& Call, UBlueprint& Blueprint)
{
	if (BlueprintRawEdits::SupportsEventDispatchers(Blueprint))
	{
		return true;
	}
	Call.Error.Set(TEXT("BlueprintMember.DispatchersUnsupported"), FString::Printf(
		TEXT("%s Blueprints have no event dispatchers (the editor offers none for interfaces, macro and function libraries)."),
		*Blueprint.GetClass()->GetName()), TEXT("/name"));
	return false;
}

bool ReadInputs(FCall& Call, const FValue& Array, TArray<FAssetDocumentGraphUserPin>& Out)
{
	TSet<FName> Seen;
	for (int32 Index = 0; Index < Array.Elements.Num(); ++Index)
	{
		FReader Reader(Array.Elements[Index], FString::Printf(TEXT("/inputs/%d"), Index), Call.Error);
		FString Name;
		const FValue* Type = nullptr;
		if (!Reader.OnlyFields({TEXT("name"), TEXT("type")}) || !Reader.String(TEXT("name"), Name) || !Reader.Object(TEXT("type"), Type))
		{
			return false;
		}
		if (Name.IsEmpty() || Seen.Contains(FName(*Name)))
		{
			return Reader.Fail(TEXT("Graph.InvalidArgument"), Name.IsEmpty() ? FString(TEXT("Input names must not be empty."))
				: FString::Printf(TEXT("Duplicate input '%s'."), *Name), TEXT("name"));
		}
		Seen.Add(FName(*Name));
		FAssetDocumentGraphUserPin& Pin = Out.AddDefaulted_GetRef();
		Pin.Name = FName(*Name);
		Pin.Direction = EGPD_Output;  // signature inputs are outputs of the entry node
		FReader TypeReader = Reader.Child(TEXT("type"), *Type);
		if (!ReadPinType(TypeReader, Pin.Type))
		{
			return false;
		}
	}
	return true;
}

bool InputsEqual(const UK2Node_FunctionEntry& Entry, TConstArrayView<FAssetDocumentGraphUserPin> Pins)
{
	if (Entry.UserDefinedPins.Num() != Pins.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < Pins.Num(); ++Index)
	{
		const TSharedPtr<FUserPinInfo>& Have = Entry.UserDefinedPins[Index];
		// FName equality ignores case; a parameter whose casing differs is a change.
		if (!Have.IsValid() || !Have->PinName.ToString().Equals(Pins[Index].Name.ToString(), ESearchCase::CaseSensitive) || !(Have->PinType == Pins[Index].Type))
		{
			return false;
		}
	}
	return true;
}

bool HandleSet(FCall& Call, FObjectBuilder& Result)
{
	UBlueprint* Blueprint = GetMemberBlueprint(Call);
	FString NameText, Category, Tooltip;
	const FValue* InputsValue = nullptr;
	if (Blueprint == nullptr || !Call.Args.OnlyFields({TEXT("name"), TEXT("inputs"), TEXT("category"), TEXT("tooltip")})
		|| !Call.Args.String(TEXT("name"), NameText) || !Call.Args.Array(TEXT("inputs"), InputsValue, false)
		|| !Call.Args.String(TEXT("category"), Category, false) || !Call.Args.String(TEXT("tooltip"), Tooltip, false)
		|| !RequireSupport(Call, *Blueprint))
	{
		return false;
	}
	if (NameText.IsEmpty())
	{
		return Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("Name must not be empty."), TEXT("name"));
	}
	const FName Name(*NameText);
	TArray<FAssetDocumentGraphUserPin> Inputs;
	if (InputsValue != nullptr && !ReadInputs(Call, *InputsValue, Inputs))
	{
		return false;
	}
	const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, Name);
	if (Index != INDEX_NONE && !IsDispatcher(Blueprint->NewVariables[Index]))
	{
		return Call.Args.Fail(TEXT("BlueprintMember.InvalidName"), FString::Printf(
			TEXT("'%s' is a variable of this Blueprint, not an event dispatcher."), *NameText), TEXT("name"));
	}
	UEdGraph* Graph = FBlueprintEditorUtils::GetDelegateSignatureGraphByName(Blueprint, Name);
	if (Index == INDEX_NONE)
	{
		if (!ValidateNewMemberName(Call, *Blueprint, Name, TEXT("name")))
		{
			return false;
		}
		Graph = BlueprintRawEdits::AddEventDispatcher(*Blueprint, Name);
		if (Graph == nullptr)
		{
			return Call.Args.Fail(TEXT("BlueprintMember.InvalidName"),
				FString::Printf(TEXT("Event dispatcher '%s' could not be added."), *NameText), TEXT("name"));
		}
		Result.AddBool(TEXT("created"), true);
	}
	UK2Node_FunctionEntry* Entry = Graph ? SignatureEntry(*Graph) : nullptr;
	if (Entry == nullptr)
	{
		Call.Error.Set(TEXT("BlueprintMember.DispatcherNotFound"), FString::Printf(
			TEXT("Event dispatcher '%s' has no signature graph."), *NameText), TEXT("/name"));
		return false;
	}
	if (InputsValue != nullptr && !InputsEqual(*Entry, Inputs))
	{
		// FBlueprintDelegateActionDetails "Inputs": the same parameter editing as a function signature.
		const TSharedPtr<IAssetDocumentGraphSchemaAdapter> K2 = FindAssetDocumentGraphSchemaAdapter(GetDefault<UEdGraphSchema_K2>());
		if (!K2.IsValid() || !K2->SetUserPins(*Entry, Inputs, Call.Error))
		{
			if (Call.Error.Code.IsEmpty())
			{
				Call.Error.Set(TEXT("Graph.UserPinsUnsupported"), TEXT("The dispatcher signature could not be changed."), TEXT("/inputs"));
			}
			else
			{
				Call.Error.Path = TEXT("/inputs");
			}
			return false;
		}
	}
	if (Call.Args.Has(TEXT("category")))
	{
		FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, Name, nullptr,
			Category.IsEmpty() ? UEdGraphSchema_K2::VR_DefaultCategory : FText::FromString(Category), /*bDontRecompile=*/true);
	}
	if (Call.Args.Has(TEXT("tooltip")))
	{
		if (Tooltip.IsEmpty())
		{
			FBlueprintEditorUtils::RemoveBlueprintVariableMetaData(Blueprint, Name, nullptr, FBlueprintMetadata::MD_Tooltip);
		}
		else
		{
			FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, Name, nullptr, FBlueprintMetadata::MD_Tooltip, Tooltip);
		}
	}
	NotifyMemberStructural(*Blueprint);
	return true;
}

bool HandleRemove(FCall& Call, FObjectBuilder& Result)
{
	UBlueprint* Blueprint = GetMemberBlueprint(Call);
	FString NameText;
	if (Blueprint == nullptr || !Call.Args.OnlyFields({TEXT("name")}) || !Call.Args.String(TEXT("name"), NameText))
	{
		return false;
	}
	const FName Name(*NameText);
	const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, Name);
	UEdGraph* Graph = FBlueprintEditorUtils::GetDelegateSignatureGraphByName(Blueprint, Name);
	if (Graph == nullptr || (Index != INDEX_NONE && !IsDispatcher(Blueprint->NewVariables[Index])))
	{
		Call.Error.Set(TEXT("BlueprintMember.DispatcherNotFound"), FString::Printf(TEXT("No event dispatcher '%s'."), *NameText), TEXT("/name"));
		return false;
	}
	BlueprintRawEdits::RemoveEventDispatcher(*Blueprint, *Graph);
	NotifyMemberStructural(*Blueprint);
	return true;
}
} // namespace

FValue Query(UBlueprint& Blueprint)
{
	TArray<FValue> Out;
	for (const FBPVariableDescription& Variable : Blueprint.NewVariables)
	{
		if (!IsDispatcher(Variable))
		{
			continue;
		}
		UEdGraph* Graph = FBlueprintEditorUtils::GetDelegateSignatureGraphByName(&Blueprint, Variable.VarName);
		Out.Add(FObjectBuilder()
			.AddString(TEXT("name"), Variable.VarName.ToString())
			.AddString(TEXT("guid"), Variable.VarGuid.ToString(EGuidFormats::Digits))
			.Add(TEXT("inputs"), EncodeInputs(Graph))
			.AddBool(TEXT("has_signature_graph"), Graph != nullptr)
			.AddString(TEXT("category"), Variable.Category.ToString())
			.AddString(TEXT("tooltip"), Variable.HasMetaData(FBlueprintMetadata::MD_Tooltip) ? Variable.GetMetaData(FBlueprintMetadata::MD_Tooltip) : FString())
			.Build());
	}
	return MakeArray(MoveTemp(Out));
}

void AppendDescriptors(TArray<FAssetDocumentPrimitiveDescriptor>& Out)
{
	Out.Add(MakeBlueprintMemberDescriptor(TEXT("blueprint.member.dispatcher.set.v1"), true,
		TEXT("FBlueprintEditorUtils::AddMemberVariable;FBlueprintEditorUtils::CreateNewGraph;UEdGraphSchema_K2::CreateFunctionGraphTerminators;FBlueprintEditorUtils::SetBlueprintVariableMetaData")));
	Out.Add(MakeBlueprintMemberDescriptor(TEXT("blueprint.member.dispatcher.remove.v1"), true,
		TEXT("FBlueprintEditorUtils::RemoveMemberVariable;FBlueprintEditorUtils::RemoveGraph")));
}

void AppendBindings(TArray<FBinding>& Out)
{
	Out.Add({TEXT("blueprint.member.dispatcher.set.v1"), &HandleSet});
	Out.Add({TEXT("blueprint.member.dispatcher.remove.v1"), &HandleRemove});
}
} // namespace UE::AssetDocument::Graph::BlueprintDispatchers
