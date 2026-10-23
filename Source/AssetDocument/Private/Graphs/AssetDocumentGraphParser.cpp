// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Graphs/AssetDocumentGraphParser.h"

#include "AssetDocumentJsonRegionUtils.h"

namespace
{
const TCHAR* GraphFields[] = {
	TEXT("Id"),
	TEXT("Kind"),
	TEXT("Owner"),
	TEXT("OwnerNodeId"),
	TEXT("OwnerPin"),
	TEXT("Name"),
	TEXT("Schema"),
	TEXT("GraphGuid"),
	TEXT("Category"),
	TEXT("Description"),
	TEXT("Signature"),
	TEXT("EntryPins"),
	TEXT("ResultPins"),
	TEXT("Position"),
	TEXT("Metadata"),
	TEXT("Evidence"),
	TEXT("Nodes"),
	TEXT("Links"),
	TEXT("Subgraphs")
};

const TCHAR* NodeFields[] = {
	TEXT("Id"),
	TEXT("NodeGuid"),
	TEXT("Class"),
	TEXT("Capability"),
	TEXT("Kind"),
	TEXT("Spawner"),
	TEXT("Fields"),
	TEXT("Pins"),
	TEXT("Member"),
	TEXT("PinOverrides"),
	TEXT("Position"),
	TEXT("SubgraphRefs"),
	TEXT("Evidence"),
	TEXT("Comment"),
	TEXT("CommentBubblePinned"),
	TEXT("CommentBubbleVisible"),
	TEXT("AdvancedPinDisplay"),
	TEXT("EnabledState")
};

const TCHAR* GraphRegionFields[] = {
	TEXT("Graphs")
};

const TCHAR* PinOverrideFields[] = {
	TEXT("Pin"),
	TEXT("Direction"),
	TEXT("PersistentGuid"),
	TEXT("SplitState"),
	TEXT("ParentPin"),
	TEXT("SubPins"),
	TEXT("Type"),
	TEXT("DefaultValue"),
	TEXT("DefaultObject"),
	TEXT("DefaultTextValue"),
	TEXT("Hidden"),
	TEXT("AdvancedView")
};

const TCHAR* LinkFields[] = {
	TEXT("From"),
	TEXT("To")
};

const TCHAR* LinkEndpointFields[] = {
	TEXT("Node"),
	TEXT("Pin")
};

const TCHAR* GraphOwnerFields[] = {
	TEXT("StateMachine"),
	TEXT("State"),
	TEXT("Transition"),
	TEXT("Layer"),
	TEXT("Function"),
	TEXT("Macro"),
	TEXT("ParentGraph"),
	TEXT("Graph"),
	TEXT("Node"),
	TEXT("OwnerNodeId"),
	TEXT("OwnerPin"),
	TEXT("ParentNode"),
	TEXT("Pin")
};

bool IsKnownField(const FString& Field, const TCHAR* const* KnownFields, int32 KnownFieldCount)
{
	for (int32 Index = 0; Index < KnownFieldCount; ++Index)
	{
		if (Field == KnownFields[Index])
		{
			return true;
		}
	}
	return false;
}

bool IsSidecarId(const FString& Value)
{
	if (Value.IsEmpty())
	{
		return false;
	}

	const TCHAR First = Value[0];
	if (!FChar::IsAlpha(First) && First != TEXT('_'))
	{
		return false;
	}

	for (int32 Index = 1; Index < Value.Len(); ++Index)
	{
		const TCHAR Character = Value[Index];
		if (!FChar::IsAlnum(Character) && Character != TEXT('_') && Character != TEXT('-'))
		{
			return false;
		}
	}
	return true;
}

bool IsKnownGraphKind(const FString& Kind)
{
	return Kind == TEXT("AnimGraph") ||
		Kind == TEXT("StateMachine") ||
		Kind == TEXT("StatePose") ||
		Kind == TEXT("TransitionRule") ||
		Kind == TEXT("TransitionBlend") ||
		Kind == TEXT("AnimLayer") ||
		Kind == TEXT("FunctionGraph") ||
		Kind == TEXT("MacroGraph");
}

bool IsValidPersistentGuid(const FString& Value)
{
	FGuid Guid;
	return Value.Len() == 32 &&
		FGuid::ParseExact(Value, EGuidFormats::Digits, Guid) &&
		Guid.IsValid();
}

bool IsKnownPinDirection(const FString& Direction)
{
	return Direction == TEXT("Input") || Direction == TEXT("Output");
}

bool IsKnownSplitState(const FString& SplitState)
{
	return SplitState == TEXT("Unsplit") || SplitState == TEXT("Split");
}

bool IsKnownAdvancedPinDisplay(const FString& AdvancedPinDisplay)
{
	return AdvancedPinDisplay == TEXT("NoPins") ||
		AdvancedPinDisplay == TEXT("Shown") ||
		AdvancedPinDisplay == TEXT("Hidden");
}

bool IsKnownEnabledState(const FString& EnabledState)
{
	return EnabledState == TEXT("Enabled") ||
		EnabledState == TEXT("Disabled") ||
		EnabledState == TEXT("DevelopmentOnly");
}

void ValidateUnknownFields(
	const TSharedRef<FJsonObject>& Object,
	const FString& Path,
	const TCHAR* const* KnownFields,
	int32 KnownFieldCount,
	const FString& Code,
	FAssetDocumentGraphParseResult& Result)
{
	for (const auto& Pair : Object->Values)
	{
		if (!IsKnownField(FString(*Pair.Key), KnownFields, KnownFieldCount))
		{
			Result.AddDiagnostic(
				Code,
				FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Path, FString(*Pair.Key)),
				FString::Printf(TEXT("Unknown graph field '%s'."), *Pair.Key));
		}
	}
}

bool TryGetStringField(
	const TSharedRef<FJsonObject>& Object,
	const TCHAR* Field,
	const FString& Path,
	const FString& MissingCode,
	FString& OutValue,
	FAssetDocumentGraphParseResult& Result)
{
	if (!Object->TryGetStringField(Field, OutValue) || OutValue.IsEmpty())
	{
		Result.AddDiagnostic(
			MissingCode,
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Path, Field),
			FString::Printf(TEXT("Required string field '%s' is missing or empty."), Field));
		return false;
	}
	return true;
}

bool TryGetObjectFieldIfPresent(
	const TSharedRef<FJsonObject>& Object,
	const TCHAR* Field,
	const FString& Path,
	const FString& InvalidTypeCode,
	TSharedPtr<FJsonObject>& OutObject,
	FAssetDocumentGraphParseResult& Result)
{
	const TSharedPtr<FJsonValue>* Value = Object->Values.Find(Field);
	if (!Value)
	{
		return false;
	}

	if (!Value->IsValid() || (*Value)->Type != EJson::Object)
	{
		Result.AddDiagnostic(
			InvalidTypeCode,
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Path, Field),
			FString::Printf(TEXT("Field '%s' must be an object."), Field));
		return false;
	}

	OutObject = (*Value)->AsObject();
	return OutObject.IsValid();
}

bool TryGetNullableObjectFieldIfPresent(
	const TSharedRef<FJsonObject>& Object,
	const TCHAR* Field,
	const FString& Path,
	const FString& InvalidTypeCode,
	TSharedPtr<FJsonObject>& OutObject,
	FAssetDocumentGraphParseResult& Result)
{
	const TSharedPtr<FJsonValue>* Value = Object->Values.Find(Field);
	if (!Value)
	{
		return false;
	}

	if (!Value->IsValid() || (*Value)->Type == EJson::Null)
	{
		OutObject.Reset();
		return true;
	}

	if ((*Value)->Type != EJson::Object)
	{
		Result.AddDiagnostic(
			InvalidTypeCode,
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Path, Field),
			FString::Printf(TEXT("Field '%s' must be an object or null."), Field));
		return false;
	}

	OutObject = (*Value)->AsObject();
	return OutObject.IsValid();
}

bool TryGetArrayFieldIfPresent(
	const TSharedRef<FJsonObject>& Object,
	const TCHAR* Field,
	const FString& Path,
	const FString& InvalidTypeCode,
	const TArray<TSharedPtr<FJsonValue>>*& OutArray,
	FAssetDocumentGraphParseResult& Result)
{
	const TSharedPtr<FJsonValue>* Value = Object->Values.Find(Field);
	if (!Value)
	{
		return false;
	}

	if (!Value->IsValid() || (*Value)->Type != EJson::Array)
	{
		Result.AddDiagnostic(
			InvalidTypeCode,
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Path, Field),
			FString::Printf(TEXT("Field '%s' must be an array."), Field));
		return false;
	}

	OutArray = &(*Value)->AsArray();
	return true;
}

bool TryGetRequiredArrayField(
	const TSharedRef<FJsonObject>& Object,
	const TCHAR* Field,
	const FString& Path,
	const FString& InvalidTypeCode,
	const TArray<TSharedPtr<FJsonValue>>*& OutArray,
	FAssetDocumentGraphParseResult& Result)
{
	if (!TryGetArrayFieldIfPresent(Object, Field, Path, InvalidTypeCode, OutArray, Result))
	{
		Result.AddDiagnostic(
			InvalidTypeCode,
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Path, Field),
			FString::Printf(TEXT("%s must be an array."), Field));
		return false;
	}
	return true;
}

