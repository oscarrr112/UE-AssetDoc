// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Property/AssetDocumentEditorPropertyAdmission.h"

#include "Provider/AssetDocumentEditorTypedValueCodec.h"
#include "Provider/AssetDocumentEditorValueDto.h"
#include "Provider/Property/AssetDocumentEditorDeclaredPropertyAccess.h"
#include "Provider/Property/AssetDocumentEditorPropertyBatchTokenPayload.h"
#include "Provider/Property/AssetDocumentEditorPropertyTokenPayload.h"
#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/PropertyAccessUtil.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

namespace
{
struct FAdmittedPropertyCandidate
{
	FProperty* Property = nullptr;
	FString Name;
	FString DeclaringClass;
	FString CodecId;
	uint64 TypeRevision = 0;
	bool bWritable = false;
	bool bDeclared = false;
};

enum class EPropertyFieldProjection
{
	All,
	WritableOnly,
};

FAssetDocumentRuntimeDiagnostic MakePropertyAdmissionDiagnostic(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = TEXT("/Primitive/exact_asset_class_path");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Diagnostic;
}

FAssetDocumentEditorPropertyAdmissionResult FailPropertyAdmission(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentEditorPropertyAdmissionResult Result;
	Result.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	Result.Diagnostics.Add(MakePropertyAdmissionDiagnostic(Code, Message));
	return Result;
}

FAssetDocumentEditorPropertyPreimageCaptureResult FailPropertyPreimageAdmission(
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

UClass* ResolveExactPropertyClass(FStringView ExactClassPath)
{
	const FString RequestedPath(ExactClassPath);
	UClass* Resolved = FSoftClassPath(RequestedPath).TryLoadClass<UObject>();
	return Resolved != nullptr && Resolved->GetPathName() == RequestedPath
		? Resolved
		: nullptr;
}

UClass* GetExactLivePropertyTargetClass(UObject& ExactTarget)
{
	UClass* ExactClass = ExactTarget.GetClass();
	return IsValid(&ExactTarget)
			&& ExactClass != nullptr
			&& IsValid(ExactClass)
		? ExactClass
		: nullptr;
}

bool IsAuthoredProperty(const FProperty& Property)
{
	constexpr uint64 RejectedFlags =
		CPF_Transient | CPF_DuplicateTransient | CPF_Deprecated | CPF_SkipSerialization;
	return Property.HasAnyPropertyFlags(CPF_Edit)
		&& !Property.HasAnyPropertyFlags(RejectedFlags);
}

bool TryAdmitProperty(
	FProperty& Property,
	FAdmittedPropertyCandidate& OutCandidate)
{
	if (!IsAuthoredProperty(Property)
		|| PropertyAccessUtil::CanGetPropertyValue(&Property)
			!= EPropertyAccessResultFlags::Success)
	{
		return false;
	}
	const FAssetDocumentEditorTypedValueCodecResult Codec =
		InspectAssetDocumentEditorTypedValueProperty(Property);
	if (!Codec.bSucceeded)
	{
		return false;
	}
	OutCandidate.Property = &Property;
	OutCandidate.Name = Property.GetAuthoredName();
	OutCandidate.DeclaringClass = Property.GetOwnerClass()->GetPathName();
	OutCandidate.CodecId = Codec.CodecId;
	OutCandidate.TypeRevision =
		GetAssetDocumentEditorPropertyTypeRevision(
			*Property.GetOwnerClass());
	OutCandidate.bWritable =
		PropertyAccessUtil::CanSetPropertyValue(
			&Property, PropertyAccessUtil::EditorReadOnlyFlags, false)
		== EPropertyAccessResultFlags::Success
		|| IsAssetDocumentEditorInPlaceInstancedProperty(Property);
	return true;
}

FAssetDocumentRuntimeDiagnostic MakeDeclaredPropertyDiagnostic(
	const FString& Name,
	const TCHAR* Code,
	const FString& Message)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = FString::Printf(TEXT("/Primitive/admit_properties/%s"), *Name);
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Diagnostic;
}

FProperty* FindDeclaredProperty(UClass& Class, const FString& Name)
{
	for (TFieldIterator<FProperty> It(&Class); It; ++It)
	{
		if (It->GetAuthoredName() == Name)
		{
			return *It;
		}
	}
	return nullptr;
}

// A declared name is admitted only when the property exists, is serialized,
// is not EditConst and has a typed codec. Edit-admitted writable properties
// keep their ordinary admission.
bool TryAdmitDeclaredProperty(
	UClass& Class,
	const FString& Name,
	TArray<FAdmittedPropertyCandidate>& InOutCandidates,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	FProperty* Property = FindDeclaredProperty(Class, Name);
	if (Property == nullptr)
	{
		OutDiagnostics.Add(MakeDeclaredPropertyDiagnostic(Name,
			TEXT("PropertyAdmission.DeclaredPropertyUnknown"),
			FString::Printf(TEXT("Declared property '%s' does not exist on %s."), *Name, *Class.GetPathName())));
		return false;
	}
	FAdmittedPropertyCandidate* Existing = InOutCandidates.FindByPredicate(
		[Property](const FAdmittedPropertyCandidate& Candidate) { return Candidate.Property == Property; });
	if (Existing != nullptr && Existing->bWritable)
	{
		return true;
	}
	if (!IsAssetDocumentEditorDeclarableProperty(*Property))
	{
		OutDiagnostics.Add(MakeDeclaredPropertyDiagnostic(Name,
			TEXT("PropertyAdmission.DeclaredPropertyRejected"),
			FString::Printf(TEXT("Declared property '%s' is transient, deprecated, not serialized or EditConst."), *Name)));
		return false;
	}
	const FAssetDocumentEditorTypedValueCodecResult Codec =
		InspectAssetDocumentEditorTypedValueProperty(*Property);
	if (!Codec.bSucceeded)
	{
		OutDiagnostics.Add(MakeDeclaredPropertyDiagnostic(Name,
			TEXT("PropertyAdmission.DeclaredPropertyUnsupportedCodec"),
			FString::Printf(TEXT("Declared property '%s' has no supported typed value codec."), *Name)));
		return false;
	}
	FAdmittedPropertyCandidate Candidate;
	Candidate.Property = Property;
	Candidate.Name = Property->GetAuthoredName();
	Candidate.DeclaringClass = Property->GetOwnerClass()->GetPathName();
	Candidate.CodecId = Codec.CodecId;
	Candidate.TypeRevision = GetAssetDocumentEditorPropertyTypeRevision(*Property->GetOwnerClass());
	Candidate.bWritable = true;
	Candidate.bDeclared = true;
	if (Existing != nullptr)
	{
		*Existing = MoveTemp(Candidate);
	}
	else
	{
		InOutCandidates.Add(MoveTemp(Candidate));
	}
	return true;
}

TArray<FAdmittedPropertyCandidate> GatherAdmittedProperties(
	UClass& Class,
	TConstArrayView<FString> DeclaredNames = {},
	TArray<FAssetDocumentRuntimeDiagnostic>* OutDiagnostics = nullptr)
{
	TArray<FAdmittedPropertyCandidate> Candidates;
	for (TFieldIterator<FProperty> It(&Class); It; ++It)
	{
		FAdmittedPropertyCandidate Candidate;
		if (TryAdmitProperty(**It, Candidate))
		{
			Candidates.Add(MoveTemp(Candidate));
		}
	}
	TArray<FAssetDocumentRuntimeDiagnostic> DeclaredDiagnostics;
	for (const FString& Name : DeclaredNames)
	{
		TryAdmitDeclaredProperty(Class, Name, Candidates, DeclaredDiagnostics);
	}
	if (!DeclaredDiagnostics.IsEmpty())
	{
		if (OutDiagnostics != nullptr)
		{
			*OutDiagnostics = MoveTemp(DeclaredDiagnostics);
		}
		return {};
	}
	Candidates.Sort([](
		const FAdmittedPropertyCandidate& Left,
		const FAdmittedPropertyCandidate& Right)
	{
		return Left.Name == Right.Name
			? Left.DeclaringClass < Right.DeclaringClass
			: Left.Name < Right.Name;
	});
	return Candidates;
}

FAssetDocumentRuntimeValueDto MakePropertyAdmissionBooleanValue(bool bValue)
{
	FAssetDocumentRuntimeValueDto Value;
	Value.Kind = EAssetDocumentRuntimeValueKind::Boolean;
	Value.BooleanValue = bValue;
	return Value;
}

FAssetDocumentRuntimeValueDto MakePropertyAdmissionStringValue(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

// The class default of a property in its codec form, read from the exact class's
// default object. Extract leaves out values equal to it (diff_only). False when the
// value cannot be read or encoded; the row then carries no default and the property
// is always extracted.
bool TryEncodePropertyDefault(
	const FAdmittedPropertyCandidate& Candidate,
	const UObject& DefaultObject,
	FAssetDocumentRuntimeValueDto& OutDefault)
{
	FAssetDocumentEditorScopedPropertyValue NativeValue(*Candidate.Property);
	if (!NativeValue.IsValid())
	{
		return false;
	}
	const bool bRead = Candidate.bDeclared
		? GetAssetDocumentEditorDeclaredPropertyValue(DefaultObject, *Candidate.Property, NativeValue.GetMemory())
		: ReadAssetDocumentEditorObjectPropertyValue(DefaultObject, *Candidate.Property, NativeValue);
	if (!bRead)
	{
		return false;
	}
	const FAssetDocumentEditorTypedValueCodecResult Encoded =
		EncodeAssetDocumentEditorTypedValue(NativeValue, OutDefault);
	if (!Encoded.bSucceeded || Encoded.CodecId != Candidate.CodecId)
	{
		OutDefault = {};
		return false;
	}
	return true;
}

bool TryBuildPropertyRow(
	const FAdmittedPropertyCandidate& Candidate,
	const UObject* DefaultObject,
	FAssetDocumentRuntimeValueDto& OutRow)
{
	FString DiagnosticCode;
	TArray<FAssetDocumentEditorValueField> Fields = {
		{TEXT("name"), MakePropertyAdmissionStringValue(Candidate.Name)},
		{TEXT("declaring_class"),
			MakePropertyAdmissionStringValue(Candidate.DeclaringClass)},
		{TEXT("codec_id"), MakePropertyAdmissionStringValue(Candidate.CodecId)},
		{TEXT("readable"), MakePropertyAdmissionBooleanValue(true)},
		{TEXT("writable"),
			MakePropertyAdmissionBooleanValue(Candidate.bWritable)},
	};
	FAssetDocumentRuntimeValueDto Default;
	if (DefaultObject != nullptr && TryEncodePropertyDefault(Candidate, *DefaultObject, Default))
	{
		Fields.Add({TEXT("default"), MoveTemp(Default)});
	}
	return TryBuildAssetDocumentEditorValueObject(
		Fields, OutRow, DiagnosticCode);
}

bool TryIssuePropertyToken(
	const FAdmittedPropertyCandidate& Candidate,
	IAssetDocumentHandleResolver& Handles,
	FAssetDocumentOpaqueHandle& OutHandle,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	UClass& DeclaringClass = *Candidate.Property->GetOwnerClass();
	FAssetDocumentIssuedHandleResult Issued = Handles.Issue(
		EAssetDocumentHandleKind::Property,
		EAssetDocumentHandleAccess::ReadOnly,
		MakeUnique<FAssetDocumentEditorPropertyTokenPayload>(
			DeclaringClass,
			*Candidate.Property,
			Candidate.CodecId,
			true,
			Candidate.bWritable,
			Candidate.TypeRevision,
			Candidate.bDeclared));
	if (!Issued.Handle.IsSet())
	{
		OutDiagnostics = MoveTemp(Issued.Diagnostics);
		return false;
	}
	OutHandle = Issued.Handle.GetValue();
	return true;
}

bool TryBuildPropertyRows(
	TConstArrayView<FAdmittedPropertyCandidate> Candidates,
	const UObject* DefaultObject,
	FAssetDocumentRuntimeValueDto& OutValue)
{
	FAssetDocumentRuntimeValueDto Rows;
	Rows.Kind = EAssetDocumentRuntimeValueKind::Array;
	for (const FAdmittedPropertyCandidate& Candidate : Candidates)
	{
		FAssetDocumentRuntimeValueDto& Row = Rows.Elements.AddDefaulted_GetRef();
		if (!TryBuildPropertyRow(Candidate, DefaultObject, Row))
		{
			return false;
		}
	}
	FString DiagnosticCode;
	const TArray<FAssetDocumentEditorValueField> Fields = {
		{TEXT("properties"), MoveTemp(Rows)},
	};
	return TryBuildAssetDocumentEditorValueObject(
		Fields, OutValue, DiagnosticCode);
}

bool TryIssuePropertyTokens(
	TConstArrayView<FAdmittedPropertyCandidate> Candidates,
	IAssetDocumentHandleResolver& Handles,
	TArray<FAssetDocumentOpaqueHandle>& OutHandles,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	TArray<FAssetDocumentOpaqueHandle> IssuedHandles;
	IssuedHandles.Reserve(Candidates.Num());
	for (const FAdmittedPropertyCandidate& Candidate : Candidates)
	{
		FAssetDocumentOpaqueHandle& Handle =
			IssuedHandles.AddDefaulted_GetRef();
		if (!TryIssuePropertyToken(
			Candidate, Handles, Handle, OutDiagnostics))
		{
			return false;
		}
	}
	OutHandles = MoveTemp(IssuedHandles);
	return true;
}

FAssetDocumentEditorPropertyBatchTokenFieldView MakePropertyBatchField(
	const FAdmittedPropertyCandidate& Candidate)
{
	FAssetDocumentEditorPropertyBatchTokenFieldView Field;
	Field.DeclaringClass = Candidate.Property->GetOwnerClass();
	Field.Property = Candidate.Property;
	Field.CodecId = Candidate.CodecId;
	Field.TypeRevision = Candidate.TypeRevision;
	Field.bReadable = true;
	Field.bWritable = Candidate.bWritable;
	Field.bDeclared = Candidate.bDeclared;
	return Field;
}

TArray<FAssetDocumentEditorPropertyBatchTokenFieldView>
ProjectPropertyBatchFields(
	TConstArrayView<FAdmittedPropertyCandidate> Candidates,
	EPropertyFieldProjection Projection)
{
	TArray<FAssetDocumentEditorPropertyBatchTokenFieldView> Fields;
	Fields.Reserve(Candidates.Num());
	for (const FAdmittedPropertyCandidate& Candidate : Candidates)
	{
		if (Projection == EPropertyFieldProjection::WritableOnly
			&& !Candidate.bWritable)
		{
			continue;
		}
		Fields.Add(MakePropertyBatchField(Candidate));
	}
	return Fields;
}

bool TryIssuePropertyBatchToken(
	UObject& ExactTarget,
	TConstArrayView<FAdmittedPropertyCandidate> Candidates,
	IAssetDocumentHandleResolver& Handles,
	TOptional<FAssetDocumentOpaqueHandle>& OutHandle,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	const TArray<FAssetDocumentEditorPropertyBatchTokenFieldView> Fields =
		ProjectPropertyBatchFields(
			Candidates, EPropertyFieldProjection::All);
	FAssetDocumentIssuedHandleResult Issued = Handles.Issue(
		EAssetDocumentHandleKind::Property,
		EAssetDocumentHandleAccess::ReadOnly,
		MakeUnique<FAssetDocumentEditorPropertyBatchTokenPayload>(
			ExactTarget, Fields));
	if (!Issued.Handle.IsSet())
	{
		OutDiagnostics = MoveTemp(Issued.Diagnostics);
		return false;
	}
	OutHandle = Issued.Handle.GetValue();
	return true;
}

FAssetDocumentEditorPropertyAdmissionResult FailBuiltPropertyAdmission(
	FAssetDocumentEditorPropertyAdmissionResult Result)
{
	if (Result.Diagnostics.IsEmpty())
	{
		Result.Diagnostics.Add(MakePropertyAdmissionDiagnostic(
			TEXT("PropertyAdmission.TokenIssueFailed"),
			TEXT("An admitted property token could not be issued.")));
	}
	Result.Value = {};
	Result.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	Result.Handles.Reset();
	Result.PropertyBatchHandle.Reset();
	return Result;
}

FAssetDocumentEditorPropertyAdmissionResult BuildPropertyAdmission(
	UClass& ExactClass,
	UObject* ExactTarget,
	IAssetDocumentHandleResolver& Handles,
	TConstArrayView<FString> DeclaredNames)
{
	FAssetDocumentEditorPropertyAdmissionResult Result;
	const TArray<FAdmittedPropertyCandidate> Candidates =
		GatherAdmittedProperties(ExactClass, DeclaredNames, &Result.Diagnostics);
	if (!Result.Diagnostics.IsEmpty())
	{
		Result.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
		return Result;
	}
	if (!TryBuildPropertyRows(Candidates, ExactClass.GetDefaultObject(), Result.Value))
	{
		return FailBuiltPropertyAdmission(MoveTemp(Result));
	}
	if (!TryIssuePropertyTokens(
		Candidates, Handles, Result.Handles, Result.Diagnostics))
	{
		return FailBuiltPropertyAdmission(MoveTemp(Result));
	}
	if (ExactTarget != nullptr)
	{
		if (!TryIssuePropertyBatchToken(
			*ExactTarget,
			Candidates,
			Handles,
			Result.PropertyBatchHandle,
			Result.Diagnostics))
		{
			return FailBuiltPropertyAdmission(MoveTemp(Result));
		}
	}
	Result.bSucceeded = true;
	return Result;
}
} // namespace

bool TryReadAssetDocumentEditorDeclaredPropertyNames(
	const FAssetDocumentRuntimeValueDto& Arguments,
	TArray<FString>& OutNames)
{
	FAssetDocumentRuntimeValueDto Remaining;
	return TrySplitAssetDocumentEditorDeclaredPropertyArguments(Arguments, Remaining, OutNames);
}

bool TrySplitAssetDocumentEditorDeclaredPropertyArguments(
	const FAssetDocumentRuntimeValueDto& Arguments,
	FAssetDocumentRuntimeValueDto& OutRemaining,
	TArray<FString>& OutNames)
{
	OutNames.Reset();
	OutRemaining = {};
	OutRemaining.Kind = EAssetDocumentRuntimeValueKind::Object;
	if (Arguments.Kind != EAssetDocumentRuntimeValueKind::Object
		|| Arguments.ObjectKeys.Num() != Arguments.Elements.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < Arguments.ObjectKeys.Num(); ++Index)
	{
		if (Arguments.ObjectKeys[Index] != TEXT("admit_properties"))
		{
			OutRemaining.ObjectKeys.Add(Arguments.ObjectKeys[Index]);
			OutRemaining.Elements.Add(Arguments.Elements[Index]);
			continue;
		}
		const FAssetDocumentRuntimeValueDto& Names = Arguments.Elements[Index];
		if (Names.Kind != EAssetDocumentRuntimeValueKind::Array)
		{
			return false;
		}
		for (const FAssetDocumentRuntimeValueDto& Name : Names.Elements)
		{
			if (Name.Kind != EAssetDocumentRuntimeValueKind::String || Name.StringValue.IsEmpty()
				|| OutNames.Contains(Name.StringValue))
			{
				OutNames.Reset();
				return false;
			}
			OutNames.Add(Name.StringValue);
		}
	}
	return true;
}

FAssetDocumentEditorPropertyAdmissionResult
EnumerateAssetDocumentEditorAdmittedProperties(
	FStringView ExactAssetClassPath,
	IAssetDocumentHandleResolver& Handles,
	TConstArrayView<FString> DeclaredNames)
{
	UClass* Class = ResolveExactPropertyClass(ExactAssetClassPath);
	if (Class == nullptr)
	{
		return FailPropertyAdmission(
			TEXT("PropertyAdmission.NonCanonicalClass"),
			TEXT("The exact asset class path is unresolved or non-canonical."));
	}
	return BuildPropertyAdmission(*Class, nullptr, Handles, DeclaredNames);
}

FAssetDocumentEditorPropertyAdmissionResult
AdmitAssetDocumentEditorPropertyBatch(
	UObject& ExactTarget,
	IAssetDocumentHandleResolver& Handles,
	TConstArrayView<FString> DeclaredNames)
{
	UClass* ExactClass = GetExactLivePropertyTargetClass(ExactTarget);
	if (ExactClass == nullptr)
	{
		return FailPropertyAdmission(
			TEXT("PropertyAdmission.InvalidTarget"),
			TEXT("The exact property-admission target or class is not live."));
	}
	return BuildPropertyAdmission(*ExactClass, &ExactTarget, Handles, DeclaredNames);
}

FAssetDocumentEditorPropertyPreimageCaptureResult
CaptureAssetDocumentEditorWritablePropertyPreimage(
	UObject& ExactTarget)
{
	UClass* ExactClass = GetExactLivePropertyTargetClass(ExactTarget);
	if (ExactClass == nullptr)
	{
		return FailPropertyPreimageAdmission(
			TEXT("PropertyPreimage.InvalidTarget"),
			TEXT("The exact property-preimage target or class is not live."));
	}
	const TArray<FAdmittedPropertyCandidate> Candidates =
		GatherAdmittedProperties(*ExactClass);
	const TArray<FAssetDocumentEditorPropertyBatchTokenFieldView> Fields =
		ProjectPropertyBatchFields(
			Candidates, EPropertyFieldProjection::WritableOnly);
	return CaptureAssetDocumentEditorAdmittedPropertyPreimage(
		ExactTarget, Fields);
}

TArray<FAssetDocumentEditorPropertyBatchTokenFieldView>
GetAssetDocumentEditorAdmittedPropertyFields(UClass& ExactClass)
{
 return ProjectPropertyBatchFields(GatherAdmittedProperties(ExactClass), EPropertyFieldProjection::All);
}

bool TryGetAssetDocumentEditorAdmittedPropertyFields(
	UClass& ExactClass,
	TConstArrayView<FString> DeclaredNames,
	TArray<FAssetDocumentEditorPropertyBatchTokenFieldView>& OutFields,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	const TArray<FAdmittedPropertyCandidate> Candidates =
		GatherAdmittedProperties(ExactClass, DeclaredNames, &Diagnostics);
	if (!Diagnostics.IsEmpty())
	{
		OutDiagnostics.Append(MoveTemp(Diagnostics));
		return false;
	}
	OutFields = ProjectPropertyBatchFields(Candidates, EPropertyFieldProjection::All);
	return true;
}
