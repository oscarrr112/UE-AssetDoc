// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/Property/AssetDocumentEditorPropertyEdit.h"

#include "AssetDocumentEditorPickerValues.h"
#include "AssetDocumentEditorPropertyValueCodec.h"
#include "Provider/AssetDocumentEditorTypedValueCodec.h"
#include "Provider/AssetDocumentEditorValueDto.h"
#include "Provider/Cutover/AssetDocumentEditorMutationFootprintBuilders.h"
#include "Provider/Lifecycle/AssetDocumentEditorEditorSafety.h"
#include "Provider/Property/AssetDocumentEditorDeclaredPropertyAccess.h"
#include "Provider/Property/AssetDocumentEditorPropertyBatchTokenPayload.h"
#include "Provider/Property/AssetDocumentEditorPreflightIsolation.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/PropertyAccessUtil.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace
{
enum class EPropertyPreimageState
{
	Ready,
	Consumed,
};

enum class EPropertyPreimageCaptureFailure : uint8
{
	None,
	InvalidFieldContract,
	TypeOrCodecChanged,
	AccessChanged,
	ReadFailed,
	EncodeFailed,
};

enum class EPropertyPreimageRestoreFailure : uint8
{
	None,
	IdentityChanged,
	TypeOrCodecChanged,
	AccessChanged,
	DecodeFailed,
	ReadFailed,
	WriteFailed,
	ReadbackMismatch,
	DirtyStateRestoreFailed,
};

struct FPropertyPreimageFieldSnapshot
{
	TWeakObjectPtr<UClass> DeclaringClass;
	TFieldPath<FProperty> Property;
	FName OriginalFieldName;
	uint64 TypeRevision = 0;
	FString CodecId;
	FAssetDocumentRuntimeValueDto EncodedValue;
};

struct FPropertyPreimageIdentity
{
	TStrongObjectPtr<UObject> Target;
	TStrongObjectPtr<UPackage> Package;
	TWeakObjectPtr<UClass> TargetClass;
	TWeakObjectPtr<UObject> TargetOuter;
	FString ObjectPath;
	FString PackageName;
	uint64 ObjectRevision = 0;
	uint64 PackageRevision = 0;
	bool bOriginalPackageDirty = false;
};

struct FPropertyPreimageRestoreField
{
	FProperty* Property = nullptr;
	TUniquePtr<FAssetDocumentEditorScopedPropertyValue> Baseline;
};

FAssetDocumentEditorPropertyEditResult FailPropertyEdit(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentEditorPropertyEditResult Result;
	Result.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	FAssetDocumentRuntimeDiagnostic& Diagnostic =
		Result.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/Primitive");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Result;
}

FAssetDocumentContractValidationResult FailPropertyPreimageRestore(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentContractValidationResult Result;
	FAssetDocumentRuntimeDiagnostic& Diagnostic =
		Result.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/PropertyPreimage");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Result;
}

FAssetDocumentEditorPropertyPreimageCaptureResult
FailPropertyPreimageCapture(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentEditorPropertyPreimageCaptureResult Result;
	FAssetDocumentRuntimeDiagnostic& Diagnostic =
		Result.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/PropertyPreimage");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Result;
}

FAssetDocumentEditorPropertyEditResult FailPropertyEdit(
	TArray<FAssetDocumentRuntimeDiagnostic>&& Diagnostics)
{
	FAssetDocumentEditorPropertyEditResult Result;
	Result.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	Result.Diagnostics = MoveTemp(Diagnostics);
	if (Result.Diagnostics.IsEmpty())
	{
		return FailPropertyEdit(
			TEXT("PropertyWrite.FootprintRejected"),
			TEXT("The active transaction rejected the property footprint."));
	}
	return Result;
}

bool IsPropertyEditCodecCurrent(
	FProperty& Property,
	FStringView CodecId)
{
	const FAssetDocumentEditorTypedValueCodecResult Codec =
		InspectAssetDocumentEditorTypedValueProperty(Property);
	return Codec.bSucceeded && FStringView(Codec.CodecId) == CodecId;
}

bool IsPropertyEditCodecCurrent(
	const FAssetDocumentEditorPropertyTokenView& Token)
{
	return Token.Property != nullptr
		&& IsPropertyEditCodecCurrent(*Token.Property, Token.CodecId);
}

bool TryBuildPropertyReadValue(
	const FAssetDocumentEditorPropertyTokenView& Token,
	const FAssetDocumentEditorScopedPropertyValue& NativeValue,
	FAssetDocumentRuntimeValueDto& OutValue,
	const FAssetDocumentRuntimeValueDto* AsAuthored = nullptr)
{
	FAssetDocumentRuntimeValueDto EncodedValue;
	const FAssetDocumentEditorTypedValueCodecResult Codec = AsAuthored != nullptr
		? EncodeAssetDocumentEditorTypedValueAsAuthored(NativeValue, *AsAuthored, EncodedValue)
		: EncodeAssetDocumentEditorTypedValue(NativeValue, EncodedValue);
	if (!Codec.bSucceeded || FStringView(Codec.CodecId) != Token.CodecId)
	{
		return false;
	}
	FString DiagnosticCode;
	const TArray<FAssetDocumentEditorValueField> Fields = {
		{TEXT("value"), MoveTemp(EncodedValue)},
	};
	return TryBuildAssetDocumentEditorValueObject(
		Fields, OutValue, DiagnosticCode);
}

FAssetDocumentRuntimeValueDto MakePropertyBooleanValue(bool bValue)
{
	FAssetDocumentRuntimeValueDto Value;
	Value.Kind = EAssetDocumentRuntimeValueKind::Boolean;
	Value.BooleanValue = bValue;
	return Value;
}

bool TryBuildPropertyWriteValue(
	FAssetDocumentRuntimeValueDto Readback,
	FAssetDocumentRuntimeValueDto& OutValue)
{
	FString DiagnosticCode;
	const TArray<FAssetDocumentEditorValueField> Fields = {
		{TEXT("value"), MoveTemp(Readback)},
		{TEXT("changed"), MakePropertyBooleanValue(true)},
	};
	return TryBuildAssetDocumentEditorValueObject(
		Fields, OutValue, DiagnosticCode);
}

bool TryBuildPropertyPreflightValue(
	FAssetDocumentRuntimeValueDto Readback,
	FAssetDocumentRuntimeValueDto& OutValue)
{
	FString DiagnosticCode;
	const TArray<FAssetDocumentEditorValueField> Fields = {
		{TEXT("canonical_value"), MoveTemp(Readback)},
		{TEXT("feasible"), MakePropertyBooleanValue(true)},
	};
	return TryBuildAssetDocumentEditorValueObject(
		Fields, OutValue, DiagnosticCode);
}

bool TryTakeExactPropertyReadbackValue(
	FAssetDocumentRuntimeValueDto& Readback,
	FAssetDocumentRuntimeValueDto& OutValue)
{
	const TArray<FString> ExpectedKeys = {TEXT("value")};
	if (Readback.Kind != EAssetDocumentRuntimeValueKind::Object
		|| Readback.ObjectKeys != ExpectedKeys
		|| Readback.Elements.Num() != 1)
	{
		return false;
	}
	OutValue = MoveTemp(Readback.Elements[0]);
	return true;
}

bool TryDecodePropertyWriteValue(
	FProperty& Property,
	FStringView CodecId,
	const FAssetDocumentRuntimeValueDto& DesiredValue,
	TUniquePtr<FAssetDocumentEditorScopedPropertyValue>& OutNativeValue,
	FAssetDocumentEditorTypedValueCodecResult* OutDecode = nullptr,
	const FAssetDocumentEditorDecodeBase& Base = FAssetDocumentEditorDecodeBase())
{
	const FAssetDocumentEditorTypedValueCodecResult Decode =
		DecodeAssetDocumentEditorTypedValue(
			Property, DesiredValue, OutNativeValue, Base);
	if (OutDecode != nullptr)
	{
		*OutDecode = Decode;
	}
	return Decode.bSucceeded
		&& FStringView(Decode.CodecId) == CodecId
		&& OutNativeValue.IsValid();
}

bool TryReadPropertyNativeValue(
	UObject& Object,
	const FAssetDocumentEditorPropertyTokenView& Token,
	FAssetDocumentEditorScopedPropertyValue& OutNativeValue);

const TCHAR* const PropertyEditorDerivedReferenceCode = TEXT("Property.EditorDerivedReference");

// Editor-derived references (codec header): a document value may not name an
// object inside the asset through a plain reference (the Details panel picker
// lists assets only); a reference the current value already holds may stay.
// Fills OutFailure (code, value path, AI-readable message).
bool CheckPropertyEditorDerivedReference(
	const FProperty& Property,
	const void* Value,
	const void* CurrentValue,
	const UObject& Owner,
	FAssetDocumentEditorTypedValueCodecResult& OutFailure)
{
	FString ValuePath;
	const UObject* Derived = FindAssetDocumentEditorDerivedReference(Property, Value, Owner, ValuePath, false, CurrentValue);
	if (Derived == nullptr)
	{
		return false;
	}
	const FString Name = Property.GetAuthoredName();
	OutFailure = FAssetDocumentEditorTypedValueCodecResult();
	OutFailure.DiagnosticCode = PropertyEditorDerivedReferenceCode;
	OutFailure.ValuePath = ValuePath;
	OutFailure.Message = FString::Printf(
		TEXT("Property '%s' would reference '%s' at %s%s, an object inside the asset. A plain reference names an "
			"asset or another object outside the asset, as the Details panel picker does; subobjects the asset's "
			"own code creates are omitted from documents and cannot be authored (replace them with an asset or "
			"null, or author the properties that drive them). Instanced subobjects are authored as "
			"{\"Class\", \"Properties\"}."),
		*Name, *Derived->GetPathName(), *Name, *ValuePath);
	return true;
}

// Decodes DesiredValue as an edit of Owner's current value when Owner is given.
bool TryDecodePropertyWriteValue(
	const FAssetDocumentEditorPropertyTokenView& Token,
	const FAssetDocumentRuntimeValueDto& DesiredValue,
	TUniquePtr<FAssetDocumentEditorScopedPropertyValue>& OutNativeValue,
	FAssetDocumentEditorTypedValueCodecResult* OutDecode = nullptr,
	UObject* Owner = nullptr)
{
	if (Token.Property == nullptr)
	{
		return false;
	}
	FAssetDocumentEditorDecodeBase Base;
	TUniquePtr<FAssetDocumentEditorScopedPropertyValue> Current;
	if (Owner != nullptr && Owner->IsA(Token.Property->GetOwnerClass()))
	{
		Current = MakeUnique<FAssetDocumentEditorScopedPropertyValue>(*Token.Property);
		if (TryReadPropertyNativeValue(*Owner, Token, *Current))
		{
			Base.Value = Current->GetMemory();
			Base.Owner = Owner;
		}
	}
	if (!TryDecodePropertyWriteValue(
		*Token.Property,
		Token.CodecId,
		DesiredValue,
		OutNativeValue,
		OutDecode,
		Base))
	{
		return false;
	}
	FAssetDocumentEditorTypedValueCodecResult Derived;
	if (Base.Owner != nullptr
		&& CheckPropertyEditorDerivedReference(*Token.Property, OutNativeValue->GetMemory(), Base.Value, *Owner, Derived))
	{
		if (OutDecode != nullptr)
		{
			*OutDecode = MoveTemp(Derived);
		}
		OutNativeValue.Reset();
		return false;
	}
	// A row handle naming a row its table lacks, or a key no picker offers (the Details pickers list only existing ones).
	UE::AssetDocument::EditorPickerValues::FInvalidPick Invalid;
	if (UE::AssetDocument::EditorPickerValues::FindInvalidPick(*Token.Property, OutNativeValue->GetMemory(), Base.Value, Invalid))
	{
		if (OutDecode != nullptr)
		{
			*OutDecode = FAssetDocumentEditorTypedValueCodecResult();
			OutDecode->DiagnosticCode = Invalid.Code;
			OutDecode->ValuePath = Invalid.ValuePath;
			OutDecode->Message = FString::Printf(TEXT("Property '%s'%s: %s"), *Token.Property->GetAuthoredName(), *Invalid.ValuePath,
				*Invalid.Message);
		}
		OutNativeValue.Reset();
		return false;
	}
	return true;
}

FAssetDocumentRuntimeDiagnosticContext MakePropertyDiagnosticContext(
	const TCHAR* Key,
	const FString& Value)
{
	FAssetDocumentRuntimeDiagnosticContext Context;
	Context.Key = Key;
	Context.Value.Kind = EAssetDocumentRuntimeValueKind::String;
	Context.Value.StringValue = Value;
	return Context;
}

bool CanNotifyPropertyChange(
	const UObject& Object,
	EPropertyAccessChangeNotifyMode NotifyMode)
{
	FString Code;
	FString Message;
	return NotifyMode == EPropertyAccessChangeNotifyMode::Never
		|| CheckAssetDocumentEditorChangeNotificationSupported(Object, Code, Message);
}

// A struct the Details panel edits member by member: not a native-text struct (FGuid, gameplay tag containers, ...,
// which it writes through their ImportTextItem) and not an FInstancedStruct.
const FStructProperty* AsMemberWiseStruct(const FProperty& Property)
{
	const FStructProperty* Struct = CastField<FStructProperty>(&Property);
	if (Struct == nullptr || Struct->Struct == nullptr || Struct->ArrayDim != 1 || Struct->Struct == FInstancedStruct::StaticStruct())
	{
		return nullptr;
	}
	const UScriptStruct::ICppStructOps* Ops = Struct->Struct->GetCppStructOps();
	return Ops != nullptr && (Ops->HasImportTextItem() || Ops->HasExportTextItem()) ? nullptr : Struct;
}

// Writes Source's reflected members into Dest in place, as the Details panel (and a pasted struct's ImportText) does,
// instead of the struct's C++ assignment, which also copies native state the struct keeps about its owner
// (FDataflowInstance copies its owner-change delegate handle: assigning a fresh value leaves the owner's raw
// delegate binding behind, and the editor crashes once the owner is destroyed). Copy is Dest copied the way Source
// was seeded from it (the struct's C++ copy): a member Source holds as that copy left it is not written, so members
// a struct's copy does not carry over (FDataflowInstance's owner on some engines) keep their value.
void CopyStructMembersInPlace(const UScriptStruct& Struct, const void* Source, const void* Copy, void* Dest)
{
	for (TFieldIterator<FProperty> It(&Struct); It; ++It)
	{
		const FProperty* Member = *It;
		const void* MemberSource = Member->ContainerPtrToValuePtr<void>(Source);
		const void* MemberCopy = Member->ContainerPtrToValuePtr<void>(Copy);
		void* MemberDest = Member->ContainerPtrToValuePtr<void>(Dest);
		if (Member->Identical(MemberDest, MemberSource, PPF_None) || Member->Identical(MemberCopy, MemberSource, PPF_None))
		{
			continue;
		}
		if (const FStructProperty* Nested = AsMemberWiseStruct(*Member))
		{
			CopyStructMembersInPlace(*Nested->Struct, MemberSource, MemberCopy, MemberDest);
		}
		else
		{
			Member->CopyCompleteValue(MemberDest, MemberSource);
		}
	}
}

bool TrySetPropertyValue(
	UObject& Object,
	FProperty& Property,
	const FAssetDocumentEditorScopedPropertyValue& NativeValue,
	EPropertyAccessChangeNotifyMode NotifyMode,
	bool bRequireCanModify)
{
	const bool bInPlace = IsAssetDocumentEditorInPlaceInstancedProperty(Property);
	if (!CanNotifyPropertyChange(Object, NotifyMode)
		|| (bRequireCanModify && !Object.CanModify())
		|| (!bInPlace && PropertyAccessUtil::CanSetPropertyValue(
			&Property,
			PropertyAccessUtil::EditorReadOnlyFlags,
			Object.IsTemplate()) != EPropertyAccessResultFlags::Success))
	{
		return false;
	}
	if (bInPlace)
	{
		// VisibleAnywhere instanced pointer: edit the current subobject's fields in
		// place (as the Details panel does); the pointer itself is never set.
		const FObjectProperty& Instanced = *CastFieldChecked<FObjectProperty>(&Property);
		void* const CurrentValue = Property.ContainerPtrToValuePtr<void>(&Object);
		UObject* const Current = Instanced.GetObjectPropertyValue(CurrentValue);
		UObject* const Desired = Instanced.GetObjectPropertyValue(NativeValue.GetMemory());
		if (Current == nullptr || Desired == nullptr || Desired->GetClass() != Current->GetClass())
		{
			return false;
		}
		AdoptAssetDocumentEditorInstancedValue(
			Object,
			NativeValue,
			CurrentValue,
			NotifyMode != EPropertyAccessChangeNotifyMode::Never);
		return Instanced.GetObjectPropertyValue(NativeValue.GetMemory()) == Current;
	}
	const FStructProperty* const MemberWise = AsMemberWiseStruct(Property);
	if (MemberWise != nullptr && !DoesAssetDocumentEditorPropertyHoldInstancedObjects(Property)
		&& !Object.HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
	{
		// A struct value is written member by member into the current struct (see CopyStructMembersInPlace), with
		// the same change notification SetPropertyValue_Object sends.
		void* const Dest = Property.ContainerPtrToValuePtr<void>(&Object);
		const void* const Source = NativeValue.GetMemory();
		FAssetDocumentEditorScopedPropertyValue Copy(Property);
		Property.CopyCompleteValue(Copy.GetMemory(), Dest);
		const void* const CopyMemory = Copy.GetMemory();
		return PropertyAccessUtil::SetPropertyValue(
			[&Property, MemberWise, Dest, Source, CopyMemory](const FPropertyAccessChangeNotify* ChangeNotify)
			{
				const bool bIdentical = Property.Identical(Dest, Source, PPF_None);
				PropertyAccessUtil::EmitPreChangeNotify(ChangeNotify, bIdentical);
				if (!bIdentical)
				{
					CopyStructMembersInPlace(*MemberWise->Struct, Source, CopyMemory, Dest);
				}
				PropertyAccessUtil::EmitPostChangeNotify(ChangeNotify, bIdentical);
				return true;
			},
			[&Property, &Object, NotifyMode]()
			{
				return PropertyAccessUtil::BuildBasicChangeNotify(&Property, &Object, NotifyMode, EPropertyChangeType::ValueSet);
			}) == EPropertyAccessResultFlags::Success;
	}
	if (!DoesAssetDocumentEditorPropertyHoldInstancedObjects(Property))
	{
		return PropertyAccessUtil::SetPropertyValue_Object(
			&Property,
			&Object,
			&Property,
			NativeValue.GetMemory(),
			INDEX_NONE,
			PropertyAccessUtil::EditorReadOnlyFlags,
			NotifyMode)
			== EPropertyAccessResultFlags::Success;
	}
	// Instanced subobjects: reuse/adopt them under Object like the Details panel
	// class picker, then move replaced ones out (see the codec header).
	void* const CurrentValue = Property.ContainerPtrToValuePtr<void>(&Object);
	FAssetDocumentEditorScopedPropertyValue Before(Property);
	Property.CopyCompleteValue(Before.GetMemory(), CurrentValue);
	AdoptAssetDocumentEditorInstancedValue(
		Object,
		NativeValue,
		CurrentValue,
		NotifyMode != EPropertyAccessChangeNotifyMode::Never);
	const bool bSet = PropertyAccessUtil::SetPropertyValue_Object(
		&Property,
		&Object,
		&Property,
		NativeValue.GetMemory(),
		INDEX_NONE,
		PropertyAccessUtil::EditorReadOnlyFlags,
		NotifyMode)
		== EPropertyAccessResultFlags::Success;
	if (bSet)
	{
		ReleaseAssetDocumentEditorReplacedInstancedObjects(
			Object, Property, Before.GetMemory(), CurrentValue);
	}
	return bSet;
}

bool TrySetPropertyValue(
	UObject& Object,
	const FAssetDocumentEditorPropertyTokenView& Token,
	const FAssetDocumentEditorScopedPropertyValue& NativeValue,
	EPropertyAccessChangeNotifyMode NotifyMode,
	bool bRequireCanModify)
{
	if (Token.Property != nullptr && Token.bDeclared)
	{
		return CanNotifyPropertyChange(Object, NotifyMode)
			&& (!bRequireCanModify || Object.CanModify())
			&& SetAssetDocumentEditorDeclaredPropertyValue(
				Object, *Token.Property, NativeValue.GetMemory(), NotifyMode);
	}
	return Token.Property != nullptr
		&& TrySetPropertyValue(
			Object,
			*Token.Property,
			NativeValue,
			NotifyMode,
			bRequireCanModify);
}

bool TryGetPropertyValue(
	UObject& Object,
	FProperty& Property,
	FAssetDocumentEditorScopedPropertyValue& OutNativeValue)
{
	return OutNativeValue.IsValid()
		&& &OutNativeValue.GetProperty() == &Property
		&& PropertyAccessUtil::GetPropertyValue_Object(
			&Property,
			&Object,
			&Property,
			OutNativeValue.GetMemory(),
			INDEX_NONE) == EPropertyAccessResultFlags::Success;
}

bool TryReadPropertyNativeValue(
	UObject& Object,
	const FAssetDocumentEditorPropertyTokenView& Token,
	FAssetDocumentEditorScopedPropertyValue& OutNativeValue)
{
	return Token.Property != nullptr
		&& (Token.bDeclared
			? OutNativeValue.IsValid() && GetAssetDocumentEditorDeclaredPropertyValue(
				Object, *Token.Property, OutNativeValue.GetMemory())
			: ReadAssetDocumentEditorObjectPropertyValue(Object, *Token.Property, OutNativeValue));
}

void AddPropertyChangedRecord(
	const UObject& Asset,
	const FAssetDocumentEditorPropertyTokenView& Token,
	FStringView AuthorityId,
	FAssetDocumentEditorPropertyEditResult& Result)
{
	FAssetDocumentChangedRecord& Changed =
		Result.ChangedSet.AddDefaulted_GetRef();
	Changed.AuthorityId = AuthorityId;
	Changed.StableObjectId = Asset.GetPathName();
	Changed.Path = FString::Printf(
		TEXT("/%s"), *Token.Property->GetAuthoredName());
	Changed.ChangeKind = TEXT("modified");
}

// AI-readable decode failure: names the property, its UE type and the JSON shape
// the codec expects, so a client can correct the value without guessing.
FString DescribePropertyDecodeFailure(
	const FAssetDocumentEditorPropertyTokenView& Token,
	const TCHAR* Role)
{
	const FString Name = Token.Property != nullptr
		? Token.Property->GetAuthoredName() : FString(TEXT("<unknown>"));
	const FString Type = Token.Property != nullptr
		? Token.Property->GetCPPType() : FString(TEXT("<unknown>"));
	const FStringView Codec = Token.CodecId;
	const TCHAR* Expected =
		Codec == TEXTVIEW("number.v1") ? TEXT("a JSON number")
		: Codec == TEXTVIEW("integer.v1") ? TEXT("an integral JSON number")
		: Codec == TEXTVIEW("bool.v1") ? TEXT("true or false")
		: Codec == TEXTVIEW("struct.v1") ? TEXT("an object of the struct's members (omitted members keep their current value)")
		: (Codec == TEXTVIEW("array.v1") || Codec == TEXTVIEW("set.v1")
			|| Codec == TEXTVIEW("fixed_array.v1")) ? TEXT("an array of element values")
		: Codec == TEXTVIEW("string_key_map.v1") ? TEXT("an object keyed by string")
		: TEXT("a JSON string");
	return FString::Printf(
		TEXT("The %s value for property '%s' (%s, codec %.*s) does not match the token codec: expected %s."),
		Role, *Name, *Type, Codec.Len(), Codec.GetData(), Expected);
}

// A decode failure names the property and, when the codec located it, the
// JSON-pointer inside the value ("value_path") plus an AI-readable reason. The
// Python adapter maps these onto /Properties/<name><value_path>.
FAssetDocumentEditorPropertyEditResult FailPropertyDecode(
	const TCHAR* Code,
	const FAssetDocumentEditorPropertyTokenView& Token,
	const TCHAR* Role,
	const FAssetDocumentEditorTypedValueCodecResult& Decode)
{
	if (Token.Property != nullptr && (Decode.DiagnosticCode == PropertyEditorDerivedReferenceCode
		|| Decode.DiagnosticCode == UE::AssetDocument::EditorPickerValues::RowNotFoundCode
		|| Decode.DiagnosticCode == UE::AssetDocument::EditorPickerValues::KeyNotFoundCode))
	{
		FAssetDocumentEditorPropertyEditResult Result = FailPropertyEdit(*Decode.DiagnosticCode, *Decode.Message);
		FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics[0];
		Diagnostic.Context.Add(MakePropertyDiagnosticContext(TEXT("property"), Token.Property->GetAuthoredName()));
		Diagnostic.Context.Add(MakePropertyDiagnosticContext(TEXT("value_path"), Decode.ValuePath));
		return Result;
	}
	if (Token.Property == nullptr || Decode.Message.IsEmpty())
	{
		FAssetDocumentEditorPropertyEditResult Result =
			FailPropertyEdit(Code, *DescribePropertyDecodeFailure(Token, Role));
		if (Token.Property != nullptr)
		{
			Result.Diagnostics[0].Context.Add(
				MakePropertyDiagnosticContext(TEXT("property"), Token.Property->GetAuthoredName()));
		}
		return Result;
	}
	const FString Name = Token.Property->GetAuthoredName();
	FAssetDocumentEditorPropertyEditResult Result = FailPropertyEdit(Code, *FString::Printf(
		TEXT("The %s value for property '%s' (%s) does not match the token codec at %s%s: %s"),
		Role, *Name, *Token.Property->GetCPPType(), *Name, *Decode.ValuePath, *Decode.Message));
	FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics[0];
	Diagnostic.Context.Add(MakePropertyDiagnosticContext(TEXT("property"), Name));
	Diagnostic.Context.Add(MakePropertyDiagnosticContext(TEXT("value_path"), Decode.ValuePath));
	return Result;
}

bool TryPreparePropertyPreflight(
	UObject& Scratch,
	const FAssetDocumentEditorPropertyTokenView& Token,
	const FAssetDocumentRuntimeValueDto& DesiredValue,
	TUniquePtr<FAssetDocumentEditorScopedPropertyValue>& OutNativeValue,
	FAssetDocumentEditorPropertyEditResult& OutFailure)
{
	if (!Token.bWritable || !IsPropertyEditCodecCurrent(Token))
	{
		OutFailure = FailPropertyEdit(
			TEXT("PropertyToken.InvalidPayload"),
			TEXT("Property preflight requires a live writable token."));
		return false;
	}
	FAssetDocumentEditorTypedValueCodecResult Decode;
	if (!TryDecodePropertyWriteValue(
		Token, DesiredValue, OutNativeValue, &Decode, &Scratch))
	{
		OutFailure = FailPropertyDecode(
			TEXT("PropertyPreflight.DecodeFailed"),
			Token,
			TEXT("candidate"),
			Decode);
		return false;
	}
	return true;
}

FAssetDocumentEditorPropertyEditResult CompletePropertyWrite(
	UObject& Asset,
	const FAssetDocumentEditorPropertyTokenView& Token,
	FStringView AuthorityId)
{
	FAssetDocumentEditorPropertyEditResult Readback =
		ReadAssetDocumentEditorAdmittedProperty(Asset, Token);
	if (!Readback.bSucceeded)
	{
		return FailPropertyEdit(
			TEXT("PropertyWrite.ReadbackFailed"),
			TEXT("The authoritative property readback could not be completed."));
	}
	FAssetDocumentRuntimeValueDto ReadbackValue;
	if (!TryTakeExactPropertyReadbackValue(
		Readback.Value, ReadbackValue))
	{
		return FailPropertyEdit(
			TEXT("PropertyWrite.ReadbackFailed"),
			TEXT("The authoritative property readback could not be completed."));
	}
	if (!TryBuildPropertyWriteValue(
		MoveTemp(ReadbackValue), Readback.Value))
	{
		return FailPropertyEdit(
			TEXT("PropertyWrite.ReadbackFailed"),
			TEXT("The authoritative property readback could not be completed."));
	}
	AddPropertyChangedRecord(Asset, Token, AuthorityId, Readback);
	return Readback;
}

// The scratch readback in the shape of the authored value (a partial value is
// compared on the members it lists), so the caller can require it to equal the
// authored value exactly.
FAssetDocumentEditorPropertyEditResult CompletePropertyPreflight(
	UObject& Scratch,
	const FAssetDocumentEditorPropertyTokenView& Token,
	const FAssetDocumentRuntimeValueDto& DesiredValue)
{
	FAssetDocumentEditorPropertyEditResult Readback;
	FAssetDocumentRuntimeValueDto ReadbackValue;
	FAssetDocumentEditorScopedPropertyValue NativeValue(*Token.Property);
	if (!IsPropertyEditCodecCurrent(Token)
		|| !TryReadPropertyNativeValue(Scratch, Token, NativeValue))
	{
		return FailPropertyEdit(
			TEXT("PropertyPreflight.ReadbackFailed"),
			TEXT("The scratch property readback could not be completed."));
	}
	const FAssetDocumentEditorTypedValueCodecResult Encoded =
		EncodeAssetDocumentEditorTypedValueAsAuthored(NativeValue, DesiredValue, ReadbackValue);
	if (!Encoded.bSucceeded
		|| FStringView(Encoded.CodecId) != Token.CodecId
		|| !TryBuildPropertyPreflightValue(
			MoveTemp(ReadbackValue), Readback.Value))
	{
		return FailPropertyEdit(
			TEXT("PropertyPreflight.ReadbackFailed"),
			TEXT("The scratch property readback could not be completed."));
	}
	Readback.bSucceeded = true;
	return Readback;
}

struct FPropertyPreimageFailureDescription
{
	const TCHAR* Code;
	const TCHAR* Message;
};

const FPropertyPreimageFailureDescription&
GetPropertyPreimageCaptureFailure(
	EPropertyPreimageCaptureFailure Failure)
{
	static const FPropertyPreimageFailureDescription Descriptions[] = {
		{TEXT(""), TEXT("")},
		{TEXT("PropertyPreimage.InvalidFieldContract"),
			TEXT("The admitted property field contract is invalid.")},
		{TEXT("PropertyPreimage.TypeOrCodecChanged"),
			TEXT("The admitted property type or codec changed before capture.")},
		{TEXT("PropertyPreimage.AccessChanged"),
			TEXT("The admitted property is no longer readable and writable.")},
		{TEXT("PropertyPreimage.ReadFailed"),
			TEXT("The admitted property value could not be captured.")},
		{TEXT("PropertyPreimage.EncodeFailed"),
			TEXT("The admitted property value could not be encoded exactly.")},
	};
	return Descriptions[static_cast<uint8>(Failure)];
}

const FPropertyPreimageFailureDescription&
GetPropertyPreimageRestoreFailure(
	EPropertyPreimageRestoreFailure Failure)
{
	static const FPropertyPreimageFailureDescription Descriptions[] = {
		{TEXT(""), TEXT("")},
		{TEXT("PropertyPreimage.IdentityChanged"),
			TEXT("The captured target or package identity changed.")},
		{TEXT("PropertyPreimage.TypeOrCodecChanged"),
			TEXT("A captured property type or codec contract changed.")},
		{TEXT("PropertyPreimage.AccessChanged"),
			TEXT("The captured target or property is no longer writable.")},
		{TEXT("PropertyPreimage.DecodeFailed"),
			TEXT("A captured property baseline could not be decoded.")},
		{TEXT("PropertyPreimage.ReadFailed"),
			TEXT("A current property value could not be read.")},
		{TEXT("PropertyPreimage.WriteFailed"),
			TEXT("A captured property baseline could not be restored.")},
		{TEXT("PropertyPreimage.ReadbackMismatch"),
			TEXT("A restored property did not match its captured baseline.")},
		{TEXT("PropertyPreimage.DirtyStateRestoreFailed"),
			TEXT("The captured package dirty state could not be restored.")},
	};
	return Descriptions[static_cast<uint8>(Failure)];
}

bool IsLivePropertyPreimageObject(const UObject* Object)
{
	if (!IsValid(Object))
	{
		return false;
	}
	if (Object->IsUnreachable())
	{
		return false;
	}
	return true;
}

bool IsValidPropertyPreimageTarget(const UObject& Target)
{
	if (!IsLivePropertyPreimageObject(&Target))
	{
		return false;
	}
	if (Target.IsTemplate())
	{
		return false;
	}
	return !Target.HasAnyFlags(
		RF_ArchetypeObject | RF_ClassDefaultObject);
}

bool TryCapturePropertyPreimageIdentity(
	UObject& ExactTarget,
	FPropertyPreimageIdentity& OutIdentity)
{
	UClass* const ExactClass = ExactTarget.GetClass();
	UPackage* const ExactPackage = ExactTarget.GetOutermost();
	if (!IsValidPropertyPreimageTarget(ExactTarget))
	{
		return false;
	}
	if (!IsLivePropertyPreimageObject(ExactClass))
	{
		return false;
	}
	if (!IsLivePropertyPreimageObject(ExactPackage))
	{
		return false;
	}
	OutIdentity.Target.Reset(&ExactTarget);
	OutIdentity.Package.Reset(ExactPackage);
	OutIdentity.TargetClass = ExactClass;
	OutIdentity.TargetOuter = ExactTarget.GetOuter();
	OutIdentity.ObjectPath = ExactTarget.GetPathName();
	OutIdentity.PackageName = ExactPackage->GetName();
	OutIdentity.ObjectRevision =
		static_cast<uint64>(ExactTarget.GetUniqueID());
	OutIdentity.PackageRevision =
		static_cast<uint64>(ExactPackage->GetUniqueID());
	OutIdentity.bOriginalPackageDirty = ExactPackage->IsDirty();
	return true;
}

bool IsPropertyPreimageFieldShapeValid(
	const FAssetDocumentEditorPropertyBatchTokenFieldView& Field)
{
	if (Field.Property == nullptr || Field.DeclaringClass == nullptr)
	{
		return false;
	}
	if (!Field.bReadable || !Field.bWritable)
	{
		return false;
	}
	return true;
}

bool IsPropertyPreimageFieldOwnerValid(
	UClass& ExactTargetClass,
	const FAssetDocumentEditorPropertyBatchTokenFieldView& Field,
	const TSet<FProperty*>& SeenProperties)
{
	if (Field.Property->GetOwnerClass() != Field.DeclaringClass)
	{
		return false;
	}
	if (!ExactTargetClass.IsChildOf(Field.DeclaringClass))
	{
		return false;
	}
	return !SeenProperties.Contains(Field.Property);
}

bool IsPropertyPreimageFieldTypeCurrent(
	const FAssetDocumentEditorPropertyBatchTokenFieldView& Field)
{
	if (Field.TypeRevision
		!= GetAssetDocumentEditorPropertyTypeRevision(*Field.DeclaringClass))
	{
		return false;
	}
	return IsPropertyEditCodecCurrent(*Field.Property, Field.CodecId);
}

bool IsPropertyPreimageFieldAccessCurrent(
	const FAssetDocumentEditorPropertyBatchTokenFieldView& Field)
{
	if (PropertyAccessUtil::CanGetPropertyValue(Field.Property)
		!= EPropertyAccessResultFlags::Success)
	{
		return false;
	}
	return PropertyAccessUtil::CanSetPropertyValue(
		Field.Property,
		PropertyAccessUtil::EditorReadOnlyFlags,
		false) == EPropertyAccessResultFlags::Success
		|| IsAssetDocumentEditorInPlaceInstancedProperty(*Field.Property);
}

EPropertyPreimageCaptureFailure ValidatePropertyPreimageField(
	UClass& ExactTargetClass,
	const FAssetDocumentEditorPropertyBatchTokenFieldView& Field,
	TSet<FProperty*>& SeenProperties)
{
	if (!IsPropertyPreimageFieldShapeValid(Field)
		|| !IsPropertyPreimageFieldOwnerValid(
			ExactTargetClass, Field, SeenProperties))
	{
		return EPropertyPreimageCaptureFailure::InvalidFieldContract;
	}
	SeenProperties.Add(Field.Property);
	if (!IsPropertyPreimageFieldTypeCurrent(Field))
	{
		return EPropertyPreimageCaptureFailure::TypeOrCodecChanged;
	}
	if (!IsPropertyPreimageFieldAccessCurrent(Field))
	{
		return EPropertyPreimageCaptureFailure::AccessChanged;
	}
	return EPropertyPreimageCaptureFailure::None;
}

bool IsPropertyPreimageCodecResultExact(
	const FAssetDocumentEditorTypedValueCodecResult& Result,
	FStringView CodecId)
{
	return Result.bSucceeded
		&& FStringView(Result.CodecId) == CodecId;
}

bool IsPropertyPreimageRoundTripExact(
	const FProperty& Property,
	const FAssetDocumentEditorScopedPropertyValue& Captured,
	const TUniquePtr<FAssetDocumentEditorScopedPropertyValue>& RoundTrip)
{
	if (!RoundTrip)
	{
		return false;
	}
	return AreAssetDocumentEditorTypedValuesEquivalent(
		const_cast<FProperty&>(Property), Captured.GetMemory(), RoundTrip->GetMemory());
}

EPropertyPreimageCaptureFailure CapturePropertyPreimageFieldValue(
	UObject& ExactTarget,
	const FAssetDocumentEditorPropertyBatchTokenFieldView& Field,
	FPropertyPreimageFieldSnapshot& OutSnapshot)
{
	FAssetDocumentEditorScopedPropertyValue Captured(*Field.Property);
	if (!TryGetPropertyValue(ExactTarget, *Field.Property, Captured))
	{
		return EPropertyPreimageCaptureFailure::ReadFailed;
	}
	FAssetDocumentRuntimeValueDto Encoded;
	const FAssetDocumentEditorTypedValueCodecResult Encode =
		EncodeAssetDocumentEditorTypedValue(Captured, Encoded);
	if (!IsPropertyPreimageCodecResultExact(Encode, Field.CodecId))
	{
		return EPropertyPreimageCaptureFailure::EncodeFailed;
	}
	TUniquePtr<FAssetDocumentEditorScopedPropertyValue> RoundTrip;
	const FAssetDocumentEditorTypedValueCodecResult Decode =
		DecodeAssetDocumentEditorTypedValue(
			*Field.Property, Encoded, RoundTrip);
	if (!IsPropertyPreimageCodecResultExact(Decode, Field.CodecId))
	{
		return EPropertyPreimageCaptureFailure::EncodeFailed;
	}
	if (!IsPropertyPreimageRoundTripExact(
		*Field.Property, Captured, RoundTrip))
	{
		return EPropertyPreimageCaptureFailure::EncodeFailed;
	}
	OutSnapshot.DeclaringClass = Field.DeclaringClass;
	OutSnapshot.Property = Field.Property;
	OutSnapshot.OriginalFieldName = Field.Property->GetFName();
	OutSnapshot.TypeRevision = Field.TypeRevision;
	OutSnapshot.CodecId = Field.CodecId;
	OutSnapshot.EncodedValue = MoveTemp(Encoded);
	return EPropertyPreimageCaptureFailure::None;
}

EPropertyPreimageCaptureFailure CapturePropertyPreimageFields(
	UObject& ExactTarget,
	UClass& ExactTargetClass,
	TConstArrayView<FAssetDocumentEditorPropertyBatchTokenFieldView> Fields,
	TArray<FPropertyPreimageFieldSnapshot>& OutSnapshots,
	FString& OutFailedField)
{
	TSet<FProperty*> SeenProperties;
	OutSnapshots.Reserve(Fields.Num());
	for (const FAssetDocumentEditorPropertyBatchTokenFieldView& Field : Fields)
	{
		const EPropertyPreimageCaptureFailure Contract =
			ValidatePropertyPreimageField(
				ExactTargetClass, Field, SeenProperties);
		if (Contract != EPropertyPreimageCaptureFailure::None)
		{
			return Contract;
		}
		if (DoesAssetDocumentEditorPropertyHoldInstancedObjects(*Field.Property))
		{
			// Instanced subobjects have no identity-preserving typed preimage;
			// Core's package snapshot restores them (functional equivalence).
			continue;
		}
		FPropertyPreimageFieldSnapshot& Snapshot =
			OutSnapshots.AddDefaulted_GetRef();
		const EPropertyPreimageCaptureFailure Captured =
			CapturePropertyPreimageFieldValue(
				ExactTarget, Field, Snapshot);
		if (Captured != EPropertyPreimageCaptureFailure::None)
		{
			OutFailedField = Field.Property->GetAuthoredName();
			return Captured;
		}
	}
	return EPropertyPreimageCaptureFailure::None;
}

bool ArePropertyPreimageIdentityObjectsLive(
	const FPropertyPreimageIdentity& Identity)
{
	if (!IsLivePropertyPreimageObject(Identity.Target.Get()))
	{
		return false;
	}
	if (!IsLivePropertyPreimageObject(Identity.Package.Get()))
	{
		return false;
	}
	return IsLivePropertyPreimageObject(Identity.TargetClass.Get());
}

bool ArePropertyPreimageObjectLinksCurrent(
	const FPropertyPreimageIdentity& Identity)
{
	UObject* const Target = Identity.Target.Get();
	if (Target->GetOuter() != Identity.TargetOuter.Get())
	{
		return false;
	}
	if (Target->GetOutermost() != Identity.Package.Get())
	{
		return false;
	}
	return Target->GetClass() == Identity.TargetClass.Get();
}

bool IsPropertyPreimageObjectIdentityCurrent(
	const FPropertyPreimageIdentity& Identity)
{
	UObject* const Target = Identity.Target.Get();
	if (Target->GetPathName() != Identity.ObjectPath)
	{
		return false;
	}
	return static_cast<uint64>(Target->GetUniqueID())
		== Identity.ObjectRevision;
}

bool IsPropertyPreimagePackageIdentityCurrent(
	const FPropertyPreimageIdentity& Identity)
{
	UPackage* const Package = Identity.Package.Get();
	if (Package->GetName() != Identity.PackageName)
	{
		return false;
	}
	return static_cast<uint64>(Package->GetUniqueID())
		== Identity.PackageRevision;
}

EPropertyPreimageRestoreFailure ValidatePropertyPreimageIdentity(
	const FPropertyPreimageIdentity& Identity)
{
	if (!ArePropertyPreimageIdentityObjectsLive(Identity))
	{
		return EPropertyPreimageRestoreFailure::IdentityChanged;
	}
	if (!ArePropertyPreimageObjectLinksCurrent(Identity))
	{
		return EPropertyPreimageRestoreFailure::IdentityChanged;
	}
	if (!IsPropertyPreimageObjectIdentityCurrent(Identity))
	{
		return EPropertyPreimageRestoreFailure::IdentityChanged;
	}
	if (!IsPropertyPreimagePackageIdentityCurrent(Identity))
	{
		return EPropertyPreimageRestoreFailure::IdentityChanged;
	}
	return EPropertyPreimageRestoreFailure::None;
}

bool IsPropertyPreimageRestoreFieldIdentityCurrent(
	UClass& ExactTargetClass,
	UClass* DeclaringClass,
	FProperty* Property,
	const FPropertyPreimageFieldSnapshot& Snapshot)
{
	if (DeclaringClass == nullptr || Property == nullptr)
	{
		return false;
	}
	if (Property->GetOwnerClass() != DeclaringClass)
	{
		return false;
	}
	if (!ExactTargetClass.IsChildOf(DeclaringClass))
	{
		return false;
	}
	return Property->GetFName() == Snapshot.OriginalFieldName;
}

EPropertyPreimageRestoreFailure
ValidatePropertyPreimageRestoreField(
	UObject& Target,
	UClass& ExactTargetClass,
	const FPropertyPreimageFieldSnapshot& Snapshot)
{
	UClass* const DeclaringClass = Snapshot.DeclaringClass.Get();
	FProperty* const Property = Snapshot.Property.Get();
	if (!IsPropertyPreimageRestoreFieldIdentityCurrent(
		ExactTargetClass, DeclaringClass, Property, Snapshot))
	{
		return EPropertyPreimageRestoreFailure::TypeOrCodecChanged;
	}
	if (GetAssetDocumentEditorPropertyTypeRevision(*DeclaringClass)
		!= Snapshot.TypeRevision)
	{
		return EPropertyPreimageRestoreFailure::TypeOrCodecChanged;
	}
	if (!IsPropertyEditCodecCurrent(*Property, Snapshot.CodecId))
	{
		return EPropertyPreimageRestoreFailure::TypeOrCodecChanged;
	}
	if (PropertyAccessUtil::CanSetPropertyValue(
		Property,
		PropertyAccessUtil::EditorReadOnlyFlags,
		Target.IsTemplate()) != EPropertyAccessResultFlags::Success
		&& !IsAssetDocumentEditorInPlaceInstancedProperty(*Property))
	{
		return EPropertyPreimageRestoreFailure::AccessChanged;
	}
	return EPropertyPreimageRestoreFailure::None;
}

EPropertyPreimageRestoreFailure PreparePropertyPreimageRestoreValue(
	UObject& Target,
	const FPropertyPreimageFieldSnapshot& Snapshot,
	TArray<FPropertyPreimageRestoreField>& OutRestoreFields)
{
	FProperty& Property = *Snapshot.Property.Get();
	TUniquePtr<FAssetDocumentEditorScopedPropertyValue> Baseline;
	if (!TryDecodePropertyWriteValue(
		Property, Snapshot.CodecId, Snapshot.EncodedValue, Baseline))
	{
		return EPropertyPreimageRestoreFailure::DecodeFailed;
	}
	FAssetDocumentEditorScopedPropertyValue Current(Property);
	if (!TryGetPropertyValue(Target, Property, Current))
	{
		return EPropertyPreimageRestoreFailure::ReadFailed;
	}
	if (!AreAssetDocumentEditorTypedValuesEquivalent(Property, Current.GetMemory(), Baseline->GetMemory()))
	{
		FPropertyPreimageRestoreField& Restore =
			OutRestoreFields.AddDefaulted_GetRef();
		Restore.Property = &Property;
		Restore.Baseline = MoveTemp(Baseline);
	}
	return EPropertyPreimageRestoreFailure::None;
}

EPropertyPreimageRestoreFailure PreparePropertyPreimageRestoreField(
	UObject& Target,
	UClass& ExactTargetClass,
	const FPropertyPreimageFieldSnapshot& Snapshot,
	TArray<FPropertyPreimageRestoreField>& OutRestoreFields)
{
	const EPropertyPreimageRestoreFailure Validation =
		ValidatePropertyPreimageRestoreField(
			Target, ExactTargetClass, Snapshot);
	if (Validation != EPropertyPreimageRestoreFailure::None)
	{
		return Validation;
	}
	return PreparePropertyPreimageRestoreValue(
		Target, Snapshot, OutRestoreFields);
}

EPropertyPreimageRestoreFailure PreparePropertyPreimageRestoreFields(
	const FPropertyPreimageIdentity& Identity,
	TConstArrayView<FPropertyPreimageFieldSnapshot> Snapshots,
	TArray<FPropertyPreimageRestoreField>& OutRestoreFields,
	FString& OutFailedField)
{
	UObject& Target = *Identity.Target.Get();
	UClass& ExactTargetClass = *Identity.TargetClass.Get();
	if (!Target.CanModify())
	{
		return EPropertyPreimageRestoreFailure::AccessChanged;
	}
	OutRestoreFields.Reserve(Snapshots.Num());
	for (const FPropertyPreimageFieldSnapshot& Snapshot : Snapshots)
	{
		const EPropertyPreimageRestoreFailure Failure =
			PreparePropertyPreimageRestoreField(
				Target, ExactTargetClass, Snapshot, OutRestoreFields);
		if (Failure != EPropertyPreimageRestoreFailure::None)
		{
			if (const FProperty* Property = Snapshot.Property.Get())
			{
				OutFailedField = Property->GetAuthoredName();
			}
			return Failure;
		}
	}
	return EPropertyPreimageRestoreFailure::None;
}

EPropertyPreimageRestoreFailure ApplyPropertyPreimageRestoreFields(
	UObject& Target,
	TConstArrayView<FPropertyPreimageRestoreField> RestoreFields,
	FString& OutFailedField)
{
	for (const FPropertyPreimageRestoreField& Restore : RestoreFields)
	{
		OutFailedField = Restore.Property->GetAuthoredName();
		if (!TrySetPropertyValue(
			Target,
			*Restore.Property,
			*Restore.Baseline,
			EPropertyAccessChangeNotifyMode::Default,
			true))
		{
			return EPropertyPreimageRestoreFailure::WriteFailed;
		}
		FAssetDocumentEditorScopedPropertyValue Readback(*Restore.Property);
		if (!TryGetPropertyValue(Target, *Restore.Property, Readback)
			|| !AreAssetDocumentEditorTypedValuesEquivalent(
				*Restore.Property, Readback.GetMemory(), Restore.Baseline->GetMemory()))
		{
			return EPropertyPreimageRestoreFailure::ReadbackMismatch;
		}
	}
	OutFailedField.Reset();
	return EPropertyPreimageRestoreFailure::None;
}

EPropertyPreimageRestoreFailure RestorePropertyPreimageDirtyState(
	const FPropertyPreimageIdentity& Identity)
{
	UPackage& Package = *Identity.Package.Get();
	Package.SetDirtyFlag(Identity.bOriginalPackageDirty);
	return Package.IsDirty() == Identity.bOriginalPackageDirty
		? EPropertyPreimageRestoreFailure::None
		: EPropertyPreimageRestoreFailure::DirtyStateRestoreFailed;
}

bool IsPropertyPreimageExecutionContextValid()
{
	if (!IsInGameThread())
	{
		return false;
	}
	return GIsEditor;
}

EPropertyPreimageRestoreFailure AttemptPropertyPreimageRestore(
	const FPropertyPreimageIdentity& Identity,
	TConstArrayView<FPropertyPreimageFieldSnapshot> Snapshots,
	bool bRestoreDirty,
	FString& OutFailedField)
{
	EPropertyPreimageRestoreFailure Failure =
		ValidatePropertyPreimageIdentity(Identity);
	TArray<FPropertyPreimageRestoreField> RestoreFields;
	if (Failure == EPropertyPreimageRestoreFailure::None)
	{
		Failure = PreparePropertyPreimageRestoreFields(
			Identity, Snapshots, RestoreFields, OutFailedField);
	}
	if (Failure == EPropertyPreimageRestoreFailure::None)
	{
		Failure = ApplyPropertyPreimageRestoreFields(
			*Identity.Target.Get(), RestoreFields, OutFailedField);
	}
	if (bRestoreDirty && Failure == EPropertyPreimageRestoreFailure::None)
	{
		Failure = RestorePropertyPreimageDirtyState(Identity);
	}
	return Failure;
}
} // namespace

struct FAssetDocumentEditorPropertyPreimage::FImpl
{
	FPropertyPreimageIdentity Identity;
	TArray<FPropertyPreimageFieldSnapshot> Snapshots;
	EPropertyPreimageState State = EPropertyPreimageState::Ready;
};

FAssetDocumentEditorPropertyPreimage::
	FAssetDocumentEditorPropertyPreimage() = default;

FAssetDocumentEditorPropertyPreimage::
	~FAssetDocumentEditorPropertyPreimage() = default;

FAssetDocumentEditorPropertyPreimage::
	FAssetDocumentEditorPropertyPreimage(
		FAssetDocumentEditorPropertyPreimage&& Other) = default;

FAssetDocumentEditorPropertyPreimage&
FAssetDocumentEditorPropertyPreimage::operator=(
	FAssetDocumentEditorPropertyPreimage&& Other) = default;

bool FAssetDocumentEditorPropertyPreimage::IsReady() const
{
	return Impl && Impl->State == EPropertyPreimageState::Ready;
}

FAssetDocumentContractValidationResult
FAssetDocumentEditorPropertyPreimage::Restore()
{
	return RestoreInternal(true);
}

FAssetDocumentContractValidationResult
FAssetDocumentEditorPropertyPreimage::RestoreFieldsOnly()
{
	return RestoreInternal(false);
}

FAssetDocumentContractValidationResult
FAssetDocumentEditorPropertyPreimage::RestoreInternal(bool bRestoreDirty)
{
	if (!Impl)
	{
		return FailPropertyPreimageRestore(
			TEXT("PropertyPreimage.InvalidState"),
			TEXT("Property preimage restore requires one ready preimage."));
	}
	if (Impl->State == EPropertyPreimageState::Consumed)
	{
		return FailPropertyPreimageRestore(
			TEXT("PropertyPreimage.AlreadyConsumed"),
			TEXT("The property preimage restore was already attempted."));
	}
	Impl->State = EPropertyPreimageState::Consumed;
	if (!IsPropertyPreimageExecutionContextValid())
	{
		return FailPropertyPreimageRestore(
			TEXT("PropertyPreimage.InvalidExecutionContext"),
			TEXT("Property preimage restore requires the Editor GameThread."));
	}
	FString FailedField;
	const EPropertyPreimageRestoreFailure Failure =
		AttemptPropertyPreimageRestore(
			Impl->Identity, Impl->Snapshots, bRestoreDirty, FailedField);
	if (Failure != EPropertyPreimageRestoreFailure::None)
	{
		const FPropertyPreimageFailureDescription& Description =
			GetPropertyPreimageRestoreFailure(Failure);
		const FString Message = FailedField.IsEmpty()
			? FString(Description.Message)
			: FString::Printf(TEXT("%s (property '%s')"), Description.Message, *FailedField);
		return FailPropertyPreimageRestore(
			Description.Code, *Message);
	}
	FAssetDocumentContractValidationResult Result;
	Result.bValid = true;
	return Result;
}

FAssetDocumentEditorPropertyPreimageCaptureResult
CaptureAssetDocumentEditorAdmittedPropertyPreimage(
	UObject& ExactTarget,
	TConstArrayView<
		FAssetDocumentEditorPropertyBatchTokenFieldView> Fields)
{
	if (!IsPropertyPreimageExecutionContextValid())
	{
		return FailPropertyPreimageCapture(
			TEXT("PropertyPreimage.InvalidExecutionContext"),
			TEXT("Property preimage capture requires the Editor GameThread."));
	}
	FPropertyPreimageIdentity Identity;
	if (!TryCapturePropertyPreimageIdentity(ExactTarget, Identity))
	{
		return FailPropertyPreimageCapture(
			TEXT("PropertyPreimage.InvalidTarget"),
			TEXT("Property preimage capture requires one live exact target."));
	}
	UClass* const ExactTargetClass = Identity.TargetClass.Get();
	TArray<FPropertyPreimageFieldSnapshot> Snapshots;
	FString FailedField;
	const EPropertyPreimageCaptureFailure Failure =
		CapturePropertyPreimageFields(
			ExactTarget, *ExactTargetClass, Fields, Snapshots, FailedField);
	if (Failure != EPropertyPreimageCaptureFailure::None)
	{
		const FPropertyPreimageFailureDescription& Description =
			GetPropertyPreimageCaptureFailure(Failure);
		const FString Message = FailedField.IsEmpty()
			? FString(Description.Message)
			: FString::Printf(TEXT("%s (property '%s')"), Description.Message, *FailedField);
		return FailPropertyPreimageCapture(
			Description.Code, *Message);
	}
	FAssetDocumentEditorPropertyPreimageCaptureResult Result;
	Result.Preimage.Impl =
		MakeUnique<FAssetDocumentEditorPropertyPreimage::FImpl>();
	Result.Preimage.Impl->Identity = MoveTemp(Identity);
	Result.Preimage.Impl->Snapshots = MoveTemp(Snapshots);
	Result.bSucceeded = true;
	return Result;
}

FAssetDocumentEditorPreparedPropertyWrite::
	FAssetDocumentEditorPreparedPropertyWrite() = default;

FAssetDocumentEditorPreparedPropertyWrite::
	~FAssetDocumentEditorPreparedPropertyWrite() = default;

FAssetDocumentEditorPreparedPropertyWrite::
	FAssetDocumentEditorPreparedPropertyWrite(
		FAssetDocumentEditorPreparedPropertyWrite&& Other) = default;

FAssetDocumentEditorPreparedPropertyWrite&
FAssetDocumentEditorPreparedPropertyWrite::operator=(
	FAssetDocumentEditorPreparedPropertyWrite&& Other) = default;

TConstArrayView<FAssetDocumentMutationFootprint>
FAssetDocumentEditorPreparedPropertyWrite::GetFootprint() const
{
	return Footprint;
}

bool TryPrepareAssetDocumentEditorAdmittedPropertyValue(
	const FAssetDocumentEditorPropertyTokenView& Token,
	const FAssetDocumentRuntimeValueDto& DesiredValue,
	FAssetDocumentEditorPreparedPropertyWrite& OutPrepared,
	FAssetDocumentEditorPropertyEditResult& OutFailure,
	UObject* Owner)
{
	OutPrepared = FAssetDocumentEditorPreparedPropertyWrite();
	if (!Token.bWritable || !IsPropertyEditCodecCurrent(Token))
	{
		OutFailure = FailPropertyEdit(
			TEXT("PropertyToken.InvalidPayload"),
			TEXT("Property write requires a live writable token."));
		return false;
	}
	FAssetDocumentEditorTypedValueCodecResult Decode;
	if (!TryDecodePropertyWriteValue(
		Token, DesiredValue, OutPrepared.NativeValue, &Decode, Owner))
	{
		OutFailure = FailPropertyDecode(
			TEXT("PropertyWrite.DecodeFailed"),
			Token,
			TEXT("desired"),
			Decode);
		return false;
	}
	return true;
}

bool TryPrepareAssetDocumentEditorAdmittedPropertyWrite(
	UObject& Asset,
	const FAssetDocumentEditorPropertyTokenView& Token,
	const FAssetDocumentRuntimeValueDto& DesiredValue,
	FAssetDocumentEditorPreparedPropertyWrite& OutPrepared,
	FAssetDocumentEditorPropertyEditResult& OutFailure)
{
	if (!TryPrepareAssetDocumentEditorAdmittedPropertyValue(
		Token, DesiredValue, OutPrepared, OutFailure, &Asset))
	{
		return false;
	}
	FString ServiceCode;
	FString ServiceMessage;
	if (!CheckAssetDocumentEditorChangeNotificationSupported(Asset, ServiceCode, ServiceMessage))
	{
		OutFailure = FailPropertyEdit(*ServiceCode, *ServiceMessage);
		return false;
	}
	FAssetDocumentEditorMutationIdentity Identity;
	if (!TryBuildAssetDocumentEditorMutationIdentity(
		Asset.GetPathName(), Identity))
	{
		OutFailure = FailPropertyEdit(
			TEXT("PropertyWrite.InvalidTarget"),
			TEXT("The admitted target identity is not canonical."));
		return false;
	}
	OutPrepared.Footprint =
		BuildAssetDocumentEditorPropertyMutationFootprint(Identity);
	return true;
}

FAssetDocumentEditorPropertyEditResult
ApplyAssetDocumentEditorPreparedPropertyWriteWithoutEnlist(
	UObject& Asset,
	const FAssetDocumentEditorPropertyTokenView& Token,
	FAssetDocumentEditorPreparedPropertyWrite&& Prepared,
	FStringView AuthorityId)
{
	TUniquePtr<FAssetDocumentEditorScopedPropertyValue> NativeValue =
		MoveTemp(Prepared.NativeValue);
	if (!NativeValue)
	{
		return FailPropertyEdit(
			TEXT("PropertyWrite.PreparedAlreadyConsumed"),
			TEXT("The prepared property write has already been consumed."));
	}
	if (&NativeValue->GetProperty() != Token.Property)
	{
		return FailPropertyEdit(
			TEXT("PropertyWrite.PreparedTokenMismatch"),
			TEXT("The prepared property write does not match the admitted token."));
	}
	if (!TrySetPropertyValue(
		Asset,
		Token,
		*NativeValue,
		EPropertyAccessChangeNotifyMode::Default,
		true))
	{
		return FailPropertyEdit(
			TEXT("PropertyWrite.AccessFailed"),
			TEXT("The admitted property could not be written."));
	}
	return CompletePropertyWrite(Asset, Token, AuthorityId);
}

FAssetDocumentEditorPropertyEditResult
ReadAssetDocumentEditorAdmittedProperty(
	UObject& Asset,
	const FAssetDocumentEditorPropertyTokenView& Token,
	const FAssetDocumentRuntimeValueDto* AsAuthored)
{
	if (!IsPropertyEditCodecCurrent(Token))
	{
		return FailPropertyEdit(
			TEXT("PropertyToken.InvalidPayload"),
			TEXT("The property token codec no longer matches its field."));
	}
	FAssetDocumentEditorScopedPropertyValue NativeValue(*Token.Property);
	if (!TryReadPropertyNativeValue(Asset, Token, NativeValue))
	{
		return FailPropertyEdit(
			TEXT("PropertyRead.AccessFailed"),
			TEXT("The admitted property could not be read."));
	}
	FAssetDocumentEditorPropertyEditResult Result;
	if (!TryBuildPropertyReadValue(Token, NativeValue, Result.Value, AsAuthored))
	{
		return FailPropertyEdit(
			TEXT("PropertyRead.EncodeFailed"),
			TEXT("The admitted property value could not be encoded."));
	}
	Result.bSucceeded = true;
	return Result;
}

FAssetDocumentEditorPropertyEditResult
ReadAssetDocumentEditorAdmittedDocumentProperty(
	UObject& Asset,
	const FAssetDocumentEditorPropertyTokenView& Token,
	const FAssetDocumentRuntimeValueDto* AsAuthored)
{
	if (!IsPropertyEditCodecCurrent(Token))
	{
		return FailPropertyEdit(
			TEXT("PropertyToken.InvalidPayload"),
			TEXT("The property token codec no longer matches its field."));
	}
	FAssetDocumentEditorScopedPropertyValue NativeValue(*Token.Property);
	FString ValuePath;
	if (!TryReadPropertyNativeValue(Asset, Token, NativeValue)
		|| FindAssetDocumentEditorDerivedReference(*Token.Property, NativeValue.GetMemory(), Asset, ValuePath) == nullptr)
	{
		return ReadAssetDocumentEditorAdmittedProperty(Asset, Token, AsAuthored);
	}
	FAssetDocumentEditorPropertyEditResult Result;
	FString DiagnosticCode;
	const TArray<FAssetDocumentEditorValueField> Fields = {
		{TEXT("editor_derived"), MakePropertyBooleanValue(true)},
	};
	if (!TryBuildAssetDocumentEditorValueObject(Fields, Result.Value, DiagnosticCode))
	{
		return FailPropertyEdit(
			TEXT("PropertyRead.EncodeFailed"),
			TEXT("The admitted property value could not be encoded."));
	}
	Result.bSucceeded = true;
	return Result;
}

FAssetDocumentEditorPropertyEditResult
ReadAssetDocumentEditorAdmittedPropertyChanged(
	UObject& Asset,
	const FAssetDocumentEditorPropertyTokenView& Token,
	UObject& Defaults)
{
	FAssetDocumentEditorPropertyEditResult Read = ReadAssetDocumentEditorAdmittedProperty(Asset, Token, nullptr);
	if (!Read.bSucceeded || Read.Value.Kind != EAssetDocumentRuntimeValueKind::Object || Read.Value.Elements.Num() != 1)
	{
		return Read;
	}
	FAssetDocumentEditorScopedPropertyValue DefaultNative(*Token.Property);
	FAssetDocumentRuntimeValueDto Default;
	if (TryReadPropertyNativeValue(Defaults, Token, DefaultNative)
		&& EncodeAssetDocumentEditorTypedValue(DefaultNative, Default).bSucceeded)
	{
		UE::AssetDocument::EditorPropertyValueCodec::PruneToChanged(*Token.Property, Read.Value.Elements[0], Default);
	}
	return Read;
}

FAssetDocumentEditorPropertyEditResult
WriteAssetDocumentEditorAdmittedProperty(
	UObject& Asset,
	const FAssetDocumentEditorPropertyTokenView& Token,
	const FAssetDocumentRuntimeValueDto& DesiredValue,
	IAssetDocumentAtomicTransaction& Transaction)
{
	FAssetDocumentEditorPreparedPropertyWrite Prepared;
	FAssetDocumentEditorPropertyEditResult Failure;
	if (!TryPrepareAssetDocumentEditorAdmittedPropertyWrite(
		Asset, Token, DesiredValue, Prepared, Failure))
	{
		return Failure;
	}
	const FAssetDocumentContractValidationResult Enlist =
		Transaction.EnlistFootprint(Prepared.GetFootprint());
	if (!Enlist.bValid)
	{
		TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics =
			Enlist.Diagnostics;
		return FailPropertyEdit(MoveTemp(Diagnostics));
	}
	return ApplyAssetDocumentEditorPreparedPropertyWriteWithoutEnlist(
		Asset,
		Token,
		MoveTemp(Prepared),
		TEXT("editor.property.write_admitted.v1"));
}

FAssetDocumentEditorPropertyEditResult
PreflightAssetDocumentEditorAdmittedProperty(
	UObject& Scratch,
	const FAssetDocumentEditorPropertyTokenView& Token,
	const FAssetDocumentRuntimeValueDto& DesiredValue)
{
	TUniquePtr<FAssetDocumentEditorScopedPropertyValue> NativeValue;
	FAssetDocumentEditorPropertyEditResult Failure;
	if (!TryPreparePropertyPreflight(
		Scratch, Token, DesiredValue, NativeValue, Failure))
	{
		return Failure;
	}
	// Refused here, before a target exists, so Apply never reaches the write.
	FString ServiceCode;
	FString ServiceMessage;
	if (!CheckAssetDocumentEditorChangeNotificationSupported(Scratch, ServiceCode, ServiceMessage))
	{
		return FailPropertyEdit(*ServiceCode, *ServiceMessage);
	}
	// Notify like the live write (and the Details panel): a value the class's
	// PostEditChange rewrites (clamped, renamed, regenerated) reads back as what the
	// editor stores, so it is refused here instead of failing after mutation.
	// PostEditChange may reach other packages (e.g. a sprite's atlas); the isolation
	// guard restores them, so the probe never changes a real asset (§4.3).
	bool bWritten = false;
	TArray<FAssetDocumentRuntimeDiagnostic> IsolationDiagnostics;
	bool bIsolated = false;
	{
		FAssetDocumentEditorPreflightIsolation Isolation(FString::Printf(TEXT("%s.%s"),
			*Scratch.GetClass()->GetName(), *Token.Property->GetAuthoredName()));
		bWritten = TrySetPropertyValue(
			Scratch,
			Token,
			*NativeValue,
			EPropertyAccessChangeNotifyMode::Default,
			false);
		bIsolated = Isolation.Restore(IsolationDiagnostics);
	}
	if (!bIsolated)
	{
		IsolationDiagnostics.RemoveAll([](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
		{
			return Diagnostic.Severity != EAssetDocumentDiagnosticSeverity::Error;
		});
		return FailPropertyEdit(MoveTemp(IsolationDiagnostics));
	}
	if (!bWritten)
	{
		return FailPropertyEdit(
			TEXT("PropertyPreflight.AccessFailed"),
			TEXT("The admitted scratch property could not be written."));
	}
	return CompletePropertyPreflight(Scratch, Token, DesiredValue);
}
