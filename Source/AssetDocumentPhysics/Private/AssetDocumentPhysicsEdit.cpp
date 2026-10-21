// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// physics.asset.edit.v1: edits of a Physics Asset as the Physics Asset Editor makes them (design §4.1). Structural
// edits (bodies, constraints) give the editor's results (AssetDocumentPhysicsEditorEquivalent.cpp); Details edits
// write the reflected values with the Editor typed value codec inside Modify / PostEditChangeProperty as the Details
// panel does; profile lists go through UPhysicsAsset::PostEditChangeProperty (SanitizeProfilesHelper renames, removes
// and keeps the per-body / per-constraint entries in step); the collision table only through Enable/DisableCollision.
// Every op is checked before anything is written, so a refused op names its reason and the editor's own dialogs
// (UPhysicsAsset::SetPreviewMesh's "missing bone" message) never open.
//
// Args {ops: [...]}, each {op, ...}:
//   set_mesh {mesh}                               preview mesh ("" clears); every body's bone must be in the mesh
//   profiles.rename {kind, from, to}              asset Details: edit one entry of the list (renames it everywhere)
//   profiles.set {kind, names}                    asset Details: the whole list (entries not listed go everywhere)
//                                                 kind: "constraint" (ConstraintProfiles) | "physical_animation"
//   generate {params?}                            the editor's generation of a new asset ("Create Physics Asset" after its
//                                                 dialog: FPhysicsAssetUtils::CreateFromSkeletalMesh, not assigned to the
//                                                 mesh) on an asset without bodies and constraints; params: sparse
//                                                 FPhysAssetCreateParams over the project's generation settings
//   bodies.add {bones, fit?}                      "Add Body" per bone, parents first (the editor's Add Body with the
//                                                 project's generation settings); fit {"<bone>": params}: that body is
//                                                 fitted from the mesh as the editor's Add Body does (params sparse over
//                                                 the generation settings); without fit the shapes are written by body.shapes
//   body.remove {bone}                            "Delete Body" (constraint fix-up to the nearest bodies below)
//   body.set {bone, properties}                   body Details (complete sparse over a new body's values)
//   body.shapes {bone, shapes}                    AggGeom: each authored type listed is complete; a type not listed is
//                                                 emptied (level sets / skinned triangle meshes are kept)
//   body.physical_animation {bone, profiles}      the body's physical animation profile memberships and data
//   constraint.add {joint, child, parent}         "Create Constraint" with the joint name (snapped, collision off)
//   constraint.remove {joint}                     "Delete Constraint"
//   constraint.rename {joint, to}                 JointName
//   constraint.bones {joint, child, parent}       constraint Details ConstraintBone1 / ConstraintBone2
//   constraint.frames {joint, frames?}            frames {Child: {Position, PriAxis, SecAxis}, Parent: {...}}; omitted:
//                                                 the editor's "Snap" (SnapTransformsToDefault)
//   constraint.set {joint, properties}            constraint Details (complete sparse; ProfileInstance is the default
//                                                 profile: UpdateProfileInstance with no current profile)
//   constraint.profiles {joint, profiles}         the constraint's profile memberships and data
//   collision.set {pairs, enabled}                "Enable / Disable Collision" between bone pairs
// Result {changed}.

#include "AssetDocumentEditorNonInteractiveScope.h"
#include "AssetDocumentNameSuggestions.h"
#include "AssetDocumentEditorPropertyValueCodec.h"
#include "AssetDocumentPhysicsPrimitives.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "MeshUtilitiesCommon.h"
#include "MeshUtilitiesEngine.h"
#include "PhysicsAssetGenerationSettings.h"
#include "PhysicsAssetUtils.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "PhysicsEngine/ConvexElem.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "ReferenceSkeleton.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "AssetDocumentPhysicsEdit"

namespace UE::AssetDocument::Graph::Physics
{
namespace
{
struct FEdit
{
	UPhysicsAsset& Asset;
	bool bChanged = false;
};

// ---- lookups -------------------------------------------------------------------------------------------------------

bool ReadName(FReader& Op, const TCHAR* Key, FName& Out, bool bAllowEmpty = false)
{
	FString Text;
	if (!Op.String(Key, Text))
	{
		return false;
	}
	if (Text.IsEmpty())
	{
		if (!bAllowEmpty)
		{
			return Op.Fail(TEXT("PhysicsAsset.InvalidName"), FString::Printf(TEXT("%s is a non-empty name."), Key), Key);
		}
		Out = NAME_None;
		return true;
	}
	if (Text.Len() >= NAME_SIZE || Text.Equals(TEXT("None"), ESearchCase::IgnoreCase))
	{
		return Op.Fail(TEXT("PhysicsAsset.InvalidName"), FString::Printf(TEXT("'%s' is not a valid name."), *Text), Key);
	}
	Out = FName(*Text);
	return true;
}

int32 FindBody(FEdit& Edit, FReader& Op, const TCHAR* Key, FName& OutBone)
{
	if (!ReadName(Op, Key, OutBone))
	{
		return INDEX_NONE;
	}
	const int32 Index = Edit.Asset.FindBodyIndex(OutBone);
	if (Index == INDEX_NONE)
	{
		Op.Fail(TEXT("PhysicsAsset.BodyNotFound"), FString::Printf(TEXT("The physics asset has no body on bone '%s'."), *OutBone.ToString()), Key);
	}
	return Index;
}

// The constraint a document key names (joint name; "<name>~N" is the Nth constraint with that joint name).
int32 FindConstraintByKey(const UPhysicsAsset& Asset, const FString& Key)
{
	const TArray<FString> Keys = ConstraintKeys(Asset);
	return Keys.IndexOfByKey(Key);
}

UPhysicsConstraintTemplate* FindConstraint(FEdit& Edit, FReader& Op, const TCHAR* Field, FString& OutKey)
{
	if (!Op.String(Field, OutKey))
	{
		return nullptr;
	}
	const int32 Index = FindConstraintByKey(Edit.Asset, OutKey);
	if (Index == INDEX_NONE || Edit.Asset.ConstraintSetup[Index] == nullptr)
	{
		Op.Fail(TEXT("PhysicsAsset.ConstraintNotFound"), FString::Printf(TEXT("The physics asset has no constraint '%s'."), *OutKey), Field);
		return nullptr;
	}
	return Edit.Asset.ConstraintSetup[Index];
}

FProperty* AssetProperty(const TCHAR* Name)
{
	return FindFProperty<FProperty>(UPhysicsAsset::StaticClass(), Name);
}

void PostEdit(UObject& Object, FProperty* Property)
{
	FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
	Object.PostEditChangeProperty(Event);
}

// ---- preview mesh --------------------------------------------------------------------------------------------------

bool OpSetMesh(FEdit& Edit, FReader& Op)
{
	FString Path;
	if (!Op.String(TEXT("mesh"), Path))
	{
		return false;
	}
	USkeletalMesh* Mesh = nullptr;
	if (!Path.IsEmpty())
	{
		Mesh = LoadObject<USkeletalMesh>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (Mesh == nullptr)
		{
			return Op.Fail(TEXT("PhysicsAsset.UnknownMesh"), FString::Printf(TEXT("'%s' is not a Skeletal Mesh."), *Path), TEXT("mesh"));
		}
		// UPhysicsAsset::SetPreviewMesh opens a message box and keeps the old mesh when a body's bone is missing.
		TArray<FString> Missing;
		for (const USkeletalBodySetup* Body : Edit.Asset.SkeletalBodySetups)
		{
			if (Body != nullptr && Mesh->GetRefSkeleton().FindBoneIndex(Body->BoneName) == INDEX_NONE)
			{
				Missing.Add(Body->BoneName.ToString());
			}
		}
		if (!Missing.IsEmpty())
		{
			return Op.Fail(TEXT("PhysicsAsset.IncompatibleMesh"), FString::Printf(TEXT("%s is missing bone%s %s needed by this physics asset "
				"(the editor refuses the mesh)."), *Mesh->GetPathName(), Missing.Num() == 1 ? TEXT("") : TEXT("s"),
				*FString::Join(Missing, TEXT(", "))), TEXT("mesh"));
		}
	}
	if (Edit.Asset.GetPreviewMesh() == Mesh)
	{
		return true;
	}
	Edit.Asset.SetPreviewMesh(Mesh, true);
	Edit.bChanged = true;
	return true;
}

// ---- profile lists -------------------------------------------------------------------------------------------------

bool ReadProfileKind(FReader& Op, FProperty*& OutProperty, TArray<FName>*& OutList, UPhysicsAsset& Asset)
{
	FString Kind;
	if (!Op.String(TEXT("kind"), Kind))
	{
		return false;
	}
	if (Kind == TEXT("constraint"))
	{
		OutProperty = AssetProperty(TEXT("ConstraintProfiles"));
		OutList = &Asset.ConstraintProfiles;
	}
	else if (Kind == TEXT("physical_animation"))
	{
		OutProperty = AssetProperty(TEXT("PhysicalAnimationProfiles"));
		OutList = &Asset.PhysicalAnimationProfiles;
	}
	else
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("kind is constraint or physical_animation."), TEXT("kind"));
	}
	return OutProperty != nullptr;
}

