// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"
#include "Templates/Function.h"
#include "Templates/UniquePtr.h"

class FProperty;
class UClass;
class ULevel;
class UObject;

// Reflected property read/write on one live object (an actor, a component, any
// UObject) with the generic AssetDocument admission and typed value codec: the
// values an AI writes into a document's Properties work here unchanged. Used by the
// level tools (docs/architecture/level-editing-tools-design.md §4).
//
// Admission is the Details panel's: CPF_Edit, not transient / deprecated /
// skip-serialized, a codec for the type, and writability with instance semantics
// (PropertyAccessUtil::CanSetPropertyValue with bOwnerIsTemplate = false, so
// EditDefaultsOnly / DisableEditOnInstance properties are read-only on instances).
// Writes are Details-equivalent (PreEditChange / Modify / PostEditChangeProperty),
// so they record into an open editor transaction.
namespace UE::AssetDocument::EditorObjectProperties
{
struct FAdmittedField
{
	FProperty* Property = nullptr;
	// The authored name (a Blueprint variable's name as the Details panel shows it).
	FString Name;
	// The document form (bool.v1, number.v1, struct.v1, object_ref.v1, ...).
	FString CodecId;
	// Writable on an instance of the class.
	bool bWritable = false;
	// Read-only on instances but editable on class defaults / archetypes
	// (EditDefaultsOnly): edit the Blueprint (its document) instead.
	bool bEditableOnTemplates = false;
};

// How object references in a value are checked.
struct FReferencePolicy
{
	// Null: asset semantics (AssetDocument's editor-derived-reference rule; a value may
	// not name an object inside the owner's outermost object, e.g. another actor of
	// the same world).
	// Non-null: level semantics, the Details actor picker's rules. A hard reference
	// (object, weak, lazy, interface) may name an actor, or a component of an actor,
	// in this level; one in another level is Level.CrossLevelHardReference (the
	// package could not save it); DisallowLevelActorReference metadata refuses level
	// actors (Level.ActorReferenceDisallowed); a class default object or template of an
	// actor or component is no placed actor (Level.NotALevelActor); any other object
	// inside a level stays Property.EditorDerivedReference. Soft references may name
	// any actor. Policy
	// failures use Level.* codes, and an EditDefaultsOnly property is reported as
	// Level.PropertyNotEditableOnInstance instead of Generic.NonWritableProperty.
	const ULevel* SameLevel = nullptr;
	// "@id" strings in reference positions whose class is an Actor or ActorComponent
	// (hard or soft) name an object created earlier in the same request; the resolver
	// returns it, or null (Level.UnknownId). Unset: "@..." is an ordinary path. A string
	// property value "@x" always stays a string.
	TFunction<UObject*(FStringView Id)> ResolveBatchId;
	// Preflight of a batch: the level an "@id" actor will be in, when the object
	// ResolveBatchId returns only stands in for it (a class default object, or the
	// source of a duplicate into another level). Hard references to it are checked
	// against this level instead of the stand-in's. Unset (or null): the object's own.
	TFunction<const ULevel*(FStringView Id)> ResolveBatchLevel;
	// Template semantics: the Details panel of an object it shows with its
	// DisableEditOnInstance properties editable (an asset's instanced node objects, e.g.
	// the generators and tests of an Environment Query, whose properties are all
	// EditDefaultsOnly). EditDefaultsOnly fields are admitted and written with
	// bOwnerIsTemplate = true; VisibleAnywhere / EditConst stay read-only.
	bool bTemplateSemantics = false;
};

class FPreparedWrite;

// Admits and decodes Value as an edit of the field's current value on Owner (omitted
// struct members keep their value, as in documents) and checks its references with
// Policy. Nothing is written. Diagnostics carry Path "/<Property><path inside the
// value>" (callers prefix their own pointer) and context "property".
// Codes: Generic.UnknownProperty, Generic.NonWritableProperty /
// Level.PropertyNotEditableOnInstance, the codec's TypedValueCodec.* (or
// PropertyWrite.DecodeFailed), Property.EditorDerivedReference, and the Level.*
// policy codes above.
ASSETDOCUMENTEDITOR_API bool Decode(UObject& Owner, FStringView Property, const FAssetDocumentRuntimeValueDto& Value,
	const FReferencePolicy& Policy, FPreparedWrite& OutPrepared, TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);

// Writes a prepared value, Details-equivalently, and reads it back (OutReadback: the
// stored value in document form). Consumes Prepared. PropertyWrite.* codes on failure.
ASSETDOCUMENTEDITOR_API bool Commit(FPreparedWrite& Prepared, TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics,
	FAssetDocumentRuntimeValueDto* OutReadback = nullptr);

// A decoded value ready to be written to its property. Move-only.
class ASSETDOCUMENTEDITOR_API FPreparedWrite
{
public:
	FPreparedWrite();
	~FPreparedWrite();
	FPreparedWrite(FPreparedWrite&& Other);
	FPreparedWrite& operator=(FPreparedWrite&& Other);
	FPreparedWrite(const FPreparedWrite&) = delete;
	FPreparedWrite& operator=(const FPreparedWrite&) = delete;

