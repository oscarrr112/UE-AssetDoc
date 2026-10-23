// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/Bindings/AssetDocumentUMGAnimationBindingHandlers.h"

#include "Provider/AssetDocumentUMGMutationContract.h"
#include "Provider/AssetDocumentUMGNativePayloads.h"
#include "Provider/AssetDocumentUMGValueDto.h"

#include "Animation/WidgetAnimation.h"
#include "Blueprint/UserWidget.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "MovieScene.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSequence.h"

namespace AssetDocumentUMGAnimationBindingHandlersPrivate
{
FAssetDocumentPrimitiveOutcome Failure(const TCHAR* Code, const TCHAR* Message)
{
	return MakeAssetDocumentUMGFailure(Code, Message);
}

FAssetDocumentPrimitiveOutcome InvalidAnimationBindingInvocation(const FString& Detail)
{
	return Failure(TEXT("AD.UMG.PROVIDER.INVALID_INVOCATION"), *Detail);
}

struct FBindingMutationContext
{
	UWidgetAnimation* Animation = nullptr;
	UWidgetBlueprint* Blueprint = nullptr;
	UMovieScene* MovieScene = nullptr;
};

bool ResolveMutationContext(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics,
	FBindingMutationContext& Out)
{
	Out.Animation = ResolveWidgetAnimation(
		Invocation, Handles, TEXT("widget_animation"),
		EAssetDocumentHandleAccess::Mutable, OutDiagnostics);
	if (Out.Animation == nullptr)
	{
		return false;
	}
	Out.Blueprint = Cast<UWidgetBlueprint>(Out.Animation->GetOuter());
	Out.MovieScene = Out.Animation->MovieScene;
	if (!IsValid(Out.Blueprint) || !IsValid(Out.MovieScene))
	{
		FAssetDocumentRuntimeDiagnostic& Diagnostic = OutDiagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/Animation");
		Diagnostic.Code = TEXT("AD.UMG.PROVIDER.INVALID_HANDLE");
		Diagnostic.Message = TEXT("The WidgetAnimation must be owned by a live WidgetBlueprint and MovieScene.");
		return false;
	}
	return true;
}

bool HasAnimationBinding(
	const UWidgetAnimation& Animation,
	const FGuid& PossessableGuid,
	FName WidgetName)
{
	return Animation.GetBindings().ContainsByPredicate(
		[&PossessableGuid, WidgetName](const FWidgetAnimationBinding& Binding)
		{
			return Binding.AnimationGuid == PossessableGuid
				&& Binding.WidgetName == WidgetName;
		});
}

bool HasAnyAnimationBinding(
	const UWidgetAnimation& Animation,
	const FGuid& PossessableGuid)
{
	return Animation.GetBindings().ContainsByPredicate(
		[&PossessableGuid](const FWidgetAnimationBinding& Binding)
		{
			return Binding.AnimationGuid == PossessableGuid;
		});
}

bool EnlistAnimationBinding(
	IAssetDocumentAtomicTransaction* Transaction,
	const FBindingMutationContext& Context,
	TConstArrayView<UObject*> Touched,
	FAssetDocumentPrimitiveOutcome& Out)
{
	if (Transaction == nullptr)
	{
		Out = Failure(TEXT("AD.UMG.PROVIDER.INVALID_TRANSACTION"),
			TEXT("A matching active Mutating transaction is required."));
		return false;
	}
	const FAssetDocumentContractValidationResult Result =
		EnlistWidgetBlueprintMutationFootprint(*Transaction, *Context.Blueprint, Touched);
	if (!Result.bValid)
	{
		Out = MakeAssetDocumentUMGFailure(
			TEXT("AD.UMG.PROVIDER.INVALID_TRANSACTION"),
			TEXT("The complete UMG mutation footprint could not be enlisted."),
			Result.Diagnostics);
		return false;
	}
	return true;
}

FAssetDocumentPrimitiveOutcome RollbackFailure(
	IAssetDocumentAtomicTransaction& Transaction,
	FAssetDocumentPrimitiveOutcome&& FailureOutcome)
{
	RollbackAssetDocumentUMGMutation(
		Transaction, TEXT("AD.UMG.PROVIDER.POSTCONDITION_FAILED"),
		FailureOutcome.Diagnostics);
	return MoveTemp(FailureOutcome);
}

bool HasPostSideEffectMutationState(IAssetDocumentAtomicTransaction& Transaction)
{
	return Transaction.GetState() == EAssetDocumentAtomicState::Mutating;
}

bool HasPossessableIdentity(
	UMovieScene& MovieScene,
	const FString& Name,
	const UClass& Class)
{
	for (int32 Index = 0; Index < MovieScene.GetPossessableCount(); ++Index)
	{
		const FMovieScenePossessable& Existing = MovieScene.GetPossessable(Index);
		if (Existing.GetName() == Name
			&& Existing.GetPossessedObjectClass() == &Class)
		{
			return true;
		}
	}
	return false;
}

bool IsPreviewWidgetContext(
	const UUserWidget& PreviewRoot,
	const UWidget& Widget,
	FName WidgetName)
{
	return PreviewRoot.WidgetTree != nullptr
		&& PreviewRoot.WidgetTree->FindWidget(WidgetName) == &Widget;
}

FAssetDocumentPrimitiveOutcome MakeBindingSuccess(
	UObject& ChangedObject,
	FAssetDocumentRuntimeValueDto&& Value)
{
	FAssetDocumentPrimitiveOutcome Outcome =
		MakeAssetDocumentUMGSuccess(MoveTemp(Value));
	Outcome.ChangedSet.Add(MakeAssetDocumentUMGChangedRecord(
		ChangedObject, TEXT("/Animation"), TEXT("Updated")));
	return Outcome;
}
} // namespace AssetDocumentUMGAnimationBindingHandlersPrivate


