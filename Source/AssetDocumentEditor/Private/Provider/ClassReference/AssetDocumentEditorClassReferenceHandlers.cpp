// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/ClassReference/AssetDocumentEditorClassReferenceHandlers.h"

#include "Provider/AssetDocumentEditorValueDto.h"
#include "Provider/ClassReference/AssetDocumentEditorAssetPayload.h"
#include "Provider/ClassReference/AssetDocumentEditorClassPayload.h"
#include "AssetDocumentNameSuggestions.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/PackageName.h"
#include "UObject/Class.h"
#include "UObject/SoftObjectPath.h"

namespace EditorClassReferenceHandlersLocal
{
FAssetDocumentPrimitiveOutcome MakeClassReferenceFailure(
	const TCHAR* Path,
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	FAssetDocumentRuntimeDiagnostic& Diagnostic =
		Outcome.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = Path;
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Outcome;
}

bool IsCanonicalTopLevelPath(
	const FString& Path,
	const FSoftObjectPath& SoftPath)
{
	return !Path.IsEmpty()
		&& Path.TrimStartAndEnd() == Path
		&& SoftPath.IsValid()
		&& SoftPath.GetSubPathUtf8String().IsEmpty()
		&& SoftPath.ToString() == Path;
}

bool HasExactClassArgumentShape(
	const FAssetDocumentRuntimeValueDto& Arguments)
{
	if (Arguments.Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return false;
	}
	if (Arguments.ObjectKeys != TArray<FString>({
		TEXT("class"), TEXT("expected_base")}))
	{
		return false;
	}
	if (Arguments.Elements.Num() != 2)
	{
		return false;
	}
	return true;
}

bool TryReadClassArguments(
	const FAssetDocumentRuntimeValueDto& Arguments,
	const FAssetDocumentRuntimeValueDto*& OutClass,
	const FAssetDocumentRuntimeValueDto*& OutExpectedBase)
{
	if (!HasExactClassArgumentShape(Arguments))
	{
		return false;
	}
	OutClass = &Arguments.Elements[0];
	OutExpectedBase = &Arguments.Elements[1];
	if (OutClass->Kind != EAssetDocumentRuntimeValueKind::String)
	{
		return false;
	}
	return OutExpectedBase->Kind == EAssetDocumentRuntimeValueKind::String
		|| OutExpectedBase->Kind == EAssetDocumentRuntimeValueKind::Null;
}

UClass* ResolveExactClass(const FString& Path)
{
	const FSoftClassPath SoftPath(Path);
	if (!IsCanonicalTopLevelPath(Path, SoftPath))
	{
		return nullptr;
	}
	UClass* ResolvedClass = SoftPath.TryLoadClass<UObject>();
	return ResolvedClass != nullptr
		&& ResolvedClass->GetPathName() == Path
		? ResolvedClass
		: nullptr;
}

bool TryValidateExpectedBase(
	const FAssetDocumentRuntimeValueDto& ExpectedBaseValue,
	const UClass& ResolvedClass,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	if (ExpectedBaseValue.Kind == EAssetDocumentRuntimeValueKind::Null)
	{
		return true;
	}
	UClass* ExpectedBase = ResolveExactClass(ExpectedBaseValue.StringValue);
	if (ExpectedBase == nullptr)
	{
		OutFailure = MakeClassReferenceFailure(
			TEXT("/ValueArguments/expected_base"),
			TEXT("ClassReference.NonCanonicalExpectedBase"),
			TEXT("The expected base did not resolve to its exact canonical identity."));
		return false;
	}
	if (!ResolvedClass.IsChildOf(ExpectedBase))
	{
		OutFailure = MakeClassReferenceFailure(
			TEXT("/ValueArguments/class"),
			TEXT("ClassReference.ExpectedBaseMismatch"),
			TEXT("The resolved class is not a child of the expected base."));
		return false;
	}
	return true;
}

FAssetDocumentRuntimeValueDto MakeClassReferenceStringValue(
	const FString& String)
{
	FAssetDocumentRuntimeValueDto Value;
	Value.Kind = EAssetDocumentRuntimeValueKind::String;
	Value.StringValue = String;
	return Value;
}

bool HasExactAssetArgumentShape(
	const FAssetDocumentRuntimeValueDto& Arguments)
{
	if (Arguments.Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return false;
	}
	if (Arguments.ObjectKeys != TArray<FString>({
		TEXT("expected_class"), TEXT("path")}))
	{
		return false;
	}
	return Arguments.Elements.Num() == 2;
}

bool TryReadAssetArguments(
	const FAssetDocumentRuntimeValueDto& Arguments,
	const FAssetDocumentRuntimeValueDto*& OutPath,
	const FAssetDocumentRuntimeValueDto*& OutExpectedClass)
{
	if (!HasExactAssetArgumentShape(Arguments))
	{
		return false;
	}
	OutExpectedClass = &Arguments.Elements[0];
	OutPath = &Arguments.Elements[1];
	if (OutPath->Kind != EAssetDocumentRuntimeValueKind::String)
	{
		return false;
	}
	return OutExpectedClass->Kind == EAssetDocumentRuntimeValueKind::String
		|| OutExpectedClass->Kind == EAssetDocumentRuntimeValueKind::Null;
}

// " Close: <object path>, ..." for an asset path that resolved to nothing: the assets of the expected class (and its
// subclasses) whose names are close to the path's asset name, so the message itself names the fix (for example a
// MetaSound Preset.Parent that did not resolve). Empty without an expected class or a close name.
FString DescribeCloseAssets(const FString& Path, const FAssetDocumentRuntimeValueDto& ExpectedClassValue)
{
	if (ExpectedClassValue.Kind != EAssetDocumentRuntimeValueKind::String || !FPackageName::IsValidObjectPath(ExpectedClassValue.StringValue))
	{
		return FString();
	}
	FString Wanted = Path.TrimStartAndEnd();
	int32 Cut = INDEX_NONE;
	if (Wanted.FindLastChar(TEXT('/'), Cut))
	{
		Wanted.RightChopInline(Cut + 1);
	}
	if (Wanted.FindChar(TEXT('.'), Cut))
	{
		Wanted.LeftInline(Cut);
	}
	if (Wanted.IsEmpty())
	{
		return FString();
	}
	TArray<FAssetData> Assets;
	IAssetRegistry::GetChecked().GetAssetsByClass(FTopLevelAssetPath(ExpectedClassValue.StringValue), Assets, true);
	TArray<FString> Names;
	TMultiMap<FString, FString> PathsByName;
	for (const FAssetData& Asset : Assets)
	{
		const FString Name = Asset.AssetName.ToString();
		Names.AddUnique(Name);
		PathsByName.Add(Name, Asset.GetObjectPathString());
	}
	TArray<FString> Close;
	for (const FString& Name : UE::AssetDocument::SuggestCloseNames(Wanted, Names, 5))
	{
		TArray<FString> Paths;
		PathsByName.MultiFind(Name, Paths, true);
		Close.Append(Paths);
	}
	if (Close.IsEmpty())
	{
		return FString();
	}
	if (Close.Num() > 8)
	{
		Close.SetNum(8);
	}
	return FString::Printf(TEXT(" Close: %s."), *FString::Join(Close, TEXT(", ")));
}

UObject* ResolveExactAsset(const FString& Path)
{
	const FSoftObjectPath SoftPath(Path);
	if (!IsCanonicalTopLevelPath(Path, SoftPath))
	{
		return nullptr;
	}
	UObject* ResolvedAsset = SoftPath.TryLoad();
	return ResolvedAsset != nullptr
		&& ResolvedAsset->GetPathName() == Path
		? ResolvedAsset
		: nullptr;
}

bool TryValidateExpectedAssetClass(
	const FAssetDocumentRuntimeValueDto& ExpectedClassValue,
	const UObject& ResolvedAsset,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	if (ExpectedClassValue.Kind == EAssetDocumentRuntimeValueKind::Null)
	{
		return true;
	}
	UClass* ExpectedClass = ResolveExactClass(ExpectedClassValue.StringValue);
	if (ExpectedClass == nullptr)
	{
		OutFailure = MakeClassReferenceFailure(
			TEXT("/ValueArguments/expected_class"),
			TEXT("ClassReference.NonCanonicalExpectedClass"),
			TEXT("The expected class did not resolve to its exact canonical identity."));
		return false;
	}
	if (!ResolvedAsset.IsA(ExpectedClass))
	{
		OutFailure = MakeClassReferenceFailure(
			TEXT("/ValueArguments/path"),
			TEXT("ClassReference.AssetTypeMismatch"),
			TEXT("The resolved asset is not an instance of the expected class."));
		return false;
	}
	return true;
}

FAssetDocumentPrimitiveOutcome MakeClassResolveSuccess(
	UClass& ResolvedClass,
	IAssetDocumentHandleResolver& Handles)
{
	FAssetDocumentIssuedHandleResult Issued = Handles.Issue(
		EAssetDocumentHandleKind::Class,
		EAssetDocumentHandleAccess::ReadOnly,
		MakeAssetDocumentEditorClassPayload(ResolvedClass));
	if (!Issued.Handle.IsSet() || !Issued.Diagnostics.IsEmpty())
	{
		FAssetDocumentPrimitiveOutcome Outcome =
			MakeClassReferenceFailure(
				TEXT("/Handle"),
				TEXT("ClassReference.HandleIssueFailed"),
				TEXT("The canonical class handle could not be issued."));
		if (!Issued.Diagnostics.IsEmpty())
		{
			Outcome.Diagnostics = MoveTemp(Issued.Diagnostics);
		}
		return Outcome;
	}

	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	FString DiagnosticCode;
	const TArray<FAssetDocumentEditorValueField> Fields = {
		{TEXT("canonical_class"),
			MakeClassReferenceStringValue(ResolvedClass.GetPathName())},
	};
	if (!TryBuildAssetDocumentEditorValueObject(
		Fields, Outcome.Value, DiagnosticCode))
	{
		return MakeClassReferenceFailure(
			TEXT("/Value"),
			TEXT("ClassReference.OutputShapeFailed"),
			TEXT("The canonical class result could not be encoded."));
	}
	FAssetDocumentPrimitiveHandleResult& HandleResult =
		Outcome.HandleResults.AddDefaulted_GetRef();
	HandleResult.Name = TEXT("class");
	HandleResult.Handles.Add(Issued.Handle.GetValue());
	return Outcome;
}

FAssetDocumentPrimitiveOutcome MakeAssetResolveSuccess(
	UObject& ResolvedAsset,
	IAssetDocumentHandleResolver& Handles)
{
	FAssetDocumentIssuedHandleResult Issued = Handles.Issue(
		EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::ReadOnly,
		MakeAssetDocumentEditorAssetPayload(ResolvedAsset));
	if (!Issued.Handle.IsSet() || !Issued.Diagnostics.IsEmpty())
	{
		FAssetDocumentPrimitiveOutcome Outcome =
			MakeClassReferenceFailure(
				TEXT("/Handle"),
				TEXT("ClassReference.AssetHandleIssueFailed"),
				TEXT("The canonical asset handle could not be issued."));
		if (!Issued.Diagnostics.IsEmpty())
		{
			Outcome.Diagnostics = MoveTemp(Issued.Diagnostics);
		}
		return Outcome;
	}

	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	FString DiagnosticCode;
	const TArray<FAssetDocumentEditorValueField> Fields = {
		{TEXT("canonical_path"),
			MakeClassReferenceStringValue(ResolvedAsset.GetPathName())},
		{TEXT("canonical_class"),
			MakeClassReferenceStringValue(
				ResolvedAsset.GetClass()->GetPathName())},
	};
	if (!TryBuildAssetDocumentEditorValueObject(
		Fields, Outcome.Value, DiagnosticCode))
	{
		return MakeClassReferenceFailure(
			TEXT("/Value"),
			TEXT("ClassReference.AssetOutputShapeFailed"),
			TEXT("The canonical asset result could not be encoded."));
	}
	FAssetDocumentPrimitiveHandleResult& HandleResult =
		Outcome.HandleResults.AddDefaulted_GetRef();
	HandleResult.Name = TEXT("asset");
	HandleResult.Handles.Add(Issued.Handle.GetValue());
	return Outcome;
}
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorClassResolveCanonical(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace EditorClassReferenceHandlersLocal;
	(void)ActiveTransaction;
	const FAssetDocumentRuntimeValueDto* ClassValue = nullptr;
	const FAssetDocumentRuntimeValueDto* ExpectedBaseValue = nullptr;
	if (!TryReadClassArguments(
		Invocation.ValueArguments, ClassValue, ExpectedBaseValue))
	{
		return MakeClassReferenceFailure(
			TEXT("/ValueArguments"),
			TEXT("ClassReference.InvalidValueShape"),
			TEXT("Class resolution requires exact class and expected_base fields."));
	}
	UClass* ResolvedClass = ResolveExactClass(ClassValue->StringValue);
	if (ResolvedClass == nullptr)
	{
		return MakeClassReferenceFailure(
			TEXT("/ValueArguments/class"),
			TEXT("ClassReference.NonCanonicalClass"),
			TEXT("The class path did not resolve to that exact canonical identity."));
	}
	FAssetDocumentPrimitiveOutcome ExpectedBaseFailure;
	if (!TryValidateExpectedBase(
		*ExpectedBaseValue, *ResolvedClass, ExpectedBaseFailure))
	{
		return ExpectedBaseFailure;
	}
	return MakeClassResolveSuccess(*ResolvedClass, Handles);
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorReferenceResolveAsset(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace EditorClassReferenceHandlersLocal;
	(void)ActiveTransaction;
	const FAssetDocumentRuntimeValueDto* PathValue = nullptr;
	const FAssetDocumentRuntimeValueDto* ExpectedClassValue = nullptr;
	if (!TryReadAssetArguments(
		Invocation.ValueArguments, PathValue, ExpectedClassValue))
	{
		return MakeClassReferenceFailure(
			TEXT("/ValueArguments"),
			TEXT("ClassReference.InvalidAssetValueShape"),
			TEXT("Asset resolution requires exact path and expected_class fields."));
	}
	UObject* ResolvedAsset = ResolveExactAsset(PathValue->StringValue);
	if (ResolvedAsset == nullptr)
	{
		const FString Message = FString(TEXT("The asset path did not resolve to that exact canonical identity (an object path, "
			"/Game/Folder/Name.Name).")) + DescribeCloseAssets(PathValue->StringValue, *ExpectedClassValue);
		return MakeClassReferenceFailure(
			TEXT("/ValueArguments/path"),
			TEXT("ClassReference.NonCanonicalAsset"),
			*Message);
	}
	FAssetDocumentPrimitiveOutcome ExpectedClassFailure;
	if (!TryValidateExpectedAssetClass(
		*ExpectedClassValue, *ResolvedAsset, ExpectedClassFailure))
	{
		return ExpectedClassFailure;
	}
	return MakeAssetResolveSuccess(*ResolvedAsset, Handles);
}

TConstArrayView<FAssetDocumentEditorPrimitiveBinding>
GetAssetDocumentEditorClassReferencePrimitiveBindings()
{
	static const FAssetDocumentEditorPrimitiveBinding Bindings[] = {
		{TEXT("editor.class.resolve_canonical.v1"), 1u,
			&HandleAssetDocumentEditorClassResolveCanonical},
		{TEXT("editor.reference.resolve_asset.v1"), 1u,
			&HandleAssetDocumentEditorReferenceResolveAsset},
	};
	return MakeArrayView(Bindings);
}
