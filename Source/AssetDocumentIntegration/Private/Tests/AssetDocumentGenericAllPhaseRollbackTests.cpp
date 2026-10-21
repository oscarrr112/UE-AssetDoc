// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentGenericRuntimeRoute.h"
#include "AssetDocumentTestDirectoryBlock.h"

#include "AssetDocumentAIPrimitiveProvider.h"
#include "AssetDocumentCoreRuntimeComposition.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "AssetDocumentEditorMutationPreparation.h"
#include "AssetDocumentRequiredPrimitiveProviderView.h"
#include "AssetDocumentGenericRuntimeBootstrap.h"
#include "AssetDocumentPythonRuntime.h"
#include "AssetDocumentService.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/SavePackage.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "../../../AssetDocument/Private/Tests/AssetDocumentSidecarWriteTestSupport.h"
#include "AssetDocumentPluginPaths.h"

FAssetDocumentGenericRuntimeBootstrap*
GetAssetDocumentActiveGenericRuntimeBootstrapForTests();

uint64 GetAssetDocumentActiveGenericRuntimeRevisionForTests();
TArray<FAssetDocumentPrimitiveRequirement> GetAssetDocumentLegacyGenericRequirementsForTests();

namespace
{
constexpr uint64 TestRuntimeRevision = 1;

struct FT6FileTreeSnapshot
{
	TArray<FString> Paths;
	TArray<TArray<uint8>> Contents;

	bool operator==(const FT6FileTreeSnapshot& Other) const
	{
		return Paths == Other.Paths && Contents == Other.Contents;
	}
};

FT6FileTreeSnapshot SnapshotT6Files(const FString& Root)
{
	FT6FileTreeSnapshot Snapshot;
	if (IFileManager::Get().FileExists(*Root))
	{
		Snapshot.Paths.Add(Root);
	}
	else
	{
		IFileManager::Get().FindFilesRecursive(
			Snapshot.Paths,
			*Root,
			TEXT("*"),
			true,
			false,
			false);
	}
	Snapshot.Paths.Sort();
	for (const FString& Path : Snapshot.Paths)
	{
		TArray<uint8>& Bytes = Snapshot.Contents.AddDefaulted_GetRef();
		FFileHelper::LoadFileToArray(Bytes, *Path);
	}
	return Snapshot;
}

FAssetDocumentRuntimeResultDto MakeInjectedPhaseFailure(
	const FAssetDocumentRuntimeRequestDto& Request)
{
	FAssetDocumentRuntimeResultDto Result;
	Result.RequestId = Request.RequestId;
	Result.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Failed;
	Result.FinalPhase = EAssetDocumentRuntimeRequestPhase::Failed;
	Result.RuntimeRevision = Request.ExpectedRuntimeRevision;
	FAssetDocumentRuntimeDiagnostic& Diagnostic =
		Result.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/GenericRuntime/AllPhaseRollback");
	Diagnostic.Code = TEXT("Test.GenericPhaseFailure");
	Diagnostic.Message = TEXT("Injected normal-route phase failure.");
	Diagnostic.Severity = EAssetDocumentDiagnosticSeverity::Error;
	return Result;
}


// Test-owned value producer after the actual Python scratch preflight succeeds.
// It carries the exact TestString intent already validated by that preflight.
FAssetDocumentRuntimeValueDto MakeT6MutationProjection(
 const FAssetDocumentRuntimeRequestDto& Request, const FString& Hash, bool bOmitTerminalStep)
{
 using FValue = FAssetDocumentRuntimeValueDto;
 auto Text = [](const FString& Value) { FValue R; R.Kind = EAssetDocumentRuntimeValueKind::String; R.StringValue = Value; return R; };
 auto Number = [](int64 Value) { FValue R; R.Kind = EAssetDocumentRuntimeValueKind::Integer; R.IntegerValue = Value; return R; };
 auto Array = [](TArray<FValue> Values) { FValue R; R.Kind = EAssetDocumentRuntimeValueKind::Array; R.Elements = MoveTemp(Values); return R; };
 auto Object = [](TArray<FString> Keys, TArray<FValue> Values) { FValue R; R.Kind = EAssetDocumentRuntimeValueKind::Object; R.ObjectKeys = MoveTemp(Keys); R.Elements = MoveTemp(Values); return R; };
 const int32 PropertiesIndex = Request.CanonicalDocument.ObjectKeys.IndexOfByKey(TEXT("Properties"));
 const FValue& Properties = Request.CanonicalDocument.Elements[PropertiesIndex];
 const FValue& Desired = Properties.Elements[Properties.ObjectKeys.IndexOfByKey(TEXT("TestString"))];
 const FValue Intent = Object({TEXT("properties")}, {Array({Object({TEXT("name"), TEXT("value")}, {Text(TEXT("TestString")), Desired})})});
 auto Step = [&](int32 Index, const TCHAR* Phase, const FValue& StepIntent)
 {
  return Object({TEXT("Index"), TEXT("PrimitiveId"), TEXT("Version"), TEXT("Phase"), TEXT("InputSchemaId"), TEXT("Intent")},
   {Number(Index), Text(TEXT("editor.property.write_admitted_batch.v1")), Number(1), Text(Phase),
    Text(TEXT("editor.property.write_admitted_batch.v1.input.v1")), StepIntent});
 };
 TArray<FValue> Steps{Step(0, TEXT("Apply"), Intent)};
 // Generic Repair intentionally schedules no setter. The extra empty expected
 // step has no second field owner, and must block persistence when unconsumed.
 if (bOmitTerminalStep) Steps.Add(Step(1, TEXT("Repair"), Object({TEXT("properties")}, {Array({})})));
 return Object({TEXT("Kind"), TEXT("RequestId"), TEXT("RuntimeRevision"), TEXT("ExactAssetClassPath"), TEXT("TargetAssetPath"), TEXT("Operation"), TEXT("SemanticHash"), TEXT("Steps")},
  {Text(TEXT("MutationIntentProjectionV1")), Text(Request.RequestId.ToString(EGuidFormats::DigitsWithHyphensLower)),
   Number(Request.ExpectedRuntimeRevision), Text(Request.RequestedAssetClassPath), Text(Request.TargetAssetPath), Text(TEXT("Apply")), Text(Hash), Array(MoveTemp(Steps))});
}

class FPhaseInjectingHost final : public IAssetDocumentPythonRuntimeHost
{
public:
	explicit FPhaseInjectingHost(IAssetDocumentPythonRuntimeHost& InInner)
		: Inner(InInner)
	{
	}

