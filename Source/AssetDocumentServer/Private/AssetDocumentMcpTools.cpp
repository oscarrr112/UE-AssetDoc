// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentMcpTools.h"

#include "AssetDocumentMcpJson.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include <limits>

namespace AssetDocumentMcpToolsLocal
{
using AssetDocumentMcpJson::ToJsString;

const TCHAR* const ApiPrefixForMessages = TEXT("UE AssetDocument API");

// A tool's outcome before it becomes the MCP result: a finished result (an error, an image), a text, or the route's
// JSON (serialized as compact text).
struct FOutcome
{
	TSharedPtr<FJsonObject> Result;
	TOptional<FString> Text;
	TSharedPtr<FJsonValue> Data;

	static FOutcome FromResult(const TSharedPtr<FJsonObject>& InResult)
	{
		FOutcome Outcome;
		Outcome.Result = InResult;
		return Outcome;
	}
	static FOutcome FromText(const FString& InText)
	{
		FOutcome Outcome;
		Outcome.Text = InText;
		return Outcome;
	}
	static FOutcome FromData(const TSharedPtr<FJsonObject>& Object)
	{
		FOutcome Outcome;
		Outcome.Data = MakeShared<FJsonValueObject>(Object.IsValid() ? Object.ToSharedRef() : MakeShared<FJsonObject>());
		return Outcome;
	}
};

// A failed argument check: the call answers "Error: Error: <message>", as a thrown Error caught by the tool handler.
struct FArgumentError
{
	FString Message;
};

TSharedPtr<FJsonObject> ThrownError(const FString& Message)
{
	return FAssetDocumentMcpTools::MakeTextResult(FString::Printf(TEXT("Error: Error: %s"), *Message), true);
}

bool IsRecord(const TSharedPtr<FJsonValue>& Value)
{
	return Value.IsValid() && Value->Type == EJson::Object && Value->AsObject().IsValid();
}

TSharedPtr<FJsonValue> Field(const TSharedPtr<FJsonObject>& Object, const FString& Key)
{
	return Object.IsValid() ? Object->TryGetField(Key) : nullptr;
}

TSharedPtr<FJsonValue> Field(const TSharedPtr<FJsonValue>& Value, const FString& Key)
{
	return IsRecord(Value) ? Value->AsObject()->TryGetField(Key) : nullptr;
}

bool IsString(const TSharedPtr<FJsonValue>& Value)
{
	return Value.IsValid() && Value->Type == EJson::String;
}

bool IsNumber(const TSharedPtr<FJsonValue>& Value)
{
	return Value.IsValid() && Value->Type == EJson::Number;
}

bool IsNullish(const TSharedPtr<FJsonValue>& Value)
{
	return !Value.IsValid() || Value->Type == EJson::Null || Value->Type == EJson::None;
}

// JavaScript truthiness of a parsed JSON value.
bool IsTruthy(const TSharedPtr<FJsonValue>& Value)
{
	if (IsNullish(Value))
	{
		return false;
	}
	switch (Value->Type)
	{
	case EJson::Boolean: return Value->AsBool();
	case EJson::Number: return Value->AsNumber() != 0.0 && !FMath::IsNaN(Value->AsNumber());
	case EJson::String: return !Value->AsString().IsEmpty();
	default: return true;
	}
}

// A string with non-whitespace content, untrimmed.
TOptional<FString> NonEmptyString(const TSharedPtr<FJsonValue>& Value)
{
	if (IsString(Value) && !Value->AsString().TrimStartAndEnd().IsEmpty())
	{
		return Value->AsString();
	}
	return {};
}

FString NonEmptyOr(const TSharedPtr<FJsonValue>& Value, const FString& Fallback)
{
	const TOptional<FString> Text = NonEmptyString(Value);
	return Text.IsSet() ? Text.GetValue() : Fallback;
}

FString Truncate(const FString& Text, int32 Max)
{
	return Text.Len() > Max ? FString::Printf(TEXT("%s... [truncated %d chars]"), *Text.Left(Max), Text.Len() - Max) : Text;
}

FString FormatDiagnostic(const TSharedPtr<FJsonValue>& Diagnostic)
{
	if (!IsRecord(Diagnostic))
	{
		return IsString(Diagnostic) ? Diagnostic->AsString() : AssetDocumentMcpJson::Stringify(Diagnostic);
	}
	const TSharedPtr<FJsonObject> Object = Diagnostic->AsObject();
	const FString Code = NonEmptyOr(Field(Object, TEXT("code")), TEXT("NoCode"));
	const TOptional<FString> Path = NonEmptyString(Field(Object, TEXT("path")));
	const FString Message = NonEmptyOr(Field(Object, TEXT("message")), TEXT("(no message)"));
	const TOptional<FString> Severity = NonEmptyString(Field(Object, TEXT("severity")));
	const FString Prefix = Severity.IsSet() && Severity.GetValue() != TEXT("error") ? Severity.GetValue() + TEXT(" ") : FString();
	const TSharedPtr<FJsonObject> Extra = MakeShared<FJsonObject>();
	for (const FString& Key : AssetDocumentMcpJson::OrderedKeys(Object))
	{
		if (Key != TEXT("code") && Key != TEXT("path") && Key != TEXT("message") && Key != TEXT("severity"))
		{
			Extra->SetField(Key, Object->TryGetField(Key));
		}
	}
	const FString ExtraText = Extra->Values.Num() > 0 ? TEXT(" ") + AssetDocumentMcpJson::StringifyObject(Extra) : FString();
	return FString::Printf(TEXT("%s[%s]%s: %s%s"), *Prefix, *Code, Path.IsSet() ? *(TEXT(" at ") + Path.GetValue()) : TEXT(""),
		*Message, *ExtraText);
}

TArray<FString> FormatCompileStatus(const TSharedPtr<FJsonValue>& Compile)
{
	TArray<FString> Lines;
	if (!IsRecord(Compile))
	{
		return Lines;
	}
	TArray<TSharedPtr<FJsonValue>> Messages;
	const TSharedPtr<FJsonValue> MessagesValue = Field(Compile, TEXT("Messages"));
	if (MessagesValue.IsValid() && MessagesValue->Type == EJson::Array)
	{
		for (const TSharedPtr<FJsonValue>& Message : MessagesValue->AsArray())
		{
			if (IsRecord(Message))
			{
				Messages.Add(Message);
			}
		}
	}
	Lines.Add(FString::Printf(TEXT("Compile: %s%s"), *NonEmptyOr(Field(Compile, TEXT("Status")), TEXT("Unknown")),
		Messages.Num() > 0 ? *FString::Printf(TEXT(" (%d messages)"), Messages.Num()) : TEXT("")));
	for (const TSharedPtr<FJsonValue>& Message : Messages)
	{
		TArray<FString> Where;
		for (const TCHAR* Key : {TEXT("Node"), TEXT("Graph")})
		{
			const TOptional<FString> Part = NonEmptyString(Field(Message, Key));
			if (Part.IsSet())
			{
				Where.Add(Part.GetValue());
			}
		}
		const FString WhereText = FString::Join(Where, TEXT(" in "));
		Lines.Add(FString::Printf(TEXT("  - %s: %s%s"), *NonEmptyOr(Field(Message, TEXT("Severity")), TEXT("Note")),
			*NonEmptyOr(Field(Message, TEXT("Message")), FString()), WhereText.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *WhereText)));
	}
	return Lines;
}

