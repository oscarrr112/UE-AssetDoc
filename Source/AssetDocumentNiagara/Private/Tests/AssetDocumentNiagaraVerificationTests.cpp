// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Struct values that list only some of their fields verify against the full
// read-back. An omitted struct member is the member's default, also inside array elements:
//   PartialStructs   a mesh renderer's Meshes / OverrideMaterials elements with one field each, and a curve data
//                    interface (a user parameter and a module input's dynamic input) whose keys give Time / Value
//                    only. The system is created, re-applied (a no-op) and its extract re-applied.
// Further cases:
//   EnumShortName    an enum input written by its short name ("Kill" for "Kill (System and Emitters Die
//                    Immediately)") applies when one value has it, reads back by its full name and re-applies as a
//                    no-op; a name no value has is refused listing the values.
//   AllDefaultInputs inputs set to their default value in two modules fail one apply with one
//                    Niagara.InputIsDefault naming them all (it used to name the first only).
//   Lifecycle        inspect says whether the system loops (SystemState's Loop Behavior), also at its default,
//                    which the document does not list.

#include "AssetDocumentNiagaraTestEditing.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentNiagaraVerificationTests
{
using namespace AssetDocumentNiagaraTests;

inline const TCHAR* const SystemStateScript = TEXT("/Niagara/Modules/System/SystemState.SystemState");

// A system with the Fountain emitter and SystemState listing Inputs (a JSON object text).
inline FString StateDocument(const FTarget& Target, const TCHAR* Action, const TCHAR* Inputs)
{
	return FString::Printf(TEXT(R"json({
  "SchemaVersion": 1, "Target": "%s", "Class": "/Script/Niagara.NiagaraSystem", "Action": "%s", "Definitions": {},
  "Body": {
    "SystemUpdate": [{"Name": "SystemState", "Script": "%s", "Inputs": %s}],
    "Emitters": [{"Name": "Jet", "From": "/Niagara/DefaultAssets/Templates/Emitters/Fountain"}]
  }
})json"), *Target.PackageName, Action, SystemStateScript, Inputs);
}

inline TSharedPtr<FJsonObject> ModuleEntry(const TSharedPtr<FJsonObject>& Document, const FString& Emitter, const FString& Stack, const FString& Module)
{
	const TSharedPtr<FJsonObject> Body = Child(Document, TEXT("Body"));
	const TSharedPtr<FJsonObject> Owner = Emitter.IsEmpty() ? Body : Named(List(Body, TEXT("Emitters")), Emitter);
	return Named(List(Owner, *Stack), Module);
}
} // namespace AssetDocumentNiagaraVerificationTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraVerificationPartialStructsTest, "UEAssetDoc.Niagara.Verification.PartialStructs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraVerificationPartialStructsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraTests;
	const FTarget Target(TEXT("NS_Partial"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
	};
	const FString Json = FString::Printf(TEXT(R"json({
  "SchemaVersion": 1, "Target": "%s", "Class": "/Script/Niagara.NiagaraSystem", "Action": "Create", "Definitions": {},
  "Body": {
    "UserParameters": [
      {"Name": "User.Falloff", "Type": "/Script/Niagara.NiagaraDataInterfaceCurve",
       "Value": {"DataInterface": {"Class": "Curve", "Properties": {"Curve": {"Keys": [{"Time": 0.0, "Value": 1.0}, {"Time": 1.0, "Value": 0.0, "InterpMode": "RCIM_Constant"}]}}}}}
    ],
    "Emitters": [
      {"Name": "Jet", "From": "/Niagara/DefaultAssets/Templates/Emitters/Fountain",
       "Renderers": [
         {"Name": "Shards", "Class": "MeshRenderer", "Properties": {
            "Meshes": [{"Mesh": "/Engine/BasicShapes/Cube.Cube"}, {"Mesh": "/Engine/BasicShapes/Sphere.Sphere", "Scale": {"X": 2.0, "Y": 2.0, "Z": 2.0}}],
            "bOverrideMaterials": true,
            "OverrideMaterials": [{"ExplicitMat": "/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"}]}}
       ]}
    ]
  }
})json"), *Target.PackageName);
	const FAssetDocumentResult Created = Apply(*this, Target, Json, TEXT("create"));
	if (!TestTrue(TEXT("partial struct elements verify"), Created.IsSuccess()))
	{
		LogResidue(*this, Target, TEXT("partial"));
		return false;
	}
	const TSharedPtr<FJsonObject> Document = Extract(*this, Target, TEXT("created"));
	const TSharedPtr<FJsonObject> Jet = Named(List(Child(Document, TEXT("Body")), TEXT("Emitters")), TEXT("Jet"));
	const TSharedPtr<FJsonObject> Shards = Named(List(Jet, TEXT("Renderers")), TEXT("Shards"));
	AddInfo(FString::Printf(TEXT("extracted mesh renderer: %s"), *Serialize(Shards).Left(3000)));
	AddInfo(FString::Printf(TEXT("extracted user parameters: %s"), *DocumentAt(Document, TEXT("/Body/UserParameters")).Left(3000)));
	AddInfo(FString::Printf(TEXT("extracted particle update: %s"), *DocumentAt(Document, TEXT("/Body/Emitters/0/ParticleUpdate")).Left(6000)));
	TestNoOpReapply(*this, Target, TEXT("partial"));
	// The same document again: nothing changes.
	const FAssetDocumentResult Again = Apply(*this, Target, Json.Replace(TEXT("\"Action\": \"Create\""), TEXT("\"Action\": \"Update\"")), TEXT("same document"));
	TestTrue(TEXT("the same partial document applies again"), Again.IsSuccess());
	TestFalse(TEXT("the same partial document changes nothing"), Again.bSavedAsset);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraVerificationEnumShortNameTest, "UEAssetDoc.Niagara.Verification.EnumShortName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraVerificationEnumShortNameTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraVerificationTests;
	const FTarget Target(TEXT("NS_EnumShort"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
	};
	const FString Json = StateDocument(Target, TEXT("Create"), TEXT(R"json({"Inactive Response": "Kill"})json"));
	if (!TestTrue(TEXT("the short enum name applies"), Apply(*this, Target, Json, TEXT("create")).IsSuccess()))
	{
		LogResidue(*this, Target, TEXT("short name"));
		return false;
	}
	const TSharedPtr<FJsonObject> State = ModuleEntry(Extract(*this, Target, TEXT("created")), FString(), TEXT("SystemUpdate"), TEXT("SystemState"));
	const TSharedPtr<FJsonObject> Inputs = Child(State, TEXT("Inputs"));
	FString Written;
	TestTrue(TEXT("the input reads back by its full name"), Inputs.IsValid() && Inputs->TryGetStringField(TEXT("Inactive Response"), Written)
		&& Written.StartsWith(TEXT("Kill (")));
	AddInfo(FString::Printf(TEXT("read back: %s"), *Written));
	const FAssetDocumentResult Again = Apply(*this, Target, Json.Replace(TEXT("\"Action\": \"Create\""), TEXT("\"Action\": \"Update\"")), TEXT("again"));
	TestTrue(TEXT("the short-name document applies again"), Again.IsSuccess());
	TestFalse(TEXT("and changes nothing"), Again.bSavedAsset);
	TestNoOpReapply(*this, Target, TEXT("short name"));
	const FAssetDocumentResult Unknown = Apply(*this, Target,
		StateDocument(Target, TEXT("Update"), TEXT(R"json({"Inactive Response": "Kil"})json")), TEXT("unknown name"));
	TestFalse(TEXT("a name no value has is refused"), Unknown.IsSuccess());
	TestTrue(TEXT("the refusal lists the values"), Unknown.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == TEXT("Niagara.InvalidInputValue") && Diagnostic.Message.Contains(TEXT("Kill ("));
	}));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraVerificationAllDefaultInputsTest, "UEAssetDoc.Niagara.Verification.AllDefaultInputs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraVerificationAllDefaultInputsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraVerificationTests;
	const FTarget Target(TEXT("NS_AllDefaults"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
	};
	if (!TestTrue(TEXT("create"), Apply(*this, Target, StateDocument(Target, TEXT("Create"), TEXT("{}")), TEXT("create")).IsSuccess()))
	{
		return false;
	}
	// Two inputs at their default value (as inspect shows them), in two different modules.
	const TSharedPtr<FJsonObject> Inspected = Inspect(*this, Target.ObjectPath, TEXT("inspect"));
	const TSharedPtr<FJsonObject> Document = Extract(*this, Target, TEXT("extract"));
	if (!Inspected.IsValid() || !Document.IsValid())
	{
		return false;
	}
	Document->SetStringField(TEXT("Action"), TEXT("Update"));
	TArray<FString> Chosen;
	TSet<FString> Modules;
	for (const TSharedPtr<FJsonValue>& ModuleValue : List(Child(Inspected, TEXT("NiagaraDetails")), TEXT("Modules")))
	{
		const TSharedPtr<FJsonObject> Module = ModuleValue->AsObject();
		const FString Name = Module->GetStringField(TEXT("Module"));
		if (Chosen.Num() >= 2 || Modules.Contains(Name) || Module->HasField(TEXT("Event")) || Module->HasField(TEXT("SimulationStage")))
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Entry = ModuleEntry(Document, Module->GetStringField(TEXT("Emitter")), Module->GetStringField(TEXT("Stack")), Name);
		for (const TSharedPtr<FJsonValue>& InputValue : List(Module, TEXT("Inputs")))
		{
			const TSharedPtr<FJsonObject> Input = InputValue->AsObject();
			const TSharedPtr<FJsonValue> Value = Input->TryGetField(TEXT("Value"));
			if (!Entry.IsValid() || Input->GetBoolField(TEXT("Overridden")) || !Input->GetBoolField(TEXT("Visible"))
				|| Input->GetStringField(TEXT("Mode")) != TEXT("Local") || !Value.IsValid() || Value->Type == EJson::Object)
			{
				continue;
			}
			TSharedPtr<FJsonObject> Inputs = Child(Entry, TEXT("Inputs"));
			if (!Inputs.IsValid())
			{
				Inputs = MakeShared<FJsonObject>();
				Entry->SetObjectField(TEXT("Inputs"), Inputs);
			}
			Inputs->SetField(Input->GetStringField(TEXT("Name")), Value);
			Chosen.Add(FString::Printf(TEXT("'%s' of '%s'"), *Input->GetStringField(TEXT("Name")), *Name));
			Modules.Add(Name);
			break;
		}
	}
	if (!TestEqual(TEXT("two default-valued inputs in two modules"), Chosen.Num(), 2))
	{
		return false;
	}
	const FAssetDocumentResult Result = ApplyDocument(*this, Target, Document, TEXT("defaults"));
	TestFalse(TEXT("default-valued inputs are refused"), Result.IsSuccess());
	const FAssetDocumentDiagnostic* Refusal = Result.Diagnostics.FindByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == TEXT("Niagara.InputIsDefault");
	});
	if (TestNotNull(TEXT("Niagara.InputIsDefault"), Refusal))
	{
		AddInfo(Refusal->Message);
		for (const FString& Label : Chosen)
		{
			TestTrue(*FString::Printf(TEXT("the one refusal names %s"), *Label), Refusal->Message.Contains(Label));
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraVerificationLifecycleTest, "UEAssetDoc.Niagara.Verification.Lifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraVerificationLifecycleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraVerificationTests;
	const FTarget Target(TEXT("NS_Lifecycle"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
	};
	auto SystemLoop = [this, &Target](const TCHAR* Stage) -> TSharedPtr<FJsonObject>
	{
		const TSharedPtr<FJsonObject> Inspected = Inspect(*this, Target.ObjectPath, Stage);
		for (const TSharedPtr<FJsonValue>& Value : List(Child(Inspected, TEXT("NiagaraDetails")), TEXT("Lifecycle")))
		{
			const TSharedPtr<FJsonObject> Fact = Value->AsObject();
			if (Fact.IsValid() && Fact->GetStringField(TEXT("Emitter")).IsEmpty() && Fact->GetStringField(TEXT("Module")) == TEXT("SystemState"))
			{
				AddInfo(FString::Printf(TEXT("%s: %s"), Stage, *Serialize(Fact)));
				return Fact;
			}
		}
		return nullptr;
	};
	if (!TestTrue(TEXT("create"), Apply(*this, Target, StateDocument(Target, TEXT("Create"), TEXT("{}")), TEXT("create")).IsSuccess()))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Default = SystemLoop(TEXT("default"));
	if (TestNotNull(TEXT("inspect has the system's lifecycle"), Default.Get()))
	{
		TestTrue(TEXT("a new system loops"), Default->GetBoolField(TEXT("Loops")));
		const TSharedPtr<FJsonObject> Behavior = Child(Default, TEXT("Loop Behavior"));
		TestTrue(TEXT("its Loop Behavior is the default"), Behavior.IsValid() && Behavior->GetBoolField(TEXT("Default")));
	}
	TestFalse(TEXT("the document does not list the default Loop Behavior"),
		Child(ModuleEntry(Extract(*this, Target, TEXT("default")), FString(), TEXT("SystemUpdate"), TEXT("SystemState")), TEXT("Inputs")).IsValid());
	if (!TestTrue(TEXT("Loop Behavior Once applies"), Apply(*this, Target,
		StateDocument(Target, TEXT("Update"), TEXT(R"json({"Loop Behavior": "Once"})json")), TEXT("once")).IsSuccess()))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Once = SystemLoop(TEXT("once"));
	TestTrue(TEXT("a system set to Once does not loop"), Once.IsValid() && !Once->GetBoolField(TEXT("Loops")));
	return !HasAnyErrors();
}
#endif
