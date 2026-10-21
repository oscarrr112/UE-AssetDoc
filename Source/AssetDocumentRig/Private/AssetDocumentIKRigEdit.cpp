// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// rig.ikrig.edit.v1: edits of an IK Rig as the IK Rig editor makes them (design §4.1): every structural edit is the
// UIKRigController call the editor's UI makes (each its own transaction and re-initialization, nested in the
// request's transaction); Details edits of solver / goal / bone settings and goals are the struct writes
// (AssetDocumentIKRigStructWrites.cpp). Every op is checked before the controller is called, so a refused op names
// its reason and the controller never logs a warning.
//
// Args {ops: [...]}, each {op, ...}:
//   set_mesh {mesh}                                  preview mesh (Asset Details "Preview Mesh"; ExcludedBones kept)
//   exclude {bone, excluded}                         hierarchy "Exclude / Include Selected Bone"
//   goal.add {goal, bone}  goal.remove {goal}  goal.rename {goal, to}  goal.set_bone {goal, bone}
//   goal.set {goal, fields}                          goal Details (complete sparse over the 7 settings)
//   goal.offset {goal, offset: {Location?, Rotation?}}  viewport gizmo: CurrentTransform = offset * InitialTransform
//   solver.add {type}  solver.remove {index}  solver.move {index, to}  solver.enable {index, enabled}
//   solver.start_bone {index, bone}  solver.end_bone {index, bone}
//   solver.connect {index, goal}  solver.disconnect {index, goal}
//   solver.bone_setting.add {index, bone}  solver.bone_setting.remove {index, bone}
//   solver.settings {index, settings}  solver.goal_settings {index, goal, settings}
//   solver.bone_settings {index, bone, settings}     struct writes (complete sparse)
//   chain.add {chain, start, end, goal?}  chain.remove {chain}  chain.rename {chain, to}
//   chain.set {chain, start?, end?, goal?}           chain list columns
//   root {bone}                                      "Set Retarget Root" ("" clears it)
// Result {changed, dependents: [{package, chains: [{chain, to?}], loaded, sides?: [Source|Target], ops?: [...]}]}:
// the IK Retargeters that reference the rig when a chain was renamed or removed (asset registry referencers and loaded
// retargeter assets that name the rig; a loaded one also names the sides that use this rig and its ops). Other
// packages are never edited (decision §8.2).

#include "AssetDocumentEditorNonInteractiveScope.h"
#include "AssetDocumentIKRigStructWrites.h"
#include "AssetDocumentRigControllerScope.h"
#include "AssetDocumentRigPrimitives.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "IKRigLogger.h"
#include "Retargeter/IKRetargeter.h"
#include "Retargeter/IKRetargetOps.h"
#include "Rig/IKRigDefinition.h"
#include "Rig/IKRigProcessor.h"
#include "Rig/IKRigSkeleton.h"
#include "Rig/Solvers/IKRigSolverBase.h"
#include "RigEditor/IKRigController.h"
#include "ScopedTransaction.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/UObjectIterator.h"

#define LOCTEXT_NAMESPACE "AssetDocumentIKRigEdit"

namespace UE::AssetDocument::Graph::Rig
{
namespace
{
struct FEdit
{
	UIKRigDefinition& Rig;
	UIKRigController& Controller;
	bool bChanged = false;
	// Chains renamed (old -> new) or removed (old -> None), for the dependents report.
	TArray<TPair<FName, FName>> ChainEdits;
};

FString JoinNames(const TArray<FString>& Names, int32 Limit = 40)
{
	if (Names.Num() <= Limit)
	{
		return FString::Join(Names, TEXT(", "));
	}
	TArray<FString> Head(Names.GetData(), Limit);
	return FString::Join(Head, TEXT(", ")) + FString::Printf(TEXT(" (+%d more)"), Names.Num() - Limit);
}

// ---- lookups -------------------------------------------------------------------------------------------------------

bool ReadName(FReader& Op, const TCHAR* Key, FName& Out, bool bAllowEmpty = false)
{
	FString Text;
	if (!Op.String(Key, Text))
	{
		return false;
	}
	if (Text.IsEmpty() && !bAllowEmpty)
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("%s is a non-empty name."), Key), Key);
	}
	Out = Text.IsEmpty() ? NAME_None : FName(*Text);
	return true;
}

// Bones whose name contains the wanted one (or the reverse), for "did you mean".
FString CloseBones(const FIKRigSkeleton& Skeleton, FName Wanted)
{
	const FString Lower = Wanted.ToString().ToLower();
	TArray<FString> Close;
	for (const FName& Bone : Skeleton.BoneNames)
	{
		const FString Name = Bone.ToString().ToLower();
		if (!Lower.IsEmpty() && (Name.Contains(Lower) || Lower.Contains(Name)))
		{
			Close.Add(Bone.ToString());
		}
	}
	return Close.IsEmpty() ? FString() : FString::Printf(TEXT(" Close: %s."), *JoinNames(Close, 8));
}

bool RequireBone(FEdit& Edit, FReader& Op, const TCHAR* Key, FName Bone)
{
	const FIKRigSkeleton& Skeleton = Edit.Rig.GetSkeleton();
	if (Skeleton.GetBoneIndexFromName(Bone) != INDEX_NONE)
	{
		return true;
	}
	if (Skeleton.BoneNames.IsEmpty())
	{
		return Op.Fail(TEXT("IKRig.PreviewMeshRequired"), FString::Printf(TEXT("The IK Rig has no skeleton yet, so it has no bone '%s': "
			"set Body.PreviewMesh first."), *Bone.ToString()), Key);
	}
	return Op.Fail(TEXT("IKRig.BoneNotFound"), FString::Printf(TEXT("The preview mesh's skeleton has no bone '%s'.%s"),
		*Bone.ToString(), *CloseBones(Skeleton, Bone)), Key);
}

TArray<FString> GoalNames(const UIKRigDefinition& Rig)
{
	TArray<FString> Names;
	for (const UIKRigEffectorGoal* Goal : Rig.GetGoalArray())
	{
		if (Goal != nullptr)
		{
			Names.Add(Goal->GoalName.ToString());
		}
	}
	return Names;
}

