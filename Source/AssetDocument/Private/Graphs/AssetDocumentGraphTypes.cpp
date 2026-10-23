// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Graphs/AssetDocumentGraphTypes.h"

#include "AssetDocumentJsonRegionUtils.h"
#include "AssetDocumentProfile.h"

namespace
{
void SetOptionalString(TSharedRef<FJsonObject> Object, const TCHAR* Field, const FString& Value)
{
	if (!Value.IsEmpty())
	{
		Object->SetStringField(Field, Value);
	}
}
}

FString FAssetDocumentGraphEndpoint::ToKey() const
{
	return FString::Printf(TEXT("%s.%s"), *Node, *Pin);
}

TSharedRef<FJsonObject> FAssetDocumentGraphEndpoint::ToJsonObject() const
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("Node"), Node);
	Object->SetStringField(TEXT("Pin"), Pin);
	return Object;
}

FString FAssetDocumentLinkSpec::ToKey() const
{
	return FString::Printf(TEXT("%s->%s"), *From.ToKey(), *To.ToKey());
}

TSharedRef<FJsonObject> FAssetDocumentLinkSpec::ToJsonObject() const
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetObjectField(TEXT("From"), From.ToJsonObject());
	Object->SetObjectField(TEXT("To"), To.ToJsonObject());
	return Object;
}

TSharedRef<FJsonObject> FAssetDocumentMemberRef::ToJsonObject() const
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("Kind"), Kind);
	SetOptionalString(Object, TEXT("OwnerClass"), OwnerClass);
	SetOptionalString(Object, TEXT("Name"), Name);
	SetOptionalString(Object, TEXT("Guid"), Guid);
	if (bHasSelfContext)
	{
		Object->SetBoolField(TEXT("SelfContext"), bSelfContext);
	}
	return Object;
}

FString FAssetDocumentPinOverrideSpec::ToSemanticKey() const
{
	return Direction.IsEmpty() ? Pin : Direction + TEXT(":") + Pin;
}

TSharedRef<FJsonObject> FAssetDocumentPinOverrideSpec::ToJsonObject() const
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("Pin"), Pin);
	SetOptionalString(Object, TEXT("Direction"), Direction);
	SetOptionalString(Object, TEXT("PersistentGuid"), PersistentGuid);
	SetOptionalString(Object, TEXT("SplitState"), SplitState);
	SetOptionalString(Object, TEXT("ParentPin"), ParentPin);
	if (bHasSubPins)
	{
		TArray<TSharedPtr<FJsonValue>> SubPinValues;
		SubPinValues.Reserve(SubPins.Num());
		for (const FString& SubPin : SubPins)
		{
			SubPinValues.Add(MakeShared<FJsonValueString>(SubPin));
		}
		Object->SetArrayField(TEXT("SubPins"), MoveTemp(SubPinValues));
	}
	if (Type.IsValid())
	{
		Object->SetField(TEXT("Type"), FAssetDocumentJsonRegionUtils::DeepCloneJsonValue(Type));
	}
	if (DefaultValue.IsValid())
	{
		Object->SetField(TEXT("DefaultValue"), FAssetDocumentJsonRegionUtils::DeepCloneJsonValue(DefaultValue));
	}
	if (DefaultObject.IsValid())
	{
		Object->SetField(TEXT("DefaultObject"), FAssetDocumentJsonRegionUtils::DeepCloneJsonValue(DefaultObject));
	}
	if (DefaultTextValue.IsValid())
	{
		Object->SetField(TEXT("DefaultTextValue"), FAssetDocumentJsonRegionUtils::DeepCloneJsonValue(DefaultTextValue));
	}
	if (Hidden.IsSet())
	{
		Object->SetBoolField(TEXT("Hidden"), Hidden.GetValue());
	}
	if (AdvancedView.IsSet())
	{
		Object->SetBoolField(TEXT("AdvancedView"), AdvancedView.GetValue());
	}
	return Object;
}

