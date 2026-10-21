// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentEditorPrimitiveProvider.h"

#include "AssetDocumentEditorProviderTestSupport.h"
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Texture2D.h"
#include "Factories/Texture2dFactoryNew.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "Interfaces/Interface_AssetUserData.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "PackageTools.h"
#include "Provider/AssetDocumentEditorNativeIdentity.h"
#include "Subtitles/SubtitlesAndClosedCaptionsDelegates.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectGlobals.h"
#include "Utils/AssetDocumentEngineCompat.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorEmbeddedProviderTests
{
using namespace UE::AssetDocument::EditorPrimitive;

uint64 GetTypeRevision(const UClass& Class)
{
	return static_cast<uint64>(Class.GetUniqueID());
}

class FTestObjectIdentityPayload final : public INativeIdentityPayload
{
public:
FTestObjectIdentityPayload(
		UObject& InObject,
		FName InPayloadTypeId)
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
			&& static_cast<uint64>(Resolved->GetUniqueID())
				== TypeRevision;
	}

	virtual bool TryGetObjectIdentity(
		FObjectIdentityView& OutView) const override
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

class FTestClassIdentityPayload final : public INativeIdentityPayload
{
public:
	explicit FTestClassIdentityPayload(UClass& InClass)
		: Class(&InClass)
		, CanonicalPath(InClass.GetPathName())
		, TypeRevision(GetTypeRevision(InClass))
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
			&& Resolved->GetPathName() == CanonicalPath
			&& GetTypeRevision(*Resolved) == TypeRevision;
	}

	virtual bool TryGetClassIdentity(
		FClassIdentityView& OutView) const override
	{
		if (!IsAlive())
		{
			return false;
		}
		OutView.Class = Class.Get();
		OutView.CanonicalClassPath = CanonicalPath;
		OutView.TypeRevision = TypeRevision;
		return true;
	}

private:
	TWeakObjectPtr<UClass> Class;
	FString CanonicalPath;
	uint64 TypeRevision = 0;
};

const FAssetDocumentPrimitiveDescriptor* FindEmbeddedDescriptor()
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor :
		CreateAssetDocumentEditorPrimitiveProvider()->DescribePrimitives())
	{
		if (Descriptor.PrimitiveId
			== TEXT("editor.fragment.embedded_object_create.v1"))
		{
			return &Descriptor;
		}
	}
	return nullptr;
}

