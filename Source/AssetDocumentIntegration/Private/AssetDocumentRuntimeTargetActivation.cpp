// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "AssetDocumentRuntimeTargetActivation.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "UObject/SoftObjectPath.h"

namespace
{
bool Exact(FStringView A, FStringView B) { return A.Equals(B, ESearchCase::CaseSensitive); }
void Fail(TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics, const TCHAR* Code)
{
	auto& D = Diagnostics.AddDefaulted_GetRef();
	D.Path = TEXT("/GenericRuntime"); D.Code = Code;
	D.Message = TEXT("The exact registered target activation or prepared decision was rejected.");
}

const FAssetDocumentRuntimeValueDto* Field(const FAssetDocumentRuntimeValueDto& Value, const TCHAR* Name)
{
	if (Value.Kind != EAssetDocumentRuntimeValueKind::Object
		|| Value.ObjectKeys.Num() != Value.Elements.Num()) return nullptr;
	TSet<FString> Keys;
	for (const FString& Key : Value.ObjectKeys)
	{
		if (Keys.Contains(Key)) return nullptr;
		Keys.Add(Key);
	}
	const int32 Index = Value.ObjectKeys.IndexOfByPredicate([Name](const FString& Key) { return Key.Equals(Name, ESearchCase::CaseSensitive); });
	return Value.Elements.IsValidIndex(Index) ? &Value.Elements[Index] : nullptr;
}

bool String(const FAssetDocumentRuntimeValueDto* Value, FString& Out)
{
	if (!Value || Value->Kind != EAssetDocumentRuntimeValueKind::String || Value->StringValue.IsEmpty()) return false;
	Out = Value->StringValue; return true;
}

bool SameRequirement(const FAssetDocumentPrimitiveRequirement& A, const FAssetDocumentPrimitiveRequirement& B)
{
	return Exact(A.PrimitiveId, B.PrimitiveId) && A.Version == B.Version && Exact(A.RequiredByRouteId, B.RequiredByRouteId);
}

bool SameRequirements(TConstArrayView<FAssetDocumentPrimitiveRequirement> A, TConstArrayView<FAssetDocumentPrimitiveRequirement> B)
{
	if (A.Num() != B.Num()) return false;
	for (int32 I = 0; I < A.Num(); ++I) if (!SameRequirement(A[I], B[I])) return false;
	return true;
}

bool ReadRequirements(const FAssetDocumentRuntimeValueDto* Value, TArray<FAssetDocumentPrimitiveRequirement>& Out)
{
	if (!Value || Value->Kind != EAssetDocumentRuntimeValueKind::Array || Value->Elements.IsEmpty()) return false;
	TSet<FString> Ids;
	for (const auto& Row : Value->Elements)
	{
		FAssetDocumentPrimitiveRequirement R;
		const auto* Version = Field(Row, TEXT("Version"));
		if (Row.ObjectKeys.Num() != 3 || !String(Field(Row, TEXT("PrimitiveId")), R.PrimitiveId)
			|| !String(Field(Row, TEXT("RequiredByRouteId")), R.RequiredByRouteId)
			|| !Version || Version->Kind != EAssetDocumentRuntimeValueKind::Integer
			|| Version->IntegerValue <= 0 || Version->IntegerValue > MAX_uint32
			|| Ids.Contains(R.PrimitiveId)) return false;
		R.Version = static_cast<uint32>(Version->IntegerValue);
		Ids.Add(R.PrimitiveId); Out.Add(MoveTemp(R));
	}
	return true;
}

bool ResolveExists(const FString& Path, const FString& Class, bool& bExists)
{
	const FSoftObjectPath ObjectPath(Path);
	if (!ObjectPath.IsValid() || !ObjectPath.IsAsset() || ObjectPath.ToString() != Path) return false;
	const FAssetData Data = IAssetRegistry::GetChecked().GetAssetByObjectPath(ObjectPath);
	bExists = Data.IsValid();
	if (!bExists) return true;
	UObject* Asset = Data.GetAsset();
	return Asset && Exact(Asset->GetPathName(), Path) && Exact(Asset->GetClass()->GetPathName(), Class);
}
}

