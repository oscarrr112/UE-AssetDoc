// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Lifecycle/AssetDocumentEditorLifecycleHandlers.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Provider/AssetDocumentEditorValueDto.h"
#include "Provider/Lifecycle/AssetDocumentEditorLifecyclePayloads.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "UObject/Object.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

namespace EditorResolveExistingHandlerLocal
{
struct FResolveArguments
{
	FString Path;
	TOptional<FString> ExpectedClass;
};

struct FResolvedTarget
{
	bool bRegistryEntryExists = false;
	UObject* Asset = nullptr;
};

FAssetDocumentPrimitiveOutcome MakeResolveFailure(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	FAssetDocumentRuntimeDiagnostic& Diagnostic =
		Outcome.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/Primitive");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Outcome;
}

bool IsCanonicalAssetPath(const FString& Path)
{
	const FSoftObjectPath ObjectPath(Path);
	return ObjectPath.IsValid()
		&& ObjectPath.IsAsset()
		&& ObjectPath.ToString() == Path;
}

bool IsCanonicalClassPath(const FString& Path)
{
	const FSoftClassPath ClassPath(Path);
	return ClassPath.IsValid()
		&& ClassPath.ToString() == Path;
}

bool TryReadResolveArguments(
	const FAssetDocumentRuntimeValueDto& Value,
	FResolveArguments& OutArguments)
{
	const FAssetDocumentEditorValueFieldSpec StringFields[] = {
		{TEXT("path"), EAssetDocumentRuntimeValueKind::String},
		{TEXT("expected_class"), EAssetDocumentRuntimeValueKind::String},
	};
	TArray<const FAssetDocumentRuntimeValueDto*> Values;
	FString DiagnosticCode;
	if (TryReadAssetDocumentEditorExactObject(
		Value, StringFields, Values, DiagnosticCode))
	{
		OutArguments.Path = Values[0]->StringValue;
		OutArguments.ExpectedClass = Values[1]->StringValue;
		return IsCanonicalAssetPath(OutArguments.Path)
			&& IsCanonicalClassPath(OutArguments.ExpectedClass.GetValue());
	}
	const FAssetDocumentEditorValueFieldSpec NullFields[] = {
		{TEXT("path"), EAssetDocumentRuntimeValueKind::String},
		{TEXT("expected_class"), EAssetDocumentRuntimeValueKind::Null},
	};
	if (!TryReadAssetDocumentEditorExactObject(
		Value, NullFields, Values, DiagnosticCode))
	{
		return false;
	}
	OutArguments.Path = Values[0]->StringValue;
	OutArguments.ExpectedClass.Reset();
	return IsCanonicalAssetPath(OutArguments.Path);
}

FAssetDocumentRuntimeValueDto MakeResolveStringValue(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto MakeResolveNullValue()
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Null;
	return Result;
}

FAssetDocumentRuntimeValueDto MakeResolveBoolValue(bool bValue)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Boolean;
	Result.BooleanValue = bValue;
	return Result;
}

bool TryBuildMissingValue(FAssetDocumentRuntimeValueDto& OutValue)
{
	const FAssetDocumentEditorValueField Fields[] = {
		{TEXT("exists"), MakeResolveBoolValue(false)},
		{TEXT("canonical_path"), MakeResolveNullValue()},
		{TEXT("canonical_class"), MakeResolveNullValue()},
		{TEXT("package_name"), MakeResolveNullValue()},
		{TEXT("package_dirty"), MakeResolveBoolValue(false)},
	};
	FString DiagnosticCode;
	return TryBuildAssetDocumentEditorValueObject(
		Fields, OutValue, DiagnosticCode);
}

bool TryBuildExistingValue(
	UObject& Asset,
	FAssetDocumentRuntimeValueDto& OutValue)
{
	UPackage* Package = Asset.GetOutermost();
	const FAssetDocumentEditorValueField Fields[] = {
		{TEXT("exists"), MakeResolveBoolValue(true)},
		{TEXT("canonical_path"), MakeResolveStringValue(Asset.GetPathName())},
		{TEXT("canonical_class"),
			MakeResolveStringValue(Asset.GetClass()->GetPathName())},
		{TEXT("package_name"), MakeResolveStringValue(Package->GetName())},
		{TEXT("package_dirty"), MakeResolveBoolValue(Package->IsDirty())},
	};
	FString DiagnosticCode;
	return TryBuildAssetDocumentEditorValueObject(
		Fields, OutValue, DiagnosticCode);
}

void AddEmptyResolveGroups(FAssetDocumentPrimitiveOutcome& Outcome)
{
	for (const TCHAR* Name : {TEXT("asset_read"), TEXT("asset_mutable"),
		TEXT("package")})
	{
		Outcome.HandleResults.AddDefaulted_GetRef().Name = Name;
	}
}

