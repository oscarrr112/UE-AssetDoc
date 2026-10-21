// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentEditorPrimitiveProvider.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetDocumentEditorGenericProviderTestTypes.h"
#include "AssetDocumentEditorProviderTestSupport.h"
#include "Engine/DataAsset.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Provider/AssetDocumentEditorNativeIdentity.h"
#include "Provider/AssetDocumentEditorTypedValueCodec.h"
#include "Provider/AssetDocumentEditorValueDto.h"
#include "Provider/Property/AssetDocumentEditorPropertyTokenPayload.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

#include <atomic>

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorPropertyProviderTests
{
const FAssetDocumentPrimitiveDescriptor* FindDescriptor(
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

FAssetDocumentRuntimeValueDto MakePropertyTestStringValue(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto MakePropertyTestBooleanValue(bool bValue)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Boolean;
	Result.BooleanValue = bValue;
	return Result;
}

FAssetDocumentRuntimeValueDto MakeExactClassArguments(FString ClassPath)
{
	FAssetDocumentRuntimeValueDto Arguments;
	FString DiagnosticCode;
	const TArray<FAssetDocumentEditorValueField> Fields = {
		{TEXT("exact_asset_class_path"),
			MakePropertyTestStringValue(MoveTemp(ClassPath))},
	};
	verify(TryBuildAssetDocumentEditorValueObject(
		Fields, Arguments, DiagnosticCode));
	return Arguments;
}

FAssetDocumentRuntimeValueDto MakePropertyWriteArguments(bool bValue)
{
	FAssetDocumentRuntimeValueDto Arguments;
	FString DiagnosticCode;
	const TArray<FAssetDocumentEditorValueField> Fields = {
		{TEXT("value"), MakePropertyTestBooleanValue(bValue)},
	};
	verify(TryBuildAssetDocumentEditorValueObject(
		Fields, Arguments, DiagnosticCode));
	return Arguments;
}

bool AreValuesEqual(
	const FAssetDocumentRuntimeValueDto& Left,
	const FAssetDocumentRuntimeValueDto& Right)
{
	if (Left.Kind != Right.Kind
		|| Left.BooleanValue != Right.BooleanValue
		|| Left.IntegerValue != Right.IntegerValue
		|| Left.NumberValue != Right.NumberValue
		|| Left.StringValue != Right.StringValue
		|| Left.ObjectKeys != Right.ObjectKeys
		|| Left.Elements.Num() != Right.Elements.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < Left.Elements.Num(); ++Index)
	{
		if (!AreValuesEqual(Left.Elements[Index], Right.Elements[Index]))
		{
			return false;
		}
	}
	return true;
}

const FAssetDocumentRuntimeValueDto* FindField(
	const FAssetDocumentRuntimeValueDto& Object,
	FStringView Name)
{
	const int32 Index = Object.ObjectKeys.IndexOfByPredicate(
		[Name](const FString& Key) { return Key == Name; });
	return Object.Elements.IsValidIndex(Index) ? &Object.Elements[Index] : nullptr;
}

FStringView ReadStringField(
	const FAssetDocumentRuntimeValueDto& Object,
	FStringView Name)
{
	const FAssetDocumentRuntimeValueDto* Value = FindField(Object, Name);
	return Value != nullptr
		&& Value->Kind == EAssetDocumentRuntimeValueKind::String
		? FStringView(Value->StringValue)
		: FStringView();
}

const FAssetDocumentPrimitiveHandleResult* FindHandleGroup(
	const FAssetDocumentPrimitiveOutcome& Outcome,
	FStringView Name)
{
	return Outcome.HandleResults.FindByPredicate(
		[Name](const FAssetDocumentPrimitiveHandleResult& Result)
		{
			return Result.Name == Name;
		});
}

class FScopedEditableFixtureProperties final
{
public:
	FScopedEditableFixtureProperties()
	{
		for (TFieldIterator<FProperty> It(
			UAssetDocumentEditorCodecTestObject::StaticClass()); It; ++It)
		{
			FProperty& Property = **It;
			if (!Property.HasAnyPropertyFlags(CPF_Edit))
			{
				Changed.Add(&Property);
				Property.SetPropertyFlags(CPF_Edit);
			}
		}
	}

	~FScopedEditableFixtureProperties()
	{
		for (FProperty* Property : Changed)
		{
			Property->ClearPropertyFlags(CPF_Edit);
		}
	}

private:
	TArray<FProperty*> Changed;
};

class FPropertyTestObjectPayload final
	: public UE::AssetDocument::EditorPrimitive::INativeIdentityPayload
{
public:
	FPropertyTestObjectPayload(
		UObject& InObject,
		FName InPayloadTypeId,
		uint64 InTypeRevision)
		: Object(&InObject)
		, CanonicalPath(InObject.GetPathName())
		, PayloadTypeId(InPayloadTypeId)
		, TypeRevision(InTypeRevision)
	{
	}

	virtual FName GetPayloadTypeId() const override
	{
		return PayloadTypeId;
	}

	virtual bool IsAlive() const override
	{
		return Object.IsValid();
	}

	virtual bool TryGetObjectIdentity(
		UE::AssetDocument::EditorPrimitive::FObjectIdentityView& OutView)
		const override
	{
		UObject* Resolved = Object.Get();
		if (Resolved == nullptr)
		{
			return false;
		}
		OutView.Object = Resolved;
		OutView.CanonicalObjectPath = CanonicalPath;
		OutView.TypeRevision = TypeRevision;
		return true;
	}

private:
	TWeakObjectPtr<UObject> Object;
	FString CanonicalPath;
	FName PayloadTypeId;
	uint64 TypeRevision = 0;
};

class FPropertyTestOpaquePayload final
	: public IAssetDocumentNativeHandlePayload
{
public:
	explicit FPropertyTestOpaquePayload(FName InPayloadTypeId)
		: PayloadTypeId(InPayloadTypeId)
	{
	}

	virtual FName GetPayloadTypeId() const override
	{
		return PayloadTypeId;
	}

	virtual bool IsAlive() const override
	{
		return true;
	}

private:
	FName PayloadTypeId;
};

struct FPropertyTestLiveness
{
	bool bAlive = true;
};

class FPropertyTestTogglePayload final
	: public IAssetDocumentNativeHandlePayload
{
public:
	explicit FPropertyTestTogglePayload(
		const TSharedRef<FPropertyTestLiveness>& InLiveness)
		: Liveness(InLiveness)
	{
	}

	virtual FName GetPayloadTypeId() const override
	{
		return FName(TEXT("assetdocument.test.stale_property.v1"));
	}

	virtual bool IsAlive() const override
	{
		return Liveness->bAlive;
	}

private:
	TSharedRef<FPropertyTestLiveness> Liveness;
};

class FPropertyTestRegistryObserver final
{
public:
	FPropertyTestRegistryObserver(
		FString InSourcePath,
		FString InScratchPath)
		: SourcePath(MoveTemp(InSourcePath))
		, ScratchPath(MoveTemp(InScratchPath))
	{
		IAssetRegistry& Registry = IAssetRegistry::GetChecked();
		AddedHandle = Registry.OnAssetAdded().AddRaw(
			this, &FPropertyTestRegistryObserver::OnAssetChanged);
		RemovedHandle = Registry.OnAssetRemoved().AddRaw(
			this, &FPropertyTestRegistryObserver::OnAssetChanged);
		UpdatedHandle = Registry.OnAssetUpdated().AddRaw(
			this, &FPropertyTestRegistryObserver::OnAssetChanged);
		RenamedHandle = Registry.OnAssetRenamed().AddRaw(
			this, &FPropertyTestRegistryObserver::OnAssetRenamed);
	}

	~FPropertyTestRegistryObserver()
	{
		if (IAssetRegistry* Registry = IAssetRegistry::Get())
		{
			Registry->OnAssetAdded().Remove(AddedHandle);
			Registry->OnAssetRemoved().Remove(RemovedHandle);
			Registry->OnAssetUpdated().Remove(UpdatedHandle);
			Registry->OnAssetRenamed().Remove(RenamedHandle);
		}
	}

	int32 GetMutationCount() const
	{
		return MutationCount.load(std::memory_order_relaxed);
	}

private:
	bool IsObservedPath(FStringView Path) const
	{
		return Path == SourcePath || Path == ScratchPath;
	}

	void OnAssetChanged(const FAssetData& AssetData)
	{
		if (IsObservedPath(AssetData.GetObjectPathString()))
		{
			MutationCount.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void OnAssetRenamed(
		const FAssetData& AssetData,
		const FString& OldObjectPath)
	{
		if (IsObservedPath(AssetData.GetObjectPathString())
			|| IsObservedPath(OldObjectPath))
		{
			MutationCount.fetch_add(1, std::memory_order_relaxed);
		}
	}

	FString SourcePath;
	FString ScratchPath;
	std::atomic<int32> MutationCount{0};
	FDelegateHandle AddedHandle;
	FDelegateHandle RemovedHandle;
	FDelegateHandle UpdatedHandle;
	FDelegateHandle RenamedHandle;
};

class FPropertyTestRejectingTransaction final
	: public IAssetDocumentAtomicTransaction
{
public:
	explicit FPropertyTestRejectingTransaction(
		const FAssetDocumentEditorProviderTestContext& Context)
	{
		Token.RequestId = Context.RequestId;
		Token.TransactionId = Context.TransactionId;
		Token.RuntimeRevision = Context.RuntimeRevision;
		RollbackActionEnlistmentResult.bValid = true;
		PostRollbackFinalizerEnlistmentResult.bValid = true;
	}

	virtual FGuid GetTransactionId() const override
	{
		return Token.TransactionId;
	}

	virtual EAssetDocumentAtomicState GetState() const override
	{
		return EAssetDocumentAtomicState::Live;
	}

	virtual const FAssetDocumentMutationToken& GetMutationToken() const override
	{
		return Token;
	}

	virtual FAssetDocumentContractValidationResult EnlistFootprint(
		TConstArrayView<FAssetDocumentMutationFootprint>) override
	{
		FAssetDocumentContractValidationResult Result;
		FAssetDocumentRuntimeDiagnostic& Diagnostic =
			Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/Footprint");
		Diagnostic.Code = TEXT("Atomic.TestInjectedEnlistFailure");
		Diagnostic.Message = TEXT("The test transaction rejected the footprint.");
		return Result;
	}

	virtual FAssetDocumentContractValidationResult MarkReadyToCommit() override
	{
		return {};
	}

	virtual FAssetDocumentContractValidationResult Commit(
		const FAssetDocumentDurableCompletionReceipt&) override
	{
		return {};
	}

	virtual FAssetDocumentContractValidationResult Rollback(FStringView) override
	{
		return {};
	}

	virtual FAssetDocumentContractValidationResult EnlistRollbackAction(
		TSharedRef<IAssetDocumentRollbackAction> Action) override
	{
		EnlistedRollbackActionComponentIds.Add(
			FString(Action->GetRollbackComponentId()));
		return RollbackActionEnlistmentResult;
	}

	virtual FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(
		TSharedRef<IAssetDocumentPostRollbackFinalizer>) override
	{
		return PostRollbackFinalizerEnlistmentResult;
	}

	TArray<FString> EnlistedRollbackActionComponentIds;
	FAssetDocumentContractValidationResult RollbackActionEnlistmentResult;
	FAssetDocumentContractValidationResult PostRollbackFinalizerEnlistmentResult;

private:
	FAssetDocumentMutationToken Token;
};

FProperty* FindPropertyTestFixtureField(FStringView AuthoredName)
{
	for (TFieldIterator<FProperty> It(
		UAssetDocumentEditorCodecTestObject::StaticClass()); It; ++It)
	{
		if (It->GetAuthoredName() == AuthoredName)
		{
			return *It;
		}
	}
	return nullptr;
}

TOptional<FAssetDocumentOpaqueHandle> IssuePropertyTestPayload(
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	EAssetDocumentHandleKind Kind,
	EAssetDocumentHandleAccess Access,
	TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload)
{
	FAssetDocumentIssuedHandleResult Issued =
		Handles.Issue(Kind, Access, MoveTemp(Payload));
	return MoveTemp(Issued.Handle);
}

TOptional<FAssetDocumentOpaqueHandle> IssuePropertyTestAsset(
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	UObject& Object,
	EAssetDocumentHandleAccess Access =
		EAssetDocumentHandleAccess::ReadOnly)
{
	return IssuePropertyTestPayload(
		Handles,
		EAssetDocumentHandleKind::Asset,
		Access,
		MakeUnique<FPropertyTestObjectPayload>(
			Object,
			UE::AssetDocument::EditorPrimitive::AssetPayloadTypeId,
			static_cast<uint64>(Object.GetUniqueID())));
}

TOptional<FAssetDocumentOpaqueHandle> IssuePropertyTestScratch(
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	UObject& Object,
	EAssetDocumentHandleKind Kind = EAssetDocumentHandleKind::Scratch,
	EAssetDocumentHandleAccess Access =
		EAssetDocumentHandleAccess::Mutable)
{
	return IssuePropertyTestPayload(
		Handles,
		Kind,
		Access,
		MakeUnique<FPropertyTestObjectPayload>(
			Object,
			UE::AssetDocument::EditorPrimitive::ScratchPayloadTypeId,
			1));
}

TOptional<FAssetDocumentOpaqueHandle> IssuePropertyTestToken(
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	FProperty& Property,
	bool bReadable = true,
	bool bWritable = true)
{
	const FAssetDocumentEditorTypedValueCodecResult Codec =
		InspectAssetDocumentEditorTypedValueProperty(Property);
	UClass* DeclaringClass = Property.GetOwnerClass();
	if (!Codec.bSucceeded || DeclaringClass == nullptr)
	{
		return {};
	}
	return IssuePropertyTestPayload(
		Handles,
		EAssetDocumentHandleKind::Property,
		EAssetDocumentHandleAccess::ReadOnly,
		MakeUnique<FAssetDocumentEditorPropertyTokenPayload>(
			*DeclaringClass,
			Property,
			Codec.CodecId,
			bReadable,
			bWritable,
			GetAssetDocumentEditorPropertyTypeRevision(*DeclaringClass)));
}

FAssetDocumentPrimitiveOutcome InvokePropertyTestRead(
	IAssetDocumentPrimitiveProvider& Provider,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentEditorProviderTestContext& Context,
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	FAssetDocumentOpaqueHandle Asset,
	FAssetDocumentOpaqueHandle Property,
	FAssetDocumentRuntimeValueDto Arguments = FAssetDocumentRuntimeValueDto())
{
	FAssetDocumentPrimitiveInvocation Invocation =
		MakeAssetDocumentEditorProviderTestInvocation(Descriptor, Context);
	Invocation.ValueArguments = MoveTemp(Arguments);
	Invocation.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Object;
	Invocation.HandleArguments = {
		{TEXT("asset"), MoveTemp(Asset)},
		{TEXT("property"), MoveTemp(Property)},
	};
	return Provider.Invoke(Invocation, Handles, nullptr);
}

FAssetDocumentPrimitiveOutcome InvokePropertyTestWrite(
	IAssetDocumentPrimitiveProvider& Provider,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentEditorProviderTestContext& Context,
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	FAssetDocumentOpaqueHandle Asset,
	FAssetDocumentOpaqueHandle Property,
	bool bValue,
	IAssetDocumentAtomicTransaction* Transaction)
{
	FAssetDocumentPrimitiveInvocation Invocation =
		MakeAssetDocumentEditorProviderTestInvocation(Descriptor, Context);
	Invocation.ValueArguments = MakePropertyWriteArguments(bValue);
	Invocation.HandleArguments = {
		{TEXT("asset"), MoveTemp(Asset)},
		{TEXT("property"), MoveTemp(Property)},
	};
	return Provider.Invoke(Invocation, Handles, Transaction);
}

FAssetDocumentPrimitiveOutcome InvokePropertyTestPreflight(
	IAssetDocumentPrimitiveProvider& Provider,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentEditorProviderTestContext& Context,
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	FAssetDocumentOpaqueHandle Scratch,
	FAssetDocumentOpaqueHandle Property,
	bool bValue)
{
	FAssetDocumentPrimitiveInvocation Invocation =
		MakeAssetDocumentEditorProviderTestInvocation(Descriptor, Context);
	Invocation.ValueArguments = MakePropertyWriteArguments(bValue);
	Invocation.HandleArguments = {
		{TEXT("scratch"), MoveTemp(Scratch)},
		{TEXT("property"), MoveTemp(Property)},
	};
	return Provider.Invoke(Invocation, Handles, nullptr);
}

FAssetDocumentHandleValidationContext MakePropertyTestScratchContext(
	const FAssetDocumentEditorProviderTestContext& Context)
{
	FAssetDocumentHandleValidationContext Validation;
	Validation.RequestId = Context.RequestId;
	Validation.TransactionId = Context.TransactionId;
	Validation.RuntimeRevision = Context.RuntimeRevision;
	Validation.ProviderId = Context.ProviderId;
	Validation.Phase = EAssetDocumentRuntimeRequestPhase::Preflight;
	Validation.RequiredKind = EAssetDocumentHandleKind::Scratch;
	Validation.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
	return Validation;
}

void TestPropertyTokenRejected(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveOutcome& Outcome,
	const TCHAR* Description,
	const TCHAR* ExpectedCode)
{
	Test.TestEqual(
		Description,
		Outcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Test.TestTrue(TEXT("forged token returns no handles"),
		Outcome.HandleResults.IsEmpty());
	Test.TestTrue(TEXT("forged token returns no changed records"),
		Outcome.ChangedSet.IsEmpty());
	Test.TestTrue(TEXT("forged token returns a stable diagnostic"),
		!Outcome.Diagnostics.IsEmpty());
	if (!Outcome.Diagnostics.IsEmpty())
	{
		Test.TestEqual(TEXT("forged token diagnostic is exact"),
			Outcome.Diagnostics[0].Code, FString(ExpectedCode));
	}
}

FAssetDocumentPrimitiveOutcome InvokeEnumeration(
	IAssetDocumentPrimitiveProvider& Provider,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentEditorProviderTestContext& Context,
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	FAssetDocumentRuntimeValueDto Arguments,
	IAssetDocumentAtomicTransaction* Transaction = nullptr)
{
	FAssetDocumentPrimitiveInvocation Invocation =
		MakeAssetDocumentEditorProviderTestInvocation(Descriptor, Context);
	Invocation.ValueArguments = MoveTemp(Arguments);
	return Provider.Invoke(Invocation, Handles, Transaction);
}

FString DescribePropertyOutcome(
	const FAssetDocumentPrimitiveOutcome& Outcome)
{
	FString Description = FString::Printf(
		TEXT("status=%d"), static_cast<int32>(Outcome.Status));
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Outcome.Diagnostics)
	{
		Description += FString::Printf(
			TEXT(" [%s:%s]"), *Diagnostic.Code, *Diagnostic.Message);
	}
	return Description;
}

void TestRejectedShape(
	FAutomationTestBase& Test,
	IAssetDocumentPrimitiveProvider& Provider,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentEditorProviderTestContext& Context,
	FAssetDocumentRuntimeValueDto Arguments,
	const TCHAR* Description)
{
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	const FAssetDocumentPrimitiveOutcome Outcome = InvokeEnumeration(
		Provider, Descriptor, Context, Handles, MoveTemp(Arguments));
	Test.TestEqual(
		Description,
		Outcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Test.TestTrue(
		TEXT("rejected enumeration returns no property handles"),
		Outcome.HandleResults.IsEmpty());
}
} // namespace AssetDocumentEditorPropertyProviderTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderPropertyDeterministicEnumerationTest,
	"UEAssetDoc.GenericProvider.Property.DeterministicEnumeration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderPropertyDeterministicEnumerationTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorPropertyProviderTests;

	FScopedEditableFixtureProperties EditableFixture;
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Descriptor = FindDescriptor(
		*Provider, TEXT("editor.property.enumerate_admitted.v1"));
	if (!TestNotNull(TEXT("property enumeration descriptor exists"), Descriptor))
	{
		return false;
	}

	const FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	const FString CanonicalClassPath =
		UAssetDocumentEditorCodecTestObject::StaticClass()->GetPathName();
	const FAssetDocumentPrimitiveOutcome First = InvokeEnumeration(
		*Provider, *Descriptor, Context, Handles,
		MakeExactClassArguments(CanonicalClassPath));
	if (First.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
	{
		AddError(FString::Printf(
			TEXT("canonical exact class enumeration failed: %s"),
			*DescribePropertyOutcome(First)));
		return false;
	}
	TestEqual(TEXT("canonical exact class enumeration succeeds"),
		First.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);

	const FAssetDocumentRuntimeValueDto* FirstRows =
		FindField(First.Value, TEXT("properties"));
	const FAssetDocumentPrimitiveHandleResult* FirstHandles =
		FindHandleGroup(First, TEXT("properties"));
	if (!TestNotNull(TEXT("enumeration returns property rows"), FirstRows)
		|| !TestNotNull(TEXT("enumeration returns property handle group"), FirstHandles))
	{
		return false;
	}
	TestEqual(
		TEXT("property rows are an array"),
		FirstRows->Kind,
		EAssetDocumentRuntimeValueKind::Array);
	TestTrue(TEXT("fixture has admitted authored properties"),
		!FirstRows->Elements.IsEmpty());
	TestEqual(TEXT("every row has a parallel property token"),
		FirstHandles->Handles.Num(), FirstRows->Elements.Num());
	FString PreviousName;
	bool bHasBoolean = false;
	bool bHasHardObject = false;
	bool bHasUnsupportedDelegate = false;
	for (int32 Index = 0; Index < FirstRows->Elements.Num(); ++Index)
	{
		const FAssetDocumentRuntimeValueDto& Row = FirstRows->Elements[Index];
		const FString Name(ReadStringField(Row, TEXT("name")));
		TestTrue(TEXT("property rows are in authored-name order"),
			PreviousName.IsEmpty() || PreviousName < Name);
		PreviousName = Name;
		bHasBoolean |= Name == TEXT("BooleanValue");
		bHasHardObject |= Name == TEXT("HardObjectValue");
		bHasUnsupportedDelegate |= Name == TEXT("DelegateValue");
		TestEqual(TEXT("parallel token kind is Property"),
			FirstHandles->Handles[Index].Kind,
			EAssetDocumentHandleKind::Property);
		TestEqual(TEXT("parallel token access is read-only"),
			FirstHandles->Handles[Index].Access,
			EAssetDocumentHandleAccess::ReadOnly);
	}
	TestTrue(TEXT("admitted scalar fixture property is enumerated"), bHasBoolean);
	TestTrue(TEXT("hard object reference property is enumerated"),
		bHasHardObject);
	TestFalse(TEXT("unsupported delegate property is excluded"),
		bHasUnsupportedDelegate);

	const FAssetDocumentPrimitiveOutcome Second = InvokeEnumeration(
		*Provider, *Descriptor, Context, Handles,
		MakeExactClassArguments(CanonicalClassPath));
	const FAssetDocumentRuntimeValueDto* SecondRows =
		FindField(Second.Value, TEXT("properties"));
	TestEqual(TEXT("second canonical enumeration succeeds"),
		Second.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	TestTrue(TEXT("same class produces deterministic rows"),
		SecondRows != nullptr && AreValuesEqual(*FirstRows, *SecondRows));

	FAssetDocumentRuntimeValueDto Missing;
	Missing.Kind = EAssetDocumentRuntimeValueKind::Object;
	TestRejectedShape(*this, *Provider, *Descriptor, Context, MoveTemp(Missing),
		TEXT("missing exact class path is rejected"));

	FAssetDocumentRuntimeValueDto Extra =
		MakeExactClassArguments(CanonicalClassPath);
	Extra.ObjectKeys.Add(TEXT("extra"));
	Extra.Elements.Add(MakePropertyTestStringValue(TEXT("forbidden")));
	TestRejectedShape(*this, *Provider, *Descriptor, Context, MoveTemp(Extra),
		TEXT("extra enumeration key is rejected"));

	FAssetDocumentRuntimeValueDto WrongKind =
		MakeExactClassArguments(CanonicalClassPath);
	WrongKind.Elements[0] = {};
	WrongKind.Elements[0].Kind = EAssetDocumentRuntimeValueKind::Integer;
	WrongKind.Elements[0].IntegerValue = 7;
	TestRejectedShape(*this, *Provider, *Descriptor, Context, MoveTemp(WrongKind),
		TEXT("non-string exact class path is rejected"));

	TestRejectedShape(
		*this, *Provider, *Descriptor, Context,
		MakeExactClassArguments(FString::Printf(TEXT(" %s"), *CanonicalClassPath)),
		TEXT("non-canonical whitespace class path is rejected"));
	TestRejectedShape(
		*this, *Provider, *Descriptor, Context,
		MakeExactClassArguments(TEXT("AssetDocumentEditorCodecTestObject")),
		TEXT("short class name is rejected"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderPropertyTokenForgeryRejectedTest,
	"UEAssetDoc.GenericProvider.Property.TokenForgeryRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderPropertyTokenForgeryRejectedTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorPropertyProviderTests;

	FScopedEditableFixtureProperties EditableFixture;
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Descriptor = FindDescriptor(
		*Provider, TEXT("editor.property.read_admitted.v1"));
	FProperty* Property = FindPropertyTestFixtureField(TEXT("BooleanValue"));
	if (!TestNotNull(TEXT("property read descriptor exists"), Descriptor)
		|| !TestNotNull(TEXT("admitted fixture property exists"), Property))
	{
		return false;
	}

	const FAssetDocumentEditorProviderTestContext Context;
	TStrongObjectPtr<UAssetDocumentEditorCodecTestObject> MatchingAsset(
		NewObject<UAssetDocumentEditorCodecTestObject>());
	UDataAsset* UnrelatedAsset = GetMutableDefault<UDataAsset>();

	{
		FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
		const TOptional<FAssetDocumentOpaqueHandle> Asset =
			IssuePropertyTestAsset(Handles, *MatchingAsset);
		const TOptional<FAssetDocumentOpaqueHandle> WrongPayload =
			IssuePropertyTestPayload(
				Handles,
				EAssetDocumentHandleKind::Property,
				EAssetDocumentHandleAccess::ReadOnly,
				MakeUnique<FPropertyTestOpaquePayload>(
					FName(TEXT("assetdocument.test.not_property.v1"))));
		if (!TestTrue(TEXT("wrong-type case handles issue"),
			Asset.IsSet() && WrongPayload.IsSet()))
		{
			return false;
		}
		TestPropertyTokenRejected(
			*this,
			InvokePropertyTestRead(
				*Provider, *Descriptor, Context, Handles,
				Asset.GetValue(), WrongPayload.GetValue()),
			TEXT("wrong native payload type is rejected"),
			TEXT("PropertyToken.InvalidPayload"));
	}

	{
		FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
		const TOptional<FAssetDocumentOpaqueHandle> Asset =
			IssuePropertyTestAsset(Handles, *UnrelatedAsset);
		const TOptional<FAssetDocumentOpaqueHandle> Token =
			IssuePropertyTestToken(Handles, *Property);
		if (!TestTrue(TEXT("unrelated-class case handles issue"),
			Asset.IsSet() && Token.IsSet()))
		{
			return false;
		}
		TestPropertyTokenRejected(
			*this,
			InvokePropertyTestRead(
				*Provider, *Descriptor, Context, Handles,
				Asset.GetValue(), Token.GetValue()),
			TEXT("token bound to an unrelated class is rejected"),
			TEXT("PropertyToken.ClassMismatch"));
	}

	{
		FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
		const TOptional<FAssetDocumentOpaqueHandle> Asset =
			IssuePropertyTestAsset(Handles, *MatchingAsset);
		TOptional<FAssetDocumentOpaqueHandle> Token =
			IssuePropertyTestToken(Handles, *Property);
		if (!TestTrue(TEXT("revision-mismatch case handles issue"),
			Asset.IsSet() && Token.IsSet()))
		{
			return false;
		}
		++Token.GetValue().RuntimeRevision;
		TestPropertyTokenRejected(
			*this,
			InvokePropertyTestRead(
				*Provider, *Descriptor, Context, Handles,
				Asset.GetValue(), Token.GetValue()),
			TEXT("outer runtime revision mismatch is rejected"),
			TEXT("HandleMetadata.RuntimeRevisionMismatch"));
	}

	{
		FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
		const TOptional<FAssetDocumentOpaqueHandle> Asset =
			IssuePropertyTestAsset(Handles, *MatchingAsset);
		const TSharedRef<FPropertyTestLiveness> Liveness =
			MakeShared<FPropertyTestLiveness>();
		const TOptional<FAssetDocumentOpaqueHandle> Token =
			IssuePropertyTestPayload(
				Handles,
				EAssetDocumentHandleKind::Property,
				EAssetDocumentHandleAccess::ReadOnly,
				MakeUnique<FPropertyTestTogglePayload>(Liveness));
		if (!TestTrue(TEXT("stale case handles issue"),
			Asset.IsSet() && Token.IsSet()))
		{
			return false;
		}
		Liveness->bAlive = false;
		TestPropertyTokenRejected(
			*this,
			InvokePropertyTestRead(
				*Provider, *Descriptor, Context, Handles,
				Asset.GetValue(), Token.GetValue()),
			TEXT("payload stale after issue is rejected"),
			TEXT("HandleMetadata.StaleHandle"));
	}

	{
		FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
		const TOptional<FAssetDocumentOpaqueHandle> Asset =
			IssuePropertyTestAsset(Handles, *MatchingAsset);
		const TOptional<FAssetDocumentOpaqueHandle> Token =
			IssuePropertyTestToken(Handles, *Property);
		if (!TestTrue(TEXT("name-injection case handles issue"),
			Asset.IsSet() && Token.IsSet()))
		{
			return false;
		}
		FAssetDocumentRuntimeValueDto Arguments;
		Arguments.Kind = EAssetDocumentRuntimeValueKind::Object;
		Arguments.ObjectKeys = {TEXT("name")};
		Arguments.Elements = {
			MakePropertyTestStringValue(TEXT("BooleanValue"))};
		TestPropertyTokenRejected(
			*this,
			InvokePropertyTestRead(
				*Provider, *Descriptor, Context, Handles,
				Asset.GetValue(), Token.GetValue(), MoveTemp(Arguments)),
			TEXT("value-argument name injection is rejected"),
			TEXT("PrimitiveInvocation.InvalidValueShape"));
	}

	{
		FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
		const TOptional<FAssetDocumentOpaqueHandle> Asset =
			IssuePropertyTestAsset(Handles, *MatchingAsset);
		const TOptional<FAssetDocumentOpaqueHandle> Token =
			IssuePropertyTestToken(Handles, *Property);
		if (!TestTrue(TEXT("valid control handles issue"),
			Asset.IsSet() && Token.IsSet()))
		{
			return false;
		}
		const FAssetDocumentPrimitiveOutcome Outcome = InvokePropertyTestRead(
			*Provider, *Descriptor, Context, Handles,
			Asset.GetValue(), Token.GetValue());
		TestEqual(TEXT("valid token reaches the implemented read behavior"),
			Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderPropertyReadAdmittedTest,
	"UEAssetDoc.GenericProvider.Property.ReadAdmitted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderPropertyReadAdmittedTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorPropertyProviderTests;

	FScopedEditableFixtureProperties EditableFixture;
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Descriptor = FindDescriptor(
		*Provider, TEXT("editor.property.read_admitted.v1"));
	FProperty* Property = FindPropertyTestFixtureField(TEXT("BooleanValue"));
	if (!TestNotNull(TEXT("property read descriptor exists"), Descriptor)
		|| !TestNotNull(TEXT("admitted boolean property exists"), Property))
	{
		return false;
	}

	const FAssetDocumentEditorProviderTestContext Context;
	TStrongObjectPtr<UAssetDocumentEditorCodecTestObject> Asset(
		NewObject<UAssetDocumentEditorCodecTestObject>());
	Asset->BooleanValue = true;

	{
		FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
		const TOptional<FAssetDocumentOpaqueHandle> AssetHandle =
			IssuePropertyTestAsset(Handles, *Asset);
		const TOptional<FAssetDocumentOpaqueHandle> Token =
			IssuePropertyTestToken(Handles, *Property, false, true);
		if (!TestTrue(TEXT("non-readable case handles issue"),
			AssetHandle.IsSet() && Token.IsSet()))
		{
			return false;
		}
		TestPropertyTokenRejected(
			*this,
			InvokePropertyTestRead(
				*Provider, *Descriptor, Context, Handles,
				AssetHandle.GetValue(), Token.GetValue()),
			TEXT("mutable-only property token is rejected for read"),
			TEXT("PropertyToken.InvalidPayload"));
	}

	const bool bSourceValueBefore = Asset->BooleanValue;
	const bool bPackageDirtyBefore = Asset->GetOutermost()->IsDirty();
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	const TOptional<FAssetDocumentOpaqueHandle> AssetHandle =
		IssuePropertyTestAsset(Handles, *Asset);
	const TOptional<FAssetDocumentOpaqueHandle> Token =
		IssuePropertyTestToken(Handles, *Property);
	if (!TestTrue(TEXT("read case handles issue"),
		AssetHandle.IsSet() && Token.IsSet()))
	{
		return false;
	}
	const FAssetDocumentPrimitiveOutcome Outcome = InvokePropertyTestRead(
		*Provider, *Descriptor, Context, Handles,
		AssetHandle.GetValue(), Token.GetValue());
	if (Outcome.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
	{
		AddError(FString::Printf(
			TEXT("admitted scalar read failed: %s"),
			*DescribePropertyOutcome(Outcome)));
		return false;
	}

	TestTrue(TEXT("read output is canonical T01"),
		ValidateAssetDocumentRuntimeValue(Outcome.Value).bValid);
	TestTrue(TEXT("read output has exact value key"),
		Outcome.Value.Kind == EAssetDocumentRuntimeValueKind::Object
			&& Outcome.Value.ObjectKeys.Num() == 1
			&& Outcome.Value.Elements.Num() == 1
			&& Outcome.Value.ObjectKeys[0] == TEXT("value"));
	const FAssetDocumentRuntimeValueDto* Value =
		FindField(Outcome.Value, TEXT("value"));
	if (!TestNotNull(TEXT("read output contains value"), Value))
	{
		return false;
	}
	TestEqual(TEXT("read output preserves scalar type"),
		Value->Kind, EAssetDocumentRuntimeValueKind::Boolean);
	TestTrue(TEXT("read output preserves scalar value"), Value->BooleanValue);
	TestTrue(TEXT("read returns no handles"), Outcome.HandleResults.IsEmpty());
	TestTrue(TEXT("read returns no changed records"), Outcome.ChangedSet.IsEmpty());
	TestTrue(TEXT("read returns no diagnostics"), Outcome.Diagnostics.IsEmpty());
	TestEqual(TEXT("read does not mutate source value"),
		Asset->BooleanValue, bSourceValueBefore);
	TestEqual(TEXT("read does not dirty source package"),
		Asset->GetOutermost()->IsDirty(), bPackageDirtyBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderPropertyWriteAdmittedTest,
	"UEAssetDoc.GenericProvider.Property.WriteAdmitted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderPropertyWriteAdmittedTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorPropertyProviderTests;

	FScopedEditableFixtureProperties EditableFixture;
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Descriptor = FindDescriptor(
		*Provider, TEXT("editor.property.write_admitted.v1"));
	FProperty* Property = FindPropertyTestFixtureField(TEXT("BooleanValue"));
	if (!TestNotNull(TEXT("property write descriptor exists"), Descriptor)
		|| !TestNotNull(TEXT("writable boolean property exists"), Property))
	{
		return false;
	}

	const FAssetDocumentEditorProviderTestContext Context;
	TStrongObjectPtr<UAssetDocumentEditorCodecTestObject> Asset(
		NewObject<UAssetDocumentEditorCodecTestObject>(
			GetTransientPackage(), NAME_None, RF_Transactional));
	Asset->BooleanValue = false;
	const bool bSourceValueBefore = Asset->BooleanValue;
	UPackage* const Package = Asset->GetOutermost();
	const bool bPackageDirtyBefore = Package->IsDirty();
	FDelegateHandle PreChangeHandle;
	FDelegateHandle PostChangeHandle;
	FDelegateHandle ModifiedHandle;
	ON_SCOPE_EXIT
	{
		if (PreChangeHandle.IsValid())
		{
			FCoreUObjectDelegates::OnPreObjectPropertyChanged.Remove(
				PreChangeHandle);
		}
		if (PostChangeHandle.IsValid())
		{
			FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(
				PostChangeHandle);
		}
		if (ModifiedHandle.IsValid())
		{
			FCoreUObjectDelegates::OnObjectModified.Remove(ModifiedHandle);
		}
		Asset->BooleanValue = bSourceValueBefore;
		Package->SetDirtyFlag(bPackageDirtyBefore);
	};

	int32 PreChangeCount = 0;
	int32 PostChangeCount = 0;
	int32 ModifiedCount = 0;
	TArray<FString> LifecycleOrder;
	PreChangeHandle =
		FCoreUObjectDelegates::OnPreObjectPropertyChanged.AddLambda(
			[Object = Asset.Get(), &PreChangeCount, &LifecycleOrder](
				UObject* ChangedObject,
				const FEditPropertyChain&)
			{
				if (ChangedObject == Object)
				{
					++PreChangeCount;
					LifecycleOrder.Add(TEXT("Pre"));
				}
			});
	PostChangeHandle =
		FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda(
			[Object = Asset.Get(), Property, &PostChangeCount, &LifecycleOrder](
				UObject* ChangedObject,
				FPropertyChangedEvent& Event)
			{
				if (ChangedObject == Object
					&& Event.GetPropertyName() == Property->GetFName())
				{
					++PostChangeCount;
					LifecycleOrder.Add(TEXT("Post"));
				}
			});
	ModifiedHandle = FCoreUObjectDelegates::OnObjectModified.AddLambda(
		[Object = Asset.Get(), &ModifiedCount, &LifecycleOrder](
			UObject* ChangedObject)
		{
			if (ChangedObject == Object)
			{
				++ModifiedCount;
				LifecycleOrder.Add(TEXT("Modify"));
			}
		});

	{
		FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
		const TOptional<FAssetDocumentOpaqueHandle> AssetHandle =
			IssuePropertyTestAsset(
				Handles, *Asset, EAssetDocumentHandleAccess::Mutable);
		const TOptional<FAssetDocumentOpaqueHandle> Token =
			IssuePropertyTestToken(Handles, *Property);
		if (!TestTrue(TEXT("no-transaction case handles issue"),
			AssetHandle.IsSet() && Token.IsSet()))
		{
			return false;
		}
		const FAssetDocumentPrimitiveOutcome Outcome = InvokePropertyTestWrite(
			*Provider, *Descriptor, Context, Handles,
			AssetHandle.GetValue(), Token.GetValue(), true, nullptr);
		TestEqual(TEXT("write without active transaction is rejected"),
			Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
		TestEqual(TEXT("missing transaction cannot mutate source"),
			Asset->BooleanValue, bSourceValueBefore);
		TestEqual(TEXT("missing transaction cannot dirty package"),
			Package->IsDirty(), bPackageDirtyBefore);
		TestTrue(TEXT("missing transaction emits no edit lifecycle"),
			LifecycleOrder.IsEmpty());
	}

	{
		FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
		FAssetDocumentEditorProviderTestTransaction Transaction(Context);
		const TOptional<FAssetDocumentOpaqueHandle> AssetHandle =
			IssuePropertyTestAsset(Handles, *Asset);
		const TOptional<FAssetDocumentOpaqueHandle> Token =
			IssuePropertyTestToken(Handles, *Property);
		if (!TestTrue(TEXT("invalid-access case handles issue"),
			AssetHandle.IsSet() && Token.IsSet()))
		{
			return false;
		}
		const FAssetDocumentPrimitiveOutcome Outcome = InvokePropertyTestWrite(
			*Provider, *Descriptor, Context, Handles,
			AssetHandle.GetValue(), Token.GetValue(), true, &Transaction);
		TestEqual(TEXT("read-only asset cannot authorize property write"),
			Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
		TestEqual(TEXT("invalid access cannot mutate source"),
			Asset->BooleanValue, bSourceValueBefore);
		TestEqual(TEXT("invalid access cannot dirty package"),
			Package->IsDirty(), bPackageDirtyBefore);
		TestTrue(TEXT("invalid access enlists no footprint"),
			Transaction.GetEnlistedFootprint().IsEmpty());
		TestTrue(TEXT("invalid access emits no edit lifecycle"),
			LifecycleOrder.IsEmpty());
	}

	{
		FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
		FPropertyTestRejectingTransaction Transaction(Context);
		const TOptional<FAssetDocumentOpaqueHandle> AssetHandle =
			IssuePropertyTestAsset(
				Handles, *Asset, EAssetDocumentHandleAccess::Mutable);
		const TOptional<FAssetDocumentOpaqueHandle> Token =
			IssuePropertyTestToken(Handles, *Property);
		if (!TestTrue(TEXT("enlist-failure case handles issue"),
			AssetHandle.IsSet() && Token.IsSet()))
		{
			return false;
		}
		const FAssetDocumentPrimitiveOutcome Outcome = InvokePropertyTestWrite(
			*Provider, *Descriptor, Context, Handles,
			AssetHandle.GetValue(), Token.GetValue(), true, &Transaction);
		TestTrue(TEXT("rejected footprint cannot report write success"),
			Outcome.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
		TestEqual(TEXT("rejected footprint cannot mutate source"),
			Asset->BooleanValue, bSourceValueBefore);
		TestEqual(TEXT("rejected footprint cannot dirty package"),
			Package->IsDirty(), bPackageDirtyBefore);
		TestTrue(TEXT("rejected footprint emits no edit lifecycle"),
			LifecycleOrder.IsEmpty());
	}

	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	FAssetDocumentEditorProviderTestTransaction Transaction(Context);
	const TOptional<FAssetDocumentOpaqueHandle> AssetHandle =
		IssuePropertyTestAsset(
			Handles, *Asset, EAssetDocumentHandleAccess::Mutable);
	const TOptional<FAssetDocumentOpaqueHandle> Token =
		IssuePropertyTestToken(Handles, *Property);
	if (!TestTrue(TEXT("write case handles issue"),
		AssetHandle.IsSet() && Token.IsSet()))
	{
		return false;
	}
	const FAssetDocumentPrimitiveOutcome Outcome = InvokePropertyTestWrite(
		*Provider, *Descriptor, Context, Handles,
		AssetHandle.GetValue(), Token.GetValue(), true, &Transaction);
	if (Outcome.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
	{
		AddError(FString::Printf(
			TEXT("admitted scalar write failed: %s"),
			*DescribePropertyOutcome(Outcome)));
		return false;
	}

	TestTrue(TEXT("write output is canonical T01"),
		ValidateAssetDocumentRuntimeValue(Outcome.Value).bValid);
	TestTrue(TEXT("write output has exact value and changed keys"),
		Outcome.Value.Kind == EAssetDocumentRuntimeValueKind::Object
			&& Outcome.Value.ObjectKeys
				== TArray<FString>({TEXT("changed"), TEXT("value")})
			&& Outcome.Value.Elements.Num() == 2);
	const FAssetDocumentRuntimeValueDto* Readback =
		FindField(Outcome.Value, TEXT("value"));
	const FAssetDocumentRuntimeValueDto* Changed =
		FindField(Outcome.Value, TEXT("changed"));
	if (!TestNotNull(TEXT("write output contains readback"), Readback)
		|| !TestNotNull(TEXT("write output contains changed"), Changed))
	{
		return false;
	}
	TestTrue(TEXT("typed readback is exact true"),
		Readback->Kind == EAssetDocumentRuntimeValueKind::Boolean
			&& Readback->BooleanValue);
	TestTrue(TEXT("write reports changed=true"),
		Changed->Kind == EAssetDocumentRuntimeValueKind::Boolean
			&& Changed->BooleanValue);
	TestTrue(TEXT("write mutates the admitted scalar"), Asset->BooleanValue);
	TestEqual(TEXT("write emits one PreEditChange"), PreChangeCount, 1);
	TestEqual(TEXT("write emits one PostEditChangeProperty"), PostChangeCount, 1);
	TestEqual(TEXT("write calls transaction Modify authority"), ModifiedCount, 1);
	TestTrue(TEXT("write lifecycle is Modify then Pre then Post"),
		LifecycleOrder == TArray<FString>({
			TEXT("Modify"), TEXT("Pre"), TEXT("Post")}));
	TestTrue(TEXT("write returns no handles"), Outcome.HandleResults.IsEmpty());
	TestTrue(TEXT("successful write returns no diagnostics"),
		Outcome.Diagnostics.IsEmpty());

	if (!TestEqual(TEXT("write returns one changed record"),
		Outcome.ChangedSet.Num(), 1))
	{
		return false;
	}
	const FAssetDocumentChangedRecord& Record = Outcome.ChangedSet[0];
	TestEqual(TEXT("changed authority is the exact primitive"),
		Record.AuthorityId, Descriptor->PrimitiveId);
	TestEqual(TEXT("changed stable object is canonical"),
		Record.StableObjectId, Asset->GetPathName());
	TestEqual(TEXT("changed path is exact authored property"),
		Record.Path, FString(TEXT("/BooleanValue")));
	TestEqual(TEXT("changed kind is exact"),
		Record.ChangeKind, FString(TEXT("modified")));

	const TArray<FAssetDocumentMutationFootprint>& Footprint =
		Transaction.GetEnlistedFootprint();
	if (!TestEqual(TEXT("write enlists exact object/package footprint"),
		Footprint.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("first footprint is UObject state"),
		Footprint[0].Kind, EAssetDocumentMutationFootprintKind::UObjectState);
	TestEqual(TEXT("first footprint has canonical object identity"),
		Footprint[0].StableId,
		FString::Printf(TEXT("Object:%s"), *Asset->GetPathName()));
	TestEqual(TEXT("first footprint uses UObject rollback authority"),
		Footprint[0].RollbackComponentId, FString(TEXT("rollback.uobject")));
	TestEqual(TEXT("second footprint is package state"),
		Footprint[1].Kind, EAssetDocumentMutationFootprintKind::Package);
	TestEqual(TEXT("second footprint has canonical package identity"),
		Footprint[1].StableId,
		FString::Printf(TEXT("Package:%s"), *Package->GetName()));
	TestEqual(TEXT("second footprint uses package rollback authority"),
		Footprint[1].RollbackComponentId, FString(TEXT("rollback.package")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderPropertyPreflightUsesScratchTest,
	"UEAssetDoc.GenericProvider.Property.PreflightUsesScratch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderPropertyPreflightUsesScratchTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorPropertyProviderTests;

	FScopedEditableFixtureProperties EditableFixture;
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Descriptor = FindDescriptor(
		*Provider, TEXT("editor.property.preflight_admitted.v1"));
	FProperty* Property = FindPropertyTestFixtureField(TEXT("BooleanValue"));
	if (!TestNotNull(TEXT("property preflight descriptor exists"), Descriptor)
		|| !TestNotNull(TEXT("preflight boolean property exists"), Property))
	{
		return false;
	}
	TestEqual(TEXT("preflight descriptor has scratch access"),
		Descriptor->Access, EAssetDocumentPrimitiveAccess::Scratch);
	TestFalse(TEXT("preflight descriptor requires no durable transaction"),
		Descriptor->bRequiresActiveTransaction);

	const FAssetDocumentEditorProviderTestContext Context;
	TStrongObjectPtr<UAssetDocumentEditorCodecTestObject> Source(
		NewObject<UAssetDocumentEditorCodecTestObject>(
			GetTransientPackage(), NAME_None, RF_Transient));
	TStrongObjectPtr<UAssetDocumentEditorCodecTestObject> Scratch(
		NewObject<UAssetDocumentEditorCodecTestObject>(
			GetTransientPackage(), NAME_None, RF_Transient));
	Source->BooleanValue = false;
	Scratch->BooleanValue = false;
	const bool bSourceValueBefore = Source->BooleanValue;
	UPackage* const SourcePackage = Source->GetOutermost();
	const bool bSourcePackageDirtyBefore = SourcePackage->IsDirty();
	FPropertyTestRegistryObserver RegistryObserver(
		Source->GetPathName(), Scratch->GetPathName());

	int32 SourceLifecycleCount = 0;
	int32 ScratchLifecycleCount = 0;
	const auto CountObservedObject =
		[SourceObject = Source.Get(),
		 ScratchObject = Scratch.Get(),
		 &SourceLifecycleCount,
		 &ScratchLifecycleCount](UObject* ChangedObject)
		{
			SourceLifecycleCount += ChangedObject == SourceObject ? 1 : 0;
			ScratchLifecycleCount += ChangedObject == ScratchObject ? 1 : 0;
		};
	const FDelegateHandle PreChangeHandle =
		FCoreUObjectDelegates::OnPreObjectPropertyChanged.AddLambda(
			[CountObservedObject](
				UObject* ChangedObject,
				const FEditPropertyChain&)
			{
				CountObservedObject(ChangedObject);
			});
	const FDelegateHandle PostChangeHandle =
		FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda(
			[CountObservedObject](
				UObject* ChangedObject,
				FPropertyChangedEvent&)
			{
				CountObservedObject(ChangedObject);
			});
	const FDelegateHandle ModifiedHandle =
		FCoreUObjectDelegates::OnObjectModified.AddLambda(
			[CountObservedObject](UObject* ChangedObject)
			{
				CountObservedObject(ChangedObject);
			});
	ON_SCOPE_EXIT
	{
		FCoreUObjectDelegates::OnPreObjectPropertyChanged.Remove(
			PreChangeHandle);
		FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(
			PostChangeHandle);
		FCoreUObjectDelegates::OnObjectModified.Remove(ModifiedHandle);
		Source->BooleanValue = bSourceValueBefore;
		SourcePackage->SetDirtyFlag(bSourcePackageDirtyBefore);
	};

	{
		FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
		const TOptional<FAssetDocumentOpaqueHandle> WrongKind =
			IssuePropertyTestScratch(
				Handles,
				*Scratch,
				EAssetDocumentHandleKind::Asset,
				EAssetDocumentHandleAccess::Mutable);
		const TOptional<FAssetDocumentOpaqueHandle> Token =
			IssuePropertyTestToken(Handles, *Property);
		if (!TestTrue(TEXT("wrong-kind preflight handles issue"),
			WrongKind.IsSet() && Token.IsSet()))
		{
			return false;
		}
		const FAssetDocumentPrimitiveOutcome Outcome =
			InvokePropertyTestPreflight(
				*Provider, *Descriptor, Context, Handles,
				WrongKind.GetValue(), Token.GetValue(), true);
		TestEqual(TEXT("Asset/Mutable cannot substitute for scratch"),
			Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
		TestFalse(TEXT("wrong scratch kind cannot mutate scratch"),
			Scratch->BooleanValue);
	}

	{
		FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
		const TOptional<FAssetDocumentOpaqueHandle> ReadOnlyScratch =
			IssuePropertyTestScratch(
				Handles,
				*Scratch,
				EAssetDocumentHandleKind::Scratch,
				EAssetDocumentHandleAccess::ReadOnly);
		const TOptional<FAssetDocumentOpaqueHandle> Token =
			IssuePropertyTestToken(Handles, *Property);
		if (!TestTrue(TEXT("read-only preflight handles issue"),
			ReadOnlyScratch.IsSet() && Token.IsSet()))
		{
			return false;
		}
		const FAssetDocumentPrimitiveOutcome Outcome =
			InvokePropertyTestPreflight(
				*Provider, *Descriptor, Context, Handles,
				ReadOnlyScratch.GetValue(), Token.GetValue(), true);
		TestEqual(TEXT("Scratch/ReadOnly cannot authorize preflight"),
			Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
		TestFalse(TEXT("read-only scratch cannot be mutated"),
			Scratch->BooleanValue);
	}

	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	const TOptional<FAssetDocumentOpaqueHandle> ScratchHandle =
		IssuePropertyTestScratch(Handles, *Scratch);
	const TOptional<FAssetDocumentOpaqueHandle> Token =
		IssuePropertyTestToken(Handles, *Property);
	if (!TestTrue(TEXT("valid preflight handles issue"),
		ScratchHandle.IsSet() && Token.IsSet()))
	{
		return false;
	}
	const FAssetDocumentPrimitiveOutcome Outcome =
		InvokePropertyTestPreflight(
			*Provider, *Descriptor, Context, Handles,
			ScratchHandle.GetValue(), Token.GetValue(), true);
	if (Outcome.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
	{
		AddError(FString::Printf(
			TEXT("scratch property preflight failed: %s"),
			*DescribePropertyOutcome(Outcome)));
		return false;
	}

	TestTrue(TEXT("preflight output is canonical T01"),
		ValidateAssetDocumentRuntimeValue(Outcome.Value).bValid);
	TestTrue(TEXT("preflight output has exact canonical keys"),
		Outcome.Value.Kind == EAssetDocumentRuntimeValueKind::Object
			&& Outcome.Value.ObjectKeys == TArray<FString>({
				TEXT("canonical_value"), TEXT("feasible")})
			&& Outcome.Value.Elements.Num() == 2);
	const FAssetDocumentRuntimeValueDto* CanonicalValue =
		FindField(Outcome.Value, TEXT("canonical_value"));
	const FAssetDocumentRuntimeValueDto* Feasible =
		FindField(Outcome.Value, TEXT("feasible"));
	if (!TestNotNull(
		TEXT("preflight output contains canonical value"), CanonicalValue)
		|| !TestNotNull(TEXT("preflight output contains feasibility"), Feasible))
	{
		return false;
	}
	TestTrue(TEXT("preflight returns the exact typed canonical value"),
		CanonicalValue->Kind == EAssetDocumentRuntimeValueKind::Boolean
			&& CanonicalValue->BooleanValue);
	TestTrue(TEXT("preflight reports exact feasible=true"),
		Feasible->Kind == EAssetDocumentRuntimeValueKind::Boolean
			&& Feasible->BooleanValue);
	TestTrue(TEXT("preflight applies only to scratch"),
		Scratch->BooleanValue);
	TestEqual(TEXT("preflight leaves source value untouched"),
		Source->BooleanValue, bSourceValueBefore);
	TestEqual(TEXT("preflight leaves source package dirty state untouched"),
		SourcePackage->IsDirty(), bSourcePackageDirtyBefore);
	TestEqual(TEXT("preflight emits no source edit lifecycle"),
		SourceLifecycleCount, 0);
	// The scratch is notified like the live write (and the Details panel), so the
	// readback shows what the class's PostEditChange would store.
	TestTrue(TEXT("preflight notifies the scratch edit lifecycle"),
		ScratchLifecycleCount > 0);
	TestEqual(TEXT("preflight performs no asset registry mutation"),
		RegistryObserver.GetMutationCount(), 0);
	TestTrue(TEXT("preflight returns no handles"),
		Outcome.HandleResults.IsEmpty());
	TestTrue(TEXT("preflight returns no durable changed records"),
		Outcome.ChangedSet.IsEmpty());
	TestTrue(TEXT("preflight returns no diagnostics"),
		Outcome.Diagnostics.IsEmpty());

	const FAssetDocumentVerifiedLeaseResult PreservedScratch =
		Handles.AcquireVerifiedLease(
			ScratchHandle.GetValue(),
			MakePropertyTestScratchContext(Context));
	TestTrue(TEXT("provider does not own terminal scratch cleanup"),
		PreservedScratch.Lease.IsValid()
			&& PreservedScratch.Diagnostics.IsEmpty());

	FAssetDocumentEditorProviderTestHandleResolver NonWritableHandles(Context);
	const TOptional<FAssetDocumentOpaqueHandle> NonWritableScratch =
		IssuePropertyTestScratch(NonWritableHandles, *Scratch);
	const TOptional<FAssetDocumentOpaqueHandle> NonWritableToken =
		IssuePropertyTestToken(
			NonWritableHandles, *Property, true, false);
	if (!TestTrue(TEXT("non-writable preflight handles issue"),
		NonWritableScratch.IsSet() && NonWritableToken.IsSet()))
	{
		return false;
	}
	const FAssetDocumentPrimitiveOutcome NonWritableOutcome =
		InvokePropertyTestPreflight(
			*Provider, *Descriptor, Context, NonWritableHandles,
			NonWritableScratch.GetValue(),
			NonWritableToken.GetValue(),
			false);
	TestEqual(TEXT("non-writable property token is rejected for preflight"),
		NonWritableOutcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Failed);
	TestTrue(TEXT("non-writable token cannot mutate scratch"),
		Scratch->BooleanValue);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderPropertyUnsupportedCodecFailsClosedTest,
	"UEAssetDoc.GenericProvider.Property.UnsupportedCodecFailsClosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool
FAssetDocumentGenericProviderPropertyUnsupportedCodecFailsClosedTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorPropertyProviderTests;

	FScopedEditableFixtureProperties EditableFixture;
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Descriptor = FindDescriptor(
		*Provider, TEXT("editor.property.enumerate_admitted.v1"));
	if (!TestNotNull(
		TEXT("property enumeration descriptor exists"), Descriptor))
	{
		return false;
	}

	// Object, interface, weak and lazy references are admitted as object paths,
	// integer-keyed maps by decimal key text; delegates still have no document form.
	const TArray<FString> UnsupportedNames = {
		TEXT("DelegateValue"),
	};
	TArray<EPropertyFlags> PropertyFlagsBefore;
	for (const FString& Name : UnsupportedNames)
	{
		FProperty* Property = FindPropertyTestFixtureField(Name);
		if (!TestNotNull(
			FString::Printf(TEXT("%s fixture property exists"), *Name),
			Property))
		{
			return false;
		}
		PropertyFlagsBefore.Add(Property->GetPropertyFlags());
	}

	UClass* const FixtureClass =
		UAssetDocumentEditorCodecTestObject::StaticClass();
	UPackage* const FixturePackage = FixtureClass->GetOutermost();
	const bool bPackageDirtyBefore = FixturePackage->IsDirty();
	const FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	FAssetDocumentEditorProviderTestTransaction Transaction(Context);
	const FAssetDocumentPrimitiveOutcome Outcome = InvokeEnumeration(
		*Provider,
		*Descriptor,
		Context,
		Handles,
		MakeExactClassArguments(FixtureClass->GetPathName()),
		&Transaction);
	if (Outcome.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
	{
		AddError(FString::Printf(
			TEXT("unsupported-family admission scan failed: %s"),
			*DescribePropertyOutcome(Outcome)));
		return false;
	}

	const FAssetDocumentRuntimeValueDto* Rows =
		FindField(Outcome.Value, TEXT("properties"));
	const FAssetDocumentPrimitiveHandleResult* Tokens =
		FindHandleGroup(Outcome, TEXT("properties"));
	if (!TestNotNull(TEXT("admission returns property rows"), Rows)
		|| !TestNotNull(TEXT("admission returns parallel tokens"), Tokens))
	{
		return false;
	}
	TestEqual(TEXT("admission rows use the exact array shape"),
		Rows->Kind, EAssetDocumentRuntimeValueKind::Array);
	TestEqual(TEXT("every admitted row has exactly one parallel token"),
		Tokens->Handles.Num(), Rows->Elements.Num());

	TSet<FString> AdmittedNames;
	for (const FAssetDocumentRuntimeValueDto& Row : Rows->Elements)
	{
		AdmittedNames.Add(FString(ReadStringField(Row, TEXT("name"))));
	}
	for (int32 Index = 0; Index < UnsupportedNames.Num(); ++Index)
	{
		const FString& Name = UnsupportedNames[Index];
		TestFalse(
			FString::Printf(
				TEXT("%s is excluded without a token"), *Name),
			AdmittedNames.Contains(Name));
		FProperty* Property = FindPropertyTestFixtureField(Name);
		TestEqual(
			FString::Printf(
				TEXT("%s metadata remains unchanged"), *Name),
			Property->GetPropertyFlags(),
			PropertyFlagsBefore[Index]);
	}
	TestTrue(TEXT("unsupported admission enlists no footprint"),
		Transaction.GetEnlistedFootprint().IsEmpty());
	TestTrue(TEXT("unsupported admission reports no durable changes"),
		Outcome.ChangedSet.IsEmpty());
	TestEqual(TEXT("unsupported admission does not dirty the class package"),
		FixturePackage->IsDirty(), bPackageDirtyBefore);
	TestTrue(TEXT("mixed-class admission returns no diagnostics"),
		Outcome.Diagnostics.IsEmpty());
	return true;
}
#endif
