// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

// Shared pieces of the Material / Material Function end-to-end tests: document edits on parsed JSON, the
// editor-side facts the tests check (compile errors, parameters, HLSL) and a reflection comparison of two
// material graphs that does not go through the profile (the fidelity oracle).

#include "AssetDocumentDataTableTestSupport.h"

#include "AssetCompilingManager.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Materials/MaterialFunction.h"
#include "RHIShaderPlatform.h"
#include "UObject/ReferenceChainSearch.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "Utils/AssetDocumentJsonKeys.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentMaterialTests
{
using namespace AssetDocumentDataTableTests;

inline const TCHAR* MaterialClass = TEXT("/Script/Engine.Material");
inline const TCHAR* FunctionClass = TEXT("/Script/Engine.MaterialFunction");

inline void FinishCompilation()
{
	FAssetCompilingManager::Get().FinishAllCompilation();
}

// ---- JSON document helpers ----------------------------------------------------------------------------

inline TSharedPtr<FJsonObject> Field(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name)
{
	const TSharedPtr<FJsonObject>* Value = nullptr;
	return Object.IsValid() && Object->TryGetObjectField(Name, Value) ? *Value : nullptr;
}

inline TArray<FString> Keys(const TSharedPtr<FJsonObject>& Object)
{
	TArray<FString> Out;
	if (Object.IsValid())
	{
		AssetDocumentServerJson::GetKeys(*Object, Out);
		Out.Sort();
	}
	return Out;
}

// Body.Expressions.<Key> of a parsed document.
inline TSharedPtr<FJsonObject> ExpressionOf(const TSharedPtr<FJsonObject>& Document, const FString& Key)
{
	return Field(Field(Field(Document, TEXT("Body")), TEXT("Expressions")), *Key);
}

// Ensures an object field exists and returns it.
inline TSharedPtr<FJsonObject> EnsureField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name)
{
	TSharedPtr<FJsonObject> Value = Field(Object, Name);
	if (!Value.IsValid() && Object.IsValid())
	{
		Value = MakeShared<FJsonObject>();
		Object->SetObjectField(Name, Value);
	}
	return Value;
}

inline TSharedPtr<FJsonObject> WithAction(const TSharedPtr<FJsonObject>& Document, const TCHAR* Action)
{
	const TSharedPtr<FJsonObject> Copy = Parse(Serialize(Document));
	if (Copy.IsValid())
	{
		Copy->SetStringField(TEXT("Action"), Action);
	}
	return Copy;
}

// The extracted document of one asset re-targeted at another (Action Create).
inline FString Retarget(const TSharedPtr<FJsonObject>& Document, const FTarget& Target)
{
	const TSharedPtr<FJsonObject> Copy = WithAction(Document, TEXT("Create"));
	if (!Copy.IsValid())
	{
		return FString();
	}
	Copy->SetStringField(TEXT("Target"), Target.PackageName);
	return Serialize(Copy);
}

// ---- editor facts -------------------------------------------------------------------------------------

inline TArray<FString> CompileErrors(UMaterial* Material)
{
	const FMaterialResource* Resource = Material != nullptr ? Material->GetMaterialResource(GMaxRHIShaderPlatform) : nullptr;
	return Resource != nullptr ? Resource->GetCompileErrors() : TArray<FString>();
}

// Parameter names (with type) the material exposes, sorted.
inline TArray<FString> ParameterNames(UMaterialInterface* Material)
{
	TArray<FString> Names;
	if (Material == nullptr)
	{
		return Names;
	}
	for (int32 Type = 0; Type < NumMaterialParameterTypes; ++Type)
	{
		TMap<FMaterialParameterInfo, FMaterialParameterMetadata> Parameters;
		Material->GetAllParametersOfType(static_cast<EMaterialParameterType>(Type), Parameters);
		for (const TPair<FMaterialParameterInfo, FMaterialParameterMetadata>& Pair : Parameters)
		{
			Names.Add(FString::Printf(TEXT("%s:%d"), *Pair.Key.Name.ToString(), Type));
		}
	}
	Names.Sort();
	return Names;
}

