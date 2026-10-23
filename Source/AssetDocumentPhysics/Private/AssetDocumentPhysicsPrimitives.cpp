// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Physics asset primitives (provider "physics", owner Physics) of the Physics Asset profile
// (docs/architecture/asset-document-physics-asset-design.md §4):
//   physics.asset.read.v1  the authored state of a Physics Asset as the Physics Asset Editor shows it (AssetDocumentPhysicsRead.cpp)
//   physics.status.v1      data validation and structural checks (AssetDocumentPhysicsStatus.cpp)
//   physics.asset.edit.v1  edits as the Physics Asset Editor makes them (AssetDocumentPhysicsEdit.cpp)
// The read-only ones never dirty a package or change the asset.

#include "AssetDocumentPhysicsPrimitives.h"

#include "AssetDocumentGraphProvider.h"
#include "AssetDocumentPhysicsProvider.h"

namespace UE::AssetDocument::Graph::Physics
{
namespace
{
constexpr uint32 PhysicsProviderVersion = 1;

FAssetDocumentPrimitiveDescriptor MakeDescriptor(const TCHAR* PrimitiveId, bool bMutating, const TCHAR* AuthoritativeApi)
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = PrimitiveId;
	Descriptor.Version = PhysicsProviderVersion;
	Descriptor.ProviderId = ProviderId;
	Descriptor.Owner = EAssetDocumentPrimitiveOwner::Physics;
	Descriptor.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
	Descriptor.Access = bMutating ? EAssetDocumentPrimitiveAccess::Mutating : EAssetDocumentPrimitiveAccess::ReadOnly;
	if (bMutating)
	{
		Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair};
		Descriptor.MutationFootprintSchemaId = TEXT("graph.footprint.whole-package.v1");
	}
	else
	{
		Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Validate, EAssetDocumentRuntimeRequestPhase::Preflight,
			EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair,
			EAssetDocumentRuntimeRequestPhase::Verify};
	}
	Descriptor.bRequiresActiveTransaction = bMutating;
	Descriptor.InputSchemaId = Descriptor.PrimitiveId + TEXT(".input.v1");
	Descriptor.OutputSchemaId = Descriptor.PrimitiveId + TEXT(".output.v1");
	Descriptor.StableDiagnosticCodes = {
		TEXT("AD.RUNTIME.PRIMITIVE_FAILED"), TEXT("Graph.UnknownField"), TEXT("Graph.InvalidArgument"),
		TEXT("Graph.UnsupportedSchemaVersion"), TEXT("Graph.PieActive"), TEXT("Graph.ResultTooLarge"),
		TEXT("PhysicsAsset.UnsupportedAsset"), TEXT("PhysicsAsset.ReadFailed"), TEXT("PhysicsAsset.InteractiveEditRefused"),
		TEXT("PhysicsAsset.EditFailed"), TEXT("PhysicsAsset.InvalidName"),
		TEXT("PhysicsAsset.BoneNotFound"), TEXT("PhysicsAsset.BodyNotFound"), TEXT("PhysicsAsset.BodyExists"),
		TEXT("PhysicsAsset.ConstraintNotFound"), TEXT("PhysicsAsset.NameTaken"), TEXT("PhysicsAsset.UnknownProfile"),
		TEXT("PhysicsAsset.FieldNotEditable"), TEXT("PhysicsAsset.InvalidValue"), TEXT("PhysicsAsset.ShapeTypeNotAuthored"),
		TEXT("PhysicsAsset.IncompatibleMesh"), TEXT("PhysicsAsset.UnknownMesh"), TEXT("PhysicsAsset.PreviewMeshRequired"),
		TEXT("PhysicsAsset.FramesRequired"), TEXT("PhysicsAsset.FitFailed"), TEXT("PhysicsAsset.GenerationFailed"),
		TEXT("PhysicsAsset.GenerationNeedsEmptyAsset")};
	Descriptor.RequiredConsumerIds = {TEXT("route.generic.v1")};
	Descriptor.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
	Descriptor.HandleArgumentSchemas.Add({TEXT("asset"), EAssetDocumentHandleKind::Asset,
		bMutating ? EAssetDocumentHandleAccess::Mutable : EAssetDocumentHandleAccess::ReadOnly, true});

	FAssetDocumentPrimitiveAdmissionEvidence& Evidence = Descriptor.Admission;
	Evidence.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
	Evidence.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper;
	Evidence.ExactEngineVersion = TEXT("5.7.4");
	Evidence.PhysicalOwner = EAssetDocumentPrimitiveOwner::Physics;
	Evidence.PublicModule = TEXT("AssetDocumentPhysics");
	Evidence.PublicHeader = TEXT("AssetDocumentPhysicsProvider.h");
	Evidence.ExportedAuthoritativeApi = AuthoritativeApi;
	Evidence.AuthorityContextId = TEXT("assetdocument.editor.game-thread.editor-ready.v1");
	Evidence.PrimaryOperationId = Descriptor.PrimitiveId;
	Evidence.IdentityLifetimeContractId = TEXT("identity.request-transaction-revision-bound.v1");
	Evidence.TransactionContractId = TEXT("transaction.descriptor-governed.v1");
	Evidence.RollbackContractId = TEXT("rollback.core-package-snapshot.v1");
	Evidence.CompletionContractId = Descriptor.PrimitiveId + TEXT(".completion.v1");
	Evidence.SaveReloadEvidenceId = TEXT("UEAssetDoc.PhysicsAsset.SaveReloadProfile");
	Evidence.FailureInjectionTestIds = {TEXT("UEAssetDoc.PhysicsAsset.Rollback")};
	Evidence.VerificationTestIds = {TEXT("UEAssetDoc.PhysicsAsset.Fidelity")};
	Evidence.AuthorityFingerprint = FString::Printf(TEXT("ue-5.7.4|physics|%s|%s"), PrimitiveId, AuthoritativeApi);
	return Descriptor;
}

