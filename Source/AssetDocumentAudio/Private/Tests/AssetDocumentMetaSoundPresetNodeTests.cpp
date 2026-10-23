// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// MetaSound documents (presets, node configuration, Update Node, member widgets, the open editor), through
// the production Service as the MCP tools send them (design §6.2-6.4, §8.4):
//
// UEAssetDoc.MetaSound.Preset: presets made as the editor makes them (Content Browser Create Preset:
// a duplicate of the parent, InitAsset, registration; the restricted-mode factory with ReferencedMetaSoundObject; a
// preset of a patch) with defaults overridden in the Details panel pass §6.2; the free edits (override, inherit again,
// Convert From Preset) give the same asset by document and by the editor's calls; what the editor cannot do (a graph,
// interfaces or member settings in a preset, another parent, a regular MetaSound made a preset, a missing or
// mismatched parent) fails at its document path and changes nothing; a failed apply rolls back; undo / redo keep the
// preset whole.
// UEAssetDoc.MetaSound.Configuration (Harmonix): a source with a Tape Stop/Start node configured to
// two channels in its Details passes §6.2; channel-count edits give the same graph by document and by the Details
// edit (configuration written, UpdateNodeInterfaceFromConfiguration); invalid configurations fail at their paths.
// UEAssetDoc.MetaSound.UpdateNode: MSP_MidiNoteAdsr's node of an unregistered class version (Harmonix;
// engines that ship it updated get the earlier version back) is updated by Version 0 -> 1 as the editor's Update Node
// does (FEditor::UpdateSelectedNodeClasses); its Create from the extraction is refused with the hint; a Version with no
// registered major is refused.
// UEAssetDoc.MetaSound.Widget: every input widget setting the Details panel writes reads back and
// applies to the same member settings.
// UEAssetDoc.MetaSound.OpenEditor: with the asset open in the MetaSound editor an apply lands in the
// open editor's graph, a failed apply rolls back, and the editor's own undo / redo keep the asset whole.
// UEAssetDoc.MetaSound.Formats: content the engine does not ship, made with the editor's calls (design
// §6.1): a 7.1 source with the UE.Attenuation / UE.Spatialization / orientation interfaces (an attenuation input in use,
// every channel wired) and a patch with stereo input and output format interfaces pass §6.2.

#include "AssetDocumentMetaSoundApplyTestSupport.h"