bool TryGetStringFieldIfPresent(
	const TSharedRef<FJsonObject>& Object,
	const TCHAR* Field,
	const FString& Path,
	const FString& InvalidTypeCode,
	FString& OutValue,
	FAssetDocumentGraphParseResult& Result)
{
	const TSharedPtr<FJsonValue>* Value = Object->Values.Find(Field);
	if (!Value)
	{
		return true;
	}

	if (!Value->IsValid() || (*Value)->Type != EJson::String)
	{
		Result.AddDiagnostic(
			InvalidTypeCode,
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Path, Field),
			FString::Printf(TEXT("Field '%s' must be a string."), Field));
		return false;
	}

	OutValue = (*Value)->AsString();
	return true;
}

bool TryGetBoolFieldIfPresent(
	const TSharedRef<FJsonObject>& Object,
	const TCHAR* Field,
	const FString& Path,
	const FString& InvalidTypeCode,
	TOptional<bool>& OutValue,
	FAssetDocumentGraphParseResult& Result)
{
	const TSharedPtr<FJsonValue>* Value = Object->Values.Find(Field);
	if (!Value)
	{
		return true;
	}

	if (!Value->IsValid() || (*Value)->Type != EJson::Boolean)
	{
		Result.AddDiagnostic(
			InvalidTypeCode,
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Path, Field),
			FString::Printf(TEXT("Field '%s' must be a boolean."), Field));
		return false;
	}

	OutValue = (*Value)->AsBool();
	return true;
}

bool TryCloneAnyFieldIfPresent(
	const TSharedRef<FJsonObject>& Object,
	const TCHAR* Field,
	TSharedPtr<FJsonValue>& OutValue)
{
	const TSharedPtr<FJsonValue>* Value = Object->Values.Find(Field);
	if (!Value)
	{
		return false;
	}

	OutValue = FAssetDocumentJsonRegionUtils::DeepCloneJsonValue(*Value);
	return true;
}

struct FGraphParseContext
{
	TArray<TPair<FString, FString>> GraphStack;
};

bool GraphStackContainsKindAndId(
	const FGraphParseContext& Context,
	const FString& Kind,
	const FString& Id)
{
	for (const TPair<FString, FString>& GraphIdentity : Context.GraphStack)
	{
		if (GraphIdentity.Key == Kind && GraphIdentity.Value == Id)
		{
			return true;
		}
	}
	return false;
}

bool ValidateOwnerIdField(
	const TSharedRef<FJsonObject>& Owner,
	const FString& OwnerPath,
	const TCHAR* Field,
	FAssetDocumentGraphParseResult& Result,
	FString& OutValue)
{
	const TSharedPtr<FJsonValue>* Value = Owner->Values.Find(Field);
	if (!Value)
	{
		return true;
	}

	if (!Value->IsValid() || (*Value)->Type != EJson::String)
	{
		Result.AddDiagnostic(
			TEXT("InvalidGraphOwner"),
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(OwnerPath, Field),
			FString::Printf(TEXT("Owner field '%s' must be a string stable id."), Field));
		return false;
	}

	OutValue = (*Value)->AsString();
	if (!IsSidecarId(OutValue))
	{
		Result.AddDiagnostic(
			TEXT("InvalidGraphOwner"),
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(OwnerPath, Field),
			FString::Printf(TEXT("Owner field '%s' must match ^[A-Za-z_][A-Za-z0-9_-]*$."), Field));
		return false;
	}

	return true;
}

void ValidateGraphOwner(
	const TSharedPtr<FJsonObject>& Owner,
	const FString& GraphPath,
	const FGraphParseContext& Context,
	FAssetDocumentGraphParseResult& Result)
{
	if (!Owner.IsValid())
	{
		return;
	}

	const FString OwnerPath = FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("Owner"));
	ValidateUnknownFields(
		Owner.ToSharedRef(),
		OwnerPath,
		GraphOwnerFields,
		UE_ARRAY_COUNT(GraphOwnerFields),
		TEXT("UnknownGraphOwnerField"),
		Result);

	FString StateMachineId;
	if (ValidateOwnerIdField(Owner.ToSharedRef(), OwnerPath, TEXT("StateMachine"), Result, StateMachineId) &&
		!StateMachineId.IsEmpty() &&
		!GraphStackContainsKindAndId(Context, TEXT("StateMachine"), StateMachineId))
	{
		Result.AddDiagnostic(
			TEXT("UnknownGraphOwnerReference"),
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(OwnerPath, TEXT("StateMachine")),
			FString::Printf(TEXT("Owner StateMachine '%s' does not refer to the current graph family."), *StateMachineId));
	}

	const TCHAR* IdOwnerFields[] = {
		TEXT("State"),
		TEXT("Transition"),
		TEXT("Layer"),
		TEXT("Function"),
		TEXT("Macro"),
		TEXT("ParentGraph"),
		TEXT("Graph"),
		TEXT("Node"),
		TEXT("OwnerNodeId"),
		TEXT("OwnerPin"),
		TEXT("ParentNode"),
		TEXT("Pin")
	};
	for (const TCHAR* Field : IdOwnerFields)
	{
		FString Ignored;
		ValidateOwnerIdField(Owner.ToSharedRef(), OwnerPath, Field, Result, Ignored);
	}
}

bool ParseSharedGraphFields(
	const TSharedRef<FJsonObject>& GraphObject,
	const FString& GraphPath,
	FAssetDocumentGraphSpec& OutGraph,
	FAssetDocumentGraphParseResult& Result)
{
	bool bParsed = true;
	if (!TryGetStringFieldIfPresent(
			GraphObject,
			TEXT("OwnerNodeId"),
			GraphPath,
			TEXT("InvalidGraphOwner"),
			OutGraph.OwnerNodeId,
			Result) ||
		(!OutGraph.OwnerNodeId.IsEmpty() && !IsSidecarId(OutGraph.OwnerNodeId)))
	{
		Result.AddDiagnostic(
			TEXT("InvalidGraphOwner"),
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("OwnerNodeId")),
			TEXT("OwnerNodeId must match ^[A-Za-z_][A-Za-z0-9_-]*$."));
		bParsed = false;
	}
	if (!TryGetStringFieldIfPresent(
			GraphObject,
			TEXT("OwnerPin"),
			GraphPath,
			TEXT("InvalidGraphOwner"),
			OutGraph.OwnerPin,
			Result) ||
		(!OutGraph.OwnerPin.IsEmpty() && !IsSidecarId(OutGraph.OwnerPin)))
	{
		Result.AddDiagnostic(
			TEXT("InvalidGraphOwner"),
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("OwnerPin")),
			TEXT("OwnerPin must match ^[A-Za-z_][A-Za-z0-9_-]*$."));
		bParsed = false;
	}

	TryCloneAnyFieldIfPresent(GraphObject, TEXT("EntryPins"), OutGraph.EntryPins);
	TryCloneAnyFieldIfPresent(GraphObject, TEXT("ResultPins"), OutGraph.ResultPins);
	TryCloneAnyFieldIfPresent(GraphObject, TEXT("Metadata"), OutGraph.Metadata);
	return bParsed;
}

bool TryParseEndpointObject(
	const TSharedRef<FJsonObject>& Object,
	const FString& Path,
	FAssetDocumentGraphEndpoint& OutEndpoint,
	FAssetDocumentGraphParseResult& Result)
{
	ValidateUnknownFields(
		Object,
		Path,
		LinkEndpointFields,
		UE_ARRAY_COUNT(LinkEndpointFields),
		TEXT("UnknownGraphLinkEndpointField"),
		Result);

	if (!TryGetStringField(Object, TEXT("Node"), Path, TEXT("InvalidGraphLinkEndpointSyntax"), OutEndpoint.Node, Result) ||
		!TryGetStringField(Object, TEXT("Pin"), Path, TEXT("InvalidGraphLinkEndpointSyntax"), OutEndpoint.Pin, Result))
	{
		return false;
	}

	if (!IsSidecarId(OutEndpoint.Node))
	{
		Result.AddDiagnostic(
			TEXT("InvalidGraphNodeId"),
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Path, TEXT("Node")),
			TEXT("Expanded link endpoint node id must match ^[A-Za-z_][A-Za-z0-9_-]*$."));
		return false;
	}

	if (!IsSidecarId(OutEndpoint.Pin))
	{
		Result.AddDiagnostic(
			TEXT("InvalidGraphPinId"),
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Path, TEXT("Pin")),
			TEXT("Expanded link endpoint pin id must match ^[A-Za-z_][A-Za-z0-9_-]*$."));
		return false;
	}
	return true;
}

bool TryParseCompactEndpoint(
	const FString& Endpoint,
	const FString& Path,
	FAssetDocumentGraphEndpoint& OutEndpoint,
	FAssetDocumentGraphParseResult& Result)
{
	TArray<FString> Parts;
	Endpoint.ParseIntoArray(Parts, TEXT("."), false);
	if (Parts.Num() != 2 || Parts[0].IsEmpty() || Parts[1].IsEmpty())
	{
		Result.AddDiagnostic(
			TEXT("InvalidGraphLinkEndpointSyntax"),
			Path,
			TEXT("Compact link endpoint must use exactly one dot in Node.Pin form."));
		return false;
	}

	OutEndpoint.Node = Parts[0];
	OutEndpoint.Pin = Parts[1];
	if (!IsSidecarId(OutEndpoint.Node))
	{
		Result.AddDiagnostic(
			TEXT("InvalidGraphNodeId"),
			Path,
			TEXT("Compact link endpoint node id must match ^[A-Za-z_][A-Za-z0-9_-]*$."));
		return false;
	}

	if (!IsSidecarId(OutEndpoint.Pin))
	{
		Result.AddDiagnostic(
			TEXT("InvalidGraphPinId"),
			Path,
			TEXT("Compact link endpoint pin id must match ^[A-Za-z_][A-Za-z0-9_-]*$."));
		return false;
	}
	return true;
}

