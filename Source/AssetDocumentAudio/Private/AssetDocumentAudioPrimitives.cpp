// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Audio asset primitives (provider "audio", owner Audio) of the MetaSound profile
// (docs/architecture/asset-document-metasound-design.md §4.2):
//   audio.metasound.read.v1     the document of a MetaSound Source / Patch as the MetaSound editor shows it after
//                               opening (MetaSound/AssetDocumentMetaSoundRead.cpp; the open-time normalisation is
//                               MetaSound/AssetDocumentMetaSoundNormalizedRead.cpp)
//   audio.metasound.catalog.v1  registered node classes, data types and interfaces
//                               (MetaSound/AssetDocumentMetaSoundCatalog.cpp)
//   audio.metasound.edit.v1     the editor's document edits (MetaSound/AssetDocumentMetaSoundEdit.cpp), mutating
//   audio.metasound.compile.v1  registration + validation after edits (MetaSound/AssetDocumentMetaSoundCompile.cpp),
//                               mutating (registration auto-updates node versions in the document)
//   audio.soundcue.read.v1      a SoundCue's validation status, derived values, quality levels, waves and the
//                               new-node Details values of node classes (SoundCue/AssetDocumentSoundCueRead.cpp); the
//                               SoundCue graph itself is read and edited through the shared graph primitives
// The read-only ones never register anything with the MetaSound frontend, dirty a package or change the asset.

#include "AssetDocumentAudioPrimitives.h"

#include "AssetDocumentAudioProvider.h"
#include "AssetDocumentGraphProvider.h"

namespace UE::AssetDocument::Graph::Audio
{
namespace
{
constexpr uint32 AudioProviderVersion = 1;

FAssetDocumentPrimitiveDescriptor MakeDescriptor(const TCHAR* PrimitiveId, bool bMutating, const TCHAR* AuthoritativeApi)
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = PrimitiveId;
	Descriptor.Version = AudioProviderVersion;
	Descriptor.ProviderId = ProviderId;
	Descriptor.Owner = EAssetDocumentPrimitiveOwner::Audio;
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
		TEXT("MetaSound.UnsupportedAsset"), TEXT("MetaSound.UnknownCatalogKind"), TEXT("MetaSound.ReadFailed"),
		TEXT("MetaSound.InteractiveEditRefused"), TEXT("MetaSound.EditFailed"),
		TEXT("MetaSound.InvalidName"), TEXT("MetaSound.InvalidLiteral"), TEXT("MetaSound.InvalidMember"),
		TEXT("MetaSound.DuplicateMember"), TEXT("MetaSound.UnknownMember"), TEXT("MetaSound.UnknownDataType"),
		TEXT("MetaSound.UnknownInterface"), TEXT("MetaSound.InterfaceNotModifiable"), TEXT("MetaSound.UnknownWidgetProperty"),
		TEXT("MetaSound.InvalidWidgetValue"), TEXT("MetaSound.UnknownNodeClass"), TEXT("MetaSound.UnsupportedNodeClass"),
		TEXT("MetaSound.UnknownAsset"), TEXT("MetaSound.NotReferenceable"), TEXT("MetaSound.ReferenceCycle"),
		TEXT("MetaSound.RerouteType"), TEXT("MetaSound.DuplicateNodeId"), TEXT("MetaSound.DuplicateSetNode"),
		TEXT("MetaSound.NodeNotFound"), TEXT("MetaSound.NodeNotDeletable"), TEXT("MetaSound.PinNotFound"),
		TEXT("MetaSound.IncompatibleConnection"), TEXT("MetaSound.ConnectionCycle"), TEXT("MetaSound.AudioDeviceRequired"),
		TEXT("MetaSound.InvalidConfiguration"), TEXT("MetaSound.UpdateNodeNotSupported"), TEXT("MetaSound.NotAPreset"),
		TEXT("SoundCue.UnsupportedAsset"), TEXT("SoundCue.UnknownNodeClass")};
	Descriptor.RequiredConsumerIds = {TEXT("route.generic.v1")};
	Descriptor.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
	Descriptor.HandleArgumentSchemas.Add({TEXT("asset"), EAssetDocumentHandleKind::Asset,
		bMutating ? EAssetDocumentHandleAccess::Mutable : EAssetDocumentHandleAccess::ReadOnly, true});

	FAssetDocumentPrimitiveAdmissionEvidence& Evidence = Descriptor.Admission;
	Evidence.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
	Evidence.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper;
	Evidence.ExactEngineVersion = TEXT("5.7.4");
	Evidence.PhysicalOwner = EAssetDocumentPrimitiveOwner::Audio;
	Evidence.PublicModule = TEXT("AssetDocumentAudio");
	Evidence.PublicHeader = TEXT("AssetDocumentAudioProvider.h");
	Evidence.ExportedAuthoritativeApi = AuthoritativeApi;
	Evidence.AuthorityContextId = TEXT("assetdocument.editor.game-thread.editor-ready.v1");
	Evidence.PrimaryOperationId = Descriptor.PrimitiveId;
	Evidence.IdentityLifetimeContractId = TEXT("identity.request-transaction-revision-bound.v1");
	Evidence.TransactionContractId = TEXT("transaction.descriptor-governed.v1");
	Evidence.RollbackContractId = TEXT("rollback.core-package-snapshot.v1");
	Evidence.CompletionContractId = Descriptor.PrimitiveId + TEXT(".completion.v1");
	Evidence.SaveReloadEvidenceId = TEXT("UEAssetDoc.MetaSound.Python");
	Evidence.FailureInjectionTestIds = {TEXT("UEAssetDoc.MetaSound.Python")};
	Evidence.VerificationTestIds = {TEXT("UEAssetDoc.MetaSound.Python")};
	Evidence.AuthorityFingerprint = FString::Printf(TEXT("ue-5.7.4|audio|%s|%s"), PrimitiveId, AuthoritativeApi);
	return Descriptor;
}

