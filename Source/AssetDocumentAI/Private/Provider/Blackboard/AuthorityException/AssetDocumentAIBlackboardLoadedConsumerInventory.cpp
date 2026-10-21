// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardLoadedConsumerCompletion.h"

#include "AIGraphNode.h"
#include "AssetRegistry/AssetData.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/ValueOrBBKey.h"
#include "BehaviorTreeDecoratorGraph.h"
#include "BehaviorTreeDecoratorGraphNode_Decorator.h"
#include "BehaviorTreeDecoratorGraphNode_Logic.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphNode_Comment.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

using namespace AssetDocumentAIBlackboardValueOrKeyCacheInvalidation;

namespace AssetDocumentAIBlackboardLoadedConsumerCompletion::Private
{
bool VisitValue(const UStruct& Struct, void* Value, const FGuid& GraphGuid,
	const FGuid& NodeGuid, TArray<FTypedPathSegment>& Path, const int32 Depth,
	TArray<FCurrentLeaf>& OutLeaves, FCodecBudget& Budget,
	FCodecBudget& ConsumerPreimageBudget);

bool ExcludedValueContainsAuthoredLeaf(const FProperty& Property, const void* Value,
	const int32 Depth, FCodecBudget& Budget);

bool ExcludedStructContainsAuthoredLeaf(const UStruct& Struct, const void* Value,
	const int32 Depth, FCodecBudget& Budget)
{
	if (Depth > LocatorMaximumDepth() || Value == nullptr) return true;
	TArray<FProperty*> Fields;
	for (TFieldIterator<FProperty> It(&Struct); It; ++It) Fields.Add(*It);
	Fields.Sort([](const FProperty& Left, const FProperty& Right)
	{
		return Left.GetPathName() < Right.GetPathName();
	});
	for (const FProperty* Field : Fields)
	{
		if (++Budget.Fields > LocatorMaximumFields()) return true;
		for (int32 StaticIndex = 0; StaticIndex < Field->ArrayDim; ++StaticIndex)
		{
			if (ExcludedValueContainsAuthoredLeaf(*Field,
				Field->ContainerPtrToValuePtr<void>(Value, StaticIndex), Depth + 1, Budget)) return true;
		}
	}
	return false;
}

bool ExcludedValueContainsAuthoredLeaf(const FProperty& Property, const void* Value,
	const int32 Depth, FCodecBudget& Budget)
{
	if (Depth > LocatorMaximumDepth() || Value == nullptr) return true;
	if (const FStructProperty* StructProperty = CastField<FStructProperty>(&Property))
	{
		if (StructProperty->Struct == FBlackboardKeySelector::StaticStruct())
		{
			return true;
		}
		EExactValueOrBlackboardKeyFamily Family;
		if (ExactValueOrStruct(*StructProperty->Struct, Family) != nullptr)
		{
			return true;
		}
		if (StructProperty->Struct == FInstancedStruct::StaticStruct())
		{
			const FInstancedStruct& Instance = *static_cast<const FInstancedStruct*>(Value);
			return Instance.IsValid() && ExcludedStructContainsAuthoredLeaf(
				*Instance.GetScriptStruct(), Instance.GetMemory(), Depth + 1, Budget);
		}
		return ExcludedStructContainsAuthoredLeaf(*StructProperty->Struct, Value, Depth + 1, Budget);
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		FScriptArrayHelper Helper(Array, Value);
		if (Budget.Elements > LocatorMaximumElements() - Helper.Num()) return true;
		Budget.Elements += Helper.Num();
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
			if (ExcludedValueContainsAuthoredLeaf(*Array->Inner, Helper.GetRawPtr(Index), Depth + 1, Budget)) return true;
		return false;
	}
	if (const FMapProperty* Map = CastField<FMapProperty>(&Property))
	{
		FScriptMapHelper Helper(Map, Value);
		if (Budget.Elements > LocatorMaximumElements() - Helper.Num()) return true;
		Budget.Elements += Helper.Num();
		for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
		{
			if (Helper.IsValidIndex(Index) && (ExcludedValueContainsAuthoredLeaf(
				*Map->KeyProp, Helper.GetKeyPtr(Index), Depth + 1, Budget)
				|| ExcludedValueContainsAuthoredLeaf(*Map->ValueProp,
					Helper.GetValuePtr(Index), Depth + 1, Budget))) return true;
		}
		return false;
	}
	if (const FSetProperty* Set = CastField<FSetProperty>(&Property))
	{
		FScriptSetHelper Helper(Set, Value);
		if (Budget.Elements > LocatorMaximumElements() - Helper.Num()) return true;
		Budget.Elements += Helper.Num();
		for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
			if (Helper.IsValidIndex(Index) && ExcludedValueContainsAuthoredLeaf(
				*Set->ElementProp, Helper.GetElementPtr(Index), Depth + 1, Budget)) return true;
		return false;
	}
	return !(Property.IsA<FBoolProperty>() || Property.IsA<FNumericProperty>()
		|| Property.IsA<FEnumProperty>() || Property.IsA<FNameProperty>()
		|| Property.IsA<FStrProperty>() || Property.IsA<FObjectProperty>()
		|| Property.IsA<FSoftObjectProperty>());
}

bool VisitProperty(const FProperty& Property, void* Value,
	const int32 StaticArrayOrdinal, const FGuid& GraphGuid, const FGuid& NodeGuid,
	TArray<FTypedPathSegment>& Path, const int32 Depth,
	TArray<FCurrentLeaf>& OutLeaves, FCodecBudget& Budget,
	FCodecBudget& ConsumerPreimageBudget)
{
	if (Depth > LocatorMaximumDepth()) return false;
	if (const FStructProperty* StructProperty = CastField<FStructProperty>(&Property))
	{
		if (StructProperty->Struct == FBlackboardKeySelector::StaticStruct())
		{
			FCurrentLeaf& Leaf = OutLeaves.AddDefaulted_GetRef();
			Leaf.Locator.GraphGuid = GraphGuid;
			Leaf.Locator.NodeGuid = NodeGuid;
			Leaf.Locator.Segments = Path;
			Leaf.Locator.Kind = ELeafKind::Selector;
			Leaf.Selector = static_cast<FBlackboardKeySelector*>(Value);
			Leaf.AuthoredKeyName = Leaf.Selector->SelectedKeyName;
			return EncodeLeafAuthoredFingerprint(Property, Value, Leaf.AuthoredFingerprint,
				&ConsumerPreimageBudget);
		}
		EExactValueOrBlackboardKeyFamily Family;
		if (UScriptStruct* ExactStruct = ExactValueOrStruct(*StructProperty->Struct, Family))
		{
			FCurrentLeaf& Leaf = OutLeaves.AddDefaulted_GetRef();
			Leaf.Locator.GraphGuid = GraphGuid;
			Leaf.Locator.NodeGuid = NodeGuid;
			Leaf.Locator.Segments = Path;
			Leaf.Locator.Kind = ELeafKind::ExactValueOr;
			Leaf.Locator.Family = Family;
			Leaf.Family = Family;
			Leaf.ValueOr = Value;
			Leaf.DeclaredStruct = ExactStruct;
			Leaf.AuthoredKeyName = ValueOrKey(Value, Family);
			return EncodeLeafAuthoredFingerprint(Property, Value, Leaf.AuthoredFingerprint,
				&ConsumerPreimageBudget);
		}
		if (StructProperty->Struct == FInstancedStruct::StaticStruct())
		{
			FInstancedStruct& Instance = *static_cast<FInstancedStruct*>(Value);
			if (!Instance.IsValid()) return false;
			FTypedPathSegment& Segment = Path.AddDefaulted_GetRef();
			Segment.Kind = ETypedPathSegmentKind::InstancedStruct;
			Segment.ContainedStructPath = Instance.GetScriptStruct()->GetPathName();
			const bool bVisited = VisitValue(*Instance.GetScriptStruct(),
				Instance.GetMutableMemory(), GraphGuid, NodeGuid, Path,
				Depth + 1, OutLeaves, Budget, ConsumerPreimageBudget);
			Path.Pop();
			return bVisited;
		}
		return VisitValue(*StructProperty->Struct, Value, GraphGuid, NodeGuid,
			Path, Depth + 1, OutLeaves, Budget, ConsumerPreimageBudget);
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		FScriptArrayHelper Helper(Array, Value);
		if (Budget.Elements > LocatorMaximumElements() - Helper.Num()) return false;
		Budget.Elements += Helper.Num();
		TSet<FString> Digests;
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			FString Digest;
			if (!EncodeBlackboardBTLeafLocatorDigestV1(*Array->Inner,
				Helper.GetRawPtr(Index), Digest, &ConsumerPreimageBudget)
				|| Digests.Contains(Digest)) return false;
			Digests.Add(Digest);
			FTypedPathSegment& Segment = Path.AddDefaulted_GetRef();
			Segment.Kind = ETypedPathSegmentKind::ArrayElementDigest;
			Segment.Digest = Digest;
			const bool bVisited = VisitProperty(*Array->Inner, Helper.GetRawPtr(Index),
				0, GraphGuid, NodeGuid, Path, Depth + 1, OutLeaves, Budget,
				ConsumerPreimageBudget);
			Path.Pop();
			if (!bVisited) return false;
		}
		return true;
	}
	if (const FMapProperty* Map = CastField<FMapProperty>(&Property))
	{
		FScriptMapHelper Helper(Map, Value);
		if (Budget.Elements > LocatorMaximumElements() - Helper.Num()) return false;
		Budget.Elements += Helper.Num();
		TSet<FString> Digests;
		for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
		{
			if (!Helper.IsValidIndex(Index)) continue;
			FString Digest;
			if (!EncodeBlackboardBTLeafLocatorDigestV1(*Map->KeyProp,
				Helper.GetKeyPtr(Index), Digest, &ConsumerPreimageBudget)
				|| Digests.Contains(Digest)) return false;
			Digests.Add(Digest);
			FTypedPathSegment& Segment = Path.AddDefaulted_GetRef();
			Segment.Kind = ETypedPathSegmentKind::MapValueKeyDigest;
			Segment.Digest = Digest;
			const bool bVisited = VisitProperty(*Map->ValueProp, Helper.GetValuePtr(Index),
				0, GraphGuid, NodeGuid, Path, Depth + 1, OutLeaves, Budget,
				ConsumerPreimageBudget);
			Path.Pop();
			if (!bVisited) return false;
		}
		return true;
	}
	if (Property.IsA<FSetProperty>())
	{
		return false;
	}
	FString IgnoredDigest;
	return EncodeBlackboardBTLeafLocatorDigestV1(Property, Value, IgnoredDigest,
		&ConsumerPreimageBudget);
}

