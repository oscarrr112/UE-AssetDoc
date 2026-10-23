// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentEditorPrimitiveProvider.h"

#include "AssetDocumentEditorProviderTestSupport.h"
#include "Engine/DataAsset.h"
#include "Engine/Texture.h"
#include "Misc/AutomationTest.h"
#include "Provider/AssetDocumentEditorNativeIdentity.h"
#include "Provider/AssetDocumentEditorValueDto.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorClassReferenceProviderTests
{
FAssetDocumentRuntimeValueDto MakeStringValue(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto MakeNullValue()
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Null;
	return Result;
}

const FAssetDocumentPrimitiveDescriptor* FindDescriptor(
	const IAssetDocumentPrimitiveProvider& Provider,
	const TCHAR* PrimitiveId)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor :
		Provider.DescribePrimitives())
	{
		if (Descriptor.PrimitiveId == PrimitiveId)
		{
			return &Descriptor;
		}
	}
	return nullptr;
}

FAssetDocumentRuntimeValueDto MakeExactObject(
	FAutomationTestBase& Test,
	TArray<FAssetDocumentEditorValueField> Fields)
{
	FAssetDocumentRuntimeValueDto Result;
	FString DiagnosticCode;
	Test.TestTrue(
		TEXT("exact argument object builds"),
		TryBuildAssetDocumentEditorValueObject(
			Fields, Result, DiagnosticCode));
	return Result;
}

FAssetDocumentPrimitiveOutcome Invoke(
	FAutomationTestBase& Test,
	const TCHAR* PrimitiveId,
	const FAssetDocumentRuntimeValueDto& Arguments,
	FAssetDocumentEditorProviderTestContext& OutContext,
	FAssetDocumentEditorProviderTestHandleResolver& Handles)
{
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Descriptor =
		FindDescriptor(*Provider, PrimitiveId);
	if (!Test.TestNotNull(TEXT("primitive descriptor exists"), Descriptor))
	{
		return {};
	}
	FAssetDocumentPrimitiveInvocation Invocation =
		MakeAssetDocumentEditorProviderTestInvocation(
			*Descriptor, OutContext);
	Invocation.ValueArguments = Arguments;
	return Provider->Invoke(Invocation, Handles, nullptr);
}

FAssetDocumentHandleValidationContext MakeLeaseContext(
	const FAssetDocumentEditorProviderTestContext& Context,
	EAssetDocumentHandleKind Kind)
{
	FAssetDocumentHandleValidationContext Result;
	Result.RequestId = Context.RequestId;
	Result.TransactionId = Context.TransactionId;
	Result.RuntimeRevision = Context.RuntimeRevision;
	Result.ProviderId = Context.ProviderId;
	Result.Phase = EAssetDocumentRuntimeRequestPhase::Validate;
	Result.RequiredKind = Kind;
	Result.RequiredAccess = EAssetDocumentHandleAccess::ReadOnly;
	return Result;
}

bool HasDiagnostic(
	const FAssetDocumentPrimitiveOutcome& Outcome,
	const TCHAR* Code)
{
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic :
		Outcome.Diagnostics)
	{
		if (Diagnostic.Code == Code)
		{
			return true;
		}
	}
	return false;
}

void AddOutcomeDiagnostics(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveOutcome& Outcome)
{
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic :
		Outcome.Diagnostics)
	{
		Test.AddInfo(FString::Printf(
			TEXT("Outcome diagnostic [%s:%s]"),
			*Diagnostic.Code,
			*Diagnostic.Message));
	}
}

