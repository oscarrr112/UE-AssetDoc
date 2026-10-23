// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardLoadedConsumerCompletion.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/ValueOrBBKey.h"
#include "BehaviorTreeGraph.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

using namespace AssetDocumentAIBlackboardLoadedConsumerCompletion;
using namespace AssetDocumentAIBlackboardLoadedConsumerCompletion::Private;
using namespace AssetDocumentAIBlackboardValueOrKeyCacheInvalidation;

namespace AIBlackboardLoadedConsumerCompletionLocal
{
#if WITH_DEV_AUTOMATION_TESTS
ELoadedConsumerCompletionTestFailurePoint GFailurePoint =
	ELoadedConsumerCompletionTestFailurePoint::None;

bool ShouldFail(const ELoadedConsumerCompletionTestFailurePoint Point)
{
	if (GFailurePoint != Point) return false;
	GFailurePoint = ELoadedConsumerCompletionTestFailurePoint::None;
	return true;
}
#else
bool ShouldFail(const ELoadedConsumerCompletionTestFailurePoint)
{
	return false;
}
#endif
ELoadedConsumerCompletionTestFailurePoint BeforeFamily(
	const EExactValueOrBlackboardKeyFamily Family)
{
	return static_cast<ELoadedConsumerCompletionTestFailurePoint>(
		static_cast<uint8>(ELoadedConsumerCompletionTestFailurePoint::BeforeBool)
		+ static_cast<uint8>(Family) * 2);
}

ELoadedConsumerCompletionTestFailurePoint AfterFamily(
	const EExactValueOrBlackboardKeyFamily Family)
{
	return static_cast<ELoadedConsumerCompletionTestFailurePoint>(
		static_cast<uint8>(ELoadedConsumerCompletionTestFailurePoint::AfterBool)
		+ static_cast<uint8>(Family) * 2);
}

class FScopedPackageDirtyState final
{
public:
	FScopedPackageDirtyState(UPackage& InPackage, const bool bInDirty)
		: Package(InPackage), bDirty(bInDirty)
	{
	}

	~FScopedPackageDirtyState()
	{
		Restore();
	}

