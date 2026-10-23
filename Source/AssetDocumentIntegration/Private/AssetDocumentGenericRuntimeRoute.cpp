// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentGenericRuntimeRoute.h"
#include "AssetDocumentDirtyTargetMessage.h"
#include "AssetDocumentAnimationRequestBinding.h"

#include "AssetDocumentBlackboardAtomicTransactionAdapter.h"
#include "AssetDocumentGenericMutationPreparation.h"
#include "AssetDocumentGenericPersistenceTargetBinder.h"
#include "AssetDocumentGenericPersistenceVerifier.h"
#include "AssetDocumentGenericReadOnlyDispatcher.h"
#include "AssetDocumentGenericRequestAdapter.h"
#include "AssetDocumentGenericValidateTrial.h"
#include "AssetDocumentGraphComponentTemplates.h"
#include "AssetDocumentEditorNonInteractiveScope.h"
#include "AssetDocumentGraphDefaultsPropagation.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "AssetDocumentEditorFloatPrecision.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "Runtime/AssetDocumentOpenEditor.h"
#include "Runtime/AssetDocumentStagedPackagePurge.h"

namespace
{
class FAnimationRequestScope
{
public:
	FAnimationRequestScope(TSharedPtr<FAssetDocumentAnimationRequestBinding> InBinding,
		const FAssetDocumentRuntimeRequestDto& Request)
	{
		Validation.bValid = true;
		// Montage and AnimSequence requests reach the Animation provider only
		// through one owner bound to the exact request and revision.
		if (InBinding && (Request.RequestedAssetClassPath == TEXT("/Script/Engine.AnimMontage")
			|| Request.RequestedAssetClassPath == TEXT("/Script/Engine.AnimSequence"))
			&& !Request.TargetAssetPath.IsEmpty())
		{
			Validation = InBinding->BindRequest(Request);
			if (Validation.bValid) Binding = MoveTemp(InBinding);
		}
	}
	~FAnimationRequestScope() { if (Binding) Binding->CloseRequest(); }
	FAssetDocumentContractValidationResult Validation;
private:
	TSharedPtr<FAssetDocumentAnimationRequestBinding> Binding;
};

FAssetDocumentRuntimeResultDto MakeRuntimeFailure(
	const FAssetDocumentRuntimeRequestDto& Request,
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentRuntimeResultDto Result;
	Result.RequestId = Request.RequestId;
	Result.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Failed;
	Result.FinalPhase = EAssetDocumentRuntimeRequestPhase::Failed;
	Result.RuntimeRevision = Request.ExpectedRuntimeRevision;
	FAssetDocumentRuntimeDiagnostic& Diagnostic =
		Result.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/GenericRuntime");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Result;
}

// Play-In-Editor refusal before anything is touched. The graph and asset-table providers
// refuse every mutating primitive while PIE runs (Graph.PieActive); a refusal in the middle of an
// apply, after other primitives already changed the package, rolled the package back while the
// play session still ran. A registered profile whose declared mutating primitives can fail with
// Graph.PieActive is refused here instead, before Preflight: no snapshot, no rollback.
bool IsAssetDocumentPlayInEditorActive()
{
	return GEditor != nullptr && (GEditor->PlayWorld != nullptr || GEditor->IsPlaySessionInProgress());
}

TOptional<FAssetDocumentRuntimeResultDto> RefuseDuringPlayInEditor(
	const FAssetDocumentRuntimeTargetActivation* Activation,
	const IAssetDocumentPrimitiveRegistry* Registry,
	const FAssetDocumentRuntimeRequestDto& Request)
{
	if (Activation == nullptr || Registry == nullptr || !IsAssetDocumentPlayInEditorActive())
	{
		return {};
	}
	for (const FAssetDocumentPrimitiveRequirement& Requirement : Activation->RequiredPrimitives)
	{
		const FAssetDocumentPrimitiveResolution Resolved = Registry->Resolve(Requirement);
		if (Resolved.bResolved && Resolved.Descriptor.Access == EAssetDocumentPrimitiveAccess::Mutating
			&& Resolved.Descriptor.StableDiagnosticCodes.Contains(TEXT("Graph.PieActive")))
		{
			FAssetDocumentRuntimeResultDto Failure = MakeRuntimeFailure(Request, TEXT("Graph.PieActive"),
				TEXT("Play-In-Editor is running: edits of this asset type are refused while a play session runs. Nothing "
					"was changed; stop the play session (query_play / the editor's Stop button) and apply again."));
			Failure.Diagnostics[0].Path = TEXT("/Target");
			return Failure;
		}
	}
	return {};
}

// A request never waits for a click. Every request runs under the editor's unattended
// mode (FAssetDocumentEditorNonInteractiveScope): a dialog that would wait for input is answered with
// its default (Cancel / No for a confirmation) and the request goes on. The default never performs
// the action the dialog asked about, so an apply whose document needed it fails verification and is
// rolled back; either way the result says that a dialog was answered.
class FUnattendedRequestScope final : public FOutputDevice
{
public:
	FUnattendedRequestScope()
	{
		GLog->AddOutputDevice(this);
	}
	virtual ~FUnattendedRequestScope() override
	{
		if (GLog != nullptr)
		{
			GLog->RemoveOutputDevice(this);
		}
	}
	FUnattendedRequestScope(const FUnattendedRequestScope&) = delete;
	FUnattendedRequestScope& operator=(const FUnattendedRequestScope&) = delete;

	// FMessageDialog::Open without an explicit default logs this (through GWarn, any category) when the
	// unattended mode answers the message box instead of showing it; the scope itself sees modal windows.
	virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type, const FName&) override
	{
		if (Message != nullptr && FCString::Strifind(Message, TEXT("Message Dialog was triggered in unattended script mode")) != nullptr)
		{
			bMessageDialogAnswered = true;
		}
	}
	virtual bool CanBeUsedOnAnyThread() const override { return true; }
	virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

	template <typename ResultType>
	void Finish(ResultType& Result) const
	{
		if (Modal.WasInteractionRefused() || bMessageDialogAnswered)
		{
			Append(Result);
		}
	}

private:
	static FAssetDocumentRuntimeDiagnostic MakeDiagnostic()
	{
		FAssetDocumentRuntimeDiagnostic Diagnostic;
		Diagnostic.Severity = EAssetDocumentDiagnosticSeverity::Warning;
		Diagnostic.Path = TEXT("/");
		Diagnostic.Code = TEXT("GenericRuntime.EditorDialogAnswered");
		Diagnostic.Message = TEXT("The editor opened a dialog that waits for a click while this request ran. An MCP request "
			"cannot wait for one, so it was answered with its default (Cancel / No for a confirmation) and the request "
			"continued. If the result is not what the document states, make that change in the editor.");
		return Diagnostic;
	}
	static void Append(FAssetDocumentResult& Result)
	{
		Result.Diagnostics.Add(ProjectAssetDocumentRuntimeDiagnostic(MakeDiagnostic()));
	}
	static void Append(FAssetDocumentRuntimeResultDto& Result)
	{
		Result.Diagnostics.Add(MakeDiagnostic());
	}