// The failure text of a route answer that is not a success: the message, the target, every diagnostic, the compile
// status and the full JSON.
FString FormatFailure(const FString& Method, const FString& Endpoint, int32 Status, const TSharedPtr<FJsonObject>& Data)
{
	const bool bOk = Status >= 200 && Status <= 299;
	const FString StatusLabel = FString::Printf(TEXT("HTTP %d"), Status);
	const FString Partial = Status == 207 ? TEXT(" (partial failure: some changes may have been applied)") : TEXT("");
	TArray<FString> Lines;

	const TOptional<FString> Message = NonEmptyString(Field(Data, TEXT("message")));
	const TSharedPtr<FJsonValue> ErrorValue = Field(Data, TEXT("error"));
	const TOptional<FString> LegacyError = IsNullish(ErrorValue) ? TOptional<FString>() : TOptional<FString>(ToJsString(ErrorValue));
	TArray<FString> SummaryParts;
	if (Message.IsSet())
	{
		SummaryParts.Add(Message.GetValue());
	}
	if (LegacyError.IsSet() && !LegacyError.GetValue().IsEmpty() && (!Message.IsSet() || !LegacyError.GetValue().Equals(Message.GetValue(), ESearchCase::CaseSensitive)))
	{
		SummaryParts.Add(LegacyError.GetValue());
	}
	const FString Summary = SummaryParts.Num() > 0 ? FString::Join(SummaryParts, TEXT(" | "))
		: bOk ? FString(TEXT("AssetDocument request reported success=false")) : FString(TEXT("request failed"));
	Lines.Add(FString::Printf(TEXT("%s %s %s failed with %s%s: %s"), ApiPrefixForMessages, *Method, *Endpoint, *StatusLabel, *Partial, *Summary));

	const TPair<const TCHAR*, const TCHAR*> Labels[] = {
		{TEXT("Target"), TEXT("target")}, {TEXT("Asset path"), TEXT("asset_path")}, {TEXT("Sidecar file"), TEXT("sidecar_file_path")}};
	for (const TPair<const TCHAR*, const TCHAR*>& Label : Labels)
	{
		const TOptional<FString> Value = NonEmptyString(Field(Data, Label.Value));
		if (Value.IsSet())
		{
			Lines.Add(FString::Printf(TEXT("%s: %s"), Label.Key, *Value.GetValue()));
		}
	}

	const TSharedPtr<FJsonValue> Diagnostics = Field(Data, TEXT("diagnostics"));
	if (Diagnostics.IsValid() && Diagnostics->Type == EJson::Array && Diagnostics->AsArray().Num() > 0)
	{
		const TArray<TSharedPtr<FJsonValue>>& Entries = Diagnostics->AsArray();
		Lines.Add(FString::Printf(TEXT("Diagnostics (%d):"), Entries.Num()));
		for (int32 Index = 0; Index < Entries.Num(); ++Index)
		{
			Lines.Add(FString::Printf(TEXT("  %d. %s"), Index + 1, *FormatDiagnostic(Entries[Index])));
		}
	}
	else
	{
		Lines.Add(TEXT("Diagnostics: none reported."));
	}
	Lines.Append(FormatCompileStatus(Field(Data, TEXT("compile"))));
	Lines.Add(FString());
	Lines.Add(TEXT("Full response JSON:"));
	Lines.Add(AssetDocumentMcpJson::StringifyObject(Data));
	return FString::Join(Lines, TEXT("\n"));
}

bool IsReportedFailure(const TSharedPtr<FJsonObject>& Data)
{
	const TSharedPtr<FJsonValue> Success = Field(Data, TEXT("success"));
	return Success.IsValid() && Success->Type == EJson::Boolean && !Success->AsBool();
}

// A route answer as a document tool reads it: the JSON when the route succeeded, else a tool error with the failure.
FOutcome InterpretDocumentAnswer(const FString& Method, const FString& Endpoint, const FAssetDocumentMcpRouteResult& Answer)
{
	const TSharedPtr<FJsonObject> Data = Answer.Json.IsValid() ? Answer.Json : MakeShared<FJsonObject>();
	const bool bOk = Answer.Status >= 200 && Answer.Status <= 299;
	if (!bOk || IsReportedFailure(Data))
	{
		return FOutcome::FromResult(FAssetDocumentMcpTools::MakeTextResult(FormatFailure(Method, Endpoint, Answer.Status, Data), true));
	}
	return FOutcome::FromData(Data);
}

