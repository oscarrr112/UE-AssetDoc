// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// edit_level request parsing: the top-level fields, the op table, and the field edits
// spawn / duplicate / set share. Purely from the request (design §3.1 step 2).

#include "LevelEditTypes.h"

#include "Dom/JsonValue.h"
#include "LevelToolResult.h"

namespace UE::AssetDocumentLevel::Private
{
const TCHAR* const LevelActorFieldNames[8] = {
	TEXT("transform"), TEXT("label"), TEXT("folder"), TEXT("tags"), TEXT("properties"), TEXT("components"), TEXT("layers"),
	TEXT("data_layers")};

namespace
{
TMap<FString, FLevelEditOpParser>& OpTable()
{
	static TMap<FString, FLevelEditOpParser> Table;
	return Table;
}

FString Join(TConstArrayView<const TCHAR*> Names)
{
	TArray<FString> Out;
	for (const TCHAR* Name : Names)
	{
		Out.Add(Name);
	}
	return FString::Join(Out, TEXT(", "));
}

bool ReadOptionalString(const FJsonObject& Json, const TCHAR* Field, const FString& Pointer, bool bAllowEmpty,
	TOptional<FString>& Out, TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	if (!Json.HasField(Field))
	{
		return true;
	}
	FString Value;
	if (!Json.TryGetStringField(Field, Value) || (!bAllowEmpty && Value.IsEmpty()))
	{
		Diagnostics.Add(MakeInvalidRequest(Pointer + TEXT("/") + Field, FString::Printf(
			TEXT("'%s' must be a%s string."), Field, bAllowEmpty ? TEXT("") : TEXT(" non-empty"))));
		return false;
	}
	Out = Value;
	return true;
}

bool ParseTransform(const TSharedPtr<FJsonValue>& Json, const FString& Pointer, FLevelTransformEdit& Out,
	TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (!Json.IsValid() || !Json->TryGetObject(Object) || Object == nullptr || !Object->IsValid())
	{
		Diagnostics.Add(MakeInvalidRequest(Pointer, TEXT("'transform' is {\"location\": {X,Y,Z}, \"rotation\": {Pitch,Yaw,Roll}, "
			"\"scale\": {X,Y,Z}, \"space\": \"local\"|\"world\"}; every member is optional.")));
		return false;
	}
	Out.Pointer = Pointer;
	bool bOk = true;
	for (const auto& Member : (*Object)->Values)
	{
		const FString MemberPointer = Pointer + TEXT("/") + EscapePointerSegment(FString(*Member.Key));
		if (Member.Key == TEXT("space"))
		{
			FString Space;
			if (!Member.Value.IsValid() || !Member.Value->TryGetString(Space) || (Space != TEXT("local") && Space != TEXT("world")))
			{
				Diagnostics.Add(MakeInvalidRequest(MemberPointer, TEXT("'space' is \"local\" (default: relative to the attach parent, "
					"as the Details panel shows it) or \"world\".")));
				bOk = false;
			}
			Out.bWorld = Space == TEXT("world");
			continue;
		}
		int32 MemberIndex = INDEX_NONE;
		for (int32 Index = 0; Index < 3; ++Index)
		{
			if (Member.Key == FLevelTransformEdit::MemberField(static_cast<ELevelTransformMember>(Index)))
			{
				MemberIndex = Index;
			}
		}
		if (MemberIndex == INDEX_NONE)
		{
			Diagnostics.Add(MakeInvalidRequest(MemberPointer, FString::Printf(
				TEXT("Unknown transform member '%s' (location, rotation, scale, space)."), *Member.Key)));
			bOk = false;
			continue;
		}
		const bool bRotation = MemberIndex == static_cast<int32>(ELevelTransformMember::Rotation);
		const TCHAR* const Keys[3] = {bRotation ? TEXT("Pitch") : TEXT("X"), bRotation ? TEXT("Yaw") : TEXT("Y"),
			bRotation ? TEXT("Roll") : TEXT("Z")};
		const TSharedPtr<FJsonObject>* Vector = nullptr;
		if (!Member.Value.IsValid() || !Member.Value->TryGetObject(Vector) || Vector == nullptr || !Vector->IsValid())
		{
			Diagnostics.Add(MakeInvalidRequest(MemberPointer, FString::Printf(TEXT("'%s' is an object {\"%s\", \"%s\", \"%s\"} "
				"(members optional)."), *Member.Key, Keys[0], Keys[1], Keys[2])));
			bOk = false;
			continue;
		}
		for (const auto& Component : (*Vector)->Values)
		{
			double Number = 0.0;
			const bool bKnown = Component.Key == Keys[0] || Component.Key == Keys[1] || Component.Key == Keys[2];
			if (!bKnown || !Component.Value.IsValid() || !Component.Value->TryGetNumber(Number) || !FMath::IsFinite(Number))
			{
				Diagnostics.Add(MakeInvalidRequest(MemberPointer + TEXT("/") + EscapePointerSegment(FString(*Component.Key)), FString::Printf(
					TEXT("'%s' has the number members %s, %s and %s."), *Member.Key, Keys[0], Keys[1], Keys[2])));
				bOk = false;
			}
		}
		Out.Members[MemberIndex] = JsonToLevelValue(Member.Value);
	}
	return bOk;
}

bool ParseProperties(const TSharedPtr<FJsonValue>& Json, const FString& Pointer, TArray<FLevelPropertyEdit>& Out,
	TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (!Json.IsValid() || !Json->TryGetObject(Object) || Object == nullptr || !Object->IsValid())
	{
		Diagnostics.Add(MakeInvalidRequest(Pointer, TEXT("'properties' is an object {\"<PropertyName>\": <value>} in the "
			"AssetDocument value forms.")));
		return false;
	}
	for (const auto& Member : (*Object)->Values)
	{
		FLevelPropertyEdit& Edit = Out.AddDefaulted_GetRef();
		Edit.Name = Member.Key;
		Edit.Value = JsonToLevelValue(Member.Value);
		Edit.Pointer = Pointer + TEXT("/") + EscapePointerSegment(FString(*Member.Key));
	}
	return true;
}

// A whole list of non-empty, distinct strings ("layers", "data_layers").
void ParseStringList(const FJsonObject& Json, const TCHAR* Field, const FString& Pointer, const TCHAR* Meaning,
	TOptional<TArray<FString>>& Out, FString& OutPointer, TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	OutPointer = Pointer + TEXT("/") + Field;
	if (!Json.HasField(Field))
	{
		return;
	}
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	TArray<FString> List;
	bool bOk = Json.TryGetArrayField(Field, Values) && Values != nullptr;
	for (int32 Index = 0; bOk && Index < Values->Num(); ++Index)
	{
		FString Value;
		bOk = (*Values)[Index].IsValid() && (*Values)[Index]->TryGetString(Value) && !Value.IsEmpty() && !List.Contains(Value);
		List.Add(Value);
	}
	if (!bOk)
	{
		Diagnostics.Add(MakeInvalidRequest(OutPointer, FString::Printf(TEXT("'%s' is the whole list of %s: an array of distinct, "
			"non-empty strings ([] clears it)."), Field, Meaning)));
		return;
	}
	Out = MoveTemp(List);
}
}

FLevelEditOpRegistration::FLevelEditOpRegistration(const TCHAR* Kind, FLevelEditOpParser Parser)
{
	OpTable().Add(Kind, MoveTemp(Parser));
}

const FLevelEditOpParser* FindLevelEditOpParser(const FString& Kind)
{
	return OpTable().Find(Kind);
}

TArray<FString> GetLevelEditOpKinds()
{
	TArray<FString> Kinds;
	OpTable().GetKeys(Kinds);
	Kinds.Sort();
	return Kinds;
}

const TCHAR* FLevelTransformEdit::MemberField(ELevelTransformMember Member)
{
	switch (Member)
	{
	case ELevelTransformMember::Location: return TEXT("location");
	case ELevelTransformMember::Rotation: return TEXT("rotation");
	case ELevelTransformMember::Scale:
	default: return TEXT("scale");
	}
}

const TCHAR* FLevelTransformEdit::MemberProperty(ELevelTransformMember Member)
{
	switch (Member)
	{
	case ELevelTransformMember::Location: return TEXT("RelativeLocation");
	case ELevelTransformMember::Rotation: return TEXT("RelativeRotation");
	case ELevelTransformMember::Scale:
	default: return TEXT("RelativeScale3D");
	}
}

bool FLevelActorFieldEdits::HasAny() const
{
	return Transform.IsSet() || Label.IsSet() || Folder.IsSet() || Tags.IsSet() || !Properties.IsEmpty() || !Components.IsEmpty()
		|| Layers.IsSet() || DataLayers.IsSet();
}

TSharedRef<FJsonObject> FLevelChange::ToJson() const
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("op"), Op);
	Json->SetStringField(TEXT("kind"), Kind);
	Json->SetStringField(TEXT("object"), Object);
	if (!Actor.IsEmpty())
	{
		Json->SetStringField(TEXT("actor"), Actor);
	}
	if (!Component.IsEmpty())
	{
		Json->SetStringField(TEXT("component"), Component);
	}
	if (!Property.IsEmpty())
	{
		Json->SetStringField(TEXT("property"), Property);
	}
	if (Kind == TEXT("Replace"))
	{
		Json->SetField(TEXT("before"), Before.IsValid() ? Before : MakeShared<FJsonValueNull>());
		Json->SetField(TEXT("after"), After.IsValid() ? After : MakeShared<FJsonValueNull>());
	}
	if (Extra.IsValid())
	{
		for (const auto& Field : Extra->Values)
		{
			Json->SetField(Field.Key, Field.Value);
		}
	}
	return Json;
}

