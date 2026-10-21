// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Physics Asset apply (design §6, phase P1):
// UEAssetDoc.PhysicsAsset.Fidelity: §6.2 without Create (P2): every source (generated fixtures, the
//   tutorial asset, the corpus) is duplicated, emptied by a document (no bodies, constraints, profiles), and rebuilt
//   from the source's extraction: extracts equal with equal status; unchanged re-apply is a no-op; reload stable.
// UEAssetDoc.PhysicsAsset.FreeEdits: §6.3 edits made once with the editor's calls (Delete Body, Add
//   Body with the fit, Add Box, Details edits of bodies / shapes / constraints, profiles created, assigned and edited
//   through the profile picker, Disable Collision, a convex copy) and once through a document, on two duplicates:
//   equal state and status.
// UEAssetDoc.PhysicsAsset.StructuralEdits: Delete Body and Add Body in the middle of a chain of three
//   bodies: joints re-hung, a missing joint created, collision between joined bodies off and back on.
// UEAssetDoc.PhysicsAsset.Authoring: an AI-written edit (new body with shapes, a box-hull convex,
//   explicit frames, a constraint rename keeping its profile data) and typical authoring errors at document paths.
// UEAssetDoc.PhysicsAsset.SaveReloadProfile: constraint profile values across save + reload, also
//   after the editor saved while previewing a profile (UPhysicsConstraintTemplate::Serialize's DefaultProfile swap),
//   and profile renames keeping the per-constraint / per-body data.
// UEAssetDoc.PhysicsAsset.Rollback: failures after earlier phases changed the asset (a bad value in a
//   late phase, a new status error) roll back to the saved state; refusals before any write.
// UEAssetDoc.PhysicsAsset.Undo: the undo-history rules around a human edit, an AI apply and a failed apply.
// UEAssetDoc.PhysicsAsset.OpenEditorApply: an apply (a body deleted, Details, a profile) to an asset open in
//   the Physics Asset Editor closes and reopens the editor on the applied asset (its preview shows the applied bodies); a
//   failed apply rolls back with the editor open; a human edit in the reopened editor saves,
//   undoes and redoes.

#include "AssetDocumentPhysicsTestSupport.h"

#include "Algo/Count.h"
#include "AssetDocumentOpenEditorTestSupport.h"
#include "AssetDocumentPhysicsPrimitives.h"
#include "MeshUtilitiesCommon.h"
#include "MeshUtilitiesEngine.h"
#include "PhysicsAssetGenerationSettings.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "ReferenceSkeleton.h"
#include "Runtime/AssetDocumentUndoHistory.h"
#include "ScopedTransaction.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Animation/DebugSkelMeshComponent.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "IPersonaToolkit.h"
#include "IPhysicsAssetEditor.h"
#include "Misc/ScopeExit.h"
#include "TickableEditorObject.h"
#include "Toolkits/AssetEditorToolkit.h"

#if WITH_DEV_AUTOMATION_TESTS

// What the open Physics Asset Editor holds (open-editor apply test).
namespace PhysicsOpenEditorTests
{
void TickEditorUi()
{
	for (int32 Tick = 0; Tick < 3; ++Tick)
	{
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().Tick();
		}
		FTickableEditorObject::TickObjects(0.f);
	}
}

IAssetEditorInstance* EditorOf(UObject& Asset)
{
	UAssetEditorSubsystem* Editors = GEditor != nullptr ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	return Editors != nullptr ? Editors->FindEditorForAsset(&Asset, false) : nullptr;
}

// The preview mesh component of the Physics Asset Editor open on Asset.
UDebugSkelMeshComponent* PreviewOf(UObject& Asset)
{
	IAssetEditorInstance* Instance = EditorOf(Asset);
	if (Instance == nullptr || Instance->GetEditorName() != FName(TEXT("PhysicsAssetEditor")))
	{
		return nullptr;
	}
	IPhysicsAssetEditor* Editor = static_cast<IPhysicsAssetEditor*>(static_cast<FAssetEditorToolkit*>(Instance));
	return Editor->GetPersonaToolkit()->GetPreviewMeshComponent();
}

// The editor's preview simulates the asset's bodies as they are now.
void TestPreviewShows(FAutomationTestBase& Test, UPhysicsAsset& Asset, const TCHAR* Stage)
{
	UDebugSkelMeshComponent* Preview = PreviewOf(Asset);
	if (!Test.TestNotNull(*FString::Printf(TEXT("%s: the editor's preview"), Stage), Preview))
	{
		return;
	}
	Test.TestTrue(*FString::Printf(TEXT("%s: the preview uses the asset"), Stage), Preview->GetPhysicsAsset() == &Asset);
	Test.AddInfo(FString::Printf(TEXT("%s: preview body instances %d, body setups %d"), Stage, Preview->Bodies.Num(), Asset.SkeletalBodySetups.Num()));
	if (!Preview->Bodies.IsEmpty())
	{
		Test.TestEqual(*FString::Printf(TEXT("%s: the preview has a body instance per body"), Stage), Preview->Bodies.Num(), Asset.SkeletalBodySetups.Num());
	}
}

// The human's Save in the open editor (its toolbar Save, Ctrl+S): the editor's SaveAsset command, run as the shared
// open-editor test support does (from automation the bare command saves nothing: see SaveFromEditor). The bare
// command's no-op in this unattended session is checked first.
bool HumanSave(FAutomationTestBase& Test, UObject& Asset)
{
	namespace OpenEditorTests = UE::AssetDocument::OpenEditorTests;
	FAssetEditorToolkit* Toolkit = OpenEditorTests::FindToolkit(&Asset);
	if (!Test.TestNotNull(TEXT("the human saves in the open editor"), Toolkit))
	{
		return false;
	}
	Test.TestTrue(TEXT("unattended, the editor's bare Save command saves nothing"), OpenEditorTests::BareSaveCommandSavesNothing(*Toolkit, Asset));
	return OpenEditorTests::SaveFromEditor(*Toolkit, Asset);
}
} // namespace PhysicsOpenEditorTests

