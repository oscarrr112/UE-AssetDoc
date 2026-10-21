// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Engine and plugin content through the production Service:
//   - ReadOnlyExtract: engine assets of every main profile extract as documents (read_only,
//     Target as given), the read neither dirties nor leaves a package, and the document
//     re-targeted to /Game applies as Create and extracts equal.
//   - PluginDataTable: the same for an asset under a plugin mount.
//   - ReadOnlyRefusals: validate, diff, apply and template of an engine Target lead with
//     Meta.ReadOnlyTarget.
//   - Duplicate: the Content Browser's Duplicate into /Game (Skeleton then a slot and a montage
//     on the copy, Material, AnimSequence, DataTable, Blueprint), saved.
//   - DuplicateRefusals: target exists, target outside /Game, missing source; nothing left.
//   - ReadOnlyInspect: inspect of engine/plugin assets goes through the profile (Material pins,
//     a User Defined Enum's enumerators), read-only and without dirtying; a path that names no
//     asset resolves to the only asset of that name under the mount, else lists candidates.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"

#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "Engine/DataTable.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEngineAssetsTests
{
const TCHAR* Folder = TEXT("AssetDocumentEngineAssets");
const TCHAR* TutorialDir = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/");
const TCHAR* EngineSkeleton = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton");
const TCHAR* EngineIdle = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle");
const TCHAR* EngineBlendSpace = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/NewBlendSpace1D");
const TCHAR* EngineMaterial = TEXT("/Engine/EngineMaterials/WorldGridMaterial");
const TCHAR* EngineBlueprint = TEXT("/Engine/EngineSky/BP_Sky_Sphere");
const TCHAR* EngineDamageType = TEXT("/Engine/EngineDamageTypes/DmgTypeBP_Environmental");
const TCHAR* EngineBlueprintClass = TEXT("/Engine/Tutorial/BlueprintTutorials/TutorialAssets/Tutorial_BP_Class");
const TCHAR* EngineMoveGoal = TEXT("/Engine/FunctionalTesting/Blueprints/AITesting_MoveGoal");
const TCHAR* TestRow = TEXT("/Script/AssetDocumentTests.AssetDocumentDataTestRow");

FString ObjectPathOf(const FString& PackageName)
{
	return PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
}

void ReleaseUndoHistory()
{
	if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting && !GEditor->IsTransactionActive()
		&& GEditor->Trans->GetQueueLength() > 0)
	{
		GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentEngineAssetsTests", "Cleanup", "AssetDocument engine asset test cleanup"));
	}
}

// Forget an in-memory, never-saved package (the plugin-mount fixture and anything a failed
// operation could have left).
void DiscardInMemoryPackage(const FString& PackageName)
{
	UPackage* Package = FindPackage(nullptr, *PackageName);
	if (Package == nullptr)
	{
		return;
	}
	TArray<UObject*> Objects;
	GetObjectsWithPackage(Package, Objects);
	for (UObject* Object : Objects)
	{
		if (Object->HasAnyFlags(RF_Standalone | RF_Public) && Object->IsAsset())
		{
			FAssetRegistryModule::AssetDeleted(Object);
		}
		Object->ClearFlags(RF_Standalone | RF_Public);
		Object->MarkAsGarbage();
	}
	Package->SetDirtyFlag(false);
	Package->ClearFlags(RF_Standalone | RF_Public);
	Package->MarkAsGarbage();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
}

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
		PackageName = FString::Printf(TEXT("/Game/%s/%s"), Folder, *AssetName);
		ObjectPath = ObjectPathOf(PackageName);
		PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		SidecarPath = FPaths::Combine(FPaths::ProjectContentDir(), Folder, AssetName + TEXT(".assetdoc.json"));
	}

	UObject* Get() const { return FSoftObjectPath(ObjectPath).ResolveObject(); }

	template <typename T>
	T* Get() const { return Cast<T>(Get()); }

	bool ExistsAnywhere() const
	{
		return Get() != nullptr || IFileManager::Get().FileExists(*PackageFile)
			|| FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid();
	}

	void Cleanup() const
	{
		ReleaseUndoHistory();
		if (UObject* Asset = Get())
		{
			if (IFileManager::Get().FileExists(*PackageFile))
			{
				UPackageTools::UnloadPackages({Asset->GetOutermost()});
			}
			else
			{
				Asset->GetOutermost()->ClearDirtyFlag();
				ObjectTools::DeleteObjects({Asset}, false, ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
			}
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		if (IFileManager::Get().FileExists(*PackageFile))
		{
			IFileManager::Get().Delete(*PackageFile, false, true);
			FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({PackageFile});
		}
		DiscardInMemoryPackage(PackageName);
		IFileManager::Get().Delete(*SidecarPath, false, true);
	}
};

TSharedPtr<FJsonObject> Parse(const FString& Text)
{
	TSharedPtr<FJsonObject> Object;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object);
	return Object;
}

