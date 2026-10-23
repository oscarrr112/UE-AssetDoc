// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Values of the Physics Asset document (design §3): sparse Details of bodies, constraints and profile data through
// the Editor typed value codec, shapes, constraint frames, document order.

#include "AssetDocumentPhysicsPrimitives.h"

#include "AssetDocumentEditorPropertyValueCodec.h"
#include "Engine/SkeletalMesh.h"
#include "PhysicsAssetGenerationSettings.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "PhysicsEngine/ConvexElem.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "ReferenceSkeleton.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::Graph::Physics
{
namespace
{
constexpr double FrameTolerance = 1.0e-4;

const FStructProperty* FindStructProperty(const UStruct* Owner, const TCHAR* Name)
{
	return Owner != nullptr ? FindFProperty<FStructProperty>(Owner, Name) : nullptr;
}

const UScriptStruct* StructOf(const FStructProperty* Property)
{
	return Property != nullptr ? Property->Struct.Get() : nullptr;
}

// Structs compared and written member by member: engine structs (FBodyInstance, FConstraintProfileProperties,
// FLinearConstraint, ...). CoreUObject structs (vectors, rotators, colors, transforms) are one value.
bool IsMemberwiseStruct(const FProperty& Property)
{
	const FStructProperty* Struct = CastField<FStructProperty>(&Property);
	if (Struct == nullptr || Struct->Struct == nullptr || Property.ArrayDim != 1)
	{
		return false;
	}
	const UPackage* Package = Struct->Struct->GetOutermost();
	return Package != nullptr && Package->GetFName() != TEXT("/Script/CoreUObject");
}

bool EncodeSparseInto(const UStruct& Struct, const void* Value, const void* Defaults, FObjectBuilder& Object, FString& OutError,
	FSkip Skip, bool& bOutAny)
{
	for (TFieldIterator<FProperty> It(&Struct); It; ++It)
	{
		const FProperty& Property = **It;
		if (!IsEditableField(Property) || Skip(Property))
		{
			continue;
		}
		const void* ValuePtr = Property.ContainerPtrToValuePtr<void>(Value);
		const void* DefaultPtr = Defaults != nullptr ? Property.ContainerPtrToValuePtr<void>(Defaults) : nullptr;
		if (IsMemberwiseStruct(Property))
		{
			const UScriptStruct& Inner = *CastFieldChecked<FStructProperty>(&Property)->Struct;
			FObjectBuilder Child;
			bool bChild = false;
			if (!EncodeSparseInto(Inner, ValuePtr, DefaultPtr, Child, OutError, Skip, bChild))
			{
				return false;
			}
			if (bChild)
			{
				Object.Add(Property.GetName(), Child.Build());
				bOutAny = true;
			}
			continue;
		}
		if (DefaultPtr != nullptr && Property.Identical(ValuePtr, DefaultPtr, PPF_None))
		{
			continue;
		}
		FValue Field;
		if (!EncodeValue(Property, ValuePtr, Field, OutError))
		{
			return false;
		}
		Object.Add(Property.GetName(), MoveTemp(Field));
		bOutAny = true;
	}
	return true;
}

TArray<FString> EditableNames(const UStruct& Struct, FSkip Skip)
{
	TArray<FString> Names;
	for (TFieldIterator<FProperty> It(&Struct); It; ++It)
	{
		if (IsEditableField(**It) && !Skip(**It))
		{
			Names.Add(It->GetName());
		}
	}
	return Names;
}

void ResetEditable(const UStruct& Struct, void* Memory, const void* Defaults, FSkip Skip)
{
	for (TFieldIterator<FProperty> It(&Struct); It; ++It)
	{
		const FProperty& Property = **It;
		if (!IsEditableField(Property) || Skip(Property))
		{
			continue;
		}
		void* ValuePtr = Property.ContainerPtrToValuePtr<void>(Memory);
		const void* DefaultPtr = Property.ContainerPtrToValuePtr<void>(Defaults);
		if (IsMemberwiseStruct(Property))
		{
			ResetEditable(*CastFieldChecked<FStructProperty>(&Property)->Struct, ValuePtr, DefaultPtr, Skip);
		}
		else
		{
			Property.CopyCompleteValue(ValuePtr, DefaultPtr);
		}
	}
}

const FShapeType ShapeTypes[] = {
	{TEXT("Spheres"), TEXT("SphereElems"), TEXT("sphere"), true},
	{TEXT("Boxes"), TEXT("BoxElems"), TEXT("box"), true},
	{TEXT("Capsules"), TEXT("SphylElems"), TEXT("capsule"), true},
	{TEXT("TaperedCapsules"), TEXT("TaperedCapsuleElems"), TEXT("tapered_capsule"), true},
	{TEXT("Convexes"), TEXT("ConvexElems"), TEXT("convex"), true},
	{TEXT("LevelSets"), TEXT("LevelSetElems"), TEXT("level_set"), false},
	{TEXT("SkinnedLevelSets"), TEXT("SkinnedLevelSetElems"), TEXT("skinned_level_set"), false},
	{TEXT("MLLevelSets"), TEXT("MLLevelSetElems"), TEXT("ml_level_set"), false},
	{TEXT("SkinnedTriangleMeshes"), TEXT("SkinnedTriangleMeshElems"), TEXT("skinned_triangle_mesh"), false},
};

int32 BoneOrderIndex(const FReferenceSkeleton* Skeleton, FName Bone)
{
	const int32 Index = Skeleton != nullptr ? Skeleton->FindBoneIndex(Bone) : INDEX_NONE;
	return Index != INDEX_NONE ? Index : MAX_int32;
}
} // namespace

// ---- reflected types -----------------------------------------------------------------------------------------------

const UScriptStruct* GetAggGeomStruct()
{
	return StructOf(FindStructProperty(UBodySetup::StaticClass(), TEXT("AggGeom")));
}

const UScriptStruct* GetConstraintInstanceStruct()
{
	return StructOf(FindStructProperty(UPhysicsConstraintTemplate::StaticClass(), TEXT("DefaultInstance")));
}

const UScriptStruct* GetConstraintProfileStruct()
{
	return StructOf(FindStructProperty(GetConstraintInstanceStruct(), TEXT("ProfileInstance")));
}

const UScriptStruct* GetPhysicalAnimationDataStruct()
{
	const UScriptStruct* Profile = StructOf(FindStructProperty(USkeletalBodySetup::StaticClass(), TEXT("CurrentPhysicalAnimationProfile")));
	return StructOf(FindStructProperty(Profile, TEXT("PhysicalAnimationData")));
}

FStructProperty* GetDefaultProfileProperty()
{
	return FindFProperty<FStructProperty>(UPhysicsConstraintTemplate::StaticClass(), TEXT("DefaultProfile"));
}

const UScriptStruct* GetCreateParamsStruct()
{
	return StructOf(FindStructProperty(UPhysicsAssetGenerationSettings::StaticClass(), TEXT("CreateParams")));
}

// ---- values --------------------------------------------------------------------------------------------------------

FPropertyValue::FPropertyValue(const FProperty& InProperty)
	: Property(InProperty)
	, Memory(static_cast<uint8*>(FMemory::Malloc(FMath::Max(InProperty.GetSize(), 1), InProperty.GetMinAlignment())))
{
	Property.InitializeValue(Memory);
}

FPropertyValue::~FPropertyValue()
{
	Property.DestroyValue(Memory);
	FMemory::Free(Memory);
}

FStructInstance::FStructInstance(const UScriptStruct* InType)
	: Type(InType)
{
	if (Type != nullptr)
	{
		Memory.SetNumZeroed(FMath::Max(Type->GetStructureSize(), 1));
		Type->InitializeStruct(Memory.GetData());
	}
}

FStructInstance::~FStructInstance()
{
	if (Type != nullptr)
	{
		Type->DestroyStruct(Memory.GetData());
	}
}

bool IsEditableField(const FProperty& Property)
{
	return Property.HasAnyPropertyFlags(CPF_Edit)
		&& !Property.HasAnyPropertyFlags(CPF_EditConst | CPF_Transient | CPF_Deprecated);
}

namespace
{
// Physics data is single precision (Chaos shapes, constraint frames); vectors and transforms are stored as doubles
// whose extra digits are noise ("1.0000000000000002"). Every number is written as the shortest decimal that reads
// back to the same float, as the codec already does for float fields (the physics diff compares as float32).
void ShortestFloatNumbers(FValue& Value)
{
	if (Value.Kind == EAssetDocumentRuntimeValueKind::Number)
	{
		Value.NumberValue = ShortestFloat(Value.NumberValue);
	}
	for (FValue& Element : Value.Elements)
	{
		ShortestFloatNumbers(Element);
	}
}
}

bool EncodeValue(const FProperty& Property, const void* ValuePtr, FValue& OutValue, FString& OutError)
{
	FString Code;
	if (!UE::AssetDocument::EditorPropertyValueCodec::Encode(const_cast<FProperty&>(Property), ValuePtr, OutValue, Code))
	{
		OutError = FString::Printf(TEXT("%s cannot be represented (%s)."), *Property.GetName(), *Code);
		return false;
	}
	ShortestFloatNumbers(OutValue);
	return true;
}

bool EncodeSparse(const UStruct& Struct, const void* Value, const void* Defaults, FValue& OutObject, FString& OutError, FSkip Skip)
{
	FObjectBuilder Object;
	bool bAny = false;
	if (!EncodeSparseInto(Struct, Value, Defaults, Object, OutError, Skip, bAny))
	{
		return false;
	}
	OutObject = Object.Build();
	return true;
}

bool DecodeSparse(FReader& Reader, const FString& Path, const FValue& Object, const UStruct& Struct, void* Memory,
	const void* Defaults, UObject* Owner, FSkip Skip)
{
	if (Object.Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return Reader.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("%s is an object of %s fields."), *Path, *Struct.GetName()), Path);
	}
	TMap<FName, const FValue*> Listed;
	for (int32 Index = 0; Index < Object.ObjectKeys.Num(); ++Index)
	{
		const FString& Name = Object.ObjectKeys[Index];
		const FProperty* Property = Struct.FindPropertyByName(FName(*Name));
		if (Property == nullptr || !IsEditableField(*Property) || Skip(*Property))
		{
			TArray<FString> Editable = EditableNames(Struct, Skip);
			const bool bOwned = Property != nullptr && IsEditableField(*Property) && Skip(*Property);
			return Reader.Fail(TEXT("PhysicsAsset.FieldNotEditable"), FString::Printf(TEXT("%s has no editable field '%s'%s. Editable: %s."),
				*Struct.GetName(), *Name, bOwned ? TEXT(" (it has its own document key or is derived)") : TEXT(""),
				*FString::Join(Editable, TEXT(", "))), FString::Printf(TEXT("%s/%s"), *Path, *Escape(Name)));
		}
		Listed.Add(Property->GetFName(), &Object.Elements[Index]);
	}
	for (TFieldIterator<FProperty> It(&Struct); It; ++It)
	{
		FProperty& Property = **It;
		if (!IsEditableField(Property) || Skip(Property))
		{
			continue;
		}
		void* ValuePtr = Property.ContainerPtrToValuePtr<void>(Memory);
		const void* DefaultPtr = Property.ContainerPtrToValuePtr<void>(Defaults);
		const FValue* const* Authored = Listed.Find(Property.GetFName());
		const FString FieldPath = FString::Printf(TEXT("%s/%s"), *Path, *Escape(Property.GetName()));
		if (IsMemberwiseStruct(Property))
		{
			const UScriptStruct& Inner = *CastFieldChecked<FStructProperty>(&Property)->Struct;
			if (Authored == nullptr)
			{
				ResetEditable(Inner, ValuePtr, DefaultPtr, Skip);
			}
			else if (!DecodeSparse(Reader, FieldPath, **Authored, Inner, ValuePtr, DefaultPtr, Owner, Skip))
			{
				return false;
			}
			continue;
		}
		FPropertyValue Value(Property);
		Property.CopyCompleteValue(Value.Memory, DefaultPtr);
		if (Authored != nullptr)
		{
			FString Code;
			if (!UE::AssetDocument::EditorPropertyValueCodec::Decode(Property, **Authored, Value.Memory, Code, Owner))
			{
				return Reader.Fail(TEXT("PhysicsAsset.InvalidValue"), FString::Printf(TEXT("The value of '%s' does not fit the field (%s)."),
					*Property.GetName(), *Code), FieldPath);
			}
		}
		Property.CopyCompleteValue(ValuePtr, Value.Memory);
	}
	return true;
}

