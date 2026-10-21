// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
//
// AnimSequence documents through the real production route:
// FAssetDocumentModule::Get().GetService() -> Generic Runtime -> Python
// profile.anim_sequence.v1 -> animation.asset.authoring.* + Editor property batch.
// Fixture assets live under a unique /Game folder and are removed again.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "AssetDocumentTypes.h"

#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimMetaData.h"
#include "Animation/AnimNotifies/AnimNotify_PlaySound.h"
#include "Animation/AnimNotifies/AnimNotifyState_TimedParticleEffect.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/AssetUserData.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "ReferenceSkeleton.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/AssetDocumentJsonKeys.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentAnimSequencePythonTests
{
const TCHAR* SequenceClass = TEXT("/Script/Engine.AnimSequence");
const TCHAR* MetadataClass = TEXT("/Script/AssetDocumentTests.AssetDocumentSequenceLifecycleMetadataTest");
const TCHAR* UserDataClass = TEXT("/Script/AssetDocumentTests.AssetDocumentSequenceLifecycleUserDataTest");
const TCHAR* BodyKeys[] = {TEXT("References"), TEXT("Preview"), TEXT("Playback"), TEXT("Additive"), TEXT("RootMotion"),
	TEXT("Compression"), TEXT("Curves"), TEXT("NotifyTracks"), TEXT("Notifies"), TEXT("NotifyStates"), TEXT("SyncMarkers"),
	TEXT("Metadata"), TEXT("AssetUserData"), TEXT("BoneTracks")};

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

struct FField { FString Key; TSharedPtr<FJsonValue> Value; };
TSharedRef<FJsonObject> Object(std::initializer_list<FField> Fields)
{
	auto Out = MakeShared<FJsonObject>();
	for (const FField& Field : Fields) Out->SetField(Field.Key, Field.Value);
	return Out;
}
TSharedPtr<FJsonValue> Num(double Value) { return MakeShared<FJsonValueNumber>(Value); }
TSharedPtr<FJsonValue> Str(const FString& Value) { return MakeShared<FJsonValueString>(Value); }
TSharedPtr<FJsonValue> Bool(bool Value) { return MakeShared<FJsonValueBoolean>(Value); }
TSharedPtr<FJsonValue> Obj(const TSharedRef<FJsonObject>& Value) { return MakeShared<FJsonValueObject>(Value); }
TSharedPtr<FJsonValue> Arr(TArray<TSharedPtr<FJsonValue>> Values) { return MakeShared<FJsonValueArray>(MoveTemp(Values)); }

void Report(FAutomationTestBase& Test, const FAssetDocumentResult& Result)
{
	for (const auto& D : Result.Diagnostics) Test.AddInfo(D.Path + TEXT(" ") + D.Code + TEXT(": ") + D.Message);
}

bool HasDiagnostic(const FAssetDocumentResult& Result, const FString& Path, const FString& Code)
{
	return Result.Diagnostics.ContainsByPredicate([&](const FAssetDocumentDiagnostic& D)
	{
		return (Path.IsEmpty() || D.Path.Contains(Path)) && D.Code.Contains(Code);
	});
}

FString PackageFile(const FString& Package) { return FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension()); }

bool Save(UObject& Object)
{
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	const FString File = PackageFile(Object.GetOutermost()->GetName());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
	return UPackage::SavePackage(Object.GetOutermost(), &Object, *File, Args);
}

void Retire(UObject* Asset)
{
	if (!IsValid(Asset)) return;
	UPackage* Package = Asset->GetOutermost();
	FAssetRegistryModule::AssetDeleted(Asset);
	Asset->ClearFlags(RF_Public | RF_Standalone);
	Asset->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
	Asset->MarkAsGarbage();
	Package->SetDirtyFlag(false);
	Package->MarkAsGarbage();
}

