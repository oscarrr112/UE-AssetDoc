// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Descriptor table of the "graph" provider (contract §3). The admission fields
// are the minimum the shared descriptor validator requires; rollback is the
// Core package snapshot of the declared whole-package footprint.

#include "AssetDocumentGraphPrimitives.h"

namespace UE::AssetDocument::Graph
{
namespace
{
constexpr uint32 ProviderVersion = 1;

FAssetDocumentPrimitiveDescriptor MakeDescriptor(const TCHAR* PrimitiveId, bool bMutating, const TCHAR* AuthoritativeApi)
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = PrimitiveId;
	Descriptor.Version = ProviderVersion;
	Descriptor.ProviderId = ProviderId;
	Descriptor.Owner = EAssetDocumentPrimitiveOwner::Graph;
	Descriptor.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
	Descriptor.Access = bMutating ? EAssetDocumentPrimitiveAccess::Mutating : EAssetDocumentPrimitiveAccess::ReadOnly;
	if (bMutating)
	{
		Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair};
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
	if (bMutating)
	{
		Descriptor.MutationFootprintSchemaId = TEXT("graph.footprint.whole-package.v1");
	}
	Descriptor.StableDiagnosticCodes = {
		TEXT("AD.RUNTIME.PRIMITIVE_FAILED"), TEXT("Graph.UnknownField"), TEXT("Graph.InvalidArgument"),
		TEXT("Graph.GraphNotFound"), TEXT("Graph.NodeNotFound"), TEXT("Graph.PinNotFound"),
		TEXT("Graph.ActionNotFound"), TEXT("Graph.ActionAmbiguous"), TEXT("Graph.ActionRequiresContext"), TEXT("Graph.EditorOnlyFunction"),
		TEXT("Graph.ConnectDisallowed"), TEXT("Graph.ConnectNeedsConversion"), TEXT("Graph.PinDefaultInvalid"),
		TEXT("Graph.PropertyNotEditable"), TEXT("Graph.NodeNotDeletable"), TEXT("Graph.UserPinsUnsupported"),
		TEXT("Graph.DynamicPinUnsupported"), TEXT("Graph.PinTypeNotEditable"), TEXT("Graph.GraphOperationUnsupported"), TEXT("Graph.FinalizeFailed"),
		TEXT("Graph.PieActive"), TEXT("Graph.UnsupportedSchemaVersion"), TEXT("Graph.ResultTooLarge")};
	Descriptor.RequiredConsumerIds = {TEXT("route.generic.v1")};
	Descriptor.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
	Descriptor.HandleArgumentSchemas.Add({TEXT("asset"), EAssetDocumentHandleKind::Asset,
		bMutating ? EAssetDocumentHandleAccess::Mutable : EAssetDocumentHandleAccess::ReadOnly, true});

