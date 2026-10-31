// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Adapters/AssetDocumentGraphPropertyAccessNode.h"

#include "Algo/Find.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Features/IModularFeatures.h"
#include "IPropertyAccessEditor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::Graph::PropertyAccessNode
{
namespace PropertyAccessNodeDetail
{
const TCHAR* const NodeClassPath = TEXT("/Script/PropertyAccessNode.K2Node_PropertyAccess");

// The Call Site menu of the node (UAnimBlueprintExtension_PropertyAccess::ContextId_*); None is Automatic.
const TCHAR* const CallSites[] = {
	TEXT("UnBatched_ThreadSafe"),
	TEXT("Batched_WorkerThreadPreEventGraph"),
	TEXT("Batched_WorkerThreadPostEventGraph"),
	TEXT("Batched_GameThreadPreEventGraph"),
	TEXT("Batched_GameThreadPostEventGraph"),
};

FArrayProperty* PathProperty(const UEdGraphNode& Node)
{
	FArrayProperty* Property = FindFProperty<FArrayProperty>(Node.GetClass(), TEXT("Path"));
	return Property != nullptr && CastField<FStrProperty>(Property->Inner) != nullptr ? Property : nullptr;
}

FNameProperty* ContextProperty(const UEdGraphNode& Node)
{
	return FindFProperty<FNameProperty>(Node.GetClass(), TEXT("ContextId"));
}

const TArray<FString>* PathOf(const UEdGraphNode& Node)
{
	const FArrayProperty* Property = PathProperty(Node);
	return Property != nullptr ? Property->ContainerPtrToValuePtr<TArray<FString>>(&Node) : nullptr;
}

bool ReadPath(const FValue& Value, TArray<FString>& OutPath)
{
	OutPath.Reset();
	if (Value.Kind == EAssetDocumentRuntimeValueKind::String)
	{
		Value.StringValue.ParseIntoArray(OutPath, TEXT("."));
		return true;
	}
	if (Value.Kind != EAssetDocumentRuntimeValueKind::Array)
	{
		return false;
	}
	for (const FValue& Segment : Value.Elements)
	{
		if (Segment.Kind != EAssetDocumentRuntimeValueKind::String || Segment.StringValue.IsEmpty())
		{
			return false;
		}
		OutPath.Add(Segment.StringValue);
	}
	return true;
}

UBlueprint* BlueprintOf(const UEdGraphNode& Node)
{
	return FBlueprintEditorUtils::FindBlueprintForNode(&Node);
}

bool SetPath(UEdGraphNode& Node, const FValue& Value, FAssetDocumentGraphError& OutError)
{
	TArray<FString> Wanted;
	if (!ReadPath(Value, Wanted))
	{
		OutError.Set(TEXT("Graph.InvalidArgument"),
			TEXT("Path is the property path the node reads: \"Member.Member\" (or a list of segments), \"\" for none."), TEXT("/value"));
		return false;
	}
	const FArrayProperty* Property = PathProperty(Node);
	TArray<FString>* Current = Property != nullptr ? Property->ContainerPtrToValuePtr<TArray<FString>>(&Node) : nullptr;
	UBlueprint* Blueprint = BlueprintOf(Node);
	if (Current == nullptr || Blueprint == nullptr)
	{
		OutError.Set(TEXT("Graph.GraphOperationUnsupported"), TEXT("This Property Access node has no path to set."), TEXT("/property_path"));
		return false;
	}
	if (*Current == Wanted)
	{
		return true;
	}
	if (!Wanted.IsEmpty())
	{
		// The path picker offers only paths that resolve on the Blueprint (its variables, components, functions...).
		IModularFeatures& Features = IModularFeatures::Get();
		IPropertyAccessEditor* Access = Features.IsModularFeatureAvailable("PropertyAccessEditor")
			? &Features.GetModularFeature<IPropertyAccessEditor>("PropertyAccessEditor") : nullptr;
		FProperty* Leaf = nullptr;
		int32 ArrayIndex = INDEX_NONE;
		if (Access == nullptr || Blueprint->SkeletonGeneratedClass == nullptr
			|| Access->ResolvePropertyAccess(Blueprint->SkeletonGeneratedClass, Wanted, Leaf, ArrayIndex).Result
				!= EPropertyAccessResolveResult::Succeeded)
		{
			OutError.Set(TEXT("Graph.InvalidArgument"), FString::Printf(
				TEXT("Path '%s' does not resolve on %s: each segment names a variable, a component, a member or a pure function "
					 "without parameters of the previous one."), *FString::Join(Wanted, TEXT(".")), *Blueprint->GetName()),
				TEXT("/value"));
			return false;
		}
	}
	// UK2Node_PropertyAccess::SetPath / ClearPath: the reconstruction resolves the output pin and the text path.
	Node.Modify();
	*Current = MoveTemp(Wanted);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Node.ReconstructNode();
	return true;
}

bool SetContext(UEdGraphNode& Node, const FValue& Value, FAssetDocumentGraphError& OutError)
{
	const FNameProperty* Property = ContextProperty(Node);
	const bool bText = Value.Kind == EAssetDocumentRuntimeValueKind::String || Value.Kind == EAssetDocumentRuntimeValueKind::Null;
	const FString Text = Value.Kind == EAssetDocumentRuntimeValueKind::String ? Value.StringValue : FString();
	const bool bKnown = Text.IsEmpty() || Algo::FindByPredicate(CallSites, [&Text](const TCHAR* Site) { return Text == Site; }) != nullptr;
	if (Property == nullptr || !bText || !bKnown)
	{
		TArray<FString> Sites = {TEXT("\"\" (Automatic)")};
		for (const TCHAR* Site : CallSites)
		{
			Sites.Add(Site);
		}
		OutError.Set(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("ContextId is the node's call site: %s."),
			*FString::Join(Sites, TEXT(", "))), TEXT("/value"));
		return false;
	}
	const FName Wanted = Text.IsEmpty() ? FName() : FName(*Text);
	FName* Current = Property->ContainerPtrToValuePtr<FName>(&Node);
	if (*Current == Wanted)
	{
		return true;
	}
	// The Call Site menu (SetContextId), then the Blueprint is modified as the menu's binding context does.
	Node.Modify();
	*Current = Wanted;
	if (UBlueprint* Blueprint = BlueprintOf(Node))
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}
	return true;
}
} // namespace PropertyAccessNodeDetail