FString Serialize(const TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	if (Object.IsValid())
	{
		const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}
	return Text;
}

void Report(FAutomationTestBase& Test, const FString& Stage, const FAssetDocumentResult& Result)
{
	Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *Result.Message));
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Test.AddInfo(FString::Printf(TEXT("%s: [%s] %s %s"), *Stage, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
	}
}

FAssetDocumentService& Service()
{
	return FAssetDocumentModule::Get().GetService();
}

FAssetDocumentResult Extract(FAutomationTestBase& Test, const FString& Path, const FString& Stage)
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = Path;
	FAssetDocumentResult Result = Service().Extract(Request);
	if (!Result.IsSuccess())
	{
		Report(Test, Stage, Result);
	}
	return Result;
}

FAssetDocumentResult Apply(FAutomationTestBase& Test, const FTarget& Target, const FString& Json, const FString& Stage)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target.SidecarPath), true);
	FFileHelper::SaveStringToFile(Json, *Target.SidecarPath);
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Target.SidecarPath;
	Request.bSaveAsset = true;
	FAssetDocumentResult Result = Service().ApplyFile(Request);
	Report(Test, Stage, Result);
	return Result;
}

// The loaded packages under /Game, sorted.
TArray<FString> GamePackageNames()
{
	TArray<FString> Names;
	for (TObjectIterator<UPackage> It; It; ++It)
	{
		const FString Name = It->GetName();
		if (Name.StartsWith(TEXT("/Game/")) && IsValid(*It))
		{
			Names.Add(Name);
		}
	}
	Names.Sort();
	return Names;
}

// A payload as Retarget writes it (numbers re-serialized the same way: 0 and 0.0 alike).
FString Normalized(const TSharedPtr<FJsonObject>& Payload)
{
	return Serialize(Parse(Serialize(Payload)));
}

FAssetDocumentResult Duplicate(FAutomationTestBase& Test, const FString& Source, const FString& Target, const FString& Stage)
{
	FAssetDocumentDuplicateRequest Request;
	Request.Source = Source;
	Request.Target = Target;
	FAssetDocumentResult Result = Service().Duplicate(Request);
	Report(Test, Stage, Result);
	return Result;
}

FString FirstCode(const FAssetDocumentResult& Result)
{
	return Result.Diagnostics.IsEmpty() ? FString() : Result.Diagnostics[0].Code;
}

// The document an AI makes from an extract to create a copy: the source's own path (Target and
// any self-reference, such as a Blueprint's generated class) becomes the new /Game path.
FString Retarget(const TSharedPtr<FJsonObject>& Extracted, const FString& SourcePackage, const FString& TargetPackage, const TCHAR* Action)
{
	TSharedPtr<FJsonObject> Document = Parse(Serialize(Extracted));
	Document->SetStringField(TEXT("Action"), Action);
	const FString SourceName = FPackageName::GetLongPackageAssetName(SourcePackage);
	const FString TargetName = FPackageName::GetLongPackageAssetName(TargetPackage);
	return Serialize(Document)
		.Replace(*(SourcePackage + TEXT(".") + SourceName), *(TargetPackage + TEXT(".") + TargetName), ESearchCase::CaseSensitive)
		.Replace(*(SourcePackage + TEXT("\"")), *(TargetPackage + TEXT("\"")), ESearchCase::CaseSensitive);
}

enum class ERetargetedCreate
{
	Equal,      // the copy extracts as the re-targeted document
	Refused,    // the class cannot be created; the refusal names the way out
	NotChecked, // read-only extract only
};

