// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// An editor-made Material Instance whose parent exposes two parameters named Mask, a Scalar Parameter and a Texture
// Sample Parameter 2D (the Material Editor keeps parameter names unique per parameter class only), both overridden as
// the Material Instance Editor overrides them. Extract keys them Mask@scalar and Mask@texture, the extract re-applies
// as a no-op, an edit of one keeps the other, a plain Mask whose value only one kind takes edits that kind, and a
// plain Mask that fits both (null) is refused as ambiguous.

#include "AssetDocumentTolerantTestSupport.h"

#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Texture.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
const TCHAR* const KindKeyDefaultTexture = TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture");
const TCHAR* const KindKeyWhiteTexture = TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture");

TSharedPtr<FJsonObject> KindKeyParameters(const TSharedPtr<FJsonObject>& Document)
{
	const TSharedPtr<FJsonObject>* Body = nullptr;
	const TSharedPtr<FJsonObject>* Listed = nullptr;
	return Document.IsValid() && Document->TryGetObjectField(TEXT("Body"), Body) && (*Body)->TryGetObjectField(TEXT("Parameters"), Listed)
		? *Listed : nullptr;
}

float KindKeyScalar(const UMaterialInstanceConstant* Instance)
{
	float Value = -1.f;
	return Instance != nullptr && Instance->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("Mask")), Value, true) ? Value : -1.f;
}