FAssetDocumentRuntimeValueDto MakeStringValue(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
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

const FAssetDocumentRuntimeValueDto* FindObjectField(
	const FAssetDocumentRuntimeValueDto& Object,
	FStringView Name)
{
	const int32 Index = Object.ObjectKeys.Find(FString(Name));
	return Object.Elements.IsValidIndex(Index)
		? &Object.Elements[Index]
		: nullptr;
}

TOptional<FAssetDocumentOpaqueHandle> IssueInputHandle(
	FAutomationTestBase& Test,
	IAssetDocumentHandleResolver& Resolver,
	const TCHAR* Label,
	EAssetDocumentHandleKind Kind,
	EAssetDocumentHandleAccess Access,
	TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload)
{
	FAssetDocumentIssuedHandleResult Issued =
		Resolver.Issue(Kind, Access, MoveTemp(Payload));
	if (!Test.TestTrue(Label, Issued.Handle.IsSet()))
	{
		return {};
	}
	return Issued.Handle.GetValue();
}

class FRejectSubobjectIssueResolver final
	: public IAssetDocumentHandleResolver
{
public:
	explicit FRejectSubobjectIssueResolver(
		const FAssetDocumentEditorProviderTestContext& Context)
		: Inner(Context)
	{
	}

	virtual FAssetDocumentIssuedHandleResult Issue(
		EAssetDocumentHandleKind Kind,
		EAssetDocumentHandleAccess Access,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override
	{
		if (Kind != EAssetDocumentHandleKind::Subobject)
		{
			return Inner.Issue(
				Kind, Access, MoveTemp(Payload));
		}
		++SubobjectIssueAttempts;
		bObservedExactIssue =
			Access == EAssetDocumentHandleAccess::Mutable
			&& Payload
			&& Payload->GetPayloadTypeId()
				== SubobjectPayloadTypeId;
		bObservedLiveAttachment =
			bObservedExactIssue && Payload->IsAlive();
		if (Payload)
		{
			CaptureObservedSubobject(*Payload);
		}
		FAssetDocumentIssuedHandleResult Result;
		FAssetDocumentRuntimeDiagnostic& Diagnostic =
			Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/Handle");
		Diagnostic.Code =
			TEXT("TestHandle.InjectedPostCreateFailure");
		Diagnostic.Message =
			TEXT("The test resolver rejects the created subobject handle.");
		return Result;
	}

	virtual FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(
		const FAssetDocumentOpaqueHandle& Handle,
		const FAssetDocumentHandleValidationContext&
			ValidationContext) override
	{
		return Inner.AcquireVerifiedLease(
			Handle, ValidationContext);
	}

	virtual FAssetDocumentHandleInvalidationResult Invalidate(
		const FAssetDocumentHandleInvalidation& Event) override
	{
		return Inner.Invalidate(Event);
	}

	int32 GetSubobjectIssueAttempts() const
	{
		return SubobjectIssueAttempts;
	}

	bool ObservedExactLiveAttachment() const
	{
		return bObservedExactIssue
			&& bObservedLiveAttachment
			&& ObservedSubobject.IsValid();
	}

private:
	void CaptureObservedSubobject(
		IAssetDocumentNativeHandlePayload& Payload)
	{
		INativeIdentityPayload* Identity =
			TryGetNativeIdentityPayload(
				Payload, SubobjectPayloadTypeId);
		FObjectIdentityView View;
		if (Identity != nullptr
			&& Identity->TryGetObjectIdentity(View))
		{
			ObservedSubobject = View.Object;
		}
	}

	FAssetDocumentEditorProviderTestHandleResolver Inner;
	TWeakObjectPtr<UObject> ObservedSubobject;
	int32 SubobjectIssueAttempts = 0;
	bool bObservedExactIssue = false;
	bool bObservedLiveAttachment = false;
};

FAssetDocumentHandleValidationContext MakeSubobjectValidationContext(
	const FAssetDocumentPrimitiveInvocation& Invocation)
{
	FAssetDocumentHandleValidationContext Context;
	Context.RequestId = Invocation.RequestId;
	Context.TransactionId = Invocation.TransactionId;
	Context.RuntimeRevision = Invocation.RuntimeRevision;
	Context.ProviderId = TEXT("editor");
	Context.Phase = Invocation.Phase;
	Context.RequiredKind = EAssetDocumentHandleKind::Subobject;
	Context.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
	return Context;
}

bool HasStringField(
	const FAssetDocumentRuntimeValueDto& Object,
	FStringView Name,
	const FString& Expected)
{
	const FAssetDocumentRuntimeValueDto* Field =
		FindObjectField(Object, Name);
	return Field != nullptr
		&& Field->Kind == EAssetDocumentRuntimeValueKind::String
		&& Field->StringValue == Expected;
}

FAssetDocumentPrimitiveInvocation MakeEmbeddedInvocation(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentEditorProviderTestContext& Context,
	const FAssetDocumentOpaqueHandle& OuterHandle,
	const FAssetDocumentOpaqueHandle& ClassHandle,
	FString CategoryId)
{
	FAssetDocumentPrimitiveInvocation Invocation =
		MakeAssetDocumentEditorProviderTestInvocation(
			Descriptor, Context);
	Invocation.ValueArguments = MakeObjectValue(
		{TEXT("category_id"), TEXT("properties")},
		{MakeStringValue(MoveTemp(CategoryId)),
		 MakeObjectValue({}, {})});
	Invocation.HandleArguments.Add(
		{TEXT("outer_asset"), OuterHandle});
	Invocation.HandleArguments.Add(
		{TEXT("object_class"), ClassHandle});
	return Invocation;
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

int32 CountExactOwnedObjects(
	UObject& Outer,
	const UClass& ObjectClass)
{
	TArray<UObject*> OwnedObjects;
	GetObjectsWithOuter(&Outer, OwnedObjects, AssetDocumentObjects::DirectOnly);
	int32 ExactCount = 0;
	for (const UObject* Object : OwnedObjects)
	{
		if (Object != nullptr
			&& Object->GetClass() == &ObjectClass)
		{
			++ExactCount;
		}
	}
	return ExactCount;
}

bool DeleteEmbeddedFixtureFiles(
	const FString& PackageFileName)
{
	const FString PackageBasePath = PackageFileName.LeftChop(
		FCString::Strlen(TEXT(".uasset")));
	const TArray<FString> SegmentPaths{
		PackageFileName,
		PackageBasePath + TEXT(".uexp"),
		PackageBasePath + TEXT(".ubulk"),
		PackageBasePath + TEXT(".uptnl"),
		PackageBasePath + TEXT(".m.ubulk"),
		PackageBasePath + TEXT(".upayload")};
	bool bDeleted = true;
	for (const FString& SegmentPath : SegmentPaths)
	{
		if (IFileManager::Get().FileExists(*SegmentPath))
		{
			bDeleted &= IFileManager::Get().Delete(
				*SegmentPath, false, true);
		}
	}
	return bDeleted;
}

bool CleanupEmbeddedFixture(
	UPackage*& LivePackage,
	const FString& PackageFileName)
{
	bool bUnloaded = true;
	if (LivePackage != nullptr && IsValid(LivePackage))
	{
		FAssetRegistryModule::GetRegistry().PackageDeleted(
			LivePackage);
		LivePackage->SetDirtyFlag(false);
		bUnloaded = UPackageTools::UnloadPackages({LivePackage});
	}
	LivePackage = nullptr;
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	return bUnloaded
		&& DeleteEmbeddedFixtureFiles(PackageFileName);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderEmbeddedAdmittedAssetUserDataTest,
	"UEAssetDoc.GenericProvider.Embedded.AdmittedAssetUserData",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderEmbeddedAdmittedAssetUserDataTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorEmbeddedProviderTests;
	const FAssetDocumentPrimitiveDescriptor* Descriptor =
		FindEmbeddedDescriptor();
	if (!TestNotNull(TEXT("embedded descriptor exists"), Descriptor))
	{
		return false;
	}

	TStrongObjectPtr<UTexture2D> Outer(
		UTexture2D::CreateTransient(1, 1));
	if (!TestTrue(TEXT("public helper creates test outer"), Outer.IsValid()))
	{
		return false;
	}
	IInterface_AssetUserData* AssetUserDataOwner =
		static_cast<IInterface_AssetUserData*>(Outer.Get());
	UClass* const EmbeddedClass = USubtitleAssetUserData::StaticClass();
	TestNull(TEXT("fixture has no pre-existing asset user data"),
		AssetUserDataOwner->GetAssetUserDataOfClass(EmbeddedClass));

	const FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Resolver(Context);
	const TOptional<FAssetDocumentOpaqueHandle> OuterHandle =
		IssueInputHandle(
			*this, Resolver, TEXT("outer handle issued"),
			EAssetDocumentHandleKind::Asset,
			EAssetDocumentHandleAccess::Mutable,
			MakeUnique<FTestObjectIdentityPayload>(
				*Outer, AssetPayloadTypeId));
	const TOptional<FAssetDocumentOpaqueHandle> ClassHandle =
		IssueInputHandle(
			*this, Resolver, TEXT("class handle issued"),
			EAssetDocumentHandleKind::Class,
			EAssetDocumentHandleAccess::ReadOnly,
			MakeUnique<FTestClassIdentityPayload>(*EmbeddedClass));
	if (!OuterHandle.IsSet() || !ClassHandle.IsSet())
	{
		return false;
	}

	const FAssetDocumentPrimitiveInvocation Invocation =
		MakeEmbeddedInvocation(
			*Descriptor, Context,
			OuterHandle.GetValue(), ClassHandle.GetValue(),
			TEXT("asset_user_data.v1"));
	FAssetDocumentEditorProviderTestTransaction Transaction(Context);
	const FAssetDocumentPrimitiveOutcome Outcome =
		CreateAssetDocumentEditorPrimitiveProvider()->Invoke(
			Invocation, Resolver, &Transaction);
	if (!TestEqual(
		TEXT("admitted asset user data succeeds"),
		Outcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
	{
		AssetUserDataOwner->RemoveUserDataOfClass(EmbeddedClass);
		return true;
	}

	UAssetUserData* const Attached =
		AssetUserDataOwner->GetAssetUserDataOfClass(EmbeddedClass);
	TestNotNull(TEXT("public helper attaches the subobject"), Attached);
	if (Attached != nullptr)
	{
		TestEqual(TEXT("attached class is exact"),
			Attached->GetClass(), EmbeddedClass);
		TestEqual(TEXT("attached outer is exact"),
			Attached->GetOuter(), static_cast<UObject*>(Outer.Get()));
	}

	const FString OuterPath = Outer->GetPathName();
	const FString ClassPath = EmbeddedClass->GetPathName();
	const FString SubobjectPath =
		Attached != nullptr ? Attached->GetPathName() : FString();
	TestEqual(TEXT("output has exact keys"),
		Outcome.Value.ObjectKeys,
		TArray<FString>({
			TEXT("category_id"),
			TEXT("canonical_class"),
			TEXT("outer_path"),
			TEXT("subobject_path")}));
	TestTrue(TEXT("output category is exact"),
		HasStringField(
			Outcome.Value, TEXT("category_id"),
			TEXT("asset_user_data.v1")));
	TestTrue(TEXT("output class is canonical"),
		HasStringField(
			Outcome.Value, TEXT("canonical_class"), ClassPath));
	TestTrue(TEXT("output outer path is canonical"),
		HasStringField(
			Outcome.Value, TEXT("outer_path"), OuterPath));
	TestTrue(TEXT("output subobject path is canonical"),
		HasStringField(
			Outcome.Value, TEXT("subobject_path"), SubobjectPath));

	TestEqual(TEXT("one subobject result group"),
		Outcome.HandleResults.Num(), 1);
	const FAssetDocumentOpaqueHandle* SubobjectHandle = nullptr;
	if (Outcome.HandleResults.Num() == 1)
	{
		const FAssetDocumentPrimitiveHandleResult& Result =
			Outcome.HandleResults[0];
		TestEqual(TEXT("subobject result name"),
			Result.Name, FString(TEXT("subobject")));
		TestEqual(TEXT("one subobject handle"),
			Result.Handles.Num(), 1);
		if (Result.Handles.Num() == 1)
		{
			SubobjectHandle = &Result.Handles[0];
			TestEqual(TEXT("subobject handle kind"),
				SubobjectHandle->Kind,
				EAssetDocumentHandleKind::Subobject);
			TestEqual(TEXT("subobject handle access"),
				SubobjectHandle->Access,
				EAssetDocumentHandleAccess::Mutable);
		}
	}

	if (SubobjectHandle != nullptr)
	{
		FAssetDocumentVerifiedLeaseResult Lease =
			Resolver.AcquireVerifiedLease(
				*SubobjectHandle,
				MakeSubobjectValidationContext(Invocation));
		TestTrue(TEXT("subobject handle acquires"),
			Lease.Lease.IsValid());
		if (Lease.Lease)
		{
			INativeIdentityPayload* Payload =
				TryGetNativeIdentityPayload(
					Lease.Lease->GetPayloadChecked(),
					SubobjectPayloadTypeId);
			FObjectIdentityView View;
			TestTrue(TEXT("subobject payload identity is exact"),
				Payload != nullptr
					&& Payload->TryGetObjectIdentity(View)
					&& View.Object == Attached
					&& View.CanonicalObjectPath == SubobjectPath);
		}
	}

	const TArray<FAssetDocumentMutationFootprint>& Footprint =
		Transaction.GetEnlistedFootprint();
	TestEqual(TEXT("one ownership footprint"),
		Footprint.Num(), 1);
	if (Footprint.Num() == 1)
	{
		TestEqual(TEXT("outer state footprint kind"),
			Footprint[0].Kind,
			EAssetDocumentMutationFootprintKind::UObjectState);
		TestEqual(TEXT("outer state footprint identity"),
			Footprint[0].StableId, OuterPath);
		TestFalse(TEXT("rollback component identity is present"),
			Footprint[0].RollbackComponentId.IsEmpty());
	}
	TestEqual(TEXT("transaction entered mutation"),
		Transaction.GetState(),
		EAssetDocumentAtomicState::Mutating);

	AssetUserDataOwner->RemoveUserDataOfClass(EmbeddedClass);
	TestNull(TEXT("test cleanup removes the attachment"),
		AssetUserDataOwner->GetAssetUserDataOfClass(EmbeddedClass));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderEmbeddedUnsupportedCategoryTest,
	"UEAssetDoc.GenericProvider.Embedded.UnsupportedCategory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderEmbeddedUnsupportedCategoryTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorEmbeddedProviderTests;
	const FAssetDocumentPrimitiveDescriptor* Descriptor =
		FindEmbeddedDescriptor();
	if (!TestNotNull(TEXT("embedded descriptor exists"), Descriptor))
	{
		return false;
	}

	TStrongObjectPtr<UTexture2D> Outer(
		UTexture2D::CreateTransient(1, 1));
	if (!TestTrue(TEXT("public helper creates test outer"), Outer.IsValid()))
	{
		return false;
	}
	IInterface_AssetUserData* AssetUserDataOwner =
		static_cast<IInterface_AssetUserData*>(Outer.Get());
	UClass* const EmbeddedClass = USubtitleAssetUserData::StaticClass();
	TestNull(TEXT("fixture has no pre-existing asset user data"),
		AssetUserDataOwner->GetAssetUserDataOfClass(EmbeddedClass));
	const int32 OwnedObjectsBefore =
		CountExactOwnedObjects(*Outer, *EmbeddedClass);

	const FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Resolver(Context);
	const TOptional<FAssetDocumentOpaqueHandle> OuterHandle =
		IssueInputHandle(
			*this, Resolver, TEXT("outer handle issued"),
			EAssetDocumentHandleKind::Asset,
			EAssetDocumentHandleAccess::Mutable,
			MakeUnique<FTestObjectIdentityPayload>(
				*Outer, AssetPayloadTypeId));
	const TOptional<FAssetDocumentOpaqueHandle> ClassHandle =
		IssueInputHandle(
			*this, Resolver, TEXT("class handle issued"),
			EAssetDocumentHandleKind::Class,
			EAssetDocumentHandleAccess::ReadOnly,
			MakeUnique<FTestClassIdentityPayload>(*EmbeddedClass));
	if (!OuterHandle.IsSet() || !ClassHandle.IsSet())
	{
		return false;
	}

	const FAssetDocumentPrimitiveInvocation Invocation =
		MakeEmbeddedInvocation(
			*Descriptor, Context,
			OuterHandle.GetValue(), ClassHandle.GetValue(),
			TEXT("unknown_embedded_category.v1"));
	FAssetDocumentEditorProviderTestTransaction Transaction(Context);
	const FAssetDocumentPrimitiveOutcome Outcome =
		CreateAssetDocumentEditorPrimitiveProvider()->Invoke(
			Invocation, Resolver, &Transaction);

	TestEqual(TEXT("unknown category fails closed"),
		Outcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Failed);
	TestTrue(TEXT("unknown category has stable diagnostic"),
		HasDiagnostic(
			Outcome,
			TEXT("UnsupportedEmbeddedObjectLifecycle")));
	TestTrue(TEXT("unknown category issues no handles"),
		Outcome.HandleResults.IsEmpty());
	TestTrue(TEXT("unknown category reports no changed set"),
		Outcome.ChangedSet.IsEmpty());
	TestTrue(TEXT("unknown category enlists no footprint"),
		Transaction.GetEnlistedFootprint().IsEmpty());
	TestEqual(TEXT("unknown category leaves transaction live"),
		Transaction.GetState(),
		EAssetDocumentAtomicState::Live);
	TestNull(TEXT("unknown category creates no attachment"),
		AssetUserDataOwner->GetAssetUserDataOfClass(EmbeddedClass));
	TestEqual(TEXT("unknown category constructs no owned object"),
		CountExactOwnedObjects(*Outer, *EmbeddedClass),
		OwnedObjectsBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderEmbeddedComplexAuthoredRejectedTest,
	"UEAssetDoc.GenericProvider.Embedded.ComplexAuthoredRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderEmbeddedComplexAuthoredRejectedTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorEmbeddedProviderTests;
	const FAssetDocumentPrimitiveDescriptor* Descriptor =
		FindEmbeddedDescriptor();
	if (!TestNotNull(TEXT("embedded descriptor exists"), Descriptor))
	{
		return false;
	}

	TStrongObjectPtr<UTexture2D> Outer(
		UTexture2D::CreateTransient(1, 1));
	if (!TestTrue(TEXT("public helper creates test outer"), Outer.IsValid()))
	{
		return false;
	}
	IInterface_AssetUserData* AssetUserDataOwner =
		static_cast<IInterface_AssetUserData*>(Outer.Get());
	const FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Resolver(Context);
	const TOptional<FAssetDocumentOpaqueHandle> OuterHandle =
		IssueInputHandle(
			*this, Resolver, TEXT("outer handle issued"),
			EAssetDocumentHandleKind::Asset,
			EAssetDocumentHandleAccess::Mutable,
			MakeUnique<FTestObjectIdentityPayload>(
				*Outer, AssetPayloadTypeId));
	if (!OuterHandle.IsSet())
	{
		return false;
	}

	struct FForbiddenClassCase
	{
		const TCHAR* Label = nullptr;
		UClass* ObjectClass = nullptr;
	};
	const FForbiddenClassCase Cases[] = {
		{TEXT("graph"), UEdGraph::StaticClass()},
		{TEXT("node"), UEdGraphNode::StaticClass()},
		{TEXT("anim notify state"), UAnimNotifyState::StaticClass()},
	};
	for (const FForbiddenClassCase& Case : Cases)
	{
		const int32 OwnedObjectsBefore =
			CountExactOwnedObjects(*Outer, *Case.ObjectClass);
		const TOptional<FAssetDocumentOpaqueHandle> ClassHandle =
			IssueInputHandle(
				*this, Resolver,
				*FString::Printf(
					TEXT("%s class handle issued"), Case.Label),
				EAssetDocumentHandleKind::Class,
				EAssetDocumentHandleAccess::ReadOnly,
				MakeUnique<FTestClassIdentityPayload>(
					*Case.ObjectClass));
		if (!ClassHandle.IsSet())
		{
			continue;
		}

		const FAssetDocumentPrimitiveInvocation Invocation =
			MakeEmbeddedInvocation(
				*Descriptor, Context,
				OuterHandle.GetValue(),
				ClassHandle.GetValue(),
				TEXT("asset_user_data.v1"));
		FAssetDocumentEditorProviderTestTransaction Transaction(Context);
		const FAssetDocumentPrimitiveOutcome Outcome =
			CreateAssetDocumentEditorPrimitiveProvider()->Invoke(
				Invocation, Resolver, &Transaction);
		const FString Prefix = FString::Printf(
			TEXT("%s complex authored class"), Case.Label);

		TestEqual(
			*(Prefix + TEXT(" fails closed")),
			Outcome.Status,
			EAssetDocumentPrimitiveOutcomeStatus::Failed);
		TestTrue(
			*(Prefix + TEXT(" has stable diagnostic")),
			HasDiagnostic(
				Outcome,
				TEXT("UnsupportedEmbeddedObjectLifecycle")));
		TestTrue(
			*(Prefix + TEXT(" issues no handles")),
			Outcome.HandleResults.IsEmpty());
		TestTrue(
			*(Prefix + TEXT(" reports no changed set")),
			Outcome.ChangedSet.IsEmpty());
		TestTrue(
			*(Prefix + TEXT(" enlists no footprint")),
			Transaction.GetEnlistedFootprint().IsEmpty());
		TestEqual(
			*(Prefix + TEXT(" leaves transaction live")),
			Transaction.GetState(),
			EAssetDocumentAtomicState::Live);
		const TArray<UAssetUserData*>* Attachments =
			AssetUserDataOwner->GetAssetUserDataArray();
		TestTrue(
			*(Prefix + TEXT(" creates no attachment")),
			Attachments != nullptr && Attachments->IsEmpty());
		TestEqual(
			*(Prefix + TEXT(" constructs no owned object")),
			CountExactOwnedObjects(*Outer, *Case.ObjectClass),
			OwnedObjectsBefore);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderEmbeddedFailureRollbackGuardTest,
	"UEAssetDoc.GenericProvider.Embedded.FailureRollbackGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderEmbeddedFailureRollbackGuardTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorEmbeddedProviderTests;
	const FAssetDocumentPrimitiveDescriptor* Descriptor =
		FindEmbeddedDescriptor();
	if (!TestNotNull(TEXT("embedded descriptor exists"), Descriptor))
	{
		return false;
	}

	TStrongObjectPtr<UTexture2D> Outer(
		UTexture2D::CreateTransient(1, 1));
	if (!TestTrue(TEXT("public helper creates test outer"), Outer.IsValid()))
	{
		return false;
	}
	IInterface_AssetUserData* AssetUserDataOwner =
		static_cast<IInterface_AssetUserData*>(Outer.Get());
	UClass* const EmbeddedClass = USubtitleAssetUserData::StaticClass();
	const int32 OwnedObjectsBefore =
		CountExactOwnedObjects(*Outer, *EmbeddedClass);
	TestNull(TEXT("fixture has no pre-existing asset user data"),
		AssetUserDataOwner->GetAssetUserDataOfClass(EmbeddedClass));

	const FAssetDocumentEditorProviderTestContext Context;
	FRejectSubobjectIssueResolver Resolver(Context);
	const TOptional<FAssetDocumentOpaqueHandle> OuterHandle =
		IssueInputHandle(
			*this, Resolver, TEXT("outer handle issued"),
			EAssetDocumentHandleKind::Asset,
			EAssetDocumentHandleAccess::Mutable,
			MakeUnique<FTestObjectIdentityPayload>(
				*Outer, AssetPayloadTypeId));
	const TOptional<FAssetDocumentOpaqueHandle> ClassHandle =
		IssueInputHandle(
			*this, Resolver, TEXT("class handle issued"),
			EAssetDocumentHandleKind::Class,
			EAssetDocumentHandleAccess::ReadOnly,
			MakeUnique<FTestClassIdentityPayload>(*EmbeddedClass));
	if (!OuterHandle.IsSet() || !ClassHandle.IsSet())
	{
		return false;
	}

	const FAssetDocumentPrimitiveInvocation Invocation =
		MakeEmbeddedInvocation(
			*Descriptor, Context,
			OuterHandle.GetValue(), ClassHandle.GetValue(),
			TEXT("asset_user_data.v1"));
	FAssetDocumentEditorProviderTestTransaction Transaction(Context);
	const FAssetDocumentPrimitiveOutcome Outcome =
		CreateAssetDocumentEditorPrimitiveProvider()->Invoke(
			Invocation, Resolver, &Transaction);

	TestEqual(TEXT("post-create injected failure fails"),
		Outcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Failed);
	TestTrue(TEXT("injected failure diagnostic is stable"),
		HasDiagnostic(
			Outcome,
			TEXT("TestHandle.InjectedPostCreateFailure")));
	TestEqual(TEXT("one post-create handle failure is injected"),
		Resolver.GetSubobjectIssueAttempts(), 1);
	TestTrue(TEXT("failure seam observes the live attachment"),
		Resolver.ObservedExactLiveAttachment());
	TestTrue(TEXT("failed outcome returns no handles"),
		Outcome.HandleResults.IsEmpty());
	TestTrue(TEXT("failed outcome reports no changed set"),
		Outcome.ChangedSet.IsEmpty());
	TestNull(TEXT("public inverse removes the attachment"),
		AssetUserDataOwner->GetAssetUserDataOfClass(EmbeddedClass));

	const TArray<FAssetDocumentMutationFootprint>& Footprint =
		Transaction.GetEnlistedFootprint();
	TestEqual(TEXT("post-create failure preserves one footprint"),
		Footprint.Num(), 1);
	if (Footprint.Num() == 1)
	{
		TestEqual(TEXT("rollback footprint kind"),
			Footprint[0].Kind,
			EAssetDocumentMutationFootprintKind::UObjectState);
		TestEqual(TEXT("rollback footprint outer identity"),
			Footprint[0].StableId, Outer->GetPathName());
		TestFalse(TEXT("rollback component identity is present"),
			Footprint[0].RollbackComponentId.IsEmpty());
	}
	TestEqual(TEXT("failed mutation remains enlisted"),
		Transaction.GetState(),
		EAssetDocumentAtomicState::Mutating);

	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestEqual(TEXT("inverse leaves no direct-outer object residue"),
		CountExactOwnedObjects(*Outer, *EmbeddedClass),
		OwnedObjectsBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderEmbeddedSaveReloadTest,
	"UEAssetDoc.GenericProvider.Embedded.SaveReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderEmbeddedSaveReloadTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorEmbeddedProviderTests;
	const FAssetDocumentPrimitiveDescriptor* Descriptor =
		FindEmbeddedDescriptor();
	if (!TestNotNull(TEXT("embedded descriptor exists"), Descriptor))
	{
		return false;
	}

	const FString AssetName = FString::Printf(
		TEXT("EmbeddedSaveReload_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString PackagePath =
		TEXT("/Game/AssetDocumentTests/GenericProvider");
	const FString PackageName = FString::Printf(
		TEXT("%s/%s"), *PackagePath, *AssetName);
	const FString ObjectPath = FString::Printf(
		TEXT("%s.%s"), *PackageName, *AssetName);
	const FString PackageFileName =
		FPackageName::LongPackageNameToFilename(
			PackageName,
			FPackageName::GetAssetPackageExtension());
	UPackage* LivePackage = nullptr;
	ON_SCOPE_EXIT
	{
		CleanupEmbeddedFixture(
			LivePackage, PackageFileName);
	};
	UTexture2DFactoryNew* FixtureFactory =
		GetMutableDefault<UTexture2DFactoryNew>();
	if (!TestNotNull(TEXT("test obtains the public texture factory"),
		FixtureFactory))
	{
		return false;
	}

	UObject* CreatedAsset = IAssetTools::Get().CreateAsset(
		AssetName,
		PackagePath,
		UTexture2D::StaticClass(),
		FixtureFactory,
		FName(TEXT("AssetDocumentEmbeddedSaveReloadTest")));
	LivePackage = CreatedAsset != nullptr
		? CreatedAsset->GetOutermost()
		: FindPackage(nullptr, *PackageName);
	UTexture2D* Outer = Cast<UTexture2D>(CreatedAsset);
	if (!TestNotNull(
		TEXT("test factory creates a legal savable outer"), Outer))
	{
		return false;
	}
	IInterface_AssetUserData* AssetUserDataOwner =
		static_cast<IInterface_AssetUserData*>(Outer);
	UClass* const EmbeddedClass = USubtitleAssetUserData::StaticClass();
	TestNull(TEXT("fixture has no pre-existing asset user data"),
		AssetUserDataOwner->GetAssetUserDataOfClass(EmbeddedClass));

	const FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Resolver(Context);
	const TOptional<FAssetDocumentOpaqueHandle> OuterHandle =
		IssueInputHandle(
			*this, Resolver, TEXT("outer handle issued"),
			EAssetDocumentHandleKind::Asset,
			EAssetDocumentHandleAccess::Mutable,
			MakeUnique<FTestObjectIdentityPayload>(
				*Outer, AssetPayloadTypeId));
	const TOptional<FAssetDocumentOpaqueHandle> ClassHandle =
		IssueInputHandle(
			*this, Resolver, TEXT("class handle issued"),
			EAssetDocumentHandleKind::Class,
			EAssetDocumentHandleAccess::ReadOnly,
			MakeUnique<FTestClassIdentityPayload>(*EmbeddedClass));
	if (!OuterHandle.IsSet() || !ClassHandle.IsSet())
	{
		return false;
	}

	const FAssetDocumentPrimitiveInvocation Invocation =
		MakeEmbeddedInvocation(
			*Descriptor, Context,
			OuterHandle.GetValue(), ClassHandle.GetValue(),
			TEXT("asset_user_data.v1"));
	FAssetDocumentEditorProviderTestTransaction Transaction(Context);
	const FAssetDocumentPrimitiveOutcome Outcome =
		CreateAssetDocumentEditorPrimitiveProvider()->Invoke(
			Invocation, Resolver, &Transaction);
	if (!TestEqual(
		TEXT("admitted persistent attachment succeeds"),
		Outcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
	{
		return true;
	}

	UAssetUserData* Attached =
		AssetUserDataOwner->GetAssetUserDataOfClass(EmbeddedClass);
	if (!TestNotNull(TEXT("admitted attachment exists before save"),
		Attached))
	{
		return true;
	}
	const FString OuterPath = Outer->GetPathName();
	const FString SubobjectPath = Attached->GetPathName();
	TestTrue(TEXT("outcome records the persisted outer identity"),
		HasStringField(
			Outcome.Value, TEXT("outer_path"), OuterPath));
	TestTrue(TEXT("outcome records the persisted subobject identity"),
		HasStringField(
			Outcome.Value, TEXT("subobject_path"), SubobjectPath));

	TestTrue(TEXT("fixture transaction becomes commit-ready"),
		Transaction.MarkReadyToCommit().bValid);
	LivePackage->MarkPackageDirty();
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	const bool bSaved = UPackage::SavePackage(
		LivePackage, Outer, *PackageFileName, SaveArgs);
	if (!TestTrue(TEXT("test-owned fixture save succeeds"), bSaved))
	{
		return true;
	}

	FAssetDocumentDurableCompletionReceipt Receipt;
	Receipt.RequestId = Context.RequestId;
	Receipt.TransactionId = Context.TransactionId;
	Receipt.JournalId =
		TEXT("fixture-only:embedded-save-reload-test");
	Receipt.PackageContentDigest =
		TEXT("fixture-only:package-saved");
	Receipt.SidecarContentDigest =
		TEXT("fixture-only:no-sidecar");
	Receipt.bSyncStateAdvanced = true;
	TestTrue(TEXT("saved fixture transaction commits"),
		Transaction.Commit(Receipt).bValid);
	TestEqual(TEXT("fixture transaction is committed"),
		Transaction.GetState(),
		EAssetDocumentAtomicState::Committed);

	// The new texture builds asynchronously. A texture whose resource was made while its data was still building can
	// keep that stand-in after the build and read as still compiling, which unloading reports; the build is awaited
	// and the resource made again from the built data.
	FAssetCompilingManager::Get().FinishAllCompilation();
	Outer->BlockOnAnyAsyncBuild();
	if (Outer->IsCompiling())
	{
		Outer->UpdateResource();
	}
	Attached = nullptr;
	AssetUserDataOwner = nullptr;
	Outer = nullptr;
	UPackage* PackageToUnload = LivePackage;
	LivePackage = nullptr;
	const bool bUnloaded =
		UPackageTools::UnloadPackages({PackageToUnload});
	if (!TestTrue(TEXT("saved fixture unloads for a fresh read"),
		bUnloaded))
	{
		LivePackage = PackageToUnload;
		return true;
	}

	UTexture2D* ReloadedOuter =
		LoadObject<UTexture2D>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("saved fixture reloads by canonical path"),
		ReloadedOuter))
	{
		return true;
	}
	LivePackage = ReloadedOuter->GetOutermost();
	IInterface_AssetUserData* ReloadedOwner =
		static_cast<IInterface_AssetUserData*>(ReloadedOuter);
	UAssetUserData* ReloadedAttachment =
		ReloadedOwner->GetAssetUserDataOfClass(EmbeddedClass);
	if (!TestNotNull(TEXT("admitted attachment survives reload"),
		ReloadedAttachment))
	{
		return true;
	}
	TestEqual(TEXT("reloaded attachment has exact admitted class"),
		ReloadedAttachment->GetClass(), EmbeddedClass);
	TestEqual(TEXT("reloaded attachment has exact outer"),
		ReloadedAttachment->GetOuter(),
		static_cast<UObject*>(ReloadedOuter));
	TestEqual(TEXT("reloaded outer preserves canonical identity"),
		ReloadedOuter->GetPathName(), OuterPath);
	TestEqual(TEXT("reloaded subobject preserves canonical identity"),
		ReloadedAttachment->GetPathName(), SubobjectPath);

	TestTrue(TEXT("test cleanup unloads and deletes fixture"),
		CleanupEmbeddedFixture(
			LivePackage, PackageFileName));
	return true;
}
#endif
