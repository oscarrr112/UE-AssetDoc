// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// End-to-end Animation Blueprint through the production Service and the Python Profile
// (profile.anim_blueprint.v1): one ApplyFile creates and populates an Animation Blueprint
// (variables, an anim layer, a state machine with states, transitions and a rule graph, cached
// poses, a linked anim layer node, a blend with a property binding, preview / sync group /
// thumbnail settings); Diff and Extract agree with the document; re-applying the extraction
// changes nothing; a layout-only edit applies; failures in the middle of Apply and at compile roll
// back to an asset that compiles and extracts identically; an AI apply after a human edit leaves an
// undo history that cannot corrupt the asset; the saved package reloads from disk to the same
// document; a failed create leaves nothing behind.
//
// Graphs.CustomTransitionBlendGraph / Graphs.BlendSpaceGraph / Graphs.FunctionBinding: the same
// stages (RunFeatureStages) for a transition's custom blend graph, a Blend Space Graph node with
// its samples and sample graphs, and a pure-function property binding, plus a removal step.
// AnimGraph.LayerInterfaceRoundTrip: an Anim Layer Interface created and edited by documents (layers,
// input poses, layer groups) and an Animation Blueprint implementing it.
//
// AnimBlueprint.OmittedNodeSettings: a node setting the document leaves out is the
// node's default (the Details panel's "reset to default"), like a DataTable row's unwritten field.
// AnimBlueprint.StateMachine.ConduitAliasAndTransitionSettings ends with the same rule for a
// state machine transition: its omitted settings are a newly placed transition's.
//
// AnimGraph.LinkedGraphAndLayerNodesRoundTrip: Linked Anim Graph + Linked Anim Layer nodes
// (own layer and a layer of another Animation Blueprint class) in one apply on the production
// Service; the layer signatures resolve because anim layers are compiled first.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_LinkedAnimLayer.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimGraphNode_UseCachedPose.h"
#include "AnimGraphNode_LinkedAnimGraph.h"
#include "AnimGraphNode_BlendSpaceGraph.h"
#include "AnimGraphNode_LinkedInputPose.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_TwoWayBlend.h"
#include "AnimBlueprint/AssetDocumentAnimBlendSpaceGraphRawEdits.h"
#include "Animation/AnimLayerInterface.h"
#include "Animation/BlendSpace.h"
#include "AnimationGraph.h"
#include "AnimationGraphSchema.h"
#include "Factories/AnimBlueprintFactory.h"
#include "AnimStateTransitionNode.h"
#include "AnimStateAliasNode.h"
#include "AnimStateNodeBase.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "HAL/FileManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "ThumbnailRendering/SceneThumbnailInfo.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentAnimBlueprintPythonTests
{
const TCHAR* Folder = TEXT("/Game/AssetDocumentAnimBlueprintPython");
const TCHAR* Skeleton = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton");
const TCHAR* PreviewMesh = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP");
const TCHAR* IdleAnim = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle.Tutorial_Idle");
const TCHAR* WalkAnim = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Walk_Fwd.Tutorial_Walk_Fwd");

// Editor undo history references assets the tests force-delete; drop it first (as
// ObjectTools::DeleteSingleObject's reference-checked path does).
void ReleaseUndoHistory()
{
	if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting && !GEditor->IsTransactionActive()
		&& GEditor->Trans->GetQueueLength() > 0)
	{
		GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentAnimBlueprintPythonTests", "Cleanup", "AssetDocument AnimBlueprint test cleanup"));
	}
}

struct FTarget
{
	FString AssetName;
	FString PackageName;
	FString ObjectPath;
	FString PackageFile;
	FString SidecarPath;

	explicit FTarget(const TCHAR* Prefix)
		: AssetName(FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits)))
	{
		PackageName = FString(Folder) / AssetName;
		ObjectPath = PackageName + TEXT(".") + AssetName;
		PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		SidecarPath = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("AssetDocumentAnimBlueprintPython"), AssetName + TEXT(".assetdoc.json"));
	}

	UAnimBlueprint* Get() const { return Cast<UAnimBlueprint>(FSoftObjectPath(ObjectPath).ResolveObject()); }

	// Unloads (saved) or deletes (unsaved) the Animation Blueprint, then removes files.
	void Cleanup() const
	{
		ReleaseUndoHistory();
		if (UAnimBlueprint* Blueprint = Get())
		{
			if (IFileManager::Get().FileExists(*PackageFile))
			{
				UPackageTools::UnloadPackages({Blueprint->GetOutermost()});
			}
			else
			{
				ObjectTools::DeleteObjects({Blueprint}, false, ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
			}
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		if (IFileManager::Get().FileExists(*PackageFile))
		{
			IFileManager::Get().Delete(*PackageFile, false, true);
			FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({PackageFile});
		}
		IFileManager::Get().Delete(*SidecarPath, false, true);
	}
};

FString Serialize(const TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	if (Object.IsValid())
	{
		const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}
	return Text;
}

TSharedPtr<FJsonObject> Parse(const FString& Json)
{
	TSharedPtr<FJsonObject> Object;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object);
	return Object;
}

void LogResult(FAutomationTestBase& Test, const TCHAR* Stage, const FAssetDocumentResult& Result)
{
	Test.AddInfo(FString::Printf(TEXT("%s: %s"), Stage, *Result.Message));
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Test.AddInfo(FString::Printf(TEXT("%s: [%s] %s %s"), Stage, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
	}
}

FAssetDocumentResult Apply(FAutomationTestBase& Test, const FTarget& Target, const FString& Json, const TCHAR* Stage)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target.SidecarPath), true);
	FFileHelper::SaveStringToFile(Json, *Target.SidecarPath);
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Target.SidecarPath;
	Request.bSaveAsset = true;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
	LogResult(Test, Stage, Result);
	return Result;
}

TSharedPtr<FJsonObject> Extract(FAutomationTestBase& Test, const FTarget& Target, const TCHAR* Stage)
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = Target.ObjectPath;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Extract(Request);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: extract succeeds"), Stage), Result.IsSuccess() && Result.Payload.IsValid()))
	{
		LogResult(Test, Stage, Result);
		return nullptr;
	}
	return Result.Payload;
}

void TestNoDiff(FAutomationTestBase& Test, const FString& Json, const TCHAR* Stage)
{
	FAssetDocumentDiffRequest Request;
	Request.Document = Parse(Json);
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Diff(Request);
	const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
	const bool bOk = Result.IsSuccess() && Result.Payload.IsValid() && Result.Payload->TryGetArrayField(TEXT("changed"), Changed);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: diff succeeds"), Stage), bOk))
	{
		LogResult(Test, Stage, Result);
		return;
	}
	if (!Test.TestEqual(*FString::Printf(TEXT("%s: live asset equals the document"), Stage), Changed->Num(), 0))
	{
		Test.AddInfo(Serialize(Result.Payload));
	}
}

bool CompilesClean(const UBlueprint* Blueprint)
{
	return Blueprint != nullptr && (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
}

bool ExtractEquals(FAutomationTestBase& Test, const FTarget& Target, const FString& Expected, const TCHAR* Stage)
{
	const FString Actual = Serialize(Extract(Test, Target, Stage));
	if (!Test.TestEqual(*FString::Printf(TEXT("%s: extraction is unchanged"), Stage), Actual, Expected))
	{
		int32 First = 0;
		while (First < Expected.Len() && First < Actual.Len() && Expected[First] == Actual[First])
		{
			++First;
		}
		const int32 Start = FMath::Max(0, First - 200);
		Test.AddInfo(FString::Printf(TEXT("expected ...%s"), *Expected.Mid(Start, 600)));
		Test.AddInfo(FString::Printf(TEXT("actual   ...%s"), *Actual.Mid(Start, 600)));
		return false;
	}
	return true;
}

FString Document(const FTarget& Target, const TCHAR* Action, const FString& Body)
{
	return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.AnimBlueprint\",\"Action\":\"%s\",")
		TEXT("\"Properties\":{\"BlueprintDescription\":\"Hero made by AssetDocument\",\"bWarnAboutBlueprintUsage\":true},")
		TEXT("\"Body\":{\"ParentClass\":\"/Script/Engine.AnimInstance\",\"TargetSkeleton\":\"%s\",%s}}"),
		*Target.PackageName, Action, Skeleton, *Body);
}

// A state machine (Idle <-> Walk, rule on bMoving), a cached pose, an own anim layer used through
// a Linked Anim Layer node, a Two Way Blend whose Alpha is bound to Speed, preview settings.
FString InitialBody(const TCHAR* WalkSequence, bool bSecondVariable, double PlayRate)
{
	return FString::Printf(TEXT(
		"\"Variables\":["
		"{\"Name\":\"Speed\",\"Type\":\"float\",\"DefaultValue\":0.5},"
		"{\"Name\":\"bMoving\",\"Type\":\"bool\"}%s],"
		"\"AnimLayers\":[{\"Name\":\"UpperLayer\",\"Nodes\":["
		"{\"Id\":\"Idle\",\"Action\":\"Anim:SequencePlayer\",\"Properties\":{\"Node.Sequence\":\"%s\"}}],"
		"\"Links\":[{\"From\":\"Idle:Pose\",\"To\":\"Output:Result\"}]}],"
		"\"AnimGraph\":{\"Nodes\":["
		"{\"Id\":\"Locomotion\",\"Action\":\"Anim:StateMachine\",\"Graph\":{\"Nodes\":["
		"{\"Id\":\"Idle\",\"Action\":\"SM:State\",\"Graph\":{\"Nodes\":["
		"{\"Id\":\"Play\",\"Action\":\"Anim:SequencePlayer\",\"Properties\":{\"Node.Sequence\":\"%s\"}}],"
		"\"Links\":[{\"From\":\"Play:Pose\",\"To\":\"Output:Result\"}]}},"
		"{\"Id\":\"Walk\",\"Action\":\"SM:State\",\"Graph\":{\"Nodes\":["
		"{\"Id\":\"Play\",\"Action\":\"Anim:SequencePlayer\",\"Properties\":{\"Node.Sequence\":\"%s\",\"Node.PlayRate\":%g}}],"
		"\"Links\":[{\"From\":\"Play:Pose\",\"To\":\"Output:Result\"}]}},"
		"{\"Id\":\"StartWalking\",\"Action\":\"SM:Transition\",\"From\":\"Idle\",\"To\":\"Walk\",\"Graph\":{\"Nodes\":["
		"{\"Id\":\"Moving\",\"Action\":\"K2:Get:bMoving\"}],"
		"\"Links\":[{\"From\":\"Moving:bMoving\",\"To\":\"Result:bCanEnterTransition\"}]}},"
		"{\"Id\":\"StopWalking\",\"Action\":\"SM:Transition\",\"From\":\"Walk\",\"To\":\"Idle\",\"Graph\":{\"Nodes\":["
		"{\"Id\":\"Moving\",\"Action\":\"K2:Get:bMoving\"},{\"Id\":\"Not\",\"Action\":\"K2:CallFunction:/Script/Engine.KismetMathLibrary:Not_PreBool\"}],"
		"\"Links\":[{\"From\":\"Moving:bMoving\",\"To\":\"Not:A\"},{\"From\":\"Not:ReturnValue\",\"To\":\"Result:bCanEnterTransition\"}]}}],"
		"\"Links\":[{\"From\":\"Entry:Entry\",\"To\":\"Idle:In\"}]}},"
		"{\"Id\":\"Cache\",\"Action\":\"Anim:SaveCachedPose:LocoCache\"},"
		"{\"Id\":\"UseCache\",\"Action\":\"Anim:UseCachedPose:LocoCache\"},"
		"{\"Id\":\"Upper\",\"Action\":\"Anim:LinkedAnimLayer:UpperLayer\"},"
		"{\"Id\":\"Blend\",\"Action\":\"Anim:TwoWayBlend\",\"Properties\":{\"Bindings\":{\"Alpha\":{\"Path\":\"Speed\"}}}}],"
		"\"Links\":["
		"{\"From\":\"Locomotion:Pose\",\"To\":\"Cache:Pose\"},"
		"{\"From\":\"UseCache:Pose\",\"To\":\"Blend:A\"},"
		"{\"From\":\"Upper:Pose\",\"To\":\"Blend:B\"},"
		"{\"From\":\"Blend:Pose\",\"To\":\"Output:Result\"}]},"
		"\"Preview\":{\"Mesh\":\"%s\"},"
		"\"SyncGroups\":[\"Feet\"],"
		"\"Thumbnail\":{\"OrbitPitch\":-15,\"OrbitYaw\":45,\"OrbitZoom\":10}"),
		bSecondVariable ? TEXT(",{\"Name\":\"Count\",\"Type\":\"int\",\"DefaultValue\":3}") : TEXT(""),
		IdleAnim, IdleAnim, WalkSequence, PlayRate, PreviewMesh);
}

