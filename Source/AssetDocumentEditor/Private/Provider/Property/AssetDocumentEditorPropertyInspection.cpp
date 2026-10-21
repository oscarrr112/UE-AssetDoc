// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Property/AssetDocumentEditorPropertyInspection.h"

#include "Provider/AssetDocumentEditorTypedValueCodec.h"
#include "Provider/AssetDocumentEditorValueDto.h"
#include "Provider/Property/AssetDocumentEditorDeclaredPropertyAccess.h"
#include "UObject/Class.h"
#include "UObject/PropertyAccessUtil.h"
#include "UObject/UnrealType.h"

namespace
{
struct FAssetDocumentEditorPropertyInspectionRow
{
	FString Name;
	FString DeclaringClass;
	FAssetDocumentRuntimeValueDto Value;
};

struct FAssetDocumentEditorPropertyInspectionEncodedValues
{
	FString CodecId;
	FAssetDocumentRuntimeValueDto CurrentValue;
	FAssetDocumentRuntimeValueDto DefaultValue;
};

FAssetDocumentRuntimeValueDto MakePropertyInspectionBooleanValue(bool bValue)
{
	FAssetDocumentRuntimeValueDto Value;
	Value.Kind = EAssetDocumentRuntimeValueKind::Boolean;
	Value.BooleanValue = bValue;
	return Value;
}

FAssetDocumentRuntimeValueDto MakePropertyInspectionStringValue(FString String)
{
	FAssetDocumentRuntimeValueDto Value;
	Value.Kind = EAssetDocumentRuntimeValueKind::String;
	Value.StringValue = MoveTemp(String);
	return Value;
}

FAssetDocumentEditorPropertyInspectionResult MakePropertyInspectionFailure(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentEditorPropertyInspectionResult Result;
	Result.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	FAssetDocumentRuntimeDiagnostic& Diagnostic =
		Result.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/PropertyInspection");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Result;
}

// Editable properties flagged transient that their class saves itself (custom Serialize), so the value is
// persisted although property serialization skips it: "Transient" would read as "not saved".
bool IsPropertyInspectionSavedByAsset(const FProperty& Property)
{
	struct FSavedByAsset
	{
		const TCHAR* ClassPath;
		const TCHAR* PropertyName;
	};
	static const FSavedByAsset SavedByAsset[] = {
		{TEXT("/Script/Engine.SkeletalMesh"), TEXT("Materials")},
		{TEXT("/Script/Engine.StaticMesh"), TEXT("BodySetup")},
		{TEXT("/Script/Engine.ParticleSystemReplay"), TEXT("Frames")},
	};
	const UClass* Owner = Property.GetOwnerClass();
	if (Owner == nullptr || !Property.HasAllPropertyFlags(CPF_Edit | CPF_Transient))
	{
		return false;
	}
	const FString OwnerPath = Owner->GetPathName();
	for (const FSavedByAsset& Entry : SavedByAsset)
	{
		if (OwnerPath == Entry.ClassPath && Property.GetName() == Entry.PropertyName)
		{
			return true;
		}
	}
	return false;
}

const TCHAR* GetPropertyInspectionFlagRejectionReason(const FProperty& Property)
{
	struct FFlagRejection
	{
		EPropertyFlags Flag;
		const TCHAR* Reason;
	};
	if (!Property.HasAnyPropertyFlags(CPF_Edit))
	{
		return TEXT("NonEditable");
	}
	if (IsPropertyInspectionSavedByAsset(Property))
	{
		return TEXT("SavedByAsset");
	}
	const FFlagRejection Rejections[] = {
		{CPF_Transient, TEXT("Transient")},
		{CPF_DuplicateTransient, TEXT("DuplicateTransient")},
		{CPF_Deprecated, TEXT("Deprecated")},
		{CPF_SkipSerialization, TEXT("SkipSerialization")},
	};
	for (const FFlagRejection& Rejection : Rejections)
	{
		if (Property.HasAnyPropertyFlags(Rejection.Flag))
		{
			return Rejection.Reason;
		}
	}
	return nullptr;
}

const TCHAR* GetPropertyInspectionRejectionReason(const FProperty& Property)
{
	// Value shape support (text, references, instanced subobjects/structs
	// included) is decided by the typed value codec when the row is encoded.
	return GetPropertyInspectionFlagRejectionReason(Property);
}

bool TryReadPropertyInspectionValue(
	UObject& Object,
	FProperty& Property,
	FAssetDocumentEditorScopedPropertyValue& OutValue,
	bool bDeclared)
{
	if (bDeclared)
	{
		return OutValue.IsValid()
			&& GetAssetDocumentEditorDeclaredPropertyValue(Object, Property, OutValue.GetMemory());
	}
	return ReadAssetDocumentEditorObjectPropertyValue(Object, Property, OutValue);
}

bool TryEncodePropertyInspectionValue(
	FProperty& Property,
	const FAssetDocumentEditorScopedPropertyValue& NativeValue,
	FString& OutCodecId,
	FAssetDocumentRuntimeValueDto& OutValue,
	bool& bOutUnsupportedCodec)
{
	bOutUnsupportedCodec = false;
	const FAssetDocumentEditorTypedValueCodecResult Codec =
		InspectAssetDocumentEditorTypedValueProperty(Property);
	if (!Codec.bSucceeded)
	{
		bOutUnsupportedCodec = true;
		return false;
	}
	const FAssetDocumentEditorTypedValueCodecResult Encoded =
		EncodeAssetDocumentEditorTypedValue(NativeValue, OutValue);
	if (!Encoded.bSucceeded || Encoded.CodecId != Codec.CodecId)
	{
		OutValue = {};
		return false;
	}
	OutCodecId = Codec.CodecId;
	return true;
}

const TCHAR* GetPropertyInspectionEncodeFailureReason(bool bUnsupportedCodec)
{
	return bUnsupportedCodec ? TEXT("UnsupportedCodec") : TEXT("EncodeFailed");
}

bool TryReadPropertyInspectionValuePair(
	UObject& CurrentObject,
	UObject& DefaultObject,
	FProperty& Property,
	FAssetDocumentEditorScopedPropertyValue& OutCurrent,
	FAssetDocumentEditorScopedPropertyValue& OutDefault,
	const TCHAR*& OutReason,
	bool bDeclared)
{
	if (!TryReadPropertyInspectionValue(CurrentObject, Property, OutCurrent, bDeclared))
	{
		OutReason = TEXT("CurrentReadFailed");
		return false;
	}
	if (!TryReadPropertyInspectionValue(DefaultObject, Property, OutDefault, bDeclared))
	{
		OutReason = TEXT("DefaultReadFailed");
		return false;
	}
	return true;
}

bool TryEncodePropertyInspectionValuePair(
	FProperty& Property,
	const FAssetDocumentEditorScopedPropertyValue& Current,
	const FAssetDocumentEditorScopedPropertyValue& Default,
	FAssetDocumentEditorPropertyInspectionEncodedValues& OutValues,
	const TCHAR*& OutReason)
{
	bool bUnsupportedCodec = false;
	if (!TryEncodePropertyInspectionValue(
		Property, Current, OutValues.CodecId, OutValues.CurrentValue, bUnsupportedCodec))
	{
		OutReason = GetPropertyInspectionEncodeFailureReason(bUnsupportedCodec);
		return false;
	}
	FString DefaultCodecId;
	if (!TryEncodePropertyInspectionValue(
		Property, Default, DefaultCodecId, OutValues.DefaultValue, bUnsupportedCodec))
	{
		OutReason = GetPropertyInspectionEncodeFailureReason(bUnsupportedCodec);
		return false;
	}
	if (OutValues.CodecId != DefaultCodecId)
	{
		OutReason = TEXT("UnsupportedCodec");
		return false;
	}
	return true;
}

bool TryBuildPropertyInspectionAdmittedValueRow(
	FProperty& Property,
	UObject& CurrentObject,
	FAssetDocumentEditorPropertyInspectionEncodedValues&& Values,
	FAssetDocumentEditorPropertyInspectionRow& OutRow,
	const TCHAR*& OutReason,
	bool bDeclared)
{
	const UClass* OwnerClass = Property.GetOwnerClass();
	if (OwnerClass == nullptr)
	{
		OutReason = TEXT("UnsupportedCodec");
		return false;
	}
	FString DiagnosticCode;
	const TArray<FAssetDocumentEditorValueField> Fields = {
		{TEXT("name"), MakePropertyInspectionStringValue(Property.GetName())},
		{TEXT("declaring_class"),
			MakePropertyInspectionStringValue(OwnerClass->GetPathName())},
		{TEXT("ue_cpp_type"),
			MakePropertyInspectionStringValue(Property.GetCPPType())},
		{TEXT("codec_id"), MakePropertyInspectionStringValue(MoveTemp(Values.CodecId))},
		{TEXT("writable"), MakePropertyInspectionBooleanValue(bDeclared ||
			PropertyAccessUtil::CanSetPropertyValue(
				&Property, PropertyAccessUtil::EditorReadOnlyFlags, CurrentObject.IsTemplate())
				== EPropertyAccessResultFlags::Success
			|| IsAssetDocumentEditorInPlaceInstancedProperty(Property))},
		{TEXT("current_value"), MoveTemp(Values.CurrentValue)},
		{TEXT("default_value"), MoveTemp(Values.DefaultValue)},
	};
	if (!TryBuildAssetDocumentEditorValueObject(Fields, OutRow.Value, DiagnosticCode))
	{
		OutReason = TEXT("EncodeFailed");
		return false;
	}
	OutRow.Name = Property.GetName();
	OutRow.DeclaringClass = OwnerClass->GetPathName();
	return true;
}

bool TryBuildPropertyInspectionAdmittedRow(
	FProperty& Property,
	UObject& CurrentObject,
	UObject& DefaultObject,
	FAssetDocumentEditorPropertyInspectionRow& OutRow,
	const TCHAR*& OutReason,
	bool bDeclared)
{
	FAssetDocumentEditorScopedPropertyValue Current(Property);
	FAssetDocumentEditorScopedPropertyValue Default(Property);
	if (!TryReadPropertyInspectionValuePair(
		CurrentObject, DefaultObject, Property, Current, Default, OutReason, bDeclared))
	{
		return false;
	}
	// An editor-derived reference (codec header) is the asset's own state, not an
	// authorable value: documents omit the property.
	FString DerivedPath;
	if (!CurrentObject.IsTemplate()
		&& FindAssetDocumentEditorDerivedReference(Property, Current.GetMemory(), CurrentObject, DerivedPath) != nullptr)
	{
		OutReason = TEXT("EditorDerivedReference");
		return false;
	}
	FAssetDocumentEditorPropertyInspectionEncodedValues Values;
	if (!TryEncodePropertyInspectionValuePair(
		Property, Current, Default, Values, OutReason))
	{
		return false;
	}
	return TryBuildPropertyInspectionAdmittedValueRow(
		Property, CurrentObject, MoveTemp(Values), OutRow, OutReason, bDeclared);
}

bool TryBuildPropertyInspectionRejectedRow(
	const FProperty& Property,
	const TCHAR* Reason,
	FAssetDocumentEditorPropertyInspectionRow& OutRow)
{
	const UClass* OwnerClass = Property.GetOwnerClass();
	if (OwnerClass == nullptr)
	{
		return false;
	}
	FString DiagnosticCode;
	const TArray<FAssetDocumentEditorValueField> Fields = {
		{TEXT("name"), MakePropertyInspectionStringValue(Property.GetName())},
		{TEXT("declaring_class"),
			MakePropertyInspectionStringValue(OwnerClass->GetPathName())},
		{TEXT("ue_cpp_type"),
			MakePropertyInspectionStringValue(Property.GetCPPType())},
		{TEXT("reason_code"), MakePropertyInspectionStringValue(Reason)},
	};
	if (!TryBuildAssetDocumentEditorValueObject(Fields, OutRow.Value, DiagnosticCode))
	{
		return false;
	}
	OutRow.Name = Property.GetName();
	OutRow.DeclaringClass = OwnerClass->GetPathName();
	return true;
}

const TCHAR* GetPropertyInspectionRowRejectionReason(const FProperty& Property, bool bDeclared)
{
	if (bDeclared)
	{
		if (!IsAssetDocumentEditorDeclarableProperty(Property))
		{
			const TCHAR* FlagReason = GetPropertyInspectionFlagRejectionReason(Property);
			return FlagReason != nullptr ? FlagReason : TEXT("NonEditable");
		}
		return nullptr;
	}
	const TCHAR* RejectionReason = GetPropertyInspectionRejectionReason(Property);
	if (RejectionReason != nullptr)
	{
		return RejectionReason;
	}
	return PropertyAccessUtil::CanGetPropertyValue(&Property)
		== EPropertyAccessResultFlags::Success
		? nullptr
		: TEXT("ReadDenied");
}

bool TryBuildPropertyInspectionRow(
	FProperty& Property,
	UObject& CurrentObject,
	UObject& DefaultObject,
	FAssetDocumentEditorPropertyInspectionRow& OutRow,
	bool& bOutAdmitted,
	bool bDeclared)
{
	const TCHAR* RejectionReason = GetPropertyInspectionRowRejectionReason(Property, bDeclared);
	if (RejectionReason == nullptr
		&& TryBuildPropertyInspectionAdmittedRow(
			Property, CurrentObject, DefaultObject, OutRow, RejectionReason, bDeclared))
	{
		bOutAdmitted = true;
		return true;
	}
	bOutAdmitted = false;
	return TryBuildPropertyInspectionRejectedRow(
		Property,
		RejectionReason != nullptr ? RejectionReason : TEXT("UnsupportedCodec"),
		OutRow);
}

void SortPropertyInspectionRows(
	TArray<FAssetDocumentEditorPropertyInspectionRow>& Rows)
{
	Rows.Sort([](
		const FAssetDocumentEditorPropertyInspectionRow& Left,
		const FAssetDocumentEditorPropertyInspectionRow& Right)
	{
		return Left.Name == Right.Name
			? Left.DeclaringClass < Right.DeclaringClass
			: Left.Name < Right.Name;
	});
}

FAssetDocumentRuntimeValueDto MakePropertyInspectionRows(
	TArray<FAssetDocumentEditorPropertyInspectionRow>& Rows)
{
	SortPropertyInspectionRows(Rows);
	FAssetDocumentRuntimeValueDto Value;
	Value.Kind = EAssetDocumentRuntimeValueKind::Array;
	for (FAssetDocumentEditorPropertyInspectionRow& Row : Rows)
	{
		Value.Elements.Add(MoveTemp(Row.Value));
	}
	return Value;
}

UObject& GetPropertyInspectionCurrentObject(
	UObject& DefaultObject,
	UObject* LiveAsset)
{
	return LiveAsset != nullptr ? *LiveAsset : DefaultObject;
}

bool TryCollectPropertyInspectionRows(
	UClass& ExactClass,
	UObject& CurrentObject,
	UObject& DefaultObject,
	TArray<FAssetDocumentEditorPropertyInspectionRow>& OutAdmitted,
	TArray<FAssetDocumentEditorPropertyInspectionRow>& OutRejected,
	TConstArrayView<FString> DeclaredNames)
{
	for (TFieldIterator<FProperty> It(&ExactClass); It; ++It)
	{
		FAssetDocumentEditorPropertyInspectionRow Row;
		bool bAdmitted = false;
		// A declared name only widens a non-edit property; edit-visible ones keep their evidence.
		const bool bDeclared = !It->HasAnyPropertyFlags(CPF_Edit)
			&& DeclaredNames.Contains(It->GetAuthoredName());
		if (!TryBuildPropertyInspectionRow(
			**It, CurrentObject, DefaultObject, Row, bAdmitted, bDeclared))
		{
			return false;
		}
		if (bAdmitted)
		{
			OutAdmitted.Add(MoveTemp(Row));
		}
		else
		{
			OutRejected.Add(MoveTemp(Row));
		}
	}
	return true;
}

bool TryBuildPropertyInspectionSubject(
	UClass& ExactClass,
	UObject* LiveAsset,
	FAssetDocumentRuntimeValueDto& OutSubject,
	FString& OutDiagnosticCode)
{
	TArray<FAssetDocumentEditorValueField> SubjectFields = {
		{TEXT("kind"), MakePropertyInspectionStringValue(TEXT("class_defaults"))},
		{TEXT("exact_class_path"),
			MakePropertyInspectionStringValue(ExactClass.GetPathName())},
	};
	if (LiveAsset != nullptr)
	{
		SubjectFields[0].Value = MakePropertyInspectionStringValue(TEXT("live_asset"));
		SubjectFields.Add({TEXT("asset_path"),
			MakePropertyInspectionStringValue(LiveAsset->GetPathName())});
	}
	return TryBuildAssetDocumentEditorValueObject(
		SubjectFields, OutSubject, OutDiagnosticCode);
}

FAssetDocumentEditorPropertyInspectionResult BuildPropertyInspectionResult(
	UClass& ExactClass,
	UObject& DefaultObject,
	UObject* LiveAsset,
	TConstArrayView<FString> DeclaredNames = {})
{
	TArray<FAssetDocumentEditorPropertyInspectionRow> Admitted;
	TArray<FAssetDocumentEditorPropertyInspectionRow> Rejected;
	if (!TryCollectPropertyInspectionRows(
		ExactClass,
		GetPropertyInspectionCurrentObject(DefaultObject, LiveAsset),
		DefaultObject,
		Admitted,
		Rejected,
		DeclaredNames))
	{
		return MakePropertyInspectionFailure(
			TEXT("PropertyInspection.OutputBuildFailed"),
			TEXT("The property inspection row could not be encoded."));
	}
	FString DiagnosticCode;
	FAssetDocumentRuntimeValueDto Subject;
	if (!TryBuildPropertyInspectionSubject(
		ExactClass, LiveAsset, Subject, DiagnosticCode))
	{
		return MakePropertyInspectionFailure(
			TEXT("PropertyInspection.OutputBuildFailed"),
			TEXT("The inspection subject could not be encoded."));
	}
	FAssetDocumentEditorPropertyInspectionResult Result;
	const TArray<FAssetDocumentEditorValueField> ResultFields = {
		{TEXT("subject"), MoveTemp(Subject)},
		{TEXT("admitted"), MakePropertyInspectionRows(Admitted)},
		{TEXT("rejected"), MakePropertyInspectionRows(Rejected)},
	};
	if (!TryBuildAssetDocumentEditorValueObject(ResultFields, Result.Value, DiagnosticCode))
	{
		return MakePropertyInspectionFailure(
			TEXT("PropertyInspection.OutputBuildFailed"),
			TEXT("The inspection result could not be encoded."));
	}
	Result.bSucceeded = true;
	return Result;
}
}

