// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentGenericPersistenceVerifier.h"

#include "AssetDocumentGenericReadOnlyDispatcher.h"
#include "AssetDocumentAnimationRequestBinding.h"
#include "AssetDocumentStagedMontageReadBinding.h"
#include "AssetDocumentStagedBehaviorTreeReadBinding.h"
#include "Animation/AnimMontage.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
FAssetDocumentRuntimeDiagnostic MakeFreshDiagnostic(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = TEXT("/GenericRuntime/FreshVerification");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Diagnostic;
}
}

FAssetDocumentGenericPersistenceVerifier::
	FAssetDocumentGenericPersistenceVerifier(
		IAssetDocumentPythonRuntime& InRuntime,
		FAssetDocumentRuntimeRequestDto InRequest,
		TSharedPtr<FAssetDocumentAnimationRequestBinding> InAnimation,
		TSharedPtr<FAssetDocumentStagedMontageReadBinding> InStagedRead,
  TSharedPtr<FAssetDocumentStagedBehaviorTreeReadBinding> InBehaviorTreeStagedRead)
	: Runtime(InRuntime)
	, Request(MoveTemp(InRequest))
	, Animation(MoveTemp(InAnimation))
	, StagedRead(MoveTemp(InStagedRead))
 , BehaviorTreeStagedRead(MoveTemp(InBehaviorTreeStagedRead))
{
}