TSharedPtr<FJsonObject> DeleteAssetsResult(const TSharedPtr<FJsonObject>& Data)
{
	const FString Text = AssetDocumentMcpJson::StringifyObject(Data);
	if (!IsReportedFailure(Data))
	{
		return FAssetDocumentMcpTools::MakeTextResult(Text, false);
	}
	TArray<FString> Failures;
	const TSharedPtr<FJsonValue> Results = Field(Data, TEXT("results"));
	if (Results.IsValid() && Results->Type == EJson::Array)
	{
		for (const TSharedPtr<FJsonValue>& Entry : Results->AsArray())
		{
			if (!IsRecord(Entry))
			{
				continue;
			}
			const TSharedPtr<FJsonValue> Status = Field(Entry, TEXT("status"));
			if (IsString(Status) && Status->AsString().Equals(TEXT("Success"), ESearchCase::CaseSensitive))
			{
				continue;
			}
			Failures.Add(FString::Printf(TEXT("%s: %s"), *NonEmptyOr(Field(Entry, TEXT("asset")), TEXT("(asset)")),
				*NonEmptyOr(Field(Entry, TEXT("error")), TEXT("not deleted"))));
		}
	}
	const FString Summary = Failures.Num() > 0 ? FString::Join(Failures, TEXT("\n")) : NonEmptyOr(Field(Data, TEXT("error")), TEXT("delete_assets failed"));
	return FAssetDocumentMcpTools::MakeTextResult(Summary + TEXT("\n") + Text, true);
}

FOutcome ExecutePythonOutcome(const TSharedPtr<FJsonObject>& Data)
{
	if (!IsReportedFailure(Data))
	{
		return FOutcome::FromData(Data);
	}
	const TSharedPtr<FJsonValue> Error = Field(Data, TEXT("error"));
	const FString Detail = IsString(Error) && !Error->AsString().IsEmpty() ? TEXT(": ") + Error->AsString() : FString();
	return FOutcome::FromResult(FAssetDocumentMcpTools::MakeTextResult(
		FString::Printf(TEXT("execute_python failed%s\n%s"), *Detail, *AssetDocumentMcpJson::StringifyObject(Data)), true));
}

FOutcome ScreenshotOutcome(const FOutcome& Answer)
{
	if (Answer.Result.IsValid())
	{
		return Answer;
	}
	const TSharedPtr<FJsonValue> Payload = Field(Answer.Data, TEXT("payload"));
	const TSharedPtr<FJsonValue> Data = Field(Payload, TEXT("data"));
	if (!IsTruthy(Data))
	{
		return FOutcome::FromResult(FAssetDocumentMcpTools::MakeTextResult(TEXT("Error: screenshot failed (no data returned)"), true));
	}
	FString Notes;
	const TSharedPtr<FJsonValue> Diagnostics = Field(Answer.Data, TEXT("diagnostics"));
	if (Diagnostics.IsValid() && Diagnostics->Type == EJson::Array)
	{
		for (const TSharedPtr<FJsonValue>& Diagnostic : Diagnostics->AsArray())
		{
			Notes += FString::Printf(TEXT(" %s: %s"), *ToJsString(Field(Diagnostic, TEXT("code"))), *ToJsString(Field(Diagnostic, TEXT("message"))));
		}
	}
	auto OrQuestion = [](const TSharedPtr<FJsonValue>& Value) { return IsNullish(Value) ? FString(TEXT("?")) : ToJsString(Value); };
	const TSharedPtr<FJsonValue> View = Field(Payload, TEXT("view"));
	const bool bGame = IsString(View) && View->AsString().Equals(TEXT("game"), ESearchCase::CaseSensitive);
	const FString What = bGame
		? FString::Printf(TEXT("Game view%s at world time %s s"), IsTruthy(Field(Payload, TEXT("hud"))) ? TEXT(" with HUD") : TEXT(""),
			*OrQuestion(Field(Payload, TEXT("world_time"))))
		: FString(TEXT("Level Editor viewport"));
	const TSharedPtr<FJsonValue> MimeType = Field(Payload, TEXT("mime_type"));
	const bool bPng = IsString(MimeType) && MimeType->AsString().Equals(TEXT("image/png"), ESearchCase::CaseSensitive);
	const TSharedPtr<FJsonValue> BytesValue = Field(Payload, TEXT("bytes"));
	double Bytes = 0.0;
	if (!IsNullish(BytesValue))
	{
		Bytes = IsNumber(BytesValue) ? BytesValue->AsNumber() : std::numeric_limits<double>::quiet_NaN();
	}
	else
	{
		Bytes = IsString(Data) ? static_cast<double>(Data->AsString().Len()) : std::numeric_limits<double>::quiet_NaN();
	}
	const double Kilobytes = Bytes / 1024.0;
	const FString KilobytesText = FMath::IsNaN(Kilobytes) ? FString(TEXT("NaN")) : FString::Printf(TEXT("%.1f"), Kilobytes);
	const FString Text = FString::Printf(TEXT("%s: %sx%s px, %s, %s KB.%s"), *What, *OrQuestion(Field(Payload, TEXT("width"))),
		*OrQuestion(Field(Payload, TEXT("height"))),
		bPng ? TEXT("PNG") : *FString::Printf(TEXT("JPEG quality %s"), *OrQuestion(Field(Payload, TEXT("quality")))), *KilobytesText, *Notes);

	const TSharedPtr<FJsonObject> Image = MakeShared<FJsonObject>();
	Image->SetStringField(TEXT("type"), TEXT("image"));
	Image->SetField(TEXT("data"), Data);
	Image->SetField(TEXT("mimeType"), IsNullish(MimeType) ? MakeShared<FJsonValueString>(TEXT("image/jpeg")) : MimeType);
	const TSharedPtr<FJsonObject> Caption = MakeShared<FJsonObject>();
	Caption->SetStringField(TEXT("type"), TEXT("text"));
	Caption->SetStringField(TEXT("text"), Text);
	const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("content"), {MakeShared<FJsonValueObject>(Image), MakeShared<FJsonValueObject>(Caption)});
	return FOutcome::FromResult(Result);
}

