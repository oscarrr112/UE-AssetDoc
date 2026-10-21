// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// End-to-end ApplyFile/Extract tests for the generic factory lifecycle through
// the production Service: one apply creates and populates a new asset of any
// factory-creatable class, and a failure after creation leaves nothing behind.

#include "AssetDocumentModule.h"
#include "AssetDocumentTestDirectoryBlock.h"
#include "AssetDocumentService.h"
#include "AssetDocumentEditorFactoryFailureTestTypes.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorGenericFactoryApplyTests
{
struct FTarget
{
	FString AssetName;
	FString PackageName;
	FString ObjectPath;
	FString PackageFile;
	FString SidecarPath;

	explicit FTarget(const TCHAR* Prefix)
		: AssetName(FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits)))
	{
		PackageName = FString(TEXT("/Game/AssetDocumentGenericFactory/")) + AssetName;
		ObjectPath = PackageName + TEXT(".") + AssetName;
		PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		SidecarPath = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("AssetDocumentGenericFactory"),
			AssetName + TEXT(".assetdoc.json"));
	}

	FString Document(const TCHAR* Class, const TCHAR* Action, const FString& Properties) const
	{
		return FString::Printf(
			TEXT("{\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\",\"Target\":\"%s\",\"Class\":\"%s\",")
			TEXT("\"Action\":\"%s\",\"Properties\":%s}"),
			*PackageName, Class, Action, *Properties);
	}

	// Saved packages are unloaded and their file removed (then rescanned);
	// unsaved ones are deleted in-editor, as the asset lifecycle tests do.
	void Cleanup() const
	{
		UObject* Asset = FSoftObjectPath(ObjectPath).ResolveObject();
		if (IFileManager::Get().FileExists(*PackageFile))
		{
			if (Asset != nullptr)
			{
				UPackageTools::UnloadPackages({Asset->GetOutermost()});
				CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			}
			IFileManager::Get().Delete(*PackageFile, false, true);
			FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({PackageFile});
		}
		else if (Asset != nullptr)
		{
			ObjectTools::DeleteObjects({Asset}, false, ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
		}
		IFileManager::Get().Delete(*SidecarPath, false, true);
	}
};

// Makes package staging fail after Apply so the transaction must roll the
// created asset back (same mechanism as GenericRuntime.AllPhaseRollback).
class FStagingBlocker
{
public:
	~FStagingBlocker() { Restore(); }

	bool Block()
	{
		IPlatformFile& Files = FPlatformFileManager::Get().GetPlatformFile();
		Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AssetDocument/Runtime/Staging"));
		bCreated = !Files.DirectoryExists(*Root);
		if ((bCreated && !Files.CreateDirectoryTree(*Root)) || Files.IsReadOnly(*Root))
		{
			return false;
		}
		bBlocked = AssetDocumentTestDirectoryBlock::SetDirectoryReadOnly(Files, *Root, true);
		return bBlocked;
	}

	void Restore()
	{
		if (!bBlocked)
		{
			return;
		}
		IPlatformFile& Files = FPlatformFileManager::Get().GetPlatformFile();
		AssetDocumentTestDirectoryBlock::SetDirectoryReadOnly(Files, *Root, false);
		if (bCreated)
		{
			IFileManager::Get().DeleteDirectory(*Root, false, false);
		}
		bBlocked = false;
	}

private:
	FString Root;
	bool bCreated = false;
	bool bBlocked = false;
};

FAssetDocumentResult ApplyDocument(FAutomationTestBase& Test, const FTarget& Target, const FString& Json,
	const TCHAR* Stage)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target.SidecarPath), true);
	Test.TestTrue(*FString::Printf(TEXT("%s writes sidecar"), Stage), FFileHelper::SaveStringToFile(Json, *Target.SidecarPath));
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Target.SidecarPath;
	Request.bSaveAsset = true;
	FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
	Test.AddInfo(FString::Printf(TEXT("%s: %s"), Stage, *Result.Message));
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Test.AddInfo(FString::Printf(TEXT("%s: [%s] %s %s"), Stage, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
	}
	return Result;
}

FString Serialize(const TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	if (Object.IsValid())
	{
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}
	return Text;
}

