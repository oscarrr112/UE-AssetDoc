// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// rig.ikretargeter.read.v1: the authored state of an IK Retargeter (design §2.2, §3.3) as document-ready records.
// The retargeter is read as stored after load (PostLoad already converted older formats into the op stack);
// nothing is initialized, re-sorted or written. Op settings are sparse over what a new op of the same type would
// hold after "Add New" in the op stack (a default op after FIKRetargetOpBase::OnAddedToStack, run on a transient
// instance), so an op's auto-filled bones are not listed when they equal what the editor fills in.
//
// Args {offset?, detail?: "document" | "inspect" | "verify"}. Records (paged under the value budget):
//   {kind: "header", source: {ik_rig, preview_mesh, rig_mesh}, target: {ik_rig, preview_mesh, rig_mesh}, ops, profiles[], current_profile}
//   {kind: "op", index, entry: {Name, Type, Parent?, Enabled?, Settings?, TargetIKRig?, ChainMapping?}}
//   {kind: "op_chains", index, chains: {<target chain>: {sparse chain settings}}}     (chunked; merged into Chains)
//   {kind: "pose", side: "Source" | "Target", name, current, root_offset?: number | {X,Y,Z}}
//   {kind: "pose_bones", side, name, bones: {<bone>: {X,Y,Z,W}}}                      (chunked; merged into Bones)
// detail "inspect" adds {kind: "op_facts", index, display_name, singleton, can_have_children, parent_type?,
// mapping_ready?} per op and {kind: "chains", side, rig, chains[{Name, Start, End, Goal?}]} per IK Rig.
// detail "verify" adds to each op record settings_defaults (every Settings field of what "Add New" makes) and
// chain_defaults (every field of a new chain row) when the op has them: what an omitted field means.

#include "AssetDocumentRigControllerScope.h"
#include "AssetDocumentRigPrimitives.h"