bool RequireGoal(FEdit& Edit, FReader& Op, const TCHAR* Key, FName Goal)
{
	if (Edit.Controller.GetGoalIndex(Goal, ENameCase::CaseSensitive) != INDEX_NONE)
	{
		return true;
	}
	return Op.Fail(TEXT("IKRig.GoalNotFound"), FString::Printf(TEXT("The IK Rig has no goal '%s'. Goals: %s."), *Goal.ToString(),
		*JoinNames(GoalNames(Edit.Rig))), Key);
}

const FBoneChain* FindChain(const UIKRigDefinition& Rig, FName Chain)
{
	return Rig.GetRetargetChains().FindByPredicate([Chain](const FBoneChain& Each) { return Each.ChainName == Chain; });
}

bool RequireChain(FEdit& Edit, FReader& Op, const TCHAR* Key, FName Chain)
{
	if (FindChain(Edit.Rig, Chain) != nullptr)
	{
		return true;
	}
	TArray<FString> Names;
	for (const FBoneChain& Each : Edit.Rig.GetRetargetChains())
	{
		Names.Add(Each.ChainName.ToString());
	}
	return Op.Fail(TEXT("IKRig.ChainNotFound"), FString::Printf(TEXT("The IK Rig has no retarget chain '%s'. Chains: %s."),
		*Chain.ToString(), *JoinNames(Names)), Key);
}

bool ReadSolver(FEdit& Edit, FReader& Op, int32& OutIndex, FIKRigSolverBase*& OutSolver)
{
	int64 Index = 0;
	if (!Op.Int(TEXT("index"), Index))
	{
		return false;
	}
	OutSolver = Index >= 0 && Index < Edit.Controller.GetNumSolvers() ? Edit.Controller.GetSolverAtIndex(static_cast<int32>(Index)) : nullptr;
	if (OutSolver == nullptr)
	{
		return Op.Fail(TEXT("IKRig.SolverNotFound"), FString::Printf(TEXT("The solver stack has no solver %lld (%d solvers)."),
			Index, Edit.Controller.GetNumSolvers()), TEXT("index"));
	}
	OutIndex = static_cast<int32>(Index);
	return true;
}

// The goal name the editor would store for Name ("" when it is not a valid goal name).
bool CheckGoalName(FReader& Op, const TCHAR* Key, FName Name)
{
	FString Clean = Name.ToString();
	UIKRigController::SanitizeGoalName(Clean);
	if (Clean == Name.ToString())
	{
		return true;
	}
	return Op.Fail(TEXT("IKRig.InvalidName"), FString::Printf(TEXT("'%s' is not a valid goal name (the editor would store '%s': "
		"letters, digits and _ only)."), *Name.ToString(), *Clean), Key);
}

bool CheckGoalNameFree(FEdit& Edit, FReader& Op, const TCHAR* Key, FName Name, FName Renamed = NAME_None)
{
	const int32 Existing = Edit.Controller.GetGoalIndex(Name, ENameCase::IgnoreCase);
	if (Existing == INDEX_NONE)
	{
		return true;
	}
	const FName ExistingName = Edit.Rig.GetGoalArray()[Existing]->GoalName;
	if (Renamed != NAME_None && ExistingName == Renamed)
	{
		return Op.Fail(TEXT("IKRig.NameTaken"), FString::Printf(TEXT("Goal names are case-insensitive: '%s' cannot be renamed to '%s' "
			"directly; rename it to another name first."), *Renamed.ToString(), *Name.ToString()), Key);
	}
	return Op.Fail(TEXT("IKRig.NameTaken"), FString::Printf(TEXT("The IK Rig already has a goal named '%s' (goal names are "
		"case-insensitive)."), *ExistingName.ToString()), Key);
}

// ---- mesh and bones ------------------------------------------------------------------------------------------------

bool OpSetMesh(FEdit& Edit, FReader& Op)
{
	FString Path;
	if (!Op.String(TEXT("mesh"), Path))
	{
		return false;
	}
	USkeletalMesh* Mesh = Path.IsEmpty() ? nullptr : LoadObject<USkeletalMesh>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (Mesh == nullptr)
	{
		return Op.Fail(TEXT("IKRig.UnknownMesh"), FString::Printf(TEXT("'%s' is not a Skeletal Mesh (the preview mesh picker lists "
			"Skeletal Meshes; it cannot be cleared)."), *Path), TEXT("mesh"));
	}
	if (Edit.Rig.GetPreviewMesh() == Mesh)
	{
		return true;
	}
	FIKRigLogger Log;
	if (!FIKRigProcessor::IsIKRigCompatibleWithSkeleton(&Edit.Rig, FIKRigInputSkeleton(Mesh), &Log))
	{
		TArray<FString> Reasons;
		for (const FText& Message : Log.GetWarnings())
		{
			Reasons.Add(Message.ToString());
		}
		for (const FText& Message : Log.GetErrors())
		{
			Reasons.Add(Message.ToString());
		}
		return Op.Fail(TEXT("IKRig.IncompatibleMesh"), FString::Printf(TEXT("'%s' lacks bones (or bone parents) the rig needs, so "
			"the editor refuses it: %s"), *Path, *JoinNames(Reasons, 8)), TEXT("mesh"));
	}
	if (!Edit.Controller.SetSkeletalMesh(Mesh))
	{
		return Op.Fail(TEXT("IKRig.EditFailed"), FString::Printf(TEXT("The IK Rig refused the preview mesh '%s'."), *Path), TEXT("mesh"));
	}
	Edit.bChanged = true;
	return true;
}

bool OpExclude(FEdit& Edit, FReader& Op)
{
	FName Bone;
	bool bExcluded = true;
	if (!ReadName(Op, TEXT("bone"), Bone) || !Op.Bool(TEXT("excluded"), bExcluded, true) || !RequireBone(Edit, Op, TEXT("bone"), Bone))
	{
		return false;
	}
	if (Edit.Controller.GetBoneExcluded(Bone) == bExcluded)
	{
		return true;
	}
	Edit.bChanged |= Edit.Controller.SetBoneExcluded(Bone, bExcluded);
	return true;
}

