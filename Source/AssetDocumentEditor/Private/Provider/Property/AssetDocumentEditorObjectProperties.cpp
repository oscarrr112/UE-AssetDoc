// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentEditorObjectProperties.h"

#include "AssetDocumentEditorFloatPrecision.h"
#include "AssetDocumentEditorPickerValues.h"
#include "AssetDocumentEditorPropertyValueCodec.h"
#include "Components/ActorComponent.h"
#include "Engine/Level.h"
#include "GameFramework/Actor.h"
#include "Provider/AssetDocumentEditorTypedValueCodec.h"
#include "Provider/Property/AssetDocumentEditorPropertyAdmission.h"
#include "Provider/Property/AssetDocumentEditorPropertyBatchTokenPayload.h"
#include "Provider/Property/AssetDocumentEditorPropertyEdit.h"
#include "Provider/Property/AssetDocumentEditorPropertyTokenPayload.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/PropertyAccessUtil.h"
#include "UObject/UnrealType.h"

void AdoptAssetDocumentEditorDecodedPropertyValue(
	TUniquePtr<FAssetDocumentEditorScopedPropertyValue>&& NativeValue,
	FAssetDocumentEditorPreparedPropertyWrite& OutPrepared)
{
	OutPrepared = FAssetDocumentEditorPreparedPropertyWrite();
	OutPrepared.NativeValue = MoveTemp(NativeValue);
}

