// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// data.material_instance.read.v1 / data.material_instance.edit.v1: parent and parameter overrides
// of a UMaterialInstanceConstant, as the Material Instance Editor edits them.
//
// The editor shows the parameters through UMaterialEditorInstanceConstant (UnrealEd, PreviewMaterial.cpp):
//   RegenerateArrays      -> one entry per parameter the parent chain exposes, from
//                            UMaterialInstance::GetAllParametersOfType (value, group, override flag);
//   PostEditChangeProperty-> SourceInstance->PreEditChange; on a Parent change UpdateSourceInstanceParent
//                            (SetParentEditorOnly + PostEditChange) inside an FMaterialUpdateContext;
//                            then CopyToSourceInstance: FMaterialInstanceParameterUpdateContext(Instance,
//                            ClearParameterFlag::All) + SetParameterValueEditorOnly(SetCurveAtlas) for every
//                            overridden entry + SetMaterialLayers; the context's destructor runs
//                            UpdateStaticPermutation (static switches / component masks / layers start an
//                            asynchronous shader compile, nothing waits for it); then
//                            SourceInstance->PostEditChangeProperty and MarkPackageDirty.
// The edit primitive runs the same sequence on the instance directly (no proxy object). As in the
// editor, an edit rewrites every override from the parameters the parent exposes: overrides of
// parameters the parent no longer has are dropped (reported in "dropped"); after a parent change,
// overrides whose name and type the new parent also has are kept.
//
// DTOs (all carry "schema_version": 1):
//   read  {offset?, limit?} -> {class, parent, base_material, has_static_permutation, total, next_offset,
//          parameters[{name, kind, association, index, group, sort_priority, description, asset, overridden,
//                      value, default, ui_min?, ui_max?}];
//          at offset 0 also stale_overrides[{name, kind, association, index}] and layers? {layers[], blends[],
//          names[], visible[]} (only when the instance has its own layer stack)}
//   edit  {parent?, ops[]}: {op: set, name, association?, index?, value} | {op: clear, name, association?, index?}
//          (clear of a parameter that is not overridden or not exposed is a no-op)
//          -> {overrides, dropped[], parent, has_static_permutation}
//          refraction_depth_bias? (number | null): the editor's Refraction Depth Bias, stored as a scalar
//          override entry no parent parameter exposes; kept by every edit unless set here. read reports it.
// Values (document form): scalar number; vector [R,G,B,A]; double_vector [X,Y,Z,W]; texture /
// texture_collection / runtime_virtual_texture / sparse_volume_texture / parameter_collection object path
// ("" = None); font {"Font": path, "Page": n}; static_switch bool; static_component_mask {"R","G","B","A"}.

#include "AssetDocumentDataPrimitives.h"

#include "AssetDocumentEditorNonInteractiveScope.h"
#include "Engine/Font.h"
#include "Engine/Texture.h"
#include "Engine/TextureCollection.h"
#include "MaterialCachedData.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialLayersFunctions.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "SparseVolumeTexture/SparseVolumeTexture.h"
#include "StaticParameterSet.h"
#include "UObject/SoftObjectPath.h"
#include "VT/RuntimeVirtualTexture.h"

#define LOCTEXT_NAMESPACE "AssetDocumentMaterialInstance"