bool OpRoot(FEdit& Edit, FReader& Op)
{
	FName Bone;
	if (!ReadName(Op, TEXT("bone"), Bone, true) || (Bone != NAME_None && !RequireBone(Edit, Op, TEXT("bone"), Bone)))
	{
		return false;
	}
	if (Edit.Controller.GetRetargetRoot() == Bone)
	{
		return true;
	}
	Edit.bChanged |= Edit.Controller.SetRetargetRoot(Bone);
	return true;
}

// ---- goals ---------------------------------------------------------------------------------------------------------

bool OpGoalAdd(FEdit& Edit, FReader& Op)
{
	FName Goal;
	FName Bone;
	if (!ReadName(Op, TEXT("goal"), Goal) || !ReadName(Op, TEXT("bone"), Bone) || !CheckGoalName(Op, TEXT("goal"), Goal)
		|| !CheckGoalNameFree(Edit, Op, TEXT("goal"), Goal) || !RequireBone(Edit, Op, TEXT("bone"), Bone))
	{
		return false;
	}
	if (Edit.Controller.AddNewGoal(Goal, Bone) != Goal)
	{
		return Op.Fail(TEXT("IKRig.GoalNotCreated"), FString::Printf(TEXT("The IK Rig did not create goal '%s'."), *Goal.ToString()), TEXT("goal"));
	}
	Edit.bChanged = true;
	return true;
}

bool OpGoalRemove(FEdit& Edit, FReader& Op)
{
	FName Goal;
	if (!ReadName(Op, TEXT("goal"), Goal) || !RequireGoal(Edit, Op, TEXT("goal"), Goal))
	{
		return false;
	}
	Edit.bChanged |= Edit.Controller.RemoveGoal(Goal);
	return true;
}

bool OpGoalRename(FEdit& Edit, FReader& Op)
{
	FName Goal;
	FName To;
	if (!ReadName(Op, TEXT("goal"), Goal) || !ReadName(Op, TEXT("to"), To) || !RequireGoal(Edit, Op, TEXT("goal"), Goal)
		|| !CheckGoalName(Op, TEXT("to"), To))
	{
		return false;
	}
	if (Goal == To)
	{
		return true;
	}
	if (!CheckGoalNameFree(Edit, Op, TEXT("to"), To, Goal))
	{
		return false;
	}
	if (Edit.Controller.RenameGoal(Goal, To) != To)
	{
		return Op.Fail(TEXT("IKRig.EditFailed"), FString::Printf(TEXT("The IK Rig did not rename goal '%s' to '%s'."), *Goal.ToString(),
			*To.ToString()), TEXT("to"));
	}
	Edit.bChanged = true;
	return true;
}

bool OpGoalSetBone(FEdit& Edit, FReader& Op)
{
	FName Goal;
	FName Bone;
	if (!ReadName(Op, TEXT("goal"), Goal) || !ReadName(Op, TEXT("bone"), Bone) || !RequireGoal(Edit, Op, TEXT("goal"), Goal)
		|| !RequireBone(Edit, Op, TEXT("bone"), Bone))
	{
		return false;
	}
	if (Edit.Controller.GetBoneForGoal(Goal) == Bone)
	{
		return true;
	}
	Edit.bChanged |= Edit.Controller.SetGoalBone(Goal, Bone);
	return true;
}

bool OpGoalSet(FEdit& Edit, FReader& Op)
{
	FName Goal;
	if (!ReadName(Op, TEXT("goal"), Goal) || !RequireGoal(Edit, Op, TEXT("goal"), Goal))
	{
		return false;
	}
	static const TMap<FString, FName> DocumentKeys = {
		{TEXT("PositionAlpha"), TEXT("PositionAlpha")}, {TEXT("RotationAlpha"), TEXT("RotationAlpha")},
		{TEXT("PreviewMode"), TEXT("PreviewMode")}, {TEXT("SizeMultiplier"), TEXT("SizeMultiplier")},
		{TEXT("ThicknessMultiplier"), TEXT("ThicknessMultiplier")}, {TEXT("ExposePosition"), TEXT("bExposePosition")},
		{TEXT("ExposeRotation"), TEXT("bExposeRotation")}};
	static const FName Fields[] = {TEXT("PositionAlpha"), TEXT("RotationAlpha"), TEXT("PreviewMode"), TEXT("SizeMultiplier"),
		TEXT("ThicknessMultiplier"), TEXT("bExposePosition"), TEXT("bExposeRotation")};
	FStructWriteResult Written;
	if (!WriteObjectFields(Op, TEXT("fields"), *Edit.Controller.GetGoal(Goal), Fields, DocumentKeys, Written))
	{
		return false;
	}
	Edit.bChanged |= Written.bChanged;
	return true;
}

bool ReadVector(FReader& Op, const FValue& Value, const TCHAR* Key, const TCHAR* const (&Axes)[3], double (&Out)[3])
{
	FReader Vector = Op.Child(Key, Value);
	if (!Vector.IsObject())
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("%s is an object {%s, %s, %s}."), Key, Axes[0], Axes[1], Axes[2]), Key);
	}
	if (!Vector.OnlyFields({Axes[0], Axes[1], Axes[2]}))
	{
		return false;
	}
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		Out[Axis] = 0.0;
		if (Vector.Has(Axes[Axis]) && !Vector.Number(Axes[Axis], Out[Axis]))
		{
			return false;
		}
	}
	return true;
}