// ---- bodies --------------------------------------------------------------------------------------------------------

USkeletalBodySetup* MakeNewBodyDefaults()
{
	// FPhysicsAssetUtils CreateNewBodyAtIndexInternal.
	USkeletalBodySetup* Body = NewObject<USkeletalBodySetup>(GetTransientPackage(), NAME_None, RF_Transient);
	Body->CollisionTraceFlag = CTF_UseSimpleAsComplex;
	Body->PhysicsType = PhysType_Default;
	return Body;
}

bool IsBodyOwnedField(const FProperty& Property)
{
	const FName Name = Property.GetFName();
	const UStruct* Owner = Property.GetOwnerStruct();
	return (Name == TEXT("AggGeom") || Name == TEXT("CurrentPhysicalAnimationProfile") || Name == TEXT("BoneName"))
		&& Owner != nullptr && Owner->IsA<UClass>();
}

bool IsDerivedCollisionField(const FProperty& Property, FName CollisionProfileName)
{
	// A named collision profile (not Custom / None) sets these on load (FBodyInstance::LoadProfileData); the Details
	// panel greys them out.
	if (CollisionProfileName == TEXT("Custom") || CollisionProfileName.IsNone())
	{
		return false;
	}
	const UStruct* Owner = Property.GetOwnerStruct();
	if (Owner == nullptr || Owner->GetFName() != TEXT("BodyInstance"))
	{
		return false;
	}
	const FName Name = Property.GetFName();
	return Name == TEXT("ObjectType") || Name == TEXT("CollisionEnabled") || Name == TEXT("CollisionResponses");
}