// Engine asset -> read-only extract (checked) -> re-targeted Create -> extract of the copy equals
// the re-targeted document (or, for a class that cannot be created, the refusal).
void ExtractRetargetCreate(FAutomationTestBase& Test, const TCHAR* EnginePackage, const TCHAR* Prefix, ERetargetedCreate Create,
	const TCHAR* ExpectedCreateCode = nullptr)
{
	const FString Stage = FPackageName::GetLongPackageAssetName(EnginePackage);
	UObject* Asset = LoadObject<UObject>(nullptr, *ObjectPathOf(EnginePackage));
	if (!Test.TestNotNull(*FString::Printf(TEXT("%s: engine asset exists"), *Stage), Asset))
	{
		return;
	}
	UPackage* EnginePackageObject = Asset->GetOutermost();
	const bool bDirtyBefore = EnginePackageObject->IsDirty();
	const TArray<FString> GameBefore = GamePackageNames();
	const FAssetDocumentResult Extracted = Extract(Test, EnginePackage, Stage + TEXT(" extract"));
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: extract succeeds"), *Stage), Extracted.IsSuccess() && Extracted.Payload.IsValid()))
	{
		return;
	}
	Test.TestTrue(*FString::Printf(TEXT("%s: result is read_only"), *Stage), Extracted.bReadOnly);
	Test.TestTrue(*FString::Printf(TEXT("%s: read_only is serialized"), *Stage), Extracted.ToJson()->HasField(TEXT("read_only")));
	Test.TestTrue(*FString::Printf(TEXT("%s: message says read-only (%s)"), *Stage, *Extracted.Message), Extracted.Message.Contains(TEXT("read-only")));
	Test.TestEqual(*FString::Printf(TEXT("%s: Target as given"), *Stage), Extracted.Payload->GetStringField(TEXT("Target")), FString(EnginePackage));
	Test.TestEqual(*FString::Printf(TEXT("%s: Class"), *Stage), Extracted.Payload->GetStringField(TEXT("Class")), Asset->GetClass()->GetPathName());
	Test.TestFalse(*FString::Printf(TEXT("%s: payload is a plain document"), *Stage), Extracted.Payload->HasField(TEXT("read_only")));
	Test.TestEqual(*FString::Printf(TEXT("%s: the read does not dirty the engine package"), *Stage), EnginePackageObject->IsDirty(), bDirtyBefore);
	Test.TestEqual(*FString::Printf(TEXT("%s: the read leaves no /Game package"), *Stage), FString::Join(GamePackageNames(), TEXT(",")), FString::Join(GameBefore, TEXT(",")));
	{
		FAssetDocumentExtractRequest Second;
		Second.AssetPath = ObjectPathOf(EnginePackage);
		Test.TestTrue(*FString::Printf(TEXT("%s: object path extracts the same"), *Stage),
			Serialize(Service().Extract(Second).Payload) == Serialize(Extracted.Payload));
	}

	if (Create == ERetargetedCreate::NotChecked)
	{
		return;
	}
	const FTarget Copy(Prefix);
	ON_SCOPE_EXIT { Copy.Cleanup(); };
	const FAssetDocumentResult Created = Apply(Test, Copy, Retarget(Extracted.Payload, EnginePackage, Copy.PackageName, TEXT("Create")),
		Stage + TEXT(" re-targeted create"));
	if (Create == ERetargetedCreate::Refused)
	{
		Test.TestFalse(*FString::Printf(TEXT("%s: Create is refused"), *Stage), Created.IsSuccess());
		Test.TestEqual(*FString::Printf(TEXT("%s: refusal code"), *Stage), FirstCode(Created), FString(ExpectedCreateCode));
		Test.TestTrue(*FString::Printf(TEXT("%s: refusal names duplicate_asset"), *Stage), Created.Message.Contains(TEXT("duplicate_asset")));
		Test.TestFalse(*FString::Printf(TEXT("%s: nothing created"), *Stage), Copy.ExistsAnywhere());
		return;
	}
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: re-targeted document applies as Create"), *Stage), Created.IsSuccess()))
	{
		return;
	}
	const FAssetDocumentResult CopyExtract = Extract(Test, Copy.PackageName, Stage + TEXT(" copy extract"));
	Test.TestFalse(*FString::Printf(TEXT("%s: the copy is not read-only"), *Stage), CopyExtract.bReadOnly);
	const FString Expected = Retarget(Extracted.Payload, EnginePackage, Copy.PackageName,
		*Extracted.Payload->GetStringField(TEXT("Action")));
	const FString Actual = Normalized(CopyExtract.Payload);
	if (!Test.TestEqual(*FString::Printf(TEXT("%s: the copy extracts as the re-targeted document"), *Stage), Actual, Expected))
	{
		Test.AddInfo(FString::Printf(TEXT("%s expected: %s"), *Stage, *Expected.Left(4000)));
		Test.AddInfo(FString::Printf(TEXT("%s actual:   %s"), *Stage, *Actual.Left(4000)));
	}
	Test.TestEqual(*FString::Printf(TEXT("%s: engine package still clean"), *Stage), EnginePackageObject->IsDirty(), bDirtyBefore);
}
} // namespace AssetDocumentEngineAssetsTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentEngineAssetsReadOnlyExtractTest,
	"UEAssetDoc.EngineAssets.ReadOnlyExtract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEngineAssetsReadOnlyExtractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEngineAssetsTests;
	ExtractRetargetCreate(*this, EngineMaterial, TEXT("M_Copy"), ERetargetedCreate::Equal);
	ExtractRetargetCreate(*this, EngineBlendSpace, TEXT("BS_Copy"), ERetargetedCreate::Equal);
	// Engine Blueprints extract read-only; re-creating one from its extract is Blueprint graph authoring
	// (a separate follow-up: deprecated action keys, component calls the context-free action menu
	// filters out, an event graph page on a Blueprint type without event graphs). The editable copy of
	// a Blueprint is duplicate_asset (EngineAssets.Duplicate).
	for (const TCHAR* Blueprint : {EngineBlueprint, EngineBlueprintClass, EngineMoveGoal, EngineDamageType})
	{
		ExtractRetargetCreate(*this, Blueprint, TEXT("BP_Copy"), ERetargetedCreate::NotChecked);
	}
	// A Skeleton is created by a mesh import: its extract is readable, Create is refused with the way out.
	ExtractRetargetCreate(*this, EngineSkeleton, TEXT("SK_Copy"), ERetargetedCreate::Refused, TEXT("Skeleton.CreateUnsupported"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentEngineAssetsPluginDataTableTest,
	"UEAssetDoc.EngineAssets.PluginDataTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEngineAssetsPluginDataTableTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEngineAssetsTests;
	// A DataTable under this plugin's mount (/UEAssetDoc, outside /Game), kept in memory.
	const FTarget Source(TEXT("DT_Source"));
	const FTarget Copy(TEXT("DT_Copy"));
	const FString PluginPackage = FString::Printf(TEXT("/UEAssetDoc/%s/%s"), Folder, *Source.AssetName);
	ON_SCOPE_EXIT
	{
		Source.Cleanup();
		Copy.Cleanup();
		DiscardInMemoryPackage(PluginPackage);
	};
	if (!TestTrue(TEXT("plugin mount exists"), FPackageName::IsValidLongPackageName(PluginPackage)))
	{
		return false;
	}
	const FString Table = FString::Printf(
		TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.DataTable\",\"Action\":\"Create\",\"Properties\":{},")
		TEXT("\"Body\":{\"RowStruct\":\"%s\",\"Rows\":[{\"Name\":\"Sword\",\"Values\":{\"Label\":\"Sword\",\"Count\":3}},")
		TEXT("{\"Name\":\"Bow\",\"Values\":{\"Count\":2,\"Weight\":0.5}}]}}"), *Source.PackageName, TestRow);
	if (!TestTrue(TEXT("source table"), Apply(*this, Source, Table, TEXT("source table")).IsSuccess()))
	{
		return false;
	}
	UDataTable* PluginTable = DuplicateObject<UDataTable>(Source.Get<UDataTable>(), CreatePackage(*PluginPackage), *Source.AssetName);
	PluginTable->SetFlags(RF_Public | RF_Standalone);
	FAssetRegistryModule::AssetCreated(PluginTable);

	const FAssetDocumentResult Extracted = Extract(*this, PluginPackage, TEXT("plugin extract"));
	if (!TestTrue(TEXT("plugin table extracts"), Extracted.IsSuccess() && Extracted.Payload.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("read_only"), Extracted.bReadOnly);
	TestEqual(TEXT("Target as given"), Extracted.Payload->GetStringField(TEXT("Target")), PluginPackage);
	TestFalse(TEXT("the read leaves the plugin package clean"), PluginTable->GetOutermost()->IsDirty());
	const FAssetDocumentResult SourceExtract = Extract(*this, Source.PackageName, TEXT("source extract"));
	TestEqual(TEXT("same document as the /Game table, Target aside"),
		Retarget(Extracted.Payload, PluginPackage, Source.PackageName, TEXT("CreateOrUpdate")), Normalized(SourceExtract.Payload));

	TestTrue(TEXT("re-targeted document applies as Create"),
		Apply(*this, Copy, Retarget(Extracted.Payload, PluginPackage, Copy.PackageName, TEXT("Create")), TEXT("copy")).IsSuccess());
	TestEqual(TEXT("the copy extracts as the re-targeted document"), Normalized(Extract(*this, Copy.PackageName, TEXT("copy extract")).Payload),
		Retarget(Extracted.Payload, PluginPackage, Copy.PackageName, TEXT("CreateOrUpdate")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentEngineAssetsReadOnlyRefusalsTest,
	"UEAssetDoc.EngineAssets.ReadOnlyRefusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEngineAssetsReadOnlyRefusalsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEngineAssetsTests;
	const FAssetDocumentResult Extracted = Extract(*this, EngineBlendSpace, TEXT("extract"));
	if (!TestTrue(TEXT("engine blend space extracts"), Extracted.IsSuccess() && Extracted.Payload.IsValid()))
	{
		return false;
	}
	auto ExpectReadOnly = [this](const FAssetDocumentResult& Result, const TCHAR* Stage)
	{
		Report(*this, Stage, Result);
		TestFalse(*FString::Printf(TEXT("%s fails"), Stage), Result.IsSuccess());
		TestEqual(*FString::Printf(TEXT("%s: first diagnostic"), Stage), FirstCode(Result), FString(TEXT("Meta.ReadOnlyTarget")));
		TestTrue(*FString::Printf(TEXT("%s: message leads with it (%s)"), Stage, *Result.Message),
			Result.Message.StartsWith(TEXT("Meta.ReadOnlyTarget: ")) && Result.Message.Contains(TEXT("duplicate_asset")));
		TestTrue(*FString::Printf(TEXT("%s: at /Target"), Stage), !Result.Diagnostics.IsEmpty() && Result.Diagnostics[0].Path == TEXT("/Target"));
	};
	{
		FAssetDocumentValidateRequest Request;
		Request.Document = Extracted.Payload;
		ExpectReadOnly(Service().Validate(Request), TEXT("validate"));
	}
	{
		FAssetDocumentDiffRequest Request;
		Request.Document = Extracted.Payload;
		ExpectReadOnly(Service().Diff(Request), TEXT("diff"));
	}
	{
		FAssetDocumentTemplateRequest Request;
		Request.Class = TEXT("/Script/Engine.BlendSpace1D");
		Request.Target = EngineBlendSpace;
		ExpectReadOnly(Service().CreateTemplate(Request), TEXT("template"));
	}
	{
		// The unchanged extract saved as a /Game sidecar: its Target is still the engine path.
		const FTarget Sidecar(TEXT("BS_Sidecar"));
		ON_SCOPE_EXIT { Sidecar.Cleanup(); };
		ExpectReadOnly(Apply(*this, Sidecar, Serialize(Extracted.Payload), TEXT("apply")), TEXT("apply"));
		TestFalse(TEXT("apply created nothing"), Sidecar.ExistsAnywhere());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentEngineAssetsDuplicateTest,
	"UEAssetDoc.EngineAssets.Duplicate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEngineAssetsDuplicateTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEngineAssetsTests;
	const FTarget Skeleton(TEXT("SK_Dup"));
	const FTarget Montage(TEXT("AM_OnCopy"));
	const FTarget Material(TEXT("M_Dup"));
	const FTarget Sequence(TEXT("AS_Dup"));
	const FTarget Blueprint(TEXT("BP_Dup"));
	const FTarget Table(TEXT("DT_Source"));
	const FTarget TableCopy(TEXT("DT_Dup"));
	ON_SCOPE_EXIT
	{
		Montage.Cleanup();
		Skeleton.Cleanup();
		Material.Cleanup();
		Sequence.Cleanup();
		Blueprint.Cleanup();
		TableCopy.Cleanup();
		Table.Cleanup();
	};
	auto ExpectCopy = [this](const FAssetDocumentResult& Result, const FTarget& Target, UClass* Class, const TCHAR* Stage)
	{
		TestTrue(*FString::Printf(TEXT("%s: duplicated"), Stage), Result.IsSuccess());
		TestTrue(*FString::Printf(TEXT("%s: saved"), Stage), Result.bSavedAsset && IFileManager::Get().FileExists(*Target.PackageFile));
		TestEqual(*FString::Printf(TEXT("%s: asset_path"), Stage), Result.AssetPath, Target.ObjectPath);
		TestTrue(*FString::Printf(TEXT("%s: class"), Stage), Target.Get() != nullptr && Target.Get()->IsA(Class));
		TestFalse(*FString::Printf(TEXT("%s: copy is saved clean"), Stage), Target.Get() != nullptr && Target.Get()->GetOutermost()->IsDirty());
		TestTrue(*FString::Printf(TEXT("%s: registered"), Stage),
			FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(Target.ObjectPath)).IsValid());
		const FAssetDocumentResult Extracted = Extract(*this, Target.PackageName, FString(Stage) + TEXT(" extract"));
		TestTrue(*FString::Printf(TEXT("%s: the copy extracts, authorable"), Stage), Extracted.IsSuccess() && !Extracted.bReadOnly);
	};

	// Trial 2 task 7: an engine Skeleton copied to /Game takes a slot; a montage on the copy plays the
	// engine animation.
	ExpectCopy(Duplicate(*this, EngineSkeleton, Skeleton.PackageName, TEXT("skeleton")), Skeleton, USkeleton::StaticClass(), TEXT("skeleton"));
	if (USkeleton* Copy = Skeleton.Get<USkeleton>())
	{
		USkeleton* Original = LoadObject<USkeleton>(nullptr, *ObjectPathOf(EngineSkeleton));
		TestTrue(TEXT("skeleton copy keeps the bones"), Original != nullptr
			&& Copy->GetReferenceSkeleton().GetNum() == Original->GetReferenceSkeleton().GetNum());
		const FString AddSlot = FString::Printf(
			TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Skeleton\",\"Action\":\"Update\",")
			TEXT("\"Properties\":{\"CompatibleSkeletons\":[\"%s\"]},")
			TEXT("\"Body\":{\"SlotGroups\":[{\"Name\":\"DefaultGroup\",\"Slots\":[\"DefaultSlot\",\"UpperBody\"]}]}}"),
			*Skeleton.PackageName, *ObjectPathOf(EngineSkeleton));
		TestTrue(TEXT("a slot is added to the copy"), Apply(*this, Skeleton, AddSlot, TEXT("add slot")).IsSuccess());
		TestTrue(TEXT("copy has the slot"), Skeleton.Get<USkeleton>() != nullptr && Skeleton.Get<USkeleton>()->ContainsSlotName(TEXT("UpperBody")));
		const FString MontageDocument = FString::Printf(
			TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.AnimMontage\",\"Action\":\"Create\",\"Definitions\":{},\"Properties\":{},")
			TEXT("\"Body\":{\"Skeleton\":\"%s\",\"SlotAnimTracks\":[{\"SlotName\":\"UpperBody\",\"Segments\":[")
			TEXT("{\"AnimReference\":\"%s\",\"StartPos\":0.0,\"AnimStartTime\":0.0,\"AnimEndTime\":1.0,\"AnimPlayRate\":1.0,\"LoopingCount\":1}]}],")
			TEXT("\"CompositeSections\":[{\"Name\":\"Default\",\"NextSectionName\":null,\"LinkableTime\":0.0,\"SlotName\":\"UpperBody\",\"LinkMethod\":0}]}}"),
			*Montage.PackageName, *Skeleton.ObjectPath, *ObjectPathOf(EngineIdle));
		TestTrue(TEXT("a montage on the copy uses the engine animation"), Apply(*this, Montage, MontageDocument, TEXT("montage")).IsSuccess());
		TestTrue(TEXT("montage plays on UpperBody"), Montage.Get<UAnimMontage>() != nullptr
			&& Montage.Get<UAnimMontage>()->SlotAnimTracks.Num() == 1
			&& Montage.Get<UAnimMontage>()->SlotAnimTracks[0].SlotName == TEXT("UpperBody"));
	}

	ExpectCopy(Duplicate(*this, ObjectPathOf(EngineMaterial), Material.PackageName, TEXT("material")), Material, UMaterial::StaticClass(), TEXT("material"));
	ExpectCopy(Duplicate(*this, EngineIdle, Sequence.PackageName, TEXT("sequence")), Sequence, UAnimSequence::StaticClass(), TEXT("sequence"));
	if (const UAnimSequence* Copy = Sequence.Get<UAnimSequence>())
	{
		TestEqual(TEXT("dependencies are not duplicated: the copy uses the engine Skeleton"),
			Copy->GetSkeleton() != nullptr ? Copy->GetSkeleton()->GetOutermost()->GetName() : FString(), FString(EngineSkeleton));
	}
	{
		const FAssetDocumentResult Result = Duplicate(*this, EngineBlueprint, Blueprint.PackageName, TEXT("blueprint"));
		ExpectCopy(Result, Blueprint, UBlueprint::StaticClass(), TEXT("blueprint"));
		const UBlueprint* Copy = Blueprint.Get<UBlueprint>();
		TestTrue(TEXT("blueprint copy has its own generated class"), Copy != nullptr && Copy->GeneratedClass != nullptr
			&& Copy->GeneratedClass->GetOutermost() == Copy->GetOutermost());
		TestTrue(TEXT("blueprint result carries compile"), Result.Compile.IsValid());
	}
	{
		const FString Source = FString::Printf(
			TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.DataTable\",\"Action\":\"Create\",\"Properties\":{},")
			TEXT("\"Body\":{\"RowStruct\":\"%s\",\"Rows\":[{\"Name\":\"A\",\"Values\":{\"Count\":1}}]}}"), *Table.PackageName, TestRow);
		if (TestTrue(TEXT("project table"), Apply(*this, Table, Source, TEXT("table")).IsSuccess()))
		{
			ExpectCopy(Duplicate(*this, Table.PackageName, TableCopy.PackageName, TEXT("table")), TableCopy, UDataTable::StaticClass(), TEXT("table"));
			TestEqual(TEXT("table copy extracts as the source, Target aside"),
				Normalized(Extract(*this, TableCopy.PackageName, TEXT("table copy")).Payload),
				Retarget(Extract(*this, Table.PackageName, TEXT("table source")).Payload, Table.PackageName, TableCopy.PackageName, TEXT("CreateOrUpdate")));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentEngineAssetsDuplicateRefusalsTest,
	"UEAssetDoc.EngineAssets.DuplicateRefusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEngineAssetsDuplicateRefusalsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEngineAssetsTests;
	const FTarget Existing(TEXT("SK_Existing"));
	const FTarget Fresh(TEXT("SK_Fresh"));
	ON_SCOPE_EXIT
	{
		Existing.Cleanup();
		Fresh.Cleanup();
	};
	auto ExpectRefused = [this](const FAssetDocumentResult& Result, const TCHAR* Code, const TCHAR* Path, const TCHAR* Stage)
	{
		TestFalse(*FString::Printf(TEXT("%s: refused"), Stage), Result.IsSuccess());
		TestEqual(*FString::Printf(TEXT("%s: code"), Stage), FirstCode(Result), FString(Code));
		TestTrue(*FString::Printf(TEXT("%s: path %s"), Stage, Path), !Result.Diagnostics.IsEmpty() && Result.Diagnostics[0].Path == Path);
		TestTrue(*FString::Printf(TEXT("%s: message leads with the code"), Stage), Result.Message.StartsWith(Code));
		TestFalse(*FString::Printf(TEXT("%s: not saved"), Stage), Result.bSavedAsset);
	};
	if (!TestTrue(TEXT("first copy"), Duplicate(*this, EngineSkeleton, Existing.PackageName, TEXT("first")).IsSuccess()))
	{
		return false;
	}
	const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Existing.PackageFile);
	ExpectRefused(Duplicate(*this, EngineIdle, Existing.PackageName, TEXT("target exists")),
		TEXT("Duplicate.TargetExists"), TEXT("/target"), TEXT("target exists"));
	TestTrue(TEXT("existing target untouched"), Existing.Get<USkeleton>() != nullptr
		&& IFileManager::Get().GetTimeStamp(*Existing.PackageFile) == Stamp);

	const FString EngineTarget = FString::Printf(TEXT("%sSK_NotHere_%s"), TutorialDir, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	ExpectRefused(Duplicate(*this, EngineSkeleton, EngineTarget, TEXT("engine target")),
		TEXT("Duplicate.InvalidTarget"), TEXT("/target"), TEXT("engine target"));
	TestNull(TEXT("no engine package"), FindPackage(nullptr, *EngineTarget));
	ExpectRefused(Duplicate(*this, EngineSkeleton, TEXT("/Game/Bad Name"), TEXT("invalid target")),
		TEXT("Duplicate.InvalidTarget"), TEXT("/target"), TEXT("invalid target"));

	const FString Missing = FString::Printf(TEXT("/Game/%s/Missing_%s"), Folder, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	ExpectRefused(Duplicate(*this, Missing, Fresh.PackageName, TEXT("missing source")),
		TEXT("Duplicate.SourceNotFound"), TEXT("/source"), TEXT("missing source"));
	TestFalse(TEXT("missing source: nothing created"), Fresh.ExistsAnywhere());
	TestNull(TEXT("missing source: no target package"), FindPackage(nullptr, *Fresh.PackageName));
	TestNull(TEXT("missing source: no source package"), FindPackage(nullptr, *Missing));
	ExpectRefused(Duplicate(*this, TEXT("/Script/Engine.Skeleton"), Fresh.PackageName, TEXT("class source")),
		TEXT("Duplicate.SourceNotFound"), TEXT("/source"), TEXT("class source"));
	TestFalse(TEXT("class source: nothing created"), Fresh.ExistsAnywhere());
	return true;
}
#endif

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentEngineAssetsReadOnlyInspectTest,
	"UEAssetDoc.EngineAssets.ReadOnlyInspect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEngineAssetsReadOnlyInspectTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEngineAssetsTests;
	auto Inspect = [this](const FString& Path, const TCHAR* Stage)
	{
		FAssetDocumentInspectRequest Request;
		Request.ClassOrAsset = Path;
		FAssetDocumentResult Result = Service().Inspect(Request);
		if (!Result.IsSuccess())
		{
			Report(*this, Stage, Result);
		}
		return Result;
	};

	// An engine Material: the Material profile's details (document, pins), read-only.
	UObject* const Material = LoadObject<UObject>(nullptr, *ObjectPathOf(EngineMaterial));
	if (!TestNotNull(TEXT("engine material loads"), Material))
	{
		return false;
	}
	const bool bMaterialDirty = Material->GetOutermost()->IsDirty();
	const FAssetDocumentResult MaterialInspect = Inspect(EngineMaterial, TEXT("material inspect"));
	if (TestTrue(TEXT("engine material inspects"), MaterialInspect.IsSuccess() && MaterialInspect.Payload.IsValid()))
	{
		TestTrue(TEXT("material: read_only"), MaterialInspect.bReadOnly);
		TestTrue(TEXT("material: ReadOnly fact"), MaterialInspect.Payload->GetBoolField(TEXT("ReadOnly")));
		TestTrue(TEXT("material: profile details (Body, Pins)"),
			MaterialInspect.Payload->HasField(TEXT("Body")) && MaterialInspect.Payload->HasField(TEXT("Pins")));
		TestEqual(TEXT("material: Target is the engine path"), MaterialInspect.Payload->GetStringField(TEXT("Target")),
			FString(EngineMaterial));
		TestTrue(TEXT("material: no error diagnostics"), !MaterialInspect.Diagnostics.ContainsByPredicate(
			[](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Severity != TEXT("info"); }));
	}
	TestEqual(TEXT("material: the read leaves the engine package as it was"), Material->GetOutermost()->IsDirty(), bMaterialDirty);

	// A User Defined Enum outside /Game (plugin content such as /Niagara/Enums/...).
	TArray<FAssetData> Enums;
	FAssetRegistryModule::GetRegistry().GetAssetsByClass(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("UserDefinedEnum")), Enums);
	Enums.RemoveAll([](const FAssetData& Asset) { return Asset.PackageName.ToString().StartsWith(TEXT("/Game/")); });
	Enums.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });
	if (Enums.IsEmpty())
	{
		AddWarning(TEXT("No User Defined Enum outside /Game; enum inspect checks skipped."));
		return true;
	}
	const FAssetData& Enum = Enums[0];
	const FString EnumPackage = Enum.PackageName.ToString();
	const FAssetDocumentResult EnumInspect = Inspect(EnumPackage, TEXT("enum inspect"));
	if (TestTrue(TEXT("plugin enum inspects"), EnumInspect.IsSuccess() && EnumInspect.Payload.IsValid()))
	{
		TestTrue(TEXT("enum: read_only"), EnumInspect.bReadOnly);
		TestTrue(TEXT("enum: enumerator details"), EnumInspect.Payload->HasField(TEXT("EnumeratorDetails")));
		TestEqual(TEXT("enum: class"), EnumInspect.Payload->GetStringField(TEXT("Class")),
			FString(TEXT("/Script/Engine.UserDefinedEnum")));
	}
	// The same enum named without its sub-folder ("/Niagara/Enums/ENiagara_X" for
	// "/Niagara/Enums/SpriteRenderer/ENiagara_X"): resolves when the name is unique under the mount.
	const FString Root = EnumPackage.Left(EnumPackage.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1));
	const FString Guessed = Root + TEXT("/") + Enum.AssetName.ToString();
	TArray<FAssetData> SameName;
	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*Root));
	Filter.bRecursivePaths = true;
	FAssetRegistryModule::GetRegistry().EnumerateAssets(Filter, [&SameName, &Enum](const FAssetData& Asset)
	{
		if (Asset.AssetName == Enum.AssetName)
		{
			SameName.Add(Asset);
		}
		return true;
	});
	FAssetDocumentInspectRequest GuessRequest;
	GuessRequest.ClassOrAsset = Guessed;
	const FAssetDocumentResult GuessInspect = Service().Inspect(GuessRequest);
	if (Guessed != EnumPackage && SameName.Num() == 1)
	{
		TestTrue(TEXT("guessed path resolves to the only asset of that name"), GuessInspect.IsSuccess()
			&& GuessInspect.AssetPath == Enum.GetObjectPathString() && GuessInspect.Message.Contains(TEXT("resolved to")));
	}
	const FString Missing = Root + TEXT("/NoSuchFolder/NoSuchAsset_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	FAssetDocumentInspectRequest MissingRequest;
	MissingRequest.ClassOrAsset = Missing;
	const FAssetDocumentResult MissingInspect = Service().Inspect(MissingRequest);
	TestTrue(TEXT("a missing plugin asset is Inspect.AssetNotFound, not ClassResolve.Unresolved"),
		!MissingInspect.IsSuccess() && MissingInspect.Diagnostics.Num() > 0
		&& MissingInspect.Diagnostics[0].Code == TEXT("Inspect.AssetNotFound"));
	// A bare enum name (as a module input shows its type) inspects the enum of that name, or lists them.
	TArray<FAssetData> NamedEnums;
	FARFilter TypeFilter;
	TypeFilter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("UserDefinedEnum")));
	TypeFilter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("UserDefinedStruct")));
	FAssetRegistryModule::GetRegistry().GetAssets(TypeFilter, NamedEnums);
	NamedEnums.RemoveAll([&Enum](const FAssetData& Asset) { return Asset.AssetName != Enum.AssetName; });
	FAssetDocumentInspectRequest BareRequest;
	BareRequest.ClassOrAsset = Enum.AssetName.ToString();
	const FAssetDocumentResult BareInspect = Service().Inspect(BareRequest);
	if (NamedEnums.Num() == 1)
	{
		TestTrue(TEXT("a bare enum name inspects that enum"), BareInspect.IsSuccess()
			&& BareInspect.AssetPath == Enum.GetObjectPathString() && BareInspect.Payload.IsValid()
			&& BareInspect.Payload->HasField(TEXT("EnumeratorDetails")));
	}
	else
	{
		TestTrue(TEXT("an ambiguous bare enum name lists the enums"), !BareInspect.IsSuccess()
			&& BareInspect.Diagnostics.Num() > 0 && BareInspect.Diagnostics[0].Message.Contains(Enum.GetObjectPathString()));
	}
	return true;
}
#endif
