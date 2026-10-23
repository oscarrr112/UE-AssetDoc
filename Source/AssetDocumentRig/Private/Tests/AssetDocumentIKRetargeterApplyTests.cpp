// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// IK Retargeter apply and the automatic-setup suggestions (design §6, phases R2 / R3):
// UEAssetDoc.IKRig.RetargeterFidelity: §6.2 on the editor-built retargeter fixtures (default ops; every
//   op type, an auto-aligned pose, a renamed pose, a disabled op) and the corpus retargeters (MoverExamples'
//   RTG_Mannequin re-pointed at a copy of its IK Rig, as the editor's replace would, and MetaHuman's when enabled):
//   extract, Create a new retargeter from the document, extract equal and equal status; unchanged re-apply is a
//   no-op; reload stable.
// UEAssetDoc.IKRig.RetargeterFreeEdits: §6.3 retargeter edits made once through the controller calls
//   the IK Retarget editor's UI makes and once through a document, on two duplicates: equal state and status.
// UEAssetDoc.IKRig.RetargeterFromScratch: §6.4: an AI builds two IK Rigs and a retargeter through
//   documents and Inspect suggestions only (auto chains + auto IK, auto-align); typical authoring errors fail at their
//   document paths and change nothing; a failed Create leaves nothing behind.
// UEAssetDoc.IKRig.RetargeterUndo: the undo-history rules around a human edit, an AI apply and a
//   failed apply.
// UEAssetDoc.IKRig.RetargeterPoses: retarget poses written as quaternions and rotators read back
//   normalized, stable after reload, re-apply a no-op.
// UEAssetDoc.IKRig.Suggest: the suggestions equal what the editors' buttons make; reading them changes
//   nothing (no dirty package, no undo history).

#include "AssetDocumentIKRigTestSupport.h"

#include "AssetDocumentRigPrimitives.h"
#include "RetargetEditor/IKRetargeterPoseGenerator.h"
#include "Retargeter/IKRetargetChainMapping.h"
#include "Retargeter/IKRetargetOps.h"
#include "Runtime/AssetDocumentUndoHistory.h"
#include "ScopedTransaction.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Utils/AssetDocumentJsonKeys.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentIKRetargeterApplyTests
{
using namespace AssetDocumentIKRigTests;

const TCHAR* const MannyMesh = TEXT("/MoverExamples/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple");
const TCHAR* const MoverRig = TEXT("/MoverExamples/Characters/Mannequins/Rigs/IK_Mannequin.IK_Mannequin");
const TCHAR* const MoverRetargeter = TEXT("/MoverExamples/Characters/Mannequins/Rigs/RTG_Mannequin.RTG_Mannequin");
const TCHAR* const MetaHumanRetargeter = TEXT("/MetaHumanCharacter/Animation/Retargeting/RTG_MH_IKRig.RTG_MH_IKRig");
const TCHAR* const MissingRigWarning = TEXT("a dependent package /Game/Characters/Mannequins/Rigs/IK_Mannequin was not available");

bool PackageExists(const TCHAR* ObjectPath)
{
	// A plugin that is not enabled has no mount point (DoesPackageExist warns on a path outside every mount point).
	const FString Package = FPackageName::ObjectPathToPackageName(FString(ObjectPath));
	return FPackageName::IsValidLongPackageName(Package) && FPackageName::DoesPackageExist(Package);
}

// The target character: UE5 Manny (MoverExamples) when mounted, else the tutorial mannequin.
const TCHAR* TargetMesh()
{
	return PackageExists(MannyMesh) ? MannyMesh : TutorialMesh;
}

TSharedPtr<FJsonObject> ForTarget(const TSharedPtr<FJsonObject>& Document, const FTarget& Target, const TCHAR* Action)
{
	const TSharedPtr<FJsonObject> Copy = Parse(Serialize(Document));
	Copy->SetStringField(TEXT("Target"), Target.PackageName);
	Copy->SetStringField(TEXT("Action"), Action);
	return Copy;
}

TSharedPtr<FJsonObject> Document(const TCHAR* Class, const FTarget& Target, const TCHAR* Action, const FString& Body)
{
	return Parse(FString::Printf(TEXT(R"({"SchemaVersion": 1, "Class": "%s", "Action": "%s", "Target": "%s", "Body": %s})"),
		Class, Action, *Target.PackageName, *Body));
}

bool ApplyOk(FAutomationTestBase& Test, const FTarget& Target, const TSharedPtr<FJsonObject>& Doc, const FString& Stage,
	FAssetDocumentResult* OutResult = nullptr)
{
	const FAssetDocumentResult Result = Apply(Target, Doc);
	if (OutResult != nullptr)
	{
		*OutResult = Result;
	}
	if (!Test.TestTrue(*FString::Printf(TEXT("%s applies"), *Stage), Result.IsSuccess()))
	{
		Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *DiagnosticsText(Result)));
		return false;
	}
	return true;
}

FString StatusOf(const FAssetDocumentResult& Extracted)
{
	return Extracted.Compile.IsValid() ? Serialize(Extracted.Compile) : FString(TEXT("-"));
}

FString StatusName(const FAssetDocumentResult& Extracted)
{
	return Extracted.Compile.IsValid() ? Extracted.Compile->GetStringField(TEXT("Status")) : FString(TEXT("-"));
}

// Refusal at a document path that changes nothing: the asset extracts as before and its package is clean.
void ExpectRefused(FAutomationTestBase& Test, const FTarget& Target, const TSharedPtr<FJsonObject>& Doc, const TCHAR* Code,
	const FString& Path, const FString& Stage)
{
	const FString Before = Serialize(ExtractResult(Target.ObjectPath).Payload);
	const FAssetDocumentResult Result = Apply(Target, Doc);
	Test.TestFalse(*FString::Printf(TEXT("%s is refused"), *Stage), Result.IsSuccess());
	Test.TestTrue(*FString::Printf(TEXT("%s: %s (got %s)"), *Stage, Code, *DiagnosticsText(Result)), HasCode(Result, Code));
	Test.TestEqual(*FString::Printf(TEXT("%s: path"), *Stage), FirstPath(Result), Path);
	const FString After = Serialize(ExtractResult(Target.ObjectPath).Payload);
	if (!Test.TestEqual(*FString::Printf(TEXT("%s: the asset is unchanged"), *Stage), After, Before))
	{
		Test.AddInfo(FirstDifference(Before, After));
	}
	if (const UObject* Asset = Target.Get())
	{
		Test.TestFalse(*FString::Printf(TEXT("%s: package clean"), *Stage), Asset->GetOutermost()->IsDirty());
	}
}

