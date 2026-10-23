// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "AssetDocumentEditorObjectProperties.h"
#include "AssetDocumentTypes.h"
#include "AssetDocumentLevelActorRef.h"
#include "Components/ActorComponent.h"
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "LevelEditValues.h"
#include "Templates/Function.h"
#include "Templates/UniquePtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

class AActor;
class UActorComponent;
class UClass;
class ULevel;
class UWorld;

// edit_level (POST /assetdoc/level/edit): request model, the symbolic plan the
// preflight runs on, and the apply context of the transaction step.
// docs/architecture/level-editing-tools-design.md §2.3, §2.4, §3; MCP/schemas/LevelEditing.md.
namespace UE::AssetDocumentLevel::Private
{
class FLevelEditPlan;
class FLevelEditApply;

// One authored reflected property value.
struct FLevelPropertyEdit
{
	FString Name;
	FLevelValue Value;
	// JSON pointer of the value in the request ("/ops/1/properties/Intensity").
	FString Pointer;
};

struct FLevelComponentEdit
{
	FString Name;
	// "/ops/1/components/LightComponent0"
	FString Pointer;
	TArray<FLevelPropertyEdit> Properties;
};

enum class ELevelTransformMember : uint8
{
	Location,
	Rotation,
	Scale,
};

// The root component's relative transform as the Details panel shows it; members
// are partial structs ({"Z": 50}), omitted members keep their value.
struct FLevelTransformEdit
{
	bool bWorld = false;
	FString Pointer;
	TOptional<FLevelValue> Members[3];

	static const TCHAR* MemberField(ELevelTransformMember Member);
	static const TCHAR* MemberProperty(ELevelTransformMember Member);
};

// The fields spawn, duplicate and set share. Omitted = unchanged.
struct FLevelActorFieldEdits
{
	TOptional<FLevelTransformEdit> Transform;
	TOptional<FString> Label;
	FString LabelPointer;
	TOptional<FString> Folder;
	FString FolderPointer;
	TOptional<FLevelValue> Tags;
	FString TagsPointer;
	TArray<FLevelPropertyEdit> Properties;
	TArray<FLevelComponentEdit> Components;
	// Whole lists: layer names (non-partitioned levels), data layer references (asset
	// path, or an instance / short name when unique; World Partition).
	TOptional<TArray<FString>> Layers;
	FString LayersPointer;
	TOptional<TArray<FString>> DataLayers;
	FString DataLayersPointer;

	bool HasAny() const;
};

// One entry of the change list (the diff_asset_document shape).
struct FLevelChange
{
	FString Op;
	// Add, Remove, Replace
	FString Kind;
	// actor, component, property, transform, label, folder, attachment, tags, layers,
	// data_layers, pin
	FString Object;
	FString Actor;
	FString Component;
	FString Property;
	TSharedPtr<FJsonValue> Before;
	TSharedPtr<FJsonValue> After;
	TSharedPtr<FJsonObject> Extra;

	TSharedRef<FJsonObject> ToJson() const;
};

// One op of the batch. Parse builds it from the request; Preflight validates it
// against the plan (the state after the earlier ops) and records planned changes;
// Apply runs it through the editor API inside the batch transaction and records the
// changes and the read-back expectations the verify step checks.
class FLevelEditOp
{
public:
	virtual ~FLevelEditOp() = default;

	int32 Index = 0;
	// "/ops/<Index>"
	FString Pointer;
	// The batch id this op defines ("" when none).
	FString BatchId;

