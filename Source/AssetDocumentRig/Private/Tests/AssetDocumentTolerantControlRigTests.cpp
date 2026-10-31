// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// A Control Rig whose hierarchy holds a Reference element (made by script, as add_reference does): it reads, its own
// extract re-applies as a no-op, another hierarchy edit works and keeps the Reference, and a new Reference is still
// refused.

#include "AssetDocumentIKRigTestSupport.h"
#include "AssetDocumentTolerantTestSupport.h"

#include "ControlRigBlueprintLegacy.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentControlRigTests
{
UControlRigBlueprint* MakeControlRig(FAutomationTestBase& Test, const AssetDocumentIKRigTests::FTarget& Target, const TCHAR* MeshPath, bool bModular);
} // namespace AssetDocumentControlRigTests

namespace AssetDocumentTolerantControlRigTestsLocal
{
// The tolerant-test view of a Control Rig fixture (same asset, package and sidecar).
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

TArray<TSharedPtr<FJsonValue>> HierarchyRows(const TSharedPtr<FJsonObject>& Document)
{
	return Document->GetObjectField(TEXT("Body"))->GetArrayField(TEXT("Hierarchy"));
}

int32 IndexOf(const TArray<TSharedPtr<FJsonValue>>& Elements, const TCHAR* Type, const TCHAR* Name)
{
	return Elements.IndexOfByPredicate([Type, Name](const TSharedPtr<FJsonValue>& Element)
	{
		return Element->AsObject()->GetStringField(TEXT("Type")) == Type && Element->AsObject()->GetStringField(TEXT("Name")) == Name;
	});
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantControlRigReferenceTest, "UEAssetDoc.Tolerant.ControlRigReference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantControlRigReferenceTest::RunTest(const FString& Parameters)
{
	namespace Tolerant = AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentTolerantControlRigTestsLocal;
	AssetDocumentIKRigTests::FFixtures Fixtures;
	const AssetDocumentIKRigTests::FTarget& Rig = Fixtures.Add(TEXT("CR_Tolerant"));
	UControlRigBlueprint* Blueprint = AssetDocumentControlRigTests::MakeControlRig(*this, Rig, AssetDocumentIKRigTests::TutorialMesh, false);
	if (Blueprint == nullptr)
	{
		return false;
	}
	// A script's add_reference under the root bone.
	URigHierarchyController* Elements = Blueprint->GetHierarchy()->GetController(true);
	const FRigElementKey Reference = Elements->AddReference(TEXT("socket_ref"), FRigElementKey(TEXT("root"), ERigElementType::Bone),
		FRigReferenceGetWorldTransformDelegate(), false);
	if (!TestTrue(TEXT("reference added"), Reference.IsValid()) || !TestTrue(TEXT("saved"), AssetDocumentIKRigTests::Save(*Blueprint)))
	{
		return false;
	}
	const Tolerant::FTarget Target = View(Rig);

	const TSharedPtr<FJsonObject> Extracted = Tolerant::TestTolerantRoundTrip(*this, Target, Tolerant::FExpected(), TEXT("Control Rig"));
	if (!Extracted.IsValid() || !TestTrue(TEXT("the Reference is extracted"), IndexOf(HierarchyRows(Extracted), TEXT("Reference"), TEXT("socket_ref")) != INDEX_NONE))
	{
		return false;
	}

	if (TestTrue(TEXT("a null is added"), Tolerant::ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		TArray<TSharedPtr<FJsonValue>> Elements = HierarchyRows(Document);
		const TSharedPtr<FJsonObject> Null = MakeShared<FJsonObject>();
		Null->SetStringField(TEXT("Type"), TEXT("Null"));
		Null->SetStringField(TEXT("Name"), TEXT("tolerant_null"));
		// Under the Reference's parent bone, as the hierarchy spells it.
		Null->SetStringField(TEXT("Parent"), Elements[IndexOf(Elements, TEXT("Reference"), TEXT("socket_ref"))]->AsObject()->GetStringField(TEXT("Parent")));
		Elements.Add(MakeShared<FJsonValueObject>(Null));
		Document->GetObjectField(TEXT("Body"))->SetArrayField(TEXT("Hierarchy"), Elements);
	}, TEXT("Edit")).IsSuccess()))
	{
		const TSharedPtr<FJsonObject> After = Tolerant::ExtractResult(Target).Payload;
		TestTrue(TEXT("the null"), After.IsValid() && IndexOf(HierarchyRows(After), TEXT("Null"), TEXT("tolerant_null")) != INDEX_NONE);
		TestTrue(TEXT("the Reference kept"), After.IsValid() && IndexOf(HierarchyRows(After), TEXT("Reference"), TEXT("socket_ref")) != INDEX_NONE);
	}

	int32 NewIndex = INDEX_NONE;
	const FAssetDocumentResult Refused = Tolerant::ApplyEdited(*this, Target, [&NewIndex](const TSharedPtr<FJsonObject>& Document)
	{
		TArray<TSharedPtr<FJsonValue>> Elements = HierarchyRows(Document);
		const int32 Existing = IndexOf(Elements, TEXT("Reference"), TEXT("socket_ref"));
		const TSharedPtr<FJsonObject> Copy = Tolerant::Parse(Tolerant::Serialize(Elements[Existing]->AsObject()));
		Copy->SetStringField(TEXT("Name"), TEXT("new_ref"));
		NewIndex = Elements.Add(MakeShared<FJsonValueObject>(Copy));
		Document->GetObjectField(TEXT("Body"))->SetArrayField(TEXT("Hierarchy"), Elements);
	}, TEXT("New Reference"));
	Tolerant::TestRefused(*this, Refused, TEXT("ControlRig.ProceduralElement"), FString::Printf(TEXT("/Body/Hierarchy/%d/Type"), NewIndex),
		TEXT("New Reference"));
	return !HasAnyErrors();
}

#endif
