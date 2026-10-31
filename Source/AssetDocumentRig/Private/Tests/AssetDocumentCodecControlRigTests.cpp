// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// A Control Rig whose Influence Map (Class Settings) has a Forwards Solve map with two entries, filled as the Influence
// Map tab does: Extract shows it in top-level Properties, the extract re-applies as a no-op, an affected key added to
// an entry is written, and a map whose KeyToIndex does not index its own entries is refused.

#include "AssetDocumentIKRigTestSupport.h"
#include "AssetDocumentTolerantTestSupport.h"

#include "ControlRigBlueprintLegacy.h"
#include "Rigs/RigHierarchyDefines.h"
#include "Rigs/RigInfluenceMap.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentControlRigTests
{
UControlRigBlueprint* MakeControlRig(FAutomationTestBase& Test, const AssetDocumentIKRigTests::FTarget& Target, const TCHAR* MeshPath, bool bModular);
} // namespace AssetDocumentControlRigTests

namespace
{
// The event the Influence Map tab lists first (FRigUnit_BeginExecution::EventName).
const TCHAR* const ControlRigInfluenceEvent = TEXT("Forwards Solve");

// The tolerant-test view of a Control Rig fixture (same asset, package and sidecar).
AssetDocumentTolerantTestSupport::FTarget ControlRigInfluenceView(const AssetDocumentIKRigTests::FTarget& Target)
{
	AssetDocumentTolerantTestSupport::FTarget Out(TEXT("Unused"));
	Out.AssetName = Target.AssetName;
	Out.PackageName = Target.PackageName;
	Out.ObjectPath = Target.ObjectPath;
	Out.PackageFile = Target.PackageFile;
	Out.SidecarPath = Target.SidecarPath;
	return Out;
}

FRigElementKey ControlRigInfluenceBone(const TCHAR* Name)
{
	return FRigElementKey(FName(Name), ERigElementType::Bone);
}

// The document's Influence Map (Properties.Influences), or null.
TSharedPtr<FJsonObject> ControlRigInfluenceMaps(const TSharedPtr<FJsonObject>& Document)
{
	const TSharedPtr<FJsonValue> Value = AssetDocumentTolerantTestSupport::At(Document, TEXT("/Properties/Influences"));
	return Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
}

// The entries of the document's first map, or null.
const TArray<TSharedPtr<FJsonValue>>* ControlRigInfluenceEntries(const TSharedPtr<FJsonObject>& Document)
{
	const TSharedPtr<FJsonValue> Value = AssetDocumentTolerantTestSupport::At(Document, TEXT("/Properties/Influences/Maps/0/Entries"));
	return Value.IsValid() && Value->Type == EJson::Array ? &Value->AsArray() : nullptr;
}

// The keys the rig's entry of Source affects in the Forwards Solve map, or INDEX_NONE when it has no such entry.
int32 ControlRigInfluenceAffectedCount(const AssetDocumentIKRigTests::FTarget& Target, const FRigElementKey& Source)
{
	UControlRigBlueprint* Blueprint = Cast<UControlRigBlueprint>(Target.Get());
	const FRigInfluenceMap* Map = Blueprint != nullptr ? Blueprint->GetInfluences().Find(ControlRigInfluenceEvent) : nullptr;
	const FRigInfluenceEntry* Entry = Map != nullptr ? Map->Find(Source) : nullptr;
	return Entry != nullptr ? Entry->Num() : INDEX_NONE;
}

bool ControlRigInfluenceAffects(const AssetDocumentIKRigTests::FTarget& Target, const FRigElementKey& Source, const FRigElementKey& Affected)
{
	UControlRigBlueprint* Blueprint = Cast<UControlRigBlueprint>(Target.Get());
	const FRigInfluenceMap* Map = Blueprint != nullptr ? Blueprint->GetInfluences().Find(ControlRigInfluenceEvent) : nullptr;
	const FRigInfluenceEntry* Entry = Map != nullptr ? Map->Find(Source) : nullptr;
	return Entry != nullptr && Entry->Contains(Affected);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentCodecControlRigInfluenceMapTest, "UEAssetDoc.Codec.ControlRig.InfluenceMap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentCodecControlRigInfluenceMapTest::RunTest(const FString& Parameters)
{
	namespace Tolerant = AssetDocumentTolerantTestSupport;
	AssetDocumentIKRigTests::FFixtures Fixtures;
	const AssetDocumentIKRigTests::FTarget& Rig = Fixtures.Add(TEXT("CR_InfluenceMap"));
	UControlRigBlueprint* Blueprint = AssetDocumentControlRigTests::MakeControlRig(*this, Rig, AssetDocumentIKRigTests::TutorialMesh, false);
	if (Blueprint == nullptr)
	{
		return false;
	}
	const FRigElementKey Root = ControlRigInfluenceBone(TEXT("root"));
	const FRigElementKey Pelvis = ControlRigInfluenceBone(TEXT("pelvis"));
	const FRigElementKey Spine01 = ControlRigInfluenceBone(TEXT("spine_01"));
	const FRigElementKey Spine02 = ControlRigInfluenceBone(TEXT("spine_02"));
	// The Influence Map tab: the Forwards Solve map, root affects pelvis and spine_01 affects spine_02.
	static_cast<UBlueprint*>(Blueprint)->Modify();
	FRigInfluenceMap& Map = Blueprint->GetInfluences().FindOrAdd(ControlRigInfluenceEvent);
	Map.FindOrAdd(Root).AddUnique(Pelvis);
	Map.FindOrAdd(Spine01).AddUnique(Spine02);
	if (!TestTrue(TEXT("saved"), AssetDocumentIKRigTests::Save(*Blueprint)))
	{
		return false;
	}
	const Tolerant::FTarget Target = ControlRigInfluenceView(Rig);

	// Extract shows the map; the extract re-applies as a no-op.
	const TSharedPtr<FJsonObject> Extracted = Tolerant::TestTolerantRoundTrip(*this, Target, Tolerant::FExpected(), TEXT("Control Rig"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Influences = ControlRigInfluenceMaps(Extracted);
	const TArray<TSharedPtr<FJsonValue>>* Entries = ControlRigInfluenceEntries(Extracted);
	if (!TestNotNull(TEXT("Properties.Influences is extracted"), Influences.Get()) || !TestNotNull(TEXT("its first map's entries"), Entries))
	{
		return false;
	}
	const TSharedPtr<FJsonValue> EventNameValue = Tolerant::At(Extracted, TEXT("/Properties/Influences/Maps/0/EventName"));
	FString EventName;
	TestTrue(TEXT("the map is the Forwards Solve map"), EventNameValue.IsValid() && EventNameValue->TryGetString(EventName)
		&& EventName == ControlRigInfluenceEvent);
	const TSharedPtr<FJsonValue> MapsValue = Tolerant::At(Extracted, TEXT("/Properties/Influences/Maps"));
	TestTrue(TEXT("one map"), MapsValue.IsValid() && MapsValue->Type == EJson::Array && MapsValue->AsArray().Num() == 1);
	TestEqual(TEXT("two entries"), Entries->Num(), 2);
	const TSharedPtr<FJsonValue> EventIndex = Tolerant::At(Extracted, FString(TEXT("/Properties/Influences/EventToIndex/")) + ControlRigInfluenceEvent);
	int32 EventPosition = INDEX_NONE;
	TestTrue(TEXT("EventToIndex names the map at 0"), EventIndex.IsValid() && EventIndex->TryGetNumber(EventPosition) && EventPosition == 0);
	const TSharedPtr<FJsonValue> KeyToIndex = Tolerant::At(Extracted, TEXT("/Properties/Influences/Maps/0/KeyToIndex"));
	TestTrue(TEXT("KeyToIndex lists both entries"), KeyToIndex.IsValid() && KeyToIndex->Type == EJson::Object
		&& KeyToIndex->AsObject()->Values.Num() == 2);

	// The first entry (root) also affects what the second (spine_01) affects.
	if (TestTrue(TEXT("an affected key is added"), Tolerant::ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TArray<TSharedPtr<FJsonValue>>* Rows = ControlRigInfluenceEntries(Document);
		if (Rows == nullptr || Rows->Num() != 2)
		{
			return;
		}
		const TSharedPtr<FJsonObject> First = (*Rows)[0]->AsObject();
		TArray<TSharedPtr<FJsonValue>> Affected = First->GetArrayField(TEXT("AffectedList"));
		Affected.Add((*Rows)[1]->AsObject()->GetArrayField(TEXT("AffectedList"))[0]);
		First->SetArrayField(TEXT("AffectedList"), Affected);
	}, TEXT("Add an affected key")).IsSuccess()))
	{
		TestTrue(TEXT("root affects spine_02"), ControlRigInfluenceAffects(Rig, Root, Spine02));
		TestTrue(TEXT("root still affects pelvis"), ControlRigInfluenceAffects(Rig, Root, Pelvis));
		TestEqual(TEXT("root affects two keys"), ControlRigInfluenceAffectedCount(Rig, Root), 2);
		TestTrue(TEXT("spine_01 still affects spine_02"), ControlRigInfluenceAffects(Rig, Spine01, Spine02));
		const FAssetDocumentResult Reread = Tolerant::ExtractResult(Target);
		const TArray<TSharedPtr<FJsonValue>>* After = Reread.IsSuccess() ? ControlRigInfluenceEntries(Reread.Payload) : nullptr;
		TestTrue(TEXT("the added key reads back"), After != nullptr && After->Num() == 2
			&& (*After)[0]->AsObject()->GetArrayField(TEXT("AffectedList")).Num() == 2);
	}

	// A KeyToIndex that names a position past the entries.
	const FAssetDocumentResult Refused = Tolerant::ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonValue> Value = Tolerant::At(Document, TEXT("/Properties/Influences/Maps/0/KeyToIndex"));
		if (Value.IsValid() && Value->Type == EJson::Object)
		{
			auto First = Value->AsObject()->Values.CreateIterator();
			if (First)
			{
				First.Value() = MakeShared<FJsonValueNumberString>(FString(TEXT("5")));
			}
		}
	}, TEXT("Inconsistent KeyToIndex"));
	Tolerant::TestRefused(*this, Refused, TEXT("ControlRig.InvalidInfluences"), TEXT("/Properties/Influences/Maps/0/KeyToIndex"),
		TEXT("Inconsistent KeyToIndex"));
	TestEqual(TEXT("the refused map leaves the rig's entries"), ControlRigInfluenceAffectedCount(Rig, Root), 2);
	return !HasAnyErrors();
}

#endif