TSharedRef<FJsonObject> FAssetDocumentNodeSpec::ToJsonObject() const
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("Id"), Id);
	SetOptionalString(Object, TEXT("NodeGuid"), NodeGuid);
	SetOptionalString(Object, TEXT("Class"), Class);
	SetOptionalString(Object, TEXT("Capability"), Capability);
	SetOptionalString(Object, TEXT("Kind"), Kind);
	if (Spawner.IsValid())
	{
		Object->SetObjectField(TEXT("Spawner"), FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(Spawner));
	}
	if (Fields.IsValid())
	{
		Object->SetObjectField(TEXT("Fields"), FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(Fields));
	}
	if (Pins.IsValid())
	{
		Object->SetObjectField(TEXT("Pins"), FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(Pins));
	}
	if (Member.IsValid())
	{
		Object->SetObjectField(TEXT("Member"), FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(Member));
	}
	if (!PinOverrides.IsEmpty())
	{
		TArray<FAssetDocumentPinOverrideSpec> SortedPinOverrides = PinOverrides;
		SortedPinOverrides.Sort([](const FAssetDocumentPinOverrideSpec& Left, const FAssetDocumentPinOverrideSpec& Right)
		{
			return Left.ToSemanticKey() < Right.ToSemanticKey();
		});

		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FAssetDocumentPinOverrideSpec& PinOverride : SortedPinOverrides)
		{
			Values.Add(MakeShared<FJsonValueObject>(PinOverride.ToJsonObject()));
		}
		Object->SetArrayField(TEXT("PinOverrides"), MoveTemp(Values));
	}
	if (Position.IsValid())
	{
		Object->SetObjectField(TEXT("Position"), FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(Position));
	}
	if (SubgraphRefs.IsValid())
	{
		Object->SetObjectField(TEXT("SubgraphRefs"), FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(SubgraphRefs));
	}
	if (Evidence.IsValid())
	{
		Object->SetObjectField(TEXT("Evidence"), FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(Evidence));
	}
	if (bHasComment)
	{
		Object->SetStringField(TEXT("Comment"), Comment);
	}
	if (CommentBubblePinned.IsSet())
	{
		Object->SetBoolField(TEXT("CommentBubblePinned"), CommentBubblePinned.GetValue());
	}
	if (CommentBubbleVisible.IsSet())
	{
		Object->SetBoolField(TEXT("CommentBubbleVisible"), CommentBubbleVisible.GetValue());
	}
	SetOptionalString(Object, TEXT("AdvancedPinDisplay"), AdvancedPinDisplay);
	SetOptionalString(Object, TEXT("EnabledState"), EnabledState);
	return Object;
}

TSharedRef<FJsonObject> FAssetDocumentGraphSpec::ToJsonObject() const
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	SetOptionalString(Object, TEXT("Id"), Id);
	SetOptionalString(Object, TEXT("Kind"), Kind);
	if (Owner.IsValid())
	{
		Object->SetObjectField(TEXT("Owner"), FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(Owner));
	}
	else if (!Id.IsEmpty() || !Kind.IsEmpty())
	{
		Object->SetField(TEXT("Owner"), MakeShared<FJsonValueNull>());
	}
	SetOptionalString(Object, TEXT("OwnerNodeId"), OwnerNodeId);
	SetOptionalString(Object, TEXT("OwnerPin"), OwnerPin);
	SetOptionalString(Object, TEXT("Name"), Name);
	SetOptionalString(Object, TEXT("Schema"), Schema);
	SetOptionalString(Object, TEXT("GraphGuid"), GraphGuid);
	SetOptionalString(Object, TEXT("Category"), Category);
	SetOptionalString(Object, TEXT("Description"), Description);
	if (Signature.IsValid())
	{
		Object->SetObjectField(TEXT("Signature"), FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(Signature));
	}
	if (EntryPins.IsValid())
	{
		Object->SetField(TEXT("EntryPins"), FAssetDocumentJsonRegionUtils::DeepCloneJsonValue(EntryPins));
	}
	if (ResultPins.IsValid())
	{
		Object->SetField(TEXT("ResultPins"), FAssetDocumentJsonRegionUtils::DeepCloneJsonValue(ResultPins));
	}
	if (Position.IsValid())
	{
		Object->SetObjectField(TEXT("Position"), FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(Position));
	}
	if (Metadata.IsValid())
	{
		Object->SetField(TEXT("Metadata"), FAssetDocumentJsonRegionUtils::DeepCloneJsonValue(Metadata));
	}
	if (Evidence.IsValid())
	{
		Object->SetObjectField(TEXT("Evidence"), FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(Evidence));
	}

	TArray<FAssetDocumentNodeSpec> SortedNodes = Nodes;
	SortedNodes.Sort([](const FAssetDocumentNodeSpec& Left, const FAssetDocumentNodeSpec& Right)
	{
		return Left.Id < Right.Id;
	});

	TArray<TSharedPtr<FJsonValue>> NodeValues;
	for (const FAssetDocumentNodeSpec& Node : SortedNodes)
	{
		NodeValues.Add(MakeShared<FJsonValueObject>(Node.ToJsonObject()));
	}
	Object->SetArrayField(TEXT("Nodes"), MoveTemp(NodeValues));

	TArray<FAssetDocumentLinkSpec> SortedLinks = Links;
	SortedLinks.Sort([](const FAssetDocumentLinkSpec& Left, const FAssetDocumentLinkSpec& Right)
	{
		return Left.ToKey() < Right.ToKey();
	});

	TArray<TSharedPtr<FJsonValue>> LinkValues;
	for (const FAssetDocumentLinkSpec& Link : SortedLinks)
	{
		LinkValues.Add(MakeShared<FJsonValueObject>(Link.ToJsonObject()));
	}
	Object->SetArrayField(TEXT("Links"), MoveTemp(LinkValues));

	if (!Subgraphs.IsEmpty() || !Id.IsEmpty() || !Kind.IsEmpty())
	{
		TArray<FAssetDocumentGraphSpec> SortedSubgraphs = Subgraphs;
		SortedSubgraphs.Sort([](const FAssetDocumentGraphSpec& Left, const FAssetDocumentGraphSpec& Right)
		{
			return Left.Id < Right.Id;
		});

		TArray<TSharedPtr<FJsonValue>> SubgraphValues;
		for (const FAssetDocumentGraphSpec& Subgraph : SortedSubgraphs)
		{
			SubgraphValues.Add(MakeShared<FJsonValueObject>(Subgraph.ToJsonObject()));
		}
		Object->SetArrayField(TEXT("Subgraphs"), MoveTemp(SubgraphValues));
	}

	return Object;
}

