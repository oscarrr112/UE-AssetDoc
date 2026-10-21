// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// End-to-end Material / Material Function documents through the production Service and the Python Profiles
// (profile.material.v1, profile.material_function.v1).
//
// UEAssetDoc.Material.Python: one ApplyFile creates a PBR material written from scratch
// (texture sample parameter with UVs, vector / scalar parameters, math, a world-aligned texture function call,
// Fresnel emissive, a comment, parameter groups, a Details setting); it compiles without errors; Extract/Diff
// agree; re-applying the extraction is a no-op (no save, package file untouched); Inspect lists pins and the
// material inputs; a Material Instance of it sees exactly the parameters it declares; an edit renames a parameter,
// deletes a node (its links and its material input go), reconnects, changes properties, moves a node and inserts a
// node into a connection; unknown class, bad pin, type-incompatible connection, cycle, unknown property and missing
// source fail at the document path and change nothing; failures after the edit started (bad property value, new
// compile error) roll back; the saved package reloads to the same document.
// UEAssetDoc.Material.Function: a function with inputs / outputs, a material that calls it; an edit
// of the function (new output, renamed input) is seen by the caller after the apply (the editor's
// UpdateMaterialFunction); Material Layer / Layer Blend functions are created by their own factories.
// UEAssetDoc.Material.Undo: a human edit + save, then an AI apply; Ctrl+Z afterwards must not corrupt
// the material; a failed apply keeps the human's history.
// UEAssetDoc.Material.ProgressWindow: with the editor's progress UI (automation replaces GWarn with a
// context that never opens a window, so the other tests cannot see it), every recompile opens the editor's
// "Updating Texture Streaming Data" progress window as in an interactive editor; a create, a Details change and a
// Material Instance of the material still apply and compile (the progress window is not an interactive dialog).

#include "AssetDocumentMaterialTestSupport.h"
#include "AssetDocumentEditorNonInteractiveScope.h"