	virtual const TCHAR* GetKind() const = 0;
	virtual void Preflight(FLevelEditPlan& Plan) const = 0;
	virtual bool Apply(FLevelEditApply& Apply) const = 0;
	// Post-commit helpers (pin / unpin): run after the batch transaction committed,
	// never rolled back (design §3.1 step 9). Not called when the batch failed.
	virtual bool IsPostCommitHelper() const { return false; }
	virtual void PostCommit(FLevelEditApply& Apply) const {}
};

// Parses one op object (its "op" field already read). Diagnostics use Pointer as prefix.
using FLevelEditOpParser = TFunction<TUniquePtr<FLevelEditOp>(const FJsonObject& Json, const FString& Pointer,
	TArray<FAssetDocumentDiagnostic>& OutDiagnostics)>;

// The op table: each op file registers its parser once (static registration).
struct FLevelEditOpRegistration
{
	FLevelEditOpRegistration(const TCHAR* Kind, FLevelEditOpParser Parser);
};
const FLevelEditOpParser* FindLevelEditOpParser(const FString& Kind);
TArray<FString> GetLevelEditOpKinds();

struct FLevelEditRequest
{
	FString Description;
	bool bDryRun = false;
	bool bSave = false;
	TArray<TUniquePtr<FLevelEditOp>> Ops;
	TArray<FActorRef> Select;
	TArray<FActorRef> Focus;
	bool bHasSelect = false;
};

constexpr int32 MaximumLevelEditOps = 500;

bool ParseLevelEditRequest(const FJsonObject& Json, FLevelEditRequest& OutRequest, TArray<FAssetDocumentDiagnostic>& OutDiagnostics);

// Shared parse helpers of the op files.
FAssetDocumentDiagnostic MakeInvalidRequest(const FString& Path, const FString& Message);
bool CheckLevelOpFields(const FJsonObject& Json, const FString& Pointer, TConstArrayView<const TCHAR*> Allowed,
	TArray<FAssetDocumentDiagnostic>& OutDiagnostics);
bool ParseLevelActorRefField(const FJsonObject& Json, const TCHAR* Field, const FString& Pointer, bool bRequired,
	FActorRef& OutRef, TArray<FAssetDocumentDiagnostic>& OutDiagnostics);
bool ParseLevelBatchIdField(const FJsonObject& Json, const FString& Pointer, FString& OutId,
	TArray<FAssetDocumentDiagnostic>& OutDiagnostics);
bool ParseLevelActorFieldEdits(const FJsonObject& Json, const FString& Pointer, FLevelActorFieldEdits& OutEdits,
	TArray<FAssetDocumentDiagnostic>& OutDiagnostics);
bool ParseLevelVector(const TSharedPtr<FJsonValue>& Json, const FString& Pointer, FVector& OutVector,
	TArray<FAssetDocumentDiagnostic>& OutDiagnostics);
// The fields every op that edits actor fields accepts.
extern const TCHAR* const LevelActorFieldNames[8];

// ---------------------------------------------------------------------------
// The symbolic plan (§3.2): actors that exist, will be created, or were deleted by an
// earlier op, each with its class, level and the object whose values stand in for it
// while it does not exist yet (class default object; the source of a duplicate).

// A component of a plan actor: the live ones, the class's (for an actor the batch
// creates), and those earlier ops add, remove or reparent.
struct FLevelPlanComponent
{
	FString Name;
	TWeakObjectPtr<UClass> Class;
	// The attach parent component's name ("" for the root and non-scene components).
	FString Parent;
	FString Socket;
	EComponentCreationMethod Creation = EComponentCreationMethod::Native;
	bool bScene = false;
	bool bRoot = false;
	// The Details panel shows it editable (FComponentEditorUtils::CanEditComponentInstance).
	bool bEditable = true;
	// The op that added it (add_component), else INDEX_NONE.
	int32 AddedByOp = INDEX_NONE;
	// The op that removed it (remove_component), else INDEX_NONE.
	int32 RemovedByOp = INDEX_NONE;
};

struct FLevelPlanActor
{
	bool bExisting = false;
	// Added only to model the attachment graph (the attach parent of an actor the
	// batch touches); not resolved as a target of an op.
	bool bRegisteredOnly = false;
	TWeakObjectPtr<AActor> Actor;
	FString Path;
	FString BatchId;
	int32 CreatedByOp = INDEX_NONE;
	TWeakObjectPtr<UClass> Class;
	TWeakObjectPtr<ULevel> Level;
	// Duplicates: the source actor (component names and values are copied from it).
	TWeakObjectPtr<AActor> Source;
	bool bDeleted = false;