TSharedPtr<FJsonObject> Finalize(const FOutcome& Outcome)
{
	if (Outcome.Result.IsValid())
	{
		return Outcome.Result;
	}
	if (Outcome.Text.IsSet())
	{
		return FAssetDocumentMcpTools::MakeTextResult(Outcome.Text.GetValue(), false);
	}
	// A route answer that already is an MCP result (it has a content array) is the result as it is.
	const TSharedPtr<FJsonValue> Content = Field(Outcome.Data, TEXT("content"));
	if (Content.IsValid() && Content->Type == EJson::Array)
	{
		return Outcome.Data->AsObject();
	}
	return FAssetDocumentMcpTools::MakeTextResult(IsString(Outcome.Data) ? Outcome.Data->AsString() : AssetDocumentMcpJson::Stringify(Outcome.Data), false);
}

// --- Arguments -----------------------------------------------------------------------------------------------------

bool RequireString(const TSharedPtr<FJsonObject>& Arguments, const FString& Key, FArgumentError& OutError)
{
	const TSharedPtr<FJsonValue> Value = Field(Arguments, Key);
	if (!IsString(Value) || Value->AsString().IsEmpty())
	{
		OutError.Message = FString::Printf(TEXT("'%s' is required."), *Key);
		return false;
	}
	return true;
}

TOptional<FString> QueryText(const TSharedPtr<FJsonValue>& Value)
{
	if (IsString(Value) && !Value->AsString().IsEmpty())
	{
		return Value->AsString();
	}
	if (Value.IsValid() && Value->Type == EJson::Array && Value->AsArray().Num() > 0)
	{
		TArray<FString> Items;
		for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
		{
			Items.Add(!Item.IsValid() || Item->IsNull() ? FString(TEXT("null")) : ToJsString(Item));
		}
		return FString::Join(Items, TEXT(","));
	}
	if (Value.IsValid() && (Value->Type == EJson::Number || Value->Type == EJson::Boolean))
	{
		return ToJsString(Value);
	}
	return {};
}

TArray<FString> StringList(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key)
{
	TArray<FString> Out;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Object.IsValid() && Object->TryGetArrayField(Key, Values))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (IsString(Value))
			{
				Out.Add(Value->AsString());
			}
		}
	}
	return Out;
}

FString GetStringOr(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, const FString& Fallback = FString())
{
	FString Value;
	return Object.IsValid() && Object->TryGetStringField(Key, Value) ? Value : Fallback;
}

// The route request a call makes: endpoint with its encoded query (as the failure text names it), the decoded query,
// and the body.
struct FRouteRequest
{
	FString Method;
	FString Route;
	FString Endpoint;
	TMap<FString, FString> Query;
	FString Body;
};

bool BuildRouteRequest(const TSharedPtr<FJsonObject>& Call, const TSharedPtr<FJsonObject>& Arguments, FRouteRequest& Out, FArgumentError& OutError)
{
	for (const FString& Key : StringList(Call, TEXT("required")))
	{
		if (!RequireString(Arguments, Key, OutError))
		{
			return false;
		}
	}
	Out.Method = GetStringOr(Call, TEXT("method"), TEXT("POST"));
	Out.Route = GetStringOr(Call, TEXT("route"));

	TArray<FString> QueryParts;
	for (const FString& Key : StringList(Call, TEXT("query")))
	{
		const TOptional<FString> Text = QueryText(Field(Arguments, Key));
		if (Text.IsSet())
		{
			QueryParts.Add(AssetDocumentMcpJson::EncodeUriComponent(Key) + TEXT("=") + AssetDocumentMcpJson::EncodeUriComponent(Text.GetValue()));
			Out.Query.Add(Key, Text.GetValue());
		}
	}
	for (const FString& Key : StringList(Call, TEXT("query_flags")))
	{
		const TSharedPtr<FJsonValue> Value = Field(Arguments, Key);
		if (Value.IsValid() && Value->Type == EJson::Boolean && Value->AsBool())
		{
			QueryParts.Add(AssetDocumentMcpJson::EncodeUriComponent(Key) + TEXT("=true"));
			Out.Query.Add(Key, TEXT("true"));
		}
	}
	Out.Endpoint = QueryParts.Num() > 0 ? Out.Route + TEXT("?") + FString::Join(QueryParts, TEXT("&")) : Out.Route;

	if (Out.Method == TEXT("GET"))
	{
		return true;
	}
	const TSharedPtr<FJsonValue> BodySpec = Call->TryGetField(TEXT("body"));
	if (IsString(BodySpec) && BodySpec->AsString() == TEXT("arguments"))
	{
		Out.Body = AssetDocumentMcpJson::StringifyObject(Arguments);
	}
	else if (IsString(BodySpec) && BodySpec->AsString() == TEXT("document_or_file"))
	{
		const bool bHasDocument = Arguments->HasField(TEXT("document"));
		const TSharedPtr<FJsonValue> FilePath = Field(Arguments, TEXT("file_path"));
		const bool bHasFilePath = IsString(FilePath) && !FilePath->AsString().IsEmpty();
		if (bHasDocument == bHasFilePath)
		{
			OutError.Message = TEXT("Exactly one of 'document' or 'file_path' is required.");
			return false;
		}
		if (bHasDocument)
		{
			if (!IsRecord(Field(Arguments, TEXT("document"))))
			{
				OutError.Message = TEXT("'document' is required.");
				return false;
			}
			Out.Body = AssetDocumentMcpJson::Stringify(Field(Arguments, TEXT("document")));
		}
		else
		{
			const TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
			Body->SetField(TEXT("file_path"), FilePath);
			Out.Body = AssetDocumentMcpJson::StringifyObject(Body);
		}
	}
	else if (IsRecord(BodySpec))
	{
		const TSharedPtr<FJsonObject> Fields = BodySpec->AsObject();
		const TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		for (const FString& BodyKey : AssetDocumentMcpJson::OrderedKeys(Fields))
		{
			const TSharedPtr<FJsonObject> FieldSpec = Fields->GetObjectField(BodyKey);
			const TSharedPtr<FJsonValue> Value = Field(Arguments, GetStringOr(FieldSpec, TEXT("argument")));
			const TSharedPtr<FJsonValue> Default = Field(FieldSpec, TEXT("default"));
			if (Default.IsValid() && IsNullish(Value))
			{
				Body->SetField(BodyKey, Default);
			}
			else if (Value.IsValid())
			{
				Body->SetField(BodyKey, Value);
			}
		}
		Out.Body = AssetDocumentMcpJson::StringifyObject(Body);
	}
	return true;
}

