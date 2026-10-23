// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// End-to-end tests: Niagara emitter assets (profile.niagara_emitter.v1) and parameter collections
// (profile.niagara_parameter_collection.v1), through the production Service as the MCP tools send documents.
//   UEAssetDoc.Niagara.EmitterAsset.Apply        a Fountain copy edited by a document: the emitter
//        editor's working copy is written back over the same object; re-apply is a no-op, save + reload is stable,
//        no undo history references the asset afterwards; a new compile error rolls the asset back; scratch pads are added and deleted through the emitter's own panel.
//   UEAssetDoc.Niagara.EmitterAsset.Inheritance  a child emitter asset created from a Parent (the
//        New Niagara Emitter dialog's inheritance), a system using it; an edit of the parent merges into the
//        child and the system (Niagara.DependentsUpdated); a dirty dependent refuses before any change; Versions:
//        a new exposed version takes the edits, editing the exposed version needs EditExposedVersion.
//   UEAssetDoc.Niagara.EmitterAsset.FreeEdits    the same edits made through the editor's API on one
//        duplicate and through a document on another give the same document.
//   UEAssetDoc.Niagara.EmitterAsset.SummaryView  an emitter whose editor shows the Summary View (the
//        stack then lists only the Emitter Summary group) reads, edits and re-applies as with the summary off, in an
//        emitter asset and in a system; the toggle is unchanged. SandEmitter (NiagaraFluids; summary on) extracts its modules.
//   UEAssetDoc.Niagara.Collection.Apply          a collection's parameters added, renamed, retyped,
//        valued and deleted as the collection editor does; refusals change nothing.
//   UEAssetDoc.Niagara.Collection.VRTemplate     NPC_VRTemplate (Templates content) round trips into
//        a new collection, re-apply is a no-op, and a system links one of its parameters.

#include "AssetDocumentNiagaraTestEditing.h"

