// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// animation_asset.skeleton.read.v1 / edit.v1: the Skeleton editor's authorable surface. Edits go
// through IEditableSkeleton (the object the Skeleton editor's Anim Slot Manager, skeleton tree,
// blend profile picker and retargeting menu call), USkeleton's exported editor API (virtual bones,
// curve metadata) and AssetDocumentSkeletonRawEdits (socket delete / socket details). Bones are read
// only; slot rename / delete is refused while montages or Animation Blueprint slot nodes use the
// slot, as SSkeletonSlotNames does.

#include "AnimAssets/AssetDocumentAnimAssetPrimitives.h"
#include "AnimAssets/AssetDocumentSkeletonRawEdits.h"
#include "AssetDocumentNameSuggestions.h"

#include "AnimGraphNode_Slot.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimCurveMetadata.h"
#include "Animation/AnimMontage.h"
#include "Animation/BlendProfile.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "Engine/SkeletalMeshSocket.h"
#include "IEditableSkeleton.h"
#include "ISkeletonEditorModule.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "AssetDocumentSkeleton"

namespace UE::AssetDocument::Graph::AnimAssets
{
namespace
{
const TCHAR* const RetargetingModeNames[] = {
	TEXT("Animation"), TEXT("Skeleton"), TEXT("AnimationScaled"), TEXT("AnimationRelative"), TEXT("OrientAndScale")};

const TCHAR* RetargetingModeName(EBoneTranslationRetargetingMode::Type Mode)
{
	const int32 Index = static_cast<int32>(Mode);
	return Index >= 0 && Index < UE_ARRAY_COUNT(RetargetingModeNames) ? RetargetingModeNames[Index] : TEXT("Animation");
}

FString BlendProfileModeName(EBlendProfileMode Mode)
{
	return StaticEnum<EBlendProfileMode>()->GetNameStringByValue(static_cast<int64>(Mode));
}

USkeleton* GetSkeleton(FCall& Call)
{
	USkeleton* Skeleton = Cast<USkeleton>(&Call.Asset);
	if (Skeleton == nullptr)
	{
		Fail(Call, TEXT("AnimationAsset.UnsupportedAsset"), TEXT("Skeleton primitives edit Skeleton assets."), TEXT("/HandleArguments/asset"));
	}
	return Skeleton;
}

TArray<FString> SortedNames(TArray<FName> Names)
{
	TArray<FString> Out;
	for (const FName Name : Names)
	{
		Out.Add(Name.ToString());
	}
	Out.Sort();
	return Out;
}

FString JoinNames(const TArray<FString>& Names)
{
	return Names.IsEmpty() ? FString(TEXT("(none)")) : FString::Join(Names, TEXT(", "));
}

TArray<FString> AllSlotNames(const USkeleton& Skeleton)
{
	TArray<FString> Out;
	for (const FAnimSlotGroup& Group : Skeleton.GetSlotGroups())
	{
		for (const FName Slot : Group.SlotNames)
		{
			Out.Add(FString::Printf(TEXT("%s (%s)"), *Slot.ToString(), *Group.GroupName.ToString()));
		}
	}
	return Out;
}

// SSkeletonSlotNames::GetMontagesAndNodesUsingSlot: montages whose "Skeleton" tag is this skeleton and
// Animation Blueprints whose "TargetSkeleton" tag is this skeleton, loaded and searched for the slot.
TArray<FString> FindSlotUsers(const USkeleton& Skeleton, FName SlotName)
{
	TArray<FString> Users;
	const FString SkeletonExportName = FAssetData(&Skeleton).GetExportTextName();
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	TArray<FAssetData> Montages;
	Registry.GetAssetsByClass(UAnimMontage::StaticClass()->GetClassPathName(), Montages, true);
	for (const FAssetData& Data : Montages)
	{
		if (Data.GetTagValueRef<FString>("Skeleton") != SkeletonExportName)
		{
			continue;
		}
		if (const UAnimMontage* Montage = Cast<UAnimMontage>(Data.GetAsset()))
		{
			if (Montage->SlotAnimTracks.ContainsByPredicate([SlotName](const FSlotAnimationTrack& Track) { return Track.SlotName == SlotName; }))
			{
				Users.Add(Montage->GetPathName());
			}
		}
	}

	TArray<FAssetData> Blueprints;
	Registry.GetAssetsByClass(UAnimBlueprint::StaticClass()->GetClassPathName(), Blueprints, true);
	for (const FAssetData& Data : Blueprints)
	{
		if (Data.GetTagValueRef<FString>("TargetSkeleton") != SkeletonExportName)
		{
			continue;
		}
		UAnimBlueprint* AnimBlueprint = Cast<UAnimBlueprint>(Data.GetAsset());
		if (AnimBlueprint == nullptr)
		{
			continue;
		}
		TArray<UEdGraph*> Graphs;
		AnimBlueprint->GetAllGraphs(Graphs);
		bool bUses = false;
		for (const UEdGraph* Graph : Graphs)
		{
			TArray<UAnimGraphNode_Slot*> SlotNodes;
			Graph->GetNodesOfClass(SlotNodes);
			bUses |= SlotNodes.ContainsByPredicate([SlotName](const UAnimGraphNode_Slot* Node) { return Node->Node.SlotName == SlotName; });
		}
		if (bUses)
		{
			Users.Add(AnimBlueprint->GetPathName());
		}
	}
	Users.Sort();
	return Users;
}

// Names as the Skeleton editor's text boxes accept them (FName(*Text)): one line, no outer spaces.
bool ReadName(FReader& Op, const TCHAR* Key, FName& Out)
{
	FString Text;
	if (!Op.String(Key, Text))
	{
		return false;
	}
	const bool bControl = Text.Contains(TEXT("\n")) || Text.Contains(TEXT("\r")) || Text.Contains(TEXT("\t"));
	if (Text.IsEmpty() || Text.TrimStartAndEnd() != Text || bControl || Text.Len() >= NAME_SIZE || Text.Equals(TEXT("None"), ESearchCase::IgnoreCase))
	{
		return Op.Fail(TEXT("Skeleton.InvalidName"), FString::Printf(
			TEXT("'%s' is not a valid name (one line, no leading or trailing spaces, not None)."), *Text), Key);
	}
	Out = FName(*Text);
	return true;
}

bool ReadBone(FReader& Op, const USkeleton& Skeleton, const TCHAR* Key, FName& Out, int32& OutIndex, bool bRawOnly)
{
	if (!ReadName(Op, Key, Out))
	{
		return false;
	}
	const FReferenceSkeleton& RefSkeleton = Skeleton.GetReferenceSkeleton();
	OutIndex = RefSkeleton.FindBoneIndex(Out);
	if (OutIndex == INDEX_NONE || (bRawOnly && OutIndex >= RefSkeleton.GetRawBoneNum()))
	{
		TArray<FString> BoneNames;
		for (int32 Index = 0; Index < (bRawOnly ? RefSkeleton.GetRawBoneNum() : RefSkeleton.GetNum()); ++Index)
		{
			BoneNames.Add(RefSkeleton.GetBoneName(Index).ToString());
		}
		return Op.Fail(TEXT("Skeleton.BoneNotFound"), FString::Printf(TEXT("Skeleton %s has no %sbone '%s'.%s"),
			*Skeleton.GetPathName(), bRawOnly ? TEXT("(non-virtual) ") : TEXT(""), *Out.ToString(),
			*UE::AssetDocument::DescribeNameCandidates(Out.ToString(), BoneNames, TEXT("Bones"))), Key);
	}
	return true;
}

bool ReadVector(FReader& Op, const TCHAR* Key, const TCHAR* const (&Fields)[3], double (&Out)[3])
{
	const FValue* Object = nullptr;
	if (!Op.Object(Key, Object))
	{
		return false;
	}
	FReader Child = Op.Child(Key, *Object);
	if (!Child.OnlyFields({Fields[0], Fields[1], Fields[2]}))
	{
		return false;
	}
	for (int32 Index = 0; Index < 3; ++Index)
	{
		if (!ReadFloat(Child, Fields[Index], Out[Index]))
		{
			return false;
		}
	}
	return true;
}

const TCHAR* const XYZ[3] = {TEXT("X"), TEXT("Y"), TEXT("Z")};
const TCHAR* const PYR[3] = {TEXT("Pitch"), TEXT("Yaw"), TEXT("Roll")};

FValue VectorValue(const FVector& Vector)
{
	FObjectBuilder Builder;
	Builder.Add(TEXT("X"), MakeNumber(Vector.X)).Add(TEXT("Y"), MakeNumber(Vector.Y)).Add(TEXT("Z"), MakeNumber(Vector.Z));
	return Builder.Build();
}

FValue RotatorValue(const FRotator& Rotator)
{
	FObjectBuilder Builder;
	Builder.Add(TEXT("Pitch"), MakeNumber(Rotator.Pitch)).Add(TEXT("Yaw"), MakeNumber(Rotator.Yaw)).Add(TEXT("Roll"), MakeNumber(Rotator.Roll));
	return Builder.Build();
}

USkeletalMeshSocket* FindSkeletonSocket(const USkeleton& Skeleton, FName Name)
{
	for (USkeletalMeshSocket* Socket : Skeleton.Sockets)
	{
		if (Socket != nullptr && Socket->SocketName == Name)
		{
			return Socket;
		}
	}
	return nullptr;
}

bool RequireSocket(FReader& Op, const USkeleton& Skeleton, FName Name, USkeletalMeshSocket*& Out)
{
	Out = FindSkeletonSocket(Skeleton, Name);
	if (Out == nullptr)
	{
		TArray<FString> Names;
		for (const USkeletalMeshSocket* Socket : Skeleton.Sockets)
		{
			if (Socket != nullptr)
			{
				Names.Add(Socket->SocketName.ToString());
			}
		}
		return Op.Fail(TEXT("Skeleton.SocketNotFound"), FString::Printf(TEXT("Skeleton %s has no socket '%s'. Sockets: %s."),
			*Skeleton.GetPathName(), *Name.ToString(), *JoinNames(Names)), TEXT("name"));
	}
	return true;
}

bool RequireVirtualBone(FReader& Op, const USkeleton& Skeleton, FName Name)
{
	if (Skeleton.GetVirtualBones().ContainsByPredicate([Name](const FVirtualBone& Bone) { return Bone.VirtualBoneName == Name; }))
	{
		return true;
	}
	TArray<FString> Names;
	for (const FVirtualBone& Bone : Skeleton.GetVirtualBones())
	{
		Names.Add(Bone.VirtualBoneName.ToString());
	}
	return Op.Fail(TEXT("Skeleton.VirtualBoneNotFound"), FString::Printf(TEXT("Skeleton %s has no virtual bone '%s'. Virtual bones: %s."),
		*Skeleton.GetPathName(), *Name.ToString(), *JoinNames(Names)), TEXT("name"));
}

bool CheckVirtualBoneName(FReader& Op, const USkeleton& Skeleton, FName Name, const TCHAR* Key)
{
	if (!Name.ToString().StartsWith(TEXT("VB "), ESearchCase::CaseSensitive) || Name.ToString().Len() <= 3)
	{
		return Op.Fail(TEXT("Skeleton.InvalidVirtualBone"), FString::Printf(
			TEXT("Virtual bone names start with \"VB \" (as the skeleton tree names them), e.g. \"VB %s\"."), *Name.ToString()), Key);
	}
	if (Skeleton.GetReferenceSkeleton().FindBoneIndex(Name) != INDEX_NONE)
	{
		return Op.Fail(TEXT("Skeleton.NameExists"), FString::Printf(TEXT("Skeleton %s already has a bone named '%s'."),
			*Skeleton.GetPathName(), *Name.ToString()), Key);
	}
	return true;
}

bool RequireBlendProfile(FReader& Op, USkeleton& Skeleton, FName Name, UBlendProfile*& Out)
{
	Out = Skeleton.GetBlendProfile(Name);
	if (Out == nullptr)
	{
		TArray<FString> Names;
		for (const UBlendProfile* Profile : Skeleton.BlendProfiles)
		{
			if (Profile != nullptr)
			{
				Names.Add(Profile->GetName());
			}
		}
		return Op.Fail(TEXT("Skeleton.BlendProfileNotFound"), FString::Printf(TEXT("Skeleton %s has no blend profile '%s'. Blend profiles: %s."),
			*Skeleton.GetPathName(), *Name.ToString(), *JoinNames(Names)), TEXT("name"));
	}
	return true;
}

bool RequireCurve(FReader& Op, const USkeleton& Skeleton, FName Name)
{
	if (Skeleton.GetCurveMetaData(Name) != nullptr)
	{
		return true;
	}
	TArray<FName> Names;
	Skeleton.GetCurveMetaDataNames(Names);
	return Op.Fail(TEXT("Skeleton.CurveNotFound"), FString::Printf(TEXT("Skeleton %s has no curve metadata '%s'. Curves: %s."),
		*Skeleton.GetPathName(), *Name.ToString(), *JoinNames(SortedNames(Names))), TEXT("name"));
}

bool RequireSlot(FReader& Op, const USkeleton& Skeleton, FName Name)
{
	if (Skeleton.ContainsSlotName(Name))
	{
		return true;
	}
	return Op.Fail(TEXT("Skeleton.SlotNotFound"), FString::Printf(TEXT("Skeleton %s has no slot '%s'. Slots: %s."),
		*Skeleton.GetPathName(), *Name.ToString(), *JoinNames(AllSlotNames(Skeleton))), TEXT("name"));
}

bool CheckSlotOrGroupNameFree(FReader& Op, const USkeleton& Skeleton, FName Name, const TCHAR* Key)
{
	// SSkeletonSlotNames keeps slot and group names unique across both.
	if (Skeleton.ContainsSlotName(Name) || Skeleton.FindAnimSlotGroup(Name) != nullptr)
	{
		return Op.Fail(TEXT("Skeleton.NameExists"), FString::Printf(TEXT("Skeleton %s already has a slot or slot group named '%s'."),
			*Skeleton.GetPathName(), *Name.ToString()), Key);
	}
	return true;
}

bool CheckSlotUnused(FReader& Op, const USkeleton& Skeleton, FName Slot, const TCHAR* Verb)
{
	const TArray<FString> Users = FindSlotUsers(Skeleton, Slot);
	if (Users.IsEmpty())
	{
		return true;
	}
	return Op.Fail(TEXT("Skeleton.SlotInUse"), FString::Printf(
		TEXT("Cannot %s slot '%s' of %s: it is used by %s. Change those assets to another slot first (the Anim Slot Manager refuses the same edit)."),
		Verb, *Slot.ToString(), *Skeleton.GetPathName(), *FString::Join(Users, TEXT(", "))), TEXT("name"));
}

bool ParseRetargetingMode(FReader& Op, const FString& Text, EBoneTranslationRetargetingMode::Type& Out)
{
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(RetargetingModeNames); ++Index)
	{
		if (Text == RetargetingModeNames[Index])
		{
			Out = static_cast<EBoneTranslationRetargetingMode::Type>(Index);
			return true;
		}
	}
	return Op.Fail(TEXT("Skeleton.InvalidRetargetingMode"), FString::Printf(
		TEXT("'%s' is not a translation retargeting mode (Animation, Skeleton, AnimationScaled, AnimationRelative, OrientAndScale)."), *Text), TEXT("mode"));
}

bool ParseBlendProfileMode(FReader& Op, const FString& Text, EBlendProfileMode& Out)
{
	const UEnum* Enum = StaticEnum<EBlendProfileMode>();
	const int64 Value = Enum->GetValueByNameString(Text);
	if (Value == INDEX_NONE || Text.Contains(TEXT("::")))
	{
		return Op.Fail(TEXT("Skeleton.InvalidBlendProfileMode"), FString::Printf(
			TEXT("'%s' is not a blend profile mode (TimeFactor, WeightFactor, BlendMask)."), *Text), TEXT("mode"));
	}
	Out = static_cast<EBlendProfileMode>(Value);
	return true;
}

// {op: blend_profile_set, name, mode?, bones?}: the blend profile picker's mode, then the skeleton
// tree's per-bone blend scale; bones not listed are set back to the default scale, which removes
// their entry (UBlendProfile keeps only non-default entries).
bool SetBlendProfile(FReader& Op, IEditableSkeleton& Editable, USkeleton& Skeleton)
{
	FName Name;
	UBlendProfile* Profile = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("mode"), TEXT("bones")}) || !ReadName(Op, TEXT("name"), Name)
		|| !RequireBlendProfile(Op, Skeleton, Name, Profile))
	{
		return false;
	}
	if (Op.Has(TEXT("mode")))
	{
		FString ModeText;
		EBlendProfileMode Mode = EBlendProfileMode::TimeFactor;
		if (!Op.String(TEXT("mode"), ModeText) || !ParseBlendProfileMode(Op, ModeText, Mode))
		{
			return false;
		}
		if (Profile->Mode != Mode)
		{
			Editable.SetBlendProfileMode(Name, Mode);
		}
	}
	if (!Op.Has(TEXT("bones")))
	{
		return true;
	}
	const FValue* Bones = nullptr;
	if (!Op.Object(TEXT("bones"), Bones))
	{
		return false;
	}
	FReader BonesReader = Op.Child(TEXT("bones"), *Bones);
	TMap<FName, float> Wanted;
	for (const FString& Key : Bones->ObjectKeys)
	{
		const FName Bone(*Key);
		double Scale = 0.0;
		if (Skeleton.GetReferenceSkeleton().FindBoneIndex(Bone) == INDEX_NONE)
		{
			TArray<FString> BoneNames;
			for (int32 Index = 0; Index < Skeleton.GetReferenceSkeleton().GetNum(); ++Index)
			{
				BoneNames.Add(Skeleton.GetReferenceSkeleton().GetBoneName(Index).ToString());
			}
			return BonesReader.Fail(TEXT("Skeleton.BoneNotFound"), FString::Printf(TEXT("Skeleton %s has no bone '%s'.%s"),
				*Skeleton.GetPathName(), *Key, *UE::AssetDocument::DescribeNameCandidates(Key, BoneNames, TEXT("Bones"))), Key);
		}
		if (!ReadFloat(BonesReader, *Key, Scale))
		{
			return false;
		}
		Wanted.Add(Bone, static_cast<float>(Scale));
	}
	TArray<FName> Stale;
	for (int32 Entry = 0; Entry < Profile->GetNumBlendEntries(); ++Entry)
	{
		const FName Bone = Profile->GetEntry(Entry).BoneReference.BoneName;
		if (!Wanted.Contains(Bone))
		{
			Stale.Add(Bone);
		}
	}
	const float DefaultScale = Profile->GetDefaultBlendScale();
	for (const FName Bone : Stale)
	{
		Editable.SetBlendProfileScale(Name, Bone, DefaultScale, false);
	}
	for (const TPair<FName, float>& Pair : Wanted)
	{
		if (Profile->GetBoneBlendScale(Pair.Key) != Pair.Value || Profile->GetEntryIndex(Pair.Key) == INDEX_NONE)
		{
			Editable.SetBlendProfileScale(Name, Pair.Key, Pair.Value, false);
		}
	}
	return true;
}