namespace UE::AssetDocument::EditorObjectProperties
{
struct FPreparedWrite::FImpl
{
	TWeakObjectPtr<UObject> Owner;
	FString Name;
	FAssetDocumentEditorPropertyBatchTokenFieldView Field;
	FAssetDocumentEditorPreparedPropertyWrite Write;
	// Set instead of Write when the shared write path would apply template semantics
	// to this instance (see NeedsInstanceSemanticsWrite).
	TUniquePtr<FAssetDocumentEditorScopedPropertyValue> InstanceWrite;
	// InstanceWrite with template semantics (FReferencePolicy::bTemplateSemantics on an
	// EditDefaultsOnly field).
	bool bTemplateWrite = false;
};

namespace
{
using FValue = FAssetDocumentRuntimeValueDto;
using EKind = EAssetDocumentRuntimeValueKind;
using FFieldView = FAssetDocumentEditorPropertyBatchTokenFieldView;

constexpr int32 MaximumValueDepth = 64;

bool ReadCurrentNative(UObject& Owner, FProperty& Property, FAssetDocumentEditorScopedPropertyValue& OutValue)
{
	return ReadAssetDocumentEditorObjectPropertyValue(Owner, Property, OutValue);
}

// PropertyAccessUtil::IsObjectTemplate treats an EditInlineNew object inside an asset
// as a template, and a placed component is one (UStaticMeshComponent,
// USceneCaptureComponent2D, ... are EditInlineNew; the world is an asset). The shared
// write path then refuses EditInstanceOnly properties that the Details panel edits on
// the instance. Such writes go through the same engine setter with instance semantics
// (instanced subobject values are adopted / released around it as the shared path does).
bool NeedsInstanceSemanticsWrite(const UObject& Owner, const FFieldView& View)
{
	const FProperty* Property = View.Property;
	return Property != nullptr && !View.bDeclared && !Owner.IsTemplate()
		&& PropertyAccessUtil::IsObjectTemplate(&Owner)
		&& !IsAssetDocumentEditorInPlaceInstancedProperty(*Property)
		&& PropertyAccessUtil::CanSetPropertyValue(Property, PropertyAccessUtil::EditorReadOnlyFlags, true)
			!= EPropertyAccessResultFlags::Success
		&& PropertyAccessUtil::CanSetPropertyValue(Property, PropertyAccessUtil::EditorReadOnlyFlags, false)
			== EPropertyAccessResultFlags::Success;
}

FAssetDocumentRuntimeDiagnosticContext MakeContext(const TCHAR* Key, const FString& Value)
{
	FAssetDocumentRuntimeDiagnosticContext Context;
	Context.Key = Key;
	Context.Value.Kind = EKind::String;
	Context.Value.StringValue = Value;
	return Context;
}

FAssetDocumentRuntimeDiagnostic MakeDiagnostic(
	const FString& PropertyName, const FString& ValuePath, const TCHAR* Code, const FString& Message)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = FString::Printf(TEXT("/%s%s"), *PropertyName, *ValuePath);
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	Diagnostic.Context.Add(MakeContext(TEXT("property"), PropertyName));
	return Diagnostic;
}

FString AppendPath(const FString& Path, const FString& Segment)
{
	FString Escaped = Segment.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
	return Path + TEXT("/") + Escaped;
}

FAdmittedField MakeField(const FFieldView& View)
{
	FAdmittedField Field;
	Field.Property = View.Property;
	Field.Name = View.Property->GetAuthoredName();
	Field.CodecId = View.CodecId;
	Field.bWritable = View.bWritable;
	Field.bEditableOnTemplates = !View.bWritable
		&& PropertyAccessUtil::CanSetPropertyValue(View.Property, PropertyAccessUtil::EditorReadOnlyFlags, true)
			== EPropertyAccessResultFlags::Success;
	return Field;
}

bool FindFieldView(UClass& Class, FStringView Name, FFieldView& OutView)
{
	for (const FFieldView& View : GetAssetDocumentEditorAdmittedPropertyFields(Class))
	{
		if (View.Property != nullptr && FStringView(View.Property->GetAuthoredName()).Equals(Name, ESearchCase::CaseSensitive))
		{
			OutView = View;
			return true;
		}
	}
	return false;
}

FAssetDocumentEditorPropertyTokenView MakeToken(const FFieldView& View)
{
	FAssetDocumentEditorPropertyTokenView Token;
	Token.DeclaringClass = View.DeclaringClass;
	Token.Property = View.Property;
	Token.CodecId = View.CodecId;
	Token.TypeRevision = View.TypeRevision;
	Token.bReadable = View.bReadable;
	Token.bWritable = View.bWritable;
	Token.bDeclared = View.bDeclared;
	return Token;
}

FFieldView MakeFieldView(const FAdmittedField& Field)
{
	FFieldView View;
	View.Property = Field.Property;
	View.DeclaringClass = Field.Property != nullptr ? Field.Property->GetOwnerClass() : nullptr;
	View.CodecId = Field.CodecId;
	View.TypeRevision = View.DeclaringClass != nullptr ? GetAssetDocumentEditorPropertyTypeRevision(*View.DeclaringClass) : 0;
	View.bReadable = true;
	View.bWritable = Field.bWritable;
	return View;
}

bool IsViewIdenticalToArchetype(UObject& Object, const FFieldView& View)
{
	UObject* Archetype = Object.GetArchetype();
	if (Archetype == nullptr || View.Property == nullptr || !Archetype->IsA(View.Property->GetOwnerClass()))
	{
		return false;
	}
	FAssetDocumentEditorScopedPropertyValue Current(*View.Property);
	FAssetDocumentEditorScopedPropertyValue Default(*View.Property);
	return ReadCurrentNative(Object, *View.Property, Current)
		&& ReadCurrentNative(*Archetype, *View.Property, Default)
		&& AreAssetDocumentEditorTypedValuesEquivalent(*View.Property, Current.GetMemory(), Default.GetMemory());
}

// {"value": V} of the property read primitives -> V.
bool TakeReadValue(FAssetDocumentEditorPropertyEditResult& Read, FValue& OutValue)
{
	if (!Read.bSucceeded || Read.Value.Kind != EKind::Object)
	{
		return false;
	}
	const int32 Index = Read.Value.ObjectKeys.IndexOfByKey(TEXT("value"));
	if (Index == INDEX_NONE || !Read.Value.Elements.IsValidIndex(Index))
	{
		return false;
	}
	OutValue = MoveTemp(Read.Value.Elements[Index]);
	return true;
}

bool IsActorOrComponentClass(const UClass* Class)
{
	return Class != nullptr && (Class->IsChildOf<AActor>() || Class->IsChildOf<UActorComponent>());
}

FProperty* FindStructMember(const UStruct& Struct, const FString& Name)
{
	for (TFieldIterator<FProperty> It(&Struct); It; ++It)
	{
		if (It->GetAuthoredName() == Name)
		{
			return *It;
		}
	}
	return nullptr;
}

// Replaces "@id" strings in actor/component reference positions with the path of the
// object the resolver returns. Type-directed: only where the property type is such a
// reference, so string values "@x" stay strings.
bool SubstituteBatchIds(
	const FProperty& Property, FValue& Value, bool bIgnoreStaticArray, int32 Depth, const FString& Path,
	const FReferencePolicy& Policy, const FString& PropertyName, TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	if (Depth > MaximumValueDepth)
	{
		return true;
	}
	if (!bIgnoreStaticArray && Property.ArrayDim > 1)
	{
		bool bOk = true;
		for (int32 Index = 0; Value.Kind == EKind::Array && Index < Value.Elements.Num(); ++Index)
		{
			bOk &= SubstituteBatchIds(Property, Value.Elements[Index], true, Depth + 1, AppendPath(Path, FString::FromInt(Index)),
				Policy, PropertyName, OutDiagnostics);
		}
		return bOk;
	}
	if (const FObjectPropertyBase* Reference = CastField<FObjectPropertyBase>(&Property))
	{
		if (Value.Kind != EKind::String || !Value.StringValue.StartsWith(TEXT("@"))
			|| CastField<FClassProperty>(&Property) != nullptr || CastField<FSoftClassProperty>(&Property) != nullptr
			|| Property.HasAnyPropertyFlags(CPF_InstancedReference | CPF_PersistentInstance)
			|| !IsActorOrComponentClass(Reference->PropertyClass))
		{
			return true;
		}
		const FString Id = Value.StringValue.RightChop(1);
		UObject* Resolved = Policy.ResolveBatchId(Id);
		if (Resolved == nullptr)
		{
			FAssetDocumentRuntimeDiagnostic Diagnostic = MakeDiagnostic(PropertyName, Path, TEXT("Level.UnknownId"),
				FString::Printf(TEXT("'@%s' names no object created earlier in this request (batch ids are defined by an "
					"earlier op's \"id\")."), *Id));
			Diagnostic.Context.Add(MakeContext(TEXT("id"), Id));
			OutDiagnostics.Add(MoveTemp(Diagnostic));
			return false;
		}
		Value.StringValue = Resolved->GetPathName();
		return true;
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		if (Value.Kind != EKind::Object || Struct->Struct == FInstancedStruct::StaticStruct())
		{
			return true;
		}
		bool bOk = true;
		for (int32 Index = 0; Index < Value.ObjectKeys.Num() && Index < Value.Elements.Num(); ++Index)
		{
			if (const FProperty* Member = FindStructMember(*Struct->Struct, Value.ObjectKeys[Index]))
			{
				bOk &= SubstituteBatchIds(*Member, Value.Elements[Index], false, Depth + 1,
					AppendPath(Path, Value.ObjectKeys[Index]), Policy, PropertyName, OutDiagnostics);
			}
		}
		return bOk;
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
	if (Inner != nullptr)
	{
		bool bOk = true;
		for (int32 Index = 0; Value.Kind == EKind::Array && Index < Value.Elements.Num(); ++Index)
		{
			bOk &= SubstituteBatchIds(*Inner, Value.Elements[Index], false, Depth + 1, AppendPath(Path, FString::FromInt(Index)),
				Policy, PropertyName, OutDiagnostics);
		}
		return bOk;
	}
	if (const FMapProperty* Map = CastField<FMapProperty>(&Property))
	{
		bool bOk = true;
		for (int32 Index = 0; Value.Kind == EKind::Object && Index < Value.Elements.Num(); ++Index)
		{
			bOk &= SubstituteBatchIds(*Map->ValueProp, Value.Elements[Index], false, Depth + 1,
				AppendPath(Path, Value.ObjectKeys.IsValidIndex(Index) ? Value.ObjectKeys[Index] : FString::FromInt(Index)),
				Policy, PropertyName, OutDiagnostics);
		}
		return bOk;
	}
	return true;
}

// Every hard reference (object, weak, lazy, interface) in a native value, with its
// JSON pointer. Soft references, class references and instanced (owned) subobjects
// are not references to other objects of the level.
void VisitHardReferences(
	const FProperty& Property, const void* Value, bool bIgnoreStaticArray, int32 Depth, const FString& Path,
	TFunctionRef<void(UObject&, const FString&)> Visit)
{
	if (Value == nullptr || Depth > MaximumValueDepth)
	{
		return;
	}
	if (!bIgnoreStaticArray && Property.ArrayDim > 1)
	{
		for (int32 Index = 0; Index < Property.ArrayDim; ++Index)
		{
			VisitHardReferences(Property, static_cast<const uint8*>(Value) + Index * Property.GetElementSize(), true,
				Depth + 1, AppendPath(Path, FString::FromInt(Index)), Visit);
		}
		return;
	}
	if (CastField<FInterfaceProperty>(&Property) != nullptr)
	{
		if (UObject* Object = static_cast<const FScriptInterface*>(Value)->GetObject())
		{
			Visit(*Object, Path);
		}
		return;
	}
	if (const FObjectPropertyBase* Reference = CastField<FObjectPropertyBase>(&Property))
	{
		if (CastField<FSoftObjectProperty>(&Property) != nullptr || CastField<FClassProperty>(&Property) != nullptr
			|| Property.HasAnyPropertyFlags(CPF_InstancedReference | CPF_PersistentInstance))
		{
			return;
		}
		if (UObject* Object = Reference->GetObjectPropertyValue(Value))
		{
			Visit(*Object, Path);
		}
		return;
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		if (Struct->Struct == FInstancedStruct::StaticStruct())
		{
			return;
		}
		for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
		{
			VisitHardReferences(**It, It->ContainerPtrToValuePtr<void>(Value), false, Depth + 1,
				AppendPath(Path, It->GetAuthoredName()), Visit);
		}
		return;
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		FScriptArrayHelper Helper(Array, Value);
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			VisitHardReferences(*Array->Inner, Helper.GetRawPtr(Index), false, Depth + 1,
				AppendPath(Path, FString::FromInt(Index)), Visit);
		}
		return;
	}
	if (const FSetProperty* Set = CastField<FSetProperty>(&Property))
	{
		FScriptSetHelper Helper(Set, Value);
		int32 Index = 0;
		for (FScriptSetHelper::FIterator It(Helper); It; ++It, ++Index)
		{
			VisitHardReferences(*Set->ElementProp, Helper.GetElementPtr(It), false, Depth + 1,
				AppendPath(Path, FString::FromInt(Index)), Visit);
		}
		return;
	}
	if (const FMapProperty* Map = CastField<FMapProperty>(&Property))
	{
		FScriptMapHelper Helper(Map, Value);
		for (FScriptMapHelper::FIterator It(Helper); It; ++It)
		{
			FString Key;
			Map->KeyProp->ExportTextItem_Direct(Key, Helper.GetKeyPtr(It), nullptr, nullptr, PPF_None);
			const FString EntryPath = AppendPath(Path, Key);
			VisitHardReferences(*Map->KeyProp, Helper.GetKeyPtr(It), false, Depth + 1, EntryPath, Visit);
			VisitHardReferences(*Map->ValueProp, Helper.GetValuePtr(It), false, Depth + 1, EntryPath, Visit);
		}
	}
}

// The Details actor picker's rules for hard references in a level object's value
// (FReferencePolicy::SameLevel). References the current value already holds are kept.
// BatchObjects: the objects "@id" strings of the value resolved to (with their ids);
// in preflight they stand in for actors that do not exist yet, whose level
// Policy.ResolveBatchLevel tells.
bool CheckLevelReferences(
	const FProperty& Property, const void* Value, const void* CurrentValue, const UObject& Owner, const FReferencePolicy& Policy,
	const TMap<const UObject*, TArray<FString>>& BatchObjects, const FString& PropertyName,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	const ULevel& SameLevel = *Policy.SameLevel;
	TSet<const UObject*> Kept;
	if (CurrentValue != nullptr)
	{
		VisitHardReferences(Property, CurrentValue, false, 0, FString(),
			[&Kept](UObject& Object, const FString&) { Kept.Add(&Object); });
	}
	const UObject* Outermost = Owner.GetOutermostObject();
	const bool bDisallowLevelActors = Property.HasMetaData(TEXT("DisallowLevelActorReference"));
	bool bOk = true;
	VisitHardReferences(Property, Value, false, 0, FString(), [&](UObject& Object, const FString& Path)
	{
		if (Kept.Contains(&Object))
		{
			return;
		}
		const TArray<FString>* Ids = BatchObjects.Find(&Object);
		const ULevel* Level = Object.GetTypedOuter<ULevel>();
		bool bLevelActor = Object.IsA<AActor>() || Object.IsA<UActorComponent>();
		if (Ids != nullptr && Policy.ResolveBatchLevel)
		{
			// A stand-in for an actor of the batch: the level it will be in.
			for (const FString& Id : *Ids)
			{
				if (const ULevel* Planned = Policy.ResolveBatchLevel(Id))
				{
					Level = Planned;
					bLevelActor = true;
				}
			}
		}
		else if (Object.IsTemplate())
		{
			// A class default object or archetype is no placed actor; the level tools'
			// preflight stands one in for an actor the batch has not created yet ("@id").
			if (Ids == nullptr && bLevelActor)
			{
				OutDiagnostics.Add(MakeDiagnostic(PropertyName, Path, TEXT("Level.NotALevelActor"), FString::Printf(
					TEXT("Property '%s' would reference '%s', a class default object or template, not an actor placed in the "
						"level. Name a placed actor by its path (query_level), or an actor of this batch by \"@id\"."),
					*PropertyName, *Object.GetPathName())));
				bOk = false;
			}
			return;
		}
		if (Level != nullptr && bLevelActor)
		{
			if (bDisallowLevelActors)
			{
				OutDiagnostics.Add(MakeDiagnostic(PropertyName, Path, TEXT("Level.ActorReferenceDisallowed"), FString::Printf(
					TEXT("Property '%s' does not accept level actors (DisallowLevelActorReference): the Details picker lists "
						"assets only, so '%s' cannot be referenced here."), *PropertyName, *Object.GetPathName())));
				bOk = false;
			}
			else if (Level != &SameLevel)
			{
				FAssetDocumentRuntimeDiagnostic Diagnostic = MakeDiagnostic(PropertyName, Path, TEXT("Level.CrossLevelHardReference"),
					FString::Printf(TEXT("Property '%s' is a hard reference and '%s' is in level %s, not in %s: the owning "
						"package could not be saved with it. Use an actor of the same level, or a soft reference property."),
						*PropertyName, *Object.GetPathName(), *Level->GetOutermost()->GetName(), *SameLevel.GetOutermost()->GetName()));
				Diagnostic.Context.Add(MakeContext(TEXT("referenced_level"), Level->GetOutermost()->GetName()));
				OutDiagnostics.Add(MoveTemp(Diagnostic));
				bOk = false;
			}
			return;
		}
		const bool bInsideOwner = Outermost != nullptr && &Object != Outermost && Object.IsIn(Outermost);
		if (Level != nullptr || bInsideOwner)
		{
			OutDiagnostics.Add(MakeDiagnostic(PropertyName, Path, TEXT("Property.EditorDerivedReference"), FString::Printf(
				TEXT("Property '%s' would reference '%s', an object the level's own code creates (not an actor or a "
					"component). Reference an actor, a component of an actor, or an asset."),
				*PropertyName, *Object.GetPathName())));
			bOk = false;
		}
	});
	return bOk;
}

} // namespace

