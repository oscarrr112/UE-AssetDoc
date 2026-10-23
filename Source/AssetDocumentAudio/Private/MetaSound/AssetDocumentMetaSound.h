// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "AssetDocumentGraphValue.h"
#include "MetasoundFrontendDocument.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/StrongObjectPtr.h"

class IAssetDocumentDerivedStateRebuilder;
class IMetaSoundDocumentInterface;
class UMetaSoundBuilderDocument;
struct FMetaSoundFrontendDocumentBuilder;
class FJsonObject;

// Shared pieces of the MetaSound primitives (asset-document-metasound-design.md §3-§5).
namespace UE::AssetDocument::Graph::Audio::MetaSound
{
// The MetaSound document interface of a MetaSound Source / Patch asset; null for anything else
// (including the transient builder documents the frontend uses for registration).
const IMetaSoundDocumentInterface* AsMetaSound(const UObject& Object);

// ID the read gives an input template node it injected for the hidden input node InputNodeID
// (FInputNodeTemplate::InitTemplateNode creates them with FGuid::NewGuid). The apply creates the
// same IDs when its first step injects them on the real asset, so an extract re-applies as a no-op.
FGuid MakeInputTemplateNodeId(const FGuid& InputNodeID);

// The document of a MetaSound as the MetaSound editor shows it right after opening
// (MetasoundEditor.cpp: FindOrBeginBuilding, VersionAsset on engines that version on open,
// FInputNodeTemplate::Inject(false)).
// The asset itself is never touched: its document is copied into a transient UMetaSoundBuilderDocument
// (UMetaSoundBuilderDocument::Create(const IMetaSoundDocumentInterface&)), a builder works on the copy,
// and nothing is registered with the frontend. Versioning already ran when the asset was loaded
// (FAssetHelper::SerializeToArchive), so the copy is at the current document version. Injection runs
// on the builder's build page (the default page when it exists), inside FDocumentIDGenerator determinism,
// and the injected template nodes are renamed to MakeInputTemplateNodeId.
class FNormalizedDocument
{
public:
	explicit FNormalizedDocument(const UObject& InAsset);
	~FNormalizedDocument();

	FNormalizedDocument(const FNormalizedDocument&) = delete;
	FNormalizedDocument& operator=(const FNormalizedDocument&) = delete;

