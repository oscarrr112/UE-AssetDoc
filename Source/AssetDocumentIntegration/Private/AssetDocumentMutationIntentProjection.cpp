// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "AssetDocumentMutationIntentProjection.h"

namespace MutationIntentProjectionLocal
{
using FValue = FAssetDocumentRuntimeValueDto;
using EKind = EAssetDocumentRuntimeValueKind;
FAssetDocumentContractValidationResult Reject(const TCHAR* Code, const TCHAR* Message)
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = false;
	auto& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/CanonicalValue");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Result;
}
bool ExactKeys(const FValue& Value, TConstArrayView<const TCHAR*> Keys)
{
	if (Value.Kind != EKind::Object || Value.ObjectKeys.Num() != Keys.Num()) return false;
	for (const TCHAR* Key : Keys) if (!Value.ObjectKeys.Contains(Key)) return false;
	return true;
}
const FValue& Field(const FValue& Value, const TCHAR* Key)
{
	return Value.Elements[Value.ObjectKeys.IndexOfByKey(Key)];
}
bool StringEquals(const FValue& Value, FStringView Expected)
{
	return Value.Kind == EKind::String && Value.StringValue == Expected;
}
bool IsValueIntent(const FValue& Value)
{
	// This rejects transport/state envelopes, not authored-domain semantics.
	static const TCHAR* const Forbidden[] = {TEXT("Body"), TEXT("Handle"), TEXT("Handles"),
		TEXT("HandleArguments"), TEXT("OpaqueHandle"), TEXT("Address"), TEXT("Pointer"),
		TEXT("Rollback"), TEXT("RollbackCode"), TEXT("TransactionId"), TEXT("Slot"), TEXT("Generation")};
	if (Value.Kind == EKind::Bytes) return false;
	for (const FString& Key : Value.ObjectKeys)
	{
		for (const TCHAR* Reserved : Forbidden) if (Key == Reserved) return false;
	}
	for (const FValue& Child : Value.Elements) if (!IsValueIntent(Child)) return false;
	return true;
}
}