FAssetDocumentPrimitiveOutcome InvokeMovieScenePossessableAdd(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AssetDocumentUMGAnimationBindingHandlersPrivate;
	FAssetDocumentUMGPossessableAddInput Input;
	FString DecodeDiagnostic;
	if (!DecodePossessableAddInput(Invocation.ValueArguments, Input, DecodeDiagnostic))
	{
		return InvalidAnimationBindingInvocation(DecodeDiagnostic);
	}
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	FBindingMutationContext Context;
	if (!ResolveMutationContext(Invocation, Handles, Diagnostics, Context))
	{
		return MakeAssetDocumentUMGFailure(
			TEXT("AD.UMG.PROVIDER.INVALID_HANDLE"),
			TEXT("The WidgetAnimation handle is invalid."), Diagnostics);
	}
	UClass* Class = ResolveWidgetClass(Invocation, Handles, TEXT("widget_class"), Diagnostics);
	if (Class == nullptr || !Class->IsChildOf(UObject::StaticClass()))
	{
		return MakeAssetDocumentUMGFailure(
			TEXT("AD.UMG.MOVIE_SCENE.INVALID_CLASS"),
			TEXT("The possessable class handle is invalid."), Diagnostics);
	}
	if (Class->GetPathName() != Input.ClassPath)
	{
		return Failure(TEXT("AD.UMG.MOVIE_SCENE.INVALID_CLASS"),
			TEXT("The class handle does not match the requested class path."));
	}
	if (HasPossessableIdentity(*Context.MovieScene, Input.Name, *Class))
	{
		return Failure(TEXT("AD.UMG.MOVIE_SCENE.POSSESSABLE_IDENTITY_COLLISION"),
			TEXT("A possessable with the requested logical identity already exists."));
	}
	UObject* Touched[] = {Context.Animation, Context.MovieScene};
	FAssetDocumentPrimitiveOutcome EnlistFailure;
	if (!EnlistAnimationBinding(ActiveTransaction, Context, Touched, EnlistFailure))
	{
		return EnlistFailure;
	}

	const FGuid Guid = Context.MovieScene->AddPossessable(Input.Name, Class);
	if (!HasPostSideEffectMutationState(*ActiveTransaction))
	{
		return RollbackFailure(*ActiveTransaction, Failure(
			TEXT("AD.UMG.PROVIDER.POSTCONDITION_FAILED"),
			TEXT("The transaction left Mutating during possessable creation.")));
	}
	FMovieScenePossessable* Added = Guid.IsValid()
		? Context.MovieScene->FindPossessable(Guid)
		: nullptr;
	if (!Guid.IsValid() || Added == nullptr || Added->GetName() != Input.Name
		|| Added->GetPossessedObjectClass() != Class)
	{
		return RollbackFailure(*ActiveTransaction, Failure(
			TEXT("AD.UMG.MOVIE_SCENE.INVALID_RETURNED_GUID"),
			TEXT("MovieScene did not return an exact possessable identity.")));
	}
	return MakeBindingSuccess(*Context.Animation, MakeAssetDocumentUMGObject({
		{TEXT("possessable_guid"), MakeAssetDocumentUMGString(Guid.ToString())}}));
}

