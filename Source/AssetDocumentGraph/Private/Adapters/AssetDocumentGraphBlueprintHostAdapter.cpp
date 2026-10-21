// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Blueprint host adapter: graph enumeration/creation/removal/rename on
// UBlueprint and graph.finalize.v1 = FKismetEditorUtilities::CompileBlueprint.

#include "Adapters/AssetDocumentGraphBuiltinAdapters.h"

#include "Algo/StableSort.h"
#include "Animation/AnimBlueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphToken.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionResult.h"
#include "Misc/OutputDeviceNull.h"
#include "UObject/StructOnScope.h"
#include "Logging/TokenizedMessage.h"

namespace UE::AssetDocument::Graph
{
namespace AssetDocumentGraphBlueprintHostAdapterLocal
{
// Saving a custom event or function result (UK2Node::Serialize -> FixupPinStringDataReferences) rewrites each
// struct pin default through ImportText/ExportText and copies the pins' defaults into the user pin infos,
// marking the Blueprint structurally modified. During the apply's save that lands after the compile and leaves
// the saved Blueprint Dirty, so the same normalization runs before the compile.
void NormalizeUserPinDefaultsAsSaved(UBlueprint& Blueprint)
{
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	TArray<UK2Node_EditablePinBase*> Nodes;
	FBlueprintEditorUtils::GetAllNodesOfClass(&Blueprint, Nodes);
	for (UK2Node_EditablePinBase* Node : Nodes)
	{
		if (!Node->IsA<UK2Node_CustomEvent>() && !Node->IsA<UK2Node_FunctionResult>())
		{
			continue;
		}
		for (UEdGraphPin* Pin : Node->Pins)
		{
			UScriptStruct* Struct = Pin != nullptr && !Pin->bDefaultValueIsIgnored && !Pin->DefaultValue.IsEmpty()
				&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct && !Schema->PinHasCustomDefaultFormat(*Pin)
				? Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get()) : nullptr;
			if (Struct == nullptr)
			{
				continue;
			}
			FStructOnScope Value(Struct);
			FOutputDeviceNull Errors;
			Struct->ImportText(*Pin->DefaultValue, Value.GetStructMemory(), Node, PPF_SerializedAsImportText, &Errors, Pin->PinName.ToString());
			FString Normalized;
			Struct->ExportText(Normalized, Value.GetStructMemory(), Value.GetStructMemory(), Node, PPF_SerializedAsImportText, nullptr);
			Pin->DefaultValue = Normalized;
		}
		Node->UpdateUserDefinedPinDefaultValues();
	}
}
} // namespace AssetDocumentGraphBlueprintHostAdapterLocal

namespace
{
void AddGraph(TArray<FAssetDocumentGraphInfo>& Out, UEdGraph* Graph, const TCHAR* Role)
{
	if (Graph == nullptr)
	{
		return;
	}
	FAssetDocumentGraphInfo& Info = Out.AddDefaulted_GetRef();
	Info.Graph = Graph;
	// Contract §2: path = graph names joined by "/" from the top-level graph.
	Info.Path = Graph->GetName();
	Info.Role = Role;
	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (Schema != nullptr && Schema->GetClass()->GetName().Contains(TEXT("AnimationGraphSchema")))
	{
		Info.Role = TEXT("anim_graph");
	}
	Info.bCanSpawn = Graph->bEditable;
	Info.bCanRemove = Graph->bAllowDeletion;
	// UEdGraph: "Graph can also be renamed if bAllowDeletion is true".
	Info.bCanRename = Graph->bAllowRenaming || Graph->bAllowDeletion;
}

FString SeverityText(EMessageSeverity::Type Severity)
{
	switch (Severity)
	{
	case EMessageSeverity::Error: return TEXT("error");
	case EMessageSeverity::Warning:
	case EMessageSeverity::PerformanceWarning: return TEXT("warning");
	default: return TEXT("info");
	}
}

bool ValidateName(UBlueprint& Blueprint, FStringView Name, FName ExistingName, FAssetDocumentGraphError& OutError)
{
	FKismetNameValidator Validator(&Blueprint, ExistingName);
	const EValidatorResult Result = Validator.IsValid(FString(Name));
	if (Result == EValidatorResult::Ok || (Result == EValidatorResult::ExistingName && ExistingName == FName(Name)))
	{
		return true;
	}
	OutError.Set(TEXT("Graph.InvalidArgument"),
		INameValidatorInterface::GetErrorString(FString(Name), Result), TEXT("/name"));
	return false;
}

class FBlueprintHostAdapter final : public IAssetDocumentGraphHostAdapter
{
public:
	virtual UClass* GetAssetClass() const override
	{
		return UBlueprint::StaticClass();
	}

