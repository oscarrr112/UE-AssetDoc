// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// rig.controlrig.edit.v1 ops called directly. node.set position / size / title / color and function.set color
// have no "reset to default" in the editor (the controller and the Details panel take a value); a null there was
// dropped without a word and the op succeeded. It is refused at the op's field, and the node is left as it was.
//   ControlRig.NullFieldsRefused (CR_AllKinds: its comment node "Comment" and its function "Scale Weight")

#include "AssetDocumentIKRigTestSupport.h"
#include "../../../AssetDocumentGraph/Private/Tests/AssetDocumentGraphTestSupport.h"

#include "AssetDocumentRigProvider.h"
#include "ControlRigBlueprintLegacy.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMNode.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentControlRigTests
{
UControlRigBlueprint* MakeAllKinds(FAutomationTestBase& Test, const AssetDocumentIKRigTests::FTarget& Target);
} // namespace AssetDocumentControlRigTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigNullFieldsRefusedTest, "UEAssetDoc.ControlRig.NullFieldsRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigNullFieldsRefusedTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentIKRigTests;
	using namespace UE::AssetDocument::Graph::Tests;
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("CR_NullFields"));
	UControlRigBlueprint* Rig = AssetDocumentControlRigTests::MakeAllKinds(*this, Target);
	URigVMGraph* Model = Rig != nullptr ? Rig->GetDefaultModel() : nullptr;
	URigVMNode* Comment = Model != nullptr ? Model->FindNodeByName(TEXT("Comment")) : nullptr;
	if (!TestNotNull(TEXT("CR_AllKinds has its comment node"), Comment))
	{
		return false;
	}
	const FVector2D Position = Comment->GetPosition();
	const FVector2D Size = Comment->GetSize();
	const FLinearColor Color = Comment->GetNodeColor();
	FSession Session(*this, *Rig, CreateAssetDocumentRigPrimitiveProvider());
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	for (const TCHAR* Field : {TEXT("position"), TEXT("size"), TEXT("title"), TEXT("color")})
	{
		const FString Ops = FString::Printf(TEXT("{\"ops\":[{\"op\":\"node.set\",\"graph\":\"Rig\",\"name\":\"Comment\",\"%s\":null}]}"), Field);
		if (Session.ExpectFailure(TEXT("rig.controlrig.edit.v1"), Ops, TEXT("ControlRig.InvalidValue"), Diagnostic))
		{
			TestTrue(*FString::Printf(TEXT("node.set %s null is refused at %s (%s)"), Field, Field, *Diagnostic.Path),
				Diagnostic.Path.EndsWith(FString(TEXT("/")) + Field) && Diagnostic.Message.Contains(TEXT("is null")));
		}
	}
	TestTrue(TEXT("the comment node is unchanged"), Comment->GetPosition().Equals(Position) && Comment->GetSize().Equals(Size)
		&& Comment->GetNodeColor().Equals(Color));

	const FString FunctionOps = TEXT("{\"ops\":[{\"op\":\"function.set\",\"graph\":[\"Functions\",\"Scale Weight\"],\"color\":null}]}");
	if (Session.ExpectFailure(TEXT("rig.controlrig.edit.v1"), FunctionOps, TEXT("ControlRig.InvalidValue"), Diagnostic))
	{
		TestTrue(*FString::Printf(TEXT("function.set color null is refused at color (%s)"), *Diagnostic.Path),
			Diagnostic.Path.EndsWith(TEXT("/color")) && Diagnostic.Message.Contains(TEXT("is null")));
	}
	return !HasAnyErrors();
}

#endif