bool OpGoalOffset(FEdit& Edit, FReader& Op)
{
	FName Goal;
	const FValue* Offset = nullptr;
	if (!ReadName(Op, TEXT("goal"), Goal) || !RequireGoal(Edit, Op, TEXT("goal"), Goal) || !Op.Object(TEXT("offset"), Offset))
	{
		return false;
	}
	FReader OffsetReader = Op.Child(TEXT("offset"), *Offset);
	if (!OffsetReader.OnlyFields({TEXT("Location"), TEXT("Rotation")}))
	{
		return false;
	}
	static const TCHAR* const LocationAxes[3] = {TEXT("X"), TEXT("Y"), TEXT("Z")};
	static const TCHAR* const RotationAxes[3] = {TEXT("Pitch"), TEXT("Yaw"), TEXT("Roll")};
	double Location[3] = {0.0, 0.0, 0.0};
	double Rotation[3] = {0.0, 0.0, 0.0};
	if (const FValue* Value = OffsetReader.Find(TEXT("Location")))
	{
		if (!ReadVector(OffsetReader, *Value, TEXT("Location"), LocationAxes, Location))
		{
			return false;
		}
	}
	if (const FValue* Value = OffsetReader.Find(TEXT("Rotation")))
	{
		if (!ReadVector(OffsetReader, *Value, TEXT("Rotation"), RotationAxes, Rotation))
		{
			return false;
		}
	}
	const UIKRigEffectorGoal* GoalObject = Edit.Controller.GetGoal(Goal);
	const FTransform Relative(FRotator(Rotation[0], Rotation[1], Rotation[2]), FVector(Location[0], Location[1], Location[2]));
	const FTransform Wanted = ApplyGoalOffset(Relative, GoalObject->InitialTransform);
	bool bIdentity = true;
	const FValue Current = EncodeGoalOffset(GoalObject->CurrentTransform, GoalObject->InitialTransform, bIdentity);
	bool bWantedIdentity = true;
	const FValue WantedOffset = EncodeGoalOffset(Wanted, GoalObject->InitialTransform, bWantedIdentity);
	if (ValueToJsonString(Current, false) == ValueToJsonString(WantedOffset, false))
	{
		return true;
	}
	// The viewport gizmo: ModifyGoal on begin, SetGoalCurrentTransform while dragging.
	Edit.Controller.ModifyGoal(Goal);
	Edit.Controller.SetGoalCurrentTransform(Goal, Wanted);
	Edit.bChanged = true;
	return true;
}

// ---- solvers -------------------------------------------------------------------------------------------------------

bool OpSolverAdd(FEdit& Edit, FReader& Op)
{
	FString TypeName;
	if (!Op.String(TEXT("type"), TypeName))
	{
		return false;
	}
	UScriptStruct* Type = FindSolverType(TypeName);
	if (Type == nullptr)
	{
		TArray<FString> Names;
		for (const UScriptStruct* Each : GetSolverTypes())
		{
			Names.Add(GetSolverTypeName(*Each));
		}
		return Op.Fail(TEXT("IKRig.UnknownSolverType"), FString::Printf(TEXT("'%s' is not a solver type. Types: %s (list_types kind "
			"ikrig_solver)."), *TypeName, *JoinNames(Names)), TEXT("type"));
	}
	if (Edit.Controller.AddSolver(Type) == INDEX_NONE)
	{
		return Op.Fail(TEXT("IKRig.EditFailed"), FString::Printf(TEXT("The IK Rig did not add a %s solver."), *TypeName), TEXT("type"));
	}
	Edit.bChanged = true;
	return true;
}

bool OpSolverRemove(FEdit& Edit, FReader& Op)
{
	int32 Index = 0;
	FIKRigSolverBase* Solver = nullptr;
	if (!ReadSolver(Edit, Op, Index, Solver))
	{
		return false;
	}
	Edit.bChanged |= Edit.Controller.RemoveSolver(Index);
	return true;
}

bool OpSolverMove(FEdit& Edit, FReader& Op)
{
	int32 Index = 0;
	FIKRigSolverBase* Solver = nullptr;
	int64 To = 0;
	if (!ReadSolver(Edit, Op, Index, Solver) || !Op.Int(TEXT("to"), To))
	{
		return false;
	}
	if (To < 0 || To >= Edit.Controller.GetNumSolvers())
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("to must be a stack position 0..%d."), Edit.Controller.GetNumSolvers() - 1), TEXT("to"));
	}
	if (To == Index)
	{
		return true;
	}
	Edit.bChanged |= Edit.Controller.MoveSolverInStack(Index, static_cast<int32>(To));
	return true;
}

bool OpSolverEnable(FEdit& Edit, FReader& Op)
{
	int32 Index = 0;
	FIKRigSolverBase* Solver = nullptr;
	bool bEnabled = true;
	if (!ReadSolver(Edit, Op, Index, Solver) || !Op.Bool(TEXT("enabled"), bEnabled, true))
	{
		return false;
	}
	if (Solver->IsEnabled() == bEnabled)
	{
		return true;
	}
	Edit.bChanged |= Edit.Controller.SetSolverEnabled(Index, bEnabled);
	return true;
}

bool OpSolverBone(FEdit& Edit, FReader& Op, bool bStart)
{
	int32 Index = 0;
	FIKRigSolverBase* Solver = nullptr;
	FName Bone;
	if (!ReadSolver(Edit, Op, Index, Solver) || !ReadName(Op, TEXT("bone"), Bone))
	{
		return false;
	}
	if (bStart ? !Solver->UsesStartBone() : !Solver->UsesEndBone())
	{
		return Op.Fail(bStart ? TEXT("IKRig.SolverHasNoStartBone") : TEXT("IKRig.SolverHasNoEndBone"), FString::Printf(TEXT("A %s solver "
			"has no %s bone."), *GetSolverTypeName(*Edit.Controller.GetSolverStructAtIndex(Index)->GetScriptStruct()),
			bStart ? TEXT("start") : TEXT("end")), TEXT("bone"));
	}
	if (!RequireBone(Edit, Op, TEXT("bone"), Bone))
	{
		return false;
	}
	if ((bStart ? Solver->GetStartBone() : Solver->GetEndBone()) == Bone)
	{
		return true;
	}
	Edit.bChanged |= bStart ? Edit.Controller.SetStartBone(Bone, Index) : Edit.Controller.SetEndBone(Bone, Index);
	return true;
}

// One-goal solvers report their empty goal slot as NAME_None.
TSet<FName> ConnectedGoals(const FIKRigSolverBase& Solver)
{
	TSet<FName> Goals;
	Solver.GetRequiredGoals(Goals);
	Goals.Remove(NAME_None);
	return Goals;
}