FAssetDocumentMcpRouteResult CallRoute(const FAssetDocumentMcpRouteCaller& RouteCaller, const FRouteRequest& Request)
{
	FAssetDocumentMcpRouteResult Answer = RouteCaller(Request.Method, Request.Route, Request.Body, Request.Query);
	if (!Answer.Json.IsValid())
	{
		Answer.Json = MakeShared<FJsonObject>();
	}
	return Answer;
}

FOutcome ReadAnswer(const TSharedPtr<FJsonObject>& Call, const FRouteRequest& Request, const FAssetDocumentMcpRouteResult& Answer)
{
	return GetStringOr(Call, TEXT("response")) == TEXT("plain") ? FOutcome::FromData(Answer.Json)
		: InterpretDocumentAnswer(Request.Method, Request.Endpoint, Answer);
}

// --- play: a pending operation is polled until it finishes ----------------------------------------------------------

struct FPlayPoll
{
	TSharedPtr<FJsonObject> Call;
	FAssetDocumentMcpRouteCaller RouteCaller;
	TFunction<void(TSharedPtr<FJsonObject>)> OnResult;
	double Deadline = 0.0;
};

void ContinuePlay(const TSharedRef<FPlayPoll>& Poll, const FOutcome& Current)
{
	const TSharedPtr<FJsonValue> Pending = Field(Field(Current.Data, TEXT("payload")), TEXT("pending"));
	const TSharedPtr<FJsonValue> Id = Field(Pending, TEXT("id"));
	const TSharedPtr<FJsonValue> Success = Field(Current.Data, TEXT("success"));
	const bool bSucceeded = Success.IsValid() && Success->Type == EJson::Boolean && Success->AsBool();
	if (!Current.Result.IsValid() && bSucceeded && IsNumber(Id) && FPlatformTime::Seconds() < Poll->Deadline)
	{
		const TSharedPtr<FJsonValue> PollMs = Field(Pending, TEXT("poll_ms"));
		const double DelayMs = FMath::Max(20.0, IsNumber(PollMs) ? PollMs->AsNumber() : 50.0);
		const TSharedPtr<FJsonValue> IdValue = Id;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Poll, IdValue](float)
		{
			const TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
			Body->SetStringField(TEXT("action"), TEXT("poll"));
			Body->SetField(TEXT("id"), IdValue);
			FRouteRequest Request;
			Request.Method = TEXT("POST");
			Request.Route = GetStringOr(Poll->Call, TEXT("route"));
			Request.Endpoint = Request.Route;
			Request.Body = AssetDocumentMcpJson::StringifyObject(Body);
			ContinuePlay(Poll, ReadAnswer(Poll->Call, Request, CallRoute(Poll->RouteCaller, Request)));
			return false;
		}), static_cast<float>(DelayMs / 1000.0));
		return;
	}
	Poll->OnResult(Finalize(Current));
}

// --- Guides ----------------------------------------------------------------------------------------------------------

struct FGuidePart
{
	FString Name;
	FString Title;
	FString Text;
};

struct FGuideSection
{
	FString Name;
	FString Title;
	TArray<FString> Classes;
	FString Text;
	TArray<FGuidePart> Parts;
};

// Lines as a multiline regular expression sees them (any line terminator ends a line).
TArray<FString> SplitLines(const FString& Text)
{
	TArray<FString> Lines;
	FString Current;
	for (const TCHAR Char : Text)
	{
		if (Char == TEXT('\n') || Char == TEXT('\r') || Char == 0x2028 || Char == 0x2029)
		{
			Lines.Add(MoveTemp(Current));
			Current.Reset();
		}
		else
		{
			Current.AppendChar(Char);
		}
	}
	Lines.Add(MoveTemp(Current));
	return Lines;
}

// The capture of the first line "<Prefix><one or more characters>".
TOptional<FString> FirstLineAfter(const FString& Text, const FString& Prefix)
{
	for (const FString& Line : SplitLines(Text))
	{
		if (Line.Len() > Prefix.Len() && Line.StartsWith(Prefix, ESearchCase::CaseSensitive))
		{
			return Line.Mid(Prefix.Len());
		}
	}
	return {};
}

FString TitleOf(const FString& Text, const FString& Fallback)
{
	const TOptional<FString> Title = FirstLineAfter(Text, TEXT("# "));
	return Title.IsSet() ? Title.GetValue().TrimStartAndEnd() : Fallback;
}

