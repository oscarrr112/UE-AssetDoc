// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Editor-made fixtures for the tolerant-read tests: an asset built through the engine API an editor uses, saved,
// then read and re-applied through the production Service exactly as the MCP tools do. Header only, test builds only.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Internationalization/Text.h"
#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

namespace AssetDocumentTolerantTestSupport
{
inline const TCHAR* FolderName = TEXT("AssetDocumentTolerant");

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
		PackageName = FString(TEXT("/Game/")) + FolderName + TEXT("/") + AssetName;
		ObjectPath = PackageName + TEXT(".") + AssetName;
		PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		SidecarPath = FPaths::Combine(FPaths::ProjectContentDir(), FolderName, AssetName + TEXT(".assetdoc.json"));
	}

	UObject* Get() const { return FSoftObjectPath(ObjectPath).ResolveObject(); }

	template <typename T>
	T* Get() const { return Cast<T>(Get()); }

	// A new asset of Class in this target's package, registered as the Content Browser's New Asset does.
	template <typename T>
	T* Create(UClass* Class = T::StaticClass()) const
	{
		UPackage* Package = CreatePackage(*PackageName);
		T* Asset = NewObject<T>(Package, Class, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Asset);
		Asset->MarkPackageDirty();
		return Asset;
	}

	// Saves the package as File > Save does; the asset is then clean, as an apply requires.
	bool Save() const
	{
		UObject* Asset = Get();
		return Asset != nullptr && UEditorLoadingAndSavingUtils::SavePackages({Asset->GetOutermost()}, false);
	}

	// Force Delete (the delete dialog), which also clears references to the fixture.
	void Cleanup() const
	{
		if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting && !GEditor->IsTransactionActive())
		{
			GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentTolerantTests", "Cleanup", "AssetDocument tolerant test cleanup"));
		}
		if (UObject* Asset = Get())
		{
			Asset->GetOutermost()->ClearDirtyFlag();
			ObjectTools::ForceDeleteObjects({Asset}, false);
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		if (IFileManager::Get().FileExists(*PackageFile))
		{
			IFileManager::Get().Delete(*PackageFile, false, true);
			FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({PackageFile});
		}
		IFileManager::Get().Delete(*SidecarPath, false, true);
	}
};

inline FString Serialize(const TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	if (Object.IsValid())
	{
		const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}
	return Text;
}

// Parses as the MCP routes and sidecar files do: each number keeps its written form, so 1 reaches the profiles as an
// integer and 1.0 as a floating-point number.
inline TSharedPtr<FJsonObject> Parse(const FString& Json)
{
	TSharedPtr<FJsonObject> Object;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object, FJsonSerializer::EFlags::StoreNumbersAsStrings);
	return Object;
}

inline void LogResult(FAutomationTestBase& Test, const FString& Stage, const FAssetDocumentResult& Result)
{
	Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *Result.Message));
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Test.AddInfo(FString::Printf(TEXT("%s: [%s %s] %s %s"), *Stage, *Diagnostic.Severity, *Diagnostic.Code,
			*Diagnostic.Path, *Diagnostic.Message));
	}
}

// The value at a JSON Pointer ("/Body/Entries/Main Menu"; ~0 and ~1 escapes), or null.
inline TSharedPtr<FJsonValue> At(const TSharedPtr<FJsonObject>& Root, const FString& Pointer)
{
	TSharedPtr<FJsonValue> Value = MakeShared<FJsonValueObject>(Root);
	TArray<FString> Tokens;
	Pointer.ParseIntoArray(Tokens, TEXT("/"), false);
	for (int32 Index = 1; Index < Tokens.Num() && Value.IsValid(); ++Index)
	{
		const FString Token = Tokens[Index].Replace(TEXT("~1"), TEXT("/")).Replace(TEXT("~0"), TEXT("~"));
		if (Value->Type == EJson::Object)
		{
			Value = Value->AsObject()->TryGetField(Token);
		}
		else if (Value->Type == EJson::Array && Token.IsNumeric())
		{
			const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
			const int32 Item = FCString::Atoi(*Token);
			Value = Items.IsValidIndex(Item) ? Items[Item] : nullptr;
		}
		else
		{
			Value = nullptr;
		}
	}
	return Value;
}

