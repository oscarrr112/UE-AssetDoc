// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// rig.suggest.v1: what the editors' automatic setup buttons would make, computed on a transient copy of the asset
// (decision §8.5: suggestions are read-only Inspect facts; the document applies them). Never changes the asset: the
// copy lives in the transient package with its own controller, and the buttons' undo transactions are cancelled
// (one enclosing transaction, cancelled, when no transaction was active).
//
// IK Rig (needs a preview mesh) -- the IK Rig editor's "Auto Create Retarget Chains" (auto characterize: a known
// skeleton template's retarget root and chains) then "Auto Create IK" (a Full Body IK setup for retargeting):
//   the rig.ikrig.read.v1 records of the copy after both buttons, then
//   {kind: "characterization", used_template, template, score, matching_bones, missing_bones[],
//    bones_with_missing_parent[], expanded_chains{}}
//   {kind: "fbik", outcome: AllOk | MissingMesh | UnknownSkeletonType | MissingChains | MissingRootBone
//    (MissingPelvisBone on engines that name it so), missing_chains[]}
// IK Retargeter -- the IK Retarget editor's "Auto Align All Bones" per side and method (sides with an IK Rig and a
// preview mesh, both meshes needed), on the current retarget pose reset first, and "Auto Map Chains" (Exact, Fuzzy,
// forced) per op with a chain mapping:
//   {kind: "header", ...} (the copy's rig.ikretargeter.read.v1 header)
//   {kind: "align", side, method, root_offset?, bones: {<bone>: {X,Y,Z,W}}}   (chunked; merged into bones)
//   {kind: "map", op, type: Exact | Fuzzy, mapping: {<target chain>: <source chain> | null}}
// Args {offset?, methods?: [ChainToChain | MeshToMesh | LocalRotationAxes | GlobalRotationAxes]} (default
// ChainToChain); records are paged like the reads.

#include "AssetDocumentRigControllerScope.h"
#include "AssetDocumentRigPrimitives.h"
#include "Algo/Find.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/EngineVersionComparison.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "RetargetEditor/IKRetargeterPoseGenerator.h"
#include "Retargeter/IKRetargetChainMapping.h"
#include "Retargeter/IKRetargeter.h"
#include "Retargeter/IKRetargetOps.h"
#include "Rig/IKRigDefinition.h"
#include "RigEditor/IKRigAutoCharacterizer.h"
#include "RigEditor/IKRigAutoFBIK.h"
#include "RigEditor/IKRigController.h"
#include "ScopedTransaction.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "AssetDocumentRigSuggest"

