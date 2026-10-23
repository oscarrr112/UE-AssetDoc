// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentTypes.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
// A Python float stays a JSON float in text: whole values keep a fraction
// ("0.0"), so an extracted document re-read from text keeps its number kind.
// The literal is the shortest decimal that reads back as the same double
// (0.3, not 0.29999999999999999), as Python's repr writes it.
// Tiny values such as an imported bone's -3.4315e-17 are printed as stored, not rounded to 0: the
// editor has no rule that stores them rounded (the Details panel only displays fewer digits), and a
// rounded extract would differ from the asset (0 is another float32), so applying it unchanged
// would no longer be a no-op. Such values are cut where a profile omits what equals its source
// (e.g. imported transforms), not here.
TSharedPtr<FJsonValue> ProjectRuntimeNumber(double Number)
{
	if (!FMath::IsFinite(Number))
	{
		return MakeShared<FJsonValueNumber>(Number);
	}
	FString Literal;
	for (int32 Digits = 1; Digits <= 17; ++Digits)
	{
		Literal = FString::Printf(TEXT("%.*g"), Digits, Number);
		if (FCString::Atod(*Literal) == Number)
		{
			break;
		}
	}
	int32 Unused = INDEX_NONE;
	// %g switches to an exponent once the exponent reaches the digit count ("2e+01" for
	// 20.0); Python's repr writes positional notation up to 1e16 ("20.0").
	int32 ExponentAt = INDEX_NONE;
	if (Literal.FindChar(TEXT('e'), ExponentAt))
	{
		const int32 Exponent = FCString::Atoi(*Literal.Mid(ExponentAt + 1));
		if (Exponent >= 0 && Exponent < 16)
		{
			int32 Digits = 0;
			for (int32 Index = 0; Index < ExponentAt; ++Index)
			{
				Digits += FChar::IsDigit(Literal[Index]) ? 1 : 0;
			}
			Literal = FString::Printf(TEXT("%.*f"), FMath::Max(Digits - 1 - Exponent, 0), Number);
		}
	}
	if (!Literal.FindChar(TEXT('.'), Unused) && !Literal.FindChar(TEXT('e'), Unused)
		&& !Literal.FindChar(TEXT('E'), Unused))
	{
		Literal += TEXT(".0");
	}
	return MakeShared<FJsonValueNumberString>(MoveTemp(Literal));
}
}

TSharedPtr<FJsonValue> ProjectAssetDocumentRuntimeValue(const FAssetDocumentRuntimeValueDto& Value)
{
	switch (Value.Kind)
	{
	case EAssetDocumentRuntimeValueKind::Null: return MakeShared<FJsonValueNull>();
	case EAssetDocumentRuntimeValueKind::Boolean: return MakeShared<FJsonValueBoolean>(Value.BooleanValue);
	case EAssetDocumentRuntimeValueKind::Integer: return MakeShared<FJsonValueNumber>(Value.IntegerValue);
	case EAssetDocumentRuntimeValueKind::Number: return ProjectRuntimeNumber(Value.NumberValue);
	case EAssetDocumentRuntimeValueKind::String: return MakeShared<FJsonValueString>(Value.StringValue);
	case EAssetDocumentRuntimeValueKind::Array:
	{
		TArray<TSharedPtr<FJsonValue>> Elements;
		Elements.Reserve(Value.Elements.Num());
		for (const FAssetDocumentRuntimeValueDto& Element : Value.Elements)
		{
			Elements.Add(ProjectAssetDocumentRuntimeValue(Element));
		}
		return MakeShared<FJsonValueArray>(MoveTemp(Elements));
	}
	case EAssetDocumentRuntimeValueKind::Object:
	{
		if (Value.ObjectKeys.Num() != Value.Elements.Num()) return nullptr;
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		for (int32 Index = 0; Index < Value.ObjectKeys.Num(); ++Index)
		{
			Object->SetField(Value.ObjectKeys[Index], ProjectAssetDocumentRuntimeValue(Value.Elements[Index]));
		}
		return MakeShared<FJsonValueObject>(Object);
	}
	default: return nullptr;
	}
}