	bool bComponentsKnown = false;
	TArray<FLevelPlanComponent> Components;
	// The attach parent in the plan (an index into the plan's actors), once known.
	bool bParentKnown = false;
	int32 ParentIndex = INDEX_NONE;
	FString ParentSocket;

	// The path, "@id", or "(created by /ops/N)".
	FString Display() const;
};

// A property value an earlier op of the batch writes (preflight), for the delete
// reference check: the JSON of the value after the write.
struct FLevelPlannedValue
{
	int32 ActorIndex = INDEX_NONE;
	FString Component;
	FString Property;
	TSharedPtr<FJsonValue> After;
	int32 OpIndex = INDEX_NONE;
};

class FLevelEditPlan
{
public:
	explicit FLevelEditPlan(UWorld& InWorld);

	UWorld& World;
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	TArray<FAssetDocumentDiagnostic> Warnings;
	TArray<FLevelChange> Changes;
	TArray<FLevelPlanActor> Actors;
	TArray<FLevelPlannedValue> PlannedValues;
	int32 Spawns = 0;
	int32 Deletes = 0;
	int32 Duplicates = 0;
	// The op being preflighted (Op::Index).
	int32 CurrentOp = INDEX_NONE;

	void AddError(const FString& Path, const FString& Code, const FString& Message, TSharedPtr<FJsonObject> Details = nullptr);
	void AddWarning(const FString& Path, const FString& Code, const FString& Message, TSharedPtr<FJsonObject> Details = nullptr);

	// The plan actor an ActorRef names (existing, loaded, editable, not deleted earlier
	// in the batch; or a batch id defined earlier). INDEX_NONE with a diagnostic. A path,
	// guid or label is resolved once, against the level before the request, and the
	// actor's path is recorded under Pointer (RefPaths): apply, the after helpers and
	// pin use that path, so a label an earlier op changes cannot redirect a later op.
	int32 Resolve(const FActorRef& Ref, const FString& Pointer);
	// Pointer of a path, guid or label reference -> the canonical path preflight resolved.
	TMap<FString, FString> RefPaths;
	int32 AddPlanned(int32 OpIndex, const FString& InBatchId, UClass& Class, ULevel& Level, AActor* Source);
	// The plan entry of a live actor, added without the checks Resolve makes (to model
	// attachments); Resolve still checks it when an op names it.
	int32 FindOrAddExisting(AActor& Actor);
	// Whether an earlier op deleted the actor at Path.
	bool IsDeletedPath(const FString& Path) const;
	// The plan entry of the actor at Path, INDEX_NONE when the plan has none.
	int32 FindByPath(const FString& Path) const;

	// The components of a plan actor (initialised from the live actor, the duplicate
	// source, or the class), including the changes of earlier ops.
	TArray<FLevelPlanComponent>& GetComponents(int32 ActorIndex);
	// A component of the plan actor that exists at this point of the batch; null when
	// there is none (removed ones are not found).
	FLevelPlanComponent* FindComponent(int32 ActorIndex, const FString& Name);
	// A component for an op at Pointer, with Level.ComponentNotFound when there is none.
	FLevelPlanComponent* RequireComponent(int32 ActorIndex, const FString& Name, const FString& Pointer);
	// The object standing in for a component of the plan actor (live component, or the
	// class template / the class default object of an added component's class).
	UObject* GetComponentStandIn(int32 ActorIndex, const FLevelPlanComponent& Component);

