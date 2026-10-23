// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Control Rig apply (docs/architecture/asset-document-controlrig-design.md §7.2: rig apply without graphs).
// UEAssetDoc.ControlRig.Fidelity: §6.2 on a hierarchy-only fixture, CR_AllKinds, #3 (Root module) and
//   #5 (Constraint, Hinge, Wheel, Finger): Create a new asset from the extract without Graphs / Functions, the created
//   asset extracts as the source minus those regions; the unchanged re-apply is a no-op; reload stable; timings.
// UEAssetDoc.ControlRig.FreeEdits: §6.3 hierarchy and variable edits made once through the controller
//   calls the editor's panels make and once through a document, on two duplicates: equal state and compile status.
// UEAssetDoc.ControlRig.Rollback: a refused op and a failed compile roll back: the rig extracts as before,
//   its package is clean.
// UEAssetDoc.ControlRig.UndoHistory: the undo-history rules around a human edit, an AI apply and a failed
//   apply.
// UEAssetDoc.ControlRig.OpenEditorApply: the Forwards Solve a Create adds equals what the editor's first
//   open adds. An apply (a null and a Set Transform node linked to the Forwards Solve) to a rig open in the Control Rig
//   editor, unsaved after the open added its Forwards Solve, closes the editor and reopens it on the applied rig
//   (ControlRig.EditorReopened): the new editor's graph and its debugged rig show the node and the null. A failed apply
//   with the editor open rolls back and the editor shows the rolled-back rig.
//   A human hierarchy edit saves, undoes and redoes.

#include "AssetDocumentIKRigTestSupport.h"
#include "AssetDocumentControlRig.h"
#include "AssetDocumentRigOpenEditorTestSupport.h"

#include "ControlRig.h"
#include "ControlRigBlueprintLegacy.h"
#include "EdGraph/EdGraph.h"
#include "Editor/RigVMEditor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "RigVMModel/Nodes/RigVMFunctionReferenceNode.h"
#include "RigVMModel/Nodes/RigVMLibraryNode.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMFunctionLibrary.h"
#include "RigVMModel/RigVMGraph.h"
#include "Runtime/AssetDocumentUndoHistory.h"
#include "Misc/ScopeExit.h"
#include "ScopedTransaction.h"
#include "Subsystems/AssetEditorSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

// Fixtures of the read tests (AssetDocumentControlRigTests.cpp).
namespace AssetDocumentControlRigTests
{
UControlRigBlueprint* MakeControlRig(FAutomationTestBase& Test, const AssetDocumentIKRigTests::FTarget& Target, const TCHAR* MeshPath, bool bModular);
UControlRigBlueprint* MakeAllKinds(FAutomationTestBase& Test, const AssetDocumentIKRigTests::FTarget& Target);
} // namespace AssetDocumentControlRigTests

namespace AssetDocumentControlRigApplyTests
{
using namespace AssetDocumentIKRigTests;

const TCHAR* const ControlRigClassPath = TEXT("/Script/ControlRigDeveloper.ControlRigBlueprint");

TSharedPtr<FJsonObject> ForTarget(const TSharedPtr<FJsonObject>& Document, const FTarget& Target, const TCHAR* Action)
{
	const TSharedPtr<FJsonObject> Copy = Parse(Serialize(Document));
	Copy->SetStringField(TEXT("Target"), Target.PackageName);
	Copy->SetStringField(TEXT("Action"), Action);
	return Copy;
}

// The document without the regions the rig apply does not author (Graphs, GraphOrder, Functions).
TSharedPtr<FJsonObject> WithoutGraphs(const TSharedPtr<FJsonObject>& Document)
{
	const TSharedPtr<FJsonObject> Copy = Parse(Serialize(Document));
	BodyOf(Copy)->RemoveField(TEXT("Graphs"));
	BodyOf(Copy)->RemoveField(TEXT("GraphOrder"));
	BodyOf(Copy)->RemoveField(TEXT("Functions"));
	return Copy;
}

FString Comparable(const TSharedPtr<FJsonObject>& Document)
{
	return Document.IsValid() ? WithoutTarget(WithoutGraphs(Document)) : FString();
}

bool ApplyOk(FAutomationTestBase& Test, const FTarget& Target, const TSharedPtr<FJsonObject>& Document, const FString& Stage,
	FAssetDocumentResult* OutResult = nullptr)
{
	const FAssetDocumentResult Result = Apply(Target, Document);
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

// Status, messages and VM facts of the last compile, with the package path left out.
FString StatusOf(const FAssetDocumentResult& Extracted, const FString& PackageName)
{
	if (!Extracted.Compile.IsValid())
	{
		return TEXT("-");
	}
	return Serialize(Extracted.Compile).Replace(*PackageName, TEXT(""));
}

FString StatusName(const FAssetDocumentResult& Extracted)
{
	return Extracted.Compile.IsValid() ? Extracted.Compile->GetStringField(TEXT("Status")) : FString(TEXT("-"));
}

// Refusal at a document path that changes nothing: the rig extracts as before and its package is clean.
void ExpectRefused(FAutomationTestBase& Test, const FTarget& Target, const TSharedPtr<FJsonObject>& Document, const TCHAR* Code,
	const FString& Stage)
{
	const FString Before = Serialize(ExtractResult(Target.ObjectPath).Payload);
	const FAssetDocumentResult Result = Apply(Target, Document);
	Test.TestFalse(*FString::Printf(TEXT("%s is refused"), *Stage), Result.IsSuccess());
	Test.TestTrue(*FString::Printf(TEXT("%s: %s (got %s)"), *Stage, Code, *DiagnosticsText(Result)), HasCode(Result, Code));
	Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *DiagnosticsText(Result)));
	const FString After = Serialize(ExtractResult(Target.ObjectPath).Payload);
	if (!Test.TestEqual(*FString::Printf(TEXT("%s: the rig is unchanged"), *Stage), After, Before))
	{
		Test.AddInfo(FirstDifference(Before, After));
	}
	if (const UObject* Asset = Target.Get())
	{
		Test.TestFalse(*FString::Printf(TEXT("%s: package clean"), *Stage), Asset->GetOutermost()->IsDirty());
	}
}

// A JSON literal (outside TEXT(): MSVC does not take raw strings with commas as macro arguments).
TSharedPtr<FJsonObject> Json(const ANSICHAR* Text)
{
	return Parse(FString(UTF8_TO_TCHAR(Text)));
}