FPreparedWrite::FPreparedWrite() = default;
FPreparedWrite::~FPreparedWrite() = default;
FPreparedWrite::FPreparedWrite(FPreparedWrite&& Other) = default;
FPreparedWrite& FPreparedWrite::operator=(FPreparedWrite&& Other) = default;

bool FPreparedWrite::IsValid() const
{
	return Impl.IsValid() && Impl->Owner.IsValid();
}

UObject* FPreparedWrite::GetOwner() const
{
	return Impl.IsValid() ? Impl->Owner.Get() : nullptr;
}

const FString& FPreparedWrite::GetPropertyName() const
{
	static const FString Empty;
	return Impl.IsValid() ? Impl->Name : Empty;
}

TArray<FAdmittedField> ListFields(UClass& Class)
{
	TArray<FAdmittedField> Fields;
	for (const FFieldView& View : GetAssetDocumentEditorAdmittedPropertyFields(Class))
	{
		if (View.Property != nullptr)
		{
			Fields.Add(MakeField(View));
		}
	}
	return Fields;
}

TArray<FAdmittedField> ListWritable(UObject& Object)
{
	TArray<FAdmittedField> Fields = ListFields(*Object.GetClass());
	Fields.RemoveAll([](const FAdmittedField& Field) { return !Field.bWritable; });
	return Fields;
}

