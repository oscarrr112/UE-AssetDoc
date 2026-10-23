// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// The Environment Query editor's model as the ai.env_query.* primitives see it: its (unexported) graph classes by
// path, the graph of a query (created as the editor's graph tab creates it), the palette and Add Test classes, the
// Details values of generators / tests and the validation the editor shows.

#include "EnvQuery/AssetDocumentEnvQueryPrimitives.h"

#include "AIGraph.h"
#include "AIGraphNode.h"
#include "AIGraphSchema.h"
#include "AIGraphTypes.h"
#include "AssetDocumentEditorObjectProperties.h"
#include "AssetDocumentGraphValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphNode_Comment.h"
#include "Editor.h"
#include "Engine/EngineTypes.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryNode.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "EnvironmentQuery/EnvQueryTypes.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_BlueprintBase.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_Composite.h"
#include "Modules/ModuleManager.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::Graph::EnvQuery
{
namespace
{
namespace Props = UE::AssetDocument::EditorObjectProperties;

const TCHAR* const EditorModuleName = TEXT("EnvironmentQueryEditor");
const TCHAR* const GeneratorPrefix = TEXT("EnvQueryGenerator_");
const TCHAR* const TestPrefix = TEXT("EnvQueryTest_");

UClass* FindEditorClass(const TCHAR* Name)
{
	if (!EnsureEditorModule())
	{
		return nullptr;
	}
	return FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/EnvironmentQueryEditor.%s"), Name));
}

bool IsEnginePath(const UClass& Class)
{
	return Class.GetPathName().StartsWith(TEXT("/Script/AIModule."));
}

TSharedPtr<FGraphNodeClassHelper> GClassCache;

// FEnvironmentQueryEditorModule::CreateEnvironmentQueryEditor's class cache, built the same way (the module's own is
// private and made only by the first editor open; UEdGraphSchema_EnvironmentQuery's menus read it unchecked).
FGraphNodeClassHelper& ClassCache()
{
	if (!GClassCache.IsValid())
	{
		EnsureEditorModule();
		GClassCache = MakeShared<FGraphNodeClassHelper>(UEnvQueryNode::StaticClass());
		FGraphNodeClassHelper::AddObservedBlueprintClasses(UEnvQueryGenerator_BlueprintBase::StaticClass());
		GClassCache->UpdateAvailableBlueprintClasses();
	}
	return *GClassCache;
}

void GatherClasses(ENodeKind Kind, TArray<FGraphNodeClassData>& OutClasses)
{
	ClassCache().GatherClasses(Kind == ENodeKind::Generator ? UEnvQueryGenerator::StaticClass() : UEnvQueryTest::StaticClass(), OutClasses);
}

} // namespace

bool HasKeyName(const UEdGraphNode& Node)
{
	// A palette or Add Test node is named after its class (FAISchemaAction_NewNode renames the template into the graph,
	// the asset's older nodes kept those names); a document key gives it another.
	return Node.GetFName().GetPlainNameString() != Node.GetClass()->GetName();
}

FString NodeKey(const UAIGraphNode& Node)
{
	if (Node.NodeInstance == nullptr)
	{
		return FString();
	}
	return HasKeyName(Node) ? Node.GetName() : Node.NodeInstance->GetName();
}

FString OptionKey(const UEnvQuery& Query, const UEnvQueryOption& Option)
{
	if (const UAIGraph* Graph = GetGraph(Query))
	{
		for (const UAIGraphNode* Node : GetOptionNodes(*Graph))
		{
			if (Node->NodeInstance == &Option)
			{
				return NodeKey(*Node);
			}
		}
	}
	return Option.GetName();
}

bool Fail(FCall& Call, FStringView Code, FStringView Message, FStringView Path)
{
	if (Call.Error.Code.IsEmpty())
	{
		Call.Error.Set(Code, Message, Path);
	}
	return false;
}

bool EnsureEditorModule()
{
	return FModuleManager::Get().IsModuleLoaded(EditorModuleName)
		|| FModuleManager::Get().LoadModule(EditorModuleName) != nullptr;
}

UClass* GraphClass()
{
	static TWeakObjectPtr<UClass> Cached;
	if (!Cached.IsValid())
	{
		Cached = FindEditorClass(TEXT("EnvironmentQueryGraph"));
	}
	return Cached.Get();
}

UClass* OptionNodeClass()
{
	static TWeakObjectPtr<UClass> Cached;
	if (!Cached.IsValid())
	{
		Cached = FindEditorClass(TEXT("EnvironmentQueryGraphNode_Option"));
	}
	return Cached.Get();
}

UClass* TestNodeClass()
{
	static TWeakObjectPtr<UClass> Cached;
	if (!Cached.IsValid())
	{
		Cached = FindEditorClass(TEXT("EnvironmentQueryGraphNode_Test"));
	}
	return Cached.Get();
}

UClass* RootNodeClass()
{
	static TWeakObjectPtr<UClass> Cached;
	if (!Cached.IsValid())
	{
		Cached = FindEditorClass(TEXT("EnvironmentQueryGraphNode_Root"));
	}
	return Cached.Get();
}

UEnvQuery* GetQuery(FCall& Call)
{
	UEnvQuery* Query = Cast<UEnvQuery>(&Call.Asset);
	if (Query == nullptr)
	{
		Fail(Call, TEXT("EnvQuery.UnsupportedAsset"), FString::Printf(TEXT("%s is not an Environment Query."), *Call.Asset.GetPathName()));
		return nullptr;
	}
	if (GraphClass() == nullptr || OptionNodeClass() == nullptr || TestNodeClass() == nullptr || RootNodeClass() == nullptr)
	{
		Fail(Call, TEXT("EnvQuery.EditorUnavailable"),
			TEXT("The Environment Query editor (plugin EnvironmentQueryEditor) is not loaded; enable the plugin."));
		return nullptr;
	}
	return Query;
}

UAIGraph* GetGraph(const UEnvQuery& Query)
{
#if WITH_EDITORONLY_DATA
	UEdGraph* Graph = Query.EdGraph;
	return Graph != nullptr && GraphClass() != nullptr && Graph->IsA(GraphClass()) ? Cast<UAIGraph>(Graph) : nullptr;
#else
	return nullptr;
#endif
}

UAIGraph* EnsureGraph(UEnvQuery& Query)
{
	if (UAIGraph* Existing = GetGraph(Query))
	{
		return Existing;
	}
#if WITH_EDITORONLY_DATA
	UClass* Class = GraphClass();
	if (Class == nullptr || Query.EdGraph != nullptr)
	{
		return nullptr;
	}
	// FEnvironmentQueryEditor::SpawnTab_UpdateGraph for a query without graph.
	Query.Modify();
	UAIGraph* Graph = NewObject<UAIGraph>(&Query, Class, NAME_None, RF_Transactional);
	Query.EdGraph = Graph;
	Graph->GetSchema()->CreateDefaultNodesForGraph(*Graph);
	Graph->OnCreated();
	Graph->Initialize();
	return Graph;
#else
	return nullptr;
#endif
}

UEdGraphNode* FindRoot(const UEdGraph& Graph)
{
	UClass* Class = RootNodeClass();
	for (UEdGraphNode* Node : Graph.Nodes)
	{
		if (Node != nullptr && Class != nullptr && Node->IsA(Class))
		{
			return Node;
		}
	}
	return nullptr;
}

UEdGraphPin* RootOutput(const UEdGraph& Graph)
{
	UEdGraphNode* Root = FindRoot(Graph);
	if (Root == nullptr)
	{
		return nullptr;
	}
	for (UEdGraphPin* Pin : Root->Pins)
	{
		if (Pin != nullptr && Pin->Direction == EGPD_Output)
		{
			return Pin;
		}
	}
	return nullptr;
}

TArray<UAIGraphNode*> GetOptionNodes(const UEdGraph& Graph)
{
	TArray<UAIGraphNode*> Out;
	UClass* Class = OptionNodeClass();
	for (UEdGraphNode* Node : Graph.Nodes)
	{
		if (Node != nullptr && Class != nullptr && Node->IsA(Class))
		{
			Out.Add(CastChecked<UAIGraphNode>(Node));
		}
	}
	return Out;
}

UEnvQueryOption* GetOption(const UAIGraphNode& OptionNode)
{
	return Cast<UEnvQueryOption>(OptionNode.NodeInstance);
}

bool IsConnected(const UAIGraphNode& OptionNode)
{
	const UEdGraphPin* Input = OptionNode.GetInputPin();
	UClass* Root = RootNodeClass();
	if (Input == nullptr || Root == nullptr)
	{
		return false;
	}
	for (const UEdGraphPin* Linked : Input->LinkedTo)
	{
		if (Linked != nullptr && Linked->GetOwningNode() != nullptr && Linked->GetOwningNode()->IsA(Root))
		{
			return true;
		}
	}
	return false;
}

UAIGraphNode* FindOptionNode(const UEdGraph& Graph, FStringView Name)
{
	for (UAIGraphNode* Node : GetOptionNodes(Graph))
	{
		if (Node->NodeInstance != nullptr && FStringView(NodeKey(*Node)).Equals(Name, ESearchCase::IgnoreCase))
		{
			return Node;
		}
	}
	return nullptr;
}

UAIGraphNode* FindTestNode(const UEdGraph& Graph, FStringView Name)
{
	for (UAIGraphNode* Option : GetOptionNodes(Graph))
	{
		for (UAIGraphNode* Sub : Option->SubNodes)
		{
			if (Sub != nullptr && Sub->NodeInstance != nullptr && FStringView(NodeKey(*Sub)).Equals(Name, ESearchCase::IgnoreCase))
			{
				return Sub;
			}
		}
	}
	return nullptr;
}

UEdGraphNode_Comment* FindComment(const UEdGraph& Graph, FStringView Name)
{
	for (UEdGraphNode* Node : Graph.Nodes)
	{
		UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node);
		if (Comment != nullptr && FStringView(Comment->GetName()).Equals(Name, ESearchCase::IgnoreCase))
		{
			return Comment;
		}
	}
	return nullptr;
}