bool OpProfilesRename(FEdit& Edit, FReader& Op)
{
	FProperty* Property = nullptr;
	TArray<FName>* List = nullptr;
	FName From;
	FName To;
	if (!ReadProfileKind(Op, Property, List, Edit.Asset) || !ReadName(Op, TEXT("from"), From) || !ReadName(Op, TEXT("to"), To))
	{
		return false;
	}
	const int32 Index = List->IndexOfByKey(From);
	if (Index == INDEX_NONE)
	{
		return Op.Fail(TEXT("PhysicsAsset.UnknownProfile"), FString::Printf(TEXT("The asset lists no profile '%s'."), *From.ToString()), TEXT("from"));
	}
	if (List->Contains(To))
	{
		return Op.Fail(TEXT("PhysicsAsset.NameTaken"), FString::Printf(TEXT("The asset already lists a profile '%s'."), *To.ToString()), TEXT("to"));
	}
	// The Details panel's edit of one array entry: PreEditChange keeps the old list, the event names the entry, and
	// SanitizeProfilesHelper renames the profile on every body / constraint.
	Edit.Asset.PreEditChange(Property);
	Edit.Asset.Modify();
	(*List)[Index] = To;
	FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
	TArray<TMap<FString, int32>> Indices;
	Indices.AddDefaulted();
	Indices[0].Add(Property->GetName(), Index);
	Event.SetArrayIndexPerObject(Indices);
	Event.ObjectIteratorIndex = 0;
	Edit.Asset.PostEditChangeProperty(Event);
	Edit.bChanged = true;
	return true;
}

bool OpProfilesSet(FEdit& Edit, FReader& Op)
{
	FProperty* Property = nullptr;
	TArray<FName>* List = nullptr;
	const FValue* Names = nullptr;
	if (!ReadProfileKind(Op, Property, List, Edit.Asset) || !Op.Array(TEXT("names"), Names))
	{
		return false;
	}
	TArray<FName> Wanted;
	for (int32 Index = 0; Index < Names->Elements.Num(); ++Index)
	{
		const FValue& Name = Names->Elements[Index];
		const FString Key = FString::Printf(TEXT("names/%d"), Index);
		if (Name.Kind != EAssetDocumentRuntimeValueKind::String || Name.StringValue.IsEmpty() || Name.StringValue.Equals(TEXT("None"), ESearchCase::IgnoreCase))
		{
			return Op.Fail(TEXT("PhysicsAsset.InvalidName"), TEXT("A profile name is a non-empty name."), Key);
		}
		const FName Profile(*Name.StringValue);
		if (Wanted.Contains(Profile))
		{
			return Op.Fail(TEXT("PhysicsAsset.NameTaken"), FString::Printf(TEXT("Profile '%s' is listed twice."), *Name.StringValue), Key);
		}
		Wanted.Add(Profile);
	}
	if (*List == Wanted)
	{
		return true;
	}
	// A whole-list Details edit: no entry index, so SanitizeProfilesHelper only drops entries not in the list.
	Edit.Asset.PreEditChange(Property);
	Edit.Asset.Modify();
	*List = Wanted;
	PostEdit(Edit.Asset, Property);
	Edit.bChanged = true;
	return true;
}

// ---- bodies --------------------------------------------------------------------------------------------------------

// Generation / fit settings: the project's generation settings (UPhysicsAssetGenerationSettings, what the editor's
// dialog shows and Add Body uses) with the document's sparse FPhysAssetCreateParams fields over them.
bool DecodeCreateParams(FReader& Op, const FString& Path, const FValue* Params, const USkeletalMesh& Mesh, FPhysAssetCreateParams& Out)
{
	const FPhysAssetCreateParams& Project = GetDefault<UPhysicsAssetGenerationSettings>()->CreateParams;
	Out = Project;
	if (Params != nullptr)
	{
		const UScriptStruct* Struct = GetCreateParamsStruct();
		if (Struct == nullptr)
		{
			return Op.Fail(TEXT("PhysicsAsset.EditFailed"), TEXT("FPhysAssetCreateParams is not reflected."), Path);
		}
		if (!DecodeSparse(Op, Path, *Params, *Struct, &Out, &Project, nullptr, [](const FProperty&) { return false; }))
		{
			return false;
		}
	}
	if (Out.LodIndex < 0 || Out.LodIndex >= Mesh.GetLODNum())
	{
		return Op.Fail(TEXT("PhysicsAsset.InvalidValue"), FString::Printf(TEXT("LodIndex %d: %s has LODs 0..%d."), Out.LodIndex,
			*Mesh.GetPathName(), Mesh.GetLODNum() - 1), Path + TEXT("/LodIndex"));
	}
	return true;
}