FAssetDocumentDiagnostic MakeInvalidRequest(const FString& Path, const FString& Message)
{
	return MakeLevelDiagnostic(Path, TEXT("Level.InvalidRequest"), Message);
}

bool CheckLevelOpFields(const FJsonObject& Json, const FString& Pointer, TConstArrayView<const TCHAR*> Allowed,
	TArray<FAssetDocumentDiagnostic>& OutDiagnostics)
{
	bool bOk = true;
	for (const auto& Field : Json.Values)
	{
		if (Field.Key == TEXT("op"))
		{
			continue;
		}
		bool bKnown = false;
		for (const TCHAR* Name : Allowed)
		{
			bKnown |= Field.Key == Name;
		}
		if (bKnown)
		{
			continue;
		}
		const FString FieldPointer = Pointer + TEXT("/") + EscapePointerSegment(FString(*Field.Key));
		OutDiagnostics.Add(MakeInvalidRequest(FieldPointer, FString::Printf(TEXT("Unknown field '%s' for this op. Fields: op, %s."),
			*Field.Key, *Join(Allowed))));
		bOk = false;
	}
	return bOk;
}

bool ParseLevelActorRefField(const FJsonObject& Json, const TCHAR* Field, const FString& Pointer, bool bRequired,
	FActorRef& OutRef, TArray<FAssetDocumentDiagnostic>& OutDiagnostics)
{
	const FString FieldPointer = Pointer + TEXT("/") + Field;
	if (!Json.HasField(Field))
	{
		if (bRequired)
		{
			OutDiagnostics.Add(MakeInvalidRequest(FieldPointer, FString::Printf(TEXT("'%s' (an actor reference) is required."), Field)));
		}
		return !bRequired;
	}
	FString Error;
	if (!ParseActorRef(Json.TryGetField(Field), OutRef, Error))
	{
		OutDiagnostics.Add(MakeInvalidRequest(FieldPointer, Error));
		return false;
	}
	return true;
}

