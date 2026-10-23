// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Graphs/K2NodeAdapters/AssetDocumentK2MemberRefUtils.h"

#include "Engine/Blueprint.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

TSharedRef<FJsonObject> FAssetDocumentK2MemberRefUtils::MakeMemberRef(
	const FString& OwnerClassPath,
	FName MemberName,
	const FGuid& MemberGuid,
	TOptional<bool> SelfContext)
{
	TSharedRef<FJsonObject> Member = MakeShared<FJsonObject>();
	Member->SetStringField(TEXT("Kind"), TEXT("MemberRef"));
	Member->SetStringField(TEXT("OwnerClass"), OwnerClassPath);
	Member->SetStringField(TEXT("Name"), MemberName.IsNone() ? FString() : MemberName.ToString());
	if (MemberGuid.IsValid())
	{
		Member->SetStringField(TEXT("Guid"), MemberGuid.ToString(EGuidFormats::Digits));
	}
	if (SelfContext.IsSet())
	{
		Member->SetBoolField(TEXT("SelfContext"), SelfContext.GetValue());
	}
	return Member;
}

bool FAssetDocumentK2MemberRefUtils::TryReadMemberRef(
	const TSharedPtr<FJsonObject>& Member,
	FString& OutOwnerClassPath,
	FString& OutMemberName)
{
	if (!Member.IsValid())
	{
		return false;
	}

	FString Kind;
	return Member->TryGetStringField(TEXT("Kind"), Kind)
		&& Kind == TEXT("MemberRef")
		&& Member->TryGetStringField(TEXT("OwnerClass"), OutOwnerClassPath)
		&& !OutOwnerClassPath.IsEmpty()
		&& Member->TryGetStringField(TEXT("Name"), OutMemberName)
		&& !OutMemberName.IsEmpty();
}

UClass* FAssetDocumentK2MemberRefUtils::ResolveClass(const FString& ClassPath)
{
	return ClassPath.IsEmpty() ? nullptr : StaticLoadClass(UObject::StaticClass(), nullptr, *ClassPath);
}

UClass* FAssetDocumentK2MemberRefUtils::ResolveMemberOwnerClass(
	const UBlueprint* Blueprint,
	const FString& OwnerClassPath)
{
	if (OwnerClassPath == TEXT("Self"))
	{
		if (Blueprint && Blueprint->GeneratedClass)
		{
			return Blueprint->GeneratedClass;
		}
		return Blueprint ? Blueprint->ParentClass.Get() : nullptr;
	}
	return ResolveClass(OwnerClassPath);
}

UFunction* FAssetDocumentK2MemberRefUtils::ResolveMemberFunction(
	const UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& Member)
{
	FString OwnerClassPath;
	FString FunctionName;
	if (!TryReadMemberRef(Member, OwnerClassPath, FunctionName))
	{
		return nullptr;
	}

	UClass* OwnerClass = ResolveMemberOwnerClass(Blueprint, OwnerClassPath);
	return OwnerClass ? OwnerClass->FindFunctionByName(FName(*FunctionName)) : nullptr;
}

FProperty* FAssetDocumentK2MemberRefUtils::ResolveMemberProperty(
	const UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& Member)
{
	FString OwnerClassPath;
	FString PropertyName;
	if (!TryReadMemberRef(Member, OwnerClassPath, PropertyName))
	{
		return nullptr;
	}

	UClass* OwnerClass = ResolveMemberOwnerClass(Blueprint, OwnerClassPath);
	return OwnerClass ? FindFProperty<FProperty>(OwnerClass, FName(*PropertyName)) : nullptr;
}

FAssetDocumentCapabilityResult FAssetDocumentK2MemberRefUtils::MakeMissingMemberFailure(
	const FAssetDocumentNodeApplyContext& Context,
	const FAssetDocumentNodeSpec& Node,
	const FString& DiagnosticCode)
{
	return FAssetDocumentCapabilityResult::Failure(
		FString::Printf(TEXT("Graph node '%s' requires a reflected MemberRef"), *Node.Id),
		Context.NodePath / TEXT("Member"),
		DiagnosticCode);
}

FAssetDocumentCapabilityResult FAssetDocumentK2MemberRefUtils::MakeUnresolvedMemberFailure(
	const FAssetDocumentNodeApplyContext& Context,
	const FAssetDocumentNodeSpec& Node,
	const FString& DiagnosticCode)
{
	return FAssetDocumentCapabilityResult::Failure(
		FString::Printf(TEXT("Graph node '%s' MemberRef could not be resolved"), *Node.Id),
		Context.NodePath / TEXT("Member"),
		DiagnosticCode);
}
