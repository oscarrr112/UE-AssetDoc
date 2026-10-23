// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// list_types kind=blueprint_node: the pins of the node an action key places, read from the Blueprint
// palette's own template node (FBlueprintNodeTemplateCache, the node its tooltips and pin filters read), so
// nothing is created in any asset. The context is a Blueprint asset (base=/Game/BP_X, its own members
// resolve) or a class (base=/Script/Engine.Character, default Actor: a scratch Blueprint of it in the
// transient package, kept for the session). name is an action key (aliases accepted, Self = the context
// class) or, without ':', words searched in action keys and menu names (the first matches get pins): the context's
// actions and, without target, the functions of other classes too (rows with Target), the exact menu name first.

#include "AssetDocumentBlueprintNodeTypeLister.h"

#include "Adapters/AssetDocumentGraphBuiltinAdapters.h"
#include "AssetDocumentGraphAdapters.h"
#include "AssetDocumentGraphPrimitives.h"
#include "AssetDocumentGraphUsageScanner.h"
#include "AssetDocumentTypeListers.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"

namespace UE::AssetDocument::Graph::BlueprintNodeLister
{
constexpr int32 MaxSearchRows = 25;

// Scratch Blueprints by parent class path, made once per session (the template cache keys its own outers by
// Blueprint class, so these only provide the context the palette filters by).
TMap<FString, TStrongObjectPtr<UBlueprint>>& ScratchBlueprints()
{
	static TMap<FString, TStrongObjectPtr<UBlueprint>> Map;
	return Map;
}

UBlueprint* ScratchBlueprint(UClass& Parent, FString& OutError)
{
	if (TStrongObjectPtr<UBlueprint>* Found = ScratchBlueprints().Find(Parent.GetPathName()); Found != nullptr && Found->IsValid())
	{
		return Found->Get();
	}
	if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(&Parent) || !Parent.IsChildOf<AActor>())
	{
		OutError = FString::Printf(TEXT("base %s: a class context is an Actor class a Blueprint can derive from; for other Blueprints give the Blueprint asset path."), *Parent.GetPathName());
		return nullptr;
	}
	const FName Name = MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(), TEXT("AssetDocumentNodeProbe"));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(&Parent, GetTransientPackage(), Name, BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (Blueprint == nullptr)
	{
		OutError = FString::Printf(TEXT("base %s: no Blueprint context could be made."), *Parent.GetPathName());
		return nullptr;
	}
	Blueprint->SetFlags(RF_Transient);
	ScratchBlueprints().Add(Parent.GetPathName(), TStrongObjectPtr<UBlueprint>(Blueprint));
	return Blueprint;
}

// The Blueprint the palette is read for, and the class a Self owner names.
bool ResolveContext(const FString& Base, UBlueprint*& OutHost, const UClass*& OutSelf, FString& OutError)
{
	const FString Path = Base.IsEmpty() ? TEXT("/Script/Engine.Actor") : Base;
	UObject* Object = FSoftObjectPath(Path).TryLoad();
	if (Object == nullptr && !Path.Contains(TEXT(".")))
	{
		Object = FSoftObjectPath(Path + TEXT(".") + FPackageName::GetShortName(Path)).TryLoad();
	}
	if (UBlueprint* Blueprint = Cast<UBlueprint>(Object))
	{
		OutHost = Blueprint;
		OutSelf = Blueprint->GeneratedClass != nullptr ? Blueprint->GeneratedClass.Get() : Blueprint->SkeletonGeneratedClass.Get();
		if (OutSelf == nullptr)
		{
			OutError = FString::Printf(TEXT("Blueprint %s has no class yet (compile it)."), *Path);
			return false;
		}
		return true;
	}
	UClass* Class = Cast<UClass>(Object);
	if (Class != nullptr && Class->ClassGeneratedBy != nullptr)
	{
		if (UBlueprint* Owner = Cast<UBlueprint>(Class->ClassGeneratedBy))
		{
			OutHost = Owner;
			OutSelf = Class;
			return true;
		}
	}
	if (Class == nullptr)
	{
		OutError = FString::Printf(TEXT("base '%s' is neither a Blueprint asset nor a class."), *Path);
		return false;
	}
	OutHost = ScratchBlueprint(*Class, OutError);
	OutSelf = Class;
	return OutHost != nullptr;
}

