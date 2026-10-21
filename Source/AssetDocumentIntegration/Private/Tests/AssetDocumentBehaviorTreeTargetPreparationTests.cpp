// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentBehaviorTreeOperationClassifier.h"
#include "AssetDocumentGenericMutationPreparation.h"

#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentBehaviorTreeTargetPreparationTests
{
FAssetDocumentRuntimeValueDto MakeString(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto MakeNull()
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Null;
	return Result;
}

FAssetDocumentRuntimeValueDto MakeObject(
	TArray<FString> Keys,
	TArray<FAssetDocumentRuntimeValueDto> Values)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Object;
	Result.ObjectKeys = MoveTemp(Keys);
	Result.Elements = MoveTemp(Values);
	return Result;
}

FAssetDocumentRuntimeValueDto MakeDocument(
	FString Action,
	FAssetDocumentRuntimeValueDto Body,
	TOptional<FAssetDocumentRuntimeValueDto> Properties)
{
	TArray<FString> Keys{TEXT("Action"), TEXT("Body")};
	TArray<FAssetDocumentRuntimeValueDto> Values{
		MakeString(MoveTemp(Action)), MoveTemp(Body)};
	if (Properties.IsSet())
	{
		Keys.Add(TEXT("Properties"));
		Values.Add(MoveTemp(Properties.GetValue()));
	}
	return MakeObject(MoveTemp(Keys), MoveTemp(Values));
}

class FNoopVerifier final : public IAssetDocumentStagedArtifactVerifier
{
public:
	FAssetDocumentFreshVerificationResult VerifyFresh(
		const FAssetDocumentFreshVerificationInput& Input) override
	{
		(void)Input;
		return {};
	}
};

FAssetDocumentRuntimeResultDto MakeValidated(
	const FAssetDocumentRuntimeRequestDto& Request)
{
	FAssetDocumentRuntimeResultDto Result;
	Result.RequestId = Request.RequestId;
	Result.RuntimeRevision = Request.ExpectedRuntimeRevision;
	Result.SemanticHash = TEXT("sha256:behavior-tree-target-preparation");
	Result.CodecVersion = 1;
	Result.CanonicalBytes = {0x7b, 0x7d};
	return Result;
}

FAssetDocumentRuntimeRequestDto MakeRequest(
	const FString& Target,
	FAssetDocumentRuntimeValueDto Document)
{
	FAssetDocumentRuntimeRequestDto Request;
	Request.RequestId = FGuid::NewGuid();
	Request.Operation = EAssetDocumentRuntimeOperation::Apply;
	Request.TargetAssetPath = Target;
	Request.RequestedAssetClassPath = TEXT("/Script/AIModule.BehaviorTree");
	Request.CanonicalDocument = MoveTemp(Document);
	Request.ExpectedRuntimeRevision = 1;
	return Request;
}

FAssetDocumentGenericMutationPreparationResult Prepare(
	const FAssetDocumentRuntimeRequestDto& Request,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	FAssetDocumentApplyFileRequest ServiceRequest;
	ServiceRequest.FilePath = TEXT("/tmp/BehaviorTreeTargetPreparation.assetdoc.json");
	static FNoopVerifier Verifier;
	const FAssetDocumentRuntimeResultDto Validated = MakeValidated(Request);
	return FAssetDocumentGenericMutationPreparation::Prepare(
		ServiceRequest, Request, Validated, Validated, Verifier, OutDiagnostics);
}

bool HasDiagnostic(
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics,
	FStringView Code)
{
	return Diagnostics.ContainsByPredicate([Code](const FAssetDocumentRuntimeDiagnostic& Item)
	{
		return Item.Code == Code;
	});
}

FString DescribeDiagnostics(
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics)
{
	TArray<FString> Codes;
	for (const FAssetDocumentRuntimeDiagnostic& Item : Diagnostics)
	{
		Codes.Add(Item.Code);
	}
	return Codes.IsEmpty() ? TEXT("<none>") : FString::Join(Codes, TEXT(", "));
}

