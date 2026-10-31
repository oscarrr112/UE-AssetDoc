// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Editor-made data assets that break rules written for new input: they read with warnings, their own extract
// re-applies as a no-op, other edits work, and the rules still refuse new input.

#include "AssetDocumentTolerantTestSupport.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraphSchema_K2.h"
#include "Engine/UserDefinedEnum.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Kismet2/StructureEditorUtils.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantStringTableTest, "UEAssetDoc.Tolerant.StringTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantStringTableTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	const FTarget Target(TEXT("ST_Imported"));
	const FString Csv = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("AssetDocumentTolerant") / (Target.AssetName + TEXT(".csv")));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
		IFileManager::Get().Delete(*Csv, false, true);
	};

	// The String Table editor's Import from CSV: any key, an empty source string, and an extra column that becomes
	// a metadata id with a space.
	UStringTable* Table = Target.Create<UStringTable>();
	Table->GetMutableStringTable()->SetNamespace(TEXT("UI"));
	FFileHelper::SaveStringToFile(TEXT("Key,SourceString,Translator Comment\n\"Main Menu\",Start,For the button\nMenu.Quit,,\n"), *Csv);
	if (!TestTrue(TEXT("CSV import"), Table->GetMutableStringTable()->ImportStrings(Csv)) || !TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	FExpected Expected;
	Expected.Warnings = {
		{TEXT("StringTable.InvalidKey"), TEXT("/Body/Entries/Main Menu")},
		{TEXT("StringTable.EmptySourceString"), TEXT("/Body/Entries/Menu.Quit")},
		{TEXT("StringTable.InvalidRowName"), TEXT("/Body/MetaData/Main Menu/Translator Comment")}};
	if (!TestTolerantRoundTrip(*this, Target, Expected, TEXT("Imported table")).IsValid())
	{
		return false;
	}

	// Another edit: a new valid entry, and a new source string for the imported key (the key itself is unchanged).
	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Entries = Document->GetObjectField(TEXT("Body"))->GetObjectField(TEXT("Entries"));
		Entries->SetStringField(TEXT("Menu.Options"), TEXT("Options"));
		Entries->SetStringField(TEXT("Main Menu"), TEXT("Play"));
	}, TEXT("Edit"));
	if (TestTrue(TEXT("edit applies"), Edited.IsSuccess()))
	{
		FString Source;
		Table = Target.Get<UStringTable>();
		TestTrue(TEXT("imported key edited"), Table != nullptr && Table->GetStringTable()->GetSourceString(FTextKey(TEXT("Main Menu")), Source) && Source == TEXT("Play"));
		TestStillShows(*this, Target, TEXT("StringTable.InvalidKey"), TEXT("/Body/Entries/Main Menu"), TEXT("After edit"));
	}

	// New input still follows the editor's rules.
	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		Document->GetObjectField(TEXT("Body"))->GetObjectField(TEXT("Entries"))->SetStringField(TEXT("Bad Key"), TEXT("x"));
	}, TEXT("New spaced key")), TEXT("StringTable.InvalidKey"), TEXT("/Body/Entries/Bad Key"), TEXT("New spaced key"));
	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		Document->GetObjectField(TEXT("Body"))->GetObjectField(TEXT("Entries"))->SetStringField(TEXT("Menu.Options"), TEXT(""));
	}, TEXT("Emptied source")), TEXT("StringTable.EmptySourceString"), TEXT("/Body/Entries/Menu.Options"), TEXT("Emptied source"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantUserDefinedEnumTest, "UEAssetDoc.Tolerant.UserDefinedEnum",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantUserDefinedEnumTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	const FTarget Target(TEXT("E_Attack"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	// The Enum editor accepts a display name with a trailing space.
	UUserDefinedEnum* Enum = Cast<UUserDefinedEnum>(FEnumEditorUtils::CreateUserDefinedEnum(CreatePackage(*Target.PackageName),
		FName(*Target.AssetName), RF_Public | RF_Standalone | RF_Transactional));
	if (!TestNotNull(TEXT("enum"), Enum))
	{
		return false;
	}
	FAssetRegistryModule::AssetCreated(Enum);
	// A new enum holds only its hidden _MAX; the editor's Add Enumerator button adds each one.
	FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Enum);
	FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Enum);
	if (!TestTrue(TEXT("trailing-space name"), FEnumEditorUtils::SetEnumeratorDisplayName(Enum, 0, FText::FromString(TEXT("Melee "))))
		|| !TestTrue(TEXT("second name"), FEnumEditorUtils::SetEnumeratorDisplayName(Enum, 1, FText::FromString(TEXT("Ranged"))))
		|| !TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	FExpected Expected;
	Expected.Warnings = {{TEXT("UserDefinedEnum.InvalidEnumeratorName"), TEXT("/Body/Enumerators/0/Name")}};
	if (!TestTolerantRoundTrip(*this, Target, Expected, TEXT("Spaced enumerator")).IsValid())
	{
		return false;
	}

	auto Append = [](const TCHAR* Name)
	{
		return [Name](const TSharedPtr<FJsonObject>& Document)
		{
			TArray<TSharedPtr<FJsonValue>> Rows = Document->GetObjectField(TEXT("Body"))->GetArrayField(TEXT("Enumerators"));
			const TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("Name"), Name);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
			Document->GetObjectField(TEXT("Body"))->SetArrayField(TEXT("Enumerators"), Rows);
		};
	};
	if (TestTrue(TEXT("another enumerator is added"), ApplyEdited(*this, Target, Append(TEXT("Magic")), TEXT("Edit")).IsSuccess()))
	{
		Enum = Target.Get<UUserDefinedEnum>();
		TestTrue(TEXT("spaced name kept"), Enum != nullptr && Enum->GetDisplayNameTextByIndex(0).ToString() == TEXT("Melee "));
		TestStillShows(*this, Target, TEXT("UserDefinedEnum.InvalidEnumeratorName"), TEXT("/Body/Enumerators/0/Name"), TEXT("After edit"));
	}
	TestRefused(*this, ApplyEdited(*this, Target, Append(TEXT("Stealth ")), TEXT("New spaced name")),
		TEXT("UserDefinedEnum.InvalidEnumeratorName"), TEXT("/Body/Enumerators/3/Name"), TEXT("New spaced name"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantUserDefinedStructTest, "UEAssetDoc.Tolerant.UserDefinedStruct",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantUserDefinedStructTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	const FTarget Target(TEXT("S_Hit"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	// The Struct editor keeps a member name with a trailing space.
	UUserDefinedStruct* Struct = FStructureEditorUtils::CreateUserDefinedStruct(CreatePackage(*Target.PackageName),
		FName(*Target.AssetName), RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("struct"), Struct))
	{
		return false;
	}
	FAssetRegistryModule::AssetCreated(Struct);
	const TArray<FStructVariableDescription>& Members = FStructureEditorUtils::GetVarDesc(Struct);
	if (!TestEqual(TEXT("one default member"), Members.Num(), 1)
		|| !TestTrue(TEXT("trailing-space name"), FStructureEditorUtils::RenameVariable(Struct, Members[0].VarGuid, TEXT("Damage ")))
		|| !TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	FExpected Expected;
	Expected.Warnings = {{TEXT("UserDefinedStruct.InvalidMemberName"), TEXT("/Body/Members/0/Name")}};
	if (!TestTolerantRoundTrip(*this, Target, Expected, TEXT("Spaced member")).IsValid())
	{
		return false;
	}

	auto Append = [](const TCHAR* Name)
	{
		return [Name](const TSharedPtr<FJsonObject>& Document)
		{
			TArray<TSharedPtr<FJsonValue>> Rows = Document->GetObjectField(TEXT("Body"))->GetArrayField(TEXT("Members"));
			const TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("Name"), Name);
			Row->SetStringField(TEXT("Type"), TEXT("int"));
			Rows.Add(MakeShared<FJsonValueObject>(Row));
			Document->GetObjectField(TEXT("Body"))->SetArrayField(TEXT("Members"), Rows);
		};
	};
	if (TestTrue(TEXT("another member is added"), ApplyEdited(*this, Target, Append(TEXT("Count")), TEXT("Edit")).IsSuccess()))
	{
		Struct = Target.Get<UUserDefinedStruct>();
		TestTrue(TEXT("spaced name kept"), Struct != nullptr && FStructureEditorUtils::GetVarDesc(Struct).ContainsByPredicate(
			[](const FStructVariableDescription& Member) { return Member.FriendlyName == TEXT("Damage "); }));
		TestStillShows(*this, Target, TEXT("UserDefinedStruct.InvalidMemberName"), TEXT("/Body/Members/0/Name"), TEXT("After edit"));
	}
	TestRefused(*this, ApplyEdited(*this, Target, Append(TEXT(" Armor")), TEXT("New spaced name")),
		TEXT("UserDefinedStruct.InvalidMemberName"), TEXT("/Body/Members/2/Name"), TEXT("New spaced name"));
	return !HasAnyErrors();
}

#endif