namespace UE::AssetDocument::Graph::Data
{
namespace
{
constexpr int32 DefaultParameterLimit = 400;
constexpr int32 MaxListedParameters = 40;

struct FKind
{
	EMaterialParameterType Type;
	const TCHAR* Name;
};

// Every parameter type the Material Instance Editor lists (EMaterialParameterType, UE 5.7).
const FKind Kinds[] = {
	{EMaterialParameterType::Scalar, TEXT("scalar")},
	{EMaterialParameterType::Vector, TEXT("vector")},
	{EMaterialParameterType::DoubleVector, TEXT("double_vector")},
	{EMaterialParameterType::Texture, TEXT("texture")},
	{EMaterialParameterType::TextureCollection, TEXT("texture_collection")},
	{EMaterialParameterType::Font, TEXT("font")},
	{EMaterialParameterType::RuntimeVirtualTexture, TEXT("runtime_virtual_texture")},
	{EMaterialParameterType::SparseVolumeTexture, TEXT("sparse_volume_texture")},
	{EMaterialParameterType::StaticSwitch, TEXT("static_switch")},
	{EMaterialParameterType::ParameterCollection, TEXT("parameter_collection")},
	{EMaterialParameterType::StaticComponentMask, TEXT("static_component_mask")},
};

const TCHAR* KindName(EMaterialParameterType Type)
{
	for (const FKind& Kind : Kinds)
	{
		if (Kind.Type == Type)
		{
			return Kind.Name;
		}
	}
	return TEXT("unknown");
}

const TCHAR* AssociationName(EMaterialParameterAssociation Association)
{
	switch (Association)
	{
	case EMaterialParameterAssociation::LayerParameter: return TEXT("layer");
	case EMaterialParameterAssociation::BlendParameter: return TEXT("blend");
	default: return TEXT("global");
	}
}

bool ParseAssociation(const FString& Text, EMaterialParameterAssociation& Out)
{
	if (Text == TEXT("global")) { Out = EMaterialParameterAssociation::GlobalParameter; return true; }
	if (Text == TEXT("layer")) { Out = EMaterialParameterAssociation::LayerParameter; return true; }
	if (Text == TEXT("blend")) { Out = EMaterialParameterAssociation::BlendParameter; return true; }
	return false;
}

struct FParameter
{
	FMaterialParameterInfo Info;
	EMaterialParameterType Type = EMaterialParameterType::None;
	FMaterialParameterMetadata Meta;
};

// The parameters the parent chain exposes (what the editor's RegenerateArrays lists), in a stable
// order: association (global, layer, blend), layer index, name, kind.
TArray<FParameter> GetExposedParameters(const UMaterialInstance& Instance)
{
	TArray<FParameter> Parameters;
	TMap<FMaterialParameterInfo, FMaterialParameterMetadata> OfType;
	for (const FKind& Kind : Kinds)
	{
		Instance.GetAllParametersOfType(Kind.Type, OfType);
		for (TPair<FMaterialParameterInfo, FMaterialParameterMetadata>& Pair : OfType)
		{
			FParameter& Parameter = Parameters.AddDefaulted_GetRef();
			Parameter.Info = Pair.Key;
			Parameter.Type = Kind.Type;
			Parameter.Meta = MoveTemp(Pair.Value);
		}
	}
	auto AssociationOrder = [](EMaterialParameterAssociation Association)
	{
		return Association == EMaterialParameterAssociation::GlobalParameter ? 0
			: Association == EMaterialParameterAssociation::LayerParameter ? 1 : 2;
	};
	Parameters.Sort([&AssociationOrder](const FParameter& A, const FParameter& B)
	{
		if (A.Info.Association != B.Info.Association)
		{
			return AssociationOrder(A.Info.Association) < AssociationOrder(B.Info.Association);
		}
		if (A.Info.Index != B.Info.Index)
		{
			return A.Info.Index < B.Info.Index;
		}
		const int32 Names = A.Info.Name.ToString().Compare(B.Info.Name.ToString(), ESearchCase::CaseSensitive);
		return Names != 0 ? Names < 0 : static_cast<uint8>(A.Type) < static_cast<uint8>(B.Type);
	});
	return Parameters;
}

UMaterialInstanceConstant* GetInstance(FCall& Call)
{
	UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(&Call.Asset);
	if (Instance == nullptr)
	{
		Fail(Call, TEXT("Data.UnsupportedAsset"), TEXT("MaterialInstance primitives edit MaterialInstanceConstant assets."), TEXT("/HandleArguments/asset"));
	}
	return Instance;
}

FValue EncodeValue(const FMaterialParameterValue& Value)
{
	auto Path = [](const UObject* Object) { return MakeString(PathOf(Object)); };
	switch (Value.Type)
	{
	case EMaterialParameterType::Scalar:
		return MakeNumber(ShortestFloat(Value.Float[0]));
	case EMaterialParameterType::Vector:
		return MakeArray({MakeNumber(ShortestFloat(Value.Float[0])), MakeNumber(ShortestFloat(Value.Float[1])),
			MakeNumber(ShortestFloat(Value.Float[2])), MakeNumber(ShortestFloat(Value.Float[3]))});
	case EMaterialParameterType::DoubleVector:
		return MakeArray({MakeNumber(Value.Double[0]), MakeNumber(Value.Double[1]), MakeNumber(Value.Double[2]), MakeNumber(Value.Double[3])});
	case EMaterialParameterType::Texture: return Path(Value.Texture);
	case EMaterialParameterType::TextureCollection: return Path(Value.TextureCollection);
	case EMaterialParameterType::RuntimeVirtualTexture: return Path(Value.RuntimeVirtualTexture);
	case EMaterialParameterType::SparseVolumeTexture: return Path(Value.SparseVolumeTexture);
	case EMaterialParameterType::ParameterCollection: return Path(Value.ParameterCollection);
	case EMaterialParameterType::Font:
	{
		FObjectBuilder Font;
		Font.AddString(TEXT("Font"), PathOf(Value.Font.Value));
		Font.AddInt(TEXT("Page"), Value.Font.Page);
		return Font.Build();
	}
	case EMaterialParameterType::StaticSwitch:
		return MakeBool(Value.Bool[0]);
	case EMaterialParameterType::StaticComponentMask:
	{
		FObjectBuilder Mask;
		Mask.AddBool(TEXT("R"), Value.Bool[0]);
		Mask.AddBool(TEXT("G"), Value.Bool[1]);
		Mask.AddBool(TEXT("B"), Value.Bool[2]);
		Mask.AddBool(TEXT("A"), Value.Bool[3]);
		return Mask.Build();
	}
	default:
		return MakeNull();
	}
}

bool ReadNumber(const FValue& Value, double& Out)
{
	if (Value.Kind == EAssetDocumentRuntimeValueKind::Number)
	{
		Out = Value.NumberValue;
		return FMath::IsFinite(Out);
	}
	if (Value.Kind == EAssetDocumentRuntimeValueKind::Integer)
	{
		Out = static_cast<double>(Value.IntegerValue);
		return true;
	}
	return false;
}

bool ReadFour(const FValue& Value, double (&Out)[4])
{
	if (Value.Kind != EAssetDocumentRuntimeValueKind::Array || Value.Elements.Num() != 4)
	{
		return false;
	}
	for (int32 Index = 0; Index < 4; ++Index)
	{
		if (!ReadNumber(Value.Elements[Index], Out[Index]))
		{
			return false;
		}
	}
	return true;
}

// An asset reference without the warnings a failed load logs: "" is None; a path whose package does
// not exist is not found.
bool ResolveAsset(const FString& Path, UClass& Class, UObject*& OutObject, FString& OutProblem)
{
	OutObject = nullptr;
	if (Path.IsEmpty())
	{
		return true;
	}
	const FSoftObjectPath SoftPath(Path);
	if (!SoftPath.IsValid() || !FPackageName::IsValidLongPackageName(SoftPath.GetLongPackageName()))
	{
		OutProblem = FString::Printf(TEXT("'%s' is not an object path such as /Game/Textures/T_Rock.T_Rock."), *Path);
		return false;
	}
	UObject* Object = SoftPath.ResolveObject();
	if (Object == nullptr && FPackageName::DoesPackageExist(SoftPath.GetLongPackageName()))
	{
		Object = SoftPath.TryLoad();
	}
	if (Object == nullptr)
	{
		OutProblem = FString::Printf(TEXT("No asset at '%s'."), *Path);
		return false;
	}
	if (!Object->IsA(&Class))
	{
		OutProblem = FString::Printf(TEXT("'%s' is a %s, not a %s."), *Path, *Object->GetClass()->GetName(), *Class.GetName());
		return false;
	}
	OutObject = Object;
	return true;
}

// Document value -> parameter value of Type. OutCode/OutProblem describe a failure.
bool DecodeValue(EMaterialParameterType Type, const FValue& Value, const FMaterialParameterMetadata& Exposed,
	FMaterialParameterValue& Out, FString& OutCode, FString& OutProblem)
{
	OutCode = TEXT("MaterialInstance.InvalidValue");
	auto Reference = [&](UClass& Class, UObject*& Object) -> bool
	{
		if (Value.Kind != EAssetDocumentRuntimeValueKind::String)
		{
			OutProblem = FString::Printf(TEXT("A %s parameter takes an object path string (\"\" for None)."), KindName(Type));
			return false;
		}
		if (!ResolveAsset(Value.StringValue, Class, Object, OutProblem))
		{
			OutCode = OutProblem.StartsWith(TEXT("No asset")) ? TEXT("MaterialInstance.AssetNotFound") : TEXT("MaterialInstance.InvalidValue");
			return false;
		}
		return true;
	};
	switch (Type)
	{
	case EMaterialParameterType::Scalar:
	{
		double Number = 0;
		if (!ReadNumber(Value, Number))
		{
			OutProblem = TEXT("A scalar parameter takes a number.");
			return false;
		}
		Out = FMaterialParameterValue(static_cast<float>(Number));
		return true;
	}
	case EMaterialParameterType::Vector:
	case EMaterialParameterType::DoubleVector:
	{
		double Four[4] = {0, 0, 0, 0};
		if (!ReadFour(Value, Four))
		{
			OutProblem = Type == EMaterialParameterType::Vector
				? TEXT("A vector parameter takes [R, G, B, A] (four numbers).")
				: TEXT("A double vector parameter takes [X, Y, Z, W] (four numbers).");
			return false;
		}
		Out = Type == EMaterialParameterType::Vector
			? FMaterialParameterValue(FLinearColor(static_cast<float>(Four[0]), static_cast<float>(Four[1]), static_cast<float>(Four[2]), static_cast<float>(Four[3])))
			: FMaterialParameterValue(FVector4d(Four[0], Four[1], Four[2], Four[3]));
		return true;
	}
	case EMaterialParameterType::Texture:
	{
		UObject* Object = nullptr;
		if (!Reference(*UTexture::StaticClass(), Object))
		{
			return false;
		}
		UTexture* Texture = Cast<UTexture>(Object);
		const UTexture* Default = Exposed.Value.Type == EMaterialParameterType::Texture ? Exposed.Value.Texture : nullptr;
		// The editor's picker only offers textures the parameter's sampler can read (2D, cube, volume, virtual...).
		if (Texture != nullptr && Default != nullptr && (Texture->GetMaterialType() & Default->GetMaterialType()) == 0)
		{
			OutProblem = FString::Printf(TEXT("'%s' (%s) does not fit this texture parameter, whose sampler reads textures like %s (%s)."),
				*Texture->GetPathName(), *Texture->GetClass()->GetName(), *Default->GetPathName(), *Default->GetClass()->GetName());
			return false;
		}
		Out = FMaterialParameterValue(Texture);
		return true;
	}
	case EMaterialParameterType::TextureCollection:
	{
		UObject* Object = nullptr;
		if (!Reference(*UTextureCollection::StaticClass(), Object))
		{
			return false;
		}
		Out = FMaterialParameterValue(static_cast<const UTextureCollection*>(Cast<UTextureCollection>(Object)));
		return true;
	}
	case EMaterialParameterType::RuntimeVirtualTexture:
	{
		UObject* Object = nullptr;
		if (!Reference(*URuntimeVirtualTexture::StaticClass(), Object))
		{
			return false;
		}
		Out = FMaterialParameterValue(Cast<URuntimeVirtualTexture>(Object));
		return true;
	}
	case EMaterialParameterType::SparseVolumeTexture:
	{
		UObject* Object = nullptr;
		if (!Reference(*USparseVolumeTexture::StaticClass(), Object))
		{
			return false;
		}
		Out = FMaterialParameterValue(Cast<USparseVolumeTexture>(Object));
		return true;
	}
	case EMaterialParameterType::ParameterCollection:
	{
		UObject* Object = nullptr;
		if (!Reference(*UMaterialParameterCollection::StaticClass(), Object))
		{
			return false;
		}
		Out = FMaterialParameterValue(static_cast<const UMaterialParameterCollection*>(Cast<UMaterialParameterCollection>(Object)));
		return true;
	}
	case EMaterialParameterType::Font:
	{
		const FValue* FontPath = Value.Kind == EAssetDocumentRuntimeValueKind::Object ? FindField(Value, TEXT("Font")) : nullptr;
		const FValue* Page = Value.Kind == EAssetDocumentRuntimeValueKind::Object ? FindField(Value, TEXT("Page")) : nullptr;
		double PageNumber = 0;
		if (FontPath == nullptr || FontPath->Kind != EAssetDocumentRuntimeValueKind::String || Value.ObjectKeys.Num() > 2
			|| (Page != nullptr && (!ReadNumber(*Page, PageNumber) || PageNumber < 0 || PageNumber != FMath::FloorToDouble(PageNumber))))
		{
			OutProblem = TEXT("A font parameter takes {\"Font\": \"<font object path>\", \"Page\": <font page, default 0>}.");
			return false;
		}
		UObject* Object = nullptr;
		if (!ResolveAsset(FontPath->StringValue, *UFont::StaticClass(), Object, OutProblem))
		{
			OutCode = OutProblem.StartsWith(TEXT("No asset")) ? TEXT("MaterialInstance.AssetNotFound") : TEXT("MaterialInstance.InvalidValue");
			return false;
		}
		Out = FMaterialParameterValue(Cast<UFont>(Object), static_cast<int32>(PageNumber));
		return true;
	}
	case EMaterialParameterType::StaticSwitch:
		if (Value.Kind != EAssetDocumentRuntimeValueKind::Boolean)
		{
			OutProblem = TEXT("A static switch parameter takes true or false.");
			return false;
		}
		Out = FMaterialParameterValue(Value.BooleanValue);
		return true;
	case EMaterialParameterType::StaticComponentMask:
	{
		bool Channels[4] = {false, false, false, false};
		const TCHAR* Names[4] = {TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A")};
		bool bValid = Value.Kind == EAssetDocumentRuntimeValueKind::Object && Value.ObjectKeys.Num() == 4;
		for (int32 Index = 0; bValid && Index < 4; ++Index)
		{
			const FValue* Channel = FindField(Value, Names[Index]);
			bValid = Channel != nullptr && Channel->Kind == EAssetDocumentRuntimeValueKind::Boolean;
			Channels[Index] = bValid && Channel->BooleanValue;
		}
		if (!bValid)
		{
			OutProblem = TEXT("A static component mask parameter takes {\"R\": bool, \"G\": bool, \"B\": bool, \"A\": bool}.");
			return false;
		}
		Out = FMaterialParameterValue(Channels[0], Channels[1], Channels[2], Channels[3]);
		return true;
	}
	default:
		OutProblem = FString::Printf(TEXT("%s parameters are not supported."), KindName(Type));
		return false;
	}
}

// The Material Instance Editor's "Refraction Depth Bias" is stored as a scalar override entry named
// RefractionDepthBias that no parent parameter exposes (CopyToSourceInstance writes it on every edit).
const FName RefractionDepthBiasName(TEXT("RefractionDepthBias"));

bool IsRefractionDepthBias(const FMaterialParameterInfo& Info, EMaterialParameterType Type, const TArray<FParameter>& Exposed)
{
	return Type == EMaterialParameterType::Scalar && Info == FMaterialParameterInfo(RefractionDepthBiasName)
		&& !Exposed.ContainsByPredicate([&Info](const FParameter& Parameter) { return Parameter.Info == Info; });
}

const FScalarParameterValue* FindRefractionDepthBias(const UMaterialInstanceConstant& Instance, const TArray<FParameter>& Exposed)
{
	for (const FScalarParameterValue& Entry : Instance.ScalarParameterValues)
	{
		if (IsRefractionDepthBias(Entry.ParameterInfo, EMaterialParameterType::Scalar, Exposed))
		{
			return &Entry;
		}
	}
	return nullptr;
}

FString ParameterKey(const FMaterialParameterInfo& Info)
{
	return Info.Association == EMaterialParameterAssociation::GlobalParameter
		? Info.Name.ToString()
		: FString::Printf(TEXT("%s[%d].%s"), AssociationName(Info.Association), Info.Index, *Info.Name.ToString());
}

// Raw override entries whose parameter the parent chain does not expose (the editor drops them on
// its next edit).
TArray<FParameter> GetStaleOverrides(const UMaterialInstanceConstant& Instance, const TArray<FParameter>& Exposed)
{
	TSet<TPair<FMaterialParameterInfo, uint8>> Known;
	for (const FParameter& Parameter : Exposed)
	{
		Known.Add({Parameter.Info, static_cast<uint8>(Parameter.Type)});
	}
	TArray<FParameter> Stale;
	auto Check = [&](const FMaterialParameterInfo& Info, EMaterialParameterType Type)
	{
		if (!Known.Contains({Info, static_cast<uint8>(Type)}) && !IsRefractionDepthBias(Info, Type, Exposed))
		{
			FParameter& Entry = Stale.AddDefaulted_GetRef();
			Entry.Info = Info;
			Entry.Type = Type;
		}
	};
	for (const FScalarParameterValue& Entry : Instance.ScalarParameterValues) { Check(Entry.ParameterInfo, EMaterialParameterType::Scalar); }
	for (const FVectorParameterValue& Entry : Instance.VectorParameterValues) { Check(Entry.ParameterInfo, EMaterialParameterType::Vector); }
	for (const FDoubleVectorParameterValue& Entry : Instance.DoubleVectorParameterValues) { Check(Entry.ParameterInfo, EMaterialParameterType::DoubleVector); }
	for (const FTextureParameterValue& Entry : Instance.TextureParameterValues) { Check(Entry.ParameterInfo, EMaterialParameterType::Texture); }
	for (const FTextureCollectionParameterValue& Entry : Instance.TextureCollectionParameterValues) { Check(Entry.ParameterInfo, EMaterialParameterType::TextureCollection); }
	for (const FParameterCollectionParameterValue& Entry : Instance.ParameterCollectionParameterValues) { Check(Entry.ParameterInfo, EMaterialParameterType::ParameterCollection); }
	for (const FRuntimeVirtualTextureParameterValue& Entry : Instance.RuntimeVirtualTextureParameterValues) { Check(Entry.ParameterInfo, EMaterialParameterType::RuntimeVirtualTexture); }
	for (const FSparseVolumeTextureParameterValue& Entry : Instance.SparseVolumeTextureParameterValues) { Check(Entry.ParameterInfo, EMaterialParameterType::SparseVolumeTexture); }
	for (const FFontParameterValue& Entry : Instance.FontParameterValues) { Check(Entry.ParameterInfo, EMaterialParameterType::Font); }
	const FStaticParameterSet Static = Instance.GetStaticParameters();
	for (const FStaticSwitchParameter& Entry : Static.StaticSwitchParameters)
	{
		if (Entry.bOverride) { Check(Entry.ParameterInfo, EMaterialParameterType::StaticSwitch); }
	}
	for (const FStaticComponentMaskParameter& Entry : Static.EditorOnly.StaticComponentMaskParameters)
	{
		if (Entry.bOverride) { Check(Entry.ParameterInfo, EMaterialParameterType::StaticComponentMask); }
	}
	return Stale;
}

FValue DescribeKey(const FParameter& Parameter)
{
	FObjectBuilder Row;
	Row.AddString(TEXT("name"), Parameter.Info.Name.ToString());
	Row.AddString(TEXT("kind"), KindName(Parameter.Type));
	Row.AddString(TEXT("association"), AssociationName(Parameter.Info.Association));
	Row.AddInt(TEXT("index"), Parameter.Info.Index);
	return Row.Build();
}

// The asset (material or material function) that declares the parameter, as the editor's "Found in" names it.
FString ParameterAssetPath(const UMaterialInstanceConstant& Instance, const FParameter& Parameter)
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	// Newer engines record per parameter name every asset that declares it, in the order the material found them;
	// the first is the declaration the parameter's value comes from.
	const TSharedPtr<FMaterialCachedExpressionEditorOnlyData>& EditorOnly = Instance.GetCachedExpressionData().EditorOnlyData;
	const FParameterAssetPathsEntry* Entry = EditorOnly.IsValid() ? EditorOnly->AssetPathsMap.Find(Parameter.Info.Name) : nullptr;
	if (Entry == nullptr || Entry->AssetPaths.IsEmpty())
	{
		return FString();
	}
	return Entry->AssetPaths.CreateConstIterator()->Key;
#else
	(void)Instance;
	return Parameter.Meta.AssetPath;
#endif
}

FValue DescribeParameter(const UMaterialInstanceConstant& Instance, const FParameter& Parameter)
{
	FObjectBuilder Row;
	Row.AddString(TEXT("name"), Parameter.Info.Name.ToString());
	Row.AddString(TEXT("kind"), KindName(Parameter.Type));
	Row.AddString(TEXT("association"), AssociationName(Parameter.Info.Association));
	Row.AddInt(TEXT("index"), Parameter.Info.Index);
	Row.AddString(TEXT("group"), Parameter.Meta.Group.IsNone() ? FString() : Parameter.Meta.Group.ToString());
	Row.AddInt(TEXT("sort_priority"), Parameter.Meta.SortPriority);
	Row.AddString(TEXT("description"), Parameter.Meta.Description);
	Row.AddString(TEXT("asset"), ParameterAssetPath(Instance, Parameter));
	Row.AddBool(TEXT("overridden"), Parameter.Meta.bOverride);
	Row.Add(TEXT("value"), EncodeValue(Parameter.Meta.Value));
	// The value the parent chain gives (what unticking the override reverts to).
	FMaterialParameterMetadata Inherited;
	const bool bInherited = Instance.GetParameterValue(Parameter.Type, FMemoryImageMaterialParameterInfo(Parameter.Info), Inherited,
		EMaterialGetParameterValueFlags::CheckNonOverrides);
	Row.Add(TEXT("default"), bInherited ? EncodeValue(Inherited.Value) : MakeNull());
	if (Parameter.Type == EMaterialParameterType::Scalar && Parameter.Meta.ScalarMax > Parameter.Meta.ScalarMin)
	{
		Row.Add(TEXT("ui_min"), MakeNumber(ShortestFloat(Parameter.Meta.ScalarMin)));
		Row.Add(TEXT("ui_max"), MakeNumber(ShortestFloat(Parameter.Meta.ScalarMax)));
	}
	return Row.Build();
}

FValue DescribeLayers(const UMaterialInstanceConstant& Instance)
{
	FMaterialLayersFunctions Layers;
	if (!Instance.GetStaticParameters().bHasMaterialLayers || !Instance.GetMaterialLayers(Layers))
	{
		return MakeNull();
	}
	TArray<FString> LayerPaths;
	TArray<FString> BlendPaths;
	TArray<FString> Names;
	TArray<FValue> Visible;
	for (const UMaterialFunctionInterface* Layer : Layers.Layers) { LayerPaths.Add(PathOf(Layer)); }
	for (const UMaterialFunctionInterface* Blend : Layers.Blends) { BlendPaths.Add(PathOf(Blend)); }
	for (const FText& Name : Layers.EditorOnly.LayerNames) { Names.Add(Name.ToString()); }
	for (const bool bState : Layers.EditorOnly.LayerStates) { Visible.Add(MakeBool(bState)); }
	FObjectBuilder Stack;
	Stack.Add(TEXT("layers"), MakeStringArray(LayerPaths));
	Stack.Add(TEXT("blends"), MakeStringArray(BlendPaths));
	Stack.Add(TEXT("names"), MakeStringArray(Names));
	Stack.Add(TEXT("visible"), MakeArray(MoveTemp(Visible)));
	return Stack.Build();
}

// Parent change as UMaterialEditorInstanceConstant does it (UpdateSourceInstanceParent).
bool ChangeParent(FCall& Call, UMaterialInstanceConstant& Instance, const FString& Path, bool& bOutChanged)
{
	bOutChanged = false;
	UObject* Object = nullptr;
	FString Problem;
	if (!ResolveAsset(Path, *UMaterialInterface::StaticClass(), Object, Problem))
	{
		return Call.Args.Fail(Problem.StartsWith(TEXT("No asset")) ? TEXT("MaterialInstance.ParentNotFound") : TEXT("MaterialInstance.InvalidParent"),
			FString::Printf(TEXT("Parent: %s"), *Problem), TEXT("parent"));
	}
	UMaterialInterface* NewParent = Cast<UMaterialInterface>(Object);
	if (NewParent == Instance.Parent)
	{
		return true;
	}
	if (NewParent != nullptr && !NewParent->IsA<UMaterial>() && !NewParent->IsA<UMaterialInstanceConstant>())
	{
		return Call.Args.Fail(TEXT("MaterialInstance.InvalidParent"), FString::Printf(
			TEXT("'%s' is a %s; a material instance's parent is a Material or a Material Instance Constant."),
			*Path, *NewParent->GetClass()->GetName()), TEXT("parent"));
	}
	const UMaterialInstance* ParentInstance = Cast<UMaterialInstance>(NewParent);
	if (NewParent == &Instance || (ParentInstance != nullptr && ParentInstance->IsChildOf(&Instance)))
	{
		return Call.Args.Fail(TEXT("MaterialInstance.ParentCycle"), FString::Printf(
			TEXT("'%s' is this instance or one of its children; the parent chain would loop."), *Path), TEXT("parent"));
	}
	FMaterialUpdateContext UpdateContext;
	Instance.SetParentEditorOnly(NewParent);
	Instance.PostEditChange();
	UpdateContext.AddMaterialInstance(&Instance);
	bOutChanged = true;
	return true;
}

const FParameter* FindParameter(const TArray<FParameter>& Exposed, const FMaterialParameterInfo& Info, int32& OutMatches)
{
	const FParameter* Found = nullptr;
	OutMatches = 0;
	for (const FParameter& Parameter : Exposed)
	{
		if (Parameter.Info == Info)
		{
			Found = Found == nullptr ? &Parameter : Found;
			++OutMatches;
		}
	}
	return Found;
}

bool ReadParameterInfo(FReader& Op, FMaterialParameterInfo& Out)
{
	FString Name;
	FString Association = TEXT("global");
	int64 Index = INDEX_NONE;
	if (!Op.String(TEXT("name"), Name) || !Op.String(TEXT("association"), Association, false) || !Op.Int(TEXT("index"), Index, false))
	{
		return false;
	}
	EMaterialParameterAssociation ParsedAssociation = EMaterialParameterAssociation::GlobalParameter;
	if (!ParseAssociation(Association, ParsedAssociation))
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("association is global, layer or blend."), TEXT("association"));
	}
	if (Name.IsEmpty())
	{
		return Op.Fail(TEXT("MaterialInstance.UnknownParameter"), TEXT("A parameter name is not empty."), TEXT("name"));
	}
	if ((ParsedAssociation == EMaterialParameterAssociation::GlobalParameter) != (Index == INDEX_NONE))
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("Global parameters have no index; layer and blend parameters need their layer index."), TEXT("index"));
	}
	Out = FMaterialParameterInfo(FName(*Name), ParsedAssociation, static_cast<int32>(Index));
	return true;
}
} // namespace