bool ParseLevelBatchIdField(const FJsonObject& Json, const FString& Pointer, FString& OutId,
	TArray<FAssetDocumentDiagnostic>& OutDiagnostics)
{
	if (!Json.HasField(TEXT("id")))
	{
		return true;
	}
	if (!Json.TryGetStringField(TEXT("id"), OutId) || OutId.IsEmpty() || OutId.StartsWith(TEXT("@")) || OutId.Contains(TEXT("/")))
	{
		OutDiagnostics.Add(MakeInvalidRequest(Pointer + TEXT("/id"), TEXT("'id' is a non-empty name without '@' or '/'; later ops "
			"refer to the actor as \"@<id>\".")));
		OutId.Reset();
		return false;
	}
	return true;
}

bool ParseLevelVector(const TSharedPtr<FJsonValue>& Json, const FString& Pointer, FVector& OutVector,
	TArray<FAssetDocumentDiagnostic>& OutDiagnostics)
{
	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (!Json.IsValid() || !Json->TryGetObject(Object) || Object == nullptr || !Object->IsValid())
	{
		OutDiagnostics.Add(MakeInvalidRequest(Pointer, TEXT("expected a vector {\"X\": .., \"Y\": .., \"Z\": ..}.")));
		return false;
	}
	OutVector = FVector::ZeroVector;
	for (const auto& Member : (*Object)->Values)
	{
		double Number = 0.0;
		if (!Member.Value.IsValid() || !Member.Value->TryGetNumber(Number) || !FMath::IsFinite(Number)
			|| (Member.Key != TEXT("X") && Member.Key != TEXT("Y") && Member.Key != TEXT("Z")))
		{
			OutDiagnostics.Add(MakeInvalidRequest(Pointer + TEXT("/") + EscapePointerSegment(FString(*Member.Key)),
				TEXT("a vector has the number members X, Y and Z.")));
			return false;
		}
		(Member.Key == TEXT("X") ? OutVector.X : Member.Key == TEXT("Y") ? OutVector.Y : OutVector.Z) = Number;
	}
	return true;
}