TArray<FString> MarkdownFiles(const FString& Directory)
{
	TArray<FString> Names;
	IFileManager::Get().FindFiles(Names, *FPaths::Combine(Directory, TEXT("*")), true, false);
	Names.RemoveAll([](const FString& Name) { return !Name.EndsWith(TEXT(".md"), ESearchCase::CaseSensitive); });
	Names.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
	return Names;
}

FString ReadText(const FString& Path)
{
	FString Text;
	FFileHelper::LoadFileToString(Text, *Path);
	return Text;
}

TArray<FGuideSection> LoadSections(const FString& SectionsDirectory)
{
	TArray<FGuideSection> Sections;
	for (const FString& File : MarkdownFiles(SectionsDirectory))
	{
		FGuideSection& Section = Sections.AddDefaulted_GetRef();
		Section.Text = ReadText(FPaths::Combine(SectionsDirectory, File));
		Section.Title = TitleOf(Section.Text, File);
		Section.Name = File.LeftChop(3);
		const TOptional<FString> ClassLine = FirstLineAfter(Section.Text, TEXT("Classes: "));
		if (ClassLine.IsSet())
		{
			TArray<FString> Entries;
			ClassLine.GetValue().ParseIntoArray(Entries, TEXT(","), false);
			for (const FString& Entry : Entries)
			{
				const FString Trimmed = Entry.TrimStartAndEnd();
				if (Trimmed.StartsWith(TEXT("/"), ESearchCase::CaseSensitive))
				{
					Section.Classes.Add(Trimmed);
				}
			}
		}
		const FString PartsDirectory = FPaths::Combine(SectionsDirectory, Section.Name);
		if (IFileManager::Get().DirectoryExists(*PartsDirectory))
		{
			for (const FString& PartFile : MarkdownFiles(PartsDirectory))
			{
				FGuidePart& Part = Section.Parts.AddDefaulted_GetRef();
				Part.Text = ReadText(FPaths::Combine(PartsDirectory, PartFile));
				Part.Name = PartFile.LeftChop(3);
				Part.Title = TitleOf(Part.Text, PartFile);
			}
		}
	}
	return Sections;
}

FString SectionWithPartList(const FGuideSection& Section)
{
	if (Section.Parts.Num() == 0)
	{
		return Section.Text;
	}
	TArray<FString> Listed;
	for (const FGuidePart& Part : Section.Parts)
	{
		Listed.Add(FString::Printf(TEXT("`%s` (%s)"), *Part.Name, *Part.Title));
	}
	return FString::Printf(TEXT("%s\n\nReference parts (get_asset_document_schema with profile \"%s\" and part): %s.\n"),
		*Section.Text.TrimEnd(), *Section.Name, *FString::Join(Listed, TEXT(", ")));
}

FString ShortClassName(const FString& Path)
{
	int32 Dot = INDEX_NONE;
	return Path.FindLastChar(TEXT('.'), Dot) ? Path.Mid(Dot + 1) : Path;
}

bool SectionMatches(const FGuideSection& Section, const FString& Query)
{
	const FString Wanted = Query.TrimStartAndEnd().ToLower();
	if (Section.Name.ToLower() == Wanted)
	{
		return true;
	}
	for (const FString& Path : Section.Classes)
	{
		if (Path.ToLower() == Wanted || ShortClassName(Path).ToLower() == Wanted)
		{
			return true;
		}
	}
	return false;
}

bool HasNoneOf(const FString& Text, const TCHAR* Characters)
{
	for (const TCHAR Char : Text)
	{
		for (const TCHAR* Cursor = Characters; *Cursor; ++Cursor)
		{
			if (Char == *Cursor)
			{
				return false;
			}
		}
	}
	return true;
}

// "/Script/<Module>.<Class>" or "/Game/<path>.<Name>_C": a class path, not an asset path.
bool IsClassPathWithoutSection(const FString& Query)
{
	if (Query.StartsWith(TEXT("/Script/"), ESearchCase::CaseSensitive))
	{
		const FString Rest = Query.Mid(8);
		int32 Dot = INDEX_NONE;
		if (Rest.FindChar(TEXT('.'), Dot))
		{
			const FString Module = Rest.Left(Dot);
			const FString Class = Rest.Mid(Dot + 1);
			if (!Module.IsEmpty() && !Class.IsEmpty() && HasNoneOf(Module, TEXT("./")) && HasNoneOf(Class, TEXT("./")))
			{
				return true;
			}
		}
	}
	if (Query.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive))
	{
		const FString Rest = Query.Mid(6);
		int32 Dot = INDEX_NONE;
		if (Rest.FindChar(TEXT('.'), Dot) && Dot > 0)
		{
			const FString Name = Rest.Mid(Dot + 1);
			if (Name.Len() >= 3 && Name.EndsWith(TEXT("_C"), ESearchCase::CaseSensitive) && HasNoneOf(Name, TEXT("./")))
			{
				return true;
			}
		}
	}
	return false;
}

