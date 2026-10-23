// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "AssetDocumentPluginPaths.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
FString MakeTarget(const TCHAR* Stem)
{
	return FString::Printf(
		TEXT("/Game/AssetDocumentBlackboardDataTests/%s_%s"),
		Stem,
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

FString MakeObjectPath(const FString& Target)
{
	return FString::Printf(
		TEXT("%s.%s"), *Target, *FPackageName::GetLongPackageAssetName(Target));
}

FString MakeKey(
	const TCHAR* Name,
	const TCHAR* KeyType,
	const TCHAR* Description)
{
	const FString Type(KeyType);
	const TCHAR* Properties = Type.EndsWith(TEXT("_Bool"))
		? TEXT("{\"bDefaultValue\":false}")
		: Type.EndsWith(TEXT("_Float"))
			? TEXT("{\"DefaultValue\":0.0}")
			: TEXT("{\"DefaultValue\":0}");
	return FString::Printf(
		TEXT("{\"Category\":\"Lifecycle\",\"Description\":\"%s\",\"")
		TEXT("KeyTypeClass\":{\"Kind\":\"ClassRef\",\"Path\":\"%s\"},")
		TEXT("\"KeyTypeProperties\":%s,\"Name\":\"%s\",\"bInstanceSynced\":false}"),
		Description,
		KeyType,
		Properties,
		Name);
}

FString MakeDocument(
	const FString& Target,
	const FString& ParentObjectPath,
	const TArray<FString>& Keys,
	const TCHAR* Action = TEXT("Update"))
{
	const FString Parent = ParentObjectPath.IsEmpty()
		? TEXT("null")
		: FString::Printf(
			TEXT("{\"Kind\":\"AssetRef\",\"Path\":\"%s\"}"),
			*ParentObjectPath);
	return FString::Printf(
		TEXT("{\"SchemaVersion\":1,\"")
		TEXT("Target\":\"%s\",\"Class\":\"/Script/AIModule.BlackboardData\",")
		TEXT("\"Action\":\"%s\",\"Definitions\":{},\"Properties\":{},")
		TEXT("\"Body\":{\"Parent\":%s,\"Keys\":[%s]}}"),
		*Target,
		Action,
		*Parent,
		*FString::Join(Keys, TEXT(",")));
}

bool HasNoLegacyBlackboardRegistration(FAutomationTestBase& Test)
{
	const FString ModulePath = FPaths::Combine(GetAssetDocumentPluginBaseDir(), TEXT("Source/AssetDocument/Private/AssetDocumentModule.cpp"));
	FString Source;
	return Test.TestTrue(
		TEXT("sole Python route removed the C++ Blackboard registration"),
		FFileHelper::LoadFileToString(Source, *ModulePath))
		&& Test.TestFalse(
			TEXT("sole Python route has no legacy Blackboard Profile symbol"),
			Source.Contains(TEXT("FBlackboardDataAssetDocumentProfile")));
}

class FScopedFixture final
{
public:
	explicit FScopedFixture(const FString& InTarget)
		: Target(InTarget)
		, ObjectPath(MakeObjectPath(InTarget))
		, SidecarPath(FPaths::Combine(
			FPaths::ProjectContentDir(),
			TEXT("AssetDocumentBlackboardDataTests"),
			FPackageName::GetLongPackageAssetName(InTarget) + TEXT(".assetdoc.json")))
		, PackagePath(FPackageName::LongPackageNameToFilename(
			InTarget,
			FPackageName::GetAssetPackageExtension()))
	{
	}

	~FScopedFixture()
	{
		IFileManager::Get().Delete(*SidecarPath, false, true);
		IFileManager::Get().Delete(*PackagePath, false, true);
	}

	bool Write(FAutomationTestBase& Test, const FString& Document) const
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(SidecarPath), true);
		return Test.TestTrue(
			TEXT("writes canonical Blackboard ApplyFile sidecar"),
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
		const TCHAR* Context) const
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
				Context,
				*Diagnostic.Path,
				*Diagnostic.Code,
				*Diagnostic.Message));
		}
		return Test.TestTrue(Context, false);
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

