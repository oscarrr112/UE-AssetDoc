// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentBlackboardFootprintProjection.h"

#include "Misc/AutomationTest.h"

namespace AssetDocumentBlackboardFootprintProjectionTests
{
FAssetDocumentRuntimeValueDto StringValue(const TCHAR* Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = Value;
	return Result;
}

FAssetDocumentRuntimeValueDto IntegerValue(int64 Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Integer;
	Result.IntegerValue = Value;
	return Result;
}

FAssetDocumentRuntimeValueDto ObjectValue(
	TConstArrayView<FString> Keys,
	TConstArrayView<FAssetDocumentRuntimeValueDto> Values)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Object;
	Result.ObjectKeys.Append(Keys.GetData(), Keys.Num());
	Result.Elements.Append(Values.GetData(), Values.Num());
	return Result;
}

FAssetDocumentRuntimeValueDto ArrayValue(TConstArrayView<FAssetDocumentRuntimeValueDto> Values)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Array;
	Result.Elements.Append(Values.GetData(), Values.Num());
	return Result;
}

FAssetDocumentRuntimeValueDto RawRow(const TCHAR* StableId, const TCHAR* ComponentId)
{
	return ObjectValue(
		{TEXT("Kind"), TEXT("StableId"), TEXT("RollbackComponentId")},
		{StringValue(TEXT("UObjectState")), StringValue(StableId), StringValue(ComponentId)});
}

FAssetDocumentRuntimeValueDto Step(int64 Index, const TCHAR* PrimitiveId, TConstArrayView<FAssetDocumentRuntimeValueDto> RawRows)
{
	return ObjectValue(
		{TEXT("Index"), TEXT("PrimitiveId"), TEXT("RawFootprint")},
		{IntegerValue(Index), StringValue(PrimitiveId), ArrayValue(RawRows)});
}

