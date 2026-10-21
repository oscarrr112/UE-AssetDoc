// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Rig asset primitives (provider "rig", owner Rig) of the IK Rig and IK Retargeter profiles
// (docs/architecture/asset-document-ikrig-design.md §4.4):
//   rig.ikrig.read.v1        the authored state of an IK Rig as the IK Rig editor shows it (AssetDocumentIKRigRead.cpp)
//   rig.ikretargeter.read.v1 the authored state of an IK Retargeter (AssetDocumentIKRetargeterRead.cpp)
//   rig.status.v1            the editors' processor initialization (their output log) (AssetDocumentRigStatus.cpp)
//   rig.catalog.v1           solver and retarget op types (AssetDocumentRigCatalog.cpp)
//   rig.ikrig.edit.v1        IK Rig edits as the IK Rig editor makes them (AssetDocumentIKRigEdit.cpp)
//   rig.ikretargeter.edit.v1 IK Retargeter edits as the IK Retarget editor makes them (AssetDocumentIKRetargeterEdit.cpp)
//   rig.suggest.v1           the editors' automatic setup on a transient copy (AssetDocumentRigSuggest.cpp)
// and of the Control Rig profile (docs/architecture/asset-document-controlrig-design.md §4.2, phases CR0 / CR1):
//   rig.controlrig.read.v1    the authored state of a Control Rig (AssetDocumentControlRigRead.cpp, ...GraphRead.cpp)
//   rig.controlrig.catalog.v1 RigVM node / value types and rig modules (AssetDocumentControlRigCatalog.cpp)
//   rig.controlrig.edit.v1    hierarchy, variable, graph and asset edits as the Control Rig editor makes them
//                             (AssetDocumentControlRigEdit.cpp, AssetDocumentControlRigGraphEdit.cpp)
//   rig.controlrig.compile.v1 the editor's Compile (AssetDocumentControlRigCompile.cpp)
// The read-only ones never dirty a package or change the asset.

#include "AssetDocumentRigPrimitives.h"

#include "AssetDocumentControlRig.h"
#include "AssetDocumentGraphProvider.h"
#include "AssetDocumentRigProvider.h"
#include "Rig/IKRigDefinition.h"

namespace UE::AssetDocument::Graph::Rig
{
namespace
{
constexpr uint32 RigProviderVersion = 1;

FAssetDocumentPrimitiveDescriptor MakeDescriptor(const TCHAR* PrimitiveId, bool bMutating, const TCHAR* AuthoritativeApi,
	const TCHAR* EvidenceTestId = TEXT("UEAssetDoc.IKRig.Python"))
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = PrimitiveId;
	Descriptor.Version = RigProviderVersion;
	Descriptor.ProviderId = ProviderId;
	Descriptor.Owner = EAssetDocumentPrimitiveOwner::Rig;
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
		TEXT("IKRig.UnsupportedAsset"), TEXT("IKRig.ReadFailed"), TEXT("IKRig.UnknownCatalogKind"),
		TEXT("IKRig.InteractiveEditRefused"), TEXT("IKRig.EditFailed"),
		TEXT("IKRig.InvalidName"), TEXT("IKRig.NameTaken"), TEXT("IKRig.BoneNotFound"), TEXT("IKRig.GoalNotFound"),
		TEXT("IKRig.GoalNotCreated"), TEXT("IKRig.ChainNotFound"), TEXT("IKRig.UnknownSolverType"),
		TEXT("IKRig.SolverNotFound"), TEXT("IKRig.SolverHasNoStartBone"), TEXT("IKRig.SolverHasNoEndBone"),
		TEXT("IKRig.SolverGoalLimit"), TEXT("IKRig.SolverHasNoGoalSettings"), TEXT("IKRig.SolverHasNoBoneSettings"),
		TEXT("IKRig.BoneSettingRefused"), TEXT("IKRig.FieldNotEditable"), TEXT("IKRig.InvalidValue"),
		TEXT("IKRig.IncompatibleMesh"), TEXT("IKRig.UnknownMesh"), TEXT("IKRig.PreviewMeshRequired"),
		TEXT("IKRetargeter.UnsupportedAsset"), TEXT("IKRetargeter.ReadFailed"),
		TEXT("IKRetargeter.InteractiveEditRefused"), TEXT("IKRetargeter.EditFailed"), TEXT("IKRetargeter.UnknownIKRig"),
		TEXT("IKRetargeter.UnknownMesh"), TEXT("IKRetargeter.UnknownOpType"), TEXT("IKRetargeter.OpNotFound"),
		TEXT("IKRetargeter.SingletonOp"), TEXT("IKRetargeter.InvalidParentOp"), TEXT("IKRetargeter.NameTaken"),
		TEXT("IKRetargeter.NoTargetIKRig"), TEXT("IKRetargeter.NoChainMapping"), TEXT("IKRetargeter.NoChainSettings"),
		TEXT("IKRetargeter.UnknownTargetChain"), TEXT("IKRetargeter.ChainNotFound"), TEXT("IKRetargeter.PoseNotFound"),
		TEXT("IKRetargeter.DefaultPoseRequired"), TEXT("IKRetargeter.IKRigRequired"), TEXT("IKRetargeter.BoneNotFound"),
		TEXT("IKRetargeter.InvalidValue"), TEXT("ControlRig.UnsupportedAsset"), TEXT("ControlRig.ReadFailed"),
		TEXT("ControlRig.UnknownCatalogKind"), TEXT("ControlRig.InteractiveEditRefused"),
		TEXT("ControlRig.EditFailed"), TEXT("ControlRig.EditRefused"), TEXT("ControlRig.InvalidValue"), TEXT("ControlRig.InvalidParent"),
		TEXT("ControlRig.NameTaken"), TEXT("ControlRig.ElementNotFound"), TEXT("ControlRig.VariableNotFound"),
		TEXT("ControlRig.UnsupportedVariableType"), TEXT("ControlRig.UnknownMesh"), TEXT("ControlRig.ConversionRefused"),
		TEXT("ControlRig.ProceduralElement")};
	Descriptor.RequiredConsumerIds = {TEXT("route.generic.v1")};
	Descriptor.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
	Descriptor.HandleArgumentSchemas.Add({TEXT("asset"), EAssetDocumentHandleKind::Asset,
		bMutating ? EAssetDocumentHandleAccess::Mutable : EAssetDocumentHandleAccess::ReadOnly, true});