TConstArrayView<FShapeType> GetShapeTypes()
{
	return ShapeTypes;
}

const FArrayProperty* GetShapeArrayProperty(const FShapeType& Type)
{
	return FindFProperty<FArrayProperty>(GetAggGeomStruct(), Type.Array);
}

const FArrayProperty* GetConvexVertexProperty()
{
	for (const FShapeType& Type : ShapeTypes)
	{
		if (FStringView(Type.Array) == TEXT("ConvexElems"))
		{
			const FArrayProperty* Convexes = GetShapeArrayProperty(Type);
			const FStructProperty* Inner = Convexes != nullptr ? CastField<FStructProperty>(Convexes->Inner) : nullptr;
			return Inner != nullptr ? FindFProperty<FArrayProperty>(Inner->Struct, TEXT("VertexData")) : nullptr;
		}
	}
	return nullptr;
}

FName AutoShapeName(FName Bone, const FShapeType& Type)
{
	return FName(FString::Printf(TEXT("%s_%s"), *Bone.ToString(), Type.AutoName));
}

bool EncodeShape(const FShapeType& Type, const UScriptStruct& Struct, const void* Element, FName Bone, FValue& OutValue, FString& OutError)
{
	FStructInstance Defaults(&Struct);
	const FName AutoName = AutoShapeName(Bone, Type);
	const FProperty* NameProperty = Struct.FindPropertyByName(TEXT("Name"));
	auto Skip = [NameProperty, Element, AutoName](const FProperty& Property)
	{
		// Name: omitted when it is the editor's auto name (written back by AutoNamePrimitive).
		return &Property == NameProperty && *NameProperty->ContainerPtrToValuePtr<FName>(Element) == AutoName;
	};
	FObjectBuilder Object;
	bool bAny = false;
	if (!EncodeSparseInto(Struct, Element, Defaults.Get(), Object, OutError, Skip, bAny))
	{
		return false;
	}
	if (Type.Array == FStringView(TEXT("ConvexElems")))
	{
		// VertexData (the hull's points) and Transform are not Details fields; they are the convex's content.
		const FKConvexElem& Convex = *static_cast<const FKConvexElem*>(Element);
		const FProperty* Transform = Struct.FindPropertyByName(TEXT("Transform"));
		if (Transform != nullptr && !Convex.GetTransform().Equals(FTransform::Identity, 0.0))
		{
			FValue Value;
			if (!EncodeValue(*Transform, Transform->ContainerPtrToValuePtr<void>(Element), Value, OutError))
			{
				return false;
			}
			Object.Add(TEXT("Transform"), MoveTemp(Value));
		}
	}
	OutValue = Object.Build();
	return true;
}

