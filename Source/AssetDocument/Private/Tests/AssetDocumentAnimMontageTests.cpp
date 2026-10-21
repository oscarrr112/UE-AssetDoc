// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
//
// AnimMontage documents through the real production route:
// FAssetDocumentModule::Get().GetService() -> Generic Runtime -> Python
// profile.anim_montage.v1 -> C++ primitives. Assets are created directly in
// test setup and removed again by the fixture.
//
// Retired legacy tests (the C++ AnimMontage profile and its local
// FAssetDocumentService seam no longer exist on the production route):
// - Apply.DefinitionRefBody, DiffTreatsDefinitionRefAsUnchanged: Definitions
//   are rejected by the Python profile (AnimMontage.UnsupportedDefinitions).
// - RejectsAbbreviatedSlots, RejectsInvalidBodySectionTypes, RejectsSemanticInvalidBody,
//   Apply.PreflightDoesNotCreateEmbeddedObjects: pinned legacy fragment shapes
//   (Kind/AssetRef/EmbeddedObject) and legacy diagnostic codes. The Python
//   codec owns the shape contract and is covered by the Python test suite.
// - Apply.Structure, Apply.Notifies, Apply.NotifiesPreservesUnmanaged,
//   ApplyReplacesExistingManagedNotifies, Extract.SkipsUnmanagedNotifies,
//   ApplyExtract.CurvesAndTimeStretch (curves part), DiffIgnoresSkippedMetadata:
//   structural/notify/curve surfaces are covered on the real route by
//   Animation.Production.*, MontageStructuralIntegration.* and MontageNotifyIntegration.*.
// - ApplyExtract.ScalarRegions.Core / .Precedence / .CollectMarkers / .Validation,
//   ApplyExtract.ExpandedBlend, ApplyExtract.MetadataRegions, ApplyFile.SyncState,
//   Apply.FailureDoesNotMutateExisting, InspectProfileIncludesStructuredBody,
//   ValidateStructuredWithoutAssetType, RejectsUnknownBodyKey, RejectsNonObjectBody,
//   DiffIgnoresObjectFieldOrder, DiffReportsChangedBodySections: replaced by
//   the real-route tests below (same assertions, current diagnostics).
// - Apply.FailureCleansNewAsset: restored below on the production route
//   (one-apply rich create; failures after create, during populate and save).
// - SidecarSync.AcceptAsset*: legacy sidecar region regeneration engine; the
//   production route rewrites the sidecar from a fresh Extract after Apply.
// - RejectsBodyWithoutProfile: generic class routing is owned by GenericRuntime tests.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "AssetDocumentTypes.h"

#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimMetaData.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifies/AnimNotify_PlaySound.h"
#include "Animation/AnimNotifies/AnimNotifyState_TimedParticleEffect.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendProfile.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Curves/CurveFloat.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "PackageTools.h"
#include "Misc/Paths.h"
#include "ReferenceSkeleton.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/AssetDocumentJsonKeys.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentAnimMontageRouteTests
{
const TCHAR* MontageClass = TEXT("/Script/Engine.AnimMontage");
const TCHAR* MetadataClass = TEXT("/Script/AssetDocumentTests.AssetDocumentSequenceLifecycleMetadataTest");
const TCHAR* CanonicalBodyKeys[] = {
	TEXT("Skeleton"), TEXT("PreviewMesh"), TEXT("References"), TEXT("Preview"), TEXT("Sync"),
	TEXT("RootMotion"), TEXT("Metadata"), TEXT("SectionMetadata"), TEXT("TimeStretch"), TEXT("Curves"),
	TEXT("SlotAnimTracks"), TEXT("CompositeSections"), TEXT("Notifies"), TEXT("NotifyStates"), TEXT("NotifyTracks"), TEXT("Blend")};
const TCHAR* SettingsBodyKeys[] = {
	TEXT("Preview"), TEXT("Blend"), TEXT("Sync"), TEXT("RootMotion"), TEXT("TimeStretch"),
	TEXT("Metadata"), TEXT("SectionMetadata")};

FAssetDocumentService& Service() { return FAssetDocumentModule::Get().GetService(); }

TSharedPtr<FJsonValue> Ordered(const TSharedPtr<FJsonValue>& Value)
{
	if (Value->Type == EJson::Object)
	{
		TArray<FString> Keys;
		AssetDocumentServerJson::GetKeys(*Value->AsObject(), Keys);
		Keys.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
		auto Out = MakeShared<FJsonObject>();
		for (const FString& Key : Keys) Out->SetField(Key, Ordered(AssetDocumentServerJson::FindFieldRef(*Value->AsObject(), Key)));
		return MakeShared<FJsonValueObject>(Out);
	}
	if (Value->Type == EJson::Array)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const auto& Element : Value->AsArray()) Out.Add(Ordered(Element));
		return MakeShared<FJsonValueArray>(MoveTemp(Out));
	}
	return Value;
}

FString Json(const TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	if (Object.IsValid())
		FJsonSerializer::Serialize(Ordered(MakeShared<FJsonValueObject>(Object))->AsObject().ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
	return Text;
}

TSharedPtr<FJsonObject> Copy(const TSharedPtr<FJsonObject>& Object)
{
	TSharedPtr<FJsonObject> Out;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json(Object)), Out);
	return Out;
}

TArray<uint8> Bytes(const FString& Path)
{
	TArray<uint8> Out;
	FFileHelper::LoadFileToArray(Out, *Path);
	return Out;
}

