// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Runtime/AssetDocumentPackageSnapshot.h"

#include "AssetDocumentPackageSnapshotTestTypes.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Serialization/ObjectWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectHash.h"
#include "Utils/AssetDocumentEngineCompat.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentPackageSnapshotTests
{
using FNode = UAssetDocumentPackageSnapshotTestNode;
using FAsset = UAssetDocumentPackageSnapshotTestAsset;

struct FFixture
{
	UPackage* Package = nullptr;
	FAsset* Asset = nullptr;
	FNode* First = nullptr;
	FNode* Second = nullptr;
	FNode* Nested = nullptr;
	FString PackageName;

	~FFixture()
	{
		// Resolve by name: a reload fallback replaces the package object.
		TArray<UObject*> Objects;
		if (UPackage* Current = FindPackage(nullptr, *PackageName))
		{
			GetObjectsWithOuter(Current, Objects);
			for (UObject* Object : Objects)
			{
				Object->ClearFlags(RF_Public | RF_Standalone);
			}
			Current->SetDirtyFlag(false);
		}
	}
};

void MakeFixture(FFixture& Fixture, const TCHAR* Stem)
{
	Fixture.PackageName = FString::Printf(
		TEXT("/Game/AssetDocumentTests/PackageSnapshot/%s_%s"),
		Stem, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	Fixture.Package = CreatePackage(*Fixture.PackageName);
	Fixture.Asset = NewObject<FAsset>(Fixture.Package, *FPackageName::GetShortName(Fixture.PackageName),
		RF_Public | RF_Standalone | RF_Transactional);
	Fixture.Asset->Title = TEXT("Original title");
	Fixture.Asset->Numbers = {1, 2, 3};
	Fixture.First = NewObject<FNode>(Fixture.Asset, TEXT("First"), RF_Transactional);
	Fixture.First->Label = TEXT("first");
	Fixture.First->Value = 10;
	Fixture.Second = NewObject<FNode>(Fixture.Asset, TEXT("Second"), RF_Transactional);
	Fixture.Second->Label = TEXT("second");
	Fixture.Second->Value = 20;
	Fixture.Nested = NewObject<FNode>(Fixture.First, TEXT("Nested"), RF_Transactional);
	Fixture.Nested->Label = TEXT("nested");
	Fixture.First->Child = Fixture.Nested;
	Fixture.Asset->Nodes = {Fixture.First, Fixture.Second};
	Fixture.Package->SetDirtyFlag(false);
}

TArray<uint8> SerializeForTest(UObject& Object)
{
	TArray<uint8> Bytes;
	FObjectWriter Writer(&Object, Bytes, false, false, false, PPF_DuplicateVerbatim);
	return Bytes;
}

struct FPackageState
{
	TMap<UObject*, TArray<uint8>> Bytes;
	TMap<UObject*, FString> Paths;
};

FPackageState CapturePackageState(UPackage& Package)
{
	FPackageState State;
	TArray<UObject*> Objects;
	GetObjectsWithOuter(&Package, Objects, AssetDocumentObjects::IncludeNested, RF_NoFlags, EInternalObjectFlags::Garbage);
	for (UObject* Object : Objects)
	{
		State.Bytes.Add(Object, SerializeForTest(*Object));
		State.Paths.Add(Object, Object->GetPathName());
	}
	return State;
}

void TestPackageStateEqual(FAutomationTestBase& Test, const TCHAR* Label, const FPackageState& Before, UPackage& Package)
{
	const FPackageState After = CapturePackageState(Package);
	Test.TestEqual(FString::Printf(TEXT("%s keeps the object count"), Label), After.Bytes.Num(), Before.Bytes.Num());
	for (const TPair<UObject*, TArray<uint8>>& Entry : Before.Bytes)
	{
		const TArray<uint8>* AfterBytes = After.Bytes.Find(Entry.Key);
		if (!Test.TestNotNull(FString::Printf(TEXT("%s keeps %s"), Label, *Before.Paths[Entry.Key]), AfterBytes))
		{
			continue;
		}
		Test.TestTrue(FString::Printf(TEXT("%s restores bytes of %s"), Label, *Before.Paths[Entry.Key]),
			*AfterBytes == Entry.Value);
		Test.TestEqual(FString::Printf(TEXT("%s restores path of %s"), Label, *Before.Paths[Entry.Key]),
			After.Paths[Entry.Key], Before.Paths[Entry.Key]);
	}
}

class FTestRebuilder final : public IAssetDocumentDerivedStateRebuilder
{
public:
	virtual FName GetId() const override { return TEXT("Test.PackageSnapshot.Rebuilder"); }
	virtual bool Supports(const UObject& PrimaryAsset) const override { return PrimaryAsset.IsA<FAsset>(); }
	virtual EAssetDocumentSnapshotObjectRole Classify(const UObject& Object) const override
	{
		const FNode* Node = Cast<FNode>(&Object);
		return Node && Node->Label.StartsWith(TEXT("derived"))
			? EAssetDocumentSnapshotObjectRole::Derived
			: IAssetDocumentDerivedStateRebuilder::Classify(Object);
	}
	virtual FAssetDocumentContractValidationResult Rebuild(UObject& PrimaryAsset) override
	{
		++RebuildCount;
		ObservedTitle = CastChecked<FAsset>(&PrimaryAsset)->Title;
		FAssetDocumentContractValidationResult Result;
		Result.bValid = true;
		return Result;
	}

	int32 RebuildCount = 0;
	FString ObservedTitle;
};

bool HasDiagnostic(const FAssetDocumentContractValidationResult& Result, const TCHAR* Code)
{
	return Result.Diagnostics.ContainsByPredicate([Code](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == Code;
	});
}
} // namespace AssetDocumentPackageSnapshotTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentPackageSnapshotRestoreTest,
	"UEAssetDoc.PackageSnapshot.RestoresPropertiesArraysAndSubobjects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPackageSnapshotRestoreTest::RunTest(const FString&)
{
	using namespace AssetDocumentPackageSnapshotTests;
	FFixture Fixture;
	MakeFixture(Fixture, TEXT("Restore"));
	const FPackageState Before = CapturePackageState(*Fixture.Package);

	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	TUniquePtr<FAssetDocumentPackageSnapshot> Snapshot =
		FAssetDocumentPackageSnapshot::Capture(*Fixture.Package, Diagnostics);
	if (!TestNotNull(TEXT("snapshot captures"), Snapshot.Get()))
	{
		return false;
	}
	TestEqual(TEXT("snapshot covers asset and three nodes"), Snapshot->NumAuthoredObjects(), 4);

	// Property and array edits.
	Fixture.Asset->Title = TEXT("Mutated title");
	Fixture.Asset->Numbers.RemoveAt(0);
	Fixture.Asset->Numbers.Add(99);
	Fixture.Second->Value = 21;
	// Add a subobject (with its own nested child) and reference it.
	FNode* Added = NewObject<FNode>(Fixture.Asset, TEXT("Added"), RF_Transactional);
	FNode* AddedChild = NewObject<FNode>(Added, TEXT("AddedChild"), RF_Transactional);
	Added->Child = AddedChild;
	Fixture.Asset->Nodes.Add(Added);
	// Remove a subobject the way editor code deletes it.
	Fixture.Asset->Nodes.Remove(Fixture.Second);
	Fixture.Second->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
	Fixture.Second->MarkAsGarbage();
	// Rename and re-parent subobjects.
	Fixture.First->Rename(TEXT("FirstRenamed"), nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
	Fixture.Nested->Rename(nullptr, Fixture.Asset, REN_DontCreateRedirectors | REN_NonTransactional);
	Fixture.Package->SetDirtyFlag(true);

	const FAssetDocumentContractValidationResult Restored = Snapshot->Restore();
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Restored.Diagnostics)
	{
		AddInfo(Diagnostic.Code + TEXT(": ") + Diagnostic.Message);
	}
	TestTrue(TEXT("restore succeeds"), Restored.bValid);
	TestTrue(TEXT("restore needs no diagnostics"), Restored.Diagnostics.IsEmpty());
	TestPackageStateEqual(*this, TEXT("restore"), Before, *Fixture.Package);
	TestEqual(TEXT("title restored"), Fixture.Asset->Title, FString(TEXT("Original title")));
	TestTrue(TEXT("array restored"), Fixture.Asset->Numbers == TArray<int32>({1, 2, 3}));
	TestTrue(TEXT("node list restored"),
		Fixture.Asset->Nodes == TArray<TObjectPtr<FNode>>({Fixture.First, Fixture.Second}));
	TestFalse(TEXT("removed node is alive again"), Fixture.Second->HasAnyInternalFlags(EInternalObjectFlags::Garbage));
	TestTrue(TEXT("added node is detached"), Added->GetPackage() == GetTransientPackage());
	TestTrue(TEXT("added node is garbage"), Added->HasAnyInternalFlags(EInternalObjectFlags::Garbage));
	TestTrue(TEXT("added nested node is garbage"), AddedChild->HasAnyInternalFlags(EInternalObjectFlags::Garbage));
	TestFalse(TEXT("clean dirty flag is restored"), Fixture.Package->IsDirty());
	TestEqual(TEXT("changed asset receives exactly one PostEditUndo"), Fixture.Asset->PostEditUndoCount, 1);

	const FAssetDocumentContractValidationResult Again = Snapshot->Restore();
	TestFalse(TEXT("snapshot is single-use"), Again.bValid);
	TestTrue(TEXT("second restore reports reuse"), HasDiagnostic(Again, TEXT("PackageSnapshot.AlreadyRestored")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentPackageSnapshotNewGraphNodeLinksTest,
	"UEAssetDoc.PackageSnapshot.UnlinksNewGraphNodesFromSurvivingNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPackageSnapshotNewGraphNodeLinksTest::RunTest(const FString&)
{
	using namespace AssetDocumentPackageSnapshotTests;
	FFixture Fixture;
	MakeFixture(Fixture, TEXT("GraphLinks"));
	UEdGraph* Graph = NewObject<UEdGraph>(Fixture.Asset, TEXT("Graph"), RF_Transactional);
	UEdGraphNode* Existing = NewObject<UEdGraphNode>(Graph, TEXT("Existing"), RF_Transactional);
	Graph->Nodes.Add(Existing);
	UEdGraphPin* ExistingOut = Existing->CreatePin(EGPD_Output, TEXT("exec"), TEXT("Then"));
	Fixture.Package->SetDirtyFlag(false);

	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	TUniquePtr<FAssetDocumentPackageSnapshot> Snapshot =
		FAssetDocumentPackageSnapshot::Capture(*Fixture.Package, Diagnostics);
	if (!TestNotNull(TEXT("snapshot captures"), Snapshot.Get()))
	{
		return false;
	}
	// A failed apply wires a new node to the surviving one.
	UEdGraphNode* Added = NewObject<UEdGraphNode>(Graph, TEXT("Added"), RF_Transactional);
	Graph->Nodes.Add(Added);
	UEdGraphPin* AddedIn = Added->CreatePin(EGPD_Input, TEXT("exec"), TEXT("Execute"));
	ExistingOut->MakeLinkTo(AddedIn);
	Fixture.Package->SetDirtyFlag(true);

	const FAssetDocumentContractValidationResult Restored = Snapshot->Restore();
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Restored.Diagnostics)
	{
		AddInfo(Diagnostic.Code + TEXT(": ") + Diagnostic.Message);
	}
	TestTrue(TEXT("restore succeeds"), Restored.bValid);
	TestTrue(TEXT("added node is garbage"), Added->HasAnyInternalFlags(EInternalObjectFlags::Garbage));
	TestTrue(TEXT("discarded node keeps no link into the restored graph"), AddedIn->LinkedTo.IsEmpty());
	TestTrue(TEXT("surviving node has no link to the discarded node"),
		Existing->Pins.Num() == 1 && Existing->Pins[0]->LinkedTo.IsEmpty());
	TestTrue(TEXT("graph node list is restored"), Graph->Nodes == TArray<TObjectPtr<UEdGraphNode>>({Existing}));
	// Destroying the discarded node must not touch freed pins.
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentPackageSnapshotReallocatedPinsTest,
	"UEAssetDoc.PackageSnapshot.RestoresReallocatedGraphPins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPackageSnapshotReallocatedPinsTest::RunTest(const FString&)
{
	using namespace AssetDocumentPackageSnapshotTests;
	FFixture Fixture;
	MakeFixture(Fixture, TEXT("GraphPins"));
	UEdGraph* Graph = NewObject<UEdGraph>(Fixture.Asset, TEXT("Graph"), RF_Transactional);
	UEdGraphNode* Source = NewObject<UEdGraphNode>(Graph, TEXT("Source"), RF_Transactional);
	UEdGraphNode* Target = NewObject<UEdGraphNode>(Graph, TEXT("Target"), RF_Transactional);
	Graph->Nodes.Add(Source);
	Graph->Nodes.Add(Target);
	UEdGraphPin* SourceOut = Source->CreatePin(EGPD_Output, TEXT("exec"), TEXT("Then"));
	UEdGraphPin* TargetIn = Target->CreatePin(EGPD_Input, TEXT("exec"), TEXT("Execute"));
	SourceOut->MakeLinkTo(TargetIn);
	const FGuid CapturedId = SourceOut->PinId;
	Fixture.Package->SetDirtyFlag(false);

	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	TUniquePtr<FAssetDocumentPackageSnapshot> Snapshot =
		FAssetDocumentPackageSnapshot::Capture(*Fixture.Package, Diagnostics);
	if (!TestNotNull(TEXT("snapshot captures"), Snapshot.Get()))
	{
		return false;
	}
	// A failed apply reconstructs the source node the way editor nodes that do not keep pin ids do: a new pin, the
	// link moved onto it, the old pin destroyed.
	UEdGraphPin* Reallocated = Source->CreatePin(EGPD_Output, TEXT("exec"), TEXT("Then"));
	SourceOut->BreakAllPinLinks();
	Source->RemovePin(SourceOut);
	Reallocated->MakeLinkTo(TargetIn);
	Fixture.Package->SetDirtyFlag(true);
	TestNotEqual(TEXT("the reallocated pin has a new id"), Reallocated->PinId, CapturedId);

	const FAssetDocumentContractValidationResult Restored = Snapshot->Restore();
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Restored.Diagnostics)
	{
		AddInfo(Diagnostic.Code + TEXT(": ") + Diagnostic.Message);
	}
	TestTrue(TEXT("restore succeeds"), Restored.bValid);
	TestTrue(TEXT("the source has its captured pin back"), Source->Pins.Num() == 1 && Source->Pins[0]->PinId == CapturedId);
	TestTrue(TEXT("the target has one pin"), Target->Pins.Num() == 1);
	if (Source->Pins.Num() == 1 && Target->Pins.Num() == 1)
	{
		UEdGraphPin* Out = Source->Pins[0];
		UEdGraphPin* In = Target->Pins[0];
		TestTrue(TEXT("source -> target link"), Out->LinkedTo.Num() == 1 && Out->LinkedTo[0] == In);
		// (the target links only the restored pin, not the freed one)
		TestTrue(TEXT("target -> source link"), In->LinkedTo.Num() == 1 && In->LinkedTo[0] == Out);
	}
	TestFalse(TEXT("package is clean again"), Fixture.Package->IsDirty());
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentPackageSnapshotNameSwapTest,
	"UEAssetDoc.PackageSnapshot.RestoresSwappedNamesAndDirtyState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPackageSnapshotNameSwapTest::RunTest(const FString&)
{
	using namespace AssetDocumentPackageSnapshotTests;
	FFixture Fixture;
	MakeFixture(Fixture, TEXT("Swap"));
	Fixture.Package->SetDirtyFlag(true);
	const FPackageState Before = CapturePackageState(*Fixture.Package);
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	TUniquePtr<FAssetDocumentPackageSnapshot> Snapshot =
		FAssetDocumentPackageSnapshot::Capture(*Fixture.Package, Diagnostics);
	if (!TestNotNull(TEXT("snapshot captures"), Snapshot.Get()))
	{
		return false;
	}
	const ERenameFlags Flags = REN_DontCreateRedirectors | REN_NonTransactional;
	Fixture.First->Rename(TEXT("Swap"), nullptr, Flags);
	Fixture.Second->Rename(TEXT("First"), nullptr, Flags);
	Fixture.First->Rename(TEXT("Second"), nullptr, Flags);
	Fixture.Package->SetDirtyFlag(false);

	const FAssetDocumentContractValidationResult Restored = Snapshot->Restore();
	TestTrue(TEXT("swapped names restore"), Restored.bValid);
	TestPackageStateEqual(*this, TEXT("swap"), Before, *Fixture.Package);
	TestEqual(TEXT("first name restored"), Fixture.First->GetName(), FString(TEXT("First")));
	TestEqual(TEXT("second name restored"), Fixture.Second->GetName(), FString(TEXT("Second")));
	TestTrue(TEXT("dirty flag restored to dirty"), Fixture.Package->IsDirty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentPackageSnapshotUndoRebuiltSubobjectTest,
	"UEAssetDoc.PackageSnapshot.AcceptsSubobjectsRebuiltByUndo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPackageSnapshotUndoRebuiltSubobjectTest::RunTest(const FString&)
{
	using FRebuilding = UAssetDocumentPackageSnapshotRebuildingTestAsset;
	using namespace AssetDocumentPackageSnapshotTests;
	const FString PackageName = FString::Printf(TEXT("/Game/AssetDocumentTests/PackageSnapshot/UndoRebuild_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UPackage* Package = CreatePackage(*PackageName);
	FRebuilding* Asset = NewObject<FRebuilding>(Package, *FPackageName::GetShortName(PackageName),
		RF_Public | RF_Standalone | RF_Transactional);
	ON_SCOPE_EXIT
	{
		TArray<UObject*> Objects;
		GetObjectsWithOuter(Package, Objects);
		for (UObject* Object : Objects)
		{
			Object->ClearFlags(RF_Public | RF_Standalone);
		}
		Package->SetDirtyFlag(false);
	};
	Asset->Title = TEXT("Original");
	Asset->RebuildDerived();
	FNode* const OriginalDerived = Asset->Derived;
	Package->SetDirtyFlag(false);

	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	TUniquePtr<FAssetDocumentPackageSnapshot> Snapshot = FAssetDocumentPackageSnapshot::Capture(*Package, Diagnostics);
	if (!TestNotNull(TEXT("snapshot captures"), Snapshot.Get()))
	{
		return false;
	}
	// A failed apply edited the asset; its change callback replaced the subobject.
	Asset->Title = TEXT("Mutated");
	Asset->RebuildDerived();
	FNode* const ApplyDerived = Asset->Derived;
	Package->SetDirtyFlag(true);

	// The package is neither on disk nor clean: only the in-memory restore can succeed.
	const FAssetDocumentContractValidationResult Restored = Snapshot->Restore();
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Restored.Diagnostics)
	{
		AddInfo(Diagnostic.Code + TEXT(": ") + Diagnostic.Message);
	}
	TestTrue(TEXT("restore succeeds although PostEditUndo rebuilt the subobject"), Restored.bValid);
	TestFalse(TEXT("restore is not reported as failed"), HasDiagnostic(Restored, TEXT("PackageSnapshot.VerificationFailed")));
	TestEqual(TEXT("authored state restored"), Asset->Title, FString(TEXT("Original")));
	TestTrue(TEXT("the apply's subobject is removed"), ApplyDerived->HasAnyInternalFlags(EInternalObjectFlags::Garbage)
		&& ApplyDerived->GetPackage() == GetTransientPackage());
	TestTrue(TEXT("the asset references a live subobject rebuilt from the restored state"),
		IsValid(Asset->Derived) && Asset->Derived->IsIn(Asset) && Asset->Derived->Label == TEXT("Original"));
	TestTrue(TEXT("the captured subobject is kept intact"), IsValid(OriginalDerived) && OriginalDerived->IsIn(Asset));
	TestFalse(TEXT("clean dirty flag is restored"), Package->IsDirty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPackageSnapshotRestoreWithoutCallbacksTest,
	"UEAssetDoc.PackageSnapshot.RestoreWithoutEditorCallbacks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPackageSnapshotRestoreWithoutCallbacksTest::RunTest(const FString&)
{
	using FRebuilding = UAssetDocumentPackageSnapshotRebuildingTestAsset;
	using namespace AssetDocumentPackageSnapshotTests;
	const FString PackageName = FString::Printf(TEXT("/Game/AssetDocumentTests/PackageSnapshot/NoCallbacks_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UPackage* Package = CreatePackage(*PackageName);
	FRebuilding* Asset = NewObject<FRebuilding>(Package, *FPackageName::GetShortName(PackageName),
		RF_Public | RF_Standalone | RF_Transactional);
	ON_SCOPE_EXIT
	{
		TArray<UObject*> Objects;
		GetObjectsWithOuter(Package, Objects);
		for (UObject* Object : Objects)
		{
			Object->ClearFlags(RF_Public | RF_Standalone);
		}
		Package->SetDirtyFlag(false);
	};
	Asset->Title = TEXT("Original");
	Asset->RebuildDerived();
	FNode* const OriginalDerived = Asset->Derived;
	Package->SetDirtyFlag(false);

	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	TUniquePtr<FAssetDocumentPackageSnapshot> Snapshot = FAssetDocumentPackageSnapshot::Capture(*Package, Diagnostics);
	if (!TestNotNull(TEXT("snapshot captures"), Snapshot.Get()))
	{
		return false;
	}
	// A probe's side effect: the asset changed and created a new subobject.
	Asset->Title = TEXT("Mutated");
	Asset->RebuildDerived();
	FNode* const ProbeDerived = Asset->Derived;
	Package->SetDirtyFlag(true);

	const FAssetDocumentContractValidationResult Restored = Snapshot->RestoreWithoutEditorCallbacks();
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Restored.Diagnostics)
	{
		AddInfo(Diagnostic.Code + TEXT(": ") + Diagnostic.Message);
	}
	TestTrue(TEXT("restore succeeds"), Restored.bValid);
	TestEqual(TEXT("authored state restored"), Asset->Title, FString(TEXT("Original")));
	// No PostEditUndo: the asset's change callback did not run again, so the
	// captured subobject is referenced as it was and nothing new was created.
	TestTrue(TEXT("the captured subobject is referenced again"), Asset->Derived == OriginalDerived);
	TestTrue(TEXT("the probe's subobject is removed"), ProbeDerived->HasAnyInternalFlags(EInternalObjectFlags::Garbage)
		&& ProbeDerived->GetPackage() == GetTransientPackage());
	TArray<UObject*> Objects;
	GetObjectsWithOuter(Package, Objects, AssetDocumentObjects::IncludeNested, RF_NoFlags, EInternalObjectFlags::Garbage);
	TestEqual(TEXT("exactly the captured objects remain"), Objects.Num(), 2);
	TestFalse(TEXT("clean dirty flag is restored"), Package->IsDirty());
	TestFalse(TEXT("a snapshot restores once"), Snapshot->Restore().bValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentPackageSnapshotRebuilderTest,
	"UEAssetDoc.PackageSnapshot.DerivedStateRebuilder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPackageSnapshotRebuilderTest::RunTest(const FString&)
{
	using namespace AssetDocumentPackageSnapshotTests;
	TestTrue(TEXT("classes are derived by default"),
		FAssetDocumentPackageSnapshot::ClassifyDefault(*FAsset::StaticClass()) == EAssetDocumentSnapshotObjectRole::Derived);
	TestTrue(TEXT("class default objects are derived by default"),
		FAssetDocumentPackageSnapshot::ClassifyDefault(*GetMutableDefault<FAsset>()) == EAssetDocumentSnapshotObjectRole::Derived);

	TSharedRef<FTestRebuilder> Rebuilder = MakeShared<FTestRebuilder>();
	TestTrue(TEXT("rebuilder registers"), RegisterAssetDocumentDerivedStateRebuilder(Rebuilder));
	TestFalse(TEXT("duplicate rebuilder id is rejected"), RegisterAssetDocumentDerivedStateRebuilder(MakeShared<FTestRebuilder>()));
	ON_SCOPE_EXIT
	{
		TestTrue(TEXT("rebuilder unregisters"), UnregisterAssetDocumentDerivedStateRebuilder(Rebuilder->GetId()));
	};

	FFixture Fixture;
	MakeFixture(Fixture, TEXT("Rebuild"));
	TestTrue(TEXT("normal objects are authored by default"),
		FAssetDocumentPackageSnapshot::ClassifyDefault(*Fixture.First) == EAssetDocumentSnapshotObjectRole::Authored);
	TestTrue(TEXT("a package without derived objects is restorable"),
		FAssetDocumentPackageSnapshot::CanRestorePackage(*Fixture.Package));
	Fixture.Second->Label = TEXT("derived cache");
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	TUniquePtr<FAssetDocumentPackageSnapshot> Snapshot =
		FAssetDocumentPackageSnapshot::Capture(*Fixture.Package, Diagnostics);
	if (!TestNotNull(TEXT("snapshot captures"), Snapshot.Get()))
	{
		return false;
	}
	TestEqual(TEXT("rebuilder-derived node is not captured"), Snapshot->NumAuthoredObjects(), 3);

	Fixture.Asset->Title = TEXT("Mutated");
	Fixture.Second->Value = 77;
	FNode* NewDerived = NewObject<FNode>(Fixture.Asset, TEXT("NewDerived"));
	NewDerived->Label = TEXT("derived regenerated");
	const FAssetDocumentContractValidationResult Restored = Snapshot->Restore();
	TestTrue(TEXT("restore succeeds"), Restored.bValid);
	TestEqual(TEXT("rebuilder runs once"), Rebuilder->RebuildCount, 1);
	TestEqual(TEXT("rebuilder observes restored authored state"), Rebuilder->ObservedTitle, FString(TEXT("Original title")));
	TestEqual(TEXT("derived node is left to the rebuilder"), Fixture.Second->Value, 77);
	TestTrue(TEXT("new derived node is not removed"), NewDerived->GetOuter() == Fixture.Asset
		&& !NewDerived->HasAnyInternalFlags(EInternalObjectFlags::Garbage));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentPackageSnapshotUnverifiedRestoreTest,
	"UEAssetDoc.PackageSnapshot.UnverifiedRestoreRefusesWithoutReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPackageSnapshotUnverifiedRestoreTest::RunTest(const FString&)
{
	using namespace AssetDocumentPackageSnapshotTests;
	{
		// A dirty package: disk is not its pre-apply state, and the remedy says so.
		FFixture Fixture;
		MakeFixture(Fixture, TEXT("DirtyUnverified"));
		Fixture.Package->SetDirtyFlag(true);
		TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
		TUniquePtr<FAssetDocumentPackageSnapshot> Snapshot =
			FAssetDocumentPackageSnapshot::Capture(*Fixture.Package, Diagnostics);
		if (!TestNotNull(TEXT("dirty snapshot captures"), Snapshot.Get()))
		{
			return false;
		}
		Snapshot->ForceVerificationFailureForTests();
		const FAssetDocumentContractValidationResult Restored = Snapshot->Restore();
		TestFalse(TEXT("unverified dirty restore fails"), Restored.bValid);
		TestTrue(TEXT("the failure leads the diagnostics"), !Restored.Diagnostics.IsEmpty()
			&& Restored.Diagnostics[0].Code == TEXT("PackageSnapshot.RestoreFailed"));
		TestTrue(TEXT("the remedy names the unsaved changes"), !Restored.Diagnostics.IsEmpty()
			&& Restored.Diagnostics[0].Message.Contains(TEXT("unsaved changes")));
		TestTrue(TEXT("dirty flag kept"), Fixture.Package->IsDirty());
	}

	// A clean package on disk: the restore does not verify (forced). It is refused and
	// the package is not reloaded: the same objects stay in place, restored as far as
	// the snapshot could, and the remedy points at a reload by the user.
	FFixture Fixture;
	MakeFixture(Fixture, TEXT("Unverified"));
	const FString PackageFile = FPackageName::LongPackageNameToFilename(
		Fixture.PackageName, FPackageName::GetAssetPackageExtension());
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*PackageFile, false, true, true);
	};
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	if (!TestTrue(TEXT("fixture saves to disk"),
		UPackage::SavePackage(Fixture.Package, Fixture.Asset, *PackageFile, SaveArgs)))
	{
		return false;
	}
	Fixture.Package->SetDirtyFlag(false);
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	TUniquePtr<FAssetDocumentPackageSnapshot> Snapshot =
		FAssetDocumentPackageSnapshot::Capture(*Fixture.Package, Diagnostics);
	if (!TestNotNull(TEXT("saved snapshot captures"), Snapshot.Get()))
	{
		return false;
	}
	Fixture.Asset->Title = TEXT("Mutated");
	Fixture.Package->SetDirtyFlag(true);
	Snapshot->ForceVerificationFailureForTests();
	const FAssetDocumentContractValidationResult Restored = Snapshot->Restore();
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Restored.Diagnostics)
	{
		AddInfo(Diagnostic.Code + TEXT(": ") + Diagnostic.Message);
	}
	TestFalse(TEXT("an unverified restore fails"), Restored.bValid);
	TestTrue(TEXT("the failure leads the diagnostics"), !Restored.Diagnostics.IsEmpty()
		&& Restored.Diagnostics[0].Code == TEXT("PackageSnapshot.RestoreFailed"));
	TestTrue(TEXT("the remedy is a reload by the user"), !Restored.Diagnostics.IsEmpty()
		&& Restored.Diagnostics[0].Message.Contains(TEXT("Reload")));
	TestTrue(TEXT("the verification failure follows"), HasDiagnostic(Restored, TEXT("PackageSnapshot.VerificationFailed")));
	TestFalse(TEXT("nothing is reloaded"), HasDiagnostic(Restored, TEXT("PackageSnapshot.ReloadedFromDisk")));
	const FString ObjectPath = Fixture.PackageName + TEXT(".") + FPackageName::GetShortName(Fixture.PackageName);
	TestTrue(TEXT("the same asset object stays"), FindObject<FAsset>(nullptr, *ObjectPath) == Fixture.Asset);
	TestTrue(TEXT("the same package object stays"), FindPackage(nullptr, *Fixture.PackageName) == Fixture.Package);
	TestEqual(TEXT("captured state is written back"), Fixture.Asset->Title, FString(TEXT("Original title")));
	TestFalse(TEXT("the captured clean flag is restored"), Fixture.Package->IsDirty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentPackageSnapshotInPlaceRebuildTest,
	"UEAssetDoc.PackageSnapshot.ObjectsRebuiltInPlace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPackageSnapshotInPlaceRebuildTest::RunTest(const FString&)
{
	using namespace AssetDocumentPackageSnapshotTests;
	FFixture Fixture;
	MakeFixture(Fixture, TEXT("InPlace"));
	const FPackageState Before = CapturePackageState(*Fixture.Package);
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	TUniquePtr<FAssetDocumentPackageSnapshot> Snapshot =
		FAssetDocumentPackageSnapshot::Capture(*Fixture.Package, Diagnostics);
	if (!TestNotNull(TEXT("snapshot captures"), Snapshot.Get()))
	{
		return false;
	}
	// Editor code regenerates a subobject in place: NewObject of the same class and name over the
	// captured object (UE destroys and reconstructs it at the same address). While the snapshot
	// held it reference-counted, the engine asserted (ObjectItem->GetRefCount() == 0).
	FNode* const OriginalFirst = Fixture.First;
	FNode* const Rebuilt = NewObject<FNode>(Fixture.Asset, TEXT("First"), RF_Transactional);
	TestTrue(TEXT("the engine rebuilt the object in place"), Rebuilt == OriginalFirst);
	TestTrue(TEXT("the rebuilt object lost its state"), Rebuilt->Label.IsEmpty() && Rebuilt->Child == nullptr);
	Rebuilt->Label = TEXT("rebuilt");
	// A removed subobject is collected while the snapshot is open: the snapshot keeps it.
	Fixture.Asset->Nodes.Remove(Fixture.Second);
	Fixture.Second->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
	Fixture.Second->MarkAsGarbage();
	const TWeakObjectPtr<FNode> Second(Fixture.Second);
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestTrue(TEXT("the snapshot keeps a removed object alive through garbage collection"), Second.IsValid(true));
	Fixture.Package->SetDirtyFlag(true);

	const FAssetDocumentContractValidationResult Restored = Snapshot->Restore();
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Restored.Diagnostics)
	{
		AddInfo(Diagnostic.Code + TEXT(": ") + Diagnostic.Message);
	}
	TestTrue(TEXT("restore succeeds"), Restored.bValid);
	TestPackageStateEqual(*this, TEXT("in-place rebuild"), Before, *Fixture.Package);
	TestEqual(TEXT("the rebuilt object has the captured state"), OriginalFirst->Label, FString(TEXT("first")));
	TestTrue(TEXT("its captured subobject is referenced again"), OriginalFirst->Child == Fixture.Nested);
	TestTrue(TEXT("the removed object is back"), Fixture.Asset->Nodes.Contains(Fixture.Second)
		&& !Fixture.Second->HasAnyInternalFlags(EInternalObjectFlags::Garbage));
	TestFalse(TEXT("clean dirty flag is restored"), Fixture.Package->IsDirty());
	// After the restore the snapshot no longer pins anything.
	Snapshot.Reset();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	return true;
}

#endif
