// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Lifecycle/AssetDocumentEditorLifecyclePayloads.h"

#include "UObject/Object.h"
#include "UObject/Package.h"
#include "UObject/StructOnScope.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"

namespace
{
uint64 GetLifecycleObjectRevision(const UObject& Object)
{
	return static_cast<uint64>(Object.GetUniqueID());
}

class FAssetDocumentEditorResolvedAssetPayload final
	: public UE::AssetDocument::EditorPrimitive::INativeIdentityPayload
{
public:
	explicit FAssetDocumentEditorResolvedAssetPayload(UObject& InAsset)
		: Asset(&InAsset)
		, CanonicalObjectPath(InAsset.GetPathName())
		, TypeRevision(GetLifecycleObjectRevision(InAsset))
	{
	}

	virtual FName GetPayloadTypeId() const override
	{
		return UE::AssetDocument::EditorPrimitive::AssetPayloadTypeId;
	}

	virtual bool IsAlive() const override
	{
		const UObject* Resolved = Asset.Get();
		return Resolved != nullptr
			&& Resolved->GetPathName() == CanonicalObjectPath
			&& GetLifecycleObjectRevision(*Resolved) == TypeRevision;
	}

	virtual bool TryGetObjectIdentity(
		UE::AssetDocument::EditorPrimitive::FObjectIdentityView& OutView)
		const override
	{
		if (!IsAlive())
		{
			return false;
		}
		OutView.Object = Asset.Get();
		OutView.CanonicalObjectPath = CanonicalObjectPath;
		OutView.TypeRevision = TypeRevision;
		return true;
	}

private:
	TWeakObjectPtr<UObject> Asset;
	FString CanonicalObjectPath;
	uint64 TypeRevision = 0;
};

class FAssetDocumentEditorResolvedPackagePayload final
	: public UE::AssetDocument::EditorPrimitive::INativeIdentityPayload
{
public:
	explicit FAssetDocumentEditorResolvedPackagePayload(UObject& InAsset)
		: Package(InAsset.GetOutermost())
		, PackageName(InAsset.GetOutermost()->GetName())
		, TypeRevision(GetLifecycleObjectRevision(*InAsset.GetOutermost()))
	{
	}

	virtual FName GetPayloadTypeId() const override
	{
		return UE::AssetDocument::EditorPrimitive::AssetPayloadTypeId;
	}

	virtual bool IsAlive() const override
	{
		const UPackage* Resolved = Package.Get();
		return Resolved != nullptr
			&& Resolved->GetName() == PackageName
			&& GetLifecycleObjectRevision(*Resolved) == TypeRevision;
	}

	virtual bool TryGetPackageIdentity(
		UE::AssetDocument::EditorPrimitive::FPackageIdentityView& OutView)
		const override
	{
		if (!IsAlive())
		{
			return false;
		}
		OutView.Package = Package.Get();
		OutView.PackageName = PackageName;
		OutView.Filename = FStringView();
		OutView.TypeRevision = TypeRevision;
		return true;
	}

private:
	TWeakObjectPtr<UPackage> Package;
	FString PackageName;
	uint64 TypeRevision = 0;
};
}

TUniquePtr<IAssetDocumentNativeHandlePayload>
MakeAssetDocumentEditorResolvedAssetPayload(UObject& Asset)
{
	return MakeUnique<FAssetDocumentEditorResolvedAssetPayload>(Asset);
}

TUniquePtr<IAssetDocumentNativeHandlePayload>
MakeAssetDocumentEditorResolvedPackagePayload(UObject& Asset)
{
	return MakeUnique<FAssetDocumentEditorResolvedPackagePayload>(Asset);
}

const FAssetDocumentEditorCreationRecord*
TryGetAssetDocumentEditorCreatedAssetRecord(
	IAssetDocumentNativeHandlePayload& Payload)
{
	using namespace UE::AssetDocument::EditorPrimitive;
	INativeIdentityPayload* Identity =
		TryGetNativeIdentityPayload(Payload, CreatedAssetPayloadTypeId);
	if (Identity == nullptr)
	{
		return nullptr;
	}
	return &static_cast<FAssetDocumentEditorCreatedAssetPayload*>(
		Identity)->GetCreationRecord();
}

const FAssetDocumentEditorCreationRecord*
TryGetAssetDocumentEditorCreatedPackageRecord(
	IAssetDocumentNativeHandlePayload& Payload)
{
	using namespace UE::AssetDocument::EditorPrimitive;
	INativeIdentityPayload* Identity =
		TryGetNativeIdentityPayload(Payload, CreatedPackagePayloadTypeId);
	if (Identity == nullptr)
	{
		return nullptr;
	}
	return &static_cast<FAssetDocumentEditorCreatedPackagePayload*>(
		Identity)->GetCreationRecord();
}