TArray<TSharedPtr<FJsonValue>> ArrayOf(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	const TArray<TSharedPtr<FJsonValue>>* Found = nullptr;
	return Object.IsValid() && Object->TryGetArrayField(Field, Found) ? *Found : TArray<TSharedPtr<FJsonValue>>();
}

// The entry of a Body list (Variables: by Name; Hierarchy: by Type + Name).
TSharedPtr<FJsonObject> FindEntry(const TSharedPtr<FJsonObject>& Body, const TCHAR* Region, const TCHAR* Name, const TCHAR* Type = nullptr)
{
	for (const TSharedPtr<FJsonValue>& Value : ArrayOf(Body, Region))
	{
		const TSharedPtr<FJsonObject> Entry = Value->AsObject();
		if (Entry.IsValid() && Entry->GetStringField(TEXT("Name")) == Name && (Type == nullptr || Entry->GetStringField(TEXT("Type")) == Type))
		{
			return Entry;
		}
	}
	return nullptr;
}

const FRigElementKey Bone(const TCHAR* Name) { return FRigElementKey(Name, ERigElementType::Bone); }
const FRigElementKey Null(const TCHAR* Name) { return FRigElementKey(Name, ERigElementType::Null); }
const FRigElementKey Control(const TCHAR* Name) { return FRigElementKey(Name, ERigElementType::Control); }

// CR_Hierarchy: the Content Browser's Control Rig on the tutorial mannequin (imported bones and curves, source import,
// preview mesh) with a hierarchy of every authored kind and member variables, made through the controllers the
// hierarchy tab and My Blueprint use; no graph nodes (the editor's first open would add the Forwards Solve). Saved.
UControlRigBlueprint* MakeHierarchyOnly(FAutomationTestBase& Test, const FTarget& Target)
{
	UControlRigBlueprint* Blueprint = AssetDocumentControlRigTests::MakeControlRig(Test, Target, TutorialMesh, false);
	if (Blueprint == nullptr)
	{
		return nullptr;
	}
	URigVMBlueprint& RigVM = *Blueprint;
	RigVM.AddMemberVariable(TEXT("Weight"), TEXT("float"), true, false, TEXT("0.5"));
	RigVM.AddMemberVariable(TEXT("Target"), TEXT("/Script/CoreUObject.Vector"), false, false, FString());
	RigVM.AddMemberVariable(TEXT("Names"), TEXT("TArray<FName>"), false, false, FString());
	URigHierarchy* Hierarchy = Blueprint->GetHierarchy();
	URigHierarchyController* Elements = Hierarchy->GetController(true);
	const FRigElementKey Space = Elements->AddNull(TEXT("space_null"), Bone(TEXT("root")), FTransform(FVector(0.0, 0.0, 100.0)), true, false);
	const FRigElementKey Multi = Elements->AddNull(TEXT("multi_null"), Bone(TEXT("root")), FTransform(FVector(10.0, 0.0, 0.0)), true, false);
	Test.TestTrue(TEXT("second parent"), Elements->AddParent(Multi, Bone(TEXT("pelvis")), 0.5f, true, TEXT("Pelvis"), false));
	const ERigControlType Types[] = {ERigControlType::Bool, ERigControlType::Float, ERigControlType::Integer, ERigControlType::Vector2D,
		ERigControlType::Position, ERigControlType::Scale, ERigControlType::Rotator, ERigControlType::Transform,
		ERigControlType::TransformNoScale, ERigControlType::EulerTransform};
	for (const ERigControlType Type : Types)
	{
		FRigControlSettings Settings;
		Settings.ControlType = Type;
		if (Type == ERigControlType::Float)
		{
			Settings.SetupLimitArrayForType(true, true, true);
			Settings.MinimumValue = FRigControlValue::Make<float>(-1.f);
			Settings.MaximumValue = FRigControlValue::Make<float>(2.f);
			Settings.ShapeName = TEXT("Box_Thick");
			Settings.ShapeColor = FLinearColor(0.f, 1.f, 0.f);
		}
		FRigControlValue Value;
		Value.SetFromTransform(FTransform(FRotator(0.0, 30.0, 0.0), FVector(1.0, 2.0, 3.0), FVector::OneVector), Type, Settings.PrimaryAxis);
		const FString Name = FString::Printf(TEXT("%s_ctrl"), *StaticEnum<ERigControlType>()->GetNameStringByValue(static_cast<int64>(Type)));
		Test.TestTrue(*FString::Printf(TEXT("control %s"), *Name),
			Elements->AddControl(*Name, Bone(TEXT("head")), Settings, Value, FTransform(FVector(0.0, 5.0, 0.0)), FTransform::Identity, false).IsValid());
	}
	FRigControlSettings ChannelSettings;
	ChannelSettings.ControlType = ERigControlType::Float;
	ChannelSettings.AnimationType = ERigControlAnimationType::AnimationChannel;
	const FRigElementKey Channel = Elements->AddAnimationChannel(TEXT("blend"), Control(TEXT("EulerTransform_ctrl")), ChannelSettings, false);
	Test.TestTrue(TEXT("channel host"), Elements->AddChannelHost(Channel, Control(TEXT("Transform_ctrl")), false));
	Test.TestTrue(TEXT("available space"), Elements->AddAvailableSpace(Control(TEXT("EulerTransform_ctrl")), Space, NAME_None, false));
	Test.TestTrue(TEXT("curve"), Elements->AddCurve(TEXT("test_curve"), 0.25f, false).IsValid());
	Test.TestTrue(TEXT("socket"), Elements->AddSocket(TEXT("test_socket"), Bone(TEXT("head")), FTransform(FVector(0.0, 0.0, 10.0)), true,
		FLinearColor::Red, TEXT("a socket"), false).IsValid());
	Test.TestTrue(TEXT("float metadata"), Hierarchy->SetFloatMetadata(Control(TEXT("EulerTransform_ctrl")), TEXT("meta_float"), 1.5f));
	Test.TestTrue(TEXT("key metadata"), Hierarchy->SetRigElementKeyMetadata(Control(TEXT("EulerTransform_ctrl")), TEXT("meta_key"), Bone(TEXT("head"))));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	Test.TestTrue(TEXT("CR_Hierarchy saved"), Save(*Blueprint));
	return Blueprint;
}

