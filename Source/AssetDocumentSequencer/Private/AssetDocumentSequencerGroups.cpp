// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Groups and cross-sequence references of the Sequencer document (design §3.2, §3.3):
//   * section groups (UMovieScene::GroupSections, Sequencer's "Group" on selected sections): the section header field
//     "Group" (a number shared by the sections of one group), written by seq.edit.v1 section_groups.set;
//   * node groups (the Sequencer's Group Manager filter, UMovieSceneNodeGroup): Body.Groups [{Name, Members}] whose
//     members name outliner nodes as the document addresses them ({Folder}, {Binding}, {Track, Node?}, {Raw}), stored
//     as the Sequencer's dot-joined outliner paths (folder names, binding GUIDs, track object names, category / channel
//     names; IOutlinerExtension::GetPathName), written by seq.edit.v1 groups.set;
//   * binding references into a sub / shot sequence (a camera cut to a camera of a shot): {"Binding": <path inside
//     that sequence>, "Sequence": <document path of the sub / shot section> | [outer, ..., inner section paths]}, the
//     sequence ID the binding picker composes (FSequenceBindingTree::FSequenceIDStack).

#include "AssetDocumentSequencerEdit.h"

#include "MovieScene.h"
#include "MovieSceneFolder.h"
#include "MovieSceneObjectBindingID.h"
#include "MovieSceneSequence.h"
#include "MovieSceneSequenceID.h"
#include "MovieSceneTrack.h"
#include "Sections/MovieSceneSubSection.h"

