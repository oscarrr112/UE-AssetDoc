// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// UEAssetDoc.Niagara.Apply.*: system Apply (N2 system level, N3 stacks) through the production
// Service, the way the MCP tools send documents. Module stack free edits: AssetDocumentNiagaraStackTests.cpp.
//   EditorEquivalence  free edits made with the Niagara editor's own API on one duplicate of a template system
//                      (rename / disable / delete emitters, add an emitter from a template, emitter Details, the
//                      Render group's "+" for two renderers, renderer Details, renderer order and delete); that
//                      copy's extract (RenamedFrom / From added) applied to a second duplicate gives the same
//                      document; re-applying it is a no-op (nothing saved, package clean); Diff is empty; the saved
//                      package reloads to the same document.
//   UserParameters     the behaviour test of AssetDocumentNiagaraUserParameters.cpp (raw editor-equivalent code):
//                      add (values, description, category in the User Parameters hierarchy), a module input linked
//                      by the editor to a parameter follows its rename (the document renames the link too) and keeps
//                      its value as a local value when the parameter is deleted (the editor's delete), data
//                      interface Details values and their reset, unknown type / retype refusals change nothing.
//   Templates          every engine template system: read-only extract, re-targeted to /Game and created as a new
//                      asset with every region (emitters added from the template emitter of the same name, else
//                      empty; lightweight emitters as lightweight; stacks, inputs, handlers, stages authored); the
//                      new asset's document equals the template's; re-apply is a no-op.
//   SparseCurves       the extract of a system with a template emitter writes its curve data
//                      interfaces with only the key fields that differ from the class default (the Diff read stays
//                      whole); applied as a copy it gives the same system (Diff empty); re-apply is a no-op.
//   FromScratch        a system written from scratch (user parameters, emitters from templates, renderers): it
//                      compiles, re-apply is a no-op, it reloads to the same document.
//   Refusals           a new compile error rolls the apply back (Niagara.CompileFailed, package unchanged), an error
//                      the system had already is a warning; a Create that fails leaves no asset; a particle module in System
//                      Update, a scratch pad added to an emitter and a parent change are refused (Niagara.ModuleUsage /
//                      Niagara.ScratchPadScope / Niagara.ParentImmutable); undo history follows the
//                      AssetDocument rules (an apply clears history that references the system, a failed one
//                      keeps it).