	bool IsValid() const;
	UObject* GetOwner() const;
	const FString& GetPropertyName() const;

	struct FImpl;

private:
	TUniquePtr<FImpl> Impl;
	friend ASSETDOCUMENTEDITOR_API bool Decode(UObject&, FStringView, const FAssetDocumentRuntimeValueDto&,
		const FReferencePolicy&, FPreparedWrite&, TArray<FAssetDocumentRuntimeDiagnostic>&);
	friend ASSETDOCUMENTEDITOR_API bool Commit(FPreparedWrite&, TArray<FAssetDocumentRuntimeDiagnostic>&,
		FAssetDocumentRuntimeValueDto*);
};

// Every admitted (readable) field of Class, writable or not, sorted by name.
ASSETDOCUMENTEDITOR_API TArray<FAdmittedField> ListFields(UClass& Class);

// The fields of Object's class that are writable on an instance.
ASSETDOCUMENTEDITOR_API TArray<FAdmittedField> ListWritable(UObject& Object);

// The current value of an admitted field in its document form. False (with a
// diagnostic when OutDiagnostic is given) for an unknown field or a codec failure.
ASSETDOCUMENTEDITOR_API bool Read(UObject& Object, FStringView Property, FAssetDocumentRuntimeValueDto& OutValue,
	FAssetDocumentRuntimeDiagnostic* OutDiagnostic = nullptr);

// As above for a field from ListFields (no name lookup; for reading many objects).
ASSETDOCUMENTEDITOR_API bool Read(UObject& Object, const FAdmittedField& Field, FAssetDocumentRuntimeValueDto& OutValue);

// As Read, shaped like Authored (struct members and instanced Properties Authored
// omits are left out), so the result equals Authored exactly when the property
// stores what was authored.
ASSETDOCUMENTEDITOR_API bool EncodeAsAuthored(UObject& Object, FStringView Property,
	const FAssetDocumentRuntimeValueDto& Authored, FAssetDocumentRuntimeValueDto& OutValue);

// The value an authored write must leave in the property, in document form, for a
// read-back comparison with EncodeAsAuthored: "@id" batch ids in actor / component
// reference positions replaced by the path of the object Policy.ResolveBatchId
// returns (Level.UnknownId when it returns null), and float32 numbers rewritten to the
// shortest decimal of the float they are stored as (0.1 stays 0.1; 0.123456789 becomes
// 0.12345679), as AssetDocument documents are normalised. False for an unknown field.
ASSETDOCUMENTEDITOR_API bool PrepareExpected(UObject& Owner, FStringView Property, const FAssetDocumentRuntimeValueDto& Authored,
	const FReferencePolicy& Policy, FAssetDocumentRuntimeValueDto& OutExpected,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);

// As Read, sparse over Object's archetype (diff_only): the value with the parts equal to the archetype's left
// out (EditorPropertyValueCodec::PruneToChanged: struct members, array elements' members), so that writing it as
// an edit of the archetype's value gives this value. False when the field holds the archetype's value (or cannot
// be read).
ASSETDOCUMENTEDITOR_API bool ReadChanged(UObject& Object, const FAdmittedField& Field, FAssetDocumentRuntimeValueDto& OutValue);
// True when the field holds the same document value as Object's archetype (the class
// default object, a Blueprint component template, ...): not an instance override.
ASSETDOCUMENTEDITOR_API bool IsIdenticalToArchetype(UObject& Object, FStringView Property);
ASSETDOCUMENTEDITOR_API bool IsIdenticalToArchetype(UObject& Object, const FAdmittedField& Field);

}