bool IsTestEnabled(const UAIGraphNode& TestNode)
{
	const FBoolProperty* Property = CastField<FBoolProperty>(TestNode.GetClass()->FindPropertyByName(TEXT("bTestEnabled")));
	return Property == nullptr || Property->GetPropertyValue_InContainer(&TestNode);
}

float GetTestWeight(const UAIGraphNode& TestNode)
{
	const FFloatProperty* Property = CastField<FFloatProperty>(TestNode.GetClass()->FindPropertyByName(TEXT("TestWeightPct")));
	return Property != nullptr ? Property->GetPropertyValue_InContainer(&TestNode) : -1.0f;
}

bool IsOpenInEditor(const UObject& Asset)
{
	UAssetEditorSubsystem* Editors = GEditor != nullptr ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	return Editors != nullptr && Editors->FindEditorForAsset(const_cast<UObject*>(&Asset), false) != nullptr;
}

// ---- classes ---------------------------------------------------------------------------------------------------

FString ClassLabel(const UClass& Class, ENodeKind Kind)
{
	const TCHAR* Prefix = Kind == ENodeKind::Generator ? GeneratorPrefix : TestPrefix;
	const FString Name = Class.GetName();
	if (IsEnginePath(Class) && Name.StartsWith(Prefix) && Name.Len() > FCString::Strlen(Prefix))
	{
		return Name.RightChop(FCString::Strlen(Prefix));
	}
	return Class.GetPathName();
}