	FAssetDocumentEditorNonInteractiveScope Modal;
	std::atomic<bool> bMessageDialogAnswered{false};
};

// A JSON number for a float property is the nearest float32 the editor stores
// (as the Details panel does): the authored document is rewritten to that value's
// shortest decimal before Validate, so the canonical bytes, the semantic hash and
// every later phase (no-op Diff, Preflight, Apply, Verify) agree at the stored
// precision. Doubles are left as written.
void NormalizeAuthoredFloatPrecision(FAssetDocumentRuntimeRequestDto& Request)
{
	UE::AssetDocument::EditorFloatPrecision::NormalizeDocumentProperties(Request.CanonicalDocument);
}

// "MaterialInstanceConstant document": the messages name the document's class (its profile).
FString DocumentNoun(const FString& AssetClassPath)
{
	FString Name = AssetClassPath;
	int32 Dot = INDEX_NONE;
	if (Name.FindLastChar(TEXT('.'), Dot))
	{
		Name.RightChopInline(Dot + 1);
	}
	return Name.IsEmpty() ? FString(TEXT("AssetDocument")) : Name + TEXT(" document");
}

FAssetDocumentResult ProjectRuntimeResult(
	const FAssetDocumentRuntimeResultDto& Runtime,
	const FString& SuccessMessage = TEXT("Generic AssetDocument applied"))
{
	const bool bSucceeded = Runtime.TerminalStatus
		== EAssetDocumentRuntimeTerminalStatus::Succeeded;
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	for (const FAssetDocumentRuntimeDiagnostic& Source : Runtime.Diagnostics)
	{
		Diagnostics.Add(ProjectAssetDocumentRuntimeDiagnostic(Source));
	}
	FAssetDocumentResult Result = bSucceeded
		? FAssetDocumentResult::Success(SuccessMessage)
		: FAssetDocumentResult::FailureFromDiagnostics(
			Diagnostics, TEXT("Generic AssetDocument runtime failed"));
	if (bSucceeded)
	{
		Result.Diagnostics = MoveTemp(Diagnostics);
	}
	return Result;
}

// Safety net: after any apply to a Blueprint (successful, failed or a no-op), no loaded
// Blueprint may keep a component template of a class a compile replaced; stale ones are
// reinstanced and every finding is a warning on the result (never silent).
void AppendComponentTemplateCheck(const FString& TargetAssetPath, FAssetDocumentResult& Result)
{
	if (Cast<UBlueprint>(FSoftObjectPath(TargetAssetPath).ResolveObject()) == nullptr)
	{
		return;
	}
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	UE::AssetDocument::Graph::CheckComponentTemplates(Diagnostics);
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Diagnostics)
	{
		Result.Diagnostics.Add(ProjectAssetDocumentRuntimeDiagnostic(Diagnostic));
	}
}

bool IsExactBlackboardApply(const FAssetDocumentRuntimeRequestDto& Request)
{
	return Request.Operation == EAssetDocumentRuntimeOperation::Apply
		&& Request.RequestedAssetClassPath
			== TEXT("/Script/AIModule.BlackboardData");
}

const FAssetDocumentRuntimeValueDto* FindRuntimeObjectField(
	const FAssetDocumentRuntimeValueDto& Object,
	FStringView Key)
{
	if (Object.Kind != EAssetDocumentRuntimeValueKind::Object
		|| Object.ObjectKeys.Num() != Object.Elements.Num())
	{
		return nullptr;
	}
	for (int32 Index = 0; Index < Object.ObjectKeys.Num(); ++Index)
	{
		if (FStringView(Object.ObjectKeys[Index]) == Key)
		{
			return &Object.Elements[Index];
		}
	}
	return nullptr;
}

bool IsExistingTargetAction(const FAssetDocumentRuntimeRequestDto& Request)
{
	const FAssetDocumentRuntimeValueDto* Action =
		FindRuntimeObjectField(Request.CanonicalDocument, TEXT("Action"));
	return Action != nullptr
		&& Action->Kind == EAssetDocumentRuntimeValueKind::String
		&& (Action->StringValue == TEXT("Update")
			|| Action->StringValue == TEXT("CreateOrUpdate"));
}

// A saved, clean, exact-class target is the only state a no-op may leave alone.
bool IsCleanSavedTarget(const FAssetDocumentRuntimeRequestDto& Request)
{
	const UObject* const Live =
		FSoftObjectPath(Request.TargetAssetPath).ResolveObject();
	const UPackage* const Package = Live ? Live->GetOutermost() : nullptr;
	return Package != nullptr
		&& Live->GetOuter() == Package
		&& !Package->IsDirty()
		&& Live->GetClass()->GetPathName() == Request.RequestedAssetClassPath
		&& FPackageName::DoesPackageExist(Package->GetName());
}

bool IsDirtyLoadedTarget(const FAssetDocumentRuntimeRequestDto& Request)
{
	const UObject* const Live =
		FSoftObjectPath(Request.TargetAssetPath).ResolveObject();
	return Live != nullptr && Live->GetOutermost()->IsDirty();
}

// Every route publishes its Diff receipt with the same buckets; only an
// all-empty receipt (nothing changed, added, removed, skipped or failed) is a no-op.
bool IsEmptyDiffReceipt(const FAssetDocumentRuntimeResultDto& Diff)
{
	if (Diff.TerminalStatus != EAssetDocumentRuntimeTerminalStatus::Succeeded
		|| !Diff.CanonicalValue.IsSet())
	{
		return false;
	}
	const FAssetDocumentRuntimeValueDto& Receipt = Diff.CanonicalValue.GetValue();
	const FAssetDocumentRuntimeValueDto* Changed = FindRuntimeObjectField(Receipt, TEXT("changed"));
	if (Changed == nullptr)
	{
		return false;
	}
	for (const TCHAR* Bucket : {TEXT("changed"), TEXT("added"), TEXT("removed"), TEXT("skipped"), TEXT("failed")})
	{
		const FAssetDocumentRuntimeValueDto* Value = FindRuntimeObjectField(Receipt, Bucket);
		if (Value != nullptr
			&& (Value->Kind != EAssetDocumentRuntimeValueKind::Array || !Value->Elements.IsEmpty()))
		{
			return false;
		}
	}
	return true;
}

class FCachedPreflightHost final : public IAssetDocumentPythonRuntimeHost
{
public:
	FCachedPreflightHost(
		IAssetDocumentPythonRuntimeHost& InInner,
		FAssetDocumentRuntimeResultDto InValidated,
		FAssetDocumentRuntimeResultDto InPreflight)
		: Inner(InInner)
		, Validated(MoveTemp(InValidated))
		, Preflight(MoveTemp(InPreflight))
	{
	}