#include "EdGraph/EdGraph.h"
#include "Factories/Factory.h"
#include "Misc/EngineVersionComparison.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundEditorSubsystem.h"
#include "MetasoundFrontendController.h"
#include "MetasoundFrontendSearchEngine.h"
#include "Runtime/AssetDocumentUndoHistory.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "TickableEditorObject.h"
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
#include "DocumentTemplates/MetasoundFrontendPresetTemplate.h"
#include "Metasound.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentMetaSoundPresetNodeTests
{
using namespace AssetDocumentMetaSoundApplyTests;

const TCHAR* const MidiNoteAdsr = TEXT("/Harmonix/MetaSounds/MSP_MidiNoteAdsr.MSP_MidiNoteAdsr");
const TCHAR* const TapeSS = TEXT("HarmonixNodes.TapeSS");
const TCHAR* const MonoOut = TEXT("UE.OutputFormat.Mono.Audio:0");

// The Content Browser's Create Preset (MetasoundAssetDefinitions ExecuteCreateMetaSoundPreset, editor not in restricted
// mode): a duplicate of the parent, InitAsset with the document cleared, registration.
UObject* CreatePresetByDuplicate(FAutomationTestBase& Test, UObject& Parent, const FTarget& Target)
{
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UObject* Preset = AssetTools.DuplicateAsset(Target.AssetName, FPackageName::GetLongPackagePath(Target.PackageName), &Parent);
	if (!Test.TestNotNull(*FString::Printf(TEXT("%s duplicated"), *Target.AssetName), Preset))
	{
		return nullptr;
	}
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	UMetaSoundEditorSubsystem::FInitAssetArgs Args;
	Args.Template = TInstancedStruct<FMetaSoundFrontendDocumentTemplate>::Make<FMetaSoundFrontendPresetTemplate>();
	Args.Template.GetMutable<FMetaSoundFrontendPresetTemplate>().Parent = &Parent;
	Args.SelectedObjects = {&Parent};
	UMetaSoundEditorSubsystem::GetChecked().InitAsset(*Preset, MoveTemp(Args));
#else
	UMetaSoundEditorSubsystem::GetChecked().InitAsset(*Preset, &Parent, true);
#endif
	UMetaSoundEditorSubsystem::GetChecked().RegisterGraphWithFrontend(*Preset);
	return Save(Test, Preset, Target.AssetName) ? Preset : nullptr;
}

// The New Asset menu's MetaSound factory; with Parent, the restricted-mode Create Preset (ReferencedMetaSoundObject).
UObject* CreateByFactory(FAutomationTestBase& Test, bool bSource, UObject* Parent, const FTarget& Target)
{
	UClass* FactoryClass = FindObject<UClass>(nullptr, bSource ? TEXT("/Script/MetasoundEditor.MetaSoundSourceFactory") : TEXT("/Script/MetasoundEditor.MetaSoundFactory"));
	if (!Test.TestNotNull(TEXT("MetaSound factory class"), FactoryClass))
	{
		return nullptr;
	}
	UFactory* Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
	if (Parent != nullptr)
	{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
		// Newer factories take a preset document template with the parent, plus the Content Browser selection.
		const FStructProperty* TemplateProperty = FindFProperty<FStructProperty>(FactoryClass, TEXT("Template"));
		const FArrayProperty* SelectedProperty = FindFProperty<FArrayProperty>(FactoryClass, TEXT("SelectedObjects"));
		if (!Test.TestNotNull(TEXT("factory Template"), TemplateProperty) || !Test.TestNotNull(TEXT("factory SelectedObjects"), SelectedProperty))
		{
			return nullptr;
		}
		TInstancedStruct<FMetaSoundFrontendDocumentTemplate>& Template =
			*TemplateProperty->ContainerPtrToValuePtr<TInstancedStruct<FMetaSoundFrontendDocumentTemplate>>(Factory);
		Template = TInstancedStruct<FMetaSoundFrontendDocumentTemplate>::Make<FMetaSoundFrontendPresetTemplate>();
		Template.GetMutable<FMetaSoundFrontendPresetTemplate>().Parent = Parent;
		*SelectedProperty->ContainerPtrToValuePtr<TArray<TObjectPtr<UObject>>>(Factory) = {Parent};
#else
		const FObjectProperty* Property = FindFProperty<FObjectProperty>(FactoryClass, TEXT("ReferencedMetaSoundObject"));
		if (!Test.TestNotNull(TEXT("ReferencedMetaSoundObject"), Property))
		{
			return nullptr;
		}
		Property->SetObjectPropertyValue_InContainer(Factory, Parent);
#endif
	}
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UObject* Asset = AssetTools.CreateAsset(Target.AssetName, FPackageName::GetLongPackagePath(Target.PackageName), Factory->GetSupportedClass(), Factory);
	if (!Test.TestNotNull(*FString::Printf(TEXT("%s created by the factory"), *Target.AssetName), Asset))
	{
		return nullptr;
	}
	return Save(Test, Asset, Target.AssetName) ? Asset : nullptr;
}

FMetasoundFrontendLiteral FloatLiteral(float Value)
{
	FMetasoundFrontendLiteral Literal;
	Literal.Set(Value);
	return Literal;
}

// The Details panel's edit of an input default (on a preset it also stops inheriting the parent's default).
void SetInputDefault(UObject& Asset, const FString& Name, float Value)
{
	FEditorSide Side(Asset);
	Side.Builder.SetGraphInputDefault(FName(*Name), FloatLiteral(Value));
}

// The preset input's override checkbox (FMetasoundInputDetailCustomization::SetInputInheritsDefault / Clear...).
void SetInputInherits(UObject& Asset, const FString& Name, bool bInherits)
{
	FEditorSide Side(Asset);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	// Newer editors keep the check box in the preset template: clearing the input's default inherits again, writing the
	// shown default overrides.
	const FMetasoundFrontendClassInput* Input = Side.Builder.FindGraphInput(FName(*Name));
	if (Input == nullptr)
	{
		return;
	}
	if (bInherits)
	{
		Side.Builder.ResetGraphInputDefault(FName(*Name));
	}
	else
	{
		Side.Builder.SetGraphInputDefaults(FName(*Name), Input->GetDefaults());
	}
#else
	Side.Builder.SetGraphInputInheritsDefault(FName(*Name), bInherits);
#endif
}

TArray<FString> FloatInputs(const TSharedPtr<FJsonObject>& Document)
{
	TArray<FString> Names;
	for (const auto& Input : Child(BodyOf(Document), TEXT("Inputs"))->Values)
	{
		FString Type;
		if (Input.Value->AsObject()->TryGetStringField(TEXT("Type"), Type) && Type == TEXT("Float"))
		{
			Names.Add(FString(*Input.Key));
		}
	}
	Names.Sort();
	return Names;
}

// The key of the node at (X, Y), or of the node whose Version is Major.
FString KeyAtPosition(const TSharedPtr<FJsonObject>& Document, double X, double Y)
{
	for (const auto& Node : NodesOf(Document)->Values)
	{
		const TArray<TSharedPtr<FJsonValue>>* Position = nullptr;
		if (Node.Value->AsObject()->TryGetArrayField(TEXT("Position"), Position) && Position->Num() == 2
			&& FMath::IsNearlyEqual((*Position)[0]->AsNumber(), X) && FMath::IsNearlyEqual((*Position)[1]->AsNumber(), Y))
		{
			return FString(*Node.Key);
		}
	}
	return FString();
}

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
// Newer engines ship MSP_MidiNoteAdsr with its MidiNoteTrigger node already updated: the copy gets back the node's
// earlier, unregistered class version 0.1 (the state the editor offers Update Node for) and is saved.
bool MarkMidiNoteTriggerOutdated(FAutomationTestBase& Test, UObject& Asset)
{
	const FStructProperty* Property = FindFProperty<FStructProperty>(Asset.GetClass(), UMetaSoundPatch::GetDocumentPropertyName());
	if (!Test.TestNotNull(TEXT("MetaSound document property"), Property))
	{
		return false;
	}
	FMetasoundFrontendDocument& Document = *Property->ContainerPtrToValuePtr<FMetasoundFrontendDocument>(&Asset);
	bool bMarked = false;
	for (FMetasoundFrontendClass& Dependency : Document.Dependencies)
	{
		const FMetasoundFrontendClassName& ClassName = Dependency.Metadata.GetClassName();
		if (ClassName.Namespace == TEXT("HarmonixNodes") && ClassName.Name == TEXT("MidiNoteTrigger") && Dependency.Metadata.GetVersion().Major == 1)
		{
			Dependency.Metadata.SetVersion({0, 1});
			bMarked = true;
		}
	}
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: MidiNoteTrigger class version set to 0.1"), *Asset.GetName()), bMarked))
	{
		return false;
	}
	Metasound::Frontend::IDocumentBuilderRegistry::GetChecked().ReloadBuilder(Document.RootGraph.Metadata.GetClassName());
	return Test.TestTrue(*FString::Printf(TEXT("%s saved"), *Asset.GetName()), UEditorLoadingAndSavingUtils::SavePackages({Asset.GetOutermost()}, false));
}
#endif

FString KeyWithVersion(const TSharedPtr<FJsonObject>& Document, int32 Major)
{
	for (const auto& Node : NodesOf(Document)->Values)
	{
		int32 Version = 1;
		if (Node.Value->AsObject()->TryGetNumberField(TEXT("Version"), Version) && Version == Major)
		{
			return FString(*Node.Key);
		}
	}
	return FString();
}

void ExpectCreateRefused(FAutomationTestBase& Test, const FTarget& Target, const TSharedPtr<FJsonObject>& Document, const TCHAR* Code, const FString& Path,
	const FString& Stage, const TCHAR* MessagePart = nullptr)
{
	const FAssetDocumentResult Result = Apply(Target, Document);
	Test.TestFalse(*FString::Printf(TEXT("%s: refused"), *Stage), Result.IsSuccess());
	const FAssetDocumentDiagnostic* Found = Result.Diagnostics.FindByPredicate([Code](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == Code; });
	if (!Test.TestNotNull(*FString::Printf(TEXT("%s: %s"), *Stage, Code), Found))
	{
		Test.AddError(FString::Printf(TEXT("%s: %s"), *Stage, *DiagnosticsText(Result)));
	}
	else
	{
		Test.TestEqual(*FString::Printf(TEXT("%s: path"), *Stage), Found->Path, Path);
		if (MessagePart != nullptr)
		{
			Test.TestTrue(*FString::Printf(TEXT("%s: the message says '%s' (%s)"), *Stage, MessagePart, *Found->Message), Found->Message.Contains(MessagePart));
		}
	}
	Test.TestTrue(*FString::Printf(TEXT("%s: nothing left"), *Stage), LeftNothing(Target));
}

// The node Details panel's configuration edit (FMetaSoundNodeConfigurationDataDetails): the property handle writes the
// document's configuration in place, then the node interface follows it.
void SetConfigurationProperty(FMetaSoundFrontendDocumentBuilder& Builder, const FGuid& NodeID, const TCHAR* Property, const TCHAR* Text)
{
	const TConstStructView<FMetaSoundFrontendNodeConfiguration> View = static_cast<const FMetaSoundFrontendDocumentBuilder&>(Builder).FindNodeConfiguration(NodeID);
	const UScriptStruct* Struct = View.GetScriptStruct();
	const FProperty* Found = Struct != nullptr ? Struct->FindPropertyByName(Property) : nullptr;
	if (!ensure(Found != nullptr))
	{
		return;
	}
	Found->ImportText_InContainer(Text, const_cast<uint8*>(View.GetMemory()), nullptr, PPF_None);
	Builder.UpdateNodeInterfaceFromConfiguration(NodeID);
}

// A source (New Asset > MetaSound Source) with a Tape Stop/Start node (class major 0) configured to two channels, both
// fed by a sine, its first channel to the output.
UObject* MakeTapeSource(FAutomationTestBase& Test, const FTarget& Target)
{
	UObject* Asset = CreateByFactory(Test, true, nullptr, Target);
	if (Asset == nullptr)
	{
		return nullptr;
	}
	{
		FEditorSide Side(*Asset);
		FMetasoundFrontendClassName Name;
		FMetasoundFrontendClassName::Parse(TapeSS, Name);
		const FMetasoundFrontendNode* Tape = Side.Builder.AddNodeByClassName(Name, 0);
		if (!Test.TestNotNull(TEXT("Tape Stop/Start node added"), Tape))
		{
			return nullptr;
		}
		const FGuid TapeID = Tape->GetID();
		Side.Move(TapeID, 400, 0);
		SetConfigurationProperty(Side.Builder, TapeID, TEXT("NumChannels"), TEXT("2"));
		const FGuid Sine = Side.AddNode(TEXT("UE.Sine.Audio"), 100, 0);
		Side.Connect(Sine, nullptr, TapeID, TEXT("In 0"));
		Side.Connect(Sine, nullptr, TapeID, TEXT("In 1"));
		// The new source's first output channel (its output format interface).
		const TCHAR* OutMember = MonoOut;
		if (!Side.OutputNode(OutMember).IsValid())
		{
			OutMember = TEXT("UE.OutputFormat.Stereo.Audio:0");
		}
		Side.Connect(TapeID, TEXT("Out 0"), Side.OutputNode(OutMember), OutMember);
	}
	return Save(Test, Asset, Target.AssetName) ? Asset : nullptr;
}

// The class inputs or outputs of the MetaSound whose names start with Prefix (its interface members), in order.
TArray<FName> MembersWithPrefix(const FMetaSoundFrontendDocumentBuilder& Builder, bool bInputs, const TCHAR* Prefix)
{
	TArray<FName> Names;
	const FMetasoundFrontendClassInterface& Interface = Builder.GetConstDocumentChecked().RootGraph.GetDefaultInterface();
	if (bInputs)
	{
		for (const FMetasoundFrontendClassInput& Input : Interface.Inputs)
		{
			if (Input.Name.ToString().StartsWith(Prefix))
			{
				Names.Add(Input.Name);
			}
		}
	}
	else
	{
		for (const FMetasoundFrontendClassOutput& Output : Interface.Outputs)
		{
			if (Output.Name.ToString().StartsWith(Prefix))
			{
				Names.Add(Output.Name);
			}
		}
	}
	Names.Sort(FNameLexicalLess());
	return Names;
}

// The engine's errors for a graph that uses HarmonixNodes.MidiNoteTrigger v0 (not registered in 5.7).
} // namespace AssetDocumentMetaSoundPresetNodeTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMetaSoundPresetTest, "UEAssetDoc.MetaSound.Preset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMetaSoundPresetTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMetaSoundPresetNodeTests;
	const FDemoTemplateContent Demo;
	UObject* Parent = LoadQuiet(OneShot);
	if (GEditor == nullptr || GEditor->Trans == nullptr || !TestNotNull(TEXT("MSS_PlayRandom_Oneshot loads"), Parent))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> ParentDocument = Extract(*this, OneShot, TEXT("parent"));
	if (!ParentDocument.IsValid())
	{
		return false;
	}
	const TArray<FString> Floats = FloatInputs(ParentDocument);
	if (!TestTrue(TEXT("the parent has two Float inputs"), Floats.Num() >= 2))
	{
		return false;
	}
	FFixtures Fixtures;

	// 1. §6.2 on presets the editor made.
	{
		struct FCase
		{
			FString Name;
			const FTarget* Target = nullptr;
		};
		TArray<FCase> Cases;
		const FTarget& ByDuplicate = Fixtures.Add(TEXT("MSS_PresetDup"));
		if (UObject* Preset = CreatePresetByDuplicate(*this, *Parent, ByDuplicate))
		{
			SetInputDefault(*Preset, Floats[0], 0.25f);
			SetInputDefault(*Preset, Floats[1], 3.0f);
			Save(*this, Preset, TEXT("preset with overrides"));
			Cases.Add({TEXT("OneShot preset (Create Preset, 2 overrides)"), &ByDuplicate});
		}
		if (UObject* OperaSource = LoadQuiet(Opera))
		{
			const FTarget& OperaPreset = Fixtures.Add(TEXT("MS_OperaPreset"));
			if (UObject* Preset = CreateByFactory(*this, true, OperaSource, OperaPreset))
			{
				const TArray<FString> OperaFloats = FloatInputs(Extract(*this, Opera, TEXT("MS_Opera")));
				if (!OperaFloats.IsEmpty())
				{
					SetInputDefault(*Preset, OperaFloats[0], 0.5f);
					Save(*this, Preset, TEXT("MS_Opera preset with an override"));
				}
				Cases.Add({TEXT("MS_Opera preset (restricted-mode factory)"), &OperaPreset});
			}
		}
		else
		{
			AddInfo(TEXT("MS_Opera (TP_UEIntro_BP template) is not available; its preset is skipped"));
		}
		const FTarget& Patch = Fixtures.Add(TEXT("MSP_PresetParent"));
		const TSharedPtr<FJsonObject> PatchDocument = Json(TEXT(R"({"SchemaVersion": 1, "Class": "/Script/MetasoundEngine.MetaSoundPatch", "Action": "Create",
			"Body": {"Metadata": {"Referenceable": true},
			  "Inputs": {"In": {"Type": "Audio"}, "Gain": {"Type": "Float", "Default": 0.5}},
			  "Outputs": {"Out": {"Type": "Audio"}},
			  "Graphs": {"Default": {"Nodes": {
			    "In": {"Class": "Input", "Member": "In", "Position": [-400, 0]},
			    "Gain": {"Class": "Input", "Member": "Gain", "Position": [-400, 120]},
			    "Mul": {"Class": "UE.Multiply.Audio by Float", "Position": [-150, 0], "Inputs": {"PrimaryOperand": "In", "AdditionalOperands": "Gain"}},
			    "Out": {"Class": "Output", "Member": "Out", "Position": [100, 0], "Inputs": {"Out": "Mul"}}}}}}})"));
		PatchDocument->SetStringField(TEXT("Target"), Patch.PackageName);
		if (ApplyOk(*this, Patch, PatchDocument, TEXT("preset parent patch Create")))
		{
			const FTarget& PatchPreset = Fixtures.Add(TEXT("MSP_Preset"));
			if (UObject* Preset = CreateByFactory(*this, false, Patch.Get(), PatchPreset))
			{
				SetInputDefault(*Preset, TEXT("Gain"), 0.9f);
				Save(*this, Preset, TEXT("patch preset with an override"));
				Cases.Add({TEXT("patch preset (factory)"), &PatchPreset});
			}
		}
		for (const FCase& Case : Cases)
		{
			const double Start = FPlatformTime::Seconds();
			const FAssetDocumentResult Original = ExtractResult(Case.Target->ObjectPath);
			const double ExtractMs = Milliseconds(Start);
			if (!TestTrue(*FString::Printf(TEXT("%s extracts"), *Case.Name), Original.IsSuccess()))
			{
				AddInfo(DiagnosticsText(Original));
				continue;
			}
			const TSharedPtr<FJsonObject> Body = BodyOf(Original.Payload);
			TestTrue(*FString::Printf(TEXT("%s: Preset.Parent extracted"), *Case.Name), Body->HasField(TEXT("Preset")) && !Body->HasField(TEXT("Graphs")));
			AddInfo(FString::Printf(TEXT("%s: %s"), *Case.Name, *Serialize(Original.Payload)));
			const FTarget& Created = Fixtures.Add(TEXT("MS_PresetNew"));
			const FRoundTrip Trip = RoundTrip(*this, Original.Payload, StatusOf(Original), Created, Case.Name);
			if (!Trip.Applied.IsSuccess())
			{
				AddError(FString::Printf(TEXT("%s: Create failed: %s"), *Case.Name, *DiagnosticsText(Trip.Applied)));
				continue;
			}
			for (const FString& Problem : Trip.Problems)
			{
				AddError(FString::Printf(TEXT("%s: %s"), *Case.Name, *Problem));
			}
			AddInfo(FString::Printf(TEXT("%s %s | extract %.0f ms, %s | %s"), Trip.Problems.IsEmpty() ? TEXT("PASS") : TEXT("FAIL"), *Case.Name, ExtractMs,
				*Trip.Timings(), *Trip.Status.Left(120)));
		}
	}

	// 2. Free edits: document vs the Details panel / Convert From Preset.
	{
		const FTarget& DocumentSide = Fixtures.Add(TEXT("MSS_PresetDoc"));
		const FTarget& EditorSide = Fixtures.Add(TEXT("MSS_PresetEd"));
		if (CreatePresetByDuplicate(*this, *Parent, DocumentSide) != nullptr && CreatePresetByDuplicate(*this, *Parent, EditorSide) != nullptr)
		{
			const FString A = Floats[0];
			const FString B = Floats[1];
			double ParentDefault = 0.0;
			const TSharedPtr<FJsonObject>* ParentEntry = nullptr;
			if (Child(BodyOf(ParentDocument), TEXT("Inputs"))->TryGetObjectField(A, ParentEntry))
			{
				(*ParentEntry)->TryGetNumberField(TEXT("Default"), ParentDefault);
			}
			TArray<FFreeEdit> Edits;
			Edits.Add({TEXT("override ") + A,
				[A](const TSharedPtr<FJsonObject>& Doc) { Child(Child(BodyOf(Doc), TEXT("Inputs")), A)->SetNumberField(TEXT("Default"), 0.25); },
				[A](UObject& Asset, const TSharedPtr<FJsonObject>&) { SetInputDefault(Asset, A, 0.25f); }});
			Edits.Add({TEXT("override ") + B,
				[B](const TSharedPtr<FJsonObject>& Doc) { Child(Child(BodyOf(Doc), TEXT("Inputs")), B)->SetNumberField(TEXT("Default"), 3.0); },
				[B](UObject& Asset, const TSharedPtr<FJsonObject>&) { SetInputDefault(Asset, B, 3.0f); }});
			Edits.Add({TEXT("inherit ") + A + TEXT(" again"),
				[A](const TSharedPtr<FJsonObject>& Doc) { Child(BodyOf(Doc), TEXT("Inputs"))->RemoveField(A); },
				[A](UObject& Asset, const TSharedPtr<FJsonObject>&) { SetInputInherits(Asset, A, true); }});
			// The override checkbox alone: the document lists the input with the value it has (the parent's).
			Edits.Add({TEXT("override ") + A + TEXT(" with the parent's value"),
				[A, ParentDefault](const TSharedPtr<FJsonObject>& Doc) { Child(Child(BodyOf(Doc), TEXT("Inputs")), A)->SetNumberField(TEXT("Default"), ParentDefault); },
				[A](UObject& Asset, const TSharedPtr<FJsonObject>&) { SetInputInherits(Asset, A, false); }});
			Edits.Add({TEXT("Convert From Preset"),
				[](const TSharedPtr<FJsonObject>& Doc) { BodyOf(Doc)->SetField(TEXT("Preset"), MakeShared<FJsonValueNull>()); },
				[](UObject& Asset, const TSharedPtr<FJsonObject>&)
				{
					// FEditor::ConvertFromPreset: a transaction, Modify, the builder's ConvertFromPreset; the editor
					// registers the converted graph on its next tick.
					{
						const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentMetaSoundTests", "ConvertFromPreset", "Convert From Preset"));
						Asset.Modify();
						EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
						Metasound::Engine::FDocumentBuilderRegistry::GetChecked().FindOrBeginBuilding(Asset).ConvertFromPreset(Result);
						ensure(Result == EMetaSoundBuilderResult::Succeeded);
					}
					UMetaSoundEditorSubsystem::GetChecked().RegisterGraphWithFrontend(Asset, true);
					Asset.MarkPackageDirty();
				}});
			TMap<FString, FString> IdMap;
			for (const FFreeEdit& Edit : Edits)
			{
				if (!CompareFreeEdit(*this, DocumentSide, EditorSide, Edit, IdMap, TEXT("preset")))
				{
					break;
				}
			}
			// The converted MetaSound is a regular one: its extraction re-applies as a no-op.
			const TSharedPtr<FJsonObject> Converted = Extract(*this, DocumentSide.ObjectPath, TEXT("converted"));
			if (Converted.IsValid())
			{
				TestFalse(TEXT("converted: no Preset"), BodyOf(Converted)->HasField(TEXT("Preset")));
				FString DiffText;
				TestEqual(TEXT("converted: Diff of its extraction is empty"), DiffCount(ForTarget(Converted, DocumentSide, TEXT("Update")), DiffText), 0);
			}
		}
	}

	// 3. What the editor cannot do.
	{
		const FTarget& Target = Fixtures.Add(TEXT("MSS_PresetRefusals"));
		const FTarget& Regular = Fixtures.Add(TEXT("MSS_NotPreset"));
		if (CreatePresetByDuplicate(*this, *Parent, Target) != nullptr && Duplicate(*this, Parent, Regular) != nullptr)
		{
			SetInputDefault(*Target.Get(), Floats[0], 0.25f);
			Save(*this, Target.Get(), TEXT("refusal preset"));
			const TSharedPtr<FJsonObject> Baseline = ForTarget(Extract(*this, Target.ObjectPath, TEXT("refusal baseline")), Target, TEXT("Update"));
			struct FBad
			{
				const TCHAR* Stage;
				TFunction<void(const TSharedPtr<FJsonObject>&)> Edit;
				const TCHAR* Code;
				FString Path;
			};
			const FString A = Floats[0];
			const TArray<FBad> Bad = {
				{TEXT("a graph"), [](const TSharedPtr<FJsonObject>& Doc) { Child(Child(BodyOf(Doc), TEXT("Graphs")), TEXT("Default")); },
					TEXT("MetaSound.PresetGraphDerived"), TEXT("/Body/Graphs")},
				{TEXT("interfaces"), [](const TSharedPtr<FJsonObject>& Doc) { BodyOf(Doc)->SetArrayField(TEXT("Interfaces"), {MakeShared<FJsonValueString>(TEXT("UE.Source.OneShot"))}); },
					TEXT("MetaSound.PresetGraphDerived"), TEXT("/Body/Interfaces")},
				{TEXT("a member setting"), [A](const TSharedPtr<FJsonObject>& Doc) { Child(Child(BodyOf(Doc), TEXT("Inputs")), A)->SetStringField(TEXT("Type"), TEXT("Int32")); },
					TEXT("MetaSound.PresetGraphDerived"), FString::Printf(TEXT("/Body/Inputs/%s/Type"), *A)},
				{TEXT("an input the parent lacks"), [](const TSharedPtr<FJsonObject>& Doc) { Child(Child(BodyOf(Doc), TEXT("Inputs")), TEXT("NoSuchInput"))->SetNumberField(TEXT("Default"), 1.0); },
					TEXT("MetaSound.UnknownMember"), TEXT("/Body/Inputs/NoSuchInput")},
				{TEXT("another parent"), [](const TSharedPtr<FJsonObject>& Doc) { Child(BodyOf(Doc), TEXT("Preset"))->SetStringField(TEXT("Parent"), Loop); },
					TEXT("MetaSound.PresetParentChanged"), TEXT("/Body/Preset/Parent")},
				{TEXT("an invalid override"), [A](const TSharedPtr<FJsonObject>& Doc) { Child(Child(BodyOf(Doc), TEXT("Inputs")), A)->SetStringField(TEXT("Default"), TEXT("loud")); },
					TEXT("MetaSound.InvalidLiteral"), FString::Printf(TEXT("/Body/Inputs/%s/Default"), *A)},
			};
			for (const FBad& Case : Bad)
			{
				const TSharedPtr<FJsonObject> Document = Parse(Serialize(Baseline));
				Case.Edit(Document);
				ExpectRefused(*this, Target, Document, Case.Code, Case.Path, FString(TEXT("preset: ")) + Case.Stage);
			}
			// A regular MetaSound cannot become a preset.
			const TSharedPtr<FJsonObject> Convert = ForTarget(Extract(*this, Regular.ObjectPath, TEXT("regular")), Regular, TEXT("Update"));
			Child(BodyOf(Convert), TEXT("Preset"))->SetStringField(TEXT("Parent"), OneShot);
			ExpectRefused(*this, Regular, Convert, TEXT("MetaSound.CannotConvertToPreset"), TEXT("/Body/Preset"), TEXT("regular MetaSound made a preset"));
			// A Create whose parent is missing or of the other class leaves nothing.
			const FTarget& Missing = Fixtures.Add(TEXT("MSS_PresetMissing"));
			const TSharedPtr<FJsonObject> MissingParent = Json(TEXT(R"({"SchemaVersion": 1, "Class": "/Script/MetasoundEngine.MetaSoundSource", "Action": "Create",
				"Body": {"Preset": {"Parent": "/Game/AssetDocumentMetaSound/NoSuchParent"}}})"));
			MissingParent->SetStringField(TEXT("Target"), Missing.PackageName);
			ExpectCreateRefused(*this, Missing, MissingParent, TEXT("MetaSound.InvalidPresetParent"), TEXT("/Body/Preset/Parent"), TEXT("missing parent"));
			// A parent in the wrong folder: the refusal names the existing MetaSound Source of that name.
			const TSharedPtr<FJsonObject> Misplaced = Parse(Serialize(MissingParent));
			Child(BodyOf(Misplaced), TEXT("Preset"))->SetStringField(TEXT("Parent"), TEXT("/Game/Audio/MSS_PlayRandom_OneShot"));
			ExpectCreateRefused(*this, Missing, Misplaced, TEXT("MetaSound.InvalidPresetParent"), TEXT("/Body/Preset/Parent"),
				TEXT("misplaced parent"), OneShot);
			const FTarget& Patch = Fixtures.Add(TEXT("MSP_ForSourcePreset"));
			if (CreateByFactory(*this, false, nullptr, Patch) != nullptr)
			{
				const TSharedPtr<FJsonObject> PatchParent = Parse(Serialize(MissingParent));
				Child(BodyOf(PatchParent), TEXT("Preset"))->SetStringField(TEXT("Parent"), Patch.ObjectPath);
				ExpectCreateRefused(*this, Missing, PatchParent, TEXT("MetaSound.InvalidPresetParent"), TEXT("/Body/Preset/Parent"), TEXT("patch parent of a source preset"));
			}
			const TSharedPtr<FJsonObject> WithGraph = Parse(Serialize(MissingParent));
			Child(BodyOf(WithGraph), TEXT("Preset"))->SetStringField(TEXT("Parent"), OneShot);
			Child(Child(BodyOf(WithGraph), TEXT("Graphs")), TEXT("Default"));
			ExpectCreateRefused(*this, Missing, WithGraph, TEXT("MetaSound.PresetGraphDerived"), TEXT("/Body/Graphs"), TEXT("Create of a preset with a graph"));
		}
	}

	// 4. Undo / redo around an AI override.
	{
		const FTarget& Target = Fixtures.Add(TEXT("MSS_PresetUndo"));
		if (CreatePresetByDuplicate(*this, *Parent, Target) != nullptr)
		{
			SetInputDefault(*Target.Get(), Floats[1], 2.0f);
			TestTrue(TEXT("preset undo: the human override is in the undo history"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
			TestTrue(TEXT("preset undo: human save"), UEditorLoadingAndSavingUtils::SavePackages({Target.Get()->GetOutermost()}, false));
			const TSharedPtr<FJsonObject> Document = ForTarget(Extract(*this, Target.ObjectPath, TEXT("preset undo")), Target, TEXT("Update"));
			Child(Child(BodyOf(Document), TEXT("Inputs")), Floats[0])->SetNumberField(TEXT("Default"), 0.75);
			if (ApplyOk(*this, Target, Document, TEXT("preset AI override")))
			{
				TestFalse(TEXT("preset undo: no undo history references the preset"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
				const FString After = Serialize(Extract(*this, Target.ObjectPath, TEXT("after the AI override")));
				UndoRedoAll(*this, Target, TEXT("preset"));
				TestEqual(TEXT("preset after undo / redo"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after undo / redo"))), After);
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMetaSoundConfigurationTest, "UEAssetDoc.MetaSound.Configuration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMetaSoundConfigurationTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMetaSoundPresetNodeTests;
	FMetasoundFrontendClassName TapeName;
	FMetasoundFrontendClassName::Parse(TapeSS, TapeName);
	FMetasoundFrontendClass TapeClass;
	if (!Metasound::Frontend::ISearchEngine::Get().FindClassWithHighestVersion(TapeName, TapeClass))
	{
		AddInfo(TEXT("SKIP: HarmonixNodes.TapeSS is not registered (enable the Harmonix plugin in the project to run the configuration test)"));
		return true;
	}
	FFixtures Fixtures;
	const FTarget& Fixture = Fixtures.Add(TEXT("MSS_Tape"));
	if (MakeTapeSource(*this, Fixture) == nullptr)
	{
		return false;
	}
	// §6.2.
	{
		const double Start = FPlatformTime::Seconds();
		const FAssetDocumentResult Original = ExtractResult(Fixture.ObjectPath);
		const double ExtractMs = Milliseconds(Start);
		if (!TestTrue(TEXT("configured source extracts"), Original.IsSuccess()))
		{
			AddInfo(DiagnosticsText(Original));
			return false;
		}
		AddInfo(FString::Printf(TEXT("configured source: %s"), *SerializePretty(Original.Payload)));
		const FString Key = KeyAtPosition(Original.Payload, 400, 0);
		const TSharedPtr<FJsonObject>* Configuration = nullptr;
		if (TestFalse(TEXT("the Tape node is extracted"), Key.IsEmpty()) && TestTrue(TEXT("its Configuration is extracted"),
			NodeOf(Original.Payload, Key)->TryGetObjectField(TEXT("Configuration"), Configuration)))
		{
			TestEqual(TEXT("NumChannels extracted"), Child(*Configuration, TEXT("Properties"))->GetIntegerField(TEXT("NumChannels")), 2);
		}
		const FTarget& Created = Fixtures.Add(TEXT("MSS_TapeNew"));
		const FRoundTrip Trip = RoundTrip(*this, Original.Payload, StatusOf(Original), Created, TEXT("configured source"));
		if (!TestTrue(TEXT("configured source Create"), Trip.Applied.IsSuccess()))
		{
			AddError(DiagnosticsText(Trip.Applied));
		}
		for (const FString& Problem : Trip.Problems)
		{
			AddError(FString::Printf(TEXT("configured source: %s"), *Problem));
		}
		AddInfo(FString::Printf(TEXT("%s configured source | extract %.0f ms, %s | %s"), Trip.Problems.IsEmpty() && Trip.Applied.IsSuccess() ? TEXT("PASS") : TEXT("FAIL"),
			ExtractMs, *Trip.Timings(), *Trip.Status.Left(120)));
	}
	// Free edits: the channel count (the node's pins follow it).
	const FTarget& DocumentSide = Fixtures.Add(TEXT("MSS_TapeDoc"));
	const FTarget& EditorSide = Fixtures.Add(TEXT("MSS_TapeEd"));
	if (Duplicate(*this, Fixture.Get(), DocumentSide) == nullptr || Duplicate(*this, Fixture.Get(), EditorSide) == nullptr
		|| !StartBoth(*this, DocumentSide, EditorSide, TEXT("configuration")))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Start = Extract(*this, DocumentSide.ObjectPath, TEXT("configuration start"));
	const FString Tape = KeyAtPosition(Start, 400, 0);
	const FString Sine = KeyAtPosition(Start, 100, 0);
	if (!TestFalse(TEXT("Tape and Sine keys"), Tape.IsEmpty() || Sine.IsEmpty()))
	{
		return false;
	}
	TArray<FFreeEdit> Edits;
	Edits.Add({TEXT("three channels, the third fed"),
		[Tape, Sine](const TSharedPtr<FJsonObject>& Doc)
		{
			Child(Child(NodeOf(Doc, Tape), TEXT("Configuration")), TEXT("Properties"))->SetNumberField(TEXT("NumChannels"), 3);
			SetInputs(Doc, Tape, TEXT("In 2"), Sine);
		},
		[Tape, Sine](UObject& Asset, const TSharedPtr<FJsonObject>& Doc)
		{
			FEditorSide Side(Asset);
			SetConfigurationProperty(Side.Builder, FEditorSide::Id(Doc, Tape), TEXT("NumChannels"), TEXT("3"));
			Side.Connect(FEditorSide::Id(Doc, Sine), nullptr, FEditorSide::Id(Doc, Tape), TEXT("In 2"));
		}});
	Edits.Add({TEXT("one channel and a longer ramp"),
		[Tape](const TSharedPtr<FJsonObject>& Doc)
		{
			const TSharedPtr<FJsonObject> Properties = Child(Child(NodeOf(Doc, Tape), TEXT("Configuration")), TEXT("Properties"));
			Properties->SetNumberField(TEXT("NumChannels"), 1);
			Properties->SetNumberField(TEXT("MaxRampSeconds"), 8.0);
			SetInputs(Doc, Tape, TEXT("In 1"), FString());
			SetInputs(Doc, Tape, TEXT("In 2"), FString());
		},
		[Tape](UObject& Asset, const TSharedPtr<FJsonObject>& Doc)
		{
			FEditorSide Side(Asset);
			SetConfigurationProperty(Side.Builder, FEditorSide::Id(Doc, Tape), TEXT("NumChannels"), TEXT("1"));
			SetConfigurationProperty(Side.Builder, FEditorSide::Id(Doc, Tape), TEXT("MaxRampSeconds"), TEXT("8.0"));
		}});
	TMap<FString, FString> IdMap;
	for (const FFreeEdit& Edit : Edits)
	{
		if (!CompareFreeEdit(*this, DocumentSide, EditorSide, Edit, IdMap, TEXT("configuration")))
		{
			break;
		}
	}
	// Invalid configurations.
	const TSharedPtr<FJsonObject> Baseline = ForTarget(Extract(*this, DocumentSide.ObjectPath, TEXT("configuration errors")), DocumentSide, TEXT("Update"));
	const FString TapePath = FString::Printf(TEXT("/Body/Graphs/Default/Nodes/%s/Configuration"), *Tape);
	struct FBad
	{
		const TCHAR* Stage;
		TFunction<void(const TSharedPtr<FJsonObject>&)> Edit;
		FString Path;
	};
	const TArray<FBad> Bad = {
		{TEXT("out of range"), [Tape](const TSharedPtr<FJsonObject>& Doc) { Child(Child(NodeOf(Doc, Tape), TEXT("Configuration")), TEXT("Properties"))->SetNumberField(TEXT("NumChannels"), 9); },
			TapePath + TEXT("/Properties/NumChannels")},
		{TEXT("unknown setting"), [Tape](const TSharedPtr<FJsonObject>& Doc) { Child(Child(NodeOf(Doc, Tape), TEXT("Configuration")), TEXT("Properties"))->SetNumberField(TEXT("Speed"), 2); },
			TapePath + TEXT("/Properties/Speed")},
		{TEXT("other struct"), [Tape](const TSharedPtr<FJsonObject>& Doc)
			{
				const TSharedPtr<FJsonObject> Configuration = Child(NodeOf(Doc, Tape), TEXT("Configuration"));
				Configuration->SetStringField(TEXT("Type"), TEXT("/Script/MetasoundFrontend.MetaSoundFrontendNodeConfiguration"));
				Child(Configuration, TEXT("Properties"))->SetNumberField(TEXT("NumChannels"), 2);
			},
			TapePath + TEXT("/Type")},
		{TEXT("a class without configuration"), [Sine](const TSharedPtr<FJsonObject>& Doc) { Child(Child(NodeOf(Doc, Sine), TEXT("Configuration")), TEXT("Properties"))->SetNumberField(TEXT("NumChannels"), 2); },
			FString::Printf(TEXT("/Body/Graphs/Default/Nodes/%s/Configuration"), *Sine)},
	};
	for (const FBad& Case : Bad)
	{
		const TSharedPtr<FJsonObject> Document = Parse(Serialize(Baseline));
		Case.Edit(Document);
		ExpectRefused(*this, DocumentSide, Document, TEXT("MetaSound.InvalidConfiguration"), Case.Path, FString(TEXT("configuration: ")) + Case.Stage);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMetaSoundUpdateNodeTest, "UEAssetDoc.MetaSound.UpdateNode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMetaSoundUpdateNodeTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMetaSoundPresetNodeTests;
	// MSP_MidiNoteAdsr's node class version is not registered: the engine logs it whenever such a graph registers.
	IgnoreUnregisteredMidiNoteTrigger(*this);
	FFixtures Fixtures;
	// A Version with no registered major (any node class).
	{
		const FTarget& Target = Fixtures.Add(TEXT("MSS_UpdateBad"));
		if (Duplicate(*this, LoadQuiet(OneShot), Target) != nullptr)
		{
			const TSharedPtr<FJsonObject> Document = ForTarget(Extract(*this, Target.ObjectPath, TEXT("update refusal")), Target, TEXT("Update"));
			NodeOf(Document, TEXT("Wave_Player"))->SetNumberField(TEXT("Version"), 7);
			ExpectRefused(*this, Target, Document, TEXT("MetaSound.UpdateNodeNotSupported"), TEXT("/Body/Graphs/Default/Nodes/Wave_Player/Version"),
				TEXT("unregistered major"));
		}
	}
	UObject* Source = LoadQuiet(MidiNoteAdsr);
	if (Source == nullptr)
	{
		AddInfo(TEXT("SKIP MSP_MidiNoteAdsr: the Harmonix plugin is not enabled in this project"));
		return true;
	}
	const FTarget& DocumentSide = Fixtures.Add(TEXT("MSP_UpdateDoc"));
	const FTarget& EditorSide = Fixtures.Add(TEXT("MSP_UpdateEd"));
	UObject* DocumentCopy = Duplicate(*this, Source, DocumentSide);
	UObject* EditorCopy = Duplicate(*this, Source, EditorSide);
	if (DocumentCopy == nullptr || EditorCopy == nullptr)
	{
		return false;
	}
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	if (!MarkMidiNoteTriggerOutdated(*this, *DocumentCopy) || !MarkMidiNoteTriggerOutdated(*this, *EditorCopy))
	{
		return false;
	}
#endif
	if (!StartBoth(*this, DocumentSide, EditorSide, TEXT("Update Node")))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Start = Extract(*this, DocumentSide.ObjectPath, TEXT("Update Node start"));
	const FString Key = KeyWithVersion(Start, 0);
	if (!TestFalse(TEXT("MSP_MidiNoteAdsr has a node of class version 0"), Key.IsEmpty()))
	{
		AddInfo(Serialize(Start));
		return false;
	}
	// Its Create is refused with the hint.
	{
		const FTarget& Created = Fixtures.Add(TEXT("MSP_UpdateNew"));
		ExpectCreateRefused(*this, Created, ForTarget(Start, Created, TEXT("Create")), TEXT("MetaSound.UnknownNodeClass"),
			FString::Printf(TEXT("/Body/Graphs/Default/Nodes/%s/Version"), *Key), TEXT("Create with an unregistered version"), TEXT("Set Version to 1"));
	}
	const FFreeEdit Update{TEXT("Update Node"),
		[Key](const TSharedPtr<FJsonObject>& Doc)
		{
			NodeOf(Doc, Key)->RemoveField(TEXT("Version"));
			// Version 1 has no "Track Number" input and no "Normalized Velocity" output: the editor drops their
			// connections, the document leaves them out.
			SetInputs(Doc, Key, TEXT("Track Number"), FString());
			for (const auto& Node : NodesOf(Doc)->Values)
			{
				const TSharedPtr<FJsonObject>* Inputs = nullptr;
				if (Node.Value->AsObject()->TryGetObjectField(TEXT("Inputs"), Inputs))
				{
					TArray<FString> Pins;
					for (const auto& Input : (*Inputs)->Values)
					{
						if (Input.Value->AsString() == Key + TEXT(".Normalized Velocity"))
						{
							Pins.Add(FString(*Input.Key));
						}
					}
					for (const FString& Pin : Pins)
					{
						(*Inputs)->RemoveField(Pin);
					}
				}
			}
		},
		[Key](UObject& Asset, const TSharedPtr<FJsonObject>& Doc)
		{
			using namespace Metasound::Frontend;
			// FEditor::UpdateSelectedNodeClasses.
			{
				const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentMetaSoundTests", "UpdateNode", "Update MetaSound Node(s) Class(es)"));
				Asset.Modify();
				FMetasoundAssetBase* MetaSound = IMetaSoundAssetManager::Get()->GetAsAsset(Asset);
				PRAGMA_DISABLE_DEPRECATION_WARNINGS
				FNodeHandle Node = MetaSound->GetRootGraphHandle()->GetNodeWithID(FEditorSide::Id(Doc, Key));
				FMetasoundFrontendClass Highest;
				if (ensure(ISearchEngine::Get().FindClassWithHighestVersion(Node->GetClassMetadata().GetClassName(), Highest)))
				{
					Node->ReplaceWithVersion(Highest.Metadata.GetVersion(), nullptr, nullptr);
				}
				FDocumentHandle DocumentHandle = MetaSound->GetDocumentHandle();
				DocumentHandle->RemoveUnreferencedDependencies();
				DocumentHandle->SynchronizeDependencyMetadata();
				PRAGMA_ENABLE_DEPRECATION_WARNINGS
				MetaSound->GetModifyContext().SetDocumentModified();
			}
			const IMetaSoundDocumentInterface* Document = Cast<IMetaSoundDocumentInterface>(&Asset);
			IDocumentBuilderRegistry::GetChecked().ReloadBuilder(Document->GetConstDocument().RootGraph.Metadata.GetClassName());
			UMetaSoundEditorSubsystem::GetChecked().RegisterGraphWithFrontend(Asset, true);
			Asset.MarkPackageDirty();
		}};
	TMap<FString, FString> IdMap;
	if (CompareFreeEdit(*this, DocumentSide, EditorSide, Update, IdMap, TEXT("MSP_MidiNoteAdsr")))
	{
		const TSharedPtr<FJsonObject> Updated = Extract(*this, DocumentSide.ObjectPath, TEXT("updated"));
		TestTrue(TEXT("the updated node has the registered version"), Updated.IsValid() && KeyWithVersion(Updated, 0).IsEmpty());
		// The updated asset re-applies as a no-op and reads back the same after a reload.
		const FAssetDocumentResult Again = Apply(DocumentSide, ForTarget(Updated, DocumentSide, TEXT("Update")));
		TestTrue(TEXT("updated: re-apply is a no-op"), Again.IsSuccess() && !Again.bSavedAsset);
		ReleaseUndoHistory();
		UPackageTools::UnloadPackages({DocumentSide.Get()->GetOutermost()});
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		LoadQuiet(DocumentSide.ObjectPath);
		TestEqual(TEXT("updated: same after a reload"), Serialize(Extract(*this, DocumentSide.ObjectPath, TEXT("updated, reloaded"))), Serialize(Updated));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMetaSoundWidgetTest, "UEAssetDoc.MetaSound.Widget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMetaSoundWidgetTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMetaSoundPresetNodeTests;
	UObject* Source = LoadQuiet(OneShot);
	if (!TestNotNull(TEXT("MSS_PlayRandom_Oneshot loads"), Source))
	{
		return false;
	}
	const TArray<FString> Floats = FloatInputs(Extract(*this, OneShot, TEXT("widget source")));
	FFixtures Fixtures;
	const FTarget& DocumentSide = Fixtures.Add(TEXT("MSS_WidgetDoc"));
	const FTarget& EditorSide = Fixtures.Add(TEXT("MSS_WidgetEd"));
	if (!TestTrue(TEXT("a Float input"), !Floats.IsEmpty()) || Duplicate(*this, Source, DocumentSide) == nullptr
		|| Duplicate(*this, Source, EditorSide) == nullptr || !StartBoth(*this, DocumentSide, EditorSide, TEXT("widget")))
	{
		return false;
	}
	const FString Input = Floats[0];
	// Every setting of the Details panel's Widget and Default Value categories, written as the panel writes them.
	const TArray<TPair<const TCHAR*, const TCHAR*>> Settings = {
		{TEXT("WidgetType"), TEXT("Slider")}, {TEXT("WidgetOrientation"), TEXT("Orient_Vertical")}, {TEXT("WidgetUnitValueType"), TEXT("Frequency")},
		{TEXT("Range"), TEXT("(X=-24.000000,Y=24.000000)")}, {TEXT("ClampDefault"), TEXT("True")}, {TEXT("VolumeWidgetUseLinearOutput"), TEXT("False")},
		{TEXT("VolumeWidgetDecibelRange"), TEXT("(X=-60.000000,Y=-6.000000)")}};
	{
		FEditorSide Side(*EditorSide.Get());
		EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
		UMetaSoundBuilderBase& Builder = Metasound::Engine::FDocumentBuilderRegistry::GetChecked().FindOrBeginBuilding(*EditorSide.Get());
		UMetaSoundFrontendMemberMetadata* Metadata = UMetaSoundEditorSubsystem::GetChecked().FindOrCreateGraphInputMetadata(&Builder, FName(*Input), Result);
		if (!TestNotNull(TEXT("the input's member settings"), Metadata))
		{
			return false;
		}
		for (const TPair<const TCHAR*, const TCHAR*>& Setting : Settings)
		{
			FProperty* Property = Metadata->GetClass()->FindPropertyByName(Setting.Key);
			if (!TestNotNull(*FString::Printf(TEXT("setting %s"), Setting.Key), Property))
			{
				continue;
			}
			FEditPropertyChain Chain;
			Chain.AddHead(Property);
			Chain.SetActiveMemberPropertyNode(Property);
			Chain.SetActivePropertyNode(Property);
			Metadata->Modify();
			Metadata->PreEditChange(Chain);
			Property->ImportText_InContainer(Setting.Value, Metadata, Metadata, PPF_None);
			// No MetaSound editor is open: the panel's interactive change (see the edit primitive's input.widget).
			FPropertyChangedEvent Event(Property, EPropertyChangeType::Interactive);
			FPropertyChangedChainEvent ChainEvent(Chain, Event);
			Metadata->PostEditChangeChainProperty(ChainEvent);
		}
	}
	const TSharedPtr<FJsonObject> EditorResult = Extract(*this, EditorSide.ObjectPath, TEXT("widget, editor side"));
	const TSharedPtr<FJsonObject>* Widget = nullptr;
	if (!EditorResult.IsValid() || !TestTrue(TEXT("the editor's widget settings are extracted"),
		Child(Child(BodyOf(EditorResult), TEXT("Inputs")), Input)->TryGetObjectField(TEXT("Widget"), Widget)))
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("widget: %s"), *Serialize(*Widget)));
	for (const TPair<const TCHAR*, const TCHAR*>& Setting : Settings)
	{
		TestTrue(*FString::Printf(TEXT("widget setting %s is extracted"), Setting.Key), (*Widget)->HasField(Setting.Key));
	}
	const TSharedPtr<FJsonObject> Document = ForTarget(Extract(*this, DocumentSide.ObjectPath, TEXT("widget, document side")), DocumentSide, TEXT("Update"));
	Child(Child(BodyOf(Document), TEXT("Inputs")), Input)->SetObjectField(TEXT("Widget"), Parse(Serialize(*Widget)));
	if (ApplyOk(*this, DocumentSide, Document, TEXT("widget")))
	{
		TMap<FString, FString> IdMap;
		const TSharedPtr<FJsonObject> DocumentResult = Extract(*this, DocumentSide.ObjectPath, TEXT("widget, document side after"));
		MapIds(DocumentResult, EditorResult, IdMap);
		TestEqual(TEXT("widget: document and editor give the same asset"), GraphForm(DocumentResult, {}), GraphForm(EditorResult, IdMap));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMetaSoundOpenEditorTest, "UEAssetDoc.MetaSound.OpenEditor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMetaSoundOpenEditorTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMetaSoundPresetNodeTests;
	UObject* Source = LoadQuiet(OneShot);
	UAssetEditorSubsystem* Editors = GEditor != nullptr ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	if (Editors == nullptr || GEditor->Trans == nullptr || !TestNotNull(TEXT("MSS_PlayRandom_Oneshot loads"), Source))
	{
		return false;
	}
	// Opening the first MetaSound editor primes the MetaSound asset tags, which registers MSP_MidiNoteAdsr too.
	IgnoreUnregisteredMidiNoteTrigger(*this);
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("MSS_Open"));
	UObject* Asset = Duplicate(*this, Source, Target);
	if (!TestNotNull(TEXT("duplicate"), Asset))
	{
		return false;
	}
	UnloadMetaSoundsNeedingAudio(*this, *Asset);
	if (!Editors->OpenEditorForAsset(Asset))
	{
		AddWarning(TEXT("The MetaSound editor did not open in this session; the open-editor apply was not exercised."));
		return true;
	}
	ON_SCOPE_EXIT
	{
		if (UObject* Open = Target.Get())
		{
			Editors->CloseAllEditorsForAsset(Open);
		}
	};
	TickEditor();
	UEdGraph* Graph = EditorGraphOf(*Asset);
	if (!TestNotNull(TEXT("the open editor has its graph"), Graph))
	{
		return false;
	}
	const int32 NodesBefore = Graph->Nodes.Num();
	ReleaseUndoHistory();

	// An apply lands in the open editor.
	const TSharedPtr<FJsonObject> Baseline = Extract(*this, Target.ObjectPath, TEXT("open baseline"));
	const TSharedPtr<FJsonObject> Edit = ForTarget(Baseline, Target, TEXT("Update"));
	NodesOf(Edit)->SetObjectField(TEXT("Extra"), Json(TEXT(R"({"Class": "UE.Sine.Audio", "Position": [0, 1200]})")));
	Child(BodyOf(Edit), TEXT("Metadata"))->SetStringField(TEXT("Description"), TEXT("edited while open"));
	const double Start = FPlatformTime::Seconds();
	if (!ApplyOk(*this, Target, Edit, TEXT("apply while open")))
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("applied while open in %.0f ms"), Milliseconds(Start)));
	TickEditor();
	Graph = EditorGraphOf(*Asset);
	TestTrue(TEXT("the editor is still open"), MetaSoundIsOpen(*Editors, *Asset));
	TestEqual(TEXT("the open editor's graph shows the new node"), Graph != nullptr ? Graph->Nodes.Num() : -1, NodesBefore + 1);
	const FString Applied = Serialize(Extract(*this, Target.ObjectPath, TEXT("after apply while open")));
	TestTrue(TEXT("the extraction has the new node"), Applied.Contains(TEXT("\"Extra\"")) || Applied.Contains(TEXT("UE.Sine.Audio")));
	TestFalse(TEXT("after the apply no undo history references the MetaSound"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));

	// A failing apply rolls back with the editor open.
	{
		const TSharedPtr<FJsonObject> Failing = ForTarget(Parse(Applied), Target, TEXT("Update"));
		SetInputs(Failing, TEXT("Wave_Player"), TEXT("Play"), TEXT("Num"));
		ExpectRefused(*this, Target, Failing, TEXT("MetaSound.IncompatibleConnection"), TEXT("/Body/Graphs/Default/Nodes/Wave_Player/Inputs/Play"),
			TEXT("failing apply while open"));
		TickEditor();
		Graph = EditorGraphOf(*Asset);
		TestEqual(TEXT("after the rollback the open editor's graph is unchanged"), Graph != nullptr ? Graph->Nodes.Num() : -1, NodesBefore + 1);
	}

	// The editor's own edit and undo / redo after the apply.
	{
		const TSharedPtr<FJsonObject> Now = Extract(*this, Target.ObjectPath, TEXT("before the human edit"));
		{
			FEditorSide Side(*Asset);
			Side.Move(FEditorSide::Id(Now, TEXT("Wave_Player")), 1900, 0);
		}
		TickEditor();
		const FString Human = Serialize(Extract(*this, Target.ObjectPath, TEXT("after the human edit")));
		TestTrue(TEXT("the human edit landed"), Human.Contains(TEXT("[1900,0]")));
		TestTrue(TEXT("the human edit can be undone"), GEditor->Trans->CanUndo() && GEditor->UndoTransaction());
		TickEditor();
		TestEqual(TEXT("undo restores the applied state"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after undo"))), Applied);
		TestTrue(TEXT("the human edit redoes"), GEditor->Trans->CanRedo() && GEditor->RedoTransaction());
		TickEditor();
		TestEqual(TEXT("redo restores the human edit"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after redo"))), Human);
		Graph = EditorGraphOf(*Asset);
		TestEqual(TEXT("after undo / redo the open editor's graph has every node"), Graph != nullptr ? Graph->Nodes.Num() : -1, NodesBefore + 1);
		// Applies need the human's changes saved first (as for every asset).
		Save(*this, Asset, TEXT("the human edit"));
		// Another apply after the human edit, then undo of everything left.
		const TSharedPtr<FJsonObject> Again = ForTarget(Parse(Human), Target, TEXT("Update"));
		// The added node, keyed by its class in the extraction.
		const FString ExtraKey = KeyAtPosition(Again, 0, 1200);
		TestFalse(TEXT("the added node is extracted"), ExtraKey.IsEmpty());
		NodesOf(Again)->RemoveField(ExtraKey);
		if (ApplyOk(*this, Target, Again, TEXT("second apply while open")))
		{
			TickEditor();
			Graph = EditorGraphOf(*Asset);
			TestEqual(TEXT("the open editor's graph drops the removed node"), Graph != nullptr ? Graph->Nodes.Num() : -1, NodesBefore);
			const FString Second = Serialize(Extract(*this, Target.ObjectPath, TEXT("after the second apply")));
			UndoRedoAll(*this, Target, TEXT("open editor"));
			TickEditor();
			TestEqual(TEXT("undo / redo after the second apply keep it"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after undo / redo"))), Second);
		}
	}
	TestTrue(TEXT("the asset is still open in its editor"), MetaSoundIsOpen(*Editors, *Asset));
	return true;
}
// Opening the MetaSound editor re-initializes every Float input's widget (WidgetType from the editor preference
// Default Input Widget Type). The document's widget defaults are the editor's, so a MetaSound made by a document
// still diffs to nothing after its editor is opened: create, open the editor, diff -> nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMetaSoundOpenEditorWidgetTest, "UEAssetDoc.MetaSound.OpenEditorWidget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMetaSoundOpenEditorWidgetTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMetaSoundPresetNodeTests;
	UAssetEditorSubsystem* Editors = GEditor != nullptr ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	if (Editors == nullptr)
	{
		return false;
	}
	IgnoreUnregisteredMidiNoteTrigger(*this);
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("MSS_OpenWidget"));
	const TSharedPtr<FJsonObject> Create = Json(TEXT(R"({"SchemaVersion": 1, "Class": "/Script/MetasoundEngine.MetaSoundSource", "Action": "Create",
		"Properties": {"OutputFormat": "Mono"},
		"Body": {"Interfaces": ["UE.Source.OneShot"],
		  "Inputs": {"Pitch": {"Type": "Float", "Default": 440.0, "Widget": {"Range": {"X": 100, "Y": 2000}}},
		             "Plain": {"Type": "Float", "Default": 1.0},
		             "Gain": {"Type": "Float", "Default": 0.5, "Widget": {"WidgetType": "Slider"}},
		             "Count": {"Type": "Int32", "Default": 2}},
		  "Graphs": {"Default": {"Nodes": {
		    "Play": {"Class": "Input", "Member": "UE.Source.OnPlay"},
		    "Pitch": {"Class": "Input", "Member": "Pitch"},
		    "Osc": {"Class": "UE.Sine.Audio", "Inputs": {"Frequency": "Pitch"}},
		    "Out": {"Class": "Output", "Member": "UE.OutputFormat.Mono.Audio:0", "Inputs": {"UE.OutputFormat.Mono.Audio:0": "Osc"}},
		    "Done": {"Class": "Output", "Member": "UE.Source.OneShot.OnFinished", "Inputs": {"UE.Source.OneShot.OnFinished": "Play"}}}}}}})"));
	Create->SetStringField(TEXT("Target"), Target.PackageName);
	if (!ApplyOk(*this, Target, Create, TEXT("create")))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Created = Extract(*this, Target.ObjectPath, TEXT("created"));
	UObject* Asset = Target.Get();
	if (!Created.IsValid() || !TestNotNull(TEXT("the MetaSound"), Asset))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Pitch = nullptr;
	const bool bPitchWidget = Child(Child(BodyOf(Created), TEXT("Inputs")), TEXT("Pitch"))->TryGetObjectField(TEXT("Widget"), Pitch);
	AddInfo(FString::Printf(TEXT("created Pitch widget: %s"), bPitchWidget ? *Serialize(*Pitch) : TEXT("none")));
	TestTrue(TEXT("the created Pitch lists only its Range (the WidgetType is the editor's default)"),
		bPitchWidget && (*Pitch)->Values.Num() == 1 && (*Pitch)->HasField(TEXT("Range")));
	UnloadMetaSoundsNeedingAudio(*this, *Asset);
	if (!Editors->OpenEditorForAsset(Asset))
	{
		AddWarning(TEXT("The MetaSound editor did not open in this session; the widget re-initialization was not exercised."));
		return true;
	}
	ON_SCOPE_EXIT
	{
		if (UObject* Open = Target.Get())
		{
			Editors->CloseAllEditorsForAsset(Open);
		}
	};
	TickEditor();
	TestTrue(TEXT("the open editor has its graph"), EditorGraphOf(*Asset) != nullptr);
	// UE 5.7's editor re-initializes the member settings on every first open (the editor graph is transient and each
	// re-bind counts as new), which resets a WidgetType the document set (Gain's Slider) to the editor preference; newer
	// engines keep the settings an asset holds. Pinned here so the guide's note is revisited when it changes. Nothing
	// else differs.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	constexpr bool bOpenResetsWidgetType = false;
#else
	constexpr bool bOpenResetsWidgetType = true;
#endif
	FString Text;
	TestEqual(TEXT("diff of the created document after opening the editor: only Gain's reset WidgetType, if the editor resets it"),
		DiffCount(ForTarget(Created, Target, TEXT("Update")), Text), bOpenResetsWidgetType ? 1 : 0);
	if (bOpenResetsWidgetType)
	{
		TestTrue(TEXT("the one change is Gain's widget"), Text.Contains(TEXT("/Body/Inputs/Gain/Widget")));
	}
	AddInfo(Text);
	const TSharedPtr<FJsonObject> Opened = Extract(*this, Target.ObjectPath, TEXT("opened"));
	if (Opened.IsValid())
	{
		const TSharedPtr<FJsonObject> Gain = Child(Child(BodyOf(Opened), TEXT("Inputs")), TEXT("Gain"));
		AddInfo(FString::Printf(TEXT("Gain after opening the editor: %s"), *Serialize(Gain)));
		TestEqual(TEXT("Gain's WidgetType after opening the editor is the editor default only if the editor resets it"),
			!Gain->HasField(TEXT("Widget")), bOpenResetsWidgetType);
		if (bOpenResetsWidgetType)
		{
			Gain->SetObjectField(TEXT("Widget"), Json(TEXT(R"({"WidgetType": "Slider"})")));
		}
	}
	TestEqual(TEXT("the extract after opening the editor is the created one (but Gain's WidgetType)"), Serialize(Opened), Serialize(Created));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMetaSoundFormatsTest, "UEAssetDoc.MetaSound.Formats",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMetaSoundFormatsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMetaSoundPresetNodeTests;
	FFixtures Fixtures;
	const TArray<FString> Spatial = {TEXT("UE.Attenuation"), TEXT("UE.Listener.Orientation"), TEXT("UE.Source.Orientation"), TEXT("UE.Spatialization")};
	const FTarget& Surround = Fixtures.Add(TEXT("MSS_Surround"));
	const FTarget& StereoPatch = Fixtures.Add(TEXT("MSP_StereoIO"));

	// A 7.1 source with the spatial interfaces, made in the editor: New MetaSound Source, Details > Output Format 7.1,
	// the Interfaces panel, a Sine fed by an attenuation input and wired to every channel.
	{
		UObject* Asset = CreateByFactory(*this, true, nullptr, Surround);
		if (Asset == nullptr)
		{
			return false;
		}
		SetOutputFormat(*Asset, EMetaSoundOutputAudioFormat::SevenDotOne);
		{
			FEditorSide Side(*Asset);
			for (const FString& Name : Spatial)
			{
				Side.Interface(*Name, true);
			}
			const FGuid Sine = Side.AddNode(TEXT("UE.Sine.Audio"), 100, 0);
			const TArray<FName> Distance = MembersWithPrefix(Side.Builder, true, TEXT("UE.Attenuation."));
			if (TestTrue(TEXT("UE.Attenuation has inputs"), !Distance.IsEmpty()))
			{
				const FMetasoundFrontendNode* Input = Metasound::Frontend::FInputNodeTemplate::CreateNode(Side.Builder, Distance[0]);
				if (TestNotNull(TEXT("attenuation input node"), Input))
				{
					const FGuid InputID = Input->GetID();
					Side.Move(InputID, -200, 0);
					Side.Connect(InputID, nullptr, Sine, TEXT("Frequency"));
				}
			}
			const TArray<FName> Channels = MembersWithPrefix(Side.Builder, false, TEXT("UE.OutputFormat.7dot1."));
			TestEqual(TEXT("7.1 has eight channels"), Channels.Num(), 8);
			for (int32 Index = 0; Index < Channels.Num(); ++Index)
			{
				const FGuid Out = Side.OutputNode(*Channels[Index].ToString());
				Side.Move(Out, 400, 120.0 * Index);
				Side.Connect(Sine, nullptr, Out, *Channels[Index].ToString());
			}
		}
		if (!Save(*this, Asset, Surround.AssetName))
		{
			return false;
		}
	}
	// A patch with stereo input and output format interfaces, each input channel wired to its output channel.
	{
		UObject* Asset = CreateByFactory(*this, false, nullptr, StereoPatch);
		if (Asset == nullptr)
		{
			return false;
		}
		{
			FEditorSide Side(*Asset);
			Side.Interface(TEXT("UE.InputFormat.Stereo"), true);
			Side.Interface(TEXT("UE.OutputFormat.Stereo"), true);
			const TArray<FName> Ins = MembersWithPrefix(Side.Builder, true, TEXT("UE.InputFormat.Stereo."));
			const TArray<FName> Outs = MembersWithPrefix(Side.Builder, false, TEXT("UE.OutputFormat.Stereo."));
			if (TestEqual(TEXT("stereo input channels"), Ins.Num(), 2) && TestEqual(TEXT("stereo output channels"), Outs.Num(), 2))
			{
				for (int32 Index = 0; Index < 2; ++Index)
				{
					const FMetasoundFrontendNode* Input = Metasound::Frontend::FInputNodeTemplate::CreateNode(Side.Builder, Ins[Index]);
					if (!TestNotNull(TEXT("input channel node"), Input))
					{
						continue;
					}
					const FGuid InputID = Input->GetID();
					const FGuid Out = Side.OutputNode(*Outs[Index].ToString());
					Side.Move(InputID, -200, 120.0 * Index);
					Side.Move(Out, 200, 120.0 * Index);
					Side.Connect(InputID, nullptr, Out, *Outs[Index].ToString());
				}
			}
		}
		if (!Save(*this, Asset, StereoPatch.AssetName))
		{
			return false;
		}
	}

	struct FCase
	{
		const FTarget* Fixture;
		const TCHAR* Name;
		TArray<FString> Interfaces;
		const TCHAR* OutputFormat;
	};
	TArray<FString> SurroundInterfaces = Spatial;
	SurroundInterfaces.Add(TEXT("UE.Source.OneShot"));
	SurroundInterfaces.Sort();
	const TArray<FCase> Cases = {
		{&Surround, TEXT("7.1 source, spatial interfaces"), SurroundInterfaces, TEXT("SevenDotOne")},
		{&StereoPatch, TEXT("stereo-format patch"), {TEXT("UE.InputFormat.Stereo"), TEXT("UE.OutputFormat.Stereo")}, nullptr},
	};
	for (const FCase& Case : Cases)
	{
		const double Start = FPlatformTime::Seconds();
		const FAssetDocumentResult Original = ExtractResult(Case.Fixture->ObjectPath);
		const double ExtractMs = Milliseconds(Start);
		if (!TestTrue(*FString::Printf(TEXT("%s extracts"), Case.Name), Original.IsSuccess()))
		{
			AddInfo(DiagnosticsText(Original));
			continue;
		}
		TArray<FString> Interfaces;
		BodyOf(Original.Payload)->TryGetStringArrayField(TEXT("Interfaces"), Interfaces);
		Interfaces.Sort();
		TestEqual(*FString::Printf(TEXT("%s: Interfaces"), Case.Name), FString::Join(Interfaces, TEXT(",")), FString::Join(Case.Interfaces, TEXT(",")));
		if (Case.OutputFormat != nullptr)
		{
			TestEqual(*FString::Printf(TEXT("%s: OutputFormat"), Case.Name), Child(Original.Payload, TEXT("Properties"))->GetStringField(TEXT("OutputFormat")),
				FString(Case.OutputFormat));
		}
		const FTarget& Created = Fixtures.Add(TEXT("MS_FormatNew"));
		const FRoundTrip Trip = RoundTrip(*this, Original.Payload, StatusOf(Original), Created, Case.Name);
		if (!TestTrue(*FString::Printf(TEXT("%s: Create"), Case.Name), Trip.Applied.IsSuccess()))
		{
			AddError(DiagnosticsText(Trip.Applied));
			continue;
		}
		for (const FString& Problem : Trip.Problems)
		{
			AddError(FString::Printf(TEXT("%s: %s"), Case.Name, *Problem));
		}
		AddInfo(FString::Printf(TEXT("%s %s | extract %.0f ms, %s | %s"), Trip.Problems.IsEmpty() ? TEXT("PASS") : TEXT("FAIL"), Case.Name, ExtractMs,
			*Trip.Timings(), *Trip.Status.Left(120)));
	}
	return true;
}
#endif // WITH_DEV_AUTOMATION_TESTS