// Every authored field must read back equal (numbers by value); Extract may add fields.
void ExpectContains(FAutomationTestBase& Test, const TSharedPtr<FJsonValue>& Expected, const TSharedPtr<FJsonValue>& Actual, const FString& Path, int32& Mismatches)
{
	auto Miss = [&](const TCHAR* Why) { ++Mismatches; Test.AddInfo(FString::Printf(TEXT("readback %s mismatch at %s"), Why, *Path)); };
	if (!Expected.IsValid() || !Actual.IsValid()) { if (Expected.IsValid() != Actual.IsValid()) Miss(TEXT("presence")); return; }
	if (Expected->Type == EJson::Number || Actual->Type == EJson::Number)
	{
		if (Expected->Type != Actual->Type || FMath::Abs(Expected->AsNumber() - Actual->AsNumber()) > 1e-6) Miss(TEXT("number"));
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

// Saved Skeleton (root bone), an in-memory compatible preview mesh, a saved
// one-second pose sequence and a saved existing 30-frame sequence "Existing".
struct FFixture
{
	FString Root = TEXT("/Game/AssetDocumentTests/AnimSequenceRoute/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	TStrongObjectPtr<USkeleton> Skeleton;
	TStrongObjectPtr<USkeletalMesh> Mesh;
	TStrongObjectPtr<UAnimSequence> Pose;
	TStrongObjectPtr<UAnimSequence> Existing;

	template <typename T>
	T* Create(const TCHAR* Name)
	{
		T* Asset = NewObject<T>(CreatePackage(*(Root + TEXT("/") + Name)), Name, RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Asset);
		return Asset;
	}

	bool MakeSequence(FAutomationTestBase& Test, TStrongObjectPtr<UAnimSequence>& Out, const TCHAR* Name, int32 Frames)
	{
		Out.Reset(Create<UAnimSequence>(Name));
		Out->SetSkeleton(Skeleton.Get());
		auto& Controller = Out->GetController();
		Controller.InitializeModel();
		Controller.SetFrameRate(FFrameRate(30, 1), false);
		Controller.SetNumberOfFrames(FFrameNumber(Frames), false);
		if (!Controller.AddBoneCurve(TEXT("root"), false)) return false;
		TArray<FVector3f> Positions, Scales;
		TArray<FQuat4f> Rotations;
		Positions.Init(FVector3f::ZeroVector, Frames + 1); Scales.Init(FVector3f::OneVector, Frames + 1); Rotations.Init(FQuat4f::Identity, Frames + 1);
		if (!Controller.SetBoneTrackKeys(TEXT("root"), Positions, Rotations, Scales, false)) return false;
		Controller.NotifyPopulated();
		Out->WaitOnExistingCompression();
		return Test.TestTrue(FString::Printf(TEXT("save fixture %s"), Name), Save(*Out));
	}

	bool Setup(FAutomationTestBase& Test)
	{
		Skeleton.Reset(Create<USkeleton>(TEXT("Skeleton")));
		{
			FReferenceSkeletonModifier Modifier(Skeleton.Get());
			Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
		}
		if (!Test.TestTrue(TEXT("save fixture skeleton"), Save(*Skeleton))) return false;
		Mesh.Reset(Create<USkeletalMesh>(TEXT("Mesh")));
		Mesh->SetSkeleton(Skeleton.Get());
		{
			FReferenceSkeletonModifier Bones(Mesh->GetRefSkeleton(), Skeleton.Get());
			Bones.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
		}
		return MakeSequence(Test, Pose, TEXT("Pose"), 30) && MakeSequence(Test, Existing, TEXT("Existing"), 30);
	}

	~FFixture()
	{
		IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(Root), false, true);
		TArray<UObject*> Assets{Existing.Get(), Pose.Get(), Mesh.Get(), Skeleton.Get()};
		Existing.Reset(); Pose.Reset(); Mesh.Reset(); Skeleton.Reset();
		for (UObject* Asset : Assets) Retire(Asset);
		IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(Root), false, true);
	}

	FString Path(const TCHAR* Name) const { return Root + TEXT("/") + Name + TEXT(".") + Name; }
};

// A sequence target that does not exist yet.
struct FCreatedTarget
{
	FString Package, ObjectPath, Sidecar, File;
	FCreatedTarget(const FFixture& Fixture, const FString& Name)
		: Package(Fixture.Root + TEXT("/") + Name), ObjectPath(Package + TEXT(".") + Name)
		, Sidecar(FPackageName::LongPackageNameToFilename(Package, TEXT(".assetdoc.json"))), File(PackageFile(Package)) {}
	UAnimSequence* Find() const { return FindObject<UAnimSequence>(nullptr, *ObjectPath); }
	~FCreatedTarget()
	{
		Retire(Find());
		IFileManager::Get().Delete(*File, false, true);
		IFileManager::Get().Delete(*Sidecar, false, true);
	}
};

TSharedPtr<FJsonObject> Extract(FAutomationTestBase& Test, const FString& ObjectPath)
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = ObjectPath;
	Request.bDiffOnly = false;
	Request.bIncludeAllWritable = true;
	const FAssetDocumentResult Result = Service().Extract(Request);
	Report(Test, Result);
	if (!Test.TestTrue(TEXT("Extract succeeds on the production route"), Result.IsSuccess() && Result.Payload.IsValid())) return {};
	return Result.Payload;
}