	FAssetDocumentRuntimeResultDto Dispatch(
		const FAssetDocumentRuntimeRequestDto& Request,
		IAssetDocumentNativeRequestContext& Context) override
	{
		const EAssetDocumentRuntimeRequestPhase Phase =
			Context.GetMetadata().Phase;
		if (Request.Operation == EAssetDocumentRuntimeOperation::Diff)
		{
			// The route's read-only unchanged-document probe is not a mutation
			// phase; record it separately and never inject failures into it.
			++ObservedDiffProbes;
			return Inner.Dispatch(Request, Context);
		}
		ObservedPhases.Add(Phase);
		if (FailPhase.IsSet() && FailPhase.GetValue() == Phase)
		{
			return MakeInjectedPhaseFailure(Request);
		}
		auto Result = Inner.Dispatch(Request, Context);
  if (bMutationProjection && Result.TerminalStatus == EAssetDocumentRuntimeTerminalStatus::Succeeded)
  {
   if (Phase == EAssetDocumentRuntimeRequestPhase::Validate)
   {
    ValidatedSemanticHash = Result.SemanticHash;
    ValidatedCanonicalBytes = Result.CanonicalBytes;
    ValidatedCodecVersion = Result.CodecVersion;
   }
   if (Phase == EAssetDocumentRuntimeRequestPhase::Preflight)
   {
    // The production generic scratch result has no semantic payload. This
    // test-owned producer binds its intent to the successful Validate evidence.
    Result.SemanticHash = ValidatedSemanticHash;
    Result.CanonicalBytes = ValidatedCanonicalBytes;
    Result.CodecVersion = ValidatedCodecVersion;
    Result.CanonicalValue = MakeT6MutationProjection(Request, ValidatedSemanticHash, bOmitTerminalStep);
    if (bMalformedProjection)
    {
     FAssetDocumentRuntimeValueDto EmptyObject;
     EmptyObject.Kind = EAssetDocumentRuntimeValueKind::Object;
     Result.CanonicalValue = MoveTemp(EmptyObject);
    }
   }
   if (Phase == EAssetDocumentRuntimeRequestPhase::Apply)
   {
    UObject* Target = FSoftObjectPath(Request.TargetAssetPath).ResolveObject();
    FStrProperty* Property = Target ? FindFProperty<FStrProperty>(Target->GetClass(), TEXT("TestString")) : nullptr;
    bObservedChangedProperty = Property && Property->GetPropertyValue_InContainer(Target) == TEXT("T6 mutation desired");
    ObservedApplyChangedSet = Result.ChangedSet;
   }
   if (Phase == EAssetDocumentRuntimeRequestPhase::Repair && bOmitTerminalStep)
   {
    // Report the real preceding mutation in a successful Repair result. The
    // route must preserve Incomplete when it converts this result to failure,
    // removing the success-only changed lane before Core validates the result.
    Result.ChangedSet = ObservedApplyChangedSet;
    bInjectedValidRepairChangedSet = !Result.ChangedSet.IsEmpty()
     && ValidateAssetDocumentRuntimeResult(Request, Result).bValid;
   }
  }
  return Result;
	}

	void Reset(TOptional<EAssetDocumentRuntimeRequestPhase> InFailPhase)
	{
		FailPhase = InFailPhase;
		ObservedPhases.Reset();
		ObservedDiffProbes = 0;
  bMutationProjection = false;
  bOmitTerminalStep = false;
  bObservedChangedProperty = false;
  bMalformedProjection = false;
  ValidatedSemanticHash.Reset();
  ValidatedCanonicalBytes.Reset();
  ValidatedCodecVersion = 0;
  ObservedApplyChangedSet.Reset();
  bInjectedValidRepairChangedSet = false;
	}

	TArray<EAssetDocumentRuntimeRequestPhase> ObservedPhases;
	int32 ObservedDiffProbes = 0;
 bool bMutationProjection = false;
 bool bOmitTerminalStep = false;
 bool bObservedChangedProperty = false;
 bool bMalformedProjection = false;
 bool bInjectedValidRepairChangedSet = false;

private:
 TArray<FAssetDocumentChangedRecord> ObservedApplyChangedSet;
 FString ValidatedSemanticHash;
 TArray<uint8> ValidatedCanonicalBytes;
 uint32 ValidatedCodecVersion = 0;
	IAssetDocumentPythonRuntimeHost& Inner;
	TOptional<EAssetDocumentRuntimeRequestPhase> FailPhase;
};

class FPhaseInjectingRuntime final : public IAssetDocumentPythonRuntime
{
private:
	TUniquePtr<IAssetDocumentPythonRuntime> Inner;

public:
	explicit FPhaseInjectingRuntime(
		const FAssetDocumentPythonRuntimeConfiguration& Configuration)
		: Inner(CreateAssetDocumentPythonRuntime(Configuration))
		, Host(Inner->GetHost())
	{
	}

