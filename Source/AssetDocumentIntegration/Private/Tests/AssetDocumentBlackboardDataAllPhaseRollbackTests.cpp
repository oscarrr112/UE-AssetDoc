// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentModule.h"
#include "AssetDocumentTestDirectoryBlock.h"
#include "AssetDocumentService.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/SoftObjectPath.h"
#include "AssetDocumentPluginPaths.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace BlackboardDataAllPhaseRollbackTestsLocal
{
FString MakeTarget(const TCHAR* Name)
{
	return FString::Printf(TEXT("/Game/AssetDocumentG3BlackboardData/%s_%s"),
		Name, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

FString MakeObjectPath(const FString& Target)
{
	return FString::Printf(TEXT("%s.%s"), *Target,
		*FPackageName::GetLongPackageAssetName(Target));
}

FString MakeKey(const TCHAR* Name, const TCHAR* Type)
{
	const FString KeyType(Type);
	const TCHAR* Properties = KeyType.EndsWith(TEXT("_Bool"))
		? TEXT("{\"bDefaultValue\":false}")
		: KeyType.EndsWith(TEXT("_Float"))
			? TEXT("{\"DefaultValue\":0.0}")
			: TEXT("{\"DefaultValue\":0}");
	return FString::Printf(
		TEXT("{\"Category\":\"Rollback\",\"Description\":\"%s\",\"")
		TEXT("KeyTypeClass\":{\"Kind\":\"ClassRef\",\"Path\":\"%s\"},")
		TEXT("\"KeyTypeProperties\":%s,\"Name\":\"%s\",\"bInstanceSynced\":false}"),
		Name, Type, Properties, Name);
}

FString MakeDocument(
	const FString& Target,
	const FString& Parent,
	const TArray<FString>& Keys,
	const TCHAR* Action = TEXT("Update"))
{
	return FString::Printf(
		TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",")
		TEXT("\"Class\":\"/Script/AIModule.BlackboardData\",\"Action\":\"%s\",")
		TEXT("\"Definitions\":{},\"Properties\":{},\"Body\":{\"Parent\":%s,\"Keys\":[%s]}}"),
		*Target, Action,
		Parent.IsEmpty() ? TEXT("null") : *FString::Printf(TEXT("{\"Kind\":\"AssetRef\",\"Path\":\"%s\"}"), *Parent),
		*FString::Join(Keys, TEXT(",")));
}

class FFixture final
{
public:
	explicit FFixture(const FString& InTarget)
		: Target(InTarget)
		, ObjectPath(MakeObjectPath(InTarget))
		, SidecarPath(FPaths::Combine(FPaths::ProjectContentDir(),
			TEXT("AssetDocumentG3BlackboardData"),
			FPackageName::GetLongPackageAssetName(InTarget) + TEXT(".assetdoc.json")))
		, PackagePath(FPackageName::LongPackageNameToFilename(InTarget,
			FPackageName::GetAssetPackageExtension()))
	{
	}

	~FFixture()
	{
		IFileManager::Get().Delete(*SidecarPath, false, true);
		IFileManager::Get().Delete(*PackagePath, false, true);
	}

	bool Write(FAutomationTestBase& Test, const FString& Document) const
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(SidecarPath), true);
		return Test.TestTrue(TEXT("writes rollback sidecar"),
			FFileHelper::SaveStringToFile(Document, *SidecarPath));
	}

	FAssetDocumentResult Apply(FAssetDocumentService& Service) const
	{
		FAssetDocumentApplyFileRequest Request;
		Request.FilePath = SidecarPath;
		Request.bSaveAsset = true;
		return Service.ApplyFile(Request);
	}

	bool ApplySucceeds(
		FAutomationTestBase& Test,
		FAssetDocumentService& Service,
		const FString& Context) const
	{
		const FAssetDocumentResult Result = Apply(Service);
		if (Result.IsSuccess())
		{
			return true;
		}
		for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
		{
			Test.AddError(FString::Printf(
				TEXT("%s [%s] %s: %s"),
				*Context,
				*Diagnostic.Path,
				*Diagnostic.Code,
				*Diagnostic.Message));
		}
		return Test.TestTrue(*Context, false);
	}

	UObject* Load() const
	{
		return FSoftObjectPath(ObjectPath).TryLoad();
	}

	FAssetDocumentResult Extract(FAssetDocumentService& Service) const
	{
		FAssetDocumentExtractRequest Request;
		Request.AssetPath = ObjectPath;
		return Service.Extract(Request);
	}
	FString Target;
	FString ObjectPath;
	FString SidecarPath;
	FString PackagePath;
};