bool Read(UObject& Object, FStringView Property, FAssetDocumentRuntimeValueDto& OutValue,
	FAssetDocumentRuntimeDiagnostic* OutDiagnostic)
{
	FFieldView View;
	if (!FindFieldView(*Object.GetClass(), Property, View))
	{
		if (OutDiagnostic != nullptr)
		{
			*OutDiagnostic = MakeDiagnostic(FString(Property), FString(), TEXT("Generic.UnknownProperty"), FString::Printf(
				TEXT("'%.*s' is not an editable property of %s."), Property.Len(), Property.GetData(),
				*Object.GetClass()->GetPathName()));
		}
		return false;
	}
	FAssetDocumentEditorPropertyEditResult ReadResult = ReadAssetDocumentEditorAdmittedProperty(Object, MakeToken(View));
	if (!TakeReadValue(ReadResult, OutValue))
	{
		if (OutDiagnostic != nullptr)
		{
			*OutDiagnostic = ReadResult.Diagnostics.IsEmpty()
				? MakeDiagnostic(FString(Property), FString(), TEXT("PropertyRead.EncodeFailed"),
					TEXT("The property value could not be encoded."))
				: ReadResult.Diagnostics[0];
		}
		return false;
	}
	return true;
}

bool EncodeAsAuthored(UObject& Object, FStringView Property, const FAssetDocumentRuntimeValueDto& Authored,
	FAssetDocumentRuntimeValueDto& OutValue)
{
	FFieldView View;
	if (!FindFieldView(*Object.GetClass(), Property, View))
	{
		return false;
	}
	FAssetDocumentEditorPropertyEditResult ReadResult =
		ReadAssetDocumentEditorAdmittedProperty(Object, MakeToken(View), &Authored);
	return TakeReadValue(ReadResult, OutValue);
}