bool SavePackage(UObject* Asset)
{
	UPackage* Package = Asset->GetOutermost();
	const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	return UPackage::SavePackage(Package, Asset, *File, Args);
}

UEdGraphNode* FindNode(const UAnimBlueprint* Blueprint, UClass* NodeClass)
{
	TArray<UEdGraph*> Graphs;
	if (Blueprint != nullptr)
	{
		Blueprint->GetAllGraphs(Graphs);
	}
	for (const UEdGraph* Graph : Graphs)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node != nullptr && Node->IsA(NodeClass))
			{
				return Node;
			}
		}
	}
	return nullptr;
}
// A Body edit that fails in the middle of Apply: after the graphs, a Class Defaults write of a
// property the class does not have.
FString WithMidApplyFailure(const FString& Body)
{
	return Body + TEXT(",\"ClassDefaults\":{\"NoSuchProperty\":1}");
}

// A Body edit that fails at compile: a ForEachLoop whose Array type is undetermined.
FString WithCompileFailure(const FString& Body)
{
	return Body + TEXT(",\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":[")
		TEXT("{\"Id\":\"Update\",\"Action\":\"K2:Event:/Script/Engine.AnimInstance:BlueprintUpdateAnimation\"},")
		TEXT("{\"Id\":\"Loop\",\"Action\":\"K2:ForEachLoop\"}],")
		TEXT("\"Links\":[{\"From\":\"Update:then\",\"To\":\"Loop:Exec\"}]}]");
}

// The production-service stages for one graph feature, from two Bodies (A, B) that differ in it:
//  1. Create with A in one apply (compiles, Diff clean, Check(A));
//  2. the extraction re-applies as a no-op (no compile, no save, package file untouched);
//  3. Update to B (compiles, Diff clean, Check(B));
//  4. A + a failure in the middle of Apply rolls back to B; 5. A + a compile failure rolls back to B;
//  6. a human edit (HumanEdit + a variable, in a transaction) saved, then the AI applies A: Ctrl+Z
//     afterwards leaves the asset as applied;
//  7. unloaded and reloaded from disk, it extracts the same.
// Both Bodies list the Variables so the AI apply of step 6 replaces the human variable.
struct FFeatureStages
{
	FString BodyA;
	FString BodyB;
	TFunction<void(UAnimBlueprint&)> HumanEdit;
	TFunction<void(FAutomationTestBase&, UAnimBlueprint&, bool bA)> Check;
};

bool RunFeatureStages(FAutomationTestBase& Test, const FTarget& Target, const FFeatureStages& Stages)
{
	// 1.
	const FString Created = Document(Target, TEXT("Create"), Stages.BodyA);
	if (!Test.TestTrue(TEXT("create with the feature applies"), Apply(Test, Target, Created, TEXT("FeatureCreate")).IsSuccess()))
	{
		return false;
	}
	UAnimBlueprint* Blueprint = Target.Get();
	if (!Test.TestNotNull(TEXT("Animation Blueprint exists"), Blueprint))
	{
		return false;
	}
	Test.TestTrue(TEXT("created Animation Blueprint compiles"), CompilesClean(Blueprint));
	Stages.Check(Test, *Blueprint, true);
	TestNoDiff(Test, Created, TEXT("after create"));

	// 2.
	const TSharedPtr<FJsonObject> First = Extract(Test, Target, TEXT("feature extract"));
	if (!First.IsValid())
	{
		return false;
	}
	First->SetStringField(TEXT("Action"), TEXT("Update"));
	const FString Extracted = Serialize(First);
	Test.AddInfo(FString::Printf(TEXT("extracted: %s"), *Extracted));
	const FDateTime SavedStamp = IFileManager::Get().GetTimeStamp(*Target.PackageFile);
	int32 Compiles = 0;
	const FDelegateHandle CompiledHandle = Blueprint->OnCompiled().AddLambda([&Compiles](UBlueprint*) { ++Compiles; });
	const FAssetDocumentResult Reapplied = Apply(Test, Target, Extracted, TEXT("FeatureReapply"));
	Blueprint->OnCompiled().Remove(CompiledHandle);
	Test.TestTrue(TEXT("extraction re-applies"), Reapplied.IsSuccess());
	Test.TestEqual(TEXT("no-op re-apply does not recompile"), Compiles, 0);
	Test.TestFalse(TEXT("no-op re-apply reports no save"), Reapplied.bSavedAsset);
	Test.TestFalse(TEXT("no-op re-apply leaves the package clean"), Blueprint->GetOutermost()->IsDirty());
	Test.TestEqual(TEXT("no-op re-apply does not rewrite the package file"), IFileManager::Get().GetTimeStamp(*Target.PackageFile), SavedStamp);
	First->SetStringField(TEXT("Action"), TEXT("CreateOrUpdate"));
	ExtractEquals(Test, Target, Serialize(First), TEXT("after no-op re-apply"));

	// 3.
	const FString Edited = Document(Target, TEXT("Update"), Stages.BodyB);
	if (!Test.TestTrue(TEXT("feature edit applies"), Apply(Test, Target, Edited, TEXT("FeatureEdit")).IsSuccess()))
	{
		return false;
	}
	Blueprint = Target.Get();
	Test.TestTrue(TEXT("edited Animation Blueprint compiles"), CompilesClean(Blueprint));
	Stages.Check(Test, *Blueprint, false);
	TestNoDiff(Test, Edited, TEXT("after edit"));
	const FString EditedBaseline = Serialize(Extract(Test, Target, TEXT("edited extract")));

	// 4.
	Test.TestFalse(TEXT("mid-apply failure fails"),
		Apply(Test, Target, Document(Target, TEXT("Update"), WithMidApplyFailure(Stages.BodyA)), TEXT("FeatureMidApplyFailure")).IsSuccess());
	Blueprint = Target.Get();
	Test.TestTrue(TEXT("rolled-back Animation Blueprint compiles"), CompilesClean(Blueprint));
	Test.TestTrue(TEXT("rolled-back package is clean"), Blueprint != nullptr && !Blueprint->GetOutermost()->IsDirty());
	ExtractEquals(Test, Target, EditedBaseline, TEXT("after mid-apply rollback"));

	// 5.
	const FAssetDocumentResult CompileFailed = Apply(Test, Target, Document(Target, TEXT("Update"), WithCompileFailure(Stages.BodyA)), TEXT("FeatureCompileFailure"));
	Test.TestFalse(TEXT("compile failure fails"), CompileFailed.IsSuccess());
	Test.TestTrue(TEXT("failure is the compiler's"), CompileFailed.Diagnostics.ContainsByPredicate(
		[](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == TEXT("Graph.FinalizeFailed"); }));
	Blueprint = Target.Get();
	Test.TestTrue(TEXT("Animation Blueprint compiles after compile-failure rollback"), CompilesClean(Blueprint));
	ExtractEquals(Test, Target, EditedBaseline, TEXT("after compile rollback"));
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

	// 6.
	FString AppliedBaseline = EditedBaseline;
	if (Blueprint != nullptr && GEditor != nullptr)
	{
		ReleaseUndoHistory();
		{
			const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentAnimBlueprintPythonTests", "FeatureUserEdit", "Edit"));
			FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("UserAdded"), FEdGraphPinType(UEdGraphSchema_K2::PC_Boolean, NAME_None, nullptr, EPinContainerType::None, false, FEdGraphTerminalType()));
			if (Stages.HumanEdit)
			{
				Stages.HumanEdit(*Blueprint);
			}
		}
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		Test.TestTrue(TEXT("human edit is saved"), SavePackage(Blueprint));
		Test.TestTrue(TEXT("human edit is in the undo history"), GEditor->Trans->GetQueueLength() > 0);
		Test.TestTrue(TEXT("AI apply after the human edit succeeds"), Apply(Test, Target, Document(Target, TEXT("Update"), Stages.BodyA), TEXT("FeatureAfterUserEdit")).IsSuccess());
		AppliedBaseline = Serialize(Extract(Test, Target, TEXT("after AI apply")));
		GEditor->UndoTransaction();
		Blueprint = Target.Get();
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		Test.TestTrue(TEXT("Animation Blueprint compiles after Ctrl+Z"), CompilesClean(Blueprint));
		Test.TestFalse(TEXT("Ctrl+Z does not restore the human variable"), FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, TEXT("UserAdded")) != INDEX_NONE);
		Stages.Check(Test, *Blueprint, true);
		ExtractEquals(Test, Target, AppliedBaseline, TEXT("after Ctrl+Z"));
		if (Blueprint->GetOutermost()->IsDirty())
		{
			Test.TestTrue(TEXT("package saves after Ctrl+Z"), SavePackage(Blueprint));
		}
	}

	// 7.
	Blueprint = Target.Get();
	if (Blueprint != nullptr)
	{
		ReleaseUndoHistory();
		UPackageTools::UnloadPackages({Blueprint->GetOutermost()});
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		Test.TestNull(TEXT("Animation Blueprint is unloaded"), Target.Get());
		ExtractEquals(Test, Target, AppliedBaseline, TEXT("after reload from disk"));
		Blueprint = Target.Get();
		Test.TestTrue(TEXT("reloaded Animation Blueprint compiles"), CompilesClean(Blueprint));
		if (Blueprint != nullptr)
		{
			Stages.Check(Test, *Blueprint, true);
		}
	}
	return true;
}