bool TryParseEndpoint(
	const TSharedPtr<FJsonValue>& Value,
	const FString& Path,
	FAssetDocumentGraphEndpoint& OutEndpoint,
	FAssetDocumentGraphParseResult& Result)
{
	if (!Value.IsValid())
	{
		Result.AddDiagnostic(TEXT("InvalidGraphLinkEndpointSyntax"), Path, TEXT("Link endpoint is missing."));
		return false;
	}

	if (Value->Type == EJson::String)
	{
		return TryParseCompactEndpoint(Value->AsString(), Path, OutEndpoint, Result);
	}

	if (Value->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		if (Object.IsValid())
		{
			return TryParseEndpointObject(Object.ToSharedRef(), Path, OutEndpoint, Result);
		}
	}

	Result.AddDiagnostic(
		TEXT("InvalidGraphLinkEndpointSyntax"),
		Path,
		TEXT("Link endpoint must be an expanded object or compact Node.Pin string."));
	return false;
}

bool ParsePinOverride(
	const TSharedRef<FJsonObject>& Object,
	const FString& Path,
	FAssetDocumentPinOverrideSpec& OutPinOverride,
	FAssetDocumentGraphParseResult& Result)
{
	ValidateUnknownFields(
		Object,
		Path,
		PinOverrideFields,
		UE_ARRAY_COUNT(PinOverrideFields),
		TEXT("UnknownGraphPinOverrideField"),
		Result);

	if (!TryGetStringField(Object, TEXT("Pin"), Path, TEXT("InvalidGraphPinId"), OutPinOverride.Pin, Result))
	{
		return false;
	}

	if (!IsSidecarId(OutPinOverride.Pin))
	{
		Result.AddDiagnostic(
			TEXT("InvalidGraphPinId"),
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Path, TEXT("Pin")),
			TEXT("Pin id must match ^[A-Za-z_][A-Za-z0-9_-]*$."));
		return false;
	}

	if (!TryGetStringFieldIfPresent(
			Object,
			TEXT("Direction"),
			Path,
			TEXT("InvalidGraphPin"),
			OutPinOverride.Direction,
			Result))
	{
		return false;
	}
	if (!OutPinOverride.Direction.IsEmpty() && !IsKnownPinDirection(OutPinOverride.Direction))
	{
		Result.AddDiagnostic(
			TEXT("InvalidGraphPinDirection"),
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Path, TEXT("Direction")),
			FString::Printf(TEXT("Unknown pin direction '%s'."), *OutPinOverride.Direction));
		return false;
	}

	if (!TryGetStringFieldIfPresent(
			Object,
			TEXT("PersistentGuid"),
			Path,
			TEXT("InvalidGraphPinGuid"),
			OutPinOverride.PersistentGuid,
			Result))
	{
		return false;
	}
	if (!OutPinOverride.PersistentGuid.IsEmpty() && !IsValidPersistentGuid(OutPinOverride.PersistentGuid))
	{
		Result.AddDiagnostic(
			TEXT("InvalidGraphPinGuid"),
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Path, TEXT("PersistentGuid")),
			TEXT("PersistentGuid must be a non-zero 32-digit GUID."));
		return false;
	}

	if (!TryGetStringFieldIfPresent(
			Object,
			TEXT("SplitState"),
			Path,
			TEXT("InvalidGraphPinSplitState"),
			OutPinOverride.SplitState,
			Result))
	{
		return false;
	}
	if (!OutPinOverride.SplitState.IsEmpty() && !IsKnownSplitState(OutPinOverride.SplitState))
	{
		Result.AddDiagnostic(
			TEXT("InvalidGraphPinSplitState"),
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Path, TEXT("SplitState")),
			FString::Printf(TEXT("Unknown pin split state '%s'."), *OutPinOverride.SplitState));
		return false;
	}

	if (!TryGetStringFieldIfPresent(
			Object,
			TEXT("ParentPin"),
			Path,
			TEXT("InvalidGraphPinId"),
			OutPinOverride.ParentPin,
			Result))
	{
		return false;
	}
	if (!OutPinOverride.ParentPin.IsEmpty() && !IsSidecarId(OutPinOverride.ParentPin))
	{
		Result.AddDiagnostic(
			TEXT("InvalidGraphPinId"),
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Path, TEXT("ParentPin")),
			TEXT("ParentPin must be a stable pin id."));
		return false;
	}

	OutPinOverride.bHasSubPins = Object->HasField(TEXT("SubPins"));
	const TArray<TSharedPtr<FJsonValue>>* SubPins = nullptr;
	if (TryGetArrayFieldIfPresent(
			Object,
			TEXT("SubPins"),
			Path,
			TEXT("InvalidGraphPinSplitState"),
			SubPins,
			Result))
	{
		TSet<FString> SubPinIds;
		for (int32 SubPinIndex = 0; SubPinIndex < SubPins->Num(); ++SubPinIndex)
		{
			const TSharedPtr<FJsonValue>& SubPinValue = (*SubPins)[SubPinIndex];
			const FString SubPinPath = FAssetDocumentJsonRegionUtils::AppendJsonPointerIndex(
				FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Path, TEXT("SubPins")),
				SubPinIndex);
			if (!SubPinValue.IsValid() || SubPinValue->Type != EJson::String || !IsSidecarId(SubPinValue->AsString()))
			{
				Result.AddDiagnostic(
					TEXT("InvalidGraphPinId"),
					SubPinPath,
					TEXT("SubPins entries must be stable pin id strings."));
				continue;
			}
			const FString SubPinId = SubPinValue->AsString();
			if (SubPinIds.Contains(SubPinId))
			{
				Result.AddDiagnostic(
					TEXT("DuplicateGraphSubPin"),
					SubPinPath,
					FString::Printf(TEXT("Duplicate sub-pin id '%s'."), *SubPinId));
				continue;
			}
			SubPinIds.Add(SubPinId);
			OutPinOverride.SubPins.Add(SubPinId);
		}
	}
	else if (OutPinOverride.bHasSubPins)
	{
		return false;
	}

	if (const TSharedPtr<FJsonValue>* Type = Object->Values.Find(TEXT("Type")))
	{
		if (!Type->IsValid() || (*Type)->Type != EJson::Object)
		{
			Result.AddDiagnostic(
				TEXT("InvalidGraphPin"),
				FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Path, TEXT("Type")),
				TEXT("Pin override Type must be an object."));
			return false;
		}
		OutPinOverride.Type = FAssetDocumentJsonRegionUtils::DeepCloneJsonValue(*Type);
	}
	if (const TSharedPtr<FJsonValue>* DefaultValue = Object->Values.Find(TEXT("DefaultValue")))
	{
		OutPinOverride.DefaultValue = FAssetDocumentJsonRegionUtils::DeepCloneJsonValue(*DefaultValue);
	}
	if (const TSharedPtr<FJsonValue>* DefaultObject = Object->Values.Find(TEXT("DefaultObject")))
	{
		OutPinOverride.DefaultObject = FAssetDocumentJsonRegionUtils::DeepCloneJsonValue(*DefaultObject);
	}
	if (const TSharedPtr<FJsonValue>* DefaultTextValue = Object->Values.Find(TEXT("DefaultTextValue")))
	{
		OutPinOverride.DefaultTextValue = FAssetDocumentJsonRegionUtils::DeepCloneJsonValue(*DefaultTextValue);
	}

	if (!TryGetBoolFieldIfPresent(
			Object,
			TEXT("Hidden"),
			Path,
			TEXT("InvalidGraphPin"),
			OutPinOverride.Hidden,
			Result))
	{
		return false;
	}

	if (!TryGetBoolFieldIfPresent(
			Object,
			TEXT("AdvancedView"),
			Path,
			TEXT("InvalidGraphPin"),
			OutPinOverride.AdvancedView,
			Result))
	{
		return false;
	}
	return true;
}