inline bool HasDiagnostic(const FAssetDocumentResult& Result, const FString& Code, const FString& Path, const TCHAR* Severity = nullptr)
{
	return Result.Diagnostics.ContainsByPredicate([&](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == Code && (Path.IsEmpty() || Diagnostic.Path == Path)
			&& (Severity == nullptr || Diagnostic.Severity == Severity);
	});
}

inline FAssetDocumentResult ExtractResult(const FTarget& Target)
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = Target.ObjectPath;
	return FAssetDocumentModule::Get().GetService().Extract(Request);
}

inline FAssetDocumentResult ApplyText(FAutomationTestBase& Test, const FTarget& Target, const FString& Json, const FString& Stage)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target.SidecarPath), true);
	FFileHelper::SaveStringToFile(Json, *Target.SidecarPath);
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Target.SidecarPath;
	Request.bSaveAsset = true;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
	LogResult(Test, Stage, Result);
	return Result;
}

// Extracts, lets Edit change the document, and applies it as an Update.
inline FAssetDocumentResult ApplyEdited(FAutomationTestBase& Test, const FTarget& Target,
	TFunctionRef<void(const TSharedPtr<FJsonObject>& Document)> Edit, const FString& Stage)
{
	const FAssetDocumentResult Extracted = ExtractResult(Target);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: extract succeeds"), *Stage), Extracted.IsSuccess() && Extracted.Payload.IsValid()))
	{
		LogResult(Test, Stage, Extracted);
		return Extracted;
	}
	const TSharedPtr<FJsonObject> Document = Extracted.Payload;
	Document->SetStringField(TEXT("Action"), TEXT("Update"));
	Edit(Document);
	return ApplyText(Test, Target, Serialize(Document), Stage);
}

struct FExpected
{
	// Read warnings Extract must carry: {code, JSON Pointer}.
	TArray<TPair<FString, FString>> Warnings;
	// JSON Pointers of the "Unsupported" markers Extract must carry.
	TArray<FString> Markers;
};

