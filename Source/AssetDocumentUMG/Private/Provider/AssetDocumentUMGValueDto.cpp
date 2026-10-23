// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/AssetDocumentUMGValueDto.h"

#include "UObject/SoftObjectPath.h"

namespace AssetDocumentUMGValueDtoPrivate
{
bool FailValueDto(FString& OutDiagnostic, const TCHAR* Detail)
{
	OutDiagnostic = FString::Printf(
		TEXT("AD.UMG.PROVIDER.INVALID_INVOCATION:%s"), Detail);
	return false;
}

const FAssetDocumentRuntimeValueDto* FindField(
	const FAssetDocumentRuntimeValueDto& Value,
	const TCHAR* Name)
{
	const int32 Index = Value.ObjectKeys.IndexOfByKey(Name);
	return Value.Elements.IsValidIndex(Index)
		? &Value.Elements[Index]
		: nullptr;
}

bool HasExactFields(
	const FAssetDocumentRuntimeValueDto& Value,
	std::initializer_list<const TCHAR*> Names,
	FString& OutDiagnostic)
{
	if (Value.Kind != EAssetDocumentRuntimeValueKind::Object
		|| Value.ObjectKeys.Num() != static_cast<int32>(Names.size())
		|| Value.Elements.Num() != Value.ObjectKeys.Num())
	{
		return FailValueDto(OutDiagnostic, TEXT("ObjectShape"));
	}
	TSet<FString> Expected;
	for (const TCHAR* Name : Names)
	{
		Expected.Add(Name);
	}
	TSet<FString> Actual(Value.ObjectKeys);
	return Expected.Difference(Actual).IsEmpty()
		&& Actual.Difference(Expected).IsEmpty()
		? true
		: FailValueDto(OutDiagnostic, TEXT("UnknownOrMissingField"));
}

bool ReadBool(
	const FAssetDocumentRuntimeValueDto& Value,
	const TCHAR* Name,
	bool& Out,
	FString& OutDiagnostic)
{
	const FAssetDocumentRuntimeValueDto* Field = FindField(Value, Name);
	if (Field == nullptr
		|| Field->Kind != EAssetDocumentRuntimeValueKind::Boolean)
	{
		return FailValueDto(OutDiagnostic, Name);
	}
	Out = Field->BooleanValue;
	return true;
}

bool ReadString(
	const FAssetDocumentRuntimeValueDto& Value,
	const TCHAR* Name,
	FString& Out,
	FString& OutDiagnostic,
	bool bAllowEmpty = false)
{
	const FAssetDocumentRuntimeValueDto* Field = FindField(Value, Name);
	if (Field == nullptr
		|| Field->Kind != EAssetDocumentRuntimeValueKind::String
		|| (!bAllowEmpty && Field->StringValue.IsEmpty()))
	{
		return FailValueDto(OutDiagnostic, Name);
	}
	Out = Field->StringValue;
	return true;
}

bool ReadName(
	const FAssetDocumentRuntimeValueDto& Value,
	const TCHAR* Name,
	FName& Out,
	FString& OutDiagnostic)
{
	FString Text;
	if (!ReadString(Value, Name, Text, OutDiagnostic)
		|| FName(*Text).IsNone())
	{
		return FailValueDto(OutDiagnostic, Name);
	}
	Out = FName(*Text);
	return !Out.IsNone();
}

bool ReadOptionalName(
	const FAssetDocumentRuntimeValueDto& Value,
	const TCHAR* Name,
	TOptional<FName>& Out,
	FString& OutDiagnostic)
{
	const FAssetDocumentRuntimeValueDto* Field = FindField(Value, Name);
	if (Field == nullptr)
	{
		return FailValueDto(OutDiagnostic, Name);
	}
	if (Field->Kind == EAssetDocumentRuntimeValueKind::Null)
	{
		Out.Reset();
		return true;
	}
	if (Field->Kind != EAssetDocumentRuntimeValueKind::String
		|| Field->StringValue.IsEmpty())
	{
		return FailValueDto(OutDiagnostic, Name);
	}
	Out = FName(*Field->StringValue);
	return !Out->IsNone();
}

bool ReadInteger(
	const FAssetDocumentRuntimeValueDto& Value,
	const TCHAR* Name,
	int32& Out,
	FString& OutDiagnostic)
{
	const FAssetDocumentRuntimeValueDto* Field = FindField(Value, Name);
	if (Field == nullptr
		|| Field->Kind != EAssetDocumentRuntimeValueKind::Integer
		|| Field->IntegerValue < MIN_int32
		|| Field->IntegerValue > MAX_int32)
	{
		return FailValueDto(OutDiagnostic, Name);
	}
	Out = static_cast<int32>(Field->IntegerValue);
	return true;
}

bool ReadOptionalInteger(
	const FAssetDocumentRuntimeValueDto& Value,
	const TCHAR* Name,
	TOptional<int32>& Out,
	FString& OutDiagnostic)
{
	const FAssetDocumentRuntimeValueDto* Field = FindField(Value, Name);
	if (Field == nullptr)
	{
		return FailValueDto(OutDiagnostic, Name);
	}
	if (Field->Kind == EAssetDocumentRuntimeValueKind::Null)
	{
		Out.Reset();
		return true;
	}
	int32 Number = 0;
	if (Field->Kind != EAssetDocumentRuntimeValueKind::Integer
		|| Field->IntegerValue < MIN_int32
		|| Field->IntegerValue > MAX_int32)
	{
		return FailValueDto(OutDiagnostic, Name);
	}
	Number = static_cast<int32>(Field->IntegerValue);
	Out = Number;
	return true;
}

bool ReadFloatValue(
	const FAssetDocumentRuntimeValueDto& Value,
	float& Out,
	FString& OutDiagnostic,
	const TCHAR* Detail)
{
	double Number = 0.0;
	if (Value.Kind == EAssetDocumentRuntimeValueKind::Number)
	{
		Number = Value.NumberValue;
	}
	else if (Value.Kind == EAssetDocumentRuntimeValueKind::Integer)
	{
		Number = static_cast<double>(Value.IntegerValue);
	}
	else
	{
		return FailValueDto(OutDiagnostic, Detail);
	}
	if (!FMath::IsFinite(Number)
		|| Number < -static_cast<double>(MAX_flt)
		|| Number > static_cast<double>(MAX_flt))
	{
		return FailValueDto(OutDiagnostic, Detail);
	}
	Out = static_cast<float>(Number);
	return true;
}

bool ReadGuid(
	const FAssetDocumentRuntimeValueDto& Value,
	const TCHAR* Name,
	FGuid& Out,
	FString& OutDiagnostic)
{
	FString Text;
	return ReadString(Value, Name, Text, OutDiagnostic)
		&& FGuid::Parse(Text, Out)
		&& Out.IsValid()
		? true
		: FailValueDto(OutDiagnostic, Name);
}

bool IsInterpolationSupported(const FString& Value)
{
	return Value == TEXT("auto")
		|| Value == TEXT("user")
		|| Value == TEXT("break")
		|| Value == TEXT("linear")
		|| Value == TEXT("constant")
		|| Value == TEXT("smart_auto");
}

bool DecodeKeys(
	const FAssetDocumentRuntimeValueDto& Value,
	TArray<FAssetDocumentUMGFloatKeyInput>& Out,
	FString& OutDiagnostic)
{
	if (Value.Kind != EAssetDocumentRuntimeValueKind::Array)
	{
		return FailValueDto(OutDiagnostic, TEXT("keys"));
	}
	TSet<int32> Frames;
	for (const FAssetDocumentRuntimeValueDto& KeyValue : Value.Elements)
	{
		if (!HasExactFields(
			KeyValue, {TEXT("frame"), TEXT("value"), TEXT("interpolation")},
			OutDiagnostic))
		{
			return false;
		}
		FAssetDocumentUMGFloatKeyInput& Key = Out.AddDefaulted_GetRef();
		const FAssetDocumentRuntimeValueDto* Numeric =
			FindField(KeyValue, TEXT("value"));
		if (!ReadInteger(
				KeyValue, TEXT("frame"), Key.Frame, OutDiagnostic)
			|| Numeric == nullptr
			|| !ReadFloatValue(
				*Numeric, Key.Value, OutDiagnostic, TEXT("value"))
			|| !ReadString(
				KeyValue, TEXT("interpolation"),
				Key.Interpolation, OutDiagnostic)
			|| !IsInterpolationSupported(Key.Interpolation)
			|| Frames.Contains(Key.Frame))
		{
			return FailValueDto(OutDiagnostic, TEXT("InvalidOrDuplicateKey"));
		}
		Frames.Add(Key.Frame);
	}
	return true;
}

bool DecodeChannel(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGFloatChannelInput& Out,
	FString& OutDiagnostic)
{
	if (!HasExactFields(
		Value, {TEXT("default_value"), TEXT("keys")}, OutDiagnostic))
	{
		return false;
	}
	const FAssetDocumentRuntimeValueDto* DefaultValue =
		FindField(Value, TEXT("default_value"));
	if (DefaultValue == nullptr)
	{
		return FailValueDto(OutDiagnostic, TEXT("default_value"));
	}
	if (DefaultValue->Kind == EAssetDocumentRuntimeValueKind::Null)
	{
		Out.DefaultValue.Reset();
	}
	else
	{
		float Number = 0.0f;
		if (!ReadFloatValue(
			*DefaultValue, Number, OutDiagnostic, TEXT("default_value")))
		{
			return false;
		}
		Out.DefaultValue = Number;
	}
	const FAssetDocumentRuntimeValueDto* Keys =
		FindField(Value, TEXT("keys"));
	return Keys != nullptr
		&& DecodeKeys(*Keys, Out.Keys, OutDiagnostic);
}

bool ReadTrackIdentity(
	const FAssetDocumentRuntimeValueDto& Value,
	FGuid& OutGuid,
	FName& OutPropertyName,
	FString& OutPropertyPath,
	int32& OutStart,
	int32& OutEnd,
	FString& OutDiagnostic)
{
	return ReadGuid(
			Value, TEXT("possessable_guid"), OutGuid, OutDiagnostic)
		&& ReadName(
			Value, TEXT("property_name"), OutPropertyName, OutDiagnostic)
		&& ReadString(
			Value, TEXT("property_path"), OutPropertyPath, OutDiagnostic)
		&& ReadInteger(
			Value, TEXT("section_start_frame"), OutStart, OutDiagnostic)
		&& ReadInteger(
			Value, TEXT("section_end_frame"), OutEnd, OutDiagnostic)
		&& OutStart < OutEnd
		? true
		: FailValueDto(OutDiagnostic, TEXT("InvalidRange"));
}
} // namespace AssetDocumentUMGValueDtoPrivate


