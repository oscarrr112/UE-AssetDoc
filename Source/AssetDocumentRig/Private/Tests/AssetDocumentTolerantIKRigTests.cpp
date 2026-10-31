// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// An IK Rig holding a project solver, and an IK Retargeter holding a project op, whose settings have a field the
// document has no form for (a TFieldPath), set as the Details panel's struct editor does. The field reads as a marker
// with a warning, the extract re-applies as a no-op, an edit of another settings field (with the marker left as read,
// or omitted) keeps the field, and an edited marker is refused. The solver and op are test types of the
// AssetDocumentTests module (AssetDocumentIKRigTestTypes.h), found by path: the automation framework loads that module
// before it lists the tests.

#include "AssetDocumentIKRigTestSupport.h"
#include "AssetDocumentTolerantTestSupport.h"

#include "Retargeter/IKRetargeter.h"
#include "Retargeter/IKRetargetOps.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Rig/IKRigDefinition.h"
#include "Rig/Solvers/IKRigSolverBase.h"
#include "RigEditor/IKRigController.h"
#include "ScopedTransaction.h"
#include "UObject/FieldPathProperty.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentTolerantIKRigTestsLocal
{
const TCHAR* const SolverPath = TEXT("/Script/AssetDocumentTests.AssetDocumentTolerantTestSolver");
const TCHAR* const OpPath = TEXT("/Script/AssetDocumentTests.AssetDocumentTolerantTestOp");

// The tolerant-test view of an IK fixture (same asset, package and sidecar).
AssetDocumentTolerantTestSupport::FTarget View(const AssetDocumentIKRigTests::FTarget& Target)
{
	AssetDocumentTolerantTestSupport::FTarget Out(TEXT("Unused"));
	Out.AssetName = Target.AssetName;
	Out.PackageName = Target.PackageName;
	Out.ObjectPath = Target.ObjectPath;
	Out.PackageFile = Target.PackageFile;
	Out.SidecarPath = Target.SidecarPath;
	return Out;
}

// What the Filter field is set to: a property path, which the document has no form for.
FFieldPath FilterValue()
{
	return FFieldPath(UIKRigEffectorGoal::StaticClass()->FindPropertyByName(TEXT("PositionAlpha")));
}

// A Details edit of the test settings (Alpha and Filter) inside the owner's transaction.
bool SetSettings(FAutomationTestBase& Test, UObject& Owner, const UScriptStruct* Type, void* Settings, float Alpha)
{
	const FFloatProperty* AlphaField = Type != nullptr ? CastField<FFloatProperty>(Type->FindPropertyByName(TEXT("Alpha"))) : nullptr;
	const FFieldPathProperty* FilterField = Type != nullptr ? CastField<FFieldPathProperty>(Type->FindPropertyByName(TEXT("Filter"))) : nullptr;
	if (!Test.TestTrue(TEXT("the test settings have Alpha and Filter"), Settings != nullptr && AlphaField != nullptr && FilterField != nullptr))
	{
		return false;
	}
	const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentTolerantIKRigTests", "EditSettings", "Edit Settings"));
	Owner.Modify();
	AlphaField->SetPropertyValue_InContainer(Settings, Alpha);
	*FilterField->ContainerPtrToValuePtr<FFieldPath>(Settings) = FilterValue();
	return true;
}

// The settings' Alpha and whether Filter still holds the value set (-1 when the settings are missing).
float AlphaOf(const UScriptStruct* Type, const void* Settings)
{
	const FFloatProperty* AlphaField = Type != nullptr ? CastField<FFloatProperty>(Type->FindPropertyByName(TEXT("Alpha"))) : nullptr;
	return AlphaField != nullptr && Settings != nullptr ? AlphaField->GetPropertyValue_InContainer(Settings) : -1.f;
}

bool FilterKept(const UScriptStruct* Type, const void* Settings)
{
	const FFieldPathProperty* FilterField = Type != nullptr ? CastField<FFieldPathProperty>(Type->FindPropertyByName(TEXT("Filter"))) : nullptr;
	return FilterField != nullptr && Settings != nullptr && *FilterField->ContainerPtrToValuePtr<FFieldPath>(Settings) == FilterValue();
}

TSharedPtr<FJsonObject> SettingsAt(const TSharedPtr<FJsonObject>& Document, const FString& Pointer)
{
	const TSharedPtr<FJsonValue> Value = AssetDocumentTolerantTestSupport::At(Document, Pointer);
	return Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
}

int32 TopLevelOpIndex(const TSharedPtr<FJsonObject>& Document, const FString& Name)
{
	const TArray<TSharedPtr<FJsonValue>>& Ops = Document->GetObjectField(TEXT("Body"))->GetArrayField(TEXT("Ops"));
	return Ops.IndexOfByPredicate([&Name](const TSharedPtr<FJsonValue>& Op) { return Op->AsObject()->GetStringField(TEXT("Name")) == Name; });
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantIKRigTest, "UEAssetDoc.Tolerant.IKRig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantIKRigTest::RunTest(const FString& Parameters)
{
	namespace Tolerant = AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentTolerantIKRigTestsLocal;
	UScriptStruct* SolverType = FindObject<UScriptStruct>(nullptr, SolverPath);
	if (!TestNotNull(TEXT("the test solver type is loaded"), SolverType))
	{
		return false;
	}
	AssetDocumentIKRigTests::FFixtures Fixtures;
	const AssetDocumentIKRigTests::FTarget& Fixture = Fixtures.Add(TEXT("IK_Tolerant"));
	UIKRigDefinition* Rig = AssetDocumentIKRigTests::MakeRig(*this, Fixture, AssetDocumentIKRigTests::TutorialMesh, true);
	if (Rig == nullptr)
	{
		return false;
	}
	// The solver stack's "Add New Solver", then the solver's Details.
	UIKRigController* Controller = UIKRigController::GetController(Rig);
	const int32 Index = Controller->AddSolver(SolverType);
	FIKRigSolverBase* Solver = Index != INDEX_NONE ? Controller->GetSolverAtIndex(Index) : nullptr;
	if (!TestNotNull(TEXT("solver added"), Solver)
		|| !SetSettings(*this, *Rig, Solver->GetSolverSettingsType(), Solver->GetSolverSettings(), 0.5f)
		|| !TestTrue(TEXT("saved"), AssetDocumentIKRigTests::Save(*Rig)))
	{
		return false;
	}
	const Tolerant::FTarget Target = View(Fixture);
	const FString Settings = FString::Printf(TEXT("/Body/Solvers/%d/Settings"), Index);
	const FString Marker = Settings + TEXT("/Filter");

	Tolerant::FExpected Expected;
	Expected.Warnings = {{TEXT("IKRig.Unrepresented"), Marker}};
	Expected.Markers = {Marker};
	if (!Tolerant::TestTolerantRoundTrip(*this, Target, Expected, TEXT("IK Rig")).IsValid())
	{
		return false;
	}

	auto Current = [&Target, Index]() -> FIKRigSolverBase*
	{
		UIKRigDefinition* Asset = Target.Get<UIKRigDefinition>();
		return Asset != nullptr ? UIKRigController::GetController(Asset)->GetSolverAtIndex(Index) : nullptr;
	};
	// Another settings field changes; the marker is left as read, then omitted.
	const TPair<double, bool> Edits[] = {{0.25, false}, {0.75, true}};
	for (const TPair<double, bool>& Edit : Edits)
	{
		const FString Stage = Edit.Value ? TEXT("Edit, marker omitted") : TEXT("Edit, marker as read");
		if (TestTrue(*FString::Printf(TEXT("%s: applies"), *Stage), Tolerant::ApplyEdited(*this, Target, [&Settings, &Edit](const TSharedPtr<FJsonObject>& Document)
		{
			const TSharedPtr<FJsonObject> Object = SettingsAt(Document, Settings);
			Object->SetNumberField(TEXT("Alpha"), Edit.Key);
			if (Edit.Value)
			{
				Object->RemoveField(TEXT("Filter"));
			}
		}, Stage).IsSuccess()))
		{
			FIKRigSolverBase* Edited = Current();
			TestEqual(*FString::Printf(TEXT("%s: Alpha written"), *Stage), Edited != nullptr ? AlphaOf(Edited->GetSolverSettingsType(), Edited->GetSolverSettings()) : -1.f,
				static_cast<float>(Edit.Key));
			TestTrue(*FString::Printf(TEXT("%s: Filter kept"), *Stage), Edited != nullptr && FilterKept(Edited->GetSolverSettingsType(), Edited->GetSolverSettings()));
			Tolerant::TestStillShows(*this, Target, TEXT("IKRig.Unrepresented"), Marker, Stage);
		}
	}

	Tolerant::TestRefused(*this, Tolerant::ApplyEdited(*this, Target, [&Settings](const TSharedPtr<FJsonObject>& Document)
	{
		SettingsAt(Document, Settings)->GetObjectField(TEXT("Filter"))->SetStringField(TEXT("Text"), TEXT("/Script/IKRig.IKRigEffectorGoal:RotationAlpha"));
	}, TEXT("Edited marker")), TEXT("IKRig.UnrepresentedValue"), Marker, TEXT("Edited marker"));
	Tolerant::TestRefused(*this, Tolerant::ApplyEdited(*this, Target, [&Settings](const TSharedPtr<FJsonObject>& Document)
	{
		SettingsAt(Document, Settings)->SetStringField(TEXT("Filter"), TEXT("/Script/IKRig.IKRigEffectorGoal:RotationAlpha"));
	}, TEXT("A value for the marker")), TEXT("IKRig.UnrepresentedValue"), Marker, TEXT("A value for the marker"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantIKRetargeterTest, "UEAssetDoc.Tolerant.IKRetargeter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantIKRetargeterTest::RunTest(const FString& Parameters)
{
	namespace Tolerant = AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentTolerantIKRigTestsLocal;
	UScriptStruct* OpType = FindObject<UScriptStruct>(nullptr, OpPath);
	if (!TestNotNull(TEXT("the test op type is loaded"), OpType))
	{
		return false;
	}
	AssetDocumentIKRigTests::FFixtures Fixtures;
	UIKRigDefinition* Rig = AssetDocumentIKRigTests::MakeRig(*this, Fixtures.Add(TEXT("IK_TolerantRig")), AssetDocumentIKRigTests::TutorialMesh, true);
	const AssetDocumentIKRigTests::FTarget& Fixture = Fixtures.Add(TEXT("RTG_Tolerant"));
	UIKRetargeter* Retargeter = Rig != nullptr ? AssetDocumentIKRigTests::MakeRetargeter(*this, Fixture, Rig, Rig) : nullptr;
	if (Retargeter == nullptr)
	{
		return false;
	}
	// The op stack's "Add New", then the op's Details.
	const UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
	const int32 Added = Controller->AddRetargetOp(OpType);
	FIKRetargetOpBase* Op = Added != INDEX_NONE ? Controller->GetRetargetOpByIndex(Added) : nullptr;
	if (!TestNotNull(TEXT("op added"), Op)
		|| !SetSettings(*this, *Retargeter, Op->GetSettingsType(), Op->GetSettings(), 0.5f)
		|| !TestTrue(TEXT("saved"), AssetDocumentIKRigTests::Save(*Retargeter)))
	{
		return false;
	}
	const FName OpName = Op->GetName();
	const Tolerant::FTarget Target = View(Fixture);
	const FAssetDocumentResult First = Tolerant::ExtractResult(Target);
	const int32 Index = First.Payload.IsValid() ? TopLevelOpIndex(First.Payload, OpName.ToString()) : INDEX_NONE;
	if (!TestTrue(TEXT("the op is a top-level op of the extract"), Index != INDEX_NONE))
	{
		Tolerant::LogResult(*this, TEXT("Extract"), First);
		return false;
	}
	const FString Settings = FString::Printf(TEXT("/Body/Ops/%d/Settings"), Index);
	const FString Marker = Settings + TEXT("/Filter");

	Tolerant::FExpected Expected;
	Expected.Warnings = {{TEXT("IKRetargeter.Unrepresented"), Marker}};
	Expected.Markers = {Marker};
	if (!Tolerant::TestTolerantRoundTrip(*this, Target, Expected, TEXT("IK Retargeter")).IsValid())
	{
		return false;
	}

	auto Current = [&Target, OpName]() -> FIKRetargetOpBase*
	{
		UIKRetargeter* Asset = Target.Get<UIKRetargeter>();
		const UIKRetargeterController* Edited = Asset != nullptr ? UIKRetargeterController::GetController(Asset) : nullptr;
		return Edited != nullptr ? Edited->GetRetargetOpByIndex(Edited->GetIndexOfOpByName(OpName)) : nullptr;
	};
	// Another settings field changes; the marker is left as read, then omitted.
	const TPair<double, bool> Edits[] = {{0.25, false}, {0.75, true}};
	for (const TPair<double, bool>& Edit : Edits)
	{
		const FString Stage = Edit.Value ? TEXT("Edit, marker omitted") : TEXT("Edit, marker as read");
		if (TestTrue(*FString::Printf(TEXT("%s: applies"), *Stage), Tolerant::ApplyEdited(*this, Target, [&Settings, &Edit](const TSharedPtr<FJsonObject>& Document)
		{
			const TSharedPtr<FJsonObject> Object = SettingsAt(Document, Settings);
			Object->SetNumberField(TEXT("Alpha"), Edit.Key);
			if (Edit.Value)
			{
				Object->RemoveField(TEXT("Filter"));
			}
		}, Stage).IsSuccess()))
		{
			FIKRetargetOpBase* Edited = Current();
			TestEqual(*FString::Printf(TEXT("%s: Alpha written"), *Stage), Edited != nullptr ? AlphaOf(Edited->GetSettingsType(), Edited->GetSettings()) : -1.f,
				static_cast<float>(Edit.Key));
			TestTrue(*FString::Printf(TEXT("%s: Filter kept"), *Stage), Edited != nullptr && FilterKept(Edited->GetSettingsType(), Edited->GetSettings()));
			Tolerant::TestStillShows(*this, Target, TEXT("IKRetargeter.Unrepresented"), Marker, Stage);
		}
	}

	Tolerant::TestRefused(*this, Tolerant::ApplyEdited(*this, Target, [&Settings](const TSharedPtr<FJsonObject>& Document)
	{
		SettingsAt(Document, Settings)->GetObjectField(TEXT("Filter"))->SetStringField(TEXT("Text"), TEXT("/Script/IKRig.IKRigEffectorGoal:RotationAlpha"));
	}, TEXT("Edited marker")), TEXT("IKRetargeter.UnrepresentedValue"), Marker, TEXT("Edited marker"));
	return !HasAnyErrors();
}

#endif