FAssetDocumentResult ApplyFile(FAutomationTestBase& Test, const FString& Sidecar, const TSharedPtr<FJsonObject>& Document)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Sidecar), true);
	if (!Test.TestTrue(TEXT("write authored sidecar"), FFileHelper::SaveStringToFile(Json(Document), *Sidecar))) return {};
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Sidecar;
	Request.bSaveAsset = true;
	const FAssetDocumentResult Result = Service().ApplyFile(Request);
	Report(Test, Result);
	return Result;
}

TArray<FString> ChangedPaths(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Document)
{
	FAssetDocumentDiffRequest Request;
	Request.Document = Document;
	const FAssetDocumentResult Result = Service().Diff(Request);
	Report(Test, Result);
	TArray<FString> Paths;
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	if (Test.TestTrue(TEXT("Diff succeeds"), Result.IsSuccess() && Result.Payload && Result.Payload->TryGetArrayField(TEXT("changed"), Rows)))
		for (const auto& Row : *Rows) Paths.Add(Row->AsObject()->GetStringField(TEXT("path")));
	return Paths;
}

TSharedRef<FJsonObject> Document(const FString& Target, const TSharedRef<FJsonObject>& Body,
	const TSharedRef<FJsonObject>& Properties = MakeShared<FJsonObject>())
{
	return Object({{TEXT("SchemaVersion"), Num(1)}, {TEXT("Target"), Str(Target)}, {TEXT("Class"), Str(SequenceClass)},
		{TEXT("Action"), Str(TEXT("CreateOrUpdate"))}, {TEXT("Definitions"), Obj(MakeShared<FJsonObject>())},
		{TEXT("Properties"), Obj(Properties)}, {TEXT("Body"), Obj(Body)}});
}

