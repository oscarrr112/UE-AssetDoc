// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "AssetDocumentGraphPrimitives.h"

class FJsonObject;
class UPhysicsAsset;
class UPhysicsConstraintTemplate;
class USkeletalBodySetup;
class USkeletalMesh;
class IAssetDocumentDerivedStateRebuilder;
class IAssetDocumentOpenEditorHandler;
struct FBoneVertInfo;
struct FConstraintInstance;
struct FPhysAssetCreateParams;

// Physics asset primitives (canonical provider "physics", owner Physics) of the Physics Asset profile
// (docs/architecture/asset-document-physics-asset-design.md §4). Same conventions as the rig provider: one "asset"
// handle, value DTOs with schema_version 1, whole-package footprint for mutating calls, errors as Graph.* /
// PhysicsAsset.* codes.
namespace UE::AssetDocument::Graph::Physics
{
inline const TCHAR* ProviderId = TEXT("physics");

TConstArrayView<FAssetDocumentPrimitiveDescriptor> GetDescriptors();
TConstArrayView<FBinding> GetBindings();

bool Fail(FCall& Call, FStringView Code, FStringView Message, FStringView Path = TEXTVIEW("/"));

// Shortest decimal that reads back as the same float32 (0.1, not 0.10000000149011612).
double ShortestFloat(double Value);

// A name as a document string ("" for None).
FValue MakeName(FName Name);

// Document path segment (JSON pointer escaping of ~ and /).
FString Escape(const FString& Name);

// ---- reflected types (AssetDocumentPhysicsValues.cpp) ------------------------------------------------------------
// The engine's shape and constraint structs are not exported; their UScriptStructs are found through the exported
// classes' properties.

const UScriptStruct* GetAggGeomStruct();
const UScriptStruct* GetConstraintInstanceStruct();
const UScriptStruct* GetConstraintProfileStruct();
const UScriptStruct* GetPhysicalAnimationDataStruct();
FStructProperty* GetDefaultProfileProperty();
// FPhysAssetCreateParams (the generation settings of "Create Physics Asset" and Add Body), found through
// UPhysicsAssetGenerationSettings::CreateParams.
const UScriptStruct* GetCreateParamsStruct();

// ---- values ------------------------------------------------------------------------------------------------------

// A field the Details panel edits: CPF_Edit, not EditConst, deprecated or transient.
bool IsEditableField(const FProperty& Property);

using FSkip = TFunctionRef<bool(const FProperty&)>;

// The editable fields of Struct (a UScriptStruct or a UClass) whose value in Value differs from Defaults, in
// declaration order, as one document object. Engine structs (not CoreUObject math / native-text structs) are
// compared member by member: only their differing editable members are emitted, so a document names the Details
// fields it changes. Skip filters out fields owned by other document keys or derived from others.
bool EncodeSparse(const UStruct& Struct, const void* Value, const void* Defaults, FValue& OutObject, FString& OutError, FSkip Skip);

// The reverse: Memory (an instance of Struct holding the current value) gets every editable field reset to Defaults
// (engine structs member by member; non-editable members keep their value), then the listed fields of Object decoded
// with the Editor typed value codec. False with Graph.UnknownField / PhysicsAsset.FieldNotEditable /
// PhysicsAsset.InvalidValue at Path/<field> (Reader reports it).
bool DecodeSparse(FReader& Reader, const FString& Path, const FValue& Object, const UStruct& Struct, void* Memory,
	const void* Defaults, UObject* Owner, FSkip Skip);

// Typed value of one property value (the Editor typed value codec).
bool EncodeValue(const FProperty& Property, const void* ValuePtr, FValue& OutValue, FString& OutError);

// One property value outside its container (initialized, destroyed with the scope).
struct FPropertyValue
{
	explicit FPropertyValue(const FProperty& InProperty);
	~FPropertyValue();
	FPropertyValue(const FPropertyValue&) = delete;
	FPropertyValue& operator=(const FPropertyValue&) = delete;

