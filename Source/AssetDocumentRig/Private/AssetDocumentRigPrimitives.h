// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "AssetDocumentGraphPrimitives.h"

class FJsonObject;
class FJsonValue;
class UIKRigDefinition;
class UIKRetargeter;
class IAssetDocumentDerivedStateRebuilder;
struct FIKRetargetOpBase;
struct FInstancedStruct;
enum class ERetargetSourceOrTarget : uint8;

// Rig asset primitives (canonical provider "rig", owner Rig) of the IK Rig and IK Retargeter profiles
// (docs/architecture/asset-document-ikrig-design.md §4.4). Same conventions as the audio provider: one "asset"
// handle, value DTOs with schema_version 1, whole-package footprint for mutating calls, errors as Graph.* /
// IKRig.* / IKRetargeter.* codes.
namespace UE::AssetDocument::Graph::Rig
{
inline const TCHAR* ProviderId = TEXT("rig");

TConstArrayView<FAssetDocumentPrimitiveDescriptor> GetDescriptors();
TConstArrayView<FBinding> GetBindings();

bool Fail(FCall& Call, FStringView Code, FStringView Message, FStringView Path = TEXTVIEW("/"));

// Shortest decimal that reads back as the same float32 (0.1, not 0.10000000149011612).
double ShortestFloat(double Value);

// Path of an object; "" for null.
FString PathOf(const UObject* Object);

// A name as a document string ("" for None).
FValue MakeName(FName Name);

// ---- values (AssetDocumentRigValues.cpp) ------------------------------------------------------------------------

// A field the Details panel edits on a solver / goal / bone / op settings struct, i.e. what the IK Rig controllers'
// CopyAllEditableProperties copies (not BlueprintReadOnly, not Transient) and what the struct viewer shows as
// editable (CPF_Edit, not EditConst), never a deprecated field.
bool IsEditableField(const FProperty& Property);

// Typed value of one field of a struct instance (the Editor typed value codec). False with OutError for a field
// the codec cannot represent.
bool EncodeField(const FProperty& Property, const void* Container, FValue& OutValue, FString& OutError);

// The editable fields of Struct whose value in Value differs from Defaults (both instances of Struct), in
// declaration order, as one document object; Skip filters out fields owned by other document keys.
bool EncodeSparse(const UStruct& Struct, const void* Value, const void* Defaults, FValue& OutObject, FString& OutError,
	TFunctionRef<bool(const FProperty&)> Skip);
bool EncodeSparse(const UStruct& Struct, const void* Value, const void* Defaults, FValue& OutObject, FString& OutError);

// The editable fields of Struct with their values in Defaults (discovery: list_types rows); Skip as above.
TArray<TSharedPtr<FJsonValue>> DescribeFields(const UStruct& Struct, const void* Defaults,
	TFunctionRef<bool(const FProperty&)> Skip);

// A struct instance of Type (default constructed), owned for the scope.
struct FStructInstance
{
	explicit FStructInstance(const UScriptStruct* InType);
	~FStructInstance();
	FStructInstance(const FStructInstance&) = delete;
	FStructInstance& operator=(const FStructInstance&) = delete;

	void* Get() { return Type != nullptr ? Memory.GetData() : nullptr; }
	const void* Get() const { return Type != nullptr ? Memory.GetData() : nullptr; }