#include "AssetDocumentNiagaraTestEditing.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraApplyEditorEquivalenceTest, "UEAssetDoc.Niagara.Apply.EditorEquivalence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraApplyEditorEquivalenceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraTests;
	UObject* Source = LoadObject<UObject>(nullptr, DirectionalBurstSystem);
	UNiagaraEmitter* Fountain = LoadObject<UNiagaraEmitter>(nullptr, FountainEmitter);
	if (!TestNotNull(TEXT("template system loads"), Source) || !TestNotNull(TEXT("template emitter loads"), Fountain))
	{
		return false;
	}
	const FTarget ByEditor(TEXT("NS_ByEditor"));
	const FTarget ByDocument(TEXT("NS_ByDocument"));
	ON_SCOPE_EXIT
	{
		ByDocument.Cleanup();
		ByEditor.Cleanup();
	};
	UNiagaraSystem* EditorSystem = Cast<UNiagaraSystem>(DuplicateTo(*Source, ByEditor));
	UNiagaraSystem* DocumentSystem = Cast<UNiagaraSystem>(DuplicateTo(*Source, ByDocument));
	if (!TestNotNull(TEXT("editor copy"), EditorSystem) || !TestNotNull(TEXT("document copy"), DocumentSystem))
	{
		return false;
	}
	EditorSystem->WaitForCompilationComplete(false, false);
	DocumentSystem->WaitForCompilationComplete(false, false);

	// 1. The editor's own API on the first copy.
	{
		FAssetDocumentEditorNonInteractiveScope NonInteractive;
		const TSharedRef<FNiagaraSystemViewModel> ViewModel = MakeViewModel(*EditorSystem);
		const TSharedPtr<FNiagaraEmitterHandleViewModel> Ribbon = HandleNamed(*ViewModel, TEXT("LocationBasedRibbon"));
		TSharedPtr<FNiagaraEmitterHandleViewModel> Burst = HandleNamed(*ViewModel, TEXT("DirectionalBurst"));
		if (!TestTrue(TEXT("template emitters present"), Ribbon.IsValid() && Burst.IsValid()))
		{
			return false;
		}
		ViewModel->DeleteEmitters({Ribbon->GetId()});
		Burst = HandleNamed(*ViewModel, TEXT("DirectionalBurst"));
		Burst->SetName(TEXT("Burst"));
		TestTrue(TEXT("editor: Fountain added"), ViewModel->AddEmitter(*Fountain, Fountain->GetExposedVersion().VersionGuid).IsValid());
		// Emitter Details: Local Space.
		const FVersionedNiagaraEmitter Emitter = Burst->GetEmitterViewModel()->GetEmitter();
		FVersionedNiagaraEmitterData* Data = Emitter.GetEmitterData();
		Emitter.Emitter->Modify();
		Data->bLocalSpace = true;
		FPropertyChangedEvent LocalSpace(FVersionedNiagaraEmitterData::StaticStruct()->FindPropertyByName(TEXT("bLocalSpace")), EPropertyChangeType::ValueSet);
		Emitter.Emitter->PostEditChangeVersionedProperty(LocalSpace, Emitter.Version);
		// Renderers: the sprite's Details, two new renderers, the light moved to the top, the sprite deleted after a
		// second sprite is added.
		UNiagaraSpriteRendererProperties* Sprite = Cast<UNiagaraSpriteRendererProperties>(Data->GetRenderers()[0]);
		DetailsEdit(*Sprite, TEXT("Alignment"), [Sprite]() { Sprite->Alignment = ENiagaraSpriteAlignment::VelocityAligned; });
		TestTrue(TEXT("editor: light renderer added"), AddRendererLikeTheEditor(*Burst, *UNiagaraLightRendererProperties::StaticClass()));
		TestTrue(TEXT("editor: mesh renderer added"), AddRendererLikeTheEditor(*Burst, *UNiagaraMeshRendererProperties::StaticClass()));
		UNiagaraRendererProperties* Light = Data->GetRenderers().Last(1);
		DetailsEdit(*Light, TEXT("RadiusScale"), [Light]() { Cast<UNiagaraLightRendererProperties>(Light)->RadiusScale = 2.0f; });
		UNiagaraStackRendererItem* LightItem = RendererItem(*Burst, Light);
		while (LightItem != nullptr && LightItem->CanMoveRendererUp())
		{
			LightItem->MoveRendererUp();
		}
		UNiagaraStackRendererItem* MeshItem = RendererItem(*Burst, Data->GetRenderers().Last());
		if (TestNotNull(TEXT("editor: mesh renderer item"), MeshItem))
		{
			MeshItem->SetIsEnabled(false);
		}
		UNiagaraStackRendererItem* SpriteItem = RendererItem(*Burst, Sprite);
		FText Message;
		const bool bDeletable = SpriteItem != nullptr && SpriteItem->TestCanDeleteWithMessage(Message);
		AddInfo(FString::Printf(TEXT("sprite item %d deletable %d: %s"), SpriteItem != nullptr ? 1 : 0, bDeletable ? 1 : 0, *Message.ToString()));
		if (TestTrue(TEXT("editor: sprite renderer deletable"), bDeletable))
		{
			SpriteItem->Delete();
		}
		// Disabled last: the editor refuses renderer deletes on a disabled emitter.
		Burst->SetIsEnabled(false, false);
		TestFalse(TEXT("editor: no dialog"), NonInteractive.WasInteractionRefused());
	}
	CompileAndWait(*EditorSystem);
	TestTrue(TEXT("editor copy saved"), SaveAsset(*EditorSystem, ByEditor.PackageFile));
	const TSharedPtr<FJsonObject> Expected = Extract(*this, ByEditor, TEXT("editor result"));
	if (!Expected.IsValid())
	{
		return false;
	}

	// 2. The same result as a document on the second copy.
	const TSharedPtr<FJsonObject> Document = Retargeted(Expected, ByDocument, TEXT("Update"));
	const TArray<TSharedPtr<FJsonValue>> Emitters = List(Child(Document, TEXT("Body")), TEXT("Emitters"));
	if (const TSharedPtr<FJsonObject> Burst = Named(Emitters, TEXT("Burst")))
	{
		Burst->SetStringField(TEXT("RenamedFrom"), TEXT("DirectionalBurst"));
	}
	if (const TSharedPtr<FJsonObject> Added = Named(Emitters, TEXT("Fountain")))
	{
		if (!Added->HasField(TEXT("Parent")))
		{
			Added->SetStringField(TEXT("From"), TEXT("/Niagara/DefaultAssets/Templates/Emitters/Fountain"));
		}
	}
	AddInfo(FString::Printf(TEXT("NiagaraExample edit document %s"), *Serialize(Document).Left(6000)));
	const FAssetDocumentResult Applied = ApplyDocument(*this, ByDocument, Document, TEXT("document"));
	if (!TestTrue(TEXT("the document applies"), Applied.IsSuccess()))
	{
		return false;
	}
	TestTrue(TEXT("the apply saves the system"), Applied.bSavedAsset);
	TestEqual(TEXT("both copies compile alike"), CompileErrors(*Cast<UNiagaraSystem>(ByDocument.Get())), CompileErrors(*EditorSystem));
	const TSharedPtr<FJsonObject> Actual = Extract(*this, ByDocument, TEXT("document result"));
	TestTextEqual(*this, TEXT("document path == editor path"), Content(Actual), Content(Expected));
	TestNoOpReapply(*this, ByDocument, TEXT("document copy"));
	TestReload(*this, ByDocument, TEXT("document copy"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraApplyUserParametersTest, "UEAssetDoc.Niagara.Apply.UserParameters",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraApplyUserParametersTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraTests;
	UObject* Source = LoadObject<UObject>(nullptr, DirectionalBurstSystem);
	if (!TestNotNull(TEXT("template system loads"), Source))
	{
		return false;
	}
	const FTarget Copy(TEXT("NS_UserParameters"));
	ON_SCOPE_EXIT
	{
		Copy.Cleanup();
	};
	UNiagaraSystem* System = Cast<UNiagaraSystem>(DuplicateTo(*Source, Copy));
	if (!TestNotNull(TEXT("copy"), System))
	{
		return false;
	}
	System->WaitForCompilationComplete(false, false);
	auto ReadParameters = [this, &Copy](const TCHAR* Stage)
	{
		return List(Child(Extract(*this, Copy, Stage), TEXT("Body")), TEXT("UserParameters"));
	};
	auto WithParameters = [this, &Copy](const FString& Json, const TCHAR* Stage)
	{
		const TSharedPtr<FJsonObject> Document = Retargeted(Extract(*this, Copy, Stage), Copy, TEXT("Update"));
		TArray<TSharedPtr<FJsonValue>> Values;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		FJsonSerializer::Deserialize(Reader, Values);
		const TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetArrayField(TEXT("UserParameters"), Values);
		Document->SetObjectField(TEXT("Body"), Body);
		return Document;
	};

	// 1. Add: values of each kind, a description, a nested category.
	const FAssetDocumentResult Added = ApplyDocument(*this, Copy, WithParameters(TEXT(R"json([
  {"Name": "User.Count", "Type": "int32", "Value": 7, "Description": "Burst size", "Category": "Spawn"},
  {"Name": "User.Speed", "Type": "float", "Value": 300.0, "Category": "Spawn|Motion"},
  {"Name": "User.Tint", "Type": "LinearColor", "Value": {"R": 1.0, "G": 0.5, "B": 0.25, "A": 1.0}},
  {"Name": "User.Mesh", "Type": "Object:/Script/Engine.StaticMesh", "Value": {"Object": "/Engine/BasicShapes/Cube.Cube"}},
  {"Name": "User.Curve", "Type": "/Script/Niagara.NiagaraDataInterfaceCurve", "Value": {"DataInterface": {"Class": "Curve", "Properties": {"bOptimizeLUT": false}}}}
])json"), TEXT("before add")), TEXT("add"));
	if (!TestTrue(TEXT("add applies"), Added.IsSuccess()))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>> AfterAdd = ReadParameters(TEXT("after add"));
	for (const TSharedPtr<FJsonValue>& Value : AfterAdd)
	{
		AddInfo(FString::Printf(TEXT("NiagaraExample user parameter %s"), *Serialize(Value->AsObject())));
	}
	TestEqual(TEXT("five parameters"), AfterAdd.Num(), 5);
	const TSharedPtr<FJsonObject> Count = Named(AfterAdd, TEXT("User.Count"));
	TestTrue(TEXT("Count: value, description, category"), Count.IsValid() && Count->GetIntegerField(TEXT("Value")) == 7
		&& Count->GetStringField(TEXT("Description")) == TEXT("Burst size") && Count->GetStringField(TEXT("Category")) == TEXT("Spawn"));
	const TSharedPtr<FJsonObject> Speed = Named(AfterAdd, TEXT("User.Speed"));
	TestTrue(TEXT("Speed: nested category"), Speed.IsValid() && Speed->GetStringField(TEXT("Category")) == TEXT("Spawn|Motion"));
	const TSharedPtr<FJsonObject> Mesh = Named(AfterAdd, TEXT("User.Mesh"));
	TestTrue(TEXT("Mesh: object value"), Mesh.IsValid() && Serialize(Child(Mesh, TEXT("Value"))).Contains(TEXT("/Engine/BasicShapes/Cube.Cube")));
	// The User Parameters panel's hierarchy holds the categories with the parameters' items (independent walk).
	{
		UNiagaraSystemEditorData* EditorData = Cast<UNiagaraSystemEditorData>(System->GetEditorData());
		const UHierarchyRoot* Root = EditorData != nullptr ? EditorData->UserParameterHierarchy.Get() : nullptr;
		const UNiagaraScriptVariable* CountVariable = EditorData != nullptr
			? EditorData->FindOrAddUserScriptVariable(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("User.Count")), *System) : nullptr;
		const UHierarchyCategory* Spawn = nullptr;
		for (const UHierarchyElement* Element : Root != nullptr ? Root->GetChildren() : TConstArrayView<const TObjectPtr<UHierarchyElement>>())
		{
			const UHierarchyCategory* Category = Cast<UHierarchyCategory>(Element);
			if (Category != nullptr && Category->GetCategoryName() == TEXT("Spawn"))
			{
				Spawn = Category;
			}
		}
		bool bCountUnderSpawn = false;
		bool bMotionUnderSpawn = false;
		for (const UHierarchyElement* Element : Spawn != nullptr ? Spawn->GetChildren() : TConstArrayView<const TObjectPtr<UHierarchyElement>>())
		{
			const UHierarchyCategory* Nested = Cast<UHierarchyCategory>(Element);
			bMotionUnderSpawn |= Nested != nullptr && Nested->GetCategoryName() == TEXT("Motion");
			bCountUnderSpawn |= CountVariable != nullptr && Element->GetPersistentIdentity().Guids.Contains(CountVariable->Metadata.GetVariableGuid());
		}
		TestTrue(TEXT("hierarchy: Spawn category holds Count and the Motion category"), bCountUnderSpawn && bMotionUnderSpawn);
	}

	// 2. A human links a module input to User.Count (the stack's "Link Inputs" menu).
	{
		FAssetDocumentEditorNonInteractiveScope NonInteractive;
		const TSharedRef<FNiagaraSystemViewModel> ViewModel = MakeViewModel(*System);
		UNiagaraStackFunctionInput* Input = ModuleInput(*ViewModel, TEXT("DirectionalBurst"), TEXT("SpawnBurst_Instantaneous"), TEXT("Spawn Count"));
		if (!TestNotNull(TEXT("Spawn Count input"), Input))
		{
			return false;
		}
		Input->SetLinkedParameterValue(FNiagaraVariableBase(FNiagaraTypeDefinition::GetIntDef(), TEXT("User.Count")));
	}
	CompileAndWait(*System);
	TestTrue(TEXT("human edit saved"), SaveAsset(*System, Copy.PackageFile));

	// 3. Rename: the document renames the parameter and the link it shows; the input follows the rename.
	{
		FString Text = Serialize(Retargeted(Extract(*this, Copy, TEXT("before rename")), Copy, TEXT("Update")));
		TestTrue(TEXT("the extract shows the link"), Text.Contains(TEXT("{\"Link\":\"User.Count\"}")));
		Text.ReplaceInline(TEXT("\"User.Count\""), TEXT("\"User.Amount\""), ESearchCase::CaseSensitive);
		const TSharedPtr<FJsonObject> Document = Parse(Text);
		Named(List(Child(Document, TEXT("Body")), TEXT("UserParameters")), TEXT("User.Amount"))->SetStringField(TEXT("RenamedFrom"), TEXT("User.Count"));
		TestTrue(TEXT("rename applies"), ApplyDocument(*this, Copy, Document, TEXT("rename")).IsSuccess());
		const TSharedRef<FNiagaraSystemViewModel> ViewModel = MakeViewModel(*System);
		const UNiagaraStackFunctionInput* Input = ModuleInput(*ViewModel, TEXT("DirectionalBurst"), TEXT("SpawnBurst_Instantaneous"), TEXT("Spawn Count"));
		TestTrue(TEXT("rename: the input links the new name"), Input != nullptr && Input->GetValueMode() == UNiagaraStackFunctionInput::EValueMode::Linked
			&& Input->GetLinkedParameterValue().GetName() == TEXT("User.Amount"));
		const TArray<TSharedPtr<FJsonValue>> AfterRename = ReadParameters(TEXT("after rename"));
		const TSharedPtr<FJsonObject> Amount = Named(AfterRename, TEXT("User.Amount"));
		TestTrue(TEXT("rename keeps value, description and category"), Amount.IsValid() && Amount->GetIntegerField(TEXT("Value")) == 7
			&& Amount->GetStringField(TEXT("Description")) == TEXT("Burst size") && Amount->GetStringField(TEXT("Category")) == TEXT("Spawn"));
		TestFalse(TEXT("rename: old name gone"), Named(AfterRename, TEXT("User.Count")).IsValid());
	}

	// 4. Values, description, category and data interface Details change; omitted DI properties go back to defaults.
	{
		const FAssetDocumentResult Changed = ApplyDocument(*this, Copy, WithParameters(TEXT(R"json([
  {"Name": "User.Amount", "Type": "int32", "Category": "Spawn"},
  {"Name": "User.Speed", "Type": "float", "Value": 450.0, "Description": "Initial speed", "Category": "Look|Size"},
  {"Name": "User.Tint", "Type": "LinearColor", "Value": {"R": 0.0, "G": 1.0, "B": 0.0, "A": 1.0}},
  {"Name": "User.Mesh", "Type": "Object:/Script/Engine.StaticMesh", "Value": {"Object": ""}},
  {"Name": "User.Curve", "Type": "/Script/Niagara.NiagaraDataInterfaceCurve", "Value": {"DataInterface": {"Class": "Curve"}}}
])json"), TEXT("before change")), TEXT("change"));
		TestTrue(TEXT("change applies"), Changed.IsSuccess());
		const TArray<TSharedPtr<FJsonValue>> AfterChange = ReadParameters(TEXT("after change"));
		const TSharedPtr<FJsonObject> Amount = Named(AfterChange, TEXT("User.Amount"));
		TestTrue(TEXT("omitted value kept, omitted description cleared"), Amount.IsValid() && Amount->GetIntegerField(TEXT("Value")) == 7
			&& !Amount->HasField(TEXT("Description")));
		const TSharedPtr<FJsonObject> Moved = Named(AfterChange, TEXT("User.Speed"));
		TestTrue(TEXT("value, description and category changed"), Moved.IsValid() && Moved->GetNumberField(TEXT("Value")) == 450.0
			&& Moved->GetStringField(TEXT("Description")) == TEXT("Initial speed") && Moved->GetStringField(TEXT("Category")) == TEXT("Look|Size"));
		const TSharedPtr<FJsonObject> Curve = Named(AfterChange, TEXT("User.Curve"));
		TestFalse(TEXT("omitted data interface property is back to its default"), Curve.IsValid()
			&& Serialize(Child(Curve, TEXT("Value"))).Contains(TEXT("bOptimizeLUT")));
		TestNoOpReapply(*this, Copy, TEXT("user parameters"));
	}

	// 5. Delete: the linked input keeps the parameter's value as a local value (the editor's delete).
	{
		const FAssetDocumentResult Deleted = ApplyDocument(*this, Copy, WithParameters(TEXT(R"json([
  {"Name": "User.Speed", "Type": "float"}
])json"), TEXT("before delete")), TEXT("delete"));
		TestTrue(TEXT("delete applies"), Deleted.IsSuccess());
		TestEqual(TEXT("one parameter left"), ReadParameters(TEXT("after delete")).Num(), 1);
		const TSharedRef<FNiagaraSystemViewModel> ViewModel = MakeViewModel(*System);
		UNiagaraStackFunctionInput* Input = ModuleInput(*ViewModel, TEXT("DirectionalBurst"), TEXT("SpawnBurst_Instantaneous"), TEXT("Spawn Count"));
		const TSharedPtr<const FStructOnScope> Local = Input != nullptr && Input->GetValueMode() == UNiagaraStackFunctionInput::EValueMode::Local
			? Input->GetLocalValueStruct() : nullptr;
		TestTrue(TEXT("delete: the input holds the value locally"), Local.IsValid() && Local->GetStructMemory() != nullptr
			&& *reinterpret_cast<const int32*>(Local->GetStructMemory()) == 7);
	}

	// 6. Refusals change nothing.
	{
		const FString Before = Serialize(Extract(*this, Copy, TEXT("before refusals")));
		const FAssetDocumentResult Unknown = ApplyDocument(*this, Copy, WithParameters(TEXT(R"json([
  {"Name": "User.Speed", "Type": "float"}, {"Name": "User.Odd", "Type": "NoSuchType"}
])json"), TEXT("unknown type")), TEXT("unknown type"));
		TestTrue(TEXT("unknown type refused"), !Unknown.IsSuccess() && HasDiagnostic(Unknown, TEXT("Niagara.UnknownUserParameterType")));
		const FAssetDocumentResult Retype = ApplyDocument(*this, Copy, WithParameters(TEXT(R"json([
  {"Name": "User.Speed", "Type": "int32"}
])json"), TEXT("retype")), TEXT("retype"));
		TestTrue(TEXT("retype refused"), !Retype.IsSuccess() && HasDiagnostic(Retype, TEXT("Niagara.UserParameterRetype")));
		TestTextEqual(*this, TEXT("refusals change nothing"), Serialize(Extract(*this, Copy, TEXT("after refusals"))), Before);
		TestFalse(TEXT("refusals leave the package clean"), System->GetOutermost()->IsDirty());
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraApplyTemplatesTest, "UEAssetDoc.Niagara.Apply.Templates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraApplyTemplatesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraTests;
	int32 Full = 0;
	int32 Present = 0;
	for (const TCHAR* Path : TemplateSystems)
	{
		const FString Label = FPackageName::ObjectPathToObjectName(FString(Path));
		if (LoadObject<UNiagaraSystem>(nullptr, Path) == nullptr)
		{
			AddInfo(FString::Printf(TEXT("NiagaraTemplate %s: not in this install"), *Label));
			continue;
		}
		++Present;
		// Read-only extract of the plugin asset.
		const FAssetDocumentResult Read = ExtractResult(Path);
		if (!TestTrue(*FString::Printf(TEXT("%s: read-only extract"), *Label), Read.IsSuccess() && Read.Payload.IsValid() && Read.bReadOnly))
		{
			LogResult(*this, Label, Read);
			continue;
		}
		const FTarget Fresh(FString(TEXT("NS_Tmpl")) + Label.Left(12));
		ON_SCOPE_EXIT
		{
			Fresh.Cleanup();
		};
		// Emitters come from the template emitter of the same name, else empty; lightweight emitters are added as
		// lightweight. Every region is authored: stacks, inputs, handlers, stages, renderers.
		const TSharedPtr<FJsonObject> Projection = Clone(Read.Payload);
		TArray<FString> Notes;
		for (const TSharedPtr<FJsonValue>& Entry : List(Child(Projection, TEXT("Body")), TEXT("Emitters")))
		{
			const TSharedPtr<FJsonObject> Emitter = Entry->AsObject();
			const FString Name = Emitter->GetStringField(TEXT("Name"));
			FString Mode;
			if (Emitter->TryGetStringField(TEXT("Mode"), Mode) && Mode == TEXT("Stateless"))
			{
				Notes.Add(Name + TEXT(" (lightweight)"));
				continue;
			}
			const FString TemplatePath = FString::Printf(TEXT("%s/%s.%s"), TemplateEmitters, *Name, *Name);
			if (!Emitter->HasField(TEXT("Parent")) && LoadObject<UNiagaraEmitter>(nullptr, *TemplatePath, nullptr, LOAD_NoWarn | LOAD_Quiet) != nullptr)
			{
				Emitter->SetStringField(TEXT("From"), TemplatePath);
			}
			else if (!Emitter->HasField(TEXT("Parent")))
			{
				Emitter->SetStringField(TEXT("From"), TEXT("Empty"));
				Notes.Add(Name + TEXT(" (from an empty emitter)"));
			}
		}
		const TSharedPtr<FJsonObject> Document = Retargeted(Projection, Fresh, TEXT("Create"));
		const double Start = FPlatformTime::Seconds();
		const FAssetDocumentResult Created = ApplyDocument(*this, Fresh, Document, Label + TEXT(" create"));
		const double CreateMs = Milliseconds(Start);
		if (!TestTrue(*FString::Printf(TEXT("%s: the template's document creates a system"), *Label), Created.IsSuccess()))
		{
			LogResidue(*this, Fresh, Label);
			continue;
		}
		const TSharedPtr<FJsonObject> Result = Extract(*this, Fresh, Label + TEXT(" created"));
		TestTextEqual(*this, FString::Printf(TEXT("%s: created system == template"), *Label), MaskEditorNames(Content(Clone(Result))),
			MaskEditorNames(Content(WithoutMakeKeys(Document))));
		TestNoOpReapply(*this, Fresh, Label);
		++Full;
		AddInfo(FString::Printf(TEXT("NiagaraTemplate %s: full, create %.0fms%s%s"), *Label, CreateMs,
			Notes.IsEmpty() ? TEXT("") : TEXT("; "), *FString::Join(Notes, TEXT("; "))));
	}
	AddInfo(FString::Printf(TEXT("NiagaraTemplate summary: %d present, %d full"), Present, Full));
	TestTrue(TEXT("template systems present"), Present >= 5);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraApplySparseCurvesTest, "UEAssetDoc.Niagara.Apply.SparseCurves",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraApplySparseCurvesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraTests;
	const FTarget Target(TEXT("NS_SparseBurst"));
	const FTarget Copy(TEXT("NS_SparseCopy"));
	ON_SCOPE_EXIT
	{
		Copy.Cleanup();
		Target.Cleanup();
	};
	const TCHAR* Template = TEXT("/Niagara/DefaultAssets/Templates/Emitters/OmnidirectionalBurst.OmnidirectionalBurst");
	// The trial's burst: a user color and one emitter from the OmnidirectionalBurst template.
	const FString Json = FString::Printf(TEXT(R"json({"SchemaVersion": 1, "Target": "%s", "Class": "/Script/Niagara.NiagaraSystem",
  "Action": "Create", "Body": {"UserParameters": [{"Name": "User.Color", "Type": "LinearColor", "Value": {"R": 0.2, "G": 1, "B": 0.3, "A": 1}}],
  "Emitters": [{"Name": "Burst", "From": "%s"}]}})json"), *Target.PackageName, Template);
	if (!TestTrue(TEXT("the system is created"), Apply(*this, Target, Json, TEXT("create")).IsSuccess()))
	{
		LogResidue(*this, Target, TEXT("sparse"));
		return false;
	}
	const TSharedPtr<FJsonObject> Document = Extract(*this, Target, TEXT("extract"));
	FAssetDocumentExtractRequest WholeRequest;
	WholeRequest.AssetPath = Target.ObjectPath;
	WholeRequest.bDiffOnly = false;
	const FAssetDocumentResult Whole = FAssetDocumentModule::Get().GetService().Extract(WholeRequest);
	if (!Document.IsValid() || !TestTrue(TEXT("whole extract"), Whole.IsSuccess() && Whole.Payload.IsValid()))
	{
		return false;
	}
	const FString SparseText = Serialize(Document);
	const FString WholeText = Serialize(Whole.Payload);
	AddInfo(FString::Printf(TEXT("Niagara extract: %d chars (data interfaces whole: %d chars)"), SparseText.Len(), WholeText.Len()));
	TestTrue(TEXT("the template's curves are read (every key field when whole)"), WholeText.Contains(TEXT("\"ArriveTangentWeight\":0")));
	TestTrue(TEXT("the extract has curve keys"), SparseText.Contains(TEXT("\"Keys\"")));
	TestFalse(TEXT("the extract leaves default key fields out"), SparseText.Contains(TEXT("\"ArriveTangentWeight\":0"))
		|| SparseText.Contains(TEXT("\"TangentWeightMode\":\"RCTWM_WeightedNone\"")));
	TestTrue(TEXT("the extract is smaller"), SparseText.Len() < WholeText.Len());

	// Applied as a copy (the emitter made from the same template), it gives the same system.
	const TSharedPtr<FJsonObject> CopyDocument = Retargeted(Document, Copy, TEXT("Create"));
	for (const TSharedPtr<FJsonValue>& Entry : List(Child(CopyDocument, TEXT("Body")), TEXT("Emitters")))
	{
		Entry->AsObject()->SetStringField(TEXT("From"), Template);
	}
	if (!TestTrue(TEXT("the extract applies as a copy"), ApplyDocument(*this, Copy, CopyDocument, TEXT("copy")).IsSuccess()))
	{
		LogResidue(*this, Copy, TEXT("sparse copy"));
		return false;
	}
	TestTextEqual(*this, TEXT("the copy extracts the same document"), MaskEditorNames(Content(Extract(*this, Copy, TEXT("copy extract")))),
		MaskEditorNames(Content(Document)));
	const FAssetDocumentResult Diff = DiffDocument(Retargeted(Document, Copy, TEXT("Update")));
	TestTrue(TEXT("Diff of the copy against the extract is empty"), Diff.IsSuccess() && DiffCount(Diff) == 0);
	if (DiffCount(Diff) > 0)
	{
		AddInfo(FString::Printf(TEXT("copy diff: %s"), *Serialize(Diff.Payload).Left(3000)));
	}
	TestNoOpReapply(*this, Target, TEXT("sparse"));
	TestNoOpReapply(*this, Copy, TEXT("sparse copy"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraApplyFromScratchTest, "UEAssetDoc.Niagara.Apply.FromScratch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraApplyFromScratchTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraTests;
	const FTarget Target(TEXT("NS_Scratch"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
	};
	const FString Json = FString::Printf(TEXT(R"json({
  "SchemaVersion": 1, "Target": "%s", "Class": "/Script/Niagara.NiagaraSystem", "Action": "Create", "Definitions": {},
  "Properties": {"bDeterminism": true, "RandomSeed": 7},
  "Body": {
    "UserParameters": [
      {"Name": "User.Rate", "Type": "float", "Value": 40.0, "Description": "Particles per second", "Category": "Spawn"},
      {"Name": "User.Color", "Type": "LinearColor", "Value": {"R": 1.0, "G": 0.2, "B": 0.1, "A": 1.0}, "Category": "Look"}
    ],
    "Emitters": [
      {"Name": "Jet", "From": "/Niagara/DefaultAssets/Templates/Emitters/Fountain",
       "Properties": {"bLocalSpace": true},
       "Renderers": [
         {"Name": "Glow", "Class": "LightRenderer", "Properties": {"RadiusScale": 3.0}},
         {"Name": "NiagaraSpriteRendererProperties_0", "Class": "SpriteRenderer", "Properties": {"Alignment": "VelocityAligned"}}
       ]},
      {"Name": "Puff", "From": "/Niagara/DefaultAssets/Templates/Emitters/OmnidirectionalBurst", "Enabled": false},
      {"Name": "Blank", "From": "Empty"}
    ]
  }
})json"), *Target.PackageName);
	const FAssetDocumentResult Created = Apply(*this, Target, Json, TEXT("create"));
	if (!TestTrue(TEXT("the system is created"), Created.IsSuccess()))
	{
		LogResidue(*this, Target, TEXT("scratch"));
		return false;
	}
	UNiagaraSystem* System = Cast<UNiagaraSystem>(Target.Get());
	if (!TestNotNull(TEXT("system"), System))
	{
		return false;
	}
	TestEqual(TEXT("no compile errors"), CompileErrors(*System), FString());
	const TSharedPtr<FJsonObject> Document = Extract(*this, Target, TEXT("created"));
	AddInfo(FString::Printf(TEXT("NiagaraExample scratch %s"), *Serialize(SystemLevel(Document, false)).Left(5000)));
	const TArray<TSharedPtr<FJsonValue>> Emitters = List(Child(Document, TEXT("Body")), TEXT("Emitters"));
	TestEqual(TEXT("three emitters"), Emitters.Num(), 3);
	const TSharedPtr<FJsonObject> Jet = Named(Emitters, TEXT("Jet"));
	const TArray<TSharedPtr<FJsonValue>> Renderers = List(Jet, TEXT("Renderers"));
	TestTrue(TEXT("Jet: renderers in document order"), Renderers.Num() == 2 && Renderers[0]->AsObject()->GetStringField(TEXT("Name")) == TEXT("Glow"));
	TestTrue(TEXT("Jet: local space"), Jet.IsValid() && Child(Jet, TEXT("Properties"))->GetBoolField(TEXT("bLocalSpace")));
	const TSharedPtr<FJsonObject> Puff = Named(Emitters, TEXT("Puff"));
	TestTrue(TEXT("Puff disabled"), Puff.IsValid() && Puff->HasField(TEXT("Enabled")) && !Puff->GetBoolField(TEXT("Enabled")));
	TestNoOpReapply(*this, Target, TEXT("scratch"));
	TestReload(*this, Target, TEXT("scratch"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraApplyRefusalsTest, "UEAssetDoc.Niagara.Apply.Refusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraApplyRefusalsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraTests;
	ExpectVectorVMCompileErrors(*this, TEXT("AssetDocumentUndefinedSymbol"));
	UObject* Source = LoadObject<UObject>(nullptr, DirectionalBurstSystem);
	if (!TestNotNull(TEXT("template system loads"), Source) || GEditor == nullptr || GEditor->Trans == nullptr)
	{
		return false;
	}
	const FTarget Copy(TEXT("NS_Refusals"));
	const FTarget Fresh(TEXT("NS_RefusedCreate"));
	ON_SCOPE_EXIT
	{
		Fresh.Cleanup();
		Copy.Cleanup();
	};
	UNiagaraSystem* System = Cast<UNiagaraSystem>(DuplicateTo(*Source, Copy));
	if (!TestNotNull(TEXT("copy"), System))
	{
		return false;
	}
	System->WaitForCompilationComplete(false, false);

	// The human disables the ribbon emitter and gives one of its inputs
	// an expression that does not compile; disabled emitters are not compiled, so the system compiles.
	{
		FAssetDocumentEditorNonInteractiveScope NonInteractive;
		const TSharedRef<FNiagaraSystemViewModel> ViewModel = MakeViewModel(*System);
		const TSharedPtr<FNiagaraEmitterHandleViewModel> Ribbon = HandleNamed(*ViewModel, TEXT("LocationBasedRibbon"));
		if (!TestTrue(TEXT("ribbon emitter"), Ribbon.IsValid()))
		{
			return false;
		}
		Ribbon->SetIsEnabled(false, false);
		UNiagaraStackFunctionInput* Input = nullptr;
		TArray<UNiagaraStackModuleItem*> Modules;
		Ribbon->GetEmitterStackViewModel()->GetRootEntry()->GetUnfilteredChildrenOfType(Modules, true);
		for (UNiagaraStackModuleItem* Module : Modules)
		{
			TArray<UNiagaraStackFunctionInput*> Inputs;
			Module->GetParameterInputs(Inputs);
			for (UNiagaraStackFunctionInput* Candidate : Inputs)
			{
				if (Input == nullptr && Candidate->GetInputType() == FNiagaraTypeDefinition::GetFloatDef())
				{
					Input = Candidate;
				}
			}
		}
		if (!TestNotNull(TEXT("a float input of the ribbon emitter"), Input))
		{
			return false;
		}
		Input->SetCustomExpression(TEXT("AssetDocumentUndefinedSymbol * 2.0"));
	}
	CompileAndWait(*System);
	TestEqual(TEXT("the system compiles with the broken emitter disabled"), CompileErrors(*System), FString());
	// A human transaction on the system (kept in the undo history), then a save.
	ReleaseUndoHistory();
	{
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentNiagaraTests", "HumanEdit", "Human Edit"));
		System->Modify();
	}
	const int32 HumanSteps = GEditor->Trans->GetQueueLength();
	TestTrue(TEXT("human edit saved"), SaveAsset(*System, Copy.PackageFile));
	const FString Baseline = Serialize(Extract(*this, Copy, TEXT("baseline")));
	const TSharedPtr<FJsonObject> Document = Retargeted(Parse(Baseline), Copy, TEXT("Update"));
	const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Copy.PackageFile);
	// A rolled-back apply may reload the package: the system is looked up again after each apply.
	auto Unchanged = [this, &Copy, &Baseline, &Stamp](const FString& Stage)
	{
		TestTextEqual(*this, FString::Printf(TEXT("%s: the system is unchanged"), *Stage), Serialize(Extract(*this, Copy, *Stage)), Baseline);
		TestEqual(*FString::Printf(TEXT("%s: package file untouched"), *Stage), IFileManager::Get().GetTimeStamp(*Copy.PackageFile), Stamp);
		const UObject* Live = Copy.Get();
		TestFalse(*FString::Printf(TEXT("%s: package clean"), *Stage), Live == nullptr || Live->GetOutermost()->IsDirty());
	};

	// 1. Enabling the broken emitter adds a compile error: the apply fails and is rolled back; the human's undo
	//    history is kept.
	{
		const TSharedPtr<FJsonObject> Enable = Clone(Document);
		Named(List(Child(Enable, TEXT("Body")), TEXT("Emitters")), TEXT("LocationBasedRibbon"))->RemoveField(TEXT("Enabled"));
		const FAssetDocumentResult Failed = ApplyDocument(*this, Copy, Enable, TEXT("compile error"));
		TestTrue(TEXT("a new compile error fails the apply"), !Failed.IsSuccess() && HasDiagnostic(Failed, TEXT("Niagara.CompileFailed")));
		Unchanged(TEXT("after compile failure"));
		TestEqual(TEXT("the human's undo history is kept"), GEditor->Trans->GetQueueLength(), HumanSteps);
	}
	// 2. A change that adds no error to a system that has one already: the error is a warning, the apply goes
	//    through and clears undo history that references the system.
	{
		const TSharedPtr<FJsonObject> Changed = Clone(Document);
		Child(Changed, TEXT("Properties"))->SetNumberField(TEXT("RandomSeed"), 42);
		const FAssetDocumentResult Applied = ApplyDocument(*this, Copy, Changed, TEXT("change"));
		TestTrue(TEXT("a change without new errors applies"), Applied.IsSuccess());
		TestFalse(TEXT("after an apply: no undo history references the system"), UE::AssetDocument::UndoHistory::ReferencesPackage(Copy.PackageName));
	}
	const FString AfterChange = Serialize(Extract(*this, Copy, TEXT("after change")));
	// 3. Stack edits the editor cannot make and parent changes are refused, and change nothing: a particle module in
	//    System Update (the "+" menu's usage filter), a scratch pad added to an emitter (the system editor's Scratch Pad
	//    panel only makes system scratch pads), a parent set to another emitter.
	{
		const TSharedPtr<FJsonObject> Usage = Parse(AfterChange);
		Usage->SetStringField(TEXT("Action"), TEXT("Update"));
		TArray<TSharedPtr<FJsonValue>> SystemUpdate = List(Child(Usage, TEXT("Body")), TEXT("SystemUpdate"));
		SystemUpdate.Add(MakeShared<FJsonValueObject>(Parse(TEXT(R"json({"Name": "Drag", "Script": "/Niagara/Modules/Update/Forces/Drag"})json"))));
		Child(Usage, TEXT("Body"))->SetArrayField(TEXT("SystemUpdate"), SystemUpdate);
		const FAssetDocumentResult Refused = ApplyDocument(*this, Copy, Usage, TEXT("usage"));
		TestTrue(TEXT("a particle module in System Update is refused"), !Refused.IsSuccess() && HasDiagnostic(Refused, TEXT("Niagara.ModuleUsage")));
		const TSharedPtr<FJsonObject> Scratch = Parse(AfterChange);
		Scratch->SetStringField(TEXT("Action"), TEXT("Update"));
		TArray<TSharedPtr<FJsonValue>> Pads;
		Pads.Add(MakeShared<FJsonValueObject>(Parse(TEXT(R"json({"Name": "Swirl"})json"))));
		Named(List(Child(Scratch, TEXT("Body")), TEXT("Emitters")), TEXT("DirectionalBurst"))->SetArrayField(TEXT("ScratchPads"), Pads);
		const FAssetDocumentResult ScratchRefused = ApplyDocument(*this, Copy, Scratch, TEXT("scratch pad"));
		TestTrue(TEXT("an emitter scratch pad cannot be added"), !ScratchRefused.IsSuccess() && HasDiagnostic(ScratchRefused, TEXT("Niagara.ScratchPadScope")));
		// An expression input with an HLSL error: the compile fails and the whole apply rolls back.
		const TSharedPtr<FJsonObject> Expression = Parse(AfterChange);
		Expression->SetStringField(TEXT("Action"), TEXT("Update"));
		const TSharedPtr<FJsonObject> Cone = Named(List(Named(List(Child(Expression, TEXT("Body")), TEXT("Emitters")), TEXT("DirectionalBurst")),
			TEXT("ParticleSpawn")), TEXT("AddVelocityInCone"));
		if (TestTrue(TEXT("AddVelocityInCone listed"), Cone.IsValid()))
		{
			if (!Cone->HasTypedField<EJson::Object>(TEXT("Inputs")))
			{
				Cone->SetObjectField(TEXT("Inputs"), MakeShared<FJsonObject>());
			}
			Child(Cone, TEXT("Inputs"))->SetObjectField(TEXT("Velocity Distribution Along Cone Axis"),
				Parse(TEXT(R"json({"Expression": "AssetDocumentUndefinedSymbol * 3.0"})json")));
			const FAssetDocumentResult Broken = ApplyDocument(*this, Copy, Expression, TEXT("expression"));
			TestTrue(TEXT("an expression that does not compile rolls back"), !Broken.IsSuccess() && HasDiagnostic(Broken, TEXT("Niagara.CompileFailed")));
			// The snapshot restore holds (the compile's data interface copies are rebuilt), no reload from disk.
			TestFalse(TEXT("the rollback restores in memory"), HasDiagnostic(Broken, TEXT("PackageSnapshot.ReloadedFromDisk")));
		}
		const TSharedPtr<FJsonObject> Parent = Parse(AfterChange);
		Parent->SetStringField(TEXT("Action"), TEXT("Update"));
		Named(List(Child(Parent, TEXT("Body")), TEXT("Emitters")), TEXT("DirectionalBurst"))->SetStringField(TEXT("Parent"), FountainEmitter);
		const FAssetDocumentResult ParentRefused = ApplyDocument(*this, Copy, Parent, TEXT("parent"));
		TestTrue(TEXT("a parent change is refused"), !ParentRefused.IsSuccess() && HasDiagnostic(ParentRefused, TEXT("Niagara.ParentImmutable")));
		TestTrue(TEXT("the parent refusal says which assets can be parents"), ParentRefused.Diagnostics.ContainsByPredicate(
			[](const FAssetDocumentDiagnostic& D) { return D.Code == TEXT("Niagara.ParentImmutable") && D.Message.Contains(TEXT("inheritable emitter asset")); }));
		// A new emitter whose Parent (with or without the same From) is the Fountain template, which is not
		// inheritable: the refusal says so at its Parent, instead of the later ParentImmutable of the parentless copy.
		for (const bool bWithFrom : {false, true})
		{
			const TSharedPtr<FJsonObject> NewChild = Parse(AfterChange);
			NewChild->SetStringField(TEXT("Action"), TEXT("Update"));
			TArray<TSharedPtr<FJsonValue>> Emitters = Child(NewChild, TEXT("Body"))->GetArrayField(TEXT("Emitters"));
			const TSharedPtr<FJsonObject> Added = Parse(FString::Printf(TEXT(R"json({"Name": "Inherited", "Parent": {"Emitter": "%s"}})json"),
				*FString(FountainEmitter).Left(FString(FountainEmitter).Find(TEXT(".")))));
			if (bWithFrom)
			{
				Added->SetStringField(TEXT("From"), FString(FountainEmitter).Left(FString(FountainEmitter).Find(TEXT("."))));
			}
			const FString Path = FString::Printf(TEXT("/Body/Emitters/%d/Parent"), Emitters.Num());
			Emitters.Add(MakeShared<FJsonValueObject>(Added));
			Child(NewChild, TEXT("Body"))->SetArrayField(TEXT("Emitters"), Emitters);
			const FAssetDocumentResult NotInheritable = ApplyDocument(*this, Copy, NewChild, bWithFrom ? TEXT("parent and from a template") : TEXT("parent a template"));
			const FAssetDocumentDiagnostic* Found = NotInheritable.Diagnostics.FindByPredicate(
				[](const FAssetDocumentDiagnostic& D) { return D.Code == TEXT("Niagara.NotInheritable"); });
			if (TestNotNull(TEXT("a template Parent is refused as not inheritable"), Found))
			{
				TestEqual(TEXT("at the new emitter's Parent"), Found->Path, Path);
				TestTrue(TEXT("the message names Is Inheritable and says to leave Parent out"),
					Found->Message.Contains(TEXT("Is Inheritable")) && Found->Message.Contains(TEXT("Leave Parent out")));
			}
		}
		TestTextEqual(*this, TEXT("refusals change nothing"), Serialize(Extract(*this, Copy, TEXT("after refusals"))), AfterChange);
	}
	// 4. A Create that fails leaves no asset.
	{
		const TSharedPtr<FJsonObject> Bad = Retargeted(Parse(AfterChange), Fresh, TEXT("Create"));
		Bad->SetObjectField(TEXT("Body"), Parse(TEXT(R"json({"Emitters": [{"Name": "Ghost", "From": "/Game/AssetDocumentNiagaraTests/NoSuchEmitter"}]})json")));
		const FAssetDocumentResult Failed = ApplyDocument(*this, Fresh, Bad, TEXT("failed create"));
		TestTrue(TEXT("create from a missing emitter fails"), !Failed.IsSuccess() && HasDiagnostic(Failed, TEXT("Niagara.EmitterAssetNotFound")));
		TestFalse(TEXT("the failed create leaves no asset"), Fresh.Exists());
	}
	return !HasAnyErrors();
}
#endif