bool VisitValue(const UStruct& Struct, void* Value, const FGuid& GraphGuid,
	const FGuid& NodeGuid, TArray<FTypedPathSegment>& Path, const int32 Depth,
	TArray<FCurrentLeaf>& OutLeaves, FCodecBudget& Budget,
	FCodecBudget& ConsumerPreimageBudget)
{
	if (Depth > LocatorMaximumDepth() || Value == nullptr) return false;
	TArray<FProperty*> Fields;
	for (TFieldIterator<FProperty> It(&Struct); It; ++It) Fields.Add(*It);
	Fields.Sort([](const FProperty& Left, const FProperty& Right)
	{
		return Left.GetPathName() < Right.GetPathName();
	});
	for (FProperty* Field : Fields)
	{
		if (++Budget.Fields > LocatorMaximumFields()) return false;
		if (IsExcludedProperty(*Field))
		{
			for (int32 StaticIndex = 0; StaticIndex < Field->ArrayDim; ++StaticIndex)
				if (ExcludedValueContainsAuthoredLeaf(*Field,
					Field->ContainerPtrToValuePtr<void>(Value, StaticIndex), Depth + 1, Budget)) return false;
			continue;
		}
		for (int32 StaticIndex = 0; StaticIndex < Field->ArrayDim; ++StaticIndex)
		{
			FTypedPathSegment& Segment = Path.AddDefaulted_GetRef();
			Segment.Kind = ETypedPathSegmentKind::Field;
			Segment.DeclaringOwnerPath = Field->GetOwnerStruct()->GetPathName();
			Segment.PropertyName = Field->GetFName();
			Segment.PropertyClassPath = PropertyClassIdentity(*Field);
			Segment.StaticArrayOrdinal = StaticIndex;
			const bool bVisited = VisitProperty(*Field,
				Field->ContainerPtrToValuePtr<void>(Value, StaticIndex), StaticIndex,
				GraphGuid, NodeGuid, Path, Depth + 1, OutLeaves, Budget,
				ConsumerPreimageBudget);
			Path.Pop();
			if (!bVisited)
			{
				return false;
			}
		}
	}
	return true;
}