bool OpGenerate(FEdit& Edit, FReader& Op)
{
	const FValue* Params = nullptr;
	if (!Op.Object(TEXT("params"), Params, false))
	{
		return false;
	}
	USkeletalMesh* Mesh = Edit.Asset.GetPreviewMesh();
	if (Mesh == nullptr)
	{
		return Op.Fail(TEXT("PhysicsAsset.PreviewMeshRequired"), TEXT("Generating bodies needs the preview mesh, as in the editor."), TEXT("params"));
	}
	if (!Edit.Asset.SkeletalBodySetups.IsEmpty() || !Edit.Asset.ConstraintSetup.IsEmpty())
	{
		return Op.Fail(TEXT("PhysicsAsset.GenerationNeedsEmptyAsset"), TEXT("Generation makes the bodies of a new physics asset; this one has "
			"bodies or constraints already."), TEXT("params"));
	}
	FPhysAssetCreateParams CreateParams;
	if (!DecodeCreateParams(Op, TEXT("params"), Params, *Mesh, CreateParams))
	{
		return false;
	}
	if (Mesh->GetResourceForRendering() == nullptr)
	{
		return Op.Fail(TEXT("PhysicsAsset.GenerationFailed"), FString::Printf(TEXT("%s has no render data to fit bodies to."), *Mesh->GetPathName()),
			TEXT("params"));
	}
	// UPhysicsAssetFactory::CreatePhysicsAssetFromMesh after its dialog's OK (bSetToMesh = false: design §8 decision 2).
	Edit.Asset.Modify();
	FText Error;
	if (!FPhysicsAssetUtils::CreateFromSkeletalMesh(&Edit.Asset, Mesh, CreateParams, Error, false, false))
	{
		return Op.Fail(TEXT("PhysicsAsset.GenerationFailed"), Error.ToString(), TEXT("params"));
	}
	Edit.bChanged = true;
	return true;
}

bool OpBodiesAdd(FEdit& Edit, FReader& Op)
{
	const FValue* Bones = nullptr;
	const FValue* Fits = nullptr;
	if (!Op.Array(TEXT("bones"), Bones) || !Op.Object(TEXT("fit"), Fits, false))
	{
		return false;
	}
	USkeletalMesh* Mesh = Edit.Asset.GetPreviewMesh();
	if (Mesh == nullptr)
	{
		return Op.Fail(TEXT("PhysicsAsset.PreviewMeshRequired"), TEXT("Adding a body needs the preview mesh (the bone hierarchy decides its "
			"constraints), as in the editor."), TEXT("bones"));
	}
	const FReferenceSkeleton& Skeleton = Mesh->GetRefSkeleton();
	TArray<TPair<int32, FName>> Ordered;
	for (int32 Index = 0; Index < Bones->Elements.Num(); ++Index)
	{
		const FValue& Value = Bones->Elements[Index];
		const FString Key = FString::Printf(TEXT("bones/%d"), Index);
		const FName Bone = Value.Kind == EAssetDocumentRuntimeValueKind::String ? FName(*Value.StringValue) : NAME_None;
		const int32 BoneIndex = Bone.IsNone() ? INDEX_NONE : Skeleton.FindBoneIndex(Bone);
		if (BoneIndex == INDEX_NONE)
		{
			TArray<FString> BoneNames;
			for (const FMeshBoneInfo& Info : Skeleton.GetRefBoneInfo())
			{
				BoneNames.Add(Info.Name.ToString());
			}
			return Op.Fail(TEXT("PhysicsAsset.BoneNotFound"), FString::Printf(TEXT("%s has no bone '%s'.%s"), *Mesh->GetPathName(),
				*Value.StringValue, *UE::AssetDocument::DescribeNameCandidates(Value.StringValue, BoneNames, TEXT("Bones"))), Key);
		}
		if (Edit.Asset.FindBodyIndex(Bone) != INDEX_NONE || Ordered.ContainsByPredicate([Bone](const TPair<int32, FName>& Row) { return Row.Value == Bone; }))
		{
			return Op.Fail(TEXT("PhysicsAsset.BodyExists"), FString::Printf(TEXT("Bone '%s' already has a body."), *Bone.ToString()), Key);
		}
		Ordered.Emplace(BoneIndex, Bone);
	}
	// Fitted bodies: the fit's settings per bone.
	TMap<FName, FPhysAssetCreateParams> FitParams;
	for (int32 Entry = 0; Fits != nullptr && Entry < Fits->ObjectKeys.Num(); ++Entry)
	{
		const FName Bone(*Fits->ObjectKeys[Entry]);
		const FString Path = FString::Printf(TEXT("fit/%s"), *Escape(Fits->ObjectKeys[Entry]));
		if (!Ordered.ContainsByPredicate([Bone](const TPair<int32, FName>& Row) { return Row.Value == Bone; }))
		{
			return Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("fit names '%s', which is not one of the bones added."),
				*Fits->ObjectKeys[Entry]), Path);
		}
		if (!DecodeCreateParams(Op, Path, &Fits->Elements[Entry], *Mesh, FitParams.Add(Bone)))
		{
			return false;
		}
	}
	// Parents before children: the editor's constraint fix-up then sees what it sees when a user adds bodies root-down.
	Ordered.StableSort([](const TPair<int32, FName>& A, const TPair<int32, FName>& B) { return A.Key < B.Key; });
	const FPhysAssetCreateParams& Settings = GetDefault<UPhysicsAssetGenerationSettings>()->CreateParams;
	// The bone vertex infos the editor computes for its fits (FPhysicsAssetEditorSharedData::Initialize), per vertex
	// weighting and LOD.
	TMap<TPair<bool, int32>, TArray<FBoneVertInfo>> VertexInfos;
	TArray<int32> BoneIndicesInCreation;
	for (const TPair<int32, FName>& Row : Ordered)
	{
		BoneIndicesInCreation.Add(Row.Key);
	}
	for (const TPair<int32, FName>& Row : Ordered)
	{
		const FPhysAssetCreateParams* Fit = FitParams.Find(Row.Value);
		const FBoneVertInfo* FitInfo = nullptr;
		if (Fit != nullptr)
		{
			const bool bDominant = Fit->VertWeight == EVW_DominantWeight;
			TArray<FBoneVertInfo>& Infos = VertexInfos.FindOrAdd(TPair<bool, int32>(bDominant, Fit->LodIndex));
			if (Infos.IsEmpty())
			{
				FMeshUtilitiesEngine::CalcBoneVertInfos(Mesh, Infos, bDominant, Fit->LodIndex);
			}
			if (!Infos.IsValidIndex(Row.Key))
			{
				return Op.Fail(TEXT("PhysicsAsset.FitFailed"), FString::Printf(TEXT("%s has no vertex data for bone '%s'."), *Mesh->GetPathName(),
					*Row.Value.ToString()), FString::Printf(TEXT("fit/%s"), *Escape(Row.Value.ToString())));
			}
			FitInfo = &Infos[Row.Key];
		}
		if (EditorEquivalent::AddBody(Edit.Asset, *Mesh, Row.Value, Fit != nullptr ? *Fit : Settings, FitInfo, BoneIndicesInCreation) == INDEX_NONE)
		{
			if (Fit != nullptr)
			{
				return Op.Fail(TEXT("PhysicsAsset.FitFailed"), FString::Printf(TEXT("No shape could be fitted to bone '%s' (too few vertices "
					"weighted to it; the editor makes no body then): give Shapes, or another GeomType / VertWeight."), *Row.Value.ToString()),
					FString::Printf(TEXT("fit/%s"), *Escape(Row.Value.ToString())));
			}
			return Op.Fail(TEXT("PhysicsAsset.EditFailed"), FString::Printf(TEXT("No body was made on '%s'."), *Row.Value.ToString()), TEXT("bones"));
		}
		Edit.bChanged = true;
	}
	return true;
}