FAssetDocumentContractValidationResult ParseAssetDocumentMutationIntentProjection(
	const FAssetDocumentRuntimeRequestDto& Request,
	const FAssetDocumentRuntimeResultDto& Preflight,
	FStringView ValidatedSemanticHash,
	const IAssetDocumentPrimitiveRegistry& Registry,
	TConstArrayView<FAssetDocumentPrimitiveRequirement> Requirements,
	FAssetDocumentMutationIntentProjection& OutProjection)
{
	using namespace MutationIntentProjectionLocal;
	OutProjection = {};
	if (!Request.RequestId.IsValid() || Request.ExpectedRuntimeRevision == 0
		|| Request.RequestedAssetClassPath.IsEmpty() || Request.TargetAssetPath.IsEmpty()
		|| Request.Operation != EAssetDocumentRuntimeOperation::Apply || ValidatedSemanticHash.IsEmpty()
		|| Preflight.RequestId != Request.RequestId || Preflight.RuntimeRevision != Request.ExpectedRuntimeRevision
		|| Preflight.TerminalStatus != EAssetDocumentRuntimeTerminalStatus::Succeeded
		|| Preflight.FinalPhase != EAssetDocumentRuntimeRequestPhase::Complete
		|| Preflight.SemanticHash != ValidatedSemanticHash || !Preflight.CanonicalValue.IsSet())
		return Reject(TEXT("MutationIntentProjection.IdentityDrift"), TEXT("Exact Apply request, successful preflight and validated semantic hash must agree."));
	const FValue& Root = Preflight.CanonicalValue.GetValue();
	const auto ValueValidation = ValidateAssetDocumentRuntimeValue(Root);
	if (!ValueValidation.bValid) return ValueValidation;
	static const TCHAR* const RootKeys[] = {TEXT("Kind"), TEXT("RequestId"), TEXT("RuntimeRevision"),
		TEXT("ExactAssetClassPath"), TEXT("TargetAssetPath"), TEXT("Operation"), TEXT("SemanticHash"), TEXT("Steps")};
	if (!ExactKeys(Root, RootKeys))
		return Reject(TEXT("MutationIntentProjection.ClosedSchema"), TEXT("Projection root fields must exactly match MutationIntentProjectionV1."));
	FGuid ParsedId;
	const FValue& Id = Field(Root, TEXT("RequestId"));
	const FValue& Revision = Field(Root, TEXT("RuntimeRevision"));
	const FValue& Steps = Field(Root, TEXT("Steps"));
	const bool bSnapshotCovered = StringEquals(Field(Root, TEXT("Kind")), TEXT("SnapshotCoveredMutationV1"));
	if ((!StringEquals(Field(Root, TEXT("Kind")), TEXT("MutationIntentProjectionV1")) && !bSnapshotCovered)
		|| (bSnapshotCovered && !Steps.Elements.IsEmpty())
		|| Id.Kind != EKind::String || !FGuid::Parse(Id.StringValue, ParsedId) || ParsedId != Request.RequestId
		|| Revision.Kind != EKind::Integer || Revision.IntegerValue <= 0
		|| static_cast<uint64>(Revision.IntegerValue) != Request.ExpectedRuntimeRevision
		|| !StringEquals(Field(Root, TEXT("ExactAssetClassPath")), Request.RequestedAssetClassPath)
		|| !StringEquals(Field(Root, TEXT("TargetAssetPath")), Request.TargetAssetPath)
		|| !StringEquals(Field(Root, TEXT("Operation")), TEXT("Apply"))
		|| !StringEquals(Field(Root, TEXT("SemanticHash")), ValidatedSemanticHash) || Steps.Kind != EKind::Array)
		return Reject(TEXT("MutationIntentProjection.IdentityOrTypeDrift"), TEXT("Projection identity or field types differ from the exact request."));
	const auto Snapshot = Registry.Snapshot();
	if (!Snapshot.bFrozen || Snapshot.RuntimeRevision != Request.ExpectedRuntimeRevision)
		return Reject(TEXT("MutationIntentProjection.RegistryDrift"), TEXT("Preparation requires the exact frozen runtime registry."));
	FAssetDocumentMutationIntentProjection Parsed;
	Parsed.RequestId = Request.RequestId;
	Parsed.RuntimeRevision = Request.ExpectedRuntimeRevision;
	Parsed.ExactAssetClassPath = Request.RequestedAssetClassPath;
	Parsed.TargetAssetPath = Request.TargetAssetPath;
	Parsed.SemanticHash = Preflight.SemanticHash;
	Parsed.bSnapshotCovered = bSnapshotCovered;
	for (int32 StepIndex = 0; StepIndex < Steps.Elements.Num(); ++StepIndex)
	{
		const FValue& Value = Steps.Elements[StepIndex];
		static const TCHAR* const Keys[] = {TEXT("Index"), TEXT("PrimitiveId"), TEXT("Version"), TEXT("Phase"), TEXT("InputSchemaId"), TEXT("Intent")};
		if (!ExactKeys(Value, Keys)) return Reject(TEXT("MutationIntentProjection.StepSchema"), TEXT("Each step must have exactly the closed intent fields."));
		const FValue& Index = Field(Value, TEXT("Index"));
		const FValue& Primitive = Field(Value, TEXT("PrimitiveId"));
		const FValue& Version = Field(Value, TEXT("Version"));
		const FValue& Phase = Field(Value, TEXT("Phase"));
		const FValue& Schema = Field(Value, TEXT("InputSchemaId"));
		const FValue& Intent = Field(Value, TEXT("Intent"));
		if (Index.Kind != EKind::Integer || Index.IntegerValue != StepIndex
			|| Primitive.Kind != EKind::String || Primitive.StringValue.IsEmpty()
			|| Version.Kind != EKind::Integer || Version.IntegerValue <= 0 || Version.IntegerValue > MAX_uint32
			|| Schema.Kind != EKind::String || Schema.StringValue.IsEmpty()
			|| (!StringEquals(Phase, TEXT("Apply")) && !StringEquals(Phase, TEXT("Repair")))
			|| Intent.Kind != EKind::Object || !IsValueIntent(Intent))
			return Reject(TEXT("MutationIntentProjection.StepIntent"), TEXT("Step index, version, phase, schema or pure value intent is invalid."));
		const FAssetDocumentPrimitiveRequirement* Required = nullptr;
		for (const auto& Requirement : Requirements)
		{
			if (Requirement.PrimitiveId != Primitive.StringValue || Requirement.Version != Version.IntegerValue) continue;
			if (Required != nullptr || Requirement.RequiredByRouteId.IsEmpty())
				return Reject(TEXT("MutationIntentProjection.AmbiguousRequirement"), TEXT("Each step must select exactly one explicit frozen consumer requirement."));
			Required = &Requirement;
		}
		if (!Required) return Reject(TEXT("MutationIntentProjection.MissingRequirement"), TEXT("The step is absent from explicit frozen requirements."));
		const auto Resolution = Registry.Resolve(*Required);
		const auto& Descriptor = Resolution.Descriptor;
		const auto NativePhase = StringEquals(Phase, TEXT("Apply")) ? EAssetDocumentRuntimeRequestPhase::Apply : EAssetDocumentRuntimeRequestPhase::Repair;
		if (!Resolution.bResolved || Resolution.ProviderId.IsEmpty() || Resolution.ProviderId != Descriptor.ProviderId
			|| Descriptor.PrimitiveId != Required->PrimitiveId || Descriptor.Version != Required->Version
			|| Descriptor.Access != EAssetDocumentPrimitiveAccess::Mutating || !Descriptor.bRequiresActiveTransaction
			|| !Descriptor.AllowedPhases.Contains(NativePhase) || Descriptor.InputSchemaId != Schema.StringValue
			|| !Descriptor.RequiredConsumerIds.Contains(Required->RequiredByRouteId)
			|| Descriptor.Admission.Disposition == EAssetDocumentPrimitiveDisposition::StableBlocker
			|| Descriptor.Admission.Disposition == EAssetDocumentPrimitiveDisposition::GovernanceCandidate)
			return Reject(TEXT("MutationIntentProjection.DescriptorDrift"), TEXT("Resolved mutating provider, consumer, schema and phase must match the exact intent."));
		int32 MatchingDescriptors = 0;
		for (const auto& Frozen : Snapshot.Descriptors)
		{
			if (Frozen.PrimitiveId != Required->PrimitiveId || Frozen.Version != Required->Version) continue;
			++MatchingDescriptors;
			if (Frozen.ProviderId != Descriptor.ProviderId || Frozen.Owner != Descriptor.Owner
				|| Frozen.Access != Descriptor.Access || Frozen.InputSchemaId != Descriptor.InputSchemaId
				|| Frozen.Admission.Disposition != Descriptor.Admission.Disposition
				|| !Frozen.bRequiresActiveTransaction || !Frozen.AllowedPhases.Contains(NativePhase)
				|| !Frozen.RequiredConsumerIds.Contains(Required->RequiredByRouteId))
				return Reject(TEXT("MutationIntentProjection.RegistryDrift"), TEXT("Resolved descriptor differs from the frozen provider contract."));
		}
		int32 AvailableProviders = 0;
		for (const auto& Provider : Snapshot.Providers)
		{
			if (Provider.ProviderId != Resolution.ProviderId) continue;
			if (Provider.State != EAssetDocumentProviderAvailabilityState::Available)
				return Reject(TEXT("MutationIntentProjection.ProviderUnavailable"), TEXT("The selected preparation provider is unavailable."));
			++AvailableProviders;
		}
		if (MatchingDescriptors != 1 || AvailableProviders != 1)
			return Reject(TEXT("MutationIntentProjection.RegistryDrift"), TEXT("The frozen snapshot must contain one exact descriptor and available provider."));
		auto& Step = Parsed.Steps.AddDefaulted_GetRef();
		Step.StepIndex = StepIndex;
		Step.Requirement = *Required;
		Step.Phase = NativePhase;
		Step.InputSchemaId = Schema.StringValue;
		Step.Intent = Intent;
	}
	OutProjection = MoveTemp(Parsed);
	FAssetDocumentContractValidationResult Result;
	Result.bValid = true;
	return Result;
}