namespace AssetDocumentPhysicsApplyTests
{
using namespace AssetDocumentPhysicsTests;
namespace EditorEquivalent = UE::AssetDocument::Graph::Physics::EditorEquivalent;

TSharedPtr<FJsonObject> PhysicsDocument(const FTarget& Target, const TCHAR* Action, const FString& Properties, const FString& Body)
{
	return Parse(FString::Printf(TEXT(R"({"SchemaVersion": 1, "Class": "%s", "Action": "%s", "Target": "%s", "Properties": %s, "Body": %s})"),
		PhysicsAssetClass, Action, *Target.PackageName, *Properties, *Body));
}

// No bodies, constraints, collision pairs or profiles (the preview mesh is kept).
TSharedPtr<FJsonObject> EmptiedDocument(const FTarget& Target)
{
	return PhysicsDocument(Target, TEXT("Update"), TEXT(R"({"ConstraintProfiles": [], "PhysicalAnimationProfiles": []})"),
		TEXT(R"({"Bodies": {}, "Constraints": {}, "CollisionDisabled": []})"));
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

// Refusal or failure at a document path that leaves the saved asset as it was: same extraction, package clean.
void ExpectUnchangedFailure(FAutomationTestBase& Test, const FTarget& Target, const TSharedPtr<FJsonObject>& Document, const TCHAR* Code,
	const FString& Path, const FString& Stage)
{
	const FString Before = Serialize(ExtractResult(Target.ObjectPath).Payload);
	const FAssetDocumentResult Result = Apply(Target, Document);
	Test.TestFalse(*FString::Printf(TEXT("%s fails"), *Stage), Result.IsSuccess());
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

TSharedPtr<FJsonObject> Field(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name)
{
	const TSharedPtr<FJsonObject>* Child = nullptr;
	if (Object.IsValid() && Object->TryGetObjectField(Name, Child))
	{
		return *Child;
	}
	const TSharedPtr<FJsonObject> Created = MakeShared<FJsonObject>();
	if (Object.IsValid())
	{
		Object->SetObjectField(Name, Created);
	}
	return Created;
}

FString FirstKey(const TSharedPtr<FJsonObject>& Map)
{
	auto First = Map->Values.CreateConstIterator();
	return First ? FString(*First->Key) : FString();
}

FString LastKey(const TSharedPtr<FJsonObject>& Map)
{
	FString Last;
	for (const auto& Entry : Map->Values)
	{
		Last = Entry.Key;
	}
	return Last;
}

UPhysicsConstraintTemplate* ConstraintOf(UPhysicsAsset& Asset, const FString& Joint)
{
	const int32 Index = Asset.FindConstraintIndex(FName(*Joint));
	return Asset.ConstraintSetup.IsValidIndex(Index) ? Asset.ConstraintSetup[Index].Get() : nullptr;
}

// The Details panel adding an entry to one of the asset's profile lists (the array's "+" and the typed name).
void AddProfileEntry(UPhysicsAsset& Asset, const TCHAR* ListName, FName Name)
{
	FProperty* Property = FindFProperty<FProperty>(UPhysicsAsset::StaticClass(), ListName);
	TArray<FName>* List = Property->ContainerPtrToValuePtr<TArray<FName>>(&Asset);
	Asset.PreEditChange(Property);
	Asset.Modify();
	const int32 Index = List->Add(Name);
	FPropertyChangedEvent Event(Property, EPropertyChangeType::ArrayAdd);
	TArray<TMap<FString, int32>> Indices;
	Indices.AddDefaulted();
	Indices[0].Add(Property->GetName(), Index);
	Event.SetArrayIndexPerObject(Indices);
	Event.ObjectIteratorIndex = 0;
	Asset.PostEditChangeProperty(Event);
}

// FPhysicsAssetDetailsCustomization::ApplyConstraintProfile (the constraint profile picker).
void PickConstraintProfile(UPhysicsAsset& Asset, FName Name)
{
	Asset.CurrentConstraintProfileName = Name;
	for (UPhysicsConstraintTemplate* Constraint : Asset.ConstraintSetup)
	{
		Constraint->ApplyConstraintProfile(Name, Constraint->DefaultInstance, false);
	}
}

// FPhysicsAssetDetailsCustomization::ApplyPhysicalAnimationProfile (the physical animation profile picker).
void PickPhysicalAnimationProfile(UPhysicsAsset& Asset, FName Name)
{
	Asset.CurrentPhysicalAnimationProfileName = Name;
	for (USkeletalBodySetup* Body : Asset.SkeletalBodySetups)
	{
		if (FPhysicalAnimationProfile* Profile = Body->FindPhysicalAnimationProfile(Name))
		{
			Body->CurrentPhysicalAnimationProfile = *Profile;
		}
	}
}

// The editor's Add Body with the project's generation settings: the body fitted to the bone's vertex infos
// (CreateCollisionFromBone on the dominant-weight vertex infos) with its shapes named after the bone.
int32 AddBodyWithFit(FAutomationTestBase& Test, UPhysicsAsset& Asset, FName Bone)
{
	USkeletalMesh* Mesh = Asset.GetPreviewMesh();
	const FPhysAssetCreateParams& Params = GetDefault<UPhysicsAssetGenerationSettings>()->CreateParams;
	const int32 BoneIndex = Mesh->GetRefSkeleton().FindBoneIndex(Bone);
	TArray<FBoneVertInfo> Infos;
	FMeshUtilitiesEngine::CalcBoneVertInfos(Mesh, Infos, Params.VertWeight == EVW_DominantWeight, Params.LodIndex);
	const int32 Index = Infos.IsValidIndex(BoneIndex) ? EditorEquivalent::AddBody(Asset, *Mesh, Bone, Params, &Infos[BoneIndex], {BoneIndex}) : INDEX_NONE;
	if (!Test.TestTrue(*FString::Printf(TEXT("Add Body on %s"), *Bone.ToString()), Index != INDEX_NONE))
	{
		return INDEX_NONE;
	}
	EditorEquivalent::Refresh(Asset);
	return Index;
}

// A bone without a body that the dominant-weight fit can fit (enough vertices) and that has a body above it.
FName AddableBone(const UPhysicsAsset& Asset, FName Excluded)
{
	USkeletalMesh* Mesh = Asset.GetPreviewMesh();
	TArray<FBoneVertInfo> Infos;
	FMeshUtilitiesEngine::CalcBoneVertInfos(Mesh, Infos, true, 0);
	const FReferenceSkeleton& Skeleton = Mesh->GetRefSkeleton();
	for (int32 Bone = 1; Bone < Skeleton.GetNum(); ++Bone)
	{
		const FName Name = Skeleton.GetBoneName(Bone);
		if (Name != Excluded && Asset.FindBodyIndex(Name) == INDEX_NONE && Infos.IsValidIndex(Bone) && Infos[Bone].Positions.Num() >= 30
			&& Asset.FindControllingBodyIndex(Mesh, Bone) != INDEX_NONE)
		{
			return Name;
		}
	}
	return NAME_None;
}

// The deepest body with no body below it (a hand or a foot of a ragdoll).
FName LeafBody(const UPhysicsAsset& Asset)
{
	USkeletalMesh* Mesh = Asset.GetPreviewMesh();
	const FReferenceSkeleton& Skeleton = Mesh->GetRefSkeleton();
	FName Leaf;
	int32 Depth = -1;
	for (const USkeletalBodySetup* Body : Asset.SkeletalBodySetups)
	{
		TArray<int32> Below;
		const_cast<UPhysicsAsset&>(Asset).GetNearestBodyIndicesBelow(Below, Body->BoneName, Mesh);
		const int32 Bone = Skeleton.FindBoneIndex(Body->BoneName);
		const int32 BoneDepth = Bone != INDEX_NONE ? Skeleton.GetDepthBetweenBones(Bone, 0) : -1;
		if (Below.IsEmpty() && Asset.FindConstraintIndex(Body->BoneName) != INDEX_NONE && BoneDepth > Depth)
		{
			Depth = BoneDepth;
			Leaf = Body->BoneName;
		}
	}
	return Leaf;
}

// Two duplicates must end equal: the extraction (Target cleared) and the status (package names cleared).
void ExpectSameState(FAutomationTestBase& Test, const FTarget& Expected, const FTarget& Got, const FString& Stage)
{
	const FAssetDocumentResult A = ExtractResult(Expected.ObjectPath);
	const FAssetDocumentResult B = ExtractResult(Got.ObjectPath);
	const FString TextA = WithoutTarget(A.Payload);
	const FString TextB = WithoutTarget(B.Payload);
	if (!Test.TestEqual(*FString::Printf(TEXT("%s: both extract equal"), *Stage), TextB, TextA))
	{
		Test.AddInfo(FirstDifference(TextA, TextB));
	}
	Test.TestEqual(*FString::Printf(TEXT("%s: equal status"), *Stage), StatusOf(B).Replace(*Got.AssetName, TEXT("")),
		StatusOf(A).Replace(*Expected.AssetName, TEXT("")));
}

const TCHAR* const BoxHull = TEXT(R"([{"X": -5, "Y": -5, "Z": -5}, {"X": 5, "Y": -5, "Z": -5}, {"X": -5, "Y": 5, "Z": -5},
	{"X": 5, "Y": 5, "Z": -5}, {"X": -5, "Y": -5, "Z": 5}, {"X": 5, "Y": -5, "Z": 5}, {"X": -5, "Y": 5, "Z": 5}, {"X": 5, "Y": 5, "Z": 5}])");
} // namespace AssetDocumentPhysicsApplyTests

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPhysicsFidelityTest, "UEAssetDoc.PhysicsAsset.Fidelity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPhysicsFidelityTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentPhysicsApplyTests;
	// MoverExamples' PA_Mannequin names /Game/Characters/Mannequins/Meshes/SKM_Manny as its preview mesh, which the
	// plugin does not ship (the template pack has SKM_Manny_Simple).
	AddExpectedMessagePlain(TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	const FTemplatePacks Packs = CorpusPacks();
	FFixtures Fixtures;
	TArray<TPair<FString, FString>> Sources; // name, object path
	const FTarget& Capsules = Fixtures.Add(TEXT("PA_Generated"));
	if (Generate(*this, Capsules, TutorialMesh, FPhysAssetCreateParams()) != nullptr)
	{
		Sources.Emplace(TEXT("generated (capsules)"), Capsules.ObjectPath);
	}
	const FTarget& Convexes = Fixtures.Add(TEXT("PA_Convex"));
	FPhysAssetCreateParams ConvexParams;
	ConvexParams.GeomType = EFG_SingleConvexHull;
	if (Generate(*this, Convexes, TutorialMesh, ConvexParams) != nullptr)
	{
		Sources.Emplace(TEXT("generated (convex hulls)"), Convexes.ObjectPath);
	}
	for (const FCorpusEntry& Entry : Corpus)
	{
		if (LoadCorpus(Entry) != nullptr)
		{
			Sources.Emplace(CorpusName(Entry), Entry.Path);
		}
		else
		{
			AddInfo(FString::Printf(TEXT("SKIP %s: not installed / enabled"), *CorpusName(Entry)));
		}
	}
	for (const TPair<FString, FString>& Source : Sources)
	{
		const FString& Name = Source.Key;
		const FAssetDocumentResult Original = ExtractResult(Source.Value);
		UObject* SourceAsset = LoadQuiet(Source.Value);
		if (!TestTrue(*FString::Printf(TEXT("%s extracts"), *Name), Original.IsSuccess() && Original.Payload.IsValid() && SourceAsset != nullptr))
		{
			AddInfo(DiagnosticsText(Original));
			continue;
		}
		if (Cast<UPhysicsAsset>(SourceAsset) == nullptr || Cast<UPhysicsAsset>(SourceAsset)->GetPreviewMesh() == nullptr)
		{
			// Add Body needs the preview mesh's skeleton (as in the editor): such an asset cannot be rebuilt from nothing.
			AddInfo(FString::Printf(TEXT("SKIP rebuild %s: no preview mesh (bodies cannot be added)"), *Name));
			continue;
		}
		const FTarget& Rebuilt = Fixtures.Add(TEXT("PA_Rebuilt"));
		if (Duplicate(*this, SourceAsset, Rebuilt) == nullptr)
		{
			continue;
		}
		double Start = FPlatformTime::Seconds();
		if (!ApplyOk(*this, Rebuilt, EmptiedDocument(Rebuilt), Name + TEXT(" emptied")))
		{
			continue;
		}
		const double EmptyMs = Milliseconds(Start);
		const TSharedPtr<FJsonObject> Emptied = Extract(*this, Rebuilt.ObjectPath, Name + TEXT(" emptied"));
		TestEqual(*FString::Printf(TEXT("%s: emptied has no bodies"), *Name), CountField(BodyOf(Emptied), TEXT("Bodies")), 0);
		Start = FPlatformTime::Seconds();
		FAssetDocumentResult Applied;
		if (!ApplyOk(*this, Rebuilt, ForTarget(Original.Payload, Rebuilt), Name + TEXT(" rebuilt"), &Applied))
		{
			continue;
		}
		const double RebuildMs = Milliseconds(Start);
		const FAssetDocumentResult Recreated = ExtractResult(Rebuilt.ObjectPath);
		const FString Expected = WithoutTarget(Original.Payload);
		const FString Got = WithoutTarget(Recreated.Payload);
		if (!TestEqual(*FString::Printf(TEXT("%s: the rebuilt asset extracts as the source"), *Name), Got, Expected))
		{
			AddInfo(FirstDifference(Expected, Got));
		}
		const FString SourceName = FPackageName::ObjectPathToObjectName(Source.Value);
		TestEqual(*FString::Printf(TEXT("%s: equal status"), *Name), StatusOf(Recreated).Replace(*Rebuilt.AssetName, TEXT("")),
			StatusOf(Original).Replace(*SourceName, TEXT("")));
		Start = FPlatformTime::Seconds();
		const FAssetDocumentResult Again = Apply(Rebuilt, ForTarget(Recreated.Payload, Rebuilt));
		const double ReapplyMs = Milliseconds(Start);
		TestTrue(*FString::Printf(TEXT("%s: unchanged re-apply succeeds (%s)"), *Name, *DiagnosticsText(Again)), Again.IsSuccess());
		TestFalse(*FString::Printf(TEXT("%s: unchanged re-apply is a no-op"), *Name), Again.bSavedAsset);
		TestNotNull(*FString::Printf(TEXT("%s: reload"), *Name), Reload(Rebuilt));
		TestEqual(*FString::Printf(TEXT("%s: equal after reload"), *Name), WithoutTarget(ExtractResult(Rebuilt.ObjectPath).Payload), Expected);
		AddInfo(FString::Printf(TEXT("PASS %s: empty %.0f ms, rebuild %.0f ms, unchanged re-apply %.0f ms, %d chars, status %s%s"), *Name, EmptyMs,
			RebuildMs, ReapplyMs, Expected.Len(), *StatusName(Recreated),
			Applied.Diagnostics.IsEmpty() ? TEXT("") : *(TEXT("; apply warnings: ") + DiagnosticsText(Applied).Left(400))));
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPhysicsFreeEditsTest, "UEAssetDoc.PhysicsAsset.FreeEdits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPhysicsFreeEditsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentPhysicsApplyTests;
	FFixtures Fixtures;
	UObject* Tutorial = LoadQuiet(TutorialPhysicsAsset);
	const FTarget& Editor = Fixtures.Add(TEXT("PA_EditorSide"));
	const FTarget& Document = Fixtures.Add(TEXT("PA_DocumentSide"));
	if (Tutorial == nullptr || Duplicate(*this, Tutorial, Editor) == nullptr || !TestNotNull(TEXT("duplicate"), Duplicate(*this, Tutorial, Document)))
	{
		return false;
	}
	UPhysicsAsset* Asset = Editor.GetAsset();
	ReleaseUndoHistory();

	// The editor's edits (design §4.1), as its UI makes them.
	// 1. Delete Body on a leaf (its constraint goes with it).
	const FName Leaf = LeafBody(*Asset);
	if (!TestFalse(TEXT("a leaf body"), Leaf.IsNone()))
	{
		return false;
	}
	{
		FScopedTransaction Transaction(NSLOCTEXT("PhysicsTests", "DeleteBody", "Delete Body"));
		EditorEquivalent::RemoveBody(*Asset, Asset->FindBodyIndex(Leaf));
		EditorEquivalent::Refresh(*Asset);
	}
	// 2. Add Body (with the fit) on a bone that has none.
	const FName NewBone = AddableBone(*Asset, Leaf);
	if (!TestFalse(TEXT("an addable bone"), NewBone.IsNone()))
	{
		return false;
	}
	{
		FScopedTransaction Transaction(NSLOCTEXT("PhysicsTests", "AddBody", "Add Body"));
		AddBodyWithFit(*this, *Asset, NewBone);
	}
	// 3. Body Details: physics type, mass override; a shape's Details; Add Box (AddNewPrimitive, default size, auto name).
	USkeletalBodySetup* Root = Asset->SkeletalBodySetups[UE::AssetDocument::Graph::Physics::BodyOrder(*Asset)[0]];
	{
		FScopedTransaction Transaction(NSLOCTEXT("PhysicsTests", "Details", "Body Details"));
		DetailsEdit(*this, *Root, TEXT("PhysicsType"), TEXT("PhysType_Kinematic"));
		DetailsEdit(*this, *Root, TEXT("DefaultInstance.bOverrideMass"), TEXT("True"));
		DetailsEdit(*this, *Root, TEXT("DefaultInstance.MassInKgOverride"), TEXT("42.5"));
		if (Root->AggGeom.SphylElems.Num() > 0)
		{
			DetailsEdit(*this, *Root, TEXT("AggGeom.SphylElems.0.Radius"), TEXT("13.25"));
		}
		Root->Modify();
		FKBoxElem& Box = Root->AggGeom.BoxElems.Add_GetRef(FKBoxElem());
		Box.SetTransform(FTransform::Identity);
		Box.X = Box.Y = Box.Z = 7.5f;
		Box.SetName(FName(*(Root->BoneName.ToString() + TEXT("_box"))));
		Asset->RefreshPhysicsAssetChange();
	}
	// 4. Constraint Details on the default profile (no profile picked).
	TArray<UPhysicsConstraintTemplate*> Constraints;
	for (UPhysicsConstraintTemplate* Constraint : Asset->ConstraintSetup)
	{
		Constraints.Add(Constraint);
	}
	if (!TestTrue(TEXT("constraints to edit"), Constraints.Num() >= 3))
	{
		return false;
	}
	{
		FScopedTransaction Transaction(NSLOCTEXT("PhysicsTests", "ConstraintDetails", "Constraint Details"));
		DetailsEdit(*this, *Constraints[0], TEXT("DefaultInstance.ProfileInstance.ConeLimit.Swing1LimitDegrees"), TEXT("33"));
		DetailsEdit(*this, *Constraints[0], TEXT("DefaultInstance.ProfileInstance.ConeLimit.Swing1Motion"), TEXT("ACM_Limited"));
		DetailsEdit(*this, *Constraints[1], TEXT("DefaultInstance.ProfileInstance.LinearLimit.Limit"), TEXT("4"));
		DetailsEdit(*this, *Constraints[1], TEXT("DefaultInstance.ProfileInstance.AngularDrive.SlerpDrive.Stiffness"), TEXT("250"));
	}
	// 5. A constraint profile: new entry, picked, one constraint assigned and edited in it, back to None.
	{
		FScopedTransaction Transaction(NSLOCTEXT("PhysicsTests", "ConstraintProfile", "Constraint profile"));
		AddProfileEntry(*Asset, TEXT("ConstraintProfiles"), TEXT("Ragdoll"));
		PickConstraintProfile(*Asset, TEXT("Ragdoll"));
		Constraints[2]->Modify();
		Constraints[2]->AddConstraintProfile(TEXT("Ragdoll"));
		DetailsEdit(*this, *Constraints[2], TEXT("DefaultInstance.ProfileInstance.TwistLimit.TwistLimitDegrees"), TEXT("12"));
		PickConstraintProfile(*Asset, NAME_None);
	}
	// 6. A physical animation profile: new entry, picked, the root body assigned and edited, back to None.
	{
		FScopedTransaction Transaction(NSLOCTEXT("PhysicsTests", "AnimationProfile", "Physical animation profile"));
		AddProfileEntry(*Asset, TEXT("PhysicalAnimationProfiles"), TEXT("HitReact"));
		PickPhysicalAnimationProfile(*Asset, TEXT("HitReact"));
		Root->Modify();
		Root->CurrentPhysicalAnimationProfile = FPhysicalAnimationProfile();
		Root->AddPhysicalAnimationProfile(TEXT("HitReact"));
		DetailsEdit(*this, *Root, TEXT("CurrentPhysicalAnimationProfile.PhysicalAnimationData.OrientationStrength"), TEXT("500"));
		DetailsEdit(*this, *Root, TEXT("CurrentPhysicalAnimationProfile.PhysicalAnimationData.bIsLocalSimulation"), TEXT("True"));
		PickPhysicalAnimationProfile(*Asset, NAME_None);
	}
	// 7. Disable Collision between the new body and the root.
	{
		FScopedTransaction Transaction(NSLOCTEXT("PhysicsTests", "Collision", "Disable Collision"));
		EditorEquivalent::SetPairCollision(*Asset, Asset->FindBodyIndex(NewBone), Asset->FindBodyIndex(Root->BoneName), false);
	}
	TestTrue(TEXT("editor side saved"), Save(*Asset));

	const FString ProfiledJoint = Constraints[2]->DefaultInstance.JointName.ToString();

	// The same state as a document on the other duplicate.
	const TSharedPtr<FJsonObject> EditorState = Extract(*this, Editor.ObjectPath, TEXT("editor side"));
	if (!EditorState.IsValid())
	{
		return false;
	}
	const double Start = FPlatformTime::Seconds();
	FAssetDocumentResult Applied;
	if (!ApplyOk(*this, Document, ForTarget(EditorState, Document), TEXT("document side"), &Applied))
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("document side applied in %.0f ms; diagnostics: %s"), Milliseconds(Start), *DiagnosticsText(Applied).Left(600)));
	ExpectSameState(*this, Editor, Document, TEXT("ragdoll edits"));
	// The profile data sits where the editor put it.
	UPhysicsConstraintTemplate* Edited = ConstraintOf(*Document.GetAsset(), ProfiledJoint);
	if (TestNotNull(TEXT("profiled constraint"), Edited))
	{
		const FPhysicsConstraintProfileHandle* Handle = Edited->ProfileHandles.FindByPredicate(
			[](const FPhysicsConstraintProfileHandle& Candidate) { return Candidate.ProfileName == TEXT("Ragdoll"); });
		TestTrue(TEXT("Ragdoll twist 12"), Handle != nullptr && FMath::IsNearlyEqual(Handle->ProfileProperties.TwistLimit.TwistLimitDegrees, 12.f));
	}
	FString DiffText;
	TestEqual(TEXT("Diff of the document is empty"), DiffCount(ForTarget(EditorState, Document), DiffText), 0);

