// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Sequencer primitives (provider "seq", owner Cinematics) of the Level Sequence profile
// (docs/architecture/asset-document-sequencer-design.md §4.4). Phases S1 (read) and S2 (core authoring):
//   seq.read.v1     the authored state of a sequence as Sequencer shows it (AssetDocumentSequencerRead.cpp)
//   seq.status.v1   the read checks of design §5.1 as a compile status (AssetDocumentSequencerStatus.cpp)
//   seq.edit.v1     Sequencer's edits without an open Sequencer (AssetDocumentSequencerEdit*.cpp, mutating)
//   seq.inspect.v1  locator resolution, animatable properties, channel catalogue, director endpoints, references and
//                   status (AssetDocumentSequencerInspect.cpp)
//   seq.catalog.v1  the list_types kinds sequencer_track / sequencer_section / sequencer_binding
//                   (AssetDocumentSequencerCatalog.cpp)
// Only seq.edit.v1 changes the asset (one transaction per call; Core snapshots the package for rollback).

#include "AssetDocumentSequencerPrimitives.h"

#include "AssetDocumentGraphProvider.h"
#include "AssetDocumentSequencerProvider.h"

namespace UE::AssetDocument::Graph::Sequencer
{
namespace
{
constexpr uint32 SequencerProviderVersion = 1;

FAssetDocumentPrimitiveDescriptor MakeDescriptor(const TCHAR* PrimitiveId, bool bMutating, const TCHAR* AuthoritativeApi)
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = PrimitiveId;
	Descriptor.Version = SequencerProviderVersion;
	Descriptor.ProviderId = ProviderId;
	Descriptor.Owner = EAssetDocumentPrimitiveOwner::Cinematics;
	Descriptor.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
	Descriptor.Access = bMutating ? EAssetDocumentPrimitiveAccess::Mutating : EAssetDocumentPrimitiveAccess::ReadOnly;
	if (bMutating)
	{
		Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair};
		Descriptor.MutationFootprintSchemaId = TEXT("graph.footprint.whole-package.v1");
	}
	else
	{
		Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Validate, EAssetDocumentRuntimeRequestPhase::Preflight,
			EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair,
			EAssetDocumentRuntimeRequestPhase::Verify};
	}
	Descriptor.bRequiresActiveTransaction = bMutating;
	Descriptor.InputSchemaId = Descriptor.PrimitiveId + TEXT(".input.v1");
	Descriptor.OutputSchemaId = Descriptor.PrimitiveId + TEXT(".output.v1");
	Descriptor.StableDiagnosticCodes = {
		TEXT("AD.RUNTIME.PRIMITIVE_FAILED"), TEXT("Graph.UnknownField"), TEXT("Graph.InvalidArgument"),
		TEXT("Graph.UnsupportedSchemaVersion"), TEXT("Graph.PieActive"), TEXT("Graph.ResultTooLarge"),
		TEXT("Sequencer.UnsupportedAsset"), TEXT("Sequencer.ReadFailed"), TEXT("Sequencer.UnknownCatalogKind"),
		TEXT("Sequencer.UnsupportedValue"), TEXT("Sequencer.UnsupportedChannel"), TEXT("Sequencer.UnsupportedChannelOverride"),
		TEXT("Sequencer.UnsupportedRemoteBinding"), TEXT("Sequencer.UnsupportedBindingShape"),
		TEXT("Sequencer.UnsupportedLocator"), TEXT("Sequencer.ControlRigTrack"), TEXT("Sequencer.DuplicateChannelName"),
		// seq.edit.v1
		TEXT("Sequencer.InteractiveEditRefused"), TEXT("Sequencer.EditFailed"), TEXT("Sequencer.InvalidValue"),
		TEXT("Sequencer.InvalidRange"), TEXT("Sequencer.InvalidBody"), TEXT("Sequencer.FieldNotEditable"), TEXT("Sequencer.UnknownBinding"),
		TEXT("Sequencer.BindingExists"), TEXT("Sequencer.InvalidLocator"), TEXT("Sequencer.InvalidBindingClass"),
		TEXT("Sequencer.InvalidSpawnableClass"), TEXT("Sequencer.UnknownComponent"), TEXT("Sequencer.UnknownTrackType"),
		TEXT("Sequencer.TrackNotSupported"), TEXT("Sequencer.UnknownTrack"), TEXT("Sequencer.UnknownSection"),
		TEXT("Sequencer.InvalidSectionClass"), TEXT("Sequencer.SectionClassChanged"), TEXT("Sequencer.SectionRangeRequired"),
		TEXT("Sequencer.CircularSubsequence"), TEXT("Sequencer.UnknownChannel"), TEXT("Sequencer.InvalidKey"), TEXT("Sequencer.InvalidFolder"),
		// director, events, groups, remote references (S5)
		TEXT("Sequencer.DirectorRequired"), TEXT("Sequencer.UnknownEventEndpoint"), TEXT("Sequencer.UnknownEventPin"),
		TEXT("Sequencer.DirectorInUse"), TEXT("Sequencer.DirectorUnsupported"), TEXT("Sequencer.DirectorCreateFailed"),
		TEXT("Sequencer.UnknownSequence"),
		// template sequences
		TEXT("Sequencer.RootClassInvalid"), TEXT("Sequencer.CameraClassRequired")};
	Descriptor.RequiredConsumerIds = {TEXT("route.generic.v1")};
	Descriptor.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
	Descriptor.HandleArgumentSchemas.Add({TEXT("asset"), EAssetDocumentHandleKind::Asset,
		bMutating ? EAssetDocumentHandleAccess::Mutable : EAssetDocumentHandleAccess::ReadOnly, true});

	FAssetDocumentPrimitiveAdmissionEvidence& Evidence = Descriptor.Admission;
	Evidence.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
	Evidence.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper;
	Evidence.ExactEngineVersion = TEXT("5.7.4");
	Evidence.PhysicalOwner = EAssetDocumentPrimitiveOwner::Cinematics;
	Evidence.PublicModule = TEXT("AssetDocumentSequencer");
	Evidence.PublicHeader = TEXT("AssetDocumentSequencerProvider.h");
	Evidence.ExportedAuthoritativeApi = AuthoritativeApi;
	Evidence.AuthorityContextId = TEXT("assetdocument.editor.game-thread.editor-ready.v1");
	Evidence.PrimaryOperationId = Descriptor.PrimitiveId;
	Evidence.IdentityLifetimeContractId = TEXT("identity.request-transaction-revision-bound.v1");
	Evidence.TransactionContractId = TEXT("transaction.descriptor-governed.v1");
	Evidence.RollbackContractId = TEXT("rollback.core-package-snapshot.v1");
	Evidence.CompletionContractId = Descriptor.PrimitiveId + TEXT(".completion.v1");
	Evidence.SaveReloadEvidenceId = bMutating ? TEXT("UEAssetDoc.Sequencer.Apply.CorpusRoundTrip")
		: TEXT("UEAssetDoc.Sequencer.Read");
	Evidence.FailureInjectionTestIds = {bMutating ? TEXT("UEAssetDoc.Sequencer.Apply.Rollback")
		: TEXT("UEAssetDoc.Sequencer.ReadOnly")};
	Evidence.VerificationTestIds = {bMutating ? TEXT("UEAssetDoc.Sequencer.Apply.FreeEdits")
		: TEXT("UEAssetDoc.Sequencer.Corpus")};
	Evidence.AuthorityFingerprint = FString::Printf(TEXT("ue-5.7.4|seq|%s|%s"), PrimitiveId, AuthoritativeApi);
	return Descriptor;
}