	FAssetDocumentPrimitiveAdmissionEvidence& Evidence = Descriptor.Admission;
	Evidence.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
	Evidence.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper;
	Evidence.ExactEngineVersion = TEXT("5.7.4");
	Evidence.PhysicalOwner = EAssetDocumentPrimitiveOwner::Graph;
	Evidence.PublicModule = TEXT("AssetDocumentGraph");
	Evidence.PublicHeader = TEXT("AssetDocumentGraphAdapters.h");
	Evidence.ExportedAuthoritativeApi = AuthoritativeApi;
	Evidence.AuthorityContextId = TEXT("assetdocument.editor.game-thread.editor-ready.v1");
	Evidence.PrimaryOperationId = Descriptor.PrimitiveId;
	Evidence.IdentityLifetimeContractId = TEXT("identity.request-transaction-revision-bound.v1");
	Evidence.TransactionContractId = TEXT("transaction.descriptor-governed.v1");
	Evidence.RollbackContractId = TEXT("rollback.core-package-snapshot.v1");
	Evidence.CompletionContractId = Descriptor.PrimitiveId + TEXT(".completion.v1");
	Evidence.SaveReloadEvidenceId = TEXT("UEAssetDoc.Graph.SaveReload");
	Evidence.FailureInjectionTestIds = {TEXT("UEAssetDoc.Graph.Diagnostics")};
	Evidence.VerificationTestIds = {TEXT("UEAssetDoc.Graph")};
	Evidence.AuthorityFingerprint = FString::Printf(TEXT("ue-5.7.4|graph|%s|%s"), PrimitiveId, AuthoritativeApi);
	return Descriptor;
}

TArray<FAssetDocumentPrimitiveDescriptor> BuildDescriptors()
{
	return {
		MakeDescriptor(TEXT("graph.list.v1"), false, TEXT("IAssetDocumentGraphHostAdapter::EnumerateGraphs;UEdGraphNode::GetSubGraphs")),
		MakeDescriptor(TEXT("graph.query.v1"), false, TEXT("UEdGraph::Nodes;UEdGraphPin::LinkedTo")),
		MakeDescriptor(TEXT("graph.actions.enumerate.v1"), false, TEXT("FBlueprintActionDatabase::GetAllActions;FBlueprintActionFilter::IsFiltered;UEdGraphSchema::GetGraphContextActions")),
		MakeDescriptor(TEXT("graph.node.spawn.v1"), true, TEXT("UBlueprintNodeSpawner::Invoke;FEdGraphSchemaAction::PerformAction")),
		MakeDescriptor(TEXT("graph.node.remove.v1"), true, TEXT("FBlueprintEditorUtils::RemoveNode;UEdGraphNode::DestroyNode")),
		MakeDescriptor(TEXT("graph.pin.connect.v1"), true, TEXT("UEdGraphSchema::CanCreateConnection;UEdGraphSchema::TryCreateConnection")),
		MakeDescriptor(TEXT("graph.pin.break.v1"), true, TEXT("UEdGraphSchema::BreakSinglePinLink;UEdGraphSchema::BreakPinLinks")),
		MakeDescriptor(TEXT("graph.link.check.v1"), false, TEXT("UBlueprintNodeSpawner::GetTemplateNode;UEdGraphSchema::CanCreateConnection;UEdGraphSchema_K2::SearchForAutocastFunction;UEdGraphSchema::IsPinDefaultValid")),
		MakeDescriptor(TEXT("graph.pin.default.set.v1"), true, TEXT("UEdGraphSchema::TrySetDefaultValue;UEdGraphSchema::TrySetDefaultObject;UEdGraphSchema::TrySetDefaultText")),
		MakeDescriptor(TEXT("graph.node.property.get.v1"), false, TEXT("TFieldIterator<FProperty>")),
		MakeDescriptor(TEXT("graph.node.property.set.v1"), true, TEXT("UObject::PreEditChange;UObject::PostEditChangeProperty")),
		MakeDescriptor(TEXT("graph.node.layout.set.v1"), true, TEXT("UEdGraphNode::NodePosX;UEdGraphNode::ResizeNode;UEdGraphNode::OnUpdateCommentText")),
		MakeDescriptor(TEXT("graph.node.rename.v1"), true, TEXT("UEdGraphNode::OnRenameNode;UEdGraphNode::MakeNameValidator")),
		MakeDescriptor(TEXT("graph.node.user_pins.set.v1"), true, TEXT("UK2Node_EditablePinBase::CreateUserDefinedPin;UK2Node_EditablePinBase::RemoveUserDefinedPinByName")),
		MakeDescriptor(TEXT("graph.node.dynamic_pin.add.v1"), true, TEXT("IK2Node_AddPinInterface::AddInputPin")),
		MakeDescriptor(TEXT("graph.node.dynamic_pin.remove.v1"), true, TEXT("IK2Node_AddPinInterface::RemoveInputPin")),
		MakeDescriptor(TEXT("graph.node.pin_type.set.v1"), true, TEXT("SGraphPinIndex::OnTypeChanged;UEdGraphNode::PinTypeChanged;UEdGraphNode::ReconstructNode")),
		MakeDescriptor(TEXT("graph.create.v1"), true, TEXT("FBlueprintEditorUtils::CreateNewGraph;FBlueprintEditorUtils::AddFunctionGraph")),
		MakeDescriptor(TEXT("graph.remove.v1"), true, TEXT("FBlueprintEditorUtils::RemoveGraph")),
		MakeDescriptor(TEXT("graph.rename.v1"), true, TEXT("FBlueprintEditorUtils::RenameGraph")),
		MakeDescriptor(TEXT("graph.finalize.v1"), true, TEXT("FMetaData::SetValue;FKismetEditorUtilities::CompileBlueprint")),
		MakeDescriptor(TEXT("graph.function.flags.set.v1"), true, TEXT("UK2Node_FunctionEntry::SetExtraFlags;FBlueprintEditorUtils::SetBlueprintFunctionOrMacroCategory")),
	};
}
} // namespace

TConstArrayView<FAssetDocumentPrimitiveDescriptor> GetGraphPrimitiveDescriptors()
{
	static const TArray<FAssetDocumentPrimitiveDescriptor> Descriptors = BuildDescriptors();
	return Descriptors;
}
} // namespace UE::AssetDocument::Graph