TOptional<FAssetDocumentRuntimeTargetActivation> ParseAndValidateRuntimeTargetActivation(
	const FAssetDocumentRuntimeValueDto& Projection,
	TConstArrayView<FAssetDocumentEditorTargetLifecycleCatalogEntry> Catalog,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	FAssetDocumentRuntimeTargetActivation Result;
	const auto* Registration = Field(Projection, TEXT("RuntimeRegistration"));
	const auto* Sections = Field(Projection, TEXT("BodySections"));
	FString Policy;
	if (Projection.ObjectKeys.Num() != 3 || !String(Field(Projection, TEXT("Class")), Result.ExactAssetClassPath)
		|| !Sections || Sections->Kind != EAssetDocumentRuntimeValueKind::Array
		|| !Registration || (Registration->ObjectKeys.Num() != 2 && Registration->ObjectKeys.Num() != 3)
		|| !ReadRequirements(Field(*Registration, TEXT("RequiredPrimitives")), Result.RequiredPrimitives))
	{
		Fail(OutDiagnostics, TEXT("GenericRuntime.TargetActivationInvalid")); return {};
	}
 Result.bExistingOnly = Registration->ObjectKeys.Num() == 2 && !Field(*Registration, TEXT("LifecycleId"));
 if (!Result.bExistingOnly && !String(Field(*Registration, TEXT("LifecycleId")), Result.LifecycleId))
 { Fail(OutDiagnostics, TEXT("GenericRuntime.TargetActivationInvalid")); return {}; }
	if (!String(Field(*Registration, TEXT("TargetPolicy")), Policy)
		|| (!Exact(Policy, TEXTVIEW("CreateOrUpdate")) && !Exact(Policy, TEXTVIEW("UpdateOnly"))))
	{
		Fail(OutDiagnostics, TEXT("GenericRuntime.TargetPolicyInvalid")); return {};
	}
	Result.TargetPolicy = Exact(Policy, TEXTVIEW("CreateOrUpdate")) ? EAssetDocumentRuntimeTargetPolicy::CreateOrUpdate : EAssetDocumentRuntimeTargetPolicy::UpdateOnly;
 if (Result.bExistingOnly && Result.TargetPolicy != EAssetDocumentRuntimeTargetPolicy::UpdateOnly)
 { Fail(OutDiagnostics, TEXT("GenericRuntime.TargetPolicyInvalid")); return {}; }
	TSet<FString> SectionNames;
	for (const auto& Section : Sections->Elements)
	{
		FString Name;
		if (!String(&Section, Name) || SectionNames.Contains(Name))
		{ Fail(OutDiagnostics, TEXT("GenericRuntime.TargetActivationInvalid")); return {}; }
		SectionNames.Add(Name);
	}
 // This closed shape has no factory authority. The selected real projection
 // must still resolve every declared requirement before activation is usable.
 if (Result.bExistingOnly) return Result;
	// Class-specific row when one exists, else the class-neutral generic factory row.
	const FAssetDocumentEditorTargetLifecycleCatalogEntry* Match =
		FindAssetDocumentEditorTargetLifecycle(Catalog, Result.ExactAssetClassPath, Result.LifecycleId);
	if (!Match || Match->RequiredPrimitives.Num() != 4 || Match->FactoryId.IsEmpty()
		|| Match->ScratchCategoryId.IsEmpty()
		|| (Match->IsClassNeutral() != Match->CreateContextHandles.IsEmpty()))
	{ Fail(OutDiagnostics, TEXT("GenericRuntime.TargetActivationInvalid")); return {}; }
	// The supplied catalog must be the frozen Editor authority, including non-wire factory/context fields.
	const FAssetDocumentEditorTargetLifecycleCatalogEntry* Authority =
		FindAssetDocumentEditorTargetLifecycle(Result.ExactAssetClassPath, Result.LifecycleId);
	bool bExactCatalog = Authority && Exact(Authority->ExactAssetClassPath, Match->ExactAssetClassPath)
		&& Exact(Authority->LifecycleId, Match->LifecycleId)
		&& Exact(Authority->FactoryId, Match->FactoryId) && Exact(Authority->ScratchCategoryId, Match->ScratchCategoryId)
		&& Exact(Authority->CreateContextExactClassPath, Match->CreateContextExactClassPath)
		&& SameRequirements(Authority->RequiredPrimitives, Match->RequiredPrimitives)
		&& Authority->CreateContextHandles.Num() == Match->CreateContextHandles.Num();
	if (bExactCatalog)
	{
		for (int32 I = 0; I < Match->CreateContextHandles.Num(); ++I)
		{
			const auto& A = Authority->CreateContextHandles[I]; const auto& B = Match->CreateContextHandles[I];
			bExactCatalog &= Exact(A.Name, B.Name) && A.Kind == B.Kind && A.RequiredAccess == B.RequiredAccess && A.bRequired == B.bRequired;
		}
	}
	if (!bExactCatalog) { Fail(OutDiagnostics, TEXT("GenericRuntime.TargetActivationInvalid")); return {}; }
	// A class-neutral row is owned by the registering Profile's route, so only
	// id/version are fixed and all four rows must share that one route.
	FString OwnerRoute;
	if (Match->IsClassNeutral())
	{
		const FAssetDocumentPrimitiveRequirement* First = Result.RequiredPrimitives.FindByPredicate(
			[&](const auto& R) { return Exact(R.PrimitiveId, Match->RequiredPrimitives[0].PrimitiveId); });
		if (!First || First->RequiredByRouteId.IsEmpty())
		{ Fail(OutDiagnostics, TEXT("GenericRuntime.TargetActivationInvalid")); return {}; }
		OwnerRoute = First->RequiredByRouteId;
	}
	for (const auto& Required : Match->RequiredPrimitives)
	{
		FAssetDocumentPrimitiveRequirement Expected = Required;
		if (Match->IsClassNeutral()) Expected.RequiredByRouteId = OwnerRoute;
		if (!Result.RequiredPrimitives.ContainsByPredicate([&](const auto& R) { return SameRequirement(R, Expected); }))
		{ Fail(OutDiagnostics, TEXT("GenericRuntime.TargetActivationInvalid")); return {}; }
	}
	return Result;
}

