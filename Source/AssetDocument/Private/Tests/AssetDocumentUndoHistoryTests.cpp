// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
//
// Editor undo history x AssetDocument apply, the real-user sequence:
// a human edits an asset in the editor (the undo buffer records its objects),
// the AI applies an .assetdoc.json to that asset (or creates one) through the
// production Service, the apply succeeds or fails and rolls back, garbage is
// collected, then the human presses Ctrl+Z / Ctrl+Y.
//
// Each test collects garbage several times around every undo/redo step (the
// fixture GC crash fixed in 48e26d31 was intermittent), checks that the asset
// never references a garbage object, that the undo buffer does not keep
// objects the apply force-removed, and the undo history behaviour documented
// in Runtime/AssetDocumentUndoHistory.h (cleared after a changing apply or
// stale apply-recorded transactions, kept after rejected / no-op / rolled-back
// applies). Without it, Ctrl+Z after a successful Blueprint apply left pins
// linked to nodes the apply had removed.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "AssetDocumentTypes.h"

#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/FileManager.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetSystemLibrary.h"
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
#include "Runtime/AssetDocumentUndoHistory.h"
#include "ReferenceSkeleton.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"
#include "Utils/AssetDocumentEngineCompat.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentUndoHistoryTests
{
const TCHAR* Folder = TEXT("/Game/AssetDocumentUndoHistory");
const TCHAR* DataAssetClass = TEXT("/Script/AssetDocumentTests.TestDataAsset");

FAssetDocumentService& Service() { return FAssetDocumentModule::Get().GetService(); }

FString UniqueName(const TCHAR* Prefix)
{
	return FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

FString PackageFile(const FString& PackageName)
{
	return FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
}

FString SidecarFor(const FString& PackageName)
{
	return FPackageName::LongPackageNameToFilename(PackageName, TEXT(".assetdoc.json"));
}

FString Json(const TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	if (Object.IsValid())
	{
		FJsonSerializer::Serialize(Object.ToSharedRef(),
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
	}
	return Text;
}

TSharedPtr<FJsonObject> Parse(const FString& Text)
{
	TSharedPtr<FJsonObject> Object;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object);
	return Object;
}

FAssetDocumentResult ApplyText(FAutomationTestBase& Test, const FString& PackageName, const FString& Text, const TCHAR* Stage)
{
	const FString Sidecar = SidecarFor(PackageName);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Sidecar), true);
	Test.TestTrue(*FString::Printf(TEXT("%s: writes the sidecar"), Stage), FFileHelper::SaveStringToFile(Text, *Sidecar));
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Sidecar;
	Request.bSaveAsset = true;
	const FAssetDocumentResult Result = Service().ApplyFile(Request);
	Test.AddInfo(FString::Printf(TEXT("%s: %s"), Stage, *Result.Message));
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Test.AddInfo(FString::Printf(TEXT("%s: [%s] %s %s"), Stage, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
	}
	return Result;
}

TSharedPtr<FJsonObject> Extract(const FString& ObjectPath)
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = ObjectPath;
	Request.bDiffOnly = false;
	Request.bIncludeAllWritable = true;
	const FAssetDocumentResult Result = Service().Extract(Request);
	return Result.IsSuccess() ? Result.Payload : nullptr;
}

// Rejects the staging save of one package: the apply fails after every
// mutation step ran, so Core must roll the live asset back.
class FScopedStagingSaveVeto
{
	FCoreUObjectDelegates::FIsPackageOKToSaveDelegate Previous;
	FString Package;

public:
	bool bTriggered = false;