	const UScriptStruct* Type;
	TArray<uint8, TAlignedHeapAllocator<16>> Memory;
};

// ---- type names ---------------------------------------------------------------------------------------------------

// Document name of a solver type: the short name of the six engine solvers (FullBodyIK, LimbIK, Pole, BodyMover,
// SetTransform, StretchLimb), else the struct path.
FString GetSolverTypeName(const UScriptStruct& Type);
// Solver type of a document name (short name or /Script path); null for an unknown or non-solver name.
UScriptStruct* FindSolverType(const FString& Name);
// Every solver type the solver stack's "Add New Solver" menu offers (SIKRigSolverStack::CacheSolverMetaData:
// every child of FIKRigSolverBase), sorted by path.
TArray<UScriptStruct*> GetSolverTypes();

// Document name of a retarget op type: for the IKRig module's ops the struct name without the "IKRetarget" prefix
// and "Op" suffix (FKChains, RunIKRig, PelvisMotion, ...), else the struct path.
FString GetOpTypeName(const UScriptStruct& Type);
UScriptStruct* FindOpType(const FString& Name);
// An op type that only classifies others (the op stack never adds one).
bool IsAbstractOpType(const UScriptStruct& Type);
// Every op type the op stack's "Add New" menu offers (every child of FIKRetargetOpBase that is not abstract).
TArray<UScriptStruct*> GetOpTypes();

// ---- retarget op settings layout (design §3.3) ---------------------------------------------------------------------

// Fields of an op settings struct owned by other document keys: chain-row arrays (element structs with a
// TargetChainName field: "Chains"), chain mappings ("ChainMapping"), the per-op target IK Rig ("TargetIKRig") and, on
// engines that keep it in the settings, the op's on/off switch ("Enabled").
bool IsChainRowArray(const FProperty& Property);
bool IsChainMappingField(const FProperty& Property);
bool IsTargetRigField(const FProperty& Property);
bool IsOpOwnedField(const FProperty& Property);

// ---- IK Rig ----------------------------------------------------------------------------------------------------

// The rig's excluded bones in skeleton order, then any excluded bone the stored skeleton does not have.
TArray<FName> GetOrderedExcludedBones(const UIKRigDefinition& Rig);

// A goal's offset (CurrentTransform relative to InitialTransform: the gizmo offset from the bone's ref pose) as
// {Location?: {X,Y,Z}, Rotation?: {Pitch,Yaw,Roll}}; bOutIdentity when both are (nearly) zero.
// Both directions use the initial transform with a normalized rotation: a ref pose rotation built in single precision
// is not quite unit length, and an offset through it would not read back as written (10 cm came back as 9.999978).
FValue EncodeGoalOffset(const FTransform& Current, const FTransform& Initial, bool& bOutIdentity);
// The goal transform for an offset (the inverse of EncodeGoalOffset): Offset * Initial.
FTransform ApplyGoalOffset(const FTransform& Offset, const FTransform& Initial);

// ---- read records (AssetDocumentIKRigRead.cpp, AssetDocumentIKRetargeterRead.cpp) -----------------------------

// The records of rig.ikrig.read.v1 / rig.ikretargeter.read.v1 for one asset (unpaged).
// bVerify (detail "verify"): each sparse settings object also comes with the complete defaults it is sparse over
// (the record's *_defaults fields), so a verification can compare an authored value that equals its default.
bool BuildIKRigRecords(const UIKRigDefinition& Rig, bool bInspect, FCall& Call, TArray<FValue>& Records, bool bVerify = false);
bool BuildIKRetargeterRecords(const UIKRetargeter& Retargeter, bool bInspect, FCall& Call, TArray<FValue>& Records,
	bool bVerify = false);

// ---- IK Retargeter ---------------------------------------------------------------------------------------------

// "Source" / "Target".
const TCHAR* RetargetSideName(ERetargetSourceOrTarget Side);
// The op named Name (FName compare: case-insensitive, as the op stack compares names); null for None / unknown.
const FIKRetargetOpBase* FindRetargetOp(const UIKRetargeter& Retargeter, FName Name);
// What the op stack's "Add New" makes for an op of Type under ParentName: a default op after
// FIKRetargetOpBase::OnAddedToStack against Retargeter (the defaults op settings are sparse over).
FInstancedStruct MakeAddedOp(const UIKRetargeter& Retargeter, const UScriptStruct& Type, FName ParentName);

// ---- status (AssetDocumentRigStatus.cpp) -----------------------------------------------------------------------

// The editors' processor initialization as a compile status (design §5): {Status, Messages[{Severity, Message,
// Path?, Source}]}, Status one of UpToDate, UpToDateWithWarnings, Error, Unknown (no preview mesh / meshes).
// False for other assets. Never changes the asset (a retargeter is initialized on a transient copy: the processor
// writes editor-instance pointers back onto the ops it copies).
bool DescribeStatusValue(const UObject& Asset, FValue& OutStatus);
// The same as a compile status describer (AssetDocumentCompileStatus.h).
TSharedPtr<FJsonObject> DescribeStatus(const UObject& Asset);

// ---- list_types kinds (AssetDocumentRigCatalog.cpp) ------------------------------------------------------------

bool ListSolverTypes(const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError);
bool ListOpTypes(const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError);

// ---- rollback (AssetDocumentRigRebuilder.cpp) ---------------------------------------------------------------------

// The Core package-snapshot rebuilder of IK Rigs and retargeters: what the editor's Undo does after a restore
// (PostEditUndo has run; the controller's BroadcastNeedsReinitialized re-initializes solvers and goal transforms
// and tells open editors / anim nodes).
TSharedRef<IAssetDocumentDerivedStateRebuilder> MakeRigRebuilder();

// ---- handlers -------------------------------------------------------------------------------------------------

bool HandleIKRigRead(FCall& Call, FObjectBuilder& Result);
bool HandleIKRetargeterRead(FCall& Call, FObjectBuilder& Result);
bool HandleStatus(FCall& Call, FObjectBuilder& Result);
bool HandleCatalog(FCall& Call, FObjectBuilder& Result);
bool HandleIKRigEdit(FCall& Call, FObjectBuilder& Result);
bool HandleIKRetargeterEdit(FCall& Call, FObjectBuilder& Result);
bool HandleSuggest(FCall& Call, FObjectBuilder& Result);

// Pages records under the runtime value budget: {total, next_offset, records}.
void AddRecordPage(TArray<FValue>& Records, int64 Offset, FObjectBuilder& Result);
} // namespace UE::AssetDocument::Graph::Rig
