// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/AssetDocumentAIPrimitiveCatalog.h"
#include "Provider/AssetDocumentAIPrimitiveHandler.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreePrimitives.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeBindingInspect.h"
#include "Provider/Blackboard/AssetDocumentAIBlackboardPrimitives.h"
#include "Provider/Selector/AssetDocumentAISelectorEvidence.h"

namespace AIProviderAssemblyLocal
{
FAssetDocumentPrimitiveOutcome HandleSelectorEvidence(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver&,
	IAssetDocumentAtomicTransaction*)
{
	return BuildAssetDocumentAISelectorEvidence(Invocation);
}

const FAssetDocumentAIPrimitiveBinding Bindings[] = {
	{TEXT("ai.behavior_tree.binding.inspect.v1"), 1, &HandleAssetDocumentAIBehaviorTreeBindingInspect},
	{TEXT("ai.behavior_tree.selector.inventory.v1"), 1, &HandleAssetDocumentAIBehaviorTreeSelectorInventory},
	{TEXT("ai.behavior_tree.graph.author_scratch.v1"), 1, &HandleAssetDocumentAIBehaviorTreeAuthorScratch},
	{TEXT("ai.behavior_tree.node_class.inspect.v2"), 2, &HandleAssetDocumentAIBehaviorTreeNodeClassInspectV2},
	{TEXT("ai.behavior_tree.blackboard.readback.v1"), 1, &HandleAssetDocumentAIBehaviorTreeBlackboardReadback},
	{TEXT("ai.behavior_tree.graph.install.v1"), 1, &HandleAssetDocumentAIBehaviorTreeGraphInstall},
	{TEXT("ai.blackboard.key_type.inspect.v1"), 1,
		&HandleAssetDocumentAIBlackboardKeyTypeInspect},
	{TEXT("ai.blackboard.entry.readback.v2"), 2,
		&HandleAssetDocumentAIBlackboardEntryReadback},
	{TEXT("ai.blackboard.parent.readback.v2"), 2,
		&HandleAssetDocumentAIBlackboardParentReadback},
	{TEXT("ai.blackboard.local_keys.readback.v2"), 2,
		&HandleAssetDocumentAIBlackboardLocalKeysReadback},
	{TEXT("ai.blackboard.entry.update_existing.v2"), 2,
		&HandleAssetDocumentAIBlackboardEntryUpdateExisting},
	{TEXT("ai.blackboard.parent.set.v2"), 2,
		&HandleAssetDocumentAIBlackboardParentSetV2},
	{TEXT("ai.blackboard.entry.add.v2"), 2,
		&HandleAssetDocumentAIBlackboardEntryAdd},
	{TEXT("ai.blackboard.entry.replace_class.v2"), 2,
		&HandleAssetDocumentAIBlackboardEntryReplaceClass},
	{TEXT("ai.blackboard.entry.remove.v2"), 2,
		&HandleAssetDocumentAIBlackboardEntryRemove},
	{TEXT("ai.blackboard.local_keys.reorder.v2"), 2,
		&HandleAssetDocumentAIBlackboardLocalKeysReorder},
	{TEXT("ai.blackboard.key_type.apply_properties.v1"), 1,
		&HandleAssetDocumentAIBlackboardKeyTypeApplyProperties},
	{TEXT("ai.blackboard.parent.set.v1"), 1,
		&HandleAssetDocumentAIBlackboardParentSet},
	{TEXT("ai.blackboard.local_keys.replace.v1"), 1,
		&HandleAssetDocumentAIBlackboardLocalKeysReplace},
	{TEXT("ai.behavior_tree.node_class.inspect.v1"), 1,
		&HandleAssetDocumentAIBehaviorTreeNodeClassInspect},
	{TEXT("ai.behavior_tree.instance.validate_properties.v1"), 1,
		&HandleAssetDocumentAIBehaviorTreeInstanceValidateProperties},
	{TEXT("ai.behavior_tree.authored_graph.readback.v1"), 1,
		&HandleAssetDocumentAIBehaviorTreeAuthoredGraphReadback},
	{TEXT("ai.behavior_tree.authored_graph.readback.v2"), 2,
		&HandleAssetDocumentAIBehaviorTreeAuthoredGraphReadback},
	{TEXT("ai.behavior_tree.decorator.ensure_input_count.v1"), 1,
		&HandleAssetDocumentAIBehaviorTreeDecoratorEnsureInputCount},
	{TEXT("ai.behavior_tree.graph.create.v1"), 1,
		&HandleAssetDocumentAIBehaviorTreeGraphCreate},
	{TEXT("ai.behavior_tree.graph.create_defaults.v1"), 1,
		&HandleAssetDocumentAIBehaviorTreeGraphCreateDefaults},
	{TEXT("ai.behavior_tree.node.create_action.v1"), 1,
		&HandleAssetDocumentAIBehaviorTreeNodeCreateAction},
	{TEXT("ai.behavior_tree.subnode.create_action.v1"), 1,
		&HandleAssetDocumentAIBehaviorTreeSubnodeCreateAction},
	{TEXT("ai.behavior_tree.comment.create_action.v1"), 1,
		&HandleAssetDocumentAIBehaviorTreeCommentCreateAction},
	{TEXT("ai.behavior_tree.graph.connect.v1"), 1,
		&HandleAssetDocumentAIBehaviorTreeGraphConnect},
	{TEXT("ai.behavior_tree.selector.resolve_batch.v1"), 1,
		&HandleAssetDocumentAIBehaviorTreeSelectorBatchResolve},
	{TEXT("ai.behavior_tree.selector.resolve_batch.v2"), 2,
		&HandleAssetDocumentAIBehaviorTreeSelectorBatchResolve},
	{TEXT("ai.behavior_tree.selector.resolve.v1"), 1,
		&HandleAssetDocumentAIBehaviorTreeSelectorResolve},
	{TEXT("ai.behavior_tree.value_or_key.refresh.v1"), 1,
		&HandleAssetDocumentAIBehaviorTreeValueOrKeyRefresh},
	{TEXT("ai.behavior_tree.value_or_key.refresh.v2"), 2,
		&HandleAssetDocumentAIBehaviorTreeValueOrKeyRefresh},
	{TEXT("ai.behavior_tree.value_or_key.refresh.v3"), 3,
		&HandleAssetDocumentAIBehaviorTreeValueOrKeyRefresh},
	{TEXT("ai.behavior_tree.decorator.refresh.v1"), 1,
		&HandleAssetDocumentAIBehaviorTreeDecoratorRefresh},
	{TEXT("ai.behavior_tree.runtime_mirror.rebuild.v1"), 1,
		&HandleAssetDocumentAIBehaviorTreeRuntimeMirrorRebuild},
	{TEXT("ai.behavior_tree.blackboard.set.v1"), 1,
		&HandleAssetDocumentAIBehaviorTreeBlackboardSet},
	{TEXT("ai.behavior_tree.selector_inspect_evidence.v1"), 1, &HandleSelectorEvidence},
};

int32 CountMatchingBindings(
	TConstArrayView<FAssetDocumentAIPrimitiveBinding> CandidateBindings,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	int32 MatchCount = 0;
	for (const FAssetDocumentAIPrimitiveBinding& Binding : CandidateBindings)
	{
		MatchCount += Binding.PrimitiveId == Descriptor.PrimitiveId
			&& Binding.Version == Descriptor.Version
			&& Binding.Handler != nullptr;
	}
	return MatchCount;
}

int32 CountMatchingDescriptors(
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors,
	const FAssetDocumentAIPrimitiveBinding& Binding)
{
	int32 MatchCount = 0;
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Descriptors)
	{
		MatchCount += Descriptor.PrimitiveId == Binding.PrimitiveId
			&& Descriptor.Version == Binding.Version
			&& Descriptor.Admission.Disposition
				!= EAssetDocumentPrimitiveDisposition::StableBlocker;
	}
	return MatchCount;
}