bool ParseLevelActorFieldEdits(const FJsonObject& Json, const FString& Pointer, FLevelActorFieldEdits& OutEdits,
	TArray<FAssetDocumentDiagnostic>& OutDiagnostics)
{
	const int32 Before = OutDiagnostics.Num();
	if (Json.HasField(TEXT("transform")))
	{
		FLevelTransformEdit Transform;
		if (ParseTransform(Json.TryGetField(TEXT("transform")), Pointer + TEXT("/transform"), Transform, OutDiagnostics))
		{
			OutEdits.Transform = MoveTemp(Transform);
		}
	}
	OutEdits.LabelPointer = Pointer + TEXT("/label");
	if (ReadOptionalString(Json, TEXT("label"), Pointer, false, OutEdits.Label, OutDiagnostics) && OutEdits.Label.IsSet())
	{
		const FString& Label = OutEdits.Label.GetValue();
		if (Label.TrimStartAndEnd() != Label)
		{
			OutDiagnostics.Add(MakeInvalidRequest(OutEdits.LabelPointer, TEXT("A label has no leading or trailing spaces (the "
				"Outliner trims them).")));
		}
	}
	OutEdits.FolderPointer = Pointer + TEXT("/folder");
	if (ReadOptionalString(Json, TEXT("folder"), Pointer, true, OutEdits.Folder, OutDiagnostics) && OutEdits.Folder.IsSet())
	{
		FString Folder = OutEdits.Folder.GetValue();
		Folder.RemoveFromEnd(TEXT("/"));
		if (Folder.StartsWith(TEXT("/")) || Folder.Contains(TEXT("//")))
		{
			OutDiagnostics.Add(MakeInvalidRequest(OutEdits.FolderPointer, TEXT("A folder is an Outliner folder path such as "
				"\"Room/Lights\" (no leading '/'); \"\" is the root.")));
		}
		OutEdits.Folder = Folder;
	}
	OutEdits.TagsPointer = Pointer + TEXT("/tags");
	if (Json.HasField(TEXT("tags")))
	{
		const TArray<TSharedPtr<FJsonValue>>* Tags = nullptr;
		bool bOk = Json.TryGetArrayField(TEXT("tags"), Tags) && Tags != nullptr;
		for (int32 Index = 0; bOk && Index < Tags->Num(); ++Index)
		{
			FString Tag;
			bOk = (*Tags)[Index].IsValid() && (*Tags)[Index]->TryGetString(Tag) && !Tag.IsEmpty();
		}
		if (!bOk)
		{
			OutDiagnostics.Add(MakeInvalidRequest(OutEdits.TagsPointer, TEXT("'tags' is the whole list of actor tags: an array of "
				"non-empty strings ([] clears them).")));
		}
		else
		{
			OutEdits.Tags = JsonToLevelValue(Json.TryGetField(TEXT("tags")));
		}
	}
	if (Json.HasField(TEXT("properties")))
	{
		ParseProperties(Json.TryGetField(TEXT("properties")), Pointer + TEXT("/properties"), OutEdits.Properties, OutDiagnostics);
	}
	ParseStringList(Json, TEXT("layers"), Pointer, TEXT("layer names (non-partitioned levels)"), OutEdits.Layers,
		OutEdits.LayersPointer, OutDiagnostics);
	ParseStringList(Json, TEXT("data_layers"), Pointer, TEXT("data layers (World Partition; data layer asset paths or instance "
		"names)"), OutEdits.DataLayers, OutEdits.DataLayersPointer, OutDiagnostics);
	if (Json.HasField(TEXT("components")))
	{
		const TSharedPtr<FJsonObject>* Components = nullptr;
		if (!Json.TryGetObjectField(TEXT("components"), Components) || Components == nullptr)
		{
			OutDiagnostics.Add(MakeInvalidRequest(Pointer + TEXT("/components"), TEXT("'components' is "
				"{\"<ComponentName>\": {\"properties\": {...}}} (names from query_level detail \"components\").")));
		}
		else
		{
			for (const auto& Entry : (*Components)->Values)
			{
				const FString EntryPointer = Pointer + TEXT("/components/") + EscapePointerSegment(FString(*Entry.Key));
				const TSharedPtr<FJsonObject>* Body = nullptr;
				if (!Entry.Value.IsValid() || !Entry.Value->TryGetObject(Body) || Body == nullptr || !(*Body)->HasField(TEXT("properties"))
					|| (*Body)->Values.Num() != 1)
				{
					OutDiagnostics.Add(MakeInvalidRequest(EntryPointer, TEXT("A component entry is {\"properties\": {...}}.")));
					continue;
				}
				FLevelComponentEdit& Component = OutEdits.Components.AddDefaulted_GetRef();
				Component.Name = Entry.Key;
				Component.Pointer = EntryPointer;
				ParseProperties((*Body)->TryGetField(TEXT("properties")), EntryPointer + TEXT("/properties"), Component.Properties,
					OutDiagnostics);
			}
		}
	}
	return OutDiagnostics.Num() == Before;
}