TArray<FPaletteClass> GetPalette(ENodeKind Kind)
{
	TArray<FPaletteClass> Out;
	if (!EnsureEditorModule())
	{
		return Out;
	}
	// UEdGraphSchema_EnvironmentQuery::GetGraphContextActions (generators) / GetSubNodeClasses (tests).
	TArray<FGraphNodeClassData> Classes;
	GatherClasses(Kind, Classes);
	TSet<UClass*> Seen;
	for (FGraphNodeClassData& Data : Classes)
	{
		UClass* Class = Data.GetClass(true);
		if (Class == nullptr || Seen.Contains(Class) || Class->HasAnyClassFlags(CLASS_Abstract))
		{
			continue;
		}
		Seen.Add(Class);
		FPaletteClass& Entry = Out.AddDefaulted_GetRef();
		Entry.Class = Class;
		Entry.Label = ClassLabel(*Class, Kind);
		Entry.DisplayName = FName::NameToDisplayString(Data.ToString(), false);
		Entry.Category = Data.GetCategory().ToString();
		Entry.Tooltip = Data.GetTooltip().ToString();
		Entry.Deprecated = Data.GetDeprecatedMessage();
		Entry.bBlueprint = Data.IsBlueprint();
	}
	Out.Sort([](const FPaletteClass& A, const FPaletteClass& B) { return A.Label < B.Label; });
	return Out;
}