bool DecodeWidgetTreeQueryInput(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGWidgetTreeQueryInput& Out,
	FString& OutDiagnostic)
{
	using namespace AssetDocumentUMGValueDtoPrivate;
	return HasExactFields(Value, {TEXT("include_properties")}, OutDiagnostic)
		&& ReadBool(
			Value, TEXT("include_properties"),
			Out.bIncludeProperties, OutDiagnostic);
}

bool DecodeAddSourceWidgetInput(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGAddSourceWidgetInput& Out,
	FString& OutDiagnostic)
{
	using namespace AssetDocumentUMGValueDtoPrivate;
	if (!HasExactFields(
		Value,
		{TEXT("requested_name"), TEXT("class_path"),
			TEXT("parent_name"), TEXT("child_index")},
		OutDiagnostic)
		|| !ReadName(
			Value, TEXT("requested_name"),
			Out.RequestedName, OutDiagnostic)
		|| !ReadString(
			Value, TEXT("class_path"), Out.ClassPath, OutDiagnostic)
		|| !FSoftClassPath(Out.ClassPath).IsValid()
		|| !ReadOptionalName(
			Value, TEXT("parent_name"),
			Out.ParentName, OutDiagnostic)
		|| !ReadOptionalInteger(
			Value, TEXT("child_index"),
			Out.ChildIndex, OutDiagnostic))
	{
		return FailValueDto(OutDiagnostic, TEXT("AddSourceWidget"));
	}
	return !Out.ChildIndex.IsSet() || Out.ChildIndex.GetValue() >= 0
		? true
		: FailValueDto(OutDiagnostic, TEXT("child_index"));
}