bool OpBodyRemove(FEdit& Edit, FReader& Op)
{
	FName Bone;
	const int32 Index = FindBody(Edit, Op, TEXT("bone"), Bone);
	if (Index == INDEX_NONE)
	{
		return false;
	}
	EditorEquivalent::RemoveBody(Edit.Asset, Index);
	Edit.bChanged = true;
	return true;
}

bool OpBodySet(FEdit& Edit, FReader& Op)
{
	FName Bone;
	const int32 Index = FindBody(Edit, Op, TEXT("bone"), Bone);
	const FValue* Properties = nullptr;
	if (Index == INDEX_NONE || !Op.Object(TEXT("properties"), Properties))
	{
		return false;
	}
	USkeletalBodySetup& Body = *Edit.Asset.SkeletalBodySetups[Index];
	const USkeletalBodySetup* Defaults = MakeNewBodyDefaults();
	// The collision profile the body ends with decides which collision fields are derived from it.
	FName Profile = Defaults->DefaultInstance.GetCollisionProfileName();
	if (const FValue* Instance = FindField(*Properties, TEXT("DefaultInstance")))
	{
		const FValue* Name = Instance->Kind == EAssetDocumentRuntimeValueKind::Object ? FindField(*Instance, TEXT("CollisionProfileName")) : nullptr;
		if (Name != nullptr && Name->Kind == EAssetDocumentRuntimeValueKind::String)
		{
			Profile = FName(*Name->StringValue);
		}
	}
	auto Skip = [Profile](const FProperty& Property) { return IsBodyOwnedField(Property) || IsDerivedCollisionField(Property, Profile); };
	Body.Modify();
	if (!DecodeSparse(Op, TEXT("properties"), *Properties, *USkeletalBodySetup::StaticClass(), &Body, Defaults, &Body, Skip))
	{
		return false;
	}
	// What load does with the instance (collision profile -> channels, response array -> container), as the Details
	// panel's collision customization keeps them.
	Body.DefaultInstance.FixupData(&Body);
	PostEdit(Body, FindFProperty<FProperty>(UBodySetup::StaticClass(), TEXT("DefaultInstance")));
	Edit.bChanged = true;
	return true;
}

// One element of a shape array from its document form (Name omitted: the editor's auto name; convex: VertexData and
// Transform), starting from a new element.
bool DecodeShape(FReader& Op, const FString& Path, const FShapeType& Type, const UScriptStruct& Struct, const FValue& Element,
	void* Memory, FName Bone, UObject& Owner)
{
	if (Element.Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("A shape is an object of its fields."), Path);
	}
	const bool bConvex = FStringView(Type.Array) == TEXT("ConvexElems");
	FValue Fields = MakeEmptyObject();
	const FValue* Vertices = nullptr;
	const FValue* Transform = nullptr;
	for (int32 Index = 0; Index < Element.ObjectKeys.Num(); ++Index)
	{
		const FString& Key = Element.ObjectKeys[Index];
		if (bConvex && Key == TEXT("VertexData"))
		{
			Vertices = &Element.Elements[Index];
		}
		else if (bConvex && Key == TEXT("Transform"))
		{
			Transform = &Element.Elements[Index];
		}
		else
		{
			Fields.ObjectKeys.Add(Key);
			Fields.Elements.Add(Element.Elements[Index]);
		}
	}
	FStructInstance Defaults(&Struct);
	if (!DecodeSparse(Op, Path, Fields, Struct, Memory, Defaults.Get(), &Owner, [](const FProperty&) { return false; }))
	{
		return false;
	}
	FName* Name = Struct.FindPropertyByName(TEXT("Name"))->ContainerPtrToValuePtr<FName>(Memory);
	if (FindField(Fields, TEXT("Name")) == nullptr)
	{
		*Name = AutoShapeName(Bone, Type);
	}
	if (bConvex)
	{
		FKConvexElem& Convex = *static_cast<FKConvexElem*>(Memory);
		if (Vertices == nullptr || Vertices->Kind != EAssetDocumentRuntimeValueKind::Array || Vertices->Elements.Num() < 4)
		{
			return Op.Fail(TEXT("PhysicsAsset.InvalidValue"), TEXT("A convex shape needs VertexData: at least four points {X, Y, Z} of its hull."),
				Path + TEXT("/VertexData"));
		}
		const FArrayProperty* VertexProperty = GetConvexVertexProperty();
		FString Code;
		if (VertexProperty == nullptr || !UE::AssetDocument::EditorPropertyValueCodec::Decode(const_cast<FArrayProperty&>(*VertexProperty), *Vertices,
			&Convex.VertexData, Code, &Owner))
		{
			return Op.Fail(TEXT("PhysicsAsset.InvalidValue"), FString::Printf(TEXT("VertexData is an array of points {X, Y, Z} (%s)."), *Code),
				Path + TEXT("/VertexData"));
		}
		if (Transform != nullptr)
		{
			const FProperty* TransformProperty = Struct.FindPropertyByName(TEXT("Transform"));
			FTransform Value = FTransform::Identity;
			if (TransformProperty == nullptr || !UE::AssetDocument::EditorPropertyValueCodec::Decode(const_cast<FProperty&>(*TransformProperty), *Transform,
				&Value, Code, &Owner))
			{
				return Op.Fail(TEXT("PhysicsAsset.InvalidValue"), FString::Printf(TEXT("Transform does not fit (%s)."), *Code), Path + TEXT("/Transform"));
			}
			Convex.SetTransform(Value);
		}
		Convex.UpdateElemBox();
	}
	return true;
}