	explicit FScopedStagingSaveVeto(FAutomationTestBase& Test, FString InPackage)
		: Previous(FCoreUObjectDelegates::IsPackageOKToSaveDelegate), Package(MoveTemp(InPackage))
	{
		Test.AddExpectedMessage(TEXT("is not allowed to save"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
		FCoreUObjectDelegates::IsPackageOKToSaveDelegate.BindLambda([this](UPackage* Saved, const FString& Filename, FOutputDevice* Error)
		{
			if (Previous.IsBound() && !Previous.Execute(Saved, Filename, Error))
			{
				return false;
			}
			if (Saved && Saved->GetName() == Package
				&& FPaths::ConvertRelativePathToFull(Filename).Contains(TEXT("/Saved/AssetDocument/Runtime/Staging/")))
			{
				bTriggered = true;
				return false;
			}
			return true;
		});
	}

	~FScopedStagingSaveVeto() { FCoreUObjectDelegates::IsPackageOKToSaveDelegate = Previous; }
};

// ---------------------------------------------------------------------------
// Undo buffer inspection.

UTransactor* Transactor() { return GEditor ? ToRawPtr(GEditor->Trans) : nullptr; }

int32 QueueLength() { return Transactor() ? Transactor()->GetQueueLength() : 0; }

// Every object the undo buffer records or references (what keeps it reachable).
TArray<UObject*> UndoReferencedObjects()
{
	TArray<UObject*> Objects;
	if (UTransactor* Trans = Transactor())
	{
		FReferenceFinder Finder(Objects);
		Trans->CallAddReferencedObjects(Finder);
	}
	return Objects;
}

// Garbage the undo buffer keeps alive that came from the given package (moved
// to the transient package by a forced removal, or still inside it).
TArray<FString> UndoHeldGarbage(const FString& PackageName)
{
	TArray<FString> Held;
	for (const UObject* Object : UndoReferencedObjects())
	{
		if (Object && !IsValid(Object)
			&& (Object->GetPackage()->GetName() == PackageName || Object->GetPackage() == GetTransientPackage()))
		{
			Held.Add(Object->GetPathName());
		}
	}
	return Held;
}

bool UndoReferencesPackage(const FString& PackageName)
{
	for (const UObject* Object : UndoReferencedObjects())
	{
		if (Object && Object->GetPackage()->GetName() == PackageName)
		{
			return true;
		}
	}
	return false;
}

// Corruption probe: a live (non-garbage) object of the package that references
// a garbage object. Transient properties are skipped.
TArray<FString> GarbageReferencedByPackage(const FString& PackageName)
{
	TArray<FString> Broken;
	UPackage* Package = FindPackage(nullptr, *PackageName);
	if (Package == nullptr)
	{
		return Broken;
	}
	TArray<UObject*> Objects;
	GetObjectsWithPackage(Package, Objects, AssetDocumentObjects::IncludeNested, RF_NoFlags, EInternalObjectFlags::Garbage);
	for (UObject* Object : Objects)
	{
		TArray<UObject*> References;
		FReferenceFinder Finder(References, nullptr, false, true, false, true);
		Finder.FindReferences(Object);
		for (const UObject* Reference : References)
		{
			if (Reference && !IsValid(Reference))
			{
				Broken.Add(Object->GetPathName() + TEXT(" -> ") + Reference->GetPathName());
			}
		}
	}
	return Broken;
}

void CollectGarbageRounds(int32 Rounds)
{
	for (int32 Index = 0; Index < Rounds; ++Index)
	{
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
	}
}

struct FUndoProbe
{
	FAutomationTestBase& Test;
	FString PackageName;
	TFunction<void(const FString&)> Validate;

	void Check(const FString& Stage) const
	{
		const TArray<FString> Broken = GarbageReferencedByPackage(PackageName);
		for (const FString& Row : Broken)
		{
			Test.AddInfo(Stage + TEXT(": garbage reference ") + Row);
		}
		Test.TestEqual(*(Stage + TEXT(": asset references no garbage")), Broken.Num(), 0);
		if (Validate)
		{
			Validate(Stage);
		}
	}

	// GC, then Ctrl+Z every recorded step and Ctrl+Y them back, collecting
	// garbage around each step. Returns the number of undone steps.
	int32 GcUndoRedo(const FString& Stage) const
	{
		const TArray<FString> Held = UndoHeldGarbage(PackageName);
		for (const FString& Row : Held)
		{
			Test.AddInfo(Stage + TEXT(": undo keeps garbage ") + Row);
		}
		Test.TestEqual(*(Stage + TEXT(": undo history keeps no force-removed objects")), Held.Num(), 0);
		Test.AddInfo(FString::Printf(TEXT("%s: undo queue=%d references asset=%d"),
			*Stage, QueueLength(), UndoReferencesPackage(PackageName) ? 1 : 0));
		for (int32 Index = 0; Index < QueueLength(); ++Index)
		{
			const FTransaction* Transaction = Transactor()->GetTransaction(Index);
			Test.AddInfo(FString::Printf(TEXT("%s: undo[%d] %s"), *Stage, Index,
				Transaction ? *Transaction->GetContext().Title.ToString() : TEXT("<null>")));
		}
		CollectGarbageRounds(4);
		Check(Stage + TEXT(" after GC"));
		int32 Undone = 0;
		while (Undone < 16 && Transactor() && Transactor()->CanUndo() && GEditor->UndoTransaction())
		{
			++Undone;
			CollectGarbageRounds(2);
			Check(FString::Printf(TEXT("%s after undo %d"), *Stage, Undone));
		}
		int32 Redone = 0;
		while (Redone < Undone && Transactor() && Transactor()->CanRedo() && GEditor->RedoTransaction())
		{
			++Redone;
			CollectGarbageRounds(2);
			Check(FString::Printf(TEXT("%s after redo %d"), *Stage, Redone));
		}
		Test.AddInfo(FString::Printf(TEXT("%s: undone=%d redone=%d"), *Stage, Undone, Redone));
		Test.TestEqual(*(Stage + TEXT(": every undone step redoes")), Redone, Undone);
		return Undone;
	}
};

// The undo history behaviour after an apply (AssetDocumentUndoHistory.h):
// cleared, or the human's history kept as it was.
void ExpectHistory(FAutomationTestBase& Test, const TCHAR* Stage, int32 Before, bool bCleared)
{
	Test.AddInfo(FString::Printf(TEXT("%s: undo queue %d -> %d"), Stage, Before, QueueLength()));
	Test.TestEqual(*FString::Printf(TEXT("%s: %s"), Stage, bCleared ? TEXT("undo history is cleared") : TEXT("human undo history is kept")),
		QueueLength(), bCleared ? 0 : Before);
}

void ResetUndo()
{
	if (GEditor)
	{
		GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentUndoHistoryTests", "Reset", "AssetDocument undo history test"));
	}
}

// Saved assets are unloaded; unsaved ones deleted; files removed.
void RemoveAsset(const FString& PackageName)
{
	if (UPackage* Package = FindPackage(nullptr, *PackageName))
	{
		if (IFileManager::Get().FileExists(*PackageFile(PackageName)))
		{
			UPackageTools::UnloadPackages({Package});
		}
		else if (UObject* Asset = Package->FindAssetInPackage())
		{
			ObjectTools::DeleteObjects({Asset}, false, ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
		}
	}
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	if (IFileManager::Get().FileExists(*PackageFile(PackageName)))
	{
		IFileManager::Get().Delete(*PackageFile(PackageName), false, true);
		FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({PackageFile(PackageName)});
	}
	IFileManager::Get().Delete(*SidecarFor(PackageName), false, true);
}

bool SaveAsset(UObject& Asset)
{
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	const FString File = PackageFile(Asset.GetOutermost()->GetName());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
	return UPackage::SavePackage(Asset.GetOutermost(), &Asset, *File, Args);
}

// Retires an in-memory fixture asset the way the animation route tests do.
void Retire(UObject* Asset)
{
	if (!IsValid(Asset))
	{
		return;
	}
	UPackage* Package = Asset->GetOutermost();
	FAssetRegistryModule::AssetDeleted(Asset);
	Asset->ClearFlags(RF_Public | RF_Standalone);
	Asset->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
	Asset->MarkAsGarbage();
	Package->SetDirtyFlag(false);
	Package->MarkAsGarbage();
}

// ---------------------------------------------------------------------------
// Generic DataAsset.

FString DataAssetDocument(const FString& PackageName, const TCHAR* Action, const TCHAR* TestString)
{
	return FString::Printf(
		TEXT("{\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\",\"Target\":\"%s\",\"Class\":\"%s\",")
		TEXT("\"Action\":\"%s\",\"Properties\":{\"TestString\":\"%s\"}}"),
		*PackageName, DataAssetClass, Action, TestString);
}

FString GetTestString(const UObject* Asset)
{
	const FStrProperty* Property = Asset ? FindFProperty<FStrProperty>(Asset->GetClass(), TEXT("TestString")) : nullptr;
	return Property ? Property->GetPropertyValue_InContainer(Asset) : FString();
}

// The human edits the asset in the editor: one undoable transaction.
void UserEditTestString(UObject& Asset, const TCHAR* Value)
{
	const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentUndoHistoryTests", "UserEdit", "User edit"));
	Asset.Modify();
	FindFProperty<FStrProperty>(Asset.GetClass(), TEXT("TestString"))->SetPropertyValue_InContainer(&Asset, Value);
}

// The human saves (Ctrl+S). Saving keeps the undo history; AssetDocument
// refuses to apply to an existing asset with unsaved changes.
void UserSave(UObject& Asset)
{
	SaveAsset(Asset);
}

// ---------------------------------------------------------------------------
// Blueprint.

FString BlueprintDocument(const FString& PackageName, const TCHAR* Action, const FString& Body)
{
	return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"%s\",")
		TEXT("\"Properties\":{\"BlueprintDescription\":\"Undo history door\"},")
		TEXT("\"Body\":{\"ParentClass\":\"/Script/Engine.Actor\",%s}}"),
		*PackageName, Action, *Body);
}

FString BlueprintBody(double Health, double Factor, bool bCount, const TCHAR* ReturnFrom)
{
	return FString::Printf(TEXT(
		"\"Variables\":["
		"{\"Name\":\"Health\",\"Type\":\"float\",\"DefaultValue\":%g,\"Category\":\"Stats\",\"InstanceEditable\":true},"
		"{\"Name\":\"bArmed\",\"Type\":\"bool\",\"DefaultValue\":true}%s],"
		"\"Components\":["
		"{\"Name\":\"Pivot\",\"Class\":\"/Script/Engine.SceneComponent\"},"
		"{\"Name\":\"Lamp\",\"Class\":\"/Script/Engine.PointLightComponent\",\"AttachTo\":\"Pivot\",\"Properties\":{\"Intensity\":1234.5}}],"
		"\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":["
		"{\"Id\":\"OnHit\",\"Action\":\"K2:CustomEvent\",\"Name\":\"OnHit\"},"
		"{\"Id\":\"SayHit\",\"Action\":\"K2:PrintString\",\"PinOverrides\":[{\"Pin\":\"InString\",\"DefaultValue\":\"hit\"}]}],"
		"\"Links\":[{\"From\":\"OnHit:then\",\"To\":\"SayHit:execute\"}]}],"
		"\"FunctionGraphs\":[{\"Name\":\"ComputeDamage\","
		"\"Signature\":{\"Inputs\":[{\"Name\":\"Base\",\"Type\":\"float\"}],\"Outputs\":[{\"Name\":\"Damage\",\"Type\":\"float\"}]},"
		"\"Nodes\":["
		"{\"Id\":\"Armed\",\"Action\":\"K2:Get:bArmed\"},"
		"{\"Id\":\"Check\",\"Action\":\"K2:Branch\"},"
		"{\"Id\":\"Say\",\"Action\":\"K2:PrintString\",\"PinOverrides\":[{\"Pin\":\"InString\",\"DefaultValue\":\"armed\"}]},"
		"{\"Id\":\"Scale\",\"Action\":\"K2:Multiply\",\"PinOverrides\":[{\"Pin\":\"B\",\"DefaultValue\":\"%g\"}]}],"
		"\"Links\":["
		"{\"From\":\"Entry:then\",\"To\":\"Check:execute\"},"
		"{\"From\":\"Armed:bArmed\",\"To\":\"Check:Condition\"},"
		"{\"From\":\"Check:then\",\"To\":\"Say:execute\"},"
		"{\"From\":\"%s\",\"To\":\"Result:execute\"},"
		"{\"From\":\"Entry:Base\",\"To\":\"Scale:A\"},"
		"{\"From\":\"Scale:ReturnValue\",\"To\":\"Result:Damage\"}]}]"),
		Health, bCount ? TEXT(",{\"Name\":\"Count\",\"Type\":\"int\",\"DefaultValue\":3}") : TEXT(""), Factor, ReturnFrom);
}

// Graph integrity: every node is live and owned by its graph, every link
// points at a live pin of a live node.
void CheckBlueprintGraphs(FAutomationTestBase& Test, const UBlueprint* Blueprint, const FString& Stage)
{
	if (!Test.TestTrue(*(Stage + TEXT(": Blueprint is live")), IsValid(Blueprint)))
	{
		return;
	}
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	int32 BadNodes = 0;
	int32 BadLinks = 0;
	for (const UEdGraph* Graph : Graphs)
	{
		if (!IsValid(Graph))
		{
			++BadNodes;
			continue;
		}
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (!IsValid(Node) || Node->GetOuter() != Graph)
			{
				Test.AddInfo(FString::Printf(TEXT("%s: bad node %s in %s"), *Stage, Node ? *Node->GetPathName() : TEXT("null"), *Graph->GetName()));
				++BadNodes;
				continue;
			}
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				for (const UEdGraphPin* Linked : Pin ? Pin->LinkedTo : TArray<UEdGraphPin*>())
				{
					if (!Linked || Linked->bWasTrashed || !IsValid(Linked->GetOwningNodeUnchecked())
						|| !Linked->GetOwningNode()->GetGraph()->Nodes.Contains(Linked->GetOwningNode()))
					{
						++BadLinks;
					}
				}
			}
		}
	}
	Test.TestEqual(*(Stage + TEXT(": graphs hold only live owned nodes")), BadNodes, 0);
	Test.TestEqual(*(Stage + TEXT(": links point at live nodes")), BadLinks, 0);
}