UTexture* KindKeyTexture(const UMaterialInstanceConstant* Instance)
{
	UTexture* Value = nullptr;
	return Instance != nullptr && Instance->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Mask")), Value, true) ? Value : nullptr;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMaterialInstanceParameterKindKeyTest, "UEAssetDoc.Codec.MaterialInstance.ParameterKindKey",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMaterialInstanceParameterKindKeyTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	const FTarget Parent(TEXT("M_KindKey"));
	const FTarget Target(TEXT("MI_KindKey"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT
	{
		FAssetCompilingManager::Get().FinishAllCompilation();
		Target.Cleanup();
		Parent.Cleanup();
	};
	UTexture* DefaultTexture = LoadObject<UTexture>(nullptr, KindKeyDefaultTexture);
	UTexture* WhiteTexture = LoadObject<UTexture>(nullptr, KindKeyWhiteTexture);
	if (!TestNotNull(TEXT("the default texture loads"), DefaultTexture) || !TestNotNull(TEXT("the white texture loads"), WhiteTexture))
	{
		return false;
	}

	// Content Browser: Add > Material; the palette places a Scalar Parameter (to Roughness) and a Texture Sample
	// Parameter 2D (to Base Color), both named Mask.
	UFactory* MaterialFactory = NewObject<UMaterialFactoryNew>();
	UMaterial* Material = Cast<UMaterial>(MaterialFactory->FactoryCreateNew(UMaterial::StaticClass(), CreatePackage(*Parent.PackageName),
		FName(*Parent.AssetName), RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
	if (!TestNotNull(TEXT("material created by the factory"), Material))
	{
		return false;
	}
	FAssetRegistryModule::AssetCreated(Material);
	Material->MarkPackageDirty();
	UMaterialExpressionScalarParameter* ScalarMask = Cast<UMaterialExpressionScalarParameter>(
		UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -400, 0));
	UMaterialExpressionTextureSampleParameter2D* TextureMask = Cast<UMaterialExpressionTextureSampleParameter2D>(
		UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -400, 200));
	if (!TestTrue(TEXT("nodes placed"), ScalarMask != nullptr && TextureMask != nullptr))
	{
		return false;
	}
	ScalarMask->ParameterName = TEXT("Mask");
	ScalarMask->DefaultValue = 0.5f;
	TextureMask->ParameterName = TEXT("Mask");
	TextureMask->Texture = DefaultTexture;
	UMaterialEditingLibrary::ConnectMaterialProperty(ScalarMask, FString(), MP_Roughness);
	UMaterialEditingLibrary::ConnectMaterialProperty(TextureMask, TEXT("RGB"), MP_BaseColor);
	UMaterialEditingLibrary::RecompileMaterial(Material);
	FAssetCompilingManager::Get().FinishAllCompilation();
	if (!TestTrue(TEXT("material saved"), Parent.Save()))
	{
		return false;
	}

	// Create Material Instance, then both overrides ticked and set in the Material Instance Editor.
	UMaterialInstanceConstantFactoryNew* InstanceFactory = NewObject<UMaterialInstanceConstantFactoryNew>();
	InstanceFactory->InitialParent = Material;
	UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(InstanceFactory->FactoryCreateNew(UMaterialInstanceConstant::StaticClass(),
		CreatePackage(*Target.PackageName), FName(*Target.AssetName), RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
	if (!TestNotNull(TEXT("instance created by the factory"), Instance))
	{
		return false;
	}
	FAssetRegistryModule::AssetCreated(Instance);
	Instance->MarkPackageDirty();
	UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, TEXT("Mask"), 0.25f);
	UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, TEXT("Mask"), WhiteTexture);
	UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
	FAssetCompilingManager::Get().FinishAllCompilation();
	if (!TestEqual(TEXT("the scalar Mask is overridden"), KindKeyScalar(Instance), 0.25f)
		|| !TestTrue(TEXT("the texture Mask is overridden"), KindKeyTexture(Instance) == WhiteTexture)
		|| !TestTrue(TEXT("instance saved"), Target.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Shared parameter name"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Listed = KindKeyParameters(Extracted);
	double ScalarValue = -1.0;
	FString TextureValue;
	TestTrue(TEXT("the scalar Mask is keyed Mask@scalar"), Listed.IsValid() && Listed->TryGetNumberField(TEXT("Mask@scalar"), ScalarValue)
		&& FMath::IsNearlyEqual(ScalarValue, 0.25));
	TestTrue(TEXT("the texture Mask is keyed Mask@texture"), Listed.IsValid() && Listed->TryGetStringField(TEXT("Mask@texture"), TextureValue)
		&& TextureValue == KindKeyWhiteTexture);
	TestFalse(TEXT("no plain Mask key"), Listed.IsValid() && Listed->HasField(TEXT("Mask")));

	// An edit of the scalar keeps the texture override.
	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		if (const TSharedPtr<FJsonObject> Values = KindKeyParameters(Document))
		{
			Values->SetNumberField(TEXT("Mask@scalar"), 0.75);
		}
	}, TEXT("Edit the scalar Mask"));
	if (TestTrue(TEXT("the edit applies"), Edited.IsSuccess()))
	{
		const UMaterialInstanceConstant* After = Target.Get<UMaterialInstanceConstant>();
		TestEqual(TEXT("the scalar Mask takes the value"), KindKeyScalar(After), 0.75f);
		TestTrue(TEXT("the texture Mask keeps its override"), KindKeyTexture(After) == WhiteTexture);
	}

	// A plain Mask with a number is the scalar: only the scalar takes a number.
	const FAssetDocumentResult Plain = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		if (const TSharedPtr<FJsonObject> Values = KindKeyParameters(Document))
		{
			Values->RemoveField(TEXT("Mask@scalar"));
			Values->SetNumberField(TEXT("Mask"), 0.5);
		}
	}, TEXT("Plain Mask"));
	if (TestTrue(TEXT("a plain Mask with a number applies"), Plain.IsSuccess()))
	{
		const UMaterialInstanceConstant* After = Target.Get<UMaterialInstanceConstant>();
		TestEqual(TEXT("the plain Mask sets the scalar"), KindKeyScalar(After), 0.5f);
		TestTrue(TEXT("the texture Mask keeps its override"), KindKeyTexture(After) == WhiteTexture);
	}

	// A plain Mask cleared with null fits both kinds and cannot tell them apart.
	const FAssetDocumentResult Ambiguous = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		if (const TSharedPtr<FJsonObject> Values = KindKeyParameters(Document))
		{
			Values->RemoveField(TEXT("Mask@scalar"));
			Values->SetField(TEXT("Mask"), MakeShared<FJsonValueNull>());
		}
	}, TEXT("Plain null Mask"));
	TestRefused(*this, Ambiguous, TEXT("MaterialInstance.AmbiguousParameter"), TEXT("/Body/Parameters/Mask"), TEXT("Plain null Mask"));
	const UMaterialInstanceConstant* Kept = Target.Get<UMaterialInstanceConstant>();
	TestEqual(TEXT("the refused apply keeps the scalar Mask"), KindKeyScalar(Kept), 0.5f);
	TestTrue(TEXT("the refused apply keeps the texture Mask"), KindKeyTexture(Kept) == WhiteTexture);
	return !HasAnyErrors();
}

#endif