namespace UE::AssetDocument::Graph::Rig
{
namespace
{
constexpr int32 AlignBonesPerRecord = 100;

// The buttons' undo transactions, cancelled: one enclosing transaction when none was active (an active one -- an
// apply's -- only records the transient copy, which nothing restores).
class FCancelledTransaction
{
public:
	FCancelledTransaction()
	{
		if (GEditor != nullptr && !GEditor->IsTransactionActive())
		{
			Transaction.Emplace(LOCTEXT("Suggest", "Suggest Rig Setup"));
		}
	}
	~FCancelledTransaction()
	{
		if (Transaction.IsSet())
		{
			Transaction->Cancel();
		}
	}

private:
	TOptional<FScopedTransaction> Transaction;
};

template <typename AssetType>
AssetType* TransientCopy(const AssetType& Asset)
{
	AssetType* Copy = DuplicateObject<AssetType>(&Asset, GetTransientPackage(), NAME_None);
	if (Copy != nullptr)
	{
		Copy->ClearFlags(RF_Public | RF_Standalone);
		Copy->SetFlags(RF_Transient);
		// Never the original's controller (a transient field is not duplicated, but be explicit).
		Copy->Controller = nullptr;
	}
	return Copy;
}

TArray<FString> NameStrings(const TArray<FName>& Names)
{
	TArray<FString> Out;
	for (const FName& Name : Names)
	{
		Out.Add(Name.ToString());
	}
	return Out;
}

const TCHAR* FBIKOutcomeName(EAutoFBIKResult Outcome)
{
	switch (Outcome)
	{
	case EAutoFBIKResult::AllOk: return TEXT("AllOk");
	case EAutoFBIKResult::MissingMesh: return TEXT("MissingMesh");
	case EAutoFBIKResult::UnknownSkeletonType: return TEXT("UnknownSkeletonType");
	case EAutoFBIKResult::MissingChains: return TEXT("MissingChains");
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	case EAutoFBIKResult::MissingPelvisBone: return TEXT("MissingPelvisBone");
#else
	case EAutoFBIKResult::MissingRootBone: return TEXT("MissingRootBone");
#endif
	}
	return TEXT("Unknown");
}

bool SuggestIKRig(const UIKRigDefinition& Rig, FCall& Call, TArray<FValue>& Records)
{
	if (Rig.GetPreviewMesh() == nullptr)
	{
		return Fail(Call, TEXT("IKRig.PreviewMeshRequired"), TEXT("The automatic setup characterizes the preview mesh's skeleton: set "
			"Body.PreviewMesh first."), TEXT("/HandleArguments/asset"));
	}
	UIKRigDefinition* Copy = TransientCopy(Rig);
	if (Copy == nullptr)
	{
		return Fail(Call, TEXT("IKRig.ReadFailed"), TEXT("Could not copy the IK Rig."));
	}
	bool bOk = true;
	{
		const FCancelledTransaction Transaction;
		const TScopedRigController<UIKRigDefinition> ControllerScope(*Copy);
		UIKRigController* Controller = UIKRigController::GetController(Copy);
		// "Auto Create Retarget Chains" (UIKRigController::ApplyAutoGeneratedRetargetDefinition): only a template match
		// replaces the retarget definition.
		FAutoCharacterizeResults Characterized;
		Controller->AutoGenerateRetargetDefinition(Characterized);
		if (Characterized.bUsedTemplate)
		{
			Controller->SetRetargetDefinition(Characterized.AutoRetargetDefinition.RetargetDefinition);
		}
		// "Auto Create IK" (UIKRigController::ApplyAutoFBIK).
		FAutoFBIKResults FBIK;
		Controller->AutoGenerateFBIK(FBIK);
		bOk = BuildIKRigRecords(*Copy, false, Call, Records);

		FObjectBuilder Characterization;
		Characterization.AddString(TEXT("kind"), TEXT("characterization"));
		Characterization.AddBool(TEXT("used_template"), Characterized.bUsedTemplate);
		Characterization.Add(TEXT("template"), MakeName(Characterized.BestTemplateName));
		Characterization.Add(TEXT("score"), MakeNumber(ShortestFloat(Characterized.BestPercentageOfTemplateScore)));
		Characterization.AddInt(TEXT("matching_bones"), Characterized.BestNumMatchingBones);
		Characterization.Add(TEXT("missing_bones"), MakeStringArray(NameStrings(Characterized.MissingBones)));
		Characterization.Add(TEXT("bones_with_missing_parent"), MakeStringArray(NameStrings(Characterized.BonesWithMissingParent)));
		FObjectBuilder Expanded;
		for (const TPair<FName, int32>& Chain : Characterized.ExpandedChains)
		{
			Expanded.AddInt(Chain.Key.ToString(), Chain.Value);
		}
		Characterization.Add(TEXT("expanded_chains"), Expanded.Build());
		Records.Add(Characterization.Build());

		FObjectBuilder FBIKRecord;
		FBIKRecord.AddString(TEXT("kind"), TEXT("fbik"));
		FBIKRecord.AddString(TEXT("outcome"), FBIKOutcomeName(FBIK.Outcome));
		FBIKRecord.Add(TEXT("missing_chains"), MakeStringArray(NameStrings(FBIK.MissingChains)));
		Records.Add(FBIKRecord.Build());
	}
	Copy->MarkAsGarbage();
	return bOk;
}

FValue SuggestQuat(const FQuat& Value)
{
	FObjectBuilder Object;
	Object.Add(TEXT("X"), MakeNumber(Value.X));
	Object.Add(TEXT("Y"), MakeNumber(Value.Y));
	Object.Add(TEXT("Z"), MakeNumber(Value.Z));
	Object.Add(TEXT("W"), MakeNumber(Value.W));
	return Object.Build();
}

void AddAlignedPose(const UIKRetargeterController& Controller, const UIKRetargeter& Copy, ERetargetSourceOrTarget Side,
	const FString& Method, TArray<FValue>& Records)
{
	const FIKRetargetPose& Pose = Controller.GetCurrentRetargetPose(Side);
	FObjectBuilder Record;
	Record.AddString(TEXT("kind"), TEXT("align"));
	Record.AddString(TEXT("side"), RetargetSideName(Side));
	Record.AddString(TEXT("method"), Method);
	if (!FMath::IsNearlyZero(Pose.GetRootTranslationDelta().Z, 0.0))
	{
		Record.Add(TEXT("root_offset"), MakeNumber(Pose.GetRootTranslationDelta().Z));
	}
	// Bones in skeleton order of the side's IK Rig, identity rotations omitted (as the read lists a pose).
	const TMap<FName, FQuat>& Rotations = Pose.GetAllDeltaRotations();
	TArray<FName> Bones;
	for (const FName& Bone : Copy.GetIKRig(Side)->GetSkeleton().BoneNames)
	{
		const FQuat* Rotation = Rotations.Find(Bone);
		if (Rotation != nullptr && !Rotation->Equals(FQuat::Identity, 0.0))
		{
			Bones.Add(Bone);
		}
	}
	FObjectBuilder First;
	for (int32 Bone = 0; Bone < FMath::Min(AlignBonesPerRecord, Bones.Num()); ++Bone)
	{
		First.Add(Bones[Bone].ToString(), SuggestQuat(Rotations[Bones[Bone]]));
	}
	Record.Add(TEXT("bones"), First.Build());
	Records.Add(Record.Build());
	for (int32 Start = AlignBonesPerRecord; Start < Bones.Num(); Start += AlignBonesPerRecord)
	{
		FObjectBuilder Chunk;
		for (int32 Bone = Start; Bone < FMath::Min(Start + AlignBonesPerRecord, Bones.Num()); ++Bone)
		{
			Chunk.Add(Bones[Bone].ToString(), SuggestQuat(Rotations[Bones[Bone]]));
		}
		FObjectBuilder More;
		More.AddString(TEXT("kind"), TEXT("align"));
		More.AddString(TEXT("side"), RetargetSideName(Side));
		More.AddString(TEXT("method"), Method);
		More.Add(TEXT("bones"), Chunk.Build());
		Records.Add(More.Build());
	}
}

bool SuggestRetargeter(const UIKRetargeter& Retargeter, const TArray<TPair<FString, ERetargetAutoAlignMethod>>& Methods, FCall& Call,
	TArray<FValue>& Records)
{
	UIKRetargeter* Copy = TransientCopy(Retargeter);
	if (Copy == nullptr)
	{
		return Fail(Call, TEXT("IKRetargeter.ReadFailed"), TEXT("Could not copy the IK Retargeter."));
	}
	bool bOk = true;
	{
		const FCancelledTransaction Transaction;
		const TScopedRigController<UIKRetargeter> ControllerScope(*Copy);
		UIKRetargeterController* Controller = UIKRetargeterController::GetController(Copy);
		TArray<FValue> Read;
		bOk = BuildIKRetargeterRecords(*Copy, false, Call, Read);
		if (bOk)
		{
			Records.Add(Read[0]);
		}
		// Auto-align needs both meshes (the pose generator initializes a processor on them) and the side's IK Rig.
		const bool bMeshes = Controller->GetPreviewMesh(ERetargetSourceOrTarget::Source) != nullptr
			&& Controller->GetPreviewMesh(ERetargetSourceOrTarget::Target) != nullptr;
		// Each suggestion is the toolbar's Auto Align on the asset as it is: the side aligns against the other side's
		// current pose, so both sides' poses are restored (and the processor re-initialized) after each run: no
		// suggestion builds on another.
		const TMap<FName, FIKRetargetPose> SourcePoses = Controller->GetRetargetPoses(ERetargetSourceOrTarget::Source);
		const TMap<FName, FIKRetargetPose> TargetPoses = Controller->GetRetargetPoses(ERetargetSourceOrTarget::Target);
		for (const ERetargetSourceOrTarget Side : {ERetargetSourceOrTarget::Source, ERetargetSourceOrTarget::Target})
		{
			if (!bOk || !bMeshes || Copy->GetIKRig(Side) == nullptr)
			{
				continue;
			}
			for (const TPair<FString, ERetargetAutoAlignMethod>& Method : Methods)
			{
				Controller->AutoAlignAllBones(Side, Method.Value);
				AddAlignedPose(*Controller, *Copy, Side, Method.Key, Records);
				Controller->GetRetargetPoses(ERetargetSourceOrTarget::Source) = SourcePoses;
				Controller->GetRetargetPoses(ERetargetSourceOrTarget::Target) = TargetPoses;
				// The pose generator's processor re-initializes only on a new asset version (it would keep the aligned pose).
				Copy->IncrementVersion();
			}
		}
		for (int32 Index = 0; bOk && Index < Copy->GetRetargetOps().Num(); ++Index)
		{
			const FIKRetargetOpBase* Op = Copy->GetRetargetOps()[Index].GetPtr<FIKRetargetOpBase>();
			if (Op == nullptr || Op->GetChainMapping() == nullptr)
			{
				continue;
			}
			const FName OpName = Op->GetName();
			for (const TPair<const TCHAR*, EAutoMapChainType>& Type : {TPair<const TCHAR*, EAutoMapChainType>(TEXT("Exact"), EAutoMapChainType::Exact),
				TPair<const TCHAR*, EAutoMapChainType>(TEXT("Fuzzy"), EAutoMapChainType::Fuzzy)})
			{
				Controller->AutoMapChains(Type.Value, true, OpName);
				FObjectBuilder Pairs;
				for (const FRetargetChainPair& Pair : Controller->GetChainMapping(OpName)->GetChainPairs())
				{
					Pairs.Add(Pair.TargetChainName.ToString(), Pair.SourceChainName.IsNone() ? MakeNull() : MakeString(Pair.SourceChainName.ToString()));
				}
				FObjectBuilder Record;
				Record.AddString(TEXT("kind"), TEXT("map"));
				Record.Add(TEXT("op"), MakeName(OpName));
				Record.AddString(TEXT("type"), Type.Key);
				Record.Add(TEXT("mapping"), Pairs.Build());
				Records.Add(Record.Build());
			}
		}
	}
	Copy->MarkAsGarbage();
	return bOk;
}
} // namespace

bool HandleSuggest(FCall& Call, FObjectBuilder& Result)
{
	int64 Offset = 0;
	const FValue* MethodNames = nullptr;
	if (!Call.Args.OnlyFields({TEXT("offset"), TEXT("methods")}) || !Call.Args.Int(TEXT("offset"), Offset, false)
		|| !Call.Args.Array(TEXT("methods"), MethodNames, false))
	{
		return false;
	}
	if (Offset < 0)
	{
		return Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("offset must be >= 0."), TEXT("offset"));
	}
	static const TPair<const TCHAR*, ERetargetAutoAlignMethod> Known[] = {
		{TEXT("ChainToChain"), ERetargetAutoAlignMethod::ChainToChain}, {TEXT("MeshToMesh"), ERetargetAutoAlignMethod::MeshToMesh},
		{TEXT("LocalRotationAxes"), ERetargetAutoAlignMethod::LocalRotationAxes},
		{TEXT("GlobalRotationAxes"), ERetargetAutoAlignMethod::GlobalRotationAxes}};
	TArray<TPair<FString, ERetargetAutoAlignMethod>> Methods;
	if (MethodNames == nullptr)
	{
		Methods.Emplace(TEXT("ChainToChain"), ERetargetAutoAlignMethod::ChainToChain);
	}
	else
	{
		for (int32 Index = 0; Index < MethodNames->Elements.Num(); ++Index)
		{
			const FValue& Name = MethodNames->Elements[Index];
			const TPair<const TCHAR*, ERetargetAutoAlignMethod>* Match = Name.Kind == EAssetDocumentRuntimeValueKind::String
				? Algo::FindByPredicate(Known, [&Name](const TPair<const TCHAR*, ERetargetAutoAlignMethod>& Each) { return Name.StringValue == Each.Key; })
				: nullptr;
			if (Match == nullptr)
			{
				return Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("methods lists ChainToChain, MeshToMesh, LocalRotationAxes, "
					"GlobalRotationAxes."), FString::Printf(TEXT("methods/%d"), Index));
			}
			Methods.Emplace(Match->Key, Match->Value);
		}
	}
	TArray<FValue> Records;
	if (const UIKRigDefinition* Rig = Cast<UIKRigDefinition>(&Call.Asset))
	{
		if (!SuggestIKRig(*Rig, Call, Records))
		{
			return false;
		}
	}
	else if (const UIKRetargeter* Retargeter = Cast<UIKRetargeter>(&Call.Asset))
	{
		if (!SuggestRetargeter(*Retargeter, Methods, Call, Records))
		{
			return false;
		}
	}
	else
	{
		return Fail(Call, TEXT("IKRig.UnsupportedAsset"), TEXT("rig.suggest.v1 reads IK Rig and IK Retargeter assets."),
			TEXT("/HandleArguments/asset"));
	}
	AddRecordPage(Records, Offset, Result);
	return true;
}
} // namespace UE::AssetDocument::Graph::Rig

#undef LOCTEXT_NAMESPACE