	void Restore()
	{
		if (bRestored) return;
		Package.SetDirtyFlag(bDirty);
		bRestored = true;
	}

private:
	UPackage& Package;
	bool bDirty = false;
	bool bRestored = false;
};

FResolvedExactValueOrLeaf MakeResolvedValueOr(FCurrentLeaf& Leaf)
{
	switch (Leaf.Family)
	{
	case EExactValueOrBlackboardKeyFamily::Bool: return {*static_cast<FValueOrBBKey_Bool*>(Leaf.ValueOr), *Leaf.DeclaredStruct};
	case EExactValueOrBlackboardKeyFamily::Class: return {*static_cast<FValueOrBBKey_Class*>(Leaf.ValueOr), *Leaf.DeclaredStruct};
	case EExactValueOrBlackboardKeyFamily::Enum: return {*static_cast<FValueOrBBKey_Enum*>(Leaf.ValueOr), *Leaf.DeclaredStruct};
	case EExactValueOrBlackboardKeyFamily::Float: return {*static_cast<FValueOrBBKey_Float*>(Leaf.ValueOr), *Leaf.DeclaredStruct};
	case EExactValueOrBlackboardKeyFamily::Int32: return {*static_cast<FValueOrBBKey_Int32*>(Leaf.ValueOr), *Leaf.DeclaredStruct};
	case EExactValueOrBlackboardKeyFamily::Name: return {*static_cast<FValueOrBBKey_Name*>(Leaf.ValueOr), *Leaf.DeclaredStruct};
	case EExactValueOrBlackboardKeyFamily::String: return {*static_cast<FValueOrBBKey_String*>(Leaf.ValueOr), *Leaf.DeclaredStruct};
	case EExactValueOrBlackboardKeyFamily::Object: return {*static_cast<FValueOrBBKey_Object*>(Leaf.ValueOr), *Leaf.DeclaredStruct};
	case EExactValueOrBlackboardKeyFamily::Rotator: return {*static_cast<FValueOrBBKey_Rotator*>(Leaf.ValueOr), *Leaf.DeclaredStruct};
	case EExactValueOrBlackboardKeyFamily::Struct: return {*static_cast<FValueOrBBKey_Struct*>(Leaf.ValueOr), *Leaf.DeclaredStruct};
	case EExactValueOrBlackboardKeyFamily::Vector: return {*static_cast<FValueOrBBKey_Vector*>(Leaf.ValueOr), *Leaf.DeclaredStruct};
	}
	checkNoEntry();
	return {*static_cast<FValueOrBBKey_Bool*>(Leaf.ValueOr), *Leaf.DeclaredStruct};
}

bool LeavesMatch(TConstArrayView<FStoredLeaf> Stored, TConstArrayView<FCurrentLeaf> Current)
{
	if (Stored.Num() != Current.Num()) return false;
	for (int32 Index = 0; Index < Stored.Num(); ++Index)
	{
		if (!SameLeafLocator(Stored[Index].Locator, Current[Index].Locator)
			|| Stored[Index].AuthoredFingerprint != Current[Index].AuthoredFingerprint
			|| Stored[Index].AuthoredKeyName != Current[Index].AuthoredKeyName) return false;
	}
	return true;
}

bool VerifyValueOrMapping(const FCurrentLeaf& Leaf, const UBlackboardData& Blackboard)
{
	if (Leaf.ValueOr == nullptr || Leaf.AuthoredKeyName.IsNone()) return Leaf.ValueOr != nullptr;
	const FBlackboard::FKey KeyId = Blackboard.GetKeyID(Leaf.AuthoredKeyName);
	if (KeyId == FBlackboard::InvalidKey)
	{
		return true;
	}
	const FBlackboardEntry* Entry = Blackboard.GetKey(KeyId);
	if (Entry == nullptr || Entry->EntryName != Leaf.AuthoredKeyName
		|| Entry->KeyType == nullptr)
	{
		return false;
	}
#if WITH_EDITOR
	return static_cast<const FValueOrBlackboardKeyBase*>(Leaf.ValueOr)
		->IsCompatibleType(Entry->KeyType.Get());
#else
	return false;
#endif
}

bool CompleteOneConsumer(FConsumer& Consumer, UBlackboardData& Target,
	const ELoadedConsumerCompletionPhase Phase)
{
	UBehaviorTree* Tree = Consumer.Tree.Get();
	if (Tree == nullptr || FSoftObjectPath(Tree).ToString() != Consumer.AssetPath
		|| Tree->GetClass() != UBehaviorTree::StaticClass()
		|| !EffectiveBlackboardContains(*Tree, Target)
		|| Tree->GetOutermost()->IsDirty() != Consumer.bPackageDirty)
	{
		return false;
	}
	FScopedPackageDirtyState DirtyState(*Tree->GetOutermost(), Consumer.bPackageDirty);
	TArray<FCurrentLeaf> Before;
	TArray<FGuid> BeforeGraphs;
	TArray<FGuid> BeforeNodes;
	if (!InventoryTree(*Tree, Before, BeforeGraphs, BeforeNodes)
		|| BeforeGraphs != Consumer.GraphGuids || BeforeNodes != Consumer.NodeGuids
		|| !LeavesMatch(Consumer.Leaves, Before)
		|| ShouldFail(ELoadedConsumerCompletionTestFailurePoint::BeforeGraph))
	{
		return false;
	}
	UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(Tree->BTGraph);
	if (Graph == nullptr)
	{
		return false;
	}
	Graph->UpdateBlackboardChange();
	if (ShouldFail(ELoadedConsumerCompletionTestFailurePoint::AfterGraph))
	{
		return false;
	}
	TArray<FCurrentLeaf> Current;
	TArray<FGuid> CurrentGraphs;
	TArray<FGuid> CurrentNodes;
	if (!InventoryTree(*Tree, Current, CurrentGraphs, CurrentNodes)
		|| CurrentGraphs != Consumer.GraphGuids || CurrentNodes != Consumer.NodeGuids
		|| !LeavesMatch(Consumer.Leaves, Current))
	{
		return false;
	}
	UBlackboardData* Effective = Tree->BlackboardAsset;
	if (Effective == nullptr)
	{
		return false;
	}
	for (FCurrentLeaf& Leaf : Current)
	{
		if (Leaf.Selector != nullptr)
		{
			if (ShouldFail(ELoadedConsumerCompletionTestFailurePoint::BeforeSelector)) return false;
			Leaf.Selector->InvalidateResolvedKey();
			Leaf.Selector->ResolveSelectedKey(*Effective);
			if (ShouldFail(ELoadedConsumerCompletionTestFailurePoint::AfterSelector)) return false;
			const FBlackboard::FKey ExpectedId = Effective->GetKeyID(Leaf.Selector->SelectedKeyName);
			const UClass* ExpectedType = ExpectedId == FBlackboard::InvalidKey
				? nullptr : Effective->GetKeyType(ExpectedId).Get();
			if (Leaf.Selector->GetSelectedKeyID() != ExpectedId
				|| Leaf.Selector->SelectedKeyType.Get() != ExpectedType)
			{
				return false;
			}
		}
		else
		{
			if (ShouldFail(BeforeFamily(Leaf.Family))) return false;
			FResolvedExactValueOrLeaf Resolved = MakeResolvedValueOr(Leaf);
			const EExactValueOrKeyCacheInvalidationResult Invalidation =
				InvalidateExactValueOrBlackboardKeyCache(Resolved);
			if (Invalidation != EExactValueOrKeyCacheInvalidationResult::Success)
			{
				return false;
			}
			if (!VerifyValueOrMapping(Leaf, *Effective))
			{
				return false;
			}
			if (ShouldFail(AfterFamily(Leaf.Family))) return false;
		}
	}
	if (ShouldFail(ELoadedConsumerCompletionTestFailurePoint::BeforeVerification)) return false;
	DirtyState.Restore();
	TArray<FCurrentLeaf> Verified;
	TArray<FGuid> VerifiedGraphs;
	TArray<FGuid> VerifiedNodes;
	if (!InventoryTree(*Tree, Verified, VerifiedGraphs, VerifiedNodes)
		|| VerifiedGraphs != Consumer.GraphGuids || VerifiedNodes != Consumer.NodeGuids
		|| !LeavesMatch(Consumer.Leaves, Verified)
		|| Tree->GetOutermost()->IsDirty() != Consumer.bPackageDirty
		|| ShouldFail(ELoadedConsumerCompletionTestFailurePoint::AfterVerification)
		|| (Phase == ELoadedConsumerCompletionPhase::RollbackRestored
			&& ShouldFail(ELoadedConsumerCompletionTestFailurePoint::RollbackVerification)))
	{
		return false;
	}
	return true;
}
} // namespace