template <typename TNode>
TArray<TNode*> NodesOf(UAnimBlueprint* Blueprint)
{
	TArray<TNode*> Nodes;
	if (Blueprint != nullptr)
	{
		FBlueprintEditorUtils::GetAllNodesOfClass<TNode>(Blueprint, Nodes);
	}
	return Nodes;
}
} // namespace AssetDocumentAnimBlueprintPythonTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimBlueprintPythonTest, "UEAssetDoc.AnimBlueprint.Python",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimBlueprintPythonTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAnimBlueprintPythonTests;
	const FTarget Target(TEXT("ABP_Hero"));
	const FTarget FailedCreate(TEXT("ABP_Failed"));
	const FTarget TemplateTarget(TEXT("ABP_Template"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
		FailedCreate.Cleanup();
		TemplateTarget.Cleanup();
	};

	// 1. One apply creates and populates the Animation Blueprint.
	const FString Created = Document(Target, TEXT("Create"), InitialBody(WalkAnim, true, 1.25));
	if (!TestTrue(TEXT("create applies"), Apply(*this, Target, Created, TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	UAnimBlueprint* Blueprint = Target.Get();
	if (!TestNotNull(TEXT("Animation Blueprint exists"), Blueprint))
	{
		return false;
	}
	TestTrue(TEXT("created Animation Blueprint compiles"), CompilesClean(Blueprint));
	TestTrue(TEXT("package is saved"), IFileManager::Get().FileExists(*Target.PackageFile));
	TestEqual(TEXT("target skeleton"), Blueprint->TargetSkeleton ? Blueprint->TargetSkeleton->GetPathName() : FString(), FString(Skeleton));
	TestEqual(TEXT("three variables"), Blueprint->NewVariables.Num(), 3);
	TestTrue(TEXT("Class Settings / optimization property set"), Blueprint->bWarnAboutBlueprintUsage);
	TestTrue(TEXT("preview mesh set"), Blueprint->GetPreviewMesh() != nullptr);
	TestTrue(TEXT("sync group set"), Blueprint->Groups.Num() == 1 && Blueprint->Groups[0].Name == TEXT("Feet"));
	const USceneThumbnailInfo* Thumbnail = Cast<USceneThumbnailInfo>(Blueprint->ThumbnailInfo);
	TestTrue(TEXT("thumbnail camera set"), Thumbnail != nullptr && FMath::IsNearlyEqual(Thumbnail->OrbitYaw, 45.f));
	const UAnimGraphNode_LinkedAnimLayer* LayerNode = Cast<UAnimGraphNode_LinkedAnimLayer>(FindNode(Blueprint, UAnimGraphNode_LinkedAnimLayer::StaticClass()));
	TestTrue(TEXT("linked anim layer node uses the own layer"), LayerNode != nullptr && LayerNode->Node.Layer == TEXT("UpperLayer"));
	const UAnimGraphNode_UseCachedPose* UseCache = Cast<UAnimGraphNode_UseCachedPose>(FindNode(Blueprint, UAnimGraphNode_UseCachedPose::StaticClass()));
	TestTrue(TEXT("use cached pose node reads the LocoCache save node"), UseCache != nullptr && UseCache->SaveCachedPoseNode.IsValid()
		&& UseCache->SaveCachedPoseNode->CacheName == TEXT("LocoCache"));
	TestNoDiff(*this, Created, TEXT("after create"));

	// 2. Re-applying the extraction is a no-op.
	const TSharedPtr<FJsonObject> First = Extract(*this, Target, TEXT("first extract"));
	if (!First.IsValid())
	{
		return false;
	}
	First->SetStringField(TEXT("Action"), TEXT("Update"));
	const FString Extracted = Serialize(First);
	AddInfo(FString::Printf(TEXT("extracted: %s"), *Extracted));
	TestFalse(TEXT("created package is saved clean"), Blueprint->GetOutermost()->IsDirty());
	const FDateTime SavedStamp = IFileManager::Get().GetTimeStamp(*Target.PackageFile);
	int32 Compiles = 0;
	const FDelegateHandle CompiledHandle = Blueprint->OnCompiled().AddLambda([&Compiles](UBlueprint*) { ++Compiles; });
	const FAssetDocumentResult Reapplied = Apply(*this, Target, Extracted, TEXT("Reapply"));
	Blueprint->OnCompiled().Remove(CompiledHandle);
	TestTrue(TEXT("extraction re-applies"), Reapplied.IsSuccess());
	TestEqual(TEXT("no-op re-apply does not recompile"), Compiles, 0);
	TestFalse(TEXT("no-op re-apply leaves the package clean"), Blueprint->GetOutermost()->IsDirty());
	TestFalse(TEXT("no-op re-apply reports no save"), Reapplied.bSavedAsset);
	TestEqual(TEXT("no-op re-apply does not rewrite the package file"), IFileManager::Get().GetTimeStamp(*Target.PackageFile), SavedStamp);
	First->SetStringField(TEXT("Action"), TEXT("CreateOrUpdate"));
	const FString Baseline = Serialize(First);
	ExtractEquals(*this, Target, Baseline, TEXT("after re-apply"));
	TestTrue(TEXT("same Animation Blueprint object"), Target.Get() == Blueprint);

	// 2b. A layout-only edit of the extraction (a node inside a state graph) is not a no-op.
	{
		const TSharedPtr<FJsonObject> Moved = Parse(Extracted);
		TSharedPtr<FJsonObject> Node;
		const TSharedPtr<FJsonObject> AnimGraph = Moved->GetObjectField(TEXT("Body"))->GetObjectField(TEXT("AnimGraph"));
		for (const TSharedPtr<FJsonValue>& Value : AnimGraph->GetArrayField(TEXT("Nodes")))
		{
			FString Action;
			if (Value->AsObject()->TryGetStringField(TEXT("Action"), Action) && Action == TEXT("Anim:TwoWayBlend"))
			{
				Node = Value->AsObject();
			}
		}
		if (TestTrue(TEXT("extracted AnimGraph has the positioned blend node"), Node.IsValid() && Node->HasField(TEXT("Position"))))
		{
			const TSharedPtr<FJsonObject> Position = Node->GetObjectField(TEXT("Position"));
			const int32 MovedX = static_cast<int32>(Position->GetNumberField(TEXT("X"))) + 160;
			Position->SetNumberField(TEXT("X"), MovedX);
			const FAssetDocumentResult MovedResult = Apply(*this, Target, Serialize(Moved), TEXT("Move"));
			TestTrue(TEXT("layout-only edit applies"), MovedResult.IsSuccess());
			TestTrue(TEXT("layout-only edit is saved"), MovedResult.bSavedAsset);
			FGuid NodeGuid;
			FGuid::Parse(Node->GetStringField(TEXT("NodeGuid")), NodeGuid);
			TArray<UEdGraph*> Graphs;
			Blueprint->GetAllGraphs(Graphs);
			const UEdGraphNode* LiveNode = nullptr;
			for (const UEdGraph* Graph : Graphs)
			{
				for (const UEdGraphNode* Candidate : Graph->Nodes)
				{
					LiveNode = Candidate && Candidate->NodeGuid == NodeGuid ? Candidate : LiveNode;
				}
			}
			TestTrue(TEXT("layout-only edit moved the node"), LiveNode != nullptr && LiveNode->NodePosX == MovedX);
		}
	}

	// 3. A second apply edits a state's sequence and play rate and removes a variable.
	const FString Edited = Document(Target, TEXT("Update"), InitialBody(IdleAnim, false, 2.0));
	if (TestTrue(TEXT("edit applies"), Apply(*this, Target, Edited, TEXT("Edit")).IsSuccess()))
	{
		TestNoDiff(*this, Edited, TEXT("after edit"));
		TestEqual(TEXT("Count was removed"), Blueprint->NewVariables.Num(), 2);
		TestTrue(TEXT("edited Animation Blueprint compiles"), CompilesClean(Blueprint));
	}
	const FString EditedBaseline = Serialize(Extract(*this, Target, TEXT("edited extract")));

	// 4. Failure in the middle of Apply (variables already changed, then an unknown anim node).
	const FString MidApply = Document(Target, TEXT("Update"), TEXT(
		"\"Variables\":[{\"Name\":\"Shield\",\"Type\":\"int\",\"DefaultValue\":9}],"
		"\"AnimGraph\":{\"Nodes\":[{\"Id\":\"Broken\",\"Action\":\"Anim:NoSuchNode\"}],\"Links\":[]}"));
	TestFalse(TEXT("mid-apply failure fails"), Apply(*this, Target, MidApply, TEXT("MidApplyFailure")).IsSuccess());
	Blueprint = Target.Get();
	TestTrue(TEXT("rolled-back Animation Blueprint compiles"), CompilesClean(Blueprint));
	TestTrue(TEXT("rolled-back package is clean"), Blueprint != nullptr && !Blueprint->GetOutermost()->IsDirty());
	ExtractEquals(*this, Target, EditedBaseline, TEXT("after mid-apply rollback"));

	// 5. Failure at compile: a ForEachLoop whose Array type is undetermined (editor compiler error).
	const FString CompileError = Document(Target, TEXT("Update"), TEXT(
		"\"Variables\":[{\"Name\":\"Speed\",\"Type\":\"float\",\"DefaultValue\":1}],"
		"\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":["
		"{\"Id\":\"Update\",\"Action\":\"K2:Event:/Script/Engine.AnimInstance:BlueprintUpdateAnimation\"},"
		"{\"Id\":\"Loop\",\"Action\":\"K2:ForEachLoop\"}],"
		"\"Links\":[{\"From\":\"Update:then\",\"To\":\"Loop:Exec\"}]}]"));
	const FAssetDocumentResult CompileFailed = Apply(*this, Target, CompileError, TEXT("CompileFailure"));
	TestFalse(TEXT("compile failure fails"), CompileFailed.IsSuccess());
	TestTrue(TEXT("failure is the compiler's"), CompileFailed.Diagnostics.ContainsByPredicate(
		[](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == TEXT("Graph.FinalizeFailed"); }));
	Blueprint = Target.Get();
	TestTrue(TEXT("Animation Blueprint compiles after compile-failure rollback"), CompilesClean(Blueprint));
	ExtractEquals(*this, Target, EditedBaseline, TEXT("after compile rollback"));

	// The rolled-back apply wired new nodes to the default Blueprint Update Animation event;
	// destroying them must not touch the restored event's pins.
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

	// 6. A human edit (recorded in the undo history) and save, then an AI apply: Ctrl+Z afterwards
	// cannot bring back pre-apply state into the applied asset.
	if (Blueprint != nullptr && GEditor != nullptr)
	{
		ReleaseUndoHistory();
		{
			const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentAnimBlueprintPythonTests", "UserEdit", "Add Variable"));
			FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("UserAdded"), FEdGraphPinType(UEdGraphSchema_K2::PC_Boolean, NAME_None, nullptr, EPinContainerType::None, false, FEdGraphTerminalType()));
		}
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		TestTrue(TEXT("human edit is saved"), SavePackage(Blueprint));
		TestTrue(TEXT("human edit is in the undo history"), GEditor->Trans->GetQueueLength() > 0);
		TestTrue(TEXT("AI apply after the human edit succeeds"), Apply(*this, Target, Edited, TEXT("AfterUserEdit")).IsSuccess());
		const FString AppliedBaseline = Serialize(Extract(*this, Target, TEXT("after AI apply")));
		GEditor->UndoTransaction();
		Blueprint = Target.Get();
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		TestTrue(TEXT("Animation Blueprint compiles after Ctrl+Z"), CompilesClean(Blueprint));
		TestFalse(TEXT("Ctrl+Z does not restore the overwritten human variable"),
			FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, TEXT("UserAdded")) != INDEX_NONE);
		ExtractEquals(*this, Target, AppliedBaseline, TEXT("after Ctrl+Z"));
		TestTrue(TEXT("extraction after Ctrl+Z equals the edited baseline"), AppliedBaseline == EditedBaseline);
		if (Blueprint->GetOutermost()->IsDirty())
		{
			// The test's own compile may dirty the package; the reload step below needs it saved.
			TestTrue(TEXT("package saves after Ctrl+Z"), SavePackage(Blueprint));
		}
	}

	// 7. The saved package, unloaded and loaded back from disk, extracts to the same document.
	Blueprint = Target.Get();
	if (Blueprint != nullptr)
	{
		ReleaseUndoHistory();
		UPackageTools::UnloadPackages({Blueprint->GetOutermost()});
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		Blueprint = nullptr;
		TestNull(TEXT("Animation Blueprint is unloaded"), Target.Get());
		ExtractEquals(*this, Target, EditedBaseline, TEXT("after reload from disk"));
		TestTrue(TEXT("Animation Blueprint is loaded from disk again"), Target.Get() != nullptr);
		TestTrue(TEXT("reloaded Animation Blueprint compiles"), CompilesClean(Target.Get()));
	}

	// 8. A failed create leaves no asset, package file or registry entry.
	const FString BadCreate = Document(FailedCreate, TEXT("Create"), TEXT(
		"\"Variables\":[{\"Name\":\"Speed\",\"Type\":\"float\"}],"
		"\"AnimGraph\":{\"Nodes\":[{\"Id\":\"Broken\",\"Action\":\"Anim:NoSuchNode\"}],\"Links\":[]}"));
	TestFalse(TEXT("failed create fails"), Apply(*this, FailedCreate, BadCreate, TEXT("FailedCreate")).IsSuccess());
	TestNull(TEXT("no created Animation Blueprint remains"), FailedCreate.Get());
	TestFalse(TEXT("no package file"), IFileManager::Get().FileExists(*FailedCreate.PackageFile));
	TestFalse(TEXT("no registry entry"),
		FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(FailedCreate.ObjectPath)).IsValid());

	// 9. A template Animation Blueprint (no skeleton) is created through the same lifecycle.
	const FString TemplateDocument = FString::Printf(
		TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.AnimBlueprint\",\"Action\":\"Create\",")
		TEXT("\"Body\":{\"ParentClass\":\"/Script/Engine.AnimInstance\",\"Template\":true,")
		TEXT("\"Variables\":[{\"Name\":\"Weight\",\"Type\":\"float\"}]}}"), *TemplateTarget.PackageName);
	if (TestTrue(TEXT("template create applies"), Apply(*this, TemplateTarget, TemplateDocument, TEXT("Template")).IsSuccess()))
	{
		const UAnimBlueprint* Template = TemplateTarget.Get();
		TestTrue(TEXT("template flag and no skeleton"), Template != nullptr && Template->bIsTemplate && Template->TargetSkeleton == nullptr);
		TestNoDiff(*this, TemplateDocument, TEXT("template"));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimBlueprintLinkedGraphAndLayerNodesTest,
	"UEAssetDoc.AnimBlueprint.AnimGraph.LinkedGraphAndLayerNodesRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimBlueprintLinkedGraphAndLayerNodesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAnimBlueprintPythonTests;
	const FTarget Library(TEXT("ABP_LayerLibrary"));
	const FTarget Host(TEXT("ABP_LayerHost"));
	ON_SCOPE_EXIT
	{
		Host.Cleanup();
		Library.Cleanup();
	};
	// The linked class: an anim layer and a pose in its AnimGraph.
	const FString LibraryDoc = FString::Printf(
		TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.AnimBlueprint\",\"Action\":\"Create\",")
		TEXT("\"Body\":{\"ParentClass\":\"/Script/Engine.AnimInstance\",\"TargetSkeleton\":\"%s\",")
		TEXT("\"AnimLayers\":[{\"Name\":\"LibraryLayer\",\"Nodes\":[{\"Id\":\"Play\",\"Action\":\"Anim:SequencePlayer\",")
		TEXT("\"Properties\":{\"Node.Sequence\":\"%s\"}}],\"Links\":[{\"From\":\"Play:Pose\",\"To\":\"Output:Result\"}]}],")
		TEXT("\"AnimGraph\":{\"Nodes\":[{\"Id\":\"Play\",\"Action\":\"Anim:SequencePlayer\",\"Properties\":{\"Node.Sequence\":\"%s\"}}],")
		TEXT("\"Links\":[{\"From\":\"Play:Pose\",\"To\":\"Output:Result\"}]}}}"),
		*Library.PackageName, Skeleton, WalkAnim, IdleAnim);
	if (!TestTrue(TEXT("library Animation Blueprint is created"), Apply(*this, Library, LibraryDoc, TEXT("Library")).IsSuccess()))
	{
		return false;
	}
	const FString LibraryClass = Library.PackageName + TEXT(".") + Library.AssetName + TEXT("_C");

	// Host: own layers used by Linked Anim Layer nodes created in the same apply (the signature
	// needs the compiled layer), and a Linked Anim Graph node running the library class.
	const FString HostDoc = FString::Printf(
		TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.AnimBlueprint\",\"Action\":\"Create\",")
		TEXT("\"Body\":{\"ParentClass\":\"/Script/Engine.AnimInstance\",\"TargetSkeleton\":\"%s\",")
		TEXT("\"AnimLayers\":[")
		TEXT("{\"Name\":\"FullBody\",\"Nodes\":[{\"Id\":\"Play\",\"Action\":\"Anim:SequencePlayer\",\"Properties\":{\"Node.Sequence\":\"%s\"}}],")
		TEXT("\"Links\":[{\"From\":\"Play:Pose\",\"To\":\"Output:Result\"}]},")
		TEXT("{\"Name\":\"Additive\",\"Nodes\":[],\"Links\":[]}],")
		TEXT("\"AnimGraph\":{\"Nodes\":[")
		TEXT("{\"Id\":\"Full\",\"Action\":\"Anim:LinkedAnimLayer:FullBody\"},")
		TEXT("{\"Id\":\"Extra\",\"Action\":\"Anim:LinkedAnimLayer:Additive\"},")
		TEXT("{\"Id\":\"Linked\",\"Action\":\"Anim:LinkedAnimGraph\",\"Properties\":{\"Node.InstanceClass\":\"%s\"}},")
		TEXT("{\"Id\":\"Blend\",\"Action\":\"Anim:TwoWayBlend\"}],")
		TEXT("\"Links\":[{\"From\":\"Full:Pose\",\"To\":\"Blend:A\"},{\"From\":\"Linked:Pose\",\"To\":\"Blend:B\"},")
		TEXT("{\"From\":\"Blend:Pose\",\"To\":\"Output:Result\"}]}}}"),
		*Host.PackageName, Skeleton, IdleAnim, *LibraryClass);
	if (!TestTrue(TEXT("host with linked layer and linked graph nodes is created in one apply"), Apply(*this, Host, HostDoc, TEXT("Host")).IsSuccess()))
	{
		return false;
	}
	UAnimBlueprint* Blueprint = Host.Get();
	TestTrue(TEXT("host compiles"), CompilesClean(Blueprint));
	const UAnimGraphNode_LinkedAnimGraph* LinkedGraph = Cast<UAnimGraphNode_LinkedAnimGraph>(FindNode(Blueprint, UAnimGraphNode_LinkedAnimGraph::StaticClass()));
	TestTrue(TEXT("linked anim graph runs the library class"), LinkedGraph != nullptr && LinkedGraph->Node.InstanceClass != nullptr
		&& LinkedGraph->Node.InstanceClass->GetPathName() == LibraryClass);
	TestNoDiff(*this, HostDoc, TEXT("host"));
	const TSharedPtr<FJsonObject> Extracted = Extract(*this, Host, TEXT("host extract"));
	if (Extracted.IsValid())
	{
		Extracted->SetStringField(TEXT("Action"), TEXT("Update"));
		const FString Text = Serialize(Extracted);
		AddInfo(FString::Printf(TEXT("host extract: %s"), *Text));
		const FAssetDocumentResult Reapplied = Apply(*this, Host, Text, TEXT("HostReapply"));
		TestTrue(TEXT("host extraction re-applies"), Reapplied.IsSuccess());
		TestFalse(TEXT("host extraction re-apply is a no-op"), Reapplied.bSavedAsset);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimBlueprintLayerInterfaceTest,
	"UEAssetDoc.AnimBlueprint.AnimGraph.LayerInterfaceRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimBlueprintLayerInterfaceTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAnimBlueprintPythonTests;
	const FTarget Interface(TEXT("ALI_Locomotion"));
	const FTarget Host(TEXT("ABP_InterfaceHost"));
	ON_SCOPE_EXIT
	{
		Host.Cleanup();
		Interface.Cleanup();
	};
	// An Anim Layer Interface created by a document (Content Browser > Animation > Animation Layer
	// Interface, My Blueprint > Add Animation Layer, layer Details > Inputs "+" and Layer Group).
	auto InterfaceDocument = [&Interface](const TCHAR* Action, const FString& Layers, const FString& Extra = FString())
	{
		return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.AnimBlueprint\",\"Action\":\"%s\",")
			TEXT("\"Body\":{\"ParentClass\":\"/Script/Engine.AnimLayerInterface\",\"AnimLayers\":%s%s}}"),
			*Interface.PackageName, Action, *Layers, *Extra);
	};
	const FString LayersA = TEXT(R"json([
  {"Name":"InterfaceLayer","Nodes":[{"Id":"Output","Properties":{"LayerGroup":"Locomotion"}}],"Links":[]},
  {"Name":"PoseLayer","Nodes":[{"Id":"Base","Action":"Anim:LinkedInputPose","Properties":{"Node.Name":"BasePose"}}],"Links":[]}])json");
	const FString LayersB = TEXT(R"json([
  {"Name":"InterfaceLayer","Nodes":[{"Id":"Output","Properties":{"LayerGroup":""}}],"Links":[]},
  {"Name":"PoseLayer","Nodes":[{"Id":"Output","Properties":{"LayerGroup":"Upper"}},
    {"Id":"Base","Action":"Anim:LinkedInputPose","Properties":{"Node.Name":"BasePose"}},
    {"Id":"Extra","Action":"Anim:LinkedInputPose","Properties":{"Node.Name":"ExtraPose"}}],"Links":[]},
  {"Name":"SpareLayer","Nodes":[],"Links":[]}])json");
	auto FindLayer = [](const UAnimBlueprint* Blueprint, const TCHAR* Name) -> UEdGraph*
	{
		for (UEdGraph* Graph : Blueprint != nullptr ? Blueprint->FunctionGraphs : TArray<TObjectPtr<UEdGraph>>())
		{
			if (Graph != nullptr && Graph->GetName() == Name)
			{
				return Graph;
			}
		}
		return nullptr;
	};
	auto LayerGroup = [](const UEdGraph* Layer) -> FString
	{
		for (const UEdGraphNode* Node : Layer != nullptr ? Layer->Nodes : TArray<TObjectPtr<UEdGraphNode>>())
		{
			if (const UAnimGraphNode_Root* Root = Cast<UAnimGraphNode_Root>(Node))
			{
				return Root->Node.GetGroup().ToString();
			}
		}
		return TEXT("<no root>");
	};
	auto InputPoses = [](const UEdGraph* Layer)
	{
		TArray<FString> Names;
		for (const UEdGraphNode* Node : Layer != nullptr ? Layer->Nodes : TArray<TObjectPtr<UEdGraphNode>>())
		{
			if (const UAnimGraphNode_LinkedInputPose* Pose = Cast<UAnimGraphNode_LinkedInputPose>(Node))
			{
				Names.Add(Pose->Node.Name.ToString());
			}
		}
		Names.Sort();
		return Names;
	};
	const FString Created = InterfaceDocument(TEXT("Create"), LayersA);
	if (!TestTrue(TEXT("Anim Layer Interface is created by a document"), Apply(*this, Interface, Created, TEXT("InterfaceCreate")).IsSuccess()))
	{
		return false;
	}
	UAnimBlueprint* InterfaceBlueprint = Interface.Get();
	TestTrue(TEXT("created asset is an Anim Layer Interface"), InterfaceBlueprint != nullptr && InterfaceBlueprint->BlueprintType == BPTYPE_Interface
		&& InterfaceBlueprint->ParentClass == UAnimLayerInterface::StaticClass() && InterfaceBlueprint->TargetSkeleton == nullptr);
	TestTrue(TEXT("Anim Layer Interface compiles"), CompilesClean(InterfaceBlueprint));
	TestEqual(TEXT("layer group set"), LayerGroup(FindLayer(InterfaceBlueprint, TEXT("InterfaceLayer"))), FString(TEXT("Locomotion")));
	TestEqual(TEXT("layer input pose added"), InputPoses(FindLayer(InterfaceBlueprint, TEXT("PoseLayer"))), TArray<FString>({TEXT("BasePose")}));
	TestNoDiff(*this, Created, TEXT("interface create"));
	const TSharedPtr<FJsonObject> InterfaceDoc = Extract(*this, Interface, TEXT("interface extract"));
	if (TestTrue(TEXT("Anim Layer Interface extracts"), InterfaceDoc.IsValid()))
	{
		InterfaceDoc->SetStringField(TEXT("Action"), TEXT("Update"));
		AddInfo(FString::Printf(TEXT("interface extract: %s"), *Serialize(InterfaceDoc)));
		const FAssetDocumentResult Reapplied = Apply(*this, Interface, Serialize(InterfaceDoc), TEXT("InterfaceReapply"));
		TestTrue(TEXT("unchanged Anim Layer Interface re-apply is a no-op"), Reapplied.IsSuccess() && !Reapplied.bSavedAsset);
	}

	// Edit: groups change, an input pose and a layer are added.
	const FString Edited = InterfaceDocument(TEXT("Update"), LayersB);
	if (TestTrue(TEXT("Anim Layer Interface edit applies"), Apply(*this, Interface, Edited, TEXT("InterfaceEdit")).IsSuccess()))
	{
		InterfaceBlueprint = Interface.Get();
		TestTrue(TEXT("edited Anim Layer Interface compiles"), CompilesClean(InterfaceBlueprint));
		TestEqual(TEXT("layer ungrouped"), LayerGroup(FindLayer(InterfaceBlueprint, TEXT("InterfaceLayer"))), FString(TEXT("None")));
		TestEqual(TEXT("layer group changed"), LayerGroup(FindLayer(InterfaceBlueprint, TEXT("PoseLayer"))), FString(TEXT("Upper")));
		TestEqual(TEXT("second input pose added"), InputPoses(FindLayer(InterfaceBlueprint, TEXT("PoseLayer"))), TArray<FString>({TEXT("BasePose"), TEXT("ExtraPose")}));
		TestNotNull(TEXT("layer added"), FindLayer(InterfaceBlueprint, TEXT("SpareLayer")));
		TestNoDiff(*this, Edited, TEXT("interface edit"));
	}
	const FString EditedBaseline = Serialize(Extract(*this, Interface, TEXT("edited interface extract")));

	// A failure in the middle of Apply (layers changed, then a bad Class Defaults write) rolls back.
	TestFalse(TEXT("interface mid-apply failure fails"), Apply(*this, Interface,
		InterfaceDocument(TEXT("Update"), LayersA, TEXT(",\"ClassDefaults\":{\"NoSuchProperty\":1}")), TEXT("InterfaceMidApply")).IsSuccess());
	TestTrue(TEXT("rolled-back Anim Layer Interface compiles"), CompilesClean(Interface.Get()));
	ExtractEquals(*this, Interface, EditedBaseline, TEXT("interface after mid-apply rollback"));

	// What an interface cannot hold, and the type, are refused.
	const FAssetDocumentResult Variable = Apply(*this, Interface, InterfaceDocument(TEXT("Update"), LayersB,
		TEXT(",\"Variables\":[{\"Name\":\"Weight\",\"Type\":\"float\"}]")), TEXT("InterfaceVariable"));
	TestFalse(TEXT("a variable on an Anim Layer Interface is refused"), Variable.IsSuccess());
	TestTrue(TEXT("refusal names the interface rule"), Variable.Message.Contains(TEXT("AnimBlueprint.InvalidLayerInterface"))
		|| Variable.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == TEXT("AnimBlueprint.InvalidLayerInterface"); }));
	const FAssetDocumentResult Retype = Apply(*this, Interface, FString::Printf(
		TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.AnimBlueprint\",\"Action\":\"Update\",")
		TEXT("\"Body\":{\"ParentClass\":\"/Script/Engine.AnimInstance\"}}"), *Interface.PackageName), TEXT("InterfaceRetype"));
	TestFalse(TEXT("turning the interface into an Animation Blueprint is refused"), Retype.IsSuccess());
	TestTrue(TEXT("refusal names the fixed type"), Retype.Message.Contains(TEXT("AnimBlueprint.BlueprintTypeImmutable"))
		|| Retype.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == TEXT("AnimBlueprint.BlueprintTypeImmutable"); }));
	ExtractEquals(*this, Interface, EditedBaseline, TEXT("interface after refusals"));

	// A human edit (layer group, in the undo history) saved, then the AI apply: Ctrl+Z afterwards
	// leaves the interface as applied.
	if (UAnimBlueprint* Human = Interface.Get(); Human != nullptr && GEditor != nullptr)
	{
		ReleaseUndoHistory();
		FBlueprintEditorUtils::SetAnimationGraphLayerGroup(FindLayer(Human, TEXT("InterfaceLayer")), FText::FromString(TEXT("Human")));
		FKismetEditorUtilities::CompileBlueprint(Human);
		TestTrue(TEXT("human interface edit is saved"), SavePackage(Human));
		TestTrue(TEXT("human interface edit is in the undo history"), GEditor->Trans->GetQueueLength() > 0);
		TestTrue(TEXT("AI apply after the human interface edit succeeds"), Apply(*this, Interface, Edited, TEXT("InterfaceAfterUserEdit")).IsSuccess());
		GEditor->UndoTransaction();
		Human = Interface.Get();
		FKismetEditorUtilities::CompileBlueprint(Human);
		TestTrue(TEXT("Anim Layer Interface compiles after Ctrl+Z"), CompilesClean(Human));
		TestEqual(TEXT("Ctrl+Z does not bring back the human layer group"), LayerGroup(FindLayer(Human, TEXT("InterfaceLayer"))), FString(TEXT("None")));
		ExtractEquals(*this, Interface, EditedBaseline, TEXT("interface after Ctrl+Z"));
		if (Human->GetOutermost()->IsDirty())
		{
			TestTrue(TEXT("interface saves after Ctrl+Z"), SavePackage(Human));
		}
	}

	// Unloaded and reloaded from disk, it extracts the same.
	if (UAnimBlueprint* Loaded = Interface.Get())
	{
		ReleaseUndoHistory();
		UPackageTools::UnloadPackages({Loaded->GetOutermost()});
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		TestNull(TEXT("Anim Layer Interface is unloaded"), Interface.Get());
		ExtractEquals(*this, Interface, EditedBaseline, TEXT("interface after reload"));
	}
	const FString InterfaceClass = Interface.PackageName + TEXT(".") + Interface.AssetName + TEXT("_C");

	// Host: implements the interface, fills the interface layer and uses it through a
	// Linked Anim Layer node, all in one apply.
	const FString HostDoc = FString::Printf(
		TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.AnimBlueprint\",\"Action\":\"Create\",")
		TEXT("\"Body\":{\"ParentClass\":\"/Script/Engine.AnimInstance\",\"TargetSkeleton\":\"%s\",")
		TEXT("\"ImplementedInterfaces\":[\"%s\"],")
		TEXT("\"AnimLayers\":[{\"Name\":\"InterfaceLayer\",\"Nodes\":[{\"Id\":\"Play\",\"Action\":\"Anim:SequencePlayer\",")
		TEXT("\"Properties\":{\"Node.Sequence\":\"%s\"}}],\"Links\":[{\"From\":\"Play:Pose\",\"To\":\"Output:Result\"}]},")
		TEXT("{\"Name\":\"PoseLayer\",\"Nodes\":[{\"Id\":\"Base\",\"Action\":\"Anim:LinkedInputPose\",\"Properties\":{\"Node.Name\":\"BasePose\"}},")
		TEXT("{\"Id\":\"Extra\",\"Action\":\"Anim:LinkedInputPose\",\"Properties\":{\"Node.Name\":\"ExtraPose\"}}],\"Links\":[{\"From\":\"Base:Pose\",\"To\":\"Output:Result\"}]},")
		TEXT("{\"Name\":\"SpareLayer\",\"Nodes\":[],\"Links\":[]},")
		TEXT("{\"Name\":\"OwnLayer\",\"Nodes\":[{\"Id\":\"In\",\"Action\":\"Anim:LinkedInputPose\",\"Properties\":{\"Node.Name\":\"OwnPose\"}}],\"Links\":[{\"From\":\"In:Pose\",\"To\":\"Output:Result\"}]}],")
		TEXT("\"AnimGraph\":{\"Nodes\":[{\"Id\":\"Layer\",\"Action\":\"Anim:LinkedAnimLayer:InterfaceLayer\"}],")
		TEXT("\"Links\":[{\"From\":\"Layer:Pose\",\"To\":\"Output:Result\"}]}}}"),
		*Host.PackageName, Skeleton, *InterfaceClass, WalkAnim);
	if (!TestTrue(TEXT("host implementing the interface is created in one apply"), Apply(*this, Host, HostDoc, TEXT("InterfaceHost")).IsSuccess()))
	{
		return false;
	}
	UAnimBlueprint* Blueprint = Host.Get();
	TestTrue(TEXT("host compiles"), CompilesClean(Blueprint));
	const UAnimGraphNode_LinkedAnimLayer* LayerNode = Cast<UAnimGraphNode_LinkedAnimLayer>(FindNode(Blueprint, UAnimGraphNode_LinkedAnimLayer::StaticClass()));
	TestTrue(TEXT("linked anim layer node uses the interface layer"), LayerNode != nullptr && LayerNode->Node.Layer == TEXT("InterfaceLayer")
		&& LayerNode->Node.Interface != nullptr && LayerNode->Node.Interface->GetPathName() == InterfaceClass);
	TestNoDiff(*this, HostDoc, TEXT("interface host"));
	const TSharedPtr<FJsonObject> Extracted = Extract(*this, Host, TEXT("interface host extract"));
	if (Extracted.IsValid())
	{
		Extracted->SetStringField(TEXT("Action"), TEXT("Update"));
		const FAssetDocumentResult Reapplied = Apply(*this, Host, Serialize(Extracted), TEXT("InterfaceHostReapply"));
		TestTrue(TEXT("interface host extraction re-applies"), Reapplied.IsSuccess());
		TestFalse(TEXT("interface host extraction re-apply is a no-op"), Reapplied.bSavedAsset);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimBlueprintStateMachineTest,
	"UEAssetDoc.AnimBlueprint.StateMachine.ConduitAliasAndTransitionSettings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimBlueprintStateMachineTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAnimBlueprintPythonTests;
	const FTarget Target(TEXT("ABP_StateMachine"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
	};
	// Idle -> Gate (conduit with a rule) -> Walk, an alias of Walk returning to Idle, transition details.
	auto Body = [](double Crossfade, bool bAlias)
	{
		return FString::Printf(TEXT(
			"\"Variables\":[{\"Name\":\"bMoving\",\"Type\":\"bool\"}],"
			"\"AnimGraph\":{\"Nodes\":[{\"Id\":\"Machine\",\"Action\":\"Anim:StateMachine\",\"Graph\":{\"Nodes\":["
			"{\"Id\":\"Idle\",\"Action\":\"SM:State\"},"
			"{\"Id\":\"Walk\",\"Action\":\"SM:State\"},"
			"{\"Id\":\"Gate\",\"Action\":\"SM:Conduit\",\"Graph\":{\"Nodes\":[{\"Id\":\"Moving\",\"Action\":\"K2:Get:bMoving\"}],"
			"\"Links\":[{\"From\":\"Moving:bMoving\",\"To\":\"Result:bCanEnterTransition\"}]}},"
			"{\"Id\":\"ToGate\",\"Action\":\"SM:Transition\",\"From\":\"Idle\",\"To\":\"Gate\",\"Graph\":{\"Nodes\":[{\"Id\":\"Moving\",\"Action\":\"K2:Get:bMoving\"}],\"Links\":[{\"From\":\"Moving:bMoving\",\"To\":\"Result:bCanEnterTransition\"}]},\"Properties\":{\"CrossfadeDuration\":%g,\"PriorityOrder\":2}},"
			"{\"Id\":\"ToWalk\",\"Action\":\"SM:Transition\",\"From\":\"Gate\",\"To\":\"Walk\",\"Graph\":{\"Nodes\":[{\"Id\":\"Moving\",\"Action\":\"K2:Get:bMoving\"}],\"Links\":[{\"From\":\"Moving:bMoving\",\"To\":\"Result:bCanEnterTransition\"}]}}%s],"
			"\"Links\":[{\"From\":\"Entry:Entry\",\"To\":\"Idle:In\"}]}}],"
			"\"Links\":[{\"From\":\"Machine:Pose\",\"To\":\"Output:Result\"}]}"),
			Crossfade, bAlias ? TEXT(",{\"Id\":\"FromWalk\",\"Action\":\"SM:StateAlias\",\"Properties\":{\"AliasedStates\":[\"Walk\"]}},"
				"{\"Id\":\"Back\",\"Action\":\"SM:Transition\",\"From\":\"FromWalk\",\"To\":\"Idle\",\"Graph\":{\"Nodes\":[{\"Id\":\"Moving\",\"Action\":\"K2:Get:bMoving\"}],\"Links\":[{\"From\":\"Moving:bMoving\",\"To\":\"Result:bCanEnterTransition\"}]}}") : TEXT(""));
	};
	const FString Created = Document(Target, TEXT("Create"), Body(0.4, true));
	if (!TestTrue(TEXT("state machine with conduit and alias is created"), Apply(*this, Target, Created, TEXT("SMCreate")).IsSuccess()))
	{
		return false;
	}
	UAnimBlueprint* Blueprint = Target.Get();
	TestTrue(TEXT("state machine Animation Blueprint compiles"), CompilesClean(Blueprint));
	const UAnimStateTransitionNode* ToGate = nullptr;
	TArray<UAnimStateTransitionNode*> Transitions;
	FBlueprintEditorUtils::GetAllNodesOfClass<UAnimStateTransitionNode>(Blueprint, Transitions);
	for (const UAnimStateTransitionNode* Transition : Transitions)
	{
		ToGate = Transition->GetNextState() != nullptr && Transition->GetNextState()->GetStateName() == TEXT("Gate") ? Transition : ToGate;
	}
	TestTrue(TEXT("transition details are set"), ToGate != nullptr && FMath::IsNearlyEqual(ToGate->CrossfadeDuration, 0.4f) && ToGate->PriorityOrder == 2);
	TArray<UAnimStateAliasNode*> Aliases;
	FBlueprintEditorUtils::GetAllNodesOfClass<UAnimStateAliasNode>(Blueprint, Aliases);
	TestTrue(TEXT("alias covers Walk"), Aliases.Num() == 1 && Aliases[0]->GetAliasedStates().Num() == 1);
	TestNoDiff(*this, Created, TEXT("state machine"));

	const TSharedPtr<FJsonObject> Extracted = Extract(*this, Target, TEXT("state machine extract"));
	if (Extracted.IsValid())
	{
		Extracted->SetStringField(TEXT("Action"), TEXT("Update"));
		const FAssetDocumentResult Reapplied = Apply(*this, Target, Serialize(Extracted), TEXT("SMReapply"));
		TestTrue(TEXT("state machine extraction re-applies as a no-op"), Reapplied.IsSuccess() && !Reapplied.bSavedAsset);
	}

	// Edit: transition details change and the alias (with its transition) is removed.
	const FString Edited = Document(Target, TEXT("Update"), Body(0.1, false));
	if (TestTrue(TEXT("state machine edit applies"), Apply(*this, Target, Edited, TEXT("SMEdit")).IsSuccess()))
	{
		TestNoDiff(*this, Edited, TEXT("state machine edit"));
		Aliases.Reset();
		FBlueprintEditorUtils::GetAllNodesOfClass<UAnimStateAliasNode>(Target.Get(), Aliases);
		TestEqual(TEXT("alias removed"), Aliases.Num(), 0);
		TestTrue(TEXT("edited state machine compiles"), CompilesClean(Target.Get()));
	}

	// The transition's settings left out are a newly placed transition's again (they used to be kept and
	// Diff said nothing).
	FString Omitted = Edited;
	const int32 Removed = Omitted.ReplaceInline(TEXT(",\"Properties\":{\"CrossfadeDuration\":0.1,\"PriorityOrder\":2}"), TEXT(""));
	TestEqual(TEXT("the transition settings are left out"), Removed, 1);
	FAssetDocumentDiffRequest DiffRequest;
	DiffRequest.Document = Parse(Omitted);
	const FAssetDocumentResult OmittedDiff = FAssetDocumentModule::Get().GetService().Diff(DiffRequest);
	const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
	TestTrue(TEXT("omitting non-default transition settings is a Diff"), OmittedDiff.IsSuccess() && OmittedDiff.Payload.IsValid()
		&& OmittedDiff.Payload->TryGetArrayField(TEXT("changed"), Changed) && !Changed->IsEmpty());
	const FAssetDocumentResult Reset = Apply(*this, Target, Omitted, TEXT("SMOmitted"));
	if (TestTrue(TEXT("omitting the transition settings applies"), Reset.IsSuccess()))
	{
		TestTrue(TEXT("omitting the transition settings is a change"), Reset.bSavedAsset);
		const UAnimStateTransitionNode* Default = GetDefault<UAnimStateTransitionNode>();
		Transitions.Reset();
		FBlueprintEditorUtils::GetAllNodesOfClass<UAnimStateTransitionNode>(Target.Get(), Transitions);
		const UAnimStateTransitionNode* const* Found = Transitions.FindByPredicate([](const UAnimStateTransitionNode* Transition)
		{
			return Transition->GetNextState() != nullptr && Transition->GetNextState()->GetStateName() == TEXT("Gate");
		});
		TestTrue(TEXT("the transition settings are a new transition's"), Found != nullptr
			&& FMath::IsNearlyEqual((*Found)->CrossfadeDuration, Default->CrossfadeDuration) && (*Found)->PriorityOrder == Default->PriorityOrder);
		TestNoDiff(*this, Omitted, TEXT("state machine omitted settings"));
	}
	return !HasAnyErrors();
}

// Two transitions between the same pair of states (the state machine editor allows it): identity
// is the NodeGuid an extraction carries, and From/To + ordinal for an authored document without
// one. Create, authored and extracted re-applies are no-ops, editing the second keeps both nodes,
// removing the first (by its extracted entry) keeps the second's node, and the package reloads.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimBlueprintDuplicateTransitionsTest,
	"UEAssetDoc.AnimBlueprint.StateMachine.DuplicateTransitions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimBlueprintDuplicateTransitionsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAnimBlueprintPythonTests;
	const FTarget Target(TEXT("ABP_DuplicateTransitions"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
	};
	auto Body = [](const TCHAR* FirstId, const TCHAR* SecondId, double SecondCrossfade)
	{
		return FString::Printf(TEXT(
			"\"Variables\":[{\"Name\":\"bMoving\",\"Type\":\"bool\"}],"
			"\"AnimGraph\":{\"Nodes\":[{\"Id\":\"Machine\",\"Action\":\"Anim:StateMachine\",\"Graph\":{\"Nodes\":["
			"{\"Id\":\"Idle\",\"Action\":\"SM:State\"},"
			"{\"Id\":\"Walk\",\"Action\":\"SM:State\"},"
			"{\"Id\":\"%s\",\"Action\":\"SM:Transition\",\"From\":\"Idle\",\"To\":\"Walk\",\"Graph\":{\"Nodes\":[{\"Id\":\"Moving\",\"Action\":\"K2:Get:bMoving\"}],\"Links\":[{\"From\":\"Moving:bMoving\",\"To\":\"Result:bCanEnterTransition\"}]},\"Properties\":{\"CrossfadeDuration\":0.1,\"PriorityOrder\":1}},"
			"{\"Id\":\"%s\",\"Action\":\"SM:Transition\",\"From\":\"Idle\",\"To\":\"Walk\",\"Properties\":{\"CrossfadeDuration\":%g,\"PriorityOrder\":2}}],"
			"\"Links\":[{\"From\":\"Entry:Entry\",\"To\":\"Idle:In\"}]}}],"
			"\"Links\":[{\"From\":\"Machine:Pose\",\"To\":\"Output:Result\"}]}"),
			FirstId, SecondId, SecondCrossfade);
	};
	// Live transitions by priority (1 = first, 2 = second).
	auto ByPriority = [&Target](int32 Priority) -> UAnimStateTransitionNode*
	{
		for (UAnimStateTransitionNode* Node : NodesOf<UAnimStateTransitionNode>(Target.Get()))
		{
			if (Node->PriorityOrder == Priority)
			{
				return Node;
			}
		}
		return nullptr;
	};
	const FString Created = Document(Target, TEXT("Create"), Body(TEXT("Fast"), TEXT("Slow"), 0.6));
	if (!TestTrue(TEXT("two Idle->Walk transitions are created"), Apply(*this, Target, Created, TEXT("DupCreate")).IsSuccess()))
	{
		return false;
	}
	const UAnimStateTransitionNode* First = ByPriority(1);
	const UAnimStateTransitionNode* Second = ByPriority(2);
	TestEqual(TEXT("two transition nodes"), NodesOf<UAnimStateTransitionNode>(Target.Get()).Num(), 2);
	if (!TestTrue(TEXT("both transitions connect Idle to Walk"), First != nullptr && Second != nullptr
		&& First->GetPreviousState() == Second->GetPreviousState() && First->GetNextState() == Second->GetNextState()
		&& First->GetPreviousState() != nullptr && First->GetPreviousState()->GetStateName() == TEXT("Idle")
		&& First->GetNextState() != nullptr && First->GetNextState()->GetStateName() == TEXT("Walk")))
	{
		return false;
	}
	const FGuid FirstGuid = First->NodeGuid;
	const FGuid SecondGuid = Second->NodeGuid;
	TestTrue(TEXT("crossfades are set per transition"), FMath::IsNearlyEqual(First->CrossfadeDuration, 0.1f) && FMath::IsNearlyEqual(Second->CrossfadeDuration, 0.6f));
	TestTrue(TEXT("duplicate transitions compile"), CompilesClean(Target.Get()));
	TestNoDiff(*this, Created, TEXT("duplicate transitions"));

	// The authored document (no NodeGuid; other document ids) re-applies as a no-op.
	const FAssetDocumentResult Authored = Apply(*this, Target, Document(Target, TEXT("Update"), Body(TEXT("Quick"), TEXT("Lazy"), 0.6)), TEXT("DupAuthoredReapply"));
	TestTrue(TEXT("authored re-apply with renamed ids is a no-op"), Authored.IsSuccess() && !Authored.bSavedAsset);

	// The extraction carries both transitions and re-applies as a no-op.
	const TSharedPtr<FJsonObject> Extracted = Extract(*this, Target, TEXT("duplicate transitions extract"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	Extracted->SetStringField(TEXT("Action"), TEXT("Update"));
	const FAssetDocumentResult Reapplied = Apply(*this, Target, Serialize(Extracted), TEXT("DupReapply"));
	TestTrue(TEXT("duplicate transition extraction re-applies as a no-op"), Reapplied.IsSuccess() && !Reapplied.bSavedAsset);

	// Edit the second one: both nodes keep their identity, only the second changes.
	const FString Edited = Document(Target, TEXT("Update"), Body(TEXT("Fast"), TEXT("Slow"), 0.9));
	if (TestTrue(TEXT("second transition edit applies"), Apply(*this, Target, Edited, TEXT("DupEdit")).IsSuccess()))
	{
		First = ByPriority(1);
		Second = ByPriority(2);
		TestTrue(TEXT("edit keeps both transition nodes"), First != nullptr && Second != nullptr && First->NodeGuid == FirstGuid && Second->NodeGuid == SecondGuid);
		TestTrue(TEXT("only the second crossfade changes"), First != nullptr && Second != nullptr
			&& FMath::IsNearlyEqual(First->CrossfadeDuration, 0.1f) && FMath::IsNearlyEqual(Second->CrossfadeDuration, 0.9f));
		TestNoDiff(*this, Edited, TEXT("second transition edit"));
	}

	// Remove the first one through a fresh extraction (its NodeGuid names it): the second stays.
	const TSharedPtr<FJsonObject> Current = Extract(*this, Target, TEXT("before removal"));
	const TSharedPtr<FJsonObject>* BodyObject = nullptr;
	const TSharedPtr<FJsonObject>* AnimGraph = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* TopNodes = nullptr;
	if (!TestTrue(TEXT("extraction has an AnimGraph"), Current.IsValid() && Current->TryGetObjectField(TEXT("Body"), BodyObject)
		&& (*BodyObject)->TryGetObjectField(TEXT("AnimGraph"), AnimGraph) && (*AnimGraph)->TryGetArrayField(TEXT("Nodes"), TopNodes)))
	{
		return false;
	}
	TSharedPtr<FJsonObject> MachineGraph;
	for (const TSharedPtr<FJsonValue>& Value : *TopNodes)
	{
		const TSharedPtr<FJsonObject>* Graph = nullptr;
		if (Value->AsObject()->GetStringField(TEXT("Action")) == TEXT("Anim:StateMachine") && Value->AsObject()->TryGetObjectField(TEXT("Graph"), Graph))
		{
			MachineGraph = *Graph;
		}
	}
	TArray<TSharedPtr<FJsonValue>> MachineNodes = MachineGraph.IsValid() ? MachineGraph->GetArrayField(TEXT("Nodes")) : TArray<TSharedPtr<FJsonValue>>();
	const int32 Removed = MachineNodes.IndexOfByPredicate([&FirstGuid](const TSharedPtr<FJsonValue>& Value)
	{
		FString Text;
		FGuid Guid;
		return Value->AsObject()->GetStringField(TEXT("Action")) == TEXT("SM:Transition") && Value->AsObject()->TryGetStringField(TEXT("NodeGuid"), Text)
			&& FGuid::Parse(Text, Guid) && Guid == FirstGuid;
	});
	if (!TestTrue(TEXT("extraction names the first transition by its NodeGuid"), Removed != INDEX_NONE))
	{
		AddInfo(Serialize(Current));
		return false;
	}
	MachineNodes.RemoveAt(Removed);
	MachineGraph->SetArrayField(TEXT("Nodes"), MachineNodes);
	Current->SetStringField(TEXT("Action"), TEXT("Update"));
	const FString Reduced = Serialize(Current);
	if (TestTrue(TEXT("first transition removal applies"), Apply(*this, Target, Reduced, TEXT("DupRemove")).IsSuccess()))
	{
		const TArray<UAnimStateTransitionNode*> Remaining = NodesOf<UAnimStateTransitionNode>(Target.Get());
		TestTrue(TEXT("the second transition node remains"), Remaining.Num() == 1 && Remaining[0]->NodeGuid == SecondGuid
			&& FMath::IsNearlyEqual(Remaining[0]->CrossfadeDuration, 0.9f) && Remaining[0]->PriorityOrder == 2);
		TestTrue(TEXT("state machine compiles after removal"), CompilesClean(Target.Get()));
		TestNoDiff(*this, Reduced, TEXT("after first transition removal"));
	}

	// Reload from disk: the saved package extracts the same.
	const FString Baseline = Serialize(Extract(*this, Target, TEXT("before reload")));
	if (UAnimBlueprint* Blueprint = Target.Get())
	{
		ReleaseUndoHistory();
		UPackageTools::UnloadPackages({Blueprint->GetOutermost()});
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		TestNull(TEXT("Animation Blueprint is unloaded"), Target.Get());
		ExtractEquals(*this, Target, Baseline, TEXT("duplicate transitions after reload"));
	}
	return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimBlueprintCustomBlendGraphTest,
	"UEAssetDoc.AnimBlueprint.Graphs.CustomTransitionBlendGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimBlueprintCustomBlendGraphTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAnimBlueprintPythonTests;
	const FTarget Target(TEXT("ABP_CustomBlend"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
	};
	// Idle -> Walk with a custom blend graph (Transition Details > Blend Logic = Custom): A blends
	// the source and destination poses with a Two Way Blend, B passes the destination pose through.
	// Mode 2: Blend Logic back to Standard Blend (no BlendGraph).
	auto Body = [](int32 Mode)
	{
		const TCHAR* Go = Mode == 0
			? TEXT(R"json("Properties":{"CrossfadeDuration":0.25},"BlendGraph":{"Nodes":[{"Id":"Blend","Action":"Anim:TwoWayBlend","Properties":{"Bindings":{"Alpha":{"Path":"Speed"}}}}],
  "Links":[{"From":"SourcePose:Pose","To":"Blend:A"},{"From":"DestinationPose:Pose","To":"Blend:B"},{"From":"Blend:Pose","To":"Output:Result"}]})json")
			: Mode == 1
			? TEXT(R"json("Properties":{"CrossfadeDuration":0.5},"BlendGraph":{"Nodes":[],"Links":[{"From":"DestinationPose:Pose","To":"Output:Result"}]})json")
			: TEXT(R"json("Properties":{"CrossfadeDuration":0.5,"LogicType":"TLT_StandardBlend"})json");
		return FString::Printf(TEXT(R"json("Variables":[{"Name":"Speed","Type":"float"}],
"AnimGraph":{"Nodes":[{"Id":"Loco","Action":"Anim:StateMachine","Graph":{"Nodes":[
  {"Id":"Idle","Action":"SM:State","Graph":{"Nodes":[{"Id":"Play","Action":"Anim:SequencePlayer","Properties":{"Node.Sequence":"%s"}}],
    "Links":[{"From":"Play:Pose","To":"Output:Result"}]}},
  {"Id":"Walk","Action":"SM:State","Graph":{"Nodes":[{"Id":"Play","Action":"Anim:SequencePlayer","Properties":{"Node.Sequence":"%s"}}],
    "Links":[{"From":"Play:Pose","To":"Output:Result"}]}},
  {"Id":"Go","Action":"SM:Transition","From":"Idle","To":"Walk",%s}],
  "Links":[{"From":"Entry:Entry","To":"Idle:In"}]}}],
  "Links":[{"From":"Loco:Pose","To":"Output:Result"}]})json"), IdleAnim, WalkAnim, Go);
	};
	auto Transition = [](UAnimBlueprint& Blueprint) -> UAnimStateTransitionNode*
	{
		const TArray<UAnimStateTransitionNode*> Nodes = NodesOf<UAnimStateTransitionNode>(&Blueprint);
		return Nodes.Num() == 1 ? Nodes[0] : nullptr;
	};
	FFeatureStages Stages;
	Stages.BodyA = Body(0);
	Stages.BodyB = Body(1);
	Stages.HumanEdit = [&Transition](UAnimBlueprint& Blueprint)
	{
		if (UAnimStateTransitionNode* Node = Transition(Blueprint))
		{
			Node->Modify();
			Node->CrossfadeDuration = 1.0f;
		}
	};
	Stages.Check = [&Transition](FAutomationTestBase& Test, UAnimBlueprint& Blueprint, bool bA)
	{
		const UAnimStateTransitionNode* Node = Transition(Blueprint);
		const UEdGraph* Custom = Node != nullptr ? Node->GetCustomTransitionGraph() : nullptr;
		Test.TestTrue(TEXT("transition uses a custom blend graph"), Node != nullptr && Node->LogicType == ETransitionLogicType::TLT_Custom && Custom != nullptr);
		const bool bHasBlend = Custom != nullptr && Custom->Nodes.ContainsByPredicate([](const UEdGraphNode* Candidate) { return Candidate && Candidate->IsA<UAnimGraphNode_TwoWayBlend>(); });
		Test.TestEqual(TEXT("custom blend graph holds the documented blend"), bHasBlend, bA);
		Test.TestTrue(TEXT("crossfade duration"), Node != nullptr && FMath::IsNearlyEqual(Node->CrossfadeDuration, bA ? 0.25f : 0.5f));
	};
	if (!RunFeatureStages(*this, Target, Stages))
	{
		return false;
	}
	// Removal: Blend Logic back to Standard Blend removes the custom blend graph.
	const FString Removed = Document(Target, TEXT("Update"), Body(2));
	if (TestTrue(TEXT("custom blend graph removal applies"), Apply(*this, Target, Removed, TEXT("CustomBlendRemove")).IsSuccess()))
	{
		const UAnimStateTransitionNode* Node = Transition(*Target.Get());
		TestTrue(TEXT("the custom blend graph is gone"), Node != nullptr && Node->LogicType == ETransitionLogicType::TLT_StandardBlend && Node->GetCustomTransitionGraph() == nullptr);
		TestTrue(TEXT("Animation Blueprint compiles without the custom blend graph"), CompilesClean(Target.Get()));
		TestNoDiff(*this, Removed, TEXT("after custom blend graph removal"));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimBlueprintBlendSpaceGraphTest,
	"UEAssetDoc.AnimBlueprint.Graphs.BlendSpaceGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimBlueprintBlendSpaceGraphTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAnimBlueprintPythonTests;
	using namespace UE::AssetDocument::AnimBlueprint::BlendSpaceRawEdits;
	const FTarget Target(TEXT("ABP_BlendSpaceGraph"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
	};
	// A: a 2D Blend Space Graph (X 0..200) with Idle and Walk samples, each with a sample graph.
	// B: X 0..300, Idle removed, Walk moved, Run added; a 1D Blend Space Graph blended in.
	const FString SampleGraph = TEXT(R"json({"Nodes":[{"Id":"Play","Action":"Anim:SequencePlayer","Properties":{"Node.Sequence":"%s"}}],"Links":[{"From":"Play:Pose","To":"Output:Result"}]})json");
	auto Graph = [&SampleGraph](const TCHAR* Sequence) { return SampleGraph.Replace(TEXT("%s"), Sequence); };
	FFeatureStages Stages;
	Stages.BodyA = FString::Printf(TEXT(R"json("Variables":[{"Name":"Speed","Type":"float"}],
"AnimGraph":{"Nodes":[{"Id":"Loco","Action":"Anim:BlendSpaceGraph:BlendSpace",
  "Properties":{"BlendSpace.BlendParameters.X.DisplayName":"Speed","BlendSpace.BlendParameters.X.Max":200,
                "BlendSpace.InterpolationParam.X.InterpolationTime":0.25},
  "Samples":[{"Name":"Idle","X":0,"Y":0,"Graph":%s},{"Name":"Walk","X":100,"Y":0,"Graph":%s}]}],
  "Links":[{"From":"Loco:Pose","To":"Output:Result"}]})json"), *Graph(IdleAnim), *Graph(WalkAnim));
	Stages.BodyB = FString::Printf(TEXT(R"json("Variables":[{"Name":"Speed","Type":"float"}],
"AnimGraph":{"Nodes":[{"Id":"Loco","Action":"Anim:BlendSpaceGraph:BlendSpace",
  "Properties":{"BlendSpace.BlendParameters.X.DisplayName":"Speed","BlendSpace.BlendParameters.X.Max":300,
                "BlendSpace.InterpolationParam.X.InterpolationTime":0.25},
  "Samples":[{"Name":"Walk","X":150,"Y":0,"Graph":%s},{"Name":"Run","X":300,"Y":0,"Graph":%s}]},
  {"Id":"Lean","Action":"Anim:BlendSpaceGraph:BlendSpace1D","Samples":[{"Name":"Left","X":0,"Graph":%s},{"Name":"Right","X":100,"Graph":%s}]},
  {"Id":"Blend","Action":"Anim:TwoWayBlend"}],
  "Links":[{"From":"Loco:Pose","To":"Blend:A"},{"From":"Lean:Pose","To":"Blend:B"},{"From":"Blend:Pose","To":"Output:Result"}]})json"),
		*Graph(WalkAnim), *Graph(WalkAnim), *Graph(IdleAnim), *Graph(WalkAnim));
	auto Loco = [](UAnimBlueprint& Blueprint) -> UAnimGraphNode_BlendSpaceGraph*
	{
		for (UAnimGraphNode_BlendSpaceGraph* Node : NodesOf<UAnimGraphNode_BlendSpaceGraph>(&Blueprint))
		{
			const UBlendSpace* BlendSpace = GetBlendSpace(*Node);
			if (BlendSpace != nullptr && BlendSpace->GetClass() == UBlendSpace::StaticClass())
			{
				return Node;
			}
		}
		return nullptr;
	};
	Stages.HumanEdit = [&Loco](UAnimBlueprint& Blueprint)
	{
		// Blend Space Graph tab: drag the first sample.
		UAnimGraphNode_BlendSpaceGraph* Node = Loco(Blueprint);
		UBlendSpace* BlendSpace = Node != nullptr ? GetBlendSpace(*Node) : nullptr;
		if (BlendSpace != nullptr && BlendSpace->GetNumberOfBlendSamples() > 0)
		{
			BlendSpace->Modify();
			BlendSpace->EditSampleValue(0, FVector(50.0, 50.0, 0.0));
			BlendSpace->ValidateSampleData();
			BlendSpace->ResampleData();
		}
	};
	Stages.Check = [&Loco](FAutomationTestBase& Test, UAnimBlueprint& Blueprint, bool bA)
	{
		const UAnimGraphNode_BlendSpaceGraph* Node = Loco(Blueprint);
		const UBlendSpace* BlendSpace = Node != nullptr ? GetBlendSpace(*Node) : nullptr;
		if (!Test.TestNotNull(TEXT("Blend Space Graph node with its blend space"), BlendSpace))
		{
			return;
		}
		TArray<FString> Samples;
		const TArray<UEdGraph*> Graphs = GetSampleGraphs(*Node);
		for (int32 Index = 0; Index < BlendSpace->GetNumberOfBlendSamples() && Index < Graphs.Num(); ++Index)
		{
			Samples.Add(FString::Printf(TEXT("%s@%g,%g"), *Graphs[Index]->GetName(), BlendSpace->GetBlendSample(Index).SampleValue.X, BlendSpace->GetBlendSample(Index).SampleValue.Y));
		}
		Samples.Sort();
		Test.TestEqual(TEXT("samples and sample graphs"), Samples, bA ? TArray<FString>({TEXT("Idle@0,0"), TEXT("Walk@100,0")})
			: TArray<FString>({TEXT("Run@300,0"), TEXT("Walk@150,0")}));
		Test.TestEqual(TEXT("one sample graph per sample"), Graphs.Num(), BlendSpace->GetNumberOfBlendSamples());
		Test.TestEqual(TEXT("X axis range"), BlendSpace->GetBlendParameter(0).Max, bA ? 200.f : 300.f);
		Test.TestEqual(TEXT("1D Blend Space Graph node"), NodesOf<UAnimGraphNode_BlendSpaceGraph>(&Blueprint).Num(), bA ? 1 : 2);
		for (const UEdGraph* SampleGraph : Graphs)
		{
			Test.TestTrue(TEXT("each sample graph plays its sequence"), SampleGraph != nullptr && SampleGraph->Nodes.Num() == 2);
		}
	};
	if (!RunFeatureStages(*this, Target, Stages))
	{
		return false;
	}
	// Removal: the Blend Space Graph node goes, with its sample graphs.
	const FString Removed = Document(Target, TEXT("Update"), FString::Printf(TEXT(R"json("AnimGraph":{"Nodes":[{"Id":"Play","Action":"Anim:SequencePlayer","Properties":{"Node.Sequence":"%s"}}],
  "Links":[{"From":"Play:Pose","To":"Output:Result"}]})json"), IdleAnim));
	if (TestTrue(TEXT("Blend Space Graph removal applies"), Apply(*this, Target, Removed, TEXT("BlendSpaceRemove")).IsSuccess()))
	{
		TestEqual(TEXT("no Blend Space Graph node remains"), NodesOf<UAnimGraphNode_BlendSpaceGraph>(Target.Get()).Num(), 0);
		TArray<UEdGraph*> Graphs;
		Target.Get()->GetAllGraphs(Graphs);
		TestFalse(TEXT("no sample graph remains"), Graphs.ContainsByPredicate([](const UEdGraph* Graph) { return Graph != nullptr && Graph->GetName() == TEXT("Idle"); }));
		TestTrue(TEXT("Animation Blueprint compiles without the Blend Space Graph"), CompilesClean(Target.Get()));
		TestNoDiff(*this, Removed, TEXT("after Blend Space Graph removal"));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimBlueprintFunctionBindingTest,
	"UEAssetDoc.AnimBlueprint.Graphs.FunctionBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimBlueprintFunctionBindingTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAnimBlueprintPythonTests;
	const FTarget Target(TEXT("ABP_FunctionBinding"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
	};
	// A: Alpha bound to a pure function of the Blueprint created in the same apply; B: to a native
	// pure function (UAnimInstance::GetDeltaSeconds).
	auto Body = [](const TCHAR* Path)
	{
		return FString::Printf(TEXT(R"json("Variables":[{"Name":"Speed","Type":"float"}],
"FunctionGraphs":[{"Name":"GetAlpha","Signature":{"Outputs":[{"Name":"ReturnValue","Type":"float"}],"Pure":true},
  "Nodes":[{"Id":"Get","Action":"K2:Get:Speed"}],
  "Links":[{"From":"Entry:then","To":"Result:execute"},{"From":"Get:Speed","To":"Result:ReturnValue"}]}],
"AnimGraph":{"Nodes":[
  {"Id":"Idle","Action":"Anim:SequencePlayer","Properties":{"Node.Sequence":"%s"}},
  {"Id":"Walk","Action":"Anim:SequencePlayer","Properties":{"Node.Sequence":"%s"}},
  {"Id":"Blend","Action":"Anim:TwoWayBlend","Properties":{"Bindings":{"Alpha":{"Path":"%s"}}}}],
  "Links":[{"From":"Idle:Pose","To":"Blend:A"},{"From":"Walk:Pose","To":"Blend:B"},{"From":"Blend:Pose","To":"Output:Result"}]})json"),
			IdleAnim, WalkAnim, Path);
	};
	FFeatureStages Stages;
	Stages.BodyA = Body(TEXT("GetAlpha"));
	Stages.BodyB = Body(TEXT("GetDeltaSeconds"));
	Stages.Check = [](FAutomationTestBase& Test, UAnimBlueprint& Blueprint, bool bA)
	{
		const TArray<UAnimGraphNode_TwoWayBlend*> Blends = NodesOf<UAnimGraphNode_TwoWayBlend>(&Blueprint);
		const UEdGraphPin* Alpha = Blends.Num() == 1 ? Blends[0]->FindPin(TEXT("Alpha")) : nullptr;
		Test.TestTrue(TEXT("bound Alpha pin is exposed and unlinked"), Alpha != nullptr && Alpha->LinkedTo.IsEmpty());
		const UAnimBlueprintGeneratedClass* Class = Cast<UAnimBlueprintGeneratedClass>(Blueprint.GeneratedClass);
		Test.TestTrue(TEXT("the bound function is compiled"), !bA || (Class != nullptr && Class->FindFunctionByName(TEXT("GetAlpha")) != nullptr));
	};
	if (!RunFeatureStages(*this, Target, Stages))
	{
		return false;
	}
	// Removal: {} removes the binding.
	const FString Removed = Document(Target, TEXT("Update"), FString::Printf(TEXT(R"json("AnimGraph":{"Nodes":[
  {"Id":"Idle","Action":"Anim:SequencePlayer","Properties":{"Node.Sequence":"%s"}},
  {"Id":"Walk","Action":"Anim:SequencePlayer","Properties":{"Node.Sequence":"%s"}},
  {"Id":"Blend","Action":"Anim:TwoWayBlend","Properties":{"Bindings":{}}}],
  "Links":[{"From":"Idle:Pose","To":"Blend:A"},{"From":"Walk:Pose","To":"Blend:B"},{"From":"Blend:Pose","To":"Output:Result"}]})json"), IdleAnim, WalkAnim));
	if (TestTrue(TEXT("binding removal applies"), Apply(*this, Target, Removed, TEXT("BindingRemove")).IsSuccess()))
	{
		TestTrue(TEXT("Animation Blueprint compiles without the binding"), CompilesClean(Target.Get()));
		TestNoDiff(*this, Removed, TEXT("after binding removal"));
	}
	// An impure or parameterised function cannot be bound (the binding picker does not offer it).
	const FString Impure = Document(Target, TEXT("Update"), FString::Printf(TEXT(R"json("AnimGraph":{"Nodes":[
  {"Id":"Idle","Action":"Anim:SequencePlayer","Properties":{"Node.Sequence":"%s"}},
  {"Id":"Walk","Action":"Anim:SequencePlayer","Properties":{"Node.Sequence":"%s"}},
  {"Id":"Blend","Action":"Anim:TwoWayBlend","Properties":{"Bindings":{"Alpha":{"Path":"GetOwningActor"}}}}],
  "Links":[{"From":"Idle:Pose","To":"Blend:A"},{"From":"Walk:Pose","To":"Blend:B"},{"From":"Blend:Pose","To":"Output:Result"}]})json"), IdleAnim, WalkAnim));
	TestFalse(TEXT("an incompatible function binding is rejected"), Apply(*this, Target, Impure, TEXT("BindingIncompatible")).IsSuccess());
	return !HasAnyErrors();
}

namespace AssetDocumentAnimBlueprintPythonTests
{
// A setting of the Sequence Player node's FAnimNode_SequencePlayer (read by reflection: the folded
// settings have no editor-node accessor).
template <typename TProperty, typename TValue>
TOptional<TValue> SequencePlayerSetting(const UAnimGraphNode_SequencePlayer& Node, const TCHAR* Name)
{
	const FStructProperty* NodeProperty = Node.GetFNodeProperty();
	const TProperty* Field = NodeProperty != nullptr ? CastField<TProperty>(NodeProperty->Struct->FindPropertyByName(Name)) : nullptr;
	if (Field == nullptr)
	{
		return {};
	}
	return TValue(Field->GetPropertyValue_InContainer(NodeProperty->ContainerPtrToValuePtr<void>(&Node)));
}

bool SequencePlayerShowsPin(const UAnimGraphNode_SequencePlayer& Node, const TCHAR* Name)
{
	const FOptionalPinFromProperty* Pin = Node.ShowPinForProperties.FindByPredicate(
		[Name](const FOptionalPinFromProperty& Entry) { return Entry.PropertyName == Name; });
	return Pin != nullptr && Pin->bShowPin;
}
} // namespace AssetDocumentAnimBlueprintPythonTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimBlueprintOmittedNodeSettingsTest,
	"UEAssetDoc.AnimBlueprint.OmittedNodeSettings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimBlueprintOmittedNodeSettingsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAnimBlueprintPythonTests;
	const FTarget Target(TEXT("ABP_OmittedSettings"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
	};
	auto Body = [](const FString& Settings)
	{
		return FString::Printf(TEXT(R"json("AnimGraph":{"Nodes":[
  {"Id":"Play","Action":"Anim:SequencePlayer","Properties":{"Node.Sequence":"%s"%s}}],
  "Links":[{"From":"Play:Pose","To":"Output:Result"}]})json"), IdleAnim, *Settings);
	};
	auto Player = [&Target]() -> const UAnimGraphNode_SequencePlayer*
	{
		const TArray<UAnimGraphNode_SequencePlayer*> Players = NodesOf<UAnimGraphNode_SequencePlayer>(Target.Get());
		return Players.Num() == 1 ? Players[0] : nullptr;
	};

	// 1. Non-default settings: play rate, no looping, the Start Position pin shown.
	const FString Set = Document(Target, TEXT("CreateOrUpdate"),
		Body(TEXT(",\"Node.PlayRate\":2.0,\"Node.bLoopAnimation\":false,\"ShowPin.StartPosition\":true")));
	if (!TestTrue(TEXT("settings apply"), Apply(*this, Target, Set, TEXT("Set")).IsSuccess()) || !TestNotNull(TEXT("one sequence player"), Player()))
	{
		return false;
	}
	TestEqual(TEXT("play rate set"), SequencePlayerSetting<FFloatProperty, float>(*Player(), TEXT("PlayRate")).Get(0.f), 2.f);
	TestEqual(TEXT("looping off"), SequencePlayerSetting<FBoolProperty, bool>(*Player(), TEXT("bLoopAnimation")).Get(true), false);
	TestTrue(TEXT("Start Position pin shown"), SequencePlayerShowsPin(*Player(), TEXT("StartPosition")));
	TestNoDiff(*this, Set, TEXT("after set"));

	// 2. The same node with the settings left out: they are the node's defaults again (not "unchanged").
	const FString Omitted = Document(Target, TEXT("Update"), Body(FString()));
	const FAssetDocumentResult Reset = Apply(*this, Target, Omitted, TEXT("Omitted"));
	if (!TestTrue(TEXT("omitting the settings applies"), Reset.IsSuccess()) || !TestNotNull(TEXT("the sequence player is kept"), Player()))
	{
		return false;
	}
	TestTrue(TEXT("omitting non-default settings is a change"), Reset.bSavedAsset);
	TestEqual(TEXT("play rate back to its default"), SequencePlayerSetting<FFloatProperty, float>(*Player(), TEXT("PlayRate")).Get(0.f), 1.f);
	TestEqual(TEXT("looping back to its default"), SequencePlayerSetting<FBoolProperty, bool>(*Player(), TEXT("bLoopAnimation")).Get(false), true);
	TestFalse(TEXT("Start Position pin hidden again"), SequencePlayerShowsPin(*Player(), TEXT("StartPosition")));
	TestTrue(TEXT("compiles"), CompilesClean(Target.Get()));
	TestNoDiff(*this, Omitted, TEXT("after omitting"));

	// 3. The earlier document now differs (its settings are not the defaults), and applying it again is a no-op afterwards.
	TestTrue(TEXT("settings apply again"), Apply(*this, Target, Set, TEXT("SetAgain")).IsSuccess());
	const FAssetDocumentResult Again = Apply(*this, Target, Set, TEXT("SetNoOp"));
	TestTrue(TEXT("re-applying the same settings is a no-op"), Again.IsSuccess() && !Again.bSavedAsset);
	return !HasAnyErrors();
}

#endif