struct FState
{
	TSharedPtr<FJsonValue> CanonicalBody;
	bool bDirty = false;
	bool bValid = false;
};

bool JsonValuesEqual(
	const TSharedPtr<FJsonValue>& Left,
	const TSharedPtr<FJsonValue>& Right)
{
	if (!Left.IsValid() || !Right.IsValid() || Left->Type != Right->Type)
	{
		return false;
	}
	if (Left->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject>* LeftObject = nullptr;
		const TSharedPtr<FJsonObject>* RightObject = nullptr;
		if (!Left->TryGetObject(LeftObject) || !Right->TryGetObject(RightObject)
			|| LeftObject == nullptr || RightObject == nullptr
			|| (*LeftObject)->Values.Num() != (*RightObject)->Values.Num())
		{
			return false;
		}
		for (const auto& Pair : (*LeftObject)->Values)
		{
			const TSharedPtr<FJsonValue>* Other = (*RightObject)->Values.Find(Pair.Key);
			if (Other == nullptr || !JsonValuesEqual(Pair.Value, *Other))
			{
				return false;
			}
		}
		return true;
	}
	if (Left->Type == EJson::Array)
	{
		const TArray<TSharedPtr<FJsonValue>>* LeftArray = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* RightArray = nullptr;
		if (!Left->TryGetArray(LeftArray) || !Right->TryGetArray(RightArray)
			|| LeftArray == nullptr || RightArray == nullptr
			|| LeftArray->Num() != RightArray->Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < LeftArray->Num(); ++Index)
		{
			if (!JsonValuesEqual((*LeftArray)[Index], (*RightArray)[Index]))
			{
				return false;
			}
		}
		return true;
	}
	if (Left->Type == EJson::Null || Left->Type == EJson::None)
	{
		return true;
	}
	FString LeftScalar;
	FString RightScalar;
	return Left->TryGetString(LeftScalar)
		&& Right->TryGetString(RightScalar)
		&& LeftScalar == RightScalar;
}

FState Capture(FFixture& Fixture, FAssetDocumentService& Service)
{
	FState Result;
	UObject* const Asset = Fixture.Load();
	const FAssetDocumentResult Extracted = Fixture.Extract(Service);
	if (Asset == nullptr || !Extracted.IsSuccess() || !Extracted.Payload.IsValid())
	{
		return Result;
	}
	const TSharedPtr<FJsonValue>* Body = Extracted.Payload->Values.Find(TEXT("Body"));
	Result.bValid = Body != nullptr && Body->IsValid();
	if (Result.bValid)
	{
		Result.CanonicalBody = *Body;
	}
	Result.bDirty = Asset->GetOutermost()->IsDirty();
	return Result;
}

