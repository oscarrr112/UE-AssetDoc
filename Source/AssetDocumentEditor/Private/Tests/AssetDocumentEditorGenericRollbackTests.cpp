// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Production-Service rollback regressions for the generic route found by the
// coverage audit (PaperSprite, Paper2D):
//   - EditorRebuiltSubobjects: a property write whose PostEditChange replaces a
//     subobject (UPaperSprite::RebuildCollisionData creates a new BodySetup on
//     every change, and again on PostEditUndo) fails after mutation, at the fresh
//     verification of the staged package. Rollback must restore the asset in memory
//     (functionally equivalent, as the editor's Undo leaves it) instead of poisoning the
//     transaction, leave the package clean and accept the next apply. The failure
//     leads with its root cause (diagnostics[0] and the message).
//   - EditorNormalizedValue: a value the editor's PostEditChange rewrites (an
//     unnamed sprite socket) is refused before mutation with an AI-readable
//     diagnostic naming what the editor would store.
//   - PreflightIsolationFirstUse: objects created by the first use of content (a first
//     load, engine content initialized on first use) pass as info; one created in a /Game
//     package already in memory, or a package the probe creates, fails with cause and remedy.
//   - PreflightIsolation: the preflight's PostEditChange on the scratch sprite
//     reaches the real AtlasGroup (the atlas rebuilds and dirties its package); the
//     isolation guard restores it, so a refused apply leaves every asset unchanged.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "Provider/Property/AssetDocumentEditorPreflightIsolation.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
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
#include "Serialization/ObjectWriter.h"
#include "UObject/PropertyAccessUtil.h"
#include "UObject/SavePackage.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectHash.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorGenericRollbackTests
{
const TCHAR* const SpriteClass = TEXT("/Script/Paper2D.PaperSprite");
const TCHAR* const AtlasClass = TEXT("/Script/Paper2D.PaperSpriteAtlas");

struct FTarget
{
	FString ClassPath;
	FString AssetName;
	FString PackageName;
	FString ObjectPath;
	FString PackageFile;
	FString SidecarPath;

	explicit FTarget(const TCHAR* Prefix, const TCHAR* InClassPath = SpriteClass)
		: ClassPath(InClassPath)
		, AssetName(FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits)))
	{
		PackageName = FString(TEXT("/Game/AssetDocumentRollback/")) + AssetName;
		ObjectPath = PackageName + TEXT(".") + AssetName;
		PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		SidecarPath = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("AssetDocumentRollback"),
			AssetName + TEXT(".assetdoc.json"));
	}

	FString Document(const TCHAR* Action, const FString& Properties) const
	{
		return FString::Printf(
			TEXT("{\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\",\"Target\":\"%s\",\"Class\":\"%s\",")
			TEXT("\"Action\":\"%s\",\"Properties\":%s}"),
			*PackageName, *ClassPath, Action, *Properties);
	}

	UPackage* FindLivePackage() const
	{
		return FindPackage(nullptr, *PackageName);
	}

	void Cleanup() const
	{
		UObject* Asset = FSoftObjectPath(ObjectPath).ResolveObject();
		if (UPackage* Package = FindLivePackage())
		{
			Package->SetDirtyFlag(false);
		}
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

FAssetDocumentResult ApplyJson(FAutomationTestBase& Test, const FTarget& Target, const FString& Json, const TCHAR* Stage)
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

// Ignored: references to subobjects the editor rebuilds on PostEditChange/PostEditUndo
// (functional equivalence: a rebuilt subobject may get a new name).
FString ExtractProperties(const FTarget& Target, TConstArrayView<const TCHAR*> Ignored = {})
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = Target.ObjectPath;
	Request.bDiffOnly = false;
	Request.bIncludeAllWritable = true;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Extract(Request);
	if (!Result.IsSuccess() || !Result.Payload.IsValid() || !Result.Payload->HasTypedField<EJson::Object>(TEXT("Properties")))
	{
		return FString();
	}
	const TSharedPtr<FJsonObject> Properties = Result.Payload->GetObjectField(TEXT("Properties"));
	for (const TCHAR* Name : Ignored)
	{
		Properties->RemoveField(Name);
	}
	return Serialize(Properties);
}

