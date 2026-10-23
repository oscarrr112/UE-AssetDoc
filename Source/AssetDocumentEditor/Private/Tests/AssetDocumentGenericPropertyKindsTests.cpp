// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Production-service E2E coverage of the generic property kinds the typed value
// codec admits beyond plain scalars: hard object / class references, instanced
// subobjects (arrays of them included), edit-inline references, FText and structs with a native text
// form. Every case runs on a real Engine/plugin asset class through
// FAssetDocumentModule::Get().GetService(): Extract shows the value, an edit
// applies and round-trips, values survive unload/reload, an unchanged re-apply
// saves nothing, invalid values fail with a located diagnostic, and an injected
// post-apply failure rolls the edit back.

#include "AssetDocumentModule.h"
#include "AssetDocumentTestDirectoryBlock.h"
#include "AssetDocumentService.h"

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
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"
#include "Internationalization/Text.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentGenericPropertyKindsTests
{
struct FFixture
{
	FString AssetName;
	FString PackageName;
	FString ObjectPath;
	FString PackageFile;
	FString SidecarPath;

	explicit FFixture(const TCHAR* Prefix)
		: AssetName(FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits)))
	{
		PackageName = FString(TEXT("/Game/AssetDocumentPropertyKinds/")) + AssetName;
		ObjectPath = PackageName + TEXT(".") + AssetName;
		PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		SidecarPath = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("AssetDocumentPropertyKinds"),
			AssetName + TEXT(".assetdoc.json"));
	}

	UObject* Asset() const
	{
		return FSoftObjectPath(ObjectPath).ResolveObject();
	}

	void Unload() const
	{
		if (UObject* Loaded = Asset())
		{
			UPackageTools::UnloadPackages({Loaded->GetOutermost()});
		}
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}

	void Cleanup() const
	{
		UObject* Loaded = Asset();
		if (IFileManager::Get().FileExists(*PackageFile))
		{
			if (Loaded != nullptr)
			{
				UPackageTools::UnloadPackages({Loaded->GetOutermost()});
				CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			}
			IFileManager::Get().Delete(*PackageFile, false, true);
			FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({PackageFile});
		}
		else if (Loaded != nullptr)
		{
			ObjectTools::DeleteObjects({Loaded}, false, ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
		}
		IFileManager::Get().Delete(*SidecarPath, false, true);
	}

	FString Document(const TCHAR* Class, const TCHAR* Action, const FString& Properties) const
	{
		return FString::Printf(
			TEXT("{\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\",\"Target\":\"%s\",\"Class\":\"%s\",")
			TEXT("\"Action\":\"%s\",\"Properties\":%s}"),
			*PackageName, Class, Action, *Properties);
	}
};

// Makes package staging fail after the Apply phase wrote the properties, so the
// transaction must roll them back (same mechanism as GenericRuntime.AllPhaseRollback).
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

void Report(FAutomationTestBase& Test, const FAssetDocumentResult& Result, const FString& Stage)
{
	Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *Result.Message));
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Test.AddInfo(FString::Printf(TEXT("%s: [%s] %s %s"), *Stage, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
	}
}

FAssetDocumentResult Apply(FAutomationTestBase& Test, const FFixture& Fixture, const FString& Json, const FString& Stage)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Fixture.SidecarPath), true);
	Test.TestTrue(*FString::Printf(TEXT("%s writes sidecar"), *Stage),
		FFileHelper::SaveStringToFile(Json, *Fixture.SidecarPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Fixture.SidecarPath;
	Request.bSaveAsset = true;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
	Report(Test, Result, Stage);
	return Result;
}

FString Serialize(const TSharedPtr<FJsonValue>& Value)
{
	FString Text;
	if (Value.IsValid())
	{
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Value.ToSharedRef(), FString(), Writer);
	}
	return Text;
}

TSharedPtr<FJsonValue> Parse(const FString& Json)
{
	TSharedPtr<FJsonValue> Value;
	const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Json);
	FJsonSerializer::Deserialize(Reader, Value);
	return Value;
}

TSharedPtr<FJsonObject> ExtractProperties(FAutomationTestBase& Test, const FFixture& Fixture, const FString& Stage)
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = Fixture.ObjectPath;
	Request.bDiffOnly = false;
	Request.bIncludeAllWritable = true;
	const FAssetDocumentResult Extracted = FAssetDocumentModule::Get().GetService().Extract(Request);
	const TSharedPtr<FJsonObject>* Properties = nullptr;
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: extract succeeds"), *Stage), Extracted.IsSuccess()
		&& Extracted.Payload.IsValid() && Extracted.Payload->TryGetObjectField(TEXT("Properties"), Properties)))
	{
		Report(Test, Extracted, Stage);
		return nullptr;
	}
	return *Properties;
}

// Every authored property reads back exactly as written.
void TestExtractShows(FAutomationTestBase& Test, const FFixture& Fixture, const FString& AuthoredJson, const FString& Stage)
{
	const TSharedPtr<FJsonObject> Properties = ExtractProperties(Test, Fixture, Stage);
	const TSharedPtr<FJsonValue> Authored = Parse(AuthoredJson);
	if (!Properties.IsValid() || !Test.TestTrue(*FString::Printf(TEXT("%s: authored JSON parses"), *Stage),
		Authored.IsValid() && Authored->Type == EJson::Object))
	{
		return;
	}
	for (const auto& Entry : Authored->AsObject()->Values)
	{
		const TSharedPtr<FJsonValue> Actual = Properties->TryGetField(Entry.Key);
		Test.TestTrue(*FString::Printf(TEXT("%s: %s reads back %s (got %s)"), *Stage, *Entry.Key,
			*Serialize(Entry.Value), *Serialize(Actual)),
			Actual.IsValid() && FJsonValue::CompareEqual(*Actual, *Entry.Value));
	}
}

FString ExtractSnapshot(FAutomationTestBase& Test, const FFixture& Fixture, const FString& Stage)
{
	const TSharedPtr<FJsonObject> Properties = ExtractProperties(Test, Fixture, Stage);
	return Properties.IsValid() ? Serialize(MakeShared<FJsonValueObject>(Properties)) : FString();
}

// The current full extract re-applied verbatim saves nothing and changes nothing.
void TestUnchangedReapply(FAutomationTestBase& Test, const FFixture& Fixture, const TCHAR* Class)
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = Fixture.ObjectPath;
	Request.bDiffOnly = false;
	Request.bIncludeAllWritable = true;
	const FAssetDocumentResult Extracted = FAssetDocumentModule::Get().GetService().Extract(Request);
	if (!Test.TestTrue(TEXT("no-op: full extract succeeds"), Extracted.IsSuccess() && Extracted.Payload.IsValid()))
	{
		return;
	}
	const FString Before = Serialize(MakeShared<FJsonValueObject>(Extracted.Payload));
	const FDateTime PackageTime = IFileManager::Get().GetTimeStamp(*Fixture.PackageFile);
	const FAssetDocumentResult Result = Apply(Test, Fixture, Before, TEXT("UnchangedReapply"));
	Test.TestTrue(*FString::Printf(TEXT("%s: unchanged re-apply succeeds"), Class), Result.IsSuccess());
	Test.TestFalse(*FString::Printf(TEXT("%s: unchanged re-apply saves nothing"), Class), Result.bSavedAsset);
	Test.TestEqual(*FString::Printf(TEXT("%s: package file untouched"), Class),
		IFileManager::Get().GetTimeStamp(*Fixture.PackageFile), PackageTime);
	const UPackage* Package = FindPackage(nullptr, *Fixture.PackageName);
	Test.TestTrue(*FString::Printf(TEXT("%s: package stays clean"), Class), Package != nullptr && !Package->IsDirty());
}

void TestReload(FAutomationTestBase& Test, const FFixture& Fixture, const FString& Expected)
{
	const TWeakObjectPtr<UObject> Before(Fixture.Asset());
	Fixture.Unload();
	Test.TestFalse(TEXT("reload: the asset was unloaded"), Before.IsValid());
	Test.TestEqual(TEXT("reload: extract after unload/reload is unchanged"),
		ExtractSnapshot(Test, Fixture, TEXT("AfterReload")), Expected);
}

struct FInvalidCase
{
	FString Properties;
	FString Path;
	FString MessageText;
};

