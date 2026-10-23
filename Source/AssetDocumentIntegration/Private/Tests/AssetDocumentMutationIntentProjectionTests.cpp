// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "AssetDocumentMutationIntentProjection.h"
#include "Misc/AutomationTest.h"

namespace AssetDocumentMutationIntentProjectionTests
{
using FValue = FAssetDocumentRuntimeValueDto;
FValue Text(const TCHAR* Value)
{
	FValue Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = Value;
	return Result;
}
FValue Integer(int64 Value)
{
	FValue Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Integer;
	Result.IntegerValue = Value;
	return Result;
}
FValue Object(TConstArrayView<FString> Keys, TConstArrayView<FValue> Values)
{
	FValue Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Object;
	Result.ObjectKeys.Append(Keys.GetData(), Keys.Num());
	Result.Elements.Append(Values.GetData(), Values.Num());
	return Result;
}
FValue Array(TConstArrayView<FValue> Values)
{
	FValue Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Array;
	Result.Elements.Append(Values.GetData(), Values.Num());
	return Result;
}
FValue& Field(FValue& Value, const TCHAR* Name)
{
	return Value.Elements[Value.ObjectKeys.IndexOfByKey(Name)];
}
class FRegistry final : public IAssetDocumentPrimitiveRegistry
{
public:
	FAssetDocumentPrimitiveRegistrySnapshot State;
	FAssetDocumentPrimitiveResolution Resolution;
	virtual FAssetDocumentPrimitiveRegistrySnapshot Snapshot() const override { return State; }
	virtual FAssetDocumentPrimitiveResolution Resolve(const FAssetDocumentPrimitiveRequirement&) const override
	{
		return Resolution;
	}
};
struct FFixture
{
	FAssetDocumentRuntimeRequestDto Request;
	FAssetDocumentRuntimeResultDto Preflight;
	FRegistry Registry;
	TArray<FAssetDocumentPrimitiveRequirement> Requirements;
	FFixture()
	{
		Request.RequestId = FGuid(1, 2, 3, 4);
		Request.ExpectedRuntimeRevision = 31;
		Request.Operation = EAssetDocumentRuntimeOperation::Apply;
		Request.RequestedAssetClassPath = TEXT("/Script/Engine.AnimMontage");
		Request.TargetAssetPath = TEXT("/Game/Test.Test");
		Preflight.RequestId = Request.RequestId;
		Preflight.RuntimeRevision = 31;
		Preflight.FinalPhase = EAssetDocumentRuntimeRequestPhase::Complete;
		Preflight.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Succeeded;
		Preflight.SemanticHash = TEXT("sha256:validated");
		const FValue Intent = Object({TEXT("TargetAssetPath"), TEXT("Desired")}, {Text(*Request.TargetAssetPath), Integer(7)});
		const FValue Step = Object({TEXT("Index"), TEXT("PrimitiveId"), TEXT("Version"), TEXT("Phase"), TEXT("InputSchemaId"), TEXT("Intent")},
			{Integer(0), Text(TEXT("editor.test.v1")), Integer(1), Text(TEXT("Apply")), Text(TEXT("TestIntentV1")), Intent});
		Preflight.CanonicalValue = Object({TEXT("Kind"), TEXT("RequestId"), TEXT("RuntimeRevision"), TEXT("ExactAssetClassPath"),
			TEXT("TargetAssetPath"), TEXT("Operation"), TEXT("SemanticHash"), TEXT("Steps")},
			{Text(TEXT("MutationIntentProjectionV1")), Text(*Request.RequestId.ToString(EGuidFormats::DigitsWithHyphensLower)),
			Integer(31), Text(*Request.RequestedAssetClassPath), Text(*Request.TargetAssetPath), Text(TEXT("Apply")),
			Text(*Preflight.SemanticHash), Array({Step})});
		Requirements.Add({TEXT("editor.test.v1"), 1, TEXT("test.exact.consumer")});
		Registry.State.bFrozen = true;
		Registry.State.RuntimeRevision = 31;
		FAssetDocumentPrimitiveDescriptor Descriptor;
		Descriptor.PrimitiveId = TEXT("editor.test.v1");
		Descriptor.Version = 1;
		Descriptor.ProviderId = TEXT("editor");
		Descriptor.Owner = EAssetDocumentPrimitiveOwner::Editor;
		Descriptor.Access = EAssetDocumentPrimitiveAccess::Mutating;
		Descriptor.bRequiresActiveTransaction = true;
		Descriptor.Admission.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
		Descriptor.InputSchemaId = TEXT("TestIntentV1");
		Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair};
		Descriptor.RequiredConsumerIds = {TEXT("test.exact.consumer")};
		Registry.State.Descriptors.Add(Descriptor);
		FAssetDocumentProviderAvailability Provider;
		Provider.ProviderId = TEXT("editor");
		Provider.State = EAssetDocumentProviderAvailabilityState::Available;
		Registry.State.Providers.Add(Provider);
		Registry.Resolution.bResolved = true;
		Registry.Resolution.Descriptor = Descriptor;
		Registry.Resolution.ProviderId = TEXT("editor");
	}
	FValue& Root() { return Preflight.CanonicalValue.GetValue(); }
	FValue& Step() { return Field(Root(), TEXT("Steps")).Elements[0]; }
	FAssetDocumentContractValidationResult Parse(FAssetDocumentMutationIntentProjection& Out) const
	{
		return ParseAssetDocumentMutationIntentProjection(Request, Preflight, TEXT("sha256:validated"), Registry, Requirements, Out);
	}
};
void ExpectRejected(FAutomationTestBase& Test, const TCHAR* Label, const FFixture& Fixture)
{
	FAssetDocumentMutationIntentProjection Output;
	Output.RequestId = FGuid(9, 9, 9, 9);
	Output.Steps.AddDefaulted();
	const auto Result = Fixture.Parse(Output);
	Test.TestFalse(Label, Result.bValid);
	Test.TestTrue(TEXT("rejection provides diagnostic"), !Result.Diagnostics.IsEmpty());
	Test.TestTrue(TEXT("failure clears previous output identity"), !Output.RequestId.IsValid());
	Test.TestEqual(TEXT("failure clears previous output steps"), Output.Steps.Num(), 0);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMutationIntentProjectionExactTest,
	"UEAssetDoc.MutationIntentProjection.Exact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentMutationIntentProjectionExactTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentMutationIntentProjectionTests;
	FFixture Fixture;
	FAssetDocumentMutationIntentProjection Output;
	if (!TestTrue(TEXT("exact projection accepted"), Fixture.Parse(Output).bValid)) return false;
	TestTrue(TEXT("request identity preserved"), Output.RequestId == Fixture.Request.RequestId);
	TestEqual(TEXT("revision preserved"), Output.RuntimeRevision, uint64(31));
	TestEqual(TEXT("class preserved"), Output.ExactAssetClassPath, Fixture.Request.RequestedAssetClassPath);
	TestEqual(TEXT("target preserved"), Output.TargetAssetPath, Fixture.Request.TargetAssetPath);
	TestEqual(TEXT("hash preserved"), Output.SemanticHash, FString(TEXT("sha256:validated")));
	TestEqual(TEXT("one step"), Output.Steps.Num(), 1);
	TestEqual(TEXT("index preserved"), Output.Steps[0].StepIndex, 0);
	TestEqual(TEXT("primitive preserved"), Output.Steps[0].Requirement.PrimitiveId, FString(TEXT("editor.test.v1")));
	TestEqual(TEXT("consumer comes from exact requirement"), Output.Steps[0].Requirement.RequiredByRouteId, FString(TEXT("test.exact.consumer")));
	TestEqual(TEXT("schema preserved"), Output.Steps[0].InputSchemaId, FString(TEXT("TestIntentV1")));
	TestEqual(TEXT("intent value preserved"), Field(Output.Steps[0].Intent, TEXT("Desired")).IntegerValue, int64(7));
	Field(Fixture.Step(), TEXT("Phase")) = Text(TEXT("Repair"));
	TestTrue(TEXT("admitted Repair accepted"), Fixture.Parse(Output).bValid);
	TestTrue(TEXT("Repair phase preserved"), Output.Steps[0].Phase == EAssetDocumentRuntimeRequestPhase::Repair);
	Field(Fixture.Root(), TEXT("Steps")) = Array({});
	TestTrue(TEXT("empty plan accepted for package-only preparation"), Fixture.Parse(Output).bValid);
	TestEqual(TEXT("empty plan has no invented steps"), Output.Steps.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMutationIntentProjectionShapeTest,
	"UEAssetDoc.MutationIntentProjection.ClosedIdentityAndIntent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentMutationIntentProjectionShapeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentMutationIntentProjectionTests;
	FAssetDocumentMutationIntentProjection BaselineOutput;
	if (!TestTrue(TEXT("negative shape scenarios start from an accepted fixture"), FFixture().Parse(BaselineOutput).bValid)) return false;
	for (const TCHAR* Identity : {TEXT("Kind"), TEXT("RequestId"), TEXT("ExactAssetClassPath"), TEXT("TargetAssetPath"), TEXT("Operation"), TEXT("SemanticHash")})
	{
		FFixture Fixture;
		Field(Fixture.Root(), Identity) = Text(TEXT("drift"));
		ExpectRejected(*this, Identity, Fixture);
	}
	FFixture Revision; Field(Revision.Root(), TEXT("RuntimeRevision")) = Integer(32);
	ExpectRejected(*this, TEXT("projection revision drift"), Revision);
	FFixture Request; Request.Preflight.RequestId = FGuid(5, 6, 7, 8);
	ExpectRejected(*this, TEXT("preflight request drift"), Request);
	FFixture Hash; Hash.Preflight.SemanticHash = TEXT("not-validated");
	ExpectRejected(*this, TEXT("preflight must bind validated hash"), Hash);
	FFixture ExtraRoot; ExtraRoot.Root().ObjectKeys.Add(TEXT("Extra")); ExtraRoot.Root().Elements.Add(Text(TEXT("unexpected")));
	ExpectRejected(*this, TEXT("closed root"), ExtraRoot);
	FFixture ExtraStep; ExtraStep.Step().ObjectKeys.Add(TEXT("RequiredByRouteId")); ExtraStep.Step().Elements.Add(Text(TEXT("consumer injection")));
	ExpectRejected(*this, TEXT("closed step"), ExtraStep);
	FFixture Duplicate; Duplicate.Root().ObjectKeys[0] = TEXT("RequestId");
	ExpectRejected(*this, TEXT("duplicate root key"), Duplicate);
	FFixture Storage; Storage.Root().Elements.Pop();
	ExpectRejected(*this, TEXT("invalid object storage"), Storage);
	FFixture Index; Field(Index.Step(), TEXT("Index")) = Integer(1);
	ExpectRejected(*this, TEXT("noncontinuous step index"), Index);
	FFixture Phase; Field(Phase.Step(), TEXT("Phase")) = Text(TEXT("Persist"));
	ExpectRejected(*this, TEXT("nonmutating phase"), Phase);
	FFixture Schema; Field(Schema.Step(), TEXT("InputSchemaId")) = Text(TEXT("OtherIntentV1"));
	ExpectRejected(*this, TEXT("wrong input schema"), Schema);
	FFixture Version; Field(Version.Step(), TEXT("Version")) = Integer(0);
	ExpectRejected(*this, TEXT("invalid version"), Version);
	for (const TCHAR* Reserved : {TEXT("Body"), TEXT("OpaqueHandle"), TEXT("Address"), TEXT("RollbackCode"), TEXT("Slot")})
	{
		FFixture Intent;
		Field(Intent.Step(), TEXT("Intent")) = Object({TEXT("Nested")}, {Object({Reserved}, {Text(TEXT("forbidden"))})});
		ExpectRejected(*this, Reserved, Intent);
	}
	// A valid earlier step must not leak into output when a later step fails.
	FFixture Later;
	FValue InvalidStep = Later.Step();
	Field(InvalidStep, TEXT("Index")) = Integer(1);
	Field(InvalidStep, TEXT("Phase")) = Text(TEXT("Verify"));
	Field(Later.Root(), TEXT("Steps")).Elements.Add(InvalidStep);
	ExpectRejected(*this, TEXT("later rejection clears all parsed steps"), Later);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMutationIntentProjectionRegistryTest,
	"UEAssetDoc.MutationIntentProjection.FrozenRegistry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentMutationIntentProjectionRegistryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentMutationIntentProjectionTests;
	FAssetDocumentMutationIntentProjection BaselineOutput;
	if (!TestTrue(TEXT("negative registry scenarios start from an accepted fixture"), FFixture().Parse(BaselineOutput).bValid)) return false;
	FFixture Unfrozen; Unfrozen.Registry.State.bFrozen = false;
	ExpectRejected(*this, TEXT("unfrozen registry"), Unfrozen);
	FFixture Revision; Revision.Registry.State.RuntimeRevision = 32;
	ExpectRejected(*this, TEXT("registry revision drift"), Revision);
	FFixture Missing; Missing.Requirements.Empty();
	ExpectRejected(*this, TEXT("missing requirement"), Missing);
	FFixture Ambiguous; Ambiguous.Requirements.Add({TEXT("editor.test.v1"), 1, TEXT("other.consumer")});
	ExpectRejected(*this, TEXT("ambiguous requirement cannot guess consumer"), Ambiguous);
	FFixture Unresolved; Unresolved.Registry.Resolution.bResolved = false;
	ExpectRejected(*this, TEXT("unresolved primitive"), Unresolved);
	FFixture Consumer; Consumer.Registry.Resolution.Descriptor.RequiredConsumerIds.Empty();
	ExpectRejected(*this, TEXT("consumer drift"), Consumer);
	FFixture Provider; Provider.Registry.Resolution.ProviderId = TEXT("animation");
	ExpectRejected(*this, TEXT("provider drift"), Provider);
	FFixture Phase; Phase.Registry.Resolution.Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Repair};
	ExpectRejected(*this, TEXT("unadmitted Apply phase"), Phase);
	FFixture ReadOnly; ReadOnly.Registry.Resolution.Descriptor.Access = EAssetDocumentPrimitiveAccess::ReadOnly;
	ExpectRejected(*this, TEXT("readonly descriptor cannot become a mutation"), ReadOnly);
	FFixture Blocker; Blocker.Registry.Resolution.Descriptor.Admission.Disposition = EAssetDocumentPrimitiveDisposition::StableBlocker;
	ExpectRejected(*this, TEXT("blocker cannot become callable"), Blocker);
	FFixture FrozenProvider; FrozenProvider.Registry.State.Descriptors[0].ProviderId = TEXT("animation");
	ExpectRejected(*this, TEXT("resolution differs from snapshot provider"), FrozenProvider);
	FFixture FrozenSchema; FrozenSchema.Registry.State.Descriptors[0].InputSchemaId = TEXT("OtherSchema");
	ExpectRejected(*this, TEXT("resolution differs from snapshot schema"), FrozenSchema);
	FFixture Unavailable; Unavailable.Registry.State.Providers[0].State = EAssetDocumentProviderAvailabilityState::Unavailable;
	ExpectRejected(*this, TEXT("unavailable provider"), Unavailable);
	FFixture Duplicate;
	const auto DuplicateDescriptor = Duplicate.Registry.State.Descriptors[0];
	Duplicate.Registry.State.Descriptors.Add(DuplicateDescriptor);
	ExpectRejected(*this, TEXT("duplicate frozen descriptor"), Duplicate);
	return true;
}
