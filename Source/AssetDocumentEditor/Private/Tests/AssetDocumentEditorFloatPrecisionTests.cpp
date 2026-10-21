// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Float precision normalisation of authored Properties, as the Details panel does: a
// number for a float property becomes the shortest decimal of the float it is
// stored as; doubles, other kinds and unknown members are left as written.

#include "AssetDocumentEditorFloatPrecision.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorFloatPrecisionTests
{
using FValue = FAssetDocumentRuntimeValueDto;
using EKind = EAssetDocumentRuntimeValueKind;

FValue Number(double Value)
{
	FValue Out;
	Out.Kind = EKind::Number;
	Out.NumberValue = Value;
	return Out;
}

FValue Integer(int64 Value)
{
	FValue Out;
	Out.Kind = EKind::Integer;
	Out.IntegerValue = Value;
	return Out;
}

FValue String(const TCHAR* Value)
{
	FValue Out;
	Out.Kind = EKind::String;
	Out.StringValue = Value;
	return Out;
}

FValue Object(std::initializer_list<TPair<const TCHAR*, FValue>> Fields)
{
	FValue Out;
	Out.Kind = EKind::Object;
	for (const TPair<const TCHAR*, FValue>& Field : Fields)
	{
		Out.ObjectKeys.Add(Field.Key);
		Out.Elements.Add(Field.Value);
	}
	return Out;
}

FValue Array(std::initializer_list<FValue> Elements)
{
	FValue Out;
	Out.Kind = EKind::Array;
	Out.Elements = Elements;
	return Out;
}

const FValue& At(const FValue& Object, const TCHAR* Key)
{
	const int32 Index = Object.ObjectKeys.IndexOfByKey(FString(Key));
	check(Index != INDEX_NONE);
	return Object.Elements[Index];
}

FValue Document(const TCHAR* Class, FValue Properties)
{
	return Object({{TEXT("Class"), String(Class)}, {TEXT("Properties"), MoveTemp(Properties)}});
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentEditorFloatPrecisionNormalizeTest,
	"UEAssetDoc.GenericRuntime.FloatPrecision.Normalize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEditorFloatPrecisionNormalizeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorFloatPrecisionTests;
	using namespace UE::AssetDocument::EditorFloatPrecision;

	// Shortest decimals.
	TestEqual(TEXT("0.3f -> 0.3"), ShortestFloatDecimal(0.3f), 0.3);
	TestEqual(TEXT("0.51f -> 0.51"), ShortestFloatDecimal(0.51f), 0.51);
	TestEqual(TEXT("whole float stays whole"), ShortestFloatDecimal(16777216.0f), 16777216.0);
	TestEqual(TEXT("float max round-trips"), static_cast<float>(ShortestFloatDecimal(MAX_FLT)), MAX_FLT);
	const float Denormal = FLT_TRUE_MIN;
	TestEqual(TEXT("denormal round-trips"), static_cast<float>(ShortestFloatDecimal(Denormal)), Denormal);

	{
		// Top-level float properties; an integer the float cannot hold stays an integer.
		FValue Doc = Document(TEXT("/Script/Engine.ReverbEffect"), Object({
			{TEXT("Density"), Number(0.30000001192092896)},
			{TEXT("Diffusion"), Number(0.3)},
			{TEXT("Gain"), Number(0.123456789123)},
			{TEXT("DecayTime"), Integer(16777217)},
			{TEXT("ReflectionsGain"), Integer(2)},
			{TEXT("NotAProperty"), Number(0.30000001192092896)},
			{TEXT("bBypassEarlyReflections"), Number(0.30000001192092896)},
		}));
		const int32 Changed = NormalizeDocumentProperties(Doc);
		const FValue& Properties = At(Doc, TEXT("Properties"));
		TestEqual(TEXT("widened float -> shortest decimal"), At(Properties, TEXT("Density")).NumberValue, 0.3);
		TestEqual(TEXT("shortest decimal unchanged"), At(Properties, TEXT("Diffusion")).NumberValue, 0.3);
		const double Gain = At(Properties, TEXT("Gain")).NumberValue;
		TestEqual(TEXT("finer than float -> the stored float"), static_cast<float>(Gain), static_cast<float>(0.123456789123));
		TestEqual(TEXT("... written shortest"), Gain, ShortestFloatDecimal(static_cast<float>(0.123456789123)));
		TestTrue(TEXT("integer literal stays an integer"), At(Properties, TEXT("DecayTime")).Kind == EKind::Integer);
		TestEqual(TEXT("16777217 -> 16777216"), At(Properties, TEXT("DecayTime")).IntegerValue, int64(16777216));
		TestEqual(TEXT("an exact integer is unchanged"), At(Properties, TEXT("ReflectionsGain")).IntegerValue, int64(2));
		TestEqual(TEXT("unknown members are left as written"), At(Properties, TEXT("NotAProperty")).NumberValue, 0.30000001192092896);
		TestEqual(TEXT("non-float members are left as written"),
			At(Properties, TEXT("bBypassEarlyReflections")).NumberValue, 0.30000001192092896);
		TestEqual(TEXT("three numbers changed"), Changed, 3);
	}
	{
		// Nested struct: float members are normalised, double (FVector) members are exact.
		FValue Doc = Document(TEXT("/Script/Engine.SoundAttenuation"), Object({
			{TEXT("Attenuation"), Object({
				{TEXT("AttenuationShapeExtents"), Object({
					{TEXT("X"), Number(0.30000000000000004)}, {TEXT("Y"), Number(0.1)}, {TEXT("Z"), Number(1e-300)}})},
				{TEXT("FalloffDistance"), Number(3600.0001)},
			})},
		}));
		NormalizeDocumentProperties(Doc);
		const FValue& Attenuation = At(At(Doc, TEXT("Properties")), TEXT("Attenuation"));
		const FValue& Extents = At(Attenuation, TEXT("AttenuationShapeExtents"));
		TestEqual(TEXT("double X exact"), At(Extents, TEXT("X")).NumberValue, 0.30000000000000004);
		TestEqual(TEXT("double Z exact"), At(Extents, TEXT("Z")).NumberValue, 1e-300);
		TestEqual(TEXT("nested float -> stored float"), At(Attenuation, TEXT("FalloffDistance")).NumberValue, 3600.0);
	}
	{
		// Array of structs.
		FValue Doc = Document(TEXT("/Script/Engine.CurveFloat"), Object({
			{TEXT("FloatCurve"), Object({
				{TEXT("Keys"), Array({
					Object({{TEXT("Time"), Number(0.1000000001)}, {TEXT("Value"), Number(0.69999998807907104)}}),
					Object({{TEXT("Time"), Integer(1)}, {TEXT("Value"), Number(2.5)}})})},
			})},
		}));
		NormalizeDocumentProperties(Doc);
		const FValue& Keys = At(At(At(Doc, TEXT("Properties")), TEXT("FloatCurve")), TEXT("Keys"));
		TestEqual(TEXT("array element member -> shortest"), At(Keys.Elements[0], TEXT("Time")).NumberValue, 0.1);
		TestEqual(TEXT("array element member widened -> shortest"), At(Keys.Elements[0], TEXT("Value")).NumberValue, 0.7);
		TestEqual(TEXT("exact values unchanged"), At(Keys.Elements[1], TEXT("Value")).NumberValue, 2.5);
	}
	{
		// Unknown class or no Properties: unchanged.
		FValue Unknown = Document(TEXT("/Script/Engine.NoSuchClass_AssetDocumentTest"),
			Object({{TEXT("Density"), Number(0.30000001192092896)}}));
		TestEqual(TEXT("unknown class is left as written"), NormalizeDocumentProperties(Unknown), 0);
		FValue NoProperties = Object({{TEXT("Class"), String(TEXT("/Script/Engine.ReverbEffect"))}});
		TestEqual(TEXT("no Properties"), NormalizeDocumentProperties(NoProperties), 0);
	}
	return true;
}
#endif
