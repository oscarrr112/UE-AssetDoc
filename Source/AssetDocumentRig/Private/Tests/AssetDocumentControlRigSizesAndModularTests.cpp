// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Control Rig extract sizes, refused calls and a modular biped created in one apply.
// UEAssetDoc.ControlRig.ExtractSizes: extract and list_types sizes (bounds, and the parts they are made
//   of); the sparse extract diffs clean against its rig.
// UEAssetDoc.ControlRig.RefusedCalls: calls that were refused or read wrong: FactoryContext
//   as a package path; a sparse Update of a modular rig without RigType; a module Parent cycle; a function rename in Diff.
// UEAssetDoc.ControlRig.ModularOneApply: a biped from one document in one apply (every connection
//   listed): equals the rig the Module Assembly panel's drops build; re-apply no-op; Inspect lists the controls the
//   modules make, and its AutoConnect suggestion of a clavicle Elbow Bone comes with Check.

#include "AssetDocumentIKRigTestSupport.h"

#include "ControlRig.h"
#include "ControlRigBlueprintLegacy.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ModularRig.h"
#include "ModularRigController.h"
#include "ModularRigModel.h"
#include "ModularRigRuleManager.h"
#include "Rigs/RigHierarchy.h"
#include "Units/Execution/RigUnit_PrepareForExecution.h"
#include "Utils/AssetDocumentJsonKeys.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentControlRigTests
{
struct FTemplateContent;
TSharedPtr<FTemplateContent> ShareCorpusTemplates();
UControlRigBlueprint* MakeControlRig(FAutomationTestBase& Test, const AssetDocumentIKRigTests::FTarget& Target, const TCHAR* MeshPath, bool bModular);
} // namespace AssetDocumentControlRigTests

