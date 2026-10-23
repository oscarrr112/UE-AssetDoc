// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// validate_asset_document on MetaSound Sources, Patches and presets, Create and Update:
// Validate runs the apply on a staged trial copy (AssetDocumentMetaSoundTrial.cpp) and reports what Apply would --
// the same success, or the same refusal (code and path) -- while the live asset, the frontend registration and the
// MetaSound asset manager are left as they were, and nothing of the trial stays loaded or registered.

#include "AssetDocumentMetaSoundApplyTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentMetaSoundValidateTests
{
using namespace AssetDocumentMetaSoundApplyTests;

const TCHAR* const StagedRoot = TEXT("/Temp/AssetDocument/ValidateTrial");

inline FAssetDocumentResult Validate(const TSharedPtr<FJsonObject>& Document)
{
	FAssetDocumentValidateRequest Request;
	Request.Document = Document;
	return FAssetDocumentModule::Get().GetService().Validate(Request);
}

inline TArray<FString> LoadedStagedPackages()
{
	TArray<FString> Names;
	for (TObjectIterator<UPackage> It; It; ++It)
	{
		if (IsValid(*It) && It->GetName().StartsWith(StagedRoot))
		{
			Names.Add(It->GetName());
		}
	}
	return Names;
}

inline bool AssetManagerKnowsStaged()
{
	bool bFound = false;
	if (const Metasound::Engine::FMetaSoundAssetManager* Manager = Metasound::Engine::FMetaSoundAssetManager::Get())
	{
		Manager->IterateAssetTagData([&bFound](const Metasound::Frontend::FMetaSoundAssetClassInfo& Info)
		{
			bFound |= Info.AssetPath.GetPackageName().ToString().StartsWith(StagedRoot);
		}, true);
	}
	return bFound;
}

inline const FAssetDocumentDiagnostic* FirstError(const FAssetDocumentResult& Result)
{
	return Result.Diagnostics.FindByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Severity.IsEmpty() || Diagnostic.Severity == TEXT("error");
	});
}

struct FValidateCheck
{
	FAutomationTestBase& Test;