FOutcome AssetDocumentGuide(const FString& SchemasDirectory, const TSharedPtr<FJsonValue>& Profile, const TSharedPtr<FJsonValue>& PartValue)
{
	const FString SectionsDirectory = FPaths::Combine(SchemasDirectory, TEXT("assetdocument"));
	const FString GuidePath = FPaths::Combine(SchemasDirectory, TEXT("AssetDocument.md"));
	const TArray<FGuideSection> Sections = LoadSections(SectionsDirectory);
	const FString Query = IsString(Profile) ? Profile->AsString().TrimStartAndEnd() : FString();
	const FString WantedPart = IsString(PartValue) ? PartValue->AsString().TrimStartAndEnd() : FString();
	auto SchemaError = [](const FString& Text) { return FOutcome::FromResult(FAssetDocumentMcpTools::MakeTextResult(Text, true)); };

	if (Query.ToLower() == TEXT("all"))
	{
		TArray<FString> Texts = {ReadText(GuidePath)};
		for (const FGuideSection& Section : Sections)
		{
			Texts.Add(SectionWithPartList(Section));
			for (const FGuidePart& Part : Section.Parts)
			{
				Texts.Add(Part.Text);
			}
		}
		return FOutcome::FromText(FString::Join(Texts, TEXT("\n\n")));
	}
	if (!Query.IsEmpty())
	{
		const FGuideSection* Match = Sections.FindByPredicate([&Query](const FGuideSection& Section) { return SectionMatches(Section, Query); });
		if (Match != nullptr && !WantedPart.IsEmpty())
		{
			for (const FGuidePart& Part : Match->Parts)
			{
				if (Part.Name.ToLower() == WantedPart.ToLower())
				{
					return FOutcome::FromText(Part.Text);
				}
			}
			TArray<FString> Listed;
			for (const FGuidePart& Part : Match->Parts)
			{
				Listed.Add(FString::Printf(TEXT("%s (%s)"), *Part.Name, *Part.Title));
			}
			return SchemaError(FString::Printf(TEXT("The %s section has no part '%s'. %s"), *Match->Name, *WantedPart,
				Listed.Num() > 0 ? *FString::Printf(TEXT("Parts: %s."), *FString::Join(Listed, TEXT(", "))) : TEXT("It has no reference parts.")));
		}
		if (Match != nullptr)
		{
			return FOutcome::FromText(SectionWithPartList(*Match));
		}
	}
	if (!WantedPart.IsEmpty() && Query.IsEmpty())
	{
		return SchemaError(TEXT("'part' needs 'profile': the section the part belongs to (its closing line lists its parts)."));
	}
	if (!Query.IsEmpty())
	{
		if (IsClassPathWithoutSection(Query))
		{
			if (const FGuideSection* Generic = Sections.FindByPredicate([](const FGuideSection& Section) { return Section.Name == TEXT("Generic"); }))
			{
				return FOutcome::FromText(FString::Printf(TEXT("No exact profile section documents '%s': it uses the generic reflected profile "
					"(inspect_asset_document_profile confirms).\n\n%s"), *Query, *Generic->Text));
			}
		}
		TArray<FString> Detailed;
		for (const FGuideSection& Section : Sections)
		{
			TArray<FString> Short;
			for (const FString& Path : Section.Classes)
			{
				Short.Add(ShortClassName(Path));
			}
			Detailed.Add(FString::Printf(TEXT("- %s: %s%s"), *Section.Name, *Section.Title,
				Short.Num() > 0 ? *FString::Printf(TEXT(" (%s)"), *FString::Join(Short, TEXT(", "))) : TEXT("")));
		}
		return SchemaError(FString::Printf(TEXT("No AssetDocument section for '%s'. Sections:\n%s\nAny other class uses the Generic section."),
			*Query, *FString::Join(Detailed, TEXT("\n"))));
	}
	TArray<FString> Names;
	for (const FGuideSection& Section : Sections)
	{
		Names.Add(Section.Name);
	}
	return FOutcome::FromText(FString::Printf(TEXT("%s\nSections: %s.\n"), *ReadText(GuidePath), *FString::Join(Names, TEXT(", "))));
}

FOutcome EditorGuide(const FString& SchemasDirectory, const TSharedPtr<FJsonObject>& Call, const TSharedPtr<FJsonObject>& Arguments)
{
	const TSharedPtr<FJsonValue> Topic = Field(Arguments, GetStringOr(Call, TEXT("argument"), TEXT("topic")));
	FString File = GetStringOr(Call, TEXT("default"));
	const TSharedPtr<FJsonObject>* Topics = nullptr;
	if (IsString(Topic) && Call->TryGetObjectField(TEXT("topics"), Topics))
	{
		// Topic names compare exactly.
		for (const FString& Key : AssetDocumentMcpJson::OrderedKeys(*Topics))
		{
			const TSharedPtr<FJsonValue> Mapped = (*Topics)->TryGetField(Key);
			if (Key.Equals(Topic->AsString(), ESearchCase::CaseSensitive) && IsString(Mapped))
			{
				File = Mapped->AsString();
			}
		}
	}
	return FOutcome::FromText(ReadText(FPaths::Combine(SchemasDirectory, File)));
}
} // namespace AssetDocumentMcpToolsLocal

FString FAssetDocumentMcpTools::DefaultMcpDirectory()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UEAssetDoc"));
	return Plugin.IsValid() ? FPaths::ConvertRelativePathToFull(FPaths::Combine(Plugin->GetBaseDir(), TEXT("MCP"))) : FString();
}

bool FAssetDocumentMcpTools::Load(const FString& InMcpDirectory, FString& OutError)
{
	McpDirectory = InMcpDirectory;
	Tools.Reset();
	ServerInfo.Reset();
	const FString ManifestPath = FPaths::Combine(McpDirectory, TEXT("tools.json"));
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *ManifestPath))
	{
		OutError = FString::Printf(TEXT("The MCP tool manifest %s cannot be read."), *ManifestPath);
		return false;
	}
	const TSharedPtr<FJsonObject> Manifest = AssetDocumentMcpJson::ParseObject(Text);
	const TArray<TSharedPtr<FJsonValue>>* ToolValues = nullptr;
	if (!Manifest.IsValid() || !Manifest->TryGetArrayField(TEXT("tools"), ToolValues))
	{
		OutError = FString::Printf(TEXT("The MCP tool manifest %s is not a JSON object with a tools array."), *ManifestPath);
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *ToolValues)
	{
		const TSharedPtr<FJsonObject> Tool = AssetDocumentMcpToolsLocal::IsRecord(Value) ? Value->AsObject() : nullptr;
		if (!Tool.IsValid() || !Tool->HasTypedField<EJson::String>(TEXT("name")) || !Tool->HasTypedField<EJson::Object>(TEXT("call")))
		{
			OutError = FString::Printf(TEXT("The MCP tool manifest %s has a tool without a name or call."), *ManifestPath);
			Tools.Reset();
			return false;
		}
		Tools.Add(Tool);
	}
	const TSharedPtr<FJsonObject>* Server = nullptr;
	ServerInfo = Manifest->TryGetObjectField(TEXT("server"), Server) ? *Server : MakeShared<FJsonObject>();
	return true;
}

