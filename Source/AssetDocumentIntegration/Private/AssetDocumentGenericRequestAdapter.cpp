// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentGenericRequestAdapter.h"

#include "AssetDocumentSidecarReader.h"

#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"

namespace
{
FAssetDocumentRuntimeDiagnostic MakeAdapterDiagnostic(
	const FString& Path,
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = Path;
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	Diagnostic.Severity = EAssetDocumentDiagnosticSeverity::Error;
	return Diagnostic;
}

bool AdaptJsonValue(
	const TSharedPtr<FJsonValue>& Source,
	FAssetDocumentRuntimeValueDto& OutValue,
	const FString& Path,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);

bool IsPlainJsonIntegerLiteral(const FString& Literal)
{
	int32 Index = Literal.StartsWith(TEXT("-")) ? 1 : 0;
	if (Index >= Literal.Len() || Literal.Len() - Index > 19)
	{
		return false;
	}
	for (; Index < Literal.Len(); ++Index)
	{
		if (!FChar::IsDigit(Literal[Index]))
		{
			return false;
		}
	}
	return true;
}

bool AdaptJsonNumber(
	const TSharedPtr<FJsonValue>& Source,
	FAssetDocumentRuntimeValueDto& OutValue,
	const FString& Path)
{
	int64 Integer = 0;
	double Number = 0.0;
	FString Literal;
	if (Source->PreferStringRepresentation() && Source->TryGetString(Literal))
	{
		// The source literal decides the kind: only a plain integer literal is an
		// Integer, so "0.0" or "1e3" stays a Number exactly as Python json reads it.
		if (IsPlainJsonIntegerLiteral(Literal) && LexTryParseString(Integer, *Literal))
		{
			OutValue.Kind = EAssetDocumentRuntimeValueKind::Integer;
			OutValue.IntegerValue = Integer;
			return true;
		}
		OutValue.Kind = EAssetDocumentRuntimeValueKind::Number;
		return Source->TryGetNumber(OutValue.NumberValue) && FMath::IsFinite(OutValue.NumberValue);
	}
	if (Source->TryGetNumber(Integer)
		&& Source->TryGetNumber(Number)
		&& Path == TEXT("/SchemaVersion")
		&& Number == static_cast<double>(Integer))
	{
		OutValue.Kind = EAssetDocumentRuntimeValueKind::Integer;
		OutValue.IntegerValue = Integer;
		return true;
	}
	OutValue.Kind = EAssetDocumentRuntimeValueKind::Number;
	return Source->TryGetNumber(OutValue.NumberValue);
}

bool AdaptJsonScalar(
	const TSharedPtr<FJsonValue>& Source,
	FAssetDocumentRuntimeValueDto& OutValue,
	const FString& Path)
{
	switch (Source->Type)
	{
	case EJson::Null:
		OutValue.Kind = EAssetDocumentRuntimeValueKind::Null;
		return true;
	case EJson::Boolean:
		OutValue.Kind = EAssetDocumentRuntimeValueKind::Boolean;
		return Source->TryGetBool(OutValue.BooleanValue);
	case EJson::Number:
		return AdaptJsonNumber(Source, OutValue, Path);
	case EJson::String:
		OutValue.Kind = EAssetDocumentRuntimeValueKind::String;
		return Source->TryGetString(OutValue.StringValue);
	default:
		return false;
	}
}

bool AdaptJsonArray(
	const TSharedPtr<FJsonValue>& Source,
	FAssetDocumentRuntimeValueDto& OutValue,
	const FString& Path,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	OutValue.Kind = EAssetDocumentRuntimeValueKind::Array;
	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (!Source->TryGetArray(Array) || Array == nullptr)
	{
		return false;
	}
	OutValue.Elements.Reserve(Array->Num());
	for (int32 Index = 0; Index < Array->Num(); ++Index)
	{
		FAssetDocumentRuntimeValueDto& Element =
			OutValue.Elements.AddDefaulted_GetRef();
		if (!AdaptJsonValue(
			(*Array)[Index],
			Element,
			FString::Printf(TEXT("%s/%d"), *Path, Index),
			OutDiagnostics))
		{
			return false;
		}
	}
	return true;
}

bool AdaptJsonObjectValue(
	const TSharedPtr<FJsonValue>& Source,
	FAssetDocumentRuntimeValueDto& OutValue,
	const FString& Path,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (!Source->TryGetObject(Object)
		|| Object == nullptr
		|| !Object->IsValid())
	{
		OutDiagnostics.Add(MakeAdapterDiagnostic(
			Path,
			TEXT("GenericRuntime.Request.InvalidJsonObject"),
			TEXT("The request contains an invalid JSON object.")));
		return false;
	}
	OutValue.Kind = EAssetDocumentRuntimeValueKind::Object;
	OutValue.ObjectKeys.Reserve((*Object)->Values.Num());
	OutValue.Elements.Reserve((*Object)->Values.Num());
	for (const auto& Field :
		(*Object)->Values)
	{
		OutValue.ObjectKeys.Add(FString(*Field.Key));
		FAssetDocumentRuntimeValueDto& FieldValue =
			OutValue.Elements.AddDefaulted_GetRef();
		if (!AdaptJsonValue(
			Field.Value,
			FieldValue,
			Path / FString(*Field.Key),
			OutDiagnostics))
		{
			return false;
		}
	}
	return true;
}

bool IsScalarJsonType(EJson Type)
{
	switch (Type)
	{
	case EJson::Null:
	case EJson::Boolean:
	case EJson::Number:
	case EJson::String:
		return true;
	default:
		return false;
	}
}

bool AdaptJsonValue(
	const TSharedPtr<FJsonValue>& Source,
	FAssetDocumentRuntimeValueDto& OutValue,
	const FString& Path,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	if (!Source.IsValid())
	{
		OutDiagnostics.Add(MakeAdapterDiagnostic(
			Path,
			TEXT("GenericRuntime.Request.InvalidJsonValue"),
			TEXT("The request contains an invalid JSON value.")));
		return false;
	}
	if (IsScalarJsonType(Source->Type))
	{
		return AdaptJsonScalar(Source, OutValue, Path);
	}
	if (Source->Type == EJson::Array)
	{
		return AdaptJsonArray(Source, OutValue, Path, OutDiagnostics);
	}
	if (Source->Type == EJson::Object)
	{
		return AdaptJsonObjectValue(Source, OutValue, Path, OutDiagnostics);
	}
	OutDiagnostics.Add(MakeAdapterDiagnostic(
		Path,
		TEXT("GenericRuntime.Request.UnsupportedJsonValue"),
		TEXT("The request contains an unsupported JSON value.")));
	return false;
}

// A whole document. Its top-level BaseRevision (what Extract emits) is checked by the service before the request
// gets here (FAssetDocumentService: Document.StaleBase) and is not part of the profiles' document.
bool AdaptJsonObject(
	const TSharedPtr<FJsonObject>& Source,
	FAssetDocumentRuntimeValueDto& OutValue,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	OutValue.Kind = EAssetDocumentRuntimeValueKind::Object;
	OutValue.ObjectKeys.Reserve(Source->Values.Num());
	OutValue.Elements.Reserve(Source->Values.Num());
	for (const auto& Field : Source->Values)
	{
		if (Field.Key == AssetDocumentBaseRevisionField)
		{
			continue;
		}
		OutValue.ObjectKeys.Add(FString(*Field.Key));
		FAssetDocumentRuntimeValueDto& FieldValue =
			OutValue.Elements.AddDefaulted_GetRef();
		if (!AdaptJsonValue(
			Field.Value,
			FieldValue,
			FString(TEXT("/")) + Field.Key,
			OutDiagnostics))
		{
			return false;
		}
	}
	return true;
}

FString ReadStringField(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Name)
{
	const TSharedPtr<FJsonValue>* Field = Object->Values.Find(Name);
	FString Value;
	if (Field != nullptr && Field->IsValid())
	{
		(*Field)->TryGetString(Value);
	}
	return Value;
}

FString ProjectRuntimeObjectPath(const FString& TargetPackagePath)
{
	if (!FPackageName::IsValidLongPackageName(TargetPackagePath))
	{
		return TargetPackagePath;
	}
	return FString::Printf(
		TEXT("%s.%s"),
		*TargetPackagePath,
		*FPackageName::GetLongPackageAssetName(TargetPackagePath));
}
}

