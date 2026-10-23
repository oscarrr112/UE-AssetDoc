// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentRequiredPrimitiveProviderView.h"
#include "AssetDocumentAIPrimitiveProvider.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "Misc/AutomationTest.h"
#include "AssetDocumentPythonRuntime.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS
TArray<FAssetDocumentPrimitiveRequirement> GetAssetDocumentLegacyGenericRequirementsForTests();

namespace
{
class FProjectionProvider final : public IAssetDocumentPrimitiveProvider
{
public:
	explicit FProjectionProvider(TSharedRef<IAssetDocumentPrimitiveProvider> Source)
		: Id(Source->GetProviderId()), Availability(Source->InspectAvailability())
	{
		Descriptors.Append(Source->DescribePrimitives());
		Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
		Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::String;
		Outcome.Value.StringValue = TEXT("provider-owned-value");
		FAssetDocumentRuntimeDiagnostic& Diagnostic = Outcome.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Code = TEXT("Test.ProviderFailure");
		Diagnostic.Path = TEXT("/original");
		Diagnostic.Message = TEXT("original provider diagnostic");
	}
	FString GetProviderId() const override { return Id; }
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> DescribePrimitives() const override { return Descriptors; }
	FAssetDocumentProviderAvailability InspectAvailability() const override { return Availability; }
	FAssetDocumentPrimitiveOutcome Invoke(const FAssetDocumentPrimitiveInvocation& Invocation,
		IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction* Transaction) override
	{
		++InvokeCount;
		Observed = Invocation;
		ObservedHandles = &Handles;
		ObservedTransaction = Transaction;
		return Outcome;
	}
	FString Id;
	FAssetDocumentProviderAvailability Availability;
	TArray<FAssetDocumentPrimitiveDescriptor> Descriptors;
	FAssetDocumentPrimitiveOutcome Outcome;
	FAssetDocumentPrimitiveInvocation Observed;
	IAssetDocumentHandleResolver* ObservedHandles = nullptr;
	IAssetDocumentAtomicTransaction* ObservedTransaction = nullptr;
	int32 InvokeCount = 0;
};

class FUnusedHandles final : public IAssetDocumentHandleResolver
{
public:
	FAssetDocumentIssuedHandleResult Issue(EAssetDocumentHandleKind, EAssetDocumentHandleAccess,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&&) override { ++Calls; return {}; }
	FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(const FAssetDocumentOpaqueHandle&,
		const FAssetDocumentHandleValidationContext&) override { ++Calls; return {}; }
	FAssetDocumentHandleInvalidationResult Invalidate(const FAssetDocumentHandleInvalidation&) override
	{ ++Calls; return {}; }
	int32 Calls = 0;
};

// Exact replay consumers, including existing-target resolution; independent of the builder.
const TCHAR* AnimationIds[] = {
	TEXT("editor.class.resolve_canonical.v1"),
	TEXT("editor.reference.resolve_asset.v1"),
	TEXT("editor.asset.resolve_existing.v1"),
	TEXT("editor.property.enumerate_admitted_batch.v1"),
	TEXT("editor.property.read_admitted.v1"),
	TEXT("editor.property.write_admitted_batch.v1"),
	TEXT("editor.property.preflight_admitted.v1")};

FString Tuple(const FAssetDocumentPrimitiveRequirement& Requirement)
{
	return FString::Printf(TEXT("%s|%u|%s"), *Requirement.PrimitiveId,
		Requirement.Version, *Requirement.RequiredByRouteId);
}
TArray<FString> Tuples(TConstArrayView<FAssetDocumentPrimitiveRequirement> Requirements)
{
	TArray<FString> Result;
	for (const auto& Requirement : Requirements) { Result.Add(Tuple(Requirement)); }
	return Result;
}

struct FProjectionFixture
{
	TSharedRef<FProjectionProvider> Editor = MakeShared<FProjectionProvider>(CreateAssetDocumentEditorPrimitiveProvider());
	TSharedRef<FProjectionProvider> AI = MakeShared<FProjectionProvider>(CreateAssetDocumentAIPrimitiveProvider());
	TArray<TSharedRef<IAssetDocumentPrimitiveProvider>> Providers = {Editor, AI};
	TArray<FAssetDocumentPrimitiveRequirement> Seed = GetAssetDocumentLegacyGenericRequirementsForTests();
	FAssetDocumentRequiredPrimitiveProjectionResult Build() const
	{
		return BuildAssetDocumentRequiredPrimitiveProjection(Providers, Seed, GetFrozenAuthorityExceptionRecords());
	}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRequiredProjectionClosureTest,
	"UEAssetDoc.GenericRuntime.Projection.ConsumerClosure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRequiredProjectionClosureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FProjectionFixture Fixture;
	// 24 legacy rows + Validate's trial target + the Enhanced Input domain check (route.generic.v1).
	TestEqual(TEXT("legacy generic and Blackboard seed remains 26"), Fixture.Seed.Num(), 26);
	for (const TCHAR* Id : AnimationIds)
	{
		Fixture.Seed.Add({Id, 1, TEXT("animation_profile")});
	}
	// Provider consumer lists are structural only; the declared seed decides consumers.
	for (auto& Descriptor : Fixture.Editor->Descriptors)
	{
		if (!Descriptor.RequiredConsumerIds.IsEmpty()) { Descriptor.RequiredConsumerIds = {TEXT("structural_only")}; }
	}
	const auto Result = Fixture.Build();
	for (const auto& D : Result.Diagnostics) { AddError(D.Code + TEXT(": ") + D.Message); }
	if (!TestTrue(TEXT("complete projection publishes"), Result.Projection.IsValid())) { return false; }
	const auto& Projection = *Result.Projection;
	TestEqual(TEXT("only 26 seeded descriptors selected"), Projection.Descriptors.Num(), 26);
	TArray<FString> Expected = Tuples(Fixture.Seed);
	Expected.Sort();
	TestTrue(TEXT("requirements are exactly the sorted declared seed"), Tuples(Projection.Requirements) == Expected);
	TestTrue(TEXT("declared closure passes Core"), ValidateAssetDocumentPrimitiveFreezeInput(
		Projection.Descriptors, Projection.Requirements, Projection.Availability,
		GetFrozenAuthorityExceptionRecords()).bValid);
	TestEqual(TEXT("canonical first provider"), Projection.Providers[0]->GetProviderId(), FString(TEXT("ai")));
	TestEqual(TEXT("canonical second provider"), Projection.Providers[1]->GetProviderId(), FString(TEXT("editor")));
	for (int32 Index = 1; Index < Projection.Descriptors.Num(); ++Index)
	{
		TestTrue(TEXT("descriptors canonical"), Projection.Descriptors[Index - 1].PrimitiveId < Projection.Descriptors[Index].PrimitiveId);
	}
	for (const auto& Descriptor : Projection.Descriptors)
	{
		TArray<FString> Routes;
		for (const auto& Row : Fixture.Seed)
		{
			if (Row.PrimitiveId == Descriptor.PrimitiveId && Row.Version == Descriptor.Version) { Routes.Add(Row.RequiredByRouteId); }
		}
		Routes.Sort();
		TestTrue(TEXT("consumers are the sorted declaring routes"), Descriptor.RequiredConsumerIds == Routes);
		const auto& Source = Descriptor.ProviderId == TEXT("editor") ? Fixture.Editor : Fixture.AI;
		const auto* Original = Source->Descriptors.FindByPredicate(
			[&](const auto& D) { return D.PrimitiveId == Descriptor.PrimitiveId; });
		if (!TestNotNull(TEXT("selected descriptor has source"), Original)) { return false; }
		TestEqual(TEXT("authority API unchanged"), Descriptor.Admission.ExportedAuthoritativeApi, Original->Admission.ExportedAuthoritativeApi);
		TestEqual(TEXT("fingerprint unchanged"), Descriptor.Admission.AuthorityFingerprint, Original->Admission.AuthorityFingerprint);
		TestTrue(TEXT("disposition unchanged"), Descriptor.Admission.Disposition == Original->Admission.Disposition);
		TestEqual(TEXT("input schema unchanged"), Descriptor.InputSchemaId, Original->InputSchemaId);
		TestEqual(TEXT("output schema unchanged"), Descriptor.OutputSchemaId, Original->OutputSchemaId);
	}
	// Input order must not change the immutable output order.
	Fixture.Providers.Swap(0, 1);
	Fixture.Seed.Swap(0, Fixture.Seed.Num() - 1);
	Fixture.Editor->Descriptors.Swap(0, Fixture.Editor->Descriptors.Num() - 1);
	const auto Reordered = Fixture.Build();
	if (!TestTrue(TEXT("reordered inputs publish"), Reordered.Projection.IsValid())) { return false; }
	TestTrue(TEXT("input permutations preserve canonical tuples"), Tuples(Reordered.Projection->Requirements) == Expected);
	for (int32 Index = 0; Index < Projection.Descriptors.Num(); ++Index)
	{
		TestEqual(TEXT("input permutations preserve descriptor order"),
			Reordered.Projection->Descriptors[Index].PrimitiveId, Projection.Descriptors[Index].PrimitiveId);
		TestTrue(TEXT("input permutations preserve consumer order"),
			Reordered.Projection->Descriptors[Index].RequiredConsumerIds == Projection.Descriptors[Index].RequiredConsumerIds);
	}
	// Any route may declare an existing primitive; it joins that descriptor's consumers.
	Fixture.Seed.Add({AnimationIds[0], 1, TEXT("route.new_profile.v1")});
	const auto ThreeConsumers = Fixture.Build();
	if (!TestTrue(TEXT("a new route may share a descriptor"), ThreeConsumers.Projection.IsValid())) { return false; }
	TestEqual(TEXT("shared descriptor is not duplicated"), ThreeConsumers.Projection->Descriptors.Num(), 26);
	TestEqual(TEXT("new route adds one requirement"), ThreeConsumers.Projection->Requirements.Num(), Expected.Num() + 1);
	const auto* Shared = ThreeConsumers.Projection->Descriptors.FindByPredicate(
		[](const auto& D) { return D.PrimitiveId == AnimationIds[0]; });
	if (!TestNotNull(TEXT("shared descriptor selected"), Shared)) { return false; }
	TestTrue(TEXT("shared descriptor lists every declaring route"), Shared->RequiredConsumerIds
		== TArray<FString>{TEXT("animation_profile"), TEXT("route.generic.v1"), TEXT("route.new_profile.v1")});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRequiredProjectionForwardingTest,
	"UEAssetDoc.GenericRuntime.Projection.Forwarding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRequiredProjectionForwardingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FProjectionFixture Fixture;
	Fixture.Seed = {{TEXT("editor.class.resolve_canonical.v1"), 1, TEXT("route.generic.v1")}};
	const auto Result = Fixture.Build();
	if (!TestTrue(TEXT("single operation projection publishes"), Result.Projection.IsValid())) { return false; }
	TestEqual(TEXT("unused AI provider is dormant"), Result.Projection->Providers.Num(), 1);
	FUnusedHandles Handles;
	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = Fixture.Seed[0].PrimitiveId;
	Invocation.Version = 1;
	Invocation.RequestId = FGuid::NewGuid();
	Invocation.TransactionId = FGuid::NewGuid();
	Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
	Invocation.RuntimeRevision = 42;
	Invocation.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::String;
	Invocation.ValueArguments.StringValue = TEXT("original-input");
	const auto Outcome = Result.Projection->Providers[0]->Invoke(Invocation, Handles, nullptr);
	TestEqual(TEXT("selected invocation delegated once"), Fixture.Editor->InvokeCount, 1);
	TestTrue(TEXT("resolver identity retained"), Fixture.Editor->ObservedHandles == &Handles);
	TestTrue(TEXT("request identity retained"), Fixture.Editor->Observed.RequestId == Invocation.RequestId);
	TestTrue(TEXT("transaction identity retained"), Fixture.Editor->Observed.TransactionId == Invocation.TransactionId);
	TestTrue(TEXT("phase retained"), Fixture.Editor->Observed.Phase == Invocation.Phase);
	TestTrue(TEXT("null transaction passed through"), Fixture.Editor->ObservedTransaction == nullptr);
	TestEqual(TEXT("revision retained"), Fixture.Editor->Observed.RuntimeRevision, 42ull);
	TestEqual(TEXT("argument retained"), Fixture.Editor->Observed.ValueArguments.StringValue, Invocation.ValueArguments.StringValue);
	TestTrue(TEXT("provider failure status preserved"), Outcome.Status == Fixture.Editor->Outcome.Status);
	TestEqual(TEXT("provider value preserved"), Outcome.Value.StringValue, Fixture.Editor->Outcome.Value.StringValue);
	if (!TestEqual(TEXT("provider diagnostic count retained"), Outcome.Diagnostics.Num(), 1)) { return false; }
	TestEqual(TEXT("provider diagnostic preserved"), Outcome.Diagnostics[0].Message, Fixture.Editor->Outcome.Diagnostics[0].Message);
	TestEqual(TEXT("provider code preserved"), Outcome.Diagnostics[0].Code, Fixture.Editor->Outcome.Diagnostics[0].Code);
	TestEqual(TEXT("provider path preserved"), Outcome.Diagnostics[0].Path, Fixture.Editor->Outcome.Diagnostics[0].Path);
	for (int32 Case = 0; Case != 2; ++Case)
	{
		Invocation.PrimitiveId = Case == 0 ? TEXT("editor.property.read_admitted.v1") : Fixture.Seed[0].PrimitiveId;
		Invocation.Version = Case == 0 ? 1 : 2;
		const auto Rejected = Result.Projection->Providers[0]->Invoke(Invocation, Handles, nullptr);
		TestTrue(TEXT("unselected or wrong version is unavailable"), Rejected.Status == EAssetDocumentPrimitiveOutcomeStatus::Unavailable);
		if (!TestEqual(TEXT("one unavailable diagnostic"), Rejected.Diagnostics.Num(), 1)) { return false; }
		TestEqual(TEXT("exact unavailable diagnostic"), Rejected.Diagnostics[0].Code, FString(TEXT("AD.RUNTIME.PRIMITIVE_UNAVAILABLE")));
	}
	TestEqual(TEXT("rejected invocations never delegated"), Fixture.Editor->InvokeCount, 1);
	TestEqual(TEXT("no handle operations"), Handles.Calls, 0);
	TestEqual(TEXT("dormant provider never invoked"), Fixture.AI->InvokeCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRequiredProjectionRejectionTest,
	"UEAssetDoc.GenericRuntime.Projection.Rejection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRequiredProjectionRejectionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FProjectionFixture ValidFixture;
	const auto Previous = ValidFixture.Build();
	if (!TestTrue(TEXT("valid previous projection exists"), Previous.Projection.IsValid())) { return false; }
	const auto* PreviousIdentity = Previous.Projection.Get();
	const auto PreviousTuples = Tuples(Previous.Projection->Requirements);
	const TCHAR* Cases[] = {TEXT("empty providers"), TEXT("empty seed"), TEXT("duplicate provider"),
		TEXT("duplicate seed"), TEXT("missing primitive"), TEXT("wrong version"), TEXT("one missing among valid rows"),
		TEXT("wrong descriptor provider"), TEXT("unavailable provider"), TEXT("noncallable"),
		TEXT("duplicate descriptor"), TEXT("invalid schema at freeze"), TEXT("empty consumer"),
		TEXT("availability identity mismatch"), TEXT("empty primitive"), TEXT("zero version")};
	for (int32 Case = 0; Case < UE_ARRAY_COUNT(Cases); ++Case)
	{
		FProjectionFixture Fixture;
		auto* Selected = Fixture.Editor->Descriptors.FindByPredicate(
			[](const auto& D) { return D.PrimitiveId == TEXT("editor.class.resolve_canonical.v1"); });
		if (!TestNotNull(TEXT("selected fixture exists"), Selected)) { return false; }
		switch (Case)
		{
		case 0: Fixture.Providers.Reset(); break;
		case 1: Fixture.Seed.Reset(); break;
		case 2: Fixture.Providers.Add(Fixture.Editor); break;
		case 3: { const auto Copy = Fixture.Seed[0]; Fixture.Seed.Add(Copy); break; }
		case 4: Fixture.Seed[0].PrimitiveId = TEXT("editor.missing.v1"); break;
		case 5: Fixture.Seed[0].Version = 99; break;
		case 6: Fixture.Seed.Add({TEXT("editor.missing.v2"), 2, TEXT("route.generic.v1")}); break;
		case 7: Selected->ProviderId = TEXT("ai"); break;
		case 8: Fixture.Editor->Availability.State = EAssetDocumentProviderAvailabilityState::Unavailable; break;
		case 9: Selected->Admission.Disposition = EAssetDocumentPrimitiveDisposition::StableBlocker; break;
		case 10: { const auto Copy = *Selected; Fixture.Editor->Descriptors.Add(Copy); break; }
		case 11: Selected->InputSchemaId.Reset(); break;
		case 12: Fixture.Seed[0].RequiredByRouteId.Reset(); break;
		case 13: Fixture.Editor->Availability.ProviderId = TEXT("ai"); break;
		case 14: Fixture.Seed[0].PrimitiveId.Reset(); break;
		case 15: Fixture.Seed[0].Version = 0; break;
		}
		const auto Rejected = Fixture.Build();
		TestFalse(Cases[Case], Rejected.Projection.IsValid());
		TestTrue(TEXT("rejection carries diagnostics"), !Rejected.Diagnostics.IsEmpty());
		TestEqual(TEXT("rejection does not invoke providers"), Fixture.Editor->InvokeCount + Fixture.AI->InvokeCount, 0);
		TestTrue(TEXT("previous publication identity unchanged"), Previous.Projection.Get() == PreviousIdentity);
		TestTrue(TEXT("previous publication contents unchanged"), Tuples(Previous.Projection->Requirements) == PreviousTuples);
	}
	{
		// A route the provider catalog never names is accepted; consumers follow declarations.
		FProjectionFixture Fixture;
		Fixture.Seed = {{TEXT("editor.class.resolve_canonical.v1"), 1, TEXT("undeclared_profile")}};
		const auto Accepted = Fixture.Build();
		if (!TestTrue(TEXT("undeclared consumer route publishes"), Accepted.Projection.IsValid())) { return false; }
		TestTrue(TEXT("descriptor consumer is the declaring route"),
			Accepted.Projection->Descriptors[0].RequiredConsumerIds == TArray<FString>{TEXT("undeclared_profile")});
	}
	{
		// Every missing declaration is reported with its exact identity.
		FProjectionFixture Fixture;
		Fixture.Seed.Add({TEXT("editor.missing.v1"), 1, TEXT("route.new_profile.v1")});
		Fixture.Seed.Add({TEXT("editor.class.resolve_canonical.v1"), 7, TEXT("route.new_profile.v1")});
		const auto Missing = Fixture.Build();
		TestFalse(TEXT("missing declarations are not published"), Missing.Projection.IsValid());
		if (!TestEqual(TEXT("one diagnostic per missing declaration"), Missing.Diagnostics.Num(), 2)) { return false; }
		const auto& First = Missing.Diagnostics[0];
		TestEqual(TEXT("missing code"), First.Code, FString(TEXT("PrimitiveRegistry.MissingRequiredPrimitive")));
		TestEqual(TEXT("missing path indexes the seed"), First.Path, FString::Printf(TEXT("/PrimitiveRegistry/RequiredPrimitives/%d"), Fixture.Seed.Num() - 2));
		if (TestEqual(TEXT("missing context has id/version/route"), First.Context.Num(), 3))
		{
			TestEqual(TEXT("context id"), First.Context[0].Value.StringValue, FString(TEXT("editor.missing.v1")));
			TestEqual(TEXT("context version"), First.Context[1].Value.IntegerValue, int64{1});
			TestEqual(TEXT("context route"), First.Context[2].Value.StringValue, FString(TEXT("route.new_profile.v1")));
		}
		TestTrue(TEXT("message names the route"), First.Message.Contains(TEXT("route.new_profile.v1")));
		TestEqual(TEXT("wrong version is also missing"), Missing.Diagnostics[1].Context[1].Value.IntegerValue, int64{7});
	}
	// Both production bridge descriptors bind the unmodified Core catalog; wrong symbols still fail.
	for (const TCHAR* Id : {TEXT("ai.blackboard.entry.remove.v2"), TEXT("ai.blackboard.local_keys.reorder.v2")})
	{
		FProjectionFixture Fixture;
		Fixture.Seed = {{Id, 2, TEXT("ai_profile")}};
		TestTrue(TEXT("production bridge freezes without normalization"), Fixture.Build().Projection.IsValid());
		auto* Bridge = Fixture.AI->Descriptors.FindByPredicate([&](const auto& D) { return D.PrimitiveId == Id; });
		if (!TestNotNull(TEXT("bridge descriptor exists"), Bridge)) { return false; }
		Bridge->Admission.ExportedAuthoritativeApi += TEXT(".wrong");
		const auto Rejected = Fixture.Build();
		TestFalse(TEXT("wrong bridge binding is not published"), Rejected.Projection.IsValid());
		TestTrue(TEXT("Core bridge binding error is preserved"), Rejected.Diagnostics.ContainsByPredicate(
			[](const auto& D) { return D.Code == TEXT("PrimitiveRegistry.BridgeAuthorityBindingMismatch"); }));
	}
	return true;
}
#endif