namespace UE::AssetDocument::Graph::Sequencer
{
namespace
{
constexpr int32 MaxSubsequenceDepth = 4;

FString FolderNodePath(const FString& FolderPath)
{
	return FolderPath.Replace(TEXT("/"), TEXT("."));
}

// The parent of a binding in the document tree (invalid for a root binding).
FGuid TreeParent(const FSequenceModel& Model, const FGuid& Guid)
{
	const FBindingInfo* Info = Model.FindBinding(Guid);
	const FBindingInfo* Parent = Info != nullptr ? Model.FindBinding(Info->Parent) : nullptr;
	return Parent != nullptr && Parent->Children.Contains(Guid) ? Info->Parent : FGuid();
}

// The outliner path of a binding: its root's folder, then the GUIDs from the root down.
FString BindingNodePath(const FSequenceModel& Model, const FGuid& Guid)
{
	TArray<FGuid> Chain;
	for (FGuid Current = Guid; Current.IsValid() && Chain.Num() < 64; Current = TreeParent(Model, Current))
	{
		Chain.Insert(Current, 0);
	}
	TArray<FString> Parts;
	if (const FString* Folder = Chain.IsEmpty() ? nullptr : Model.BindingFolders.Find(Chain[0]))
	{
		Parts.Add(FolderNodePath(*Folder));
	}
	for (const FGuid& Item : Chain)
	{
		Parts.Add(Item.ToString());
	}
	return FString::Join(Parts, TEXT("."));
}

FString TrackNodePath(const FSequenceModel& Model, const FTrackInfo& Track)
{
	if (Track.Binding.IsValid())
	{
		return BindingNodePath(Model, Track.Binding) + TEXT(".") + Track.Track->GetName();
	}
	const FString* Folder = Model.TrackFolders.Find(Track.Track);
	return Folder != nullptr ? FolderNodePath(*Folder) + TEXT(".") + Track.Track->GetName() : Track.Track->GetName();
}

// The document member of an outliner path, or {Raw} when the path names nothing the document has.
FValue DescribeNode(const FSequenceModel& Model, const FString& NodePath)
{
	TArray<FString> Parts;
	NodePath.ParseIntoArray(Parts, TEXT("."));
	int32 Index = 0;
	// Folders from the root.
	FString Folder;
	while (Index < Parts.Num())
	{
		const FString Candidate = Folder.IsEmpty() ? Parts[Index] : Folder + TEXT("/") + Parts[Index];
		if (!Model.Folders.ContainsByPredicate([&Candidate](const FFolderInfo& Info) { return Info.Path == Candidate; }))
		{
			break;
		}
		Folder = Candidate;
		++Index;
	}
	FObjectBuilder Member;
	if (Index == Parts.Num() && !Folder.IsEmpty())
	{
		Member.AddString(TEXT("Folder"), Folder);
		return Member.Build();
	}
	// Bindings from a root binding.
	FGuid Binding;
	while (Index < Parts.Num())
	{
		FGuid Guid;
		const FBindingInfo* Info = FGuid::Parse(Parts[Index], Guid) ? Model.FindBinding(Guid) : nullptr;
		if (Info == nullptr || TreeParent(Model, Guid) != Binding)
		{
			break;
		}
		Binding = Guid;
		++Index;
	}
	auto Raw = [&NodePath]()
	{
		FObjectBuilder Out;
		Out.AddString(TEXT("Raw"), NodePath);
		return Out.Build();
	};
	if (Binding.IsValid() && BindingNodePath(Model, Binding) != FString::Join(TArrayView<const FString>(Parts.GetData(), Index), TEXT(".")))
	{
		return Raw(); // a binding listed under a folder it is not in
	}
	if (Index == Parts.Num())
	{
		if (!Binding.IsValid())
		{
			return Raw();
		}
		Member.AddString(TEXT("Binding"), Model.Bindings[Binding].Path);
		return Member.Build();
	}
	// A track of that binding (or a global track in that folder), then category / channel names.
	for (const FTrackInfo& Track : Model.Tracks)
	{
		if (Track.Binding == Binding && Track.Track->GetName() == Parts[Index])
		{
			const FString Prefix = FString::Join(TArrayView<const FString>(Parts.GetData(), Index + 1), TEXT("."));
			if (TrackNodePath(Model, Track) != Prefix)
			{
				return Raw();
			}
			Member.AddString(TEXT("Track"), Track.DocumentPath);
			if (Index + 1 < Parts.Num())
			{
				Member.AddString(TEXT("Node"), FString::Join(TArrayView<const FString>(Parts.GetData() + Index + 1, Parts.Num() - Index - 1), TEXT(".")));
			}
			return Member.Build();
		}
	}
	return Raw();
}

const FTrackInfo* FindTrackByPath(const FSequenceModel& Model, const FString& DocumentPath)
{
	return Model.Tracks.FindByPredicate([&DocumentPath](const FTrackInfo& Track) { return Track.DocumentPath == DocumentPath; });
}

// The sub / shot section at a document section path ("/Body/Tracks/1/Sections/0").
UMovieSceneSubSection* FindSubSection(const FSequenceModel& Model, const FString& SectionPath)
{
	const int32 At = SectionPath.Find(TEXT("/Sections/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (At == INDEX_NONE)
	{
		return nullptr;
	}
	const FTrackInfo* Track = FindTrackByPath(Model, SectionPath.Left(At));
	const FString IndexText = SectionPath.Mid(At + 10);
	if (Track == nullptr || IndexText.IsEmpty() || !IndexText.IsNumeric())
	{
		return nullptr;
	}
	const int32 Index = FCString::Atoi(*IndexText);
	const TArray<UMovieSceneSection*>& Sections = Track->Track->GetAllSections();
	return Sections.IsValidIndex(Index) ? Cast<UMovieSceneSubSection>(Sections[Index]) : nullptr;
}

// The sequence ID of a chain of sub sections, outer to inner (FSequenceBindingTree::FSequenceIDStack::GetCurrent).
FMovieSceneSequenceID ChainId(TConstArrayView<FMovieSceneSequenceID> Ids)
{
	FMovieSceneSequenceID Id = MovieSceneSequenceID::Root;
	for (int32 Index = Ids.Num() - 1; Index >= 0; --Index)
	{
		Id = Id.AccumulateParentID(Ids[Index]);
	}
	return Id;
}

bool FindRemote(const FSequenceModel& Model, const FMovieSceneSequenceID& Target, const FGuid& Guid, TArray<FMovieSceneSequenceID>& Ids,
	TArray<FString>& Chain, FString& OutBinding, TSet<const UMovieSceneSequence*>& Visiting)
{
	for (const FTrackInfo& Track : Model.Tracks)
	{
		for (const UMovieSceneSection* Section : Track.Track->GetAllSections())
		{
			const UMovieSceneSubSection* Sub = Cast<UMovieSceneSubSection>(Section);
			const UMovieSceneSequence* Inner = Sub != nullptr ? Sub->GetSequence() : nullptr;
			if (Inner == nullptr || Visiting.Contains(Inner))
			{
				continue;
			}
			FSequenceModel InnerModel;
			FString Error;
			if (!BuildModel(*Inner, InnerModel, Error))
			{
				continue;
			}
			Ids.Add(Sub->GetSequenceID());
			Chain.Add(Model.SectionPath(*Sub));
			if (ChainId(Ids) == Target)
			{
				if (const FBindingInfo* Binding = InnerModel.FindBinding(Guid))
				{
					OutBinding = Binding->Path;
					return true;
				}
			}
			if (Ids.Num() < MaxSubsequenceDepth)
			{
				Visiting.Add(Inner);
				const bool bFound = FindRemote(InnerModel, Target, Guid, Ids, Chain, OutBinding, Visiting);
				Visiting.Remove(Inner);
				if (bFound)
				{
					return true;
				}
			}
			Ids.Pop();
			Chain.Pop();
		}
	}
	return false;
}
} // namespace

bool DescribeRemoteBinding(const FSequenceModel& Model, const FMovieSceneSequenceID& SequenceId, const FGuid& Guid, FValue& Out)
{
	if (Model.Sequence == nullptr)
	{
		return false;
	}
	TArray<FMovieSceneSequenceID> Ids;
	TArray<FString> Chain;
	FString Binding;
	TSet<const UMovieSceneSequence*> Visiting = {Model.Sequence};
	if (!FindRemote(Model, SequenceId, Guid, Ids, Chain, Binding, Visiting))
	{
		return false;
	}
	FObjectBuilder Object;
	Object.AddString(TEXT("Binding"), Binding);
	Object.Add(TEXT("Sequence"), Chain.Num() == 1 ? MakeString(Chain[0]) : MakeStringArray(Chain));
	Out = Object.Build();
	return true;
}

bool DecodeRemoteBinding(const FValue& Value, FMovieSceneObjectBindingID& Out, FDecodeContext& Context)
{
	const FValue* Sequence = FindField(Value, TEXT("Sequence"));
	const FValue* Path = FindField(Value, TEXT("Binding"));
	const FString At = Context.Path.IsEmpty() ? FString(TEXT("Sequence")) : Context.Path + TEXT("/Sequence");
	TArray<FString> Chain;
	if (Sequence != nullptr && Sequence->Kind == EAssetDocumentRuntimeValueKind::String)
	{
		Chain.Add(Sequence->StringValue);
	}
	else if (Sequence != nullptr && Sequence->Kind == EAssetDocumentRuntimeValueKind::Array)
	{
		for (const FValue& Item : Sequence->Elements)
		{
			Chain.Add(Item.StringValue);
		}
	}
	if (Chain.IsEmpty() || Chain.Num() > MaxSubsequenceDepth || Path == nullptr || Path->Kind != EAssetDocumentRuntimeValueKind::String)
	{
		return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("A binding reference into a sub-sequence is {\"Binding\": \"<binding path in "
			"that sequence>\", \"Sequence\": \"<document path of the sub / shot section>\"} (a list of section paths, outer to inner, for a "
			"sequence nested deeper)."), At);
	}
	TArray<FMovieSceneSequenceID> Ids;
	FSequenceModel Current = Context.Model;
	for (int32 Index = 0; Index < Chain.Num(); ++Index)
	{
		const UMovieSceneSubSection* Sub = FindSubSection(Current, Chain[Index]);
		const UMovieSceneSequence* Inner = Sub != nullptr ? Sub->GetSequence() : nullptr;
		FSequenceModel InnerModel;
		FString Error;
		if (Inner == nullptr || !BuildModel(*Inner, InnerModel, Error))
		{
			return Context.Reader.Fail(TEXT("Sequencer.UnknownSequence"), FString::Printf(TEXT("'%s' is not a sub / shot section with a sequence%s "
				"(Sequence names the section by its document path, e.g. /Body/Tracks/1/Sections/0)."), *Chain[Index],
				Index > 0 ? TEXT(" in the sequence the previous section plays") : TEXT("")), Chain.Num() > 1 ? At + FString::Printf(TEXT("/%d"), Index) : At);
		}
		Ids.Add(Sub->GetSequenceID());
		Current = MoveTemp(InnerModel);
	}
	FGuid Guid;
	if (!ResolveBindingPath(Current, Path->StringValue, Guid))
	{
		return Context.Reader.Fail(TEXT("Sequencer.UnknownBinding"), FString::Printf(TEXT("%s has no binding '%s'."),
			Current.Sequence != nullptr ? *Current.Sequence->GetPathName() : TEXT("The sub-sequence"), *Path->StringValue),
			Context.Path.IsEmpty() ? FString(TEXT("Binding")) : Context.Path + TEXT("/Binding"));
	}
	Out = UE::MovieScene::FRelativeObjectBindingID(Guid, ChainId(Ids), 0);
	return true;
}

FValue ReadNodeGroups(const FSequenceModel& Model)
{
	TArray<FValue> Groups;
#if WITH_EDITORONLY_DATA
	UMovieScene* MovieScene = const_cast<UMovieScene*>(Model.MovieScene);
	if (MovieScene == nullptr)
	{
		return MakeArray(MoveTemp(Groups));
	}
	for (UMovieSceneNodeGroup* Group : MovieScene->GetNodeGroups())
	{
		if (Group == nullptr)
		{
			continue;
		}
		TArray<FValue> Members;
		for (const FString& Node : Group->GetNodes())
		{
			Members.Add(DescribeNode(Model, Node));
		}
		FObjectBuilder Entry;
		Entry.Add(TEXT("Name"), MakeName(Group->GetName()));
		Entry.Add(TEXT("Members"), MakeArray(MoveTemp(Members)));
		Groups.Add(Entry.Build());
	}
#endif
	return MakeArray(MoveTemp(Groups));
}
} // namespace UE::AssetDocument::Graph::Sequencer

namespace UE::AssetDocument::Graph::Sequencer::Edit
{
namespace
{
bool ComposeNodePath(FEdit& Edit, FReader& Reader, const FValue& Member, FString& Out)
{
	if (Member.Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return Reader.Fail(TEXT("Sequencer.InvalidBody"), TEXT("A group member is {Folder} | {Binding} | {Track, Node?} | {Raw}."));
	}
	if (!CheckFields(Reader, Member, {TEXT("Folder"), TEXT("Binding"), TEXT("Track"), TEXT("Node"), TEXT("Raw")}, TEXT("A group member")))
	{
		return false;
	}
	const FSequenceModel& Model = Edit.Model();
	const FValue* Folder = FindField(Member, TEXT("Folder"));
	const FValue* Binding = FindField(Member, TEXT("Binding"));
	const FValue* Track = FindField(Member, TEXT("Track"));
	const FValue* Node = FindField(Member, TEXT("Node"));
	const FValue* Raw = FindField(Member, TEXT("Raw"));
	const int32 Kinds = static_cast<int32>(Folder != nullptr) + static_cast<int32>(Binding != nullptr) + static_cast<int32>(Track != nullptr)
		+ static_cast<int32>(Raw != nullptr);
	if (Kinds != 1 || (Node != nullptr && Track == nullptr))
	{
		return Reader.Fail(TEXT("Sequencer.InvalidBody"), TEXT("A group member is one of {Folder}, {Binding}, {Track, Node?} or {Raw}."));
	}
	if (Raw != nullptr)
	{
		Out = Raw->StringValue;
		return !Out.IsEmpty() || Reader.Fail(TEXT("Sequencer.InvalidBody"), TEXT("Raw is an outliner path."), TEXT("Raw"));
	}
	if (Folder != nullptr)
	{
		const FString Path = Folder->StringValue;
		if (!Model.Folders.ContainsByPredicate([&Path](const FFolderInfo& Info) { return Info.Path == Path; }))
		{
			return Reader.Fail(TEXT("Sequencer.InvalidFolder"), FString::Printf(TEXT("Folder '%s' is not listed in Body.Folders."), *Path), TEXT("Folder"));
		}
		Out = FolderNodePath(Path);
		return true;
	}
	if (Binding != nullptr)
	{
		FGuid Guid;
		if (!ResolveBindingPath(Model, Binding->StringValue, Guid))
		{
			return Reader.Fail(TEXT("Sequencer.UnknownBinding"), FString::Printf(TEXT("The sequence has no binding '%s'."), *Binding->StringValue),
				TEXT("Binding"));
		}
		Out = BindingNodePath(Model, Guid);
		return true;
	}
	const FTrackInfo* Info = FindTrackByPath(Model, Track->StringValue);
	if (Info == nullptr)
	{
		return Reader.Fail(TEXT("Sequencer.UnknownTrack"), FString::Printf(TEXT("'%s' is not the document path of a track (/Body/Tracks/<n> or "
			"/Body/Bindings/<n>/Tracks/<n>)."), *Track->StringValue), TEXT("Track"));
	}
	Out = TrackNodePath(Model, *Info);
	if (Node != nullptr && !Node->StringValue.IsEmpty())
	{
		Out += TEXT(".") + Node->StringValue;
	}
	return true;
}

#if WITH_EDITORONLY_DATA
// A {Raw} member names nothing the document has. Track object names are not authored, so a track this sequence got
// (a Create, a new track) may carry the name a Raw path ends in; such a track is renamed so the path keeps naming
// nothing (a stale outliner path stays stale).
void KeepRawPathsUnresolved(FEdit& Edit, const FValue& Groups)
{
	TArray<FString> RawPaths;
	TSet<FString> RawParts;
	for (const FValue& Group : Groups.Elements)
	{
		const FValue* Members = Group.Kind == EAssetDocumentRuntimeValueKind::Object ? FindField(Group, TEXT("Members")) : nullptr;
		for (int32 Index = 0; Members != nullptr && Members->Kind == EAssetDocumentRuntimeValueKind::Array && Index < Members->Elements.Num(); ++Index)
		{
			const FValue& Member = Members->Elements[Index];
			const FValue* Raw = Member.Kind == EAssetDocumentRuntimeValueKind::Object ? FindField(Member, TEXT("Raw")) : nullptr;
			if (Raw != nullptr && Raw->Kind == EAssetDocumentRuntimeValueKind::String && !Raw->StringValue.IsEmpty())
			{
				RawPaths.Add(Raw->StringValue);
				TArray<FString> Parts;
				Raw->StringValue.ParseIntoArray(Parts, TEXT("."));
				RawParts.Append(Parts);
			}
		}
	}
	for (const FString& RawPath : RawPaths)
	{
		const FValue Described = DescribeNode(Edit.Model(), RawPath);
		const FValue* TrackPath = FindField(Described, TEXT("Track"));
		const FTrackInfo* Info = TrackPath != nullptr ? FindTrackByPath(Edit.Model(), TrackPath->StringValue) : nullptr;
		UMovieSceneTrack* Track = Info != nullptr && Info->Track != nullptr ? FindTrackByName(Edit.MovieScene, Info->Track->GetName()) : nullptr;
		if (Track == nullptr)
		{
			continue;
		}
		const FString Base = Track->GetClass()->GetName();
		FString Name;
		for (int32 Number = 0; Number < 100000; ++Number)
		{
			const FString Candidate = FString::Printf(TEXT("%s_%d"), *Base, Number);
			if (!RawParts.Contains(Candidate) && StaticFindObjectFast(nullptr, Track->GetOuter(), FName(*Candidate)) == nullptr)
			{
				Name = Candidate;
				break;
			}
		}
		if (!Name.IsEmpty())
		{
			Track->Rename(*Name, nullptr, REN_DontCreateRedirectors);
			Edit.Invalidate();
			Edit.bChanged = true;
		}
	}
}
#endif
} // namespace

bool OpNodeGroupsSet(FEdit& Edit, FReader& Op)
{
#if WITH_EDITORONLY_DATA
	const FValue* Groups = nullptr;
	if (!Op.Array(TEXT("groups"), Groups))
	{
		return false;
	}
	KeepRawPathsUnresolved(Edit, *Groups);
	struct FWanted
	{
		FName Name;
		TArray<FString> Nodes;
	};
	TArray<FWanted> Wanted;
	for (int32 Index = 0; Index < Groups->Elements.Num(); ++Index)
	{
		FReader Entry = Op.Child(FString::Printf(TEXT("groups/%d"), Index), Groups->Elements[Index]);
		const FValue& Value = Groups->Elements[Index];
		if (Value.Kind != EAssetDocumentRuntimeValueKind::Object)
		{
			return Entry.Fail(TEXT("Sequencer.InvalidBody"), TEXT("A group is {Name, Members}."));
		}
		if (!CheckFields(Entry, Value, {TEXT("Name"), TEXT("Members")}, TEXT("A group")))
		{
			return false;
		}
		FWanted& Group = Wanted.AddDefaulted_GetRef();
		const FValue* Name = FindField(Value, TEXT("Name"));
		if (Name == nullptr || Name->Kind != EAssetDocumentRuntimeValueKind::String || Name->StringValue.IsEmpty())
		{
			return Entry.Fail(TEXT("Sequencer.InvalidBody"), TEXT("A group has a Name."), TEXT("Name"));
		}
		Group.Name = FName(*Name->StringValue);
		const FValue* Members = FindField(Value, TEXT("Members"));
		if (Members != nullptr && Members->Kind != EAssetDocumentRuntimeValueKind::Array)
		{
			return Entry.Fail(TEXT("Sequencer.InvalidBody"), TEXT("Members is an array."), TEXT("Members"));
		}
		for (int32 Member = 0; Members != nullptr && Member < Members->Elements.Num(); ++Member)
		{
			FReader MemberReader = Entry.Child(FString::Printf(TEXT("Members/%d"), Member), Members->Elements[Member]);
			FString Path;
			if (!ComposeNodePath(Edit, MemberReader, Members->Elements[Member], Path))
			{
				return false;
			}
			Group.Nodes.Add(MoveTemp(Path));
		}
	}
	// Existing groups are edited in place by position (the Group Manager's rename and add / remove nodes), the rest
	// created (FSequencer::AddSelectedNodesToNewNodeGroup) or removed.
	UMovieSceneNodeGroupCollection& Collection = Edit.MovieScene.GetNodeGroups();
	Collection.Modify();
	TArray<UMovieSceneNodeGroup*> Existing;
	for (UMovieSceneNodeGroup* Group : Collection)
	{
		Existing.Add(Group);
	}
	for (int32 Index = Wanted.Num(); Index < Existing.Num(); ++Index)
	{
		if (Existing[Index] != nullptr)
		{
			Collection.RemoveNodeGroup(Existing[Index]);
		}
	}
	for (int32 Index = 0; Index < Wanted.Num(); ++Index)
	{
		UMovieSceneNodeGroup* Group = Existing.IsValidIndex(Index) ? Existing[Index] : nullptr;
		if (Group == nullptr)
		{
			Group = NewObject<UMovieSceneNodeGroup>(&Collection, NAME_None, RF_Transactional);
			Group->SetName(Wanted[Index].Name);
			for (const FString& Node : Wanted[Index].Nodes)
			{
				Group->AddNode(Node);
			}
			Collection.AddNodeGroup(Group);
			continue;
		}
		if (Group->GetName() != Wanted[Index].Name)
		{
			Group->SetName(Wanted[Index].Name);
		}
		const TArray<FString> Current(Group->GetNodes());
		if (Current != Wanted[Index].Nodes)
		{
			for (const FString& Node : Current)
			{
				Group->RemoveNode(Node);
			}
			for (const FString& Node : Wanted[Index].Nodes)
			{
				Group->AddNode(Node);
			}
		}
	}
	Edit.bChanged = true;
	return true;
#else
	return Op.Fail(TEXT("Sequencer.InvalidBody"), TEXT("Node groups are editor data."));
#endif
}

bool OpSectionGroupsSet(FEdit& Edit, FReader& Op)
{
	const FValue* Groups = nullptr;
	if (!Op.Array(TEXT("groups"), Groups))
	{
		return false;
	}
	TArray<TArray<UMovieSceneSection*>> Wanted;
	for (int32 Index = 0; Index < Groups->Elements.Num(); ++Index)
	{
		const FValue& Group = Groups->Elements[Index];
		if (Group.Kind != EAssetDocumentRuntimeValueKind::Array || Group.Elements.Num() < 2)
		{
			return Op.Fail(TEXT("Sequencer.InvalidBody"), TEXT("A section group has two or more sections."), FString::Printf(TEXT("groups/%d"), Index));
		}
		TArray<UMovieSceneSection*>& Sections = Wanted.AddDefaulted_GetRef();
		for (int32 Member = 0; Member < Group.Elements.Num(); ++Member)
		{
			FReader Row = Op.Child(FString::Printf(TEXT("groups/%d/%d"), Index, Member), Group.Elements[Member]);
			FString TrackName;
			FString SectionName;
			if (!Row.String(TEXT("track"), TrackName) || !Row.String(TEXT("section"), SectionName))
			{
				return false;
			}
			UMovieSceneTrack* Track = FindTrackByName(Edit.MovieScene, TrackName);
			UMovieSceneSection* const* Found = Track != nullptr ? Track->GetAllSections().FindByPredicate(
				[&SectionName](const UMovieSceneSection* Section) { return Section != nullptr && Section->GetName() == SectionName; }) : nullptr;
			if (Found == nullptr)
			{
				return Row.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("No section %s in track %s."), *SectionName, *TrackName));
			}
			Sections.Add(*Found);
		}
	}
	// Ungroup every grouped section, then group as listed (Sequencer's Ungroup / Group on the selected sections).
	FSequenceModel Model;
	FString Error;
	BuildModel(Edit.Sequence, Model, Error);
	for (const TPair<const UMovieSceneSection*, int32>& Entry : Model.SectionGroups)
	{
		Edit.MovieScene.UngroupSection(*Entry.Key);
	}
	for (const TArray<UMovieSceneSection*>& Sections : Wanted)
	{
		Edit.MovieScene.GroupSections(Sections);
	}
	Edit.bChanged = true;
	return true;
}
} // namespace UE::AssetDocument::Graph::Sequencer::Edit
