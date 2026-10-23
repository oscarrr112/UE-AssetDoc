// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// physics.asset.read.v1: the authored state of a Physics Asset (design §2.1, §3) as document-ready records. The asset
// is read as stored; nothing is rebuilt or written.
//
// Args {offset?, detail?: "document" | "inspect"}. Records (paged under the value budget), in document order (bodies by
// the preview mesh's bone order, constraints by child bone):
//   {kind: "header", preview_mesh, bodies, constraints, constraint_profiles[], physical_animation_profiles[],
//    defaults: {body, constraint, physical_animation, shapes: {<type key>: new element}}}  (the complete values the
//                                                       sparse entries are read against, so a diff compares an
//                                                       explicitly authored default as equal)
//   {kind: "body", bone, entry: {Properties?, Shapes, PhysicalAnimation?}}      (convex VertexData in convex_vertices)
//   {kind: "convex_vertices", bone, index, vertices[]}                          (chunked; Python appends to VertexData)
//   {kind: "constraint", key, entry: {Child, Parent, Frames?, Properties?, Profiles?}}
//   {kind: "collision", pairs: [[boneA, boneB], ...]}                           (chunked; disabled pairs)
//   {kind: "warning", code, message, path}                                      (duplicate joint names, stale table entries)
// detail "inspect" adds {kind: "skeleton", offset, bones[{Name, Parent, Body?}]}, {kind: "body_facts", bone, mass,
// in_mesh, read_only_shapes{}} and {kind: "mesh_users", meshes[{Path, Loaded, UsesAsset?}]}.
// detail "generation" (Inspect's Generation suggestion) reads instead what the editor's generation with the project's
// settings makes for the preview mesh: {kind: "generation", available, settings?, reason?} and then the document
// records of that generated (transient, discarded) asset.
//
// Values: body Properties are the USkeletalBodySetup Details sparse against a body as FPhysicsAssetUtils::CreateNewBody
// makes it; constraint Properties the FConstraintInstance Details (ProfileInstance read from the template's
// DefaultProfile, the value a save writes) sparse against a new constraint; profile data sparse against the
// constraint's default profile / a new FPhysicalAnimationData. Frames are emitted only when they differ from what
// the editor's Snap gives (FConstraintInstance::SnapTransformsToDefault).

