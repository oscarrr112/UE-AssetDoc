// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentGraphNodeSize.h"

#include "EdGraph/EdGraphNode.h"
#include "Framework/Application/SlateApplication.h"
#include "K2Node.h"
#include "NodeFactory.h"
#include "SGraphNode.h"

namespace UE::AssetDocument::Graph
{
bool MeasureGraphNodeWidget(UEdGraphNode& Node, FVector2D& OutSize)
{
	if (IsRunningCommandlet() || !FSlateApplication::IsInitialized())
	{
		return false;
	}
	// A widget is measured without the SGraphPanel that owns it in the editor. Blueprint node widgets
	// (SGraphNodeK2*) lay out without their panel; others may not (SGraphNodeAnimState reads the panel while it
	// lays out and asserts without one), and Anim Graph nodes use the animation graph factory's widgets.
	const UClass* NodeClass = Node.GetClass();
	if (!NodeClass->IsChildOf(UK2Node::StaticClass()) || NodeClass->GetOutermost()->GetName() == TEXT("/Script/AnimGraph"))
	{
		return false;
	}
	const TSharedPtr<SGraphNode> Widget = FNodeFactory::CreateNodeWidget(&Node);
	if (!Widget.IsValid())
	{
		return false;
	}
	Widget->SlatePrepass(1.0f);
	const FVector2D Size(Widget->GetDesiredSize());
	if (Size.X <= 0.0 || Size.Y <= 0.0)
	{
		return false;
	}
	OutSize = Size;
	return true;
}
} // namespace UE::AssetDocument::Graph
