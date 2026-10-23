// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

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
	FrozenNotApplicable = 0,
	VerifiedAssetIdentityReady = 1,
	RuntimeHandleBootstrapBlocked = 2,
};

enum class EAssetDocumentAnimationFinalDisposition : uint8
{
	Admitted = 0,
	StableBlocker = 1,
};

struct FAssetDocumentAnimationAuthorityDecisionRecord
{
	const TCHAR* DecisionId = TEXT("");
	const TCHAR* AuthorityGroupId = TEXT("");
	const TCHAR* RowDiagnostic = TEXT("");
	bool bFrozenBlocker = false;
	bool bMutating = false;
	uint8 RequiredInputHandleCount = 0;
	EAssetDocumentAnimationBootstrapDependency BootstrapDependency =
		EAssetDocumentAnimationBootstrapDependency::None;
	EAssetDocumentAnimationBootstrapDisposition BootstrapDisposition =
		EAssetDocumentAnimationBootstrapDisposition::FrozenNotApplicable;
	EAssetDocumentAnimationFinalDisposition FinalDisposition =
		EAssetDocumentAnimationFinalDisposition::StableBlocker;
	const TCHAR* FinalDiagnostic = TEXT("");
};

TConstArrayView<FAssetDocumentAnimationAuthorityDecisionRecord>
GetAssetDocumentAnimationAuthorityDecisionRecords();

struct FAssetDocumentAnimationAuthorityEvidence
{
	const TCHAR* GroupId = TEXT("");
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

TConstArrayView<FAssetDocumentAnimationAuthorityEvidence>
GetAssetDocumentAnimationAuthorityEvidence();

#endif