	FAssetDocumentRuntimeResultDto Dispatch(
		const FAssetDocumentRuntimeRequestDto& Request,
		IAssetDocumentNativeRequestContext& Context) override
	{
		if (Context.GetMetadata().Phase
			== EAssetDocumentRuntimeRequestPhase::Validate)
		{
			if (!bValidateConsumed && Request.RequestId == Validated.RequestId)
			{
				bValidateConsumed = true;
				return Validated;
			}
			return MakeRuntimeFailure(
				Request,
				TEXT("GenericRuntime.DuplicateValidate"),
				TEXT("The cached Validate result may be consumed exactly once."));
		}
		if (Context.GetMetadata().Phase
			== EAssetDocumentRuntimeRequestPhase::Preflight)
		{
			if (!bPreflightConsumed && Request.RequestId == Preflight.RequestId)
			{
				bPreflightConsumed = true;
				return Preflight;
			}
			return MakeRuntimeFailure(
				Request,
				TEXT("GenericRuntime.DuplicatePreflight"),
				TEXT("The cached Preflight result may be consumed exactly once."));
		}
		return Inner.Dispatch(Request, Context);
	}

private:
	IAssetDocumentPythonRuntimeHost& Inner;
	FAssetDocumentRuntimeResultDto Validated;
	FAssetDocumentRuntimeResultDto Preflight;
	bool bValidateConsumed = false;
	bool bPreflightConsumed = false;
};
}

namespace AssetDocumentGenericRuntimeRouteLocal
{
// A Profile that checks while it edits: its Preflight is covered by Core's package snapshot (SnapshotCoveredMutationV1).
bool IsSnapshotCoveredPreflight(const FAssetDocumentRuntimeResultDto& Preflight)
{
	const FAssetDocumentRuntimeValueDto* Kind = Preflight.CanonicalValue.IsSet()
		? FindRuntimeObjectField(Preflight.CanonicalValue.GetValue(), TEXT("Kind")) : nullptr;
	return Kind != nullptr && Kind->Kind == EAssetDocumentRuntimeValueKind::String
		&& Kind->StringValue == TEXT("SnapshotCoveredMutationV1");
}
}

FAssetDocumentGenericRuntimeRoute::FAssetDocumentGenericRuntimeRoute() = default;

FAssetDocumentGenericRuntimeRoute::FAssetDocumentGenericRuntimeRoute(
	IAssetDocumentPythonRuntime& InRuntime)
	: Runtime(&InRuntime)
{
}

FAssetDocumentGenericRuntimeRoute::~FAssetDocumentGenericRuntimeRoute() = default;

FAssetDocumentContractValidationResult FAssetDocumentGenericRuntimeRoute::BindRuntimeTargetActivations(
	TArray<FAssetDocumentRuntimeTargetActivation>&& Activations)
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = false;
	auto Reject = [&Result]()
	{
		auto& D = Result.Diagnostics.AddDefaulted_GetRef();
		D.Path = TEXT("/GenericRuntime"); D.Code = TEXT("GenericRuntime.TargetActivationInvalid");
		D.Message = TEXT("Runtime target binding requires one exact immutable active registration set before composition.");
		return Result;
	};
	if (!Runtime || bTargetActivationsBound || bCompositionEverBound) return Reject();
	const auto Projection = Runtime->GetActiveRuntimeRegistrationProjection();
	if (Projection.Kind != EAssetDocumentRuntimeValueKind::Array || Projection.Elements.Num() != Activations.Num()) return Reject();
	TMap<FString, FAssetDocumentRuntimeTargetActivation> Parsed;
	for (const auto& Row : Projection.Elements)
	{
		auto Activation = ParseAndValidateRuntimeTargetActivation(Row, GetAssetDocumentEditorTargetLifecycleCatalog(), Result.Diagnostics);
		if (!Activation.IsSet() || Parsed.Contains(Activation->ExactAssetClassPath)) return Reject();
		Parsed.Add(Activation->ExactAssetClassPath, MoveTemp(Activation.GetValue()));
	}
	TSet<FString> Supplied;
	const IAssetDocumentPrimitiveRegistry* Registry = Runtime->GetActivePrimitiveRegistry();
	for (const auto& Activation : Activations)
	{
		const auto* Expected = Parsed.Find(Activation.ExactAssetClassPath);
		if (!Expected || Supplied.Contains(Activation.ExactAssetClassPath)
			|| !AreAssetDocumentRuntimeTargetActivationsEqual(*Expected, Activation) || !Registry) return Reject();
		for (const auto& Requirement : Activation.RequiredPrimitives)
			if (!Registry->Resolve(Requirement).bResolved) return Reject();
		Supplied.Add(Activation.ExactAssetClassPath);
	}
	TargetActivations = MoveTemp(Parsed);
	RegistrationProjection = Projection;
	ActivationRevision = Runtime->GetActiveRuntimeRevision();
	bTargetActivationsBound = true;
	Result.bValid = true;
	return Result;
}

FAssetDocumentRuntimeValueDto FAssetDocumentGenericRuntimeRoute::DispatchDescribeRegisteredProfiles() const
{
	if (bTargetActivationsBound) return RegistrationProjection;
	FAssetDocumentRuntimeValueDto Empty;
	Empty.Kind = EAssetDocumentRuntimeValueKind::Array;
	return Empty;
}

void FAssetDocumentGenericRuntimeRoute::BindMutationPreparationProjection(
 TSharedPtr<const FAssetDocumentRequiredPrimitiveProjection> Projection)
{
 check(!bCompositionEverBound && !PreparationProjection); PreparationProjection = MoveTemp(Projection);
}
void FAssetDocumentGenericRuntimeRoute::BindAnimationRequestProvider(TSharedPtr<FAssetDocumentAnimationRequestBinding> Binding,
	TSharedPtr<FAssetDocumentStagedMontageReadBinding> StagedReadBinding)
{
	check(!bCompositionEverBound && !AnimationRequestBinding);
	AnimationRequestBinding = MoveTemp(Binding);
	StagedMontageReadBinding = MoveTemp(StagedReadBinding);
}
void FAssetDocumentGenericRuntimeRoute::BindBehaviorTreeStagedReadProvider(TSharedPtr<FAssetDocumentStagedBehaviorTreeReadBinding> Binding)
{
 check(!bCompositionEverBound && !BehaviorTreeStagedReadBinding);
 BehaviorTreeStagedReadBinding = MoveTemp(Binding);
}
FAssetDocumentPrimitiveOutcome FAssetDocumentGenericRuntimeRoute::InvokePreparedMutation(
 const FString& ProviderId, const FAssetDocumentPrimitiveInvocation& Invocation,
 IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction* Transaction,
 TFunctionRef<FAssetDocumentPrimitiveOutcome()> Forward)
{
 return ActiveMutationComposition ? ActiveMutationComposition->Invoke(ProviderId, Invocation, Handles, Transaction, Forward) : Forward();
}

void FAssetDocumentGenericRuntimeRoute::BindComposition(
	FAssetDocumentCoreRuntimeComposition& InComposition)
{
	bCompositionEverBound = true;
	Composition = &InComposition;
}

void FAssetDocumentGenericRuntimeRoute::UnbindComposition()
{
	Composition = nullptr;
	ActiveRequestHost = nullptr;
	ActiveTargetDecision.Reset();
	ActiveMutationComposition.Reset();
	ActiveBlackboardAdapter.Reset();
	ActiveBlackboardProjection.Reset();
	ActiveBlackboardCreatePrefix.Reset();
}