	// The plan actor's attach parent (INDEX_NONE when it is not attached).
	int32 GetParent(int32 ActorIndex);
	// Validates attaching Child to Parent as UEditorEngine::CanParentActors would at
	// this point of the batch (Level.AttachRejected with the editor's reason) and the
	// component / socket; records the planned attachment. False with a diagnostic.
	bool PreflightAttach(int32 Child, int32 Parent, const FString& Socket, const FString& Component, const FString& Pointer);
	// The plan actor's object standing in for it in checks (live actor, duplicate
	// source, or class default object).
	AActor* GetStandIn(int32 ActorIndex) const;

	// Outliner folders of the world at this point of the batch.
	TSet<FString>& GetFolders();

	// A loaded, unlocked level of the editor world by package ("" = the current level).
	ULevel* ResolveTargetLevel(const FString& Package, const FString& Pointer);

	// Validates the field edits for Actors[ActorIndex] and records planned changes.
	void PreflightFields(int32 ActorIndex, const FLevelActorFieldEdits& Edits, const FString& OpPointer);

	// The object whose values stand in for the actor (or its component) during
	// preflight: the live actor, the duplicate source, or the class default object /
	// component template. Null with a diagnostic when the component is unknown or not
	// editable.
	UObject* GetValueOwner(int32 ActorIndex, const FString& Component, const FString& Pointer);

	TFunction<UObject*(FStringView)> MakeBatchResolver() const;

	// Hard (actor), Level Blueprint and soft referencers of an actor about to be deleted
	// (what the Delete key's dialog lists), not counting actors in IgnoredPaths.
	TArray<TSharedPtr<FJsonValue>> FindReferencers(AActor& Actor, const TSet<FString>& IgnoredPaths);

	// The planned referencers of the plan actor Target (batch-created actors and
	// duplicates whose planned values name it), and whether an existing referencer
	// still references it after the earlier ops' writes. Used by delete.
	TArray<TSharedPtr<FJsonValue>> FindPlannedReferencers(int32 Target, const TSet<FString>& IgnoredPaths);
	// bIfNoneFound: the answer when no property of LiveReferencer references Target.
	bool StillReferences(int32 Referencer, AActor& LiveReferencer, int32 Target, bool bIfNoneFound = true);

	// Layers / data layers of a field edit (Ops/LevelOpLayers.cpp).
	void PreflightLayers(int32 ActorIndex, const FLevelActorFieldEdits& Edits);

private:
	TMap<FString, int32> ByPath;
	TMap<FString, int32> ByBatchId;
	bool bReferenceMapBuilt = false;
	TMap<TWeakObjectPtr<AActor>, TArray<TWeakObjectPtr<AActor>>> ReferenceMap;
	bool bFoldersKnown = false;
	TSet<FString> Folders;
};

// ---------------------------------------------------------------------------
// The apply step: runs inside the batch transaction; every op re-resolves the actors
// it touches by path (a construction script rerun recreates components, and the
// editor may collect garbage between steps).

class FLevelEditApply
{
public:
	TMap<FString, FString> BatchPaths;
	// From preflight (FLevelEditPlan::RefPaths): the actor each path, guid or label
	// reference names, by its pointer.
	TMap<FString, FString> RefPaths;
	// The reference preflight resolved at Pointer: a path reference to that actor, or Ref
	// itself ("@id", or a pointer preflight did not resolve).
	FActorRef PinRef(const FActorRef& Ref, const FString& Pointer) const;
	TArray<FLevelChange> Changes;
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	TArray<FAssetDocumentDiagnostic> Warnings;
	// Per op: the result entry of the response (actor references it created / deleted).
	TArray<TSharedPtr<FJsonObject>> OpResults;
	// Actors the batch spawned, duplicated or changed, and the packages of deleted ones.
	TArray<FString> TouchedActorPaths;
	TArray<FString> DeletedActorPackages;
	// Packages of the actor folder objects the batch created, renamed or deleted.
	TArray<FString> FolderPackages;
	// Folders the batch created or renamed that hold no actor, in a level without actor folder objects: the editor keeps
	// them for this session only (they are not part of any package), so a save does not keep them.
	TArray<FString> SessionOnlyFolders;
	TSet<FString> DeletedPaths;
	// "<actor path>|<component>" of components removed by the batch.
	TSet<FString> RemovedComponents;
	int32 Spawned = 0;
	int32 Deleted = 0;
	int32 Duplicated = 0;
	int32 ValuesSet = 0;
	int32 ComponentsChanged = 0;
	int32 Attachments = 0;
	int32 FolderOps = 0;
	// Post-commit helper results (pin / unpin).
	TArray<TSharedPtr<FJsonValue>> Pinned;
	TArray<TSharedPtr<FJsonValue>> Unpinned;

