// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// data.material.read.v1 / data.material.edit.v1 / data.material.compile.v1: the expression graph of a
// UMaterial or UMaterialFunction (incl. Material Layer / Layer Blend functions), edited as the Material
// Editor edits it.
//
// Editor reference (MaterialEditor / UnrealEd):
//   new node      -> FMaterialEditor::CreateNewMaterialExpression = UMaterialEditingLibrary::CreateMaterialExpressionEx
//                    (comments: FMaterialEditor::CreateNewMaterialExpressionComment: "Comment", 400x100, the graph
//                    editor's default comment colour, EditorComments);
//   delete node   -> UMaterialEditingLibrary::DeleteMaterialExpression(InFunction) (links to it are broken);
//   wire a pin    -> UMaterialGraphSchema::CanCreateConnection (r.PreventInvalidMaterialConnections type check:
//                    CanConnectMaterialValueTypes) + UMaterialGraph::LinkMaterialExpressionsFromGraph
//                    (FExpressionInput::Connect / Expression = nullptr);
//   Details edit  -> Modify, PreEditChange(Property), write, PostEditChangeProperty(ValueSet), then
//                    FMaterialEditor::NotifyPostChange: UpdateExpressionParameterName (ParameterName),
//                    UpdateExpressionDynamicParameters, PropagateExpressionParameterChanges;
//   move node     -> MaterialExpressionEditorX/Y (LinkMaterialExpressionsFromGraph);
//   Apply         -> UMaterialEditingLibrary::RecompileMaterial / UpdateMaterialFunction (data.material.compile.v1).
// The Material Editor edits a preview copy (bIsPreviewMaterial), so expression edits do not recompile the material
// one by one; the edit primitive marks the edited material the same way for the duration of the call and the
// compile primitive recompiles once (the editor's Apply).
//
// Identity: an expression's key is its object name inside the material / function (stable across save, reload and
// duplication). New expressions are created with the editor's factory path and then named after their key.
// Deleted expressions are renamed out of the way ("Deleted_<key>") before the editor delete, so a key can be
// reused for an expression of another class in the same apply.
//
// Properties of an expression are the ones the Details panel edits (CPF_Edit, not EditConst) plus references to
// other expressions (named reroute declaration, composite membership) and the node state the graph stores
// (bCollapsed, bCommentBubbleVisible, comment size, previewed function output). Pin connections (FExpressionInput members) are never
// properties; they are "inputs". A property value is the Editor typed value codec's (float32 shortest), an
// expression reference is the referenced expression's key, and a value the codec cannot represent uses UE's
// export-text form. Read reports the properties that differ from a new node of the same class (created the editor
// way in a transient material / function); an omitted property on edit means that new-node value.
//
// Pin names: inputs use UMaterialExpression::GetInputName (function calls: the function input name without its
// type suffix); outputs use FExpressionOutput::OutputName. A source is "<key>" (output 0 when it has no name),
// "<key>.<output name>" or "<key>.#<index>" (unnamed / duplicate output names). Inputs without a unique name are
// "#<index>". Material inputs (the main material node's pins) are named by FMaterialAttributeDefinitionMap::
// GetAttributeName (BaseColor, Metallic, ..., MaterialAttributes, FrontMaterial).
//
// DTOs (all carry "schema_version": 1):
//   read    {classes: [class]} -> {classes: {class: {properties (every authored property of a new node), pins}}}
//   read    {offset?, limit?, pins?} -> {class, host: material|function, total, next_offset,
//            expressions[{key, class, position[x,y], properties{}, inputs{name: source}, outputs[names],
//                         pins?{inputs[{name, type}], outputs[{name, type}]}}];
//            at offset 0 also outputs{name: source} and parameter_groups{name: priority} (materials),
//            compile_errors[] (the current shader platform's resource, no recompile), with pins also
//            material_inputs[{name, type}]}
//   edit    {ops[]}: {op: create, key, class, position?} | {op: delete, key} |
//            {op: set, key, properties{}, position?} | {op: connect, key, input, from (source | null)} |
//            {op: output, name, from (source | null)} | {op: parameter_groups, groups{name: priority}}
//            -> {created[], deleted[], changed}
//   compile {} -> {status: ok|errors, errors[], error_expressions[], compiling}

#include "AssetDocumentDataMaterial.h"
#include "AssetDocumentDataPrimitives.h"

#include "AssetDocumentEditorNonInteractiveScope.h"
#include "AssetDocumentEditorPropertyValueCodec.h"
#include "AssetDocumentEditorTrialScope.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "GraphEditorSettings.h"
#include "Editor.h"
#include "HAL/IConsoleManager.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionDynamicParameter.h"
#include "Materials/MaterialExpressionGetMaterialAttributes.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Materials/MaterialExpressionSetMaterialAttributes.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialParameters.h"
#include "RHIShaderPlatform.h"
#include "Runtime/AssetDocumentPackageSnapshot.h"
#include "ScopedTransaction.h"
#include "UObject/TextProperty.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "AssetDocumentMaterial"

namespace UE::AssetDocument::Graph::Data
{
namespace
{
constexpr int32 DefaultExpressionLimit = 100;
constexpr int32 MaxListedNames = 60;

// ---------------------------------------------------------------------------------------------
// Host (the material or function that owns the expressions)
// ---------------------------------------------------------------------------------------------
struct FHost
{
	UMaterial* Material = nullptr;
	UMaterialFunction* Function = nullptr;

	UObject* GetOuter() const { return Material != nullptr ? static_cast<UObject*>(Material) : Function; }
	FMaterialExpressionCollection& Collection() const
	{
		return Material != nullptr ? Material->GetExpressionCollection() : Function->GetExpressionCollection();
	}
};

bool GetHost(FCall& Call, FHost& Out)
{
	Out.Material = Cast<UMaterial>(&Call.Asset);
	Out.Function = Out.Material == nullptr ? Cast<UMaterialFunction>(&Call.Asset) : nullptr;
	if (Out.Material == nullptr && Out.Function == nullptr)
	{
		return Fail(Call, TEXT("Data.UnsupportedAsset"), TEXT("Material primitives edit Material and Material Function assets."),
			TEXT("/HandleArguments/asset"));
	}
	return true;
}

FString KeyOf(const UMaterialExpression& Expression)
{
	return Expression.GetName();
}

// Expressions and comments, sorted by key.
TArray<UMaterialExpression*> GetExpressions(const FHost& Host)
{
	TArray<UMaterialExpression*> Out;
	FMaterialExpressionCollection& Collection = Host.Collection();
	for (UMaterialExpression* Expression : Collection.Expressions)
	{
		if (Expression != nullptr)
		{
			Out.AddUnique(Expression);
		}
	}
	for (UMaterialExpressionComment* Comment : Collection.EditorComments)
	{
		if (Comment != nullptr)
		{
			Out.AddUnique(Comment);
		}
	}
	// (Graph order, not sorted: the translator numbers textures and custom vertex interpolators in it.)
	return Out;
}

TMap<FString, UMaterialExpression*> IndexByKey(const TArray<UMaterialExpression*>& Expressions)
{
	TMap<FString, UMaterialExpression*> ByKey;
	for (UMaterialExpression* Expression : Expressions)
	{
		ByKey.Add(KeyOf(*Expression), Expression);
	}
	return ByKey;
}

// ---------------------------------------------------------------------------------------------
// Classes
// ---------------------------------------------------------------------------------------------
const TCHAR* ExpressionClassPrefix = TEXT("MaterialExpression");

// A short class name ("Multiply" or "MaterialExpressionMultiply"): the engine's class first, then any loaded one.
UClass* FindExpressionClassByShortName(const FString& Text)
{
	const FString Name = Text.StartsWith(ExpressionClassPrefix, ESearchCase::CaseSensitive) ? Text : ExpressionClassPrefix + Text;
	if (UClass* Engine = FindObject<UClass>(nullptr, *(TEXT("/Script/Engine.") + Name)))
	{
		return Engine;
	}
	return FindFirstObject<UClass>(*Name, EFindFirstObjectOptions::None, ELogVerbosity::NoLogging);
}

// Native expression classes by short name ("Multiply", "LandscapeLayerBlend") when that name finds the class
// again; others by class path.
FString ClassLabel(const UClass& Class)
{
	const FString Name = Class.GetName();
	const int32 PrefixLength = FCString::Strlen(ExpressionClassPrefix);
	if (Class.IsNative() && Name.StartsWith(ExpressionClassPrefix, ESearchCase::CaseSensitive) && Name.Len() > PrefixLength)
	{
		const FString Short = Name.Mid(PrefixLength);
		if (FindExpressionClassByShortName(Short) == &Class)
		{
			return Short;
		}
	}
	return Class.GetPathName();
}

TArray<FString> ExpressionClassLabels()
{
	TArray<UClass*> Classes;
	GetDerivedClasses(UMaterialExpression::StaticClass(), Classes, true);
	TArray<FString> Labels;
	for (const UClass* Class : Classes)
	{
		if (!Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists | CLASS_Hidden))
		{
			Labels.Add(ClassLabel(*Class));
		}
	}
	Labels.Sort();
	return Labels;
}

UClass* ResolveExpressionClass(const FString& Text, FString& OutProblem)
{
	UClass* Class = nullptr;
	if (Text.StartsWith(TEXT("/")))
	{
		Class = FindObject<UClass>(nullptr, *Text);
		if (Class == nullptr && !Text.StartsWith(TEXT("/Script/")))
		{
			Class = LoadObject<UClass>(nullptr, *Text, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
	}
	else if (!Text.IsEmpty())
	{
		Class = FindExpressionClassByShortName(Text);
	}
	if (Class == nullptr || !Class->IsChildOf(UMaterialExpression::StaticClass()))
	{
		const TArray<FString> Suggestions = SuggestActionKeys(Text, ExpressionClassLabels());
		OutProblem = FString::Printf(TEXT("'%s' is not a material expression class (engine classes by short name, e.g. "
			"\"Multiply\", \"TextureSampleParameter2D\"; others by class path).%s"), *Text,
			Suggestions.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" Did you mean: %s?"), *FString::Join(Suggestions, TEXT(", "))));
		return nullptr;
	}
	if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
	{
		OutProblem = FString::Printf(TEXT("%s is abstract or deprecated; the Material Editor cannot place it."), *ClassLabel(*Class));
		return nullptr;
	}
	return Class;
}

// ---------------------------------------------------------------------------------------------
// Pins
// ---------------------------------------------------------------------------------------------
bool IsExpressionInputStruct(const UStruct* Struct);

// The reflected member that holds an input (pins the editor shows without a name, e.g. a function output's).
FString InputMemberName(UMaterialExpression& Expression, const FExpressionInput* Input)
{
	for (TFieldIterator<FStructProperty> It(Expression.GetClass()); It; ++It)
	{
		if (!IsExpressionInputStruct(It->Struct))
		{
			continue;
		}
		for (int32 Element = 0; Element < It->ArrayDim; ++Element)
		{
			if (It->ContainerPtrToValuePtr<void>(&Expression, Element) == Input)
			{
				return It->ArrayDim > 1 ? FString::Printf(TEXT("%s_%d"), *It->GetName(), Element) : It->GetName();
			}
		}
	}
	return FString();
}

int32 ShownInputCount(UMaterialExpression& Expression)
{
	int32 Count = 0;
	while (Expression.GetInput(Count) != nullptr)
	{
		++Count;
	}
	return Count;
}

// Reflected pins the expression does not show (GetInput): a pin hidden by a setting (a texture sample's MipValue
// in another mip mode) or a legacy node's pins (LegacyBlendMaterialAttributes). Their stored links are kept and
// read by member name after the shown pins.
TArray<FExpressionInput*> HiddenInputs(UMaterialExpression& Expression)
{
	TArray<FExpressionInput*> Shown;
	for (int32 Index = 0; FExpressionInput* Input = Expression.GetInput(Index); ++Index)
	{
		Shown.Add(Input);
	}
	TArray<FExpressionInput*> Hidden;
	for (TFieldIterator<FStructProperty> It(Expression.GetClass()); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated) || !IsExpressionInputStruct(It->Struct))
		{
			continue;
		}
		for (int32 Element = 0; Element < It->ArrayDim; ++Element)
		{
			FExpressionInput* Input = static_cast<FExpressionInput*>(It->ContainerPtrToValuePtr<void>(&Expression, Element));
			if (!Shown.Contains(Input))
			{
				Hidden.Add(Input);
			}
		}
	}
	return Hidden;
}

