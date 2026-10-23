// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// The sidecar an apply writes back (the canonical document) is indented, two spaces per level,
// so that a person can read and hand-edit it after the first apply; it used to be one compact line. The layout is
// not content: re-applying the written sidecar is a no-op, and a hand edit of the indented file applies.

#include "AssetDocumentMaterialTestSupport.h"

#include "Materials/MaterialExpressionConstant.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSidecarIndentedWriteBackTest, "UEAssetDoc.Sidecar.IndentedWriteBack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSidecarIndentedWriteBackTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentMaterialTests;
	const FTarget Target(TEXT("M_SidecarLayout"));
	ON_SCOPE_EXIT
	{
		FinishCompilation();
		Target.Cleanup();
	};
	// One compact line, as a tool writes it.
	const FString Json = Document(Target, MaterialClass, TEXT("CreateOrUpdate"), TEXT(
		"\"Expressions\":{\"Rough\":{\"Class\":\"Constant\",\"Position\":[-400,0],\"Properties\":{\"R\":0.25}}},\"Outputs\":{\"Roughness\":\"Rough\"}"));
	if (!TestTrue(TEXT("create applies"), Apply(*this, Target, Json, TEXT("create")).IsSuccess()))
	{
		return false;
	}
	FString Written;
	if (!TestTrue(TEXT("the sidecar is there"), FFileHelper::LoadFileToString(Written, *Target.SidecarPath)))
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("written sidecar:\n%s"), *Written.Left(2000)));
	TestTrue(TEXT("the written sidecar is indented, one member per line"), Written.StartsWith(TEXT("{\n  \"")));
	TestTrue(TEXT("a short array of numbers stays on one line"), Written.Contains(TEXT("\"Position\": [-400, 0]")));
	TestTrue(TEXT("the file ends with a newline"), Written.EndsWith(TEXT("}\n")));
	TestTrue(TEXT("the written sidecar is the same document"), Parse(Written).IsValid()
		&& Field(Field(Parse(Written), TEXT("Body")), TEXT("Expressions")).IsValid());

	// Applying the written file as it is: nothing to do.
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Target.SidecarPath;
	Request.bSaveAsset = true;
	const FAssetDocumentResult Reapplied = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
	LogResult(*this, TEXT("re-apply"), Reapplied);
	TestTrue(TEXT("the written sidecar re-applies"), Reapplied.IsSuccess());
	TestFalse(TEXT("re-applying it saves nothing"), Reapplied.bSavedAsset);

	// A hand edit of the indented file applies.
	const FString Edited = Written.Replace(TEXT("\"R\": 0.25"), TEXT("\"R\": 0.75"));
	if (!TestNotEqual(TEXT("the value is in the written file"), Edited, Written))
	{
		return false;
	}
	FFileHelper::SaveStringToFile(Edited, *Target.SidecarPath);
	const FAssetDocumentResult HandEdit = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
	LogResult(*this, TEXT("hand edit"), HandEdit);
	TestTrue(TEXT("the hand-edited sidecar applies"), HandEdit.IsSuccess());
	const UMaterialExpressionConstant* Rough = nullptr;
	for (UMaterialExpression* Expression : GraphExpressions(Target.Get<UMaterial>()))
	{
		if (Expression->GetName() == TEXT("Rough"))
		{
			Rough = Cast<UMaterialExpressionConstant>(Expression);
		}
	}
	TestTrue(TEXT("the hand edit reached the asset"), Rough != nullptr && Rough->R == 0.75f);
	return !HasAnyErrors();
}
#endif