TArray<FAssetDocumentPrimitiveDescriptor> BuildDescriptors()
{
	return {
		MakeDescriptor(TEXT("seq.read.v1"), false,
			TEXT("UMovieSceneSequence::GetMovieScene;UMovieSceneSequence::GetBindingReferences;UMovieScene::GetPossessable;"
				"UMovieScene::GetBindings;UMovieScene::GetTracks;UMovieScene::GetCameraCutTrack;UMovieScene::GetRootFolders;"
				"UMovieScene::GetMarkedFrames;UMovieScene::GetObjectDisplayName;UMovieSceneSection::GetChannelProxy;"
				"FUniversalObjectLocator::ToString;ULevelSequence::GetDirectorBlueprint")),
		MakeDescriptor(TEXT("seq.inspect.v1"), false,
			TEXT("FUniversalObjectLocator::SyncFind;UEditorEngine::GetEditorWorldContext;ISequencerModule::CanAnimateProperty;"
				"UMovieSceneSection::GetChannelProxy;IAssetRegistry::GetDependencies;UMovieSceneSequence::IsTrackSupported")),
		MakeDescriptor(TEXT("seq.catalog.v1"), false,
			TEXT("UMovieSceneSequence::IsTrackSupported;UMovieScene::IsTrackClassAllowed;UMovieSceneTrack::SupportsType;"
				"UMovieSceneSection::GetChannelProxy;MovieSceneHelpers::GetPrioritySortedCustomBindingTypes")),
		MakeDescriptor(TEXT("seq.status.v1"), false,
			TEXT("UMovieSceneSequence::GetMovieScene;UMovieSceneSection::GetChannelProxy;UMovieSceneSubSection::GetSequence;"
				"FUniversalObjectLocator::ToString")),
		MakeDescriptor(TEXT("seq.edit.v1"), true,
			TEXT("UE::MovieScene::TimeHelpers::MigrateFrameTimes;UMovieScene::SetTickResolutionDirectly;UMovieScene::SetDisplayRate;"
				"UMovieScene::SetPlaybackRange;UMovieScene::AddPossessable;UMovieScene::RemovePossessable;UMovieScene::SetObjectDisplayName;"
				"FMovieSceneBindingReferences::AddBinding;FMovieSceneBindingReferences::RemoveBinding;"
				"UMovieSceneCustomBinding::CreateNewCustomBinding;UMovieScene::AddTrack;UMovieScene::AddCameraCutTrack;UMovieScene::RemoveTrack;"
				"UMovieScenePropertyTrack::SetPropertyNameAndPath;UMovieSceneTrack::CreateNewSection;UMovieSceneTrack::AddSection;"
				"UMovieSceneTrack::RemoveSection;UMovieSceneTrack::UpdateEasing;UMovieSceneSection::SetRange;"
				"FMovieSceneFloatChannel::Set;FMovieSceneFloatChannel::AutoSetTangents;UMovieSceneFolder::AddChildObjectBinding;"
				"UMovieSceneFolder::AddChildTrack;UMovieSceneMetaData::SetAuthor;ILevelSequenceModule::GenerateObjectSpawners;"
				"IMovieSceneObjectSpawner::CreateNewSpawnableType;UMovieScene::AddSpawnable;UMovieScene::MoveBindingContents;"
				"UMovieScene::RemoveSpawnable")),
	};
}