// Pin Index of InputLabels: a shown pin, then the hidden ones.
FExpressionInput* InputAt(UMaterialExpression& Expression, int32 Index)
{
	const int32 Shown = ShownInputCount(Expression);
	if (Index < Shown)
	{
		return Expression.GetInput(Index);
	}
	const TArray<FExpressionInput*> Hidden = HiddenInputs(Expression);
	return Hidden.IsValidIndex(Index - Shown) ? Hidden[Index - Shown] : nullptr;
}

TArray<FString> InputLabels(UMaterialExpression& Expression)
{
	TArray<FString> Labels;
	const UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(&Expression);
	for (int32 Index = 0; Expression.GetInput(Index) != nullptr; ++Index)
	{
		const FName Name = Call != nullptr && Call->FunctionInputs.IsValidIndex(Index)
			? Call->FunctionInputs[Index].Input.InputName : Expression.GetInputName(Index);
		FString Label = Name.IsNone() ? FString() : Name.ToString().TrimStartAndEnd();
		if (Label.IsEmpty())
		{
			Label = InputMemberName(Expression, Expression.GetInput(Index));
		}
		if (Label.IsEmpty() || Label.StartsWith(TEXT("#")) || Labels.Contains(Label))
		{
			Label = FString::Printf(TEXT("#%d"), Index);
		}
		Labels.Add(Label);
	}
	for (FExpressionInput* Input : HiddenInputs(Expression))
	{
		FString Label = InputMemberName(Expression, Input);
		if (Label.IsEmpty() || Labels.Contains(Label))
		{
			Label = FString::Printf(TEXT("#%d"), Labels.Num());
		}
		Labels.Add(Label);
	}
	return Labels;
}

TArray<FString> OutputLabels(UMaterialExpression& Expression)
{
	TArray<FString> Labels;
	const TArray<FExpressionOutput>& Outputs = Expression.GetOutputs();
	for (int32 Index = 0; Index < Outputs.Num(); ++Index)
	{
		FString Label = Outputs[Index].OutputName.IsNone() ? FString() : Outputs[Index].OutputName.ToString();
		if ((Label.IsEmpty() && Index > 0) || Label.StartsWith(TEXT("#")) || Label.Contains(TEXT(".")) || Labels.Contains(Label))
		{
			Label = FString::Printf(TEXT("#%d"), Index);
		}
		Labels.Add(Label);
	}
	return Labels;
}

FString SourceOf(UMaterialExpression& Source, int32 OutputIndex)
{
	const TArray<FString> Labels = OutputLabels(Source);
	// (A few classes list no outputs and are read from output 0: "<key>".)
	const FString Label = Labels.IsValidIndex(OutputIndex) ? Labels[OutputIndex]
		: (Labels.IsEmpty() && OutputIndex == 0 ? FString() : FString::Printf(TEXT("#%d"), OutputIndex));
	return Label.IsEmpty() ? KeyOf(Source) : FString::Printf(TEXT("%s.%s"), *KeyOf(Source), *Label);
}

// The name of output 0 on a new node of the expression's class (empty when unnamed or without outputs).
FString FreshOutputZero(UMaterialExpression& Expression)
{
	const UMaterialExpression* Default = Expression.GetClass()->GetDefaultObject<UMaterialExpression>();
	const TArray<FExpressionOutput>& Outputs = const_cast<UMaterialExpression*>(Default)->GetOutputs();
	return Outputs.IsEmpty() || Outputs[0].OutputName.IsNone() ? FString() : Outputs[0].OutputName.ToString();
}

int32 FindLabel(const TArray<FString>& Labels, const FString& Label)
{
	const int32 Found = Labels.IndexOfByKey(Label);
	if (Found != INDEX_NONE)
	{
		return Found;
	}
	if (Label.StartsWith(TEXT("#")) && Label.Len() > 1)
	{
		const FString Digits = Label.Mid(1);
		if (Digits.IsNumeric() && !Digits.Contains(TEXT(".")) && !Digits.Contains(TEXT("-")))
		{
			const int32 Index = FCString::Atoi(*Digits);
			return Labels.IsValidIndex(Index) ? Index : INDEX_NONE;
		}
	}
	return INDEX_NONE;
}

const TPair<uint64, const TCHAR*> ValueTypeNames[] = {
	{MCT_Float1, TEXT("float")}, {MCT_Float2, TEXT("float2")}, {MCT_Float3, TEXT("float3")}, {MCT_Float4, TEXT("float4")},
	{MCT_Texture2D, TEXT("Texture2D")}, {MCT_TextureCube, TEXT("TextureCube")}, {MCT_Texture2DArray, TEXT("Texture2DArray")},
	{MCT_TextureCubeArray, TEXT("TextureCubeArray")}, {MCT_VolumeTexture, TEXT("VolumeTexture")}, {MCT_StaticBool, TEXT("StaticBool")},
	{MCT_Unknown, TEXT("Unknown")}, {MCT_MaterialAttributes, TEXT("MaterialAttributes")}, {MCT_TextureExternal, TEXT("TextureExternal")},
	{MCT_TextureVirtual, TEXT("TextureVirtual")}, {MCT_SparseVolumeTexture, TEXT("SparseVolumeTexture")},
	{MCT_VTPageTableResult, TEXT("VTPageTableResult")}, {MCT_ShadingModel, TEXT("ShadingModel")}, {MCT_Substrate, TEXT("Substrate")},
	{MCT_LWCScalar, TEXT("LWCScalar")}, {MCT_LWCVector2, TEXT("LWCVector2")}, {MCT_LWCVector3, TEXT("LWCVector3")},
	{MCT_LWCVector4, TEXT("LWCVector4")}, {MCT_Execution, TEXT("Execution")}, {MCT_VoidStatement, TEXT("VoidStatement")},
	{MCT_Bool, TEXT("bool")}, {MCT_UInt1, TEXT("uint")}, {MCT_UInt2, TEXT("uint2")}, {MCT_UInt3, TEXT("uint3")}, {MCT_UInt4, TEXT("uint4")},
	{MCT_TextureCollection, TEXT("TextureCollection")}, {MCT_TextureMeshPaint, TEXT("TextureMeshPaint")},
	{MCT_TextureMaterialCache, TEXT("TextureMaterialCache")}, {MCT_Float3x3, TEXT("float3x3")}, {MCT_Float4x4, TEXT("float4x4")},
	{MCT_LWCMatrix, TEXT("LWCMatrix")}, {MCT_MaterialCacheABuffer, TEXT("MaterialCacheABuffer")}, {MCT_Unexposed, TEXT("Unexposed")}};

FString ValueTypeName(EMaterialValueType Type)
{
	const uint64 Bits = static_cast<uint64>(Type);
	if (Bits == static_cast<uint64>(MCT_Float))
	{
		return TEXT("float*");
	}
	if (Bits == static_cast<uint64>(MCT_Texture))
	{
		return TEXT("Texture");
	}
	TArray<FString> Names;
	for (const TPair<uint64, const TCHAR*>& Pair : ValueTypeNames)
	{
		if ((Bits & Pair.Key) != 0)
		{
			Names.Add(Pair.Value);
		}
	}
	return Names.IsEmpty() ? FString(TEXT("none")) : FString::Join(Names, TEXT("|"));
}

FValue DescribePins(UMaterialExpression& Expression)
{
	TArray<FValue> Inputs;
	const TArray<FString> InputNames = InputLabels(Expression);
	const int32 Shown = ShownInputCount(Expression);
	for (int32 Index = 0; Index < InputNames.Num() && Index < Shown; ++Index)
	{
		FObjectBuilder Pin;
		Pin.AddString(TEXT("name"), InputNames[Index]);
		Pin.AddString(TEXT("type"), ValueTypeName(Expression.GetInputValueType(Index)));
		if (Expression.IsInputConnectionRequired(Index))
		{
			Pin.AddBool(TEXT("required"), true);
		}
		Inputs.Add(Pin.Build());
	}
	TArray<FValue> Outputs;
	const TArray<FString> OutputNames = OutputLabels(Expression);
	for (int32 Index = 0; Index < OutputNames.Num(); ++Index)
	{
		FObjectBuilder Pin;
		Pin.AddString(TEXT("name"), OutputNames[Index]);
		Pin.AddString(TEXT("type"), ValueTypeName(Expression.GetOutputValueType(Index)));
		Outputs.Add(Pin.Build());
	}
	FObjectBuilder Pins;
	Pins.Add(TEXT("inputs"), MakeArray(MoveTemp(Inputs)));
	Pins.Add(TEXT("outputs"), MakeArray(MoveTemp(Outputs)));
	return Pins.Build();
}

// Material inputs (the main material node's pins) by attribute name.
struct FMaterialInputPin
{
	EMaterialProperty Property = MP_MAX;
	FString Name;
};

TArray<FMaterialInputPin> MaterialInputPins(UMaterial& Material)
{
	TArray<FMaterialInputPin> Pins;
	for (int32 Index = 0; Index < MP_MAX; ++Index)
	{
		const EMaterialProperty Property = static_cast<EMaterialProperty>(Index);
		if (Material.GetExpressionInputForProperty(Property) == nullptr)
		{
			continue;
		}
		const FString& Name = FMaterialAttributeDefinitionMap::GetAttributeName(Property);
		if (!Name.IsEmpty() && !Pins.ContainsByPredicate([&Name](const FMaterialInputPin& Pin) { return Pin.Name == Name; }))
		{
			Pins.Add({Property, Name});
		}
	}
	return Pins;
}

EMaterialValueType MaterialInputValueType(EMaterialProperty Property)
{
	// UMaterialGraphNode_Root::GetPinMaterialValueType
	if (Property == MP_MaterialAttributes)
	{
		return MCT_MaterialAttributes;
	}
	if (Property == MP_FrontMaterial)
	{
		return MCT_Substrate;
	}
	return FMaterialAttributeDefinitionMap::GetValueType(Property);
}

// ---------------------------------------------------------------------------------------------
// Property values
// ---------------------------------------------------------------------------------------------
const FName ExpressionInputStructName(TEXT("ExpressionInput"));

bool IsExpressionInputStruct(const UStruct* Struct)
{
	for (const UStruct* Current = Struct; Current != nullptr; Current = Current->GetSuperStruct())
	{
		if (Current->GetFName() == ExpressionInputStructName)
		{
			return true;
		}
	}
	return false;
}

// A pin connection (FExpressionInput, or an array of them): never a property.
bool IsConnectionProperty(const FProperty& Property)
{
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		return IsExpressionInputStruct(Struct->Struct);
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		return IsConnectionProperty(*Array->Inner);
	}
	return false;
}

bool IsExpressionReference(const FProperty& Property)
{
	const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(&Property);
	return Object != nullptr && !Property.IsA<FSoftObjectProperty>() && !Property.IsA<FClassProperty>()
		&& Object->PropertyClass != nullptr && Object->PropertyClass->IsChildOf(UMaterialExpression::StaticClass());
}

// True when the value holds pin connections or expression references and is walked member by member.
bool NeedsWalk(const FProperty& Property)
{
	static TMap<const FProperty*, bool> Cache;
	if (const bool* Known = Cache.Find(&Property))
	{
		return *Known;
	}
	Cache.Add(&Property, false); // recursion guard
	bool bWalk = IsConnectionProperty(Property) || IsExpressionReference(Property);
	if (!bWalk)
	{
		if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
		{
			for (TFieldIterator<FProperty> It(Struct->Struct); It && !bWalk; ++It)
			{
				bWalk = !It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated) && NeedsWalk(**It);
			}
		}
		else if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
		{
			bWalk = NeedsWalk(*Array->Inner);
		}
	}
	Cache.Add(&Property, bWalk);
	return bWalk;
}

