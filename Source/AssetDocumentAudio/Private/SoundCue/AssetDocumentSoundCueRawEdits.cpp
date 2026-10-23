// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Raw editor-equivalent edits of the SoundCue profile (raw paths only where the editor does the same, in their own
// file, each with a behaviour test):
//
// DeleteNodeAsTheEditorDoes   deletes one node with the same result as the Sound Cue editor's Delete command.
//                             Behaviour test: UEAssetDoc.SoundCue.EditorDelete deletes the same node
//                             with the editor's own command on an open Sound Cue editor and compares graph, runtime
//                             tree and AllNodes.
// RenameNodeIdentity          renames the sound node object (or the comment node) to the document key, as the Material
//                             profile names its expressions. The editor never renames these objects; object names are
//                             only identity. Behaviour test: UEAssetDoc.SoundCue.RenamedNodes (a renamed
//                             node saves, reloads, duplicates and still plays its wave / parses).

#include "AssetDocumentSoundCue.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphNode_Comment.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "SoundCueGraph/SoundCueGraphNode.h"
#include "SoundCueGraph/SoundCueGraphNode_Root.h"
#include "UObject/UObjectGlobals.h"

namespace UE::AssetDocument::Graph::Audio::SoundCue
{
namespace
{
bool IsValidIdentity(const FString& Name, FString& OutError)
{
	FText Reason;
	if (Name.IsEmpty() || Name.Len() >= NAME_SIZE || Name.Equals(TEXT("None"), ESearchCase::IgnoreCase) || Name.Contains(TEXT(" "))
		|| !FName::IsValidXName(Name, INVALID_OBJECTNAME_CHARACTERS, &Reason))
	{
		OutError = FString::Printf(TEXT("'%s' is not a valid key: a key is an object name (letters, digits, '_', '-'; no spaces, "
			"'.', '/', ':' or quotes)."), *Name);
		return false;
	}
	return true;
}

// The identity object of a graph node: its sound node, or the comment node itself.
UObject* IdentityObject(UEdGraphNode& Node)
{
	if (USoundCueGraphNode* SoundGraphNode = Cast<USoundCueGraphNode>(&Node))
	{
		return SoundGraphNode->SoundNode;
	}
	if (Node.IsA<UEdGraphNode_Comment>())
	{
		return &Node;
	}
	return nullptr;
}

// Whether Object is part of the cue's live graph (a sound node of a graph node, or a node of the graph).
bool IsLive(const USoundCue& Cue, const UObject& Object)
{
	const UEdGraph* Graph = Cue.SoundCueGraph;
	if (Graph == nullptr)
	{
		return false;
	}
	for (const UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node == &Object)
		{
			return true;
		}
		if (const USoundCueGraphNode* SoundGraphNode = Cast<USoundCueGraphNode>(Node); SoundGraphNode != nullptr && SoundGraphNode->SoundNode == &Object)
		{
			return true;
		}
	}
	return false;
}
} // namespace

FString NodeIdentity(const UEdGraphNode& Node)
{
	const UObject* Identity = IdentityObject(const_cast<UEdGraphNode&>(Node));
	return Identity != nullptr ? Identity->GetName() : FString();
}

bool DeleteNodeAsTheEditorDoes(USoundCue& Cue, UEdGraphNode& Node, FString& OutError)
{
	// The editor offers Delete for any node the user may delete, except the Output node.
	if (!Node.CanUserDeleteNode() || Node.IsA<USoundCueGraphNode_Root>())
	{
		OutError = FString::Printf(TEXT("%s cannot be deleted (the Sound Cue editor keeps its Output node)."), *Node.GetName());
		return false;
	}
	UEdGraph* Graph = Node.GetGraph();
	if (Graph == nullptr || Graph != Cue.SoundCueGraph)
	{
		OutError = FString::Printf(TEXT("%s is not a node of %s's graph."), *Node.GetName(), *Cue.GetName());
		return false;
	}
	const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentSoundCue", "DeleteSelectedNode", "Delete Selected Sound Cue Node"));
	Graph->Modify();
	// A sound node backs every graph node except comments: once the graph node is gone, the runtime tree is rebuilt
	// from the graph and the orphaned sound node leaves the cue's node list.
	USoundCueGraphNode* const CueGraphNode = Cast<USoundCueGraphNode>(&Node);
	USoundNode* const Orphan = CueGraphNode != nullptr ? CueGraphNode->SoundNode.Get() : nullptr;
	constexpr bool bSkipBlueprintRecompile = true;
	FBlueprintEditorUtils::RemoveNode(nullptr, &Node, bSkipBlueprintRecompile);
	if (CueGraphNode == nullptr)
	{
		return true;
	}
	Cue.CompileSoundNodesFromGraphNodes();
	Cue.AllNodes.Remove(Orphan);
	Cue.MarkPackageDirty();
	return true;
}

bool RenameNodeIdentity(USoundCue& Cue, UEdGraphNode& Node, const FString& Name, FString& OutError)
{
	UObject* Identity = IdentityObject(Node);
	if (Identity == nullptr)
	{
		OutError = FString::Printf(TEXT("%s has no key (the Output node is Body.Root)."), *Node.GetName());
		return false;
	}
	if (Identity->GetName() == Name)
	{
		return true;
	}
	if (!IsValidIdentity(Name, OutError))
	{
		return false;
	}
	UObject* Outer = Identity->GetOuter();
	if (UObject* Blocker = StaticFindObjectFast(UObject::StaticClass(), Outer, FName(*Name)))
	{
		if (IsLive(Cue, *Blocker) || Blocker == Cue.SoundCueGraph)
		{
			OutError = FString::Printf(TEXT("The key '%s' is already used by another node of the cue."), *Name);
			return false;
		}
		// A node deleted earlier (not garbage collected yet) still holds the name: move it aside, as the Material
		// profile does with deleted expressions.
		const FName Aside = MakeUniqueObjectName(Outer, Blocker->GetClass(), FName(*(TEXT("Deleted_") + Name)));
		Blocker->Rename(*Aside.ToString(), nullptr, REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
	}
	if (!Identity->Rename(*Name, nullptr, REN_Test | REN_DontCreateRedirectors))
	{
		OutError = FString::Printf(TEXT("%s cannot be renamed to '%s'."), *Identity->GetName(), *Name);
		return false;
	}
	Identity->Rename(*Name, nullptr, REN_DontCreateRedirectors | REN_DoNotDirty);
	Cue.MarkPackageDirty();
	return true;
}
} // namespace UE::AssetDocument::Graph::Audio::SoundCue
