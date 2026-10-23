// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "AssetDocumentGraphValue.h"

class FJsonObject;
class IAssetDocumentDerivedStateRebuilder;
class IAssetDocumentOpenEditorHandler;
class UClass;
class UEdGraph;
class UEdGraphNode;
class USoundCue;
class USoundNode;

// Shared pieces of the SoundCue profile (docs/architecture/asset-document-soundcue-design.md): the Sound Cue editor's
// graph (USoundCue::SoundCueGraph) edited through the shared graph primitives (schema + host adapters,
// AssetDocumentSoundCueGraphAdapters.cpp), the raw editor-equivalent edits (AssetDocumentSoundCueRawEdits.cpp), the
// finalize validation, compile-status describer and rollback rebuilder (AssetDocumentSoundCueValidation.cpp), the
// read primitive audio.soundcue.read.v1 and the list_types kind sound_node (AssetDocumentSoundCueRead.cpp).
namespace UE::AssetDocument::Graph::Audio::SoundCue
{
// The Sound Cue editor's graph schema class (AudioEditor's USoundCueGraphSchema; its header includes a private
// AudioEditor header, so the class is found by path). Null when AudioEditor is not loaded.
UClass* GetSchemaClass();

// The cue a graph belongs to (USoundCueGraph::GetSoundCue), null for other graphs.
USoundCue* CueOfGraph(const UEdGraph* Graph);

// A SoundCue whose graph the profile edits: exactly USoundCue or a subclass that is not a SoundCueTemplate
// (template cues rebuild their graph from their settings).
bool IsEditableCue(const UObject& Object);

// True when the cue is open in an asset editor (reported by Inspect as OpenInEditor).
bool IsOpenInEditor(const UObject& Cue);

// Document class label: engine node classes (/Script/Engine.SoundNode<Name>) by <Name>, others by class path.
FString ClassLabel(const UClass& Class);
// The USoundNode class of a label (short engine name or class path); null when unknown.
UClass* ResolveClassLabel(const FString& Label);

// The node classes the Sound Cue editor's palette offers (non-abstract USoundNode classes allowed by the class
// viewer's global filter, as SSoundCuePalette / USoundCueGraphSchema::UpdateSoundNodeList filter them), sorted.
TArray<UClass*> GetPaletteClasses();

// Input pins a new node of the class gets (USoundNode::CreateStartingConnectors on a node the palette created).
int32 GetStartingInputs(UClass& Class);
int32 GetMinInputs(UClass& Class);
int32 GetMaxInputs(UClass& Class);

// Details values ({name: value}, the graph property codec) of every editable property of a new node of Class with
// Inputs child slots (per-child arrays at the editor's per-slot defaults).
FValue EncodeDefaultProperties(UClass& Class, int32 Inputs);
// The editable properties of Node that differ from a new node of its class with the same number of child slots.
FValue EncodeChangedProperties(const USoundNode& Node);

// The comment box settings ({CommentColor, FontSize, bCommentBubbleVisible_InDetailsPanel, bColorCommentBubble,
// MoveMode, NodeDetails}) of Comment that differ from a comment the schema's comment action places.
FValue EncodeChangedCommentProperties(const UEdGraphNode& Comment);
FValue EncodeDefaultCommentProperties();
// Names of the authored comment box properties.
TConstArrayView<const TCHAR*> GetCommentPropertyNames();

// The palette's (or the graph context menu's) placement of a node of NodeClass, or of a comment box when NodeClass is
// null, at Location: FSoundCueGraphSchemaAction_NewNode / the schema's comment action, PerformAction without a
// dragged pin and without selecting the new node. Null when AudioEditor's action is not available.
UEdGraphNode* PerformPaletteAction(UEdGraph& Graph, UClass* NodeClass, const FVector2f& Location);

// ---- raw editor-equivalent edits (AssetDocumentSoundCueRawEdits.cpp) --------------------------------------------

// FSoundCueEditor::DeleteSelectedNodes for one node (the editor's Delete; the editor method is private):
// transaction, graph Modify, CanUserDeleteNode, FBlueprintEditorUtils::RemoveNode, then for a sound node graph node
// CompileSoundNodesFromGraphNodes, AllNodes.Remove, MarkPackageDirty. False with OutError when the node cannot be
// deleted (the Root node).
bool DeleteNodeAsTheEditorDoes(USoundCue& Cue, UEdGraphNode& Node, FString& OutError);

// Renames a node's identity object to Name: the sound node of a sound node graph node (outer: the cue), or the comment
// node itself (outer: the graph). A stale object holding the name (deleted, not yet garbage collected) is moved aside;
// a live node holding it fails. False with OutError (Name invalid, Root node, name taken).
bool RenameNodeIdentity(USoundCue& Cue, UEdGraphNode& Node, const FString& Name, FString& OutError);

// The node's identity name (sound node or comment object name); empty for the Root node.
FString NodeIdentity(const UEdGraphNode& Node);

// ---- validation, compile status, rollback (AssetDocumentSoundCueValidation.cpp) ---------------------------------

struct FValidationMessage
{
	FString Severity;  // "error" | "warning" | "info"
	FString Message;
	const UEdGraphNode* Node = nullptr;
	FString Pin;
};

// Design §5.1: what runtime code assumes about a cue but nothing checks (errors), plus the warnings / infos the
// Sound Cue editor shows as dangling or silent parts.
void Validate(const USoundCue& Cue, TArray<FValidationMessage>& OutMessages);

// "Compile": {Status: UpToDate | UpToDateWithWarnings | Error, Messages[{Severity, Message, Node?, Path?}]}; null for
// other assets.
TSharedPtr<FJsonObject> DescribeCompileStatus(const UObject& Asset);

TSharedRef<IAssetDocumentDerivedStateRebuilder> MakeRebuilder();

// Open-editor apply (AssetDocumentSoundCueOpenEditor.cpp): the Sound Cue editor edits the cue's own graph, so an apply
// runs in place and the open editor is refreshed as its Undo does (FSoundCueEditor::PostUndo).
TSharedRef<IAssetDocumentOpenEditorHandler> MakeOpenEditorHandler();

// ---- registration -----------------------------------------------------------------------------------------------

void RegisterGraphAdapters();
void UnregisterGraphAdapters();

// list_types kind "sound_node": the palette's node classes.
bool ListNodeTypes(const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError);
} // namespace UE::AssetDocument::Graph::Audio::SoundCue
