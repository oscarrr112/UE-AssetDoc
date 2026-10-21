// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentBlackboardFootprintProjection.h"

namespace BlackboardFootprintProjectionLocal
{
constexpr const TCHAR* BlackboardClassPath = TEXT("/Script/AIModule.BlackboardData");

FAssetDocumentContractValidationResult Reject(const TCHAR* Code, const TCHAR* Message)
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = false;
	FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/CanonicalValue");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Result;
}

FAssetDocumentContractValidationResult Accept()
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = true;
	return Result;
}

const FAssetDocumentRuntimeValueDto* FindField(
	const FAssetDocumentRuntimeValueDto& Object,
	FStringView Name)
{
	for (int32 Index = 0; Index < Object.ObjectKeys.Num(); ++Index)
	{
		if (Object.ObjectKeys[Index] == Name)
		{
			return &Object.Elements[Index];
		}
	}
	return nullptr;
}

bool HasExactObjectKeys(const FAssetDocumentRuntimeValueDto& Value, TConstArrayView<const TCHAR*> Expected)
{
	if (Value.Kind != EAssetDocumentRuntimeValueKind::Object
		|| Value.ObjectKeys.Num() != Expected.Num()
		|| Value.Elements.Num() != Expected.Num())
	{
		return false;
	}
	TSet<FString> Seen;
	for (const FString& Key : Value.ObjectKeys)
	{
		if (Seen.Contains(Key) || !Expected.ContainsByPredicate([&Key](const TCHAR* ExpectedKey)
			{
				return Key == ExpectedKey;
			}))
		{
			return false;
		}
		Seen.Add(Key);
	}
	return Seen.Num() == Expected.Num();
}

bool IsPrimitiveAllowed(FStringView PrimitiveId)
{
	static const TCHAR* const MutatingPrimitives[] = {
		TEXT("ai.blackboard.parent.set.v2"),
		TEXT("ai.blackboard.entry.update_existing.v2"),
		TEXT("ai.blackboard.entry.add.v2"),
		TEXT("ai.blackboard.entry.replace_class.v2"),
		TEXT("ai.blackboard.entry.remove.v2"),
		TEXT("ai.blackboard.local_keys.reorder.v2"),
	};
	for (const TCHAR* Candidate : MutatingPrimitives)
	{
		if (PrimitiveId == Candidate) return true;
	}
	return false;
}

bool IsExactRawRow(
	FStringView PrimitiveId,
	int32 RowIndex,
	int32 RowCount,
	const FString& Target,
	const FAssetDocumentMutationFootprint& Row)
{
	if (Row.Kind != EAssetDocumentMutationFootprintKind::UObjectState)
	{
		return false;
	}
	const FString AssetPrefix = FString::Printf(TEXT("asset:%s/"), *Target);
	const FString Keys = AssetPrefix + TEXT("Keys[*]");
	if (PrimitiveId == TEXT("ai.blackboard.parent.set.v2"))
	{
		return RowCount == 2 && ((RowIndex == 0 && Row.StableId == Keys
			&& Row.RollbackComponentId == TEXT("assetdocument.ai.blackboard.parent.derived_closure.v2"))
			|| (RowIndex == 1 && Row.StableId == AssetPrefix + TEXT("Parent")
			&& Row.RollbackComponentId == TEXT("assetdocument.ai.blackboard.parent.reference.v2")));
	}
	if (PrimitiveId == TEXT("ai.blackboard.entry.update_existing.v2"))
	{
		const FString EntryPrefix = AssetPrefix + TEXT("Keys[name=");
		const bool bMetadata = RowIndex == 0
			&& Row.StableId.StartsWith(EntryPrefix)
			&& Row.StableId.EndsWith(TEXT("].LocalKeysMetadataAggregate"))
			&& Row.RollbackComponentId == TEXT("assetdocument.ai.blackboard.entry_metadata.v2");
		const bool bKeyType = RowIndex == 1
			&& Row.StableId.StartsWith(EntryPrefix)
			&& Row.StableId.EndsWith(TEXT("].KeyTypeProperties"))
			&& Row.RollbackComponentId == TEXT("assetdocument.ai.blackboard.key_type_properties.v2");
		return RowCount == 2 && (bMetadata || bKeyType);
	}
	if (RowCount != 1 || RowIndex != 0 || Row.StableId != Keys)
	{
		return false;
	}
	if (PrimitiveId == TEXT("ai.blackboard.entry.add.v2")) return Row.RollbackComponentId == TEXT("assetdocument.ai.blackboard.entry.add.v2");
	if (PrimitiveId == TEXT("ai.blackboard.entry.replace_class.v2")) return Row.RollbackComponentId == TEXT("assetdocument.ai.blackboard.entry.replace_class.v2");
	if (PrimitiveId == TEXT("ai.blackboard.entry.remove.v2")) return Row.RollbackComponentId == TEXT("assetdocument.ai.blackboard.entry_remove.v2");
	return PrimitiveId == TEXT("ai.blackboard.local_keys.reorder.v2")
		&& Row.RollbackComponentId == TEXT("assetdocument.ai.blackboard.local_keys_reorder.v2");
}