bool SetSocket(FReader& Op, IEditableSkeleton& Editable, USkeleton& Skeleton)
{
	FName Name;
	USkeletalMeshSocket* Socket = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("bone"), TEXT("location"), TEXT("rotation"), TEXT("scale"), TEXT("force_always_animated")})
		|| !ReadName(Op, TEXT("name"), Name) || !RequireSocket(Op, Skeleton, Name, Socket))
	{
		return false;
	}
	using namespace UE::AssetDocument::Animation;
	if (Op.Has(TEXT("bone")))
	{
		FName Bone;
		int32 BoneIndex = INDEX_NONE;
		if (!ReadBone(Op, Skeleton, TEXT("bone"), Bone, BoneIndex, false))
		{
			return false;
		}
		if (Socket->BoneName != Bone)
		{
			// The skeleton tree's drag-and-drop re-parent keeps the relative transform.
			Editable.SetSocketParent(Name, Bone, nullptr, false);
		}
	}
	double Values[3] = {0.0, 0.0, 0.0};
	if (Op.Has(TEXT("location")))
	{
		if (!ReadVector(Op, TEXT("location"), XYZ, Values))
		{
			return false;
		}
		const FVector Location(Values[0], Values[1], Values[2]);
		if (!Socket->RelativeLocation.Equals(Location, 0.0))
		{
			SkeletonEdits::SetSocketField(*Socket, TEXT("RelativeLocation"), [&Location](USkeletalMeshSocket& Target) { Target.RelativeLocation = Location; });
		}
	}
	if (Op.Has(TEXT("rotation")))
	{
		if (!ReadVector(Op, TEXT("rotation"), PYR, Values))
		{
			return false;
		}
		const FRotator Rotation(Values[0], Values[1], Values[2]);
		if (!Socket->RelativeRotation.Equals(Rotation, 0.0))
		{
			SkeletonEdits::SetSocketField(*Socket, TEXT("RelativeRotation"), [&Rotation](USkeletalMeshSocket& Target) { Target.RelativeRotation = Rotation; });
		}
	}
	if (Op.Has(TEXT("scale")))
	{
		if (!ReadVector(Op, TEXT("scale"), XYZ, Values))
		{
			return false;
		}
		const FVector Scale(Values[0], Values[1], Values[2]);
		if (!Socket->RelativeScale.Equals(Scale, 0.0))
		{
			SkeletonEdits::SetSocketField(*Socket, TEXT("RelativeScale"), [&Scale](USkeletalMeshSocket& Target) { Target.RelativeScale = Scale; });
		}
	}
	if (Op.Has(TEXT("force_always_animated")))
	{
		bool bForce = true;
		if (!Op.Bool(TEXT("force_always_animated"), bForce, true))
		{
			return false;
		}
		if (Socket->bForceAlwaysAnimated != bForce)
		{
			SkeletonEdits::SetSocketField(*Socket, TEXT("bForceAlwaysAnimated"), [bForce](USkeletalMeshSocket& Target) { Target.bForceAlwaysAnimated = bForce; });
		}
	}
	return true;
}

