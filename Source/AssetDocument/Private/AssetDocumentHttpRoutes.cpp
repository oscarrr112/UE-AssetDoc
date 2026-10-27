// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentHttpRoutes.h"

#include "AssetDocumentJson.h"

#include "AssetDocumentService.h"
#include "AssetDocumentHttpServer.h"

#include "Async/Async.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include <initializer_list>

namespace
{
bool TryParseJsonBody(const FString& RequestBody, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
{
	if (RequestBody.IsEmpty())
	{
		OutError = TEXT("Request body is required");
		return false;
	}

	// Keep the authored number kinds (1 vs 1.0): an inline document must reach the
	// profiles exactly as the same document read from its sidecar file.
	if (!FAssetDocumentJson::ParseJsonString(RequestBody, OutJson))
	{
		FString Path;
		FString First;
		FString Second;
		OutError = FAssetDocumentJson::FindDuplicateKey(RequestBody, Path, First, Second)
			? FString::Printf(TEXT("Invalid JSON body: Document.DuplicateKey: %s (at %s)"),
				*FAssetDocumentJson::DuplicateKeyMessage(Path, First, Second), *Path)
			: FString(TEXT("Invalid JSON body"));
		return false;
	}

	return true;
}

bool TryGetStringFieldAny(const TSharedPtr<FJsonObject>& Json, std::initializer_list<const TCHAR*> FieldNames, FString& OutValue)
{
	if (!Json.IsValid())
	{
		return false;
	}

	for (const TCHAR* FieldName : FieldNames)
	{
		if (Json->TryGetStringField(FieldName, OutValue))
		{
			return true;
		}
	}
	return false;
}

bool TryGetBoolFieldAny(const TSharedPtr<FJsonObject>& Json, std::initializer_list<const TCHAR*> FieldNames, bool& OutValue)
{
	if (!Json.IsValid())
	{
		return false;
	}

	for (const TCHAR* FieldName : FieldNames)
	{
		if (Json->TryGetBoolField(FieldName, OutValue))
		{
			return true;
		}
	}
	return false;
}

bool TryGetQueryParamAny(const TMap<FString, FString>& QueryParams, std::initializer_list<const TCHAR*> FieldNames, FString& OutValue)
{
	for (const TCHAR* FieldName : FieldNames)
	{
		if (const FString* Value = QueryParams.Find(FString(FieldName)))
		{
			OutValue = *Value;
			return true;
		}
	}
	return false;
}

TSharedPtr<FJsonObject> MakeErrorJson(const FString& ErrorMessage)
{
	TSharedPtr<FJsonObject> ErrorJson = MakeShared<FJsonObject>();
	ErrorJson->SetBoolField(TEXT("success"), false);
	ErrorJson->SetStringField(TEXT("error"), ErrorMessage);
	return ErrorJson;
}

int32 GetStatusCode(const FAssetDocumentResult& Result)
{
	switch (Result.Status)
	{
	case EAssetDocumentResultStatus::Success:
		return 200;
	case EAssetDocumentResultStatus::PartialFailed:
		return 207;
	case EAssetDocumentResultStatus::Failed:
	default:
		return 400;
	}
}

template<typename CallableType>
FAssetDocumentResult RunOnGameThread(CallableType&& Callable)
{
	if (IsInGameThread())
	{
		return Callable();
	}

	FAssetDocumentResult Result;
	FEvent* CompletionEvent = FPlatformProcess::GetSynchEventFromPool(true);
	AsyncTask(ENamedThreads::GameThread, [&]()
	{
		Result = Callable();
		CompletionEvent->Trigger();
	});
	CompletionEvent->Wait();
	FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);
	return Result;
}

void AddRouteDescriptor(TArray<TSharedPtr<FJsonValue>>& Routes, const FString& Method, const FString& Path, const FString& Description, bool bRequiresBody)
{
	TSharedPtr<FJsonObject> Route = MakeShared<FJsonObject>();
	Route->SetStringField(TEXT("method"), Method);
	Route->SetStringField(TEXT("path"), Path);
	Route->SetStringField(TEXT("description"), Description);
	Route->SetBoolField(TEXT("requires_body"), bRequiresBody);
	Routes.Add(MakeShared<FJsonValueObject>(Route));
}

TArray<TSharedPtr<FJsonValue>> BuildRouteDescriptors()
{
	TArray<TSharedPtr<FJsonValue>> Routes;
	AddRouteDescriptor(Routes, TEXT("POST"), TEXT("/assetdoc/assetdocument/apply-file"), TEXT("Apply an AssetDocument sidecar file"), true);
	AddRouteDescriptor(Routes, TEXT("GET"), TEXT("/assetdoc/assetdocument/schema"), TEXT("Describe AssetDocument HTTP routes"), false);
	AddRouteDescriptor(Routes, TEXT("GET"), TEXT("/assetdoc/assetdocument/inspect"), TEXT("Inspect writable reflected properties for a class or asset"), false);
	AddRouteDescriptor(Routes, TEXT("GET"), TEXT("/assetdoc/assetdocument/profile"), TEXT("Inspect the AssetDocument profile for a class or asset"), false);
	AddRouteDescriptor(Routes, TEXT("POST"), TEXT("/assetdoc/assetdocument/template"), TEXT("Create a generic AssetDocument template"), true);
	AddRouteDescriptor(Routes, TEXT("POST"), TEXT("/assetdoc/assetdocument/extract"), TEXT("Extract an asset as an AssetDocument JSON document"), true);
	AddRouteDescriptor(Routes, TEXT("POST"), TEXT("/assetdoc/assetdocument/validate"), TEXT("Validate an AssetDocument JSON document or sidecar file"), true);
	AddRouteDescriptor(Routes, TEXT("POST"), TEXT("/assetdoc/assetdocument/diff"), TEXT("Diff an AssetDocument JSON document or sidecar file against the current asset"), true);
	AddRouteDescriptor(Routes, TEXT("GET"), TEXT("/assetdoc/assetdocument/find-assets"), TEXT("Find assets by path prefix, class and name (Asset Registry, paged)"), false);
	AddRouteDescriptor(Routes, TEXT("GET"), TEXT("/assetdoc/assetdocument/list-types"), TEXT("List classes, structs or enums (reflection and Asset Registry, paged)"), false);
	AddRouteDescriptor(Routes, TEXT("POST"), TEXT("/assetdoc/assetdocument/duplicate"), TEXT("Duplicate an asset into an absent /Game path and save it"), true);
	AddRouteDescriptor(Routes, TEXT("POST"), TEXT("/assetdoc/assetdocument/sync"), TEXT("Sidecar mirroring: status, conflict resolution and materialize"), true);
	return Routes;
}

TSharedPtr<FJsonObject> MakeServiceUnavailableJson(int32& OutStatusCode)
{
	OutStatusCode = 503;
	return MakeErrorJson(TEXT("AssetDocument service is unavailable"));
}

template<typename HandlerType>
FAssetDocumentExternalRouteHandler CreateServiceRouteHandler(TWeakPtr<FAssetDocumentService> WeakService, HandlerType Handler)
{
	return FAssetDocumentExternalRouteHandler::CreateLambda([WeakService, Handler](const FString& RequestBody, const TMap<FString, FString>& QueryParams, int32& OutStatusCode)
	{
		TSharedPtr<FAssetDocumentService> PinnedService = WeakService.Pin();
		if (!PinnedService.IsValid())
		{
			return MakeServiceUnavailableJson(OutStatusCode);
		}

		return Handler(PinnedService.ToSharedRef(), RequestBody, QueryParams, OutStatusCode);
	});
}

TSharedPtr<FJsonObject> HandleApplyFile(const TSharedRef<FAssetDocumentService>& Service, const FString& RequestBody, const TMap<FString, FString>& QueryParams, int32& OutStatusCode)
{
	TSharedPtr<FJsonObject> Json;
	FString Error;
	if (!TryParseJsonBody(RequestBody, Json, Error))
	{
		OutStatusCode = 400;
		return MakeErrorJson(Error);
	}

	FAssetDocumentApplyFileRequest ApplyFileRequest;
	if (!TryGetStringFieldAny(Json, {TEXT("file_path"), TEXT("FilePath")}, ApplyFileRequest.FilePath) || ApplyFileRequest.FilePath.IsEmpty())
	{
		OutStatusCode = 400;
		return MakeErrorJson(TEXT("ApplyFile requires file_path or FilePath"));
	}
	TryGetBoolFieldAny(Json, {TEXT("save_asset"), TEXT("SaveAsset")}, ApplyFileRequest.bSaveAsset);

	const FAssetDocumentResult Result = RunOnGameThread([Service, ApplyFileRequest]()
	{
		return Service->ApplyFile(ApplyFileRequest);
	});

	OutStatusCode = GetStatusCode(Result);
	return Result.ToJson();
}

TSharedPtr<FJsonObject> HandleSchema(const TSharedRef<FAssetDocumentService>& Service, const FString& RequestBody, const TMap<FString, FString>& QueryParams, int32& OutStatusCode)
{
	const FAssetDocumentResult Result = RunOnGameThread([Service]()
	{
		return Service->GetSchema();
	});

	OutStatusCode = GetStatusCode(Result);
	if (!Result.IsSuccess() || !Result.Payload.IsValid())
	{
		return Result.ToJson();
	}

	TSharedPtr<FJsonObject> Json = Result.Payload;
	Json->SetBoolField(TEXT("success"), true);
	Json->SetArrayField(TEXT("routes"), BuildRouteDescriptors());
	return Json;
}

TSharedPtr<FJsonObject> HandleInspect(const TSharedRef<FAssetDocumentService>& Service, const FString& RequestBody, const TMap<FString, FString>& QueryParams, int32& OutStatusCode)
{
	FAssetDocumentInspectRequest InspectRequest;
	if (!TryGetQueryParamAny(QueryParams, {TEXT("class_or_asset"), TEXT("ClassOrAsset"), TEXT("target"), TEXT("Target")}, InspectRequest.ClassOrAsset) || InspectRequest.ClassOrAsset.IsEmpty())
	{
		OutStatusCode = 400;
		return MakeErrorJson(TEXT("Inspect requires class_or_asset, ClassOrAsset, target, or Target"));
	}

	FString Detail;
	InspectRequest.bSummary = !(TryGetQueryParamAny(QueryParams, {TEXT("detail")}, Detail)
		&& (Detail.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Detail == TEXT("1")));

	const FAssetDocumentResult Result = RunOnGameThread([Service, InspectRequest]()
	{
		return Service->Inspect(InspectRequest);
	});

	OutStatusCode = GetStatusCode(Result);
	return Result.ToJson();
}

TSharedPtr<FJsonObject> HandleProfile(const TSharedRef<FAssetDocumentService>& Service, const FString& RequestBody, const TMap<FString, FString>& QueryParams, int32& OutStatusCode)
{
	FAssetDocumentProfileRequest ProfileRequest;
	if (!TryGetQueryParamAny(QueryParams, {TEXT("class_or_asset"), TEXT("ClassOrAsset"), TEXT("target"), TEXT("Target")}, ProfileRequest.ClassOrAsset) || ProfileRequest.ClassOrAsset.IsEmpty())
	{
		OutStatusCode = 400;
		return MakeErrorJson(TEXT("InspectProfile requires class_or_asset, ClassOrAsset, target, or Target"));
	}

	FString Debug;
	ProfileRequest.bIncludeRuntimeRegistration = TryGetQueryParamAny(QueryParams, {TEXT("debug")}, Debug)
		&& (Debug.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Debug == TEXT("1"));

	const FAssetDocumentResult Result = RunOnGameThread([Service, ProfileRequest]()
	{
		return Service->InspectProfile(ProfileRequest);
	});

	OutStatusCode = GetStatusCode(Result);
	return Result.ToJson();
}

TSharedPtr<FJsonObject> HandleFindAssets(const TSharedRef<FAssetDocumentService>& Service, const FString& RequestBody, const TMap<FString, FString>& QueryParams, int32& OutStatusCode)
{
	const FAssetDocumentResult Result = RunOnGameThread([Service, QueryParams]()
	{
		return Service->FindAssets(QueryParams);
	});
	OutStatusCode = GetStatusCode(Result);
	return Result.ToJson();
}

TSharedPtr<FJsonObject> HandleListTypes(const TSharedRef<FAssetDocumentService>& Service, const FString& RequestBody, const TMap<FString, FString>& QueryParams, int32& OutStatusCode)
{
	const FAssetDocumentResult Result = RunOnGameThread([Service, QueryParams]()
	{
		return Service->ListTypes(QueryParams);
	});
	OutStatusCode = GetStatusCode(Result);
	return Result.ToJson();
}

TSharedPtr<FJsonObject> HandleDuplicate(const TSharedRef<FAssetDocumentService>& Service, const FString& RequestBody, const TMap<FString, FString>& QueryParams, int32& OutStatusCode)
{
	TSharedPtr<FJsonObject> Json;
	FString Error;
	if (!TryParseJsonBody(RequestBody, Json, Error))
	{
		OutStatusCode = 400;
		return MakeErrorJson(Error);
	}

	FAssetDocumentDuplicateRequest DuplicateRequest;
	if (!TryGetStringFieldAny(Json, {TEXT("source"), TEXT("Source")}, DuplicateRequest.Source) || DuplicateRequest.Source.IsEmpty()
		|| !TryGetStringFieldAny(Json, {TEXT("target"), TEXT("Target")}, DuplicateRequest.Target) || DuplicateRequest.Target.IsEmpty())
	{
		OutStatusCode = 400;
		return MakeErrorJson(TEXT("Duplicate requires source and target"));
	}

	const FAssetDocumentResult Result = RunOnGameThread([Service, DuplicateRequest]()
	{
		return Service->Duplicate(DuplicateRequest);
	});

	OutStatusCode = GetStatusCode(Result);
	return Result.ToJson();
}

// A string array field; a single string counts as one element.
bool TryGetSyncStringList(const TSharedPtr<FJsonObject>& Json, const TCHAR* Field, TArray<FString>& OutValues, FString& OutError)
{
	const TSharedPtr<FJsonValue> Value = Json.IsValid() ? Json->TryGetField(Field) : nullptr;
	if (!Value.IsValid() || Value->Type == EJson::Null)
	{
		return true;
	}
	if (Value->Type == EJson::String)
	{
		OutValues.Add(Value->AsString());
		return true;
	}
	if (Value->Type == EJson::Array)
	{
		for (const TSharedPtr<FJsonValue>& Element : Value->AsArray())
		{
			if (!Element.IsValid() || Element->Type != EJson::String)
			{
				OutError = FString::Printf(TEXT("%s must be a list of strings"), Field);
				return false;
			}
			OutValues.Add(Element->AsString());
		}
		return true;
	}
	OutError = FString::Printf(TEXT("%s must be a list of strings"), Field);
	return false;
}

TSharedPtr<FJsonObject> HandleSync(const TSharedRef<FAssetDocumentService>& Service, const FString& RequestBody, const TMap<FString, FString>& QueryParams, int32& OutStatusCode)
{
	TSharedPtr<FJsonObject> Json;
	FString Error;
	if (!TryParseJsonBody(RequestBody, Json, Error))
	{
		OutStatusCode = 400;
		return MakeErrorJson(Error);
	}
	FString Action;
	TArray<FString> Targets;
	TArray<FString> Regions;
	if (!TryGetStringFieldAny(Json, {TEXT("action"), TEXT("Action")}, Action) || Action.IsEmpty())
	{
		OutStatusCode = 400;
		return MakeErrorJson(TEXT("Sync requires action (status, accept_sidecar, accept_asset or materialize)"));
	}
	if (!TryGetSyncStringList(Json, TEXT("targets"), Targets, Error) || !TryGetSyncStringList(Json, TEXT("regions"), Regions, Error))
	{
		OutStatusCode = 400;
		return MakeErrorJson(Error);
	}
	const FAssetDocumentResult Result = RunOnGameThread([Service, Action, Targets, Regions]()
	{
		return Service->SyncSidecars(Action, Targets, Regions);
	});
	OutStatusCode = GetStatusCode(Result);
	return Result.ToJson();
}

TSharedPtr<FJsonObject> HandleTemplate(const TSharedRef<FAssetDocumentService>& Service, const FString& RequestBody, const TMap<FString, FString>& QueryParams, int32& OutStatusCode)
{
	TSharedPtr<FJsonObject> Json;
	FString Error;
	if (!TryParseJsonBody(RequestBody, Json, Error))
	{
		OutStatusCode = 400;
		return MakeErrorJson(Error);
	}

	FAssetDocumentTemplateRequest TemplateRequest;
	if (!TryGetStringFieldAny(Json, {TEXT("Class"), TEXT("class")}, TemplateRequest.Class) || TemplateRequest.Class.IsEmpty())
	{
		OutStatusCode = 400;
		return MakeErrorJson(TEXT("CreateTemplate requires Class or class"));
	}
	if (!TryGetStringFieldAny(Json, {TEXT("Target"), TEXT("target")}, TemplateRequest.Target) || TemplateRequest.Target.IsEmpty())
	{
		OutStatusCode = 400;
		return MakeErrorJson(TEXT("CreateTemplate requires Target or target"));
	}

	const FAssetDocumentResult Result = RunOnGameThread([Service, TemplateRequest]()
	{
		return Service->CreateTemplate(TemplateRequest);
	});

	OutStatusCode = GetStatusCode(Result);
	return Result.ToJson();
}

TSharedPtr<FJsonObject> HandleExtract(const TSharedRef<FAssetDocumentService>& Service, const FString& RequestBody, const TMap<FString, FString>& QueryParams, int32& OutStatusCode)
{
	TSharedPtr<FJsonObject> Json;
	FString Error;
	if (!TryParseJsonBody(RequestBody, Json, Error))
	{
		OutStatusCode = 400;
		return MakeErrorJson(Error);
	}

	FAssetDocumentExtractRequest ExtractRequest;
	if (!TryGetStringFieldAny(Json, {TEXT("asset_path"), TEXT("AssetPath")}, ExtractRequest.AssetPath) || ExtractRequest.AssetPath.IsEmpty())
	{
		OutStatusCode = 400;
		return MakeErrorJson(TEXT("Extract requires asset_path or AssetPath"));
	}
	TryGetBoolFieldAny(Json, {TEXT("diff_only"), TEXT("DiffOnly")}, ExtractRequest.bDiffOnly);
	TryGetBoolFieldAny(Json, {TEXT("include_all_writable"), TEXT("IncludeAllWritable")}, ExtractRequest.bIncludeAllWritable);
	// The extract a client edits carries its BaseRevision (a stale apply of it is refused: Document.StaleBase).
	ExtractRequest.bIncludeBaseRevision = true;

	const FAssetDocumentResult Result = RunOnGameThread([Service, ExtractRequest]()
	{
		return Service->Extract(ExtractRequest);
	});

	OutStatusCode = GetStatusCode(Result);
	return Result.ToJson();
}

TSharedPtr<FJsonObject> HandleValidate(const TSharedRef<FAssetDocumentService>& Service, const FString& RequestBody, const TMap<FString, FString>& QueryParams, int32& OutStatusCode)
{
	TSharedPtr<FJsonObject> Json;
	FString Error;
	if (!TryParseJsonBody(RequestBody, Json, Error))
	{
		OutStatusCode = 400;
		return MakeErrorJson(Error);
	}

	FAssetDocumentValidateRequest ValidateRequest;
	if (!TryGetStringFieldAny(Json, {TEXT("file_path"), TEXT("FilePath")}, ValidateRequest.FilePath))
	{
		ValidateRequest.Document = Json;
	}

	const FAssetDocumentResult Result = RunOnGameThread([Service, ValidateRequest]()
	{
		return Service->Validate(ValidateRequest);
	});

	OutStatusCode = GetStatusCode(Result);
	return Result.ToJson();
}

TSharedPtr<FJsonObject> HandleDiff(const TSharedRef<FAssetDocumentService>& Service, const FString& RequestBody, const TMap<FString, FString>& QueryParams, int32& OutStatusCode)
{
	TSharedPtr<FJsonObject> Json;
	FString Error;
	if (!TryParseJsonBody(RequestBody, Json, Error))
	{
		OutStatusCode = 400;
		return MakeErrorJson(Error);
	}

	FAssetDocumentDiffRequest DiffRequest;
	if (!TryGetStringFieldAny(Json, {TEXT("file_path"), TEXT("FilePath")}, DiffRequest.FilePath))
	{
		DiffRequest.Document = Json;
	}

	const FAssetDocumentResult Result = RunOnGameThread([Service, DiffRequest]()
	{
		return Service->Diff(DiffRequest);
	});

	OutStatusCode = GetStatusCode(Result);
	return Result.ToJson();
}
}

