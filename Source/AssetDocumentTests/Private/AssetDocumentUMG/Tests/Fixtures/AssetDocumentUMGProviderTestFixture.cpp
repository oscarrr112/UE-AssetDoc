// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentUMGProviderTestFixture.h"
#include "AssetDocumentUMGProviderTestTypes.h"

#include "Animation/WidgetAnimation.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "WidgetBlueprint.h"
#include "Components/Widget.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

FAssetDocumentUMGProviderTestFixture::~FAssetDocumentUMGProviderTestFixture()
{
	DestroyTransientWidgetBlueprint();
}

bool FAssetDocumentUMGProviderTestFixture::CreateTransientWidgetBlueprint()
{
	if (WidgetBlueprint != nullptr)
	{
		return false;
	}
	FixtureName = FString::Printf(
		TEXT("AssetDocumentUMGProviderTest_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	WidgetBlueprint = NewObject<UWidgetBlueprint>(
		GetTransientPackage(), FName(*FixtureName), RF_Transient);
	if (WidgetBlueprint == nullptr)
	{
		return false;
	}
#if WITH_EDITORONLY_DATA
	WidgetBlueprint->WidgetTree = NewObject<UWidgetTree>(
		WidgetBlueprint, TEXT("WidgetTree"), RF_Transactional);
#endif
	return WidgetBlueprint->WidgetTree != nullptr;
}

void FAssetDocumentUMGProviderTestFixture::DestroyTransientWidgetBlueprint()
{
	if (WidgetBlueprint != nullptr)
	{
		WidgetBlueprint->MarkAsGarbage();
		WidgetBlueprint = nullptr;
	}
	StrongTransientObjects.Reset();
}

UWidgetBlueprint* FAssetDocumentUMGProviderTestFixture::GetWidgetBlueprint() const
{
	return WidgetBlueprint.Get();
}

UWidget* FAssetDocumentUMGProviderTestFixture::AddFixtureWidget(
	UClass* WidgetClass,
	FName Name)
{
	if (WidgetBlueprint == nullptr
		|| WidgetBlueprint->WidgetTree == nullptr
		|| WidgetClass == nullptr
		|| !WidgetClass->IsChildOf(UWidget::StaticClass()))
	{
		return nullptr;
	}
	UWidget* Widget =
		WidgetBlueprint->WidgetTree->ConstructWidget<UWidget>(
			WidgetClass, Name);
	if (Widget != nullptr
		&& WidgetBlueprint->WidgetTree->RootWidget == nullptr)
	{
		WidgetBlueprint->WidgetTree->RootWidget = Widget;
	}
	return Widget;
}

UWidgetAnimation*
FAssetDocumentUMGProviderTestFixture::AddFixtureAnimation(FName Name)
{
	if (WidgetBlueprint == nullptr)
	{
		return nullptr;
	}
	UWidgetAnimation* Animation =
		NewObject<UWidgetAnimation>(
			WidgetBlueprint, Name, RF_Transactional);
	UMovieScene* MovieScene =
		Animation != nullptr
			? NewObject<UMovieScene>(
				Animation, TEXT("MovieScene"), RF_Transactional)
			: nullptr;
	if (Animation == nullptr || MovieScene == nullptr)
	{
		return nullptr;
	}
	Animation->MovieScene = MovieScene;
	WidgetBlueprint->Animations.Add(Animation);
	return Animation;
}

UUserWidget* FAssetDocumentUMGProviderTestFixture::CreatePreviewRoot(
	FName Name)
{
	UUserWidget* PreviewRoot =
		NewObject<UAssetDocumentUMGProviderTestUserWidget>(
			GetTransientPackage(), Name, RF_Transient);
	if (PreviewRoot != nullptr)
	{
		PreviewRoot->WidgetTree = NewObject<UWidgetTree>(
			PreviewRoot, TEXT("WidgetTree"), RF_Transient);
		StrongTransientObjects.Add(PreviewRoot);
	}
	return PreviewRoot;
}

UWidget* FAssetDocumentUMGProviderTestFixture::CreatePreviewWidget(
	UClass* WidgetClass,
	FName Name,
	UUserWidget& PreviewRoot)
{
	if (WidgetClass == nullptr
		|| !WidgetClass->IsChildOf(UWidget::StaticClass()))
	{
		return nullptr;
	}
	if (PreviewRoot.WidgetTree == nullptr)
	{
		return nullptr;
	}
	UWidget* Widget =
		PreviewRoot.WidgetTree->ConstructWidget<UWidget>(
			WidgetClass, Name);
	if (Widget != nullptr)
	{
		if (PreviewRoot.WidgetTree->RootWidget == nullptr)
		{
			PreviewRoot.WidgetTree->RootWidget = Widget;
		}
		StrongTransientObjects.Add(Widget);
	}
	return Widget;
}

FString FAssetDocumentUMGProviderTestFixture::CaptureDigest() const
{
	if (WidgetBlueprint == nullptr)
	{
		return FixtureName + TEXT("|absent");
	}
	TArray<FString> Facts;
	Facts.Add(FixtureName);
	Facts.Add(WidgetBlueprint->GetPathName());
	Facts.Add(WidgetBlueprint->GetOutermost()->IsDirty()
		? TEXT("dirty") : TEXT("clean"));
	if (WidgetBlueprint->WidgetTree != nullptr)
	{
		TArray<UWidget*> Widgets;
		WidgetBlueprint->WidgetTree->GetAllWidgets(Widgets);
		for (UWidget* Widget : Widgets)
		{
			Facts.Add(FString::Printf(
				TEXT("widget:%s:%s:%s"),
				*Widget->GetName(),
				*Widget->GetClass()->GetPathName(),
				Widget->GetParent()
					? *Widget->GetParent()->GetName()
					: TEXT("none")));
		}
	}
	for (UWidgetAnimation* Animation : WidgetBlueprint->Animations)
	{
		if (Animation == nullptr)
		{
			continue;
		}
		UMovieSceneSequence& Sequence = *Animation;
		UMovieScene* MovieScene = Sequence.GetMovieScene();
		Facts.Add(FString::Printf(
			TEXT("animation:%s:%d:%d:%d"),
			*Animation->GetName(),
			Animation->GetBindings().Num(),
			MovieScene ? MovieScene->GetPossessableCount() : -1,
			MovieScene
				? static_cast<const UMovieScene*>(
					MovieScene)->GetBindings().Num()
				: -1));
		for (const FWidgetAnimationBinding& Binding :
			Animation->GetBindings())
		{
			Facts.Add(FString::Printf(
				TEXT("animation-binding:%s:%s:%s"),
				*Binding.AnimationGuid.ToString(
					EGuidFormats::DigitsWithHyphens),
				*Binding.WidgetName.ToString(),
				*Binding.SlotWidgetName.ToString()));
		}
		if (MovieScene == nullptr)
		{
			continue;
		}
		for (int32 PossessableIndex = 0;
			PossessableIndex < MovieScene->GetPossessableCount();
			++PossessableIndex)
		{
			const FMovieScenePossessable& Possessable =
				MovieScene->GetPossessable(PossessableIndex);
			Facts.Add(FString::Printf(
				TEXT("possessable:%s:%s:%s"),
				*Possessable.GetGuid().ToString(
					EGuidFormats::DigitsWithHyphens),
				*Possessable.GetName(),
				Possessable.GetPossessedObjectClass()
					? *Possessable.GetPossessedObjectClass()
						->GetPathName()
					: TEXT("none")));
		}
		for (const FMovieSceneBinding& SceneBinding :
			static_cast<const UMovieScene*>(
				MovieScene)->GetBindings())
		{
			Facts.Add(FString::Printf(
				TEXT("scene-binding:%s:%d"),
				*SceneBinding.GetObjectGuid().ToString(
					EGuidFormats::DigitsWithHyphens),
				SceneBinding.GetTracks().Num()));
			for (UMovieSceneTrack* Track :
				SceneBinding.GetTracks())
			{
				if (Track == nullptr)
				{
					Facts.Add(TEXT("track:null"));
					continue;
				}
				const UMovieScenePropertyTrack* PropertyTrack =
					Cast<UMovieScenePropertyTrack>(Track);
				Facts.Add(FString::Printf(
					TEXT("track:%s:%s:%s:%d"),
					*Track->GetClass()->GetPathName(),
					PropertyTrack
						? *PropertyTrack->GetPropertyName()
							.ToString()
						: TEXT("none"),
					PropertyTrack
						? *PropertyTrack->GetPropertyPath()
							.ToString()
						: TEXT("none"),
					Track->GetAllSections().Num()));
				for (UMovieSceneSection* Section :
					Track->GetAllSections())
				{
					Facts.Add(FString::Printf(
						TEXT("section:%s:%s"),
						Section
							? *Section->GetClass()
								->GetPathName()
							: TEXT("none"),
						Section
							? *Section->GetName()
							: TEXT("none")));
				}
			}
		}
	}
	Facts.Sort();
	return FString::Join(Facts, TEXT("|"));
}