	FAssetDocumentContractValidationResult InitializePythonBinding() override
	{
		return Inner->InitializePythonBinding();
	}

	FAssetDocumentPythonRevisionActivationResult TryActivateRevision(
		const FAssetDocumentPythonRevisionCandidate& Candidate) override
	{
		return Inner->TryActivateRevision(Candidate);
	}

	uint64 GetActiveRuntimeRevision() const override
	{
		return Inner->GetActiveRuntimeRevision();
	}

	bool HasActiveRequests() const override
	{
		return Inner->HasActiveRequests();
	}

	IAssetDocumentPythonRuntimeHost& GetHost() override
	{
		return Host;
	}

	const IAssetDocumentPrimitiveRegistry* GetActivePrimitiveRegistry()
		const override
	{
		return Inner->GetActivePrimitiveRegistry();
	}

	FAssetDocumentPythonRequestContextCreateResult CreateRequestContext(
		const FAssetDocumentRequestContextMetadata& Metadata,
		const FGuid& TransactionId,
		IAssetDocumentAtomicTransaction* ActiveTransaction) override
	{
		return Inner->CreateRequestContext(
			Metadata,
			TransactionId,
			ActiveTransaction);
	}

	FAssetDocumentHandleInvalidationResult InvalidateHandles(
		const FAssetDocumentHandleInvalidation& Event) override
	{
		Invalidations.Add(Event);
		return Inner->InvalidateHandles(Event);
	}

	void ShutdownPythonBinding() override
	{
		Inner->ShutdownPythonBinding();
	}

	void Reset(TOptional<EAssetDocumentRuntimeRequestPhase> FailPhase)
	{
		Host.Reset(FailPhase);
		Invalidations.Reset();
	}

	int32 CountInvalidation(
		EAssetDocumentHandleInvalidationReason Reason) const
	{
		int32 Count = 0;
		for (const FAssetDocumentHandleInvalidation& Event : Invalidations)
		{
			Count += Event.Reason == Reason ? 1 : 0;
		}
		return Count;
	}

	FPhaseInjectingHost Host;

private:
	TArray<FAssetDocumentHandleInvalidation> Invalidations;
};

class FT6FrozenRegistryView final : public IAssetDocumentPrimitiveRegistry
{
public:
	explicit FT6FrozenRegistryView(
		const IAssetDocumentPrimitiveRegistry& InRegistry)
		: Registry(InRegistry)
	{
	}

	FAssetDocumentPrimitiveRegistrySnapshot Snapshot() const override
	{
		return Registry.Snapshot();
	}

	FAssetDocumentPrimitiveResolution Resolve(
		const FAssetDocumentPrimitiveRequirement& Requirement) const override
	{
		return Registry.Resolve(Requirement);
	}

private:
	const IAssetDocumentPrimitiveRegistry& Registry;
};

class FT6NoCancellation final : public IAssetDocumentCancellationView
{
public:
	bool IsCancellationRequested() const override
	{
		return false;
	}
};

class FPhaseObserver final : public IAssetDocumentRequestObserver
{
public:
	void OnPhaseChanged(
		FGuid RequestId,
		EAssetDocumentRuntimeRequestPhase Phase) override
	{
		(void)RequestId;
		Phases.Add(Phase);
	}

	void Reset()
	{
		Phases.Reset();
	}

	TArray<EAssetDocumentRuntimeRequestPhase> Phases;
};

class FT6StagingRootBlocker final
{
public:
	~FT6StagingRootBlocker()
	{
		Restore();
	}

	bool Block()
	{
		if (bBlocked)
		{
			return false;
		}
		Root = FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("AssetDocument/Runtime/Staging"));
		IPlatformFile& PlatformFile =
			FPlatformFileManager::Get().GetPlatformFile();
		bCreatedDirectory = !PlatformFile.DirectoryExists(*Root);
		if (bCreatedDirectory
			&& !PlatformFile.CreateDirectoryTree(*Root))
		{
			return false;
		}
		bWasReadOnly = PlatformFile.IsReadOnly(*Root);
		const bool bReadOnly = bWasReadOnly
			|| AssetDocumentTestDirectoryBlock::SetDirectoryReadOnly(PlatformFile, *Root, true);
		if (!bReadOnly)
		{
			RestoreOriginal();
			return false;
		}
		bBlocked = true;
		return PlatformFile.IsReadOnly(*Root);
	}

	bool Restore()
	{
		if (!bBlocked)
		{
			return true;
		}
		IPlatformFile& PlatformFile =
			FPlatformFileManager::Get().GetPlatformFile();
		const bool bRestored = RestoreOriginal();
		bBlocked = !bRestored;
		return bRestored;
	}

private:
	bool RestoreOriginal()
	{
		IPlatformFile& PlatformFile =
			FPlatformFileManager::Get().GetPlatformFile();
		if (!bWasReadOnly
			&& (PlatformFile.DirectoryExists(*Root) || PlatformFile.FileExists(*Root))
			&& !AssetDocumentTestDirectoryBlock::SetDirectoryReadOnly(PlatformFile, *Root, false))
		{
			return false;
		}
		if (bCreatedDirectory
			&& PlatformFile.DirectoryExists(*Root))
		{
			return IFileManager::Get().DeleteDirectory(
				*Root,
				false,
				false);
		}
		return true;
	}

	FString Root;
	bool bCreatedDirectory = false;
	bool bWasReadOnly = false;
	bool bBlocked = false;
};