// ---- constraints ---------------------------------------------------------------------------------------------------

const void* GetDefaultProfile(const UPhysicsConstraintTemplate& Constraint)
{
	const FStructProperty* Property = GetDefaultProfileProperty();
	return Property != nullptr ? Property->ContainerPtrToValuePtr<void>(&Constraint) : &Constraint.DefaultInstance.ProfileInstance;
}

void* GetMutableDefaultProfile(UPhysicsConstraintTemplate& Constraint)
{
	return const_cast<void*>(GetDefaultProfile(Constraint));
}

bool CanSnap(const FConstraintInstance& Instance, const UPhysicsAsset& Asset)
{
	// FConstraintInstance::CalculateDefaultParentTransform asserts (check) when the preview mesh lacks either bone;
	// without a preview mesh it snaps to identity.
	const USkeletalMesh* Mesh = Asset.GetPreviewMesh();
	return Mesh == nullptr || (Mesh->GetRefSkeleton().FindBoneIndex(Instance.ConstraintBone1) != INDEX_NONE
		&& Mesh->GetRefSkeleton().FindBoneIndex(Instance.ConstraintBone2) != INDEX_NONE);
}

FConstraintInstance SnappedFrames(const FConstraintInstance& Instance, const UPhysicsAsset& Asset)
{
	FConstraintInstance Snapped;
	Snapped.ConstraintBone1 = Instance.ConstraintBone1;
	Snapped.ConstraintBone2 = Instance.ConstraintBone2;
	Snapped.SnapTransformsToDefault(EConstraintTransformComponentFlags::All, &Asset);
	return Snapped;
}