bool OpBodyShapes(FEdit& Edit, FReader& Op)
{
	FName Bone;
	const int32 Index = FindBody(Edit, Op, TEXT("bone"), Bone);
	const FValue* Shapes = nullptr;
	if (Index == INDEX_NONE || !Op.Object(TEXT("shapes"), Shapes))
	{
		return false;
	}
	USkeletalBodySetup& Body = *Edit.Asset.SkeletalBodySetups[Index];
	for (const FString& Key : Shapes->ObjectKeys)
	{
		const FShapeType* Type = GetShapeTypes().FindByPredicate([&Key](const FShapeType& Candidate) { return Key == Candidate.Key; });
		if (Type == nullptr)
		{
			TArray<FString> Keys;
			for (const FShapeType& Candidate : GetShapeTypes())
			{
				if (Candidate.bAuthored)
				{
					Keys.Add(Candidate.Key);
				}
			}
			return Op.Fail(TEXT("Graph.UnknownField"), FString::Printf(TEXT("'%s' is not a shape type. Types: %s."), *Key, *FString::Join(Keys, TEXT(", "))),
				FString::Printf(TEXT("shapes/%s"), *Escape(Key)));
		}
		if (!Type->bAuthored)
		{
			return Op.Fail(TEXT("PhysicsAsset.ShapeTypeNotAuthored"), FString::Printf(TEXT("%s are experimental shapes the editor only makes "
				"through its fit tools; they are kept as they are and cannot be written."), *Key), FString::Printf(TEXT("shapes/%s"), *Escape(Key)));
		}
	}
	bool bChanged = false;
	bool bConvexChanged = false;
	for (const FShapeType& Type : GetShapeTypes())
	{
		if (!Type.bAuthored)
		{
			continue;
		}
		const FArrayProperty* ArrayProperty = GetShapeArrayProperty(Type);
		const FStructProperty* Inner = ArrayProperty != nullptr ? CastField<FStructProperty>(ArrayProperty->Inner) : nullptr;
		if (Inner == nullptr)
		{
			return Op.Fail(TEXT("PhysicsAsset.EditFailed"), FString::Printf(TEXT("FKAggregateGeom has no %s."), Type.Array));
		}
		const FValue* Elements = FindField(*Shapes, Type.Key);
		const FString Path = FString::Printf(TEXT("shapes/%s"), Type.Key);
		if (Elements != nullptr && Elements->Kind != EAssetDocumentRuntimeValueKind::Array)
		{
			return Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("%s is an array of shapes."), Type.Key), Path);
		}
		void* Current = ArrayProperty->ContainerPtrToValuePtr<void>(&Body.AggGeom);
		FPropertyValue Wanted(*ArrayProperty);
		FScriptArrayHelper WantedElements(ArrayProperty, Wanted.Memory);
		FScriptArrayHelper CurrentElements(ArrayProperty, Current);
		const int32 Count = Elements != nullptr ? Elements->Elements.Num() : 0;
		WantedElements.AddValues(Count);
		for (int32 Element = 0; Element < Count; ++Element)
		{
			if (!DecodeShape(Op, FString::Printf(TEXT("%s/%d"), *Path, Element), Type, *Inner->Struct, Elements->Elements[Element],
				WantedElements.GetRawPtr(Element), Body.BoneName, Body))
			{
				return false;
			}
			if (FStringView(Type.Array) == TEXT("ConvexElems") && Element < CurrentElements.Num())
			{
				// An unchanged hull keeps its cooked indices (IndexData is derived from VertexData when cooked).
				FKConvexElem& New = *reinterpret_cast<FKConvexElem*>(WantedElements.GetRawPtr(Element));
				const FKConvexElem& Old = *reinterpret_cast<const FKConvexElem*>(CurrentElements.GetRawPtr(Element));
				if (New.VertexData == Old.VertexData)
				{
					New.IndexData = Old.IndexData;
				}
			}
		}
		if (ArrayProperty->Identical(Current, Wanted.Memory, PPF_None))
		{
			continue;
		}
		if (!bChanged)
		{
			Body.Modify();
			Body.PreEditChange(FindFProperty<FProperty>(UBodySetup::StaticClass(), TEXT("AggGeom")));
		}
		ArrayProperty->CopyCompleteValue(Current, Wanted.Memory);
		bChanged = true;
		bConvexChanged |= FStringView(Type.Array) == TEXT("ConvexElems");
	}
	if (!bChanged)
	{
		return true;
	}
	if (bConvexChanged)
	{
		// What the editor does after a convex edit (FPhysicsAssetEditor duplicate / delete primitive, static mesh import).
		Body.InvalidatePhysicsData();
		Body.CreatePhysicsMeshes();
	}
	PostEdit(Body, FindFProperty<FProperty>(UBodySetup::StaticClass(), TEXT("AggGeom")));
	Edit.bChanged = true;
	return true;
}

bool OpBodyPhysicalAnimation(FEdit& Edit, FReader& Op)
{
	FName Bone;
	const int32 Index = FindBody(Edit, Op, TEXT("bone"), Bone);
	const FValue* Profiles = nullptr;
	if (Index == INDEX_NONE || !Op.Object(TEXT("profiles"), Profiles))
	{
		return false;
	}
	const UScriptStruct* DataStruct = GetPhysicalAnimationDataStruct();
	if (DataStruct == nullptr)
	{
		return Op.Fail(TEXT("PhysicsAsset.EditFailed"), TEXT("FPhysicalAnimationData is not reflected."));
	}
	USkeletalBodySetup& Body = *Edit.Asset.SkeletalBodySetups[Index];
	for (const FString& Name : Profiles->ObjectKeys)
	{
		if (!Edit.Asset.PhysicalAnimationProfiles.Contains(FName(*Name)))
		{
			return Op.Fail(TEXT("PhysicsAsset.UnknownProfile"), FString::Printf(TEXT("The asset lists no physical animation profile '%s' "
				"(Properties.PhysicalAnimationProfiles)."), *Name), FString::Printf(TEXT("profiles/%s"), *Escape(Name)));
		}
	}
	Body.Modify();
	TArray<FName> Remove;
	for (const FPhysicalAnimationProfile& Profile : Body.GetPhysicalAnimationProfiles())
	{
		if (FindField(*Profiles, Profile.ProfileName.ToString()) == nullptr)
		{
			Remove.AddUnique(Profile.ProfileName);
		}
	}
	for (const FName& Name : Remove)
	{
		Body.RemovePhysicalAnimationProfile(Name);
	}
	FStructInstance Defaults(DataStruct);
	for (int32 Entry = 0; Entry < Profiles->ObjectKeys.Num(); ++Entry)
	{
		const FName Name(*Profiles->ObjectKeys[Entry]);
		if (Body.FindPhysicalAnimationProfile(Name) == nullptr)
		{
			Body.AddPhysicalAnimationProfile(Name);
		}
		FStructInstance Data(DataStruct);
		DataStruct->CopyScriptStruct(Data.Get(), &Body.FindPhysicalAnimationProfile(Name)->PhysicalAnimationData);
		if (!DecodeSparse(Op, FString::Printf(TEXT("profiles/%s"), *Escape(Profiles->ObjectKeys[Entry])), Profiles->Elements[Entry], *DataStruct,
			Data.Get(), Defaults.Get(), &Body, [](const FProperty&) { return false; }))
		{
			return false;
		}
		DataStruct->CopyScriptStruct(&Body.FindPhysicalAnimationProfile(Name)->PhysicalAnimationData, Data.Get());
	}
	PostEdit(Body, FindFProperty<FProperty>(USkeletalBodySetup::StaticClass(), TEXT("CurrentPhysicalAnimationProfile")));
	Edit.bChanged = true;
	return true;
}

// ---- constraints ---------------------------------------------------------------------------------------------------

bool ReadBodyPair(FEdit& Edit, FReader& Op, int32& OutChild, int32& OutParent)
{
	FName Child;
	FName Parent;
	OutChild = FindBody(Edit, Op, TEXT("child"), Child);
	if (OutChild == INDEX_NONE)
	{
		return false;
	}
	OutParent = FindBody(Edit, Op, TEXT("parent"), Parent);
	if (OutParent == INDEX_NONE)
	{
		return false;
	}
	if (OutChild == OutParent)
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("A constraint joins two bodies; child and parent are both '%s'."),
			*Child.ToString()), TEXT("parent"));
	}
	return true;
}