#include "FeedbackContextEditor.h"
#include "FileHelpers.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialFunctionMaterialLayer.h"
#include "Materials/MaterialFunctionMaterialLayerBlend.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/CoreDelegates.h"
#include "Misc/ScopeExit.h"
#include "Runtime/AssetDocumentUndoHistory.h"
#include "ScopedTransaction.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentMaterialPythonTests
{
using namespace AssetDocumentMaterialTests;

const TCHAR* DefaultTexture = TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture");
const TCHAR* WorldAlignedTexture = TEXT("/Engine/Functions/Engine_MaterialFunctions01/Texturing/WorldAlignedTexture.WorldAlignedTexture");

// A layered PBR material an AI writes without a template.
FString PbrBody()
{
	return FString::Printf(TEXT(R"json(
"Expressions": {
  "UV": {"Class": "TextureCoordinate", "Position": [-1200, -200], "Properties": {"UTiling": 2, "VTiling": 2}},
  "Albedo": {"Class": "TextureSampleParameter2D", "Position": [-900, -200],
             "Properties": {"ParameterName": "Albedo", "Group": "Surface", "Texture": "%s"}, "Inputs": {"Coordinates": "UV"}},
  "Tint": {"Class": "VectorParameter", "Position": [-900, 50],
           "Properties": {"ParameterName": "Tint", "Group": "Surface", "DefaultValue": {"R": 1, "G": 0.5, "B": 0.25, "A": 1}}},
  "Mul": {"Class": "Multiply", "Position": [-600, -100], "Inputs": {"A": "Albedo.RGB", "B": "Tint.RGB"}},
  "Detail": {"Class": "TextureObjectParameter", "Position": [-1200, 300], "Properties": {"ParameterName": "Detail", "Group": "Detail"}},
  "Size": {"Class": "Constant3Vector", "Position": [-1200, 500], "Properties": {"Constant": {"R": 256, "G": 256, "B": 256, "A": 0}}},
  "Wat": {"Class": "MaterialFunctionCall", "Position": [-900, 350], "Properties": {"MaterialFunction": "%s"},
          "Inputs": {"TextureObject": "Detail", "TextureSize": "Size"}},
  "Blend": {"Class": "LinearInterpolate", "Position": [-400, 0], "Properties": {"ConstAlpha": 0.3},
            "Inputs": {"A": "Mul", "B": "Wat.XYZ Texture"}},
  "Rough": {"Class": "ScalarParameter", "Position": [-400, 200],
            "Properties": {"ParameterName": "Roughness", "DefaultValue": 0.5, "SliderMax": 1, "Group": "Surface", "SortPriority": 2}},
  "Metal": {"Class": "Constant", "Position": [-400, 300], "Properties": {"R": 1}},
  "Fres": {"Class": "Fresnel", "Position": [-700, 600], "Properties": {"Exponent": 3}},
  "Glow": {"Class": "ScalarParameter", "Position": [-700, 750], "Properties": {"ParameterName": "GlowStrength", "DefaultValue": 2, "Group": "Emissive"}},
  "Emis": {"Class": "Multiply", "Position": [-400, 650], "Inputs": {"A": "Fres", "B": "Glow"}},
  "Note": {"Class": "Comment", "Position": [-1250, -300], "Properties": {"Text": "Albedo", "SizeX": 800, "SizeY": 450}}
},
"Outputs": {"BaseColor": "Blend", "Metallic": "Metal", "Roughness": "Rough", "EmissiveColor": "Emis"},
"ParameterGroups": {"Surface": 0, "Detail": 1, "Emissive": 2})json"), DefaultTexture, WorldAlignedTexture);
}

UMaterialExpression* FindExpression(UObject* Host, const TCHAR* Key)
{
	for (UMaterialExpression* Expression : GraphExpressions(Host))
	{
		if (Expression->GetName() == Key)
		{
			return Expression;
		}
	}
	return nullptr;
}

// A parsed baseline with one change applied (Action Update).
FString Edited(const FString& Baseline, TFunctionRef<void(const TSharedPtr<FJsonObject>&)> Edit)
{
	const TSharedPtr<FJsonObject> Document = WithAction(Parse(Baseline), TEXT("Update"));
	Edit(Document);
	return Serialize(Document);
}

// A refused edit fails with Code at Path and leaves the extraction unchanged.
void TestRefused(FAutomationTestBase& Test, const FTarget& Target, const FString& Baseline, const TCHAR* Stage,
	const TCHAR* Code, const TCHAR* Path, TFunctionRef<void(const TSharedPtr<FJsonObject>&)> Edit)
{
	// Validate reports the same diagnostic (its trial apply edits a discarded transient copy) and
	// changes nothing: not the material, not its package, not the undo history.
	const FString Json = Edited(Baseline, Edit);
	const FAssetDocumentUndoHistoryMark UndoBefore = UE::AssetDocument::UndoHistory::Capture();
	const double ValidateStart = FPlatformTime::Seconds();
	TestFailsWith(Test, ValidateDocument(Test, Json, *FString::Printf(TEXT("%s (validate)"), Stage)), Code, Path,
		*FString::Printf(TEXT("%s (validate)"), Stage));
	Test.AddInfo(FString::Printf(TEXT("%s: validate took %.3f s"), Stage, FPlatformTime::Seconds() - ValidateStart));
	Test.TestTrue(*FString::Printf(TEXT("%s: validate leaves the undo history"), Stage), UE::AssetDocument::UndoHistory::Capture() == UndoBefore);
	Test.TestTrue(*FString::Printf(TEXT("%s: validate leaves the package clean"), Stage),
		Target.Get() != nullptr && !Target.Get()->GetOutermost()->IsDirty());
	ExtractEquals(Test, Target, Baseline, *FString::Printf(TEXT("%s (validate)"), Stage));
	TestFailsWith(Test, Apply(Test, Target, Json, Stage), Code, Path, Stage);
	ExtractEquals(Test, Target, Baseline, Stage);
	UObject* Asset = Target.Get();
	Test.TestTrue(*FString::Printf(TEXT("%s: package stays clean"), Stage), Asset != nullptr && !Asset->GetOutermost()->IsDirty());
}
} // namespace AssetDocumentMaterialPythonTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMaterialPythonTest, "UEAssetDoc.Material.Python",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMaterialPythonTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMaterialPythonTests;
	const FTarget Target(TEXT("M_Pbr"));
	const FTarget Instance(TEXT("MI_Pbr"));
	const FTarget BadCreate(TEXT("M_BadCreate"));
	ON_SCOPE_EXIT
	{
		FinishCompilation();
		for (const FTarget* Fixture : {&Instance, &BadCreate, &Target})
		{
			Fixture->Cleanup();
		}
	};

	// 1. One apply creates the material from a document written from scratch; it compiles.
	const FString Created = Document(Target, MaterialClass, TEXT("Create"), PbrBody(), TEXT("{\"TwoSided\":true}"));
	{
		// Validate of the valid document runs the whole edit and compile on a new transient material.
		const double ValidateStart = FPlatformTime::Seconds();
		const FAssetDocumentResult Validated = ValidateDocument(*this, Created, TEXT("Create (validate)"));
		AddInfo(FString::Printf(TEXT("validate (trial create + compile) took %.2f s"), FPlatformTime::Seconds() - ValidateStart));
		TestTrue(TEXT("valid document validates"), Validated.IsSuccess() && Validated.Message.Contains(TEXT("would create")));
		TestNothingCreated(*this, Target, TEXT("validate creates nothing"));
	}
	const double Start = FPlatformTime::Seconds();
	if (!TestTrue(TEXT("create applies"), Apply(*this, Target, Created, TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("create + compile took %.2f s (shaders compile asynchronously)"), FPlatformTime::Seconds() - Start));
	UMaterial* Material = Target.Get<UMaterial>();
	if (!TestNotNull(TEXT("material exists"), Material))
	{
		return false;
	}
	TestEqual(TEXT("no compile errors"), FString::Join(CompileErrors(Material), TEXT(" | ")), FString());
	TestEqual(TEXT("14 graph nodes"), GraphExpressions(Material).Num(), 14);
	TestTrue(TEXT("Details setting"), Material->TwoSided != 0);
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	TestTrue(TEXT("BaseColor from Blend"), Inputs->BaseColor.Expression == FindExpression(Material, TEXT("Blend")));
	TestTrue(TEXT("EmissiveColor from Emis"), Inputs->EmissiveColor.Expression == FindExpression(Material, TEXT("Emis")));
	UMaterialExpression* Blend = FindExpression(Material, TEXT("Blend"));
	UMaterialExpression* Wat = FindExpression(Material, TEXT("Wat"));
	TestTrue(TEXT("Blend.B from the function's XYZ Texture output"), Blend != nullptr && Wat != nullptr && Blend->GetInput(1)->Expression == Wat
		&& Wat->GetOutputs().IsValidIndex(Blend->GetInput(1)->OutputIndex)
		&& Wat->GetOutputs()[Blend->GetInput(1)->OutputIndex].OutputName == TEXT("XYZ Texture"));
	TestEqual(TEXT("declared parameters"), FString::Join(ParameterNames(Material), TEXT(",")),
		FString(TEXT("Albedo:3,Detail:3,GlowStrength:0,Roughness:0,Tint:1")));
	TestEqual(TEXT("comment kept"), Cast<UMaterialExpressionComment>(FindExpression(Material, TEXT("Note"))) != nullptr
		? Cast<UMaterialExpressionComment>(FindExpression(Material, TEXT("Note")))->Text : FString(), FString(TEXT("Albedo")));
	TestNoDiff(*this, Created, TEXT("after create"));

	// 2. The extraction re-applies as a no-op; Inspect lists pins and the material inputs.
	FString Baseline = TestNoOpReapply(*this, Target, TEXT("Reapply"));
	{
		const TSharedPtr<FJsonObject> Extracted = Parse(Baseline);
		TestEqual(TEXT("expression keys"), FString::Join(Keys(Field(Field(Extracted, TEXT("Body")), TEXT("Expressions"))), TEXT(",")),
			FString(TEXT("Albedo,Blend,Detail,Emis,Fres,Glow,Metal,Mul,Note,Rough,Size,Tint,UV,Wat")));
		TestFalse(TEXT("no expression guids in the document"), Baseline.Contains(TEXT("ExpressionGUID")) || Baseline.Contains(TEXT("MaterialExpressionGuid")));
		TestTrue(TEXT("new-node values are omitted"), !Baseline.Contains(TEXT("\"SliderMin\"")));
	}
	const FString Inspected = Inspect(*this, Target, TEXT("Inspect"));
	TestTrue(TEXT("Inspect lists pins"), Inspected.Contains(TEXT("\"Pins\"")) && Inspected.Contains(TEXT("\"Coordinates: float*\"")));
	TestTrue(TEXT("Inspect lists the material inputs"), Inspected.Contains(TEXT("\"MaterialInputs\"")) && Inspected.Contains(TEXT("BaseColor: float3")));

	// 3. A Material Instance of the authored material sees exactly its parameters.
	FinishCompilation();
	const FString InstanceDocument = Document(Instance, TEXT("/Script/Engine.MaterialInstanceConstant"), TEXT("Create"), FString::Printf(
		TEXT(R"json("Parent":"%s","Parameters":{"Roughness":0.2,"Tint":[0,1,0,1],"GlowStrength":4})json"), *Target.ObjectPath));
	if (TestTrue(TEXT("instance applies"), Apply(*this, Instance, InstanceDocument, TEXT("Instance")).IsSuccess()))
	{
		const FString Schema = Inspect(*this, Instance, TEXT("Inspect instance"));
		for (const TCHAR* Name : {TEXT("Albedo"), TEXT("Detail"), TEXT("Tint"), TEXT("Roughness"), TEXT("GlowStrength")})
		{
			TestTrue(*FString::Printf(TEXT("instance schema lists %s"), Name), Schema.Contains(FString::Printf(TEXT("{\"Name\":\"%s\""), Name)));
		}
		TestTrue(TEXT("instance schema has the group"), Schema.Contains(TEXT("\"Group\":\"Emissive\"")));
		TestEqual(TEXT("instance sees the same parameter set"), FString::Join(ParameterNames(Instance.Get<UMaterialInstanceConstant>()), TEXT(",")),
			FString::Join(ParameterNames(Material), TEXT(",")));
	}

	// 4. Free edit: rename a parameter, delete a node (link and material input go), reconnect, change properties,
	//    move a node, insert a node into a connection.
	const FString Edit = Edited(Baseline, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Body = Field(Document, TEXT("Body"));
		const TSharedPtr<FJsonObject> Expressions = Field(Body, TEXT("Expressions"));
		EnsureField(ExpressionOf(Document, TEXT("Rough")), TEXT("Properties"))->SetStringField(TEXT("ParameterName"), TEXT("Rough2"));
		Expressions->RemoveField(TEXT("Metal"));
		Field(Body, TEXT("Outputs"))->RemoveField(TEXT("Metallic"));
		Field(ExpressionOf(Document, TEXT("Emis")), TEXT("Inputs"))->SetStringField(TEXT("B"), TEXT("Rough"));
		EnsureField(ExpressionOf(Document, TEXT("Fres")), TEXT("Properties"))->SetNumberField(TEXT("Exponent"), 5);
		ExpressionOf(Document, TEXT("UV"))->SetArrayField(TEXT("Position"),
			TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueNumber>(-1300), MakeShared<FJsonValueNumber>(-250)});
		const TSharedPtr<FJsonObject> Sat = MakeShared<FJsonObject>();
		Sat->SetStringField(TEXT("Class"), TEXT("Saturate"));
		EnsureField(Sat, TEXT("Inputs"))->SetStringField(TEXT("Input"), TEXT("Blend"));
		Expressions->SetObjectField(TEXT("Sat"), Sat);
		Field(Body, TEXT("Outputs"))->SetStringField(TEXT("BaseColor"), TEXT("Sat"));
	});
	if (TestTrue(TEXT("edit applies"), Apply(*this, Target, Edit, TEXT("Edit")).IsSuccess()))
	{
		Material = Target.Get<UMaterial>();
		Inputs = Material->GetEditorOnlyData();
		TestEqual(TEXT("edit: no compile errors"), FString::Join(CompileErrors(Material), TEXT(" | ")), FString());
		TestEqual(TEXT("renamed parameter"), FString::Join(ParameterNames(Material), TEXT(",")),
			FString(TEXT("Albedo:3,Detail:3,GlowStrength:0,Rough2:0,Tint:1")));
		TestNull(TEXT("deleted node is gone"), FindExpression(Material, TEXT("Metal")));
		TestNull(TEXT("its material input is disconnected"), Inputs->Metallic.Expression);
		TestTrue(TEXT("reconnected"), FindExpression(Material, TEXT("Emis"))->GetInput(1)->Expression == FindExpression(Material, TEXT("Rough")));
		TestTrue(TEXT("inserted node feeds BaseColor"), Inputs->BaseColor.Expression == FindExpression(Material, TEXT("Sat"))
			&& FindExpression(Material, TEXT("Sat"))->GetInput(0)->Expression == FindExpression(Material, TEXT("Blend")));
		TestEqual(TEXT("moved"), FindExpression(Material, TEXT("UV"))->MaterialExpressionEditorX, -1300);
		TestNoDiff(*this, Edit, TEXT("after edit"));
	}
	Baseline = TestNoOpReapply(*this, Target, TEXT("Reapply edited"));

	// 5. Invalid documents fail at the document path and change nothing.
	TestRefused(*this, Target, Baseline, TEXT("unknown class"), TEXT("Material.UnknownExpressionClass"), TEXT("/Body/Expressions/Bad/Class"),
		[](const TSharedPtr<FJsonObject>& Document)
		{
			const TSharedPtr<FJsonObject> Bad = MakeShared<FJsonObject>();
			Bad->SetStringField(TEXT("Class"), TEXT("Multiplyy"));
			Field(Field(Document, TEXT("Body")), TEXT("Expressions"))->SetObjectField(TEXT("Bad"), Bad);
		});
	TestRefused(*this, Target, Baseline, TEXT("bad pin"), TEXT("Material.PinNotFound"), TEXT("/Body/Expressions/Emis/Inputs/C"),
		[](const TSharedPtr<FJsonObject>& Document) { Field(ExpressionOf(Document, TEXT("Emis")), TEXT("Inputs"))->SetStringField(TEXT("C"), TEXT("Glow")); });
	TestRefused(*this, Target, Baseline, TEXT("bad output"), TEXT("Material.PinNotFound"), TEXT("/Body/Expressions/Mul/Inputs/A"),
		[](const TSharedPtr<FJsonObject>& Document) { Field(ExpressionOf(Document, TEXT("Mul")), TEXT("Inputs"))->SetStringField(TEXT("A"), TEXT("Albedo.RGBX")); });
	TestRefused(*this, Target, Baseline, TEXT("incompatible connection"), TEXT("Material.IncompatibleConnection"), TEXT("/Body/Outputs/BaseColor"),
		[](const TSharedPtr<FJsonObject>& Document) { Field(Field(Document, TEXT("Body")), TEXT("Outputs"))->SetStringField(TEXT("BaseColor"), TEXT("Detail")); });
	TestRefused(*this, Target, Baseline, TEXT("cycle"), TEXT("Material.ConnectionCycle"), TEXT(""),
		[](const TSharedPtr<FJsonObject>& Document) { Field(ExpressionOf(Document, TEXT("Mul")), TEXT("Inputs"))->SetStringField(TEXT("A"), TEXT("Sat")); });
	TestRefused(*this, Target, Baseline, TEXT("unknown property"), TEXT("Material.UnknownProperty"), TEXT("/Body/Expressions/Glow/Properties/DefaultValu"),
		[](const TSharedPtr<FJsonObject>& Document) { EnsureField(ExpressionOf(Document, TEXT("Glow")), TEXT("Properties"))->SetNumberField(TEXT("DefaultValu"), 1); });
	TestRefused(*this, Target, Baseline, TEXT("missing source"), TEXT("Material.ExpressionNotFound"), TEXT("/Body/Expressions/Emis/Inputs/A"),
		[](const TSharedPtr<FJsonObject>& Document) { Field(ExpressionOf(Document, TEXT("Emis")), TEXT("Inputs"))->SetStringField(TEXT("A"), TEXT("Nope")); });
	TestRefused(*this, Target, Baseline, TEXT("function-only class"), TEXT("Material.ExpressionNotAllowed"), TEXT("/Body/Expressions/In/Class"),
		[](const TSharedPtr<FJsonObject>& Document)
		{
			const TSharedPtr<FJsonObject> In = MakeShared<FJsonObject>();
			In->SetStringField(TEXT("Class"), TEXT("FunctionInput"));
			Field(Field(Document, TEXT("Body")), TEXT("Expressions"))->SetObjectField(TEXT("In"), In);
		});

	// 6. Failures after the edit started roll back whole: a bad property value after a delete, and a new
	//    compile error (a texture sample without a texture).
	TestRefused(*this, Target, Baseline, TEXT("bad value after a delete"), TEXT("Material.InvalidPropertyValue"), TEXT("/Body/Expressions/Glow/Properties/DefaultValue"),
		[](const TSharedPtr<FJsonObject>& Document)
		{
			Field(Field(Document, TEXT("Body")), TEXT("Expressions"))->RemoveField(TEXT("Note"));
			EnsureField(ExpressionOf(Document, TEXT("Glow")), TEXT("Properties"))->SetStringField(TEXT("DefaultValue"), TEXT("bright"));
		});
	TestNotNull(TEXT("rolled back: deleted comment restored"), FindExpression(Target.Get(), TEXT("Note")));
	// (The refused material fails to compile once before the rollback; the engine logs it.)
	AddExpectedMessagePlain(TEXT("Failed to compile Material"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	TestRefused(*this, Target, Baseline, TEXT("new compile error"), TEXT("Material.CompileFailed"), TEXT(""),
		[](const TSharedPtr<FJsonObject>& Document)
		{
			const TSharedPtr<FJsonObject> NoTexture = MakeShared<FJsonObject>();
			NoTexture->SetStringField(TEXT("Class"), TEXT("TextureSample"));
			Field(Field(Document, TEXT("Body")), TEXT("Expressions"))->SetObjectField(TEXT("NoTexture"), NoTexture);
			Field(ExpressionOf(Document, TEXT("Emis")), TEXT("Inputs"))->SetStringField(TEXT("A"), TEXT("NoTexture.RGB"));
		});
	Material = Target.Get<UMaterial>();
	TestEqual(TEXT("rolled back: compiles without errors"), FString::Join(CompileErrors(Material), TEXT(" | ")), FString());

	// 7. The saved package, unloaded and loaded back from disk, extracts to the same document.
	Instance.Cleanup();
	FinishCompilation();
	TestMaterialReload(*this, Target, Baseline);

	// 8. A failed create leaves nothing; validate (a trial on a new transient material) reports it first.
	const FString BadCreateDocument = Document(BadCreate, MaterialClass, TEXT("Create"),
		TEXT(R"json("Expressions":{"A":{"Class":"Add","Inputs":{"A":"B"}},"B":{"Class":"Nope"}})json"));
	TestFailsWith(*this, ValidateDocument(*this, BadCreateDocument, TEXT("BadCreate (validate)")),
		TEXT("Material.UnknownExpressionClass"), TEXT("/Body/Expressions/B/Class"), TEXT("create with an unknown class (validate)"));
	TestNothingCreated(*this, BadCreate, TEXT("create with an unknown class (validate)"));
	TestFailsWith(*this, Apply(*this, BadCreate, BadCreateDocument, TEXT("BadCreate")),
		TEXT("Material.UnknownExpressionClass"), TEXT("/Body/Expressions/B/Class"), TEXT("create with an unknown class"));
	TestNothingCreated(*this, BadCreate, TEXT("create with an unknown class"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMaterialFunctionTest, "UEAssetDoc.Material.Function",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMaterialFunctionTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMaterialPythonTests;
	const FTarget Function(TEXT("MF_Scale"));
	const FTarget Caller(TEXT("M_Caller"));
	const FTarget Layer(TEXT("ML_Layer"));
	const FTarget Blend(TEXT("MLB_Blend"));
	ON_SCOPE_EXIT
	{
		FinishCompilation();
		for (const FTarget* Fixture : {&Caller, &Function, &Layer, &Blend})
		{
			Fixture->Cleanup();
		}
	};
	const FString FunctionDocument = Document(Function, FunctionClass, TEXT("Create"), TEXT(R"json(
"Expressions": {
  "Value": {"Class": "FunctionInput", "Position": [-500, 0], "Properties": {"InputName": "Value", "InputType": "FunctionInput_Vector3", "SortPriority": 0}},
  "Scale": {"Class": "FunctionInput", "Position": [-500, 200], "Properties": {"InputName": "Scale", "InputType": "FunctionInput_Scalar", "SortPriority": 1,
            "bUsePreviewValueAsDefault": true, "PreviewValue": {"X": 2, "Y": 0, "Z": 0, "W": 0}}},
  "Mul": {"Class": "Multiply", "Position": [-250, 100], "Inputs": {"A": "Value", "B": "Scale"}},
  "Result": {"Class": "FunctionOutput", "Position": [0, 100], "Properties": {"OutputName": "Result"}, "Inputs": {"A": "Mul"}}
})json"), TEXT("{\"Description\":\"Scales a vector\",\"bExposeToLibrary\":true}"));
	if (!TestTrue(TEXT("function applies"), Apply(*this, Function, FunctionDocument, TEXT("Function")).IsSuccess()))
	{
		return false;
	}
	TestNoDiff(*this, FunctionDocument, TEXT("after function create"));
	FString FunctionBaseline = TestNoOpReapply(*this, Function, TEXT("Reapply function"));

	const FString CallerDocument = Document(Caller, MaterialClass, TEXT("Create"), FString::Printf(TEXT(R"json(
"Expressions": {
  "Color": {"Class": "Constant3Vector", "Position": [-600, 0], "Properties": {"Constant": {"R": 0.5, "G": 0.25, "B": 1, "A": 0}}},
  "Call": {"Class": "MaterialFunctionCall", "Position": [-300, 0], "Properties": {"MaterialFunction": "%s"}, "Inputs": {"Value": "Color"}}
},
"Outputs": {"BaseColor": "Call.Result"})json"), *Function.ObjectPath));
	if (!TestTrue(TEXT("caller applies"), Apply(*this, Caller, CallerDocument, TEXT("Caller")).IsSuccess()))
	{
		return false;
	}
	TestNoDiff(*this, CallerDocument, TEXT("after caller create"));
	TestEqual(TEXT("caller compiles"), FString::Join(CompileErrors(Caller.Get<UMaterial>()), TEXT(" | ")), FString());
	TestNoOpReapply(*this, Caller, TEXT("Reapply caller"));

	// Editing the function (a second output) updates its callers, as the Material Function Editor's Apply does.
	const FString FunctionEdit = Edited(FunctionBaseline, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Twice = MakeShared<FJsonObject>();
		Twice->SetStringField(TEXT("Class"), TEXT("FunctionOutput"));
		EnsureField(Twice, TEXT("Properties"))->SetStringField(TEXT("OutputName"), TEXT("Squared"));
		const TSharedPtr<FJsonObject> Square = MakeShared<FJsonObject>();
		Square->SetStringField(TEXT("Class"), TEXT("Multiply"));
		EnsureField(Square, TEXT("Inputs"))->SetStringField(TEXT("A"), TEXT("Mul"));
		Field(Square, TEXT("Inputs"))->SetStringField(TEXT("B"), TEXT("Mul"));
		EnsureField(Twice, TEXT("Inputs"))->SetStringField(TEXT("A"), TEXT("Square"));
		const TSharedPtr<FJsonObject> Expressions = Field(Field(Document, TEXT("Body")), TEXT("Expressions"));
		Expressions->SetObjectField(TEXT("Square"), Square);
		Expressions->SetObjectField(TEXT("Squared"), Twice);
	});
	if (TestTrue(TEXT("function edit applies"), Apply(*this, Function, FunctionEdit, TEXT("FunctionEdit")).IsSuccess()))
	{
		TestNoDiff(*this, FunctionEdit, TEXT("after function edit"));
		UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(FindExpression(Caller.Get(), TEXT("Call")));
		TestTrue(TEXT("caller sees the new output"), Call != nullptr && Call->FunctionOutputs.Num() == 2);
		TestTrue(TEXT("caller keeps its links"), Caller.Get<UMaterial>()->GetEditorOnlyData()->BaseColor.Expression == Call);
		const TSharedPtr<FJsonObject> CallerExtracted = Extract(*this, Caller, TEXT("caller after function edit"));
		const TSharedPtr<FJsonObject> CallEntry = ExpressionOf(CallerExtracted, TEXT("Call"));
		TestTrue(TEXT("caller document still connects Value"), CallEntry.IsValid() && Field(CallEntry, TEXT("Inputs")).IsValid()
			&& Field(CallEntry, TEXT("Inputs"))->GetStringField(TEXT("Value")) == TEXT("Color"));
	}
	FunctionBaseline = TestNoOpReapply(*this, Function, TEXT("Reapply edited function"));
	FinishCompilation();
	// The caller's function call keeps the function loaded; unload it first (it is saved and clean).
	if (UObject* CallerAsset = Caller.Get())
	{
		TestFalse(TEXT("caller is clean before unload"), CallerAsset->GetOutermost()->IsDirty());
		ReleaseUndoHistory();
		UPackageTools::UnloadPackages({CallerAsset->GetOutermost()});
	}
	TestMaterialReload(*this, Function, FunctionBaseline);

	// Material Layer / Layer Blend functions are created by their own factories (usage set).
	const FString LayerDocument = Document(Layer, TEXT("/Script/Engine.MaterialFunctionMaterialLayer"), TEXT("Create"), TEXT(R"json(
"Expressions": {
  "In": {"Class": "FunctionInput", "Position": [-400, 0], "Properties": {"InputName": "Material Attributes", "InputType": "FunctionInput_MaterialAttributes", "bUsePreviewValueAsDefault": true}},
  "Set": {"Class": "SetMaterialAttributes", "Position": [-200, 0], "Inputs": {"MaterialAttributes": "In"}},
  "Out": {"Class": "FunctionOutput", "Position": [0, 0], "Properties": {"OutputName": "Result"}, "Inputs": {"A": "Set"}}
})json"));
	if (TestTrue(TEXT("layer applies"), Apply(*this, Layer, LayerDocument, TEXT("Layer")).IsSuccess()))
	{
		TestTrue(TEXT("layer usage"), Layer.Get<UMaterialFunctionMaterialLayer>() != nullptr
			&& Layer.Get<UMaterialFunctionMaterialLayer>()->GetMaterialFunctionUsage() == EMaterialFunctionUsage::MaterialLayer);
		TestNoDiff(*this, LayerDocument, TEXT("after layer create"));
		TestNoOpReapply(*this, Layer, TEXT("Reapply layer"));
	}
	const FString BlendDocument = Document(Blend, TEXT("/Script/Engine.MaterialFunctionMaterialLayerBlend"), TEXT("Create"), TEXT(R"json(
"Expressions": {
  "Top": {"Class": "FunctionInput", "Position": [-500, 0], "Properties": {"InputName": "Top Layer", "InputType": "FunctionInput_MaterialAttributes", "bUsePreviewValueAsDefault": true}},
  "Bottom": {"Class": "FunctionInput", "Position": [-500, 200], "Properties": {"InputName": "Bottom Layer", "InputType": "FunctionInput_MaterialAttributes", "SortPriority": 1, "bUsePreviewValueAsDefault": true}},
  "Alpha": {"Class": "ScalarParameter", "Position": [-500, 400], "Properties": {"ParameterName": "Alpha", "DefaultValue": 0.5}},
  "Mix": {"Class": "BlendMaterialAttributes", "Position": [-250, 100], "Inputs": {"A": "Top", "B": "Bottom", "Alpha": "Alpha"}},
  "Out": {"Class": "FunctionOutput", "Position": [0, 100], "Properties": {"OutputName": "Result"}, "Inputs": {"A": "Mix"}}
})json"));
	if (TestTrue(TEXT("layer blend applies"), Apply(*this, Blend, BlendDocument, TEXT("LayerBlend")).IsSuccess()))
	{
		TestTrue(TEXT("blend usage"), Blend.Get<UMaterialFunctionMaterialLayerBlend>() != nullptr
			&& Blend.Get<UMaterialFunctionMaterialLayerBlend>()->GetMaterialFunctionUsage() == EMaterialFunctionUsage::MaterialLayerBlend);
		TestNoDiff(*this, BlendDocument, TEXT("after layer blend create"));
		TestNoOpReapply(*this, Blend, TEXT("Reapply layer blend"));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMaterialUndoTest, "UEAssetDoc.Material.Undo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMaterialUndoTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMaterialPythonTests;
	const FTarget Target(TEXT("M_Undo"));
	ON_SCOPE_EXIT
	{
		FinishCompilation();
		Target.Cleanup();
	};
	if (GEditor == nullptr || GEditor->Trans == nullptr)
	{
		return false;
	}
	if (!TestTrue(TEXT("create applies"), Apply(*this, Target, Document(Target, MaterialClass, TEXT("Create"), TEXT(R"json(
"Expressions": {
  "Rough": {"Class": "ScalarParameter", "Position": [-400, 0], "Properties": {"ParameterName": "Roughness", "DefaultValue": 0.5}},
  "Color": {"Class": "VectorParameter", "Position": [-400, 200], "Properties": {"ParameterName": "Color"}}
},
"Outputs": {"Roughness": "Rough", "BaseColor": "Color.RGB"})json")), TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	ReleaseUndoHistory();
	auto Live = [&Target]() { return Target.Get<UMaterial>(); };
	// The Material Editor's Details edit of a node and its node creation (both transactions).
	auto HumanEdit = [&Live](float Value)
	{
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentMaterialTests", "UserEdit", "Edit Node"));
		UMaterialExpressionScalarParameter* Rough = Cast<UMaterialExpressionScalarParameter>(FindExpression(Live(), TEXT("Rough")));
		Rough->Modify();
		Rough->DefaultValue = Value;
		FPropertyChangedEvent Event(UMaterialExpressionScalarParameter::StaticClass()->FindPropertyByName(TEXT("DefaultValue")));
		Rough->PostEditChangeProperty(Event);
	};
	auto UndoRedoAll = [this, &Live](const TCHAR* Stage)
	{
		int32 Undone = 0;
		while (Undone < 16 && GEditor->Trans->CanUndo() && GEditor->UndoTransaction())
		{
			++Undone;
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			TestTrue(*FString::Printf(TEXT("%s: material whole after undo %d"), Stage, Undone), Live() != nullptr
				&& FindExpression(Live(), TEXT("Rough")) != nullptr && Live()->GetEditorOnlyData()->Roughness.Expression == FindExpression(Live(), TEXT("Rough")));
		}
		int32 Redone = 0;
		while (Redone < Undone && GEditor->Trans->CanRedo() && GEditor->RedoTransaction())
		{
			++Redone;
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		TestEqual(*FString::Printf(TEXT("%s: every undone step redoes"), Stage), Redone, Undone);
	};

	// 1. The human edits and saves; the AI then adds nodes: history referencing the material is cleared.
	HumanEdit(0.7f);
	TestTrue(TEXT("human save"), UEditorLoadingAndSavingUtils::SavePackages({Live()->GetOutermost()}, false));
	TestTrue(TEXT("the human edit is in the undo history"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	const FString Baseline = Serialize(Extract(*this, Target, TEXT("after human edit")));
	const FString AiEdit = Edited(Baseline, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Half = MakeShared<FJsonObject>();
		Half->SetStringField(TEXT("Class"), TEXT("Multiply"));
		EnsureField(Half, TEXT("Properties"))->SetNumberField(TEXT("ConstB"), 0.5);
		EnsureField(Half, TEXT("Inputs"))->SetStringField(TEXT("A"), TEXT("Rough"));
		Field(Field(Document, TEXT("Body")), TEXT("Expressions"))->SetObjectField(TEXT("Half"), Half);
		Field(Field(Document, TEXT("Body")), TEXT("Outputs"))->SetStringField(TEXT("Specular"), TEXT("Half"));
	});
	if (!TestTrue(TEXT("AI edit applies"), Apply(*this, Target, AiEdit, TEXT("Edit")).IsSuccess()))
	{
		return false;
	}
	TestFalse(TEXT("after AI edit: no undo history references the material"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	const FString AfterEdit = Serialize(Extract(*this, Target, TEXT("after AI edit")));
	TestTrue(TEXT("human value kept"), AfterEdit.Contains(TEXT("\"DefaultValue\":0.7")));
	UndoRedoAll(TEXT("after AI edit"));
	ExtractEquals(*this, Target, AfterEdit, TEXT("after AI edit + undo/redo"));

	// 2. The human edits again; an AI apply fails and rolls back: the human step stays undoable.
	ReleaseUndoHistory();
	HumanEdit(0.2f);
	const int32 HumanSteps = GEditor->Trans->GetQueueLength();
	const FString HumanState = Serialize(Extract(*this, Target, TEXT("after second human edit")));
	const FAssetDocumentResult Failed = Apply(*this, Target, Edited(HumanState, [](const TSharedPtr<FJsonObject>& Document)
	{
		Field(Field(Document, TEXT("Body")), TEXT("Expressions"))->RemoveField(TEXT("Half"));
		EnsureField(ExpressionOf(Document, TEXT("Rough")), TEXT("Properties"))->SetStringField(TEXT("DefaultValue"), TEXT("x"));
	}), TEXT("Failed"));
	TestFalse(TEXT("failing apply fails"), Failed.IsSuccess());
	TestEqual(TEXT("after rollback: the human history is kept"), GEditor->Trans->GetQueueLength(), HumanSteps);
	ExtractEquals(*this, Target, HumanState, TEXT("after rollback"));
	TestTrue(TEXT("Ctrl+Z undoes the human edit"), GEditor->UndoTransaction());
	ExtractEquals(*this, Target, AfterEdit, TEXT("after rollback + undo"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMaterialProgressWindowTest, "UEAssetDoc.Material.ProgressWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMaterialProgressWindowTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentMaterialPythonTests;
	const FTarget Target(TEXT("M_Progress"));
	const FTarget Instance(TEXT("MI_Progress"));
	ON_SCOPE_EXIT
	{
		FinishCompilation();
		Instance.Cleanup();
		Target.Cleanup();
	};
	// The editor's own feedback context, as GWarn is outside automation.
	static FFeedbackContextEditor EditorFeedback;
	int32 ProgressWindows = 0;
	const FDelegateHandle ProgressWindowCounter = FAssetDocumentEditorNonInteractiveScope::AddPreSlateModalHandler([&ProgressWindows]()
	{
		ProgressWindows += GIsSlowTask ? 1 : 0;
	});
	ON_SCOPE_EXIT
	{
		FAssetDocumentEditorNonInteractiveScope::RemovePreSlateModalHandler(ProgressWindowCounter);
	};
	TGuardValue<FFeedbackContext*> EditorProgressUI(GWarn, &EditorFeedback);

	const FString Body = TEXT(R"json("Expressions": {"Tint": {"Class": "VectorParameter", "Position": [-400, 0],
  "Properties": {"ParameterName": "Tint", "DefaultValue": {"R": 1, "G": 0.5, "B": 0.25, "A": 1}}}},
"Outputs": {"BaseColor": "Tint"})json");
	if (!TestTrue(TEXT("create applies"), Apply(*this, Target, Document(Target, MaterialClass, TEXT("Create"), Body), TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	TestTrue(TEXT("the recompile opened the editor's progress window (the case this test covers)"), ProgressWindows > 0);
	UMaterial* Material = Target.Get<UMaterial>();
	if (!TestNotNull(TEXT("material exists"), Material))
	{
		return false;
	}
	TestEqual(TEXT("no compile errors"), FString::Join(CompileErrors(Material), TEXT(" | ")), FString());

	const int32 WindowsBeforeUpdate = ProgressWindows;
	TestTrue(TEXT("a Details change applies"), Apply(*this, Target,
		Document(Target, MaterialClass, TEXT("Update"), Body, TEXT("{\"TwoSided\":true}")), TEXT("Update")).IsSuccess());
	TestTrue(TEXT("Details change recompiled with a progress window"), ProgressWindows > WindowsBeforeUpdate);
	TestTrue(TEXT("TwoSided set"), Material->TwoSided != 0);

	FinishCompilation();
	const FString InstanceDocument = Document(Instance, TEXT("/Script/Engine.MaterialInstanceConstant"), TEXT("Create"), FString::Printf(
		TEXT(R"json("Parent":"%s","Parameters":{"Tint":[0,1,0,1]})json"), *Target.ObjectPath));
	TestTrue(TEXT("an instance of it applies"), Apply(*this, Instance, InstanceDocument, TEXT("Instance")).IsSuccess());
	return !HasAnyErrors();
}
#endif