	UWorld* GetWorld() const;
	AActor* ResolvePath(const FString& Path) const;
	AActor* ResolveRef(const FActorRef& InRef, const FString& Pointer);
	UObject* ResolveBatchObject(FStringView Id) const;
	void AddError(const FString& Path, const FString& Code, const FString& Message, TSharedPtr<FJsonObject> Details = nullptr);
	void AddWarning(const FString& Path, const FString& Code, const FString& Message, TSharedPtr<FJsonObject> Details = nullptr);
	void Touch(const AActor& Actor);

	// Applies field edits to the actor at ActorPath, Details-equivalently.
	bool ApplyFields(const FString& ActorPath, const FLevelActorFieldEdits& Edits, const FString& OpPointer);

	// Records that the actor at Path must exist (with Class) after the batch.
	void ExpectAlive(const FString& Path, UClass* Class, const FString& Pointer);
	// Records that the actor at Path must be gone after the batch.
	void ExpectGone(const FString& Path, const FString& Pointer);
	// Records the final state of a component (alive with class / attach parent / socket,
	// or gone).
	void ExpectComponent(const FString& ActorPath, const FString& Name, UClass* Class, const FString& Parent,
		const FString& Socket, const FString& Pointer);
	void ExpectComponentGone(const FString& ActorPath, const FString& Name, const FString& Pointer);
	// Records the final attachment of an actor (ParentPath "" = detached).
	void ExpectAttachment(const FString& ActorPath, const FString& ParentPath, const FString& Socket, const FString& Component,
		const FString& Pointer);
	// Records that a folder must (not) exist after the batch.
	void ExpectFolder(const FString& Path, bool bExists, const FString& Pointer);
	// The actor's attachment changes and the editor keeps its world transform: the
	// relative transform written earlier in the batch is checked now and its
	// expectation dropped (it was relative to the old parent), and earlier folder
	// expectations of the actor and its attached children give way to the editor's
	// folder rule. False with Level.PropertyValueNotStoredExactly.
	bool SettleBeforeReparent(AActor& Actor, const FString& Pointer);
	// A component the batch removes: its expectations go, and those of its children name
	// NewParentForChildren (DeleteComponents promotes them).
	void ForgetComponent(const FString& ActorPath, const FString& Name, const FString& NewParentForChildren);
	// The component is reparented keeping its world transform: its relative transform
	// values written earlier in the batch are checked now and their expectations dropped.
	bool SettleComponentTransform(const FString& ActorPath, const FString& Name);
	// Folder expectations of actors follow a folder rename / delete (From -> To).
	void MoveFolderExpectations(const FString& From, const FString& To);
	// Adds a change entry.
	FLevelChange& AddChange(const FString& Op, const TCHAR* Kind, const TCHAR* Object, const FString& Actor);

