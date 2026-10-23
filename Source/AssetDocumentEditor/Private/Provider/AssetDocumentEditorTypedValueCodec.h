// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"
#include "UObject/StrongObjectPtr.h"

class FProperty;
class UObject;

struct FAssetDocumentEditorTypedValueCodecResult
{
	bool bSucceeded = false;
	FString CodecId;
	FString DiagnosticCode;
	// Decode failures: the JSON-pointer suffix inside the property value that is
	// wrong ("" = the value itself) and an AI-readable reason. Empty otherwise.
	FString ValuePath;
	FString Message;
};

class ASSETDOCUMENTEDITOR_API FAssetDocumentEditorScopedPropertyValue
{
public:
	explicit FAssetDocumentEditorScopedPropertyValue(FProperty& InProperty);
	~FAssetDocumentEditorScopedPropertyValue();

	FAssetDocumentEditorScopedPropertyValue(
		const FAssetDocumentEditorScopedPropertyValue&) = delete;
	FAssetDocumentEditorScopedPropertyValue& operator=(
		const FAssetDocumentEditorScopedPropertyValue&) = delete;

	FProperty& GetProperty() const;
	void* GetMemory() const;
	bool IsValid() const;

	// Reads Source (memory holding a value of the property) into this value. When
	// the property's C++ copy does not reproduce Source (a struct whose copy leaves
	// reflected members out, such as an owner the copy does not share), the value
	// views Source itself, so an encode reports what is stored. Such a value is
	// read-only and must not outlive Source.
	void ReadFrom(const void* Source);

	// Instanced subobjects a decode created for this value. They live in the
	// transient package until a write adopts them into the owning object (see
	// AdoptAssetDocumentEditorInstancedValue).
	void AddStagedObject(UObject& Object);
	bool IsStagedObject(const UObject* Object) const;
	bool HasStagedObjects() const;

private:
	FProperty* Property = nullptr;
	void* Memory = nullptr;
	const void* View = nullptr;
	TArray<TStrongObjectPtr<UObject>> StagedObjects;
};

// Reads Object's value of Property as stored (see ReadFrom). False when the
// property cannot be read.
bool ReadAssetDocumentEditorObjectPropertyValue(
	const UObject& Object,
	FProperty& Property,
	FAssetDocumentEditorScopedPropertyValue& OutValue);

ASSETDOCUMENTEDITOR_API FAssetDocumentEditorTypedValueCodecResult
InspectAssetDocumentEditorTypedValueProperty(FProperty& Property);

// The value a decode edits. With a current value the document is an edit of it
// ("omitted = keep current value, explicit value = set"): struct members and
// instanced-subobject / instanced-struct Properties the document omits keep their
// current value (a new element, map entry or subobject of another class starts
// from its default / class default object), a changed FText source string keeps
// the text's namespace and key (as the Details text box does), NoClear refuses
// clearing a set reference, and GetAllowedClasses/GetDisallowedClasses functions
// run on Owner. Without a current value every struct lists all its members.
struct FAssetDocumentEditorDecodeBase
{
	// The property's current value memory (nullptr: no current value).
	const void* Value = nullptr;
	// The object that holds the property (text keys, picker filter functions).
	UObject* Owner = nullptr;
};

// Decodes a document value into native memory. Hard object/class references are
// resolved (loading the asset when needed) and checked against the property type
// and the class filters the Details panel picker applies; instanced subobjects
// ({"Class", "Properties"}) are constructed in the transient package and staged
// on OutNativeValue.
ASSETDOCUMENTEDITOR_API FAssetDocumentEditorTypedValueCodecResult
DecodeAssetDocumentEditorTypedValue(
	FProperty& Property,
	const FAssetDocumentRuntimeValueDto& Value,
	TUniquePtr<FAssetDocumentEditorScopedPropertyValue>& OutNativeValue);

ASSETDOCUMENTEDITOR_API FAssetDocumentEditorTypedValueCodecResult
DecodeAssetDocumentEditorTypedValue(
	FProperty& Property,
	const FAssetDocumentRuntimeValueDto& Value,
	TUniquePtr<FAssetDocumentEditorScopedPropertyValue>& OutNativeValue,
	const FAssetDocumentEditorDecodeBase& Base);

ASSETDOCUMENTEDITOR_API FAssetDocumentEditorTypedValueCodecResult
EncodeAssetDocumentEditorTypedValue(
	const FAssetDocumentEditorScopedPropertyValue& NativeValue,
	FAssetDocumentRuntimeValueDto& OutValue);