// The three invariants on one editor-made asset: Extract and Inspect read it with the expected warnings and
// markers; Diff of the extract is empty and Validate says it would change nothing; applying the extract saves
// nothing. Returns the extract (nullptr when it failed).
inline TSharedPtr<FJsonObject> TestTolerantRoundTrip(FAutomationTestBase& Test, const FTarget& Target,
	const FExpected& Expected, const FString& Stage)
{
	const FAssetDocumentResult Extracted = ExtractResult(Target);
	LogResult(Test, Stage + TEXT(" extract"), Extracted);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: extract succeeds"), *Stage), Extracted.IsSuccess() && Extracted.Payload.IsValid()))
	{
		return nullptr;
	}
	for (const TPair<FString, FString>& Warning : Expected.Warnings)
	{
		Test.TestTrue(*FString::Printf(TEXT("%s: extract warns %s at %s"), *Stage, *Warning.Key, *Warning.Value),
			HasDiagnostic(Extracted, Warning.Key, Warning.Value, TEXT("warning")));
	}
	for (const FString& Pointer : Expected.Markers)
	{
		const TSharedPtr<FJsonValue> Marker = At(Extracted.Payload, Pointer);
		FString Reason;
		Test.TestTrue(*FString::Printf(TEXT("%s: marker at %s"), *Stage, *Pointer), Marker.IsValid()
			&& Marker->Type == EJson::Object && Marker->AsObject()->TryGetStringField(TEXT("Unsupported"), Reason) && !Reason.IsEmpty());
	}
	const FString Baseline = Serialize(Extracted.Payload);
	Test.AddInfo(FString::Printf(TEXT("%s: extracted %s"), *Stage, *Baseline));

	FAssetDocumentInspectRequest InspectRequest;
	InspectRequest.ClassOrAsset = Target.ObjectPath;
	const FAssetDocumentResult Inspected = FAssetDocumentModule::Get().GetService().Inspect(InspectRequest);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: inspect succeeds"), *Stage), Inspected.IsSuccess()))
	{
		LogResult(Test, Stage + TEXT(" inspect"), Inspected);
	}

	const TSharedPtr<FJsonObject> Update = Parse(Baseline);
	Update->SetStringField(TEXT("Action"), TEXT("Update"));
	const FString UpdateText = Serialize(Update);

	FAssetDocumentDiffRequest DiffRequest;
	DiffRequest.Document = Parse(UpdateText);
	const FAssetDocumentResult Diffed = FAssetDocumentModule::Get().GetService().Diff(DiffRequest);
	const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: diff of the extract is empty"), *Stage), Diffed.IsSuccess() && Diffed.Payload.IsValid()
		&& Diffed.Payload->TryGetArrayField(TEXT("changed"), Changed) && Changed->IsEmpty()))
	{
		LogResult(Test, Stage + TEXT(" diff"), Diffed);
		if (Diffed.Payload.IsValid())
		{
			Test.AddInfo(Serialize(Diffed.Payload));
		}
	}

	FAssetDocumentValidateRequest ValidateRequest;
	ValidateRequest.Document = Parse(UpdateText);
	const FAssetDocumentResult Validated = FAssetDocumentModule::Get().GetService().Validate(ValidateRequest);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: validate of the extract changes nothing"), *Stage),
		Validated.IsSuccess() && Validated.Message.Contains(TEXT("would change nothing"))))
	{
		LogResult(Test, Stage + TEXT(" validate"), Validated);
	}

	UObject* Asset = Target.Get();
	if (Test.TestNotNull(*FString::Printf(TEXT("%s: asset loaded"), *Stage), Asset))
	{
		Test.TestFalse(*FString::Printf(TEXT("%s: package is saved clean"), *Stage), Asset->GetOutermost()->IsDirty());
		const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Target.PackageFile);
		const FAssetDocumentResult Reapplied = ApplyText(Test, Target, UpdateText, Stage + TEXT(" re-apply"));
		Test.TestTrue(*FString::Printf(TEXT("%s: the extract re-applies"), *Stage), Reapplied.IsSuccess());
		Test.TestFalse(*FString::Printf(TEXT("%s: re-apply saves nothing"), *Stage), Reapplied.bSavedAsset);
		Test.TestEqual(*FString::Printf(TEXT("%s: package file untouched"), *Stage), IFileManager::Get().GetTimeStamp(*Target.PackageFile), Stamp);
	}
	return Extracted.Payload;
}

// After an edit: Extract still succeeds and still shows the anomaly (a warning, or the marker) at Pointer.
inline void TestStillShows(FAutomationTestBase& Test, const FTarget& Target, const FString& Code, const FString& Pointer, const FString& Stage)
{
	const FAssetDocumentResult Extracted = ExtractResult(Target);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: extract succeeds"), *Stage), Extracted.IsSuccess() && Extracted.Payload.IsValid()))
	{
		LogResult(Test, Stage, Extracted);
		return;
	}
	const bool bWarned = HasDiagnostic(Extracted, Code, Pointer, TEXT("warning"));
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: %s still at %s"), *Stage, *Code, *Pointer), bWarned))
	{
		LogResult(Test, Stage, Extracted);
	}
}

inline bool TestRefused(FAutomationTestBase& Test, const FAssetDocumentResult& Result, const FString& Code, const FString& Path, const FString& Stage)
{
	Test.TestFalse(*FString::Printf(TEXT("%s: refused"), *Stage), Result.IsSuccess());
	return Test.TestTrue(*FString::Printf(TEXT("%s: %s at %s"), *Stage, *Code, *Path), HasDiagnostic(Result, Code, Path));
}