const FBinding Bindings[] = {
	{TEXT("seq.read.v1"), &HandleRead},
	{TEXT("seq.inspect.v1"), &HandleInspect},
	{TEXT("seq.catalog.v1"), &HandleCatalog},
	{TEXT("seq.status.v1"), &HandleStatus},
	{TEXT("seq.edit.v1"), &HandleEdit},
};
} // namespace

bool Fail(FCall& Call, FStringView Code, FStringView Message, FStringView Path)
{
	if (Call.Error.Code.IsEmpty())
	{
		Call.Error.Set(Code, Message, Path);
	}
	return false;
}

double ShortestFloat(double Value)
{
	const float Single = static_cast<float>(Value);
	if (!FMath::IsFinite(Single))
	{
		return Value;
	}
	for (int32 Digits = 1; Digits <= 9; ++Digits)
	{
		const FString Text = FString::Printf(TEXT("%.*g"), Digits, static_cast<double>(Single));
		const double Parsed = FCString::Atod(*Text);
		if (static_cast<float>(Parsed) == Single)
		{
			return Parsed;
		}
	}
	return Single;
}

FValue MakeName(FName Name)
{
	return MakeString(Name.IsNone() ? FString() : Name.ToString());
}

FString Escape(const FString& Name)
{
	return Name.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
}