// Parameters with their default value and group, sorted (the parameter set a Material Instance sees).
inline TArray<FString> ParameterSet(UMaterialInterface* Material)
{
	TArray<FString> Rows;
	if (Material == nullptr)
	{
		return Rows;
	}
	for (int32 Type = 0; Type < NumMaterialParameterTypes; ++Type)
	{
		TMap<FMaterialParameterInfo, FMaterialParameterMetadata> Parameters;
		Material->GetAllParametersOfType(static_cast<EMaterialParameterType>(Type), Parameters);
		for (const TPair<FMaterialParameterInfo, FMaterialParameterMetadata>& Pair : Parameters)
		{
			FString Value;
			const FMaterialParameterValue& Default = Pair.Value.Value;
			switch (Default.Type)
			{
			case EMaterialParameterType::Scalar: Value = FString::SanitizeFloat(Default.AsScalar()); break;
			case EMaterialParameterType::Vector: Value = Default.AsLinearColor().ToString(); break;
			case EMaterialParameterType::Texture: Value = GetPathNameSafe(Default.Texture); break;
			case EMaterialParameterType::StaticSwitch: Value = Default.AsStaticSwitch() ? TEXT("true") : TEXT("false"); break;
			default: Value = TEXT("-"); break;
			}
			Rows.Add(FString::Printf(TEXT("%s:%d=%s group=%s sort=%d"), *Pair.Key.ToString(), Type, *Value,
				*Pair.Value.Group.ToString(), Pair.Value.SortPriority));
		}
	}
	Rows.Sort();
	return Rows;
}

