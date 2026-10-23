// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentGenericRuntimeRoute.h"

#include "AssetDocumentService.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TestDataAsset.h"
#include "UObject/Package.h"

#include <type_traits>
#include "AssetDocumentPluginPaths.h"

#if WITH_DEV_AUTOMATION_TESTS
static_assert(
	std::has_virtual_destructor_v<IAssetDocumentGenericRuntimeRoute>,
	"The generic route must remain safely weak-bound through its interface.");

namespace
{
class FCountingGenericRuntimeRoute final
	: public IAssetDocumentGenericRuntimeRoute
{
public:
	explicit FCountingGenericRuntimeRoute(FAssetDocumentResult InResult)
		: ApplyFileResult(InResult)
		, ValidateResult(MoveTemp(InResult))
	{
	}

	virtual FAssetDocumentResult DispatchApplyFile(
		const FAssetDocumentApplyFileRequest& Request) override
	{
		++ApplyFileDispatchCount;
		LastSourceDocumentPath = Request.FilePath;
		return ApplyFileResult;
	}

	FAssetDocumentResult DispatchValidate(
		const FAssetDocumentValidateRequest& Request) override
	{
		++ValidateDispatchCount;
		LastValidateRequest = Request;
		return ValidateResult;
	}

	FAssetDocumentRuntimeResultDto DispatchExtract(
		const FAssetDocumentExtractRequest& Request,
		const FString& AssetClassPath)
	{
		++ExtractDispatchCount;
		LastExtractRequest = Request;
		LastExtractClassPath = AssetClassPath;
		return ReadOnlyResult;
	}

	FAssetDocumentRuntimeResultDto DispatchDiff(
		const FAssetDocumentDiffRequest& Request)
	{
		++DiffDispatchCount;
		LastDiffRequest = Request;
		return ReadOnlyResult;
	}

	FAssetDocumentRuntimeResultDto DispatchInspectProfile(
		const FAssetDocumentProfileRequest& Request,
		const FString& AssetClassPath)
	{
		++ProfileDispatchCount;
		LastProfileRequest = Request;
		LastProfileClassPath = AssetClassPath;
		return ProfileResult;
	}

	FAssetDocumentRuntimeResultDto DispatchInspect(
		const FAssetDocumentInspectRequest& Request,
		const FString& AssetClassPath,
		const FString& AssetObjectPath)
	{
		++InspectDispatchCount;
		LastInspectRequest = Request;
		LastInspectClassPath = AssetClassPath;
		LastInspectObjectPath = AssetObjectPath;
		return InspectResult;
	}

	FAssetDocumentRuntimeResultDto DispatchCreateTemplate(
		const FAssetDocumentTemplateRequest& Request,
		const FString& NormalizedTarget,
		const FString& AssetClassPath)
	{
		++TemplateDispatchCount;
		LastTemplateRequest = Request;
		LastTemplateTarget = NormalizedTarget;
		LastTemplateClassPath = AssetClassPath;
		return TemplateResult;
	}

	void SetInspectResult(FAssetDocumentRuntimeResultDto InResult)
	{
		InspectResult = MoveTemp(InResult);
	}

	FAssetDocumentRuntimeResultDto& MutableProfileResult()
	{
		return ProfileResult;
	}

	int32 ApplyFileDispatchCount = 0;
	int32 ValidateDispatchCount = 0;
	int32 ExtractDispatchCount = 0;
	int32 DiffDispatchCount = 0;
	int32 ProfileDispatchCount = 0;
	int32 InspectDispatchCount = 0;
	int32 TemplateDispatchCount = 0;
	FString LastSourceDocumentPath;
	FString LastExtractClassPath;
	FString LastProfileClassPath;
	FString LastInspectClassPath;
	FString LastInspectObjectPath;
	FString LastTemplateTarget;
	FString LastTemplateClassPath;
	FAssetDocumentValidateRequest LastValidateRequest;
	FAssetDocumentExtractRequest LastExtractRequest;
	FAssetDocumentDiffRequest LastDiffRequest;
	FAssetDocumentProfileRequest LastProfileRequest;
	FAssetDocumentInspectRequest LastInspectRequest;
	FAssetDocumentTemplateRequest LastTemplateRequest;

private:
	static FAssetDocumentRuntimeResultDto MakeReadOnlyResult(const FString& Marker)
	{
		FAssetDocumentRuntimeResultDto Result;
		Result.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Succeeded;
		Result.FinalPhase = EAssetDocumentRuntimeRequestPhase::Complete;
		Result.CanonicalValue = FAssetDocumentRuntimeValueDto();
		Result.CanonicalValue->Kind = EAssetDocumentRuntimeValueKind::Object;
		Result.CanonicalValue->ObjectKeys.Add(TEXT("RouteMarker"));
		FAssetDocumentRuntimeValueDto& Value =
			Result.CanonicalValue->Elements.AddDefaulted_GetRef();
		Value.Kind = EAssetDocumentRuntimeValueKind::String;
		Value.StringValue = Marker;
		return Result;
	}

	FAssetDocumentResult ApplyFileResult;
	FAssetDocumentResult ValidateResult;
	FAssetDocumentRuntimeResultDto ReadOnlyResult = MakeReadOnlyResult(TEXT("ReadOnlyRoute"));
	FAssetDocumentRuntimeResultDto ProfileResult = MakeReadOnlyResult(TEXT("ProfileRoute"));
	FAssetDocumentRuntimeResultDto InspectResult = MakeReadOnlyResult(TEXT("InspectRoute"));
	FAssetDocumentRuntimeResultDto TemplateResult = MakeReadOnlyResult(TEXT("TemplateRoute"));
};

TSharedPtr<FJsonObject> MakeGenericApplyDocument(
	const FString& Target,
	const FString& Action,
	const FString& ClassPath =
		TEXT("/Script/AssetDocumentTests.TestActorBase"))
{
	TSharedPtr<FJsonObject> Document = MakeShared<FJsonObject>();
	Document->SetNumberField(TEXT("SchemaVersion"), 1);
	Document->SetStringField(TEXT("AssetType"), TEXT("GenericAsset"));
	Document->SetStringField(TEXT("Target"), Target);
	Document->SetStringField(
		TEXT("Class"),
		ClassPath);
	Document->SetStringField(TEXT("Action"), Action);
	Document->SetObjectField(TEXT("Properties"), MakeShared<FJsonObject>());
	return Document;
}

TSharedPtr<FJsonObject> MakeGenericValidateDocument(
	const FString& Target,
	const FString& ClassPath = TEXT("/Script/AssetDocumentTests.TestDataAsset"))
{
	TSharedPtr<FJsonObject> Document = MakeShared<FJsonObject>();
	Document->SetNumberField(TEXT("SchemaVersion"), 1);
	Document->SetStringField(TEXT("AssetType"), TEXT("GenericAsset"));
	Document->SetStringField(TEXT("Target"), Target);
	Document->SetStringField(TEXT("Class"), ClassPath);
	Document->SetStringField(TEXT("Action"), TEXT("Create"));
	Document->SetObjectField(TEXT("Body"), MakeShared<FJsonObject>());
	return Document;
}

class FTemporaryGenericApplySidecar final
{
public:
	explicit FTemporaryGenericApplySidecar(const TSharedPtr<FJsonObject>& Document)
	{
		if (!Document.IsValid() || !Document->TryGetStringField(TEXT("Target"), Target))
		{
			return;
		}

		SidecarPath = FPackageName::LongPackageNameToFilename(
			Target,
			TEXT(".assetdoc.json"));
		SourceDocumentPath = FPaths::ConvertRelativePathToFull(SidecarPath);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(SidecarPath), true);