	bool IsValid() const { return Builder.IsValid(); }
	const UObject& GetAsset() const { return Asset; }
	const FMetasoundFrontendDocument& GetDocument() const;
	const FMetaSoundFrontendDocumentBuilder& GetBuilder() const { return *Builder; }
	// Template nodes the open-time injection added (their final IDs), sorted.
	const TArray<FGuid>& GetInjectedNodeIds() const { return InjectedNodeIds; }

private:
	const UObject& Asset;
	TStrongObjectPtr<UMetaSoundBuilderDocument> Copy;
	TUniquePtr<FMetaSoundFrontendDocumentBuilder> Builder;
	TArray<FGuid> InjectedNodeIds;
};

// ---- literals (design §3.4) -------------------------------------------------------------------------

// Element data type of an array data type ("Float:Array" -> "Float"); the name itself otherwise.
FName GetElementDataType(FName DataType, bool* bOutIsArray = nullptr);

// Document form of a literal for a pin / member of DataType: Bool -> bool, Int32 -> integer, Float / Time
// -> number (shortest float32), String -> string, enum data types -> the entry name (integer when the
// value has no entry), UObject proxies -> object path or null, "<T>:Array" -> array of the element form,
// None (Trigger, Audio, default-constructed) -> null. The form follows the data type, not the stored
// literal type, so an old Integer literal on a Float pin reads as the same number as a Float literal.
FValue EncodeLiteral(const FMetasoundFrontendLiteral& Literal, FName DataType);

// Literal of a document value for DataType (the inverse of EncodeLiteral), stored in the data type's
// preferred literal type as the editor's pin / member default widgets store it (FGraphBuilder::GetPinLiteral):
// enum entries by name or value, objects by path (loaded, checked against the data type's class), arrays
// element-wise, Trigger / Audio only null. False with OutProblem for a value the data type cannot take.
bool DecodeLiteral(const FValue& Value, FName DataType, FMetasoundFrontendLiteral& OutLiteral, FString& OutProblem);

// Same value for DataType (compared in document form).
bool LiteralsEqual(const FMetasoundFrontendLiteral& A, const FMetasoundFrontendLiteral& B, FName DataType);

// The class default of a node input (what the pin's "reset to default" restores): the default-page default of the
// node class input of the same name, or of the last class input for an Output node. Null when the node, the input or
// its class is unknown, or the class input has no default.
const FMetasoundFrontendLiteral* FindNodeInputClassDefault(const FMetaSoundFrontendDocumentBuilder& Builder, const FGuid& NodeID,
	const FGuid& VertexID, const FGuid* PageID);

// ---- registry ---------------------------------------------------------------------------------------

// The registered node classes, the highest version of each (the classes the editor's node menu offers).
TArray<FMetasoundFrontendClass> FindAllRegisteredClasses();

// The registered node class ClassName of major version Major, at its highest minor version. False when none is registered.
bool FindRegisteredNodeClass(const FMetasoundFrontendClassName& ClassName, int32 Major, FMetasoundFrontendClass& OutClass);

// The registered interfaces, the highest version of each.
TArray<FMetasoundFrontendInterface> FindAllRegisteredInterfaces();

// The registered definition of an interface version. False when the version is not registered.
bool FindRegisteredInterface(const FMetasoundFrontendVersion& Version, FMetasoundFrontendInterface& OutInterface);

// ---- presets ---------------------------------------------------------------------------------------

// Names of the preset inputs that inherit their default from the parent (the input's "override inherited default"
// check box is clear). Empty for a MetaSound that is no preset.
TSet<FName> GetInputsInheritingDefault(const FMetaSoundFrontendDocumentBuilder& Builder);

// Makes a preset input inherit its parent's default (bInherits) or keep its own default (the current value). True when
// the input's state changed.
bool SetInputInheritsDefault(FMetaSoundFrontendDocumentBuilder& Builder, FName InputName, bool bInherits);

// Literal form of a data type for discovery: "bool", "integer", "number", "string", "enum", "object",
// "none", with "[]" appended for array types.
FString DescribeLiteralForm(FName DataType);

// ---- names ----------------------------------------------------------------------------------------

// "Namespace.Name.Variant" (empty variant dropped), FMetasoundFrontendClassName::ToString.
FString GetClassFullName(const FMetasoundFrontendClassName& ClassName);

// Object paths of the MetaSounds this asset uses as nodes, by their class full name (the asset's
// ReferencedAssetClassObjects, then IMetaSoundAssetManager for asset classes not among them).
class FAssetClassPaths
{
public:
	explicit FAssetClassPaths(const UObject& Asset);
	// Object path of an asset class; empty for a class that is not a known MetaSound asset.
	FString Find(const FMetasoundFrontendClassMetadata& Metadata) const;

private:
	TMap<FString, FString> PathsByClassName;
};

// Page display name: "Default" for the default page, the UMetaSoundSettings page name otherwise, the page
// ID for a page the project settings do not define.
FString GetPageName(const FGuid& PageID);

// Page names a document may use (UMetaSoundSettings::IteratePageSettings): "Default", then the project's pages in
// settings order.
TArray<FString> GetConfiguredPageNames();

// The page a document name stands for: "Default", a configured page name, or the 32 hex digits GetPageName gives a
// page the project settings do not define (bOutConfigured false). False for any other name.
bool FindPageID(const FString& Name, FGuid& OutPageID, bool* bOutConfigured = nullptr);

// The pages of a root graph in the order documents list them: the default page, the configured pages in settings
// order, then pages the settings do not define (document order). The frontend keeps pages in creation order and
// RemoveGraphPage swaps, so that order is not authored.
TArray<FGuid> GetOrderedPageIDs(const FMetasoundFrontendGraphClass& RootGraph);

// The page the frontend registers (and plays): the first page of UMetaSoundSettings::GetPageOrder the document
// implements (FindPreferredPage, as registration and the editor resolve it).
FGuid GetRegisteredPageID(const FMetasoundFrontendGraphClass& RootGraph);

// ---- interfaces -----------------------------------------------------------------------------------

struct FInterfaceFacts
{
	FMetasoundFrontendVersion Version;
	bool bRegistered = false;
	// What the Interfaces Details panel allows for the asset's class (MetasoundDetailCustomization.cpp
	// CanAddOrRemoveInterface: the class options' bIsModifiable; no options = modifiable; unregistered = not).
	bool bModifiable = false;
	// The registered definition (members, required outputs); unset when not registered.
	TOptional<FMetasoundFrontendInterface> Definition;
};

// The declared interfaces of a document, sorted by name.
TArray<FInterfaceFacts> DescribeInterfaces(const FMetasoundFrontendDocument& Document, const UClass& AssetClass);

// ---- compile status (design §5.2, validation without registration) --------------------------------

// Compile describer for MetaSound Sources / Patches (registered by the module): {Status, Messages, Registered}.
// Messages[{Severity, Message, Graph (page), NodeGuid, Source: Validation | Build}]: the editor's validation on every
// page, then the operator build check of the registered graph (when the MetaSound is registered).
TSharedPtr<FJsonObject> DescribeCompileStatus(const UObject& Asset);

// Operator build check (design §5.1 step 3): the graph the frontend registered for the MetaSound is built into an
// operator (FOperatorBuilder, all validations, no pruning) as a generator builds it when the sound plays; its build
// errors are what a human only sees in the log on Play. False when the MetaSound has no registered graph.
struct FBuildMessage
{
	FString Message;
	// Frontend node IDs the error names (its nodes, then the nodes of its edges and vertices), without duplicates.
	TArray<FGuid> NodeIDs;
};
bool CheckOperatorBuild(const UObject& Asset, TArray<FBuildMessage>& OutMessages);

// The settings of a new member settings object (widget) of Class as the MetaSound editor makes it: the class
// default, except that a Float input of a MetaSound that is no preset has the editor setting Default Input Widget
// Type (Editor Preferences > MetaSound) as its WidgetType: UMetasoundEditorGraphMemberDefaultFloat::Initialize sets
// it whenever the editor binds the settings object to a member of its transient graph, i.e. on each first open of
// the editor after a load. A transient object, alive while the returned pointer is.
TStrongObjectPtr<UObject> MakeEditorWidgetDefaults(const UClass& Class, bool bPreset);

// The node IDs of a DescribeCompileStatus message row (NodeGuids; NodeGuid alone when it has none).
TArray<FString> GetMessageNodeIds(const FJsonObject& Row);

// Automation only: while set, a node with this ID validates with an error (the rollback test of a failed
// validation; documents cannot reach the editor's own validation errors on purpose). Invalid GUID = off.
void SetValidationErrorNodeForTest(const FGuid& NodeID);

// ---- node configuration ---------------------------------------------------------------------------

// The configuration a new node of this class gets (FMetaSoundFrontendDocumentBuilder::AddNodeInternal:
// INodeClassRegistry::CreateFrontendNodeConfiguration); invalid for classes without one (and templates).
TInstancedStruct<FMetaSoundFrontendNodeConfiguration> MakeDefaultNodeConfiguration(const FMetasoundFrontendClassMetadata& Metadata);

// ---- edit / compile ---------------------------------------------------------------------------------

// The asset is open in an asset editor (the MetaSound editor). Edits then land in the open editor as its own do:
// the builder fills the document's modify context and the editor's tick synchronizes its graph from it.
bool IsOpenInMetaSoundEditor(const UObject& Asset);

// The MetaSound editor's Update Node (FEditor::UpdateSelectedNodeClasses) for one node: INodeController::
// ReplaceWithVersion (same node ID, style, configuration; connections and literals kept by vertex name and type),
// RemoveUnreferencedDependencies + SynchronizeDependencyMetadata, SetDocumentModified, then ReloadBuilder (the
// controller writes the document past the builder's caches, as the editor's PostEditUndo reloads them).
// AssetDocumentMetaSoundUpdateNode.cpp: the one place that uses the (soft-deprecated) controller API.
bool ReplaceNodeWithVersion(UObject& Asset, const FGuid& NodeID, const FMetasoundFrontendVersionNumber& Version, FString& OutError);

// What the editor does after an edit: UMetaSoundEditorSubsystem::RegisterGraphWithFrontend(bForceViewSync),
// ConformObjectToDocument, and a wait for async graph registration.
void RegisterAsTheEditorDoes(UObject& Asset);

// Registration creates the audio proxies of the document's object literals (FProxyDataCache::CreateAndCacheProxies).
// Without an audio device (-NoSound: FApp::CanEverRenderAudio() is false) USoundWave::CreateProxyData returns null and
// Harmonix's UFusionPatch::CreateProxyData asserts on it (FKeyzoneSettings::InitProxyData), which would crash the
// editor. Registration also registers the MetaSounds the document uses as nodes (a preset's parent among them), so
// those are searched too. Returns the path of the first such literal object (empty when registration is safe).
FString FindLiteralNeedingAudioDevice(const UObject& Asset);

// Rollback rebuilder (registered by the module): re-registers a MetaSound after Core restored its package.
TSharedRef<IAssetDocumentDerivedStateRebuilder> MakeMetaSoundRebuilder();

// Validate's trial target of a MetaSound Source / Patch is a staged package asset (the document builder asserts
// IsAsset(); AssetDocumentEditorTrialScope.h, staged trial classes), discarded as the editor deletes a never-saved
// MetaSound: its builder finished and its graph unregistered from the frontend and the MetaSound asset manager.
// Registered by the module (AssetDocumentMetaSoundTrial.cpp).
void RegisterStagedTrial();
void UnregisterStagedTrial();

// Configures the MetaSound New-asset factories with a preset parent on engines whose factories take a document
// template (the generic factory context of a preset create).
void RegisterFactoryContext();
void UnregisterFactoryContext();

// ---- list_types kinds (design §3.6) ----------------------------------------------------------------

bool ListNodeTypes(const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError);
bool ListDataTypes(const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError);
bool ListInterfaces(const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError);
} // namespace UE::AssetDocument::Graph::Audio::MetaSound