bool InventoryGraph(UBehaviorTree& Tree, UEdGraph& Graph, const UObject& ExpectedOuter,
	TSet<FGuid>& GraphGuids,
	TSet<FGuid>& NodeGuids, TSet<const UEdGraphNode*>& VisitedNodes,
	TArray<FCurrentLeaf>& OutLeaves, FCodecBudget& ConsumerPreimageBudget)
{
	if (Graph.GetOuter() != &ExpectedOuter || !Graph.GraphGuid.IsValid()
		|| GraphGuids.Contains(Graph.GraphGuid)) return false;
	GraphGuids.Add(Graph.GraphGuid);
	struct FNestedGraph { UEdGraph* Graph = nullptr; UObject* Owner = nullptr; };
	TArray<FNestedGraph> NestedGraphs;
	TFunction<bool(UEdGraphNode*)> VisitNode = [&](UEdGraphNode* Node)
	{
		if (Node == nullptr) return false;
		if (VisitedNodes.Contains(Node)) return true;
		if (Node->GetOuter() != &Graph || Node->GetGraph() != &Graph) return false;
		VisitedNodes.Add(Node);
		if (!Node->NodeGuid.IsValid() || NodeGuids.Contains(Node->NodeGuid)) return false;
		NodeGuids.Add(Node->NodeGuid);
		UObject* NodeInstance = nullptr;
		if (UAIGraphNode* AINode = Cast<UAIGraphNode>(Node))
		{
			NodeInstance = AINode->NodeInstance;
			if (NodeInstance == nullptr)
			{
				UBehaviorTreeGraphNode_CompositeDecorator* Composite =
					Cast<UBehaviorTreeGraphNode_CompositeDecorator>(Node);
				if (!Node->IsA<UBehaviorTreeGraphNode_Root>()
					&& (Composite == nullptr || Composite->GetBoundGraph() == nullptr)) return false;
			}
		}
		else if (Graph.IsA<UBehaviorTreeDecoratorGraph>()
			&& Node->GetClass() == UBehaviorTreeDecoratorGraphNode_Decorator::StaticClass())
		{
			NodeInstance = CastChecked<UBehaviorTreeDecoratorGraphNode_Decorator>(Node)->NodeInstance;
			if (NodeInstance == nullptr) return false;
		}
		else if (!(Graph.IsA<UBehaviorTreeDecoratorGraph>()
			&& Node->GetClass() == UBehaviorTreeDecoratorGraphNode_Logic::StaticClass())
			&& Node->GetClass() != UEdGraphNode_Comment::StaticClass())
		{
			return false;
		}
		if (NodeInstance != nullptr)
		{
			if (NodeInstance->GetTypedOuter<UBehaviorTree>() != &Tree) return false;
			FCodecBudget NodeBudget;
			TArray<FTypedPathSegment> Path;
			if (!VisitValue(*NodeInstance->GetClass(), NodeInstance,
				Graph.GraphGuid, Node->NodeGuid, Path, 0, OutLeaves, NodeBudget,
				ConsumerPreimageBudget)) return false;
		}
		if (UBehaviorTreeGraphNode* BTNode = Cast<UBehaviorTreeGraphNode>(Node))
		{
			for (UBehaviorTreeGraphNode* Decorator : BTNode->Decorators) if (!VisitNode(Decorator)) return false;
			for (UBehaviorTreeGraphNode* Service : BTNode->Services) if (!VisitNode(Service)) return false;
			if (UEdGraph* Bound = BTNode->GetBoundGraph()) NestedGraphs.Add({Bound, BTNode});
		}
		return true;
	};
	for (UEdGraphNode* Node : Graph.Nodes) if (!VisitNode(Node)) return false;
	for (const FNestedGraph& Nested : NestedGraphs)
	{
		if (Nested.Graph == nullptr || Nested.Owner == nullptr
			|| !InventoryGraph(Tree, *Nested.Graph, *Nested.Owner, GraphGuids, NodeGuids,
			VisitedNodes, OutLeaves, ConsumerPreimageBudget)) return false;
	}
	return true;
}