void AddRecordPage(TArray<FValue>& Records, int64 Offset, FObjectBuilder& Result)
{
	TArray<FValue> Page;
	int32 NodeCount = 8;
	int32 Index = static_cast<int32>(FMath::Clamp<int64>(Offset, 0, Records.Num()));
	for (; Index < Records.Num(); ++Index)
	{
		const int32 RowNodes = CountValueNodes(Records[Index]);
		if (!Page.IsEmpty() && NodeCount + RowNodes > QueryValueNodeBudget)
		{
			break;
		}
		NodeCount += RowNodes;
		Page.Add(MoveTemp(Records[Index]));
	}
	Result.AddInt(TEXT("total"), Records.Num());
	Result.AddInt(TEXT("next_offset"), Index < Records.Num() ? Index : -1);
	Result.Add(TEXT("records"), MakeArray(MoveTemp(Page)));
}

// ---- time ----------------------------------------------------------------------------------------------------------

bool IsWholeFrame(int64 Ticks, const FTimeBase& Time)
{
	// frames = ticks * (Display.N * Tick.D) / (Display.D * Tick.N)
	const int64 Numerator = Ticks * static_cast<int64>(Time.Display.Numerator) * static_cast<int64>(Time.Tick.Denominator);
	const int64 Denominator = static_cast<int64>(Time.Display.Denominator) * static_cast<int64>(Time.Tick.Numerator);
	return Denominator != 0 && Numerator % Denominator == 0;
}

FValue MakeFrames(int64 Ticks, const FTimeBase& Time)
{
	const int64 Numerator = Ticks * static_cast<int64>(Time.Display.Numerator) * static_cast<int64>(Time.Tick.Denominator);
	const int64 Denominator = static_cast<int64>(Time.Display.Denominator) * static_cast<int64>(Time.Tick.Numerator);
	if (Denominator == 0)
	{
		return MakeInt(Ticks);
	}
	if (Numerator % Denominator == 0)
	{
		return MakeInt(Numerator / Denominator);
	}
	const double Exact = static_cast<double>(Numerator) / static_cast<double>(Denominator);
	for (int32 Digits = 1; Digits <= 17; ++Digits)
	{
		const FString Text = FString::Printf(TEXT("%.*g"), Digits, Exact);
		const double Parsed = FCString::Atod(*Text);
		if (TicksOf(Parsed, Time) == Ticks)
		{
			return MakeNumber(Parsed);
		}
	}
	return MakeNumber(Exact);
}

FValue MakeRate(const FFrameRate& Rate)
{
	FObjectBuilder Object;
	Object.AddInt(TEXT("Numerator"), Rate.Numerator);
	Object.AddInt(TEXT("Denominator"), Rate.Denominator);
	return Object.Build();
}

TConstArrayView<FAssetDocumentPrimitiveDescriptor> GetDescriptors()
{
	static const TArray<FAssetDocumentPrimitiveDescriptor> Descriptors = BuildDescriptors();
	return Descriptors;
}

TConstArrayView<FBinding> GetBindings()
{
	return Bindings;
}
} // namespace UE::AssetDocument::Graph::Sequencer

TSharedRef<IAssetDocumentPrimitiveProvider> CreateAssetDocumentSequencerPrimitiveProvider()
{
	namespace SequencerTable = UE::AssetDocument::Graph::Sequencer;
	return CreateAssetDocumentAssetTablePrimitiveProvider(SequencerTable::ProviderId, &SequencerTable::GetDescriptors, &SequencerTable::GetBindings);
}