bool HasNoLegacyRegistration(FAutomationTestBase& Test)
{
	FString Source;
	const FString Path = FPaths::Combine(GetAssetDocumentPluginBaseDir(), TEXT("Source/AssetDocument/Private/AssetDocumentModule.cpp"));
	return Test.TestTrue(TEXT("reads the sole-route registration source"),
		FFileHelper::LoadFileToString(Source, *Path))
		&& Test.TestFalse(TEXT("rollback scope has no legacy Blackboard Profile"),
			Source.Contains(TEXT("FBlackboardDataAssetDocumentProfile")));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBlackboardDataAllPhaseRollbackTest,
	"UEAssetDoc.Python.BlackboardData.AllPhaseRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBlackboardDataAllPhaseRollbackTest::RunTest(
	const FString& Parameters)
{
	using namespace BlackboardDataAllPhaseRollbackTestsLocal;
	(void)Parameters;
	if (!HasNoLegacyRegistration(*this))
	{
		return false;
	}
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	const FString Int = TEXT("/Script/AIModule.BlackboardKeyType_Int");
	const FString Bool = TEXT("/Script/AIModule.BlackboardKeyType_Bool");
	const FString Float = TEXT("/Script/AIModule.BlackboardKeyType_Float");
	FFixture Parent(MakeTarget(TEXT("BB_G3RollbackParent")));
	FFixture Child(MakeTarget(TEXT("BB_G3Rollback")));
	if (!Parent.Write(*this, MakeDocument(Parent.Target, FString(), {MakeKey(TEXT("ParentKey"), *Int)}, TEXT("Create")))
		|| !TestTrue(TEXT("creates rollback Parent"), Parent.Apply(Service).IsSuccess())
		|| !Child.Write(*this, MakeDocument(Child.Target, FString(), {
			MakeKey(TEXT("First"), *Int), MakeKey(TEXT("Second"), *Bool)}, TEXT("Create")))
		|| !TestTrue(TEXT("creates rollback baseline"), Child.Apply(Service).IsSuccess()))
	{
		return false;
	}

	struct FRow
	{
		const TCHAR* Name;
		FString ParentPath;
		TArray<FString> Keys;
	};
	const TArray<FRow> Rows{
		{TEXT("Parent"), Parent.ObjectPath,
			{MakeKey(TEXT("First"), *Int), MakeKey(TEXT("Second"), *Bool)}},
		{TEXT("Remove"), FString(), {MakeKey(TEXT("First"), *Int)}},
		{TEXT("Update"), FString(), {
			MakeKey(TEXT("First"), *Int).Replace(
				TEXT("\"Description\":\"First\""),
				TEXT("\"Description\":\"updated First\"")),
			MakeKey(TEXT("Second"), *Bool)}},
		{TEXT("Replace"), FString(),
			{MakeKey(TEXT("First"), *Float), MakeKey(TEXT("Second"), *Bool)}},
		{TEXT("Add"), FString(), {MakeKey(TEXT("First"), *Int),
			MakeKey(TEXT("Second"), *Bool), MakeKey(TEXT("Added"), *Int)}},
		{TEXT("Reorder"), FString(),
			{MakeKey(TEXT("Second"), *Bool), MakeKey(TEXT("First"), *Int)}},
	};
	for (const FRow& Row : Rows)
	{
		const FState Before = Capture(Child, Service);
		if (!Child.Write(*this, MakeDocument(Child.Target, Row.ParentPath, Row.Keys)))
		{
			return false;
		}
		const FString StagingRoot = FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("AssetDocument/Runtime/Staging"));
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		const bool bCreatedStagingRoot = !PlatformFile.DirectoryExists(*StagingRoot);
		if (bCreatedStagingRoot && !PlatformFile.CreateDirectoryTree(*StagingRoot))
		{
			return false;
		}
		const bool bWasReadOnly = PlatformFile.IsReadOnly(*StagingRoot);
		if (!bWasReadOnly && !AssetDocumentTestDirectoryBlock::SetDirectoryReadOnly(PlatformFile, *StagingRoot, true))
		{
			return false;
		}
		const FAssetDocumentResult Failure = Child.Apply(Service);
		if (!bWasReadOnly)
		{
			AssetDocumentTestDirectoryBlock::SetDirectoryReadOnly(PlatformFile, *StagingRoot, false);
		}
		if (bCreatedStagingRoot)
		{
			IFileManager::Get().DeleteDirectory(*StagingRoot, false, false);
		}
		const FState After = Capture(Child, Service);
		TestFalse(*FString::Printf(TEXT("%s Atomic persistence seam fails ApplyFile"), Row.Name), Failure.IsSuccess());
		TestTrue(*FString::Printf(TEXT("%s restores the complete canonical preimage"), Row.Name),
			Before.bValid
				&& After.bValid
			&& JsonValuesEqual(Before.CanonicalBody, After.CanonicalBody));
		TestTrue(*FString::Printf(TEXT("%s restores dirty-last state"), Row.Name),
			Before.bDirty == After.bDirty);
		Child.Write(*this, MakeDocument(Child.Target, FString(), {
			MakeKey(TEXT("First"), *Int), MakeKey(TEXT("Second"), *Bool)},
			TEXT("Create")));
	}
	return true;
}

#endif