int32 OpIndexOfType(const UIKRetargeter& Retargeter, const TCHAR* Type)
{
	const UScriptStruct* Struct = UE::AssetDocument::Graph::Rig::FindOpType(Type);
	const TArray<FInstancedStruct>& Ops = Retargeter.GetRetargetOps();
	for (int32 Index = 0; Index < Ops.Num(); ++Index)
	{
		if (Struct != nullptr && Ops[Index].GetScriptStruct() == Struct)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

// The editor-built fixture with every op type (design §6.1): the default ops of the factory, every other op type as the
// op stack's "Add New" adds it (child op types under Run IK Rig), one op disabled; an auto-aligned target pose (the
// toolbar's Auto Align All Bones) and a renamed source pose with a bone rotation and a root offset. Saved.
UIKRetargeter* MakeRichRetargeter(FAutomationTestBase& Test, const FTarget& Target, UIKRigDefinition* Source, UIKRigDefinition* TargetRig)
{
	UIKRetargeter* Retargeter = MakeRetargeter(Test, Target, Source, TargetRig);
	if (Retargeter == nullptr)
	{
		return nullptr;
	}
	const UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
	const FName RunIK = Controller->GetOpName(OpIndexOfType(*Retargeter, TEXT("RunIKRig")));
	for (const TCHAR* Type : {TEXT("PinBone"), TEXT("FilterBone"), TEXT("CopyBasePose"), TEXT("ScaleSource"), TEXT("AdditivePose"),
		TEXT("StretchChain"), TEXT("AlignPoleVector")})
	{
		Test.TestTrue(*FString::Printf(TEXT("add %s"), Type), Controller->AddRetargetOp(UE::AssetDocument::Graph::Rig::FindOpType(Type)) != INDEX_NONE);
	}
	for (const TCHAR* Type : {TEXT("FloorConstraint"), TEXT("SpeedPlanting"), TEXT("StrideWarping")})
	{
		Test.TestTrue(*FString::Printf(TEXT("add %s under %s"), Type, *RunIK.ToString()),
			Controller->AddRetargetOp(UE::AssetDocument::Graph::Rig::FindOpType(Type), RunIK) != INDEX_NONE);
	}
	Test.TestTrue(TEXT("disable Pin Bone"), Controller->SetRetargetOpEnabled(OpIndexOfType(*Retargeter, TEXT("PinBone")), false));
	const ERetargetSourceOrTarget SourceSide = ERetargetSourceOrTarget::Source;
	const ERetargetSourceOrTarget TargetSide = ERetargetSourceOrTarget::Target;
	Test.TestEqual(TEXT("target pose"), Controller->CreateRetargetPose(TEXT("Aligned"), TargetSide), FName(TEXT("Aligned")));
	Controller->AutoAlignAllBones(TargetSide, ERetargetAutoAlignMethod::ChainToChain);
	Test.TestEqual(TEXT("source pose"), Controller->CreateRetargetPose(TEXT("Temp"), SourceSide), FName(TEXT("Temp")));
	Test.TestTrue(TEXT("rename source pose"), Controller->RenameRetargetPose(TEXT("Temp"), TEXT("Source Pose"), SourceSide));
	{
		FScopedTransaction Transaction(NSLOCTEXT("IKRetargeterTests", "EditPose", "Edit pose"));
		Retargeter->Modify();
		Controller->SetRotationOffsetForRetargetPoseBone(TEXT("spine_01"), FRotator(5.0, 0.0, 0.0).Quaternion(), SourceSide);
		Controller->SetRootOffsetInRetargetPose(FVector(0.0, 0.0, 3.0), SourceSide);
	}
	Test.TestTrue(TEXT("source default pose current"), Controller->SetCurrentRetargetPose(UIKRetargeter::GetDefaultPoseName(), SourceSide));
	Test.TestTrue(TEXT("rich retargeter saved"), Save(*Retargeter));
	return Retargeter;
}

// MoverExamples' RTG_Mannequin re-pointed at a /Game copy of its IK Rig (it references /Game/Characters/Mannequins/
// Rigs/IK_Mannequin, which the plugin does not ship): the Details pick of both rigs and the replace dialog's "assign to
// all ops", as a user would repair it. Saved; null when MoverExamples is not mounted.
UIKRetargeter* MakeMoverRetargeter(FAutomationTestBase& Test, FFixtures& Fixtures)
{
	if (!PackageExists(MoverRig) || !PackageExists(MoverRetargeter))
	{
		return nullptr;
	}
	const FTarget& RigCopy = Fixtures.Add(TEXT("IK_MoverCopy"));
	UIKRigDefinition* Rig = Cast<UIKRigDefinition>(Duplicate(Test, LoadQuiet(MoverRig), RigCopy));
	const FTarget& Copy = Fixtures.Add(TEXT("RTG_MoverCopy"));
	UIKRetargeter* Retargeter = Rig != nullptr ? Cast<UIKRetargeter>(Duplicate(Test, LoadQuiet(MoverRetargeter), Copy)) : nullptr;
	if (Retargeter == nullptr)
	{
		return nullptr;
	}
	const UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
	for (const ERetargetSourceOrTarget Side : {ERetargetSourceOrTarget::Source, ERetargetSourceOrTarget::Target})
	{
		Retargeter->Modify();
		Controller->SetIKRig(Side, Rig);
		Controller->AssignIKRigToAllOps(Side, Rig);
	}
	Test.TestTrue(TEXT("re-pointed corpus retargeter saved"), Save(*Retargeter));
	return Retargeter;
}

const FTarget* FindTarget(const FFixtures& Fixtures, const UObject* Asset)
{
	for (const TUniquePtr<FTarget>& Target : Fixtures.Targets)
	{
		if (Asset != nullptr && Target->ObjectPath == Asset->GetPathName())
		{
			return Target.Get();
		}
	}
	return nullptr;
}

TSharedPtr<FJsonObject> ObjectAt(const TSharedPtr<FJsonObject>& Object, std::initializer_list<const TCHAR*> Keys)
{
	TSharedPtr<FJsonObject> Current = Object;
	for (const TCHAR* Key : Keys)
	{
		const TSharedPtr<FJsonObject>* Next = nullptr;
		if (!Current.IsValid() || !Current->TryGetObjectField(Key, Next))
		{
			return nullptr;
		}
		Current = *Next;
	}
	return Current;
}

// A JSON value with every object's keys sorted (Extract writes the canonical key order; Inspect additions keep theirs).
TSharedPtr<FJsonValue> Sorted(const TSharedPtr<FJsonValue>& Value)
{
	if (Value.IsValid() && Value->Type == EJson::Object)
	{
		TArray<FString> Keys;
		AssetDocumentServerJson::GetKeys(*Value->AsObject(), Keys);
		Keys.Sort();
		const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		for (const FString& Key : Keys)
		{
			Result->SetField(Key, Sorted(AssetDocumentServerJson::FindFieldRef(*Value->AsObject(), Key)));
		}
		return MakeShared<FJsonValueObject>(Result);
	}
	if (Value.IsValid() && Value->Type == EJson::Array)
	{
		TArray<TSharedPtr<FJsonValue>> Elements;
		for (const TSharedPtr<FJsonValue>& Element : Value->AsArray())
		{
			Elements.Add(Sorted(Element));
		}
		return MakeShared<FJsonValueArray>(Elements);
	}
	return Value;
}

FString Canonical(const TSharedPtr<FJsonObject>& Object)
{
	return Object.IsValid() ? Serialize(Sorted(MakeShared<FJsonValueObject>(Object))->AsObject()) : FString(TEXT("null"));
}

TSharedPtr<FJsonObject> OpNamed(const TSharedPtr<FJsonObject>& Body, const FString& Type)
{
	const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
	if (Body->TryGetArrayField(TEXT("Ops"), Ops))
	{
		for (const TSharedPtr<FJsonValue>& Op : *Ops)
		{
			if (Op->AsObject()->GetStringField(TEXT("Type")) == Type)
			{
				return Op->AsObject();
			}
		}
	}
	return nullptr;
}
} // namespace AssetDocumentIKRetargeterApplyTests

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentIKRetargeterFidelityTest, "UEAssetDoc.IKRig.RetargeterFidelity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentIKRetargeterFidelityTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentIKRetargeterApplyTests;
	AddExpectedMessagePlain(MissingRigWarning, ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	FFixtures Fixtures;
	TArray<TPair<FString, const FTarget*>> Sources;
	UIKRigDefinition* SourceRig = MakeRig(*this, Fixtures.Add(TEXT("IK_Source")), TutorialMesh, true);
	UIKRigDefinition* TargetRig = MakeRig(*this, Fixtures.Add(TEXT("IK_Target")), TargetMesh(), true);
	if (SourceRig == nullptr || TargetRig == nullptr)
	{
		return false;
	}
	const FTarget& Plain = Fixtures.Add(TEXT("RTG_Plain"));
	if (MakeRetargeter(*this, Plain, SourceRig, TargetRig) != nullptr)
	{
		Sources.Emplace(TEXT("fixture RTG_Plain (default ops)"), &Plain);
	}
	const FTarget& Rich = Fixtures.Add(TEXT("RTG_Rich"));
	if (MakeRichRetargeter(*this, Rich, SourceRig, TargetRig) != nullptr)
	{
		Sources.Emplace(FString::Printf(TEXT("fixture RTG_Rich (every op type, Tutorial -> %s)"), *FPackageName::ObjectPathToObjectName(FString(TargetMesh()))), &Rich);
	}
	if (const UIKRetargeter* Mover = MakeMoverRetargeter(*this, Fixtures))
	{
		Sources.Emplace(TEXT("MoverExamples:RTG_Mannequin (re-pointed at a copy of IK_Mannequin)"), FindTarget(Fixtures, Mover));
	}
	else
	{
		AddInfo(TEXT("SKIP MoverExamples:RTG_Mannequin: the plugin is not enabled in this project"));
	}
	if (PackageExists(MetaHumanRetargeter))
	{
		const FTarget& Copy = Fixtures.Add(TEXT("RTG_MetaHuman"));
		if (Duplicate(*this, LoadQuiet(MetaHumanRetargeter), Copy) != nullptr)
		{
			Sources.Emplace(TEXT("MetaHumanCharacter:RTG_MH_IKRig"), &Copy);
		}
	}
	else
	{
		AddInfo(TEXT("SKIP MetaHumanCharacter:RTG_MH_IKRig: the plugin is not enabled in this project"));
	}
	for (const TPair<FString, const FTarget*>& Source : Sources)
	{
		const FString& Name = Source.Key;
		const FAssetDocumentResult Original = ExtractResult(Source.Value->ObjectPath);
		if (!TestTrue(*FString::Printf(TEXT("%s extracts"), *Name), Original.IsSuccess() && Original.Payload.IsValid()))
		{
			AddInfo(DiagnosticsText(Original));
			continue;
		}
		const FTarget& Created = Fixtures.Add(TEXT("RTG_Recreated"));
		double Start = FPlatformTime::Seconds();
		FAssetDocumentResult Applied;
		if (!ApplyOk(*this, Created, ForTarget(Original.Payload, Created, TEXT("Create")), Name + TEXT(" Create"), &Applied))
		{
			AddInfo(SerializePretty(Original.Payload).Left(4000));
			continue;
		}
		const double CreateMs = Milliseconds(Start);
		const FAssetDocumentResult Recreated = ExtractResult(Created.ObjectPath);
		const FString Expected = WithoutTarget(Original.Payload);
		const FString Got = WithoutTarget(Recreated.Payload);
		if (!TestEqual(*FString::Printf(TEXT("%s: the created retargeter extracts as the source"), *Name), Got, Expected))
		{
			AddInfo(FirstDifference(Expected, Got));
		}
		TestEqual(*FString::Printf(TEXT("%s: equal status"), *Name), StatusOf(Recreated).Replace(*Created.PackageName, TEXT("")),
			StatusOf(Original).Replace(*Source.Value->PackageName, TEXT("")));
		Start = FPlatformTime::Seconds();
		const FAssetDocumentResult Again = Apply(Created, ForTarget(Recreated.Payload, Created, TEXT("Update")));
		const double ReapplyMs = Milliseconds(Start);
		TestTrue(*FString::Printf(TEXT("%s: unchanged re-apply succeeds"), *Name), Again.IsSuccess());
		TestFalse(*FString::Printf(TEXT("%s: unchanged re-apply is a no-op"), *Name), Again.bSavedAsset);
		TestNotNull(*FString::Printf(TEXT("%s: reload"), *Name), Reload(Created));
		TestEqual(*FString::Printf(TEXT("%s: equal after reload"), *Name), WithoutTarget(ExtractResult(Created.ObjectPath).Payload), Expected);
		AddInfo(FString::Printf(TEXT("PASS %s: ops %d, Create %.0f ms, unchanged re-apply %.0f ms, %d chars, status %s%s"), *Name,
			CountField(BodyOf(Original.Payload), TEXT("Ops")), CreateMs, ReapplyMs, Expected.Len(), *StatusName(Recreated),
			Applied.Diagnostics.IsEmpty() ? TEXT("") : *(TEXT("; apply warnings: ") + DiagnosticsText(Applied).Left(400))));
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentIKRetargeterFreeEditsTest, "UEAssetDoc.IKRig.RetargeterFreeEdits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentIKRetargeterFreeEditsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentIKRetargeterApplyTests;
	using UE::AssetDocument::Graph::Rig::FindOpType;
	FFixtures Fixtures;
	UIKRigDefinition* SourceRig = MakeRig(*this, Fixtures.Add(TEXT("IK_Source")), TutorialMesh, true);
	UIKRigDefinition* TargetRig = MakeRig(*this, Fixtures.Add(TEXT("IK_Target")), TargetMesh(), true);
	const FTarget& Editor = Fixtures.Add(TEXT("RTG_EditorSide"));
	const FTarget& Document = Fixtures.Add(TEXT("RTG_DocumentSide"));
	UIKRetargeter* Retargeter = SourceRig != nullptr && TargetRig != nullptr ? MakeRichRetargeter(*this, Editor, SourceRig, TargetRig) : nullptr;
	if (Retargeter == nullptr || !TestNotNull(TEXT("duplicate"), Duplicate(*this, Retargeter, Document)))
	{
		return false;
	}
	const UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
	const ERetargetSourceOrTarget SourceSide = ERetargetSourceOrTarget::Source;
	const ERetargetSourceOrTarget TargetSide = ERetargetSourceOrTarget::Target;
	// Op stack (design §7.2), as the op stack panel makes the edits.
	const int32 Added = Controller->AddRetargetOp(FindOpType(TEXT("PinBone")));
	TestEqual(TEXT("name the new op"), Controller->SetOpName(TEXT("Pin Hands"), Added), FName(TEXT("Pin Hands")));
	TestTrue(TEXT("remove Filter Bone"), Controller->RemoveRetargetOp(OpIndexOfType(*Retargeter, TEXT("FilterBone"))));
	TestTrue(TEXT("move Root Motion to the top"), Controller->MoveRetargetOpInStack(OpIndexOfType(*Retargeter, TEXT("RootMotion")), 0));
	TestTrue(TEXT("enable Pin Bone"), Controller->SetRetargetOpEnabled(OpIndexOfType(*Retargeter, TEXT("PinBone")), true));
	TestTrue(TEXT("disable Curve Remap"), Controller->SetRetargetOpEnabled(OpIndexOfType(*Retargeter, TEXT("CurveRemap")), false));
	const int32 RunIK = OpIndexOfType(*Retargeter, TEXT("RunIKRig"));
	const FName OldRunIKName = Controller->GetOpName(RunIK);
	TestEqual(TEXT("rename Run IK Rig"), Controller->SetOpName(TEXT("Legs IK"), RunIK), FName(TEXT("Legs IK")));
	// Details edits of op settings and of a chain's settings (struct viewer: Modify, write, re-initialize when asked).
	const int32 Pelvis = OpIndexOfType(*Retargeter, TEXT("PelvisMotion"));
	FIKRetargetOpBase* PelvisOp = Controller->GetRetargetOpByIndex(Pelvis);
	{
		FScopedTransaction Transaction(NSLOCTEXT("IKRetargeterTests", "OpSettings", "Edited property."));
		Retargeter->Modify();
		const UScriptStruct* Type = PelvisOp->GetSettingsType();
		void* Settings = PelvisOp->GetSettings();
		Type->FindPropertyByName(TEXT("RotationAlpha"))->ImportText_InContainer(TEXT("0.5"), Settings, nullptr, PPF_None);
		Type->FindPropertyByName(TEXT("ScaleVertical"))->ImportText_InContainer(TEXT("1.25"), Settings, nullptr, PPF_None);
	}
	const int32 FK = OpIndexOfType(*Retargeter, TEXT("FKChains"));
	const FName FKName = Controller->GetOpName(FK);
	FName Unmapped;
	FName RowChain;
	for (const FRetargetChainPair& Pair : Controller->GetChainMapping(FKName)->GetChainPairs())
	{
		if (!Pair.SourceChainName.IsNone())
		{
			Unmapped = Unmapped.IsNone() ? Pair.TargetChainName : Unmapped;
			RowChain = Unmapped != Pair.TargetChainName ? Pair.TargetChainName : RowChain;
		}
	}
	TestFalse(TEXT("the FK op maps chains"), Unmapped.IsNone() || RowChain.IsNone());
	TestTrue(TEXT("unmap a chain"), Controller->SetSourceChain(NAME_None, Unmapped, FKName));
	{
		FScopedTransaction Transaction(NSLOCTEXT("IKRetargeterTests", "ChainSettings", "Edited property."));
		Retargeter->Modify();
		FIKRetargetOpBase* FKOp = Controller->GetRetargetOpByIndex(FK);
		const UScriptStruct* Type = FKOp->GetSettingsType();
		const FArrayProperty* Rows = CastField<FArrayProperty>(Type->FindPropertyByName(TEXT("ChainsToRetarget")));
		FScriptArrayHelper Helper(Rows, Rows->ContainerPtrToValuePtr<void>(FKOp->GetSettings()));
		const UScriptStruct* RowType = CastField<FStructProperty>(Rows->Inner)->Struct;
		for (int32 Row = 0; Row < Helper.Num(); ++Row)
		{
			uint8* Value = Helper.GetRawPtr(Row);
			if (CastField<FNameProperty>(RowType->FindPropertyByName(TEXT("TargetChainName")))->GetPropertyValue_InContainer(Value) == RowChain)
			{
				RowType->FindPropertyByName(TEXT("RotationAlpha"))->ImportText_InContainer(TEXT("0.75"), Value, nullptr, PPF_None);
				RowType->FindPropertyByName(TEXT("TranslationMode"))->ImportText_InContainer(TEXT("GloballyScaled"), Value, nullptr, PPF_None);
			}
		}
	}
	// Retarget poses: a new target pose posed in edit mode, a renamed source pose, the auto-aligned pose removed.
	TestEqual(TEXT("new pose"), Controller->CreateRetargetPose(TEXT("Crouch"), TargetSide), FName(TEXT("Crouch")));
	{
		FScopedTransaction Transaction(NSLOCTEXT("IKRetargeterTests", "EditPose", "Edit pose"));
		Retargeter->Modify();
		Controller->SetRotationOffsetForRetargetPoseBone(Retargeter->GetIKRig(TargetSide)->GetSkeleton().BoneNames[3],
			FRotator(0.0, 0.0, 20.0).Quaternion(), TargetSide);
		Controller->SetRootOffsetInRetargetPose(FVector(0.0, 0.0, -12.5), TargetSide);
	}
	TestTrue(TEXT("rename source pose"), Controller->RenameRetargetPose(TEXT("Source Pose"), TEXT("Renamed Source Pose"), SourceSide));
	TestTrue(TEXT("remove aligned pose"), Controller->RemoveRetargetPose(TEXT("Aligned"), TargetSide));
	TestTrue(TEXT("source current"), Controller->SetCurrentRetargetPose(TEXT("Renamed Source Pose"), SourceSide));
	TestTrue(TEXT("editor side saved"), Save(*Retargeter));

	// The same state as a document (renames carried with RenamedFrom) on the other duplicate.
	const TSharedPtr<FJsonObject> EditorState = Extract(*this, Editor.ObjectPath, TEXT("editor side"));
	if (!EditorState.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Edit = ForTarget(EditorState, Document, TEXT("Update"));
	if (const TSharedPtr<FJsonObject> Legs = OpNamed(BodyOf(Edit), TEXT("RunIKRig")))
	{
		Legs->SetStringField(TEXT("RenamedFrom"), OldRunIKName.ToString());
	}
	if (const TSharedPtr<FJsonObject> Renamed = ObjectAt(BodyOf(Edit), {TEXT("Poses"), TEXT("Source"), TEXT("Poses"), TEXT("Renamed Source Pose")}))
	{
		Renamed->SetStringField(TEXT("RenamedFrom"), TEXT("Source Pose"));
	}
	const double Start = FPlatformTime::Seconds();
	FAssetDocumentResult Applied;
	if (!ApplyOk(*this, Document, Edit, TEXT("document side"), &Applied))
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("document side applied in %.0f ms; diagnostics: %s"), Milliseconds(Start), *DiagnosticsText(Applied).Left(600)));
	const FAssetDocumentResult EditorSide = ExtractResult(Editor.ObjectPath);
	const FAssetDocumentResult DocumentSide = ExtractResult(Document.ObjectPath);
	const FString Expected = WithoutTarget(EditorSide.Payload);
	const FString Got = WithoutTarget(DocumentSide.Payload);
	if (!TestEqual(TEXT("both duplicates extract equal"), Got, Expected))
	{
		AddInfo(FirstDifference(Expected, Got));
	}
	TestEqual(TEXT("equal status"), StatusOf(DocumentSide).Replace(*Document.PackageName, TEXT("")),
		StatusOf(EditorSide).Replace(*Editor.PackageName, TEXT("")));
	// The renamed op kept its children and settings (a rename, not remove + add).
	UIKRetargeter* DocumentRetargeter = Cast<UIKRetargeter>(Document.Get());
	if (TestNotNull(TEXT("document retargeter"), DocumentRetargeter))
	{
		const UIKRetargeterController* DocumentController = UIKRetargeterController::GetController(DocumentRetargeter);
		const int32 Legs = DocumentController->GetIndexOfOpByName(TEXT("Legs IK"));
		TestTrue(TEXT("renamed op keeps its children"), Legs != INDEX_NONE && DocumentController->GetChildOpIndices(Legs).Num() == 4);
	}
	FString DiffText;
	TestEqual(TEXT("Diff of the document is empty"), DiffCount(ForTarget(EditorState, Document, TEXT("Update")), DiffText), 0);
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentIKRetargeterFromScratchTest, "UEAssetDoc.IKRig.RetargeterFromScratch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentIKRetargeterFromScratchTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentIKRetargeterApplyTests;
	FFixtures Fixtures;
	// 1. Two IK Rigs: a document with only the mesh, then Inspect's suggestions (auto chains + auto IK) applied.
	const FTarget* Rigs[2] = {&Fixtures.Add(TEXT("IK_AISource")), &Fixtures.Add(TEXT("IK_AITarget"))};
	const TCHAR* Meshes[2] = {TutorialMesh, TargetMesh()};
	for (int32 Side = 0; Side < 2; ++Side)
	{
		const FString Stage = Side == 0 ? TEXT("source rig") : TEXT("target rig");
		if (!ApplyOk(*this, *Rigs[Side], Document(IKRigClass, *Rigs[Side], TEXT("Create"),
			FString::Printf(TEXT(R"({"PreviewMesh": "%s"})"), Meshes[Side])), Stage + TEXT(" Create")))
		{
			return false;
		}
		const FAssetDocumentResult Inspected = Inspect(Rigs[Side]->ObjectPath);
		const TSharedPtr<FJsonObject> Suggested = Inspected.Payload.IsValid() ? ObjectAt(Inspected.Payload, {TEXT("Suggestions"), TEXT("Body")}) : nullptr;
		if (!TestTrue(*FString::Printf(TEXT("%s: Inspect suggests a Body (%s)"), *Stage, *DiagnosticsText(Inspected)), Suggested.IsValid()))
		{
			return false;
		}
		AddInfo(FString::Printf(TEXT("%s suggestions: %s"), *Stage, *Serialize(ObjectAt(Inspected.Payload, {TEXT("Suggestions")})).Left(600)));
		const TSharedPtr<FJsonObject> Doc = Document(IKRigClass, *Rigs[Side], TEXT("Update"), Serialize(Suggested));
		if (!ApplyOk(*this, *Rigs[Side], Doc, Stage + TEXT(" suggestions")))
		{
			return false;
		}
		const FAssetDocumentResult Rig = ExtractResult(Rigs[Side]->ObjectPath);
		TestTrue(*FString::Printf(TEXT("%s: chains"), *Stage), CountField(BodyOf(Rig.Payload), TEXT("Chains")) >= 8);
		TestEqual(*FString::Printf(TEXT("%s: status"), *Stage), StatusName(Rig), FString(TEXT("UpToDate")));
	}

	// 2. The retargeter: an AI-written op stack, then the auto-aligned target pose from Inspect.
	const FTarget& Target = Fixtures.Add(TEXT("RTG_AI"));
	const FString Ops = TEXT(R"([
		{"Name": "Pelvis Motion", "Type": "PelvisMotion", "Settings": {"TranslationAlpha": 0.9}},
		{"Name": "FK Chains", "Type": "FKChains"},
		{"Name": "Run IK Rig", "Type": "RunIKRig", "Children": [
			{"Name": "IK Chains", "Type": "IKChains"},
			{"Name": "Stride Warping", "Type": "StrideWarping", "Enabled": false}]},
		{"Name": "Root Motion", "Type": "RootMotion"},
		{"Name": "Curves", "Type": "CurveRemap"}])");
	const FString RetargeterBody = FString::Printf(TEXT(R"({"Source": {"IKRig": "%s"}, "Target": {"IKRig": "%s"}, "Ops": %s})"),
		*Rigs[0]->ObjectPath, *Rigs[1]->ObjectPath, *Ops);
	FAssetDocumentResult Created;
	if (!ApplyOk(*this, Target, Document(RetargeterClass, Target, TEXT("Create"), RetargeterBody), TEXT("retargeter Create"), &Created))
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("retargeter Create diagnostics: %s"), *DiagnosticsText(Created)));
	const FAssetDocumentResult Inspected = Inspect(Target.ObjectPath);
	const TSharedPtr<FJsonObject> Aligned = Inspected.Payload.IsValid()
		? ObjectAt(Inspected.Payload, {TEXT("Suggestions"), TEXT("AutoAlign"), TEXT("Target"), TEXT("ChainToChain")}) : nullptr;
	if (!TestTrue(*FString::Printf(TEXT("Inspect suggests an aligned target pose (%s)"), *DiagnosticsText(Inspected)), Aligned.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("Inspect suggests chain mappings"), ObjectAt(Inspected.Payload, {TEXT("Suggestions"), TEXT("AutoMap"), TEXT("FK Chains"), TEXT("Fuzzy")}).IsValid());
	const TSharedPtr<FJsonObject> Full = Document(RetargeterClass, Target, TEXT("Update"), RetargeterBody);
	const TSharedPtr<FJsonObject> Poses = Parse(FString::Printf(TEXT(R"({"Target": {"Current": "Aligned", "Poses": {"Default Pose": {}, "Aligned": %s}}})"),
		*Serialize(Aligned)));
	BodyOf(Full)->SetObjectField(TEXT("Poses"), Poses);
	if (!ApplyOk(*this, Target, Full, TEXT("aligned pose")))
	{
		return false;
	}
	const FAssetDocumentResult Extracted = ExtractResult(Target.ObjectPath);
	AddInfo(FString::Printf(TEXT("retargeter status %s"), *StatusOf(Extracted)));
	TestNotEqual(TEXT("retargeter status has no errors"), StatusName(Extracted), FString(TEXT("Error")));
	TestNotEqual(TEXT("retargeter status is known (both meshes)"), StatusName(Extracted), FString(TEXT("Unknown")));
	FString DiffText;
	if (!TestEqual(TEXT("the retargeter has no differences from the document"), DiffCount(Full, DiffText), 0))
	{
		AddInfo(DiffText.Left(1500));
	}
	const FAssetDocumentResult Again = Apply(Target, Full);
	TestTrue(TEXT("re-apply succeeds"), Again.IsSuccess());
	TestFalse(TEXT("re-apply is a no-op"), Again.bSavedAsset);
	const TSharedPtr<FJsonObject> Fk = OpNamed(BodyOf(Extracted.Payload), TEXT("FKChains"));
	TestTrue(TEXT("FK Chains auto-mapped its chains"), Fk.IsValid() && CountField(Fk, TEXT("ChainMapping")) >= 8);

	// 3. Authoring errors at their document paths; the retargeter is unchanged.
	auto Edited = [&Full](const TFunction<void(const TSharedPtr<FJsonObject>&)>& Change)
	{
		const TSharedPtr<FJsonObject> Copy = Parse(Serialize(Full));
		Change(BodyOf(Copy));
		return Copy;
	};
	auto AddOp = [](const TSharedPtr<FJsonObject>& Body, const TCHAR* Json)
	{
		TArray<TSharedPtr<FJsonValue>> List = Body->GetArrayField(TEXT("Ops"));
		List.Add(MakeShared<FJsonValueObject>(Parse(Json)));
		Body->SetArrayField(TEXT("Ops"), List);
	};
	ExpectRefused(*this, Target, Edited([&AddOp](const TSharedPtr<FJsonObject>& Body) { AddOp(Body, TEXT(R"({"Name": "X", "Type": "NoSuchOp"})")); }),
		TEXT("IKRetargeter.UnknownOpType"), TEXT("/Body/Ops/5/Type"), TEXT("unknown op type"));
	ExpectRefused(*this, Target, Edited([&AddOp](const TSharedPtr<FJsonObject>& Body) { AddOp(Body, TEXT(R"({"Name": "Curves 2", "Type": "CurveRemap"})")); }),
		TEXT("IKRetargeter.SingletonOp"), TEXT("/Body/Ops/5/Type"), TEXT("second Curve Remap"));
	ExpectRefused(*this, Target, Edited([&AddOp](const TSharedPtr<FJsonObject>& Body) { AddOp(Body, TEXT(R"({"Name": "Goals", "Type": "IKChains"})")); }),
		TEXT("IKRetargeter.InvalidParentOp"), TEXT("/Body/Ops/5"), TEXT("child op at the top level"));
	ExpectRefused(*this, Target, Edited([](const TSharedPtr<FJsonObject>& Body)
	{
		Body->GetArrayField(TEXT("Ops"))[0]->AsObject()->GetObjectField(TEXT("Settings"))->SetNumberField(TEXT("NoSuchSetting"), 1.0);
	}), TEXT("IKRig.FieldNotEditable"), TEXT("/Body/Ops/0/Settings/NoSuchSetting"), TEXT("unknown op setting"));
	ExpectRefused(*this, Target, Edited([](const TSharedPtr<FJsonObject>& Body)
	{
		Body->GetArrayField(TEXT("Ops"))[1]->AsObject()->SetObjectField(TEXT("ChainMapping"), Parse(TEXT(R"({"NoSuchChain": null})")));
	}), TEXT("IKRetargeter.UnknownTargetChain"), TEXT("/Body/Ops/1/ChainMapping/NoSuchChain"), TEXT("unknown target chain"));
	ExpectRefused(*this, Target, Edited([](const TSharedPtr<FJsonObject>& Body)
	{
		ObjectAt(Body, {TEXT("Poses"), TEXT("Target"), TEXT("Poses"), TEXT("Aligned"), TEXT("Bones")})->SetObjectField(TEXT("no_such_bone"),
			Parse(TEXT(R"({"Yaw": 10})")));
	}), TEXT("IKRetargeter.BoneNotFound"), TEXT("/Body/Poses/Target/Poses/Aligned/Bones/no_such_bone"), TEXT("pose bone not in the skeleton"));
	ExpectRefused(*this, Target, Edited([](const TSharedPtr<FJsonObject>& Body)
	{
		ObjectAt(Body, {TEXT("Poses"), TEXT("Target"), TEXT("Poses")})->RemoveField(TEXT("Default Pose"));
	}), TEXT("IKRetargeter.DefaultPoseRequired"), TEXT("/Body/Poses/Target/Poses"), TEXT("default pose removed"));
	ExpectRefused(*this, Target, Edited([](const TSharedPtr<FJsonObject>& Body)
	{
		Body->GetObjectField(TEXT("Target"))->SetStringField(TEXT("IKRig"), TEXT("/Engine/BasicShapes/Cube.Cube"));
	}), TEXT("IKRetargeter.UnknownIKRig"), TEXT("/Body/Target/IKRig"), TEXT("not an IK Rig"));

	// 4. A failed Create leaves nothing behind (an op is added before the pose refuses an unknown bone).
	const FTarget& Failed = Fixtures.Add(TEXT("RTG_FailedCreate"));
	const TSharedPtr<FJsonObject> Bad = ForTarget(Full, Failed, TEXT("Create"));
	ObjectAt(BodyOf(Bad), {TEXT("Poses"), TEXT("Target"), TEXT("Poses"), TEXT("Aligned"), TEXT("Bones")})->SetObjectField(TEXT("nope"), Parse(TEXT(R"({"Yaw": 1})")));
	const FAssetDocumentResult FailedCreate = Apply(Failed, Bad);
	TestFalse(TEXT("failing Create fails"), FailedCreate.IsSuccess());
	TestTrue(*FString::Printf(TEXT("failing Create: BoneNotFound (%s)"), *DiagnosticsText(FailedCreate)), HasCode(FailedCreate, TEXT("IKRetargeter.BoneNotFound")));
	TestNull(TEXT("failing Create leaves no object"), Failed.Get());
	TestFalse(TEXT("failing Create leaves no file"), IFileManager::Get().FileExists(*Failed.PackageFile));
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentIKRetargeterUndoTest, "UEAssetDoc.IKRig.RetargeterUndo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentIKRetargeterUndoTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentIKRetargeterApplyTests;
	if (GEditor == nullptr || GEditor->Trans == nullptr)
	{
		return false;
	}
	FFixtures Fixtures;
	UIKRigDefinition* Rig = MakeRig(*this, Fixtures.Add(TEXT("IK_Undo")), TutorialMesh, true);
	const FTarget& Target = Fixtures.Add(TEXT("RTG_Undo"));
	UIKRetargeter* Retargeter = Rig != nullptr ? MakeRetargeter(*this, Target, Rig, Rig) : nullptr;
	if (Retargeter == nullptr)
	{
		return false;
	}
	ReleaseUndoHistory();
	auto UndoRedoAll = [this, &Target](const TCHAR* Stage)
	{
		int32 Undone = 0;
		while (Undone < 16 && GEditor->Trans->CanUndo() && GEditor->UndoTransaction())
		{
			++Undone;
			TestTrue(*FString::Printf(TEXT("%s: whole after undo %d"), Stage, Undone), ExtractResult(Target.ObjectPath).IsSuccess());
		}
		int32 Redone = 0;
		while (Redone < Undone && GEditor->Trans->CanRedo() && GEditor->RedoTransaction())
		{
			++Redone;
		}
		TestEqual(*FString::Printf(TEXT("%s: every undone step redoes"), Stage), Redone, Undone);
	};
	// 1. A human edit in the editor, saved; the AI edits: history referencing the retargeter is cleared, the human edit kept.
	const UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
	TestTrue(TEXT("human edit"), Controller->SetRetargetOpEnabled(OpIndexOfType(*Retargeter, TEXT("RootMotion")), false));
	TestTrue(TEXT("human save"), UEditorLoadingAndSavingUtils::SavePackages({Retargeter->GetOutermost()}, false));
	TestTrue(TEXT("the human edit is in the undo history"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	const TSharedPtr<FJsonObject> HumanState = Extract(*this, Target.ObjectPath, TEXT("after human edit"));
	const TSharedPtr<FJsonObject> AiEdit = ForTarget(HumanState, Target, TEXT("Update"));
	TArray<TSharedPtr<FJsonValue>> Ops = BodyOf(AiEdit)->GetArrayField(TEXT("Ops"));
	Ops.Add(MakeShared<FJsonValueObject>(Parse(TEXT(R"({"Name": "Pin", "Type": "PinBone"})"))));
	BodyOf(AiEdit)->SetArrayField(TEXT("Ops"), Ops);
	BodyOf(AiEdit)->SetObjectField(TEXT("Poses"), Parse(TEXT(R"({"Target": {"Current": "Bent", "Poses": {"Default Pose": {},
		"Bent": {"RootOffset": 2, "Bones": {"spine_02": {"Pitch": 10}}}}}})")));
	if (!ApplyOk(*this, Target, AiEdit, TEXT("AI edit")))
	{
		return false;
	}
	TestFalse(TEXT("after AI edit: no undo history references the retargeter"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	const TSharedPtr<FJsonObject> AfterEditDocument = Extract(*this, Target.ObjectPath, TEXT("after AI edit"));
	const FString AfterEdit = Serialize(AfterEditDocument);
	const TSharedPtr<FJsonObject> RootMotion = AfterEditDocument.IsValid() ? OpNamed(BodyOf(AfterEditDocument), TEXT("RootMotion")) : nullptr;
	TestTrue(TEXT("human edit kept"), RootMotion.IsValid() && RootMotion->HasField(TEXT("Enabled")) && !RootMotion->GetBoolField(TEXT("Enabled")));
	UndoRedoAll(TEXT("after AI edit"));
	TestEqual(TEXT("after AI edit + undo/redo"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after undo/redo"))), AfterEdit);

	// 2. The human edits again; an AI apply fails after some edits and rolls back to the human state; undo stays safe.
	ReleaseUndoHistory();
	// Re-resolved by path: an apply may end with a full GC (objects are not held across applies by raw pointer).
	UIKRetargeter* Edited = Cast<UIKRetargeter>(Target.Get());
	if (!TestNotNull(TEXT("retargeter after the AI edit"), Edited))
	{
		return false;
	}
	TestTrue(TEXT("second human edit"), UIKRetargeterController::GetController(Edited)->SetRetargetOpEnabled(OpIndexOfType(*Edited, TEXT("RootMotion")), true));
	const FString SecondHuman = Serialize(Extract(*this, Target.ObjectPath, TEXT("after second human edit")));
	const TSharedPtr<FJsonObject> Failing = ForTarget(Parse(SecondHuman), Target, TEXT("Update"));
	BodyOf(Failing)->GetArrayField(TEXT("Ops"))[0]->AsObject()->SetBoolField(TEXT("Enabled"), false);
	ObjectAt(BodyOf(Failing), {TEXT("Poses"), TEXT("Target"), TEXT("Poses"), TEXT("Bent"), TEXT("Bones")})->SetObjectField(TEXT("nope"), Parse(TEXT(R"({"Yaw": 1})")));
	const FAssetDocumentResult Failed = Apply(Target, Failing);
	TestFalse(TEXT("failing apply fails"), Failed.IsSuccess());
	TestEqual(TEXT("after rollback: the human state"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after rollback"))), SecondHuman);
	UndoRedoAll(TEXT("after rollback"));
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentIKRetargeterPosesTest, "UEAssetDoc.IKRig.RetargeterPoses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentIKRetargeterPosesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentIKRetargeterApplyTests;
	FFixtures Fixtures;
	UIKRigDefinition* Rig = MakeRig(*this, Fixtures.Add(TEXT("IK_Poses")), TutorialMesh, true);
	const FTarget& Target = Fixtures.Add(TEXT("RTG_Poses"));
	if (Rig == nullptr || MakeRetargeter(*this, Target, Rig, Rig) == nullptr)
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Doc = ForTarget(Extract(*this, Target.ObjectPath, TEXT("before")), Target, TEXT("Update"));
	// A rotator, an approximate quaternion (normalized on write) and an exact one.
	BodyOf(Doc)->SetObjectField(TEXT("Poses"), Parse(TEXT(R"({"Target": {"Current": "Arms Down", "Poses": {"Default Pose": {},
		"Arms Down": {"RootOffset": -4.5, "Bones": {
			"upperarm_l": {"Roll": -45},
			"upperarm_r": {"X": 0, "Y": 0, "Z": 0.38, "W": 0.92},
			"lowerarm_l": {"X": 0.1, "Y": 0.2, "Z": 0.3, "W": 0.9273618495495703}}}}}})")));
	if (!ApplyOk(*this, Target, Doc, TEXT("poses")))
	{
		return false;
	}
	UIKRetargeter* Retargeter = Cast<UIKRetargeter>(Target.Get());
	const FIKRetargetPose* Pose = Retargeter != nullptr
		? UIKRetargeterController::GetController(Retargeter)->GetRetargetPoses(ERetargetSourceOrTarget::Target).Find(TEXT("Arms Down")) : nullptr;
	if (TestNotNull(TEXT("pose"), Pose))
	{
		TestTrue(TEXT("rotator"), Pose->GetDeltaRotationForBone(TEXT("upperarm_l")).Equals(FRotator(0.0, 0.0, -45.0).Quaternion(), 1.0e-12));
		TestTrue(TEXT("approximate quaternion normalized"), Pose->GetDeltaRotationForBone(TEXT("upperarm_r")).IsNormalized()
			&& Pose->GetDeltaRotationForBone(TEXT("upperarm_r")).Equals(FQuat(0.0, 0.0, 0.38, 0.92).GetNormalized(), 1.0e-12));
		TestEqual(TEXT("exact quaternion kept"), Pose->GetDeltaRotationForBone(TEXT("lowerarm_l")).W, 0.9273618495495703);
		TestEqual(TEXT("root offset"), Pose->GetRootTranslationDelta(), FVector(0.0, 0.0, -4.5));
	}
	const FString Applied = Serialize(Extract(*this, Target.ObjectPath, TEXT("after apply")));
	TestNotNull(TEXT("reload"), Reload(Target));
	const TSharedPtr<FJsonObject> Reloaded = Extract(*this, Target.ObjectPath, TEXT("after reload"));
	TestEqual(TEXT("poses stable after reload"), Serialize(Reloaded), Applied);
	const FAssetDocumentResult Again = Apply(Target, ForTarget(Reloaded, Target, TEXT("Update")));
	TestTrue(TEXT("re-apply of the extract succeeds"), Again.IsSuccess());
	TestFalse(TEXT("re-apply of the extract is a no-op"), Again.bSavedAsset);
	// Removing every bone of a pose and the pose itself.
	const TSharedPtr<FJsonObject> Reset = ForTarget(Reloaded, Target, TEXT("Update"));
	BodyOf(Reset)->SetObjectField(TEXT("Poses"), Parse(TEXT(R"({"Target": {"Poses": {"Default Pose": {}}}})")));
	if (ApplyOk(*this, Target, Reset, TEXT("remove pose")))
	{
		const TSharedPtr<FJsonObject> Target2 = ObjectAt(BodyOf(Extract(*this, Target.ObjectPath, TEXT("after remove"))), {TEXT("Poses"), TEXT("Target")});
		TestTrue(TEXT("only the default pose, current"), Target2.IsValid() && CountField(Target2, TEXT("Poses")) == 1
			&& Target2->GetStringField(TEXT("Current")) == TEXT("Default Pose"));
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

// A Speed Planting op's spring setting written by a document is what the asset holds after a save and reload. Where the
// op's load recomputes Spring Strength from its stored stiffness, the edit stores the stiffness that loads back as the
// written strength; where the op stores Stiffness itself, that is the setting.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentIKRetargeterSpeedPlantingReloadTest, "UEAssetDoc.IKRig.RetargeterSpeedPlantingReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentIKRetargeterSpeedPlantingReloadTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentIKRetargeterApplyTests;
	FFixtures Fixtures;
	UIKRigDefinition* Rig = MakeRig(*this, Fixtures.Add(TEXT("IK_SpeedPlanting")), TutorialMesh, true);
	const FTarget& Target = Fixtures.Add(TEXT("RTG_SpeedPlanting"));
	UIKRetargeter* Retargeter = Rig != nullptr ? MakeRetargeter(*this, Target, Rig, Rig) : nullptr;
	if (Retargeter == nullptr)
	{
		return false;
	}
	// The op as the op stack's "Add New" adds it under Run IK Rig.
	{
		const UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
		const FName RunIK = Controller->GetOpName(OpIndexOfType(*Retargeter, TEXT("RunIKRig")));
		TestTrue(TEXT("add Speed Planting"),
			Controller->AddRetargetOp(UE::AssetDocument::Graph::Rig::FindOpType(TEXT("SpeedPlanting")), RunIK) != INDEX_NONE);
		TestTrue(TEXT("fixture saved"), Save(*Retargeter));
	}
	const UScriptStruct* Settings = FindObject<UScriptStruct>(nullptr, TEXT("/Script/IKRig.IKRetargetSpeedPlantingOpSettings"));
	const FProperty* Strength = Settings != nullptr ? Settings->FindPropertyByName(TEXT("SpringStrength")) : nullptr;
	const TCHAR* const Field = Strength != nullptr ? TEXT("SpringStrength") : TEXT("Stiffness");
	const double Written = Strength != nullptr ? 4.4 : 321.5;

	const TSharedPtr<FJsonObject> Doc = ForTarget(Extract(*this, Target.ObjectPath, TEXT("before")), Target, TEXT("Update"));
	// The op entry, at the top of the stack or among an op's Children.
	const TFunction<TSharedPtr<FJsonObject>(const TSharedPtr<FJsonObject>&, const TCHAR*)> FindOp =
		[&FindOp](const TSharedPtr<FJsonObject>& Parent, const TCHAR* ListField) -> TSharedPtr<FJsonObject>
	{
		const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
		if (Parent.IsValid() && Parent->TryGetArrayField(ListField, List))
		{
			for (const TSharedPtr<FJsonValue>& Each : *List)
			{
				const TSharedPtr<FJsonObject> Object = Each->AsObject();
				if (Object->GetStringField(TEXT("Type")) == TEXT("SpeedPlanting"))
				{
					return Object;
				}
				if (const TSharedPtr<FJsonObject> Child = FindOp(Object, TEXT("Children")))
				{
					return Child;
				}
			}
		}
		return nullptr;
	};
	const TSharedPtr<FJsonObject> Op = FindOp(BodyOf(Doc), TEXT("Ops"));
	if (!TestTrue(TEXT("the document lists the Speed Planting op"), Op.IsValid()))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> OpSettings = Op->HasField(TEXT("Settings")) ? Op->GetObjectField(TEXT("Settings")) : MakeShared<FJsonObject>();
	OpSettings->SetNumberField(Field, Written);
	Op->SetObjectField(TEXT("Settings"), OpSettings);
	if (!ApplyOk(*this, Target, Doc, FString::Printf(TEXT("%s edit"), Field)))
	{
		return false;
	}
	const auto ReadBack = [this, &Target, Field, &FindOp](const TCHAR* Stage)
	{
		const TSharedPtr<FJsonObject> Read = FindOp(BodyOf(Extract(*this, Target.ObjectPath, Stage)), TEXT("Ops"));
		const TSharedPtr<FJsonObject>* ReadSettings = nullptr;
		double Value = -1.0;
		if (Read.IsValid() && Read->TryGetObjectField(TEXT("Settings"), ReadSettings))
		{
			(*ReadSettings)->TryGetNumberField(Field, Value);
		}
		return Value;
	};
	TestEqual(FString::Printf(TEXT("%s after apply"), Field), ReadBack(TEXT("after apply")), Written);
	TestNotNull(TEXT("reload"), Reload(Target));
	TestEqual(FString::Printf(TEXT("%s after save and reload"), Field), ReadBack(TEXT("after reload")), Written);
	const FAssetDocumentResult Again = Apply(Target, Doc);
	TestTrue(*FString::Printf(TEXT("re-apply after reload succeeds (%s)"), *DiagnosticsText(Again)), Again.IsSuccess());
	TestFalse(TEXT("re-apply after reload is a no-op"), Again.bSavedAsset);
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentIKRigSuggestTest, "UEAssetDoc.IKRig.Suggest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentIKRigSuggestTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentIKRetargeterApplyTests;
	FFixtures Fixtures;
	const FTarget& Plain = Fixtures.Add(TEXT("IK_SuggestPlain"));
	const FTarget& Auto = Fixtures.Add(TEXT("IK_SuggestAuto"));
	if (MakeRig(*this, Plain, TutorialMesh, false) == nullptr || MakeRig(*this, Auto, TutorialMesh, true) == nullptr)
	{
		return false;
	}
	ReleaseUndoHistory();
	// IK Rig: the suggestion is what the toolbar's Auto Characterize + Auto FBIK make.
	const FReadSideEffects SideEffects;
	const int32 UndoBefore = GEditor->Trans->GetQueueLength();
	double Start = FPlatformTime::Seconds();
	const FAssetDocumentResult Inspected = Inspect(Plain.ObjectPath);
	const double RigMs = Milliseconds(Start);
	const TSharedPtr<FJsonObject> Suggested = Inspected.Payload.IsValid() ? ObjectAt(Inspected.Payload, {TEXT("Suggestions"), TEXT("Body")}) : nullptr;
	if (TestTrue(*FString::Printf(TEXT("rig suggestions (%s)"), *DiagnosticsText(Inspected)), Suggested.IsValid()))
	{
		const FString Expected = Canonical(BodyOf(ExtractResult(Auto.ObjectPath).Payload));
		if (!TestEqual(TEXT("suggested Body = Auto Characterize + Auto FBIK"), Canonical(Suggested), Expected))
		{
			AddInfo(FirstDifference(Expected, Canonical(Suggested)));
		}
		const TSharedPtr<FJsonObject> Characterization = ObjectAt(Inspected.Payload, {TEXT("Suggestions"), TEXT("Characterization")});
		TestTrue(TEXT("characterization used a template"), Characterization.IsValid() && Characterization->GetBoolField(TEXT("UsedTemplate")));
		AddInfo(FString::Printf(TEXT("rig Inspect with suggestions %.0f ms: %s"), RigMs, *Serialize(Characterization)));
	}
	TestEqual(TEXT("the plain rig is unchanged"), CountField(BodyOf(ExtractResult(Plain.ObjectPath).Payload), TEXT("Chains")), 0);

	// IK Retargeter: Auto Align All Bones as the toolbar runs it, Auto Map per op.
	UIKRigDefinition* SourceRig = Cast<UIKRigDefinition>(Auto.Get());
	UIKRigDefinition* TargetRig = MakeRig(*this, Fixtures.Add(TEXT("IK_SuggestTarget")), TargetMesh(), true);
	const FTarget& Retargeter = Fixtures.Add(TEXT("RTG_Suggest"));
	const FTarget& Aligned = Fixtures.Add(TEXT("RTG_SuggestAligned"));
	UIKRetargeter* Original = TargetRig != nullptr ? MakeRetargeter(*this, Retargeter, SourceRig, TargetRig) : nullptr;
	UIKRetargeter* Copy = Original != nullptr ? Cast<UIKRetargeter>(Duplicate(*this, Original, Aligned)) : nullptr;
	if (Copy == nullptr)
	{
		return false;
	}
	ReleaseUndoHistory();
	const FReadSideEffects RetargeterSideEffects;
	const FString Before = Serialize(ExtractResult(Retargeter.ObjectPath).Payload);
	Start = FPlatformTime::Seconds();
	const FAssetDocumentResult RetargeterInspected = Inspect(Retargeter.ObjectPath);
	const double RetargeterMs = Milliseconds(Start);
	const TSharedPtr<FJsonObject> Pose = RetargeterInspected.Payload.IsValid()
		? ObjectAt(RetargeterInspected.Payload, {TEXT("Suggestions"), TEXT("AutoAlign"), TEXT("Target"), TEXT("ChainToChain")}) : nullptr;
	TestEqual(TEXT("Inspect leaves no undo history"), GEditor->Trans->GetQueueLength(), 0);
	if (TestTrue(*FString::Printf(TEXT("retargeter suggestions (%s)"), *DiagnosticsText(RetargeterInspected)), Pose.IsValid()))
	{
		const UIKRetargeterController* Controller = UIKRetargeterController::GetController(Cast<UIKRetargeter>(Aligned.Get()));
		Controller->AutoAlignAllBones(ERetargetSourceOrTarget::Target, ERetargetAutoAlignMethod::ChainToChain);
		const TSharedPtr<FJsonObject> Expected = ObjectAt(BodyOf(ExtractResult(Aligned.ObjectPath).Payload),
			{TEXT("Poses"), TEXT("Target"), TEXT("Poses"), TEXT("Default Pose")});
		if (!TestEqual(TEXT("suggested pose = Auto Align All Bones"), Canonical(Pose), Canonical(Expected)))
		{
			const TSharedPtr<FJsonObject> Want = Expected.IsValid() ? Expected->GetObjectField(TEXT("Bones")) : nullptr;
			const TSharedPtr<FJsonObject> Got = Pose->GetObjectField(TEXT("Bones"));
			FString Report;
			for (const auto& Bone : Got->Values)
			{
				const TSharedPtr<FJsonObject>* Other = nullptr;
				if (!Want.IsValid() || !Want->TryGetObjectField(*Bone.Key, Other))
				{
					Report += FString::Printf(TEXT(" +%s"), *Bone.Key);
					continue;
				}
				double Worst = 0.0;
				for (const TCHAR* Key : {TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("W")})
				{
					Worst = FMath::Max(Worst, FMath::Abs(Bone.Value->AsObject()->GetNumberField(Key) - (*Other)->GetNumberField(Key)));
				}
				Report += Worst > 1.0e-9 ? FString::Printf(TEXT(" %s:%.2g"), *Bone.Key, Worst) : FString();
			}
			for (const auto& Bone : Want.IsValid() ? Want->Values : TMap<AssetDocumentServerJson::FKey, TSharedPtr<FJsonValue>>())
			{
				Report += Got->HasField(*Bone.Key) ? FString() : FString::Printf(TEXT(" -%s"), *Bone.Key);
			}
			AddInfo(FString::Printf(TEXT("aligned pose differences (suggested vs controller):%s; root %s vs %s"), *Report,
				*Serialize(Pose).Right(60), *Serialize(Expected).Right(60)));
		}
		TestTrue(TEXT("auto map suggestions"), ObjectAt(RetargeterInspected.Payload, {TEXT("Suggestions"), TEXT("AutoMap")}).IsValid());
		AddInfo(FString::Printf(TEXT("retargeter Inspect with suggestions %.0f ms; %d aligned bones"), RetargeterMs, CountField(Pose, TEXT("Bones"))));
	}
	TestEqual(TEXT("the retargeter is unchanged"), Serialize(ExtractResult(Retargeter.ObjectPath).Payload), Before);
	TArray<FString> Problems;
	SideEffects.Check(Problems);
	for (const FString& Problem : Problems)
	{
		if (!Problem.Contains(Aligned.PackageName))
		{
			AddError(Problem);
		}
	}
	TestTrue(TEXT("the IK Rig Inspect left no undo history"), UndoBefore == 0 || GEditor->Trans->GetQueueLength() <= UndoBefore);
	return true;
}

#endif