FAssetDocumentPythonRuntimeConfiguration MakeT6RuntimeConfiguration()
{
	FAssetDocumentPythonRuntimeConfiguration Configuration;
	Configuration.RepositoryRoot =
		GetAssetDocumentPluginBaseDir();
	Configuration.PackageRootRelativePath = TEXT("Content/Python");
	return Configuration;
}

FAssetDocumentPythonRevisionCandidate MakeT6RevisionCandidate(
 const FAssetDocumentRequiredPrimitiveProjection& Projection)
{
 FAssetDocumentPythonRevisionCandidate Candidate;
 Candidate.RuntimeRevision = TestRuntimeRevision;
 Candidate.EntryModuleRelativePath = TEXT("asset_document/bootstrap.py");
 Candidate.RequiredPrimitives = Projection.Requirements;
 Candidate.Providers = Projection.Providers;
 Candidate.AuthorityCatalog.Append(GetG1FrozenAuthorityExceptionRecords());
 return Candidate;
}

class FNormalRouteHarness final
{
public:
	FNormalRouteHarness()
		: Runtime(MakeT6RuntimeConfiguration())
	{
	}

	~FNormalRouteHarness()
	{
		if (Service.IsValid() && Route.IsValid())
		{
			Service->UnbindGenericRuntimeRoute(Route.ToSharedRef());
		}
		if (Route.IsValid())
		{
			Route->UnbindComposition();
		}
		Composition.Reset();
		Registry.Reset();
		Route.Reset();
		Service.Reset();
		Providers.Reset();
		if (bBindingInitialized)
		{
			Runtime.ShutdownPythonBinding();
		}
	}

	bool Initialize(FAutomationTestBase& Test, bool bPreparationMode = false, bool bBindEditorCallback = true)
	{
		const FAssetDocumentContractValidationResult Binding =
			Runtime.InitializePythonBinding();
		bBindingInitialized = Binding.bValid;
		if (!Test.TestTrue(
			TEXT("T6 runtime binding initializes"),
			Binding.bValid))
		{
			return false;
		}
		Providers.Add(CreateAssetDocumentEditorPrimitiveProvider());
		Providers.Add(CreateAssetDocumentAIPrimitiveProvider());
  Route = MakeShared<FAssetDocumentGenericRuntimeRoute>(Runtime);
  TArray<TSharedRef<IAssetDocumentPrimitiveProvider>> RawProviders;
  for (const auto& Provider : Providers) RawProviders.Add(Provider.ToSharedRef());
  TArray<FAssetDocumentMutationPreparationBinding> Bindings;
  if (bPreparationMode && bBindEditorCallback)
   Bindings.Add({RawProviders[0], PrepareAssetDocumentEditorMutationBatch});
  FAssetDocumentPreparedInvocation PreparedInvoke;
  if (bPreparationMode)
   PreparedInvoke = [this](const FString& ProviderId, const FAssetDocumentPrimitiveInvocation& Invocation,
    IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction* Transaction,
    TFunctionRef<FAssetDocumentPrimitiveOutcome()> Forward)
   {
    ++PreparedInvocationCount;
    auto Outcome = Route->InvokePreparedMutation(ProviderId, Invocation, Handles, Transaction,
     [&]() { ++PreparedForwardCount; return Forward(); });
    if (Outcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded) ++PreparedSuccessCount;
    return Outcome;
   };
  // Editor + AI only: the generic route and Blackboard rows of the declared set.
  auto Requirements = GetAssetDocumentLegacyGenericRequirementsForTests();
  const auto Projected = BuildAssetDocumentRequiredPrimitiveProjection(RawProviders,
   Requirements, GetG1FrozenAuthorityExceptionRecords(), Bindings, MoveTemp(PreparedInvoke));
  for (const auto& Diagnostic : Projected.Diagnostics)
   Test.AddInfo(Diagnostic.Code + TEXT(": ") + Diagnostic.Message);
  if (!Test.TestTrue(TEXT("T6 exact selected provider projection freezes"), Projected.Projection.IsValid())) return false;
  PreparationProjection = Projected.Projection;
  const FAssetDocumentPythonRevisionActivationResult Activation =
   Runtime.TryActivateRevision(MakeT6RevisionCandidate(*PreparationProjection));
		if (!Test.TestTrue(
			TEXT("T6 production generic revision activates"),
			Activation.bActivated))
		{
			return false;
		}
		const IAssetDocumentPrimitiveRegistry* ActiveRegistry =
			Runtime.GetActivePrimitiveRegistry();
		if (!Test.TestNotNull(
			TEXT("T6 active primitive registry exists"),
			ActiveRegistry))
		{
			return false;
		}
		Registry = MakeUnique<FT6FrozenRegistryView>(*ActiveRegistry);
  if (bPreparationMode) Route->BindMutationPreparationProjection(PreparationProjection);
		FAssetDocumentCoreRuntimeDependencies Dependencies{
			*Route,
			*Registry,
			Cancellation,
			Observer,
			[this](
				const FAssetDocumentRequestContextMetadata& Metadata,
				const FGuid& TransactionId,
				IAssetDocumentAtomicTransaction* ActiveTransaction,
				TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
			{
    IAssetDocumentAtomicTransaction* RoutedTransaction = Route->ResolveTransactionForPhase(Metadata, ActiveTransaction, OutDiagnostics);
    if (ActiveTransaction != nullptr && RoutedTransaction == nullptr) return TUniquePtr<IAssetDocumentNativeRequestContext>();
				const FGuid ScopeId = TransactionId.IsValid()
					? TransactionId
					: FGuid::NewGuid();
				FAssetDocumentPythonRequestContextCreateResult Result =
					Runtime.CreateRequestContext(
						Metadata,
						ScopeId,
						RoutedTransaction);
				OutDiagnostics.Append(Result.Diagnostics);
				return MoveTemp(Result.Context);
			},
			[this](const FAssetDocumentHandleInvalidation& Event)
			{
				return Runtime.InvalidateHandles(Event);
			}};
		Composition =
			MakeUnique<FAssetDocumentCoreRuntimeComposition>(
				MoveTemp(Dependencies));
		if (!Test.TestTrue(
			TEXT("T6 Core composition is valid"),
			Composition->IsValid()))
		{
			return false;
		}
		Route->BindComposition(*Composition);
		Service = MakeShared<FAssetDocumentService>();
		Service->BindGenericRuntimeRoute(Route.ToSharedRef());
		return true;
	}

	void Reset(TOptional<EAssetDocumentRuntimeRequestPhase> FailPhase)
	{
		Runtime.Reset(FailPhase);
		Observer.Reset();
  PreparedInvocationCount = PreparedForwardCount = PreparedSuccessCount = 0;
	}

	FPhaseInjectingRuntime Runtime;
	TSharedPtr<FAssetDocumentService> Service;
 int32 PreparedInvocationCount = 0, PreparedForwardCount = 0, PreparedSuccessCount = 0;
 const TArray<EAssetDocumentRuntimeRequestPhase>& ObservedCorePhases() const { return Observer.Phases; }

private:
	TArray<TSharedPtr<IAssetDocumentPrimitiveProvider>> Providers;
 TSharedPtr<const FAssetDocumentRequiredPrimitiveProjection> PreparationProjection;
	TSharedPtr<FAssetDocumentGenericRuntimeRoute> Route;
	TUniquePtr<FT6FrozenRegistryView> Registry;
	TUniquePtr<FAssetDocumentCoreRuntimeComposition> Composition;
	FT6NoCancellation Cancellation;
	FPhaseObserver Observer;
	bool bBindingInitialized = false;
};

struct FNormalRouteFailureRow
{
	const TCHAR* Name;
	TOptional<EAssetDocumentRuntimeRequestPhase> FailPhase;
	bool bFailStaging = false;
	TArray<EAssetDocumentRuntimeRequestPhase> ExpectedHostPhases;
	const TCHAR* ExpectedDiagnostic = nullptr;
	int32 ExpectedRollbackInvalidations = 0;
 bool bMutationProjection = false;
 bool bOmitTerminalStep = false;
 int32 ExpectedPreparedCalls = 0;
 bool bMalformedProjection = false;
};

bool HasDiagnostic(
	const FAssetDocumentResult& Result,
	FStringView ExpectedCode)
{
	return Result.Diagnostics.ContainsByPredicate(
		[ExpectedCode](const FAssetDocumentDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == ExpectedCode;
		});
}

FString DescribeDiagnosticCodes(const FAssetDocumentResult& Result)
{
	TArray<FString> Codes;
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Codes.Add(Diagnostic.Code);
	}
	return Codes.IsEmpty() ? TEXT("<none>") : FString::Join(Codes, TEXT(", "));
}