bool IsPropertyAccessNode(const UEdGraphNode& Node)
{
	return Node.GetClass()->GetPathName() == PropertyAccessNodeDetail::NodeClassPath;
}

void Describe(const UEdGraphNode& Node, FObjectBuilder& Settings)
{
	using namespace PropertyAccessNodeDetail;
	if (const TArray<FString>* Path = PathOf(Node))
	{
		Settings.AddString(TEXT("Path"), FString::Join(*Path, TEXT(".")));
	}
	if (const FNameProperty* Property = ContextProperty(Node))
	{
		const FName Context = Property->GetPropertyValue_InContainer(&Node);
		if (!Context.IsNone())
		{
			Settings.AddString(TEXT("ContextId"), Context.ToString());
		}
	}
}

bool SetSetting(UEdGraphNode& Node, FStringView Path, const FValue& Value, bool& bOutHandled, FAssetDocumentGraphError& OutError)
{
	bOutHandled = false;
	if (!IsPropertyAccessNode(Node) || (Path != TEXTVIEW("Path") && Path != TEXTVIEW("ContextId")))
	{
		return true;
	}
	bOutHandled = true;
	return Path == TEXTVIEW("Path") ? PropertyAccessNodeDetail::SetPath(Node, Value, OutError)
		: PropertyAccessNodeDetail::SetContext(Node, Value, OutError);
}
} // namespace UE::AssetDocument::Graph::PropertyAccessNode