FString PackageFile(const UObject& Object)
{
	return FPackageName::LongPackageNameToFilename(Object.GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
}

bool Save(UObject& Object)
{
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	const FString File = PackageFile(Object);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
	return UPackage::SavePackage(Object.GetOutermost(), &Object, *File, Args);
}

void Report(FAutomationTestBase& Test, const FAssetDocumentResult& Result)
{
	for (const auto& D : Result.Diagnostics) Test.AddInfo(D.Path + TEXT(" ") + D.Code + TEXT(": ") + D.Message);
}

bool HasDiagnostic(const FAssetDocumentResult& Result, const FString& Path, const FString& Code)
{
	return Result.Diagnostics.ContainsByPredicate([&](const FAssetDocumentDiagnostic& D)
	{
		return (Path.IsEmpty() || D.Path.Contains(Path) || D.Message.Contains(Path)) && D.Code.Contains(Code);
	});
}

struct FField
{
	FString Key;
	TSharedPtr<FJsonValue> Value;
};
TSharedRef<FJsonObject> Object(std::initializer_list<FField> Fields)
{
	auto Out = MakeShared<FJsonObject>();
	for (const FField& Field : Fields) Out->SetField(Field.Key, Field.Value);
	return Out;
}
TSharedPtr<FJsonValue> Num(double Value) { return MakeShared<FJsonValueNumber>(Value); }
TSharedPtr<FJsonValue> Str(const FString& Value) { return MakeShared<FJsonValueString>(Value); }
TSharedPtr<FJsonValue> Bool(bool Value) { return MakeShared<FJsonValueBoolean>(Value); }
TSharedPtr<FJsonValue> Null() { return MakeShared<FJsonValueNull>(); }
TSharedPtr<FJsonValue> Obj(const TSharedRef<FJsonObject>& Value) { return MakeShared<FJsonValueObject>(Value); }
TSharedPtr<FJsonValue> Arr(TArray<TSharedPtr<FJsonValue>> Values) { return MakeShared<FJsonValueArray>(MoveTemp(Values)); }

TSharedRef<FJsonObject> MinimalDocument(const FString& Target)
{
	return Object({{TEXT("SchemaVersion"), Num(1)}, {TEXT("Target"), Str(Target)}, {TEXT("Class"), Str(MontageClass)},
		{TEXT("Action"), Str(TEXT("CreateOrUpdate"))}, {TEXT("Definitions"), Obj(MakeShared<FJsonObject>())},
		{TEXT("Properties"), Obj(MakeShared<FJsonObject>())}, {TEXT("Body"), Obj(MakeShared<FJsonObject>())}});
}

int32 ReadInt(const UObject& Object, const TCHAR* Name)
{
	const FIntProperty* Property = FindFProperty<FIntProperty>(Object.GetClass(), Name);
	return Property ? Property->GetPropertyValue_InContainer(&Object) : INDEX_NONE;
}

FString ReadString(const UObject& Object, const TCHAR* Name)
{
	const FStrProperty* Property = FindFProperty<FStrProperty>(Object.GetClass(), Name);
	return Property ? Property->GetPropertyValue_InContainer(&Object) : FString();
}

TSharedPtr<FJsonObject> Region(const TSharedPtr<FJsonObject>& Document, const TCHAR* Key)
{
	const TSharedPtr<FJsonObject>* Body = nullptr;
	const TSharedPtr<FJsonObject>* Value = nullptr;
	if (!Document || !Document->TryGetObjectField(TEXT("Body"), Body) || !(*Body)->TryGetObjectField(Key, Value)) return {};
	return *Value;
}

// Saved Skeleton (root bone, DefaultSlot, blend profile "Upper"), a one-second
// source sequence, a CurveFloat asset and a Montage with one slot segment, one
// section "Default" and one float curve "Speed".
struct FFixture
{
	FString Root = TEXT("/Game/AssetDocumentTests/AnimMontageRoute/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	FString Sidecar = FPackageName::LongPackageNameToFilename(Root + TEXT("/Montage"), TEXT(".assetdoc.json"));
	TStrongObjectPtr<USkeleton> Skeleton;
	TStrongObjectPtr<UAnimSequence> Source;
	TStrongObjectPtr<UCurveFloat> Curve;
	TStrongObjectPtr<UAnimMontage> Montage;

	template <typename T>
	T* Create(const TCHAR* Name)
	{
		T* Asset = NewObject<T>(CreatePackage(*(Root + TEXT("/") + Name)), Name, RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Asset);
		return Asset;
	}

	bool Setup(FAutomationTestBase& Test)
	{
		Skeleton.Reset(Create<USkeleton>(TEXT("Skeleton")));
		{
			FReferenceSkeletonModifier Modifier(Skeleton.Get());
			Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
		}
		Skeleton->RegisterSlotNode(TEXT("DefaultSlot"));
		if (!Test.TestNotNull(TEXT("fixture blend profile"), Skeleton->CreateNewBlendProfile(TEXT("Upper")))) return false;
		if (!Test.TestTrue(TEXT("save fixture skeleton"), Save(*Skeleton))) return false;

		Source.Reset(Create<UAnimSequence>(TEXT("Source")));
		Source->SetSkeleton(Skeleton.Get());
		auto& SourceController = Source->GetController();
		SourceController.InitializeModel();
		SourceController.SetFrameRate(FFrameRate(30, 1), false);
		SourceController.SetNumberOfFrames(FFrameNumber(30), false);
		if (!SourceController.AddBoneCurve(TEXT("root"), false)) return false;
		TArray<FVector3f> Positions, Scales;
		TArray<FQuat4f> Rotations;
		Positions.Init(FVector3f::ZeroVector, 31); Scales.Init(FVector3f::OneVector, 31); Rotations.Init(FQuat4f::Identity, 31);
		if (!SourceController.SetBoneTrackKeys(TEXT("root"), Positions, Rotations, Scales, false)) return false;
		SourceController.NotifyPopulated();
		Source->WaitOnExistingCompression();
		if (!Test.TestTrue(TEXT("save fixture sequence"), Save(*Source))) return false;

		Curve.Reset(Create<UCurveFloat>(TEXT("BlendCurve")));
		Curve->FloatCurve.AddKey(0.f, 0.f);
		Curve->FloatCurve.AddKey(1.f, 1.f);
		if (!Test.TestTrue(TEXT("save fixture curve"), Save(*Curve))) return false;

		Montage.Reset(Create<UAnimMontage>(TEXT("Montage")));
		Montage->SetSkeleton(Skeleton.Get());
		Montage->RateScale = 1.f;
		auto& Controller = Montage->GetController();
		Controller.InitializeModel();
		Controller.SetFrameRate(FFrameRate(30, 1), false);
		Controller.SetNumberOfFrames(FFrameNumber(30), false);
		Controller.NotifyPopulated();
		const FAnimationCurveIdentifier Speed(TEXT("Speed"), ERawCurveTrackTypes::RCT_Float);
		if (!Controller.AddCurve(Speed, AACF_Editable, false)) return false;
		FRichCurveKey First(0.f, 1.f), Last(1.f, 2.f);
		First.InterpMode = Last.InterpMode = RCIM_Linear;
		if (!Controller.SetCurveKeys(Speed, {First, Last}, false)) return false;
		Montage->SyncSlotIndex = 0;
		Montage->SlotAnimTracks.Reset();
		auto& Slot = Montage->SlotAnimTracks.AddDefaulted_GetRef();
		Slot.SlotName = TEXT("DefaultSlot");
		FAnimSegment Segment;
		Segment.SetAnimReference(Source.Get());
		Segment.StartPos = 0.f; Segment.AnimStartTime = 0.f; Segment.AnimEndTime = 1.f;
		Segment.AnimPlayRate = 1.f; Segment.LoopingCount = 1;
		Slot.AnimTrack.AnimSegments.Add(Segment);
		Montage->CompositeSections.Reset();
		Montage->AddAnimCompositeSection(TEXT("Default"), 0.f);
		Montage->RefreshCacheData();
		if (!Test.TestTrue(TEXT("save fixture montage"), Save(*Montage))) return false;
		return Test.TestEqual(TEXT("fixture montage plays one second"), Montage->GetPlayLength(), 1.f);
	}

	~FFixture()
	{
		// Files first (like the Skeleton harness), then the in-memory assets.
		IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(Root), false, true);
		TArray<UObject*> Assets{Montage.Get(), Curve.Get(), Source.Get(), Skeleton.Get()};
		Montage.Reset(); Curve.Reset(); Source.Reset(); Skeleton.Reset();
		for (UObject* Asset : Assets)
		{
			if (!IsValid(Asset)) continue;
			UPackage* Package = Asset->GetOutermost();
			FAssetRegistryModule::AssetDeleted(Asset);
			Asset->ClearFlags(RF_Public | RF_Standalone);
			Asset->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
			Asset->MarkAsGarbage();
			Package->SetDirtyFlag(false);
			Package->MarkAsGarbage();
		}
		IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(Root), false, true);
	}

	TSharedPtr<FJsonObject> Extract(FAutomationTestBase& Test) const
	{
		FAssetDocumentExtractRequest Request;
		Request.AssetPath = Montage->GetPathName();
		Request.bDiffOnly = false;
		Request.bIncludeAllWritable = true;
		const FAssetDocumentResult Result = Service().Extract(Request);
		Report(Test, Result);
		if (!Test.TestTrue(TEXT("Extract succeeds on the production route"), Result.IsSuccess() && Result.Payload.IsValid())) return {};
		return Result.Payload;
	}

	FAssetDocumentResult ApplyDocument(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Document) const
	{
		Document->SetStringField(TEXT("Action"), TEXT("Update"));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Sidecar), true);
		if (!Test.TestTrue(TEXT("write authoring sidecar"), FFileHelper::SaveStringToFile(Json(Document), *Sidecar))) return {};
		FAssetDocumentApplyFileRequest Request;
		Request.FilePath = Sidecar;
		Request.bSaveAsset = true;
		const FAssetDocumentResult Result = Service().ApplyFile(Request);
		Report(Test, Result);
		return Result;
	}

	// Sparse edit: the extracted header with only the given Body regions and Properties.
	FAssetDocumentResult ApplySparse(FAutomationTestBase& Test, const TSharedRef<FJsonObject>& Body,
		const TSharedRef<FJsonObject>& Properties = MakeShared<FJsonObject>()) const
	{
		const TSharedPtr<FJsonObject> Document = Extract(Test);
		if (!Document) return {};
		Document->SetObjectField(TEXT("Body"), Body);
		Document->SetObjectField(TEXT("Properties"), Properties);
		return ApplyDocument(Test, Document);
	}
};
} // namespace AssetDocumentAnimMontageRouteTests


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimMontageRegisteredProfileSchemaTest,
	"UEAssetDoc.AnimMontage.RegisteredProfileSchema",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAnimMontageRegisteredProfileSchemaTest::RunTest(const FString&)
{
	using namespace AssetDocumentAnimMontageRouteTests;
	FAssetDocumentProfileRequest Request;
	Request.ClassOrAsset = MontageClass;
	const FAssetDocumentResult Result = Service().InspectProfile(Request);
	Report(*this, Result);
	if (!TestTrue(TEXT("InspectProfile succeeds for AnimMontage"), Result.IsSuccess() && Result.Payload.IsValid())) return false;
	const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
	if (!TestTrue(TEXT("AnimMontage profile exposes BodySections"), Result.Payload->TryGetArrayField(TEXT("BodySections"), Sections))) return false;
	TArray<FString> Names;
	for (const auto& Value : *Sections) Names.Add(Value->AsString());
	TestEqual(TEXT("BodySections are exactly the canonical keys"), Names.Num(), int32(UE_ARRAY_COUNT(CanonicalBodyKeys)));
	for (const TCHAR* Key : CanonicalBodyKeys) TestTrue(FString::Printf(TEXT("BodySections includes %s"), Key), Names.Contains(Key));
	TestFalse(TEXT("extract-only _Skipped is not a body section"), Names.Contains(TEXT("_Skipped")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimMontageCreateTemplateTest,
	"UEAssetDoc.AnimMontage.CreateTemplate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAnimMontageCreateTemplateTest::RunTest(const FString&)
{
	using namespace AssetDocumentAnimMontageRouteTests;
	FAssetDocumentTemplateRequest Request;
	Request.Class = MontageClass;
	Request.Target = TEXT("/Game/AssetDocumentTests/AM_Template");
	const FAssetDocumentResult Result = Service().CreateTemplate(Request);
	Report(*this, Result);
	if (!TestTrue(TEXT("CreateTemplate succeeds for AnimMontage"), Result.IsSuccess() && Result.Payload.IsValid())) return false;
	TestFalse(TEXT("template has no AssetType"), Result.Payload->HasField(TEXT("AssetType")));
	TestEqual(TEXT("template class"), Result.Payload->GetStringField(TEXT("Class")), FString(MontageClass));
	TestTrue(TEXT("template Definitions object"), Result.Payload->HasTypedField<EJson::Object>(TEXT("Definitions")));
	TestTrue(TEXT("template Properties object"), Result.Payload->HasTypedField<EJson::Object>(TEXT("Properties")));
	const TSharedPtr<FJsonObject>* Body = nullptr;
	if (!TestTrue(TEXT("template Body"), Result.Payload->TryGetObjectField(TEXT("Body"), Body))) return false;
	TestEqual(TEXT("template emits only canonical keys"), (*Body)->Values.Num(), int32(UE_ARRAY_COUNT(CanonicalBodyKeys)));
	for (const TCHAR* Key : CanonicalBodyKeys) TestTrue(FString::Printf(TEXT("template Body has %s"), Key), (*Body)->HasField(Key));
	TestTrue(TEXT("Skeleton starts null"), (*Body)->HasTypedField<EJson::Null>(TEXT("Skeleton")));
	TestTrue(TEXT("PreviewMesh starts null"), (*Body)->HasTypedField<EJson::Null>(TEXT("PreviewMesh")));
	for (const TCHAR* Key : {TEXT("References"), TEXT("Preview"), TEXT("Sync"), TEXT("RootMotion"), TEXT("SectionMetadata"), TEXT("TimeStretch"), TEXT("Blend")})
		TestTrue(FString::Printf(TEXT("%s starts as an object"), Key), (*Body)->HasTypedField<EJson::Object>(Key));
	for (const TCHAR* Key : {TEXT("Metadata"), TEXT("Curves"), TEXT("SlotAnimTracks"), TEXT("CompositeSections"), TEXT("Notifies"), TEXT("NotifyStates")})
		TestTrue(FString::Printf(TEXT("%s starts as an array"), Key), (*Body)->HasTypedField<EJson::Array>(Key));
	FAssetDocumentValidateRequest Validate;
	Validate.Document = Result.Payload;
	const FAssetDocumentResult Validated = Service().Validate(Validate);
	Report(*this, Validated);
	// Validate runs Apply's checks: creating this absent target needs the Skeleton the
	// template leaves null for the author.
	TestTrue(TEXT("the template is well formed; creating it needs only the Skeleton"),
		!Validated.IsSuccess() && Validated.Diagnostics.Num() == 1
		&& Validated.Diagnostics[0].Path == TEXT("/Body/Skeleton")
		&& Validated.Diagnostics[0].Code == TEXT("AnimMontage.InvalidIntent"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimMontageValidateTest,
	"UEAssetDoc.AnimMontage.Validate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAnimMontageValidateTest::RunTest(const FString&)
{
	using namespace AssetDocumentAnimMontageRouteTests;
	const FString Target = TEXT("/Game/AssetDocumentTests/AM_Validate");
	auto Validate = [&](const TSharedRef<FJsonObject>& Document)
	{
		FAssetDocumentValidateRequest Request;
		Request.Document = Document;
		const FAssetDocumentResult Result = Service().Validate(Request);
		Report(*this, Result);
		return Result;
	};
	{
		// Well formed; the target is absent, so Validate (Apply's checks) asks for the
		// Skeleton a create needs.
		const FAssetDocumentResult Minimal = Validate(MinimalDocument(Target));
		TestTrue(TEXT("structured document without AssetType is well formed; creating it needs only the Skeleton"),
			!Minimal.IsSuccess() && Minimal.Diagnostics.Num() == 1
			&& Minimal.Diagnostics[0].Path == TEXT("/Body/Skeleton")
			&& Minimal.Diagnostics[0].Code == TEXT("AnimMontage.InvalidIntent"));
	}

	auto Unknown = MinimalDocument(Target);
	Unknown->GetObjectField(TEXT("Body"))->SetArrayField(TEXT("UnexpectedSection"), {});
	auto Result = Validate(Unknown);
	TestFalse(TEXT("unknown Body key is rejected"), Result.IsSuccess());
	TestTrue(TEXT("UnknownBodyKey at the key"), HasDiagnostic(Result, TEXT("/Body/UnexpectedSection"), TEXT("UnknownBodyKey")));

	auto Skipped = MinimalDocument(Target);
	Skipped->GetObjectField(TEXT("Body"))->SetObjectField(TEXT("_Skipped"), MakeShared<FJsonObject>());
	Result = Validate(Skipped);
	TestFalse(TEXT("authored _Skipped is rejected"), Result.IsSuccess());
	TestTrue(TEXT("_Skipped is extract-only"), HasDiagnostic(Result, TEXT("/Body/_Skipped"), TEXT("ExtractOnlyBodyKey")));

	auto NonObject = MinimalDocument(Target);
	NonObject->SetStringField(TEXT("Body"), TEXT("not an object"));
	Result = Validate(NonObject);
	TestFalse(TEXT("non-object Body is rejected"), Result.IsSuccess());
	TestTrue(TEXT("InvalidBodyType at Body"), HasDiagnostic(Result, TEXT("/Body"), TEXT("InvalidBodyType")));

	auto BadSectionMetadata = MinimalDocument(Target);
	BadSectionMetadata->GetObjectField(TEXT("Body"))->SetArrayField(TEXT("CompositeSections"), {});
	BadSectionMetadata->GetObjectField(TEXT("Body"))->SetObjectField(TEXT("SectionMetadata"), Object({{TEXT("Missing"), Arr({})}}));
	Result = Validate(BadSectionMetadata);
	TestFalse(TEXT("SectionMetadata must name an authored section"), Result.IsSuccess());
	TestTrue(TEXT("InvalidSectionMetadata"), HasDiagnostic(Result, TEXT("/Body/SectionMetadata"), TEXT("InvalidSectionMetadata")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimMontageExtractSettingsTest,
	"UEAssetDoc.AnimMontage.Extract.Settings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAnimMontageExtractSettingsTest::RunTest(const FString&)
{
	using namespace AssetDocumentAnimMontageRouteTests;
	FFixture Fixture;
	if (!Fixture.Setup(*this)) return false;
	// 0.1 and 0.8 are not exact float32 values: Extract writes the shortest decimal that reads back as the
	// stored float (0.1, not 0.10000000149011612), as for every other float (core guide).
	Fixture.Montage->BlendIn.SetBlendTime(0.1f);
	Fixture.Montage->SlotAnimTracks[0].AnimTrack.AnimSegments[0].AnimPlayRate = 0.8f;
	Fixture.Montage->BlendOut.SetBlendOption(EAlphaBlendOption::Cubic);
	Fixture.Montage->RateScale = 0.5f;
	Fixture.Montage->bEnableRootMotionTranslation = true;
	if (!TestTrue(TEXT("save native edits"), Save(*Fixture.Montage))) return false;

	const TSharedPtr<FJsonObject> Document = Fixture.Extract(*this);
	if (!Document) return false;
	const TSharedPtr<FJsonObject>* Body = nullptr;
	if (!TestTrue(TEXT("extract has Body"), Document->TryGetObjectField(TEXT("Body"), Body))) return false;
	TestEqual(TEXT("extract emits 8 structural + 7 settings regions"), (*Body)->Values.Num(), 15);
	for (const TCHAR* Key : SettingsBodyKeys) TestTrue(FString::Printf(TEXT("extract has %s"), Key), (*Body)->HasField(Key));
	TArray<FString> Canonical;
	for (const TCHAR* Key : CanonicalBodyKeys) Canonical.Add(Key);
	for (const auto& Pair : (*Body)->Values) TestTrue(FString::Printf(TEXT("extract key %s is canonical"), *Pair.Key), Canonical.Contains(FString(*Pair.Key)));
	const auto Blend = Region(Document, TEXT("Blend"));
	if (!TestTrue(TEXT("Blend region"), Blend.IsValid())) return false;
	TestEqual(TEXT("BlendInTime is the shortest float32 decimal"), Blend->GetNumberField(TEXT("BlendInTime")), 0.1);
	const TArray<TSharedPtr<FJsonValue>>* Slots = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Segments = nullptr;
	TestTrue(TEXT("AnimPlayRate is the shortest float32 decimal"), (*Body)->TryGetArrayField(TEXT("SlotAnimTracks"), Slots) && Slots->Num() == 1
		&& (*Slots)[0]->AsObject()->TryGetArrayField(TEXT("Segments"), Segments) && Segments->Num() == 1
		&& (*Segments)[0]->AsObject()->GetNumberField(TEXT("AnimPlayRate")) == 0.8);
	TestEqual(TEXT("BlendOutOption read from native"), Blend->GetStringField(TEXT("BlendOutOption")), FString(TEXT("Cubic")));
	TestTrue(TEXT("BlendProfileIn is null by default"), Blend->HasTypedField<EJson::Null>(TEXT("BlendProfileIn")));
	TestEqual(TEXT("BlendModeIn"), Blend->GetStringField(TEXT("BlendModeIn")), FString(TEXT("Standard")));
	const auto RootMotion = Region(Document, TEXT("RootMotion"));
	TestTrue(TEXT("root motion translation read"), RootMotion && RootMotion->GetBoolField(TEXT("bEnableRootMotionTranslation")));
	const auto Sync = Region(Document, TEXT("Sync"));
	TestTrue(TEXT("Sync region has SyncGroup and SyncSlotIndex"), Sync && Sync->HasField(TEXT("SyncGroup")) && Sync->HasField(TEXT("SyncSlotIndex")));
	const auto TimeStretch = Region(Document, TEXT("TimeStretch"));
	TestTrue(TEXT("TimeStretch region has the private configuration"), TimeStretch
		&& TimeStretch->HasField(TEXT("SamplingRate")) && TimeStretch->HasField(TEXT("CurveValueMinPrecision")));
	const TSharedPtr<FJsonObject>* Properties = nullptr;
	if (!TestTrue(TEXT("extract has Properties"), Document->TryGetObjectField(TEXT("Properties"), Properties))) return false;
	TestEqual(TEXT("Properties carries RateScale"), (*Properties)->GetNumberField(TEXT("RateScale")), 0.5);
	TestFalse(TEXT("Body-owned SyncGroup is not repeated in Properties"), (*Properties)->HasField(TEXT("SyncGroup")));
	TestFalse(TEXT("Body-owned BlendModeIn is not repeated in Properties"), (*Properties)->HasField(TEXT("BlendModeIn")));

	// Diff the extract as an authored sidecar file (integers stay integers when
	// parsed from text; an in-memory FJsonValueNumber is adapted as a float).
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Fixture.Sidecar), true);
	if (!TestTrue(TEXT("write extracted sidecar"), FFileHelper::SaveStringToFile(Json(Document), *Fixture.Sidecar))) return false;
	FAssetDocumentDiffRequest Diff;
	Diff.FilePath = Fixture.Sidecar;
	const FAssetDocumentResult Diffed = Service().Diff(Diff);
	Report(*this, Diffed);
	const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
	TestTrue(TEXT("Diff of a fresh extract succeeds"), Diffed.IsSuccess() && Diffed.Payload && Diffed.Payload->TryGetArrayField(TEXT("changed"), Changed));
	if (Changed) for (const auto& Row : *Changed) AddInfo(TEXT("changed: ") + Json(Row->AsObject()));
	TestTrue(TEXT("Diff of a fresh extract is empty"), Changed && Changed->IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimMontageSettingsRoundTripTest,
	"UEAssetDoc.AnimMontage.Settings.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAnimMontageSettingsRoundTripTest::RunTest(const FString&)
{
	using namespace AssetDocumentAnimMontageRouteTests;
	FFixture Fixture;
	if (!Fixture.Setup(*this)) return false;
	UAnimMontage& Montage = *Fixture.Montage;
	const FString CurvePath = Fixture.Curve->GetPathName();
	const FString PosePath = Fixture.Source->GetPathName();

	auto Body = MakeShared<FJsonObject>();
	Body->SetObjectField(TEXT("Blend"), Object({
		{TEXT("BlendInTime"), Num(0.5)}, {TEXT("BlendInOption"), Str(TEXT("Cubic"))},
		{TEXT("BlendOutTime"), Num(0.125)}, {TEXT("BlendOutOption"), Str(TEXT("Custom"))},
		{TEXT("BlendOutCustomCurve"), Str(CurvePath)}, {TEXT("BlendModeIn"), Str(TEXT("Inertialization"))},
		{TEXT("BlendOutTriggerTime"), Num(0.25)}, {TEXT("bEnableAutoBlendOut"), Bool(false)},
		{TEXT("BlendProfileIn"), Str(TEXT("Upper"))}}));
	Body->SetObjectField(TEXT("Sync"), Object({{TEXT("SyncGroup"), Str(TEXT("Locomotion"))}, {TEXT("SyncSlotIndex"), Num(0)}}));
	Body->SetObjectField(TEXT("RootMotion"), Object({{TEXT("bEnableRootMotionTranslation"), Bool(true)},
		{TEXT("bEnableRootMotionRotation"), Bool(true)}, {TEXT("RootMotionRootLock"), Str(TEXT("AnimFirstFrame"))}}));
	Body->SetObjectField(TEXT("TimeStretch"), Object({{TEXT("TimeStretchCurveName"), Str(TEXT("Speed"))},
		{TEXT("SamplingRate"), Num(30)}, {TEXT("CurveValueMinPrecision"), Num(0.25)}}));
	Body->SetObjectField(TEXT("Preview"), Object({{TEXT("PreviewBasePose"), Str(PosePath)}}));
	const auto Applied = Fixture.ApplySparse(*this, Body, Object({{TEXT("RateScale"), Num(1.5)}}));
	if (!TestTrue(TEXT("settings apply through the production route"), Applied.IsSuccess())) return false;
	TestTrue(TEXT("settings apply saves the asset"), Applied.bSavedAsset);

	TestEqual(TEXT("native BlendIn time"), Montage.BlendIn.GetBlendTime(), 0.5f);
	TestEqual(TEXT("native BlendIn option"), Montage.BlendIn.GetBlendOption(), EAlphaBlendOption::Cubic);
	TestEqual(TEXT("native BlendOut time"), Montage.BlendOut.GetBlendTime(), 0.125f);
	TestEqual(TEXT("native BlendOut option"), Montage.BlendOut.GetBlendOption(), EAlphaBlendOption::Custom);
	TestEqual(TEXT("native BlendOut custom curve"), Montage.BlendOut.GetCustomCurve(), Fixture.Curve.Get());
	TestEqual(TEXT("native BlendModeIn"), Montage.BlendModeIn, EMontageBlendMode::Inertialization);
	TestEqual(TEXT("native BlendOutTriggerTime"), Montage.BlendOutTriggerTime, 0.25f);
	TestFalse(TEXT("native bEnableAutoBlendOut"), Montage.bEnableAutoBlendOut);
	TestTrue(TEXT("native BlendProfileIn"), Montage.BlendProfileIn != nullptr
		&& Montage.BlendProfileIn == Fixture.Skeleton->GetBlendProfile(TEXT("Upper")));
	TestEqual(TEXT("native SyncGroup"), Montage.SyncGroup, FName(TEXT("Locomotion")));
	TestEqual(TEXT("native SyncSlotIndex"), Montage.SyncSlotIndex, 0);
	TestTrue(TEXT("native root motion translation"), Montage.bEnableRootMotionTranslation);
	TestTrue(TEXT("native root motion rotation"), Montage.bEnableRootMotionRotation);
	TestTrue(TEXT("native root lock"), Montage.RootMotionRootLock == ERootMotionRootLock::AnimFirstFrame);
	TestEqual(TEXT("native time stretch curve name"), Montage.TimeStretchCurveName, FName(TEXT("Speed")));
	TestTrue(TEXT("time stretch curve was baked"), Montage.TimeStretchCurve.IsValid());
#if WITH_EDITORONLY_DATA
	TestTrue(TEXT("native preview base pose"), Montage.PreviewBasePose.Get() == Fixture.Source.Get());
#endif
	TestEqual(TEXT("native RateScale"), Montage.RateScale, 1.5f);

	const TSharedPtr<FJsonObject> First = Fixture.Extract(*this);
	if (!First) return false;
	for (const auto& Pair : Body->Values)
	{
		const auto Observed = Region(First, *Pair.Key);
		if (!TestTrue(FString::Printf(TEXT("extract has %s"), *Pair.Key), Observed.IsValid())) continue;
		for (const auto& Field : Pair.Value->AsObject()->Values)
		{
			const TSharedPtr<FJsonValue> Actual = Observed->TryGetField(Field.Key);
			TestTrue(FString::Printf(TEXT("extract reads back %s.%s"), *Pair.Key, *Field.Key),
				Actual.IsValid() && FJsonValue::CompareEqual(*Actual, *Field.Value));
		}
	}
	TestEqual(TEXT("extract reads back RateScale"), First->GetObjectField(TEXT("Properties"))->GetNumberField(TEXT("RateScale")), 1.5);

	// extract -> apply -> extract is a fixed point, and the full document diffs clean.
	const auto Reapplied = Fixture.ApplyDocument(*this, Copy(First));
	if (!TestTrue(TEXT("re-applying the complete extract succeeds"), Reapplied.IsSuccess())) return false;
	const TSharedPtr<FJsonObject> Second = Fixture.Extract(*this);
	if (!Second) return false;
	TestEqual(TEXT("extract -> apply -> extract round-trips"), Json(Second), Json(First));
	FAssetDocumentDiffRequest Diff;
	Diff.FilePath = Fixture.Sidecar;
	const FAssetDocumentResult Diffed = Service().Diff(Diff);
	Report(*this, Diffed);
	const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
	TestTrue(TEXT("Diff of the applied sidecar is empty"), Diffed.IsSuccess() && Diffed.Payload
		&& Diffed.Payload->TryGetArrayField(TEXT("changed"), Changed) && Changed->IsEmpty());

	// Clearing references with null round-trips as well.
	auto Clear = MakeShared<FJsonObject>();
	Clear->SetObjectField(TEXT("Blend"), Object({{TEXT("BlendProfileIn"), Null()},
		{TEXT("BlendOutOption"), Str(TEXT("Linear"))}, {TEXT("BlendOutCustomCurve"), Null()}}));
	Clear->SetObjectField(TEXT("Preview"), Object({{TEXT("PreviewBasePose"), Null()}}));
	if (!TestTrue(TEXT("null references apply"), Fixture.ApplySparse(*this, Clear).IsSuccess())) return false;
	TestTrue(TEXT("BlendProfileIn cleared"), Montage.BlendProfileIn == nullptr);
	TestTrue(TEXT("BlendOut custom curve cleared"), Montage.BlendOut.GetCustomCurve() == nullptr);
	const auto Cleared = Region(Fixture.Extract(*this), TEXT("Blend"));
	TestTrue(TEXT("cleared profile reads back as null"), Cleared && Cleared->HasTypedField<EJson::Null>(TEXT("BlendProfileIn")));
	TestTrue(TEXT("other Blend fields are preserved"), Cleared && Cleared->GetNumberField(TEXT("BlendInTime")) == 0.5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimMontageSettingsMetadataTest,
	"UEAssetDoc.AnimMontage.Settings.Metadata",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAnimMontageSettingsMetadataTest::RunTest(const FString&)
{
	using namespace AssetDocumentAnimMontageRouteTests;
	UClass* Class = LoadObject<UClass>(nullptr, MetadataClass);
	if (!TestNotNull(TEXT("concrete metadata test class"), Class)) return false;
	FFixture Fixture;
	if (!Fixture.Setup(*this)) return false;
	UAnimMontage& Montage = *Fixture.Montage;
	auto Row = [&](int32 First, const TCHAR* Label)
	{
		return Obj(Object({{TEXT("Class"), Str(MetadataClass)},
			{TEXT("Properties"), Obj(Object({{TEXT("First"), Num(First)}, {TEXT("Label"), Str(Label)}}))}}));
	};
	auto Body = MakeShared<FJsonObject>();
	Body->SetArrayField(TEXT("Metadata"), {Row(7, TEXT("montage"))});
	Body->SetObjectField(TEXT("SectionMetadata"), Object({{TEXT("Default"), Arr({Row(9, TEXT("section"))})}}));
	if (!TestTrue(TEXT("metadata applies"), Fixture.ApplySparse(*this, Body).IsSuccess())) return false;

	const TArray<UAnimMetaData*>& Meta = Montage.GetMetaData();
	if (!TestEqual(TEXT("one montage metadata object"), Meta.Num(), 1) || !TestNotNull(TEXT("metadata object"), Meta[0])) return false;
	TestTrue(TEXT("metadata class"), Meta[0]->GetClass() == Class);
	TestTrue(TEXT("metadata is instanced in the montage"), Meta[0]->GetOuter() == &Montage);
	TestEqual(TEXT("metadata First"), ReadInt(*Meta[0], TEXT("First")), 7);
	TestEqual(TEXT("metadata Label"), ReadString(*Meta[0], TEXT("Label")), FString(TEXT("montage")));
	const int32 Section = Montage.GetSectionIndex(TEXT("Default"));
	if (!TestTrue(TEXT("section exists"), Section != INDEX_NONE)) return false;
	const TArray<UAnimMetaData*>& SectionMeta = Montage.CompositeSections[Section].GetMetaData();
	if (!TestEqual(TEXT("one section metadata object"), SectionMeta.Num(), 1) || !TestNotNull(TEXT("section metadata"), SectionMeta[0])) return false;
	TestTrue(TEXT("section metadata outer"), SectionMeta[0]->GetOuter() == &Montage);
	TestEqual(TEXT("section metadata First"), ReadInt(*SectionMeta[0], TEXT("First")), 9);
	const UAnimMetaData* Instance = Meta[0];

	const TSharedPtr<FJsonObject> First = Fixture.Extract(*this);
	if (!First) return false;
	const auto Extracted = First->GetObjectField(TEXT("Body"))->GetArrayField(TEXT("Metadata"));
	if (TestEqual(TEXT("extract reads one metadata row"), Extracted.Num(), 1))
	{
		const auto Props = Extracted[0]->AsObject()->GetObjectField(TEXT("Properties"));
		TestEqual(TEXT("extract metadata class"), Extracted[0]->AsObject()->GetStringField(TEXT("Class")), FString(MetadataClass));
		TestEqual(TEXT("extract metadata First"), Props->GetIntegerField(TEXT("First")), 7);
		TestEqual(TEXT("extract metadata Label"), Props->GetStringField(TEXT("Label")), FString(TEXT("montage")));
	}
	const auto ExtractedSections = Region(First, TEXT("SectionMetadata"));
	TestTrue(TEXT("extract has section metadata"), ExtractedSections && ExtractedSections->HasField(TEXT("Default")));
	if (!TestTrue(TEXT("re-applying the extract succeeds"), Fixture.ApplyDocument(*this, Copy(First)).IsSuccess())) return false;
	TestEqual(TEXT("metadata extract -> apply -> extract round-trips"), Json(Fixture.Extract(*this)), Json(First));
	TestTrue(TEXT("same-class metadata instance is reused"), Montage.GetMetaData().Num() == 1 && Montage.GetMetaData()[0] == Instance);

	auto Clear = MakeShared<FJsonObject>();
	Clear->SetArrayField(TEXT("Metadata"), {});
	if (!TestTrue(TEXT("empty Metadata applies"), Fixture.ApplySparse(*this, Clear).IsSuccess())) return false;
	TestEqual(TEXT("empty Metadata clears montage metadata"), Montage.GetMetaData().Num(), 0);
	TestEqual(TEXT("section metadata is untouched by a montage-level edit"), Montage.CompositeSections[Section].GetMetaData().Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimMontageSettingsInvalidValueRollsBackTest,
	"UEAssetDoc.AnimMontage.Settings.InvalidValueRollsBack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAnimMontageSettingsInvalidValueRollsBackTest::RunTest(const FString&)
{
	using namespace AssetDocumentAnimMontageRouteTests;
	FFixture Fixture;
	if (!Fixture.Setup(*this)) return false;
	UAnimMontage& Montage = *Fixture.Montage;
	const TSharedPtr<FJsonObject> Before = Fixture.Extract(*this);
	if (!Before) return false;
	const float BeforeBlendIn = Montage.BlendIn.GetBlendTime();
	const TArray<uint8> BeforeBytes = Bytes(PackageFile(Montage));

	// The plain fields (RateScale, SyncGroup) are written by the generic Editor
	// batch first; the unknown blend profile then fails inside the settings
	// primitive. Core's package snapshot must restore the earlier writes.
	auto Body = MakeShared<FJsonObject>();
	Body->SetObjectField(TEXT("Sync"), Object({{TEXT("SyncGroup"), Str(TEXT("Broken"))}}));
	Body->SetObjectField(TEXT("Blend"), Object({{TEXT("BlendInTime"), Num(0.75)}, {TEXT("BlendProfileIn"), Str(TEXT("Missing"))}}));
	const auto Failed = Fixture.ApplySparse(*this, Body, Object({{TEXT("RateScale"), Num(2.0)}}));
	TestFalse(TEXT("an unavailable blend profile fails the apply"), Failed.IsSuccess());
	TestFalse(TEXT("a failed apply does not claim a save"), Failed.bSavedAsset);
	TestTrue(TEXT("the failure names the unavailable settings reference"), HasDiagnostic(Failed, FString(), TEXT("SettingsReferenceUnavailable")));
	TestEqual(TEXT("RateScale restored"), Montage.RateScale, 1.f);
	TestEqual(TEXT("SyncGroup restored"), Montage.SyncGroup, FName(NAME_None));
	TestEqual(TEXT("BlendIn time untouched"), Montage.BlendIn.GetBlendTime(), BeforeBlendIn);
	TestTrue(TEXT("package bytes unchanged"), Bytes(PackageFile(Montage)) == BeforeBytes);
	TestEqual(TEXT("observable document restored"), Json(Fixture.Extract(*this)), Json(Before));

	// A value rejected by the Python codec fails before any invocation.
	auto Invalid = MakeShared<FJsonObject>();
	Invalid->SetObjectField(TEXT("Blend"), Object({{TEXT("BlendInTime"), Num(-1.0)}}));
	const auto Rejected = Fixture.ApplySparse(*this, Invalid);
	TestFalse(TEXT("negative blend time is rejected"), Rejected.IsSuccess());
	TestTrue(TEXT("InvalidSettings at the field"), HasDiagnostic(Rejected, TEXT("/Body/Blend/BlendInTime"), TEXT("InvalidSettings")));
	const auto Overlapped = Fixture.ApplySparse(*this, MakeShared<FJsonObject>(), Object({{TEXT("SyncGroup"), Str(TEXT("Twice"))}}));
	TestFalse(TEXT("Body-owned fields cannot be authored in Properties"), Overlapped.IsSuccess());
	TestTrue(TEXT("ManagedPropertyOverlap"), HasDiagnostic(Overlapped, FString(), TEXT("ManagedPropertyOverlap")));
	TestTrue(TEXT("rejections leave package bytes unchanged"), Bytes(PackageFile(Montage)) == BeforeBytes);

	auto Valid = MakeShared<FJsonObject>();
	Valid->SetObjectField(TEXT("Sync"), Object({{TEXT("SyncGroup"), Str(TEXT("Locomotion"))}}));
	TestTrue(TEXT("a valid apply succeeds after the rollback"), Fixture.ApplySparse(*this, Valid).IsSuccess());
	TestEqual(TEXT("valid SyncGroup applied"), Montage.SyncGroup, FName(TEXT("Locomotion")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimMontageDiffTest,
	"UEAssetDoc.AnimMontage.Diff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAnimMontageDiffTest::RunTest(const FString&)
{
	using namespace AssetDocumentAnimMontageRouteTests;
	FFixture Fixture;
	if (!Fixture.Setup(*this)) return false;
	const TSharedPtr<FJsonObject> Extracted = Fixture.Extract(*this);
	if (!Extracted) return false;
	auto ChangedPaths = [&](const TSharedPtr<FJsonObject>& Document)
	{
		FAssetDocumentDiffRequest Request;
		Request.Document = Document;
		const FAssetDocumentResult Result = Service().Diff(Request);
		Report(*this, Result);
		TArray<FString> Paths;
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (TestTrue(TEXT("Diff succeeds"), Result.IsSuccess() && Result.Payload && Result.Payload->TryGetArrayField(TEXT("changed"), Rows)))
			for (const auto& Row : *Rows) Paths.Add(Row->AsObject()->GetStringField(TEXT("path")));
		return Paths;
	};
	const auto Blend = Region(Extracted, TEXT("Blend"));
	if (!TestTrue(TEXT("extract has Blend"), Blend.IsValid())) return false;
	// Sparse: authored settings equal to native are unchanged, whatever the key order.
	auto Sparse = Copy(Extracted);
	auto Body = MakeShared<FJsonObject>();
	Body->SetObjectField(TEXT("Blend"), Object({{TEXT("BlendOutTime"), Blend->TryGetField(TEXT("BlendOutTime"))},
		{TEXT("BlendInTime"), Blend->TryGetField(TEXT("BlendInTime"))}}));
	Sparse->SetObjectField(TEXT("Body"), Body);
	Sparse->SetObjectField(TEXT("Properties"), MakeShared<FJsonObject>());
	TestEqual(TEXT("sparse unchanged settings diff clean"), ChangedPaths(Sparse).Num(), 0);
	Body->GetObjectField(TEXT("Blend"))->SetNumberField(TEXT("BlendInTime"), 0.5);
	TestTrue(TEXT("changed Blend field reported at /Body/Blend"), ChangedPaths(Sparse).Contains(TEXT("/Body/Blend")));
	Body->RemoveField(TEXT("Blend"));
	Sparse->SetObjectField(TEXT("Properties"), Object({{TEXT("RateScale"), Num(3.0)}}));
	TestTrue(TEXT("changed Properties reported"), ChangedPaths(Sparse).Contains(TEXT("/Properties")));
	return true;
}

namespace AssetDocumentAnimMontageCreateTests
{
using namespace AssetDocumentAnimMontageRouteTests;

// Every authored field must read back equal (numbers by value); Extract may add fields.
void ExpectContains(FAutomationTestBase& Test, const TSharedPtr<FJsonValue>& Expected, const TSharedPtr<FJsonValue>& Actual, const FString& Path, int32& Mismatches)
{
	auto Miss = [&](const TCHAR* Why) { ++Mismatches; Test.AddInfo(FString::Printf(TEXT("readback %s mismatch at %s"), Why, *Path)); };
	if (!Expected.IsValid() || !Actual.IsValid()) { if (Expected.IsValid() != Actual.IsValid()) Miss(TEXT("presence")); return; }
	if (Expected->Type == EJson::Number || Actual->Type == EJson::Number)
	{
		if (Expected->Type != Actual->Type || Expected->AsNumber() != Actual->AsNumber()) Miss(TEXT("number"));
		return;
	}
	if (Expected->Type != Actual->Type) { Miss(TEXT("type")); return; }
	if (Expected->Type == EJson::Object)
	{
		for (const auto& Pair : Expected->AsObject()->Values)
			ExpectContains(Test, Pair.Value, Actual->AsObject()->TryGetField(Pair.Key), Path + TEXT("/") + Pair.Key, Mismatches);
		return;
	}
	if (Expected->Type == EJson::Array)
	{
		const auto& A = Expected->AsArray(); const auto& B = Actual->AsArray();
		if (A.Num() != B.Num()) { Miss(TEXT("array length")); return; }
		for (int32 I = 0; I < A.Num(); ++I) ExpectContains(Test, A[I], B[I], FString::Printf(TEXT("%s/%d"), *Path, I), Mismatches);
		return;
	}
	if (!FJsonValue::CompareEqual(*Expected, *Actual)) Miss(TEXT("value"));
}

// A Montage target that does not exist yet, next to the route fixture's assets.
struct FCreatedTarget
{
	FString Package, Name, ObjectPath, Sidecar, File;
	FCreatedTarget(const FFixture& Fixture, const FString& InName)
		: Package(Fixture.Root + TEXT("/") + InName), Name(InName), ObjectPath(Package + TEXT(".") + InName)
		, Sidecar(FPackageName::LongPackageNameToFilename(Package, TEXT(".assetdoc.json")))
		, File(FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension())) {}
	UAnimMontage* Find() const { return FindObject<UAnimMontage>(nullptr, *ObjectPath); }
	bool InRegistry() const
	{
		return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get()
			.GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid();
	}
	~FCreatedTarget()
	{
		if (UAnimMontage* Montage = Find())
		{
			UPackage* Owner = Montage->GetOutermost();
			FAssetRegistryModule::AssetDeleted(Montage);
			Montage->ClearFlags(RF_Public | RF_Standalone);
			Montage->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
			Montage->MarkAsGarbage();
			Owner->SetDirtyFlag(false);
			Owner->MarkAsGarbage();
		}
		IFileManager::Get().Delete(*File, false, true);
		IFileManager::Get().Delete(*Sidecar, false, true);
	}
};

TSharedPtr<FJsonObject> ExtractPath(FAutomationTestBase& Test, const FString& ObjectPath)
{
	using namespace AssetDocumentAnimMontageRouteTests;
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = ObjectPath;
	Request.bDiffOnly = false;
	Request.bIncludeAllWritable = true;
	const FAssetDocumentResult Result = Service().Extract(Request);
	Report(Test, Result);
	if (!Test.TestTrue(TEXT("Extract succeeds on the production route"), Result.IsSuccess() && Result.Payload.IsValid())) return {};
	return Result.Payload;
}

FAssetDocumentResult ApplyAuthored(FAutomationTestBase& Test, const FCreatedTarget& Target, const TSharedPtr<FJsonObject>& Document)
{
	using namespace AssetDocumentAnimMontageRouteTests;
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target.Sidecar), true);
	if (!Test.TestTrue(TEXT("write authored sidecar"), FFileHelper::SaveStringToFile(Json(Document), *Target.Sidecar))) return {};
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Target.Sidecar;
	Request.bSaveAsset = true;
	const FAssetDocumentResult Result = Service().ApplyFile(Request);
	Report(Test, Result);
	return Result;
}

// One CreateOrUpdate document that authors every Montage surface at once:
// Skeleton (factory context), curve, slot segment, two linked sections, one
// notify, one notify state, Blend/Sync/TimeStretch settings and Properties.
TSharedPtr<FJsonObject> RichDocument(FAutomationTestBase& Test, const FFixture& Fixture, const FCreatedTarget& Target)
{
	using namespace AssetDocumentAnimMontageRouteTests;
	const TSharedPtr<FJsonObject> Extracted = Fixture.Extract(Test);
	if (!Extracted) return {};
	TSharedPtr<FJsonObject> Document = Copy(Extracted);
	Document->SetStringField(TEXT("Target"), Document->GetStringField(TEXT("Target")).Contains(TEXT(".")) ? Target.ObjectPath : Target.Package);
	Document->SetStringField(TEXT("Action"), TEXT("CreateOrUpdate"));
	TSharedPtr<FJsonObject> Body = Document->GetObjectField(TEXT("Body"));
	const TArray<TSharedPtr<FJsonValue>> Sections = Body->GetArrayField(TEXT("CompositeSections"));
	if (!Test.TestEqual(TEXT("fixture extract has one section"), Sections.Num(), 1)) return {};
	auto First = Copy(Sections[0]->AsObject());
	First->SetStringField(TEXT("NextSectionName"), TEXT("Second"));
	auto Second = Copy(Sections[0]->AsObject());
	Second->SetStringField(TEXT("Name"), TEXT("Second"));
	Second->SetNumberField(TEXT("LinkableTime"), 0.5);
	Body->SetArrayField(TEXT("CompositeSections"), {Obj(First.ToSharedRef()), Obj(Second.ToSharedRef())});
	Body->SetArrayField(TEXT("Notifies"), {Obj(Object({{TEXT("Time"), Num(0.25)}, {TEXT("TrackIndex"), Num(0)},
		{TEXT("Object"), Obj(Object({{TEXT("Class"), Str(TEXT("/Script/Engine.AnimNotify_PlaySound"))},
			{TEXT("Properties"), Obj(Object({{TEXT("VolumeMultiplier"), Num(0.25)}}))}}))}}))});
	Body->SetArrayField(TEXT("NotifyStates"), {Obj(Object({{TEXT("Time"), Num(0.5)}, {TEXT("Duration"), Num(0.25)}, {TEXT("TrackIndex"), Num(0)},
		{TEXT("Object"), Obj(Object({{TEXT("Class"), Str(TEXT("/Script/Engine.AnimNotifyState_TimedParticleEffect"))},
			{TEXT("Properties"), Obj(Object({{TEXT("bDestroyAtEnd"), Bool(false)}}))}}))}}))});
	Body->GetObjectField(TEXT("Blend"))->SetNumberField(TEXT("BlendInTime"), 0.5);
	Body->GetObjectField(TEXT("Blend"))->SetStringField(TEXT("BlendProfileIn"), TEXT("Upper"));
	Body->GetObjectField(TEXT("Sync"))->SetStringField(TEXT("SyncGroup"), TEXT("Locomotion"));
	Body->GetObjectField(TEXT("TimeStretch"))->SetStringField(TEXT("TimeStretchCurveName"), TEXT("Speed"));
	Document->GetObjectField(TEXT("Properties"))->SetNumberField(TEXT("RateScale"), 1.5);
	return Document;
}

// Rejects only this target's staging save, after every populate step published.
class FScopedCreateSaveVeto
{
	FCoreUObjectDelegates::FIsPackageOKToSaveDelegate Previous;
	FString Package;
public:
	bool bTriggered = false;
	explicit FScopedCreateSaveVeto(FString InPackage)
		: Previous(FCoreUObjectDelegates::IsPackageOKToSaveDelegate), Package(MoveTemp(InPackage))
	{
		FCoreUObjectDelegates::IsPackageOKToSaveDelegate.BindLambda([this](UPackage* Saved, const FString& Filename, FOutputDevice* Error)
		{
			if (Previous.IsBound() && !Previous.Execute(Saved, Filename, Error)) return false;
			if (!bTriggered && Saved && Saved->GetName() == Package
				&& FPaths::ConvertRelativePathToFull(Filename).Contains(TEXT("/Saved/AssetDocument/Runtime/Staging/")))
			{
				bTriggered = true;
				return false;
			}
			return true;
		});
	}
	~FScopedCreateSaveVeto() { FCoreUObjectDelegates::IsPackageOKToSaveDelegate = Previous; }
};

class FScopedAssetAddedObserver
{
	FDelegateHandle Handle;
	FString ObjectPath;
public:
	int32 Count = 0;
	explicit FScopedAssetAddedObserver(FString InObjectPath) : ObjectPath(MoveTemp(InObjectPath))
	{
		Handle = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().OnAssetAdded().AddLambda(
			[this](const FAssetData& Data) { if (Data.GetObjectPathString() == ObjectPath) ++Count; });
	}
	~FScopedAssetAddedObserver()
	{
		if (FAssetRegistryModule* Module = FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry")))
			Module->Get().OnAssetAdded().Remove(Handle);
	}
};
} // namespace AssetDocumentAnimMontageCreateTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimMontageCreatePopulatedTest,
	"UEAssetDoc.AnimMontage.Apply.CreatePopulated",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAnimMontageCreatePopulatedTest::RunTest(const FString&)
{
	using namespace AssetDocumentAnimMontageRouteTests;
	using namespace AssetDocumentAnimMontageCreateTests;
	FFixture Fixture;
	if (!Fixture.Setup(*this)) return false;
	FCreatedTarget Target(Fixture, TEXT("Created"));
	const TSharedPtr<FJsonObject> Authored = RichDocument(*this, Fixture, Target);
	if (!Authored) return false;
	if (!TestNull(TEXT("target starts absent"), Target.Find())) return false;

	const FAssetDocumentResult Created = ApplyAuthored(*this, Target, Authored);
	if (!TestTrue(TEXT("one ApplyFile creates the fully populated Montage"), Created.IsSuccess() && Created.bSavedAsset)) return false;
	UAnimMontage* Montage = Target.Find();
	if (!TestNotNull(TEXT("factory-created Montage"), Montage)) return false;
	TestTrue(TEXT("Skeleton from the factory context"), Montage->GetSkeleton() == Fixture.Skeleton.Get());
	TestEqual(TEXT("two sections"), Montage->CompositeSections.Num(), 2);
	TestEqual(TEXT("section link"), Montage->CompositeSections.Num() == 2 ? Montage->CompositeSections[0].NextSectionName : FName(), FName(TEXT("Second")));
	TestTrue(TEXT("slot segment references the source"), Montage->SlotAnimTracks.Num() == 1 && Montage->SlotAnimTracks[0].AnimTrack.AnimSegments.Num() == 1
		&& Montage->SlotAnimTracks[0].AnimTrack.AnimSegments[0].GetAnimReference() == Fixture.Source.Get());
	bool bNotify = false, bState = false;
	for (const FAnimNotifyEvent& Event : Montage->Notifies)
	{
		if (const auto* Sound = Cast<UAnimNotify_PlaySound>(Event.Notify.Get())) bNotify = Sound->GetOuter() == Montage && Sound->VolumeMultiplier == 0.25f;
		if (const auto* Particle = Cast<UAnimNotifyState_TimedParticleEffect>(Event.NotifyStateClass.Get())) bState = Particle->GetOuter() == Montage && !Particle->bDestroyAtEnd && Event.GetDuration() == 0.25f;
	}
	TestTrue(TEXT("notify created in the new Montage"), bNotify);
	TestTrue(TEXT("notify state created in the new Montage"), bState);
	TestEqual(TEXT("BlendIn time"), Montage->BlendIn.GetBlendTime(), 0.5f);
	TestTrue(TEXT("BlendProfileIn"), Montage->BlendProfileIn != nullptr && Montage->BlendProfileIn == Fixture.Skeleton->GetBlendProfile(TEXT("Upper")));
	TestEqual(TEXT("SyncGroup"), Montage->SyncGroup, FName(TEXT("Locomotion")));
	TestEqual(TEXT("time stretch curve"), Montage->TimeStretchCurveName, FName(TEXT("Speed")));
	TestEqual(TEXT("RateScale"), Montage->RateScale, 1.5f);
	TestFalse(TEXT("created package is clean"), Montage->GetOutermost()->IsDirty());
	TestTrue(TEXT("created package file"), IFileManager::Get().FileExists(*Target.File));
	TestTrue(TEXT("created registry entry"), Target.InRegistry());

	const TSharedPtr<FJsonObject> Extracted = ExtractPath(*this, Target.ObjectPath);
	if (!Extracted) return false;
	int32 Mismatches = 0;
	for (const TCHAR* Key : {TEXT("Class"), TEXT("Body"), TEXT("Properties")})
		ExpectContains(*this, Authored->TryGetField(Key), Extracted->TryGetField(Key), Key, Mismatches);
	TestEqual(TEXT("Extract reads back every authored field"), Mismatches, 0);

	// Re-applying the same unchanged file is a no-op.
	const TArray<uint8> PackageBytes = Bytes(Target.File), SidecarBytes = Bytes(Target.Sidecar);
	FAssetDocumentApplyFileRequest Again;
	Again.FilePath = Target.Sidecar; Again.bSaveAsset = true;
	const FAssetDocumentResult Reapplied = Service().ApplyFile(Again);
	Report(*this, Reapplied);
	TestTrue(TEXT("unchanged re-apply succeeds"), Reapplied.IsSuccess());
	TestFalse(TEXT("unchanged re-apply saves nothing"), Reapplied.bSavedAsset);
	TestTrue(TEXT("unchanged re-apply keeps package bytes"), Bytes(Target.File) == PackageBytes);
	TestTrue(TEXT("unchanged re-apply keeps sidecar bytes"), Bytes(Target.Sidecar) == SidecarBytes);
	TestFalse(TEXT("unchanged re-apply leaves the package clean"), Target.Find() && Target.Find()->GetOutermost()->IsDirty());

	// Persistence: unload, reload from disk, Extract is unchanged.
	const FString Before = Json(Extracted);
	{
		TArray<UPackage*> Packages{Montage->GetOutermost()};
		Montage = nullptr;
		FText Error;
		if (!TestTrue(TEXT("created package unloads"), UPackageTools::UnloadPackages(Packages, Error)))
		{
			AddInfo(Error.ToString());
			return false;
		}
	}
	TestNull(TEXT("unloaded"), Target.Find());
	const TSharedPtr<FJsonObject> Reloaded = ExtractPath(*this, Target.ObjectPath);
	if (!Reloaded) return false;
	TestEqual(TEXT("reloaded Extract equals the applied Extract"), Json(Reloaded), Before);
	TestTrue(TEXT("reload leaves package bytes"), Bytes(Target.File) == PackageBytes);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimMontageFailureCleansNewAssetTest,
	"UEAssetDoc.AnimMontage.Apply.FailureCleansNewAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAnimMontageFailureCleansNewAssetTest::RunTest(const FString&)
{
	using namespace AssetDocumentAnimMontageRouteTests;
	using namespace AssetDocumentAnimMontageCreateTests;
	FFixture Fixture;
	if (!Fixture.Setup(*this)) return false;
	struct FCase
	{
		const TCHAR* Name;
		bool bAfterCreate;
		const TCHAR* Code;
	};
	const FCase Cases[] = {
		{TEXT("InvalidPlayRate"), false, TEXT("AnimMontage.InvalidCanonicalShape")},
		{TEXT("SegmentNotAnimation"), true, nullptr},
		{TEXT("UnknownNotifyProperty"), true, nullptr},
		{TEXT("MissingBlendProfile"), true, TEXT("SettingsReferenceUnavailable")},
		{TEXT("SaveVeto"), true, TEXT("Persistence.StagePackageFailed")},
	};
	for (const FCase& Case : Cases)
	{
		FCreatedTarget Target(Fixture, FString(TEXT("Failed_")) + Case.Name);
		const TSharedPtr<FJsonObject> Document = RichDocument(*this, Fixture, Target);
		if (!Document) return false;
		TSharedPtr<FJsonObject> Body = Document->GetObjectField(TEXT("Body"));
		auto Segment = [&]() { return Body->GetArrayField(TEXT("SlotAnimTracks"))[0]->AsObject()->GetArrayField(TEXT("Segments"))[0]->AsObject(); };
		const FString Name(Case.Name);
		if (Name == TEXT("InvalidPlayRate")) Segment()->SetNumberField(TEXT("AnimPlayRate"), 0);
		if (Name == TEXT("SegmentNotAnimation")) Segment()->SetStringField(TEXT("AnimReference"), Fixture.Curve->GetPathName());
		if (Name == TEXT("UnknownNotifyProperty"))
			Body->GetArrayField(TEXT("Notifies"))[0]->AsObject()->GetObjectField(TEXT("Object"))->SetObjectField(TEXT("Properties"), Object({{TEXT("DoesNotExist"), Num(1)}}));
		if (Name == TEXT("MissingBlendProfile")) Body->GetObjectField(TEXT("Blend"))->SetStringField(TEXT("BlendProfileIn"), TEXT("Missing"));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target.Sidecar), true);
		if (!TestTrue(TEXT("write failing sidecar"), FFileHelper::SaveStringToFile(Json(Document), *Target.Sidecar))) return false;
		const TArray<uint8> SidecarBytes = Bytes(Target.Sidecar);
		FAssetDocumentResult Result;
		FScopedAssetAddedObserver Added(Target.ObjectPath);
		{
			TOptional<FScopedCreateSaveVeto> Veto;
			if (Name == TEXT("SaveVeto")) Veto.Emplace(Target.Package);
			FAssetDocumentApplyFileRequest Request;
			Request.FilePath = Target.Sidecar; Request.bSaveAsset = true;
			Result = Service().ApplyFile(Request);
			Report(*this, Result);
			if (Veto) TestTrue(TEXT("staging save veto executed"), Veto->bTriggered);
		}
		const FString Label = FString::Printf(TEXT("[%s] "), Case.Name);
		TestFalse(Label + TEXT("create fails"), Result.IsSuccess());
		TestFalse(Label + TEXT("failed create claims no save"), Result.bSavedAsset);
		if (Case.Code) TestTrue(Label + TEXT("expected diagnostic"), HasDiagnostic(Result, FString(), Case.Code));
		if (Case.bAfterCreate) TestTrue(Label + TEXT("failure happens after the factory created the asset"), Added.Count > 0);
		else TestEqual(Label + TEXT("rejected before creation"), Added.Count, 0);
		TestNull(Label + TEXT("no live object"), Target.Find());
		UPackage* Package = FindPackage(nullptr, *Target.Package);
		TestTrue(Label + TEXT("no live package"), !Package || (UPackage::IsEmptyPackage(Package) && !Package->IsDirty()));
		TestFalse(Label + TEXT("no registry entry"), Target.InRegistry());
		TestFalse(Label + TEXT("no package file"), IFileManager::Get().FileExists(*Target.File));
		TestTrue(Label + TEXT("authored sidecar bytes unchanged"), Bytes(Target.Sidecar) == SidecarBytes);
		// (Probing with LoadObject would itself create an empty in-memory package.)
		TestFalse(Label + TEXT("nothing loadable"), FPackageName::DoesPackageExist(Target.Package));
		if (Name == TEXT("SaveVeto"))
		{
			const auto Valid = ApplyAuthored(*this, Target, RichDocument(*this, Fixture, Target));
			TestTrue(Label + TEXT("the same target is creatable after cleanup"), Valid.IsSuccess() && Target.Find() != nullptr);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimMontageUnchangedReapplyTest,
	"UEAssetDoc.AnimMontage.Apply.UnchangedReapplyIsNoOp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAnimMontageUnchangedReapplyTest::RunTest(const FString&)
{
	using namespace AssetDocumentAnimMontageRouteTests;
	FFixture Fixture;
	if (!Fixture.Setup(*this)) return false;
	const TSharedPtr<FJsonObject> Before = Fixture.Extract(*this);
	if (!Before) return false;
	const TArray<uint8> PackageBytes = Bytes(PackageFile(*Fixture.Montage));
	const FAssetDocumentResult Result = Fixture.ApplyDocument(*this, Copy(Before));
	TestTrue(TEXT("unchanged full document applies"), Result.IsSuccess());
	TestFalse(TEXT("unchanged apply is a no-op without a save"), Result.bSavedAsset);
	TestTrue(TEXT("package bytes unchanged"), Bytes(PackageFile(*Fixture.Montage)) == PackageBytes);
	TestFalse(TEXT("package stays clean"), Fixture.Montage->GetOutermost()->IsDirty());
	TestEqual(TEXT("Extract unchanged"), Json(Fixture.Extract(*this)), Json(Before));
	return true;
}

// The Montage keeps notifies sorted by time; a document listing them in another order (or
// two at the same time) is the same Montage. Verification matches them by (time, track, class, properties).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimMontageVerificationNotifyOrderTest,
	"UEAssetDoc.AnimMontage.Verification.NotifyOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAnimMontageVerificationNotifyOrderTest::RunTest(const FString&)
{
	using namespace AssetDocumentAnimMontageRouteTests;
	using namespace AssetDocumentAnimMontageCreateTests;
	FFixture Fixture;
	if (!Fixture.Setup(*this)) return false;
	FCreatedTarget Target(Fixture, TEXT("Ordered"));
	const TSharedPtr<FJsonObject> Extracted = Fixture.Extract(*this);
	if (!Extracted) return false;
	TSharedPtr<FJsonObject> Document = Copy(Extracted);
	Document->SetStringField(TEXT("Target"), Document->GetStringField(TEXT("Target")).Contains(TEXT(".")) ? Target.ObjectPath : Target.Package);
	Document->SetStringField(TEXT("Action"), TEXT("CreateOrUpdate"));
	TSharedPtr<FJsonObject> Body = Document->GetObjectField(TEXT("Body"));
	auto Event = [](double Time, double Duration, const TCHAR* Class, const TSharedRef<FJsonObject>& Properties)
	{
		TSharedRef<FJsonObject> Row = Object({{TEXT("Time"), Num(Time)}, {TEXT("TrackIndex"), Num(0)},
			{TEXT("Object"), Obj(Object({{TEXT("Class"), Str(Class)}, {TEXT("Properties"), Obj(Properties)}}))}});
		if (Duration > 0.0) Row->SetNumberField(TEXT("Duration"), Duration);
		return Obj(Row);
	};
	const TCHAR* Particle = TEXT("/Script/Engine.AnimNotifyState_TimedParticleEffect");
	const TCHAR* Trail = TEXT("/Script/Engine.AnimNotifyState_Trail");
	const TCHAR* Sound = TEXT("/Script/Engine.AnimNotify_PlaySound");
	// Later first (one notify start per track and time: the editor's rule).
	Body->SetArrayField(TEXT("NotifyStates"), {
		Event(0.5, 0.25, Particle, Object({{TEXT("bDestroyAtEnd"), Bool(false)}})),
		Event(0.15, 0.3, Particle, MakeShared<FJsonObject>()),
		Event(0.55, 0.1, Trail, MakeShared<FJsonObject>())});
	Body->SetArrayField(TEXT("Notifies"), {
		Event(0.6, 0.0, Sound, Object({{TEXT("VolumeMultiplier"), Num(0.5)}})),
		Event(0.2, 0.0, Sound, MakeShared<FJsonObject>())});
	const FAssetDocumentResult Created = ApplyAuthored(*this, Target, Document);
	if (!TestTrue(TEXT("notifies in another order than the Montage keeps verify"), Created.IsSuccess())) return false;
	FAssetDocumentApplyFileRequest Again;
	Again.FilePath = Target.Sidecar; Again.bSaveAsset = true;
	const FAssetDocumentResult Reapplied = Service().ApplyFile(Again);
	Report(*this, Reapplied);
	TestTrue(TEXT("re-apply succeeds"), Reapplied.IsSuccess());
	TestFalse(TEXT("re-apply changes nothing"), Reapplied.bSavedAsset);
	// Two states starting at one time on one track: the editor would move one to another track. The refusal says so.
	Body->SetArrayField(TEXT("NotifyStates"), {
		Event(0.5, 0.25, Particle, Object({{TEXT("bDestroyAtEnd"), Bool(false)}})),
		Event(0.5, 0.1, Trail, MakeShared<FJsonObject>())});
	const FAssetDocumentResult Coincident = ApplyAuthored(*this, Target, Document);
	TestFalse(TEXT("coincident notify starts on one track are refused"), Coincident.IsSuccess());
	TestTrue(TEXT("the refusal names the time and the track"), Coincident.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& D)
	{
		return D.Message.Contains(TEXT("Time 0.5 on notify track 0"));
	}));
	return !HasAnyErrors();
}

// A user enum property of a notify object takes the enumerator's display name (what Extract reads back);
// any other value is the clear PropertyWrite.DecodeFailed listing the values (an internal NewEnumeratorN was written,
// then failed as a verification mismatch / AnimationProvider.Montage.DependencyUnavailable).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimMontageNotifyEnumSpellingsTest,
	"UEAssetDoc.AnimMontage.NotifyEnumSpellings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAnimMontageNotifyEnumSpellingsTest::RunTest(const FString&)
{
	using namespace AssetDocumentAnimMontageRouteTests;
	FFixture Fixture;
	if (!Fixture.Setup(*this)) return false;
	const FString EnumPackage = Fixture.Root + TEXT("/E_Team");
	const FString NotifyPackage = Fixture.Root + TEXT("/BP_TeamState");
	const FString NotifyClass = NotifyPackage + TEXT(".BP_TeamState_C");
	auto ApplyText = [this](const FString& Package, const FString& Text)
	{
		const FString File = FPackageName::LongPackageNameToFilename(Package, TEXT(".assetdoc.json"));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
		FFileHelper::SaveStringToFile(Text, *File);
		FAssetDocumentApplyFileRequest Request;
		Request.FilePath = File; Request.bSaveAsset = true;
		const FAssetDocumentResult Result = Service().ApplyFile(Request);
		Report(*this, Result);
		return Result;
	};
	ON_SCOPE_EXIT
	{
		TArray<UPackage*> Packages;
		for (const FString& Name : {NotifyPackage, EnumPackage})
			if (UPackage* Package = FindPackage(nullptr, *Name)) { Package->SetDirtyFlag(false); Packages.Add(Package); }
		if (!Packages.IsEmpty()) UPackageTools::UnloadPackages(Packages);
	};
	if (!TestTrue(TEXT("user enum"), ApplyText(EnumPackage, FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.UserDefinedEnum\","
		"\"Action\":\"Create\",\"Body\":{\"Enumerators\":[{\"Name\":\"Red\"},{\"Name\":\"Blue\"},{\"Name\":\"Green\"}]}}"), *EnumPackage)).IsSuccess())
		|| !TestTrue(TEXT("notify state Blueprint with a user enum variable"), ApplyText(NotifyPackage, FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\","
		"\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"Create\",\"Body\":{\"ParentClass\":\"/Script/Engine.AnimNotifyState\","
		"\"Variables\":[{\"Name\":\"Team\",\"Type\":\"byte:%s.E_Team\",\"DefaultValue\":\"Red\",\"InstanceEditable\":true}]}}"),
		*NotifyPackage, *EnumPackage)).IsSuccess()))
	{
		return false;
	}
	auto States = [&NotifyClass](const FString& Team)
	{
		TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
		Properties->SetStringField(TEXT("Team"), Team);
		TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetArrayField(TEXT("NotifyStates"), {Obj(Object({{TEXT("Time"), Num(0.25)}, {TEXT("Duration"), Num(0.25)}, {TEXT("TrackIndex"), Num(0)},
			{TEXT("Object"), Obj(Object({{TEXT("Class"), Str(NotifyClass)}, {TEXT("Properties"), Obj(Properties)}}))}}))});
		return Body;
	};
	auto StoredTeam = [&Fixture]() -> int64
	{
		for (const FAnimNotifyEvent& Event : Fixture.Montage->Notifies)
		{
			if (const UObject* Notify = Event.NotifyStateClass)
			{
				const FNumericProperty* Property = CastField<FNumericProperty>(Notify->GetClass()->FindPropertyByName(TEXT("Team")));
				if (Property != nullptr) return Property->GetSignedIntPropertyValue(Property->ContainerPtrToValuePtr<void>(Notify));
			}
		}
		return -1;
	};
	const FAssetDocumentResult Applied = Fixture.ApplySparse(*this, States(TEXT("Green")));
	if (!TestTrue(TEXT("the display name applies"), Applied.IsSuccess())) return false;
	TestEqual(TEXT("the display name stores its enumerator"), StoredTeam(), int64(2));
	// Another spelling of an enumerator would read back as its display name (the verification would fail after the
	// write): it is refused before, naming the display name. A value that names no enumerator lists the values.
	for (const TCHAR* Spelling : {TEXT("NewEnumerator1"), TEXT("green"), TEXT("2"), TEXT("Purple")})
	{
		const FAssetDocumentResult Refused = Fixture.ApplySparse(*this, States(Spelling));
		TestFalse(*FString::Printf(TEXT("'%s' is refused"), Spelling), Refused.IsSuccess());
		TestTrue(*FString::Printf(TEXT("'%s': PropertyWrite.DecodeFailed lists the values"), Spelling), Refused.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& D)
		{
			return D.Code.Contains(TEXT("DecodeFailed")) && D.Message.Contains(TEXT("Red, Blue, Green"));
		}));
		TestFalse(*FString::Printf(TEXT("'%s': not DependencyUnavailable or a verification mismatch"), Spelling),
			HasDiagnostic(Refused, FString(), TEXT("DependencyUnavailable")) || HasDiagnostic(Refused, FString(), TEXT("VerificationMismatch")));
	}
	TestEqual(TEXT("the refused edits changed nothing"), StoredTeam(), int64(2));
	// The notify instances of the Blueprint class go before its package unloads.
	TSharedRef<FJsonObject> Clear = MakeShared<FJsonObject>();
	Clear->SetArrayField(TEXT("NotifyStates"), {});
	TestTrue(TEXT("clear the notify states"), Fixture.ApplySparse(*this, Clear).IsSuccess());
	return !HasAnyErrors();
}

#endif