bool FindClassData(const UClass& Class, ENodeKind Kind, FGraphNodeClassData& OutData)
{
	if (!EnsureEditorModule())
	{
		return false;
	}
	TArray<FGraphNodeClassData> Classes;
	GatherClasses(Kind, Classes);
	for (FGraphNodeClassData& Data : Classes)
	{
		if (Data.GetClass(true) == &Class)
		{
			OutData = Data;
			return true;
		}
	}
	return false;
}

void ReleaseClassCache()
{
	GClassCache.Reset();
}

UClass* ResolveClass(const FString& Text, ENodeKind Kind, TArray<FString>& OutCandidates)
{
	const TArray<FPaletteClass> Palette = GetPalette(Kind);
	const TCHAR* Prefix = Kind == ENodeKind::Generator ? GeneratorPrefix : TestPrefix;
	FString Bare = Text;
	Bare.RemoveFromEnd(TEXT("_C"));
	for (const FPaletteClass& Entry : Palette)
	{
		const FString Path = Entry.Class->GetPathName();
		if (Entry.Label.Equals(Text, ESearchCase::IgnoreCase) || Path.Equals(Text, ESearchCase::IgnoreCase)
			|| Entry.Class->GetName().Equals(Text, ESearchCase::IgnoreCase)
			|| (IsEnginePath(*Entry.Class) && Entry.Class->GetName().Equals(FString(Prefix) + Text, ESearchCase::IgnoreCase)))
		{
			return Entry.Class;
		}
	}
	TArray<FString> Labels;
	for (const FPaletteClass& Entry : Palette)
	{
		Labels.Add(Entry.Label);
	}
	OutCandidates = SuggestActionKeys(Text, Labels, 5);
	return nullptr;
}

// ---- values ----------------------------------------------------------------------------------------------------

UObject* NewDefaultsObject(UClass& Class)
{
	UObject* Object = NewObject<UObject>(GetTransientPackage(), &Class, NAME_None, RF_Transient);
	if (UEnvQueryNode* Node = Cast<UEnvQueryNode>(Object))
	{
		Node->UpdateNodeVersion();
	}
	return Object;
}