bool IsDecisionDescriptorVersionAdmitted(
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	if (Descriptor.Version == 1)
	{
		return true;
	}
	if (Descriptor.Version == 3)
	{
		return Descriptor.PrimitiveId == TEXT("ai.behavior_tree.value_or_key.refresh.v3");
	}
	if (Descriptor.Version != 2)
	{
		return false;
	}
	static const TCHAR* const V2DecisionIds[] = {
		TEXT("ai.behavior_tree.node_class.inspect.v2"),
		TEXT("ai.behavior_tree.selector.resolve_batch.v2"),
		TEXT("ai.behavior_tree.authored_graph.readback.v2"),
		TEXT("ai.behavior_tree.value_or_key.refresh.v2"),
		TEXT("ai.blackboard.entry.readback.v2"),
		TEXT("ai.blackboard.parent.readback.v2"),
		TEXT("ai.blackboard.local_keys.readback.v2"),
		TEXT("ai.blackboard.entry.update_existing.v2"),
		TEXT("ai.blackboard.parent.set.v2"),
		TEXT("ai.blackboard.entry.add.v2"),
		TEXT("ai.blackboard.entry.replace_class.v2"),
		TEXT("ai.blackboard.entry.remove.v2"),
		TEXT("ai.blackboard.local_keys.reorder.v2")};
	for (const TCHAR* DecisionId : V2DecisionIds)
	{
		if (Descriptor.PrimitiveId == DecisionId)
		{
			return true;
		}
	}
	return false;
}