	// The verify step: reads back every expectation (the final value of each target).
	bool Verify();

private:
	struct FPropertyExpectation
	{
		FString ActorPath;
		FString Component;
		bool bRoot = false;
		FString Property;
		FLevelValue Authored;
		FString Pointer;
	};
	struct FActorExpectation
	{
		FString Path;
		FString Pointer;
		bool bAlive = true;
		TWeakObjectPtr<UClass> Class;
	};
	struct FFieldExpectation
	{
		FString ActorPath;
		// "label" or "folder"
		FString Field;
		FString Value;
		FString Pointer;
	};
	struct FComponentExpectation
	{
		FString ActorPath;
		FString Name;
		bool bAlive = true;
		TWeakObjectPtr<UClass> Class;
		FString Parent;
		FString Socket;
		FString Pointer;
	};
	struct FAttachExpectation
	{
		FString ActorPath;
		FString ParentPath;
		FString Socket;
		FString Component;
		FString Pointer;
	};
	struct FListExpectation
	{
		FString ActorPath;
		// "layers" or "data_layers"
		FString Field;
		TArray<FString> Values;
		FString Pointer;
	};
	struct FFolderExpectation
	{
		FString Path;
		bool bExists = true;
		FString Pointer;
	};
	TArray<FPropertyExpectation> PropertyExpectations;
	TArray<FActorExpectation> ActorExpectations;
	TArray<FFieldExpectation> FieldExpectations;
	TArray<FComponentExpectation> ComponentExpectations;
	TArray<FAttachExpectation> AttachExpectations;
	TArray<FListExpectation> ListExpectations;
	TArray<FFolderExpectation> FolderExpectations;

	UObject* ResolveTarget(const FString& ActorPath, const FString& Component, bool bRoot) const;
	bool VerifyProperty(const FPropertyExpectation& Expectation);
	// Components, attachments, folders, layers and data layers.
	bool VerifyStructure();
	// Verifies the property expectations Filter selects now and drops them.
	bool SettleProperties(TFunctionRef<bool(const FPropertyExpectation&)> Filter);
	bool ApplyPropertyEdit(const FString& ActorPath, const FString& Component, bool bRoot, const FString& Property,
		const FLevelValue& Value, const FString& Pointer, const FString& ChangeOp, const TCHAR* ChangeObject,
		const FString& ChangeProperty);
	bool ApplyTransform(const FString& ActorPath, const FLevelTransformEdit& Transform);
	// Ops/LevelOpLayers.cpp
	bool ApplyLayers(const FString& ActorPath, const FLevelActorFieldEdits& Edits);
	bool VerifyLists();
};

// attach and spawn's attach_to (Ops/LevelOpAttach.cpp): CanParentActors, then
// ParentActors, with the change entry and the expectation.
bool ApplyLevelAttach(FLevelEditApply& Apply, AActor& Child, AActor& Parent, const FString& Socket, const FString& Component,
	const FString& Pointer);

// Shared by plan and apply.
UActorComponent* FindLevelActorComponent(AActor& Actor, const FString& Name);
TArray<FString> ListLevelActorComponentNames(AActor& Actor);
// The template of a component the class creates (native default subobject, or a
// Blueprint component along the class chain); null when there is none.
UActorComponent* FindLevelClassComponentTemplate(UClass& Class, const FString& Name);
TArray<FString> ListLevelClassComponentNames(UClass& Class);
// Maps a property diagnostic of the object property API ("/<Property>/inner") to the
// request: Prefix + "/inner" (Prefix names the property value).
FAssetDocumentDiagnostic ProjectLevelPropertyDiagnostic(const FAssetDocumentRuntimeDiagnostic& Diagnostic, const FString& Prefix,
	const FString& PropertyName);
UE::AssetDocument::EditorObjectProperties::FReferencePolicy MakeLevelReferencePolicy(const ULevel* Level,
	TFunction<UObject*(FStringView)> ResolveBatchId);
FString GetLevelPackageOf(const AActor& Actor);
const TCHAR* DescribeLevelComponentCreation(EComponentCreationMethod Method);
// The Outliner folder object of a path in World (the world's root folder object).
FString GetLevelFolderText(const AActor& Actor);
// Every Outliner folder of World.
TSet<FString> GatherLevelFolders(UWorld& World);
// Data layer instance of World for a reference (asset path, instance name or unique
// short name); null with OutError (and the known names) when there is none.
class UDataLayerInstance* FindLevelDataLayerInstance(UWorld& World, const FString& Reference, FString& OutError,
	TArray<FString>* OutKnown = nullptr);
}