TOptional<FAssetDocumentRuntimeRequestDto>
FAssetDocumentGenericRequestAdapter::AdaptApplyFile(
	const FAssetDocumentApplyFileRequest& Request,
	uint64 RuntimeRevision,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	TSharedPtr<FJsonObject> Document;
	FString Error;
	if (Request.FilePath.IsEmpty()
		|| !FAssetDocumentSidecarReader::LoadValidatedDocument(
			Request.FilePath,
			Document,
			Error)
		|| !Document.IsValid()
		|| RuntimeRevision == 0)
	{
		OutDiagnostics.Add(MakeAdapterDiagnostic(
			TEXT("/Request"),
			TEXT("GenericRuntime.Request.InvalidApply"),
			TEXT("The generic Apply request or runtime revision is invalid.")));
		return {};
	}

	FAssetDocumentRuntimeRequestDto Adapted;
	Adapted.RequestId = FGuid::NewGuid();
	Adapted.Operation = EAssetDocumentRuntimeOperation::Apply;
	Adapted.TargetAssetPath = ProjectRuntimeObjectPath(
		ReadStringField(Document, TEXT("Target")));
	Adapted.RequestedAssetClassPath =
		ReadStringField(Document, TEXT("Class"));
	Adapted.ExpectedRuntimeRevision = RuntimeRevision;
	if (!AdaptJsonObject(
		Document,
		Adapted.CanonicalDocument,
		OutDiagnostics))
	{
		return {};
	}
	return Adapted;
}