// Every human-edited surface at once: settings, frame data, a curve, two notify
// tracks, a class notify, a named notify, a notify state, sync markers, the
// preview mesh, an object reference, metadata and asset user data.
TSharedRef<FJsonObject> RichBody(const FFixture& Fixture)
{
	auto Key = [](double Time, double Value)
	{ return Obj(Object({{TEXT("Time"), Num(Time)}, {TEXT("Value"), Num(Value)}, {TEXT("InterpMode"), Str(TEXT("Linear"))}})); };
	return Object({
		{TEXT("References"), Obj(Object({{TEXT("Skeleton"), Str(Fixture.Path(TEXT("Skeleton")))}}))},
		{TEXT("Preview"), Obj(Object({{TEXT("PreviewMesh"), Str(Fixture.Path(TEXT("Mesh")))}}))},
		{TEXT("Playback"), Obj(Object({{TEXT("RateScale"), Num(1.5)}, {TEXT("bLoop"), Bool(true)},
			{TEXT("NumberOfFrames"), Num(60)}, {TEXT("FrameRate"), Num(30)}}))},
		{TEXT("Additive"), Obj(Object({{TEXT("AdditiveAnimType"), Str(TEXT("AAT_LocalSpaceBase"))},
			{TEXT("RefPoseType"), Str(TEXT("ABPT_AnimScaled"))}, {TEXT("RefPoseSeq"), Str(Fixture.Path(TEXT("Pose")))}}))},
		{TEXT("RootMotion"), Obj(Object({{TEXT("bEnableRootMotion"), Bool(true)}}))},
		{TEXT("Compression"), Obj(Object({{TEXT("CompressionErrorThresholdScale"), Num(0.5)}}))},
		{TEXT("Curves"), Arr({Obj(Object({{TEXT("Name"), Str(TEXT("Speed"))}, {TEXT("Keys"), Arr({Key(0, 1), Key(1, 2)})},
			{TEXT("Comment"), Str(TEXT("run speed"))}}))})},
		{TEXT("NotifyTracks"), Arr({Obj(Object({{TEXT("Name"), Str(TEXT("Sound"))}})), Obj(Object({{TEXT("Name"), Str(TEXT("FX"))}}))})},
		{TEXT("Notifies"), Arr({
			Obj(Object({{TEXT("Time"), Num(0.25)}, {TEXT("Track"), Str(TEXT("Sound"))}, {TEXT("Class"), Str(TEXT("/Script/Engine.AnimNotify_PlaySound"))},
				{TEXT("Properties"), Obj(Object({{TEXT("VolumeMultiplier"), Num(0.5)}}))}, {TEXT("NotifyTriggerChance"), Num(0.5)}})),
			Obj(Object({{TEXT("Time"), Num(0.5)}, {TEXT("Track"), Str(TEXT("Sound"))}, {TEXT("Name"), Str(TEXT("Footstep"))}}))})},
		{TEXT("NotifyStates"), Arr({Obj(Object({{TEXT("Time"), Num(1.0)}, {TEXT("Duration"), Num(0.5)}, {TEXT("Track"), Str(TEXT("FX"))},
			{TEXT("Class"), Str(TEXT("/Script/Engine.AnimNotifyState_TimedParticleEffect"))},
			{TEXT("Properties"), Obj(Object({{TEXT("bDestroyAtEnd"), Bool(true)}}))}}))})},
		{TEXT("SyncMarkers"), Arr({Obj(Object({{TEXT("Name"), Str(TEXT("LeftFoot"))}, {TEXT("Time"), Num(0.5)}, {TEXT("Track"), Str(TEXT("FX"))}})),
			Obj(Object({{TEXT("Name"), Str(TEXT("RightFoot"))}, {TEXT("Time"), Num(1.5)}}))})},
		{TEXT("Metadata"), Arr({Obj(Object({{TEXT("Class"), Str(MetadataClass)},
			{TEXT("Properties"), Obj(Object({{TEXT("First"), Num(5)}, {TEXT("Label"), Str(TEXT("after"))}}))}}))})},
		{TEXT("AssetUserData"), Arr({Obj(Object({{TEXT("Class"), Str(UserDataClass)},
			{TEXT("Properties"), Obj(Object({{TEXT("First"), Num(9)}}))}}))})},
	});
}