	FAssetDocumentPrimitiveAdmissionEvidence& Evidence = Descriptor.Admission;
	Evidence.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
	Evidence.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper;
	Evidence.ExactEngineVersion = TEXT("5.7.4");
	Evidence.PhysicalOwner = EAssetDocumentPrimitiveOwner::Rig;
	Evidence.PublicModule = TEXT("AssetDocumentRig");
	Evidence.PublicHeader = TEXT("AssetDocumentRigProvider.h");
	Evidence.ExportedAuthoritativeApi = AuthoritativeApi;
	Evidence.AuthorityContextId = TEXT("assetdocument.editor.game-thread.editor-ready.v1");
	Evidence.PrimaryOperationId = Descriptor.PrimitiveId;
	Evidence.IdentityLifetimeContractId = TEXT("identity.request-transaction-revision-bound.v1");
	Evidence.TransactionContractId = TEXT("transaction.descriptor-governed.v1");
	Evidence.RollbackContractId = TEXT("rollback.core-package-snapshot.v1");
	Evidence.CompletionContractId = Descriptor.PrimitiveId + TEXT(".completion.v1");
	Evidence.SaveReloadEvidenceId = EvidenceTestId;
	Evidence.FailureInjectionTestIds = {EvidenceTestId};
	Evidence.VerificationTestIds = {EvidenceTestId};
	Evidence.AuthorityFingerprint = FString::Printf(TEXT("ue-5.7.4|rig|%s|%s"), PrimitiveId, AuthoritativeApi);
	return Descriptor;
}