// ---- Localized texts ------------------------------------------------------------------------------------------------

// A String Table asset made in its editor (Target's asset: a namespace, then each row's key and source string), saved.
// Null when it could not be saved.
inline UStringTable* CreateStringTable(const FTarget& Target, const FString& Namespace, const TArray<TPair<FString, FString>>& Rows)
{
	UStringTable* Table = Target.Create<UStringTable>();
	Table->GetMutableStringTable()->SetNamespace(Namespace);
	for (const TPair<FString, FString>& Row : Rows)
	{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
		Table->GetMutableStringTable()->SetSourceString(Row.Key, Row.Value, FString());
#else
		Table->GetMutableStringTable()->SetSourceString(Row.Key, Row.Value);
#endif
	}
	return Target.Save() ? Table : nullptr;
}

// The text of Table's row Key, as a text box's String Table picker sets it.
inline FText StringTableText(const FTarget& Table, const FString& Key)
{
	return FText::FromStringTable(FName(*Table.ObjectPath), Key);
}

// The document form of that text: LOCTABLE("<table object path>", "<key>").
inline FString StringTableForm(const FTarget& Table, const FString& Key)
{
	return FString::Printf(TEXT("LOCTABLE(\"%s\", \"%s\")"), *Table.ObjectPath, *Key);
}

// Whether Text is still Table's row Key (the String Table reference, not a copy of its string).
inline bool IsStringTableText(const FText& Text, const FTarget& Table, const FString& Key)
{
	FName TableId;
	FString TextKey;
	return Text.IsFromStringTable() && FTextInspector::GetTableIdAndKey(Text, TableId, TextKey)
		&& TableId == FName(*Table.ObjectPath) && TextKey == Key;
}

// The string at Pointer in Document (empty when absent or not a string).
inline FString StringAt(const TSharedPtr<FJsonObject>& Document, const FString& Pointer)
{
	const TSharedPtr<FJsonValue> Value = Document.IsValid() ? At(Document, Pointer) : nullptr;
	return Value.IsValid() && Value->Type == EJson::String ? Value->AsString() : FString();
}

// Sets the string at Pointer in Document; its parent object must exist. False when it does not.
inline bool SetStringAt(const TSharedPtr<FJsonObject>& Document, const FString& Pointer, const FString& Value)
{
	int32 Slash = INDEX_NONE;
	if (!Document.IsValid() || !Pointer.FindLastChar(TEXT('/'), Slash))
	{
		return false;
	}
	const TSharedPtr<FJsonValue> Parent = At(Document, Pointer.Left(Slash));
	if (!Parent.IsValid() || Parent->Type != EJson::Object)
	{
		return false;
	}
	Parent->AsObject()->SetStringField(Pointer.Mid(Slash + 1).Replace(TEXT("~1"), TEXT("/")).Replace(TEXT("~0"), TEXT("~")), Value);
	return true;
}

// Applies Document (an extract with an edit) and expects the apply to save nothing: the edit names what the asset holds.
inline void TestAppliesUnchanged(FAutomationTestBase& Test, const FTarget& Target, const TSharedPtr<FJsonObject>& Document, const FString& Stage)
{
	Document->SetStringField(TEXT("Action"), TEXT("Update"));
	const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Target.PackageFile);
	const FAssetDocumentResult Applied = ApplyText(Test, Target, Serialize(Document), Stage);
	Test.TestTrue(*FString::Printf(TEXT("%s: applies"), *Stage), Applied.IsSuccess());
	Test.TestFalse(*FString::Printf(TEXT("%s: saves nothing"), *Stage), Applied.bSavedAsset);
	Test.TestEqual(*FString::Printf(TEXT("%s: package file untouched"), *Stage), IFileManager::Get().GetTimeStamp(*Target.PackageFile), Stamp);
}
}

#endif
