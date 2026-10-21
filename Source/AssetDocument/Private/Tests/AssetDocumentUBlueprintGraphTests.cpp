// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "AssetDocumentSidecar.h"
#include "Profiles/UBlueprintAssetDocumentCapability.h"

#include "BlueprintEventNodeSpawner.h"
#include "BlueprintNodeSpawner.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/MemberReference.h"
#include "GameFramework/Actor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Self.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
TSharedRef<FJsonValue> MakeUBlueprintGraphBodyValue(const TSharedRef<FJsonObject>& Body)
{
	return StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueObject>(Body));
}

FString ResolveUEAssetDocPluginBaseDir()
{
	if (const TSharedPtr<IPlugin> UEAssetDocPlugin = IPluginManager::Get().FindPlugin(TEXT("UEAssetDoc")))
	{
		return UEAssetDocPlugin->GetBaseDir();
	}

	TArray<FString> CandidatePluginFiles;
	IFileManager::Get().FindFilesRecursive(
		CandidatePluginFiles,
		*FPaths::ProjectPluginsDir(),
		TEXT("UEAssetDoc.uplugin"),
		true,
		false,
		false);

	CandidatePluginFiles.Sort();
	return CandidatePluginFiles.Num() > 0
		? FPaths::ConvertRelativePathToFull(FPaths::GetPath(CandidatePluginFiles[0]))
		: FString();
}

TSharedRef<FJsonObject> MakeUBlueprintGraphActorParentClassRef()
{
	TSharedRef<FJsonObject> ParentClass = MakeShared<FJsonObject>();
	ParentClass->SetStringField(TEXT("Kind"), TEXT("ClassRef"));
	ParentClass->SetStringField(TEXT("Class"), TEXT("/Script/Engine.Actor"));
	return ParentClass;
}

TSharedRef<FJsonObject> MakeMemberRef(const TCHAR* OwnerClass, const TCHAR* Name)
{
	TSharedRef<FJsonObject> Member = MakeShared<FJsonObject>();
	Member->SetStringField(TEXT("Kind"), TEXT("MemberRef"));
	Member->SetStringField(TEXT("OwnerClass"), OwnerClass);
	Member->SetStringField(TEXT("Name"), Name);
	return Member;
}

TSharedRef<FJsonObject> UBlueprintGraphTestMakeGraphNode(const TCHAR* Id, const TCHAR* ClassPath, TSharedPtr<FJsonObject> Member = nullptr)
{
	TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("Id"), Id);
	Node->SetStringField(TEXT("Class"), ClassPath);
	if (Member.IsValid())
	{
		Node->SetObjectField(TEXT("Member"), Member);
	}

	TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
	Position->SetNumberField(TEXT("X"), 0.0);
	Position->SetNumberField(TEXT("Y"), 0.0);
	Node->SetObjectField(TEXT("Position"), Position);
	return Node;
}

TArray<TSharedPtr<FJsonValue>> MakeJsonArray(std::initializer_list<TSharedRef<FJsonObject>> Objects)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const TSharedRef<FJsonObject>& Object : Objects)
	{
		Values.Add(MakeShared<FJsonValueObject>(Object));
	}
	return Values;
}

TSharedRef<FJsonObject> MakeEventGraph(std::initializer_list<TSharedRef<FJsonObject>> Nodes)
{
	TSharedRef<FJsonObject> Graph = MakeShared<FJsonObject>();
	Graph->SetStringField(TEXT("Name"), TEXT("EventGraph"));
	Graph->SetStringField(TEXT("Schema"), TEXT("/Script/BlueprintGraph.EdGraphSchema_K2"));
	Graph->SetArrayField(TEXT("Nodes"), MakeJsonArray(Nodes));
	Graph->SetArrayField(TEXT("Links"), {});
	return Graph;
}

TSharedRef<FJsonObject> MakeBodyWithRegion(const FString& RegionName, std::initializer_list<TSharedRef<FJsonObject>> Entries)
{
	TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetObjectField(TEXT("ParentClass"), MakeUBlueprintGraphActorParentClassRef());
	Body->SetArrayField(RegionName, MakeJsonArray(Entries));
	return Body;
}

bool UBlueprintGraphResultHasDiagnosticCode(const FAssetDocumentCapabilityResult& Result, const FString& Code)
{
	return Result.Diagnostics.ContainsByPredicate([&Code](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == Code;
	});
}

bool UBlueprintGraphResultHasDiagnostic(const FAssetDocumentCapabilityResult& Result, const FString& Path, const FString& Code)
{
	return Result.Diagnostics.ContainsByPredicate([&Path, &Code](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Path == Path && Diagnostic.Code == Code;
	});
}

TSharedPtr<FJsonObject> GetFirstUnsupportedGraphDiagnostic(const FAssetDocumentCapabilityResult& Result)
{
	if (!Result.Payload.IsValid())
	{
		return nullptr;
	}

	const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
	if (!Result.Payload->TryGetArrayField(TEXT("UnsupportedGraphDiagnostics"), Diagnostics) || !Diagnostics || Diagnostics->IsEmpty())
	{
		return nullptr;
	}

	return (*Diagnostics)[0].IsValid() ? (*Diagnostics)[0]->AsObject() : nullptr;
}

int32 GetUnsupportedGraphDiagnosticCount(const FAssetDocumentCapabilityResult& Result)
{
	if (!Result.Payload.IsValid())
	{
		return 0;
	}

	const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
	return Result.Payload->TryGetArrayField(TEXT("UnsupportedGraphDiagnostics"), Diagnostics) && Diagnostics
		? Diagnostics->Num()
		: 0;
}

bool FallbackHasActionableFields(const TSharedPtr<FJsonObject>& Fallback)
{
	if (!Fallback.IsValid())
	{
		return false;
	}

	return Fallback->HasTypedField<EJson::String>(TEXT("Code"))
		&& Fallback->HasTypedField<EJson::String>(TEXT("Path"))
		&& Fallback->HasTypedField<EJson::String>(TEXT("Class"))
		&& Fallback->HasField(TEXT("Capability"))
		&& Fallback->HasField(TEXT("Member"))
		&& Fallback->HasTypedField<EJson::String>(TEXT("Reason"))
		&& Fallback->HasTypedField<EJson::String>(TEXT("SuggestedAction"));
}

UBlueprint* CreateTransientUBlueprintGraphActorBlueprint(const TCHAR* NamePrefix)
{
	const FName BlueprintName(*FString::Printf(TEXT("%s_%s"), NamePrefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		BlueprintName,
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	if (Blueprint)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}
	return Blueprint;
}

UEdGraph* GetEventGraph(UBlueprint* Blueprint)
{
	return Blueprint ? FBlueprintEditorUtils::FindEventGraph(Blueprint) : nullptr;
}

template <typename NodeType>
NodeType* AddUBlueprintGraphK2Node(UEdGraph* Graph, int32 X, int32 Y)
{
	NodeType* Node = Graph ? NewObject<NodeType>(Graph) : nullptr;
	if (!Node)
	{
		return nullptr;
	}

	Graph->AddNode(Node, true, false);
	Node->NodePosX = X;
	Node->NodePosY = Y;
	return Node;
}

UK2Node_Event* AddBeginPlayNode(UEdGraph* Graph, int32 X = 0, int32 Y = 0)
{
	UFunction* Function = AActor::StaticClass()->FindFunctionByName(TEXT("ReceiveBeginPlay"));
	UBlueprintEventNodeSpawner* Spawner = Function
		? UBlueprintEventNodeSpawner::Create(Function)
		: nullptr;
	if (!Graph || !Spawner)
	{
		return nullptr;
	}

	IBlueprintNodeBinder::FBindingSet Bindings;
	UK2Node_Event* Node = Cast<UK2Node_Event>(Spawner->Invoke(Graph, Bindings, FVector2D(X, Y)));
	if (Node)
	{
		Node->NodePosX = X;
		Node->NodePosY = Y;
	}
	return Node;
}

UK2Node_CallFunction* AddPrintStringNode(UEdGraph* Graph, int32 X = 320, int32 Y = 0, const FString& InString = FString())
{
	UK2Node_CallFunction* Node = AddUBlueprintGraphK2Node<UK2Node_CallFunction>(Graph, X, Y);
	UFunction* Function = UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString"));
	if (!Node || !Function)
	{
		return nullptr;
	}

	Node->SetFromFunction(Function);
	Node->AllocateDefaultPins();
	if (!InString.IsEmpty())
	{
		if (UEdGraphPin* InStringPin = Node->FindPin(TEXT("InString")))
		{
			InStringPin->DefaultValue = InString;
		}
	}
	return Node;
}

UK2Node_VariableGet* AddVariableGetNode(UBlueprint* Blueprint, UEdGraph* Graph, FName VariableName, int32 X, int32 Y)
{
	UK2Node_VariableGet* Node = AddUBlueprintGraphK2Node<UK2Node_VariableGet>(Graph, X, Y);
	if (!Node || !Blueprint)
	{
		return nullptr;
	}

	Node->VariableReference.SetSelfMember(VariableName);
	Node->AllocateDefaultPins();
	return Node;
}

UK2Node_VariableSet* AddVariableSetNode(UBlueprint* Blueprint, UEdGraph* Graph, FName VariableName, int32 X, int32 Y)
{
	UK2Node_VariableSet* Node = AddUBlueprintGraphK2Node<UK2Node_VariableSet>(Graph, X, Y);
	if (!Node || !Blueprint)
	{
		return nullptr;
	}

	Node->VariableReference.SetSelfMember(VariableName);
	Node->AllocateDefaultPins();
	return Node;
}

UK2Node_Self* AddSelfNode(UEdGraph* Graph, int32 X, int32 Y)
{
	UK2Node_Self* Node = AddUBlueprintGraphK2Node<UK2Node_Self>(Graph, X, Y);
	if (Node)
	{
		Node->AllocateDefaultPins();
	}
	return Node;
}

UEdGraph* AddBlueprintFunctionLocalVariable(UBlueprint* Blueprint, FName FunctionName, FName VariableName)
{
	if (!Blueprint)
	{
		return nullptr;
	}

	UEdGraph* FunctionGraph = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint,
		FunctionName,
		UEdGraph::StaticClass(),
		UEdGraphSchema_K2::StaticClass());
	if (!FunctionGraph)
	{
		return nullptr;
	}
	FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Blueprint, FunctionGraph, false, nullptr);
	if (const UEdGraphSchema* Schema = FunctionGraph->GetSchema())
	{
		Schema->CreateDefaultNodesForGraph(*FunctionGraph);
	}

	TArray<UK2Node_FunctionEntry*> Entries;
	FunctionGraph->GetNodesOfClass(Entries);
	if (Entries.Num() != 1)
	{
		return nullptr;
	}

	FBPVariableDescription LocalVariable;
	LocalVariable.VarName = VariableName;
	LocalVariable.VarGuid = FGuid::NewGuid();
	LocalVariable.VarType.PinCategory = UEdGraphSchema_K2::PC_Int;
	Entries[0]->LocalVariables.Add(MoveTemp(LocalVariable));
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	return FunctionGraph;
}

UK2Node_IfThenElse* AddUBlueprintGraphUnsupportedBranchNode(UEdGraph* Graph, int32 X, int32 Y)
{
	UK2Node_IfThenElse* Node = AddUBlueprintGraphK2Node<UK2Node_IfThenElse>(Graph, X, Y);
	if (Node)
	{
		Node->AllocateDefaultPins();
	}
	return Node;
}

bool LinkPins(UEdGraphPin* From, UEdGraphPin* To)
{
	if (!From || !To)
	{
		return false;
	}
	From->MakeLinkTo(To);
	return true;
}

TSharedRef<FJsonObject> ExtractBlueprintBody(UBlueprint* Blueprint, FAssetDocumentCapabilityResult& OutResult)
{
	const FUBlueprintAssetDocumentCapability Capability;
	FAssetDocumentCapabilityContext Context;
	Context.Asset = Blueprint;
	TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	OutResult = Capability.Extract(Context, Body);
	return Body;
}

const TArray<TSharedPtr<FJsonValue>>* GetUbergraphPages(const TSharedRef<FJsonObject>& Body)
{
	const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
	return Body->TryGetArrayField(TEXT("UbergraphPages"), Graphs) ? Graphs : nullptr;
}

TSharedPtr<FJsonObject> FindNodeByClass(const TArray<TSharedPtr<FJsonValue>>& Nodes, const FString& ClassPath)
{
	for (const TSharedPtr<FJsonValue>& Value : Nodes)
	{
		const TSharedPtr<FJsonObject> Node = Value.IsValid() ? Value->AsObject() : nullptr;
		if (!Node.IsValid())
		{
			continue;
		}
		FString Class;
		if (Node->TryGetStringField(TEXT("Class"), Class) && Class == ClassPath)
		{
			return Node;
		}
	}
	return nullptr;
}

TSharedPtr<FJsonObject> FindNodeByMemberName(const TArray<TSharedPtr<FJsonValue>>& Nodes, const FString& MemberName)
{
	for (const TSharedPtr<FJsonValue>& Value : Nodes)
	{
		const TSharedPtr<FJsonObject> Node = Value.IsValid() ? Value->AsObject() : nullptr;
		const TSharedPtr<FJsonObject>* Member = nullptr;
		if (!Node.IsValid() || !Node->TryGetObjectField(TEXT("Member"), Member) || !Member || !Member->IsValid())
		{
			continue;
		}

		FString Name;
		if ((*Member)->TryGetStringField(TEXT("Name"), Name) && Name == MemberName)
		{
			return Node;
		}
	}
	return nullptr;
}