UEdGraphNode* FindPrintString(UBlueprint& Blueprint)
{
	for (UEdGraph* Graph : Blueprint.UbergraphPages)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
			{
				if (Call->FunctionReference.GetMemberName() == TEXT("PrintString"))
				{
					return Node;
				}
			}
		}
	}
	return nullptr;
}

// The human edits the Blueprint in the Blueprint editor: moves a node, adds a
// node and adds a variable, each as its own undoable transaction, then compiles.
void UserEditBlueprint(FAutomationTestBase& Test, UBlueprint& Blueprint, const TCHAR* VariableName)
{
	if (UEdGraphNode* Node = FindPrintString(Blueprint))
	{
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentUndoHistoryTests", "MoveNode", "Move Node"));
		Node->Modify();
		Node->NodePosX += 64;
	}
	if (Blueprint.UbergraphPages.Num() > 0)
	{
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentUndoHistoryTests", "AddNode", "Add Node"));
		UEdGraph* Graph = Blueprint.UbergraphPages[0];
		// As FEdGraphSchemaAction_NewNode::PerformAction: the graph is modified before the node is added.
		Graph->Modify();
		FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
		UK2Node_CallFunction* Node = Creator.CreateNode(false);
		Node->FunctionReference.SetExternalMember(TEXT("PrintString"), UKismetSystemLibrary::StaticClass());
		Node->NodePosY = 400;
		Creator.Finalize();
	}
	{
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentUndoHistoryTests", "AddVariable", "Add Variable"));
		FEdGraphPinType Int;
		Int.PinCategory = UEdGraphSchema_K2::PC_Int;
		Test.TestTrue(TEXT("user adds a variable"), FBlueprintEditorUtils::AddMemberVariable(&Blueprint, VariableName, Int, TEXT("5")));
	}
	FKismetEditorUtilities::CompileBlueprint(&Blueprint);
	UserSave(Blueprint);
}

// ---------------------------------------------------------------------------
// Animation fixtures (saved Skeleton with a root bone, compatible preview mesh,
// pose sequence, existing sequence and a montage over it).

TSharedPtr<FJsonValue> Num(double Value) { return MakeShared<FJsonValueNumber>(Value); }
TSharedPtr<FJsonValue> Str(const FString& Value) { return MakeShared<FJsonValueString>(Value); }
TSharedPtr<FJsonValue> Bool(bool Value) { return MakeShared<FJsonValueBoolean>(Value); }
TSharedPtr<FJsonValue> Obj(const TSharedRef<FJsonObject>& Value) { return MakeShared<FJsonValueObject>(Value); }
TSharedPtr<FJsonValue> Arr(TArray<TSharedPtr<FJsonValue>> Values) { return MakeShared<FJsonValueArray>(MoveTemp(Values)); }
struct FField { FString Key; TSharedPtr<FJsonValue> Value; };
TSharedRef<FJsonObject> Object(std::initializer_list<FField> Fields)
{
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	for (const FField& Field : Fields)
	{
		Out->SetField(Field.Key, Field.Value);
	}
	return Out;
}

struct FAnimFixture
{
	FString Root = FString(Folder) / TEXT("Anim") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
	TStrongObjectPtr<USkeleton> Skeleton;
	TStrongObjectPtr<USkeletalMesh> Mesh;
	TStrongObjectPtr<UAnimSequence> Pose;
	TStrongObjectPtr<UAnimSequence> Existing;
	TStrongObjectPtr<UAnimMontage> Montage;

	template <typename T>
	T* Create(const TCHAR* Name)
	{
		T* Asset = NewObject<T>(CreatePackage(*(Root / Name)), Name, RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Asset);
		return Asset;
	}

	FString Package(const TCHAR* Name) const { return Root / Name; }
	FString Path(const TCHAR* Name) const { return Root / Name + TEXT(".") + Name; }

	bool MakeSequence(FAutomationTestBase& Test, TStrongObjectPtr<UAnimSequence>& Out, const TCHAR* Name)
	{
		Out.Reset(Create<UAnimSequence>(Name));
		Out->SetSkeleton(Skeleton.Get());
		IAnimationDataController& Controller = Out->GetController();
		Controller.InitializeModel();
		Controller.SetFrameRate(FFrameRate(30, 1), false);
		Controller.SetNumberOfFrames(FFrameNumber(30), false);
		if (!Controller.AddBoneCurve(TEXT("root"), false))
		{
			return false;
		}
		TArray<FVector3f> Positions;
		TArray<FVector3f> Scales;
		TArray<FQuat4f> Rotations;
		Positions.Init(FVector3f::ZeroVector, 31);
		Scales.Init(FVector3f::OneVector, 31);
		Rotations.Init(FQuat4f::Identity, 31);
		if (!Controller.SetBoneTrackKeys(TEXT("root"), Positions, Rotations, Scales, false))
		{
			return false;
		}
		Controller.NotifyPopulated();
		Out->WaitOnExistingCompression();
		return Test.TestTrue(FString::Printf(TEXT("save fixture %s"), Name), SaveAsset(*Out));
	}

	bool Setup(FAutomationTestBase& Test, bool bWithMontage)
	{
		Skeleton.Reset(Create<USkeleton>(TEXT("Skeleton")));
		{
			FReferenceSkeletonModifier Modifier(Skeleton.Get());
			Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
		}
		Skeleton->RegisterSlotNode(TEXT("DefaultSlot"));
		if (!Test.TestTrue(TEXT("save fixture skeleton"), SaveAsset(*Skeleton)))
		{
			return false;
		}
		Mesh.Reset(Create<USkeletalMesh>(TEXT("Mesh")));
		Mesh->SetSkeleton(Skeleton.Get());
		{
			FReferenceSkeletonModifier Bones(Mesh->GetRefSkeleton(), Skeleton.Get());
			Bones.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
		}
		if (!MakeSequence(Test, Pose, TEXT("Pose")) || !MakeSequence(Test, Existing, TEXT("Existing")))
		{
			return false;
		}
		if (!bWithMontage)
		{
			return true;
		}
		Montage.Reset(Create<UAnimMontage>(TEXT("Montage")));
		Montage->SetSkeleton(Skeleton.Get());
		Montage->RateScale = 1.f;
		IAnimationDataController& Controller = Montage->GetController();
		Controller.InitializeModel();
		Controller.SetFrameRate(FFrameRate(30, 1), false);
		Controller.SetNumberOfFrames(FFrameNumber(30), false);
		Controller.NotifyPopulated();
		Montage->SlotAnimTracks.Reset();
		FSlotAnimationTrack& Slot = Montage->SlotAnimTracks.AddDefaulted_GetRef();
		Slot.SlotName = TEXT("DefaultSlot");
		FAnimSegment Segment;
		Segment.SetAnimReference(Existing.Get());
		Segment.StartPos = 0.f;
		Segment.AnimStartTime = 0.f;
		Segment.AnimEndTime = 1.f;
		Segment.AnimPlayRate = 1.f;
		Segment.LoopingCount = 1;
		Slot.AnimTrack.AnimSegments.Add(Segment);
		Montage->CompositeSections.Reset();
		Montage->AddAnimCompositeSection(TEXT("Default"), 0.f);
		Montage->RefreshCacheData();
		return Test.TestTrue(TEXT("save fixture montage"), SaveAsset(*Montage));
	}