bool DecodeDeleteWidgetInput(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGDeleteWidgetInput& Out,
	FString& OutDiagnostic)
{
	using namespace AssetDocumentUMGValueDtoPrivate;
	return HasExactFields(Value, {TEXT("warning_policy")}, OutDiagnostic)
		&& ReadString(
			Value, TEXT("warning_policy"),
			Out.WarningPolicy, OutDiagnostic)
		&& Out.WarningPolicy == TEXT("delete_silently")
		? true
		: FailValueDto(OutDiagnostic, TEXT("warning_policy"));
}

bool DecodePanelChildAttachInput(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGPanelChildAttachInput& Out,
	FString& OutDiagnostic)
{
	using namespace AssetDocumentUMGValueDtoPrivate;
	return HasExactFields(Value, {TEXT("requested_index")}, OutDiagnostic)
		&& ReadInteger(
			Value, TEXT("requested_index"),
			Out.RequestedIndex, OutDiagnostic)
		&& Out.RequestedIndex >= 0
		? true
		: FailValueDto(OutDiagnostic, TEXT("requested_index"));
}

bool DecodeContentSetInput(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGContentSetInput& Out,
	FString& OutDiagnostic)
{
	using namespace AssetDocumentUMGValueDtoPrivate;
	return HasExactFields(Value, {TEXT("clear")}, OutDiagnostic)
		&& ReadBool(Value, TEXT("clear"), Out.bClear, OutDiagnostic);
}