// ---- FEnvTraceData as its Details panel shows it ---------------------------------------------------------------
// FEnvTraceDataCustomization shows the trace mode, filter, channel, shape, profile, extents, projection and trace
// flags; VersionNum (no Edit flag), SerializedChannel (derived from TraceChannel: OnTraceChannelChanged) and the
// bCan* capability flags (set by the node class) are not shown. They are also what FEnvTraceData::OnPostLoad
// rewrites (a new node's VersionNum 0 / SerializedChannel ECC_WorldStatic become 1 / its TraceChannel's channel), so
// a document carrying them would not read back the same after a reload.
namespace
{
const TCHAR* const HiddenTraceMembers[] = {TEXT("VersionNum"), TEXT("SerializedChannel"), TEXT("bCanTraceOnNavMesh"),
	TEXT("bCanTraceOnGeometry"), TEXT("bCanDisableTrace"), TEXT("bCanProjectDown")};

bool IsHiddenTraceMember(const FString& Name)
{
	for (const TCHAR* Hidden : HiddenTraceMembers)
	{
		if (Name.Equals(Hidden))
		{
			return true;
		}
	}
	return false;
}

bool IsTraceData(const FProperty& Property)
{
	const FStructProperty* Struct = CastField<FStructProperty>(&Property);
	return Struct != nullptr && Struct->Struct == FEnvTraceData::StaticStruct();
}

FProperty* FindMember(const UStruct& Struct, const FString& Name)
{
	if (FProperty* Found = Struct.FindPropertyByName(*Name))
	{
		return Found;
	}
	for (TFieldIterator<FProperty> It(&Struct); It; ++It)
	{
		if (It->GetAuthoredName() == Name)
		{
			return *It;
		}
	}
	return nullptr;
}

const FValue* FindKey(const FValue& Object, const TCHAR* Key)
{
	const int32 Index = Object.Kind == EAssetDocumentRuntimeValueKind::Object ? Object.ObjectKeys.IndexOfByKey(FString(Key)) : INDEX_NONE;
	return Index != INDEX_NONE && Object.Elements.IsValidIndex(Index) ? &Object.Elements[Index] : nullptr;
}

UClass* InstancedClass(const FValue& Value)
{
	const FValue* Class = FindKey(Value, TEXT("Class"));
	return Class != nullptr && Class->Kind == EAssetDocumentRuntimeValueKind::String
		? FindObject<UClass>(nullptr, *Class->StringValue) : nullptr;
}

void TidyObjectProperties(UClass& Class, FValue& Properties);

void TidyValue(const FProperty& Property, FValue& Value)
{
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		if (Value.Kind != EAssetDocumentRuntimeValueKind::Object)
		{
			return;
		}
		for (int32 Index = Value.ObjectKeys.Num() - 1; Index >= 0; --Index)
		{
			if (IsTraceData(Property) && IsHiddenTraceMember(Value.ObjectKeys[Index]))
			{
				Value.ObjectKeys.RemoveAt(Index);
				Value.Elements.RemoveAt(Index);
			}
			else if (FProperty* Member = FindMember(*Struct->Struct, Value.ObjectKeys[Index]))
			{
				TidyValue(*Member, Value.Elements[Index]);
			}
		}
	}
	else if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		for (FValue& Element : Value.Elements)
		{
			TidyValue(*Array->Inner, Element);
		}
	}
	else if (CastField<FObjectPropertyBase>(&Property) != nullptr && Value.Kind == EAssetDocumentRuntimeValueKind::Object)
	{
		UClass* Class = InstancedClass(Value);
		const int32 Index = Value.ObjectKeys.IndexOfByKey(FString(TEXT("Properties")));
		if (Class != nullptr && Index != INDEX_NONE)
		{
			TidyObjectProperties(*Class, Value.Elements[Index]);
		}
	}
}

// An instanced object's Properties (the codec lists the fields that differ from the class defaults): tidied, and a
// field left out again when it then equals the class default's (a reloaded TraceData differs only in hidden members).
void TidyObjectProperties(UClass& Class, FValue& Properties)
{
	if (Properties.Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return;
	}
	UObject* Defaults = Class.GetDefaultObject();
	for (int32 Index = Properties.ObjectKeys.Num() - 1; Index >= 0; --Index)
	{
		FProperty* Field = FindMember(Class, Properties.ObjectKeys[Index]);
		if (Field == nullptr)
		{
			continue;
		}
		const FString Before = ValueToJsonString(Properties.Elements[Index], false);
		TidyValue(*Field, Properties.Elements[Index]);
		FValue Default;
		if (Defaults != nullptr && ValueToJsonString(Properties.Elements[Index], false) != Before
			&& Props::Read(*Defaults, Properties.ObjectKeys[Index], Default))
		{
			TidyValue(*Field, Default);
			if (ValueToJsonString(Default, false) == ValueToJsonString(Properties.Elements[Index], false))
			{
				Properties.ObjectKeys.RemoveAt(Index);
				Properties.Elements.RemoveAt(Index);
			}
		}
	}
}