FAssetDocumentRuntimeValueDto MakeFullBody()
{
	FAssetDocumentRuntimeValueDto EmptyArray;
	EmptyArray.Kind = EAssetDocumentRuntimeValueKind::Array;
	return MakeObject({TEXT("Tree")}, {MakeObject(
		{TEXT("Comments"), TEXT("Nodes")}, {EmptyArray, EmptyArray})});
}

FAssetDocumentRuntimeValueDto MakeSparseBody()
{
	return MakeObject(
		{TEXT("BlackboardAsset")},
		{MakeString(TEXT("/Game/AI/BB_TargetPreparation"))});
}

TOptional<FAssetDocumentRuntimeValueDto> MakeProperties(int32 Mode)
{
	if (Mode == 0)
	{
		return {};
	}
	if (Mode == 1)
	{
		return MakeObject({}, {});
	}
	return MakeObject({TEXT("BlackboardAsset")}, {MakeNull()});
}

UObject* CreateExistingBehaviorTreeTarget(const FString& Target)
{
	UClass* const AssetClass = FSoftClassPath(
		TEXT("/Script/AIModule.BehaviorTree")).TryLoadClass<UObject>();
	if (AssetClass == nullptr)
	{
		return nullptr;
	}
	const FString PackageName = FSoftObjectPath(Target).GetLongPackageName();
	UPackage* const Package = CreatePackage(*PackageName);
	const FString AssetName = FSoftObjectPath(Target).GetAssetName();
	UObject* const Asset = NewObject<UObject>(
		Package, AssetClass, FName(*AssetName), RF_Public | RF_Standalone);
	if (Package != nullptr)
	{
		Package->SetDirtyFlag(false);
	}
	return Asset;
}