bool OpConstraintAdd(FEdit& Edit, FReader& Op)
{
	FName Joint;
	int32 Child = INDEX_NONE;
	int32 Parent = INDEX_NONE;
	if (!ReadName(Op, TEXT("joint"), Joint) || !ReadBodyPair(Edit, Op, Child, Parent))
	{
		return false;
	}
	if (Joint.ToString().Contains(TEXT("~")) || Edit.Asset.FindConstraintIndex(Joint) != INDEX_NONE)
	{
		return Op.Fail(TEXT("PhysicsAsset.NameTaken"), FString::Printf(TEXT("A constraint named '%s' exists (or the name has '~')."),
			*Joint.ToString()), TEXT("joint"));
	}
	Edit.Asset.Modify();
	if (EditorEquivalent::AddJoint(Edit.Asset, Joint, Child, Parent) == INDEX_NONE)
	{
		return Op.Fail(TEXT("PhysicsAsset.EditFailed"), TEXT("Constraints cannot be created in this project (asset class permissions)."), TEXT("joint"));
	}
	Edit.bChanged = true;
	return true;
}

bool OpConstraintRemove(FEdit& Edit, FReader& Op)
{
	FString Key;
	if (FindConstraint(Edit, Op, TEXT("joint"), Key) == nullptr)
	{
		return false;
	}
	// SharedData::DeleteCurrentConstraint.
	Edit.Asset.Modify();
	FPhysicsAssetUtils::DestroyConstraint(&Edit.Asset, FindConstraintByKey(Edit.Asset, Key));
	Edit.bChanged = true;
	return true;
}

bool OpConstraintRename(FEdit& Edit, FReader& Op)
{
	FString Key;
	FName To;
	UPhysicsConstraintTemplate* Constraint = FindConstraint(Edit, Op, TEXT("joint"), Key);
	if (Constraint == nullptr || !ReadName(Op, TEXT("to"), To))
	{
		return false;
	}
	if (To.ToString().Contains(TEXT("~")) || Edit.Asset.FindConstraintIndex(To) != INDEX_NONE)
	{
		return Op.Fail(TEXT("PhysicsAsset.NameTaken"), FString::Printf(TEXT("A constraint named '%s' exists (or the name has '~')."), *To.ToString()), TEXT("to"));
	}
	Constraint->Modify();
	Constraint->DefaultInstance.JointName = To;
	PostEdit(*Constraint, FindFProperty<FProperty>(UPhysicsConstraintTemplate::StaticClass(), TEXT("DefaultInstance")));
	Edit.bChanged = true;
	return true;
}

bool OpConstraintBones(FEdit& Edit, FReader& Op)
{
	FString Key;
	int32 Child = INDEX_NONE;
	int32 Parent = INDEX_NONE;
	UPhysicsConstraintTemplate* Constraint = FindConstraint(Edit, Op, TEXT("joint"), Key);
	if (Constraint == nullptr || !ReadBodyPair(Edit, Op, Child, Parent))
	{
		return false;
	}
	// Constraint Details: Child Bone / Parent Bone.
	Constraint->Modify();
	Constraint->DefaultInstance.ConstraintBone1 = Edit.Asset.SkeletalBodySetups[Child]->BoneName;
	Constraint->DefaultInstance.ConstraintBone2 = Edit.Asset.SkeletalBodySetups[Parent]->BoneName;
	PostEdit(*Constraint, FindFProperty<FProperty>(UPhysicsConstraintTemplate::StaticClass(), TEXT("DefaultInstance")));
	Edit.bChanged = true;
	return true;
}

bool DecodeFrame(FReader& Op, const FValue& Frames, const TCHAR* Side, const TCHAR* const Fields[3], FConstraintInstance& Instance)
{
	static const TCHAR* const Keys[] = {TEXT("Position"), TEXT("PriAxis"), TEXT("SecAxis")};
	const FString Path = FString::Printf(TEXT("frames/%s"), Side);
	const FValue* Frame = FindField(Frames, Side);
	if (Frame == nullptr || Frame->Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("Frames.%s is {Position, PriAxis, SecAxis}."), Side), Path);
	}
	for (const FString& Key : Frame->ObjectKeys)
	{
		if (Key != Keys[0] && Key != Keys[1] && Key != Keys[2])
		{
			return Op.Fail(TEXT("Graph.UnknownField"), FString::Printf(TEXT("A frame is {Position, PriAxis, SecAxis}; '%s' is not one of them."), *Key),
				Path + TEXT("/") + Escape(Key));
		}
	}
	const UScriptStruct* InstanceStruct = GetConstraintInstanceStruct();
	for (int32 Index = 0; Index < 3; ++Index)
	{
		const FValue* Value = FindField(*Frame, Keys[Index]);
		FProperty* Property = InstanceStruct != nullptr ? InstanceStruct->FindPropertyByName(Fields[Index]) : nullptr;
		FString Code;
		if (Value == nullptr || Property == nullptr
			|| !UE::AssetDocument::EditorPropertyValueCodec::Decode(*Property, *Value, Property->ContainerPtrToValuePtr<void>(&Instance), Code))
		{
			return Op.Fail(TEXT("PhysicsAsset.InvalidValue"), FString::Printf(TEXT("%s is a vector {X, Y, Z}%s."), Keys[Index],
				Code.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *Code)), Path + TEXT("/") + Keys[Index]);
		}
	}
	return true;
}

bool OpConstraintFrames(FEdit& Edit, FReader& Op)
{
	FString Key;
	UPhysicsConstraintTemplate* Constraint = FindConstraint(Edit, Op, TEXT("joint"), Key);
	if (Constraint == nullptr)
	{
		return false;
	}
	const FValue* Frames = nullptr;
	if (!Op.Object(TEXT("frames"), Frames, false))
	{
		return false;
	}
	FConstraintInstance Instance;
	Instance.ConstraintBone1 = Constraint->DefaultInstance.ConstraintBone1;
	Instance.ConstraintBone2 = Constraint->DefaultInstance.ConstraintBone2;
	if (Frames == nullptr)
	{
		if (!CanSnap(Instance, Edit.Asset))
		{
			return Op.Fail(TEXT("PhysicsAsset.FramesRequired"), FString::Printf(TEXT("The preview mesh has no bone %s or %s, so the joint "
				"cannot be snapped to the child bone: give Frames."), *Instance.ConstraintBone1.ToString(), *Instance.ConstraintBone2.ToString()),
				TEXT("frames"));
		}
		// The editor's "Snap" (SharedData::SnapConstraintToBone).
		Instance.SnapTransformsToDefault(EConstraintTransformComponentFlags::All, &Edit.Asset);
	}
	else
	{
		static const TCHAR* const ChildFields[3] = {TEXT("Pos1"), TEXT("PriAxis1"), TEXT("SecAxis1")};
		static const TCHAR* const ParentFields[3] = {TEXT("Pos2"), TEXT("PriAxis2"), TEXT("SecAxis2")};
		for (const FString& Side : Frames->ObjectKeys)
		{
			if (Side != TEXT("Child") && Side != TEXT("Parent"))
			{
				return Op.Fail(TEXT("Graph.UnknownField"), FString::Printf(TEXT("Frames is {Child, Parent}; '%s' is not one of them."), *Side),
					TEXT("frames/") + Escape(Side));
			}
		}
		if (!DecodeFrame(Op, *Frames, TEXT("Child"), ChildFields, Instance) || !DecodeFrame(Op, *Frames, TEXT("Parent"), ParentFields, Instance))
		{
			return false;
		}
	}
	Constraint->Modify();
	FConstraintInstance& Target = Constraint->DefaultInstance;
	Target.Pos1 = Instance.Pos1;
	Target.PriAxis1 = Instance.PriAxis1;
	Target.SecAxis1 = Instance.SecAxis1;
	Target.Pos2 = Instance.Pos2;
	Target.PriAxis2 = Instance.PriAxis2;
	Target.SecAxis2 = Instance.SecAxis2;
	PostEdit(*Constraint, FindFProperty<FProperty>(UPhysicsConstraintTemplate::StaticClass(), TEXT("DefaultInstance")));
	Edit.bChanged = true;
	return true;
}