#include "AssetDocumentPhysicsPrimitives.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/SkeletalMesh.h"
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
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace UE::AssetDocument::Graph::Physics
{
namespace
{
constexpr int32 VerticesPerRecord = 400;
constexpr int32 PairsPerRecord = 400;
constexpr int32 SkeletonBonesPerRecord = 150;

// Profile names of a body / constraint in the asset's list order, then names the list does not have.
template <typename FGetName>
TArray<int32> ProfileOrder(int32 Num, FGetName GetName, const TArray<FName>& Listed)
{
	TArray<int32> Order;
	for (int32 Index = 0; Index < Num; ++Index)
	{
		Order.Add(Index);
	}
	Order.StableSort([&](int32 A, int32 B)
	{
		const int32 ListA = Listed.IndexOfByKey(GetName(A));
		const int32 ListB = Listed.IndexOfByKey(GetName(B));
		const int32 KeyA = ListA != INDEX_NONE ? ListA : MAX_int32;
		const int32 KeyB = ListB != INDEX_NONE ? ListB : MAX_int32;
		if (KeyA != KeyB)
		{
			return KeyA < KeyB;
		}
		return GetName(A).LexicalLess(GetName(B));
	});
	return Order;
}

bool EncodeFrame(const UScriptStruct& InstanceStruct, const FConstraintInstance& Instance, const TCHAR* const Fields[3], FValue& OutFrame, FString& OutError)
{
	static const TCHAR* const Keys[] = {TEXT("Position"), TEXT("PriAxis"), TEXT("SecAxis")};
	FObjectBuilder Frame;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		const FProperty* Property = InstanceStruct.FindPropertyByName(Fields[Index]);
		FValue Value;
		if (Property == nullptr || !EncodeValue(*Property, Property->ContainerPtrToValuePtr<void>(&Instance), Value, OutError))
		{
			OutError = OutError.IsEmpty() ? FString::Printf(TEXT("No field %s."), Fields[Index]) : OutError;
			return false;
		}
		Frame.Add(Keys[Index], MoveTemp(Value));
	}
	OutFrame = Frame.Build();
	return true;
}

bool AddBody(const UPhysicsAsset& Asset, const USkeletalBodySetup& Body, const USkeletalBodySetup& Defaults, FCall& Call, TArray<FValue>& Records)
{
	FString Error;
	FObjectBuilder Entry;
	const FName Profile = Body.DefaultInstance.GetCollisionProfileName();
	auto Skip = [Profile](const FProperty& Property) { return IsBodyOwnedField(Property) || IsDerivedCollisionField(Property, Profile); };
	FValue Properties;
	if (!EncodeSparse(*USkeletalBodySetup::StaticClass(), &Body, &Defaults, Properties, Error, Skip))
	{
		return Fail(Call, TEXT("PhysicsAsset.ReadFailed"), FString::Printf(TEXT("Body %s: %s"), *Body.BoneName.ToString(), *Error));
	}
	if (!Properties.ObjectKeys.IsEmpty())
	{
		Entry.Add(TEXT("Properties"), MoveTemp(Properties));
	}

	FObjectBuilder Shapes;
	TArray<FValue> VertexRecords;
	for (const FShapeType& Type : GetShapeTypes())
	{
		const FArrayProperty* ArrayProperty = GetShapeArrayProperty(Type);
		const FStructProperty* Inner = ArrayProperty != nullptr ? CastField<FStructProperty>(ArrayProperty->Inner) : nullptr;
		if (!Type.bAuthored || Inner == nullptr)
		{
			continue;
		}
		FScriptArrayHelper Elements(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(&Body.AggGeom));
		if (Elements.Num() == 0)
		{
			continue;
		}
		TArray<FValue> Values;
		for (int32 Index = 0; Index < Elements.Num(); ++Index)
		{
			FValue Element;
			if (!EncodeShape(Type, *Inner->Struct, Elements.GetRawPtr(Index), Body.BoneName, Element, Error))
			{
				return Fail(Call, TEXT("PhysicsAsset.ReadFailed"), FString::Printf(TEXT("Body %s %s %d: %s"), *Body.BoneName.ToString(), Type.Key, Index, *Error));
			}
			Values.Add(MoveTemp(Element));
		}
		Shapes.Add(Type.Key, MakeArray(MoveTemp(Values)));
	}
	// Convex points: chunked records (a hull can have hundreds).
	const FArrayProperty* VertexArray = GetConvexVertexProperty();
	for (int32 Index = 0; Index < Body.AggGeom.ConvexElems.Num() && VertexArray != nullptr; ++Index)
	{
		const TArray<FVector>& Vertices = Body.AggGeom.ConvexElems[Index].VertexData;
		for (int32 Start = 0; Start < FMath::Max(Vertices.Num(), 1); Start += VerticesPerRecord)
		{
			TArray<FValue> Chunk;
			for (int32 Vertex = Start; Vertex < FMath::Min(Start + VerticesPerRecord, Vertices.Num()); ++Vertex)
			{
				FValue Value;
				if (!EncodeValue(*VertexArray->Inner, &Vertices[Vertex], Value, Error))
				{
					return Fail(Call, TEXT("PhysicsAsset.ReadFailed"), Error);
				}
				Chunk.Add(MoveTemp(Value));
			}
			FObjectBuilder Record;
			Record.AddString(TEXT("kind"), TEXT("convex_vertices"));
			Record.Add(TEXT("bone"), MakeName(Body.BoneName));
			Record.AddInt(TEXT("index"), Index);
			Record.Add(TEXT("vertices"), MakeArray(MoveTemp(Chunk)));
			VertexRecords.Add(Record.Build());
		}
	}
	Entry.Add(TEXT("Shapes"), Shapes.Build());

	const TArray<FPhysicalAnimationProfile>& Profiles = Body.GetPhysicalAnimationProfiles();
	if (!Profiles.IsEmpty())
	{
		const UScriptStruct* DataStruct = GetPhysicalAnimationDataStruct();
		if (DataStruct == nullptr)
		{
			return Fail(Call, TEXT("PhysicsAsset.ReadFailed"), TEXT("FPhysicalAnimationData is not reflected."));
		}
		FStructInstance DataDefaults(DataStruct);
		FObjectBuilder PhysicalAnimation;
		const TArray<int32> Order = ProfileOrder(Profiles.Num(), [&Profiles](int32 Index) { return Profiles[Index].ProfileName; },
			Asset.GetPhysicalAnimationProfileNames());
		for (const int32 Index : Order)
		{
			FValue Data;
			if (!EncodeSparse(*DataStruct, &Profiles[Index].PhysicalAnimationData, DataDefaults.Get(), Data, Error, [](const FProperty&) { return false; }))
			{
				return Fail(Call, TEXT("PhysicsAsset.ReadFailed"), Error);
			}
			PhysicalAnimation.Add(Profiles[Index].ProfileName.ToString(), MoveTemp(Data));
		}
		Entry.Add(TEXT("PhysicalAnimation"), PhysicalAnimation.Build());
	}

	FObjectBuilder Record;
	Record.AddString(TEXT("kind"), TEXT("body"));
	Record.Add(TEXT("bone"), MakeName(Body.BoneName));
	Record.Add(TEXT("entry"), Entry.Build());
	Records.Add(Record.Build());
	Records.Append(MoveTemp(VertexRecords));
	return true;
}

bool AddConstraint(const UPhysicsAsset& Asset, const UPhysicsConstraintTemplate& Constraint, const FString& Key, FCall& Call, TArray<FValue>& Records)
{
	const UScriptStruct* InstanceStruct = GetConstraintInstanceStruct();
	const UScriptStruct* ProfileStruct = GetConstraintProfileStruct();
	if (InstanceStruct == nullptr || ProfileStruct == nullptr)
	{
		return Fail(Call, TEXT("PhysicsAsset.ReadFailed"), TEXT("FConstraintInstance is not reflected."));
	}
	const FConstraintInstance& Instance = Constraint.DefaultInstance;
	FString Error;
	FObjectBuilder Entry;
	Entry.Add(TEXT("Child"), MakeName(Instance.ConstraintBone1));
	Entry.Add(TEXT("Parent"), MakeName(Instance.ConstraintBone2));
	if (!FramesAreSnapped(Instance, Asset))
	{
		static const TCHAR* const ChildFields[3] = {TEXT("Pos1"), TEXT("PriAxis1"), TEXT("SecAxis1")};
		static const TCHAR* const ParentFields[3] = {TEXT("Pos2"), TEXT("PriAxis2"), TEXT("SecAxis2")};
		FValue Child;
		FValue Parent;
		if (!EncodeFrame(*InstanceStruct, Instance, ChildFields, Child, Error) || !EncodeFrame(*InstanceStruct, Instance, ParentFields, Parent, Error))
		{
			return Fail(Call, TEXT("PhysicsAsset.ReadFailed"), Error);
		}
		FObjectBuilder Frames;
		Frames.Add(TEXT("Child"), MoveTemp(Child));
		Frames.Add(TEXT("Parent"), MoveTemp(Parent));
		Entry.Add(TEXT("Frames"), Frames.Build());
	}

	// The authored instance: ProfileInstance is the default profile (what a save writes; the instance may hold a
	// profile the editor previewed).
	FStructInstance Authored(InstanceStruct);
	InstanceStruct->CopyScriptStruct(Authored.Get(), &Instance);
	ProfileStruct->CopyScriptStruct(&static_cast<FConstraintInstance*>(Authored.Get())->ProfileInstance, GetDefaultProfile(Constraint));
	const FConstraintInstance& Defaults = GetDefault<UPhysicsConstraintTemplate>()->DefaultInstance;
	FValue Properties;
	if (!EncodeSparse(*InstanceStruct, Authored.Get(), &Defaults, Properties, Error, [](const FProperty& Property) { return IsConstraintOwnedField(Property); }))
	{
		return Fail(Call, TEXT("PhysicsAsset.ReadFailed"), FString::Printf(TEXT("Constraint %s: %s"), *Key, *Error));
	}
	if (!Properties.ObjectKeys.IsEmpty())
	{
		Entry.Add(TEXT("Properties"), MoveTemp(Properties));
	}

	if (!Constraint.ProfileHandles.IsEmpty())
	{
		FObjectBuilder Profiles;
		const TArray<FPhysicsConstraintProfileHandle>& Handles = Constraint.ProfileHandles;
		const TArray<int32> Order = ProfileOrder(Handles.Num(), [&Handles](int32 Index) { return Handles[Index].ProfileName; },
			Asset.GetConstraintProfileNames());
		for (const int32 Index : Order)
		{
			FValue Data;
			if (!EncodeSparse(*ProfileStruct, &Handles[Index].ProfileProperties, GetDefaultProfile(Constraint), Data, Error, [](const FProperty&) { return false; }))
			{
				return Fail(Call, TEXT("PhysicsAsset.ReadFailed"), Error);
			}
			Profiles.Add(Handles[Index].ProfileName.ToString(), MoveTemp(Data));
		}
		Entry.Add(TEXT("Profiles"), Profiles.Build());
	}

	FObjectBuilder Record;
	Record.AddString(TEXT("kind"), TEXT("constraint"));
	Record.AddString(TEXT("key"), Key);
	Record.Add(TEXT("entry"), Entry.Build());
	Records.Add(Record.Build());
	return true;
}

void AddWarning(TArray<FValue>& Records, const TCHAR* Code, const FString& Message, const FString& Path)
{
	FObjectBuilder Record;
	Record.AddString(TEXT("kind"), TEXT("warning"));
	Record.AddString(TEXT("code"), Code);
	Record.AddString(TEXT("message"), Message);
	Record.AddString(TEXT("path"), Path);
	Records.Add(Record.Build());
}

void AddCollision(const UPhysicsAsset& Asset, const TArray<int32>& BodyOrderIndices, TArray<FValue>& Records)
{
	TMap<int32, int32> Position;
	for (int32 Index = 0; Index < BodyOrderIndices.Num(); ++Index)
	{
		Position.Add(BodyOrderIndices[Index], Index);
	}
	TArray<TPair<int32, int32>> Pairs;
	int32 Stale = 0;
	for (const TPair<FRigidBodyIndexPair, bool>& Entry : Asset.CollisionDisableTable)
	{
		const int32* A = Position.Find(Entry.Key.Indices[0]);
		const int32* B = Position.Find(Entry.Key.Indices[1]);
		if (A == nullptr || B == nullptr || *A == *B)
		{
			++Stale;
			continue;
		}
		Pairs.Emplace(FMath::Min(*A, *B), FMath::Max(*A, *B));
	}
	Pairs.Sort([](const TPair<int32, int32>& L, const TPair<int32, int32>& R) { return L.Key != R.Key ? L.Key < R.Key : L.Value < R.Value; });
	for (int32 Start = 0; Start < Pairs.Num(); Start += PairsPerRecord)
	{
		TArray<FValue> Chunk;
		for (int32 Index = Start; Index < FMath::Min(Start + PairsPerRecord, Pairs.Num()); ++Index)
		{
			const FName A = Asset.SkeletalBodySetups[BodyOrderIndices[Pairs[Index].Key]]->BoneName;
			const FName B = Asset.SkeletalBodySetups[BodyOrderIndices[Pairs[Index].Value]]->BoneName;
			Chunk.Add(MakeArray({MakeName(A), MakeName(B)}));
		}
		FObjectBuilder Record;
		Record.AddString(TEXT("kind"), TEXT("collision"));
		Record.Add(TEXT("pairs"), MakeArray(MoveTemp(Chunk)));
		Records.Add(Record.Build());
	}
	if (Stale > 0)
	{
		AddWarning(Records, TEXT("PhysicsAsset.StaleCollisionEntry"), FString::Printf(TEXT("%d collision table entr%s name%s no body (index out of "
			"range); they have no effect and are dropped when the table is next written."), Stale, Stale == 1 ? TEXT("y") : TEXT("ies"),
			Stale == 1 ? TEXT("s") : TEXT("")), TEXT("/Body/CollisionDisabled"));
	}
}

void AddInspect(const UPhysicsAsset& Asset, const TArray<int32>& Order, TArray<FValue>& Records)
{
	const USkeletalMesh* Mesh = Asset.GetPreviewMesh();
	if (Mesh != nullptr)
	{
		const FReferenceSkeleton& Skeleton = Mesh->GetRefSkeleton();
		for (int32 Start = 0; Start < Skeleton.GetRawBoneNum(); Start += SkeletonBonesPerRecord)
		{
			TArray<FValue> Bones;
			for (int32 Index = Start; Index < FMath::Min(Start + SkeletonBonesPerRecord, Skeleton.GetRawBoneNum()); ++Index)
			{
				FObjectBuilder Bone;
				const FName Name = Skeleton.GetBoneName(Index);
				Bone.Add(TEXT("Name"), MakeName(Name));
				const int32 Parent = Skeleton.GetParentIndex(Index);
				Bone.Add(TEXT("Parent"), MakeName(Parent != INDEX_NONE ? Skeleton.GetBoneName(Parent) : NAME_None));
				if (Asset.FindBodyIndex(Name) != INDEX_NONE)
				{
					Bone.AddBool(TEXT("Body"), true);
				}
				Bones.Add(Bone.Build());
			}
			FObjectBuilder Record;
			Record.AddString(TEXT("kind"), TEXT("skeleton"));
			Record.AddInt(TEXT("offset"), Start);
			Record.Add(TEXT("bones"), MakeArray(MoveTemp(Bones)));
			Records.Add(Record.Build());
		}
	}
	for (const int32 Index : Order)
	{
		const USkeletalBodySetup& Body = *Asset.SkeletalBodySetups[Index];
		FObjectBuilder Record;
		Record.AddString(TEXT("kind"), TEXT("body_facts"));
		Record.Add(TEXT("bone"), MakeName(Body.BoneName));
		Record.Add(TEXT("mass"), MakeNumber(ShortestFloat(Body.CalculateMass())));
		Record.AddBool(TEXT("in_mesh"), Mesh != nullptr && Mesh->GetRefSkeleton().FindBoneIndex(Body.BoneName) != INDEX_NONE);
		FObjectBuilder ReadOnly;
		for (const FShapeType& Type : GetShapeTypes())
		{
			const FArrayProperty* ArrayProperty = GetShapeArrayProperty(Type);
			if (Type.bAuthored || ArrayProperty == nullptr)
			{
				continue;
			}
			FScriptArrayHelper Elements(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(&Body.AggGeom));
			if (Elements.Num() > 0)
			{
				ReadOnly.AddInt(Type.Key, Elements.Num());
			}
		}
		Record.Add(TEXT("read_only_shapes"), ReadOnly.Build());
		Records.Add(Record.Build());
	}

	// Skeletal meshes that reference the asset's package (the assignment is the mesh's property: design §8.2).
	TArray<FValue> Meshes;
	IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
	TArray<FName> Referencers;
	Registry.GetReferencers(Asset.GetOutermost()->GetFName(), Referencers);
	Referencers.Sort(FNameLexicalLess());
	for (const FName& Package : Referencers)
	{
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPackageName(Package, Assets, true);
		for (const FAssetData& Data : Assets)
		{
			if (Data.AssetClassPath != USkeletalMesh::StaticClass()->GetClassPathName())
			{
				continue;
			}
			FObjectBuilder Row;
			Row.AddString(TEXT("Path"), Data.GetObjectPathString());
			const USkeletalMesh* Loaded = Cast<USkeletalMesh>(Data.FastGetAsset(false));
			Row.AddBool(TEXT("Loaded"), Loaded != nullptr);
			if (Loaded != nullptr)
			{
				Row.AddBool(TEXT("UsesAsset"), Loaded->GetPhysicsAsset() == &Asset);
			}
			Meshes.Add(Row.Build());
		}
	}
	FObjectBuilder Record;
	Record.AddString(TEXT("kind"), TEXT("mesh_users"));
	Record.Add(TEXT("meshes"), MakeArray(MoveTemp(Meshes)));
	Records.Add(Record.Build());
}

// The complete values sparse entries omit (a new body's Details, a new constraint's Details, a new physical
// animation profile entry); a constraint's profile entries are sparse against its own default profile, which Python
// merges from these and the constraint's Properties.
bool AddDefaults(FCall& Call, FObjectBuilder& Header)
{
	const UScriptStruct* InstanceStruct = GetConstraintInstanceStruct();
	const UScriptStruct* DataStruct = GetPhysicalAnimationDataStruct();
	if (InstanceStruct == nullptr || DataStruct == nullptr)
	{
		return Fail(Call, TEXT("PhysicsAsset.ReadFailed"), TEXT("FConstraintInstance / FPhysicalAnimationData is not reflected."));
	}
	FString Error;
	FValue Body;
	FValue Constraint;
	FValue Animation;
	FStructInstance DataDefaults(DataStruct);
	if (!EncodeSparse(*USkeletalBodySetup::StaticClass(), MakeNewBodyDefaults(), nullptr, Body, Error, [](const FProperty& Property) { return IsBodyOwnedField(Property); })
		|| !EncodeSparse(*InstanceStruct, &GetDefault<UPhysicsConstraintTemplate>()->DefaultInstance, nullptr, Constraint, Error,
			[](const FProperty& Property) { return IsConstraintOwnedField(Property); })
		|| !EncodeSparse(*DataStruct, DataDefaults.Get(), nullptr, Animation, Error, [](const FProperty&) { return false; }))
	{
		return Fail(Call, TEXT("PhysicsAsset.ReadFailed"), FString::Printf(TEXT("Defaults: %s"), *Error));
	}
	// A new shape element of each authored type (EncodeShape reads elements sparse against it): every editable
	// field but Name (a new element's name is the editor's auto name, "<bone>_<suffix>"), and a convex's Transform.
	FObjectBuilder Shapes;
	for (const FShapeType& Type : GetShapeTypes())
	{
		const FArrayProperty* ArrayProperty = GetShapeArrayProperty(Type);
		const FStructProperty* Inner = ArrayProperty != nullptr ? CastField<FStructProperty>(ArrayProperty->Inner) : nullptr;
		if (!Type.bAuthored || Inner == nullptr)
		{
			continue;
		}
		FStructInstance Element(Inner->Struct);
		FValue Fields;
		if (!EncodeSparse(*Inner->Struct, Element.Get(), nullptr, Fields, Error,
			[](const FProperty& Property) { return Property.GetFName() == TEXT("Name"); }))
		{
			return Fail(Call, TEXT("PhysicsAsset.ReadFailed"), FString::Printf(TEXT("Defaults: %s %s"), Type.Key, *Error));
		}
		if (const FProperty* Transform = Type.Array == FStringView(TEXT("ConvexElems")) ? Inner->Struct->FindPropertyByName(TEXT("Transform")) : nullptr)
		{
			FValue Value;
			if (!EncodeValue(*Transform, Transform->ContainerPtrToValuePtr<void>(Element.Get()), Value, Error))
			{
				return Fail(Call, TEXT("PhysicsAsset.ReadFailed"), FString::Printf(TEXT("Defaults: %s %s"), Type.Key, *Error));
			}
			Fields.ObjectKeys.Add(TEXT("Transform"));
			Fields.Elements.Add(MoveTemp(Value));
		}
		Shapes.Add(Type.Key, MoveTemp(Fields));
	}
	FObjectBuilder Defaults;
	Defaults.Add(TEXT("body"), MoveTemp(Body));
	Defaults.Add(TEXT("constraint"), MoveTemp(Constraint));
	Defaults.Add(TEXT("physical_animation"), MoveTemp(Animation));
	Defaults.Add(TEXT("shapes"), Shapes.Build());
	Header.Add(TEXT("defaults"), Defaults.Build());
	return true;
}
} // namespace

