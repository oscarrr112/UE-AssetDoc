// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// A data asset edited in its Details panel: maps keyed by gameplay tags, soft references and a plain struct, and
// optional values (an optional number, an optional struct, a struct with an optional member). Extract shows each
// value in its document form, the extract re-applies as a no-op, an edit of each value works and keeps the rest, and
// a key the tag picker would not offer is refused. A document written when these values read as "Unsupported"
// markers re-applies as a no-op and keeps them.

#include "AssetDocumentEditorCodecTestTypes.h"
#include "AssetDocumentTolerantTestSupport.h"

#include "GameplayTagsManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentCodecGenericTestsLocal
{
const TCHAR* const CodecTagFire = TEXT("AssetDocumentTest.Codec.Fire");
const TCHAR* const CodecTagIce = TEXT("AssetDocumentTest.Codec.Ice");
const TCHAR* const CodecTagPoison = TEXT("AssetDocumentTest.Codec.Poison");
const TCHAR* const CodecMaterialPath = TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial");
const TCHAR* const CodecTexturePath = TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture");

// The test's gameplay tags, from a tag list file in a tag search path of its own (Saved/), for the scope.
struct FCodecTagListScope
{
	FString Directory;

	FCodecTagListScope()
		: Directory(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), AssetDocumentTolerantTestSupport::FolderName,
			FString::Printf(TEXT("Tags_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)))))
	{
		FString List = TEXT("[/Script/GameplayTags.GameplayTagsList]\n");
		for (const TCHAR* Tag : {CodecTagFire, CodecTagIce, CodecTagPoison})
		{
			List += FString::Printf(TEXT("GameplayTagList=(Tag=\"%s\",DevComment=\"\")\n"), Tag);
		}
		IFileManager::Get().MakeDirectory(*Directory, true);
		FFileHelper::SaveStringToFile(List, *FPaths::Combine(Directory, TEXT("AssetDocumentCodecTags.ini")));
		UGameplayTagsManager::Get().AddTagIniSearchPath(Directory);
	}

	~FCodecTagListScope()
	{
		UGameplayTagsManager::Get().RemoveTagIniSearchPath(Directory);
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	}

	FCodecTagListScope(const FCodecTagListScope&) = delete;
	FCodecTagListScope& operator=(const FCodecTagListScope&) = delete;
};

FGameplayTag CodecTag(const TCHAR* Name)
{
	return UGameplayTagsManager::Get().RequestGameplayTag(FName(Name), false);
}

FAssetDocumentCodecTestCellKey CodecCell(const TCHAR* Layer, int32 Index)
{
	FAssetDocumentCodecTestCellKey Key;
	Key.Layer = FName(Layer);
	Key.Index = Index;
	return Key;
}

// The key's document form: its export text, as the map's key property writes it.
FString CodecCellText(const FAssetDocumentCodecTestCellKey& Key)
{
	const FMapProperty* Map = CastField<FMapProperty>(UAssetDocumentCodecTestAsset::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UAssetDocumentCodecTestAsset, CellNames)));
	FString Text;
	if (Map != nullptr)
	{
		Map->KeyProp->ExportTextItem_Direct(Text, &Key, nullptr, nullptr, PPF_None);
	}
	return Text;
}

TSharedPtr<FJsonObject> CodecProperties(const TSharedPtr<FJsonObject>& Document)
{
	return Document->GetObjectField(TEXT("Properties"));
}

TSharedPtr<FJsonObject> CodecObjectAt(const TSharedPtr<FJsonObject>& Document, const FString& Field)
{
	const TSharedPtr<FJsonObject>* Object = nullptr;
	return CodecProperties(Document)->TryGetObjectField(Field, Object) ? *Object : MakeShared<FJsonObject>();
}

bool CodecNumberAt(const TSharedPtr<FJsonObject>& Document, const FString& Pointer, double Expected)
{
	const TSharedPtr<FJsonValue> Value = AssetDocumentTolerantTestSupport::At(Document, Pointer);
	double Number = 0.0;
	return Value.IsValid() && Value->TryGetNumber(Number) && Number == Expected;
}

bool CodecIsNullAt(const TSharedPtr<FJsonObject>& Document, const FString& Pointer)
{
	const TSharedPtr<FJsonValue> Value = AssetDocumentTolerantTestSupport::At(Document, Pointer);
	return Value.IsValid() && Value->IsNull();
}

// The marker a read gave a whole value of Type (TMap, TOptional) when the type had no document form.
TSharedRef<FJsonObject> CodecFormerMarker(const TCHAR* Type, const TCHAR* Text)
{
	TSharedRef<FJsonObject> Marker = MakeShared<FJsonObject>();
	Marker->SetStringField(TEXT("Unsupported"), FString::Printf(TEXT("%s has no document form; AssetDoc keeps the value."), Type));
	Marker->SetStringField(TEXT("Text"), Text);
	return Marker;
}

// The extract with Edit applied: a document as an earlier read wrote it.
TSharedPtr<FJsonObject> CodecOlderDocument(FAutomationTestBase& Test, const AssetDocumentTolerantTestSupport::FTarget& Target,
	TFunctionRef<void(const TSharedPtr<FJsonObject>& Document)> Edit)
{
	const FAssetDocumentResult Read = AssetDocumentTolerantTestSupport::ExtractResult(Target);
	if (!Test.TestTrue(TEXT("older document: extract succeeds"), Read.IsSuccess() && Read.Payload.IsValid()))
	{
		return nullptr;
	}
	Edit(Read.Payload);
	return Read.Payload;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentCodecGenericMapKeysTest, "UEAssetDoc.Codec.Generic.MapKeys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentCodecGenericMapKeysTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentCodecGenericTestsLocal;
	const FCodecTagListScope Tags;
	const FTarget Target(TEXT("DA_CodecMapKeys"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	if (!TestTrue(TEXT("tags registered"), CodecTag(CodecTagFire).IsValid() && CodecTag(CodecTagIce).IsValid() && CodecTag(CodecTagPoison).IsValid()))
	{
		return false;
	}

	// The Details panel: a tag picked for each Tag Weights key, an asset picked for a Soft Counts key, a struct key's
	// members typed in.
	UAssetDocumentCodecTestAsset* Asset = Target.Create<UAssetDocumentCodecTestAsset>();
	Asset->TagWeights.Add(CodecTag(CodecTagFire), 1.5f);
	Asset->TagWeights.Add(CodecTag(CodecTagIce), 2.0f);
	Asset->SoftCounts.Add(TSoftObjectPtr<UObject>(FSoftObjectPath(CodecMaterialPath)), 3);
	Asset->CellNames.Add(CodecCell(TEXT("Ground"), 2), TEXT("Start"));
	Asset->Keyed.Label = TEXT("Keyed");
	Asset->Keyed.SoftNames.Add(TSoftObjectPtr<UObject>(FSoftObjectPath(CodecMaterialPath)), TEXT("Base"));
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const FString GroundKey = CodecCellText(CodecCell(TEXT("Ground"), 2));
	const FString WaterKey = CodecCellText(CodecCell(TEXT("Water"), 4));
	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Keyed maps"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	TestTrue(TEXT("tag key reads as the tag name"), CodecNumberAt(Extracted, FString(TEXT("/Properties/TagWeights/")) + CodecTagFire, 1.5));
	TestTrue(TEXT("second tag key"), CodecNumberAt(Extracted, FString(TEXT("/Properties/TagWeights/")) + CodecTagIce, 2.0));
	TestTrue(TEXT("soft key reads as the object path"), CodecObjectAt(Extracted, TEXT("SoftCounts"))->HasField(CodecMaterialPath));
	const TSharedPtr<FJsonValue> Ground = CodecObjectAt(Extracted, TEXT("CellNames"))->TryGetField(GroundKey);
	TestTrue(TEXT("struct key reads as its export text"), Ground.IsValid() && Ground->Type == EJson::String && Ground->AsString() == TEXT("Start"));

	// A document written when these maps read as markers: the marker keeps each map, also inside a struct value (never
	// read as a map whose keys are "Unsupported" and "Text").
	if (const TSharedPtr<FJsonObject> Older = CodecOlderDocument(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
		{
			CodecProperties(Document)->SetObjectField(TEXT("TagWeights"), CodecFormerMarker(TEXT("TMap"), TEXT("()")));
			CodecProperties(Document)->SetObjectField(TEXT("SoftCounts"), CodecFormerMarker(TEXT("TMap"), TEXT("()")));
			CodecProperties(Document)->SetObjectField(TEXT("CellNames"), CodecFormerMarker(TEXT("TMap"), TEXT("()")));
			const TSharedPtr<FJsonObject> Keyed = CodecObjectAt(Document, TEXT("Keyed"));
			Keyed->SetObjectField(TEXT("SoftNames"), CodecFormerMarker(TEXT("TMap"), TEXT("()")));
			CodecProperties(Document)->SetObjectField(TEXT("Keyed"), Keyed);
		}))
	{
		TestAppliesUnchanged(*this, Target, Older, TEXT("Older document"));
		Asset = Target.Get<UAssetDocumentCodecTestAsset>();
		if (TestNotNull(TEXT("asset after the older document"), Asset))
		{
			TestEqual(TEXT("older document keeps the tag map"), Asset->TagWeights.Num(), 2);
			const int32* Material = Asset->SoftCounts.Find(TSoftObjectPtr<UObject>(FSoftObjectPath(CodecMaterialPath)));
			TestTrue(TEXT("older document keeps the soft map"), Asset->SoftCounts.Num() == 1 && Material != nullptr && *Material == 3);
			TestEqual(TEXT("older document keeps the struct-keyed map"), Asset->CellNames.Num(), 1);
			const FString* Base = Asset->Keyed.SoftNames.Find(TSoftObjectPtr<UObject>(FSoftObjectPath(CodecMaterialPath)));
			TestTrue(TEXT("older document keeps the map inside the struct"), Asset->Keyed.SoftNames.Num() == 1 && Base != nullptr && *Base == TEXT("Base"));
			TestEqual(TEXT("older document keeps the struct's other member"), Asset->Keyed.Label, FString(TEXT("Keyed")));
		}
	}

	// An edit of each map: a tag value changed, a tag removed and another added, a soft key added, a struct key added.
	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [&](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Weights = CodecObjectAt(Document, TEXT("TagWeights"));
		Weights->SetNumberField(CodecTagFire, 3.0);
		Weights->RemoveField(CodecTagIce);
		Weights->SetNumberField(CodecTagPoison, 1.0);
		CodecProperties(Document)->SetObjectField(TEXT("TagWeights"), Weights);
		const TSharedPtr<FJsonObject> Counts = CodecObjectAt(Document, TEXT("SoftCounts"));
		Counts->SetNumberField(CodecTexturePath, 7.0);
		CodecProperties(Document)->SetObjectField(TEXT("SoftCounts"), Counts);
		const TSharedPtr<FJsonObject> Cells = CodecObjectAt(Document, TEXT("CellNames"));
		Cells->SetStringField(WaterKey, TEXT("Lake"));
		CodecProperties(Document)->SetObjectField(TEXT("CellNames"), Cells);
	}, TEXT("Edit"));
	if (TestTrue(TEXT("edit applies"), Edited.IsSuccess()))
	{
		Asset = Target.Get<UAssetDocumentCodecTestAsset>();
		if (TestNotNull(TEXT("asset after edit"), Asset))
		{
			const float* Fire = Asset->TagWeights.Find(CodecTag(CodecTagFire));
			TestTrue(TEXT("tag value edited"), Fire != nullptr && *Fire == 3.0f);
			TestFalse(TEXT("tag removed"), Asset->TagWeights.Contains(CodecTag(CodecTagIce)));
			TestTrue(TEXT("tag added"), Asset->TagWeights.Contains(CodecTag(CodecTagPoison)));
			const int32* Material = Asset->SoftCounts.Find(TSoftObjectPtr<UObject>(FSoftObjectPath(CodecMaterialPath)));
			const int32* Texture = Asset->SoftCounts.Find(TSoftObjectPtr<UObject>(FSoftObjectPath(CodecTexturePath)));
			TestTrue(TEXT("soft key kept"), Material != nullptr && *Material == 3);
			TestTrue(TEXT("soft key added"), Texture != nullptr && *Texture == 7);
			const FString* Start = Asset->CellNames.Find(CodecCell(TEXT("Ground"), 2));
			const FString* Lake = Asset->CellNames.Find(CodecCell(TEXT("Water"), 4));
			TestTrue(TEXT("struct key kept"), Start != nullptr && *Start == TEXT("Start"));
			TestTrue(TEXT("struct key added"), Lake != nullptr && *Lake == TEXT("Lake"));
		}
		TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("After edit"));
	}

	// A key the tag picker does not offer, and a struct key that does not read back as written, are refused.
	const FAssetDocumentResult Unregistered = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Weights = CodecObjectAt(Document, TEXT("TagWeights"));
		Weights->SetNumberField(TEXT("AssetDocumentTest.Codec.NotRegistered"), 1.0);
		CodecProperties(Document)->SetObjectField(TEXT("TagWeights"), Weights);
	}, TEXT("Unregistered tag"));
	TestFalse(TEXT("unregistered tag key refused"), Unregistered.IsSuccess());
	const FAssetDocumentResult Spelled = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Cells = CodecObjectAt(Document, TEXT("CellNames"));
		Cells->SetStringField(TEXT("(Index=9,Layer=\"Sky\")"), TEXT("Cloud"));
		CodecProperties(Document)->SetObjectField(TEXT("CellNames"), Cells);
	}, TEXT("Reordered struct key"));
	TestFalse(TEXT("non-canonical struct key refused"), Spelled.IsSuccess());

	// A soft key that is not an object path, and two keys that differ only in case (one key in the map), are refused.
	const FAssetDocumentResult NotAPath = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Keyed = CodecObjectAt(Document, TEXT("Keyed"));
		const TSharedPtr<FJsonObject> Names = MakeShared<FJsonObject>();
		Names->SetStringField(CodecMaterialPath, TEXT("Base"));
		Names->SetStringField(TEXT("Not a path"), TEXT("Lost"));
		Keyed->SetObjectField(TEXT("SoftNames"), Names);
		CodecProperties(Document)->SetObjectField(TEXT("Keyed"), Keyed);
	}, TEXT("Soft key that is not a path"));
	TestFalse(TEXT("soft key that is not a path refused"), NotAPath.IsSuccess());
	const FAssetDocumentResult Folded = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Counts = CodecObjectAt(Document, TEXT("SoftCounts"));
		Counts->SetNumberField(FString(CodecTexturePath).ToLower(), 8.0);
		CodecProperties(Document)->SetObjectField(TEXT("SoftCounts"), Counts);
	}, TEXT("Keys differing in case"));
	TestFalse(TEXT("keys differing only in case refused"), Folded.IsSuccess());
	Asset = Target.Get<UAssetDocumentCodecTestAsset>();
	TestTrue(TEXT("refused edits change nothing"), Asset != nullptr && Asset->TagWeights.Num() == 2 && Asset->CellNames.Num() == 2
		&& Asset->SoftCounts.Num() == 2 && Asset->Keyed.SoftNames.Num() == 1);
	if (Asset != nullptr)
	{
		const int32* Texture = Asset->SoftCounts.Find(TSoftObjectPtr<UObject>(FSoftObjectPath(CodecTexturePath)));
		TestTrue(TEXT("refused case duplicate keeps the value"), Texture != nullptr && *Texture == 7);
		TestFalse(TEXT("no None soft key"), Asset->Keyed.SoftNames.Contains(TSoftObjectPtr<UObject>()));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentCodecGenericOptionalTest, "UEAssetDoc.Codec.Generic.Optional",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentCodecGenericOptionalTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentCodecGenericTestsLocal;
	const FTarget Target(TEXT("DA_CodecOptional"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	// The Details panel: each optional's check box ticked and a value typed in; the optional struct's own optional
	// member left unticked.
	UAssetDocumentCodecTestAsset* Asset = Target.Create<UAssetDocumentCodecTestAsset>();
	Asset->OptionalCount = 5;
	Asset->Settings.Label = TEXT("Main");
	Asset->Settings.Scale = 0.5f;
	FAssetDocumentCodecTestOptionalSettings Inner;
	Inner.Label = TEXT("Inner");
	Asset->OptionalSettings = Inner;
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Optional values"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	TestTrue(TEXT("set optional reads as its value"), CodecNumberAt(Extracted, TEXT("/Properties/OptionalCount"), 5.0));
	TestTrue(TEXT("optional member reads as its value"), CodecNumberAt(Extracted, TEXT("/Properties/Settings/Scale"), 0.5));
	const TSharedPtr<FJsonValue> InnerLabel = At(Extracted, TEXT("/Properties/OptionalSettings/Label"));
	TestTrue(TEXT("set optional struct reads as the struct"), InnerLabel.IsValid() && InnerLabel->Type == EJson::String && InnerLabel->AsString() == TEXT("Inner"));

	// A document written when optionals read as markers: each marker keeps its optional, also a struct member's.
	if (const TSharedPtr<FJsonObject> Older = CodecOlderDocument(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
		{
			CodecProperties(Document)->SetObjectField(TEXT("OptionalCount"), CodecFormerMarker(TEXT("TOptional"), TEXT("5")));
			CodecProperties(Document)->SetObjectField(TEXT("OptionalSettings"), CodecFormerMarker(TEXT("TOptional"), TEXT("(Label=\"Inner\")")));
			const TSharedPtr<FJsonObject> Settings = CodecObjectAt(Document, TEXT("Settings"));
			Settings->SetObjectField(TEXT("Scale"), CodecFormerMarker(TEXT("TOptional"), TEXT("0.500000")));
			CodecProperties(Document)->SetObjectField(TEXT("Settings"), Settings);
		}))
	{
		TestAppliesUnchanged(*this, Target, Older, TEXT("Older document"));
		Asset = Target.Get<UAssetDocumentCodecTestAsset>();
		if (TestNotNull(TEXT("asset after the older document"), Asset))
		{
			TestTrue(TEXT("older document keeps the optional number"), Asset->OptionalCount.IsSet() && *Asset->OptionalCount == 5);
			TestTrue(TEXT("older document keeps the optional member"), Asset->Settings.Scale.IsSet() && *Asset->Settings.Scale == 0.5f);
			TestTrue(TEXT("older document keeps the optional struct"), Asset->OptionalSettings.IsSet()
				&& Asset->OptionalSettings->Label == TEXT("Inner"));
		}
	}

	// Unticking the number and the struct member, ticking the inner member with a value.
	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		CodecProperties(Document)->SetField(TEXT("OptionalCount"), MakeShared<FJsonValueNull>());
		const TSharedPtr<FJsonObject> Settings = CodecObjectAt(Document, TEXT("Settings"));
		Settings->SetField(TEXT("Scale"), MakeShared<FJsonValueNull>());
		CodecProperties(Document)->SetObjectField(TEXT("Settings"), Settings);
		const TSharedPtr<FJsonObject> Optional = CodecObjectAt(Document, TEXT("OptionalSettings"));
		Optional->SetNumberField(TEXT("Scale"), 2.0);
		CodecProperties(Document)->SetObjectField(TEXT("OptionalSettings"), Optional);
	}, TEXT("Edit"));
	if (TestTrue(TEXT("edit applies"), Edited.IsSuccess()))
	{
		Asset = Target.Get<UAssetDocumentCodecTestAsset>();
		if (TestNotNull(TEXT("asset after edit"), Asset))
		{
			TestFalse(TEXT("optional number unset"), Asset->OptionalCount.IsSet());
			TestFalse(TEXT("optional member unset"), Asset->Settings.Scale.IsSet());
			TestEqual(TEXT("struct member kept"), Asset->Settings.Label, FString(TEXT("Main")));
			TestTrue(TEXT("optional struct still set"), Asset->OptionalSettings.IsSet());
			if (Asset->OptionalSettings.IsSet())
			{
				TestEqual(TEXT("optional struct member kept"), Asset->OptionalSettings->Label, FString(TEXT("Inner")));
				TestTrue(TEXT("inner optional set"), Asset->OptionalSettings->Scale.IsSet() && *Asset->OptionalSettings->Scale == 2.0f);
			}
		}
		const TSharedPtr<FJsonObject> Reread = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("After edit"));
		if (Reread.IsValid())
		{
			const TSharedPtr<FJsonValue> Count = At(Reread, TEXT("/Properties/OptionalCount"));
			TestTrue(TEXT("unset optional reads as null or the default"), !Count.IsValid() || Count->IsNull());
		}
	}

	// Ticking the optional number again with a value; unsetting the whole optional struct.
	const FAssetDocumentResult Reset = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		CodecProperties(Document)->SetNumberField(TEXT("OptionalCount"), 9.0);
		CodecProperties(Document)->SetField(TEXT("OptionalSettings"), MakeShared<FJsonValueNull>());
	}, TEXT("Set and unset"));
	if (TestTrue(TEXT("set and unset applies"), Reset.IsSuccess()))
	{
		Asset = Target.Get<UAssetDocumentCodecTestAsset>();
		TestTrue(TEXT("optional number set"), Asset != nullptr && Asset->OptionalCount.IsSet() && *Asset->OptionalCount == 9);
		TestTrue(TEXT("optional struct unset"), Asset != nullptr && !Asset->OptionalSettings.IsSet());
		TestTrue(TEXT("struct member still kept"), Asset != nullptr && Asset->Settings.Label == TEXT("Main"));
	}

	// A value of the wrong type for the optional is refused.
	TestFalse(TEXT("wrong value type refused"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		CodecProperties(Document)->SetStringField(TEXT("OptionalCount"), TEXT("many"));
	}, TEXT("Wrong type")).IsSuccess());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentCodecGenericInstancedFieldsTest, "UEAssetDoc.Codec.Generic.InstancedFields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentCodecGenericInstancedFieldsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentCodecGenericTestsLocal;
	const FTarget Target(TEXT("DA_CodecInstancedFields"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	// The Details panel: the inline effect's values typed in; its saved state set by the editor.
	UAssetDocumentCodecTestAsset* Asset = Target.Create<UAssetDocumentCodecTestAsset>();
	Asset->Effect = NewObject<UAssetDocumentCodecTestEffect>(Asset);
	Asset->Effect->Strength = 1.0f;
	Asset->Effect->Preview = TEXT("Preview pose");
	Asset->Effect->SavedState = 42;
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Instanced fields"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("DuplicateTransient field reads"), StringAt(Extracted, TEXT("/Properties/Effect/Properties/Preview")), FString(TEXT("Preview pose")));
	TestFalse(TEXT("field with no Details form is not read"), At(Extracted, TEXT("/Properties/Effect/Properties/SavedState")).IsValid());

	// Editing one field keeps the others, also the one the document has no form for.
	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Effect = CodecObjectAt(Document, TEXT("Effect"));
		Effect->GetObjectField(TEXT("Properties"))->SetNumberField(TEXT("Strength"), 2.0);
		CodecProperties(Document)->SetObjectField(TEXT("Effect"), Effect);
	}, TEXT("Edit"));
	if (TestTrue(TEXT("edit applies"), Edited.IsSuccess()))
	{
		Asset = Target.Get<UAssetDocumentCodecTestAsset>();
		if (TestTrue(TEXT("effect after edit"), Asset != nullptr && Asset->Effect != nullptr))
		{
			TestEqual(TEXT("edited field"), Asset->Effect->Strength, 2.0f);
			TestEqual(TEXT("DuplicateTransient field kept"), Asset->Effect->Preview, FString(TEXT("Preview pose")));
			TestEqual(TEXT("field with no Details form kept"), Asset->Effect->SavedState, 42);
		}
	}

	// The DuplicateTransient field is written like the others.
	const FAssetDocumentResult Written = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		SetStringAt(Document, TEXT("/Properties/Effect/Properties/Preview"), TEXT("Other pose"));
	}, TEXT("Write DuplicateTransient"));
	if (TestTrue(TEXT("DuplicateTransient write applies"), Written.IsSuccess()))
	{
		Asset = Target.Get<UAssetDocumentCodecTestAsset>();
		TestTrue(TEXT("DuplicateTransient field written, the rest kept"), Asset != nullptr && Asset->Effect != nullptr
			&& Asset->Effect->Preview == TEXT("Other pose") && Asset->Effect->SavedState == 42 && Asset->Effect->Strength == 2.0f);
	}
	return !HasAnyErrors();
}

#endif
