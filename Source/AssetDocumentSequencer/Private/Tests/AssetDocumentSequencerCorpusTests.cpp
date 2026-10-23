// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// UEAssetDoc.Sequencer.Corpus: design §6.1 (template folders copied in at their /Game paths, plugin
//   content when enabled): every sequence extracts with no unknown diagnostic, twice equal and equal after reload, Diff
//   empty, Inspect works, reading dirties nothing; per-asset timings (the smoke run).
// UEAssetDoc.Sequencer.Inspect: Inspect resolves the possessables' locators of #8 (Seq_SunSkyEquinox)
//   in the copied map /Game/Main opened in the editor and reports them unresolved while another map is open; #1's
//   locator (a component of its own spawnable) resolves in the spawnable's template.

#include "AssetDocumentSequencerTestSupport.h"

#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentSequencerTests
{
TArray<FString> CheckStable(FAutomationTestBase& Test, const FString& ObjectPath, const FString& Name, FString& OutTimings,
	TSharedPtr<FJsonObject>& OutExtracted, FAssetDocumentResult& OutInspected);

FString CorpusCounts(const TSharedPtr<FJsonObject>& Document, const FAssetDocumentResult& Inspected)
{
	const TSharedPtr<FJsonObject> Body = BodyOf(Document);
	TMap<FString, int32> Codes;
	for (const TSharedPtr<FJsonObject>& Warning : ReadWarnings(Inspected))
	{
		++Codes.FindOrAdd(Warning->GetStringField(TEXT("Code")));
	}
	TArray<FString> Listed;
	for (const TPair<FString, int32>& Code : Codes)
	{
		Listed.Add(FString::Printf(TEXT("%s x%d"), *Code.Key, Code.Value));
	}
	return FString::Printf(TEXT("bindings %d (facts %d), global tracks %d, markers %d; warnings: %s"), CountField(Body, TEXT("Bindings")),
		CountField(Inspected.Payload, TEXT("BindingFacts")), CountField(Body, TEXT("Tracks")), CountField(Body, TEXT("Markers")),
		Listed.IsEmpty() ? TEXT("none") : *FString::Join(Listed, TEXT(", ")));
}

// Opens a copied map as the editor world (unattended: no save prompt for the map that was open).
UWorld* OpenMap(const FString& PackageName)
{
	TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetMapPackageExtension());
	return FEditorFileUtils::LoadMap(Filename, /*LoadAsTemplate*/ false, /*bShowProgress*/ false) ? GEditor->GetEditorWorldContext().World() : nullptr;
}

void NewUntitledMap()
{
	TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
	GEditor->NewMap(false);
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
}