TArray<FAssetDocumentPrimitiveDescriptor> BuildDescriptors()
{
	return {
		MakeDescriptor(TEXT("rig.ikrig.read.v1"), false,
			TEXT("UIKRigDefinition::GetSolverStructs;UIKRigDefinition::GetGoalArray;UIKRigDefinition::GetRetargetChains;"
				"FIKRigSolverBase::GetSolverSettings;FIKRigSolverBase::GetGoalSettings;FIKRigSolverBase::GetBoneSettings;"
				"FIKRigSkeleton::ValidateChainAndGetBones")),
		MakeDescriptor(TEXT("rig.ikretargeter.read.v1"), false,
			TEXT("UIKRetargeter::GetRetargetOps;UIKRetargeterController::GetRetargetPoses;UIKRetargeterController::GetCurrentRetargetPoseName;"
				"FIKRetargetOpBase::GetSettings;FIKRetargetOpBase::GetChainMapping;FIKRetargetOpBase::OnAddedToStack")),
		MakeDescriptor(TEXT("rig.status.v1"), false,
			TEXT("FIKRigProcessor::Initialize;FIKRetargetProcessor::Initialize;FIKRigLogger;FIKRigSolverBase::GetWarningMessage;"
				"FIKRetargetOpBase::GetWarningMessage")),
		MakeDescriptor(TEXT("rig.ikrig.edit.v1"), true,
			TEXT("UIKRigController::SetSkeletalMesh;UIKRigController::SetBoneExcluded;UIKRigController::AddNewGoal;"
				"UIKRigController::RemoveGoal;UIKRigController::RenameGoal;UIKRigController::SetGoalBone;UIKRigController::ModifyGoal;"
				"UIKRigController::SetGoalCurrentTransform;UIKRigController::AddSolver;UIKRigController::RemoveSolver;"
				"UIKRigController::MoveSolverInStack;UIKRigController::SetSolverEnabled;UIKRigController::SetStartBone;"
				"UIKRigController::SetEndBone;UIKRigController::ConnectGoalToSolver;UIKRigController::DisconnectGoalFromSolver;"
				"UIKRigController::AddBoneSetting;UIKRigController::RemoveBoneSetting;UIKRigController::AddRetargetChain;"
				"UIKRigController::RemoveRetargetChain;UIKRigController::RenameRetargetChain;UIKRigController::SetRetargetChainStartBone;"
				"UIKRigController::SetRetargetChainEndBone;UIKRigController::SetRetargetChainGoal;UIKRigController::SetRetargetRoot;"
				"UIKRigStructWrapperBase::PostEditChangeProperty")),
		MakeDescriptor(TEXT("rig.ikretargeter.edit.v1"), true,
			TEXT("UIKRetargeterController::SetIKRig;UIKRetargeterController::AssignIKRigToAllOps;UIKRetargeterController::SetPreviewMesh;"
				"UIKRetargeterController::AddRetargetOp;UIKRetargeterController::RemoveRetargetOp;UIKRetargeterController::SetOpName;"
				"UIKRetargeterController::MoveRetargetOpInStack;UIKRetargeterController::SetParentOpByName;"
				"UIKRetargeterController::SetRetargetOpEnabled;UIKRetargeterController::OnOpPropertyChanged;"
				"UIKRetargeterController::SetSourceChain;UIKRetargeterController::CreateRetargetPose;"
				"UIKRetargeterController::RemoveRetargetPose;UIKRetargeterController::RenameRetargetPose;"
				"UIKRetargeterController::SetCurrentRetargetPose;UIKRetargeterController::ResetRetargetPose;"
				"FIKRetargetPose::SetDeltaRotationForBone;FIKRetargetPose::SetRootTranslationDelta;UIKRigStructViewer::TriggerReinitIfNeeded")),
		MakeDescriptor(TEXT("rig.suggest.v1"), false,
			TEXT("UIKRigController::AutoGenerateRetargetDefinition;UIKRigController::SetRetargetDefinition;UIKRigController::AutoGenerateFBIK;"
				"UIKRetargeterController::AutoAlignAllBones;UIKRetargeterController::AutoMapChains")),
		MakeDescriptor(TEXT("rig.catalog.v1"), false,
			TEXT("FIKRigSolverBase::GetNiceName;FIKRetargetOpBase::GetDefaultName;FIKRetargetOpBase::GetParentOpType;"
				"FIKRetargetOpBase::IsSingleton;FIKRetargetOpBase::CanHaveChildOps")),
		MakeDescriptor(TEXT("rig.controlrig.read.v1"), false,
			TEXT("FRigVMClient::GetModels;FRigVMClient::GetFunctionLibrary;URigVMGraph::GetNodes;URigVMPin::GetDefaultValue;"
				"URigVMPin::GetOriginalDefaultValue;URigVMPin::GetSourceLinks;URigHierarchy::GetAllKeys;URigHierarchy::GetParentWeightArray;"
				"URigHierarchy::GetControlValue;URigVMBlueprint::GetAssetVariables;FModularRigModel::Modules"),
			TEXT("UEAssetDoc.ControlRig.Read")),
		MakeDescriptor(TEXT("rig.controlrig.catalog.v1"), false,
			TEXT("FRigVMRegistry::GetFactories;FRigVMRegistry::GetTemplates;FRigVMRegistry::GetTypesForCategory;"
				"FRigVMStruct::ShouldCreatePinForProperty;FRigVMStruct::GetPinDirectionFromProperty;IAssetRegistry::GetAssets"),
			TEXT("UEAssetDoc.ControlRig.ListTypes")),
		MakeDescriptor(TEXT("rig.controlrig.edit.v1"), true,
			TEXT("URigHierarchyController::AddBone;URigHierarchyController::AddNull;URigHierarchyController::AddControl;"
				"URigHierarchyController::AddAnimationChannel;URigHierarchyController::AddCurve;URigHierarchyController::AddConnector;"
				"URigHierarchyController::AddSocket;URigHierarchyController::RemoveElement;URigHierarchyController::RenameElement;"
				"URigHierarchyController::ReorderElement;URigHierarchyController::SetParent;URigHierarchyController::AddParent;"
				"URigHierarchyController::RemoveAllParents;URigHierarchyController::AddAvailableSpace;"
				"URigHierarchyController::AddChannelHost;URigHierarchy::SetControlSettings;URigHierarchy::SetControlValue;"
				"URigHierarchy::SetLocalTransform;URigHierarchy::SetParentWeight;URigHierarchy::GetMetadataForElement;"
				"URigVMBlueprint::AddMemberVariable;URigVMBlueprint::RenameMemberVariable;URigVMBlueprint::ChangeMemberVariableType;"
				"URigVMBlueprint::RemoveMemberVariable;IControlRigAssetInterface::SetPreviewMesh;IControlRigAssetInterface::GetSourceHierarchyImport;"
				"IControlRigAssetInterface::TurnIntoControlRigModule;IControlRigAssetInterface::TurnIntoStandaloneRig;"
				"URigVMController::AddUnitNode;URigVMController::AddUnitNodeWithDefaults;URigVMController::AddTemplateNode;"
				"URigVMController::AddVariableNode;URigVMController::AddFreeRerouteNode;URigVMController::AddEnumNode;"
				"URigVMController::AddInvokeEntryNode;URigVMController::AddCommentNode;URigVMController::RemoveNodeByName;"
				"URigVMController::RenameNode;URigVMController::AddLink;URigVMController::BreakLink;"
				"URigVMController::SetPinDefaultValue;URigVMController::ResetPinDefaultValue;URigVMController::BindPinToVariable;"
				"URigVMController::ResolveWildCardPin;URigVMController::AddAggregatePin;URigVMController::AddTrait;"
				"URigVMController::SetArrayPinSize;URigVMBlueprint::AddModel;URigVMBlueprint::RemoveModel;FRigVMClient::RenameModel"),
			TEXT("UEAssetDoc.ControlRig.Apply")),
		MakeDescriptor(TEXT("rig.controlrig.compile.v1"), true,
			TEXT("FKismetEditorUtilities::CompileBlueprint;IControlRigAssetInterface::RecompileModularRig;"
				"URigVMEdGraph::InitializeFromAsset;IControlRigAssetInterface::PropagateHierarchyFromBPToInstances"),
			TEXT("UEAssetDoc.ControlRig.Apply")),
	};
}