void TestClassifierMatrix(FAutomationTestBase& Test)
{
	const FAssetDocumentRuntimeValueDto Full = MakeDocument(
		TEXT("Create"),
		MakeObject({TEXT("Tree")}, {MakeObject({}, {})}),
		TOptional<FAssetDocumentRuntimeValueDto>());
	const FAssetDocumentRuntimeValueDto Sparse = MakeDocument(
		TEXT("Create"),
		MakeObject({TEXT("BlackboardAsset")}, {MakeString(TEXT("/Game/AI/BB_Profile"))}),
		TOptional<FAssetDocumentRuntimeValueDto>());
	const FAssetDocumentRuntimeValueDto SparseNull = MakeDocument(
		TEXT("Create"),
		MakeObject({TEXT("BlackboardAsset")}, {MakeNull()}),
		TOptional<FAssetDocumentRuntimeValueDto>());
	const FAssetDocumentRuntimeValueDto InvalidSparse = MakeDocument(
		TEXT("Create"),
		MakeObject({TEXT("BlackboardAsset")}, {MakeObject({}, {})}),
		TOptional<FAssetDocumentRuntimeValueDto>());
	const FAssetDocumentRuntimeValueDto EmptyProperties = MakeDocument(
		TEXT("Create"),
		MakeObject({TEXT("Tree")}, {MakeObject({}, {})}), MakeObject({}, {}));
	const FAssetDocumentRuntimeValueDto NonEmptyProperties = MakeDocument(
		TEXT("Create"),
		MakeObject({TEXT("Tree")}, {MakeObject({}, {})}),
		MakeObject({TEXT("DisplayName")}, {MakeString(TEXT("BT"))}));
	const FString BehaviorTreeClass = TEXT("/Script/AIModule.BehaviorTree");
	const FAssetDocumentBehaviorTreeOperationClassification FullOmitted =
		ClassifyAssetDocumentBehaviorTreeOperation(BehaviorTreeClass, Full);
	Test.TestEqual(TEXT("FullCreate is a complete lifecycle"), FullOmitted.Shape,
		EAssetDocumentBehaviorTreeOperationShape::FullTreeLifecycle);
	Test.TestEqual(TEXT("FullCreate omitted Properties is no-write"),
		FullOmitted.PropertyMode,
		EAssetDocumentBehaviorTreeTopLevelPropertyMode::NoTopLevelPropertyWrite);
	Test.TestEqual(TEXT("SparseCreate remains the retained-overlay shape"),
		ClassifyAssetDocumentBehaviorTreeOperation(BehaviorTreeClass, Sparse).Shape,
		EAssetDocumentBehaviorTreeOperationShape::RetainedTreeOverlay);
	Test.TestEqual(TEXT("null BlackboardAsset remains the retained-overlay shape"),
		ClassifyAssetDocumentBehaviorTreeOperation(BehaviorTreeClass, SparseNull).Shape,
		EAssetDocumentBehaviorTreeOperationShape::RetainedTreeOverlay);
	Test.TestEqual(TEXT("empty Body retains the graph for explicit lifecycle refresh"),
		ClassifyAssetDocumentBehaviorTreeOperation(BehaviorTreeClass,
			MakeDocument(TEXT("Update"), MakeObject({}, {}), {})).Shape,
		EAssetDocumentBehaviorTreeOperationShape::RetainedTreeOverlay);
	Test.TestEqual(TEXT("unknown Body fields cannot become a retained overlay"),
		ClassifyAssetDocumentBehaviorTreeOperation(BehaviorTreeClass,
			MakeDocument(TEXT("Update"), MakeObject({TEXT("Unknown")}, {MakeNull()}), {})).Shape,
		EAssetDocumentBehaviorTreeOperationShape::InvalidBehaviorTreeShape);
	Test.TestEqual(TEXT("object BlackboardAsset is not a retained-overlay shape"),
		ClassifyAssetDocumentBehaviorTreeOperation(BehaviorTreeClass, InvalidSparse).Shape,
		EAssetDocumentBehaviorTreeOperationShape::InvalidBehaviorTreeShape);
	const auto BlackboardRef = MakeObject({TEXT("Kind"), TEXT("Path")},
		{MakeString(TEXT("AssetRef")), MakeString(TEXT("/Game/AI/BB_Profile.BB_Profile"))});
	Test.TestEqual(TEXT("canonical AssetRef retains the same Blackboard-only overlay"),
		ClassifyAssetDocumentBehaviorTreeOperation(BehaviorTreeClass, MakeDocument(TEXT("Update"),
			MakeObject({TEXT("BlackboardAsset")}, {BlackboardRef}), {})).Shape,
		EAssetDocumentBehaviorTreeOperationShape::RetainedTreeOverlay);
	for (const auto& InvalidRef : {
		MakeObject({TEXT("Kind"), TEXT("Path")}, {MakeString(TEXT("ClassRef")), MakeString(TEXT("/Game/AI/BB_Profile.BB_Profile"))}),
		MakeObject({TEXT("Kind")}, {MakeString(TEXT("AssetRef"))}),
		MakeObject({TEXT("Kind"), TEXT("Path"), TEXT("Extra")}, {MakeString(TEXT("AssetRef")), MakeString(TEXT("/Game/AI/BB_Profile.BB_Profile")), MakeNull()})})
		Test.TestEqual(TEXT("malformed AssetRef cannot become an overlay footprint"),
			ClassifyAssetDocumentBehaviorTreeOperation(BehaviorTreeClass, MakeDocument(TEXT("Update"),
				MakeObject({TEXT("BlackboardAsset")}, {InvalidRef}), {})).Shape,
			EAssetDocumentBehaviorTreeOperationShape::InvalidBehaviorTreeShape);
	Test.TestEqual(TEXT("explicit empty Properties remains no-write"),
		ClassifyAssetDocumentBehaviorTreeOperation(BehaviorTreeClass, EmptyProperties).PropertyMode,
		EAssetDocumentBehaviorTreeTopLevelPropertyMode::NoTopLevelPropertyWrite);
	Test.TestEqual(TEXT("non-empty Properties remains generic-owned"),
		ClassifyAssetDocumentBehaviorTreeOperation(BehaviorTreeClass, NonEmptyProperties).PropertyMode,
		EAssetDocumentBehaviorTreeTopLevelPropertyMode::GenericTopLevelPropertyWrite);
}