#include "RetargetEditor/IKRetargeterController.h"
#include "Retargeter/IKRetargetChainMapping.h"
#include "Retargeter/IKRetargetOps.h"
#include "Retargeter/IKRetargetSettings.h"
#include "Retargeter/IKRetargeter.h"
#include "Rig/IKRigDefinition.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::Graph::Rig
{
namespace
{
constexpr int32 ChainsPerRecord = 16;
constexpr int32 PoseBonesPerRecord = 100;

// A soft object path stored in a (private) UPROPERTY of the retargeter, read by reflection.
FString StoredSoftPath(const UIKRetargeter& Retargeter, FName PropertyName)
{
	const FSoftObjectProperty* Property = CastField<FSoftObjectProperty>(UIKRetargeter::StaticClass()->FindPropertyByName(PropertyName));
	if (Property == nullptr)
	{
		return FString();
	}
	const FSoftObjectPtr& Value = Property->GetPropertyValue_InContainer(&Retargeter);
	return Value.IsNull() ? FString() : Value.ToSoftObjectPath().ToString();
}

FValue MakeSide(const UIKRetargeter& Retargeter, ERetargetSourceOrTarget Side)
{
	FObjectBuilder Object;
	Object.AddString(TEXT("ik_rig"), PathOf(Retargeter.GetIKRig(Side)));
	Object.AddString(TEXT("preview_mesh"), StoredSoftPath(Retargeter, Side == ERetargetSourceOrTarget::Source
		? UIKRetargeter::GetSourcePreviewMeshPropertyName() : UIKRetargeter::GetTargetPreviewMeshPropertyName()));
	// What picking the rig stores as the preview mesh (UIKRetargeterController::SetIKRig): the rig's preview mesh.
	const UIKRigDefinition* Rig = Retargeter.GetIKRig(Side);
	Object.AddString(TEXT("rig_mesh"), Rig != nullptr && !Rig->PreviewSkeletalMesh.IsNull() ? Rig->PreviewSkeletalMesh.ToSoftObjectPath().ToString() : FString());
	return Object.Build();
}

TArray<FString> ProfileNames(const UIKRetargeter& Retargeter, FString& OutCurrent)
{
	TArray<FString> Names;
	if (const FMapProperty* Map = CastField<FMapProperty>(UIKRetargeter::StaticClass()->FindPropertyByName(TEXT("Profiles"))))
	{
		FScriptMapHelper Helper(Map, Map->ContainerPtrToValuePtr<void>(&Retargeter));
		for (FScriptMapHelper::FIterator It(Helper); It; ++It)
		{
			if (const FNameProperty* Key = CastField<FNameProperty>(Map->KeyProp))
			{
				Names.Add(Key->GetPropertyValue(Helper.GetKeyPtr(It)).ToString());
			}
		}
	}
	if (const FNameProperty* Current = CastField<FNameProperty>(UIKRetargeter::StaticClass()->FindPropertyByName(TEXT("CurrentProfile"))))
	{
		const FName Name = Current->GetPropertyValue_InContainer(&Retargeter);
		OutCurrent = Name.IsNone() ? FString() : Name.ToString();
	}
	Names.Sort();
	return Names;
}

// The chain rows of one chain-row array as {<target chain>: sparse settings}, rows at default omitted.
bool EncodeChainRows(const FArrayProperty& Array, const void* Settings, FCall& Call, int32 OpIndex, TArray<TPair<FString, FValue>>& OutRows)
{
	const FStructProperty* Element = CastField<FStructProperty>(Array.Inner);
	const UScriptStruct* RowType = Element->Struct;
	const FProperty* NameProperty = RowType->FindPropertyByName(TEXT("TargetChainName"));
	const FStructInstance Defaults(RowType);
	FScriptArrayHelper Rows(&Array, Array.ContainerPtrToValuePtr<void>(Settings));
	for (int32 Row = 0; Row < Rows.Num(); ++Row)
	{
		const uint8* RowValue = Rows.GetRawPtr(Row);
		const FNameProperty* NameField = CastField<FNameProperty>(NameProperty);
		const FName Chain = NameField != nullptr ? NameField->GetPropertyValue_InContainer(RowValue) : NAME_None;
		FValue Sparse;
		FString Error;
		if (!EncodeSparse(*RowType, RowValue, Defaults.Get(), Sparse, Error, [NameProperty](const FProperty& Property) { return &Property == NameProperty; }))
		{
			return Fail(Call, TEXT("IKRetargeter.ReadFailed"), FString::Printf(TEXT("Op %d chain %s: %s"), OpIndex, *Chain.ToString(), *Error));
		}
		if (CountValueNodes(Sparse) > 1)
		{
			OutRows.Emplace(Chain.ToString(), MoveTemp(Sparse));
		}
	}
	return true;
}

// Every field of the default chain row of an op's chain-row arrays (the first one; ops have one).
bool EncodeChainRowDefaults(const UScriptStruct& SettingsType, FValue& OutDefaults, bool& bOutHasRows, FString& OutError)
{
	bOutHasRows = false;
	for (TFieldIterator<FProperty> It(&SettingsType); It; ++It)
	{
		if (IsChainRowArray(**It))
		{
			const UScriptStruct* RowType = CastField<FStructProperty>(CastField<FArrayProperty>(*It)->Inner)->Struct;
			const FProperty* NameProperty = RowType->FindPropertyByName(TEXT("TargetChainName"));
			const FStructInstance Defaults(RowType);
			bOutHasRows = true;
			return EncodeSparse(*RowType, Defaults.Get(), nullptr, OutDefaults, OutError,
				[NameProperty](const FProperty& Property) { return &Property == NameProperty; });
		}
	}
	return true;
}

bool AddOp(const UIKRetargeter& Retargeter, int32 Index, bool bInspect, bool bVerify, FCall& Call, TArray<FValue>& Records)
{
	const FInstancedStruct& OpStruct = Retargeter.GetRetargetOps()[Index];
	const UScriptStruct* Type = OpStruct.GetScriptStruct();
	FObjectBuilder Entry;
	FObjectBuilder Record;
	Record.AddString(TEXT("kind"), TEXT("op"));
	Record.AddInt(TEXT("index"), Index);
	if (!OpStruct.IsValid() || Type == nullptr)
	{
		Entry.AddString(TEXT("Name"), FString::Printf(TEXT("Unknown_%d"), Index));
		Entry.AddString(TEXT("Type"), FString());
		Record.Add(TEXT("entry"), Entry.Build());
		Records.Add(Record.Build());
		return true;
	}
	FIKRetargetOpBase& Op = const_cast<FInstancedStruct&>(OpStruct).GetMutable<FIKRetargetOpBase>();
	Entry.Add(TEXT("Name"), MakeName(Op.GetName()));
	Entry.AddString(TEXT("Type"), GetOpTypeName(*Type));
	if (!Op.GetParentOpName().IsNone())
	{
		Entry.Add(TEXT("Parent"), MakeName(Op.GetParentOpName()));
	}
	if (!Op.IsEnabled())
	{
		Entry.AddBool(TEXT("Enabled"), false);
	}

	// What "Add New" would make: a default op of this type after OnAddedToStack against this retargeter.
	FInstancedStruct DefaultStruct = MakeAddedOp(Retargeter, *Type, Op.GetParentOpName());
	FIKRetargetOpBase& DefaultOp = DefaultStruct.GetMutable<FIKRetargetOpBase>();

	TArray<TPair<FString, FValue>> ChainRows;
	if (const UScriptStruct* SettingsType = Op.GetSettingsType())
	{
		const FIKRetargetOpSettingsBase* Settings = Op.GetSettings();
		const FIKRetargetOpSettingsBase* Defaults = DefaultOp.GetSettings();
		FString Error;
		FValue Sparse;
		if (Settings != nullptr && !EncodeSparse(*SettingsType, Settings, Defaults, Sparse, Error, [](const FProperty& Property) { return IsOpOwnedField(Property); }))
		{
			return Fail(Call, TEXT("IKRetargeter.ReadFailed"), FString::Printf(TEXT("Op %s settings: %s"), *Op.GetName().ToString(), *Error));
		}
		if (Settings != nullptr && CountValueNodes(Sparse) > 1)
		{
			Entry.Add(TEXT("Settings"), MoveTemp(Sparse));
		}
		if (bVerify && Defaults != nullptr)
		{
			FValue AllDefaults;
			FValue RowDefaults;
			bool bHasRows = false;
			if (!EncodeSparse(*SettingsType, Defaults, nullptr, AllDefaults, Error, [](const FProperty& Property) { return IsOpOwnedField(Property); })
				|| !EncodeChainRowDefaults(*SettingsType, RowDefaults, bHasRows, Error))
			{
				return Fail(Call, TEXT("IKRetargeter.ReadFailed"), FString::Printf(TEXT("Op %s defaults: %s"), *Op.GetName().ToString(), *Error));
			}
			Record.Add(TEXT("settings_defaults"), MoveTemp(AllDefaults));
			if (bHasRows)
			{
				Record.Add(TEXT("chain_defaults"), MoveTemp(RowDefaults));
			}
		}
		for (TFieldIterator<FProperty> It(SettingsType); It && Settings != nullptr; ++It)
		{
			const FProperty& Property = **It;
			if (IsTargetRigField(Property) && !Property.Identical_InContainer(Settings, Defaults, 0, PPF_None))
			{
				const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(&Property);
				Entry.AddString(TEXT("TargetIKRig"), PathOf(Object->GetObjectPropertyValue_InContainer(Settings)));
			}
			else if (IsChainRowArray(Property) && !EncodeChainRows(*CastField<FArrayProperty>(&Property), Settings, Call, Index, ChainRows))
			{
				return false;
			}
		}
	}
	if (const FRetargetChainMapping* Mapping = static_cast<const FIKRetargetOpBase&>(Op).GetChainMapping())
	{
		FObjectBuilder Pairs;
		for (const FRetargetChainPair& Pair : Mapping->GetChainPairs())
		{
			Pairs.Add(Pair.TargetChainName.ToString(), Pair.SourceChainName.IsNone() ? MakeNull() : MakeString(Pair.SourceChainName.ToString()));
		}
		Entry.Add(TEXT("ChainMapping"), Pairs.Build());
	}
	Record.Add(TEXT("entry"), Entry.Build());
	Records.Add(Record.Build());

	for (int32 Start = 0; Start < ChainRows.Num(); Start += ChainsPerRecord)
	{
		FObjectBuilder Chunk;
		for (int32 Row = Start; Row < FMath::Min(Start + ChainsPerRecord, ChainRows.Num()); ++Row)
		{
			Chunk.Add(ChainRows[Row].Key, ChainRows[Row].Value);
		}
		FObjectBuilder ChainsRecord;
		ChainsRecord.AddString(TEXT("kind"), TEXT("op_chains"));
		ChainsRecord.AddInt(TEXT("index"), Index);
		ChainsRecord.Add(TEXT("chains"), Chunk.Build());
		Records.Add(ChainsRecord.Build());
	}

	if (bInspect)
	{
		FObjectBuilder Facts;
		Facts.AddString(TEXT("kind"), TEXT("op_facts"));
		Facts.AddInt(TEXT("index"), Index);
		Facts.Add(TEXT("display_name"), MakeName(Op.GetDefaultName()));
		Facts.AddBool(TEXT("singleton"), Op.IsSingleton());
		Facts.AddBool(TEXT("can_have_children"), Op.CanHaveChildOps());
		if (const UScriptStruct* ParentType = Op.GetParentOpType())
		{
			Facts.AddString(TEXT("parent_type"), GetOpTypeName(*ParentType));
		}
		if (const FRetargetChainMapping* Mapping = static_cast<const FIKRetargetOpBase&>(Op).GetChainMapping())
		{
			Facts.AddBool(TEXT("mapping_ready"), Mapping->IsReady());
		}
		Records.Add(Facts.Build());
	}
	return true;
}

FValue Quat(const FQuat& Value)
{
	FObjectBuilder Object;
	Object.Add(TEXT("X"), MakeNumber(Value.X));
	Object.Add(TEXT("Y"), MakeNumber(Value.Y));
	Object.Add(TEXT("Z"), MakeNumber(Value.Z));
	Object.Add(TEXT("W"), MakeNumber(Value.W));
	return Object.Build();
}

void AddPoses(const UIKRetargeter& Retargeter, ERetargetSourceOrTarget Side, TArray<FValue>& Records)
{
	const TScopedRigController<UIKRetargeter> ControllerScope(Retargeter);
	const UIKRetargeterController* Controller = UIKRetargeterController::GetController(&Retargeter);
	if (Controller == nullptr)
	{
		return;
	}
	const TMap<FName, FIKRetargetPose>& Poses = Controller->GetRetargetPoses(Side);
	const FName Current = Retargeter.GetCurrentRetargetPoseName(Side);
	const UIKRigDefinition* Rig = Retargeter.GetIKRig(Side);
	TArray<FName> Names;
	Poses.GetKeys(Names);
	// "Default Pose" first, then by name (the pose list of the editor's pose menu).
	Names.Sort([](const FName A, const FName B)
	{
		const bool bADefault = A == UIKRetargeter::GetDefaultPoseName();
		const bool bBDefault = B == UIKRetargeter::GetDefaultPoseName();
		return bADefault != bBDefault ? bADefault : A.LexicalLess(B);
	});
	for (const FName& Name : Names)
	{
		const FIKRetargetPose& Pose = Poses[Name];
		FObjectBuilder Record;
		Record.AddString(TEXT("kind"), TEXT("pose"));
		Record.AddString(TEXT("side"), RetargetSideName(Side));
		Record.Add(TEXT("name"), MakeName(Name));
		Record.AddBool(TEXT("current"), Name == Current);
		const FVector Root = Pose.GetRootTranslationDelta();
		if (!Root.IsZero())
		{
			if (Root.X == 0.0 && Root.Y == 0.0)
			{
				Record.Add(TEXT("root_offset"), MakeNumber(Root.Z));
			}
			else
			{
				FObjectBuilder Vector;
				Vector.Add(TEXT("X"), MakeNumber(Root.X));
				Vector.Add(TEXT("Y"), MakeNumber(Root.Y));
				Vector.Add(TEXT("Z"), MakeNumber(Root.Z));
				Record.Add(TEXT("root_offset"), Vector.Build());
			}
		}
		Records.Add(Record.Build());

		// Bones in skeleton order of the side's IK Rig, then the rest by name; identity rotations omitted.
		TArray<FName> Bones;
		const TMap<FName, FQuat>& Rotations = Pose.GetAllDeltaRotations();
		if (Rig != nullptr)
		{
			for (const FName& Bone : Rig->GetSkeleton().BoneNames)
			{
				if (Rotations.Contains(Bone))
				{
					Bones.Add(Bone);
				}
			}
		}
		TArray<FName> Rest;
		for (const TPair<FName, FQuat>& Pair : Rotations)
		{
			if (!Bones.Contains(Pair.Key))
			{
				Rest.Add(Pair.Key);
			}
		}
		Rest.Sort(FNameLexicalLess());
		Bones.Append(Rest);
		Bones.RemoveAll([&Rotations](const FName& Bone) { return Rotations[Bone].Equals(FQuat::Identity, 0.0); });
		for (int32 Start = 0; Start < Bones.Num(); Start += PoseBonesPerRecord)
		{
			FObjectBuilder Chunk;
			for (int32 Bone = Start; Bone < FMath::Min(Start + PoseBonesPerRecord, Bones.Num()); ++Bone)
			{
				Chunk.Add(Bones[Bone].ToString(), Quat(Rotations[Bones[Bone]]));
			}
			FObjectBuilder BonesRecord;
			BonesRecord.AddString(TEXT("kind"), TEXT("pose_bones"));
			BonesRecord.AddString(TEXT("side"), RetargetSideName(Side));
			BonesRecord.Add(TEXT("name"), MakeName(Name));
			BonesRecord.Add(TEXT("bones"), Chunk.Build());
			Records.Add(BonesRecord.Build());
		}
	}
}

void AddRigChains(const UIKRetargeter& Retargeter, ERetargetSourceOrTarget Side, TArray<FValue>& Records)
{
	const UIKRigDefinition* Rig = Retargeter.GetIKRig(Side);
	if (Rig == nullptr)
	{
		return;
	}
	TArray<FValue> Chains;
	for (const FBoneChain& Chain : Rig->GetRetargetChains())
	{
		FObjectBuilder Entry;
		Entry.Add(TEXT("Name"), MakeName(Chain.ChainName));
		Entry.Add(TEXT("Start"), MakeName(Chain.StartBone.BoneName));
		Entry.Add(TEXT("End"), MakeName(Chain.EndBone.BoneName));
		if (!Chain.IKGoalName.IsNone())
		{
			Entry.Add(TEXT("Goal"), MakeName(Chain.IKGoalName));
		}
		Chains.Add(Entry.Build());
	}
	FObjectBuilder Record;
	Record.AddString(TEXT("kind"), TEXT("chains"));
	Record.AddString(TEXT("side"), RetargetSideName(Side));
	Record.AddString(TEXT("rig"), PathOf(Rig));
	Record.Add(TEXT("chains"), MakeArray(MoveTemp(Chains)));
	Records.Add(Record.Build());
}
} // namespace

const TCHAR* RetargetSideName(ERetargetSourceOrTarget Side)
{
	return Side == ERetargetSourceOrTarget::Source ? TEXT("Source") : TEXT("Target");
}

const FIKRetargetOpBase* FindRetargetOp(const UIKRetargeter& Retargeter, FName Name)
{
	if (Name.IsNone())
	{
		return nullptr;
	}
	for (const FInstancedStruct& Op : Retargeter.GetRetargetOps())
	{
		const FIKRetargetOpBase* Base = Op.GetPtr<FIKRetargetOpBase>();
		if (Base != nullptr && Base->GetName() == Name)
		{
			return Base;
		}
	}
	return nullptr;
}

FInstancedStruct MakeAddedOp(const UIKRetargeter& Retargeter, const UScriptStruct& Type, FName ParentName)
{
	FInstancedStruct Added(&Type);
	Added.GetMutable<FIKRetargetOpBase>().OnAddedToStack(&Retargeter, FindRetargetOp(Retargeter, ParentName));
	return Added;
}

bool BuildIKRetargeterRecords(const UIKRetargeter& Retargeter, bool bInspect, FCall& Call, TArray<FValue>& Records, bool bVerify)
{
	{
		FObjectBuilder Header;
		Header.AddString(TEXT("kind"), TEXT("header"));
		Header.Add(TEXT("source"), MakeSide(Retargeter, ERetargetSourceOrTarget::Source));
		Header.Add(TEXT("target"), MakeSide(Retargeter, ERetargetSourceOrTarget::Target));
		Header.AddInt(TEXT("ops"), Retargeter.GetRetargetOps().Num());
		FString CurrentProfile;
		Header.Add(TEXT("profiles"), MakeStringArray(ProfileNames(Retargeter, CurrentProfile)));
		Header.AddString(TEXT("current_profile"), CurrentProfile);
		Records.Add(Header.Build());
	}
	for (int32 Index = 0; Index < Retargeter.GetRetargetOps().Num(); ++Index)
	{
		if (!AddOp(Retargeter, Index, bInspect, bVerify, Call, Records))
		{
			return false;
		}
	}
	AddPoses(Retargeter, ERetargetSourceOrTarget::Source, Records);
	AddPoses(Retargeter, ERetargetSourceOrTarget::Target, Records);
	if (bInspect)
	{
		AddRigChains(Retargeter, ERetargetSourceOrTarget::Source, Records);
		AddRigChains(Retargeter, ERetargetSourceOrTarget::Target, Records);
	}
	return true;
}

bool HandleIKRetargeterRead(FCall& Call, FObjectBuilder& Result)
{
	int64 Offset = 0;
	FString Detail = TEXT("document");
	if (!Call.Args.OnlyFields({TEXT("offset"), TEXT("detail")}) || !Call.Args.Int(TEXT("offset"), Offset, false)
		|| !Call.Args.String(TEXT("detail"), Detail, false))
	{
		return false;
	}
	if (Offset < 0 || (Detail != TEXT("document") && Detail != TEXT("inspect") && Detail != TEXT("verify")))
	{
		return Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("offset must be >= 0 and detail document, inspect or verify."), TEXT("detail"));
	}
	const UIKRetargeter* Retargeter = Cast<UIKRetargeter>(&Call.Asset);
	if (Retargeter == nullptr)
	{
		return Fail(Call, TEXT("IKRetargeter.UnsupportedAsset"), TEXT("rig.ikretargeter.read.v1 reads IK Retargeter assets."), TEXT("/HandleArguments/asset"));
	}
	TArray<FValue> Records;
	if (!BuildIKRetargeterRecords(*Retargeter, Detail == TEXT("inspect"), Call, Records, Detail == TEXT("verify")))
	{
		return false;
	}
	AddRecordPage(Records, Offset, Result);
	return true;
}
} // namespace UE::AssetDocument::Graph::Rig