	~FAnimFixture()
	{
		IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(Root), false, true);
		TArray<UObject*> Assets{Montage.Get(), Existing.Get(), Pose.Get(), Mesh.Get(), Skeleton.Get()};
		Montage.Reset();
		Existing.Reset();
		Pose.Reset();
		Mesh.Reset();
		Skeleton.Reset();
		for (UObject* Asset : Assets)
		{
			Retire(Asset);
		}
		IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(Root), false, true);
	}
};

TSharedRef<FJsonObject> SequenceDocument(const FString& PackageName, const TSharedRef<FJsonObject>& Body)
{
	return Object({{TEXT("SchemaVersion"), Num(1)}, {TEXT("Target"), Str(PackageName)}, {TEXT("Class"), Str(TEXT("/Script/Engine.AnimSequence"))},
		{TEXT("Action"), Str(TEXT("CreateOrUpdate"))}, {TEXT("Definitions"), Obj(MakeShared<FJsonObject>())},
		{TEXT("Properties"), Obj(MakeShared<FJsonObject>())}, {TEXT("Body"), Obj(Body)}});
}

// Settings, frame count, a curve, notify tracks, a class notify, a named
// notify, a notify state and sync markers: new instanced subobjects included.
TSharedRef<FJsonObject> SequenceBody(const FAnimFixture& Fixture, double RateScale)
{
	auto Key = [](double Time, double Value)
	{
		return Obj(Object({{TEXT("Time"), Num(Time)}, {TEXT("Value"), Num(Value)}, {TEXT("InterpMode"), Str(TEXT("Linear"))}}));
	};
	return Object({
		{TEXT("References"), Obj(Object({{TEXT("Skeleton"), Str(Fixture.Path(TEXT("Skeleton")))}}))},
		{TEXT("Playback"), Obj(Object({{TEXT("RateScale"), Num(RateScale)}, {TEXT("bLoop"), Bool(true)},
			{TEXT("NumberOfFrames"), Num(60)}, {TEXT("FrameRate"), Num(30)}}))},
		{TEXT("Curves"), Arr({Obj(Object({{TEXT("Name"), Str(TEXT("Speed"))}, {TEXT("Keys"), Arr({Key(0, 1), Key(1, 2)})}}))})},
		{TEXT("NotifyTracks"), Arr({Obj(Object({{TEXT("Name"), Str(TEXT("Sound"))}})), Obj(Object({{TEXT("Name"), Str(TEXT("FX"))}}))})},
		{TEXT("Notifies"), Arr({
			Obj(Object({{TEXT("Time"), Num(0.25)}, {TEXT("Track"), Str(TEXT("Sound"))}, {TEXT("Class"), Str(TEXT("/Script/Engine.AnimNotify_PlaySound"))},
				{TEXT("Properties"), Obj(Object({{TEXT("VolumeMultiplier"), Num(0.5)}}))}})),
			Obj(Object({{TEXT("Time"), Num(0.5)}, {TEXT("Track"), Str(TEXT("Sound"))}, {TEXT("Name"), Str(TEXT("Footstep"))}}))})},
		{TEXT("NotifyStates"), Arr({Obj(Object({{TEXT("Time"), Num(1.0)}, {TEXT("Duration"), Num(0.5)}, {TEXT("Track"), Str(TEXT("FX"))},
			{TEXT("Class"), Str(TEXT("/Script/Engine.AnimNotifyState_TimedParticleEffect"))},
			{TEXT("Properties"), Obj(Object({{TEXT("bDestroyAtEnd"), Bool(true)}}))}}))})},
		{TEXT("SyncMarkers"), Arr({Obj(Object({{TEXT("Name"), Str(TEXT("LeftFoot"))}, {TEXT("Time"), Num(0.5)}, {TEXT("Track"), Str(TEXT("FX"))}}))})},
	});
}

void CheckNotifies(FAutomationTestBase& Test, const UAnimSequenceBase* Animation, const FString& Stage)
{
	if (!Test.TestTrue(*(Stage + TEXT(": animation is live")), IsValid(Animation)))
	{
		return;
	}
	int32 Bad = 0;
	for (const FAnimNotifyEvent& Event : Animation->Notifies)
	{
		const UObject* Notify = Event.Notify ? static_cast<const UObject*>(Event.Notify.Get()) : static_cast<const UObject*>(Event.NotifyStateClass.Get());
		if (Notify && (!IsValid(Notify) || Notify->GetOuter() != Animation))
		{
			Test.AddInfo(Stage + TEXT(": bad notify ") + Notify->GetPathName());
			++Bad;
		}
	}
	Test.TestEqual(*(Stage + TEXT(": notifies are live and owned")), Bad, 0);
}