// Extract (authored diff) must reproduce exactly the authored Properties.
void TestExtractEquals(FAutomationTestBase& Test, const FTarget& Target, const FString& ExpectedProperties,
	const TCHAR* Stage)
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = Target.ObjectPath;
	const FAssetDocumentResult Extracted = FAssetDocumentModule::Get().GetService().Extract(Request);
	const TSharedPtr<FJsonObject>* Properties = nullptr;
	if (!Test.TestTrue(*FString::Printf(TEXT("%s extract succeeds"), Stage), Extracted.IsSuccess() && Extracted.Payload.IsValid())
		|| !Test.TestTrue(*FString::Printf(TEXT("%s extract has Properties"), Stage),
			Extracted.Payload->TryGetObjectField(TEXT("Properties"), Properties)))
	{
		Test.AddInfo(Serialize(Extracted.Payload));
		return;
	}
	// Extract also reports unauthored writable defaults; every authored value must round-trip.
	TSharedPtr<FJsonObject> Expected;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ExpectedProperties), Expected);
	if (!Test.TestTrue(TEXT("expected Properties parse"), Expected.IsValid()))
	{
		return;
	}
	for (const auto& Field : Expected->Values)
	{
		const TSharedPtr<FJsonValue> Actual = (*Properties)->TryGetField(Field.Key);
		Test.TestTrue(*FString::Printf(TEXT("%s extract equals authored %s"), Stage, *Field.Key),
			Actual.IsValid() && FJsonValue::CompareEqual(*Actual, *Field.Value));
	}
}

void TestNoLeftovers(FAutomationTestBase& Test, const FTarget& Target, const TCHAR* Stage)
{
	Test.TestNull(*FString::Printf(TEXT("%s: no object"), Stage), FSoftObjectPath(Target.ObjectPath).ResolveObject());
	const UPackage* Package = FindPackage(nullptr, *Target.PackageName);
	Test.TestTrue(*FString::Printf(TEXT("%s: no live package"), Stage), Package == nullptr || !Package->IsDirty());
	Test.TestFalse(*FString::Printf(TEXT("%s: no registry entry"), Stage),
		FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(Target.ObjectPath)).IsValid());
	Test.TestFalse(*FString::Printf(TEXT("%s: no package file"), Stage), IFileManager::Get().FileExists(*Target.PackageFile));
}

// One create+populate apply, extract, update, extract, then an injected
// post-create failure on a fresh target that must leave no residue.
void RunLifecycle(FAutomationTestBase& Test, const TCHAR* Prefix, const TCHAR* Class,
	const FString& CreateProperties, const FString& UpdateProperties)
{
	const FTarget Target(Prefix);
	ON_SCOPE_EXIT { Target.Cleanup(); };
	const FAssetDocumentResult Created =
		ApplyDocument(Test, Target, Target.Document(Class, TEXT("Create"), CreateProperties), TEXT("Create"));
	if (!Test.TestTrue(FString::Printf(TEXT("%s is created and populated in one apply"), Class), Created.IsSuccess()))
	{
		return;
	}
	const UObject* Asset = FSoftObjectPath(Target.ObjectPath).ResolveObject();
	Test.TestTrue(TEXT("created asset has the exact class"), Asset != nullptr && Asset->GetClass()->GetPathName() == Class);
	Test.TestTrue(TEXT("created asset is saved"), IFileManager::Get().FileExists(*Target.PackageFile));
	TestExtractEquals(Test, Target, CreateProperties, TEXT("after create"));

	if (!UpdateProperties.IsEmpty())
	{
		const FAssetDocumentResult Updated =
			ApplyDocument(Test, Target, Target.Document(Class, TEXT("Update"), UpdateProperties), TEXT("Update"));
		Test.TestTrue(FString::Printf(TEXT("%s update succeeds"), Class), Updated.IsSuccess());
		Test.TestTrue(TEXT("update keeps the object identity"), FSoftObjectPath(Target.ObjectPath).ResolveObject() == Asset);
		TestExtractEquals(Test, Target, UpdateProperties, TEXT("after update"));
	}

	const FTarget Failed(Prefix);
	ON_SCOPE_EXIT { Failed.Cleanup(); };
	FStagingBlocker Blocker;
	if (!Test.TestTrue(TEXT("staging root can be blocked"), Blocker.Block()))
	{
		return;
	}
	const FAssetDocumentResult Rejected =
		ApplyDocument(Test, Failed, Failed.Document(Class, TEXT("Create"), CreateProperties), TEXT("InjectedStageFailure"));
	Blocker.Restore();
	Test.TestFalse(TEXT("post-create stage failure fails the apply"), Rejected.IsSuccess());
	Test.TestTrue(TEXT("failure is the injected staging failure"), Rejected.Diagnostics.ContainsByPredicate(
		[](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == TEXT("Persistence.StagePackageFailed"); }));
	TestNoLeftovers(Test, Failed, TEXT("after injected failure"));
}