bool NodeHasSparsePinOverride(const TSharedPtr<FJsonObject>& Node, const FString& PinName, const FString& ExpectedDefault)
{
	if (!Node.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* PinOverrides = nullptr;
	if (!Node->TryGetArrayField(TEXT("PinOverrides"), PinOverrides) || !PinOverrides)
	{
		return false;
	}

	return PinOverrides->ContainsByPredicate([&](const TSharedPtr<FJsonValue>& Value)
	{
		const TSharedPtr<FJsonObject> Pin = Value.IsValid() ? Value->AsObject() : nullptr;
		if (!Pin.IsValid())
		{
			return false;
		}
		FString PinId;
		FString DefaultValue;
		return Pin->TryGetStringField(TEXT("Pin"), PinId)
			&& PinId == PinName
			&& Pin->TryGetStringField(TEXT("DefaultValue"), DefaultValue)
			&& DefaultValue == ExpectedDefault;
	});
}

bool SkippedGraphsContainClass(const TSharedRef<FJsonObject>& Body, const FString& ClassPath)
{
	const TSharedPtr<FJsonObject>* Skipped = nullptr;
	const TSharedPtr<FJsonObject>* Graphs = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	if (!Body->TryGetObjectField(TEXT("_Skipped"), Skipped) || !Skipped || !Skipped->IsValid()
		|| !(*Skipped)->TryGetObjectField(TEXT("Graphs"), Graphs) || !Graphs || !Graphs->IsValid()
		|| !(*Graphs)->TryGetArrayField(TEXT("Nodes"), Nodes) || !Nodes)
	{
		return false;
	}

	return Nodes->ContainsByPredicate([&](const TSharedPtr<FJsonValue>& Value)
	{
		const TSharedPtr<FJsonObject> Node = Value.IsValid() ? Value->AsObject() : nullptr;
		FString FoundClass;
		return Node.IsValid() && Node->TryGetStringField(TEXT("Class"), FoundClass) && FoundClass == ClassPath;
	});
}

TSharedPtr<FJsonObject> UBlueprintGraphTestFindDiffEntryByPath(const TArray<TSharedPtr<FJsonValue>>& Entries, const FString& ExpectedPath)
{
	for (const TSharedPtr<FJsonValue>& Value : Entries)
	{
		const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
		FString Path;
		if (Entry.IsValid() && Entry->TryGetStringField(TEXT("path"), Path) && Path == ExpectedPath)
		{
			return Entry;
		}
	}
	return nullptr;
}

void RemoveExtractOnlyEvidence(const TSharedRef<FJsonObject>& Body)
{
	Body->RemoveField(TEXT("_Skipped"));
}

bool HasNonUnchangedGraphDiffEntry(const TArray<TSharedPtr<FJsonValue>>& Entries)
{
	for (const TSharedPtr<FJsonValue>& Value : Entries)
	{
		const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
		FString Path;
		FString Status;
		if (Entry.IsValid()
			&& Entry->TryGetStringField(TEXT("path"), Path)
			&& Path.StartsWith(TEXT("/Body/UbergraphPages/"))
			&& Entry->TryGetStringField(TEXT("status"), Status)
			&& Status != TEXT("unchanged"))
		{
			return true;
		}
	}
	return false;
}

struct FDiffRoutingBuckets
{
	int32 Changed = 0;
	int32 Unchanged = 0;
	int32 Skipped = 0;
	int32 Failed = 0;
};

FDiffRoutingBuckets RouteLikeAssetDocumentService(const TArray<TSharedPtr<FJsonValue>>& Entries)
{
	FDiffRoutingBuckets Buckets;
	for (const TSharedPtr<FJsonValue>& EntryValue : Entries)
	{
		const TSharedPtr<FJsonObject> EntryObject = EntryValue.IsValid() ? EntryValue->AsObject() : nullptr;
		FString Status;
		if (!EntryObject.IsValid() || !EntryObject->TryGetStringField(TEXT("status"), Status))
		{
			++Buckets.Failed;
		}
		else if (Status == TEXT("changed"))
		{
			++Buckets.Changed;
		}
		else if (Status == TEXT("unchanged"))
		{
			++Buckets.Unchanged;
		}
		else if (Status == TEXT("skipped"))
		{
			++Buckets.Skipped;
		}
		else
		{
			++Buckets.Failed;
		}
	}
	return Buckets;
}

FAssetDocumentCapabilityResult DiffBlueprintBody(
	UBlueprint* Blueprint,
	const TSharedRef<FJsonObject>& DesiredBody,
	TArray<TSharedPtr<FJsonValue>>& OutDiffEntries,
	const TSharedPtr<FJsonObject>* Definitions = nullptr)
{
	const FUBlueprintAssetDocumentCapability Capability;
	FAssetDocumentCapabilityContext Context;
	Context.Asset = Blueprint;
	Context.Definitions = Definitions;
	return Capability.Diff(Context, MakeUBlueprintGraphBodyValue(DesiredBody), OutDiffEntries);
}

TSharedPtr<FJsonObject> FindGraphJsonByName(const TSharedRef<FJsonObject>& Body, const FString& GraphName)
{
	const TArray<TSharedPtr<FJsonValue>>* Graphs = GetUbergraphPages(Body);
	if (!Graphs)
	{
		return nullptr;
	}

	for (const TSharedPtr<FJsonValue>& Value : *Graphs)
	{
		const TSharedPtr<FJsonObject> Graph = Value.IsValid() ? Value->AsObject() : nullptr;
		FString Name;
		if (Graph.IsValid() && Graph->TryGetStringField(TEXT("Name"), Name) && Name == GraphName)
		{
			return Graph;
		}
	}
	return nullptr;
}

FString GetNodeIdByMemberName(const TSharedPtr<FJsonObject>& Graph, const FString& MemberName)
{
	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	if (!Graph.IsValid() || !Graph->TryGetArrayField(TEXT("Nodes"), Nodes) || !Nodes)
	{
		return FString();
	}

	const TSharedPtr<FJsonObject> Node = FindNodeByMemberName(*Nodes, MemberName);
	FString Id;
	return Node.IsValid() && Node->TryGetStringField(TEXT("Id"), Id) ? Id : FString();
}

TSharedPtr<FJsonObject> FindNodeById(const TSharedPtr<FJsonObject>& Graph, const FString& NodeId)
{
	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	if (!Graph.IsValid() || !Graph->TryGetArrayField(TEXT("Nodes"), Nodes) || !Nodes)
	{
		return nullptr;
	}

	for (const TSharedPtr<FJsonValue>& Value : *Nodes)
	{
		const TSharedPtr<FJsonObject> Node = Value.IsValid() ? Value->AsObject() : nullptr;
		FString Id;
		if (Node.IsValid() && Node->TryGetStringField(TEXT("Id"), Id) && Id == NodeId)
		{
			return Node;
		}
	}
	return nullptr;
}

bool RemoveNodeByMemberName(const TSharedPtr<FJsonObject>& Graph, const FString& MemberName)
{
	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	if (!Graph.IsValid() || !Graph->TryGetArrayField(TEXT("Nodes"), Nodes) || !Nodes)
	{
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> FilteredNodes;
	bool bRemoved = false;
	for (const TSharedPtr<FJsonValue>& Value : *Nodes)
	{
		const TSharedPtr<FJsonObject> Node = Value.IsValid() ? Value->AsObject() : nullptr;
		const TSharedPtr<FJsonObject>* Member = nullptr;
		FString Name;
		if (Node.IsValid()
			&& Node->TryGetObjectField(TEXT("Member"), Member)
			&& Member
			&& Member->IsValid()
			&& (*Member)->TryGetStringField(TEXT("Name"), Name)
			&& Name == MemberName)
		{
			bRemoved = true;
			continue;
		}
		FilteredNodes.Add(Value);
	}
	Graph->SetArrayField(TEXT("Nodes"), MoveTemp(FilteredNodes));
	return bRemoved;
}

bool SetPinDefaultForMemberNode(const TSharedPtr<FJsonObject>& Graph, const FString& MemberName, const FString& PinName, const FString& DefaultValue)
{
	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	if (!Graph.IsValid() || !Graph->TryGetArrayField(TEXT("Nodes"), Nodes) || !Nodes)
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Node = FindNodeByMemberName(*Nodes, MemberName);
	if (!Node.IsValid())
	{
		return false;
	}

	TSharedRef<FJsonObject> PinOverride = MakeShared<FJsonObject>();
	PinOverride->SetStringField(TEXT("Pin"), PinName);
	PinOverride->SetStringField(TEXT("DefaultValue"), DefaultValue);
	Node->SetArrayField(TEXT("PinOverrides"), MakeJsonArray({PinOverride}));
	return true;
}

bool ReplaceMemberWithDefinitionRef(
	const TSharedPtr<FJsonObject>& Graph,
	const FString& MemberName,
	const FString& DefinitionId)
{
	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	if (!Graph.IsValid() || !Graph->TryGetArrayField(TEXT("Nodes"), Nodes) || !Nodes)
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Node = FindNodeByMemberName(*Nodes, MemberName);
	if (!Node.IsValid())
	{
		return false;
	}

	TSharedRef<FJsonObject> DefinitionRef = MakeShared<FJsonObject>();
	DefinitionRef->SetStringField(TEXT("Kind"), TEXT("DefinitionRef"));
	DefinitionRef->SetStringField(TEXT("Id"), DefinitionId);
	Node->SetObjectField(TEXT("Member"), DefinitionRef);
	return true;
}

void AddLinkToGraph(
	const TSharedPtr<FJsonObject>& Graph,
	const FString& FromNode,
	const FString& FromPin,
	const FString& ToNode,
	const FString& ToPin)
{
	TSharedRef<FJsonObject> From = MakeShared<FJsonObject>();
	From->SetStringField(TEXT("Node"), FromNode);
	From->SetStringField(TEXT("Pin"), FromPin);

	TSharedRef<FJsonObject> To = MakeShared<FJsonObject>();
	To->SetStringField(TEXT("Node"), ToNode);
	To->SetStringField(TEXT("Pin"), ToPin);

	TSharedRef<FJsonObject> Link = MakeShared<FJsonObject>();
	Link->SetObjectField(TEXT("From"), From);
	Link->SetObjectField(TEXT("To"), To);
	Graph->SetArrayField(TEXT("Links"), MakeJsonArray({Link}));
}

void AppendLinkToGraph(
	const TSharedPtr<FJsonObject>& Graph,
	const FString& FromNode,
	const FString& FromPin,
	const FString& ToNode,
	const FString& ToPin)
{
	if (!Graph.IsValid())
	{
		return;
	}

	const TArray<TSharedPtr<FJsonValue>>* ExistingLinks = nullptr;
	TArray<TSharedPtr<FJsonValue>> Links;
	if (Graph->TryGetArrayField(TEXT("Links"), ExistingLinks) && ExistingLinks)
	{
		Links = *ExistingLinks;
	}

	TSharedRef<FJsonObject> From = MakeShared<FJsonObject>();
	From->SetStringField(TEXT("Node"), FromNode);
	From->SetStringField(TEXT("Pin"), FromPin);

	TSharedRef<FJsonObject> To = MakeShared<FJsonObject>();
	To->SetStringField(TEXT("Node"), ToNode);
	To->SetStringField(TEXT("Pin"), ToPin);

	TSharedRef<FJsonObject> Link = MakeShared<FJsonObject>();
	Link->SetObjectField(TEXT("From"), From);
	Link->SetObjectField(TEXT("To"), To);
	Links.Add(MakeShared<FJsonValueObject>(Link));
	Graph->SetArrayField(TEXT("Links"), MoveTemp(Links));
}

TSharedRef<FJsonObject> MakeBeginPlayPrintStringBody(
	const FString& PrintDefault = TEXT("Hello from AssetDocument"),
	bool bIncludePrint = true,
	bool bIncludeLink = true,
	const FString& FromPin = UEdGraphSchema_K2::PN_Then.ToString(),
	const FString& ToPin = UEdGraphSchema_K2::PN_Execute.ToString())
{
	TSharedRef<FJsonObject> BeginPlay = UBlueprintGraphTestMakeGraphNode(
		TEXT("BeginPlay"),
		TEXT("/Script/BlueprintGraph.K2Node_Event"),
		MakeMemberRef(TEXT("/Script/Engine.Actor"), TEXT("ReceiveBeginPlay")));

	TArray<TSharedPtr<FJsonValue>> Nodes;
	Nodes.Add(MakeShared<FJsonValueObject>(BeginPlay));

	if (bIncludePrint)
	{
		TSharedRef<FJsonObject> Print = UBlueprintGraphTestMakeGraphNode(
			TEXT("Print"),
			TEXT("/Script/BlueprintGraph.K2Node_CallFunction"),
			MakeMemberRef(TEXT("/Script/Engine.KismetSystemLibrary"), TEXT("PrintString")));
		TSharedRef<FJsonObject> PinOverride = MakeShared<FJsonObject>();
		PinOverride->SetStringField(TEXT("Pin"), TEXT("InString"));
		PinOverride->SetStringField(TEXT("DefaultValue"), PrintDefault);
		Print->SetArrayField(TEXT("PinOverrides"), MakeJsonArray({PinOverride}));

		TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
		Position->SetNumberField(TEXT("X"), 320.0);
		Position->SetNumberField(TEXT("Y"), 0.0);
		Print->SetObjectField(TEXT("Position"), Position);

		Nodes.Add(MakeShared<FJsonValueObject>(Print));
	}

	TSharedRef<FJsonObject> Graph = MakeShared<FJsonObject>();
	Graph->SetStringField(TEXT("Name"), TEXT("EventGraph"));
	Graph->SetStringField(TEXT("Schema"), TEXT("/Script/BlueprintGraph.EdGraphSchema_K2"));
	Graph->SetArrayField(TEXT("Nodes"), MoveTemp(Nodes));
	Graph->SetArrayField(TEXT("Links"), {});
	if (bIncludePrint && bIncludeLink)
	{
		AddLinkToGraph(Graph, TEXT("BeginPlay"), FromPin, TEXT("Print"), ToPin);
	}

	return MakeBodyWithRegion(TEXT("UbergraphPages"), {Graph});
}

TSharedPtr<FJsonObject> MakeUBlueprintGraphSidecarDocument(const FString& Target, const TSharedRef<FJsonObject>& Body)
{
	if (!Body->HasField(TEXT("ImplementedInterfaces")))
	{
		Body->SetArrayField(TEXT("ImplementedInterfaces"), {});
	}
	if (!Body->HasField(TEXT("Variables")))
	{
		Body->SetArrayField(TEXT("Variables"), {});
	}
	if (!Body->HasField(TEXT("Components")))
	{
		Body->SetArrayField(TEXT("Components"), {});
	}
	if (!Body->HasField(TEXT("ClassDefaults")))
	{
		Body->SetObjectField(TEXT("ClassDefaults"), MakeShared<FJsonObject>());
	}
	if (!Body->HasField(TEXT("FunctionGraphs")))
	{
		Body->SetArrayField(TEXT("FunctionGraphs"), {});
	}
	if (!Body->HasField(TEXT("MacroGraphs")))
	{
		Body->SetArrayField(TEXT("MacroGraphs"), {});
	}
	if (!Body->HasField(TEXT("Timelines")))
	{
		Body->SetArrayField(TEXT("Timelines"), {});
	}

	TSharedPtr<FJsonObject> Document = MakeShared<FJsonObject>();
	Document->SetNumberField(TEXT("SchemaVersion"), 1);
	Document->SetStringField(TEXT("Target"), Target);
	Document->SetStringField(TEXT("Class"), TEXT("/Script/Engine.Blueprint"));
	Document->SetStringField(TEXT("Action"), TEXT("CreateOrUpdate"));
	Document->SetObjectField(TEXT("Definitions"), MakeShared<FJsonObject>());
	Document->SetObjectField(TEXT("Properties"), MakeShared<FJsonObject>());
	Document->SetObjectField(TEXT("Body"), Body);
	return Document;
}

FString MakeUniqueBlueprintTarget(const TCHAR* Prefix)
{
	return FString::Printf(
		TEXT("/Game/AssetDocumentTests/%s_%s"),
		Prefix,
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

FString MakeUBlueprintGraphObjectPathFromTarget(const FString& Target)
{
	return FString::Printf(TEXT("%s.%s"), *Target, *FPackageName::GetLongPackageAssetName(Target));
}

bool UBlueprintGraphTestWriteJsonObjectToFile(TSharedPtr<FJsonObject> Document, const FString& FilePath)
{
	FString JsonText;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
	if (!Document.IsValid() || !FJsonSerializer::Serialize(Document.ToSharedRef(), Writer))
	{
		return false;
	}
	return FFileHelper::SaveStringToFile(JsonText, *FilePath);
}

TSharedRef<FJsonObject> MakeIntVariableSpec(const FString& Name, const FString& DefaultValue = TEXT("0"))
{
	TSharedRef<FJsonObject> Type = MakeShared<FJsonObject>();
	Type->SetStringField(TEXT("PinCategory"), UEdGraphSchema_K2::PC_Int.ToString());

	TSharedRef<FJsonObject> Variable = MakeShared<FJsonObject>();
	Variable->SetStringField(TEXT("Name"), Name);
	Variable->SetObjectField(TEXT("Type"), Type);
	Variable->SetStringField(TEXT("DefaultValue"), DefaultValue);
	return Variable;
}

FAssetDocumentCapabilityResult ApplyBlueprintBody(UBlueprint* Blueprint, const TSharedRef<FJsonObject>& Body)
{
	FUBlueprintAssetDocumentCapability Capability;
	FAssetDocumentCapabilityContext Context;
	Context.Asset = Blueprint;
	Context.AssetClass = UBlueprint::StaticClass();
	return Capability.Apply(Context, MakeUBlueprintGraphBodyValue(Body));
}

int32 CountGraphNodesByClass(UEdGraph* Graph, UClass* NodeClass)
{
	int32 Count = 0;
	if (!Graph || !NodeClass)
	{
		return Count;
	}

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Node->IsA(NodeClass))
		{
			++Count;
		}
	}
	return Count;
}

int32 CountGraphNodesByMemberName(UEdGraph* Graph, const FString& MemberName)
{
	int32 Count = 0;
	if (!Graph)
	{
		return Count;
	}

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (const UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
		{
			if (EventNode->GetFunctionName().ToString() == MemberName)
			{
				++Count;
			}
		}
		else if (const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
		{
			if (CallNode->GetFunctionName().ToString() == MemberName)
			{
				++Count;
			}
		}
	}
	return Count;
}

UK2Node_CallFunction* FindCallFunctionNodeByMemberName(UEdGraph* Graph, const FString& MemberName)
{
	if (!Graph)
	{
		return nullptr;
	}

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
		if (CallNode && CallNode->GetFunctionName().ToString() == MemberName)
		{
			return CallNode;
		}
	}
	return nullptr;
}

UK2Node_Variable* FindVariableNodeByMemberName(UEdGraph* Graph, const FString& MemberName)
{
	if (!Graph)
	{
		return nullptr;
	}

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node);
		if (VariableNode && VariableNode->GetVarName().ToString() == MemberName)
		{
			return VariableNode;
		}
	}
	return nullptr;
}

double GetBlueprintInitialLifeSpan(UBlueprint* Blueprint)
{
	const UBlueprintGeneratedClass* GeneratedClass = Blueprint ? Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass) : nullptr;
	const AActor* CDO = GeneratedClass ? Cast<AActor>(GeneratedClass->GetDefaultObject()) : nullptr;
	return CDO ? CDO->InitialLifeSpan : 0.0;
}

void SetClassDefaultInitialLifeSpan(const TSharedRef<FJsonObject>& Body, double Value)
{
	TSharedRef<FJsonObject> ClassDefaults = MakeShared<FJsonObject>();
	ClassDefaults->SetNumberField(TEXT("InitialLifeSpan"), Value);
	Body->SetObjectField(TEXT("ClassDefaults"), ClassDefaults);
}

bool GraphHasLink(UEdGraph* Graph, const FName FromPinName, const FName ToPinName)
{
	if (!Graph)
	{
		return false;
	}

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output || Pin->PinName != FromPinName)
			{
				continue;
			}

			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (LinkedPin && LinkedPin->PinName == ToPinName)
				{
					return true;
				}
			}
		}
	}
	return false;
}

FString GraphPath(const FString& GraphName)
{
	return FString::Printf(TEXT("/Body/UbergraphPages/%s"), *GraphName);
}

FString UBlueprintGraphNodePath(const FString& GraphName, const FString& NodeId)
{
	return FString::Printf(TEXT("/Body/UbergraphPages/%s/Nodes/%s"), *GraphName, *NodeId);
}

FString PinPath(const FString& GraphName, const FString& NodeId, const FString& PinId)
{
	return FString::Printf(TEXT("/Body/UbergraphPages/%s/Nodes/%s/PinOverrides/%s"), *GraphName, *NodeId, *PinId);
}