FString MakeQualifiedId(
	FStringView Target,
	FStringView PrimitiveId,
	int32 StepIndex,
	int32 RowIndex,
	FStringView RawId,
	FStringView Label)
{
	return FString::Printf(TEXT("blackboard:%s|apply|step:%d|primitive:%s|row:%d|%s:%s"),
		*FString(Target), StepIndex, *FString(PrimitiveId), RowIndex, *FString(Label), *FString(RawId));
}

bool ParseGuidString(const FAssetDocumentRuntimeValueDto& Value, FGuid& OutGuid)
{
	return Value.Kind == EAssetDocumentRuntimeValueKind::String && FGuid::Parse(Value.StringValue, OutGuid);
}
} // namespace

FAssetDocumentContractValidationResult ParseAssetDocumentBlackboardFootprintProjection(
	const FAssetDocumentRuntimeRequestDto& Request,
	const FAssetDocumentRuntimeResultDto& Preflight,
	FAssetDocumentBlackboardFootprintProjection& OutProjection)
{
	using namespace BlackboardFootprintProjectionLocal;
	OutProjection = {};
	if (!Request.RequestId.IsValid() || Request.ExpectedRuntimeRevision == 0
		|| Request.RequestedAssetClassPath != BlackboardClassPath || Request.TargetAssetPath.IsEmpty()
		|| Request.Operation != EAssetDocumentRuntimeOperation::Apply)
	{
		return Reject(TEXT("BlackboardProjection.InvalidRequest"), TEXT("The native request is not an exact Blackboard Apply request."));
	}
	if (Preflight.RequestId != Request.RequestId
		|| Preflight.RuntimeRevision != Request.ExpectedRuntimeRevision
		|| Preflight.TerminalStatus != EAssetDocumentRuntimeTerminalStatus::Succeeded
		|| Preflight.FinalPhase != EAssetDocumentRuntimeRequestPhase::Complete
		|| Preflight.SemanticHash.IsEmpty() || !Preflight.CanonicalValue.IsSet())
	{
		return Reject(TEXT("BlackboardProjection.IdentityDrift"), TEXT("Preflight identity or canonical projection is incomplete."));
	}
	const FAssetDocumentRuntimeValueDto& Value = Preflight.CanonicalValue.GetValue();
	static const TCHAR* const ProjectionKeys[] = {
		TEXT("Kind"), TEXT("RequestId"), TEXT("RuntimeRevision"), TEXT("ExactAssetClassPath"),
		TEXT("TargetAssetPath"), TEXT("Operation"), TEXT("SemanticHash"), TEXT("Steps"),
	};
	if (!HasExactObjectKeys(Value, ProjectionKeys))
	{
		return Reject(TEXT("BlackboardProjection.ClosedSchema"), TEXT("The Blackboard projection must have exactly its closed root fields."));
	}
	const FAssetDocumentRuntimeValueDto* Kind = FindField(Value, TEXT("Kind"));
	const FAssetDocumentRuntimeValueDto* RequestId = FindField(Value, TEXT("RequestId"));
	const FAssetDocumentRuntimeValueDto* Revision = FindField(Value, TEXT("RuntimeRevision"));
	const FAssetDocumentRuntimeValueDto* ClassPath = FindField(Value, TEXT("ExactAssetClassPath"));
	const FAssetDocumentRuntimeValueDto* Target = FindField(Value, TEXT("TargetAssetPath"));
	const FAssetDocumentRuntimeValueDto* Operation = FindField(Value, TEXT("Operation"));
	const FAssetDocumentRuntimeValueDto* SemanticHash = FindField(Value, TEXT("SemanticHash"));
	const FAssetDocumentRuntimeValueDto* Steps = FindField(Value, TEXT("Steps"));
	FGuid ParsedRequestId;
	if (!Kind || !RequestId || !Revision || !ClassPath || !Target || !Operation || !SemanticHash || !Steps
		|| Kind->Kind != EAssetDocumentRuntimeValueKind::String || Kind->StringValue != TEXT("BlackboardFootprintProjectionV1")
		|| !ParseGuidString(*RequestId, ParsedRequestId) || ParsedRequestId != Request.RequestId
		|| Revision->Kind != EAssetDocumentRuntimeValueKind::Integer || Revision->IntegerValue <= 0 || static_cast<uint64>(Revision->IntegerValue) != Request.ExpectedRuntimeRevision
		|| ClassPath->Kind != EAssetDocumentRuntimeValueKind::String || ClassPath->StringValue != BlackboardClassPath
		|| Target->Kind != EAssetDocumentRuntimeValueKind::String || Target->StringValue != Request.TargetAssetPath
		|| Operation->Kind != EAssetDocumentRuntimeValueKind::String || Operation->StringValue != TEXT("Apply")
		|| SemanticHash->Kind != EAssetDocumentRuntimeValueKind::String || SemanticHash->StringValue != Preflight.SemanticHash
		// An empty step list is a valid no-op Body plan (e.g. creating an empty Blackboard
		// from its own template, or re-applying unchanged Keys/Parent).
		|| Steps->Kind != EAssetDocumentRuntimeValueKind::Array)
	{
		return Reject(TEXT("BlackboardProjection.IdentityOrTypeDrift"), TEXT("Projection identity, operation, or field type drifted from the request."));
	}
	TSet<FString> QualifiedStableIds;
	TSet<FString> QualifiedComponentIds;
	for (int32 StepArrayIndex = 0; StepArrayIndex < Steps->Elements.Num(); ++StepArrayIndex)
	{
		const FAssetDocumentRuntimeValueDto& StepValue = Steps->Elements[StepArrayIndex];
		static const TCHAR* const StepKeys[] = {TEXT("Index"), TEXT("PrimitiveId"), TEXT("RawFootprint")};
		if (!HasExactObjectKeys(StepValue, StepKeys)) return Reject(TEXT("BlackboardProjection.StepSchema"), TEXT("Each step must have exactly Index, PrimitiveId, and RawFootprint."));
		const FAssetDocumentRuntimeValueDto* Index = FindField(StepValue, TEXT("Index"));
		const FAssetDocumentRuntimeValueDto* PrimitiveId = FindField(StepValue, TEXT("PrimitiveId"));
		const FAssetDocumentRuntimeValueDto* RawFootprint = FindField(StepValue, TEXT("RawFootprint"));
		if (!Index || !PrimitiveId || !RawFootprint || Index->Kind != EAssetDocumentRuntimeValueKind::Integer
			|| Index->IntegerValue != StepArrayIndex || PrimitiveId->Kind != EAssetDocumentRuntimeValueKind::String
			|| !IsPrimitiveAllowed(PrimitiveId->StringValue) || RawFootprint->Kind != EAssetDocumentRuntimeValueKind::Array || RawFootprint->Elements.IsEmpty())
		{
			return Reject(TEXT("BlackboardProjection.StepOrderOrCatalog"), TEXT("Step order, primitive catalog, or raw footprint is invalid."));
		}
		FAssetDocumentBlackboardFootprintProjectionStep& Step = OutProjection.Steps.AddDefaulted_GetRef();
		Step.Index = StepArrayIndex;
		Step.PrimitiveId = PrimitiveId->StringValue;
		TSet<FString> RawRows;
		for (int32 RowIndex = 0; RowIndex < RawFootprint->Elements.Num(); ++RowIndex)
		{
			const FAssetDocumentRuntimeValueDto& RowValue = RawFootprint->Elements[RowIndex];
			static const TCHAR* const RowKeys[] = {TEXT("Kind"), TEXT("StableId"), TEXT("RollbackComponentId")};
			if (!HasExactObjectKeys(RowValue, RowKeys)) return Reject(TEXT("BlackboardProjection.RawSchema"), TEXT("Each raw footprint row must have exactly its closed fields."));
			const FAssetDocumentRuntimeValueDto* RowKind = FindField(RowValue, TEXT("Kind"));
			const FAssetDocumentRuntimeValueDto* StableId = FindField(RowValue, TEXT("StableId"));
			const FAssetDocumentRuntimeValueDto* ComponentId = FindField(RowValue, TEXT("RollbackComponentId"));
			if (!RowKind || !StableId || !ComponentId || RowKind->Kind != EAssetDocumentRuntimeValueKind::String
				|| StableId->Kind != EAssetDocumentRuntimeValueKind::String || ComponentId->Kind != EAssetDocumentRuntimeValueKind::String
				|| RowKind->StringValue != TEXT("UObjectState") || StableId->StringValue.IsEmpty() || ComponentId->StringValue.IsEmpty())
			{
				return Reject(TEXT("BlackboardProjection.RawType"), TEXT("Raw footprint rows require UObjectState and non-empty identities."));
			}
			FAssetDocumentMutationFootprint Raw;
			Raw.Kind = EAssetDocumentMutationFootprintKind::UObjectState;
			Raw.StableId = StableId->StringValue;
			Raw.RollbackComponentId = ComponentId->StringValue;
			if (!IsExactRawRow(Step.PrimitiveId, RowIndex, RawFootprint->Elements.Num(), Request.TargetAssetPath, Raw)
				|| RawRows.Contains(Raw.StableId + TEXT("\n") + Raw.RollbackComponentId))
			{
				return Reject(TEXT("BlackboardProjection.RawFootprintDrift"), TEXT("A raw footprint row does not match its exact primitive contract."));
			}
			RawRows.Add(Raw.StableId + TEXT("\n") + Raw.RollbackComponentId);
			FAssetDocumentMutationFootprint Qualified = Raw;
			Qualified.StableId = MakeQualifiedId(Request.TargetAssetPath, Step.PrimitiveId, Step.Index, RowIndex, Raw.StableId, TEXT("raw-stable"));
			Qualified.RollbackComponentId = MakeQualifiedId(Request.TargetAssetPath, Step.PrimitiveId, Step.Index, RowIndex, Raw.RollbackComponentId, TEXT("raw-component"));
			if (QualifiedStableIds.Contains(Qualified.StableId) || QualifiedComponentIds.Contains(Qualified.RollbackComponentId))
			{
				return Reject(TEXT("BlackboardProjection.QualifiedCollision"), TEXT("Qualified receipt identities must be unique."));
			}
			QualifiedStableIds.Add(Qualified.StableId);
			QualifiedComponentIds.Add(Qualified.RollbackComponentId);
			Step.RawFootprint.Add(MoveTemp(Raw));
			Step.QualifiedFootprint.Add(Qualified);
			OutProjection.QualifiedFootprint.Add(MoveTemp(Qualified));
		}
	}
	OutProjection.RequestId = Request.RequestId;
	OutProjection.RuntimeRevision = Request.ExpectedRuntimeRevision;
	OutProjection.ExactAssetClassPath = BlackboardClassPath;
	OutProjection.TargetAssetPath = Request.TargetAssetPath;
	OutProjection.Operation = TEXT("Apply");
	OutProjection.SemanticHash = Preflight.SemanticHash;
	return Accept();
}