void ExpectInvalid(FAutomationTestBase& Test, const FFixture& Fixture, const TCHAR* Class, const FInvalidCase& Case)
{
	const FString Before = ExtractSnapshot(Test, Fixture, TEXT("BeforeInvalid"));
	const FAssetDocumentResult Result =
		Apply(Test, Fixture, Fixture.Document(Class, TEXT("Update"), Case.Properties), Case.Path);
	Test.TestFalse(*FString::Printf(TEXT("invalid %s is rejected"), *Case.Properties), Result.IsSuccess());
	Test.TestFalse(*FString::Printf(TEXT("invalid %s saves nothing"), *Case.Properties), Result.bSavedAsset);
	const FAssetDocumentDiagnostic* Located = Result.Diagnostics.FindByPredicate(
		[&Case](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Path == Case.Path; });
	if (Test.TestNotNull(*FString::Printf(TEXT("diagnostic is located at %s"), *Case.Path), Located))
	{
		Test.TestTrue(*FString::Printf(TEXT("diagnostic at %s says '%s' (was '%s')"), *Case.Path, *Case.MessageText,
			*Located->Message), Located->Message.Contains(Case.MessageText));
	}
	Test.TestEqual(*FString::Printf(TEXT("invalid %s leaves the asset unchanged"), *Case.Properties),
		ExtractSnapshot(Test, Fixture, TEXT("AfterInvalid")), Before);
}

// An edit whose transaction fails after Apply (package staging blocked) restores the asset.
void TestRollback(FAutomationTestBase& Test, const FFixture& Fixture, const TCHAR* Class, const FString& Properties,
	TFunctionRef<void(const FString& Stage)> CheckLiveState)
{
	const FString Before = ExtractSnapshot(Test, Fixture, TEXT("BeforeRollback"));
	CheckLiveState(TEXT("before rollback"));
	FStagingBlocker Blocker;
	if (!Test.TestTrue(TEXT("staging root can be blocked"), Blocker.Block()))
	{
		return;
	}
	const FAssetDocumentResult Rejected =
		Apply(Test, Fixture, Fixture.Document(Class, TEXT("Update"), Properties), TEXT("InjectedStageFailure"));
	Blocker.Restore();
	Test.TestFalse(TEXT("rollback: the injected failure fails the apply"), Rejected.IsSuccess());
	Test.TestTrue(TEXT("rollback: failure is the injected staging failure"), Rejected.Diagnostics.ContainsByPredicate(
		[](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == TEXT("Persistence.StagePackageFailed"); }));
	Test.TestEqual(TEXT("rollback: extract equals the pre-apply state"),
		ExtractSnapshot(Test, Fixture, TEXT("AfterRollback")), Before);
	CheckLiveState(TEXT("after rollback"));
}

FProperty* FindProperty(const UObject& Object, const TCHAR* Name)
{
	return FindFProperty<FProperty>(Object.GetClass(), Name);
}

UObject* ReadObject(const UObject& Object, const TCHAR* Name)
{
	const FObjectPropertyBase* Property = CastField<FObjectPropertyBase>(FindProperty(Object, Name));
	return Property != nullptr ? Property->GetObjectPropertyValue_InContainer(&Object) : nullptr;
}

TArray<UObject*> ReadObjectArray(const UObject& Object, const TCHAR* Name)
{
	TArray<UObject*> Out;
	const FArrayProperty* Array = CastField<FArrayProperty>(FindProperty(Object, Name));
	const FObjectPropertyBase* Inner = Array != nullptr ? CastField<FObjectPropertyBase>(Array->Inner) : nullptr;
	if (Inner != nullptr)
	{
		FScriptArrayHelper Helper(Array, Array->ContainerPtrToValuePtr<void>(&Object));
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			Out.Add(Inner->GetObjectPropertyValue(Helper.GetRawPtr(Index)));
		}
	}
	return Out;
}

float ReadFloat(const UObject& Object, const TCHAR* Name)
{
	const FFloatProperty* Property = CastField<FFloatProperty>(FindProperty(Object, Name));
	return Property != nullptr ? Property->GetPropertyValue_InContainer(&Object) : -1.0f;
}

FString ReadText(const UObject& Object, const TCHAR* Name)
{
	const FTextProperty* Property = CastField<FTextProperty>(FindProperty(Object, Name));
	return Property != nullptr ? Property->GetPropertyValue_InContainer(&Object).ToString() : FString(TEXT("<missing>"));
}

// Follows Name[/Index]... through struct members and arrays to the object a
// pointer property holds (e.g. DefaultKeyMappings/Mappings/0/Triggers/0).
UObject* ReadObjectAt(const UObject& Object, const FString& Path)
{
	TArray<FString> Segments;
	Path.ParseIntoArray(Segments, TEXT("/"));
	const UStruct* Struct = Object.GetClass();
	const void* Container = &Object;
	const FProperty* Property = nullptr;
	const void* Value = nullptr;
	for (const FString& Segment : Segments)
	{
		if (const FArrayProperty* Array = CastField<FArrayProperty>(Property); Array != nullptr && Segment.IsNumeric())
		{
			FScriptArrayHelper Helper(Array, Value);
			const int32 Index = FCString::Atoi(*Segment);
			if (!Helper.IsValidIndex(Index))
			{
				return nullptr;
			}
			Property = Array->Inner;
			Value = Helper.GetRawPtr(Index);
			continue;
		}
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			Struct = StructProperty->Struct;
			Container = Value;
		}
		else if (Property != nullptr)
		{
			return nullptr;
		}
		Property = FindFProperty<FProperty>(Struct, *Segment);
		if (Property == nullptr)
		{
			return nullptr;
		}
		Value = Property->ContainerPtrToValuePtr<void>(Container);
	}
	const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property);
	return ObjectProperty != nullptr ? ObjectProperty->GetObjectPropertyValue(Value) : nullptr;
}

bool ReadBool(const UObject& Object, const TCHAR* Name)
{
	const FBoolProperty* Property = CastField<FBoolProperty>(FindProperty(Object, Name));
	return Property != nullptr && Property->GetPropertyValue_InContainer(&Object);
}

FText ReadTextValue(const UObject& Object, const TCHAR* Name)
{
	const FTextProperty* Property = CastField<FTextProperty>(FindProperty(Object, Name));
	return Property != nullptr ? Property->GetPropertyValue_InContainer(&Object) : FText::GetEmpty();
}

// Applies a document and checks that it succeeds and whether it saved.
FAssetDocumentResult ApplyUpdate(FAutomationTestBase& Test, const FFixture& Fixture, const TCHAR* Class,
	const FString& Properties, const FString& Stage, bool bExpectSave)
{
	const FAssetDocumentResult Result = Apply(Test, Fixture, Fixture.Document(Class, TEXT("Update"), Properties), Stage);
	Test.TestTrue(*FString::Printf(TEXT("%s: update succeeds"), *Stage), Result.IsSuccess());
	Test.TestEqual(*FString::Printf(TEXT("%s: saves the asset"), *Stage), Result.bSavedAsset, bExpectSave);
	return Result;
}

// Diff of the document against the live asset reports no change.
void TestNoDiff(FAutomationTestBase& Test, const FFixture& Fixture, const TCHAR* Class, const FString& Properties,
	const FString& Stage)
{
	FAssetDocumentDiffRequest Request;
	const TSharedPtr<FJsonValue> Document = Parse(Fixture.Document(Class, TEXT("Update"), Properties));
	Request.Document = Document.IsValid() ? Document->AsObject() : nullptr;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Diff(Request);
	const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: diff succeeds"), *Stage), Result.IsSuccess() && Result.Payload.IsValid()
		&& Result.Payload->TryGetArrayField(TEXT("changed"), Changed)))
	{
		Report(Test, Result, Stage);
		return;
	}
	if (!Test.TestEqual(*FString::Printf(TEXT("%s: diff reports no change"), *Stage), Changed->Num(), 0))
	{
		Test.AddInfo(Serialize(MakeShared<FJsonValueObject>(Result.Payload)));
	}
}

// Temporarily adds property metadata (e.g. NoClear) the way a UPROPERTY specifier would.
class FScopedPropertyMetadata
{
public:
	FScopedPropertyMetadata(FProperty* InProperty, const TCHAR* InKey, const TCHAR* Value)
		: Property(InProperty), Key(InKey)
	{
		if (Property != nullptr)
		{
			bHad = Property->HasMetaData(*Key);
			Previous = Property->GetMetaData(*Key);
			Property->SetMetaData(*Key, Value);
		}
	}

	~FScopedPropertyMetadata()
	{
		if (Property == nullptr)
		{
			return;
		}
		if (bHad)
		{
			Property->SetMetaData(*Key, *Previous);
		}
		else
		{
			Property->RemoveMetaData(*Key);
		}
	}

private:
	FProperty* Property = nullptr;
	FString Key;
	FString Previous;
	bool bHad = false;
};