// list_types target: a class path (/Script/EnhancedInput.EnhancedInputLocalPlayerSubsystem) or a Blueprint asset.
// A path that names nothing is the caller's typo, refused by the caller: resolved without load warnings.
UClass* ResolveTargetClass(const FString& Path)
{
	const FString ObjectPath = Path.Contains(TEXT(".")) ? Path : Path + TEXT(".") + FPackageName::GetShortName(Path);
	if (!FPackageName::IsValidObjectPath(ObjectPath))
	{
		return nullptr;
	}
	UObject* Object = FindObject<UObject>(nullptr, *ObjectPath);
	if (Object == nullptr && !ObjectPath.StartsWith(TEXT("/Script/")) && FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(ObjectPath)))
	{
		Object = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	if (const UBlueprint* Blueprint = Cast<UBlueprint>(Object))
	{
		return Blueprint->GeneratedClass != nullptr ? Blueprint->GeneratedClass.Get() : Blueprint->SkeletonGeneratedClass.Get();
	}
	return Cast<UClass>(Object);
}

UEdGraph* ResolveGraph(UBlueprint& Host, const FString& Name, FString& OutError)
{
	TArray<UEdGraph*> Graphs;
	Host.GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (Graph != nullptr && !Graph->HasAnyFlags(RF_Transient) && Graph->GetOuter() == &Host
			&& (Name.IsEmpty() ? Host.UbergraphPages.Contains(Graph) : Graph->GetName().Equals(Name, ESearchCase::IgnoreCase)))
		{
			return Graph;
		}
	}
	OutError = Name.IsEmpty() ? FString::Printf(TEXT("%s has no event graph; give graph=<function graph>."), *Host.GetName())
		: FString::Printf(TEXT("%s has no graph '%s'."), *Host.GetName(), *Name);
	return nullptr;
}

// The keys an authored key may mean here, in order: as written (Self = the context class), a local
// variable of the graph's function, the member of the class that declares it.
TArray<FString> CandidateKeys(const FString& Authored, const UClass& Self, const UEdGraph& Graph, bool bOwnBlueprint)
{
	TArray<FString> Keys;
	TArray<FString> Parts;
	UsageScan::NormalizeActionKey(Authored).ParseIntoArray(Parts, TEXT(":"), false);
	if (Parts.Num() >= 3 && Parts[2].Equals(TEXT("Self"), ESearchCase::IgnoreCase))
	{
		Parts[2] = K2Keys::OwnerPath(&Self);
	}
	Keys.Add(FString::Join(Parts, TEXT(":")));
	// A call keyed by a class that inherits or implements the function (an interface's function on a class
	// implementing it: EnhancedInputLocalPlayerSubsystem:AddMappingContext): Apply places the action of the class
	// declaring it, as the editor's menu off a pin of that class lists it.
	if (Parts.Num() == 4 && Parts[1] == TEXT("CallFunction") && Parts[2].StartsWith(TEXT("/")))
	{
		const UClass* Named = FSoftClassPath(Parts[2]).ResolveClass();
		const UFunction* Function = Named != nullptr ? Named->FindFunctionByName(FName(*Parts[3])) : nullptr;
		if (Function != nullptr && Function->GetOwnerClass() != nullptr && Function->GetOwnerClass() != Named)
		{
			Keys.Add(FString::Printf(TEXT("K2:%s:%s:%s"), *Parts[1], *K2Keys::OwnerPath(Function->GetOwnerClass()), *Parts[3]));
		}
	}
	if (Parts.Num() != 4 || !Parts[2].Equals(K2Keys::OwnerPath(&Self), ESearchCase::IgnoreCase))
	{
		return Keys;
	}
	if (bOwnBlueprint && (Parts[1] == TEXT("VariableGet") || Parts[1] == TEXT("VariableSet")))
	{
		Keys.Add(FString::Printf(TEXT("K2:%s:%s:%s:%s"), *Parts[1], *Parts[2], *Graph.GetName(), *Parts[3]));
	}
	for (const UClass* Ancestor = Self.GetSuperClass(); Ancestor != nullptr; Ancestor = Ancestor->GetSuperClass())
	{
		Keys.Add(FString::Printf(TEXT("K2:%s:%s:%s"), *Parts[1], *K2Keys::OwnerPath(Ancestor), *Parts[3]));
	}
	return Keys;
}

