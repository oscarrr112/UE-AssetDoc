// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentEditorSync.h"
#include "AssetDocumentService.h"
#include "AssetDocumentModule.h"
#include "AssetDocumentSidecar.h"
#include "Runtime/AssetDocumentSidecarByteIo.h"
#include "Runtime/AssetDocumentTypedSyncRuntime.h"
#include "AssetDocumentJsonRegionUtils.h"
#include "AssetDocumentProfile.h"
#include "AssetDocumentRegion.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CoreGlobals.h"
#include "Tests/AssetDocumentReflectedPropertyTestTypes.h"
#include "AIGraphTypes.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Decorators/BTDecorator_Blackboard.h"
#include "BehaviorTree/Services/BTService_DefaultFocus.h"
#include "BehaviorTree/Tasks/BTTask_MoveTo.h"
#include "BehaviorTree/Tasks/BTTask_BlueprintBase.h"
#include "BehaviorTree/Tasks/BTTask_RunBehavior.h"
#include "BehaviorTree/Tasks/BTTask_SetKeyValue.h"
#include "BehaviorTree/Tasks/BTTask_Wait.h"
#include "BehaviorTree/Tasks/BTTask_WaitBlackboardTime.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeDecoratorGraph.h"
#include "BehaviorTreeDecoratorGraphNode_Decorator.h"
#include "BehaviorTreeDecoratorGraphNode_Logic.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"
#include "BehaviorTreeGraphNode_Decorator.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Service.h"
#include "BehaviorTreeGraphNode_SimpleParallel.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "Animation/NodeMappingContainer.h"
#include "BlueprintEditorSettings.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "DirectoryWatcherModule.h"
#include "Engine/Blueprint.h"
#include "Engine/EngineTypes.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "HAL/FileManager.h"
#include "IDirectoryWatcher.h"
#include "PackageTools.h"
#include "Sections/MovieSceneCVarSection.h"
#include "TestActorBase.h"
#include "TestDataAsset.h"
#include "Misc/AutomationTest.h"
#include "Misc/Crc.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "UObject/SavePackage.h"
#include "Tests/AssetDocumentBehaviorTreeDurableDependencies.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"
#include "Utils/AssetDocumentJsonKeys.h"
#include "Utils/AssetDocumentEngineCompat.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
const FAssetDocumentRegionPolicy* BTTestFindPolicyByRegionId(const TArray<FAssetDocumentRegionPolicy>& Policies, FName RegionId)
{
	return Policies.FindByPredicate([RegionId](const FAssetDocumentRegionPolicy& Policy)
	{
		return Policy.RegionId == RegionId;
	});
}

TSharedRef<FJsonObject> BTTestMakeObject()
{
	return MakeShared<FJsonObject>();
}

TSharedPtr<FJsonObject> BTTestMakeAssetRef(const FString& Path)
{
	TSharedPtr<FJsonObject> Ref = MakeShared<FJsonObject>();
	Ref->SetStringField(TEXT("Kind"), TEXT("AssetRef"));
	Ref->SetStringField(TEXT("Path"), Path);
	return Ref;
}

TSharedPtr<FJsonValue> BTTestMakeObjectValue(TSharedPtr<FJsonObject> Object)
{
	return MakeShared<FJsonValueObject>(Object.ToSharedRef());
}

FString BTTestMakeObjectPathFromTarget(const FString& Target)
{
	return FString::Printf(TEXT("%s.%s"), *Target, *FPackageName::GetLongPackageAssetName(Target));
}

UBlackboardData* BTTestMakeExistingBlackboardAsset(const FString& Target)
{
	UPackage* Package = CreatePackage(*Target);
	return NewObject<UBlackboardData>(
		Package,
		*FPackageName::GetLongPackageAssetName(Target),
		RF_Public | RF_Standalone | RF_Transactional);
}

template <typename TKeyType>
void BTTestAddBlackboardKey(UBlackboardData* Blackboard, FName Name)
{
	if (!Blackboard)
	{
		return;
	}

	FBlackboardEntry Entry;
	Entry.EntryName = Name;
	Entry.KeyType = NewObject<TKeyType>(Blackboard);
	Blackboard->Keys.Add(Entry);
}

void BTTestAddObjectBlackboardKey(UBlackboardData* Blackboard, FName Name, UClass* BaseClass = AActor::StaticClass())
{
	if (!Blackboard)
	{
		return;
	}

	FBlackboardEntry Entry;
	Entry.EntryName = Name;
	UBlackboardKeyType_Object* KeyType = NewObject<UBlackboardKeyType_Object>(Blackboard);
	KeyType->BaseClass = BaseClass;
	Entry.KeyType = KeyType;
	Blackboard->Keys.Add(Entry);
}

UBlackboardData* BTTestMakeDecoratorBlackboardAsset(const FString& Target)
{
	UBlackboardData* Blackboard = BTTestMakeExistingBlackboardAsset(Target);
	BTTestAddObjectBlackboardKey(Blackboard, TEXT("TargetActor"));
	BTTestAddObjectBlackboardKey(Blackboard, TEXT("OtherTargetActor"));
	return Blackboard;
}

UBehaviorTree* BTTestMakeExistingBehaviorTreeAsset(const FString& Target)
{
	UPackage* Package = CreatePackage(*Target);
	return NewObject<UBehaviorTree>(
		Package,
		*FPackageName::GetLongPackageAssetName(Target),
		RF_Public | RF_Standalone | RF_Transactional);
}

UBehaviorTree* BTTestLoadBehaviorTreeForTarget(const FString& Target)
{
	return LoadObject<UBehaviorTree>(nullptr, *BTTestMakeObjectPathFromTarget(Target));
}

FString BTTestMakeUniqueTarget(const TCHAR* Prefix)
{
	return FString::Printf(
		TEXT("/Game/AssetDocumentTests/%s_%s"),
		Prefix,
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

class FScopedBehaviorTreeMutationFixture final
{
 FScopedBehaviorTreeDurableDependencies DurableDependencies;
public:
	bool ReserveTarget(const FString& Target)
	{
		FTargetSlot* Slot = FindOrReserveTarget(Target);
		if (!Slot || Slot->bPreexistingTarget)
		{
			return false;
		}
		Slot->bOwnsTarget = true;
		return true;
	}

	~FScopedBehaviorTreeMutationFixture()
	{
		for (int32 Index = Targets.Num() - 1; Index >= 0; --Index)
		{
			Cleanup(Targets[Index]);
		}
	}

	FAssetDocumentResult Apply(
		FAssetDocumentService& Service,
		const FString& Target,
		const TSharedPtr<FJsonObject>& Document)
	{
		FString DocumentTarget;
		if (!Document.IsValid()
			|| !Document->TryGetStringField(TEXT("Target"), DocumentTarget)
			|| DocumentTarget != Target)
		{
			return FAssetDocumentResult::Failure(TEXT("ApplyFile fixture requires Document.Target to match the explicit target"));
		}

		FString SidecarPath;
		const FAssetDocumentResult WriteResult = WriteCanonicalSidecar(Target, Document, SidecarPath);
		if (!WriteResult.IsSuccess())
		{
			return WriteResult;
		}

		FAssetDocumentApplyFileRequest Request;
		Request.FilePath = SidecarPath;
		Request.bSaveAsset = true;
		FAssetDocumentResult Result = Service.ApplyFile(Request);
		if (!Result.IsSuccess())
			for (const auto& Diagnostic : Result.Diagnostics)
				Result.Message += FString::Printf(TEXT(" [%s %s: %s]"),
					*Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message);
		return Result;
	}

	FAssetDocumentResult WriteCanonicalSidecar(
		const FString& Target,
		const TSharedPtr<FJsonObject>& Document,
		FString& OutSidecarPath)
	{
		FString DocumentTarget;
		if (!Document.IsValid()
			|| !Document->TryGetStringField(TEXT("Target"), DocumentTarget)
			|| DocumentTarget != Target)
		{
			return FAssetDocumentResult::Failure(TEXT("ApplyFile fixture requires Document.Target to match the explicit target"));
		}

		FTargetSlot* Slot = FindOrReserveTarget(Target);
		if (!Slot || Slot->bPreexistingTarget)
		{
			return FAssetDocumentResult::Failure(TEXT("ApplyFile fixture requires a GUID-unique, non-preexisting target"));
		}
		if (!DurableDependencies.Prepare(Document))
			return FAssetDocumentResult::Failure(TEXT("ApplyFile fixture could not save its BehaviorTree dependencies"));
		if (Slot->SidecarPath.IsEmpty()
			|| !IFileManager::Get().MakeDirectory(*FPaths::GetPath(Slot->SidecarPath), true))
		{
			return FAssetDocumentResult::Failure(TEXT("ApplyFile fixture failed to create canonical sidecar directory"));
		}

		FString WriteError;
		bool bWroteSidecar = false;
		{
			FAssetDocumentEditorSync::FScopedSidecarWrite Guard(Slot->SidecarPath);
			bWroteSidecar = FAssetDocumentSidecar::WriteJsonFile(Slot->SidecarPath, Document, WriteError);
		}
		Slot->bOwnsTarget = bWroteSidecar
			|| IFileManager::Get().FileExists(*Slot->SidecarPath)
			|| IFileManager::Get().FileExists(*Slot->PackageFilePath);
		if (!bWroteSidecar)
		{
			return FAssetDocumentResult::Failure(FString::Printf(
				TEXT("ApplyFile fixture failed to write canonical sidecar: %s"),
				*WriteError));
		}
		OutSidecarPath = Slot->SidecarPath;
		return FAssetDocumentResult::Success(TEXT("ApplyFile fixture wrote canonical sidecar"));
	}

private:
	struct FTargetSlot
	{
		FString Target;
		FString SidecarPath;
		FString PackageFilePath;
		FString ObjectPath;
		bool bPreexistingTarget = false;
		bool bOwnsTarget = false;
	};

	FTargetSlot* FindOrReserveTarget(const FString& Target)
	{
		if (FTargetSlot* Existing = Targets.FindByPredicate([&Target](const FTargetSlot& Candidate)
			{
				return Candidate.Target == Target;
			}))
		{
			return Existing;
		}
		const FString AssetName = FPackageName::GetLongPackageAssetName(Target);
		TArray<FString> NameParts;
		AssetName.ParseIntoArray(NameParts, TEXT("_"), true);
		const bool bHasGuidSegment = NameParts.ContainsByPredicate([](const FString& NamePart)
		{
			FGuid UniqueId;
			return FGuid::ParseExact(NamePart, EGuidFormats::Digits, UniqueId);
		});
		if (!bHasGuidSegment)
		{
			return nullptr;
		}

		FTargetSlot& Slot = Targets.AddDefaulted_GetRef();
		Slot.Target = Target;
		Slot.SidecarPath = FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(Target);
		Slot.PackageFilePath = FPackageName::LongPackageNameToFilename(Target, FPackageName::GetAssetPackageExtension());
		Slot.ObjectPath = BTTestMakeObjectPathFromTarget(Target);
		Slot.bPreexistingTarget = FindObject<UObject>(nullptr, *Slot.ObjectPath) != nullptr
			|| FindObject<UPackage>(nullptr, *Target) != nullptr
			|| IFileManager::Get().FileExists(*Slot.PackageFilePath)
			|| IFileManager::Get().FileExists(*Slot.SidecarPath);
		return &Slot;
	}

	TArray<FTargetSlot> Targets;

	void Cleanup(FTargetSlot& Slot) const
	{
		if (!Slot.bOwnsTarget)
		{
			return;
		}

		FAssetDocumentEditorSync::FScopedSidecarWrite Guard(Slot.SidecarPath);
		FScopedBehaviorTreeDurableDependencies::FlushFileEvents();
		UObject* ExistingAsset = FindObject<UObject>(nullptr, *Slot.ObjectPath);
		if (!ExistingAsset && IFileManager::Get().FileExists(*Slot.PackageFilePath))
		{
			ExistingAsset = LoadObject<UObject>(nullptr, *Slot.ObjectPath);
		}
		// Late macOS save events are classified as Removed once the owned file is
		// gone, before stock deletion creates the AssetRegistry empty-package mark.
		IFileManager::Get().Delete(*Slot.SidecarPath, false, true);
		IFileManager::Get().Delete(*Slot.PackageFilePath, false, true);
		FScopedBehaviorTreeDurableDependencies::FlushFileEvents();
		if (ExistingAsset)
		{
			TArray<UObject*> ObjectsToDelete{ExistingAsset};
			ObjectTools::DeleteObjectsUnchecked(ObjectsToDelete);
		}
		if (UPackage* ExistingPackage = FindPackage(nullptr, *Slot.Target))
		{
			ExistingPackage->SetDirtyFlag(false);
			TArray<UPackage*> PackagesToUnload{ExistingPackage};
			UPackageTools::UnloadPackages(PackagesToUnload);
		}
		FScopedBehaviorTreeDurableDependencies::FlushFileEvents();
	}
};

FAssetDocumentDiffRequest BTTestMakeDiffRequest(TSharedPtr<FJsonObject> Document)
{
	FAssetDocumentDiffRequest Request;
	Request.Document = Document;
	return Request;
}

bool BTTestResultHasDiagnostic(const FAssetDocumentResult& Result, const FString& ExpectedCode, const FString& ExpectedPath)
{
	return Result.Diagnostics.ContainsByPredicate([&ExpectedCode, &ExpectedPath](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == ExpectedCode && Diagnostic.Path == ExpectedPath;
	});
}

int32 BTTestCountBehaviorTreeNodeChildren(UBehaviorTree* BehaviorTree)
{
	int32 Count = 0;
	ForEachObjectWithOuter(BehaviorTree, [&Count](UObject* Object)
	{
		if (Object && Object->IsA<UBTNode>())
		{
			++Count;
		}
	});
	return Count;
}

TArray<UBTNode*> BTTestCollectBehaviorTreeNodeChildren(UBehaviorTree* BehaviorTree)
{
	TArray<UBTNode*> Nodes;
	ForEachObjectWithOuter(BehaviorTree, [&Nodes](UObject* Object)
	{
		if (UBTNode* Node = Cast<UBTNode>(Object))
		{
			Nodes.Add(Node);
		}
	});
	return Nodes;
}

TArray<UBehaviorTreeGraph*> BTTestCollectBehaviorTreeGraphChildren(UBehaviorTree* BehaviorTree)
{
	TArray<UBehaviorTreeGraph*> Graphs;
	ForEachObjectWithOuter(BehaviorTree, [&Graphs](UObject* Object)
	{
		if (UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(Object))
		{
			Graphs.Add(Graph);
		}
	}, AssetDocumentObjects::DirectOnly);
	return Graphs;
}

FString BTTestNormalizedId(const FString& Id)
{
	return Id.Replace(TEXT("-"), TEXT("")).Replace(TEXT("{"), TEXT("")).Replace(TEXT("}"), TEXT("")).ToUpper();
}

bool BTTestFindTreePointer(const TSharedPtr<FJsonValue>& Value, const FString& Id, const FString& Pointer, FString& OutPointer)
{
	if (!Value.IsValid())
	{
		return false;
	}
	if (Value->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		FString ItemId;
		if (Object->TryGetStringField(TEXT("Id"), ItemId) && BTTestNormalizedId(ItemId) == Id)
		{
			OutPointer = Pointer;
			return true;
		}
		for (const auto& Field : Object->Values)
		{
			if (BTTestFindTreePointer(Field.Value, Id, Pointer + TEXT("/") + Field.Key, OutPointer))
			{
				return true;
			}
		}
	}
	else if (Value->Type == EJson::Array)
	{
		const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
		for (int32 Index = 0; Index < Items.Num(); ++Index)
		{
			if (BTTestFindTreePointer(Items[Index], Id, FString::Printf(TEXT("%s/%d"), *Pointer, Index), OutPointer))
			{
				return true;
			}
		}
	}
	return false;
}

// Diff paths are JSON pointers into the document: the pointer of the node, decorator, service,
// comment or bound-graph node with this Id inside the document Tree (Body.Tree).
FString BTTestTreePointer(const TSharedPtr<FJsonObject>& Tree, const FString& Id)
{
	FString Pointer;
	return Tree.IsValid() && BTTestFindTreePointer(MakeShared<FJsonValueObject>(Tree), BTTestNormalizedId(Id), TEXT("/Body/Tree"), Pointer)
		? Pointer : FString::Printf(TEXT("<no tree item %s>"), *Id);
}

bool BTTestDiffPayloadHasEntry(const TSharedPtr<FJsonObject>& Payload, const FString& BucketName, const FString& ExpectedPath)
{
	if (!Payload.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
	if (!Payload->TryGetArrayField(BucketName, Entries) || !Entries)
	{
		return false;
	}

	return Entries->ContainsByPredicate([&ExpectedPath](const TSharedPtr<FJsonValue>& EntryValue)
	{
		const TSharedPtr<FJsonObject> Entry = EntryValue.IsValid() ? EntryValue->AsObject() : nullptr;
		FString Path;
		return Entry.IsValid() && Entry->TryGetStringField(TEXT("path"), Path) && Path == ExpectedPath;
	});
}

bool BTTestDiffPayloadHasAnyEntry(const TSharedPtr<FJsonObject>& Payload, const FString& ExpectedPath)
{
	return BTTestDiffPayloadHasEntry(Payload, TEXT("changed"), ExpectedPath)
		|| BTTestDiffPayloadHasEntry(Payload, TEXT("added"), ExpectedPath)
		|| BTTestDiffPayloadHasEntry(Payload, TEXT("removed"), ExpectedPath)
		|| BTTestDiffPayloadHasEntry(Payload, TEXT("unchanged"), ExpectedPath);
}

bool BTTestDiffPayloadHasNoChangedOrFailedEntries(const TSharedPtr<FJsonObject>& Payload)
{
	const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Failed = nullptr;
	return Payload.IsValid()
		&& Payload->TryGetArrayField(TEXT("changed"), Changed)
		&& Payload->TryGetArrayField(TEXT("failed"), Failed)
		&& Changed
		&& Failed
		&& Changed->Num() == 0
		&& Failed->Num() == 0;
}

bool BTTestDiffPayloadBucketHasPathPrefix(const TSharedPtr<FJsonObject>& Payload, const FString& BucketName, const FString& ExpectedPrefix)
{
	if (!Payload.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
	if (!Payload->TryGetArrayField(BucketName, Entries) || !Entries)
	{
		return false;
	}

	return Entries->ContainsByPredicate([&ExpectedPrefix](const TSharedPtr<FJsonValue>& EntryValue)
	{
		const TSharedPtr<FJsonObject> Entry = EntryValue.IsValid() ? EntryValue->AsObject() : nullptr;
		FString Path;
		return Entry.IsValid() && Entry->TryGetStringField(TEXT("path"), Path) && Path.StartsWith(ExpectedPrefix);
	});
}

bool BTTestDiffPayloadHasChangedPathPrefix(const TSharedPtr<FJsonObject>& Payload, const FString& ExpectedPrefix)
{
	return BTTestDiffPayloadBucketHasPathPrefix(Payload, TEXT("changed"), ExpectedPrefix)
		|| BTTestDiffPayloadBucketHasPathPrefix(Payload, TEXT("added"), ExpectedPrefix)
		|| BTTestDiffPayloadBucketHasPathPrefix(Payload, TEXT("removed"), ExpectedPrefix);
}

void BTTestSwapArrayEntries(TSharedPtr<FJsonObject> Object, const FString& FieldName, int32 FirstIndex, int32 SecondIndex)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Values) || !Values || !Values->IsValidIndex(FirstIndex) || !Values->IsValidIndex(SecondIndex))
	{
		return;
	}

	TArray<TSharedPtr<FJsonValue>> Swapped = *Values;
	Swapped.Swap(FirstIndex, SecondIndex);
	Object->SetArrayField(FieldName, Swapped);
}

TSharedPtr<FJsonObject> BTTestGetObjectField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName);
TSharedPtr<FJsonObject> BTTestGetObjectFromValue(const TSharedPtr<FJsonValue>& Value);

FString BTTestCanonicalGuid(const FString& StableName)
{
	const FGuid Guid(
		FCrc::StrCrc32(*(TEXT("A:") + StableName)),
		FCrc::StrCrc32(*(TEXT("B:") + StableName)),
		FCrc::StrCrc32(*(TEXT("C:") + StableName)),
		FCrc::StrCrc32(*(TEXT("D:") + StableName)));
	return Guid.ToString(EGuidFormats::Digits);
}

TSharedPtr<FJsonObject> BTTestMakeCanonicalEditor(
	const FString& LegacyId,
	const TMap<FString, TSharedPtr<FJsonObject>>& PositionsById,
	int32& NextGeneratedOrdinal,
	int32 Depth)
{
	TSharedPtr<FJsonObject> Position = MakeShared<FJsonObject>();
	if (const TSharedPtr<FJsonObject>* AuthoredPosition = PositionsById.Find(LegacyId))
	{
		Position->SetNumberField(TEXT("X"), (*AuthoredPosition)->GetNumberField(TEXT("X")));
		Position->SetNumberField(TEXT("Y"), (*AuthoredPosition)->GetNumberField(TEXT("Y")));
	}
	else
	{
		Position->SetNumberField(TEXT("X"), NextGeneratedOrdinal++ * 300);
		Position->SetNumberField(TEXT("Y"), Depth * 300);
	}

	TSharedPtr<FJsonObject> Editor = MakeShared<FJsonObject>();
	Editor->SetObjectField(TEXT("Position"), Position);
	return Editor;
}

TSharedPtr<FJsonObject> BTTestCanonicalizeLegacyAttachment(
	const TSharedPtr<FJsonObject>& LegacyAttachment,
	const TMap<FString, TSharedPtr<FJsonObject>>& PositionsById,
	int32& NextGeneratedOrdinal,
	int32 Depth)
{
	if (!LegacyAttachment.IsValid())
	{
		return nullptr;
	}

	const FString LegacyId = LegacyAttachment->GetStringField(TEXT("Id"));
	TSharedPtr<FJsonObject> Attachment = MakeShared<FJsonObject>();
	Attachment->SetStringField(TEXT("Id"), BTTestCanonicalGuid(TEXT("Node:") + LegacyId));
	Attachment->SetStringField(TEXT("Class"), LegacyAttachment->GetStringField(TEXT("Class")));
	TSharedPtr<FJsonObject> Properties = BTTestGetObjectField(LegacyAttachment, TEXT("Properties"));
	if (!Properties.IsValid())
	{
		Properties = MakeShared<FJsonObject>();
	}
	if (!Properties->HasField(TEXT("NodeName")))
	{
		Properties->SetStringField(TEXT("NodeName"), LegacyId);
	}
	Attachment->SetObjectField(TEXT("Properties"), Properties);
	Attachment->SetObjectField(TEXT("Editor"), BTTestMakeCanonicalEditor(LegacyId, PositionsById, NextGeneratedOrdinal, Depth));
	return Attachment;
}

TSharedPtr<FJsonObject> BTTestCanonicalizeLegacyNode(
	const TSharedPtr<FJsonObject>& LegacyNode,
	const TMap<FString, TSharedPtr<FJsonObject>>& PositionsById,
	int32& NextGeneratedOrdinal,
	int32 Depth)
{
	if (!LegacyNode.IsValid() || LegacyNode->Values.Num() == 0)
	{
		return MakeShared<FJsonObject>();
	}

	TSharedPtr<FJsonObject> Node = BTTestCanonicalizeLegacyAttachment(
		LegacyNode,
		PositionsById,
		NextGeneratedOrdinal,
		Depth);
	Node->SetArrayField(TEXT("Decorators"), {});
	Node->SetArrayField(TEXT("Services"), {});
	Node->SetArrayField(TEXT("Children"), {});

	const TArray<TSharedPtr<FJsonValue>>* LegacyDecorators = nullptr;
	if (LegacyNode->TryGetArrayField(TEXT("Decorators"), LegacyDecorators) && LegacyDecorators)
	{
		TArray<TSharedPtr<FJsonValue>> Decorators;
		for (const TSharedPtr<FJsonValue>& Value : *LegacyDecorators)
		{
			Decorators.Add(BTTestMakeObjectValue(BTTestCanonicalizeLegacyAttachment(
				BTTestGetObjectFromValue(Value),
				PositionsById,
				NextGeneratedOrdinal,
				Depth)));
		}
		Node->SetArrayField(TEXT("Decorators"), MoveTemp(Decorators));
	}

	const TArray<TSharedPtr<FJsonValue>>* LegacyServices = nullptr;
	if (LegacyNode->TryGetArrayField(TEXT("Services"), LegacyServices) && LegacyServices)
	{
		TArray<TSharedPtr<FJsonValue>> Services;
		for (const TSharedPtr<FJsonValue>& Value : *LegacyServices)
		{
			Services.Add(BTTestMakeObjectValue(BTTestCanonicalizeLegacyAttachment(
				BTTestGetObjectFromValue(Value),
				PositionsById,
				NextGeneratedOrdinal,
				Depth)));
		}
		Node->SetArrayField(TEXT("Services"), MoveTemp(Services));
	}

	const TArray<TSharedPtr<FJsonValue>>* LegacyChildren = nullptr;
	if (LegacyNode->TryGetArrayField(TEXT("Children"), LegacyChildren) && LegacyChildren)
	{
		TArray<TSharedPtr<FJsonValue>> Children;
		for (const TSharedPtr<FJsonValue>& Value : *LegacyChildren)
		{
			TSharedPtr<FJsonObject> LegacyEntry = BTTestGetObjectFromValue(Value);
			TSharedPtr<FJsonObject> LegacyChild = BTTestGetObjectField(LegacyEntry, TEXT("Child"));
			if (!LegacyChild.IsValid())
			{
				LegacyChild = LegacyEntry;
			}
			TSharedPtr<FJsonObject> Child = BTTestCanonicalizeLegacyNode(
				LegacyChild,
				PositionsById,
				NextGeneratedOrdinal,
				Depth + 1);

			const TArray<TSharedPtr<FJsonValue>>* EdgeDecorators = nullptr;
			if (LegacyEntry.IsValid()
				&& LegacyEntry != LegacyChild
				&& LegacyEntry->TryGetArrayField(TEXT("Decorators"), EdgeDecorators)
				&& EdgeDecorators)
			{
				TArray<TSharedPtr<FJsonValue>> Decorators;
				for (const TSharedPtr<FJsonValue>& DecoratorValue : *EdgeDecorators)
				{
					Decorators.Add(BTTestMakeObjectValue(BTTestCanonicalizeLegacyAttachment(
						BTTestGetObjectFromValue(DecoratorValue),
						PositionsById,
						NextGeneratedOrdinal,
						Depth + 1)));
				}
				Child->SetArrayField(TEXT("Decorators"), MoveTemp(Decorators));
			}
			Children.Add(BTTestMakeObjectValue(Child));
		}
		Node->SetArrayField(TEXT("Children"), MoveTemp(Children));
	}
	return Node;
}

TSharedPtr<FJsonObject> BTTestCanonicalizeLegacyTree(
	const TSharedPtr<FJsonObject>& LegacyTree,
	const TSharedPtr<FJsonObject>& LegacyEditorLayout)
{
	if (!LegacyTree.IsValid() || LegacyTree->HasField(TEXT("GraphGuid")))
	{
		return LegacyTree;
	}

	TMap<FString, TSharedPtr<FJsonObject>> PositionsById;
	const TArray<TSharedPtr<FJsonValue>>* LayoutNodes = nullptr;
	if (LegacyEditorLayout.IsValid()
		&& LegacyEditorLayout->TryGetArrayField(TEXT("Nodes"), LayoutNodes)
		&& LayoutNodes)
	{
		for (const TSharedPtr<FJsonValue>& Value : *LayoutNodes)
		{
			TSharedPtr<FJsonObject> LayoutNode = BTTestGetObjectFromValue(Value);
			const TSharedPtr<FJsonObject>* Position = nullptr;
			FString NodeId;
			if (LayoutNode.IsValid()
				&& LayoutNode->TryGetStringField(TEXT("NodeId"), NodeId)
				&& LayoutNode->TryGetObjectField(TEXT("Position"), Position)
				&& Position
				&& Position->IsValid())
			{
				PositionsById.Add(NodeId, *Position);
			}
		}
	}

	int32 NextGeneratedOrdinal = 1;
	TSharedPtr<FJsonObject> Tree = MakeShared<FJsonObject>();
	Tree->SetStringField(TEXT("GraphGuid"), BTTestCanonicalGuid(TEXT("LegacyBehaviorTreeGraph")));
	TSharedPtr<FJsonObject> Root = BTTestCanonicalizeLegacyNode(
		BTTestGetObjectField(LegacyTree, TEXT("Root")),
		PositionsById,
		NextGeneratedOrdinal,
		0);

	const TArray<TSharedPtr<FJsonValue>>* RootDecorators = nullptr;
	if (Root->Values.Num() > 0
		&& LegacyTree->TryGetArrayField(TEXT("RootDecorators"), RootDecorators)
		&& RootDecorators)
	{
		TArray<TSharedPtr<FJsonValue>> Decorators;
		for (const TSharedPtr<FJsonValue>& Value : *RootDecorators)
		{
			Decorators.Add(BTTestMakeObjectValue(BTTestCanonicalizeLegacyAttachment(
				BTTestGetObjectFromValue(Value),
				PositionsById,
				NextGeneratedOrdinal,
				0)));
		}
		Root->SetArrayField(TEXT("Decorators"), MoveTemp(Decorators));
	}
	Tree->SetObjectField(TEXT("Root"), Root);

	TArray<TSharedPtr<FJsonValue>> Comments;
	const TArray<TSharedPtr<FJsonValue>>* LegacyComments = nullptr;
	if (LegacyEditorLayout.IsValid()
		&& LegacyEditorLayout->TryGetArrayField(TEXT("Comments"), LegacyComments)
		&& LegacyComments)
	{
		for (const TSharedPtr<FJsonValue>& Value : *LegacyComments)
		{
			TSharedPtr<FJsonObject> LegacyComment = BTTestGetObjectFromValue(Value);
			if (!LegacyComment.IsValid())
			{
				continue;
			}
			const FString LegacyId = LegacyComment->GetStringField(TEXT("Id"));
			FGuid ParsedGuid;
			const FString CommentId = FGuid::Parse(LegacyId, ParsedGuid)
				? ParsedGuid.ToString(EGuidFormats::Digits)
				: BTTestCanonicalGuid(TEXT("Comment:") + LegacyId);
			TSharedPtr<FJsonObject> Comment = MakeShared<FJsonObject>();
			Comment->SetStringField(TEXT("Id"), CommentId);
			Comment->SetStringField(TEXT("Text"), LegacyComment->GetStringField(TEXT("Text")));
			if (TSharedPtr<FJsonObject> Position = BTTestGetObjectField(LegacyComment, TEXT("Position")))
			{
				Comment->SetObjectField(TEXT("Position"), Position);
			}
			if (TSharedPtr<FJsonObject> LegacySize = BTTestGetObjectField(LegacyComment, TEXT("Size")))
			{
				TSharedPtr<FJsonObject> Size = MakeShared<FJsonObject>();
				Size->SetNumberField(TEXT("Width"), LegacySize->GetNumberField(TEXT("X")));
				Size->SetNumberField(TEXT("Height"), LegacySize->GetNumberField(TEXT("Y")));
				Comment->SetObjectField(TEXT("Size"), Size);
			}
			if (TSharedPtr<FJsonObject> Color = BTTestGetObjectField(LegacyComment, TEXT("Color")))
			{
				Comment->SetObjectField(TEXT("Color"), Color);
			}
			Comments.Add(BTTestMakeObjectValue(Comment));
		}
	}
	Tree->SetArrayField(TEXT("Comments"), MoveTemp(Comments));
	return Tree;
}

TSharedPtr<FJsonObject> BTTestMakeBehaviorTreeBody(
	TSharedPtr<FJsonObject> Blackboard,
	TSharedPtr<FJsonObject> Tree = nullptr,
	TSharedPtr<FJsonObject> EditorLayout = nullptr)
{
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	if (Blackboard.IsValid())
	{
		Body->SetObjectField(TEXT("BlackboardAsset"), Blackboard);
	}
	else
	{
		Body->SetField(TEXT("BlackboardAsset"), MakeShared<FJsonValueNull>());
	}

	if (Tree.IsValid())
	{
		Body->SetObjectField(TEXT("Tree"), BTTestCanonicalizeLegacyTree(Tree, EditorLayout));
	}
	return Body;
}

TSharedPtr<FJsonObject> BTTestMakeEmptyBehaviorTree()
{
	TSharedPtr<FJsonObject> Tree = MakeShared<FJsonObject>();
	Tree->SetStringField(TEXT("GraphGuid"), BTTestCanonicalGuid(TEXT("LegacyBehaviorTreeGraph")));
	Tree->SetArrayField(TEXT("Comments"), TArray<TSharedPtr<FJsonValue>>());
	Tree->SetObjectField(TEXT("Root"), MakeShared<FJsonObject>());
	return Tree;
}

TSharedPtr<FJsonObject> BTTestMakeMinimalSelectorBehaviorTree()
{
	TSharedPtr<FJsonObject> Tree = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("Id"), TEXT("Root"));
	Root->SetStringField(TEXT("Class"), TEXT("/Script/AIModule.BTComposite_Selector"));
	Root->SetArrayField(TEXT("Services"), TArray<TSharedPtr<FJsonValue>>());
	Root->SetArrayField(TEXT("Children"), TArray<TSharedPtr<FJsonValue>>());
	Tree->SetObjectField(TEXT("Root"), Root);
	return Tree;
}

TSharedPtr<FJsonObject> BTTestMakeTreeWithKeySelectorLikeProperty()
{
	TSharedPtr<FJsonObject> Selector = MakeShared<FJsonObject>();
	Selector->SetStringField(TEXT("Key"), TEXT("TargetActor"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetObjectField(TEXT("BlackboardKey"), Selector);

	TSharedPtr<FJsonObject> Task = MakeShared<FJsonObject>();
	Task->SetStringField(TEXT("Id"), TEXT("WaitForTarget"));
	Task->SetStringField(TEXT("Class"), TEXT("/Script/AIModule.BTTask_WaitBlackboardTime"));
	Task->SetObjectField(TEXT("Properties"), Properties);
	TSharedPtr<FJsonObject> Edge = MakeShared<FJsonObject>();
	Edge->SetObjectField(TEXT("Child"), Task);

	TSharedPtr<FJsonObject> Tree = BTTestMakeMinimalSelectorBehaviorTree();
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("Id"), TEXT("Root"));
	Root->SetStringField(TEXT("Class"), TEXT("/Script/AIModule.BTComposite_Selector"));
	Root->SetArrayField(TEXT("Services"), TArray<TSharedPtr<FJsonValue>>());
	Root->SetArrayField(TEXT("Children"), {BTTestMakeObjectValue(Edge)});
	Tree->SetObjectField(TEXT("Root"), Root);
	return Tree;
}

TSharedPtr<FJsonObject> BTTestMakeSelectorProperty(const FString& KeyName)
{
	TSharedPtr<FJsonObject> Selector = MakeShared<FJsonObject>();
	Selector->SetStringField(TEXT("Key"), KeyName);
	return Selector;
}

TSharedPtr<FJsonObject> BTTestMakeBtNode(
	const FString& Id,
	const FString& Class,
	TSharedPtr<FJsonObject> Properties = nullptr)
{
	TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("Id"), Id);
	Node->SetStringField(TEXT("Class"), Class);
	if (Properties.IsValid())
	{
		Node->SetObjectField(TEXT("Properties"), Properties);
	}
	return Node;
}

TSharedPtr<FJsonObject> BTTestMakeMoveToTreeWithKeys(const TArray<FString>& KeyNames)
{
	TArray<TSharedPtr<FJsonValue>> Children;
	for (const FString& KeyName : KeyNames)
	{
		TSharedPtr<FJsonObject> MoveToProperties = MakeShared<FJsonObject>();
		MoveToProperties->SetObjectField(TEXT("BlackboardKey"), BTTestMakeSelectorProperty(KeyName));
		const FString NodeId = KeyName == TEXT("TargetActor")
			? FString(TEXT("MoveToTarget"))
			: FString::Printf(TEXT("MoveTo_%s"), *KeyName);
		TSharedPtr<FJsonObject> MoveToNode = BTTestMakeBtNode(NodeId, TEXT("/Script/AIModule.BTTask_MoveTo"), MoveToProperties);

		TSharedPtr<FJsonObject> Edge = MakeShared<FJsonObject>();
		Edge->SetObjectField(TEXT("Child"), MoveToNode);
		Children.Add(BTTestMakeObjectValue(Edge));
	}

	TSharedPtr<FJsonObject> Root = BTTestMakeBtNode(TEXT("RootSelector"), TEXT("/Script/AIModule.BTComposite_Selector"));
	Root->SetArrayField(TEXT("Children"), Children);

	TSharedPtr<FJsonObject> Tree = MakeShared<FJsonObject>();
	Tree->SetObjectField(TEXT("Root"), Root);
	return Tree;
}

TSharedPtr<FJsonObject> BTTestMakePositionObject(double X, double Y)
{
	TSharedPtr<FJsonObject> Position = MakeShared<FJsonObject>();
	Position->SetNumberField(TEXT("X"), X);
	Position->SetNumberField(TEXT("Y"), Y);
	return Position;
}

TSharedPtr<FJsonObject> BTTestMakeEditorLayoutNode(const FString& NodeId, double X, double Y)
{
	TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("NodeId"), NodeId);
	Node->SetObjectField(TEXT("Position"), BTTestMakePositionObject(X, Y));
	return Node;
}

TSharedPtr<FJsonObject> BTTestMakeEditorLayoutComment(
	const FString& Id,
	const FString& Text,
	double X,
	double Y,
	double Width,
	double Height,
	bool bIncludeColor = true)
{
	TSharedPtr<FJsonObject> Comment = MakeShared<FJsonObject>();
	Comment->SetStringField(TEXT("Id"), Id);
	Comment->SetStringField(TEXT("Text"), Text);
	Comment->SetObjectField(TEXT("Position"), BTTestMakePositionObject(X, Y));
	Comment->SetObjectField(TEXT("Size"), BTTestMakePositionObject(Width, Height));
	if (bIncludeColor)
	{
		TSharedPtr<FJsonObject> Color = MakeShared<FJsonObject>();
		Color->SetNumberField(TEXT("R"), 0.1);
		Color->SetNumberField(TEXT("G"), 0.2);
		Color->SetNumberField(TEXT("B"), 0.3);
		Color->SetNumberField(TEXT("A"), 0.4);
		Comment->SetObjectField(TEXT("Color"), Color);
	}
	return Comment;
}

TSharedPtr<FJsonObject> BTTestMakeEditorLayout(
	const TArray<TSharedPtr<FJsonValue>>& Nodes,
	const TArray<TSharedPtr<FJsonValue>>& Comments = TArray<TSharedPtr<FJsonValue>>())
{
	TSharedPtr<FJsonObject> Layout = MakeShared<FJsonObject>();
	Layout->SetArrayField(TEXT("Nodes"), Nodes);
	Layout->SetArrayField(TEXT("Comments"), Comments);
	return Layout;
}

UEdGraphNode_Comment* BTTestFindEditorLayoutComment(UBehaviorTreeGraph* Graph, const FString& Id)
{
	FGuid Guid;
	if (!Graph || !FGuid::Parse(Id, Guid))
	{
		return nullptr;
	}

	for (UEdGraphNode* GraphNode : Graph->Nodes)
	{
		UEdGraphNode_Comment* CommentNode = Cast<UEdGraphNode_Comment>(GraphNode);
		if (CommentNode && CommentNode->NodeGuid == Guid)
		{
			return CommentNode;
		}
	}
	return nullptr;
}

TSharedPtr<FJsonObject> BTTestMakeRunBehaviorTree(const FString& SubtreeTarget)
{
	TSharedPtr<FJsonObject> RunSubtreeProperties = MakeShared<FJsonObject>();
	RunSubtreeProperties->SetObjectField(TEXT("BehaviorAsset"), BTTestMakeAssetRef(BTTestMakeObjectPathFromTarget(SubtreeTarget)));
	TSharedPtr<FJsonObject> RunSubtreeNode = BTTestMakeBtNode(
		TEXT("RunSubtree"),
		TEXT("/Script/AIModule.BTTask_RunBehavior"),
		RunSubtreeProperties);

	TSharedPtr<FJsonObject> Edge = MakeShared<FJsonObject>();
	Edge->SetObjectField(TEXT("Child"), RunSubtreeNode);

	TArray<TSharedPtr<FJsonValue>> Children;
	Children.Add(BTTestMakeObjectValue(Edge));

	TSharedPtr<FJsonObject> Root = BTTestMakeBtNode(TEXT("RootSelector"), TEXT("/Script/AIModule.BTComposite_Selector"));
	Root->SetArrayField(TEXT("Children"), Children);

	TSharedPtr<FJsonObject> Tree = MakeShared<FJsonObject>();
	Tree->SetObjectField(TEXT("Root"), Root);
	return Tree;
}

TSharedPtr<FJsonObject> BTTestMakeTreeWithSelectorTask(
	TSharedPtr<FJsonObject> Selector,
	const FString& TaskClass = TEXT("/Script/AIModule.BTTask_MoveTo"))
{
	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetObjectField(TEXT("BlackboardKey"), Selector);
	TSharedPtr<FJsonObject> MoveToNode = BTTestMakeBtNode(TEXT("SelectorTask"), TaskClass, Properties);

	TSharedPtr<FJsonObject> Edge = MakeShared<FJsonObject>();
	Edge->SetObjectField(TEXT("Child"), MoveToNode);

	TSharedPtr<FJsonObject> Root = BTTestMakeBtNode(TEXT("RootSelector"), TEXT("/Script/AIModule.BTComposite_Selector"));
	Root->SetArrayField(TEXT("Children"), {BTTestMakeObjectValue(Edge)});

	TSharedPtr<FJsonObject> Tree = MakeShared<FJsonObject>();
	Tree->SetObjectField(TEXT("Root"), Root);
	return Tree;
}

TSharedPtr<FJsonObject> BTTestMakeDecoratorBehaviorTree(
	const FString& SubtreeTarget,
	bool bIncludeRootDecorator)
{
	const FString SelectorClass = TEXT("/Script/AIModule.BTComposite_Selector");
	const FString ServiceClass = TEXT("/Script/AIModule.BTService_DefaultFocus");
	const FString DecoratorClass = TEXT("/Script/AIModule.BTDecorator_Blackboard");
	const FString MoveToClass = TEXT("/Script/AIModule.BTTask_MoveTo");
	const FString RunBehaviorClass = TEXT("/Script/AIModule.BTTask_RunBehavior");

	TSharedPtr<FJsonObject> ServiceProperties = MakeShared<FJsonObject>();
	ServiceProperties->SetNumberField(TEXT("Interval"), 1.25);
	ServiceProperties->SetObjectField(TEXT("BlackboardKey"), BTTestMakeSelectorProperty(TEXT("TargetActor")));
	TArray<TSharedPtr<FJsonValue>> Services;
	Services.Add(BTTestMakeObjectValue(BTTestMakeBtNode(TEXT("FocusService"), ServiceClass, ServiceProperties)));

	TSharedPtr<FJsonObject> DecoratorProperties = MakeShared<FJsonObject>();
	DecoratorProperties->SetObjectField(TEXT("BlackboardKey"), BTTestMakeSelectorProperty(TEXT("TargetActor")));
	TArray<TSharedPtr<FJsonValue>> EdgeDecorators;
	EdgeDecorators.Add(BTTestMakeObjectValue(BTTestMakeBtNode(TEXT("HasTarget"), DecoratorClass, DecoratorProperties)));

	TSharedPtr<FJsonObject> MoveToProperties = MakeShared<FJsonObject>();
	MoveToProperties->SetObjectField(TEXT("BlackboardKey"), BTTestMakeSelectorProperty(TEXT("TargetActor")));
	TSharedPtr<FJsonObject> MoveToNode = BTTestMakeBtNode(TEXT("MoveToTarget"), MoveToClass, MoveToProperties);

	TSharedPtr<FJsonObject> MoveEdge = MakeShared<FJsonObject>();
	MoveEdge->SetObjectField(TEXT("Child"), MoveToNode);
	MoveEdge->SetArrayField(TEXT("Decorators"), EdgeDecorators);

	TSharedPtr<FJsonObject> RunSubtreeProperties = MakeShared<FJsonObject>();
	RunSubtreeProperties->SetObjectField(TEXT("BehaviorAsset"), BTTestMakeAssetRef(BTTestMakeObjectPathFromTarget(SubtreeTarget)));
	TSharedPtr<FJsonObject> RunSubtreeNode = BTTestMakeBtNode(TEXT("RunSubtree"), RunBehaviorClass, RunSubtreeProperties);

	TSharedPtr<FJsonObject> RunSubtreeEdge = MakeShared<FJsonObject>();
	RunSubtreeEdge->SetObjectField(TEXT("Child"), RunSubtreeNode);

	TArray<TSharedPtr<FJsonValue>> Children;
	Children.Add(BTTestMakeObjectValue(MoveEdge));
	Children.Add(BTTestMakeObjectValue(RunSubtreeEdge));

	TSharedPtr<FJsonObject> Root = BTTestMakeBtNode(TEXT("RootSelector"), SelectorClass);
	Root->SetArrayField(TEXT("Services"), Services);
	Root->SetArrayField(TEXT("Children"), Children);

	TSharedPtr<FJsonObject> Tree = MakeShared<FJsonObject>();
	Tree->SetObjectField(TEXT("Root"), Root);

	TArray<TSharedPtr<FJsonValue>> RootDecorators;
	if (bIncludeRootDecorator)
	{
		TSharedPtr<FJsonObject> RootDecoratorProperties = MakeShared<FJsonObject>();
		RootDecoratorProperties->SetObjectField(TEXT("BlackboardKey"), BTTestMakeSelectorProperty(TEXT("TargetActor")));
		RootDecorators.Add(BTTestMakeObjectValue(BTTestMakeBtNode(TEXT("RootHasTarget"), DecoratorClass, RootDecoratorProperties)));
	}
	Tree->SetArrayField(TEXT("RootDecorators"), RootDecorators);

	return Tree;
}

TSharedPtr<FJsonObject> BTTestMakeBehaviorTreeDocument(const FString& Target, TSharedPtr<FJsonObject> Body)
{
	TSharedPtr<FJsonObject> Document = MakeShared<FJsonObject>();
	Document->SetNumberField(TEXT("SchemaVersion"), 1);
	Document->SetStringField(TEXT("Target"), Target);
	Document->SetStringField(TEXT("Class"), TEXT("/Script/AIModule.BehaviorTree"));
	Document->SetStringField(TEXT("Action"), TEXT("CreateOrUpdate"));
	Document->SetObjectField(TEXT("Definitions"), MakeShared<FJsonObject>());
	Document->SetObjectField(TEXT("Properties"), MakeShared<FJsonObject>());
	Document->SetObjectField(TEXT("Body"), Body);
	return Document;
}

TSharedRef<FJsonObject> BTTestMakeBlackboardKeyJson(
	const FString& Name,
	const FString& Type,
	const FString& BaseClass = TEXT(""),
	const FString& Enum = TEXT(""),
	const FString& KeyTypeClass = TEXT(""),
	const FString& Description = TEXT(""))
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("Name"), Name);
	Json->SetStringField(TEXT("Category"), TEXT(""));
	Json->SetStringField(TEXT("Description"), Description);
	Json->SetBoolField(TEXT("bInstanceSynced"), false);
	if (!Type.IsEmpty())
	{
		Json->SetStringField(TEXT("Type"), Type);
	}
	if (!BaseClass.IsEmpty())
	{
		Json->SetStringField(TEXT("BaseClass"), BaseClass);
	}
	if (!Enum.IsEmpty())
	{
		Json->SetStringField(TEXT("Enum"), Enum);
	}
	if (!KeyTypeClass.IsEmpty())
	{
		Json->SetStringField(TEXT("KeyTypeClass"), KeyTypeClass);
	}
	if (Type == TEXT("Object") || Type == TEXT("Class"))
	{
		TSharedPtr<FJsonObject> KeyTypeProperties = MakeShared<FJsonObject>();
		KeyTypeProperties->SetField(TEXT("DefaultValue"), MakeShared<FJsonValueNull>());
		Json->SetObjectField(TEXT("KeyTypeProperties"), KeyTypeProperties);
	}
	else if (Type == TEXT("Bool"))
	{
		TSharedPtr<FJsonObject> KeyTypeProperties = MakeShared<FJsonObject>();
		KeyTypeProperties->SetBoolField(TEXT("bDefaultValue"), false);
		Json->SetObjectField(TEXT("KeyTypeProperties"), KeyTypeProperties);
	}
	return Json;
}

TSharedPtr<FJsonObject> BTTestMakeBlackboardDataBody(TSharedPtr<FJsonObject> Parent, TArray<TSharedRef<FJsonObject>> Keys)
{
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	if (Parent.IsValid())
	{
		Body->SetObjectField(TEXT("Parent"), Parent);
	}
	else
	{
		Body->SetField(TEXT("Parent"), MakeShared<FJsonValueNull>());
	}

	TArray<TSharedPtr<FJsonValue>> KeyValues;
	KeyValues.Reserve(Keys.Num());
	for (const TSharedRef<FJsonObject>& Key : Keys)
	{
		KeyValues.Add(MakeShared<FJsonValueObject>(Key));
	}
	Body->SetArrayField(TEXT("Keys"), MoveTemp(KeyValues));
	return Body;
}

TSharedPtr<FJsonObject> BTTestMakeBlackboardDataDocument(const FString& Target, TSharedPtr<FJsonObject> Body)
{
	TSharedPtr<FJsonObject> Document = MakeShared<FJsonObject>();
	Document->SetNumberField(TEXT("SchemaVersion"), 1);
	Document->SetStringField(TEXT("Target"), Target);
	Document->SetStringField(TEXT("Class"), TEXT("/Script/AIModule.BlackboardData"));
	// This creates GUID-unique Blackboard prerequisites through the existing
	// Blackboard Create route, including its exact Editor creation footprint.
	Document->SetStringField(TEXT("Action"), TEXT("Create"));
	Document->SetObjectField(TEXT("Definitions"), MakeShared<FJsonObject>());
	Document->SetObjectField(TEXT("Properties"), MakeShared<FJsonObject>());
	Document->SetObjectField(TEXT("Body"), Body);
	return Document;
}

TSharedPtr<FJsonObject> BTTestMakeClassRef(const FString& Path)
{
	TSharedPtr<FJsonObject> Ref = MakeShared<FJsonObject>();
	Ref->SetStringField(TEXT("Kind"), TEXT("ClassRef"));
	Ref->SetStringField(TEXT("Path"), Path);
	return Ref;
}

bool BTTestHasDiagnostic(const FAssetDocumentCapabilityResult& Result, const FString& Path, const FString& Code)
{
	return Result.Diagnostics.ContainsByPredicate([&Path, &Code](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Path == Path && Diagnostic.Code == Code;
	});
}

TSharedPtr<FJsonObject> BTTestGetObjectField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
{
	const TSharedPtr<FJsonObject>* FieldObject = nullptr;
	return Object.IsValid() && Object->TryGetObjectField(FieldName, FieldObject) && FieldObject ? *FieldObject : nullptr;
}

TSharedPtr<FJsonObject> BTTestGetObjectFromValue(const TSharedPtr<FJsonValue>& Value)
{
	const TSharedPtr<FJsonObject>* Object = nullptr;
	return Value.IsValid() && Value->TryGetObject(Object) && Object ? *Object : nullptr;
}

void BTTestAddSecondDecoratorSet(TSharedPtr<FJsonObject> Tree)
{
	TSharedPtr<FJsonObject> OtherDecoratorProperties = MakeShared<FJsonObject>();
	OtherDecoratorProperties->SetObjectField(TEXT("BlackboardKey"), BTTestMakeSelectorProperty(TEXT("OtherTargetActor")));
	TSharedPtr<FJsonObject> OtherDecorator = BTTestMakeBtNode(TEXT("HasOtherTarget"), TEXT("/Script/AIModule.BTDecorator_Blackboard"), OtherDecoratorProperties);

	TSharedPtr<FJsonObject> Root = BTTestGetObjectField(Tree, TEXT("Root"));
	const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
	if (Root.IsValid() && Root->TryGetArrayField(TEXT("Children"), Children) && Children && Children->Num() > 0)
	{
		TSharedPtr<FJsonObject> FirstEdge = BTTestGetObjectFromValue((*Children)[0]);
		const TArray<TSharedPtr<FJsonValue>>* EdgeDecorators = nullptr;
		TArray<TSharedPtr<FJsonValue>> UpdatedDecorators;
		if (FirstEdge.IsValid() && FirstEdge->TryGetArrayField(TEXT("Decorators"), EdgeDecorators) && EdgeDecorators)
		{
			UpdatedDecorators = *EdgeDecorators;
		}
		UpdatedDecorators.Add(BTTestMakeObjectValue(OtherDecorator));
		FirstEdge->SetArrayField(TEXT("Decorators"), UpdatedDecorators);
	}

	TSharedPtr<FJsonObject> OtherRootDecoratorProperties = MakeShared<FJsonObject>();
	OtherRootDecoratorProperties->SetObjectField(TEXT("BlackboardKey"), BTTestMakeSelectorProperty(TEXT("OtherTargetActor")));
	TSharedPtr<FJsonObject> OtherRootDecorator = BTTestMakeBtNode(TEXT("RootHasOtherTarget"), TEXT("/Script/AIModule.BTDecorator_Blackboard"), OtherRootDecoratorProperties);
	const TArray<TSharedPtr<FJsonValue>>* RootDecorators = nullptr;
	TArray<TSharedPtr<FJsonValue>> UpdatedRootDecorators;
	if (Tree.IsValid() && Tree->TryGetArrayField(TEXT("RootDecorators"), RootDecorators) && RootDecorators)
	{
		UpdatedRootDecorators = *RootDecorators;
	}
	UpdatedRootDecorators.Add(BTTestMakeObjectValue(OtherRootDecorator));
	Tree->SetArrayField(TEXT("RootDecorators"), UpdatedRootDecorators);
}

bool BTTestApplyDecoratorTreeFixture(
	FAutomationTestBase& Test,
	FAssetDocumentService& Service,
	FScopedBehaviorTreeMutationFixture& Fixture,
	const FString& Target,
	bool bIncludeRootDecorator = true)
{
	const FString BlackboardTarget = Target + TEXT("_BB");
	const FString SubtreeTarget = Target + TEXT("_Subtree");
	if (!Fixture.ReserveTarget(BlackboardTarget) || !Fixture.ReserveTarget(SubtreeTarget))
	{
		return false;
	}
	UBlackboardData* Blackboard = BTTestMakeDecoratorBlackboardAsset(BlackboardTarget);
	Test.TestNotNull(TEXT("fixture blackboard exists"), Blackboard);
	if (!Blackboard)
	{
		return false;
	}

	UBehaviorTree* Subtree = BTTestMakeExistingBehaviorTreeAsset(SubtreeTarget);
	Test.TestNotNull(TEXT("fixture subtree exists"), Subtree);
	if (!Subtree)
	{
		return false;
	}
	Subtree->BlackboardAsset = Blackboard;

	TSharedPtr<FJsonObject> Body = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BTTestMakeObjectPathFromTarget(BlackboardTarget)),
		BTTestMakeDecoratorBehaviorTree(SubtreeTarget, bIncludeRootDecorator),
		MakeShared<FJsonObject>());
	const FAssetDocumentResult ApplyResult = Fixture.Apply(Service, Target, BTTestMakeBehaviorTreeDocument(Target, Body));
	Test.TestTrue(TEXT("fixture apply succeeds"), ApplyResult.IsSuccess());
	if (!ApplyResult.IsSuccess())
	{
		Test.AddError(ApplyResult.Message);
	}
	return ApplyResult.IsSuccess();
}

bool BTTestCollectIdsFromAttachmentObject(const TSharedPtr<FJsonObject>& Object, TSet<FString>& SeenIds, TArray<FString>& OutIds)
{
	if (!Object.IsValid())
	{
		return true;
	}

	FString Id;
	if (!Object->TryGetStringField(TEXT("Id"), Id))
	{
		return false;
	}
	if (SeenIds.Contains(Id))
	{
		return false;
	}

	SeenIds.Add(Id);
	OutIds.Add(Id);
	return true;
}

bool BTTestCollectIdsFromTreeNode(const TSharedPtr<FJsonObject>& Node, TSet<FString>& SeenIds, TArray<FString>& OutIds)
{
	if (!BTTestCollectIdsFromAttachmentObject(Node, SeenIds, OutIds))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Decorators = nullptr;
	if (Node.IsValid() && Node->TryGetArrayField(TEXT("Decorators"), Decorators) && Decorators)
	{
		for (const TSharedPtr<FJsonValue>& DecoratorValue : *Decorators)
		{
			if (!BTTestCollectIdsFromAttachmentObject(BTTestGetObjectFromValue(DecoratorValue), SeenIds, OutIds))
			{
				return false;
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Services = nullptr;
	if (Node.IsValid() && Node->TryGetArrayField(TEXT("Services"), Services) && Services)
	{
		for (const TSharedPtr<FJsonValue>& ServiceValue : *Services)
		{
			if (!BTTestCollectIdsFromAttachmentObject(BTTestGetObjectFromValue(ServiceValue), SeenIds, OutIds))
			{
				return false;
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
	if (Node.IsValid() && Node->TryGetArrayField(TEXT("Children"), Children) && Children)
	{
		for (const TSharedPtr<FJsonValue>& ChildValue : *Children)
		{
			if (!BTTestCollectIdsFromTreeNode(BTTestGetObjectFromValue(ChildValue), SeenIds, OutIds))
			{
				return false;
			}
		}
	}

	return true;
}

bool BTTestCollectAllTreeIds(const TSharedPtr<FJsonObject>& Tree, TArray<FString>& OutIds)
{
	OutIds.Reset();
	TSet<FString> SeenIds;
	return BTTestCollectIdsFromTreeNode(BTTestGetObjectField(Tree, TEXT("Root")), SeenIds, OutIds);
}

bool BTTestJsonArrayContainsString(const TArray<TSharedPtr<FJsonValue>>* Values, const FString& Expected)
{
	if (!Values)
	{
		return false;
	}

	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		if (Value.IsValid() && Value->Type == EJson::String && Value->AsString() == Expected)
		{
			return true;
		}
	}
	return false;
}

bool BTTestRegionPoliciesContain(const TArray<TSharedPtr<FJsonValue>>* Values, const FString& ExpectedRegionId)
{
	if (!Values)
	{
		return false;
	}

	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject> Object = Value.IsValid() && Value->Type == EJson::Object
			? Value->AsObject()
			: nullptr;
		FString RegionId;
		if (Object.IsValid() && Object->TryGetStringField(TEXT("RegionId"), RegionId) && RegionId == ExpectedRegionId)
		{
			return true;
		}
	}
	return false;
}

bool BTTestLoadSidecarJson(FAutomationTestBase* Test, const FString& SidecarPath, TSharedPtr<FJsonObject>& OutDocument)
{
	FString Error;
	const bool bLoaded = FAssetDocumentSidecar::LoadJsonFile(SidecarPath, OutDocument, Error);
	Test->TestTrue(FString::Printf(TEXT("Loads sidecar JSON '%s'"), *SidecarPath), bLoaded);
	if (!bLoaded)
	{
		Test->AddError(Error);
	}
	return bLoaded;
}

bool BTTestExpectDocumentSync(
	FAutomationTestBase* Test,
	FAssetDocumentService& Service,
	const FString& Target,
	const FString& SidecarPath,
	const TSharedPtr<FJsonObject>& Document,
	const TArray<FString>& ExpectedRegionIds)
{
	FAssetDocumentJournaledTypedSyncStateStore Store(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AssetDocument/Runtime/Sync")));
	TOptional<FAssetDocumentTypedSyncStateRecord> Record;
	TArray<uint8> EncodedBytes;
	bool bExisted = false;
	const auto Loaded = Store.Load({Target, TEXT("document")}, Record, EncodedBytes, bExisted);
	Test->TestTrue(TEXT("ApplyFile document sync record loads validly"), Loaded.bValid);
	Test->TestTrue(TEXT("ApplyFile document sync record exists durably"), bExisted);
	Test->TestTrue(TEXT("ApplyFile document sync record is populated"), Record.IsSet() && !EncodedBytes.IsEmpty());
	if (!Loaded.bValid || !bExisted || !Record.IsSet() || EncodedBytes.IsEmpty()) return false;

	const auto& Pair = Record->LastDurable;
	Test->TestTrue(TEXT("document asset evidence is present"), Pair.Asset.Kind == EAssetDocumentEvidenceKind::Present);
	Test->TestTrue(TEXT("document sidecar evidence is present"), Pair.Sidecar.Kind == EAssetDocumentEvidenceKind::Present);
	Test->TestFalse(TEXT("document asset semantic hash is non-empty"), Pair.Asset.SemanticHash.IsEmpty());
	Test->TestFalse(TEXT("document sidecar semantic hash is non-empty"), Pair.Sidecar.SemanticHash.IsEmpty());
	Test->TestEqual(TEXT("document sync hashes match"), Pair.Asset.SemanticHash, Pair.Sidecar.SemanticHash);
	Test->TestEqual(TEXT("document asset codec is current v1"), Pair.Asset.CodecVersion, uint32(1));
	Test->TestEqual(TEXT("document sidecar codec is current v1"), Pair.Sidecar.CodecVersion, uint32(1));
	Test->TestTrue(TEXT("document sync request identity is valid"), Record->RequestId.IsValid());
	Test->TestTrue(TEXT("document sync transaction identity is valid"), Record->TransactionId.IsValid());
	Test->TestFalse(TEXT("document sync journal identity is non-empty"), Record->JournalId.IsEmpty());

	TArray<uint8> SidecarBytes;
	const bool bReadBytes = FFileHelper::LoadFileToArray(SidecarBytes, *SidecarPath);
	Test->TestTrue(TEXT("document sync reads actual durable sidecar bytes"), bReadBytes && !SidecarBytes.IsEmpty());
	if (!bReadBytes || SidecarBytes.IsEmpty()) return false;
	// Existing DocumentCodec v1 semantic domain, followed by the compact encoding of the installed sidecar. The sidecar
	// is the indented form of that encoding (DocumentCodec.encode_sidecar): the same text with whitespace between tokens.
	static const char Domain[] = "AssetDocumentSemanticHash\0v1\0document";
	TArray<uint8> SemanticBytes;
	SemanticBytes.Append(reinterpret_cast<const uint8*>(Domain), UE_ARRAY_COUNT(Domain));
	bool bInString = false;
	bool bEscaped = false;
	for (const uint8 Byte : SidecarBytes)
	{
		if (bInString)
		{
			bInString = bEscaped || Byte != '"';
			bEscaped = !bEscaped && Byte == '\\';
		}
		else if (Byte == ' ' || Byte == '\n')
		{
			continue;
		}
		else
		{
			bInString = Byte == '"';
		}
		SemanticBytes.Add(Byte);
	}
	FString SidecarDigest = ComputeAssetDocumentContentDigest(SemanticBytes);
	Test->TestTrue(TEXT("document sidecar digest uses existing SHA-256 format"), SidecarDigest.RemoveFromStart(TEXT("sha256:")));
	const FString ActualSidecarHash = TEXT("adsh:v1:document:") + SidecarDigest;
	Test->TestEqual(TEXT("document durable hash binds actual sidecar content"), Pair.Sidecar.SemanticHash, ActualSidecarHash);

	for (const FAssetDocumentSyncStateKey& WrongKey : TArray<FAssetDocumentSyncStateKey>{
		{Target + TEXT("_WrongTarget"), TEXT("document")}, {Target, TEXT("Body.Tree")}})
	{
		TOptional<FAssetDocumentTypedSyncStateRecord> WrongRecord;
		TArray<uint8> WrongBytes;
		bool bWrongExisted = false;
		const auto WrongLoaded = Store.Load(WrongKey, WrongRecord, WrongBytes, bWrongExisted);
		Test->TestTrue(TEXT("unrelated sync key lookup is valid"), WrongLoaded.bValid);
		Test->TestFalse(TEXT("document sync cannot be read through another target or region"), bWrongExisted || WrongRecord.IsSet() || !WrongBytes.IsEmpty());
	}

	FAssetDocumentExtractRequest ExtractRequest;
	ExtractRequest.AssetPath = Target;
	ExtractRequest.bDiffOnly = true;
	const FAssetDocumentResult Extracted = Service.Extract(ExtractRequest);
	Test->TestTrue(TEXT("document sync native content readback succeeds"), Extracted.IsSuccess() && Extracted.Payload.IsValid());
	const auto SidecarBody = BTTestGetObjectField(Document, TEXT("Body"));
	const auto NativeBody = BTTestGetObjectField(Extracted.Payload, TEXT("Body"));
	Test->TestTrue(TEXT("document sync sidecar and native complete bodies exist"), SidecarBody.IsValid() && NativeBody.IsValid());
	if (!SidecarBody.IsValid() || !NativeBody.IsValid()) return false;
	for (const FString& RegionId : ExpectedRegionIds)
	{
		FString Prefix, Field;
		const bool bBodyRegion = RegionId.Split(TEXT("."), &Prefix, &Field) && Prefix == TEXT("Body");
		const auto* SidecarValue = bBodyRegion ? AssetDocumentServerJson::FindField(*SidecarBody, Field) : nullptr;
		const auto* NativeValue = bBodyRegion ? AssetDocumentServerJson::FindField(*NativeBody, Field) : nullptr;
		Test->TestTrue(FString::Printf(TEXT("canonical sidecar includes complete %s"), *RegionId), SidecarValue && SidecarValue->IsValid());
		Test->TestTrue(FString::Printf(TEXT("native readback includes complete %s"), *RegionId), NativeValue && NativeValue->IsValid());
		if (!SidecarValue || !SidecarValue->IsValid() || !NativeValue || !NativeValue->IsValid()) return false;
		Test->TestTrue(FString::Printf(TEXT("native and canonical sidecar complete %s contents match"), *RegionId),
			FJsonValue::CompareEqual(**SidecarValue, **NativeValue));
	}
	return true;
}

bool BTTestApplyFullBlackboardFixture(
	FAutomationTestBase& Test,
	FAssetDocumentService& Service,
	FScopedBehaviorTreeMutationFixture& Fixture,
	const FString& ParentBlackboardTarget,
	const FString& BlackboardTarget)
{
	const FAssetDocumentResult ParentApply = Fixture.Apply(Service, ParentBlackboardTarget, BTTestMakeBlackboardDataDocument(
		ParentBlackboardTarget,
		BTTestMakeBlackboardDataBody(nullptr, {
			BTTestMakeBlackboardKeyJson(TEXT("TargetActor"), TEXT("Object"), AActor::StaticClass()->GetPathName(), TEXT(""), TEXT(""), TEXT("Inherited target")),
			BTTestMakeBlackboardKeyJson(TEXT("HasTarget"), TEXT("Bool"),
				TEXT(""),
				TEXT(""),
				TEXT(""),
				TEXT("Inherited visibility gate")),
		})));
	Test.TestTrue(TEXT("parent blackboard apply succeeds"), ParentApply.IsSuccess());
	if (!ParentApply.IsSuccess())
	{
		Test.AddError(ParentApply.Message);
		return false;
	}

	TSharedPtr<FJsonObject> LocalBody = BTTestMakeBlackboardDataBody(BTTestMakeAssetRef(BTTestMakeObjectPathFromTarget(ParentBlackboardTarget)), {
		BTTestMakeBlackboardKeyJson(TEXT("OtherTargetActor"), TEXT("Object"), AActor::StaticClass()->GetPathName()),
		BTTestMakeBlackboardKeyJson(TEXT("SpareTargetActor"), TEXT("Object"), AActor::StaticClass()->GetPathName()),
	});
	const FAssetDocumentResult LocalApply = Fixture.Apply(Service, BlackboardTarget, BTTestMakeBlackboardDataDocument(BlackboardTarget, LocalBody));
	Test.TestTrue(TEXT("local blackboard apply succeeds"), LocalApply.IsSuccess());
	if (!LocalApply.IsSuccess())
	{
		Test.AddError(LocalApply.Message);
	}
	return LocalApply.IsSuccess();
}

TSharedPtr<FJsonObject> BTTestMakeFullEditorLayout()
{
	return BTTestMakeEditorLayout(
		{
			BTTestMakeObjectValue(BTTestMakeEditorLayoutNode(TEXT("RootSelector"), 0, 0)),
			BTTestMakeObjectValue(BTTestMakeEditorLayoutNode(TEXT("RootHasTarget"), -180, -160)),
			BTTestMakeObjectValue(BTTestMakeEditorLayoutNode(TEXT("FocusService"), -240, 120)),
			BTTestMakeObjectValue(BTTestMakeEditorLayoutNode(TEXT("HasTarget"), -120, 320)),
			BTTestMakeObjectValue(BTTestMakeEditorLayoutNode(TEXT("MoveToTarget"), 80, 420)),
			BTTestMakeObjectValue(BTTestMakeEditorLayoutNode(TEXT("RunSubtree"), 420, 420)),
		},
		{
			BTTestMakeObjectValue(BTTestMakeEditorLayoutComment(
				TEXT("55555555-6666-7777-8888-999999999999"),
				TEXT("Full BT+BB roundtrip"),
				-320,
				-220,
				900,
				760)),
		});
}

TSharedPtr<FJsonObject> BTTestMakeReflectedFullNodeProperties(UClass* NodeClass, const TSharedRef<FJsonObject>& SparseProperties)
{
	// Node properties are authored sparsely (only the fields that differ from the class default).
	return SparseProperties;
}

TSharedRef<FJsonObject> BTTestMakeFullSparseNodeProperties(const FString& NodeName)
{
	TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetStringField(TEXT("NodeName"), NodeName);
	return Properties;
}

TSharedPtr<FJsonObject> BTTestMakeFullCompositeProperties(const FString& NodeName)
{
	return BTTestMakeReflectedFullNodeProperties(UBTComposite_Selector::StaticClass(), BTTestMakeFullSparseNodeProperties(NodeName));
}

TSharedPtr<FJsonObject> BTTestMakeFullDefaultFocusProperties(const FString& NodeName)
{
	TSharedRef<FJsonObject> Properties = BTTestMakeFullSparseNodeProperties(NodeName);
	Properties->SetNumberField(TEXT("Interval"), 1.25);
	Properties->SetObjectField(TEXT("BlackboardKey"), BTTestMakeSelectorProperty(TEXT("TargetActor")));
	return BTTestMakeReflectedFullNodeProperties(UBTService_DefaultFocus::StaticClass(), Properties);
}

TSharedPtr<FJsonObject> BTTestMakeFullBlackboardDecoratorProperties(const FString& NodeName)
{
	TSharedRef<FJsonObject> Properties = BTTestMakeFullSparseNodeProperties(NodeName);
	Properties->SetObjectField(TEXT("BlackboardKey"), BTTestMakeSelectorProperty(TEXT("TargetActor")));
	return BTTestMakeReflectedFullNodeProperties(UBTDecorator_Blackboard::StaticClass(), Properties);
}

TSharedPtr<FJsonObject> BTTestMakeFullMoveToProperties(const FString& NodeName)
{
	TSharedRef<FJsonObject> Properties = BTTestMakeFullSparseNodeProperties(NodeName);
	Properties->SetObjectField(TEXT("BlackboardKey"), BTTestMakeSelectorProperty(TEXT("TargetActor")));
	return BTTestMakeReflectedFullNodeProperties(UBTTask_MoveTo::StaticClass(), Properties);
}

TSharedPtr<FJsonObject> BTTestMakeFullRunBehaviorProperties(const FString& NodeName, const FString& SubtreeTarget)
{
	TSharedRef<FJsonObject> Properties = BTTestMakeFullSparseNodeProperties(NodeName);
	Properties->SetObjectField(TEXT("BehaviorAsset"), BTTestMakeAssetRef(BTTestMakeObjectPathFromTarget(SubtreeTarget)));
	return BTTestMakeReflectedFullNodeProperties(UBTTask_RunBehavior::StaticClass(), Properties);
}

TSharedPtr<FJsonObject> BTTestMakeFullBehaviorTree(const FString& SubtreeTarget)
{
	TArray<TSharedPtr<FJsonValue>> Services;
	Services.Add(BTTestMakeObjectValue(BTTestMakeBtNode(
		TEXT("FocusService"),
		TEXT("/Script/AIModule.BTService_DefaultFocus"),
		BTTestMakeFullDefaultFocusProperties(TEXT("FocusService")))));

	TArray<TSharedPtr<FJsonValue>> EdgeDecorators;
	EdgeDecorators.Add(BTTestMakeObjectValue(BTTestMakeBtNode(
		TEXT("HasTarget"),
		TEXT("/Script/AIModule.BTDecorator_Blackboard"),
		BTTestMakeFullBlackboardDecoratorProperties(TEXT("HasTarget")))));

	TSharedPtr<FJsonObject> MoveEdge = MakeShared<FJsonObject>();
	MoveEdge->SetObjectField(TEXT("Child"), BTTestMakeBtNode(
		TEXT("MoveToTarget"),
		TEXT("/Script/AIModule.BTTask_MoveTo"),
		BTTestMakeFullMoveToProperties(TEXT("MoveToTarget"))));
	MoveEdge->SetArrayField(TEXT("Decorators"), EdgeDecorators);

	TSharedPtr<FJsonObject> RunSubtreeEdge = MakeShared<FJsonObject>();
	RunSubtreeEdge->SetObjectField(TEXT("Child"), BTTestMakeBtNode(
		TEXT("RunSubtree"),
		TEXT("/Script/AIModule.BTTask_RunBehavior"),
		BTTestMakeFullRunBehaviorProperties(TEXT("RunSubtree"), SubtreeTarget)));

	TArray<TSharedPtr<FJsonValue>> Children;
	Children.Add(BTTestMakeObjectValue(MoveEdge));
	Children.Add(BTTestMakeObjectValue(RunSubtreeEdge));

	TSharedPtr<FJsonObject> Root = BTTestMakeBtNode(
		TEXT("RootSelector"),
		TEXT("/Script/AIModule.BTComposite_Selector"),
		BTTestMakeFullCompositeProperties(TEXT("RootSelector")));
	Root->SetArrayField(TEXT("Services"), Services);
	Root->SetArrayField(TEXT("Children"), Children);

	TArray<TSharedPtr<FJsonValue>> RootDecorators;
	RootDecorators.Add(BTTestMakeObjectValue(BTTestMakeBtNode(
		TEXT("RootHasTarget"),
		TEXT("/Script/AIModule.BTDecorator_Blackboard"),
		BTTestMakeFullBlackboardDecoratorProperties(TEXT("RootHasTarget")))));

	TSharedPtr<FJsonObject> Tree = MakeShared<FJsonObject>();
	Tree->SetObjectField(TEXT("Root"), Root);
	Tree->SetArrayField(TEXT("RootDecorators"), RootDecorators);
	return Tree;
}

TSharedPtr<FJsonObject> BTTestMakeFullBehaviorTreeBody(
	const FString& BlackboardTarget,
	const FString& SubtreeTarget)
{
	return BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BTTestMakeObjectPathFromTarget(BlackboardTarget)),
		BTTestMakeFullBehaviorTree(SubtreeTarget),
		BTTestMakeFullEditorLayout());
}

bool BTTestPrepareFullBehaviorTreeFixture(
	FAutomationTestBase& Test,
	FAssetDocumentService& Service,
	FScopedBehaviorTreeMutationFixture& Fixture,
	const FString& Target,
	FString& OutParentBlackboardTarget,
	FString& OutBlackboardTarget,
	FString& OutSubtreeTarget)
{
	OutParentBlackboardTarget = Target + TEXT("_ParentBB");
	OutBlackboardTarget = Target + TEXT("_BB");
	OutSubtreeTarget = Target + TEXT("_Subtree");
	if (!BTTestApplyFullBlackboardFixture(Test, Service, Fixture, OutParentBlackboardTarget, OutBlackboardTarget))
	{
		return false;
	}

	if (!Fixture.ReserveTarget(OutSubtreeTarget))
	{
		return false;
	}
	UBehaviorTree* Subtree = BTTestMakeExistingBehaviorTreeAsset(OutSubtreeTarget);
	Test.TestNotNull(TEXT("subtree asset exists"), Subtree);
	if (!Subtree)
	{
		return false;
	}

	Subtree->BlackboardAsset = LoadObject<UBlackboardData>(nullptr, *BTTestMakeObjectPathFromTarget(OutBlackboardTarget));
	Test.TestNotNull(TEXT("subtree blackboard resolves"), Subtree->BlackboardAsset.Get());
	return Subtree->BlackboardAsset != nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeProfileShapeTest,
	"UEAssetDoc.BehaviorTree.ProfileShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeProfileShapeTest::RunTest(const FString&)
{
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	FAssetDocumentTemplateRequest TemplateRequest;
	TemplateRequest.Target = TEXT("/Game/AssetDocumentTests/BT_ProfileShape");
	TemplateRequest.Class = TEXT("/Script/AIModule.BehaviorTree");
	const FAssetDocumentResult TemplateResult = Service.CreateTemplate(TemplateRequest);
	TestTrue(TEXT("public BehaviorTree template succeeds"), TemplateResult.IsSuccess());
	if (!TemplateResult.IsSuccess() || !TemplateResult.Payload.IsValid()) return false;
	const TSharedPtr<FJsonObject> Template = TemplateResult.Payload;
	TestEqual(TEXT("Template class is BehaviorTree"), Template->GetStringField(TEXT("Class")), FString(TEXT("/Script/AIModule.BehaviorTree")));

	const TSharedPtr<FJsonObject>* Body = nullptr;
	TestTrue(TEXT("Template contains Body"), Template->TryGetObjectField(TEXT("Body"), Body) && Body && Body->IsValid());
	if (Body && Body->IsValid())
	{
		TestTrue(TEXT("Body contains BlackboardAsset"), (*Body)->HasField(TEXT("BlackboardAsset")));
		TestTrue(TEXT("Body contains Tree"), (*Body)->HasField(TEXT("Tree")));
		TestFalse(TEXT("Body does not split editor state from Tree"), (*Body)->HasField(TEXT("EditorLayout")));
		TestFalse(TEXT("Body does not contain BlackboardInline"), (*Body)->HasField(TEXT("BlackboardInline")));

		const TSharedPtr<FJsonObject>* Tree = nullptr;
		TestTrue(TEXT("Tree is an object"), (*Body)->TryGetObjectField(TEXT("Tree"), Tree) && Tree && Tree->IsValid());
		if (Tree && Tree->IsValid())
		{
			TestTrue(TEXT("Tree contains GraphGuid"), (*Tree)->HasField(TEXT("GraphGuid")));
			TestTrue(TEXT("Tree contains Root"), (*Tree)->HasField(TEXT("Root")));
			TestTrue(TEXT("Tree contains Comments"), (*Tree)->HasField(TEXT("Comments")));
			TestFalse(TEXT("Tree does not expose runtime RootDecorators"), (*Tree)->HasField(TEXT("RootDecorators")));
			TestFalse(TEXT("Tree does not expose runtime RootDecoratorLogic"), (*Tree)->HasField(TEXT("RootDecoratorLogic")));
		}
	}

	FAssetDocumentProfileRequest ProfileRequest;
	ProfileRequest.ClassOrAsset = TemplateRequest.Class;
	const FAssetDocumentResult Profile = Service.InspectProfile(ProfileRequest);
	TestTrue(TEXT("public BehaviorTree profile succeeds"), Profile.IsSuccess());
	if (!Profile.Payload.IsValid()) return false;
	const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
	TestTrue(TEXT("profile has BodySections"), Profile.Payload->TryGetArrayField(TEXT("BodySections"), Sections));
	TestTrue(TEXT("BlackboardAsset body key is registered"), BTTestJsonArrayContainsString(Sections, TEXT("BlackboardAsset")));
	TestTrue(TEXT("Tree body key is registered"), BTTestJsonArrayContainsString(Sections, TEXT("Tree")));
	TestFalse(TEXT("EditorLayout body key is not registered"), BTTestJsonArrayContainsString(Sections, TEXT("EditorLayout")));
	TestFalse(TEXT("BlackboardInline body key is not registered"), BTTestJsonArrayContainsString(Sections, TEXT("BlackboardInline")));
	const TArray<TSharedPtr<FJsonValue>>* Policies = nullptr;
	TestTrue(TEXT("profile has RegionPolicies"), Profile.Payload->TryGetArrayField(TEXT("RegionPolicies"), Policies));
	TestTrue(TEXT("Policy includes Body.BlackboardAsset"), BTTestRegionPoliciesContain(Policies, TEXT("Body.BlackboardAsset")));
	TestTrue(TEXT("Policy includes Body.Tree"), BTTestRegionPoliciesContain(Policies, TEXT("Body.Tree")));
	TestFalse(TEXT("Policy omits Body.EditorLayout"), BTTestRegionPoliciesContain(Policies, TEXT("Body.EditorLayout")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeBlackboardReferenceTest,
	"UEAssetDoc.BehaviorTree.BlackboardReference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeBlackboardReferenceTest::RunTest(const FString&)
{
	const FString BlackboardTarget = BTTestMakeUniqueTarget(TEXT("BB_BT_BlackboardReference"));
	const FString BehaviorTreeTarget = BTTestMakeUniqueTarget(TEXT("BT_AD_BlackboardReference"));
	FScopedBehaviorTreeMutationFixture Fixture;
	if (!Fixture.ReserveTarget(BlackboardTarget))
	{
		return false;
	}
	UBlackboardData* Blackboard = BTTestMakeExistingBlackboardAsset(BlackboardTarget);
	TestNotNull(TEXT("Referenced blackboard asset exists"), Blackboard);
	if (!Blackboard)
	{
		return false;
	}

	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	TSharedPtr<FJsonObject> Body = BTTestMakeBehaviorTreeBody(BTTestMakeAssetRef(BTTestMakeObjectPathFromTarget(BlackboardTarget)), BTTestMakeEmptyBehaviorTree(), MakeShared<FJsonObject>());
	const FAssetDocumentResult ApplyResult = Fixture.Apply(Service, BehaviorTreeTarget, BTTestMakeBehaviorTreeDocument(BehaviorTreeTarget, Body));
	TestTrue(TEXT("BehaviorTree apply with Blackboard AssetRef succeeds"), ApplyResult.IsSuccess());
	if (!ApplyResult.IsSuccess())
	{
		AddError(ApplyResult.Message);
		return false;
	}

	UBehaviorTree* BehaviorTree = BTTestLoadBehaviorTreeForTarget(BehaviorTreeTarget);
	TestNotNull(TEXT("BehaviorTree lifecycle creates an asset"), BehaviorTree);
	if (!BehaviorTree)
	{
		return false;
	}
	TestTrue(TEXT("BehaviorTree has exact class"), BehaviorTree->GetClass() == UBehaviorTree::StaticClass());
	TestTrue(TEXT("BlackboardAsset is assigned"), BehaviorTree->BlackboardAsset == Blackboard);

	FAssetDocumentExtractRequest ExtractRequest;
	ExtractRequest.AssetPath = BehaviorTreeTarget;
	ExtractRequest.bDiffOnly = true;
	const FAssetDocumentResult ExtractResult = Service.Extract(ExtractRequest);
	TestTrue(TEXT("BehaviorTree extract succeeds"), ExtractResult.IsSuccess());
	const TSharedPtr<FJsonObject>* ExtractedBody = nullptr;
	const TSharedPtr<FJsonObject>* ExtractedBlackboard = nullptr;
	TestTrue(TEXT("Extracted payload contains Body"), ExtractResult.Payload.IsValid() && ExtractResult.Payload->TryGetObjectField(TEXT("Body"), ExtractedBody) && ExtractedBody && ExtractedBody->IsValid());
	TestTrue(TEXT("Extracted Body contains BlackboardAsset AssetRef"), ExtractedBody && (*ExtractedBody)->TryGetObjectField(TEXT("BlackboardAsset"), ExtractedBlackboard) && ExtractedBlackboard && ExtractedBlackboard->IsValid());
	if (ExtractedBlackboard && ExtractedBlackboard->IsValid())
	{
		TestEqual(TEXT("Extracted Blackboard Kind"), (*ExtractedBlackboard)->GetStringField(TEXT("Kind")), FString(TEXT("AssetRef")));
		TestEqual(TEXT("Extracted Blackboard Path"), (*ExtractedBlackboard)->GetStringField(TEXT("Path")), Blackboard->GetPathName());
	}

	const FAssetDocumentResult DiffResult = Service.Diff(BTTestMakeDiffRequest(BTTestMakeBehaviorTreeDocument(BehaviorTreeTarget, Body)));
	TestTrue(TEXT("BehaviorTree diff succeeds"), DiffResult.IsSuccess());
	TestTrue(TEXT("BlackboardAsset diff path is stable"), BTTestDiffPayloadHasEntry(DiffResult.Payload, TEXT("unchanged"), TEXT("/Body/BlackboardAsset")));
	UBehaviorTreeGraph* SavedGraph = Cast<UBehaviorTreeGraph>(BehaviorTree->BTGraph);
	if (!TestNotNull(TEXT("saved empty tree retains its authored graph"), SavedGraph)) return false;
	const FGuid SavedGraphGuid = SavedGraph->GraphGuid;
	const auto SavedNodes = SavedGraph->Nodes;
	for (const bool ClearBlackboard : {false, true})
	{
		TSharedPtr<FJsonObject> OverlayBody = MakeShared<FJsonObject>();
		if (ClearBlackboard) OverlayBody->SetField(TEXT("BlackboardAsset"), MakeShared<FJsonValueNull>());
		const auto OverlayDocument = BTTestMakeBehaviorTreeDocument(BehaviorTreeTarget, OverlayBody);
		OverlayDocument->SetStringField(TEXT("Action"), TEXT("Update"));
		const auto Overlay = Fixture.Apply(Service, BehaviorTreeTarget, OverlayDocument);
		TestTrue(ClearBlackboard ? TEXT("saved empty tree accepts a Blackboard-only overlay")
			: TEXT("saved empty tree accepts an explicit empty Body refresh"), Overlay.IsSuccess());
		if (!Overlay.IsSuccess()) { AddError(Overlay.Message); return false; }
		TestTrue(TEXT("retained empty graph and node identities stay exact"),
			BehaviorTree->BTGraph == SavedGraph && SavedGraph->GraphGuid == SavedGraphGuid && SavedGraph->Nodes == SavedNodes);
		TestTrue(TEXT("retained empty runtime stays empty"), BehaviorTree->RootNode == nullptr
			&& BehaviorTree->RootDecorators.IsEmpty() && BehaviorTree->RootDecoratorOps.IsEmpty());
		TestTrue(TEXT("retained empty tree has the requested Blackboard"),
			BehaviorTree->BlackboardAsset == (ClearBlackboard ? nullptr : Blackboard));
		TestFalse(TEXT("retained empty tree is clean after persistence"), BehaviorTree->GetOutermost()->IsDirty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeRejectsBlackboardInlineTest,
	"UEAssetDoc.BehaviorTree.RejectsBlackboardInline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeRejectsBlackboardInlineTest::RunTest(const FString&)
{
	TSharedPtr<FJsonObject> Body = BTTestMakeBehaviorTreeBody(nullptr, BTTestMakeEmptyBehaviorTree(), MakeShared<FJsonObject>());
	Body->SetObjectField(TEXT("BlackboardInline"), MakeShared<FJsonObject>());

	FAssetDocumentValidateRequest Request;
	Request.Document = BTTestMakeBehaviorTreeDocument(TEXT("/Game/AssetDocumentTests/BT_AD_RejectsBlackboardInline"), Body);
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Validate(Request);
	TestFalse(TEXT("BehaviorTree rejects BlackboardInline"), Result.IsSuccess());
	TestTrue(TEXT("BlackboardInline reports unknown region"), BTTestResultHasDiagnostic(Result, TEXT("UnknownBodyKey"), TEXT("/Body/BlackboardInline")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeRejectsMissingBlackboardForKeySelectorsTest,
	"UEAssetDoc.BehaviorTree.RejectsMissingBlackboardForKeySelectors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeRejectsMissingBlackboardForKeySelectorsTest::RunTest(const FString&)
{
	TSharedPtr<FJsonObject> Body = BTTestMakeBehaviorTreeBody(nullptr, BTTestMakeTreeWithKeySelectorLikeProperty(), MakeShared<FJsonObject>());

	FAssetDocumentValidateRequest Request;
	Request.Document = BTTestMakeBehaviorTreeDocument(TEXT("/Game/AssetDocumentTests/BT_AD_RejectsMissingBlackboardForKeySelectors"), Body);
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Validate(Request);
	TestFalse(TEXT("BehaviorTree rejects key selectors without Blackboard"), Result.IsSuccess());
	TestTrue(TEXT("Missing blackboard diagnostic is stable"), BTTestResultHasDiagnostic(Result, TEXT("MissingBehaviorTreeBlackboard"), TEXT("/Body/BlackboardAsset")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeTreeTest,
	"UEAssetDoc.BehaviorTree.Tree",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeTreeTest::RunTest(const FString&)
{
	const FString Target = BTTestMakeUniqueTarget(TEXT("BT_AD_Tree"));
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	FScopedBehaviorTreeMutationFixture Fixture;
	if (!BTTestApplyDecoratorTreeFixture(*this, Service, Fixture, Target))
	{
		return false;
	}

	UBehaviorTree* BehaviorTree = BTTestLoadBehaviorTreeForTarget(Target);
	TestNotNull(TEXT("BehaviorTree exists after semantic tree apply"), BehaviorTree);
	UBTCompositeNode* Root = BehaviorTree ? BehaviorTree->RootNode : nullptr;
	TestNotNull(TEXT("semantic RootNode is materialized"), Root);
	if (!Root)
	{
		return false;
	}

	TestTrue(TEXT("root is selector class"), Root->GetClass()->GetPathName().Contains(TEXT("BTComposite_Selector")));
	TestEqual(TEXT("root services materialized"), Root->Services.Num(), 1);
	TestEqual(TEXT("root children materialized"), Root->Children.Num(), 2);
	UBTTaskNode* MoveToTask = Root->Children.IsValidIndex(0) ? Root->Children[0].ChildTask : nullptr;
	UBTTaskNode* RunSubtreeTask = Root->Children.IsValidIndex(1) ? Root->Children[1].ChildTask : nullptr;
	TestTrue(TEXT("first child is BTTask_MoveTo"), MoveToTask && MoveToTask->GetClass()->IsChildOf(UBTTask_MoveTo::StaticClass()));
	TestTrue(TEXT("second child is BTTask_RunBehavior"), RunSubtreeTask && RunSubtreeTask->GetClass()->IsChildOf(UBTTask_RunBehavior::StaticClass()));
	TestEqual(TEXT("edge decorator materialized"), Root->Children[0].Decorators.Num(), 1);

	FAssetDocumentExtractRequest ExtractRequest;
	ExtractRequest.AssetPath = Target;
	ExtractRequest.bDiffOnly = true;
	const FAssetDocumentResult ExtractResult = Service.Extract(ExtractRequest);
	TestTrue(TEXT("semantic tree extract succeeds"), ExtractResult.IsSuccess());
	const TSharedPtr<FJsonObject>* Body = nullptr;
	const TSharedPtr<FJsonObject>* Tree = nullptr;
	const TSharedPtr<FJsonObject>* ExtractedRoot = nullptr;
	TestTrue(TEXT("extract payload contains Body.Tree.Root"), ExtractResult.Payload.IsValid()
		&& ExtractResult.Payload->TryGetObjectField(TEXT("Body"), Body)
		&& Body && (*Body)->TryGetObjectField(TEXT("Tree"), Tree)
		&& Tree && (*Tree)->TryGetObjectField(TEXT("Root"), ExtractedRoot));
	if (ExtractedRoot && ExtractedRoot->IsValid())
	{
		TestEqual(TEXT("root NodeGuid extracted"), (*ExtractedRoot)->GetStringField(TEXT("Id")), BTTestCanonicalGuid(TEXT("Node:RootSelector")));
		TestEqual(TEXT("root class extracted as full path"), (*ExtractedRoot)->GetStringField(TEXT("Class")), FString(TEXT("/Script/AIModule.BTComposite_Selector")));
		TestEqual(TEXT("visible root label is an authored property"), BTTestGetObjectField(*ExtractedRoot, TEXT("Properties"))->GetStringField(TEXT("NodeName")), FString(TEXT("RootSelector")));
		const TArray<TSharedPtr<FJsonValue>>* ExtractedChildren = nullptr;
		TestTrue(TEXT("children extracted"), (*ExtractedRoot)->TryGetArrayField(TEXT("Children"), ExtractedChildren) && ExtractedChildren && ExtractedChildren->Num() == 2);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeEmptyTreeExtractTest,
	"UEAssetDoc.BehaviorTree.EmptyTreeExtract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeEmptyTreeExtractTest::RunTest(const FString&)
{
	const FString Target = BTTestMakeUniqueTarget(TEXT("BT_AD_EmptyExtract"));
	FScopedBehaviorTreeMutationFixture Cleanup;
	if (!Cleanup.ReserveTarget(Target)) return false;
	UBehaviorTree* EmptyTree = NewObject<UBehaviorTree>(
		CreatePackage(*Target),
		*FPackageName::GetLongPackageAssetName(Target),
		RF_Public | RF_Standalone | RF_Transactional);
	TestNotNull(TEXT("empty BehaviorTree asset exists"), EmptyTree);
	if (!EmptyTree)
	{
		return false;
	}

	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	FAssetDocumentExtractRequest ExtractRequest;
	ExtractRequest.AssetPath = Target;
	ExtractRequest.bDiffOnly = true;
	const FAssetDocumentResult ExtractResult = Service.Extract(ExtractRequest);
	TestTrue(TEXT("empty BehaviorTree extract succeeds"), ExtractResult.IsSuccess());

	const TSharedPtr<FJsonObject>* Body = nullptr;
	const TSharedPtr<FJsonObject>* Tree = nullptr;
	const TSharedPtr<FJsonObject>* Root = nullptr;
	TestTrue(TEXT("empty extract payload contains Body.Tree.Root"), ExtractResult.Payload.IsValid()
		&& ExtractResult.Payload->TryGetObjectField(TEXT("Body"), Body)
		&& Body && (*Body)->TryGetObjectField(TEXT("Tree"), Tree)
		&& Tree && (*Tree)->TryGetObjectField(TEXT("Root"), Root));
	TestEqual(TEXT("empty root is strict empty object"), Root && Root->IsValid() ? (*Root)->Values.Num() : -1, 0);

	TSharedPtr<FJsonObject> BodyDocument = BTTestMakeBehaviorTreeBody(nullptr, BTTestMakeEmptyBehaviorTree(), MakeShared<FJsonObject>());
	const FAssetDocumentResult DiffResult = Service.Diff(BTTestMakeDiffRequest(BTTestMakeBehaviorTreeDocument(Target, BodyDocument)));
	TestTrue(TEXT("empty BehaviorTree diff succeeds"), DiffResult.IsSuccess());
	TestTrue(TEXT("empty tree graph identity diff path is stable"), BTTestDiffPayloadHasAnyEntry(DiffResult.Payload, TEXT("/Body/Tree/GraphGuid")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeRootDecoratorsTest,
	"UEAssetDoc.BehaviorTree.RootDecorators",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeRootDecoratorsTest::RunTest(const FString&)
{
	const FString Target = BTTestMakeUniqueTarget(TEXT("BT_AD_RootDecorators"));
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	FScopedBehaviorTreeMutationFixture Fixture;
	if (!BTTestApplyDecoratorTreeFixture(*this, Service, Fixture, Target, true))
	{
		return false;
	}

	UBehaviorTree* BehaviorTree = BTTestLoadBehaviorTreeForTarget(Target);
	TestNotNull(TEXT("BehaviorTree exists"), BehaviorTree);
	TestEqual(TEXT("root decorators materialized"), BehaviorTree ? BehaviorTree->RootDecorators.Num() : 0, 1);
	TestEqual(TEXT("ordinary root decorators do not author runtime logic"), BehaviorTree ? BehaviorTree->RootDecoratorOps.Num() : -1, 0);

	TSharedPtr<FJsonObject> DesiredBody = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BehaviorTree->BlackboardAsset->GetPathName()),
		BTTestMakeDecoratorBehaviorTree(Target + TEXT("_Subtree"), true),
		MakeShared<FJsonObject>());
	TSharedPtr<FJsonObject> DesiredRoot = BTTestGetObjectField(BTTestGetObjectField(DesiredBody, TEXT("Tree")), TEXT("Root"));
	const TArray<TSharedPtr<FJsonValue>>* DesiredDecorators = nullptr;
	if (DesiredRoot.IsValid()
		&& DesiredRoot->TryGetArrayField(TEXT("Decorators"), DesiredDecorators)
		&& DesiredDecorators
		&& DesiredDecorators->Num() == 1)
	{
		BTTestGetObjectField(
			BTTestGetObjectField(BTTestGetObjectFromValue((*DesiredDecorators)[0]), TEXT("Properties")),
			TEXT("BlackboardKey"))->SetStringField(TEXT("Key"), TEXT("OtherTargetActor"));
	}
	const FAssetDocumentResult DiffResult = Service.Diff(BTTestMakeDiffRequest(BTTestMakeBehaviorTreeDocument(Target, DesiredBody)));
	TestTrue(TEXT("root decorator diff succeeds"), DiffResult.IsSuccess());
	const FString RootDecoratorPath = BTTestTreePointer(BTTestGetObjectField(DesiredBody, TEXT("Tree")),
		BTTestCanonicalGuid(TEXT("Node:RootHasTarget"))) + TEXT("/Properties/BlackboardKey/Key");
	TestTrue(TEXT("root decorator canonical identity path is emitted"), BTTestDiffPayloadHasAnyEntry(DiffResult.Payload, RootDecoratorPath));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeDecoratorLogicTest,
	"UEAssetDoc.BehaviorTree.DecoratorLogic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeDecoratorLogicTest::RunTest(const FString&)
{
	const FString Target = BTTestMakeUniqueTarget(TEXT("BT_AD_DecoratorLogic"));
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	FScopedBehaviorTreeMutationFixture Fixture;
	if (!BTTestApplyDecoratorTreeFixture(*this, Service, Fixture, Target, true))
	{
		return false;
	}

	UBehaviorTree* BehaviorTree = BTTestLoadBehaviorTreeForTarget(Target);
	UBTCompositeNode* Root = BehaviorTree ? BehaviorTree->RootNode : nullptr;
	TestTrue(TEXT("ordinary edge decorators derive no authored logic program"), Root && Root->Children.Num() > 0 && Root->Children[0].DecoratorOps.Num() == 0);
	TestTrue(TEXT("ordinary root decorators derive no authored logic program"), BehaviorTree && BehaviorTree->RootDecoratorOps.Num() == 0);

	TSharedPtr<FJsonObject> LegacyLogicBody = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BehaviorTree->BlackboardAsset->GetPathName()),
		BTTestMakeDecoratorBehaviorTree(Target + TEXT("_Subtree"), true),
		MakeShared<FJsonObject>());
	BTTestGetObjectField(BTTestGetObjectField(LegacyLogicBody, TEXT("Tree")), TEXT("Root"))->SetArrayField(TEXT("DecoratorLogic"), {});
	FAssetDocumentValidateRequest Request;
	Request.Document = BTTestMakeBehaviorTreeDocument(Target, LegacyLogicBody);
	const FAssetDocumentResult Result = Service.Validate(Request);
	TestFalse(TEXT("runtime DecoratorLogic is not an authored graph-source field"), Result.IsSuccess());
	TestTrue(TEXT("legacy runtime logic field diagnostic is exact"), BTTestResultHasDiagnostic(Result, TEXT("UnknownField"), TEXT("/Body/Tree/Root/DecoratorLogic")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeDecoratorLogicRejectsInvalidShapeTest,
	"UEAssetDoc.BehaviorTree.DecoratorLogicRejectsInvalidShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeDecoratorLogicRejectsInvalidShapeTest::RunTest(const FString&)
{
	const FString Target = TEXT("/Game/AssetDocumentTests/BT_AD_InvalidDecoratorLogic");
	const FString BlackboardTarget = Target + TEXT("_BB");
	const FString SubtreeTarget = Target + TEXT("_Subtree");
	UBlackboardData* FixtureBlackboard = BTTestMakeDecoratorBlackboardAsset(BlackboardTarget);
	BTTestMakeExistingBehaviorTreeAsset(SubtreeTarget)->BlackboardAsset = FixtureBlackboard;

	const auto MakeBoundNode = [](const FString& Id, const FString& Kind)
	{
		TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("Id"), BTTestCanonicalGuid(TEXT("Bound:") + Id));
		Node->SetStringField(TEXT("Kind"), Kind);
		if (Kind == TEXT("Test"))
		{
			Node->SetStringField(TEXT("Class"), TEXT("/Script/AIModule.BTDecorator_Blackboard"));
			TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
			Properties->SetObjectField(TEXT("BlackboardKey"), BTTestMakeSelectorProperty(TEXT("TargetActor")));
			Node->SetObjectField(TEXT("Properties"), Properties);
		}
		return Node;
	};
	const auto MakeLink = [](const FString& From, const FString& To, int32 ToInput)
	{
		TSharedPtr<FJsonObject> Link = MakeShared<FJsonObject>();
		Link->SetStringField(TEXT("From"), BTTestCanonicalGuid(TEXT("Bound:") + From));
		Link->SetStringField(TEXT("To"), BTTestCanonicalGuid(TEXT("Bound:") + To));
		Link->SetNumberField(TEXT("ToInput"), ToInput);
		return BTTestMakeObjectValue(Link);
	};
	const auto MakeComposite = [](
		const TArray<TSharedPtr<FJsonValue>>& Nodes,
		const TArray<TSharedPtr<FJsonValue>>& Links)
	{
		TSharedPtr<FJsonObject> Composite = MakeShared<FJsonObject>();
		Composite->SetStringField(TEXT("Id"), BTTestCanonicalGuid(TEXT("Node:InvalidComposite")));
		Composite->SetStringField(TEXT("Kind"), TEXT("Composite"));
		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		Properties->SetStringField(TEXT("CompositeName"), TEXT("Invalid canonical expression"));
		Properties->SetBoolField(TEXT("bShowOperations"), true);
		Composite->SetObjectField(TEXT("Properties"), Properties);
		TSharedPtr<FJsonObject> BoundGraph = MakeShared<FJsonObject>();
		BoundGraph->SetStringField(TEXT("GraphGuid"), BTTestCanonicalGuid(TEXT("InvalidCompositeGraph")));
		BoundGraph->SetArrayField(TEXT("Nodes"), Nodes);
		BoundGraph->SetArrayField(TEXT("Links"), Links);
		Composite->SetObjectField(TEXT("BoundGraph"), BoundGraph);
		return Composite;
	};
	const auto SetFirstChildComposite = [](const TSharedPtr<FJsonObject>& Body, const TSharedPtr<FJsonObject>& Composite)
	{
		TSharedPtr<FJsonObject> Root = BTTestGetObjectField(BTTestGetObjectField(Body, TEXT("Tree")), TEXT("Root"));
		const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
		if (Root.IsValid() && Root->TryGetArrayField(TEXT("Children"), Children) && Children && Children->Num() > 0)
		{
			BTTestGetObjectFromValue((*Children)[0])->SetArrayField(TEXT("Decorators"), {BTTestMakeObjectValue(Composite)});
		}
	};

	TSharedPtr<FJsonObject> Body = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BTTestMakeObjectPathFromTarget(BlackboardTarget)),
		BTTestMakeDecoratorBehaviorTree(SubtreeTarget, true),
		MakeShared<FJsonObject>());
	SetFirstChildComposite(Body, MakeComposite(
		{
			BTTestMakeObjectValue(MakeBoundNode(TEXT("Sink"), TEXT("Sink"))),
			BTTestMakeObjectValue(MakeBoundNode(TEXT("And"), TEXT("And"))),
			BTTestMakeObjectValue(MakeBoundNode(TEXT("Test"), TEXT("Test"))),
		},
		{
			MakeLink(TEXT("Test"), TEXT("And"), 0),
			MakeLink(TEXT("And"), TEXT("Sink"), 0),
		}));
	FAssetDocumentValidateRequest AndRequest;
	AndRequest.Document = BTTestMakeBehaviorTreeDocument(Target, Body);
	const FAssetDocumentResult AndResult = FAssetDocumentModule::Get().GetService().Validate(AndRequest);
	TestFalse(TEXT("canonical And with only one input is rejected"), AndResult.IsSuccess());
	TestTrue(TEXT("invalid And arity reports exact BoundGraph node path"), BTTestResultHasDiagnostic(AndResult, TEXT("InvalidBehaviorTreeDecoratorArity"), TEXT("/Body/Tree/Root/Children/0/Decorators/0/BoundGraph/Nodes/1")));

	Body = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BTTestMakeObjectPathFromTarget(BlackboardTarget)),
		BTTestMakeDecoratorBehaviorTree(SubtreeTarget, true),
		MakeShared<FJsonObject>());
	TSharedPtr<FJsonValue> DanglingLink = MakeLink(TEXT("Test"), TEXT("Missing"), 0);
	SetFirstChildComposite(Body, MakeComposite(
		{
			BTTestMakeObjectValue(MakeBoundNode(TEXT("Sink"), TEXT("Sink"))),
			BTTestMakeObjectValue(MakeBoundNode(TEXT("Test"), TEXT("Test"))),
		},
		{DanglingLink}));
	FAssetDocumentValidateRequest TestRequest;
	TestRequest.Document = BTTestMakeBehaviorTreeDocument(Target, Body);
	const FAssetDocumentResult TestResult = FAssetDocumentModule::Get().GetService().Validate(TestRequest);
	TestFalse(TEXT("canonical dangling BoundGraph link is rejected"), TestResult.IsSuccess());
	TestTrue(TEXT("dangling BoundGraph link reports exact target path"), BTTestResultHasDiagnostic(TestResult, TEXT("DanglingBehaviorTreeDecoratorLink"), TEXT("/Body/Tree/Root/Children/0/Decorators/0/BoundGraph/Links/0/To")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeRejectsNodeLevelDecoratorsTest,
	"UEAssetDoc.BehaviorTree.RejectsNodeLevelDecorators",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeRejectsNodeLevelDecoratorsTest::RunTest(const FString&)
{
	const FString Target = TEXT("/Game/AssetDocumentTests/BT_AD_NodeLevelDecorators");
	const FString BlackboardTarget = Target + TEXT("_BB");
	const FString SubtreeTarget = Target + TEXT("_Subtree");
	UBlackboardData* FixtureBlackboard = BTTestMakeDecoratorBlackboardAsset(BlackboardTarget);
	BTTestMakeExistingBehaviorTreeAsset(SubtreeTarget)->BlackboardAsset = FixtureBlackboard;

	TSharedPtr<FJsonObject> Body = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BTTestMakeObjectPathFromTarget(BlackboardTarget)),
		BTTestMakeDecoratorBehaviorTree(SubtreeTarget, true),
		MakeShared<FJsonObject>());
	FAssetDocumentValidateRequest DecoratorsRequest;
	DecoratorsRequest.Document = BTTestMakeBehaviorTreeDocument(Target, Body);
	const FAssetDocumentResult DecoratorsResult = FAssetDocumentModule::Get().GetService().Validate(DecoratorsRequest);
	TestTrue(TEXT("canonical node-level Decorators are accepted"), DecoratorsResult.IsSuccess());

	TSharedPtr<FJsonObject> Root = BTTestGetObjectField(BTTestGetObjectField(Body, TEXT("Tree")), TEXT("Root"));
	Root->SetArrayField(TEXT("DecoratorLogic"), {});
	FAssetDocumentValidateRequest LogicRequest;
	LogicRequest.Document = BTTestMakeBehaviorTreeDocument(Target, Body);
	const FAssetDocumentResult LogicResult = FAssetDocumentModule::Get().GetService().Validate(LogicRequest);
	TestFalse(TEXT("legacy node-level DecoratorLogic is rejected instead of ignored"), LogicResult.IsSuccess());
	TestTrue(TEXT("legacy DecoratorLogic diagnostic is exact"), BTTestResultHasDiagnostic(LogicResult, TEXT("UnknownField"), TEXT("/Body/Tree/Root/DecoratorLogic")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeSemanticDiffPathsTest,
	"UEAssetDoc.BehaviorTree.SemanticDiffPaths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeSemanticDiffPathsTest::RunTest(const FString&)
{
	const FString Target = BTTestMakeUniqueTarget(TEXT("BT_AD_SemanticDiffPaths"));
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	FScopedBehaviorTreeMutationFixture Fixture;
	if (!BTTestApplyDecoratorTreeFixture(*this, Service, Fixture, Target, true))
	{
		return false;
	}

	UBehaviorTree* BehaviorTree = BTTestLoadBehaviorTreeForTarget(Target);
	TestNotNull(TEXT("BehaviorTree exists"), BehaviorTree);
	if (!BehaviorTree)
	{
		return false;
	}

	TSharedPtr<FJsonObject> DesiredTree = BTTestMakeDecoratorBehaviorTree(Target + TEXT("_Subtree"), true);
	TSharedPtr<FJsonObject> Root = BTTestGetObjectField(DesiredTree, TEXT("Root"));
	const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
	if (Root.IsValid() && Root->TryGetArrayField(TEXT("Children"), Children) && Children && Children->Num() > 0)
	{
		TSharedPtr<FJsonObject> FirstEdge = BTTestGetObjectFromValue((*Children)[0]);
		const TArray<TSharedPtr<FJsonValue>>* Decorators = nullptr;
		if (FirstEdge.IsValid() && FirstEdge->TryGetArrayField(TEXT("Decorators"), Decorators) && Decorators && Decorators->Num() > 0)
		{
			TSharedPtr<FJsonObject> Decorator = BTTestGetObjectFromValue((*Decorators)[0]);
			TSharedPtr<FJsonObject> Properties = BTTestGetObjectField(Decorator, TEXT("Properties"));
			TSharedPtr<FJsonObject> BlackboardKey = BTTestGetObjectField(Properties, TEXT("BlackboardKey"));
			if (BlackboardKey.IsValid())
			{
				BlackboardKey->SetStringField(TEXT("Key"), TEXT("OtherTargetActor"));
			}
		}
	}

	TSharedPtr<FJsonObject> DesiredBody = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BehaviorTree->BlackboardAsset->GetPathName()),
		DesiredTree,
		MakeShared<FJsonObject>());
	const FAssetDocumentResult DiffResult = Service.Diff(BTTestMakeDiffRequest(BTTestMakeBehaviorTreeDocument(Target, DesiredBody)));
	TestTrue(TEXT("semantic diff succeeds"), DiffResult.IsSuccess());
	const TSharedPtr<FJsonObject> DesiredDocumentTree = BTTestGetObjectField(DesiredBody, TEXT("Tree"));
	const FString DecoratorPropertyPath = BTTestTreePointer(DesiredDocumentTree, BTTestCanonicalGuid(TEXT("Node:HasTarget")))
		+ TEXT("/Properties/BlackboardKey/Key");
	const FString RootNodePath = BTTestTreePointer(DesiredDocumentTree, BTTestCanonicalGuid(TEXT("Node:RootSelector")));
	const FString MoveNodePath = BTTestTreePointer(DesiredDocumentTree, BTTestCanonicalGuid(TEXT("Node:MoveToTarget")));
	TestEqual(TEXT("the root node's document pointer"), RootNodePath, FString(TEXT("/Body/Tree/Root")));
	TestTrue(TEXT("edge decorator change path is identity-addressed and precise"), BTTestDiffPayloadHasEntry(DiffResult.Payload, TEXT("changed"), DecoratorPropertyPath));
	TestFalse(TEXT("edge decorator property change does not dirty parent node path"), BTTestDiffPayloadHasEntry(DiffResult.Payload, TEXT("changed"), RootNodePath));
	TestFalse(TEXT("edge decorator property change does not dirty child node path"), BTTestDiffPayloadHasEntry(DiffResult.Payload, TEXT("changed"), MoveNodePath));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeOrderDiffPathsTest,
	"UEAssetDoc.BehaviorTree.OrderDiffPaths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeOrderDiffPathsTest::RunTest(const FString&)
{
	const FString Target = BTTestMakeUniqueTarget(TEXT("BT_AD_OrderDiffPaths"));
	const FString BlackboardTarget = Target + TEXT("_BB");
	const FString SubtreeTarget = Target + TEXT("_Subtree");
	FScopedBehaviorTreeMutationFixture Fixture;
	if (!Fixture.ReserveTarget(BlackboardTarget) || !Fixture.ReserveTarget(SubtreeTarget))
	{
		return false;
	}
	UBlackboardData* FixtureBlackboard = BTTestMakeDecoratorBlackboardAsset(BlackboardTarget);
	BTTestMakeExistingBehaviorTreeAsset(SubtreeTarget)->BlackboardAsset = FixtureBlackboard;

	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	TSharedPtr<FJsonObject> AppliedTree = BTTestMakeDecoratorBehaviorTree(SubtreeTarget, true);
	BTTestAddSecondDecoratorSet(AppliedTree);
	TSharedPtr<FJsonObject> Body = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BTTestMakeObjectPathFromTarget(BlackboardTarget)),
		AppliedTree,
		MakeShared<FJsonObject>());
	const FAssetDocumentResult ApplyResult = Fixture.Apply(Service, Target, BTTestMakeBehaviorTreeDocument(Target, Body));
	TestTrue(TEXT("order fixture apply succeeds"), ApplyResult.IsSuccess());
	if (!ApplyResult.IsSuccess())
	{
		AddError(ApplyResult.Message);
		return false;
	}

	TSharedPtr<FJsonObject> DesiredTree = BTTestMakeDecoratorBehaviorTree(SubtreeTarget, true);
	BTTestAddSecondDecoratorSet(DesiredTree);
	TSharedPtr<FJsonObject> Root = BTTestGetObjectField(DesiredTree, TEXT("Root"));
	BTTestSwapArrayEntries(Root, TEXT("Children"), 0, 1);
	if (Root.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
		if (Root->TryGetArrayField(TEXT("Children"), Children) && Children && Children->Num() > 1)
		{
			TSharedPtr<FJsonObject> MoveEdge = BTTestGetObjectFromValue((*Children)[1]);
			BTTestSwapArrayEntries(MoveEdge, TEXT("Decorators"), 0, 1);
		}
	}
	BTTestSwapArrayEntries(DesiredTree, TEXT("RootDecorators"), 0, 1);

	TSharedPtr<FJsonObject> DesiredBody = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BTTestMakeObjectPathFromTarget(BlackboardTarget)),
		DesiredTree,
		MakeShared<FJsonObject>());
	const FAssetDocumentResult DiffResult = Service.Diff(BTTestMakeDiffRequest(BTTestMakeBehaviorTreeDocument(Target, DesiredBody)));
	TestTrue(TEXT("order diff succeeds"), DiffResult.IsSuccess());
	const TSharedPtr<FJsonObject> DesiredDocumentTree = BTTestGetObjectField(DesiredBody, TEXT("Tree"));
	const FString RootNodePath = BTTestTreePointer(DesiredDocumentTree, BTTestCanonicalGuid(TEXT("Node:RootSelector")));
	const FString MoveNodePath = BTTestTreePointer(DesiredDocumentTree, BTTestCanonicalGuid(TEXT("Node:MoveToTarget")));
	TestTrue(TEXT("child order change is reported by parent NodeGuid"), BTTestDiffPayloadHasEntry(DiffResult.Payload, TEXT("changed"), RootNodePath + TEXT("/Children")));
	TestTrue(TEXT("edge decorator order change is reported by child NodeGuid"), BTTestDiffPayloadHasEntry(DiffResult.Payload, TEXT("changed"), MoveNodePath + TEXT("/Decorators")));
	TestTrue(TEXT("root decorator order change is reported on the root node"), BTTestDiffPayloadHasEntry(DiffResult.Payload, TEXT("changed"), RootNodePath + TEXT("/Decorators")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeExtractsUniqueIdsForDuplicateDisplayNamesTest,
	"UEAssetDoc.BehaviorTree.ExtractsUniqueIdsForDuplicateDisplayNames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeExtractsUniqueIdsForDuplicateDisplayNamesTest::RunTest(const FString&)
{
	const FString Target = BTTestMakeUniqueTarget(TEXT("BT_AD_DuplicateDisplayNames"));
	const FString BlackboardTarget = Target + TEXT("_BB");
	const FString SubtreeTarget = Target + TEXT("_Subtree");
	FScopedBehaviorTreeMutationFixture Fixture;
	if (!Fixture.ReserveTarget(BlackboardTarget) || !Fixture.ReserveTarget(SubtreeTarget))
	{
		return false;
	}
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	UBlackboardData* Blackboard = BTTestMakeDecoratorBlackboardAsset(BlackboardTarget);
	UBehaviorTree* Subtree = BTTestMakeExistingBehaviorTreeAsset(SubtreeTarget);
	Subtree->BlackboardAsset = Blackboard;
	TSharedPtr<FJsonObject> LegacyTree = BTTestMakeDecoratorBehaviorTree(SubtreeTarget, true);
	BTTestAddSecondDecoratorSet(LegacyTree);
	TSharedPtr<FJsonObject> LegacyRoot = BTTestGetObjectField(LegacyTree, TEXT("Root"));
	const TArray<TSharedPtr<FJsonValue>>* Services = nullptr;
	TArray<TSharedPtr<FJsonValue>> ExpandedServices;
	if (LegacyRoot.IsValid() && LegacyRoot->TryGetArrayField(TEXT("Services"), Services) && Services)
	{
		ExpandedServices = *Services;
	}
	TSharedPtr<FJsonObject> SecondServiceProperties = MakeShared<FJsonObject>();
	SecondServiceProperties->SetStringField(TEXT("NodeName"), TEXT("Duplicate"));
	SecondServiceProperties->SetObjectField(TEXT("BlackboardKey"), BTTestMakeSelectorProperty(TEXT("OtherTargetActor")));
	ExpandedServices.Add(BTTestMakeObjectValue(BTTestMakeBtNode(
		TEXT("SecondFocusService"),
		TEXT("/Script/AIModule.BTService_DefaultFocus"),
		SecondServiceProperties)));
	LegacyRoot->SetArrayField(TEXT("Services"), MoveTemp(ExpandedServices));

	TSharedPtr<FJsonObject> Body = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(Blackboard->GetPathName()),
		LegacyTree,
		MakeShared<FJsonObject>());
	TSharedPtr<FJsonObject> CanonicalRoot = BTTestGetObjectField(BTTestGetObjectField(Body, TEXT("Tree")), TEXT("Root"));
	TFunction<void(const TSharedPtr<FJsonObject>&)> SetDuplicateLabels;
	SetDuplicateLabels = [&SetDuplicateLabels](const TSharedPtr<FJsonObject>& Node)
	{
		if (!Node.IsValid())
		{
			return;
		}
		BTTestGetObjectField(Node, TEXT("Properties"))->SetStringField(TEXT("NodeName"), TEXT("Duplicate"));
		for (const FString& AttachmentField : {FString(TEXT("Decorators")), FString(TEXT("Services"))})
		{
			const TArray<TSharedPtr<FJsonValue>>* Attachments = nullptr;
			if (Node->TryGetArrayField(AttachmentField, Attachments) && Attachments)
			{
				for (const TSharedPtr<FJsonValue>& Attachment : *Attachments)
				{
					BTTestGetObjectField(BTTestGetObjectFromValue(Attachment), TEXT("Properties"))->SetStringField(TEXT("NodeName"), TEXT("Duplicate"));
				}
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
		if (Node->TryGetArrayField(TEXT("Children"), Children) && Children)
		{
			for (const TSharedPtr<FJsonValue>& Child : *Children)
			{
				SetDuplicateLabels(BTTestGetObjectFromValue(Child));
			}
		}
	};
	SetDuplicateLabels(CanonicalRoot);
	const FAssetDocumentResult ApplyResult = Fixture.Apply(Service, Target, BTTestMakeBehaviorTreeDocument(Target, Body));
	TestTrue(TEXT("duplicate display-name canonical graph applies"), ApplyResult.IsSuccess());
	if (!ApplyResult.IsSuccess())
	{
		AddError(ApplyResult.Message);
		return false;
	}
	UBehaviorTree* BehaviorTree = BTTestLoadBehaviorTreeForTarget(Target);

	FAssetDocumentExtractRequest ExtractRequest;
	ExtractRequest.AssetPath = Target;
	ExtractRequest.bDiffOnly = true;
	const FAssetDocumentResult FirstExtract = Service.Extract(ExtractRequest);
	TestTrue(TEXT("duplicate display name extract succeeds"), FirstExtract.IsSuccess());
	const FAssetDocumentResult SecondExtract = Service.Extract(ExtractRequest);
	TestTrue(TEXT("duplicate display name extract is repeatable"), SecondExtract.IsSuccess());
	if (!FirstExtract.IsSuccess() || !SecondExtract.IsSuccess())
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* FirstBody = nullptr;
	const TSharedPtr<FJsonObject>* FirstTree = nullptr;
	TestTrue(TEXT("first extract contains tree"), FirstExtract.Payload->TryGetObjectField(TEXT("Body"), FirstBody) && FirstBody && (*FirstBody)->TryGetObjectField(TEXT("Tree"), FirstTree));
	TArray<FString> FirstIds;
	TestTrue(TEXT("all extracted ids are unique"), BTTestCollectAllTreeIds(FirstTree ? *FirstTree : nullptr, FirstIds));
	TestTrue(TEXT("duplicate display names produce multiple ids"), FirstIds.Num() >= 9);

	const TSharedPtr<FJsonObject>* SecondBody = nullptr;
	const TSharedPtr<FJsonObject>* SecondTree = nullptr;
	TestTrue(TEXT("second extract contains tree"), SecondExtract.Payload->TryGetObjectField(TEXT("Body"), SecondBody) && SecondBody && (*SecondBody)->TryGetObjectField(TEXT("Tree"), SecondTree));
	const FString FirstTreeJson = FAssetDocumentJsonRegionUtils::JsonValueToComparableString(MakeShared<FJsonValueObject>(*FirstTree));
	const FString SecondTreeJson = FAssetDocumentJsonRegionUtils::JsonValueToComparableString(MakeShared<FJsonValueObject>(*SecondTree));
	TestEqual(TEXT("duplicate display name ids are stable across extracts"), SecondTreeJson, FirstTreeJson);

	TSharedPtr<FJsonObject> DiffBody = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BehaviorTree->BlackboardAsset->GetPathName()),
		*FirstTree,
		MakeShared<FJsonObject>());
	const FAssetDocumentResult DiffResult = Service.Diff(BTTestMakeDiffRequest(BTTestMakeBehaviorTreeDocument(Target, DiffBody)));
	TestTrue(TEXT("diff with extracted duplicate-display tree succeeds"), DiffResult.IsSuccess());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeEditorLayoutTest,
	"UEAssetDoc.BehaviorTree.EditorLayout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeEditorLayoutTest::RunTest(const FString&)
{
	const FString Target = BTTestMakeUniqueTarget(TEXT("BT_AD_EditorLayout"));
	const FString BlackboardTarget = Target + TEXT("_BB");
	const FString SubtreeTarget = Target + TEXT("_Subtree");
	FScopedBehaviorTreeMutationFixture Fixture;
	if (!Fixture.ReserveTarget(BlackboardTarget) || !Fixture.ReserveTarget(SubtreeTarget))
	{
		return false;
	}
	UBlackboardData* FixtureBlackboard = BTTestMakeDecoratorBlackboardAsset(BlackboardTarget);
	BTTestMakeExistingBehaviorTreeAsset(SubtreeTarget)->BlackboardAsset = FixtureBlackboard;

	const FString CommentId = TEXT("11111111-2222-3333-4444-555555555555");
	TSharedPtr<FJsonObject> Layout = BTTestMakeEditorLayout(
		{
			BTTestMakeObjectValue(BTTestMakeEditorLayoutNode(TEXT("RootSelector"), 100, 200)),
			BTTestMakeObjectValue(BTTestMakeEditorLayoutNode(TEXT("MoveToTarget"), 300, 520)),
		},
		{
			BTTestMakeObjectValue(BTTestMakeEditorLayoutComment(CommentId, TEXT("Primary movement branch"), -40, 80, 640, 220)),
		});

	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	TSharedPtr<FJsonObject> Body = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BTTestMakeObjectPathFromTarget(BlackboardTarget)),
		BTTestMakeDecoratorBehaviorTree(SubtreeTarget, true),
		Layout);
	const FAssetDocumentResult ApplyResult = Fixture.Apply(Service, Target, BTTestMakeBehaviorTreeDocument(Target, Body));
	TestTrue(TEXT("semantic tree and editor layout apply succeeds"), ApplyResult.IsSuccess());
	if (!ApplyResult.IsSuccess())
	{
		AddError(ApplyResult.Message);
		return false;
	}

	UBehaviorTree* BehaviorTree = BTTestLoadBehaviorTreeForTarget(Target);
	UBehaviorTreeGraph* Graph = BehaviorTree ? Cast<UBehaviorTreeGraph>(BehaviorTree->BTGraph) : nullptr;
	TestNotNull(TEXT("BTGraph exists after layout apply"), Graph);
	if (!Graph)
	{
		return false;
	}

	bool bFoundRoot = false;
	bool bFoundMoveTo = false;
	bool bFoundComment = false;
	for (UEdGraphNode* GraphNode : Graph->Nodes)
	{
		if (UBehaviorTreeGraphNode* BTGraphNode = Cast<UBehaviorTreeGraphNode>(GraphNode))
		{
			const UBTNode* NodeInstance = Cast<UBTNode>(BTGraphNode->NodeInstance);
			if (NodeInstance && NodeInstance->NodeName == TEXT("RootSelector"))
			{
				bFoundRoot = true;
				TestEqual(TEXT("root X applied to graph node"), BTGraphNode->NodePosX, 100);
				TestEqual(TEXT("root Y applied to graph node"), BTGraphNode->NodePosY, 200);
			}
			if (NodeInstance && NodeInstance->NodeName == TEXT("MoveToTarget"))
			{
				bFoundMoveTo = true;
				TestEqual(TEXT("MoveTo X applied to graph node"), BTGraphNode->NodePosX, 300);
				TestEqual(TEXT("MoveTo Y applied to graph node"), BTGraphNode->NodePosY, 520);
			}
		}
		else if (UEdGraphNode_Comment* CommentNode = Cast<UEdGraphNode_Comment>(GraphNode))
		{
			bFoundComment = CommentNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens).Equals(CommentId, ESearchCase::IgnoreCase);
			if (bFoundComment)
			{
				TestEqual(TEXT("comment text applied"), CommentNode->NodeComment, FString(TEXT("Primary movement branch")));
				TestEqual(TEXT("comment X applied"), CommentNode->NodePosX, -40);
				TestEqual(TEXT("comment Y applied"), CommentNode->NodePosY, 80);
				TestEqual(TEXT("comment width applied"), CommentNode->NodeWidth, 640);
				TestEqual(TEXT("comment height applied"), CommentNode->NodeHeight, 220);
			}
		}
	}
	TestTrue(TEXT("RootSelector graph node found"), bFoundRoot);
	TestTrue(TEXT("MoveToTarget graph node found"), bFoundMoveTo);
	TestTrue(TEXT("layout comment graph node found"), bFoundComment);

	FAssetDocumentExtractRequest ExtractRequest;
	ExtractRequest.AssetPath = Target;
	ExtractRequest.bDiffOnly = true;
	const FAssetDocumentResult ExtractResult = Service.Extract(ExtractRequest);
	TestTrue(TEXT("editor layout extract succeeds"), ExtractResult.IsSuccess());

	const TSharedPtr<FJsonObject>* ExtractedBody = nullptr;
	const TSharedPtr<FJsonObject>* ExtractedTree = nullptr;
	TestTrue(TEXT("extract payload contains graph-source Body.Tree"), ExtractResult.Payload.IsValid()
		&& ExtractResult.Payload->TryGetObjectField(TEXT("Body"), ExtractedBody)
		&& ExtractedBody && (*ExtractedBody)->TryGetObjectField(TEXT("Tree"), ExtractedTree));
	if (ExtractedTree && ExtractedTree->IsValid())
	{
		TSharedPtr<FJsonObject> ExtractedRoot = BTTestGetObjectField(*ExtractedTree, TEXT("Root"));
		const TArray<TSharedPtr<FJsonValue>>* ExtractedComments = nullptr;
		TestEqual(TEXT("root editor X extracts inline"), BTTestGetObjectField(BTTestGetObjectField(ExtractedRoot, TEXT("Editor")), TEXT("Position"))->GetNumberField(TEXT("X")), 100.0);
		TestEqual(TEXT("root editor Y extracts inline"), BTTestGetObjectField(BTTestGetObjectField(ExtractedRoot, TEXT("Editor")), TEXT("Position"))->GetNumberField(TEXT("Y")), 200.0);
		TestTrue(TEXT("tree comments extract with graph source"), (*ExtractedTree)->TryGetArrayField(TEXT("Comments"), ExtractedComments) && ExtractedComments && ExtractedComments->Num() == 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeEditorLayoutSparsePreservesCommentsTest,
	"UEAssetDoc.BehaviorTree.EditorLayoutSparsePreservesComments",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeEditorLayoutSparsePreservesCommentsTest::RunTest(const FString&)
{
	const FString Target = BTTestMakeUniqueTarget(TEXT("BT_AD_SparseLayoutComments"));
	const FString BlackboardTarget = Target + TEXT("_BB");
	const FString SubtreeTarget = Target + TEXT("_Subtree");
	FScopedBehaviorTreeMutationFixture Fixture;
	if (!Fixture.ReserveTarget(BlackboardTarget) || !Fixture.ReserveTarget(SubtreeTarget))
	{
		return false;
	}
	UBlackboardData* FixtureBlackboard = BTTestMakeDecoratorBlackboardAsset(BlackboardTarget);
	BTTestMakeExistingBehaviorTreeAsset(SubtreeTarget)->BlackboardAsset = FixtureBlackboard;

	const FString CommentId = TEXT("22222222-3333-4444-5555-666666666666");
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	TSharedPtr<FJsonObject> InitialBody = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BTTestMakeObjectPathFromTarget(BlackboardTarget)),
		BTTestMakeDecoratorBehaviorTree(SubtreeTarget, true),
		BTTestMakeEditorLayout(
			{
				BTTestMakeObjectValue(BTTestMakeEditorLayoutNode(TEXT("RootSelector"), 100, 200)),
				BTTestMakeObjectValue(BTTestMakeEditorLayoutNode(TEXT("MoveToTarget"), 300, 520)),
			},
			{
				BTTestMakeObjectValue(BTTestMakeEditorLayoutComment(CommentId, TEXT("Persist me"), -80, 40, 420, 160)),
			}));
	const FAssetDocumentResult InitialApply = Fixture.Apply(Service, Target, BTTestMakeBehaviorTreeDocument(Target, InitialBody));
	TestTrue(TEXT("initial editor layout with comment applies"), InitialApply.IsSuccess());
	if (!InitialApply.IsSuccess())
	{
		AddError(InitialApply.Message);
		return false;
	}

	TSharedPtr<FJsonObject> SparseBody = MakeShared<FJsonObject>();
	const FAssetDocumentResult SparseApply = Fixture.Apply(Service, Target, BTTestMakeBehaviorTreeDocument(Target, SparseBody));
	TestTrue(TEXT("omitted canonical Tree region apply succeeds"), SparseApply.IsSuccess());
	if (!SparseApply.IsSuccess())
	{
		AddError(SparseApply.Message);
		return false;
	}

	UBehaviorTree* BehaviorTree = BTTestLoadBehaviorTreeForTarget(Target);
	UBehaviorTreeGraph* Graph = BehaviorTree ? Cast<UBehaviorTreeGraph>(BehaviorTree->BTGraph) : nullptr;
	UEdGraphNode_Comment* Comment = BTTestFindEditorLayoutComment(Graph, CommentId);
	TestNotNull(TEXT("omitted Tree preserves existing graph comments"), Comment);
	if (Comment)
	{
		TestEqual(TEXT("preserved comment text"), Comment->NodeComment, FString(TEXT("Persist me")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeEditorLayoutExplicitEmptyCommentsDeletesTest,
	"UEAssetDoc.BehaviorTree.EditorLayoutExplicitEmptyCommentsDeletes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeEditorLayoutExplicitEmptyCommentsDeletesTest::RunTest(const FString&)
{
	const FString Target = BTTestMakeUniqueTarget(TEXT("BT_AD_DeleteLayoutComments"));
	const FString BlackboardTarget = Target + TEXT("_BB");
	const FString SubtreeTarget = Target + TEXT("_Subtree");
	FScopedBehaviorTreeMutationFixture Fixture;
	if (!Fixture.ReserveTarget(BlackboardTarget) || !Fixture.ReserveTarget(SubtreeTarget))
	{
		return false;
	}
	UBlackboardData* FixtureBlackboard = BTTestMakeDecoratorBlackboardAsset(BlackboardTarget);
	BTTestMakeExistingBehaviorTreeAsset(SubtreeTarget)->BlackboardAsset = FixtureBlackboard;

	const FString CommentId = TEXT("44444444-5555-6666-7777-888888888888");
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	TSharedPtr<FJsonObject> InitialBody = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BTTestMakeObjectPathFromTarget(BlackboardTarget)),
		BTTestMakeDecoratorBehaviorTree(SubtreeTarget, true),
		BTTestMakeEditorLayout(
			{
				BTTestMakeObjectValue(BTTestMakeEditorLayoutNode(TEXT("RootSelector"), 100, 200)),
				BTTestMakeObjectValue(BTTestMakeEditorLayoutNode(TEXT("MoveToTarget"), 300, 520)),
			},
			{
				BTTestMakeObjectValue(BTTestMakeEditorLayoutComment(CommentId, TEXT("Delete me"), -40, 80, 640, 220)),
			}));
	const FAssetDocumentResult InitialApply = Fixture.Apply(Service, Target, BTTestMakeBehaviorTreeDocument(Target, InitialBody));
	TestTrue(TEXT("initial editor layout comment applies before explicit delete"), InitialApply.IsSuccess());
	if (!InitialApply.IsSuccess())
	{
		AddError(InitialApply.Message);
		return false;
	}

	TSharedPtr<FJsonObject> DeleteTree = BTTestGetObjectField(InitialBody, TEXT("Tree"));
	DeleteTree->SetArrayField(TEXT("Comments"), TArray<TSharedPtr<FJsonValue>>());
	TSharedPtr<FJsonObject> DeleteBody = MakeShared<FJsonObject>();
	DeleteBody->SetObjectField(TEXT("Tree"), DeleteTree);
	const FAssetDocumentResult DeleteApply = Fixture.Apply(Service, Target, BTTestMakeBehaviorTreeDocument(Target, DeleteBody));
	TestTrue(TEXT("explicit empty Comments apply succeeds"), DeleteApply.IsSuccess());
	if (!DeleteApply.IsSuccess())
	{
		AddError(DeleteApply.Message);
		return false;
	}

	FAssetDocumentExtractRequest ExtractRequest;
	ExtractRequest.AssetPath = Target;
	ExtractRequest.bDiffOnly = true;
	const FAssetDocumentResult ExtractResult = Service.Extract(ExtractRequest);
	TestTrue(TEXT("extract after explicit comment delete succeeds"), ExtractResult.IsSuccess());

	const TSharedPtr<FJsonObject>* ExtractedBody = nullptr;
	const TSharedPtr<FJsonObject>* ExtractedTree = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* ExtractedComments = nullptr;
	TestTrue(TEXT("extract contains empty Body.Tree.Comments"), ExtractResult.Payload.IsValid()
		&& ExtractResult.Payload->TryGetObjectField(TEXT("Body"), ExtractedBody)
		&& ExtractedBody
		&& (*ExtractedBody)->TryGetObjectField(TEXT("Tree"), ExtractedTree)
		&& ExtractedTree
		&& (*ExtractedTree)->TryGetArrayField(TEXT("Comments"), ExtractedComments)
		&& ExtractedComments
		&& ExtractedComments->Num() == 0);

	UBehaviorTree* BehaviorTree = BTTestLoadBehaviorTreeForTarget(Target);
	UBehaviorTreeGraph* Graph = BehaviorTree ? Cast<UBehaviorTreeGraph>(BehaviorTree->BTGraph) : nullptr;
	TestNull(TEXT("explicit empty Comments removes existing comment graph node"), BTTestFindEditorLayoutComment(Graph, CommentId));

	const FAssetDocumentResult DiffResult = Service.Diff(BTTestMakeDiffRequest(BTTestMakeBehaviorTreeDocument(Target, DeleteBody)));
	TestTrue(TEXT("diff after explicit empty Comments succeeds"), DiffResult.IsSuccess());
	if (!DiffResult.IsSuccess())
	{
		AddError(DiffResult.Message);
		return false;
	}
	TestFalse(TEXT("explicit comment deletion does not emit changed semantic tree entries"), BTTestDiffPayloadHasChangedPathPrefix(DiffResult.Payload, TEXT("/Body/Tree")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeEditorLayoutCommentColorOptionalDiffTest,
	"UEAssetDoc.BehaviorTree.EditorLayoutCommentColorOptionalDiff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeEditorLayoutCommentColorOptionalDiffTest::RunTest(const FString&)
{
	const FString Target = BTTestMakeUniqueTarget(TEXT("BT_AD_OptionalCommentColor"));
	const FString BlackboardTarget = Target + TEXT("_BB");
	const FString SubtreeTarget = Target + TEXT("_Subtree");
	FScopedBehaviorTreeMutationFixture Fixture;
	if (!Fixture.ReserveTarget(BlackboardTarget) || !Fixture.ReserveTarget(SubtreeTarget))
	{
		return false;
	}
	UBlackboardData* FixtureBlackboard = BTTestMakeDecoratorBlackboardAsset(BlackboardTarget);
	BTTestMakeExistingBehaviorTreeAsset(SubtreeTarget)->BlackboardAsset = FixtureBlackboard;

	const FString CommentId = TEXT("33333333-4444-5555-6666-777777777777");
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	TSharedPtr<FJsonObject> Tree = BTTestMakeDecoratorBehaviorTree(SubtreeTarget, true);
	TSharedPtr<FJsonObject> LayoutWithoutColor = BTTestMakeEditorLayout(
		{
			BTTestMakeObjectValue(BTTestMakeEditorLayoutNode(TEXT("RootSelector"), 100, 200)),
			BTTestMakeObjectValue(BTTestMakeEditorLayoutNode(TEXT("MoveToTarget"), 300, 520)),
		},
		{
			BTTestMakeObjectValue(BTTestMakeEditorLayoutComment(CommentId, TEXT("No authored color"), -120, 60, 500, 180, false)),
		});
	TSharedPtr<FJsonObject> Body = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BTTestMakeObjectPathFromTarget(BlackboardTarget)),
		Tree,
		LayoutWithoutColor);
	const FAssetDocumentResult ApplyResult = Fixture.Apply(Service, Target, BTTestMakeBehaviorTreeDocument(Target, Body));
	TestTrue(TEXT("editor layout comment without Color applies"), ApplyResult.IsSuccess());
	if (!ApplyResult.IsSuccess())
	{
		AddError(ApplyResult.Message);
		return false;
	}

	TSharedPtr<FJsonObject> DesiredBody = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BTTestMakeObjectPathFromTarget(BlackboardTarget)),
		BTTestMakeDecoratorBehaviorTree(SubtreeTarget, true),
		LayoutWithoutColor);
	const FAssetDocumentResult DiffResult = Service.Diff(BTTestMakeDiffRequest(BTTestMakeBehaviorTreeDocument(Target, DesiredBody)));
	TestTrue(TEXT("diff for comment without Color succeeds"), DiffResult.IsSuccess());
	if (!DiffResult.IsSuccess())
	{
		AddError(DiffResult.Message);
		return false;
	}

	FGuid ParsedCommentGuid;
	FGuid::Parse(CommentId, ParsedCommentGuid);
	const FString CommentPath = BTTestTreePointer(BTTestGetObjectField(DesiredBody, TEXT("Tree")), ParsedCommentGuid.ToString(EGuidFormats::Digits));
	TestFalse(TEXT("omitted Color does not keep comment changed"), BTTestDiffPayloadHasChangedPathPrefix(DiffResult.Payload, CommentPath));
	TestFalse(TEXT("optional comment Color produces a canonical no-op Tree diff"), BTTestDiffPayloadHasChangedPathPrefix(DiffResult.Payload, TEXT("/Body/Tree")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeEditorLayoutRejectsDanglingNodeTest,
	"UEAssetDoc.BehaviorTree.EditorLayoutRejectsDanglingNode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeEditorLayoutRejectsDanglingNodeTest::RunTest(const FString&)
{
	const FString Target = BTTestMakeUniqueTarget(TEXT("BT_AD_DanglingLayout"));
	const FString BlackboardTarget = Target + TEXT("_BB");
	const FString SubtreeTarget = Target + TEXT("_Subtree");
	FScopedBehaviorTreeMutationFixture Fixture;
	if (!Fixture.ReserveTarget(BlackboardTarget) || !Fixture.ReserveTarget(SubtreeTarget))
	{
		return false;
	}
	UBlackboardData* FixtureBlackboard = BTTestMakeDecoratorBlackboardAsset(BlackboardTarget);
	BTTestMakeExistingBehaviorTreeAsset(SubtreeTarget)->BlackboardAsset = FixtureBlackboard;

	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	TSharedPtr<FJsonObject> Body = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BTTestMakeObjectPathFromTarget(BlackboardTarget)),
		BTTestMakeDecoratorBehaviorTree(SubtreeTarget, true),
		MakeShared<FJsonObject>());
	TSharedPtr<FJsonObject> Root = BTTestGetObjectField(BTTestGetObjectField(Body, TEXT("Tree")), TEXT("Root"));
	BTTestGetObjectField(BTTestGetObjectField(Root, TEXT("Editor")), TEXT("Position"))->SetNumberField(TEXT("X"), 10.5);
	const FAssetDocumentResult ApplyResult = Fixture.Apply(Service, Target, BTTestMakeBehaviorTreeDocument(Target, Body));
	TestFalse(TEXT("non-integral inline editor coordinate is rejected"), ApplyResult.IsSuccess());
	TestTrue(TEXT("inline editor coordinate diagnostic is exact"), BTTestResultHasDiagnostic(ApplyResult, TEXT("InvalidBehaviorTreeCoordinate"), TEXT("/Body/Tree/Root/Editor/Position/X")));
	TestNull(
		TEXT("failed graph-source editor preflight does not create target BehaviorTree"),
		FindObject<UBehaviorTree>(nullptr, *BTTestMakeObjectPathFromTarget(Target)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeEditorLayoutOnlyDiffTest,
	"UEAssetDoc.BehaviorTree.EditorLayoutOnlyDiff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeEditorLayoutOnlyDiffTest::RunTest(const FString&)
{
	const FString Target = BTTestMakeUniqueTarget(TEXT("BT_AD_LayoutOnlyDiff"));
	const FString BlackboardTarget = Target + TEXT("_BB");
	const FString SubtreeTarget = Target + TEXT("_Subtree");
	FScopedBehaviorTreeMutationFixture Fixture;
	if (!Fixture.ReserveTarget(BlackboardTarget) || !Fixture.ReserveTarget(SubtreeTarget))
	{
		return false;
	}
	UBlackboardData* FixtureBlackboard = BTTestMakeDecoratorBlackboardAsset(BlackboardTarget);
	BTTestMakeExistingBehaviorTreeAsset(SubtreeTarget)->BlackboardAsset = FixtureBlackboard;

	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	TSharedPtr<FJsonObject> AppliedTree = BTTestMakeDecoratorBehaviorTree(SubtreeTarget, true);
	TSharedPtr<FJsonObject> AppliedLayout = BTTestMakeEditorLayout(
		{
			BTTestMakeObjectValue(BTTestMakeEditorLayoutNode(TEXT("RootSelector"), 100, 200)),
			BTTestMakeObjectValue(BTTestMakeEditorLayoutNode(TEXT("MoveToTarget"), 300, 520)),
		});
	TSharedPtr<FJsonObject> AppliedBody = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BTTestMakeObjectPathFromTarget(BlackboardTarget)),
		AppliedTree,
		AppliedLayout);
	const FAssetDocumentResult ApplyResult = Fixture.Apply(Service, Target, BTTestMakeBehaviorTreeDocument(Target, AppliedBody));
	TestTrue(TEXT("layout diff fixture apply succeeds"), ApplyResult.IsSuccess());
	if (!ApplyResult.IsSuccess())
	{
		AddError(ApplyResult.Message);
		return false;
	}

	TSharedPtr<FJsonObject> DesiredTree = BTTestMakeDecoratorBehaviorTree(SubtreeTarget, true);
	TSharedPtr<FJsonObject> DesiredLayout = BTTestMakeEditorLayout(
		{
			BTTestMakeObjectValue(BTTestMakeEditorLayoutNode(TEXT("RootSelector"), 180, 260)),
			BTTestMakeObjectValue(BTTestMakeEditorLayoutNode(TEXT("MoveToTarget"), 300, 520)),
		});
	TSharedPtr<FJsonObject> DesiredBody = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BTTestMakeObjectPathFromTarget(BlackboardTarget)),
		DesiredTree,
		DesiredLayout);
	const FAssetDocumentResult DiffResult = Service.Diff(BTTestMakeDiffRequest(BTTestMakeBehaviorTreeDocument(Target, DesiredBody)));
	TestTrue(TEXT("layout-only diff succeeds"), DiffResult.IsSuccess());
	const FString RootEditorPath = BTTestTreePointer(BTTestGetObjectField(DesiredBody, TEXT("Tree")),
		BTTestCanonicalGuid(TEXT("Node:RootSelector"))) + TEXT("/Editor/Position");
	TestTrue(TEXT("inline root editor X path changed"), BTTestDiffPayloadHasEntry(DiffResult.Payload, TEXT("changed"), RootEditorPath + TEXT("/X")));
	TestTrue(TEXT("inline root editor Y path changed"), BTTestDiffPayloadHasEntry(DiffResult.Payload, TEXT("changed"), RootEditorPath + TEXT("/Y")));
	const TArray<TSharedPtr<FJsonValue>>* ChangedEntries = nullptr;
	TestTrue(TEXT("layout-only diff contains exactly the two authored coordinate changes"), DiffResult.Payload.IsValid()
		&& DiffResult.Payload->TryGetArrayField(TEXT("changed"), ChangedEntries)
		&& ChangedEntries
		&& ChangedEntries->Num() == 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeDynamicNodeClassesTest,
	"UEAssetDoc.BehaviorTree.DynamicNodeClasses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeDynamicNodeClassesTest::RunTest(const FString&)
{
	const FString Target = BTTestMakeUniqueTarget(TEXT("BT_AD_DynamicClasses"));
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	FScopedBehaviorTreeMutationFixture Fixture;
	if (!BTTestApplyDecoratorTreeFixture(*this, Service, Fixture, Target, true))
	{
		return false;
	}

	UBehaviorTree* BehaviorTree = BTTestLoadBehaviorTreeForTarget(Target);
	UBTCompositeNode* Root = BehaviorTree ? BehaviorTree->RootNode : nullptr;
	TestTrue(TEXT("canonical selector class path resolves dynamically"), Root && Root->GetClass()->GetPathName() == TEXT("/Script/AIModule.BTComposite_Selector"));
	TestTrue(TEXT("canonical service class path resolves dynamically"), Root && Root->Services.Num() == 1 && Root->Services[0]->GetClass() == UBTService_DefaultFocus::StaticClass());
	TestTrue(TEXT("canonical decorator class path resolves dynamically"), Root && Root->Children.Num() > 0 && Root->Children[0].Decorators.Num() == 1 && Root->Children[0].Decorators[0]->GetClass() == UBTDecorator_Blackboard::StaticClass());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeApplyFailureDoesNotMutateExistingTest,
	"UEAssetDoc.BehaviorTree.ApplyFailureDoesNotMutateExisting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeApplyFailureDoesNotMutateExistingTest::RunTest(const FString&)
{
	const FString Target = BTTestMakeUniqueTarget(TEXT("BT_AD_FailureAtomicity"));
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	FScopedBehaviorTreeMutationFixture Fixture;
	if (!BTTestApplyDecoratorTreeFixture(*this, Service, Fixture, Target, false))
	{
		return false;
	}

	UBehaviorTree* BehaviorTree = BTTestLoadBehaviorTreeForTarget(Target);
	UBTCompositeNode* OriginalRoot = BehaviorTree ? BehaviorTree->RootNode : nullptr;
	const int32 OriginalChildren = OriginalRoot ? OriginalRoot->Children.Num() : -1;
	const int32 OriginalServices = OriginalRoot ? OriginalRoot->Services.Num() : -1;
	const int32 OriginalDecorators = OriginalRoot && OriginalRoot->Children.Num() > 0 ? OriginalRoot->Children[0].Decorators.Num() : -1;
	UBlackboardData* OriginalBlackboard = BehaviorTree ? BehaviorTree->BlackboardAsset : nullptr;
	TestNotNull(TEXT("original root exists"), OriginalRoot);

	TSharedPtr<FJsonObject> InvalidTree = BTTestMakeDecoratorBehaviorTree(Target + TEXT("_Subtree"), false);
	TSharedPtr<FJsonObject> InvalidRoot = BTTestGetObjectField(InvalidTree, TEXT("Root"));
	if (InvalidRoot.IsValid())
	{
		InvalidRoot->SetStringField(TEXT("Class"), TEXT("/Script/AIModule.BTTask_MoveTo"));
	}

	TSharedPtr<FJsonObject> Body = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BehaviorTree->BlackboardAsset->GetPathName()),
		InvalidTree,
		MakeShared<FJsonObject>());
	const FAssetDocumentResult FailedApply = Fixture.Apply(Service, Target, BTTestMakeBehaviorTreeDocument(Target, Body));
	TestFalse(TEXT("invalid root task apply fails"), FailedApply.IsSuccess());

	TestTrue(TEXT("RootNode pointer survives failed apply"), BehaviorTree->RootNode == OriginalRoot);
	TestEqual(TEXT("existing children survive failed apply"), OriginalRoot ? OriginalRoot->Children.Num() : -1, OriginalChildren);

	TSharedPtr<FJsonObject> LateInvalidTree = BTTestMakeDecoratorBehaviorTree(Target + TEXT("_Subtree"), false);
	TSharedPtr<FJsonObject> LateInvalidRoot = BTTestGetObjectField(LateInvalidTree, TEXT("Root"));
	const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
	if (LateInvalidRoot.IsValid() && LateInvalidRoot->TryGetArrayField(TEXT("Children"), Children) && Children && Children->Num() > 1)
	{
		TSharedPtr<FJsonObject> SecondEdge = BTTestGetObjectFromValue((*Children)[1]);
		TSharedPtr<FJsonObject> SecondChild = BTTestGetObjectField(SecondEdge, TEXT("Child"));
		TSharedPtr<FJsonObject> Properties = BTTestGetObjectField(SecondChild, TEXT("Properties"));
		if (!Properties.IsValid())
		{
			Properties = MakeShared<FJsonObject>();
			SecondChild->SetObjectField(TEXT("Properties"), Properties);
		}
		Properties->SetStringField(TEXT("ParentNode"), TEXT("invalid"));
	}

	TSharedPtr<FJsonObject> LateInvalidBody = BTTestMakeBehaviorTreeBody(
		nullptr,
		LateInvalidTree,
		MakeShared<FJsonObject>());
	LateInvalidBody->RemoveField(TEXT("BlackboardAsset"));
	const FAssetDocumentResult LateFailedApply = Fixture.Apply(Service, Target, BTTestMakeBehaviorTreeDocument(Target, LateInvalidBody));
	TestFalse(TEXT("late reflected property apply fails"), LateFailedApply.IsSuccess());
	TestTrue(TEXT("RootNode pointer survives late failed apply"), BehaviorTree->RootNode == OriginalRoot);
	TestEqual(TEXT("children survive late failed apply"), OriginalRoot ? OriginalRoot->Children.Num() : -1, OriginalChildren);
	TestEqual(TEXT("services survive late failed apply"), OriginalRoot ? OriginalRoot->Services.Num() : -1, OriginalServices);
	TestEqual(TEXT("decorators survive late failed apply"), OriginalRoot && OriginalRoot->Children.Num() > 0 ? OriginalRoot->Children[0].Decorators.Num() : -1, OriginalDecorators);
	TestTrue(TEXT("BlackboardAsset survives late failed apply"), BehaviorTree->BlackboardAsset == OriginalBlackboard);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeCrossRegionValidationUsesTransientOuterTest,
	"UEAssetDoc.BehaviorTree.CrossRegionValidationUsesTransientOuter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeCrossRegionValidationUsesTransientOuterTest::RunTest(const FString&)
{
	const FString Target = BTTestMakeUniqueTarget(TEXT("BT_AD_CrossRegionTransientOuter"));
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	FScopedBehaviorTreeMutationFixture Fixture;
	if (!BTTestApplyDecoratorTreeFixture(*this, Service, Fixture, Target, false))
	{
		return false;
	}

	UBehaviorTree* BehaviorTree = BTTestLoadBehaviorTreeForTarget(Target);
	TestNotNull(TEXT("BehaviorTree exists before cross-region validation"), BehaviorTree);
	if (!BehaviorTree)
	{
		return false;
	}

	const int32 OriginalNodeChildren = BTTestCountBehaviorTreeNodeChildren(BehaviorTree);
	TSharedPtr<FJsonObject> Body = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BehaviorTree->BlackboardAsset->GetPathName()),
		BTTestMakeMoveToTreeWithKeys({TEXT("TargetActor")}),
		MakeShared<FJsonObject>());

	FAssetDocumentValidateRequest ValidateRequest;
	ValidateRequest.Document = BTTestMakeBehaviorTreeDocument(Target, Body);
	const FAssetDocumentResult ValidateResult = Service.Validate(ValidateRequest);
	TestTrue(TEXT("cross-region validation succeeds"), ValidateResult.IsSuccess());
	TestEqual(TEXT("cross-region validation does not create preview BT nodes under real asset"), BTTestCountBehaviorTreeNodeChildren(BehaviorTree), OriginalNodeChildren);

	TSharedPtr<FJsonObject> InvalidBody = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BehaviorTree->BlackboardAsset->GetPathName()),
		BTTestMakeMoveToTreeWithKeys({TEXT("MissingTarget")}),
		MakeShared<FJsonObject>());
	const FAssetDocumentResult FailedApply = Fixture.Apply(Service, Target, BTTestMakeBehaviorTreeDocument(Target, InvalidBody));
	TestFalse(TEXT("invalid key apply preflight fails"), FailedApply.IsSuccess());
	TestEqual(TEXT("failed apply preflight does not create preview BT nodes under real asset"), BTTestCountBehaviorTreeNodeChildren(BehaviorTree), OriginalNodeChildren);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeBlackboardKeySelectorsTest,
	"UEAssetDoc.BehaviorTree.BlackboardKeySelectors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeBlackboardKeySelectorsTest::RunTest(const FString&)
{
	const FString Target = BTTestMakeUniqueTarget(TEXT("BT_AD_KeySelectors"));
	const FString ParentBlackboardTarget = Target + TEXT("_ParentBB");
	const FString BlackboardTarget = Target + TEXT("_BB");
	FScopedBehaviorTreeMutationFixture Cleanup;
	if (!Cleanup.ReserveTarget(ParentBlackboardTarget) || !Cleanup.ReserveTarget(BlackboardTarget)) return false;
	UBlackboardData* ParentBlackboard = BTTestMakeExistingBlackboardAsset(ParentBlackboardTarget);
	UBlackboardData* Blackboard = BTTestMakeExistingBlackboardAsset(BlackboardTarget);
	TestNotNull(TEXT("parent blackboard exists"), ParentBlackboard);
	TestNotNull(TEXT("child blackboard exists"), Blackboard);
	if (!ParentBlackboard || !Blackboard)
	{
		return false;
	}

	BTTestAddObjectBlackboardKey(ParentBlackboard, TEXT("TargetActor"));
	BTTestAddBlackboardKey<UBlackboardKeyType_Vector>(Blackboard, TEXT("MoveLocation"));
	Blackboard->Parent = ParentBlackboard;

	TSharedPtr<FJsonObject> Body = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BTTestMakeObjectPathFromTarget(BlackboardTarget)),
		BTTestMakeMoveToTreeWithKeys({TEXT("TargetActor"), TEXT("MoveLocation")}),
		MakeShared<FJsonObject>());
	FAssetDocumentValidateRequest Request;
	Request.Document = BTTestMakeBehaviorTreeDocument(Target, Body);
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Validate(Request);
	TestTrue(TEXT("selector keys resolve through local and parent blackboards"), Result.IsSuccess());
	if (!Result.IsSuccess())
	{
		AddError(Result.Message);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeSubtreeBlackboardCompatibilityTest,
	"UEAssetDoc.BehaviorTree.SubtreeBlackboardCompatibility",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeSubtreeBlackboardCompatibilityTest::RunTest(const FString&)
{
	const FString Target = BTTestMakeUniqueTarget(TEXT("BT_AD_SubtreeCompatibility"));
	FScopedBehaviorTreeMutationFixture Fixture;
	if (!TestTrue(TEXT("subtree compatibility fixture reserves target"), Fixture.ReserveTarget(Target)))
	{
		return false;
	}
	const FString ParentBlackboardTarget = Target + TEXT("_ParentBB");
	const FString ChildBlackboardTarget = Target + TEXT("_ChildBB");
	const FString IncompatibleBlackboardTarget = Target + TEXT("_IncompatibleBB");
	const FString ParentSubtreeTarget = Target + TEXT("_ParentSubtree");
	const FString SameSubtreeTarget = Target + TEXT("_SameSubtree");
	const FString ChildSubtreeTarget = Target + TEXT("_ChildSubtree");
	const FString IncompatibleSubtreeTarget = Target + TEXT("_IncompatibleSubtree");

	UBlackboardData* ParentBlackboard = BTTestMakeExistingBlackboardAsset(ParentBlackboardTarget);
	UBlackboardData* ChildBlackboard = BTTestMakeExistingBlackboardAsset(ChildBlackboardTarget);
	UBlackboardData* IncompatibleBlackboard = BTTestMakeExistingBlackboardAsset(IncompatibleBlackboardTarget);
	TestNotNull(TEXT("parent blackboard exists"), ParentBlackboard);
	TestNotNull(TEXT("child blackboard exists"), ChildBlackboard);
	TestNotNull(TEXT("incompatible blackboard exists"), IncompatibleBlackboard);
	if (!ParentBlackboard || !ChildBlackboard || !IncompatibleBlackboard)
	{
		return false;
	}

	BTTestAddObjectBlackboardKey(ParentBlackboard, TEXT("TargetActor"));
	BTTestAddBlackboardKey<UBlackboardKeyType_Vector>(ChildBlackboard, TEXT("MoveLocation"));
	BTTestAddBlackboardKey<UBlackboardKeyType_Bool>(IncompatibleBlackboard, TEXT("HasTarget"));
	ChildBlackboard->Parent = ParentBlackboard;

	UBehaviorTree* ParentSubtree = BTTestMakeExistingBehaviorTreeAsset(ParentSubtreeTarget);
	UBehaviorTree* SameSubtree = BTTestMakeExistingBehaviorTreeAsset(SameSubtreeTarget);
	UBehaviorTree* ChildSubtree = BTTestMakeExistingBehaviorTreeAsset(ChildSubtreeTarget);
	UBehaviorTree* IncompatibleSubtree = BTTestMakeExistingBehaviorTreeAsset(IncompatibleSubtreeTarget);
	TestNotNull(TEXT("parent subtree exists"), ParentSubtree);
	TestNotNull(TEXT("same subtree exists"), SameSubtree);
	TestNotNull(TEXT("child subtree exists"), ChildSubtree);
	TestNotNull(TEXT("incompatible subtree exists"), IncompatibleSubtree);
	if (!ParentSubtree || !SameSubtree || !ChildSubtree || !IncompatibleSubtree)
	{
		return false;
	}
	ParentSubtree->BlackboardAsset = ParentBlackboard;
	SameSubtree->BlackboardAsset = ParentBlackboard;
	ChildSubtree->BlackboardAsset = ChildBlackboard;
	IncompatibleSubtree->BlackboardAsset = IncompatibleBlackboard;

	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	TSharedPtr<FJsonObject> ParentUsesSameSubtreeBody = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BTTestMakeObjectPathFromTarget(ParentBlackboardTarget)),
		BTTestMakeRunBehaviorTree(SameSubtreeTarget),
		MakeShared<FJsonObject>());
	FAssetDocumentValidateRequest ParentUsesSameSubtreeRequest;
	ParentUsesSameSubtreeRequest.Document = BTTestMakeBehaviorTreeDocument(Target, ParentUsesSameSubtreeBody);
	const FAssetDocumentResult ParentUsesSameSubtreeResult = Service.Validate(ParentUsesSameSubtreeRequest);
	TestTrue(TEXT("parent blackboard can run same-blackboard subtree"), ParentUsesSameSubtreeResult.IsSuccess());
	if (!ParentUsesSameSubtreeResult.IsSuccess())
	{
		AddError(ParentUsesSameSubtreeResult.Message);
	}

	TSharedPtr<FJsonObject> ChildUsesParentSubtreeBody = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BTTestMakeObjectPathFromTarget(ChildBlackboardTarget)),
		BTTestMakeRunBehaviorTree(ParentSubtreeTarget),
		MakeShared<FJsonObject>());
	FAssetDocumentValidateRequest ChildUsesParentSubtreeRequest;
	ChildUsesParentSubtreeRequest.Document = BTTestMakeBehaviorTreeDocument(Target, ChildUsesParentSubtreeBody);
	const FAssetDocumentResult ChildUsesParentSubtreeResult = Service.Validate(ChildUsesParentSubtreeRequest);
	TestTrue(TEXT("child blackboard can run parent-blackboard subtree"), ChildUsesParentSubtreeResult.IsSuccess());
	if (!ChildUsesParentSubtreeResult.IsSuccess())
	{
		AddError(ChildUsesParentSubtreeResult.Message);
	}

	TSharedPtr<FJsonObject> ChildUsesSameSubtreeBody = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BTTestMakeObjectPathFromTarget(ChildBlackboardTarget)),
		BTTestMakeRunBehaviorTree(ChildSubtreeTarget),
		MakeShared<FJsonObject>());
	FAssetDocumentValidateRequest ChildUsesSameSubtreeRequest;
	ChildUsesSameSubtreeRequest.Document = BTTestMakeBehaviorTreeDocument(Target, ChildUsesSameSubtreeBody);
	const FAssetDocumentResult ChildUsesSameSubtreeResult = Service.Validate(ChildUsesSameSubtreeRequest);
	TestTrue(TEXT("child blackboard can run same child-blackboard subtree"), ChildUsesSameSubtreeResult.IsSuccess());
	if (!ChildUsesSameSubtreeResult.IsSuccess())
	{
		AddError(ChildUsesSameSubtreeResult.Message);
	}

	TSharedPtr<FJsonObject> ParentUsesChildSubtreeBody = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BTTestMakeObjectPathFromTarget(ParentBlackboardTarget)),
		BTTestMakeRunBehaviorTree(ChildSubtreeTarget),
		MakeShared<FJsonObject>());
	FAssetDocumentValidateRequest ParentUsesChildSubtreeRequest;
	ParentUsesChildSubtreeRequest.Document = BTTestMakeBehaviorTreeDocument(Target, ParentUsesChildSubtreeBody);
	const FAssetDocumentResult ParentUsesChildSubtreeResult = Service.Validate(ParentUsesChildSubtreeRequest);
	TestFalse(TEXT("parent blackboard cannot run child-blackboard subtree"), ParentUsesChildSubtreeResult.IsSuccess());
	TestTrue(TEXT("child subtree mismatch diagnostic is exact"), BTTestResultHasDiagnostic(ParentUsesChildSubtreeResult, TEXT("IncompatibleBehaviorTreeBlackboard"), TEXT("/Body/Tree/Root/Children/0/Properties/BehaviorAsset")));

	TSharedPtr<FJsonObject> IncompatibleBody = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BTTestMakeObjectPathFromTarget(ParentBlackboardTarget)),
		BTTestMakeRunBehaviorTree(IncompatibleSubtreeTarget),
		MakeShared<FJsonObject>());
	FAssetDocumentValidateRequest IncompatibleRequest;
	IncompatibleRequest.Document = BTTestMakeBehaviorTreeDocument(Target, IncompatibleBody);
	const FAssetDocumentResult IncompatibleResult = Service.Validate(IncompatibleRequest);
	TestFalse(TEXT("subtree with unrelated blackboard is rejected"), IncompatibleResult.IsSuccess());
	TestTrue(TEXT("subtree mismatch diagnostic is exact"), BTTestResultHasDiagnostic(IncompatibleResult, TEXT("IncompatibleBehaviorTreeBlackboard"), TEXT("/Body/Tree/Root/Children/0/Properties/BehaviorAsset")));

	const FAssetDocumentResult ApplyCompatible = Fixture.Apply(Service, Target, BTTestMakeBehaviorTreeDocument(Target, ParentUsesSameSubtreeBody));
	TestTrue(TEXT("compatible subtree fixture apply succeeds"), ApplyCompatible.IsSuccess());
	UBehaviorTree* BehaviorTree = BTTestLoadBehaviorTreeForTarget(Target);
	UBTCompositeNode* OriginalRoot = BehaviorTree ? BehaviorTree->RootNode : nullptr;
	UBlackboardData* OriginalBlackboard = BehaviorTree ? BehaviorTree->BlackboardAsset : nullptr;
	TestNotNull(TEXT("applied root exists before missing blackboard check"), OriginalRoot);

	TSharedPtr<FJsonObject> MissingBlackboardBody = BTTestMakeBehaviorTreeBody(
		nullptr,
		BTTestMakeRunBehaviorTree(SameSubtreeTarget),
		MakeShared<FJsonObject>());
	const FAssetDocumentResult MissingBlackboardApply = Fixture.Apply(Service, Target, BTTestMakeBehaviorTreeDocument(Target, MissingBlackboardBody));
	TestFalse(TEXT("explicit null Body.BlackboardAsset for subtree validation is rejected"), MissingBlackboardApply.IsSuccess());
	TestTrue(TEXT("missing blackboard diagnostic is exact"), BTTestResultHasDiagnostic(MissingBlackboardApply, TEXT("MissingBehaviorTreeBlackboard"), TEXT("/Body/BlackboardAsset")));
	TestTrue(TEXT("failed missing-blackboard apply does not replace root"), BehaviorTree && BehaviorTree->RootNode == OriginalRoot);
	TestTrue(TEXT("failed missing-blackboard apply does not replace blackboard"), BehaviorTree && BehaviorTree->BlackboardAsset == OriginalBlackboard);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeUnknownBlackboardKeyRejectsTest,
	"UEAssetDoc.BehaviorTree.UnknownBlackboardKeyRejects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeUnknownBlackboardKeyRejectsTest::RunTest(const FString&)
{
	const FString Target = BTTestMakeUniqueTarget(TEXT("BT_AD_UnknownKey"));
	const FString BlackboardTarget = Target + TEXT("_BB");
	FScopedBehaviorTreeMutationFixture Cleanup;
	if (!Cleanup.ReserveTarget(BlackboardTarget)) return false;
	UBlackboardData* Blackboard = BTTestMakeExistingBlackboardAsset(BlackboardTarget);
	TestNotNull(TEXT("blackboard exists"), Blackboard);
	if (!Blackboard)
	{
		return false;
	}
	BTTestAddObjectBlackboardKey(Blackboard, TEXT("TargetActor"));

	TSharedPtr<FJsonObject> Body = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BTTestMakeObjectPathFromTarget(BlackboardTarget)),
		BTTestMakeMoveToTreeWithKeys({TEXT("MissingTarget")}),
		MakeShared<FJsonObject>());
	FAssetDocumentValidateRequest Request;
	Request.Document = BTTestMakeBehaviorTreeDocument(Target, Body);
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Validate(Request);
	TestFalse(TEXT("unknown blackboard selector key is rejected"), Result.IsSuccess());
	TestTrue(TEXT("unknown key diagnostic is exact"), BTTestResultHasDiagnostic(Result, TEXT("UnknownBlackboardKey"), TEXT("/Body/Tree/Root/Children/0/Properties/BlackboardKey/Key")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeDuplicateNodeIdDiagnosticTest,
	"UEAssetDoc.BehaviorTree.DuplicateNodeIdDiagnostic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeDuplicateNodeIdDiagnosticTest::RunTest(const FString&)
{
	const FString Target = TEXT("/Game/AssetDocumentTests/BT_AD_DuplicateNodeIdDiagnostic");
	TSharedPtr<FJsonObject> Child = BTTestMakeBtNode(
		TEXT("Root"),
		TEXT("/Script/AIModule.BTTask_WaitBlackboardTime"),
		MakeShared<FJsonObject>());
	TSharedPtr<FJsonObject> Edge = MakeShared<FJsonObject>();
	Edge->SetObjectField(TEXT("Child"), Child);

	TSharedPtr<FJsonObject> Root = BTTestMakeBtNode(TEXT("Root"), TEXT("/Script/AIModule.BTComposite_Selector"));
	Root->SetArrayField(TEXT("Children"), {BTTestMakeObjectValue(Edge)});

	TSharedPtr<FJsonObject> Tree = MakeShared<FJsonObject>();
	Tree->SetObjectField(TEXT("Root"), Root);

	FAssetDocumentValidateRequest Request;
	Request.Document = BTTestMakeBehaviorTreeDocument(
		Target,
		BTTestMakeBehaviorTreeBody(nullptr, Tree, MakeShared<FJsonObject>()));
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Validate(Request);
	TestFalse(TEXT("duplicate behavior tree node id is rejected"), Result.IsSuccess());
	TestTrue(TEXT("duplicate behavior tree NodeGuid diagnostic is exact"), BTTestResultHasDiagnostic(Result, TEXT("DuplicateBehaviorTreeNodeGuid"), TEXT("/Body/Tree/Root/Children/0/Id")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeIncompatibleBlackboardKeyTypeRejectsTest,
	"UEAssetDoc.BehaviorTree.IncompatibleBlackboardKeyTypeRejects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeIncompatibleBlackboardKeyTypeRejectsTest::RunTest(const FString&)
{
	const FString Target = BTTestMakeUniqueTarget(TEXT("BT_AD_KeyTypeMismatch"));
	const FString BlackboardTarget = Target + TEXT("_BB");
	FScopedBehaviorTreeMutationFixture Cleanup;
	if (!Cleanup.ReserveTarget(BlackboardTarget)) return false;
	UBlackboardData* Blackboard = BTTestMakeExistingBlackboardAsset(BlackboardTarget);
	TestNotNull(TEXT("blackboard exists"), Blackboard);
	if (!Blackboard)
	{
		return false;
	}
	BTTestAddBlackboardKey<UBlackboardKeyType_Bool>(Blackboard, TEXT("HasTarget"));
	BTTestAddBlackboardKey<UBlackboardKeyType_Vector>(Blackboard, TEXT("MoveLocation"));

	TSharedPtr<FJsonObject> Selector = BTTestMakeSelectorProperty(TEXT("HasTarget"));
	TSharedPtr<FJsonObject> Body = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BTTestMakeObjectPathFromTarget(BlackboardTarget)),
		BTTestMakeTreeWithSelectorTask(Selector),
		MakeShared<FJsonObject>());
	FAssetDocumentValidateRequest Request;
	Request.Document = BTTestMakeBehaviorTreeDocument(Target, Body);
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Validate(Request);
	TestFalse(TEXT("selector key type mismatch is rejected"), Result.IsSuccess());
	TestTrue(TEXT("key type mismatch diagnostic is exact"), BTTestResultHasDiagnostic(Result, TEXT("IncompatibleBlackboardKeyType"), TEXT("/Body/Tree/Root/Children/0/Properties/BlackboardKey/Key")));

	TSharedPtr<FJsonObject> CompatibleMoveToBody = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BTTestMakeObjectPathFromTarget(BlackboardTarget)),
		BTTestMakeTreeWithSelectorTask(BTTestMakeSelectorProperty(TEXT("MoveLocation"))),
		MakeShared<FJsonObject>());
	FAssetDocumentValidateRequest CompatibleMoveToRequest;
	CompatibleMoveToRequest.Document = BTTestMakeBehaviorTreeDocument(Target, CompatibleMoveToBody);
	const FAssetDocumentResult CompatibleMoveToResult = FAssetDocumentModule::Get().GetService().Validate(CompatibleMoveToRequest);
	TestTrue(TEXT("MoveTo class policy accepts a vector key"), CompatibleMoveToResult.IsSuccess());
	if (!CompatibleMoveToResult.IsSuccess())
	{
		AddError(CompatibleMoveToResult.Message);
	}

	TSharedPtr<FJsonObject> CompatibleBoolBody = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BTTestMakeObjectPathFromTarget(BlackboardTarget)),
		BTTestMakeTreeWithSelectorTask(
			BTTestMakeSelectorProperty(TEXT("HasTarget")),
			TEXT("/Script/AIModule.BTTask_SetKeyValueBool")),
		MakeShared<FJsonObject>());
	FAssetDocumentValidateRequest CompatibleBoolRequest;
	CompatibleBoolRequest.Document = BTTestMakeBehaviorTreeDocument(Target, CompatibleBoolBody);
	const FAssetDocumentResult CompatibleBoolResult = FAssetDocumentModule::Get().GetService().Validate(CompatibleBoolRequest);
	TestTrue(TEXT("SetKeyValueBool class policy accepts a bool key"), CompatibleBoolResult.IsSuccess());
	if (!CompatibleBoolResult.IsSuccess())
	{
		AddError(CompatibleBoolResult.Message);
	}

	TSharedPtr<FJsonObject> IncompatibleBoolBody = BTTestMakeBehaviorTreeBody(
		BTTestMakeAssetRef(BTTestMakeObjectPathFromTarget(BlackboardTarget)),
		BTTestMakeTreeWithSelectorTask(
			BTTestMakeSelectorProperty(TEXT("MoveLocation")),
			TEXT("/Script/AIModule.BTTask_SetKeyValueBool")),
		MakeShared<FJsonObject>());
	FAssetDocumentValidateRequest IncompatibleBoolRequest;
	IncompatibleBoolRequest.Document = BTTestMakeBehaviorTreeDocument(Target, IncompatibleBoolBody);
	const FAssetDocumentResult IncompatibleBoolResult = FAssetDocumentModule::Get().GetService().Validate(IncompatibleBoolRequest);
	TestFalse(TEXT("SetKeyValueBool class policy rejects a vector key"), IncompatibleBoolResult.IsSuccess());
	TestTrue(TEXT("bool policy mismatch diagnostic is exact"), BTTestResultHasDiagnostic(IncompatibleBoolResult, TEXT("IncompatibleBlackboardKeyType"), TEXT("/Body/Tree/Root/Children/0/Properties/BlackboardKey/Key")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeFullApplyExtractDiffTest,
	"UEAssetDoc.BehaviorTree.FullApplyExtractDiff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeFullApplyExtractDiffTest::RunTest(const FString&)
{
	const FString Target = BTTestMakeUniqueTarget(TEXT("BT_AD_FullApplyExtractDiff"));
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	FScopedBehaviorTreeMutationFixture Fixture;
	FString ParentBlackboardTarget;
	FString BlackboardTarget;
	FString SubtreeTarget;
	if (!BTTestPrepareFullBehaviorTreeFixture(*this, Service, Fixture, Target, ParentBlackboardTarget, BlackboardTarget, SubtreeTarget))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Body = BTTestMakeFullBehaviorTreeBody(BlackboardTarget, SubtreeTarget);
	TSharedPtr<FJsonObject> Document = BTTestMakeBehaviorTreeDocument(Target, Body);
	const FAssetDocumentResult ApplyResult = Fixture.Apply(Service, Target, Document);
	TestTrue(TEXT("full BehaviorTree apply succeeds"), ApplyResult.IsSuccess());
	if (!ApplyResult.IsSuccess())
	{
		AddError(ApplyResult.Message);
		return false;
	}

	UBlackboardData* Blackboard = LoadObject<UBlackboardData>(nullptr, *BTTestMakeObjectPathFromTarget(BlackboardTarget));
	TestTrue(TEXT("local blackboard has parent"), Blackboard && Blackboard->Parent.Get() == LoadObject<UBlackboardData>(nullptr, *BTTestMakeObjectPathFromTarget(ParentBlackboardTarget)));
	TestEqual(TEXT("local blackboard keeps local key count"), Blackboard ? Blackboard->Keys.Num() : -1, 2);

	UBehaviorTree* BehaviorTree = BTTestLoadBehaviorTreeForTarget(Target);
	UBTCompositeNode* Root = BehaviorTree ? BehaviorTree->RootNode : nullptr;
	TestTrue(TEXT("BehaviorTree references authored blackboard"), BehaviorTree && BehaviorTree->BlackboardAsset == Blackboard);
	TestNotNull(TEXT("root composite exists"), Root);
	TestEqual(TEXT("root decorator count"), BehaviorTree ? BehaviorTree->RootDecorators.Num() : -1, 1);
	TestEqual(TEXT("ordinary root decorators have no runtime logic program"), BehaviorTree ? BehaviorTree->RootDecoratorOps.Num() : -1, 0);
	TestEqual(TEXT("root service count"), Root ? Root->Services.Num() : -1, 1);
	TestEqual(TEXT("root child count"), Root ? Root->Children.Num() : -1, 2);
	if (Root && Root->Children.Num() >= 2)
	{
		TestEqual(TEXT("first edge decorator count"), Root->Children[0].Decorators.Num(), 1);
		TestEqual(TEXT("ordinary edge decorators have no runtime logic program"), Root->Children[0].DecoratorOps.Num(), 0);
		TestNotNull(TEXT("first child task exists"), Root->Children[0].ChildTask.Get());
		UBTTask_RunBehavior* RunSubtreeTask = Cast<UBTTask_RunBehavior>(Root->Children[1].ChildTask);
		TestNotNull(TEXT("subtree task exists"), RunSubtreeTask);
		TestTrue(TEXT("second child is BTTask_RunBehavior"), RunSubtreeTask && RunSubtreeTask->GetClass()->IsChildOf(UBTTask_RunBehavior::StaticClass()));
	}

	FAssetDocumentExtractRequest ExtractRequest;
	ExtractRequest.AssetPath = Target;
	ExtractRequest.bDiffOnly = true;
	const FAssetDocumentResult ExtractResult = Service.Extract(ExtractRequest);
	TestTrue(TEXT("full BehaviorTree extract succeeds"), ExtractResult.IsSuccess());
	TestTrue(TEXT("full BehaviorTree extract returns payload"), ExtractResult.Payload.IsValid());
	if (!ExtractResult.IsSuccess() || !ExtractResult.Payload.IsValid())
	{
		AddError(ExtractResult.Message);
		return false;
	}

	const TSharedPtr<FJsonObject>* ExtractedBody = nullptr;
	const TSharedPtr<FJsonObject>* ExtractedTree = nullptr;
	TestTrue(TEXT("extract contains Body"), ExtractResult.Payload->TryGetObjectField(TEXT("Body"), ExtractedBody));
	TestTrue(TEXT("extract contains Body.Tree"), ExtractedBody && ExtractedBody->IsValid() && (*ExtractedBody)->TryGetObjectField(TEXT("Tree"), ExtractedTree));
	TestFalse(TEXT("extract does not split Body.EditorLayout"), ExtractedBody && ExtractedBody->IsValid() && (*ExtractedBody)->HasField(TEXT("EditorLayout")));
	if (ExtractedTree && ExtractedTree->IsValid())
	{
		TSharedPtr<FJsonObject> ExtractedRoot = BTTestGetObjectField(*ExtractedTree, TEXT("Root"));
		const TArray<TSharedPtr<FJsonValue>>* RootDecorators = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Comments = nullptr;
		TestTrue(TEXT("extract includes root node decorators"), ExtractedRoot.IsValid() && ExtractedRoot->TryGetArrayField(TEXT("Decorators"), RootDecorators) && RootDecorators && RootDecorators->Num() == 1);
		TestTrue(TEXT("extract includes graph comments"), (*ExtractedTree)->TryGetArrayField(TEXT("Comments"), Comments) && Comments && Comments->Num() == 1);
		TestNotNull(TEXT("extract includes inline root editor state"), BTTestGetObjectField(ExtractedRoot, TEXT("Editor")).Get());
	}

	const FAssetDocumentResult DiffResult = Service.Diff(BTTestMakeDiffRequest(Document));
	TestTrue(TEXT("full BehaviorTree diff succeeds"), DiffResult.IsSuccess());
	TestTrue(TEXT("full BehaviorTree roundtrip has no changed or failed diff entries"), BTTestDiffPayloadHasNoChangedOrFailedEntries(DiffResult.Payload));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeApplyFileCanonicalWritebackTest,
	"UEAssetDoc.BehaviorTree.ApplyFileCanonicalWriteback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeApplyFileCanonicalWritebackTest::RunTest(const FString&)
{
	const FString Target = BTTestMakeUniqueTarget(TEXT("BT_AD_ApplyFileWriteback"));
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	FScopedBehaviorTreeMutationFixture Fixture;
	FString ParentBlackboardTarget;
	FString BlackboardTarget;
	FString SubtreeTarget;
	if (!BTTestPrepareFullBehaviorTreeFixture(*this, Service, Fixture, Target, ParentBlackboardTarget, BlackboardTarget, SubtreeTarget))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Document = BTTestMakeBehaviorTreeDocument(Target, BTTestMakeFullBehaviorTreeBody(BlackboardTarget, SubtreeTarget));
	TestFalse(TEXT("primary ApplyFile target is absent before full fixture projection"), FPackageName::DoesPackageExist(Target));
	TestNull(TEXT("primary ApplyFile target is not loaded before full fixture projection"), FindObject<UBehaviorTree>(nullptr, *BTTestMakeObjectPathFromTarget(Target)));
	// Complete omitted defaults through the existing production projection on an
	// independently owned target; the primary target still starts with Create.
	const FString ProjectionTarget = Target + TEXT("_Normalization");
	const FAssetDocumentResult ProjectionApplied = Fixture.Apply(Service, ProjectionTarget,
		BTTestMakeBehaviorTreeDocument(ProjectionTarget, BTTestGetObjectField(Document, TEXT("Body"))));
	TestTrue(TEXT("full fixture projection applies and saves through the public route"), ProjectionApplied.IsSuccess() && ProjectionApplied.bSavedAsset);
	if (!ProjectionApplied.IsSuccess())
	{
		AddError(ProjectionApplied.Message);
		return false;
	}
	FAssetDocumentExtractRequest ProjectionRequest;
	ProjectionRequest.AssetPath = ProjectionTarget;
	ProjectionRequest.bDiffOnly = true;
	const FAssetDocumentResult ProjectionExtracted = Service.Extract(ProjectionRequest);
	const auto CompleteBody = BTTestGetObjectField(ProjectionExtracted.Payload, TEXT("Body"));
	TestTrue(TEXT("full fixture projection extracts complete BlackboardAsset and Tree"), ProjectionExtracted.IsSuccess()
		&& CompleteBody.IsValid() && CompleteBody->HasField(TEXT("BlackboardAsset")) && CompleteBody->HasField(TEXT("Tree")));
	if (!ProjectionExtracted.IsSuccess() || !CompleteBody.IsValid()) return false;
	Document->SetObjectField(TEXT("Body"), CompleteBody);
	TestFalse(TEXT("primary ApplyFile target remains absent after full fixture projection"), FPackageName::DoesPackageExist(Target));
	TestNull(TEXT("primary ApplyFile target remains unloaded after full fixture projection"), FindObject<UBehaviorTree>(nullptr, *BTTestMakeObjectPathFromTarget(Target)));
	FString SidecarPath;
	const FAssetDocumentResult WriteResult = Fixture.WriteCanonicalSidecar(Target, Document, SidecarPath);
	if (!WriteResult.IsSuccess())
	{
		AddError(WriteResult.Message);
		return false;
	}

	FAssetDocumentValidateRequest ValidateRequest;
	ValidateRequest.FilePath = SidecarPath;
	const FAssetDocumentResult ValidateResult = Service.Validate(ValidateRequest);
	TestTrue(TEXT("BehaviorTree ApplyFile sidecar validates"), ValidateResult.IsSuccess());
	if (!ValidateResult.IsSuccess())
	{
		AddError(ValidateResult.Message);
		return false;
	}

	FAssetDocumentApplyFileRequest ApplyFileRequest;
	ApplyFileRequest.FilePath = SidecarPath;
	ApplyFileRequest.bSaveAsset = true;
	const FAssetDocumentResult ApplyFileResult = Service.ApplyFile(ApplyFileRequest);
	TestTrue(TEXT("BehaviorTree ApplyFile succeeds"), ApplyFileResult.IsSuccess());
	TestTrue(TEXT("BehaviorTree ApplyFile writes sidecar sync state"), ApplyFileResult.bWroteSidecar);
	if (ApplyFileResult.Payload.IsValid())
	{
		FString SkipReason;
		if (ApplyFileResult.Payload->TryGetStringField(TEXT("sidecar_sync_update_skip_reason"), SkipReason))
		{
			AddError(FString::Printf(TEXT("BehaviorTree sidecar sync update skip reason: %s"), *SkipReason));
		}
		TestFalse(TEXT("BehaviorTree ApplyFile does not skip sync update"), ApplyFileResult.Payload->HasField(TEXT("sidecar_sync_update_skipped")));
	}
	if (!ApplyFileResult.IsSuccess())
	{
		for (const FAssetDocumentDiagnostic& Diagnostic : ApplyFileResult.Diagnostics)
			AddInfo(FString::Printf(TEXT("ApplyFile diagnostic [%s %s: %s]"),
				*Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
		AddError(ApplyFileResult.Message);
		return false;
	}

	TSharedPtr<FJsonObject> ReloadedSidecar;
	if (!BTTestLoadSidecarJson(this, SidecarPath, ReloadedSidecar))
	{
		return false;
	}
	BTTestExpectDocumentSync(this, Service, Target, SidecarPath, ReloadedSidecar, {
		TEXT("Body.BlackboardAsset"),
		TEXT("Body.Tree"),
	});

	FAssetDocumentDiffRequest DiffRequest;
	DiffRequest.FilePath = SidecarPath;
	const FAssetDocumentResult DiffResult = Service.Diff(DiffRequest);
	TestTrue(TEXT("BehaviorTree ApplyFile diff succeeds"), DiffResult.IsSuccess());
	TestTrue(TEXT("BehaviorTree ApplyFile diff has no changed or failed entries"), BTTestDiffPayloadHasNoChangedOrFailedEntries(DiffResult.Payload));

	TSharedPtr<FJsonObject> DerivedAllowedTypesDocument = BTTestMakeBehaviorTreeDocument(Target, BTTestMakeFullBehaviorTreeBody(BlackboardTarget, SubtreeTarget));
	TSharedPtr<FJsonObject> DerivedAllowedTypesBody = BTTestGetObjectField(DerivedAllowedTypesDocument, TEXT("Body"));
	TSharedPtr<FJsonObject> DerivedAllowedTypesTree = BTTestGetObjectField(DerivedAllowedTypesBody, TEXT("Tree"));
	TSharedPtr<FJsonObject> DerivedAllowedTypesRoot = BTTestGetObjectField(DerivedAllowedTypesTree, TEXT("Root"));
	const TArray<TSharedPtr<FJsonValue>>* DerivedAllowedTypesChildren = nullptr;
	if (DerivedAllowedTypesRoot.IsValid() && DerivedAllowedTypesRoot->TryGetArrayField(TEXT("Children"), DerivedAllowedTypesChildren) && DerivedAllowedTypesChildren && DerivedAllowedTypesChildren->Num() > 0)
	{
		TSharedPtr<FJsonObject> MoveToNode = BTTestGetObjectFromValue((*DerivedAllowedTypesChildren)[0]);
		TSharedPtr<FJsonObject> MoveToProperties = BTTestGetObjectField(MoveToNode, TEXT("Properties"));
		TSharedPtr<FJsonObject> MoveToBlackboardKey = BTTestGetObjectField(MoveToProperties, TEXT("BlackboardKey"));
		if (MoveToBlackboardKey.IsValid())
		{
			MoveToBlackboardKey->SetArrayField(TEXT("AllowedTypes"), {});
		}
	}
	const FAssetDocumentResult AllowedTypesDiffResult = Service.Diff(BTTestMakeDiffRequest(DerivedAllowedTypesDocument));
	TestFalse(TEXT("BehaviorTree rejects authored selector AllowedTypes"), AllowedTypesDiffResult.IsSuccess());
	TestTrue(TEXT("BehaviorTree selector AllowedTypes diagnostic is exact"), BTTestResultHasDiagnostic(AllowedTypesDiffResult, TEXT("NonAuthoredProperty"), TEXT("/Body/Tree/Root/Children/0/Properties/BlackboardKey/AllowedTypes")));

	TSharedPtr<FJsonObject> ChangedServiceSelectorDocument = BTTestMakeBehaviorTreeDocument(Target, BTTestMakeFullBehaviorTreeBody(BlackboardTarget, SubtreeTarget));
	TSharedPtr<FJsonObject> ChangedBody = BTTestGetObjectField(ChangedServiceSelectorDocument, TEXT("Body"));
	TSharedPtr<FJsonObject> ChangedTree = BTTestGetObjectField(ChangedBody, TEXT("Tree"));
	TSharedPtr<FJsonObject> ChangedRoot = BTTestGetObjectField(ChangedTree, TEXT("Root"));
	const TArray<TSharedPtr<FJsonValue>>* Services = nullptr;
	if (ChangedRoot.IsValid() && ChangedRoot->TryGetArrayField(TEXT("Services"), Services) && Services && Services->Num() > 0)
	{
		TSharedPtr<FJsonObject> ServiceNode = BTTestGetObjectFromValue((*Services)[0]);
		TSharedPtr<FJsonObject> ServiceProperties = BTTestGetObjectField(ServiceNode, TEXT("Properties"));
		TSharedPtr<FJsonObject> ServiceBlackboardKey = BTTestGetObjectField(ServiceProperties, TEXT("BlackboardKey"));
		if (ServiceBlackboardKey.IsValid())
		{
			ServiceBlackboardKey->SetBoolField(TEXT("bNoneIsAllowedValue"), true);
		}
	}
	const FAssetDocumentResult ServiceSelectorDiffResult = Service.Diff(BTTestMakeDiffRequest(ChangedServiceSelectorDocument));
	TestFalse(TEXT("BehaviorTree rejects authored selector None policy"), ServiceSelectorDiffResult.IsSuccess());
	TestTrue(TEXT("BehaviorTree selector None policy diagnostic is exact"), BTTestResultHasDiagnostic(ServiceSelectorDiffResult, TEXT("NonAuthoredProperty"), TEXT("/Body/Tree/Root/Services/0/Properties/BlackboardKey/bNoneIsAllowedValue")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeApplyFailureRollsBackTest,
	"UEAssetDoc.BehaviorTree.ApplyFailureRollsBack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeApplyFailureRollsBackTest::RunTest(const FString&)
{
	const FString Target = BTTestMakeUniqueTarget(TEXT("BT_AD_FailureRollback"));
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	FScopedBehaviorTreeMutationFixture Fixture;
	FString ParentBlackboardTarget;
	FString BlackboardTarget;
	FString SubtreeTarget;
	if (!BTTestPrepareFullBehaviorTreeFixture(*this, Service, Fixture, Target, ParentBlackboardTarget, BlackboardTarget, SubtreeTarget))
	{
		return false;
	}

	TSharedPtr<FJsonObject> InitialDocument = BTTestMakeBehaviorTreeDocument(Target, BTTestMakeFullBehaviorTreeBody(BlackboardTarget, SubtreeTarget));
	const FAssetDocumentResult InitialApply = Fixture.Apply(Service, Target, InitialDocument);
	TestTrue(TEXT("initial full BehaviorTree apply succeeds"), InitialApply.IsSuccess());
	if (!InitialApply.IsSuccess())
	{
		AddError(InitialApply.Message);
		return false;
	}

	UBehaviorTree* BehaviorTree = BTTestLoadBehaviorTreeForTarget(Target);
	UBTCompositeNode* OriginalRoot = BehaviorTree ? BehaviorTree->RootNode : nullptr;
	UBlackboardData* OriginalBlackboard = BehaviorTree ? BehaviorTree->BlackboardAsset : nullptr;
	const int32 OriginalRootDecorators = BehaviorTree ? BehaviorTree->RootDecorators.Num() : -1;
	const int32 OriginalRootLogic = BehaviorTree ? BehaviorTree->RootDecoratorOps.Num() : -1;
	const int32 OriginalServices = OriginalRoot ? OriginalRoot->Services.Num() : -1;
	const int32 OriginalChildren = OriginalRoot ? OriginalRoot->Children.Num() : -1;
	const int32 OriginalEdgeDecorators = OriginalRoot && OriginalRoot->Children.Num() > 0 ? OriginalRoot->Children[0].Decorators.Num() : -1;
	UBehaviorTreeGraph* OriginalGraph = BehaviorTree ? Cast<UBehaviorTreeGraph>(BehaviorTree->BTGraph) : nullptr;
	TestNotNull(TEXT("original full tree root exists"), OriginalRoot);

	TSharedPtr<FJsonObject> InvalidBody = BTTestMakeFullBehaviorTreeBody(BlackboardTarget, SubtreeTarget);
	TSharedPtr<FJsonObject> InvalidTree = BTTestGetObjectField(InvalidBody, TEXT("Tree"));
	TSharedPtr<FJsonObject> InvalidRoot = BTTestGetObjectField(InvalidTree, TEXT("Root"));
	if (InvalidRoot.IsValid())
	{
		InvalidRoot->SetStringField(TEXT("Class"), TEXT("/Script/AIModule.BTTask_MoveTo"));
	}

	const FAssetDocumentResult FailedApply = Fixture.Apply(Service, Target, BTTestMakeBehaviorTreeDocument(Target, InvalidBody));
	TestFalse(TEXT("invalid full BehaviorTree apply fails"), FailedApply.IsSuccess());
	TestTrue(TEXT("failure reports invalid root diagnostic"), BTTestResultHasDiagnostic(FailedApply, TEXT("InvalidBehaviorTreeNodeClass"), TEXT("/Body/Tree/Root/Class")));
	TestTrue(TEXT("failed apply preserves blackboard"), BehaviorTree && BehaviorTree->BlackboardAsset == OriginalBlackboard);
	TestTrue(TEXT("failed apply preserves root pointer"), BehaviorTree && BehaviorTree->RootNode == OriginalRoot);
	TestEqual(TEXT("failed apply preserves root decorators"), BehaviorTree ? BehaviorTree->RootDecorators.Num() : -1, OriginalRootDecorators);
	TestEqual(TEXT("failed apply preserves root decorator logic"), BehaviorTree ? BehaviorTree->RootDecoratorOps.Num() : -1, OriginalRootLogic);
	TestEqual(TEXT("failed apply preserves services"), OriginalRoot ? OriginalRoot->Services.Num() : -1, OriginalServices);
	TestEqual(TEXT("failed apply preserves children"), OriginalRoot ? OriginalRoot->Children.Num() : -1, OriginalChildren);
	TestEqual(TEXT("failed apply preserves edge decorators"), OriginalRoot && OriginalRoot->Children.Num() > 0 ? OriginalRoot->Children[0].Decorators.Num() : -1, OriginalEdgeDecorators);
	TestTrue(TEXT("failed apply preserves editor graph"), BehaviorTree && Cast<UBehaviorTreeGraph>(BehaviorTree->BTGraph) == OriginalGraph);

	const FAssetDocumentResult DiffResult = Service.Diff(BTTestMakeDiffRequest(InitialDocument));
	TestTrue(TEXT("post-failure original document diff succeeds"), DiffResult.IsSuccess());
	TestTrue(TEXT("post-failure original document remains unchanged"), BTTestDiffPayloadHasNoChangedOrFailedEntries(DiffResult.Payload));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeProfileInspectionListsAllRegionsTest,
	"UEAssetDoc.BehaviorTree.ProfileInspectionListsAllRegions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeProfileInspectionListsAllRegionsTest::RunTest(const FString&)
{
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	FAssetDocumentProfileRequest Request;
	Request.ClassOrAsset = TEXT("/Script/AIModule.BehaviorTree");
	const FAssetDocumentResult Result = Service.InspectProfile(Request);
	TestTrue(TEXT("BehaviorTree profile inspection succeeds"), Result.IsSuccess());
	TestTrue(TEXT("BehaviorTree profile payload exists"), Result.Payload.IsValid());
	if (!Result.Payload.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("Profile class is BehaviorTree"), Result.Payload->GetStringField(TEXT("Class")), FString(TEXT("/Script/AIModule.BehaviorTree")));
	const TArray<TSharedPtr<FJsonValue>>* BodySections = nullptr;
	TestTrue(TEXT("BehaviorTree profile exposes BodySections"), Result.Payload->TryGetArrayField(TEXT("BodySections"), BodySections));
	for (const FName& BodyKey : TArray<FName>{TEXT("BlackboardAsset"), TEXT("Tree")})
	{
		TestTrue(
			FString::Printf(TEXT("BehaviorTree BodySections contains %s"), *BodyKey.ToString()),
			BTTestJsonArrayContainsString(BodySections, BodyKey.ToString()));
	}
	TestFalse(TEXT("BehaviorTree BodySections omits BlackboardInline"), BTTestJsonArrayContainsString(BodySections, TEXT("BlackboardInline")));

	const TArray<TSharedPtr<FJsonValue>>* RegionPolicies = nullptr;
	TestTrue(TEXT("BehaviorTree profile exposes RegionPolicies"), Result.Payload->TryGetArrayField(TEXT("RegionPolicies"), RegionPolicies));
	for (const FString& RegionId : {
		TEXT("Body.BlackboardAsset"),
		TEXT("Body.Tree"),
	})
	{
		TestTrue(FString::Printf(TEXT("BehaviorTree RegionPolicies contains %s"), *RegionId), BTTestRegionPoliciesContain(RegionPolicies, RegionId));
	}
	TestFalse(TEXT("BehaviorTree RegionPolicies omit split EditorLayout"), BTTestRegionPoliciesContain(RegionPolicies, TEXT("Body.EditorLayout")));

	return true;
}

namespace BTGraphSourceTests
{
class FPublicGraphFixture;
static FPublicGraphFixture* ActiveFixture = nullptr;

class FPublicGraphFixture
{
public:
	FScopedBehaviorTreeMutationFixture Cleanup;
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	UBlackboardData* Blackboard = nullptr;
	FPublicGraphFixture() { check(!ActiveFixture); ActiveFixture = this; }
	~FPublicGraphFixture() { ActiveFixture = nullptr; }
};

UBehaviorTree* CreateTree()
{
	check(ActiveFixture);
	const FString Target = BTTestMakeUniqueTarget(TEXT("BT_PublicGraph"));
	if (!ActiveFixture->Cleanup.ReserveTarget(Target)) return nullptr;
	return BTTestMakeExistingBehaviorTreeAsset(Target);
}

FAssetDocumentCapabilityResult PublicResult(const FAssetDocumentResult& Result)
{
	FAssetDocumentCapabilityResult Out;
	Out.bSuccess = Result.IsSuccess();
	Out.Message = Result.Message;
	Out.Diagnostics = Result.Diagnostics;
	Out.Payload = Result.Payload;
	if (!Out.bSuccess)
		for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
			Out.Message += FString::Printf(TEXT(" [%s %s: %s]"),
				*Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message);
	return Out;
}

TSharedPtr<FJsonObject> MakeGraphSourceBody(const TSharedPtr<FJsonObject>& Tree);

FString AssetTarget(const FAssetDocumentRegionContext& Context)
{
	return Context.Asset ? Context.Asset->GetOutermost()->GetName() : FString();
}

FAssetDocumentCapabilityResult ValidateTree(const FAssetDocumentRegionContext& Context, const TSharedRef<FJsonObject>& Tree)
{
	FAssetDocumentValidateRequest Request;
	Request.Document = BTTestMakeBehaviorTreeDocument(AssetTarget(Context), MakeGraphSourceBody(Tree));
	return PublicResult(ActiveFixture->Service.Validate(Request));
}

FAssetDocumentCapabilityResult ExtractTree(const FAssetDocumentRegionContext& Context, TSharedRef<FJsonObject>& Out)
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = AssetTarget(Context);
	Request.bDiffOnly = true;
	const FAssetDocumentResult Result = ActiveFixture->Service.Extract(Request);
	if (Result.IsSuccess())
	{
		const auto Body = BTTestGetObjectField(Result.Payload, TEXT("Body"));
		const auto Tree = BTTestGetObjectField(Body, TEXT("Tree"));
		if (!Tree) return FAssetDocumentCapabilityResult::Failure(TEXT("Public Extract omitted Body.Tree"));
		Out = Tree.ToSharedRef();
	}
	return PublicResult(Result);
}

FAssetDocumentCapabilityResult DiffTree(const FAssetDocumentRegionContext& Context,
	const TSharedRef<FJsonObject>& Tree, TArray<TSharedPtr<FJsonValue>>& Entries)
{
	const FAssetDocumentResult Result = ActiveFixture->Service.Diff(BTTestMakeDiffRequest(
		BTTestMakeBehaviorTreeDocument(AssetTarget(Context), MakeGraphSourceBody(Tree))));
	Entries.Reset();
	const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
	if (Result.Payload && Result.Payload->TryGetArrayField(TEXT("changed"), Changed)) Entries = *Changed;
	FAssetDocumentCapabilityResult Out = PublicResult(Result);
	const TArray<TSharedPtr<FJsonValue>>* Failed = nullptr;
	if (Result.Payload && Result.Payload->TryGetArrayField(TEXT("failed"), Failed) && !Failed->IsEmpty())
	{
		Out.bSuccess = false;
		for (const auto& Entry : *Failed)
		{
			const auto Object = Entry->AsObject();
			FAssetDocumentDiagnostic Diagnostic;
			Object->TryGetStringField(TEXT("path"), Diagnostic.Path);
			Object->TryGetStringField(TEXT("code"), Diagnostic.Code);
			Object->TryGetStringField(TEXT("message"), Diagnostic.Message);
			Out.Diagnostics.Add(MoveTemp(Diagnostic));
		}
	}
	return Out;
}

// Contract fixtures intentionally author graph identity, topology, and layout together.
constexpr const TCHAR* GraphGuid = TEXT("0102030405060708090A0B0C0D0E0F10");
constexpr const TCHAR* RootGuid = TEXT("11111111222222223333333344444444");
constexpr const TCHAR* FirstTaskGuid = TEXT("AAAAAAAA11111111BBBBBBBB22222222");
constexpr const TCHAR* SecondTaskGuid = TEXT("CCCCCCCC33333333DDDDDDDD44444444");
constexpr const TCHAR* NestedCompositeGuid = TEXT("55555555666666667777777788888888");
constexpr const TCHAR* RootServiceGuid = TEXT("99999999AAAABBBBCCCCDDDDEEEEFFFF");

TSharedPtr<FJsonObject> MakePosition(double X, double Y)
{
	TSharedPtr<FJsonObject> Position = MakeShared<FJsonObject>();
	Position->SetNumberField(TEXT("X"), X);
	Position->SetNumberField(TEXT("Y"), Y);
	return Position;
}

TSharedPtr<FJsonObject> MakeEditor(double X, double Y)
{
	TSharedPtr<FJsonObject> Editor = MakeShared<FJsonObject>();
	Editor->SetObjectField(TEXT("Position"), MakePosition(X, Y));
	return Editor;
}

TSharedPtr<FJsonObject> MakeGraphNode(
	const FString& Id,
	const FString& ClassPath,
	const FString& NodeName,
	double X,
	double Y,
	TArray<TSharedPtr<FJsonValue>> Children = {})
{
	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetStringField(TEXT("NodeName"), NodeName);
	if (ClassPath.Contains(TEXT("BTComposite_")))
	{
		Properties->SetBoolField(TEXT("bApplyDecoratorScope"), true);
	}
	if (ClassPath.Contains(TEXT("BTTask_")) || ClassPath.Contains(TEXT("BTTaskBlueprint")))
	{
		Properties->SetBoolField(TEXT("bIgnoreRestartSelf"), true);
	}

	TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("Id"), Id);
	Node->SetStringField(TEXT("Class"), ClassPath);
	Node->SetObjectField(TEXT("Properties"), Properties);
	Node->SetArrayField(TEXT("Decorators"), {});
	Node->SetArrayField(TEXT("Services"), {});
	Node->SetArrayField(TEXT("Children"), Children);
	Node->SetObjectField(TEXT("Editor"), MakeEditor(X, Y));
	return Node;
}

TSharedPtr<FJsonObject> MakeGraphService()
{
	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetStringField(TEXT("NodeName"), TEXT("Authored focus service"));
	Properties->SetNumberField(TEXT("Interval"), 0.75);
	Properties->SetNumberField(TEXT("RandomDeviation"), 0.125);
	Properties->SetBoolField(TEXT("bCallTickOnSearchStart"), true);
	Properties->SetBoolField(TEXT("bRestartTimerOnEachActivation"), false);
	Properties->SetObjectField(TEXT("BlackboardKey"), BTTestMakeSelectorProperty(TEXT("TargetActor")));

	TSharedPtr<FJsonObject> Service = MakeShared<FJsonObject>();
	Service->SetStringField(TEXT("Id"), RootServiceGuid);
	Service->SetStringField(TEXT("Class"), TEXT("/Script/AIModule.BTService_DefaultFocus"));
	Service->SetObjectField(TEXT("Properties"), Properties);
	return Service;
}

UBlackboardData* GetGraphSourceBlackboard()
{
	check(ActiveFixture);
	if (ActiveFixture->Blackboard) return ActiveFixture->Blackboard;
	const FString Target = BTTestMakeUniqueTarget(TEXT("BB_PublicGraph"));
	if (!ActiveFixture->Cleanup.ReserveTarget(Target)) return nullptr;
	UBlackboardData* Blackboard = BTTestMakeExistingBlackboardAsset(Target);
	if (!Blackboard) return nullptr;
	BTTestAddObjectBlackboardKey(Blackboard, TEXT("TargetActor"));
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	const FString Filename = FPackageName::LongPackageNameToFilename(Target, FPackageName::GetAssetPackageExtension());
	if (!UPackage::SavePackage(Blackboard->GetOutermost(), Blackboard, *Filename, Args)) return nullptr;
	ActiveFixture->Blackboard = Blackboard;
	return Blackboard;
}

TSharedPtr<FJsonObject> MakeGraphSourceTree(
	const FString& FirstNodeName = TEXT("Shared visible label"),
	double FirstX = 100.0,
	double SecondX = 300.0,
	const FString& FirstClass = TEXT("/Script/AIModule.BTTask_Wait"))
{
	TArray<TSharedPtr<FJsonValue>> Children;
	Children.Add(BTTestMakeObjectValue(MakeGraphNode(
		FirstTaskGuid,
		FirstClass,
		FirstNodeName,
		FirstX,
		300.0)));
	Children.Add(BTTestMakeObjectValue(MakeGraphNode(
		SecondTaskGuid,
		TEXT("/Script/AIModule.BTTask_Wait"),
		TEXT("Shared visible label"),
		SecondX,
		300.0)));

	TSharedPtr<FJsonObject> Tree = MakeShared<FJsonObject>();
	Tree->SetStringField(TEXT("GraphGuid"), GraphGuid);
	TSharedPtr<FJsonObject> Root = MakeGraphNode(
		RootGuid,
		TEXT("/Script/AIModule.BTComposite_Selector"),
		TEXT("Authored selector label"),
		200.0,
		0.0,
		Children);
	Root->SetArrayField(TEXT("Services"), {BTTestMakeObjectValue(MakeGraphService())});
	Tree->SetObjectField(TEXT("Root"), Root);
	Tree->SetArrayField(TEXT("Comments"), {});
	return Tree;
}

FAssetDocumentRegionContext MakeTreeContext(UBehaviorTree* BehaviorTree)
{
	FAssetDocumentRegionContext Context;
	Context.Asset = BehaviorTree;
	Context.AssetClass = UBehaviorTree::StaticClass();
	Context.BodyPath = TEXT("Body.Tree");
	Context.JsonPointer = TEXT("/Body/Tree");
	return Context;
}

FAssetDocumentCapabilityResult ApplyTree(UBehaviorTree* BehaviorTree, const TSharedPtr<FJsonObject>& Tree, bool& bOutChanged)
{
	bOutChanged = false;
	if (!BehaviorTree || !Tree) return FAssetDocumentCapabilityResult::Failure(TEXT("Missing public graph fixture"));
	const FString Target = BehaviorTree->GetOutermost()->GetName();
	const FString SidecarPath = FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(Target);
	const auto Document = BTTestMakeBehaviorTreeDocument(Target, MakeGraphSourceBody(Tree));
	FString Error;
	{
		FAssetDocumentEditorSync::FScopedSidecarWrite Guard(SidecarPath);
		if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(SidecarPath), true)
			|| !FAssetDocumentSidecar::WriteJsonFile(SidecarPath, Document, Error))
			return FAssetDocumentCapabilityResult::Failure(Error);
	}
	TSharedRef<FJsonObject> Before = MakeShared<FJsonObject>();
	const bool bHadBefore = ExtractTree(MakeTreeContext(BehaviorTree), Before).bSuccess;
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = SidecarPath;
	Request.bSaveAsset = true;
	const FAssetDocumentResult Result = ActiveFixture->Service.ApplyFile(Request);
	if (Result.IsSuccess())
	{
		TSharedRef<FJsonObject> After = MakeShared<FJsonObject>();
		if (!ExtractTree(MakeTreeContext(BehaviorTree), After).bSuccess)
			return FAssetDocumentCapabilityResult::Failure(TEXT("Public ApplyFile succeeded but Extract failed"));
		bOutChanged = !bHadBefore || FAssetDocumentJsonRegionUtils::JsonValueToComparableString(BTTestMakeObjectValue(Before))
			!= FAssetDocumentJsonRegionUtils::JsonValueToComparableString(BTTestMakeObjectValue(After));
	}
	return PublicResult(Result);
}

UBehaviorTreeGraphNode* FindGraphNodeByGuid(UBehaviorTreeGraph* Graph, const FString& GuidString)
{
	if (!Graph)
	{
		return nullptr;
	}
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		UBehaviorTreeGraphNode* BehaviorNode = Cast<UBehaviorTreeGraphNode>(Node);
		if (BehaviorNode && BehaviorNode->NodeGuid.ToString(EGuidFormats::Digits).Equals(GuidString, ESearchCase::IgnoreCase))
		{
			return BehaviorNode;
		}
		if (BehaviorNode)
		{
			for (UBehaviorTreeGraphNode* Service : BehaviorNode->Services)
			{
				if (Service && Service->NodeGuid.ToString(EGuidFormats::Digits).Equals(GuidString, ESearchCase::IgnoreCase))
				{
					return Service;
				}
			}
		}
	}
	return nullptr;
}

UEdGraphPin* FindGraphPin(UBehaviorTreeGraphNode* Node, EEdGraphPinDirection Direction)
{
	if (!Node)
	{
		return nullptr;
	}
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == Direction)
		{
			return Pin;
		}
	}
	return nullptr;
}

TSharedPtr<FJsonObject> FindExtractedNodeById(const TSharedPtr<FJsonObject>& Node, const FString& Id)
{
	if (!Node.IsValid())
	{
		return nullptr;
	}
	FString CandidateId;
	if (Node->TryGetStringField(TEXT("Id"), CandidateId) && CandidateId == Id)
	{
		return Node;
	}

	const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
	if (Node->TryGetArrayField(TEXT("Children"), Children) && Children)
	{
		for (const TSharedPtr<FJsonValue>& ChildValue : *Children)
		{
			TSharedPtr<FJsonObject> Found = FindExtractedNodeById(ChildValue.IsValid() ? ChildValue->AsObject() : nullptr, Id);
			if (Found.IsValid())
			{
				return Found;
			}
		}
	}
	return nullptr;
}

TSharedPtr<FJsonObject> ExtractTree(FAutomationTestBase& Test, UBehaviorTree* BehaviorTree)
{
	FAssetDocumentRegionContext Context = MakeTreeContext(BehaviorTree);
	TSharedRef<FJsonObject> Extracted = MakeShared<FJsonObject>();
	const FAssetDocumentCapabilityResult Result = BTGraphSourceTests::ExtractTree(Context, Extracted);
	Test.TestTrue(TEXT("graph-source extraction succeeds"), Result.bSuccess);
	if (!Result.bSuccess)
	{
		Test.AddError(Result.Message);
		return nullptr;
	}
	return Extracted;
}

TSharedPtr<FJsonObject> MakeGraphSourceBody(const TSharedPtr<FJsonObject>& Tree)
{
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetObjectField(TEXT("BlackboardAsset"), BTTestMakeAssetRef(GetGraphSourceBlackboard()->GetPathName()));
	Body->SetObjectField(TEXT("Tree"), Tree);
	return Body;
}

void RemoveEditorLayoutRecursively(const TSharedPtr<FJsonObject>& Node)
{
	if (!Node.IsValid())
	{
		return;
	}
	Node->RemoveField(TEXT("Editor"));
	const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
	if (Node->TryGetArrayField(TEXT("Children"), Children) && Children)
	{
		for (const TSharedPtr<FJsonValue>& Child : *Children)
		{
			RemoveEditorLayoutRecursively(Child.IsValid() ? Child->AsObject() : nullptr);
		}
	}
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeGraphSourceIdentityTest,
	"UEAssetDoc.BehaviorTree.GraphSource.IdentityAndNodeName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeGraphSourceIdentityTest::RunTest(const FString&)
{
	BTGraphSourceTests::FPublicGraphFixture PublicGraphFixture;
	using namespace BTGraphSourceTests;
	UBehaviorTree* BehaviorTree = BTGraphSourceTests::CreateTree();
	bool bChanged = false;
	FAssetDocumentCapabilityResult Result = ApplyTree(BehaviorTree, MakeGraphSourceTree(), bChanged);
	TestTrue(TEXT("graph-source apply succeeds"), Result.bSuccess);
	if (!Result.bSuccess)
	{
		AddError(Result.Message);
		return false;
	}
	TestTrue(TEXT("initial graph-source apply reports change"), bChanged);

	UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(BehaviorTree->BTGraph);
	TestNotNull(TEXT("UBehaviorTree owns a production UBehaviorTreeGraph"), Graph);
	TestEqual(TEXT("authored GraphGuid is preserved"), Graph ? Graph->GraphGuid.ToString(EGuidFormats::Digits) : FString(), FString(GraphGuid));

	UBehaviorTreeGraphNode* RootNode = FindGraphNodeByGuid(Graph, RootGuid);
	UBehaviorTreeGraphNode* FirstTask = FindGraphNodeByGuid(Graph, FirstTaskGuid);
	UBehaviorTreeGraphNode* SecondTask = FindGraphNodeByGuid(Graph, SecondTaskGuid);
	UBehaviorTreeGraphNode* RootService = FindGraphNodeByGuid(Graph, RootServiceGuid);
	TestTrue(TEXT("explicit root NodeGuid becomes graph wrapper identity"), RootNode && RootNode->NodeGuid.ToString(EGuidFormats::Digits) == RootGuid);
	TestTrue(TEXT("explicit first task NodeGuid becomes graph wrapper identity"), FirstTask && FirstTask->NodeGuid.ToString(EGuidFormats::Digits) == FirstTaskGuid);
	TestTrue(TEXT("explicit second task NodeGuid becomes graph wrapper identity"), SecondTask && SecondTask->NodeGuid.ToString(EGuidFormats::Digits) == SecondTaskGuid);
	TestTrue(TEXT("explicit service NodeGuid becomes graph subnode identity"), RootService && RootService->NodeGuid.ToString(EGuidFormats::Digits) == RootServiceGuid);
	TestTrue(TEXT("duplicate visible labels do not collapse graph identity"), FirstTask && SecondTask && FirstTask != SecondTask);
	TestEqual(TEXT("NodeName is an independent authored property"), FirstTask && FirstTask->NodeInstance ? CastChecked<UBTNode>(FirstTask->NodeInstance)->NodeName : FString(), FString(TEXT("Shared visible label")));

	TSharedPtr<FJsonObject> Extracted = ExtractTree(*this, BehaviorTree);
	TSharedPtr<FJsonObject> ExtractedRoot = Extracted.IsValid() ? BTTestGetObjectField(Extracted, TEXT("Root")) : nullptr;
	TSharedPtr<FJsonObject> ExtractedFirst = FindExtractedNodeById(ExtractedRoot, FirstTaskGuid);
	TestTrue(TEXT("extract reads first NodeGuid from graph"), ExtractedFirst.IsValid());
	TestEqual(TEXT("extract keeps canonical graph identity"), ExtractedFirst.IsValid() ? ExtractedFirst->GetStringField(TEXT("Id")) : FString(), FString(FirstTaskGuid));
	const TSharedPtr<FJsonObject> ExtractedRootProperties = ExtractedRoot.IsValid() ? BTTestGetObjectField(ExtractedRoot, TEXT("Properties")) : nullptr;
	TestTrue(TEXT("base composite editable property round-trips"), ExtractedRootProperties.IsValid() && ExtractedRootProperties->GetBoolField(TEXT("bApplyDecoratorScope")));
	const TSharedPtr<FJsonObject> ExtractedTaskProperties = ExtractedFirst.IsValid() ? BTTestGetObjectField(ExtractedFirst, TEXT("Properties")) : nullptr;
	TestTrue(TEXT("base task editable property round-trips"), ExtractedTaskProperties.IsValid() && ExtractedTaskProperties->GetBoolField(TEXT("bIgnoreRestartSelf")));
	const TArray<TSharedPtr<FJsonValue>>* ExtractedServices = nullptr;
	TestTrue(TEXT("root service order and identity extract from graph subnodes"), ExtractedRoot.IsValid() && ExtractedRoot->TryGetArrayField(TEXT("Services"), ExtractedServices) && ExtractedServices && ExtractedServices->Num() == 1 && (*ExtractedServices)[0]->AsObject()->GetStringField(TEXT("Id")) == RootServiceGuid);
	if (ExtractedServices && ExtractedServices->Num() == 1)
	{
		const TSharedPtr<FJsonObject> ServiceProperties = BTTestGetObjectField((*ExtractedServices)[0]->AsObject(), TEXT("Properties"));
		TestEqual(TEXT("concrete service float property round-trips"), ServiceProperties.IsValid() ? ServiceProperties->GetNumberField(TEXT("Interval")) : -1.0, 0.75);
		TestTrue(TEXT("concrete service bool property round-trips"), ServiceProperties.IsValid() && ServiceProperties->GetBoolField(TEXT("bCallTickOnSearchStart")));
	}

	TSharedPtr<FJsonObject> UpdatedTree = MakeGraphSourceTree(TEXT("Renamed without identity change"));
	Result = ApplyTree(BehaviorTree, UpdatedTree, bChanged);
	TestTrue(TEXT("graph-source update succeeds"), Result.bSuccess);
	TestTrue(TEXT("NodeName-only update reports change"), bChanged);
	Graph = Cast<UBehaviorTreeGraph>(BehaviorTree->BTGraph);
	FirstTask = FindGraphNodeByGuid(Graph, FirstTaskGuid);
	TestEqual(TEXT("NodeGuid survives update"), FirstTask ? FirstTask->NodeGuid.ToString(EGuidFormats::Digits) : FString(), FString(FirstTaskGuid));
	TestEqual(TEXT("NodeName update does not rewrite identity"), FirstTask && FirstTask->NodeInstance ? CastChecked<UBTNode>(FirstTask->NodeInstance)->NodeName : FString(), FString(TEXT("Renamed without identity change")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeGraphSourceTopologyAndOrderTest,
	"UEAssetDoc.BehaviorTree.GraphSource.TopologyOrderAndRuntimeMirror",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeGraphSourceTopologyAndOrderTest::RunTest(const FString&)
{
	BTGraphSourceTests::FPublicGraphFixture PublicGraphFixture;
	using namespace BTGraphSourceTests;
	UBehaviorTree* BehaviorTree = BTGraphSourceTests::CreateTree();
	bool bChanged = false;
	const FAssetDocumentCapabilityResult ApplyResult = ApplyTree(BehaviorTree, MakeGraphSourceTree(), bChanged);
	TestTrue(TEXT("ordered graph apply succeeds"), ApplyResult.bSuccess);
	if (!ApplyResult.bSuccess)
	{
		AddError(ApplyResult.Message);
		return false;
	}

	UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(BehaviorTree->BTGraph);
	UBehaviorTreeGraphNode* RootGraphNode = FindGraphNodeByGuid(Graph, RootGuid);
	UBehaviorTreeGraphNode* FirstGraphTask = FindGraphNodeByGuid(Graph, FirstTaskGuid);
	UBehaviorTreeGraphNode* SecondGraphTask = FindGraphNodeByGuid(Graph, SecondTaskGuid);
	UBTCompositeNode* RuntimeRoot = BehaviorTree->RootNode;
	TestTrue(TEXT("semantic child order has strictly increasing X"), FirstGraphTask && SecondGraphTask && FirstGraphTask->NodePosX < SecondGraphTask->NodePosX);
	TestTrue(TEXT("standard UpdateAsset sets runtime root from graph wrapper"), RootGraphNode && RuntimeRoot == RootGraphNode->NodeInstance);
	TestEqual(TEXT("standard UpdateAsset creates two runtime children"), RuntimeRoot ? RuntimeRoot->Children.Num() : -1, 2);
	TestTrue(TEXT("standard UpdateAsset mirrors ordered service subnodes"), RuntimeRoot && RuntimeRoot->Services.Num() == 1 && RootGraphNode->Services.Num() == 1 && RuntimeRoot->Services[0] == RootGraphNode->Services[0]->NodeInstance);
	if (RuntimeRoot && RuntimeRoot->Children.Num() == 2)
	{
		TestTrue(TEXT("runtime first child mirrors graph X order"), RuntimeRoot->Children[0].ChildTask == FirstGraphTask->NodeInstance);
		TestTrue(TEXT("runtime second child mirrors graph X order"), RuntimeRoot->Children[1].ChildTask == SecondGraphTask->NodeInstance);

		RuntimeRoot->Children.Swap(0, 1);
		TSharedPtr<FJsonObject> Extracted = ExtractTree(*this, BehaviorTree);
		TSharedPtr<FJsonObject> ExtractedRoot = Extracted.IsValid() ? BTTestGetObjectField(Extracted, TEXT("Root")) : nullptr;
		const TArray<TSharedPtr<FJsonValue>>* ExtractedChildren = nullptr;
		TestTrue(TEXT("extract has graph children"), ExtractedRoot.IsValid() && ExtractedRoot->TryGetArrayField(TEXT("Children"), ExtractedChildren) && ExtractedChildren && ExtractedChildren->Num() == 2);
		if (ExtractedChildren && ExtractedChildren->Num() == 2)
		{
			TestEqual(TEXT("extract ignores corrupted runtime order and reads graph first"), (*ExtractedChildren)[0]->AsObject()->GetStringField(TEXT("Id")), FString(FirstTaskGuid));
			TestEqual(TEXT("extract ignores corrupted runtime order and reads graph second"), (*ExtractedChildren)[1]->AsObject()->GetStringField(TEXT("Id")), FString(SecondTaskGuid));
		}

		Graph->UpdateAsset();
		RuntimeRoot = BehaviorTree->RootNode;
		TestTrue(TEXT("standard graph rebuild restores runtime first child"), RuntimeRoot && RuntimeRoot->Children[0].ChildTask == FirstGraphTask->NodeInstance);
		TestTrue(TEXT("standard graph rebuild restores runtime second child"), RuntimeRoot && RuntimeRoot->Children[1].ChildTask == SecondGraphTask->NodeInstance);
	}

	TSharedPtr<FJsonObject> GeneratedLayoutTree = MakeGraphSourceTree();
	RemoveEditorLayoutRecursively(BTTestGetObjectField(GeneratedLayoutTree, TEXT("Root")));
	UBehaviorTree* GeneratedLayoutAsset = BTGraphSourceTests::CreateTree();
	const FAssetDocumentCapabilityResult GeneratedLayoutResult = ApplyTree(GeneratedLayoutAsset, GeneratedLayoutTree, bChanged);
	TestTrue(TEXT("omitted layout generates deterministic graph coordinates"), GeneratedLayoutResult.bSuccess);
	UBehaviorTreeGraph* GeneratedLayoutGraph = Cast<UBehaviorTreeGraph>(GeneratedLayoutAsset->BTGraph);
	UBehaviorTreeGraphNode* GeneratedFirst = FindGraphNodeByGuid(GeneratedLayoutGraph, FirstTaskGuid);
	UBehaviorTreeGraphNode* GeneratedSecond = FindGraphNodeByGuid(GeneratedLayoutGraph, SecondTaskGuid);
	TestTrue(TEXT("generated sibling coordinates preserve semantic order"), GeneratedFirst && GeneratedSecond && GeneratedFirst->NodePosX < GeneratedSecond->NodePosX);
	TSharedPtr<FJsonObject> GeneratedExtract = ExtractTree(*this, GeneratedLayoutAsset);
	TSharedPtr<FJsonObject> GeneratedRoot = GeneratedExtract.IsValid() ? BTTestGetObjectField(GeneratedExtract, TEXT("Root")) : nullptr;
	const TArray<TSharedPtr<FJsonValue>>* GeneratedChildren = nullptr;
	TestTrue(TEXT("generated layout is emitted canonically"), GeneratedRoot.IsValid() && GeneratedRoot->TryGetArrayField(TEXT("Children"), GeneratedChildren) && GeneratedChildren && GeneratedChildren->Num() == 2 && BTTestGetObjectField((*GeneratedChildren)[0]->AsObject(), TEXT("Editor")).IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeGraphSourceValidationTest,
	"UEAssetDoc.BehaviorTree.GraphSource.Validation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeGraphSourceValidationTest::RunTest(const FString&)
{
	BTGraphSourceTests::FPublicGraphFixture PublicGraphFixture;
	using namespace BTGraphSourceTests;
	UBehaviorTree* BehaviorTree = BTGraphSourceTests::CreateTree();
	FAssetDocumentRegionContext Context = MakeTreeContext(BehaviorTree);

	TSharedPtr<FJsonObject> NameIdTree = MakeGraphSourceTree();
	BTTestGetObjectField(NameIdTree, TEXT("Root"))->SetStringField(TEXT("Id"), TEXT("RootByName"));
	FAssetDocumentCapabilityResult Result = BTGraphSourceTests::ValidateTree(Context, NameIdTree.ToSharedRef());
	TestFalse(TEXT("legacy name identity is rejected"), Result.bSuccess);
	TestTrue(TEXT("legacy name identity diagnostic is exact"), BTTestHasDiagnostic(Result, TEXT("/Body/Tree/Root/Id"), TEXT("InvalidBehaviorTreeNodeGuid")));

	TSharedPtr<FJsonObject> DuplicateGuidTree = MakeGraphSourceTree();
	TSharedPtr<FJsonObject> DuplicateRoot = BTTestGetObjectField(DuplicateGuidTree, TEXT("Root"));
	const TArray<TSharedPtr<FJsonValue>>* DuplicateChildren = nullptr;
	if (DuplicateRoot->TryGetArrayField(TEXT("Children"), DuplicateChildren) && DuplicateChildren && DuplicateChildren->Num() == 2)
	{
		(*DuplicateChildren)[1]->AsObject()->SetStringField(TEXT("Id"), FirstTaskGuid);
	}
	Result = BTGraphSourceTests::ValidateTree(Context, DuplicateGuidTree.ToSharedRef());
	TestFalse(TEXT("duplicate NodeGuid is rejected"), Result.bSuccess);
	TestTrue(TEXT("duplicate NodeGuid diagnostic is exact"), BTTestHasDiagnostic(Result, TEXT("/Body/Tree/Root/Children/1/Id"), TEXT("DuplicateBehaviorTreeNodeGuid")));

	TSharedPtr<FJsonObject> OrderConflictTree = MakeGraphSourceTree(TEXT("first"), 400.0, 100.0);
	Result = BTGraphSourceTests::ValidateTree(Context, OrderConflictTree.ToSharedRef());
	TestFalse(TEXT("semantic child order conflicting with X order is rejected"), Result.bSuccess);
	TestTrue(TEXT("layout order conflict diagnostic is exact"), BTTestHasDiagnostic(Result, TEXT("/Body/Tree/Root/Children/1/Editor/Position/X"), TEXT("BehaviorTreeLayoutOrderConflict")));

	TSharedPtr<FJsonObject> DuplicateXTree = MakeGraphSourceTree(TEXT("first"), 100.0, 100.0);
	Result = BTGraphSourceTests::ValidateTree(Context, DuplicateXTree.ToSharedRef());
	TestFalse(TEXT("sibling X tie is rejected"), Result.bSuccess);
	TestTrue(TEXT("sibling X tie diagnostic is exact"), BTTestHasDiagnostic(Result, TEXT("/Body/Tree/Root/Children/1/Editor/Position/X"), TEXT("DuplicateBehaviorTreeSiblingCoordinate")));

	TSharedPtr<FJsonObject> WrongRootClassTree = MakeGraphSourceTree();
	BTTestGetObjectField(WrongRootClassTree, TEXT("Root"))->SetStringField(TEXT("Class"), TEXT("/Script/AIModule.BTTask_Wait"));
	Result = BTGraphSourceTests::ValidateTree(Context, WrongRootClassTree.ToSharedRef());
	TestFalse(TEXT("task class cannot be the root composite"), Result.bSuccess);
	TestTrue(TEXT("root class legality diagnostic is exact"), BTTestHasDiagnostic(Result, TEXT("/Body/Tree/Root/Class"), TEXT("InvalidBehaviorTreeNodeClass")));

	TSharedPtr<FJsonObject> AbstractClassTree = MakeGraphSourceTree(TEXT("first"), 100.0, 300.0, TEXT("/Script/AIModule.BTTaskNode"));
	Result = BTGraphSourceTests::ValidateTree(Context, AbstractClassTree.ToSharedRef());
	TestFalse(TEXT("abstract task class is rejected"), Result.bSuccess);
	TestTrue(TEXT("abstract class diagnostic is exact"), BTTestHasDiagnostic(Result, TEXT("/Body/Tree/Root/Children/0/Class"), TEXT("AbstractBehaviorTreeNodeClass")));

	UBehaviorTree* MultipleRootsTree = BTGraphSourceTests::CreateTree();
	bool bChanged = false;
	Result = ApplyTree(MultipleRootsTree, MakeGraphSourceTree(), bChanged);
	TestTrue(TEXT("multiple-root graph fixture applies before corruption"), Result.bSuccess);
	if (UBehaviorTreeGraph* MultipleRootsGraph = Cast<UBehaviorTreeGraph>(MultipleRootsTree->BTGraph))
	{
		UBehaviorTreeGraphNode_Root* ExtraRoot = NewObject<UBehaviorTreeGraphNode_Root>(MultipleRootsGraph);
		MultipleRootsGraph->AddNode(ExtraRoot, false, false);
		ExtraRoot->CreateNewGuid();
		ExtraRoot->AllocateDefaultPins();
		TSharedRef<FJsonObject> Ignored = MakeShared<FJsonObject>();
		Result = BTGraphSourceTests::ExtractTree(MakeTreeContext(MultipleRootsTree), Ignored);
		TestFalse(TEXT("extract rejects a graph with multiple synthetic roots"), Result.bSuccess);
		TestTrue(TEXT("multiple-root graph diagnostic is exact"), BTTestHasDiagnostic(Result, TEXT("/Body/Tree/Root"), TEXT("MultipleBehaviorTreeRoots")));
	}

	UBehaviorTree* OrphanTree = BTGraphSourceTests::CreateTree();
	Result = ApplyTree(OrphanTree, MakeGraphSourceTree(), bChanged);
	TestTrue(TEXT("orphan graph fixture applies before corruption"), Result.bSuccess);
	if (UBehaviorTreeGraph* OrphanGraph = Cast<UBehaviorTreeGraph>(OrphanTree->BTGraph))
	{
		UBehaviorTreeGraphNode_Task* Orphan = NewObject<UBehaviorTreeGraphNode_Task>(OrphanGraph);
		Orphan->ClassData = FGraphNodeClassData(UBTTask_Wait::StaticClass(), FString());
		OrphanGraph->AddNode(Orphan, false, false);
		Orphan->CreateNewGuid();
		Orphan->PostPlacedNewNode();
		Orphan->AllocateDefaultPins();
		TSharedRef<FJsonObject> Ignored = MakeShared<FJsonObject>();
		Result = BTGraphSourceTests::ExtractTree(MakeTreeContext(OrphanTree), Ignored);
		TestFalse(TEXT("extract rejects an unreachable graph node"), Result.bSuccess);
		TestTrue(TEXT("unreachable graph diagnostic is exact"), BTTestHasDiagnostic(Result, TEXT("/Body/Tree/Nodes"), TEXT("UnreachableBehaviorTreeGraphNode")));
	}

	UBehaviorTree* CycleTree = BTGraphSourceTests::CreateTree();
	Result = ApplyTree(CycleTree, MakeGraphSourceTree(), bChanged);
	TestTrue(TEXT("cycle graph fixture applies before corruption"), Result.bSuccess);
	if (UBehaviorTreeGraph* CycleGraph = Cast<UBehaviorTreeGraph>(CycleTree->BTGraph))
	{
		UBehaviorTreeGraphNode* RootGraphNode = FindGraphNodeByGuid(CycleGraph, RootGuid);
		UEdGraphPin* RootOutput = FindGraphPin(RootGraphNode, EGPD_Output);
		UEdGraphPin* RootInput = FindGraphPin(RootGraphNode, EGPD_Input);
		if (RootOutput && RootInput)
		{
			RootGraphNode->NodePosX = -100;
			RootOutput->MakeLinkTo(RootInput);
		}
		TSharedRef<FJsonObject> Ignored = MakeShared<FJsonObject>();
		Result = BTGraphSourceTests::ExtractTree(MakeTreeContext(CycleTree), Ignored);
		TestFalse(TEXT("extract rejects a graph cycle"), Result.bSuccess);
		TestTrue(TEXT("cycle graph diagnostic is exact"), BTTestHasDiagnostic(Result, TEXT("/Body/Tree/Root/Children/0"), TEXT("BehaviorTreeGraphCycle")));
	}

	UBehaviorTree* IllegalPinTree = BTGraphSourceTests::CreateTree();
	Result = ApplyTree(IllegalPinTree, MakeGraphSourceTree(), bChanged);
	TestTrue(TEXT("illegal-pin graph fixture applies before corruption"), Result.bSuccess);
	if (UBehaviorTreeGraph* IllegalPinGraph = Cast<UBehaviorTreeGraph>(IllegalPinTree->BTGraph))
	{
		UEdGraphPin* TaskInput = FindGraphPin(FindGraphNodeByGuid(IllegalPinGraph, FirstTaskGuid), EGPD_Input);
		if (TaskInput)
		{
			TaskInput->PinType.PinCategory = FName(TEXT("SingleTask"));
		}
		TSharedRef<FJsonObject> Ignored = MakeShared<FJsonObject>();
		Result = BTGraphSourceTests::ExtractTree(MakeTreeContext(IllegalPinTree), Ignored);
		TestFalse(TEXT("extract rejects a schema-illegal pin connection"), Result.bSuccess);
		TestTrue(TEXT("illegal pin diagnostic is exact"), BTTestHasDiagnostic(Result, TEXT("/Body/Tree/Root/Children/0"), TEXT("InvalidBehaviorTreeGraphConnection")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeGraphSourceDynamicBlueprintClassTest,
	"UEAssetDoc.BehaviorTree.GraphSource.DynamicBlueprintClass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeGraphSourceDynamicBlueprintClassTest::RunTest(const FString&)
{
	BTGraphSourceTests::FPublicGraphFixture PublicGraphFixture;
	using namespace BTGraphSourceTests;
	const FString PackageName = BTTestMakeUniqueTarget(TEXT("BTTaskBlueprint_GraphSource"));
	if (!PublicGraphFixture.Cleanup.ReserveTarget(PackageName)) return false;
	UPackage* BlueprintPackage = CreatePackage(*PackageName);
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		UBTTask_BlueprintBase::StaticClass(),
		BlueprintPackage,
		FName(*FPackageName::GetLongPackageAssetName(PackageName)),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		TEXT("AssetDocumentBehaviorTreeGraphSource"));
	TestNotNull(TEXT("dynamic Blueprint task class fixture is created"), Blueprint);
	if (!Blueprint)
	{
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	UClass* GeneratedClass = Blueprint->GeneratedClass;
	TestTrue(TEXT("generated Blueprint class is concrete UBTTaskNode"), GeneratedClass && GeneratedClass->IsChildOf(UBTTaskNode::StaticClass()) && !GeneratedClass->HasAnyClassFlags(CLASS_Abstract));
	if (!GeneratedClass)
	{
		return false;
	}

	FSavePackageArgs BlueprintSave;
	BlueprintSave.TopLevelFlags = RF_Public | RF_Standalone;
	BlueprintSave.SaveFlags = SAVE_NoError;
	const FString BlueprintFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	if (!UPackage::SavePackage(BlueprintPackage, Blueprint, *BlueprintFile, BlueprintSave)) return false;

	TSharedPtr<FJsonObject> Tree = MakeGraphSourceTree(TEXT("Blueprint task label"), 100.0, 300.0, GeneratedClass->GetPathName());
	UBehaviorTree* BehaviorTree = BTGraphSourceTests::CreateTree();
	bool bChanged = false;
	const FAssetDocumentCapabilityResult Result = ApplyTree(BehaviorTree, Tree, bChanged);
	TestTrue(TEXT("dynamically loaded Blueprint node class applies"), Result.bSuccess);
	if (!Result.bSuccess)
	{
		AddError(Result.Message);
		return false;
	}
	UBehaviorTreeGraphNode* GraphNode = FindGraphNodeByGuid(Cast<UBehaviorTreeGraph>(BehaviorTree->BTGraph), FirstTaskGuid);
	TestTrue(TEXT("graph wrapper owns generated Blueprint NodeInstance"), GraphNode && GraphNode->NodeInstance && GraphNode->NodeInstance->GetClass() == GeneratedClass);
	TSharedPtr<FJsonObject> Extracted = ExtractTree(*this, BehaviorTree);
	TSharedPtr<FJsonObject> ExtractedNode = FindExtractedNodeById(Extracted.IsValid() ? BTTestGetObjectField(Extracted, TEXT("Root")) : nullptr, FirstTaskGuid);
	TestEqual(TEXT("dynamic class extracts as canonical concrete path"), ExtractedNode.IsValid() ? ExtractedNode->GetStringField(TEXT("Class")) : FString(), GeneratedClass->GetPathName());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeGraphSourceSaveReloadTest,
	"UEAssetDoc.BehaviorTree.GraphSource.SaveReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeGraphSourceSaveReloadTest::RunTest(const FString&)
{
	BTGraphSourceTests::FPublicGraphFixture PublicGraphFixture;
	using namespace BTGraphSourceTests;
	const FString Target = BTTestMakeUniqueTarget(TEXT("BT_GraphSource_Reload"));
	TSharedPtr<FJsonObject> Document = BTTestMakeBehaviorTreeDocument(Target, MakeGraphSourceBody(MakeGraphSourceTree()));
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	FScopedBehaviorTreeMutationFixture Fixture;
	const FAssetDocumentResult ApplyResult = Fixture.Apply(Service, Target, Document);
	TestTrue(TEXT("saved graph-source apply succeeds"), ApplyResult.IsSuccess());
	if (!ApplyResult.IsSuccess())
	{
		AddError(ApplyResult.Message);
		return false;
	}

	UBehaviorTree* BeforeReload = BTTestLoadBehaviorTreeForTarget(Target);
	TestNotNull(TEXT("saved BehaviorTree loads before explicit unload"), BeforeReload);
	UPackage* Package = BeforeReload ? BeforeReload->GetOutermost() : nullptr;
	TestNotNull(TEXT("saved BehaviorTree package exists"), Package);
	if (!Package)
	{
		return false;
	}

	const bool bUnloaded = UPackageTools::UnloadPackages({Package});
	TestTrue(TEXT("saved BehaviorTree package unloads"), bUnloaded);
	UBehaviorTree* Reloaded = BTTestLoadBehaviorTreeForTarget(Target);
	TestNotNull(TEXT("BehaviorTree fresh reload succeeds"), Reloaded);
	UBehaviorTreeGraph* ReloadedGraph = Reloaded ? Cast<UBehaviorTreeGraph>(Reloaded->BTGraph) : nullptr;
	TestEqual(TEXT("GraphGuid survives save and fresh reload"), ReloadedGraph ? ReloadedGraph->GraphGuid.ToString(EGuidFormats::Digits) : FString(), FString(GraphGuid));
	TestNotNull(TEXT("root NodeGuid survives save and fresh reload"), FindGraphNodeByGuid(ReloadedGraph, RootGuid));
	TestNotNull(TEXT("first task NodeGuid survives save and fresh reload"), FindGraphNodeByGuid(ReloadedGraph, FirstTaskGuid));
	TestNotNull(TEXT("second task NodeGuid survives save and fresh reload"), FindGraphNodeByGuid(ReloadedGraph, SecondTaskGuid));
	TestNotNull(TEXT("service NodeGuid survives save and fresh reload"), FindGraphNodeByGuid(ReloadedGraph, RootServiceGuid));
	return true;
}

namespace BTGraphShapeTests
{
constexpr const TCHAR* RootDecoratorGuidA = TEXT("10101010202020203030303040404040");
constexpr const TCHAR* RootDecoratorGuidB = TEXT("11112222333344445555666677778888");
constexpr const TCHAR* EdgeDecoratorGuidA = TEXT("12121212343434345656565678787878");
constexpr const TCHAR* EdgeDecoratorGuidB = TEXT("13131313353535355757575779797979");
constexpr const TCHAR* RootServiceGuidA = TEXT("1414141436363636585858587A7A7A7A");
constexpr const TCHAR* RootServiceGuidB = TEXT("1515151537373737595959597B7B7B7B");
constexpr const TCHAR* CompositeDecoratorGuid = TEXT("16161616383838385A5A5A5A7C7C7C7C");
constexpr const TCHAR* BoundGraphGuid = TEXT("17171717393939395B5B5B5B7D7D7D7D");
constexpr const TCHAR* SinkGuid = TEXT("181818183A3A3A3A5C5C5C5C7E7E7E7E");
constexpr const TCHAR* LogicGuidA = TEXT("191919193B3B3B3B5D5D5D5D7F7F7F7F");
constexpr const TCHAR* LogicGuidB = TEXT("202020204A4A4A4A6C6C6C6C8E8E8E8E");
constexpr const TCHAR* TestGuidA = TEXT("212121214B4B4B4B6D6D6D6D8F8F8F8F");
constexpr const TCHAR* TestGuidB = TEXT("222222224C4C4C4C6E6E6E6E90909090");
constexpr const TCHAR* CommentGuid = TEXT("232323234D4D4D4D6F6F6F6F91919191");
constexpr const TCHAR* SimpleParallelGuid = TEXT("242424244E4E4E4E7070707092929292");
constexpr const TCHAR* ForegroundTaskGuid = TEXT("252525254F4F4F4F7171717193939393");
constexpr const TCHAR* BackgroundCompositeGuid = TEXT("262626265A5A5A5A7272727294949494");
constexpr const TCHAR* BackgroundTaskGuid = TEXT("272727275B5B5B5B7373737395959595");
constexpr const TCHAR* ProjectCompositeGuid = TEXT("282828285C5C5C5C7474747496969696");
constexpr const TCHAR* ProjectTaskGuid = TEXT("292929295D5D5D5D7575757597979797");
constexpr const TCHAR* ProjectServiceGuid = TEXT("303030306A6A6A6A7676767698989898");
constexpr const TCHAR* TestGuidC = TEXT("313131316B6B6B6B7777777799999999");
constexpr const TCHAR* CommentGuidB = TEXT("323232326C6C6C6C787878789A9A9A9A");
constexpr const TCHAR* ReplacementGraphGuid = TEXT("333333336D6D6D6D797979799B9B9B9B");

TSharedPtr<FJsonObject> MakeEditor(
	double X,
	double Y,
	const FString& NodeComment,
	bool bPinned,
	bool bVisible)
{
	TSharedPtr<FJsonObject> Editor = MakeShared<FJsonObject>();
	Editor->SetObjectField(TEXT("Position"), BTGraphSourceTests::MakePosition(X, Y));
	Editor->SetStringField(TEXT("NodeComment"), NodeComment);
	Editor->SetBoolField(TEXT("bCommentBubblePinned"), bPinned);
	Editor->SetBoolField(TEXT("bCommentBubbleVisible"), bVisible);
	return Editor;
}

TSharedPtr<FJsonObject> MakeDecorator(
	const FString& Id,
	const FString& NodeName,
	double X,
	double Y,
	const FString& Comment)
{
	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetStringField(TEXT("NodeName"), NodeName);
	TSharedPtr<FJsonObject> Decorator = MakeShared<FJsonObject>();
	Decorator->SetStringField(TEXT("Id"), Id);
	Decorator->SetStringField(TEXT("Class"), TEXT("/Script/AIModule.BTDecorator_ForceSuccess"));
	Decorator->SetObjectField(TEXT("Properties"), Properties);
	Decorator->SetObjectField(TEXT("Editor"), MakeEditor(X, Y, Comment, true, false));
	return Decorator;
}

TSharedPtr<FJsonObject> MakeService(
	const FString& Id,
	const FString& NodeName,
	double Interval,
	double X,
	double Y,
	const FString& Comment)
{
	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetStringField(TEXT("NodeName"), NodeName);
	Properties->SetNumberField(TEXT("Interval"), Interval);
	Properties->SetNumberField(TEXT("RandomDeviation"), 0.0);
	Properties->SetObjectField(TEXT("BlackboardKey"), BTTestMakeSelectorProperty(TEXT("TargetActor")));
	TSharedPtr<FJsonObject> Service = MakeShared<FJsonObject>();
	Service->SetStringField(TEXT("Id"), Id);
	Service->SetStringField(TEXT("Class"), TEXT("/Script/AIModule.BTService_DefaultFocus"));
	Service->SetObjectField(TEXT("Properties"), Properties);
	Service->SetObjectField(TEXT("Editor"), MakeEditor(X, Y, Comment, false, true));
	return Service;
}

TSharedPtr<FJsonObject> FirstChild(const TSharedPtr<FJsonObject>& Tree)
{
	TSharedPtr<FJsonObject> Root = BTTestGetObjectField(Tree, TEXT("Root"));
	const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
	return Root.IsValid()
		&& Root->TryGetArrayField(TEXT("Children"), Children)
		&& Children
		&& Children->Num() > 0
		? (*Children)[0]->AsObject()
		: nullptr;
}

TSharedPtr<FJsonObject> MakeAttachmentTree(bool bReverseRootDecorators = false, bool bReverseServices = false)
{
	TSharedPtr<FJsonObject> Tree = BTGraphSourceTests::MakeGraphSourceTree();
	TSharedPtr<FJsonObject> Root = BTTestGetObjectField(Tree, TEXT("Root"));
	TArray<TSharedPtr<FJsonValue>> RootDecorators = {
		BTTestMakeObjectValue(MakeDecorator(RootDecoratorGuidA, TEXT("Root decorator A"), -80.0, 20.0, TEXT("root decorator comment A"))),
		BTTestMakeObjectValue(MakeDecorator(RootDecoratorGuidB, TEXT("Root decorator B"), -40.0, 40.0, TEXT("root decorator comment B")))};
	if (bReverseRootDecorators)
	{
		RootDecorators.Swap(0, 1);
	}
	Root->SetArrayField(TEXT("Decorators"), RootDecorators);

	TArray<TSharedPtr<FJsonValue>> Services = {
		BTTestMakeObjectValue(MakeService(RootServiceGuidA, TEXT("Root service A"), 0.5, -20.0, 60.0, TEXT("service comment A"))),
		BTTestMakeObjectValue(MakeService(RootServiceGuidB, TEXT("Root service B"), 1.5, 20.0, 80.0, TEXT("service comment B")))};
	if (bReverseServices)
	{
		Services.Swap(0, 1);
	}
	Root->SetArrayField(TEXT("Services"), Services);

	TSharedPtr<FJsonObject> Child = FirstChild(Tree);
	Child->SetArrayField(TEXT("Decorators"), {
		BTTestMakeObjectValue(MakeDecorator(EdgeDecoratorGuidA, TEXT("Edge decorator A"), 80.0, 260.0, TEXT("edge decorator comment A"))),
		BTTestMakeObjectValue(MakeDecorator(EdgeDecoratorGuidB, TEXT("Edge decorator B"), 120.0, 280.0, TEXT("edge decorator comment B")))});
	return Tree;
}

TSharedPtr<FJsonObject> MakeBoundNode(
	const FString& Id,
	const FString& Kind,
	double X,
	double Y,
	const FString& NodeName = FString())
{
	TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("Id"), Id);
	Node->SetStringField(TEXT("Kind"), Kind);
	Node->SetObjectField(TEXT("Editor"), MakeEditor(X, Y, FString::Printf(TEXT("%s node comment"), *Kind), false, true));
	if (Kind == TEXT("Test"))
	{
		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		Properties->SetStringField(TEXT("NodeName"), NodeName);
		Node->SetStringField(TEXT("Class"), TEXT("/Script/AIModule.BTDecorator_ForceSuccess"));
		Node->SetObjectField(TEXT("Properties"), Properties);
	}
	return Node;
}

TSharedPtr<FJsonValue> MakeLink(const FString& From, const FString& To, int32 ToInput)
{
	TSharedPtr<FJsonObject> Link = MakeShared<FJsonObject>();
	Link->SetStringField(TEXT("From"), From);
	Link->SetStringField(TEXT("To"), To);
	Link->SetNumberField(TEXT("ToInput"), ToInput);
	return BTTestMakeObjectValue(Link);
}

TSharedPtr<FJsonObject> MakeBoundGraph(const FString& Shape)
{
	TSharedPtr<FJsonObject> Graph = MakeShared<FJsonObject>();
	Graph->SetStringField(TEXT("GraphGuid"), BoundGraphGuid);
	TArray<TSharedPtr<FJsonValue>> Nodes = {
		BTTestMakeObjectValue(MakeBoundNode(SinkGuid, TEXT("Sink"), 500.0, 0.0))};
	TArray<TSharedPtr<FJsonValue>> Links;
	if (Shape == TEXT("Test"))
	{
		Nodes.Add(BTTestMakeObjectValue(MakeBoundNode(TestGuidA, TEXT("Test"), 100.0, 0.0, TEXT("Test A"))));
		Links.Add(MakeLink(TestGuidA, SinkGuid, 0));
	}
	else if (Shape == TEXT("Not"))
	{
		Nodes.Add(BTTestMakeObjectValue(MakeBoundNode(LogicGuidA, TEXT("Not"), 300.0, 0.0)));
		Nodes.Add(BTTestMakeObjectValue(MakeBoundNode(TestGuidA, TEXT("Test"), 100.0, 0.0, TEXT("Test A"))));
		Links.Add(MakeLink(TestGuidA, LogicGuidA, 0));
		Links.Add(MakeLink(LogicGuidA, SinkGuid, 0));
	}
	else if (Shape == TEXT("And") || Shape == TEXT("Or") || Shape == TEXT("And3") || Shape == TEXT("Or3"))
	{
		const FString LogicKind = Shape.StartsWith(TEXT("And")) ? TEXT("And") : TEXT("Or");
		Nodes.Add(BTTestMakeObjectValue(MakeBoundNode(LogicGuidA, LogicKind, 300.0, 0.0)));
		Nodes.Add(BTTestMakeObjectValue(MakeBoundNode(TestGuidA, TEXT("Test"), 100.0, -100.0, TEXT("Test A"))));
		Nodes.Add(BTTestMakeObjectValue(MakeBoundNode(TestGuidB, TEXT("Test"), 100.0, 100.0, TEXT("Test B"))));
		Links.Add(MakeLink(TestGuidA, LogicGuidA, 0));
		Links.Add(MakeLink(TestGuidB, LogicGuidA, 1));
		if (Shape.EndsWith(TEXT("3")))
		{
			Nodes.Add(BTTestMakeObjectValue(MakeBoundNode(TestGuidC, TEXT("Test"), 100.0, 200.0, TEXT("Test C"))));
			Links.Add(MakeLink(TestGuidC, LogicGuidA, 2));
		}
		Links.Add(MakeLink(LogicGuidA, SinkGuid, 0));
	}
	else if (Shape == TEXT("NestedNotOr"))
	{
		Nodes.Add(BTTestMakeObjectValue(MakeBoundNode(LogicGuidA, TEXT("Not"), 350.0, 0.0)));
		Nodes.Add(BTTestMakeObjectValue(MakeBoundNode(LogicGuidB, TEXT("Or"), 200.0, 0.0)));
		Nodes.Add(BTTestMakeObjectValue(MakeBoundNode(TestGuidA, TEXT("Test"), 0.0, -100.0, TEXT("Test A"))));
		Nodes.Add(BTTestMakeObjectValue(MakeBoundNode(TestGuidB, TEXT("Test"), 0.0, 100.0, TEXT("Test B"))));
		Links.Add(MakeLink(TestGuidA, LogicGuidB, 0));
		Links.Add(MakeLink(TestGuidB, LogicGuidB, 1));
		Links.Add(MakeLink(LogicGuidB, LogicGuidA, 0));
		Links.Add(MakeLink(LogicGuidA, SinkGuid, 0));
	}
	Graph->SetArrayField(TEXT("Nodes"), Nodes);
	Graph->SetArrayField(TEXT("Links"), Links);
	return Graph;
}

TSharedPtr<FJsonObject> MakeCompositeDecorator(const TSharedPtr<FJsonObject>& BoundGraph)
{
	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetStringField(TEXT("CompositeName"), TEXT("Authored composite decorator"));
	Properties->SetBoolField(TEXT("bShowOperations"), true);
	TSharedPtr<FJsonObject> Composite = MakeShared<FJsonObject>();
	Composite->SetStringField(TEXT("Id"), CompositeDecoratorGuid);
	Composite->SetStringField(TEXT("Kind"), TEXT("Composite"));
	Composite->SetObjectField(TEXT("Properties"), Properties);
	Composite->SetObjectField(TEXT("Editor"), MakeEditor(150.0, 250.0, TEXT("composite wrapper comment"), true, true));
	Composite->SetObjectField(TEXT("BoundGraph"), BoundGraph);
	return Composite;
}

TSharedPtr<FJsonObject> MakeCompositeTree(const FString& Shape)
{
	TSharedPtr<FJsonObject> Tree = BTGraphSourceTests::MakeGraphSourceTree();
	FirstChild(Tree)->SetArrayField(TEXT("Decorators"), {BTTestMakeObjectValue(MakeCompositeDecorator(MakeBoundGraph(Shape)))});
	return Tree;
}

TSharedPtr<FJsonObject> MakeComment(
	const FString& Id = CommentGuid,
	const FString& Text = TEXT("Inherited comment text"),
	double X = -320.0)
{
	TSharedPtr<FJsonObject> Comment = MakeShared<FJsonObject>();
	Comment->SetStringField(TEXT("Id"), Id);
	Comment->SetStringField(TEXT("Text"), Text);
	Comment->SetObjectField(TEXT("Position"), BTGraphSourceTests::MakePosition(X, -180.0));
	TSharedPtr<FJsonObject> Size = MakeShared<FJsonObject>();
	Size->SetNumberField(TEXT("Width"), 640.0);
	Size->SetNumberField(TEXT("Height"), 360.0);
	Comment->SetObjectField(TEXT("Size"), Size);
	TSharedPtr<FJsonObject> Color = MakeShared<FJsonObject>();
	Color->SetNumberField(TEXT("R"), 0.1);
	Color->SetNumberField(TEXT("G"), 0.2);
	Color->SetNumberField(TEXT("B"), 0.3);
	Color->SetNumberField(TEXT("A"), 0.4);
	Comment->SetObjectField(TEXT("Color"), Color);
	Comment->SetNumberField(TEXT("CommentDepth"), 7);
	Comment->SetNumberField(TEXT("FontSize"), 23);
	Comment->SetStringField(TEXT("MoveMode"), TEXT("NoGroupMovement"));
	Comment->SetStringField(TEXT("NodeDetails"), TEXT("Stable localized details"));
	Comment->SetBoolField(TEXT("bCommentBubblePinned"), true);
	Comment->SetBoolField(TEXT("bCommentBubbleVisible"), false);
	Comment->SetBoolField(TEXT("bCommentBubbleVisible_InDetailsPanel"), true);
	Comment->SetBoolField(TEXT("bColorCommentBubble"), true);
	return Comment;
}

TSharedPtr<FJsonObject> MakeCommentTree()
{
	TSharedPtr<FJsonObject> Tree = MakeAttachmentTree();
	TSharedPtr<FJsonObject> Root = BTTestGetObjectField(Tree, TEXT("Root"));
	Root->SetObjectField(TEXT("Editor"), MakeEditor(200.0, 0.0, TEXT("root composite comment"), true, false));
	FirstChild(Tree)->SetObjectField(TEXT("Editor"), MakeEditor(100.0, 300.0, TEXT("task comment"), false, true));
	Tree->SetArrayField(TEXT("Comments"), {BTTestMakeObjectValue(MakeComment())});
	return Tree;
}

TSharedPtr<FJsonObject> MakeSimpleParallelTree(
	const FString& BackgroundNodeName = TEXT("Background selector"),
	bool bReverseSemanticX = false)
{
	const double ForegroundX = bReverseSemanticX ? 500.0 : 100.0;
	const double BackgroundX = bReverseSemanticX ? 100.0 : 300.0;
	TSharedPtr<FJsonObject> BackgroundTask = BTGraphSourceTests::MakeGraphNode(
		BackgroundTaskGuid,
		TEXT("/Script/AIModule.BTTask_Wait"),
		TEXT("Background wait"),
		BackgroundX,
		600.0);
	TSharedPtr<FJsonObject> BackgroundComposite = BTGraphSourceTests::MakeGraphNode(
		BackgroundCompositeGuid,
		TEXT("/Script/AIModule.BTComposite_Selector"),
		BackgroundNodeName,
		BackgroundX,
		300.0,
		{BTTestMakeObjectValue(BackgroundTask)});
	TSharedPtr<FJsonObject> ForegroundTask = BTGraphSourceTests::MakeGraphNode(
		ForegroundTaskGuid,
		TEXT("/Script/AIModule.BTTask_Wait"),
		TEXT("Foreground task"),
		ForegroundX,
		300.0);
	TSharedPtr<FJsonObject> Root = BTGraphSourceTests::MakeGraphNode(
		SimpleParallelGuid,
		TEXT("/Script/AIModule.BTComposite_SimpleParallel"),
		TEXT("Authored simple parallel"),
		200.0,
		0.0,
		{BTTestMakeObjectValue(ForegroundTask), BTTestMakeObjectValue(BackgroundComposite)});
	TSharedPtr<FJsonObject> Tree = MakeShared<FJsonObject>();
	Tree->SetStringField(TEXT("GraphGuid"), BTGraphSourceTests::GraphGuid);
	Tree->SetObjectField(TEXT("Root"), Root);
	Tree->SetArrayField(TEXT("Comments"), {});
	return Tree;
}

UBehaviorTreeGraphNode* FindSubNodeByGuid(UBehaviorTreeGraph* Graph, const FString& Guid)
{
	if (!Graph)
	{
		return nullptr;
	}
	for (UEdGraphNode* NodeObject : Graph->Nodes)
	{
		UBehaviorTreeGraphNode* Node = Cast<UBehaviorTreeGraphNode>(NodeObject);
		if (!Node)
		{
			continue;
		}
		for (UBehaviorTreeGraphNode* Decorator : Node->Decorators)
		{
			if (Decorator && Decorator->NodeGuid.ToString(EGuidFormats::Digits).Equals(Guid, ESearchCase::IgnoreCase))
			{
				return Decorator;
			}
		}
		for (UBehaviorTreeGraphNode* Service : Node->Services)
		{
			if (Service && Service->NodeGuid.ToString(EGuidFormats::Digits).Equals(Guid, ESearchCase::IgnoreCase))
			{
				return Service;
			}
		}
	}
	return nullptr;
}

UEdGraphNode_Comment* FindComment(UBehaviorTreeGraph* Graph)
{
	if (!Graph)
	{
		return nullptr;
	}
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node))
		{
			return Comment;
		}
	}
	return nullptr;
}

TSharedPtr<FJsonObject> FindAttachment(const TSharedPtr<FJsonObject>& Node, const FString& Field, const FString& Id)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Node.IsValid() || !Node->TryGetArrayField(Field, Values) || !Values)
	{
		return nullptr;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		TSharedPtr<FJsonObject> Object = Value->AsObject();
		FString Candidate;
		if (Object.IsValid() && Object->TryGetStringField(TEXT("Id"), Candidate) && Candidate == Id)
		{
			return Object;
		}
	}
	return nullptr;
}

FString ComparableTree(UBehaviorTree* BehaviorTree)
{
	TSharedRef<FJsonObject> Extracted = MakeShared<FJsonObject>();
	FAssetDocumentRegionContext Context = BTGraphSourceTests::MakeTreeContext(BehaviorTree);
	if (!BTGraphSourceTests::ExtractTree(Context, Extracted).bSuccess)
	{
		return FString();
	}
	return FAssetDocumentJsonRegionUtils::JsonValueToComparableString(BTTestMakeObjectValue(Extracted));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeGraphGuidContractTest,
	"UEAssetDoc.BehaviorTree.GraphSource.GraphGuidContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeGraphGuidContractTest::RunTest(const FString&)
{
	BTGraphSourceTests::FPublicGraphFixture PublicGraphFixture;
	using namespace BTGraphShapeTests;
	const auto ComparableDiff = [](const TArray<TSharedPtr<FJsonValue>>& Entries)
	{
		return FAssetDocumentJsonRegionUtils::JsonValueToComparableString(MakeShared<FJsonValueArray>(Entries));
	};
	TSharedPtr<FJsonObject> MissingGuidTree = BTGraphSourceTests::MakeGraphSourceTree();
	MissingGuidTree->RemoveField(TEXT("GraphGuid"));
	UBehaviorTree* NewAsset = BTGraphSourceTests::CreateTree();
	FAssetDocumentRegionContext NewContext = BTGraphSourceTests::MakeTreeContext(NewAsset);
	FAssetDocumentCapabilityResult Result = BTGraphSourceTests::ValidateTree(NewContext, MissingGuidTree.ToSharedRef());
	TestTrue(TEXT("new graph may omit GraphGuid during public Validate"), Result.bSuccess);
	TArray<TSharedPtr<FJsonValue>> DiffEntries;
	Result = BTGraphSourceTests::DiffTree(NewContext, MissingGuidTree.ToSharedRef(), DiffEntries);
	TestTrue(TEXT("public Diff accepts omitted new GraphGuid"), Result.bSuccess);
	TestNull(TEXT("Validate and Diff do not create a live graph"), NewAsset->BTGraph.Get());
	bool bChanged = false;
	Result = BTGraphSourceTests::ApplyTree(NewAsset, MissingGuidTree, bChanged);
	if (!TestTrue(TEXT("public ApplyFile generates the new graph identity"), Result.bSuccess)) return false;
	const UBehaviorTreeGraph* GeneratedGraph = Cast<UBehaviorTreeGraph>(NewAsset->BTGraph);
	TestTrue(TEXT("generated graph identity is valid"), GeneratedGraph && GeneratedGraph->GraphGuid.IsValid());

	TSharedPtr<FJsonObject> EmptyMissingGuidTree = MakeShared<FJsonObject>();
	EmptyMissingGuidTree->SetObjectField(TEXT("Root"), MakeShared<FJsonObject>());
	EmptyMissingGuidTree->SetArrayField(TEXT("Comments"), {});
	UBehaviorTree* EmptyMissingGuidAsset = BTGraphSourceTests::CreateTree();
	FAssetDocumentRegionContext EmptyMissingContext = BTGraphSourceTests::MakeTreeContext(EmptyMissingGuidAsset);
	Result = BTGraphSourceTests::ValidateTree(EmptyMissingContext, EmptyMissingGuidTree.ToSharedRef());
	TestTrue(TEXT("empty new graph may omit GraphGuid during Validate"), Result.bSuccess);
	DiffEntries.Reset();
	Result = BTGraphSourceTests::DiffTree(EmptyMissingContext, EmptyMissingGuidTree.ToSharedRef(), DiffEntries);
	TestTrue(TEXT("public Diff accepts omitted empty graph identity"), Result.bSuccess);
	TestNull(TEXT("empty graph dry runs do not create a live graph"), EmptyMissingGuidAsset->BTGraph.Get());
	bChanged = false;
	Result = BTGraphSourceTests::ApplyTree(EmptyMissingGuidAsset, EmptyMissingGuidTree, bChanged);
	if (!TestTrue(TEXT("public ApplyFile generates empty graph identity"), Result.bSuccess)) return false;
	const UBehaviorTreeGraph* GeneratedEmpty = Cast<UBehaviorTreeGraph>(EmptyMissingGuidAsset->BTGraph);
	TestTrue(TEXT("empty generated graph identity is valid"), GeneratedEmpty && GeneratedEmpty->GraphGuid.IsValid());

	TSharedPtr<FJsonObject> AuthoredEmptyTree = MakeShared<FJsonObject>();
	AuthoredEmptyTree->SetStringField(TEXT("GraphGuid"), BTGraphSourceTests::GraphGuid);
	AuthoredEmptyTree->SetObjectField(TEXT("Root"), MakeShared<FJsonObject>());
	AuthoredEmptyTree->SetArrayField(TEXT("Comments"), {});
	UBehaviorTree* AuthoredEmptyAsset = BTGraphSourceTests::CreateTree();
	FAssetDocumentRegionContext AuthoredEmptyContext = BTGraphSourceTests::MakeTreeContext(AuthoredEmptyAsset);
	TArray<TSharedPtr<FJsonValue>> EmptyDiffA;
	TArray<TSharedPtr<FJsonValue>> EmptyDiffB;
	Result = BTGraphSourceTests::DiffTree(AuthoredEmptyContext, AuthoredEmptyTree.ToSharedRef(), EmptyDiffA);
	TestTrue(TEXT("authored empty graph first diff succeeds"), Result.bSuccess);
	Result = BTGraphSourceTests::DiffTree(AuthoredEmptyContext, AuthoredEmptyTree.ToSharedRef(), EmptyDiffB);
	TestTrue(TEXT("authored empty graph repeated diff succeeds"), Result.bSuccess);
	TestEqual(TEXT("authored empty graph repeated diff is stable"), ComparableDiff(EmptyDiffB), ComparableDiff(EmptyDiffA));
	bChanged = false;
	Result = BTGraphSourceTests::ApplyTree(AuthoredEmptyAsset, AuthoredEmptyTree, bChanged);
	TestTrue(TEXT("authored empty graph applies"), Result.bSuccess);
	TestEqual(TEXT("authored empty graph keeps exact identity"), Cast<UBehaviorTreeGraph>(AuthoredEmptyAsset->BTGraph)->GraphGuid.ToString(EGuidFormats::Digits), FString(BTGraphSourceTests::GraphGuid));
	EmptyDiffA.Reset();
	EmptyDiffB.Reset();
	Result = BTGraphSourceTests::DiffTree(AuthoredEmptyContext, AuthoredEmptyTree.ToSharedRef(), EmptyDiffA);
	TestTrue(TEXT("applied empty graph first no-op diff succeeds"), Result.bSuccess);
	Result = BTGraphSourceTests::DiffTree(AuthoredEmptyContext, AuthoredEmptyTree.ToSharedRef(), EmptyDiffB);
	TestTrue(TEXT("applied empty graph repeated no-op diff succeeds"), Result.bSuccess);
	TestEqual(TEXT("applied empty graph first diff is empty"), EmptyDiffA.Num(), 0);
	TestEqual(TEXT("applied empty graph repeated diff is empty"), EmptyDiffB.Num(), 0);

	UBehaviorTree* Existing = BTGraphSourceTests::CreateTree();
	Result = BTGraphSourceTests::ApplyTree(Existing, BTGraphSourceTests::MakeGraphSourceTree(), bChanged);
	TestTrue(TEXT("existing graph fixture applies with explicit GraphGuid"), Result.bSuccess);
	UBehaviorTreeGraph* OriginalGraph = Cast<UBehaviorTreeGraph>(Existing->BTGraph);
	const FGuid OriginalGuid = OriginalGraph ? OriginalGraph->GraphGuid : FGuid();
	TSharedPtr<FJsonObject> ExistingUpdate = BTGraphSourceTests::MakeGraphSourceTree(TEXT("Updated with omitted graph GUID"));
	ExistingUpdate->RemoveField(TEXT("GraphGuid"));
	Result = BTGraphSourceTests::ApplyTree(Existing, ExistingUpdate, bChanged);
	TestTrue(TEXT("existing graph update may omit GraphGuid"), Result.bSuccess);
	TestEqual(TEXT("existing graph omission reuses persistent GraphGuid"), Cast<UBehaviorTreeGraph>(Existing->BTGraph)->GraphGuid, OriginalGuid);

	UBehaviorTreeGraph* BeforeReplacementGraph = Cast<UBehaviorTreeGraph>(Existing->BTGraph);
	UBTCompositeNode* BeforeReplacementRoot = Existing->RootNode;
	const FString BeforeReplacementCanonical = ComparableTree(Existing);
	TSharedPtr<FJsonObject> Replacement = BTGraphSourceTests::MakeGraphSourceTree(TEXT("Graph identity replacement must be rejected"));
	Replacement->SetStringField(TEXT("GraphGuid"), ReplacementGraphGuid);
	FAssetDocumentRegionContext ExistingContext = BTGraphSourceTests::MakeTreeContext(Existing);
	Result = BTGraphSourceTests::ValidateTree(ExistingContext, Replacement.ToSharedRef());
	TestFalse(TEXT("existing graph explicit identity replacement validate is rejected"), Result.bSuccess);
	TestTrue(TEXT("existing graph replacement validate diagnostic is exact"), BTTestHasDiagnostic(Result, TEXT("/Body/Tree/GraphGuid"), TEXT("BehaviorTreeGraphGuidReplacementRejected")));
	DiffEntries.Reset();
	Result = BTGraphSourceTests::DiffTree(ExistingContext, Replacement.ToSharedRef(), DiffEntries);
	TestFalse(TEXT("existing graph explicit identity replacement diff is rejected"), Result.bSuccess);
	TestTrue(TEXT("existing graph replacement diff diagnostic is exact"), BTTestHasDiagnostic(Result, TEXT("/Body/Tree/GraphGuid"), TEXT("BehaviorTreeGraphGuidReplacementRejected")));
	bChanged = false;
	Result = BTGraphSourceTests::ApplyTree(Existing, Replacement, bChanged);
	TestFalse(TEXT("existing graph explicit identity replacement apply is rejected"), Result.bSuccess);
	TestTrue(TEXT("existing graph replacement apply diagnostic is exact"), BTTestHasDiagnostic(Result, TEXT("/Body/Tree/GraphGuid"), TEXT("BehaviorTreeGraphGuidReplacementRejected")));
	TestFalse(TEXT("rejected graph identity replacement reports unchanged"), bChanged);
	TestTrue(TEXT("rejected graph identity replacement keeps graph pointer"), Existing->BTGraph == BeforeReplacementGraph);
	TestTrue(TEXT("rejected graph identity replacement keeps runtime pointer"), Existing->RootNode == BeforeReplacementRoot);
	TestEqual(TEXT("rejected graph identity replacement keeps canonical extract"), ComparableTree(Existing), BeforeReplacementCanonical);

	FAssetDocumentTemplateRequest TemplateRequest;
	TemplateRequest.Target = TEXT("/Game/AssetDocumentTests/BT_GraphShape_GraphGuidTemplate");
	TemplateRequest.Class = TEXT("/Script/AIModule.BehaviorTree");
	const FAssetDocumentResult TemplateResult = FAssetDocumentModule::Get().GetService().CreateTemplate(TemplateRequest);
	if (!TestTrue(TEXT("public CreateTemplate succeeds for the GraphGuid contract"), TemplateResult.IsSuccess())
		|| !TestNotNull(TEXT("public CreateTemplate returns its document"), TemplateResult.Payload.Get())) return false;
	TSharedRef<FJsonObject> Template = TemplateResult.Payload.ToSharedRef();
	TSharedPtr<FJsonObject> Body = BTTestGetObjectField(Template, TEXT("Body"));
	TSharedPtr<FJsonObject> Tree = BTTestGetObjectField(Body, TEXT("Tree"));
	FString TemplateGuidString;
	FGuid TemplateGuid;
	TestTrue(TEXT("CreateTemplate exposes canonical GraphGuid"), Tree.IsValid()
		&& Tree->TryGetStringField(TEXT("GraphGuid"), TemplateGuidString)
		&& FGuid::ParseExact(TemplateGuidString, EGuidFormats::Digits, TemplateGuid)
		&& TemplateGuid.IsValid()
		&& TemplateGuidString == TemplateGuid.ToString(EGuidFormats::Digits));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeProjectClassFamiliesTest,
	"UEAssetDoc.BehaviorTree.GraphSource.ProjectClassFamilies",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeProjectClassFamiliesTest::RunTest(const FString&)
{
	using namespace BTGraphShapeTests;
	FScopedBehaviorTreeMutationFixture BlueprintFixtureCleanup;
	BTGraphSourceTests::FPublicGraphFixture PublicGraphFixture;
	TArray<TStrongObjectPtr<UBlueprint>> FixtureBlueprints;
	TArray<UPackage*> FixtureBlueprintPackages;
	const auto CreateProjectBlueprintClass =
		[&BlueprintFixtureCleanup, &FixtureBlueprintPackages, &FixtureBlueprints](
			UClass* ParentClass,
			const TCHAR* Stem)
	{
		const FString PackageName = FString::Printf(
			TEXT("/Game/AssetDocumentTests/BehaviorTree/%s_%s"),
			Stem,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		if (!BlueprintFixtureCleanup.ReserveTarget(PackageName))
		{
			return static_cast<UClass*>(nullptr);
		}
		UPackage* Package = CreatePackage(*PackageName);
		UBlueprint* Blueprint = Package
			? FKismetEditorUtilities::CreateBlueprint(
				ParentClass,
				Package,
				FName(*FPackageName::GetLongPackageAssetName(PackageName)),
				BPTYPE_Normal,
				UBlueprint::StaticClass(),
				UBlueprintGeneratedClass::StaticClass(),
				TEXT("AssetDocumentBehaviorTreeGraphShape"))
			: nullptr;
		if (!Blueprint)
		{
			return static_cast<UClass*>(nullptr);
		}
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		UClass* GeneratedClass = Blueprint->GeneratedClass;
		if (!GeneratedClass || GeneratedClass->HasAnyClassFlags(CLASS_Abstract))
		{
			return static_cast<UClass*>(nullptr);
		}
		const FString PackageFileName = FPackageName::LongPackageNameToFilename(
			PackageName,
			FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		if (!UPackage::SavePackage(Package, Blueprint, *PackageFileName, SaveArgs))
		{
			return static_cast<UClass*>(nullptr);
		}
		FixtureBlueprintPackages.Add(Package);
		FixtureBlueprints.Emplace(Blueprint);
		return GeneratedClass;
	};
	UClass* TaskClass = CreateProjectBlueprintClass(
		UBTTask_BlueprintBase::StaticClass(),
		TEXT("BTTask_ProjectFixture"));
	UClass* CompositeClass = CreateProjectBlueprintClass(
		UAssetDocumentProjectCompositeTestNode::StaticClass(),
		TEXT("BTComposite_ProjectFixture"));
	UClass* ServiceClass = CreateProjectBlueprintClass(
		UAssetDocumentProjectServiceTestNode::StaticClass(),
		TEXT("BTService_ProjectFixture"));
	TestNotNull(TEXT("required project Blueprint task fixture is generated"), TaskClass);
	TestNotNull(TEXT("required project Blueprint composite fixture is generated"), CompositeClass);
	TestNotNull(TEXT("required project Blueprint service fixture is generated"), ServiceClass);
	if (!TaskClass || !CompositeClass || !ServiceClass)
	{
		AddError(TEXT("This test requires generated project Blueprint task/composite/service classes; skipping is forbidden"));
		return false;
	}

	TSharedPtr<FJsonObject> Task = BTGraphSourceTests::MakeGraphNode(
		ProjectTaskGuid,
		TaskClass->GetPathName(),
		TEXT("Project task authored property"),
		100.0,
		300.0);
	TSharedPtr<FJsonObject> Root = BTGraphSourceTests::MakeGraphNode(
		ProjectCompositeGuid,
		CompositeClass->GetPathName(),
		TEXT("Project composite authored property"),
		100.0,
		0.0,
		{BTTestMakeObjectValue(Task)});
	TSharedPtr<FJsonObject> ProjectService = MakeService(
		ProjectServiceGuid,
		TEXT("Project service authored property"),
		1.875,
		0.0,
		100.0,
		TEXT("project service wrapper comment"));
	ProjectService->SetStringField(TEXT("Class"), ServiceClass->GetPathName());
	Root->SetArrayField(TEXT("Services"), {BTTestMakeObjectValue(ProjectService)});
	TSharedPtr<FJsonObject> Tree = MakeShared<FJsonObject>();
	Tree->SetStringField(TEXT("GraphGuid"), BTGraphSourceTests::GraphGuid);
	Tree->SetObjectField(TEXT("Root"), Root);
	Tree->SetArrayField(TEXT("Comments"), {});

	UBehaviorTree* BehaviorTree = BTGraphSourceTests::CreateTree();
	bool bChanged = false;
	FAssetDocumentCapabilityResult Result = BTGraphSourceTests::ApplyTree(BehaviorTree, Tree, bChanged);
	TestTrue(TEXT("project task/composite/service families apply dynamically"), Result.bSuccess);
	if (!Result.bSuccess)
	{
		AddError(Result.Message);
		return false;
	}
	UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(BehaviorTree->BTGraph);
	UBehaviorTreeGraphNode* RootWrapper = BTGraphSourceTests::FindGraphNodeByGuid(Graph, ProjectCompositeGuid);
	UBehaviorTreeGraphNode* TaskWrapper = BTGraphSourceTests::FindGraphNodeByGuid(Graph, ProjectTaskGuid);
	UBehaviorTreeGraphNode* ServiceWrapper = FindSubNodeByGuid(Graph, ProjectServiceGuid);
	TestTrue(TEXT("project composite wrapper owns exact class"), RootWrapper && RootWrapper->NodeInstance && RootWrapper->NodeInstance->GetClass() == CompositeClass);
	TestTrue(TEXT("project task wrapper owns exact class"), TaskWrapper && TaskWrapper->NodeInstance && TaskWrapper->NodeInstance->GetClass() == TaskClass);
	TestTrue(TEXT("project service wrapper owns exact class"), ServiceWrapper && ServiceWrapper->NodeInstance && ServiceWrapper->NodeInstance->GetClass() == ServiceClass);
	TestEqual(TEXT("project composite inherited property applies"), RootWrapper ? CastChecked<UBTNode>(RootWrapper->NodeInstance)->NodeName : FString(), FString(TEXT("Project composite authored property")));
	TestEqual(TEXT("project task inherited property applies"), TaskWrapper ? CastChecked<UBTNode>(TaskWrapper->NodeInstance)->NodeName : FString(), FString(TEXT("Project task authored property")));
	TestEqual(TEXT("project service inherited property applies"), ServiceWrapper ? CastChecked<UBTNode>(ServiceWrapper->NodeInstance)->NodeName : FString(), FString(TEXT("Project service authored property")));
	const FString TaskClassPath = TaskClass->GetPathName();
	const FString CompositeClassPath = CompositeClass->GetPathName();
	const FString ServiceClassPath = ServiceClass->GetPathName();

	const FString Target = BTTestMakeUniqueTarget(TEXT("BT_GraphShape_ProjectClassesReload"));
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	FScopedBehaviorTreeMutationFixture Fixture;
	const FAssetDocumentResult ApplyResult = Fixture.Apply(Service, Target, BTTestMakeBehaviorTreeDocument(Target, BTGraphSourceTests::MakeGraphSourceBody(Tree)));
	TestTrue(TEXT("project class tree saves"), ApplyResult.IsSuccess());
	UBehaviorTree* BeforeReload = BTTestLoadBehaviorTreeForTarget(Target);
	UPackage* Package = BeforeReload ? BeforeReload->GetOutermost() : nullptr;
	TArray<UPackage*> PackagesToUnload = FixtureBlueprintPackages;
	if (Package)
	{
		PackagesToUnload.Add(Package);
	}
	FixtureBlueprints.Reset();
	TestTrue(
		TEXT("project class tree and Blueprint fixture packages unload"),
		Package && UPackageTools::UnloadPackages(PackagesToUnload));
	UBehaviorTree* Reloaded = BTTestLoadBehaviorTreeForTarget(Target);
	UBehaviorTreeGraph* ReloadedGraph = Reloaded ? Cast<UBehaviorTreeGraph>(Reloaded->BTGraph) : nullptr;
	TestNotNull(TEXT("project class tree reloads"), Reloaded);
	TestNotNull(TEXT("project class graph reloads"), ReloadedGraph);
	UBehaviorTreeGraphNode* ReloadedRootWrapper =
		BTGraphSourceTests::FindGraphNodeByGuid(ReloadedGraph, ProjectCompositeGuid);
	UBehaviorTreeGraphNode* ReloadedTaskWrapper =
		BTGraphSourceTests::FindGraphNodeByGuid(ReloadedGraph, ProjectTaskGuid);
	UBehaviorTreeGraphNode* ReloadedServiceWrapper =
		FindSubNodeByGuid(ReloadedGraph, ProjectServiceGuid);
	TestNotNull(TEXT("project composite wrapper reloads"), ReloadedRootWrapper);
	TestNotNull(TEXT("project task wrapper reloads"), ReloadedTaskWrapper);
	TestNotNull(TEXT("project service wrapper reloads"), ReloadedServiceWrapper);
	TestNotNull(
		TEXT("project composite instance reloads"),
		ReloadedRootWrapper ? ReloadedRootWrapper->NodeInstance.Get() : nullptr);
	TestNotNull(
		TEXT("project task instance reloads"),
		ReloadedTaskWrapper ? ReloadedTaskWrapper->NodeInstance.Get() : nullptr);
	TestNotNull(
		TEXT("project service instance reloads"),
		ReloadedServiceWrapper ? ReloadedServiceWrapper->NodeInstance.Get() : nullptr);
	TestTrue(
		TEXT("project composite class survives reload"),
		ReloadedRootWrapper && ReloadedRootWrapper->NodeInstance
			&& ReloadedRootWrapper->NodeInstance->GetClass()->GetPathName() == CompositeClassPath);
	TestTrue(
		TEXT("project task class survives reload"),
		ReloadedTaskWrapper && ReloadedTaskWrapper->NodeInstance
			&& ReloadedTaskWrapper->NodeInstance->GetClass()->GetPathName() == TaskClassPath);
	TestTrue(
		TEXT("project service class survives reload"),
		ReloadedServiceWrapper && ReloadedServiceWrapper->NodeInstance
			&& ReloadedServiceWrapper->NodeInstance->GetClass()->GetPathName() == ServiceClassPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeSimpleParallelOutputsTest,
	"UEAssetDoc.BehaviorTree.GraphSource.SimpleParallelOutputs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeSimpleParallelOutputsTest::RunTest(const FString&)
{
	BTGraphSourceTests::FPublicGraphFixture PublicGraphFixture;
	using namespace BTGraphShapeTests;
	const auto FindOutputPin = [](UBehaviorTreeGraphNode* Node, const FString& Name)
	{
		if (!Node)
		{
			return static_cast<UEdGraphPin*>(nullptr);
		}
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output && Pin->PinName.ToString() == Name)
			{
				return Pin;
			}
		}
		return static_cast<UEdGraphPin*>(nullptr);
	};
	UBehaviorTree* BehaviorTree = BTGraphSourceTests::CreateTree();
	bool bChanged = false;
	FAssetDocumentCapabilityResult Result = BTGraphSourceTests::ApplyTree(BehaviorTree, MakeSimpleParallelTree(), bChanged);
	TestTrue(TEXT("SimpleParallel foreground/background graph applies"), Result.bSuccess);
	if (!Result.bSuccess)
	{
		AddError(Result.Message);
		return false;
	}
	UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(BehaviorTree->BTGraph);
	UBehaviorTreeGraphNode_SimpleParallel* Parallel = Cast<UBehaviorTreeGraphNode_SimpleParallel>(BTGraphSourceTests::FindGraphNodeByGuid(Graph, SimpleParallelGuid));
	UBehaviorTreeGraphNode* Foreground = BTGraphSourceTests::FindGraphNodeByGuid(Graph, ForegroundTaskGuid);
	UBehaviorTreeGraphNode* Background = BTGraphSourceTests::FindGraphNodeByGuid(Graph, BackgroundCompositeGuid);
	UEdGraphPin* TaskPin = FindOutputPin(Parallel, TEXT("Task"));
	UEdGraphPin* OutPin = FindOutputPin(Parallel, TEXT("Out"));
	TestNotNull(TEXT("SimpleParallel exposes Task output"), TaskPin);
	TestNotNull(TEXT("SimpleParallel exposes Out output"), OutPin);
	TestTrue(TEXT("foreground is connected only to Task output"), TaskPin && TaskPin->LinkedTo.Num() == 1 && TaskPin->LinkedTo[0]->GetOwningNode() == Foreground);
	TestTrue(TEXT("background is connected only to Out output"), OutPin && OutPin->LinkedTo.Num() == 1 && OutPin->LinkedTo[0]->GetOwningNode() == Background);
	TestEqual(TEXT("SimpleParallel runtime mirror keeps both children"), BehaviorTree->RootNode ? BehaviorTree->RootNode->Children.Num() : -1, 2);
	TestTrue(TEXT("runtime child zero mirrors foreground Task pin"), BehaviorTree->RootNode && BehaviorTree->RootNode->Children.Num() == 2 && BehaviorTree->RootNode->Children[0].ChildTask == Foreground->NodeInstance);
	TestTrue(TEXT("runtime child one mirrors background Out pin"), BehaviorTree->RootNode && BehaviorTree->RootNode->Children.Num() == 2 && BehaviorTree->RootNode->Children[1].ChildComposite == Background->NodeInstance);

	TSharedPtr<FJsonObject> Extracted = BTGraphSourceTests::ExtractTree(*this, BehaviorTree);
	TSharedPtr<FJsonObject> ExtractedRoot = Extracted.IsValid() ? BTTestGetObjectField(Extracted, TEXT("Root")) : nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
	TestTrue(TEXT("SimpleParallel extracts two semantic children"), ExtractedRoot.IsValid() && ExtractedRoot->TryGetArrayField(TEXT("Children"), Children) && Children && Children->Num() == 2);
	if (Children && Children->Num() == 2)
	{
		TestEqual(TEXT("foreground extracts first from Task pin"), (*Children)[0]->AsObject()->GetStringField(TEXT("Id")), FString(ForegroundTaskGuid));
		TestEqual(TEXT("background extracts second from Out pin"), (*Children)[1]->AsObject()->GetStringField(TEXT("Id")), FString(BackgroundCompositeGuid));
	}

	TArray<TSharedPtr<FJsonValue>> DiffEntries;
	FAssetDocumentRegionContext Context = BTGraphSourceTests::MakeTreeContext(BehaviorTree);
	const TSharedPtr<FJsonObject> RenamedTree = MakeSimpleParallelTree(TEXT("Renamed background selector"));
	Result = BTGraphSourceTests::DiffTree(Context, RenamedTree.ToSharedRef(), DiffEntries);
	TestTrue(TEXT("SimpleParallel background diff succeeds"), Result.bSuccess);
	const FString ExpectedDiffPath = BTTestTreePointer(RenamedTree, BackgroundCompositeGuid) + TEXT("/Properties/NodeName");
	TestTrue(TEXT("background diff retains identity and pin-semantic child"), DiffEntries.ContainsByPredicate([&ExpectedDiffPath](const TSharedPtr<FJsonValue>& Value)
	{
		TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
		FString Path;
		return Entry.IsValid() && Entry->TryGetStringField(TEXT("path"), Path) && Path == ExpectedDiffPath;
	}));

	UBehaviorTree* ReverseXTree = BTGraphSourceTests::CreateTree();
	bChanged = false;
	Result = BTGraphSourceTests::ApplyTree(ReverseXTree, MakeSimpleParallelTree(TEXT("Background selector"), true), bChanged);
	TestTrue(TEXT("SimpleParallel semantic Task/Out children accept inverse X layout"), Result.bSuccess);
	if (Result.bSuccess)
	{
		TSharedPtr<FJsonObject> ReverseExtracted = BTGraphSourceTests::ExtractTree(*this, ReverseXTree);
		TSharedPtr<FJsonObject> ReverseRoot = ReverseExtracted.IsValid() ? BTTestGetObjectField(ReverseExtracted, TEXT("Root")) : nullptr;
		const TArray<TSharedPtr<FJsonValue>>* ReverseChildren = nullptr;
		TestTrue(TEXT("inverse-X SimpleParallel extracts two semantic children"), ReverseRoot.IsValid() && ReverseRoot->TryGetArrayField(TEXT("Children"), ReverseChildren) && ReverseChildren && ReverseChildren->Num() == 2);
		if (ReverseChildren && ReverseChildren->Num() == 2)
		{
			TestEqual(TEXT("inverse-X foreground remains first by Task pin"), (*ReverseChildren)[0]->AsObject()->GetStringField(TEXT("Id")), FString(ForegroundTaskGuid));
			TestEqual(TEXT("inverse-X background remains second by Out pin"), (*ReverseChildren)[1]->AsObject()->GetStringField(TEXT("Id")), FString(BackgroundCompositeGuid));
			const TSharedPtr<FJsonObject>* ForegroundEditor = nullptr;
			const TSharedPtr<FJsonObject>* BackgroundEditor = nullptr;
			const TSharedPtr<FJsonObject>* ForegroundPosition = nullptr;
			const TSharedPtr<FJsonObject>* BackgroundPosition = nullptr;
			const bool bHasPositions = (*ReverseChildren)[0]->AsObject()->TryGetObjectField(TEXT("Editor"), ForegroundEditor)
				&& ForegroundEditor && ForegroundEditor->IsValid()
				&& (*ForegroundEditor)->TryGetObjectField(TEXT("Position"), ForegroundPosition)
				&& ForegroundPosition && ForegroundPosition->IsValid()
				&& (*ReverseChildren)[1]->AsObject()->TryGetObjectField(TEXT("Editor"), BackgroundEditor)
				&& BackgroundEditor && BackgroundEditor->IsValid()
				&& (*BackgroundEditor)->TryGetObjectField(TEXT("Position"), BackgroundPosition)
				&& BackgroundPosition && BackgroundPosition->IsValid();
			TestTrue(TEXT("inverse-X layout survives extract without semantic reorder"), bHasPositions
				&& (*ForegroundPosition)->GetNumberField(TEXT("X")) > (*BackgroundPosition)->GetNumberField(TEXT("X")));
		}
		DiffEntries.Reset();
		Result = BTGraphSourceTests::DiffTree(
			BTGraphSourceTests::MakeTreeContext(ReverseXTree),
			MakeSimpleParallelTree(TEXT("Background selector"), true).ToSharedRef(),
			DiffEntries);
		TestTrue(TEXT("inverse-X SimpleParallel roundtrip diff succeeds"), Result.bSuccess);
		TestEqual(TEXT("inverse-X SimpleParallel roundtrip is canonical no-op"), DiffEntries.Num(), 0);
	}

	const FString Target = BTTestMakeUniqueTarget(TEXT("BT_GraphShape_SimpleParallelReload"));
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	FScopedBehaviorTreeMutationFixture Fixture;
	const FAssetDocumentResult ApplyResult = Fixture.Apply(Service, Target, BTTestMakeBehaviorTreeDocument(Target, BTGraphSourceTests::MakeGraphSourceBody(MakeSimpleParallelTree(TEXT("Background selector"), true))));
	TestTrue(TEXT("SimpleParallel saves"), ApplyResult.IsSuccess());
	UBehaviorTree* BeforeReload = BTTestLoadBehaviorTreeForTarget(Target);
	UPackage* Package = BeforeReload ? BeforeReload->GetOutermost() : nullptr;
	TestTrue(TEXT("SimpleParallel package unloads"), Package && UPackageTools::UnloadPackages({Package}));
	UBehaviorTree* Reloaded = BTTestLoadBehaviorTreeForTarget(Target);
	UBehaviorTreeGraphNode_SimpleParallel* ReloadedParallel = Reloaded
		? Cast<UBehaviorTreeGraphNode_SimpleParallel>(BTGraphSourceTests::FindGraphNodeByGuid(Cast<UBehaviorTreeGraph>(Reloaded->BTGraph), SimpleParallelGuid))
		: nullptr;
	TaskPin = FindOutputPin(ReloadedParallel, TEXT("Task"));
	OutPin = FindOutputPin(ReloadedParallel, TEXT("Out"));
	TestTrue(TEXT("foreground Task connection survives reload"), TaskPin && TaskPin->LinkedTo.Num() == 1 && TaskPin->LinkedTo[0]->GetOwningNode()->NodeGuid.ToString(EGuidFormats::Digits) == ForegroundTaskGuid);
	TestTrue(TEXT("background Out connection survives reload"), OutPin && OutPin->LinkedTo.Num() == 1 && OutPin->LinkedTo[0]->GetOwningNode()->NodeGuid.ToString(EGuidFormats::Digits) == BackgroundCompositeGuid);
	TestEqual(TEXT("two runtime children survive reload"), Reloaded && Reloaded->RootNode ? Reloaded->RootNode->Children.Num() : -1, 2);
	TSharedPtr<FJsonObject> ReloadedExtracted = Reloaded ? BTGraphSourceTests::ExtractTree(*this, Reloaded) : nullptr;
	TSharedPtr<FJsonObject> ReloadedRoot = ReloadedExtracted.IsValid() ? BTTestGetObjectField(ReloadedExtracted, TEXT("Root")) : nullptr;
	const TArray<TSharedPtr<FJsonValue>>* ReloadedChildren = nullptr;
	TestTrue(TEXT("inverse-X reload extracts Task/Out semantic order"), ReloadedRoot.IsValid() && ReloadedRoot->TryGetArrayField(TEXT("Children"), ReloadedChildren) && ReloadedChildren && ReloadedChildren->Num() == 2
		&& (*ReloadedChildren)[0]->AsObject()->GetStringField(TEXT("Id")) == ForegroundTaskGuid
		&& (*ReloadedChildren)[1]->AsObject()->GetStringField(TEXT("Id")) == BackgroundCompositeGuid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeAttachmentsTest,
	"UEAssetDoc.BehaviorTree.GraphSource.Attachments",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeAttachmentsTest::RunTest(const FString&)
{
	BTGraphSourceTests::FPublicGraphFixture PublicGraphFixture;
	using namespace BTGraphShapeTests;
	UBehaviorTree* BehaviorTree = BTGraphSourceTests::CreateTree();
	bool bChanged = false;
	FAssetDocumentCapabilityResult Result = BTGraphSourceTests::ApplyTree(BehaviorTree, MakeAttachmentTree(), bChanged);
	TestTrue(TEXT("root/edge decorators and composite services apply"), Result.bSuccess);
	if (!Result.bSuccess)
	{
		AddError(Result.Message);
		return false;
	}

	UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(BehaviorTree->BTGraph);
	UBehaviorTreeGraphNode* Root = BTGraphSourceTests::FindGraphNodeByGuid(Graph, BTGraphSourceTests::RootGuid);
	UBehaviorTreeGraphNode* FirstTask = BTGraphSourceTests::FindGraphNodeByGuid(Graph, BTGraphSourceTests::FirstTaskGuid);
	TestEqual(TEXT("root wrapper keeps two decorators"), Root ? Root->Decorators.Num() : -1, 2);
	TestEqual(TEXT("edge wrapper keeps two decorators"), FirstTask ? FirstTask->Decorators.Num() : -1, 2);
	TestEqual(TEXT("composite wrapper keeps two services"), Root ? Root->Services.Num() : -1, 2);
	TestEqual(TEXT("root decorator order keeps first GUID"), Root && Root->Decorators.Num() == 2 ? Root->Decorators[0]->NodeGuid.ToString(EGuidFormats::Digits) : FString(), FString(RootDecoratorGuidA));
	TestEqual(TEXT("root decorator order keeps second GUID"), Root && Root->Decorators.Num() == 2 ? Root->Decorators[1]->NodeGuid.ToString(EGuidFormats::Digits) : FString(), FString(RootDecoratorGuidB));
	TestEqual(TEXT("edge decorator order keeps first GUID"), FirstTask && FirstTask->Decorators.Num() == 2 ? FirstTask->Decorators[0]->NodeGuid.ToString(EGuidFormats::Digits) : FString(), FString(EdgeDecoratorGuidA));
	TestEqual(TEXT("service order keeps first GUID"), Root && Root->Services.Num() == 2 ? Root->Services[0]->NodeGuid.ToString(EGuidFormats::Digits) : FString(), FString(RootServiceGuidA));
	TestEqual(TEXT("service order keeps second GUID"), Root && Root->Services.Num() == 2 ? Root->Services[1]->NodeGuid.ToString(EGuidFormats::Digits) : FString(), FString(RootServiceGuidB));
	TestEqual(TEXT("decorator property A is independent"), Root && Root->Decorators.Num() == 2 ? CastChecked<UBTDecorator>(Root->Decorators[0]->NodeInstance)->NodeName : FString(), FString(TEXT("Root decorator A")));
	TestEqual(TEXT("decorator property B is independent"), Root && Root->Decorators.Num() == 2 ? CastChecked<UBTDecorator>(Root->Decorators[1]->NodeInstance)->NodeName : FString(), FString(TEXT("Root decorator B")));
	TestEqual(TEXT("service property A is independent"), Root && Root->Services.Num() == 2 ? CastChecked<UBTService>(Root->Services[0]->NodeInstance)->NodeName : FString(), FString(TEXT("Root service A")));
	TestEqual(TEXT("service property B is independent"), Root && Root->Services.Num() == 2 ? CastChecked<UBTService>(Root->Services[1]->NodeInstance)->NodeName : FString(), FString(TEXT("Root service B")));
	TestEqual(TEXT("decorator wrapper comment persists"), Root && Root->Decorators.Num() == 2 ? Root->Decorators[0]->NodeComment : FString(), FString(TEXT("root decorator comment A")));
	TestTrue(TEXT("decorator wrapper pinned persists"), Root && Root->Decorators.Num() == 2 && Root->Decorators[0]->bCommentBubblePinned);
	TestEqual(TEXT("service wrapper comment persists"), Root && Root->Services.Num() == 2 ? Root->Services[0]->NodeComment : FString(), FString(TEXT("service comment A")));
	TestTrue(TEXT("service wrapper visible persists"), Root && Root->Services.Num() == 2 && Root->Services[0]->bCommentBubbleVisible);

	TestEqual(TEXT("runtime root decorators mirror graph order"), BehaviorTree->RootDecorators.Num(), 2);
	TestTrue(TEXT("runtime root decorator A mirrors wrapper"), Root && BehaviorTree->RootDecorators.Num() == 2 && BehaviorTree->RootDecorators[0] == Root->Decorators[0]->NodeInstance);
	TestTrue(TEXT("runtime root decorator B mirrors wrapper"), Root && BehaviorTree->RootDecorators.Num() == 2 && BehaviorTree->RootDecorators[1] == Root->Decorators[1]->NodeInstance);
	TestEqual(TEXT("runtime root services mirror graph count"), BehaviorTree->RootNode ? BehaviorTree->RootNode->Services.Num() : -1, 2);
	TestTrue(TEXT("runtime service order mirrors graph"), Root && BehaviorTree->RootNode && BehaviorTree->RootNode->Services.Num() == 2 && BehaviorTree->RootNode->Services[0] == Root->Services[0]->NodeInstance && BehaviorTree->RootNode->Services[1] == Root->Services[1]->NodeInstance);
	TestEqual(TEXT("runtime first edge decorators mirror graph count"), BehaviorTree->RootNode && BehaviorTree->RootNode->Children.Num() > 0 ? BehaviorTree->RootNode->Children[0].Decorators.Num() : -1, 2);

	TSharedPtr<FJsonObject> Extracted = BTGraphSourceTests::ExtractTree(*this, BehaviorTree);
	TSharedPtr<FJsonObject> ExtractedRoot = Extracted.IsValid() ? BTTestGetObjectField(Extracted, TEXT("Root")) : nullptr;
	TSharedPtr<FJsonObject> ExtractedFirst = ExtractedRoot.IsValid() ? BTGraphSourceTests::FindExtractedNodeById(ExtractedRoot, BTGraphSourceTests::FirstTaskGuid) : nullptr;
	TestNotNull(TEXT("root decorator A extracts by GUID"), FindAttachment(ExtractedRoot, TEXT("Decorators"), RootDecoratorGuidA).Get());
	TestNotNull(TEXT("root decorator B extracts by GUID"), FindAttachment(ExtractedRoot, TEXT("Decorators"), RootDecoratorGuidB).Get());
	TestNotNull(TEXT("edge decorator A extracts by GUID"), FindAttachment(ExtractedFirst, TEXT("Decorators"), EdgeDecoratorGuidA).Get());
	TestNotNull(TEXT("edge decorator B extracts by GUID"), FindAttachment(ExtractedFirst, TEXT("Decorators"), EdgeDecoratorGuidB).Get());
	TestNotNull(TEXT("service A extracts by GUID"), FindAttachment(ExtractedRoot, TEXT("Services"), RootServiceGuidA).Get());
	TestNotNull(TEXT("service B extracts by GUID"), FindAttachment(ExtractedRoot, TEXT("Services"), RootServiceGuidB).Get());

	TArray<TSharedPtr<FJsonValue>> DiffEntries;
	FAssetDocumentRegionContext Context = BTGraphSourceTests::MakeTreeContext(BehaviorTree);
	Result = BTGraphSourceTests::DiffTree(Context, MakeAttachmentTree(true, true).ToSharedRef(), DiffEntries);
	TestTrue(TEXT("attachment reorder diff succeeds"), Result.bSuccess);
	const FString RootNodePath = TEXT("/Body/Tree/Root");
	const auto HasPath = [&DiffEntries](const FString& Expected)
	{
		return DiffEntries.ContainsByPredicate([&Expected](const TSharedPtr<FJsonValue>& Value)
		{
			TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
			FString Path;
			return Entry.IsValid() && Entry->TryGetStringField(TEXT("path"), Path) && Path == Expected;
		});
	};
	TestTrue(TEXT("root decorator reorder uses identity-addressed diff"), HasPath(RootNodePath + TEXT("/Decorators")));
	TestTrue(TEXT("service reorder uses identity-addressed diff"), HasPath(RootNodePath + TEXT("/Services")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeCompositeExpressionsTest,
	"UEAssetDoc.BehaviorTree.GraphSource.CompositeExpressions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeCompositeExpressionsTest::RunTest(const FString&)
{
	BTGraphSourceTests::FPublicGraphFixture PublicGraphFixture;
	using namespace BTGraphShapeTests;
	struct FExpressionCase
	{
		FString Shape;
		TArray<EBTDecoratorLogic::Type> Operations;
		TArray<int32> Numbers;
	};
	const TArray<FExpressionCase> Cases = {
		{TEXT("Test"), {EBTDecoratorLogic::Test}, {0}},
		{TEXT("Not"), {EBTDecoratorLogic::Not, EBTDecoratorLogic::Test}, {1, 0}},
		{TEXT("And"), {EBTDecoratorLogic::And, EBTDecoratorLogic::Test, EBTDecoratorLogic::Test}, {2, 0, 1}},
		{TEXT("Or"), {EBTDecoratorLogic::Or, EBTDecoratorLogic::Test, EBTDecoratorLogic::Test}, {2, 0, 1}},
		{TEXT("And3"), {EBTDecoratorLogic::And, EBTDecoratorLogic::Test, EBTDecoratorLogic::Test, EBTDecoratorLogic::Test}, {3, 0, 1, 2}},
		{TEXT("Or3"), {EBTDecoratorLogic::Or, EBTDecoratorLogic::Test, EBTDecoratorLogic::Test, EBTDecoratorLogic::Test}, {3, 0, 1, 2}},
		{TEXT("NestedNotOr"), {EBTDecoratorLogic::Not, EBTDecoratorLogic::Or, EBTDecoratorLogic::Test, EBTDecoratorLogic::Test}, {1, 2, 0, 1}}};

	for (const FExpressionCase& Case : Cases)
	{
		UBehaviorTree* BehaviorTree = BTGraphSourceTests::CreateTree();
		bool bChanged = false;
		const FAssetDocumentCapabilityResult Result = BTGraphSourceTests::ApplyTree(BehaviorTree, MakeCompositeTree(Case.Shape), bChanged);
		TestTrue(*FString::Printf(TEXT("%s composite expression applies"), *Case.Shape), Result.bSuccess);
		if (!Result.bSuccess)
		{
			AddError(Result.Message);
			continue;
		}
		UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(BehaviorTree->BTGraph);
		UBehaviorTreeGraphNode_CompositeDecorator* Composite = Cast<UBehaviorTreeGraphNode_CompositeDecorator>(FindSubNodeByGuid(Graph, CompositeDecoratorGuid));
		TestNotNull(*FString::Printf(TEXT("%s creates a composite decorator wrapper"), *Case.Shape), Composite);
		UBehaviorTreeDecoratorGraph* BoundGraph = Composite ? Cast<UBehaviorTreeDecoratorGraph>(Composite->BoundGraph) : nullptr;
		TestNotNull(*FString::Printf(TEXT("%s owns a real UBehaviorTreeDecoratorGraph"), *Case.Shape), BoundGraph);
		TestEqual(*FString::Printf(TEXT("%s BoundGraph GUID is stable"), *Case.Shape), BoundGraph ? BoundGraph->GraphGuid.ToString(EGuidFormats::Digits) : FString(), FString(BoundGraphGuid));
		TArray<UBTDecorator*> Instances;
		TArray<FBTDecoratorLogic> Operations;
		if (Composite)
		{
			Composite->CollectDecoratorData(Instances, Operations);
		}
		TestEqual(*FString::Printf(TEXT("%s derived operation count"), *Case.Shape), Operations.Num(), Case.Operations.Num());
		for (int32 Index = 0; Index < FMath::Min(Operations.Num(), Case.Operations.Num()); ++Index)
		{
			TestEqual(*FString::Printf(TEXT("%s derived operation %d"), *Case.Shape, Index), static_cast<int32>(Operations[Index].Operation), static_cast<int32>(Case.Operations[Index]));
			TestEqual(*FString::Printf(TEXT("%s derived operation number %d"), *Case.Shape, Index), static_cast<int32>(Operations[Index].Number), Case.Numbers[Index]);
		}
		const FBTCompositeChild* RuntimeChild = BehaviorTree->RootNode && BehaviorTree->RootNode->Children.Num() > 0 ? &BehaviorTree->RootNode->Children[0] : nullptr;
		TestEqual(*FString::Printf(TEXT("%s runtime ops are standard-rebuild derived"), *Case.Shape), RuntimeChild ? RuntimeChild->DecoratorOps.Num() : -1, Case.Operations.Num());
		if (RuntimeChild)
		{
			for (int32 Index = 0; Index < FMath::Min(RuntimeChild->DecoratorOps.Num(), Case.Operations.Num()); ++Index)
			{
				TestEqual(*FString::Printf(TEXT("%s runtime operation %d is exact"), *Case.Shape, Index), static_cast<int32>(RuntimeChild->DecoratorOps[Index].Operation), static_cast<int32>(Case.Operations[Index]));
				TestEqual(*FString::Printf(TEXT("%s runtime operation number/index %d is exact"), *Case.Shape, Index), static_cast<int32>(RuntimeChild->DecoratorOps[Index].Number), Case.Numbers[Index]);
			}
		}

		TSharedPtr<FJsonObject> Extracted = BTGraphSourceTests::ExtractTree(*this, BehaviorTree);
		TSharedPtr<FJsonObject> ExtractedRoot = Extracted.IsValid() ? BTTestGetObjectField(Extracted, TEXT("Root")) : nullptr;
		TSharedPtr<FJsonObject> ExtractedFirst = BTGraphSourceTests::FindExtractedNodeById(ExtractedRoot, BTGraphSourceTests::FirstTaskGuid);
		TSharedPtr<FJsonObject> ExtractedComposite = FindAttachment(ExtractedFirst, TEXT("Decorators"), CompositeDecoratorGuid);
		TSharedPtr<FJsonObject> ExtractedBound = BTTestGetObjectField(ExtractedComposite, TEXT("BoundGraph"));
		TestEqual(*FString::Printf(TEXT("%s extracts stable BoundGraph GUID"), *Case.Shape), ExtractedBound.IsValid() ? ExtractedBound->GetStringField(TEXT("GraphGuid")) : FString(), FString(BoundGraphGuid));
		const TArray<TSharedPtr<FJsonValue>>* ExtractedLinks = nullptr;
		TestTrue(*FString::Printf(TEXT("%s extracts canonical links"), *Case.Shape), ExtractedBound.IsValid() && ExtractedBound->TryGetArrayField(TEXT("Links"), ExtractedLinks) && ExtractedLinks && ExtractedLinks->Num() == MakeBoundGraph(Case.Shape)->GetArrayField(TEXT("Links")).Num());
	}

	TSharedPtr<FJsonObject> RootCompositeTree = BTGraphSourceTests::MakeGraphSourceTree();
	BTTestGetObjectField(RootCompositeTree, TEXT("Root"))->SetArrayField(
		TEXT("Decorators"),
		{BTTestMakeObjectValue(MakeCompositeDecorator(MakeBoundGraph(TEXT("And3"))))});
	UBehaviorTree* RootCompositeAsset = BTGraphSourceTests::CreateTree();
	bool bRootChanged = false;
	FAssetDocumentCapabilityResult RootResult = BTGraphSourceTests::ApplyTree(RootCompositeAsset, RootCompositeTree, bRootChanged);
	TestTrue(TEXT("root three-input composite decorator applies"), RootResult.bSuccess);
	const TArray<EBTDecoratorLogic::Type> RootExpectedOperations = {
		EBTDecoratorLogic::And,
		EBTDecoratorLogic::Test,
		EBTDecoratorLogic::Test,
		EBTDecoratorLogic::Test};
	const TArray<int32> RootExpectedNumbers = {3, 0, 1, 2};
	TestEqual(TEXT("root composite runtime ops are not hidden by ordinary-root exception"), RootCompositeAsset->RootDecoratorOps.Num(), RootExpectedOperations.Num());
	for (int32 Index = 0; Index < FMath::Min(RootCompositeAsset->RootDecoratorOps.Num(), RootExpectedOperations.Num()); ++Index)
	{
		TestEqual(*FString::Printf(TEXT("root composite runtime operation %d is exact"), Index), static_cast<int32>(RootCompositeAsset->RootDecoratorOps[Index].Operation), static_cast<int32>(RootExpectedOperations[Index]));
		TestEqual(*FString::Printf(TEXT("root composite runtime number/index %d is exact"), Index), static_cast<int32>(RootCompositeAsset->RootDecoratorOps[Index].Number), RootExpectedNumbers[Index]);
	}

	const FString Target = BTTestMakeUniqueTarget(TEXT("BT_GraphShape_CompositeReload"));
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	FScopedBehaviorTreeMutationFixture Fixture;
	const FAssetDocumentResult ApplyResult = Fixture.Apply(Service, Target, BTTestMakeBehaviorTreeDocument(Target, BTGraphSourceTests::MakeGraphSourceBody(MakeCompositeTree(TEXT("NestedNotOr")))));
	TestTrue(TEXT("nested composite expression saves"), ApplyResult.IsSuccess());
	UBehaviorTree* BeforeReload = BTTestLoadBehaviorTreeForTarget(Target);
	UPackage* Package = BeforeReload ? BeforeReload->GetOutermost() : nullptr;
	TestTrue(TEXT("nested composite package unloads"), Package && UPackageTools::UnloadPackages({Package}));
	UBehaviorTree* Reloaded = BTTestLoadBehaviorTreeForTarget(Target);
	UBehaviorTreeGraphNode_CompositeDecorator* ReloadedComposite = Reloaded
		? Cast<UBehaviorTreeGraphNode_CompositeDecorator>(FindSubNodeByGuid(Cast<UBehaviorTreeGraph>(Reloaded->BTGraph), CompositeDecoratorGuid))
		: nullptr;
	TestNotNull(TEXT("composite wrapper GUID survives save/reload"), ReloadedComposite);
	UBehaviorTreeDecoratorGraph* ReloadedBound = ReloadedComposite ? Cast<UBehaviorTreeDecoratorGraph>(ReloadedComposite->BoundGraph) : nullptr;
	TestEqual(TEXT("BoundGraph GUID survives save/reload"), ReloadedBound ? ReloadedBound->GraphGuid.ToString(EGuidFormats::Digits) : FString(), FString(BoundGraphGuid));
	TSet<FString> ReloadedNodeGuids;
	if (ReloadedBound)
	{
		for (UEdGraphNode* Node : ReloadedBound->Nodes)
		{
			ReloadedNodeGuids.Add(Node->NodeGuid.ToString(EGuidFormats::Digits));
		}
	}
	TestTrue(TEXT("Sink GUID survives save/reload"), ReloadedNodeGuids.Contains(SinkGuid));
	TestTrue(TEXT("Not GUID survives save/reload"), ReloadedNodeGuids.Contains(LogicGuidA));
	TestTrue(TEXT("Or GUID survives save/reload"), ReloadedNodeGuids.Contains(LogicGuidB));
	TestTrue(TEXT("Test A GUID survives save/reload"), ReloadedNodeGuids.Contains(TestGuidA));
	TestTrue(TEXT("Test B GUID survives save/reload"), ReloadedNodeGuids.Contains(TestGuidB));

	for (const FString& ThreeInputShape : {FString(TEXT("And3")), FString(TEXT("Or3"))})
	{
		const FString ThreeInputTarget = FString::Printf(
			TEXT("/Game/AssetDocumentTests/BT_GraphShape_%s_Reload_%s"),
			*ThreeInputShape,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		const FAssetDocumentResult ThreeInputApply = Fixture.Apply(
			Service,
			ThreeInputTarget,
			BTTestMakeBehaviorTreeDocument(
				ThreeInputTarget,
				BTGraphSourceTests::MakeGraphSourceBody(MakeCompositeTree(ThreeInputShape))));
		TestTrue(*FString::Printf(TEXT("%s three-input composite saves"), *ThreeInputShape), ThreeInputApply.IsSuccess());
		UBehaviorTree* ThreeInputBeforeReload = BTTestLoadBehaviorTreeForTarget(ThreeInputTarget);
		UPackage* ThreeInputPackage = ThreeInputBeforeReload ? ThreeInputBeforeReload->GetOutermost() : nullptr;
		TestTrue(*FString::Printf(TEXT("%s three-input package unloads"), *ThreeInputShape), ThreeInputPackage && UPackageTools::UnloadPackages({ThreeInputPackage}));
		UBehaviorTree* ThreeInputReloaded = BTTestLoadBehaviorTreeForTarget(ThreeInputTarget);
		UBehaviorTreeGraphNode_CompositeDecorator* ThreeInputComposite = ThreeInputReloaded
			? Cast<UBehaviorTreeGraphNode_CompositeDecorator>(FindSubNodeByGuid(Cast<UBehaviorTreeGraph>(ThreeInputReloaded->BTGraph), CompositeDecoratorGuid))
			: nullptr;
		TArray<UBTDecorator*> ThreeInputInstances;
		TArray<FBTDecoratorLogic> ThreeInputOperations;
		if (ThreeInputComposite)
		{
			ThreeInputComposite->CollectDecoratorData(ThreeInputInstances, ThreeInputOperations);
		}
		const EBTDecoratorLogic::Type ExpectedLogic = ThreeInputShape == TEXT("And3") ? EBTDecoratorLogic::And : EBTDecoratorLogic::Or;
		const TArray<EBTDecoratorLogic::Type> ExpectedOperations = {ExpectedLogic, EBTDecoratorLogic::Test, EBTDecoratorLogic::Test, EBTDecoratorLogic::Test};
		const TArray<int32> ExpectedNumbers = {3, 0, 1, 2};
		TestEqual(*FString::Printf(TEXT("%s reload graph operation count is exact"), *ThreeInputShape), ThreeInputOperations.Num(), ExpectedOperations.Num());
		for (int32 Index = 0; Index < FMath::Min(ThreeInputOperations.Num(), ExpectedOperations.Num()); ++Index)
		{
			TestEqual(*FString::Printf(TEXT("%s reload graph operation %d is exact"), *ThreeInputShape, Index), static_cast<int32>(ThreeInputOperations[Index].Operation), static_cast<int32>(ExpectedOperations[Index]));
			TestEqual(*FString::Printf(TEXT("%s reload graph number/index %d is exact"), *ThreeInputShape, Index), static_cast<int32>(ThreeInputOperations[Index].Number), ExpectedNumbers[Index]);
		}
		const FBTCompositeChild* ThreeInputRuntimeChild = ThreeInputReloaded && ThreeInputReloaded->RootNode && ThreeInputReloaded->RootNode->Children.Num() > 0
			? &ThreeInputReloaded->RootNode->Children[0]
			: nullptr;
		TestEqual(*FString::Printf(TEXT("%s reload runtime operation count is exact"), *ThreeInputShape), ThreeInputRuntimeChild ? ThreeInputRuntimeChild->DecoratorOps.Num() : -1, ExpectedOperations.Num());
		if (ThreeInputRuntimeChild)
		{
			for (int32 Index = 0; Index < FMath::Min(ThreeInputRuntimeChild->DecoratorOps.Num(), ExpectedOperations.Num()); ++Index)
			{
				TestEqual(*FString::Printf(TEXT("%s reload runtime operation %d is exact"), *ThreeInputShape, Index), static_cast<int32>(ThreeInputRuntimeChild->DecoratorOps[Index].Operation), static_cast<int32>(ExpectedOperations[Index]));
				TestEqual(*FString::Printf(TEXT("%s reload runtime number/index %d is exact"), *ThreeInputShape, Index), static_cast<int32>(ThreeInputRuntimeChild->DecoratorOps[Index].Number), ExpectedNumbers[Index]);
			}
		}
		TArray<TSharedPtr<FJsonValue>> ThreeInputDiff;
		FAssetDocumentCapabilityResult ThreeInputDiffResult = BTGraphSourceTests::DiffTree(
			BTGraphSourceTests::MakeTreeContext(ThreeInputReloaded),
			MakeCompositeTree(ThreeInputShape).ToSharedRef(),
			ThreeInputDiff);
		TestTrue(*FString::Printf(TEXT("%s reload roundtrip diff succeeds"), *ThreeInputShape), ThreeInputDiffResult.bSuccess);
		TestEqual(*FString::Printf(TEXT("%s reload roundtrip is canonical no-op"), *ThreeInputShape), ThreeInputDiff.Num(), 0);
	}

	TSharedPtr<FJsonObject> IdentityTree = MakeCompositeTree(TEXT("NestedNotOr"));
	IdentityTree->SetArrayField(TEXT("Comments"), {
		BTTestMakeObjectValue(MakeComment(CommentGuid, TEXT("Identity comment A"), -320.0)),
		BTTestMakeObjectValue(MakeComment(CommentGuidB, TEXT("Identity comment B"), -120.0))});
	UBehaviorTree* IdentityAsset = BTGraphSourceTests::CreateTree();
	bool bIdentityChanged = false;
	FAssetDocumentCapabilityResult IdentityResult = BTGraphSourceTests::ApplyTree(IdentityAsset, IdentityTree, bIdentityChanged);
	TestTrue(TEXT("identity reorder baseline applies"), IdentityResult.bSuccess);
	TSharedPtr<FJsonObject> IdentityExtracted = IdentityResult.bSuccess ? BTGraphSourceTests::ExtractTree(*this, IdentityAsset) : nullptr;
	TSharedPtr<FJsonObject> IdentityRoot = IdentityExtracted.IsValid() ? BTTestGetObjectField(IdentityExtracted, TEXT("Root")) : nullptr;
	TSharedPtr<FJsonObject> IdentityFirst = IdentityRoot.IsValid() ? BTGraphSourceTests::FindExtractedNodeById(IdentityRoot, BTGraphSourceTests::FirstTaskGuid) : nullptr;
	TSharedPtr<FJsonObject> IdentityComposite = FindAttachment(IdentityFirst, TEXT("Decorators"), CompositeDecoratorGuid);
	TSharedPtr<FJsonObject> IdentityBound = BTTestGetObjectField(IdentityComposite, TEXT("BoundGraph"));
	const TArray<TSharedPtr<FJsonValue>>* CanonicalBoundNodes = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* CanonicalLinks = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* CanonicalComments = nullptr;
	TestTrue(TEXT("canonical identity extract exposes BoundGraph nodes"), IdentityBound.IsValid() && IdentityBound->TryGetArrayField(TEXT("Nodes"), CanonicalBoundNodes) && CanonicalBoundNodes);
	TestTrue(TEXT("canonical identity extract exposes BoundGraph links"), IdentityBound.IsValid() && IdentityBound->TryGetArrayField(TEXT("Links"), CanonicalLinks) && CanonicalLinks);
	TestTrue(TEXT("canonical identity extract exposes comments"), IdentityExtracted.IsValid() && IdentityExtracted->TryGetArrayField(TEXT("Comments"), CanonicalComments) && CanonicalComments);
	FString PreviousKey;
	if (CanonicalBoundNodes)
	{
		for (const TSharedPtr<FJsonValue>& NodeValue : *CanonicalBoundNodes)
		{
			const FString Key = NodeValue->AsObject()->GetStringField(TEXT("Id"));
			TestTrue(TEXT("BoundGraph nodes extract in canonical identity order"), PreviousKey.IsEmpty() || Key.Compare(PreviousKey) >= 0);
			PreviousKey = Key;
		}
	}
	PreviousKey.Reset();
	if (CanonicalComments)
	{
		for (const TSharedPtr<FJsonValue>& CommentValue : *CanonicalComments)
		{
			const FString Key = CommentValue->AsObject()->GetStringField(TEXT("Id"));
			TestTrue(TEXT("comments extract in canonical identity order"), PreviousKey.IsEmpty() || Key.Compare(PreviousKey) >= 0);
			PreviousKey = Key;
		}
	}
	PreviousKey.Reset();
	if (CanonicalLinks)
	{
		for (const TSharedPtr<FJsonValue>& LinkValue : *CanonicalLinks)
		{
			const TSharedPtr<FJsonObject> Link = LinkValue->AsObject();
			const FString Key = FString::Printf(
				TEXT("%s|%s|%010d"),
				*Link->GetStringField(TEXT("From")),
				*Link->GetStringField(TEXT("To")),
				static_cast<int32>(Link->GetNumberField(TEXT("ToInput"))));
			TestTrue(TEXT("BoundGraph links extract in canonical semantic-key order"), PreviousKey.IsEmpty() || Key.Compare(PreviousKey) >= 0);
			PreviousKey = Key;
		}
	}

	TSharedPtr<FJsonObject> ReorderedIdentityTree = MakeCompositeTree(TEXT("NestedNotOr"));
	ReorderedIdentityTree->SetArrayField(TEXT("Comments"), {
		BTTestMakeObjectValue(MakeComment(CommentGuidB, TEXT("Identity comment B"), -120.0)),
		BTTestMakeObjectValue(MakeComment(CommentGuid, TEXT("Identity comment A"), -320.0))});
	TSharedPtr<FJsonObject> ReorderedFirst = FirstChild(ReorderedIdentityTree);
	TSharedPtr<FJsonObject> ReorderedComposite = FindAttachment(ReorderedFirst, TEXT("Decorators"), CompositeDecoratorGuid);
	TSharedPtr<FJsonObject> ReorderedBound = BTTestGetObjectField(ReorderedComposite, TEXT("BoundGraph"));
	const auto ReverseArrayField = [](const TSharedPtr<FJsonObject>& Object, const FString& Field)
	{
		TArray<TSharedPtr<FJsonValue>> Values = Object->GetArrayField(Field);
		for (int32 Left = 0, Right = Values.Num() - 1; Left < Right; ++Left, --Right)
		{
			Values.Swap(Left, Right);
		}
		Object->SetArrayField(Field, Values);
	};
	ReverseArrayField(ReorderedBound, TEXT("Nodes"));
	ReverseArrayField(ReorderedBound, TEXT("Links"));
	TArray<TSharedPtr<FJsonValue>> IdentityDiff;
	IdentityResult = BTGraphSourceTests::DiffTree(
		BTGraphSourceTests::MakeTreeContext(IdentityAsset),
		ReorderedIdentityTree.ToSharedRef(),
		IdentityDiff);
	TestTrue(TEXT("identity collection reorder diff succeeds"), IdentityResult.bSuccess);
	TestEqual(TEXT("BoundGraph node/link and comment storage reorder is a no-op diff"), IdentityDiff.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeCompositeValidationTest,
	"UEAssetDoc.BehaviorTree.GraphSource.CompositeValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeCompositeValidationTest::RunTest(const FString&)
{
	BTGraphSourceTests::FPublicGraphFixture PublicGraphFixture;
	using namespace BTGraphShapeTests;
	struct FInvalidCase
	{
		FString Label;
		TSharedPtr<FJsonObject> Tree;
		FString Path;
		FString Code;
	};
	const auto BoundGraphFromTree = [](const TSharedPtr<FJsonObject>& Tree)
	{
		TSharedPtr<FJsonObject> Child = FirstChild(Tree);
		const TArray<TSharedPtr<FJsonValue>>* Decorators = nullptr;
		if (!Child.IsValid() || !Child->TryGetArrayField(TEXT("Decorators"), Decorators) || !Decorators || Decorators->Num() == 0)
		{
			return TSharedPtr<FJsonObject>();
		}
		return BTTestGetObjectField((*Decorators)[0]->AsObject(), TEXT("BoundGraph"));
	};
	const FString BasePath = TEXT("/Body/Tree/Root/Children/0/Decorators/0/BoundGraph");
	TArray<FInvalidCase> Cases;

	TSharedPtr<FJsonObject> CycleTree = MakeCompositeTree(TEXT("Not"));
	TSharedPtr<FJsonObject> CycleGraph = BoundGraphFromTree(CycleTree);
	CycleGraph->SetArrayField(TEXT("Links"), {MakeLink(LogicGuidA, TestGuidA, 0), MakeLink(TestGuidA, LogicGuidA, 0)});
	Cases.Add({TEXT("cycle"), CycleTree, BasePath + TEXT("/Links/1"), TEXT("BehaviorTreeDecoratorGraphCycle")});

	TSharedPtr<FJsonObject> WrongNotTree = MakeCompositeTree(TEXT("Not"));
	TSharedPtr<FJsonObject> WrongNotGraph = BoundGraphFromTree(WrongNotTree);
	TArray<TSharedPtr<FJsonValue>> WrongNotNodes = WrongNotGraph->GetArrayField(TEXT("Nodes"));
	WrongNotNodes.Add(BTTestMakeObjectValue(MakeBoundNode(TestGuidB, TEXT("Test"), 100.0, 100.0, TEXT("Test B"))));
	WrongNotGraph->SetArrayField(TEXT("Nodes"), WrongNotNodes);
	TArray<TSharedPtr<FJsonValue>> WrongNotLinks = WrongNotGraph->GetArrayField(TEXT("Links"));
	WrongNotLinks.Insert(MakeLink(TestGuidB, LogicGuidA, 1), 1);
	WrongNotGraph->SetArrayField(TEXT("Links"), WrongNotLinks);
	Cases.Add({TEXT("wrong Not arity"), WrongNotTree, BasePath + TEXT("/Nodes/1"), TEXT("InvalidBehaviorTreeDecoratorArity")});

	TSharedPtr<FJsonObject> WrongAndTree = MakeCompositeTree(TEXT("And"));
	TSharedPtr<FJsonObject> WrongAndGraph = BoundGraphFromTree(WrongAndTree);
	TArray<TSharedPtr<FJsonValue>> WrongAndLinks = WrongAndGraph->GetArrayField(TEXT("Links"));
	WrongAndLinks.RemoveAt(1);
	WrongAndGraph->SetArrayField(TEXT("Links"), WrongAndLinks);
	Cases.Add({TEXT("wrong And arity"), WrongAndTree, BasePath + TEXT("/Nodes/1"), TEXT("InvalidBehaviorTreeDecoratorArity")});

	TSharedPtr<FJsonObject> WrongOrTree = MakeCompositeTree(TEXT("Or"));
	TSharedPtr<FJsonObject> WrongOrGraph = BoundGraphFromTree(WrongOrTree);
	TArray<TSharedPtr<FJsonValue>> WrongOrLinks = WrongOrGraph->GetArrayField(TEXT("Links"));
	WrongOrLinks.RemoveAt(1);
	WrongOrGraph->SetArrayField(TEXT("Links"), WrongOrLinks);
	Cases.Add({TEXT("wrong Or arity"), WrongOrTree, BasePath + TEXT("/Nodes/1"), TEXT("InvalidBehaviorTreeDecoratorArity")});

	TSharedPtr<FJsonObject> DanglingTree = MakeCompositeTree(TEXT("Test"));
	TSharedPtr<FJsonObject> DanglingGraph = BoundGraphFromTree(DanglingTree);
	DanglingGraph->SetArrayField(TEXT("Links"), {MakeLink(TEXT("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), SinkGuid, 0)});
	Cases.Add({TEXT("dangling endpoint"), DanglingTree, BasePath + TEXT("/Links/0/From"), TEXT("DanglingBehaviorTreeDecoratorLink")});

	TSharedPtr<FJsonObject> DuplicateTree = MakeCompositeTree(TEXT("Test"));
	TSharedPtr<FJsonObject> DuplicateGraph = BoundGraphFromTree(DuplicateTree);
	TArray<TSharedPtr<FJsonValue>> DuplicateNodes = DuplicateGraph->GetArrayField(TEXT("Nodes"));
	DuplicateNodes.Add(BTTestMakeObjectValue(MakeBoundNode(TestGuidA, TEXT("Test"), 0.0, 100.0, TEXT("Duplicate"))));
	DuplicateGraph->SetArrayField(TEXT("Nodes"), DuplicateNodes);
	Cases.Add({TEXT("duplicate GUID"), DuplicateTree, BasePath + TEXT("/Nodes/2/Id"), TEXT("DuplicateBehaviorTreeNodeGuid")});

	TSharedPtr<FJsonObject> MultipleLinkTree = MakeCompositeTree(TEXT("Not"));
	TSharedPtr<FJsonObject> MultipleLinkGraph = BoundGraphFromTree(MultipleLinkTree);
	TArray<TSharedPtr<FJsonValue>> MultipleLinks = MultipleLinkGraph->GetArrayField(TEXT("Links"));
	MultipleLinks.Add(MakeLink(TestGuidA, SinkGuid, 0));
	MultipleLinkGraph->SetArrayField(TEXT("Links"), MultipleLinks);
	Cases.Add({TEXT("multiple outgoing links"), MultipleLinkTree, BasePath + TEXT("/Links/2/From"), TEXT("MultipleBehaviorTreeDecoratorLinks")});

	TSharedPtr<FJsonObject> UnreachableTree = MakeCompositeTree(TEXT("Test"));
	TSharedPtr<FJsonObject> UnreachableGraph = BoundGraphFromTree(UnreachableTree);
	TArray<TSharedPtr<FJsonValue>> UnreachableNodes = UnreachableGraph->GetArrayField(TEXT("Nodes"));
	UnreachableNodes.Add(BTTestMakeObjectValue(MakeBoundNode(TestGuidB, TEXT("Test"), 0.0, 100.0, TEXT("Unreachable"))));
	UnreachableGraph->SetArrayField(TEXT("Nodes"), UnreachableNodes);
	Cases.Add({TEXT("unreachable node"), UnreachableTree, BasePath + TEXT("/Nodes/2"), TEXT("UnreachableBehaviorTreeDecoratorNode")});

	UBehaviorTree* Existing = BTGraphSourceTests::CreateTree();
	bool bChanged = false;
	FAssetDocumentCapabilityResult Result = BTGraphSourceTests::ApplyTree(Existing, MakeCompositeTree(TEXT("NestedNotOr")), bChanged);
	TestTrue(TEXT("valid rollback baseline applies"), Result.bSuccess);
	const FString OriginalCanonical = ComparableTree(Existing);
	UBehaviorTreeGraph* OriginalGraph = Cast<UBehaviorTreeGraph>(Existing->BTGraph);
	UBehaviorTreeGraphNode_CompositeDecorator* OriginalComposite = Cast<UBehaviorTreeGraphNode_CompositeDecorator>(FindSubNodeByGuid(OriginalGraph, CompositeDecoratorGuid));
	UEdGraph* OriginalBoundGraph = OriginalComposite ? OriginalComposite->BoundGraph : nullptr;

	for (const FInvalidCase& Case : Cases)
	{
		bChanged = false;
		Result = BTGraphSourceTests::ApplyTree(Existing, Case.Tree, bChanged);
		TestFalse(*FString::Printf(TEXT("%s is rejected"), *Case.Label), Result.bSuccess);
		TestTrue(*FString::Printf(TEXT("%s reports exact JSON pointer"), *Case.Label), BTTestHasDiagnostic(Result, Case.Path, Case.Code));
		TestFalse(*FString::Printf(TEXT("%s reports no change"), *Case.Label), bChanged);
		TestTrue(*FString::Printf(TEXT("%s leaves main graph object unchanged"), *Case.Label), Existing->BTGraph == OriginalGraph);
		TestTrue(*FString::Printf(TEXT("%s leaves composite wrapper unchanged"), *Case.Label), FindSubNodeByGuid(OriginalGraph, CompositeDecoratorGuid) == OriginalComposite);
		TestTrue(*FString::Printf(TEXT("%s leaves BoundGraph unchanged"), *Case.Label), OriginalComposite && OriginalComposite->BoundGraph == OriginalBoundGraph);
		TestEqual(*FString::Printf(TEXT("%s leaves canonical extract unchanged"), *Case.Label), ComparableTree(Existing), OriginalCanonical);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeCommentsTest,
	"UEAssetDoc.BehaviorTree.GraphSource.Comments",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeCommentsTest::RunTest(const FString&)
{
	BTGraphSourceTests::FPublicGraphFixture PublicGraphFixture;
	using namespace BTGraphShapeTests;
	const FString Target = BTTestMakeUniqueTarget(TEXT("BT_GraphShape_CommentsReload"));
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	FScopedBehaviorTreeMutationFixture Fixture;
	FAssetDocumentResult ApplyResult = Fixture.Apply(Service, Target, BTTestMakeBehaviorTreeDocument(Target, BTGraphSourceTests::MakeGraphSourceBody(MakeCommentTree())));
	TestTrue(TEXT("full comment surface saves"), ApplyResult.IsSuccess());
	if (!ApplyResult.IsSuccess())
	{
		AddError(ApplyResult.Message);
		return false;
	}
	UBehaviorTree* BeforeReload = BTTestLoadBehaviorTreeForTarget(Target);
	UPackage* Package = BeforeReload ? BeforeReload->GetOutermost() : nullptr;
	TestTrue(TEXT("comment fixture package unloads"), Package && UPackageTools::UnloadPackages({Package}));
	UBehaviorTree* Reloaded = BTTestLoadBehaviorTreeForTarget(Target);
	TestNotNull(TEXT("comment fixture reloads"), Reloaded);
	UBehaviorTreeGraph* Graph = Reloaded ? Cast<UBehaviorTreeGraph>(Reloaded->BTGraph) : nullptr;
	UBehaviorTreeGraphNode* Root = BTGraphSourceTests::FindGraphNodeByGuid(Graph, BTGraphSourceTests::RootGuid);
	UBehaviorTreeGraphNode* Task = BTGraphSourceTests::FindGraphNodeByGuid(Graph, BTGraphSourceTests::FirstTaskGuid);
	UBehaviorTreeGraphNode* Decorator = FindSubNodeByGuid(Graph, RootDecoratorGuidA);
	UBehaviorTreeGraphNode* ServiceNode = FindSubNodeByGuid(Graph, RootServiceGuidA);
	TestEqual(TEXT("composite NodeComment survives reload"), Root ? Root->NodeComment : FString(), FString(TEXT("root composite comment")));
	TestTrue(TEXT("composite pinned survives reload"), Root && Root->bCommentBubblePinned && !Root->bCommentBubbleVisible);
	TestEqual(TEXT("task NodeComment survives reload"), Task ? Task->NodeComment : FString(), FString(TEXT("task comment")));
	TestTrue(TEXT("task visible survives reload"), Task && !Task->bCommentBubblePinned && Task->bCommentBubbleVisible);
	TestEqual(TEXT("decorator NodeComment survives reload"), Decorator ? Decorator->NodeComment : FString(), FString(TEXT("root decorator comment A")));
	TestEqual(TEXT("service NodeComment survives reload"), ServiceNode ? ServiceNode->NodeComment : FString(), FString(TEXT("service comment A")));

	UEdGraphNode_Comment* Comment = FindComment(Graph);
	TestNotNull(TEXT("comment box survives reload"), Comment);
	TestEqual(TEXT("comment text maps inherited NodeComment"), Comment ? Comment->NodeComment : FString(), FString(TEXT("Inherited comment text")));
	TestEqual(TEXT("comment X survives reload"), Comment ? Comment->NodePosX : 0, -320);
	TestEqual(TEXT("comment Y survives reload"), Comment ? Comment->NodePosY : 0, -180);
	TestEqual(TEXT("comment width survives reload"), Comment ? Comment->NodeWidth : 0, 640);
	TestEqual(TEXT("comment height survives reload"), Comment ? Comment->NodeHeight : 0, 360);
	TestEqual(TEXT("comment red survives reload"), Comment ? Comment->CommentColor.R : 0.0f, 0.1f);
	TestEqual(TEXT("comment green survives reload"), Comment ? Comment->CommentColor.G : 0.0f, 0.2f);
	TestEqual(TEXT("comment blue survives reload"), Comment ? Comment->CommentColor.B : 0.0f, 0.3f);
	TestEqual(TEXT("comment alpha survives reload"), Comment ? Comment->CommentColor.A : 0.0f, 0.4f);
	TestEqual(TEXT("comment depth survives reload"), Comment ? Comment->CommentDepth : 0, 7);
	TestEqual(TEXT("comment font size survives reload"), Comment ? Comment->FontSize : 0, 23);
	TestEqual(TEXT("comment move mode survives reload"), Comment ? static_cast<int32>(Comment->MoveMode.GetValue()) : -1, static_cast<int32>(ECommentBoxMode::NoGroupMovement));
	TestEqual(TEXT("comment details FText canonically survives reload"), Comment ? Comment->NodeDetails.ToString() : FString(), FString(TEXT("Stable localized details")));
	TestTrue(TEXT("inherited comment bubble fields survive reload"), Comment && Comment->bCommentBubblePinned && !Comment->bCommentBubbleVisible);
	TestTrue(TEXT("comment-specific bubble fields survive reload"), Comment && Comment->bCommentBubbleVisible_InDetailsPanel && Comment->bColorCommentBubble);

	TSharedPtr<FJsonObject> Extracted = BTGraphSourceTests::ExtractTree(*this, Reloaded);
	const TArray<TSharedPtr<FJsonValue>>* ExtractedComments = nullptr;
	TestTrue(TEXT("canonical extract includes one comment"), Extracted.IsValid() && Extracted->TryGetArrayField(TEXT("Comments"), ExtractedComments) && ExtractedComments && ExtractedComments->Num() == 1);
	TSharedPtr<FJsonObject> ExtractedComment = ExtractedComments && ExtractedComments->Num() == 1 ? (*ExtractedComments)[0]->AsObject() : nullptr;
	TestEqual(TEXT("canonical comment identity is GUID"), ExtractedComment.IsValid() ? ExtractedComment->GetStringField(TEXT("Id")) : FString(), FString(CommentGuid));
	TestEqual(TEXT("canonical NodeDetails is stable string"), ExtractedComment.IsValid() ? ExtractedComment->GetStringField(TEXT("NodeDetails")) : FString(), FString(TEXT("Stable localized details")));
	TestFalse(TEXT("transient bCommentBubbleMakeVisible is omitted"), ExtractedComment.IsValid() && ExtractedComment->HasField(TEXT("bCommentBubbleMakeVisible")));
	TestFalse(TEXT("transient NodeUpgradeMessage is omitted"), ExtractedComment.IsValid() && ExtractedComment->HasField(TEXT("NodeUpgradeMessage")));

	TSharedPtr<FJsonObject> SparseTree = MakeCommentTree();
	TSharedPtr<FJsonObject> SparseComment = MakeShared<FJsonObject>();
	SparseComment->SetStringField(TEXT("Id"), CommentGuid);
	SparseComment->SetStringField(TEXT("Text"), TEXT("Sparse updated text"));
	SparseTree->SetArrayField(TEXT("Comments"), {BTTestMakeObjectValue(SparseComment)});
	TArray<TSharedPtr<FJsonValue>> DiffEntries;
	FAssetDocumentRegionContext Context = BTGraphSourceTests::MakeTreeContext(Reloaded);
	FAssetDocumentCapabilityResult Result = BTGraphSourceTests::DiffTree(Context, SparseTree.ToSharedRef(), DiffEntries);
	TestTrue(TEXT("sparse comment diff succeeds"), Result.bSuccess);
	const FString ExpectedTextPath = BTTestTreePointer(SparseTree, CommentGuid) + TEXT("/Text");
	TestTrue(TEXT("comment diff is identity-addressed"), DiffEntries.ContainsByPredicate([&ExpectedTextPath](const TSharedPtr<FJsonValue>& Value)
	{
		TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
		FString Path;
		return Entry.IsValid() && Entry->TryGetStringField(TEXT("path"), Path) && Path == ExpectedTextPath;
	}));
	bool bChanged = false;
	Result = BTGraphSourceTests::ApplyTree(Reloaded, SparseTree, bChanged);
	TestTrue(TEXT("sparse comment update applies"), Result.bSuccess);
	Comment = FindComment(Cast<UBehaviorTreeGraph>(Reloaded->BTGraph));
	TestEqual(TEXT("sparse update changes text"), Comment ? Comment->NodeComment : FString(), FString(TEXT("Sparse updated text")));
	TestEqual(TEXT("sparse update preserves width"), Comment ? Comment->NodeWidth : 0, 640);
	TestEqual(TEXT("sparse update preserves alpha"), Comment ? Comment->CommentColor.A : 0.0f, 0.4f);
	TestEqual(TEXT("sparse update preserves NodeDetails"), Comment ? Comment->NodeDetails.ToString() : FString(), FString(TEXT("Stable localized details")));
	TestTrue(TEXT("sparse update preserves both comment-specific bubble fields"), Comment && Comment->bCommentBubbleVisible_InDetailsPanel && Comment->bColorCommentBubble);

	TSharedPtr<FJsonObject> DeleteTree = MakeCommentTree();
	DeleteTree->SetArrayField(TEXT("Comments"), {});
	Result = BTGraphSourceTests::ApplyTree(Reloaded, DeleteTree, bChanged);
	TestTrue(TEXT("explicit empty Comments deletes comment boxes"), Result.bSuccess);
	TestNull(TEXT("comment box is explicitly deleted"), FindComment(Cast<UBehaviorTreeGraph>(Reloaded->BTGraph)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeRollbackTest,
	"UEAssetDoc.BehaviorTree.GraphSource.Rollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeRollbackTest::RunTest(const FString&)
{
	BTGraphSourceTests::FPublicGraphFixture PublicGraphFixture;
	using namespace BTGraphShapeTests;
	TSharedPtr<FJsonObject> OriginalTree = MakeCommentTree();
	FirstChild(OriginalTree)->SetArrayField(TEXT("Decorators"), {
		BTTestMakeObjectValue(MakeDecorator(EdgeDecoratorGuidA, TEXT("Edge decorator A"), 80.0, 260.0, TEXT("edge decorator comment A"))),
		BTTestMakeObjectValue(MakeCompositeDecorator(MakeBoundGraph(TEXT("NestedNotOr"))))});
	UBehaviorTree* BehaviorTree = BTGraphSourceTests::CreateTree();
	bool bChanged = false;
	FAssetDocumentCapabilityResult Result = BTGraphSourceTests::ApplyTree(BehaviorTree, OriginalTree, bChanged);
	TestTrue(TEXT("rollback baseline applies"), Result.bSuccess);
	if (!Result.bSuccess)
	{
		AddError(Result.Message);
		return false;
	}
	UBehaviorTreeGraph* OriginalGraph = Cast<UBehaviorTreeGraph>(BehaviorTree->BTGraph);
	UBehaviorTreeGraphNode* OriginalRoot = BTGraphSourceTests::FindGraphNodeByGuid(OriginalGraph, BTGraphSourceTests::RootGuid);
	UBehaviorTreeGraphNode_CompositeDecorator* OriginalComposite = Cast<UBehaviorTreeGraphNode_CompositeDecorator>(FindSubNodeByGuid(OriginalGraph, CompositeDecoratorGuid));
	UEdGraph* OriginalBoundGraph = OriginalComposite ? OriginalComposite->BoundGraph : nullptr;
	UEdGraphNode_Comment* OriginalComment = FindComment(OriginalGraph);
	UBTCompositeNode* OriginalRuntimeRoot = BehaviorTree->RootNode;
	const int32 OriginalRootDecoratorCount = BehaviorTree->RootDecorators.Num();
	const int32 OriginalRootOpCount = BehaviorTree->RootDecoratorOps.Num();
	const int32 OriginalEdgeDecoratorCount = BehaviorTree->RootNode && BehaviorTree->RootNode->Children.Num() > 0 ? BehaviorTree->RootNode->Children[0].Decorators.Num() : -1;
	const int32 OriginalEdgeOpCount = BehaviorTree->RootNode && BehaviorTree->RootNode->Children.Num() > 0 ? BehaviorTree->RootNode->Children[0].DecoratorOps.Num() : -1;
	const FString OriginalCanonical = ComparableTree(BehaviorTree);

	TSharedPtr<FJsonObject> InvalidTree = MakeCommentTree();
	TSharedPtr<FJsonObject> InvalidBound = MakeBoundGraph(TEXT("NestedNotOr"));
	InvalidBound->SetArrayField(TEXT("Links"), {MakeLink(TEXT("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), SinkGuid, 0)});
	FirstChild(InvalidTree)->SetArrayField(TEXT("Decorators"), {
		BTTestMakeObjectValue(MakeDecorator(EdgeDecoratorGuidA, TEXT("Edge decorator A"), 80.0, 260.0, TEXT("edge decorator comment A"))),
		BTTestMakeObjectValue(MakeCompositeDecorator(InvalidBound))});
	bChanged = false;
	Result = BTGraphSourceTests::ApplyTree(BehaviorTree, InvalidTree, bChanged);
	TestFalse(TEXT("late-surface invalid BoundGraph apply fails"), Result.bSuccess);
	TestTrue(TEXT("invalid BoundGraph points at dangling From"), BTTestHasDiagnostic(Result, TEXT("/Body/Tree/Root/Children/0/Decorators/1/BoundGraph/Links/0/From"), TEXT("DanglingBehaviorTreeDecoratorLink")));
	TestFalse(TEXT("failed apply reports unchanged"), bChanged);
	TestTrue(TEXT("failed apply keeps original main graph"), BehaviorTree->BTGraph == OriginalGraph);
	TestTrue(TEXT("failed apply keeps original root wrapper"), BTGraphSourceTests::FindGraphNodeByGuid(OriginalGraph, BTGraphSourceTests::RootGuid) == OriginalRoot);
	TestTrue(TEXT("failed apply keeps composite wrapper object"), FindSubNodeByGuid(OriginalGraph, CompositeDecoratorGuid) == OriginalComposite);
	TestTrue(TEXT("failed apply keeps BoundGraph object and links"), OriginalComposite && OriginalComposite->BoundGraph == OriginalBoundGraph);
	TestTrue(TEXT("failed apply keeps comment object"), FindComment(OriginalGraph) == OriginalComment);
	TestTrue(TEXT("failed apply keeps runtime root object"), BehaviorTree->RootNode == OriginalRuntimeRoot);
	TestEqual(TEXT("failed apply keeps root decorator mirror"), BehaviorTree->RootDecorators.Num(), OriginalRootDecoratorCount);
	TestEqual(TEXT("failed apply keeps root decorator ops"), BehaviorTree->RootDecoratorOps.Num(), OriginalRootOpCount);
	TestEqual(TEXT("failed apply keeps child decorator mirror"), BehaviorTree->RootNode && BehaviorTree->RootNode->Children.Num() > 0 ? BehaviorTree->RootNode->Children[0].Decorators.Num() : -1, OriginalEdgeDecoratorCount);
	TestEqual(TEXT("failed apply keeps child decorator ops"), BehaviorTree->RootNode && BehaviorTree->RootNode->Children.Num() > 0 ? BehaviorTree->RootNode->Children[0].DecoratorOps.Num() : -1, OriginalEdgeOpCount);
	TestEqual(TEXT("failed apply keeps canonical extract unchanged"), ComparableTree(BehaviorTree), OriginalCanonical);

	return true;
}

#endif