bool ParseNode(
	const TSharedRef<FJsonObject>& Object,
	const FString& Path,
	FAssetDocumentNodeSpec& OutNode,
	FAssetDocumentGraphParseResult& Result)
{
	ValidateUnknownFields(
		Object,
		Path,
		NodeFields,
		UE_ARRAY_COUNT(NodeFields),
		TEXT("UnknownGraphNodeField"),
		Result);

	if (!TryGetStringField(Object, TEXT("Id"), Path, TEXT("InvalidGraphNodeId"), OutNode.Id, Result))
	{
		return false;
	}

	if (!IsSidecarId(OutNode.Id))
	{
		Result.AddDiagnostic(
			TEXT("InvalidGraphNodeId"),
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Path, TEXT("Id")),
			TEXT("Node id must match ^[A-Za-z_][A-Za-z0-9_-]*$."));
		return false;
	}

	TryGetStringField(Object, TEXT("Class"), Path, TEXT("MissingGraphNodeClass"), OutNode.Class, Result);
	if (!TryGetStringFieldIfPresent(
		Object,
		TEXT("NodeGuid"),
		Path,
		TEXT("InvalidGraphNodeGuid"),
		OutNode.NodeGuid,
		Result))
	{
		return false;
	}
	if (!TryGetStringFieldIfPresent(
			Object,
			TEXT("Capability"),
			Path,
			TEXT("InvalidGraphNodeCapability"),
			OutNode.Capability,
			Result) ||
		!TryGetStringFieldIfPresent(
			Object,
			TEXT("Kind"),
			Path,
			TEXT("InvalidGraphNodeKind"),
			OutNode.Kind,
			Result) ||
		!TryGetStringFieldIfPresent(
			Object,
			TEXT("Comment"),
			Path,
			TEXT("InvalidGraphNodeComment"),
			OutNode.Comment,
			Result))
	{
		return false;
	}
	OutNode.bHasComment = Object->HasField(TEXT("Comment"));
	if (!TryGetBoolFieldIfPresent(
			Object,
			TEXT("CommentBubblePinned"),
			Path,
			TEXT("InvalidGraphNodeLayout"),
			OutNode.CommentBubblePinned,
			Result) ||
		!TryGetBoolFieldIfPresent(
			Object,
			TEXT("CommentBubbleVisible"),
			Path,
			TEXT("InvalidGraphNodeLayout"),
			OutNode.CommentBubbleVisible,
			Result) ||
		!TryGetStringFieldIfPresent(
			Object,
			TEXT("AdvancedPinDisplay"),
			Path,
			TEXT("InvalidGraphAdvancedPinDisplay"),
			OutNode.AdvancedPinDisplay,
			Result) ||
		!TryGetStringFieldIfPresent(
			Object,
			TEXT("EnabledState"),
			Path,
			TEXT("InvalidGraphNodeEnabledState"),
			OutNode.EnabledState,
			Result))
	{
		return false;
	}
	if (!OutNode.AdvancedPinDisplay.IsEmpty() && !IsKnownAdvancedPinDisplay(OutNode.AdvancedPinDisplay))
	{
		Result.AddDiagnostic(
			TEXT("InvalidGraphAdvancedPinDisplay"),
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Path, TEXT("AdvancedPinDisplay")),
			FString::Printf(TEXT("Unknown advanced-pin display state '%s'."), *OutNode.AdvancedPinDisplay));
		return false;
	}
	if (!OutNode.EnabledState.IsEmpty() && !IsKnownEnabledState(OutNode.EnabledState))
	{
		Result.AddDiagnostic(
			TEXT("InvalidGraphNodeEnabledState"),
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Path, TEXT("EnabledState")),
			FString::Printf(TEXT("Unknown node enabled state '%s'."), *OutNode.EnabledState));
		return false;
	}

	TSharedPtr<FJsonObject> Spawner;
	if (TryGetObjectFieldIfPresent(Object, TEXT("Spawner"), Path, TEXT("InvalidGraphNodeSpawner"), Spawner, Result))
	{
		OutNode.Spawner = FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(Spawner);
	}

	TSharedPtr<FJsonObject> Fields;
	if (TryGetObjectFieldIfPresent(Object, TEXT("Fields"), Path, TEXT("InvalidGraphNodeFields"), Fields, Result))
	{
		OutNode.Fields = FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(Fields);
	}

	TSharedPtr<FJsonObject> Pins;
	if (TryGetObjectFieldIfPresent(Object, TEXT("Pins"), Path, TEXT("InvalidGraphNodePins"), Pins, Result))
	{
		OutNode.Pins = FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(Pins);
	}

	TSharedPtr<FJsonObject> Member;
	if (TryGetObjectFieldIfPresent(Object, TEXT("Member"), Path, TEXT("InvalidGraphMemberReference"), Member, Result))
	{
		OutNode.Member = FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(Member);
	}

	TSharedPtr<FJsonObject> Position;
	if (TryGetObjectFieldIfPresent(Object, TEXT("Position"), Path, TEXT("InvalidGraphPosition"), Position, Result))
	{
		OutNode.Position = FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(Position);
	}

	TSharedPtr<FJsonObject> SubgraphRefs;
	if (TryGetObjectFieldIfPresent(Object, TEXT("SubgraphRefs"), Path, TEXT("InvalidGraphSubgraphRefs"), SubgraphRefs, Result))
	{
		OutNode.SubgraphRefs = FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(SubgraphRefs);
	}

	TSharedPtr<FJsonObject> Evidence;
	if (TryGetObjectFieldIfPresent(Object, TEXT("Evidence"), Path, TEXT("InvalidGraphEvidence"), Evidence, Result))
	{
		OutNode.Evidence = FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(Evidence);
	}

	const TArray<TSharedPtr<FJsonValue>>* PinOverrides = nullptr;
	if (TryGetArrayFieldIfPresent(Object, TEXT("PinOverrides"), Path, TEXT("InvalidGraphPin"), PinOverrides, Result))
	{
		TSet<FString> PinOverrideIds;
		TMap<FString, TSet<FString>> PinOverrideDirectionsByName;
		TSet<FString> PersistentPinGuids;
		for (int32 Index = 0; Index < PinOverrides->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> PinObject = (*PinOverrides)[Index].IsValid()
				? (*PinOverrides)[Index]->AsObject()
				: nullptr;
			if (!PinObject.IsValid())
			{
				Result.AddDiagnostic(
					TEXT("InvalidGraphPin"),
					FAssetDocumentJsonRegionUtils::AppendJsonPointerIndex(
						FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Path, TEXT("PinOverrides")),
						Index),
					TEXT("Pin override must be an object."));
				continue;
			}

			FAssetDocumentPinOverrideSpec PinOverride;
			const FString PinOverridePath = FAssetDocumentJsonRegionUtils::AppendJsonPointerIndex(
				FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Path, TEXT("PinOverrides")),
				Index);
			if (ParsePinOverride(PinObject.ToSharedRef(), PinOverridePath, PinOverride, Result))
			{
				const FString PinIdentity = PinOverride.ToSemanticKey();
				const TSet<FString>* ExistingDirections = PinOverrideDirectionsByName.Find(PinOverride.Pin);
				const bool bConflictsWithDirectionlessIdentity = ExistingDirections &&
					((PinOverride.Direction.IsEmpty() && !ExistingDirections->IsEmpty()) ||
					 (!PinOverride.Direction.IsEmpty() && ExistingDirections->Contains(FString())));
				if (PinOverrideIds.Contains(PinIdentity))
				{
					Result.AddDiagnostic(
						TEXT("DuplicateGraphPinId"),
						FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(PinOverridePath, TEXT("Pin")),
						FString::Printf(TEXT("Duplicate pin override identity '%s'."), *PinIdentity));
				}
				else if (bConflictsWithDirectionlessIdentity)
				{
					Result.AddDiagnostic(
						TEXT("DuplicateGraphPinId"),
						FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(PinOverridePath, TEXT("Pin")),
						FString::Printf(
							TEXT("Directionless and directed overrides for pin '%s' cannot be authored together."),
							*PinOverride.Pin));
				}
				else
				{
					PinOverrideIds.Add(PinIdentity);
				}
				PinOverrideDirectionsByName.FindOrAdd(PinOverride.Pin).Add(PinOverride.Direction);
				if (!PinOverride.PersistentGuid.IsEmpty())
				{
					if (PersistentPinGuids.Contains(PinOverride.PersistentGuid))
					{
						Result.AddDiagnostic(
							TEXT("DuplicateGraphPinGuid"),
							FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(
								FAssetDocumentJsonRegionUtils::AppendJsonPointerIndex(
									FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Path, TEXT("PinOverrides")),
									Index),
								TEXT("PersistentGuid")),
							FString::Printf(TEXT("Duplicate persistent pin GUID '%s'."), *PinOverride.PersistentGuid));
					}
					PersistentPinGuids.Add(PinOverride.PersistentGuid);
				}
				OutNode.PinOverrides.Add(MoveTemp(PinOverride));
			}
		}

		TMap<FString, const FAssetDocumentPinOverrideSpec*> PinsById;
		for (const FAssetDocumentPinOverrideSpec& PinOverride : OutNode.PinOverrides)
		{
			PinsById.Add(PinOverride.ToSemanticKey(), &PinOverride);
		}
		for (const FAssetDocumentPinOverrideSpec& PinOverride : OutNode.PinOverrides)
		{
			const FString PinPath = FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(
				FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Path, TEXT("PinOverrides")),
				PinOverride.ToSemanticKey());
			if (!PinOverride.ParentPin.IsEmpty())
			{
				const FString ParentIdentity = PinOverride.Direction.IsEmpty()
					? PinOverride.ParentPin
					: PinOverride.Direction + TEXT(":") + PinOverride.ParentPin;
				const FAssetDocumentPinOverrideSpec* const* Parent = PinsById.Find(ParentIdentity);
				if (!Parent)
				{
					Result.AddDiagnostic(
						TEXT("UnknownGraphParentPin"),
						FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(PinPath, TEXT("ParentPin")),
						FString::Printf(TEXT("Parent pin '%s' does not exist on the node."), *PinOverride.ParentPin));
				}
				else if (!(*Parent)->SubPins.Contains(PinOverride.Pin))
				{
					Result.AddDiagnostic(
						TEXT("ConflictingGraphPinSplit"),
						FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(PinPath, TEXT("ParentPin")),
						TEXT("ParentPin and parent SubPins must describe the same split relationship."));
				}
			}

			if (PinOverride.SplitState == TEXT("Split") && (!PinOverride.bHasSubPins || PinOverride.SubPins.IsEmpty()))
			{
				Result.AddDiagnostic(
					TEXT("InvalidGraphPinSplitState"),
					FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(PinPath, TEXT("SubPins")),
					TEXT("A split pin must declare at least one SubPins entry."));
			}
			if (PinOverride.SplitState == TEXT("Unsplit") && PinOverride.bHasSubPins && !PinOverride.SubPins.IsEmpty())
			{
				Result.AddDiagnostic(
					TEXT("InvalidGraphPinSplitState"),
					FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(PinPath, TEXT("SubPins")),
					TEXT("An unsplit pin cannot declare SubPins."));
			}
			for (const FString& SubPinId : PinOverride.SubPins)
			{
				const FString ChildIdentity = PinOverride.Direction.IsEmpty()
					? SubPinId
					: PinOverride.Direction + TEXT(":") + SubPinId;
				const FAssetDocumentPinOverrideSpec* const* Child = PinsById.Find(ChildIdentity);
				if (!Child)
				{
					Result.AddDiagnostic(
						TEXT("UnknownGraphSubPin"),
						FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(PinPath, TEXT("SubPins")),
						FString::Printf(TEXT("Sub-pin '%s' does not exist on the node."), *SubPinId));
				}
				else if ((*Child)->ParentPin != PinOverride.Pin)
				{
					Result.AddDiagnostic(
						TEXT("ConflictingGraphPinSplit"),
						FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(PinPath, TEXT("SubPins")),
						TEXT("SubPins and child ParentPin must describe the same split relationship."));
				}
			}

			TSet<FString> VisitedParents;
			const FAssetDocumentPinOverrideSpec* Cursor = &PinOverride;
			while (Cursor && !Cursor->ParentPin.IsEmpty())
			{
				const FString CursorIdentity = Cursor->ToSemanticKey();
				if (VisitedParents.Contains(CursorIdentity))
				{
					Result.AddDiagnostic(
						TEXT("CircularGraphPinSplit"),
						FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(PinPath, TEXT("ParentPin")),
						TEXT("Pin split parent relationships must not contain a cycle."));
					break;
				}
				VisitedParents.Add(CursorIdentity);
				const FString ParentIdentity = Cursor->Direction.IsEmpty()
					? Cursor->ParentPin
					: Cursor->Direction + TEXT(":") + Cursor->ParentPin;
				const FAssetDocumentPinOverrideSpec* const* Parent = PinsById.Find(ParentIdentity);
				Cursor = Parent ? *Parent : nullptr;
			}
		}
	}

	return true;
}