namespace
{
// The document records of Asset: header (with the defaults on the first page), bodies, constraints, collision pairs,
// read warnings.
bool AddDocumentRecords(FCall& Call, const UPhysicsAsset& Asset, int64 Offset, TArray<FValue>& Records, TArray<int32>& OutBodies)
{
	{
		FObjectBuilder Header;
		Header.AddString(TEXT("kind"), TEXT("header"));
		Header.AddString(TEXT("preview_mesh"), Asset.PreviewSkeletalMesh.IsNull() ? FString() : Asset.PreviewSkeletalMesh.ToSoftObjectPath().ToString());
		Header.AddInt(TEXT("bodies"), Asset.SkeletalBodySetups.Num());
		Header.AddInt(TEXT("constraints"), Asset.ConstraintSetup.Num());
		TArray<FString> ConstraintProfiles;
		for (const FName& Name : Asset.GetConstraintProfileNames())
		{
			ConstraintProfiles.Add(Name.ToString());
		}
		TArray<FString> AnimationProfiles;
		for (const FName& Name : Asset.GetPhysicalAnimationProfileNames())
		{
			AnimationProfiles.Add(Name.ToString());
		}
		Header.Add(TEXT("constraint_profiles"), MakeStringArray(ConstraintProfiles));
		Header.Add(TEXT("physical_animation_profiles"), MakeStringArray(AnimationProfiles));
		if (Offset == 0 && !AddDefaults(Call, Header))
		{
			return false;
		}
		Records.Add(Header.Build());
	}
	OutBodies = BodyOrder(Asset);
	const USkeletalBodySetup* Defaults = MakeNewBodyDefaults();
	for (const int32 Index : OutBodies)
	{
		if (!AddBody(Asset, *Asset.SkeletalBodySetups[Index], *Defaults, Call, Records))
		{
			return false;
		}
	}
	const TArray<FString> Keys = ConstraintKeys(Asset);
	for (const int32 Index : ConstraintOrder(Asset))
	{
		if (!AddConstraint(Asset, *Asset.ConstraintSetup[Index], Keys[Index], Call, Records))
		{
			return false;
		}
		if (Keys[Index].Contains(TEXT("~")))
		{
			AddWarning(Records, TEXT("PhysicsAsset.DuplicateJointName"), FString::Printf(TEXT("Two constraints are named '%s'; the later one is "
				"keyed '%s'. Rename one (RenamedFrom) so each joint name is unique."), *Asset.ConstraintSetup[Index]->DefaultInstance.JointName.ToString(),
				*Keys[Index]), FString::Printf(TEXT("/Body/Constraints/%s"), *Escape(Keys[Index])));
		}
	}
	AddCollision(Asset, OutBodies, Records);
	return true;
}

// What the editor's "Create Physics Asset" (or the document's Generation with no settings) would make for the asset's
// preview mesh with the project's generation settings: FPhysicsAssetUtils::CreateFromSkeletalMesh on a transient
// asset, read as a document, and the settings used. Nothing persistent is touched (the asset and the mesh are only
// read; the transient asset is discarded).
bool AddGeneration(FCall& Call, const UPhysicsAsset& Asset, int64 Offset, TArray<FValue>& Records)
{
	USkeletalMesh* Mesh = Asset.GetPreviewMesh();
	const UScriptStruct* ParamsStruct = GetCreateParamsStruct();
	FObjectBuilder Settings;
	Settings.AddString(TEXT("kind"), TEXT("generation"));
	if (Mesh == nullptr || Mesh->GetResourceForRendering() == nullptr || ParamsStruct == nullptr)
	{
		Settings.AddBool(TEXT("available"), false);
		Records.Add(Settings.Build());
		return true;
	}
	const FPhysAssetCreateParams& Params = GetDefault<UPhysicsAssetGenerationSettings>()->CreateParams;
	FValue Encoded;
	FString Error;
	if (!EncodeSparse(*ParamsStruct, &Params, nullptr, Encoded, Error, [](const FProperty&) { return false; }))
	{
		return Fail(Call, TEXT("PhysicsAsset.ReadFailed"), FString::Printf(TEXT("Generation settings: %s"), *Error));
	}
	TStrongObjectPtr<UPhysicsAsset> Generated(NewObject<UPhysicsAsset>(GetTransientPackage(), NAME_None, RF_Transient));
	FText GenerationError;
	if (!FPhysicsAssetUtils::CreateFromSkeletalMesh(Generated.Get(), Mesh, Params, GenerationError, false, false))
	{
		Settings.AddBool(TEXT("available"), false);
		Settings.AddString(TEXT("reason"), GenerationError.ToString());
		Records.Add(Settings.Build());
		Generated->MarkAsGarbage();
		return true;
	}
	Settings.AddBool(TEXT("available"), true);
	Settings.Add(TEXT("settings"), MoveTemp(Encoded));
	Records.Add(Settings.Build());
	TArray<int32> Bodies;
	const bool bRead = AddDocumentRecords(Call, *Generated.Get(), Offset, Records, Bodies);
	Generated->MarkAsGarbage();
	return bRead;
}
} // namespace