const TCHAR* GetPhaseName(EAssetDocumentRuntimeRequestPhase Phase)
{
	switch (Phase)
	{
	case EAssetDocumentRuntimeRequestPhase::Validate:
		return TEXT("Validate");
	case EAssetDocumentRuntimeRequestPhase::Preflight:
		return TEXT("Preflight");
	case EAssetDocumentRuntimeRequestPhase::Apply:
		return TEXT("Apply");
	case EAssetDocumentRuntimeRequestPhase::Repair:
		return TEXT("Repair");
	case EAssetDocumentRuntimeRequestPhase::Persist:
		return TEXT("Persist");
	case EAssetDocumentRuntimeRequestPhase::Verify:
		return TEXT("Verify");
	case EAssetDocumentRuntimeRequestPhase::Complete:
		return TEXT("Complete");
	case EAssetDocumentRuntimeRequestPhase::Failed:
		return TEXT("Failed");
	}
	return TEXT("Unknown");
}

FString DescribeObservedPhases(
	const TArray<EAssetDocumentRuntimeRequestPhase>& Phases)
{
	TArray<FString> Names;
	for (const EAssetDocumentRuntimeRequestPhase Phase : Phases)
	{
		Names.Add(GetPhaseName(Phase));
	}
	return Names.IsEmpty() ? TEXT("<none>") : FString::Join(Names, TEXT(", "));
}

void RunEmptyApplyFilePathFailure(
	FAutomationTestBase& Test,
	FNormalRouteHarness& Harness)
{
	Harness.Reset({});
	FAssetDocumentApplyFileRequest Request;
	const FAssetDocumentResult Result =
		Harness.Service->ApplyFile(Request);
	Test.TestFalse(TEXT("empty ApplyFile path fails"), Result.IsSuccess());
	Test.TestEqual(
		TEXT("empty ApplyFile path preserves the failure message"),
		Result.Message,
		FString(TEXT("ApplyFile requires a sidecar file path")));
	Test.TestTrue(
		TEXT("empty ApplyFile path does not dispatch a runtime phase"),
		Harness.Runtime.Host.ObservedPhases.IsEmpty());
	Test.TestEqual(
		TEXT("empty ApplyFile path does not invalidate rollback handles"),
		Harness.Runtime.CountInvalidation(
			EAssetDocumentHandleInvalidationReason::Rollback),
		0);
}

