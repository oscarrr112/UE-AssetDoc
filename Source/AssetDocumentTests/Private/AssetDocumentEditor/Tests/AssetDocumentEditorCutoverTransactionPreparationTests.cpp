// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentEditorCutoverTransactionPreparation.h"

#include "AssetDocumentEditorGenericProviderTestTypes.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "AssetDocumentEditorProviderTestSupport.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/PrimaryAssetLabel.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "Interfaces/Interface_AssetUserData.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Provider/AssetDocumentEditorNativeIdentity.h"
#include "Provider/AssetDocumentEditorTypedValueCodec.h"
#include "Provider/AssetDocumentEditorValueDto.h"
#include "Provider/Property/AssetDocumentEditorPropertyTokenPayload.h"
#include "Subtitles/SubtitlesAndClosedCaptionsDelegates.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorCutoverPreparationTests
{
using namespace UE::AssetDocument::EditorPrimitive;

class FTestClassPayload final : public INativeIdentityPayload
{
public:
	explicit FTestClassPayload(UClass& InClass)
		: Class(&InClass)
		, CanonicalClassPath(InClass.GetPathName())
		, TypeRevision(static_cast<uint64>(InClass.GetUniqueID()))
	{
	}

	virtual FName GetPayloadTypeId() const override
	{
		return ClassPayloadTypeId;
	}

	virtual bool IsAlive() const override
	{
		const UClass* Resolved = Class.Get();
		return Resolved != nullptr
			&& Resolved->GetPathName() == CanonicalClassPath
			&& static_cast<uint64>(Resolved->GetUniqueID()) == TypeRevision;
	}

	virtual bool TryGetClassIdentity(FClassIdentityView& OutView) const override
	{
		if (!IsAlive())
		{
			return false;
		}
		OutView.Class = Class.Get();
		OutView.CanonicalClassPath = CanonicalClassPath;
		OutView.TypeRevision = TypeRevision;
		return true;
	}

private:
	TWeakObjectPtr<UClass> Class;
	FString CanonicalClassPath;
	uint64 TypeRevision = 0;
};

class FCutoverTestObjectPayload final : public INativeIdentityPayload
{
public:
	FCutoverTestObjectPayload(UObject& InObject, FName InPayloadTypeId)
		: Object(&InObject)
		, CanonicalPath(InObject.GetPathName())
		, TypeRevision(static_cast<uint64>(InObject.GetUniqueID()))
		, PayloadTypeId(InPayloadTypeId)
	{
	}

	virtual FName GetPayloadTypeId() const override
	{
		return PayloadTypeId;
	}

	virtual bool IsAlive() const override
	{
		const UObject* Resolved = Object.Get();
		return Resolved != nullptr
			&& Resolved->GetPathName() == CanonicalPath
			&& static_cast<uint64>(Resolved->GetUniqueID()) == TypeRevision;
	}

	virtual bool TryGetObjectIdentity(FObjectIdentityView& OutView) const override
	{
		if (!IsAlive())
		{
			return false;
		}
		OutView.Object = Object.Get();
		OutView.CanonicalObjectPath = CanonicalPath;
		OutView.TypeRevision = TypeRevision;
		return true;
	}

private:
	TWeakObjectPtr<UObject> Object;
	FString CanonicalPath;
	uint64 TypeRevision = 0;
	FName PayloadTypeId;
};

FString MakeTargetName()
{
	return FString::Printf(
		TEXT("DA_CutoverPreparation_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

class FCanonicalPropertyAssetFixture final
{
public:
	FCanonicalPropertyAssetFixture()
		: Name(FString::Printf(
			TEXT("DA_ExistingRollback_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits)))
		, PackageName(FString::Printf(
			TEXT("/Game/AssetDocumentTests/%s"), *Name))
		, Package(CreatePackage(*PackageName))
		, Asset(NewObject<UAssetDocumentEditorCodecTestObject>(
			Package, *Name, RF_Transactional))
	{
		AddEditableField(TEXT("BooleanValue"));
		AddEditableField(TEXT("IntegerValue"));
		AddEditableField(TEXT("StringValue"));
	}

	~FCanonicalPropertyAssetFixture()
	{
		for (FProperty* Property : AddedEditFields)
		{
			Property->ClearPropertyFlags(CPF_Edit);
		}
		if (Package != nullptr)
		{
			Package->SetDirtyFlag(false);
			Asset.Reset();
			UPackageTools::UnloadPackages({Package});
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
	}

	bool IsValid() const
	{
		return Package != nullptr && Asset.IsValid()
			&& BooleanProperty != nullptr
			&& IntegerProperty != nullptr
			&& StringProperty != nullptr;
	}

	void SetBaseline(bool bDirty)
	{
		Asset->BooleanValue = false;
		Asset->IntegerValue = 17;
		Asset->StringValue = TEXT("baseline");
		Package->SetDirtyFlag(bDirty);
	}

	FAssetDocumentEditorCutoverMutationIntent MakeIntent(
		const FAssetDocumentEditorProviderTestContext& Context) const
	{
		FAssetDocumentEditorCutoverMutationIntent Intent;
		Intent.RequestId = Context.RequestId;
		Intent.RuntimeRevision = Context.RuntimeRevision;
		Intent.CanonicalObjectPath = Asset->GetPathName();
		Intent.ExactAssetClassPath = Asset->GetClass()->GetPathName();
		Intent.bWriteAdmittedProperties = true;
		return Intent;
	}

	FString Name;
	FString PackageName;
	UPackage* Package = nullptr;
	TStrongObjectPtr<UAssetDocumentEditorCodecTestObject> Asset;
	FProperty* BooleanProperty = nullptr;
	FProperty* IntegerProperty = nullptr;
	FProperty* StringProperty = nullptr;

private:
	void AddEditableField(const TCHAR* FieldName)
	{
		FProperty* Property = FindFProperty<FProperty>(
			UAssetDocumentEditorCodecTestObject::StaticClass(), FieldName);
		if (Property != nullptr && !Property->HasAnyPropertyFlags(CPF_Edit))
		{
			Property->SetPropertyFlags(CPF_Edit);
			AddedEditFields.Add(Property);
		}
		if (FCString::Strcmp(FieldName, TEXT("BooleanValue")) == 0)
		{
			BooleanProperty = Property;
		}
		else if (FCString::Strcmp(FieldName, TEXT("IntegerValue")) == 0)
		{
			IntegerProperty = Property;
		}
		else
		{
			StringProperty = Property;
		}
	}

	TArray<FProperty*> AddedEditFields;
};

FAssetDocumentEditorCutoverMutationIntent MakeCreateIntent(
	const FAssetDocumentEditorProviderTestContext& Context,
	const FString& Name)
{
	FAssetDocumentEditorCutoverMutationIntent Intent;
	Intent.RequestId = Context.RequestId;
	Intent.RuntimeRevision = Context.RuntimeRevision;
	Intent.CanonicalObjectPath = FString::Printf(
		TEXT("/Game/AssetDocumentTests/%s.%s"), *Name, *Name);
	Intent.ExactAssetClassPath = UPrimaryAssetLabel::StaticClass()->GetPathName();
	Intent.FactoryId = TEXT("data_asset_factory.v1");
	Intent.bCreateByFactory = true;
	return Intent;
}

FAssetDocumentRuntimeValueDto MakeStringValue(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto MakeBooleanValue(bool bValue)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Boolean;
	Result.BooleanValue = bValue;
	return Result;
}

FAssetDocumentRuntimeValueDto MakeObjectValue(
	TArray<FString> Keys,
	TArray<FAssetDocumentRuntimeValueDto> Values)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Object;
	Result.ObjectKeys = MoveTemp(Keys);
	Result.Elements = MoveTemp(Values);
	return Result;
}

FAssetDocumentRuntimeValueDto MakeFactoryArguments(
	FAutomationTestBase& Test,
	const FString& Name)
{
	const FAssetDocumentEditorValueField Fields[] = {
		{TEXT("factory_id"), MakeStringValue(TEXT("data_asset_factory.v1"))},
		{TEXT("package_path"),
			MakeStringValue(TEXT("/Game/AssetDocumentTests"))},
		{TEXT("asset_name"), MakeStringValue(Name)},
	};
	FAssetDocumentRuntimeValueDto Result;
	FString DiagnosticCode;
	Test.TestTrue(TEXT("factory arguments have exact shape"),
		TryBuildAssetDocumentEditorValueObject(
			Fields, Result, DiagnosticCode));
	return Result;
}

TOptional<FAssetDocumentOpaqueHandle> IssueCutoverHandle(
	FAutomationTestBase& Test,
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	const TCHAR* Label,
	EAssetDocumentHandleKind Kind,
	EAssetDocumentHandleAccess Access,
	TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload)
{
	FAssetDocumentIssuedHandleResult Issued =
		Handles.Issue(Kind, Access, MoveTemp(Payload));
	Test.TestTrue(Label, Issued.Handle.IsSet());
	return MoveTemp(Issued.Handle);
}

FProperty* FindCutoverBooleanProperty()
{
	for (TFieldIterator<FProperty> It(
		UAssetDocumentEditorCodecTestObject::StaticClass()); It; ++It)
	{
		if (It->GetAuthoredName() == TEXT("BooleanValue"))
		{
			return *It;
		}
	}
	return nullptr;
}

const FAssetDocumentOpaqueHandle* FindCutoverOutcomeHandle(
	const FAssetDocumentPrimitiveOutcome& Outcome,
	FStringView Name)
{
	const FAssetDocumentPrimitiveHandleResult* Result =
		Outcome.HandleResults.FindByPredicate(
			[Name](const FAssetDocumentPrimitiveHandleResult& Candidate)
			{
				return Candidate.Name == Name;
			});
	return Result != nullptr && Result->Handles.Num() == 1
		? &Result->Handles[0]
		: nullptr;
}

bool HasCutoverDiagnostic(
	const FAssetDocumentContractValidationResult& Result,
	FStringView Code)
{
	return Result.Diagnostics.ContainsByPredicate(
		[Code](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == Code;
		});
}

const FAssetDocumentPrimitiveDescriptor* FindCutoverDescriptor(
	const IAssetDocumentPrimitiveProvider& Provider,
	FStringView PrimitiveId)
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

bool TestCutoverFootprintEquals(
	FAutomationTestBase& Test,
	const TCHAR* Label,
	TConstArrayView<FAssetDocumentMutationFootprint> Actual,
	TConstArrayView<FAssetDocumentMutationFootprint> Expected)
{
	if (!Test.TestEqual(
		*FString::Printf(TEXT("%s row count"), Label),
		Actual.Num(), Expected.Num()))
	{
		return false;
	}
	for (int32 Index = 0; Index < Actual.Num(); ++Index)
	{
		Test.TestEqual(
			*FString::Printf(TEXT("%s row %d kind"), Label, Index),
			Actual[Index].Kind, Expected[Index].Kind);
		Test.TestEqual(
			*FString::Printf(TEXT("%s row %d stable id"), Label, Index),
			Actual[Index].StableId, Expected[Index].StableId);
		Test.TestEqual(
			*FString::Printf(TEXT("%s row %d rollback owner"), Label, Index),
			Actual[Index].RollbackComponentId,
			Expected[Index].RollbackComponentId);
	}
	return true;
}

TArray<FAssetDocumentMutationFootprint> MakeExpectedPropertyFootprint(
	const FString& CanonicalObjectPath)
{
	const FString PackageName =
		FSoftObjectPath(CanonicalObjectPath).GetLongPackageName();
	return {
		{EAssetDocumentMutationFootprintKind::UObjectState,
			FString::Printf(TEXT("Object:%s"), *CanonicalObjectPath),
			TEXT("rollback.uobject")},
		{EAssetDocumentMutationFootprintKind::Package,
			FString::Printf(TEXT("Package:%s"), *PackageName),
			TEXT("rollback.package")},
	};
}

TArray<FAssetDocumentMutationFootprint> MakeExpectedFactoryFootprint(
	const FString& CanonicalObjectPath)
{
	const FString PackageName =
		FSoftObjectPath(CanonicalObjectPath).GetLongPackageName();
	const FString PackageFilename =
		FPackageName::LongPackageNameToFilename(
			PackageName, FPackageName::GetAssetPackageExtension());
	const FString Owner = TEXT("editor.asset.rollback_created.v1");
	auto StableId = [&Owner](const TCHAR* Kind, const FString& Identity)
	{
		return FString::Printf(TEXT("%s|%s|%s"), *Owner, Kind, *Identity);
	};
	return {
		{EAssetDocumentMutationFootprintKind::UObjectState,
			StableId(TEXT("UObjectState"), CanonicalObjectPath), Owner},
		{EAssetDocumentMutationFootprintKind::Package,
			StableId(TEXT("Package"), PackageName), Owner},
		{EAssetDocumentMutationFootprintKind::AssetRegistry,
			StableId(TEXT("AssetRegistry"), CanonicalObjectPath), Owner},
		{EAssetDocumentMutationFootprintKind::File,
			StableId(TEXT("File"), PackageFilename), Owner},
		{EAssetDocumentMutationFootprintKind::Handle,
			StableId(TEXT("Handle"), CanonicalObjectPath), Owner},
	};
}

TArray<FAssetDocumentMutationFootprint> MakeExpectedEmbeddedFootprint(
	const FString& OuterPath)
{
	return {
		{EAssetDocumentMutationFootprintKind::UObjectState, OuterPath,
			TEXT("assetdocument.editor.embedded.asset_user_data.v1")},
	};
}

void InvalidateHandles(
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	const FAssetDocumentEditorProviderTestContext& Context)
{
	FAssetDocumentHandleInvalidation Event;
	Event.Reason = EAssetDocumentHandleInvalidationReason::Rollback;
	Event.RequestId = Context.RequestId;
	Event.TransactionId = Context.TransactionId;
	Event.RuntimeRevision = Context.RuntimeRevision;
	Event.ProviderId = Context.ProviderId;
	Handles.Invalidate(Event);
}

void DeleteTargetIfPresent(const FString& CanonicalObjectPath)
{
	if (UObject* Asset =
		FSoftObjectPath(CanonicalObjectPath).ResolveObject())
	{
		ObjectTools::DeleteObjects(
			{Asset}, false,
			ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
	}
	const FString PackageName =
		FSoftObjectPath(CanonicalObjectPath).GetLongPackageName();
	if (UPackage* Package = FindPackage(nullptr, *PackageName))
	{
		Package->SetDirtyFlag(false);
		UPackageTools::UnloadPackages({Package});
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	IFileManager::Get().Delete(*Filename, false, true);
}

UObject* CreateCutoverAsset(
	const FAssetDocumentEditorCutoverMutationIntent& Intent,
	UClass& AssetClass);

bool TestPreparationShapes(FAutomationTestBase& Test)
{
	FAssetDocumentEditorProviderTestContext PropertyContext;
	FCanonicalPropertyAssetFixture PropertyFixture;
	PropertyFixture.SetBaseline(true);
	if (!Test.TestTrue(
		TEXT("property shape fixture is canonical"),
		PropertyFixture.IsValid()))
	{
		return false;
	}
	const FAssetDocumentEditorCutoverMutationIntent PropertyIntent =
		PropertyFixture.MakeIntent(PropertyContext);
	const FAssetDocumentEditorCutoverTransactionPreparation Property =
		PrepareAssetDocumentEditorCutoverTransaction(PropertyIntent);
	Test.TestTrue(TEXT("property intent prepares"), Property.bPrepared);
	const TArray<FAssetDocumentMutationFootprint> ExpectedProperty =
		MakeExpectedPropertyFootprint(PropertyIntent.CanonicalObjectPath);
	TestCutoverFootprintEquals(
		Test, TEXT("property preparation"),
		Property.CompleteFootprint, ExpectedProperty);

	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorCutoverMutationIntent Intent =
		MakeCreateIntent(Context, MakeTargetName());
	const FAssetDocumentEditorCutoverTransactionPreparation Create =
		PrepareAssetDocumentEditorCutoverTransaction(Intent);
	Test.TestTrue(TEXT("create intent prepares"), Create.bPrepared);
	const TArray<FAssetDocumentMutationFootprint> ExpectedFactory =
		MakeExpectedFactoryFootprint(Intent.CanonicalObjectPath);
	TestCutoverFootprintEquals(
		Test, TEXT("create preparation"),
		Create.CompleteFootprint, ExpectedFactory);
	Test.TestEqual(TEXT("create returns one rollback seed"),
		Create.RollbackSeeds.Num(), 1);

	Intent.bWriteAdmittedProperties = true;
	const FAssetDocumentEditorCutoverTransactionPreparation Combined =
		PrepareAssetDocumentEditorCutoverTransaction(Intent);
	Test.TestTrue(TEXT("combined intent prepares"), Combined.bPrepared);
	TArray<FAssetDocumentMutationFootprint> ExpectedCombined = ExpectedFactory;
	ExpectedCombined.Append(
		MakeExpectedPropertyFootprint(Intent.CanonicalObjectPath));
	TestCutoverFootprintEquals(
		Test, TEXT("combined preparation"),
		Combined.CompleteFootprint, ExpectedCombined);
	TSet<FString> StableIds;
	for (const FAssetDocumentMutationFootprint& Row :
		Combined.CompleteFootprint)
	{
		StableIds.Add(Row.StableId);
	}
	Test.TestEqual(TEXT("combined StableIds are unique"),
		StableIds.Num(), 7);
	return true;
}

bool TestRollbackComponentIds(
	FAutomationTestBase& Test,
	const TCHAR* Label,
	const FAssetDocumentEditorCutoverTransactionPreparation& Preparation,
	TConstArrayView<FString> Expected)
{
	if (!Test.TestEqual(
		*FString::Printf(TEXT("%s count"), Label),
		Preparation.RollbackSeeds.Num(), Expected.Num()))
	{
		return false;
	}
	for (int32 Index = 0; Index < Expected.Num(); ++Index)
	{
		Test.TestEqual(
			*FString::Printf(TEXT("%s component %d"), Label, Index),
			FString(Preparation.RollbackSeeds[Index]->
				GetRollbackComponentId()),
			Expected[Index]);
	}
	return true;
}

bool TestValidationResultsEqual(
	FAutomationTestBase& Test,
	const TCHAR* Label,
	const FAssetDocumentContractValidationResult& Actual,
	const FAssetDocumentContractValidationResult& Expected)
{
	Test.TestEqual(
		*FString::Printf(TEXT("%s validity"), Label),
		Actual.bValid, Expected.bValid);
	if (!Test.TestEqual(
		*FString::Printf(TEXT("%s diagnostic count"), Label),
		Actual.Diagnostics.Num(), Expected.Diagnostics.Num()))
	{
		return false;
	}
	for (int32 Index = 0; Index < Expected.Diagnostics.Num(); ++Index)
	{
		Test.TestEqual(
			*FString::Printf(TEXT("%s path %d"), Label, Index),
			Actual.Diagnostics[Index].Path, Expected.Diagnostics[Index].Path);
		Test.TestEqual(
			*FString::Printf(TEXT("%s code %d"), Label, Index),
			Actual.Diagnostics[Index].Code, Expected.Diagnostics[Index].Code);
		Test.TestEqual(
			*FString::Printf(TEXT("%s message %d"), Label, Index),
			Actual.Diagnostics[Index].Message,
			Expected.Diagnostics[Index].Message);
	}
	return true;
}

bool OpenAllRollbackCaptures(
	FAutomationTestBase& Test,
	const FAssetDocumentEditorCutoverTransactionPreparation& Preparation,
	const FGuid& TransactionId,
	TArray<TUniquePtr<IAssetDocumentEditorCutoverMutationCapture>>& OutCaptures)
{
	for (const TSharedRef<IAssetDocumentEditorCutoverRollbackSeed>& Seed :
		Preparation.RollbackSeeds)
	{
		TUniquePtr<IAssetDocumentEditorCutoverMutationCapture> Capture =
			Seed->OpenMutationCapture(TransactionId);
		if (!Test.TestTrue(TEXT("rollback component capture opens"),
			Capture.IsValid()))
		{
			return false;
		}
		OutCaptures.Add(MoveTemp(Capture));
	}
	return true;
}

void CloseAllRollbackCaptures(
	TArray<TUniquePtr<IAssetDocumentEditorCutoverMutationCapture>>& Captures)
{
	for (TUniquePtr<IAssetDocumentEditorCutoverMutationCapture>& Capture :
		Captures)
	{
		Capture.Reset();
	}
}

bool HasExpectedExistingRollbackSeeds(
	const FAssetDocumentEditorCutoverTransactionPreparation& Preparation)
{
	return Preparation.bPrepared && Preparation.RollbackSeeds.Num() == 2;
}

bool TestExistingPropertyPreparationShape(
	FAutomationTestBase& Test,
	const FCanonicalPropertyAssetFixture& Fixture,
	const FAssetDocumentEditorCutoverMutationIntent& Intent,
	const FAssetDocumentEditorCutoverTransactionPreparation& Preparation)
{
	bool bShapeValid = true;
	bShapeValid &= Test.TestTrue(
		TEXT("existing property fixture is canonical"), Fixture.IsValid());
	bShapeValid &= Test.TestTrue(
		TEXT("existing property target prepares"), Preparation.bPrepared);
	bShapeValid &= Test.TestTrue(
		TEXT("existing property preparation has no diagnostics"),
		Preparation.Diagnostics.IsEmpty());
	if (!bShapeValid)
	{
		return false;
	}
	const TArray<FString> ExpectedComponents = {
		TEXT("rollback.uobject"), TEXT("rollback.package")};
	const TArray<FAssetDocumentMutationFootprint> ExpectedFootprint =
		MakeExpectedPropertyFootprint(Intent.CanonicalObjectPath);
	const bool bComponentsValid = TestRollbackComponentIds(
			Test, TEXT("property rollback seeds"),
			Preparation, ExpectedComponents);
	const bool bFootprintValid = TestCutoverFootprintEquals(
			Test, TEXT("property rollback footprint"),
			Preparation.CompleteFootprint, ExpectedFootprint);
	if (!bComponentsValid || !bFootprintValid)
	{
		return false;
	}
	Test.TestFalse(TEXT("capture preserves Boolean baseline"),
		Fixture.Asset->BooleanValue);
	Test.TestEqual(TEXT("capture preserves Integer baseline"),
		Fixture.Asset->IntegerValue, static_cast<int64>(17));
	Test.TestEqual(TEXT("capture preserves String baseline"),
		Fixture.Asset->StringValue, FString(TEXT("baseline")));
	Test.TestTrue(TEXT("capture preserves dirty baseline"),
		Fixture.Package->IsDirty());
	return true;
}

bool TestExistingPropertyShapeAndRestore(FAutomationTestBase& Test)
{
	FCanonicalPropertyAssetFixture Fixture;
	Fixture.SetBaseline(true);
	FAssetDocumentEditorProviderTestContext Context;
	const FAssetDocumentEditorCutoverMutationIntent Intent =
		Fixture.MakeIntent(Context);
	const FAssetDocumentEditorCutoverTransactionPreparation Preparation =
		PrepareAssetDocumentEditorCutoverTransaction(Intent);
	if (!TestExistingPropertyPreparationShape(
		Test, Fixture, Intent, Preparation))
	{
		return false;
	}
	TArray<TUniquePtr<IAssetDocumentEditorCutoverMutationCapture>> Captures;
	if (!OpenAllRollbackCaptures(
		Test, Preparation, Context.TransactionId, Captures))
	{
		return false;
	}
	Fixture.Asset->BooleanValue = true;
	Fixture.Asset->IntegerValue = 91;
	Fixture.Package->SetDirtyFlag(false);
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	InvalidateHandles(Handles, Context);
	CloseAllRollbackCaptures(Captures);
	const FAssetDocumentContractValidationResult First =
		Preparation.RollbackSeeds[0]->Rollback(TEXT("Test.Property"));
	Test.TestTrue(TEXT("property rollback succeeds"), First.bValid);
	Test.TestFalse(TEXT("property rollback restores Boolean"),
		Fixture.Asset->BooleanValue);
	Test.TestEqual(TEXT("property rollback restores Integer"),
		Fixture.Asset->IntegerValue, static_cast<int64>(17));
	Test.TestEqual(TEXT("property rollback preserves unchanged String"),
		Fixture.Asset->StringValue, FString(TEXT("baseline")));
	Test.TestTrue(TEXT("property rollback restores dirty state"),
		Fixture.Package->IsDirty());

	Fixture.Asset->BooleanValue = true;
	Fixture.Package->SetDirtyFlag(false);
	const FAssetDocumentContractValidationResult Second =
		Preparation.RollbackSeeds[1]->Rollback(TEXT("Test.PropertyAgain"));
	TestValidationResultsEqual(
		Test, TEXT("property cached success"), Second, First);
	Test.TestTrue(TEXT("cached success does not restore twice"),
		Fixture.Asset->BooleanValue);
	Test.TestFalse(TEXT("cached success does not rewrite dirty state"),
		Fixture.Package->IsDirty());
	return true;
}

bool TestExistingPropertyIdentityFailureCached(FAutomationTestBase& Test)
{
	FCanonicalPropertyAssetFixture Fixture;
	Fixture.SetBaseline(true);
	FAssetDocumentEditorProviderTestContext Context;
	const FAssetDocumentEditorCutoverTransactionPreparation Preparation =
		PrepareAssetDocumentEditorCutoverTransaction(
			Fixture.MakeIntent(Context));
	TArray<TUniquePtr<IAssetDocumentEditorCutoverMutationCapture>> Captures;
	if (!HasExpectedExistingRollbackSeeds(Preparation))
	{
		return false;
	}
	if (!OpenAllRollbackCaptures(
		Test, Preparation, Context.TransactionId, Captures))
	{
		return false;
	}
	CloseAllRollbackCaptures(Captures);
	Fixture.Asset->BooleanValue = true;
	const FString DriftName = Fixture.Name + TEXT("_Drift");
	if (!Test.TestTrue(TEXT("identity drift rename succeeds"),
		Fixture.Asset->Rename(
			*DriftName, Fixture.Package,
			REN_DontCreateRedirectors | REN_NonTransactional)))
	{
		return false;
	}
	const FAssetDocumentContractValidationResult First =
		Preparation.RollbackSeeds[0]->Rollback(TEXT("Test.Identity"));
	Test.TestFalse(TEXT("identity drift fails restore"), First.bValid);
	Test.TestTrue(TEXT("identity drift diagnostic is exact"),
		HasCutoverDiagnostic(
			First, TEXT("PropertyPreimage.IdentityChanged")));
	Test.TestTrue(TEXT("identity failure writes no field"),
		Fixture.Asset->BooleanValue);
	if (!Test.TestTrue(TEXT("identity drift rename restores fixture"),
		Fixture.Asset->Rename(
			*Fixture.Name, Fixture.Package,
			REN_DontCreateRedirectors | REN_NonTransactional)))
	{
		return false;
	}
	const FAssetDocumentContractValidationResult Second =
		Preparation.RollbackSeeds[1]->Rollback(TEXT("Test.IdentityAgain"));
	TestValidationResultsEqual(
		Test, TEXT("identity failure is cached"), Second, First);
	Test.TestTrue(TEXT("cached identity failure does not retry"),
		Fixture.Asset->BooleanValue);
	return true;
}

bool TestExistingPropertyAccessFailureCached(FAutomationTestBase& Test)
{
	FCanonicalPropertyAssetFixture Fixture;
	Fixture.SetBaseline(true);
	FAssetDocumentEditorProviderTestContext Context;
	const FAssetDocumentEditorCutoverTransactionPreparation Preparation =
		PrepareAssetDocumentEditorCutoverTransaction(
			Fixture.MakeIntent(Context));
	TArray<TUniquePtr<IAssetDocumentEditorCutoverMutationCapture>> Captures;
	if (!Preparation.bPrepared || Preparation.RollbackSeeds.Num() != 2
		|| !OpenAllRollbackCaptures(
			Test, Preparation, Context.TransactionId, Captures))
	{
		return false;
	}
	CloseAllRollbackCaptures(Captures);
	Fixture.Asset->BooleanValue = true;
	Fixture.Asset->IntegerValue = 91;
	Fixture.IntegerProperty->SetPropertyFlags(CPF_EditConst);
	const FAssetDocumentContractValidationResult First =
		Preparation.RollbackSeeds[0]->Rollback(TEXT("Test.Access"));
	Test.TestFalse(TEXT("access drift fails restore"), First.bValid);
	Test.TestTrue(TEXT("access drift diagnostic is exact"),
		HasCutoverDiagnostic(
			First, TEXT("PropertyPreimage.AccessChanged")));
	Test.TestTrue(TEXT("later access drift prevents earlier Boolean write"),
		Fixture.Asset->BooleanValue);
	Test.TestEqual(TEXT("later access drift preserves Integer mutation"),
		Fixture.Asset->IntegerValue, static_cast<int64>(91));
	Fixture.IntegerProperty->ClearPropertyFlags(CPF_EditConst);
	const FAssetDocumentContractValidationResult Second =
		Preparation.RollbackSeeds[1]->Rollback(TEXT("Test.AccessAgain"));
	TestValidationResultsEqual(
		Test, TEXT("access failure is cached"), Second, First);
	Test.TestTrue(TEXT("cached access failure does not retry"),
		Fixture.Asset->BooleanValue);
	return true;
}

bool TestExistingPropertyOpenRejections(FAutomationTestBase& Test)
{
	FCanonicalPropertyAssetFixture Fixture;
	Fixture.SetBaseline(false);
	FAssetDocumentEditorProviderTestContext Context;
	const FAssetDocumentEditorCutoverTransactionPreparation Preparation =
		PrepareAssetDocumentEditorCutoverTransaction(
			Fixture.MakeIntent(Context));
	if (!Preparation.bPrepared || Preparation.RollbackSeeds.Num() != 2)
	{
		return false;
	}
	TUniquePtr<IAssetDocumentEditorCutoverMutationCapture> First =
		Preparation.RollbackSeeds[0]->OpenMutationCapture(
			Context.TransactionId);
	Test.TestTrue(TEXT("first component capture opens"), First.IsValid());
	Test.TestFalse(TEXT("duplicate component open is rejected"),
		Preparation.RollbackSeeds[0]->OpenMutationCapture(
			Context.TransactionId).IsValid());
	Test.TestFalse(TEXT("mismatched transaction is rejected"),
		Preparation.RollbackSeeds[1]->OpenMutationCapture(
			FGuid::NewGuid()).IsValid());
	First.Reset();
	Test.TestFalse(TEXT("reopen after close is rejected"),
		Preparation.RollbackSeeds[1]->OpenMutationCapture(
			Context.TransactionId).IsValid());
	return true;
}

bool TestExistingPropertyMissingCaptureCached(FAutomationTestBase& Test)
{
	FCanonicalPropertyAssetFixture Fixture;
	Fixture.SetBaseline(false);
	FAssetDocumentEditorProviderTestContext Context;
	const FAssetDocumentEditorCutoverTransactionPreparation Preparation =
		PrepareAssetDocumentEditorCutoverTransaction(
			Fixture.MakeIntent(Context));
	if (!Preparation.bPrepared || Preparation.RollbackSeeds.Num() != 2)
	{
		return false;
	}
	TUniquePtr<IAssetDocumentEditorCutoverMutationCapture> First =
		Preparation.RollbackSeeds[0]->OpenMutationCapture(
			Context.TransactionId);
	if (!Test.TestTrue(TEXT("partial capture opens"), First.IsValid()))
	{
		return false;
	}
	First.Reset();
	const FAssetDocumentContractValidationResult Missing =
		Preparation.RollbackSeeds[0]->Rollback(TEXT("Test.Missing"));
	Test.TestFalse(TEXT("missing component fails rollback"), Missing.bValid);
	Test.TestTrue(TEXT("missing component diagnostic is exact"),
		HasCutoverDiagnostic(
			Missing, TEXT("ExistingPropertyRollback.MissingCapture")));
	const FAssetDocumentContractValidationResult Cached =
		Preparation.RollbackSeeds[1]->Rollback(TEXT("Test.MissingAgain"));
	TestValidationResultsEqual(
		Test, TEXT("missing capture failure is cached"), Cached, Missing);
	Test.TestFalse(TEXT("open after consume is rejected"),
		Preparation.RollbackSeeds[1]->OpenMutationCapture(
			Context.TransactionId).IsValid());
	return true;
}

bool TestExistingPropertyActiveLeaseCached(FAutomationTestBase& Test)
{
	FCanonicalPropertyAssetFixture Fixture;
	Fixture.SetBaseline(false);
	FAssetDocumentEditorProviderTestContext Context;
	const FAssetDocumentEditorCutoverTransactionPreparation Preparation =
		PrepareAssetDocumentEditorCutoverTransaction(
			Fixture.MakeIntent(Context));
	TArray<TUniquePtr<IAssetDocumentEditorCutoverMutationCapture>> Captures;
	if (!Preparation.bPrepared || Preparation.RollbackSeeds.Num() != 2
		|| !OpenAllRollbackCaptures(
			Test, Preparation, Context.TransactionId, Captures))
	{
		return false;
	}
	Captures[0].Reset();
	const FAssetDocumentContractValidationResult Active =
		Preparation.RollbackSeeds[0]->Rollback(TEXT("Test.Active"));
	Test.TestFalse(TEXT("active lease fails rollback"), Active.bValid);
	Test.TestTrue(TEXT("active lease diagnostic is exact"),
		HasCutoverDiagnostic(
			Active, TEXT("ExistingPropertyRollback.InvalidState")));
	Captures[1].Reset();
	const FAssetDocumentContractValidationResult Cached =
		Preparation.RollbackSeeds[1]->Rollback(TEXT("Test.ActiveAgain"));
	TestValidationResultsEqual(
		Test, TEXT("active lease failure is cached"), Cached, Active);
	return true;
}

bool TestExistingPropertyInvalidTargets(FAutomationTestBase& Test)
{
	FCanonicalPropertyAssetFixture Fixture;
	Fixture.SetBaseline(true);
	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorCutoverMutationIntent WrongClass =
		Fixture.MakeIntent(Context);
	WrongClass.ExactAssetClassPath = UTexture2D::StaticClass()->GetPathName();
	const FAssetDocumentEditorCutoverTransactionPreparation ClassFailure =
		PrepareAssetDocumentEditorCutoverTransaction(WrongClass);
	Test.TestFalse(TEXT("property class mismatch fails closed"),
		ClassFailure.bPrepared);
	Test.TestTrue(TEXT("class mismatch has no footprint"),
		ClassFailure.CompleteFootprint.IsEmpty());
	Test.TestTrue(TEXT("class mismatch has no seed"),
		ClassFailure.RollbackSeeds.IsEmpty());
	Test.TestTrue(TEXT("class mismatch diagnostic is exact"),
		ClassFailure.Diagnostics.Num() == 1
			&& ClassFailure.Diagnostics[0].Code
				== TEXT("CutoverPreparation.InvalidTarget"));

	FAssetDocumentEditorCutoverMutationIntent Missing =
		Fixture.MakeIntent(Context);
	Missing.CanonicalObjectPath = FString::Printf(
		TEXT("%s.Missing"), *Fixture.PackageName);
	const FAssetDocumentEditorCutoverTransactionPreparation TargetFailure =
		PrepareAssetDocumentEditorCutoverTransaction(Missing);
	Test.TestFalse(TEXT("missing property target fails closed"),
		TargetFailure.bPrepared);
	Test.TestTrue(TEXT("missing target has no footprint"),
		TargetFailure.CompleteFootprint.IsEmpty());
	Test.TestTrue(TEXT("missing target has no seed"),
		TargetFailure.RollbackSeeds.IsEmpty());
	Test.TestFalse(TEXT("invalid targets preserve Boolean"),
		Fixture.Asset->BooleanValue);
	Test.TestEqual(TEXT("invalid targets preserve Integer"),
		Fixture.Asset->IntegerValue, static_cast<int64>(17));
	Test.TestTrue(TEXT("invalid targets preserve dirty state"),
		Fixture.Package->IsDirty());
	return true;
}

bool TestCombinedPreparationShape(
	FAutomationTestBase& Test,
	const FAssetDocumentEditorCutoverTransactionPreparation& Preparation)
{
	if (!Test.TestTrue(
		TEXT("combined rollback target prepares"), Preparation.bPrepared)
		|| !Test.TestTrue(
			TEXT("combined preparation has no diagnostics"),
			Preparation.Diagnostics.IsEmpty()))
	{
		return false;
	}
	const TArray<FString> ExpectedComponents = {
		TEXT("editor.asset.rollback_created.v1"),
		TEXT("rollback.uobject"),
		TEXT("rollback.package")};
	if (!TestRollbackComponentIds(
		Test, TEXT("combined rollback aliases"),
		Preparation, ExpectedComponents))
	{
		return false;
	}
	return true;
}

bool TestCombinedRollbackAliases(FAutomationTestBase& Test)
{
	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorCutoverMutationIntent Intent =
		MakeCreateIntent(Context, MakeTargetName());
	Intent.bWriteAdmittedProperties = true;
	ON_SCOPE_EXIT
	{
		DeleteTargetIfPresent(Intent.CanonicalObjectPath);
	};
	const FAssetDocumentEditorCutoverTransactionPreparation Preparation =
		PrepareAssetDocumentEditorCutoverTransaction(Intent);
	if (!TestCombinedPreparationShape(Test, Preparation))
	{
		return false;
	}
	TArray<TUniquePtr<IAssetDocumentEditorCutoverMutationCapture>> Captures;
	if (!OpenAllRollbackCaptures(
		Test, Preparation, Context.TransactionId, Captures))
	{
		return false;
	}
	UObject* Created =
		CreateCutoverAsset(Intent, *UPrimaryAssetLabel::StaticClass());
	if (!Test.TestNotNull(TEXT("combined target is created"), Created))
	{
		return false;
	}
	CloseAllRollbackCaptures(Captures);
	const FAssetDocumentContractValidationResult First =
		Preparation.RollbackSeeds[0]->Rollback(TEXT("Test.Combined"));
	Test.TestTrue(TEXT("combined rollback deletes target"), First.bValid);
	Test.TestNull(TEXT("combined target is absent after first alias"),
		FSoftObjectPath(Intent.CanonicalObjectPath).ResolveObject());
	const FAssetDocumentContractValidationResult Second =
		Preparation.RollbackSeeds[1]->Rollback(TEXT("Test.CombinedAgain"));
	const FAssetDocumentContractValidationResult Third =
		Preparation.RollbackSeeds[2]->Rollback(TEXT("Test.CombinedThird"));
	TestValidationResultsEqual(
		Test, TEXT("combined second alias caches terminal"), Second, First);
	TestValidationResultsEqual(
		Test, TEXT("combined third alias caches terminal"), Third, First);
	Test.TestNull(TEXT("combined aliases delete exactly once"),
		FSoftObjectPath(Intent.CanonicalObjectPath).ResolveObject());
	return true;
}

bool TestPropertyHandlerParity(FAutomationTestBase& Test)
{
	FCanonicalPropertyAssetFixture Fixture;
	Fixture.SetBaseline(false);
	FProperty* Property = Fixture.BooleanProperty;
	if (!Test.TestNotNull(TEXT("cutover property exists"), Property)
		|| !Test.TestTrue(
			TEXT("cutover property asset exists"), Fixture.IsValid()))
	{
		return false;
	}

	FAssetDocumentEditorProviderTestContext Context;
	const FAssetDocumentEditorCutoverMutationIntent Intent =
		Fixture.MakeIntent(Context);
	const FAssetDocumentEditorCutoverTransactionPreparation Preparation =
		PrepareAssetDocumentEditorCutoverTransaction(Intent);
	if (!Test.TestTrue(TEXT("property parity prepares"), Preparation.bPrepared))
	{
		return false;
	}
	const TArray<FAssetDocumentMutationFootprint> Expected =
		MakeExpectedPropertyFootprint(Intent.CanonicalObjectPath);
	TestCutoverFootprintEquals(
		Test, TEXT("property parity preparation"),
		Preparation.CompleteFootprint, Expected);

	TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Descriptor =
		FindCutoverDescriptor(
			*Provider, TEXT("editor.property.write_admitted.v1"));
	const FAssetDocumentEditorTypedValueCodecResult Codec =
		InspectAssetDocumentEditorTypedValueProperty(*Property);
	if (!Test.TestNotNull(TEXT("property write descriptor exists"), Descriptor)
		|| !Test.TestTrue(TEXT("property codec is admitted"), Codec.bSucceeded))
	{
		return false;
	}

	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	const TOptional<FAssetDocumentOpaqueHandle> AssetHandle =
		IssueCutoverHandle(
			Test, Handles, TEXT("property asset handle issues"),
			EAssetDocumentHandleKind::Asset,
			EAssetDocumentHandleAccess::Mutable,
			MakeUnique<FCutoverTestObjectPayload>(
				*Fixture.Asset, AssetPayloadTypeId));
	const TOptional<FAssetDocumentOpaqueHandle> PropertyHandle =
		IssueCutoverHandle(
			Test, Handles, TEXT("property token handle issues"),
			EAssetDocumentHandleKind::Property,
			EAssetDocumentHandleAccess::ReadOnly,
			MakeUnique<FAssetDocumentEditorPropertyTokenPayload>(
				*Property->GetOwnerClass(), *Property, Codec.CodecId,
				true, true,
				GetAssetDocumentEditorPropertyTypeRevision(
					*Property->GetOwnerClass())));
	if (!AssetHandle.IsSet() || !PropertyHandle.IsSet())
	{
		return false;
	}
	FAssetDocumentPrimitiveInvocation Invocation =
		MakeAssetDocumentEditorProviderTestInvocation(*Descriptor, Context);
	Invocation.ValueArguments = MakeObjectValue(
		{TEXT("value")}, {MakeBooleanValue(true)});
	Invocation.HandleArguments = {
		{TEXT("asset"), AssetHandle.GetValue()},
		{TEXT("property"), PropertyHandle.GetValue()},
	};
	FAssetDocumentEditorProviderTestTransaction Transaction(Context);
	const FAssetDocumentPrimitiveOutcome Outcome =
		Provider->Invoke(Invocation, Handles, &Transaction);
	if (!Test.TestEqual(TEXT("property parity invoke succeeds"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
	{
		return false;
	}
	return TestCutoverFootprintEquals(
		Test, TEXT("property handler parity"),
		Transaction.GetEnlistedFootprint(), Expected);
}

bool TestEmbeddedHandlerParity(FAutomationTestBase& Test)
{
	TStrongObjectPtr<UTexture2D> Outer(UTexture2D::CreateTransient(1, 1));
	UClass* EmbeddedClass = USubtitleAssetUserData::StaticClass();
	if (!Test.TestTrue(TEXT("embedded parity outer exists"), Outer.IsValid())
		|| !Test.TestNotNull(
			TEXT("embedded parity class exists"), EmbeddedClass))
	{
		return false;
	}
	IInterface_AssetUserData* AssetUserDataOwner =
		static_cast<IInterface_AssetUserData*>(Outer.Get());
	ON_SCOPE_EXIT
	{
		AssetUserDataOwner->RemoveUserDataOfClass(EmbeddedClass);
	};

	TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Descriptor =
		FindCutoverDescriptor(
			*Provider, TEXT("editor.fragment.embedded_object_create.v1"));
	if (!Test.TestNotNull(TEXT("embedded parity descriptor exists"), Descriptor))
	{
		return false;
	}
	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	const TOptional<FAssetDocumentOpaqueHandle> OuterHandle =
		IssueCutoverHandle(
			Test, Handles, TEXT("embedded outer handle issues"),
			EAssetDocumentHandleKind::Asset,
			EAssetDocumentHandleAccess::Mutable,
			MakeUnique<FCutoverTestObjectPayload>(*Outer, AssetPayloadTypeId));
	const TOptional<FAssetDocumentOpaqueHandle> ClassHandle =
		IssueCutoverHandle(
			Test, Handles, TEXT("embedded class handle issues"),
			EAssetDocumentHandleKind::Class,
			EAssetDocumentHandleAccess::ReadOnly,
			MakeUnique<FTestClassPayload>(*EmbeddedClass));
	if (!OuterHandle.IsSet() || !ClassHandle.IsSet())
	{
		return false;
	}

	FAssetDocumentPrimitiveInvocation Invocation =
		MakeAssetDocumentEditorProviderTestInvocation(*Descriptor, Context);
	Invocation.ValueArguments = MakeObjectValue(
		{TEXT("category_id"), TEXT("properties")},
		{MakeStringValue(TEXT("asset_user_data.v1")),
			MakeObjectValue({}, {})});
	Invocation.HandleArguments = {
		{TEXT("outer_asset"), OuterHandle.GetValue()},
		{TEXT("object_class"), ClassHandle.GetValue()},
	};
	FAssetDocumentEditorProviderTestTransaction Transaction(Context);
	const FAssetDocumentPrimitiveOutcome Outcome =
		Provider->Invoke(Invocation, Handles, &Transaction);
	if (!Test.TestEqual(TEXT("embedded parity invoke succeeds"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
	{
		return false;
	}
	const TArray<FAssetDocumentMutationFootprint> Expected =
		MakeExpectedEmbeddedFootprint(Outer->GetPathName());
	return TestCutoverFootprintEquals(
		Test, TEXT("embedded handler parity"),
		Transaction.GetEnlistedFootprint(), Expected);
}

bool TestCapturedRollback(FAutomationTestBase& Test)
{
	FAssetDocumentEditorProviderTestContext Context;
	const FString Name = MakeTargetName();
	const FAssetDocumentEditorCutoverMutationIntent Intent =
		MakeCreateIntent(Context, Name);
	ON_SCOPE_EXIT
	{
		DeleteTargetIfPresent(Intent.CanonicalObjectPath);
	};
	const FAssetDocumentEditorCutoverTransactionPreparation Preparation =
		PrepareAssetDocumentEditorCutoverTransaction(Intent);
	if (!Test.TestTrue(TEXT("captured create prepares"),
		Preparation.bPrepared)
		|| !Test.TestEqual(TEXT("captured create has one seed"),
			Preparation.RollbackSeeds.Num(), 1))
	{
		return false;
	}
	TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
			CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Descriptor =
		FindCutoverDescriptor(
			*Provider, TEXT("editor.asset.create_by_factory.v1"));
	if (!Test.TestNotNull(TEXT("factory descriptor exists"), Descriptor))
	{
		return false;
	}
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	FAssetDocumentEditorProviderTestTransaction Transaction(Context);
	FAssetDocumentIssuedHandleResult ClassHandle = Handles.Issue(
		EAssetDocumentHandleKind::Class,
		EAssetDocumentHandleAccess::ReadOnly,
		MakeUnique<FTestClassPayload>(
			*UPrimaryAssetLabel::StaticClass()));
	if (!Test.TestTrue(TEXT("class handle issues"),
		ClassHandle.Handle.IsSet()))
	{
		return false;
	}
	TUniquePtr<IAssetDocumentEditorCutoverMutationCapture> Capture =
		Preparation.RollbackSeeds[0]->OpenMutationCapture(
			Context.TransactionId);
	if (!Test.TestTrue(TEXT("capture opens"), Capture.IsValid()))
	{
		return false;
	}
	FAssetDocumentPrimitiveInvocation Invocation =
		MakeAssetDocumentEditorProviderTestInvocation(*Descriptor, Context);
	Invocation.ValueArguments = MakeFactoryArguments(Test, Name);
	Invocation.HandleArguments.Add({
		TEXT("asset_class"), ClassHandle.Handle.GetValue()});
	const FAssetDocumentPrimitiveOutcome Outcome =
		Provider->Invoke(Invocation, Handles, &Transaction);
	Capture.Reset();
	if (!Test.TestEqual(TEXT("real factory create succeeds"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
	{
		return false;
	}
	TestCutoverFootprintEquals(
		Test, TEXT("factory handler parity"),
		Transaction.GetEnlistedFootprint(),
		Preparation.CompleteFootprint);
	const FAssetDocumentEditorCutoverTransactionPreparation Existing =
		PrepareAssetDocumentEditorCutoverTransaction(Intent);
	Test.TestFalse(TEXT("existing create target fails closed"),
		Existing.bPrepared);

	InvalidateHandles(Handles, Context);
	const FAssetDocumentContractValidationResult Rollback =
		Preparation.RollbackSeeds[0]->Rollback(
			TEXT("Test.AfterHandleInvalidation"));
	Test.TestTrue(TEXT("seed rolls back after handle invalidation"),
		Rollback.bValid);
	Test.TestNull(TEXT("rolled-back target is absent"),
		FSoftObjectPath(Intent.CanonicalObjectPath).ResolveObject());
	const FAssetDocumentContractValidationResult SecondRollback =
		Preparation.RollbackSeeds[0]->Rollback(TEXT("Test.Second"));
	Test.TestFalse(TEXT("second rollback fails closed"),
		SecondRollback.bValid);
	return true;
}

bool TestRollbackHandlerParity(FAutomationTestBase& Test)
{
	FAssetDocumentEditorProviderTestContext Context;
	const FString Name = MakeTargetName();
	const FAssetDocumentEditorCutoverMutationIntent Intent =
		MakeCreateIntent(Context, Name);
	ON_SCOPE_EXIT
	{
		DeleteTargetIfPresent(Intent.CanonicalObjectPath);
	};
	const FAssetDocumentEditorCutoverTransactionPreparation Preparation =
		PrepareAssetDocumentEditorCutoverTransaction(Intent);
	if (!Test.TestTrue(TEXT("rollback parity prepares"), Preparation.bPrepared))
	{
		return false;
	}
	const TArray<FAssetDocumentMutationFootprint> Expected =
		MakeExpectedFactoryFootprint(Intent.CanonicalObjectPath);
	TestCutoverFootprintEquals(
		Test, TEXT("rollback parity preparation"),
		Preparation.CompleteFootprint, Expected);

	TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* FactoryDescriptor =
		FindCutoverDescriptor(
			*Provider, TEXT("editor.asset.create_by_factory.v1"));
	const FAssetDocumentPrimitiveDescriptor* RollbackDescriptor =
		FindCutoverDescriptor(
			*Provider, TEXT("editor.asset.rollback_created.v1"));
	if (!Test.TestNotNull(
		TEXT("rollback parity factory descriptor exists"), FactoryDescriptor)
		|| !Test.TestNotNull(
			TEXT("rollback parity descriptor exists"), RollbackDescriptor))
	{
		return false;
	}

	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	const TOptional<FAssetDocumentOpaqueHandle> ClassHandle =
		IssueCutoverHandle(
			Test, Handles, TEXT("rollback parity class handle issues"),
			EAssetDocumentHandleKind::Class,
			EAssetDocumentHandleAccess::ReadOnly,
			MakeUnique<FTestClassPayload>(
				*UPrimaryAssetLabel::StaticClass()));
	if (!ClassHandle.IsSet())
	{
		return false;
	}
	FAssetDocumentPrimitiveInvocation CreateInvocation =
		MakeAssetDocumentEditorProviderTestInvocation(
			*FactoryDescriptor, Context);
	CreateInvocation.ValueArguments = MakeFactoryArguments(Test, Name);
	CreateInvocation.HandleArguments.Add(
		{TEXT("asset_class"), ClassHandle.GetValue()});
	FAssetDocumentEditorProviderTestTransaction CreationTransaction(Context);
	const FAssetDocumentPrimitiveOutcome CreateOutcome =
		Provider->Invoke(CreateInvocation, Handles, &CreationTransaction);
	const FAssetDocumentOpaqueHandle* AssetHandle =
		FindCutoverOutcomeHandle(CreateOutcome, TEXT("asset_mutable"));
	const FAssetDocumentOpaqueHandle* PackageHandle =
		FindCutoverOutcomeHandle(CreateOutcome, TEXT("package"));
	if (!Test.TestEqual(TEXT("rollback parity fixture creates"),
		CreateOutcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
		|| !Test.TestNotNull(
			TEXT("rollback parity asset handle exists"), AssetHandle)
		|| !Test.TestNotNull(
			TEXT("rollback parity package handle exists"), PackageHandle))
	{
		return false;
	}

	FAssetDocumentPrimitiveInvocation RollbackInvocation =
		MakeAssetDocumentEditorProviderTestInvocation(
			*RollbackDescriptor, Context);
	RollbackInvocation.HandleArguments = {
		{TEXT("asset"), *AssetHandle},
		{TEXT("package"), *PackageHandle},
	};
	FAssetDocumentEditorProviderTestTransaction RollbackTransaction(Context);
	const FAssetDocumentPrimitiveOutcome RollbackOutcome =
		Provider->Invoke(RollbackInvocation, Handles, &RollbackTransaction);
	if (!Test.TestEqual(TEXT("rollback parity invoke succeeds"),
		RollbackOutcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
	{
		return false;
	}
	Test.TestNull(TEXT("rollback parity target is absent"),
		FSoftObjectPath(Intent.CanonicalObjectPath).ResolveObject());
	return TestCutoverFootprintEquals(
		Test, TEXT("rollback handler parity"),
		RollbackTransaction.GetEnlistedFootprint(), Expected);
}

UObject* CreateCutoverAsset(
	const FAssetDocumentEditorCutoverMutationIntent& Intent,
	UClass& AssetClass)
{
	const FSoftObjectPath TargetPath(Intent.CanonicalObjectPath);
	return IAssetTools::Get().CreateAsset(
		TargetPath.GetAssetName(),
		FPackageName::GetLongPackagePath(TargetPath.GetLongPackageName()),
		&AssetClass,
		nullptr);
}

bool TestCaptureFailureClosed(FAutomationTestBase& Test)
{
	{
		FAssetDocumentEditorProviderTestContext Context;
		const FAssetDocumentEditorCutoverMutationIntent Intent =
			MakeCreateIntent(Context, MakeTargetName());
		ON_SCOPE_EXIT
		{
			DeleteTargetIfPresent(Intent.CanonicalObjectPath);
		};
		const FAssetDocumentEditorCutoverTransactionPreparation Preparation =
			PrepareAssetDocumentEditorCutoverTransaction(Intent);
		if (!Test.TestTrue(TEXT("mismatched capture prepares"),
			Preparation.bPrepared))
		{
			return false;
		}
		TUniquePtr<IAssetDocumentEditorCutoverMutationCapture> Capture =
			Preparation.RollbackSeeds[0]->OpenMutationCapture(
				Context.TransactionId);
		if (!Test.TestTrue(TEXT("mismatched capture opens"), Capture.IsValid())
			|| !Test.TestNotNull(TEXT("wrong-class target is created"),
				CreateCutoverAsset(Intent, *UTexture2D::StaticClass())))
		{
			return false;
		}
		Capture.Reset();
		const FAssetDocumentContractValidationResult Rollback =
			Preparation.RollbackSeeds[0]->Rollback(TEXT("Test.Mismatch"));
		Test.TestFalse(TEXT("mismatched event poisons rollback"),
			Rollback.bValid);
		Test.TestTrue(TEXT("mismatched event diagnostic is exact"),
			HasCutoverDiagnostic(
				Rollback, TEXT("CutoverRollback.PoisonedCapture")));
		Test.TestNotNull(TEXT("poisoned rollback preserves target"),
			FSoftObjectPath(Intent.CanonicalObjectPath).ResolveObject());
	}

	{
		FAssetDocumentEditorProviderTestContext Context;
		const FAssetDocumentEditorCutoverMutationIntent Intent =
			MakeCreateIntent(Context, MakeTargetName());
		ON_SCOPE_EXIT
		{
			DeleteTargetIfPresent(Intent.CanonicalObjectPath);
		};
		const FAssetDocumentEditorCutoverTransactionPreparation Preparation =
			PrepareAssetDocumentEditorCutoverTransaction(Intent);
		if (!Test.TestTrue(TEXT("duplicate capture prepares"),
			Preparation.bPrepared))
		{
			return false;
		}
		TUniquePtr<IAssetDocumentEditorCutoverMutationCapture> Capture =
			Preparation.RollbackSeeds[0]->OpenMutationCapture(
				Context.TransactionId);
		UObject* Created = Capture.IsValid()
			? CreateCutoverAsset(Intent, *UPrimaryAssetLabel::StaticClass())
			: nullptr;
		if (!Test.TestTrue(TEXT("duplicate capture opens"), Capture.IsValid())
			|| !Test.TestNotNull(
				TEXT("duplicate capture target is created"), Created))
		{
			return false;
		}
		FAssetRegistryModule::AssetCreated(Created);
		Capture.Reset();
		const FAssetDocumentContractValidationResult Rollback =
			Preparation.RollbackSeeds[0]->Rollback(TEXT("Test.Duplicate"));
		Test.TestFalse(TEXT("duplicate event poisons rollback"),
			Rollback.bValid);
		Test.TestTrue(TEXT("duplicate event diagnostic is exact"),
			HasCutoverDiagnostic(
				Rollback, TEXT("CutoverRollback.PoisonedCapture")));
		Test.TestNotNull(TEXT("duplicate rollback preserves target"),
			FSoftObjectPath(Intent.CanonicalObjectPath).ResolveObject());
	}

	{
		FAssetDocumentEditorProviderTestContext Context;
		const FAssetDocumentEditorCutoverMutationIntent Intent =
			MakeCreateIntent(Context, MakeTargetName());
		ON_SCOPE_EXIT
		{
			DeleteTargetIfPresent(Intent.CanonicalObjectPath);
		};
		const FAssetDocumentEditorCutoverTransactionPreparation Preparation =
			PrepareAssetDocumentEditorCutoverTransaction(Intent);
		if (!Test.TestTrue(TEXT("missing capture prepares"),
			Preparation.bPrepared))
		{
			return false;
		}
		TUniquePtr<IAssetDocumentEditorCutoverMutationCapture> Capture =
			Preparation.RollbackSeeds[0]->OpenMutationCapture(
				Context.TransactionId);
		if (!Test.TestTrue(TEXT("missing capture opens"), Capture.IsValid()))
		{
			return false;
		}
		Capture.Reset();
		if (!Test.TestNotNull(TEXT("post-capture target is created"),
			CreateCutoverAsset(Intent, *UPrimaryAssetLabel::StaticClass())))
		{
			return false;
		}
		const FAssetDocumentContractValidationResult Rollback =
			Preparation.RollbackSeeds[0]->Rollback(TEXT("Test.MissingCapture"));
		Test.TestFalse(TEXT("missing event fails rollback"), Rollback.bValid);
		Test.TestTrue(TEXT("missing event diagnostic is exact"),
			HasCutoverDiagnostic(
				Rollback, TEXT("CutoverRollback.MissingCapture")));
		Test.TestNotNull(TEXT("missing-capture rollback preserves target"),
			FSoftObjectPath(Intent.CanonicalObjectPath).ResolveObject());
	}
	return true;
}

bool TestFailureClosed(FAutomationTestBase& Test)
{
	const FAssetDocumentEditorCutoverTransactionPreparation Invalid =
		PrepareAssetDocumentEditorCutoverTransaction({});
	Test.TestFalse(TEXT("empty intent fails closed"), Invalid.bPrepared);
	Test.TestTrue(TEXT("invalid intent has a stable diagnostic"),
		!Invalid.Diagnostics.IsEmpty()
			&& Invalid.Diagnostics[0].Code
				== TEXT("CutoverPreparation.InvalidIntent"));

	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorCutoverMutationIntent WrongFactory =
		MakeCreateIntent(Context, MakeTargetName());
	WrongFactory.FactoryId = TEXT("unsupported");
	Test.TestFalse(TEXT("wrong factory fails closed"),
		PrepareAssetDocumentEditorCutoverTransaction(
			WrongFactory).bPrepared);

	FAssetDocumentEditorCutoverMutationIntent WrongClass =
		MakeCreateIntent(Context, MakeTargetName());
	WrongClass.ExactAssetClassPath = UTexture2D::StaticClass()->GetPathName();
	Test.TestFalse(TEXT("wrong factory class fails closed"),
		PrepareAssetDocumentEditorCutoverTransaction(WrongClass).bPrepared);

	FAssetDocumentEditorCutoverMutationIntent MalformedPath =
		MakeCreateIntent(Context, MakeTargetName());
	MalformedPath.CanonicalObjectPath = TEXT("/Game/AssetDocumentTests/MissingDot");
	Test.TestFalse(TEXT("malformed target path fails closed"),
		PrepareAssetDocumentEditorCutoverTransaction(MalformedPath).bPrepared);

	FAssetDocumentEditorCutoverMutationIntent WrongLeaf =
		MakeCreateIntent(Context, MakeTargetName());
	WrongLeaf.CanonicalObjectPath =
		TEXT("/Game/AssetDocumentTests/WrongLeaf.OtherAsset");
	Test.TestFalse(TEXT("mismatched package leaf fails closed"),
		PrepareAssetDocumentEditorCutoverTransaction(WrongLeaf).bPrepared);

	FAssetDocumentEditorCutoverMutationIntent MalformedClass =
		MakeCreateIntent(Context, MakeTargetName());
	MalformedClass.ExactAssetClassPath = TEXT("NotAClass");
	Test.TestFalse(TEXT("malformed class path fails closed"),
		PrepareAssetDocumentEditorCutoverTransaction(MalformedClass).bPrepared);

	const FAssetDocumentEditorCutoverMutationIntent ZeroIntent =
		MakeCreateIntent(Context, MakeTargetName());
	const FAssetDocumentEditorCutoverTransactionPreparation Zero =
		PrepareAssetDocumentEditorCutoverTransaction(ZeroIntent);
	if (!Test.TestTrue(TEXT("zero-mutation target prepares"),
		Zero.bPrepared))
	{
		return false;
	}
	const FAssetDocumentContractValidationResult ZeroRollback =
		Zero.RollbackSeeds[0]->Rollback(TEXT("Test.ZeroMutation"));
	Test.TestTrue(TEXT("zero-mutation rollback succeeds"),
		ZeroRollback.bValid);

	const FAssetDocumentEditorCutoverTransactionPreparation CapturePreparation =
		PrepareAssetDocumentEditorCutoverTransaction(
			MakeCreateIntent(Context, MakeTargetName()));
	Test.TestFalse(TEXT("invalid transaction capture is rejected"),
		CapturePreparation.RollbackSeeds[0]->OpenMutationCapture(
			FGuid()).IsValid());
	TUniquePtr<IAssetDocumentEditorCutoverMutationCapture> First =
		CapturePreparation.RollbackSeeds[0]->OpenMutationCapture(
			Context.TransactionId);
	Test.TestTrue(TEXT("first capture opens"), First.IsValid());
	Test.TestFalse(TEXT("simultaneous capture is rejected"),
		CapturePreparation.RollbackSeeds[0]->OpenMutationCapture(
			Context.TransactionId).IsValid());
	First.Reset();
	return true;
}

bool TestExistingTargetAcquisitionOnlyIntent(FAutomationTestBase& Test)
{
	FCanonicalPropertyAssetFixture Fixture;
	if (!Test.TestTrue(TEXT("acquisition fixture is valid"), Fixture.IsValid()))
	{
		return false;
	}
	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorCutoverMutationIntent Intent = Fixture.MakeIntent(Context);
	Intent.bWriteAdmittedProperties = false;
	const FAssetDocumentEditorCutoverTransactionPreparation Preparation =
		PrepareAssetDocumentEditorCutoverTransaction(Intent);
	Test.TestTrue(TEXT("existing target acquisition-only intent prepares"),
		Preparation.bPrepared);
	Test.TestEqual(TEXT("acquisition-only has zero editor rows"),
		Preparation.CompleteFootprint.Num(), 0);
	Test.TestEqual(TEXT("acquisition-only has zero rollback seeds"),
		Preparation.RollbackSeeds.Num(), 0);
	return Preparation.bPrepared && Preparation.CompleteFootprint.IsEmpty()
		&& Preparation.RollbackSeeds.IsEmpty();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentEditorCutoverTransactionPreparationTest,
	"UEAssetDoc.GenericProvider.CutoverTransactionPreparation.FullMatrix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEditorCutoverTransactionPreparationTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorCutoverPreparationTests;
	const bool bShapes = TestPreparationShapes(*this);
	const bool bPropertyParity =
		bShapes && TestPropertyHandlerParity(*this);
	const bool bEmbeddedParity =
		bPropertyParity && TestEmbeddedHandlerParity(*this);
	const bool bCapturedRollback =
		bEmbeddedParity && TestCapturedRollback(*this);
	const bool bRollbackParity =
		bCapturedRollback && TestRollbackHandlerParity(*this);
	const bool bFailureClosed =
		bRollbackParity && TestFailureClosed(*this);
	const bool bCaptureFailureClosed =
		bFailureClosed && TestCaptureFailureClosed(*this);
	const bool bAcquisitionOnly = bCaptureFailureClosed
		&& TestExistingTargetAcquisitionOnlyIntent(*this);
	return bShapes && bPropertyParity && bEmbeddedParity
		&& bCapturedRollback && bRollbackParity
		&& bFailureClosed && bCaptureFailureClosed && bAcquisitionOnly;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentEditorExistingPropertyRollbackTest,
	"UEAssetDoc.GenericProvider."
	"CutoverTransactionPreparation.ExistingPropertyRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEditorExistingPropertyRollbackTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorCutoverPreparationTests;
	bool bPassed = TestExistingPropertyShapeAndRestore(*this);
	bPassed &= TestExistingPropertyIdentityFailureCached(*this);
	bPassed &= TestExistingPropertyAccessFailureCached(*this);
	bPassed &= TestExistingPropertyOpenRejections(*this);
	bPassed &= TestExistingPropertyMissingCaptureCached(*this);
	bPassed &= TestExistingPropertyActiveLeaseCached(*this);
	bPassed &= TestExistingPropertyInvalidTargets(*this);
	return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentEditorCombinedRollbackAliasesTest,
	"UEAssetDoc.GenericProvider."
	"CutoverTransactionPreparation.CombinedRollbackAliases",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEditorCombinedRollbackAliasesTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	return AssetDocumentEditorCutoverPreparationTests::
		TestCombinedRollbackAliases(*this);
}
#endif