bool HasCanonicalState(
	const FAssetDocumentResult& Result,
	const FString& ExpectedParentPath,
	const TArray<FString>& ExpectedNames,
	const TArray<FString>& ExpectedClasses,
	const TArray<FString>& ExpectedDescriptions = {})
{
	if (!Result.IsSuccess() || !Result.Payload.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonValue>* BodyValue = Result.Payload->Values.Find(TEXT("Body"));
	if (BodyValue == nullptr || !BodyValue->IsValid()
		|| (*BodyValue)->Type != EJson::Object)
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* BodyPtr = nullptr;
	if (!(*BodyValue)->TryGetObject(BodyPtr) || BodyPtr == nullptr)
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Body = *BodyPtr;
	if (!ExpectedParentPath.IsEmpty())
	{
		const TSharedPtr<FJsonValue>* ParentValue = Body->Values.Find(TEXT("Parent"));
		if (ParentValue == nullptr || !ParentValue->IsValid()
			|| (*ParentValue)->Type != EJson::Object)
		{
			return false;
		}
		const TSharedPtr<FJsonObject>* ParentPtr = nullptr;
		FString ParentPath;
		if (!(*ParentValue)->TryGetObject(ParentPtr) || ParentPtr == nullptr
			|| !(*ParentPtr)->Values.FindRef(TEXT("Path"))->TryGetString(ParentPath)
			|| ParentPath != ExpectedParentPath)
		{
			return false;
		}
	}
	const TSharedPtr<FJsonValue>* KeysValue = Body->Values.Find(TEXT("Keys"));
	if (KeysValue == nullptr || !KeysValue->IsValid()
		|| (*KeysValue)->Type != EJson::Array)
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* KeysPtr = nullptr;
	if (!(*KeysValue)->TryGetArray(KeysPtr) || KeysPtr == nullptr
		|| KeysPtr->Num() != ExpectedNames.Num())
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>& Keys = *KeysPtr;
	for (int32 Index = 0; Index < Keys.Num(); ++Index)
	{
		if (!Keys[Index].IsValid() || Keys[Index]->Type != EJson::Object)
		{
			return false;
		}
		const TSharedPtr<FJsonObject>* KeyPtr = nullptr;
		if (!Keys[Index]->TryGetObject(KeyPtr) || KeyPtr == nullptr)
		{
			return false;
		}
		const TSharedPtr<FJsonObject> Key = *KeyPtr;
		const TSharedPtr<FJsonValue> KeyTypeValue =
			Key->Values.FindRef(TEXT("KeyTypeClass"));
		const TSharedPtr<FJsonObject>* KeyTypePtr = nullptr;
		FString Name;
		FString KeyTypePath;
		FString Description;
		if (!KeyTypeValue.IsValid() || KeyTypeValue->Type != EJson::Object
			|| !Key->Values.FindRef(TEXT("Name"))->TryGetString(Name)
			|| !KeyTypeValue->TryGetObject(KeyTypePtr) || KeyTypePtr == nullptr
			|| !(*KeyTypePtr)->Values.FindRef(TEXT("Path"))->TryGetString(KeyTypePath)
			|| Name != ExpectedNames[Index]
			|| KeyTypePath != ExpectedClasses[Index]
			|| (!ExpectedDescriptions.IsEmpty()
				&& (ExpectedDescriptions.Num() != Keys.Num()
					|| !Key->Values.FindRef(TEXT("Description"))->TryGetString(Description)
					|| Description != ExpectedDescriptions[Index])))
		{
			return false;
		}
	}
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBlackboardDataFullLifecycleTest,
	"UEAssetDoc.Python.BlackboardData.FullLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBlackboardDataFullLifecycleTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	if (!HasNoLegacyBlackboardRegistration(*this))
	{
		return false;
	}
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	FScopedFixture Parent(MakeTarget(TEXT("BB_LifecycleParent")));
	FScopedFixture Child(MakeTarget(TEXT("BB_Lifecycle")));
	const FString Bool = TEXT("/Script/AIModule.BlackboardKeyType_Bool");
	const FString Int = TEXT("/Script/AIModule.BlackboardKeyType_Int");
	const FString Float = TEXT("/Script/AIModule.BlackboardKeyType_Float");
	if (!Parent.Write(*this, MakeDocument(Parent.Target, FString(), {
		MakeKey(TEXT("ParentKey"), *Int, TEXT("parent baseline"))}, TEXT("Create")))
		|| !Parent.ApplySucceeds(
			*this,
			Service,
			TEXT("Parent fixture applies through production ApplyFile")))
	{
		return false;
	}
	if (!Child.Write(*this, MakeDocument(Child.Target, Parent.ObjectPath, {
		MakeKey(TEXT("First"), *Int, TEXT("initial first")),
		MakeKey(TEXT("Second"), *Bool, TEXT("initial second"))}, TEXT("Create")))
		|| !Child.ApplySucceeds(
			*this,
			Service,
			TEXT("Parent lifecycle applies through production ApplyFile")))
	{
		return false;
	}
	if (!TestTrue(TEXT("Parent resolves after production ApplyFile"),
		HasCanonicalState(
			Child.Extract(Service),
			Parent.ObjectPath,
			{TEXT("First"), TEXT("Second")},
			{Int, Bool})))
	{
		return false;
	}

	if (!Child.Write(*this, MakeDocument(Child.Target, Parent.ObjectPath, {
		MakeKey(TEXT("First"), *Int, TEXT("updated existing entry")),
		MakeKey(TEXT("Second"), *Bool, TEXT("initial second"))}))
		|| !Child.ApplySucceeds(
			*this,
			Service,
			TEXT("UpdateExisting lifecycle applies")))
	{
		return false;
	}
	TestTrue(TEXT("UpdateExisting changes only the exact entry metadata"),
		HasCanonicalState(
			Child.Extract(Service),
			Parent.ObjectPath,
			{TEXT("First"), TEXT("Second")},
			{Int, Bool},
			{TEXT("updated existing entry"), TEXT("initial second")}));

	if (!Child.Write(*this, MakeDocument(Child.Target, Parent.ObjectPath, {
		MakeKey(TEXT("First"), *Float, TEXT("replacement class")),
		MakeKey(TEXT("Second"), *Bool, TEXT("initial second")),
		MakeKey(TEXT("Added"), *Int, TEXT("new key"))}))
		|| !Child.ApplySucceeds(
			*this,
			Service,
			TEXT("AddReplace lifecycle applies")))
	{
		return false;
	}
	TestTrue(TEXT("AddReplace preserves ordered key classes"),
		HasCanonicalState(
			Child.Extract(Service),
			Parent.ObjectPath,
			{TEXT("First"), TEXT("Second"), TEXT("Added")},
			{Float, Bool, Int}));

	if (!Child.Write(*this, MakeDocument(Child.Target, Parent.ObjectPath, {
		MakeKey(TEXT("Added"), *Int, TEXT("new key")),
		MakeKey(TEXT("First"), *Float, TEXT("replacement class"))}))
		|| !Child.ApplySucceeds(
			*this,
			Service,
			TEXT("RemoveReorder lifecycle applies")))
	{
		return false;
	}
	TestTrue(TEXT("RemoveReorder preserves final Parent and authored order"),
		HasCanonicalState(
			Child.Extract(Service),
			Parent.ObjectPath,
			{TEXT("Added"), TEXT("First")},
			{Int, Float}));
	TestTrue(TEXT("ProductionHttpApplyFileRestart handoff leaves a durable package"),
		IFileManager::Get().FileExists(*Child.PackagePath));
	return true;
}

#endif