bool InventoryTree(UBehaviorTree& Tree, TArray<FCurrentLeaf>& OutLeaves,
	TArray<FGuid>& OutGraphGuids, TArray<FGuid>& OutNodeGuids)
{
	UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(Tree.BTGraph);
	if (Graph == nullptr || Graph->GetOuter() != &Tree) return false;
	TSet<FGuid> GraphGuids;
	TSet<FGuid> NodeGuids;
	TSet<const UEdGraphNode*> VisitedNodes;
	FCodecBudget ConsumerPreimageBudget;
	if (!InventoryGraph(Tree, *Graph, Tree, GraphGuids, NodeGuids,
		VisitedNodes, OutLeaves, ConsumerPreimageBudget)) return false;
	OutGraphGuids = GraphGuids.Array();
	OutNodeGuids = NodeGuids.Array();
	auto SortGuids = [](TArray<FGuid>& Guids)
	{
		Guids.Sort([](const FGuid& Left, const FGuid& Right)
		{
			return Left.ToString(EGuidFormats::Digits) < Right.ToString(EGuidFormats::Digits);
		});
	};
	SortGuids(OutGraphGuids);
	SortGuids(OutNodeGuids);
	OutLeaves.Sort([](const FCurrentLeaf& Left, const FCurrentLeaf& Right)
	{
		return LeafLocatorSortKey(Left.Locator) < LeafLocatorSortKey(Right.Locator);
	});
	for (int32 Index = 1; Index < OutLeaves.Num(); ++Index)
	{
		if (SameLeafLocator(OutLeaves[Index].Locator, OutLeaves[Index - 1].Locator)) return false;
	}
	return true;
}