// float32 values extract as their shortest decimal; integral numbers decode into float members.
void CoerceNumbers(const FProperty& Property, FValue& Value, bool bEncode)
{
	if (Property.ArrayDim > 1 && Value.Kind == EAssetDocumentRuntimeValueKind::Array)
	{
		for (FValue& Element : Value.Elements)
		{
			// Element values of a fixed array share the property's type.
			if (Property.IsA<FFloatProperty>() || Property.IsA<FDoubleProperty>())
			{
				if (!bEncode && Element.Kind == EAssetDocumentRuntimeValueKind::Integer)
				{
					Element.Kind = EAssetDocumentRuntimeValueKind::Number;
					Element.NumberValue = static_cast<double>(Element.IntegerValue);
				}
				else if (bEncode && Property.IsA<FFloatProperty>() && Element.Kind == EAssetDocumentRuntimeValueKind::Number)
				{
					Element.NumberValue = ShortestFloat(static_cast<float>(Element.NumberValue));
				}
			}
		}
		return;
	}
	if (Property.IsA<FFloatProperty>() || Property.IsA<FDoubleProperty>())
	{
		if (!bEncode && Value.Kind == EAssetDocumentRuntimeValueKind::Integer)
		{
			Value.Kind = EAssetDocumentRuntimeValueKind::Number;
			Value.NumberValue = static_cast<double>(Value.IntegerValue);
			Value.IntegerValue = 0;
		}
		if (bEncode && Property.IsA<FFloatProperty>() && Value.Kind == EAssetDocumentRuntimeValueKind::Number)
		{
			Value.NumberValue = ShortestFloat(static_cast<float>(Value.NumberValue));
		}
		return;
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		if (Value.Kind == EAssetDocumentRuntimeValueKind::Object && Struct->Struct != nullptr)
		{
			for (int32 Index = 0; Index < Value.ObjectKeys.Num() && Index < Value.Elements.Num(); ++Index)
			{
				if (const FProperty* Field = Struct->Struct->FindPropertyByName(FName(*Value.ObjectKeys[Index])))
				{
					CoerceNumbers(*Field, Value.Elements[Index], bEncode);
				}
			}
		}
		return;
	}
	const FProperty* Inner = nullptr;
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		Inner = Array->Inner;
	}
	else if (const FSetProperty* Set = CastField<FSetProperty>(&Property))
	{
		Inner = Set->ElementProp;
	}
	else if (const FMapProperty* Map = CastField<FMapProperty>(&Property))
	{
		Inner = Map->ValueProp;
	}
	if (Inner != nullptr && (Value.Kind == EAssetDocumentRuntimeValueKind::Array || Value.Kind == EAssetDocumentRuntimeValueKind::Object))
	{
		for (FValue& Element : Value.Elements)
		{
			CoerceNumbers(*Inner, Element, bEncode);
		}
	}
}

struct FCodecContext
{
	const TMap<FString, UMaterialExpression*>* ByKey = nullptr;
	UObject* Owner = nullptr;
};

FValue EncodeValue(const FProperty& Property, const void* ValuePtr, const FCodecContext& Context);

FValue EncodeWalked(const FProperty& Property, const void* ValuePtr, const FCodecContext& Context)
{
	if (IsExpressionReference(Property))
	{
		const UObject* Object = CastFieldChecked<FObjectPropertyBase>(&Property)->GetObjectPropertyValue(ValuePtr);
		if (Object == nullptr)
		{
			return MakeNull();
		}
		const UMaterialExpression* Expression = Cast<UMaterialExpression>(Object);
		return MakeString(Expression != nullptr && Expression->GetOuter() == Context.Owner ? KeyOf(*Expression) : Object->GetPathName());
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		FObjectBuilder Builder;
		for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated) || IsConnectionProperty(**It))
			{
				continue;
			}
			Builder.Add(It->GetName(), EncodeValue(**It, It->ContainerPtrToValuePtr<void>(ValuePtr), Context));
		}
		return Builder.Build();
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		FScriptArrayHelper Helper(Array, ValuePtr);
		TArray<FValue> Elements;
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			Elements.Add(EncodeValue(*Array->Inner, Helper.GetRawPtr(Index), Context));
		}
		return MakeArray(MoveTemp(Elements));
	}
	return MakeNull();
}

// One value (ValuePtr = the property's own memory; a fixed-size array is the whole array).
FValue EncodeValue(const FProperty& Property, const void* ValuePtr, const FCodecContext& Context)
{
	if (NeedsWalk(Property))
	{
		if (Property.ArrayDim > 1)
		{
			TArray<FValue> Elements;
			for (int32 Index = 0; Index < Property.ArrayDim; ++Index)
			{
				Elements.Add(EncodeWalked(Property, static_cast<const uint8*>(ValuePtr) + Index * Property.GetElementSize(), Context));
			}
			return MakeArray(MoveTemp(Elements));
		}
		return EncodeWalked(Property, ValuePtr, Context);
	}
	FProperty& Mutable = const_cast<FProperty&>(Property);
	FString CodecId;
	FValue Value;
	FString Code;
	if (EditorPropertyValueCodec::Inspect(Mutable, CodecId) && EditorPropertyValueCodec::Encode(Mutable, ValuePtr, Value, Code))
	{
		CoerceNumbers(Property, Value, true);
		return Value;
	}
	FString Text;
	Property.ExportTextItem_Direct(Text, ValuePtr, nullptr, Context.Owner, PPF_None);
	return MakeString(MoveTemp(Text));
}

bool DecodeValue(const FProperty& Property, const FValue& Value, void* ValuePtr, const FCodecContext& Context, FString& OutProblem);

bool DecodeWalked(const FProperty& Property, const FValue& Value, void* ValuePtr, const FCodecContext& Context, FString& OutProblem)
{
	if (IsExpressionReference(Property))
	{
		const FObjectPropertyBase* Object = CastFieldChecked<FObjectPropertyBase>(&Property);
		if (Value.Kind == EAssetDocumentRuntimeValueKind::Null)
		{
			Object->SetObjectPropertyValue(ValuePtr, nullptr);
			return true;
		}
		if (Value.Kind != EAssetDocumentRuntimeValueKind::String)
		{
			OutProblem = FString::Printf(TEXT("%s is the key of a %s expression in this graph, or null."), *Property.GetName(),
				*ClassLabel(*Object->PropertyClass));
			return false;
		}
		UMaterialExpression* const* Found = Context.ByKey->Find(Value.StringValue);
		if (Found == nullptr || !(*Found)->IsA(Object->PropertyClass))
		{
			OutProblem = FString::Printf(TEXT("%s: '%s' is not a %s expression of this graph."), *Property.GetName(), *Value.StringValue,
				*ClassLabel(*Object->PropertyClass));
			return false;
		}
		Object->SetObjectPropertyValue(ValuePtr, *Found);
		return true;
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		if (Value.Kind != EAssetDocumentRuntimeValueKind::Object)
		{
			OutProblem = FString::Printf(TEXT("%s is an object of %s fields."), *Property.GetName(), *Struct->Struct->GetName());
			return false;
		}
		for (int32 Index = 0; Index < Value.ObjectKeys.Num(); ++Index)
		{
			FProperty* Field = Struct->Struct->FindPropertyByName(FName(*Value.ObjectKeys[Index]));
			if (Field == nullptr || Field->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated) || IsConnectionProperty(*Field))
			{
				OutProblem = FString::Printf(TEXT("%s has no field '%s' (pin connections are inputs, not fields)."),
					*Struct->Struct->GetName(), *Value.ObjectKeys[Index]);
				return false;
			}
			if (!DecodeValue(*Field, Value.Elements[Index], Field->ContainerPtrToValuePtr<void>(ValuePtr), Context, OutProblem))
			{
				OutProblem = FString::Printf(TEXT("%s.%s"), *Property.GetName(), *OutProblem);
				return false;
			}
		}
		return true;
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		if (Value.Kind != EAssetDocumentRuntimeValueKind::Array)
		{
			OutProblem = FString::Printf(TEXT("%s is an array."), *Property.GetName());
			return false;
		}
		FScriptArrayHelper Helper(Array, ValuePtr);
		Helper.Resize(Value.Elements.Num());
		for (int32 Index = 0; Index < Value.Elements.Num(); ++Index)
		{
			if (!DecodeValue(*Array->Inner, Value.Elements[Index], Helper.GetRawPtr(Index), Context, OutProblem))
			{
				OutProblem = FString::Printf(TEXT("%s[%d]: %s"), *Property.GetName(), Index, *OutProblem);
				return false;
			}
		}
		return true;
	}
	OutProblem = FString::Printf(TEXT("%s cannot be decoded."), *Property.GetName());
	return false;
}

bool DecodeValue(const FProperty& Property, const FValue& Value, void* ValuePtr, const FCodecContext& Context, FString& OutProblem)
{
	if (NeedsWalk(Property))
	{
		if (Property.ArrayDim > 1)
		{
			if (Value.Kind != EAssetDocumentRuntimeValueKind::Array || Value.Elements.Num() != Property.ArrayDim)
			{
				OutProblem = FString::Printf(TEXT("%s is an array of exactly %d values."), *Property.GetName(), Property.ArrayDim);
				return false;
			}
			for (int32 Index = 0; Index < Property.ArrayDim; ++Index)
			{
				if (!DecodeWalked(Property, Value.Elements[Index], static_cast<uint8*>(ValuePtr) + Index * Property.GetElementSize(), Context, OutProblem))
				{
					return false;
				}
			}
			return true;
		}
		return DecodeWalked(Property, Value, ValuePtr, Context, OutProblem);
	}
	FProperty& Mutable = const_cast<FProperty&>(Property);
	FString CodecId;
	if (EditorPropertyValueCodec::Inspect(Mutable, CodecId))
	{
		FValue Coerced = Value;
		CoerceNumbers(Property, Coerced, false);
		FString Code;
		if (EditorPropertyValueCodec::Decode(Mutable, Coerced, ValuePtr, Code, Context.Owner))
		{
			return true;
		}
		// (Numbers and booleans have no export-text alternative: ImportText reads "bright" as 0.)
		if (Value.Kind != EAssetDocumentRuntimeValueKind::String || Property.IsA<FNumericProperty>() || Property.IsA<FBoolProperty>())
		{
			OutProblem = FString::Printf(TEXT("%s: the value is not a %s value (%s)."), *Property.GetName(), *CodecId, *Code);
			return false;
		}
	}
	if (Value.Kind != EAssetDocumentRuntimeValueKind::String)
	{
		OutProblem = FString::Printf(TEXT("%s takes UE's export-text form as a string."), *Property.GetName());
		return false;
	}
	if (Property.ImportText_Direct(*Value.StringValue, ValuePtr, Context.Owner, PPF_None) == nullptr)
	{
		OutProblem = FString::Printf(TEXT("%s: '%s' is not a valid value."), *Property.GetName(), *Value.StringValue);
		return false;
	}
	return true;
}