// The HLSL the translator generates for the editor's shader platform, with the asset names replaced and the two
// numberings that follow the order of the material's expression list (not part of the document; a document
// creates expressions in key order) replaced by '#': the texture reference index of the texture streaming
// analysis (MaterialStoreTexCoordScale / MaterialStoreTexSample) and custom vertex interpolator indices.
inline FString TranslatedSource(UMaterial* Material)
{
	FMaterialResource* Resource = Material != nullptr ? Material->GetMaterialResource(GMaxRHIShaderPlatform) : nullptr;
	FString Source;
	if (Resource == nullptr || !Resource->GetMaterialExpressionSource(Source))
	{
		return FString();
	}
	Source = Source.Replace(*Material->GetOutermost()->GetName(), TEXT("$PACKAGE")).Replace(*Material->GetName(), TEXT("$ASSET"));
	TArray<FString> Lines;
	Source.ParseIntoArrayLines(Lines, false);
	for (FString& Line : Lines)
	{
		int32 At = Line.Find(TEXT("MaterialStoreTex"));
		if (At != INDEX_NONE)
		{
			const int32 Close = Line.Find(TEXT(")"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			const int32 Comma = Line.Find(TEXT(","), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			if (Close > Comma && Comma > At)
			{
				Line = Line.Left(Comma) + TEXT(", #") + Line.Mid(Close);
			}
		}
		// Custom vertex interpolators are numbered in that order too (VertexInterpolator0(...)).
		for (At = Line.Find(TEXT("VertexInterpolator")); At != INDEX_NONE; At = Line.Find(TEXT("VertexInterpolator"), ESearchCase::CaseSensitive, ESearchDir::FromStart, At + 1))
		{
			const int32 Digits = At + 18;
			int32 End = Digits;
			while (End < Line.Len() && FChar::IsDigit(Line[End]))
			{
				++End;
			}
			if (End > Digits)
			{
				Line = Line.Left(Digits) + TEXT("#") + Line.Mid(End);
			}
		}
		// Preshader listings name a texture by its reference index: TEX["Name"][1].
		for (At = Line.Find(TEXT("TEX[\"")); At != INDEX_NONE; At = Line.Find(TEXT("TEX[\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, At + 1))
		{
			const int32 Index = Line.Find(TEXT("\"]["), ESearchCase::CaseSensitive, ESearchDir::FromStart, At + 5);
			int32 End = Index + 3;
			while (Index != INDEX_NONE && End < Line.Len() && FChar::IsDigit(Line[End]))
			{
				++End;
			}
			if (Index != INDEX_NONE && End > Index + 3)
			{
				Line = Line.Left(Index + 3) + TEXT("#") + Line.Mid(End);
			}
		}
		for (At = Line.Find(TEXT("VERTEX_INTERPOLATOR_")); At != INDEX_NONE; At = Line.Find(TEXT("VERTEX_INTERPOLATOR_"), ESearchCase::CaseSensitive, ESearchDir::FromStart, At + 1))
		{
			const int32 Digits = At + 20;
			int32 End = Digits;
			while (End < Line.Len() && FChar::IsDigit(Line[End]))
			{
				++End;
			}
			if (End > Digits)
			{
				Line = Line.Left(Digits) + TEXT("#") + Line.Mid(End);
			}
		}
	}
	// (The interpolator definitions are listed in interpolator order; compare them as a set.)
	TArray<FString> Definitions;
	for (int32 Index = Lines.Num() - 1; Index >= 0; --Index)
	{
		if (Lines[Index].StartsWith(TEXT("#define VERTEX_INTERPOLATOR_")))
		{
			Definitions.Add(Lines[Index]);
			Lines.RemoveAt(Index);
		}
	}
	Definitions.Sort();
	Lines.Append(Definitions);
	return FString::Join(Lines, TEXT("\n"));
}

// TestReload, logging what still references the asset when it does not unload.
inline void TestMaterialReload(FAutomationTestBase& Test, const FTarget& Target, const FString& Expected)
{
	UObject* Asset = Target.Get();
	if (Asset == nullptr)
	{
		return;
	}
	Test.TestFalse(TEXT("package is clean before unload"), Asset->GetOutermost()->IsDirty());
	ReleaseUndoHistory();
	// Persistence verification loads each staged package (/Temp/AssetDocument/Runtime/Staging/...) and leaves it
	// loaded; a staged material instance or caller references this asset. Unload those first.
	TArray<UPackage*> Staged;
	for (TObjectIterator<UPackage> It; It; ++It)
	{
		if (It->GetName().StartsWith(TEXT("/Temp/AssetDocument/Runtime/Staging/")))
		{
			Staged.Add(*It);
		}
	}
	if (!Staged.IsEmpty())
	{
		UPackageTools::UnloadPackages(Staged);
	}
	const TWeakObjectPtr<UObject> Weak = Asset;
	FText UnloadError;
	const bool bUnloaded = UPackageTools::UnloadPackages({Asset->GetOutermost()}, UnloadError);
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	if (Weak.IsValid())
	{
		FReferenceChainSearch Search(Weak.Get(), EReferenceChainSearchMode::Shortest | EReferenceChainSearchMode::PrintResults);
		Test.AddError(FString::Printf(TEXT("%s is still loaded after unload (unload %d '%s', flags 0x%x, rooted %d, chains: %s)"),
			*Target.AssetName, bUnloaded ? 1 : 0, *UnloadError.ToString(), static_cast<uint32>(Weak->GetFlags()), Weak->IsRooted() ? 1 : 0,
			*Search.GetRootPath().Left(1500)));
		return;
	}
	ExtractEquals(Test, Target, Expected, TEXT("after reload from disk"));
}

// ---- reflection comparison of two graphs (the fidelity oracle) -----------------------------------------

inline TArray<UMaterialExpression*> GraphExpressions(UObject* Host)
{
	TArray<UMaterialExpression*> Out;
	FMaterialExpressionCollection* Collection = nullptr;
	if (UMaterial* Material = Cast<UMaterial>(Host))
	{
		Collection = &Material->GetExpressionCollection();
	}
	else if (UMaterialFunction* Function = Cast<UMaterialFunction>(Host))
	{
		Collection = &Function->GetExpressionCollection();
	}
	if (Collection == nullptr)
	{
		return Out;
	}
	for (UMaterialExpression* Expression : Collection->Expressions)
	{
		if (Expression != nullptr)
		{
			Out.AddUnique(Expression);
		}
	}
	for (UMaterialExpressionComment* Comment : Collection->EditorComments)
	{
		if (Comment != nullptr)
		{
			Out.AddUnique(Comment);
		}
	}
	return Out;
}

// Properties that are identity or editor bookkeeping (regenerated by the editor for any new node), not graph content.
inline bool IsIdentityProperty(const FProperty& Property)
{
	static const TSet<FName> Names = {TEXT("MaterialExpressionGuid"), TEXT("ExpressionGUID"), TEXT("VariableGuid"),
		TEXT("DeclarationGuid"), TEXT("Id"), TEXT("GraphNode"), TEXT("Material"), TEXT("Function"), TEXT("SubgraphExpression"),
		TEXT("bNeedToUpdatePreview"), TEXT("bRealtimePreview"), TEXT("bIsParameterExpression")};
	return Names.Contains(Property.GetFName());
}

inline bool IsExpressionInputStruct(const UStruct* Struct)
{
	for (const UStruct* It = Struct; It != nullptr; It = It->GetSuperStruct())
	{
		if (It->GetFName() == TEXT("ExpressionInput"))
		{
			return true;
		}
	}
	return false;
}

// Text of one value, normalized where two values mean the same material:
// - a pin (FExpressionInput and the material inputs): its link (expression, output index, input name). The
//   component mask is re-derived from the output by every connect the editor makes (older assets keep an
//   empty mask), and the constant of a material input (UseConstant, Constant) is legacy data the Material
//   Editor no longer edits; a semantic difference in either shows in the translated HLSL, which the fidelity
//   test compares. The output index of an unlinked pin is inert. A pin's InputName is a display cache (pins are
//   named by their expression: GetInputName).
// - text: its string (localization keys are identity).
// - an expression reference: the expression's name (the document key).
inline FString NormalizedText(const FProperty& Property, const void* Value)
{
	if (const FStructProperty* StructProperty = CastField<FStructProperty>(&Property))
	{
		// (Material inputs are reflected through their own structs, not as FExpressionInput subtypes.)
		const bool bPin = IsExpressionInputStruct(StructProperty->Struct)
			|| (StructProperty->Struct->FindPropertyByName(TEXT("Expression")) != nullptr
				&& StructProperty->Struct->FindPropertyByName(TEXT("OutputIndex")) != nullptr);
		// A function call's pin records point at the function's input / output expressions (refreshed from the
		// function by UpdateFromFunctionResource); the pins are matched by id and name.
		const bool bFunctionPin = StructProperty->Struct->GetFName() == TEXT("FunctionExpressionInput")
			|| StructProperty->Struct->GetFName() == TEXT("FunctionExpressionOutput");
		bool bLinked = true;
		if (bPin)
		{
			const FObjectPropertyBase* Expression = CastField<FObjectPropertyBase>(StructProperty->Struct->FindPropertyByName(TEXT("Expression")));
			bLinked = Expression != nullptr && Expression->GetObjectPropertyValue_InContainer(Value) != nullptr;
		}
		FString Text = TEXT("(");
		for (TFieldIterator<FProperty> It(StructProperty->Struct); It; ++It)
		{
			const FName Name = It->GetFName();
			if ((bPin && (Name.ToString().StartsWith(TEXT("Mask")) || (!bLinked && Name == TEXT("OutputIndex"))
					|| Name.ToString().StartsWith(TEXT("Constant")) || Name == TEXT("UseConstant") || Name == TEXT("InputName")))
				|| (bFunctionPin && (Name == TEXT("ExpressionInput") || Name == TEXT("ExpressionOutput"))))
			{
				continue;
			}
			for (int32 Index = 0; Index < It->ArrayDim; ++Index)
			{
				Text += FString::Printf(TEXT("%s=%s,"), *Name.ToString(), *NormalizedText(**It, It->ContainerPtrToValuePtr<void>(Value, Index)));
			}
		}
		return Text + TEXT(")");
	}
	if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(&Property))
	{
		FScriptArrayHelper Array(ArrayProperty, Value);
		FString Text = TEXT("[");
		for (int32 Index = 0; Index < Array.Num(); ++Index)
		{
			Text += NormalizedText(*ArrayProperty->Inner, Array.GetRawPtr(Index)) + TEXT(",");
		}
		return Text + TEXT("]");
	}
	if (const FTextProperty* TextProperty = CastField<FTextProperty>(&Property))
	{
		return FString::Printf(TEXT("\"%s\""), *TextProperty->GetPropertyValue(Value).ToString());
	}
	if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(&Property))
	{
		if (const UMaterialExpression* Expression = Cast<UMaterialExpression>(ObjectProperty->GetObjectPropertyValue(Value)))
		{
			return Expression->GetName();
		}
	}
	FString Text;
	Property.ExportTextItem_Direct(Text, Value, nullptr, nullptr, PPF_None);
	return Text;
}

// Normalized text of a property with both assets' paths replaced by $SELF.
inline FString ComparableText(const FProperty& Property, const void* Container, const UObject* Host)
{
	FString Text;
	for (int32 Index = 0; Index < Property.ArrayDim; ++Index)
	{
		Text += NormalizedText(Property, Property.ContainerPtrToValuePtr<void>(Container, Index)) + TEXT("|");
	}
	return Text.Replace(*Host->GetPathName(), TEXT("$SELF")).Replace(*Host->GetOutermost()->GetName(), TEXT("$PACKAGE"));
}

// Parameter group sort priorities as UMaterial::GetGroupSortPriority reads them (first entry of a name).
inline FString GroupPriorities(const UObject* EditorOnly)
{
	const UMaterialEditorOnlyData* Data = Cast<UMaterialEditorOnlyData>(EditorOnly);
	if (Data == nullptr)
	{
		return FString();
	}
	TMap<FString, int32> Priorities;
	for (const FParameterGroupData& Group : Data->ParameterGroupData)
	{
		if (!Priorities.Contains(Group.GroupName))
		{
			Priorities.Add(Group.GroupName, Group.GroupSortPriority);
		}
	}
	Priorities.KeySort(TLess<FString>());
	FString Text;
	for (const TPair<FString, int32>& Pair : Priorities)
	{
		Text += FString::Printf(TEXT("%s=%d;"), *Pair.Key, Pair.Value);
	}
	return Text;
}

// Differences between the expressions of two graphs (matched by name: the document key), every reflected
// property but identity / bookkeeping ones; empty when equal.
inline TArray<FString> CompareGraphs(UObject* Expected, UObject* Actual)
{
	TArray<FString> Problems;
	TMap<FString, UMaterialExpression*> Left;
	TMap<FString, UMaterialExpression*> Right;
	for (UMaterialExpression* Expression : GraphExpressions(Expected))
	{
		Left.Add(Expression->GetName(), Expression);
	}
	for (UMaterialExpression* Expression : GraphExpressions(Actual))
	{
		Right.Add(Expression->GetName(), Expression);
	}
	for (const TPair<FString, UMaterialExpression*>& Pair : Left)
	{
		UMaterialExpression* const* Other = Right.Find(Pair.Key);
		if (Other == nullptr)
		{
			Problems.Add(FString::Printf(TEXT("%s (%s) missing"), *Pair.Key, *Pair.Value->GetClass()->GetName()));
			continue;
		}
		if ((*Other)->GetClass() != Pair.Value->GetClass())
		{
			Problems.Add(FString::Printf(TEXT("%s: class %s vs %s"), *Pair.Key, *Pair.Value->GetClass()->GetName(), *(*Other)->GetClass()->GetName()));
			continue;
		}
		for (TFieldIterator<FProperty> It(Pair.Value->GetClass()); It; ++It)
		{
			// (Outputs is the class's output list: older assets keep the list their engine version saved, a new
			// node gets the current one. Links are compared by output index and the HLSL is compared.)
			if (It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated) || IsIdentityProperty(**It) || It->GetFName() == TEXT("Outputs"))
			{
				continue;
			}
			const FString A = ComparableText(**It, Pair.Value, Expected);
			const FString B = ComparableText(**It, *Other, Actual);
			if (A != B)
			{
				Problems.Add(FString::Printf(TEXT("%s (%s).%s: %s vs %s"), *Pair.Key, *Pair.Value->GetClass()->GetName(), *It->GetName(),
					*A.Left(300), *B.Left(300)));
			}
		}
		const UMaterialExpressionNamedRerouteUsage* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(*Other);
		if (Usage != nullptr && Usage->Declaration != nullptr && Usage->DeclarationGuid != Usage->Declaration->VariableGuid)
		{
			Problems.Add(FString::Printf(TEXT("%s: named reroute usage guid does not match its declaration"), *Pair.Key));
		}
	}
	for (const TPair<FString, UMaterialExpression*>& Pair : Right)
	{
		if (!Left.Contains(Pair.Key))
		{
			Problems.Add(FString::Printf(TEXT("%s (%s) extra"), *Pair.Key, *Pair.Value->GetClass()->GetName()));
		}
	}
	// Host settings (Details panel) and the material inputs / parameter groups of the editor-only data.
	auto CompareObject = [&Problems, Expected, Actual](const UObject* A, const UObject* B, const TCHAR* Label, bool bEditOnly)
	{
		if (A == nullptr || B == nullptr || A->GetClass() != B->GetClass())
		{
			Problems.Add(FString::Printf(TEXT("%s: missing or of another class"), Label));
			return;
		}
		for (TFieldIterator<FProperty> It(A->GetClass()); It; ++It)
		{
			// (The editor-only data is compared on its own; the thumbnail is not material content.)
			if (It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated) || IsIdentityProperty(**It)
				|| (bEditOnly && !It->HasAnyPropertyFlags(CPF_Edit))
				|| It->GetFName() == TEXT("EditorOnlyData") || It->GetFName() == TEXT("ThumbnailInfo") || It->GetFName() == TEXT("AssetImportData")
				// (The expression list is compared expression by expression above; its order is not content.)
				|| It->GetFName() == TEXT("ExpressionCollection"))
			{
				continue;
			}
			const bool bGroups = It->GetFName() == TEXT("ParameterGroupData");
			const FString Left = bGroups ? GroupPriorities(A) : ComparableText(**It, A, Expected);
			const FString Right = bGroups ? GroupPriorities(B) : ComparableText(**It, B, Actual);
			if (Left != Right)
			{
				Problems.Add(FString::Printf(TEXT("%s.%s: %s vs %s"), Label, *It->GetName(), *Left.Left(300), *Right.Left(300)));
			}
		}
	};
	CompareObject(Expected, Actual, TEXT("<asset>"), true);
	if (UMaterial* ExpectedMaterial = Cast<UMaterial>(Expected))
	{
		UMaterial* ActualMaterial = Cast<UMaterial>(Actual);
		CompareObject(ExpectedMaterial->GetEditorOnlyData(), ActualMaterial != nullptr ? ActualMaterial->GetEditorOnlyData() : nullptr,
			TEXT("<material inputs>"), false);
	}
	return Problems;
}
} // namespace AssetDocumentMaterialTests
#endif
