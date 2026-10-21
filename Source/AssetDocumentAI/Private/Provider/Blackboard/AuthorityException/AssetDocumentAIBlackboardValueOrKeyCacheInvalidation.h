// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Misc/TVariant.h"

struct FValueOrBBKey_Bool;
struct FValueOrBBKey_Class;
struct FValueOrBBKey_Enum;
struct FValueOrBBKey_Float;
struct FValueOrBBKey_Int32;
struct FValueOrBBKey_Name;
struct FValueOrBBKey_String;
struct FValueOrBBKey_Object;
struct FValueOrBBKey_Rotator;
struct FValueOrBBKey_Struct;
struct FValueOrBBKey_Vector;
class UScriptStruct;

namespace AssetDocumentAIBlackboardValueOrKeyCacheInvalidation
{
inline constexpr FStringView AuthorityId =
	TEXTVIEW("AD.AI.BLACKBOARD.VALUE-OR-KEY-CACHE-INVALIDATION-v1");

enum class EExactValueOrBlackboardKeyFamily : uint8
{
	Bool,
	Class,
	Enum,
	Float,
	Int32,
	Name,
	String,
	Object,
	Rotator,
	Struct,
	Vector,
};

enum class EExactValueOrKeyCacheInvalidationResult : uint8
{
	Success,
	InvalidResolvedLeaf,
	UnsupportedExactFamily,
	SchemaMismatch,
	AuthoredFingerprintMismatch,
	TestFailure,
};

/**
 * A closed, caller-owned reference captured by the loaded-consumer receipt,
 * or resolved immediately through the approved BT CurrentGraph typed inventory
 * by DerivedCompletion.cpp::CompleteExactCurrentGraphValueOrInventory.
 * It deliberately has no broad reconstruction constructor: each of the
 * eleven frozen UE 5.7 families has a separate typed overload.
 */
class ASSETDOCUMENTAI_API FResolvedExactValueOrLeaf final
{
public:
	FResolvedExactValueOrLeaf(FValueOrBBKey_Bool& InLeaf, UScriptStruct& InDeclaredStruct);
	FResolvedExactValueOrLeaf(FValueOrBBKey_Class& InLeaf, UScriptStruct& InDeclaredStruct);
	FResolvedExactValueOrLeaf(FValueOrBBKey_Enum& InLeaf, UScriptStruct& InDeclaredStruct);
	FResolvedExactValueOrLeaf(FValueOrBBKey_Float& InLeaf, UScriptStruct& InDeclaredStruct);
	FResolvedExactValueOrLeaf(FValueOrBBKey_Int32& InLeaf, UScriptStruct& InDeclaredStruct);
	FResolvedExactValueOrLeaf(FValueOrBBKey_Name& InLeaf, UScriptStruct& InDeclaredStruct);
	FResolvedExactValueOrLeaf(FValueOrBBKey_String& InLeaf, UScriptStruct& InDeclaredStruct);
	FResolvedExactValueOrLeaf(FValueOrBBKey_Object& InLeaf, UScriptStruct& InDeclaredStruct);
	FResolvedExactValueOrLeaf(FValueOrBBKey_Rotator& InLeaf, UScriptStruct& InDeclaredStruct);
	FResolvedExactValueOrLeaf(FValueOrBBKey_Struct& InLeaf, UScriptStruct& InDeclaredStruct);
	FResolvedExactValueOrLeaf(FValueOrBBKey_Vector& InLeaf, UScriptStruct& InDeclaredStruct);

	EExactValueOrBlackboardKeyFamily GetFamily() const { return Family; }
	const UScriptStruct* GetDeclaredStruct() const { return DeclaredStruct; }

private:
	using FLeafReference = TVariant<
		FValueOrBBKey_Bool*,
		FValueOrBBKey_Class*,
		FValueOrBBKey_Enum*,
		FValueOrBBKey_Float*,
		FValueOrBBKey_Int32*,
		FValueOrBBKey_Name*,
		FValueOrBBKey_String*,
		FValueOrBBKey_Object*,
		FValueOrBBKey_Rotator*,
		FValueOrBBKey_Struct*,
		FValueOrBBKey_Vector*>;

	FResolvedExactValueOrLeaf(
		FLeafReference InLeaf,
		UScriptStruct& InDeclaredStruct,
		EExactValueOrBlackboardKeyFamily InFamily);

	FLeafReference Leaf;
	UScriptStruct* DeclaredStruct = nullptr;
	EExactValueOrBlackboardKeyFamily Family;

	friend ASSETDOCUMENTAI_API EExactValueOrKeyCacheInvalidationResult InvalidateExactValueOrBlackboardKeyCache(
		FResolvedExactValueOrLeaf& ResolvedLeaf);
};

ASSETDOCUMENTAI_API EExactValueOrKeyCacheInvalidationResult InvalidateExactValueOrBlackboardKeyCache(
	FResolvedExactValueOrLeaf& ResolvedLeaf);

enum class EValueOrKeyCacheInvalidationTestFailurePoint : uint8
{
	None,
	BeforeBool, AfterBool,
	BeforeClass, AfterClass,
	BeforeEnum, AfterEnum,
	BeforeFloat, AfterFloat,
	BeforeInt32, AfterInt32,
	BeforeName, AfterName,
	BeforeString, AfterString,
	BeforeObject, AfterObject,
	BeforeRotator, AfterRotator,
	BeforeStruct, AfterStruct,
	BeforeVector, AfterVector,
	BeforeFinalAssignment,
};

#if WITH_DEV_AUTOMATION_TESTS
ASSETDOCUMENTAI_API void SetValueOrKeyCacheInvalidationTestFailurePoint(
	EValueOrKeyCacheInvalidationTestFailurePoint FailurePoint);
#endif // WITH_DEV_AUTOMATION_TESTS
} // namespace AssetDocumentAIBlackboardValueOrKeyCacheInvalidation