bool HandleMaterialInstanceRead(FCall& Call, FObjectBuilder& Result)
{
	UMaterialInstanceConstant* Instance = GetInstance(Call);
	int64 Offset = 0;
	int64 Limit = DefaultParameterLimit;
	if (Instance == nullptr || !Call.Args.OnlyFields({TEXT("offset"), TEXT("limit")})
		|| !Call.Args.Int(TEXT("offset"), Offset, false) || !Call.Args.Int(TEXT("limit"), Limit, false))
	{
		return false;
	}
	if (Offset < 0 || Limit < 1)
	{
		return Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("offset must be >= 0 and limit >= 1."), TEXT("offset"));
	}
	const TArray<FParameter> Exposed = GetExposedParameters(*Instance);
	Result.AddString(TEXT("class"), Instance->GetClass()->GetPathName());
	Result.AddString(TEXT("parent"), PathOf(Instance->Parent));
	const UMaterial* Base = Instance->Parent != nullptr ? Instance->GetMaterial() : nullptr;
	Result.AddString(TEXT("base_material"), PathOf(Base));
	Result.AddBool(TEXT("has_static_permutation"), Instance->bHasStaticPermutationResource);
	const FScalarParameterValue* Refraction = FindRefractionDepthBias(*Instance, Exposed);
	Result.Add(TEXT("refraction_depth_bias"), Refraction != nullptr ? MakeNumber(ShortestFloat(Refraction->ParameterValue)) : MakeNull());
	int32 NodeCount = 16;
	if (Offset == 0)
	{
		TArray<FValue> Stale;
		for (const FParameter& Entry : GetStaleOverrides(*Instance, Exposed))
		{
			Stale.Add(DescribeKey(Entry));
		}
		NodeCount += 1 + 5 * Stale.Num();
		Result.Add(TEXT("stale_overrides"), MakeArray(MoveTemp(Stale)));
		FValue Layers = DescribeLayers(*Instance);
		if (Layers.Kind != EAssetDocumentRuntimeValueKind::Null)
		{
			NodeCount += CountValueNodes(Layers);
			Result.Add(TEXT("layers"), MoveTemp(Layers));
		}
	}
	TArray<FValue> Rows;
	const int32 NextOffset = AppendPaged(Rows, NodeCount, [&](int32 Index)
	{
		return DescribeParameter(*Instance, Exposed[Index]);
	}, static_cast<int32>(Offset), Exposed.Num(), static_cast<int32>(Limit));
	Result.AddInt(TEXT("total"), Exposed.Num());
	Result.AddInt(TEXT("next_offset"), NextOffset);
	Result.Add(TEXT("parameters"), MakeArray(MoveTemp(Rows)));
	return true;
}