FString TypeText(const FEdGraphPinType& Type)
{
	FString Text = Type.PinCategory.ToString();
	if (Type.PinCategory == UEdGraphSchema_K2::PC_Real)
	{
		Text = Type.PinSubCategory == UEdGraphSchema_K2::PC_Float ? TEXT("float32") : TEXT("float");
	}
	else if (const UObject* Object = Type.PinSubCategoryObject.Get())
	{
		Text += TEXT(":") + Object->GetPathName();
	}
	else if (!Type.PinSubCategory.IsNone() && Type.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
	{
		Text += TEXT(":") + Type.PinSubCategory.ToString();
	}
	if (Type.IsArray())
	{
		Text = TEXT("array:") + Text;
	}
	else if (Type.IsSet())
	{
		Text = TEXT("set:") + Text;
	}
	else if (Type.IsMap())
	{
		const FEdGraphTerminalType& Value = Type.PinValueType;
		const UObject* ValueObject = Value.TerminalSubCategoryObject.Get();
		Text = FString::Printf(TEXT("map:%s->%s%s"), *Text, *Value.TerminalCategory.ToString(),
			ValueObject != nullptr ? *(TEXT(":") + ValueObject->GetPathName()) : TEXT(""));
	}
	return Type.bIsReference ? Text + TEXT(" (by ref)") : Text;
}

TSharedPtr<FJsonObject> PinRow(const UEdGraphPin& Pin)
{
	TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
	Row->SetStringField(TEXT("Name"), Pin.PinName.ToString());
	Row->SetStringField(TEXT("Direction"), Pin.Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
	Row->SetStringField(TEXT("Type"), TypeText(Pin.PinType));
	if (!Pin.PinFriendlyName.IsEmpty() && Pin.PinFriendlyName.ToString() != Pin.PinName.ToString())
	{
		Row->SetStringField(TEXT("DisplayName"), Pin.PinFriendlyName.ToString());
	}
	const FString Default = Pin.DefaultObject != nullptr ? Pin.DefaultObject->GetPathName()
		: !Pin.DefaultTextValue.IsEmpty() ? Pin.DefaultTextValue.ToString() : Pin.DefaultValue;
	if (!Default.IsEmpty())
	{
		Row->SetStringField(TEXT("Default"), Default);
	}
	if (Pin.bAdvancedView)
	{
		Row->SetBoolField(TEXT("Advanced"), true);
	}
	return Row;
}

bool ListNodes(const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError)
{
	const FString* NameValue = Query.Find(TEXT("name"));
	const FString Name = NameValue != nullptr ? NameValue->TrimStartAndEnd() : FString();
	if (Name.IsEmpty())
	{
		OutError = TEXT("blueprint_node needs name: an action key (K2:Branch, K2:Get:Health, K2:CallFunction:/Script/Engine.Actor:K2_DestroyActor) or search words (\"select float\").");
		return false;
	}
	const FString* BaseValue = Query.Find(TEXT("base"));
	const FString* GraphValue = Query.Find(TEXT("graph"));
	UBlueprint* Host = nullptr;
	const UClass* Self = nullptr;
	if (!ResolveContext(BaseValue != nullptr ? BaseValue->TrimStartAndEnd() : FString(), Host, Self, OutError))
	{
		return false;
	}
	UEdGraph* Graph = ResolveGraph(*Host, GraphValue != nullptr ? GraphValue->TrimStartAndEnd() : FString(), OutError);
	const TSharedPtr<IAssetDocumentGraphSchemaAdapter> Adapter = Graph != nullptr ? FindAssetDocumentGraphSchemaAdapter(Graph->GetSchema()) : nullptr;
	if (Graph == nullptr || !Adapter.IsValid())
	{
		OutError = OutError.IsEmpty() ? TEXT("The context graph has no Blueprint schema.") : OutError;
		return false;
	}
	FAssetDocumentGraphActionQuery ActionQuery;
	ActionQuery.Asset = Host;
	ActionQuery.Graph = Graph;
	ActionQuery.bWantUiText = true;
	// target: the actions callable on an object of that class too (its interfaces' functions included), as
	// the editor's menu lists them when dragging off a pin of that class.
	if (const FString* TargetValue = Query.Find(TEXT("target")); TargetValue != nullptr && !TargetValue->TrimStartAndEnd().IsEmpty())
	{
		UClass* Target = ResolveTargetClass(TargetValue->TrimStartAndEnd());
		if (Target == nullptr)
		{
			OutError = FString::Printf(TEXT("target '%s' is not a class (a /Script class path, or a Blueprint asset)."), *TargetValue->TrimStartAndEnd());
			return false;
		}
		ActionQuery.TargetClasses.Add(Target);
	}
	const bool bKey = Name.Contains(TEXT(":"));
	TArray<FString> Wanted;
	if (bKey)
	{
		Wanted = CandidateKeys(Name, *Self, *Graph, !Host->HasAnyFlags(RF_Transient));
		ActionQuery.ExactKeys = Wanted;
	}
	else
	{
		ActionQuery.Text = Name;
		// Without a target, words also find the functions of other classes, as the editor's menu with Context
		// Sensitive off lists them.
		ActionQuery.bContextFreeCalls = ActionQuery.TargetClasses.IsEmpty();
	}
	TArray<FAssetDocumentGraphActionRecord> Records;
	FAssetDocumentGraphError Error;
	if (!Adapter->EnumerateActions(ActionQuery, Records, Error))
	{
		OutError = Error.Message;
		return false;
	}
	// A search's order: the action whose menu name is the words first, then the actions of the context, then the
	// functions of other classes.
	const FString Compact = Name.Replace(TEXT(" "), TEXT(""));
	const auto RankOf = [&Compact](const FAssetDocumentGraphActionRecord& Record)
	{
		const bool bTitle = Record.DisplayName.Replace(TEXT(" "), TEXT("")).Equals(Compact, ESearchCase::IgnoreCase);
		return (bTitle ? 0 : 2) + (Record.bRequiresContext ? 1 : 0);
	};
	if (bKey)
	{
		// The first key the context offers, in CandidateKeys order.
		const FString* Picked = Wanted.FindByPredicate([&Records](const FString& Key)
		{
			return Records.ContainsByPredicate([&Key](const FAssetDocumentGraphActionRecord& Record) { return Record.ActionKey == Key; });
		});
		Records.RemoveAll([Picked](const FAssetDocumentGraphActionRecord& Record) { return Picked == nullptr || Record.ActionKey != *Picked; });
		if (Records.IsEmpty())
		{
			FAssetDocumentGraphActionQuery AllQuery;
			AllQuery.Asset = Host;
			AllQuery.Graph = Graph;
			TArray<FAssetDocumentGraphActionRecord> All;
			FAssetDocumentGraphError Ignored;
			TArray<FString> Known;
			if (Adapter->EnumerateActions(AllQuery, All, Ignored))
			{
				for (const FAssetDocumentGraphActionRecord& Record : All)
				{
					Known.Add(Record.ActionKey);
				}
			}
			const TArray<FString> Suggestions = SuggestActionKeys(Wanted[0], Known);
			OutError = FString::Printf(TEXT("No Blueprint action has key %s in %s (graph %s).%s Search with words instead (name=\"%s\"), or check the key's owner."),
				*Wanted[0], *Host->GetName(), *Graph->GetName(),
				Suggestions.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" Did you mean: %s?"), *FString::Join(Suggestions, TEXT(", "))),
				*Name.Replace(TEXT(":"), TEXT(" ")));
			return false;
		}
	}
	else
	{
		TArray<FString> Words;
		Name.ParseIntoArrayWS(Words);
		Records.RemoveAll([&Words](const FAssetDocumentGraphActionRecord& Record)
		{
			const FString Haystack = Record.ActionKey + TEXT(" ") + Record.DisplayName;
			return Words.ContainsByPredicate([&Haystack](const FString& Word) { return !Haystack.Contains(Word, ESearchCase::IgnoreCase); });
		});
		Records.Sort([&RankOf](const FAssetDocumentGraphActionRecord& A, const FAssetDocumentGraphActionRecord& B)
		{
			const int32 RankA = RankOf(A);
			const int32 RankB = RankOf(B);
			return RankA != RankB ? RankA < RankB : A.ActionKey < B.ActionKey;
		});
		if (Records.Num() > MaxSearchRows)
		{
			Records.SetNum(MaxSearchRows);
		}
	}
	TArray<FString> Keys;
	for (const FAssetDocumentGraphActionRecord& Record : Records)
	{
		Keys.Add(Record.ActionKey);
	}
	TMap<FString, UEdGraphNode*> Templates;
	Adapter->GetTemplateNodes(*Host, *Graph, Keys, Templates);
	for (const FAssetDocumentGraphActionRecord& Record : Records)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Path"), Record.ActionKey);
		Row->SetStringField(TEXT("Title"), Record.DisplayName);
		if (!bKey)
		{
			// The order of the search (list_types orders rows by Rank, then by path).
			Row->SetNumberField(TEXT("Rank"), RankOf(Record));
		}
		if (!Record.Category.IsEmpty())
		{
			Row->SetStringField(TEXT("Category"), Record.Category);
		}
		Row->SetStringField(TEXT("NodeClass"), Record.NodeClassPath);
		TArray<FString> KeyParts;
		Record.ActionKey.ParseIntoArray(KeyParts, TEXT(":"), false);
		if (Record.bRequiresContext && !bKey && KeyParts.Num() == 4)
		{
			Row->SetStringField(TEXT("Target"), KeyParts[2]);
			Row->SetStringField(TEXT("Note"), FString::Printf(TEXT("A function of %s, not of the context: link an object of that class "
				"into its self (Target) pin. list_types target=%s lists what that class offers."), *KeyParts[2], *KeyParts[2]));
		}
		if (UEdGraphNode* const* Template = Templates.Find(Record.ActionKey); Template != nullptr && *Template != nullptr)
		{
			TArray<TSharedPtr<FJsonValue>> Pins;
			for (const UEdGraphPin* Pin : (*Template)->Pins)
			{
				if (Pin != nullptr && !Pin->bHidden)
				{
					Pins.Add(MakeShared<FJsonValueObject>(PinRow(*Pin)));
				}
			}
			Row->SetArrayField(TEXT("Pins"), Pins);
		}
		else
		{
			Row->SetStringField(TEXT("PinsUnavailable"), TEXT("The palette has no template node for this action (it needs a pin context or a placed node)."));
		}
		OutRows.Add(Row);
	}
	return true;
}
} // namespace UE::AssetDocument::Graph::BlueprintNodeLister

namespace UE::AssetDocument::Graph
{
void RegisterBlueprintNodeTypeLister()
{
	RegisterAssetDocumentTypeLister(TEXT("blueprint_node"), &BlueprintNodeLister::ListNodes);
}

void UnregisterBlueprintNodeTypeLister()
{
	UnregisterAssetDocumentTypeLister(TEXT("blueprint_node"));
	BlueprintNodeLister::ScratchBlueprints().Empty();
}
} // namespace UE::AssetDocument::Graph
