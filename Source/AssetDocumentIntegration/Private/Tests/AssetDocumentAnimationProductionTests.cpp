// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "AssetDocumentModule.h"
#include "AssetDocumentGenericRuntimeBootstrap.h"
#include "Tests/AssetDocumentSkeletonTestHarness.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "ReferenceSkeleton.h"
#include "Animation/AnimNotifies/AnimNotify_PlaySound.h"
#include "Animation/AnimNotifies/AnimNotifyState_TimedParticleEffect.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/PlatformProcess.h"
#include "Utils/AssetDocumentJsonKeys.h"

#if WITH_DEV_AUTOMATION_TESTS
FAssetDocumentGenericRuntimeBootstrap* GetAssetDocumentActiveGenericRuntimeBootstrapForTests();
namespace AnimationProductionTestsLocal
{
using namespace AssetDocumentSkeletonTests;
TSharedPtr<FJsonValue> OrderedJson(const TSharedPtr<FJsonValue>& Value)
{
	if (Value->Type == EJson::Object)
	{
		const auto Object = Value->AsObject();
		TArray<FString> Keys; AssetDocumentServerJson::GetKeys(*Object, Keys);
		Keys.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
		auto Ordered = MakeShared<FJsonObject>();
		for (const auto& Key : Keys) Ordered->SetField(Key, OrderedJson(AssetDocumentServerJson::FindFieldRef(*Object, Key)));
		return MakeShared<FJsonValueObject>(Ordered);
	}
	if (Value->Type == EJson::Array)
	{
		TArray<TSharedPtr<FJsonValue>> Elements;
		for (const auto& Element : Value->AsArray()) Elements.Add(OrderedJson(Element));
		return MakeShared<FJsonValueArray>(MoveTemp(Elements));
	}
	// Compare numbers by value: Extract writes floats as "1.0" while a JSON
	// round trip of a deep copy prints the same double as "1".
	if (Value->Type == EJson::Number) return MakeShared<FJsonValueNumber>(Value->AsNumber());
	return Value;
}
FString Json(const TSharedRef<FJsonObject>& Object)
{
	FString Text;
	// Canonicalize object key order and number spelling only: scalar values and
	// array order remain intact, including every C1 bit field and authored name spelling.
	FJsonSerializer::Serialize(OrderedJson(MakeShared<FJsonValueObject>(Object))->AsObject().ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
	return Text;
}
void Report(FAutomationTestBase& Test, const FAssetDocumentResult& Result)
{
	for (const auto& D : Result.Diagnostics) Test.AddInfo(D.Code + TEXT(": ") + D.Message);
}
TSharedPtr<FJsonObject> ExtractBody(FAutomationTestBase& Test, FAssets& Assets)
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = Assets.Target; Request.bDiffOnly = false; Request.bIncludeAllWritable = true;
	const auto Result = FAssetDocumentModule::Get().GetService().Extract(Request);
	Report(Test, Result);
	if (!Test.TestTrue(TEXT("production Extract reads native snapshot"), Result.IsSuccess()) || !Result.Payload) return {};
	const TSharedPtr<FJsonObject>* Body = nullptr;
	if (!Test.TestTrue(TEXT("production snapshot has Body"), Result.Payload->TryGetObjectField(TEXT("Body"), Body))) return {};
	return *Body;
}
FAssetDocumentResult ApplyBody(FAutomationTestBase& Test, FAssets& Assets, const TSharedRef<FJsonObject>& Body)
{
	FAssetDocumentExtractRequest ExtractRequest; ExtractRequest.AssetPath = Assets.Target;
	ExtractRequest.bDiffOnly = false; ExtractRequest.bIncludeAllWritable = true;
	const auto Extracted = FAssetDocumentModule::Get().GetService().Extract(ExtractRequest);
	Report(Test, Extracted);
	if (!Test.TestTrue(TEXT("production authoring starts from actual canonical document"), Extracted.IsSuccess() && Extracted.Payload.IsValid())) return {};
	Extracted.Payload->SetStringField(TEXT("Action"), TEXT("Update"));
	Extracted.Payload->SetObjectField(TEXT("Body"), Body);
	if (!Test.TestTrue(TEXT("write real authoring sidecar"), FFileHelper::SaveStringToFile(Json(Extracted.Payload.ToSharedRef()), *Assets.Sidecar))) return {};
	FAssetDocumentApplyFileRequest Request; Request.FilePath = Assets.Sidecar;
	Request.bSaveAsset = true;
	const auto Result = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
	Report(Test, Result);
	return Result;
}
struct FProductionAssets : FAssets
{
	TStrongObjectPtr<UAnimSequence> Source;
	~FProductionAssets()
	{
		if (bKeep || !Source.IsValid()) return;
		auto* Package = Source->GetOutermost();
		FAssetRegistryModule::AssetDeleted(Source.Get()); Source->ClearFlags(RF_Public | RF_Standalone);
		Source->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
		Source->MarkAsGarbage(); Package->SetDirtyFlag(false); Package->MarkAsGarbage();
	}
};
bool Setup(FAutomationTestBase& Test, FProductionAssets& Assets)
{
	auto* Bootstrap = GetAssetDocumentActiveGenericRuntimeBootstrapForTests();
	if (!Test.TestTrue(TEXT("actual production bootstrap is active"), Bootstrap && Bootstrap->IsActive())) return false;
	if (!Test.TestTrue(TEXT("saved fixture"), Assets.Setup())) return false;
	{ FReferenceSkeletonModifier Modifier(Assets.Old.Get()); Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity); }
	if (!Save(Assets.Old.Get())) return false;
	Assets.Source.Reset(NewObject<UAnimSequence>(CreatePackage(*(Assets.Root + TEXT("/Source"))), TEXT("Source"), RF_Public | RF_Standalone | RF_Transactional));
	Assets.Source->SetSkeleton(Assets.Old.Get()); FAssetRegistryModule::AssetCreated(Assets.Source.Get());
	auto& SourceController = Assets.Source->GetController();
	SourceController.InitializeModel(); SourceController.SetFrameRate(FFrameRate(30, 1), false);
	SourceController.SetNumberOfFrames(FFrameNumber(30), false);
	if (!SourceController.AddBoneCurve(TEXT("root"), false)) return false;
	TArray<FVector3f> Positions, Scales; TArray<FQuat4f> Rotations;
	Positions.Init(FVector3f::ZeroVector, 31); Scales.Init(FVector3f::OneVector, 31); Rotations.Init(FQuat4f::Identity, 31);
	if (!SourceController.SetBoneTrackKeys(TEXT("root"), Positions, Rotations, Scales, false)) return false;
	SourceController.NotifyPopulated(); Assets.Source->WaitOnExistingCompression();
	if (!Test.TestTrue(TEXT("save real one-second reference sequence"), Save(Assets.Source.Get()))) return false;
	auto& Controller = Assets.Montage->GetController();
	Controller.InitializeModel(); Controller.SetFrameRate(FFrameRate(30, 1), false);
	Controller.SetNumberOfFrames(FFrameNumber(30), false); Controller.NotifyPopulated();
	const FAnimationCurveIdentifier Id(TEXT("ProductionCurve"), ERawCurveTrackTypes::RCT_Float);
	if (!Controller.AddCurve(Id, AACF_Editable, false)) return false;
	FRichCurveKey Key(0.f, 1.f); Key.InterpMode = RCIM_Linear;
	if (!Controller.SetCurveKeys(Id, {Key}, false)) return false;
	Assets.Montage->SyncSlotIndex = INDEX_NONE;
	Assets.Montage->SlotAnimTracks.Reset();
	auto& Slot = Assets.Montage->SlotAnimTracks.AddDefaulted_GetRef(); Slot.SlotName = TEXT("DefaultSlot");
	FAnimSegment Segment; Segment.SetAnimReference(Assets.Source.Get());
	Segment.StartPos = 0.f; Segment.AnimStartTime = 0.f; Segment.AnimEndTime = 1.f;
	Segment.AnimPlayRate = 1.f; Segment.LoopingCount = 1; Slot.AnimTrack.AnimSegments.Add(Segment);
	if (Assets.Montage->AnimNotifyTracks.IsEmpty())
	{
		auto& Track = Assets.Montage->AnimNotifyTracks.AddDefaulted_GetRef(); Track.TrackName = TEXT("ProductionTrack");
	}
	Assets.Montage->RefreshCacheData();
	if (!Test.TestTrue(TEXT("save model fixture before production operations"), Save(Assets.Montage.Get()))) return false;
	return Test.TestEqual(TEXT("fixture has real one-second playback length"), Assets.Montage->GetPlayLength(), 1.f);
}
TSharedRef<FJsonObject> CurveEdit(const TSharedPtr<FJsonObject>& Snapshot, double ValueBits)
{
	// Work on a JSON copy; desired data cannot alias the observation retained by the test.
	TSharedPtr<FJsonObject> Copy;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json(Snapshot.ToSharedRef())), Copy);
	auto Curve = Copy->GetArrayField(TEXT("Curves"))[0]->AsObject();
	Curve->GetArrayField(TEXT("Keys"))[0]->AsObject()->GetObjectField(TEXT("Value"))->SetNumberField(TEXT("Bits"), ValueBits);
	auto Body = MakeShared<FJsonObject>(); Body->SetArrayField(TEXT("Curves"), Copy->GetArrayField(TEXT("Curves")));
	return Body;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimationProductionApplyFileTest,
	"UEAssetDoc.Animation.Production.ApplyFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAnimationProductionApplyFileTest::RunTest(const FString&)
{
	using namespace AnimationProductionTestsLocal;
	FProductionAssets Assets; if (!Setup(*this, Assets)) return false;
	auto Baseline = ExtractBody(*this, Assets); if (!Baseline) return false;
	const TCHAR* Keys[] = {TEXT("Skeleton"), TEXT("PreviewMesh"), TEXT("Curves"), TEXT("SlotAnimTracks"), TEXT("CompositeSections"), TEXT("Notifies"), TEXT("NotifyStates"), TEXT("NotifyTracks"),
		TEXT("Preview"), TEXT("Blend"), TEXT("Sync"), TEXT("RootMotion"), TEXT("TimeStretch"), TEXT("Metadata"), TEXT("SectionMetadata")};
	if (!TestEqual(TEXT("complete native observation: eight structural and seven settings keys"), Baseline->Values.Num(), 15)) return false;
	const auto SkeletonBytes = Bytes(PackageFile(Assets.Old->GetPathName()));
	const auto SourceBytes = Bytes(PackageFile(Assets.Source->GetPathName()));
	{
		auto& Service = FAssetDocumentModule::Get().GetService();
		FAssetDocumentExtractRequest ExtractRequest; ExtractRequest.AssetPath = Assets.Target;
		ExtractRequest.bDiffOnly = false; ExtractRequest.bIncludeAllWritable = true;
		const auto Extracted = Service.Extract(ExtractRequest); Report(*this, Extracted);
		if (!TestTrue(TEXT("production extracts complete document for read-only entrypoints"), Extracted.IsSuccess() && Extracted.Payload.IsValid())) return false;
		if (!TestTrue(TEXT("write actual extracted document as sidecar"), FFileHelper::SaveStringToFile(Json(Extracted.Payload.ToSharedRef()), *Assets.Sidecar))) return false;
		const auto TargetBytes = Bytes(PackageFile(Assets.Target)), SidecarBytes = Bytes(Assets.Sidecar);
		FAssetDocumentValidateRequest ValidateRequest; ValidateRequest.FilePath = Assets.Sidecar;
		const auto Validated = Service.Validate(ValidateRequest); Report(*this, Validated);
		if (!TestTrue(TEXT("production Service Validate accepts extracted sidecar"), Validated.IsSuccess())) return false;
		FAssetDocumentDiffRequest DiffRequest; DiffRequest.FilePath = Assets.Sidecar;
		const auto Diffed = Service.Diff(DiffRequest); Report(*this, Diffed);
		if (!TestTrue(TEXT("production Service Diff succeeds"), Diffed.IsSuccess() && Diffed.Payload.IsValid())) return false;
		for (const TCHAR* Key : {TEXT("changed"), TEXT("failed")})
		{
			const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
			if (!TestTrue(FString(TEXT("production diff exposes ")) + Key, Diffed.Payload->TryGetArrayField(Key, Rows))) return false;
			TestTrue(FString(TEXT("production diff has no ")) + Key, Rows->IsEmpty());
		}
		FAssetDocumentInspectRequest InspectRequest; InspectRequest.ClassOrAsset = Assets.Target;
		const auto Inspected = Service.Inspect(InspectRequest); Report(*this, Inspected);
		if (!TestTrue(TEXT("production live Inspect succeeds"), Inspected.IsSuccess() && Inspected.Payload.IsValid())) return false;
		const TSharedPtr<FJsonObject>* InspectedBody = nullptr;
		if (!TestTrue(TEXT("production Inspect exposes canonical Body"), Inspected.Payload->TryGetObjectField(TEXT("Body"), InspectedBody))) return false;
		TestEqual(TEXT("Inspect and Extract agree on all seven native surfaces"), Json(InspectedBody->ToSharedRef()), Json(Baseline.ToSharedRef()));
		TestTrue(TEXT("read-only Service entrypoints preserve target bytes"), Bytes(PackageFile(Assets.Target)) == TargetBytes);
		TestTrue(TEXT("read-only Service entrypoints preserve sidecar bytes"), Bytes(Assets.Sidecar) == SidecarBytes);
	}
	{
		const auto PackageBytes = Bytes(PackageFile(Assets.Target));
		const auto EmptyUpdate = ApplyBody(*this, Assets, MakeShared<FJsonObject>());
		if (!TestTrue(TEXT("empty sparse Update succeeds"), EmptyUpdate.IsSuccess())) return false;
		// An unchanged Update is a no-op: nothing is saved and the package file is untouched.
		TestFalse(TEXT("empty sparse Update is a no-op without a save"), EmptyUpdate.bSavedAsset);
		TestTrue(TEXT("empty sparse Update preserves package bytes"), Bytes(PackageFile(Assets.Target)) == PackageBytes);
		auto Observed = ExtractBody(*this, Assets); if (!Observed) return false;
		TestEqual(TEXT("empty sparse Update preserves all seven native surfaces"), Json(Observed.ToSharedRef()), Json(Baseline.ToSharedRef()));
		TestFalse(TEXT("empty sparse Update leaves target clean"), Assets.Montage->GetOutermost()->IsDirty());
	}
	for (const TCHAR* Key : Keys)
	{
		if (!TestTrue(TEXT("snapshot contains each admitted key"), Baseline->HasField(Key))) return false;
		auto Sparse = MakeShared<FJsonObject>(); Sparse->SetField(Key, Baseline->TryGetField(Key));
		// NotifyTracks travels with the notifies on those tracks (AnimMontage.NotifyTracksNeedNotifies).
		if (FCString::Strcmp(Key, TEXT("NotifyTracks")) == 0)
			for (const TCHAR* Family : {TEXT("Notifies"), TEXT("NotifyStates")}) Sparse->SetField(Family, Baseline->TryGetField(Family));
		const auto Result = ApplyBody(*this, Assets, Sparse);
		if (!TestTrue(FString(TEXT("production sparse ApplyFile: ")) + Key, Result.IsSuccess())) return false;
		if (!TestTrue(TEXT("sparse operation preserves positive playback duration for notify editing"), Assets.Montage->GetPlayLength() > 0.f)) return false;
		// Re-applying a key with its current value changes nothing, so it is a no-op without a save.
		TestFalse(TEXT("unchanged sparse key is a no-op without a save"), Result.bSavedAsset);
		auto Observed = ExtractBody(*this, Assets); if (!Observed) return false;
		TestEqual(TEXT("omitted keys remain unchanged"), Json(Observed.ToSharedRef()), Json(Baseline.ToSharedRef()));
	}
	{
		TSharedPtr<FJsonObject> FullBody;
		if (!TestTrue(TEXT("deep-copy complete seven-key intent"), FJsonSerializer::Deserialize(
			TJsonReaderFactory<>::Create(Json(Baseline.ToSharedRef())), FullBody))) return false;
		FullBody->SetArrayField(TEXT("Curves"), CurveEdit(Baseline, 1069547520.0)->GetArrayField(TEXT("Curves"))); // float32 1.5
		if (!TestTrue(TEXT("complete seven-key production intent commits a real curve change"), ApplyBody(*this, Assets, FullBody.ToSharedRef()).IsSuccess())) return false;
		auto Observed = ExtractBody(*this, Assets); if (!Observed) return false;
		TestEqual(TEXT("merged structure and notify plan preserves complete authored result"), Json(Observed.ToSharedRef()), Json(FullBody.ToSharedRef()));
	}
	// This changes a real nonempty key; the next operation shares a revision but
	// must obtain a new native owner and observe the first request's saved result.
	auto Edited = CurveEdit(Baseline, 1073741824.0); // float32 2.0
	if (!TestTrue(TEXT("nonempty curve edit commits through production"), ApplyBody(*this, Assets, Edited).IsSuccess())) return false;
	const auto* Curve = Assets.Montage->GetDataModel()->FindFloatCurve(FAnimationCurveIdentifier(TEXT("ProductionCurve"), ERawCurveTrackTypes::RCT_Float));
	if (!TestNotNull(TEXT("real authored curve exists"), Curve)) return false;
	TestEqual(TEXT("real curve value changed"), Curve->FloatCurve.GetConstRefOfKeys()[0].Value, 2.f);
	auto Committed = ExtractBody(*this, Assets); if (!Committed) return false;
	const auto CommittedBytes = Bytes(PackageFile(Assets.Target));
	auto InvalidCombination = CurveEdit(Committed, 1077936128.0); // float32 3.0
	InvalidCombination->SetStringField(TEXT("Skeleton"), Assets.Root + TEXT("/Missing.Missing"));
	TestFalse(TEXT("combined intent with unavailable dependency fails"), ApplyBody(*this, Assets, InvalidCombination).IsSuccess());
	TestTrue(TEXT("failed dependency leaves committed package bytes intact"), Bytes(PackageFile(Assets.Target)) == CommittedBytes);
	auto AfterFailure = ExtractBody(*this, Assets); if (!AfterFailure) return false;
	TestEqual(TEXT("failed combination leaves all seven observed surfaces intact"), Json(AfterFailure.ToSharedRef()), Json(Committed.ToSharedRef()));
	auto Next = CurveEdit(Committed, 1077936128.0); Next->SetField(TEXT("PreviewMesh"), Committed->TryGetField(TEXT("PreviewMesh")));
	TestTrue(TEXT("next valid combination succeeds after failure"), ApplyBody(*this, Assets, Next).IsSuccess());
	Curve = Assets.Montage->GetDataModel()->FindFloatCurve(FAnimationCurveIdentifier(TEXT("ProductionCurve"), ERawCurveTrackTypes::RCT_Float));
	if (TestNotNull(TEXT("curve survives subsequent request"), Curve)) TestEqual(TEXT("subsequent request has its own prepared state"), Curve->FloatCurve.GetConstRefOfKeys()[0].Value, 3.f);
	auto BeforeRollback = ExtractBody(*this, Assets); if (!BeforeRollback) return false;
	const auto BeforeRollbackBytes = Bytes(PackageFile(Assets.Target));
	{
		FAssetDocumentAnimationFloatCurveFailureScope Failure(Assets.Target);
		if (!TestTrue(TEXT("target-bound native forward failure is armed"), Failure.IsArmed())) return false;
		auto RollbackIntent = CurveEdit(BeforeRollback, 1082130432.0); // float32 4.0
		RollbackIntent->SetField(TEXT("PreviewMesh"), BeforeRollback->TryGetField(TEXT("PreviewMesh")));
		const auto Failed = ApplyBody(*this, Assets, RollbackIntent);
		TestFalse(TEXT("real forward publication failure aborts production ApplyFile"), Failed.IsSuccess());
		TestTrue(TEXT("native first patch actually ran before failure"), Failure.WasTriggered());
		TestTrue(TEXT("Core preserves the real native completion failure"), Failed.Diagnostics.ContainsByPredicate(
			[](const auto& D) { return D.Code == TEXT("AnimationProvider.CompletionFailed"); }));
		TestFalse(TEXT("failed publication does not claim a saved asset"), Failed.bSavedAsset);
	}
	auto RolledBack = ExtractBody(*this, Assets); if (!RolledBack) return false;
	TestEqual(TEXT("typed inverse restores complete seven-key observation"), Json(RolledBack.ToSharedRef()), Json(BeforeRollback.ToSharedRef()));
	TestTrue(TEXT("failed forward leaves durable target unchanged"), Bytes(PackageFile(Assets.Target)) == BeforeRollbackBytes);
	TestFalse(TEXT("typed rollback restores original clean state"), Assets.Montage->GetOutermost()->IsDirty());
	TestTrue(TEXT("valid request after inverse succeeds with fresh prepared state"), ApplyBody(*this, Assets, CurveEdit(RolledBack, 1073741824.0)).IsSuccess());
	{
		auto BeforeClear = ExtractBody(*this, Assets); if (!BeforeClear) return false;
		if (!TestTrue(TEXT("curve is nonempty before explicit clear"), !BeforeClear->GetArrayField(TEXT("Curves")).IsEmpty())) return false;
		auto Clear = MakeShared<FJsonObject>(); Clear->SetArrayField(TEXT("Curves"), {});
		if (!TestTrue(TEXT("explicit empty Curves array commits"), ApplyBody(*this, Assets, Clear).IsSuccess())) return false;
		auto Cleared = ExtractBody(*this, Assets); if (!Cleared) return false;
		TestTrue(TEXT("explicit empty array removes curves rather than omitting the operation"), Cleared->GetArrayField(TEXT("Curves")).IsEmpty());
		BeforeClear->SetArrayField(TEXT("Curves"), {});
		TestEqual(TEXT("curve clear preserves the other six surfaces"), Json(Cleared.ToSharedRef()), Json(BeforeClear.ToSharedRef()));
	}
	TestFalse(TEXT("committed target is clean"), Assets.Montage->GetOutermost()->IsDirty());
	TestTrue(TEXT("referenced skeleton remains byte-identical"), Bytes(PackageFile(Assets.Old->GetPathName())) == SkeletonBytes);
	TestTrue(TEXT("referenced source sequence remains byte-identical"), Bytes(PackageFile(Assets.Source->GetPathName())) == SourceBytes);
	return true;
}

