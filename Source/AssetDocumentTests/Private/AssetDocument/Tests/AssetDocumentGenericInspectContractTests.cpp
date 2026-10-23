// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentGenericInspectContractTestTypes.h"

#include "AssetDocumentService.h"
#include "Misc/AutomationTest.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"

#include <initializer_list>

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
FAssetDocumentRuntimeValueDto MakeString(const TCHAR* Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = Value;
	return Result;
}

FAssetDocumentRuntimeValueDto MakeArray()
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Array;
	return Result;
}

FAssetDocumentRuntimeValueDto MakeObject(
	std::initializer_list<TPair<FString, FAssetDocumentRuntimeValueDto>> Fields)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Object;
	for (const TPair<FString, FAssetDocumentRuntimeValueDto>& Field : Fields)
	{
		Result.ObjectKeys.Add(Field.Key);
		Result.Elements.Add(Field.Value);
	}
	return Result;
}

class FGenericInspectContractRoute final : public IAssetDocumentGenericRuntimeRoute
{
public:
	FAssetDocumentResult DispatchApplyFile(
		const FAssetDocumentApplyFileRequest& Request) override
	{
		(void)Request;
		return FAssetDocumentResult::Failure(TEXT("unused"));
	}

	FAssetDocumentResult DispatchValidate(
		const FAssetDocumentValidateRequest& Request) override
	{
		(void)Request;
		return FAssetDocumentResult::Failure(TEXT("unused"));
	}

	FAssetDocumentRuntimeResultDto DispatchExtract(
		const FAssetDocumentExtractRequest& Request,
		const FString& AssetClassPath) override
	{
		(void)Request;
		(void)AssetClassPath;
		return {};
	}

	FAssetDocumentRuntimeResultDto DispatchDiff(
		const FAssetDocumentDiffRequest& Request) override
	{
		(void)Request;
		return {};
	}

	FAssetDocumentRuntimeResultDto DispatchInspectProfile(
		const FAssetDocumentProfileRequest& Request,
		const FString& AssetClassPath) override
	{
		(void)Request;
		(void)AssetClassPath;
		return {};
	}

	FAssetDocumentRuntimeResultDto DispatchInspect(
		const FAssetDocumentInspectRequest& Request,
		const FString& AssetClassPath,
		const FString& AssetObjectPath) override
	{
		(void)Request;
		(void)AssetObjectPath;
		++InspectDispatchCount;
		FAssetDocumentRuntimeResultDto Result;
		Result.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Succeeded;
		Result.FinalPhase = EAssetDocumentRuntimeRequestPhase::Verify;
		FAssetDocumentRuntimeValueDto Properties = MakeArray();
		Properties.Elements.Add(MakeObject({
			{TEXT("name"), MakeString(TEXT("DisplayText"))},
			{TEXT("type_token"), MakeString(TEXT("Text"))},
			{TEXT("current_value"), MakeString(TEXT("display text"))},
			{TEXT("default_value"), MakeString(TEXT("display text"))},
		}));
		Result.CanonicalValue = MakeObject({
			{TEXT("class"), MakeString(*AssetClassPath)},
			{TEXT("properties"), MoveTemp(Properties)},
			{TEXT("skipped"), MakeArray()},
			{TEXT("selector_policies"), MakeArray()},
		});
		return Result;
	}

	FAssetDocumentRuntimeResultDto DispatchCreateTemplate(
		const FAssetDocumentTemplateRequest& Request,
		const FString& NormalizedTarget,
		const FString& AssetClassPath) override
	{
		(void)Request;
		(void)NormalizedTarget;
		(void)AssetClassPath;
		return {};
	}

	int32 InspectDispatchCount = 0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericInspectPublicContractTest,
	"UEAssetDoc.GenericRuntime.Inspect.Contract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericInspectPublicContractTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FAssetDocumentService Service;
	TSharedRef<FGenericInspectContractRoute> Route =
		MakeShared<FGenericInspectContractRoute>();
	Service.BindGenericRuntimeRoute(Route);

	FAssetDocumentInspectRequest Request;
	Request.ClassOrAsset =
		UAssetDocumentGenericInspectContractTestDataAsset::StaticClass()->GetPathName();
	const FAssetDocumentResult Result = Service.Inspect(Request);
	TestTrue(TEXT("generic public Inspect succeeds"), Result.IsSuccess());
	TestEqual(TEXT("generic public Inspect dispatches once"), Route->InspectDispatchCount, 1);
	TestTrue(TEXT("generic public Inspect has a payload"), Result.Payload.IsValid());
	if (!Result.Payload.IsValid())
	{
		return false;
	}
	TestTrue(TEXT("public payload has class"), Result.Payload->HasField(TEXT("class")));
	TestFalse(
		TEXT("class-only public payload omits asset_path"),
		Result.Payload->HasField(TEXT("asset_path")));
	TestTrue(TEXT("public payload has properties"), Result.Payload->HasField(TEXT("properties")));
	TestTrue(TEXT("public payload has skipped"), Result.Payload->HasField(TEXT("skipped")));
	TestTrue(
		TEXT("public payload has selector_policies"),
		Result.Payload->HasField(TEXT("selector_policies")));
	const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
	TestTrue(
		TEXT("public payload preserves the FText property projection"),
		Result.Payload->TryGetArrayField(TEXT("properties"), Properties)
			&& Properties != nullptr
			&& Properties->Num() == 1
			&& (*Properties)[0]->AsObject()->GetStringField(TEXT("type_token"))
				== TEXT("Text"));
	const TArray<TSharedPtr<FJsonValue>>* SelectorPolicies = nullptr;
	TestTrue(
		TEXT("generic fixture has no selector policies"),
		Result.Payload->TryGetArrayField(TEXT("selector_policies"), SelectorPolicies)
			&& SelectorPolicies != nullptr
			&& SelectorPolicies->IsEmpty());
	return true;
}
#endif