// {resolved, total} locators over the binding facts of an Inspect payload.
TPair<int32, int32> CountResolved(FAutomationTestBase& Test, const FAssetDocumentResult& Inspected, const FString& Stage)
{
	int32 Resolved = 0;
	int32 Total = 0;
	const TArray<TSharedPtr<FJsonValue>>* Facts = nullptr;
	if (!Inspected.Payload.IsValid() || !Inspected.Payload->TryGetArrayField(TEXT("BindingFacts"), Facts))
	{
		return {0, 0};
	}
	for (const TSharedPtr<FJsonValue>& Fact : *Facts)
	{
		const TArray<TSharedPtr<FJsonValue>>* Locators = nullptr;
		if (!Fact->AsObject()->TryGetArrayField(TEXT("Locators"), Locators))
		{
			continue;
		}
		for (const TSharedPtr<FJsonValue>& Locator : *Locators)
		{
			++Total;
			const bool bResolved = Locator->AsObject()->GetBoolField(TEXT("Resolved"));
			Resolved += bResolved ? 1 : 0;
			Test.AddInfo(FString::Printf(TEXT("%s: %s %s"), *Stage, *Fact->AsObject()->GetStringField(TEXT("Path")), *Serialize(Locator->AsObject())));
		}
	}
	return {Resolved, Total};
}
} // namespace AssetDocumentSequencerTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequencerCorpusTest, "UEAssetDoc.Sequencer.Corpus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSequencerCorpusTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSequencerTests;
	const FContentCopies Copies = CorpusCopies();
	const double Start = FPlatformTime::Seconds();
	int32 Tried = 0;
	int32 Passed = 0;
	for (const FCorpusEntry& Entry : Corpus)
	{
		const FString Name = CorpusName(Entry);
		if (LoadCorpus(Entry) == nullptr)
		{
			AddInfo(FString::Printf(TEXT("SKIP %s: %s"), *Name, Entry.Plugin != nullptr && !PluginEnabled(Entry.Plugin)
				? *FString::Printf(TEXT("plugin %s is not enabled"), Entry.Plugin) : TEXT("not installed")));
			continue;
		}
		++Tried;
		FString Timings;
		TSharedPtr<FJsonObject> Extracted;
		FAssetDocumentResult Inspected;
		const TArray<FString> Problems = CheckStable(*this, Entry.Path, Name, Timings, Extracted, Inspected);
		for (const FString& Problem : Problems)
		{
			AddError(FString::Printf(TEXT("%s: %s"), *Name, *Problem));
		}
		Passed += Problems.IsEmpty() ? 1 : 0;
		AddInfo(FString::Printf(TEXT("%s %s (%s): %s; %s"), Problems.IsEmpty() ? TEXT("PASS") : TEXT("FAIL"), *Name, Entry.Why, *Timings,
			*CorpusCounts(Extracted, Inspected)));
	}
	AddInfo(FString::Printf(TEXT("corpus: %d of %d read cleanly in %.1f s (copied %d files)"), Passed, Tried, Milliseconds(Start) / 1000.0,
		Copies.CopiedFiles.Num()));
	TestTrue(TEXT("the template corpus is read"), Tried >= 10);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequencerInspectTest, "UEAssetDoc.Sequencer.Inspect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSequencerInspectTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSequencerTests;
	// #1: its only locator names the camera component of the sequence's own spawnable: resolved in the spawnable's saved
	// template (the spawned actor exists only while the sequence plays), whatever map is open.
	{
		const FContentCopies Copies({{TEXT("Templates/TP_UEIntro_BP/Content"), TEXT("DemoTemplate/Sequencer")}});
		const TCHAR* Sequence = TEXT("/Game/DemoTemplate/Sequencer/NewLevelSequence.NewLevelSequence");
		if (TestNotNull(TEXT("NewLevelSequence copied"), LoadQuiet(Sequence)))
		{
			const FAssetDocumentResult Inspected = Inspect(Sequence);
			TestTrue(*FString::Printf(TEXT("NewLevelSequence: inspect (%s)"), *DiagnosticsText(Inspected)), Inspected.IsSuccess());
			const TPair<int32, int32> Resolved = CountResolved(*this, Inspected, TEXT("NewLevelSequence"));
			TestTrue(TEXT("NewLevelSequence: the spawnable's component locator is listed"), Resolved.Value > 0);
			TestEqual(TEXT("NewLevelSequence: the component resolves in the spawnable's template"), Resolved.Key, Resolved.Value);
			TestTrue(TEXT("NewLevelSequence: marked SpawnableTemplate"), Serialize(Inspected.Payload).Contains(TEXT("\"SpawnableTemplate\":true")));
		}
	}
	// #8: possessables in the Archvis template's map (/Game/Main with the Building pack it uses).
	if (!PluginEnabled(TEXT("SunPosition")))
	{
		AddInfo(TEXT("SKIP Seq_SunSkyEquinox: plugin SunPosition is not enabled"));
		return true;
	}
	const TCHAR* Sequence = TEXT("/Game/ArchvisProject/Sequences/SunSky/Seq_SunSkyEquinox.Seq_SunSkyEquinox");
	const TCHAR* Map = TEXT("/Game/Main");
	const FContentCopies Copies({{TEXT("Templates/TP_AEC_ArchvisBP/Content"), TEXT("ArchvisProject")},
		{TEXT("Templates/TP_AEC_ArchvisBP/Content"), TEXT("Main.umap")},
		{TEXT("Templates/TemplateResources/Standard/Building/Content"), TEXT("Geometry"), TEXT("Building")},
		{TEXT("Templates/TemplateResources/Standard/Building/Content"), TEXT("Materials"), TEXT("Building")},
		{TEXT("Templates/TemplateResources/Standard/Building/Content"), TEXT("Textures"), TEXT("Building")}});
	ON_SCOPE_EXIT
	{
		NewUntitledMap();
	};
	if (!FPackageName::DoesPackageExist(Map) || LoadQuiet(Sequence) == nullptr)
	{
		AddError(TEXT("Seq_SunSkyEquinox: the copied map or sequence is missing"));
		return true;
	}
	// Another map open: the locators do not resolve (reported, not an error).
	NewUntitledMap();
	const TPair<int32, int32> Elsewhere = CountResolved(*this, Inspect(Sequence), TEXT("Seq_SunSkyEquinox (untitled map)"));
	TestTrue(TEXT("Seq_SunSkyEquinox: has locators"), Elsewhere.Value > 0);
	TestEqual(TEXT("Seq_SunSkyEquinox: nothing resolves in an untitled map"), Elsewhere.Key, 0);

	const double Start = FPlatformTime::Seconds();
	UWorld* World = OpenMap(Map);
	const double OpenMs = Milliseconds(Start);
	if (!TestNotNull(TEXT("Seq_SunSkyEquinox: map /Game/Main opens"), World))
	{
		return true;
	}
	const FReadSideEffects SideEffects;
	const double InspectStart = FPlatformTime::Seconds();
	const FAssetDocumentResult Inspected = Inspect(Sequence);
	const double InspectMs = Milliseconds(InspectStart);
	if (!TestTrue(*FString::Printf(TEXT("Seq_SunSkyEquinox: inspect (%s)"), *DiagnosticsText(Inspected)), Inspected.IsSuccess()))
	{
		return true;
	}
	const TPair<int32, int32> Resolved = CountResolved(*this, Inspected, TEXT("Seq_SunSkyEquinox"));
	TestTrue(TEXT("Seq_SunSkyEquinox: has locators"), Resolved.Value > 0);
	TestEqual(TEXT("Seq_SunSkyEquinox: every locator resolves in /Game/Main"), Resolved.Key, Resolved.Value);
	TArray<FString> Problems;
	SideEffects.Check(Problems);
	for (const FString& Problem : Problems)
	{
		AddError(FString::Printf(TEXT("Seq_SunSkyEquinox: %s"), *Problem));
	}
	AddInfo(FString::Printf(TEXT("Seq_SunSkyEquinox: map opened in %.0f ms, inspect %.0f ms, %d/%d locators resolved"), OpenMs, InspectMs,
		Resolved.Key, Resolved.Value));
	return true;
}

#endif
