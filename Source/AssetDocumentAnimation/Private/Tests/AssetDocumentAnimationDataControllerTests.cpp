// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#if WITH_DEV_AUTOMATION_TESTS

#include "Provider/Curve/AssetDocumentAnimationCurveOperations.h"

#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimCurveMetadata.h"
#include "Animation/AnimMontage.h"
#include "AssetDocumentAnimationPrimitiveProvider.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "AssetDocumentPluginPaths.h"

namespace AssetDocumentAnimationDataControllerTests
{
FAssetDocumentRuntimeValueDto MakeStringValue(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto MakeArrayValue(
	TArray<FAssetDocumentRuntimeValueDto> Elements)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Array;
	Result.Elements = MoveTemp(Elements);
	return Result;
}

FAssetDocumentRuntimeValueDto MakeObjectValue(
	TArray<FString> Keys,
	TArray<FAssetDocumentRuntimeValueDto> Elements)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Object;
	Result.ObjectKeys = MoveTemp(Keys);
	Result.Elements = MoveTemp(Elements);
	return Result;
}

FAssetDocumentPrimitiveInvocation MakeCurveInvocation(
	TArray<FAssetDocumentRuntimeValueDto> CurveNames)
{
	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = TEXT("animation.curve.float.enumerate.v1");
	Invocation.Version = 1;
	Invocation.RequestId = FGuid::NewGuid();
	Invocation.TransactionId = FGuid::NewGuid();
	Invocation.RuntimeRevision = 31;
	Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
	Invocation.ValueArguments = MakeObjectValue(
		{TEXT("curve_names")}, {MakeArrayValue(MoveTemp(CurveNames))});
	return Invocation;
}

const FAssetDocumentRuntimeValueDto* FindObjectField(
	const FAssetDocumentRuntimeValueDto& Object,
	FStringView Name)
{
	if (Object.Kind != EAssetDocumentRuntimeValueKind::Object
		|| Object.ObjectKeys.Num() != Object.Elements.Num())
	{
		return nullptr;
	}
	for (int32 Index = 0; Index < Object.ObjectKeys.Num(); ++Index)
	{
		if (Object.ObjectKeys[Index] == Name)
		{
			return &Object.Elements[Index];
		}
	}
	return nullptr;
}

bool HasDiagnostic(
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics,
	FStringView Code)
{
	return Diagnostics.ContainsByPredicate(
		[Code](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == Code;
		});
}

class FCurveEnumerationHandleResolver final : public IAssetDocumentHandleResolver
{
public:
	explicit FCurveEnumerationHandleResolver(
		const FAssetDocumentPrimitiveInvocation& InInvocation)
		: Invocation(InInvocation)
	{
	}