bool ParseLevelEditRequest(const FJsonObject& Json, FLevelEditRequest& OutRequest, TArray<FAssetDocumentDiagnostic>& OutDiagnostics)
{
	static const TCHAR* const TopFields[] = {TEXT("description"), TEXT("dry_run"), TEXT("save"), TEXT("ops"), TEXT("after")};
	for (const auto& Field : Json.Values)
	{
		bool bKnown = false;
		for (const TCHAR* Name : TopFields)
		{
			bKnown |= Field.Key == Name;
		}
		if (!bKnown)
		{
			OutDiagnostics.Add(MakeInvalidRequest(TEXT("/") + EscapePointerSegment(FString(*Field.Key)), FString::Printf(
				TEXT("Unknown edit_level field '%s'. Fields: description, dry_run, save, ops, after."), *Field.Key)));
		}
	}
	if (Json.HasField(TEXT("description")) && (!Json.TryGetStringField(TEXT("description"), OutRequest.Description)
		|| OutRequest.Description.TrimStartAndEnd().IsEmpty()))
	{
		OutDiagnostics.Add(MakeInvalidRequest(TEXT("/description"), TEXT("'description' (the Undo entry title) is a non-empty string.")));
	}
	if (Json.HasField(TEXT("dry_run")) && !Json.TryGetBoolField(TEXT("dry_run"), OutRequest.bDryRun))
	{
		OutDiagnostics.Add(MakeInvalidRequest(TEXT("/dry_run"), TEXT("'dry_run' must be true or false.")));
	}
	if (Json.HasField(TEXT("save")) && !Json.TryGetBoolField(TEXT("save"), OutRequest.bSave))
	{
		OutDiagnostics.Add(MakeInvalidRequest(TEXT("/save"), TEXT("'save' must be true or false.")));
	}

	const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
	if (!Json.TryGetArrayField(TEXT("ops"), Ops) || Ops == nullptr || Ops->IsEmpty())
	{
		OutDiagnostics.Add(MakeInvalidRequest(TEXT("/ops"), FString::Printf(TEXT("'ops' is a non-empty array of operations "
			"({\"op\": <kind>, ...}; kinds: %s)."), *FString::Join(GetLevelEditOpKinds(), TEXT(", ")))));
	}
	else if (Ops->Num() > MaximumLevelEditOps)
	{
		OutDiagnostics.Add(MakeLevelDiagnostic(TEXT("/ops"), TEXT("Level.TooManyOps"), FString::Printf(
			TEXT("A batch has at most %d ops (this one has %d); split it into several edit_level calls."), MaximumLevelEditOps, Ops->Num())));
	}
	else
	{
		TMap<FString, FString> Ids;
		for (int32 Index = 0; Index < Ops->Num(); ++Index)
		{
			const FString Pointer = FString::Printf(TEXT("/ops/%d"), Index);
			const TSharedPtr<FJsonObject>* OpJson = nullptr;
			FString Kind;
			if (!(*Ops)[Index].IsValid() || !(*Ops)[Index]->TryGetObject(OpJson) || OpJson == nullptr || !OpJson->IsValid()
				|| !(*OpJson)->TryGetStringField(TEXT("op"), Kind))
			{
				OutDiagnostics.Add(MakeInvalidRequest(Pointer, TEXT("An op is an object with \"op\": the op kind.")));
				continue;
			}
			const FLevelEditOpParser* Parser = FindLevelEditOpParser(Kind);
			if (Parser == nullptr)
			{
				OutDiagnostics.Add(MakeLevelDiagnostic(Pointer + TEXT("/op"), TEXT("Level.UnknownOp"), FString::Printf(
					TEXT("Unknown op '%s'. edit_level ops: %s."), *Kind,
					*FString::Join(GetLevelEditOpKinds(), TEXT(", ")))));
				continue;
			}
			TUniquePtr<FLevelEditOp> Op = (*Parser)(**OpJson, Pointer, OutDiagnostics);
			if (!Op.IsValid())
			{
				continue;
			}
			Op->Index = Index;
			Op->Pointer = Pointer;
			if (!Op->BatchId.IsEmpty())
			{
				if (const FString* Previous = Ids.Find(Op->BatchId))
				{
					OutDiagnostics.Add(MakeLevelDiagnostic(Pointer + TEXT("/id"), TEXT("Level.DuplicateId"), FString::Printf(
						TEXT("Batch id '%s' is already defined by %s; ids are unique within one request."), *Op->BatchId, **Previous)));
				}
				else
				{
					Ids.Add(Op->BatchId, Pointer);
				}
			}
			OutRequest.Ops.Add(MoveTemp(Op));
		}
	}

	if (Json.HasField(TEXT("after")))
	{
		const TSharedPtr<FJsonObject>* After = nullptr;
		if (!Json.TryGetObjectField(TEXT("after"), After) || After == nullptr)
		{
			OutDiagnostics.Add(MakeInvalidRequest(TEXT("/after"), TEXT("'after' is {\"select\": [actor refs], \"focus\": [actor refs]}.")));
		}
		else
		{
			for (const auto& Field : (*After)->Values)
			{
				const FString FieldPointer = TEXT("/after/") + EscapePointerSegment(FString(*Field.Key));
				const bool bSelect = Field.Key == TEXT("select");
				if (!bSelect && Field.Key != TEXT("focus"))
				{
					OutDiagnostics.Add(MakeInvalidRequest(FieldPointer, TEXT("'after' has the fields select and focus.")));
					continue;
				}
				const TArray<TSharedPtr<FJsonValue>>* Refs = nullptr;
				if (!Field.Value.IsValid() || !Field.Value->TryGetArray(Refs) || Refs == nullptr)
				{
					OutDiagnostics.Add(MakeInvalidRequest(FieldPointer, TEXT("an array of actor references.")));
					continue;
				}
				OutRequest.bHasSelect |= bSelect;
				for (int32 Index = 0; Index < Refs->Num(); ++Index)
				{
					FActorRef Ref;
					FString Error;
					if (!ParseActorRef((*Refs)[Index], Ref, Error))
					{
						OutDiagnostics.Add(MakeInvalidRequest(FString::Printf(TEXT("%s/%d"), *FieldPointer, Index), Error));
						continue;
					}
					(bSelect ? OutRequest.Select : OutRequest.Focus).Add(MoveTemp(Ref));
				}
			}
		}
	}
	return OutDiagnostics.IsEmpty();
}
}