bool PrepareExpected(UObject& Owner, FStringView Property, const FAssetDocumentRuntimeValueDto& Authored,
	const FReferencePolicy& Policy, FAssetDocumentRuntimeValueDto& OutExpected,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	FFieldView View;
	if (!FindFieldView(*Owner.GetClass(), Property, View) || View.Property == nullptr)
	{
		return false;
	}
	OutExpected = Authored;
	if (Policy.ResolveBatchId
		&& !SubstituteBatchIds(*View.Property, OutExpected, false, 0, FString(), Policy, FString(Property), OutDiagnostics))
	{
		return false;
	}
	UE::AssetDocument::EditorFloatPrecision::NormalizeValue(*View.Property, OutExpected);
	return true;
}

bool IsIdenticalToArchetype(UObject& Object, FStringView Property)
{
	FFieldView View;
	return FindFieldView(*Object.GetClass(), Property, View) && IsViewIdenticalToArchetype(Object, View);
}

bool IsIdenticalToArchetype(UObject& Object, const FAdmittedField& Field)
{
	return IsViewIdenticalToArchetype(Object, MakeFieldView(Field));
}

bool Read(UObject& Object, const FAdmittedField& Field, FAssetDocumentRuntimeValueDto& OutValue)
{
	if (Field.Property == nullptr || !Object.IsA(Field.Property->GetOwnerClass()))
	{
		return false;
	}
	FAssetDocumentEditorPropertyEditResult ReadResult =
		ReadAssetDocumentEditorAdmittedProperty(Object, MakeToken(MakeFieldView(Field)));
	return TakeReadValue(ReadResult, OutValue);
}

