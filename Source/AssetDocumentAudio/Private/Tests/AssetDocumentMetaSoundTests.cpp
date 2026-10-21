// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// MetaSound documents (reading), through the production Service as the MCP tools send them:
//
// UEAssetDoc.MetaSound.Python: a /Game duplicate of the canonical Epic source
// (MSS_PlayRandom_Oneshot) extracts through the Python Profile (profile.metasound_source.v1, audio.* primitives)
// with its interfaces, members, nodes, connections and comments; Diff of the extraction is empty; Inspect adds
// pins, interfaces and pages; the unchanged extraction applies as a no-op; reading registers, dirties and leaves
// nothing.
// UEAssetDoc.MetaSound.Corpus: the design §6.1 corpus (the Metasound plugin's sources, the
// TP_UEIntro_BP template's MetaSounds copied into /Game as the template does, and Harmonix when it is enabled;
// absent content is skipped with a note). Each asset is duplicated into /Game and saved; its extraction is stable
// when read twice, after unloading and reloading the package and on a second duplicate (Target aside). Reading
// leaves no dirty package, no builder document and the registration state unchanged. Timings are reported.
// UEAssetDoc.MetaSound.EditorOpen: the extraction (which simulates the editor's open-time input
// template injection on a copy) equals the extraction after the editor's own open path (FindOrBeginBuilding,
// VersionAsset, FInputNodeTemplate::Inject) has run on the asset, node ids aside.
// UEAssetDoc.MetaSound.ListTypes (node families as one row): list_types kinds metasound_node / metasound_datatype /
// metasound_interface, their filters and paging.