FAssetDocumentHttpRoutes::FAssetDocumentHttpRoutes(const TSharedRef<FAssetDocumentService>& InService)
	: Service(InService)
{
}

FAssetDocumentHttpRoutes::~FAssetDocumentHttpRoutes()
{
	Unregister();
}

void FAssetDocumentHttpRoutes::Register()
{
	if (!RouteHandles.IsEmpty())
	{
		return;
	}

	auto RegisterRoute = [](const TCHAR* Path, FAssetDocumentExternalRouteHandler Handler, const TCHAR* MethodText, const TCHAR* Description, bool bRequiresBody, TArray<FDelegateHandle>& OutRouteHandles)
	{
		FAssetDocumentExternalRoute Route;
		Route.Path = Path;
		Route.Handler = Handler;
		Route.MethodText = MethodText;
		Route.Description = Description;
		Route.bRequiresBody = bRequiresBody;

		const FDelegateHandle Handle = FAssetDocumentHttpServer::RegisterExternalRoute(Route);
		if (Handle.IsValid())
		{
			OutRouteHandles.Add(Handle);
		}
	};

	RegisterRoute(TEXT("/assetdoc/assetdocument/apply-file"), CreateServiceRouteHandler(Service, HandleApplyFile), TEXT("POST"), TEXT("Apply an AssetDocument sidecar file"), true, RouteHandles);
	RegisterRoute(TEXT("/assetdoc/assetdocument/schema"), CreateServiceRouteHandler(Service, HandleSchema), TEXT("GET"), TEXT("Describe AssetDocument HTTP routes"), false, RouteHandles);
	RegisterRoute(TEXT("/assetdoc/assetdocument/inspect"), CreateServiceRouteHandler(Service, HandleInspect), TEXT("GET"), TEXT("Inspect writable reflected properties for a class or asset"), false, RouteHandles);
	RegisterRoute(TEXT("/assetdoc/assetdocument/profile"), CreateServiceRouteHandler(Service, HandleProfile), TEXT("GET"), TEXT("Inspect the AssetDocument profile for a class or asset"), false, RouteHandles);
	RegisterRoute(TEXT("/assetdoc/assetdocument/template"), CreateServiceRouteHandler(Service, HandleTemplate), TEXT("POST"), TEXT("Create a generic AssetDocument template"), true, RouteHandles);
	RegisterRoute(TEXT("/assetdoc/assetdocument/extract"), CreateServiceRouteHandler(Service, HandleExtract), TEXT("POST"), TEXT("Extract an asset as an AssetDocument JSON document"), true, RouteHandles);
	RegisterRoute(TEXT("/assetdoc/assetdocument/validate"), CreateServiceRouteHandler(Service, HandleValidate), TEXT("POST"), TEXT("Validate an AssetDocument JSON document or sidecar file"), true, RouteHandles);
	RegisterRoute(TEXT("/assetdoc/assetdocument/diff"), CreateServiceRouteHandler(Service, HandleDiff), TEXT("POST"), TEXT("Diff an AssetDocument JSON document or sidecar file against the current asset"), true, RouteHandles);
	RegisterRoute(TEXT("/assetdoc/assetdocument/find-assets"), CreateServiceRouteHandler(Service, HandleFindAssets), TEXT("GET"), TEXT("Find assets by path prefix, class and name (Asset Registry, paged)"), false, RouteHandles);
	RegisterRoute(TEXT("/assetdoc/assetdocument/list-types"), CreateServiceRouteHandler(Service, HandleListTypes), TEXT("GET"), TEXT("List classes, structs or enums (reflection and Asset Registry, paged)"), false, RouteHandles);
	RegisterRoute(TEXT("/assetdoc/assetdocument/duplicate"), CreateServiceRouteHandler(Service, HandleDuplicate), TEXT("POST"), TEXT("Duplicate an asset into an absent /Game path and save it"), true, RouteHandles);
	RegisterRoute(TEXT("/assetdoc/assetdocument/sync"), CreateServiceRouteHandler(Service, HandleSync), TEXT("POST"), TEXT("Sidecar mirroring: status, conflict resolution and materialize"), true, RouteHandles);
}

void FAssetDocumentHttpRoutes::Unregister()
{
	for (const FDelegateHandle& Handle : RouteHandles)
	{
		FAssetDocumentHttpServer::UnregisterExternalRoute(Handle);
	}
	RouteHandles.Reset();
}