FAssetDocumentDiagnostic ProjectAssetDocumentRuntimeDiagnostic(const FAssetDocumentRuntimeDiagnostic& Source)
{
	FAssetDocumentDiagnostic Diagnostic;
	Diagnostic.Path = Source.Path;
	Diagnostic.Code = Source.Code;
	Diagnostic.Message = Source.Message;
	switch (Source.Severity)
	{
	case EAssetDocumentDiagnosticSeverity::Info: Diagnostic.Severity = TEXT("info"); break;
	case EAssetDocumentDiagnosticSeverity::Warning: Diagnostic.Severity = TEXT("warning"); break;
	default: Diagnostic.Severity = TEXT("error"); break;
	}
	for (const FAssetDocumentRuntimeDiagnosticContext& Entry : Source.Context)
	{
		const TSharedPtr<FJsonValue> Value = ProjectAssetDocumentRuntimeValue(Entry.Value);
		if (Entry.Key.IsEmpty() || !Value.IsValid())
		{
			continue;
		}
		if (!Diagnostic.Details.IsValid())
		{
			Diagnostic.Details = MakeShared<FJsonObject>();
		}
		Diagnostic.Details->SetField(Entry.Key, Value);
	}
	return Diagnostic;
}

FAssetDocumentResult FAssetDocumentResult::Success(const FString& InMessage)
{
	FAssetDocumentResult Result;
	Result.Status = EAssetDocumentResultStatus::Success;
	Result.Message = InMessage;
	return Result;
}

FAssetDocumentResult FAssetDocumentResult::Failure(const FString& InMessage)
{
	FAssetDocumentResult Result;
	Result.Status = EAssetDocumentResultStatus::Failed;
	Result.Message = InMessage;
	return Result;
}

namespace
{
// Diagnostics that only report that a later stage saw the failure of an earlier one: the
// coordinator's invalid durable receipt, persistence's canonical mismatch after the fresh
// verifier said why, the atomic framework's rollback bookkeeping, and the route wrappers
// around a concrete adaptation/dispatch failure. They stay in the list, after the cause.
bool IsConsequenceDiagnostic(const FString& Code)
{
	static const TCHAR* const Prefixes[] = {
		TEXT("RequestCoordinator."),
		TEXT("Atomic."),
	};
	for (const TCHAR* Prefix : Prefixes)
	{
		if (Code.StartsWith(Prefix, ESearchCase::CaseSensitive))
		{
			return true;
		}
	}
	static const TCHAR* const Codes[] = {
		TEXT("Persistence.CanonicalMismatch"),
		TEXT("Persistence.FreshLoadFailed"),
		TEXT("Persistence.FreshVerificationFailed"),
		TEXT("Persistence.Persist"),
		TEXT("GenericRuntime.RequestAdaptationFailed"),
		TEXT("GenericRuntime.MutationPreparationFailed"),
		TEXT("GenericRuntime.AnimationRequestBindingFailed"),
		TEXT("GenericRuntime.ParentRequestBindingFailed"),
		TEXT("GenericRuntime.ContextCreationFailed"),
		TEXT("GenericRuntime.DispatchFailed"),
	};
	for (const TCHAR* Candidate : Codes)
	{
		if (Code.Equals(Candidate, ESearchCase::CaseSensitive))
		{
			return true;
		}
	}
	return false;
}

bool IsErrorDiagnostic(const FAssetDocumentDiagnostic& Diagnostic)
{
	return Diagnostic.Severity.IsEmpty() || Diagnostic.Severity == TEXT("error");
}

int32 RootCauseRank(const FAssetDocumentDiagnostic& Diagnostic)
{
	if (!IsErrorDiagnostic(Diagnostic))
	{
		return 2;
	}
	return IsConsequenceDiagnostic(Diagnostic.Code) ? 1 : 0;
}
}

TArray<FAssetDocumentDiagnostic> FAssetDocumentResult::OrderRootCauseFirst(
	const TArray<FAssetDocumentDiagnostic>& InDiagnostics)
{
	TArray<FAssetDocumentDiagnostic> Ordered;
	Ordered.Reserve(InDiagnostics.Num());
	for (int32 Rank = 0; Rank <= 2; ++Rank)
	{
		for (const FAssetDocumentDiagnostic& Diagnostic : InDiagnostics)
		{
			if (RootCauseRank(Diagnostic) == Rank)
			{
				Ordered.Add(Diagnostic);
			}
		}
	}
	return Ordered;
}