void FAssetDocumentGraphParseResult::AddDiagnostic(const FString& Code, const FString& Path, const FString& Message)
{
	FAssetDocumentGraphDiagnostic Diagnostic;
	Diagnostic.Code = Code;
	Diagnostic.Path = Path;
	Diagnostic.Message = Message;
	Diagnostics.Add(MoveTemp(Diagnostic));
}

FAssetDocumentCapabilityResult FAssetDocumentGraphParseResult::ToCapabilityResult() const
{
	if (Diagnostics.IsEmpty())
	{
		return FAssetDocumentCapabilityResult::Success();
	}

	FAssetDocumentCapabilityResult Result;
	Result.bSuccess = false;
	Result.Message = Diagnostics[0].Message;
	for (const FAssetDocumentGraphDiagnostic& GraphDiagnostic : Diagnostics)
	{
		FAssetDocumentDiagnostic Diagnostic;
		Diagnostic.Path = GraphDiagnostic.Path;
		Diagnostic.Code = GraphDiagnostic.Code;
		Diagnostic.Message = GraphDiagnostic.Message;
		Result.Diagnostics.Add(MoveTemp(Diagnostic));
	}
	return Result;
}

namespace AssetDocumentGraphJson
{
bool AreJsonValuesEqual(const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right)
{
	if (!Left.IsValid() || !Right.IsValid())
	{
		return !Left.IsValid() && !Right.IsValid();
	}
	if (Left->Type != Right->Type)
	{
		return false;
	}

	switch (Left->Type)
	{
	case EJson::Object:
		return AreJsonObjectsEqual(Left->AsObject(), Right->AsObject());
	case EJson::Array:
	{
		const TArray<TSharedPtr<FJsonValue>>& LeftArray = Left->AsArray();
		const TArray<TSharedPtr<FJsonValue>>& RightArray = Right->AsArray();
		if (LeftArray.Num() != RightArray.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < LeftArray.Num(); ++Index)
		{
			if (!AreJsonValuesEqual(LeftArray[Index], RightArray[Index]))
			{
				return false;
			}
		}
		return true;
	}
	case EJson::String:
		return Left->AsString() == Right->AsString();
	case EJson::Number:
		return FMath::IsNearlyEqual(Left->AsNumber(), Right->AsNumber());
	case EJson::Boolean:
		return Left->AsBool() == Right->AsBool();
	case EJson::Null:
	default:
		return true;
	}
}

bool AreJsonObjectsEqual(const TSharedPtr<FJsonObject>& Left, const TSharedPtr<FJsonObject>& Right)
{
	if (!Left.IsValid() || !Right.IsValid())
	{
		return !Left.IsValid() && !Right.IsValid();
	}
	if (Left->Values.Num() != Right->Values.Num())
	{
		return false;
	}

	for (const auto& Pair : Left->Values)
	{
		const TSharedPtr<FJsonValue>* RightValue = Right->Values.Find(Pair.Key);
		if (!RightValue || !AreJsonValuesEqual(Pair.Value, *RightValue))
		{
			return false;
		}
	}
	return true;
}
}