bool ParseLink(
	const TSharedRef<FJsonObject>& Object,
	const FString& Path,
	FAssetDocumentLinkSpec& OutLink,
	FAssetDocumentGraphParseResult& Result)
{
	ValidateUnknownFields(
		Object,
		Path,
		LinkFields,
		UE_ARRAY_COUNT(LinkFields),
		TEXT("UnknownGraphLinkField"),
		Result);

	const TSharedPtr<FJsonValue>* From = Object->Values.Find(TEXT("From"));
	const TSharedPtr<FJsonValue>* To = Object->Values.Find(TEXT("To"));
	const bool bParsedFrom = TryParseEndpoint(
		From ? *From : nullptr,
		FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Path, TEXT("From")),
		OutLink.From,
		Result);
	const bool bParsedTo = TryParseEndpoint(
		To ? *To : nullptr,
		FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Path, TEXT("To")),
		OutLink.To,
		Result);
	return bParsedFrom && bParsedTo;
}

void ParseNodeArray(
	const TArray<TSharedPtr<FJsonValue>>& Nodes,
	const FString& GraphPath,
	FAssetDocumentGraphSpec& Graph,
	FAssetDocumentGraphParseResult& Result)
{
	TSet<FString> NodeIds;
	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		const FString NodePath = FAssetDocumentJsonRegionUtils::AppendJsonPointerIndex(
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("Nodes")),
			Index);
		const TSharedPtr<FJsonObject> NodeObject = Nodes[Index].IsValid() ? Nodes[Index]->AsObject() : nullptr;
		if (!NodeObject.IsValid())
		{
			Result.AddDiagnostic(TEXT("InvalidGraphNode"), NodePath, TEXT("Node entry must be an object."));
			continue;
		}

		FAssetDocumentNodeSpec Node;
		if (!ParseNode(NodeObject.ToSharedRef(), NodePath, Node, Result))
		{
			continue;
		}

		if (NodeIds.Contains(Node.Id))
		{
			Result.AddDiagnostic(
				TEXT("DuplicateGraphNodeId"),
				FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(NodePath, TEXT("Id")),
				FString::Printf(TEXT("Duplicate node id '%s'."), *Node.Id));
		}
		else
		{
			NodeIds.Add(Node.Id);
		}
		Graph.Nodes.Add(MoveTemp(Node));
	}
}

void ParseLinkArray(
	const TArray<TSharedPtr<FJsonValue>>& Links,
	const FString& GraphPath,
	FAssetDocumentGraphSpec& Graph,
	FAssetDocumentGraphParseResult& Result)
{
	TSet<FString> LinkKeys;
	for (int32 Index = 0; Index < Links.Num(); ++Index)
	{
		const FString LinkPath = FAssetDocumentJsonRegionUtils::AppendJsonPointerIndex(
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("Links")),
			Index);
		const TSharedPtr<FJsonObject> LinkObject = Links[Index].IsValid() ? Links[Index]->AsObject() : nullptr;
		if (!LinkObject.IsValid())
		{
			Result.AddDiagnostic(TEXT("InvalidGraphLink"), LinkPath, TEXT("Link entry must be an object."));
			continue;
		}

		FAssetDocumentLinkSpec Link;
		if (!ParseLink(LinkObject.ToSharedRef(), LinkPath, Link, Result))
		{
			continue;
		}

		const FString LinkKey = Link.ToKey();
		if (LinkKeys.Contains(LinkKey))
		{
			Result.AddDiagnostic(
				TEXT("DuplicateGraphLink"),
				LinkPath,
				FString::Printf(TEXT("Duplicate graph link '%s'."), *LinkKey));
		}
		else
		{
			LinkKeys.Add(LinkKey);
		}
		Graph.Links.Add(MoveTemp(Link));
	}
}

bool ParseRecursiveGraph(
	const TSharedRef<FJsonObject>& GraphObject,
	const FString& GraphPath,
	bool bRejectUnknownGraphFields,
	const FGraphParseContext& Context,
	FAssetDocumentGraphSpec& OutGraph,
	FAssetDocumentGraphParseResult& Result);

void ParseSubgraphArray(
	const TArray<TSharedPtr<FJsonValue>>& Subgraphs,
	const FString& GraphPath,
	bool bRejectUnknownGraphFields,
	const FGraphParseContext& Context,
	FAssetDocumentGraphSpec& Graph,
	FAssetDocumentGraphParseResult& Result)
{
	TSet<FString> GraphIds;
	for (int32 Index = 0; Index < Subgraphs.Num(); ++Index)
	{
		const FString SubgraphPath = FAssetDocumentJsonRegionUtils::AppendJsonPointerIndex(
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("Subgraphs")),
			Index);
		const TSharedPtr<FJsonObject> SubgraphObject = Subgraphs[Index].IsValid() ? Subgraphs[Index]->AsObject() : nullptr;
		if (!SubgraphObject.IsValid())
		{
			Result.AddDiagnostic(TEXT("InvalidGraphRegionType"), SubgraphPath, TEXT("Subgraph entry must be an object."));
			continue;
		}

		FAssetDocumentGraphSpec Subgraph;
		FGraphParseContext SubgraphContext = Context;
		SubgraphContext.GraphStack.Add(TPair<FString, FString>(Graph.Kind, Graph.Id));
		if (!ParseRecursiveGraph(SubgraphObject.ToSharedRef(), SubgraphPath, bRejectUnknownGraphFields, SubgraphContext, Subgraph, Result))
		{
			continue;
		}

		if (GraphIds.Contains(Subgraph.Id))
		{
			Result.AddDiagnostic(
				TEXT("DuplicateGraphId"),
				FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(SubgraphPath, TEXT("Id")),
				FString::Printf(TEXT("Duplicate graph id '%s'."), *Subgraph.Id));
		}
		else
		{
			GraphIds.Add(Subgraph.Id);
		}
		Graph.Subgraphs.Add(MoveTemp(Subgraph));
	}
}