bool OpConstraintSet(FEdit& Edit, FReader& Op)
{
	FString Key;
	UPhysicsConstraintTemplate* Constraint = FindConstraint(Edit, Op, TEXT("joint"), Key);
	const FValue* Properties = nullptr;
	if (Constraint == nullptr || !Op.Object(TEXT("properties"), Properties))
	{
		return false;
	}
	const UScriptStruct* InstanceStruct = GetConstraintInstanceStruct();
	const UScriptStruct* ProfileStruct = GetConstraintProfileStruct();
	if (InstanceStruct == nullptr || ProfileStruct == nullptr)
	{
		return Op.Fail(TEXT("PhysicsAsset.EditFailed"), TEXT("FConstraintInstance is not reflected."));
	}
	FStructInstance Authored(InstanceStruct);
	InstanceStruct->CopyScriptStruct(Authored.Get(), &Constraint->DefaultInstance);
	FConstraintInstance& Instance = *static_cast<FConstraintInstance*>(Authored.Get());
	ProfileStruct->CopyScriptStruct(&Instance.ProfileInstance, GetDefaultProfile(*Constraint));
	const FConstraintInstance& Defaults = GetDefault<UPhysicsConstraintTemplate>()->DefaultInstance;
	if (!DecodeSparse(Op, TEXT("properties"), *Properties, *InstanceStruct, Authored.Get(), &Defaults, Constraint,
		[](const FProperty& Property) { return IsConstraintOwnedField(Property); }))
	{
		return false;
	}
	// A Details edit of the joint: the instance, then UpdateProfileInstance (no current profile: the default profile).
	Constraint->Modify();
	InstanceStruct->CopyScriptStruct(&Constraint->DefaultInstance, Authored.Get());
	PostEdit(*Constraint, FindFProperty<FProperty>(UPhysicsConstraintTemplate::StaticClass(), TEXT("DefaultInstance")));
	Edit.bChanged = true;
	return true;
}

bool OpConstraintProfiles(FEdit& Edit, FReader& Op)
{
	FString Key;
	UPhysicsConstraintTemplate* Constraint = FindConstraint(Edit, Op, TEXT("joint"), Key);
	const FValue* Profiles = nullptr;
	if (Constraint == nullptr || !Op.Object(TEXT("profiles"), Profiles))
	{
		return false;
	}
	const UScriptStruct* ProfileStruct = GetConstraintProfileStruct();
	if (ProfileStruct == nullptr)
	{
		return Op.Fail(TEXT("PhysicsAsset.EditFailed"), TEXT("FConstraintProfileProperties is not reflected."));
	}
	for (const FString& Name : Profiles->ObjectKeys)
	{
		if (!Edit.Asset.ConstraintProfiles.Contains(FName(*Name)))
		{
			return Op.Fail(TEXT("PhysicsAsset.UnknownProfile"), FString::Printf(TEXT("The asset lists no constraint profile '%s' "
				"(Properties.ConstraintProfiles)."), *Name), FString::Printf(TEXT("profiles/%s"), *Escape(Name)));
		}
	}
	Constraint->Modify();
	TArray<FName> Remove;
	for (const FPhysicsConstraintProfileHandle& Handle : Constraint->ProfileHandles)
	{
		if (FindField(*Profiles, Handle.ProfileName.ToString()) == nullptr)
		{
			Remove.AddUnique(Handle.ProfileName);
		}
	}
	for (const FName& Name : Remove)
	{
		Constraint->RemoveConstraintProfile(Name);
	}
	for (int32 Entry = 0; Entry < Profiles->ObjectKeys.Num(); ++Entry)
	{
		const FName Name(*Profiles->ObjectKeys[Entry]);
		if (!Constraint->ContainsConstraintProfile(Name))
		{
			Constraint->AddConstraintProfile(Name);
		}
		FPhysicsConstraintProfileHandle* Handle = Constraint->ProfileHandles.FindByPredicate(
			[Name](const FPhysicsConstraintProfileHandle& Candidate) { return Candidate.ProfileName == Name; });
		FStructInstance Data(ProfileStruct);
		ProfileStruct->CopyScriptStruct(Data.Get(), &Handle->ProfileProperties);
		if (!DecodeSparse(Op, FString::Printf(TEXT("profiles/%s"), *Escape(Profiles->ObjectKeys[Entry])), Profiles->Elements[Entry], *ProfileStruct,
			Data.Get(), GetDefaultProfile(*Constraint), Constraint, [](const FProperty&) { return false; }))
		{
			return false;
		}
		// Written into the handle directly: no "current profile" switch (which would also rewrite every constraint's
		// instance through ApplyConstraintProfile).
		ProfileStruct->CopyScriptStruct(&Handle->ProfileProperties, Data.Get());
	}
	Edit.bChanged = true;
	return true;
}

// ---- collision -----------------------------------------------------------------------------------------------------

bool OpCollisionSet(FEdit& Edit, FReader& Op)
{
	const FValue* Pairs = nullptr;
	bool bEnabled = false;
	if (!Op.Array(TEXT("pairs"), Pairs) || !Op.Bool(TEXT("enabled"), bEnabled, true))
	{
		return false;
	}
	TArray<TPair<int32, int32>> Indices;
	for (int32 Index = 0; Index < Pairs->Elements.Num(); ++Index)
	{
		const FValue& Pair = Pairs->Elements[Index];
		const FString Path = FString::Printf(TEXT("pairs/%d"), Index);
		if (Pair.Kind != EAssetDocumentRuntimeValueKind::Array || Pair.Elements.Num() != 2
			|| Pair.Elements[0].Kind != EAssetDocumentRuntimeValueKind::String || Pair.Elements[1].Kind != EAssetDocumentRuntimeValueKind::String)
		{
			return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("A collision pair is [boneA, boneB]."), Path);
		}
		int32 Bodies[2];
		for (int32 Side = 0; Side < 2; ++Side)
		{
			Bodies[Side] = Edit.Asset.FindBodyIndex(FName(*Pair.Elements[Side].StringValue));
			if (Bodies[Side] == INDEX_NONE)
			{
				return Op.Fail(TEXT("PhysicsAsset.BodyNotFound"), FString::Printf(TEXT("The physics asset has no body on bone '%s'."),
					*Pair.Elements[Side].StringValue), FString::Printf(TEXT("%s/%d"), *Path, Side));
			}
		}
		if (Bodies[0] == Bodies[1])
		{
			return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("A collision pair names two different bodies."), Path);
		}
		Indices.Emplace(Bodies[0], Bodies[1]);
	}
	for (const TPair<int32, int32>& Pair : Indices)
	{
		if (Edit.Asset.IsCollisionEnabled(Pair.Key, Pair.Value) != bEnabled)
		{
			EditorEquivalent::SetPairCollision(Edit.Asset, Pair.Key, Pair.Value, bEnabled);
			Edit.bChanged = true;
		}
	}
	return true;
}