namespace AnimationProductionTestsLocal
{
FAssetDocumentResult CreateSimple(FAutomationTestBase& Test, FAssets& Assets)
{
 FAssetDocumentTemplateRequest Template; Template.Class = TEXT("/Script/Engine.AnimMontage");
 Template.Target = FPackageName::ObjectPathToPackageName(Assets.Target);
 auto Document = FAssetDocumentModule::Get().GetService().CreateTemplate(Template); Report(Test, Document);
 if (!Test.TestTrue(TEXT("actual production CreateTemplate succeeds"), Document.IsSuccess() && Document.Payload.IsValid())) return {};
 Document.Payload->SetStringField(TEXT("Action"), TEXT("Create"));
 auto Body = MakeShared<FJsonObject>(); Body->SetStringField(TEXT("Skeleton"), Assets.Old->GetPathName());
 Document.Payload->SetObjectField(TEXT("Body"), Body);
 if (!Test.TestTrue(TEXT("write simple Create canonical sidecar"), FFileHelper::SaveStringToFile(Json(Document.Payload.ToSharedRef()), *Assets.Sidecar))) return {};
 FAssetDocumentApplyFileRequest Request; Request.FilePath = Assets.Sidecar;
 const auto Result = FAssetDocumentModule::Get().GetService().ApplyFile(Request); Report(Test, Result); return Result;
}
TSharedRef<FJsonObject> NonemptyCombination(const TSharedPtr<FJsonObject>& Baseline, int32 Loops = 2, double Volume = 0.25)
{
 TSharedPtr<FJsonObject> Body;
 FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json(Baseline.ToSharedRef())), Body);
 Body->RemoveField(TEXT("Skeleton")); Body->RemoveField(TEXT("PreviewMesh"));
 Body->SetArrayField(TEXT("Curves"), CurveEdit(Baseline, Loops == 2 ? 1073741824.0 : 1082130432.0)->GetArrayField(TEXT("Curves")));
 Body->GetArrayField(TEXT("SlotAnimTracks"))[0]->AsObject()->GetArrayField(TEXT("Segments"))[0]->AsObject()->SetNumberField(TEXT("LoopingCount"), Loops);
 TArray<TSharedPtr<FJsonValue>> Sections;
 for (int32 I = 0; I < 2; ++I)
 {
  auto Row = MakeShared<FJsonObject>(); Row->SetStringField(TEXT("Name"), I == 0 ? TEXT("First") : TEXT("Second"));
  Row->SetStringField(TEXT("NextSectionName"), I == 0 ? TEXT("Second") : TEXT("First"));
  Row->SetNumberField(TEXT("LinkableTime"), I); Row->SetStringField(TEXT("SlotName"), TEXT("DefaultSlot"));
  Row->SetNumberField(TEXT("LinkMethod"), 0); Sections.Add(MakeShared<FJsonValueObject>(Row));
 }
 Body->SetArrayField(TEXT("CompositeSections"), Sections);
 for (const bool State : {false, true})
 {
  auto Row = MakeShared<FJsonObject>(); Row->SetNumberField(TEXT("Time"), State ? 0.875 : 0.125);
  Row->SetNumberField(TEXT("TrackIndex"), 0); if (State) Row->SetNumberField(TEXT("Duration"), 0.125);
  auto Object = MakeShared<FJsonObject>(), Properties = MakeShared<FJsonObject>();
  Object->SetStringField(TEXT("Class"), State ? TEXT("/Script/Engine.AnimNotifyState_TimedParticleEffect") : TEXT("/Script/Engine.AnimNotify_PlaySound"));
  if (State) Properties->SetBoolField(TEXT("bDestroyAtEnd"), Loops != 2); else Properties->SetNumberField(TEXT("VolumeMultiplier"), Volume);
  Object->SetObjectField(TEXT("Properties"), Properties); Row->SetObjectField(TEXT("Object"), Object);
  Body->SetArrayField(State ? TEXT("NotifyStates") : TEXT("Notifies"), {MakeShared<FJsonValueObject>(Row)});
 }
 return Body.ToSharedRef();
}
bool HasNonemptyState(const UAnimMontage& Target, int32 Loops, float Volume)
{
 if (Target.GetPlayLength() != float(Loops) || Target.SlotAnimTracks.Num() != 1
  || Target.SlotAnimTracks[0].AnimTrack.AnimSegments.Num() != 1
  || Target.SlotAnimTracks[0].AnimTrack.AnimSegments[0].LoopingCount != Loops
  || Target.CompositeSections.Num() != 2 || Target.Notifies.Num() != 2) return false;
 if (Target.CompositeSections[0].SectionName != TEXT("First") || Target.CompositeSections[0].NextSectionName != TEXT("Second")
  || Target.CompositeSections[1].SectionName != TEXT("Second") || Target.CompositeSections[1].NextSectionName != TEXT("First")
  || Target.CompositeSections[0].GetTime() != 0.f || Target.CompositeSections[1].GetTime() != 1.f) return false;
 bool FoundNotify = false, FoundState = false;
 for (const auto& Event : Target.Notifies)
 {
  if (const auto* Notify = Cast<UAnimNotify_PlaySound>(Event.Notify.Get()))
   FoundNotify = Notify->GetOuter() == &Target && Notify->VolumeMultiplier == Volume && Event.GetTime() == 0.125f
    && Event.NotifyName == TEXT("AssetDocument.Notify");
  if (const auto* State = Cast<UAnimNotifyState_TimedParticleEffect>(Event.NotifyStateClass.Get()))
   FoundState = State->GetOuter() == &Target && State->bDestroyAtEnd == (Loops != 2) && Event.GetTime() == 0.875f
    && Event.GetDuration() == 0.125f && Event.NotifyName == TEXT("AssetDocument.NotifyState");
 }
 return FoundNotify && FoundState;
}
// Use stock SavePackage admission after all typed publications. This observes
// actual live state and rejects only this test target's request staging save.
class FScopedProductionSaveVeto
{
 FCoreUObjectDelegates::FIsPackageOKToSaveDelegate Previous;
 FString Target;
 TFunction<bool(const UAnimMontage&)> Observe;
public:
 bool bTriggered = false, bPublishedStateObserved = false;
 FScopedProductionSaveVeto(FString InTarget, TFunction<bool(const UAnimMontage&)> InObserve)
  : Previous(FCoreUObjectDelegates::IsPackageOKToSaveDelegate), Target(MoveTemp(InTarget)), Observe(MoveTemp(InObserve))
 {
  FCoreUObjectDelegates::IsPackageOKToSaveDelegate.BindLambda([this](UPackage* Package, const FString& Filename, FOutputDevice* Error)
  {
   if (Previous.IsBound() && !Previous.Execute(Package, Filename, Error)) return false;
   const FString Full = FPaths::ConvertRelativePathToFull(Filename);
   if (!bTriggered && Package && Package->GetName() == FPackageName::ObjectPathToPackageName(Target)
    && Full.Contains(TEXT("/Saved/AssetDocument/Runtime/Staging/")))
   {
    const auto* Montage = FindObject<UAnimMontage>(nullptr, *Target);
    bPublishedStateObserved = Montage && Observe(*Montage); bTriggered = true; return false;
   }
   return true;
  });
 }
 ~FScopedProductionSaveVeto() { FCoreUObjectDelegates::IsPackageOKToSaveDelegate = Previous; }
};

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimationProductionSimpleCreateTest,
 "UEAssetDoc.Animation.Followthrough.SimpleCreate", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAnimationProductionSimpleCreateTest::RunTest(const FString&)
{
 using namespace AnimationProductionTestsLocal;
 FAssets Assets; if (!TestTrue(TEXT("only existing Skeleton references are prepared"), Assets.Setup(false))) return false;
 if (!TestNull(TEXT("Create target is actually absent"), FindObject<UAnimMontage>(nullptr, *Assets.Target))) return false;
 if (!TestFalse(TEXT("Create target has no package file"), IFileManager::Get().FileExists(*PackageFile(Assets.Target)))) return false;
 const auto Created = CreateSimple(*this, Assets);
 if (!TestTrue(TEXT("real simple Create saves through staged Verify"), Created.IsSuccess() && Created.bSavedAsset)) return false;
 Assets.Montage.Reset(FindObject<UAnimMontage>(nullptr, *Assets.Target));
 if (!TestNotNull(TEXT("factory created a real Montage"), Assets.Montage.Get())) return false;
 TestEqual(TEXT("simple Create actual Skeleton"), Assets.Montage->GetSkeleton(), Assets.Old.Get());
 TestFalse(TEXT("created target is clean"), Assets.Montage->GetOutermost()->IsDirty());
 const auto Before = Bytes(PackageFile(Assets.Target));
 const auto Repeated = CreateSimple(*this, Assets);
 TestFalse(TEXT("Create refuses an already existing target"), Repeated.IsSuccess());
 TestTrue(TEXT("existing-target refusal preserves created bytes"), Bytes(PackageFile(Assets.Target)) == Before);
 auto Body = ExtractBody(*this, Assets); if (!Body) return false;
 FAssets Failed; if (!Failed.Setup(false)) return false;
 const auto SkeletonBytes = Bytes(PackageFile(Failed.Old->GetPathName()));
 TArray<uint8> FailedCreateSidecar;
 {
  FScopedProductionSaveVeto Veto(Failed.Target, [&](const UAnimMontage& Target)
   { FailedCreateSidecar = Bytes(Failed.Sidecar); return Target.GetSkeleton() == Failed.Old.Get() && Target.GetOutermost()->IsDirty(); });
  const auto Result = CreateSimple(*this, Failed);
  TestTrue(TEXT("late Create save veto actually executes"), Veto.bTriggered);
  TestTrue(TEXT("late failure observes actual factory-created target"), Veto.bPublishedStateObserved);
  TestFalse(TEXT("created target rolls back after actual persistence failure"), Result.IsSuccess());
  TestTrue(TEXT("Create returns the real package staging failure"), Result.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("Persistence.StagePackageFailed"); }));
  TestFalse(TEXT("failed Create never claims saved asset"), Result.bSavedAsset);
 }
 TestTrue(TEXT("failed Create preserves authored sidecar bytes"), !FailedCreateSidecar.IsEmpty() && FailedCreateSidecar == Bytes(Failed.Sidecar));
 TestNull(TEXT("failed Create removes live target"), FindObject<UAnimMontage>(nullptr, *Failed.Target));
 TestNull(TEXT("failed Create removes live package"), FindPackage(nullptr, *FPackageName::ObjectPathToPackageName(Failed.Target)));
 TestFalse(TEXT("failed Create leaves no package file"), IFileManager::Get().FileExists(*PackageFile(Failed.Target)));
 auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
 TestFalse(TEXT("failed Create leaves no asset registry entry"), Registry.GetAssetByObjectPath(FSoftObjectPath(Failed.Target)).IsValid());
 TestTrue(TEXT("failed Create preserves referenced Skeleton bytes"), Bytes(PackageFile(Failed.Old->GetPathName())) == SkeletonBytes);
 TestTrue(TEXT("same target can be created after failed cleanup"), CreateSimple(*this, Failed).IsSuccess());
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimationProductionNonemptyCombinationTest,
 "UEAssetDoc.Animation.Followthrough.NonemptyCombination", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAnimationProductionNonemptyCombinationTest::RunTest(const FString&)
{
 using namespace AnimationProductionTestsLocal;
 FProductionAssets Assets; if (!Setup(*this, Assets)) return false;
 auto Baseline = ExtractBody(*this, Assets); if (!Baseline) return false;
 if (!TestTrue(TEXT("production nonempty structure and both managed families commit"), ApplyBody(*this, Assets, NonemptyCombination(Baseline)).IsSuccess())) return false;
 if (!TestTrue(TEXT("real structure, section cycle and configured managed objects changed"), HasNonemptyState(*Assets.Montage, 2, 0.25f))) return false;
 TestEqual(TEXT("structure completion updated actual model frame count"), Assets.Montage->GetDataModel()->GetNumberOfFrames(), 60);
 auto Committed = ExtractBody(*this, Assets); if (!Committed) return false;
 if (!TestTrue(TEXT("later sparse curve update preserves omitted structure and notifications"), ApplyBody(*this, Assets, CurveEdit(Committed, 1077936128.0)).IsSuccess())) return false;
 TestTrue(TEXT("omitted nonempty managed families and structure preserved"), HasNonemptyState(*Assets.Montage, 2, 0.25f));
 auto BeforeFailure = ExtractBody(*this, Assets); if (!BeforeFailure) return false;
 const auto PackageBytes = Bytes(PackageFile(Assets.Target));
 const auto SourceBytes = Bytes(PackageFile(Assets.Source->GetPathName()));
 const auto SkeletonBytes = Bytes(PackageFile(Assets.Old->GetPathName()));
 TArray<uint8> FailedCombinationSidecar;
 {
  FScopedProductionSaveVeto Veto(Assets.Target, [&](const UAnimMontage& Target) { FailedCombinationSidecar = Bytes(Assets.Sidecar); return HasNonemptyState(Target, 3, 0.75f); });
  const auto Failed = ApplyBody(*this, Assets, NonemptyCombination(BeforeFailure, 3, 0.75));
  TestTrue(TEXT("late combined save veto actually executes"), Veto.bTriggered);
  TestTrue(TEXT("failure occurs after changed structure and both managed families publish"), Veto.bPublishedStateObserved);
  TestFalse(TEXT("late combined persistence failure aborts"), Failed.IsSuccess());
  TestTrue(TEXT("combined failure returns the real package staging diagnostic"), Failed.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("Persistence.StagePackageFailed"); }));
 }
 TestTrue(TEXT("late failure preserves authored sidecar bytes"), !FailedCombinationSidecar.IsEmpty() && FailedCombinationSidecar == Bytes(Assets.Sidecar));
 TestTrue(TEXT("typed inverse restores section cycle and both configured managed families"), HasNonemptyState(*Assets.Montage, 2, 0.25f));
 auto Restored = ExtractBody(*this, Assets); if (!Restored) return false;
 TestEqual(TEXT("typed inverse restores all seven canonical surfaces"), Json(Restored.ToSharedRef()), Json(BeforeFailure.ToSharedRef()));
 TestTrue(TEXT("late failure preserves durable target bytes"), PackageBytes == Bytes(PackageFile(Assets.Target)));
 TestTrue(TEXT("late failure preserves source bytes"), SourceBytes == Bytes(PackageFile(Assets.Source->GetPathName())));
 TestTrue(TEXT("late failure preserves Skeleton bytes"), SkeletonBytes == Bytes(PackageFile(Assets.Old->GetPathName())));
 TestFalse(TEXT("late inverse restores clean state"), Assets.Montage->GetOutermost()->IsDirty());
 TestTrue(TEXT("next request succeeds after combined inverse"), ApplyBody(*this, Assets, NonemptyCombination(Restored)).IsSuccess());
 return !HasAnyErrors();
}

#endif
