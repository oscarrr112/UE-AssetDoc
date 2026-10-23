// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Case-only member renames (provider "blueprint"). FName compares case-insensitively, so a Diff or a set
// primitive cannot see "ondied" -> "OnDied"; the editor still applies it, through the same calls it uses for
// every rename (SBlueprintPaletteItem::OnNameTextCommitted -> RenameMemberVariable / RenameTimeline;
// FBlueprintComponentDetails::OnVariableTextCommitted -> RenameComponentMemberVariable). The member stays the
// same object: variable nodes, delegate references and the timeline node follow the new name.
//
//   blueprint.member.rename.v1 {kind: "variable"|"dispatcher"|"component"|"timeline", name, new_name}
//       name and new_name differ only by case. -> {name: <the name now>}
//
// Graphs (functions, macros, event graphs) rename through graph.rename.v1 and widgets through umg.widget.rename.v1.

#include "Blueprint/AssetDocumentBlueprintMemberExtensions.h"

#include "Components/ActorComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/TimelineTemplate.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_ComponentBoundEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/Kismet2NameValidators.h"

namespace UE::AssetDocument::Graph::BlueprintRenames
{
namespace
{
USCS_Node* FindOwnComponent(UBlueprint& Blueprint, FName Name)
{
	if (Blueprint.SimpleConstructionScript == nullptr)
	{
		return nullptr;
	}
	for (USCS_Node* Node : Blueprint.SimpleConstructionScript->GetAllNodes())
	{
		if (Node != nullptr && Node->GetVariableName() == Name)
		{
			return Node;
		}
	}
	return nullptr;
}

bool HandleRename(FCall& Call, FObjectBuilder& Result)
{
	UBlueprint* Blueprint = GetMemberBlueprint(Call);
	FString Kind;
	FString NameText;
	FString NewText;
	if (Blueprint == nullptr || !Call.Args.OnlyFields({TEXT("kind"), TEXT("name"), TEXT("new_name")})
		|| !Call.Args.String(TEXT("kind"), Kind) || !Call.Args.String(TEXT("name"), NameText)
		|| !Call.Args.String(TEXT("new_name"), NewText))
	{
		return false;
	}
	if (!NameText.Equals(NewText, ESearchCase::IgnoreCase))
	{
		return Call.Args.Fail(TEXT("BlueprintMember.RenameNotCaseOnly"), FString::Printf(
			TEXT("'%s' -> '%s' is not a case-only change; a member with another name is removed and added."), *NameText, *NewText),
			TEXT("new_name"));
	}
	const FName Name(*NameText);
	const FName NewName(*NewText);
	// The editor's rename box validates with the member's own name as the existing name, so a case-only change is Ok.
	FKismetNameValidator Validator(Blueprint, Name);
	if (Validator.IsValid(NewText) != EValidatorResult::Ok)
	{
		return Call.Args.Fail(TEXT("BlueprintMember.InvalidName"), FString::Printf(TEXT("'%s' is not a valid member name."), *NewText),
			TEXT("new_name"));
	}
	// FString::operator== ignores case, so the no-change test compares case-sensitively.
	if (NameText.Equals(NewText, ESearchCase::CaseSensitive))
	{
		Result.AddString(TEXT("name"), NewText);
		return true;
	}

	FString Current;
	if (Kind == TEXT("variable") || Kind == TEXT("dispatcher"))
	{
		const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, Name);
		const bool bDelegate = Index != INDEX_NONE && Blueprint->NewVariables[Index].VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate;
		if (Index == INDEX_NONE || bDelegate != (Kind == TEXT("dispatcher")))
		{
			return Call.Args.Fail(Kind == TEXT("variable") ? TEXT("BlueprintMember.VariableNotFound") : TEXT("BlueprintMember.DispatcherNotFound"),
				FString::Printf(TEXT("No %s '%s'."), *Kind, *NameText), TEXT("name"));
		}
		// A dispatcher is a delegate variable: the same call renames its signature graph and the delegate references.
		FBlueprintEditorUtils::RenameMemberVariable(Blueprint, Name, NewName);
		const int32 After = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, NewName);
		Current = After != INDEX_NONE ? Blueprint->NewVariables[After].VarName.ToString() : FString();
	}
	else if (Kind == TEXT("component"))
	{
		USCS_Node* Node = FindOwnComponent(*Blueprint, Name);
		if (Node == nullptr)
		{
			return Call.Args.Fail(TEXT("BlueprintMember.ComponentNotFound"),
				FString::Printf(TEXT("This Blueprint has no own component '%s' (inherited components cannot be renamed)."), *NameText), TEXT("name"));
		}
		// UK2Node_ComponentBoundEvent::HandleVariableRenamed reads "new name == bound name" (always true for a
		// case-only change) as a duplicate bound event: it logs an error and keeps the old casing. Detach the
		// events from the name for the call and give them the new casing afterwards.
		TArray<UK2Node_ComponentBoundEvent*> BoundEvents;
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			for (UEdGraphNode* GraphNode : Graph->Nodes)
			{
				UK2Node_ComponentBoundEvent* Bound = Cast<UK2Node_ComponentBoundEvent>(GraphNode);
				if (Bound != nullptr && Bound->ComponentPropertyName == Name)
				{
					Bound->Modify();
					Bound->ComponentPropertyName = NAME_None;
					BoundEvents.Add(Bound);
				}
			}
		}
		FBlueprintEditorUtils::RenameComponentMemberVariable(Blueprint, Node, NewName);
		for (UK2Node_ComponentBoundEvent* Bound : BoundEvents)
		{
			Bound->ComponentPropertyName = NewName;
		}
		Current = Node->GetVariableName().ToString();
	}
	else if (Kind == TEXT("timeline"))
	{
		if (Blueprint->FindTimelineTemplateByVariableName(Name) == nullptr)
		{
			return Call.Args.Fail(TEXT("BlueprintMember.TimelineNotFound"), FString::Printf(TEXT("No timeline '%s'."), *NameText), TEXT("name"));
		}
		// RenameTimeline ends in UObject::Rename on the "<Name>_Template" object, which ignores a rename that only
		// changes case, so the editor itself leaves the template (and with it the timeline name) unchanged.
		// Going through a unique intermediate name makes the same editor function do the case change; both steps
		// update the timeline node and the variable references, and the template keeps its identity.
		const FName Intermediate = FBlueprintEditorUtils::FindUniqueKismetName(Blueprint, NameText + TEXT("_CaseRename"));
		if (FBlueprintEditorUtils::RenameTimeline(Blueprint, Name, Intermediate)
			&& FBlueprintEditorUtils::RenameTimeline(Blueprint, Intermediate, NewName))
		{
			if (const UTimelineTemplate* Template = Blueprint->FindTimelineTemplateByVariableName(NewName))
			{
				Current = Template->GetVariableName().ToString();
			}
		}
	}
	else
	{
		return Call.Args.Fail(TEXT("BlueprintMember.InvalidKind"), TEXT("kind is variable, dispatcher, component or timeline."), TEXT("kind"));
	}
	if (!Current.Equals(NewText, ESearchCase::CaseSensitive))
	{
		Call.Error.Set(TEXT("BlueprintMember.RenameFailed"), FString::Printf(
			TEXT("The editor did not rename %s '%s' to '%s' (now '%s')."), *Kind, *NameText, *NewText, *Current), TEXT("/name"));
		return false;
	}
	NotifyMemberStructural(*Blueprint);
	Result.AddString(TEXT("name"), Current);
	return true;
}
} // namespace

void AppendDescriptors(TArray<FAssetDocumentPrimitiveDescriptor>& Out)
{
	Out.Add(MakeBlueprintMemberDescriptor(TEXT("blueprint.member.rename.v1"), true,
		TEXT("FBlueprintEditorUtils::RenameMemberVariable;FBlueprintEditorUtils::RenameComponentMemberVariable;FBlueprintEditorUtils::RenameTimeline")));
}

void AppendBindings(TArray<FBinding>& Out)
{
	Out.Add({TEXT("blueprint.member.rename.v1"), &HandleRename});
}
} // namespace UE::AssetDocument::Graph::BlueprintRenames