FAssetDocumentResult FAssetDocumentGenericRuntimeRoute::DispatchApplyFileImpl(
	const FAssetDocumentApplyFileRequest& Request)
{
	const UE::AssetDocument::StagedPackages::FScopedRequest StagedCopies;
	if (Runtime == nullptr || Composition == nullptr)
	{
		FAssetDocumentRuntimeRequestDto Inactive;
		return ProjectRuntimeResult(MakeRuntimeFailure(
			Inactive,
			TEXT("GenericRuntime.Inactive"),
			TEXT("Generic AssetDocument runtime is inactive.")));
	}
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	TOptional<FAssetDocumentRuntimeRequestDto> Adapted =
		FAssetDocumentGenericRequestAdapter::AdaptApplyFile(
			Request,
			Runtime->GetActiveRuntimeRevision(),
			Diagnostics);
	if (!Adapted.IsSet())
	{
		FAssetDocumentRuntimeResultDto Failure = MakeRuntimeFailure(
			{},
			TEXT("GenericRuntime.RequestAdaptationFailed"),
			TEXT("The Service request could not be adapted."));
		Failure.Diagnostics.Insert(Diagnostics, 0);
		return ProjectRuntimeResult(Failure);
	}
	NormalizeAuthoredFloatPrecision(Adapted.GetValue());
	FAnimationRequestScope AnimationScope(AnimationRequestBinding, Adapted.GetValue());
	if (!AnimationScope.Validation.bValid)
	{
		auto Failure = MakeRuntimeFailure(Adapted.GetValue(), TEXT("GenericRuntime.AnimationRequestBindingFailed"), TEXT("Animation could not bind this Service request."));
		Failure.Diagnostics.Append(AnimationScope.Validation.Diagnostics);
		return ProjectRuntimeResult(Failure);
	}
	const FAssetDocumentRuntimeTargetActivation* TargetActivation = TargetActivations.Find(Adapted->RequestedAssetClassPath);
	if ((bTargetActivationsBound && Runtime->GetActiveRuntimeRevision() != ActivationRevision)
		|| (!bTargetActivationsBound && !Runtime->GetActiveRuntimeRegistrationProjection().Elements.IsEmpty()))
	{
		return ProjectRuntimeResult(MakeRuntimeFailure(Adapted.GetValue(), TEXT("GenericRuntime.TargetActivationDrift"),
			TEXT("The runtime registration revision is not bound to this route.")));
	}
	FAssetDocumentRuntimeResultDto Validated =
		FAssetDocumentGenericReadOnlyDispatcher::DispatchValidate(
			*Runtime,
			Adapted.GetValue());
	if (Validated.TerminalStatus
			!= EAssetDocumentRuntimeTerminalStatus::Succeeded
		|| !Validated.CanonicalValue.IsSet())
	{
		return ProjectRuntimeResult(Validated);
	}
	Adapted->CanonicalDocument = Validated.CanonicalValue.GetValue();
	// An Update whose read-only Diff against the saved, clean target is empty is
	// a no-op on every route: it opens no transaction, enlists nothing, saves no
	// package and rewrites no sidecar. It runs before Preflight so that request-
	// local Preflight relays stay bound to the mutation path.
	if (Request.bSaveAsset
		&& IsExistingTargetAction(Adapted.GetValue())
		&& !IsDirtyLoadedTarget(Adapted.GetValue()))
	{
		FAssetDocumentRuntimeRequestDto DiffRequest = Adapted.GetValue();
		DiffRequest.Operation = EAssetDocumentRuntimeOperation::Diff;
		const FAssetDocumentRuntimeResultDto Diff =
			FAssetDocumentGenericReadOnlyDispatcher::DispatchPhase(
				*Runtime,
				DiffRequest,
				EAssetDocumentRuntimeRequestPhase::Verify);
		if (IsEmptyDiffReceipt(Diff) && IsCleanSavedTarget(Adapted.GetValue()))
		{
			FAssetDocumentResult Unchanged = FAssetDocumentResult::Success(
				DocumentNoun(Adapted->RequestedAssetClassPath) + TEXT(" unchanged; nothing to apply"));
			Unchanged.Target = FPackageName::ObjectPathToPackageName(Adapted->TargetAssetPath);
			Unchanged.AssetPath = Adapted->TargetAssetPath;
			Unchanged.SidecarFilePath = Request.FilePath;
			AppendComponentTemplateCheck(Adapted->TargetAssetPath, Unchanged);
			return Unchanged;
		}
	}
	if (TOptional<FAssetDocumentRuntimeResultDto> Refused = RefuseDuringPlayInEditor(
			TargetActivation, Runtime->GetActivePrimitiveRegistry(), Adapted.GetValue()))
	{
		return ProjectRuntimeResult(Refused.GetValue());
	}
	TOptional<FAssetDocumentPreparedTargetDecision> PreflightTargetDecision;
	if (TargetActivation)
	{
		PreflightTargetDecision = PrepareAssetDocumentRuntimeTargetDecision(*TargetActivation, Adapted.GetValue(), Diagnostics);
		if (!PreflightTargetDecision.IsSet())
		{
			auto Failure = MakeRuntimeFailure(Adapted.GetValue(), TEXT("GenericRuntime.MutationPreparationFailed"),
				TEXT("Registered target policy rejected the Preflight decision."));
			Failure.Diagnostics.Append(Diagnostics); return ProjectRuntimeResult(Failure);
		}
	}
	FAssetDocumentRuntimeResultDto Preflight =
		FAssetDocumentGenericReadOnlyDispatcher::DispatchPhase(
			*Runtime,
			Adapted.GetValue(),
			EAssetDocumentRuntimeRequestPhase::Preflight);
	if (Preflight.TerminalStatus
		!= EAssetDocumentRuntimeTerminalStatus::Succeeded
		|| (IsExactBlackboardApply(Adapted.GetValue())
			&& !Preflight.CanonicalValue.IsSet()))
	{
		return ProjectRuntimeResult(Preflight);
	}
	if (PreflightTargetDecision.IsSet()
		&& !ValidateAssetDocumentPreparedTargetDecision(PreflightTargetDecision.GetValue(), Adapted.GetValue(), Diagnostics))
	{
		return ProjectRuntimeResult(MakeRuntimeFailure(Adapted.GetValue(), TEXT("GenericRuntime.TargetActivationDrift"),
			TEXT("Preflight target existence changed before native preparation.")));
	}
	if (!Request.bSaveAsset)
	{
		return ProjectRuntimeResult(Preflight);
	}
	// The document passed validation, the no-op check and preflight: the apply changes the asset now. A close/reopen
	// editor open on it is closed only here (AssetDocumentOpenEditor.h). Such a profile checks while it edits
	// (snapshot-covered Preflight), so Validate's trial apply on a transient copy runs first: a document error it finds
	// fails the apply with the editor still open and untouched.
	if (UE::AssetDocument::OpenEditor::IsClosePending() && TargetActivation
		&& AssetDocumentGenericRuntimeRouteLocal::IsSnapshotCoveredPreflight(Preflight))
	{
		const FAssetDocumentRuntimeResultDto Trial = RunAssetDocumentValidateTrial(*Runtime, Adapted.GetValue());
		if (Trial.TerminalStatus != EAssetDocumentRuntimeTerminalStatus::Succeeded)
		{
			return ProjectRuntimeResult(Trial);
		}
	}
	UE::AssetDocument::OpenEditor::BeforeChange();
	FAssetDocumentGenericPersistenceVerifier Verifier(
		*Runtime,
		Adapted.GetValue(), AnimationRequestBinding, StagedMontageReadBinding, BehaviorTreeStagedReadBinding);
	FAssetDocumentGenericMutationPreparationResult Prepared =
		FAssetDocumentGenericMutationPreparation::Prepare(
			Request,
			Adapted.GetValue(),
			Validated,
			Preflight,
			Verifier,
			Diagnostics,
			TargetActivation, Runtime->GetActivePrimitiveRegistry(), PreparationProjection.Get());
	if (!Prepared)
	{
		FAssetDocumentRuntimeResultDto Failure = MakeRuntimeFailure(
			Adapted.GetValue(),
			TEXT("GenericRuntime.MutationPreparationFailed"),
			TEXT("Generic mutation preparation failed."));
		Failure.Diagnostics.Append(Diagnostics);
		return ProjectRuntimeResult(Failure);
	}
	Prepared.Mutation->BindPersistenceTarget = FAssetDocumentGenericPersistenceTargetBinder::Bind;
	ActiveTargetDecision = MoveTemp(Prepared.TargetDecision);
 ActiveMutationComposition = MoveTemp(Prepared.MutationComposition);
	ActiveBlackboardProjection = MoveTemp(Prepared.BlackboardProjection);
	ActiveBlackboardCreatePrefix = MoveTemp(Prepared.OptionalBlackboardCreatePrefix);
	FCachedPreflightHost CachedHost(
		Runtime->GetHost(), MoveTemp(Validated), MoveTemp(Preflight));
	ActiveRequestHost = &CachedHost;
	ON_SCOPE_EXIT
	{
		ActiveRequestHost = nullptr;
  ActiveMutationComposition.Reset();
		ActiveTargetDecision.Reset();
		ActiveBlackboardAdapter.Reset();
		ActiveBlackboardProjection.Reset();
		ActiveBlackboardCreatePrefix.Reset();
	};
	// The values the Blueprint's dependents inherit, before the apply changes them.
	const TOptional<UE::AssetDocument::Graph::FBlueprintDefaultsSnapshot> InheritedDefaults =
		UE::AssetDocument::Graph::FBlueprintDefaultsSnapshot::Capture(Adapted->TargetAssetPath);
	FAssetDocumentRuntimeResultDto Executed = Composition->ExecuteMutation(Adapted.GetValue(), MoveTemp(*Prepared.Mutation));
	if (IsExactBlackboardApply(Adapted.GetValue())
		&& Executed.TerminalStatus == EAssetDocumentRuntimeTerminalStatus::Succeeded)
	{
		if (!ActiveBlackboardAdapter.IsValid())
		{
			Executed = MakeRuntimeFailure(
				Adapted.GetValue(),
				TEXT("GenericRuntime.BlackboardAdapterMissing"),
				TEXT("Blackboard Apply completed without the request-local transaction adapter."));
		}
		else
		{
			const FAssetDocumentContractValidationResult Complete =
				ActiveBlackboardAdapter->ValidateCompleteEnlistment();
			if (!Complete.bValid)
			{
				Executed = MakeRuntimeFailure(
					Adapted.GetValue(),
					TEXT("GenericRuntime.BlackboardEnlistmentIncomplete"),
					TEXT("Blackboard Apply completed without the complete projected enlistment."));
				Executed.Diagnostics.Append(Complete.Diagnostics);
			}
		}
	}
	FAssetDocumentResult Result = ProjectRuntimeResult(Executed,
		DocumentNoun(Adapted->RequestedAssetClassPath) + TEXT(" applied"));
	Result.Target =
		FPackageName::ObjectPathToPackageName(Adapted->TargetAssetPath);
	Result.AssetPath = Adapted->TargetAssetPath;
	Result.SidecarFilePath = Request.FilePath;
	Result.bSavedAsset = Result.IsSuccess() && Request.bSaveAsset;
	Result.bWroteSidecar = Result.IsSuccess() && Request.bSaveAsset;
	AppendComponentTemplateCheck(Adapted->TargetAssetPath, Result);
	if (Result.IsSuccess() && InheritedDefaults.IsSet())
	{
		TArray<FAssetDocumentRuntimeDiagnostic> Propagated;
		InheritedDefaults->Propagate(Propagated);
		for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Propagated)
		{
			Result.Diagnostics.Add(ProjectAssetDocumentRuntimeDiagnostic(Diagnostic));
		}
	}
	return Result;
}