// The human edits the animation in the editor: a property change and a data
// model change, each its own undoable transaction.
void UserEditAnimation(UAnimSequenceBase& Animation, float RateScale, int32 Frames)
{
	{
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentUndoHistoryTests", "AnimEdit", "Edit RateScale"));
		Animation.Modify();
		Animation.RateScale = RateScale;
	}
	// The animation editor's own data-model edits transact through the controller.
	Animation.GetController().SetNumberOfFrames(FFrameNumber(Frames), true);
	UserSave(Animation);
}
} // namespace AssetDocumentUndoHistoryTests


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUndoHistoryGenericDataAssetTest,
	"UEAssetDoc.UndoHistory.GenericDataAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentUndoHistoryGenericDataAssetTest::RunTest(const FString&)
{
	using namespace AssetDocumentUndoHistoryTests;
	const FString PackageName = FString(Folder) / UniqueName(TEXT("DA_Undo"));
	const FString AssetName = FPackageName::GetShortName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	ON_SCOPE_EXIT { ResetUndo(); RemoveAsset(PackageName); };
	if (!TestTrue(TEXT("AI creates the asset"), ApplyText(*this, PackageName, DataAssetDocument(PackageName, TEXT("Create"), TEXT("created")), TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	UObject* Asset = FSoftObjectPath(ObjectPath).ResolveObject();
	if (!TestNotNull(TEXT("created asset"), Asset))
	{
		return false;
	}
	ResetUndo();
	const FUndoProbe Probe{*this, PackageName, [this, ObjectPath](const FString& Stage)
	{
		TestTrue(*(Stage + TEXT(": asset is live")), IsValid(FSoftObjectPath(ObjectPath).ResolveObject()));
	}};

	// 1. A rolled-back apply keeps the human's history; Ctrl+Z / Ctrl+Y still work.
	UserEditTestString(*Asset, TEXT("user"));
	UserSave(*Asset);
	int32 Before = QueueLength();
	TestEqual(TEXT("user edit is undoable"), Before, 1);
	{
		FScopedStagingSaveVeto Veto(*this, PackageName);
		TestFalse(TEXT("vetoed AI update fails"), ApplyText(*this, PackageName, DataAssetDocument(PackageName, TEXT("Update"), TEXT("ai1")), TEXT("Rollback")).IsSuccess());
		TestTrue(TEXT("staging save was vetoed"), Veto.bTriggered);
	}
	TestEqual(TEXT("rollback restores the human value"), GetTestString(Asset), FString(TEXT("user")));
	ExpectHistory(*this, TEXT("after rollback"), Before, false);
	TestEqual(TEXT("after rollback: every human step undoes"), Probe.GcUndoRedo(TEXT("after rollback")), Before);
	TestEqual(TEXT("after rollback undo/redo: human value"), GetTestString(Asset), FString(TEXT("user")));
	UserSave(*Asset);

	// 2. A rejected apply and an unchanged re-apply keep the history.
	TestFalse(TEXT("an unknown property is rejected"), ApplyText(*this, PackageName, FString::Printf(
		TEXT("{\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\",\"Target\":\"%s\",\"Class\":\"%s\",")
		TEXT("\"Action\":\"Update\",\"Properties\":{\"NoSuchProperty\":1}}"), *PackageName, DataAssetClass), TEXT("Rejected")).IsSuccess());
	ExpectHistory(*this, TEXT("after rejected apply"), Before, false);
	const FAssetDocumentResult Unchanged = ApplyText(*this, PackageName, DataAssetDocument(PackageName, TEXT("Update"), TEXT("user")), TEXT("NoOp"));
	TestTrue(TEXT("unchanged re-apply succeeds without saving"), Unchanged.IsSuccess() && !Unchanged.bSavedAsset);
	ExpectHistory(*this, TEXT("after no-op apply"), Before, false);

	// 3. A successful apply clears history that references the asset: Ctrl+Z
	// cannot step back across the AI change into a mixed state.
	TestTrue(TEXT("AI update succeeds"), ApplyText(*this, PackageName, DataAssetDocument(PackageName, TEXT("Update"), TEXT("ai")), TEXT("Update")).IsSuccess());
	TestEqual(TEXT("AI value applied"), GetTestString(Asset), FString(TEXT("ai")));
	ExpectHistory(*this, TEXT("after success"), Before, true);
	Probe.GcUndoRedo(TEXT("after success"));
	TestEqual(TEXT("AI value survives Ctrl+Z"), GetTestString(Asset), FString(TEXT("ai")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUndoHistoryBlueprintTest,
	"UEAssetDoc.UndoHistory.Blueprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentUndoHistoryBlueprintTest::RunTest(const FString&)
{
	using namespace AssetDocumentUndoHistoryTests;
	const FString PackageName = FString(Folder) / UniqueName(TEXT("BP_Undo"));
	const FString AssetName = FPackageName::GetShortName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	ON_SCOPE_EXIT { ResetUndo(); RemoveAsset(PackageName); };
	if (!TestTrue(TEXT("AI creates the Blueprint"), ApplyText(*this, PackageName,
		BlueprintDocument(PackageName, TEXT("Create"), BlueprintBody(100.0, 2.0, true, TEXT("Say:then"))), TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	UBlueprint* Blueprint = Cast<UBlueprint>(FSoftObjectPath(ObjectPath).ResolveObject());
	if (!TestNotNull(TEXT("created Blueprint"), Blueprint))
	{
		return false;
	}
	{
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			int32 NonTransactionalNodes = 0;
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				NonTransactionalNodes += Node && !Node->HasAnyFlags(RF_Transactional) ? 1 : 0;
			}
			AddInfo(FString::Printf(TEXT("created graph %s transactional=%d nodes=%d non-transactional nodes=%d"),
				*Graph->GetName(), Graph->HasAnyFlags(RF_Transactional) ? 1 : 0, Graph->Nodes.Num(), NonTransactionalNodes));
		}
		AddInfo(FString::Printf(TEXT("created Blueprint transactional=%d"), Blueprint->HasAnyFlags(RF_Transactional) ? 1 : 0));
	}
	ResetUndo();
	const FUndoProbe Probe{*this, PackageName, [this, ObjectPath](const FString& Stage)
	{
		CheckBlueprintGraphs(*this, Cast<UBlueprint>(FSoftObjectPath(ObjectPath).ResolveObject()), Stage);
	}};
	auto CompilesAndExtracts = [this, ObjectPath](const FString& Stage)
	{
		UBlueprint* Live = Cast<UBlueprint>(FSoftObjectPath(ObjectPath).ResolveObject());
		if (!TestNotNull(*(Stage + TEXT(": Blueprint resolves")), Live))
		{
			return;
		}
		FKismetEditorUtilities::CompileBlueprint(Live);
		TestTrue(*(Stage + TEXT(": Blueprint compiles")), Live->Status == BS_UpToDate || Live->Status == BS_UpToDateWithWarnings);
		TestTrue(*(Stage + TEXT(": Blueprint extracts")), Extract(ObjectPath).IsValid());
		CheckBlueprintGraphs(*this, Live, Stage + TEXT(" compiled"));
	};

	// 1. Human edits, then a successful AI apply that rewrites graphs and variables.
	UserEditBlueprint(*this, *Blueprint, TEXT("UserVarA"));
	AddInfo(FString::Printf(TEXT("human edits recorded: %d"), QueueLength()));
	int32 QueueBefore = QueueLength();
	TestTrue(TEXT("AI edit succeeds"), ApplyText(*this, PackageName,
		BlueprintDocument(PackageName, TEXT("Update"), BlueprintBody(150.0, 3.0, false, TEXT("Check:else"))), TEXT("Edit")).IsSuccess());
	ExpectHistory(*this, TEXT("after success"), QueueBefore, true);
	Probe.GcUndoRedo(TEXT("after success"));
	CompilesAndExtracts(TEXT("after success undo/redo"));

	// 2. Human edits, then an AI apply that fails in the middle of Apply.
	ResetUndo();
	Blueprint = Cast<UBlueprint>(FSoftObjectPath(ObjectPath).ResolveObject());
	if (!TestNotNull(TEXT("Blueprint before mid-apply failure"), Blueprint))
	{
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	UserEditBlueprint(*this, *Blueprint, TEXT("UserVarB"));
	QueueBefore = QueueLength();
	TestFalse(TEXT("mid-apply failure fails"), ApplyText(*this, PackageName, BlueprintDocument(PackageName, TEXT("Update"), TEXT(
		"\"Variables\":[{\"Name\":\"Shield\",\"Type\":\"int\",\"DefaultValue\":9}],"
		"\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":[{\"Id\":\"Begin\",\"Action\":\"K2:BeginPlay\"},"
		"{\"Id\":\"Say\",\"Action\":\"K2:PrintString\"}],\"Links\":[{\"From\":\"Begin:then\",\"To\":\"Say:execute\"}]}],"
		"\"Components\":[{\"Name\":\"Broken\",\"Class\":\"/Script/Engine.Actor\"}]")), TEXT("MidApplyFailure")).IsSuccess());
	ExpectHistory(*this, TEXT("after mid-apply rollback"), QueueBefore, false);
	TestEqual(TEXT("after mid-apply rollback: every human step undoes"), Probe.GcUndoRedo(TEXT("after mid-apply rollback")), QueueBefore);
	CompilesAndExtracts(TEXT("after mid-apply rollback undo/redo"));

	// 3. Human edits, then an AI apply that fails at compile.
	ResetUndo();
	Blueprint = Cast<UBlueprint>(FSoftObjectPath(ObjectPath).ResolveObject());
	if (!TestNotNull(TEXT("Blueprint before compile failure"), Blueprint))
	{
		return false;
	}
	UserEditBlueprint(*this, *Blueprint, TEXT("UserVarC"));
	QueueBefore = QueueLength();
	TestFalse(TEXT("compile failure fails"), ApplyText(*this, PackageName, BlueprintDocument(PackageName, TEXT("Update"), TEXT(
		"\"Variables\":[{\"Name\":\"Health\",\"Type\":\"float\",\"DefaultValue\":1}],"
		"\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":["
		"{\"Id\":\"Begin\",\"Action\":\"K2:BeginPlay\"},"
		"{\"Id\":\"Loop\",\"Action\":\"K2:ForEachLoop\"}],"
		"\"Links\":[{\"From\":\"Begin:then\",\"To\":\"Loop:Exec\"}]}]")), TEXT("CompileFailure")).IsSuccess());
	// The failed compile left stock editor transactions (e.g. Break Pin Links) behind.
	ExpectHistory(*this, TEXT("after compile rollback"), QueueBefore, true);
	Probe.GcUndoRedo(TEXT("after compile rollback"));
	CompilesAndExtracts(TEXT("after compile rollback undo/redo"));

	// 4. Human edits, then an AI apply whose staging save fails after compile.
	ResetUndo();
	Blueprint = Cast<UBlueprint>(FSoftObjectPath(ObjectPath).ResolveObject());
	if (!TestNotNull(TEXT("Blueprint before staging failure"), Blueprint))
	{
		return false;
	}
	UserEditBlueprint(*this, *Blueprint, TEXT("UserVarD"));
	QueueBefore = QueueLength();
	{
		FScopedStagingSaveVeto Veto(*this, PackageName);
		TestFalse(TEXT("staging failure fails"), ApplyText(*this, PackageName,
			BlueprintDocument(PackageName, TEXT("Update"), BlueprintBody(175.0, 4.0, true, TEXT("Say:then"))), TEXT("StagingFailure")).IsSuccess());
		TestTrue(TEXT("Blueprint staging save was vetoed"), Veto.bTriggered);
	}
	ExpectHistory(*this, TEXT("after staging rollback"), QueueBefore, true);
	Probe.GcUndoRedo(TEXT("after staging rollback"));
	CompilesAndExtracts(TEXT("after staging rollback undo/redo"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUndoHistoryBlueprintControlTest,
	"UEAssetDoc.UndoHistory.BlueprintControl",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentUndoHistoryBlueprintControlTest::RunTest(const FString&)
{
	using namespace AssetDocumentUndoHistoryTests;
	// Control without any AI mutation after creation: AI-created and editor-created Blueprints.
	for (int32 Variant = 0; Variant < 2; ++Variant)
	{
		const FString PackageName = FString(Folder) / UniqueName(Variant == 0 ? TEXT("BP_ControlAI") : TEXT("BP_ControlEditor"));
		const FString AssetName = FPackageName::GetShortName(PackageName);
		const FString ObjectPath = PackageName + TEXT(".") + AssetName;
		ON_SCOPE_EXIT { ResetUndo(); RemoveAsset(PackageName); };
		UBlueprint* Blueprint = nullptr;
		if (Variant == 0)
		{
			ApplyText(*this, PackageName, BlueprintDocument(PackageName, TEXT("Create"), BlueprintBody(100.0, 2.0, true, TEXT("Say:then"))), TEXT("Create"));
			Blueprint = Cast<UBlueprint>(FSoftObjectPath(ObjectPath).ResolveObject());
		}
		else
		{
			Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), CreatePackage(*PackageName), FName(*AssetName),
				BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
			if (Blueprint)
			{
				FAssetRegistryModule::AssetCreated(Blueprint);
				UEdGraph* Graph = Blueprint->UbergraphPages[0];
				FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
				UK2Node_CallFunction* Node = Creator.CreateNode(false);
				Node->FunctionReference.SetExternalMember(TEXT("PrintString"), UKismetSystemLibrary::StaticClass());
				Creator.Finalize();
				FKismetEditorUtilities::CompileBlueprint(Blueprint);
				SaveAsset(*Blueprint);
			}
		}
		if (!TestNotNull(TEXT("control Blueprint"), Blueprint))
		{
			return false;
		}
		ResetUndo();
		const FString Label = Variant == 0 ? TEXT("AI-created control") : TEXT("editor-created control");
		const FUndoProbe Probe{*this, PackageName, [this, ObjectPath](const FString& Stage)
		{
			CheckBlueprintGraphs(*this, Cast<UBlueprint>(FSoftObjectPath(ObjectPath).ResolveObject()), Stage);
		}};
		UserEditBlueprint(*this, *Blueprint, TEXT("UserVar"));
		Probe.GcUndoRedo(Label);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUndoHistoryAnimSequenceTest,
	"UEAssetDoc.UndoHistory.AnimSequence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentUndoHistoryAnimSequenceTest::RunTest(const FString&)
{
	using namespace AssetDocumentUndoHistoryTests;
	FAnimFixture Fixture;
	ON_SCOPE_EXIT { ResetUndo(); };
	if (!Fixture.Setup(*this, false))
	{
		return false;
	}
	const FString PackageName = Fixture.Package(TEXT("Existing"));
	UAnimSequence* Sequence = Fixture.Existing.Get();
	ResetUndo();
	const FUndoProbe Probe{*this, PackageName, [this, Sequence](const FString& Stage) { CheckNotifies(*this, Sequence, Stage); }};

	UserEditAnimation(*Sequence, 0.75f, 45);
	int32 QueueBefore = QueueLength();
	TestTrue(TEXT("human edits are undoable"), QueueBefore >= 2);
	TestTrue(TEXT("AI edit succeeds"), ApplyText(*this, PackageName, Json(SequenceDocument(PackageName, SequenceBody(Fixture, 1.5))), TEXT("Edit")).IsSuccess());
	ExpectHistory(*this, TEXT("after success"), QueueBefore, true);
	Probe.GcUndoRedo(TEXT("after success"));
	TestTrue(TEXT("sequence extracts after success undo/redo"), Extract(Fixture.Path(TEXT("Existing"))).IsValid());

	// Mid-apply failure: plain fields written, then an unknown notify property.
	UserEditAnimation(*Sequence, 0.5f, 50);
	QueueBefore = QueueLength();
	TSharedRef<FJsonObject> Failing = SequenceBody(Fixture, 0.25);
	Failing->GetArrayField(TEXT("Notifies"))[0]->AsObject()->SetObjectField(TEXT("Properties"), Object({{TEXT("DoesNotExist"), Num(1)}}));
	TestFalse(TEXT("mid-apply failure fails"), ApplyText(*this, PackageName, Json(SequenceDocument(PackageName, Failing)), TEXT("MidApplyFailure")).IsSuccess());
	TestEqual(TEXT("rollback restores the human RateScale"), Sequence->RateScale, 0.5f);
	ExpectHistory(*this, TEXT("after mid-apply rollback"), QueueBefore, false);
	TestEqual(TEXT("after mid-apply rollback: every human step undoes"), Probe.GcUndoRedo(TEXT("after mid-apply rollback")), QueueBefore);

	// Staging failure after every step (new notify objects already created).
	UserEditAnimation(*Sequence, 0.6f, 40);
	QueueBefore = QueueLength();
	{
		FScopedStagingSaveVeto Veto(*this, PackageName);
		TestFalse(TEXT("staging failure fails"), ApplyText(*this, PackageName, Json(SequenceDocument(PackageName, SequenceBody(Fixture, 2.0))), TEXT("StagingFailure")).IsSuccess());
		TestTrue(TEXT("sequence staging save was vetoed"), Veto.bTriggered);
	}
	TestEqual(TEXT("staging rollback restores the human RateScale"), Sequence->RateScale, 0.6f);
	ExpectHistory(*this, TEXT("after staging rollback"), QueueBefore, false);
	TestEqual(TEXT("after staging rollback: every human step undoes"), Probe.GcUndoRedo(TEXT("after staging rollback")), QueueBefore);
	TestTrue(TEXT("sequence extracts after rollback undo/redo"), Extract(Fixture.Path(TEXT("Existing"))).IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUndoHistoryAnimMontageTest,
	"UEAssetDoc.UndoHistory.AnimMontage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentUndoHistoryAnimMontageTest::RunTest(const FString&)
{
	using namespace AssetDocumentUndoHistoryTests;
	FAnimFixture Fixture;
	ON_SCOPE_EXIT { ResetUndo(); };
	if (!Fixture.Setup(*this, true))
	{
		return false;
	}
	const FString PackageName = Fixture.Package(TEXT("Montage"));
	const FString ObjectPath = Fixture.Path(TEXT("Montage"));
	UAnimMontage* Montage = Fixture.Montage.Get();
	ResetUndo();
	const FUndoProbe Probe{*this, PackageName, [this, Montage](const FString& Stage) { CheckNotifies(*this, Montage, Stage); }};
	auto Sparse = [&](const TSharedRef<FJsonObject>& Body, double RateScale)
	{
		TSharedPtr<FJsonObject> Document = Extract(ObjectPath);
		if (!Document.IsValid())
		{
			return FString();
		}
		Document->SetStringField(TEXT("Action"), TEXT("Update"));
		Document->SetObjectField(TEXT("Body"), Body);
		Document->SetObjectField(TEXT("Properties"), Object({{TEXT("RateScale"), Num(RateScale)}}));
		return Json(Document);
	};
	auto Notifies = []()
	{
		return Arr({Obj(Object({{TEXT("Time"), Num(0.25)}, {TEXT("TrackIndex"), Num(0)},
			{TEXT("Object"), Obj(Object({{TEXT("Class"), Str(TEXT("/Script/Engine.AnimNotify_PlaySound"))},
				{TEXT("Properties"), Obj(Object({{TEXT("VolumeMultiplier"), Num(0.25)}}))}}))}}))});
	};

	UserEditAnimation(*Montage, 0.75f, 33);
	int32 QueueBefore = QueueLength();
	TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetField(TEXT("Notifies"), Notifies());
	TestTrue(TEXT("AI edit succeeds"), ApplyText(*this, PackageName, Sparse(Body, 1.5), TEXT("Edit")).IsSuccess());
	ExpectHistory(*this, TEXT("after success"), QueueBefore, true);
	Probe.GcUndoRedo(TEXT("after success"));

	// Mid-apply failure: new notify, then an unavailable blend profile.
	UserEditAnimation(*Montage, 0.5f, 36);
	QueueBefore = QueueLength();
	Body = MakeShared<FJsonObject>();
	Body->SetField(TEXT("Notifies"), Notifies());
	Body->SetObjectField(TEXT("Blend"), Object({{TEXT("BlendInTime"), Num(0.75)}, {TEXT("BlendProfileIn"), Str(TEXT("Missing"))}}));
	TestFalse(TEXT("mid-apply failure fails"), ApplyText(*this, PackageName, Sparse(Body, 2.0), TEXT("MidApplyFailure")).IsSuccess());
	TestEqual(TEXT("rollback restores the human RateScale"), Montage->RateScale, 0.5f);
	ExpectHistory(*this, TEXT("after mid-apply rollback"), QueueBefore, false);
	TestEqual(TEXT("after mid-apply rollback: every human step undoes"), Probe.GcUndoRedo(TEXT("after mid-apply rollback")), QueueBefore);

	// Staging failure after every step.
	UserEditAnimation(*Montage, 0.6f, 39);
	QueueBefore = QueueLength();
	Body = MakeShared<FJsonObject>();
	Body->SetField(TEXT("Notifies"), Notifies());
	const FString Staged = Sparse(Body, 2.5);
	{
		FScopedStagingSaveVeto Veto(*this, PackageName);
		TestFalse(TEXT("staging failure fails"), ApplyText(*this, PackageName, Staged, TEXT("StagingFailure")).IsSuccess());
		TestTrue(TEXT("montage staging save was vetoed"), Veto.bTriggered);
	}
	TestEqual(TEXT("staging rollback restores the human RateScale"), Montage->RateScale, 0.6f);
	ExpectHistory(*this, TEXT("after staging rollback"), QueueBefore, false);
	TestEqual(TEXT("after staging rollback: every human step undoes"), Probe.GcUndoRedo(TEXT("after staging rollback")), QueueBefore);
	TestTrue(TEXT("montage extracts after rollback undo/redo"), Extract(ObjectPath).IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUndoHistoryCreatedAssetRollbackTest,
	"UEAssetDoc.UndoHistory.CreatedAssetRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentUndoHistoryCreatedAssetRollbackTest::RunTest(const FString&)
{
	using namespace AssetDocumentUndoHistoryTests;
	// The human's undo history is about an unrelated asset; the AI creates new
	// assets whose creation fails and is rolled back.
	FAnimFixture Fixture;
	const FString Human = FString(Folder) / UniqueName(TEXT("DA_Human"));
	const FString CreatedData = FString(Folder) / UniqueName(TEXT("DA_Created"));
	const FString CreatedBlueprint = FString(Folder) / UniqueName(TEXT("BP_Created"));
	const FString CreatedSequence = Fixture.Package(TEXT("CreatedSequence"));
	ON_SCOPE_EXIT
	{
		ResetUndo();
		RemoveAsset(Human);
		RemoveAsset(CreatedData);
		RemoveAsset(CreatedBlueprint);
		RemoveAsset(CreatedSequence);
	};
	if (!Fixture.Setup(*this, false)
		|| !TestTrue(TEXT("human asset exists"), ApplyText(*this, Human, DataAssetDocument(Human, TEXT("Create"), TEXT("human")), TEXT("HumanCreate")).IsSuccess()))
	{
		return false;
	}
	UObject* HumanAsset = FSoftObjectPath(Human + TEXT(".") + FPackageName::GetShortName(Human)).ResolveObject();
	if (!TestNotNull(TEXT("human asset resolves"), HumanAsset))
	{
		return false;
	}
	auto Absent = [this](const FString& PackageName, const FString& Stage)
	{
		const UPackage* Package = FindPackage(nullptr, *PackageName);
		TestTrue(*(Stage + TEXT(": no live created package")), Package == nullptr || Package->FindAssetInPackage() == nullptr);
		TestFalse(*(Stage + TEXT(": no created package file")), IFileManager::Get().FileExists(*PackageFile(PackageName)));
	};
	auto Probe = [this](const FString& PackageName, const FString& Stage)
	{
		const FUndoProbe UndoProbe{*this, PackageName, {}};
		ExpectHistory(*this, *Stage, 1, false);
		TestEqual(*(Stage + TEXT(": the human edit undoes and redoes")), UndoProbe.GcUndoRedo(Stage), 1);
	};

	ResetUndo();
	UserEditTestString(*HumanAsset, TEXT("human edit"));
	{
		FScopedStagingSaveVeto Veto(*this, CreatedData);
		TestFalse(TEXT("DataAsset create fails at staging"), ApplyText(*this, CreatedData, DataAssetDocument(CreatedData, TEXT("Create"), TEXT("ai")), TEXT("DataCreate")).IsSuccess());
		TestTrue(TEXT("DataAsset staging save was vetoed"), Veto.bTriggered);
	}
	Absent(CreatedData, TEXT("DataAsset"));
	Probe(CreatedData, TEXT("after DataAsset create rollback"));

	ResetUndo();
	UserEditTestString(*HumanAsset, TEXT("human edit 2"));
	TestFalse(TEXT("Blueprint create fails mid-apply"), ApplyText(*this, CreatedBlueprint, BlueprintDocument(CreatedBlueprint, TEXT("Create"), TEXT(
		"\"Variables\":[{\"Name\":\"Health\",\"Type\":\"float\"}],"
		"\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":[{\"Id\":\"Begin\",\"Action\":\"K2:BeginPlay\"},"
		"{\"Id\":\"Say\",\"Action\":\"K2:PrintString\"}],\"Links\":[{\"From\":\"Begin:then\",\"To\":\"Say:execute\"}]}],"
		"\"Components\":[{\"Name\":\"Broken\",\"Class\":\"/Script/Engine.Actor\"}]")), TEXT("BlueprintCreate")).IsSuccess());
	Absent(CreatedBlueprint, TEXT("Blueprint"));
	Probe(CreatedBlueprint, TEXT("after Blueprint create rollback"));

	ResetUndo();
	UserEditTestString(*HumanAsset, TEXT("human edit 3"));
	{
		FScopedStagingSaveVeto Veto(*this, CreatedSequence);
		TestFalse(TEXT("AnimSequence create fails at staging"), ApplyText(*this, CreatedSequence,
			Json(SequenceDocument(CreatedSequence, SequenceBody(Fixture, 1.25))), TEXT("SequenceCreate")).IsSuccess());
		TestTrue(TEXT("AnimSequence staging save was vetoed"), Veto.bTriggered);
	}
	Absent(CreatedSequence, TEXT("AnimSequence"));
	Probe(CreatedSequence, TEXT("after AnimSequence create rollback"));

	// A successful create of another asset leaves history about other assets alone.
	ResetUndo();
	UserEditTestString(*HumanAsset, TEXT("human edit 4"));
	TestTrue(TEXT("DataAsset create succeeds"), ApplyText(*this, CreatedData, DataAssetDocument(CreatedData, TEXT("Create"), TEXT("ai")), TEXT("DataCreateSuccess")).IsSuccess());
	ExpectHistory(*this, TEXT("after unrelated create"), 1, false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUndoHistoryKeepsUnrelatedTest,
	"UEAssetDoc.UndoHistory.KeepsUnrelatedHistory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentUndoHistoryKeepsUnrelatedTest::RunTest(const FString&)
{
	using namespace AssetDocumentUndoHistoryTests;
	// An apply that recorded editor transactions used to clear the whole Undo history,
	// silently. Now only the apply's own entries and the entries that touch the applied package go
	// (plus an older entry that would restore an object across a removed change); Undo and Redo of
	// everything else keep working, in order, and the counts are returned.
	const FString Other = FString(Folder) / UniqueName(TEXT("DA_Other"));
	const FString Applied = FString(Folder) / UniqueName(TEXT("DA_Applied"));
	ON_SCOPE_EXIT { ResetUndo(); RemoveAsset(Other); RemoveAsset(Applied); };
	if (!ApplyText(*this, Other, DataAssetDocument(Other, TEXT("Create"), TEXT("other0")), TEXT("CreateOther")).IsSuccess()
		|| !ApplyText(*this, Applied, DataAssetDocument(Applied, TEXT("Create"), TEXT("applied0")), TEXT("CreateApplied")).IsSuccess())
	{
		return false;
	}
	UObject* OtherAsset = FSoftObjectPath(Other + TEXT(".") + FPackageName::GetShortName(Other)).ResolveObject();
	UObject* AppliedAsset = FSoftObjectPath(Applied + TEXT(".") + FPackageName::GetShortName(Applied)).ResolveObject();
	UTransBuffer* Buffer = Cast<UTransBuffer>(Transactor());
	if (!TestNotNull(TEXT("other asset"), OtherAsset) || !TestNotNull(TEXT("applied asset"), AppliedAsset)
		|| !TestNotNull(TEXT("the editor's transaction buffer"), Buffer))
	{
		return false;
	}
	const TWeakObjectPtr<UObject> OtherWeak(OtherAsset);
	const TWeakObjectPtr<UObject> AppliedWeak(AppliedAsset);
	auto Edit = [](const TArray<UObject*>& Assets, const TCHAR* Title, const TCHAR* Value)
	{
		const FScopedTransaction Transaction(FText::FromString(Title));
		for (UObject* Asset : Assets)
		{
			Asset->Modify();
			FindFProperty<FStrProperty>(Asset->GetClass(), TEXT("TestString"))->SetPropertyValue_InContainer(Asset, Value);
		}
	};
	auto Titles = []()
	{
		TArray<FString> Result;
		for (int32 Index = 0; Index < QueueLength(); ++Index)
		{
			Result.Add(Transactor()->GetTransaction(Index)->GetTitle().ToString());
		}
		return Result;
	};
	int32 Broadcasts = 0;
	const FDelegateHandle Handle = Buffer->OnUndoBufferChanged().AddLambda([&Broadcasts]() { ++Broadcasts; });
	ON_SCOPE_EXIT { if (UTransBuffer* Live = Cast<UTransBuffer>(Transactor())) { Live->OnUndoBufferChanged().Remove(Handle); } };
	ResetUndo();

	// 1. Unrelated, then the target, then the apply's own transaction (as Sequencer's edits record).
	Edit({OtherAsset}, TEXT("Edit other"), TEXT("other1"));
	Edit({AppliedAsset}, TEXT("Edit applied"), TEXT("applied1"));
	SaveAsset(*AppliedAsset);
	FAssetDocumentUndoHistoryMark Mark = UE::AssetDocument::UndoHistory::Capture();
	Edit({AppliedAsset}, TEXT("Apply step"), TEXT("ai"));
	Broadcasts = 0;
	FAssetDocumentUndoHistoryOutcome Outcome = UE::AssetDocument::UndoHistory::FinishApply(Mark, Applied, true);
	TestEqual(TEXT("1: the apply's own entry is removed"), Outcome.RemovedApplyEntries, 1);
	TestEqual(TEXT("1: the entry of the applied asset is removed"), Outcome.RemovedOtherEntries, 1);
	TestEqual(TEXT("1: named"), Outcome.RemovedTitles, TArray<FString>{TEXT("Edit applied")});
	TestEqual(TEXT("1: the unrelated entry is kept"), Titles(), TArray<FString>{TEXT("Edit other")});
	TestEqual(TEXT("1: kept count"), Outcome.KeptEntries, 1);
	TestFalse(TEXT("1: not cleared"), Outcome.bCleared);
	TestTrue(TEXT("1: the Undo History panel is told"), Broadcasts > 0);
	TestTrue(TEXT("1: the unrelated edit undoes"), GEditor->UndoTransaction());
	TestEqual(TEXT("1: other value restored"), GetTestString(OtherWeak.Get()), FString(TEXT("other0")));
	TestEqual(TEXT("1: undo never touches the applied asset"), GetTestString(AppliedWeak.Get()), FString(TEXT("ai")));
	TestFalse(TEXT("1: nothing older"), Transactor()->CanUndo());
	TestTrue(TEXT("1: and redoes"), GEditor->RedoTransaction());
	TestEqual(TEXT("1: other value again"), GetTestString(OtherWeak.Get()), FString(TEXT("other1")));

	// 2. A Redo entry of the applied asset goes, the Redo count follows; an apply that recorded nothing
	// and touches nothing in the history changes nothing.
	Edit({AppliedAsset}, TEXT("Edit applied 2"), TEXT("applied2"));
	TestTrue(TEXT("2: undone into Redo"), GEditor->UndoTransaction());
	SaveAsset(*AppliedAsset);
	Mark = UE::AssetDocument::UndoHistory::Capture();
	Outcome = UE::AssetDocument::UndoHistory::FinishApply(Mark, Applied, true);
	TestEqual(TEXT("2: the Redo entry is removed"), Outcome.RemovedOtherEntries, 1);
	TestEqual(TEXT("2: the queue"), Titles(), TArray<FString>{TEXT("Edit other")});
	TestEqual(TEXT("2: no Redo left"), Transactor()->GetUndoCount(), 0);
	TestTrue(TEXT("2: the unrelated edit still undoes"), Transactor()->CanUndo());
	Mark = UE::AssetDocument::UndoHistory::Capture();
	Outcome = UE::AssetDocument::UndoHistory::FinishApply(Mark, Applied, true);
	TestFalse(TEXT("2: an apply that touches nothing in the history changes nothing"), Outcome.ChangedUserHistory()
		|| Outcome.RemovedApplyEntries > 0);

	// 3. An entry that changed both assets goes; the older entry of the other asset would restore it
	// across that change and goes too; the newer one stays. The undo barrier follows the kept entries.
	ResetUndo();
	Edit({OtherAsset}, TEXT("Edit other A"), TEXT("otherA"));
	Edit({OtherAsset, AppliedAsset}, TEXT("Edit both"), TEXT("both"));
	Edit({OtherAsset}, TEXT("Edit other B"), TEXT("otherB"));
	Transactor()->SetUndoBarrier();
	Edit({OtherAsset}, TEXT("Edit other C"), TEXT("otherC"));
	SaveAsset(*AppliedAsset);
	Mark = UE::AssetDocument::UndoHistory::Capture();
	Outcome = UE::AssetDocument::UndoHistory::FinishApply(Mark, Applied, true);
	TestEqual(TEXT("3: removed"), Outcome.RemovedOtherEntries, 2);
	TestEqual(TEXT("3: kept"), Titles(), (TArray<FString>{TEXT("Edit other B"), TEXT("Edit other C")}));
	TestEqual(TEXT("3: the barrier stays above 'Edit other B'"), Transactor()->GetCurrentUndoBarrier(), 1);
	TestTrue(TEXT("3: the newest undoes"), GEditor->UndoTransaction());
	TestEqual(TEXT("3: to the state after the removed change"), GetTestString(OtherWeak.Get()), FString(TEXT("otherB")));
	TestFalse(TEXT("3: the barrier stops further undo"), Transactor()->CanUndo());
	Transactor()->RemoveUndoBarrier();
	TestTrue(TEXT("3: past the barrier"), GEditor->UndoTransaction());
	TestEqual(TEXT("3: 'Edit other B' undone"), GetTestString(OtherWeak.Get()), FString(TEXT("both")));
	TestEqual(TEXT("3: the applied asset untouched"), GetTestString(AppliedWeak.Get()), FString(TEXT("both")));
	TestFalse(TEXT("3: nothing older"), Transactor()->CanUndo());

	// 4. The production apply reports what it did: a human edit of the target, an unrelated one, then
	// an AI apply of the target (Undo.HistoryTrimmed, payload undo_history).
	ResetUndo();
	Edit({OtherAsset}, TEXT("Edit other D"), TEXT("otherD"));
	Edit({AppliedAsset}, TEXT("Edit applied D"), TEXT("appliedD"));
	SaveAsset(*AppliedAsset);
	const FAssetDocumentResult Result = ApplyText(*this, Applied, DataAssetDocument(Applied, TEXT("Update"), TEXT("aiD")), TEXT("ApplyD"));
	TestTrue(TEXT("4: apply succeeds"), Result.IsSuccess());
	const FAssetDocumentDiagnostic* Trimmed = Result.Diagnostics.FindByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == TEXT("Undo.HistoryTrimmed");
	});
	if (TestNotNull(TEXT("4: Undo.HistoryTrimmed"), Trimmed))
	{
		TestEqual(TEXT("4: info"), Trimmed->Severity, FString(TEXT("info")));
		TestTrue(TEXT("4: names the entry and the counts"), Trimmed->Message.Contains(TEXT("'Edit applied D'"))
			&& Trimmed->Message.Contains(TEXT("Removed 1 Undo entry")) && Trimmed->Message.Contains(TEXT("Kept 1 other entry")));
	}
	const TSharedPtr<FJsonObject>* History = nullptr;
	if (TestTrue(TEXT("4: payload undo_history"), Result.Payload.IsValid() && Result.Payload->TryGetObjectField(TEXT("undo_history"), History)))
	{
		TestEqual(TEXT("4: removed"), (*History)->GetIntegerField(TEXT("removed")), 1);
		TestEqual(TEXT("4: kept"), (*History)->GetIntegerField(TEXT("kept")), 1);
		TestFalse(TEXT("4: not cleared"), (*History)->GetBoolField(TEXT("cleared")));
	}
	TestEqual(TEXT("4: the unrelated entry stays"), Titles(), TArray<FString>{TEXT("Edit other D")});
	return true;
}

#endif