#include "AssetDocumentMetaSoundTestSupport.h"
#include "Misc/EngineVersionComparison.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMetaSoundPythonTest, "UEAssetDoc.MetaSound.Python",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMetaSoundPythonTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMetaSoundTests;
	UObject* Source = LoadQuiet(OneShot);
	if (!TestNotNull(TEXT("the Metasound plugin's MSS_PlayRandom_Oneshot loads"), Source))
	{
		return false;
	}
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("MSS_Read"));
	UObject* Asset = Duplicate(*this, Source, Target);
	if (!TestNotNull(TEXT("duplicate"), Asset))
	{
		return false;
	}

	const FReadSideEffects SideEffects(*Asset);
	const TSharedPtr<FJsonObject> Extracted = Extract(*this, Target.ObjectPath, TEXT("Extract"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("extracted %s"), *SerializePretty(Extracted)));
	TestEqual(TEXT("Class"), Extracted->GetStringField(TEXT("Class")), FString(SourceClass));
	TestEqual(TEXT("Target"), Extracted->GetStringField(TEXT("Target")), Target.PackageName);
	const TSharedPtr<FJsonObject> Body = Extracted->GetObjectField(TEXT("Body"));
	TArray<FString> Interfaces;
	Body->TryGetStringArrayField(TEXT("Interfaces"), Interfaces);
	TestTrue(TEXT("interfaces include UE.Source.OneShot"), Interfaces.Contains(TEXT("UE.Source.OneShot")));
	TestTrue(TEXT("metadata"), Body->HasTypedField<EJson::Object>(TEXT("Metadata")));
	TestTrue(TEXT("inputs"), Body->HasTypedField<EJson::Object>(TEXT("Inputs")));
	TestTrue(TEXT("outputs"), Body->HasTypedField<EJson::Object>(TEXT("Outputs")));
	const TSharedPtr<FJsonObject>* Graphs = nullptr;
	const TSharedPtr<FJsonObject>* Graph = nullptr;
	if (TestTrue(TEXT("default page graph"), Body->TryGetObjectField(TEXT("Graphs"), Graphs) && (*Graphs)->TryGetObjectField(TEXT("Default"), Graph)))
	{
		const TSharedPtr<FJsonObject> Nodes = (*Graph)->GetObjectField(TEXT("Nodes"));
		TestTrue(TEXT("nodes"), Nodes->Values.Num() > 3);
		TestTrue(TEXT("comments region"), (*Graph)->HasTypedField<EJson::Object>(TEXT("Comments")));
		bool bConnected = false;
		bool bHidden = false;
		for (const auto& Node : Nodes->Values)
		{
			bConnected |= Node.Value->AsObject()->HasTypedField<EJson::Object>(TEXT("Inputs"));
			bHidden |= Node.Value->AsObject()->HasField(TEXT("Hidden"));
		}
		TestTrue(TEXT("connections are written on the consuming node"), bConnected);
		TestFalse(TEXT("derived (hidden) nodes are not emitted for the canonical source"), bHidden);
	}
	TestTrue(TEXT("Details settings are Properties"), Extracted->HasTypedField<EJson::Object>(TEXT("Properties")));
	TestFalse(TEXT("the document payload stays out of Properties"), Extracted->GetObjectField(TEXT("Properties"))->HasField(TEXT("RootMetasoundDocument")));
	TestEqual(TEXT("extract again is identical"), Serialize(Extract(*this, Target.ObjectPath, TEXT("Extract again"))), Serialize(Extracted));

	FString DiffText;
	TestEqual(TEXT("Diff of the extraction is empty"), DiffCount(Extracted, DiffText), 0);

	const FAssetDocumentResult Inspected = Inspect(Target.ObjectPath);
	if (TestTrue(TEXT("Inspect succeeds"), Inspected.IsSuccess() && Inspected.Payload.IsValid()))
	{
		TestTrue(TEXT("Inspect: pins"), Inspected.Payload->HasTypedField<EJson::Object>(TEXT("Pins")));
		TestTrue(TEXT("Inspect: interfaces with versions"), Inspected.Payload->HasTypedField<EJson::Array>(TEXT("Interfaces")));
		TestTrue(TEXT("Inspect: available interfaces"), Inspected.Payload->HasTypedField<EJson::Array>(TEXT("AvailableInterfaces")));
		TestEqual(TEXT("Inspect: build page"), Inspected.Payload->GetStringField(TEXT("BuildPage")), FString(TEXT("Default")));
	}
	else
	{
		AddInfo(DiagnosticsText(Inspected));
	}
	TArray<FString> Problems;
	SideEffects.Check(*Asset, Problems);
	for (const FString& Problem : Problems)
	{
		AddError(Problem);
	}

	// The unchanged extraction is a no-op (graph edits: AssetDocumentMetaSoundApplyTests.cpp).
	const FString Baseline = Serialize(Extracted);
	const FDateTime SavedStamp = IFileManager::Get().GetTimeStamp(*Target.PackageFile);
	for (const bool bWithProperties : {true, false})
	{
		// The unchanged extraction, with or without its Properties, is a no-op.
		const TSharedPtr<FJsonObject> Unchanged = Parse(Baseline);
		Unchanged->SetStringField(TEXT("Action"), TEXT("Update"));
		if (!bWithProperties)
		{
			Unchanged->RemoveField(TEXT("Properties"));
		}
		const FString Stage = bWithProperties ? TEXT("unchanged extraction") : TEXT("unchanged Body");
		const FAssetDocumentResult Result = Apply(Target, Unchanged);
		if (!TestTrue(*FString::Printf(TEXT("%s applies"), *Stage), Result.IsSuccess()))
		{
			AddInfo(DiagnosticsText(Result));
		}
		TestFalse(*FString::Printf(TEXT("%s: nothing saved"), *Stage), Result.bSavedAsset);
		TestFalse(*FString::Printf(TEXT("%s: package clean"), *Stage), Asset->GetOutermost()->IsDirty());
		TestEqual(*FString::Printf(TEXT("%s: package file untouched"), *Stage), IFileManager::Get().GetTimeStamp(*Target.PackageFile), SavedStamp);
		TestEqual(*FString::Printf(TEXT("%s: extraction unchanged"), *Stage), Serialize(Extract(*this, Target.ObjectPath, Stage)), Baseline);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMetaSoundCorpusTest, "UEAssetDoc.MetaSound.Corpus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMetaSoundCorpusTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMetaSoundTests;
	const FDemoTemplateContent Demo;
	int32 Tried = 0;
	int32 Passed = 0;
	bool bHarmonixSkipped = false;
	TArray<FString> Report;
	for (const FCorpusEntry& Entry : Corpus)
	{
		const FString Path = Entry.Path;
		const FString Name(FPackageName::ObjectPathToObjectName(Path));
		UObject* Source = LoadQuiet(Path);
		if (Source == nullptr)
		{
			bHarmonixSkipped |= Path.StartsWith(TEXT("/Harmonix/"));
			AddInfo(FString::Printf(TEXT("SKIP %s: not available (%s)"), *Name,
				Path.StartsWith(TEXT("/Harmonix/")) ? TEXT("the Harmonix plugin is not enabled in this project") : TEXT("content missing")));
			continue;
		}
		++Tried;
		TArray<FString> Problems;
		FFixtures Fixtures;
		const FTarget& First = Fixtures.Add(TEXT("MS_Corpus"));
		const FTarget& Second = Fixtures.Add(TEXT("MS_Corpus2"));
		UObject* Copy = Duplicate(*this, Source, First);
		if (Copy == nullptr)
		{
			AddError(FString::Printf(TEXT("%s: duplicate failed"), *Name));
			continue;
		}

		const FReadSideEffects SideEffects(*Copy);
		double Start = FPlatformTime::Seconds();
		const FAssetDocumentResult FirstResult = ExtractResult(First.ObjectPath);
		const double ExtractMs = Milliseconds(Start);
		const TSharedPtr<FJsonObject> Extracted = FirstResult.IsSuccess() ? FirstResult.Payload : nullptr;
		if (!Extracted.IsValid())
		{
			AddError(FString::Printf(TEXT("%s: extract failed: %s"), *Name, *DiagnosticsText(FirstResult)));
			continue;
		}
		const FString Baseline = Serialize(Extracted);
		Start = FPlatformTime::Seconds();
		const FString Again = Serialize(Extract(*this, First.ObjectPath, Name + TEXT(" again")));
		const double AgainMs = Milliseconds(Start);
		if (Again != Baseline)
		{
			Problems.Add(FString::Printf(TEXT("second extraction differs %s"), *FirstDifference(Baseline, Again)));
		}
		Start = FPlatformTime::Seconds();
		const FAssetDocumentResult Inspected = Inspect(First.ObjectPath);
		const double InspectMs = Milliseconds(Start);
		if (!Inspected.IsSuccess())
		{
			Problems.Add(FString::Printf(TEXT("inspect failed: %s"), *DiagnosticsText(Inspected)));
		}
		FString DiffText;
		const int32 Changes = DiffCount(Extracted, DiffText);
		if (Changes != 0)
		{
			Problems.Add(FString::Printf(TEXT("Diff of the extraction reports %d changes: %s"), Changes, *DiffText.Left(600)));
		}
		SideEffects.Check(*Copy, Problems);
		FString Compile = TEXT("-");
		if (FirstResult.Compile.IsValid())
		{
			Compile = FirstResult.Compile->GetStringField(TEXT("Status"));
			const TArray<TSharedPtr<FJsonValue>>* Messages = nullptr;
			if (FirstResult.Compile->TryGetArrayField(TEXT("Messages"), Messages) && !Messages->IsEmpty())
			{
				Compile += FString::Printf(TEXT(" (%d messages: %s)"), Messages->Num(), *(*Messages)[0]->AsObject()->GetStringField(TEXT("Message")));
			}
		}

		// Unloaded and loaded back from disk.
		UPackageTools::UnloadPackages({Copy->GetOutermost()});
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		Start = FPlatformTime::Seconds();
		UObject* Reloaded = LoadQuiet(First.ObjectPath);
		const double LoadMs = Milliseconds(Start);
		double ReloadMs = 0.0;
		if (Reloaded == nullptr)
		{
			Problems.Add(TEXT("reload failed"));
		}
		else
		{
			Start = FPlatformTime::Seconds();
			const FString AfterReload = Serialize(Extract(*this, First.ObjectPath, Name + TEXT(" reloaded")));
			ReloadMs = Milliseconds(Start);
			if (AfterReload != Baseline)
			{
				Problems.Add(FString::Printf(TEXT("extraction after reload differs %s"), *FirstDifference(Baseline, AfterReload)));
			}
		}

		// A second duplicate of the same source.
		if (Duplicate(*this, Source, Second) == nullptr)
		{
			Problems.Add(TEXT("second duplicate failed"));
		}
		else
		{
			const FString Expected = WithoutTarget(Extracted);
			const FString Other = WithoutTarget(Extract(*this, Second.ObjectPath, Name + TEXT(" duplicate")));
			if (Other != Expected)
			{
				Problems.Add(FString::Printf(TEXT("extraction of a second duplicate differs %s"), *FirstDifference(Expected, Other)));
			}
		}

		const FString Line = FString::Printf(TEXT("%-52s %s | %s | extract %.0f ms, again %.0f ms, reload %.0f ms (load %.0f ms), inspect %.0f ms, %d chars | compile %s"),
			*Name, *Extracted->GetStringField(TEXT("Class")).Replace(TEXT("/Script/MetasoundEngine."), TEXT("")), *GraphSummary(Extracted),
			ExtractMs, AgainMs, ReloadMs, LoadMs, InspectMs, Baseline.Len(), *Compile);
		if (Problems.IsEmpty())
		{
			++Passed;
			Report.Add(TEXT("PASS ") + Line);
		}
		else
		{
			Report.Add(TEXT("FAIL ") + Line);
			for (const FString& Problem : Problems)
			{
				AddError(FString::Printf(TEXT("%s: %s"), *Name, *Problem));
			}
		}
	}
	AddInfo(FString::Printf(TEXT("MetaSound corpus: %d / %d stable (%d of %d available)%s"), Passed, Tried, Tried, UE_ARRAY_COUNT(Corpus),
		bHarmonixSkipped ? TEXT("; Harmonix entries skipped: enable the Harmonix plugin in the project to run them") : TEXT("")));
	for (const FString& Line : Report)
	{
		AddInfo(Line);
	}
	TestTrue(TEXT("at least the Metasound plugin's sources are available"), Tried >= 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMetaSoundEditorOpenTest, "UEAssetDoc.MetaSound.EditorOpen",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMetaSoundEditorOpenTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMetaSoundTests;
	const FDemoTemplateContent Demo;
	int32 Compared = 0;
	for (const TCHAR* Path : {OneShot, TEXT("/Game/DemoTemplate/Metasounds/MS_TuningFork.MS_TuningFork"), TEXT("/Game/DemoTemplate/Metasounds/MS_Opera.MS_Opera"),
		TEXT("/Harmonix/Tests/HarmonixMetasound/MidiPlayerNode/MS_Test_MidiPlayerNode_Simple.MS_Test_MidiPlayerNode_Simple")})
	{
		const FString Name(FPackageName::ObjectPathToObjectName(FStringView(Path)));
		UObject* Source = LoadQuiet(Path);
		if (Source == nullptr)
		{
			AddInfo(FString::Printf(TEXT("SKIP %s: not available"), *Name));
			continue;
		}
		FFixtures Fixtures;
		const FTarget& Target = Fixtures.Add(TEXT("MS_Open"));
		UObject* Copy = Duplicate(*this, Source, Target);
		if (!TestNotNull(*FString::Printf(TEXT("%s: duplicate"), *Name), Copy))
		{
			continue;
		}
		// As saved by an older engine: the editor adds the input template nodes when it opens the asset.
		const int32 Removed = RemoveInputTemplates(*Copy);
		const TSharedPtr<FJsonObject> Normalized = Extract(*this, Target.ObjectPath, Name + TEXT(" normalised"));
		const FAssetDocumentResult InspectedBefore = Inspect(Target.ObjectPath);
		TArray<FString> Injected;
		if (InspectedBefore.Payload.IsValid())
		{
			InspectedBefore.Payload->TryGetStringArrayField(TEXT("InjectedNodes"), Injected);
		}

		// What the MetaSound editor does when it opens the asset (FEditor::InitMetasoundEditor).
		FMetaSoundFrontendDocumentBuilder& Builder = Metasound::Engine::FDocumentBuilderRegistry::GetChecked().FindOrBeginBuilding(*Copy).GetBuilder();
#if !UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
		// Newer engines version MetaSounds only when they load, not when the editor opens them.
		FMetasoundAssetBase* MetaSound = Metasound::Frontend::IMetaSoundAssetManager::GetChecked().GetAsAsset(*Copy);
		if (MetaSound != nullptr)
		{
			MetaSound->VersionAsset(Builder);
		}
#endif
		constexpr bool bForceNodeCreation = false;
		const bool bInjected = Metasound::Frontend::FInputNodeTemplate::GetChecked().Inject(Builder, bForceNodeCreation);
		const FMetasoundFrontendClassName ClassName = Builder.GetConstDocumentChecked().RootGraph.Metadata.GetClassName();

		const TSharedPtr<FJsonObject> Opened = Extract(*this, Target.ObjectPath, Name + TEXT(" opened"));
		const FAssetDocumentResult InspectedAfter = Inspect(Target.ObjectPath);
		TestFalse(*FString::Printf(TEXT("%s: after the editor's injection nothing is left to inject"), *Name),
			InspectedAfter.Payload.IsValid() && InspectedAfter.Payload->HasField(TEXT("InjectedNodes")));
		Metasound::Engine::FDocumentBuilderRegistry::GetChecked().FinishBuilding(ClassName, FTopLevelAssetPath(Copy));
		Copy->GetOutermost()->ClearDirtyFlag();

		const FString Expected = WithoutIds(Normalized);
		const FString Actual = WithoutIds(Opened);
		if (!TestEqual(*FString::Printf(TEXT("%s: the normalised read equals the editor-opened asset (ids aside)"), *Name), Actual, Expected))
		{
			AddInfo(FirstDifference(Expected, Actual));
		}
		// (One template node per input member: an input that fed several template nodes gets one back.)
		TestEqual(*FString::Printf(TEXT("%s: the read reports the template nodes the editor adds"), *Name), Injected.Num() > 0, Removed > 0);
		TestTrue(*FString::Printf(TEXT("%s: at most one injected node per removed template"), *Name), Injected.Num() <= Removed);
		TestEqual(*FString::Printf(TEXT("%s: the editor injects when templates are missing"), *Name), bInjected, Removed > 0);
		AddInfo(FString::Printf(TEXT("%s: %d input template nodes removed; the editor injected %s; the read injects %d (%s)"), *Name, Removed,
			bInjected ? TEXT("template nodes") : TEXT("nothing"), Injected.Num(), *FString::Join(Injected, TEXT(", "))));
		++Compared;
	}
	TestTrue(TEXT("at least one MetaSound compared"), Compared > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMetaSoundListTypesTest, "UEAssetDoc.MetaSound.ListTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMetaSoundListTypesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMetaSoundTests;
	const FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	auto List = [&](const TMap<FString, FString>& Query, const TCHAR* Stage) -> TArray<TSharedPtr<FJsonValue>>
	{
		const FAssetDocumentResult Result = Service.ListTypes(Query);
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (!TestTrue(*FString::Printf(TEXT("%s: lists"), Stage), Result.IsSuccess() && Result.Payload.IsValid() && Result.Payload->TryGetArrayField(TEXT("Types"), Rows)))
		{
			AddInfo(DiagnosticsText(Result));
			AddInfo(Serialize(Result.Payload));
			return {};
		}
		return *Rows;
	};
	auto Find = [](const TArray<TSharedPtr<FJsonValue>>& Rows, const FString& Path) -> TSharedPtr<FJsonObject>
	{
		for (const TSharedPtr<FJsonValue>& Row : Rows)
		{
			if (Row->AsObject()->GetStringField(TEXT("Path")) == Path)
			{
				return Row->AsObject();
			}
		}
		return nullptr;
	};

	// Nodes: the class full names the document's Class field uses, with pins.
	const TArray<TSharedPtr<FJsonValue>> Sines = List({{TEXT("kind"), TEXT("metasound_node")}, {TEXT("name"), TEXT("Sine")}, {TEXT("limit"), TEXT("400")}}, TEXT("nodes named Sine"));
	TestTrue(TEXT("Sine nodes are listed"), Sines.Num() > 0);
	if (Sines.Num() > 0)
	{
		const TSharedPtr<FJsonObject> Row = Sines[0]->AsObject();
		AddInfo(FString::Printf(TEXT("node row %s"), *Serialize(Row)));
		TestTrue(TEXT("node row: class name"), Row->HasTypedField<EJson::String>(TEXT("ClassName")));
		TestTrue(TEXT("node row: version"), Row->HasTypedField<EJson::Number>(TEXT("Version")));
		TestTrue(TEXT("node row: inputs"), Row->HasTypedField<EJson::Array>(TEXT("Inputs")));
		TestTrue(TEXT("node row: outputs"), Row->HasTypedField<EJson::Array>(TEXT("Outputs")));
	}
	const TArray<TSharedPtr<FJsonValue>> AllNodes = List({{TEXT("kind"), TEXT("metasound_node")}, {TEXT("limit"), TEXT("400")}}, TEXT("all nodes"));
	TestTrue(TEXT("many node classes"), AllNodes.Num() > 100);
	const TArray<TSharedPtr<FJsonValue>> Paged = List({{TEXT("kind"), TEXT("metasound_node")}, {TEXT("limit"), TEXT("5")}, {TEXT("offset"), TEXT("5")}}, TEXT("paged nodes"));
	TestEqual(TEXT("paging"), Paged.Num(), 5);
	if (Paged.Num() == 5 && AllNodes.Num() > 10)
	{
		TestEqual(TEXT("paging follows the sorted order"), Paged[0]->AsObject()->GetStringField(TEXT("Path")), AllNodes[5]->AsObject()->GetStringField(TEXT("Path")));
	}

	// A node family that differs only in its data type is one row: <T> stands for each of its Variants.
	const TArray<TSharedPtr<FJsonValue>> RandomGets = List({{TEXT("kind"), TEXT("metasound_node")}, {TEXT("name"), TEXT("*Random Get*")}, {TEXT("limit"), TEXT("400")}}, TEXT("Random Get family"));
	const TSharedPtr<FJsonObject> RandomGet = Find(RandomGets, TEXT("Array.Random Get.<T>:Array"));
	if (TestTrue(TEXT("Random Get is one family row"), RandomGet.IsValid()))
	{
		AddInfo(FString::Printf(TEXT("family row %s (%d rows)"), *Serialize(RandomGet), RandomGets.Num()));
		TestTrue(TEXT("the family replaces its variants' rows"), RandomGets.Num() < 5);
		TestEqual(TEXT("family display name"), RandomGet->GetStringField(TEXT("DisplayName")), FString(TEXT("Random Get (<T>:Array)")));
		TArray<FString> Variants;
		RandomGet->TryGetStringArrayField(TEXT("Variants"), Variants);
		TestTrue(TEXT("family variants"), Variants.Contains(TEXT("Float")) && Variants.Contains(TEXT("Int32")) && Variants.Contains(TEXT("Bool")));
		TArray<FString> Inputs;
		TArray<FString> Outputs;
		RandomGet->TryGetStringArrayField(TEXT("Inputs"), Inputs);
		RandomGet->TryGetStringArrayField(TEXT("Outputs"), Outputs);
		TestTrue(TEXT("family pin of the variant type"), Inputs.Contains(TEXT("In Array: <T>:Array")) && Outputs.Contains(TEXT("Value: <T>")));
		TestTrue(TEXT("family pin of a fixed type"), Inputs.Contains(TEXT("Weights: Float:Array")));
	}
	const TArray<TSharedPtr<FJsonValue>> FloatRandomGet = List({{TEXT("kind"), TEXT("metasound_node")}, {TEXT("name"), TEXT("Array.Random Get.Float:Array")}}, TEXT("one Random Get"));
	if (TestEqual(TEXT("a filter matching one class lists it"), FloatRandomGet.Num(), 1) && FloatRandomGet.Num() == 1)
	{
		TArray<FString> Inputs;
		FloatRandomGet[0]->AsObject()->TryGetStringArrayField(TEXT("Inputs"), Inputs);
		TestEqual(TEXT("its class"), FloatRandomGet[0]->AsObject()->GetStringField(TEXT("ClassName")), FString(TEXT("Array.Random Get.Float:Array")));
		TestTrue(TEXT("its pins are its own"), Inputs.Contains(TEXT("In Array: Float:Array")));
		TestFalse(TEXT("no Variants"), FloatRandomGet[0]->AsObject()->HasField(TEXT("Variants")));
	}

	// Data types: literal kind, array and constructor support.
	const TArray<TSharedPtr<FJsonValue>> Types = List({{TEXT("kind"), TEXT("metasound_datatype")}, {TEXT("limit"), TEXT("400")}}, TEXT("data types"));
	for (const TCHAR* Expected : {TEXT("Float"), TEXT("Int32"), TEXT("Bool"), TEXT("Trigger"), TEXT("Audio"), TEXT("Time"), TEXT("WaveAsset"), TEXT("Float:Array")})
	{
		TestTrue(*FString::Printf(TEXT("data type %s"), Expected), Find(Types, Expected).IsValid());
	}
	if (const TSharedPtr<FJsonObject> Float = Find(Types, TEXT("Float")))
	{
		AddInfo(FString::Printf(TEXT("data type row %s"), *Serialize(Float)));
		TestEqual(TEXT("Float literal is a JSON number"), Float->GetStringField(TEXT("Literal")), FString(TEXT("number")));
	}

	// Interfaces: versions, members and the asset classes that may use them.
	const TArray<TSharedPtr<FJsonValue>> Interfaces = List({{TEXT("kind"), TEXT("metasound_interface")}, {TEXT("name"), TEXT("UE.Source*")}}, TEXT("source interfaces"));
	const TSharedPtr<FJsonObject> OneShotInterface = Find(Interfaces, TEXT("UE.Source.OneShot"));
	if (TestTrue(TEXT("UE.Source.OneShot is listed"), OneShotInterface.IsValid()))
	{
		AddInfo(FString::Printf(TEXT("interface row %s"), *Serialize(OneShotInterface)));
		TestTrue(TEXT("interface outputs"), OneShotInterface->HasTypedField<EJson::Array>(TEXT("Outputs")));
		TestTrue(TEXT("interface classes"), OneShotInterface->HasTypedField<EJson::Array>(TEXT("Classes")));
	}
	TestTrue(TEXT("UE.Source is listed"), Find(Interfaces, TEXT("UE.Source")).IsValid());

	// An unknown kind names every kind, the MetaSound ones included.
	const FAssetDocumentResult Unknown = Service.ListTypes({{TEXT("kind"), TEXT("metasound")}});
	TestFalse(TEXT("unknown kind fails"), Unknown.IsSuccess());
	TestTrue(TEXT("unknown kind lists the MetaSound kinds"), Unknown.Message.Contains(TEXT("metasound_datatype"))
		|| DiagnosticsText(Unknown).Contains(TEXT("metasound_datatype")));
	return true;
}
#endif // WITH_DEV_AUTOMATION_TESTS