FAssetDocumentPrimitiveOutcome InvokeMovieScenePossessableRemove(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AssetDocumentUMGAnimationBindingHandlersPrivate;
	FAssetDocumentUMGPossessableRemoveInput Input;
	FString DecodeDiagnostic;
	if (!DecodePossessableRemoveInput(Invocation.ValueArguments, Input, DecodeDiagnostic))
	{
		return InvalidAnimationBindingInvocation(DecodeDiagnostic);
	}
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	FBindingMutationContext Context;
	if (!ResolveMutationContext(Invocation, Handles, Diagnostics, Context))
	{
		return MakeAssetDocumentUMGFailure(
			TEXT("AD.UMG.PROVIDER.INVALID_HANDLE"),
			TEXT("The WidgetAnimation handle is invalid."), Diagnostics);
	}
	if (Context.MovieScene->FindPossessable(Input.PossessableGuid) == nullptr)
	{
		return Failure(TEXT("AD.UMG.MOVIE_SCENE.POSSESSABLE_NOT_FOUND"),
			TEXT("The requested possessable GUID is absent."));
	}
	if (HasAnyAnimationBinding(*Context.Animation, Input.PossessableGuid))
	{
		return Failure(TEXT("AD.UMG.MOVIE_SCENE.BINDING_STILL_PRESENT"),
			TEXT("The possessable still has a WidgetAnimation binding companion."));
	}
	UObject* Touched[] = {Context.Animation, Context.MovieScene};
	FAssetDocumentPrimitiveOutcome EnlistFailure;
	if (!EnlistAnimationBinding(ActiveTransaction, Context, Touched, EnlistFailure))
	{
		return EnlistFailure;
	}
	if (!Context.MovieScene->RemovePossessable(Input.PossessableGuid)
		|| !HasPostSideEffectMutationState(*ActiveTransaction)
		|| Context.MovieScene->FindPossessable(Input.PossessableGuid) != nullptr
		|| HasAnyAnimationBinding(*Context.Animation, Input.PossessableGuid))
	{
		return RollbackFailure(*ActiveTransaction, Failure(
			TEXT("AD.UMG.PROVIDER.POSTCONDITION_FAILED"),
			TEXT("MovieScene possessable removal did not reach exact absence.")));
	}
	return MakeBindingSuccess(*Context.Animation, MakeAssetDocumentUMGObject({
		{TEXT("removed_guid"), MakeAssetDocumentUMGString(Input.PossessableGuid.ToString())}}));
}

FAssetDocumentPrimitiveOutcome InvokeWidgetAnimationSequenceBind(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AssetDocumentUMGAnimationBindingHandlersPrivate;
	FAssetDocumentUMGSequenceBindingInput Input;
	FString DecodeDiagnostic;
	if (!DecodeSequenceBindInput(Invocation.ValueArguments, Input, DecodeDiagnostic))
	{
		return InvalidAnimationBindingInvocation(DecodeDiagnostic);
	}
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	FBindingMutationContext Context;
	if (!ResolveMutationContext(Invocation, Handles, Diagnostics, Context))
	{
		return MakeAssetDocumentUMGFailure(TEXT("AD.UMG.PROVIDER.INVALID_HANDLE"),
			TEXT("The WidgetAnimation handle is invalid."), Diagnostics);
	}
	UUserWidget* PreviewRoot = ResolvePreviewRoot(
		Invocation, Handles, TEXT("preview_root"), Diagnostics);
	UWidget* Widget = ResolveWidget(Invocation, Handles, TEXT("widget"),
		EAssetDocumentHandleAccess::ReadOnly, Diagnostics);
	if (PreviewRoot == nullptr || Widget == nullptr)
	{
		return MakeAssetDocumentUMGFailure(TEXT("AD.UMG.PROVIDER.INVALID_HANDLE"),
			TEXT("The preview context or widget handle is invalid."), Diagnostics);
	}
	if (Context.MovieScene->FindPossessable(Input.PossessableGuid) == nullptr)
	{
		return Failure(TEXT("AD.UMG.ANIMATION_BINDING.GUID_MISMATCH"),
			TEXT("The possessable GUID is not owned by the WidgetAnimation MovieScene."));
	}
	if (!IsPreviewWidgetContext(*PreviewRoot, *Widget, Input.WidgetName))
	{
		return Failure(TEXT("AD.UMG.ANIMATION_BINDING.PREVIEW_CONTEXT_MISMATCH"),
			TEXT("The preview root does not own the named widget."));
	}
	if (HasAnimationBinding(*Context.Animation, Input.PossessableGuid, Input.WidgetName))
	{
		return Failure(TEXT("AD.UMG.ANIMATION_BINDING.DUPLICATE_BINDING"),
			TEXT("The requested WidgetAnimation binding already exists."));
	}
	UObject* Touched[] = {Context.Animation, Context.MovieScene, PreviewRoot, Widget};
	FAssetDocumentPrimitiveOutcome EnlistFailure;
	if (!EnlistAnimationBinding(ActiveTransaction, Context, Touched, EnlistFailure))
	{
		return EnlistFailure;
	}

	UMovieSceneSequence& Sequence = *Context.Animation;
	Sequence.BindPossessableObject(Input.PossessableGuid, *Widget, PreviewRoot);
	if (!HasPostSideEffectMutationState(*ActiveTransaction)
		|| !HasAnimationBinding(*Context.Animation, Input.PossessableGuid, Input.WidgetName))
	{
		return RollbackFailure(*ActiveTransaction, Failure(
			TEXT("AD.UMG.PROVIDER.POSTCONDITION_FAILED"),
			TEXT("Virtual WidgetAnimation binding dispatch did not produce exact readback.")));
	}
	return MakeBindingSuccess(*Context.Animation, MakeAssetDocumentUMGObject({
		{TEXT("possessable_guid"), MakeAssetDocumentUMGString(Input.PossessableGuid.ToString())},
		{TEXT("widget_name"), MakeAssetDocumentUMGString(Input.WidgetName.ToString())}}));
}