namespace AssetDocumentControlRigSizesAndModularTests
{
using namespace AssetDocumentIKRigTests;

int32 SizeOf(const TSharedPtr<FJsonValue>& Value)
{
	TArray<TSharedPtr<FJsonValue>> One;
	One.Add(Value);
	FString Text;
	const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	FJsonSerializer::Serialize(One, Writer);
	return Text.Len() - 2;
}

void AddField(TMap<FString, int64>& Sizes, const FString& Key, const TSharedPtr<FJsonValue>& Value)
{
	Sizes.FindOrAdd(Key) += SizeOf(Value) + Key.Len() + 4;
}

FString Table(const TMap<FString, int64>& Sizes, int32 Top = 24)
{
	TArray<TPair<FString, int64>> Rows;
	for (const TPair<FString, int64>& Row : Sizes)
	{
		Rows.Add(Row);
	}
	Rows.Sort([](const TPair<FString, int64>& A, const TPair<FString, int64>& B) { return A.Value > B.Value; });
	TArray<FString> Lines;
	for (int32 Index = 0; Index < Rows.Num() && Index < Top; ++Index)
	{
		Lines.Add(FString::Printf(TEXT("%s=%lld"), *Rows[Index].Key, Rows[Index].Value));
	}
	return FString::Join(Lines, TEXT(", "));
}

// Per node field over the nodes of a container (and its collapse graphs); per default pin; container fields.
void NodeSizes(const TSharedPtr<FJsonObject>& Container, TMap<FString, int64>& Fields, TMap<FString, int64>& Defaults,
	TMap<FString, int64>& ContainerFields, int32& Nodes)
{
	for (const auto& Field : Container->Values)
	{
		if (Field.Key != TEXT("Nodes"))
		{
			AddField(ContainerFields, FString(*Field.Key), Field.Value);
		}
	}
	const TSharedPtr<FJsonObject>* NodesObject = nullptr;
	if (!Container->TryGetObjectField(TEXT("Nodes"), NodesObject))
	{
		return;
	}
	for (const auto& Node : (*NodesObject)->Values)
	{
		++Nodes;
		Fields.FindOrAdd(TEXT("<names>")) += Node.Key.Len() + 4;
		for (const auto& Field : Node.Value->AsObject()->Values)
		{
			if (Field.Key == TEXT("Collapse") && Field.Value->Type == EJson::Object)
			{
				NodeSizes(Field.Value->AsObject(), Fields, Defaults, ContainerFields, Nodes);
				continue;
			}
			AddField(Fields, FString(*Field.Key), Field.Value);
			if (Field.Key == TEXT("Defaults") && Field.Value->Type == EJson::Object)
			{
				for (const auto& Pin : Field.Value->AsObject()->Values)
				{
					FString Leaf = FString(*Pin.Key);
					Leaf.Split(TEXT("."), &Leaf, nullptr);
					AddField(Defaults, Leaf, Pin.Value);
				}
			}
		}
	}
}

// The parts of a document, in condensed characters.
void Breakdown(FAutomationTestBase& Test, const FString& Label, const TSharedPtr<FJsonObject>& Document)
{
	TMap<FString, int64> Top;
	for (const auto& Field : Document->Values)
	{
		if (Field.Key != TEXT("Body"))
		{
			AddField(Top, FString(*Field.Key), Field.Value);
		}
	}
	const TSharedPtr<FJsonObject> Body = BodyOf(Document);
	for (const auto& Field : Body->Values)
	{
		AddField(Top, TEXT("Body.") + FString(*Field.Key), Field.Value);
	}
	TMap<FString, int64> Fields;
	TMap<FString, int64> Defaults;
	TMap<FString, int64> ContainerFields;
	int32 Nodes = 0;
	for (const TCHAR* Section : {TEXT("Graphs"), TEXT("Functions")})
	{
		const TSharedPtr<FJsonObject>* Containers = nullptr;
		if (Body->TryGetObjectField(Section, Containers))
		{
			for (const auto& Container : (*Containers)->Values)
			{
				NodeSizes(Container.Value->AsObject(), Fields, Defaults, ContainerFields, Nodes);
			}
		}
	}
	TMap<FString, int64> Elements;
	TMap<FString, int64> ElementTypes;
	int32 TransformsOnImported = 0;
	int32 Imported = 0;
	const TArray<TSharedPtr<FJsonValue>>* Hierarchy = nullptr;
	if (Body->TryGetArrayField(TEXT("Hierarchy"), Hierarchy))
	{
		for (const TSharedPtr<FJsonValue>& Element : *Hierarchy)
		{
			const TSharedPtr<FJsonObject> Entry = Element->AsObject();
			ElementTypes.FindOrAdd(Entry->GetStringField(TEXT("Type"))) += SizeOf(Element);
			bool bImported = false;
			Entry->TryGetBoolField(TEXT("Imported"), bImported);
			Imported += bImported ? 1 : 0;
			TransformsOnImported += bImported && Entry->HasField(TEXT("Transform")) ? 1 : 0;
			for (const auto& Field : Entry->Values)
			{
				AddField(Elements, FString(*Field.Key), Field.Value);
				if (Field.Key == TEXT("Settings") && Field.Value->Type == EJson::Object)
				{
					for (const auto& Setting : Field.Value->AsObject()->Values)
					{
						AddField(Elements, TEXT("Settings.") + FString(*Setting.Key), Setting.Value);
					}
				}
			}
		}
	}
	Test.AddInfo(FString::Printf(TEXT("SIZE %s: condensed %d, pretty %d; parts: %s"), *Label, Serialize(Document).Len(),
		SerializePretty(Document).Len(), *Table(Top)));
	Test.AddInfo(FString::Printf(TEXT("SIZE %s: %d nodes; node fields: %s"), *Label, Nodes, *Table(Fields)));
	Test.AddInfo(FString::Printf(TEXT("SIZE %s: default pins: %s"), *Label, *Table(Defaults, 30)));
	Test.AddInfo(FString::Printf(TEXT("SIZE %s: function / graph fields: %s"), *Label, *Table(ContainerFields)));
	Test.AddInfo(FString::Printf(TEXT("SIZE %s: %d elements (%d imported, %d of them with Transform); element fields: %s; by type: %s"), *Label,
		Hierarchy != nullptr ? Hierarchy->Num() : 0, Imported, TransformsOnImported, *Table(Elements, 40), *Table(ElementTypes)));
}

FAssetDocumentResult ListTypes(const TCHAR* Kind, const TMap<FString, FString>& Extra = {})
{
	TMap<FString, FString> Query = Extra;
	Query.Add(TEXT("kind"), Kind);
	Query.Add(TEXT("limit"), TEXT("500"));
	return FAssetDocumentModule::Get().GetService().ListTypes(Query);
}

UModularRig* ConstructPreview(UControlRigBlueprint& Blueprint)
{
	if (Blueprint.UBlueprint::GetObjectBeingDebugged() == nullptr)
	{
		Blueprint.RecompileModularRig();
	}
	UModularRig* Debugged = Cast<UModularRig>(Blueprint.UBlueprint::GetObjectBeingDebugged());
	if (Debugged != nullptr)
	{
		Debugged->RequestConstruction();
		Debugged->Execute(FRigUnit_PrepareForExecution::EventName);
	}
	return Debugged;
}

FString StateName(ERigElementResolveState State)
{
	return StaticEnum<ERigElementResolveState>()->GetNameStringByValue(static_cast<int64>(State));
}

// Every connector of Module with its matches (state) as the rule manager gives them now.
FString DescribeMatches(UControlRigBlueprint& Blueprint, const FName Module)
{
	UModularRig* Debugged = ConstructPreview(Blueprint);
	const FRigModuleReference* Reference = Blueprint.GetModularRigController()->FindModule(Module);
	URigHierarchy* Hierarchy = Debugged != nullptr ? Debugged->GetHierarchy() : nullptr;
	if (Reference == nullptr || Hierarchy == nullptr)
	{
		return TEXT("-");
	}
	TArray<FRigElementKey> Keys;
	for (const FRigConnectorElement* Connector : Reference->FindConnectors(Hierarchy))
	{
		Keys.Add(Connector->GetKey());
	}
	TArray<FString> Lines;
	for (const FRigElementKey& Key : Keys)
	{
		const FRigConnectorElement* Connector = Hierarchy->Find<FRigConnectorElement>(Key);
		if (Connector == nullptr)
		{
			continue;
		}
		const FModularRigResolveResult Result = Hierarchy->GetRuleManager()->FindMatches(Connector, Debugged->FindModule(Module), Debugged->GetElementKeyRedirector());
		TArray<FString> Matches;
		for (const FRigElementResolveResult& Match : Result.GetMatches())
		{
			Matches.Add(FString::Printf(TEXT("%s%s"), *Match.GetKey().ToString(), Match.GetState() == ERigElementResolveState::DefaultTarget ? TEXT("*") : TEXT("")));
		}
		const TArray<FRigElementKey> Connected = Blueprint.GetModularRigModel().Connections.FindTargetsFromConnector(Key);
		Lines.Add(FString::Printf(TEXT("%s [%s%s%s] -> %s; matches %d: %s"), *Key.Name.ToString(), Connector->IsPrimary() ? TEXT("primary") : TEXT("secondary"),
			Connector->IsOptional() ? TEXT(",optional") : TEXT(""), Connector->IsArrayConnector() ? TEXT(",array") : TEXT(""),
			*FString::JoinBy(Connected, TEXT(","), [](const FRigElementKey& K) { return K.ToString(); }), Matches.Num(),
			*FString::Join(Matches, TEXT(" ")).Left(600)));
	}
	return FString::Join(Lines, TEXT("\n"));
}

// A drop of Module on the primary match whose name contains Hint, with auto-resolve (the Module Assembly panel).
bool Drop(FAutomationTestBase& Test, UControlRigBlueprint& Blueprint, const FName Module, const TCHAR* Hint)
{
	UModularRigController* Controller = Blueprint.GetModularRigController();
	UModularRig* Debugged = ConstructPreview(Blueprint);
	const FRigModuleReference* Reference = Controller->FindModule(Module);
	URigHierarchy* Hierarchy = Debugged != nullptr ? Debugged->GetHierarchy() : nullptr;
	const FRigConnectorElement* Primary = Reference != nullptr && Hierarchy != nullptr ? Reference->FindPrimaryConnector(Hierarchy) : nullptr;
	if (Primary == nullptr)
	{
		Test.AddError(FString::Printf(TEXT("%s: no primary connector"), *Module.ToString()));
		return false;
	}
	const FRigElementKey PrimaryKey = Primary->GetKey();
	const FModularRigResolveResult Result = Hierarchy->GetRuleManager()->FindMatches(Primary, Debugged->FindModule(Module), Debugged->GetElementKeyRedirector());
	FRigElementKey Pick;
	for (const FRigElementResolveResult& Match : Result.GetMatches())
	{
		if (!Pick.IsValid() && Match.GetKey().Name.ToString().Contains(Hint))
		{
			Pick = Match.GetKey();
		}
	}
	if (!Pick.IsValid())
	{
		Test.AddError(FString::Printf(TEXT("%s: no primary match containing %s"), *Module.ToString(), Hint));
		return false;
	}
	return Controller->ConnectConnectorToElement(PrimaryKey, Pick, false, true, true);
}

const TCHAR* const ControlRigClassPath = TEXT("/Script/ControlRigDeveloper.ControlRigBlueprint");
const TCHAR* const BipedMesh = TEXT("/ControlRigModules/Modules/Meshes/SKM_Biped_Template.SKM_Biped_Template");

TSharedPtr<FJsonObject> Json(const FString& Text)
{
	return Parse(Text);
}

TSharedPtr<FJsonObject> Document(const FTarget& Target, const TCHAR* Action, const FString& Body)
{
	const TSharedPtr<FJsonObject> Made = Json(TEXT("{\"SchemaVersion\": 1, \"Properties\": {}}"));
	Made->SetStringField(TEXT("Class"), ControlRigClassPath);
	Made->SetStringField(TEXT("Target"), Target.PackageName);
	Made->SetStringField(TEXT("Action"), Action);
	Made->SetObjectField(TEXT("Body"), Json(Body));
	return Made;
}

FAssetDocumentResult Validate(const TSharedPtr<FJsonObject>& Made)
{
	FAssetDocumentValidateRequest Request;
	Request.Document = Made;
	return FAssetDocumentModule::Get().GetService().Validate(Request);
}

bool Applied(FAutomationTestBase& Test, const FTarget& Target, const TSharedPtr<FJsonObject>& Made, const FString& Stage,
	FAssetDocumentResult* OutResult = nullptr)
{
	const FAssetDocumentResult Result = Apply(Target, Made);
	if (OutResult != nullptr)
	{
		*OutResult = Result;
	}
	Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *DiagnosticsText(Result).Left(4000)));
	return Test.TestTrue(*FString::Printf(TEXT("%s applies"), *Stage), Result.IsSuccess());
}

