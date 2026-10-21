// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AnimBlueprint/AssetDocumentAnimBlendSpaceGraphRawEdits.h"

#include "AnimBlueprint/AssetDocumentAnimBlueprintAdapters.h"
#include "AnimAssets/AssetDocumentBlendSpaceEdits.h"
#include "AnimGraphNode_BlendSpaceGraphBase.h"
#include "AnimationBlendSpaceSampleGraph.h"
#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "BlendSpaceGraph.h"
#include "EdGraph/EdGraph.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::AnimBlueprint::BlendSpaceRawEdits
{
namespace BlendSpaceDetail
{
const FString Prefix = TEXT("BlendSpace.");
const TCHAR* AxisNames[] = {TEXT("X"), TEXT("Y")};

// UBlendSpace members are protected: reached by reflection, by name.
const FName BlendParametersName(TEXT("BlendParameters"));
const FName InterpolationParamName(TEXT("InterpolationParam"));
const FName AxisToScaleAnimationName(TEXT("AxisToScaleAnimation"));

// Static-array per-axis settings (FBlendSpaceDetails' Axis Settings groups).
bool IsAxisProperty(const FProperty& Property)
{
	return Property.GetFName() == BlendParametersName || Property.GetFName() == InterpolationParamName;
}

bool IsEditable(const FProperty& Property)
{
	return Property.HasAnyPropertyFlags(CPF_Edit)
		&& !Property.HasAnyPropertyFlags(CPF_EditConst | CPF_Transient | CPF_Deprecated | CPF_DisableEditOnInstance);
}

int32 UsedAxes(const UBlendSpace& BlendSpace)
{
	return BlendSpace.IsA<UBlendSpace1D>() ? 1 : 2;
}

// The non-axis settings FBlendSpaceDetails shows for an internal blend space: Input Interpolation
// and Sample Smoothing (Animation, Notifies, Additive, Analysis, MetaData... are hidden; a 1D
// blend space also hides AxisToScaleAnimation and PreferredTriangulationDirection).
bool IsReportedProperty(const UBlendSpace& BlendSpace, const FProperty& Property)
{
	if (!IsEditable(Property) || IsAxisProperty(Property) || Property.ArrayDim != 1)
	{
		return false;
	}
	const FString& Category = Property.GetMetaData(TEXT("Category"));
	if (Category != TEXT("InputInterpolation") && Category != TEXT("SampleSmoothing"))
	{
		return false;
	}
	return !(BlendSpace.IsA<UBlendSpace1D>()
		&& (Property.GetFName() == AxisToScaleAnimationName
			|| Property.GetFName() == GET_MEMBER_NAME_CHECKED(UBlendSpace, PreferredTriangulationDirection)));
}

FArrayProperty* GraphsProperty()
{
	FArrayProperty* Property = FindFProperty<FArrayProperty>(UAnimGraphNode_BlendSpaceGraphBase::StaticClass(), TEXT("Graphs"));
	const FObjectPropertyBase* Inner = Property != nullptr ? CastField<FObjectPropertyBase>(Property->Inner) : nullptr;
	return Inner != nullptr && Inner->PropertyClass->IsChildOf(UEdGraph::StaticClass()) ? Property : nullptr;
}

struct FWantedSample
{
	FString Name;
	FVector Value = FVector::ZeroVector;
};

bool Fail(FAssetDocumentGraphError& OutError, const FString& Message)
{
	OutError.Set(TEXT("Graph.InvalidArgument"), Message, TEXT("/value"));
	return false;
}

bool ReadSamples(const UBlendSpace& BlendSpace, const FValue& Value, TArray<FWantedSample>& Out, FAssetDocumentGraphError& OutError)
{
	const bool b1D = UsedAxes(BlendSpace) == 1;
	const FString Shape = b1D ? TEXT("{\"Name\", \"X\"}") : TEXT("{\"Name\", \"X\", \"Y\"}");
	if (Value.Kind != EAssetDocumentRuntimeValueKind::Array)
	{
		return Fail(OutError, FString::Printf(TEXT("Samples is an array of %s."), *Shape));
	}
	for (int32 Index = 0; Index < Value.Elements.Num(); ++Index)
	{
		const FValue& Row = Value.Elements[Index];
		const FValue* Name = FindValueField(Row, TEXT("Name"));
		const FValue* X = FindValueField(Row, TEXT("X"));
		const FValue* Y = FindValueField(Row, TEXT("Y"));
		FWantedSample Sample;
		double Number = 0.0;
		const bool bKnown = Row.Kind == EAssetDocumentRuntimeValueKind::Object && !Row.ObjectKeys.ContainsByPredicate(
			[b1D](const FString& Key) { return Key != TEXT("Name") && Key != TEXT("X") && (b1D || Key != TEXT("Y")); });
		if (!bKnown || Name == nullptr || Name->Kind != EAssetDocumentRuntimeValueKind::String || X == nullptr || !ReadNumber(*X, Number))
		{
			return Fail(OutError, FString::Printf(TEXT("Samples[%d] must be %s%s."), Index, *Shape,
				b1D ? TEXT(" (a 1D blend space has no Y)") : TEXT("")));
		}
		Sample.Name = Name->StringValue;
		Sample.Value.X = Number;
		if (Y != nullptr)
		{
			if (!ReadNumber(*Y, Number))
			{
				return Fail(OutError, FString::Printf(TEXT("Samples[%d].Y must be a number."), Index));
			}
			Sample.Value.Y = Number;
		}
		FText Reason;
		if (Sample.Name.IsEmpty() || !FName::IsValidXName(Sample.Name, INVALID_OBJECTNAME_CHARACTERS, &Reason))
		{
			return Fail(OutError, FString::Printf(TEXT("Samples[%d].Name '%s' is not a valid sample graph name."), Index, *Sample.Name));
		}
		if (Out.ContainsByPredicate([&Sample](const FWantedSample& Other) { return Other.Name.Equals(Sample.Name, ESearchCase::IgnoreCase); }))
		{
			return Fail(OutError, FString::Printf(TEXT("Samples: '%s' is listed twice (sample graph names are unique)."), *Sample.Name));
		}
		Out.Add(MoveTemp(Sample));
	}
	return true;
}

int32 FindSample(const TArray<UEdGraph*>& Graphs, const FString& Name)
{
	return Graphs.IndexOfByPredicate([&Name](const UEdGraph* Graph) { return Graph != nullptr && Graph->GetName().Equals(Name, ESearchCase::IgnoreCase); });
}

FString Describe(const FVector& Value, bool b1D)
{
	return b1D ? FString::Printf(TEXT("X=%g"), Value.X) : FString::Printf(TEXT("X=%g, Y=%g"), Value.X, Value.Y);
}
} // namespace BlendSpaceDetail

UBlendSpace* GetBlendSpace(const UAnimGraphNode_BlendSpaceGraphBase& Node)
{
	const UBlendSpaceGraph* Graph = Node.GetBlendSpaceGraph();
	return Graph != nullptr ? Graph->BlendSpace.Get() : nullptr;
}

TArray<UEdGraph*> GetSampleGraphs(const UAnimGraphNode_BlendSpaceGraphBase& Node)
{
	return TArray<UEdGraph*>(Node.GetGraphs());
}

void QueryBlendSpace(const UAnimGraphNode_BlendSpaceGraphBase& Node, FAssetDocumentRuntimeValueDto& Out)
{
	using namespace BlendSpaceDetail;
	const UBlendSpace* BlendSpace = GetBlendSpace(Node);
	if (BlendSpace == nullptr)
	{
		return;
	}
	const UObject* Defaults = BlendSpace->GetClass()->GetDefaultObject();
	const int32 Axes = UsedAxes(*BlendSpace);
	for (TFieldIterator<FProperty> It(BlendSpace->GetClass()); It; ++It)
	{
		const FProperty& Property = **It;
		if (IsAxisProperty(Property))
		{
			const FStructProperty* Struct = CastField<FStructProperty>(&Property);
			for (int32 Axis = 0; Struct != nullptr && Axis < Axes && Axis < Property.ArrayDim; ++Axis)
			{
				const void* Value = Property.ContainerPtrToValuePtr<void>(BlendSpace, Axis);
				const void* Default = Property.ContainerPtrToValuePtr<void>(Defaults, Axis);
				for (TFieldIterator<FProperty> Field(Struct->Struct); Field; ++Field)
				{
					if (!IsEditable(**Field))
					{
						continue;
					}
					const void* FieldValue = Field->ContainerPtrToValuePtr<void>(Value);
					if (!Field->Identical(FieldValue, Field->ContainerPtrToValuePtr<void>(Default), PPF_None))
					{
						SetField(Out, FString::Printf(TEXT("%s%s.%s.%s"), *Prefix, *Property.GetName(), AxisNames[Axis], *Field->GetName()),
							EncodeAssetDocumentGraphPropertyValue(**Field, FieldValue));
					}
				}
			}
			continue;
		}
		if (!IsReportedProperty(*BlendSpace, Property))
		{
			continue;
		}
		const void* Value = Property.ContainerPtrToValuePtr<void>(BlendSpace);
		if (!Property.Identical(Value, Property.ContainerPtrToValuePtr<void>(Defaults), PPF_None))
		{
			SetField(Out, Prefix + Property.GetName(), EncodeAssetDocumentGraphPropertyValue(Property, Value));
		}
	}
	const TArray<UEdGraph*> Graphs = GetSampleGraphs(Node);
	TArray<FValue> Samples;
	for (int32 Index = 0; Index < Graphs.Num() && Index < BlendSpace->GetNumberOfBlendSamples(); ++Index)
	{
		const FVector& Point = BlendSpace->GetBlendSample(Index).SampleValue;
		FValue Sample = MakeObjectValue();
		SetField(Sample, TEXT("Name"), MakeStringValue(Graphs[Index] != nullptr ? Graphs[Index]->GetName() : FString()));
		SetField(Sample, TEXT("X"), MakeNumberValue(Point.X));
		if (Axes == 2)
		{
			SetField(Sample, TEXT("Y"), MakeNumberValue(Point.Y));
		}
		Samples.Add(MoveTemp(Sample));
	}
	SetField(Out, TEXT("Samples"), MakeArrayValue(MoveTemp(Samples)));
}

bool SetBlendSpaceSetting(UAnimGraphNode_BlendSpaceGraphBase& Node, FStringView Path, const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentGraphError& OutError)
{
	using namespace BlendSpaceDetail;
	UBlendSpace* BlendSpace = GetBlendSpace(Node);
	UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForNode(&Node);
	if (BlendSpace == nullptr || Blueprint == nullptr)
	{
		OutError.Set(TEXT("Graph.GraphOperationUnsupported"), TEXT("This Blend Space Graph node has no internal blend space."), TEXT("/property_path"));
		return false;
	}
	TArray<FString> Parts;
	FString(Path).RightChop(Prefix.Len()).ParseIntoArray(Parts, TEXT("."), false);
	FProperty* Top = Parts.IsEmpty() ? nullptr : FindFProperty<FProperty>(BlendSpace->GetClass(), FName(Parts[0]));
	FProperty* Leaf = nullptr;
	void* LeafPtr = nullptr;
	const int32 Axes = UsedAxes(*BlendSpace);
	if (Top != nullptr && IsAxisProperty(*Top) && Parts.Num() == 3)
	{
		const int32 Axis = Parts[1] == AxisNames[0] ? 0 : (Parts[1] == AxisNames[1] ? 1 : INDEX_NONE);
		const FStructProperty* Struct = CastField<FStructProperty>(Top);
		Leaf = Struct != nullptr ? Struct->Struct->FindPropertyByName(FName(Parts[2])) : nullptr;
		if (Axis == INDEX_NONE || Axis >= Axes || Leaf == nullptr || !IsEditable(*Leaf))
		{
			Leaf = nullptr;
		}
		else
		{
			LeafPtr = Leaf->ContainerPtrToValuePtr<void>(Top->ContainerPtrToValuePtr<void>(BlendSpace, Axis));
		}
	}
	else if (Top != nullptr && Parts.Num() == 1 && IsReportedProperty(*BlendSpace, *Top))
	{
		Leaf = Top;
		LeafPtr = Top->ContainerPtrToValuePtr<void>(BlendSpace);
	}
	if (Leaf == nullptr)
	{
		OutError.Set(TEXT("Graph.InvalidArgument"), FString::Printf(
			TEXT("'%.*s' is not a blend space setting of %s. Settings: BlendSpace.<Input Interpolation / Sample Smoothing property>, ")
			TEXT("BlendSpace.BlendParameters.<%s>.<DisplayName|Min|Max|GridNum|bSnapToGrid|bWrapInput>, ")
			TEXT("BlendSpace.InterpolationParam.<%s>.<InterpolationTime|InterpolationType|DampingRatio|MaxSpeed>."),
			Path.Len(), Path.GetData(), *BlendSpace->GetClass()->GetName(), Axes == 1 ? TEXT("X") : TEXT("X|Y"),
			Axes == 1 ? TEXT("X") : TEXT("X|Y")), TEXT("/property_path"));
		return false;
	}
	// A details-panel edit of the blend space (FBlendSpaceDetails) followed by SBlendSpaceEditor::NotifyPostChange
	// (one implementation, shared with the standalone Blend Space asset primitives).
	FString Error;
	const bool bDecoded = UE::AssetDocument::Animation::BlendSpaceEdits::WriteDetailsValue(*BlendSpace, *Top, *Leaf, LeafPtr, Value, Error);
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	if (!bDecoded)
	{
		OutError.Set(TEXT("Graph.InvalidArgument"), Error, TEXT("/value"));
		return false;
	}
	return true;
}

bool RemoveSampleGraph(UAnimGraphNode_BlendSpaceGraphBase& Node, int32 Index)
{
	FArrayProperty* Property = BlendSpaceDetail::GraphsProperty();
	UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForNode(&Node);
	if (Property == nullptr || Blueprint == nullptr)
	{
		return false;
	}
	TArray<TObjectPtr<UEdGraph>>& Graphs = *Property->ContainerPtrToValuePtr<TArray<TObjectPtr<UEdGraph>>>(&Node);
	if (!Graphs.IsValidIndex(Index))
	{
		return false;
	}
	UEdGraph* Graph = Graphs[Index];
	// UAnimGraphNode_BlendSpaceGraphBase::RemoveGraph without closing the (absent) document tab.
	Node.Modify();
	Graphs.RemoveAtSwap(Index);
	if (Graph != nullptr)
	{
		FBlueprintEditorUtils::RemoveGraph(Blueprint, Graph);
	}
	return true;
}

bool SetSamples(UAnimGraphNode_BlendSpaceGraphBase& Node, const FAssetDocumentRuntimeValueDto& Value, FAssetDocumentGraphError& OutError)
{
	using namespace BlendSpaceDetail;
	UBlendSpace* BlendSpace = GetBlendSpace(Node);
	UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForNode(&Node);
	if (BlendSpace == nullptr || Blueprint == nullptr || GraphsProperty() == nullptr)
	{
		OutError.Set(TEXT("Graph.GraphOperationUnsupported"), TEXT("This Blend Space Graph node has no internal blend space."), TEXT("/value"));
		return false;
	}
	TArray<FWantedSample> Wanted;
	if (!ReadSamples(*BlendSpace, Value, Wanted, OutError))
	{
		return false;
	}
	const bool b1D = UsedAxes(*BlendSpace) == 1;
	TArray<UEdGraph*> Graphs = GetSampleGraphs(Node);
	if (Graphs.Num() != BlendSpace->GetNumberOfBlendSamples())
	{
		OutError.Set(TEXT("Graph.GraphOperationUnsupported"), FString::Printf(
			TEXT("The blend space has %d samples but the node has %d sample graphs."), BlendSpace->GetNumberOfBlendSamples(), Graphs.Num()), TEXT("/value"));
		return false;
	}
	const bool bUnchanged = Graphs.Num() == Wanted.Num() && !Wanted.ContainsByPredicate([&](const FWantedSample& Sample)
	{
		const int32 Index = FindSample(Graphs, Sample.Name);
		return Index == INDEX_NONE || BlendSpace->GetBlendSample(Index).SampleValue != Sample.Value;
	});
	if (bUnchanged)
	{
		return true;
	}
	Node.Modify();
	BlendSpace->Modify();
	// OnSampleRemoved: UBlendSpace::DeleteSample + the sample graph (both RemoveAtSwap).
	for (;;)
	{
		const int32 Index = Graphs.IndexOfByPredicate([&Wanted](const UEdGraph* Graph)
		{
			return Graph == nullptr || !Wanted.ContainsByPredicate([Graph](const FWantedSample& Sample) { return Graph->GetName().Equals(Sample.Name, ESearchCase::IgnoreCase); });
		});
		if (Index == INDEX_NONE)
		{
			break;
		}
		if (!BlendSpace->DeleteSample(Index) || !RemoveSampleGraph(Node, Index))
		{
			OutError.Set(TEXT("Graph.GraphOperationUnsupported"), FString::Printf(TEXT("Sample %d could not be removed."), Index), TEXT("/value"));
			return false;
		}
		Graphs = GetSampleGraphs(Node);
	}
	// OnSampleMoved: UBlendSpace::EditSampleValue (grows the axis range like the editor, refuses a point on another sample).
	for (const FWantedSample& Sample : Wanted)
	{
		const int32 Index = FindSample(Graphs, Sample.Name);
		if (Index != INDEX_NONE && BlendSpace->GetBlendSample(Index).SampleValue != Sample.Value
			&& !BlendSpace->EditSampleValue(Index, Sample.Value))
		{
			return Fail(OutError, FString::Printf(TEXT("Sample '%s' cannot move to %s (outside the axis range or on another sample)."),
				*Sample.Name, *Describe(Sample.Value, b1D)));
		}
	}
	// OnSampleAdded: UBlendSpace::AddSample + AddGraph (an empty sample graph named after the sample).
	for (const FWantedSample& Sample : Wanted)
	{
		if (FindSample(Graphs, Sample.Name) != INDEX_NONE)
		{
			continue;
		}
		const int32 Index = BlendSpace->AddSample(Sample.Value);
		if (Index == INDEX_NONE)
		{
			return Fail(OutError, FString::Printf(TEXT("Sample '%s' at %s cannot be added (outside the axis range or on another sample)."),
				*Sample.Name, *Describe(Sample.Value, b1D)));
		}
		UAnimationBlendSpaceSampleGraph* Graph = Node.AddGraph(FName(*Sample.Name), nullptr);
		Graphs = GetSampleGraphs(Node);
		if (Graph == nullptr || Graph->GetName() != Sample.Name || Node.GetSampleIndex(Graph) != Index)
		{
			return Fail(OutError, FString::Printf(TEXT("Sample graph '%s' could not be created under that name."), *Sample.Name));
		}
	}
	BlendSpace->ResampleData();
	BlendSpace->ValidateSampleData();
	BlendSpace->PostEditChange();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	return true;
}
} // namespace UE::AssetDocument::AnimBlueprint::BlendSpaceRawEdits
