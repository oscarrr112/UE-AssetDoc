// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// An editor-made material holding a node of an expression class that was deprecated after the material was saved
// (UAssetDocumentMaterialTestExpression of the AssetDocumentTests module, found by path, flagged CLASS_Deprecated
// for the test only). The material reads with every property of that node and a warning, the extract re-applies as
// a no-op, and a new node of the class is still refused.

#include "AssetDocumentTolerantTestSupport.h"

#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Factories/MaterialFactoryNew.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantMaterialTest, "UEAssetDoc.Tolerant.Material",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantMaterialTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	const TCHAR* const ClassPath = TEXT("/Script/AssetDocumentTests.AssetDocumentMaterialTestExpression");
	const FTarget Target(TEXT("M_Tolerant"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	UClass* ExpressionClass = FindObject<UClass>(nullptr, ClassPath);
	if (!TestNotNull(TEXT("the test expression class is loaded"), ExpressionClass))
	{
		return false;
	}
	const bool bWasDeprecated = ExpressionClass->HasAnyClassFlags(CLASS_Deprecated);
	ON_SCOPE_EXIT
	{
		if (!bWasDeprecated)
		{
			ExpressionClass->ClassFlags &= ~CLASS_Deprecated;
		}
		FAssetCompilingManager::Get().FinishAllCompilation();
		Target.Cleanup();
	};

	// Content Browser: Add > Material; the palette places a Scalar Parameter (wired to Roughness) and the project node.
	UFactory* Factory = NewObject<UMaterialFactoryNew>();
	UMaterial* Material = Cast<UMaterial>(Factory->FactoryCreateNew(UMaterial::StaticClass(), CreatePackage(*Target.PackageName),
		FName(*Target.AssetName), RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
	if (!TestNotNull(TEXT("material created by the factory"), Material))
	{
		return false;
	}
	FAssetRegistryModule::AssetCreated(Material);
	Material->MarkPackageDirty();
	UMaterialExpressionScalarParameter* Rough = Cast<UMaterialExpressionScalarParameter>(
		UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -400, 0));
	UMaterialExpression* Old = UMaterialEditingLibrary::CreateMaterialExpression(Material, ExpressionClass, -400, 200);
	FProperty* Scale = ExpressionClass->FindPropertyByName(TEXT("Scale"));
	if (!TestTrue(TEXT("nodes placed"), Rough != nullptr && Old != nullptr && Scale != nullptr))
	{
		return false;
	}
	Rough->ParameterName = TEXT("Roughness");
	Rough->DefaultValue = 0.5f;
	UMaterialEditingLibrary::ConnectMaterialProperty(Rough, FString(), MP_Roughness);
	CastField<FFloatProperty>(Scale)->SetPropertyValue_InContainer(Old, 3.f);
	UMaterialEditingLibrary::RecompileMaterial(Material);
	FAssetCompilingManager::Get().FinishAllCompilation();
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}
	const FString Key = Old->GetName();

	// The class is deprecated since the material was saved (a deprecated class's nodes are not saved again).
	ExpressionClass->ClassFlags |= CLASS_Deprecated;
	const FString Node = FString::Printf(TEXT("/Body/Expressions/%s"), *Key);
	FExpected Expected;
	Expected.Warnings = {{TEXT("Material.UnplaceableExpression"), Node}};
	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, Expected, TEXT("Deprecated expression"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	// No new node to compare with: every property is read.
	const TSharedPtr<FJsonValue> ScaleValue = At(Extracted, Node + TEXT("/Properties/Scale"));
	const TSharedPtr<FJsonValue> BiasValue = At(Extracted, Node + TEXT("/Properties/Bias"));
	TestTrue(TEXT("the set property is read"), ScaleValue.IsValid() && ScaleValue->Type == EJson::Number && ScaleValue->AsNumber() == 3.0);
	TestTrue(TEXT("the default property is read too"), BiasValue.IsValid() && BiasValue->Type == EJson::Number && BiasValue->AsNumber() == 0.0);

	// The Material Editor does not place a deprecated class: neither does a document.
	TestRefused(*this, ApplyEdited(*this, Target, [ClassPath](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Another = MakeShared<FJsonObject>();
		Another->SetStringField(TEXT("Class"), ClassPath);
		Document->GetObjectField(TEXT("Body"))->GetObjectField(TEXT("Expressions"))->SetObjectField(TEXT("Another"), Another);
	}, TEXT("New deprecated node")), TEXT("Material.UnknownExpressionClass"), TEXT("/Body/Expressions/Another/Class"), TEXT("New deprecated node"));
	return !HasAnyErrors();
}

#endif