TOptional<FAssetDocumentRuntimeRequestDto>
FAssetDocumentGenericRequestAdapter::AdaptValidate(
	const FAssetDocumentValidateRequest& Request,
	uint64 RuntimeRevision,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	if (!Request.Document.IsValid() || RuntimeRevision == 0)
	{
		OutDiagnostics.Add(MakeAdapterDiagnostic(
			TEXT("/Request"),
			TEXT("GenericRuntime.Request.InvalidValidate"),
			TEXT("The generic Validate request or runtime revision is invalid.")));
		return {};
	}

	const FString TargetPackagePath =
		ReadStringField(Request.Document, TEXT("Target"));
	const FString TargetObjectPath =
		ProjectRuntimeObjectPath(TargetPackagePath);
	if (!TargetPackagePath.StartsWith(TEXT("/Game/"))
		|| !FPackageName::IsValidLongPackageName(TargetPackagePath)
		|| !FPackageName::IsValidObjectPath(TargetObjectPath))
	{
		OutDiagnostics.Add(MakeAdapterDiagnostic(
			TEXT("/Target"),
			TEXT("GenericRuntime.Request.InvalidValidateTarget"),
			TEXT("The generic Validate target must be a canonical /Game package path.")));
		return {};
	}

	FAssetDocumentRuntimeRequestDto Adapted;
	Adapted.RequestId = FGuid::NewGuid();
	Adapted.Operation = EAssetDocumentRuntimeOperation::Validate;
	Adapted.TargetAssetPath = TargetObjectPath;
	Adapted.RequestedAssetClassPath =
		ReadStringField(Request.Document, TEXT("Class"));
	Adapted.ExpectedRuntimeRevision = RuntimeRevision;
	if (!AdaptJsonObject(
		Request.Document,
		Adapted.CanonicalDocument,
		OutDiagnostics))
	{
		return {};
	}
	return Adapted;
}