// Validate runs every check Apply makes before it mutates anything, with the same
// diagnostics: the document (Validate phase), the registered target decision, the Profile's
// Preflight on a scratch copy (property names and kinds, value decoding, reference and
// RowStruct resolution, create context), the native target checks (Create on an
// existing target, Update on a missing one, unsaved changes on the target) and, for Profiles
// that check while they edit, the edit itself on a discarded transient copy (trial apply).
// Nothing is created, written or saved. The success payload says whether Apply would create
// the target.
FAssetDocumentResult FAssetDocumentGenericRuntimeRoute::DispatchValidateImpl(
	const FAssetDocumentValidateRequest& Request)
{
	if (Runtime == nullptr)
	{
		FAssetDocumentRuntimeRequestDto Inactive;
		return ProjectRuntimeResult(MakeRuntimeFailure(
			Inactive,
			TEXT("GenericRuntime.Inactive"),
			TEXT("Generic AssetDocument runtime is inactive.")));
	}
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	TOptional<FAssetDocumentRuntimeRequestDto> Adapted =
		FAssetDocumentGenericRequestAdapter::AdaptValidate(
			Request,
			Runtime->GetActiveRuntimeRevision(),
			Diagnostics);
	if (!Adapted.IsSet())
	{
		FAssetDocumentRuntimeResultDto Failure = MakeRuntimeFailure(
			{},
			TEXT("GenericRuntime.RequestAdaptationFailed"),
			TEXT("The Service request could not be adapted."));
		Failure.Diagnostics.Insert(Diagnostics, 0);
		return ProjectRuntimeResult(Failure);
	}
	// The same request Apply prepares; it stops after Preflight.
	Adapted->Operation = EAssetDocumentRuntimeOperation::Apply;
	NormalizeAuthoredFloatPrecision(Adapted.GetValue());
	FAnimationRequestScope AnimationScope(AnimationRequestBinding, Adapted.GetValue());
	if (!AnimationScope.Validation.bValid)
	{
		auto Failure = MakeRuntimeFailure(Adapted.GetValue(), TEXT("GenericRuntime.AnimationRequestBindingFailed"), TEXT("Animation could not bind this Service request."));
		Failure.Diagnostics.Append(AnimationScope.Validation.Diagnostics);
		return ProjectRuntimeResult(Failure);
	}
	const FAssetDocumentRuntimeTargetActivation* TargetActivation = TargetActivations.Find(Adapted->RequestedAssetClassPath);
	if ((bTargetActivationsBound && Runtime->GetActiveRuntimeRevision() != ActivationRevision)
		|| (!bTargetActivationsBound && !Runtime->GetActiveRuntimeRegistrationProjection().Elements.IsEmpty()))
	{
		return ProjectRuntimeResult(MakeRuntimeFailure(Adapted.GetValue(), TEXT("GenericRuntime.TargetActivationDrift"),
			TEXT("The runtime registration revision is not bound to this route.")));
	}
	const FAssetDocumentRuntimeResultDto Validated =
		FAssetDocumentGenericReadOnlyDispatcher::DispatchValidate(*Runtime, Adapted.GetValue());
	if (Validated.TerminalStatus != EAssetDocumentRuntimeTerminalStatus::Succeeded
		|| !Validated.CanonicalValue.IsSet())
	{
		return ProjectRuntimeResult(Validated);
	}
	Adapted->CanonicalDocument = Validated.CanonicalValue.GetValue();
	// Apply refuses this before it touches anything; so does Validate.
	if (TOptional<FAssetDocumentRuntimeResultDto> Refused = RefuseDuringPlayInEditor(
			TargetActivation, Runtime->GetActivePrimitiveRegistry(), Adapted.GetValue()))
	{
		return ProjectRuntimeResult(Refused.GetValue());
	}
	if (TargetActivation
		&& !PrepareAssetDocumentRuntimeTargetDecision(*TargetActivation, Adapted.GetValue(), Diagnostics).IsSet())
	{
		auto Failure = MakeRuntimeFailure(Adapted.GetValue(), TEXT("GenericRuntime.MutationPreparationFailed"),
			TEXT("Registered target policy rejected the Preflight decision."));
		Failure.Diagnostics.Append(Diagnostics);
		return ProjectRuntimeResult(Failure);
	}
	const FAssetDocumentRuntimeResultDto Preflight =
		FAssetDocumentGenericReadOnlyDispatcher::DispatchPhase(
			*Runtime, Adapted.GetValue(), EAssetDocumentRuntimeRequestPhase::Preflight);
	if (Preflight.TerminalStatus != EAssetDocumentRuntimeTerminalStatus::Succeeded)
	{
		return ProjectRuntimeResult(Preflight);
	}

	const FAssetDocumentRuntimeValueDto* Action = FindRuntimeObjectField(Adapted->CanonicalDocument, TEXT("Action"));
	const FString ActionName = Action && Action->Kind == EAssetDocumentRuntimeValueKind::String ? Action->StringValue : FString();
	const bool bExists = IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(Adapted->TargetAssetPath)).IsValid();
	const bool bWouldCreate = !bExists && ActionName != TEXT("Update");
	auto TargetFailure = [&Adapted](const TCHAR* Code, const FString& Message)
	{
		FAssetDocumentRuntimeResultDto Failure = MakeRuntimeFailure(Adapted.GetValue(), Code, *Message);
		Failure.Diagnostics[0].Path = TEXT("/Target");
		return ProjectRuntimeResult(Failure);
	};
	// The unregistered route's native preparation rejects these at Apply (registered
	// Profiles already rejected them in the target decision above).
	if (!TargetActivation && ActionName == TEXT("Create") && bExists)
	{
		return TargetFailure(TEXT("CutoverPreparation.TargetExists"), FString::Printf(
			TEXT("Create requires an absent target, but '%s' already exists (loaded, registered or on disk); use Action Update or CreateOrUpdate."),
			*Adapted->TargetAssetPath));
	}
	if (!TargetActivation && ActionName == TEXT("Update") && !bExists)
	{
		return TargetFailure(TEXT("CutoverPreparation.InvalidTarget"), FString::Printf(
			TEXT("Update requires the existing target '%s', which does not exist; use Action Create or CreateOrUpdate."),
			*Adapted->TargetAssetPath));
	}
	if (bExists && IsDirtyLoadedTarget(Adapted.GetValue()))
	{
		return TargetFailure(TEXT("DirtyExistingAssetTransactionUnsupported"),
			DescribeAssetDocumentDirtyTarget(Adapted->TargetAssetPath));
	}

	// Profiles whose checks run while they edit (snapshot-covered Preflight): run that edit on a
	// transient copy and discard it (AssetDocumentGenericValidateTrial.h).
	TArray<FAssetDocumentRuntimeDiagnostic> TrialDiagnostics;
	if (TargetActivation && AssetDocumentGenericRuntimeRouteLocal::IsSnapshotCoveredPreflight(Preflight))
	{
		FAssetDocumentRuntimeResultDto Trial = RunAssetDocumentValidateTrial(*Runtime, Adapted.GetValue());
		if (Trial.TerminalStatus != EAssetDocumentRuntimeTerminalStatus::Succeeded)
		{
			return ProjectRuntimeResult(Trial);
		}
		TrialDiagnostics = MoveTemp(Trial.Diagnostics);
	}

	FAssetDocumentResult Result = FAssetDocumentResult::Success(TEXT("AssetDocument valid"));
	Result.Diagnostics = ProjectRuntimeResult(Preflight).Diagnostics;
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : TrialDiagnostics)
	{
		Result.Diagnostics.Add(ProjectAssetDocumentRuntimeDiagnostic(Diagnostic));
	}
	Result.Target = FPackageName::ObjectPathToPackageName(Adapted->TargetAssetPath);
	Result.AssetPath = Adapted->TargetAssetPath;
	Result.Payload = MakeShared<FJsonObject>();
	Result.Payload->SetBoolField(TEXT("valid"), true);
	Result.Payload->SetBoolField(TEXT("would_create"), bWouldCreate);
	Result.bSavedAsset = false;
	Result.bWroteSidecar = false;
	return Result;
}