FAssetDocumentEditorPropertyInspectionResult
InspectAssetDocumentEditorProperties(UClass& ExactClass, UObject* LiveAsset,
	TConstArrayView<FString> DeclaredNames)
{
	UObject* DefaultObject = ExactClass.GetDefaultObject();
	if (DefaultObject == nullptr)
	{
		return MakePropertyInspectionFailure(
			TEXT("PropertyInspection.DefaultUnavailable"),
			TEXT("The exact class default object is unavailable."));
	}
	return BuildPropertyInspectionResult(ExactClass, *DefaultObject, LiveAsset, DeclaredNames);
}

FAssetDocumentEditorPropertyInspectionResult
InspectAssetDocumentEditorExistingObjectProperties(UObject& ExactObject)
{
	if (!IsInGameThread())
	{
		return MakePropertyInspectionFailure(TEXT("PropertyInspection.ContextInvalid"),
			TEXT("Existing-object property observation requires the game thread."));
	}
	if (!IsValid(&ExactObject) || !IsValid(ExactObject.GetClass()))
	{
		return MakePropertyInspectionFailure(TEXT("PropertyInspection.InvalidObject"),
			TEXT("Existing-object property observation requires a live object and class."));
	}
	UClass& ExactClass = *ExactObject.GetClass();
	UObject* DefaultObject = ExactClass.GetDefaultObject(false);
	if (!IsValid(DefaultObject))
	{
		return MakePropertyInspectionFailure(TEXT("PropertyInspection.DefaultUnavailable"),
			TEXT("The existing class default object is unavailable; observation does not create it."));
	}
	return BuildPropertyInspectionResult(ExactClass, *DefaultObject, &ExactObject);
}