	// Validate succeeds, and leaves the target (existing or absent), the dirty packages and the trial machinery as they were.
	void ExpectValid(const FTarget& Target, const TSharedPtr<FJsonObject>& Document, const FString& Stage)
	{
		const bool bExists = Target.Get() != nullptr;
		const FString Before = bExists ? Serialize(Extract(Test, Target.ObjectPath, Stage + TEXT(" before"))) : FString();
		const TSet<FString> DirtyBefore = DirtyPackages();
		const FAssetDocumentResult Result = Validate(Document);
		if (!Test.TestTrue(*FString::Printf(TEXT("%s: validates"), *Stage), Result.IsSuccess()))
		{
			Test.AddError(FString::Printf(TEXT("%s: %s"), *Stage, *DiagnosticsText(Result)));
		}
		Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *Result.Message));
		ExpectUntouched(Target, bExists, Before, DirtyBefore, Stage);
	}

	// Validate refuses with what Apply then refuses with (code and path); Apply runs on the same target afterwards.
	void ExpectSameRefusal(const FTarget& Target, const TSharedPtr<FJsonObject>& Document, const FString& Stage)
	{
		const bool bExists = Target.Get() != nullptr;
		const FString Before = bExists ? Serialize(Extract(Test, Target.ObjectPath, Stage + TEXT(" before"))) : FString();
		const TSet<FString> DirtyBefore = DirtyPackages();
		const FAssetDocumentResult Validated = Validate(Document);
		Test.TestFalse(*FString::Printf(TEXT("%s: Validate refuses"), *Stage), Validated.IsSuccess());
		ExpectUntouched(Target, bExists, Before, DirtyBefore, Stage);
		const FAssetDocumentResult Applied = Apply(Target, Document);
		Test.TestFalse(*FString::Printf(TEXT("%s: Apply refuses"), *Stage), Applied.IsSuccess());
		const FAssetDocumentDiagnostic* ValidateError = FirstError(Validated);
		const FAssetDocumentDiagnostic* ApplyError = FirstError(Applied);
		if (Test.TestNotNull(*FString::Printf(TEXT("%s: Validate names the error"), *Stage), ValidateError)
			&& Test.TestNotNull(*FString::Printf(TEXT("%s: Apply names the error"), *Stage), ApplyError))
		{
			Test.TestEqual(*FString::Printf(TEXT("%s: same code as Apply"), *Stage), ValidateError->Code, ApplyError->Code);
			Test.TestEqual(*FString::Printf(TEXT("%s: same path as Apply"), *Stage), ValidateError->Path, ApplyError->Path);
		}
		Test.AddInfo(FString::Printf(TEXT("%s: Validate %s | Apply %s"), *Stage, *DiagnosticsText(Validated), *DiagnosticsText(Applied)));
	}

	void ExpectUntouched(const FTarget& Target, bool bExisted, const FString& Before, const TSet<FString>& DirtyBefore, const FString& Stage)
	{
		if (bExisted)
		{
			Test.TestEqual(*FString::Printf(TEXT("%s: the asset extracts as before"), *Stage),
				Serialize(Extract(Test, Target.ObjectPath, Stage + TEXT(" after"))), Before);
			if (const UObject* Asset = Target.Get())
			{
				Test.TestTrue(*FString::Printf(TEXT("%s: the asset stays registered"), *Stage), IsRegistered(*Asset));
			}
		}
		else
		{
			Test.TestNull(*FString::Printf(TEXT("%s: no asset was created"), *Stage), Target.Get());
			Test.TestFalse(*FString::Printf(TEXT("%s: the asset manager does not know the target"), *Stage), AssetManagerKnows(Target.PackageName));
		}
		Test.TestEqual(*FString::Printf(TEXT("%s: no package was dirtied"), *Stage), DirtyPackages().Num(), DirtyBefore.Num());
		Test.TestEqual(*FString::Printf(TEXT("%s: no staged package stays loaded"), *Stage), FString::Join(LoadedStagedPackages(), TEXT(", ")), FString());
		Test.TestFalse(*FString::Printf(TEXT("%s: the asset manager forgot the staged copy"), *Stage), AssetManagerKnowsStaged());
	}
};

const TCHAR* const PatchText = TEXT(R"({"SchemaVersion": 1, "Class": "/Script/MetasoundEngine.MetaSoundPatch", "Action": "Create",
	"Body": {"Metadata": {"Referenceable": true},
	  "Inputs": {"In": {"Type": "Audio"}, "Gain": {"Type": "Float", "Default": 0.5}},
	  "Outputs": {"Out": {"Type": "Audio"}},
	  "Graphs": {"Default": {"Nodes": {
	    "In": {"Class": "Input", "Member": "In", "Position": [-400, 0]},
	    "Gain": {"Class": "Input", "Member": "Gain", "Position": [-400, 120]},
	    "Mul": {"Class": "UE.Multiply.Audio by Float", "Position": [-150, 0], "Inputs": {"PrimaryOperand": "In", "AdditionalOperands": "Gain"}},
	    "Out": {"Class": "Output", "Member": "Out", "Position": [100, 0], "Inputs": {"Out": "Mul"}}}}}}})");

const TCHAR* const SourceText = TEXT(R"({"SchemaVersion": 1, "Class": "/Script/MetasoundEngine.MetaSoundSource", "Action": "Create",
	"Properties": {"OutputFormat": "Mono"},
	"Body": {"Interfaces": ["UE.Source.OneShot"],
	  "Inputs": {"Pitch": {"Type": "Float", "Default": 440.0}},
	  "Graphs": {"Default": {"Nodes": {
	    "Play":  {"Class": "Input", "Member": "UE.Source.OnPlay", "Position": [-600, 0]},
	    "Pitch": {"Class": "Input", "Member": "Pitch", "Position": [-600, 120]},
	    "Osc":   {"Class": "UE.Sine.Audio", "Position": [-350, 0], "Inputs": {"Frequency": "Pitch"}},
	    "Env":   {"Class": "AD Envelope.AD Envelope.Audio", "Position": [-350, 200], "Inputs": {"Trigger": "Play"}, "Defaults": {"Decay Time": 0.3}},
	    "Amp":   {"Class": "UE.Multiply.Audio", "Position": [-100, 0], "Inputs": {"PrimaryOperand": "Osc", "AdditionalOperands": "Env.Out Envelope"}},
	    "Out":   {"Class": "Output", "Member": "UE.OutputFormat.Mono.Audio:0", "Position": [150, 0], "Inputs": {"UE.OutputFormat.Mono.Audio:0": "Amp"}},
	    "Done":  {"Class": "Output", "Member": "UE.Source.OneShot.OnFinished", "Position": [150, 200], "Inputs": {"UE.Source.OneShot.OnFinished": "Env.On Done"}}}}}}})");

