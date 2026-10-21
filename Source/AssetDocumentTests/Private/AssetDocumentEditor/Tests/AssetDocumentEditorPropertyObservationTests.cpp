// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentEditorPropertyObservation.h"
#include "AssetDocumentEditorPropertyInspectTestTypes.h"
#include "Engine/Blueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Provider/Property/AssetDocumentEditorPropertyInspection.h"
#include "UObject/Class.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorPropertyObservationTests
{
const FAssetDocumentRuntimeValueDto* Field(
	const FAssetDocumentRuntimeValueDto& Object, const TCHAR* Name)
{
	const int32 Index = Object.ObjectKeys.IndexOfByKey(Name);
	return Object.Elements.IsValidIndex(Index) ? &Object.Elements[Index] : nullptr;
}

const FAssetDocumentRuntimeValueDto* Row(
	const FAssetDocumentRuntimeValueDto& Object, const TCHAR* Collection, const TCHAR* Name)
{
	const FAssetDocumentRuntimeValueDto* Rows = Field(Object, Collection);
	if (Rows != nullptr)
	{
		for (const FAssetDocumentRuntimeValueDto& Candidate : Rows->Elements)
		{
			const FAssetDocumentRuntimeValueDto* RowName = Field(Candidate, TEXT("name"));
			if (RowName != nullptr && RowName->StringValue == Name)
			{
				return &Candidate;
			}
		}
	}
	return nullptr;
}

bool Equal(const FAssetDocumentRuntimeValueDto& A, const FAssetDocumentRuntimeValueDto& B)
{
	if (A.Kind != B.Kind || A.BooleanValue != B.BooleanValue
		|| A.IntegerValue != B.IntegerValue || A.NumberValue != B.NumberValue
		|| A.StringValue != B.StringValue || A.ObjectKeys != B.ObjectKeys
		|| A.Elements.Num() != B.Elements.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < A.Elements.Num(); ++Index)
	{
		if (!Equal(A.Elements[Index], B.Elements[Index])) return false;
	}
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentEditorExistingObjectObservationTest,
	"UEAssetDoc.GenericProvider.Property.ExistingObjectObservation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEditorExistingObjectObservationTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentEditorPropertyObservationTests;
	(void)Parameters;
	TStrongObjectPtr<UAssetDocumentEditorPropertyInspectTestObject> Object(
		NewObject<UAssetDocumentEditorPropertyInspectTestObject>(GetTransientPackage()));
	Object->IntegerValue = 83;
	const auto Observed = InspectAssetDocumentEditorExistingObjectProperties(*Object);
	if (!TestTrue(TEXT("existing object is observed"), Observed.bSucceeded)) return false;
	const auto* IntegerRow = Row(Observed.Value, TEXT("admitted"), TEXT("IntegerValue"));
	const auto* RejectedRow = Row(Observed.Value, TEXT("rejected"), TEXT("TransientValue"));
	if (!TestNotNull(TEXT("nonempty admitted property survives"), IntegerRow)
		|| !TestNotNull(TEXT("rejected property is retained"), RejectedRow)) return false;
	const auto* Current = Field(*IntegerRow, TEXT("current_value"));
	const auto* Reason = Field(*RejectedRow, TEXT("reason_code"));
	if (!TestNotNull(TEXT("current typed value exists"), Current)
		|| !TestNotNull(TEXT("rejection reason exists"), Reason)) return false;
	TestEqual(TEXT("actual live integer is captured"), Current->IntegerValue, int64(83));
	TestEqual(TEXT("transient rejection remains explicit"), Reason->StringValue, FString(TEXT("Transient")));
	const auto Legacy = InspectAssetDocumentEditorProperties(*Object->GetClass(), Object.Get());
	TestTrue(TEXT("legacy live inspection remains identical"),
		Legacy.bSucceeded && Equal(Legacy.Value, Observed.Value));
	UObject* CDO = Object->GetClass()->GetDefaultObject(false);
	if (!TestNotNull(TEXT("fixture CDO already exists"), CDO)) return false;
	const auto Defaults = InspectAssetDocumentEditorExistingObjectProperties(*CDO);
	const auto* DefaultInteger = Row(Defaults.Value, TEXT("admitted"), TEXT("IntegerValue"));
	if (!TestTrue(TEXT("CDO inspection succeeds"), Defaults.bSucceeded)
		|| !TestNotNull(TEXT("CDO keeps real property rows"), DefaultInteger)) return false;
	const auto* DefaultCurrent = Field(*DefaultInteger, TEXT("current_value"));
	if (!TestNotNull(TEXT("CDO integer value is present"), DefaultCurrent)) return false;
	TestEqual(TEXT("CDO default is independently expected"), DefaultCurrent->IntegerValue, int64(17));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentEditorMissingCDOObservationTest,
	"UEAssetDoc.GenericProvider.Property.ExistingObjectMissingCDO",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEditorMissingCDOObservationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TStrongObjectPtr<UBlueprint> Blueprint(FKismetEditorUtilities::CreateBlueprint(
		UObject::StaticClass(), GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(),
			TEXT("AssetDocumentObservationMissingCDO")), BPTYPE_Normal));
	if (!TestNotNull(TEXT("test-owned Blueprint created"), Blueprint.Get())
		|| !TestNotNull(TEXT("test-owned generated class exists"), Blueprint->GeneratedClass.Get())) return false;
	UClass* Class = Blueprint->GeneratedClass;
	TStrongObjectPtr<UObject> CDO(Class->GetDefaultObject());
	if (!TestNotNull(TEXT("test-owned CDO prepared before observation"), CDO.Get())) return false;
	ON_SCOPE_EXIT { Class->SetDefaultObject(CDO.Get()); };
	Class->SetDefaultObject(nullptr);
	const auto Observed = InspectAssetDocumentEditorExistingObjectProperties(*CDO);
	TestFalse(TEXT("missing CDO fails observation"), Observed.bSucceeded);
	TestNull(TEXT("observation does not create missing CDO"), Class->GetDefaultObject(false));
	TestTrue(TEXT("missing CDO has explicit diagnostic"), Observed.Diagnostics.ContainsByPredicate(
		[](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
		{ return Diagnostic.Code == TEXT("PropertyInspection.DefaultUnavailable"); }));
	return true;
}
#endif
