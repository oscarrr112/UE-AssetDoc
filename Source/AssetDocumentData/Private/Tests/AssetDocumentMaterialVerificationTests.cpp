// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Material edits whose order matters in the editor.
//   ClassChange   an existing expression gets another Class under the same key: it is replaced and the links
//                 to it (another expression's input, a material output) are made again, as when one deletes a
//                 node, places the new one and wires it like the old one.
//   CustomInputs  a Custom node's Inputs list changes (an input removed, another added): the old input's link
//                 is broken before the input goes, the new one is wired after it exists.
// Duplicate keys:
//   CaseDuplicateKey  two expression keys that differ only by case ('DropS' / 'Drops') are one UE object name:
//                     the document is refused when it is read (Document.DuplicateKey naming both: a JSON object
//                     read by the editor keeps one of them), nothing is created.
//   OutputConstant    a number in Outputs ("Roughness": 0.15) is a Constant node wired to the pin, as one places
//                     it in the Material Editor; extract writes it back as the number, a changed number edits the
//                     node, a source instead of the number deletes it.

#include "AssetDocumentMaterialTestSupport.h"

#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentMaterialVerificationTests
{
using namespace AssetDocumentMaterialTests;

UMaterialExpression* FindByName(UObject* Host, const TCHAR* Key)
{
	for (UMaterialExpression* Expression : GraphExpressions(Host))
	{
		if (Expression != nullptr && Expression->GetName() == Key)
		{
			return Expression;
		}
	}
	return nullptr;
}
} // namespace AssetDocumentMaterialVerificationTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMaterialVerificationClassChangeTest, "UEAssetDoc.Material.Verification.ClassChange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMaterialVerificationClassChangeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentMaterialVerificationTests;
	const FTarget Target(TEXT("M_ClassChange"));
	ON_SCOPE_EXIT
	{
		FinishCompilation();
		Target.Cleanup();
	};
	auto Body = [](const TCHAR* LZ)
	{
		return FString::Printf(TEXT(R"json(
"Expressions": {
  "LZ": %s,
  "V": {"Class": "Constant3Vector", "Position": [-600, 200], "Properties": {"Constant": {"R": 1, "G": 0.5, "B": 0.25, "A": 0}}},
  "ZRel": {"Class": "Multiply", "Position": [-300, 0], "Inputs": {"A": "LZ", "B": "V"}}
},
"Outputs": {"BaseColor": "ZRel", "Roughness": "LZ"})json"), LZ);
	};
	const FString Before = Document(Target, MaterialClass, TEXT("Create"),
		Body(TEXT(R"json({"Class": "Constant", "Position": [-600, 0], "Properties": {"R": 0.5}})json")));
	if (!TestTrue(TEXT("create"), Apply(*this, Target, Before, TEXT("create")).IsSuccess()))
	{
		return false;
	}
	const FString After = Document(Target, MaterialClass, TEXT("Update"),
		Body(TEXT(R"json({"Class": "ScalarParameter", "Position": [-600, 0], "Properties": {"ParameterName": "LZ", "DefaultValue": 0.75}})json")));
	if (!TestTrue(TEXT("the same key with another Class applies"), Apply(*this, Target, After, TEXT("class change")).IsSuccess()))
	{
		return false;
	}
	UMaterial* Material = Target.Get<UMaterial>();
	UMaterialExpression* LZ = FindByName(Material, TEXT("LZ"));
	UMaterialExpression* ZRel = FindByName(Material, TEXT("ZRel"));
	TestTrue(TEXT("LZ is a scalar parameter"), LZ != nullptr && LZ->IsA<UMaterialExpressionScalarParameter>());
	TestTrue(TEXT("ZRel.A is wired to the new LZ"), ZRel != nullptr && LZ != nullptr && ZRel->GetInput(0)->Expression == LZ);
	TestTrue(TEXT("Roughness is wired to the new LZ"), Material != nullptr && LZ != nullptr
		&& Material->GetEditorOnlyData()->Roughness.Expression == LZ);
	TestNoOpReapply(*this, Target, TEXT("after the class change"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMaterialVerificationCustomInputsTest, "UEAssetDoc.Material.Verification.CustomInputs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMaterialVerificationCustomInputsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentMaterialVerificationTests;
	const FTarget Target(TEXT("M_CustomInputs"));
	ON_SCOPE_EXIT
	{
		FinishCompilation();
		Target.Cleanup();
	};
	auto Body = [](const TCHAR* Code, const TCHAR* Second)
	{
		return FString::Printf(TEXT(R"json(
"Expressions": {
  "C1": {"Class": "Constant", "Position": [-700, 0], "Properties": {"R": 0.25}},
  "C2": {"Class": "Constant", "Position": [-700, 150], "Properties": {"R": 2}},
  "Arc": {"Class": "Custom", "Position": [-400, 0],
          "Properties": {"Code": "%s", "OutputType": "CMOT_Float1", "Inputs": [{"InputName": "A"}, {"InputName": "%s"}]},
          "Inputs": {"A": "C1", "%s": "C2"}}
},
"Outputs": {"Roughness": "Arc"})json"), Code, Second, Second);
	};
	if (!TestTrue(TEXT("create"), Apply(*this, Target, Document(Target, MaterialClass, TEXT("Create"), Body(TEXT("return A + N;"), TEXT("N"))),
		TEXT("create")).IsSuccess()))
	{
		return false;
	}
	if (const TSharedPtr<FJsonObject> Extracted = Extract(*this, Target, TEXT("created")))
	{
		AddInfo(FString::Printf(TEXT("created Arc: %s"), *Serialize(ExpressionOf(Extracted, TEXT("Arc"))).Left(2000)));
	}
	if (!TestTrue(TEXT("a Custom node's Inputs change applies"), Apply(*this, Target,
		Document(Target, MaterialClass, TEXT("Update"), Body(TEXT("return A * M;"), TEXT("M"))), TEXT("inputs change")).IsSuccess()))
	{
		return false;
	}
	UMaterialExpressionCustom* Arc = Cast<UMaterialExpressionCustom>(FindByName(Target.Get<UMaterial>(), TEXT("Arc")));
	if (TestNotNull(TEXT("Arc"), Arc) && TestEqual(TEXT("two inputs"), Arc->Inputs.Num(), 2))
	{
		TestEqual(TEXT("second input renamed"), Arc->Inputs[1].InputName, FName(TEXT("M")));
		TestTrue(TEXT("M wired to C2"), Arc->Inputs[1].Input.Expression == FindByName(Target.Get<UMaterial>(), TEXT("C2")));
	}
	TestNoOpReapply(*this, Target, TEXT("after the inputs change"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMaterialVerificationCaseDuplicateKeyTest, "UEAssetDoc.Material.Verification.CaseDuplicateKey",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMaterialVerificationCaseDuplicateKeyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentMaterialVerificationTests;
	const FTarget Target(TEXT("M_CaseKeys"));
	ON_SCOPE_EXIT
	{
		FinishCompilation();
		Target.Cleanup();
	};
	const FString Json = Document(Target, MaterialClass, TEXT("Create"), TEXT(R"json(
"Expressions": {
  "DropS": {"Class": "Constant", "Position": [-600, 0], "Properties": {"R": 0.25}},
  "Drops": {"Class": "Constant", "Position": [-600, 200], "Properties": {"R": 0.75}},
  "Mul": {"Class": "Multiply", "Position": [-300, 0], "Inputs": {"A": "DropS", "B": "Drops"}}
},
"Outputs": {"Roughness": "Mul"})json"));
	const FAssetDocumentResult Result = Apply(*this, Target, Json, TEXT("case duplicates"));
	TestFailsWith(*this, Result, TEXT("Document.DuplicateKey"), TEXT("/Body/Expressions/Drops"), TEXT("case duplicates"));
	TestTrue(TEXT("the message names both keys"), Result.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Message.Contains(TEXT("'DropS'")) && Diagnostic.Message.Contains(TEXT("'Drops'"));
	}));
	TestFalse(TEXT("not reported as a missing expression"), Result.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == TEXT("Material.ExpressionNotFound");
	}));
	TestNothingCreated(*this, Target, TEXT("case duplicates"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMaterialVerificationOutputConstantTest, "UEAssetDoc.Material.Verification.OutputConstant",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMaterialVerificationOutputConstantTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentMaterialVerificationTests;
	const FTarget Target(TEXT("M_OutputConstant"));
	ON_SCOPE_EXIT
	{
		FinishCompilation();
		Target.Cleanup();
	};
	auto Body = [](const TCHAR* Roughness)
	{
		return FString::Printf(TEXT(R"json(
"Expressions": {
  "Rough": {"Class": "ScalarParameter", "Position": [-600, 0], "Properties": {"ParameterName": "Rough", "DefaultValue": 0.5}}
},
"Outputs": {"Roughness": %s, "Metallic": 1})json"), Roughness);
	};
	auto ConstantOn = [](UMaterial* Material, FScalarMaterialInput UMaterialEditorOnlyData::* Pin) -> UMaterialExpressionConstant*
	{
		return Material != nullptr ? Cast<UMaterialExpressionConstant>((Material->GetEditorOnlyData()->*Pin).Expression) : nullptr;
	};
	if (!TestTrue(TEXT("numbers in Outputs apply"), Apply(*this, Target, Document(Target, MaterialClass, TEXT("Create"), Body(TEXT("0.15"))),
		TEXT("create")).IsSuccess()))
	{
		return false;
	}
	UMaterialExpressionConstant* Roughness = ConstantOn(Target.Get<UMaterial>(), &UMaterialEditorOnlyData::Roughness);
	if (TestNotNull(TEXT("Roughness is wired from a Constant node"), Roughness))
	{
		TestEqual(TEXT("its key"), Roughness->GetName(), FString(TEXT("Constant_Roughness")));
		TestEqual(TEXT("its value"), Roughness->R, 0.15f);
	}
	UMaterialExpressionConstant* Metallic = ConstantOn(Target.Get<UMaterial>(), &UMaterialEditorOnlyData::Metallic);
	TestTrue(TEXT("Metallic is wired from a Constant 1"), Metallic != nullptr && Metallic->R == 1.0f);
	TestTrue(TEXT("the material compiles"), CompileErrors(Target.Get<UMaterial>()).IsEmpty());
	if (const TSharedPtr<FJsonObject> Extracted = Extract(*this, Target, TEXT("created")))
	{
		const TSharedPtr<FJsonObject> Outputs = Field(Field(Extracted, TEXT("Body")), TEXT("Outputs"));
		double Value = 0.0;
		TestTrue(TEXT("extract writes Roughness back as the number"), Outputs.IsValid() && Outputs->TryGetNumberField(TEXT("Roughness"), Value)
			&& FMath::IsNearlyEqual(Value, 0.15, 1e-6));
		TestFalse(TEXT("the implicit node is not listed"), ExpressionOf(Extracted, TEXT("Constant_Roughness")).IsValid());
	}
	TestNoOpReapply(*this, Target, TEXT("after create"));
	if (!TestTrue(TEXT("a changed number applies"), Apply(*this, Target, Document(Target, MaterialClass, TEXT("Update"), Body(TEXT("0.3"))),
		TEXT("change")).IsSuccess()))
	{
		return false;
	}
	UMaterialExpressionConstant* Changed = ConstantOn(Target.Get<UMaterial>(), &UMaterialEditorOnlyData::Roughness);
	TestTrue(TEXT("the same node holds the new value"), Changed != nullptr && Changed->GetName() == TEXT("Constant_Roughness") && Changed->R == 0.3f);
	if (!TestTrue(TEXT("a source instead of the number applies"), Apply(*this, Target,
		Document(Target, MaterialClass, TEXT("Update"), Body(TEXT("\"Rough\""))), TEXT("source")).IsSuccess()))
	{
		return false;
	}
	UMaterial* Material = Target.Get<UMaterial>();
	TestTrue(TEXT("Roughness is wired to the parameter"), Material != nullptr
		&& Material->GetEditorOnlyData()->Roughness.Expression == FindByName(Material, TEXT("Rough")));
	TestNull(TEXT("the implicit constant is deleted"), FindByName(Material, TEXT("Constant_Roughness")));
	TestNoOpReapply(*this, Target, TEXT("after the source"));
	return !HasAnyErrors();
}
#endif