bool IsConnected(const FIKRigSolverBase& Solver, FName Goal)
{
	return ConnectedGoals(Solver).Contains(Goal);
}

bool OpSolverConnect(FEdit& Edit, FReader& Op, bool bConnect)
{
	int32 Index = 0;
	FIKRigSolverBase* Solver = nullptr;
	FName Goal;
	if (!ReadSolver(Edit, Op, Index, Solver) || !ReadName(Op, TEXT("goal"), Goal) || !RequireGoal(Edit, Op, TEXT("goal"), Goal))
	{
		return false;
	}
	if (IsConnected(*Solver, Goal) == bConnect)
	{
		return true;
	}
	if (!bConnect)
	{
		Edit.bChanged |= Edit.Controller.DisconnectGoalFromSolver(Goal, Index);
		return true;
	}
	// Solvers with one goal slot (Limb, Pole, Set Transform, Stretch Limb) replace their goal on connect: refuse
	// instead of dropping the connected one.
	const TSet<FName> Before = ConnectedGoals(*Solver);
	FInstancedStruct Probe = *Edit.Controller.GetSolverStructAtIndex(Index);
	Probe.GetMutable<FIKRigSolverBase>().AddGoal(*Edit.Controller.GetGoal(Goal));
	const TSet<FName> After = ConnectedGoals(Probe.Get<FIKRigSolverBase>());
	for (const FName& Existing : Before)
	{
		if (!After.Contains(Existing))
		{
			return Op.Fail(TEXT("IKRig.SolverGoalLimit"), FString::Printf(TEXT("A %s solver takes one goal; it is connected to '%s'. "
				"Disconnect it first (list it alone in the solver's Goals)."),
				*GetSolverTypeName(*Probe.GetScriptStruct()), *Existing.ToString()), TEXT("goal"));
		}
	}
	// ConnectGoalToSolver does not Modify the asset (the editor's call does not either; we do, for Undo).
	Edit.Rig.Modify();
	Edit.bChanged |= Edit.Controller.ConnectGoalToSolver(Goal, Index);
	return true;
}

bool OpBoneSetting(FEdit& Edit, FReader& Op, bool bAdd)
{
	int32 Index = 0;
	FIKRigSolverBase* Solver = nullptr;
	FName Bone;
	if (!ReadSolver(Edit, Op, Index, Solver) || !ReadName(Op, TEXT("bone"), Bone))
	{
		return false;
	}
	if (!Solver->UsesCustomBoneSettings())
	{
		return Op.Fail(TEXT("IKRig.SolverHasNoBoneSettings"), FString::Printf(TEXT("A %s solver has no bone settings."),
			*GetSolverTypeName(*Edit.Controller.GetSolverStructAtIndex(Index)->GetScriptStruct())), TEXT("bone"));
	}
	if (!RequireBone(Edit, Op, TEXT("bone"), Bone))
	{
		return false;
	}
	if (Solver->HasSettingsOnBone(Bone) == bAdd)
	{
		return true;
	}
	if (bAdd)
	{
		FText Reason;
		if (!Edit.Controller.CanAddBoneSetting(Bone, Index, &Reason))
		{
			return Op.Fail(TEXT("IKRig.BoneSettingRefused"), FString::Printf(TEXT("The solver refuses settings on '%s': %s"), *Bone.ToString(),
				*Reason.ToString()), TEXT("bone"));
		}
		Edit.bChanged |= Edit.Controller.AddBoneSetting(Bone, Index);
		return true;
	}
	Edit.bChanged |= Edit.Controller.RemoveBoneSetting(Bone, Index);
	return true;
}

bool OpSolverSettings(FEdit& Edit, FReader& Op)
{
	int32 Index = 0;
	FIKRigSolverBase* Solver = nullptr;
	if (!ReadSolver(Edit, Op, Index, Solver))
	{
		return false;
	}
	const UScriptStruct* Type = Solver->GetSolverSettingsType();
	FIKRigSolverSettingsBase* Settings = Solver->GetSolverSettings();
	if (Type == nullptr || Settings == nullptr)
	{
		return Op.Fail(TEXT("IKRig.FieldNotEditable"), TEXT("This solver has no settings."), TEXT("settings"));
	}
	FInstancedStruct DefaultSolver(Edit.Controller.GetSolverStructAtIndex(Index)->GetScriptStruct());
	const FIKRigSolverSettingsBase* Defaults = DefaultSolver.GetMutable<FIKRigSolverBase>().GetSolverSettings();
	FStructWriteResult Written;
	if (!WriteSettingsStruct(Op, TEXT("settings"), *Type, Settings, Defaults, Edit.Rig, [](const FProperty&) { return false; }, Written))
	{
		return false;
	}
	Edit.bChanged |= Written.bChanged;
	return true;
}

bool OpGoalSettings(FEdit& Edit, FReader& Op)
{
	int32 Index = 0;
	FIKRigSolverBase* Solver = nullptr;
	FName Goal;
	if (!ReadSolver(Edit, Op, Index, Solver) || !ReadName(Op, TEXT("goal"), Goal))
	{
		return false;
	}
	const UScriptStruct* Type = Solver->UsesCustomGoalSettings() ? Solver->GetGoalSettingsType() : nullptr;
	FIKRigGoalSettingsBase* Settings = Type != nullptr ? Solver->GetGoalSettings(Goal) : nullptr;
	if (Type == nullptr)
	{
		return Op.Fail(TEXT("IKRig.SolverHasNoGoalSettings"), TEXT("This solver has no per-goal settings."), TEXT("settings"));
	}
	if (Settings == nullptr)
	{
		return Op.Fail(TEXT("IKRig.GoalNotFound"), FString::Printf(TEXT("Goal '%s' is not connected to this solver."), *Goal.ToString()), TEXT("goal"));
	}
	const FStructInstance Defaults(Type);
	FStructWriteResult Written;
	if (!WriteSettingsStruct(Op, TEXT("settings"), *Type, Settings, Defaults.Get(), Edit.Rig, [](const FProperty&) { return false; }, Written))
	{
		return false;
	}
	Edit.bChanged |= Written.bChanged;
	return true;
}

