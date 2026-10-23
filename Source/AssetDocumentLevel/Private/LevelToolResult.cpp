// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "LevelToolResult.h"

#include "Dom/JsonValue.h"

namespace UE::AssetDocumentLevel::Private
{
FLevelToolResult FLevelToolResult::Success(FString InMessage, TSharedPtr<FJsonObject> InPayload)
{
	FLevelToolResult Result;
	Result.Message = MoveTemp(InMessage);
	Result.Payload = MoveTemp(InPayload);
	return Result;
}

FLevelToolResult FLevelToolResult::Failure(TArray<FAssetDocumentDiagnostic> InDiagnostics, TSharedPtr<FJsonObject> InPayload)
{
	FLevelToolResult Result;
	Result.HttpStatus = 400;
	Result.bSuccess = false;
	Result.Diagnostics = FAssetDocumentResult::OrderRootCauseFirst(InDiagnostics);
	Result.Message = FAssetDocumentResult::FormatDiagnosticsSummary(Result.Diagnostics, TEXT("Level request failed."));
	Result.Payload = MoveTemp(InPayload);
	return Result;
}

TSharedRef<FJsonObject> FLevelToolResult::ToJson() const
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetBoolField(TEXT("success"), bSuccess);
	Json->SetStringField(TEXT("message"), Message);
	TArray<TSharedPtr<FJsonValue>> DiagnosticValues;
	for (const FAssetDocumentDiagnostic& Diagnostic : Diagnostics)
	{
		TSharedRef<FJsonObject> DiagnosticJson = MakeShared<FJsonObject>();
		DiagnosticJson->SetStringField(TEXT("path"), Diagnostic.Path);
		DiagnosticJson->SetStringField(TEXT("code"), Diagnostic.Code);
		DiagnosticJson->SetStringField(TEXT("message"), Diagnostic.Message);
		DiagnosticJson->SetStringField(TEXT("severity"), Diagnostic.Severity.IsEmpty() ? TEXT("error") : *Diagnostic.Severity);
		if (Diagnostic.Details.IsValid() && Diagnostic.Details->Values.Num() > 0)
		{
			DiagnosticJson->SetObjectField(TEXT("details"), Diagnostic.Details);
		}
		DiagnosticValues.Add(MakeShared<FJsonValueObject>(DiagnosticJson));
	}
	Json->SetArrayField(TEXT("diagnostics"), DiagnosticValues);
	if (Payload.IsValid())
	{
		Json->SetObjectField(TEXT("payload"), Payload);
	}
	return Json;
}

const FAssetDocumentDiagnostic* FLevelToolResult::FindDiagnostic(const FString& Code) const
{
	return Diagnostics.FindByPredicate([&Code](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == Code; });
}

FAssetDocumentDiagnostic MakeLevelDiagnostic(const FString& Path, const FString& Code, const FString& Message,
	TSharedPtr<FJsonObject> Details)
{
	FAssetDocumentDiagnostic Diagnostic;
	Diagnostic.Path = Path;
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	Diagnostic.Severity = TEXT("error");
	Diagnostic.Details = MoveTemp(Details);
	return Diagnostic;
}
}