void TestRejectedReferencePath(
	FAutomationTestBase& Test,
	const TCHAR* PrimitiveId,
	const TCHAR* PathField,
	const FString& Path,
	const TCHAR* ExpectedField)
{
	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	const FAssetDocumentRuntimeValueDto Arguments = MakeExactObject(
		Test,
		{
			{PathField, MakeStringValue(Path)},
			{ExpectedField, MakeNullValue()},
		});
	const FAssetDocumentPrimitiveOutcome Outcome = Invoke(
		Test, PrimitiveId, Arguments, Context, Handles);
	const FString Description = FString::Printf(
		TEXT("non-canonical path fails closed: %s"), *Path);
	Test.TestEqual(
		*Description,
		Outcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Test.TestTrue(
		TEXT("non-canonical path issues no handle"),
		Outcome.HandleResults.IsEmpty());
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderCanonicalClassResolveTest,
	"UEAssetDoc.GenericProvider.ClassReference.CanonicalClassResolve",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderCanonicalClassResolveTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorClassReferenceProviderTests;
	using namespace UE::AssetDocument::EditorPrimitive;

	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	const FString ClassPath = UDataAsset::StaticClass()->GetPathName();
	const FAssetDocumentRuntimeValueDto Arguments = MakeExactObject(
		*this,
		{
			{TEXT("class"), MakeStringValue(ClassPath)},
			{TEXT("expected_base"),
				MakeStringValue(UObject::StaticClass()->GetPathName())},
		});
	const FAssetDocumentPrimitiveOutcome Outcome = Invoke(
		*this,
		TEXT("editor.class.resolve_canonical.v1"),
		Arguments,
		Context,
		Handles);

	TestEqual(TEXT("canonical class resolution succeeds"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	TestEqual(TEXT("one named class handle result"),
		Outcome.HandleResults.Num(), 1);
	if (Outcome.HandleResults.Num() != 1)
	{
		return false;
	}
	TestEqual(TEXT("class handle result name"),
		Outcome.HandleResults[0].Name, FString(TEXT("class")));
	TestEqual(TEXT("exactly one class handle"),
		Outcome.HandleResults[0].Handles.Num(), 1);
	if (Outcome.HandleResults[0].Handles.Num() != 1)
	{
		return false;
	}

	FAssetDocumentVerifiedLeaseResult Lease = Handles.AcquireVerifiedLease(
		Outcome.HandleResults[0].Handles[0],
		MakeLeaseContext(Context, EAssetDocumentHandleKind::Class));
	TestTrue(TEXT("issued class handle is leasable"),
		Lease.Lease.IsValid());
	if (!Lease.Lease)
	{
		return false;
	}
	INativeIdentityPayload* Payload = TryGetNativeIdentityPayload(
		Lease.Lease->GetPayloadChecked(), ClassPayloadTypeId);
	TestNotNull(TEXT("class payload type is exact"), Payload);
	if (Payload == nullptr)
	{
		return false;
	}
	FClassIdentityView Identity;
	TestTrue(TEXT("class identity is available"),
		Payload->TryGetClassIdentity(Identity));
	TestTrue(TEXT("resolved class identity is exact"),
		Identity.Class == UDataAsset::StaticClass());
	TestEqual(TEXT("canonical class path is preserved"),
		FString(Identity.CanonicalClassPath), ClassPath);
	TestTrue(TEXT("class type revision is nonzero"),
		Identity.TypeRevision > 0);

	TArray<const FAssetDocumentRuntimeValueDto*> Values;
	FString DiagnosticCode;
	const TArray<FAssetDocumentEditorValueFieldSpec> OutputShape = {
		{TEXT("canonical_class"), EAssetDocumentRuntimeValueKind::String},
	};
	TestTrue(TEXT("successful output has exact shape"),
		TryReadAssetDocumentEditorExactObject(
			Outcome.Value, OutputShape, Values, DiagnosticCode));
	if (Values.Num() == 1)
	{
		TestEqual(TEXT("successful output preserves canonical class"),
			Values[0]->StringValue, ClassPath);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderExpectedBaseMismatchTest,
	"UEAssetDoc.GenericProvider.ClassReference.ExpectedBaseMismatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderExpectedBaseMismatchTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorClassReferenceProviderTests;

	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	const FAssetDocumentRuntimeValueDto Arguments = MakeExactObject(
		*this,
		{
			{TEXT("class"),
				MakeStringValue(UDataAsset::StaticClass()->GetPathName())},
			{TEXT("expected_base"),
				MakeStringValue(UTexture::StaticClass()->GetPathName())},
		});
	const FAssetDocumentPrimitiveOutcome Outcome = Invoke(
		*this,
		TEXT("editor.class.resolve_canonical.v1"),
		Arguments,
		Context,
		Handles);

	TestEqual(TEXT("base mismatch fails closed"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	TestTrue(TEXT("base mismatch uses stable diagnostic"),
		HasDiagnostic(
			Outcome, TEXT("ClassReference.ExpectedBaseMismatch")));
	TestTrue(TEXT("base mismatch issues no handle"),
		Outcome.HandleResults.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderAssetResolveReadOnlyTest,
	"UEAssetDoc.GenericProvider.ClassReference.AssetResolveReadOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderAssetResolveReadOnlyTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorClassReferenceProviderTests;
	using namespace UE::AssetDocument::EditorPrimitive;

	const FString AssetPath =
		TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture");
	UObject* ExpectedAsset = FSoftObjectPath(AssetPath).TryLoad();
	if (!TestNotNull(TEXT("engine fixture asset loads"), ExpectedAsset))
	{
		return false;
	}
	UPackage* Package = ExpectedAsset->GetOutermost();
	const bool bWasPackageDirty = Package->IsDirty();

	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	const FAssetDocumentRuntimeValueDto Arguments = MakeExactObject(
		*this,
		{
			{TEXT("path"), MakeStringValue(AssetPath)},
			{TEXT("expected_class"),
				MakeStringValue(UTexture::StaticClass()->GetPathName())},
		});
	const FAssetDocumentPrimitiveOutcome Outcome = Invoke(
		*this,
		TEXT("editor.reference.resolve_asset.v1"),
		Arguments,
		Context,
		Handles);

	AddOutcomeDiagnostics(*this, Outcome);
	TestEqual(TEXT("canonical asset resolution succeeds"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	TestEqual(TEXT("one named asset handle result"),
		Outcome.HandleResults.Num(), 1);
	TestEqual(TEXT("resolution preserves package dirty state"),
		Package->IsDirty(), bWasPackageDirty);
	if (Outcome.HandleResults.Num() != 1)
	{
		return false;
	}
	TestEqual(TEXT("asset handle result name"),
		Outcome.HandleResults[0].Name, FString(TEXT("asset")));
	TestEqual(TEXT("exactly one asset handle"),
		Outcome.HandleResults[0].Handles.Num(), 1);
	if (Outcome.HandleResults[0].Handles.Num() != 1)
	{
		return false;
	}

	FAssetDocumentVerifiedLeaseResult Lease = Handles.AcquireVerifiedLease(
		Outcome.HandleResults[0].Handles[0],
		MakeLeaseContext(Context, EAssetDocumentHandleKind::Asset));
	TestTrue(TEXT("issued asset handle is read-only and leasable"),
		Lease.Lease.IsValid());
	if (!Lease.Lease)
	{
		return false;
	}
	INativeIdentityPayload* Payload = TryGetNativeIdentityPayload(
		Lease.Lease->GetPayloadChecked(), AssetPayloadTypeId);
	TestNotNull(TEXT("asset payload type is exact"), Payload);
	if (Payload == nullptr)
	{
		return false;
	}
	FObjectIdentityView Identity;
	TestTrue(TEXT("asset identity is available"),
		Payload->TryGetObjectIdentity(Identity));
	TestTrue(TEXT("resolved asset identity is exact"),
		Identity.Object == ExpectedAsset);
	TestEqual(TEXT("canonical asset path is preserved"),
		FString(Identity.CanonicalObjectPath), AssetPath);
	TestTrue(TEXT("asset type revision is nonzero"),
		Identity.TypeRevision > 0);

	TArray<const FAssetDocumentRuntimeValueDto*> Values;
	FString DiagnosticCode;
	const TArray<FAssetDocumentEditorValueFieldSpec> OutputShape = {
		{TEXT("canonical_path"), EAssetDocumentRuntimeValueKind::String},
		{TEXT("canonical_class"), EAssetDocumentRuntimeValueKind::String},
	};
	TestTrue(TEXT("asset output has exact shape"),
		TryReadAssetDocumentEditorExactObject(
			Outcome.Value, OutputShape, Values, DiagnosticCode));
	if (Values.Num() == 2)
	{
		TestEqual(TEXT("asset output preserves canonical path"),
			Values[0]->StringValue, AssetPath);
		TestEqual(TEXT("asset output preserves canonical class"),
			Values[1]->StringValue,
			ExpectedAsset->GetClass()->GetPathName());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderAssetTypeMismatchTest,
	"UEAssetDoc.GenericProvider.ClassReference.AssetTypeMismatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderAssetTypeMismatchTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorClassReferenceProviderTests;

	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	const FAssetDocumentRuntimeValueDto Arguments = MakeExactObject(
		*this,
		{
			{TEXT("path"),
				MakeStringValue(
					TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"))},
			{TEXT("expected_class"),
				MakeStringValue(UDataAsset::StaticClass()->GetPathName())},
		});
	const FAssetDocumentPrimitiveOutcome Outcome = Invoke(
		*this,
		TEXT("editor.reference.resolve_asset.v1"),
		Arguments,
		Context,
		Handles);

	TestEqual(TEXT("asset type mismatch fails closed"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	TestTrue(TEXT("asset type mismatch uses stable diagnostic"),
		HasDiagnostic(Outcome, TEXT("ClassReference.AssetTypeMismatch")));
	TestTrue(TEXT("asset type mismatch issues no handle"),
		Outcome.HandleResults.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderNonCanonicalRejectedTest,
	"UEAssetDoc.GenericProvider.ClassReference.NonCanonicalRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderNonCanonicalRejectedTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorClassReferenceProviderTests;

	AddExpectedError(
		TEXT("DataAsset_C"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	AddExpectedError(
		TEXT("MovieSceneSubTrack"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	const FString ClassCases[] = {
		TEXT("DataAsset"),
		TEXT(" /Script/Engine.DataAsset"),
		TEXT("Class'/Script/Engine.DataAsset'"),
		TEXT("/Script/Engine"),
		TEXT("/Script/Engine.DataAsset:Subobject"),
		TEXT("/Script/Engine.DataAsset_C"),
		TEXT("/Script/MovieSceneTracks.MovieSceneSubTrack"),
	};
	for (const FString& Path : ClassCases)
	{
		TestRejectedReferencePath(
			*this,
			TEXT("editor.class.resolve_canonical.v1"),
			TEXT("class"),
			Path,
			TEXT("expected_base"));
	}

	const FString AssetCases[] = {
		TEXT("DefaultTexture"),
		TEXT(" /Engine/EngineResources/DefaultTexture.DefaultTexture"),
		TEXT("Texture2D'/Engine/EngineResources/DefaultTexture.DefaultTexture'"),
		TEXT("/Engine/EngineResources/DefaultTexture"),
		TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture:Subobject"),
	};
	for (const FString& Path : AssetCases)
	{
		TestRejectedReferencePath(
			*this,
			TEXT("editor.reference.resolve_asset.v1"),
			TEXT("path"),
			Path,
			TEXT("expected_class"));
	}
	return true;
}
#endif