bool FindUniqueDecisionDescriptor(
	const FAssetDocumentAIDecisionProjectionRow& Row,
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors,
	const FAssetDocumentPrimitiveDescriptor*& OutDescriptor)
{
	OutDescriptor = nullptr;
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Descriptors)
	{
		if (Descriptor.PrimitiveId != Row.DecisionId
			|| !IsDecisionDescriptorVersionAdmitted(Descriptor))
		{
			continue;
		}
		if (OutDescriptor != nullptr)
		{
			return false;
		}
		OutDescriptor = &Descriptor;
	}
	return OutDescriptor != nullptr;
}

bool IsStableBlockerProjectionValid(
	const FAssetDocumentAIDecisionProjectionRow& Row,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	return Row.DecisionConsumerId == TEXT("ai_provider_design")
		&& !Row.StableDiagnosticCode.IsEmpty()
		&& Descriptor.RequiredConsumerIds.IsEmpty()
		&& Descriptor.StableDiagnosticCodes
			== TArray<FString>{Row.StableDiagnosticCode};
}

bool IsCallableProjectionValid(
	const FAssetDocumentAIDecisionProjectionRow& Row,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	return (Row.Disposition == EAssetDocumentPrimitiveDisposition::OrdinaryHelper
			|| Row.Disposition == EAssetDocumentPrimitiveDisposition::AdmittedBridge)
		&& !Row.DecisionConsumerId.IsEmpty()
		&& Row.StableDiagnosticCode.IsEmpty()
		&& Descriptor.RequiredConsumerIds
			== TArray<FString>{Row.DecisionConsumerId};
}

bool TryAddDecisionId(
	const FAssetDocumentAIDecisionProjectionRow& Row,
	TSet<FString>& SeenDecisionIds)
{
	if (Row.DecisionId.IsEmpty() || SeenDecisionIds.Contains(Row.DecisionId)
		|| Row.DecisionId == TEXT("ai.behavior_tree.selector_inspect_evidence.v1"))
	{
		return false;
	}
	SeenDecisionIds.Add(Row.DecisionId);
	return true;
}

bool ValidateDecisionProjectionRow(
	const FAssetDocumentAIDecisionProjectionRow& Row,
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors,
	TSet<FString>& SeenDecisionIds)
{
	if (!TryAddDecisionId(Row, SeenDecisionIds))
	{
		return false;
	}
	const FAssetDocumentPrimitiveDescriptor* MatchedDescriptor = nullptr;
	if (!FindUniqueDecisionDescriptor(Row, Descriptors, MatchedDescriptor))
	{
		return false;
	}
	if (MatchedDescriptor->Admission.Disposition != Row.Disposition)
	{
		return false;
	}
	if (Row.Disposition == EAssetDocumentPrimitiveDisposition::StableBlocker)
	{
		return IsStableBlockerProjectionValid(Row, *MatchedDescriptor);
	}
	return IsCallableProjectionValid(Row, *MatchedDescriptor);
}