bool OpBoneSettings(FEdit& Edit, FReader& Op)
{
	int32 Index = 0;
	FIKRigSolverBase* Solver = nullptr;
	FName Bone;
	if (!ReadSolver(Edit, Op, Index, Solver) || !ReadName(Op, TEXT("bone"), Bone))
	{
		return false;
	}
	const UScriptStruct* Type = Solver->UsesCustomBoneSettings() ? Solver->GetBoneSettingsType() : nullptr;
	FIKRigBoneSettingsBase* Settings = Type != nullptr ? Solver->GetBoneSettings(Bone) : nullptr;
	if (Type == nullptr)
	{
		return Op.Fail(TEXT("IKRig.SolverHasNoBoneSettings"), TEXT("This solver has no bone settings."), TEXT("settings"));
	}
	if (Settings == nullptr)
	{
		return Op.Fail(TEXT("IKRig.BoneNotFound"), FString::Printf(TEXT("The solver has no settings on bone '%s'."), *Bone.ToString()), TEXT("bone"));
	}
	const FStructInstance Defaults(Type);
	FStructWriteResult Written;
	if (!WriteSettingsStruct(Op, TEXT("settings"), *Type, Settings, Defaults.Get(), Edit.Rig, [](const FProperty&) { return false; }, Written))
	{
		return false;
	}
	Edit.bChanged |= Written.bChanged;
	return true;
}

// ---- retarget chains -----------------------------------------------------------------------------------------------

bool ReadChainGoal(FEdit& Edit, FReader& Op, FName& OutGoal)
{
	OutGoal = NAME_None;
	if (!Op.Has(TEXT("goal")))
	{
		return true;
	}
	if (!ReadName(Op, TEXT("goal"), OutGoal, true))
	{
		return false;
	}
	return OutGoal == NAME_None || RequireGoal(Edit, Op, TEXT("goal"), OutGoal);
}

bool OpChainAdd(FEdit& Edit, FReader& Op)
{
	FName Chain;
	FName Start;
	FName End;
	FName Goal;
	if (!ReadName(Op, TEXT("chain"), Chain) || !ReadName(Op, TEXT("start"), Start, true) || !ReadName(Op, TEXT("end"), End, true)
		|| !ReadChainGoal(Edit, Op, Goal))
	{
		return false;
	}
	if (FindChain(Edit.Rig, Chain) != nullptr || Edit.Controller.GetUniqueRetargetChainName(Chain) != Chain)
	{
		return Op.Fail(TEXT("IKRig.NameTaken"), FString::Printf(TEXT("The IK Rig already has a retarget chain named '%s'."), *Chain.ToString()), TEXT("chain"));
	}
	if ((Start != NAME_None && !RequireBone(Edit, Op, TEXT("start"), Start)) || (End != NAME_None && !RequireBone(Edit, Op, TEXT("end"), End)))
	{
		return false;
	}
	if (Edit.Controller.AddRetargetChain(Chain, Start, End, Goal) != Chain)
	{
		return Op.Fail(TEXT("IKRig.EditFailed"), FString::Printf(TEXT("The IK Rig did not add retarget chain '%s'."), *Chain.ToString()), TEXT("chain"));
	}
	Edit.bChanged = true;
	return true;
}

bool OpChainRemove(FEdit& Edit, FReader& Op)
{
	FName Chain;
	if (!ReadName(Op, TEXT("chain"), Chain) || !RequireChain(Edit, Op, TEXT("chain"), Chain))
	{
		return false;
	}
	if (Edit.Controller.RemoveRetargetChain(Chain))
	{
		Edit.ChainEdits.Emplace(Chain, FName());
		Edit.bChanged = true;
	}
	return true;
}

bool OpChainRename(FEdit& Edit, FReader& Op)
{
	FName Chain;
	FName To;
	if (!ReadName(Op, TEXT("chain"), Chain) || !ReadName(Op, TEXT("to"), To) || !RequireChain(Edit, Op, TEXT("chain"), Chain))
	{
		return false;
	}
	if (Chain == To)
	{
		return true;
	}
	if (Edit.Controller.GetUniqueRetargetChainName(To) != To)
	{
		return Op.Fail(TEXT("IKRig.NameTaken"), FString::Printf(TEXT("The IK Rig already has a retarget chain named '%s' (chain names are "
			"case-insensitive)."), *To.ToString()), TEXT("to"));
	}
	if (Edit.Controller.RenameRetargetChain(Chain, To) != To)
	{
		return Op.Fail(TEXT("IKRig.EditFailed"), FString::Printf(TEXT("The IK Rig did not rename retarget chain '%s'."), *Chain.ToString()), TEXT("to"));
	}
	Edit.ChainEdits.Emplace(Chain, To);
	Edit.bChanged = true;
	return true;
}

bool OpChainSet(FEdit& Edit, FReader& Op)
{
	FName Chain;
	if (!ReadName(Op, TEXT("chain"), Chain) || !RequireChain(Edit, Op, TEXT("chain"), Chain))
	{
		return false;
	}
	FName Start = NAME_None;
	FName End = NAME_None;
	FName Goal = NAME_None;
	if ((Op.Has(TEXT("start")) && (!ReadName(Op, TEXT("start"), Start) || !RequireBone(Edit, Op, TEXT("start"), Start)))
		|| (Op.Has(TEXT("end")) && (!ReadName(Op, TEXT("end"), End) || !RequireBone(Edit, Op, TEXT("end"), End)))
		|| !ReadChainGoal(Edit, Op, Goal))
	{
		return false;
	}
	const FBoneChain Current = *FindChain(Edit.Rig, Chain);
	if (Start != NAME_None && Current.StartBone.BoneName != Start)
	{
		Edit.bChanged |= Edit.Controller.SetRetargetChainStartBone(Chain, Start);
	}
	if (End != NAME_None && Current.EndBone.BoneName != End)
	{
		Edit.bChanged |= Edit.Controller.SetRetargetChainEndBone(Chain, End);
	}
	if (Op.Has(TEXT("goal")) && Current.IKGoalName != Goal)
	{
		Edit.bChanged |= Edit.Controller.SetRetargetChainGoal(Chain, Goal);
	}
	return true;
}