bool AreAssetDocumentRuntimeTargetActivationsEqual(const FAssetDocumentRuntimeTargetActivation& A,
	const FAssetDocumentRuntimeTargetActivation& B)
{
	return Exact(A.ExactAssetClassPath, B.ExactAssetClassPath) && Exact(A.LifecycleId, B.LifecycleId)
		&& A.bExistingOnly == B.bExistingOnly && A.TargetPolicy == B.TargetPolicy && SameRequirements(A.RequiredPrimitives, B.RequiredPrimitives);
}

TOptional<bool> SelectAssetDocumentRuntimeTargetDecision(EAssetDocumentRuntimeTargetPolicy Policy,
	FStringView Action, bool bExists, TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	const TCHAR* Code = nullptr;
	if (Policy != EAssetDocumentRuntimeTargetPolicy::CreateOrUpdate && Policy != EAssetDocumentRuntimeTargetPolicy::UpdateOnly)
		Code = TEXT("GenericRuntime.TargetPolicyInvalid");
	else if (Policy == EAssetDocumentRuntimeTargetPolicy::UpdateOnly && !Exact(Action, TEXTVIEW("Update")))
		Code = TEXT("GenericRuntime.UpdateOnlyActionRequired");
	else if (Exact(Action, TEXTVIEW("Create")) && bExists) Code = TEXT("GenericRuntime.CreateTargetExists");
	else if (Exact(Action, TEXTVIEW("Update")) && !bExists) Code = TEXT("GenericRuntime.UpdateTargetMissing");
	else if (!Exact(Action, TEXTVIEW("Create")) && !Exact(Action, TEXTVIEW("Update")) && !Exact(Action, TEXTVIEW("CreateOrUpdate")))
		Code = TEXT("GenericRuntime.TargetActivationInvalid");
	if (Code) { Fail(OutDiagnostics, Code); return {}; }
	return !bExists;
}