// ---- dispatch ------------------------------------------------------------------------------------------------------

bool RunOp(FEdit& Edit, FReader& Op, const FString& Kind)
{
	if (Kind == TEXT("set_mesh")) { return OpSetMesh(Edit, Op); }
	if (Kind == TEXT("profiles.rename")) { return OpProfilesRename(Edit, Op); }
	if (Kind == TEXT("profiles.set")) { return OpProfilesSet(Edit, Op); }
	if (Kind == TEXT("generate")) { return OpGenerate(Edit, Op); }
	if (Kind == TEXT("bodies.add")) { return OpBodiesAdd(Edit, Op); }
	if (Kind == TEXT("body.remove")) { return OpBodyRemove(Edit, Op); }
	if (Kind == TEXT("body.set")) { return OpBodySet(Edit, Op); }
	if (Kind == TEXT("body.shapes")) { return OpBodyShapes(Edit, Op); }
	if (Kind == TEXT("body.physical_animation")) { return OpBodyPhysicalAnimation(Edit, Op); }
	if (Kind == TEXT("constraint.add")) { return OpConstraintAdd(Edit, Op); }
	if (Kind == TEXT("constraint.remove")) { return OpConstraintRemove(Edit, Op); }
	if (Kind == TEXT("constraint.rename")) { return OpConstraintRename(Edit, Op); }
	if (Kind == TEXT("constraint.bones")) { return OpConstraintBones(Edit, Op); }
	if (Kind == TEXT("constraint.frames")) { return OpConstraintFrames(Edit, Op); }
	if (Kind == TEXT("constraint.set")) { return OpConstraintSet(Edit, Op); }
	if (Kind == TEXT("constraint.profiles")) { return OpConstraintProfiles(Edit, Op); }
	if (Kind == TEXT("collision.set")) { return OpCollisionSet(Edit, Op); }
	return Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("Unknown op '%s'."), *Kind), TEXT("op"));
}

bool CheckOpFields(FReader& Op, const FValue& Value, const FString& Kind)
{
	static const TMap<FString, TArray<FString>> Fields = {
		{TEXT("set_mesh"), {TEXT("mesh")}}, {TEXT("profiles.rename"), {TEXT("kind"), TEXT("from"), TEXT("to")}},
		{TEXT("profiles.set"), {TEXT("kind"), TEXT("names")}}, {TEXT("generate"), {TEXT("params")}}, {TEXT("bodies.add"), {TEXT("bones"), TEXT("fit")}}, {TEXT("body.remove"), {TEXT("bone")}},
		{TEXT("body.set"), {TEXT("bone"), TEXT("properties")}}, {TEXT("body.shapes"), {TEXT("bone"), TEXT("shapes")}},
		{TEXT("body.physical_animation"), {TEXT("bone"), TEXT("profiles")}},
		{TEXT("constraint.add"), {TEXT("joint"), TEXT("child"), TEXT("parent")}}, {TEXT("constraint.remove"), {TEXT("joint")}},
		{TEXT("constraint.rename"), {TEXT("joint"), TEXT("to")}}, {TEXT("constraint.bones"), {TEXT("joint"), TEXT("child"), TEXT("parent")}},
		{TEXT("constraint.frames"), {TEXT("joint"), TEXT("frames")}}, {TEXT("constraint.set"), {TEXT("joint"), TEXT("properties")}},
		{TEXT("constraint.profiles"), {TEXT("joint"), TEXT("profiles")}}, {TEXT("collision.set"), {TEXT("pairs"), TEXT("enabled")}}};
	const TArray<FString>* Allowed = Fields.Find(Kind);
	if (Allowed == nullptr)
	{
		return true; // RunOp names the unknown op
	}
	for (const FString& Key : Value.ObjectKeys)
	{
		if (Key != TEXT("op") && !Allowed->Contains(Key))
		{
			return Op.Fail(TEXT("Graph.UnknownField"), FString::Printf(TEXT("'%s' takes %s; '%s' is not one of them."), *Kind,
				*FString::Join(*Allowed, TEXT(", ")), *Key), Key);
		}
	}
	return true;
}

// The editor's "current profile" previews are transient UI state: an asset the editor left on a profile has that
// profile's values in every constraint instance. Edits always target the default profile (design §4.1), so the
// previews go back to "None" first, as choosing None in the editor's profile picker does.
void ResetCurrentProfiles(UPhysicsAsset& Asset)
{
	if (!Asset.CurrentConstraintProfileName.IsNone())
	{
		Asset.Modify();
		Asset.CurrentConstraintProfileName = NAME_None;
		for (UPhysicsConstraintTemplate* Constraint : Asset.ConstraintSetup)
		{
			if (Constraint != nullptr)
			{
				Constraint->Modify();
				Constraint->ApplyConstraintProfile(NAME_None, Constraint->DefaultInstance, true);
			}
		}
	}
	if (!Asset.CurrentPhysicalAnimationProfileName.IsNone())
	{
		Asset.Modify();
		Asset.CurrentPhysicalAnimationProfileName = NAME_None;
	}
}
} // namespace

bool HandleEdit(FCall& Call, FObjectBuilder& Result)
{
	const FValue* Ops = nullptr;
	if (!Call.Args.OnlyFields({TEXT("ops")}) || !Call.Args.Array(TEXT("ops"), Ops))
	{
		return false;
	}
	UPhysicsAsset* Asset = Cast<UPhysicsAsset>(&Call.Asset);
	if (Asset == nullptr)
	{
		return Fail(Call, TEXT("PhysicsAsset.UnsupportedAsset"), TEXT("physics.asset.edit.v1 edits Physics Assets."), TEXT("/HandleArguments/asset"));
	}
	FAssetDocumentEditorNonInteractiveScope NonInteractive;
	const FScopedTransaction Transaction(LOCTEXT("EditPhysicsAsset", "Edit Physics Asset"));
	FEdit Edit{*Asset};
	if (!Ops->Elements.IsEmpty())
	{
		ResetCurrentProfiles(*Asset);
	}
	for (int32 Index = 0; Index < Ops->Elements.Num(); ++Index)
	{
		const FString OpPath = FString::Printf(TEXT("ops/%d"), Index);
		FReader Op = Call.Args.Child(OpPath, Ops->Elements[Index]);
		FString Kind;
		if (!Op.IsObject())
		{
			return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("Each op is an object."));
		}
		if (!Op.String(TEXT("op"), Kind) || !CheckOpFields(Op, Ops->Elements[Index], Kind) || !RunOp(Edit, Op, Kind))
		{
			return false;
		}
		if (NonInteractive.WasInteractionRefused())
		{
			NonInteractive.DiscardCreatedPackages();
			return Op.Fail(TEXT("PhysicsAsset.InteractiveEditRefused"), TEXT("The edit tried to open an editor dialog, which an MCP request "
				"cannot answer; the edit was refused."));
		}
	}
	if (Edit.bChanged)
	{
		EditorEquivalent::Refresh(*Asset);
		Asset->MarkPackageDirty();
	}
	Result.AddBool(TEXT("changed"), Edit.bChanged);
	return true;
}
} // namespace UE::AssetDocument::Graph::Physics

#undef LOCTEXT_NAMESPACE