TArray<FAssetDocumentPrimitiveDescriptor> BuildDescriptors()
{
	return {
		MakeDescriptor(TEXT("audio.metasound.read.v1"), false,
			TEXT("IMetaSoundDocumentInterface::GetConstDocument;UMetaSoundBuilderDocument::Create;FMetaSoundFrontendDocumentBuilder;FInputNodeTemplate::Inject;FDocumentIDGenerator::FScopeDeterminism;FMetaSoundFrontendDocumentBuilder::GetNodeInputClassDefault;IDataTypeRegistry::GetDesiredLiteralType;IInterfaceRegistry::FindInterfaceRegistryEntry")),
		MakeDescriptor(TEXT("audio.metasound.catalog.v1"), false,
			TEXT("ISearchEngine::FindAllClasses;ISearchEngine::FindAllInterfaces;IDataTypeRegistry::IterateDataTypeInfo;IDataTypeRegistry::GetEnumInterfaceForDataType")),
		MakeDescriptor(TEXT("audio.metasound.edit.v1"), true,
			TEXT("FDocumentBuilderRegistry::FindOrBeginBuilding;FMetaSoundFrontendDocumentBuilder;ISearchEngine::FindClassWithHighestMinorVersion;"
				"ISearchEngine::FindInterfaceWithHighestVersion;FMetasoundAssetBase::AddingReferenceCausesLoop;UMetaSoundBuilderBase::RegisterGraphIfOutstandingTransactions;"
				"INodeTemplateRegistry::FindTemplate;UMetaSoundEditorSubsystem::FindOrCreateGraphInputMetadata;UObject::PostEditChangeChainProperty;"
				"FMetaSoundFrontendDocumentBuilder::SetNodeConfiguration;INodeClassRegistry::CreateFrontendNodeConfiguration;INodeController::ReplaceWithVersion;"
				"FMetaSoundFrontendDocumentBuilder::SetGraphInputInheritsDefault;UMetaSoundBuilderBase::ConvertFromPreset")),
		MakeDescriptor(TEXT("audio.metasound.compile.v1"), true,
			TEXT("UMetaSoundEditorSubsystem::RegisterGraphWithFrontend;IMetaSoundDocumentInterface::ConformObjectToDocument;"
				"INodeClassRegistry::WaitForAsyncGraphRegistration;FMetaSoundFrontendDocumentBuilder::DiffAgainstRegistryInterface;INodeTemplate::HasRequiredConnections")),
		MakeDescriptor(TEXT("audio.soundcue.read.v1"), false,
			TEXT("USoundCue::CacheAggregateValues;USoundNode::GetMinChildNodes;USoundNode::GetMaxChildNodes;USoundNode::SetChildNodes;"
				"USoundCue::ConstructSoundNode;USoundNode::CreateStartingConnectors;UAudioSettings::GetQualityLevelSettings;"
				"UAssetEditorSubsystem::FindEditorForAsset")),
	};
}

const FBinding Bindings[] = {
	{TEXT("audio.metasound.read.v1"), &HandleMetaSoundRead},
	{TEXT("audio.metasound.catalog.v1"), &HandleMetaSoundCatalog},
	{TEXT("audio.metasound.edit.v1"), &HandleMetaSoundEdit},
	{TEXT("audio.metasound.compile.v1"), &HandleMetaSoundCompile},
	{TEXT("audio.soundcue.read.v1"), &HandleSoundCueRead},
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

double ShortestFloat(float Value)
{
	if (!FMath::IsFinite(Value))
	{
		return Value;
	}
	for (int32 Digits = 1; Digits <= 9; ++Digits)
	{
		const FString Text = FString::Printf(TEXT("%.*g"), Digits, static_cast<double>(Value));
		const double Parsed = FCString::Atod(*Text);
		if (static_cast<float>(Parsed) == Value)
		{
			return Parsed;
		}
	}
	return Value;
}

FValue MakeCoordinate(double Value)
{
	const double Rounded = FMath::RoundToDouble(Value);
	if (FMath::IsFinite(Value) && Rounded == Value && FMath::Abs(Value) < 1.0e15)
	{
		return MakeInt(static_cast<int64>(Rounded));
	}
	return MakeNumber(ShortestFloat(static_cast<float>(Value)));
}

FString PathOf(const UObject* Object)
{
	return Object != nullptr ? Object->GetPathName() : FString();
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
} // namespace UE::AssetDocument::Graph::Audio

TSharedRef<IAssetDocumentPrimitiveProvider> CreateAssetDocumentAudioPrimitiveProvider()
{
	namespace AudioTable = UE::AssetDocument::Graph::Audio;
	return CreateAssetDocumentAssetTablePrimitiveProvider(AudioTable::ProviderId, &AudioTable::GetDescriptors, &AudioTable::GetBindings);
}