	// A convex asset: the editor's copy of a convex shape (AddNewPrimitive with a selected convex: points shifted,
	// UpdateElemBox, the hull rebuilt); then the document.
	const FTarget& ConvexEditor = Fixtures.Add(TEXT("PA_ConvexEditor"));
	const FTarget& ConvexDocument = Fixtures.Add(TEXT("PA_ConvexDocument"));
	FPhysAssetCreateParams ConvexParams;
	ConvexParams.GeomType = EFG_SingleConvexHull;
	if (Generate(*this, ConvexEditor, TutorialMesh, ConvexParams) == nullptr || Duplicate(*this, ConvexEditor.Get(), ConvexDocument) == nullptr)
	{
		return false;
	}
	UPhysicsAsset* ConvexAsset = ConvexEditor.GetAsset();
	USkeletalBodySetup* Hulled = ConvexAsset->SkeletalBodySetups[0];
	const FName HulledBone = Hulled->BoneName;
	if (!TestTrue(TEXT("generated hull"), Hulled->AggGeom.ConvexElems.Num() > 0))
	{
		return false;
	}
	{
		FScopedTransaction Transaction(NSLOCTEXT("PhysicsTests", "CopyConvex", "Copy convex"));
		Hulled->Modify();
		FKConvexElem Copy;
		Copy.SetTransform(Hulled->AggGeom.ConvexElems[0].GetTransform());
		for (FVector Vertex : Hulled->AggGeom.ConvexElems[0].VertexData)
		{
			Vertex.X += 5.0;
			Copy.VertexData.Add(Vertex);
		}
		Copy.UpdateElemBox();
		Copy.SetName(FName(*(Hulled->BoneName.ToString() + TEXT("_convex"))));
		Hulled->AggGeom.ConvexElems.Add(Copy);
		Hulled->InvalidatePhysicsData();
		Hulled->CreatePhysicsMeshes();
		ConvexAsset->RefreshPhysicsAssetChange();
	}
	TestTrue(TEXT("convex editor side saved"), Save(*ConvexAsset));
	const TSharedPtr<FJsonObject> ConvexState = Extract(*this, ConvexEditor.ObjectPath, TEXT("convex editor side"));
	if (ConvexState.IsValid() && ApplyOk(*this, ConvexDocument, ForTarget(ConvexState, ConvexDocument), TEXT("convex document side")))
	{
		ExpectSameState(*this, ConvexEditor, ConvexDocument, TEXT("convex copy"));
		const USkeletalBodySetup* DocumentBody = ConvexDocument.GetAsset()->SkeletalBodySetups[ConvexDocument.GetAsset()->FindBodyIndex(HulledBone)];
		TestEqual(TEXT("two hulls"), DocumentBody->AggGeom.ConvexElems.Num(), 2);
		TestTrue(TEXT("the new hull is cooked"), DocumentBody->AggGeom.ConvexElems.Num() == 2 && DocumentBody->AggGeom.ConvexElems[1].GetChaosConvexMesh().IsValid());
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

// The structural edits on a body in the middle of a chain (Above - Middle - Below): Delete Body re-hangs the body below
// from the body above, Add Body puts the new body between them again (the body below re-hung, collision between the
// outer two back on), and Add Body over an existing body gives a body below without a joint a new one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPhysicsStructuralEditsTest, "UEAssetDoc.PhysicsAsset.StructuralEdits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPhysicsStructuralEditsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentPhysicsApplyTests;
	FFixtures Fixtures;
	UObject* Tutorial = LoadQuiet(TutorialPhysicsAsset);
	const FTarget& Target = Fixtures.Add(TEXT("PA_Structure"));
	UPhysicsAsset* Asset = Tutorial != nullptr ? Duplicate(*this, Tutorial, Target) : nullptr;
	USkeletalMesh* Mesh = Asset != nullptr ? Asset->GetPreviewMesh() : nullptr;
	if (!TestNotNull(TEXT("duplicate with a preview mesh"), Mesh))
	{
		return false;
	}
	const FReferenceSkeleton& Skeleton = Mesh->GetRefSkeleton();
	auto JointOf = [Asset](FName Child) -> const FConstraintInstance*
	{
		const int32 Index = Asset->FindConstraintIndex(Child);
		return Asset->ConstraintSetup.IsValidIndex(Index) ? &Asset->ConstraintSetup[Index]->DefaultInstance : nullptr;
	};
	auto JointsNamed = [Asset](FName Name) -> int32
	{
		return static_cast<int32>(Algo::CountIf(Asset->ConstraintSetup, [Name](const UPhysicsConstraintTemplate* Joint) { return Joint->DefaultInstance.JointName == Name; }));
	};
	auto Collides = [Asset](FName A, FName B) { return Asset->IsCollisionEnabled(Asset->FindBodyIndex(A), Asset->FindBodyIndex(B)); };

	FName Above, Middle, Below;
	for (const UPhysicsConstraintTemplate* Joint : Asset->ConstraintSetup)
	{
		const FConstraintInstance& Lower = Joint->DefaultInstance;
		const FConstraintInstance* Upper = JointOf(Lower.ConstraintBone2);
		if (Lower.JointName != Lower.ConstraintBone1 || Upper == nullptr || Upper->ConstraintBone1 != Lower.ConstraintBone2
			|| Asset->FindBodyIndex(Upper->ConstraintBone2) == INDEX_NONE)
		{
			continue;
		}
		const int32 MiddleBone = Skeleton.FindBoneIndex(Lower.ConstraintBone2);
		TArray<int32> Nearest;
		Asset->GetNearestBodyIndicesBelow(Nearest, Lower.ConstraintBone2, Mesh);
		if (MiddleBone != INDEX_NONE && Asset->FindParentBodyIndex(Mesh, MiddleBone) == Asset->FindBodyIndex(Upper->ConstraintBone2)
			&& Nearest.Contains(Asset->FindBodyIndex(Lower.ConstraintBone1)))
		{
			Above = Upper->ConstraintBone2;
			Middle = Lower.ConstraintBone2;
			Below = Lower.ConstraintBone1;
			break;
		}
	}
	if (!TestFalse(TEXT("a chain of three bodies"), Middle.IsNone()))
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("chain %s - %s - %s"), *Above.ToString(), *Middle.ToString(), *Below.ToString()));

	// Delete Body on the middle: the body below hangs from the body above, and the two do not collide.
	EditorEquivalent::RemoveBody(*Asset, Asset->FindBodyIndex(Middle));
	EditorEquivalent::Refresh(*Asset);
	TestEqual(TEXT("delete: the middle body is gone"), Asset->FindBodyIndex(Middle), INDEX_NONE);
	TestEqual(TEXT("delete: no joint of the middle body is left"), JointsNamed(Middle), 0);
	TestEqual(TEXT("delete: one joint for the body below"), JointsNamed(Below), 1);
	const FConstraintInstance* Rehung = JointOf(Below);
	TestTrue(TEXT("delete: the body below hangs from the body above"), Rehung != nullptr && Rehung->ConstraintBone1 == Below
		&& Rehung->ConstraintBone2 == Above);
	TestFalse(TEXT("delete: below and above do not collide"), Collides(Below, Above));

	// Add Body on the middle bone again: the chain is back, the outer two collide again.
	const FPhysAssetCreateParams Params;
	TestTrue(TEXT("add: a body"), EditorEquivalent::AddBody(*Asset, *Mesh, Middle, Params, nullptr, {}) != INDEX_NONE);
	EditorEquivalent::Refresh(*Asset);
	const FConstraintInstance* MiddleJoint = JointOf(Middle);
	const FConstraintInstance* BelowJoint = JointOf(Below);
	TestTrue(TEXT("add: the new body hangs from the body above"), MiddleJoint != nullptr && MiddleJoint->ConstraintBone1 == Middle
		&& MiddleJoint->ConstraintBone2 == Above);
	TestTrue(TEXT("add: the body below hangs from the new body"), BelowJoint != nullptr && BelowJoint->ConstraintBone1 == Below
		&& BelowJoint->ConstraintBone2 == Middle);
	TestEqual(TEXT("add: still one joint for the body below"), JointsNamed(Below), 1);
	TestTrue(TEXT("add: below and above collide again"), Collides(Below, Above));
	TestFalse(TEXT("add: below and the new body do not collide"), Collides(Below, Middle));
	TestFalse(TEXT("add: the new body and above do not collide"), Collides(Middle, Above));

	// The body below loses its joint; Add Body over the existing middle body replaces it and gives the body below a
	// new joint to it.
	FPhysicsAssetUtils::DestroyConstraint(Asset, Asset->FindConstraintIndex(Below));
	TestTrue(TEXT("re-add: a body"), EditorEquivalent::AddBody(*Asset, *Mesh, Middle, Params, nullptr, {}) != INDEX_NONE);
	EditorEquivalent::Refresh(*Asset);
	BelowJoint = JointOf(Below);
	MiddleJoint = JointOf(Middle);
	TestTrue(TEXT("re-add: a new joint hangs the body below from the new body"), BelowJoint != nullptr
		&& BelowJoint->ConstraintBone1 == Below && BelowJoint->ConstraintBone2 == Middle);
	TestEqual(TEXT("re-add: one joint for the new body"), JointsNamed(Middle), 1);
	TestTrue(TEXT("re-add: the new body hangs from the body above"), MiddleJoint != nullptr && MiddleJoint->ConstraintBone2 == Above);
	TestFalse(TEXT("re-add: below and the new body do not collide"), Collides(Below, Middle));
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPhysicsAuthoringTest, "UEAssetDoc.PhysicsAsset.Authoring",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPhysicsAuthoringTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentPhysicsApplyTests;
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("PA_Authoring"));
	if (Generate(*this, Target, TutorialMesh, FPhysAssetCreateParams()) == nullptr)
	{
		return false;
	}
	const TSharedPtr<FJsonObject> State = Extract(*this, Target.ObjectPath, TEXT("generated"));
	if (!State.IsValid())
	{
		return false;
	}
	{
		// Numbers are single precision written shortest (the trial saw 17-digit noise such as 1.0000000000000002):
		// each one reads back from 9 significant digits.
		TArray<FString> Long;
		TFunction<void(const TSharedPtr<FJsonValue>&, const FString&)> Visit = [&Long, &Visit](const TSharedPtr<FJsonValue>& Value, const FString& Path)
		{
			if (Value->Type == EJson::Number)
			{
				const double Number = Value->AsNumber();
				if (FCString::Atod(*FString::Printf(TEXT("%.9g"), Number)) != Number && Long.Num() < 5)
				{
					Long.Add(FString::Printf(TEXT("%s=%.17g"), *Path, Number));
				}
			}
			else if (Value->Type == EJson::Array)
			{
				const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
				for (int32 Index = 0; Index < Items.Num(); ++Index)
				{
					Visit(Items[Index], FString::Printf(TEXT("%s/%d"), *Path, Index));
				}
			}
			else if (Value->Type == EJson::Object)
			{
				for (const auto& Pair : Value->AsObject()->Values)
				{
					Visit(Pair.Value, Path + TEXT("/") + Pair.Key);
				}
			}
		};
		Visit(MakeShared<FJsonValueObject>(State), FString());
		TestTrue(*FString::Printf(TEXT("numbers are shortest float32 decimals (%s)"), *FString::Join(Long, TEXT(", "))), Long.IsEmpty());
	}
	const FString NewBone = AddableBone(*Target.GetAsset(), NAME_None).ToString();
	const FString Joint = FirstKey(Field(BodyOf(State), TEXT("Constraints")));
	const FString Hand = LastKey(Field(BodyOf(State), TEXT("Bodies")));
	if (!TestFalse(TEXT("an addable bone"), NewBone == TEXT("None")) || !TestFalse(TEXT("a constraint"), Joint.IsEmpty()))
	{
		return false;
	}

	// The AI's edit: a profile; a new body with a capsule and a box-hull convex; the first constraint renamed (its
	// profile data and Details go with it) with explicit frames; a body's Details.
	const TSharedPtr<FJsonObject> Edit = ForTarget(State, Target);
	const TSharedPtr<FJsonObject> Bodies = Field(BodyOf(Edit), TEXT("Bodies"));
	const TSharedPtr<FJsonObject> Constraints = Field(BodyOf(Edit), TEXT("Constraints"));
	PropertiesOf(Edit)->SetArrayField(TEXT("ConstraintProfiles"), {MakeShared<FJsonValueString>(TEXT("Ragdoll"))});
	Bodies->SetObjectField(NewBone, Parse(FString::Printf(TEXT(R"({"Shapes": {
		"Capsules": [{"Center": {"X": 5, "Y": 0, "Z": 0}, "Rotation": {"Pitch": 0, "Yaw": 0, "Roll": 90}, "Radius": 4, "Length": 10}],
		"Convexes": [{"VertexData": %s}]}})"), BoxHull)));
	const TSharedPtr<FJsonObject> Renamed = Parse(Serialize(Constraints->GetObjectField(Joint)));
	Renamed->SetStringField(TEXT("RenamedFrom"), Joint);
	Renamed->SetObjectField(TEXT("Profiles"), Parse(TEXT(R"({"Ragdoll": {"TwistLimit": {"TwistMotion": "ACM_Limited", "TwistLimitDegrees": 15}}})")));
	Renamed->SetObjectField(TEXT("Frames"), Parse(TEXT(R"({
		"Child": {"Position": {"X": 0, "Y": 0, "Z": 0}, "PriAxis": {"X": 1, "Y": 0, "Z": 0}, "SecAxis": {"X": 0, "Y": 1, "Z": 0}},
		"Parent": {"Position": {"X": 10, "Y": 0, "Z": 0}, "PriAxis": {"X": 1, "Y": 0, "Z": 0}, "SecAxis": {"X": 0, "Y": 1, "Z": 0}}})")));
	Constraints->RemoveField(Joint);
	Constraints->SetObjectField(TEXT("RenamedJoint"), Renamed);
	Field(Bodies->GetObjectField(Hand), TEXT("Properties"))->SetObjectField(TEXT("DefaultInstance"),
		Parse(TEXT(R"({"bOverrideMass": true, "MassInKgOverride": 3.5})")));
	FAssetDocumentResult Applied;
	if (!ApplyOk(*this, Target, Edit, TEXT("AI edit"), &Applied))
	{
		return false;
	}
	const FAssetDocumentResult After = ExtractResult(Target.ObjectPath);
	const TSharedPtr<FJsonObject> AfterBody = BodyOf(After.Payload);
	TestTrue(TEXT("new body"), Field(AfterBody, TEXT("Bodies"))->HasField(NewBone));
	TestTrue(TEXT("renamed constraint"), Field(AfterBody, TEXT("Constraints"))->HasField(TEXT("RenamedJoint")));
	TestFalse(TEXT("old joint name gone"), Field(AfterBody, TEXT("Constraints"))->HasField(Joint));
	const FString RenamedText = Serialize(Field(AfterBody, TEXT("Constraints"))->GetObjectField(TEXT("RenamedJoint")));
	TestTrue(*FString::Printf(TEXT("renamed constraint keeps its profile and frames: %s"), *RenamedText),
		RenamedText.Contains(TEXT("\"Ragdoll\"")) && RenamedText.Contains(TEXT("\"Frames\"")));
	TestFalse(*FString::Printf(TEXT("no new status error: %s"), *StatusOf(After)), StatusName(After) == TEXT("Error"));
	const UPhysicsAsset* Asset = Target.GetAsset();
	const USkeletalBodySetup* Added = Asset->SkeletalBodySetups[Asset->FindBodyIndex(FName(*NewBone))];
	TestTrue(TEXT("the authored hull is cooked"), Added->AggGeom.ConvexElems.Num() == 1 && Added->AggGeom.ConvexElems[0].GetChaosConvexMesh().IsValid());
	TestEqual(TEXT("auto names"), Added->AggGeom.SphylElems.IsEmpty() ? FString() : Added->AggGeom.SphylElems[0].GetName().ToString(),
		NewBone + TEXT("_capsule"));
	// Re-applying the extraction is a no-op; Diff of the AI document is empty.
	const FAssetDocumentResult Again = Apply(Target, ForTarget(After.Payload, Target));
	TestTrue(TEXT("re-apply"), Again.IsSuccess() && !Again.bSavedAsset);
	FString DiffText;
	const TSharedPtr<FJsonObject> Settled = Parse(Serialize(Edit));
	Field(BodyOf(Settled), TEXT("Constraints"))->GetObjectField(TEXT("RenamedJoint"))->RemoveField(TEXT("RenamedFrom"));
	TestEqual(*FString::Printf(TEXT("Diff of the AI document is empty: %s"), *DiffText), DiffCount(Settled, DiffText), 0);
	// Frames omitted again: the editor's Snap.
	const TSharedPtr<FJsonObject> Snap = ForTarget(After.Payload, Target);
	Field(BodyOf(Snap), TEXT("Constraints"))->GetObjectField(TEXT("RenamedJoint"))->RemoveField(TEXT("Frames"));
	if (ApplyOk(*this, Target, Snap, TEXT("snap")))
	{
		TestFalse(TEXT("snapped frames are omitted"), Serialize(BodyOf(ExtractResult(Target.ObjectPath).Payload)).Contains(TEXT("\"Frames\"")));
	}

	// Typical authoring errors: refused at their document paths, nothing changes.
	const TSharedPtr<FJsonObject> Base = ForTarget(ExtractResult(Target.ObjectPath).Payload, Target);
	const FString Bodiless = AddableBone(*Target.GetAsset(), NAME_None).ToString();
	auto Variant = [&Base]() { return Parse(Serialize(Base)); };
	{
		const TSharedPtr<FJsonObject> Bad = Variant();
		Field(BodyOf(Bad), TEXT("Bodies"))->SetObjectField(TEXT("no_such_bone"), Parse(TEXT(R"({"Shapes": {"Spheres": [{"Radius": 3}]}})")));
		ExpectUnchangedFailure(*this, Target, Bad, TEXT("PhysicsAsset.BoneNotFound"), TEXT("/Body/Bodies/no_such_bone"), TEXT("unknown bone"));
	}
	{
		// A near miss ("<bone>_bone" for "<bone>"): the message itself names the close bone.
		const TSharedPtr<FJsonObject> Bad = Variant();
		Field(BodyOf(Bad), TEXT("Bodies"))->SetObjectField(Hand + TEXT("_bone"), Parse(TEXT(R"({"Shapes": {"Spheres": [{"Radius": 3}]}})")));
		const FAssetDocumentResult Result = Apply(Target, Bad);
		const FString Text = DiagnosticsText(Result);
		TestTrue(*FString::Printf(TEXT("near-miss bone: the message names %s (got %s)"), *Hand, *Text),
			!Result.IsSuccess() && HasCode(Result, TEXT("PhysicsAsset.BoneNotFound")) && Text.Contains(FString::Printf(TEXT("Close: %s"), *Hand)));
	}
	{
		const TSharedPtr<FJsonObject> Bad = Variant();
		Field(BodyOf(Bad), TEXT("Bodies"))->SetObjectField(Bodiless, MakeShared<FJsonObject>());
		ExpectUnchangedFailure(*this, Target, Bad, TEXT("PhysicsAsset.ShapesRequired"), FString(TEXT("/Body/Bodies/")) + Bodiless,
			TEXT("new body without shapes"));
	}
	{
		const TSharedPtr<FJsonObject> Bad = Variant();
		Field(Field(BodyOf(Bad), TEXT("Bodies"))->GetObjectField(Hand), TEXT("Shapes"))->SetArrayField(TEXT("Cylinders"), {});
		ExpectUnchangedFailure(*this, Target, Bad, TEXT("PhysicsAsset.UnknownKey"), FString::Printf(TEXT("/Body/Bodies/%s/Shapes/Cylinders"), *Hand),
			TEXT("unknown shape type"));
	}
	{
		const TSharedPtr<FJsonObject> Bad = Variant();
		Field(Field(BodyOf(Bad), TEXT("Bodies"))->GetObjectField(Hand), TEXT("PhysicalAnimation"))->SetObjectField(TEXT("Missing"), MakeShared<FJsonObject>());
		ExpectUnchangedFailure(*this, Target, Bad, TEXT("PhysicsAsset.UnknownProfile"),
			FString::Printf(TEXT("/Body/Bodies/%s/PhysicalAnimation/Missing"), *Hand), TEXT("unlisted profile"));
	}
	{
		const TSharedPtr<FJsonObject> Bad = Variant();
		Field(Field(BodyOf(Bad), TEXT("Bodies"))->GetObjectField(Hand), TEXT("Properties"))->SetNumberField(TEXT("NoSuchField"), 1);
		ExpectUnchangedFailure(*this, Target, Bad, TEXT("PhysicsAsset.FieldNotEditable"), FString::Printf(TEXT("/Body/Bodies/%s/Properties/NoSuchField"), *Hand),
			TEXT("unknown Details field"));
	}
	{
		const TSharedPtr<FJsonObject> Bad = Variant();
		Field(Field(BodyOf(Bad), TEXT("Constraints")), TEXT("Extra"))->SetStringField(TEXT("Child"), Hand);
		Field(BodyOf(Bad), TEXT("Constraints"))->GetObjectField(TEXT("Extra"))->SetStringField(TEXT("Parent"), TEXT("no_body_here"));
		ExpectUnchangedFailure(*this, Target, Bad, TEXT("PhysicsAsset.BodyNotFound"), TEXT("/Body/Constraints/Extra/Parent"), TEXT("constraint without a body"));
	}
	{
		const TSharedPtr<FJsonObject> Bad = Variant();
		Field(BodyOf(Bad), TEXT("Bodies"))->SetObjectField(Bodiless, Parse(TEXT(R"({"Fit": {"GeomType": "EFG_NoSuchType"}})")));
		ExpectUnchangedFailure(*this, Target, Bad, TEXT("PhysicsAsset.InvalidValue"), FString::Printf(TEXT("/Body/Bodies/%s/Fit/GeomType"), *Bodiless),
			TEXT("bad Fit setting"));
	}
	{
		// Fit is a create-time helper: on a body that exists it is ignored (with a warning when the apply runs).
		const TSharedPtr<FJsonObject> Fitted = Variant();
		Field(BodyOf(Fitted), TEXT("Bodies"))->GetObjectField(Hand)->SetObjectField(TEXT("Fit"), MakeShared<FJsonObject>());
		Field(Field(BodyOf(Fitted), TEXT("Bodies"))->GetObjectField(Hand), TEXT("Properties"))->SetStringField(TEXT("PhysicsType"), TEXT("PhysType_Kinematic"));
		FAssetDocumentResult Ignored;
		if (ApplyOk(*this, Target, Fitted, TEXT("Fit on an existing body"), &Ignored))
		{
			TestTrue(*FString::Printf(TEXT("FitIgnored warning: %s"), *DiagnosticsText(Ignored)), HasCode(Ignored, TEXT("PhysicsAsset.FitIgnored")));
		}
	}
	{
		const FTarget& Missing = Fixtures.Add(TEXT("PA_Missing"));
		const FAssetDocumentResult Refused = Apply(Missing, PhysicsDocument(Missing, TEXT("Create"), TEXT("{}"), TEXT("{}")));
		TestTrue(*FString::Printf(TEXT("Create needs the mesh: %s"), *DiagnosticsText(Refused)),
			!Refused.IsSuccess() && HasCode(Refused, TEXT("PhysicsAsset.MeshRequired")) && FirstPath(Refused) == TEXT("/FactoryContext"));
		TestNull(TEXT("no asset was made"), Missing.Get());
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPhysicsSaveReloadTest, "UEAssetDoc.PhysicsAsset.SaveReloadProfile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPhysicsSaveReloadTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentPhysicsApplyTests;
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("PA_Profiles"));
	if (Generate(*this, Target, TutorialMesh, FPhysAssetCreateParams()) == nullptr)
	{
		return false;
	}
	const TSharedPtr<FJsonObject> State = Extract(*this, Target.ObjectPath, TEXT("generated"));
	const FString Joint = FirstKey(Field(BodyOf(State), TEXT("Constraints")));
	const FString Bone = FirstKey(Field(BodyOf(State), TEXT("Bodies")));
	// Default profile 20 degrees, Ragdoll 70; a physical animation profile on a body.
	const TSharedPtr<FJsonObject> Edit = ForTarget(State, Target);
	PropertiesOf(Edit)->SetArrayField(TEXT("ConstraintProfiles"), {MakeShared<FJsonValueString>(TEXT("Ragdoll"))});
	PropertiesOf(Edit)->SetArrayField(TEXT("PhysicalAnimationProfiles"), {MakeShared<FJsonValueString>(TEXT("HitReact"))});
	const TSharedPtr<FJsonObject> Constraint = Field(BodyOf(Edit), TEXT("Constraints"))->GetObjectField(Joint);
	Field(Constraint, TEXT("Properties"))->SetObjectField(TEXT("ProfileInstance"), Parse(TEXT(R"({"ConeLimit": {"Swing1LimitDegrees": 20}})")));
	Constraint->SetObjectField(TEXT("Profiles"), Parse(TEXT(R"({"Ragdoll": {"ConeLimit": {"Swing1LimitDegrees": 70}}})")));
	Field(BodyOf(Edit), TEXT("Bodies"))->GetObjectField(Bone)->SetObjectField(TEXT("PhysicalAnimation"),
		Parse(TEXT(R"({"HitReact": {"OrientationStrength": 300}})")));
	if (!ApplyOk(*this, Target, Edit, TEXT("profiles")))
	{
		return false;
	}
	auto Check = [this, &Target, &Joint](const TCHAR* Stage, const TCHAR* ProfileName, float Default, float Profile)
	{
		UPhysicsAsset* Asset = Target.GetAsset();
		UPhysicsConstraintTemplate* Template = Asset != nullptr ? ConstraintOf(*Asset, Joint) : nullptr;
		if (!TestNotNull(*FString::Printf(TEXT("%s: constraint"), Stage), Template))
		{
			return;
		}
		const FPhysicsConstraintProfileHandle* Handle = Template->ProfileHandles.FindByPredicate(
			[ProfileName](const FPhysicsConstraintProfileHandle& Candidate) { return Candidate.ProfileName == ProfileName; });
		TestTrue(*FString::Printf(TEXT("%s: %s handle %.0f"), Stage, ProfileName, Profile),
			Handle != nullptr && FMath::IsNearlyEqual(Handle->ProfileProperties.ConeLimit.Swing1LimitDegrees, Profile));
		TestTrue(*FString::Printf(TEXT("%s: instance is the default profile %.0f (got %.1f)"), Stage, Default,
			Template->DefaultInstance.ProfileInstance.ConeLimit.Swing1LimitDegrees),
			FMath::IsNearlyEqual(Template->DefaultInstance.ProfileInstance.ConeLimit.Swing1LimitDegrees, Default));
	};
	Check(TEXT("after apply + save"), TEXT("Ragdoll"), 20.f, 70.f);
	const FString Saved = WithoutTarget(ExtractResult(Target.ObjectPath).Payload);
	TestNotNull(TEXT("reload"), Reload(Target));
	Check(TEXT("after reload"), TEXT("Ragdoll"), 20.f, 70.f);
	TestEqual(TEXT("extract after reload"), WithoutTarget(ExtractResult(Target.ObjectPath).Payload), Saved);

	// The human previews Ragdoll in the editor (every instance shows the profile's values) and saves: the document is
	// still the default profile; an AI edit then goes back to None and the saved file keeps both values.
	PickConstraintProfile(*Target.GetAsset(), TEXT("Ragdoll"));
	TestTrue(TEXT("human save while previewing"), Save(*Target.GetAsset()));
	TestEqual(TEXT("extract while previewing"), WithoutTarget(ExtractResult(Target.ObjectPath).Payload), Saved);
	const TSharedPtr<FJsonObject> Mass = ForTarget(Parse(Saved), Target);
	Field(Field(BodyOf(Mass), TEXT("Bodies"))->GetObjectField(Bone), TEXT("Properties"))->SetObjectField(TEXT("DefaultInstance"),
		Parse(TEXT(R"({"bOverrideMass": true, "MassInKgOverride": 9})")));
	if (ApplyOk(*this, Target, Mass, TEXT("AI edit after the preview")))
	{
		TestTrue(TEXT("preview reset to None"), Target.GetAsset()->CurrentConstraintProfileName.IsNone());
		Check(TEXT("after the AI edit"), TEXT("Ragdoll"), 20.f, 70.f);
		TestNotNull(TEXT("reload 2"), Reload(Target));
		Check(TEXT("after reload 2"), TEXT("Ragdoll"), 20.f, 70.f);
	}

	// Profile renames (the same list position): the data moves with the name.
	const TSharedPtr<FJsonObject> Rename = ForTarget(ExtractResult(Target.ObjectPath).Payload, Target);
	PropertiesOf(Rename)->SetArrayField(TEXT("ConstraintProfiles"), {MakeShared<FJsonValueString>(TEXT("Falling"))});
	PropertiesOf(Rename)->SetArrayField(TEXT("PhysicalAnimationProfiles"), {MakeShared<FJsonValueString>(TEXT("Stagger"))});
	const TSharedPtr<FJsonObject> RenamedConstraint = Field(BodyOf(Rename), TEXT("Constraints"))->GetObjectField(Joint);
	RenamedConstraint->SetObjectField(TEXT("Profiles"), Parse(TEXT(R"({"Falling": {"ConeLimit": {"Swing1LimitDegrees": 70}}})")));
	Field(BodyOf(Rename), TEXT("Bodies"))->GetObjectField(Bone)->SetObjectField(TEXT("PhysicalAnimation"),
		Parse(TEXT(R"({"Stagger": {"OrientationStrength": 300}})")));
	if (ApplyOk(*this, Target, Rename, TEXT("profile renames")))
	{
		TestNotNull(TEXT("reload 3"), Reload(Target));
		Check(TEXT("after rename + reload"), TEXT("Falling"), 20.f, 70.f);
		const UPhysicsAsset* Asset = Target.GetAsset();
		const FPhysicalAnimationProfile* Profile = Asset->SkeletalBodySetups[Asset->FindBodyIndex(FName(*Bone))]->FindPhysicalAnimationProfile(TEXT("Stagger"));
		TestTrue(TEXT("physical animation data renamed"), Profile != nullptr && FMath::IsNearlyEqual(Profile->PhysicalAnimationData.OrientationStrength, 300.f));
		FString DiffText;
		TestEqual(TEXT("Diff of the rename is empty"), DiffCount(Rename, DiffText), 0);
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPhysicsRollbackTest, "UEAssetDoc.PhysicsAsset.Rollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPhysicsRollbackTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentPhysicsApplyTests;
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("PA_Rollback"));
	if (Generate(*this, Target, TutorialMesh, FPhysAssetCreateParams()) == nullptr)
	{
		return false;
	}
	const TSharedPtr<FJsonObject> State = Extract(*this, Target.ObjectPath, TEXT("generated"));
	const FString Saved = WithoutTarget(State);
	const TSharedPtr<FJsonObject> Bodies = Field(BodyOf(State), TEXT("Bodies"));
	const TSharedPtr<FJsonObject> Constraints = Field(BodyOf(State), TEXT("Constraints"));
	const FString Joint = FirstKey(Constraints);
	const FString First = FirstKey(Bodies);
	// A body no constraint has as its parent (a hand or a foot): removing it removes only its own constraint.
	FString Last;
	for (const auto& Entry : Bodies->Values)
	{
		bool bParent = false;
		for (const auto& Constraint : Constraints->Values)
		{
			bParent |= Constraint.Value->AsObject()->GetStringField(TEXT("Parent")) == FString(*Entry.Key);
		}
		if (!bParent && FString(*Entry.Key) != First)
		{
			Last = Entry.Key;
		}
	}

	// 1. Profiles, a body removal and body Details run first; a bad value in a late phase (constraint Details) fails.
	{
		const TSharedPtr<FJsonObject> Bad = ForTarget(State, Target);
		PropertiesOf(Bad)->SetArrayField(TEXT("ConstraintProfiles"), {MakeShared<FJsonValueString>(TEXT("Ragdoll"))});
		Field(BodyOf(Bad), TEXT("Bodies"))->RemoveField(Last);
		for (const auto& Constraint : Constraints->Values)
		{
			if (Constraint.Value->AsObject()->GetStringField(TEXT("Child")) == Last)
			{
				Field(BodyOf(Bad), TEXT("Constraints"))->RemoveField(Constraint.Key);
			}
		}
		TArray<TSharedPtr<FJsonValue>> Pairs;
		for (const TSharedPtr<FJsonValue>& Pair : BodyOf(Bad)->GetArrayField(TEXT("CollisionDisabled")))
		{
			const TArray<TSharedPtr<FJsonValue>>& Bones = Pair->AsArray();
			if (Bones[0]->AsString() != Last && Bones[1]->AsString() != Last)
			{
				Pairs.Add(Pair);
			}
		}
		BodyOf(Bad)->SetArrayField(TEXT("CollisionDisabled"), Pairs);
		Field(Field(BodyOf(Bad), TEXT("Bodies"))->GetObjectField(First), TEXT("Properties"))->SetStringField(TEXT("PhysicsType"), TEXT("PhysType_Kinematic"));
		Field(Field(BodyOf(Bad), TEXT("Constraints"))->GetObjectField(Joint), TEXT("Properties"))->SetObjectField(TEXT("ProfileInstance"),
			Parse(TEXT(R"({"TwistLimit": {"TwistMotion": "NotAMotion"}})")));
		ExpectUnchangedFailure(*this, Target, Bad, TEXT("PhysicsAsset.InvalidValue"),
			FString::Printf(TEXT("/Body/Constraints/%s/Properties/ProfileInstance/TwistLimit/TwistMotion"), *Joint), TEXT("late bad value"));
	}
	// 2. A change that leaves the asset invalid (a body without shapes): new status error, rolled back.
	{
		const TSharedPtr<FJsonObject> Bad = ForTarget(State, Target);
		Field(BodyOf(Bad), TEXT("Bodies"))->GetObjectField(First)->SetObjectField(TEXT("Shapes"), MakeShared<FJsonObject>());
		ExpectUnchangedFailure(*this, Target, Bad, TEXT("PhysicsAsset.ValidationFailed"), FString::Printf(TEXT("/Body/Bodies/%s/Shapes"), *First),
			TEXT("validation failure"));
	}
	// The saved file is what it was.
	TestNotNull(TEXT("reload"), Reload(Target));
	TestEqual(TEXT("saved state after both failures"), WithoutTarget(ExtractResult(Target.ObjectPath).Payload), Saved);
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPhysicsUndoTest, "UEAssetDoc.PhysicsAsset.Undo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPhysicsUndoTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentPhysicsApplyTests;
	if (GEditor == nullptr || GEditor->Trans == nullptr)
	{
		return false;
	}
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("PA_Undo"));
	if (Generate(*this, Target, TutorialMesh, FPhysAssetCreateParams()) == nullptr)
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
	// 1. The human edits a body's mass in the Details panel and saves; the AI edits: the history referencing the asset
	// is cleared, the human edit is kept.
	USkeletalBodySetup* Body = Target.GetAsset()->SkeletalBodySetups[0];
	const FString Bone = Body->BoneName.ToString();
	{
		FScopedTransaction Transaction(NSLOCTEXT("PhysicsTests", "Human", "Human edit"));
		DetailsEdit(*this, *Body, TEXT("DefaultInstance.bOverrideMass"), TEXT("True"));
		DetailsEdit(*this, *Body, TEXT("DefaultInstance.MassInKgOverride"), TEXT("11"));
	}
	TestTrue(TEXT("human save"), UEditorLoadingAndSavingUtils::SavePackages({Target.GetAsset()->GetOutermost()}, false));
	TestTrue(TEXT("the human edit is in the undo history"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	const TSharedPtr<FJsonObject> HumanState = Extract(*this, Target.ObjectPath, TEXT("after human edit"));
	const TSharedPtr<FJsonObject> AiEdit = ForTarget(HumanState, Target);
	PropertiesOf(AiEdit)->SetArrayField(TEXT("ConstraintProfiles"), {MakeShared<FJsonValueString>(TEXT("Ragdoll"))});
	Field(Field(BodyOf(AiEdit), TEXT("Bodies"))->GetObjectField(Bone), TEXT("Properties"))->SetStringField(TEXT("PhysicsType"), TEXT("PhysType_Kinematic"));
	if (!ApplyOk(*this, Target, AiEdit, TEXT("AI edit")))
	{
		return false;
	}
	TestFalse(TEXT("after AI edit: no undo history references the asset"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	const FString AfterEdit = Serialize(Extract(*this, Target.ObjectPath, TEXT("after AI edit")));
	TestTrue(TEXT("human edit kept"), AfterEdit.Contains(TEXT("\"MassInKgOverride\":11")));
	UndoRedoAll(TEXT("after AI edit"));
	TestEqual(TEXT("after AI edit + undo/redo"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after undo/redo"))), AfterEdit);

	// 2. The human edits again (unsaved); an AI apply fails and rolls back to the human state; undo stays safe.
	ReleaseUndoHistory();
	{
		FScopedTransaction Transaction(NSLOCTEXT("PhysicsTests", "Human2", "Human edit 2"));
		DetailsEdit(*this, *Target.GetAsset()->SkeletalBodySetups[0], TEXT("DefaultInstance.MassInKgOverride"), TEXT("12"));
	}
	const FString SecondHuman = Serialize(Extract(*this, Target.ObjectPath, TEXT("after second human edit")));
	const TSharedPtr<FJsonObject> Failing = ForTarget(Parse(SecondHuman), Target);
	Field(Field(BodyOf(Failing), TEXT("Bodies"))->GetObjectField(Bone), TEXT("Properties"))->SetStringField(TEXT("PhysicsType"), TEXT("NotAType"));
	const FAssetDocumentResult Failed = Apply(Target, Failing);
	TestFalse(TEXT("failing apply fails"), Failed.IsSuccess());
	TestEqual(TEXT("after rollback: the human state"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after rollback"))), SecondHuman);
	UndoRedoAll(TEXT("after rollback"));
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPhysicsOpenEditorApplyTest, "UEAssetDoc.PhysicsAsset.OpenEditorApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPhysicsOpenEditorApplyTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentPhysicsApplyTests;
	// Engine noise on the editor's first open: two Physics Asset Editor commands share the Cmd+C default chord.
	AddExpectedMessagePlain(TEXT("has the same default chord"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	UAssetEditorSubsystem* AssetEditors = GEditor != nullptr ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("asset editors"), AssetEditors) || GEditor->Trans == nullptr)
	{
		return false;
	}
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("PA_Open"));
	if (Generate(*this, Target, TutorialMesh, FPhysAssetCreateParams()) == nullptr)
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		if (UObject* Open = Target.Get())
		{
			AssetEditors->CloseAllEditorsForAsset(Open);
		}
	};
	const TSharedPtr<FJsonObject> State = Extract(*this, Target.ObjectPath, TEXT("before"));
	if (!AssetEditors->OpenEditorForAsset(Target.Get()))
	{
		AddWarning(TEXT("The Physics Asset Editor did not open in this session; the open-editor apply was not exercised."));
		return true;
	}
	PhysicsOpenEditorTests::TickEditorUi();
	// The human does not save after opening.
	TestFalse(TEXT("opening the Physics Asset Editor leaves the package clean"), Target.Get()->GetOutermost()->IsDirty());
	ReleaseUndoHistory();
	const IAssetEditorInstance* EditorBefore = PhysicsOpenEditorTests::EditorOf(*Target.Get());
	TestNotNull(TEXT("the preview shows the asset's bodies"), PhysicsOpenEditorTests::PreviewOf(*Target.Get()));

	// A body without children is deleted (with its constraint and collision pairs), a body's mass changes, a constraint
	// profile is added: indices of the editor's selection and of its preview bodies shift.
	const TSharedPtr<FJsonObject> Bodies = Field(BodyOf(State), TEXT("Bodies"));
	const TSharedPtr<FJsonObject> Constraints = Field(BodyOf(State), TEXT("Constraints"));
	const FString First = FirstKey(Bodies);
	FString Leaf;
	for (const auto& Entry : Bodies->Values)
	{
		bool bParent = false;
		for (const auto& Constraint : Constraints->Values)
		{
			bParent |= Constraint.Value->AsObject()->GetStringField(TEXT("Parent")) == FString(*Entry.Key);
		}
		if (!bParent && FString(*Entry.Key) != First)
		{
			Leaf = Entry.Key;
		}
	}
	const TSharedPtr<FJsonObject> Edit = ForTarget(State, Target);
	PropertiesOf(Edit)->SetArrayField(TEXT("ConstraintProfiles"), {MakeShared<FJsonValueString>(TEXT("Ragdoll"))});
	Field(BodyOf(Edit), TEXT("Bodies"))->RemoveField(Leaf);
	for (const auto& Constraint : Constraints->Values)
	{
		if (Constraint.Value->AsObject()->GetStringField(TEXT("Child")) == Leaf)
		{
			Field(BodyOf(Edit), TEXT("Constraints"))->RemoveField(Constraint.Key);
		}
	}
	TArray<TSharedPtr<FJsonValue>> Pairs;
	for (const TSharedPtr<FJsonValue>& Pair : BodyOf(Edit)->GetArrayField(TEXT("CollisionDisabled")))
	{
		const TArray<TSharedPtr<FJsonValue>>& Bones = Pair->AsArray();
		if (Bones[0]->AsString() != Leaf && Bones[1]->AsString() != Leaf)
		{
			Pairs.Add(Pair);
		}
	}
	BodyOf(Edit)->SetArrayField(TEXT("CollisionDisabled"), Pairs);
	Field(Field(Field(BodyOf(Edit), TEXT("Bodies"))->GetObjectField(First), TEXT("Properties")), TEXT("DefaultInstance"))
		->SetBoolField(TEXT("bOverrideMass"), true);
	Field(Field(Field(BodyOf(Edit), TEXT("Bodies"))->GetObjectField(First), TEXT("Properties")), TEXT("DefaultInstance"))
		->SetNumberField(TEXT("MassInKgOverride"), 7.0);
	const int32 BodiesBefore = Target.GetAsset()->SkeletalBodySetups.Num();
	// The apply closes the open editor and opens a new one on the applied asset.
	UE::AssetDocument::OpenEditorTests::FEditorReopenWatch Reopen(Target.PackageName, EditorBefore);
	FAssetDocumentResult Applied;
	const bool bApplied = ApplyOk(*this, Target, Edit, TEXT("apply while open"), &Applied);
	Reopen.Stop();
	if (!bApplied)
	{
		return false;
	}
	TestTrue(*FString::Printf(TEXT("the editor was reopened (%s)"), *DiagnosticsText(Applied)), HasCode(Applied, TEXT("PhysicsAsset.EditorReopened")));
	UPhysicsAsset* Asset = Target.GetAsset();
	if (!TestNotNull(TEXT("asset after the apply"), Asset))
	{
		return false;
	}
	PhysicsOpenEditorTests::TickEditorUi();
	TestEqual(TEXT("one body fewer"), Asset->SkeletalBodySetups.Num(), BodiesBefore - 1);
	TestTrue(TEXT("the profile"), Asset->GetConstraintProfileNames().Contains(FName(TEXT("Ragdoll"))));
	TestFalse(TEXT("the package is saved"), Asset->GetOutermost()->IsDirty());
	const IAssetEditorInstance* Reopened = PhysicsOpenEditorTests::EditorOf(*Asset);
	TestNotNull(TEXT("the Physics Asset Editor is open again on the asset"), Reopened);
	TestTrue(TEXT("the apply closed the open editor"), Reopen.Closed());
	TestTrue(TEXT("it is a new editor (its selection and preview were built from the applied asset)"), Reopened != nullptr && Reopened == Reopen.Reopened());
	PhysicsOpenEditorTests::TestPreviewShows(*this, *Asset, TEXT("after the apply"));
	const FString AppliedText = Serialize(Extract(*this, Target.ObjectPath, TEXT("applied")));

	// A failure after earlier phases changed the asset (a bad constraint value) rolls back with the editor open.
	{
		const TSharedPtr<FJsonObject> Bad = ForTarget(Parse(AppliedText), Target);
		PropertiesOf(Bad)->SetArrayField(TEXT("ConstraintProfiles"), {});
		const FString Joint = FirstKey(Field(BodyOf(Bad), TEXT("Constraints")));
		Field(Field(BodyOf(Bad), TEXT("Constraints"))->GetObjectField(Joint), TEXT("Properties"))->SetObjectField(TEXT("ProfileInstance"),
			Parse(TEXT(R"({"TwistLimit": {"TwistMotion": "NotAMotion"}})")));
		const FAssetDocumentResult Failed = Apply(Target, Bad);
		TestFalse(TEXT("failing apply while open fails"), Failed.IsSuccess());
		TestTrue(*FString::Printf(TEXT("failing apply: InvalidValue (%s)"), *DiagnosticsText(Failed)), HasCode(Failed, TEXT("PhysicsAsset.InvalidValue")));
		Asset = Target.GetAsset();
		if (!TestNotNull(TEXT("asset after the rollback"), Asset))
		{
			return false;
		}
		PhysicsOpenEditorTests::TickEditorUi();
		TestEqual(TEXT("after the rollback the asset is as applied"), Serialize(Extract(*this, Target.ObjectPath, TEXT("rolled back"))), AppliedText);
		TestFalse(TEXT("after the rollback the package is clean"), Asset->GetOutermost()->IsDirty());
		TestNotNull(TEXT("after the rollback the editor is open"), PhysicsOpenEditorTests::EditorOf(*Asset));
		PhysicsOpenEditorTests::TestPreviewShows(*this, *Asset, TEXT("after the rollback"));
	}

	// A human Details edit in the reopened editor, saved: the apply stays; undo / redo work.
	ReleaseUndoHistory();
	{
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentPhysicsTests", "HumanEdit", "Edit Body"));
		DetailsEdit(*this, *Asset->SkeletalBodySetups[0], TEXT("DefaultInstance.LinearDamping"), TEXT("0.5"));
	}
	const FString Human = Serialize(Extract(*this, Target.ObjectPath, TEXT("human edit")));
	TestNotEqual(TEXT("the human edit landed"), Human, AppliedText);
	TestTrue(TEXT("the human's save"), PhysicsOpenEditorTests::HumanSave(*this, *Asset));
	TestFalse(TEXT("saved"), Asset->GetOutermost()->IsDirty());
	TestEqual(TEXT("saving keeps the apply and the human edit"), Serialize(Extract(*this, Target.ObjectPath, TEXT("saved"))), Human);
	TestTrue(TEXT("the human edit undoes"), GEditor->Trans->CanUndo() && GEditor->UndoTransaction());
	PhysicsOpenEditorTests::TickEditorUi();
	TestEqual(TEXT("undo restores the applied asset"), Serialize(Extract(*this, Target.ObjectPath, TEXT("undo"))), AppliedText);
	TestTrue(TEXT("the human edit redoes"), GEditor->Trans->CanRedo() && GEditor->RedoTransaction());
	PhysicsOpenEditorTests::TickEditorUi();
	TestEqual(TEXT("redo restores the human edit"), Serialize(Extract(*this, Target.ObjectPath, TEXT("redo"))), Human);
	ReleaseUndoHistory();
	Asset->GetOutermost()->SetDirtyFlag(false);

	AssetEditors->CloseAllEditorsForAsset(Asset);
	TestNull(TEXT("the editor closed"), PhysicsOpenEditorTests::EditorOf(*Asset));
	return true;
}

#endif