TSharedPtr<FJsonObject> FAssetDocumentMcpTools::GetServerInfo() const
{
	return ServerInfo.IsValid() ? ServerInfo : MakeShared<FJsonObject>();
}

namespace AssetDocumentMcpToolsLocal
{
bool IsEnabled(const TSharedPtr<FJsonObject>& Tool, const FAssetDocumentMcpToolPolicy& Policy)
{
	const FString Setting = GetStringOr(Tool->GetObjectField(TEXT("call")), TEXT("setting"));
	if (Setting == TEXT("execute_python"))
	{
		return Policy.bExecutePython;
	}
	if (Setting == TEXT("delete_assets"))
	{
		return Policy.bDeleteAssets;
	}
	return true;
}
} // namespace AssetDocumentMcpToolsLocal

TArray<TSharedPtr<FJsonValue>> FAssetDocumentMcpTools::ListTools(const FAssetDocumentMcpToolPolicy& Policy) const
{
	TArray<TSharedPtr<FJsonValue>> Listed;
	for (const TSharedPtr<FJsonObject>& Tool : Tools)
	{
		if (!AssetDocumentMcpToolsLocal::IsEnabled(Tool, Policy))
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		for (const TCHAR* Key : {TEXT("name"), TEXT("description"), TEXT("inputSchema")})
		{
			if (const TSharedPtr<FJsonValue> Value = Tool->TryGetField(Key))
			{
				Entry->SetField(Key, Value);
			}
		}
		Listed.Add(MakeShared<FJsonValueObject>(Entry));
	}
	return Listed;
}

TSharedPtr<FJsonObject> FAssetDocumentMcpTools::MakeTextResult(const FString& Text, bool bIsError)
{
	const TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
	Block->SetStringField(TEXT("type"), TEXT("text"));
	Block->SetStringField(TEXT("text"), Text);
	const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("content"), {MakeShared<FJsonValueObject>(Block)});
	if (bIsError)
	{
		Result->SetBoolField(TEXT("isError"), true);
	}
	return Result;
}

FString FAssetDocumentMcpTools::DescribeDisabledTool(const FString& Name)
{
	return FString::Printf(TEXT("%s is turned off in this editor (Project Settings > Plugins > UE-AssetDoc > Security)."), *Name);
}

void FAssetDocumentMcpTools::CallTool(const FString& Name, const TSharedPtr<FJsonObject>& InArguments, const FAssetDocumentMcpToolPolicy& Policy,
	const FAssetDocumentMcpRouteCaller& RouteCaller, TFunction<void(TSharedPtr<FJsonObject>)> OnResult) const
{
	using namespace AssetDocumentMcpToolsLocal;
	const TSharedPtr<FJsonObject> Arguments = InArguments.IsValid() ? InArguments : MakeShared<FJsonObject>();
	const TSharedPtr<FJsonObject>* Found = Tools.FindByPredicate([&Name](const TSharedPtr<FJsonObject>& Tool)
	{
		return GetStringOr(Tool, TEXT("name")).Equals(Name, ESearchCase::CaseSensitive);
	});
	if (Found == nullptr)
	{
		OnResult(ThrownError(FString::Printf(TEXT("unknown tool: %s"), *Name)));
		return;
	}
	if (!IsEnabled(*Found, Policy))
	{
		OnResult(MakeTextResult(DescribeDisabledTool(Name), true));
		return;
	}
	const TSharedPtr<FJsonObject> Call = (*Found)->GetObjectField(TEXT("call"));
	const FString SchemasDirectory = FPaths::Combine(McpDirectory, TEXT("schemas"));

	const FString Guide = GetStringOr(Call, TEXT("guide"));
	if (Guide == TEXT("asset_document"))
	{
		OnResult(Finalize(AssetDocumentGuide(SchemasDirectory, Field(Arguments, TEXT("profile")), Field(Arguments, TEXT("part")))));
		return;
	}
	if (Guide == TEXT("editor"))
	{
		OnResult(Finalize(EditorGuide(SchemasDirectory, Call, Arguments)));
		return;
	}

	FRouteRequest Request;
	FArgumentError Error;
	if (!BuildRouteRequest(Call, Arguments, Request, Error))
	{
		OnResult(ThrownError(Error.Message));
		return;
	}
	const FOutcome Answer = ReadAnswer(Call, Request, CallRoute(RouteCaller, Request));
	const FString ResultKind = GetStringOr(Call, TEXT("result"));
	if (ResultKind == TEXT("poll"))
	{
		const TSharedRef<FPlayPoll> Poll = MakeShared<FPlayPoll>();
		Poll->Call = Call;
		Poll->RouteCaller = RouteCaller;
		Poll->OnResult = MoveTemp(OnResult);
		Poll->Deadline = FPlatformTime::Seconds() + 150.0;
		ContinuePlay(Poll, Answer);
		return;
	}
	if (ResultKind == TEXT("delete_assets"))
	{
		OnResult(DeleteAssetsResult(Answer.Data.IsValid() ? Answer.Data->AsObject() : nullptr));
		return;
	}
	if (ResultKind == TEXT("execute_python"))
	{
		OnResult(Finalize(Answer.Result.IsValid() ? Answer : ExecutePythonOutcome(Answer.Data->AsObject())));
		return;
	}
	if (ResultKind == TEXT("screenshot"))
	{
		OnResult(Finalize(ScreenshotOutcome(Answer)));
		return;
	}
	OnResult(Finalize(Answer));
}