inline TSharedPtr<FJsonObject> Document(const TCHAR* Text, const FTarget& Target, const TCHAR* Action)
{
	const TSharedPtr<FJsonObject> Result = Parse(Text);
	Result->SetStringField(TEXT("Target"), Target.PackageName);
	Result->SetStringField(TEXT("Action"), Action);
	return Result;
}

inline TSharedPtr<FJsonObject> PresetDocument(const FTarget& Parent, const FTarget& Target, const TCHAR* Action, const TSharedPtr<FJsonValue>& PitchDefault)
{
	const TSharedPtr<FJsonObject> Result = Parse(TEXT(R"({"SchemaVersion": 1, "Class": "/Script/MetasoundEngine.MetaSoundSource",
		"Body": {"Preset": {}, "Inputs": {"Pitch": {}}}})"));
	Result->SetStringField(TEXT("Target"), Target.PackageName);
	Result->SetStringField(TEXT("Action"), Action);
	Child(BodyOf(Result), TEXT("Preset"))->SetStringField(TEXT("Parent"), Parent.PackageName);
	Child(Child(BodyOf(Result), TEXT("Inputs")), TEXT("Pitch"))->SetField(TEXT("Default"), PitchDefault);
	return Result;
}

// The node's input wired to another source (a Trigger into a Float or Audio input: the editor refuses the connection).
inline TSharedPtr<FJsonObject> Rewired(const TSharedPtr<FJsonObject>& Base, const TCHAR* Node, const TCHAR* Input, const TCHAR* From)
{
	const TSharedPtr<FJsonObject> Result = Parse(Serialize(Base));
	Child(NodeOf(Result, Node), TEXT("Inputs"))->SetStringField(Input, From);
	return Result;
}

inline TSharedPtr<FJsonObject> WithDefault(const TSharedPtr<FJsonObject>& Base, const TCHAR* Input, double Value)
{
	const TSharedPtr<FJsonObject> Result = Parse(Serialize(Base));
	Child(Child(BodyOf(Result), TEXT("Inputs")), Input)->SetNumberField(TEXT("Default"), Value);
	return Result;
}