bool HasExactlyOneDecisionProjection(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	TConstArrayView<FAssetDocumentAIDecisionProjectionRow> CandidateRows)
{
	int32 MatchingDecisionCount = 0;
	for (const FAssetDocumentAIDecisionProjectionRow& Row : CandidateRows)
	{
		MatchingDecisionCount += Row.DecisionId == Descriptor.PrimitiveId;
	}
	return MatchingDecisionCount == 1;
}

bool ValidateDecisionProjectionCoverage(
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors,
	TConstArrayView<FAssetDocumentAIDecisionProjectionRow> CandidateRows)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Descriptors)
	{
		if (Descriptor.PrimitiveId == TEXT("ai.behavior_tree.selector_inspect_evidence.v1"))
		{
			continue;
		}
		if (!HasExactlyOneDecisionProjection(Descriptor, CandidateRows))
		{
			return false;
		}
	}
	return true;
}
}

const FAssetDocumentAIPrimitiveBinding* FindAssetDocumentAIPrimitiveBinding(
	FStringView PrimitiveId,
	uint32 Version)
{
	using namespace AIProviderAssemblyLocal;
	for (const FAssetDocumentAIPrimitiveBinding& Binding : Bindings)
	{
		if (Binding.PrimitiveId == PrimitiveId && Binding.Version == Version)
		{
			return &Binding;
		}
	}
	return nullptr;
}

bool ValidateAssetDocumentAIDecisionProjection(
	TConstArrayView<FAssetDocumentAIDecisionProjectionRow> CandidateRows)
{
	using namespace AIProviderAssemblyLocal;
	const TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors =
		GetAssetDocumentAIPrimitiveDescriptors();
	if (CandidateRows.Num() != 40 || Descriptors.Num() != CandidateRows.Num() + 1)
	{
		return false;
	}

	TSet<FString> SeenDecisionIds;
	for (const FAssetDocumentAIDecisionProjectionRow& Row : CandidateRows)
	{
		if (!ValidateDecisionProjectionRow(Row, Descriptors, SeenDecisionIds))
		{
			return false;
		}
	}
	return ValidateDecisionProjectionCoverage(Descriptors, CandidateRows);
}

bool IsAssetDocumentAIPrimitiveAssemblyValid()
{
	using namespace AIProviderAssemblyLocal;
	const TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors =
		GetAssetDocumentAIPrimitiveDescriptors();
	const TConstArrayView<FAssetDocumentAIPrimitiveBinding> CandidateBindings = Bindings;
	if (!ValidateAssetDocumentAIDecisionProjection(
		GetAssetDocumentAIDecisionProjectionRows()))
	{
		return false;
	}

	for (int32 Index = 0; Index < Descriptors.Num(); ++Index)
	{
		const FAssetDocumentPrimitiveDescriptor& Descriptor = Descriptors[Index];
		if (!ValidateAssetDocumentPrimitiveDescriptor(
			Descriptor, Descriptors.Left(Index)).bValid)
		{
			return false;
		}

		const int32 MatchCount = CountMatchingBindings(CandidateBindings, Descriptor);
		if (Descriptor.Admission.Disposition
			== EAssetDocumentPrimitiveDisposition::StableBlocker)
		{
			if (MatchCount != 0 || !Descriptor.RequiredConsumerIds.IsEmpty())
			{
				return false;
			}
		}
		else if (MatchCount != 1)
		{
			return false;
		}
	}

	for (const FAssetDocumentAIPrimitiveBinding& Binding : CandidateBindings)
	{
		if (Binding.Handler == nullptr
			|| CountMatchingDescriptors(Descriptors, Binding) != 1)
		{
			return false;
		}
	}
	return true;
}

bool EnsureAssetDocumentAIProviderReady()
{
	return IsAssetDocumentAIPrimitiveAssemblyValid()
		&& EnsureAssetDocumentAIBehaviorTreeEditorAuthority();
}