	const FProperty& Property;
	uint8* Memory;
};

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

// ---- bodies ------------------------------------------------------------------------------------------------------

// A body as FPhysicsAssetUtils::CreateNewBody makes it (USkeletalBodySetup defaults, CollisionTraceFlag
// UseSimpleAsComplex, PhysicsType Default): the defaults of a body's sparse Properties. Transient.
USkeletalBodySetup* MakeNewBodyDefaults();

// Body fields owned by other document keys (AggGeom: Shapes; the editor-only dummy CurrentPhysicalAnimationProfile)
// and, for a body whose collision profile is not Custom, the FBodyInstance fields the profile derives
// (CollisionEnabled, ObjectType, CollisionResponses: greyed out in the Details panel, reloaded from the profile on
// load).
bool IsBodyOwnedField(const FProperty& Property);
bool IsDerivedCollisionField(const FProperty& Property, FName CollisionProfileName);

// Shape types of a body (AggGeom arrays). Authored: Spheres, Boxes, Capsules, TaperedCapsules, Convexes; read only:
// the level set and skinned triangle mesh types (listed by Inspect, kept on apply).
struct FShapeType
{
	const TCHAR* Key;        // document key
	const TCHAR* Array;      // FKAggregateGeom member
	const TCHAR* AutoName;   // the editor's AutoNamePrimitive suffix
	bool bAuthored;
};
TConstArrayView<FShapeType> GetShapeTypes();
const FArrayProperty* GetShapeArrayProperty(const FShapeType& Type);
// FKConvexElem::VertexData (the hull's points; not a Details field).
const FArrayProperty* GetConvexVertexProperty();

// The editor's auto name of a body's shape: "<bone>_<suffix>" (FPhysicsAssetEditorSharedData::AutoNamePrimitive).
FName AutoShapeName(FName Bone, const FShapeType& Type);

// The document form of one shape element (sparse over the element's editable fields against a new element; Name
// omitted when it is the auto name; convex: VertexData and a non-identity Transform).
bool EncodeShape(const FShapeType& Type, const UScriptStruct& Struct, const void* Element, FName Bone, FValue& OutValue, FString& OutError);

// ---- constraints -------------------------------------------------------------------------------------------------

// The constraint's authored default profile: DefaultProfile, the value Serialize saves as DefaultInstance.ProfileInstance.
const void* GetDefaultProfile(const UPhysicsConstraintTemplate& Constraint);
void* GetMutableDefaultProfile(UPhysicsConstraintTemplate& Constraint);

// Whether the editor's "Snap" can run: the preview mesh (if any) has both bones (the engine asserts otherwise).
bool CanSnap(const FConstraintInstance& Instance, const UPhysicsAsset& Asset);
// The frames the editor's "Snap" gives the constraint (FConstraintInstance::SnapTransformsToDefault on a copy;
// only when CanSnap).
FConstraintInstance SnappedFrames(const FConstraintInstance& Instance, const UPhysicsAsset& Asset);
bool FramesAreSnapped(const FConstraintInstance& Instance, const UPhysicsAsset& Asset);

// FConstraintInstance fields owned by other document keys: JointName, ConstraintBone1/2 (Child / Parent), the frames
// (Frames), ProfileInstance (encoded from DefaultProfile).
bool IsConstraintOwnedField(const FProperty& Property);

// ---- order ---------------------------------------------------------------------------------------------------------

// Body indices in document order: by the preview mesh's bone order, bones the mesh lacks last (by name).
TArray<int32> BodyOrder(const UPhysicsAsset& Asset);
// Constraint indices in document order: by child bone order, then joint name.
TArray<int32> ConstraintOrder(const UPhysicsAsset& Asset);
// Document keys of the constraints (joint names; a repeated joint name gets "~2", "~3" in array order).
TArray<FString> ConstraintKeys(const UPhysicsAsset& Asset);

// ---- structural edits with the editor's results (AssetDocumentPhysicsEditorEquivalent.cpp) ------------------------

namespace EditorEquivalent
{
// Add Body on Bone: a body created with Params; with FitInfo its shapes are fitted to the bone's vertex infos and named
// after the bone (a failed fit removes the body again, as in the editor); without FitInfo the caller writes the
// document's shapes. Physical bodies below the bone are re-hung from the new body, and the new body hangs from the
// body that controlled the bone. BoneIndicesInCreation are all bones getting bodies in the same Add Bodies step (the
// editor's selected bones); engines whose fit takes them leave those bones' vertices to their own bodies. Returns the
// new body index (INDEX_NONE when the mesh lacks the bone or the fit fails).
int32 AddBody(UPhysicsAsset& Asset, USkeletalMesh& Mesh, FName Bone, const FPhysAssetCreateParams& Params, const FBoneVertInfo* FitInfo,
	TConstArrayView<int32> BoneIndicesInCreation);
// Delete Body: the nearest bodies below get copies of their joints hanging from the body above, then the body and its
// joints go (FPhysicsAssetUtils::DestroyBody).
void RemoveBody(UPhysicsAsset& Asset, int32 BodyIndex);
// A new joint named JointName between ChildBody and ParentBody (ConnectJoint). INDEX_NONE when joints cannot be created.
int32 AddJoint(UPhysicsAsset& Asset, FName JointName, int32 ChildBody, int32 ParentBody);
// Points Joint at ChildBody (bone 1) and ParentBody (bone 2), snaps its frames to the child bone, makes its current
// settings the default profile and turns collision off between the two bodies.
void ConnectJoint(UPhysicsAsset& Asset, UPhysicsConstraintTemplate& Joint, int32 ChildBody, int32 ParentBody);
// Collision on or off between two bodies (the editor's Enable / Disable Collision, without its viewport refresh).
void SetPairCollision(UPhysicsAsset& Asset, int32 BodyA, int32 BodyB, bool bEnable);
// What the editor does after an undo or a structural change: index map, bounds bodies, physics state of components.
void Refresh(UPhysicsAsset& Asset);
}

// ---- status (AssetDocumentPhysicsStatus.cpp) -----------------------------------------------------------------------

// Data validation and structural checks as a compile status (design §5): {Status, Messages[{Severity, Message,
// Path?, Source}]}, Status one of UpToDate, UpToDateWithWarnings, Error, Unknown (no preview mesh). False for other
// assets. Never changes the asset.
bool DescribeStatusValue(const UObject& Asset, FValue& OutStatus);
TSharedPtr<FJsonObject> DescribeStatus(const UObject& Asset);

// ---- rollback (AssetDocumentPhysicsRebuilder.cpp) ------------------------------------------------------------------

TSharedRef<IAssetDocumentDerivedStateRebuilder> MakePhysicsRebuilder();

// ---- open-editor apply (AssetDocumentPhysicsOpenEditor.cpp) --------------------------------------------------------

TSharedRef<IAssetDocumentOpenEditorHandler> MakeOpenEditorHandler();

// ---- handlers ------------------------------------------------------------------------------------------------------

bool HandleRead(FCall& Call, FObjectBuilder& Result);
bool HandleStatus(FCall& Call, FObjectBuilder& Result);
bool HandleEdit(FCall& Call, FObjectBuilder& Result);

// Pages records under the runtime value budget: {total, next_offset, records}.
void AddRecordPage(TArray<FValue>& Records, int64 Offset, FObjectBuilder& Result);
} // namespace UE::AssetDocument::Graph::Physics