TOptional<FAssetDocumentRuntimeRequestDto>
FAssetDocumentGenericRequestAdapter::AdaptExtract(
	const FAssetDocumentExtractRequest& Request,
	const FString& AssetClassPath,
	uint64 RuntimeRevision,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	if (Request.AssetPath.IsEmpty() || AssetClassPath.IsEmpty() || RuntimeRevision == 0)
	{
		OutDiagnostics.Add(MakeAdapterDiagnostic(
			TEXT("/Request"),
			TEXT("GenericRuntime.Request.InvalidExtract"),
			TEXT("The generic Extract request or runtime revision is invalid.")));
		return {};
	}

	FAssetDocumentRuntimeRequestDto Adapted;
	Adapted.RequestId = FGuid::NewGuid();
	Adapted.Operation = EAssetDocumentRuntimeOperation::Extract;
	Adapted.TargetAssetPath = ProjectRuntimeObjectPath(Request.AssetPath);
	Adapted.RequestedAssetClassPath = AssetClassPath;
	Adapted.ExpectedRuntimeRevision = RuntimeRevision;
	Adapted.CanonicalDocument.Kind = EAssetDocumentRuntimeValueKind::Object;
	Adapted.CanonicalDocument.ObjectKeys.Add(TEXT("include_defaults"));
	FAssetDocumentRuntimeValueDto& IncludeDefaults =
		Adapted.CanonicalDocument.Elements.AddDefaulted_GetRef();
	IncludeDefaults.Kind = EAssetDocumentRuntimeValueKind::Boolean;
	// diff_only (default) lists only values that differ from the class defaults;
	// diff_only=false or include_all_writable lists every writable value.
	IncludeDefaults.BooleanValue = Request.bIncludeAllWritable || !Request.bDiffOnly;
	return Adapted;
}

TOptional<FAssetDocumentRuntimeRequestDto>
FAssetDocumentGenericRequestAdapter::AdaptDiff(
	const FAssetDocumentDiffRequest& Request,
	uint64 RuntimeRevision,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	if (!Request.Document.IsValid() || RuntimeRevision == 0)
	{
		OutDiagnostics.Add(MakeAdapterDiagnostic(
			TEXT("/Request"),
			TEXT("GenericRuntime.Request.InvalidDiff"),
			TEXT("The generic Diff request or runtime revision is invalid.")));
		return {};
	}

	FAssetDocumentRuntimeRequestDto Adapted;
	Adapted.RequestId = FGuid::NewGuid();
	Adapted.Operation = EAssetDocumentRuntimeOperation::Diff;
	Adapted.TargetAssetPath = ProjectRuntimeObjectPath(
		ReadStringField(Request.Document, TEXT("Target")));
	Adapted.RequestedAssetClassPath =
		ReadStringField(Request.Document, TEXT("Class"));
	Adapted.ExpectedRuntimeRevision = RuntimeRevision;
	if (!AdaptJsonObject(
		Request.Document,
		Adapted.CanonicalDocument,
		OutDiagnostics))
	{
		return {};
	}
	return Adapted;
}

TOptional<FAssetDocumentRuntimeRequestDto>
FAssetDocumentGenericRequestAdapter::AdaptInspectProfile(
	const FAssetDocumentProfileRequest& Request,
	const FString& AssetClassPath,
	uint64 RuntimeRevision,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	if (Request.ClassOrAsset.IsEmpty()
		|| AssetClassPath.IsEmpty()
		|| RuntimeRevision == 0)
	{
		OutDiagnostics.Add(MakeAdapterDiagnostic(
			TEXT("/Request"),
			TEXT("GenericRuntime.Request.InvalidProfile"),
			TEXT("The generic profile request or runtime revision is invalid.")));
		return {};
	}

	FAssetDocumentRuntimeRequestDto Adapted;
	Adapted.RequestId = FGuid::NewGuid();
	Adapted.Operation = EAssetDocumentRuntimeOperation::Schema;
	Adapted.TargetAssetPath = AssetClassPath;
	Adapted.RequestedAssetClassPath = AssetClassPath;
	Adapted.ExpectedRuntimeRevision = RuntimeRevision;
	Adapted.CanonicalDocument.Kind = EAssetDocumentRuntimeValueKind::Object;
	return Adapted;
}

