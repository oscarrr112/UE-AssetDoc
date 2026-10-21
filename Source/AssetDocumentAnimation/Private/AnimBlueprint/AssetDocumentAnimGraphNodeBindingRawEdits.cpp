// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AnimBlueprint/AssetDocumentAnimGraphNodeBindingRawEdits.h"

#include "AnimBlueprint/AssetDocumentAnimBlueprintAdapters.h"
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

int32 OptionalPinIndex(const UAnimGraphNode_Base& Node, FName PropertyName)
{
	return Node.ShowPinForProperties.IndexOfByPredicate(
		[PropertyName](const FOptionalPinFromProperty& Pin) { return Pin.PropertyName == PropertyName; });
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
		const bool bKnownFields = Entry.Kind == EAssetDocumentRuntimeValueKind::Object && !Entry.ObjectKeys.ContainsByPredicate(
			[](const FString& Field) { return Field != TEXT("Path") && Field != TEXT("OnlyUpdateWhenActive"); });
		if (!bKnownFields || Path == nullptr || !ReadPath(*Path, Item.Path)
			|| (Only != nullptr && Only->Kind != EAssetDocumentRuntimeValueKind::Boolean))
		{
			OutError.Set(TEXT("Graph.InvalidArgument"), FString::Printf(
				TEXT("Binding '%s' needs {\"Path\": \"Variable.Member\", \"OnlyUpdateWhenActive\"?: bool}."), *Key), TEXT("/value"));
			return false;
		}
		Item.bOnlyUpdateWhenActive = Only != nullptr && Only->BooleanValue;
		FProperty* PinProperty = Node.GetPinProperty(Item.Name);
		if (PinProperty == nullptr || OptionalPinIndex(Node, Item.Name) == INDEX_NONE)
		{
			OutError.Set(TEXT("Graph.InvalidArgument"), FString::Printf(
				TEXT("%s has no bindable property '%s'."), *Node.GetClass()->GetName(), *Key), TEXT("/value"));
			return false;
		}
		int32 ArrayIndex = INDEX_NONE;
		const FPropertyAccessResolveResult Resolved = Access->ResolvePropertyAccess(
			Blueprint->SkeletonGeneratedClass, Item.Path, Item.Leaf, ArrayIndex);
		if (Resolved.Result != EPropertyAccessResolveResult::Succeeded || Item.Leaf == nullptr
			|| Access->GetPropertyCompatibility(Item.Leaf, PinProperty) == EPropertyAccessCompatibility::Incompatible)
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

	// OnRemoveBinding / OnAddBinding of the details-panel binding widget.
	UObject* BindingObject = FindBindingObject(Node);
	Node.Modify();
	BindingObject->Modify();
	TArray<FName> Existing;
	Map->GetKeys(Existing);
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
		if (Current != nullptr && Current->PropertyPath == Item.Path && Current->bOnlyUpdateWhenActive == Item.bOnlyUpdateWhenActive
			&& Current->Type == Type)
		{
			continue;
		}
		// Pins are exposed when bound; a bound pin keeps no links.
		const int32 PinIndex = OptionalPinIndex(Node, Item.Name);
		Node.SetPinVisibility(true, PinIndex);
		if (UEdGraphPin* Pin = Node.FindPin(Item.Name))
		{
			Pin->BreakAllPinLinks();
		}
		FAnimGraphNodePropertyBinding Binding;
		Binding.PropertyName = Item.Name;
		Binding.PropertyPath = Item.Path;
		Binding.PathAsText = Access->MakeTextPath(Item.Path, Blueprint->SkeletonGeneratedClass);
		// OnAddBinding: a binding chain ending in a function is a function binding.
		Binding.Type = Type;
		Binding.bIsBound = true;
		Binding.bOnlyUpdateWhenActive = Item.bOnlyUpdateWhenActive;
		// UAnimGraphNodeBinding_Base::RecalculateBindingType.
		Schema->ConvertPropertyToPinType(Item.Leaf, Binding.PinType);
		FProperty* PinProperty = Node.GetPinProperty(Item.Name);
		if (PinProperty != nullptr && Access->GetPropertyCompatibility(Item.Leaf, PinProperty) == EPropertyAccessCompatibility::Promotable)
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