bool HasCode(const FAssetDocumentResult& Result, const FString& Code)
{
	return Result.Diagnostics.ContainsByPredicate(
		[&Code](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == Code; });
}

bool HasCodePrefix(const FAssetDocumentResult& Result, const FString& Prefix)
{
	return Result.Diagnostics.ContainsByPredicate(
		[&Prefix](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code.StartsWith(Prefix); });
}

// The failed apply left the asset exactly as before, clean, and not poisoned.
void TestRolledBack(FAutomationTestBase& Test, const FTarget& Target, const FAssetDocumentResult& Result,
	const FString& PropertiesBefore, const UObject* AssetBefore, const TCHAR* Stage,
	TConstArrayView<const TCHAR*> IgnoredProperties = {})
{
	Test.TestFalse(*FString::Printf(TEXT("%s: apply fails"), Stage), Result.IsSuccess());
	Test.TestFalse(*FString::Printf(TEXT("%s: rollback is not poisoned"), Stage), HasCodePrefix(Result, TEXT("Atomic.Poisoned")));
	Test.TestFalse(*FString::Printf(TEXT("%s: package snapshot restored"), Stage), HasCode(Result, TEXT("PackageSnapshot.RestoreFailed")));
	Test.TestFalse(*FString::Printf(TEXT("%s: restored in memory, not reloaded"), Stage),
		HasCode(Result, TEXT("PackageSnapshot.ReloadedFromDisk")));
	const UPackage* Package = Target.FindLivePackage();
	Test.TestTrue(*FString::Printf(TEXT("%s: package is clean"), Stage), Package != nullptr && !Package->IsDirty());
	Test.TestTrue(*FString::Printf(TEXT("%s: same asset object"), Stage),
		FSoftObjectPath(Target.ObjectPath).ResolveObject() == AssetBefore);
	Test.TestEqual(*FString::Printf(TEXT("%s: properties unchanged"), Stage), ExtractProperties(Target, IgnoredProperties), PropertiesBefore);
}