bool FindHidden(const FProperty& Property, const FValue& Value, const FString& Path, FString& OutPath)
{
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		for (int32 Index = 0; Value.Kind == EAssetDocumentRuntimeValueKind::Object && Index < Value.ObjectKeys.Num(); ++Index)
		{
			const FString Inner = Path + TEXT("/") + Value.ObjectKeys[Index];
			if (IsTraceData(Property) && IsHiddenTraceMember(Value.ObjectKeys[Index]))
			{
				OutPath = Inner;
				return true;
			}
			FProperty* Member = FindMember(*Struct->Struct, Value.ObjectKeys[Index]);
			if (Member != nullptr && FindHidden(*Member, Value.Elements[Index], Inner, OutPath))
			{
				return true;
			}
		}
	}
	else if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		for (int32 Index = 0; Value.Kind == EAssetDocumentRuntimeValueKind::Array && Index < Value.Elements.Num(); ++Index)
		{
			if (FindHidden(*Array->Inner, Value.Elements[Index], FString::Printf(TEXT("%s/%d"), *Path, Index), OutPath))
			{
				return true;
			}
		}
	}
	else if (CastField<FObjectPropertyBase>(&Property) != nullptr)
	{
		UClass* Class = InstancedClass(Value);
		const FValue* Properties = FindKey(Value, TEXT("Properties"));
		for (int32 Index = 0; Class != nullptr && Properties != nullptr && Properties->Kind == EAssetDocumentRuntimeValueKind::Object
			&& Index < Properties->ObjectKeys.Num(); ++Index)
		{
			FProperty* Field = FindMember(*Class, Properties->ObjectKeys[Index]);
			if (Field != nullptr && FindHidden(*Field, Properties->Elements[Index],
				Path + TEXT("/Properties/") + Properties->ObjectKeys[Index], OutPath))
			{
				return true;
			}
		}
	}
	return false;
}

void SyncTraceChannels(UObject& Object, int32 Depth);

void SyncTraceValue(const FProperty& Property, void* Data, UObject& Owner, int32 Depth)
{
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		if (Struct->Struct == FEnvTraceData::StaticStruct())
		{
			FEnvTraceData& Trace = *static_cast<FEnvTraceData*>(Data);
			const ETraceTypeQuery Channel = Trace.TraceChannel.GetValue();
			if (Trace.VersionNum >= 1 && UEngineTypes::ConvertToTraceType(Trace.SerializedChannel.GetValue()) != Channel)
			{
				Owner.Modify();
				Trace.SerializedChannel = UEngineTypes::ConvertToCollisionChannel(Channel);
			}
			return;
		}
		for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
		{
			for (int32 Element = 0; Element < It->ArrayDim; ++Element)
			{
				SyncTraceValue(**It, It->ContainerPtrToValuePtr<void>(Data, Element), Owner, Depth);
			}
		}
	}
	else if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		FScriptArrayHelper Helper(Array, Data);
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			SyncTraceValue(*Array->Inner, Helper.GetRawPtr(Index), Owner, Depth);
		}
	}
	else if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(&Property))
	{
		UObject* Sub = ObjectProperty->HasAnyPropertyFlags(CPF_InstancedReference | CPF_PersistentInstance)
			? ObjectProperty->GetObjectPropertyValue(Data) : nullptr;
		if (Sub != nullptr && Sub->IsIn(&Owner))
		{
			SyncTraceChannels(*Sub, Depth + 1);
		}
	}
}

void SyncTraceChannels(UObject& Object, int32 Depth)
{
	if (Depth > 8)
	{
		return;
	}
	for (TFieldIterator<FProperty> It(Object.GetClass()); It; ++It)
	{
		for (int32 Element = 0; Element < It->ArrayDim; ++Element)
		{
			SyncTraceValue(**It, It->ContainerPtrToValuePtr<void>(&Object, Element), Object, Depth);
		}
	}
}
} // namespace

bool FindHiddenMember(UObject& Owner, const FString& Property, const FValue& Value, FString& OutPath)
{
	FProperty* Field = FindMember(*Owner.GetClass(), Property);
	return Field != nullptr && FindHidden(*Field, Value, FString(), OutPath);
}

void SyncTraceChannels(UObject& Object)
{
	SyncTraceChannels(Object, 0);
}

