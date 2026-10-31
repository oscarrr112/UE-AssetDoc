// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Anim node functions: the node Details panel's Functions section (On Initial Update, On Become Relevant,
// On Update), UAnimGraphNode_Base::InitialUpdateFunction / BecomeRelevantFunction / UpdateFunction.
// Settings "Functions.OnInitialUpdate", "Functions.OnBecomeRelevant", "Functions.OnUpdate": the bound function, as
// its name for a function of the Animation Blueprint (or a parent class) and "<class path>:<function>" for a function
// library's; "" unbinds. Written as the function picker does (FMemberReference::SetFromField, then the property's
// PostEditChangeProperty); a function of the Blueprint that is not compiled yet binds by its graph.

#include "CoreMinimal.h"
#include "AssetDocumentGraphAdapters.h"

class UAnimGraphNode_Base;

namespace UE::AssetDocument::AnimBlueprint::NodeFunctions
{
// Adds the bound functions of Node to Settings ("Functions.<Name>": text).
void QueryNodeFunctions(const UAnimGraphNode_Base& Node, FAssetDocumentRuntimeValueDto& Settings);

// True when Path names a node function setting.
bool IsNodeFunctionSetting(FStringView Path);

bool SetNodeFunction(UAnimGraphNode_Base& Node, FStringView Path, const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentGraphError& OutError);
} // namespace UE::AssetDocument::AnimBlueprint::NodeFunctions