		FString Json;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
		bIsReady = FJsonSerializer::Serialize(Document.ToSharedRef(), Writer)
			&& FFileHelper::SaveStringToFile(Json, *SidecarPath);
	}

	~FTemporaryGenericApplySidecar()
	{
		if (!SidecarPath.IsEmpty())
		{
			IFileManager::Get().Delete(*SidecarPath, false, true);
		}
	}

	bool IsReady() const
	{
		return bIsReady;
	}

	const FString& GetSourceDocumentPath() const
	{
		return SourceDocumentPath;
	}

	FAssetDocumentResult Apply(
		FAssetDocumentService& Service,
		const bool bSaveAsset) const
	{
		FAssetDocumentApplyFileRequest Request;
		Request.FilePath = SidecarPath;
		Request.bSaveAsset = bSaveAsset;
		return Service.ApplyFile(Request);
	}

private:
	FString Target;
	FString SidecarPath;
	FString SourceDocumentPath;
	bool bIsReady = false;
};

FString FirstDiagnosticCode(const FAssetDocumentResult& Result)
{
	return Result.Diagnostics.IsEmpty()
		? FString()
		: Result.Diagnostics[0].Code;
}

FString LoadRouteTestPluginSource(
	FAutomationTestBase& Test,
	const TCHAR* RelativePath)
{
	const FString Path = FPaths::Combine(
		GetAssetDocumentPluginBaseDir(),
		RelativePath);
	FString Source;
	if (!FFileHelper::LoadFileToString(Source, *Path))
	{
		Test.AddError(FString::Printf(
			TEXT("Failed to read plugin source file '%s'"),
			*Path));
	}
	return Source;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericRuntimeWeakRouteDispatchTest,
	"UEAssetDoc.AssetDocumentGenericRuntimeRouteTests.WeakDispatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericRuntimeWeakRouteDispatchTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const FString Target =
		TEXT("/Game/AssetDocumentGenericRuntimeRouteTests/DA_WeakDispatch");
	FAssetDocumentResult RouteFailure =
		FAssetDocumentResult::Failure(TEXT("Test route rejected apply"));
	FAssetDocumentDiagnostic RouteDiagnostic;
	RouteDiagnostic.Path = TEXT("/GenericRuntime");
	RouteDiagnostic.Code = TEXT("Test.RouteFailure");
	RouteDiagnostic.Message = RouteFailure.Message;
	RouteFailure.Diagnostics.Add(RouteDiagnostic);

	FAssetDocumentService Service;
	TSharedRef<FCountingGenericRuntimeRoute> Route =
		MakeShared<FCountingGenericRuntimeRoute>(MoveTemp(RouteFailure));
	Service.BindGenericRuntimeRoute(Route);

	FTemporaryGenericApplySidecar Sidecar(
		MakeGenericApplyDocument(Target, TEXT("Create")));
	TestTrue(TEXT("writes WeakDispatch canonical sidecar"), Sidecar.IsReady());
	TestFalse(
		TEXT("WeakDispatch sidecar has normalized source path"),
		Sidecar.GetSourceDocumentPath().IsEmpty());
	const FAssetDocumentResult Result = Sidecar.Apply(Service, false);

	TestEqual(TEXT("unregistered class dispatches route once"), Route->ApplyFileDispatchCount, 1);
	TestEqual(
		TEXT("route receives WeakDispatch normalized source path"),
		Route->LastSourceDocumentPath,
		Sidecar.GetSourceDocumentPath());
	TestEqual(
		TEXT("route failure is returned without legacy fallback"),
		FirstDiagnosticCode(Result),
		FString(TEXT("Test.RouteFailure")));
	TestNull(
		TEXT("route failure does not create a legacy generic package"),
		FindPackage(nullptr, *Target));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericRuntimeMissingRouteTest,
	"UEAssetDoc.AssetDocumentGenericRuntimeRouteTests.MissingRoute",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericRuntimeMissingRouteTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FAssetDocumentService Service;
	{
		TSharedRef<FCountingGenericRuntimeRoute> Route =
			MakeShared<FCountingGenericRuntimeRoute>(
				FAssetDocumentResult::Failure(TEXT("expired route")));
		Service.BindGenericRuntimeRoute(Route);
	}

	FTemporaryGenericApplySidecar Sidecar(MakeGenericApplyDocument(
		TEXT("/Game/AssetDocumentGenericRuntimeRouteTests/DA_MissingRoute"),
		TEXT("Update")));
	TestTrue(TEXT("writes MissingRoute canonical sidecar"), Sidecar.IsReady());
	TestFalse(
		TEXT("MissingRoute sidecar has normalized source path"),
		Sidecar.GetSourceDocumentPath().IsEmpty());
	const FAssetDocumentResult Result = Sidecar.Apply(Service, false);

	TestFalse(TEXT("missing generic route fails closed"), Result.IsSuccess());
	TestEqual(
		TEXT("missing route has stable diagnostic"),
		FirstDiagnosticCode(Result),
		FString(TEXT("GenericRuntime.Unavailable")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBlackboardDataSoleAuthoritativeRouteTest,
	"UEAssetDoc.Python.BlackboardData.SoleAuthoritativeRoute",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBlackboardDataSoleAuthoritativeRouteTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const FString ModuleSource = LoadRouteTestPluginSource(
		*this,
		TEXT("Source/AssetDocument/Private/AssetDocumentModule.cpp"));
	const FString RuntimeSource = LoadRouteTestPluginSource(
		*this,
		TEXT("Content/Python/asset_document/runtime.py"));
	const FString ProfileSource = LoadRouteTestPluginSource(
		*this,
		TEXT("Content/Python/asset_document/profiles/blackboard_data.py"));
	TestFalse(
		TEXT("BlackboardData has no C++ Profile registration"),
		ModuleSource.Contains(TEXT("FBlackboardDataAssetDocumentProfile")));
	TestTrue(
		TEXT("runtime resolves exact profiles through the frozen registry"),
		RuntimeSource.Contains(TEXT("ProfileRegistryBuilder")));
	TestTrue(
		TEXT("Blackboard candidate exports production ProfileMeta"),
		ProfileSource.Contains(TEXT("ProfileMeta")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentApplyFileReentersApplyRouteTest,
	"UEAssetDoc.AssetDocumentGenericRuntimeRouteTests.ApplyFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentApplyFileReentersApplyRouteTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const FString TestDirectory = FPaths::Combine(
		FPaths::ProjectContentDir(),
		TEXT("AssetDocumentGenericRuntimeRouteTests"));
	const FString SidecarPath = FPaths::Combine(
		TestDirectory,
		TEXT("DA_ApplyFile.assetdoc.json"));
	const FString Target =
		TEXT("/Game/AssetDocumentGenericRuntimeRouteTests/DA_ApplyFile");
	IFileManager::Get().MakeDirectory(*TestDirectory, true);
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*SidecarPath, false, true);
	};
	const FString Json = TEXT(
		"{\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\","
		"\"Target\":\"/Game/AssetDocumentGenericRuntimeRouteTests/DA_ApplyFile\","
		"\"Class\":\"/Script/AssetDocumentTests.TestActorBase\","
		"\"Action\":\"Update\",\"Properties\":{}}");
	TestTrue(
		TEXT("writes ApplyFile route fixture"),
		FFileHelper::SaveStringToFile(Json, *SidecarPath));

	FAssetDocumentResult RouteFailure =
		FAssetDocumentResult::Failure(TEXT("ApplyFile route terminal"));
	FAssetDocumentDiagnostic RouteDiagnostic;
	RouteDiagnostic.Code = TEXT("Test.ApplyFileRoute");
	RouteFailure.Diagnostics.Add(RouteDiagnostic);
	FAssetDocumentService Service;
	TSharedRef<FCountingGenericRuntimeRoute> Route =
		MakeShared<FCountingGenericRuntimeRoute>(MoveTemp(RouteFailure));
	Service.BindGenericRuntimeRoute(Route);

	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = SidecarPath;
	Request.bSaveAsset = true;
	const FAssetDocumentResult Result = Service.ApplyFile(Request);

	TestEqual(
		TEXT("ApplyFile re-enters the generic Apply decision once"),
		Route->ApplyFileDispatchCount,
		1);
	TestEqual(
		TEXT("ApplyFile preserves normalized source path"),
		Route->LastSourceDocumentPath,
		FPaths::ConvertRelativePathToFull(SidecarPath));
	TestEqual(
		TEXT("ApplyFile returns route terminal without legacy fallback"),
		FirstDiagnosticCode(Result),
		FString(TEXT("Test.ApplyFileRoute")));
	TestNull(
		TEXT("ApplyFile route failure does not create a package"),
		FindPackage(nullptr, *Target));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericRuntimeValidateDispatchTest,
	"UEAssetDoc.AssetDocumentGenericRuntimeRouteTests.ValidateDispatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericRuntimeValidateDispatchTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FAssetDocumentResult RouteTerminal =
		FAssetDocumentResult::Success(TEXT("generic Validate route terminal"));

	FAssetDocumentService Service;
	TSharedRef<FCountingGenericRuntimeRoute> Route =
		MakeShared<FCountingGenericRuntimeRoute>(RouteTerminal);
	Service.BindGenericRuntimeRoute(Route);

	FAssetDocumentValidateRequest Request;
	Request.Document = MakeGenericValidateDocument(
		TEXT("/Game/AssetDocumentGenericRuntimeRouteTests/DA_ValidateDispatch"));
	const FAssetDocumentResult Result = Service.Validate(Request);

	TestEqual(
		TEXT("unregistered Validate dispatches generic route once"),
		Route->ValidateDispatchCount,
		1);
	TestEqual(
		TEXT("generic Validate returns the route terminal"),
		Result.Message,
		FString(TEXT("generic Validate route terminal")));
	TestNotEqual(
		TEXT("generic Validate does not fall back to MissingProfile"),
		FirstDiagnosticCode(Result),
		FString(TEXT("MissingProfile")));
	TestFalse(TEXT("generic Validate never saves an asset"), Result.bSavedAsset);
	TestFalse(TEXT("generic Validate never writes a sidecar"), Result.bWroteSidecar);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericRuntimeValidateMissingRouteTest,
	"UEAssetDoc.AssetDocumentGenericRuntimeRouteTests.ValidateMissingRoute",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericRuntimeValidateMissingRouteTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FAssetDocumentService Service;
	FAssetDocumentValidateRequest Request;
	Request.Document = MakeGenericValidateDocument(
		TEXT("/Game/AssetDocumentGenericRuntimeRouteTests/DA_ValidateMissingRoute"));
	const FAssetDocumentResult Result = Service.Validate(Request);

	TestFalse(TEXT("missing generic Validate route fails closed"), Result.IsSuccess());
	TestEqual(
		TEXT("missing generic Validate route has stable diagnostic"),
		FirstDiagnosticCode(Result),
		FString(TEXT("GenericRuntime.Unavailable")));
	TestNotEqual(
		TEXT("missing generic Validate route does not use legacy fallback"),
		FirstDiagnosticCode(Result),
		FString(TEXT("MissingProfile")));
	TestFalse(TEXT("missing generic Validate route never saves an asset"), Result.bSavedAsset);
	TestFalse(TEXT("missing generic Validate route never writes a sidecar"), Result.bWroteSidecar);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericRuntimeValidateExpiredRouteTest,
	"UEAssetDoc.AssetDocumentGenericRuntimeRouteTests.ValidateExpiredRoute",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericRuntimeValidateExpiredRouteTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FAssetDocumentService Service;
	{
		TSharedRef<FCountingGenericRuntimeRoute> Route =
			MakeShared<FCountingGenericRuntimeRoute>(
				FAssetDocumentResult::Failure(TEXT("expired Validate route")));
		Service.BindGenericRuntimeRoute(Route);
	}

	FAssetDocumentValidateRequest Request;
	Request.Document = MakeGenericValidateDocument(
		TEXT("/Game/AssetDocumentGenericRuntimeRouteTests/DA_ValidateExpiredRoute"));
	const FAssetDocumentResult Result = Service.Validate(Request);

	TestFalse(TEXT("expired generic Validate route fails closed"), Result.IsSuccess());
	TestEqual(
		TEXT("expired generic Validate route has stable diagnostic"),
		FirstDiagnosticCode(Result),
		FString(TEXT("GenericRuntime.Unavailable")));
	TestNotEqual(
		TEXT("expired generic Validate route does not use legacy fallback"),
		FirstDiagnosticCode(Result),
		FString(TEXT("MissingProfile")));
	TestFalse(TEXT("expired generic Validate route never saves an asset"), Result.bSavedAsset);
	TestFalse(TEXT("expired generic Validate route never writes a sidecar"), Result.bWroteSidecar);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericRuntimeValidateFailureTest,
	"UEAssetDoc.AssetDocumentGenericRuntimeRouteTests.ValidateFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericRuntimeValidateFailureTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FAssetDocumentResult RouteFailure =
		FAssetDocumentResult::Failure(TEXT("generic Validate route failure"));
	FAssetDocumentDiagnostic RouteDiagnostic;
	RouteDiagnostic.Code = TEXT("Test.ValidateRouteFailure");
	RouteFailure.Diagnostics.Add(RouteDiagnostic);

	FAssetDocumentService Service;
	TSharedRef<FCountingGenericRuntimeRoute> Route =
		MakeShared<FCountingGenericRuntimeRoute>(MoveTemp(RouteFailure));
	Service.BindGenericRuntimeRoute(Route);

	FAssetDocumentValidateRequest Request;
	Request.Document = MakeGenericValidateDocument(
		TEXT("/Game/AssetDocumentGenericRuntimeRouteTests/DA_ValidateFailure"));
	const FAssetDocumentResult Result = Service.Validate(Request);

	TestEqual(
		TEXT("generic Validate failure dispatches route once"),
		Route->ValidateDispatchCount,
		1);
	TestEqual(
		TEXT("generic Validate failure is returned unchanged"),
		FirstDiagnosticCode(Result),
		FString(TEXT("Test.ValidateRouteFailure")));
	TestFalse(TEXT("generic Validate failure never saves an asset"), Result.bSavedAsset);
	TestFalse(TEXT("generic Validate failure never writes a sidecar"), Result.bWroteSidecar);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericRuntimeReadOnlyDispatchTest,
	"UEAssetDoc.AssetDocumentGenericRuntimeRouteTests.ReadOnlyDispatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericRuntimeReadOnlyDispatchTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const FString Target =
		TEXT("/Game/AssetDocumentGenericRuntimeRouteTests/DA_ReadOnlyDispatch");
	UPackage* const Package = CreatePackage(*Target);
	UTestDataAsset* const Asset = NewObject<UTestDataAsset>(
		Package,
		TEXT("DA_ReadOnlyDispatch"),
		RF_Public | RF_Standalone);
	TestNotNull(TEXT("creates test-owned generic read-only fixture"), Asset);
	if (Asset == nullptr)
	{
		return false;
	}

	FAssetDocumentResult RouteTerminal =
		FAssetDocumentResult::Success(TEXT("generic read-only route terminal"));
	FAssetDocumentService Service;
	TSharedRef<FCountingGenericRuntimeRoute> Route =
		MakeShared<FCountingGenericRuntimeRoute>(RouteTerminal);
	Service.BindGenericRuntimeRoute(Route);

	FAssetDocumentExtractRequest ExtractRequest;
	ExtractRequest.AssetPath = Target;
	ExtractRequest.bDiffOnly = false;
	ExtractRequest.bIncludeAllWritable = true;
	const FAssetDocumentResult Extract = Service.Extract(ExtractRequest);

	TestEqual(
		TEXT("unregistered Extract dispatches generic route once"),
		Route->ExtractDispatchCount,
		1);
	TestEqual(
		TEXT("generic Extract returns the route terminal"),
		Extract.Message,
		FString(TEXT("AssetDocument extracted")));
	TestEqual(
		TEXT("generic Extract binds the exact resolved class path"),
		Route->LastExtractClassPath,
		UTestDataAsset::StaticClass()->GetPathName());

	FAssetDocumentDiffRequest DiffRequest;
	DiffRequest.Document = MakeGenericValidateDocument(Target);
	const FAssetDocumentResult Diff = Service.Diff(DiffRequest);

	TestEqual(
		TEXT("unregistered Diff dispatches generic route once"),
		Route->DiffDispatchCount,
		1);
	TestEqual(
		TEXT("generic Diff returns the route terminal"),
		Diff.Message,
		FString(TEXT("AssetDocument diffed")));
	TestFalse(TEXT("generic Extract never saves an asset"), Extract.bSavedAsset);
	TestFalse(TEXT("generic Extract never writes a sidecar"), Extract.bWroteSidecar);
	TestFalse(TEXT("generic Diff never saves an asset"), Diff.bSavedAsset);
	TestFalse(TEXT("generic Diff never writes a sidecar"), Diff.bWroteSidecar);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericRuntimeProfileRouteTest,
	"UEAssetDoc.AssetDocumentGenericRuntimeRouteTests.ProfileRoute",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericRuntimeProfileRouteTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FAssetDocumentService Service;
	TSharedRef<FCountingGenericRuntimeRoute> Route =
		MakeShared<FCountingGenericRuntimeRoute>(
			FAssetDocumentResult::Failure(TEXT("unused apply route")));
	Service.BindGenericRuntimeRoute(Route);

	FAssetDocumentProfileRequest Request;
	Request.ClassOrAsset = TEXT("/Script/AssetDocumentTests.TestDataAsset");
	const FAssetDocumentResult Result = Service.InspectProfile(Request);

	FString Marker;
	TestEqual(
		TEXT("unregistered InspectProfile dispatches generic route once"),
		Route->ProfileDispatchCount,
		1);
	TestEqual(
		TEXT("InspectProfile dispatches the resolved class"),
		Route->LastProfileClassPath,
		UTestDataAsset::StaticClass()->GetPathName());
	TestTrue(TEXT("InspectProfile projects route payload"), Result.Payload.IsValid());
	TestTrue(
		TEXT("InspectProfile projects route marker"),
		Result.Payload.IsValid()
			&& Result.Payload->TryGetStringField(TEXT("RouteMarker"), Marker));
	TestEqual(TEXT("InspectProfile preserves route marker"), Marker, FString(TEXT("ProfileRoute")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericRuntimeProfileRegistrationDebugOnlyTest,
	"UEAssetDoc.AssetDocumentGenericRuntimeRouteTests.ProfileRuntimeRegistrationDebugOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericRuntimeProfileRegistrationDebugOnlyTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FAssetDocumentService Service;
	TSharedRef<FCountingGenericRuntimeRoute> Route =
		MakeShared<FCountingGenericRuntimeRoute>(
			FAssetDocumentResult::Failure(TEXT("unused apply route")));
	Route->MutableProfileResult().CanonicalValue->ObjectKeys.Add(TEXT("RuntimeRegistration"));
	FAssetDocumentRuntimeValueDto& Registration = Route->MutableProfileResult().CanonicalValue->Elements.AddDefaulted_GetRef();
	Registration.Kind = EAssetDocumentRuntimeValueKind::String;
	Registration.StringValue = TEXT("registration");
	Service.BindGenericRuntimeRoute(Route);

	FAssetDocumentProfileRequest Request;
	Request.ClassOrAsset = TEXT("/Script/AssetDocumentTests.TestDataAsset");
	const FAssetDocumentResult Default = Service.InspectProfile(Request);
	TestTrue(TEXT("default profile payload keeps the profile fields"),
		Default.Payload.IsValid() && Default.Payload->HasField(TEXT("RouteMarker")));
	TestFalse(TEXT("default profile payload omits RuntimeRegistration"),
		Default.Payload.IsValid() && Default.Payload->HasField(TEXT("RuntimeRegistration")));

	Request.bIncludeRuntimeRegistration = true;
	const FAssetDocumentResult Debug = Service.InspectProfile(Request);
	TestTrue(TEXT("debug profile payload keeps RuntimeRegistration"),
		Debug.Payload.IsValid() && Debug.Payload->HasField(TEXT("RuntimeRegistration")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericRuntimeInspectRouteTest,
	"UEAssetDoc.GenericRuntime.Inspect.Route",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericRuntimeInspectRouteTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FAssetDocumentService Service;
	TSharedRef<FCountingGenericRuntimeRoute> Route =
		MakeShared<FCountingGenericRuntimeRoute>(
			FAssetDocumentResult::Failure(TEXT("unused apply route")));
	Service.BindGenericRuntimeRoute(Route);

	FAssetDocumentInspectRequest ClassRequest;
	ClassRequest.ClassOrAsset = UTestDataAsset::StaticClass()->GetPathName();
	const FAssetDocumentResult ClassResult = Service.Inspect(ClassRequest);
	TestEqual(
		TEXT("generic class Inspect dispatches exactly once"),
		Route->InspectDispatchCount,
		1);
	TestEqual(
		TEXT("generic class Inspect uses its exact class path"),
		Route->LastInspectClassPath,
		UTestDataAsset::StaticClass()->GetPathName());
	TestTrue(
		TEXT("generic class Inspect has no live object path"),
		Route->LastInspectObjectPath.IsEmpty());
	TestTrue(TEXT("generic class Inspect projects the sole route payload"),
		ClassResult.Payload.IsValid());

	const FString Target =
		TEXT("/Game/AssetDocumentGenericRuntimeRouteTests/DA_InspectLive");
	UPackage* const Package = CreatePackage(*Target);
	UTestDataAsset* const Asset = NewObject<UTestDataAsset>(
		Package,
		TEXT("DA_InspectLive"),
		RF_Public | RF_Standalone);
	TestNotNull(TEXT("creates test-owned generic live Inspect fixture"), Asset);
	if (Asset == nullptr)
	{
		return false;
	}
	FAssetDocumentInspectRequest LiveRequest;
	LiveRequest.ClassOrAsset = Asset->GetPathName();
	const FAssetDocumentResult LiveResult = Service.Inspect(LiveRequest);
	TestEqual(
		TEXT("generic live Inspect dispatches exactly once"),
		Route->InspectDispatchCount,
		2);
	TestEqual(
		TEXT("generic live Inspect uses its exact class path"),
		Route->LastInspectClassPath,
		UTestDataAsset::StaticClass()->GetPathName());
	TestEqual(
		TEXT("generic live Inspect uses its exact object path"),
		Route->LastInspectObjectPath,
		Asset->GetPathName());
	TestTrue(TEXT("generic live Inspect projects the sole route payload"),
		LiveResult.Payload.IsValid());

	FAssetDocumentRuntimeResultDto RouteFailure;
	RouteFailure.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Failed;
	RouteFailure.FinalPhase = EAssetDocumentRuntimeRequestPhase::Failed;
	FAssetDocumentRuntimeDiagnostic& FailureDiagnostic =
		RouteFailure.Diagnostics.AddDefaulted_GetRef();
	FailureDiagnostic.Code = TEXT("Test.InspectRouteFailure");
	Route->SetInspectResult(MoveTemp(RouteFailure));
	const FAssetDocumentResult FailedResult = Service.Inspect(ClassRequest);
	TestEqual(
		TEXT("generic Inspect runtime failure dispatches only once"),
		Route->InspectDispatchCount,
		3);
	TestEqual(
		TEXT("generic Inspect runtime failure has no legacy fallback"),
		FirstDiagnosticCode(FailedResult),
		FString(TEXT("Test.InspectRouteFailure")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericRuntimeInspectMissingRouteTest,
	"UEAssetDoc.GenericRuntime.Inspect.MissingRoute",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericRuntimeInspectMissingRouteTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FAssetDocumentService Service;
	FAssetDocumentInspectRequest Request;
	Request.ClassOrAsset = UTestDataAsset::StaticClass()->GetPathName();
	const FAssetDocumentResult Result = Service.Inspect(Request);
	TestFalse(TEXT("missing generic Inspect route fails closed"), Result.IsSuccess());
	TestEqual(
		TEXT("missing generic Inspect route has stable diagnostic"),
		FirstDiagnosticCode(Result),
		FString(TEXT("GenericRuntime.Unavailable")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericRuntimeProfileMissingRouteTest,
	"UEAssetDoc.AssetDocumentGenericRuntimeRouteTests.ProfileMissingRoute",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericRuntimeProfileMissingRouteTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FAssetDocumentService Service;
	FAssetDocumentProfileRequest Request;
	Request.ClassOrAsset = TEXT("/Script/AssetDocumentTests.TestDataAsset");
	const FAssetDocumentResult Result = Service.InspectProfile(Request);

	TestFalse(TEXT("missing InspectProfile route fails closed"), Result.IsSuccess());
	TestEqual(
		TEXT("missing InspectProfile route has stable diagnostic"),
		FirstDiagnosticCode(Result),
		FString(TEXT("GenericRuntime.Unavailable")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericRuntimeTemplateRouteTest,
	"UEAssetDoc.AssetDocumentGenericRuntimeRouteTests.TemplateRoute",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericRuntimeTemplateRouteTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const FString Target =
		TEXT("/Game/AssetDocumentGenericRuntimeRouteTests/DA_TemplateRoute");
	FAssetDocumentService Service;
	TSharedRef<FCountingGenericRuntimeRoute> Route =
		MakeShared<FCountingGenericRuntimeRoute>(
			FAssetDocumentResult::Failure(TEXT("unused apply route")));
	Service.BindGenericRuntimeRoute(Route);

	FAssetDocumentTemplateRequest Request;
	Request.Target = Target + TEXT(".DA_TemplateRoute");
	Request.Class = TEXT("/Script/AssetDocumentTests.TestDataAsset");
	const FAssetDocumentResult Result = Service.CreateTemplate(Request);

	FString Marker;
	TestEqual(
		TEXT("unregistered CreateTemplate dispatches generic route once"),
		Route->TemplateDispatchCount,
		1);
	TestEqual(TEXT("CreateTemplate normalizes route target"), Route->LastTemplateTarget, Target);
	TestEqual(
		TEXT("CreateTemplate dispatches the resolved class"),
		Route->LastTemplateClassPath,
		UTestDataAsset::StaticClass()->GetPathName());
	TestTrue(TEXT("CreateTemplate projects route payload"), Result.Payload.IsValid());
	TestTrue(
		TEXT("CreateTemplate projects route marker"),
		Result.Payload.IsValid()
			&& Result.Payload->TryGetStringField(TEXT("RouteMarker"), Marker));
	TestEqual(TEXT("CreateTemplate preserves route marker"), Marker, FString(TEXT("TemplateRoute")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericRuntimeTemplateMissingRouteTest,
	"UEAssetDoc.AssetDocumentGenericRuntimeRouteTests.TemplateMissingRoute",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericRuntimeTemplateMissingRouteTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FAssetDocumentService Service;
	FAssetDocumentTemplateRequest Request;
	Request.Target = TEXT("/Game/AssetDocumentGenericRuntimeRouteTests/DA_TemplateMissingRoute");
	Request.Class = TEXT("/Script/AssetDocumentTests.TestDataAsset");
	const FAssetDocumentResult Result = Service.CreateTemplate(Request);

	TestFalse(TEXT("missing CreateTemplate route fails closed"), Result.IsSuccess());
	TestEqual(
		TEXT("missing CreateTemplate route has stable diagnostic"),
		FirstDiagnosticCode(Result),
		FString(TEXT("GenericRuntime.Unavailable")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentSchemaAdvertisesFileOnlyMutationTest,
	"UEAssetDoc.AssetDocumentGenericRuntimeRouteTests.SchemaAdvertisesFileOnlyMutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSchemaAdvertisesFileOnlyMutationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FAssetDocumentService Service;
	const FAssetDocumentResult Result = Service.GetSchema();
	TestTrue(TEXT("GetSchema succeeds"), Result.IsSuccess());
	TestTrue(TEXT("GetSchema returns its payload"), Result.Payload.IsValid());
	if (!Result.Payload.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
	TestTrue(
		TEXT("GetSchema exposes asset_document_tools"),
		Result.Payload->TryGetArrayField(TEXT("asset_document_tools"), Tools));
	if (!Tools)
	{
		return false;
	}

	bool bHasApplyFile = false;
	bool bHasObsoleteInlineApply = false;
	for (const TSharedPtr<FJsonValue>& Tool : *Tools)
	{
		FString ToolName;
		if (Tool.IsValid() && Tool->TryGetString(ToolName))
		{
			bHasApplyFile |= ToolName == TEXT("apply_asset_document_file");
			bHasObsoleteInlineApply |= ToolName == TEXT("apply_asset_document");
		}
	}

	TestTrue(TEXT("schema advertises file-backed mutation"), bHasApplyFile);
	TestFalse(TEXT("schema omits obsolete inline mutation"), bHasObsoleteInlineApply);
	return true;
}
#endif