FString FAssetDocumentResult::FormatDiagnosticsSummary(
	const TArray<FAssetDocumentDiagnostic>& InDiagnostics,
	const FString& FallbackMessage)
{
	if (InDiagnostics.Num() == 0)
	{
		return FallbackMessage;
	}
	// Wrapper/consequence diagnostics may precede the concrete cause they report;
	// lead with the first concrete error.
	int32 LeadIndex = 0;
	int32 LeadRank = RootCauseRank(InDiagnostics[0]);
	for (int32 Index = 1; Index < InDiagnostics.Num() && LeadRank > 0; ++Index)
	{
		const int32 Rank = RootCauseRank(InDiagnostics[Index]);
		if (Rank < LeadRank)
		{
			LeadIndex = Index;
			LeadRank = Rank;
		}
	}
	const FAssetDocumentDiagnostic& First = InDiagnostics[LeadIndex];
	FString Summary;
	if (!First.Code.IsEmpty() && !First.Message.IsEmpty())
	{
		Summary = FString::Printf(TEXT("%s: %s"), *First.Code, *First.Message);
	}
	else if (!First.Code.IsEmpty() || !First.Message.IsEmpty())
	{
		Summary = First.Code.IsEmpty() ? First.Message : First.Code;
	}
	else
	{
		Summary = FallbackMessage;
	}
	if (!First.Path.IsEmpty())
	{
		Summary += FString::Printf(TEXT(" (at %s)"), *First.Path);
	}
	if (InDiagnostics.Num() > 1)
	{
		Summary += FString::Printf(TEXT(" (+%d more)"), InDiagnostics.Num() - 1);
	}
	return Summary;
}

FAssetDocumentResult FAssetDocumentResult::FailureFromDiagnostics(
	const TArray<FAssetDocumentDiagnostic>& InDiagnostics,
	const FString& FallbackMessage)
{
	TArray<FAssetDocumentDiagnostic> Ordered = OrderRootCauseFirst(InDiagnostics);
	FAssetDocumentResult Result = Failure(FormatDiagnosticsSummary(Ordered, FallbackMessage));
	Result.Diagnostics = MoveTemp(Ordered);
	return Result;
}

TSharedPtr<FJsonObject> FAssetDocumentResult::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetBoolField(TEXT("success"), IsSuccess());
	Json->SetStringField(TEXT("message"), Message);
	Json->SetStringField(TEXT("target"), Target);
	Json->SetStringField(TEXT("asset_path"), AssetPath);
	Json->SetStringField(TEXT("sidecar_file_path"), SidecarFilePath);
	Json->SetBoolField(TEXT("saved_asset"), bSavedAsset);
	Json->SetBoolField(TEXT("wrote_sidecar"), bWroteSidecar);
	if (bReadOnly)
	{
		Json->SetBoolField(TEXT("read_only"), true);
	}

	TArray<TSharedPtr<FJsonValue>> DiagnosticValues;
	for (const FAssetDocumentDiagnostic& Diagnostic : Diagnostics)
	{
		TSharedPtr<FJsonObject> DiagnosticJson = MakeShared<FJsonObject>();
		DiagnosticJson->SetStringField(TEXT("path"), Diagnostic.Path);
		DiagnosticJson->SetStringField(TEXT("code"), Diagnostic.Code);
		DiagnosticJson->SetStringField(TEXT("message"), Diagnostic.Message);
		if (!Diagnostic.Severity.IsEmpty())
		{
			DiagnosticJson->SetStringField(TEXT("severity"), Diagnostic.Severity);
		}
		if (Diagnostic.Details.IsValid() && Diagnostic.Details->Values.Num() > 0)
		{
			DiagnosticJson->SetObjectField(TEXT("details"), Diagnostic.Details);
		}
		DiagnosticValues.Add(MakeShared<FJsonValueObject>(DiagnosticJson));
	}
	Json->SetArrayField(TEXT("diagnostics"), DiagnosticValues);

	if (Compile.IsValid())
	{
		Json->SetObjectField(TEXT("compile"), Compile);
	}

	if (Payload.IsValid())
	{
		Json->SetObjectField(TEXT("payload"), Payload);
	}

	return Json;
}