bool ParseRecursiveGraph(
	const TSharedRef<FJsonObject>& GraphObject,
	const FString& GraphPath,
	bool bRejectUnknownGraphFields,
	const FGraphParseContext& Context,
	FAssetDocumentGraphSpec& OutGraph,
	FAssetDocumentGraphParseResult& Result)
{
	if (bRejectUnknownGraphFields)
	{
		ValidateUnknownFields(
			GraphObject,
			GraphPath,
			GraphFields,
			UE_ARRAY_COUNT(GraphFields),
			TEXT("UnknownGraphField"),
			Result);
	}

	bool bParsed = true;
	bParsed &= TryGetStringField(GraphObject, TEXT("Id"), GraphPath, TEXT("MissingGraphId"), OutGraph.Id, Result);
	bParsed &= TryGetStringField(GraphObject, TEXT("Kind"), GraphPath, TEXT("MissingGraphKind"), OutGraph.Kind, Result);
	if (!OutGraph.Id.IsEmpty() && !IsSidecarId(OutGraph.Id))
	{
		Result.AddDiagnostic(
			TEXT("InvalidGraphId"),
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("Id")),
			TEXT("Graph id must match ^[A-Za-z_][A-Za-z0-9_-]*$."));
		bParsed = false;
	}
	if (!OutGraph.Kind.IsEmpty() && !IsKnownGraphKind(OutGraph.Kind))
	{
		Result.AddDiagnostic(
			TEXT("UnknownGraphKind"),
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("Kind")),
			FString::Printf(TEXT("Unknown graph kind '%s'."), *OutGraph.Kind));
		bParsed = false;
	}
	bParsed &= ParseSharedGraphFields(GraphObject, GraphPath, OutGraph, Result);
	OutGraph.Name = OutGraph.Id;
	bParsed &= TryGetStringFieldIfPresent(
		GraphObject,
		TEXT("Name"),
		GraphPath,
		TEXT("InvalidGraphName"),
		OutGraph.Name,
		Result);
	bParsed &= TryGetStringFieldIfPresent(
		GraphObject,
		TEXT("Schema"),
		GraphPath,
		TEXT("InvalidGraphSchema"),
		OutGraph.Schema,
		Result);
	bParsed &= TryGetStringFieldIfPresent(
		GraphObject,
		TEXT("GraphGuid"),
		GraphPath,
		TEXT("InvalidGraphGuid"),
		OutGraph.GraphGuid,
		Result);
	bParsed &= TryGetStringFieldIfPresent(
		GraphObject,
		TEXT("Category"),
		GraphPath,
		TEXT("InvalidGraphCategory"),
		OutGraph.Category,
		Result);
	bParsed &= TryGetStringFieldIfPresent(
		GraphObject,
		TEXT("Description"),
		GraphPath,
		TEXT("InvalidGraphDescription"),
		OutGraph.Description,
		Result);

	TSharedPtr<FJsonObject> Owner;
	if (TryGetNullableObjectFieldIfPresent(GraphObject, TEXT("Owner"), GraphPath, TEXT("InvalidGraphOwner"), Owner, Result) && Owner.IsValid())
	{
		FGraphParseContext OwnerContext = Context;
		OwnerContext.GraphStack.Add(TPair<FString, FString>(OutGraph.Kind, OutGraph.Id));
		ValidateGraphOwner(Owner, GraphPath, OwnerContext, Result);
		OutGraph.Owner = FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(Owner);
	}

	TSharedPtr<FJsonObject> Signature;
	if (TryGetObjectFieldIfPresent(GraphObject, TEXT("Signature"), GraphPath, TEXT("InvalidGraphSignature"), Signature, Result))
	{
		OutGraph.Signature = FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(Signature);
	}

	TSharedPtr<FJsonObject> Position;
	if (TryGetObjectFieldIfPresent(GraphObject, TEXT("Position"), GraphPath, TEXT("InvalidGraphPosition"), Position, Result))
	{
		OutGraph.Position = FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(Position);
	}

	TSharedPtr<FJsonObject> Evidence;
	if (TryGetObjectFieldIfPresent(GraphObject, TEXT("Evidence"), GraphPath, TEXT("InvalidGraphEvidence"), Evidence, Result))
	{
		OutGraph.Evidence = FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(Evidence);
	}

	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	if (TryGetRequiredArrayField(GraphObject, TEXT("Nodes"), GraphPath, TEXT("InvalidGraphRegionType"), Nodes, Result))
	{
		ParseNodeArray(*Nodes, GraphPath, OutGraph, Result);
	}
	else
	{
		bParsed = false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Links = nullptr;
	if (TryGetRequiredArrayField(GraphObject, TEXT("Links"), GraphPath, TEXT("InvalidGraphRegionType"), Links, Result))
	{
		ParseLinkArray(*Links, GraphPath, OutGraph, Result);
	}
	else
	{
		bParsed = false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Subgraphs = nullptr;
	if (TryGetRequiredArrayField(GraphObject, TEXT("Subgraphs"), GraphPath, TEXT("InvalidGraphRegionType"), Subgraphs, Result))
	{
		ParseSubgraphArray(*Subgraphs, GraphPath, bRejectUnknownGraphFields, Context, OutGraph, Result);
	}
	else
	{
		bParsed = false;
	}

	return bParsed;
}

void ValidateGraphIdentity(
	const FAssetDocumentGraphSpec& Graph,
	const FString& GraphPath,
	TMap<FString, FString>& GraphGuidPaths,
	TMap<FString, FString>& NodeGuidPaths,
	FAssetDocumentGraphParseResult& Result)
{
	if (!Graph.GraphGuid.IsEmpty())
	{
		const FString GuidPath = FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("GraphGuid"));
		if (!IsValidPersistentGuid(Graph.GraphGuid))
		{
			Result.AddDiagnostic(
				TEXT("InvalidGraphGuid"),
				GuidPath,
				TEXT("GraphGuid must be a non-zero 32-digit GUID."));
		}
		else if (const FString* ExistingPath = GraphGuidPaths.Find(Graph.GraphGuid))
		{
			Result.AddDiagnostic(
				TEXT("DuplicateGraphGuid"),
				GuidPath,
				FString::Printf(TEXT("GraphGuid is already owned by '%s'."), **ExistingPath));
		}
		else
		{
			GraphGuidPaths.Add(Graph.GraphGuid, GuidPath);
		}
	}

	for (const FAssetDocumentNodeSpec& Node : Graph.Nodes)
	{
		if (Node.NodeGuid.IsEmpty())
		{
			continue;
		}
		const FString NodePath = FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("Nodes")),
			Node.Id);
		const FString GuidPath = FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(NodePath, TEXT("NodeGuid"));
		if (!IsValidPersistentGuid(Node.NodeGuid))
		{
			Result.AddDiagnostic(
				TEXT("InvalidGraphNodeGuid"),
				GuidPath,
				TEXT("NodeGuid must be a non-zero 32-digit GUID."));
		}
		else if (const FString* ExistingPath = NodeGuidPaths.Find(Node.NodeGuid))
		{
			Result.AddDiagnostic(
				TEXT("DuplicateGraphNodeGuid"),
				GuidPath,
				FString::Printf(TEXT("NodeGuid is already owned by '%s'."), **ExistingPath));
		}
		else
		{
			NodeGuidPaths.Add(Node.NodeGuid, GuidPath);
		}
	}

	for (const FAssetDocumentGraphSpec& Subgraph : Graph.Subgraphs)
	{
		const FString SubgraphPath = FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("Subgraphs")),
			Subgraph.Id);
		ValidateGraphIdentity(Subgraph, SubgraphPath, GraphGuidPaths, NodeGuidPaths, Result);
	}
}

void ValidateGraphArrayIdentities(
	const TArray<FAssetDocumentGraphSpec>& Graphs,
	const FString& GraphsPath,
	bool bUseGraphId,
	FAssetDocumentGraphParseResult& Result)
{
	TMap<FString, FString> GraphGuidPaths;
	TMap<FString, FString> NodeGuidPaths;
	for (const FAssetDocumentGraphSpec& Graph : Graphs)
	{
		const FString Identity = bUseGraphId ? Graph.Id : Graph.Name;
		const FString GraphPath = FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphsPath, Identity);
		ValidateGraphIdentity(Graph, GraphPath, GraphGuidPaths, NodeGuidPaths, Result);
	}
}

FString MakeSemanticLinkId(const FAssetDocumentLinkSpec& Link)
{
	return FString::Printf(
		TEXT("%s:%s->%s:%s"),
		*Link.From.Node,
		*Link.From.Pin,
		*Link.To.Node,
		*Link.To.Pin);
}

const FAssetDocumentNodeSpec* FindGraphNode(
	const FAssetDocumentGraphSpec& Graph,
	const FString& NodeId)
{
	return Graph.Nodes.FindByPredicate(
		[&NodeId](const FAssetDocumentNodeSpec& Node)
		{
			return Node.Id == NodeId;
		});
}

bool NodeDeclaresPin(const FAssetDocumentNodeSpec& Node, const FString& PinId, bool& bOutHasAuthoredPins)
{
	bOutHasAuthoredPins = Node.Pins.IsValid();
	return Node.Pins.IsValid() && Node.Pins->HasField(PinId);
}

void ValidateLinkEndpointReference(
	const FAssetDocumentGraphSpec& Graph,
	const FAssetDocumentGraphEndpoint& Endpoint,
	const FString& EndpointPath,
	FAssetDocumentGraphParseResult& Result)
{
	const FAssetDocumentNodeSpec* Node = FindGraphNode(Graph, Endpoint.Node);
	if (!Node)
	{
		Result.AddDiagnostic(
			TEXT("UnknownGraphLinkNode"),
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(EndpointPath, TEXT("Node")),
			FString::Printf(TEXT("Link endpoint node '%s' does not exist in the graph."), *Endpoint.Node));
		return;
	}

	bool bHasAuthoredPins = false;
	const bool bDeclaresPin = NodeDeclaresPin(*Node, Endpoint.Pin, bHasAuthoredPins);
	if (bHasAuthoredPins && !bDeclaresPin)
	{
		Result.AddDiagnostic(
			TEXT("UnknownGraphLinkPin"),
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(EndpointPath, TEXT("Pin")),
			FString::Printf(
				TEXT("Link endpoint pin '%s' does not exist on node '%s'."),
				*Endpoint.Pin,
				*Endpoint.Node));
	}
}

