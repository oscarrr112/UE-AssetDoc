// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// A Niagara emitter asset (a Content Browser duplicate of the Fountain template) whose sprite renderer holds a value
// with no document form (a NaN float): it reads as a marker with a warning, its own extract re-applies as a no-op,
// another Details edit applies and keeps the value, and an edited marker is refused. An emitter subscribed to a
// Parameter Definitions library reads with a warning (the document has no form for subscriptions), re-applies as a
// no-op, and keeps the subscription through another edit.

#include "AssetDocumentTolerantTestSupport.h"

#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "NiagaraEmitter.h"
#include "NiagaraParameterDefinitions.h"
#include "NiagaraSpriteRendererProperties.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantNiagaraTest, "UEAssetDoc.Tolerant.Niagara",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantNiagaraTest::RunTest(const FString& Parameters)
{
	namespace Tolerant = AssetDocumentTolerantTestSupport;
	const Tolerant::FTarget Target(TEXT("NE_Tolerant"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
	};
	UObject* Source = LoadObject<UObject>(nullptr, TEXT("/Niagara/DefaultAssets/Templates/Emitters/Fountain.Fountain"));
	if (!TestNotNull(TEXT("Fountain loads"), Source))
	{
		return false;
	}
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(AssetTools.DuplicateAsset(Target.AssetName,
		FPackageName::GetLongPackagePath(Target.PackageName), Source));
	FVersionedNiagaraEmitterData* Data = Emitter != nullptr ? Emitter->GetLatestEmitterData() : nullptr;
	if (!TestNotNull(TEXT("duplicate"), Data))
	{
		return false;
	}
	int32 RendererIndex = INDEX_NONE;
	UNiagaraSpriteRendererProperties* Sprite = nullptr;
	const TArray<UNiagaraRendererProperties*> Renderers = Data->GetRenderers();
	for (int32 Index = 0; Index < Renderers.Num() && Sprite == nullptr; ++Index)
	{
		Sprite = Cast<UNiagaraSpriteRendererProperties>(Renderers[Index]);
		RendererIndex = Index;
	}
	if (!TestNotNull(TEXT("Fountain has a sprite renderer"), Sprite))
	{
		return false;
	}
	// A value no Details panel shows a number for.
	Sprite->Modify();
	Sprite->MacroUVRadius = std::numeric_limits<float>::quiet_NaN();
	Emitter->MarkPackageDirty();
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}
	const FString Marker = FString::Printf(TEXT("/Body/Renderers/%d/Properties/MacroUVRadius"), RendererIndex);

	Tolerant::FExpected Expected;
	Expected.Warnings.Add({TEXT("Niagara.Unrepresented"), Marker});
	Expected.Markers.Add(Marker);
	if (!Tolerant::TestTolerantRoundTrip(*this, Target, Expected, TEXT("Niagara")).IsValid())
	{
		return false;
	}

	// Another Details edit applies (the marked value is left out of the write) and the value is kept.
	bool bLocalSpace = false;
	const FAssetDocumentResult Edited = Tolerant::ApplyEdited(*this, Target, [&bLocalSpace](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Body = Document->GetObjectField(TEXT("Body"));
		if (!Body->HasTypedField<EJson::Object>(TEXT("Properties")))
		{
			Body->SetObjectField(TEXT("Properties"), MakeShared<FJsonObject>());
		}
		const TSharedPtr<FJsonObject> Properties = Body->GetObjectField(TEXT("Properties"));
		bool bCurrent = false;
		Properties->TryGetBoolField(TEXT("bLocalSpace"), bCurrent);
		bLocalSpace = !bCurrent;
		Properties->SetBoolField(TEXT("bLocalSpace"), bLocalSpace);
	}, TEXT("Edit"));
	if (TestTrue(TEXT("another Details edit applies"), Edited.IsSuccess()))
	{
		Tolerant::TestStillShows(*this, Target, TEXT("Niagara.Unrepresented"), Marker, TEXT("After edit"));
		const UNiagaraEmitter* After = Target.Get<UNiagaraEmitter>();
		const FVersionedNiagaraEmitterData* AfterData = After != nullptr ? After->GetLatestEmitterData() : nullptr;
		TestTrue(TEXT("the edit is written"), AfterData != nullptr && AfterData->bLocalSpace == bLocalSpace);
		const UNiagaraSpriteRendererProperties* AfterSprite = AfterData != nullptr && AfterData->GetRenderers().IsValidIndex(RendererIndex)
			? Cast<UNiagaraSpriteRendererProperties>(AfterData->GetRenderers()[RendererIndex]) : nullptr;
		TestTrue(TEXT("the marked value is kept"), AfterSprite != nullptr && FMath::IsNaN(AfterSprite->MacroUVRadius));
	}

	const FAssetDocumentResult Refused = Tolerant::ApplyEdited(*this, Target, [&Marker](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonValue> Value = Tolerant::At(Document, Marker);
		if (Value.IsValid() && Value->Type == EJson::Object)
		{
			Value->AsObject()->SetStringField(TEXT("Text"), TEXT("1.0"));
		}
	}, TEXT("Edited marker"));
	Tolerant::TestRefused(*this, Refused, TEXT("Niagara.UnrepresentedValue"), Marker, TEXT("Edited marker"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantNiagaraDefinitionsTest, "UEAssetDoc.Tolerant.NiagaraParameterDefinitions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantNiagaraDefinitionsTest::RunTest(const FString& Parameters)
{
	namespace Tolerant = AssetDocumentTolerantTestSupport;
	const Tolerant::FTarget Library(TEXT("NPD_Tolerant"));
	const Tolerant::FTarget Target(TEXT("NE_TolerantDefs"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
		Library.Cleanup();
	};
	UNiagaraParameterDefinitions* Definitions = Library.Create<UNiagaraParameterDefinitions>();
	UObject* Source = LoadObject<UObject>(nullptr, TEXT("/Niagara/DefaultAssets/Templates/Emitters/Fountain.Fountain"));
	if (!TestNotNull(TEXT("definitions"), Definitions) || !TestNotNull(TEXT("Fountain loads"), Source) || !TestTrue(TEXT("library saved"), Library.Save()))
	{
		return false;
	}
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(AssetTools.DuplicateAsset(Target.AssetName,
		FPackageName::GetLongPackagePath(Target.PackageName), Source));
	if (!TestNotNull(TEXT("duplicate"), Emitter))
	{
		return false;
	}
	// What the emitter editor's Parameters panel does when a library is added to the emitter.
	Emitter->Modify();
	Emitter->SubscribeToParameterDefinitions(Definitions);
	Emitter->MarkPackageDirty();
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	Tolerant::FExpected Expected;
	Expected.Warnings.Add({TEXT("Niagara.UnrepresentedParameterDefinitions"), TEXT("/Body")});
	if (!Tolerant::TestTolerantRoundTrip(*this, Target, Expected, TEXT("NiagaraDefinitions")).IsValid())
	{
		return false;
	}

	const FAssetDocumentResult Edited = Tolerant::ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Body = Document->GetObjectField(TEXT("Body"));
		if (!Body->HasTypedField<EJson::Object>(TEXT("Properties")))
		{
			Body->SetObjectField(TEXT("Properties"), MakeShared<FJsonObject>());
		}
		const TSharedPtr<FJsonObject> Properties = Body->GetObjectField(TEXT("Properties"));
		bool bCurrent = false;
		Properties->TryGetBoolField(TEXT("bLocalSpace"), bCurrent);
		Properties->SetBoolField(TEXT("bLocalSpace"), !bCurrent);
	}, TEXT("Edit"));
	if (TestTrue(TEXT("another Details edit applies"), Edited.IsSuccess()))
	{
		Tolerant::TestStillShows(*this, Target, TEXT("Niagara.UnrepresentedParameterDefinitions"), TEXT("/Body"), TEXT("After edit"));
		const UNiagaraEmitter* After = Target.Get<UNiagaraEmitter>();
		const UNiagaraParameterDefinitions* Kept = Library.Get<UNiagaraParameterDefinitions>();
		TestTrue(TEXT("the subscription is kept"), After != nullptr && Kept != nullptr
			&& After->GetSubscribedParameterDefinitions().Contains(Kept));
	}
	return !HasAnyErrors();
}

#endif