FAssetDocumentPrimitiveOutcome InvokeWidgetAnimationSequenceUnbind(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AssetDocumentUMGAnimationBindingHandlersPrivate;
	FAssetDocumentUMGSequenceBindingInput Input;
	FString DecodeDiagnostic;
	if (!DecodeSequenceUnbindInput(Invocation.ValueArguments, Input, DecodeDiagnostic))
	{
		return InvalidAnimationBindingInvocation(DecodeDiagnostic);
	}
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	FBindingMutationContext Context;
	if (!ResolveMutationContext(Invocation, Handles, Diagnostics, Context))
	{
		return MakeAssetDocumentUMGFailure(TEXT("AD.UMG.PROVIDER.INVALID_HANDLE"),
			TEXT("The WidgetAnimation handle is invalid."), Diagnostics);
	}
	UUserWidget* PreviewRoot = ResolvePreviewRoot(
		Invocation, Handles, TEXT("preview_root"), Diagnostics);
	if (PreviewRoot == nullptr)
	{
		return MakeAssetDocumentUMGFailure(TEXT("AD.UMG.PROVIDER.INVALID_HANDLE"),
			TEXT("The preview root handle is invalid."), Diagnostics);
	}
	if (!HasAnimationBinding(*Context.Animation, Input.PossessableGuid, Input.WidgetName))
	{
		return Failure(TEXT("AD.UMG.ANIMATION_BINDING.BINDING_NOT_FOUND"),
			TEXT("The exact WidgetAnimation binding is absent."));
	}
	UObject* Touched[] = {Context.Animation, Context.MovieScene, PreviewRoot};
	FAssetDocumentPrimitiveOutcome EnlistFailure;
	if (!EnlistAnimationBinding(ActiveTransaction, Context, Touched, EnlistFailure))
	{
		return EnlistFailure;
	}

	UMovieSceneSequence& Sequence = *Context.Animation;
	Sequence.UnbindPossessableObjects(Input.PossessableGuid);
	if (!HasPostSideEffectMutationState(*ActiveTransaction)
		|| HasAnyAnimationBinding(*Context.Animation, Input.PossessableGuid))
	{
		return RollbackFailure(*ActiveTransaction, Failure(
			TEXT("AD.UMG.ANIMATION_BINDING.COMPANION_REMAINS"),
			TEXT("Virtual WidgetAnimation unbind dispatch left a binding companion.")));
	}
	return MakeBindingSuccess(*Context.Animation, MakeAssetDocumentUMGObject({
		{TEXT("possessable_guid"), MakeAssetDocumentUMGString(Input.PossessableGuid.ToString())},
		{TEXT("widget_name"), MakeAssetDocumentUMGString(Input.WidgetName.ToString())}}));
}

TConstArrayView<FAssetDocumentUMGPrimitiveBinding>
GetAssetDocumentUMGAnimationBindingPrimitiveBindings()
{
	static const FAssetDocumentUMGPrimitiveBinding Bindings[] = {
		{TEXT("umg.movie_scene.possessable.add.v1"), 1, &InvokeMovieScenePossessableAdd},
		{TEXT("umg.movie_scene.possessable.remove.v1"), 1, &InvokeMovieScenePossessableRemove},
		{TEXT("umg.animation.sequence.bind.v1"), 1, &InvokeWidgetAnimationSequenceBind},
		{TEXT("umg.animation.sequence.unbind.v1"), 1, &InvokeWidgetAnimationSequenceUnbind}};
	return Bindings;
}