TOptional<FAssetDocumentPreparedTargetDecision> PrepareAssetDocumentRuntimeTargetDecision(
	const FAssetDocumentRuntimeTargetActivation& Activation, const FAssetDocumentRuntimeRequestDto& Request,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	FAssetDocumentPreparedTargetDecision Result;
	Result.Activation = Activation; Result.RequestId = Request.RequestId;
	Result.RuntimeRevision = Request.ExpectedRuntimeRevision; Result.TargetPath = Request.TargetAssetPath;
	if (!Exact(Activation.ExactAssetClassPath, Request.RequestedAssetClassPath) || !Request.RequestId.IsValid()
		|| Request.ExpectedRuntimeRevision == 0 || !String(Field(Request.CanonicalDocument, TEXT("Action")), Result.Action))
	{ Fail(OutDiagnostics, TEXT("GenericRuntime.TargetActivationInvalid")); return {}; }
	// Reject policy/action before even resolving the target.
	if (Activation.TargetPolicy == EAssetDocumentRuntimeTargetPolicy::UpdateOnly && !Exact(Result.Action, TEXTVIEW("Update")))
	{ Fail(OutDiagnostics, TEXT("GenericRuntime.UpdateOnlyActionRequired")); return {}; }
	bool bExists = false;
	if (!ResolveExists(Result.TargetPath, Activation.ExactAssetClassPath, bExists))
	{ Fail(OutDiagnostics, TEXT("GenericRuntime.TargetActivationInvalid")); return {}; }
	const auto Decision = SelectAssetDocumentRuntimeTargetDecision(Activation.TargetPolicy, Result.Action, bExists, OutDiagnostics);
	if (!Decision.IsSet()) return {};
	Result.bCreate = Decision.GetValue(); return Result;
}

bool ValidateAssetDocumentPreparedTargetDecision(const FAssetDocumentPreparedTargetDecision& Prepared,
	const FAssetDocumentRuntimeRequestDto& Request, TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	TArray<FAssetDocumentRuntimeDiagnostic> FreshDiagnostics;
	const auto Fresh = PrepareAssetDocumentRuntimeTargetDecision(Prepared.Activation, Request, FreshDiagnostics);
	if (!Fresh.IsSet() || Fresh->RequestId != Prepared.RequestId || Fresh->RuntimeRevision != Prepared.RuntimeRevision
		|| !Exact(Fresh->TargetPath, Prepared.TargetPath) || !Exact(Fresh->Action, Prepared.Action) || Fresh->bCreate != Prepared.bCreate)
	{ Fail(OutDiagnostics, TEXT("GenericRuntime.TargetActivationDrift")); return false; }
	return true;
}

FAssetDocumentPreparedTargetContext::FAssetDocumentPreparedTargetContext(IAssetDocumentNativeRequestContext& InInner,
	const FAssetDocumentPreparedTargetDecision& InPrepared, const FAssetDocumentRuntimeRequestDto& InRequest)
	: Inner(InInner), Prepared(InPrepared), Request(InRequest) {}
const FAssetDocumentRequestContextMetadata& FAssetDocumentPreparedTargetContext::GetMetadata() const { return Inner.GetMetadata(); }
FAssetDocumentPrimitiveRegistrySnapshot FAssetDocumentPreparedTargetContext::InspectCatalog() const { return Inner.InspectCatalog(); }
FAssetDocumentIssuedHandleResult FAssetDocumentPreparedTargetContext::Handoff(const FAssetDocumentOpaqueHandle& Source,
	FStringView Provider, EAssetDocumentHandleAccess Access) { return Inner.Handoff(Source, Provider, Access); }
FAssetDocumentPrimitiveOutcome FAssetDocumentPreparedTargetContext::Invoke(const FAssetDocumentPrimitiveInvocation& Invocation)
{
	if (!bMutationStarted)
	{
		FAssetDocumentPrimitiveOutcome Rejected;
		if (GetMetadata().RequestId != Prepared.RequestId || GetMetadata().PinnedRuntimeRevision != Prepared.RuntimeRevision
			|| GetMetadata().Phase != EAssetDocumentRuntimeRequestPhase::Apply
			|| !ValidateAssetDocumentPreparedTargetDecision(Prepared, Request, Rejected.Diagnostics))
		{
			if (Rejected.Diagnostics.IsEmpty()) Fail(Rejected.Diagnostics, TEXT("GenericRuntime.TargetActivationDrift"));
			return Rejected;
		}
		for (const auto& D : Inner.InspectCatalog().Descriptors)
		{
			if (D.PrimitiveId == Invocation.PrimitiveId && D.Version == Invocation.Version
				&& D.Access == EAssetDocumentPrimitiveAccess::Mutating)
			{ bMutationStarted = true; break; }
		}
	}
	return Inner.Invoke(Invocation);
}