bool EffectiveBlackboardContains(const UBehaviorTree& Tree, const UBlackboardData& Target)
{
	const UBlackboardData* Effective = Tree.BlackboardAsset;
	return Effective == &Target || (Effective != nullptr && Effective->IsChildOf(Target));
}

bool ResolveLoadedExactAsset(const FAssetData& Asset, const UClass& ExactClass,
	UObject*& OutObject)
{
	OutObject = nullptr;
	if (!Asset.IsValid() || Asset.AssetClassPath != ExactClass.GetClassPathName()
		|| !Asset.IsAssetLoaded())
	{
		return false;
	}
	UObject* Object = Asset.FastGetAsset(false);
	if (Object == nullptr || Object != Asset.GetSoftObjectPath().ResolveObject()
		|| Object->GetClass() != &ExactClass
		|| FSoftObjectPath(Object) != Asset.GetSoftObjectPath())
	{
		return false;
	}
	OutObject = Object;
	return true;
}

bool CaptureConsumers(UBlackboardData& Target, TConstArrayView<FAssetData> Assets,
	TArray<FConsumer>& OutConsumers, TArray<FString>& OutSkippedUnloaded)
{
	for (const FAssetData& Asset : Assets)
	{
		if (!Asset.IsAssetLoaded())
		{
			OutSkippedUnloaded.Add(Asset.GetObjectPathString());
			continue;
		}
		UObject* Object = nullptr;
		if (!ResolveLoadedExactAsset(Asset, *UBehaviorTree::StaticClass(), Object)) return false;
		UBehaviorTree& Tree = *CastChecked<UBehaviorTree>(Object);
		if (!EffectiveBlackboardContains(Tree, Target)) continue;
		FConsumer& Consumer = OutConsumers.AddDefaulted_GetRef();
		Consumer.AssetPath = Asset.GetObjectPathString();
		Consumer.Tree.Reset(&Tree);
		Consumer.bPackageDirty = Tree.GetOutermost()->IsDirty();
		TArray<FCurrentLeaf> Current;
		if (!InventoryTree(Tree, Current, Consumer.GraphGuids, Consumer.NodeGuids)) return false;
		for (const FCurrentLeaf& Leaf : Current)
		{
			Consumer.Leaves.Add({Leaf.Locator, Leaf.AuthoredFingerprint, Leaf.AuthoredKeyName});
		}
	}
	OutConsumers.Sort([](const FConsumer& Left, const FConsumer& Right)
	{
		return Left.AssetPath < Right.AssetPath;
	});
	for (int32 Index = 1; Index < OutConsumers.Num(); ++Index)
	{
		if (OutConsumers[Index].AssetPath == OutConsumers[Index - 1].AssetPath) return false;
	}
	OutSkippedUnloaded.Sort();
	for (int32 Index = 1; Index < OutSkippedUnloaded.Num(); ++Index)
	{
		if (OutSkippedUnloaded[Index] == OutSkippedUnloaded[Index - 1]) return false;
	}
	return true;
}

} // namespace AssetDocumentAIBlackboardLoadedConsumerCompletion::Private