void RunInvalid(FAutomationTestBase& Test, const TCHAR* Prefix, const TCHAR* Class, const FString& Properties,
	const TCHAR* ExpectedCode, bool bExpectNoPackage = false, const TCHAR* ExpectedMessageText = nullptr,
	bool bMessageLeadsWithCode = true)
{
	const FTarget Target(Prefix);
	ON_SCOPE_EXIT { Target.Cleanup(); };
	const FAssetDocumentResult Result =
		ApplyDocument(Test, Target, Target.Document(Class, TEXT("Create"), Properties), ExpectedCode);
	Test.TestFalse(FString::Printf(TEXT("%s is rejected"), ExpectedCode), Result.IsSuccess());
	Test.TestTrue(FString::Printf(TEXT("diagnostic %s"), ExpectedCode), Result.Diagnostics.ContainsByPredicate(
		[ExpectedCode](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == ExpectedCode; }));
	// The Message must say what went wrong, not a generic runtime text: on the Python route
	// it leads with the first diagnostic, "<Code>: <Message> (at <Path>)". Failures the Service
	// reports before the route runs (class resolution) already carry a plain specific message.
	if (bMessageLeadsWithCode)
	{
		Test.TestTrue(*FString::Printf(TEXT("%s message leads with the diagnostic code (was '%s')"), ExpectedCode, *Result.Message),
			Result.Message.StartsWith(FString(ExpectedCode) + TEXT(": ")));
	}
	Test.TestFalse(*FString::Printf(TEXT("%s message is not the generic runtime text"), ExpectedCode),
		Result.Message.Contains(TEXT("Generic AssetDocument runtime failed")));
	if (ExpectedMessageText != nullptr)
	{
		Test.TestTrue(*FString::Printf(TEXT("%s message carries '%s' (was '%s')"), ExpectedCode, ExpectedMessageText, *Result.Message),
			Result.Message.Contains(ExpectedMessageText));
	}
	TestNoLeftovers(Test, Target, ExpectedCode);
	if (bExpectNoPackage)
	{
		Test.TestNull(*FString::Printf(TEXT("%s: no orphan package"), ExpectedCode), FindPackage(nullptr, *Target.PackageName));
	}
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericFactoryDataAssetApplyTest,
	"UEAssetDoc.GenericRuntime.GenericFactory.DataAssetApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericFactoryDataAssetApplyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericFactoryApplyTests;
	RunLifecycle(*this, TEXT("DA_GenericFactory"), TEXT("/Script/AssetDocumentTests.TestDataAsset"),
		TEXT("{\"TestInt\":3,\"TestString\":\"created\"}"), TEXT("{\"TestInt\":4,\"TestString\":\"updated\"}"));
	RunInvalid(*this, TEXT("DA_GenericFactoryInvalid"), TEXT("/Script/AssetDocumentTests.TestDataAsset"),
		TEXT("{\"TestInt\":\"not an int\"}"), TEXT("PropertyPreflight.DecodeFailed"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericFactoryCurveFloatApplyTest,
	"UEAssetDoc.GenericRuntime.GenericFactory.CurveFloatApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericFactoryCurveFloatApplyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericFactoryApplyTests;
	// UCurveFloatFactory is selected generically. CurveFloat key authoring
	// (Profile-declared FloatCurve) is covered by the CurveFloat apply scenarios.
	RunLifecycle(*this, TEXT("Curve_GenericFactory"), TEXT("/Script/Engine.CurveFloat"), TEXT("{}"), FString());
	// A rejected CurveFloat document says what is wrong in Result.Message.
	RunInvalid(*this, TEXT("Curve_GenericFactoryInvalid"), TEXT("/Script/Engine.CurveFloat"),
		TEXT("{\"NoSuchProperty\":1}"), TEXT("Generic.UnknownProperty"), false,
		TEXT("property is not admitted (at /Properties/NoSuchProperty)"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericFactoryRejectionApplyTest,
	"UEAssetDoc.GenericRuntime.GenericFactory.RejectionApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericFactoryRejectionApplyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericFactoryApplyTests;
	RunInvalid(*this, TEXT("Curve_Abstract"), TEXT("/Script/Engine.CurveBase"), TEXT("{}"), TEXT("FactoryCreate.AbstractClass"),
		false, TEXT("is abstract"), false);
	RunInvalid(*this, TEXT("SM_NoFactory"), TEXT("/Script/Engine.StaticMesh"), TEXT("{}"), TEXT("FactoryCreate.NoFactory"));
	// AssetTools creates the package before FactoryCreateNew returns null; the
	// failed creation must not leave that package behind (TestNoLeftovers).
	RunInvalid(*this, TEXT("FactoryReturnsNull"),
		*UAssetDocumentEditorFactoryFailureTestAsset::StaticClass()->GetPathName(), TEXT("{}"),
		TEXT("FactoryCreate.IncompleteCreation"), true);
	return true;
}
#endif