FAssetDocumentRuntimeResultDto FAssetDocumentGenericRuntimeRoute::DispatchExtractImpl(
	const FAssetDocumentExtractRequest& Request,
	const FString& AssetClassPath)
{
	if (Runtime == nullptr)
	{
		FAssetDocumentRuntimeRequestDto Inactive;
		return MakeRuntimeFailure(
			Inactive,
			TEXT("GenericRuntime.Inactive"),
			TEXT("Generic AssetDocument runtime is inactive."));
	}
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	TOptional<FAssetDocumentRuntimeRequestDto> Adapted =
		FAssetDocumentGenericRequestAdapter::AdaptExtract(
			Request,
			AssetClassPath,
			Runtime->GetActiveRuntimeRevision(),
			Diagnostics);
	if (!Adapted.IsSet())
	{
		FAssetDocumentRuntimeResultDto Failure = MakeRuntimeFailure(
			{},
			TEXT("GenericRuntime.RequestAdaptationFailed"),
			TEXT("The Service request could not be adapted."));
		Failure.Diagnostics.Insert(Diagnostics, 0);
		return Failure;
	}
	FAnimationRequestScope AnimationScope(AnimationRequestBinding, Adapted.GetValue());
	if (!AnimationScope.Validation.bValid)
	{
	 auto Failure = MakeRuntimeFailure(Adapted.GetValue(), TEXT("GenericRuntime.AnimationRequestBindingFailed"), TEXT("Animation could not bind this Service request."));
	 Failure.Diagnostics.Append(AnimationScope.Validation.Diagnostics);
	 return Failure;
	}
	return FAssetDocumentGenericReadOnlyDispatcher::DispatchPhase(
			*Runtime,
			Adapted.GetValue(),
			EAssetDocumentRuntimeRequestPhase::Verify);
}

