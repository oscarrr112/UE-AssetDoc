// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Graphs/AssetDocumentNodeAdapter.h"

class UBlueprint;
class UClass;
class UFunction;
class FProperty;

class FAssetDocumentK2MemberRefUtils final
{
public:
	FAssetDocumentK2MemberRefUtils() = delete;

	static TSharedRef<FJsonObject> MakeMemberRef(
		const FString& OwnerClassPath,
		FName MemberName,
		const FGuid& MemberGuid,
		TOptional<bool> SelfContext = TOptional<bool>());
	static bool TryReadMemberRef(
		const TSharedPtr<FJsonObject>& Member,
		FString& OutOwnerClassPath,
		FString& OutMemberName);

	static UClass* ResolveClass(const FString& ClassPath);
	static UClass* ResolveMemberOwnerClass(const UBlueprint* Blueprint, const FString& OwnerClassPath);
	static UFunction* ResolveMemberFunction(const UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Member);
	static FProperty* ResolveMemberProperty(const UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Member);

	static FAssetDocumentCapabilityResult MakeMissingMemberFailure(
		const FAssetDocumentNodeApplyContext& Context,
		const FAssetDocumentNodeSpec& Node,
		const FString& DiagnosticCode);
	static FAssetDocumentCapabilityResult MakeUnresolvedMemberFailure(
		const FAssetDocumentNodeApplyContext& Context,
		const FAssetDocumentNodeSpec& Node,
		const FString& DiagnosticCode);
};