void RunNormalRouteFailureRow(
	FAutomationTestBase& Test,
	FNormalRouteHarness& Harness,
	const FNormalRouteFailureRow& Row)
{
	const FString UniqueName = FString::Printf(
		TEXT("DA_AllPhase_%s_%s"),
		Row.Name,
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString FixtureDirectory = FPaths::Combine(
		FPaths::ProjectContentDir(),
		TEXT("AssetDocumentGenericCutover/AllPhaseRollback"),
		UniqueName);
	const FString Target = FString::Printf(
		TEXT("/Game/AssetDocumentGenericCutover/AllPhaseRollback/%s/%s"),
		*UniqueName,
		*UniqueName);
	const FString ObjectPath =
		FString::Printf(TEXT("%s.%s"), *Target, *UniqueName);
	const FString SidecarPath = FPaths::Combine(
		FixtureDirectory,
		UniqueName + TEXT(".assetdoc.json"));
	const FString PackagePath =
		FPackageName::LongPackageNameToFilename(
			Target,
			FPackageName::GetAssetPackageExtension());
	const FString RuntimeRoot = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AssetDocument/Runtime"));
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*FixtureDirectory, false, true);
	};

	UClass* FixtureClass = LoadObject<UClass>(
		nullptr,
		TEXT("/Script/AssetDocumentTests.TestDataAsset"));
	if (!Test.TestNotNull(
		*FString::Printf(TEXT("%s loads fixture class"), Row.Name),
		FixtureClass))
	{
		return;
	}
	UPackage* Package = CreatePackage(*Target);
	if (!Test.TestNotNull(
		*FString::Printf(TEXT("%s creates fixture package"), Row.Name),
		Package))
	{
		return;
	}
	UObject* FixtureAsset = NewObject<UObject>(
		Package,
		FixtureClass,
		FName(*UniqueName),
		RF_Public | RF_Standalone | RF_Transactional);
	if (!Test.TestNotNull(
		*FString::Printf(TEXT("%s creates existing fixture asset"), Row.Name),
		FixtureAsset))
	{
		return;
	}
	FStrProperty* TestStringProperty = FindFProperty<FStrProperty>(
		FixtureClass,
		TEXT("TestString"));
	if (!Test.TestNotNull(
		*FString::Printf(TEXT("%s reflects TestString"), Row.Name),
		TestStringProperty))
	{
		return;
	}
	const FString TestStringBaseline = TEXT("T6 rollback baseline");
	TestStringProperty->SetPropertyValue_InContainer(
		FixtureAsset,
		TestStringBaseline);
	const FString FixtureClassPath = FixtureAsset->GetClass()->GetPathName();
	const bool bPackageWasDirty = Package->IsDirty();
	Test.TestEqual(
		*FString::Printf(TEXT("%s fixture has the requested object identity"), Row.Name),
		FixtureAsset->GetPathName(),
		ObjectPath);
	FAssetDocumentExtractRequest ExtractRequest;
	ExtractRequest.AssetPath = ObjectPath;
	const FAssetDocumentResult Extracted =
		Harness.Service->Extract(ExtractRequest);
	if (!Test.TestTrue(
		*FString::Printf(TEXT("%s extracts fixture document"), Row.Name),
		Extracted.IsSuccess()
			&& Extracted.Payload.IsValid()))
	{
		return;
	}
	IFileManager::Get().MakeDirectory(*FixtureDirectory, true);
	const FString SidecarJson = FString::Printf(
		TEXT("{\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\",\"Target\":\"%s\",\"Class\":\"%s\",\"Action\":\"Update\",\"Properties\":{\"TestString\":\"%s\"}}"),
		*Target,
		*FixtureClassPath,
		*(Row.bMutationProjection ? FString(TEXT("T6 mutation desired")) : TestStringBaseline));
	if (!Test.TestTrue(
		*FString::Printf(TEXT("%s writes input sidecar"), Row.Name),
		FFileHelper::SaveStringToFile(SidecarJson, *SidecarPath)))
	{
		return;
	}
	const FT6FileTreeSnapshot SidecarBefore = SnapshotT6Files(SidecarPath);
	const FT6FileTreeSnapshot RuntimeBefore = SnapshotT6Files(RuntimeRoot);

	Harness.Reset(Row.FailPhase);
 Harness.Runtime.Host.bMutationProjection = Row.bMutationProjection;
 Harness.Runtime.Host.bOmitTerminalStep = Row.bOmitTerminalStep;
 Harness.Runtime.Host.bMalformedProjection = Row.bMalformedProjection;
	TUniquePtr<FT6StagingRootBlocker> StagingBlocker;
	if (Row.bFailStaging)
	{
		StagingBlocker = MakeUnique<FT6StagingRootBlocker>();
		if (!Test.TestTrue(
			TEXT("Stage blocks the production staging root"),
			StagingBlocker->Block()))
		{
			return;
		}
	}
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = SidecarPath;
	Request.bSaveAsset = true;
	const FAssetDocumentResult Result =
		Harness.Service->ApplyFile(Request);
	if (StagingBlocker
		&& !Test.TestTrue(
			TEXT("Stage restores the production staging root"),
			StagingBlocker->Restore()))
	{
		return;
	}

 if (Row.bMutationProjection)
 {
  Test.TestEqual(TEXT("prepared invoke hook count"), Harness.PreparedInvocationCount, Row.ExpectedPreparedCalls);
  Test.TestEqual(TEXT("provider validator forwards exact real invoke"), Harness.PreparedForwardCount, Row.ExpectedPreparedCalls);
  Test.TestEqual(TEXT("real provider invocation succeeds before failure"), Harness.PreparedSuccessCount, Row.ExpectedPreparedCalls);
  Test.TestEqual(TEXT("live changed property observed before rollback"), Harness.Runtime.Host.bObservedChangedProperty, Row.ExpectedPreparedCalls > 0);
  Test.TestFalse(TEXT("composition failure blocks Core Persist phase"), Harness.ObservedCorePhases().Contains(EAssetDocumentRuntimeRequestPhase::Persist));
  Test.TestFalse(TEXT("failure leaves no package on disk"), IFileManager::Get().FileExists(*PackagePath));
  if (Row.bOmitTerminalStep)
   Test.TestTrue(TEXT("unconsumed-step fixture supplied a valid successful Repair changed lane"),
    Harness.Runtime.Host.bInjectedValidRepairChangedSet);
 }
	Test.TestFalse(
		*FString::Printf(TEXT("%s returns failure"), Row.Name),
		Result.IsSuccess());
	Test.TestTrue(
		*FString::Printf(
			TEXT("%s preserves the stable failure owner (actual diagnostics: %s)"),
			Row.Name,
			*DescribeDiagnosticCodes(Result)),
		HasDiagnostic(Result, Row.ExpectedDiagnostic));
	Test.TestTrue(
		*FString::Printf(
			TEXT("%s dispatches each phase once (actual phases: %s)"),
			Row.Name,
			*DescribeObservedPhases(Harness.Runtime.Host.ObservedPhases)),
		Harness.Runtime.Host.ObservedPhases == Row.ExpectedHostPhases);
	Test.TestTrue(
		*FString::Printf(TEXT("%s dispatches at most one read-only unchanged probe"), Row.Name),
		Harness.Runtime.Host.ObservedDiffProbes <= 1);
	Test.TestEqual(
		*FString::Printf(
			TEXT("%s rollback invalidation count is exact"),
			Row.Name),
		Harness.Runtime.CountInvalidation(
			EAssetDocumentHandleInvalidationReason::Rollback),
		Row.ExpectedRollbackInvalidations);
	Test.TestEqual(
		*FString::Printf(
			TEXT("%s never commits after failure"),
			Row.Name),
		Harness.Runtime.CountInvalidation(
			EAssetDocumentHandleInvalidationReason::Commit),
		0);
	UObject* ResolvedAsset = FSoftObjectPath(ObjectPath).ResolveObject();
	Test.TestTrue(
		*FString::Printf(TEXT("%s restores the existing asset identity"), Row.Name),
		ResolvedAsset == FixtureAsset);
	Test.TestEqual(
		*FString::Printf(TEXT("%s restores the existing asset class"), Row.Name),
		ResolvedAsset != nullptr
			? ResolvedAsset->GetClass()->GetPathName()
			: FString(),
		FixtureClassPath);
	Test.TestEqual(
		*FString::Printf(TEXT("%s restores TestString baseline"), Row.Name),
		TestStringProperty->GetPropertyValue_InContainer(FixtureAsset),
		TestStringBaseline);
	Test.TestTrue(
		*FString::Printf(
			TEXT("%s restores package dirty state"),
			Row.Name),
		Package->IsDirty() == bPackageWasDirty);
	Test.TestTrue(
		*FString::Printf(TEXT("%s restores input sidecar bytes"), Row.Name),
		SidecarBefore == SnapshotT6Files(SidecarPath));
	Test.TestTrue(
		*FString::Printf(TEXT("%s leaves no runtime file residue"), Row.Name),
		RuntimeBefore == SnapshotT6Files(RuntimeRoot));
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericAllPhaseRollbackTest,
	"UEAssetDoc.GenericRuntime.AllPhaseRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericAllPhaseRollbackTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FAssetDocumentGenericRuntimeBootstrap* Bootstrap =
		GetAssetDocumentActiveGenericRuntimeBootstrapForTests();
	if (!TestNotNull(
			TEXT("production bootstrap is available for isolation"),
			Bootstrap))
	{
		return false;
	}
	if (!TestTrue(
			TEXT("production bootstrap is active before isolation"),
			Bootstrap->IsActive()))
	{
		return false;
	}
	const uint64 ProductionRevision =
		GetAssetDocumentActiveGenericRuntimeRevisionForTests();
	const int32 ProductionRuntimeCount = Bootstrap->GetActiveRuntimeCount();
	const int32 ProductionRouteBindingCount =
		Bootstrap->GetRouteBindingCount();
	Bootstrap->Deactivate();
	TestEqual(
		TEXT("test isolation releases the production runtime"),
		Bootstrap->GetActiveRuntimeCount(),
		0);
	TestEqual(
		TEXT("test isolation releases the production route"),
		Bootstrap->GetRouteBindingCount(),
		0);
	ON_SCOPE_EXIT
	{
		const bool bReactivated = Bootstrap->Activate();
		TestTrue(
			TEXT("test isolation restores the production bootstrap"),
			bReactivated);
		TestTrue(
			TEXT("restored production bootstrap is active"),
			Bootstrap->IsActive());
		TestEqual(
			TEXT("test isolation restores the production revision"),
			GetAssetDocumentActiveGenericRuntimeRevisionForTests(),
			ProductionRevision);
		TestEqual(
			TEXT("test isolation restores the production runtime"),
			Bootstrap->GetActiveRuntimeCount(),
			ProductionRuntimeCount);
		TestEqual(
			TEXT("test isolation restores the production route"),
			Bootstrap->GetRouteBindingCount(),
			ProductionRouteBindingCount);
	};
	FNormalRouteHarness Harness;
	if (!Harness.Initialize(*this))
	{
		return false;
	}
	RunEmptyApplyFilePathFailure(*this, Harness);
	const TArray<FNormalRouteFailureRow> Rows{
		{TEXT("Validate"),
			EAssetDocumentRuntimeRequestPhase::Validate,
			false,
			{EAssetDocumentRuntimeRequestPhase::Validate},
			TEXT("Test.GenericPhaseFailure"),
			0},
		{TEXT("Preflight"),
			EAssetDocumentRuntimeRequestPhase::Preflight,
			false,
			{EAssetDocumentRuntimeRequestPhase::Validate,
				EAssetDocumentRuntimeRequestPhase::Preflight},
			TEXT("Test.GenericPhaseFailure"),
			0},
		{TEXT("Apply"),
			EAssetDocumentRuntimeRequestPhase::Apply,
			false,
			{EAssetDocumentRuntimeRequestPhase::Validate,
				EAssetDocumentRuntimeRequestPhase::Preflight,
				EAssetDocumentRuntimeRequestPhase::Apply},
			TEXT("Test.GenericPhaseFailure"),
			1},
		{TEXT("Repair"),
			EAssetDocumentRuntimeRequestPhase::Repair,
			false,
			{EAssetDocumentRuntimeRequestPhase::Validate,
				EAssetDocumentRuntimeRequestPhase::Preflight,
				EAssetDocumentRuntimeRequestPhase::Apply,
				EAssetDocumentRuntimeRequestPhase::Repair},
			TEXT("Test.GenericPhaseFailure"),
			1},
		{TEXT("Stage"),
			{},
			true,
			{EAssetDocumentRuntimeRequestPhase::Validate,
				EAssetDocumentRuntimeRequestPhase::Preflight,
				EAssetDocumentRuntimeRequestPhase::Apply,
				EAssetDocumentRuntimeRequestPhase::Repair},
			TEXT("Persistence.StagePackageFailed"),
			1},
		{TEXT("Verify"),
			EAssetDocumentRuntimeRequestPhase::Verify,
			false,
			{EAssetDocumentRuntimeRequestPhase::Validate,
				EAssetDocumentRuntimeRequestPhase::Preflight,
				EAssetDocumentRuntimeRequestPhase::Apply,
				EAssetDocumentRuntimeRequestPhase::Repair,
				EAssetDocumentRuntimeRequestPhase::Verify},
			TEXT("Persistence.CanonicalMismatch"),
			1},
	};
	for (const FNormalRouteFailureRow& Row : Rows)
	{
		RunNormalRouteFailureRow(*this, Harness, Row);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
 FAssetDocumentGenericMutationPreparationRoutingTest,
 "UEAssetDoc.GenericRuntime.MutationPreparationRouting",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentGenericMutationPreparationRoutingTest::RunTest(const FString& Parameters)
{
 (void)Parameters;
 auto* Bootstrap = GetAssetDocumentActiveGenericRuntimeBootstrapForTests();
 if (!TestNotNull(TEXT("routing test production bootstrap exists"), Bootstrap)
  || !TestTrue(TEXT("routing test production bootstrap is active"), Bootstrap->IsActive())) return false;
 const uint64 ProductionRevision = GetAssetDocumentActiveGenericRuntimeRevisionForTests();
 Bootstrap->Deactivate();
 ON_SCOPE_EXIT
 {
  TestTrue(TEXT("routing test restores production bootstrap"), Bootstrap->Activate());
  TestEqual(TEXT("routing test preserves production revision"), GetAssetDocumentActiveGenericRuntimeRevisionForTests(), ProductionRevision);
 };
 using EPhase = EAssetDocumentRuntimeRequestPhase;
 const TArray<FNormalRouteFailureRow> Rows{
  {TEXT("ProjectionMissingCallback"), {}, false, {EPhase::Validate, EPhase::Preflight},
   TEXT("MutationComposition.PreparationUnavailable"), 0, true, false, 0},
  {TEXT("ProjectionUnconsumed"), {}, false, {EPhase::Validate, EPhase::Preflight, EPhase::Apply, EPhase::Repair},
   TEXT("MutationComposition.Incomplete"), 1, true, true, 1},
  {TEXT("ProjectionRepairFailure"), EPhase::Repair, false, {EPhase::Validate, EPhase::Preflight, EPhase::Apply, EPhase::Repair},
   TEXT("Test.GenericPhaseFailure"), 1, true, false, 1},
  {TEXT("MalformedProjection"), {}, false, {EPhase::Validate, EPhase::Preflight},
   TEXT("MutationIntentProjection.ClosedSchema"), 0, true, false, 0, true},
 };
 IConsoleVariable* PackageSnapshot = IConsoleManager::Get().FindConsoleVariable(TEXT("AssetDocument.CorePackageSnapshot"));
 if (!TestNotNull(TEXT("routing test finds the Core package snapshot switch"), PackageSnapshot)) return false;
 const bool bPackageSnapshotWasEnabled = PackageSnapshot->GetBool();
 ON_SCOPE_EXIT { PackageSnapshot->Set(bPackageSnapshotWasEnabled, ECVF_SetByCode); };
 for (int32 Index = 0; Index < Rows.Num(); ++Index)
 {
  // Without Core's package snapshot a binding-less provider has no inverse
  // coverage; the snapshot-covered fallback is tested by CorePackageSnapshotRollback.
  PackageSnapshot->Set(Index != 0 && bPackageSnapshotWasEnabled, ECVF_SetByCode);
  FNormalRouteHarness Harness;
  if (!Harness.Initialize(*this, true, Index != 0)) return false;
  RunNormalRouteFailureRow(*this, Harness, Rows[Index]);
 }
 return true;
}

#endif
