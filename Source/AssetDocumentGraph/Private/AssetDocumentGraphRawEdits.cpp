// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentGraphRawEdits.h"

#include "EdGraph/EdGraphNode.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"

namespace UE::AssetDocument::Graph::RawEdits
{
void SetNodePosition(UEdGraphNode& Node, int32 X, int32 Y)
{
	Node.Modify();
	Node.NodePosX = X;
	Node.NodePosY = Y;
}

void SetNodeComment(UEdGraphNode& Node, const FString& Comment)
{
	Node.Modify();
	Node.OnUpdateCommentText(Comment);
	Node.NodeComment = Comment;
}

void SetCommentBubbleVisible(UEdGraphNode& Node, bool bVisible)
{
	Node.Modify();
#if WITH_EDITORONLY_DATA
	Node.bCommentBubbleVisible = bVisible;
	Node.bCommentBubblePinned = bVisible;
#endif
}

bool SetNodeSize(UEdGraphNode& Node, int32 Width, int32 Height)
{
#if WITH_EDITORONLY_DATA
	if (!Node.bCanResizeNode)
	{
		return false;
	}
	Node.Modify();
	Node.ResizeNode(FVector2f(static_cast<float>(Width), static_cast<float>(Height)));
	return true;
#else
	return false;
#endif
}

namespace RawEditsLocal
{
FName DocumentIdKey()
{
	return FName(TEXT("AssetDocument.NodeId"));
}

FName DocumentActionKey()
{
	return FName(TEXT("AssetDocument.NodeAction"));
}

FString GetNodeMetadata(const UEdGraphNode& Node, FName Key)
{
	UPackage* Package = Node.GetOutermost();
	const FString* Value = Package != nullptr ? Package->GetMetaData().FindValue(&Node, Key) : nullptr;
	return Value != nullptr ? *Value : FString();
}
} // namespace RawEditsLocal

bool SetNodeDocumentIdentity(UEdGraphNode& Node, const FString& Id, const FString& Action)
{
	using namespace RawEditsLocal;
	UPackage* Package = Node.GetOutermost();
	if (Package == nullptr || Package == GetTransientPackage())
	{
		return false;
	}
	FMetaData& MetaData = Package->GetMetaData();
	bool bWritten = false;
	if (GetNodeMetadata(Node, DocumentIdKey()) != Id)
	{
		MetaData.SetValue(&Node, DocumentIdKey(), *Id);
		bWritten = true;
	}
	if (GetNodeMetadata(Node, DocumentActionKey()) != Action)
	{
		if (Action.IsEmpty())
		{
			MetaData.RemoveValue(&Node, DocumentActionKey());
		}
		else
		{
			MetaData.SetValue(&Node, DocumentActionKey(), *Action);
		}
		bWritten = true;
	}
	if (bWritten)
	{
		Package->MarkPackageDirty();
	}
	return bWritten;
}

FString GetNodeDocumentId(const UEdGraphNode& Node)
{
	return RawEditsLocal::GetNodeMetadata(Node, RawEditsLocal::DocumentIdKey());
}

FString GetNodeDocumentAction(const UEdGraphNode& Node)
{
	return RawEditsLocal::GetNodeMetadata(Node, RawEditsLocal::DocumentActionKey());
}

bool ConvertGhostNodeToRealNode(UEdGraphNode& Node)
{
	if (!Node.IsAutomaticallyPlacedGhostNode())
	{
		return false;
	}
	Node.Modify();
	Node.SetEnabledState(ENodeEnabledState::Enabled, /*bUserAction=*/false);
	Node.NodeComment.Empty();
#if WITH_EDITORONLY_DATA
	Node.bCommentBubbleVisible = false;
#endif
	return true;
}
} // namespace UE::AssetDocument::Graph::RawEdits