FAssetDocumentFreshVerificationResult
FAssetDocumentGenericPersistenceVerifier::VerifyFresh(
	const FAssetDocumentFreshVerificationInput& Input)
{
	FAssetDocumentFreshVerificationResult Verification;
	// Loaded as the editor's diff loads another revision of an asset (DiffUtils::LoadPackageForDiff):
	// a staged Blueprint copy is read as saved and never compiled on load. Its compile would resolve
	// against the live Blueprints it references (a component Blueprint that casts to the Actor
	// Blueprint holding it: "In use pin ... no longer exists") and take part in the live compile and
	// reinstancing machinery for a copy that is discarded right after this read (KI-1).
	UPackage* Package = LoadPackage(
		nullptr,
		*Input.StagedPackagePath,
		LOAD_ForDiff | LOAD_NoWarn | LOAD_DisableCompileOnLoad);
	if (Package == nullptr)
	{
		Verification.Diagnostics.Add(MakeFreshDiagnostic(
			TEXT("GenericRuntime.FreshLoadFailed"),
			TEXT("The staged package could not be loaded ordinarily.")));
		return Verification;
	}
	const FString LoadedPath =
		Package->GetLoadedPath().GetLocalFullPath();
	Verification.bFreshLoadProven =
		FPaths::IsSamePath(LoadedPath, Input.StagedPackagePath);
	TUniquePtr<FAssetDocumentAnimationStagedReadLease> AnimationStagedRead;
	FString StagedObjectPath;
	if (Animation && Request.RequestedAssetClassPath == TEXT("/Script/Engine.AnimMontage"))
	{
		UObject* Object = FindObject<UObject>(Package, *FPackageName::ObjectPathToObjectName(Request.TargetAssetPath));
		if (!Verification.bFreshLoadProven || !Object || Object->GetOuter() != Package
			|| Object->GetClass() != UAnimMontage::StaticClass() || Input.FreshHandleRevision == 0)
		{
			Verification.Diagnostics.Add(MakeFreshDiagnostic(TEXT("GenericRuntime.AnimationStagedIdentityMismatch"),
				TEXT("The fresh package must contain this request's exact staged Montage.")));
			return Verification;
		}
		AnimationStagedRead = Animation->BindStagedRead(*CastChecked<UAnimMontage>(Object),
			Input.StagedPackagePath, Input.FreshHandleRevision, Verification.Diagnostics);
		if (!AnimationStagedRead) return Verification;
		StagedObjectPath = Object->GetPathName();
	}
 const bool bBehaviorTree = Request.RequestedAssetClassPath == TEXT("/Script/AIModule.BehaviorTree");
 FString BehaviorTreeStagedObjectPath;
 if (bBehaviorTree)
 {
  UObject* Object = FindObject<UObject>(Package, *FPackageName::ObjectPathToObjectName(Request.TargetAssetPath));
  UObject* Live = FindObject<UObject>(nullptr, *Request.TargetAssetPath);
  if (!BehaviorTreeStagedRead || !Verification.bFreshLoadProven || Input.FreshHandleRevision == 0
   || !Object || Object == Live || Object->GetOuter() != Package || Object->GetPathName() == Request.TargetAssetPath
   || Object->GetClass()->GetPathName() != Request.RequestedAssetClassPath
   || (Live && Live->GetOutermost() == Package))
  {
   Verification.Diagnostics.Add(MakeFreshDiagnostic(TEXT("GenericRuntime.BehaviorTreeStagedIdentityMismatch"),
    TEXT("Verify requires the distinct exact staged BehaviorTree, matching loaded filename and its scoped native read binding.")));
   return Verification;
  }
  BehaviorTreeStagedObjectPath = Object->GetPathName();
 }
	const FAssetDocumentRuntimeRequestDto OriginalRequest = Request;
	Request.ExpectedRuntimeRevision = Input.FreshHandleRevision;
	FAssetDocumentRuntimeResultDto Result;
	if (AnimationStagedRead)
	{
		FAssetDocumentRequestContextMetadata Metadata;
		Metadata.RequestId = Request.RequestId;
		Metadata.PinnedRuntimeRevision = Request.ExpectedRuntimeRevision;
		Metadata.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
		auto Context = Runtime.CreateRequestContext(Metadata, FGuid::NewGuid(), nullptr);
		if (!Context.Context) { Verification.Diagnostics.Append(Context.Diagnostics); return Verification; }
		if (!StagedRead || !StagedRead->Bind(OriginalRequest, StagedObjectPath, Input.StagedPackagePath, Input.FreshHandleRevision))
		{
			Verification.Diagnostics.Add(MakeFreshDiagnostic(TEXT("GenericRuntime.AnimationStagedResolveMismatch"),
				TEXT("The exact staged Editor read issuance could not bind this verification.")));
			return Verification;
		}
		// Close native issuance and all handoff aliases before destroying the context.
		ON_SCOPE_EXIT { StagedRead->Close(); };
		Result = Runtime.GetHost().Dispatch(Request, *Context.Context);
	}
 else if (bBehaviorTree)
 {
  FAssetDocumentRequestContextMetadata Metadata;
  Metadata.RequestId = Request.RequestId;
  Metadata.PinnedRuntimeRevision = Request.ExpectedRuntimeRevision;
  Metadata.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
  auto Context = Runtime.CreateRequestContext(Metadata, FGuid::NewGuid(), nullptr);
  if (!Context.Context) { Verification.Diagnostics.Append(Context.Diagnostics); return Verification; }
  // Binding and all aliases close before the Verify context is destroyed,
  // including a partially rejected Bind; never fall back to the live target.
  ON_SCOPE_EXIT { BehaviorTreeStagedRead->Close(); };
  if (!BehaviorTreeStagedRead->Bind(OriginalRequest, BehaviorTreeStagedObjectPath, Input.StagedPackagePath, Input.FreshHandleRevision))
  {
   Verification.Diagnostics.Add(MakeFreshDiagnostic(TEXT("GenericRuntime.BehaviorTreeStagedResolveMismatch"),
    TEXT("The exact staged BehaviorTree read binding rejected this fresh verification.")));
   return Verification;
  }
  Result = Runtime.GetHost().Dispatch(Request, *Context.Context);
 }
	else
	{
		Result = FAssetDocumentGenericReadOnlyDispatcher::DispatchPhase(Runtime, Request, EAssetDocumentRuntimeRequestPhase::Verify);
	}
	Verification.Diagnostics = Result.Diagnostics;
	Verification.ActualSemanticHash = Result.SemanticHash;
	Verification.ActualCodecVersion = Result.CodecVersion;
	Verification.bCanonicalMatch =
		Result.TerminalStatus
			== EAssetDocumentRuntimeTerminalStatus::Succeeded
		&& Result.SemanticHash == Input.ExpectedSemanticHash
		&& Result.CodecVersion == Input.ExpectedCodecVersion;
	return Verification;
}