bool SameValue(const FValue& A, const FValue& B)
{
	if (A.Kind != B.Kind)
	{
		const bool bANumber = A.Kind == EAssetDocumentRuntimeValueKind::Number || A.Kind == EAssetDocumentRuntimeValueKind::Integer;
		const bool bBNumber = B.Kind == EAssetDocumentRuntimeValueKind::Number || B.Kind == EAssetDocumentRuntimeValueKind::Integer;
		if (bANumber && bBNumber)
		{
			const double Left = A.Kind == EAssetDocumentRuntimeValueKind::Number ? A.NumberValue : static_cast<double>(A.IntegerValue);
			const double Right = B.Kind == EAssetDocumentRuntimeValueKind::Number ? B.NumberValue : static_cast<double>(B.IntegerValue);
			return Left == Right;
		}
		return false;
	}
	switch (A.Kind)
	{
	case EAssetDocumentRuntimeValueKind::Null: return true;
	case EAssetDocumentRuntimeValueKind::Boolean: return A.BooleanValue == B.BooleanValue;
	case EAssetDocumentRuntimeValueKind::Integer: return A.IntegerValue == B.IntegerValue;
	case EAssetDocumentRuntimeValueKind::Number: return A.NumberValue == B.NumberValue;
	case EAssetDocumentRuntimeValueKind::String: return A.StringValue.Equals(B.StringValue, ESearchCase::CaseSensitive);
	case EAssetDocumentRuntimeValueKind::Array:
	{
		if (A.Elements.Num() != B.Elements.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < A.Elements.Num(); ++Index)
		{
			if (!SameValue(A.Elements[Index], B.Elements[Index]))
			{
				return false;
			}
		}
		return true;
	}
	case EAssetDocumentRuntimeValueKind::Object:
	{
		if (A.ObjectKeys.Num() != B.ObjectKeys.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < A.ObjectKeys.Num(); ++Index)
		{
			const FValue* Other = FindField(B, A.ObjectKeys[Index]);
			if (Other == nullptr || !SameValue(A.Elements[Index], *Other))
			{
				return false;
			}
		}
		return true;
	}
	default:
		return false;
	}
}

// Properties a document authors on an expression of Class, in declaration order (base class first).
const TArray<FProperty*>& AuthoredProperties(const UClass& Class)
{
	static TMap<const UClass*, TArray<FProperty*>> Cache;
	if (const TArray<FProperty*>* Known = Cache.Find(&Class))
	{
		return *Known;
	}
	static const TSet<FName> Excluded = {
		TEXT("MaterialExpressionEditorX"), TEXT("MaterialExpressionEditorY"), TEXT("GraphNode"), TEXT("MaterialExpressionGuid"),
		TEXT("Material"), TEXT("Function"), TEXT("Outputs"), TEXT("bRealtimePreview"), TEXT("bNeedToUpdatePreview"),
		TEXT("bIsParameterExpression"), TEXT("bShowOutputNameOnPin"), TEXT("bShowMaskColorsOnPin"), TEXT("bHidePreviewWindow"),
		TEXT("bShaderInputData"), TEXT("bShowInputs"), TEXT("bShowOutputs"), TEXT("MenuCategories"), TEXT("ExpressionGUID"),
		TEXT("VariableGuid"), TEXT("DeclarationGuid"), TEXT("FunctionInputs"), TEXT("FunctionOutputs"), TEXT("Id")};
	// Node state the graph stores outside the Details panel.
	// (bLastPreviewed: the function output the Material Function Editor previews.)
	static const TSet<FName> GraphState = {TEXT("bCollapsed"), TEXT("bCommentBubbleVisible"), TEXT("SizeX"), TEXT("SizeY"),
		TEXT("bLastPreviewed")};
	TArray<FProperty*>& Properties = Cache.Add(&Class);
	TArray<UStruct*> Chain;
	for (UStruct* Current = const_cast<UClass*>(&Class); Current != nullptr; Current = Current->GetSuperStruct())
	{
		Chain.Insert(Current, 0);
	}
	for (UStruct* Struct : Chain)
	{
		for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated) || Excluded.Contains(Property->GetFName())
				|| IsConnectionProperty(*Property))
			{
				continue;
			}
			const bool bDetails = Property->HasAnyPropertyFlags(CPF_Edit) && !Property->HasAnyPropertyFlags(CPF_EditConst);
			if (bDetails || (NeedsWalk(*Property) && !IsConnectionProperty(*Property)) || GraphState.Contains(Property->GetFName()))
			{
				Properties.Add(Property);
			}
		}
	}
	return Properties;
}

FProperty* FindAuthoredProperty(const UClass& Class, const FString& Name)
{
	for (FProperty* Property : AuthoredProperties(Class))
	{
		if (Property->GetName() == Name)
		{
			return Property;
		}
	}
	return nullptr;
}

// ---------------------------------------------------------------------------------------------
// New nodes (the editor's factory path) and per-call baselines
// ---------------------------------------------------------------------------------------------
UMaterialExpression* CreateExpression(const FHost& Host, UClass& Class, int32 X, int32 Y)
{
	if (Class.IsChildOf(UMaterialExpressionComment::StaticClass()))
	{
		// FMaterialEditor::CreateNewMaterialExpressionComment (no selection: 400 x 100 at the position).
		UMaterialExpressionComment* Comment = NewObject<UMaterialExpressionComment>(Host.GetOuter(), &Class, NAME_None, RF_Transactional);
		Host.Collection().AddComment(Comment);
		Comment->MaterialExpressionEditorX = X;
		Comment->MaterialExpressionEditorY = Y;
		Comment->SizeX = 400;
		Comment->SizeY = 100;
		Comment->Text = NSLOCTEXT("K2Node", "CommentBlock_NewEmptyComment", "Comment").ToString();
		Comment->CommentColor = GetDefault<UGraphEditorSettings>()->DefaultCommentNodeTitleColor;
		Host.GetOuter()->MarkPackageDirty();
		return Comment;
	}
	return UMaterialEditingLibrary::CreateMaterialExpressionEx(Host.Material, Host.Material != nullptr ? nullptr : Host.Function,
		&Class, nullptr, X, Y);
}

// A new node of each class, created the editor way in a transient material / function (the values an omitted
// property means). Discarded with the call.
class FBaselines
{
public:
	explicit FBaselines(bool bInFunction) : bFunction(bInFunction) {}
	~FBaselines()
	{
		for (TPair<UClass*, TStrongObjectPtr<UMaterialExpression>>& Pair : Nodes)
		{
			if (Pair.Value.IsValid())
			{
				Pair.Value->MarkAsGarbage();
			}
		}
		if (Host.IsValid())
		{
			Host->MarkAsGarbage();
		}
	}

	UMaterialExpression* Get(UClass& Class)
	{
		if (TStrongObjectPtr<UMaterialExpression>* Known = Nodes.Find(&Class))
		{
			return Known->Get();
		}
		if (!Host.IsValid())
		{
			Host.Reset(bFunction ? static_cast<UObject*>(NewObject<UMaterialFunction>(GetTransientPackage(), NAME_None, RF_Transient))
				: static_cast<UObject*>(NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient)));
		}
		FHost Transient;
		Transient.Material = Cast<UMaterial>(Host.Get());
		Transient.Function = Cast<UMaterialFunction>(Host.Get());
		UMaterialExpression* Node = CreateExpression(Transient, Class, 0, 0);
		Nodes.Add(&Class, TStrongObjectPtr<UMaterialExpression>(Node));
		return Node;
	}

private:
	bool bFunction = false;
	TStrongObjectPtr<UObject> Host;
	TMap<UClass*, TStrongObjectPtr<UMaterialExpression>> Nodes;
};

// Properties whose new-node value is random (a named reroute's color: FLinearColor::MakeRandomColor in the
// constructor). They are always read; omitted on edit they keep the node's value (a new node: the editor's color).
bool IsVolatileDefault(const FProperty& Property)
{
	return Property.GetFName() == TEXT("NodeColor") && Property.GetOwnerClass() != nullptr
		&& Property.GetOwnerClass()->IsChildOf(UMaterialExpressionNamedRerouteDeclaration::StaticClass());
}

FValue EncodeProperties(UMaterialExpression& Expression, FBaselines& Baselines, const FCodecContext& Context)
{
	UMaterialExpression* Baseline = Baselines.Get(*Expression.GetClass());
	FCodecContext BaselineContext;
	TMap<FString, UMaterialExpression*> NoKeys;
	BaselineContext.ByKey = &NoKeys;
	BaselineContext.Owner = Baseline != nullptr ? Baseline->GetOuter() : nullptr;
	FObjectBuilder Builder;
	for (FProperty* Property : AuthoredProperties(*Expression.GetClass()))
	{
		FValue Current = EncodeValue(*Property, Property->ContainerPtrToValuePtr<void>(&Expression), Context);
		if (Baseline != nullptr && !IsVolatileDefault(*Property))
		{
			const FValue Default = EncodeValue(*Property, Property->ContainerPtrToValuePtr<void>(Baseline), BaselineContext);
			if (SameValue(Current, Default))
			{
				continue;
			}
		}
		Builder.Add(Property->GetName(), MoveTemp(Current));
	}
	return Builder.Build();
}

FValue EncodeInputs(UMaterialExpression& Expression)
{
	FObjectBuilder Builder;
	const TArray<FString> Labels = InputLabels(Expression);
	for (int32 Index = 0; Index < Labels.Num(); ++Index)
	{
		const FExpressionInput* Input = InputAt(Expression, Index);
		if (Input != nullptr && Input->Expression != nullptr)
		{
			Builder.AddString(Labels[Index], SourceOf(*Input->Expression, Input->OutputIndex));
		}
	}
	return Builder.Build();
}

FValue DescribeExpression(UMaterialExpression& Expression, FBaselines& Baselines, const FCodecContext& Context, bool bPins)
{
	FObjectBuilder Row;
	Row.AddString(TEXT("key"), KeyOf(Expression));
	Row.AddString(TEXT("class"), ClassLabel(*Expression.GetClass()));
	Row.Add(TEXT("position"), MakeArray({MakeInt(Expression.MaterialExpressionEditorX), MakeInt(Expression.MaterialExpressionEditorY)}));
	Row.Add(TEXT("properties"), EncodeProperties(Expression, Baselines, Context));
	Row.Add(TEXT("inputs"), EncodeInputs(Expression));
	Row.Add(TEXT("outputs"), MakeStringArray(OutputLabels(Expression)));
	if (bPins)
	{
		Row.Add(TEXT("pins"), DescribePins(Expression));
	}
	return Row.Build();
}

// ---------------------------------------------------------------------------------------------
// Compile state
// ---------------------------------------------------------------------------------------------
struct FCompileState
{
	TArray<FString> Errors;
	TArray<FString> ErrorExpressions;
	bool bCompiling = false;
};

FCompileState ReadCompileState(UMaterial& Material, bool bFinishCaching)
{
	FCompileState State;
	FMaterialResource* Resource = Material.GetMaterialResource(GMaxRHIShaderPlatform);
	if (Resource == nullptr)
	{
		return State;
	}
	if (bFinishCaching && Resource->IsCachingShaders())
	{
		Resource->FinishCacheShaders();
	}
	State.Errors = Resource->GetCompileErrors();
	for (const UMaterialExpression* Expression : Resource->GetErrorExpressions())
	{
		if (Expression != nullptr && Expression->GetOuter() == &Material)
		{
			State.ErrorExpressions.AddUnique(KeyOf(*Expression));
		}
	}
	State.bCompiling = !Resource->IsCompilationFinished();
	return State;
}

// ---------------------------------------------------------------------------------------------
// Edit helpers
// ---------------------------------------------------------------------------------------------
// The Material Editor's expressions belong to its preview copy (bIsPreviewMaterial): an expression's
// PostEditChangeProperty then does not recompile the material. The edit call marks the edited material the same
// way; data.material.compile.v1 recompiles once (the editor's Apply).
class FEditorCopyScope
{
public:
	explicit FEditorCopyScope(UMaterial* InMaterial) : Material(InMaterial)
	{
		if (Material != nullptr)
		{
			bWasPreview = Material->bIsPreviewMaterial != 0;
			Material->bIsPreviewMaterial = true;
		}
	}
	~FEditorCopyScope()
	{
		if (Material != nullptr)
		{
			Material->bIsPreviewMaterial = bWasPreview;
		}
	}

private:
	UMaterial* Material = nullptr;
	bool bWasPreview = false;
};

bool IsValidKey(const FString& Key, FString& OutProblem)
{
	if (Key.IsEmpty() || Key.Len() >= NAME_SIZE)
	{
		OutProblem = TEXT("A key is 1 to 1023 characters.");
		return false;
	}
	FText Reason;
	if (!FName::IsValidXName(Key, INVALID_OBJECTNAME_CHARACTERS, &Reason) || Key.Contains(TEXT(" ")))
	{
		OutProblem = FString::Printf(TEXT("'%s' is not a valid key (an object name: letters, digits, '_' and '-'; no spaces, '.', ':' or '/')."), *Key);
		return false;
	}
	if (FName(*Key).IsNone())
	{
		OutProblem = TEXT("'None' is not a valid key.");
		return false;
	}
	return true;
}