// The corpus sources of §7.2 (rig apply): #3 and #5.
const TCHAR* const ModuleSources[] = {
	TEXT("/ControlRig/Modules/Modules56/Root.Root"),
	TEXT("/ControlRigModules/Modules56/Constraint.Constraint"),
	TEXT("/ControlRigModules/Modules56/Hinge.Hinge"),
	TEXT("/ControlRigModules/Modules56/Wheel.Wheel"),
	TEXT("/ControlRigModules/Modules56/Finger.Finger"),
};
// Graphs.Rig.Nodes of a document (created when missing).
TSharedPtr<FJsonObject> RigNodes(const TSharedPtr<FJsonObject>& Document)
{
	const TSharedPtr<FJsonObject> Body = BodyOf(Document);
	const TSharedPtr<FJsonObject>* Found = nullptr;
	if (!Body->TryGetObjectField(TEXT("Graphs"), Found))
	{
		Body->SetObjectField(TEXT("Graphs"), MakeShared<FJsonObject>());
	}
	const TSharedPtr<FJsonObject> Graphs = Body->GetObjectField(TEXT("Graphs"));
	if (!Graphs->TryGetObjectField(TEXT("Rig"), Found))
	{
		Graphs->SetObjectField(TEXT("Rig"), MakeShared<FJsonObject>());
	}
	const TSharedPtr<FJsonObject> Rig = Graphs->GetObjectField(TEXT("Rig"));
	if (!Rig->TryGetObjectField(TEXT("Nodes"), Found))
	{
		Rig->SetObjectField(TEXT("Nodes"), MakeShared<FJsonObject>());
	}
	return Rig->GetObjectField(TEXT("Nodes"));
}

// The key of the first node whose Unit contains Unit, or empty.
FString NodeKeyOfUnit(const TSharedPtr<FJsonObject>& Nodes, const TCHAR* Unit)
{
	for (const auto& Node : Nodes->Values)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		FString NodeUnit;
		if (Node.Value->TryGetObject(Object) && (*Object)->TryGetStringField(TEXT("Unit"), NodeUnit) && NodeUnit.Contains(Unit))
		{
			return FString(*Node.Key);
		}
	}
	return FString();
}