	virtual FAssetDocumentIssuedHandleResult Issue(
		EAssetDocumentHandleKind Kind,
		EAssetDocumentHandleAccess Access,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override
	{
		++IssueCount;
		FAssetDocumentIssuedHandleResult Result;
		if (!Payload || !Payload->IsAlive() || IssueCount == RejectedIssue)
		{
			FAssetDocumentRuntimeDiagnostic& Diagnostic =
				Result.Diagnostics.AddDefaulted_GetRef();
			Diagnostic.Path = TEXT("/Test/Issue");
			Diagnostic.Code = TEXT("TestHandle.IssueRejected");
			Diagnostic.Message = TEXT("The test resolver rejected curve-handle issuance.");
			return Result;
		}

		FAssetDocumentOpaqueHandle Handle;
		Handle.RequestId = Invocation.RequestId;
		Handle.TransactionId = Invocation.TransactionId;
		Handle.RuntimeRevision = Invocation.RuntimeRevision;
		Handle.ProviderId = TEXT("animation");
		Handle.Slot = NextSlot++;
		Handle.Generation = 1;
		Handle.Kind = Kind;
		Handle.Access = Access;
		IssuedHandles.Add(Handle);
		Result.Handle = Handle;
		return Result;
	}

	virtual FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(
		const FAssetDocumentOpaqueHandle&,
		const FAssetDocumentHandleValidationContext&) override
	{
		++AcquireCount;
		return {};
	}

	virtual FAssetDocumentHandleInvalidationResult Invalidate(
		const FAssetDocumentHandleInvalidation&) override
	{
		++InvalidationCount;
		return {};
	}

	void RejectIssueNumber(int32 InIssueNumber)
	{
		RejectedIssue = InIssueNumber;
	}

	int32 GetIssueCount() const { return IssueCount; }
	int32 GetAcquireCount() const { return AcquireCount; }
	int32 GetInvalidationCount() const { return InvalidationCount; }
	const TArray<FAssetDocumentOpaqueHandle>& GetIssuedHandles() const
	{
		return IssuedHandles;
	}

private:
	FAssetDocumentPrimitiveInvocation Invocation;
	TArray<FAssetDocumentOpaqueHandle> IssuedHandles;
	uint32 NextSlot = 1;
	int32 RejectedIssue = INDEX_NONE;
	int32 IssueCount = 0;
	int32 AcquireCount = 0;
	int32 InvalidationCount = 0;
};

struct FExpectedCurveKey
{
	float Time = 0.f;
	float Value = 0.f;
	float ArriveTangent = 0.f;
	float LeaveTangent = 0.f;
	ERichCurveInterpMode InterpMode = RCIM_Linear;
	ERichCurveTangentMode TangentMode = RCTM_Auto;
	ERichCurveTangentWeightMode TangentWeightMode = RCTWM_WeightedNone;
};

struct FExpectedCurve
{
	FName Name;
	int32 Flags = 0;
	TArray<FExpectedCurveKey> Keys;
};

FRichCurveKey MakeRichKey(const FExpectedCurveKey& Expected,
	float ArriveTangentWeight,
	float LeaveTangentWeight)
{
	FRichCurveKey Key;
	Key.Time = Expected.Time;
	Key.Value = Expected.Value;
	Key.ArriveTangent = Expected.ArriveTangent;
	Key.LeaveTangent = Expected.LeaveTangent;
	Key.InterpMode = Expected.InterpMode;
	Key.TangentMode = Expected.TangentMode;
	Key.TangentWeightMode = Expected.TangentWeightMode;
	Key.ArriveTangentWeight = ArriveTangentWeight;
	Key.LeaveTangentWeight = LeaveTangentWeight;
	return Key;
}

UAnimMontage* MakeCurveFixture(
	FAutomationTestBase& Test,
	TArray<FExpectedCurve>& OutExpectedCurves)
{
	UAnimMontage* Sequence = NewObject<UAnimMontage>(GetTransientPackage(),
		TEXT("AssetDocumentAnimationCurveDataModel"), RF_Transient);
	Test.TestNotNull(TEXT("transient sequence fixture exists"), Sequence);
	if (Sequence == nullptr)
	{
		return nullptr;
	}

	FExpectedCurve First;
	First.Name = TEXT("CurveFirst");
	First.Flags = AACF_Editable | AACF_Metadata;
	First.Keys = {
		{0.25f, 1.5f, 2.25f, -3.5f, RCIM_Cubic, RCTM_Break,
			RCTWM_WeightedBoth},
		{1.0f, -4.0f, 0.5f, 1.25f, RCIM_Linear, RCTM_User,
			RCTWM_WeightedArrive},
	};
	FExpectedCurve Second;
	Second.Name = TEXT("CurveSecond");
	Second.Flags = AACF_Editable;
	Second.Keys = {
		{0.5f, 7.0f, 0.0f, 0.0f, RCIM_Constant, RCTM_None,
			RCTWM_WeightedLeave},
	};

	IAnimationDataController& Controller = Sequence->GetController();
	Controller.OpenBracket(FText::FromString(TEXT("AssetDocument curve fixture")), false);
	Controller.InitializeModel();
	Controller.SetFrameRate(FFrameRate(30, 1), false);
	Controller.SetNumberOfFrames(FFrameNumber(60), false);
	for (const FExpectedCurve& Curve : {First, Second})
	{
		const FAnimationCurveIdentifier Identifier(
			Curve.Name, ERawCurveTrackTypes::RCT_Float);
		Test.TestTrue(FString::Printf(TEXT("fixture adds %s"), *Curve.Name.ToString()),
			Controller.AddCurve(Identifier, Curve.Flags, false));
		TArray<FRichCurveKey> Keys;
		for (int32 Index = 0; Index < Curve.Keys.Num(); ++Index)
		{
			Keys.Add(MakeRichKey(Curve.Keys[Index],
				0.75f + static_cast<float>(Index),
				1.75f + static_cast<float>(Index)));
		}
		Test.TestTrue(FString::Printf(TEXT("fixture sets %s keys"), *Curve.Name.ToString()),
			Controller.SetCurveKeys(Identifier, Keys, false));
		Test.TestTrue(FString::Printf(TEXT("fixture sets %s flags"), *Curve.Name.ToString()),
			Controller.SetCurveFlags(Identifier, Curve.Flags, false));
	}
	Controller.NotifyPopulated();
	Controller.CloseBracket(false);

	const IAnimationDataModel* Model = Sequence->GetDataModel();
	Test.TestNotNull(TEXT("fixture exposes the public animation data model"), Model);
	if (Model == nullptr)
	{
		return nullptr;
	}
	Test.TestEqual(TEXT("fixture preserves Engine float-curve order"),
		Model->GetFloatCurves().Num(), 2);
	if (Model->GetFloatCurves().Num() == 2)
	{
		Test.TestEqual(TEXT("fixture first curve is Engine first"),
			Model->GetFloatCurves()[0].GetName(), First.Name);
		Test.TestEqual(TEXT("fixture second curve is Engine second"),
			Model->GetFloatCurves()[1].GetName(), Second.Name);
	}
	OutExpectedCurves = {MoveTemp(First), MoveTemp(Second)};
	return Sequence;
}

void AssertExactKey(
	FAutomationTestBase& Test,
	const FAssetDocumentRuntimeValueDto& Actual,
	const FExpectedCurveKey& Expected)
{
	Test.TestEqual(TEXT("curve key has the exact frozen field order"), Actual.ObjectKeys,
		TArray<FString>{TEXT("time"), TEXT("value"), TEXT("arrive_tangent"),
			TEXT("leave_tangent"), TEXT("interp_mode"), TEXT("tangent_mode"),
			TEXT("tangent_weight_mode")});
	Test.TestEqual(TEXT("curve key has exactly seven values"), Actual.Elements.Num(), 7);
	if (Actual.Elements.Num() != 7)
	{
		return;
	}
	Test.TestEqual(TEXT("curve key time"), Actual.Elements[0].NumberValue,
		static_cast<double>(Expected.Time));
	Test.TestEqual(TEXT("curve key value"), Actual.Elements[1].NumberValue,
		static_cast<double>(Expected.Value));
	Test.TestEqual(TEXT("curve key arrive tangent"), Actual.Elements[2].NumberValue,
		static_cast<double>(Expected.ArriveTangent));
	Test.TestEqual(TEXT("curve key leave tangent"), Actual.Elements[3].NumberValue,
		static_cast<double>(Expected.LeaveTangent));
	Test.TestEqual(TEXT("curve key interpolation mode"), Actual.Elements[4].IntegerValue,
		static_cast<int64>(Expected.InterpMode));
	Test.TestEqual(TEXT("curve key tangent mode"), Actual.Elements[5].IntegerValue,
		static_cast<int64>(Expected.TangentMode));
	Test.TestEqual(TEXT("curve key tangent-weight mode"), Actual.Elements[6].IntegerValue,
		static_cast<int64>(Expected.TangentWeightMode));
}

void AssertCurveRows(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveOutcome& Outcome,
	const FCurveEnumerationHandleResolver& Handles,
	TConstArrayView<FExpectedCurve> ExpectedCurves)
{
	Test.TestEqual(TEXT("float-curve enumeration succeeds"), Outcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	Test.TestTrue(TEXT("read-only enumeration has no changed records"),
		Outcome.ChangedSet.IsEmpty());
	Test.TestTrue(TEXT("read-only enumeration has no diagnostics"),
		Outcome.Diagnostics.IsEmpty());
	Test.TestEqual(TEXT("read-only enumeration never reacquires a lease"),
		Handles.GetAcquireCount(), 0);
	Test.TestEqual(TEXT("read-only enumeration never invalidates handles"),
		Handles.GetInvalidationCount(), 0);
	Test.TestEqual(TEXT("one Curve child-handle family is returned"),
		Outcome.HandleResults.Num(), 1);
	if (Outcome.HandleResults.Num() != 1)
	{
		return;
	}
	const FAssetDocumentPrimitiveHandleResult& CurveHandles = Outcome.HandleResults[0];
	Test.TestEqual(TEXT("Curve child-handle family is exact"), CurveHandles.Name,
		FString(TEXT("curves")));
	Test.TestEqual(TEXT("one request-local Curve handle is returned per curve"),
		CurveHandles.Handles.Num(), ExpectedCurves.Num());
	for (int32 Index = 0; Index < CurveHandles.Handles.Num()
		&& Index < Handles.GetIssuedHandles().Num(); ++Index)
	{
		const FAssetDocumentOpaqueHandle& Handle = CurveHandles.Handles[Index];
		Test.TestEqual(TEXT("curve handle is request-local"), Handle.RequestId,
			Invocation.RequestId);
		Test.TestEqual(TEXT("curve handle transaction is request-local"),
			Handle.TransactionId, Invocation.TransactionId);
		Test.TestEqual(TEXT("curve handle revision is request-local"),
			Handle.RuntimeRevision, Invocation.RuntimeRevision);
		Test.TestEqual(TEXT("curve handle belongs to the Animation provider"),
			Handle.ProviderId, FString(TEXT("animation")));
		Test.TestEqual(TEXT("curve handle kind is Curve"), Handle.Kind,
			EAssetDocumentHandleKind::Curve);
		Test.TestEqual(TEXT("curve handle is read-only"), Handle.Access,
			EAssetDocumentHandleAccess::ReadOnly);
		Test.TestEqual(TEXT("published curve handle is the issued handle"), Handle.Slot,
			Handles.GetIssuedHandles()[Index].Slot);
	}

	Test.TestEqual(TEXT("float-curve output has only the curves field"),
		Outcome.Value.ObjectKeys, TArray<FString>{TEXT("curves")});
	const FAssetDocumentRuntimeValueDto* Curves = FindObjectField(
		Outcome.Value, TEXTVIEW("curves"));
	Test.TestNotNull(TEXT("float-curve output contains curves"), Curves);
	if (Curves == nullptr)
	{
		return;
	}
	Test.TestEqual(TEXT("curve output is an array"), Curves->Kind,
		EAssetDocumentRuntimeValueKind::Array);
	Test.TestEqual(TEXT("curve output count is exact"), Curves->Elements.Num(),
		ExpectedCurves.Num());
	for (int32 CurveIndex = 0; CurveIndex < Curves->Elements.Num()
		&& CurveIndex < ExpectedCurves.Num(); ++CurveIndex)
	{
		const FAssetDocumentRuntimeValueDto& ActualCurve = Curves->Elements[CurveIndex];
		const FExpectedCurve& ExpectedCurve = ExpectedCurves[CurveIndex];
		Test.TestEqual(TEXT("curve row has exactly name flags keys"),
			ActualCurve.ObjectKeys,
			TArray<FString>{TEXT("name"), TEXT("flags"), TEXT("keys")});
		Test.TestEqual(TEXT("curve row has three values"), ActualCurve.Elements.Num(), 3);
		if (ActualCurve.Elements.Num() != 3)
		{
			continue;
		}
		Test.TestEqual(TEXT("curve name is authoritative"),
			ActualCurve.Elements[0].StringValue, ExpectedCurve.Name.ToString());
		Test.TestEqual(TEXT("curve flags are authoritative"),
			ActualCurve.Elements[1].IntegerValue,
			static_cast<int64>(ExpectedCurve.Flags));
		Test.TestEqual(TEXT("curve key array is present"), ActualCurve.Elements[2].Kind,
			EAssetDocumentRuntimeValueKind::Array);
		Test.TestEqual(TEXT("curve key count is authoritative"),
			ActualCurve.Elements[2].Elements.Num(), ExpectedCurve.Keys.Num());
		for (int32 KeyIndex = 0; KeyIndex < ActualCurve.Elements[2].Elements.Num()
			&& KeyIndex < ExpectedCurve.Keys.Num(); ++KeyIndex)
		{
			AssertExactKey(Test, ActualCurve.Elements[2].Elements[KeyIndex],
				ExpectedCurve.Keys[KeyIndex]);
		}
	}
}

void AssertTerminalFailure(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveOutcome& Outcome,
	FStringView Diagnostic)
{
	Test.TestEqual(TEXT("curve enumeration failure is terminal"), Outcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Test.TestTrue(TEXT("curve enumeration failure has its exact diagnostic"),
		HasDiagnostic(Outcome.Diagnostics, Diagnostic));
	Test.TestTrue(TEXT("curve enumeration failure returns no public handles"),
		Outcome.HandleResults.IsEmpty());
	Test.TestTrue(TEXT("curve enumeration failure returns no changed records"),
		Outcome.ChangedSet.IsEmpty());
	Test.TestTrue(TEXT("curve enumeration failure returns no partial public value"),
		Outcome.Value.ObjectKeys.IsEmpty() && Outcome.Value.Elements.IsEmpty());
}

void AssertReadOnlyCurveSource(
	FAutomationTestBase& Test)
{
	const FString Path = FPaths::Combine(GetAssetDocumentPluginBaseDir(), TEXT("Source/AssetDocumentAnimation/Private/Provider/Curve/"
			"AssetDocumentAnimationCurveOperations.cpp"));
	FString Source;
	if (!Test.TestTrue(TEXT("Curve operation source is readable"),
		FFileHelper::LoadFileToString(Source, *Path)))
	{
		return;
	}
	for (const TCHAR* Forbidden : {
		TEXT("GetController("), TEXT("FScopedBracket"), TEXT("AddCurve("),
		TEXT("RemoveCurve("), TEXT("SetCurve"), TEXT("RawCurveData"),
		TEXT("ChangedSet")})
	{
		Test.TestFalse(FString::Printf(TEXT("Curve read-only source excludes %s"),
			Forbidden), Source.Contains(Forbidden));
	}
}
} // namespace AssetDocumentAnimationDataControllerTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAnimationDataControllerTest,
	"UEAssetDoc.AnimationProvider.DataController",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationDataControllerTest::RunTest(const FString&)
{
	using namespace AssetDocumentAnimationDataControllerTests;
	TArray<FExpectedCurve> ExpectedCurves;
	UAnimMontage* Sequence = MakeCurveFixture(*this, ExpectedCurves);
	if (Sequence == nullptr)
	{
		return false;
	}

	const FAssetDocumentPrimitiveInvocation AllInvocation = MakeCurveInvocation({});
	FCurveEnumerationHandleResolver AllHandles(AllInvocation);
	const FAssetDocumentPrimitiveOutcome AllOutcome =
		BuildAnimationFloatCurveEnumeration(*Sequence, AllInvocation, AllHandles);
	AssertCurveRows(*this, AllInvocation, AllOutcome, AllHandles, ExpectedCurves);

	const FAssetDocumentPrimitiveInvocation FilteredInvocation = MakeCurveInvocation(
		{MakeStringValue(TEXT("CurveSecond"))});
	FCurveEnumerationHandleResolver FilteredHandles(FilteredInvocation);
	const FAssetDocumentPrimitiveOutcome FilteredOutcome =
		BuildAnimationFloatCurveEnumeration(*Sequence, FilteredInvocation,
			FilteredHandles);
	const TArray<FExpectedCurve> ExpectedSecond{ExpectedCurves[1]};
	AssertCurveRows(*this, FilteredInvocation, FilteredOutcome, FilteredHandles,
		ExpectedSecond);

	const FAssetDocumentPrimitiveInvocation ReorderedFilterInvocation =
		MakeCurveInvocation({MakeStringValue(TEXT("CurveSecond")),
			MakeStringValue(TEXT("CurveFirst"))});
	FCurveEnumerationHandleResolver ReorderedFilterHandles(
		ReorderedFilterInvocation);
	const FAssetDocumentPrimitiveOutcome ReorderedFilterOutcome =
		BuildAnimationFloatCurveEnumeration(*Sequence, ReorderedFilterInvocation,
			ReorderedFilterHandles);
	AssertCurveRows(*this, ReorderedFilterInvocation, ReorderedFilterOutcome,
		ReorderedFilterHandles, ExpectedCurves);

	const FAssetDocumentPrimitiveInvocation NoMatchInvocation = MakeCurveInvocation(
		{MakeStringValue(TEXT("CurveAbsent"))});
	FCurveEnumerationHandleResolver NoMatchHandles(NoMatchInvocation);
	const FAssetDocumentPrimitiveOutcome NoMatchOutcome =
		BuildAnimationFloatCurveEnumeration(*Sequence, NoMatchInvocation,
			NoMatchHandles);
	const TArray<FExpectedCurve> NoCurves;
	AssertCurveRows(*this, NoMatchInvocation, NoMatchOutcome, NoMatchHandles,
		NoCurves);

	FAssetDocumentPrimitiveInvocation InvalidShape = MakeCurveInvocation({});
	InvalidShape.ValueArguments.ObjectKeys.Add(TEXT("unexpected"));
	InvalidShape.ValueArguments.Elements.Add(MakeStringValue(TEXT("value")));
	FCurveEnumerationHandleResolver InvalidShapeHandles(InvalidShape);
	AssertTerminalFailure(*this,
		BuildAnimationFloatCurveEnumeration(*Sequence, InvalidShape, InvalidShapeHandles),
		TEXTVIEW("AnimationProvider.InvalidValueShape"));
	FAssetDocumentPrimitiveInvocation MissingCurveNames = MakeCurveInvocation({});
	MissingCurveNames.ValueArguments = MakeObjectValue({}, {});
	FCurveEnumerationHandleResolver MissingCurveNamesHandles(MissingCurveNames);
	AssertTerminalFailure(*this,
		BuildAnimationFloatCurveEnumeration(*Sequence, MissingCurveNames,
			MissingCurveNamesHandles),
		TEXTVIEW("AnimationProvider.InvalidValueShape"));
	FAssetDocumentPrimitiveInvocation NonStringCurveName = MakeCurveInvocation(
		{MakeObjectValue({}, {})});
	FCurveEnumerationHandleResolver NonStringCurveNameHandles(NonStringCurveName);
	AssertTerminalFailure(*this,
		BuildAnimationFloatCurveEnumeration(*Sequence, NonStringCurveName,
			NonStringCurveNameHandles),
		TEXTVIEW("AnimationProvider.InvalidValueShape"));

	UAnimCurveMetaData* WrongTarget = NewObject<UAnimCurveMetaData>(GetTransientPackage(),
		TEXT("AssetDocumentAnimationCurveWrongTarget"), RF_Transient);
	TestNotNull(TEXT("wrong-target fixture exists"), WrongTarget);
	if (WrongTarget == nullptr)
	{
		return false;
	}
	const FAssetDocumentPrimitiveInvocation WrongTargetInvocation = MakeCurveInvocation({});
	FCurveEnumerationHandleResolver WrongTargetHandles(WrongTargetInvocation);
	AssertTerminalFailure(*this,
		BuildAnimationFloatCurveEnumeration(*WrongTarget, WrongTargetInvocation,
			WrongTargetHandles),
		TEXTVIEW("AnimationProvider.TargetClassMismatch"));

	const FAssetDocumentPrimitiveInvocation IssueFailureInvocation = MakeCurveInvocation({});
	FCurveEnumerationHandleResolver IssueFailureHandles(IssueFailureInvocation);
	IssueFailureHandles.RejectIssueNumber(2);
	AssertTerminalFailure(*this,
		BuildAnimationFloatCurveEnumeration(*Sequence, IssueFailureInvocation,
			IssueFailureHandles),
		TEXTVIEW("AnimationProvider.OperationRejected"));
	TestEqual(TEXT("issue failure occurs after one private handle issue"),
		IssueFailureHandles.GetIssueCount(), 2);

	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentAnimationPrimitiveProvider();
	FAssetDocumentPrimitiveInvocation Replacement;
	Replacement.PrimitiveId = TEXT("animation.curve.float.replace.v1");
	Replacement.Version = 1;
	FCurveEnumerationHandleResolver ReplacementHandles(Replacement);
	const FAssetDocumentPrimitiveOutcome ReplacementOutcome = Provider->Invoke(
		Replacement, ReplacementHandles, nullptr);
	AssertTerminalFailure(*this, ReplacementOutcome,
		TEXTVIEW("AD.PANIM.BLOCK.ROLLBACK_COMPOSITION"));
	TestEqual(TEXT("replacement blocker issues no Curve handles"),
		ReplacementHandles.GetIssueCount(), 0);

	AssertReadOnlyCurveSource(*this);
	return true;
}

#endif