bool ReadChanged(UObject& Object, const FAdmittedField& Field, FAssetDocumentRuntimeValueDto& OutValue)
{
	if (IsIdenticalToArchetype(Object, Field) || !Read(Object, Field, OutValue))
	{
		return false;
	}
	UObject* Archetype = Object.GetArchetype();
	FAssetDocumentRuntimeValueDto Base;
	// Equal in document form (members the codec leaves out differ): nothing to write either.
	return Archetype == nullptr || !Read(*Archetype, Field, Base)
		|| UE::AssetDocument::EditorPropertyValueCodec::PruneToChanged(*Field.Property, OutValue, Base);
}

bool Decode(UObject& Owner, FStringView Property, const FAssetDocumentRuntimeValueDto& Value,
	const FReferencePolicy& Policy, FPreparedWrite& OutPrepared, TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	OutPrepared = FPreparedWrite();
	const FString Name(Property);
	FFieldView View;
	if (!FindFieldView(*Owner.GetClass(), Property, View))
	{
		FAssetDocumentRuntimeDiagnostic Diagnostic = MakeDiagnostic(Name, FString(), TEXT("Generic.UnknownProperty"), FString::Printf(
			TEXT("'%s' is not an editable property of %s (names are case-sensitive; inspect_asset_document_target lists "
				"a class's properties)."), *Name, *Owner.GetClass()->GetName()));
		Diagnostic.Context.Add(MakeContext(TEXT("class"), Owner.GetClass()->GetPathName()));
		OutDiagnostics.Add(MoveTemp(Diagnostic));
		return false;
	}
	const FAdmittedField Field = MakeField(View);
	const bool bTemplateWrite = !Field.bWritable && Policy.bTemplateSemantics && Field.bEditableOnTemplates;
	if (!Field.bWritable && !bTemplateWrite)
	{
		const bool bInstanceRule = Policy.SameLevel != nullptr && Field.bEditableOnTemplates;
		FAssetDocumentRuntimeDiagnostic Diagnostic = bInstanceRule
			? MakeDiagnostic(Name, FString(), TEXT("Level.PropertyNotEditableOnInstance"), FString::Printf(
				TEXT("'%s' of %s is editable on the class defaults only (EditDefaultsOnly), not on a placed instance: edit "
					"the Blueprint's defaults (its AssetDocument) instead."), *Name, *Owner.GetClass()->GetName()))
			: MakeDiagnostic(Name, FString(), TEXT("Generic.NonWritableProperty"), FString::Printf(
				TEXT("'%s' of %s is read-only in the Details panel (VisibleAnywhere / EditConst or not editable on this "
					"object)."), *Name, *Owner.GetClass()->GetName()));
		Diagnostic.Context.Add(MakeContext(TEXT("class"), Owner.GetClass()->GetPathName()));
		OutDiagnostics.Add(MoveTemp(Diagnostic));
		return false;
	}

	// The objects "@id" strings resolve to, for the level reference check.
	TMap<const UObject*, TArray<FString>> BatchObjects;
	FReferencePolicy Recording;
	if (Policy.ResolveBatchId)
	{
		Recording.ResolveBatchId = [&Policy, &BatchObjects](FStringView Id) -> UObject*
		{
			UObject* Resolved = Policy.ResolveBatchId(Id);
			if (Resolved != nullptr)
			{
				BatchObjects.FindOrAdd(Resolved).AddUnique(FString(Id));
			}
			return Resolved;
		};
	}
	FValue Substituted = Value;
	if (Policy.ResolveBatchId
		&& !SubstituteBatchIds(*View.Property, Substituted, false, 0, FString(), Recording, Name, OutDiagnostics))
	{
		return false;
	}

	FAssetDocumentEditorScopedPropertyValue Current(*View.Property);
	FAssetDocumentEditorDecodeBase Base;
	if (ReadCurrentNative(Owner, *View.Property, Current))
	{
		Base.Value = Current.GetMemory();
		Base.Owner = &Owner;
	}
	TUniquePtr<FAssetDocumentEditorScopedPropertyValue> NativeValue;
	const FAssetDocumentEditorTypedValueCodecResult Decoded =
		DecodeAssetDocumentEditorTypedValue(*View.Property, Substituted, NativeValue, Base);
	if (!Decoded.bSucceeded || Decoded.CodecId != View.CodecId || !NativeValue.IsValid())
	{
		const FString Code = Decoded.DiagnosticCode.StartsWith(TEXT("TypedValueCodec."))
			? Decoded.DiagnosticCode : FString(TEXT("PropertyWrite.DecodeFailed"));
		FAssetDocumentRuntimeDiagnostic Diagnostic = MakeDiagnostic(Name, Decoded.ValuePath, *Code, FString::Printf(
			TEXT("The value for '%s' (%s, codec %s) is not valid%s%s"), *Name, *View.Property->GetCPPType(), *View.CodecId,
			Decoded.Message.IsEmpty() ? TEXT(".") : TEXT(": "), *Decoded.Message));
		if (!Decoded.DiagnosticCode.IsEmpty())
		{
			Diagnostic.Context.Add(MakeContext(TEXT("codec_code"), Decoded.DiagnosticCode));
		}
		OutDiagnostics.Add(MoveTemp(Diagnostic));
		return false;
	}

	// A row handle naming a row its table lacks, or a key that does not exist (the Details pickers list only existing ones).
	{
		UE::AssetDocument::EditorPickerValues::FInvalidPick Invalid;
		if (UE::AssetDocument::EditorPickerValues::FindInvalidPick(*View.Property, NativeValue->GetMemory(), Base.Value, Invalid))
		{
			OutDiagnostics.Add(MakeDiagnostic(Name, Invalid.ValuePath, *Invalid.Code,
				FString::Printf(TEXT("'%s'%s: %s"), *Name, *Invalid.ValuePath, *Invalid.Message)));
			return false;
		}
	}

	if (Policy.SameLevel != nullptr)
	{
		if (!CheckLevelReferences(*View.Property, NativeValue->GetMemory(), Base.Value, Owner, Policy, BatchObjects, Name,
			OutDiagnostics))
		{
			return false;
		}
	}
	else
	{
		FString ValuePath;
		if (const UObject* Derived = FindAssetDocumentEditorDerivedReference(
			*View.Property, NativeValue->GetMemory(), Owner, ValuePath, false, Base.Value))
		{
			OutDiagnostics.Add(MakeDiagnostic(Name, ValuePath, TEXT("Property.EditorDerivedReference"), FString::Printf(
				TEXT("Property '%s' would reference '%s', an object inside '%s'; a plain reference names an asset or an "
					"object outside it, as the Details panel picker does."), *Name, *Derived->GetPathName(),
				*Owner.GetOutermostObject()->GetPathName())));
			return false;
		}
	}

	OutPrepared.Impl = MakeUnique<FPreparedWrite::FImpl>();
	OutPrepared.Impl->Owner = &Owner;
	OutPrepared.Impl->Name = Name;
	OutPrepared.Impl->Field = View;
	if (bTemplateWrite || NeedsInstanceSemanticsWrite(Owner, View))
	{
		OutPrepared.Impl->InstanceWrite = MoveTemp(NativeValue);
		OutPrepared.Impl->bTemplateWrite = bTemplateWrite;
	}
	else
	{
		AdoptAssetDocumentEditorDecodedPropertyValue(MoveTemp(NativeValue), OutPrepared.Impl->Write);
	}
	return true;
}

