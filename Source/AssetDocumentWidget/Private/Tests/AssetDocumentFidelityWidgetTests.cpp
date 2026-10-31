// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// An editor-made Widget Blueprint whose animation keys the user widget itself (a deactivated track with a bounded
// section), a material
// parameter of an Image's brush, and an event track: extract shows them (the event keys read-only), the extract
// re-applies as a no-op, and an edit applies as the Sequencer would.

#include "AssetDocumentTolerantTestSupport.h"

#include "AssetDocumentWidgetPrimitives.h"
#include "AssetDocumentWidgetRawEdits.h"

#include "Animation/MovieSceneWidgetMaterialTrack.h"
#include "Animation/WidgetAnimation.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetNavigation.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Channels/MovieSceneEventChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Components/CanvasPanel.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/ScopeExit.h"
#include "MovieScene.h"
#include "Sections/MovieSceneEventTriggerSection.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Sections/MovieSceneParameterSection.h"
#include "StructUtils/InstancedStruct.h"
#include "Tracks/MovieSceneEventTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "UIComponentWidgetBlueprintExtension.h"
#include "WidgetBlueprint.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentFidelityWidgetTests
{
// The Body.Animations[Animation].Tracks entry whose Type is Type (nullptr if none).
TSharedPtr<FJsonObject> FindTrackRow(const TSharedPtr<FJsonObject>& Document, const FString& Animation, const FString& Type)
{
	const TSharedPtr<FJsonObject>* Body = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Animations = nullptr;
	if (!Document.IsValid() || !Document->TryGetObjectField(TEXT("Body"), Body) || !(*Body)->TryGetArrayField(TEXT("Animations"), Animations))
	{
		return nullptr;
	}
	for (const TSharedPtr<FJsonValue>& Row : *Animations)
	{
		const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
		if (Row->AsObject()->GetStringField(TEXT("Name")) != Animation || !Row->AsObject()->TryGetArrayField(TEXT("Tracks"), Tracks))
		{
			continue;
		}
		for (const TSharedPtr<FJsonValue>& Track : *Tracks)
		{
			if (Track->AsObject()->GetStringField(TEXT("Type")) == Type)
			{
				return Track->AsObject();
			}
		}
	}
	return nullptr;
}

FFrameNumber FidelityWidgetTicks(const UMovieScene& MovieScene, int32 Frame)
{
	return FFrameNumber(UE::AssetDocument::Graph::Widget::DisplayToTicks(Frame, MovieScene.GetTickResolution(), MovieScene.GetDisplayRate()));
}

FGuid FidelityWidgetBind(UWidgetAnimation& Animation, UObject& Object, FName WidgetName, bool bRoot)
{
	const FGuid Guid = Animation.GetMovieScene()->AddPossessable(Object.GetName(), Object.GetClass());
	FWidgetAnimationBinding Binding;
	Binding.AnimationGuid = Guid;
	Binding.WidgetName = WidgetName;
	Binding.bIsRootWidget = bRoot;
	Animation.AnimationBindings.Add(Binding);
	return Guid;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelityWidgetAnimationTracksTest, "UEAssetDoc.Fidelity.Widget.AnimationTracks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentFidelityWidgetAnimationTracksTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentFidelityWidgetTests;
	const FTarget Target(TEXT("WBP_AnimationTracks"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	UWidgetBlueprint* Blueprint = CastChecked<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(UUserWidget::StaticClass(),
		CreatePackage(*Target.PackageName), FName(*Target.AssetName), BPTYPE_Normal, UWidgetBlueprint::StaticClass(),
		UWidgetBlueprintGeneratedClass::StaticClass()));
	FAssetRegistryModule::AssetCreated(Blueprint);
	UCanvasPanel* Root = Blueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
	Blueprint->WidgetTree->RootWidget = Root;
	UImage* Glow = Blueprint->WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("Glow"));
	Root->AddChild(Glow);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	UWidgetAnimation* Animation = UE::AssetDocument::Graph::Widget::Raw::CreateAnimation(*Blueprint, TEXT("Pulse"));
	if (!TestNotNull(TEXT("animation created"), Animation))
	{
		return false;
	}
	UMovieScene& MovieScene = *Animation->GetMovieScene();

	// The user widget's Render Opacity, deactivated in the Sequencer.
	const FGuid RootGuid = FidelityWidgetBind(*Animation, *Blueprint->GeneratedClass->GetDefaultObject(), Blueprint->GetFName(), true);
	UMovieSceneFloatTrack* Opacity = MovieScene.AddTrack<UMovieSceneFloatTrack>(RootGuid);
	Opacity->SetPropertyNameAndPath(TEXT("RenderOpacity"), TEXT("RenderOpacity"));
	UMovieSceneFloatSection* OpacitySection = CastChecked<UMovieSceneFloatSection>(Opacity->CreateNewSection());
	OpacitySection->SetRange(TRange<FFrameNumber>(TRangeBound<FFrameNumber>::Inclusive(FidelityWidgetTicks(MovieScene, 0)),
		TRangeBound<FFrameNumber>::Exclusive(FidelityWidgetTicks(MovieScene, 20))));
	Opacity->AddSection(*OpacitySection);
	OpacitySection->GetChannel().GetData().AddKey(FidelityWidgetTicks(MovieScene, 0), FMovieSceneFloatValue(1.0f));
	OpacitySection->GetChannel().GetData().AddKey(FidelityWidgetTicks(MovieScene, 20), FMovieSceneFloatValue(0.0f));
	Opacity->SetEvalDisabled(true);

	// A scalar parameter of Glow's brush material (FWidgetBlueprintEditor::AddMaterialTrack, then a key).
	const FGuid GlowGuid = FidelityWidgetBind(*Animation, *Glow, Glow->GetFName(), false);
	UMovieSceneWidgetMaterialTrack* Material = MovieScene.AddTrack<UMovieSceneWidgetMaterialTrack>(GlowGuid);
	Material->SetBrushPropertyNamePath({TEXT("Brush")});
	UMovieSceneParameterSection* MaterialSection = CastChecked<UMovieSceneParameterSection>(Material->CreateNewSection());
	MaterialSection->SetRange(TRange<FFrameNumber>::All());
	Material->AddSection(*MaterialSection);
	MaterialSection->AddScalarParameterKey(TEXT("Pulse"), FidelityWidgetTicks(MovieScene, 0), 0.0f);
	MaterialSection->AddScalarParameterKey(TEXT("Pulse"), FidelityWidgetTicks(MovieScene, 10), 1.0f);

	// An event track on the animation itself, with one key.
	UMovieSceneEventTrack* Events = MovieScene.AddTrack<UMovieSceneEventTrack>();
	UMovieSceneEventTriggerSection* EventSection = CastChecked<UMovieSceneEventTriggerSection>(Events->CreateNewSection());
	EventSection->SetRange(TRange<FFrameNumber>::All());
	Events->AddSection(*EventSection);
	EventSection->EventChannel.GetData().AddKey(FidelityWidgetTicks(MovieScene, 15), FMovieSceneEvent());
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Animation tracks"));
	const TSharedPtr<FJsonObject> RootRow = FindTrackRow(Extracted, TEXT("Pulse"), TEXT("Float"));
	const TSharedPtr<FJsonObject> MaterialRow = FindTrackRow(Extracted, TEXT("Pulse"), TEXT("Material"));
	const TSharedPtr<FJsonObject> EventRow = FindTrackRow(Extracted, TEXT("Pulse"), UMovieSceneEventTrack::StaticClass()->GetPathName());
	if (!TestTrue(TEXT("root track extracted"), RootRow.IsValid()) || !TestTrue(TEXT("material track extracted"), MaterialRow.IsValid())
		|| !TestTrue(TEXT("event track extracted"), EventRow.IsValid()))
	{
		return false;
	}
	bool bFlag = false;
	TestEqual(TEXT("root target"), RootRow->GetStringField(TEXT("Target")), FString(TEXT("Root")));
	TestTrue(TEXT("root track deactivated"), RootRow->TryGetBoolField(TEXT("Deactivated"), bFlag) && bFlag);
	TestFalse(TEXT("root track is editable"), RootRow->HasField(TEXT("Unsupported")));
	const TSharedPtr<FJsonObject>* SectionRow = nullptr;
	TestTrue(TEXT("section range extracted"), RootRow->TryGetObjectField(TEXT("Section"), SectionRow)
		&& (*SectionRow)->GetNumberField(TEXT("StartFrame")) == 0 && (*SectionRow)->GetNumberField(TEXT("EndFrame")) == 20);
	const TSharedPtr<FJsonObject>* ParametersRow = nullptr;
	TestTrue(TEXT("material parameters extracted"), MaterialRow->TryGetObjectField(TEXT("Parameters"), ParametersRow)
		&& (*ParametersRow)->GetStringField(TEXT("Pulse")) == TEXT("Scalar"));
	TestTrue(TEXT("event track read-only"), EventRow->HasField(TEXT("Unsupported")) && EventRow->HasTypedField<EJson::Array>(TEXT("Sections")));

	// Reactivate the root track and add a key past its section (Section left out: it grows); key the brush's Tint.
	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		if (const TSharedPtr<FJsonObject> Row = FindTrackRow(Document, TEXT("Pulse"), TEXT("Float")))
		{
			Row->RemoveField(TEXT("Deactivated"));
			Row->RemoveField(TEXT("Section"));
			const TArray<TSharedPtr<FJsonValue>>* Channels = nullptr;
			if (Row->TryGetArrayField(TEXT("Channels"), Channels) && Channels->Num() == 1)
			{
				const TSharedPtr<FJsonObject> Channel = (*Channels)[0]->AsObject();
				TArray<TSharedPtr<FJsonValue>> Keys = Channel->GetArrayField(TEXT("Keys"));
				const TSharedPtr<FJsonObject> Key = MakeShared<FJsonObject>();
				Key->SetNumberField(TEXT("Frame"), 30);
				Key->SetNumberField(TEXT("Value"), 0.5);
				Keys.Add(MakeShared<FJsonValueObject>(Key));
				Channel->SetArrayField(TEXT("Keys"), Keys);
			}
		}
		if (const TSharedPtr<FJsonObject> Row = FindTrackRow(Document, TEXT("Pulse"), TEXT("Material")))
		{
			Row->GetObjectField(TEXT("Parameters"))->SetStringField(TEXT("Tint"), TEXT("Color"));
			TArray<TSharedPtr<FJsonValue>> Channels = Row->GetArrayField(TEXT("Channels"));
			const TSharedPtr<FJsonObject> Key = MakeShared<FJsonObject>();
			Key->SetNumberField(TEXT("Frame"), 5);
			Key->SetNumberField(TEXT("Value"), 0.25);
			const TSharedPtr<FJsonObject> Channel = MakeShared<FJsonObject>();
			Channel->SetStringField(TEXT("Name"), TEXT("TintR"));
			TArray<TSharedPtr<FJsonValue>> TintKeys;
			TintKeys.Add(MakeShared<FJsonValueObject>(Key));
			Channel->SetArrayField(TEXT("Keys"), TintKeys);
			Channels.Add(MakeShared<FJsonValueObject>(Channel));
			Row->SetArrayField(TEXT("Channels"), Channels);
		}
	}, TEXT("Edit animation tracks"));
	if (TestTrue(TEXT("edit applies"), Edited.IsSuccess()))
	{
		UWidgetBlueprint* After = Target.Get<UWidgetBlueprint>();
		UWidgetAnimation* AfterAnimation = nullptr;
		for (UWidgetAnimation* Candidate : After != nullptr ? After->Animations : TArray<TObjectPtr<UWidgetAnimation>>())
		{
			AfterAnimation = Candidate != nullptr && Candidate->GetName() == TEXT("Pulse") ? Candidate : AfterAnimation;
		}
		if (TestNotNull(TEXT("animation after"), AfterAnimation))
		{
			bool bRootActive = false;
			bool bTint = false;
			for (const FMovieSceneBinding& Binding : static_cast<const UMovieScene*>(AfterAnimation->GetMovieScene())->GetBindings())
			{
				for (UMovieSceneTrack* Track : Binding.GetTracks())
				{
					if (Track->IsA<UMovieSceneFloatTrack>())
					{
						bRootActive = !Track->IsEvalDisabled(false);
						const TRange<FFrameNumber> Range = Track->GetAllSections()[0]->GetRange();
						TestTrue(TEXT("section grew to the new key"), Range.Contains(FidelityWidgetTicks(*AfterAnimation->GetMovieScene(), 30)));
						TestTrue(TEXT("section start kept"), Range.HasLowerBound()
							&& Range.GetLowerBoundValue() == FidelityWidgetTicks(*AfterAnimation->GetMovieScene(), 0));
					}
					if (const UMovieSceneWidgetMaterialTrack* AfterMaterial = Cast<UMovieSceneWidgetMaterialTrack>(Track))
					{
						const UMovieSceneParameterSection* Section = Cast<UMovieSceneParameterSection>(AfterMaterial->GetAllSections()[0]);
						bTint = Section != nullptr && Section->GetColorParameterNamesAndCurves().ContainsByPredicate(
							[](const FColorParameterNameAndCurves& Item) { return Item.ParameterName == TEXT("Tint"); });
					}
				}
			}
			TestTrue(TEXT("root track reactivated"), bRootActive);
			TestTrue(TEXT("Tint parameter keyed"), bTint);
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelityWidgetExtensionsTest, "UEAssetDoc.Fidelity.Widget.Extensions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// A Widget Blueprint with a UI Component added to a widget (the Details panel's Add Component): extract warns that the
// editor extension holding it has no place in the document, and an apply keeps it.
bool FAssetDocumentFidelityWidgetExtensionsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	const FTarget Target(TEXT("WBP_Extensions"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	UWidgetBlueprint* Blueprint = CastChecked<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(UUserWidget::StaticClass(),
		CreatePackage(*Target.PackageName), FName(*Target.AssetName), BPTYPE_Normal, UWidgetBlueprint::StaticClass(),
		UWidgetBlueprintGeneratedClass::StaticClass()));
	FAssetRegistryModule::AssetCreated(Blueprint);
	UCanvasPanel* Root = Blueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
	Blueprint->WidgetTree->RootWidget = Root;
	UImage* Icon = Blueprint->WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("Icon"));
	Root->AddChild(Icon);
	const UClass* ComponentClass = FindObject<UClass>(nullptr, TEXT("/Script/UMG.SizeBoxComponent"));
	UUIComponentWidgetBlueprintExtension* Extension = UWidgetBlueprintExtension::RequestExtension<UUIComponentWidgetBlueprintExtension>(Blueprint);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	FText AddError;
	auto AddComponent = [&]() { return Extension->AddComponent(ComponentClass, Icon->GetFName(), AddError); };
#else
	auto AddComponent = [&]() { return Extension->AddComponent(ComponentClass, Icon->GetFName()); };
#endif
	if (!TestNotNull(TEXT("component class"), ComponentClass) || !TestNotNull(TEXT("extension"), Extension)
		|| !TestNotNull(TEXT("component added"), AddComponent()))
	{
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	FExpected Expected;
	Expected.Warnings.Add({TEXT("UBlueprint.Unrepresented"), TEXT("/Body")});
	TestTolerantRoundTrip(*this, Target, Expected, TEXT("Widget extensions"));
	const UWidgetBlueprint* Applied = Target.Get<UWidgetBlueprint>();
	const UUIComponentWidgetBlueprintExtension* Kept = Applied != nullptr
		? UWidgetBlueprintExtension::GetExtension<UUIComponentWidgetBlueprintExtension>(Applied) : nullptr;
	TestTrue(TEXT("component kept"), Kept != nullptr && Kept->GetComponent(ComponentClass, TEXT("Icon")) != nullptr);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelityWidgetNavigationMethodTest, "UEAssetDoc.Fidelity.Widget.NavigationMethod",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// A widget whose Details set Navigation > Navigation Method to Proximity with a narrower search angle: extract states
// the method and the changed value; an edit changes another value, and null clears the method.
bool FAssetDocumentFidelityWidgetNavigationMethodTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	const FTarget Target(TEXT("WBP_NavigationMethod"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	const TCHAR* const ProximityPath = TEXT("/Script/SlateCore.NavigationMethodProximity");
	UWidgetBlueprint* Blueprint = CastChecked<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(UUserWidget::StaticClass(),
		CreatePackage(*Target.PackageName), FName(*Target.AssetName), BPTYPE_Normal, UWidgetBlueprint::StaticClass(),
		UWidgetBlueprintGeneratedClass::StaticClass()));
	FAssetRegistryModule::AssetCreated(Blueprint);
	UCanvasPanel* Root = Blueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
	Blueprint->WidgetTree->RootWidget = Root;
	UImage* Icon = Blueprint->WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("Icon"));
	Root->AddChild(Icon);
	const UScriptStruct* Proximity = FindObject<UScriptStruct>(nullptr, ProximityPath);
	const FStructProperty* MethodProperty = FindFProperty<FStructProperty>(UWidgetNavigation::StaticClass(), TEXT("NavigationMethod"));
	const FFloatProperty* AngleProperty = Proximity != nullptr
		? CastField<FFloatProperty>(Proximity->FindPropertyByName(TEXT("SearchAngleDegrees"))) : nullptr;
	if (!TestNotNull(TEXT("proximity method"), Proximity) || !TestNotNull(TEXT("method property"), MethodProperty)
		|| !TestNotNull(TEXT("angle property"), AngleProperty))
	{
		return false;
	}
	// The Details panel's writes: the Navigation Method combo, then the method's Search Angle.
	Icon->Navigation = NewObject<UWidgetNavigation>(Icon);
	FInstancedStruct& Method = *MethodProperty->ContainerPtrToValuePtr<FInstancedStruct>(Icon->Navigation.Get());
	Method.InitializeAs(Proximity);
	AngleProperty->SetPropertyValue_InContainer(Method.GetMutableMemory(), 30.0f);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	auto FindMethod = [](const TSharedPtr<FJsonObject>& Document) -> TSharedPtr<FJsonObject>
	{
		const TSharedPtr<FJsonObject>* Body = nullptr;
		const TSharedPtr<FJsonObject>* Tree = nullptr;
		const TSharedPtr<FJsonObject>* RootRow = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
		const TSharedPtr<FJsonObject>* Navigation = nullptr;
		const TSharedPtr<FJsonObject>* Found = nullptr;
		if (Document.IsValid() && Document->TryGetObjectField(TEXT("Body"), Body) && (*Body)->TryGetObjectField(TEXT("WidgetTree"), Tree)
			&& (*Tree)->TryGetObjectField(TEXT("RootWidget"), RootRow) && (*RootRow)->TryGetArrayField(TEXT("Children"), Children)
			&& Children->Num() == 1 && (*Children)[0]->AsObject()->TryGetObjectField(TEXT("Navigation"), Navigation)
			&& (*Navigation)->TryGetObjectField(TEXT("NavigationMethod"), Found))
		{
			return *Found;
		}
		return nullptr;
	};
	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Navigation method"));
	const TSharedPtr<FJsonObject> Row = FindMethod(Extracted);
	if (!TestTrue(TEXT("navigation method extracted"), Row.IsValid()))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Values = nullptr;
	double Angle = 0.0;
	TestEqual(TEXT("method struct"), Row->GetStringField(TEXT("Struct")), FString(ProximityPath));
	TestTrue(TEXT("changed value extracted"), Row->TryGetObjectField(TEXT("Properties"), Values)
		&& (*Values)->TryGetNumberField(TEXT("SearchAngleDegrees"), Angle) && FMath::IsNearlyEqual(Angle, 30.0));
	TestTrue(TEXT("unchanged values not stated"), Values != nullptr && !(*Values)->HasField(TEXT("AlignmentFactor")));

	auto CurrentMethod = [&Target, MethodProperty]() -> const FInstancedStruct*
	{
		const UWidgetBlueprint* Applied = Target.Get<UWidgetBlueprint>();
		const UWidget* Widget = Applied != nullptr ? Applied->WidgetTree->FindWidget(TEXT("Icon")) : nullptr;
		return Widget != nullptr && Widget->Navigation != nullptr
			? MethodProperty->ContainerPtrToValuePtr<FInstancedStruct>(Widget->Navigation.Get()) : nullptr;
	};
	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [&FindMethod](const TSharedPtr<FJsonObject>& Document)
	{
		if (const TSharedPtr<FJsonObject> Found = FindMethod(Document))
		{
			Found->GetObjectField(TEXT("Properties"))->SetNumberField(TEXT("AlignmentFactor"), 0.5);
		}
	}, TEXT("Edit navigation method"));
	if (TestTrue(TEXT("edit applies"), Edited.IsSuccess()))
	{
		const FInstancedStruct* Current = CurrentMethod();
		const FFloatProperty* AlignmentProperty = CastField<FFloatProperty>(Proximity->FindPropertyByName(TEXT("AlignmentFactor")));
		TestTrue(TEXT("method kept with both values"), Current != nullptr && Current->GetScriptStruct() == Proximity && AlignmentProperty != nullptr
			&& FMath::IsNearlyEqual(AlignmentProperty->GetPropertyValue_InContainer(Current->GetMemory()), 0.5f)
			&& FMath::IsNearlyEqual(AngleProperty->GetPropertyValue_InContainer(Current->GetMemory()), 30.0f));
	}
	const FAssetDocumentResult Cleared = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Child = Document->GetObjectField(TEXT("Body"))->GetObjectField(TEXT("WidgetTree"))
			->GetObjectField(TEXT("RootWidget"))->GetArrayField(TEXT("Children"))[0]->AsObject();
		Child->GetObjectField(TEXT("Navigation"))->SetField(TEXT("NavigationMethod"), MakeShared<FJsonValueNull>());
	}, TEXT("Clear navigation method"));
	if (TestTrue(TEXT("clear applies"), Cleared.IsSuccess()))
	{
		const FInstancedStruct* Current = CurrentMethod();
		TestTrue(TEXT("method cleared"), Current == nullptr || !Current->IsValid());
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelityWidgetFunctionPathBindingTest, "UEAssetDoc.Fidelity.Widget.FunctionPathBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// A text bound through the Bind menu to "Title > GetText" (a pure function at the end of the path): extract shows the
// path, the extract re-applies as a no-op, and an edit retargets it to "Caption > GetText".
bool FAssetDocumentFidelityWidgetFunctionPathBindingTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	const FTarget Target(TEXT("WBP_FunctionPathBinding"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	UWidgetBlueprint* Blueprint = CastChecked<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(UUserWidget::StaticClass(),
		CreatePackage(*Target.PackageName), FName(*Target.AssetName), BPTYPE_Normal, UWidgetBlueprint::StaticClass(),
		UWidgetBlueprintGeneratedClass::StaticClass()));
	FAssetRegistryModule::AssetCreated(Blueprint);
	UCanvasPanel* Root = Blueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
	Blueprint->WidgetTree->RootWidget = Root;
	for (const TCHAR* Name : {TEXT("Title"), TEXT("Caption"), TEXT("Subtitle")})
	{
		UTextBlock* Text = Blueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), Name);
		Text->bIsVariable = true;
		Root->AddChild(Text);
	}
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	FProperty* Title = FindFProperty<FProperty>(Blueprint->SkeletonGeneratedClass, TEXT("Title"));
	UFunction* GetText = UTextBlock::StaticClass()->FindFunctionByName(TEXT("GetText"));
	if (!TestNotNull(TEXT("Title variable"), Title) || !TestNotNull(TEXT("GetText"), GetText))
	{
		return false;
	}
	// SPropertyBinding::HandleAddBinding: the chain of the picked members.
	FDelegateEditorBinding Binding;
	Binding.ObjectName = TEXT("Subtitle");
	Binding.PropertyName = TEXT("Text");
	Binding.Kind = EBindingKind::Property;
	Binding.SourcePath = FEditorPropertyPath(TArray<FFieldVariant>{FFieldVariant(Title), FFieldVariant(GetText)});
	Blueprint->Bindings.Add(Binding);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	auto FindBinding = [](const TSharedPtr<FJsonObject>& Document) -> TSharedPtr<FJsonObject>
	{
		const TSharedPtr<FJsonObject>* Body = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
		return Document.IsValid() && Document->TryGetObjectField(TEXT("Body"), Body) && (*Body)->TryGetArrayField(TEXT("Bindings"), Bindings)
			&& Bindings->Num() == 1 ? (*Bindings)[0]->AsObject() : nullptr;
	};
	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Function path binding"));
	const TSharedPtr<FJsonObject> Row = FindBinding(Extracted);
	const TArray<TSharedPtr<FJsonValue>>* Path = nullptr;
	if (!TestTrue(TEXT("binding extracted"), Row.IsValid() && Row->TryGetArrayField(TEXT("SourcePath"), Path)))
	{
		return false;
	}
	TestTrue(TEXT("path with its function"), Path->Num() == 2 && (*Path)[0]->AsString() == TEXT("Title") && (*Path)[1]->AsString() == TEXT("GetText"));

	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [&FindBinding](const TSharedPtr<FJsonObject>& Document)
	{
		if (const TSharedPtr<FJsonObject> Found = FindBinding(Document))
		{
			Found->SetArrayField(TEXT("SourcePath"), TArray<TSharedPtr<FJsonValue>>{
				MakeShared<FJsonValueString>(TEXT("Caption")), MakeShared<FJsonValueString>(TEXT("GetText"))});
		}
	}, TEXT("Retarget function path binding"));
	if (TestTrue(TEXT("edit applies"), Edited.IsSuccess()))
	{
		const UWidgetBlueprint* Applied = Target.Get<UWidgetBlueprint>();
		const FDelegateEditorBinding* Current = Applied != nullptr && Applied->Bindings.Num() == 1 ? &Applied->Bindings[0] : nullptr;
		TestTrue(TEXT("retargeted"), Current != nullptr && Current->SourcePath.Segments.Num() == 2
			&& Current->SourcePath.Segments[0].GetMemberName() == TEXT("Caption") && Current->SourcePath.Segments[1].GetMemberName() == TEXT("GetText"));
	}
	return !HasAnyErrors();
}

#endif