// A document as a rig's state (to compare two rigs): sorted keys, without Target, Action and the variant's Guid.
TSharedPtr<FJsonValue> Sorted(const TSharedPtr<FJsonValue>& Value)
{
	if (Value->Type == EJson::Object)
	{
		TArray<FString> Keys;
		AssetDocumentServerJson::GetKeys(*Value->AsObject(), Keys);
		Keys.Sort();
		const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		for (const FString& Key : Keys)
		{
			Object->SetField(Key, Sorted(AssetDocumentServerJson::FindFieldRef(*Value->AsObject(), Key)));
		}
		return MakeShared<FJsonValueObject>(Object);
	}
	if (Value->Type == EJson::Array)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
		{
			Items.Add(Sorted(Item));
		}
		return MakeShared<FJsonValueArray>(Items);
	}
	return Value;
}

FString Comparable(const TSharedPtr<FJsonObject>& Made)
{
	if (!Made.IsValid())
	{
		return FString();
	}
	const TSharedPtr<FJsonObject> Copy = Parse(Serialize(Made));
	Copy->RemoveField(TEXT("Target"));
	Copy->RemoveField(TEXT("Action"));
	const TSharedPtr<FJsonObject>* Properties = nullptr;
	const TSharedPtr<FJsonObject>* Variant = nullptr;
	if (Copy->TryGetObjectField(TEXT("Properties"), Properties) && (*Properties)->TryGetObjectField(TEXT("AssetVariant"), Variant))
	{
		(*Variant)->RemoveField(TEXT("Guid"));
	}
	return Serialize(Sorted(MakeShared<FJsonValueObject>(Copy))->AsObject());
}