bool FramesAreSnapped(const FConstraintInstance& Instance, const UPhysicsAsset& Asset)
{
	if (!CanSnap(Instance, Asset))
	{
		return false;
	}
	const FConstraintInstance Snapped = SnappedFrames(Instance, Asset);
	return Instance.Pos1.Equals(Snapped.Pos1, FrameTolerance) && Instance.PriAxis1.Equals(Snapped.PriAxis1, FrameTolerance)
		&& Instance.SecAxis1.Equals(Snapped.SecAxis1, FrameTolerance) && Instance.Pos2.Equals(Snapped.Pos2, FrameTolerance)
		&& Instance.PriAxis2.Equals(Snapped.PriAxis2, FrameTolerance) && Instance.SecAxis2.Equals(Snapped.SecAxis2, FrameTolerance);
}

bool IsConstraintOwnedField(const FProperty& Property)
{
	const UStruct* Owner = Property.GetOwnerStruct();
	if (Owner == nullptr || Owner->GetFName() != TEXT("ConstraintInstance"))
	{
		return false;
	}
	static const FName Owned[] = {TEXT("JointName"), TEXT("ConstraintBone1"), TEXT("ConstraintBone2"), TEXT("Pos1"), TEXT("PriAxis1"),
		TEXT("SecAxis1"), TEXT("Pos2"), TEXT("PriAxis2"), TEXT("SecAxis2")};
	for (const FName& Name : Owned)
	{
		if (Property.GetFName() == Name)
		{
			return true;
		}
	}
	return false;
}

