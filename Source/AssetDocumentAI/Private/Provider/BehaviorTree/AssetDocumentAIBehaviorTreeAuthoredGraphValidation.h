// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredReadbackInternal.h"
class UAIGraphNode;
class UBehaviorTree;
class UBehaviorTreeGraph;
namespace UE::AssetDocumentAI::BehaviorTreeAuthoredReadback
{
// A node whose class is missing: no instance, and its class data names a class that no loaded class answers to. The
// editor keeps such a node (shown with "class missing") and leaves it, with the branch below it, out of the runtime tree.
bool IsMissingClassNode(const UAIGraphNode& Node);
// The class such a node was made with: "<package>.<class name>", or the class name when its data has no package.
FString MissingNodeClassPath(const UAIGraphNode& Node);

enum class EAuthoredGraphValidationFault : uint8
{
	None,
	MultipleRoots,
	Unreachable,
	Cycle,
	InvalidConnection,
};

struct FAuthoredGraphValidationFailure
{
	EAuthoredGraphValidationFault Fault = EAuthoredGraphValidationFault::None;
	FString Path;
};

// Validates the complete authored inventory before any recursive DTO construction.
// The digest is invocation-local identity evidence, never a transport value.
bool CaptureGraphIdentity(const UBehaviorTree& Tree, FString& OutDigest, FString& OutDiagnostic,
	EAuthoredGraphReadbackVersion Version = EAuthoredGraphReadbackVersion::V1,
	FAuthoredGraphValidationFailure* OutFailure = nullptr);
bool CaptureGraphIdentity(const UBehaviorTree& ScratchTree, const UBehaviorTreeGraph& ScratchGraph,
	FString& OutDigest, FString& OutDiagnostic,
	EAuthoredGraphReadbackVersion Version = EAuthoredGraphReadbackVersion::V1);
}