bool DecodeNamedSlotSetInput(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGNamedSlotSetInput& Out,
	FString& OutDiagnostic)
{
	using namespace AssetDocumentUMGValueDtoPrivate;
	return HasExactFields(
			Value, {TEXT("slot_name"), TEXT("clear")}, OutDiagnostic)
		&& ReadName(
			Value, TEXT("slot_name"), Out.SlotName, OutDiagnostic)
		&& ReadBool(Value, TEXT("clear"), Out.bClear, OutDiagnostic);
}

bool DecodeDesignerBindingQueryInput(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGDesignerBindingQueryInput& Out,
	FString& OutDiagnostic)
{
	using namespace AssetDocumentUMGValueDtoPrivate;
	return HasExactFields(
			Value,
			{TEXT("widget_name"), TEXT("property_name")},
			OutDiagnostic)
		&& ReadOptionalName(
			Value, TEXT("widget_name"), Out.WidgetName, OutDiagnostic)
		&& ReadOptionalName(
			Value, TEXT("property_name"), Out.PropertyName, OutDiagnostic);
}

bool DecodeVariableNotifyInput(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGVariableNotifyInput& Out,
	FString& OutDiagnostic)
{
	using namespace AssetDocumentUMGValueDtoPrivate;
	if (Value.Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return FailValueDto(OutDiagnostic, TEXT("VariableNotify"));
	}
	FString Operation;
	if (!ReadString(
		Value, TEXT("operation"), Operation, OutDiagnostic))
	{
		return false;
	}
	if (Operation == TEXT("add"))
	{
		Out.Operation = EAssetDocumentUMGVariableNotifyOperation::Add;
		return HasExactFields(
				Value, {TEXT("operation"), TEXT("name")}, OutDiagnostic)
			&& ReadName(
				Value, TEXT("name"), Out.Name, OutDiagnostic);
	}
	if (Operation == TEXT("rename"))
	{
		Out.Operation = EAssetDocumentUMGVariableNotifyOperation::Rename;
		return HasExactFields(
				Value,
				{TEXT("operation"), TEXT("old_name"), TEXT("new_name")},
				OutDiagnostic)
			&& ReadName(
				Value, TEXT("old_name"), Out.OldName, OutDiagnostic)
			&& ReadName(
				Value, TEXT("new_name"), Out.NewName, OutDiagnostic)
			&& Out.OldName != Out.NewName;
	}
	if (Operation == TEXT("remove"))
	{
		Out.Operation = EAssetDocumentUMGVariableNotifyOperation::Remove;
		return HasExactFields(
				Value, {TEXT("operation"), TEXT("name")}, OutDiagnostic)
			&& ReadName(
				Value, TEXT("name"), Out.Name, OutDiagnostic);
	}
	return FailValueDto(OutDiagnostic, TEXT("operation"));
}

