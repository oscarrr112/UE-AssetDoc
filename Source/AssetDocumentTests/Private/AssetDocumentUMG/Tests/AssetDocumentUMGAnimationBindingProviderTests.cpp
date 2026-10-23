// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentUMGPrimitiveProvider.h"
#include "AssetDocumentUMGProviderTestSupport.h"
#include "Fixtures/AssetDocumentUMGProviderTestFixture.h"
#include "Provider/AssetDocumentUMGPrimitiveCatalog.h"
#include "Provider/AssetDocumentUMGValueDto.h"

#include "Animation/WidgetAnimation.h"
#include "Blueprint/UserWidget.h"
#include "Components/Border.h"
#include "Components/Widget.h"
#include "Misc/AutomationTest.h"
#include "MovieScene.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSequence.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentUMGAnimationBindingProviderTestsPrivate
{
const FAssetDocumentPrimitiveDescriptor* FindAnimationBindingDescriptor(FStringView PrimitiveId)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : GetAssetDocumentUMGPrimitiveDescriptors())
	{
		if (Descriptor.PrimitiveId == PrimitiveId) return &Descriptor;
	}
	return nullptr;
}

void AddAnimationBindingArgument(FAssetDocumentPrimitiveInvocation& Invocation, FString Name, FAssetDocumentOpaqueHandle Handle)
{
	Invocation.HandleArguments.Add({MoveTemp(Name), MoveTemp(Handle)});
}

bool HasArgument(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	FStringView Name)
{
	return Descriptor.HandleArgumentSchemas.ContainsByPredicate(
		[Name](const FAssetDocumentPrimitiveHandleArgumentSchema& Argument)
		{
			return Argument.Name == Name;
		});
}

FAssetDocumentPrimitiveOutcome Invoke(
	IAssetDocumentPrimitiveProvider& Provider, const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentUMGProviderTestContext& Context, FAssetDocumentUMGProviderTestHandleResolver& Handles,
	FAssetDocumentUMGProviderTestTransaction& Transaction, UWidgetAnimation& Animation,
	UClass& WidgetClass, UUserWidget* PreviewRoot, UWidget* Widget, FAssetDocumentRuntimeValueDto Value)
{
	FAssetDocumentPrimitiveInvocation Invocation = MakeAssetDocumentUMGProviderTestInvocation(Descriptor, Context);
	AddAnimationBindingArgument(Invocation, TEXT("widget_animation"), Handles.AddObjectHandle(&Animation, EAssetDocumentHandleKind::Subobject, EAssetDocumentHandleAccess::Mutable));
	if (Descriptor.PrimitiveId == TEXT("umg.movie_scene.possessable.add.v1"))
		AddAnimationBindingArgument(Invocation, TEXT("widget_class"), Handles.AddObjectHandle(&WidgetClass, EAssetDocumentHandleKind::Class, EAssetDocumentHandleAccess::ReadOnly));
	if (PreviewRoot != nullptr
		&& HasArgument(Descriptor, TEXT("preview_root")))
		AddAnimationBindingArgument(Invocation, TEXT("preview_root"), Handles.AddObjectHandle(PreviewRoot, EAssetDocumentHandleKind::Scratch, EAssetDocumentHandleAccess::Mutable));
	if (Widget != nullptr
		&& HasArgument(Descriptor, TEXT("widget")))
		AddAnimationBindingArgument(Invocation, TEXT("widget"), Handles.AddObjectHandle(Widget, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::ReadOnly));
	Invocation.ValueArguments = MoveTemp(Value);
	return Provider.Invoke(Invocation, Handles, &Transaction);
}

FGuid ReadAnimationBindingGuid(const FAssetDocumentPrimitiveOutcome& Outcome, FStringView Name)
{
	const int32 Index = Outcome.Value.ObjectKeys.IndexOfByKey(Name);
	FGuid Result;
	if (Outcome.Value.Elements.IsValidIndex(Index)) FGuid::Parse(Outcome.Value.Elements[Index].StringValue, Result);
	return Result;
}