FAssetDocumentEditorCreationRecord::FAssetDocumentEditorCreationRecord(
	FGuid InRequestId,
	FGuid InTransactionId,
	uint64 InRuntimeRevision,
	FString InFactoryId,
	FString InCanonicalObjectPath,
	FString InPackageName,
	FString InPackageFilename,
	bool bInObjectExistedBefore,
	bool bInPackageExistedBefore,
	bool bInFileExistedBefore,
	bool bInPackageWasDirty,
	UObject& InAsset,
	UPackage& InPackage)
	: RequestId(InRequestId)
	, TransactionId(InTransactionId)
	, RuntimeRevision(InRuntimeRevision)
	, FactoryId(MoveTemp(InFactoryId))
	, CanonicalObjectPath(MoveTemp(InCanonicalObjectPath))
	, PackageName(MoveTemp(InPackageName))
	, PackageFilename(MoveTemp(InPackageFilename))
	, bObjectExistedBefore(bInObjectExistedBefore)
	, bPackageExistedBefore(bInPackageExistedBefore)
	, bFileExistedBefore(bInFileExistedBefore)
	, bPackageWasDirty(bInPackageWasDirty)
	, Asset(&InAsset)
	, Package(&InPackage)
{
}

FAssetDocumentEditorCreatedAssetPayload::
	FAssetDocumentEditorCreatedAssetPayload(
		UObject& InAsset,
		TSharedRef<const FAssetDocumentEditorCreationRecord> InRecord)
	: Asset(&InAsset)
	, Record(MoveTemp(InRecord))
{
}

FName FAssetDocumentEditorCreatedAssetPayload::GetPayloadTypeId() const
{
	return UE::AssetDocument::EditorPrimitive::CreatedAssetPayloadTypeId;
}

bool FAssetDocumentEditorCreatedAssetPayload::IsAlive() const
{
	const UObject* Resolved = Asset.Get();
	return Resolved != nullptr
		&& Record->Asset.Get() == Resolved
		&& Resolved->GetPathName() == Record->CanonicalObjectPath
		&& Resolved->GetOutermost() == Record->Package.Get();
}

bool FAssetDocumentEditorCreatedAssetPayload::TryGetObjectIdentity(
	UE::AssetDocument::EditorPrimitive::FObjectIdentityView& OutView) const
{
	if (!IsAlive())
	{
		return false;
	}
	OutView.Object = Asset.Get();
	OutView.CanonicalObjectPath = Record->CanonicalObjectPath;
	OutView.TypeRevision = GetLifecycleObjectRevision(*Asset.Get());
	return true;
}

const FAssetDocumentEditorCreationRecord&
FAssetDocumentEditorCreatedAssetPayload::GetCreationRecord() const
{
	return *Record;
}

FAssetDocumentEditorCreatedPackagePayload::
	FAssetDocumentEditorCreatedPackagePayload(
		UPackage& InPackage,
		TSharedRef<const FAssetDocumentEditorCreationRecord> InRecord)
	: Package(&InPackage)
	, Record(MoveTemp(InRecord))
{
}

FName FAssetDocumentEditorCreatedPackagePayload::GetPayloadTypeId() const
{
	return UE::AssetDocument::EditorPrimitive::CreatedPackagePayloadTypeId;
}

bool FAssetDocumentEditorCreatedPackagePayload::IsAlive() const
{
	const UPackage* Resolved = Package.Get();
	return Resolved != nullptr
		&& Record->Package.Get() == Resolved
		&& Resolved->GetName() == Record->PackageName;
}

bool FAssetDocumentEditorCreatedPackagePayload::TryGetPackageIdentity(
	UE::AssetDocument::EditorPrimitive::FPackageIdentityView& OutView) const
{
	if (!IsAlive())
	{
		return false;
	}
	OutView.Package = Package.Get();
	OutView.PackageName = Record->PackageName;
	OutView.Filename = Record->PackageFilename;
	OutView.TypeRevision = GetLifecycleObjectRevision(*Package.Get());
	return true;
}

const FAssetDocumentEditorCreationRecord&
FAssetDocumentEditorCreatedPackagePayload::GetCreationRecord() const
{
	return *Record;
}

FAssetDocumentEditorScratchPayload::FAssetDocumentEditorScratchPayload(
	UObject& InScratchAsset)
	: ScratchAsset(&InScratchAsset)
	, CanonicalObjectPath(InScratchAsset.GetPathName())
{
}

FAssetDocumentEditorScratchPayload::~FAssetDocumentEditorScratchPayload()
{
	if (UObject* const Scratch = ScratchAsset.Get())
	{
		ReleaseAssetDocumentEditorScratchNativeBindings(*Scratch);
	}
}