FAssetDocumentRuntimeResultDto FAssetDocumentGenericRuntimeRoute::DispatchDiffImpl(
	const FAssetDocumentDiffRequest& Request)
{
	if (Runtime == nullptr)
	{
		FAssetDocumentRuntimeRequestDto Inactive;
		return MakeRuntimeFailure(
			Inactive,
			TEXT("GenericRuntime.Inactive"),
			TEXT("Generic AssetDocument runtime is inactive."));
	}
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	TOptional<FAssetDocumentRuntimeRequestDto> Adapted =
		FAssetDocumentGenericRequestAdapter::AdaptDiff(
			Request,
			Runtime->GetActiveRuntimeRevision(),
			Diagnostics);
	if (!Adapted.IsSet())
	{
		FAssetDocumentRuntimeResultDto Failure = MakeRuntimeFailure(
			{},
			TEXT("GenericRuntime.RequestAdaptationFailed"),
			TEXT("The Service request could not be adapted."));
		Failure.Diagnostics.Insert(Diagnostics, 0);
		return Failure;
	}
	NormalizeAuthoredFloatPrecision(Adapted.GetValue());
	FAnimationRequestScope AnimationScope(AnimationRequestBinding, Adapted.GetValue());
	if (!AnimationScope.Validation.bValid)
	{
	 auto Failure = MakeRuntimeFailure(Adapted.GetValue(), TEXT("GenericRuntime.AnimationRequestBindingFailed"), TEXT("Animation could not bind this Service request."));
	 Failure.Diagnostics.Append(AnimationScope.Validation.Diagnostics);
	 return Failure;
	}
	return FAssetDocumentGenericReadOnlyDispatcher::DispatchPhase(
			*Runtime,
			Adapted.GetValue(),
			EAssetDocumentRuntimeRequestPhase::Verify);
}

FAssetDocumentRuntimeResultDto FAssetDocumentGenericRuntimeRoute::DispatchInspectProfileImpl(
	const FAssetDocumentProfileRequest& Request,
	const FString& AssetClassPath)
{
	if (Runtime == nullptr)
	{
		FAssetDocumentRuntimeRequestDto Inactive;
		return MakeRuntimeFailure(
			Inactive,
			TEXT("GenericRuntime.Inactive"),
			TEXT("Generic AssetDocument runtime is inactive."));
	}
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	TOptional<FAssetDocumentRuntimeRequestDto> Adapted =
		FAssetDocumentGenericRequestAdapter::AdaptInspectProfile(
			Request,
			AssetClassPath,
			Runtime->GetActiveRuntimeRevision(),
			Diagnostics);
	if (!Adapted.IsSet())
	{
		FAssetDocumentRuntimeResultDto Failure = MakeRuntimeFailure(
			{},
			TEXT("GenericRuntime.RequestAdaptationFailed"),
			TEXT("The Service request could not be adapted."));
		Failure.Diagnostics.Insert(Diagnostics, 0);
		return Failure;
	}
	FAnimationRequestScope AnimationScope(AnimationRequestBinding, Adapted.GetValue());
	if (!AnimationScope.Validation.bValid)
	{
	 auto Failure = MakeRuntimeFailure(Adapted.GetValue(), TEXT("GenericRuntime.AnimationRequestBindingFailed"), TEXT("Animation could not bind this Service request."));
	 Failure.Diagnostics.Append(AnimationScope.Validation.Diagnostics);
	 return Failure;
	}
	return FAssetDocumentGenericReadOnlyDispatcher::DispatchValidate(
		*Runtime,
		Adapted.GetValue());
}

FAssetDocumentRuntimeResultDto FAssetDocumentGenericRuntimeRoute::DispatchInspectImpl(
	const FAssetDocumentInspectRequest& Request,
	const FString& AssetClassPath,
	const FString& AssetObjectPath)
{
	if (Runtime == nullptr)
	{
		FAssetDocumentRuntimeRequestDto Inactive;
		return MakeRuntimeFailure(
			Inactive,
			TEXT("GenericRuntime.Inactive"),
			TEXT("Generic AssetDocument runtime is inactive."));
	}
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	TOptional<FAssetDocumentRuntimeRequestDto> Adapted =
		FAssetDocumentGenericRequestAdapter::AdaptInspect(
			Request,
			AssetClassPath,
			AssetObjectPath,
			Runtime->GetActiveRuntimeRevision(),
			Diagnostics);
	if (!Adapted.IsSet())
	{
		FAssetDocumentRuntimeResultDto Failure = MakeRuntimeFailure(
			{},
			TEXT("GenericRuntime.RequestAdaptationFailed"),
			TEXT("The Service request could not be adapted."));
		Failure.Diagnostics.Insert(Diagnostics, 0);
		return Failure;
	}
	FAnimationRequestScope AnimationScope(AnimationRequestBinding, Adapted.GetValue());
	if (!AnimationScope.Validation.bValid)
	{
	 auto Failure = MakeRuntimeFailure(Adapted.GetValue(), TEXT("GenericRuntime.AnimationRequestBindingFailed"), TEXT("Animation could not bind this Service request."));
	 Failure.Diagnostics.Append(AnimationScope.Validation.Diagnostics);
	 return Failure;
	}
	return FAssetDocumentGenericReadOnlyDispatcher::DispatchPhase(
		*Runtime,
		Adapted.GetValue(),
		EAssetDocumentRuntimeRequestPhase::Verify);
}

FAssetDocumentRuntimeResultDto FAssetDocumentGenericRuntimeRoute::DispatchCreateTemplateImpl(
	const FAssetDocumentTemplateRequest& Request,
	const FString& NormalizedTarget,
	const FString& AssetClassPath)
{
	if (Runtime == nullptr)
	{
		FAssetDocumentRuntimeRequestDto Inactive;
		return MakeRuntimeFailure(
			Inactive,
			TEXT("GenericRuntime.Inactive"),
			TEXT("Generic AssetDocument runtime is inactive."));
	}
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	TOptional<FAssetDocumentRuntimeRequestDto> Adapted =
		FAssetDocumentGenericRequestAdapter::AdaptCreateTemplate(
			Request,
			NormalizedTarget,
			AssetClassPath,
			Runtime->GetActiveRuntimeRevision(),
			Diagnostics);
	if (!Adapted.IsSet())
	{
		FAssetDocumentRuntimeResultDto Failure = MakeRuntimeFailure(
			{},
			TEXT("GenericRuntime.RequestAdaptationFailed"),
			TEXT("The Service request could not be adapted."));
		Failure.Diagnostics.Insert(Diagnostics, 0);
		return Failure;
	}
	FAnimationRequestScope AnimationScope(AnimationRequestBinding, Adapted.GetValue());
	if (!AnimationScope.Validation.bValid)
	{
	 auto Failure = MakeRuntimeFailure(Adapted.GetValue(), TEXT("GenericRuntime.AnimationRequestBindingFailed"), TEXT("Animation could not bind this Service request."));
	 Failure.Diagnostics.Append(AnimationScope.Validation.Diagnostics);
	 return Failure;
	}
	return FAssetDocumentGenericReadOnlyDispatcher::DispatchValidate(
		*Runtime,
		Adapted.GetValue());
}