// Rejects only this target's staging save, after every apply step published.
class FScopedSaveVeto
{
	FCoreUObjectDelegates::FIsPackageOKToSaveDelegate Previous;
	FString Package;
public:
	bool bTriggered = false;
	explicit FScopedSaveVeto(FString InPackage)
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
	~FScopedSaveVeto() { FCoreUObjectDelegates::IsPackageOKToSaveDelegate = Previous; }
};
} // namespace AssetDocumentAnimSequencePythonTests


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimSequencePythonSchemaTest,
	"UEAssetDoc.AnimSequence.Python.Schema",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAnimSequencePythonSchemaTest::RunTest(const FString&)
{
	using namespace AssetDocumentAnimSequencePythonTests;
	FAssetDocumentProfileRequest Request;
	Request.ClassOrAsset = SequenceClass;
	const FAssetDocumentResult Result = Service().InspectProfile(Request);
	Report(*this, Result);
	if (!TestTrue(TEXT("InspectProfile succeeds for AnimSequence"), Result.IsSuccess() && Result.Payload.IsValid())) return false;
	const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
	if (!TestTrue(TEXT("BodySections"), Result.Payload->TryGetArrayField(TEXT("BodySections"), Sections))) return false;
	TArray<FString> Names;
	for (const auto& Value : *Sections) Names.Add(Value->AsString());
	TestEqual(TEXT("BodySections are exactly the canonical keys"), Names.Num(), int32(UE_ARRAY_COUNT(BodyKeys)));
	for (const TCHAR* Key : BodyKeys) TestTrue(FString::Printf(TEXT("BodySections includes %s"), Key), Names.Contains(Key));

	FAssetDocumentTemplateRequest TemplateRequest;
	TemplateRequest.Class = SequenceClass;
	TemplateRequest.Target = TEXT("/Game/AssetDocumentTests/AS_Template");
	const FAssetDocumentResult Template = Service().CreateTemplate(TemplateRequest);
	Report(*this, Template);
	if (!TestTrue(TEXT("CreateTemplate succeeds"), Template.IsSuccess() && Template.Payload.IsValid())) return false;
	TestEqual(TEXT("template class"), Template.Payload->GetStringField(TEXT("Class")), FString(SequenceClass));
	FAssetDocumentValidateRequest Validate;
	Validate.Document = Template.Payload;
	const FAssetDocumentResult Validated = Service().Validate(Validate);
	Report(*this, Validated);
	// Validate runs Apply's checks: creating this absent target needs a Skeleton, which
	// the template leaves for the author to fill in.
	TestTrue(TEXT("the template is well formed; creating it needs only the Skeleton"),
		!Validated.IsSuccess() && Validated.Diagnostics.Num() == 1
		&& HasDiagnostic(Validated, TEXT("/Body/References/Skeleton"), TEXT("AnimSequence.SkeletonRequired")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimSequencePythonValidationTest,
	"UEAssetDoc.AnimSequence.Python.Validation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAnimSequencePythonValidationTest::RunTest(const FString&)
{
	using namespace AssetDocumentAnimSequencePythonTests;
	const FString Target = TEXT("/Game/AssetDocumentTests/AS_Validate");
	auto Validate = [&](const TSharedRef<FJsonObject>& Body, const TSharedRef<FJsonObject>& Properties = MakeShared<FJsonObject>())
	{
		FAssetDocumentValidateRequest Request;
		Request.Document = Document(Target, Body, Properties);
		const FAssetDocumentResult Result = Service().Validate(Request);
		Report(*this, Result);
		return Result;
	};
	{
		// The target is absent, so Validate (Apply's checks) asks for the Skeleton a create needs.
		const FAssetDocumentResult Empty = Validate(MakeShared<FJsonObject>());
		TestTrue(TEXT("empty Body is well formed; creating it needs only the Skeleton"),
			!Empty.IsSuccess() && Empty.Diagnostics.Num() == 1
			&& HasDiagnostic(Empty, TEXT("/Body/References/Skeleton"), TEXT("AnimSequence.SkeletonRequired")));
	}
	auto Result = Validate(Object({{TEXT("Sections"), Arr({})}}));
	TestTrue(TEXT("unknown Body key"), !Result.IsSuccess() && HasDiagnostic(Result, TEXT("/Body/Sections"), TEXT("AnimSequence.UnknownBodyKey")));
	Result = Validate(Object({{TEXT("NotifyTracks"), Arr({})}}));
	TestTrue(TEXT("UE keeps one notify track"), !Result.IsSuccess() && HasDiagnostic(Result, TEXT("/Body/NotifyTracks"), TEXT("AnimSequence.NotifyTrackRequired")));
	Result = Validate(Object({{TEXT("NotifyTracks"), Arr({Obj(Object({{TEXT("Name"), Str(TEXT("A"))}}))})},
		{TEXT("Notifies"), Arr({Obj(Object({{TEXT("Time"), Num(0.1)}, {TEXT("Track"), Str(TEXT("B"))}, {TEXT("Name"), Str(TEXT("X"))}}))})}}));
	TestTrue(TEXT("notify on an unknown track"), !Result.IsSuccess() && HasDiagnostic(Result, TEXT("/Body/Notifies/0/Track"), TEXT("AnimSequence.UnknownNotifyTrack")));
	Result = Validate(Object({{TEXT("Notifies"), Arr({Obj(Object({{TEXT("Time"), Num(0.1)}, {TEXT("Name"), Str(TEXT("X"))}})),
		Obj(Object({{TEXT("Time"), Num(0.1)}, {TEXT("Name"), Str(TEXT("Y"))}}))})}}));
	TestTrue(TEXT("two notifies on one track and time"), !Result.IsSuccess() && HasDiagnostic(Result, TEXT("/Body/Notifies/1/Time"), TEXT("AnimSequence.OverlappingNotifies")));
	Result = Validate(MakeShared<FJsonObject>(), Object({{TEXT("RateScale"), Num(2)}}));
	TestTrue(TEXT("Body-owned field in Properties"), !Result.IsSuccess() && HasDiagnostic(Result, TEXT("/Properties/RateScale"), TEXT("AnimSequence.ManagedPropertyOverlap")));
	Result = Validate(Object({{TEXT("Playback"), Obj(Object({{TEXT("RateScale"), Str(TEXT("fast"))}}))}}));
	TestTrue(TEXT("typed settings field"), !Result.IsSuccess() && HasDiagnostic(Result, TEXT("/Body/Playback/RateScale"), TEXT("AnimSequence.InvalidDocument")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimSequencePythonCreatePopulatedTest,
	"UEAssetDoc.AnimSequence.Python.CreatePopulated",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAnimSequencePythonCreatePopulatedTest::RunTest(const FString&)
{
	using namespace AssetDocumentAnimSequencePythonTests;
	FFixture Fixture;
	if (!Fixture.Setup(*this)) return false;
	FCreatedTarget Target(Fixture, TEXT("Created"));
	const TSharedRef<FJsonObject> Authored = Document(Target.Package, RichBody(Fixture));
	if (!TestNull(TEXT("target starts absent"), Target.Find())) return false;

	const FAssetDocumentResult Created = ApplyFile(*this, Target.Sidecar, Authored);
	if (!TestTrue(TEXT("one ApplyFile creates the fully populated sequence"), Created.IsSuccess() && Created.bSavedAsset)) return false;
	UAnimSequence* Sequence = Target.Find();
	if (!TestNotNull(TEXT("factory-created sequence"), Sequence)) return false;
	TestTrue(TEXT("Skeleton from the factory context"), Sequence->GetSkeleton() == Fixture.Skeleton.Get());
	TestEqual(TEXT("frames"), Sequence->GetDataModel()->GetNumberOfFrames(), 60);
	TestEqual(TEXT("play length"), Sequence->GetPlayLength(), 2.f);
	TestEqual(TEXT("RateScale"), Sequence->RateScale, 1.5f);
	TestTrue(TEXT("bLoop"), Sequence->bLoop);
	TestTrue(TEXT("RefPoseSeq"), Sequence->RefPoseSeq == Fixture.Pose.Get());
	TestTrue(TEXT("preview mesh"), static_cast<const UAnimationAsset*>(Sequence)->GetPreviewMesh() == Fixture.Mesh.Get());
	TestEqual(TEXT("one curve"), Sequence->GetDataModel()->GetFloatCurves().Num(), 1);
	TestEqual(TEXT("two notify tracks"), Sequence->AnimNotifyTracks.Num(), 2);
	TestEqual(TEXT("two notifies and one state"), Sequence->Notifies.Num(), 3);
	bool bSound = false, bState = false, bNamed = false;
	for (const FAnimNotifyEvent& Event : Sequence->Notifies)
	{
		if (const auto* Sound = Cast<UAnimNotify_PlaySound>(Event.Notify.Get()))
			bSound = Sound->GetOuter() == Sequence && Sound->VolumeMultiplier == 0.5f && Event.TrackIndex == 0
				&& Event.NotifyTriggerChance == 0.5f && Event.NotifyName == TEXT("PlaySound") && Event.Guid.IsValid();
		if (const auto* Particle = Cast<UAnimNotifyState_TimedParticleEffect>(Event.NotifyStateClass.Get()))
			bState = Particle->GetOuter() == Sequence && Particle->bDestroyAtEnd && Event.GetDuration() == 0.5f && Event.TrackIndex == 1;
		if (!Event.Notify && !Event.NotifyStateClass) bNamed = Event.NotifyName == TEXT("Footstep") && Event.GetTime() == 0.5f;
	}
	TestTrue(TEXT("class notify created in the sequence"), bSound);
	TestTrue(TEXT("notify state created in the sequence"), bState);
	TestTrue(TEXT("named notify"), bNamed);
	TestEqual(TEXT("two sync markers"), Sequence->AuthoredSyncMarkers.Num(), 2);
	TestEqual(TEXT("unique marker names refreshed"), Sequence->UniqueMarkerNames.Num(), 2);
	TestEqual(TEXT("metadata"), Sequence->GetMetaData().Num(), 1);
	TestTrue(TEXT("asset user data"), Sequence->GetAssetUserDataArray() && Sequence->GetAssetUserDataArray()->Num() == 1);
	TestFalse(TEXT("created package is clean"), Sequence->GetOutermost()->IsDirty());
	TestTrue(TEXT("created package file"), IFileManager::Get().FileExists(*Target.File));

	const TSharedPtr<FJsonObject> Extracted = Extract(*this, Target.ObjectPath);
	if (!Extracted) return false;
	int32 Mismatches = 0;
	for (const TCHAR* Key : {TEXT("Class"), TEXT("Body"), TEXT("Properties")})
		ExpectContains(*this, Authored->TryGetField(Key), Extracted->TryGetField(Key), Key, Mismatches);
	TestEqual(TEXT("Extract reads back every authored field"), Mismatches, 0);
	TestEqual(TEXT("Diff of the authored document is empty"), ChangedPaths(*this, Authored).Num(), 0);

	// Extract -> apply -> Extract round-trips, and is a no-op.
	auto RoundTrip = Copy(Extracted);
	RoundTrip->SetStringField(TEXT("Target"), Target.Package);
	TestEqual(TEXT("Diff of the extracted document is empty"), ChangedPaths(*this, RoundTrip).Num(), 0);

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
		TArray<UPackage*> Packages{Sequence->GetOutermost()};
		Sequence = nullptr;
		FText Error;
		if (!TestTrue(TEXT("created package unloads"), UPackageTools::UnloadPackages(Packages, Error)))
		{
			AddInfo(Error.ToString());
			return false;
		}
	}
	TestNull(TEXT("unloaded"), Target.Find());
	const TSharedPtr<FJsonObject> Reloaded = Extract(*this, Target.ObjectPath);
	if (!Reloaded) return false;
	TestEqual(TEXT("reloaded Extract equals the applied Extract"), Json(Reloaded), Before);
	TestTrue(TEXT("reload leaves package bytes"), Bytes(Target.File) == PackageBytes);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimSequencePythonUpdateRollbackTest,
	"UEAssetDoc.AnimSequence.Python.UpdateRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAnimSequencePythonUpdateRollbackTest::RunTest(const FString&)
{
	using namespace AssetDocumentAnimSequencePythonTests;
	FFixture Fixture;
	if (!Fixture.Setup(*this)) return false;
	const FString Package = Fixture.Root + TEXT("/Existing");
	const FString Sidecar = FPackageName::LongPackageNameToFilename(Package, TEXT(".assetdoc.json"));
	const FString ObjectPath = Fixture.Path(TEXT("Existing"));
	const TSharedPtr<FJsonObject> Original = Extract(*this, ObjectPath);
	if (!Original) return false;
	const TArray<uint8> OriginalBytes = Bytes(PackageFile(Package));

	// Update: omitted regions are kept; the authored ones replace.
	auto Body = RichBody(Fixture);
	Body->RemoveField(TEXT("Metadata"));
	Body->RemoveField(TEXT("AssetUserData"));
	const FAssetDocumentResult Updated = ApplyFile(*this, Sidecar, Document(Package, Body));
	if (!TestTrue(TEXT("rich update succeeds"), Updated.IsSuccess() && Updated.bSavedAsset)) return false;
	const TSharedPtr<FJsonObject> After = Extract(*this, ObjectPath);
	if (!After) return false;
	TestEqual(TEXT("bone track kept"), After->GetObjectField(TEXT("Body"))->GetArrayField(TEXT("BoneTracks")).Num(), 1);
	TestEqual(TEXT("Diff after update is empty"), ChangedPaths(*this, Document(Package, Body)).Num(), 0);

	// Explicit [] clears; omitted keeps.
	auto Clear = Object({{TEXT("Notifies"), Arr({})}, {TEXT("Curves"), Arr({})}});
	if (!TestTrue(TEXT("clear succeeds"), ApplyFile(*this, Sidecar, Document(Package, Clear)).IsSuccess())) return false;
	UAnimSequence* Sequence = Fixture.Existing.Get();
	TestEqual(TEXT("[] cleared notifies, the state is kept"), Sequence->Notifies.Num(), 1);
	TestEqual(TEXT("[] cleared curves"), Sequence->GetDataModel()->GetFloatCurves().Num(), 0);
	TestEqual(TEXT("omitted markers kept"), Sequence->AuthoredSyncMarkers.Num(), 2);
	const TSharedPtr<FJsonObject> Committed = Extract(*this, ObjectPath);
	if (!Committed) return false;
	const TArray<uint8> CommittedBytes = Bytes(PackageFile(Package));

	// Mid-apply failure: the Editor batch writes the plain fields, then the
	// authoring edit rejects an unknown notify property -> Core restores the
	// package snapshot.
	auto Failing = RichBody(Fixture);
	Failing->GetObjectField(TEXT("Playback"))->SetNumberField(TEXT("RateScale"), 0.25);
	Failing->GetArrayField(TEXT("Notifies"))[0]->AsObject()->SetObjectField(TEXT("Properties"), Object({{TEXT("DoesNotExist"), Num(1)}}));
	FAssetDocumentResult Failed = ApplyFile(*this, Sidecar, Document(Package, Failing));
	TestFalse(TEXT("unknown notify property fails the apply"), Failed.IsSuccess());
	TestTrue(TEXT("failure reported at the authored notify"), HasDiagnostic(Failed, TEXT("/Body/Notifies/0/Properties"), TEXT("")));
	TestEqual(TEXT("rollback restores the observable document"), Json(Extract(*this, ObjectPath)), Json(Committed));
	TestTrue(TEXT("rollback keeps package bytes"), Bytes(PackageFile(Package)) == CommittedBytes);
	TestFalse(TEXT("rollback leaves the package clean"), Fixture.Existing->GetOutermost()->IsDirty());

	// Failure after every step published: the staging save is vetoed.
	{
		AddExpectedMessage(TEXT("is not allowed to save"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
		FScopedSaveVeto Veto(Package);
		Failed = ApplyFile(*this, Sidecar, Document(Package, RichBody(Fixture)));
		TestTrue(TEXT("save veto triggered"), Veto.bTriggered);
	}
	TestFalse(TEXT("vetoed save fails the apply"), Failed.IsSuccess());
	TestEqual(TEXT("save failure restores the observable document"), Json(Extract(*this, ObjectPath)), Json(Committed));
	TestTrue(TEXT("save failure keeps package bytes"), Bytes(PackageFile(Package)) == CommittedBytes);

	// Primitive failure inside the authoring edit (kept state on a removed track).
	auto Orphan = Object({{TEXT("NotifyTracks"), Arr({Obj(Object({{TEXT("Name"), Str(TEXT("Sound"))}}))})}});
	Failed = ApplyFile(*this, Sidecar, Document(Package, Orphan));
	TestFalse(TEXT("removing a used track fails"), Failed.IsSuccess());
	TestTrue(TEXT("TrackInUse reported"), HasDiagnostic(Failed, TEXT("/Body/NotifyTracks"), TEXT("NotifyTrackInUse")));
	TestEqual(TEXT("track failure restores the observable document"), Json(Extract(*this, ObjectPath)), Json(Committed));
	TestTrue(TEXT("original differs from committed"), OriginalBytes != CommittedBytes);
	return true;
}

#endif