FValue EncodeProperties(UObject& Object, UObject* Reference, TArray<FString>& OutUnsupported)
{
	FObjectBuilder Values;
	for (const Props::FAdmittedField& Field : Props::ListFields(*Object.GetClass()))
	{
		if (!Field.bWritable && !Field.bEditableOnTemplates)
		{
			continue;
		}
		FValue Value;
		if (!Props::Read(Object, Field, Value))
		{
			OutUnsupported.Add(Field.Name);
			continue;
		}
		TidyValue(*Field.Property, Value);
		if (Reference != nullptr && Reference->IsA(Field.Property->GetOwnerClass()))
		{
			FValue Default;
			if (Props::Read(*Reference, Field, Default))
			{
				TidyValue(*Field.Property, Default);
				if (ValueToJsonString(Default, false) == ValueToJsonString(Value, false))
				{
					continue;
				}
			}
		}
		Values.Add(Field.Name, MoveTemp(Value));
	}
	return Values.Build();
}

const TCHAR* const CommentPropertyNames[] = {
	TEXT("CommentColor"), TEXT("FontSize"), TEXT("bCommentBubbleVisible_InDetailsPanel"), TEXT("bColorCommentBubble"),
	TEXT("MoveMode"), TEXT("NodeDetails")};
const int32 NumCommentPropertyNames = UE_ARRAY_COUNT(CommentPropertyNames);

FValue EncodeCommentProperties(UEdGraphNode_Comment& Comment)
{
	FObjectBuilder Values;
	for (const TCHAR* Name : CommentPropertyNames)
	{
		FValue Value;
		if (Props::Read(Comment, Name, Value))
		{
			Values.Add(Name, MoveTemp(Value));
		}
	}
	return Values.Build();
}

// ---- validation ------------------------------------------------------------------------------------------------

void Validate(const UEnvQuery& Query, TArray<FMessage>& OutMessages)
{
	auto Add = [&OutMessages](const TCHAR* Severity, FString Message, const FString& Option, const FString& Test)
	{
		FMessage& Entry = OutMessages.AddDefaulted_GetRef();
		Entry.Severity = Severity;
		Entry.Message = MoveTemp(Message);
		Entry.Option = Option;
		Entry.Test = Test;
	};
	const UAIGraph* Graph = GetGraph(Query);
	if (Graph == nullptr)
	{
		int32 Index = 0;
		for (const UEnvQueryOption* Option : Query.GetOptions())
		{
			if (Option == nullptr || Option->Generator == nullptr)
			{
				Add(TEXT("error"), FString::Printf(TEXT("Option %d has no generator."), Index), Option != nullptr ? Option->GetName() : FString(), FString());
			}
			++Index;
		}
		if (Query.GetOptions().IsEmpty())
		{
			Add(TEXT("warning"), TEXT("The query has no option: it returns no items."), FString(), FString());
		}
		return;
	}
	int32 Connected = 0;
	for (const UAIGraphNode* OptionNode : GetOptionNodes(*Graph))
	{
		const UEnvQueryOption* Option = GetOption(*OptionNode);
		const FString OptionName = Option != nullptr ? NodeKey(*OptionNode) : OptionNode->GetName();
		const UEnvQueryGenerator* Generator = Option != nullptr ? Option->Generator.Get() : nullptr;
		if (Generator == nullptr)
		{
			Add(TEXT("error"), FString::Printf(TEXT("%s has no generator (class %s missing)."), *OptionName,
				*OptionNode->ClassData.GetClassName()), OptionName, FString());
			continue;
		}
		Connected += IsConnected(*OptionNode) ? 1 : 0;
		const FString Deprecated = FGraphNodeClassHelper::GetDeprecationMessage(Generator->GetClass());
		if (!Deprecated.IsEmpty())
		{
			Add(TEXT("warning"), Deprecated, OptionName, FString());
		}
		if (const UEnvQueryGenerator_Composite* Composite = Cast<UEnvQueryGenerator_Composite>(Generator))
		{
			if (!Composite->bHasMatchingItemType)
			{
				Add(TEXT("error"), FString::Printf(TEXT("%s: nested generators must work on exactly the same item types!"), *OptionName),
					OptionName, FString());
			}
		}
		for (const UAIGraphNode* TestNode : OptionNode->SubNodes)
		{
			if (TestNode == nullptr)
			{
				continue;
			}
			const UEnvQueryTest* Test = Cast<UEnvQueryTest>(TestNode->NodeInstance);
			if (Test == nullptr)
			{
				Add(TEXT("error"), FString::Printf(TEXT("A test of %s has no instance (class %s missing)."), *OptionName,
					*TestNode->ClassData.GetClassName()), OptionName, FString());
				continue;
			}
			const FString TestDeprecated = FGraphNodeClassHelper::GetDeprecationMessage(Test->GetClass());
			if (!TestDeprecated.IsEmpty())
			{
				Add(TEXT("warning"), TestDeprecated, OptionName, NodeKey(*TestNode));
			}
			if (IsTestEnabled(*TestNode) && Generator->ItemType != nullptr && !Test->IsSupportedItem(Generator->ItemType))
			{
				Add(TEXT("warning"), FString::Printf(TEXT("%s does not support the items of %s (%s): the query drops it at runtime."),
					*NodeKey(*TestNode), *OptionName, *Generator->ItemType->GetName()), OptionName, NodeKey(*TestNode));
			}
		}
	}
	if (Connected == 0)
	{
		Add(TEXT("warning"), TEXT("No option is connected to Root: the query returns no items."), FString(), FString());
	}
}

