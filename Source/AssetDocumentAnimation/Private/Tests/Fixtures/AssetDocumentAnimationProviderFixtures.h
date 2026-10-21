// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"

enum class EAssetDocumentAnimationBootstrapDependency : uint8
{
	None = 0,
	Asset = 1,
	AssetAndProviderDerived = 2,
	AssetPair = 3,
	AssetAndClass = 4,
	GraphOrNode = 5,
};

enum class EAssetDocumentAnimationBootstrapDisposition : uint8
{
	FrozenG1NotApplicable = 0,
	VerifiedAssetIdentityReady = 1,
	RuntimeHandleBootstrapBlocked = 2,
};

enum class EAssetDocumentAnimationFinalDisposition : uint8
{
	Admitted = 0,
	StableBlocker = 1,
};

struct FAssetDocumentAnimationT0DecisionRecord
{
	const TCHAR* DecisionId = TEXT("");
	const TCHAR* AuthorityGateId = TEXT("");
	const TCHAR* RowDiagnostic = TEXT("");
	bool bFrozenG1Blocker = false;
	bool bMutating = false;
	uint8 RequiredInputHandleCount = 0;
	EAssetDocumentAnimationBootstrapDependency BootstrapDependency =
		EAssetDocumentAnimationBootstrapDependency::None;
	EAssetDocumentAnimationBootstrapDisposition BootstrapDisposition =
		EAssetDocumentAnimationBootstrapDisposition::FrozenG1NotApplicable;
	EAssetDocumentAnimationFinalDisposition FinalDisposition =
		EAssetDocumentAnimationFinalDisposition::StableBlocker;
	const TCHAR* FinalDiagnostic = TEXT("");
};

TConstArrayView<FAssetDocumentAnimationT0DecisionRecord>
GetAssetDocumentAnimationT0DecisionRecords();

struct FAssetDocumentAnimationAuthorityGateEvidence
{
	const TCHAR* GateId = TEXT("");
	const TCHAR* PublicModule = TEXT("");
	const TCHAR* PublicHeader = TEXT("");
	const TCHAR* ExportMacro = TEXT("");
	const TCHAR* PublicSymbols = TEXT("");
	const TCHAR* CallContext = TEXT("");
	const TCHAR* IdentityAndLifetime = TEXT("");
	const TCHAR* TransactionAndRollback = TEXT("");
	const TCHAR* Completion = TEXT("");
	const TCHAR* CompileAndLinkResult = TEXT("");
	const TCHAR* FocusedTestId = TEXT("");
	const TCHAR* StableDiagnostic = TEXT("");
	bool bLocalAuthorityClosed = false;
};

TConstArrayView<FAssetDocumentAnimationAuthorityGateEvidence>
GetAssetDocumentAnimationAuthorityGateEvidence();

#endif