namespace AssetDocumentAIBlackboardLoadedConsumerCompletion::Private
{
bool QueryExactBehaviorTrees(IAssetRegistry& Registry, TArray<FAssetData>& OutAssets)
{
	OutAssets.Reset();
	return Registry.GetAssetsByClass(UBehaviorTree::StaticClass()->GetClassPathName(), OutAssets, false);
}

bool RegistryIdentityDigest(TConstArrayView<FAssetData> Assets, FString& OutDigest)
{
	TArray<FString> Identities;
	for (const FAssetData& Asset : Assets)
	{
		if (!Asset.IsValid() || Asset.AssetClassPath != UBehaviorTree::StaticClass()->GetClassPathName()) return false;
		Identities.Add(Asset.GetObjectPathString() + TEXT("|")
			+ Asset.PackageName.ToString() + TEXT("|") + Asset.AssetClassPath.ToString());
	}
	Identities.Sort();
	for (int32 Index = 1; Index < Identities.Num(); ++Index)
	{
		if (Identities[Index] == Identities[Index - 1]) return false;
	}
	return HashUtf8(FString::Join(Identities, TEXT("\n")), OutDigest);
}

bool QueryPackageAssets(IAssetRegistry& Registry, const FName PackageName,
	TArray<FAssetData>& OutAssets)
{
	OutAssets.Reset();
	return !PackageName.IsNone()
		&& Registry.GetAssetsByPackageName(PackageName, OutAssets, false, true);
}

bool QueryPackageReferencers(IAssetRegistry& Registry, const FName PackageName,
	TArray<FName>& OutReferencers)
{
	OutReferencers.Reset();
	return !PackageName.IsNone() && Registry.GetReferencers(PackageName, OutReferencers);
}

bool AddCandidate(const FAssetData& Asset, TMap<FString, FAssetData>& Candidates)
{
	if (!Asset.IsValid()
		|| Asset.AssetClassPath != UBehaviorTree::StaticClass()->GetClassPathName())
	{
		return false;
	}
	const FString Identity = Asset.GetObjectPathString();
	if (Identity.IsEmpty()) return false;
	if (const FAssetData* Existing = Candidates.Find(Identity))
	{
		return Existing->PackageName == Asset.PackageName
			&& Existing->GetSoftObjectPath() == Asset.GetSoftObjectPath();
	}
	Candidates.Add(Identity, Asset);
	return true;
}

bool BuildCandidateUnion(IAssetRegistry& Registry, UBlackboardData& Target,
	TConstArrayView<FAssetData> BehaviorTreeAudit, TArray<FAssetData>& OutCandidates)
{
	const FSoftObjectPath TargetPath(&Target);
	const FName TargetPackage = Target.GetOutermost() != nullptr
		? Target.GetOutermost()->GetFName() : NAME_None;
	if (TargetPackage.IsNone())
	{
		return false;
	}

	const bool bTargetRegistered = Registry.GetAssetPackageDataCopy(TargetPackage).IsSet();
	const bool bTransientTarget = Target.GetOutermost() == GetTransientPackage()
		|| Target.HasAnyFlags(RF_Transient);
	if (bTargetRegistered && !bTransientTarget)
	{
		TArray<FAssetData> TargetPackageAssets;
		if (!QueryPackageAssets(Registry, TargetPackage, TargetPackageAssets)) return false;
		bool bResolvedTarget = false;
		for (const FAssetData& Asset : TargetPackageAssets)
		{
			if (Asset.AssetClassPath != UBlackboardData::StaticClass()->GetClassPathName()
				|| Asset.GetSoftObjectPath() != TargetPath) continue;
			if (bResolvedTarget || !Asset.IsAssetLoaded()) return false;
			UObject* Resolved = nullptr;
			if (!ResolveLoadedExactAsset(Asset, *UBlackboardData::StaticClass(), Resolved)
				|| Resolved != &Target) return false;
			bResolvedTarget = true;
		}
		if (!bResolvedTarget) return false;
	}
	if (bTargetRegistered && (bTransientTarget || !TargetPath.IsValid())) return false;

	TSet<FName> AffectedBlackboardPackages;
	TArray<FName> PendingBlackboards;
	if (bTargetRegistered)
	{
		AffectedBlackboardPackages.Add(TargetPackage);
		PendingBlackboards.Add(TargetPackage);
	}
	for (int32 QueueIndex = 0; QueueIndex < PendingBlackboards.Num(); ++QueueIndex)
	{
		TArray<FName> Referencers;
		if (!QueryPackageReferencers(Registry, PendingBlackboards[QueueIndex], Referencers)) return false;
		Referencers.Sort(FNameLexicalLess());
		for (const FName ReferencerPackage : Referencers)
		{
			if (AffectedBlackboardPackages.Contains(ReferencerPackage)) continue;
			TArray<FAssetData> PackageAssets;
			if (!QueryPackageAssets(Registry, ReferencerPackage, PackageAssets)) return false;
			for (const FAssetData& Asset : PackageAssets)
			{
				if (Asset.AssetClassPath != UBlackboardData::StaticClass()->GetClassPathName()
					|| !Asset.IsAssetLoaded()) continue;
				UObject* Resolved = nullptr;
				if (!ResolveLoadedExactAsset(Asset, *UBlackboardData::StaticClass(), Resolved)) return false;
				UBlackboardData* Blackboard = CastChecked<UBlackboardData>(Resolved);
				if (Blackboard != &Target && Blackboard->IsChildOf(Target))
				{
					AffectedBlackboardPackages.Add(ReferencerPackage);
					PendingBlackboards.Add(ReferencerPackage);
				}
			}
		}
	}

	TMap<FString, FAssetData> Candidates;
	TArray<FName> SortedAffected = AffectedBlackboardPackages.Array();
	SortedAffected.Sort(FNameLexicalLess());
	for (const FName BlackboardPackage : SortedAffected)
	{
		TArray<FName> Referencers;
		if (!QueryPackageReferencers(Registry, BlackboardPackage, Referencers)) return false;
		Referencers.Sort(FNameLexicalLess());
		for (const FName ReferencerPackage : Referencers)
		{
			TArray<FAssetData> PackageAssets;
			if (!QueryPackageAssets(Registry, ReferencerPackage, PackageAssets)) return false;
			for (const FAssetData& Asset : PackageAssets)
			{
				if (Asset.AssetClassPath == UBehaviorTree::StaticClass()->GetClassPathName()
					&& !AddCandidate(Asset, Candidates)) return false;
			}
		}
	}

	for (const FAssetData& Asset : BehaviorTreeAudit)
	{
		if (Asset.IsAssetLoaded() && !AddCandidate(Asset, Candidates)) return false;
	}
	Candidates.GenerateValueArray(OutCandidates);
	OutCandidates.Sort([](const FAssetData& Left, const FAssetData& Right)
	{
		return Left.GetObjectPathString() < Right.GetObjectPathString();
	});
	return true;
}
} // namespace AssetDocumentAIBlackboardLoadedConsumerCompletion::Private

