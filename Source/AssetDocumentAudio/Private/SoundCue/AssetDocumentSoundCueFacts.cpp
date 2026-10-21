// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Facts of the Sound Cue editor the SoundCue profile reads (design §3.2 / §3.4): class labels, the palette's node
// classes, starting connectors and child limits, the Details values of a new node (what an omitted property means),
// the comment box defaults, the open-editor check and the list_types kind sound_node.

#include "AssetDocumentSoundCue.h"

#include "AssetDocumentGraphPropertyCodec.h"
#include "ClassViewerFilter.h"
#include "ClassViewerModule.h"
#include "Dom/JsonObject.h"
#include "EdGraphNode_Comment.h"
#include "Editor.h"
#include "Modules/ModuleManager.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "SoundCueGraph/SoundCueGraph.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

namespace UE::AssetDocument::Graph::Audio::SoundCue
{
namespace
{
const TCHAR* const EnginePrefix = TEXT("/Script/Engine.SoundNode");
const TCHAR* const CommentPropertyNames[] = {
	TEXT("CommentColor"), TEXT("FontSize"), TEXT("bCommentBubbleVisible_InDetailsPanel"), TEXT("bColorCommentBubble"),
	TEXT("MoveMode"), TEXT("NodeDetails")};

FString ValueText(const FValue& Value)
{
	return ValueToJsonString(Value, false);
}

// A node of Class as the editor leaves it with Inputs child slots: a new node whose per-child arrays got the editor's
// per-slot defaults (SetChildNodes, which AddInputPin / CreateStartingConnectors + compile end with). Transient.
USoundNode* NewReferenceNode(UClass& Class, int32 Inputs)
{
	USoundNode* Node = NewObject<USoundNode>(GetTransientPackage(), &Class, NAME_None, RF_Transient);
	TArray<USoundNode*> Children;
	Children.AddZeroed(FMath::Max(0, Inputs));
	Node->SetChildNodes(Children);
	return Node;
}

FValue EncodeObjectProperties(const UObject& Object, const UObject* Reference)
{
	FObjectBuilder Builder;
	for (TFieldIterator<FProperty> It(Object.GetClass()); It; ++It)
	{
		if (!IsPropertyEditable(**It))
		{
			continue;
		}
		const FValue Value = EncodePropertyValue(**It, It->ContainerPtrToValuePtr<void>(&Object));
		if (Reference != nullptr && ValueText(Value) == ValueText(EncodePropertyValue(**It, It->ContainerPtrToValuePtr<void>(Reference))))
		{
			continue;
		}
		Builder.Add(It->GetName(), Value);
	}
	return Builder.Build();
}

// A comment box as the schema's comment action places it: FSoundCueGraphSchemaAction_NewComment creates a template
// comment and spawns it (FEdGraphSchemaAction_NewNode::SpawnNodeFromTemplate: PostPlacedNewNode applies the graph
// editor settings' colour, move mode and bubble default).
UEdGraphNode_Comment* NewReferenceComment()
{
	UEdGraphNode_Comment* Comment = NewObject<UEdGraphNode_Comment>(GetTransientPackage(), NAME_None, RF_Transient);
	Comment->PostPlacedNewNode();
	return Comment;
}

FValue EncodeCommentProperties(const UEdGraphNode& Comment, const UEdGraphNode* Reference)
{
	FObjectBuilder Builder;
	for (const TCHAR* Name : CommentPropertyNames)
	{
		const FProperty* Property = Comment.GetClass()->FindPropertyByName(Name);
		if (Property == nullptr)
		{
			continue;
		}
		const FValue Value = EncodePropertyValue(*Property, Property->ContainerPtrToValuePtr<void>(&Comment));
		if (Reference != nullptr && ValueText(Value) == ValueText(EncodePropertyValue(*Property, Property->ContainerPtrToValuePtr<void>(Reference))))
		{
			continue;
		}
		Builder.Add(Name, Value);
	}
	return Builder.Build();
}

UClass* FindTemplateCueClass()
{
	return FindObject<UClass>(nullptr, TEXT("/Script/SoundCueTemplates.SoundCueTemplate"));
}

FString QueryValue(const TMap<FString, FString>& Query, const TCHAR* Key)
{
	const FString* Value = Query.Find(Key);
	return Value != nullptr ? Value->TrimStartAndEnd() : FString();
}
} // namespace

UClass* GetSchemaClass()
{
	static TWeakObjectPtr<UClass> Cached;
	if (!Cached.IsValid())
	{
		Cached = FindObject<UClass>(nullptr, TEXT("/Script/AudioEditor.SoundCueGraphSchema"));
	}
	return Cached.Get();
}

USoundCue* CueOfGraph(const UEdGraph* Graph)
{
	const USoundCueGraph* CueGraph = Cast<const USoundCueGraph>(Graph);
	return CueGraph != nullptr ? Cast<USoundCue>(CueGraph->GetOuter()) : nullptr;
}

bool IsEditableCue(const UObject& Object)
{
	if (!Object.IsA<USoundCue>())
	{
		return false;
	}
	const UClass* Template = FindTemplateCueClass();
	return Template == nullptr || !Object.IsA(Template);
}

bool IsOpenInEditor(const UObject& Cue)
{
	UAssetEditorSubsystem* Editors = GEditor != nullptr ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	return Editors != nullptr && Editors->FindEditorForAsset(const_cast<UObject*>(&Cue), false) != nullptr;
}

FString ClassLabel(const UClass& Class)
{
	const FString Path = Class.GetPathName();
	if (Path.StartsWith(EnginePrefix, ESearchCase::CaseSensitive) && Path.Len() > FCString::Strlen(EnginePrefix))
	{
		return Path.RightChop(FCString::Strlen(EnginePrefix));
	}
	return Path;
}

UClass* ResolveClassLabel(const FString& Label)
{
	if (Label.IsEmpty())
	{
		return nullptr;
	}
	FString Path = Label;
	if (!Label.StartsWith(TEXT("/")))
	{
		Path = FString(EnginePrefix) + (Label.StartsWith(TEXT("SoundNode")) ? Label.RightChop(9) : Label);
	}
	UClass* Class = FindObject<UClass>(nullptr, *Path);
	if (Class == nullptr && Path.StartsWith(TEXT("/")) && !Path.StartsWith(TEXT("/Script/")))
	{
		Class = LoadObject<UClass>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	return Class != nullptr && Class->IsChildOf(USoundNode::StaticClass()) ? Class : nullptr;
}

TArray<UClass*> GetPaletteClasses()
{
	FClassViewerModule& ClassViewer = FModuleManager::LoadModuleChecked<FClassViewerModule>(TEXT("ClassViewer"));
	const FClassViewerInitializationOptions Options;
	const TSharedRef<IClassViewerFilter> Filter = ClassViewer.CreateClassFilter(Options);
	const TSharedRef<FClassViewerFilterFuncs> FilterFuncs = ClassViewer.CreateFilterFuncs();
	TArray<UClass*> Derived;
	GetDerivedClasses(USoundNode::StaticClass(), Derived, true);
	Derived.Sort();
	TArray<UClass*> Out;
	for (UClass* Class : Derived)
	{
		if (Class != nullptr && !Class->HasAnyClassFlags(CLASS_Abstract) && Filter->IsClassAllowed(Options, Class, FilterFuncs))
		{
			Out.Add(Class);
		}
	}
	Out.Sort([](const UClass& A, const UClass& B) { return A.GetPathName() < B.GetPathName(); });
	return Out;
}

int32 GetStartingInputs(UClass& Class)
{
	// Keyed with the child limits too: Quality Level's follow the project's quality levels.
	static TMap<FString, int32> Cache;
	const FString Key = FString::Printf(TEXT("%s|%d|%d"), *Class.GetPathName(), GetMinInputs(Class), GetMaxInputs(Class));
	if (const int32* Found = Cache.Find(Key))
	{
		return *Found;
	}
	// FSoundCueGraphSchemaAction_NewNode::PerformAction's node creation on a transient cue (no transaction, no graph
	// notifications anyone listens to): ConstructSoundNode, then CreateStartingConnectors when the class takes inputs.
	USoundCue* Cue = NewObject<USoundCue>(GetTransientPackage(), NAME_None, RF_Transient);
	USoundNode* Node = Cue->ConstructSoundNode<USoundNode>(&Class, false);
	if (Node->GetMaxChildNodes() > 0 && Node->ChildNodes.Num() == 0)
	{
		Node->CreateStartingConnectors();
	}
	const int32 Inputs = Node->ChildNodes.Num();
	Cache.Add(Key, Inputs);
	return Inputs;
}

int32 GetMinInputs(UClass& Class)
{
	return Class.GetDefaultObject<USoundNode>()->GetMinChildNodes();
}

int32 GetMaxInputs(UClass& Class)
{
	return Class.GetDefaultObject<USoundNode>()->GetMaxChildNodes();
}

FValue EncodeDefaultProperties(UClass& Class, int32 Inputs)
{
	return EncodeObjectProperties(*NewReferenceNode(Class, Inputs), nullptr);
}

FValue EncodeChangedProperties(const USoundNode& Node)
{
	return EncodeObjectProperties(Node, NewReferenceNode(*Node.GetClass(), Node.ChildNodes.Num()));
}

FValue EncodeChangedCommentProperties(const UEdGraphNode& Comment)
{
	return EncodeCommentProperties(Comment, NewReferenceComment());
}

FValue EncodeDefaultCommentProperties()
{
	return EncodeCommentProperties(*NewReferenceComment(), nullptr);
}

TConstArrayView<const TCHAR*> GetCommentPropertyNames()
{
	return CommentPropertyNames;
}

bool ListNodeTypes(const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError)
{
	const FString Pattern = QueryValue(Query, TEXT("name"));
	const FString Prefix = QueryValue(Query, TEXT("path"));
	const bool bWildcard = Pattern.Contains(TEXT("*")) || Pattern.Contains(TEXT("?"));
	for (UClass* Class : GetPaletteClasses())
	{
		const FString Label = ClassLabel(*Class);
		const FString DisplayName = Class->GetDisplayNameText().ToString();
		auto Test = [&Pattern, bWildcard](const FString& Text)
		{
			return bWildcard ? Text.MatchesWildcard(Pattern, ESearchCase::IgnoreCase) : Text.Contains(Pattern, ESearchCase::IgnoreCase);
		};
		if (!Pattern.IsEmpty() && !Test(Label) && !Test(DisplayName) && !Test(Class->GetPathName()))
		{
			continue;
		}
		if (!Prefix.IsEmpty() && !Class->GetPathName().StartsWith(Prefix, ESearchCase::IgnoreCase))
		{
			continue;
		}
		const int32 Inputs = GetStartingInputs(*Class);
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Path"), Class->GetPathName());
		Row->SetStringField(TEXT("Class"), Label);
		Row->SetStringField(TEXT("DisplayName"), DisplayName);
		Row->SetStringField(TEXT("Category"), TEXT("Sound Node"));
		const FString Tooltip = Class->GetToolTipText().ToString();
		if (!Tooltip.IsEmpty())
		{
			Row->SetStringField(TEXT("Description"), Tooltip);
		}
		Row->SetNumberField(TEXT("Inputs"), Inputs);
		Row->SetNumberField(TEXT("MinInputs"), GetMinInputs(*Class));
		Row->SetNumberField(TEXT("MaxInputs"), GetMaxInputs(*Class));
		Row->SetObjectField(TEXT("Properties"), ValueToJson(EncodeDefaultProperties(*Class, Inputs))->AsObject());
		OutRows.Add(Row);
	}
	return true;
}
} // namespace UE::AssetDocument::Graph::Audio::SoundCue