FName FAssetDocumentEditorScratchPayload::GetPayloadTypeId() const
{
	return UE::AssetDocument::EditorPrimitive::ScratchPayloadTypeId;
}

bool FAssetDocumentEditorScratchPayload::IsAlive() const
{
	return ScratchAsset.IsValid();
}

bool FAssetDocumentEditorScratchPayload::TryGetObjectIdentity(
	UE::AssetDocument::EditorPrimitive::FObjectIdentityView& OutView) const
{
	OutView.Object = ScratchAsset.Get();
	OutView.CanonicalObjectPath = CanonicalObjectPath;
	OutView.TypeRevision = 1;
	return OutView.Object != nullptr;
}

namespace
{
// FDataflowInstance (DataflowEngine; GeometryCollection, ChaosClothAsset, ...) binds itself raw to
// FCoreUObjectDelegates::OnObjectPropertyChanged in its owner-taking constructor and keeps the handle in a plain
// member that its implicit assignment copies. Duplication initialises every property of the copy by assignment from
// the class default object (FObjectInitializer::InitProperties with bCopyTransientsFromClassDefaults), so a duplicated
// owner -- a scratch clone -- holds the class default object's handle, while its own binding is held by nobody: once
// the scratch is collected its destructor removes the wrong binding and the next property change notification calls
// into freed memory. The scratch's own binding is removed here, and the value is reset to a default one (no handle),
// so its destructor removes nothing else.
const UScriptStruct* FindDataflowInstanceStruct()
{
	return FindObject<UScriptStruct>(nullptr, TEXT("/Script/DataflowEngine.DataflowInstance"));
}

bool DoesStructReach(const UStruct& Struct, const UScriptStruct& Target, TSet<const UStruct*>& Visited);

bool DoesPropertyReach(const FProperty* Property, const UScriptStruct& Target, TSet<const UStruct*>& Visited)
{
	if (Property == nullptr)
	{
		return false;
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
	{
		return Struct->Struct != nullptr && (Struct->Struct == &Target || DoesStructReach(*Struct->Struct, Target, Visited));
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(Property))
	{
		return DoesPropertyReach(Array->Inner, Target, Visited);
	}
	if (const FSetProperty* Set = CastField<FSetProperty>(Property))
	{
		return DoesPropertyReach(Set->ElementProp, Target, Visited);
	}
	if (const FMapProperty* Map = CastField<FMapProperty>(Property))
	{
		return DoesPropertyReach(Map->KeyProp, Target, Visited) || DoesPropertyReach(Map->ValueProp, Target, Visited);
	}
	if (const FOptionalProperty* Optional = CastField<FOptionalProperty>(Property))
	{
		return DoesPropertyReach(Optional->GetValueProperty(), Target, Visited);
	}
	return false;
}

bool DoesStructReach(const UStruct& Struct, const UScriptStruct& Target, TSet<const UStruct*>& Visited)
{
	bool bAlreadyVisited = false;
	Visited.Add(&Struct, &bAlreadyVisited);
	if (bAlreadyVisited)
	{
		return false;
	}
	for (TFieldIterator<FProperty> It(&Struct); It; ++It)
	{
		if (DoesPropertyReach(*It, Target, Visited))
		{
			return true;
		}
	}
	return false;
}

void ReleaseObjectDataflowBindings(UObject& Object, const UScriptStruct& DataflowInstance)
{
	TSet<const UStruct*> Visited;
	if (!DoesStructReach(*Object.GetClass(), DataflowInstance, Visited))
	{
		return;
	}
	TArray<void*> Values;
	for (TPropertyValueIterator<FStructProperty> It(Object.GetClass(), &Object); It; ++It)
	{
		if (It.Key()->Struct == &DataflowInstance)
		{
			Values.Add(const_cast<void*>(It.Value()));
			It.SkipRecursiveProperty();
		}
	}
	for (void* Value : Values)
	{
		FCoreUObjectDelegates::OnObjectPropertyChanged.RemoveAll(Value);
		const FStructOnScope Default(&DataflowInstance);
		DataflowInstance.CopyScriptStruct(Value, Default.GetStructMemory());
	}
}
}

void ReleaseAssetDocumentEditorScratchNativeBindings(UObject& Scratch)
{
	const UScriptStruct* const DataflowInstance = FindDataflowInstanceStruct();
	if (DataflowInstance == nullptr)
	{
		return;
	}
	ReleaseObjectDataflowBindings(Scratch, *DataflowInstance);
	TArray<UObject*> Inner;
	GetObjectsWithOuter(&Scratch, Inner);
	for (UObject* Object : Inner)
	{
		if (Object != nullptr)
		{
			ReleaseObjectDataflowBindings(*Object, *DataflowInstance);
		}
	}
}