struct FLoadedBehaviorTreeConsumerReceipt::FImpl
{
	FString TargetPath;
	FString RegistryStateDigest;
	TArray<FString> SkippedUnloadedConsumerPaths;
	TArray<FConsumer> Consumers;
	bool bForwardAttempted = false;
	bool bRollbackAttempted = false;
};

FLoadedBehaviorTreeConsumerReceipt::FLoadedBehaviorTreeConsumerReceipt(TUniquePtr<FImpl> InImpl)
	: Impl(MoveTemp(InImpl))
{
}

FLoadedBehaviorTreeConsumerReceipt::FLoadedBehaviorTreeConsumerReceipt(
	FLoadedBehaviorTreeConsumerReceipt&&) = default;
FLoadedBehaviorTreeConsumerReceipt& FLoadedBehaviorTreeConsumerReceipt::operator=(
	FLoadedBehaviorTreeConsumerReceipt&&) = default;
FLoadedBehaviorTreeConsumerReceipt::~FLoadedBehaviorTreeConsumerReceipt() = default;

TValueOrError<FLoadedBehaviorTreeConsumerReceipt, ELoadedConsumerReceiptCaptureFailure>
AssetDocumentAIBlackboardLoadedConsumerCompletion::CaptureLoadedBehaviorTreeConsumerReceipt(
	UBlackboardData& ExactTarget)
{
	using namespace AIBlackboardLoadedConsumerCompletionLocal;
	IAssetRegistry* Registry = IAssetRegistry::Get();
	if (Registry == nullptr || ShouldFail(ELoadedConsumerCompletionTestFailurePoint::RegistryBeforeReadiness)
		|| !Registry->IsSearchAllAssets() || Registry->IsGathering())
	{
		return MakeError(ELoadedConsumerReceiptCaptureFailure::AuthorityUnavailable);
	}
	TArray<FAssetData> SnapshotA;
	TArray<FAssetData> SnapshotB;
	FString DigestA;
	FString DigestB;
	if (ShouldFail(ELoadedConsumerCompletionTestFailurePoint::RegistrySnapshotA)
		|| !QueryExactBehaviorTrees(*Registry, SnapshotA)
		|| !RegistryIdentityDigest(SnapshotA, DigestA))
	{
		return MakeError(ELoadedConsumerReceiptCaptureFailure::AuthorityUnavailable);
	}
	if (ShouldFail(ELoadedConsumerCompletionTestFailurePoint::RegistryReferencers))
	{
		return MakeError(ELoadedConsumerReceiptCaptureFailure::AuthorityUnavailable);
	}
	TUniquePtr<FLoadedBehaviorTreeConsumerReceipt::FImpl> Impl =
		MakeUnique<FLoadedBehaviorTreeConsumerReceipt::FImpl>();
	Impl->TargetPath = FSoftObjectPath(&ExactTarget).ToString();
	TArray<FAssetData> Candidates;
	if (Impl->TargetPath.IsEmpty())
	{
		return MakeError(ELoadedConsumerReceiptCaptureFailure::AuthorityUnavailable);
	}
	if (!BuildCandidateUnion(*Registry, ExactTarget, SnapshotA, Candidates))
	{
		return MakeError(ELoadedConsumerReceiptCaptureFailure::AuthorityUnavailable);
	}
	if (!CaptureConsumers(ExactTarget, Candidates, Impl->Consumers,
		Impl->SkippedUnloadedConsumerPaths))
	{
		return MakeError(ELoadedConsumerReceiptCaptureFailure::AuthorityUnavailable);
	}
	if (ShouldFail(ELoadedConsumerCompletionTestFailurePoint::RegistrySnapshotB)
		|| !QueryExactBehaviorTrees(*Registry, SnapshotB)
		|| !RegistryIdentityDigest(SnapshotB, DigestB))
	{
		return MakeError(ELoadedConsumerReceiptCaptureFailure::AuthorityUnavailable);
	}
	if (ShouldFail(ELoadedConsumerCompletionTestFailurePoint::RegistryAfterReadiness)
		|| !Registry->IsSearchAllAssets() || Registry->IsGathering() || DigestA != DigestB)
	{
		return MakeError(ELoadedConsumerReceiptCaptureFailure::AuthorityUnavailable);
	}
	Impl->RegistryStateDigest = MoveTemp(DigestA);
	return MakeValue(FLoadedBehaviorTreeConsumerReceipt(MoveTemp(Impl)));
}