void DeleteExpression(const FHost& Host, UMaterialExpression& Expression)
{
	// Free the key (a new expression may take it in the same apply); Core's snapshot restores the name on rollback.
	const FName Parked = MakeUniqueObjectName(Host.GetOuter(), Expression.GetClass(), FName(*FString::Printf(TEXT("Deleted_%s"), *KeyOf(Expression))));
	Expression.Rename(*Parked.ToString(), nullptr, REN_DontCreateRedirectors | REN_DoNotDirty | REN_NonTransactional);
	if (UMaterialExpressionComment* Comment = Cast<UMaterialExpressionComment>(&Expression))
	{
		Host.Collection().RemoveComment(Comment);
		Comment->MarkAsGarbage();
		Host.GetOuter()->MarkPackageDirty();
	}
	else if (Host.Material != nullptr)
	{
		UMaterialEditingLibrary::DeleteMaterialExpression(Host.Material, &Expression);
	}
	else
	{
		UMaterialEditingLibrary::DeleteMaterialExpressionInFunction(Host.Function, &Expression);
	}
}

// FMaterialEditor::NotifyPostChange for an expression property of a material. UMaterial's
// UpdateExpressionDynamicParameters / PropagateExpressionParameterChanges also refresh the editor graph nodes
// (GraphNode), which exist only in an open Material Editor; their model updates are done here without that refresh.
void NotifyMaterialOfPropertyChange(const FHost& Host, UMaterialExpression& Expression, const FProperty& Property)
{
	UMaterial* Material = Host.Material;
	if (Material == nullptr)
	{
		return;
	}
	if (Property.GetFName() == TEXT("ParameterName"))
	{
		Material->UpdateExpressionParameterName(&Expression);
	}
	else if (const UMaterialExpressionDynamicParameter* Changed = Cast<UMaterialExpressionDynamicParameter>(&Expression))
	{
		// UMaterial::UpdateExpressionDynamicParameters (UMaterialExpressionDynamicParameter::CopyDynamicParameterProperties).
		for (UMaterialExpression* Other : Host.Collection().Expressions)
		{
			UMaterialExpressionDynamicParameter* Shared = Cast<UMaterialExpressionDynamicParameter>(Other);
			if (Shared != nullptr && Shared != Changed && Shared->ParameterIndex == Changed->ParameterIndex
				&& Shared->ParamNames.Num() >= 4 && Changed->ParamNames.Num() >= 4)
			{
				Shared->Modify();
				for (int32 Index = 0; Index < 4; ++Index)
				{
					Shared->ParamNames[Index] = Changed->ParamNames[Index];
				}
				Shared->DefaultValue = Changed->DefaultValue;
			}
		}
	}
	else if (!Property.IsA<FTextProperty>())
	{
		// UMaterial::PropagateExpressionParameterChanges: parameters sharing the name take the value.
		FMaterialParameterMetadata Meta;
		if (Expression.GetParameterValue(Meta))
		{
			const FName Name = Expression.GetParameterName();
			TArray<UMaterialExpression*>* Shared = Material->EditorParameters.Find(Name);
			if (Shared != nullptr && Shared->Num() > 1)
			{
				for (UMaterialExpression* Other : *Shared)
				{
					const EMaterialExpressionSetParameterValueFlags Flags = EMaterialExpressionSetParameterValueFlags::NoUpdateExpressionGuid
						| EMaterialExpressionSetParameterValueFlags::AssignGroupAndSortPriority;
					if (Other != nullptr && Other->SetParameterValue(Name, Meta, Flags))
					{
						Other->Modify();
						Other->Desc = Meta.Description;
					}
				}
			}
		}
	}
}

// Set / Get Material Attributes keep one pin per attribute of AttributeSetTypes / AttributeGetTypes. Their
// PostEditChangeProperty updates the pins only through the open editor's graph node (GraphNode), one Details
// "+" / "-" / type change at a time; this is that pin update for any new attribute list without the graph:
// a pin follows its attribute (links kept), a new attribute gets a new pin, a removed one loses its pin, and
// links from a Get node's outputs are renumbered (LinkMaterialExpressionsFromGraph after RemovePinAt).
void SyncAttributePins(const FHost& Host, UMaterialExpression& Expression, const TArray<FGuid>& Before)
{
	// Pin names are the attributes' display names for the material (GetInputName's rule; in a function, without a
	// material, the attribute names).
	UMaterial* NameContext = Expression.Material;
	if (UMaterialExpressionSetMaterialAttributes* Set = Cast<UMaterialExpressionSetMaterialAttributes>(&Expression))
	{
		const TArray<FExpressionInput> Old = Set->Inputs;
		TArray<FExpressionInput> New;
		New.Add(Old.IsEmpty() ? FExpressionInput() : Old[0]);
		for (const FGuid& Id : Set->AttributeSetTypes)
		{
			const int32 Was = Before.IndexOfByKey(Id);
			FExpressionInput Input = Was != INDEX_NONE && Old.IsValidIndex(Was + 1) ? Old[Was + 1] : FExpressionInput();
			Input.InputName = FName(*FMaterialAttributeDefinitionMap::GetDisplayNameForMaterial(Id, NameContext).ToString());
			New.Add(Input);
		}
		Set->Inputs = MoveTemp(New);
		return;
	}
	UMaterialExpressionGetMaterialAttributes* Get = Cast<UMaterialExpressionGetMaterialAttributes>(&Expression);
	if (Get == nullptr || Get->Outputs.IsEmpty())
	{
		return;
	}
	TArray<int32> Remap;
	Remap.Init(INDEX_NONE, Get->Outputs.Num());
	Remap[0] = 0;
	TArray<FExpressionOutput> New;
	New.Add(Get->Outputs[0]);
	for (const FGuid& Id : Get->AttributeGetTypes)
	{
		const int32 Was = Before.IndexOfByKey(Id);
		if (Was != INDEX_NONE && Remap.IsValidIndex(Was + 1))
		{
			Remap[Was + 1] = New.Num();
		}
		New.Add(FExpressionOutput(FName(*FMaterialAttributeDefinitionMap::GetDisplayNameForMaterial(Id, NameContext).ToString()), 0, 0, 0, 0, 0));
	}
	Get->Outputs = MoveTemp(New);
	auto Relink = [Get, &Remap](FExpressionInput* Input, UObject* Owner)
	{
		if (Input == nullptr || Input->Expression != Get)
		{
			return;
		}
		const int32 Index = Remap.IsValidIndex(Input->OutputIndex) ? Remap[Input->OutputIndex] : INDEX_NONE;
		if (Index != Input->OutputIndex)
		{
			Owner->Modify();
			if (Index == INDEX_NONE)
			{
				Input->Expression = nullptr;
				Input->OutputIndex = 0;
			}
			else
			{
				Input->OutputIndex = Index;
			}
		}
	};
	for (UMaterialExpression* Other : Host.Collection().Expressions)
	{
		if (Other != nullptr)
		{
			for (FExpressionInputIterator It{Other}; It; ++It)
			{
				Relink(It.Input, Other);
			}
		}
	}
	if (Host.Material != nullptr)
	{
		for (const FMaterialInputPin& Pin : MaterialInputPins(*Host.Material))
		{
			Relink(Host.Material->GetExpressionInputForProperty(Pin.Property), Host.Material->GetEditorOnlyData());
		}
	}
}

// Writes the authored properties of one expression (omitted = the new-node value); returns false on a bad value.
bool SetProperties(FReader& Op, const FHost& Host, UMaterialExpression& Expression, const FValue* Properties,
	FBaselines& Baselines, const FCodecContext& Context, bool& bOutChanged)
{
	UClass& Class = *Expression.GetClass();
	const TArray<FProperty*>& Authored = AuthoredProperties(Class);
	if (Properties != nullptr)
	{
		for (int32 Index = 0; Index < Properties->ObjectKeys.Num(); ++Index)
		{
			const FString& Name = Properties->ObjectKeys[Index];
			if (FindAuthoredProperty(Class, Name) != nullptr)
			{
				continue;
			}
			TArray<FString> Names;
			for (const FProperty* Property : Authored)
			{
				Names.Add(Property->GetName());
			}
			const TArray<FString> Suggestions = SuggestActionKeys(Name, Names);
			const bool bConnection = Class.FindPropertyByName(FName(*Name)) != nullptr
				&& IsConnectionProperty(*Class.FindPropertyByName(FName(*Name)));
			return Op.Fail(TEXT("Material.UnknownProperty"), FString::Printf(TEXT("%s has no authored property '%s'%s.%s Properties: %s."),
				*ClassLabel(Class), *Name, bConnection ? TEXT(" (it is a pin: author it under Inputs)") : TEXT(""),
				Suggestions.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" Did you mean: %s?"), *FString::Join(Suggestions, TEXT(", "))),
				*FString::Join(Names, TEXT(", "))), FString::Printf(TEXT("properties/%s"), *Name));
		}
	}
	UMaterialExpression* Baseline = Baselines.Get(Class);
	FCodecContext BaselineContext;
	TMap<FString, UMaterialExpression*> NoKeys;
	BaselineContext.ByKey = &NoKeys;
	BaselineContext.Owner = Baseline != nullptr ? Baseline->GetOuter() : nullptr;
	for (FProperty* Property : Authored)
	{
		const FValue* Authored_ = Properties != nullptr ? FindField(*Properties, Property->GetName()) : nullptr;
		if (Authored_ == nullptr && IsVolatileDefault(*Property))
		{
			continue;
		}
		const FValue Desired = Authored_ != nullptr ? *Authored_
			: (Baseline != nullptr ? EncodeValue(*Property, Property->ContainerPtrToValuePtr<void>(Baseline), BaselineContext) : FValue());
		if (Authored_ == nullptr && Baseline == nullptr)
		{
			continue;
		}
		void* Live = Property->ContainerPtrToValuePtr<void>(&Expression);
		const FValue Current = EncodeValue(*Property, Live, Context);
		if (SameValue(Current, Desired))
		{
			continue;
		}
		// Decode onto a copy of the current value (connections inside walked structs stay as they are).
		void* Temp = FMemory::Malloc(Property->GetSize(), Property->GetMinAlignment());
		Property->InitializeValue(Temp);
		Property->CopyCompleteValue(Temp, Live);
		FString Problem;
		const bool bDecoded = DecodeValue(*Property, Desired, Temp, Context, Problem);
		const bool bDifferent = bDecoded && !SameValue(EncodeValue(*Property, Temp, Context), Current);
		if (bDifferent)
		{
			// The Details panel's edit.
			Expression.Modify();
			Expression.PreEditChange(Property);
			const bool bAttributePins = (Property->GetFName() == TEXT("AttributeSetTypes") && Expression.IsA<UMaterialExpressionSetMaterialAttributes>())
				|| (Property->GetFName() == TEXT("AttributeGetTypes") && Expression.IsA<UMaterialExpressionGetMaterialAttributes>());
			const TArray<FGuid> AttributesBefore = bAttributePins ? *static_cast<const TArray<FGuid>*>(Live) : TArray<FGuid>();
			Property->CopyCompleteValue(Live, Temp);
			FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
			Expression.PostEditChangeProperty(Event);
			if (bAttributePins)
			{
				SyncAttributePins(Host, Expression, AttributesBefore);
			}
			NotifyMaterialOfPropertyChange(Host, Expression, *Property);
			bOutChanged = true;
		}
		Property->DestroyValue(Temp);
		FMemory::Free(Temp);
		if (!bDecoded)
		{
			return Op.Fail(TEXT("Material.InvalidPropertyValue"), FString::Printf(TEXT("%s.%s: %s"), *ClassLabel(Class),
				*Property->GetName(), *Problem), FString::Printf(TEXT("properties/%s"), *Property->GetName()));
		}
	}
	return true;
}