void AddResolveHandle(
	FAssetDocumentPrimitiveOutcome& Outcome,
	const TCHAR* Name,
	FAssetDocumentIssuedHandleResult& Issued)
{
	FAssetDocumentPrimitiveHandleResult& Result =
		Outcome.HandleResults.AddDefaulted_GetRef();
	Result.Name = Name;
	Result.Handles.Add(MoveTemp(Issued.Handle.GetValue()));
}

bool IsIssued(const FAssetDocumentIssuedHandleResult& Result)
{
	return Result.Handle.IsSet() && Result.Diagnostics.IsEmpty();
}

FAssetDocumentPrimitiveOutcome MakeMissingSuccess()
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	if (!TryBuildMissingValue(Outcome.Value))
	{
		return MakeResolveFailure(
			TEXT("ResolveExisting.InvalidOutcome"),
			TEXT("Missing-target evidence could not be projected."));
	}
	AddEmptyResolveGroups(Outcome);
	return Outcome;
}

FAssetDocumentPrimitiveOutcome MakeExistingSuccess(
	UObject& Asset,
	IAssetDocumentHandleResolver& Handles)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	if (!TryBuildExistingValue(Asset, Outcome.Value))
	{
		return MakeResolveFailure(
			TEXT("ResolveExisting.InvalidOutcome"),
			TEXT("Existing-target evidence could not be projected."));
	}
	FAssetDocumentIssuedHandleResult Read = Handles.Issue(
		EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::ReadOnly,
		MakeAssetDocumentEditorResolvedAssetPayload(Asset));
	FAssetDocumentIssuedHandleResult Mutable = Handles.Issue(
		EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::Mutable,
		MakeAssetDocumentEditorResolvedAssetPayload(Asset));
	FAssetDocumentIssuedHandleResult Package = Handles.Issue(
		EAssetDocumentHandleKind::Package,
		EAssetDocumentHandleAccess::ReadOnly,
		MakeAssetDocumentEditorResolvedPackagePayload(Asset));
	if (!IsIssued(Read) || !IsIssued(Mutable) || !IsIssued(Package))
	{
		return MakeResolveFailure(
			TEXT("ResolveExisting.HandleIssueFailed"),
			TEXT("The request context rejected a resolved identity."));
	}
	AddResolveHandle(Outcome, TEXT("asset_read"), Read);
	AddResolveHandle(Outcome, TEXT("asset_mutable"), Mutable);
	AddResolveHandle(Outcome, TEXT("package"), Package);
	return Outcome;
}

FResolvedTarget ResolveExactAsset(const FResolveArguments& Arguments)
{
	FResolvedTarget Result;
	const FAssetData AssetData = FAssetRegistryModule::GetRegistry()
		.GetAssetByObjectPath(FSoftObjectPath(Arguments.Path));
	if (!AssetData.IsValid())
	{
		return Result;
	}
	Result.bRegistryEntryExists = true;
	UObject* Asset = AssetData.GetAsset();
	if (Asset != nullptr && Asset->GetPathName() == Arguments.Path)
	{
		Result.Asset = Asset;
	}
	return Result;
}

bool DoesExpectedClassMatch(
	const FResolveArguments& Arguments,
	const UObject& Asset)
{
	return !Arguments.ExpectedClass.IsSet()
		|| Arguments.ExpectedClass.GetValue()
			== Asset.GetClass()->GetPathName();
}
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorResolveExistingAsset(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace EditorResolveExistingHandlerLocal;
	(void)ActiveTransaction;
	FResolveArguments Arguments;
	if (!TryReadResolveArguments(Invocation.ValueArguments, Arguments))
	{
		return MakeResolveFailure(
			TEXT("ResolveExisting.InvalidArguments"),
			TEXT("Resolve-existing requires one canonical target path."));
	}
	const FResolvedTarget Target = ResolveExactAsset(Arguments);
	if (!Target.bRegistryEntryExists)
	{
		return MakeMissingSuccess();
	}
	if (Target.Asset == nullptr)
	{
		return MakeResolveFailure(
			TEXT("ResolveExisting.AssetUnavailable"),
			TEXT("The registry target could not be resolved exactly."));
	}
	if (!DoesExpectedClassMatch(Arguments, *Target.Asset))
	{
		return MakeResolveFailure(
			TEXT("ResolveExisting.ExpectedClassMismatch"),
			TEXT("The target does not have the exact expected class."));
	}
	return MakeExistingSuccess(*Target.Asset, Handles);
}