const struct FDrop { const TCHAR* Name; const TCHAR* Class; const TCHAR* Parent; const TCHAR* Hint; } BipedDrops[] = {
	{TEXT("Root"), nullptr, nullptr, TEXT("root")},
	{TEXT("Spine"), TEXT("Spine"), TEXT("Root"), TEXT("spine_01_socket")},
	{TEXT("Neck"), TEXT("Neck"), TEXT("Spine"), TEXT("neck")},
	{TEXT("Arm_l"), TEXT("Arm"), TEXT("Spine"), TEXT("shoulder_l")},
	{TEXT("Arm_r"), TEXT("Arm"), TEXT("Spine"), TEXT("shoulder_r")},
	{TEXT("Leg_l"), TEXT("Leg"), TEXT("Spine"), TEXT("thigh_l")},
	{TEXT("Leg_r"), TEXT("Leg"), TEXT("Spine"), TEXT("thigh_r")},
	{TEXT("Foot_l"), TEXT("Foot"), TEXT("Leg_l"), TEXT("Leg_l/")},
};
} // namespace AssetDocumentControlRigSizesAndModularTests

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigExtractSizesTest, "UEAssetDoc.ControlRig.ExtractSizes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigExtractSizesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentControlRigSizesAndModularTests;
	// The MoverExamples Body rig has stale function references (its compile messages are the asset's).
	AssetDocumentIKRigTests::ExpectStalePinErrors(*this, -1);
	AssetDocumentIKRigTests::ExpectUnmappedVariableErrors(*this, -1);
	const TSharedPtr<AssetDocumentControlRigTests::FTemplateContent> Templates = AssetDocumentControlRigTests::ShareCorpusTemplates();
	FFixtures Fixtures;
	// Sizes of diff_only extracts of condensed characters: CR_Mannequin_Body 511907 with 306 of 342 imported bones
	// carrying Transform, PinTypes 38903; CR_Mannequin_BasicFootIK 56930 with 156 of 174; MoverExamples
	// CR_Mannequin_Body 428485; with the PinTypes limited to the pins that decide the others: 492123 (PinTypes 19119); 56876; 411564. These rigs
	// have no SourceHierarchyImport, so their imported bones keep Transform (their stored pose is what they run on).
	const struct { const TCHAR* Path; int32 MaxSize; int32 MaxImportedTransforms; } Rigs[] = {
		{TEXT("/Game/Characters/Mannequins/Rigs/CR_Mannequin_Body.CR_Mannequin_Body"), 495000, 306},
		{TEXT("/MoverExamples/Characters/Mannequins/Rigs/CR_Mannequin_BasicFootIK.CR_Mannequin_BasicFootIK"), 57500, 156},
		{TEXT("/MoverExamples/Characters/Mannequins/Rigs/CR_Mannequin_Body.CR_Mannequin_Body"), 415000, 145},
	};
	for (const auto& Rig : Rigs)
	{
		UObject* Source = LoadQuiet(Rig.Path);
		if (Source == nullptr)
		{
			AddInfo(FString::Printf(TEXT("SKIP %s"), Rig.Path));
			continue;
		}
		const FTarget& Copy = Fixtures.Add(TEXT("CR_Size"));
		if (Duplicate(*this, Source, Copy) == nullptr)
		{
			continue;
		}
		FAssetDocumentExtractRequest Request;
		Request.AssetPath = Copy.ObjectPath;
		Request.bDiffOnly = true;
		const FAssetDocumentResult Extracted = FAssetDocumentModule::Get().GetService().Extract(Request);
		if (!TestTrue(*FString::Printf(TEXT("%s extracts"), Rig.Path), Extracted.IsSuccess() && Extracted.Payload.IsValid()))
		{
			continue;
		}
		const FString Label = FPackageName::ObjectPathToObjectName(FString(Rig.Path));
		Breakdown(*this, Label, Extracted.Payload);
		const int32 Size = Serialize(Extracted.Payload).Len();
		int32 ImportedTransforms = 0;
		for (const TSharedPtr<FJsonValue>& Element : BodyOf(Extracted.Payload)->GetArrayField(TEXT("Hierarchy")))
		{
			bool bImported = false;
			ImportedTransforms += Element->AsObject()->TryGetBoolField(TEXT("Imported"), bImported) && bImported
				&& Element->AsObject()->HasField(TEXT("Transform")) ? 1 : 0;
		}
		TestTrue(*FString::Printf(TEXT("%s: %d characters (at most %d)"), Rig.Path, Size, Rig.MaxSize), Size <= Rig.MaxSize);
		TestTrue(*FString::Printf(TEXT("%s: %d imported bones with Transform (at most %d)"), Rig.Path, ImportedTransforms, Rig.MaxImportedTransforms),
			ImportedTransforms <= Rig.MaxImportedTransforms);
		// The diff_only document diffs clean against its rig (what it leaves out reads as the rig's).
		const TSharedPtr<FJsonObject> Again = Parse(Serialize(Extracted.Payload));
		Again->SetStringField(TEXT("Action"), TEXT("Update"));
		FString Text;
		TestEqual(*FString::Printf(TEXT("%s: the extract diffs clean"), Rig.Path), DiffCount(Again, Text), 0);
		if (Text.Contains(TEXT("\"path\"")))
		{
			AddInfo(Text.Left(2000));
		}
	}
	// The descriptions were 58.7 KB (pretty); a compact row per module, details on request.
	const FAssetDocumentResult Modules = ListTypes(TEXT("controlrig_module"));
	if (TestTrue(TEXT("list_types controlrig_module"), Modules.IsSuccess() && Modules.Payload.IsValid()))
	{
		const int32 Size = SerializePretty(Modules.Payload).Len();
		AddInfo(FString::Printf(TEXT("SIZE list_types controlrig_module: condensed %d, pretty %d"), Serialize(Modules.Payload).Len(), Size));
		TestTrue(*FString::Printf(TEXT("list_types controlrig_module: %d characters pretty (at most 20000)"), Size), Size <= 20000);
		const FAssetDocumentResult Detailed = ListTypes(TEXT("controlrig_module"), {{TEXT("detail"), TEXT("true")}});
		AddInfo(FString::Printf(TEXT("SIZE list_types controlrig_module detail=true: pretty %d"), Detailed.Payload.IsValid() ? SerializePretty(Detailed.Payload).Len() : 0));
		TestTrue(TEXT("detail=true has the descriptions"), Detailed.Payload.IsValid() && Serialize(Detailed.Payload).Contains(TEXT("\"Description\"")));
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigAutoConnectProbeTest, "UEAssetDoc.ControlRig.AutoConnectProbe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigAutoConnectProbeTest::RunTest(const FString& Parameters)
{
	const AssetDocumentIKRigTests::FModules56RootScope Modules56Root;
	using namespace AssetDocumentControlRigSizesAndModularTests;
	AddExpectedMessagePlain(TEXT("out of bounds (count 0)"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, -1);
	FFixtures Fixtures;
	for (const TCHAR* Mesh : {TEXT("/MoverExamples/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple"),
			 TEXT("/ControlRigModules/Modules/Meshes/SKM_Biped_Template.SKM_Biped_Template")})
	{
		if (LoadQuiet(Mesh) == nullptr)
		{
			continue;
		}
		const FTarget& Target = Fixtures.Add(TEXT("CR_Probe"));
		UControlRigBlueprint* Blueprint = AssetDocumentControlRigTests::MakeControlRig(*this, Target, Mesh, true);
		if (Blueprint == nullptr)
		{
			continue;
		}
		UModularRigController* Controller = Blueprint->GetModularRigController();
		const struct { const TCHAR* Name; const TCHAR* Class; const TCHAR* Parent; const TCHAR* Hint; } Plan[] = {
			{TEXT("Root"), nullptr, nullptr, TEXT("root")},
			{TEXT("Spine"), TEXT("Spine"), TEXT("Root"), TEXT("spine_01_socket")},
			{TEXT("Neck"), TEXT("Neck"), TEXT("Spine"), TEXT("neck")},
			{TEXT("Arm_l"), TEXT("Arm"), TEXT("Spine"), TEXT("shoulder_l")},
			{TEXT("Shoulder_r"), TEXT("Shoulder"), TEXT("Spine"), TEXT("shoulder_r")},
			{TEXT("Arm_r"), TEXT("Arm"), TEXT("Shoulder_r"), TEXT("Shoulder_r/")},
			{TEXT("Leg_l"), TEXT("Leg"), TEXT("Spine"), TEXT("thigh_l")},
		};
		for (const auto& Step : Plan)
		{
			if (Step.Class != nullptr)
			{
				const FString Asset = FString::Printf(TEXT("/ControlRigModules/Modules56/%s.%s"), Step.Class, Step.Class);
				const UControlRigBlueprint* Module = LoadObject<UControlRigBlueprint>(nullptr, *Asset);
				if (Module == nullptr || Controller->AddModule(Step.Name, Module->GeneratedClass.Get(), Step.Parent, false) != FName(Step.Name))
				{
					AddError(FString::Printf(TEXT("add %s"), Step.Name));
					continue;
				}
			}
			AddInfo(FString::Printf(TEXT("PROBE %s before drop of %s:\n%s"), Mesh, Step.Name, *DescribeMatches(*Blueprint, Step.Name)));
			Drop(*this, *Blueprint, Step.Name, Step.Hint);
			AddInfo(FString::Printf(TEXT("PROBE %s after drop of %s (auto-resolve):\n%s"), Mesh, Step.Name, *DescribeMatches(*Blueprint, Step.Name)));
		}
		if (UModularRig* Debugged = ConstructPreview(*Blueprint))
		{
			TArray<FString> Lines;
			URigHierarchy* Hierarchy = Debugged->GetHierarchy();
			for (const FRigElementKey& Key : Hierarchy->GetAllKeys(true))
			{
				if (!Hierarchy->GetModuleFName(Key).IsNone())
				{
					const FVector T = Hierarchy->GetGlobalTransform(Key, true).GetTranslation();
					Lines.Add(FString::Printf(TEXT("%s (%.1f %.1f %.1f) parent %s"), *Key.ToString(), T.X, T.Y, T.Z, *Hierarchy->GetFirstParent(Key).ToString()));
				}
			}
			AddInfo(FString::Printf(TEXT("PROBE %s module elements %d:\n%s"), Mesh, Lines.Num(), *FString::Join(Lines, TEXT("\n")).Left(20000)));
		}
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigRefusedCallsTest, "UEAssetDoc.ControlRig.RefusedCalls",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigRefusedCallsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentControlRigSizesAndModularTests;
	FFixtures Fixtures;
	// 1. FactoryContext as the mesh's package path (it was refused: "the object path").
	const FTarget& OnMesh = Fixtures.Add(TEXT("CR_PackagePath"));
	{
		const TSharedPtr<FJsonObject> Create = Document(OnMesh, TEXT("Create"), TEXT("{\"Hierarchy\": [{\"Type\": \"Null\", \"Name\": \"extra\"}],"
			"\"Functions\": {\"Align\": {\"Inputs\": [{\"Name\": \"X\", \"Type\": \"int32\"}], \"Nodes\": {}}}}"));
		Create->SetStringField(TEXT("FactoryContext"), FPackageName::ObjectPathToPackageName(FString(TutorialMesh)));
		if (!Applied(*this, OnMesh, Create, TEXT("Create with FactoryContext = a package path")))
		{
			return false;
		}
		const TSharedPtr<FJsonObject> Made = Extract(*this, OnMesh.ObjectPath, TEXT("package path"));
		const TArray<TSharedPtr<FJsonValue>>* Hierarchy = nullptr;
		TestTrue(TEXT("the mesh's bones were imported"), Made.IsValid() && BodyOf(Made)->TryGetArrayField(TEXT("Hierarchy"), Hierarchy)
			&& Hierarchy->Num() > 10);
		// 2. A function renamed with RenamedFrom reads as a rename in Diff (it read as a delete and an add).
		const TSharedPtr<FJsonObject> Renaming = Parse(Serialize(Made));
		Renaming->SetStringField(TEXT("Action"), TEXT("Update"));
		const TSharedPtr<FJsonObject> Functions = BodyOf(Renaming)->GetObjectField(TEXT("Functions"));
		const TSharedPtr<FJsonObject> Function = Functions->GetObjectField(TEXT("Align"));
		Function->SetStringField(TEXT("RenamedFrom"), TEXT("Align"));
		Functions->RemoveField(TEXT("Align"));
		Functions->SetObjectField(TEXT("Align Chain"), Function);
		FString Text;
		const int32 Changes = DiffCount(Renaming, Text);
		AddInfo(FString::Printf(TEXT("rename diff: %s"), *Text.Left(2000)));
		TestEqual(TEXT("the rename is one change"), Changes, 1);
		TestTrue(TEXT("at the function's Name"), Text.Contains(TEXT("/Body/Functions/Align Chain/Name")));
		TestFalse(TEXT("not a remove of the old function"), Text.Contains(TEXT("\"/Body/Functions/Align\"")));
		if (Applied(*this, OnMesh, Renaming, TEXT("rename the function")))
		{
			const TSharedPtr<FJsonObject> Renamed = Extract(*this, OnMesh.ObjectPath, TEXT("renamed"));
			TestTrue(TEXT("renamed"), Renamed.IsValid() && BodyOf(Renamed)->GetObjectField(TEXT("Functions"))->HasField(TEXT("Align Chain")));
		}
	}
	// 3. A sparse Update of a modular rig without RigType (refused ModularRegionUnsupported: omitted meant Rig).
	const FTarget& Modular = Fixtures.Add(TEXT("CR_SparseModular"));
	const FString Modules = TEXT("[{\"Name\": \"Root\", \"Class\": \"/ControlRig/Modules/Modules56/Root.Root_C\"},"
		"{\"Name\": \"Spine\", \"Class\": \"/ControlRigModules/Modules56/Spine.Spine_C\", \"Parent\": \"Root\"},"
		"{\"Name\": \"Neck\", \"Class\": \"/ControlRigModules/Modules56/Neck.Neck_C\", \"Parent\": \"Spine\"}]");
	{
		const TSharedPtr<FJsonObject> Create = Document(Modular, TEXT("Create"), FString::Printf(TEXT("{\"RigType\": \"Modular\", \"Modules\": %s,"
			"\"Connections\": {\"Root/Root\": \"Bone(root)\", \"Spine/Spine Primary\": \"Socket(Root/spine_01_socket)\"}}"), *Modules));
		Create->SetStringField(TEXT("FactoryContext"), BipedMesh);
		if (!Applied(*this, Modular, Create, TEXT("modular create")))
		{
			return false;
		}
		const TSharedPtr<FJsonObject> Sparse = Document(Modular, TEXT("Update"),
			TEXT("{\"Connections\": {\"Root/Root\": \"Bone(root)\", \"Spine/Spine Primary\": \"Socket(Root/spine_01_socket)\"}}"));
		Sparse->RemoveField(TEXT("Properties"));
		const FAssetDocumentResult Validated = Validate(Sparse);
		TestTrue(*FString::Printf(TEXT("the sparse Update validates (%s)"), *DiagnosticsText(Validated)), Validated.IsSuccess());
		if (Applied(*this, Modular, Sparse, TEXT("sparse Update without RigType")))
		{
			const TSharedPtr<FJsonObject> After = Extract(*this, Modular.ObjectPath, TEXT("sparse"));
			TestEqual(TEXT("still a modular rig"), After.IsValid() ? BodyOf(After)->GetStringField(TEXT("RigType")) : FString(), FString(TEXT("Modular")));
		}
	}
	// Automatic resolution is an Inspect suggestion, never a document value (design decision 6).
	{
		const TSharedPtr<FJsonObject> Auto = Document(Modular, TEXT("Update"), TEXT("{\"Connections\": {\"Root/Root\": \"Bone(root)\", \"Neck/*\": \"Auto\"}}"));
		Auto->RemoveField(TEXT("Properties"));
		TestTrue(TEXT("\"Auto\" is not a connection (ControlRig.InvalidBody)"), HasCode(Validate(Auto), TEXT("ControlRig.InvalidBody")));
	}
	// 4. Spine under Neck and Neck under Spine is a cycle (it came back as ParentAfterChild).
	{
		const TSharedPtr<FJsonObject> Cycle = Document(Modular, TEXT("Update"), TEXT("{\"Modules\": [{\"Name\": \"Root\", \"Class\": \"/ControlRig/Modules/Modules56/Root.Root_C\"},"
			"{\"Name\": \"Spine\", \"Class\": \"/ControlRigModules/Modules56/Spine.Spine_C\", \"Parent\": \"Neck\"},"
			"{\"Name\": \"Neck\", \"Class\": \"/ControlRigModules/Modules56/Neck.Neck_C\", \"Parent\": \"Spine\"}]}"));
		Cycle->RemoveField(TEXT("Properties"));
		const FAssetDocumentResult Result = Validate(Cycle);
		const FString Text = DiagnosticsText(Result);
		AddInfo(Text);
		TestTrue(TEXT("ControlRig.ModuleCycle"), HasCode(Result, TEXT("ControlRig.ModuleCycle")));
		TestTrue(TEXT("the message names the cycle"), Text.Contains(TEXT("Spine -> Neck -> Spine")));
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigModularOneApplyTest, "UEAssetDoc.ControlRig.ModularOneApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigModularOneApplyTest::RunTest(const FString& Parameters)
{
	const AssetDocumentIKRigTests::FModules56RootScope Modules56Root;
	using namespace AssetDocumentControlRigSizesAndModularTests;
	FFixtures Fixtures;
	// The editor: modules dropped one after another in the Module Assembly panel (primary connector on the hinted
	// match, the others auto-resolved).
	const FTarget& EditorBuilt = Fixtures.Add(TEXT("CR_OneApplyEditor"));
	UControlRigBlueprint* Editor = AssetDocumentControlRigTests::MakeControlRig(*this, EditorBuilt, BipedMesh, true);
	if (Editor == nullptr)
	{
		return false;
	}
	Editor->GetSourceCurveImport() = Editor->GetSourceHierarchyImport();
	UModularRigController* Controller = Editor->GetModularRigController();
	FString ModulesJson;
	for (const FDrop& Step : BipedDrops)
	{
		const FString Class = Step.Class != nullptr ? FString::Printf(TEXT("/ControlRigModules/Modules56/%s.%s_C"), Step.Class, Step.Class)
			: FString(TEXT("/ControlRig/Modules/Modules56/Root.Root_C"));
		if (Step.Class != nullptr)
		{
			const UControlRigBlueprint* Module = LoadObject<UControlRigBlueprint>(nullptr, *FString::Printf(TEXT("/ControlRigModules/Modules56/%s.%s"), Step.Class, Step.Class));
			if (!TestTrue(*FString::Printf(TEXT("add %s"), Step.Name), Module != nullptr
				&& Controller->AddModule(Step.Name, Module->GeneratedClass.Get(), Step.Parent, false) == FName(Step.Name)))
			{
				return false;
			}
		}
		Drop(*this, *Editor, Step.Name, Step.Hint);
		ModulesJson += FString::Printf(TEXT("%s{\"Name\": \"%s\", \"Class\": \"%s\"%s}"), ModulesJson.IsEmpty() ? TEXT("") : TEXT(","), Step.Name, *Class,
			Step.Parent != nullptr ? *FString::Printf(TEXT(", \"Parent\": \"%s\""), Step.Parent) : TEXT(""));
	}
	FKismetEditorUtilities::CompileBlueprint(Editor);
	TestTrue(TEXT("editor rig saved"), Save(*Editor));
	// The document: the modules and every connection the drops made, listed in reverse (the apply orders them).
	const TSharedPtr<FJsonObject> Connections = MakeShared<FJsonObject>();
	const TSharedPtr<FJsonObject> PartialConnections = MakeShared<FJsonObject>();
	const TArray<FModularRigSingleConnection>& Dropped = Editor->GetModularRigModel().Connections.GetConnectionList();
	for (int32 Index = Dropped.Num() - 1; Index >= 0; --Index)
	{
		const FModularRigSingleConnection& Connection = Dropped[Index];
		TArray<TSharedPtr<FJsonValue>> Targets;
		for (const FRigElementKey& Key : Connection.Targets)
		{
			Targets.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s(%s)"),
				*StaticEnum<ERigElementType>()->GetNameStringByValue(static_cast<int64>(Key.Type)), *Key.Name.ToString())));
		}
		const FString Name = Connection.Connector.Name.ToString();
		if (Targets.Num() == 1)
		{
			Connections->SetField(Name, Targets[0]);
		}
		else
		{
			Connections->SetArrayField(Name, Targets);
		}
		if (Name.StartsWith(TEXT("Root/")) || Name.StartsWith(TEXT("Spine/")) || Name == TEXT("Arm_l/Arm Primary"))
		{
			PartialConnections->SetField(Name, AssetDocumentServerJson::FindFieldRef(*Connections, Name));
		}
	}
	const FTarget& Target = Fixtures.Add(TEXT("CR_OneApply"));
	const TSharedPtr<FJsonObject> Create = Document(Target, TEXT("Create"), FString::Printf(TEXT("{\"RigType\": \"Modular\", \"Modules\": [%s]}"), *ModulesJson));
	BodyOf(Create)->SetObjectField(TEXT("Connections"), Connections);
	Create->SetStringField(TEXT("FactoryContext"), FPackageName::ObjectPathToPackageName(FString(BipedMesh)));
	AddInfo(FString::Printf(TEXT("document: %s"), *Serialize(Create)));
	const double Start = FPlatformTime::Seconds();
	FAssetDocumentResult Result;
	if (!Applied(*this, Target, Create, TEXT("one apply"), &Result))
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("one apply: %d connections, %.0f ms, diagnostics %s"), Connections->Values.Num(), Milliseconds(Start),
		*DiagnosticsText(Result).Left(2000)));
	const FString Built = Comparable(Extract(*this, Target.ObjectPath, TEXT("one apply")));
	const FString Expected = Comparable(Extract(*this, EditorBuilt.ObjectPath, TEXT("editor")));
	if (!TestEqual(TEXT("one apply builds the editor's rig"), Built, Expected))
	{
		AddInfo(FirstDifference(Expected, Built));
	}
	// The same document again changes nothing.
	const TSharedPtr<FJsonObject> Again = Parse(Serialize(Create));
	Again->SetStringField(TEXT("Action"), TEXT("Update"));
	FString Text;
	TestEqual(TEXT("re-apply diff"), DiffCount(Again, Text), 0);
	AddInfo(Text.Left(2000));
	// Inspect lists what the modules build (it showed only the mesh's bones).
	const FAssetDocumentResult Inspected = Inspect(Target.ObjectPath);
	const TSharedPtr<FJsonObject>* Globals = nullptr;
	const TSharedPtr<FJsonObject>* Counts = nullptr;
	if (TestTrue(TEXT("Inspect"), Inspected.IsSuccess() && Inspected.Payload.IsValid() && Inspected.Payload->TryGetObjectField(TEXT("GlobalTransforms"), Globals)
		&& Inspected.Payload->TryGetObjectField(TEXT("Counts"), Counts)))
	{
		int32 ModuleControls = 0;
		for (const auto& Entry : (*Globals)->Values)
		{
			ModuleControls += FString(*Entry.Key).StartsWith(TEXT("Control(Arm_l/")) ? 1 : 0;
		}
		AddInfo(FString::Printf(TEXT("Inspect: %d global transforms, %d Arm_l controls, counts %s, size %d"), (*Globals)->Values.Num(), ModuleControls,
			*Serialize(*Counts), Serialize(Inspected.Payload).Len()));
		TestTrue(TEXT("Arm_l's controls are listed"), ModuleControls > 0);
		TestTrue(TEXT("module_elements counted"), (*Counts)->GetIntegerField(TEXT("module_elements")) > 0);
	}
	// Inspect's AutoConnect suggestion for an Arm on the Spine shoulder socket (the editor's drop picks the same
	// clavicle as its Elbow Bone) comes with Check on it.
	const FTarget& Suggesting = Fixtures.Add(TEXT("CR_OneApplySuggest"));
	const TSharedPtr<FJsonObject> Partial = Document(Suggesting, TEXT("Create"), TEXT("{\"RigType\": \"Modular\", \"Modules\": ["
		"{\"Name\": \"Root\", \"Class\": \"/ControlRig/Modules/Modules56/Root.Root_C\"},"
		"{\"Name\": \"Spine\", \"Class\": \"/ControlRigModules/Modules56/Spine.Spine_C\", \"Parent\": \"Root\"},"
		"{\"Name\": \"Arm_l\", \"Class\": \"/ControlRigModules/Modules56/Arm.Arm_C\", \"Parent\": \"Spine\"}]}"));
	BodyOf(Partial)->SetObjectField(TEXT("Connections"), PartialConnections);
	Partial->SetStringField(TEXT("FactoryContext"), BipedMesh);
	if (Applied(*this, Suggesting, Partial, TEXT("primaries only")))
	{
		const FAssetDocumentResult Suggested = Inspect(Suggesting.ObjectPath);
		const TSharedPtr<FJsonObject>* Suggestions = nullptr;
		const TSharedPtr<FJsonObject>* Check = nullptr;
		FString Reason;
		const TSharedPtr<FJsonObject>* AutoConnect = nullptr;
		FString Elbow;
		TestTrue(TEXT("Inspect ModularRigSuggestions.Check names Arm_l/Elbow Bone"), Suggested.Payload.IsValid()
			&& Suggested.Payload->TryGetObjectField(TEXT("ModularRigSuggestions"), Suggestions) && (*Suggestions)->TryGetObjectField(TEXT("Check"), Check)
			&& (*Check)->TryGetStringField(TEXT("Arm_l/Elbow Bone"), Reason));
		TestTrue(TEXT("AutoConnect suggests the clavicle as Arm_l/Elbow Bone"), Suggestions != nullptr
			&& (*Suggestions)->TryGetObjectField(TEXT("AutoConnect"), AutoConnect) && (*AutoConnect)->TryGetStringField(TEXT("Arm_l/Elbow Bone"), Elbow)
			&& Elbow == TEXT("Bone(clavicle_l)"));
		TestFalse(TEXT("Check leaves Arm_l/Hand Bone alone"), Check != nullptr && (*Check)->HasField(TEXT("Arm_l/Hand Bone")));
		AddInfo(FString::Printf(TEXT("suggestions: %s"), Suggestions != nullptr ? *Serialize(*Suggestions).Left(3000) : TEXT("-")));
	}
	return true;
}

#endif
