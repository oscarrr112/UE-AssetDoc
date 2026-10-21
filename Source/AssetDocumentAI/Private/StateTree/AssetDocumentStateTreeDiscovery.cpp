// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// StateTree type discovery hooks (list_types / inspect, AssetDocumentTypeDiscovery.h), as the node picker
// and the node Details panel present node types:
//   inspect of a node struct -> Fields: the node struct values an author sets (document `Node`; the
//     FStateTreeNodeBase internals such as Name, BindingsBatch, InstanceDataHandle are not shown),
//     InstanceType and InstanceFields: the instance data values (document `Instance`) with their
//     Usage (Parameter: set by value or bound; Input: must be bound), Outputs: what the node writes
//     (binding sources, not authored).
//   list_types schema=<StateTree schema class> -> UStateTreeSchema::IsStructAllowed / IsClassAllowed,
//     the picker's schema filter (SStateTreeNodeTypePicker::CacheNodeTypes).

#include "StateTree/AssetDocumentStateTreePrimitives.h"

#include "AssetDocumentTypeDiscovery.h"
#include "Dom/JsonValue.h"
#include "StateTreeNodeBase.h"
#include "StateTreePropertyBindings.h"
#include "StateTreeSchema.h"
#include "StructUtils/InstancedStruct.h"

namespace UE::AssetDocument::Graph::StateTree
{
namespace
{
const FName DiscoveryHooksId(TEXT("ai.state_tree.discovery"));

const TCHAR* UsageName(EStateTreePropertyUsage Usage)
{
	return Usage == EStateTreePropertyUsage::Input ? TEXT("Input") : TEXT("Parameter");
}

void DescribeFields(const UStruct& Type, EValueOwner Owner, TArray<TSharedPtr<FJsonValue>>& OutAuthored,
	TArray<TSharedPtr<FJsonValue>>* OutOutputs)
{
	const bool bInstance = Owner == EValueOwner::InstanceStruct || Owner == EValueOwner::InstanceObject;
	for (TFieldIterator<FProperty> It(&Type); It; ++It)
	{
		const FProperty& Property = **It;
		if (IsAuthoredProperty(Property, Owner))
		{
			TSharedPtr<FJsonObject> Field = DescribeAssetDocumentTypeField(Property);
			if (bInstance)
			{
				Field->SetStringField(TEXT("Usage"), UsageName(UE::StateTree::GetUsageFromMetaData(&Property)));
			}
			OutAuthored.Add(MakeShared<FJsonValueObject>(Field));
		}
		else if (OutOutputs != nullptr && !Property.HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)
			&& UE::StateTree::GetUsageFromMetaData(&Property) == EStateTreePropertyUsage::Output)
		{
			OutOutputs->Add(MakeShared<FJsonValueObject>(DescribeAssetDocumentTypeField(Property)));
		}
	}
}

bool DescribeNodeStruct(const UScriptStruct& Struct, FJsonObject& InOutPayload)
{
	if (!Struct.IsChildOf(FStateTreeNodeBase::StaticStruct()))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> NodeFields;
	DescribeFields(Struct, EValueOwner::NodeStruct, NodeFields, nullptr);
	InOutPayload.SetArrayField(TEXT("Fields"), NodeFields);

	const FInstancedStruct Probe(&Struct);
	const UStruct* InstanceType = Probe.IsValid() ? Probe.Get<FStateTreeNodeBase>().GetInstanceDataType() : nullptr;
	if (InstanceType != nullptr)
	{
		TArray<TSharedPtr<FJsonValue>> InstanceFields;
		TArray<TSharedPtr<FJsonValue>> Outputs;
		DescribeFields(*InstanceType, InstanceType->IsA<UClass>() ? EValueOwner::InstanceObject : EValueOwner::InstanceStruct,
			InstanceFields, &Outputs);
		InOutPayload.SetStringField(TEXT("InstanceType"), InstanceType->GetPathName());
		InOutPayload.SetArrayField(TEXT("InstanceFields"), InstanceFields);
		if (!Outputs.IsEmpty())
		{
			InOutPayload.SetArrayField(TEXT("Outputs"), Outputs);
		}
	}
	return true;
}

bool IsTypeAllowedBySchema(const UClass& Schema, const UStruct& Type, bool& bOutAllowed)
{
	if (!Schema.IsChildOf(UStateTreeSchema::StaticClass()))
	{
		return false;
	}
	const UStateTreeSchema* Defaults = Schema.GetDefaultObject<UStateTreeSchema>();
	if (const UScriptStruct* Struct = Cast<UScriptStruct>(&Type))
	{
		bOutAllowed = Defaults != nullptr && Defaults->IsStructAllowed(Struct);
	}
	else
	{
		const UClass* Class = Cast<UClass>(&Type);
		bOutAllowed = Defaults != nullptr && Class != nullptr && Defaults->IsClassAllowed(Class);
	}
	return true;
}
} // namespace
} // namespace UE::AssetDocument::Graph::StateTree

void RegisterAssetDocumentStateTreeDiscoveryHooks()
{
	using namespace UE::AssetDocument::Graph::StateTree;
	FAssetDocumentTypeDiscoveryHooks Hooks;
	Hooks.DescribeStruct = &DescribeNodeStruct;
	Hooks.IsTypeAllowed = &IsTypeAllowedBySchema;
	RegisterAssetDocumentTypeDiscoveryHooks(DiscoveryHooksId, Hooks);
}

void UnregisterAssetDocumentStateTreeDiscoveryHooks()
{
	UnregisterAssetDocumentTypeDiscoveryHooks(UE::AssetDocument::Graph::StateTree::DiscoveryHooksId);
}