FAssetDocumentRuntimeResultDto FAssetDocumentGenericRuntimeRoute::Dispatch(
	const FAssetDocumentRuntimeRequestDto& Request,
	IAssetDocumentNativeRequestContext& Context)
{
	if (ActiveRequestHost && ActiveTargetDecision.IsSet()
		&& Context.GetMetadata().Phase == EAssetDocumentRuntimeRequestPhase::Apply)
	{
		TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
		if (!ValidateAssetDocumentPreparedTargetDecision(ActiveTargetDecision.GetValue(), Request, Diagnostics))
		{
			auto Failure = MakeRuntimeFailure(Request, TEXT("GenericRuntime.TargetActivationDrift"),
				TEXT("Apply no longer matches the prepared target decision."));
			return Failure;
		}
		FAssetDocumentPreparedTargetContext Guard(Context, ActiveTargetDecision.GetValue(), Request);
		return ActiveRequestHost->Dispatch(Request, Guard);
	}
 auto Result = ActiveRequestHost != nullptr
  ? ActiveRequestHost->Dispatch(Request, Context)
  : MakeRuntimeFailure(Request, TEXT("GenericRuntime.RequestScopeMissing"), TEXT("Core dispatch occurred outside a generic request scope."));
 if (ActiveMutationComposition && Context.GetMetadata().Phase == EAssetDocumentRuntimeRequestPhase::Repair)
 {
  const auto Complete = ActiveMutationComposition->ValidateComplete();
  if (!Complete.bValid)
  {
   Result.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Failed;
   Result.FinalPhase = EAssetDocumentRuntimeRequestPhase::Failed;
   Result.ChangedSet.Reset();
   Result.Diagnostics.Append(Complete.Diagnostics);
  }
 }
 return Result;
}

IAssetDocumentAtomicTransaction*
FAssetDocumentGenericRuntimeRoute::ResolveTransactionForPhase(
	const FAssetDocumentRequestContextMetadata& Metadata,
	IAssetDocumentAtomicTransaction* ActiveTransaction,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
 if (ActiveMutationComposition && (Metadata.Phase == EAssetDocumentRuntimeRequestPhase::Apply || Metadata.Phase == EAssetDocumentRuntimeRequestPhase::Repair))
 {
  if (!ActiveTransaction) return nullptr;
  return ActiveMutationComposition->BindTransaction(*ActiveTransaction, Metadata, OutDiagnostics);
 }
	if (!ActiveBlackboardProjection.IsSet())
	{
		return ActiveTransaction;
	}
	if (Metadata.Phase != EAssetDocumentRuntimeRequestPhase::Apply)
	{
		return ActiveTransaction;
	}
	if (ActiveTransaction == nullptr
		|| Metadata.RequestId != ActiveBlackboardProjection->RequestId)
	{
		FAssetDocumentRuntimeDiagnostic& Diagnostic =
			OutDiagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/GenericRuntime/BlackboardFootprintProjection");
		Diagnostic.Code = TEXT("GenericRuntime.BlackboardProjectionContextMismatch");
		Diagnostic.Message = TEXT("The Blackboard projection is valid only for its exact Apply transaction.");
		return nullptr;
	}
	if (!ActiveBlackboardAdapter.IsValid())
	{
		ActiveBlackboardAdapter =
			MakeUnique<FAssetDocumentBlackboardAtomicTransactionAdapter>(
				*ActiveTransaction,
				MoveTemp(ActiveBlackboardProjection.GetValue()),
				MoveTemp(ActiveBlackboardCreatePrefix));
	}
	return ActiveBlackboardAdapter.Get();
}

// Every request entry point runs its work under FUnattendedRequestScope.
FAssetDocumentResult FAssetDocumentGenericRuntimeRoute::DispatchApplyFile(const FAssetDocumentApplyFileRequest& Request)
{
	const FUnattendedRequestScope Unattended;
	FAssetDocumentResult Result = DispatchApplyFileImpl(Request);
	Unattended.Finish(Result);
	return Result;
}

FAssetDocumentResult FAssetDocumentGenericRuntimeRoute::DispatchValidate(const FAssetDocumentValidateRequest& Request)
{
	const FUnattendedRequestScope Unattended;
	FAssetDocumentResult Result = DispatchValidateImpl(Request);
	Unattended.Finish(Result);
	return Result;
}

FAssetDocumentRuntimeResultDto FAssetDocumentGenericRuntimeRoute::DispatchExtract(
	const FAssetDocumentExtractRequest& Request, const FString& AssetClassPath)
{
	const FUnattendedRequestScope Unattended;
	FAssetDocumentRuntimeResultDto Result = DispatchExtractImpl(Request, AssetClassPath);
	Unattended.Finish(Result);
	return Result;
}

FAssetDocumentRuntimeResultDto FAssetDocumentGenericRuntimeRoute::DispatchDiff(const FAssetDocumentDiffRequest& Request)
{
	const FUnattendedRequestScope Unattended;
	FAssetDocumentRuntimeResultDto Result = DispatchDiffImpl(Request);
	Unattended.Finish(Result);
	return Result;
}

FAssetDocumentRuntimeResultDto FAssetDocumentGenericRuntimeRoute::DispatchInspectProfile(
	const FAssetDocumentProfileRequest& Request, const FString& AssetClassPath)
{
	const FUnattendedRequestScope Unattended;
	FAssetDocumentRuntimeResultDto Result = DispatchInspectProfileImpl(Request, AssetClassPath);
	Unattended.Finish(Result);
	return Result;
}

FAssetDocumentRuntimeResultDto FAssetDocumentGenericRuntimeRoute::DispatchInspect(
	const FAssetDocumentInspectRequest& Request, const FString& AssetClassPath, const FString& AssetObjectPath)
{
	const FUnattendedRequestScope Unattended;
	FAssetDocumentRuntimeResultDto Result = DispatchInspectImpl(Request, AssetClassPath, AssetObjectPath);
	Unattended.Finish(Result);
	return Result;
}

FAssetDocumentRuntimeResultDto FAssetDocumentGenericRuntimeRoute::DispatchCreateTemplate(
	const FAssetDocumentTemplateRequest& Request, const FString& NormalizedTarget, const FString& AssetClassPath)
{
	const FUnattendedRequestScope Unattended;
	FAssetDocumentRuntimeResultDto Result = DispatchCreateTemplateImpl(Request, NormalizedTarget, AssetClassPath);
	Unattended.Finish(Result);
	return Result;
}