bool HandleRead(FCall& Call, FObjectBuilder& Result)
{
	int64 Offset = 0;
	FString Detail = TEXT("document");
	if (!Call.Args.OnlyFields({TEXT("offset"), TEXT("detail")}) || !Call.Args.Int(TEXT("offset"), Offset, false)
		|| !Call.Args.String(TEXT("detail"), Detail, false))
	{
		return false;
	}
	if (Offset < 0 || (Detail != TEXT("document") && Detail != TEXT("inspect") && Detail != TEXT("generation")))
	{
		return Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("offset must be >= 0 and detail document, inspect or generation."), TEXT("detail"));
	}
	const UPhysicsAsset* Asset = Cast<UPhysicsAsset>(&Call.Asset);
	if (Asset == nullptr)
	{
		return Fail(Call, TEXT("PhysicsAsset.UnsupportedAsset"), TEXT("physics.asset.read.v1 reads Physics Assets."), TEXT("/HandleArguments/asset"));
	}

	TArray<FValue> Records;
	if (Detail == TEXT("generation"))
	{
		if (!AddGeneration(Call, *Asset, Offset, Records))
		{
			return false;
		}
		AddRecordPage(Records, Offset, Result);
		return true;
	}
	TArray<int32> Bodies;
	if (!AddDocumentRecords(Call, *Asset, Offset, Records, Bodies))
	{
		return false;
	}
	if (Detail == TEXT("inspect"))
	{
		AddInspect(*Asset, Bodies, Records);
	}
	AddRecordPage(Records, Offset, Result);
	return true;
}
} // namespace UE::AssetDocument::Graph::Physics