TOptional<FAssetDocumentRuntimeRequestDto>
FAssetDocumentGenericRequestAdapter::AdaptInspect(
	const FAssetDocumentInspectRequest& Request,
	const FString& AssetClassPath,
	const FString& AssetObjectPath,
	uint64 RuntimeRevision,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	// A live target is a /Game asset or read-only engine/plugin content (the Python
	// runtime admits the latter only for this read, as it does for Extract).
	const bool bClassOnly = AssetObjectPath.IsEmpty();
	const bool bCanonicalLiveObjectPath = bClassOnly
		|| (FPackageName::IsValidObjectPath(AssetObjectPath)
			&& ProjectRuntimeObjectPath(
				FPackageName::ObjectPathToPackageName(AssetObjectPath))
				== AssetObjectPath);
	if (Request.ClassOrAsset.IsEmpty()
		|| AssetClassPath.IsEmpty()
		|| RuntimeRevision == 0
		|| !FPackageName::IsValidObjectPath(AssetClassPath)
		|| (!bClassOnly && (AssetObjectPath.StartsWith(TEXT("/Script/"))
			|| !bCanonicalLiveObjectPath)))
	{
		OutDiagnostics.Add(MakeAdapterDiagnostic(
			TEXT("/Request"),
			TEXT("GenericRuntime.Request.InvalidInspect"),
			TEXT("The generic Inspect request or runtime revision is invalid.")));
		return {};
	}

	FAssetDocumentRuntimeRequestDto Adapted;
	Adapted.RequestId = FGuid::NewGuid();
	Adapted.Operation = EAssetDocumentRuntimeOperation::Inspect;
	Adapted.TargetAssetPath = bClassOnly ? AssetClassPath : AssetObjectPath;
	Adapted.RequestedAssetClassPath = AssetClassPath;
	Adapted.ExpectedRuntimeRevision = RuntimeRevision;
	Adapted.CanonicalDocument.Kind = EAssetDocumentRuntimeValueKind::Object;
	Adapted.CanonicalDocument.ObjectKeys.Add(TEXT("mode"));
	FAssetDocumentRuntimeValueDto& Mode =
		Adapted.CanonicalDocument.Elements.AddDefaulted_GetRef();
	Mode.Kind = EAssetDocumentRuntimeValueKind::String;
	Mode.StringValue = bClassOnly ? TEXT("class_defaults") : TEXT("live_asset");
	if (Request.bSummary)
	{
		Adapted.CanonicalDocument.ObjectKeys.Add(TEXT("summary"));
		FAssetDocumentRuntimeValueDto& Summary =
			Adapted.CanonicalDocument.Elements.AddDefaulted_GetRef();
		Summary.Kind = EAssetDocumentRuntimeValueKind::Boolean;
		Summary.BooleanValue = true;
	}
	return Adapted;
}

TOptional<FAssetDocumentRuntimeRequestDto>
FAssetDocumentGenericRequestAdapter::AdaptCreateTemplate(
	const FAssetDocumentTemplateRequest& Request,
	const FString& NormalizedTarget,
	const FString& AssetClassPath,
	uint64 RuntimeRevision,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	if (Request.Class.IsEmpty()
		|| NormalizedTarget.IsEmpty()
		|| AssetClassPath.IsEmpty()
		|| RuntimeRevision == 0)
	{
		OutDiagnostics.Add(MakeAdapterDiagnostic(
			TEXT("/Request"),
			TEXT("GenericRuntime.Request.InvalidTemplate"),
			TEXT("The generic template request or runtime revision is invalid.")));
		return {};
	}

	FAssetDocumentRuntimeRequestDto Adapted;
	Adapted.RequestId = FGuid::NewGuid();
	Adapted.Operation = EAssetDocumentRuntimeOperation::Template;
	Adapted.TargetAssetPath = ProjectRuntimeObjectPath(NormalizedTarget);
	Adapted.RequestedAssetClassPath = AssetClassPath;
	Adapted.ExpectedRuntimeRevision = RuntimeRevision;
	Adapted.CanonicalDocument.Kind = EAssetDocumentRuntimeValueKind::Object;
	return Adapted;
}