bool ApplySkeletonOp(FReader& Op, const FString& Kind, IEditableSkeleton& Editable, USkeleton& Skeleton)
{
	FName Name;
	FName To;
	if (Kind == TEXT("slot_group_add"))
	{
		if (!Op.OnlyFields({TEXT("op"), TEXT("name")}) || !ReadName(Op, TEXT("name"), Name) || !CheckSlotOrGroupNameFree(Op, Skeleton, Name, TEXT("name")))
		{
			return false;
		}
		Editable.AddSlotGroupName(Name);
		return true;
	}
	if (Kind == TEXT("slot_group_remove"))
	{
		if (!Op.OnlyFields({TEXT("op"), TEXT("name")}) || !ReadName(Op, TEXT("name"), Name))
		{
			return false;
		}
		const FAnimSlotGroup* Group = Skeleton.FindAnimSlotGroup(Name);
		if (Group == nullptr)
		{
			TArray<FString> Groups;
			for (const FAnimSlotGroup& Each : Skeleton.GetSlotGroups())
			{
				Groups.Add(Each.GroupName.ToString());
			}
			return Op.Fail(TEXT("Skeleton.SlotGroupNotFound"), FString::Printf(TEXT("Skeleton %s has no slot group '%s'. Groups: %s."),
				*Skeleton.GetPathName(), *Name.ToString(), *JoinNames(Groups)), TEXT("name"));
		}
		if (Name == FAnimSlotGroup::DefaultGroupName)
		{
			return Op.Fail(TEXT("Skeleton.DefaultGroupRequired"), TEXT("DefaultGroup cannot be deleted (the Anim Slot Manager does not offer it)."), TEXT("name"));
		}
		for (const FName Slot : TArray<FName>(Group->SlotNames))
		{
			if (!CheckSlotUnused(Op, Skeleton, Slot, TEXT("delete the group of")))
			{
				return false;
			}
		}
		Editable.DeleteSlotGroup(Name);
		return true;
	}
	if (Kind == TEXT("slot_set"))
	{
		FName Group;
		if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("group")}) || !ReadName(Op, TEXT("name"), Name) || !ReadName(Op, TEXT("group"), Group))
		{
			return false;
		}
		if (Skeleton.FindAnimSlotGroup(Group) == nullptr)
		{
			return Op.Fail(TEXT("Skeleton.SlotGroupNotFound"), FString::Printf(TEXT("Skeleton %s has no slot group '%s'; add the group first."),
				*Skeleton.GetPathName(), *Group.ToString()), TEXT("group"));
		}
		if (!Skeleton.ContainsSlotName(Name) && !CheckSlotOrGroupNameFree(Op, Skeleton, Name, TEXT("name")))
		{
			return false;
		}
		if (!Skeleton.ContainsSlotName(Name) || Skeleton.GetSlotGroupName(Name) != Group)
		{
			Editable.SetSlotGroupName(Name, Group);
		}
		return true;
	}
	if (Kind == TEXT("slot_remove"))
	{
		if (!Op.OnlyFields({TEXT("op"), TEXT("name")}) || !ReadName(Op, TEXT("name"), Name) || !RequireSlot(Op, Skeleton, Name)
			|| !CheckSlotUnused(Op, Skeleton, Name, TEXT("delete")))
		{
			return false;
		}
		Editable.DeleteSlotName(Name);
		return true;
	}
	if (Kind == TEXT("slot_rename"))
	{
		if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("to")}) || !ReadName(Op, TEXT("name"), Name) || !ReadName(Op, TEXT("to"), To)
			|| !RequireSlot(Op, Skeleton, Name) || !CheckSlotOrGroupNameFree(Op, Skeleton, To, TEXT("to")) || !CheckSlotUnused(Op, Skeleton, Name, TEXT("rename")))
		{
			return false;
		}
		Editable.RenameSlotName(Name, To);
		return true;
	}
	if (Kind == TEXT("socket_add"))
	{
		FName Bone;
		int32 BoneIndex = INDEX_NONE;
		if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("bone")}) || !ReadName(Op, TEXT("name"), Name)
			|| !ReadBone(Op, Skeleton, TEXT("bone"), Bone, BoneIndex, false))
		{
			return false;
		}
		if (FindSkeletonSocket(Skeleton, Name) != nullptr)
		{
			return Op.Fail(TEXT("Skeleton.NameExists"), FString::Printf(TEXT("Skeleton %s already has a socket named '%s'."),
				*Skeleton.GetPathName(), *Name.ToString()), TEXT("name"));
		}
		// Skeleton tree "Add Socket" (named <Bone>Socket), then the inline rename the tree opens.
		USkeletalMeshSocket* Socket = Editable.AddSocket(Bone);
		if (Socket == nullptr)
		{
			return Op.Fail(TEXT("AD.RUNTIME.PRIMITIVE_FAILED"), TEXT("The skeleton editor did not add the socket."));
		}
		if (Socket->SocketName != Name)
		{
			Editable.RenameSocket(Socket->SocketName, Name, nullptr);
		}
		return true;
	}
	if (Kind == TEXT("socket_remove"))
	{
		USkeletalMeshSocket* Socket = nullptr;
		if (!Op.OnlyFields({TEXT("op"), TEXT("name")}) || !ReadName(Op, TEXT("name"), Name) || !RequireSocket(Op, Skeleton, Name, Socket))
		{
			return false;
		}
		UE::AssetDocument::Animation::SkeletonEdits::RemoveSocket(Skeleton, *Socket);
		return true;
	}
	if (Kind == TEXT("socket_rename"))
	{
		USkeletalMeshSocket* Socket = nullptr;
		if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("to")}) || !ReadName(Op, TEXT("name"), Name) || !ReadName(Op, TEXT("to"), To)
			|| !RequireSocket(Op, Skeleton, Name, Socket))
		{
			return false;
		}
		if (FindSkeletonSocket(Skeleton, To) != nullptr)
		{
			return Op.Fail(TEXT("Skeleton.NameExists"), FString::Printf(TEXT("Skeleton %s already has a socket named '%s'."),
				*Skeleton.GetPathName(), *To.ToString()), TEXT("to"));
		}
		Editable.RenameSocket(Name, To, nullptr);
		return true;
	}
	if (Kind == TEXT("socket_set"))
	{
		return SetSocket(Op, Editable, Skeleton);
	}
	if (Kind == TEXT("virtual_bone_add"))
	{
		FName Source;
		FName Target;
		int32 SourceIndex = INDEX_NONE;
		int32 TargetIndex = INDEX_NONE;
		if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("source"), TEXT("target")}) || !ReadName(Op, TEXT("name"), Name)
			|| !ReadBone(Op, Skeleton, TEXT("source"), Source, SourceIndex, false) || !ReadBone(Op, Skeleton, TEXT("target"), Target, TargetIndex, false)
			|| !CheckVirtualBoneName(Op, Skeleton, Name, TEXT("name")))
		{
			return false;
		}
		// FEditableSkeleton::HandleAddVirtualBone with the name the tree's rename box then commits.
		if (!Skeleton.AddNewNamedVirtualBone(Source, Target, Name))
		{
			return Op.Fail(TEXT("Skeleton.InvalidVirtualBone"), FString::Printf(
				TEXT("Skeleton %s refused virtual bone '%s' from %s to %s (a virtual bone between those bones already exists)."),
				*Skeleton.GetPathName(), *Name.ToString(), *Source.ToString(), *Target.ToString()), TEXT("name"));
		}
		return true;
	}
	if (Kind == TEXT("virtual_bone_remove"))
	{
		if (!Op.OnlyFields({TEXT("op"), TEXT("name")}) || !ReadName(Op, TEXT("name"), Name) || !RequireVirtualBone(Op, Skeleton, Name))
		{
			return false;
		}
		Skeleton.RemoveVirtualBones({Name});
		return true;
	}
	if (Kind == TEXT("virtual_bone_rename"))
	{
		if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("to")}) || !ReadName(Op, TEXT("name"), Name) || !ReadName(Op, TEXT("to"), To)
			|| !RequireVirtualBone(Op, Skeleton, Name) || !CheckVirtualBoneName(Op, Skeleton, To, TEXT("to")))
		{
			return false;
		}
		Editable.RenameVirtualBone(Name, To);
		return true;
	}
	if (Kind == TEXT("blend_profile_add"))
	{
		if (!Op.OnlyFields({TEXT("op"), TEXT("name")}) || !ReadName(Op, TEXT("name"), Name))
		{
			return false;
		}
		if (Skeleton.GetBlendProfile(Name) != nullptr || StaticFindObjectFast(nullptr, &Skeleton, Name) != nullptr)
		{
			return Op.Fail(TEXT("Skeleton.NameExists"), FString::Printf(TEXT("Skeleton %s already has a blend profile (or sub-object) named '%s'."),
				*Skeleton.GetPathName(), *Name.ToString()), TEXT("name"));
		}
		Editable.CreateNewBlendProfile(Name);
		return true;
	}
	if (Kind == TEXT("blend_profile_remove"))
	{
		UBlendProfile* Profile = nullptr;
		if (!Op.OnlyFields({TEXT("op"), TEXT("name")}) || !ReadName(Op, TEXT("name"), Name) || !RequireBlendProfile(Op, Skeleton, Name, Profile))
		{
			return false;
		}
		Editable.RemoveBlendProfile(Profile);
		return true;
	}
	if (Kind == TEXT("blend_profile_rename"))
	{
		UBlendProfile* Profile = nullptr;
		if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("to")}) || !ReadName(Op, TEXT("name"), Name) || !ReadName(Op, TEXT("to"), To)
			|| !RequireBlendProfile(Op, Skeleton, Name, Profile))
		{
			return false;
		}
		if (Skeleton.GetBlendProfile(To) != nullptr || StaticFindObjectFast(nullptr, &Skeleton, To) != nullptr)
		{
			return Op.Fail(TEXT("Skeleton.NameExists"), FString::Printf(TEXT("Skeleton %s already has a blend profile (or sub-object) named '%s'."),
				*Skeleton.GetPathName(), *To.ToString()), TEXT("to"));
		}
		if (Editable.RenameBlendProfile(Name, To) == nullptr)
		{
			return Op.Fail(TEXT("AD.RUNTIME.PRIMITIVE_FAILED"), FString::Printf(TEXT("Blend profile '%s' could not be renamed."), *Name.ToString()), TEXT("to"));
		}
		return true;
	}
	if (Kind == TEXT("blend_profile_set"))
	{
		return SetBlendProfile(Op, Editable, Skeleton);
	}
	if (Kind == TEXT("retarget"))
	{
		FName Bone;
		int32 BoneIndex = INDEX_NONE;
		FString ModeText;
		EBoneTranslationRetargetingMode::Type Mode = EBoneTranslationRetargetingMode::Animation;
		if (!Op.OnlyFields({TEXT("op"), TEXT("bone"), TEXT("mode")}) || !ReadBone(Op, Skeleton, TEXT("bone"), Bone, BoneIndex, true)
			|| !Op.String(TEXT("mode"), ModeText) || !ParseRetargetingMode(Op, ModeText, Mode))
		{
			return false;
		}
		if (Skeleton.GetBoneTranslationRetargetingMode(BoneIndex) != Mode)
		{
			Editable.SetBoneTranslationRetargetingMode(Bone, Mode);
		}
		return true;
	}
	if (Kind == TEXT("curve_add"))
	{
		if (!Op.OnlyFields({TEXT("op"), TEXT("name")}) || !ReadName(Op, TEXT("name"), Name))
		{
			return false;
		}
		if (Skeleton.GetCurveMetaData(Name) != nullptr)
		{
			return Op.Fail(TEXT("Skeleton.NameExists"), FString::Printf(TEXT("Skeleton %s already has curve metadata '%s'."),
				*Skeleton.GetPathName(), *Name.ToString()), TEXT("name"));
		}
		// SAnimCurveMetadataEditor "Add Curve".
		Skeleton.AddCurveMetaData(Name, true);
		return true;
	}
	if (Kind == TEXT("curve_remove"))
	{
		if (!Op.OnlyFields({TEXT("op"), TEXT("name")}) || !ReadName(Op, TEXT("name"), Name) || !RequireCurve(Op, Skeleton, Name))
		{
			return false;
		}
		Skeleton.RemoveCurveMetaData(Name);
		return true;
	}
	if (Kind == TEXT("curve_rename"))
	{
		if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("to")}) || !ReadName(Op, TEXT("name"), Name) || !ReadName(Op, TEXT("to"), To)
			|| !RequireCurve(Op, Skeleton, Name))
		{
			return false;
		}
		if (Skeleton.GetCurveMetaData(To) != nullptr)
		{
			return Op.Fail(TEXT("Skeleton.NameExists"), FString::Printf(TEXT("Skeleton %s already has curve metadata '%s'."),
				*Skeleton.GetPathName(), *To.ToString()), TEXT("to"));
		}
		Skeleton.RenameCurveMetaData(Name, To);
		return true;
	}
	if (Kind == TEXT("curve_set"))
	{
		if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("material"), TEXT("morph_target")}) || !ReadName(Op, TEXT("name"), Name)
			|| !RequireCurve(Op, Skeleton, Name))
		{
			return false;
		}
		bool bValue = false;
		if (Op.Has(TEXT("material")))
		{
			if (!Op.Bool(TEXT("material"), bValue, true))
			{
				return false;
			}
			if (Skeleton.GetCurveMetaDataMaterial(Name) != bValue)
			{
				Skeleton.SetCurveMetaDataMaterial(Name, bValue);
			}
		}
		if (Op.Has(TEXT("morph_target")))
		{
			if (!Op.Bool(TEXT("morph_target"), bValue, true))
			{
				return false;
			}
			if (Skeleton.GetCurveMetaDataMorphTarget(Name) != bValue)
			{
				Skeleton.SetCurveMetaDataMorphTarget(Name, bValue);
			}
		}
		return true;
	}
	return Op.Fail(TEXT("AnimationAsset.InvalidOp"), FString::Printf(TEXT("Unknown skeleton op '%s'."), *Kind), TEXT("op"));
}
} // namespace