#include "Runtime/AssetDocumentUndoHistory.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonValue.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterEditorData.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraParameterCollection.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraSystem.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentNiagaraTests
{
namespace
{
const TCHAR* const VRTemplateCollection = TEXT("Templates/TemplateResources/Standard/XRFramework/Content/VFX/NPC_VRTemplate.uasset");

TSharedPtr<FJsonObject> BodyOf(const TSharedPtr<FJsonObject>& Document)
{
	return Child(Document, TEXT("Body"));
}

TSharedPtr<FJsonObject> PropertiesOf(const TSharedPtr<FJsonObject>& Document)
{
	const TSharedPtr<FJsonObject> Body = BodyOf(Document);
	if (!Body->HasTypedField<EJson::Object>(TEXT("Properties")))
	{
		Body->SetObjectField(TEXT("Properties"), MakeShared<FJsonObject>());
	}
	return Child(Body, TEXT("Properties"));
}

UNiagaraEmitter* EmitterOf(const FTarget& Target)
{
	return Cast<UNiagaraEmitter>(Target.Get());
}

bool LocalSpace(const UNiagaraEmitter* Emitter)
{
	const FVersionedNiagaraEmitterData* Data = Emitter != nullptr ? Emitter->GetLatestEmitterData() : nullptr;
	return Data != nullptr && Data->bLocalSpace;
}

void SaveTargetAsset(const FTarget& Target)
{
	if (UObject* Asset = Target.Get())
	{
		SaveAsset(*Asset, Target.PackageFile);
	}
}

// A float input with a plain value in one of the document's particle stacks: {module, input} (the expression
// rollback case writes an expression with an undefined symbol there).
bool FindFloatInput(const TSharedPtr<FJsonObject>& Body, TSharedPtr<FJsonObject>& OutInputs, FString& OutInput)
{
	for (const TCHAR* Group : {TEXT("ParticleSpawn"), TEXT("ParticleUpdate")})
	{
		for (const TSharedPtr<FJsonValue>& Module : List(Body, Group))
		{
			const TSharedPtr<FJsonObject> Inputs = Child(Module->AsObject(), TEXT("Inputs"));
			for (const auto& Input : Inputs->Values)
			{
				if (Input.Value.IsValid() && Input.Value->Type == EJson::Number)
				{
					OutInputs = Inputs;
					OutInput = Input.Key;
					return true;
				}
			}
		}
	}
	return false;
}

void LogDiagnostics(FAutomationTestBase& Test, const FString& Stage, const FAssetDocumentResult& Result)
{
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Test.AddInfo(FString::Printf(TEXT("%s: [%s] %s %s"), *Stage, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message.Left(600)));
		UE_LOG(LogTemp, Display, TEXT("AssetDocumentNiagaraTests %s diagnostic: [%s] %s %s"), *Stage, *Diagnostic.Code, *Diagnostic.Path,
			*Diagnostic.Message.Left(600));
	}
}

// A Create document of an emitter asset.
TSharedPtr<FJsonObject> EmitterDocument(const FTarget& Target, const FString& BodyJson)
{
	return Parse(FString::Printf(TEXT(R"json({"SchemaVersion": 1, "Target": "%s", "Class": "/Script/Niagara.NiagaraEmitter",
		"Action": "Create", "Definitions": {}, "Properties": {}, "Body": %s})json"), *Target.PackageName, *BodyJson));
}

TSharedPtr<FJsonObject> CollectionDocument(const FTarget& Target, const FString& BodyJson, const TCHAR* Action = TEXT("Create"))
{
	return Parse(FString::Printf(TEXT(R"json({"SchemaVersion": 1, "Target": "%s", "Class": "/Script/Niagara.NiagaraParameterCollection",
		"Action": "%s", "Definitions": {}, "Properties": {}, "Body": %s})json"), *Target.PackageName, Action, *BodyJson));
}

// The emitter editor data of an emitter asset, or of a system's first emitter.
UNiagaraEmitterEditorData* EditorDataOf(const FVersionedNiagaraEmitterData* Data)
{
	return Data != nullptr ? Cast<UNiagaraEmitterEditorData>(Data->GetEditorData()) : nullptr;
}

UNiagaraEmitterEditorData* SystemEmitterEditorData(const FTarget& Target)
{
	const UNiagaraSystem* System = Cast<UNiagaraSystem>(Target.Get());
	return System != nullptr && System->GetEmitterHandles().Num() > 0 ? EditorDataOf(System->GetEmitterHandles()[0].GetEmitterData()) : nullptr;
}

int32 StackModules(const TSharedPtr<FJsonObject>& Body)
{
	int32 Modules = 0;
	for (const TCHAR* Group : {TEXT("EmitterSpawn"), TEXT("EmitterUpdate"), TEXT("ParticleSpawn"), TEXT("ParticleUpdate")})
	{
		Modules += List(Body, Group).Num();
	}
	return Modules;
}

// The parameter entry of a collection document.
TSharedPtr<FJsonObject> CollectionParameter(const TSharedPtr<FJsonObject>& Document, const FString& Name)
{
	return Named(List(BodyOf(Document), TEXT("Parameters")), Name);
}
} // namespace
} // namespace AssetDocumentNiagaraTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraEmitterAssetApplyTest, "UEAssetDoc.Niagara.EmitterAsset.Apply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraEmitterAssetApplyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraTests;
	ExpectVectorVMCompileErrors(*this, TEXT("AssetDocumentUndefinedSymbol"));
	UObject* Source = LoadObject<UObject>(nullptr, FountainEmitter);
	if (!TestNotNull(TEXT("Fountain loads"), Source))
	{
		return false;
	}
	const FTarget Copy(TEXT("NE_Apply"));
	ON_SCOPE_EXIT
	{
		Copy.Cleanup();
	};
	if (!TestNotNull(TEXT("copy"), DuplicateTo(*Source, Copy)))
	{
		return false;
	}
	UNiagaraEmitter* Before = EmitterOf(Copy);
	const TSharedPtr<FJsonObject> Extracted = Extract(*this, Copy, TEXT("extract"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("NiagaraExample emitter %s"), *Serialize(Extracted).Left(6000)));
	const bool bWasLocal = LocalSpace(Before);

	// 1. A Details edit: written back over the same object, saved, re-apply is a no-op, reload is stable, no undo
	//    history references the asset.
	{
		const TSharedPtr<FJsonObject> Document = Retargeted(Extracted, Copy, TEXT("Update"));
		PropertiesOf(Document)->SetBoolField(TEXT("bLocalSpace"), !bWasLocal);
		const double Start = FPlatformTime::Seconds();
		const FAssetDocumentResult Applied = ApplyDocument(*this, Copy, Document, TEXT("details"));
		AddInfo(FString::Printf(TEXT("NiagaraTiming emitter asset apply=%.1fms"), Milliseconds(Start)));
		TestTrue(TEXT("the emitter applies"), Applied.IsSuccess());
		TestTrue(TEXT("the working copy is written back over the same object"), EmitterOf(Copy) == Before);
		TestEqual(TEXT("the asset holds the new value"), LocalSpace(EmitterOf(Copy)), !bWasLocal);
		TestFalse(TEXT("after an apply: no undo history references the emitter"), UE::AssetDocument::UndoHistory::ReferencesPackage(Copy.PackageName));
		TestNoOpReapply(*this, Copy, TEXT("emitter details"));
		TestReload(*this, Copy, TEXT("emitter details"));
	}
	const TSharedPtr<FJsonObject> Baseline = Extract(*this, Copy, TEXT("baseline"));
	if (!Baseline.IsValid())
	{
		return false;
	}
	const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Copy.PackageFile);
	auto Unchanged = [this, &Copy, &Baseline, &Stamp](const FString& Stage)
	{
		TestTextEqual(*this, FString::Printf(TEXT("%s: the emitter is unchanged"), *Stage), Serialize(Extract(*this, Copy, Stage)), Serialize(Baseline));
		TestEqual(*FString::Printf(TEXT("%s: package file untouched"), *Stage), IFileManager::Get().GetTimeStamp(*Copy.PackageFile), Stamp);
		const UObject* Live = Copy.Get();
		TestFalse(*FString::Printf(TEXT("%s: package clean"), *Stage), Live == nullptr || Live->GetOutermost()->IsDirty());
	};
	// 2. A Details edit and an expression that does not compile: the write-back is rolled back.
	{
		const TSharedPtr<FJsonObject> Broken = Retargeted(Baseline, Copy, TEXT("Update"));
		PropertiesOf(Broken)->SetBoolField(TEXT("bLocalSpace"), bWasLocal);
		TSharedPtr<FJsonObject> Inputs;
		FString Input;
		if (TestTrue(TEXT("Fountain has a float input"), FindFloatInput(BodyOf(Broken), Inputs, Input)))
		{
			Inputs->SetObjectField(Input, Parse(TEXT(R"json({"Expression": "AssetDocumentUndefinedSymbol * 2.0"})json")));
			AddInfo(FString::Printf(TEXT("expression on input %s"), *Input));
			const FAssetDocumentResult Failed = ApplyDocument(*this, Copy, Broken, TEXT("compile error"));
			LogDiagnostics(*this, TEXT("compile error"), Failed);
			TestTrue(TEXT("a new compile error fails the apply"), !Failed.IsSuccess() && HasDiagnostic(Failed, TEXT("Niagara.CompileFailed")));
			Unchanged(TEXT("after compile failure"));
		}
	}
	// 3. Refusals before any change: a lightweight body, a parent set on an existing emitter.
	{
		const TSharedPtr<FJsonObject> Stateless = Retargeted(Baseline, Copy, TEXT("Update"));
		BodyOf(Stateless)->SetObjectField(TEXT("Stateless"), MakeShared<FJsonObject>());
		const FAssetDocumentResult Refused = ApplyDocument(*this, Copy, Stateless, TEXT("stateless"));
		TestTrue(TEXT("a lightweight emitter asset is refused"), !Refused.IsSuccess() && HasDiagnostic(Refused, TEXT("Niagara.StatelessEmitter")));
		const TSharedPtr<FJsonObject> Parent = Retargeted(Baseline, Copy, TEXT("Update"));
		BodyOf(Parent)->SetStringField(TEXT("Parent"), FountainEmitter);
		const FAssetDocumentResult ParentRefused = ApplyDocument(*this, Copy, Parent, TEXT("parent"));
		TestTrue(TEXT("a parent cannot be set on an existing emitter"), !ParentRefused.IsSuccess() && HasDiagnostic(ParentRefused, TEXT("Niagara.ParentImmutable")));
		Unchanged(TEXT("after refusals"));
	}
	// 4. Scratch pads: the emitter editor's Scratch Pad panel adds and deletes the emitter's own pads.
	{
		const TSharedPtr<FJsonObject> Added = Retargeted(Baseline, Copy, TEXT("Update"));
		TArray<TSharedPtr<FJsonValue>> Pads = List(BodyOf(Added), TEXT("ScratchPads"));
		Pads.Add(MakeShared<FJsonValueObject>(Parse(TEXT(R"json({"Name": "Swirl", "Usage": "Module"})json"))));
		BodyOf(Added)->SetArrayField(TEXT("ScratchPads"), Pads);
		const FAssetDocumentResult Applied = ApplyDocument(*this, Copy, Added, TEXT("add scratch pad"));
		TestTrue(TEXT("an emitter scratch pad is added"), Applied.IsSuccess());
		const TSharedPtr<FJsonObject> WithPad = Extract(*this, Copy, TEXT("with pad"));
		TestTrue(TEXT("the pad is listed"), Named(List(BodyOf(WithPad), TEXT("ScratchPads")), TEXT("Swirl")).IsValid());
		TestTrue(TEXT("the pad lives in the emitter asset"), EmitterOf(Copy) != nullptr && EmitterOf(Copy)->GetLatestEmitterData()->ScratchPads != nullptr
			&& EmitterOf(Copy)->GetLatestEmitterData()->ScratchPads->Scripts.Num() == Pads.Num());
		TestNoOpReapply(*this, Copy, TEXT("emitter scratch pad"));
		TestReload(*this, Copy, TEXT("emitter scratch pad"));
		const TSharedPtr<FJsonObject> Without = Retargeted(Baseline, Copy, TEXT("Update"));
		Pads.Pop();
		BodyOf(Without)->SetArrayField(TEXT("ScratchPads"), Pads);
		const FAssetDocumentResult Deleted = ApplyDocument(*this, Copy, Without, TEXT("delete scratch pad"));
		TestTrue(TEXT("the pad is deleted"), Deleted.IsSuccess());
		TestTextEqual(*this, TEXT("back to the baseline"), Content(Extract(*this, Copy, TEXT("after delete"))), Content(Baseline));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraEmitterAssetSummaryViewTest, "UEAssetDoc.Niagara.EmitterAsset.SummaryView",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraEmitterAssetSummaryViewTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraTests;
	UObject* Fountain = LoadObject<UObject>(nullptr, FountainEmitter);
	UObject* Burst = LoadObject<UObject>(nullptr, DirectionalBurstSystem);
	if (!TestNotNull(TEXT("Fountain loads"), Fountain) || !TestNotNull(TEXT("DirectionalBurst loads"), Burst))
	{
		return false;
	}
	const FTarget Emitter(TEXT("NE_Summary"));
	const FTarget System(TEXT("NS_Summary"));
	ON_SCOPE_EXIT
	{
		Emitter.Cleanup();
		System.Cleanup();
	};

	// An emitter asset: the summary on, read and edited as with the summary off.
	if (!TestNotNull(TEXT("emitter copy"), DuplicateTo(*Fountain, Emitter)))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Full = Extract(*this, Emitter, TEXT("summary off"));
	UNiagaraEmitterEditorData* EditorData = EditorDataOf(EmitterOf(Emitter) != nullptr ? EmitterOf(Emitter)->GetLatestEmitterData() : nullptr);
	if (!Full.IsValid() || !TestNotNull(TEXT("emitter editor data"), EditorData))
	{
		return false;
	}
	TestTrue(TEXT("Fountain's stacks have modules"), StackModules(BodyOf(Full)) > 0);
	EditorData->SetShowSummaryView(true);
	SaveTargetAsset(Emitter);
	TestTextEqual(*this, TEXT("emitter asset: the summary view reads the full stacks"), Content(Extract(*this, Emitter, TEXT("summary on"))), Content(Full));
	TestTrue(TEXT("emitter asset: a read keeps the summary view on"), EditorData->ShouldShowSummaryView());
	TestFalse(TEXT("emitter asset: a read leaves the package clean"), Emitter.Get()->GetOutermost()->IsDirty());
	TestNoOpReapply(*this, Emitter, TEXT("emitter asset with summary view"));
	{
		const TSharedPtr<FJsonObject> Changed = Retargeted(Full, Emitter, TEXT("Update"));
		TSharedPtr<FJsonObject> Inputs;
		FString Input;
		if (TestTrue(TEXT("Fountain has a float input"), FindFloatInput(BodyOf(Changed), Inputs, Input)))
		{
			const double Value = Inputs->GetNumberField(Input) + 0.5;
			Inputs->SetNumberField(Input, Value);
			const FAssetDocumentResult Applied = ApplyDocument(*this, Emitter, Changed, TEXT("summary on edit"));
			TestTrue(TEXT("emitter asset: an input edit applies with the summary view on"), Applied.IsSuccess());
			TestTextEqual(*this, TEXT("emitter asset: the edit is the document"), Content(Clone(Extract(*this, Emitter, TEXT("after edit")))),
				Content(Clone(Changed)));
			const UNiagaraEmitter* After = EmitterOf(Emitter);
			const UNiagaraEmitterEditorData* AfterData = EditorDataOf(After != nullptr ? After->GetLatestEmitterData() : nullptr);
			TestTrue(TEXT("emitter asset: an edit keeps the summary view on"), AfterData != nullptr && AfterData->ShouldShowSummaryView());
		}
	}

	// A system whose emitter shows the summary.
	if (!TestNotNull(TEXT("system copy"), DuplicateTo(*Burst, System)))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> SystemFull = Extract(*this, System, TEXT("system summary off"));
	UNiagaraEmitterEditorData* SystemEditorData = SystemEmitterEditorData(System);
	if (!SystemFull.IsValid() || !TestNotNull(TEXT("system emitter editor data"), SystemEditorData))
	{
		return false;
	}
	SystemEditorData->SetShowSummaryView(true);
	SaveTargetAsset(System);
	TestTextEqual(*this, TEXT("system: the summary view reads the full stacks"), Content(Extract(*this, System, TEXT("system summary on"))),
		Content(SystemFull));
	TestNoOpReapply(*this, System, TEXT("system with summary view"));
	TestTrue(TEXT("system: read and re-apply keep the summary view on"), SystemEmitterEditorData(System) != nullptr
		&& SystemEmitterEditorData(System)->ShouldShowSummaryView());

	// SandEmitter's editor data shows the summary: its extract had empty module stacks and simulation stages, so a copy
	// made from it missed Emitter.NeighborGrid (Niagara.CompileFailed). Its document now has the modules; a create from
	// it meets what the editor's "+" menu can make: its module usage filter (one module sits in a stack whose menu no
	// longer offers it) or the name a new module gets (a module keeps a name its script no longer has).
	const FString Sand = TEXT("/NiagaraFluids/Templates/Sand/Emitters/SandEmitter.SandEmitter");
	if (FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Sand)))
	{
		const FTarget SandCopy(TEXT("NE_SummarySand"));
		ON_SCOPE_EXIT
		{
			SandCopy.Cleanup();
		};
		UObject* SandSource = LoadObject<UObject>(nullptr, *Sand);
		const UNiagaraEmitter* SandEmitter = Cast<UNiagaraEmitter>(SandSource);
		const UNiagaraEmitterEditorData* SandData = EditorDataOf(SandEmitter != nullptr ? SandEmitter->GetLatestEmitterData() : nullptr);
		TestTrue(TEXT("SandEmitter shows the summary view"), SandData != nullptr && SandData->ShouldShowSummaryView());
		const FAssetDocumentResult Read = ExtractResult(Sand);
		const TSharedPtr<FJsonObject> SandDocument = Read.IsSuccess() ? Read.Payload : nullptr;
		if (TestTrue(TEXT("SandEmitter extracts"), SandDocument.IsValid()))
		{
			TestTrue(TEXT("SandEmitter: the extract has its modules"), StackModules(BodyOf(SandDocument)) > 0);
			int32 StageModules = 0;
			for (const TSharedPtr<FJsonValue>& Stage : List(BodyOf(SandDocument), TEXT("SimulationStages")))
			{
				StageModules += List(Stage->AsObject(), TEXT("Modules")).Num();
			}
			TestTrue(TEXT("SandEmitter: the extract has its simulation stage modules"), StageModules > 0);
			FString Text = Serialize(SandDocument);
			Text.ReplaceInline(*FString(Sand), *SandCopy.ObjectPath, ESearchCase::CaseSensitive);
			Text.ReplaceInline(*FPackageName::ObjectPathToPackageName(Sand), *SandCopy.PackageName, ESearchCase::CaseSensitive);
			const TSharedPtr<FJsonObject> Create = Retargeted(Parse(Text), SandCopy, TEXT("Create"));
			const FAssetDocumentResult Created = ApplyDocument(*this, SandCopy, Create, TEXT("SandEmitter copy"));
			LogDiagnostics(*this, TEXT("SandEmitter copy"), Created);
			TestFalse(TEXT("SandEmitter: the copy is not refused for a compile error"), HasDiagnostic(Created, TEXT("Niagara.CompileFailed")));
			TestTrue(TEXT("SandEmitter: the copy is created or meets what the stack's \"+\" menu makes"),
				Created.IsSuccess() || HasDiagnostic(Created, TEXT("Niagara.ModuleUsage")) || HasDiagnostic(Created, TEXT("Niagara.InvalidModuleName")));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraEmitterAssetInheritanceTest, "UEAssetDoc.Niagara.EmitterAsset.Inheritance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraEmitterAssetInheritanceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraTests;
	UObject* Source = LoadObject<UObject>(nullptr, FountainEmitter);
	if (!TestNotNull(TEXT("Fountain loads"), Source))
	{
		return false;
	}
	const FTarget Parent(TEXT("NE_Parent"));
	const FTarget ChildTarget(TEXT("NE_Child"));
	const FTarget Copied(TEXT("NE_Copied"));
	const FTarget User(TEXT("NS_ChildUser"));
	ON_SCOPE_EXIT
	{
		User.Cleanup();
		Copied.Cleanup();
		ChildTarget.Cleanup();
		Parent.Cleanup();
	};
	if (!TestNotNull(TEXT("parent copy"), DuplicateTo(*Source, Parent)))
	{
		return false;
	}
	// 0. The template copy is not inheritable: its Asset Options (top-level Properties) make it so.
	{
		const TSharedPtr<FJsonObject> Options = Retargeted(Extract(*this, Parent, TEXT("parent options")), Parent, TEXT("Update"));
		TestFalse(TEXT("the template copy is not inheritable"), Child(Options, TEXT("Properties"))->GetBoolField(TEXT("bIsInheritable")));
		Child(Options, TEXT("Properties"))->SetBoolField(TEXT("bIsInheritable"), true);
		TestTrue(TEXT("Asset Options apply"), ApplyDocument(*this, Parent, Options, TEXT("inheritable")).IsSuccess());
		TestTrue(TEXT("the parent is inheritable"), EmitterOf(Parent) != nullptr && EmitterOf(Parent)->bIsInheritable);
		TestNoOpReapply(*this, Parent, TEXT("inheritable"));
	}
	// 1. A new emitter asset that inherits from the parent (New Niagara Emitter > from an existing emitter, with
	//    inheritance), and one copied From it (no inheritance).
	const FAssetDocumentResult Created = ApplyDocument(*this, ChildTarget,
		EmitterDocument(ChildTarget, FString::Printf(TEXT(R"json({"Parent": "%s"})json"), *Parent.PackageName)), TEXT("child"));
	if (!TestTrue(TEXT("a child emitter asset is created"), Created.IsSuccess()))
	{
		return false;
	}
	const UNiagaraEmitter* ChildEmitter = EmitterOf(ChildTarget);
	TestTrue(TEXT("the child's parent is the parent asset"), ChildEmitter != nullptr && ChildEmitter->GetLatestEmitterData() != nullptr
		&& ChildEmitter->GetLatestEmitterData()->GetParent().Emitter == EmitterOf(Parent));
	const TSharedPtr<FJsonObject> ChildDocument = Extract(*this, ChildTarget, TEXT("child"));
	TestTrue(TEXT("the child's document names its parent"), Serialize(BodyOf(ChildDocument)).Contains(Parent.PackageName));
	TestNoOpReapply(*this, ChildTarget, TEXT("child"));
	const FAssetDocumentResult CopyCreated = ApplyDocument(*this, Copied,
		EmitterDocument(Copied, FString::Printf(TEXT(R"json({"From": "%s"})json"), *Parent.PackageName)), TEXT("copied"));
	TestTrue(TEXT("a copied emitter asset is created"), CopyCreated.IsSuccess());
	UNiagaraEmitter* CopiedEmitter = EmitterOf(Copied);
	TestTrue(TEXT("the copy has no parent"), CopiedEmitter != nullptr && CopiedEmitter->GetLatestEmitterData() != nullptr
		&& CopiedEmitter->GetLatestEmitterData()->GetParent().Emitter == nullptr);
	TestTextEqual(*this, TEXT("the copy reads as the parent"), Serialize(BodyOf(Extract(*this, Copied, TEXT("copied")))),
		Serialize(BodyOf(Extract(*this, Parent, TEXT("parent")))));
	// 2. A system using the child.
	const TSharedPtr<FJsonObject> System = Parse(FString::Printf(TEXT(R"json({"SchemaVersion": 1, "Target": "%s",
		"Class": "/Script/Niagara.NiagaraSystem", "Action": "Create", "Definitions": {}, "Properties": {},
		"Body": {"Emitters": [{"Name": "Child", "Parent": "%s"}]}})json"), *User.PackageName, *ChildTarget.PackageName));
	if (!TestTrue(TEXT("a system uses the child"), ApplyDocument(*this, User, System, TEXT("user system")).IsSuccess()))
	{
		return false;
	}
	auto SystemLocalSpace = [&User]()
	{
		const UNiagaraSystem* UserSystem = Cast<UNiagaraSystem>(User.Get());
		const FVersionedNiagaraEmitterData* Data = UserSystem != nullptr && UserSystem->GetEmitterHandles().Num() == 1
			? UserSystem->GetEmitterHandles()[0].GetEmitterData() : nullptr;
		return Data != nullptr && Data->bLocalSpace;
	};
	const bool bWasLocal = LocalSpace(EmitterOf(Parent));
	const TSharedPtr<FJsonObject> ParentDocument = Extract(*this, Parent, TEXT("parent"));
	const TSharedPtr<FJsonObject> Edit = Retargeted(ParentDocument, Parent, TEXT("Update"));
	PropertiesOf(Edit)->SetBoolField(TEXT("bLocalSpace"), !bWasLocal);
	// 3. A dependent with unsaved changes refuses before any change.
	if (UObject* UserAsset = User.Get())
	{
		UserAsset->GetOutermost()->MarkPackageDirty();
	}
	const FAssetDocumentResult Refused = ApplyDocument(*this, Parent, Edit, TEXT("dirty dependent"));
	TestTrue(TEXT("a dependent with unsaved changes refuses"), !Refused.IsSuccess() && HasDiagnostic(Refused, TEXT("Niagara.DependentHasUnsavedChanges")));
	TestTextEqual(*this, TEXT("the refusal changes nothing"), Content(Extract(*this, Parent, TEXT("after refusal"))), Content(ParentDocument));
	SaveTargetAsset(User);
	// 4. The parent's edit merges into the child and the system that inherit from it.
	const double Start = FPlatformTime::Seconds();
	const FAssetDocumentResult Applied = ApplyDocument(*this, Parent, Edit, TEXT("parent edit"));
	AddInfo(FString::Printf(TEXT("NiagaraTiming emitter asset apply with dependents=%.1fms"), Milliseconds(Start)));
	TestTrue(TEXT("the parent applies and refreshes its dependents"), Applied.IsSuccess() && HasDiagnostic(Applied, TEXT("Niagara.DependentsUpdated")));
	TestEqual(TEXT("the child follows its parent"), LocalSpace(EmitterOf(ChildTarget)), !bWasLocal);
	TestEqual(TEXT("the system's emitter follows"), SystemLocalSpace(), !bWasLocal);
	TestEqual(TEXT("the copy does not"), LocalSpace(EmitterOf(Copied)), bWasLocal);
	SaveTargetAsset(ChildTarget);
	SaveTargetAsset(User);
	// 5. Versions: a new exposed version takes the edits; the old version keeps its data.
	{
		const TSharedPtr<FJsonObject> Versioned = Extract(*this, Parent, TEXT("before versions"));
		const TSharedPtr<FJsonObject> Document = Retargeted(Versioned, Parent, TEXT("Update"));
		BodyOf(Document)->SetArrayField(TEXT("Versions"), List(Parse(TEXT(R"json({"V": [{"Number": "1.0"},
			{"Number": "1.1", "Exposed": true, "ChangeDescription": "Local space off"}]})json")), TEXT("V")));
		PropertiesOf(Document)->SetBoolField(TEXT("bLocalSpace"), bWasLocal);
		// The new version's renderers are copies under new object names (AddEmitterVersion): kept, not listed.
		BodyOf(Document)->RemoveField(TEXT("Renderers"));
		const FAssetDocumentResult NewVersion = ApplyDocument(*this, Parent, Document, TEXT("new version"));
		TestTrue(TEXT("a new exposed version applies"), NewVersion.IsSuccess());
		UNiagaraEmitter* ParentEmitter = EmitterOf(Parent);
		if (TestTrue(TEXT("the parent has two versions"), ParentEmitter != nullptr && ParentEmitter->GetAllAvailableVersions().Num() == 2))
		{
			const FNiagaraAssetVersion Exposed = ParentEmitter->GetExposedVersion();
			TestTrue(TEXT("1.1 is exposed"), Exposed.MajorVersion == 1 && Exposed.MinorVersion == 1);
			const FVersionedNiagaraEmitterData* New = ParentEmitter->GetEmitterData(Exposed.VersionGuid);
			const FVersionedNiagaraEmitterData* Old = ParentEmitter->GetEmitterData(ParentEmitter->GetAllAvailableVersions()[0].VersionGuid);
			TestTrue(TEXT("1.1 holds the edit"), New != nullptr && New->bLocalSpace == bWasLocal);
			TestTrue(TEXT("1.0 keeps its data"), Old != nullptr && Old->bLocalSpace == !bWasLocal);
		}
		TestNoOpReapply(*this, Parent, TEXT("versioned parent"));
		SaveTargetAsset(ChildTarget);
		SaveTargetAsset(User);
		const TSharedPtr<FJsonObject> Exposed = Retargeted(Extract(*this, Parent, TEXT("versioned")), Parent, TEXT("Update"));
		PropertiesOf(Exposed)->SetBoolField(TEXT("bLocalSpace"), !bWasLocal);
		const FAssetDocumentResult ExposedRefused = ApplyDocument(*this, Parent, Exposed, TEXT("exposed version"));
		TestTrue(TEXT("editing the exposed version needs EditExposedVersion"), !ExposedRefused.IsSuccess()
			&& HasDiagnostic(ExposedRefused, TEXT("Niagara.ExposedVersionEdit")));
		BodyOf(Exposed)->SetBoolField(TEXT("EditExposedVersion"), true);
		TestTrue(TEXT("EditExposedVersion edits it"), ApplyDocument(*this, Parent, Exposed, TEXT("opt in")).IsSuccess());
		TestReload(*this, Parent, TEXT("versioned parent"));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraEmitterAssetFreeEditsTest, "UEAssetDoc.Niagara.EmitterAsset.FreeEdits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraEmitterAssetFreeEditsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraTests;
	UObject* Source = LoadObject<UObject>(nullptr, FountainEmitter);
	if (!TestNotNull(TEXT("Fountain loads"), Source))
	{
		return false;
	}
	const FTarget Editor(TEXT("NE_FreeEditor"));
	const FTarget Document(TEXT("NE_FreeDocument"));
	ON_SCOPE_EXIT
	{
		Document.Cleanup();
		Editor.Cleanup();
	};
	if (!TestNotNull(TEXT("editor copy"), DuplicateTo(*Source, Editor)) || !TestNotNull(TEXT("document copy"), DuplicateTo(*Source, Document)))
	{
		return false;
	}
	// The editor: the emitter Details (Modify, write, PostEditChangeVersionedProperty) and a renderer's Details.
	UNiagaraEmitter* Emitter = EmitterOf(Editor);
	FVersionedNiagaraEmitterData* Data = Emitter != nullptr ? Emitter->GetLatestEmitterData() : nullptr;
	if (!TestNotNull(TEXT("emitter data"), Data) || !TestTrue(TEXT("Fountain has a renderer"), !Data->GetRenderers().IsEmpty()))
	{
		return false;
	}
	{
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentNiagaraTests", "EmitterDetails", "Emitter Details"));
		Emitter->Modify();
		FProperty* Property = FVersionedNiagaraEmitterData::StaticStruct()->FindPropertyByName(TEXT("bDeterminism"));
		Data->bDeterminism = true;
		FPropertyChangedEvent Event(Property);
		Emitter->PostEditChangeVersionedProperty(Event, Data->Version.VersionGuid);
		Property = FVersionedNiagaraEmitterData::StaticStruct()->FindPropertyByName(TEXT("RandomSeed"));
		Data->RandomSeed = 77;
		FPropertyChangedEvent SeedEvent(Property);
		Emitter->PostEditChangeVersionedProperty(SeedEvent, Data->Version.VersionGuid);
	}
	UNiagaraRendererProperties* Renderer = Data->GetRenderers()[0];
	DetailsEdit(*Renderer, TEXT("SortOrderHint"), [Renderer]() { Renderer->SortOrderHint = 3; });
	TestTrue(TEXT("editor copy saved"), SaveAsset(*Emitter, Editor.PackageFile));
	const TSharedPtr<FJsonObject> Edited = Extract(*this, Editor, TEXT("editor"));
	// The document: the editor copy's content written on the other duplicate.
	FString Text = Serialize(Edited);
	Text.ReplaceInline(*Editor.ObjectPath, *Document.ObjectPath, ESearchCase::CaseSensitive);
	Text.ReplaceInline(*Editor.PackageName, *Document.PackageName, ESearchCase::CaseSensitive);
	const double Start = FPlatformTime::Seconds();
	const FAssetDocumentResult Applied = ApplyDocument(*this, Document, Retargeted(Parse(Text), Document, TEXT("Update")), TEXT("document"));
	AddInfo(FString::Printf(TEXT("NiagaraTiming emitter free edits apply=%.1fms"), Milliseconds(Start)));
	TestTrue(TEXT("the document applies"), Applied.IsSuccess());
	FString Result = Serialize(Extract(*this, Document, TEXT("document result")));
	Result.ReplaceInline(*Document.ObjectPath, *Editor.ObjectPath, ESearchCase::CaseSensitive);
	Result.ReplaceInline(*Document.PackageName, *Editor.PackageName, ESearchCase::CaseSensitive);
	TestTextEqual(*this, TEXT("document edits == editor edits"), Content(Parse(Result)), Content(Clone(Edited)));
	TestNoOpReapply(*this, Document, TEXT("free edits"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraCollectionApplyTest, "UEAssetDoc.Niagara.Collection.Apply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraCollectionApplyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraTests;
	const FTarget Target(TEXT("NPC_Apply"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
	};
	// 1. Create: parameters with values (a data interface's Properties sparse).
	const FAssetDocumentResult Created = ApplyDocument(*this, Target, CollectionDocument(Target, TEXT(R"json({"Parameters": [
		{"Name": "Speed", "Type": "float", "Value": 2.5},
		{"Name": "Tint", "Type": "LinearColor", "Value": {"R": 1.0, "G": 0.5, "B": 0.25, "A": 1.0}},
		{"Name": "Count", "Type": "int32"},
		{"Name": "Curve", "Type": "/Script/Niagara.NiagaraDataInterfaceCurve"}]})json")), TEXT("create"));
	if (!TestTrue(TEXT("the collection is created"), Created.IsSuccess()))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Made = Extract(*this, Target, TEXT("created"));
	AddInfo(FString::Printf(TEXT("NiagaraExample collection %s"), *Serialize(Made).Left(4000)));
	TestTrue(TEXT("values as written"), Serialize(BodyOf(Made)).Contains(TEXT("{\"Name\":\"Speed\",\"Type\":\"float\",\"Value\":2.5}")));
	const UNiagaraParameterCollection* Collection = Cast<UNiagaraParameterCollection>(Target.Get());
	TestTrue(TEXT("four parameters"), Collection != nullptr && Collection->GetParameters().Num() == 4);
	TestNoOpReapply(*this, Target, TEXT("collection"));
	TestReload(*this, Target, TEXT("collection"));
	// 2. Rename, retype (with a value), value, delete: the editor's own operations.
	const TSharedPtr<FJsonObject> Edit = Retargeted(Extract(*this, Target, TEXT("before edit")), Target, TEXT("Update"));
	BodyOf(Edit)->SetArrayField(TEXT("Parameters"), List(Parse(TEXT(R"json({"P": [
		{"Name": "Velocity", "RenamedFrom": "Speed", "Type": "float", "Value": 4.0},
		{"Name": "Tint", "Type": "LinearColor", "Value": {"R": 0.0, "G": 1.0, "B": 0.0, "A": 1.0}},
		{"Name": "Count", "Type": "float", "Value": 7.0}]})json")), TEXT("P")));
	const FAssetDocumentResult Edited = ApplyDocument(*this, Target, Edit, TEXT("edit"));
	TestTrue(TEXT("the edits apply"), Edited.IsSuccess());
	const TSharedPtr<FJsonObject> After = Extract(*this, Target, TEXT("after edit"));
	const TSharedPtr<FJsonObject> Velocity = CollectionParameter(After, TEXT("Velocity"));
	TestTrue(TEXT("renamed with its new value"), Velocity.IsValid() && Velocity->GetNumberField(TEXT("Value")) == 4.0);
	const TSharedPtr<FJsonObject> Count = CollectionParameter(After, TEXT("Count"));
	TestTrue(TEXT("retyped with a value"), Count.IsValid() && Count->GetStringField(TEXT("Type")) == TEXT("float") && Count->GetNumberField(TEXT("Value")) == 7.0);
	TestFalse(TEXT("unlisted parameters are deleted"), CollectionParameter(After, TEXT("Curve")).IsValid() || CollectionParameter(After, TEXT("Speed")).IsValid());
	TestNoOpReapply(*this, Target, TEXT("edited collection"));
	// 3. Refusals change nothing: an unknown type, a name with the NPC prefix; a Details namespace another collection uses.
	const FString Baseline = Content(After);
	const TSharedPtr<FJsonObject> BadType = Retargeted(After, Target, TEXT("Update"));
	CollectionParameter(BadType, TEXT("Tint"))->SetStringField(TEXT("Type"), TEXT("NoSuchType"));
	TestFalse(TEXT("an unknown type is refused"), ApplyDocument(*this, Target, BadType, TEXT("bad type")).IsSuccess());
	const TSharedPtr<FJsonObject> Prefixed = Retargeted(After, Target, TEXT("Update"));
	CollectionParameter(Prefixed, TEXT("Tint"))->SetStringField(TEXT("Name"), TEXT("NPC.X.Tint"));
	const FAssetDocumentResult PrefixRefused = ApplyDocument(*this, Target, Prefixed, TEXT("prefixed"));
	TestTrue(TEXT("an NPC-prefixed name is refused"), !PrefixRefused.IsSuccess() && HasDiagnostic(PrefixRefused, TEXT("Niagara.InvalidParameterName")));
	TestTextEqual(*this, TEXT("refusals change nothing"), Content(Extract(*this, Target, TEXT("after refusals"))), Baseline);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraCollectionVRTemplateTest, "UEAssetDoc.Niagara.Collection.VRTemplate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraCollectionVRTemplateTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraTests;
	const FString SourceFile = FPaths::Combine(FPaths::RootDir(), VRTemplateCollection);
	if (!IFileManager::Get().FileExists(*SourceFile))
	{
		AddInfo(FString::Printf(TEXT("NiagaraCorpus absent in this install: %s"), *SourceFile));
		return true;
	}
	// The template's collection as a project gets it (the package copied into the content folder; its asset name stays
	// NPC_VRTemplate, so it lives in a folder of its own).
	const FString Folder = FString(TEXT("/Game/")) + FolderName + TEXT("/VR_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString PackageName = Folder + TEXT("/NPC_VRTemplate");
	const FString PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FTarget Fresh(TEXT("NPC_RT"));
	const FTarget User(TEXT("NS_NPCUser"));
	ON_SCOPE_EXIT
	{
		User.Cleanup();
		Fresh.Cleanup();
		ReleaseUndoHistory();
		if (UPackage* Package = FindPackage(nullptr, *PackageName))
		{
			AssetDocumentNiagaraTests::UnloadPackages({Package});
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		IFileManager::Get().Delete(*PackageFile, false, true);
		IFileManager::Get().Delete(*(FPaths::ChangeExtension(PackageFile, TEXT("")) + TEXT(".assetdoc.json")), false, true);
		IAssetRegistry::GetChecked().ScanModifiedAssetFiles({PackageFile});
	};
	if (!TestEqual(TEXT("template collection copied"), IFileManager::Get().Copy(*PackageFile, *SourceFile), static_cast<uint32>(COPY_OK)))
	{
		return false;
	}
	IAssetRegistry::GetChecked().ScanFilesSynchronous({PackageFile}, true);
	UNiagaraParameterCollection* Source = LoadObject<UNiagaraParameterCollection>(nullptr, *(PackageName + TEXT(".NPC_VRTemplate")));
	if (!TestNotNull(TEXT("template collection loads"), Source))
	{
		return false;
	}
	FTarget SourceTarget(TEXT("unused"));
	SourceTarget.AssetName = TEXT("NPC_VRTemplate");
	SourceTarget.PackageName = PackageName;
	SourceTarget.ObjectPath = PackageName + TEXT(".NPC_VRTemplate");
	SourceTarget.PackageFile = PackageFile;
	SourceTarget.SidecarPath = FPaths::ChangeExtension(PackageFile, TEXT("")) + TEXT(".assetdoc.json");
	const TSharedPtr<FJsonObject> Extracted = Extract(*this, SourceTarget, TEXT("VR template"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("NiagaraExample VR collection %s"), *Serialize(Extracted).Left(4000)));
	TestNoOpReapply(*this, SourceTarget, TEXT("VR template"));
	// Round trip into a new collection: the same parameters and values (its namespace is its own: two collections
	// cannot share one).
	const TSharedPtr<FJsonObject> Document = Retargeted(Extracted, Fresh, TEXT("Create"));
	Child(Document, TEXT("Properties"))->RemoveField(TEXT("Namespace"));
	double Start = FPlatformTime::Seconds();
	const FAssetDocumentResult Created = ApplyDocument(*this, Fresh, Document, TEXT("VR round trip"));
	AddInfo(FString::Printf(TEXT("NiagaraTiming NPC_VRTemplate roundtrip create=%.1fms"), Milliseconds(Start)));
	if (TestTrue(TEXT("the round trip creates the collection"), Created.IsSuccess()))
	{
		const TSharedPtr<FJsonObject> Result = Extract(*this, Fresh, TEXT("VR round trip result"));
		Child(Result, TEXT("Properties"))->RemoveField(TEXT("Namespace"));
		TestTextEqual(*this, TEXT("round trip == source"), Content(Clone(Result)), Content(Clone(Document)));
		TestNoOpReapply(*this, Fresh, TEXT("VR round trip"));
	}
	// The namespace of the template is taken: authoring it on another collection is refused.
	{
		const TSharedPtr<FJsonObject> Taken = Retargeted(Extracted, Fresh, TEXT("Update"));
		const FAssetDocumentResult Refused = ApplyDocument(*this, Fresh, Taken, TEXT("namespace in use"));
		TestTrue(TEXT("a namespace another collection uses is refused"), !Refused.IsSuccess() && HasDiagnostic(Refused, TEXT("Niagara.NamespaceInUse")));
	}
	// A system links one of the template's parameters (the stack's link menu lists NPC.<Namespace>.<name>).
	FString Linked;
	for (const TSharedPtr<FJsonValue>& Entry : List(BodyOf(Extracted), TEXT("Parameters")))
	{
		if (Linked.IsEmpty() && Entry->AsObject()->GetStringField(TEXT("Type")) == TEXT("LinearColor"))
		{
			Linked = FString::Printf(TEXT("NPC.%s.%s"), *Source->GetNamespace().ToString(), *Entry->AsObject()->GetStringField(TEXT("Name")));
		}
	}
	if (TestFalse(TEXT("the template has a color parameter"), Linked.IsEmpty()))
	{
		const TSharedPtr<FJsonObject> System = Parse(FString::Printf(TEXT(R"json({"SchemaVersion": 1, "Target": "%s",
			"Class": "/Script/Niagara.NiagaraSystem", "Action": "Create", "Definitions": {}, "Properties": {},
			"Body": {"Emitters": [{"Name": "Ring", "From": "Minimal"}]}})json"), *User.PackageName));
		if (TestTrue(TEXT("a system is made"), ApplyDocument(*this, User, System, TEXT("NPC user")).IsSuccess()))
		{
			const TSharedPtr<FJsonObject> Now = Retargeted(Extract(*this, User, TEXT("NPC user")), User, TEXT("Update"));
			const TSharedPtr<FJsonObject> Ring = Named(List(BodyOf(Now), TEXT("Emitters")), TEXT("Ring"));
			TArray<TSharedPtr<FJsonValue>> Update = List(Ring, TEXT("ParticleUpdate"));
			Update.Add(MakeShared<FJsonValueObject>(Parse(FString::Printf(TEXT(R"json({"Name": "Color",
				"Script": "/Niagara/Modules/Update/Color/Color", "Inputs": {"Color": {"Link": "%s"}}})json"), *Linked))));
			Ring->SetArrayField(TEXT("ParticleUpdate"), Update);
			const FAssetDocumentResult Linking = ApplyDocument(*this, User, Now, TEXT("NPC link"));
			TestTrue(TEXT("the system links the collection parameter"), Linking.IsSuccess());
			const FString Text = Serialize(Extract(*this, User, TEXT("NPC linked")));
			TestTrue(TEXT("the link reads back"), Text.Contains(FString::Printf(TEXT("{\"Link\":\"%s\"}"), *Linked)));
			if (UNiagaraSystem* UserSystem = Cast<UNiagaraSystem>(User.Get()))
			{
				CompileAndWait(*UserSystem);
				TestEqual(TEXT("the system compiles with the link"), CompileErrors(*UserSystem), FString());
			}
			TestNoOpReapply(*this, User, TEXT("NPC link"));
		}
	}
	return !HasAnyErrors();
}
#endif