bool CreateEmpty(FAutomationTestBase& Test, const FFixture& Fixture, const TCHAR* Class)
{
	const FAssetDocumentResult Created = Apply(Test, Fixture, Fixture.Document(Class, TEXT("Create"), TEXT("{}")), TEXT("Create"));
	return Test.TestTrue(FString::Printf(TEXT("%s is created"), Class), Created.IsSuccess() && Fixture.Asset() != nullptr);
}

bool Update(FAutomationTestBase& Test, const FFixture& Fixture, const TCHAR* Class, const FString& Properties,
	const FString& Stage)
{
	const FAssetDocumentResult Updated = Apply(Test, Fixture, Fixture.Document(Class, TEXT("Update"), Properties), Stage);
	Test.TestTrue(*FString::Printf(TEXT("%s: update succeeds"), *Stage), Updated.IsSuccess());
	Test.TestTrue(*FString::Printf(TEXT("%s: update saves the asset"), *Stage), Updated.bSavedAsset);
	TestExtractShows(Test, Fixture, Properties, Stage);
	return Updated.IsSuccess();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericPropertyKindsHardReferenceTest,
	"UEAssetDoc.GenericRuntime.PropertyKinds.HardObjectReference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericPropertyKindsHardReferenceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentGenericPropertyKindsTests;
	const TCHAR* Class = TEXT("/Script/Engine.HapticFeedbackEffect_SoundWave");
	const TCHAR* Wave = TEXT("/Engine/EngineSounds/1kSineTonePing.1kSineTonePing");
	const FFixture Fixture(TEXT("Haptic"));
	ON_SCOPE_EXIT { Fixture.Cleanup(); };
	if (!CreateEmpty(*this, Fixture, Class))
	{
		return false;
	}
	TestExtractShows(*this, Fixture, TEXT("{\"SoundWave\":null}"), TEXT("AfterCreate"));
	const FString Edit = FString::Printf(TEXT("{\"SoundWave\":\"%s\"}"), Wave);
	if (!Update(*this, Fixture, Class, Edit, TEXT("SetReference")))
	{
		return false;
	}
	UObject* Referenced = ReadObject(*Fixture.Asset(), TEXT("SoundWave"));
	TestTrue(TEXT("the property points at the loaded asset"), Referenced != nullptr && Referenced->GetPathName() == Wave);
	const FString Expected = ExtractSnapshot(*this, Fixture, TEXT("BeforeReload"));
	TestReload(*this, Fixture, Expected);
	TestUnchangedReapply(*this, Fixture, Class);

	ExpectInvalid(*this, Fixture, Class, {TEXT("{\"SoundWave\":\"/Engine/EngineSounds/Master.Master\"}"),
		TEXT("/Properties/SoundWave"), TEXT("requires a SoundWave")});
	ExpectInvalid(*this, Fixture, Class, {TEXT("{\"SoundWave\":\"/Game/AssetDocumentPropertyKinds/Missing.Missing\"}"),
		TEXT("/Properties/SoundWave"), TEXT("does not resolve to an existing object")});
	ExpectInvalid(*this, Fixture, Class, {TEXT("{\"SoundWave\":\"/Engine/EngineSounds/1kSineTonePing\"}"),
		TEXT("/Properties/SoundWave"), TEXT("exact object path")});
	ExpectInvalid(*this, Fixture, Class, {TEXT("{\"SoundWave\":3}"),
		TEXT("/Properties/SoundWave"), TEXT("expected an object path string")});

	TestRollback(*this, Fixture, Class, TEXT("{\"SoundWave\":null}"), [&](const FString& Stage)
	{
		const UObject* Current = ReadObject(*Fixture.Asset(), TEXT("SoundWave"));
		TestTrue(*FString::Printf(TEXT("%s: the reference is set"), *Stage), Current != nullptr && Current->GetPathName() == Wave);
	});
	Update(*this, Fixture, Class, TEXT("{\"SoundWave\":null}"), TEXT("ClearReference"));
	TestNull(TEXT("null clears the reference"), ReadObject(*Fixture.Asset(), TEXT("SoundWave")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericPropertyKindsClassReferenceTest,
	"UEAssetDoc.GenericRuntime.PropertyKinds.ClassAndObjectArrayReference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericPropertyKindsClassReferenceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentGenericPropertyKindsTests;
	const TCHAR* Class = TEXT("/Script/Engine.ObjectLibrary");
	const FFixture Fixture(TEXT("ObjLib"));
	ON_SCOPE_EXIT { Fixture.Cleanup(); };
	if (!CreateEmpty(*this, Fixture, Class))
	{
		return false;
	}
	TestExtractShows(*this, Fixture, TEXT("{\"ObjectBaseClass\":null,\"Objects\":[]}"), TEXT("AfterCreate"));
	const FString Edit = TEXT("{\"ObjectBaseClass\":\"/Script/Engine.Texture2D\",")
		TEXT("\"Objects\":[\"/Engine/EngineResources/DefaultTexture.DefaultTexture\",\"/Engine/EngineResources/Black.Black\"]}");
	if (!Update(*this, Fixture, Class, Edit, TEXT("SetClassAndObjects")))
	{
		return false;
	}
	const FString Expected = ExtractSnapshot(*this, Fixture, TEXT("BeforeReload"));
	TestReload(*this, Fixture, Expected);
	TestUnchangedReapply(*this, Fixture, Class);

	ExpectInvalid(*this, Fixture, Class, {TEXT("{\"ObjectBaseClass\":\"/Engine/EngineResources/Black.Black\"}"),
		TEXT("/Properties/ObjectBaseClass"), TEXT("is not a subclass of")});
	ExpectInvalid(*this, Fixture, Class, {TEXT("{\"Objects\":[\"/Engine/EngineResources/Black.Black\",\"/Script/Engine.Nope\"]}"),
		TEXT("/Properties/Objects/1"), TEXT("does not resolve")});

	TestRollback(*this, Fixture, Class, TEXT("{\"ObjectBaseClass\":\"/Script/Engine.StaticMesh\",\"Objects\":[]}"),
		[&](const FString& Stage)
	{
		const UObject* BaseClass = ReadObject(*Fixture.Asset(), TEXT("ObjectBaseClass"));
		TestTrue(*FString::Printf(TEXT("%s: class reference is Texture2D"), *Stage),
			BaseClass != nullptr && BaseClass->GetPathName() == TEXT("/Script/Engine.Texture2D"));
		TestEqual(*FString::Printf(TEXT("%s: two object references"), *Stage),
			ReadObjectArray(*Fixture.Asset(), TEXT("Objects")).Num(), 2);
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericPropertyKindsInstancedTest,
	"UEAssetDoc.GenericRuntime.PropertyKinds.InstancedSubobjectsAndText",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericPropertyKindsInstancedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentGenericPropertyKindsTests;
	const TCHAR* Class = TEXT("/Script/EnhancedInput.InputAction");
	if (FSoftObjectPath(Class).ResolveObject() == nullptr)
	{
		AddWarning(TEXT("EnhancedInput is not enabled in this project; skipping."));
		return true;
	}
	const FFixture Fixture(TEXT("IA"));
	ON_SCOPE_EXIT { Fixture.Cleanup(); };
	if (!CreateEmpty(*this, Fixture, Class))
	{
		return false;
	}
	TestExtractShows(*this, Fixture, TEXT("{\"Triggers\":[],\"ActionDescription\":\"\"}"), TEXT("AfterCreate"));
	const FString Edit = TEXT("{\"ActionDescription\":\"Jump over things\",\"Triggers\":[")
		TEXT("{\"Class\":\"/Script/EnhancedInput.InputTriggerHold\",\"Properties\":{\"HoldTimeThreshold\":1.5}},")
		TEXT("{\"Class\":\"/Script/EnhancedInput.InputTriggerPressed\",\"Properties\":{}}]}");
	if (!Update(*this, Fixture, Class, Edit, TEXT("CreateSubobjects")))
	{
		return false;
	}
	UObject* Asset = Fixture.Asset();
	TArray<UObject*> Triggers = ReadObjectArray(*Asset, TEXT("Triggers"));
	if (!TestEqual(TEXT("two trigger subobjects"), Triggers.Num(), 2) || Triggers[0] == nullptr || Triggers[1] == nullptr)
	{
		return false;
	}
	for (UObject* Trigger : Triggers)
	{
		TestTrue(TEXT("trigger is owned by the asset (Details class-picker outer)"), Trigger->GetOuter() == Asset);
		TestFalse(TEXT("trigger is not transient"), Trigger->HasAnyFlags(RF_Transient));
	}
	TestEqual(TEXT("first trigger is Hold"), Triggers[0]->GetClass()->GetName(), FString(TEXT("InputTriggerHold")));
	TestEqual(TEXT("hold threshold is written"), ReadFloat(*Triggers[0], TEXT("HoldTimeThreshold")), 1.5f);
	TestEqual(TEXT("text is written"), ReadText(*Asset, TEXT("ActionDescription")), FString(TEXT("Jump over things")));

	// Editing a field of an existing subobject keeps the subobject (as the Details panel does).
	// Held weakly: the replaced subobject is garbage once an apply returns (its staged copies are purged then).
	const TWeakObjectPtr<UObject> Hold = Triggers[0];
	const FString FieldEdit = TEXT("{\"Triggers\":[")
		TEXT("{\"Class\":\"/Script/EnhancedInput.InputTriggerHold\",\"Properties\":{\"HoldTimeThreshold\":2.5}},")
		TEXT("{\"Class\":\"/Script/EnhancedInput.InputTriggerPressed\",\"Properties\":{}}]}");
	Update(*this, Fixture, Class, FieldEdit, TEXT("EditSubobjectField"));
	Triggers = ReadObjectArray(*Fixture.Asset(), TEXT("Triggers"));
	TestTrue(TEXT("field edit keeps the same subobject"), Triggers.Num() == 2 && Triggers[0] == Hold.Get());
	TestEqual(TEXT("field edit writes the value"), Hold.IsValid() ? ReadFloat(*Hold.Get(), TEXT("HoldTimeThreshold")) : 0.0f, 2.5f);

	// Changing the class replaces the subobject and moves the old one out of the asset.
	const FString ClassEdit = TEXT("{\"Triggers\":[")
		TEXT("{\"Class\":\"/Script/EnhancedInput.InputTriggerTap\",\"Properties\":{}},")
		TEXT("{\"Class\":\"/Script/EnhancedInput.InputTriggerPressed\",\"Properties\":{}}]}");
	Update(*this, Fixture, Class, ClassEdit, TEXT("ReplaceSubobjectClass"));
	Triggers = ReadObjectArray(*Fixture.Asset(), TEXT("Triggers"));
	TestTrue(TEXT("class change creates a new subobject"), Triggers.Num() == 2 && Triggers[0] != Hold.Get()
		&& Triggers[0] != nullptr && Triggers[0]->GetClass()->GetName() == TEXT("InputTriggerTap"));
	TestFalse(TEXT("replaced subobject no longer lives in the asset"), Hold.IsValid() && Hold->IsIn(Fixture.Asset()));

	const FString Expected = ExtractSnapshot(*this, Fixture, TEXT("BeforeReload"));
	TestReload(*this, Fixture, Expected);
	TestUnchangedReapply(*this, Fixture, Class);

	ExpectInvalid(*this, Fixture, Class, {TEXT("{\"Triggers\":[{\"Class\":\"/Script/EnhancedInput.InputTrigger\",\"Properties\":{}}]}"),
		TEXT("/Properties/Triggers/0/Class"), TEXT("abstract")});
	ExpectInvalid(*this, Fixture, Class, {TEXT("{\"Triggers\":[{\"Class\":\"/Script/EnhancedInput.InputModifierNegate\"}]}"),
		TEXT("/Properties/Triggers/0/Class"), TEXT("is not a subclass of")});
	ExpectInvalid(*this, Fixture, Class,
		{TEXT("{\"Triggers\":[{\"Class\":\"/Script/EnhancedInput.InputTriggerHold\",\"Properties\":{\"Nope\":1}}]}"),
		TEXT("/Properties/Triggers/0/Properties/Nope"), TEXT("HoldTimeThreshold")});
	ExpectInvalid(*this, Fixture, Class, {TEXT("{\"Triggers\":[\"/Script/EnhancedInput.InputTriggerHold\"]}"),
		TEXT("/Properties/Triggers/0"), TEXT("{\"Class\"")});
	ExpectInvalid(*this, Fixture, Class, {TEXT("{\"Triggers\":[{\"Class\":\"/Script/EnhancedInput.InputTriggerPressed\",\"Properties\":{}},null]}"),
		TEXT("/Properties/Triggers/1"), TEXT("cannot be null")});

	TestRollback(*this, Fixture, Class, TEXT("{\"ActionDescription\":\"changed\",\"Triggers\":[]}"), [&](const FString& Stage)
	{
		const TArray<UObject*> Live = ReadObjectArray(*Fixture.Asset(), TEXT("Triggers"));
		TestTrue(*FString::Printf(TEXT("%s: two triggers, Tap first"), *Stage), Live.Num() == 2 && Live[0] != nullptr
			&& Live[0]->GetClass()->GetName() == TEXT("InputTriggerTap") && Live[0]->GetOuter() == Fixture.Asset());
		TestEqual(*FString::Printf(TEXT("%s: text"), *Stage), ReadText(*Fixture.Asset(), TEXT("ActionDescription")),
			FString(TEXT("Jump over things")));
	});

	Update(*this, Fixture, Class, TEXT("{\"Triggers\":[],\"ActionDescription\":\"\"}"), TEXT("ClearSubobjects"));
	TestEqual(TEXT("clearing removes every trigger"), ReadObjectArray(*Fixture.Asset(), TEXT("Triggers")).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericPropertyKindsStructMembersTest,
	"UEAssetDoc.GenericRuntime.PropertyKinds.StructMembersAndTextForm",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// InputMappingContext.DefaultKeyMappings.Mappings[]: a struct array whose members
// are a hard reference to another (freshly created) asset, an FKey (struct with a
// native text form), an enum, an instanced subobject array and an instanced
// subobject; plus a top-level FText.
bool FAssetDocumentGenericPropertyKindsStructMembersTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentGenericPropertyKindsTests;
	const TCHAR* ActionClass = TEXT("/Script/EnhancedInput.InputAction");
	const TCHAR* Class = TEXT("/Script/EnhancedInput.InputMappingContext");
	const FFixture ActionFixture(TEXT("IMCAction"));
	const FFixture Fixture(TEXT("IMC"));
	ON_SCOPE_EXIT
	{
		Fixture.Cleanup();
		ActionFixture.Cleanup();
	};
	if (!CreateEmpty(*this, ActionFixture, ActionClass) || !CreateEmpty(*this, Fixture, Class))
	{
		return false;
	}
	TestExtractShows(*this, Fixture, TEXT("{\"DefaultKeyMappings\":{\"Mappings\":[]},\"ContextDescription\":\"\"}"),
		TEXT("AfterCreate"));
	const auto Mapping = [&ActionFixture](const TCHAR* Key, const TCHAR* Triggers)
	{
		return FString::Printf(
			TEXT("{\"Triggers\":%s,\"Modifiers\":[],\"Action\":\"%s\",\"Key\":\"%s\",")
			TEXT("\"SettingBehavior\":\"InheritSettingsFromAction\",\"PlayerMappableKeySettings\":null}"),
			Triggers, *ActionFixture.ObjectPath, Key);
	};
	const TCHAR* Pressed = TEXT("[{\"Class\":\"/Script/EnhancedInput.InputTriggerPressed\",\"Properties\":{}}]");
	const FString Edit = FString::Printf(TEXT("{\"DefaultKeyMappings\":{\"Mappings\":[%s,%s]},\"ContextDescription\":\"Move\"}"),
		*Mapping(TEXT("SpaceBar"), Pressed), *Mapping(TEXT("Gamepad_FaceButton_Bottom"), TEXT("[]")));
	if (!Update(*this, Fixture, Class, Edit, TEXT("SetMappings")))
	{
		return false;
	}
	const FString Expected = ExtractSnapshot(*this, Fixture, TEXT("BeforeReload"));
	TestReload(*this, Fixture, Expected);
	TestUnchangedReapply(*this, Fixture, Class);

	ExpectInvalid(*this, Fixture, Class, {FString::Printf(TEXT("{\"DefaultKeyMappings\":{\"Mappings\":[%s]}}"),
		*Mapping(TEXT("SpaceBar"), TEXT("[]")).Replace(TEXT("\"Key\":\"SpaceBar\""), TEXT("\"Key\":{\"KeyName\":\"SpaceBar\"}"))),
		TEXT("/Properties/DefaultKeyMappings/Mappings/0/Key"), TEXT("text form as a string")});
	ExpectInvalid(*this, Fixture, Class, {FString::Printf(TEXT("{\"DefaultKeyMappings\":{\"Mappings\":[%s]}}"),
		*Mapping(TEXT("SpaceBar"), TEXT("[]")).Replace(*ActionFixture.ObjectPath, TEXT("/Engine/EngineResources/Black.Black"))),
		TEXT("/Properties/DefaultKeyMappings/Mappings/0/Action"), TEXT("requires a InputAction")});
	ExpectInvalid(*this, Fixture, Class, {FString::Printf(TEXT("{\"DefaultKeyMappings\":{\"Mappings\":[%s]}}"),
		*Mapping(TEXT("SpaceBar"), TEXT("[{\"Class\":\"/Script/EnhancedInput.InputModifierNegate\",\"Properties\":{}}]"))),
		TEXT("/Properties/DefaultKeyMappings/Mappings/0/Triggers/0/Class"), TEXT("InputTrigger")});

	TestRollback(*this, Fixture, Class, TEXT("{\"DefaultKeyMappings\":{\"Mappings\":[]},\"ContextDescription\":\"\"}"),
		[&](const FString& Stage)
	{
		const TSharedPtr<FJsonObject> Live = ExtractProperties(*this, Fixture, Stage);
		const TSharedPtr<FJsonObject>* Mappings = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		TestTrue(*FString::Printf(TEXT("%s: two mappings"), *Stage), Live.IsValid()
			&& Live->TryGetObjectField(TEXT("DefaultKeyMappings"), Mappings)
			&& (*Mappings)->TryGetArrayField(TEXT("Mappings"), Rows) && Rows->Num() == 2);
		TestEqual(*FString::Printf(TEXT("%s: text"), *Stage), ReadText(*Fixture.Asset(), TEXT("ContextDescription")),
			FString(TEXT("Move")));
	});
	Update(*this, Fixture, Class, TEXT("{\"DefaultKeyMappings\":{\"Mappings\":[]}}"), TEXT("ClearMappings"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericPropertyKindsPartialValuesTest,
	"UEAssetDoc.GenericRuntime.PropertyKinds.PartialValues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// The document rule "omitted = keep the current value" inside values: struct
// members and instanced subobject Properties that a document leaves out keep
// their current value; a class change starts the new subobject from its CDO.
bool FAssetDocumentGenericPropertyKindsPartialValuesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentGenericPropertyKindsTests;
	const TCHAR* ActionClass = TEXT("/Script/EnhancedInput.InputAction");
	const TCHAR* Class = TEXT("/Script/EnhancedInput.InputMappingContext");
	if (FSoftObjectPath(Class).ResolveObject() == nullptr)
	{
		AddWarning(TEXT("EnhancedInput is not enabled in this project; skipping."));
		return true;
	}
	const FFixture ActionFixture(TEXT("PartialAction"));
	const FFixture Fixture(TEXT("PartialIMC"));
	ON_SCOPE_EXIT
	{
		Fixture.Cleanup();
		ActionFixture.Cleanup();
	};
	if (!CreateEmpty(*this, ActionFixture, ActionClass) || !CreateEmpty(*this, Fixture, Class))
	{
		return false;
	}
	const auto Mapping = [&ActionFixture](const TCHAR* Key, const TCHAR* Triggers)
	{
		return FString::Printf(
			TEXT("{\"Triggers\":%s,\"Modifiers\":[],\"Action\":\"%s\",\"Key\":\"%s\",")
			TEXT("\"SettingBehavior\":\"InheritSettingsFromAction\",\"PlayerMappableKeySettings\":null}"),
			Triggers, *ActionFixture.ObjectPath, Key);
	};
	const auto Mappings = [](const FString& First, const FString& Second)
	{
		return FString::Printf(TEXT("{\"DefaultKeyMappings\":{\"Mappings\":[%s,%s]}}"), *First, *Second);
	};
	const TCHAR* Hold = TEXT("[{\"Class\":\"/Script/EnhancedInput.InputTriggerHold\",\"Properties\":{\"HoldTimeThreshold\":1.5}}]");
	if (!Update(*this, Fixture, Class, Mappings(Mapping(TEXT("SpaceBar"), Hold), Mapping(TEXT("Gamepad_FaceButton_Bottom"), TEXT("[]"))),
		TEXT("SetFullMappings")))
	{
		return false;
	}
	UObject* const HoldObject = ReadObjectAt(*Fixture.Asset(), TEXT("DefaultKeyMappings/Mappings/0/Triggers/0"));
	if (!TestNotNull(TEXT("the hold trigger exists"), HoldObject))
	{
		return false;
	}

	// Struct members: only Key is authored; Action, Triggers, the second row stay.
	const FString PartialKey = Mappings(TEXT("{\"Key\":\"A\"}"), TEXT("{}"));
	ApplyUpdate(*this, Fixture, Class, PartialKey, TEXT("PartialStruct"), true);
	TestExtractShows(*this, Fixture,
		Mappings(Mapping(TEXT("A"), Hold), Mapping(TEXT("Gamepad_FaceButton_Bottom"), TEXT("[]"))), TEXT("PartialStruct"));
	TestTrue(TEXT("partial struct keeps the trigger subobject"),
		ReadObjectAt(*Fixture.Asset(), TEXT("DefaultKeyMappings/Mappings/0/Triggers/0")) == HoldObject);
	ApplyUpdate(*this, Fixture, Class, PartialKey, TEXT("PartialStructReapply"), false);
	TestNoDiff(*this, Fixture, Class, PartialKey, TEXT("PartialStructDiff"));

	// Instanced Properties: only bIsOneShot is authored; HoldTimeThreshold stays 1.5.
	const FString PartialTrigger = Mappings(TEXT("{\"Triggers\":[{\"Class\":\"/Script/EnhancedInput.InputTriggerHold\",")
		TEXT("\"Properties\":{\"bIsOneShot\":true}}]}"), TEXT("{}"));
	ApplyUpdate(*this, Fixture, Class, PartialTrigger, TEXT("PartialInstanced"), true);
	TestTrue(TEXT("partial Properties keep the same subobject"),
		ReadObjectAt(*Fixture.Asset(), TEXT("DefaultKeyMappings/Mappings/0/Triggers/0")) == HoldObject);
	TestEqual(TEXT("omitted HoldTimeThreshold keeps its value"), ReadFloat(*HoldObject, TEXT("HoldTimeThreshold")), 1.5f);
	TestTrue(TEXT("authored bIsOneShot is written"), ReadBool(*HoldObject, TEXT("bIsOneShot")));
	ApplyUpdate(*this, Fixture, Class, PartialTrigger, TEXT("PartialInstancedReapply"), false);
	TestNoDiff(*this, Fixture, Class, PartialTrigger, TEXT("PartialInstancedDiff"));

	// An explicit default value is a value: it is written.
	ApplyUpdate(*this, Fixture, Class, Mappings(TEXT("{\"Triggers\":[{\"Class\":\"/Script/EnhancedInput.InputTriggerHold\",")
		TEXT("\"Properties\":{\"HoldTimeThreshold\":1.0,\"bAffectedByTimeDilation\":true}}]}"), TEXT("{}")),
		TEXT("ExplicitDefault"), true);
	TestEqual(TEXT("explicit default HoldTimeThreshold is written"), ReadFloat(*HoldObject, TEXT("HoldTimeThreshold")), 1.0f);
	TestTrue(TEXT("bIsOneShot is still kept"), ReadBool(*HoldObject, TEXT("bIsOneShot")));
	TestTrue(TEXT("bAffectedByTimeDilation is written"), ReadBool(*HoldObject, TEXT("bAffectedByTimeDilation")));

	// A class change starts from the new class's CDO, not from the old object.
	ApplyUpdate(*this, Fixture, Class, Mappings(
		TEXT("{\"Triggers\":[{\"Class\":\"/Script/EnhancedInput.InputTriggerTap\",\"Properties\":{}}]}"), TEXT("{}")),
		TEXT("ClassChange"), true);
	UObject* const Tap = ReadObjectAt(*Fixture.Asset(), TEXT("DefaultKeyMappings/Mappings/0/Triggers/0"));
	if (TestTrue(TEXT("class change creates a Tap trigger"), Tap != nullptr && Tap != HoldObject
		&& Tap->GetClass()->GetName() == TEXT("InputTriggerTap")))
	{
		TestFalse(TEXT("the new object starts from the CDO (bAffectedByTimeDilation not carried over)"),
			ReadBool(*Tap, TEXT("bAffectedByTimeDilation")));
		TestEqual(TEXT("the new object has the CDO threshold"), ReadFloat(*Tap, TEXT("TapReleaseTimeThreshold")), 0.2f);
	}

	// null is a value only for references and instanced subobjects.
	ApplyUpdate(*this, Fixture, Class, Mappings(TEXT("{\"Action\":null}"), TEXT("{}")), TEXT("NullReference"), true);
	TestNull(TEXT("null clears the Action reference"), ReadObjectAt(*Fixture.Asset(), TEXT("DefaultKeyMappings/Mappings/0/Action")));
	ExpectInvalid(*this, Fixture, Class, {Mappings(TEXT("{\"Key\":null}"), TEXT("{}")),
		TEXT("/Properties/DefaultKeyMappings/Mappings/0/Key"), TEXT("null is not a")});
	ExpectInvalid(*this, Fixture, Class, {Mappings(TEXT("{\"SettingBehavior\":null}"), TEXT("{}")),
		TEXT("/Properties/DefaultKeyMappings/Mappings/0/SettingBehavior"), TEXT("omit a struct member")});
	ExpectInvalid(*this, Fixture, Class, {TEXT("{\"DefaultKeyMappings\":null}"),
		TEXT("/Properties/DefaultKeyMappings"), TEXT("null is not a")});
	ExpectInvalid(*this, Fixture, Class, {Mappings(TEXT("{\"Nope\":1}"), TEXT("{}")),
		TEXT("/Properties/DefaultKeyMappings/Mappings/0/Nope"), TEXT("Key")});

	const FString Expected = ExtractSnapshot(*this, Fixture, TEXT("BeforeReload"));
	TestReload(*this, Fixture, Expected);
	TestUnchangedReapply(*this, Fixture, Class);
	TestRollback(*this, Fixture, Class, Mappings(TEXT("{\"Key\":\"B\"}"), TEXT("{}")), [&](const FString& Stage)
	{
		const TSharedPtr<FJsonObject> Live = ExtractProperties(*this, Fixture, Stage);
		const TSharedPtr<FJsonObject>* MappingData = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		FString Key;
		TestTrue(*FString::Printf(TEXT("%s: first key is A"), *Stage), Live.IsValid()
			&& Live->TryGetObjectField(TEXT("DefaultKeyMappings"), MappingData)
			&& (*MappingData)->TryGetArrayField(TEXT("Mappings"), Rows) && Rows->Num() == 2
			&& (*Rows)[0]->AsObject()->TryGetStringField(TEXT("Key"), Key) && Key == TEXT("A"));
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericPropertyKindsTextIdentityTest,
	"UEAssetDoc.GenericRuntime.PropertyKinds.TextKeyIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// Editing an FText's source string keeps its namespace and key, as the Details
// panel's text box does; an unchanged string keeps the text as it is.
bool FAssetDocumentGenericPropertyKindsTextIdentityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentGenericPropertyKindsTests;
	const TCHAR* Class = TEXT("/Script/EnhancedInput.InputAction");
	if (FSoftObjectPath(Class).ResolveObject() == nullptr)
	{
		AddWarning(TEXT("EnhancedInput is not enabled in this project; skipping."));
		return true;
	}
	const FFixture Fixture(TEXT("TextKey"));
	ON_SCOPE_EXIT { Fixture.Cleanup(); };
	if (!CreateEmpty(*this, Fixture, Class) || !Update(*this, Fixture, Class, TEXT("{\"ActionDescription\":\"Jump\"}"),
		TEXT("SetText")))
	{
		return false;
	}
	const FText First = ReadTextValue(*Fixture.Asset(), TEXT("ActionDescription"));
	const FString Namespace = FTextInspector::GetNamespace(First).Get(FString());
	const FString Key = FTextInspector::GetKey(First).Get(FString());
	TestFalse(TEXT("the new text has a key"), Key.IsEmpty());
	const auto ExpectIdentity = [&](const FString& Stage, const TCHAR* Source)
	{
		const FText Text = ReadTextValue(*Fixture.Asset(), TEXT("ActionDescription"));
		TestEqual(*FString::Printf(TEXT("%s: source string"), *Stage), Text.ToString(), FString(Source));
		TestEqual(*FString::Printf(TEXT("%s: namespace is kept"), *Stage),
			FTextInspector::GetNamespace(Text).Get(FString()), Namespace);
		TestEqual(*FString::Printf(TEXT("%s: key is kept"), *Stage), FTextInspector::GetKey(Text).Get(FString()), Key);
	};
	Update(*this, Fixture, Class, TEXT("{\"ActionDescription\":\"Leap\"}"), TEXT("EditText"));
	ExpectIdentity(TEXT("after edit"), TEXT("Leap"));
	ApplyUpdate(*this, Fixture, Class, TEXT("{\"ActionDescription\":\"Leap\"}"), TEXT("UnchangedText"), false);
	ExpectIdentity(TEXT("after unchanged re-apply"), TEXT("Leap"));
	Fixture.Unload();
	if (TestNotNull(TEXT("the asset reloads"), FSoftObjectPath(Fixture.ObjectPath).TryLoad()))
	{
		ExpectIdentity(TEXT("after reload"), TEXT("Leap"));
	}
	Update(*this, Fixture, Class, TEXT("{\"ActionDescription\":\"\"}"), TEXT("ClearText"));
	TestTrue(TEXT("an empty string clears the text"), ReadTextValue(*Fixture.Asset(), TEXT("ActionDescription")).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericPropertyKindsPickerConstraintsTest,
	"UEAssetDoc.GenericRuntime.PropertyKinds.PickerConstraints",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// The Details panel's class picker hides abstract classes unless AllowAbstract,
// and a NoClear property offers no "Clear"; a changed value must respect both,
// while re-applying the stored value stays a no-op.
bool FAssetDocumentGenericPropertyKindsPickerConstraintsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentGenericPropertyKindsTests;
	const TCHAR* FoliageClass = TEXT("/Script/Foliage.FoliageType_Actor");
	const TCHAR* LibraryClass = TEXT("/Script/Engine.ObjectLibrary");
	const FFixture Foliage(TEXT("PickerFoliage"));
	const FFixture Library(TEXT("PickerLibrary"));
	ON_SCOPE_EXIT
	{
		Foliage.Cleanup();
		Library.Cleanup();
	};
	if (!CreateEmpty(*this, Foliage, FoliageClass) || !CreateEmpty(*this, Library, LibraryClass))
	{
		return false;
	}
	// TSubclassOf<AActor> ActorClass has no AllowAbstract; ObjectLibrary.ObjectBaseClass has it.
	ExpectInvalid(*this, Foliage, FoliageClass, {TEXT("{\"ActorClass\":\"/Script/Engine.Info\"}"),
		TEXT("/Properties/ActorClass"), TEXT("abstract")});
	Update(*this, Library, LibraryClass, TEXT("{\"ObjectBaseClass\":\"/Script/Engine.Info\"}"), TEXT("AllowAbstract"));
	Update(*this, Foliage, FoliageClass, TEXT("{\"ActorClass\":\"/Script/Engine.Actor\"}"), TEXT("ConcreteClass"));

	FProperty* const ActorClass = FindFProperty<FProperty>(Foliage.Asset()->GetClass(), TEXT("ActorClass"));
	FProperty* const BaseClass = FindFProperty<FProperty>(Library.Asset()->GetClass(), TEXT("ObjectBaseClass"));
	{
		const FScopedPropertyMetadata NoClear(ActorClass, TEXT("NoClear"), TEXT(""));
		ExpectInvalid(*this, Foliage, FoliageClass, {TEXT("{\"ActorClass\":null}"),
			TEXT("/Properties/ActorClass"), TEXT("NoClear")});
		ApplyUpdate(*this, Foliage, FoliageClass, TEXT("{\"ActorClass\":\"/Script/Engine.Actor\"}"), TEXT("NoClearReapply"), false);
		Update(*this, Foliage, FoliageClass, TEXT("{\"ActorClass\":\"/Script/Engine.StaticMeshActor\"}"), TEXT("NoClearReplace"));
	}
	Update(*this, Library, LibraryClass, TEXT("{\"ObjectBaseClass\":null}"), TEXT("ClearBeforeNoClear"));
	{
		// A stored null on a NoClear property stays re-appliable.
		const FScopedPropertyMetadata NoClear(BaseClass, TEXT("NoClear"), TEXT(""));
		ApplyUpdate(*this, Library, LibraryClass, TEXT("{\"ObjectBaseClass\":null}"), TEXT("NoClearStoredNull"), false);
	}
	TestFalse(TEXT("the injected NoClear metadata is removed"), ActorClass != nullptr && ActorClass->HasMetaData(TEXT("NoClear")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericPropertyKindsVisibleInstancedTest,
	"UEAssetDoc.GenericRuntime.PropertyKinds.VisibleInstancedInPlace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// HLODLayer.HLODBuilderSettings is VisibleAnywhere + Instanced: the Details panel
// edits the subobject's fields but cannot replace or clear it. Documents edit it
// in place; another class or null is rejected.
bool FAssetDocumentGenericPropertyKindsVisibleInstancedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentGenericPropertyKindsTests;
	const TCHAR* Class = TEXT("/Script/Engine.HLODLayer");
	const TCHAR* Settings = TEXT("/Script/WorldPartitionHLODUtilities.HLODBuilderInstancingSettings");
	if (FSoftObjectPath(Settings).TryLoad() == nullptr)
	{
		AddWarning(TEXT("WorldPartitionHLODUtilities is not enabled in this project; skipping."));
		return true;
	}
	const FFixture Fixture(TEXT("HLODLayer"));
	ON_SCOPE_EXIT { Fixture.Cleanup(); };
	if (!CreateEmpty(*this, Fixture, Class))
	{
		return false;
	}
	if (ReadObject(*Fixture.Asset(), TEXT("HLODBuilderSettings")) == nullptr)
	{
		// The layer creates its settings when LayerType changes (PostEditChangeProperty).
		ApplyUpdate(*this, Fixture, Class, TEXT("{\"LayerType\":\"MeshMerge\"}"), TEXT("LayerTypeMeshMerge"), true);
		ApplyUpdate(*this, Fixture, Class, TEXT("{\"LayerType\":\"Instancing\"}"), TEXT("LayerTypeInstancing"), true);
	}
	UObject* const Current = ReadObject(*Fixture.Asset(), TEXT("HLODBuilderSettings"));
	if (!TestTrue(TEXT("the layer holds instancing settings"), Current != nullptr && Current->GetClass()->GetPathName() == Settings))
	{
		return false;
	}
	const FString Edit = FString::Printf(TEXT("{\"HLODBuilderSettings\":{\"Class\":\"%s\",\"Properties\":{\"bDisallowNanite\":true}}}"), Settings);
	if (!Update(*this, Fixture, Class, Edit, TEXT("EditInPlace")))
	{
		return false;
	}
	TestTrue(TEXT("the same subobject is edited"), ReadObject(*Fixture.Asset(), TEXT("HLODBuilderSettings")) == Current);
	TestTrue(TEXT("the field is written"), ReadBool(*Current, TEXT("bDisallowNanite")));
	TestTrue(TEXT("the subobject stays owned by the layer"), Current->GetOuter() == Fixture.Asset());
	ApplyUpdate(*this, Fixture, Class, Edit, TEXT("EditInPlaceReapply"), false);
	TestNoDiff(*this, Fixture, Class, Edit, TEXT("EditInPlaceDiff"));

	ExpectInvalid(*this, Fixture, Class, {TEXT("{\"HLODBuilderSettings\":null}"),
		TEXT("/Properties/HLODBuilderSettings"), TEXT("VisibleAnywhere")});
	ExpectInvalid(*this, Fixture, Class, {TEXT("{\"HLODBuilderSettings\":{\"Class\":\"/Script/WorldPartitionHLODUtilities.HLODBuilderMeshMergeSettings\",\"Properties\":{}}}"),
		TEXT("/Properties/HLODBuilderSettings/Class"), TEXT("only edited in place")});

	const FString Expected = ExtractSnapshot(*this, Fixture, TEXT("BeforeReload"));
	TestReload(*this, Fixture, Expected);
	TestUnchangedReapply(*this, Fixture, Class);
	TestRollback(*this, Fixture, Class,
		FString::Printf(TEXT("{\"HLODBuilderSettings\":{\"Class\":\"%s\",\"Properties\":{\"bDisallowNanite\":false}}}"), Settings),
		[&](const FString& Stage)
	{
		const UObject* Live = ReadObject(*Fixture.Asset(), TEXT("HLODBuilderSettings"));
		TestTrue(*FString::Printf(TEXT("%s: bDisallowNanite is true"), *Stage), Live != nullptr && ReadBool(*Live, TEXT("bDisallowNanite")));
	});
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericPropertyKindsEditorDerivedTest,
	"UEAssetDoc.GenericRuntime.PropertyKinds.EditorDerivedReference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// A non-instanced hard reference to an object inside the asset is created by the
// asset's own code (PaperSprite.BodySetup: every PostEditChange recreates and
// renames it); the Details panel picker lists assets only, so no human can set
// it. Documents omit such a property (stable extracts and diffs) and refuse
// naming an object inside the asset through a plain reference; replacing the
// stored subobject stays possible, as with the picker.
bool FAssetDocumentGenericPropertyKindsEditorDerivedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentGenericPropertyKindsTests;
	const TCHAR* LibraryClass = TEXT("/Script/Engine.ObjectLibrary");
	const FFixture Library(TEXT("DerivedLibrary"));
	ON_SCOPE_EXIT { Library.Cleanup(); };
	if (!CreateEmpty(*this, Library, LibraryClass))
	{
		return false;
	}
	UObject* const Inner = NewObject<UObject>(Library.Asset(), Library.Asset()->GetClass(), TEXT("DerivedInner"));
	ExpectInvalid(*this, Library, LibraryClass, {FString::Printf(TEXT("{\"Objects\":[\"%s\"]}"), *Inner->GetPathName()),
		TEXT("/Properties/Objects/0"), TEXT("an object inside the asset")});
	{
		// The asset's own code stores a reference to its subobject.
		FArrayProperty* const Objects = CastField<FArrayProperty>(FindProperty(*Library.Asset(), TEXT("Objects")));
		if (!TestNotNull(TEXT("ObjectLibrary.Objects exists"), Objects))
		{
			return false;
		}
		FScriptArrayHelper Helper(Objects, Objects->ContainerPtrToValuePtr<void>(Library.Asset()));
		ON_SCOPE_EXIT { Helper.EmptyValues(); };
		Helper.EmptyValues();
		CastFieldChecked<FObjectPropertyBase>(Objects->Inner)->SetObjectPropertyValue(Helper.GetRawPtr(Helper.AddValue()), Inner);
		const TSharedPtr<FJsonObject> Extracted = ExtractProperties(*this, Library, TEXT("LibraryDerived"));
		TestTrue(TEXT("a property holding a subobject reference is omitted from Extract"),
			Extracted.IsValid() && !Extracted->HasField(TEXT("Objects")) && Extracted->HasField(TEXT("ObjectBaseClass")));
		// As with the picker, the stored subobject can be replaced or removed.
		if (Update(*this, Library, LibraryClass, TEXT("{\"Objects\":[]}"), TEXT("RemoveDerived")))
		{
			TestEqual(TEXT("the subobject reference is removed"), Helper.Num(), 0);
		}
	}

	const TCHAR* SpriteClass = TEXT("/Script/Paper2D.PaperSprite");
	if (FindObject<UClass>(nullptr, SpriteClass) == nullptr)
	{
		AddWarning(TEXT("Paper2D is not loaded; the PaperSprite.BodySetup case is skipped."));
		return true;
	}
	const FFixture Sprite(TEXT("DerivedSprite"));
	ON_SCOPE_EXIT { Sprite.Cleanup(); };
	if (!CreateEmpty(*this, Sprite, SpriteClass)
		|| !Update(*this, Sprite, SpriteClass, TEXT("{\"CollisionThickness\":12.5}"), TEXT("RebuildCollision")))
	{
		return false;
	}
	UObject* const BodySetup = ReadObject(*Sprite.Asset(), TEXT("BodySetup"));
	if (!TestTrue(TEXT("the sprite holds a BodySetup subobject"), BodySetup != nullptr && BodySetup->IsIn(Sprite.Asset())))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Extracted = ExtractProperties(*this, Sprite, TEXT("SpriteDerived"));
	TestTrue(TEXT("BodySetup is omitted from Extract"), Extracted.IsValid() && !Extracted->HasField(TEXT("BodySetup")));
	{
		FAssetDocumentInspectRequest Request;
		Request.ClassOrAsset = Sprite.ObjectPath;
		const FAssetDocumentResult Inspected = FAssetDocumentModule::Get().GetService().Inspect(Request);
		const TArray<TSharedPtr<FJsonValue>>* Skipped = nullptr;
		const bool bListed = Inspected.IsSuccess() && Inspected.Payload.IsValid()
			&& Inspected.Payload->TryGetArrayField(TEXT("skipped"), Skipped)
			&& Skipped->ContainsByPredicate([](const TSharedPtr<FJsonValue>& Row)
			{
				const TSharedPtr<FJsonObject> Object = Row.IsValid() ? Row->AsObject() : nullptr;
				return Object.IsValid() && Object->GetStringField(TEXT("name")) == TEXT("BodySetup")
					&& Object->GetStringField(TEXT("reason")) == TEXT("editor-derived-reference");
			});
		if (!TestTrue(TEXT("Inspect lists BodySetup as skipped: editor-derived-reference"), bListed))
		{
			Report(*this, Inspected, TEXT("Inspect"));
		}
	}
	// Clearing it is what the picker offers, but the sprite's PostEditChange rebuilds it:
	// refused before mutation with what the editor would store.
	ExpectInvalid(*this, Sprite, SpriteClass, {TEXT("{\"BodySetup\":null}"),
		TEXT("/Properties/BodySetup"), TEXT("BodySetup")});
	// Each edit recreates BodySetup under a new name; documents and diffs stay stable.
	Update(*this, Sprite, SpriteClass, TEXT("{\"CollisionThickness\":20}"), TEXT("RecreateBodySetup"));
	TestTrue(TEXT("the edit recreated BodySetup"), ReadObject(*Sprite.Asset(), TEXT("BodySetup")) != BodySetup);
	TestNoDiff(*this, Sprite, SpriteClass, TEXT("{\"CollisionThickness\":20}"), TEXT("SpriteNoDiff"));
	TestUnchangedReapply(*this, Sprite, SpriteClass);
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericPropertyKindsEditInlineTest,
	"UEAssetDoc.GenericRuntime.PropertyKinds.EditInlineReference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// AnimCurveCompressionSettings.Codec is a plain (non-instanced) reference with EditInline metadata
// (UPROPERTY(Export, EditAnywhere, NoClear, meta=(EditInline))). The Details panel shows a class picker that
// creates the codec under the settings asset and the codec's own fields inline, and no asset picker. Documents
// write the asset's own codec like an instanced subobject: same class edits it in place, another class creates a
// new one under the asset. A codec that belongs to another asset would be edited in place by the Details panel
// (changing that asset): such an edit is refused, pointing at the owning asset.
bool FAssetDocumentGenericPropertyKindsEditInlineTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentGenericPropertyKindsTests;
	const TCHAR* Class = TEXT("/Script/Engine.AnimCurveCompressionSettings");
	const TCHAR* RichCurve = TEXT("/Script/Engine.AnimCurveCompressionCodec_CompressedRichCurve");
	const TCHAR* Uniform = TEXT("/Script/Engine.AnimCurveCompressionCodec_UniformlySampled");
	const FFixture Fixture(TEXT("CurveCompression"));
	const FFixture Other(TEXT("CurveCompressionOther"));
	ON_SCOPE_EXIT
	{
		Fixture.Cleanup();
		Other.Cleanup();
	};
	if (!CreateEmpty(*this, Fixture, Class) || !CreateEmpty(*this, Other, Class))
	{
		return false;
	}
	UObject* const Codec = ReadObject(*Fixture.Asset(), TEXT("Codec"));
	if (!TestTrue(TEXT("the factory creates a rich-curve codec inside the asset"), Codec != nullptr
		&& Codec->GetClass()->GetPathName() == RichCurve && Codec->GetOuter() == Fixture.Asset()))
	{
		return false;
	}
	{
		const TSharedPtr<FJsonObject> Extracted = ExtractProperties(*this, Fixture, TEXT("ExtractCodec"));
		const TSharedPtr<FJsonObject>* Value = nullptr;
		FString ExtractedClass;
		TestTrue(TEXT("Extract shows the asset's own codec as {Class, Properties}"), Extracted.IsValid()
			&& Extracted->TryGetObjectField(TEXT("Codec"), Value) && (*Value)->TryGetStringField(TEXT("Class"), ExtractedClass)
			&& ExtractedClass == RichCurve);
	}

	// Same class: the codec's fields are edited in place (the inline rows under Codec).
	const FString InPlace = FString::Printf(TEXT("{\"Codec\":{\"Class\":\"%s\",\"Properties\":{\"MaxCurveError\":0.25}}}"), RichCurve);
	if (!Update(*this, Fixture, Class, InPlace, TEXT("EditInPlace")))
	{
		return false;
	}
	TestTrue(TEXT("the same codec is edited"), ReadObject(*Fixture.Asset(), TEXT("Codec")) == Codec);
	TestEqual(TEXT("MaxCurveError is written"), ReadFloat(*Codec, TEXT("MaxCurveError")), 0.25f);
	ApplyUpdate(*this, Fixture, Class, InPlace, TEXT("EditInPlaceReapply"), false);
	TestNoDiff(*this, Fixture, Class, InPlace, TEXT("EditInPlaceDiff"));

	// The picker offers concrete EditInlineNew classes; NoClear: no null.
	ExpectInvalid(*this, Fixture, Class, {TEXT("{\"Codec\":{\"Class\":\"/Script/Engine.AnimCurveCompressionCodec\"}}"),
		TEXT("/Properties/Codec/Class"), TEXT("abstract")});
	ExpectInvalid(*this, Fixture, Class, {TEXT("{\"Codec\":null}"), TEXT("/Properties/Codec"), TEXT("NoClear")});

	// Another class: the class picker's NewObject(Settings, Class); the old codec is no longer referenced.
	const FString Replaced = FString::Printf(
		TEXT("{\"Codec\":{\"Class\":\"%s\",\"Properties\":{\"UseAnimSequenceSampleRate\":false,\"SampleRate\":15}}}"), Uniform);
	if (!Update(*this, Fixture, Class, Replaced, TEXT("ReplaceClass")))
	{
		return false;
	}
	UObject* const NewCodec = ReadObject(*Fixture.Asset(), TEXT("Codec"));
	TestTrue(TEXT("a new uniformly-sampled codec is created inside the asset"), NewCodec != nullptr && NewCodec != Codec
		&& NewCodec->GetClass()->GetPathName() == Uniform && NewCodec->GetOuter() == Fixture.Asset());
	TestEqual(TEXT("SampleRate is written"), NewCodec != nullptr ? ReadFloat(*NewCodec, TEXT("SampleRate")) : 0.0f, 15.0f);
	ApplyUpdate(*this, Fixture, Class, Replaced, TEXT("ReplaceClassReapply"), false);
	TestNoDiff(*this, Fixture, Class, Replaced, TEXT("ReplaceClassDiff"));

	const FString Expected = ExtractSnapshot(*this, Fixture, TEXT("BeforeReload"));
	TestReload(*this, Fixture, Expected);
	TestUnchangedReapply(*this, Fixture, Class);
	TestRollback(*this, Fixture, Class,
		FString::Printf(TEXT("{\"Codec\":{\"Class\":\"%s\",\"Properties\":{\"SampleRate\":20}}}"), Uniform),
		[&](const FString& Stage)
	{
		const UObject* Live = ReadObject(*Fixture.Asset(), TEXT("Codec"));
		TestTrue(*FString::Printf(TEXT("%s: SampleRate is 15"), *Stage), Live != nullptr && ReadFloat(*Live, TEXT("SampleRate")) == 15.0f);
	});

	// A codec of another asset (set by code; the Details panel would edit it in place): an edit of its fields is
	// refused and names the asset to author instead.
	{
		UObject* const Settings = Fixture.Asset();
		UObject* const Foreign = ReadObject(*Other.Asset(), TEXT("Codec"));
		FObjectProperty* const CodecProperty = CastField<FObjectProperty>(FindProperty(*Settings, TEXT("Codec")));
		if (!TestTrue(TEXT("the other asset has its own codec"), Foreign != nullptr && CodecProperty != nullptr))
		{
			return false;
		}
		UObject* const Own = CodecProperty->GetObjectPropertyValue_InContainer(Settings);
		CodecProperty->SetObjectPropertyValue_InContainer(Settings, Foreign);
		ON_SCOPE_EXIT { CodecProperty->SetObjectPropertyValue_InContainer(Settings, Own); };
		ExpectInvalid(*this, Fixture, Class, {FString::Printf(TEXT("{\"Codec\":{\"Class\":\"%s\",\"Properties\":{\"MaxCurveError\":0.5}}}"), RichCurve),
			TEXT("/Properties/Codec/Properties"), Other.ObjectPath});
		TestEqual(TEXT("the other asset's codec is unchanged"), ReadFloat(*Foreign, TEXT("MaxCurveError")), 0.0f);
	}
	return true;
}
#endif