// ---- order ---------------------------------------------------------------------------------------------------------

TArray<int32> BodyOrder(const UPhysicsAsset& Asset)
{
	const USkeletalMesh* Mesh = Asset.GetPreviewMesh();
	const FReferenceSkeleton* Skeleton = Mesh != nullptr ? &Mesh->GetRefSkeleton() : nullptr;
	TArray<int32> Order;
	for (int32 Index = 0; Index < Asset.SkeletalBodySetups.Num(); ++Index)
	{
		if (Asset.SkeletalBodySetups[Index] != nullptr)
		{
			Order.Add(Index);
		}
	}
	Order.StableSort([&Asset, Skeleton](int32 A, int32 B)
	{
		const FName BoneA = Asset.SkeletalBodySetups[A]->BoneName;
		const FName BoneB = Asset.SkeletalBodySetups[B]->BoneName;
		const int32 OrderA = BoneOrderIndex(Skeleton, BoneA);
		const int32 OrderB = BoneOrderIndex(Skeleton, BoneB);
		if (OrderA != OrderB)
		{
			return OrderA < OrderB;
		}
		return BoneA.LexicalLess(BoneB);
	});
	return Order;
}

TArray<int32> ConstraintOrder(const UPhysicsAsset& Asset)
{
	const USkeletalMesh* Mesh = Asset.GetPreviewMesh();
	const FReferenceSkeleton* Skeleton = Mesh != nullptr ? &Mesh->GetRefSkeleton() : nullptr;
	TArray<int32> Order;
	for (int32 Index = 0; Index < Asset.ConstraintSetup.Num(); ++Index)
	{
		if (Asset.ConstraintSetup[Index] != nullptr)
		{
			Order.Add(Index);
		}
	}
	Order.StableSort([&Asset, Skeleton](int32 A, int32 B)
	{
		const FConstraintInstance& InstanceA = Asset.ConstraintSetup[A]->DefaultInstance;
		const FConstraintInstance& InstanceB = Asset.ConstraintSetup[B]->DefaultInstance;
		const int32 OrderA = BoneOrderIndex(Skeleton, InstanceA.ConstraintBone1);
		const int32 OrderB = BoneOrderIndex(Skeleton, InstanceB.ConstraintBone1);
		if (OrderA != OrderB)
		{
			return OrderA < OrderB;
		}
		if (InstanceA.ConstraintBone1 != InstanceB.ConstraintBone1)
		{
			return InstanceA.ConstraintBone1.LexicalLess(InstanceB.ConstraintBone1);
		}
		return InstanceA.JointName.LexicalLess(InstanceB.JointName);
	});
	return Order;
}

TArray<FString> ConstraintKeys(const UPhysicsAsset& Asset)
{
	TArray<FString> Keys;
	TMap<FName, int32> Seen;
	Keys.SetNum(Asset.ConstraintSetup.Num());
	for (int32 Index = 0; Index < Asset.ConstraintSetup.Num(); ++Index)
	{
		const UPhysicsConstraintTemplate* Constraint = Asset.ConstraintSetup[Index];
		if (Constraint == nullptr)
		{
			continue;
		}
		const FName Joint = Constraint->DefaultInstance.JointName;
		int32& Count = Seen.FindOrAdd(Joint, 0);
		++Count;
		Keys[Index] = Count == 1 ? Joint.ToString() : FString::Printf(TEXT("%s~%d"), *Joint.ToString(), Count);
	}
	return Keys;
}
} // namespace UE::AssetDocument::Graph::Physics