bool HandleSkeletonRead(FCall& Call, FObjectBuilder& Result)
{
	USkeleton* Skeleton = GetSkeleton(Call);
	if (Skeleton == nullptr || !Call.Args.OnlyFields({}))
	{
		return false;
	}
	const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();
	const int32 RawBones = RefSkeleton.GetRawBoneNum();
	Result.AddInt(TEXT("bone_count"), RawBones);

	FObjectBuilder Retargeting;
	for (int32 Bone = 0; Bone < RawBones; ++Bone)
	{
		const EBoneTranslationRetargetingMode::Type Mode = Skeleton->GetBoneTranslationRetargetingMode(Bone);
		if (Mode != EBoneTranslationRetargetingMode::Animation)
		{
			Retargeting.AddString(RefSkeleton.GetBoneName(Bone).ToString(), RetargetingModeName(Mode));
		}
	}
	Result.Add(TEXT("retargeting"), Retargeting.Build());

	TArray<FValue> Groups;
	for (const FAnimSlotGroup& Group : Skeleton->GetSlotGroups())
	{
		TArray<FString> Slots;
		for (const FName Slot : Group.SlotNames)
		{
			Slots.Add(Slot.ToString());
		}
		FObjectBuilder Row;
		Row.AddString(TEXT("name"), Group.GroupName.ToString());
		Row.Add(TEXT("slots"), MakeStringArray(Slots));
		Groups.Add(Row.Build());
	}
	Result.Add(TEXT("slot_groups"), MakeArray(MoveTemp(Groups)));

	TArray<FValue> Sockets;
	for (const USkeletalMeshSocket* Socket : Skeleton->Sockets)
	{
		if (Socket == nullptr)
		{
			continue;
		}
		FObjectBuilder Row;
		Row.AddString(TEXT("name"), Socket->SocketName.ToString());
		Row.AddString(TEXT("bone"), Socket->BoneName.ToString());
		Row.Add(TEXT("location"), VectorValue(Socket->RelativeLocation));
		Row.Add(TEXT("rotation"), RotatorValue(Socket->RelativeRotation));
		Row.Add(TEXT("scale"), VectorValue(Socket->RelativeScale));
		Row.AddBool(TEXT("force_always_animated"), Socket->bForceAlwaysAnimated);
		Sockets.Add(Row.Build());
	}
	Result.Add(TEXT("sockets"), MakeArray(MoveTemp(Sockets)));

	TArray<FValue> VirtualBones;
	for (const FVirtualBone& Bone : Skeleton->GetVirtualBones())
	{
		FObjectBuilder Row;
		Row.AddString(TEXT("name"), Bone.VirtualBoneName.ToString());
		Row.AddString(TEXT("source"), Bone.SourceBoneName.ToString());
		Row.AddString(TEXT("target"), Bone.TargetBoneName.ToString());
		VirtualBones.Add(Row.Build());
	}
	Result.Add(TEXT("virtual_bones"), MakeArray(MoveTemp(VirtualBones)));

	TArray<FValue> Profiles;
	for (const UBlendProfile* Profile : Skeleton->BlendProfiles)
	{
		if (Profile == nullptr)
		{
			continue;
		}
		FObjectBuilder Bones;
		for (int32 Entry = 0; Entry < Profile->GetNumBlendEntries(); ++Entry)
		{
			const FBlendProfileBoneEntry& BoneEntry = Profile->GetEntry(Entry);
			Bones.Add(BoneEntry.BoneReference.BoneName.ToString(), FloatValue(BoneEntry.BlendScale));
		}
		FObjectBuilder Row;
		Row.AddString(TEXT("name"), Profile->GetName());
		Row.AddString(TEXT("mode"), BlendProfileModeName(Profile->Mode));
		Row.Add(TEXT("bones"), Bones.Build());
		Profiles.Add(Row.Build());
	}
	Result.Add(TEXT("blend_profiles"), MakeArray(MoveTemp(Profiles)));

	TArray<FName> CurveNames;
	Skeleton->GetCurveMetaDataNames(CurveNames);
	CurveNames.Sort(FNameLexicalLess());
	TArray<FValue> Curves;
	for (const FName Curve : CurveNames)
	{
		FObjectBuilder Row;
		Row.AddString(TEXT("name"), Curve.ToString());
		Row.AddBool(TEXT("material"), Skeleton->GetCurveMetaDataMaterial(Curve));
		Row.AddBool(TEXT("morph_target"), Skeleton->GetCurveMetaDataMorphTarget(Curve));
		Curves.Add(Row.Build());
	}
	Result.Add(TEXT("curves"), MakeArray(MoveTemp(Curves)));

	return true;
}

