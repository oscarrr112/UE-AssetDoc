// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// Behavior test for the UE-editor-equivalent raw Montage settings edits.

#if WITH_DEV_AUTOMATION_TESTS

#include "Provider/Montage/MontageSettingsRawEdits.h"
#include "Tests/AssetDocumentAnimationSequenceOwnedFragmentLifecycleTestTypes.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimMetaData.h"
#include "Animation/AnimMontage.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimationMontageSettingsRawEditsTest,
	"UEAssetDoc.AnimationProvider.MontageSettings.RawEdits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAnimationMontageSettingsRawEditsTest::RunTest(const FString&)
{
	namespace Raw = AssetDocumentMontageSettingsRaw;
	UAnimMontage* Montage = NewObject<UAnimMontage>(GetTransientPackage(), NAME_None, RF_Transient);
	auto& Controller = Montage->GetController();
	Controller.InitializeModel();
	Controller.SetFrameRate(FFrameRate(30, 1), false);
	Controller.SetNumberOfFrames(FFrameNumber(30), false);
	Controller.NotifyPopulated();
	const FAnimationCurveIdentifier Speed(TEXT("Speed"), ERawCurveTrackTypes::RCT_Float);
	if (!TestTrue(TEXT("add float curve"), Controller.AddCurve(Speed, AACF_Editable, false))) return false;
	FRichCurveKey First(0.f, 1.f), Last(1.f, 2.f);
	First.InterpMode = Last.InterpMode = RCIM_Linear;
	if (!TestTrue(TEXT("key float curve"), Controller.SetCurveKeys(Speed, {First, Last}, false))) return false;

	// Private time-stretch configuration: read/write like the details panel.
	float Rate = 0.f, Precision = 0.f;
	if (!TestTrue(TEXT("read default config"), Raw::GetTimeStretchConfig(*Montage, Rate, Precision))) return false;
	const float NewRate = 24.f, NewPrecision = 0.125f;
	TestTrue(TEXT("write sampling rate only"), Raw::SetTimeStretchConfig(*Montage, &NewRate, nullptr));
	float ReadRate = 0.f, ReadPrecision = 0.f;
	Raw::GetTimeStretchConfig(*Montage, ReadRate, ReadPrecision);
	TestEqual(TEXT("sampling rate written"), ReadRate, NewRate);
	TestEqual(TEXT("precision untouched by a partial write"), ReadPrecision, Precision);
	Raw::SetTimeStretchConfig(*Montage, nullptr, &NewPrecision);
	Raw::GetTimeStretchConfig(*Montage, ReadRate, ReadPrecision);
	TestEqual(TEXT("precision written"), ReadPrecision, NewPrecision);
	TestEqual(TEXT("rate untouched by a partial write"), ReadRate, NewRate);

	// Bake as saving does: named curve -> markers, else reset.
	Montage->TimeStretchCurveName = TEXT("Speed");
	Raw::BakeTimeStretchCurve(*Montage);
	TestTrue(TEXT("named float curve bakes markers"), Montage->TimeStretchCurve.IsValid());
	Montage->TimeStretchCurveName = TEXT("Missing");
	Raw::BakeTimeStretchCurve(*Montage);
	TestFalse(TEXT("an unknown curve name resets the baked curve"), Montage->TimeStretchCurve.IsValid());

	// Sync refresh routes the editor change notification without altering authored fields.
	Montage->SyncGroup = TEXT("Locomotion");
	Raw::RefreshDerivedSyncState(*Montage);
	TestEqual(TEXT("sync group preserved by refresh"), Montage->SyncGroup, FName(TEXT("Locomotion")));

	// Instanced metadata allocation matches the details panel "+".
	UAnimMetaData* Meta = Raw::NewInstancedMetaData(*Montage, *UAssetDocumentSequenceLifecycleMetadataTest::StaticClass());
	if (!TestNotNull(TEXT("metadata allocated"), Meta)) return false;
	TestTrue(TEXT("metadata outer is the montage"), Meta->GetOuter() == Montage);
	TestTrue(TEXT("metadata is transactional"), Meta->HasAnyFlags(RF_Transactional));
	TestTrue(TEXT("metadata has the requested class"), Meta->GetClass() == UAssetDocumentSequenceLifecycleMetadataTest::StaticClass());
	Montage->MarkAsGarbage();
	return true;
}

#endif