bool Commit(FPreparedWrite& Prepared, TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics,
	FAssetDocumentRuntimeValueDto* OutReadback)
{
	TUniquePtr<FPreparedWrite::FImpl> Impl = MoveTemp(Prepared.Impl);
	UObject* Owner = Impl.IsValid() ? Impl->Owner.Get() : nullptr;
	if (Owner == nullptr)
	{
		FAssetDocumentRuntimeDiagnostic Diagnostic;
		Diagnostic.Code = TEXT("PropertyWrite.InvalidTarget");
		Diagnostic.Message = TEXT("The prepared write has no live owner (already committed, or the object was destroyed).");
		if (Impl.IsValid())
		{
			Diagnostic.Path = FString::Printf(TEXT("/%s"), *Impl->Name);
		}
		OutDiagnostics.Add(MoveTemp(Diagnostic));
		return false;
	}
	if (Impl->InstanceWrite.IsValid())
	{
		FProperty* const Property = Impl->Field.Property;
		UObject* const Target = Owner;
		const bool bOwnerIsTemplate = Impl->bTemplateWrite;
		const bool bInstanced = DoesAssetDocumentEditorPropertyHoldInstancedObjects(*Property);
		void* const CurrentValue = Property->ContainerPtrToValuePtr<void>(Target);
		TUniquePtr<FAssetDocumentEditorScopedPropertyValue> Before;
		bool bWritten = Target->CanModify();
		if (bWritten && bInstanced)
		{
			// As the shared write path: reuse / adopt the decoded subobjects under the
			// owner (the Details class picker's NewObject(Owner, Class)) before the set.
			Before = MakeUnique<FAssetDocumentEditorScopedPropertyValue>(*Property);
			Property->CopyCompleteValue(Before->GetMemory(), CurrentValue);
			AdoptAssetDocumentEditorInstancedValue(*Target, *Impl->InstanceWrite, CurrentValue, /*bNotify*/ true);
		}
		bWritten = bWritten
			&& PropertyAccessUtil::SetPropertyValue_InContainer(Property, Target, TArray<void*>(), Property,
				Impl->InstanceWrite->GetMemory(), INDEX_NONE, PropertyAccessUtil::EditorReadOnlyFlags, bOwnerIsTemplate,
				[Property, Target]()
				{
					return PropertyAccessUtil::BuildBasicChangeNotify(Property, Target, EPropertyAccessChangeNotifyMode::Default,
						EPropertyChangeType::ValueSet);
				}) == EPropertyAccessResultFlags::Success;
		if (bWritten && Before.IsValid())
		{
			ReleaseAssetDocumentEditorReplacedInstancedObjects(*Target, *Property, Before->GetMemory(), CurrentValue);
		}
		if (!bWritten)
		{
			OutDiagnostics.Add(MakeDiagnostic(Impl->Name, FString(), TEXT("PropertyWrite.AccessFailed"),
				TEXT("The admitted property could not be written.")));
			return false;
		}
		if (OutReadback != nullptr)
		{
			FAssetDocumentRuntimeDiagnostic ReadDiagnostic;
			if (!Read(*Target, Impl->Name, *OutReadback, &ReadDiagnostic))
			{
				OutDiagnostics.Add(MoveTemp(ReadDiagnostic));
				return false;
			}
		}
		return true;
	}
	FAssetDocumentEditorPropertyEditResult Result = ApplyAssetDocumentEditorPreparedPropertyWriteWithoutEnlist(
		*Owner, MakeToken(Impl->Field), MoveTemp(Impl->Write), TEXT("EditorObjectProperties"));
	if (!Result.bSucceeded)
	{
		for (FAssetDocumentRuntimeDiagnostic& Diagnostic : Result.Diagnostics)
		{
			if (Diagnostic.Path.IsEmpty() || !Diagnostic.Path.StartsWith(TEXT("/") + Impl->Name))
			{
				Diagnostic.Path = FString::Printf(TEXT("/%s"), *Impl->Name);
			}
			OutDiagnostics.Add(MoveTemp(Diagnostic));
		}
		return false;
	}
	if (OutReadback != nullptr)
	{
		return TakeReadValue(Result, *OutReadback);
	}
	return true;
}
}