bool ReadPosition(FReader& Op, int32& OutX, int32& OutY, bool& bOutHas)
{
	bOutHas = false;
	const FValue* Position = Op.Find(TEXT("position"));
	if (Position == nullptr)
	{
		return true;
	}
	// Integral numbers (-1300.0) are accepted: JSON writers may not keep the integer kind.
	auto Coordinate = [](const FValue& Element, int32& Out)
	{
		if (Element.Kind == EAssetDocumentRuntimeValueKind::Integer)
		{
			Out = static_cast<int32>(Element.IntegerValue);
			return true;
		}
		if (Element.Kind == EAssetDocumentRuntimeValueKind::Number && FMath::IsFinite(Element.NumberValue)
			&& FMath::FloorToDouble(Element.NumberValue) == Element.NumberValue)
		{
			Out = static_cast<int32>(Element.NumberValue);
			return true;
		}
		return false;
	};
	if (Position->Kind != EAssetDocumentRuntimeValueKind::Array || Position->Elements.Num() != 2
		|| !Coordinate(Position->Elements[0], OutX) || !Coordinate(Position->Elements[1], OutY))
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("position is [x, y] (two integers)."), TEXT("position"));
	}
	bOutHas = true;
	return true;
}

// Resolves "<key>[.<output>]" against the graph.
bool ResolveSource(FReader& Op, const TMap<FString, UMaterialExpression*>& ByKey, const FString& Text,
	UMaterialExpression*& OutSource, int32& OutIndex)
{
	FString Key = Text;
	FString Label;
	int32 Dot = INDEX_NONE;
	if (Text.FindChar(TEXT('.'), Dot))
	{
		Key = Text.Left(Dot);
		Label = Text.Mid(Dot + 1);
	}
	UMaterialExpression* const* Found = ByKey.Find(Key);
	if (Found == nullptr)
	{
		TArray<FString> Keys;
		ByKey.GetKeys(Keys);
		const TArray<FString> Suggestions = SuggestActionKeys(Key, Keys);
		return Op.Fail(TEXT("Material.ExpressionNotFound"), FString::Printf(TEXT("No expression '%s' in this graph.%s"), *Key,
			Suggestions.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" Did you mean: %s?"), *FString::Join(Suggestions, TEXT(", ")))),
			TEXT("from"));
	}
	const TArray<FString> Labels = OutputLabels(**Found);
	int32 Index = FindLabel(Labels, Label);
	// "<key>" is output 0. Nodes saved by older engine versions can keep an unnamed output 0 where a new node
	// of the class names it ("RGB"); that name also means output 0 of such a node.
	if (Index == INDEX_NONE && (Label.IsEmpty() || (!Labels.IsEmpty() && Labels[0].IsEmpty() && Label == FreshOutputZero(**Found))))
	{
		Index = 0;
	}
	if (Index == INDEX_NONE)
	{
		TArray<FString> Forms;
		for (const FString& Output : Labels)
		{
			Forms.Add(Output.IsEmpty() ? Key : FString::Printf(TEXT("%s.%s"), *Key, *Output));
		}
		return Op.Fail(TEXT("Material.PinNotFound"), FString::Printf(TEXT("%s (%s) has no output '%s'. Outputs: %s."), *Key,
			*ClassLabel(*(*Found)->GetClass()), *Label, Forms.IsEmpty() ? TEXT("none") : *FString::Join(Forms, TEXT(", "))), TEXT("from"));
	}
	OutSource = *Found;
	OutIndex = Index;
	return true;
}

// UMaterialGraphSchema::CanCreateConnection's type rule (r.PreventInvalidMaterialConnections).
bool CheckConnectionType(FReader& Op, EMaterialValueType InputType, UMaterialExpression& Source, int32 OutputIndex, const FString& Where)
{
	static const IConsoleVariable* Prevent = IConsoleManager::Get().FindConsoleVariable(TEXT("r.PreventInvalidMaterialConnections"));
	if (Prevent != nullptr && Prevent->GetInt() == 0)
	{
		return true;
	}
	const EMaterialValueType OutputType = Source.GetOutputValueType(OutputIndex);
	if (CanConnectMaterialValueTypes(InputType, OutputType))
	{
		return true;
	}
	return Op.Fail(TEXT("Material.IncompatibleConnection"), FString::Printf(
		TEXT("%s (%s) is not compatible with %s (%s); the Material Editor refuses this wire."),
		*SourceOf(Source, OutputIndex), *ValueTypeName(OutputType), *Where, *ValueTypeName(InputType)), TEXT("from"));
}

void ConnectInput(FExpressionInput& Input, UObject& Owner, UMaterialExpression* Source, int32 OutputIndex, bool& bOutChanged)
{
	// UMaterialGraph::LinkMaterialExpressionsFromGraph.
	if (Source == nullptr)
	{
		if (Input.Expression != nullptr)
		{
			Owner.Modify();
			Input.Expression = nullptr;
			bOutChanged = true;
		}
		return;
	}
	if (!Source->IsExpressionConnected(&Input, OutputIndex))
	{
		Owner.Modify();
		Source->Modify();
		Input.Connect(OutputIndex, Source);
		if (Input.Expression != Source && Source->GetOutputs().IsEmpty() && OutputIndex == 0)
		{
			// A node without output pins (LegacyBlendMaterialAttributes) that older assets link from output 0:
			// ConnectExpression refuses it; keep the link as those assets store it.
			Input.Expression = Source;
			Input.OutputIndex = 0;
			Input.Mask = Input.MaskR = Input.MaskG = Input.MaskB = Input.MaskA = 0;
		}
		bOutChanged = true;
	}
}

// A named reroute usage keeps its declaration's guid (the editor sets both when it creates a usage).
void SyncNamedReroutes(const FHost& Host)
{
	for (UMaterialExpression* Expression : GetExpressions(Host))
	{
		UMaterialExpressionNamedRerouteUsage* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(Expression);
		if (Usage != nullptr && Usage->Declaration != nullptr && Usage->DeclarationGuid != Usage->Declaration->VariableGuid)
		{
			Usage->Modify();
			Usage->DeclarationGuid = Usage->Declaration->VariableGuid;
		}
	}
}
} // namespace

namespace AssetDocumentDataMaterialPrimitivesLocal
{
// Instances named in full; the rest are counted.
constexpr int32 MaxNamedOverridingInstances = 10;
// Referencer packages the walk visits (instances of instances included).
constexpr int32 MaxInstanceReferencerPackages = 4096;

// The instance's value of Name comes from an instance override (its own or a parent instance's), so a new material
// default does not reach it.
bool OverridesParameter(const UMaterialInstance& Instance, FName Name)
{
	const FMemoryImageMaterialParameterInfo Info(Name);
	for (int32 Type = 0; Type < static_cast<int32>(EMaterialParameterType::Num); ++Type)
	{
		FMaterialParameterMetadata Value;
		if (Instance.GetParameterValue(static_cast<EMaterialParameterType>(Type), Info, Value,
			EMaterialGetParameterValueFlags::CheckInstanceOverrides))
		{
			return true;
		}
	}
	return false;
}

// A trial copy, preview or other transient instance (not an asset a user has).
bool IsTransientInstance(const UMaterialInstance& Instance)
{
	const UPackage* Package = Instance.GetOutermost();
	return Package == GetTransientPackage() || Package->HasAnyFlags(RF_Transient) || Instance.HasAnyFlags(RF_Transient);
}

// Material instances (saved: asset registry referencers, recursively; loaded: in memory) whose base material is
// Material. Transient instances (trial copies, previews) are not listed.
TArray<UMaterialInstance*> FindMaterialInstances(const UMaterial& Material)
{
	TSet<UMaterialInstance*> Found;
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FName> Pending{Material.GetOutermost()->GetFName()};
	TSet<FName> Visited{Pending[0]};
	while (!Pending.IsEmpty() && Visited.Num() < MaxInstanceReferencerPackages)
	{
		TArray<FName> Referencers;
		Registry.GetReferencers(Pending.Pop(), Referencers, UE::AssetRegistry::EDependencyCategory::Package);
		for (const FName& Package : Referencers)
		{
			if (Visited.Contains(Package))
			{
				continue;
			}
			Visited.Add(Package);
			TArray<FAssetData> Assets;
			Registry.GetAssetsByPackageName(Package, Assets);
			for (const FAssetData& Asset : Assets)
			{
				const UClass* Class = Asset.GetClass();
				if (Class == nullptr || !Class->IsChildOf<UMaterialInstance>())
				{
					continue;
				}
				if (UMaterialInstance* Instance = Cast<UMaterialInstance>(Asset.GetAsset()))
				{
					Found.Add(Instance);
					Pending.Add(Package);
				}
			}
		}
	}
	for (TObjectIterator<UMaterialInstance> It; It; ++It)
	{
		if (It->IsAsset() && !IsTransientInstance(**It))
		{
			Found.Add(*It);
		}
	}
	TArray<UMaterialInstance*> Result;
	for (UMaterialInstance* Instance : Found)
	{
		if (Instance->IsAsset() && !IsTransientInstance(*Instance) && Instance->GetBaseMaterial() == &Material)
		{
			Result.Add(Instance);
		}
	}
	Result.Sort([](const UMaterialInstance& Left, const UMaterialInstance& Right) { return Left.GetPathName() < Right.GetPathName(); });
	return Result;
}

// {instance_overrides: [parameter name]} -> {instance_overrides: {count, instances: [{path, parameters}]}}: the material
// instances whose value of a named parameter is an instance override (so a changed default does not reach them); the
// first MaxNamedOverridingInstances are named. The material a trial copy stands in for is the one searched.
bool ReadInstanceOverrides(FCall& Call, const FHost& Host, const FValue& Names, FObjectBuilder& Result)
{
	TArray<FName> Parameters;
	for (int32 Index = 0; Index < Names.Elements.Num(); ++Index)
	{
		const FValue& Name = Names.Elements[Index];
		if (Name.Kind != EAssetDocumentRuntimeValueKind::String || Name.StringValue.IsEmpty())
		{
			return Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("A parameter name is a non-empty string."),
				FString::Printf(TEXT("instance_overrides/%d"), Index));
		}
		Parameters.AddUnique(FName(*Name.StringValue));
	}
	const UMaterial* Material = Host.Material;
	if (Material != nullptr)
	{
		if (const UMaterial* Source = Cast<UMaterial>(FAssetDocumentEditorTrialScope::FindTrialSource(*Material)))
		{
			Material = Source;
		}
		else if (FAssetDocumentEditorTrialScope::IsTrialTarget(*Material))
		{
			Material = nullptr;
		}
	}
	TArray<FValue> Named;
	int32 Count = 0;
	if (Material != nullptr && !Parameters.IsEmpty())
	{
		for (UMaterialInstance* Instance : FindMaterialInstances(*Material))
		{
			TArray<FString> Overridden;
			for (const FName& Parameter : Parameters)
			{
				if (OverridesParameter(*Instance, Parameter))
				{
					Overridden.Add(Parameter.ToString());
				}
			}
			if (Overridden.IsEmpty())
			{
				continue;
			}
			if (++Count <= MaxNamedOverridingInstances)
			{
				FObjectBuilder Row;
				Row.AddString(TEXT("path"), Instance->GetPathName());
				Row.Add(TEXT("parameters"), MakeStringArray(Overridden));
				Named.Add(Row.Build());
			}
		}
	}
	FObjectBuilder Overrides;
	Overrides.AddInt(TEXT("count"), Count);
	Overrides.Add(TEXT("instances"), MakeArray(MoveTemp(Named)));
	Result.Add(TEXT("instance_overrides"), Overrides.Build());
	return true;
}
} // namespace AssetDocumentDataMaterialPrimitivesLocal