bool DecodeAnimationQueryInput(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGAnimationQueryInput& Out,
	FString& OutDiagnostic)
{
	using namespace AssetDocumentUMGValueDtoPrivate;
	return HasExactFields(Value, {TEXT("animation_name")}, OutDiagnostic)
		&& ReadOptionalName(
			Value, TEXT("animation_name"),
			Out.AnimationName, OutDiagnostic);
}

bool DecodePossessableAddInput(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGPossessableAddInput& Out,
	FString& OutDiagnostic)
{
	using namespace AssetDocumentUMGValueDtoPrivate;
	return HasExactFields(
			Value, {TEXT("name"), TEXT("class_path")}, OutDiagnostic)
		&& ReadString(Value, TEXT("name"), Out.Name, OutDiagnostic)
		&& ReadString(
			Value, TEXT("class_path"), Out.ClassPath, OutDiagnostic)
		&& FSoftClassPath(Out.ClassPath).IsValid();
}

bool DecodePossessableRemoveInput(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGPossessableRemoveInput& Out,
	FString& OutDiagnostic)
{
	using namespace AssetDocumentUMGValueDtoPrivate;
	return HasExactFields(
			Value, {TEXT("possessable_guid")}, OutDiagnostic)
		&& ReadGuid(
			Value, TEXT("possessable_guid"),
			Out.PossessableGuid, OutDiagnostic);
}

bool DecodeSequenceBindInput(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGSequenceBindingInput& Out,
	FString& OutDiagnostic)
{
	using namespace AssetDocumentUMGValueDtoPrivate;
	return HasExactFields(
			Value,
			{TEXT("possessable_guid"), TEXT("widget_name")},
			OutDiagnostic)
		&& ReadGuid(
			Value, TEXT("possessable_guid"),
			Out.PossessableGuid, OutDiagnostic)
		&& ReadName(
			Value, TEXT("widget_name"), Out.WidgetName, OutDiagnostic);
}

bool DecodeSequenceUnbindInput(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGSequenceBindingInput& Out,
	FString& OutDiagnostic)
{
	return DecodeSequenceBindInput(Value, Out, OutDiagnostic);
}

bool DecodeFloatTrackAddInput(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGFloatTrackAddInput& Out,
	FString& OutDiagnostic)
{
	using namespace AssetDocumentUMGValueDtoPrivate;
	if (!HasExactFields(
		Value,
		{TEXT("possessable_guid"), TEXT("property_name"),
			TEXT("property_path"), TEXT("section_start_frame"),
			TEXT("section_end_frame"), TEXT("default_value"),
			TEXT("keys")},
		OutDiagnostic)
		|| !ReadTrackIdentity(
			Value, Out.PossessableGuid, Out.PropertyName,
			Out.PropertyPath, Out.SectionStartFrame,
			Out.SectionEndFrame, OutDiagnostic))
	{
		return false;
	}
	FAssetDocumentRuntimeValueDto ChannelValue;
	ChannelValue.Kind = EAssetDocumentRuntimeValueKind::Object;
	ChannelValue.ObjectKeys = {
		TEXT("default_value"), TEXT("keys")};
	ChannelValue.Elements = {
		*FindField(Value, TEXT("default_value")),
		*FindField(Value, TEXT("keys"))};
	return DecodeChannel(ChannelValue, Out.Channel, OutDiagnostic);
}