TArray<FAssetDocumentPrimitiveDescriptor> BuildDescriptors()
{
	return {
		MakeDescriptor(TEXT("physics.asset.read.v1"), false,
			TEXT("UPhysicsAsset::GetPreviewMesh;UPhysicsAsset::IsCollisionEnabled;UPhysicsAsset::FindBodyIndex;"
				"USkeletalBodySetup::GetPhysicalAnimationProfiles;UPhysicsConstraintTemplate::ProfileHandles;"
				"FConstraintInstance::SnapTransformsToDefault;UBodySetup::CalculateMass;FPhysicsAssetUtils::CreateFromSkeletalMesh")),
		MakeDescriptor(TEXT("physics.status.v1"), false,
			TEXT("UPhysicsAsset::IsDataValid;UBodySetup::IsDataValid;UPhysicsAsset::FindControllingBodyIndex;"
				"FKConvexElem::GetChaosConvexMesh")),
		MakeDescriptor(TEXT("physics.asset.edit.v1"), true,
			TEXT("UPhysicsAsset::SetPreviewMesh;UPhysicsAsset::PostEditChangeProperty;FPhysicsAssetUtils::CreateFromSkeletalMesh;"
				"FPhysicsAssetUtils::CreateNewBody;FPhysicsAssetUtils::CreateCollisionFromBone;FMeshUtilitiesEngine::CalcBoneVertInfos;"
				"FPhysicsAssetUtils::DestroyBody;FPhysicsAssetUtils::CreateNewConstraint;FPhysicsAssetUtils::DestroyConstraint;"
				"FConstraintInstance::SnapTransformsToDefault;UPhysicsConstraintTemplate::SetDefaultProfile;"
				"UPhysicsConstraintTemplate::UpdateProfileInstance;UPhysicsConstraintTemplate::AddConstraintProfile;"
				"UPhysicsConstraintTemplate::RemoveConstraintProfile;USkeletalBodySetup::AddPhysicalAnimationProfile;"
				"USkeletalBodySetup::RemovePhysicalAnimationProfile;UPhysicsAsset::DisableCollision;UPhysicsAsset::EnableCollision;"
				"UBodySetup::InvalidatePhysicsData;UBodySetup::CreatePhysicsMeshes;FKConvexElem::UpdateElemBox;"
				"FBodyInstance::FixupData;UPhysicsAsset::RefreshPhysicsAssetChange")),
	};
}

const FBinding Bindings[] = {
	{TEXT("physics.asset.read.v1"), &HandleRead},
	{TEXT("physics.status.v1"), &HandleStatus},
	{TEXT("physics.asset.edit.v1"), &HandleEdit},
};
} // namespace

bool Fail(FCall& Call, FStringView Code, FStringView Message, FStringView Path)
{
	if (Call.Error.Code.IsEmpty())
	{
		Call.Error.Set(Code, Message, Path);
	}
	return false;
}

double ShortestFloat(double Value)
{
	const float Single = static_cast<float>(Value);
	if (!FMath::IsFinite(Single))
	{
		return Value;
	}
	for (int32 Digits = 1; Digits <= 9; ++Digits)
	{
		const FString Text = FString::Printf(TEXT("%.*g"), Digits, static_cast<double>(Single));
		const double Parsed = FCString::Atod(*Text);
		if (static_cast<float>(Parsed) == Single)
		{
			return Parsed;
		}
	}
	return Single;
}

FValue MakeName(FName Name)
{
	return MakeString(Name.IsNone() ? FString() : Name.ToString());
}

FString Escape(const FString& Name)
{
	return Name.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
}

void AddRecordPage(TArray<FValue>& Records, int64 Offset, FObjectBuilder& Result)
{
	TArray<FValue> Page;
	int32 NodeCount = 8;
	int32 Index = static_cast<int32>(FMath::Clamp<int64>(Offset, 0, Records.Num()));
	for (; Index < Records.Num(); ++Index)
	{
		const int32 RowNodes = CountValueNodes(Records[Index]);
		if (!Page.IsEmpty() && NodeCount + RowNodes > QueryValueNodeBudget)
		{
			break;
		}
		NodeCount += RowNodes;
		Page.Add(MoveTemp(Records[Index]));
	}
	Result.AddInt(TEXT("total"), Records.Num());
	Result.AddInt(TEXT("next_offset"), Index < Records.Num() ? Index : -1);
	Result.Add(TEXT("records"), MakeArray(MoveTemp(Page)));
}

TConstArrayView<FAssetDocumentPrimitiveDescriptor> GetDescriptors()
{
	static const TArray<FAssetDocumentPrimitiveDescriptor> Descriptors = BuildDescriptors();
	return Descriptors;
}

TConstArrayView<FBinding> GetBindings()
{
	return Bindings;
}
} // namespace UE::AssetDocument::Graph::Physics

TSharedRef<IAssetDocumentPrimitiveProvider> CreateAssetDocumentPhysicsPrimitiveProvider()
{
	namespace PhysicsTable = UE::AssetDocument::Graph::Physics;
	return CreateAssetDocumentAssetTablePrimitiveProvider(PhysicsTable::ProviderId, &PhysicsTable::GetDescriptors, &PhysicsTable::GetBindings);
}