ELoadedConsumerCompletionResult
AssetDocumentAIBlackboardLoadedConsumerCompletion::CompleteLoadedBehaviorTreeConsumerReceipt(
	FLoadedBehaviorTreeConsumerReceipt& Receipt,
	UBlackboardData& ExactTarget,
	const ELoadedConsumerCompletionPhase Phase)
{
	using namespace AIBlackboardLoadedConsumerCompletionLocal;
	if (!Receipt.Impl.IsValid() || Receipt.Impl->TargetPath != FSoftObjectPath(&ExactTarget).ToString())
	{
		return ELoadedConsumerCompletionResult::DerivedRefreshFailed;
	}
	bool& bAttempted = Phase == ELoadedConsumerCompletionPhase::ForwardPublished
		? Receipt.Impl->bForwardAttempted : Receipt.Impl->bRollbackAttempted;
	if (bAttempted)
	{
		return ELoadedConsumerCompletionResult::DerivedRefreshFailed;
	}
	bAttempted = true;
	for (FConsumer& Consumer : Receipt.Impl->Consumers)
	{
		if (!CompleteOneConsumer(Consumer, ExactTarget, Phase))
		{
			return ELoadedConsumerCompletionResult::DerivedRefreshFailed;
		}
	}
	return ELoadedConsumerCompletionResult::Succeeded;
}

#if WITH_DEV_AUTOMATION_TESTS
void AssetDocumentAIBlackboardLoadedConsumerCompletion::SetLoadedConsumerCompletionTestFailurePoint(
	const ELoadedConsumerCompletionTestFailurePoint FailurePoint)
{
	using namespace AIBlackboardLoadedConsumerCompletionLocal;
	GFailurePoint = FailurePoint;
}

bool AssetDocumentAIBlackboardLoadedConsumerCompletion::ConsumeLoadedConsumerCompletionTestFailurePoint(
	const ELoadedConsumerCompletionTestFailurePoint FailurePoint)
{
	using namespace AIBlackboardLoadedConsumerCompletionLocal;
	return ShouldFail(FailurePoint);
}
#endif