bool DecodeTransformTrackAddInput(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGTransformTrackAddInput& Out,
	FString& OutDiagnostic)
{
	using namespace AssetDocumentUMGValueDtoPrivate;
	if (!HasExactFields(
		Value,
		{TEXT("possessable_guid"), TEXT("property_name"),
			TEXT("property_path"), TEXT("section_start_frame"),
			TEXT("section_end_frame"), TEXT("channels")},
		OutDiagnostic)
		|| !ReadTrackIdentity(
			Value, Out.PossessableGuid, Out.PropertyName,
			Out.PropertyPath, Out.SectionStartFrame,
			Out.SectionEndFrame, OutDiagnostic))
	{
		return false;
	}
	const FAssetDocumentRuntimeValueDto* Channels =
		FindField(Value, TEXT("channels"));
	if (Channels == nullptr
		|| Channels->Kind != EAssetDocumentRuntimeValueKind::Object
		|| Channels->Elements.Num() != Channels->ObjectKeys.Num()
		|| Channels->Elements.IsEmpty())
	{
		return FailValueDto(OutDiagnostic, TEXT("channels"));
	}
	static const TSet<FString> Supported = {
		TEXT("translation_x"), TEXT("translation_y"), TEXT("angle"),
		TEXT("scale_x"), TEXT("scale_y"),
		TEXT("shear_x"), TEXT("shear_y")};
	for (int32 Index = 0; Index < Channels->ObjectKeys.Num(); ++Index)
	{
		const FString& Name = Channels->ObjectKeys[Index];
		if (!Supported.Contains(Name) || Out.Channels.Contains(FName(*Name)))
		{
			return FailValueDto(OutDiagnostic, TEXT("UnsupportedOrDuplicateChannel"));
		}
		FAssetDocumentUMGFloatChannelInput Channel;
		if (!DecodeChannel(
			Channels->Elements[Index], Channel, OutDiagnostic))
		{
			return false;
		}
		Out.Channels.Add(FName(*Name), MoveTemp(Channel));
	}
	return true;
}

bool DecodeTrackSectionRemoveInput(
	const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentUMGTrackSectionRemoveInput& Out,
	FString& OutDiagnostic)
{
	using namespace AssetDocumentUMGValueDtoPrivate;
	FString Target;
	if (!HasExactFields(Value, {TEXT("target")}, OutDiagnostic)
		|| !ReadString(Value, TEXT("target"), Target, OutDiagnostic))
	{
		return false;
	}
	if (Target == TEXT("track"))
	{
		Out.Target = EAssetDocumentUMGTrackSectionRemoveTarget::Track;
		return true;
	}
	if (Target == TEXT("section"))
	{
		Out.Target = EAssetDocumentUMGTrackSectionRemoveTarget::Section;
		return true;
	}
	return FailValueDto(OutDiagnostic, TEXT("target"));
}

FAssetDocumentRuntimeValueDto MakeAssetDocumentUMGNull()
{
	return {};
}

FAssetDocumentRuntimeValueDto MakeAssetDocumentUMGBool(bool Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Boolean;
	Result.BooleanValue = Value;
	return Result;
}

FAssetDocumentRuntimeValueDto MakeAssetDocumentUMGInteger(int64 Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Integer;
	Result.IntegerValue = Value;
	return Result;
}

FAssetDocumentRuntimeValueDto MakeAssetDocumentUMGNumber(double Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Number;
	Result.NumberValue = Value;
	return Result;
}

FAssetDocumentRuntimeValueDto MakeAssetDocumentUMGString(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto MakeAssetDocumentUMGArray(
	TArray<FAssetDocumentRuntimeValueDto>&& Values)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Array;
	Result.Elements = MoveTemp(Values);
	return Result;
}

FAssetDocumentRuntimeValueDto MakeAssetDocumentUMGObject(
	std::initializer_list<TPair<FString, FAssetDocumentRuntimeValueDto>> Fields)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Object;
	for (const TPair<FString, FAssetDocumentRuntimeValueDto>& Field : Fields)
	{
		Result.ObjectKeys.Add(Field.Key);
		Result.Elements.Add(Field.Value);
	}
	return Result;
}