// What the open Control Rig editor holds: its debugged rig has (or lacks) the null, its focused graph the node.
void TestControlRigEditorShows(FAutomationTestBase& Test, UControlRigBlueprint& Rig, const TCHAR* NullName, const TCHAR* NodeName,
	bool bExpected, const FString& Stage)
{
	// IRigVMEditor: GetRigVMHost / GetFocusedGraph are public there (protected on FRigVMEditorBase).
	IRigVMEditor* Editor = nullptr;
	if (IAssetEditorInstance* Instance = AssetDocumentRigOpenEditorTests::EditorOf(Rig))
	{
		Editor = FRigVMEditorBase::GetFromAssetEditorInstance(Instance);
	}
	if (!Test.TestNotNull(*FString::Printf(TEXT("%s: the Control Rig editor"), *Stage), Editor))
	{
		return;
	}
	UControlRig* Debugged = Cast<UControlRig>(Editor->GetRigVMHost());
	if (Test.TestNotNull(*FString::Printf(TEXT("%s: the editor's debugged rig"), *Stage), Debugged))
	{
		Test.TestEqual(*FString::Printf(TEXT("%s: the editor's rig has %s: %d"), *Stage, NullName, bExpected),
			Debugged->GetHierarchy()->Contains(Null(NullName)), bExpected);
	}
	if (const UEdGraph* Graph = Editor->GetFocusedGraph())
	{
		const bool bHasNode = Graph->Nodes.ContainsByPredicate([NodeName](const UEdGraphNode* Node)
		{
			return Node != nullptr && Node->GetFName() == FName(NodeName);
		});
		Test.TestEqual(*FString::Printf(TEXT("%s: the editor's graph has %s: %d"), *Stage, NodeName, bExpected), bHasNode, bExpected);
	}
	else
	{
		Test.AddInfo(FString::Printf(TEXT("%s: the editor has no focused graph"), *Stage));
	}
}

} // namespace AssetDocumentControlRigApplyTests

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigFidelityTest, "UEAssetDoc.ControlRig.Fidelity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigFidelityTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentControlRigApplyTests;
	FFixtures Fixtures;
	TArray<TPair<FString, const FTarget*>> Sources;
	const FTarget& HierarchyOnly = Fixtures.Add(TEXT("CR_Hierarchy"));
	if (MakeHierarchyOnly(*this, HierarchyOnly) != nullptr)
	{
		Sources.Emplace(TEXT("fixture CR_Hierarchy"), &HierarchyOnly);
	}
	const FTarget& AllKinds = Fixtures.Add(TEXT("CR_AllKinds"));
	if (AssetDocumentControlRigTests::MakeAllKinds(*this, AllKinds) != nullptr)
	{
		Sources.Emplace(TEXT("fixture CR_AllKinds"), &AllKinds);
	}
	for (const TCHAR* Path : ModuleSources)
	{
		UObject* Source = LoadQuiet(Path);
		if (!TestNotNull(*FString::Printf(TEXT("%s loads"), Path), Source))
		{
			continue;
		}
		const FTarget& Copy = Fixtures.Add(TEXT("CR_Corpus"));
		if (Duplicate(*this, Source, Copy) != nullptr)
		{
			Sources.Emplace(FPackageName::ObjectPathToObjectName(FString(Path)), &Copy);
		}
	}
	for (const TPair<FString, const FTarget*>& Source : Sources)
	{
		const FString& Name = Source.Key;
		const FAssetDocumentResult Original = ExtractResult(Source.Value->ObjectPath);
		if (!TestTrue(*FString::Printf(TEXT("%s extracts"), *Name), Original.IsSuccess() && Original.Payload.IsValid()))
		{
			continue;
		}
		const FTarget& Created = Fixtures.Add(TEXT("CR_Recreated"));
		double Start = FPlatformTime::Seconds();
		FAssetDocumentResult Applied;
		if (!ApplyOk(*this, Created, ForTarget(WithoutGraphs(Original.Payload), Created, TEXT("Create")), Name + TEXT(" Create"), &Applied))
		{
			continue;
		}
		const double CreateMs = Milliseconds(Start);
		const FAssetDocumentResult Recreated = ExtractResult(Created.ObjectPath);
		const FString Expected = Comparable(Original.Payload);
		const FString Got = Comparable(Recreated.Payload);
		if (!TestEqual(*FString::Printf(TEXT("%s: the created rig extracts as the source (minus Graphs / Functions)"), *Name), Got, Expected))
		{
			AddInfo(FirstDifference(Expected, Got));
		}
		TestTrue(*FString::Printf(TEXT("%s: the created rig compiles (%s)"), *Name, *StatusOf(Recreated, Created.PackageName)),
			StatusName(Recreated) == TEXT("UpToDate") || StatusName(Recreated) == TEXT("UpToDateWithWarnings"));
		Start = FPlatformTime::Seconds();
		const FAssetDocumentResult Again = Apply(Created, ForTarget(Recreated.Payload, Created, TEXT("Update")));
		const double ReapplyMs = Milliseconds(Start);
		TestTrue(*FString::Printf(TEXT("%s: unchanged re-apply succeeds (%s)"), *Name, *DiagnosticsText(Again)), Again.IsSuccess());
		TestFalse(*FString::Printf(TEXT("%s: unchanged re-apply is a no-op"), *Name), Again.bSavedAsset);
		Start = FPlatformTime::Seconds();
		TestNotNull(*FString::Printf(TEXT("%s: reload"), *Name), Reload(Created));
		const double ReloadMs = Milliseconds(Start);
		const FAssetDocumentResult Reloaded = ExtractResult(Created.ObjectPath);
		if (!TestEqual(*FString::Printf(TEXT("%s: equal after reload"), *Name), WithoutTarget(Reloaded.Payload), WithoutTarget(Recreated.Payload)))
		{
			AddInfo(FirstDifference(WithoutTarget(Recreated.Payload), WithoutTarget(Reloaded.Payload)));
		}
		AddInfo(FString::Printf(TEXT("PASS %s: Create %.0f ms, unchanged re-apply %.0f ms, reload %.0f ms, %d chars, status %s / source %s%s"),
			*Name, CreateMs, ReapplyMs, ReloadMs, Expected.Len(), *StatusName(Recreated), *StatusName(Original),
			Applied.Diagnostics.IsEmpty() ? TEXT("") : *(TEXT("; apply warnings: ") + DiagnosticsText(Applied).Left(400))));
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigFreeEditsTest, "UEAssetDoc.ControlRig.FreeEdits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigFreeEditsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentControlRigApplyTests;
	// FBlueprintEditorUtils::RenameMemberVariable (My Blueprint's rename) compiles before URigVMBlueprint moves the
	// variable nodes to the new name: the editor logs these for the renamed variable's nodes, then recovers.
	AddExpectedError(TEXT("is using a missing variable"), EAutomationExpectedErrorFlags::Contains, 0);
	FFixtures Fixtures;
	struct FSource
	{
		const TCHAR* Name;
		const FTarget* Target;
	};
	TArray<FSource> Sources;
	const FTarget& HierarchyOnly = Fixtures.Add(TEXT("CR_Hierarchy"));
	if (MakeHierarchyOnly(*this, HierarchyOnly) != nullptr)
	{
		Sources.Add({TEXT("CR_Hierarchy"), &HierarchyOnly});
	}
	const FTarget& AllKinds = Fixtures.Add(TEXT("CR_AllKinds"));
	if (AssetDocumentControlRigTests::MakeAllKinds(*this, AllKinds) != nullptr)
	{
		Sources.Add({TEXT("CR_AllKinds"), &AllKinds});
	}
	for (const FSource& Source : Sources)
	{
		const FString Name = Source.Name;
		UObject* Original = Source.Target->Get();
		const FTarget& EditorSide = Fixtures.Add(TEXT("CR_EditorEdit"));
		const FTarget& DocumentSide = Fixtures.Add(TEXT("CR_DocumentEdit"));
		if (Duplicate(*this, Original, EditorSide) == nullptr || Duplicate(*this, Original, DocumentSide) == nullptr)
		{
			continue;
		}
		// Both sides as a person finds them: saved rigs loaded from disk.
		UControlRigBlueprint* Edited = Cast<UControlRigBlueprint>(Reload(EditorSide));
		if (!TestNotNull(*FString::Printf(TEXT("%s reloads"), *Name), Edited) || Reload(DocumentSide) == nullptr)
		{
			continue;
		}
		// A duplicate's variable nodes keep the source's variable guids; the editor side starts as a rig made in place
		// has them (the document edit points them at the variables itself).
		UE::AssetDocument::Graph::Rig::ControlRig::PointVariableNodesAtVariables(*Edited);

		// The editor's edits: hierarchy tab (add, reparent keeping global, reorder, rename, delete, spaces, channels),
		// element Details (shape, colour, limits, curve value, metadata), My Blueprint (add, rename, retype, category,
		// tooltip, default value).
		URigHierarchy* Hierarchy = Edited->GetHierarchy();
		URigHierarchyController* Elements = Hierarchy->GetController(true);
		FRigControlSettings Settings;
		Settings.ControlType = ERigControlType::Float;
		Settings.SetupLimitArrayForType(true, true, true);
		Settings.MinimumValue = FRigControlValue::Make<float>(-2.f);
		Settings.MaximumValue = FRigControlValue::Make<float>(3.f);
		Settings.ShapeName = TEXT("Box_Thick");
		Settings.ShapeColor = FLinearColor(0.f, 1.f, 0.f);
		TestTrue(TEXT("add a control under hand_r"), Elements->AddControl(TEXT("hand_free"), Bone(TEXT("hand_r")), Settings,
			FRigControlValue::Make<float>(0.5f), FTransform(FVector(0.0, 3.0, 0.0)), FTransform::Identity, false).IsValid());
		TestTrue(TEXT("add a space"), Elements->AddAvailableSpace(Control(TEXT("EulerTransform_ctrl")), Bone(TEXT("pelvis")), TEXT("Hips"), false));
		TestTrue(TEXT("reparent a null keeping global"), Elements->SetParent(Null(TEXT("space_null")), Bone(TEXT("spine_01")), true, false));
		TestTrue(TEXT("reorder"), Elements->ReorderElement(Control(TEXT("Bool_ctrl")), 2, false));
		TestEqual(TEXT("rename a null"), Elements->RenameElement(Null(TEXT("multi_null")), TEXT("blend_null"), false).Name, FName(TEXT("blend_null")));
		TestTrue(TEXT("delete a control"), Elements->RemoveElement(Control(TEXT("Scale_ctrl")), false));
		FRigControlSettings ChannelSettings;
		ChannelSettings.ControlType = ERigControlType::Float;
		ChannelSettings.AnimationType = ERigControlAnimationType::AnimationChannel;
		const FRigElementKey Channel = Elements->AddAnimationChannel(TEXT("free_channel"), Control(TEXT("Transform_ctrl")), ChannelSettings, false);
		TestTrue(TEXT("animation channel with a second host"), Channel.IsValid()
			&& Elements->AddChannelHost(Channel, Control(TEXT("Position_ctrl")), false));
		Hierarchy->SetCurveValue(FRigElementKey(TEXT("test_curve"), ERigElementType::Curve), 0.75f);
		TestTrue(TEXT("metadata"), Hierarchy->SetFloatMetadata(Control(TEXT("Float_ctrl")), TEXT("free_meta"), 2.5f));
		Hierarchy->SetLocalTransform(Control(TEXT("Position_ctrl")), FTransform(FVector(0.0, 7.0, 1.0)), true, true);
		URigVMBlueprint& RigVM = *Edited;
		IRigVMAssetInterface& Variables = *Edited;
		TestEqual(TEXT("add a variable"), RigVM.AddMemberVariable(TEXT("Speed"), TEXT("float"), false, false, FString()), FName(TEXT("Speed")));
		TestTrue(TEXT("rename a variable"), RigVM.RenameMemberVariable(TEXT("Weight"), TEXT("BlendWeight")));
		TestTrue(TEXT("retype a variable"), RigVM.ChangeMemberVariableType(TEXT("Target"), TEXT("/Script/CoreUObject.Rotator")));
		Variables.SetVariableCategory(TEXT("Speed"), TEXT("Motion"));
		Variables.SetVariableTooltip(TEXT("Speed"), FText::FromString(TEXT("Units per second")));
		FKismetEditorUtilities::CompileBlueprint(Edited);
		if (UObject* Defaults = Edited->GeneratedClass != nullptr ? Edited->GeneratedClass->GetDefaultObject() : nullptr)
		{
			if (FFloatProperty* Speed = FindFProperty<FFloatProperty>(Edited->GeneratedClass, TEXT("Speed")))
			{
				Defaults->Modify();
				Speed->SetPropertyValue_InContainer(Defaults, 4.f);
				FBlueprintEditorUtils::MarkBlueprintAsModified(Edited);
			}
		}
		FKismetEditorUtilities::CompileBlueprint(Edited);
		TestTrue(TEXT("editor edits saved"), Save(*Edited));
		const FAssetDocumentResult EditorResult = ExtractResult(EditorSide.ObjectPath);
		if (!TestTrue(*FString::Printf(TEXT("%s: editor side extracts"), *Name), EditorResult.IsSuccess()))
		{
			continue;
		}

		// The same edit as a document: the editor side's state, renames marked.
		const TSharedPtr<FJsonObject> Document = ForTarget(EditorResult.Payload, DocumentSide, TEXT("Update"));
		if (const TSharedPtr<FJsonObject> Renamed = FindEntry(BodyOf(Document), TEXT("Hierarchy"), TEXT("blend_null"), TEXT("Null")))
		{
			Renamed->SetStringField(TEXT("RenamedFrom"), TEXT("multi_null"));
		}
		if (const TSharedPtr<FJsonObject> Renamed = FindEntry(BodyOf(Document), TEXT("Variables"), TEXT("BlendWeight")))
		{
			Renamed->SetStringField(TEXT("RenamedFrom"), TEXT("Weight"));
		}
		const double Start = FPlatformTime::Seconds();
		FAssetDocumentResult Applied;
		if (!ApplyOk(*this, DocumentSide, Document, Name + TEXT(" document edit"), &Applied))
		{
			continue;
		}
		const double ApplyMs = Milliseconds(Start);
		const FAssetDocumentResult DocumentResult = ExtractResult(DocumentSide.ObjectPath);
		const FString Expected = WithoutTarget(EditorResult.Payload);
		const FString Got = WithoutTarget(DocumentResult.Payload);
		if (!TestEqual(*FString::Printf(TEXT("%s: document edit equals the editor's"), *Name), Got, Expected))
		{
			AddInfo(FirstDifference(Expected, Got));
		}
		TestEqual(*FString::Printf(TEXT("%s: equal compile status"), *Name), StatusOf(DocumentResult, DocumentSide.PackageName),
			StatusOf(EditorResult, EditorSide.PackageName));
		const FAssetDocumentResult Again = Apply(DocumentSide, ForTarget(DocumentResult.Payload, DocumentSide, TEXT("Update")));
		TestFalse(*FString::Printf(TEXT("%s: re-apply is a no-op"), *Name), Again.bSavedAsset);
		AddInfo(FString::Printf(TEXT("PASS %s: document edit %.0f ms, status %s%s"), *Name, ApplyMs, *StatusName(DocumentResult),
			Applied.Diagnostics.IsEmpty() ? TEXT("") : *(TEXT("; warnings: ") + DiagnosticsText(Applied).Left(400))));
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigRollbackTest, "UEAssetDoc.ControlRig.Rollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigRollbackTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentControlRigApplyTests;
	FFixtures Fixtures;

	// 1. A refused op after others ran (a variable added, an element written): everything is rolled back.
	const FTarget& Target = Fixtures.Add(TEXT("CR_Rollback"));
	if (MakeHierarchyOnly(*this, Target) != nullptr)
	{
		const TSharedPtr<FJsonObject> State = Extract(*this, Target.ObjectPath, TEXT("before"));
		const TSharedPtr<FJsonObject> Failing = ForTarget(State, Target, TEXT("Update"));
		TArray<TSharedPtr<FJsonValue>> Variables = ArrayOf(BodyOf(Failing), TEXT("Variables"));
		Variables.Add(MakeShared<FJsonValueObject>(Json(R"J({"Name": "Added", "Type": "float", "Default": 2.0})J")));
		BodyOf(Failing)->SetArrayField(TEXT("Variables"), Variables);
		if (const TSharedPtr<FJsonObject> Null = FindEntry(BodyOf(Failing), TEXT("Hierarchy"), TEXT("space_null"), TEXT("Null")))
		{
			Null->SetObjectField(TEXT("Transform"), Json(R"J({"Translation": {"X": 5.0, "Y": 0.0, "Z": 0.0}})J"));
		}
		TArray<TSharedPtr<FJsonValue>> Elements = ArrayOf(BodyOf(Failing), TEXT("Hierarchy"));
		Elements.Add(MakeShared<FJsonValueObject>(Json(R"J({"Type": "Control", "Name": "bad_ctrl", "Parent": "Bone(head)", "Settings": {"ControlType": "Float", "NoSuchSetting": 1}})J")));
		BodyOf(Failing)->SetArrayField(TEXT("Hierarchy"), Elements);
		ExpectRefused(*this, Target, Failing, TEXT("ControlRig.InvalidValue"), TEXT("refused op"));
	}

	// 2. A failed compile: rig B calls a public function of rig A that reads A's member variable, remapped to B's Gain;
	// retyping Gain unmaps it ("Node @@ has unmapped variables"): a new compile error, rolled back.
	// The reference node reports unmapped variables until it is remapped, and again in the failed compile.
	AssetDocumentIKRigTests::ExpectUnmappedVariableErrors(*this, 0);
	const FTarget& Host = Fixtures.Add(TEXT("CR_FunctionHost"));
	const FTarget& User = Fixtures.Add(TEXT("CR_FunctionUser"));
	UControlRigBlueprint* HostRig = AssetDocumentControlRigTests::MakeControlRig(*this, Host, nullptr, false);
	UControlRigBlueprint* UserRig = AssetDocumentControlRigTests::MakeControlRig(*this, User, nullptr, false);
	if (HostRig == nullptr || UserRig == nullptr)
	{
		return false;
	}
	URigVMBlueprint& HostVM = *HostRig;
	HostVM.AddMemberVariable(TEXT("Amount"), TEXT("float"), true, false, TEXT("1.0"));
	URigVMFunctionLibrary* Library = HostVM.GetLocalFunctionLibrary();
	URigVMController* LibraryController = Library != nullptr ? HostVM.GetOrCreateController(Library) : nullptr;
	URigVMLibraryNode* Function = LibraryController != nullptr
		? LibraryController->AddFunctionToLibrary(TEXT("ReadAmount"), false, FVector2D::ZeroVector, false) : nullptr;
	if (!TestNotNull(TEXT("host function"), Function))
	{
		return false;
	}
	URigVMController* FunctionBody = HostVM.GetOrCreateController(Function->GetContainedGraph());
	FunctionBody->AddExposedPin(TEXT("Out"), ERigVMPinDirection::Output, TEXT("float"), NAME_None, TEXT("0.0"), false);
	TestNotNull(TEXT("member variable read in the function"), FunctionBody->AddVariableNode(TEXT("Amount"), TEXT("float"), nullptr, true,
		FString(), FVector2D(-200.0, 0.0), TEXT("GetAmount"), false));
	TestTrue(TEXT("function link"), FunctionBody->AddLink(TEXT("GetAmount.Value"), TEXT("Return.Out"), false));
	HostVM.MarkFunctionPublic(Function->GetFName(), true);
	FKismetEditorUtilities::CompileBlueprint(HostRig);
	TestTrue(TEXT("host saved"), Save(*HostRig));

	URigVMBlueprint& UserVM = *UserRig;
	UserVM.AddMemberVariable(TEXT("Gain"), TEXT("float"), true, false, TEXT("1.0"));
	URigVMController* Rig = UserVM.GetOrCreateController(UserVM.GetDefaultModel());
	Rig->AddUnitNodeFromStructPath(TEXT("/Script/ControlRig.RigUnit_BeginExecution"), TEXT("Execute"), FVector2D::ZeroVector, TEXT("ForwardsSolve"), false);
	URigVMFunctionReferenceNode* Call = Rig->AddFunctionReferenceNode(Function, FVector2D(300.0, 0.0), TEXT("CallRead"), false);
	if (!TestNotNull(TEXT("reference to the host's function"), Call))
	{
		return false;
	}
	TestTrue(TEXT("variable remapped"), UE::AssetDocument::Graph::Rig::ControlRig::SetFunctionVariableMapping(*Rig, *Call, TEXT("Amount"), TEXT("Gain"), false));
	FKismetEditorUtilities::CompileBlueprint(UserRig);
	TestTrue(TEXT("user saved"), Save(*UserRig));
	const FAssetDocumentResult UserState = ExtractResult(User.ObjectPath);
	AddInfo(FString::Printf(TEXT("function user compiles: %s"), *StatusOf(UserState, User.PackageName)));
	TestTrue(TEXT("function user compiles without errors"), StatusName(UserState) == TEXT("UpToDate") || StatusName(UserState) == TEXT("UpToDateWithWarnings"));
	const TSharedPtr<FJsonObject> Retyped = ForTarget(WithoutGraphs(UserState.Payload), User, TEXT("Update"));
	if (const TSharedPtr<FJsonObject> Gain = FindEntry(BodyOf(Retyped), TEXT("Variables"), TEXT("Gain")))
	{
		Gain->SetStringField(TEXT("Type"), TEXT("FString"));
		Gain->RemoveField(TEXT("Default"));
	}
	ExpectRefused(*this, User, Retyped, TEXT("ControlRig.CompileFailed"), TEXT("failed compile"));
	TestEqual(TEXT("status after rollback"), StatusName(ExtractResult(User.ObjectPath)), StatusName(UserState));
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigUndoHistoryTest, "UEAssetDoc.ControlRig.UndoHistory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigUndoHistoryTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentControlRigApplyTests;
	if (GEditor == nullptr || GEditor->Trans == nullptr)
	{
		return false;
	}
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("CR_Undo"));
	if (MakeHierarchyOnly(*this, Target) == nullptr)
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
	// The hierarchy tab's add (one transaction, controller undo on), as a human makes it.
	auto HumanAdd = [&Target](const TCHAR* Name)
	{
		UControlRigBlueprint* Rig = Cast<UControlRigBlueprint>(Target.Get());
		if (Rig == nullptr)
		{
			return false;
		}
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentControlRigTests", "HumanAdd", "Add Null"));
		return Rig->GetHierarchy()->GetController(true)->AddNull(Name, Bone(TEXT("root")), FTransform(FVector(0.0, 0.0, 30.0)), false, true).IsValid();
	};

	// 1. The human adds a null and saves; the AI edits: history referencing the rig is cleared, the human edit is kept.
	TestTrue(TEXT("human edit"), HumanAdd(TEXT("human_null")));
	TestTrue(TEXT("human save"), Target.Get() != nullptr && UEditorLoadingAndSavingUtils::SavePackages({Target.Get()->GetOutermost()}, false));
	TestTrue(TEXT("the human edit is in the undo history"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	const TSharedPtr<FJsonObject> HumanState = Extract(*this, Target.ObjectPath, TEXT("after human edit"));
	const TSharedPtr<FJsonObject> AiEdit = ForTarget(HumanState, Target, TEXT("Update"));
	TArray<TSharedPtr<FJsonValue>> Elements = ArrayOf(BodyOf(AiEdit), TEXT("Hierarchy"));
	Elements.Add(MakeShared<FJsonValueObject>(Json(R"J({"Type": "Null", "Name": "ai_null", "Parent": "Bone(pelvis)", "Transform": {"Translation": {"X": 0.0, "Y": 12.0, "Z": 0.0}}})J")));
	BodyOf(AiEdit)->SetArrayField(TEXT("Hierarchy"), Elements);
	if (!ApplyOk(*this, Target, AiEdit, TEXT("AI edit")))
	{
		return false;
	}
	TestFalse(TEXT("after AI edit: no undo history references the rig"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	const FString AfterEdit = Serialize(Extract(*this, Target.ObjectPath, TEXT("after AI edit")));
	TestTrue(TEXT("human edit kept"), AfterEdit.Contains(TEXT("\"Name\":\"human_null\"")));
	TestTrue(TEXT("AI edit made"), AfterEdit.Contains(TEXT("\"Name\":\"ai_null\"")));
	UndoRedoAll(TEXT("after AI edit"));
	TestEqual(TEXT("after AI edit + undo/redo"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after undo/redo"))), AfterEdit);

	// 2. The human edits again; an AI apply fails and rolls back to the human state; undo stays safe.
	ReleaseUndoHistory();
	TestTrue(TEXT("second human edit"), HumanAdd(TEXT("human_null_2")));
	// Saved (Core refuses an apply to an asset with unsaved changes), the undo history kept.
	TestTrue(TEXT("second human save"), Target.Get() != nullptr && UEditorLoadingAndSavingUtils::SavePackages({Target.Get()->GetOutermost()}, false));
	const FString SecondHuman = Serialize(Extract(*this, Target.ObjectPath, TEXT("after second human edit")));
	const TSharedPtr<FJsonObject> Failing = ForTarget(Parse(SecondHuman), Target, TEXT("Update"));
	TArray<TSharedPtr<FJsonValue>> More = ArrayOf(BodyOf(Failing), TEXT("Hierarchy"));
	More.Add(MakeShared<FJsonValueObject>(Json(R"J({"Type": "Null", "Name": "fine_null", "Parent": "Bone(Root)"})J")));
	More.Add(MakeShared<FJsonValueObject>(Json(R"J({"Type": "Control", "Name": "bad", "Parent": "Bone(Root)", "Settings": {"ControlType": "Nope"}})J")));
	BodyOf(Failing)->SetArrayField(TEXT("Hierarchy"), More);
	const FAssetDocumentResult Failed = Apply(Target, Failing);
	TestFalse(TEXT("failing apply fails"), Failed.IsSuccess());
	AddInfo(FString::Printf(TEXT("failing apply: %s"), *DiagnosticsText(Failed)));
	TestEqual(TEXT("after rollback: the human state"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after rollback"))), SecondHuman);
	UndoRedoAll(TEXT("after rollback"));
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigOpenEditorApplyTest, "UEAssetDoc.ControlRig.OpenEditorApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigOpenEditorApplyTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentControlRigApplyTests;
	namespace OpenEd = AssetDocumentRigOpenEditorTests;
	UAssetEditorSubsystem* AssetEditors = OpenEd::Editors();
	if (!TestNotNull(TEXT("asset editors"), AssetEditors) || GEditor->Trans == nullptr)
	{
		return false;
	}
	FFixtures Fixtures;

	// A new rig from a document has the Forwards Solve event the editor's first open adds to an empty rig.
	const FTarget& Created = Fixtures.Add(TEXT("CR_Created"));
	const TSharedPtr<FJsonObject> Empty = Json(R"J({"SchemaVersion": 1, "Action": "Create", "Body": {}})J");
	Empty->SetStringField(TEXT("Class"), ControlRigClassPath);
	Empty->SetStringField(TEXT("Target"), Created.PackageName);
	if (ApplyOk(*this, Created, Empty, TEXT("Create from an empty document")))
	{
		const TSharedPtr<FJsonObject> Graphs = BodyOf(Extract(*this, Created.ObjectPath, TEXT("created")))->GetObjectField(TEXT("Graphs"));
		AddInfo(FString::Printf(TEXT("created graphs: %s"), *Serialize(Graphs)));
		const FTarget& Opened = Fixtures.Add(TEXT("CR_Opened"));
		UControlRigBlueprint* Rig = AssetDocumentControlRigTests::MakeControlRig(*this, Opened, nullptr, false);
		if (Rig != nullptr && Save(*Rig) && AssetEditors->OpenEditorForAsset(Rig))
		{
			AssetEditors->CloseAllEditorsForAsset(Rig);
			const TSharedPtr<FJsonObject> EditorGraphs = BodyOf(Extract(*this, Opened.ObjectPath, TEXT("opened")))->GetObjectField(TEXT("Graphs"));
			TestEqual(TEXT("the created graph equals the editor's first open"), Serialize(Graphs), Serialize(EditorGraphs));
		}
		else
		{
			AddInfo(TEXT("The Control Rig editor did not open in this session; the first-open graph is not compared."));
		}
	}

	// The rig is opened and not saved (the open added the Forwards Solve); the apply lands with the editor open.
	const FTarget& Target = Fixtures.Add(TEXT("CR_Open"));
	UControlRigBlueprint* Rig = MakeHierarchyOnly(*this, Target);
	if (Rig == nullptr)
	{
		return false;
	}
	if (!AssetEditors->OpenEditorForAsset(Rig))
	{
		AddWarning(TEXT("The Control Rig editor did not open in this session; the open-editor apply was not exercised."));
		return true;
	}
	ON_SCOPE_EXIT
	{
		if (UObject* Open = Target.Get())
		{
			AssetEditors->CloseAllEditorsForAsset(Open);
		}
	};
	OpenEd::TickEditorUi();
	AddInfo(FString::Printf(TEXT("after the open the package is %s"), Rig->GetOutermost()->IsDirty() ? TEXT("dirty (the editor's Forwards Solve)") : TEXT("clean")));
	ReleaseUndoHistory();
	const IAssetEditorInstance* EditorBefore = OpenEd::EditorOf(*Rig);
	const TSharedPtr<FJsonObject> State = Extract(*this, Target.ObjectPath, TEXT("open"));
	const TSharedPtr<FJsonObject> Edit = ForTarget(State, Target, TEXT("Update"));
	TArray<TSharedPtr<FJsonValue>> Elements = ArrayOf(BodyOf(Edit), TEXT("Hierarchy"));
	Elements.Add(MakeShared<FJsonValueObject>(Json(R"J({"Type": "Null", "Name": "open_null", "Parent": "Bone(Root)"})J")));
	BodyOf(Edit)->SetArrayField(TEXT("Hierarchy"), Elements);
	const TSharedPtr<FJsonObject> Nodes = RigNodes(Edit);
	const FString Solve = NodeKeyOfUnit(Nodes, TEXT("BeginExecution"));
	if (!TestFalse(TEXT("the open rig has its Forwards Solve"), Solve.IsEmpty()))
	{
		AddInfo(Serialize(Nodes));
		return false;
	}
	const TSharedPtr<FJsonObject> SetNode = Json(R"J({"Unit": "RigUnit_SetTransform", "Position": [300, 0],
		"Defaults": {"Item": "Null(open_null)"}})J");
	SetNode->SetObjectField(TEXT("Inputs"), Json(R"J({})J"));
	SetNode->GetObjectField(TEXT("Inputs"))->SetStringField(TEXT("ExecutePin"), Solve + TEXT(".ExecutePin"));
	Nodes->SetObjectField(TEXT("OpenSet"), SetNode);
	// The apply closes the open editor and opens a new one on the applied rig.
	UE::AssetDocument::OpenEditorTests::FEditorReopenWatch Reopen(Target.PackageName, EditorBefore);
	FAssetDocumentResult Applied;
	const bool bApplied = ApplyOk(*this, Target, Edit, TEXT("apply while open"), &Applied);
	Reopen.Stop();
	if (!bApplied)
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("apply while open: %s"), *DiagnosticsText(Applied)));
	OpenEd::TickEditorUi();
	Rig = Cast<UControlRigBlueprint>(Target.Get());
	if (!TestNotNull(TEXT("rig after the apply"), Rig))
	{
		return false;
	}
	TestTrue(TEXT("the null"), Rig->GetHierarchy()->Contains(Null(TEXT("open_null"))));
	TestFalse(TEXT("the package is saved"), Rig->GetOutermost()->IsDirty());
	TestFalse(TEXT("the apply is not an undo step"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	TestTrue(*FString::Printf(TEXT("ControlRig.EditorReopened (%s)"), *DiagnosticsText(Applied)), HasCode(Applied, TEXT("ControlRig.EditorReopened")));
	TestTrue(TEXT("the apply closed the open editor"), Reopen.Closed());
	TestTrue(TEXT("a new editor, built from the applied rig"), Reopen.Reopened() != nullptr && OpenEd::EditorOf(*Rig) == Reopen.Reopened());
	TestControlRigEditorShows(*this, *Rig, TEXT("open_null"), TEXT("OpenSet"), true, TEXT("after the apply"));
	const FString AppliedText = Serialize(Extract(*this, Target.ObjectPath, TEXT("applied")));

	// A failure (an unknown unit) after a valid change (another null) rolls back with the editor open.
	{
		const TSharedPtr<FJsonObject> Bad = ForTarget(Parse(AppliedText), Target, TEXT("Update"));
		TArray<TSharedPtr<FJsonValue>> BadElements = ArrayOf(BodyOf(Bad), TEXT("Hierarchy"));
		BadElements.Add(MakeShared<FJsonValueObject>(Json(R"J({"Type": "Null", "Name": "fail_null", "Parent": "Bone(Root)"})J")));
		BodyOf(Bad)->SetArrayField(TEXT("Hierarchy"), BadElements);
		RigNodes(Bad)->SetObjectField(TEXT("FailNode"), Json(R"J({"Unit": "RigUnit_NoSuchUnit", "Position": [600, 0]})J"));
		const FAssetDocumentResult Failed = Apply(Target, Bad);
		AddInfo(FString::Printf(TEXT("failing apply while open: %s"), *DiagnosticsText(Failed)));
		TestFalse(TEXT("failing apply while open fails"), Failed.IsSuccess());
		TestTrue(TEXT("failing apply: ControlRig.UnknownUnit"), HasCode(Failed, TEXT("ControlRig.UnknownUnit")));
		OpenEd::TickEditorUi();
		Rig = Cast<UControlRigBlueprint>(Target.Get());
		if (!TestNotNull(TEXT("rig after the rollback"), Rig))
		{
			return false;
		}
		TestEqual(TEXT("after the rollback the rig is as applied"), Serialize(Extract(*this, Target.ObjectPath, TEXT("rolled back"))), AppliedText);
		TestFalse(TEXT("after the rollback the package is clean"), Rig->GetOutermost()->IsDirty());
		TestNotNull(TEXT("after the rollback the editor is open"), OpenEd::EditorOf(*Rig));
		TestControlRigEditorShows(*this, *Rig, TEXT("open_null"), TEXT("OpenSet"), true, TEXT("after the rollback"));
		TestControlRigEditorShows(*this, *Rig, TEXT("fail_null"), TEXT("FailNode"), false, TEXT("after the rollback"));
	}

	// A human hierarchy edit (the hierarchy tab's add), saved; undo / redo with the editor open.
	ReleaseUndoHistory();
	{
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentControlRigTests", "HumanAddOpen", "Add Null"));
		Rig->GetHierarchy()->GetController(true)->AddNull(TEXT("human_null"), Bone(TEXT("root")), FTransform(FVector(0.0, 0.0, 30.0)), false, true);
	}
	const FString Human = Serialize(Extract(*this, Target.ObjectPath, TEXT("human edit")));
	TestNotEqual(TEXT("the human edit landed"), Human, AppliedText);
	TestTrue(TEXT("the human's save"), OpenEd::HumanSave(*this, *Rig));
	TestTrue(TEXT("saving keeps the apply"), Rig->GetHierarchy()->Contains(Null(TEXT("open_null"))));
	TestTrue(TEXT("the human edit undoes"), GEditor->Trans->CanUndo() && GEditor->UndoTransaction());
	OpenEd::TickEditorUi();
	TestFalse(TEXT("undo removes the human null"), Rig->GetHierarchy()->Contains(Null(TEXT("human_null"))));
	TestTrue(TEXT("undo keeps the applied null"), Rig->GetHierarchy()->Contains(Null(TEXT("open_null"))));
	TestTrue(TEXT("the human edit redoes"), GEditor->Trans->CanRedo() && GEditor->RedoTransaction());
	OpenEd::TickEditorUi();
	TestTrue(TEXT("redo restores the human null"), Rig->GetHierarchy()->Contains(Null(TEXT("human_null"))));
	ReleaseUndoHistory();
	Rig->GetOutermost()->SetDirtyFlag(false);
	AssetEditors->CloseAllEditorsForAsset(Rig);
	TestNull(TEXT("the editor closed"), OpenEd::EditorOf(*Rig));
	return true;
}

#endif