// {classes: [label]} -> {classes: {label: {properties: {every authored property of a new node}, pins}}}: the
// values an omitted property means, and the pins of a new node (expression catalogue lookup).
bool ReadClasses(FCall& Call, const FHost& Host, const FValue& Labels, FObjectBuilder& Result)
{
	FBaselines Baselines(Host.Function != nullptr);
	FObjectBuilder Classes;
	int32 NodeCount = 8;
	for (int32 Index = 0; Index < Labels.Elements.Num(); ++Index)
	{
		const FValue& Label = Labels.Elements[Index];
		FString Problem;
		UClass* Class = Label.Kind == EAssetDocumentRuntimeValueKind::String ? ResolveExpressionClass(Label.StringValue, Problem) : nullptr;
		if (Class == nullptr)
		{
			return Call.Args.Fail(TEXT("Material.UnknownExpressionClass"), Problem.IsEmpty() ? FString(TEXT("A class is a string.")) : Problem,
				FString::Printf(TEXT("classes/%d"), Index));
		}
		UMaterialExpression* Baseline = Baselines.Get(*Class);
		if (Baseline == nullptr)
		{
			continue;
		}
		TMap<FString, UMaterialExpression*> NoKeys;
		FCodecContext Context;
		Context.ByKey = &NoKeys;
		Context.Owner = Baseline->GetOuter();
		FObjectBuilder Properties;
		TArray<FString> Volatile;
		for (FProperty* Property : AuthoredProperties(*Class))
		{
			if (IsVolatileDefault(*Property))
			{
				Volatile.Add(Property->GetName());
				continue;
			}
			Properties.Add(Property->GetName(), EncodeValue(*Property, Property->ContainerPtrToValuePtr<void>(Baseline), Context));
		}
		FObjectBuilder Row;
		Row.Add(TEXT("properties"), Properties.Build());
		Row.Add(TEXT("volatile"), MakeStringArray(Volatile));
		Row.Add(TEXT("pins"), DescribePins(*Baseline));
		FValue RowValue = Row.Build();
		NodeCount += 1 + CountValueNodes(RowValue);
		if (NodeCount > QueryValueNodeBudget)
		{
			return Call.Args.Fail(TEXT("Graph.ResultTooLarge"), TEXT("Ask for fewer classes per call."), TEXT("classes"));
		}
		Classes.Add(Label.StringValue, MoveTemp(RowValue));
	}
	Result.Add(TEXT("classes"), Classes.Build());
	return true;
}

bool HandleMaterialRead(FCall& Call, FObjectBuilder& Result)
{
	FHost Host;
	int64 Offset = 0;
	int64 Limit = DefaultExpressionLimit;
	bool bPins = false;
	const FValue* ClassLabels = nullptr;
	const FValue* OverrideNames = nullptr;
	if (!GetHost(Call, Host)
		|| !Call.Args.OnlyFields({TEXT("offset"), TEXT("limit"), TEXT("pins"), TEXT("classes"), TEXT("instance_overrides")})
		|| !Call.Args.Int(TEXT("offset"), Offset, false) || !Call.Args.Int(TEXT("limit"), Limit, false) || !Call.Args.Bool(TEXT("pins"), bPins)
		|| !Call.Args.Array(TEXT("classes"), ClassLabels, false) || !Call.Args.Array(TEXT("instance_overrides"), OverrideNames, false))
	{
		return false;
	}
	if (OverrideNames != nullptr)
	{
		return AssetDocumentDataMaterialPrimitivesLocal::ReadInstanceOverrides(Call, Host, *OverrideNames, Result);
	}
	if (ClassLabels != nullptr)
	{
		return ReadClasses(Call, Host, *ClassLabels, Result);
	}
	if (Offset < 0 || Limit < 1)
	{
		return Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("offset must be >= 0 and limit >= 1."), TEXT("offset"));
	}
	const TArray<UMaterialExpression*> Expressions = GetExpressions(Host);
	const TMap<FString, UMaterialExpression*> ByKey = IndexByKey(Expressions);
	FCodecContext Context;
	Context.ByKey = &ByKey;
	Context.Owner = Host.GetOuter();
	FBaselines Baselines(Host.Function != nullptr);
	Result.AddString(TEXT("class"), Call.Asset.GetClass()->GetPathName());
	Result.AddString(TEXT("host"), Host.Material != nullptr ? TEXT("material") : TEXT("function"));
	int32 NodeCount = 16;
	if (Offset == 0)
	{
		if (Host.Material != nullptr)
		{
			FObjectBuilder Outputs;
			for (const FMaterialInputPin& Pin : MaterialInputPins(*Host.Material))
			{
				const FExpressionInput* Input = Host.Material->GetExpressionInputForProperty(Pin.Property);
				if (Input != nullptr && Input->Expression != nullptr)
				{
					Outputs.AddString(Pin.Name, SourceOf(*Input->Expression, Input->OutputIndex));
				}
			}
			FValue OutputsValue = Outputs.Build();
			NodeCount += CountValueNodes(OutputsValue);
			Result.Add(TEXT("outputs"), MoveTemp(OutputsValue));
			FObjectBuilder Groups;
			if (const UMaterialEditorOnlyData* EditorOnly = Host.Material->GetEditorOnlyData())
			{
				// The first entry of a group name is the one UMaterial::GetGroupSortPriority uses (older assets
				// can carry duplicates).
				TSet<FString> Seen;
				for (const FParameterGroupData& Group : EditorOnly->ParameterGroupData)
				{
					bool bSeen = false;
					Seen.Add(Group.GroupName, &bSeen);
					if (!bSeen)
					{
						Groups.AddInt(Group.GroupName, Group.GroupSortPriority);
					}
				}
			}
			FValue GroupsValue = Groups.Build();
			NodeCount += CountValueNodes(GroupsValue);
			Result.Add(TEXT("parameter_groups"), MoveTemp(GroupsValue));
			if (bPins)
			{
				TArray<FValue> Inputs;
				for (const FMaterialInputPin& Pin : MaterialInputPins(*Host.Material))
				{
					FObjectBuilder Row;
					Row.AddString(TEXT("name"), Pin.Name);
					Row.AddString(TEXT("type"), ValueTypeName(MaterialInputValueType(Pin.Property)));
					Inputs.Add(Row.Build());
				}
				NodeCount += 1 + 3 * Inputs.Num();
				Result.Add(TEXT("material_inputs"), MakeArray(MoveTemp(Inputs)));
			}
			const FCompileState Compile = ReadCompileState(*Host.Material, false);
			Result.Add(TEXT("compile_errors"), MakeStringArray(Compile.Errors));
			NodeCount += 1 + Compile.Errors.Num();
		}
		else
		{
			Result.Add(TEXT("compile_errors"), MakeStringArray(TArray<FString>()));
		}
	}
	TArray<FValue> Rows;
	const int32 NextOffset = AppendPaged(Rows, NodeCount, [&](int32 Index)
	{
		return DescribeExpression(*Expressions[Index], Baselines, Context, bPins);
	}, static_cast<int32>(Offset), Expressions.Num(), static_cast<int32>(Limit));
	Result.AddInt(TEXT("total"), Expressions.Num());
	Result.AddInt(TEXT("next_offset"), NextOffset);
	Result.Add(TEXT("expressions"), MakeArray(MoveTemp(Rows)));
	return true;
}