// A property (here BodySetup) must point at a live subobject of the asset.
void TestSubobjectReference(FAutomationTestBase& Test, UObject& Asset, const TCHAR* PropertyName, const TCHAR* Stage)
{
	const FObjectProperty* Property = CastField<FObjectProperty>(Asset.GetClass()->FindPropertyByName(PropertyName));
	if (!Test.TestNotNull(*FString::Printf(TEXT("%s: %s property exists"), Stage, PropertyName), Property))
	{
		return;
	}
	const UObject* Value = Property->GetObjectPropertyValue_InContainer(&Asset);
	Test.TestTrue(*FString::Printf(TEXT("%s: %s is a live subobject"), Stage, PropertyName),
		IsValid(Value) && Value->IsIn(&Asset));
}
} // namespace AssetDocumentEditorGenericRollbackTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericRollbackEditorRebuiltSubobjectsTest,
	"UEAssetDoc.GenericRuntime.Rollback.EditorRebuiltSubobjects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericRollbackEditorRebuiltSubobjectsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericRollbackTests;
	if (FindObject<UClass>(nullptr, SpriteClass) == nullptr)
	{
		AddWarning(TEXT("Paper2D is not loaded; PaperSprite rollback regression skipped."));
		return true;
	}
	const FTarget Target(TEXT("Sprite_Rollback"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	const FAssetDocumentResult Created = ApplyJson(*this, Target,
		Target.Document(TEXT("Create"), TEXT("{\"PivotMode\":\"Custom\"}")), TEXT("create"));
	if (!TestTrue(TEXT("sprite is created"), Created.IsSuccess() && Created.bSavedAsset))
	{
		return false;
	}
	UObject* const Asset = FSoftObjectPath(Target.ObjectPath).ResolveObject();
	UPackage* const Package = Target.FindLivePackage();
	if (!TestNotNull(TEXT("created sprite is loaded"), Asset) || !TestNotNull(TEXT("created package is loaded"), Package))
	{
		return false;
	}
	// A durable create saved the package; like UE's own save it is no longer "newly created".
	TestFalse(TEXT("saved package is no longer newly created"), Package->HasAnyPackageFlags(PKG_NewlyCreated));
	// BodySetup is rebuilt (renamed) by every sprite PostEditChange, including PostEditUndo;
	// it is checked as a live subobject instead.
	const TArray<const TCHAR*> Rebuilt = {TEXT("BodySetup")};
	const FString PropertiesBefore = ExtractProperties(Target, Rebuilt);
	TestFalse(TEXT("extract succeeds"), PropertiesBefore.IsEmpty());

	// Each value is storable on its own (the per-property preflight accepts both), but
	// together they are not: with a non-Custom PivotMode the sprite's PostEditChange
	// recomputes CustomPivotPoint. The write mutates the asset (and, like every sprite
	// PostEditChange, replaces its BodySetup); the staged package then fails its
	// fresh-load verification, so the apply must roll back after mutation.
	const FAssetDocumentResult Failed = ApplyJson(*this, Target, Target.Document(TEXT("Update"),
		TEXT("{\"CustomPivotPoint\":{\"X\":3,\"Y\":4},\"PivotMode\":\"Center_Center\"}")), TEXT("inconsistent pivot"));
	TestTrue(TEXT("fresh-load verification reports the mismatch"), HasCode(Failed, TEXT("GenericRuntime.VerificationMismatch")));
	// The cause leads: diagnostics[0] and the message name the mismatch, not the coordinator's
	// invalid durable receipt or persistence's canonical mismatch that follow from it.
	TestTrue(TEXT("diagnostics[0] is the root cause"), Failed.Diagnostics.Num() > 0
		&& Failed.Diagnostics[0].Code == TEXT("GenericRuntime.VerificationMismatch"));
	TestTrue(TEXT("message leads with the root cause"), Failed.Message.StartsWith(TEXT("GenericRuntime.VerificationMismatch")));
	TestTrue(TEXT("consequences are kept after the cause"), HasCode(Failed, TEXT("RequestCoordinator.InvalidDurableReceipt")));
	TestTrue(TEXT("the mismatch names the property the editor rewrote"), Failed.Diagnostics.ContainsByPredicate(
		[](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Message.Contains(TEXT("CustomPivotPoint (asset holds")); }));
	TestRolledBack(*this, Target, Failed, PropertiesBefore, Asset, TEXT("inconsistent pivot"), Rebuilt);
	TestSubobjectReference(*this, *Asset, TEXT("BodySetup"), TEXT("after rollback"));

	// The asset stays usable: a following apply succeeds.
	const FAssetDocumentResult Next = ApplyJson(*this, Target,
		Target.Document(TEXT("Update"), TEXT("{\"CollisionThickness\":12.5}")), TEXT("following update"));
	TestTrue(TEXT("following update succeeds"), Next.IsSuccess() && Next.bSavedAsset);
	TestTrue(TEXT("following update leaves the package clean"), !Package->IsDirty());
	TestSubobjectReference(*this, *Asset, TEXT("BodySetup"), TEXT("after following update"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericRollbackEditorNormalizedValueTest,
	"UEAssetDoc.GenericRuntime.Rollback.EditorNormalizedValue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericRollbackEditorNormalizedValueTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericRollbackTests;
	if (FindObject<UClass>(nullptr, SpriteClass) == nullptr)
	{
		AddWarning(TEXT("Paper2D is not loaded; PaperSprite normalization regression skipped."));
		return true;
	}
	const FTarget Target(TEXT("Sprite_Normalized"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	const FAssetDocumentResult Created = ApplyJson(*this, Target, Target.Document(TEXT("Create"), TEXT("{}")), TEXT("create"));
	if (!TestTrue(TEXT("sprite is created"), Created.IsSuccess()))
	{
		return false;
	}
	UObject* const Asset = FSoftObjectPath(Target.ObjectPath).ResolveObject();
	const FString PropertiesBefore = ExtractProperties(Target);

	// UPaperSprite::ValidateSocketNames names an unnamed socket "Socket" in PostEditChange,
	// exactly what the Details panel does.
	const FString UnnamedSocket = TEXT("{\"Sockets\":[{\"LocalTransform\":{\"Rotation\":{\"X\":0,\"Y\":0,\"Z\":0,\"W\":1},")
		TEXT("\"Translation\":{\"X\":0,\"Y\":0,\"Z\":0},\"Scale3D\":{\"X\":1,\"Y\":1,\"Z\":1}},\"SocketName\":\"None\"}]}");
	const FAssetDocumentResult Refused = ApplyJson(*this, Target, Target.Document(TEXT("Update"), UnnamedSocket), TEXT("unnamed socket"));
	TestTrue(TEXT("refused as a value the editor stores differently"), HasCode(Refused, TEXT("Generic.PropertyValueNotStoredExactly")));
	TestTrue(TEXT("diagnostic shows the stored socket name"), Refused.Message.Contains(TEXT("Socket")));
	TestRolledBack(*this, Target, Refused, PropertiesBefore, Asset, TEXT("unnamed socket"));

	// The value the editor would store is accepted as authored.
	const FString NamedSocket = TEXT("{\"Sockets\":[{\"LocalTransform\":{\"Rotation\":{\"X\":0,\"Y\":0,\"Z\":0,\"W\":1},")
		TEXT("\"Translation\":{\"X\":0,\"Y\":0,\"Z\":0},\"Scale3D\":{\"X\":1,\"Y\":1,\"Z\":1}},\"SocketName\":\"Grip\"}]}");
	const FAssetDocumentResult Accepted = ApplyJson(*this, Target, Target.Document(TEXT("Update"), NamedSocket), TEXT("named socket"));
	TestTrue(TEXT("named socket is accepted"), Accepted.IsSuccess() && Accepted.bSavedAsset);
	return true;
}

namespace AssetDocumentEditorGenericRollbackTests
{
TArray<uint8> SerializeState(UObject& Object)
{
	TArray<uint8> Bytes;
	FObjectWriter Writer(&Object, Bytes, false, false, false, PPF_DuplicateVerbatim);
	return Bytes;
}

bool HasPaper2D(FAutomationTestBase& Test)
{
	if (FindObject<UClass>(nullptr, SpriteClass) != nullptr && FindObject<UClass>(nullptr, AtlasClass) != nullptr)
	{
		return true;
	}
	Test.AddWarning(TEXT("Paper2D is not loaded; preflight isolation regression skipped."));
	return false;
}

// A saved (clean) atlas and a saved sprite outside it. Sprite atlases have no
// factory, so the atlas is created and saved directly.
bool CreateAtlasAndSprite(FAutomationTestBase& Test, const FTarget& Atlas, const FTarget& Sprite)
{
	UPackage* const AtlasPackage = CreatePackage(*Atlas.PackageName);
	UObject* const AtlasAsset = NewObject<UObject>(AtlasPackage, FindObject<UClass>(nullptr, AtlasClass),
		*Atlas.AssetName, RF_Public | RF_Standalone);
	FAssetRegistryModule::AssetCreated(AtlasAsset);
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	const bool bAtlasSaved = UPackage::SavePackage(AtlasPackage, AtlasAsset, *Atlas.PackageFile, SaveArgs);
	const FAssetDocumentResult SpriteCreated = ApplyJson(Test, Sprite, Sprite.Document(TEXT("Create"), TEXT("{}")), TEXT("create sprite"));
	return Test.TestTrue(TEXT("atlas and sprite are created"), bAtlasSaved && SpriteCreated.IsSuccess())
		&& Test.TestTrue(TEXT("atlas package starts clean"), !AtlasPackage->IsDirty());
}
} // namespace AssetDocumentEditorGenericRollbackTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericRollbackPreflightIsolationGuardTest,
	"UEAssetDoc.GenericRuntime.Rollback.PreflightIsolationRestoresOtherPackages",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericRollbackPreflightIsolationGuardTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericRollbackTests;
	if (!HasPaper2D(*this))
	{
		return true;
	}
	const FTarget Atlas(TEXT("Atlas_Isolation"), AtlasClass);
	const FTarget Sprite(TEXT("Sprite_Isolation"));
	ON_SCOPE_EXIT { Sprite.Cleanup(); Atlas.Cleanup(); };
	if (!CreateAtlasAndSprite(*this, Atlas, Sprite))
	{
		return false;
	}
	UObject* const AtlasAsset = FSoftObjectPath(Atlas.ObjectPath).ResolveObject();
	UObject* const SpriteAsset = FSoftObjectPath(Sprite.ObjectPath).ResolveObject();
	UPackage* const AtlasPackage = Atlas.FindLivePackage();
	if (!TestNotNull(TEXT("atlas is loaded"), AtlasAsset) || !TestNotNull(TEXT("sprite is loaded"), SpriteAsset))
	{
		return false;
	}
	const TArray<uint8> AtlasBefore = SerializeState(*AtlasAsset);
	TArray<UObject*> AtlasObjectsBefore;
	GetObjectsWithOuter(AtlasPackage, AtlasObjectsBefore);

	// A transient scratch clone, as the preflight uses.
	UObject* const Scratch = DuplicateObject<UObject>(SpriteAsset, GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), SpriteAsset->GetClass(), TEXT("Scratch_Isolation")));
	Scratch->ClearFlags(RF_Public | RF_Standalone);
	Scratch->SetFlags(RF_Transient);
	ON_SCOPE_EXIT { Scratch->MarkAsGarbage(); };
	const FProperty* const AtlasGroup = Scratch->GetClass()->FindPropertyByName(TEXT("AtlasGroup"));
	if (!TestNotNull(TEXT("AtlasGroup property exists"), AtlasGroup))
	{
		return false;
	}
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	bool bRestored = false;
	TArray<FString> Touched;
	{
		FAssetDocumentEditorPreflightIsolation Isolation(TEXT("PaperSprite.AtlasGroup"));
		UObject* NewAtlasGroup = AtlasAsset;
		const EPropertyAccessResultFlags Written = PropertyAccessUtil::SetPropertyValue_Object(
			AtlasGroup, Scratch, AtlasGroup, &NewAtlasGroup, INDEX_NONE,
			PropertyAccessUtil::EditorReadOnlyFlags, EPropertyAccessChangeNotifyMode::Default);
		TestTrue(TEXT("scratch AtlasGroup is written with notification"), Written == EPropertyAccessResultFlags::Success);
		// The reproduction: the scratch's PostEditChange rebuilt the real atlas.
		TestTrue(TEXT("the scratch's PostEditChange reached and dirtied the real atlas"), AtlasPackage->IsDirty());
		Touched = Isolation.GetTouchedPackageNames();
		bRestored = Isolation.Restore(Diagnostics);
	}
	TestTrue(TEXT("isolation restored every touched package"), bRestored);
	TestTrue(TEXT("the atlas package was tracked"), Touched.Contains(Atlas.PackageName));
	TestTrue(TEXT("restoration is reported"), Diagnostics.ContainsByPredicate([](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == TEXT("PreflightIsolation.PackageRestored");
	}));
	TestFalse(TEXT("atlas package is clean again"), AtlasPackage->IsDirty());
	TestTrue(TEXT("atlas state is byte-identical"), SerializeState(*AtlasAsset) == AtlasBefore);
	TArray<UObject*> AtlasObjectsAfter;
	GetObjectsWithOuter(AtlasPackage, AtlasObjectsAfter);
	TestEqual(TEXT("atlas package holds the same objects"), AtlasObjectsAfter.Num(), AtlasObjectsBefore.Num());
	TestFalse(TEXT("sprite package stays clean"), Sprite.FindLivePackage()->IsDirty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericRollbackPreflightIsolationFirstUseTest,
	"UEAssetDoc.GenericRuntime.Rollback.PreflightIsolationFirstUse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// A trial's first use of content creates objects in packages without changing them (a Niagara
// module loaded for the first time builds its graph's editor data in PostLoad, outside the
// loader). That is reported as info and passes; an object created in a /Game package that was
// already in memory, or a package the probe creates, still fails, naming the objects and the
// remedy.
bool FAssetDocumentGenericRollbackPreflightIsolationFirstUseTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericRollbackTests;
	const TCHAR* const CurveClass = TEXT("/Script/Engine.CurveFloat");
	const FTarget Saved(TEXT("Curve_FirstLoad"), CurveClass);
	const FString CreatedPackageName = FString::Printf(TEXT("/Game/AssetDocumentRollback/Created_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	TArray<UObject*> Created;
	ON_SCOPE_EXIT
	{
		for (UObject* Object : Created)
		{
			Object->ClearFlags(RF_Public | RF_Standalone);
			Object->MarkAsGarbage();
		}
		if (UPackage* Package = FindPackage(nullptr, *CreatedPackageName))
		{
			Package->SetDirtyFlag(false);
			Package->MarkAsGarbage();
		}
		Saved.Cleanup();
	};
	auto HasDiagnostic = [](const TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics, const TCHAR* Code, const FString& Package)
	{
		return Diagnostics.ContainsByPredicate([Code, &Package](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == Code && Diagnostic.Message.Contains(Package);
		});
	};
	// A saved object (a transient one is never written by a save and is not tracked).
	auto CreateIn = [&Created](UPackage* Package, const TCHAR* Name, EObjectFlags Flags = RF_NoFlags)
	{
		UObject* Object = NewObject<UObject>(Package, FindObject<UClass>(nullptr, TEXT("/Script/Engine.CurveFloat")),
			MakeUniqueObjectName(Package, UObject::StaticClass(), Name), Flags);
		Created.Add(Object);
		return Object;
	};

	// A saved /Game asset, unloaded: the probe's first load of it.
	if (!TestTrue(TEXT("curve is created and saved"), ApplyJson(*this, Saved, Saved.Document(TEXT("Create"), TEXT("{}")),
		TEXT("create curve")).IsSuccess()))
	{
		return false;
	}
	if (UPackage* Package = Saved.FindLivePackage())
	{
		UPackageTools::UnloadPackages({Package});
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}
	if (!TestNull(TEXT("curve package is unloaded"), Saved.FindLivePackage()))
	{
		return false;
	}
	{
		TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
		FAssetDocumentEditorPreflightIsolation Isolation(TEXT("Test.FirstLoad"));
		UObject* const Loaded = LoadObject<UObject>(nullptr, *Saved.ObjectPath);
		if (!TestNotNull(TEXT("curve loads inside the probe"), Loaded))
		{
			return false;
		}
		// What a loaded object's lazily built editor data looks like to the guard.
		CreateIn(Loaded->GetOutermost(), TEXT("LazyEditorData"));
		TestTrue(TEXT("first load passes"), Isolation.Restore(Diagnostics));
		TestTrue(TEXT("first load is reported as info"), HasDiagnostic(Diagnostics, TEXT("PreflightIsolation.PackageLoaded"), Saved.PackageName));
		TestFalse(TEXT("first load is no error"), HasDiagnostic(Diagnostics, TEXT("PreflightIsolation.PackageNotRestored"), Saved.PackageName));
		TestFalse(TEXT("loaded package stays clean"), Loaded->GetOutermost()->IsDirty());
	}

	// Engine content already in memory, initialized on first use.
	UObject* const EngineAsset = LoadObject<UObject>(nullptr, TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"));
	if (TestNotNull(TEXT("engine material loads"), EngineAsset) && !EngineAsset->GetOutermost()->IsDirty())
	{
		TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
		FAssetDocumentEditorPreflightIsolation Isolation(TEXT("Test.EngineContent"));
		CreateIn(EngineAsset->GetOutermost(), TEXT("LazyEngineData"));
		TestTrue(TEXT("engine content first use passes"), Isolation.Restore(Diagnostics));
		TestTrue(TEXT("engine content first use is reported as info"),
			HasDiagnostic(Diagnostics, TEXT("PreflightIsolation.ContentInitialized"), EngineAsset->GetOutermost()->GetName()));
	}

	// A /Game package already in memory: an object created in it is a real change.
	UObject* const InMemory = LoadObject<UObject>(nullptr, *Saved.ObjectPath);
	if (TestNotNull(TEXT("curve is in memory"), InMemory))
	{
		TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
		FAssetDocumentEditorPreflightIsolation Isolation(TEXT("Test.Leak"));
		const UObject* const Leaked = CreateIn(InMemory->GetOutermost(), TEXT("Leaked"));
		TestFalse(TEXT("a leak into a /Game package fails"), Isolation.Restore(Diagnostics));
		const FAssetDocumentRuntimeDiagnostic* const Error = Diagnostics.FindByPredicate([](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == TEXT("PreflightIsolation.PackageNotRestored");
		});
		TestTrue(TEXT("the error names the object, the cause and the remedy"), Error != nullptr
			&& Error->Message.Contains(Leaked->GetName()) && Error->Message.Contains(TEXT("CurveFloat"))
			&& Error->Message.Contains(TEXT("before any change notification")) && Error->Message.Contains(TEXT("Asset Actions > Reload")));
	}

	// A transient object created (and changed) in a /Game package in memory is never saved:
	// not a change (the editor's actor factory spawns a transient preview actor in the open map).
	if (InMemory != nullptr)
	{
		TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
		FAssetDocumentEditorPreflightIsolation Isolation(TEXT("Test.Transient"));
		UObject* const Preview = CreateIn(InMemory->GetOutermost(), TEXT("TransientPreview"), RF_Transient);
		Preview->Modify();
		TestTrue(TEXT("a transient object in a /Game package passes"), Isolation.Restore(Diagnostics));
		TestEqual(TEXT("a transient object is not tracked"), Isolation.GetTouchedPackageNames().Num(), 0);
		TestFalse(TEXT("its package stays clean"), InMemory->GetOutermost()->IsDirty());
	}

	// A /Game package the probe creates.
	{
		TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
		FAssetDocumentEditorPreflightIsolation Isolation(TEXT("Test.CreatedPackage"));
		CreateIn(CreatePackage(*CreatedPackageName), TEXT("Orphan"));
		TestFalse(TEXT("a package created by the probe fails"), Isolation.Restore(Diagnostics));
		TestTrue(TEXT("the error says the probe created it"), Diagnostics.ContainsByPredicate(
			[&CreatedPackageName](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
			{
				return Diagnostic.Code == TEXT("PreflightIsolation.PackageNotRestored")
					&& Diagnostic.Message.Contains(CreatedPackageName) && Diagnostic.Message.Contains(TEXT("created this package"));
			}));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericRollbackPreflightLeavesOtherAssetsTest,
	"UEAssetDoc.GenericRuntime.Rollback.PreflightLeavesOtherAssetsUnchanged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericRollbackPreflightLeavesOtherAssetsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericRollbackTests;
	if (!HasPaper2D(*this))
	{
		return true;
	}
	const FTarget Atlas(TEXT("Atlas_Refused"), AtlasClass);
	const FTarget Sprite(TEXT("Sprite_Refused"));
	ON_SCOPE_EXIT { Sprite.Cleanup(); Atlas.Cleanup(); };
	if (!CreateAtlasAndSprite(*this, Atlas, Sprite))
	{
		return false;
	}
	UObject* const AtlasAsset = FSoftObjectPath(Atlas.ObjectPath).ResolveObject();
	UObject* const SpriteAsset = FSoftObjectPath(Sprite.ObjectPath).ResolveObject();
	const TArray<uint8> AtlasBefore = SerializeState(*AtlasAsset);
	const FString SpriteBefore = ExtractProperties(Sprite, {TEXT("BodySetup")});

	// Preflight runs per property in name order: AtlasGroup is simulated on the
	// scratch (its PostEditChange rebuilds the real atlas), then the unnamed socket
	// is refused, so the apply never reaches the live write.
	const FString Properties = FString::Printf(
		TEXT("{\"AtlasGroup\":\"%s\",\"Sockets\":[{\"LocalTransform\":{\"Rotation\":{\"X\":0,\"Y\":0,\"Z\":0,\"W\":1},")
		TEXT("\"Translation\":{\"X\":0,\"Y\":0,\"Z\":0},\"Scale3D\":{\"X\":1,\"Y\":1,\"Z\":1}},\"SocketName\":\"None\"}]}"),
		*Atlas.ObjectPath);
	const FAssetDocumentResult Refused = ApplyJson(*this, Sprite, Sprite.Document(TEXT("Update"), Properties), TEXT("refused"));
	TestTrue(TEXT("refused before mutation"), HasCode(Refused, TEXT("Generic.PropertyValueNotStoredExactly")));
	TestTrue(TEXT("refusal names the socket sub-path"), Refused.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Message.Contains(TEXT("/Properties/Sockets/0/SocketName"));
	}));
	TestRolledBack(*this, Sprite, Refused, SpriteBefore, SpriteAsset, TEXT("refused"), {TEXT("BodySetup")});
	TestFalse(TEXT("atlas package stays clean"), Atlas.FindLivePackage()->IsDirty());
	TestTrue(TEXT("atlas state is unchanged"), SerializeState(*AtlasAsset) == AtlasBefore);
	return true;
}
#endif