const FBinding Bindings[] = {
	{TEXT("rig.ikrig.read.v1"), &HandleIKRigRead},
	{TEXT("rig.ikretargeter.read.v1"), &HandleIKRetargeterRead},
	{TEXT("rig.status.v1"), &HandleStatus},
	{TEXT("rig.catalog.v1"), &HandleCatalog},
	{TEXT("rig.ikrig.edit.v1"), &HandleIKRigEdit},
	{TEXT("rig.ikretargeter.edit.v1"), &HandleIKRetargeterEdit},
	{TEXT("rig.suggest.v1"), &HandleSuggest},
	{TEXT("rig.controlrig.read.v1"), &ControlRig::HandleRead},
	{TEXT("rig.controlrig.catalog.v1"), &ControlRig::HandleCatalog},
	{TEXT("rig.controlrig.edit.v1"), &ControlRig::HandleEdit},
	{TEXT("rig.controlrig.compile.v1"), &ControlRig::HandleCompile},
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

FString PathOf(const UObject* Object)
{
	return Object != nullptr ? Object->GetPathName() : FString();
}

TArray<FName> GetOrderedExcludedBones(const UIKRigDefinition& Rig)
{
	const FIKRigSkeleton& Skeleton = Rig.GetSkeleton();
	TArray<FName> Ordered;
	for (const FName& Bone : Skeleton.BoneNames)
	{
		if (Skeleton.ExcludedBones.Contains(Bone))
		{
			Ordered.Add(Bone);
		}
	}
	for (const FName& Bone : Skeleton.ExcludedBones)
	{
		if (!Ordered.Contains(Bone))
		{
			Ordered.Add(Bone);
		}
	}
	return Ordered;
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
} // namespace UE::AssetDocument::Graph::Rig

TSharedRef<IAssetDocumentPrimitiveProvider> CreateAssetDocumentRigPrimitiveProvider()
{
	namespace RigTable = UE::AssetDocument::Graph::Rig;
	return CreateAssetDocumentAssetTablePrimitiveProvider(RigTable::ProviderId, &RigTable::GetDescriptors, &RigTable::GetBindings);
}