bool HandleMaterialEdit(FCall& Call, FObjectBuilder& Result)
{
	FHost Host;
	const FValue* Ops = nullptr;
	if (!GetHost(Call, Host) || !Call.Args.OnlyFields({TEXT("ops")}) || !Call.Args.Array(TEXT("ops"), Ops))
	{
		return false;
	}
	FAssetDocumentEditorNonInteractiveScope NonInteractive;
	const FScopedTransaction Transaction(LOCTEXT("EditMaterialGraph", "Edit Material Graph"));
	FEditorCopyScope EditorCopy(Host.Material);
	Host.GetOuter()->Modify();
	if (Host.Material != nullptr)
	{
		// The Material Editor builds the parameter map when it opens the material.
		Host.Material->BuildEditorParameterList();
	}
	FBaselines Baselines(Host.Function != nullptr);
	TMap<FString, UMaterialExpression*> ByKey = IndexByKey(GetExpressions(Host));
	FCodecContext Context;
	Context.ByKey = &ByKey;
	Context.Owner = Host.GetOuter();
	TArray<FString> Created;
	TArray<FString> Deleted;
	bool bChanged = false;

	for (int32 Index = 0; Index < Ops->Elements.Num(); ++Index)
	{
		FReader Op = Call.Args.Child(FString::Printf(TEXT("ops/%d"), Index), Ops->Elements[Index]);
		FString Kind;
		if (!Op.IsObject())
		{
			return Op.Fail(TEXT("Data.InvalidOp"), TEXT("Each op is an object."));
		}
		if (!Op.String(TEXT("op"), Kind))
		{
			return false;
		}
		if (Kind == TEXT("create"))
		{
			FString Key;
			FString ClassText;
			int32 X = 0;
			int32 Y = 0;
			bool bHasPosition = false;
			if (!Op.OnlyFields({TEXT("op"), TEXT("key"), TEXT("class"), TEXT("position")}) || !Op.String(TEXT("key"), Key)
				|| !Op.String(TEXT("class"), ClassText) || !ReadPosition(Op, X, Y, bHasPosition))
			{
				return false;
			}
			FString Problem;
			if (!IsValidKey(Key, Problem))
			{
				return Op.Fail(TEXT("Material.InvalidKey"), Problem, TEXT("key"));
			}
			if (ByKey.Contains(Key) || StaticFindObjectFast(UObject::StaticClass(), Host.GetOuter(), FName(*Key)) != nullptr)
			{
				return Op.Fail(TEXT("Material.InvalidKey"), FString::Printf(TEXT("'%s' is already the name of an object in %s; choose another key."),
					*Key, *Host.GetOuter()->GetName()), TEXT("key"));
			}
			UClass* Class = ResolveExpressionClass(ClassText, Problem);
			if (Class == nullptr)
			{
				return Op.Fail(TEXT("Material.UnknownExpressionClass"), Problem, TEXT("class"));
			}
			const UMaterialExpression* Default = Class->GetDefaultObject<UMaterialExpression>();
			// (Comments come from the graph's comment command, not the palette that IsAllowedIn filters.)
			if (!Class->IsChildOf(UMaterialExpressionComment::StaticClass()) && !Default->IsAllowedIn(Host.GetOuter()))
			{
				return Op.Fail(TEXT("Material.ExpressionNotAllowed"), FString::Printf(TEXT("%s cannot be placed in a %s (the Material Editor does not offer it there)."),
					*ClassLabel(*Class), *Host.GetOuter()->GetClass()->GetName()), TEXT("class"));
			}
			UMaterialExpression* Expression = CreateExpression(Host, *Class, X, Y);
			if (Expression == nullptr)
			{
				return Op.Fail(TEXT("Material.CreateFailed"), FString::Printf(TEXT("The editor did not create a %s."), *ClassLabel(*Class)), TEXT("class"));
			}
			Expression->Rename(*Key, nullptr, REN_DontCreateRedirectors | REN_DoNotDirty);
			ByKey.Add(Key, Expression);
			Created.Add(Key);
			bChanged = true;
		}
		else if (Kind == TEXT("delete"))
		{
			FString Key;
			if (!Op.OnlyFields({TEXT("op"), TEXT("key")}) || !Op.String(TEXT("key"), Key))
			{
				return false;
			}
			UMaterialExpression* const* Found = ByKey.Find(Key);
			if (Found == nullptr)
			{
				return Op.Fail(TEXT("Material.ExpressionNotFound"), FString::Printf(TEXT("No expression '%s' to delete."), *Key), TEXT("key"));
			}
			UMaterialExpression* Expression = *Found;
			ByKey.Remove(Key);
			DeleteExpression(Host, *Expression);
			Deleted.Add(Key);
			bChanged = true;
		}
		else if (Kind == TEXT("set"))
		{
			FString Key;
			const FValue* Properties = nullptr;
			int32 X = 0;
			int32 Y = 0;
			bool bHasPosition = false;
			if (!Op.OnlyFields({TEXT("op"), TEXT("key"), TEXT("properties"), TEXT("position")}) || !Op.String(TEXT("key"), Key)
				|| !Op.Object(TEXT("properties"), Properties, false) || !ReadPosition(Op, X, Y, bHasPosition))
			{
				return false;
			}
			UMaterialExpression* const* Found = ByKey.Find(Key);
			if (Found == nullptr)
			{
				return Op.Fail(TEXT("Material.ExpressionNotFound"), FString::Printf(TEXT("No expression '%s'."), *Key), TEXT("key"));
			}
			UMaterialExpression& Expression = **Found;
			if (bHasPosition && (Expression.MaterialExpressionEditorX != X || Expression.MaterialExpressionEditorY != Y))
			{
				Expression.Modify();
				Expression.MaterialExpressionEditorX = X;
				Expression.MaterialExpressionEditorY = Y;
				bChanged = true;
			}
			if (Op.Has(TEXT("properties")) && !SetProperties(Op, Host, Expression, Properties, Baselines, Context, bChanged))
			{
				return false;
			}
		}
		else if (Kind == TEXT("connect"))
		{
			FString Key;
			FString InputName;
			if (!Op.OnlyFields({TEXT("op"), TEXT("key"), TEXT("input"), TEXT("from")}) || !Op.String(TEXT("key"), Key)
				|| !Op.String(TEXT("input"), InputName))
			{
				return false;
			}
			UMaterialExpression* const* Found = ByKey.Find(Key);
			if (Found == nullptr)
			{
				return Op.Fail(TEXT("Material.ExpressionNotFound"), FString::Printf(TEXT("No expression '%s'."), *Key), TEXT("key"));
			}
			UMaterialExpression& Target = **Found;
			const TArray<FString> Labels = InputLabels(Target);
			int32 InputIndex = FindLabel(Labels, InputName);
			if (InputIndex == INDEX_NONE)
			{
				// The pin label of a function call carries the input type ("UVs (V2)").
				for (int32 Candidate = 0; Candidate < Labels.Num() && InputIndex == INDEX_NONE; ++Candidate)
				{
					InputIndex = Target.GetInputName(Candidate).ToString() == InputName ? Candidate : INDEX_NONE;
				}
			}
			if (InputIndex == INDEX_NONE)
			{
				return Op.Fail(TEXT("Material.PinNotFound"), FString::Printf(TEXT("%s (%s) has no input '%s'. Inputs: %s."), *Key,
					*ClassLabel(*Target.GetClass()), *InputName, Labels.IsEmpty() ? TEXT("none") : *FString::Join(Labels, TEXT(", "))),
					TEXT("input"));
			}
			FExpressionInput* Input = InputAt(Target, InputIndex);
			const bool bShown = InputIndex < ShownInputCount(Target);
			const FValue* From = Op.Find(TEXT("from"));
			if (From == nullptr || (From->Kind != EAssetDocumentRuntimeValueKind::Null && From->Kind != EAssetDocumentRuntimeValueKind::String))
			{
				return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("from is a source (\"<key>\" or \"<key>.<output>\") or null."), TEXT("from"));
			}
			UMaterialExpression* Source = nullptr;
			int32 OutputIndex = 0;
			if (From->Kind == EAssetDocumentRuntimeValueKind::String)
			{
				if (!ResolveSource(Op, ByKey, From->StringValue, Source, OutputIndex))
				{
					return false;
				}
				if (Source == &Target)
				{
					return Op.Fail(TEXT("Material.ConnectionCycle"), FString::Printf(TEXT("%s cannot feed its own input %s."), *Key, *InputName), TEXT("from"));
				}
				if (bShown && !Source->IsExpressionConnected(Input, OutputIndex)
					&& !CheckConnectionType(Op, Target.GetInputValueType(InputIndex), *Source, OutputIndex, FString::Printf(TEXT("%s input %s"), *Key, *Labels[InputIndex])))
				{
					return false;
				}
			}
			ConnectInput(*Input, Target, Source, OutputIndex, bChanged);
		}
		else if (Kind == TEXT("output"))
		{
			FString Name;
			if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("from")}) || !Op.String(TEXT("name"), Name))
			{
				return false;
			}
			if (Host.Material == nullptr)
			{
				return Op.Fail(TEXT("Material.OutputsOnFunction"), TEXT("A material function has no material inputs; use FunctionOutput expressions."), TEXT("name"));
			}
			const TArray<FMaterialInputPin> Pins = MaterialInputPins(*Host.Material);
			const FMaterialInputPin* Pin = Pins.FindByPredicate([&Name](const FMaterialInputPin& Candidate) { return Candidate.Name == Name; });
			if (Pin == nullptr)
			{
				TArray<FString> Names;
				for (const FMaterialInputPin& Candidate : Pins)
				{
					Names.Add(Candidate.Name);
				}
				const TArray<FString> Suggestions = SuggestActionKeys(Name, Names);
				return Op.Fail(TEXT("Material.PinNotFound"), FString::Printf(TEXT("The material has no input '%s'.%s Inputs: %s."), *Name,
					Suggestions.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" Did you mean: %s?"), *FString::Join(Suggestions, TEXT(", "))),
					*FString::Join(Names, TEXT(", "))), TEXT("name"));
			}
			FExpressionInput* Input = Host.Material->GetExpressionInputForProperty(Pin->Property);
			const FValue* From = Op.Find(TEXT("from"));
			if (From == nullptr || (From->Kind != EAssetDocumentRuntimeValueKind::Null && From->Kind != EAssetDocumentRuntimeValueKind::String))
			{
				return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("from is a source (\"<key>\" or \"<key>.<output>\") or null."), TEXT("from"));
			}
			UMaterialExpression* Source = nullptr;
			int32 OutputIndex = 0;
			if (From->Kind == EAssetDocumentRuntimeValueKind::String)
			{
				if (!ResolveSource(Op, ByKey, From->StringValue, Source, OutputIndex))
				{
					return false;
				}
				if (!Source->IsExpressionConnected(Input, OutputIndex)
					&& !CheckConnectionType(Op, MaterialInputValueType(Pin->Property), *Source, OutputIndex, FString::Printf(TEXT("material input %s"), *Name)))
				{
					return false;
				}
			}
			ConnectInput(*Input, *Host.Material, Source, OutputIndex, bChanged);
		}
		else if (Kind == TEXT("parameter_groups"))
		{
			const FValue* Groups = nullptr;
			if (!Op.OnlyFields({TEXT("op"), TEXT("groups")}) || !Op.Object(TEXT("groups"), Groups))
			{
				return false;
			}
			UMaterialEditorOnlyData* EditorOnly = Host.Material != nullptr ? Host.Material->GetEditorOnlyData() : nullptr;
			if (EditorOnly == nullptr)
			{
				return Op.Fail(TEXT("Material.OutputsOnFunction"), TEXT("Parameter group sorting belongs to materials."), TEXT("groups"));
			}
			TArray<FParameterGroupData> Data;
			for (int32 Group = 0; Group < Groups->ObjectKeys.Num(); ++Group)
			{
				const FValue& Priority = Groups->Elements[Group];
				const bool bIntegral = Priority.Kind == EAssetDocumentRuntimeValueKind::Number && FMath::IsFinite(Priority.NumberValue)
					&& FMath::FloorToDouble(Priority.NumberValue) == Priority.NumberValue;
				if (Priority.Kind != EAssetDocumentRuntimeValueKind::Integer && !bIntegral)
				{
					return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("A group sort priority is an integer."), FString::Printf(TEXT("groups/%s"), *Groups->ObjectKeys[Group]));
				}
				Data.Add(FParameterGroupData(Groups->ObjectKeys[Group], Priority.Kind == EAssetDocumentRuntimeValueKind::Integer
					? static_cast<int32>(Priority.IntegerValue) : static_cast<int32>(Priority.NumberValue)));
			}
			bool bSame = Data.Num() == EditorOnly->ParameterGroupData.Num();
			for (int32 Group = 0; bSame && Group < Data.Num(); ++Group)
			{
				bSame = Data[Group].GroupName == EditorOnly->ParameterGroupData[Group].GroupName
					&& Data[Group].GroupSortPriority == EditorOnly->ParameterGroupData[Group].GroupSortPriority;
			}
			if (!bSame)
			{
				EditorOnly->Modify();
				EditorOnly->ParameterGroupData = MoveTemp(Data);
				bChanged = true;
			}
		}
		else
		{
			return Op.Fail(TEXT("Data.InvalidOp"), FString::Printf(TEXT("Unknown op '%s' (create, delete, set, connect, output, parameter_groups)."), *Kind), TEXT("op"));
		}
		if (NonInteractive.WasInteractionRefused())
		{
			NonInteractive.DiscardCreatedPackages();
			return Op.Fail(TEXT("Material.InteractiveEditRefused"), TEXT(
				"The edit tried to open an editor dialog, which an MCP request cannot answer; the edit was refused."));
		}
	}
	SyncNamedReroutes(Host);
	if (bChanged)
	{
		Host.GetOuter()->MarkPackageDirty();
	}
	Result.Add(TEXT("created"), MakeStringArray(Created));
	Result.Add(TEXT("deleted"), MakeStringArray(Deleted));
	Result.AddBool(TEXT("changed"), bChanged);
	return true;
}

bool HandleMaterialCompile(FCall& Call, FObjectBuilder& Result)
{
	FHost Host;
	if (!GetHost(Call, Host) || !Call.Args.OnlyFields({}))
	{
		return false;
	}
	FAssetDocumentEditorNonInteractiveScope NonInteractive;
	if (Host.Material != nullptr)
	{
		// The Material Editor's Apply.
		Host.Material->BuildEditorParameterList();
		UMaterialEditingLibrary::RecompileMaterial(Host.Material);
		const FCompileState State = ReadCompileState(*Host.Material, true);
		Result.AddString(TEXT("status"), State.Errors.IsEmpty() ? TEXT("ok") : TEXT("errors"));
		Result.Add(TEXT("errors"), MakeStringArray(State.Errors));
		Result.Add(TEXT("error_expressions"), MakeStringArray(State.ErrorExpressions));
		Result.AddBool(TEXT("compiling"), State.bCompiling);
	}
	else
	{
		// The Material Function Editor's Apply: materials and function instances that use it recompile.
		UMaterialEditingLibrary::UpdateMaterialFunction(Host.Function, nullptr);
		Result.AddString(TEXT("status"), TEXT("ok"));
		Result.Add(TEXT("errors"), MakeStringArray(TArray<FString>()));
		Result.Add(TEXT("error_expressions"), MakeStringArray(TArray<FString>()));
		Result.AddBool(TEXT("compiling"), false);
	}
	if (NonInteractive.WasInteractionRefused())
	{
		NonInteractive.DiscardCreatedPackages();
		return Call.Args.Fail(TEXT("Material.InteractiveEditRefused"), TEXT(
			"Compiling tried to open an editor dialog, which an MCP request cannot answer."));
	}
	return true;
}

namespace Material
{
namespace
{
class FMaterialRebuilder final : public IAssetDocumentDerivedStateRebuilder
{
public:
	virtual FName GetId() const override { return TEXT("AssetDocumentData.Material"); }
	virtual bool Supports(const UObject& PrimaryAsset) const override
	{
		return PrimaryAsset.IsA<UMaterial>() || PrimaryAsset.IsA<UMaterialFunction>();
	}
	virtual FAssetDocumentContractValidationResult Rebuild(UObject& PrimaryAsset) override
	{
		FAssetDocumentContractValidationResult Result;
		if (UMaterial* RestoredMaterial = Cast<UMaterial>(&PrimaryAsset))
		{
			RestoredMaterial->bIsPreviewMaterial = false;
			RestoredMaterial->BuildEditorParameterList();
			UMaterialEditingLibrary::RecompileMaterial(RestoredMaterial);
		}
		else if (UMaterialFunction* RestoredFunction = Cast<UMaterialFunction>(&PrimaryAsset))
		{
			UMaterialEditingLibrary::UpdateMaterialFunction(RestoredFunction, nullptr);
		}
		Result.bValid = true;
		return Result;
	}
};
} // namespace

TSharedRef<IAssetDocumentDerivedStateRebuilder> MakeMaterialRebuilder()
{
	return MakeShared<FMaterialRebuilder>();
}
} // namespace Material
} // namespace UE::AssetDocument::Graph::Data

#undef LOCTEXT_NAMESPACE
