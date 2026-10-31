// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AnimBlueprint/AssetDocumentAnimGraphNodeBindingRawEdits.h"

#include "AnimBlueprint/AssetDocumentAnimBlueprintAdapters.h"
#include "Algo/AllOf.h"
#include "AnimBlueprintExtension_PropertyAccess.h"
#include "AnimGraphNode_Base.h"
#include "AnimationGraphSchema.h"
#include "Animation/AnimBlueprint.h"
#include "EdGraph/EdGraphPin.h"
#include "Features/IModularFeatures.h"
#include "IPropertyAccessEditor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::AnimBlueprint::BindingRawEdits
{
namespace BindingDetail
{
using FBindingMap = TMap<FName, FAnimGraphNodePropertyBinding>;

const TCHAR* BindingBaseClassPath = TEXT("/Script/AnimGraph.AnimGraphNodeBinding_Base");

// UAnimGraphNode_Base::Binding (UAnimGraphNodeBinding is declared in AnimGraph's Internal headers).
UObject* FindBindingObject(const UAnimGraphNode_Base& Node)
{
	const FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(UAnimGraphNode_Base::StaticClass(), TEXT("Binding"));
	return Property != nullptr ? Property->GetObjectPropertyValue_InContainer(&Node) : nullptr;
}

// UAnimGraphNodeBinding_Base::PropertyBindings (private to AnimGraph; reached by reflection).
FBindingMap* FindBindingMap(const UAnimGraphNode_Base& Node)
{
	UObject* Binding = FindBindingObject(Node);
	if (Binding == nullptr || Binding->GetClass()->GetPathName() != BindingBaseClassPath)
	{
		return nullptr;
	}
	const FMapProperty* Property = FindFProperty<FMapProperty>(Binding->GetClass(), TEXT("PropertyBindings"));
	if (Property == nullptr || !CastField<FNameProperty>(Property->KeyProp) || !CastField<FStructProperty>(Property->ValueProp)
		|| CastField<FStructProperty>(Property->ValueProp)->Struct->GetFName() != TEXT("AnimGraphNodePropertyBinding"))
	{
		return nullptr;
	}
	return Property->ContainerPtrToValuePtr<FBindingMap>(Binding);
}

IPropertyAccessEditor* PropertyAccessEditor()
{
	IModularFeatures& Features = IModularFeatures::Get();
	return Features.IsModularFeatureAvailable("PropertyAccessEditor")
		? &Features.GetModularFeature<IPropertyAccessEditor>("PropertyAccessEditor") : nullptr;
}

bool ReadPath(const FValue& Value, TArray<FString>& OutPath)
{
	OutPath.Reset();
	if (Value.Kind == EAssetDocumentRuntimeValueKind::String)
	{
		Value.StringValue.ParseIntoArray(OutPath, TEXT("."));
	}
	else if (Value.Kind == EAssetDocumentRuntimeValueKind::Array)
	{
		for (const FValue& Segment : Value.Elements)
		{
			if (Segment.Kind != EAssetDocumentRuntimeValueKind::String || Segment.StringValue.IsEmpty())
			{
				return false;
			}
			OutPath.Add(Segment.StringValue);
		}
	}
	return !OutPath.IsEmpty();
}

// The pure function a path ends in, if any (the binding widget's "Bind > function" entries:
// OnCanBindFunction accepts a pure function without parameters returning a compatible value).
UFunction* LeafFunction(IPropertyAccessEditor& Access, const UStruct* Struct, const TArray<FString>& Path)
{
	UFunction* Leaf = nullptr;
	IPropertyAccessEditor::FResolvePropertyAccessArgs Args;
	Args.FunctionFunction = [&Leaf, Last = Path.Num() - 1](int32 SegmentIndex, UFunction* Function, FProperty*)
	{
		if (SegmentIndex == Last)
		{
			Leaf = Function;
		}
	};
	return Access.ResolvePropertyAccess(Struct, Path, Args).Result == EPropertyAccessResolveResult::Succeeded ? Leaf : nullptr;
}

// The node property a binding name binds: an array element's pin ("BlendWeights_0", FName number 1) binds its array;
// any other name is the property of that whole name (a property may itself be named "Weight_1").
FProperty* BindingProperty(const UAnimGraphNode_Base& Node, FName PropertyName)
{
	FProperty* Whole = Node.GetPinProperty(PropertyName);
	if (PropertyName.GetNumber() > 0 && (Whole == nullptr || !Whole->IsA<FArrayProperty>()))
	{
		const UScriptStruct* NodeType = Node.GetFNodeType();
		return NodeType != nullptr ? NodeType->FindPropertyByName(PropertyName) : nullptr;
	}
	return Whole;
}

// The optional pin of a binding name: an array element's pin shows with its array.
int32 OptionalPinIndex(const UAnimGraphNode_Base& Node, FName PropertyName, bool bElement)
{
	const FName Shown = bElement ? FName(PropertyName, 0) : PropertyName;
	return Node.ShowPinForProperties.IndexOfByPredicate(
		[Shown](const FOptionalPinFromProperty& Pin) { return Pin.PropertyName == Shown; });
}

// The binding's Call Site menu (FPropertyAccessAnimBlueprintBinding): None is Automatic.
const FName* const CallSites[] = {
	&UAnimBlueprintExtension_PropertyAccess::ContextId_UnBatched_ThreadSafe,
	&UAnimBlueprintExtension_PropertyAccess::ContextId_Batched_WorkerThreadPreEventGraph,
	&UAnimBlueprintExtension_PropertyAccess::ContextId_Batched_WorkerThreadPostEventGraph,
	&UAnimBlueprintExtension_PropertyAccess::ContextId_Batched_GameThreadPreEventGraph,
	&UAnimBlueprintExtension_PropertyAccess::ContextId_Batched_GameThreadPostEventGraph,
};

bool ReadContextId(const FValue& Value, FName& OutContext)
{
	if (Value.Kind == EAssetDocumentRuntimeValueKind::Null || (Value.Kind == EAssetDocumentRuntimeValueKind::String && Value.StringValue.IsEmpty()))
	{
		OutContext = NAME_None;
		return true;
	}
	if (Value.Kind != EAssetDocumentRuntimeValueKind::String)
	{
		return false;
	}
	for (const FName* Site : CallSites)
	{
		if (Site->ToString() == Value.StringValue)
		{
			OutContext = *Site;
			return true;
		}
	}
	return false;
}

FString CallSiteList()
{
	TArray<FString> Names = {TEXT("\"\" (Automatic)")};
	for (const FName* Site : CallSites)
	{
		Names.Add(Site->ToString());
	}
	return FString::Join(Names, TEXT(", "));
}
} // namespace BindingDetail

FAssetDocumentRuntimeValueDto QueryNodeBindings(const UAnimGraphNode_Base& Node)
{
	const BindingDetail::FBindingMap* Map = BindingDetail::FindBindingMap(Node);
	if (Map == nullptr || Map->IsEmpty())
	{
		return FAssetDocumentRuntimeValueDto();
	}
	TArray<FName> Names;
	Map->GetKeys(Names);
	Names.Sort(FNameLexicalLess());
	FValue Out = MakeObjectValue();
	for (const FName Name : Names)
	{
		const FAnimGraphNodePropertyBinding& Binding = Map->FindChecked(Name);
		FValue Entry = MakeObjectValue();
		SetField(Entry, TEXT("Path"), MakeStringValue(FString::Join(Binding.PropertyPath, TEXT("."))));
		if (Binding.bOnlyUpdateWhenActive)
		{
			SetField(Entry, TEXT("OnlyUpdateWhenActive"), MakeBoolValue(true));
		}
		if (!Binding.ContextId.IsNone())
		{
			SetField(Entry, TEXT("ContextId"), MakeStringValue(Binding.ContextId.ToString()));
		}
		SetField(Out, Name.ToString(), MoveTemp(Entry));
	}
	return Out;
}

bool SetNodeBindings(UAnimGraphNode_Base& Node, const FAssetDocumentRuntimeValueDto& Value, FAssetDocumentGraphError& OutError)
{
	using namespace BindingDetail;
	if (Value.Kind != EAssetDocumentRuntimeValueKind::Object && Value.Kind != EAssetDocumentRuntimeValueKind::Null)
	{
		OutError.Set(TEXT("Graph.InvalidArgument"),
			TEXT("Bindings is an object {\"<PinProperty>\": {\"Path\": \"Variable\", \"OnlyUpdateWhenActive\": false}}."), TEXT("/value"));
		return false;
	}
	FBindingMap* Map = FindBindingMap(Node);
	UAnimBlueprint* Blueprint = Node.GetAnimBlueprint();
	IPropertyAccessEditor* Access = PropertyAccessEditor();
	if (Map == nullptr || Blueprint == nullptr || Blueprint->SkeletonGeneratedClass == nullptr || Access == nullptr)
	{
		OutError.Set(TEXT("Graph.GraphOperationUnsupported"), FString::Printf(
			TEXT("Node %s has no %s binding (set the Animation Blueprint's DefaultBindingClass)."),
			*Node.GetClass()->GetName(), BindingBaseClassPath), TEXT("/value"));
		return false;
	}
	struct FWanted
	{
		FName Name;
		TArray<FString> Path;
		bool bOnlyUpdateWhenActive = false;
		FProperty* Leaf = nullptr;
		bool bFunction = false;
		// An array element's binding ("BlendWeights_0"): the element index, and the element property it binds.
		int32 ArrayIndex = INDEX_NONE;
		FProperty* Target = nullptr;
		TOptional<FName> ContextId;
	};
	TArray<FWanted> Wanted;
	for (int32 Index = 0; Index < Value.ObjectKeys.Num(); ++Index)
	{
		const FString& Key = Value.ObjectKeys[Index];
		const FValue& Entry = Value.Elements[Index];
		FWanted& Item = Wanted.AddDefaulted_GetRef();
		Item.Name = FName(Key);
		const FValue* Path = FindValueField(Entry, TEXT("Path"));
		const FValue* Only = FindValueField(Entry, TEXT("OnlyUpdateWhenActive"));
		const FValue* Context = FindValueField(Entry, TEXT("ContextId"));
		const bool bKnownFields = Entry.Kind == EAssetDocumentRuntimeValueKind::Object && !Entry.ObjectKeys.ContainsByPredicate(
			[](const FString& Field) { return Field != TEXT("Path") && Field != TEXT("OnlyUpdateWhenActive") && Field != TEXT("ContextId"); });
		if (!bKnownFields || Path == nullptr || !ReadPath(*Path, Item.Path)
			|| (Only != nullptr && Only->Kind != EAssetDocumentRuntimeValueKind::Boolean))
		{
			OutError.Set(TEXT("Graph.InvalidArgument"), FString::Printf(
				TEXT("Binding '%s' needs {\"Path\": \"Variable.Member\", \"OnlyUpdateWhenActive\"?: bool}."), *Key), TEXT("/value"));
			return false;
		}
		Item.bOnlyUpdateWhenActive = Only != nullptr && Only->BooleanValue;
		if (Context != nullptr)
		{
			FName ContextId;
			if (!ReadContextId(*Context, ContextId))
			{
				OutError.Set(TEXT("Graph.InvalidArgument"), FString::Printf(
					TEXT("Binding '%s': ContextId is the binding's call site: %s."), *Key, *CallSiteList()), TEXT("/value"));
				return false;
			}
			Item.ContextId = ContextId;
		}
		FProperty* PinProperty = BindingProperty(Node, Item.Name);
		const bool bElement = Item.Name.GetNumber() > 0 && PinProperty != nullptr && PinProperty->IsA<FArrayProperty>()
			&& PinProperty->GetFName() != Item.Name;
		if (PinProperty == nullptr || OptionalPinIndex(Node, Item.Name, bElement) == INDEX_NONE
			|| (bElement && Node.FindPin(Item.Name) == nullptr))
		{
			OutError.Set(TEXT("Graph.InvalidArgument"), FString::Printf(
				TEXT("%s has no bindable property '%s'."), *Node.GetClass()->GetName(), *Key), TEXT("/value"));
			return false;
		}
		// MakePropertyBindingWidget: an array element pin binds the array's element type at its index.
		Item.ArrayIndex = bElement ? Item.Name.GetNumber() - 1 : INDEX_NONE;
		Item.Target = bElement ? CastField<FArrayProperty>(PinProperty)->Inner : PinProperty;
		int32 ArrayIndex = INDEX_NONE;
		const FPropertyAccessResolveResult Resolved = Access->ResolvePropertyAccess(
			Blueprint->SkeletonGeneratedClass, Item.Path, Item.Leaf, ArrayIndex);
		if (Resolved.Result != EPropertyAccessResolveResult::Succeeded || Item.Leaf == nullptr
			|| Access->GetPropertyCompatibility(Item.Leaf, Item.Target) == EPropertyAccessCompatibility::Incompatible)
		{
			OutError.Set(TEXT("Graph.InvalidArgument"), FString::Printf(
				TEXT("Binding '%s': '%s' is not a property or pure function of %s compatible with the pin."), *Key,
				*FString::Join(Item.Path, TEXT(".")), *Blueprint->GetName()), TEXT("/value"));
			return false;
		}
		if (const UFunction* Function = LeafFunction(*Access, Blueprint->SkeletonGeneratedClass, Item.Path))
		{
			if (Function->NumParms != 1 || Function->GetReturnProperty() == nullptr || !Function->HasAnyFunctionFlags(FUNC_BlueprintPure))
			{
				OutError.Set(TEXT("Graph.InvalidArgument"), FString::Printf(
					TEXT("Binding '%s': function '%s' must be pure, take no parameters and return a value."), *Key,
					*Function->GetName()), TEXT("/value"));
				return false;
			}
			Item.bFunction = true;
		}
	}

	// Nothing to change: the node and its Blueprint are left untouched (not dirtied, not reconstructed).
	TArray<FName> Existing;
	Map->GetKeys(Existing);
	const auto Unchanged = [Map](const FWanted& Item)
	{
		const FAnimGraphNodePropertyBinding* Current = Map->Find(Item.Name);
		const EAnimGraphNodePropertyBindingType Type = Item.bFunction ? EAnimGraphNodePropertyBindingType::Function
			: EAnimGraphNodePropertyBindingType::Property;
		return Current != nullptr && Current->PropertyPath == Item.Path && Current->bOnlyUpdateWhenActive == Item.bOnlyUpdateWhenActive
			&& Current->Type == Type && Current->ArrayIndex == Item.ArrayIndex
			&& (!Item.ContextId.IsSet() || Current->ContextId == Item.ContextId.GetValue());
	};
	if (Existing.Num() == Wanted.Num() && Algo::AllOf(Wanted, Unchanged))
	{
		return true;
	}

	// OnRemoveBinding / OnAddBinding of the details-panel binding widget.
	UObject* BindingObject = FindBindingObject(Node);
	Node.Modify();
	BindingObject->Modify();
	for (const FName Name : Existing)
	{
		if (!Wanted.ContainsByPredicate([Name](const FWanted& Item) { return Item.Name == Name; }))
		{
			Map->Remove(Name);
		}
	}
	const UAnimationGraphSchema* Schema = GetDefault<UAnimationGraphSchema>();
	for (const FWanted& Item : Wanted)
	{
		const FAnimGraphNodePropertyBinding* Current = Map->Find(Item.Name);
		const EAnimGraphNodePropertyBindingType Type = Item.bFunction ? EAnimGraphNodePropertyBindingType::Function
			: EAnimGraphNodePropertyBindingType::Property;
		// A binding written without ContextId keeps the call site the binding has.
		const FName ContextId = Item.ContextId.IsSet() ? Item.ContextId.GetValue() : (Current != nullptr ? Current->ContextId : NAME_None);
		if (Current != nullptr && Current->PropertyPath == Item.Path && Current->bOnlyUpdateWhenActive == Item.bOnlyUpdateWhenActive
			&& Current->Type == Type && Current->ArrayIndex == Item.ArrayIndex)
		{
			if (Current->ContextId != ContextId)
			{
				// The binding's Call Site menu (OnSetPropertyAccessContextId).
				Map->FindChecked(Item.Name).ContextId = ContextId;
			}
			continue;
		}
		// Pins are exposed when bound; a bound pin keeps no links.
		const int32 PinIndex = OptionalPinIndex(Node, Item.Name, Item.ArrayIndex != INDEX_NONE);
		Node.SetPinVisibility(true, PinIndex);
		if (UEdGraphPin* Pin = Node.FindPin(Item.Name))
		{
			Pin->BreakAllPinLinks();
		}
		if (Item.ArrayIndex == INDEX_NONE && Item.Target != nullptr && Item.Target->IsA<FArrayProperty>())
		{
			// A whole-array binding replaces the bindings of its elements.
			const FName Whole(Item.Name, 0);
			for (auto It = Map->CreateIterator(); It; ++It)
			{
				if (It.Key() != Item.Name && FName(It.Key(), 0) == Whole)
				{
					It.RemoveCurrent();
				}
			}
		}
		else if (Item.ArrayIndex != INDEX_NONE)
		{
			// An element binding replaces a binding of the whole array.
			Map->Remove(FName(Item.Name, 0));
		}
		FAnimGraphNodePropertyBinding Binding;
		Binding.PropertyName = Item.Name;
		Binding.ArrayIndex = Item.ArrayIndex;
		Binding.ContextId = ContextId;
		Binding.PropertyPath = Item.Path;
		Binding.PathAsText = Access->MakeTextPath(Item.Path, Blueprint->SkeletonGeneratedClass);
		// OnAddBinding: a binding chain ending in a function is a function binding.
		Binding.Type = Type;
		Binding.bIsBound = true;
		Binding.bOnlyUpdateWhenActive = Item.bOnlyUpdateWhenActive;
		// UAnimGraphNodeBinding_Base::RecalculateBindingType.
		Schema->ConvertPropertyToPinType(Item.Leaf, Binding.PinType);
		if (Item.Target != nullptr && Access->GetPropertyCompatibility(Item.Leaf, Item.Target) == EPropertyAccessCompatibility::Promotable)
		{
			Binding.bIsPromotion = true;
			Schema->ConvertPropertyToPinType(Item.Leaf, Binding.PromotedPinType);
		}
		else
		{
			Binding.bIsPromotion = false;
			Binding.PromotedPinType = Binding.PinType;
		}
		Map->Add(Item.Name, MoveTemp(Binding));
	}
	Node.ReconstructNode();
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	return true;
}
} // namespace UE::AssetDocument::AnimBlueprint::BindingRawEdits
