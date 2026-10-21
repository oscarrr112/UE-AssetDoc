// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// rig.ikrig.read.v1: the authored state of an IK Rig (design §2.1, §3.3) as document-ready records. The rig is read
// as stored after load (PostLoad already converted legacy UObject solvers into the struct stack); nothing is
// initialized, re-sorted or written.
//
// Args {offset?, detail?: "document" | "inspect" | "verify"}. Records (paged under the value budget):
//   {kind: "header", preview_mesh, excluded_bones[], retarget_root, bones, goals, solvers, chains}
//   {kind: "goal", name, entry: {Bone, <Details fields that differ from a new goal>, Offset?}}
//   {kind: "solver", index, entry: {Type, Enabled?, StartBone?, EndBone?, Settings?, Goals?}}
//   {kind: "solver_bones", index, bones: {<bone>: {sparse bone settings}}}   (chunked; Python merges into Bones)
//   {kind: "chain", name, entry: {Start, End, Goal?}}
// detail "inspect" adds {kind: "skeleton", offset, bones[{Name, Parent, Excluded?}]}, {kind: "chain_bones", name,
// valid, bones[]} and {kind: "solver_facts", index, display_name, warning?, connected_goals[]}.
// detail "verify" adds to each solver record settings_defaults, goal_defaults and bone_defaults (every field of a
// new solver's settings, goal settings and bone settings, when the solver has them): what an omitted field means.

#include "AssetDocumentRigPrimitives.h"

#include "Engine/SkeletalMesh.h"
#include "Rig/IKRigDefinition.h"
#include "Rig/IKRigSkeleton.h"
#include "Rig/Solvers/IKRigSolverBase.h"
#include "StructUtils/InstancedStruct.h"