void TestGenericPreparationMatrix(FAutomationTestBase& Test)
{
	const FString Unique = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Root = TEXT("/Game/AssetDocumentTests/BehaviorTreeTargetPreparation/") + Unique;
	const FString ExistingTarget = Root + TEXT("/BT_Existing.BT_Existing");
	const FString MissingTarget = Root + TEXT("/BT_Missing.BT_Missing");
	UObject* const Existing = CreateExistingBehaviorTreeTarget(ExistingTarget);
	if (!Test.TestNotNull(TEXT("matrix creates an exact existing BehaviorTree"), Existing))
	{
		return;
	}
	for (const FString& Action : {TEXT("Create"), TEXT("Update"), TEXT("CreateOrUpdate")})
	{
		for (const bool bFull : {true, false})
		{
			const TArray<bool> TargetPresence = Action == TEXT("Create")
				? TArray<bool>{false}
				: TArray<bool>{true, false};
			for (const bool bExistingTarget : TargetPresence)
			{
				for (int32 PropertyMode = 0; PropertyMode != 3; ++PropertyMode)
				{
					// CreateOrUpdate of an absent target is a Create.
					const bool bSparseCreate = !bFull && (Action == TEXT("Create")
						|| (Action == TEXT("CreateOrUpdate") && !bExistingTarget));
					const bool bFactory = bFull && !bExistingTarget
						&& (Action == TEXT("Create")
							|| Action == TEXT("CreateOrUpdate"));
					const bool bExpectedAbsentTarget = !bExistingTarget
						&& !bFactory && !bSparseCreate;
					const FString& Target = bExistingTarget ? ExistingTarget : MissingTarget;
					TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
					const FAssetDocumentGenericMutationPreparationResult Prepared = Prepare(
						MakeRequest(Target, MakeDocument(
							Action,
							bFull ? MakeFullBody() : MakeSparseBody(),
							MakeProperties(PropertyMode))),
						Diagnostics);
					const FString Label = FString::Printf(
						TEXT("%s %s %s properties-%d"),
						*Action, bFull ? TEXT("full") : TEXT("sparse"),
						bExistingTarget ? TEXT("existing") : TEXT("absent"), PropertyMode);
					if (bSparseCreate)
					{
						Test.TestFalse(Label + TEXT(" rejects sparse Create"), Prepared.Mutation.IsValid());
						Test.TestTrue(Label + TEXT(" reports canonical-plan failure"),
							HasDiagnostic(Diagnostics,
								TEXT("GenericRuntime.BehaviorTreeFootprint.InvalidCanonicalPlan")));
						continue;
					}
					if (bExpectedAbsentTarget)
					{
						Test.TestFalse(Label + TEXT(" keeps an absent target out of Generic preparation"),
							Prepared.Mutation.IsValid());
						Test.TestTrue(Label + TEXT(" reports the exact absent-target terminal"),
							HasDiagnostic(Diagnostics, TEXT("CutoverPreparation.InvalidTarget")));
						continue;
					}
					Test.TestTrue(Label + FString::Printf(
						TEXT(" prepares through Generic runtime (diagnostics: %s)"),
						*DescribeDiagnostics(Diagnostics)),
						Prepared.Mutation.IsValid());
					if (!Prepared.Mutation)
					{
						continue;
					}
					const int32 ExpectedRows = bFactory
						? (PropertyMode == 2 ? 7 : 5)
						: (PropertyMode == 2 ? 2 : 0);
					Test.TestEqual(Label + TEXT(" preserves Editor rows plus the exact BT carrier"),
						Prepared.Mutation->PreflightReceipt.CompleteFootprint.Num(), ExpectedRows + (bFull ? 4 : 3));
					Test.TestEqual(Label + TEXT(" has matching rollback ownership"),
						Prepared.Mutation->RollbackActions.Num(), ExpectedRows == 0 ? 0 : (bFactory && PropertyMode == 2 ? 3 : (bFactory ? 1 : 2)));
				}
			}
		}
	}
	const auto BlackboardRef = MakeObject({TEXT("Kind"), TEXT("Path")},
		{MakeString(TEXT("AssetRef")), MakeString(TEXT("/Game/AI/BB_Profile.BB_Profile"))});
	TArray<FAssetDocumentRuntimeDiagnostic> RefDiagnostics;
	const auto RefPrepared = Prepare(MakeRequest(ExistingTarget, MakeDocument(TEXT("Update"),
		MakeObject({TEXT("BlackboardAsset")}, {BlackboardRef}), {})), RefDiagnostics);
	Test.TestTrue(TEXT("canonical AssetRef Update prepares the real retained target"), RefPrepared.Mutation.IsValid());
	if (RefPrepared.Mutation)
	{
		Test.TestEqual(TEXT("AssetRef overlay keeps the exact three BT footprint rows"),
			RefPrepared.Mutation->PreflightReceipt.CompleteFootprint.Num(), 3);
		Test.TestEqual(TEXT("AssetRef overlay keeps receipt and persistence row counts aligned"),
			RefPrepared.Mutation->PersistencePlanTemplate.Footprint.Num(), 3);
		Test.TestFalse(TEXT("AssetRef overlay does not claim graph replacement"),
			RefPrepared.Mutation->PreflightReceipt.CompleteFootprint.ContainsByPredicate([](const auto& Row)
				{ return Row.RollbackComponentId == TEXT("assetdocument.ai.behavior_tree.graph_install.v1"); }));
	}
	TArray<FAssetDocumentRuntimeDiagnostic> RefreshDiagnostics;
	const auto RefreshPrepared = Prepare(MakeRequest(ExistingTarget,
		MakeDocument(TEXT("Update"), MakeObject({}, {}), {})), RefreshDiagnostics);
	Test.TestTrue(TEXT("empty Body Update prepares the real retained target"), RefreshPrepared.Mutation.IsValid());
	if (RefreshPrepared.Mutation)
	{
		Test.TestEqual(TEXT("empty Body refresh keeps the exact two lifecycle footprint rows"),
			RefreshPrepared.Mutation->PreflightReceipt.CompleteFootprint.Num(), 2);
		Test.TestEqual(TEXT("empty Body refresh persistence rows match the receipt"),
			RefreshPrepared.Mutation->PersistencePlanTemplate.Footprint.Num(), 2);
		const FString AssetId = TEXT("asset:") + ExistingTarget;
		for (const auto* Rows : {&RefreshPrepared.Mutation->PreflightReceipt.CompleteFootprint,
			&RefreshPrepared.Mutation->PersistencePlanTemplate.Footprint})
		{
			Test.TestTrue(TEXT("empty Body refresh contains exactly the real selector-cache and completion rows"),
				Rows->Num() == 2 && Rows->ContainsByPredicate([&](const auto& Row)
				{
					return Row.Kind == EAssetDocumentMutationFootprintKind::UObjectState
						&& Row.StableId == AssetId + TEXT("/SelectorCache")
						&& Row.RollbackComponentId == TEXT("assetdocument.ai.behavior_tree.selector_cache.v1");
				}) && Rows->ContainsByPredicate([&](const auto& Row)
				{
					return Row.Kind == EAssetDocumentMutationFootprintKind::UObjectState
						&& Row.StableId == AssetId + TEXT("/DerivedCompletion")
						&& Row.RollbackComponentId == TEXT("assetdocument.ai.behavior_tree.derived_completion.v1");
				}));
			Test.TestFalse(TEXT("omitted BlackboardAsset declares no Blackboard write row"),
				Rows->ContainsByPredicate([&](const auto& Row)
				{
					return Row.StableId == AssetId + TEXT("/BlackboardAsset")
						|| Row.RollbackComponentId == TEXT("assetdocument.ai.behavior_tree.blackboard_asset.v1");
				}));
		}
		Test.TestFalse(TEXT("empty Body refresh does not claim graph replacement"),
			RefreshPrepared.Mutation->PreflightReceipt.CompleteFootprint.ContainsByPredicate([](const auto& Row)
				{ return Row.RollbackComponentId == TEXT("assetdocument.ai.behavior_tree.graph_install.v1"); }));
	}
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeTargetPreparationTest,
	"UEAssetDoc.Integration.BehaviorTree.TargetPreparation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeTargetPreparationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	AssetDocumentBehaviorTreeTargetPreparationTests::TestClassifierMatrix(*this);
	AssetDocumentBehaviorTreeTargetPreparationTests::TestGenericPreparationMatrix(*this);
	return !HasAnyErrors();
}
#endif