FAssetDocumentRuntimeValueDto MakeProjectionValue(const FAssetDocumentRuntimeRequestDto& Request, const FString& SemanticHash)
{
	const FAssetDocumentRuntimeValueDto ParentRows[] = {
		RawRow(TEXT("asset:/Game/AI/BB_Target.BB_Target/Keys[*]"), TEXT("assetdocument.ai.blackboard.parent.derived_closure.v2")),
		RawRow(TEXT("asset:/Game/AI/BB_Target.BB_Target/Parent"), TEXT("assetdocument.ai.blackboard.parent.reference.v2")),
	};
	const FAssetDocumentRuntimeValueDto StructuralRows[] = {
		RawRow(TEXT("asset:/Game/AI/BB_Target.BB_Target/Keys[*]"), TEXT("assetdocument.ai.blackboard.entry.replace_class.v2")),
	};
	const FAssetDocumentRuntimeValueDto Steps[] = {
		Step(0, TEXT("ai.blackboard.parent.set.v2"), ParentRows),
		Step(1, TEXT("ai.blackboard.entry.replace_class.v2"), StructuralRows),
		Step(2, TEXT("ai.blackboard.entry.add.v2"), {RawRow(TEXT("asset:/Game/AI/BB_Target.BB_Target/Keys[*]"), TEXT("assetdocument.ai.blackboard.entry.add.v2"))}),
		Step(3, TEXT("ai.blackboard.entry.remove.v2"), {RawRow(TEXT("asset:/Game/AI/BB_Target.BB_Target/Keys[*]"), TEXT("assetdocument.ai.blackboard.entry_remove.v2"))}),
		Step(4, TEXT("ai.blackboard.local_keys.reorder.v2"), {RawRow(TEXT("asset:/Game/AI/BB_Target.BB_Target/Keys[*]"), TEXT("assetdocument.ai.blackboard.local_keys_reorder.v2"))}),
	};
	return ObjectValue(
		{TEXT("Kind"), TEXT("RequestId"), TEXT("RuntimeRevision"), TEXT("ExactAssetClassPath"), TEXT("TargetAssetPath"), TEXT("Operation"), TEXT("SemanticHash"), TEXT("Steps")},
		{StringValue(TEXT("BlackboardFootprintProjectionV1")), StringValue(*Request.RequestId.ToString(EGuidFormats::DigitsWithHyphensLower)), IntegerValue(Request.ExpectedRuntimeRevision), StringValue(TEXT("/Script/AIModule.BlackboardData")), StringValue(*Request.TargetAssetPath), StringValue(TEXT("Apply")), StringValue(*SemanticHash), ArrayValue(Steps)});
}
} // namespace AssetDocumentBlackboardFootprintProjectionTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBlackboardFootprintProjectionTest,
	"UEAssetDoc.Python.BlackboardData.PreflightFootprintHandoff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBlackboardFootprintProjectionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentBlackboardFootprintProjectionTests;
	// RED contract: the private parser must accept only the closed Python projection,
	// preserve raw rows, and derive unique operation-qualified receipt identities.
	FAssetDocumentRuntimeRequestDto Request;
	Request.RequestId = FGuid::NewGuid();
	Request.Operation = EAssetDocumentRuntimeOperation::Apply;
	Request.ExpectedRuntimeRevision = 17;
	Request.RequestedAssetClassPath = TEXT("/Script/AIModule.BlackboardData");
	Request.TargetAssetPath = TEXT("/Game/AI/BB_Target.BB_Target");
	FAssetDocumentRuntimeResultDto Preflight;
	Preflight.RequestId = Request.RequestId;
	Preflight.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Succeeded;
	Preflight.FinalPhase = EAssetDocumentRuntimeRequestPhase::Complete;
	Preflight.RuntimeRevision = Request.ExpectedRuntimeRevision;
	Preflight.SemanticHash = TEXT("sha256:bb-projection");
	Preflight.CanonicalValue = MakeProjectionValue(Request, Preflight.SemanticHash);
	FAssetDocumentBlackboardFootprintProjection Projection;
	const FAssetDocumentContractValidationResult Result = ParseAssetDocumentBlackboardFootprintProjection(Request, Preflight, Projection);
	TestTrue(TEXT("valid closed Blackboard projection parses"), Result.bValid);
	if (!Result.bValid) return false;
	TestEqual(TEXT("parent projection keeps two raw rows"), Projection.Steps[0].RawFootprint.Num(), 2);
	TestNotEqual(TEXT("qualified collisions are unique"), Projection.Steps[1].QualifiedFootprint[0].StableId, Projection.Steps[2].QualifiedFootprint[0].StableId);
	TestEqual(TEXT("aggregate qualified receipt rows preserve every raw row"), Projection.GetQualifiedFootprint().Num(), 6);
	FAssetDocumentRuntimeResultDto Drifted = Preflight;
	Drifted.RequestId = FGuid::NewGuid();
	TestFalse(TEXT("request identity drift fails closed"),
		ParseAssetDocumentBlackboardFootprintProjection(Request, Drifted, Projection).bValid);
	FAssetDocumentRuntimeResultDto Extra = Preflight;
	Extra.CanonicalValue->ObjectKeys.Add(TEXT("Unexpected"));
	Extra.CanonicalValue->Elements.Add(StringValue(TEXT("no aliases")));
	TestFalse(TEXT("extra closed-schema key fails"), ParseAssetDocumentBlackboardFootprintProjection(Request, Extra, Projection).bValid);
	FAssetDocumentRuntimeResultDto UnknownPrimitive = Preflight;
	UnknownPrimitive.CanonicalValue->Elements[7].Elements[0].Elements[1] = StringValue(TEXT("ai.blackboard.unknown.v2"));
	TestFalse(TEXT("unknown frozen primitive fails"), ParseAssetDocumentBlackboardFootprintProjection(Request, UnknownPrimitive, Projection).bValid);
	FAssetDocumentRuntimeResultDto RawKindDrift = Preflight;
	RawKindDrift.CanonicalValue->Elements[7].Elements[0].Elements[2].Elements[0].Elements[0] = StringValue(TEXT("Package"));
	TestFalse(TEXT("non UObjectState raw footprint fails"), ParseAssetDocumentBlackboardFootprintProjection(Request, RawKindDrift, Projection).bValid);
	return true;
}