namespace UE::AssetDocument::Graph::Rig
{
namespace
{
constexpr int32 BonesPerRecord = 40;
constexpr int32 SkeletonBonesPerRecord = 150;
constexpr double OffsetTolerance = 1.0e-4;

double Clean(double Value)
{
	return FMath::Abs(Value) < OffsetTolerance ? 0.0 : ShortestFloat(Value);
}

struct FGoalField
{
	const TCHAR* Property;
	const TCHAR* Key;
};

const FGoalField GoalFields[] = {
	{TEXT("PositionAlpha"), TEXT("PositionAlpha")},
	{TEXT("RotationAlpha"), TEXT("RotationAlpha")},
	{TEXT("PreviewMode"), TEXT("PreviewMode")},
	{TEXT("SizeMultiplier"), TEXT("SizeMultiplier")},
	{TEXT("ThicknessMultiplier"), TEXT("ThicknessMultiplier")},
	{TEXT("bExposePosition"), TEXT("ExposePosition")},
	{TEXT("bExposeRotation"), TEXT("ExposeRotation")},
};

bool AddGoal(const UIKRigEffectorGoal& Goal, FCall& Call, TArray<FValue>& Records)
{
	const UIKRigEffectorGoal* Defaults = GetDefault<UIKRigEffectorGoal>();
	FObjectBuilder Entry;
	Entry.Add(TEXT("Bone"), MakeName(Goal.BoneName));
	for (const FGoalField& Field : GoalFields)
	{
		const FProperty* Property = UIKRigEffectorGoal::StaticClass()->FindPropertyByName(Field.Property);
		if (Property == nullptr || Property->Identical_InContainer(&Goal, Defaults, 0, PPF_None))
		{
			continue;
		}
		FValue Value;
		FString Error;
		if (!EncodeField(*Property, &Goal, Value, Error))
		{
			return Fail(Call, TEXT("IKRig.ReadFailed"), Error);
		}
		Entry.Add(Field.Key, MoveTemp(Value));
	}
	bool bIdentity = true;
	FValue Offset = EncodeGoalOffset(Goal.CurrentTransform, Goal.InitialTransform, bIdentity);
	if (!bIdentity)
	{
		Entry.Add(TEXT("Offset"), MoveTemp(Offset));
	}
	FObjectBuilder Record;
	Record.AddString(TEXT("kind"), TEXT("goal"));
	Record.Add(TEXT("name"), MakeName(Goal.GoalName));
	Record.Add(TEXT("entry"), Entry.Build());
	Records.Add(Record.Build());
	return true;
}

// Names of a set in a stable order: Order first (goal array / skeleton order), then the rest by name.
TArray<FName> Ordered(const TSet<FName>& Names, TConstArrayView<FName> Order)
{
	TArray<FName> Result;
	for (const FName& Name : Order)
	{
		if (Names.Contains(Name))
		{
			Result.Add(Name);
		}
	}
	TArray<FName> Rest;
	for (const FName& Name : Names)
	{
		if (!Name.IsNone() && !Result.Contains(Name))
		{
			Rest.Add(Name);
		}
	}
	Rest.Sort(FNameLexicalLess());
	Result.Append(Rest);
	return Result;
}

bool AddSolver(const UIKRigDefinition& Rig, int32 Index, bool bInspect, bool bVerify, FCall& Call, TArray<FValue>& Records)
{
	const FInstancedStruct& SolverStruct = Rig.GetSolverStructs()[Index];
	FObjectBuilder Entry;
	FObjectBuilder Record;
	Record.AddString(TEXT("kind"), TEXT("solver"));
	Record.AddInt(TEXT("index"), Index);
	const UScriptStruct* Type = SolverStruct.GetScriptStruct();
	if (!SolverStruct.IsValid() || Type == nullptr)
	{
		// A solver whose type no longer exists (the processor warns "null/unknown solver").
		Entry.AddString(TEXT("Type"), FString());
		Record.Add(TEXT("entry"), Entry.Build());
		Records.Add(Record.Build());
		return true;
	}
	FIKRigSolverBase& Solver = const_cast<FInstancedStruct&>(SolverStruct).GetMutable<FIKRigSolverBase>();
	FInstancedStruct DefaultStruct(Type);
	FIKRigSolverBase& DefaultSolver = DefaultStruct.GetMutable<FIKRigSolverBase>();

	Entry.AddString(TEXT("Type"), GetSolverTypeName(*Type));
	if (!Solver.IsEnabled())
	{
		Entry.AddBool(TEXT("Enabled"), false);
	}
	if (Solver.UsesStartBone() && !Solver.GetStartBone().IsNone())
	{
		Entry.Add(TEXT("StartBone"), MakeName(Solver.GetStartBone()));
	}
	if (Solver.UsesEndBone() && !Solver.GetEndBone().IsNone())
	{
		Entry.Add(TEXT("EndBone"), MakeName(Solver.GetEndBone()));
	}
	FString Error;
	if (const UScriptStruct* SettingsType = Solver.GetSolverSettingsType())
	{
		const FIKRigSolverSettingsBase* Settings = Solver.GetSolverSettings();
		const FIKRigSolverSettingsBase* Defaults = DefaultSolver.GetSolverSettings();
		FValue Sparse;
		if (Settings != nullptr && !EncodeSparse(*SettingsType, Settings, Defaults, Sparse, Error))
		{
			return Fail(Call, TEXT("IKRig.ReadFailed"), FString::Printf(TEXT("Solver %d settings: %s"), Index, *Error));
		}
		if (Settings != nullptr && CountValueNodes(Sparse) > 1)
		{
			Entry.Add(TEXT("Settings"), MoveTemp(Sparse));
		}
		FValue AllDefaults;
		if (bVerify && Defaults != nullptr)
		{
			if (!EncodeSparse(*SettingsType, Defaults, nullptr, AllDefaults, Error))
			{
				return Fail(Call, TEXT("IKRig.ReadFailed"), FString::Printf(TEXT("Solver %d defaults: %s"), Index, *Error));
			}
			Record.Add(TEXT("settings_defaults"), MoveTemp(AllDefaults));
		}
	}
	if (bVerify)
	{
		// The defaults of the per-goal and per-bone settings (what an omitted field means).
		const UScriptStruct* GoalType = Solver.UsesCustomGoalSettings() ? Solver.GetGoalSettingsType() : nullptr;
		const UScriptStruct* BoneType = Solver.UsesCustomBoneSettings() ? Solver.GetBoneSettingsType() : nullptr;
		const TPair<const TCHAR*, const UScriptStruct*> Kinds[] = {{TEXT("goal_defaults"), GoalType}, {TEXT("bone_defaults"), BoneType}};
		for (const TPair<const TCHAR*, const UScriptStruct*>& Kind : Kinds)
		{
			if (Kind.Value == nullptr)
			{
				continue;
			}
			const FStructInstance KindDefaults(Kind.Value);
			FValue All;
			if (!EncodeSparse(*Kind.Value, KindDefaults.Get(), nullptr, All, Error))
			{
				return Fail(Call, TEXT("IKRig.ReadFailed"), FString::Printf(TEXT("Solver %d %s: %s"), Index, Kind.Key, *Error));
			}
			Record.Add(Kind.Key, MoveTemp(All));
		}
	}

	TArray<FName> GoalOrder;
	for (const UIKRigEffectorGoal* Goal : Rig.GetGoalArray())
	{
		if (Goal != nullptr)
		{
			GoalOrder.Add(Goal->GoalName);
		}
	}
	TSet<FName> Connected;
	Solver.GetRequiredGoals(Connected);
	const TArray<FName> Goals = Ordered(Connected, GoalOrder);
	if (!Goals.IsEmpty())
	{
		FObjectBuilder GoalSettings;
		const UScriptStruct* GoalType = Solver.UsesCustomGoalSettings() ? Solver.GetGoalSettingsType() : nullptr;
		const FStructInstance GoalDefaults(GoalType);
		for (const FName& Goal : Goals)
		{
			FValue Sparse = MakeEmptyObject();
			if (GoalType != nullptr)
			{
				if (const FIKRigGoalSettingsBase* Settings = Solver.GetGoalSettings(Goal))
				{
					if (!EncodeSparse(*GoalType, Settings, GoalDefaults.Get(), Sparse, Error))
					{
						return Fail(Call, TEXT("IKRig.ReadFailed"), FString::Printf(TEXT("Solver %d goal %s: %s"), Index, *Goal.ToString(), *Error));
					}
				}
			}
			GoalSettings.Add(Goal.ToString(), MoveTemp(Sparse));
		}
		Entry.Add(TEXT("Goals"), GoalSettings.Build());
	}
	Record.Add(TEXT("entry"), Entry.Build());
	Records.Add(Record.Build());

	if (Solver.UsesCustomBoneSettings())
	{
		const UScriptStruct* BoneType = Solver.GetBoneSettingsType();
		const FStructInstance BoneDefaults(BoneType);
		TSet<FName> WithSettings;
		Solver.GetBonesWithSettings(WithSettings);
		const TArray<FName> Bones = Ordered(WithSettings, Rig.GetSkeleton().BoneNames);
		for (int32 Start = 0; Start < Bones.Num(); Start += BonesPerRecord)
		{
			FObjectBuilder Chunk;
			for (int32 BoneIndex = Start; BoneIndex < FMath::Min(Start + BonesPerRecord, Bones.Num()); ++BoneIndex)
			{
				const FIKRigBoneSettingsBase* Settings = Solver.GetBoneSettings(Bones[BoneIndex]);
				FValue Sparse = MakeEmptyObject();
				if (Settings != nullptr && BoneType != nullptr && !EncodeSparse(*BoneType, Settings, BoneDefaults.Get(), Sparse, Error))
				{
					return Fail(Call, TEXT("IKRig.ReadFailed"), FString::Printf(TEXT("Solver %d bone %s: %s"), Index, *Bones[BoneIndex].ToString(), *Error));
				}
				Chunk.Add(Bones[BoneIndex].ToString(), MoveTemp(Sparse));
			}
			FObjectBuilder BonesRecord;
			BonesRecord.AddString(TEXT("kind"), TEXT("solver_bones"));
			BonesRecord.AddInt(TEXT("index"), Index);
			BonesRecord.Add(TEXT("bones"), Chunk.Build());
			Records.Add(BonesRecord.Build());
		}
	}

	if (bInspect)
	{
		FObjectBuilder Facts;
		Facts.AddString(TEXT("kind"), TEXT("solver_facts"));
		Facts.AddInt(TEXT("index"), Index);
		Facts.AddString(TEXT("display_name"), Solver.GetNiceName().ToString());
		FText Warning;
		if (Solver.GetWarningMessage(Warning))
		{
			Facts.AddString(TEXT("warning"), Warning.ToString());
		}
		Facts.AddBool(TEXT("uses_start_bone"), Solver.UsesStartBone());
		Facts.AddBool(TEXT("uses_end_bone"), Solver.UsesEndBone());
		Facts.AddBool(TEXT("uses_goal_settings"), Solver.UsesCustomGoalSettings());
		Facts.AddBool(TEXT("uses_bone_settings"), Solver.UsesCustomBoneSettings());
		Records.Add(Facts.Build());
	}
	return true;
}

void AddChain(const UIKRigDefinition& Rig, const FBoneChain& Chain, bool bInspect, TArray<FValue>& Records)
{
	FObjectBuilder Entry;
	Entry.Add(TEXT("Start"), MakeName(Chain.StartBone.BoneName));
	Entry.Add(TEXT("End"), MakeName(Chain.EndBone.BoneName));
	if (!Chain.IKGoalName.IsNone())
	{
		Entry.Add(TEXT("Goal"), MakeName(Chain.IKGoalName));
	}
	FObjectBuilder Record;
	Record.AddString(TEXT("kind"), TEXT("chain"));
	Record.Add(TEXT("name"), MakeName(Chain.ChainName));
	Record.Add(TEXT("entry"), Entry.Build());
	Records.Add(Record.Build());
	if (bInspect)
	{
		TArray<int32> Indices;
		const FIKRigSkeleton& Skeleton = Rig.GetSkeleton();
		const bool bValid = Skeleton.ValidateChainAndGetBones(Chain, Indices);
		TArray<FString> Bones;
		// ValidateChainAndGetBones lists tip to root; the chain panel reads root to tip.
		for (int32 Position = Indices.Num() - 1; Position >= 0; --Position)
		{
			Bones.Add(Skeleton.GetBoneNameFromIndex(Indices[Position]).ToString());
		}
		FObjectBuilder Facts;
		Facts.AddString(TEXT("kind"), TEXT("chain_bones"));
		Facts.Add(TEXT("name"), MakeName(Chain.ChainName));
		Facts.AddBool(TEXT("valid"), bValid);
		Facts.Add(TEXT("bones"), MakeStringArray(Bones));
		Records.Add(Facts.Build());
	}
}
} // namespace

namespace
{
FTransform GoalFrame(const FTransform& Initial)
{
	FTransform Frame = Initial;
	Frame.NormalizeRotation();
	return Frame;
}
} // namespace

FTransform ApplyGoalOffset(const FTransform& Offset, const FTransform& Initial)
{
	return Offset * GoalFrame(Initial);
}

FValue EncodeGoalOffset(const FTransform& Current, const FTransform& Initial, bool& bOutIdentity)
{
	const FTransform Relative = Current.GetRelativeTransform(GoalFrame(Initial));
	const FVector Location = Relative.GetTranslation();
	const FRotator Rotation = Relative.Rotator();
	FObjectBuilder Offset;
	bOutIdentity = true;
	if (!Location.IsNearlyZero(OffsetTolerance))
	{
		FObjectBuilder Vector;
		Vector.Add(TEXT("X"), MakeNumber(Clean(Location.X)));
		Vector.Add(TEXT("Y"), MakeNumber(Clean(Location.Y)));
		Vector.Add(TEXT("Z"), MakeNumber(Clean(Location.Z)));
		Offset.Add(TEXT("Location"), Vector.Build());
		bOutIdentity = false;
	}
	if (!Rotation.IsNearlyZero(OffsetTolerance))
	{
		FObjectBuilder Rotator;
		Rotator.Add(TEXT("Pitch"), MakeNumber(Clean(Rotation.Pitch)));
		Rotator.Add(TEXT("Yaw"), MakeNumber(Clean(Rotation.Yaw)));
		Rotator.Add(TEXT("Roll"), MakeNumber(Clean(Rotation.Roll)));
		Offset.Add(TEXT("Rotation"), Rotator.Build());
		bOutIdentity = false;
	}
	return Offset.Build();
}

bool BuildIKRigRecords(const UIKRigDefinition& Rig, bool bInspect, FCall& Call, TArray<FValue>& Records, bool bVerify)
{
	const FIKRigSkeleton& Skeleton = Rig.GetSkeleton();

	{
		FObjectBuilder Header;
		Header.AddString(TEXT("kind"), TEXT("header"));
		Header.AddString(TEXT("preview_mesh"), Rig.PreviewSkeletalMesh.IsNull() ? FString() : Rig.PreviewSkeletalMesh.ToSoftObjectPath().ToString());
		TArray<FString> Excluded;
		for (const FName& Bone : GetOrderedExcludedBones(Rig))
		{
			Excluded.Add(Bone.ToString());
		}
		Header.Add(TEXT("excluded_bones"), MakeStringArray(Excluded));
		Header.Add(TEXT("retarget_root"), MakeName(Rig.GetPelvis()));
		Header.AddInt(TEXT("bones"), Skeleton.BoneNames.Num());
		Header.AddInt(TEXT("goals"), Rig.GetGoalArray().Num());
		Header.AddInt(TEXT("solvers"), Rig.GetSolverStructs().Num());
		Header.AddInt(TEXT("chains"), Rig.GetRetargetChains().Num());
		Records.Add(Header.Build());
	}
	for (const UIKRigEffectorGoal* Goal : Rig.GetGoalArray())
	{
		if (Goal != nullptr && !AddGoal(*Goal, Call, Records))
		{
			return false;
		}
	}
	for (int32 Index = 0; Index < Rig.GetSolverStructs().Num(); ++Index)
	{
		if (!AddSolver(Rig, Index, bInspect, bVerify, Call, Records))
		{
			return false;
		}
	}
	for (const FBoneChain& Chain : Rig.GetRetargetChains())
	{
		AddChain(Rig, Chain, bInspect, Records);
	}
	if (bInspect)
	{
		for (int32 Start = 0; Start < Skeleton.BoneNames.Num(); Start += SkeletonBonesPerRecord)
		{
			TArray<FValue> Bones;
			for (int32 Index = Start; Index < FMath::Min(Start + SkeletonBonesPerRecord, Skeleton.BoneNames.Num()); ++Index)
			{
				FObjectBuilder Bone;
				Bone.Add(TEXT("Name"), MakeName(Skeleton.BoneNames[Index]));
				const int32 Parent = Skeleton.ParentIndices.IsValidIndex(Index) ? Skeleton.ParentIndices[Index] : INDEX_NONE;
				Bone.Add(TEXT("Parent"), MakeName(Skeleton.GetBoneNameFromIndex(Parent)));
				if (Skeleton.ExcludedBones.Contains(Skeleton.BoneNames[Index]))
				{
					Bone.AddBool(TEXT("Excluded"), true);
				}
				Bones.Add(Bone.Build());
			}
			FObjectBuilder Record;
			Record.AddString(TEXT("kind"), TEXT("skeleton"));
			Record.AddInt(TEXT("offset"), Start);
			Record.Add(TEXT("bones"), MakeArray(MoveTemp(Bones)));
			Records.Add(Record.Build());
		}
	}
	return true;
}

bool HandleIKRigRead(FCall& Call, FObjectBuilder& Result)
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
	const UIKRigDefinition* Rig = Cast<UIKRigDefinition>(&Call.Asset);
	if (Rig == nullptr)
	{
		return Fail(Call, TEXT("IKRig.UnsupportedAsset"), TEXT("rig.ikrig.read.v1 reads IK Rig assets."), TEXT("/HandleArguments/asset"));
	}
	TArray<FValue> Records;
	if (!BuildIKRigRecords(*Rig, Detail == TEXT("inspect"), Call, Records, Detail == TEXT("verify")))
	{
		return false;
	}
	AddRecordPage(Records, Offset, Result);
	return true;
}
} // namespace UE::AssetDocument::Graph::Rig
