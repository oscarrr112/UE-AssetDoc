// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Content of generic-route assets that asset editors author outside the Details panel. A colour curve and a vector
// curve shaped in the Curve editor show their keys per channel, an empty channel too; an edit of one channel applies,
// a channel the document leaves out keeps its keys, an explicit empty channel clears it, and the extract re-applies
// as a no-op. A curve a script gave two keys at one time reads with a warning, keeps its keys on re-apply, and an
// edit that adds a key out of order is refused. A data asset with saved state no document form covers names it in an Info
// diagnostic, and an edit of another property keeps it.

#include "AssetDocumentFidelityGenericTestTypes.h"
#include "AssetDocumentTolerantTestSupport.h"

#include "Curves/CurveLinearColor.h"
#include "Curves/CurveVector.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentFidelityGenericTestsLocal
{
double NumberAt(const TSharedPtr<FJsonObject>& Document, const FString& Pointer)
{
	const TSharedPtr<FJsonValue> Value = AssetDocumentTolerantTestSupport::At(Document, Pointer);
	double Number = 0.0;
	return Value.IsValid() && Value->TryGetNumber(Number) ? Number : -12345.0;
}

TSharedPtr<FJsonObject> CurveKeys(std::initializer_list<TPair<double, double>> Keys)
{
	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const TPair<double, double>& Key : Keys)
	{
		const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetNumberField(TEXT("Time"), Key.Key);
		Row->SetNumberField(TEXT("Value"), Key.Value);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	const TSharedRef<FJsonObject> Curve = MakeShared<FJsonObject>();
	Curve->SetArrayField(TEXT("Keys"), Rows);
	return Curve;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelityGenericColorCurveTest, "UEAssetDoc.Fidelity.Generic.ColorCurve",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentFidelityGenericColorCurveTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentFidelityGenericTestsLocal;
	const FTarget Target(TEXT("Curve_FidelityColor"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	// The Curve editor on a new colour curve (black to white): the red key at 0 raised, the green keys deleted, an
	// alpha key added at 0.5, and a hue adjustment in Details.
	UCurveLinearColor* Curve = Target.Create<UCurveLinearColor>();
	Curve->FloatCurves[0].UpdateOrAddKey(0.0f, 0.25f);
	Curve->FloatCurves[0].UpdateOrAddKey(1.0f, 1.0f);
	Curve->FloatCurves[1].Reset();
	Curve->FloatCurves[3].UpdateOrAddKey(0.5f, 0.5f);
	Curve->AdjustHue = 30.0f;
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Colour curve"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("red keys read"), NumberAt(Extracted, TEXT("/Properties/FloatCurves/R/Keys/0/Value")), 0.25);
	TestEqual(TEXT("alpha key read"), NumberAt(Extracted, TEXT("/Properties/FloatCurves/A/Keys/1/Time")), 0.5);
	const TSharedPtr<FJsonValue> GreenKeys = At(Extracted, TEXT("/Properties/FloatCurves/G/Keys"));
	TestTrue(TEXT("empty green channel listed empty"), GreenKeys.IsValid() && GreenKeys->AsArray().Num() == 0);
	TestEqual(TEXT("hue read"), NumberAt(Extracted, TEXT("/Properties/AdjustHue")), 30.0);

	// Keys on the green channel; the other channels are given as extracted.
	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		Document->GetObjectField(TEXT("Properties"))->GetObjectField(TEXT("FloatCurves"))
			->SetObjectField(TEXT("G"), CurveKeys({{0.0, 0.0}, {2.0, 0.75}}));
	}, TEXT("Edit green"));
	if (TestTrue(TEXT("edit applies"), Edited.IsSuccess()))
	{
		Curve = Target.Get<UCurveLinearColor>();
		if (TestNotNull(TEXT("curve after edit"), Curve))
		{
			TestEqual(TEXT("green keys written"), Curve->FloatCurves[1].GetNumKeys(), 2);
			TestEqual(TEXT("green value"), Curve->FloatCurves[1].Eval(2.0f), 0.75f);
			TestEqual(TEXT("red keys kept"), Curve->FloatCurves[0].GetNumKeys(), 2);
			TestEqual(TEXT("alpha keys kept"), Curve->FloatCurves[3].GetNumKeys(), 3);
			TestEqual(TEXT("hue kept"), Curve->AdjustHue, 30.0f);
		}
	}
	TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Colour curve after edit"));

	// A document that names only the green channel keeps red, blue and alpha.
	const FAssetDocumentResult OnlyGreen = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedRef<FJsonObject> Channels = MakeShared<FJsonObject>();
		Channels->SetObjectField(TEXT("G"), CurveKeys({{0.0, 0.5}}));
		Document->GetObjectField(TEXT("Properties"))->SetObjectField(TEXT("FloatCurves"), Channels);
	}, TEXT("Only green"));
	if (TestTrue(TEXT("green-only edit applies"), OnlyGreen.IsSuccess()))
	{
		Curve = Target.Get<UCurveLinearColor>();
		if (TestNotNull(TEXT("curve after green-only edit"), Curve))
		{
			TestEqual(TEXT("green replaced"), Curve->FloatCurves[1].GetNumKeys(), 1);
			TestEqual(TEXT("red kept"), Curve->FloatCurves[0].GetNumKeys(), 2);
			TestEqual(TEXT("red value kept"), Curve->FloatCurves[0].Eval(0.0f), 0.25f);
			TestEqual(TEXT("blue kept"), Curve->FloatCurves[2].GetNumKeys(), 2);
			TestEqual(TEXT("alpha kept"), Curve->FloatCurves[3].GetNumKeys(), 3);
		}
	}

	// A channel the curve does not have is refused.
	const FAssetDocumentResult Refused = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		Document->GetObjectField(TEXT("Properties"))->GetObjectField(TEXT("FloatCurves"))
			->SetObjectField(TEXT("W"), CurveKeys({{0.0, 1.0}}));
	}, TEXT("Unknown channel"));
	TestRefused(*this, Refused, TEXT("CurveFloat.UnknownField"), TEXT("/Properties/FloatCurves/W"), TEXT("Unknown channel"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelityGenericVectorCurveTest, "UEAssetDoc.Fidelity.Generic.VectorCurve",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentFidelityGenericVectorCurveTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentFidelityGenericTestsLocal;
	const FTarget Target(TEXT("Curve_FidelityVector"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	UCurveVector* Curve = Target.Create<UCurveVector>();
	Curve->FloatCurves[2].AddKey(1.0f, 3.0f);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Vector curve"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("Z key read"), NumberAt(Extracted, TEXT("/Properties/FloatCurves/Z/Keys/0/Value")), 3.0);
	const TSharedPtr<FJsonValue> XKeys = At(Extracted, TEXT("/Properties/FloatCurves/X/Keys"));
	TestTrue(TEXT("empty X listed empty"), XKeys.IsValid() && XKeys->AsArray().Num() == 0);

	// A channel left out keeps its keys.
	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedRef<FJsonObject> Channels = MakeShared<FJsonObject>();
		Channels->SetObjectField(TEXT("X"), CurveKeys({{0.0, 1.0}}));
		Document->GetObjectField(TEXT("Properties"))->SetObjectField(TEXT("FloatCurves"), Channels);
	}, TEXT("Only X"));
	if (TestTrue(TEXT("edit applies"), Edited.IsSuccess()))
	{
		Curve = Target.Get<UCurveVector>();
		TestTrue(TEXT("X written, Z kept"), Curve != nullptr && Curve->FloatCurves[0].GetNumKeys() == 1
			&& Curve->FloatCurves[2].GetNumKeys() == 1);
	}

	// An explicit empty channel clears it.
	const FAssetDocumentResult Cleared = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedRef<FJsonObject> Channels = MakeShared<FJsonObject>();
		Channels->SetObjectField(TEXT("Z"), MakeShared<FJsonObject>());
		Document->GetObjectField(TEXT("Properties"))->SetObjectField(TEXT("FloatCurves"), Channels);
	}, TEXT("Clear Z"));
	if (TestTrue(TEXT("clear applies"), Cleared.IsSuccess()))
	{
		Curve = Target.Get<UCurveVector>();
		TestTrue(TEXT("Z cleared, X kept"), Curve != nullptr && Curve->FloatCurves[2].GetNumKeys() == 0
			&& Curve->FloatCurves[0].GetNumKeys() == 1);
	}
	TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Vector curve after edits"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelityGenericKeysAtOneTimeTest, "UEAssetDoc.Fidelity.Generic.CurveKeysAtOneTime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentFidelityGenericKeysAtOneTimeTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentFidelityGenericTestsLocal;
	const FTarget Target(TEXT("Curve_FidelityKeysAtOneTime"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	// A script's AddKey on a new colour curve: a second red key at time 0, beside the default one.
	UCurveLinearColor* Curve = Target.Create<UCurveLinearColor>();
	Curve->FloatCurves[0].AddKey(0.0f, 0.25f);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}
	FExpected Expected;
	Expected.Warnings = {{TEXT("CurveFloat.KeysNotAscending"), TEXT("/Properties/FloatCurves/R/Keys/1/Time")}};
	if (!TestTolerantRoundTrip(*this, Target, Expected, TEXT("Keys at one time")).IsValid())
	{
		return false;
	}
	Curve = Target.Get<UCurveLinearColor>();
	TestTrue(TEXT("the re-applied extract keeps the three red keys"), Curve != nullptr && Curve->FloatCurves[0].GetNumKeys() == 3);

	// Another key out of order is new input and is refused.
	const FAssetDocumentResult Refused = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		TSharedPtr<FJsonObject> Blue = Document->GetObjectField(TEXT("Properties"))->GetObjectField(TEXT("FloatCurves"))
			->GetObjectField(TEXT("B"));
		TArray<TSharedPtr<FJsonValue>> Keys = Blue->GetArrayField(TEXT("Keys"));
		Keys.Add(CurveKeys({{0.5, 0.5}})->GetArrayField(TEXT("Keys"))[0]);
		Blue->SetArrayField(TEXT("Keys"), Keys);
	}, TEXT("Key out of order"));
	TestRefused(*this, Refused, TEXT("CurveFloat.KeysNotAscending"), TEXT("/Properties/FloatCurves/B/Keys/2/Time"), TEXT("Key out of order"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelityGenericOmittedSavedStateTest, "UEAssetDoc.Fidelity.Generic.OmittedSavedState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentFidelityGenericOmittedSavedStateTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	const FTarget Target(TEXT("DA_FidelityOmitted"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	UAssetDocumentFidelityGenericTestAsset* Asset = Target.Create<UAssetDocumentFidelityGenericTestAsset>();
	Asset->EditorKeys = {1.0f, 2.0f};
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}
	const FAssetDocumentResult Extracted = ExtractResult(Target);
	LogResult(*this, TEXT("Extract"), Extracted);
	if (!TestTrue(TEXT("extract succeeds"), Extracted.IsSuccess()))
	{
		return false;
	}
	const FAssetDocumentDiagnostic* Note = Extracted.Diagnostics.FindByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == TEXT("Generic.OmittedSavedState");
	});
	if (TestNotNull(TEXT("omitted saved state is named"), Note))
	{
		TestEqual(TEXT("at Properties"), Note->Path, FString(TEXT("/Properties")));
		TestEqual(TEXT("as information"), Note->Severity, FString(TEXT("info")));
		TestTrue(TEXT("names the property"), Note->Message.Contains(TEXT("EditorKeys")));
		TestFalse(TEXT("does not name an editable property"), Note->Message.Contains(TEXT("Gain")));
	}
	TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Omitted saved state"));

	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		Document->GetObjectField(TEXT("Properties"))->SetNumberField(TEXT("Gain"), 2.0);
	}, TEXT("Edit Gain"));
	if (TestTrue(TEXT("edit applies"), Edited.IsSuccess()))
	{
		Asset = Target.Get<UAssetDocumentFidelityGenericTestAsset>();
		TestTrue(TEXT("saved state kept"), Asset != nullptr && Asset->Gain == 2.0f && Asset->EditorKeys.Num() == 2);
	}
	return !HasAnyErrors();
}

#endif