bool TryGetOwnerAlias(
	const TSharedPtr<FJsonObject>& Owner,
	const TCHAR* Field,
	FString& OutValue)
{
	return Owner.IsValid() && Owner->TryGetStringField(Field, OutValue) && !OutValue.IsEmpty();
}

void MergeOwnerAlias(
	const FString& Candidate,
	const FString& CandidatePath,
	FString& InOutValue,
	FString& InOutPath,
	FAssetDocumentGraphParseResult& Result)
{
	if (Candidate.IsEmpty())
	{
		return;
	}
	if (InOutValue.IsEmpty())
	{
		InOutValue = Candidate;
		InOutPath = CandidatePath;
		return;
	}
	if (InOutValue != Candidate)
	{
		Result.AddDiagnostic(
			TEXT("ConflictingGraphOwnership"),
			CandidatePath,
			FString::Printf(
				TEXT("Ownership alias '%s' conflicts with '%s' at '%s'."),
				*Candidate,
				*InOutValue,
				*InOutPath));
	}
}

void ValidateGraphReferences(
	const FAssetDocumentGraphSpec& Graph,
	const FString& GraphPath,
	FAssetDocumentGraphParseResult& Result)
{
	const FString LinksPath =
		FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("Links"));
	for (const FAssetDocumentLinkSpec& Link : Graph.Links)
	{
		const FString LinkPath = FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(
			LinksPath,
			MakeSemanticLinkId(Link));
		ValidateLinkEndpointReference(
			Graph,
			Link.From,
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(LinkPath, TEXT("From")),
			Result);
		ValidateLinkEndpointReference(
			Graph,
			Link.To,
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(LinkPath, TEXT("To")),
			Result);
	}

	TMap<FString, const FAssetDocumentGraphSpec*> DirectSubgraphs;
	for (const FAssetDocumentGraphSpec& Subgraph : Graph.Subgraphs)
	{
		DirectSubgraphs.Add(Subgraph.Id, &Subgraph);
	}

	struct FSubgraphClaim
	{
		FString NodeId;
		FString Path;
	};
	TMap<FString, FSubgraphClaim> Claims;
	const FString NodesPath =
		FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("Nodes"));
	for (const FAssetDocumentNodeSpec& Node : Graph.Nodes)
	{
		if (!Node.SubgraphRefs.IsValid())
		{
			continue;
		}

		const FString RefsPath = FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(NodesPath, Node.Id),
			TEXT("SubgraphRefs"));
		for (const auto& Pair : Node.SubgraphRefs->Values)
		{
			const FString RefPath =
				FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(RefsPath, FString(*Pair.Key));
			if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::String || Pair.Value->AsString().IsEmpty())
			{
				Result.AddDiagnostic(
					TEXT("InvalidGraphSubgraphReference"),
					RefPath,
					TEXT("Subgraph reference must be a non-empty string stable id."));
				continue;
			}

			const FString SubgraphId = Pair.Value->AsString();
			if (!DirectSubgraphs.Contains(SubgraphId))
			{
				Result.AddDiagnostic(
					TEXT("UnknownGraphSubgraphReference"),
					RefPath,
					FString::Printf(
						TEXT("Subgraph reference '%s' is not a direct child of this graph."),
						*SubgraphId));
				continue;
			}

			if (const FSubgraphClaim* ExistingClaim = Claims.Find(SubgraphId))
			{
				if (ExistingClaim->NodeId != Node.Id)
				{
					Result.AddDiagnostic(
						TEXT("ConflictingGraphOwnership"),
						RefPath,
						FString::Printf(
							TEXT("Subgraph '%s' is already owned by node '%s'."),
							*SubgraphId,
							*ExistingClaim->NodeId));
				}
			}
			else
			{
				Claims.Add(SubgraphId, { Node.Id, RefPath });
			}
		}
	}

	const FString ParentGraphId = Graph.Id.IsEmpty() ? Graph.Name : Graph.Id;
	const FString SubgraphsPath =
		FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("Subgraphs"));
	for (const FAssetDocumentGraphSpec& Subgraph : Graph.Subgraphs)
	{
		const FString SubgraphPath =
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(SubgraphsPath, Subgraph.Id);
		const FString OwnerPath =
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(SubgraphPath, TEXT("Owner"));

		FString OwnerGraphId;
		FString OwnerGraphPath;
		FString Candidate;
		if (TryGetOwnerAlias(Subgraph.Owner, TEXT("ParentGraph"), Candidate))
		{
			MergeOwnerAlias(
				Candidate,
				FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(OwnerPath, TEXT("ParentGraph")),
				OwnerGraphId,
				OwnerGraphPath,
				Result);
		}
		Candidate.Reset();
		if (TryGetOwnerAlias(Subgraph.Owner, TEXT("Graph"), Candidate))
		{
			MergeOwnerAlias(
				Candidate,
				FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(OwnerPath, TEXT("Graph")),
				OwnerGraphId,
				OwnerGraphPath,
				Result);
		}
		if (!OwnerGraphId.IsEmpty() && OwnerGraphId != ParentGraphId)
		{
			Result.AddDiagnostic(
				TEXT("UnknownGraphOwnerReference"),
				OwnerGraphPath,
				FString::Printf(
					TEXT("Owner graph '%s' is not the direct parent graph '%s'."),
					*OwnerGraphId,
					*ParentGraphId));
		}

		FString OwnerNodeId;
		FString OwnerNodePath;
		const TCHAR* OwnerNodeFields[] = {
			TEXT("ParentNode"),
			TEXT("Node"),
			TEXT("OwnerNodeId"),
			TEXT("State"),
			TEXT("Transition")
		};
		for (const TCHAR* OwnerNodeField : OwnerNodeFields)
		{
			Candidate.Reset();
			if (TryGetOwnerAlias(Subgraph.Owner, OwnerNodeField, Candidate))
			{
				MergeOwnerAlias(
					Candidate,
					FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(OwnerPath, OwnerNodeField),
					OwnerNodeId,
					OwnerNodePath,
					Result);
			}
		}
		if (!Subgraph.OwnerNodeId.IsEmpty())
		{
			MergeOwnerAlias(
				Subgraph.OwnerNodeId,
				FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(SubgraphPath, TEXT("OwnerNodeId")),
				OwnerNodeId,
				OwnerNodePath,
				Result);
		}

		if (!OwnerNodeId.IsEmpty() && !FindGraphNode(Graph, OwnerNodeId))
		{
			Result.AddDiagnostic(
				TEXT("UnknownGraphOwnerReference"),
				OwnerNodePath,
				FString::Printf(
					TEXT("Owner node '%s' does not exist in parent graph '%s'."),
					*OwnerNodeId,
					*ParentGraphId));
		}

		if (const FSubgraphClaim* Claim = Claims.Find(Subgraph.Id))
		{
			if (!OwnerNodeId.IsEmpty() && Claim->NodeId != OwnerNodeId)
			{
				Result.AddDiagnostic(
					TEXT("ConflictingGraphOwnership"),
					Claim->Path,
					FString::Printf(
						TEXT("Subgraph '%s' is explicitly owned by node '%s', not claimant '%s'."),
						*Subgraph.Id,
						*OwnerNodeId,
						*Claim->NodeId));
			}
		}

		ValidateGraphReferences(Subgraph, SubgraphPath, Result);
	}
}

void ValidateGraphArrayReferences(
	const TArray<FAssetDocumentGraphSpec>& Graphs,
	const FString& GraphsPath,
	bool bUseGraphId,
	FAssetDocumentGraphParseResult& Result)
{
	for (const FAssetDocumentGraphSpec& Graph : Graphs)
	{
		const FString Identity = bUseGraphId ? Graph.Id : Graph.Name;
		const FString GraphPath = FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphsPath, Identity);
		ValidateGraphReferences(Graph, GraphPath, Result);
	}
}
}

FAssetDocumentGraphParseResult FAssetDocumentGraphParser::ParseGraphArray(
	const TArray<TSharedPtr<FJsonValue>>& GraphValues,
	const FAssetDocumentGraphParseOptions& Options)
{
	FAssetDocumentGraphParseResult Result;
	TSet<FString> GraphNames;

	for (int32 Index = 0; Index < GraphValues.Num(); ++Index)
	{
		const FString GraphPath = FAssetDocumentJsonRegionUtils::AppendJsonPointerIndex(Options.Path, Index);
		const TSharedPtr<FJsonObject> GraphObject = GraphValues[Index].IsValid() ? GraphValues[Index]->AsObject() : nullptr;
		if (!GraphObject.IsValid())
		{
			Result.AddDiagnostic(TEXT("InvalidGraphRegionType"), GraphPath, TEXT("Graph entry must be an object."));
			continue;
		}

		FAssetDocumentGraphParseOptions GraphOptions = Options;
		GraphOptions.Path = GraphPath;
		FAssetDocumentGraphParseResult GraphResult = ParseSingleGraph(GraphObject.ToSharedRef(), GraphOptions);
		Result.Diagnostics.Append(GraphResult.Diagnostics);
		if (GraphResult.Graphs.Num() != 1)
		{
			continue;
		}

		FAssetDocumentGraphSpec Graph = MoveTemp(GraphResult.Graphs[0]);
		if (GraphNames.Contains(Graph.Name))
		{
			Result.AddDiagnostic(
				TEXT("DuplicateGraphName"),
				FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("Name")),
				FString::Printf(TEXT("Duplicate graph name '%s'."), *Graph.Name));
		}
		else
		{
			GraphNames.Add(Graph.Name);
		}
		Result.Graphs.Add(MoveTemp(Graph));
	}
	ValidateGraphArrayIdentities(Result.Graphs, Options.Path, false, Result);
	ValidateGraphArrayReferences(Result.Graphs, Options.Path, false, Result);

	return Result;
}