FString LinkPath(const FString& GraphName, const FString& FromNode, const FString& FromPin, const FString& ToNode, const FString& ToPin)
{
	return FString::Printf(
		TEXT("/Body/UbergraphPages/%s/Links/%s:%s->%s:%s"),
		*GraphName,
		*FromNode,
		*FromPin,
		*ToNode,
		*ToPin);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphValidationAcceptsTier1ShapeBeforeApplyTest,
	"UEAssetDoc.UBlueprint.GraphValidation.AcceptsTier1ShapeBeforeApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphValidationAcceptsTier1ShapeBeforeApplyTest::RunTest(const FString&)
{
	const FUBlueprintAssetDocumentCapability Capability;
	FAssetDocumentCapabilityContext Context;
	Context.AssetClass = UBlueprint::StaticClass();

	TSharedRef<FJsonObject> Body = MakeBodyWithRegion(
		TEXT("UbergraphPages"),
		{MakeEventGraph({
			UBlueprintGraphTestMakeGraphNode(
				TEXT("BeginPlay"),
				TEXT("/Script/BlueprintGraph.K2Node_Event"),
				MakeMemberRef(TEXT("/Script/Engine.Actor"), TEXT("ReceiveBeginPlay")))
		})});

	const FAssetDocumentCapabilityResult Result = Capability.Validate(Context, MakeUBlueprintGraphBodyValue(Body));
	TestTrue(TEXT("Tier 1 event graph shape validates before graph apply exists"), Result.bSuccess);
	TestFalse(TEXT("Graph shape is not rejected by old protected-region guard"), UBlueprintGraphResultHasDiagnosticCode(Result, TEXT("UnsupportedUBlueprintRegion")));
	TestFalse(TEXT("Tier 1 event adapter is registered"), UBlueprintGraphResultHasDiagnostic(Result, TEXT("/Body/UbergraphPages/0/Nodes/0"), TEXT("UnsupportedGraphNodeClass")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphValidationPreservesMultipleParserDiagnosticsTest,
	"UEAssetDoc.UBlueprint.GraphValidation.PreservesMultipleParserDiagnostics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphValidationPreservesMultipleParserDiagnosticsTest::RunTest(const FString&)
{
	const FUBlueprintAssetDocumentCapability Capability;
	FAssetDocumentCapabilityContext Context;
	Context.AssetClass = UBlueprint::StaticClass();

	TSharedRef<FJsonObject> InvalidGraph = MakeShared<FJsonObject>();
	InvalidGraph->SetStringField(TEXT("Unexpected"), TEXT("value"));
	InvalidGraph->SetArrayField(TEXT("Nodes"), {});
	InvalidGraph->SetArrayField(TEXT("Links"), {});

	TSharedRef<FJsonObject> Body = MakeBodyWithRegion(TEXT("UbergraphPages"), {InvalidGraph});
	const FAssetDocumentCapabilityResult Result = Capability.Validate(Context, MakeUBlueprintGraphBodyValue(Body));
	TestFalse(TEXT("Invalid graph shape fails validation"), Result.bSuccess);
	TestTrue(TEXT("All graph parser diagnostics are preserved"), Result.Diagnostics.Num() >= 2);
	TestTrue(TEXT("MissingGraphName diagnostic is preserved"), UBlueprintGraphResultHasDiagnosticCode(Result, TEXT("MissingGraphName")));
	TestTrue(TEXT("MissingGraphSchema diagnostic is preserved"), UBlueprintGraphResultHasDiagnosticCode(Result, TEXT("MissingGraphSchema")));
	TestTrue(TEXT("UnknownGraphField diagnostic is preserved"), UBlueprintGraphResultHasDiagnosticCode(Result, TEXT("UnknownGraphField")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphValidationRejectsInvalidFunctionGraphsTest,
	"UEAssetDoc.UBlueprint.GraphValidation.RejectsInvalidFunctionGraphs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphValidationRejectsInvalidFunctionGraphsTest::RunTest(const FString&)
{
	const FUBlueprintAssetDocumentCapability Capability;
	FAssetDocumentCapabilityContext Context;
	Context.AssetClass = UBlueprint::StaticClass();

	const FAssetDocumentCapabilityResult Result = Capability.Validate(
		Context,
		MakeUBlueprintGraphBodyValue(MakeBodyWithRegion(TEXT("FunctionGraphs"), {MakeShared<FJsonObject>()})));
	TestFalse(TEXT("FunctionGraphs rejects invalid graph shape"), Result.bSuccess);
	TestTrue(TEXT("FunctionGraphs reports graph parser diagnostic"), UBlueprintGraphResultHasDiagnosticCode(Result, TEXT("MissingGraphName")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphValidationRejectsInvalidMacroGraphsTest,
	"UEAssetDoc.UBlueprint.GraphValidation.RejectsInvalidMacroGraphs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphValidationRejectsInvalidMacroGraphsTest::RunTest(const FString&)
{
	const FUBlueprintAssetDocumentCapability Capability;
	FAssetDocumentCapabilityContext Context;
	Context.AssetClass = UBlueprint::StaticClass();

	const FAssetDocumentCapabilityResult Result = Capability.Validate(
		Context,
		MakeUBlueprintGraphBodyValue(MakeBodyWithRegion(TEXT("MacroGraphs"), {MakeShared<FJsonObject>()})));
	TestFalse(TEXT("MacroGraphs rejects invalid graph shape"), Result.bSuccess);
	TestTrue(TEXT("MacroGraphs reports graph parser diagnostic"), UBlueprintGraphResultHasDiagnosticCode(Result, TEXT("MissingGraphName")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphValidationRejectsTimelinesUntilImplementedTest,
	"UEAssetDoc.UBlueprint.GraphValidation.RejectsTimelinesUntilImplemented",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphValidationRejectsTimelinesUntilImplementedTest::RunTest(const FString&)
{
	const FUBlueprintAssetDocumentCapability Capability;
	FAssetDocumentCapabilityContext Context;
	Context.AssetClass = UBlueprint::StaticClass();

	const FAssetDocumentCapabilityResult Result = Capability.Validate(
		Context,
		MakeUBlueprintGraphBodyValue(MakeBodyWithRegion(TEXT("Timelines"), {MakeShared<FJsonObject>()})));
	TestFalse(TEXT("Timelines remains unsupported"), Result.bSuccess);
	TestTrue(TEXT("Timelines diagnostic remains protected"), UBlueprintGraphResultHasDiagnostic(Result, TEXT("/Body/Timelines"), TEXT("UnsupportedUBlueprintRegion")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphValidationUnsupportedNodeHasActionableDiagnosticTest,
	"UEAssetDoc.UBlueprint.GraphValidation.UnsupportedNodeHasActionableDiagnostic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphValidationUnsupportedNodeHasActionableDiagnosticTest::RunTest(const FString&)
{
	const FUBlueprintAssetDocumentCapability Capability;
	FAssetDocumentCapabilityContext Context;
	Context.AssetClass = UBlueprint::StaticClass();

	TSharedRef<FJsonObject> Body = MakeBodyWithRegion(
		TEXT("UbergraphPages"),
		{MakeEventGraph({
			UBlueprintGraphTestMakeGraphNode(TEXT("Branch"), TEXT("/Script/BlueprintGraph.K2Node_IfThenElse"))
		})});

	const FAssetDocumentCapabilityResult Result = Capability.Validate(Context, MakeUBlueprintGraphBodyValue(Body));
	TestFalse(TEXT("Unsupported node class fails validation"), Result.bSuccess);
	TestTrue(TEXT("Primary diagnostic uses unsupported node class code"), UBlueprintGraphResultHasDiagnostic(Result, TEXT("/Body/UbergraphPages/0/Nodes/0"), TEXT("UnsupportedGraphNodeClass")));

	const TSharedPtr<FJsonObject> Fallback = GetFirstUnsupportedGraphDiagnostic(Result);
	TestTrue(TEXT("Fallback payload has actionable fields"), FallbackHasActionableFields(Fallback));
	if (Fallback.IsValid())
	{
		TestEqual(TEXT("Fallback code"), Fallback->GetStringField(TEXT("Code")), FString(TEXT("UnsupportedGraphNodeClass")));
		TestEqual(TEXT("Fallback path"), Fallback->GetStringField(TEXT("Path")), FString(TEXT("/Body/UbergraphPages/0/Nodes/0")));
		TestEqual(TEXT("Fallback class"), Fallback->GetStringField(TEXT("Class")), FString(TEXT("/Script/BlueprintGraph.K2Node_IfThenElse")));
		TestTrue(TEXT("Fallback reason mentions adapter"), Fallback->GetStringField(TEXT("Reason")).Contains(TEXT("adapter")));
		TestFalse(TEXT("Fallback suggested action is not empty"), Fallback->GetStringField(TEXT("SuggestedAction")).IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphValidationMultipleUnsupportedNodesHaveActionableDiagnosticsTest,
	"UEAssetDoc.UBlueprint.GraphValidation.MultipleUnsupportedNodesHaveActionableDiagnostics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphValidationMultipleUnsupportedNodesHaveActionableDiagnosticsTest::RunTest(const FString&)
{
	const FUBlueprintAssetDocumentCapability Capability;
	FAssetDocumentCapabilityContext Context;
	Context.AssetClass = UBlueprint::StaticClass();

	TSharedRef<FJsonObject> Body = MakeBodyWithRegion(
		TEXT("UbergraphPages"),
		{MakeEventGraph({
			UBlueprintGraphTestMakeGraphNode(TEXT("BranchA"), TEXT("/Script/BlueprintGraph.K2Node_IfThenElse")),
			UBlueprintGraphTestMakeGraphNode(TEXT("BranchB"), TEXT("/Script/BlueprintGraph.K2Node_IfThenElse"))
		})});

	const FAssetDocumentCapabilityResult Result = Capability.Validate(Context, MakeUBlueprintGraphBodyValue(Body));
	TestFalse(TEXT("Unsupported node classes fail validation"), Result.bSuccess);
	TestTrue(TEXT("First unsupported node diagnostic is preserved"), UBlueprintGraphResultHasDiagnostic(Result, TEXT("/Body/UbergraphPages/0/Nodes/0"), TEXT("UnsupportedGraphNodeClass")));
	TestTrue(TEXT("Second unsupported node diagnostic is preserved"), UBlueprintGraphResultHasDiagnostic(Result, TEXT("/Body/UbergraphPages/0/Nodes/1"), TEXT("UnsupportedGraphNodeClass")));
	TestEqual(TEXT("Fallback payload includes both unsupported nodes"), GetUnsupportedGraphDiagnosticCount(Result), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphValidationSupportsTier1CallFunctionTest,
	"UEAssetDoc.UBlueprint.GraphValidation.SupportsTier1CallFunction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphValidationSupportsTier1CallFunctionTest::RunTest(const FString&)
{
	const FUBlueprintAssetDocumentCapability Capability;
	FAssetDocumentCapabilityContext Context;
	Context.Asset = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphValidationCallFunction"));
	Context.AssetClass = UBlueprint::StaticClass();

	TSharedRef<FJsonObject> Body = MakeBodyWithRegion(
		TEXT("UbergraphPages"),
		{MakeEventGraph({
			UBlueprintGraphTestMakeGraphNode(
				TEXT("Print"),
				TEXT("/Script/BlueprintGraph.K2Node_CallFunction"),
				MakeMemberRef(TEXT("/Script/Engine.KismetSystemLibrary"), TEXT("PrintString")))
		})});

	const FAssetDocumentCapabilityResult Result = Capability.Validate(Context, MakeUBlueprintGraphBodyValue(Body));
	TestTrue(TEXT("Tier 1 reflected function validates"), Result.bSuccess);
	TestFalse(TEXT("Reflected function is no longer current-tier unsupported"), UBlueprintGraphResultHasDiagnostic(Result, TEXT("/Body/UbergraphPages/0/Nodes/0"), TEXT("UnsupportedGraphFunction")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphValidationUnresolvedFunctionHasActionableDiagnosticTest,
	"UEAssetDoc.UBlueprint.GraphValidation.UnresolvedFunctionHasActionableDiagnostic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphValidationUnresolvedFunctionHasActionableDiagnosticTest::RunTest(const FString&)
{
	const FUBlueprintAssetDocumentCapability Capability;
	FAssetDocumentCapabilityContext Context;
	Context.Asset = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphValidationMissingFunction"));
	Context.AssetClass = UBlueprint::StaticClass();

	TSharedRef<FJsonObject> Body = MakeBodyWithRegion(
		TEXT("UbergraphPages"),
		{MakeEventGraph({
			UBlueprintGraphTestMakeGraphNode(
				TEXT("MissingFunction"),
				TEXT("/Script/BlueprintGraph.K2Node_CallFunction"),
				MakeMemberRef(TEXT("/Script/Engine.KismetSystemLibrary"), TEXT("FunctionThatDoesNotExist")))
		})});

	const FAssetDocumentCapabilityResult Result = Capability.Validate(Context, MakeUBlueprintGraphBodyValue(Body));
	TestFalse(TEXT("Unresolved function member fails validation"), Result.bSuccess);
	TestTrue(
		TEXT("Unresolved function uses precise diagnostic code"),
		UBlueprintGraphResultHasDiagnostic(Result, UBlueprintGraphNodePath(TEXT("EventGraph"), TEXT("MissingFunction")) / TEXT("Member"), TEXT("UnresolvedGraphFunction")));
	TestFalse(TEXT("Unresolved function is not treated as unsupported node class"), UBlueprintGraphResultHasDiagnosticCode(Result, TEXT("UnsupportedGraphNodeClass")));
	TestTrue(TEXT("Unresolved function diagnostic mentions MemberRef"), Result.Message.Contains(TEXT("MemberRef")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphExtractExtractsBeginPlayPrintStringTest,
	"UEAssetDoc.UBlueprint.GraphExtract.ExtractsBeginPlayPrintString",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphExtractExtractsBeginPlayPrintStringTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphExtractBeginPlayPrint"));
	UEdGraph* Graph = GetEventGraph(Blueprint);
	TestNotNull(TEXT("Transient actor Blueprint has an EventGraph"), Graph);
	if (!Graph)
	{
		return false;
	}

	UK2Node_Event* BeginPlay = AddBeginPlayNode(Graph);
	UK2Node_CallFunction* Print = AddPrintStringNode(Graph, 320, 0, TEXT("Hello from extract"));
	TestTrue(TEXT("BeginPlay links to PrintString"), LinkPins(BeginPlay ? BeginPlay->FindPin(UEdGraphSchema_K2::PN_Then) : nullptr, Print ? Print->FindPin(UEdGraphSchema_K2::PN_Execute) : nullptr));

	FAssetDocumentCapabilityResult ExtractResult;
	const TSharedRef<FJsonObject> Body = ExtractBlueprintBody(Blueprint, ExtractResult);
	TestTrue(TEXT("Graph extract succeeds"), ExtractResult.bSuccess);

	const TArray<TSharedPtr<FJsonValue>>* Graphs = GetUbergraphPages(Body);
	TestTrue(TEXT("Extract writes Body.UbergraphPages"), Graphs && Graphs->Num() == 1);
	if (!Graphs || Graphs->IsEmpty())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> ExtractedGraph = (*Graphs)[0]->AsObject();
	TestEqual(TEXT("Graph name"), ExtractedGraph->GetStringField(TEXT("Name")), FString(TEXT("EventGraph")));
	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Links = nullptr;
	TestTrue(TEXT("Graph has nodes"), ExtractedGraph->TryGetArrayField(TEXT("Nodes"), Nodes) && Nodes && Nodes->Num() >= 2);
	TestTrue(TEXT("Graph has one expanded link"), ExtractedGraph->TryGetArrayField(TEXT("Links"), Links) && Links && Links->Num() == 1);
	TestTrue(TEXT("BeginPlay node extracted"), FindNodeByMemberName(*Nodes, TEXT("ReceiveBeginPlay")).IsValid());
	const TSharedPtr<FJsonObject> PrintNode = FindNodeByMemberName(*Nodes, TEXT("PrintString"));
	TestTrue(TEXT("PrintString node extracted"), PrintNode.IsValid());
	TestTrue(TEXT("PrintString input default is sparse override"), NodeHasSparsePinOverride(PrintNode, TEXT("InString"), TEXT("Hello from extract")));

	if (Links && Links->Num() == 1)
	{
		const TSharedPtr<FJsonObject> Link = (*Links)[0]->AsObject();
		const TSharedPtr<FJsonObject>* From = nullptr;
		const TSharedPtr<FJsonObject>* To = nullptr;
		TestTrue(TEXT("Link From is expanded object"), Link->TryGetObjectField(TEXT("From"), From) && From && From->IsValid());
		TestTrue(TEXT("Link To is expanded object"), Link->TryGetObjectField(TEXT("To"), To) && To && To->IsValid());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphExtractExtractsVariableGetSetAndSelfTest,
	"UEAssetDoc.UBlueprint.GraphExtract.ExtractsVariableGetSetAndSelf",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphExtractExtractsVariableGetSetAndSelfTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphExtractVars"));
	UEdGraph* Graph = GetEventGraph(Blueprint);
	TestNotNull(TEXT("Transient actor Blueprint has an EventGraph"), Graph);
	if (!Blueprint || !Graph)
	{
		return false;
	}

	FEdGraphPinType IntPinType;
	IntPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
	TestTrue(TEXT("Blueprint variable is added through UE API"), FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("GraphCounter"), IntPinType, TEXT("0")));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	AddVariableGetNode(Blueprint, Graph, TEXT("GraphCounter"), 0, 160);
	AddVariableSetNode(Blueprint, Graph, TEXT("GraphCounter"), 320, 160);
	AddSelfNode(Graph, 0, 320);

	FAssetDocumentCapabilityResult ExtractResult;
	const TSharedRef<FJsonObject> Body = ExtractBlueprintBody(Blueprint, ExtractResult);
	TestTrue(TEXT("Graph extract succeeds"), ExtractResult.bSuccess);
	const TArray<TSharedPtr<FJsonValue>>* Graphs = GetUbergraphPages(Body);
	TestTrue(TEXT("Extract writes Body.UbergraphPages"), Graphs && Graphs->Num() == 1);
	if (!Graphs || Graphs->IsEmpty())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> ExtractedGraph = (*Graphs)[0]->AsObject();
	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	TestTrue(TEXT("Graph has nodes"), ExtractedGraph->TryGetArrayField(TEXT("Nodes"), Nodes) && Nodes && Nodes->Num() >= 3);
	TestTrue(TEXT("Variable get extracted"), FindNodeByClass(*Nodes, TEXT("/Script/BlueprintGraph.K2Node_VariableGet")).IsValid());
	TestTrue(TEXT("Variable set extracted"), FindNodeByClass(*Nodes, TEXT("/Script/BlueprintGraph.K2Node_VariableSet")).IsValid());
	const TSharedPtr<FJsonObject> SelfNode = FindNodeByClass(*Nodes, TEXT("/Script/BlueprintGraph.K2Node_Self"));
	TestTrue(TEXT("Self node extracted"), SelfNode.IsValid());
	TestFalse(TEXT("Self node has no MemberRef"), SelfNode.IsValid() && SelfNode->HasField(TEXT("Member")));
	TestTrue(TEXT("Variable member ref extracted"), FindNodeByMemberName(*Nodes, TEXT("GraphCounter")).IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphExtractSkipsStaleVariableNodesTest,
	"UEAssetDoc.UBlueprint.GraphExtract.SkipsStaleVariableNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphExtractSkipsStaleVariableNodesTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphExtractStaleVars"));
	UEdGraph* Graph = GetEventGraph(Blueprint);
	TestNotNull(TEXT("Transient actor Blueprint has an EventGraph"), Graph);
	if (!Blueprint || !Graph)
	{
		return false;
	}

	AddVariableGetNode(Blueprint, Graph, TEXT("DeletedCounter"), 0, 160);
	AddVariableSetNode(Blueprint, Graph, TEXT("DeletedCounter"), 320, 160);

	FAssetDocumentCapabilityResult ExtractResult;
	const TSharedRef<FJsonObject> Body = ExtractBlueprintBody(Blueprint, ExtractResult);
	TestTrue(TEXT("Graph extract succeeds with skipped stale variables"), ExtractResult.bSuccess);

	const TArray<TSharedPtr<FJsonValue>>* Graphs = GetUbergraphPages(Body);
	if (Graphs && !Graphs->IsEmpty())
	{
		const TSharedPtr<FJsonObject> ExtractedGraph = (*Graphs)[0]->AsObject();
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		TestTrue(TEXT("Graph nodes field is readable"), ExtractedGraph->TryGetArrayField(TEXT("Nodes"), Nodes) && Nodes);
		TestFalse(TEXT("Stale variable is not emitted as lossy MemberRef"), Nodes && FindNodeByMemberName(*Nodes, TEXT("DeletedCounter")).IsValid());
	}

	TestTrue(TEXT("Skipped graph evidence names stale variable get"), SkippedGraphsContainClass(Body, TEXT("/Script/BlueprintGraph.K2Node_VariableGet")));
	TestTrue(TEXT("Skipped graph evidence names stale variable set"), SkippedGraphsContainClass(Body, TEXT("/Script/BlueprintGraph.K2Node_VariableSet")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphExtractSkipsUnsupportedExistingNodesTest,
	"UEAssetDoc.UBlueprint.GraphExtract.SkipsUnsupportedExistingNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphExtractSkipsUnsupportedExistingNodesTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphExtractUnsupported"));
	UEdGraph* Graph = GetEventGraph(Blueprint);
	TestNotNull(TEXT("Transient actor Blueprint has an EventGraph"), Graph);
	if (!Graph)
	{
		return false;
	}

	AddBeginPlayNode(Graph);
	AddUBlueprintGraphUnsupportedBranchNode(Graph, 320, 0);

	FAssetDocumentCapabilityResult ExtractResult;
	const TSharedRef<FJsonObject> Body = ExtractBlueprintBody(Blueprint, ExtractResult);
	TestTrue(TEXT("Graph extract succeeds with skipped evidence"), ExtractResult.bSuccess);
	const TArray<TSharedPtr<FJsonValue>>* Graphs = GetUbergraphPages(Body);
	TestTrue(TEXT("Supported graph still extracted"), Graphs && Graphs->Num() == 1);
	if (Graphs && !Graphs->IsEmpty())
	{
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		const TSharedPtr<FJsonObject> ExtractedGraph = (*Graphs)[0]->AsObject();
		TestTrue(TEXT("Supported node is emitted"), ExtractedGraph->TryGetArrayField(TEXT("Nodes"), Nodes) && Nodes && FindNodeByMemberName(*Nodes, TEXT("ReceiveBeginPlay")).IsValid());
		TestFalse(TEXT("Unsupported node is not emitted as lossy NodeSpec"), Nodes && FindNodeByClass(*Nodes, TEXT("/Script/BlueprintGraph.K2Node_IfThenElse")).IsValid());
	}
	TestTrue(TEXT("Skipped graph evidence names unsupported class"), SkippedGraphsContainClass(Body, TEXT("/Script/BlueprintGraph.K2Node_IfThenElse")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphExtractKeepsPinOverridesSparseTest,
	"UEAssetDoc.UBlueprint.GraphExtract.KeepsPinOverridesSparse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphExtractKeepsPinOverridesSparseTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphExtractSparsePins"));
	UEdGraph* Graph = GetEventGraph(Blueprint);
	TestNotNull(TEXT("Transient actor Blueprint has an EventGraph"), Graph);
	if (!Graph)
	{
		return false;
	}

	AddPrintStringNode(Graph, 320, 0);

	FAssetDocumentCapabilityResult ExtractResult;
	const TSharedRef<FJsonObject> Body = ExtractBlueprintBody(Blueprint, ExtractResult);
	TestTrue(TEXT("Graph extract succeeds"), ExtractResult.bSuccess);
	const TArray<TSharedPtr<FJsonValue>>* Graphs = GetUbergraphPages(Body);
	TestTrue(TEXT("Extract writes Body.UbergraphPages"), Graphs && Graphs->Num() == 1);
	if (!Graphs || Graphs->IsEmpty())
	{
		return false;
	}

	const TSharedPtr<FJsonObject> ExtractedGraph = (*Graphs)[0]->AsObject();
	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	TestTrue(TEXT("Graph has nodes"), ExtractedGraph->TryGetArrayField(TEXT("Nodes"), Nodes) && Nodes && Nodes->Num() >= 1);
	const TSharedPtr<FJsonObject> PrintNode = Nodes ? FindNodeByMemberName(*Nodes, TEXT("PrintString")) : nullptr;
	TestTrue(TEXT("PrintString node extracted"), PrintNode.IsValid());
	TestFalse(TEXT("Baseline/default pins are omitted from sparse PinOverrides"), PrintNode.IsValid() && PrintNode->HasField(TEXT("PinOverrides")));
	TestFalse(
		TEXT("Invalid transient node GUID is omitted instead of serialized as a zero identity"),
		PrintNode.IsValid() && PrintNode->HasField(TEXT("NodeGuid")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphDiffUnchangedAfterExtractTest,
	"UEAssetDoc.UBlueprint.GraphDiff.UnchangedAfterExtract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphDiffUnchangedAfterExtractTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphDiffUnchanged"));
	UEdGraph* Graph = GetEventGraph(Blueprint);
	TestNotNull(TEXT("Transient actor Blueprint has an EventGraph"), Graph);
	if (!Graph)
	{
		return false;
	}

	UK2Node_Event* BeginPlay = AddBeginPlayNode(Graph);
	UK2Node_CallFunction* Print = AddPrintStringNode(Graph, 320, 0, TEXT("Hello"));
	TestTrue(TEXT("BeginPlay links to PrintString"), LinkPins(BeginPlay ? BeginPlay->FindPin(UEdGraphSchema_K2::PN_Then) : nullptr, Print ? Print->FindPin(UEdGraphSchema_K2::PN_Execute) : nullptr));

	FAssetDocumentCapabilityResult ExtractResult;
	const TSharedRef<FJsonObject> DesiredBody = ExtractBlueprintBody(Blueprint, ExtractResult);
	TestTrue(TEXT("Graph extract succeeds"), ExtractResult.bSuccess);
	RemoveExtractOnlyEvidence(DesiredBody);

	TArray<TSharedPtr<FJsonValue>> DiffEntries;
	const FAssetDocumentCapabilityResult DiffResult = DiffBlueprintBody(Blueprint, DesiredBody, DiffEntries);
	TestTrue(TEXT("Graph diff succeeds"), DiffResult.bSuccess);

	const TSharedPtr<FJsonObject> GraphDiff = UBlueprintGraphTestFindDiffEntryByPath(DiffEntries, GraphPath(TEXT("EventGraph")));
	TestTrue(TEXT("Graph diff includes EventGraph"), GraphDiff.IsValid());
	if (GraphDiff.IsValid())
	{
		TestEqual(TEXT("EventGraph diff is unchanged"), GraphDiff->GetStringField(TEXT("status")), FString(TEXT("unchanged")));
	}
	TestFalse(TEXT("Extracted graph desired has no changed graph diff entries"), HasNonUnchangedGraphDiffEntry(DiffEntries));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphDiffReportsMissingNodeTest,
	"UEAssetDoc.UBlueprint.GraphDiff.ReportsMissingNode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphDiffReportsMissingNodeTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphDiffMissingNode"));
	UEdGraph* Graph = GetEventGraph(Blueprint);
	TestNotNull(TEXT("Transient actor Blueprint has an EventGraph"), Graph);
	if (!Graph)
	{
		return false;
	}
	AddBeginPlayNode(Graph);

	FAssetDocumentCapabilityResult ExtractResult;
	const TSharedRef<FJsonObject> DesiredBody = ExtractBlueprintBody(Blueprint, ExtractResult);
	TestTrue(TEXT("Graph extract succeeds"), ExtractResult.bSuccess);
	RemoveExtractOnlyEvidence(DesiredBody);
	TSharedPtr<FJsonObject> DesiredGraph = FindGraphJsonByName(DesiredBody, TEXT("EventGraph"));
	TestTrue(TEXT("Desired graph exists"), DesiredGraph.IsValid());
	if (!DesiredGraph.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* ExistingNodes = nullptr;
	TestTrue(TEXT("Desired graph has nodes"), DesiredGraph->TryGetArrayField(TEXT("Nodes"), ExistingNodes) && ExistingNodes);
	TArray<TSharedPtr<FJsonValue>> Nodes = ExistingNodes ? *ExistingNodes : TArray<TSharedPtr<FJsonValue>>();
	TSharedRef<FJsonObject> DesiredPrint = UBlueprintGraphTestMakeGraphNode(
		TEXT("PrintString"),
		TEXT("/Script/BlueprintGraph.K2Node_CallFunction"),
		MakeMemberRef(TEXT("/Script/Engine.KismetSystemLibrary"), TEXT("PrintString")));
	Nodes.Add(MakeShared<FJsonValueObject>(DesiredPrint));
	DesiredGraph->SetArrayField(TEXT("Nodes"), MoveTemp(Nodes));

	TArray<TSharedPtr<FJsonValue>> DiffEntries;
	const FAssetDocumentCapabilityResult DiffResult = DiffBlueprintBody(Blueprint, DesiredBody, DiffEntries);
	TestTrue(TEXT("Graph diff succeeds"), DiffResult.bSuccess);
	const TSharedPtr<FJsonObject> MissingNode = UBlueprintGraphTestFindDiffEntryByPath(DiffEntries, UBlueprintGraphNodePath(TEXT("EventGraph"), TEXT("PrintString")));
	TestTrue(TEXT("Diff reports missing PrintString node"), MissingNode.IsValid());
	if (MissingNode.IsValid())
	{
		TestEqual(TEXT("Missing node uses public changed status"), MissingNode->GetStringField(TEXT("status")), FString(TEXT("changed")));
		TestEqual(TEXT("Missing node keeps semantic change"), MissingNode->GetStringField(TEXT("change")), FString(TEXT("missing")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphDiffReportsExtraNodeTest,
	"UEAssetDoc.UBlueprint.GraphDiff.ReportsExtraNode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphDiffReportsExtraNodeTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphDiffExtraNode"));
	UEdGraph* Graph = GetEventGraph(Blueprint);
	TestNotNull(TEXT("Transient actor Blueprint has an EventGraph"), Graph);
	if (!Graph)
	{
		return false;
	}
	AddBeginPlayNode(Graph);
	AddPrintStringNode(Graph, 320, 0);

	FAssetDocumentCapabilityResult ExtractResult;
	const TSharedRef<FJsonObject> DesiredBody = ExtractBlueprintBody(Blueprint, ExtractResult);
	TestTrue(TEXT("Graph extract succeeds"), ExtractResult.bSuccess);
	RemoveExtractOnlyEvidence(DesiredBody);
	const TSharedPtr<FJsonObject> DesiredGraph = FindGraphJsonByName(DesiredBody, TEXT("EventGraph"));
	const FString PrintNodeId = GetNodeIdByMemberName(DesiredGraph, TEXT("PrintString"));
	TestFalse(TEXT("Extracted PrintString node id exists"), PrintNodeId.IsEmpty());
	TestTrue(TEXT("Desired graph omits PrintString node"), RemoveNodeByMemberName(DesiredGraph, TEXT("PrintString")));

	TArray<TSharedPtr<FJsonValue>> DiffEntries;
	const FAssetDocumentCapabilityResult DiffResult = DiffBlueprintBody(Blueprint, DesiredBody, DiffEntries);
	TestTrue(TEXT("Graph diff succeeds"), DiffResult.bSuccess);
	const TSharedPtr<FJsonObject> ExtraNode = UBlueprintGraphTestFindDiffEntryByPath(DiffEntries, UBlueprintGraphNodePath(TEXT("EventGraph"), PrintNodeId));
	TestTrue(TEXT("Diff reports extra PrintString node"), ExtraNode.IsValid());
	if (ExtraNode.IsValid())
	{
		TestEqual(TEXT("Extra node uses public changed status"), ExtraNode->GetStringField(TEXT("status")), FString(TEXT("changed")));
		TestEqual(TEXT("Extra node keeps semantic change"), ExtraNode->GetStringField(TEXT("change")), FString(TEXT("extra")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphDiffReportsChangedPinDefaultTest,
	"UEAssetDoc.UBlueprint.GraphDiff.ReportsChangedPinDefault",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphDiffReportsChangedPinDefaultTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphDiffChangedPin"));
	UEdGraph* Graph = GetEventGraph(Blueprint);
	TestNotNull(TEXT("Transient actor Blueprint has an EventGraph"), Graph);
	if (!Graph)
	{
		return false;
	}
	AddPrintStringNode(Graph, 320, 0, TEXT("Current"));

	FAssetDocumentCapabilityResult ExtractResult;
	const TSharedRef<FJsonObject> DesiredBody = ExtractBlueprintBody(Blueprint, ExtractResult);
	TestTrue(TEXT("Graph extract succeeds"), ExtractResult.bSuccess);
	RemoveExtractOnlyEvidence(DesiredBody);
	const TSharedPtr<FJsonObject> DesiredGraph = FindGraphJsonByName(DesiredBody, TEXT("EventGraph"));
	const FString PrintNodeId = GetNodeIdByMemberName(DesiredGraph, TEXT("PrintString"));
	TestFalse(TEXT("Extracted PrintString node id exists"), PrintNodeId.IsEmpty());
	TestTrue(TEXT("Desired pin default is edited"), SetPinDefaultForMemberNode(DesiredGraph, TEXT("PrintString"), TEXT("InString"), TEXT("Desired")));

	TArray<TSharedPtr<FJsonValue>> DiffEntries;
	const FAssetDocumentCapabilityResult DiffResult = DiffBlueprintBody(Blueprint, DesiredBody, DiffEntries);
	TestTrue(TEXT("Graph diff succeeds"), DiffResult.bSuccess);
	const TSharedPtr<FJsonObject> ChangedPin = UBlueprintGraphTestFindDiffEntryByPath(DiffEntries, PinPath(TEXT("EventGraph"), PrintNodeId, TEXT("InString")));
	TestTrue(TEXT("Diff reports changed InString default"), ChangedPin.IsValid());
	if (ChangedPin.IsValid())
	{
		TestEqual(TEXT("Changed pin status"), ChangedPin->GetStringField(TEXT("status")), FString(TEXT("changed")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphDiffReportsMissingLinkTest,
	"UEAssetDoc.UBlueprint.GraphDiff.ReportsMissingLink",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphDiffReportsMissingLinkTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphDiffMissingLink"));
	UEdGraph* Graph = GetEventGraph(Blueprint);
	TestNotNull(TEXT("Transient actor Blueprint has an EventGraph"), Graph);
	if (!Graph)
	{
		return false;
	}
	AddBeginPlayNode(Graph);
	AddPrintStringNode(Graph, 320, 0);

	FAssetDocumentCapabilityResult ExtractResult;
	const TSharedRef<FJsonObject> DesiredBody = ExtractBlueprintBody(Blueprint, ExtractResult);
	TestTrue(TEXT("Graph extract succeeds"), ExtractResult.bSuccess);
	RemoveExtractOnlyEvidence(DesiredBody);
	const TSharedPtr<FJsonObject> DesiredGraph = FindGraphJsonByName(DesiredBody, TEXT("EventGraph"));
	const FString BeginPlayNodeId = GetNodeIdByMemberName(DesiredGraph, TEXT("ReceiveBeginPlay"));
	const FString PrintNodeId = GetNodeIdByMemberName(DesiredGraph, TEXT("PrintString"));
	TestFalse(TEXT("Extracted BeginPlay node id exists"), BeginPlayNodeId.IsEmpty());
	TestFalse(TEXT("Extracted PrintString node id exists"), PrintNodeId.IsEmpty());
	AddLinkToGraph(DesiredGraph, BeginPlayNodeId, UEdGraphSchema_K2::PN_Then.ToString(), PrintNodeId, UEdGraphSchema_K2::PN_Execute.ToString());

	TArray<TSharedPtr<FJsonValue>> DiffEntries;
	const FAssetDocumentCapabilityResult DiffResult = DiffBlueprintBody(Blueprint, DesiredBody, DiffEntries);
	TestTrue(TEXT("Graph diff succeeds"), DiffResult.bSuccess);
	const TSharedPtr<FJsonObject> MissingLink = UBlueprintGraphTestFindDiffEntryByPath(
		DiffEntries,
		LinkPath(TEXT("EventGraph"), BeginPlayNodeId, UEdGraphSchema_K2::PN_Then.ToString(), PrintNodeId, UEdGraphSchema_K2::PN_Execute.ToString()));
	TestTrue(TEXT("Diff reports missing execution link"), MissingLink.IsValid());
	if (MissingLink.IsValid())
	{
		TestEqual(TEXT("Missing link uses public changed status"), MissingLink->GetStringField(TEXT("status")), FString(TEXT("changed")));
		TestEqual(TEXT("Missing link keeps semantic change"), MissingLink->GetStringField(TEXT("change")), FString(TEXT("missing")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphDiffUsesServiceCompatibleStatusesTest,
	"UEAssetDoc.UBlueprint.GraphDiff.UsesServiceCompatibleStatuses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphDiffUsesServiceCompatibleStatusesTest::RunTest(const FString&)
{
	{
		UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphDiffRoutingMissing"));
		UEdGraph* Graph = GetEventGraph(Blueprint);
		TestNotNull(TEXT("Transient actor Blueprint has an EventGraph"), Graph);
		if (!Graph)
		{
			return false;
		}
		AddBeginPlayNode(Graph);

		FAssetDocumentCapabilityResult ExtractResult;
		const TSharedRef<FJsonObject> DesiredBody = ExtractBlueprintBody(Blueprint, ExtractResult);
		TestTrue(TEXT("Graph extract succeeds"), ExtractResult.bSuccess);
		RemoveExtractOnlyEvidence(DesiredBody);
		const TSharedPtr<FJsonObject> DesiredGraph = FindGraphJsonByName(DesiredBody, TEXT("EventGraph"));
		const TArray<TSharedPtr<FJsonValue>>* ExistingNodes = nullptr;
		TestTrue(TEXT("Desired graph has nodes"), DesiredGraph.IsValid() && DesiredGraph->TryGetArrayField(TEXT("Nodes"), ExistingNodes) && ExistingNodes);
		TArray<TSharedPtr<FJsonValue>> Nodes = ExistingNodes ? *ExistingNodes : TArray<TSharedPtr<FJsonValue>>();
		Nodes.Add(MakeShared<FJsonValueObject>(UBlueprintGraphTestMakeGraphNode(
			TEXT("PrintString"),
			TEXT("/Script/BlueprintGraph.K2Node_CallFunction"),
			MakeMemberRef(TEXT("/Script/Engine.KismetSystemLibrary"), TEXT("PrintString")))));
		DesiredGraph->SetArrayField(TEXT("Nodes"), MoveTemp(Nodes));

		TArray<TSharedPtr<FJsonValue>> DiffEntries;
		const FAssetDocumentCapabilityResult DiffResult = DiffBlueprintBody(Blueprint, DesiredBody, DiffEntries);
		TestTrue(TEXT("Graph diff succeeds"), DiffResult.bSuccess);
		const FDiffRoutingBuckets Routed = RouteLikeAssetDocumentService(DiffEntries);
		TestTrue(TEXT("Missing graph node routes to changed bucket"), Routed.Changed > 0);
		TestEqual(TEXT("Missing graph node does not route to failed bucket"), Routed.Failed, 0);
	}

	{
		UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphDiffRoutingUnsupported"));
		UEdGraph* Graph = GetEventGraph(Blueprint);
		TestNotNull(TEXT("Transient actor Blueprint has an EventGraph"), Graph);
		if (!Graph)
		{
			return false;
		}
		AddUBlueprintGraphUnsupportedBranchNode(Graph, 320, 0);

		TSharedRef<FJsonObject> DesiredBody = MakeShared<FJsonObject>();
		DesiredBody->SetObjectField(TEXT("ParentClass"), MakeUBlueprintGraphActorParentClassRef());

		TArray<TSharedPtr<FJsonValue>> DiffEntries;
		const FAssetDocumentCapabilityResult DiffResult = DiffBlueprintBody(Blueprint, DesiredBody, DiffEntries);
		TestTrue(TEXT("Unsupported graph diff succeeds"), DiffResult.bSuccess);
		const FDiffRoutingBuckets Routed = RouteLikeAssetDocumentService(DiffEntries);
		TestTrue(TEXT("Unsupported graph evidence routes to skipped bucket"), Routed.Skipped > 0);
		TestEqual(TEXT("Unsupported graph evidence does not route to failed bucket"), Routed.Failed, 0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphDiffTreatsDefinitionRefAndInlineMemberRefAsEqualTest,
	"UEAssetDoc.UBlueprint.GraphDiff.TreatsDefinitionRefAndInlineMemberRefAsEqual",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphDiffTreatsDefinitionRefAndInlineMemberRefAsEqualTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphDiffDefinitionRef"));
	UEdGraph* Graph = GetEventGraph(Blueprint);
	TestNotNull(TEXT("Transient actor Blueprint has an EventGraph"), Graph);
	if (!Graph)
	{
		return false;
	}
	AddPrintStringNode(Graph, 320, 0);

	FAssetDocumentCapabilityResult ExtractResult;
	const TSharedRef<FJsonObject> DesiredBody = ExtractBlueprintBody(Blueprint, ExtractResult);
	TestTrue(TEXT("Graph extract succeeds"), ExtractResult.bSuccess);
	RemoveExtractOnlyEvidence(DesiredBody);
	const TSharedPtr<FJsonObject> DesiredGraph = FindGraphJsonByName(DesiredBody, TEXT("EventGraph"));
	const FString PrintNodeId = GetNodeIdByMemberName(DesiredGraph, TEXT("PrintString"));
	TestFalse(TEXT("Extracted PrintString node id exists"), PrintNodeId.IsEmpty());
	TestTrue(TEXT("Desired PrintString member uses DefinitionRef"), ReplaceMemberWithDefinitionRef(DesiredGraph, TEXT("PrintString"), TEXT("Func.KismetSystemLibrary.PrintString")));

	TSharedPtr<FJsonObject> Definitions = MakeShared<FJsonObject>();
	Definitions->SetObjectField(
		TEXT("Func.KismetSystemLibrary.PrintString"),
		MakeMemberRef(TEXT("/Script/Engine.KismetSystemLibrary"), TEXT("PrintString")));

	TArray<TSharedPtr<FJsonValue>> DiffEntries;
	const FAssetDocumentCapabilityResult DiffResult = DiffBlueprintBody(Blueprint, DesiredBody, DiffEntries, &Definitions);
	TestTrue(TEXT("Graph diff succeeds"), DiffResult.bSuccess);
	const TSharedPtr<FJsonObject> PrintNodeDiff = UBlueprintGraphTestFindDiffEntryByPath(DiffEntries, UBlueprintGraphNodePath(TEXT("EventGraph"), PrintNodeId));
	TestTrue(TEXT("Diff includes PrintString node"), PrintNodeDiff.IsValid());
	if (PrintNodeDiff.IsValid())
	{
		TestEqual(TEXT("DefinitionRef and inline MemberRef compare unchanged"), PrintNodeDiff->GetStringField(TEXT("status")), FString(TEXT("unchanged")));
	}
	TestFalse(TEXT("DefinitionRef desired has no changed graph diff entries"), HasNonUnchangedGraphDiffEntry(DiffEntries));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphApplyCreatesBeginPlayPrintStringTest,
	"UEAssetDoc.UBlueprint.GraphApply.CreatesBeginPlayPrintString",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphApplyCreatesBeginPlayPrintStringTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphApplyCreate"));
	TestNotNull(TEXT("Transient actor Blueprint exists"), Blueprint);
	if (!Blueprint)
	{
		return false;
	}

	const FAssetDocumentCapabilityResult ApplyResult = ApplyBlueprintBody(Blueprint, MakeBeginPlayPrintStringBody());
	TestTrue(TEXT("Graph apply succeeds"), ApplyResult.bSuccess);

	UEdGraph* Graph = GetEventGraph(Blueprint);
	TestNotNull(TEXT("EventGraph exists after apply"), Graph);
	if (!Graph)
	{
		return false;
	}

	TestEqual(TEXT("BeginPlay node was created"), CountGraphNodesByMemberName(Graph, TEXT("ReceiveBeginPlay")), 1);
	UK2Node_CallFunction* PrintNode = FindCallFunctionNodeByMemberName(Graph, TEXT("PrintString"));
	TestNotNull(TEXT("PrintString node was created through reflected UFunction lookup"), PrintNode);
	UEdGraphPin* InStringPin = PrintNode ? PrintNode->FindPin(TEXT("InString")) : nullptr;
	TestNotNull(TEXT("PrintString InString pin exists"), InStringPin);
	if (InStringPin)
	{
		TestEqual(TEXT("Authored pin default was applied"), InStringPin->DefaultValue, FString(TEXT("Hello from AssetDocument")));
	}
	TestTrue(TEXT("BeginPlay exec pin links to PrintString execute pin"), GraphHasLink(Graph, UEdGraphSchema_K2::PN_Then, UEdGraphSchema_K2::PN_Execute));
	TestFalse(TEXT("Applied Blueprint compiles without error"), Blueprint->Status == BS_Error);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphApplyUsesSpawnerLifecycleForDevelopmentOnlyCallTest,
	"UEAssetDoc.UBlueprint.GraphApply.UsesSpawnerLifecycleForDevelopmentOnlyCall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphApplyUsesSpawnerLifecycleForDevelopmentOnlyCallTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphApplyDevelopmentOnlySpawner"));
	TestNotNull(TEXT("Transient actor Blueprint exists"), Blueprint);
	if (!Blueprint)
	{
		return false;
	}

	const FAssetDocumentCapabilityResult ApplyResult = ApplyBlueprintBody(Blueprint, MakeBeginPlayPrintStringBody());
	TestTrue(TEXT("Graph apply succeeds"), ApplyResult.bSuccess);
	UK2Node_CallFunction* CallNode = FindCallFunctionNodeByMemberName(GetEventGraph(Blueprint), TEXT("PrintString"));
	TestNotNull(TEXT("PrintString node exists after document apply"), CallNode);
	if (CallNode)
	{
		TestEqual(
			TEXT("DevelopmentOnly metadata is applied by PostPlacedNewNode"),
			CallNode->GetDesiredEnabledState(),
			ENodeEnabledState::DevelopmentOnly);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphApplyPreservesBlueprintVariableGuidThroughSpawnerTest,
	"UEAssetDoc.UBlueprint.GraphApply.PreservesBlueprintVariableGuidThroughSpawner",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphApplyPreservesBlueprintVariableGuidThroughSpawnerTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphApplyVariableGuidSpawner"));
	TestNotNull(TEXT("Transient actor Blueprint exists"), Blueprint);
	if (!Blueprint)
	{
		return false;
	}

	TSharedRef<FJsonObject> Body = MakeBeginPlayPrintStringBody(TEXT("Ignored"), false, false);
	Body->SetArrayField(TEXT("Variables"), MakeJsonArray({MakeIntVariableSpec(TEXT("GraphCounter"), TEXT("7"))}));
	TSharedPtr<FJsonObject> Graph = FindGraphJsonByName(Body, TEXT("EventGraph"));
	const TArray<TSharedPtr<FJsonValue>>* ExistingNodes = nullptr;
	TestTrue(TEXT("Desired graph has nodes"), Graph.IsValid() && Graph->TryGetArrayField(TEXT("Nodes"), ExistingNodes) && ExistingNodes);
	TArray<TSharedPtr<FJsonValue>> Nodes = ExistingNodes ? *ExistingNodes : TArray<TSharedPtr<FJsonValue>>();
	Nodes.Add(MakeShared<FJsonValueObject>(UBlueprintGraphTestMakeGraphNode(
		TEXT("GraphCounterGet"),
		TEXT("/Script/BlueprintGraph.K2Node_VariableGet"),
		MakeMemberRef(TEXT("Self"), TEXT("GraphCounter")))));
	Graph->SetArrayField(TEXT("Nodes"), MoveTemp(Nodes));

	const FAssetDocumentCapabilityResult ApplyResult = ApplyBlueprintBody(Blueprint, Body);
	TestTrue(TEXT("Graph apply succeeds"), ApplyResult.bSuccess);
	const FBPVariableDescription* VariableDescription = Blueprint->NewVariables.FindByPredicate([](const FBPVariableDescription& Variable)
	{
		return Variable.VarName == TEXT("GraphCounter");
	});
	TestNotNull(TEXT("GraphCounter variable exists after document apply"), VariableDescription);
	UK2Node_Variable* VariableNode = FindVariableNodeByMemberName(GetEventGraph(Blueprint), TEXT("GraphCounter"));
	TestNotNull(TEXT("GraphCounter node exists after document apply"), VariableNode);
	if (VariableDescription && VariableNode)
	{
		TestEqual(
			TEXT("Variable spawner preserves the Blueprint variable GUID"),
			VariableNode->VariableReference.GetMemberGuid(),
			VariableDescription->VarGuid);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphApplyReplacesAutomaticallyPlacedGhostEventTest,
	"UEAssetDoc.UBlueprint.GraphApply.ReplacesAutomaticallyPlacedGhostEvent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphApplyReplacesAutomaticallyPlacedGhostEventTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphApplyGhostEventSpawner"));
	UEdGraph* EventGraph = GetEventGraph(Blueprint);
	TestNotNull(TEXT("Transient actor Blueprint has an EventGraph"), EventGraph);
	UK2Node_Event* GhostEvent = AddBeginPlayNode(EventGraph);
	TestNotNull(TEXT("BeginPlay ghost fixture exists"), GhostEvent);
	if (!Blueprint || !EventGraph || !GhostEvent)
	{
		return false;
	}
	GhostEvent->MakeAutomaticallyPlacedGhostNode();
	TestTrue(TEXT("Fixture event is an automatically placed ghost"), GhostEvent->IsAutomaticallyPlacedGhostNode());

	const FAssetDocumentCapabilityResult ApplyResult = ApplyBlueprintBody(
		Blueprint,
		MakeBeginPlayPrintStringBody(TEXT("Ignored"), false, false));
	TestTrue(TEXT("Graph apply succeeds"), ApplyResult.bSuccess);
	EventGraph = GetEventGraph(Blueprint);
	TestEqual(TEXT("Exactly one final BeginPlay event exists"), CountGraphNodesByMemberName(EventGraph, TEXT("ReceiveBeginPlay")), 1);

	UK2Node_Event* EventNode = nullptr;
	if (EventGraph)
	{
		for (UEdGraphNode* Node : EventGraph->Nodes)
		{
			UK2Node_Event* Candidate = Cast<UK2Node_Event>(Node);
			if (Candidate && Candidate->GetFunctionName() == TEXT("ReceiveBeginPlay"))
			{
				EventNode = Candidate;
				break;
			}
		}
	}
	TestNotNull(TEXT("Final BeginPlay event exists"), EventNode);
	if (EventNode)
	{
		TestFalse(TEXT("Automatically placed ghost event is not reused"), EventNode->IsAutomaticallyPlacedGhostNode());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphApplyRejectsDuplicateEventDuringPreflightWithoutMutationTest,
	"UEAssetDoc.UBlueprint.GraphApply.RejectsDuplicateEventDuringPreflightWithoutMutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphApplyRejectsDuplicateEventDuringPreflightWithoutMutationTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphApplyDuplicateEventPreflight"));
	UEdGraph* EventGraph = GetEventGraph(Blueprint);
	TestNotNull(TEXT("Transient actor Blueprint has an EventGraph"), EventGraph);
	if (!Blueprint || !EventGraph)
	{
		return false;
	}
	const FGuid GraphGuidBefore = EventGraph->GraphGuid;
	const int32 NodeCountBefore = EventGraph->Nodes.Num();

	TSharedRef<FJsonObject> DesiredGraph = MakeEventGraph({
		UBlueprintGraphTestMakeGraphNode(
			TEXT("BeginPlay"),
			TEXT("/Script/BlueprintGraph.K2Node_Event"),
			MakeMemberRef(TEXT("/Script/Engine.Actor"), TEXT("ReceiveBeginPlay"))),
		UBlueprintGraphTestMakeGraphNode(
			TEXT("BeginPlayDuplicate"),
			TEXT("/Script/BlueprintGraph.K2Node_Event"),
			MakeMemberRef(TEXT("/Script/Engine.Actor"), TEXT("ReceiveBeginPlay")))
	});
	const FAssetDocumentCapabilityResult ApplyResult = ApplyBlueprintBody(
		Blueprint,
		MakeBodyWithRegion(TEXT("UbergraphPages"), {DesiredGraph}));
	TestFalse(TEXT("Duplicate desired event fails during preflight"), ApplyResult.bSuccess);
	TestTrue(
		TEXT("Duplicate desired event reports the exact diagnostic"),
		UBlueprintGraphResultHasDiagnostic(
			ApplyResult,
			TEXT("/Body/UbergraphPages/EventGraph/Nodes/BeginPlayDuplicate"),
			TEXT("DuplicateGraphEvent")));
	TestTrue(TEXT("Live EventGraph identity is unchanged"), GetEventGraph(Blueprint) == EventGraph);
	TestEqual(TEXT("Live EventGraph guid is unchanged"), EventGraph->GraphGuid, GraphGuidBefore);
	TestEqual(TEXT("Live EventGraph node count is unchanged"), EventGraph->Nodes.Num(), NodeCountBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphApplyRejectsEventOutsideUbergraphBeforeMutationTest,
	"UEAssetDoc.UBlueprint.GraphApply.RejectsEventOutsideUbergraphBeforeMutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphApplyRejectsEventOutsideUbergraphBeforeMutationTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphApplyEventOutsideUbergraph"));
	TestNotNull(TEXT("Transient actor Blueprint exists"), Blueprint);
	if (!Blueprint)
	{
		return false;
	}
	const int32 FunctionGraphCountBefore = Blueprint->FunctionGraphs.Num();
	const double InitialLifeSpanBefore = GetBlueprintInitialLifeSpan(Blueprint);

	TSharedRef<FJsonObject> FunctionGraph = MakeEventGraph({
		UBlueprintGraphTestMakeGraphNode(
			TEXT("BeginPlay"),
			TEXT("/Script/BlueprintGraph.K2Node_Event"),
			MakeMemberRef(TEXT("/Script/Engine.Actor"), TEXT("ReceiveBeginPlay")))
	});
	FunctionGraph->SetStringField(TEXT("Name"), TEXT("BadEventFunction"));
	TSharedRef<FJsonObject> Body = MakeBodyWithRegion(TEXT("FunctionGraphs"), {FunctionGraph});
	SetClassDefaultInitialLifeSpan(Body, 42.0);

	const FAssetDocumentCapabilityResult ApplyResult = ApplyBlueprintBody(Blueprint, Body);
	TestFalse(TEXT("Event outside an Ubergraph fails before mutation"), ApplyResult.bSuccess);
	TestTrue(
		TEXT("Event outside an Ubergraph reports the exact diagnostic"),
		UBlueprintGraphResultHasDiagnostic(
			ApplyResult,
			TEXT("/Body/FunctionGraphs/BadEventFunction/Nodes/BeginPlay/Member"),
			TEXT("UnsupportedGraphEventPlacement")));
	TestEqual(TEXT("No live function graph was added"), Blueprint->FunctionGraphs.Num(), FunctionGraphCountBefore);
	TestEqual(TEXT("Earlier body fields were not mutated"), GetBlueprintInitialLifeSpan(Blueprint), InitialLifeSpanBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphApplyRejectsEventOwnedByDifferentUbergraphTest,
	"UEAssetDoc.UBlueprint.GraphApply.RejectsEventOwnedByDifferentUbergraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphApplyRejectsEventOwnedByDifferentUbergraphTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphApplyEventDifferentUbergraph"));
	UEdGraph* EventGraph = GetEventGraph(Blueprint);
	UK2Node_Event* ExistingEvent = AddBeginPlayNode(EventGraph);
	UEdGraph* OtherGraph = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint,
		TEXT("SecondaryEventGraph"),
		UEdGraph::StaticClass(),
		UEdGraphSchema_K2::StaticClass());
	if (OtherGraph)
	{
		FBlueprintEditorUtils::AddUbergraphPage(Blueprint, OtherGraph);
	}
	TestNotNull(TEXT("Existing BeginPlay event exists in the primary EventGraph"), ExistingEvent);
	TestNotNull(TEXT("Secondary Ubergraph exists"), OtherGraph);
	if (!Blueprint || !EventGraph || !ExistingEvent || !OtherGraph)
	{
		return false;
	}
	const int32 UbergraphCountBefore = Blueprint->UbergraphPages.Num();

	TSharedRef<FJsonObject> DesiredGraph = MakeEventGraph({
		UBlueprintGraphTestMakeGraphNode(
			TEXT("BeginPlay"),
			TEXT("/Script/BlueprintGraph.K2Node_Event"),
			MakeMemberRef(TEXT("/Script/Engine.Actor"), TEXT("ReceiveBeginPlay")))
	});
	DesiredGraph->SetStringField(TEXT("Name"), TEXT("SecondaryEventGraph"));
	const FAssetDocumentCapabilityResult ApplyResult = ApplyBlueprintBody(
		Blueprint,
		MakeBodyWithRegion(TEXT("UbergraphPages"), {DesiredGraph}));
	TestFalse(TEXT("Event owned by another Ubergraph fails preflight"), ApplyResult.bSuccess);
	TestTrue(
		TEXT("Different Ubergraph ownership reports the exact diagnostic"),
		UBlueprintGraphResultHasDiagnostic(
			ApplyResult,
			TEXT("/Body/UbergraphPages/SecondaryEventGraph/Nodes/BeginPlay"),
			TEXT("GraphEventOwnedByDifferentUbergraph")));
	TestEqual(TEXT("Ubergraph array remains unchanged"), Blueprint->UbergraphPages.Num(), UbergraphCountBefore);
	TestTrue(TEXT("Existing event identity remains in the primary graph"), EventGraph->Nodes.Contains(ExistingEvent));
	TestEqual(TEXT("Secondary graph remains empty"), OtherGraph->Nodes.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphApplyUsesSelfContextForInheritedMemberVariableTest,
	"UEAssetDoc.UBlueprint.GraphApply.UsesSelfContextForInheritedMemberVariable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphApplyUsesSelfContextForInheritedMemberVariableTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphApplyInheritedVariableSpawner"));
	TSharedRef<FJsonObject> Body = MakeBeginPlayPrintStringBody(TEXT("Ignored"), false, false);
	TSharedPtr<FJsonObject> Graph = FindGraphJsonByName(Body, TEXT("EventGraph"));
	const TArray<TSharedPtr<FJsonValue>>* ExistingNodes = nullptr;
	TestTrue(TEXT("Desired graph has nodes"), Graph.IsValid() && Graph->TryGetArrayField(TEXT("Nodes"), ExistingNodes) && ExistingNodes);
	TArray<TSharedPtr<FJsonValue>> Nodes = ExistingNodes ? *ExistingNodes : TArray<TSharedPtr<FJsonValue>>();
	Nodes.Add(MakeShared<FJsonValueObject>(UBlueprintGraphTestMakeGraphNode(
		TEXT("InitialLifeSpanGet"),
		TEXT("/Script/BlueprintGraph.K2Node_VariableGet"),
		MakeMemberRef(TEXT("/Script/Engine.Actor"), TEXT("InitialLifeSpan")))));
	Graph->SetArrayField(TEXT("Nodes"), MoveTemp(Nodes));

	const FAssetDocumentCapabilityResult ApplyResult = ApplyBlueprintBody(Blueprint, Body);
	TestTrue(TEXT("Inherited member variable graph apply succeeds"), ApplyResult.bSuccess);
	UK2Node_Variable* VariableNode = FindVariableNodeByMemberName(GetEventGraph(Blueprint), TEXT("InitialLifeSpan"));
	TestNotNull(TEXT("Inherited InitialLifeSpan variable node exists"), VariableNode);
	if (VariableNode)
	{
		TestTrue(TEXT("UE variable spawner selects self context for inherited member"), VariableNode->VariableReference.IsSelfContext());
		TestTrue(
			TEXT("Inherited member resolves to the Actor property"),
			VariableNode->GetPropertyForVariable() == FindFProperty<FProperty>(AActor::StaticClass(), TEXT("InitialLifeSpan")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphApplyDoesNotTreatLocalVariableAsMemberTest,
	"UEAssetDoc.UBlueprint.GraphApply.DoesNotTreatLocalVariableAsMember",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphApplyDoesNotTreatLocalVariableAsMemberTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphApplyLocalVariableScope"));
	UEdGraph* FunctionGraph = AddBlueprintFunctionLocalVariable(Blueprint, TEXT("ComputeValue"), TEXT("LocalCounter"));
	TestNotNull(TEXT("Function local-variable fixture exists"), FunctionGraph);
	if (!Blueprint || !FunctionGraph)
	{
		return false;
	}

	TSharedRef<FJsonObject> Body = MakeBeginPlayPrintStringBody(TEXT("Ignored"), false, false);
	TSharedPtr<FJsonObject> Graph = FindGraphJsonByName(Body, TEXT("EventGraph"));
	const TArray<TSharedPtr<FJsonValue>>* ExistingNodes = nullptr;
	TestTrue(TEXT("Desired graph has nodes"), Graph.IsValid() && Graph->TryGetArrayField(TEXT("Nodes"), ExistingNodes) && ExistingNodes);
	TArray<TSharedPtr<FJsonValue>> Nodes = ExistingNodes ? *ExistingNodes : TArray<TSharedPtr<FJsonValue>>();
	Nodes.Add(MakeShared<FJsonValueObject>(UBlueprintGraphTestMakeGraphNode(
		TEXT("LocalCounterGet"),
		TEXT("/Script/BlueprintGraph.K2Node_VariableGet"),
		MakeMemberRef(TEXT("Self"), TEXT("LocalCounter")))));
	Graph->SetArrayField(TEXT("Nodes"), MoveTemp(Nodes));

	const int32 EventNodeCountBefore = GetEventGraph(Blueprint)->Nodes.Num();
	const FAssetDocumentCapabilityResult ApplyResult = ApplyBlueprintBody(Blueprint, Body);
	TestFalse(TEXT("Function local variable is not silently treated as a Blueprint member"), ApplyResult.bSuccess);
	TestTrue(
		TEXT("Function-scoped name remains unresolved without function identity"),
		UBlueprintGraphResultHasDiagnostic(
			ApplyResult,
			TEXT("/Body/UbergraphPages/EventGraph/Nodes/LocalCounterGet/Member"),
			TEXT("UnresolvedGraphMemberReference")));
	TestFalse(TEXT("No Blueprint member was synthesized for the local"), Blueprint->NewVariables.ContainsByPredicate([](const FBPVariableDescription& Variable)
	{
		return Variable.VarName == TEXT("LocalCounter");
	}));
	TestEqual(TEXT("Live EventGraph remains unchanged"), GetEventGraph(Blueprint)->Nodes.Num(), EventNodeCountBefore);
	TestTrue(TEXT("Local-variable function graph remains owned by the Blueprint"), Blueprint->FunctionGraphs.Contains(FunctionGraph));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphApplyRejectsNonEventFunctionAsEventTest,
	"UEAssetDoc.UBlueprint.GraphApply.RejectsNonEventFunctionAsEvent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphApplyRejectsNonEventFunctionAsEventTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphApplyNonEventAsEvent"));
	UEdGraph* EventGraph = GetEventGraph(Blueprint);
	TestNotNull(TEXT("Transient actor Blueprint has an EventGraph"), EventGraph);
	if (!Blueprint || !EventGraph)
	{
		return false;
	}
	const FGuid GraphGuidBefore = EventGraph->GraphGuid;
	const int32 NodeCountBefore = EventGraph->Nodes.Num();
	const double InitialLifeSpanBefore = GetBlueprintInitialLifeSpan(Blueprint);

	TSharedRef<FJsonObject> Body = MakeBodyWithRegion(
		TEXT("UbergraphPages"),
		{MakeEventGraph({
			UBlueprintGraphTestMakeGraphNode(
				TEXT("GetActorLocationEvent"),
				TEXT("/Script/BlueprintGraph.K2Node_Event"),
				MakeMemberRef(TEXT("/Script/Engine.Actor"), TEXT("K2_GetActorLocation")))
		})});
	SetClassDefaultInitialLifeSpan(Body, 42.0);

	const FAssetDocumentCapabilityResult ApplyResult = ApplyBlueprintBody(Blueprint, Body);
	TestFalse(TEXT("A callable non-event function cannot be materialized as an event"), ApplyResult.bSuccess);
	TestTrue(
		TEXT("Non-event function reports the exact event admission diagnostic"),
		UBlueprintGraphResultHasDiagnostic(
			ApplyResult,
			TEXT("/Body/UbergraphPages/EventGraph/Nodes/GetActorLocationEvent/Member"),
			TEXT("UnsupportedGraphEvent")));
	UEdGraph* EventGraphAfter = GetEventGraph(Blueprint);
	const bool bSameEventGraph = EventGraphAfter == EventGraph;
	TestTrue(TEXT("Live EventGraph identity is unchanged"), bSameEventGraph);
	if (bSameEventGraph)
	{
		TestEqual(TEXT("Live EventGraph guid is unchanged"), EventGraphAfter->GraphGuid, GraphGuidBefore);
		TestEqual(TEXT("Live EventGraph node count is unchanged"), EventGraphAfter->Nodes.Num(), NodeCountBefore);
	}
	TestEqual(TEXT("ClassDefaults remain unchanged"), GetBlueprintInitialLifeSpan(Blueprint), InitialLifeSpanBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphApplyRejectsIncompatibleOwnerEventTest,
	"UEAssetDoc.UBlueprint.GraphApply.RejectsIncompatibleOwnerEvent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphApplyRejectsIncompatibleOwnerEventTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphApplyIncompatibleOwnerEvent"));
	UEdGraph* EventGraph = GetEventGraph(Blueprint);
	TestNotNull(TEXT("Transient actor Blueprint has an EventGraph"), EventGraph);
	if (!Blueprint || !EventGraph)
	{
		return false;
	}
	const FGuid GraphGuidBefore = EventGraph->GraphGuid;
	const int32 NodeCountBefore = EventGraph->Nodes.Num();
	const double InitialLifeSpanBefore = GetBlueprintInitialLifeSpan(Blueprint);

	TSharedRef<FJsonObject> Body = MakeBodyWithRegion(
		TEXT("UbergraphPages"),
		{MakeEventGraph({
			UBlueprintGraphTestMakeGraphNode(
				TEXT("OnInitialized"),
				TEXT("/Script/BlueprintGraph.K2Node_Event"),
				MakeMemberRef(TEXT("/Script/UMG.UserWidget"), TEXT("OnInitialized")))
		})});
	SetClassDefaultInitialLifeSpan(Body, 42.0);

	const FAssetDocumentCapabilityResult ApplyResult = ApplyBlueprintBody(Blueprint, Body);
	TestFalse(TEXT("An event from an incompatible owner cannot be materialized in an Actor Blueprint"), ApplyResult.bSuccess);
	TestTrue(
		TEXT("Incompatible event owner reports the exact event admission diagnostic"),
		UBlueprintGraphResultHasDiagnostic(
			ApplyResult,
			TEXT("/Body/UbergraphPages/EventGraph/Nodes/OnInitialized/Member"),
			TEXT("UnsupportedGraphEvent")));
	UEdGraph* EventGraphAfter = GetEventGraph(Blueprint);
	const bool bSameEventGraph = EventGraphAfter == EventGraph;
	TestTrue(TEXT("Live EventGraph identity is unchanged"), bSameEventGraph);
	if (bSameEventGraph)
	{
		TestEqual(TEXT("Live EventGraph guid is unchanged"), EventGraphAfter->GraphGuid, GraphGuidBefore);
		TestEqual(TEXT("Live EventGraph node count is unchanged"), EventGraphAfter->Nodes.Num(), NodeCountBefore);
	}
	TestEqual(TEXT("ClassDefaults remain unchanged"), GetBlueprintInitialLifeSpan(Blueprint), InitialLifeSpanBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphApplyRejectsKismetHiddenOverrideBeforeMutationTest,
	"UEAssetDoc.UBlueprint.GraphApply.RejectsKismetHiddenOverrideBeforeMutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphApplyRejectsKismetHiddenOverrideBeforeMutationTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphApplyHiddenOverride"));
	UEdGraph* EventGraph = GetEventGraph(Blueprint);
	TestNotNull(TEXT("Transient actor Blueprint has an EventGraph"), EventGraph);
	if (!Blueprint || !EventGraph)
	{
		return false;
	}

	UClass* ParentClass = Blueprint->ParentClass;
	static const TCHAR* HiddenOverridesKey = TEXT("KismetHideOverrides");
	const bool bHadHiddenOverrides = ParentClass->HasMetaData(HiddenOverridesKey);
	const FString PreviousHiddenOverrides = ParentClass->GetMetaData(HiddenOverridesKey);
	ParentClass->SetMetaData(HiddenOverridesKey, TEXT("ReceiveActorBeginOverlap"));
	ON_SCOPE_EXIT
	{
		if (bHadHiddenOverrides)
		{
			ParentClass->SetMetaData(HiddenOverridesKey, *PreviousHiddenOverrides);
		}
		else
		{
			ParentClass->RemoveMetaData(HiddenOverridesKey);
		}
	};

	const FGuid GraphGuidBefore = EventGraph->GraphGuid;
	const int32 NodeCountBefore = EventGraph->Nodes.Num();
	const double InitialLifeSpanBefore = GetBlueprintInitialLifeSpan(Blueprint);
	TSharedRef<FJsonObject> Body = MakeBodyWithRegion(
		TEXT("UbergraphPages"),
		{MakeEventGraph({
			UBlueprintGraphTestMakeGraphNode(
				TEXT("ReceiveActorBeginOverlap"),
				TEXT("/Script/BlueprintGraph.K2Node_Event"),
				MakeMemberRef(TEXT("/Script/Engine.Actor"), TEXT("ReceiveActorBeginOverlap")))
		})});
	SetClassDefaultInitialLifeSpan(Body, 42.0);

	const FAssetDocumentCapabilityResult ApplyResult = ApplyBlueprintBody(Blueprint, Body);
	TestFalse(TEXT("An override hidden by the Blueprint parent cannot be materialized"), ApplyResult.bSuccess);
	TestTrue(
		TEXT("Hidden override reports the event admission diagnostic"),
		UBlueprintGraphResultHasDiagnostic(
			ApplyResult,
			TEXT("/Body/UbergraphPages/EventGraph/Nodes/ReceiveActorBeginOverlap/Member"),
			TEXT("UnsupportedGraphEvent")));
	TestEqual(TEXT("Live EventGraph identity is unchanged"), GetEventGraph(Blueprint), EventGraph);
	TestEqual(TEXT("Live EventGraph guid is unchanged"), EventGraph->GraphGuid, GraphGuidBefore);
	TestEqual(TEXT("Live EventGraph node count is unchanged"), EventGraph->Nodes.Num(), NodeCountBefore);
	TestEqual(TEXT("ClassDefaults remain unchanged"), GetBlueprintInitialLifeSpan(Blueprint), InitialLifeSpanBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphApplyRejectsEventFunctionAsCallTest,
	"UEAssetDoc.UBlueprint.GraphApply.RejectsEventFunctionAsCall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphApplyRejectsEventFunctionAsCallTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphApplyEventFunctionAsCall"));
	UEdGraph* EventGraph = GetEventGraph(Blueprint);
	TestNotNull(TEXT("Transient actor Blueprint has an EventGraph"), EventGraph);
	if (!Blueprint || !EventGraph)
	{
		return false;
	}
	const FGuid GraphGuidBefore = EventGraph->GraphGuid;
	const int32 NodeCountBefore = EventGraph->Nodes.Num();
	const double InitialLifeSpanBefore = GetBlueprintInitialLifeSpan(Blueprint);

	TSharedRef<FJsonObject> Body = MakeBodyWithRegion(
		TEXT("UbergraphPages"),
		{MakeEventGraph({
			UBlueprintGraphTestMakeGraphNode(
				TEXT("CallReceiveBeginPlay"),
				TEXT("/Script/BlueprintGraph.K2Node_CallFunction"),
				MakeMemberRef(TEXT("/Script/Engine.Actor"), TEXT("ReceiveBeginPlay")))
		})});
	SetClassDefaultInitialLifeSpan(Body, 42.0);

	const FAssetDocumentCapabilityResult ApplyResult = ApplyBlueprintBody(Blueprint, Body);
	TestFalse(TEXT("An event-only function cannot be materialized as a call node"), ApplyResult.bSuccess);
	TestTrue(
		TEXT("Uncallable event function reports the exact call admission diagnostic"),
		UBlueprintGraphResultHasDiagnostic(
			ApplyResult,
			TEXT("/Body/UbergraphPages/EventGraph/Nodes/CallReceiveBeginPlay/Member"),
			TEXT("UnsupportedGraphFunction")));
	UEdGraph* EventGraphAfter = GetEventGraph(Blueprint);
	const bool bSameEventGraph = EventGraphAfter == EventGraph;
	TestTrue(TEXT("Live EventGraph identity is unchanged"), bSameEventGraph);
	if (bSameEventGraph)
	{
		TestEqual(TEXT("Live EventGraph guid is unchanged"), EventGraphAfter->GraphGuid, GraphGuidBefore);
		TestEqual(TEXT("Live EventGraph node count is unchanged"), EventGraphAfter->Nodes.Num(), NodeCountBefore);
	}
	TestEqual(TEXT("ClassDefaults remain unchanged"), GetBlueprintInitialLifeSpan(Blueprint), InitialLifeSpanBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphApplySnapshotUsesStrongOwnershipTest,
	"UEAssetDoc.UBlueprint.GraphApply.SnapshotUsesStrongOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphApplySnapshotUsesStrongOwnershipTest::RunTest(const FString&)
{
	const FString UEAssetDocPluginBaseDir = ResolveUEAssetDocPluginBaseDir();
	TestFalse(TEXT("UEAssetDoc plugin base dir resolves"), UEAssetDocPluginBaseDir.IsEmpty());
	if (UEAssetDocPluginBaseDir.IsEmpty())
	{
		return false;
	}

	const FString K2GraphAdapterPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		UEAssetDocPluginBaseDir,
		TEXT("Source/AssetDocument/Private/Graphs/K2GraphAdapter.cpp")));
	FString Source;
	const bool bLoadedSource = FFileHelper::LoadFileToString(Source, *K2GraphAdapterPath);
	TestTrue(TEXT("K2GraphAdapter source is available to snapshot ownership test"), bLoadedSource);
	if (!bLoadedSource)
	{
		return false;
	}

	const int32 SnapshotStart = Source.Find(TEXT("class FLosslessUbergraphPagesSnapshot"));
	const int32 SnapshotEnd = Source.Find(TEXT("FString SanitizeSidecarId"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SnapshotStart);
	TestTrue(TEXT("Snapshot ownership test finds FLosslessUbergraphPagesSnapshot"), SnapshotStart != INDEX_NONE);
	TestTrue(TEXT("Snapshot ownership test finds the end of FLosslessUbergraphPagesSnapshot"), SnapshotEnd != INDEX_NONE && SnapshotEnd > SnapshotStart);
	if (SnapshotStart == INDEX_NONE || SnapshotEnd == INDEX_NONE || SnapshotEnd <= SnapshotStart)
	{
		return false;
	}

	const FString SnapshotSource = Source.Mid(SnapshotStart, SnapshotEnd - SnapshotStart);
	TestFalse(TEXT("Lossless graph snapshot does not add its duplicate to the root set"), SnapshotSource.Contains(TEXT("AddToRoot()")));
	TestFalse(TEXT("Lossless graph snapshot does not remove its duplicate from the root set"), SnapshotSource.Contains(TEXT("RemoveFromRoot()")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphApplyUsesNodeAdaptersForMemberBindingTest,
	"UEAssetDoc.UBlueprint.GraphApply.UsesNodeAdaptersForMemberBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphApplyUsesNodeAdaptersForMemberBindingTest::RunTest(const FString&)
{
	const FString UEAssetDocPluginBaseDir = ResolveUEAssetDocPluginBaseDir();
	TestFalse(TEXT("UEAssetDoc plugin base dir resolves"), UEAssetDocPluginBaseDir.IsEmpty());
	if (UEAssetDocPluginBaseDir.IsEmpty())
	{
		return false;
	}

	const FString K2GraphAdapterPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		UEAssetDocPluginBaseDir,
		TEXT("Source/AssetDocument/Private/Graphs/K2GraphAdapter.cpp")));
	const FString K2SpawnerResolverPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		UEAssetDocPluginBaseDir,
		TEXT("Source/AssetDocument/Private/Graphs/K2NodeAdapters/AssetDocumentK2NodeSpawnerResolver.cpp")));

	FString Source;
	const bool bLoadedSource = FFileHelper::LoadFileToString(Source, *K2GraphAdapterPath);
	TestTrue(TEXT("K2GraphAdapter source is available to architecture test"), bLoadedSource);
	FString SpawnerResolverSource;
	const bool bLoadedSpawnerResolver = FFileHelper::LoadFileToString(SpawnerResolverSource, *K2SpawnerResolverPath);
	TestTrue(TEXT("K2 spawner resolver source is available to architecture test"), bLoadedSpawnerResolver);
	if (!bLoadedSource || !bLoadedSpawnerResolver)
	{
		return false;
	}

	FString NodeAdapterSources;
	for (const TCHAR* RelativePath : {
		TEXT("Source/AssetDocument/Private/Graphs/K2NodeAdapters/EventNodeAdapter.cpp"),
		TEXT("Source/AssetDocument/Private/Graphs/K2NodeAdapters/CallFunctionNodeAdapter.cpp"),
		TEXT("Source/AssetDocument/Private/Graphs/K2NodeAdapters/VariableGetSetNodeAdapter.cpp")})
	{
		FString AdapterSource;
		const FString AdapterPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(UEAssetDocPluginBaseDir, RelativePath));
		const bool bLoadedAdapter = FFileHelper::LoadFileToString(AdapterSource, *AdapterPath);
		TestTrue(FString::Printf(TEXT("%s is available to architecture test"), RelativePath), bLoadedAdapter);
		NodeAdapterSources += AdapterSource;
	}

	TestFalse(TEXT("K2GraphAdapter apply does not centralize member binding in ConfigureNodeFromSpec"), Source.Contains(TEXT("ConfigureNodeFromSpec")));
	TestFalse(TEXT("Tier 1 materialization does not directly allocate graph nodes"), Source.Contains(TEXT("NewObject<UEdGraphNode>")));
	TestFalse(TEXT("Tier 1 materialization does not directly add graph nodes"), Source.Contains(TEXT("Graph->AddNode(")));
	TestFalse(TEXT("Tier 1 materialization does not allocate pins after placement"), Source.Contains(TEXT("Node->AllocateDefaultPins(")));
	TestFalse(TEXT("Tier 1 materialization does not reconstruct nodes after placement"), Source.Contains(TEXT("Node->ReconstructNode(")));
	TestTrue(TEXT("Tier 1 materialization invokes UE node spawners"), Source.Contains(TEXT("Spawner->Invoke(")));
	TestTrue(TEXT("Event admission uses the UE spawner template"), SpawnerResolverSource.Contains(TEXT("GetTemplateNode(Graph)")));
	TestTrue(TEXT("Event admission delegates placement rules to UE"), SpawnerResolverSource.Contains(TEXT("CanPasteHere(Graph)")));
	TestFalse(TEXT("Event admission does not copy GetOverrideFunctionClass rules"), SpawnerResolverSource.Contains(TEXT("GetOverrideFunctionClass")));
	TestTrue(TEXT("Event admission honors the Blueprint type's override policy"), SpawnerResolverSource.Contains(TEXT("AllowFunctionOverride(Function)")));
	TestFalse(TEXT("Event adapter does not manually bind EventReference"), NodeAdapterSources.Contains(TEXT("EventReference.SetExternalMember")));
	TestFalse(TEXT("CallFunction adapter does not manually bind FunctionReference"), NodeAdapterSources.Contains(TEXT("CallNode->SetFromFunction")));
	TestFalse(TEXT("Variable adapter does not manually bind self members"), NodeAdapterSources.Contains(TEXT("VariableReference.SetSelfMember")));
	TestFalse(TEXT("Variable adapter does not manually bind external members"), NodeAdapterSources.Contains(TEXT("VariableReference.SetExternalMember")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphApplyUsesStagedVariableReferencesTest,
	"UEAssetDoc.UBlueprint.GraphApply.UsesStagedVariableReferences",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphApplyUsesStagedVariableReferencesTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphApplyStagedVariable"));
	TestNotNull(TEXT("Transient actor Blueprint exists"), Blueprint);
	if (!Blueprint)
	{
		return false;
	}

	TSharedRef<FJsonObject> Body = MakeBeginPlayPrintStringBody(TEXT("Ignored"), false, false);
	Body->SetArrayField(TEXT("Variables"), MakeJsonArray({MakeIntVariableSpec(TEXT("GraphCounter"), TEXT("7"))}));

	TSharedPtr<FJsonObject> Graph = FindGraphJsonByName(Body, TEXT("EventGraph"));
	const TArray<TSharedPtr<FJsonValue>>* ExistingNodes = nullptr;
	TestTrue(TEXT("Desired graph has nodes"), Graph.IsValid() && Graph->TryGetArrayField(TEXT("Nodes"), ExistingNodes) && ExistingNodes);
	TArray<TSharedPtr<FJsonValue>> Nodes = ExistingNodes ? *ExistingNodes : TArray<TSharedPtr<FJsonValue>>();
	Nodes.Add(MakeShared<FJsonValueObject>(UBlueprintGraphTestMakeGraphNode(
		TEXT("GraphCounterGet"),
		TEXT("/Script/BlueprintGraph.K2Node_VariableGet"),
		MakeMemberRef(TEXT("Self"), TEXT("GraphCounter")))));
	Nodes.Add(MakeShared<FJsonValueObject>(UBlueprintGraphTestMakeGraphNode(
		TEXT("GraphCounterSet"),
		TEXT("/Script/BlueprintGraph.K2Node_VariableSet"),
		MakeMemberRef(TEXT("Self"), TEXT("GraphCounter")))));
	Graph->SetArrayField(TEXT("Nodes"), MoveTemp(Nodes));
	AppendLinkToGraph(Graph, TEXT("BeginPlay"), UEdGraphSchema_K2::PN_Then.ToString(), TEXT("GraphCounterSet"), UEdGraphSchema_K2::PN_Execute.ToString());
	AppendLinkToGraph(Graph, TEXT("GraphCounterGet"), TEXT("GraphCounter"), TEXT("GraphCounterSet"), TEXT("GraphCounter"));

	const FAssetDocumentCapabilityResult ApplyResult = ApplyBlueprintBody(Blueprint, Body);
	TestTrue(TEXT("Graph apply resolves same-document staged variable references"), ApplyResult.bSuccess);
	if (!ApplyResult.bSuccess)
	{
		return false;
	}

	UEdGraph* EventGraph = GetEventGraph(Blueprint);
	TestNotNull(TEXT("EventGraph exists after apply"), EventGraph);
	TestTrue(TEXT("GraphCounter variable was created"), Blueprint->NewVariables.ContainsByPredicate([](const FBPVariableDescription& Variable)
	{
		return Variable.VarName == TEXT("GraphCounter");
	}));
	TestEqual(TEXT("Variable get was created"), CountGraphNodesByClass(EventGraph, UK2Node_VariableGet::StaticClass()), 1);
	TestEqual(TEXT("Variable set was created"), CountGraphNodesByClass(EventGraph, UK2Node_VariableSet::StaticClass()), 1);
	TestFalse(TEXT("Applied Blueprint compiles without error"), Blueprint->Status == BS_Error);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphApplyUpdatesPinDefaultTest,
	"UEAssetDoc.UBlueprint.GraphApply.UpdatesPinDefault",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphApplyUpdatesPinDefaultTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphApplyUpdatePin"));
	TestTrue(TEXT("Initial graph apply succeeds"), ApplyBlueprintBody(Blueprint, MakeBeginPlayPrintStringBody(TEXT("Initial"))).bSuccess);
	TestTrue(TEXT("Updated graph apply succeeds"), ApplyBlueprintBody(Blueprint, MakeBeginPlayPrintStringBody(TEXT("Updated"))).bSuccess);

	UEdGraph* Graph = GetEventGraph(Blueprint);
	UK2Node_CallFunction* PrintNode = FindCallFunctionNodeByMemberName(Graph, TEXT("PrintString"));
	UEdGraphPin* InStringPin = PrintNode ? PrintNode->FindPin(TEXT("InString")) : nullptr;
	TestNotNull(TEXT("Updated PrintString InString pin exists"), InStringPin);
	if (InStringPin)
	{
		TestEqual(TEXT("Pin default changed in-place from sidecar"), InStringPin->DefaultValue, FString(TEXT("Updated")));
	}
	TestEqual(TEXT("Apply reused sidecar node identity instead of duplicating PrintString"), CountGraphNodesByMemberName(Graph, TEXT("PrintString")), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphApplyDeletesOmittedNodeAndLinkTest,
	"UEAssetDoc.UBlueprint.GraphApply.DeletesOmittedNodeAndLink",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphApplyDeletesOmittedNodeAndLinkTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphApplyDelete"));
	TestTrue(TEXT("Initial graph apply succeeds"), ApplyBlueprintBody(Blueprint, MakeBeginPlayPrintStringBody()).bSuccess);
	TestTrue(TEXT("Omitting PrintString apply succeeds"), ApplyBlueprintBody(Blueprint, MakeBeginPlayPrintStringBody(TEXT("Ignored"), false, false)).bSuccess);

	UEdGraph* Graph = GetEventGraph(Blueprint);
	TestEqual(TEXT("Omitted PrintString node is deleted"), CountGraphNodesByMemberName(Graph, TEXT("PrintString")), 0);
	TestFalse(TEXT("Omitted execution link is deleted"), GraphHasLink(Graph, UEdGraphSchema_K2::PN_Then, UEdGraphSchema_K2::PN_Execute));
	TestEqual(TEXT("Remaining BeginPlay node is preserved"), CountGraphNodesByMemberName(Graph, TEXT("ReceiveBeginPlay")), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphApplyRejectsInvalidLinkBeforeMutationTest,
	"UEAssetDoc.UBlueprint.GraphApply.RejectsInvalidLinkBeforeMutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphApplyRejectsInvalidLinkBeforeMutationTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphApplyInvalidLink"));
	TestTrue(TEXT("Initial BeginPlay-only graph apply succeeds"), ApplyBlueprintBody(Blueprint, MakeBeginPlayPrintStringBody(TEXT("Ignored"), false, false)).bSuccess);

	const FAssetDocumentCapabilityResult ApplyResult = ApplyBlueprintBody(
		Blueprint,
		MakeBeginPlayPrintStringBody(TEXT("ShouldNotAppear"), true, true, TEXT("NotARealPin"), UEdGraphSchema_K2::PN_Execute.ToString()));
	TestFalse(TEXT("Invalid link endpoint rejects apply"), ApplyResult.bSuccess);
	TestTrue(TEXT("Invalid link reports endpoint diagnostic"), UBlueprintGraphResultHasDiagnosticCode(ApplyResult, TEXT("UnresolvedGraphLinkEndpoint")));

	UEdGraph* Graph = GetEventGraph(Blueprint);
	TestEqual(TEXT("Invalid link rejection did not create PrintString"), CountGraphNodesByMemberName(Graph, TEXT("PrintString")), 0);
	TestEqual(TEXT("Invalid link rejection preserved original BeginPlay"), CountGraphNodesByMemberName(Graph, TEXT("ReceiveBeginPlay")), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphApplyCompileFailureDoesNotSavePartialGraphTest,
	"UEAssetDoc.UBlueprint.GraphApply.CompileFailureDoesNotSavePartialGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphApplyCompileFailureDoesNotSavePartialGraphTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphApplyCompileFail"));
	TestTrue(TEXT("Initial clean graph apply succeeds"), ApplyBlueprintBody(Blueprint, MakeBeginPlayPrintStringBody()).bSuccess);

	TSharedRef<FJsonObject> BadBody = MakeBeginPlayPrintStringBody(TEXT("First"));
	TSharedPtr<FJsonObject> Graph = FindGraphJsonByName(BadBody, TEXT("EventGraph"));
	const TArray<TSharedPtr<FJsonValue>>* ExistingNodes = nullptr;
	TestTrue(TEXT("Bad body graph has nodes"), Graph.IsValid() && Graph->TryGetArrayField(TEXT("Nodes"), ExistingNodes) && ExistingNodes);
	TArray<TSharedPtr<FJsonValue>> Nodes = ExistingNodes ? *ExistingNodes : TArray<TSharedPtr<FJsonValue>>();
	TSharedRef<FJsonObject> DuplicateBeginPlay = UBlueprintGraphTestMakeGraphNode(
		TEXT("BeginPlayDuplicate"),
		TEXT("/Script/BlueprintGraph.K2Node_Event"),
		MakeMemberRef(TEXT("/Script/Engine.Actor"), TEXT("ReceiveBeginPlay")));
	Nodes.Add(MakeShared<FJsonValueObject>(DuplicateBeginPlay));
	Graph->SetArrayField(TEXT("Nodes"), MoveTemp(Nodes));
	AppendLinkToGraph(Graph, TEXT("BeginPlayDuplicate"), UEdGraphSchema_K2::PN_Then.ToString(), TEXT("Print"), UEdGraphSchema_K2::PN_Execute.ToString());

	const FAssetDocumentCapabilityResult ApplyResult = ApplyBlueprintBody(Blueprint, BadBody);
	TestFalse(TEXT("Duplicate event preflight rejects apply"), ApplyResult.bSuccess);
	TestTrue(TEXT("Duplicate event reports DuplicateGraphEvent"), UBlueprintGraphResultHasDiagnosticCode(ApplyResult, TEXT("DuplicateGraphEvent")));

	UEdGraph* EventGraph = GetEventGraph(Blueprint);
	TestEqual(TEXT("Preflight rejection did not add duplicate BeginPlay"), CountGraphNodesByMemberName(EventGraph, TEXT("ReceiveBeginPlay")), 1);
	UK2Node_CallFunction* PrintNode = FindCallFunctionNodeByMemberName(EventGraph, TEXT("PrintString"));
	UEdGraphPin* InStringPin = PrintNode ? PrintNode->FindPin(TEXT("InString")) : nullptr;
	TestNotNull(TEXT("Rollback preserved existing PrintString node"), PrintNode);
	if (InStringPin)
	{
		TestEqual(TEXT("Rollback preserved existing pin default"), InStringPin->DefaultValue, FString(TEXT("Hello from AssetDocument")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphApplyCompileFailureRollbackPreservesUnsupportedNodeTest,
	"UEAssetDoc.UBlueprint.GraphApply.CompileFailureRollbackPreservesUnsupportedNode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphApplyCompileFailureRollbackPreservesUnsupportedNodeTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphApplyCompileFailUnsupported"));
	TestTrue(TEXT("Initial clean graph apply succeeds"), ApplyBlueprintBody(Blueprint, MakeBeginPlayPrintStringBody()).bSuccess);

	UEdGraph* EventGraph = GetEventGraph(Blueprint);
	TestNotNull(TEXT("EventGraph exists before adding unsupported node"), EventGraph);
	AddUBlueprintGraphUnsupportedBranchNode(EventGraph, 640, 0);
	const int32 UnsupportedCountBefore = CountGraphNodesByClass(EventGraph, UK2Node_IfThenElse::StaticClass());
	TestEqual(TEXT("Unsupported branch node was added before failing apply"), UnsupportedCountBefore, 1);

	TSharedRef<FJsonObject> BadBody = MakeBeginPlayPrintStringBody(TEXT("First"));
	TSharedPtr<FJsonObject> Graph = FindGraphJsonByName(BadBody, TEXT("EventGraph"));
	const TArray<TSharedPtr<FJsonValue>>* ExistingNodes = nullptr;
	TestTrue(TEXT("Bad body graph has nodes"), Graph.IsValid() && Graph->TryGetArrayField(TEXT("Nodes"), ExistingNodes) && ExistingNodes);
	TArray<TSharedPtr<FJsonValue>> Nodes = ExistingNodes ? *ExistingNodes : TArray<TSharedPtr<FJsonValue>>();
	TSharedRef<FJsonObject> DuplicateBeginPlay = UBlueprintGraphTestMakeGraphNode(
		TEXT("BeginPlayDuplicate"),
		TEXT("/Script/BlueprintGraph.K2Node_Event"),
		MakeMemberRef(TEXT("/Script/Engine.Actor"), TEXT("ReceiveBeginPlay")));
	Nodes.Add(MakeShared<FJsonValueObject>(DuplicateBeginPlay));
	Graph->SetArrayField(TEXT("Nodes"), MoveTemp(Nodes));
	AppendLinkToGraph(Graph, TEXT("BeginPlayDuplicate"), UEdGraphSchema_K2::PN_Then.ToString(), TEXT("Print"), UEdGraphSchema_K2::PN_Execute.ToString());

	const FAssetDocumentCapabilityResult ApplyResult = ApplyBlueprintBody(Blueprint, BadBody);
	TestFalse(TEXT("Apply rejects before deleting unsupported existing node"), ApplyResult.bSuccess);
	TestTrue(TEXT("Duplicate event is rejected before live deletion checks"), UBlueprintGraphResultHasDiagnosticCode(ApplyResult, TEXT("DuplicateGraphEvent")));

	EventGraph = GetEventGraph(Blueprint);
	TestEqual(TEXT("Preflight rejection preserves unsupported branch node"), CountGraphNodesByClass(EventGraph, UK2Node_IfThenElse::StaticClass()), UnsupportedCountBefore);
	TestEqual(TEXT("Rejected apply did not add duplicate BeginPlay"), CountGraphNodesByMemberName(EventGraph, TEXT("ReceiveBeginPlay")), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphApplyIgnoresStaleNodeGuidWithDifferentMemberTest,
	"UEAssetDoc.UBlueprint.GraphApply.IgnoresStaleNodeGuidWithDifferentMember",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphApplyIgnoresStaleNodeGuidWithDifferentMemberTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphApplyStaleNodeGuid"));
	TestTrue(TEXT("Initial graph apply succeeds"), ApplyBlueprintBody(Blueprint, MakeBeginPlayPrintStringBody()).bSuccess);

	UEdGraph* Graph = GetEventGraph(Blueprint);
	UK2Node_CallFunction* ExistingPrintNode = FindCallFunctionNodeByMemberName(Graph, TEXT("PrintString"));
	TestNotNull(TEXT("Existing PrintString node exists"), ExistingPrintNode);
	const FGuid StaleGuid = ExistingPrintNode ? ExistingPrintNode->NodeGuid : FGuid();

	TSharedRef<FJsonObject> Body = MakeBeginPlayPrintStringBody(TEXT("Ignored"), false, false);
	TSharedPtr<FJsonObject> DesiredGraph = FindGraphJsonByName(Body, TEXT("EventGraph"));
	const TArray<TSharedPtr<FJsonValue>>* ExistingNodes = nullptr;
	TestTrue(TEXT("Desired graph has nodes"), DesiredGraph.IsValid() && DesiredGraph->TryGetArrayField(TEXT("Nodes"), ExistingNodes) && ExistingNodes);
	TArray<TSharedPtr<FJsonValue>> Nodes = ExistingNodes ? *ExistingNodes : TArray<TSharedPtr<FJsonValue>>();

	TSharedRef<FJsonObject> LogString = UBlueprintGraphTestMakeGraphNode(
		TEXT("Log"),
		TEXT("/Script/BlueprintGraph.K2Node_CallFunction"),
		MakeMemberRef(TEXT("/Script/Engine.KismetSystemLibrary"), TEXT("LogString")));
	LogString->SetStringField(TEXT("NodeGuid"), StaleGuid.ToString(EGuidFormats::Digits));
	TSharedRef<FJsonObject> PinOverride = MakeShared<FJsonObject>();
	PinOverride->SetStringField(TEXT("Pin"), TEXT("InString"));
	PinOverride->SetStringField(TEXT("DefaultValue"), TEXT("Log through reflected function"));
	LogString->SetArrayField(TEXT("PinOverrides"), MakeJsonArray({PinOverride}));
	Nodes.Add(MakeShared<FJsonValueObject>(LogString));
	DesiredGraph->SetArrayField(TEXT("Nodes"), MoveTemp(Nodes));
	AppendLinkToGraph(DesiredGraph, TEXT("BeginPlay"), UEdGraphSchema_K2::PN_Then.ToString(), TEXT("Log"), UEdGraphSchema_K2::PN_Execute.ToString());

	const FAssetDocumentCapabilityResult ApplyResult = ApplyBlueprintBody(Blueprint, Body);
	TestTrue(TEXT("Graph apply succeeds with stale NodeGuid ignored"), ApplyResult.bSuccess);

	Graph = GetEventGraph(Blueprint);
	UK2Node_CallFunction* LogNode = FindCallFunctionNodeByMemberName(Graph, TEXT("LogString"));
	TestNotNull(TEXT("LogString node was created"), LogNode);
	if (LogNode)
	{
		TestNotEqual(TEXT("Stale NodeGuid was not reused for a semantically different node"), LogNode->NodeGuid, StaleGuid);
	}
	TestEqual(TEXT("Omitted PrintString node is not rebound into LogString"), CountGraphNodesByMemberName(Graph, TEXT("PrintString")), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphApplyCompileFailureRollsBackClassDefaultsTest,
	"UEAssetDoc.UBlueprint.GraphApply.CompileFailureRollsBackClassDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphApplyCompileFailureRollsBackClassDefaultsTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphApplyCompileFailClassDefaults"));
	TestTrue(TEXT("Initial clean graph apply succeeds"), ApplyBlueprintBody(Blueprint, MakeBeginPlayPrintStringBody()).bSuccess);
	const double InitialLifeSpanBefore = GetBlueprintInitialLifeSpan(Blueprint);

	TSharedRef<FJsonObject> BadBody = MakeBeginPlayPrintStringBody(TEXT("First"));
	SetClassDefaultInitialLifeSpan(BadBody, 42.0);
	TSharedPtr<FJsonObject> Graph = FindGraphJsonByName(BadBody, TEXT("EventGraph"));
	const TArray<TSharedPtr<FJsonValue>>* ExistingNodes = nullptr;
	TestTrue(TEXT("Bad body graph has nodes"), Graph.IsValid() && Graph->TryGetArrayField(TEXT("Nodes"), ExistingNodes) && ExistingNodes);
	TArray<TSharedPtr<FJsonValue>> Nodes = ExistingNodes ? *ExistingNodes : TArray<TSharedPtr<FJsonValue>>();
	TSharedRef<FJsonObject> DuplicateBeginPlay = UBlueprintGraphTestMakeGraphNode(
		TEXT("BeginPlayDuplicate"),
		TEXT("/Script/BlueprintGraph.K2Node_Event"),
		MakeMemberRef(TEXT("/Script/Engine.Actor"), TEXT("ReceiveBeginPlay")));
	Nodes.Add(MakeShared<FJsonValueObject>(DuplicateBeginPlay));
	Graph->SetArrayField(TEXT("Nodes"), MoveTemp(Nodes));
	AppendLinkToGraph(Graph, TEXT("BeginPlayDuplicate"), UEdGraphSchema_K2::PN_Then.ToString(), TEXT("Print"), UEdGraphSchema_K2::PN_Execute.ToString());

	const FAssetDocumentCapabilityResult ApplyResult = ApplyBlueprintBody(Blueprint, BadBody);
	TestFalse(TEXT("Duplicate event preflight rejects apply"), ApplyResult.bSuccess);
	TestTrue(TEXT("Duplicate event reports DuplicateGraphEvent"), UBlueprintGraphResultHasDiagnosticCode(ApplyResult, TEXT("DuplicateGraphEvent")));
	TestEqual(TEXT("Preflight rejection leaves preceding ClassDefaults unchanged"), GetBlueprintInitialLifeSpan(Blueprint), InitialLifeSpanBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphApplyRejectsDeletingUnsupportedExistingNodeTest,
	"UEAssetDoc.UBlueprint.GraphApply.RejectsDeletingUnsupportedExistingNode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphApplyRejectsDeletingUnsupportedExistingNodeTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphApplyDeleteUnsupported"));
	TestTrue(TEXT("Initial graph apply succeeds"), ApplyBlueprintBody(Blueprint, MakeBeginPlayPrintStringBody()).bSuccess);

	UEdGraph* Graph = GetEventGraph(Blueprint);
	AddUBlueprintGraphUnsupportedBranchNode(Graph, 640, 0);
	TestEqual(TEXT("Unsupported branch exists before apply"), CountGraphNodesByClass(Graph, UK2Node_IfThenElse::StaticClass()), 1);

	const FAssetDocumentCapabilityResult ApplyResult = ApplyBlueprintBody(Blueprint, MakeBeginPlayPrintStringBody());
	TestFalse(TEXT("Apply rejects deleting unsupported existing node"), ApplyResult.bSuccess);
	TestTrue(TEXT("Deleting unsupported node reports actionable diagnostic"), UBlueprintGraphResultHasDiagnosticCode(ApplyResult, TEXT("UnsupportedGraphNodeClass")));
	TestEqual(TEXT("Rejected apply preserves unsupported branch"), CountGraphNodesByClass(GetEventGraph(Blueprint), UK2Node_IfThenElse::StaticClass()), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphApplyRejectsUnsupportedLatentCallFunctionTest,
	"UEAssetDoc.UBlueprint.GraphApply.RejectsUnsupportedLatentCallFunction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphApplyRejectsUnsupportedLatentCallFunctionTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphApplyUnsupportedLatent"));
	TSharedRef<FJsonObject> Body = MakeBeginPlayPrintStringBody(TEXT("Ignored"), false, false);
	TSharedPtr<FJsonObject> Graph = FindGraphJsonByName(Body, TEXT("EventGraph"));
	const TArray<TSharedPtr<FJsonValue>>* ExistingNodes = nullptr;
	TestTrue(TEXT("Desired graph has nodes"), Graph.IsValid() && Graph->TryGetArrayField(TEXT("Nodes"), ExistingNodes) && ExistingNodes);
	TArray<TSharedPtr<FJsonValue>> Nodes = ExistingNodes ? *ExistingNodes : TArray<TSharedPtr<FJsonValue>>();
	Nodes.Add(MakeShared<FJsonValueObject>(UBlueprintGraphTestMakeGraphNode(
		TEXT("Delay"),
		TEXT("/Script/BlueprintGraph.K2Node_CallFunction"),
		MakeMemberRef(TEXT("/Script/Engine.KismetSystemLibrary"), TEXT("Delay")))));
	Graph->SetArrayField(TEXT("Nodes"), MoveTemp(Nodes));
	AppendLinkToGraph(Graph, TEXT("BeginPlay"), UEdGraphSchema_K2::PN_Then.ToString(), TEXT("Delay"), UEdGraphSchema_K2::PN_Execute.ToString());

	const FAssetDocumentCapabilityResult ApplyResult = ApplyBlueprintBody(Blueprint, Body);
	TestFalse(TEXT("Latent function is outside Tier 1 apply support"), ApplyResult.bSuccess);
	TestTrue(TEXT("Latent function reports unsupported function diagnostic"), UBlueprintGraphResultHasDiagnosticCode(ApplyResult, TEXT("UnsupportedGraphFunction")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphApplyRejectsDeletingUnsupportedLatentCallFunctionTest,
	"UEAssetDoc.UBlueprint.GraphApply.RejectsDeletingUnsupportedLatentCallFunction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphApplyRejectsDeletingUnsupportedLatentCallFunctionTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphApplyDeleteUnsupportedLatent"));
	TestTrue(TEXT("Initial graph apply succeeds"), ApplyBlueprintBody(Blueprint, MakeBeginPlayPrintStringBody()).bSuccess);

	UEdGraph* Graph = GetEventGraph(Blueprint);
	UK2Node_CallFunction* DelayNode = AddUBlueprintGraphK2Node<UK2Node_CallFunction>(Graph, 640, 0);
	UFunction* DelayFunction = UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("Delay"));
	TestNotNull(TEXT("Delay function resolves"), DelayFunction);
	if (DelayNode && DelayFunction)
	{
		DelayNode->SetFromFunction(DelayFunction);
		DelayNode->AllocateDefaultPins();
	}
	TestEqual(TEXT("Existing Delay node was added"), CountGraphNodesByMemberName(Graph, TEXT("Delay")), 1);

	const FAssetDocumentCapabilityResult ApplyResult = ApplyBlueprintBody(Blueprint, MakeBeginPlayPrintStringBody());
	TestFalse(TEXT("Apply rejects deleting unsupported latent call function"), ApplyResult.bSuccess);
	TestTrue(TEXT("Deleting unsupported latent call reports unsupported function"), UBlueprintGraphResultHasDiagnosticCode(ApplyResult, TEXT("UnsupportedGraphFunction")));
	TestEqual(TEXT("Rejected apply preserves Delay node"), CountGraphNodesByMemberName(GetEventGraph(Blueprint), TEXT("Delay")), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphApplyRejectsUnsupportedPinDefaultObjectTest,
	"UEAssetDoc.UBlueprint.GraphApply.RejectsUnsupportedPinDefaultObject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphApplyRejectsUnsupportedPinDefaultObjectTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphApplyUnsupportedPinDefaultObject"));
	TSharedRef<FJsonObject> Body = MakeBeginPlayPrintStringBody();
	TSharedPtr<FJsonObject> Graph = FindGraphJsonByName(Body, TEXT("EventGraph"));
	const TSharedPtr<FJsonObject> Print = FindNodeById(Graph, TEXT("Print"));
	TestTrue(TEXT("Print node exists"), Print.IsValid());
	if (Print.IsValid())
	{
		TSharedRef<FJsonObject> PinOverride = MakeShared<FJsonObject>();
		PinOverride->SetStringField(TEXT("Pin"), TEXT("InString"));
		PinOverride->SetStringField(TEXT("DefaultObject"), TEXT("/Script/Engine.Actor"));
		Print->SetArrayField(TEXT("PinOverrides"), MakeJsonArray({PinOverride}));
	}

	const FAssetDocumentCapabilityResult ApplyResult = ApplyBlueprintBody(Blueprint, Body);
	TestFalse(TEXT("DefaultObject on string pin is outside Tier 1 apply support"), ApplyResult.bSuccess);
	TestTrue(TEXT("Unsupported pin default reports actionable diagnostic"), UBlueprintGraphResultHasDiagnosticCode(ApplyResult, TEXT("UnsupportedGraphPinDefault")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphApplyRejectsInvalidEventMemberBeforeBodyMutationTest,
	"UEAssetDoc.UBlueprint.GraphApply.RejectsInvalidEventMemberBeforeBodyMutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphApplyRejectsInvalidEventMemberBeforeBodyMutationTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphApplyInvalidEventMember"));
	const double InitialLifeSpanBefore = GetBlueprintInitialLifeSpan(Blueprint);

	TSharedRef<FJsonObject> Body = MakeBeginPlayPrintStringBody(TEXT("Ignored"), false, false);
	SetClassDefaultInitialLifeSpan(Body, 42.0);
	const TSharedPtr<FJsonObject> Graph = FindGraphJsonByName(Body, TEXT("EventGraph"));
	const TSharedPtr<FJsonObject> BeginPlay = FindNodeById(Graph, TEXT("BeginPlay"));
	TestTrue(TEXT("BeginPlay sidecar node exists"), BeginPlay.IsValid());
	if (BeginPlay.IsValid())
	{
		BeginPlay->SetObjectField(TEXT("Member"), MakeMemberRef(TEXT("/Script/Engine.Actor"), TEXT("ReceiveDoesNotExist")));
	}

	const FAssetDocumentCapabilityResult ApplyResult = ApplyBlueprintBody(Blueprint, Body);
	TestFalse(TEXT("Invalid event member rejects apply"), ApplyResult.bSuccess);
	TestTrue(TEXT("Invalid event member reports unresolved member diagnostic"), UBlueprintGraphResultHasDiagnosticCode(ApplyResult, TEXT("UnresolvedGraphMemberReference")));
	TestEqual(TEXT("Invalid graph member failed before ClassDefaults mutation"), GetBlueprintInitialLifeSpan(Blueprint), InitialLifeSpanBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphApplyRejectsInvalidVariableMemberBeforeBodyMutationTest,
	"UEAssetDoc.UBlueprint.GraphApply.RejectsInvalidVariableMemberBeforeBodyMutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphApplyRejectsInvalidVariableMemberBeforeBodyMutationTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = CreateTransientUBlueprintGraphActorBlueprint(TEXT("BP_GraphApplyInvalidVariableMember"));
	const double InitialLifeSpanBefore = GetBlueprintInitialLifeSpan(Blueprint);

	TSharedRef<FJsonObject> Body = MakeBeginPlayPrintStringBody(TEXT("Ignored"), false, false);
	SetClassDefaultInitialLifeSpan(Body, 42.0);
	TSharedPtr<FJsonObject> Graph = FindGraphJsonByName(Body, TEXT("EventGraph"));
	const TArray<TSharedPtr<FJsonValue>>* ExistingNodes = nullptr;
	TestTrue(TEXT("Desired graph has nodes"), Graph.IsValid() && Graph->TryGetArrayField(TEXT("Nodes"), ExistingNodes) && ExistingNodes);
	TArray<TSharedPtr<FJsonValue>> Nodes = ExistingNodes ? *ExistingNodes : TArray<TSharedPtr<FJsonValue>>();
	Nodes.Add(MakeShared<FJsonValueObject>(UBlueprintGraphTestMakeGraphNode(
		TEXT("MissingVariableGet"),
		TEXT("/Script/BlueprintGraph.K2Node_VariableGet"),
		MakeMemberRef(TEXT("Self"), TEXT("MissingVariable")))));
	Graph->SetArrayField(TEXT("Nodes"), MoveTemp(Nodes));

	const FAssetDocumentCapabilityResult ApplyResult = ApplyBlueprintBody(Blueprint, Body);
	TestFalse(TEXT("Invalid variable member rejects apply"), ApplyResult.bSuccess);
	TestTrue(TEXT("Invalid variable member reports unresolved member diagnostic"), UBlueprintGraphResultHasDiagnosticCode(ApplyResult, TEXT("UnresolvedGraphMemberReference")));
	TestEqual(TEXT("Invalid graph member failed before ClassDefaults mutation"), GetBlueprintInitialLifeSpan(Blueprint), InitialLifeSpanBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintGraphApplyFileWritesSyncStateForGeneratedMetadataOnlyDiffTest,
	"UEAssetDoc.UBlueprint.GraphApply.ApplyFileWritesSyncStateForGeneratedMetadataOnlyDiff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintGraphApplyFileWritesSyncStateForGeneratedMetadataOnlyDiffTest::RunTest(const FString&)
{
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	const FString Target = MakeUniqueBlueprintTarget(TEXT("BP_GraphApplyFileSync"));
	const FString SidecarPath = FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(Target);
	TestTrue(TEXT("Creates graph ApplyFile sidecar directory"), IFileManager::Get().MakeDirectory(*FPaths::GetPath(SidecarPath), true));

	TSharedPtr<FJsonObject> Document = MakeUBlueprintGraphSidecarDocument(
		Target,
		MakeBeginPlayPrintStringBody(TEXT("Hello from graph apply-file sync"), true, true));
	if (!TestTrue(TEXT("Graph ApplyFile sidecar writes"), UBlueprintGraphTestWriteJsonObjectToFile(Document, SidecarPath)))
	{
		return false;
	}

	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = SidecarPath;
	Request.bSaveAsset = true;
	const FAssetDocumentResult Result = Service.ApplyFile(Request);

	TestTrue(TEXT("ApplyFile succeeds for generated-metadata-only UBlueprint graph sidecar"), Result.IsSuccess());
	if (!Result.IsSuccess())
	{
		AddError(Result.Message);
		return false;
	}
	TestTrue(TEXT("ApplyFile writes UBlueprint graph sidecar sync state"), Result.bWroteSidecar);
	TestTrue(TEXT("ApplyFile returns payload"), Result.Payload.IsValid());
	if (Result.Payload.IsValid())
	{
		TestFalse(TEXT("ApplyFile does not skip UBlueprint graph sidecar sync update"), Result.Payload->HasField(TEXT("sidecar_sync_update_skipped")));
	}

	TSharedPtr<FJsonObject> ReloadedDocument;
	FString LoadError;
	TestTrue(TEXT("ApplyFile reloads synced UBlueprint graph sidecar"), FAssetDocumentSidecar::LoadJsonFile(SidecarPath, ReloadedDocument, LoadError));
	if (!ReloadedDocument.IsValid())
	{
		AddError(LoadError);
		return false;
	}

	// Python route (profile.ublueprint.v2): the rewritten sidecar is the canonical document; the
	// legacy C++ per-region _meta.sync bookkeeping is not part of the generic runtime.
	const TSharedPtr<FJsonObject>* Body = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* UbergraphPages = nullptr;
	TestTrue(TEXT("ApplyFile keeps the Body"), ReloadedDocument->TryGetObjectField(TEXT("Body"), Body));
	TestTrue(TEXT("ApplyFile keeps Body.UbergraphPages"),
		Body && Body->IsValid() && (*Body)->TryGetArrayField(TEXT("UbergraphPages"), UbergraphPages) && UbergraphPages->Num() > 0);

	TestNotNull(TEXT("ApplyFile-created UBlueprint loads"), LoadObject<UBlueprint>(nullptr, *MakeUBlueprintGraphObjectPathFromTarget(Target)));
	return true;
}

#endif
