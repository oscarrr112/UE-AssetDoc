// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Editor-made Material Instances with content the parameter list alone does not show.
// UEAssetDoc.Fidelity.MaterialInstance.CurveAtlas: a Curve Atlas Row Parameter whose curve the Material Instance
//   Editor's picker chose reads as {Curve, Row}; the extract re-applies as a no-op; an edit picks another curve of the
//   atlas (stored, so it survives the PostLoad recompute from the curve); the older bare row number picks the curve at
//   that row, and a row the atlas does not have (also on an atlas with no curves) is stored with no curve, as
//   {"Curve": "", "Row": n} is; a curve outside the atlas is refused.
// UEAssetDoc.Fidelity.MaterialInstance.StaleOverrides: an override of a parameter the parent deleted is not in the
//   document: Extract warns about it, the extract re-applies as a no-op (it stays), and an apply that writes another
//   override drops it with a warning, as the editor's next edit does.
// UEAssetDoc.Fidelity.MaterialInstance.SpecularProfile: the specular profile override the Material Instance Editor sets
//   next to its override flag (a property without CPF_Edit) reads in Properties, re-applies as a no-op, an edit picks
//   another profile, and a new instance created from the extract holds it.

#include "AssetDocumentTolerantTestSupport.h"

#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/CurveLinearColorAtlas.h"
#include "Engine/SpecularProfile.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCurveAtlasRowParameter.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentFidelityMaterialInstanceTestsLocal
{
using namespace AssetDocumentTolerantTestSupport;

UMaterial* NewFidelityMaterial(const FTarget& Target)
{
	UFactory* Factory = NewObject<UMaterialFactoryNew>();
	UMaterial* Material = Cast<UMaterial>(Factory->FactoryCreateNew(UMaterial::StaticClass(), CreatePackage(*Target.PackageName),
		FName(*Target.AssetName), RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
	if (Material != nullptr)
	{
		FAssetRegistryModule::AssetCreated(Material);
		Material->MarkPackageDirty();
	}
	return Material;
}

UMaterialInstanceConstant* NewFidelityInstance(const FTarget& Target, UMaterial* Parent)
{
	UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
	Factory->InitialParent = Parent;
	UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Factory->FactoryCreateNew(UMaterialInstanceConstant::StaticClass(),
		CreatePackage(*Target.PackageName), FName(*Target.AssetName), RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
	if (Instance != nullptr)
	{
		FAssetRegistryModule::AssetCreated(Instance);
		Instance->MarkPackageDirty();
	}
	return Instance;
}

// A colour curve as the Content Browser's Curve asset (Linear Color) makes it, with one key.
UCurveLinearColor* NewFidelityCurve(const FTarget& Target, float Red)
{
	UCurveLinearColor* Curve = Target.Create<UCurveLinearColor>();
	Curve->FloatCurves[0].AddKey(0.0f, Red);
	return Curve;
}

// The stored override of a curve atlas scalar (value and curve), or a null curve and -1.
TPair<float, UObject*> AtlasOverride(const FTarget& Target, const TCHAR* Name)
{
	const UMaterialInstanceConstant* Instance = Target.Get<UMaterialInstanceConstant>();
	for (const FScalarParameterValue& Value : Instance != nullptr ? Instance->ScalarParameterValues : TArray<FScalarParameterValue>())
	{
		if (Value.ParameterInfo.Name == Name)
		{
			return TPair<float, UObject*>(Value.ParameterValue, Value.AtlasData.Curve.Get());
		}
	}
	return TPair<float, UObject*>(-1.0f, nullptr);
}

void SetGradient(const TSharedPtr<FJsonObject>& Document, const TSharedPtr<FJsonValue>& Value)
{
	Document->GetObjectField(TEXT("Body"))->GetObjectField(TEXT("Parameters"))->SetField(TEXT("Gradient"), Value);
}

TSharedPtr<FJsonValue> CurveValue(const FString& Path)
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("Curve"), Path);
	return MakeShared<FJsonValueObject>(Object);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelityMaterialInstanceCurveAtlasTest, "UEAssetDoc.Fidelity.MaterialInstance.CurveAtlas",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentFidelityMaterialInstanceCurveAtlasTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentFidelityMaterialInstanceTestsLocal;
	const FTarget FireTarget(TEXT("C_FidelityFire"));
	const FTarget SmokeTarget(TEXT("C_FidelitySmoke"));
	const FTarget OtherTarget(TEXT("C_FidelityOther"));
	const FTarget AtlasTarget(TEXT("CA_Fidelity"));
	const FTarget EmptyAtlasTarget(TEXT("CA_FidelityEmpty"));
	const FTarget Parent(TEXT("M_FidelityAtlas"));
	const FTarget Target(TEXT("MI_FidelityAtlas"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT
	{
		FAssetCompilingManager::Get().FinishAllCompilation();
		Target.Cleanup();
		Parent.Cleanup();
		EmptyAtlasTarget.Cleanup();
		AtlasTarget.Cleanup();
		OtherTarget.Cleanup();
		SmokeTarget.Cleanup();
		FireTarget.Cleanup();
	};

	// Two gradient curves in a Curve Atlas (Content Browser: Curve Atlas, Gradient Curves set in Details), and a third
	// curve outside it.
	UCurveLinearColor* Fire = NewFidelityCurve(FireTarget, 1.0f);
	UCurveLinearColor* Smoke = NewFidelityCurve(SmokeTarget, 0.5f);
	UCurveLinearColor* Other = NewFidelityCurve(OtherTarget, 0.25f);
	// The Content Browser's Curve Atlas factory (found by class path: the class is not exported).
	UClass* AtlasFactoryClass = FindObject<UClass>(nullptr, TEXT("/Script/UnrealEd.CurveLinearColorAtlasFactory"));
	auto NewAtlas = [AtlasFactoryClass](const FTarget& AtlasAt, TArray<UCurveLinearColor*> Curves) -> UCurveLinearColorAtlas*
	{
		UFactory* AtlasFactory = AtlasFactoryClass != nullptr ? NewObject<UFactory>(GetTransientPackage(), AtlasFactoryClass) : nullptr;
		UCurveLinearColorAtlas* Made = AtlasFactory != nullptr ? Cast<UCurveLinearColorAtlas>(AtlasFactory->FactoryCreateNew(UCurveLinearColorAtlas::StaticClass(),
			CreatePackage(*AtlasAt.PackageName), FName(*AtlasAt.AssetName), RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn)) : nullptr;
		if (Made != nullptr)
		{
			FAssetRegistryModule::AssetCreated(Made);
			Made->GradientCurves = MoveTemp(Curves);
			Made->UpdateTextures();
			Made->MarkPackageDirty();
		}
		return Made;
	};
	UCurveLinearColorAtlas* Atlas = NewAtlas(AtlasTarget, {Fire, Smoke});
	// A second atlas with no gradient curves (they were removed after a parameter picked one).
	UCurveLinearColorAtlas* EmptyAtlas = NewAtlas(EmptyAtlasTarget, {});
	if (!TestNotNull(TEXT("atlas created by the factory"), Atlas) || !TestNotNull(TEXT("empty atlas created by the factory"), EmptyAtlas))
	{
		return false;
	}

	// The Material Editor: a Curve Atlas Row Parameter "Gradient" on the atlas, to Base Color.
	UMaterial* Material = NewFidelityMaterial(Parent);
	UMaterialExpressionCurveAtlasRowParameter* Row = Material != nullptr ? Cast<UMaterialExpressionCurveAtlasRowParameter>(
		UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCurveAtlasRowParameter::StaticClass(), -400, 0)) : nullptr;
	if (!TestNotNull(TEXT("row parameter placed"), Row))
	{
		return false;
	}
	Row->ParameterName = TEXT("Gradient");
	Row->Atlas = Atlas;
	Row->Curve = Fire;
	UMaterialEditingLibrary::ConnectMaterialProperty(Row, FString(), MP_BaseColor);
	// A second one, "Bare", on the empty atlas with the curve it picked before, to Emissive Color. Its atlas has no row
	// for the curve, so the material does not compile (the Material Editor shows the same error).
	AddExpectedMessage(TEXT("Failed to compile Material"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	UMaterialExpressionCurveAtlasRowParameter* Bare = Cast<UMaterialExpressionCurveAtlasRowParameter>(
		UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCurveAtlasRowParameter::StaticClass(), -400, 300));
	if (!TestNotNull(TEXT("second row parameter placed"), Bare))
	{
		return false;
	}
	Bare->ParameterName = TEXT("Bare");
	Bare->Atlas = EmptyAtlas;
	Bare->Curve = Other;
	UMaterialEditingLibrary::ConnectMaterialProperty(Bare, FString(), MP_EmissiveColor);
	UMaterialEditingLibrary::RecompileMaterial(Material);
	FAssetCompilingManager::Get().FinishAllCompilation();

	// The Material Instance Editor: Gradient overridden, the Smoke curve picked (row 1).
	UMaterialInstanceConstant* Instance = NewFidelityInstance(Target, Material);
	if (!TestNotNull(TEXT("instance created by the factory"), Instance))
	{
		return false;
	}
	FMaterialParameterMetadata Meta;
	const FMaterialParameterInfo Info(FName(TEXT("Gradient")));
	if (!TestTrue(TEXT("the parent exposes Gradient"), Instance->GetParameterValue(EMaterialParameterType::Scalar, FMemoryImageMaterialParameterInfo(Info), Meta)
		&& Meta.bUsedAsAtlasPosition))
	{
		return false;
	}
	Meta.ScalarCurve = Smoke;
	Meta.Value = FMaterialParameterValue(1.0f);
	Meta.bOverride = true;
	{
		FMaterialInstanceParameterUpdateContext UpdateContext(Instance);
		UpdateContext.SetParameterValueEditorOnly(Info, Meta, EMaterialSetParameterValueFlags::SetCurveAtlas);
	}
	UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
	FAssetCompilingManager::Get().FinishAllCompilation();
	for (const FTarget* Saved : {&FireTarget, &SmokeTarget, &OtherTarget, &AtlasTarget, &EmptyAtlasTarget, &Parent, &Target})
	{
		if (!TestTrue(*FString::Printf(TEXT("%s saved"), *Saved->AssetName), Saved->Save()))
		{
			return false;
		}
	}

	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Curve atlas"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("the picked curve is read"), StringAt(Extracted, TEXT("/Body/Parameters/Gradient/Curve")), Smoke->GetPathName());
	const TSharedPtr<FJsonValue> RowValue = At(Extracted, TEXT("/Body/Parameters/Gradient/Row"));
	double RowNumber = -1.0;
	TestTrue(TEXT("its row is read"), RowValue.IsValid() && RowValue->TryGetNumber(RowNumber) && RowNumber == 1.0);

	const FString FirePath = Fire->GetPathName();
	TestTrue(TEXT("the Fire curve is picked"), ApplyEdited(*this, Target, [&FirePath](const TSharedPtr<FJsonObject>& Document)
	{
		SetGradient(Document, CurveValue(FirePath));
	}, TEXT("Pick Fire")).IsSuccess());
	const TPair<float, UObject*> Picked = AtlasOverride(Target, TEXT("Gradient"));
	TestTrue(TEXT("the curve and its row are stored"), Picked.Key == 0.0f && Picked.Value == Fire);

	TestTrue(TEXT("the older row number applies"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		SetGradient(Document, MakeShared<FJsonValueNumber>(1));
	}, TEXT("Row number")).IsSuccess());
	const TPair<float, UObject*> ByRow = AtlasOverride(Target, TEXT("Gradient"));
	TestTrue(TEXT("the row picks the curve at that row"), ByRow.Key == 1.0f && ByRow.Value == Smoke);

	const FString OtherPath = OtherTarget.ObjectPath;
	TestRefused(*this, ApplyEdited(*this, Target, [&OtherPath](const TSharedPtr<FJsonObject>& Document)
	{
		SetGradient(Document, CurveValue(OtherPath));
	}, TEXT("Curve outside the atlas")), TEXT("MaterialInstance.InvalidValue"), TEXT("/Body/Parameters/Gradient"), TEXT("Curve outside the atlas"));
	TestTrue(TEXT("the refused apply left the curve"), AtlasOverride(Target, TEXT("Gradient")).Value == Smoke);

	// A row the atlas does not have, and a row of an atlas with no curves: a bare number is stored as
	// {"Curve": "", "Row": n} is, with no curve.
	auto CurvelessRow = [](double Row)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("Curve"), FString());
		Object->SetNumberField(TEXT("Row"), Row);
		return MakeShared<FJsonValueObject>(Object);
	};
	for (const TCHAR* Name : {TEXT("Gradient"), TEXT("Bare")})
	{
		for (const bool bBareNumber : {true, false})
		{
			const FString Stage = FString::Printf(TEXT("%s row 5 as %s"), Name, bBareNumber ? TEXT("a number") : TEXT("{Curve: \"\", Row}"));
			const FString ParameterName(Name);
			const FAssetDocumentResult Applied = ApplyEdited(*this, Target, [&ParameterName, bBareNumber, &CurvelessRow](const TSharedPtr<FJsonObject>& Document)
			{
				Document->GetObjectField(TEXT("Body"))->GetObjectField(TEXT("Parameters"))->SetField(ParameterName,
					bBareNumber ? StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueNumber>(5)) : StaticCastSharedRef<FJsonValue>(CurvelessRow(5)));
			}, Stage);
			TestTrue(*FString::Printf(TEXT("%s: applies"), *Stage), Applied.IsSuccess());
			const TPair<float, UObject*> Stored = AtlasOverride(Target, Name);
			TestTrue(*FString::Printf(TEXT("%s: the row is stored with no curve"), *Stage), Stored.Key == 5.0f && Stored.Value == nullptr);
		}
	}
	TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Curve atlas rows with no curve"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelityMaterialInstanceStaleTest, "UEAssetDoc.Fidelity.MaterialInstance.StaleOverrides",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentFidelityMaterialInstanceStaleTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentFidelityMaterialInstanceTestsLocal;
	const FTarget Parent(TEXT("M_FidelityStale"));
	const FTarget Target(TEXT("MI_FidelityStale"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT
	{
		FAssetCompilingManager::Get().FinishAllCompilation();
		Target.Cleanup();
		Parent.Cleanup();
	};

	UMaterial* Material = NewFidelityMaterial(Parent);
	auto Place = [Material](const TCHAR* Name, int32 Y, EMaterialProperty Pin)
	{
		UMaterialExpressionScalarParameter* Scalar = Cast<UMaterialExpressionScalarParameter>(
			UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -400, Y));
		Scalar->ParameterName = Name;
		UMaterialEditingLibrary::ConnectMaterialProperty(Scalar, FString(), Pin);
		return Scalar;
	};
	UMaterialExpressionScalarParameter* Old = Material != nullptr ? Place(TEXT("Wetness"), 0, MP_Roughness) : nullptr;
	if (!TestNotNull(TEXT("parameters placed"), Old))
	{
		return false;
	}
	Place(TEXT("Metal"), 200, MP_Metallic);
	UMaterialEditingLibrary::RecompileMaterial(Material);
	FAssetCompilingManager::Get().FinishAllCompilation();
	UMaterialInstanceConstant* Instance = NewFidelityInstance(Target, Material);
	if (!TestNotNull(TEXT("instance created by the factory"), Instance))
	{
		return false;
	}
	UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, TEXT("Wetness"), 0.8f);
	UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, TEXT("Metal"), 0.5f);
	UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
	// The material's author deletes Wetness (a rename is followed by the instance, through the expression's id): the
	// instance's override no longer names a parameter of its parent.
	UMaterialEditingLibrary::DeleteMaterialExpression(Material, Old);
	UMaterialEditingLibrary::RecompileMaterial(Material);
	FAssetCompilingManager::Get().FinishAllCompilation();
	if (!TestTrue(TEXT("material saved"), Parent.Save()) || !TestTrue(TEXT("instance saved"), Target.Save()))
	{
		return false;
	}

	FExpected Expected;
	Expected.Warnings = {{TEXT("MaterialInstance.StaleOverrides"), TEXT("/Body/Parameters")}};
	if (!TestTolerantRoundTrip(*this, Target, Expected, TEXT("Stale override")).IsValid())
	{
		return false;
	}
	auto HasWetness = [&Target]()
	{
		const UMaterialInstanceConstant* Current = Target.Get<UMaterialInstanceConstant>();
		return Current != nullptr && Current->ScalarParameterValues.ContainsByPredicate([](const FScalarParameterValue& Value)
		{
			return Value.ParameterInfo.Name == TEXT("Wetness");
		});
	};
	TestTrue(TEXT("the re-applied extract keeps the stale override"), HasWetness());

	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		Document->GetObjectField(TEXT("Body"))->GetObjectField(TEXT("Parameters"))->SetNumberField(TEXT("Metal"), 1);
	}, TEXT("Edit Metal"));
	TestTrue(TEXT("the edit applies"), Edited.IsSuccess());
	TestTrue(TEXT("the apply warns about the dropped override"),
		HasDiagnostic(Edited, TEXT("MaterialInstance.OverridesDropped"), TEXT("/Body/Parameters"), TEXT("warning")));
	TestFalse(TEXT("the stale override is dropped, as the editor's next edit drops it"), HasWetness());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelityMaterialInstanceSpecularTest, "UEAssetDoc.Fidelity.MaterialInstance.SpecularProfile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentFidelityMaterialInstanceSpecularTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentFidelityMaterialInstanceTestsLocal;
	const FTarget Parent(TEXT("M_FidelitySpecular"));
	const FTarget First(TEXT("SP_FidelityFirst"));
	const FTarget Second(TEXT("SP_FidelitySecond"));
	const FTarget Target(TEXT("MI_FidelitySpecular"));
	const FTarget Created(TEXT("MI_FidelitySpecularCreated"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT
	{
		FAssetCompilingManager::Get().FinishAllCompilation();
		Created.Cleanup();
		Target.Cleanup();
		Second.Cleanup();
		First.Cleanup();
		Parent.Cleanup();
	};

	UMaterial* Material = NewFidelityMaterial(Parent);
	USpecularProfile* FirstProfile = First.Create<USpecularProfile>();
	USpecularProfile* SecondProfile = Second.Create<USpecularProfile>();
	if (!TestNotNull(TEXT("material"), Material) || !TestNotNull(TEXT("profiles"), SecondProfile) || FirstProfile == nullptr
		|| !TestTrue(TEXT("dependencies saved"), Parent.Save() && First.Save() && Second.Save()))
	{
		return false;
	}
	UMaterialInstanceConstant* Instance = NewFidelityInstance(Target, Material);
	if (!TestNotNull(TEXT("instance created by the factory"), Instance))
	{
		return false;
	}
	// The Material Instance Editor's Details: tick Override Specular Profile and pick the first profile.
	Instance->Modify();
	Instance->bOverrideSpecularProfile = true;
	Instance->SpecularProfileOverride = FirstProfile;
	FPropertyChangedEvent Changed(UMaterialInstance::StaticClass()->FindPropertyByName(TEXT("SpecularProfileOverride")));
	Instance->PostEditChangeProperty(Changed);
	if (!TestTrue(TEXT("instance saved"), Target.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Specular profile"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("the override reads in Properties"), StringAt(Extracted, TEXT("/Properties/SpecularProfileOverride")), First.ObjectPath);

	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [&Second](const TSharedPtr<FJsonObject>& Document)
	{
		Document->GetObjectField(TEXT("Properties"))->SetStringField(TEXT("SpecularProfileOverride"), Second.ObjectPath);
	}, TEXT("Pick the second profile"));
	const UMaterialInstanceConstant* After = Target.Get<UMaterialInstanceConstant>();
	TestTrue(TEXT("the edit applies"), Edited.IsSuccess() && After != nullptr && After->SpecularProfileOverride != nullptr
		&& After->SpecularProfileOverride->GetPathName() == Second.ObjectPath);

	const FAssetDocumentResult Read = ExtractResult(Target);
	if (TestTrue(TEXT("re-extract"), Read.IsSuccess()))
	{
		const TSharedPtr<FJsonObject> Copy = Parse(Serialize(Read.Payload));
		Copy->SetStringField(TEXT("Action"), TEXT("Create"));
		Copy->SetStringField(TEXT("Target"), Created.PackageName);
		const FAssetDocumentResult Made = ApplyText(*this, Created, Serialize(Copy), TEXT("Create"));
		const UMaterialInstanceConstant* New = Created.Get<UMaterialInstanceConstant>();
		TestTrue(TEXT("a new instance holds the override"), Made.IsSuccess() && New != nullptr && New->bOverrideSpecularProfile
			&& New->SpecularProfileOverride != nullptr && New->SpecularProfileOverride->GetPathName() == Second.ObjectPath);
	}
	return !HasAnyErrors();
}

#endif