inline TSharedPtr<FJsonObject> WithAssetNode(const TSharedPtr<FJsonObject>& Base, const FTarget& Used)
{
	const TSharedPtr<FJsonObject> Result = Parse(Serialize(Base));
	const TSharedPtr<FJsonObject> Node = NodeOf(Result, TEXT("Used"));
	Node->SetStringField(TEXT("Class"), Used.ObjectPath);
	Node->SetArrayField(TEXT("Position"), AssetDocumentMetaSoundApplyTests::Point(-150, 300));
	return Result;
}
} // namespace AssetDocumentMetaSoundValidateTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMetaSoundValidateTest, "UEAssetDoc.MetaSound.Validate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMetaSoundValidateTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMetaSoundValidateTests;
	FFixtures Fixtures;
	FValidateCheck Check{*this};
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	const int32 BuildersBefore = BuilderObjectCount();

	// Patch: Create (absent target), then Update of the applied patch.
	const FTarget& Patch = Fixtures.Add(TEXT("MSP_Validate"));
	Check.ExpectValid(Patch, Document(PatchText, Patch, TEXT("Create")), TEXT("Patch Create"));
	Check.ExpectSameRefusal(Patch, Rewired(Document(PatchText, Patch, TEXT("Create")), TEXT("Out"), TEXT("Out"), TEXT("Gain")),
		TEXT("Patch Create, a Float into an Audio output"));
	if (!ApplyOk(*this, Patch, Document(PatchText, Patch, TEXT("Create")), TEXT("Patch")))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> PatchUpdate = Document(PatchText, Patch, TEXT("Update"));
	Check.ExpectValid(Patch, WithDefault(PatchUpdate, TEXT("Gain"), 0.75), TEXT("Patch Update"));
	Check.ExpectSameRefusal(Patch, Rewired(PatchUpdate, TEXT("Out"), TEXT("Out"), TEXT("Gain")), TEXT("Patch Update, a Float into an Audio output"));

	// Source: Create, then Update.
	const FTarget& Source = Fixtures.Add(TEXT("MSS_Validate"));
	Check.ExpectValid(Source, Document(SourceText, Source, TEXT("Create")), TEXT("Source Create"));
	Check.ExpectSameRefusal(Source, Rewired(Document(SourceText, Source, TEXT("Create")), TEXT("Osc"), TEXT("Frequency"), TEXT("Play")),
		TEXT("Source Create, a Trigger into a Float input"));
	if (!ApplyOk(*this, Source, Document(SourceText, Source, TEXT("Create")), TEXT("Source")))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> SourceUpdate = Document(SourceText, Source, TEXT("Update"));
	Check.ExpectValid(Source, WithDefault(SourceUpdate, TEXT("Pitch"), 550.0), TEXT("Source Update"));
	Check.ExpectSameRefusal(Source, Rewired(SourceUpdate, TEXT("Osc"), TEXT("Frequency"), TEXT("Play")), TEXT("Source Update, a Trigger into a Float input"));

	// Preset of the Source: Create, then Update.
	const FTarget& Preset = Fixtures.Add(TEXT("MSS_ValidatePreset"));
	Check.ExpectValid(Preset, PresetDocument(Source, Preset, TEXT("Create"), MakeShared<FJsonValueNumber>(880.0)), TEXT("Preset Create"));
	Check.ExpectSameRefusal(Preset, PresetDocument(Source, Preset, TEXT("Create"), MakeShared<FJsonValueString>(TEXT("high"))),
		TEXT("Preset Create, a text default of a Float input"));
	if (!ApplyOk(*this, Preset, PresetDocument(Source, Preset, TEXT("Create"), MakeShared<FJsonValueNumber>(880.0)), TEXT("Preset")))
	{
		return false;
	}
	Check.ExpectValid(Preset, PresetDocument(Source, Preset, TEXT("Update"), MakeShared<FJsonValueNumber>(660.0)), TEXT("Preset Update"));
	Check.ExpectSameRefusal(Preset, PresetDocument(Source, Preset, TEXT("Update"), MakeShared<FJsonValueString>(TEXT("high"))),
		TEXT("Preset Update, a text default of a Float input"));

	// The trial copy keeps the reference checks of the asset it copies: the patch as its own node, and a patch that
	// uses it as a node of it.
	Check.ExpectSameRefusal(Patch, WithAssetNode(PatchUpdate, Patch), TEXT("Patch Update, itself as a node"));
	const FTarget& User = Fixtures.Add(TEXT("MSP_ValidateUser"));
	if (ApplyOk(*this, User, WithAssetNode(Document(PatchText, User, TEXT("Create")), Patch), TEXT("a patch that uses the patch")))
	{
		Check.ExpectSameRefusal(Patch, WithAssetNode(PatchUpdate, User), TEXT("Patch Update, a node of a patch that uses it"));
		// A valid Update of a patch others use: Validate reports the dependents Apply registers again.
		const FAssetDocumentResult Validated = Validate(WithDefault(PatchUpdate, TEXT("Gain"), 0.6));
		TestTrue(TEXT("Patch Update with a dependent: validates"), Validated.IsSuccess());
		TestTrue(TEXT("Patch Update with a dependent: reports MetaSound.DependentsUpdated"), HasCode(Validated, TEXT("MetaSound.DependentsUpdated")));
	}

	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestTrue(TEXT("no builder of a trial copy outlives the trials"), BuilderObjectCount() <= BuildersBefore + 4);
	AddInfo(FString::Printf(TEXT("builders: %d before, %d after (4 assets applied)"), BuildersBefore, BuilderObjectCount()));
	return true;
}
#endif