bool RunOp(FEdit& Edit, FReader& Op, const FString& Kind)
{
	if (Kind == TEXT("set_mesh")) { return OpSetMesh(Edit, Op); }
	if (Kind == TEXT("exclude")) { return OpExclude(Edit, Op); }
	if (Kind == TEXT("root")) { return OpRoot(Edit, Op); }
	if (Kind == TEXT("goal.add")) { return OpGoalAdd(Edit, Op); }
	if (Kind == TEXT("goal.remove")) { return OpGoalRemove(Edit, Op); }
	if (Kind == TEXT("goal.rename")) { return OpGoalRename(Edit, Op); }
	if (Kind == TEXT("goal.set_bone")) { return OpGoalSetBone(Edit, Op); }
	if (Kind == TEXT("goal.set")) { return OpGoalSet(Edit, Op); }
	if (Kind == TEXT("goal.offset")) { return OpGoalOffset(Edit, Op); }
	if (Kind == TEXT("solver.add")) { return OpSolverAdd(Edit, Op); }
	if (Kind == TEXT("solver.remove")) { return OpSolverRemove(Edit, Op); }
	if (Kind == TEXT("solver.move")) { return OpSolverMove(Edit, Op); }
	if (Kind == TEXT("solver.enable")) { return OpSolverEnable(Edit, Op); }
	if (Kind == TEXT("solver.start_bone")) { return OpSolverBone(Edit, Op, true); }
	if (Kind == TEXT("solver.end_bone")) { return OpSolverBone(Edit, Op, false); }
	if (Kind == TEXT("solver.connect")) { return OpSolverConnect(Edit, Op, true); }
	if (Kind == TEXT("solver.disconnect")) { return OpSolverConnect(Edit, Op, false); }
	if (Kind == TEXT("solver.bone_setting.add")) { return OpBoneSetting(Edit, Op, true); }
	if (Kind == TEXT("solver.bone_setting.remove")) { return OpBoneSetting(Edit, Op, false); }
	if (Kind == TEXT("solver.settings")) { return OpSolverSettings(Edit, Op); }
	if (Kind == TEXT("solver.goal_settings")) { return OpGoalSettings(Edit, Op); }
	if (Kind == TEXT("solver.bone_settings")) { return OpBoneSettings(Edit, Op); }
	if (Kind == TEXT("chain.add")) { return OpChainAdd(Edit, Op); }
	if (Kind == TEXT("chain.remove")) { return OpChainRemove(Edit, Op); }
	if (Kind == TEXT("chain.rename")) { return OpChainRename(Edit, Op); }
	if (Kind == TEXT("chain.set")) { return OpChainSet(Edit, Op); }
	return Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("Unknown op '%s'."), *Kind), TEXT("op"));
}

// The fields an op may carry besides "op" (an unknown field is refused, like OnlyFields).
bool CheckOpFields(FReader& Op, const FValue& Value, const FString& Kind)
{
	static const TMap<FString, TArray<FString>> Fields = {
		{TEXT("set_mesh"), {TEXT("mesh")}}, {TEXT("exclude"), {TEXT("bone"), TEXT("excluded")}}, {TEXT("root"), {TEXT("bone")}},
		{TEXT("goal.add"), {TEXT("goal"), TEXT("bone")}}, {TEXT("goal.remove"), {TEXT("goal")}},
		{TEXT("goal.rename"), {TEXT("goal"), TEXT("to")}}, {TEXT("goal.set_bone"), {TEXT("goal"), TEXT("bone")}},
		{TEXT("goal.set"), {TEXT("goal"), TEXT("fields")}}, {TEXT("goal.offset"), {TEXT("goal"), TEXT("offset")}},
		{TEXT("solver.add"), {TEXT("type")}}, {TEXT("solver.remove"), {TEXT("index")}}, {TEXT("solver.move"), {TEXT("index"), TEXT("to")}},
		{TEXT("solver.enable"), {TEXT("index"), TEXT("enabled")}}, {TEXT("solver.start_bone"), {TEXT("index"), TEXT("bone")}},
		{TEXT("solver.end_bone"), {TEXT("index"), TEXT("bone")}}, {TEXT("solver.connect"), {TEXT("index"), TEXT("goal")}},
		{TEXT("solver.disconnect"), {TEXT("index"), TEXT("goal")}}, {TEXT("solver.bone_setting.add"), {TEXT("index"), TEXT("bone")}},
		{TEXT("solver.bone_setting.remove"), {TEXT("index"), TEXT("bone")}}, {TEXT("solver.settings"), {TEXT("index"), TEXT("settings")}},
		{TEXT("solver.goal_settings"), {TEXT("index"), TEXT("goal"), TEXT("settings")}},
		{TEXT("solver.bone_settings"), {TEXT("index"), TEXT("bone"), TEXT("settings")}},
		{TEXT("chain.add"), {TEXT("chain"), TEXT("start"), TEXT("end"), TEXT("goal")}}, {TEXT("chain.remove"), {TEXT("chain")}},
		{TEXT("chain.rename"), {TEXT("chain"), TEXT("to")}}, {TEXT("chain.set"), {TEXT("chain"), TEXT("start"), TEXT("end"), TEXT("goal")}}};
	const TArray<FString>* Allowed = Fields.Find(Kind);
	if (Allowed == nullptr)
	{
		return true; // RunOp names the unknown op
	}
	for (const FString& Key : Value.ObjectKeys)
	{
		if (Key != TEXT("op") && !Allowed->Contains(Key))
		{
			return Op.Fail(TEXT("Graph.UnknownField"), FString::Printf(TEXT("'%s' takes %s; '%s' is not one of them."), *Kind,
				*FString::Join(*Allowed, TEXT(", ")), *Key), Key);
		}
	}
	return true;
}

// ---- dependents ----------------------------------------------------------------------------------------------------