	// An interface's graphs in the order the editor makes them when the interface is implemented
	// (FBlueprintEditorUtils::ImplementNewInterface: the interface's functions, super included). The stored
	// order also holds the interface's history (conforming appends a function added later) and the editor
	// cannot reorder interface graphs (MoveGraphBeforeOtherGraph moves functions and macros only).
	static TArray<UEdGraph*> InterfaceGraphsInEditorOrder(const FBPInterfaceDescription& Interface)
	{
		TArray<UEdGraph*> Graphs = Interface.Graphs;
		if (Interface.Interface == nullptr)
		{
			return Graphs;
		}
		TMap<FName, int32> Rank;
		for (TFieldIterator<UFunction> It(Interface.Interface, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			Rank.FindOrAdd(It->GetFName(), Rank.Num());
		}
		Algo::StableSortBy(Graphs, [&Rank](const UEdGraph* Graph)
		{
			const int32* Found = Graph != nullptr ? Rank.Find(Graph->GetFName()) : nullptr;
			return Found != nullptr ? *Found : MAX_int32;
		});
		return Graphs;
	}

	virtual void EnumerateGraphs(UObject& Asset, TArray<FAssetDocumentGraphInfo>& OutGraphs) const override
	{
		UBlueprint& Blueprint = *CastChecked<UBlueprint>(&Asset);
		// A Blueprint of a const class (DamageType) is created without event graphs
		// (FKismetEditorUtilities::CreateBlueprint makes it BPTYPE_Const), but one saved before its class
		// became const is still a normal Blueprint with its empty event graph (DmgTypeBP_Environmental).
		const bool bEventGraphs = FBlueprintEditorUtils::DoesSupportEventGraphs(&Blueprint)
			&& !(Blueprint.ParentClass != nullptr && Blueprint.ParentClass->HasAnyClassFlags(CLASS_Const));
		for (UEdGraph* Graph : Blueprint.UbergraphPages)
		{
			// An empty event graph there holds nothing the compiler uses and the editor cannot create one:
			// it is not part of the document (neither extracted nor removed).
			if (!bEventGraphs && Graph != nullptr && Graph->Nodes.FindByPredicate([](const UEdGraphNode* Node)
				{ return Node != nullptr && !Node->IsAutomaticallyPlacedGhostNode(); }) == nullptr)
			{
				continue;
			}
			AddGraph(OutGraphs, Graph, TEXT("ubergraph"));
		}
		for (UEdGraph* Graph : Blueprint.FunctionGraphs)
		{
			AddGraph(OutGraphs, Graph, TEXT("function"));
		}
		for (const FBPInterfaceDescription& Interface : Blueprint.ImplementedInterfaces)
		{
			for (UEdGraph* Graph : InterfaceGraphsInEditorOrder(Interface))
			{
				AddGraph(OutGraphs, Graph, TEXT("function"));
			}
		}
		for (UEdGraph* Graph : Blueprint.MacroGraphs)
		{
			AddGraph(OutGraphs, Graph, TEXT("macro"));
		}
		for (UEdGraph* Graph : Blueprint.DelegateSignatureGraphs)
		{
			AddGraph(OutGraphs, Graph, TEXT("delegate_signature"));
		}
	}

	virtual bool CreateGraph(
		UObject& Asset, FStringView Role, FStringView Name, UEdGraph* ParentGraph, UEdGraph*& OutGraph,
		FAssetDocumentGraphError& OutError) const override
	{
		UBlueprint& Blueprint = *CastChecked<UBlueprint>(&Asset);
		if (ParentGraph != nullptr)
		{
			OutError.Set(TEXT("Graph.GraphOperationUnsupported"),
				TEXT("Blueprint graphs are top-level; nested graphs come from composite/state nodes."), TEXT("/parent"));
			return false;
		}
		const bool bFunction = Role == TEXTVIEW("function");
		const bool bMacro = Role == TEXTVIEW("macro");
		const bool bUbergraph = Role == TEXTVIEW("ubergraph");
		if (!bFunction && !bMacro && !bUbergraph)
		{
			OutError.Set(TEXT("Graph.GraphOperationUnsupported"), FString::Printf(
				TEXT("Blueprints can create function, macro or ubergraph graphs, not '%.*s'."), Role.Len(), Role.GetData()), TEXT("/role"));
			return false;
		}
		if (bUbergraph && !FBlueprintEditorUtils::DoesSupportEventGraphs(&Blueprint))
		{
			OutError.Set(TEXT("Graph.GraphOperationUnsupported"), TEXT("This Blueprint type has no event graphs."), TEXT("/role"));
			return false;
		}
		// FBlueprintEditor::NewDocument_IsVisibleForType: a Macro Library has no functions (an Animation
		// Layer Interface has layers, not functions); macros exist in Blueprint Classes and Macro Libraries.
		const EBlueprintType Type = Blueprint.BlueprintType;
		const bool bAnimBlueprint = Blueprint.IsA<UAnimBlueprint>();
		if (bFunction && (bAnimBlueprint ? Type == BPTYPE_Interface : Type == BPTYPE_MacroLibrary))
		{
			OutError.Set(TEXT("Graph.GraphOperationUnsupported"), TEXT("This Blueprint type has no function graphs."), TEXT("/role"));
			return false;
		}
		if (bMacro && Type != BPTYPE_MacroLibrary && Type != BPTYPE_Normal && Type != BPTYPE_LevelScript)
		{
			OutError.Set(TEXT("Graph.GraphOperationUnsupported"), TEXT("This Blueprint type has no macro graphs."), TEXT("/role"));
			return false;
		}
		// A function graph named after an overridable parent (or interface) function implements it,
		// as the My Blueprint panel's Override menu does (SMyBlueprint::ImplementFunction: a graph
		// with the inherited signature).
		UFunction* OverrideFunction = nullptr;
		UClass* const OverrideClass = bFunction && FindObject<UEdGraph>(&Blueprint, *FString(Name)) == nullptr
			? FBlueprintEditorUtils::GetOverrideFunctionClass(&Blueprint, FName(Name), &OverrideFunction) : nullptr;
		if (OverrideClass != nullptr && OverrideFunction != nullptr && UEdGraphSchema_K2::CanKismetOverrideFunction(OverrideFunction))
		{
			Blueprint.Modify();
			UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
				&Blueprint, FName(Name), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			FBlueprintEditorUtils::AddFunctionGraph(&Blueprint, Graph, /*bIsUserCreated=*/false, OverrideClass);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
			OutGraph = Graph;
			return true;
		}
		if (!ValidateName(Blueprint, Name, NAME_None, OutError))
		{
			return false;
		}
		// FBlueprintEditor::OnAddNewFunction / OnAddNewMacro / OnAddNewEventGraph.
		Blueprint.Modify();
		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
			&Blueprint, FName(Name), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (bFunction)
		{
			FBlueprintEditorUtils::AddFunctionGraph<UClass>(&Blueprint, Graph, /*bIsUserCreated=*/true, nullptr);
		}
		else if (bMacro)
		{
			FBlueprintEditorUtils::AddMacroGraph(&Blueprint, Graph, /*bIsUserCreated=*/true, nullptr);
		}
		else
		{
			FBlueprintEditorUtils::AddUbergraphPage(&Blueprint, Graph);
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
		OutGraph = Graph;
		return true;
	}

	virtual bool RemoveGraph(UObject& Asset, UEdGraph& Graph, FAssetDocumentGraphError& OutError) const override
	{
		if (!Graph.bAllowDeletion)
		{
			OutError.Set(TEXT("Graph.GraphOperationUnsupported"),
				FString::Printf(TEXT("Graph %s cannot be deleted."), *Graph.GetName()), TEXT("/graph"));
			return false;
		}
		UBlueprint& Blueprint = *CastChecked<UBlueprint>(&Asset);
		Blueprint.Modify();
		FBlueprintEditorUtils::RemoveGraph(&Blueprint, &Graph, EGraphRemoveFlags::MarkTransient);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
		return true;
	}

	virtual bool RenameGraph(UObject& Asset, UEdGraph& Graph, FStringView NewName, FAssetDocumentGraphError& OutError) const override
	{
		if (!(Graph.bAllowRenaming || Graph.bAllowDeletion))
		{
			OutError.Set(TEXT("Graph.GraphOperationUnsupported"),
				FString::Printf(TEXT("Graph %s cannot be renamed."), *Graph.GetName()), TEXT("/graph"));
			return false;
		}
		UBlueprint& Blueprint = *CastChecked<UBlueprint>(&Asset);
		if (!ValidateName(Blueprint, NewName, Graph.GetFName(), OutError))
		{
			return false;
		}
		FBlueprintEditorUtils::RenameGraph(&Graph, FString(NewName));
		return true;
	}

	// Package-snapshot rollback recompiles through AssetDocumentGraphBlueprintRebuilder.
	virtual bool Finalize(UObject& Asset, FAssetDocumentGraphFinalizeResult& OutResult, FAssetDocumentGraphError& OutError) const override
	{
		UBlueprint& Blueprint = *CastChecked<UBlueprint>(&Asset);
		FCompilerResultsLog Log;
		Log.bSilentMode = true;
		Log.SetSourcePath(Blueprint.GetPathName());
		AssetDocumentGraphBlueprintHostAdapterLocal::NormalizeUserPinDefaultsAsSaved(Blueprint);
		FKismetEditorUtilities::CompileBlueprint(&Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Log);
		OutResult = FAssetDocumentGraphFinalizeResult();
		bool bErrors = Blueprint.Status == BS_Error || Log.NumErrors > 0;
		bool bWarnings = Log.NumWarnings > 0;
		for (const TSharedRef<FTokenizedMessage>& Message : Log.Messages)
		{
			FAssetDocumentGraphFinalizeMessage& Out = OutResult.Messages.AddDefaulted_GetRef();
			Out.Severity = SeverityText(Message->GetSeverity());
			Out.Message = Message->ToText().ToString();
			if (!Message->GetIdentifier().IsNone())
			{
				Out.Identifier = Message->GetIdentifier().ToString();
			}
			for (const TSharedRef<IMessageToken>& Token : Message->GetMessageTokens())
			{
				if (Token->GetType() != EMessageToken::EdGraph)
				{
					continue;
				}
				const FEdGraphToken& GraphToken = static_cast<const FEdGraphToken&>(*Token);
				const UEdGraphNode* Node = nullptr;
				if (const UEdGraphPin* Pin = GraphToken.GetPin())
				{
					Node = Pin->GetOwningNodeUnchecked();
					if (Out.PinName.IsEmpty())
					{
						Out.PinName = Pin->PinName.ToString();
						Out.bOrphanedPin = Pin->bOrphanedPin;
					}
				}
				if (Node == nullptr)
				{
					Node = Cast<UEdGraphNode>(GraphToken.GetGraphObject());
				}
				if (Node != nullptr && !Out.NodeGuid.IsValid())
				{
					Out.NodeGuid = Node->NodeGuid;
				}
			}
		}
		OutResult.Status = bErrors ? TEXT("errors") : bWarnings ? TEXT("warnings") : TEXT("ok");
		return true;
	}

	virtual void NotifyGraphChanged(UObject& Asset, UEdGraph* Graph, bool bStructural) const override
	{
		UBlueprint& Blueprint = *CastChecked<UBlueprint>(&Asset);
		if (Graph != nullptr)
		{
			Graph->NotifyGraphChanged();
		}
		if (bStructural)
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
		}
		else
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified(&Blueprint);
		}
	}
};
} // namespace

TSharedRef<IAssetDocumentGraphHostAdapter> MakeBlueprintHostAdapter()
{
	return MakeShared<FBlueprintHostAdapter>();
}
} // namespace UE::AssetDocument::Graph