FAssetDocumentGraphParseResult FAssetDocumentGraphParser::ParseSingleGraph(
	const TSharedRef<FJsonObject>& GraphObject,
	const FAssetDocumentGraphParseOptions& Options)
{
	FAssetDocumentGraphParseResult Result;
	if (Options.bRejectUnknownGraphFields)
	{
		ValidateUnknownFields(
			GraphObject,
			Options.Path,
			GraphFields,
			UE_ARRAY_COUNT(GraphFields),
			TEXT("UnknownGraphField"),
			Result);
	}

	FAssetDocumentGraphSpec Graph;
	TryGetStringFieldIfPresent(
		GraphObject,
		TEXT("Id"),
		Options.Path,
		TEXT("InvalidGraphId"),
		Graph.Id,
		Result);
	TryGetStringFieldIfPresent(
		GraphObject,
		TEXT("Kind"),
		Options.Path,
		TEXT("InvalidGraphKind"),
		Graph.Kind,
		Result);
	TryGetStringField(GraphObject, TEXT("Name"), Options.Path, TEXT("MissingGraphName"), Graph.Name, Result);
	TryGetStringField(GraphObject, TEXT("Schema"), Options.Path, TEXT("MissingGraphSchema"), Graph.Schema, Result);
	TryGetStringFieldIfPresent(
		GraphObject,
		TEXT("GraphGuid"),
		Options.Path,
		TEXT("InvalidGraphGuid"),
		Graph.GraphGuid,
		Result);
	TryGetStringFieldIfPresent(
		GraphObject,
		TEXT("Category"),
		Options.Path,
		TEXT("InvalidGraphCategory"),
		Graph.Category,
		Result);
	TryGetStringFieldIfPresent(
		GraphObject,
		TEXT("Description"),
		Options.Path,
		TEXT("InvalidGraphDescription"),
		Graph.Description,
		Result);
	ParseSharedGraphFields(GraphObject, Options.Path, Graph, Result);

	TSharedPtr<FJsonObject> Owner;
	if (TryGetNullableObjectFieldIfPresent(GraphObject, TEXT("Owner"), Options.Path, TEXT("InvalidGraphOwner"), Owner, Result) && Owner.IsValid())
	{
		Graph.Owner = FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(Owner);
	}

	TSharedPtr<FJsonObject> Signature;
	if (TryGetObjectFieldIfPresent(GraphObject, TEXT("Signature"), Options.Path, TEXT("InvalidGraphSignature"), Signature, Result))
	{
		Graph.Signature = FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(Signature);
	}

	TSharedPtr<FJsonObject> Position;
	if (TryGetObjectFieldIfPresent(GraphObject, TEXT("Position"), Options.Path, TEXT("InvalidGraphPosition"), Position, Result))
	{
		Graph.Position = FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(Position);
	}

	TSharedPtr<FJsonObject> Evidence;
	if (TryGetObjectFieldIfPresent(GraphObject, TEXT("Evidence"), Options.Path, TEXT("InvalidGraphEvidence"), Evidence, Result))
	{
		Graph.Evidence = FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(Evidence);
	}

	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	if (GraphObject->TryGetArrayField(TEXT("Nodes"), Nodes))
	{
		ParseNodeArray(*Nodes, Options.Path, Graph, Result);
	}
	else
	{
		Result.AddDiagnostic(
			TEXT("InvalidGraphRegionType"),
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Options.Path, TEXT("Nodes")),
			TEXT("Nodes must be an array."));
	}

	const TArray<TSharedPtr<FJsonValue>>* Links = nullptr;
	if (GraphObject->TryGetArrayField(TEXT("Links"), Links))
	{
		ParseLinkArray(*Links, Options.Path, Graph, Result);
	}
	else
	{
		Result.AddDiagnostic(
			TEXT("InvalidGraphRegionType"),
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Options.Path, TEXT("Links")),
			TEXT("Links must be an array."));
	}

	const TArray<TSharedPtr<FJsonValue>>* Subgraphs = nullptr;
	if (TryGetArrayFieldIfPresent(GraphObject, TEXT("Subgraphs"), Options.Path, TEXT("InvalidGraphRegionType"), Subgraphs, Result))
	{
		FGraphParseContext Context;
		ParseSubgraphArray(*Subgraphs, Options.Path, Options.bRejectUnknownGraphFields, Context, Graph, Result);
	}

	Result.Graphs.Add(MoveTemp(Graph));
	return Result;
}

FAssetDocumentGraphParseResult FAssetDocumentGraphParser::ParseGraphRegion(
	const TSharedRef<FJsonObject>& RegionObject,
	const FAssetDocumentGraphParseOptions& Options)
{
	FAssetDocumentGraphParseResult Result;
	if (Options.bRejectUnknownGraphFields)
	{
		ValidateUnknownFields(
			RegionObject,
			Options.Path,
			GraphRegionFields,
			UE_ARRAY_COUNT(GraphRegionFields),
			TEXT("UnknownGraphRegionField"),
			Result);
	}

	const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
	if (!TryGetRequiredArrayField(RegionObject, TEXT("Graphs"), Options.Path, TEXT("InvalidGraphRegionType"), Graphs, Result))
	{
		return Result;
	}

	TSet<FString> GraphIds;
	for (int32 Index = 0; Index < Graphs->Num(); ++Index)
	{
		const FString GraphPath = FAssetDocumentJsonRegionUtils::AppendJsonPointerIndex(
			FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Options.Path, TEXT("Graphs")),
			Index);
		const TSharedPtr<FJsonObject> GraphObject = (*Graphs)[Index].IsValid() ? (*Graphs)[Index]->AsObject() : nullptr;
		if (!GraphObject.IsValid())
		{
			Result.AddDiagnostic(TEXT("InvalidGraphRegionType"), GraphPath, TEXT("Graph entry must be an object."));
			continue;
		}

		FAssetDocumentGraphSpec Graph;
		FGraphParseContext Context;
		if (!ParseRecursiveGraph(
				GraphObject.ToSharedRef(),
				GraphPath,
				Options.bRejectUnknownGraphFields,
				Context,
				Graph,
				Result))
		{
			continue;
		}

		if (GraphIds.Contains(Graph.Id))
		{
			Result.AddDiagnostic(
				TEXT("DuplicateGraphId"),
				FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(GraphPath, TEXT("Id")),
				FString::Printf(TEXT("Duplicate graph id '%s'."), *Graph.Id));
		}
		else
		{
			GraphIds.Add(Graph.Id);
		}
		Result.Graphs.Add(MoveTemp(Graph));
	}
	ValidateGraphArrayIdentities(
		Result.Graphs,
		FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Options.Path, TEXT("Graphs")),
		true,
		Result);
	ValidateGraphArrayReferences(
		Result.Graphs,
		FAssetDocumentJsonRegionUtils::AppendJsonPointerToken(Options.Path, TEXT("Graphs")),
		true,
		Result);

	return Result;
}

TSharedRef<FJsonValue> FAssetDocumentGraphParser::WriteCanonicalGraphArray(const TArray<FAssetDocumentGraphSpec>& Graphs)
{
	TArray<FAssetDocumentGraphSpec> SortedGraphs = Graphs;
	SortedGraphs.Sort([](const FAssetDocumentGraphSpec& Left, const FAssetDocumentGraphSpec& Right)
	{
		return Left.Name < Right.Name;
	});

	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FAssetDocumentGraphSpec& Graph : SortedGraphs)
	{
		Values.Add(MakeShared<FJsonValueObject>(Graph.ToJsonObject()));
	}
	return MakeShared<FJsonValueArray>(MoveTemp(Values));
}

TSharedRef<FJsonObject> FAssetDocumentGraphParser::WriteCanonicalGraphRegion(const TArray<FAssetDocumentGraphSpec>& Graphs)
{
	TArray<FAssetDocumentGraphSpec> SortedGraphs = Graphs;
	SortedGraphs.Sort([](const FAssetDocumentGraphSpec& Left, const FAssetDocumentGraphSpec& Right)
	{
		return Left.Id < Right.Id;
	});

	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FAssetDocumentGraphSpec& Graph : SortedGraphs)
	{
		Values.Add(MakeShared<FJsonValueObject>(Graph.ToJsonObject()));
	}

	TSharedRef<FJsonObject> RegionObject = MakeShared<FJsonObject>();
	RegionObject->SetArrayField(TEXT("Graphs"), MoveTemp(Values));
	return RegionObject;
}