void AnimationBindingRecordDiagnostics(
	FAutomationTestBase& Test,
	const TCHAR* Operation,
	const FAssetDocumentPrimitiveOutcome& Outcome)
{
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic :
		Outcome.Diagnostics)
	{
		Test.AddInfo(FString::Printf(
			TEXT("%s diagnostic: %s %s"),
			Operation,
			*Diagnostic.Code,
			*Diagnostic.Message));
	}
}

bool HasDiagnostic(
	const FAssetDocumentPrimitiveOutcome& Outcome,
	FStringView Code)
{
	return Outcome.Diagnostics.ContainsByPredicate(
		[Code](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == Code;
		});
}

FAssetDocumentPrimitiveOutcome InvokeWithExplicitBindingHandles(
	IAssetDocumentPrimitiveProvider& Provider,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentUMGProviderTestContext& Context,
	FAssetDocumentUMGProviderTestHandleResolver& Handles,
	FAssetDocumentUMGProviderTestTransaction& Transaction,
	const FAssetDocumentOpaqueHandle& AnimationHandle,
	const FAssetDocumentOpaqueHandle& PreviewRootHandle,
	const FAssetDocumentOpaqueHandle& WidgetHandle,
	const FGuid& PossessableGuid)
{
	FAssetDocumentPrimitiveInvocation Invocation =
		MakeAssetDocumentUMGProviderTestInvocation(Descriptor, Context);
	AddAnimationBindingArgument(
		Invocation, TEXT("widget_animation"), AnimationHandle);
	AddAnimationBindingArgument(
		Invocation, TEXT("preview_root"), PreviewRootHandle);
	AddAnimationBindingArgument(
		Invocation, TEXT("widget"), WidgetHandle);
	Invocation.ValueArguments = MakeAssetDocumentUMGObject({
		{TEXT("possessable_guid"),
			MakeAssetDocumentUMGString(PossessableGuid.ToString())},
		{TEXT("widget_name"),
			MakeAssetDocumentUMGString(TEXT("TargetWidget"))}});
	return Provider.Invoke(Invocation, Handles, &Transaction);
}
} // namespace AssetDocumentUMGAnimationBindingProviderTestsPrivate


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUMGAnimationBindingLifecycleTest,
	"UEAssetDoc.UMGProvider.AnimationBindingLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUMGAnimationBindingLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentUMGAnimationBindingProviderTestsPrivate;
	const FAssetDocumentPrimitiveDescriptor* Add = FindAnimationBindingDescriptor(TEXT("umg.movie_scene.possessable.add.v1"));
	const FAssetDocumentPrimitiveDescriptor* Remove = FindAnimationBindingDescriptor(TEXT("umg.movie_scene.possessable.remove.v1"));
	const FAssetDocumentPrimitiveDescriptor* Bind = FindAnimationBindingDescriptor(TEXT("umg.animation.sequence.bind.v1"));
	const FAssetDocumentPrimitiveDescriptor* Unbind = FindAnimationBindingDescriptor(TEXT("umg.animation.sequence.unbind.v1"));
	if (!TestNotNull(TEXT("Add descriptor."), Add) || !TestNotNull(TEXT("Remove descriptor."), Remove)
		|| !TestNotNull(TEXT("Bind descriptor."), Bind) || !TestNotNull(TEXT("Unbind descriptor."), Unbind)) return false;
	FAssetDocumentUMGProviderTestFixture Fixture;
	if (!TestTrue(TEXT("Fixture."), Fixture.CreateTransientWidgetBlueprint())) return false;
	UWidgetAnimation* Animation = Fixture.AddFixtureAnimation(TEXT("Fade"));
	UUserWidget* PreviewRoot = Fixture.CreatePreviewRoot(TEXT("Preview"));
	UWidget* Widget = PreviewRoot ? Fixture.CreatePreviewWidget(UBorder::StaticClass(), TEXT("TargetWidget"), *PreviewRoot) : nullptr;
	if (!TestNotNull(TEXT("Animation."), Animation) || !TestNotNull(TEXT("Preview widget."), Widget)) return false;
	FAssetDocumentUMGProviderTestContext Context;
	FAssetDocumentUMGProviderTestHandleResolver Handles(Context);
	FAssetDocumentUMGProviderTestTransaction Transaction(Context);
	TSharedRef<IAssetDocumentPrimitiveProvider> Provider = CreateAssetDocumentUMGPrimitiveProvider();
	const FAssetDocumentPrimitiveOutcome AddOutcome = Invoke(*Provider, *Add, Context, Handles, Transaction, *Animation,
		*UBorder::StaticClass(), PreviewRoot, Widget, MakeAssetDocumentUMGObject({
			{TEXT("name"), MakeAssetDocumentUMGString(TEXT("TargetWidget"))}, {TEXT("class_path"), MakeAssetDocumentUMGString(UBorder::StaticClass()->GetPathName())}}));
	AnimationBindingRecordDiagnostics(
		*this, TEXT("possessable add"), AddOutcome);
	const FGuid Guid = ReadAnimationBindingGuid(AddOutcome, TEXT("possessable_guid"));
	TestTrue(TEXT("Add returns an exact possessable."), AddOutcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded && Guid.IsValid() && Animation->MovieScene->FindPossessable(Guid) != nullptr);
	const FString AfterAdd = Fixture.CaptureDigest();
	const FAssetDocumentPrimitiveOutcome DuplicatePossessable = Invoke(
		*Provider, *Add, Context, Handles, Transaction, *Animation,
		*UBorder::StaticClass(), PreviewRoot, Widget,
		MakeAssetDocumentUMGObject({
			{TEXT("name"),
				MakeAssetDocumentUMGString(TEXT("TargetWidget"))},
			{TEXT("class_path"),
				MakeAssetDocumentUMGString(
					UBorder::StaticClass()->GetPathName())}}));
	TestTrue(
		TEXT("Duplicate possessable identity fails without mutation."),
		DuplicatePossessable.Status
				== EAssetDocumentPrimitiveOutcomeStatus::Failed
			&& HasDiagnostic(
				DuplicatePossessable,
				TEXT("AD.UMG.MOVIE_SCENE.POSSESSABLE_IDENTITY_COLLISION"))
			&& Fixture.CaptureDigest() == AfterAdd);

	const FGuid AbsentGuid = FGuid::NewGuid();
	const FAssetDocumentPrimitiveOutcome AbsentGuidBinding = Invoke(
		*Provider, *Bind, Context, Handles, Transaction, *Animation,
		*UBorder::StaticClass(), PreviewRoot, Widget,
		MakeAssetDocumentUMGObject({
			{TEXT("possessable_guid"),
				MakeAssetDocumentUMGString(AbsentGuid.ToString())},
			{TEXT("widget_name"),
				MakeAssetDocumentUMGString(TEXT("TargetWidget"))}}));
	TestTrue(
		TEXT("Binding an absent GUID fails closed."),
		AbsentGuidBinding.Status
				== EAssetDocumentPrimitiveOutcomeStatus::Failed
			&& HasDiagnostic(
				AbsentGuidBinding,
				TEXT("AD.UMG.ANIMATION_BINDING.GUID_MISMATCH"))
			&& Fixture.CaptureDigest() == AfterAdd);

	UUserWidget* WrongPreviewRoot =
		Fixture.CreatePreviewRoot(TEXT("WrongPreview"));
	const FAssetDocumentPrimitiveOutcome WrongPreviewBinding = Invoke(
		*Provider, *Bind, Context, Handles, Transaction, *Animation,
		*UBorder::StaticClass(), WrongPreviewRoot, Widget,
		MakeAssetDocumentUMGObject({
			{TEXT("possessable_guid"),
				MakeAssetDocumentUMGString(Guid.ToString())},
			{TEXT("widget_name"),
				MakeAssetDocumentUMGString(TEXT("TargetWidget"))}}));
	TestTrue(
		TEXT("Wrong preview ownership fails closed."),
		WrongPreviewBinding.Status
				== EAssetDocumentPrimitiveOutcomeStatus::Failed
			&& HasDiagnostic(
				WrongPreviewBinding,
				TEXT("AD.UMG.ANIMATION_BINDING.PREVIEW_CONTEXT_MISMATCH"))
			&& Fixture.CaptureDigest() == AfterAdd);

	const FAssetDocumentOpaqueHandle StalePreviewHandle =
		Handles.AddObjectHandle(
			PreviewRoot,
			EAssetDocumentHandleKind::Scratch,
			EAssetDocumentHandleAccess::Mutable);
	Handles.AdvanceProviderGeneration();
	const FAssetDocumentOpaqueHandle FreshAnimationHandle =
		Handles.AddObjectHandle(
			Animation,
			EAssetDocumentHandleKind::Subobject,
			EAssetDocumentHandleAccess::Mutable);
	const FAssetDocumentOpaqueHandle FreshWidgetHandle =
		Handles.AddObjectHandle(
			Widget,
			EAssetDocumentHandleKind::Node,
			EAssetDocumentHandleAccess::ReadOnly);
	const FAssetDocumentPrimitiveOutcome StalePreviewBinding =
		InvokeWithExplicitBindingHandles(
			*Provider, *Bind, Context, Handles, Transaction,
			FreshAnimationHandle, StalePreviewHandle,
			FreshWidgetHandle, Guid);
	TestTrue(
		TEXT("Stale Scratch preview handle fails closed."),
		StalePreviewBinding.Status
				== EAssetDocumentPrimitiveOutcomeStatus::Failed
			&& HasDiagnostic(
				StalePreviewBinding,
				TEXT("AD.UMG.PROVIDER.INVALID_HANDLE"))
			&& Fixture.CaptureDigest() == AfterAdd);

	const FAssetDocumentPrimitiveOutcome BindOutcome = Invoke(*Provider, *Bind, Context, Handles, Transaction, *Animation,
		*UBorder::StaticClass(), PreviewRoot, Widget, MakeAssetDocumentUMGObject({
			{TEXT("possessable_guid"), MakeAssetDocumentUMGString(Guid.ToString())}, {TEXT("widget_name"), MakeAssetDocumentUMGString(TEXT("TargetWidget"))}}));
	AnimationBindingRecordDiagnostics(
		*this, TEXT("sequence bind"), BindOutcome);
	TestTrue(TEXT("Bind performs virtual dispatch and readback."), BindOutcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded && Animation->GetBindings().Num() == 1);
	const FString AfterBind = Fixture.CaptureDigest();
	const FAssetDocumentPrimitiveOutcome DuplicateBinding = Invoke(
		*Provider, *Bind, Context, Handles, Transaction, *Animation,
		*UBorder::StaticClass(), PreviewRoot, Widget,
		MakeAssetDocumentUMGObject({
			{TEXT("possessable_guid"),
				MakeAssetDocumentUMGString(Guid.ToString())},
			{TEXT("widget_name"),
				MakeAssetDocumentUMGString(TEXT("TargetWidget"))}}));
	TestTrue(
		TEXT("Duplicate binding fails without mutation."),
		DuplicateBinding.Status
				== EAssetDocumentPrimitiveOutcomeStatus::Failed
			&& HasDiagnostic(
				DuplicateBinding,
				TEXT("AD.UMG.ANIMATION_BINDING.DUPLICATE_BINDING"))
			&& Fixture.CaptureDigest() == AfterBind);
	const FAssetDocumentPrimitiveOutcome RemoveWithCompanion = Invoke(
		*Provider, *Remove, Context, Handles, Transaction, *Animation,
		*UBorder::StaticClass(), nullptr, nullptr,
		MakeAssetDocumentUMGObject({
			{TEXT("possessable_guid"),
				MakeAssetDocumentUMGString(Guid.ToString())}}));
	TestTrue(
		TEXT("Possessable removal requires binding companion cleanup."),
		RemoveWithCompanion.Status
				== EAssetDocumentPrimitiveOutcomeStatus::Failed
			&& HasDiagnostic(
				RemoveWithCompanion,
				TEXT("AD.UMG.MOVIE_SCENE.BINDING_STILL_PRESENT"))
			&& Fixture.CaptureDigest() == AfterBind);
	const FAssetDocumentPrimitiveOutcome UnbindOutcome = Invoke(*Provider, *Unbind, Context, Handles, Transaction, *Animation,
		*UBorder::StaticClass(), PreviewRoot, nullptr, MakeAssetDocumentUMGObject({
			{TEXT("possessable_guid"), MakeAssetDocumentUMGString(Guid.ToString())}, {TEXT("widget_name"), MakeAssetDocumentUMGString(TEXT("TargetWidget"))}}));
	AnimationBindingRecordDiagnostics(
		*this, TEXT("sequence unbind"), UnbindOutcome);
	TestTrue(TEXT("Unbind cleans the exact binding companion."), UnbindOutcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded && Animation->GetBindings().IsEmpty());
	const FString AfterUnbind = Fixture.CaptureDigest();
	const FAssetDocumentPrimitiveOutcome NoOpUnbind = Invoke(
		*Provider, *Unbind, Context, Handles, Transaction, *Animation,
		*UBorder::StaticClass(), PreviewRoot, nullptr,
		MakeAssetDocumentUMGObject({
			{TEXT("possessable_guid"),
				MakeAssetDocumentUMGString(Guid.ToString())},
			{TEXT("widget_name"),
				MakeAssetDocumentUMGString(TEXT("TargetWidget"))}}));
	TestTrue(
		TEXT("Absent binding unbind is an explicit no-op failure."),
		NoOpUnbind.Status
				== EAssetDocumentPrimitiveOutcomeStatus::Failed
			&& HasDiagnostic(
				NoOpUnbind,
				TEXT("AD.UMG.ANIMATION_BINDING.BINDING_NOT_FOUND"))
			&& Fixture.CaptureDigest() == AfterUnbind);
	const FAssetDocumentPrimitiveOutcome RemoveOutcome = Invoke(*Provider, *Remove, Context, Handles, Transaction, *Animation,
		*UBorder::StaticClass(), nullptr, nullptr, MakeAssetDocumentUMGObject({
			{TEXT("possessable_guid"), MakeAssetDocumentUMGString(Guid.ToString())}}));
	AnimationBindingRecordDiagnostics(
		*this, TEXT("possessable remove"), RemoveOutcome);
	TestTrue(TEXT("Remove verifies absence."), RemoveOutcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded && Animation->MovieScene->FindPossessable(Guid) == nullptr);
	const FString AfterRemove = Fixture.CaptureDigest();
	const FAssetDocumentPrimitiveOutcome NoOpRemove = Invoke(
		*Provider, *Remove, Context, Handles, Transaction, *Animation,
		*UBorder::StaticClass(), nullptr, nullptr,
		MakeAssetDocumentUMGObject({
			{TEXT("possessable_guid"),
				MakeAssetDocumentUMGString(Guid.ToString())}}));
	TestTrue(
		TEXT("Absent possessable removal is an explicit no-op failure."),
		NoOpRemove.Status
				== EAssetDocumentPrimitiveOutcomeStatus::Failed
			&& HasDiagnostic(
				NoOpRemove,
				TEXT("AD.UMG.MOVIE_SCENE.POSSESSABLE_NOT_FOUND"))
			&& Fixture.CaptureDigest() == AfterRemove);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUMGAnimationBindingRollbackTest,
	"UEAssetDoc.UMGProvider.AnimationBindingRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUMGAnimationBindingRollbackTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentUMGAnimationBindingProviderTestsPrivate;
	const FAssetDocumentPrimitiveDescriptor* Add = FindAnimationBindingDescriptor(TEXT("umg.movie_scene.possessable.add.v1"));
	const FAssetDocumentPrimitiveDescriptor* Bind = FindAnimationBindingDescriptor(TEXT("umg.animation.sequence.bind.v1"));
	if (!TestNotNull(TEXT("Add descriptor."), Add)
		|| !TestNotNull(TEXT("Bind descriptor."), Bind)) return false;

	{
		FAssetDocumentUMGProviderTestFixture Fixture;
		if (!TestTrue(TEXT("Possessable rollback fixture."), Fixture.CreateTransientWidgetBlueprint())) return false;
		UWidgetAnimation* Animation = Fixture.AddFixtureAnimation(TEXT("RollbackPossessable"));
		if (!TestNotNull(TEXT("Possessable rollback animation."), Animation)) return false;
		FAssetDocumentUMGProviderTestContext Context;
		FAssetDocumentUMGProviderTestHandleResolver Handles(Context);
		FAssetDocumentUMGProviderTestTransaction Transaction(Context);
		const FString Before = Fixture.CaptureDigest();
		Transaction.SetRestoreCallback(
			[MovieScene = Animation->MovieScene]()
			{
				for (int32 Index = MovieScene->GetPossessableCount() - 1;
					Index >= 0; --Index)
				{
					const FMovieScenePossessable& Possessable =
						MovieScene->GetPossessable(Index);
					if (Possessable.GetName() == TEXT("RollbackWidget"))
					{
						MovieScene->RemovePossessable(
							Possessable.GetGuid());
					}
				}
			});
		Transaction.SetPoisonAfterStateRead(3);
		const FAssetDocumentPrimitiveOutcome Outcome = Invoke(
			*CreateAssetDocumentUMGPrimitiveProvider(), *Add,
			Context, Handles, Transaction, *Animation,
			*UBorder::StaticClass(), nullptr, nullptr,
			MakeAssetDocumentUMGObject({
				{TEXT("name"),
					MakeAssetDocumentUMGString(TEXT("RollbackWidget"))},
				{TEXT("class_path"),
					MakeAssetDocumentUMGString(
						UBorder::StaticClass()->GetPathName())}}));
		TestTrue(
			TEXT("Failure after possessable creation rolls back."),
			Outcome.Status
					== EAssetDocumentPrimitiveOutcomeStatus::Failed
				&& Transaction.GetRollbackCount() == 1);
		TestEqual(
			TEXT("Possessable rollback restores the complete fixture digest."),
			Fixture.CaptureDigest(), Before);
	}

	{
		FAssetDocumentUMGProviderTestFixture Fixture;
		if (!TestTrue(TEXT("Sequence rollback fixture."), Fixture.CreateTransientWidgetBlueprint())) return false;
		UWidgetAnimation* Animation = Fixture.AddFixtureAnimation(TEXT("RollbackSequence"));
		UUserWidget* PreviewRoot = Fixture.CreatePreviewRoot(TEXT("RollbackPreview"));
		UWidget* Widget = PreviewRoot
			? Fixture.CreatePreviewWidget(
				UBorder::StaticClass(),
				TEXT("TargetWidget"), *PreviewRoot)
			: nullptr;
		if (!TestNotNull(TEXT("Sequence rollback animation."), Animation)
			|| !TestNotNull(TEXT("Sequence rollback widget."), Widget)) return false;
		const FGuid Guid = Animation->MovieScene->AddPossessable(
			TEXT("TargetWidget"), UBorder::StaticClass());
		if (!TestTrue(TEXT("Sequence rollback possessable."), Guid.IsValid())) return false;
		FAssetDocumentUMGProviderTestContext Context;
		FAssetDocumentUMGProviderTestHandleResolver Handles(Context);
		FAssetDocumentUMGProviderTestTransaction Transaction(Context);
		const FString Before = Fixture.CaptureDigest();
		Transaction.SetRestoreCallback(
			[Animation, Guid]()
			{
				static_cast<UMovieSceneSequence&>(*Animation)
					.UnbindPossessableObjects(Guid);
			});
		Transaction.SetPoisonAfterStateRead(3);
		const FAssetDocumentPrimitiveOutcome Outcome = Invoke(
			*CreateAssetDocumentUMGPrimitiveProvider(), *Bind,
			Context, Handles, Transaction, *Animation,
			*UBorder::StaticClass(), PreviewRoot, Widget,
			MakeAssetDocumentUMGObject({
				{TEXT("possessable_guid"),
					MakeAssetDocumentUMGString(Guid.ToString())},
				{TEXT("widget_name"),
					MakeAssetDocumentUMGString(TEXT("TargetWidget"))}}));
		TestTrue(
			TEXT("Failure after sequence binding rolls back."),
			Outcome.Status
					== EAssetDocumentPrimitiveOutcomeStatus::Failed
				&& Transaction.GetRollbackCount() == 1);
		TestEqual(
			TEXT("Sequence binding rollback restores the complete fixture digest."),
			Fixture.CaptureDigest(), Before);
	}
	TestFalse(TEXT("Rejected broad apply-delta candidate is absent."), FindAnimationBindingDescriptor(TEXT("UMG-ANIMATION-BINDING-APPLY-DELTA-v1")) != nullptr);
	return true;
}

#endif