bool HandleSkeletonEdit(FCall& Call, FObjectBuilder& Result)
{
	USkeleton* Skeleton = GetSkeleton(Call);
	const FValue* Ops = nullptr;
	if (Skeleton == nullptr || !Call.Args.OnlyFields({TEXT("ops")}) || !Call.Args.Array(TEXT("ops"), Ops))
	{
		return false;
	}
	TSharedRef<IEditableSkeleton> Editable = FModuleManager::LoadModuleChecked<ISkeletonEditorModule>(TEXT("SkeletonEditor")).CreateEditableSkeleton(Skeleton);
	// One editor transaction for the call: the Skeleton editor's per-edit transactions nest into it.
	const FScopedTransaction CallTransaction(LOCTEXT("EditSkeleton", "Edit Skeleton"));
	for (int32 Index = 0; Index < Ops->Elements.Num(); ++Index)
	{
		FReader Op = Call.Args.Child(FString::Printf(TEXT("ops/%d"), Index), Ops->Elements[Index]);
		FString Kind;
		if (!Op.IsObject())
		{
			return Op.Fail(TEXT("AnimationAsset.InvalidOp"), TEXT("Each op is an object."));
		}
		if (!Op.String(TEXT("op"), Kind) || !ApplySkeletonOp(Op, Kind, *Editable, *Skeleton))
		{
			return false;
		}
	}
	if (!Ops->Elements.IsEmpty())
	{
		Skeleton->MarkPackageDirty();
	}
	Result.AddInt(TEXT("applied"), Ops->Elements.Num());
	return true;
}
} // namespace UE::AssetDocument::Graph::AnimAssets

#undef LOCTEXT_NAMESPACE