// The retargeters that use the rig: asset registry referencers (saved ones) plus loaded retargeter assets that name the
// rig on a side (one created or re-pointed since its last save is not a registry referencer yet).
TMap<FName, const UIKRetargeter*> FindRetargeters(const UIKRigDefinition& Rig)
{
	TMap<FName, const UIKRetargeter*> Found;
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FName> Referencers;
	Registry.GetReferencers(Rig.GetOutermost()->GetFName(), Referencers, UE::AssetRegistry::EDependencyCategory::Package);
	for (const FName& Package : Referencers)
	{
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPackageName(Package, Assets, true);
		for (const FAssetData& Asset : Assets)
		{
			if (Asset.AssetClassPath == UIKRetargeter::StaticClass()->GetClassPathName())
			{
				// Only a retargeter that is already loaded is looked at (loading one would re-sync it with this rig).
				Found.Add(Package, Cast<UIKRetargeter>(Asset.FastGetAsset(false)));
			}
		}
	}
	for (TObjectIterator<UIKRetargeter> It; It; ++It)
	{
		const UIKRetargeter* Retargeter = *It;
		if (!IsValid(Retargeter) || !Retargeter->IsAsset() || Retargeter->GetOutermost()->GetName().StartsWith(TEXT("/Temp/")))
		{
			continue;
		}
		if (Retargeter->GetIKRig(ERetargetSourceOrTarget::Source) == &Rig || Retargeter->GetIKRig(ERetargetSourceOrTarget::Target) == &Rig)
		{
			Found.Add(Retargeter->GetOutermost()->GetFName(), Retargeter);
		}
	}
	Found.KeySort(FNameLexicalLess());
	return Found;
}

void AddDependents(const FEdit& Edit, FObjectBuilder& Result)
{
	TArray<FValue> Dependents;
	if (!Edit.ChainEdits.IsEmpty())
	{
		for (const TPair<FName, const UIKRetargeter*>& Found : FindRetargeters(Edit.Rig))
		{
			FObjectBuilder Dependent;
			Dependent.AddString(TEXT("package"), Found.Key.ToString());
			TArray<FValue> Chains;
			for (const TPair<FName, FName>& Chain : Edit.ChainEdits)
			{
				FObjectBuilder Row;
				Row.Add(TEXT("chain"), MakeName(Chain.Key));
				if (Chain.Value != NAME_None)
				{
					Row.Add(TEXT("to"), MakeName(Chain.Value));
				}
				Chains.Add(Row.Build());
			}
			Dependent.Add(TEXT("chains"), MakeArray(MoveTemp(Chains)));
			const UIKRetargeter* Retargeter = Found.Value;
			Dependent.AddBool(TEXT("loaded"), Retargeter != nullptr);
			if (Retargeter != nullptr)
			{
				TArray<FString> Sides;
				if (Retargeter->GetIKRig(ERetargetSourceOrTarget::Source) == &Edit.Rig)
				{
					Sides.Add(TEXT("Source"));
				}
				if (Retargeter->GetIKRig(ERetargetSourceOrTarget::Target) == &Edit.Rig)
				{
					Sides.Add(TEXT("Target"));
				}
				Dependent.Add(TEXT("sides"), MakeStringArray(Sides));
				TArray<FString> Ops;
				for (const FInstancedStruct& OpStruct : Retargeter->GetRetargetOps())
				{
					if (const FIKRetargetOpBase* RetargetOp = OpStruct.GetPtr<FIKRetargetOpBase>())
					{
						Ops.Add(RetargetOp->GetName().ToString());
					}
				}
				Dependent.Add(TEXT("ops"), MakeStringArray(Ops));
			}
			Dependents.Add(Dependent.Build());
		}
	}
	Result.Add(TEXT("dependents"), MakeArray(MoveTemp(Dependents)));
}

} // namespace

bool HandleIKRigEdit(FCall& Call, FObjectBuilder& Result)
{
	const FValue* Ops = nullptr;
	if (!Call.Args.OnlyFields({TEXT("ops")}) || !Call.Args.Array(TEXT("ops"), Ops))
	{
		return false;
	}
	UIKRigDefinition* Rig = Cast<UIKRigDefinition>(&Call.Asset);
	if (Rig == nullptr)
	{
		return Fail(Call, TEXT("IKRig.UnsupportedAsset"), TEXT("rig.ikrig.edit.v1 edits IK Rig assets."), TEXT("/HandleArguments/asset"));
	}
	// Declared first: the controller is released after the transaction and every controller call ended.
	const TScopedRigController<UIKRigDefinition> ControllerScope(*Rig);
	UIKRigController* Controller = UIKRigController::GetController(Rig);
	if (Controller == nullptr)
	{
		return Fail(Call, TEXT("IKRig.EditFailed"), TEXT("No IK Rig controller for the asset."));
	}
	FAssetDocumentEditorNonInteractiveScope NonInteractive;
	const FScopedTransaction Transaction(LOCTEXT("EditIKRig", "Edit IK Rig"));
	FEdit Edit{*Rig, *Controller};
	for (int32 Index = 0; Index < Ops->Elements.Num(); ++Index)
	{
		const FString OpPath = FString::Printf(TEXT("ops/%d"), Index);
		FReader Op = Call.Args.Child(OpPath, Ops->Elements[Index]);
		FString Kind;
		if (!Op.IsObject())
		{
			return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("Each op is an object."));
		}
		if (!Op.String(TEXT("op"), Kind) || !CheckOpFields(Op, Ops->Elements[Index], Kind) || !RunOp(Edit, Op, Kind))
		{
			return false;
		}
		if (NonInteractive.WasInteractionRefused())
		{
			NonInteractive.DiscardCreatedPackages();
			return Op.Fail(TEXT("IKRig.InteractiveEditRefused"), TEXT("The edit tried to open an editor dialog, which an MCP request cannot "
				"answer; the edit was refused."));
		}
	}
	if (Edit.bChanged)
	{
		Rig->MarkPackageDirty();
	}
	Result.AddBool(TEXT("changed"), Edit.bChanged);
	AddDependents(Edit, Result);
	return true;
}
} // namespace UE::AssetDocument::Graph::Rig

#undef LOCTEXT_NAMESPACE