FValue MessagesValue(TConstArrayView<FMessage> Messages)
{
	TArray<FValue> Rows;
	for (const FMessage& Message : Messages)
	{
		FObjectBuilder Row;
		Row.AddString(TEXT("severity"), Message.Severity);
		Row.AddString(TEXT("message"), Message.Message);
		if (!Message.Option.IsEmpty())
		{
			Row.AddString(TEXT("option"), Message.Option);
		}
		if (!Message.Test.IsEmpty())
		{
			Row.AddString(TEXT("test"), Message.Test);
		}
		Rows.Add(Row.Build());
	}
	return MakeArray(MoveTemp(Rows));
}

FValue ParamsValue(const UEnvQuery& Query)
{
	TArray<FAIDynamicParam> Params;
	// The owner only scopes the Blackboard key filters CollectQueryParams creates: the transient package, not the query.
	Query.CollectQueryParams(*GetTransientPackage(), Params);
	TArray<FValue> Rows;
	const UEnum* Types = StaticEnum<EAIParamType>();
	for (const FAIDynamicParam& Param : Params)
	{
		FObjectBuilder Row;
		Row.AddString(TEXT("name"), Param.ParamName.ToString());
		Row.AddString(TEXT("type"), Types != nullptr ? Types->GetNameStringByValue(static_cast<int64>(Param.ParamType)) : FString());
		Rows.Add(Row.Build());
	}
	return MakeArray(MoveTemp(Rows));
}

TSharedPtr<FJsonObject> DescribeCompileStatus(const UObject& Asset)
{
	const UEnvQuery* Query = Cast<UEnvQuery>(&Asset);
	if (Query == nullptr || !EnsureEditorModule())
	{
		return nullptr;
	}
	TArray<FMessage> Messages;
	Validate(*Query, Messages);
	bool bErrors = false;
	bool bWarnings = false;
	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const FMessage& Message : Messages)
	{
		bErrors |= Message.Severity == TEXT("error");
		bWarnings |= Message.Severity == TEXT("warning");
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Severity"), Message.Severity == TEXT("error") ? TEXT("Error")
			: Message.Severity == TEXT("warning") ? TEXT("Warning") : TEXT("Info"));
		Row->SetStringField(TEXT("Message"), Message.Message);
		const FString Node = !Message.Test.IsEmpty() ? Message.Test : Message.Option;
		if (!Node.IsEmpty())
		{
			Row->SetStringField(TEXT("Node"), Node);
		}
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	TSharedPtr<FJsonObject> Status = MakeShared<FJsonObject>();
	Status->SetStringField(TEXT("Status"), bErrors ? TEXT("Error") : bWarnings ? TEXT("UpToDateWithWarnings") : TEXT("UpToDate"));
	Status->SetArrayField(TEXT("Messages"), Rows);
	return Status;
}
} // namespace UE::AssetDocument::Graph::EnvQuery