bool HandleMaterialInstanceEdit(FCall& Call, FObjectBuilder& Result)
{
	UMaterialInstanceConstant* Instance = GetInstance(Call);
	const FValue* Ops = nullptr;
	FString ParentPath;
	if (Instance == nullptr || !Call.Args.OnlyFields({TEXT("parent"), TEXT("ops"), TEXT("refraction_depth_bias")})
		|| !Call.Args.String(TEXT("parent"), ParentPath, false) || !Call.Args.Array(TEXT("ops"), Ops))
	{
		return false;
	}
	FAssetDocumentEditorNonInteractiveScope NonInteractive;
	const FScopedTransaction Transaction(LOCTEXT("EditMaterialInstance", "Edit Material Instance"));
	// The editor's SourceInstance->PreEditChange (records the instance in the transaction).
	Instance->PreEditChange(nullptr);

	bool bParentChanged = false;
	if (Call.Args.Has(TEXT("parent")) && !ChangeParent(Call, *Instance, ParentPath, bParentChanged))
	{
		return false;
	}

	// The editor's parameter list: every exposed parameter with its override flag and value.
	const TArray<FParameter> Exposed = GetExposedParameters(*Instance);
	TArray<FString> Candidates;
	TMap<FMaterialParameterInfo, FParameter> Overrides;
	for (const FParameter& Parameter : Exposed)
	{
		Candidates.AddUnique(ParameterKey(Parameter.Info));
		if (Parameter.Meta.bOverride)
		{
			Overrides.Add(Parameter.Info, Parameter);
		}
	}
	const TMap<FMaterialParameterInfo, FParameter> Before = Overrides;
	const TArray<FParameter> Stale = GetStaleOverrides(*Instance, Exposed);
	// Refraction Depth Bias: kept unless the call sets it (number) or removes it (null: the parent's value).
	const FScalarParameterValue* CurrentRefraction = FindRefractionDepthBias(*Instance, Exposed);
	TOptional<float> Refraction;
	if (CurrentRefraction != nullptr)
	{
		Refraction = CurrentRefraction->ParameterValue;
	}
	const TOptional<float> RefractionBefore = Refraction;
	if (const FValue* RefractionValue = Call.Args.Find(TEXT("refraction_depth_bias")))
	{
		double Number = 0;
		if (RefractionValue->Kind == EAssetDocumentRuntimeValueKind::Null)
		{
			Refraction.Reset();
		}
		else if (ReadNumber(*RefractionValue, Number))
		{
			Refraction = static_cast<float>(Number);
		}
		else
		{
			return Call.Args.Fail(TEXT("MaterialInstance.InvalidValue"), TEXT("RefractionDepthBias takes a number (or null for the parent's value)."),
				TEXT("refraction_depth_bias"));
		}
	}

	for (int32 Index = 0; Index < Ops->Elements.Num(); ++Index)
	{
		FReader Op = Call.Args.Child(FString::Printf(TEXT("ops/%d"), Index), Ops->Elements[Index]);
		FString Kind;
		FMaterialParameterInfo Info;
		if (!Op.IsObject())
		{
			return Op.Fail(TEXT("Data.InvalidOp"), TEXT("Each op is an object."));
		}
		if (!Op.String(TEXT("op"), Kind) || !ReadParameterInfo(Op, Info))
		{
			return false;
		}
		const bool bSet = Kind == TEXT("set");
		if (!bSet && Kind != TEXT("clear"))
		{
			return Op.Fail(TEXT("Data.InvalidOp"), FString::Printf(TEXT("Unknown op '%s' (set, clear)."), *Kind), TEXT("op"));
		}
		if (!(bSet ? Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("association"), TEXT("index"), TEXT("value")})
			: Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("association"), TEXT("index")})))
		{
			return false;
		}
		int32 Matches = 0;
		const FParameter* Parameter = FindParameter(Exposed, Info, Matches);
		if (!bSet)
		{
			// Unticking the override; a parameter that is not exposed (or not overridden) has none.
			Overrides.Remove(Info);
			continue;
		}
		if (Parameter == nullptr)
		{
			// The whole parameter list (capped) so a caller can fix every name in one round.
			const TArray<FString> Suggestions = SuggestActionKeys(ParameterKey(Info), Candidates);
			TArray<FString> Listed;
			for (const FParameter& Exposure : Exposed)
			{
				if (Listed.Num() < MaxListedParameters)
				{
					Listed.Add(FString::Printf(TEXT("%s (%s)"), *ParameterKey(Exposure.Info), KindName(Exposure.Type)));
				}
			}
			FString Message = FString::Printf(TEXT("The parent %s exposes no parameter '%s'."), *PathOf(Instance->Parent), *ParameterKey(Info));
			if (!Suggestions.IsEmpty())
			{
				Message += FString::Printf(TEXT(" Did you mean: %s?"), *FString::Join(Suggestions, TEXT(", ")));
			}
			Message += Exposed.IsEmpty() ? FString(TEXT(" It exposes no parameters."))
				: FString::Printf(TEXT(" Its parameters: %s%s."), *FString::Join(Listed, TEXT(", ")),
					Exposed.Num() > Listed.Num() ? *FString::Printf(TEXT(" and %d more (Inspect the instance for all)"), Exposed.Num() - Listed.Num()) : TEXT(""));
			return Op.Fail(TEXT("MaterialInstance.UnknownParameter"), Message, TEXT("name"));
		}
		if (Matches > 1)
		{
			return Op.Fail(TEXT("MaterialInstance.AmbiguousParameter"), FString::Printf(
				TEXT("The parent exposes several parameters named '%s' with different types; the editor shows each, but a document key cannot tell them apart."),
				*ParameterKey(Info)), TEXT("name"));
		}
		const FValue* Value = Op.Find(TEXT("value"));
		if (Value == nullptr)
		{
			return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("A set op has a value."), TEXT("value"));
		}
		FMaterialParameterValue Decoded;
		FString Code;
		FString Problem;
		if (!DecodeValue(Parameter->Type, *Value, Parameter->Meta, Decoded, Code, Problem))
		{
			return Op.Fail(Code, FString::Printf(TEXT("%s is a %s parameter: %s"), *ParameterKey(Info), KindName(Parameter->Type), *Problem), TEXT("value"));
		}
		FParameter Override = *Parameter;
		Override.Meta.Value = Decoded;
		Override.Meta.bOverride = true;
		Overrides.Add(Info, MoveTemp(Override));
	}

	bool bOverridesChanged = Overrides.Num() != Before.Num() || !Stale.IsEmpty() || !(Refraction == RefractionBefore);
	for (const TPair<FMaterialParameterInfo, FParameter>& Pair : Overrides)
	{
		const FParameter* Previous = Before.Find(Pair.Key);
		bOverridesChanged |= Previous == nullptr || Previous->Meta.Value != Pair.Value.Meta.Value;
	}
	if (bOverridesChanged || bParentChanged)
	{
		// CopyToSourceInstance: rewrite every override; the context's destructor runs UpdateStaticPermutation.
		Instance->MarkPackageDirty();
		{
			FMaterialInstanceParameterUpdateContext UpdateContext(Instance, EMaterialInstanceClearParameterFlag::All);
			for (const FParameter& Parameter : Exposed)
			{
				if (const FParameter* Override = Overrides.Find(Parameter.Info))
				{
					UpdateContext.SetParameterValueEditorOnly(Override->Info, Override->Meta, EMaterialSetParameterValueFlags::SetCurveAtlas);
				}
			}
			if (Refraction.IsSet())
			{
				UpdateContext.SetParameterValueEditorOnly(FMaterialParameterInfo(RefractionDepthBiasName), FMaterialParameterMetadata(FMaterialParameterValue(Refraction.GetValue())));
			}
			FMaterialLayersFunctions Layers;
			if (Instance->GetMaterialLayers(Layers))
			{
				UpdateContext.SetMaterialLayers(Layers);
			}
		}
		Instance->PostEditChange();
	}
	if (NonInteractive.WasInteractionRefused())
	{
		NonInteractive.DiscardCreatedPackages();
		return Call.Args.Fail(TEXT("MaterialInstance.InteractiveEditRefused"), TEXT(
			"Editing the material instance tried to open an editor dialog, which an MCP request cannot answer; the edit was refused."));
	}
	TArray<FString> Dropped;
	for (const FParameter& Entry : Stale)
	{
		Dropped.Add(FString::Printf(TEXT("%s (%s)"), *ParameterKey(Entry.Info), KindName(Entry.Type)));
	}
	Result.AddInt(TEXT("overrides"), Overrides.Num());
	Result.Add(TEXT("dropped"), MakeStringArray(bOverridesChanged || bParentChanged ? Dropped : TArray<FString>()));
	Result.AddString(TEXT("parent"), PathOf(Instance->Parent));
	Result.AddBool(TEXT("has_static_permutation"), Instance->bHasStaticPermutationResource);
	return true;
}
} // namespace UE::AssetDocument::Graph::Data

#undef LOCTEXT_NAMESPACE