// Encodes NativeValue in the shape of Authored (a document value of the same
// property): struct members and instanced Properties that Authored omits are left
// out and those it lists are included even when they hold the default, so the
// result equals Authored exactly when the property stores what was authored.
FAssetDocumentEditorTypedValueCodecResult
EncodeAssetDocumentEditorTypedValueAsAuthored(
	const FAssetDocumentEditorScopedPropertyValue& NativeValue,
	const FAssetDocumentRuntimeValueDto& Authored,
	FAssetDocumentRuntimeValueDto& OutValue);

// True when two native values of Property are the same document value: Identical,
// or equal encoded forms (structs such as FRichCurve carry transient caches, e.g.
// key-handle maps, that Identical compares but the document never contains).
bool AreAssetDocumentEditorTypedValuesEquivalent(
	FProperty& Property,
	const void* Left,
	const void* Right);

// True when the property value can hold instanced subobjects (directly or in a
// struct/container member). Such values are not restorable by a typed
// encode/decode preimage; Core's package snapshot restores them. With
// bIncludeInstancedStructs false, FInstancedStruct members count only through a
// statically typed instanced object member (their dynamic struct is not known here).
// Edit-inline references (EditInline metadata on a plain reference) count too: their
// own subobject is written like an instanced one; with bIncludeEditInline false they
// count as plain references (a value without an owning object holds them by path).
bool DoesAssetDocumentEditorPropertyHoldInstancedObjects(const FProperty& Property, bool bIncludeInstancedStructs = true,
	bool bIncludeEditInline = true);

// A VisibleAnywhere (EditConst) instanced subobject pointer, e.g.
// UHLODLayer::HLODBuilderSettings: the Details panel shows the subobject's own
// fields as editable but offers no class picker or Clear. Such a property is
// admitted as writable for in-place edits only: the value must name the current
// subobject's class, and the write copies the fields into that subobject
// without setting the pointer.
bool IsAssetDocumentEditorInPlaceInstancedProperty(const FProperty& Property);

// Editor-derived references. A plain hard reference (object, class, weak/lazy,
// interface; not instanced, not edit-inline) is set with the Details panel asset
// picker, which lists assets, never the edited asset's own subobjects. A value
// that holds an object inside the asset (its outer chain reaches Owner's
// top-level object; e.g. UPaperSprite::BodySetup, which every sprite
// PostEditChange recreates and renames, or a default subobject such as
// UConstantQNRT::Settings) was created by the asset's own code: documents omit
// the property, and a document value may not name such an object (replacing it
// with an asset or null stays possible, as with the picker). Edit-inline
// references (EditInline / ShowInnerProperties metadata) are edited through the
// referenced object and are not editor-derived. Returns the first such object in
// the value (struct members, container elements and static-array slots; inside
// instanced subobjects/structs only with bIntoInstanced, whose fields are edited
// in place) with OutValuePath its JSON pointer inside the property value, else
// nullptr. Objects that KeptValue (another value of Property, e.g. the current
// one) also references are skipped: keeping a stored reference is not authoring it.
const UObject* FindAssetDocumentEditorDerivedReference(
	const FProperty& Property,
	const void* Value,
	const UObject& Owner,
	FString& OutValuePath,
	bool bIntoInstanced = false,
	const void* KeptValue = nullptr);

// Prepares a decoded value for a write of its property on Owner,
// editor-equivalently: a staged subobject whose class equals the subobject
// currently at the same position (owned by Owner) is copied into that existing
// subobject (PreEditChange/PostEditChange on it per changed field when bNotify)
// and the value keeps pointing at the existing one; any other staged subobject is
// moved under Owner (like the Details panel class picker's NewObject(Owner, Class)).
// CurrentValue is the property's current memory on Owner (nullptr when unknown).
void AdoptAssetDocumentEditorInstancedValue(
	UObject& Owner,
	const FAssetDocumentEditorScopedPropertyValue& Desired,
	const void* CurrentValue,
	bool bNotify);

// Instanced subobjects of Owner that Before referenced and After no longer does
// are moved to the transient package (as the Details panel does on replace).
void ReleaseAssetDocumentEditorReplacedInstancedObjects(
	UObject& Owner,
	FProperty& Property,
	const void* BeforeValue,
	const void* AfterValue);
