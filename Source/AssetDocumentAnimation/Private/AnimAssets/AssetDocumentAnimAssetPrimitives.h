// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "AssetDocumentGraphPrimitives.h"

class UAnimationAsset;
class UClass;
class UObject;
class USkeleton;

// Standalone animation asset primitives (canonical provider "animation_asset", owner Animation).
// DTOs (all carry "schema_version": 1; every edit runs in one editor transaction per call and is
// covered by Core's package snapshot; names in ops are the names the editors show):
//
//   animation_asset.skeleton.read.v1  {} -> {bone_count, retargeting{bone: mode} (non-default),
//                                      slot_groups[{name, slots[]}], sockets[{name, bone, location{X,Y,Z},
//                                      rotation{Pitch,Yaw,Roll}, scale{X,Y,Z}, force_always_animated}],
//                                      virtual_bones[{name, source, target}], blend_profiles[{name, mode,
//                                      bones{bone: scale}}], curves[{name, material, morph_target}]}
//   animation_asset.skeleton.edit.v1  {ops[]}: {op: slot_group_add|slot_group_remove, name} |
//                                      {op: slot_set, name, group} | {op: slot_remove, name} | {op: slot_rename, name, to} |
//                                      {op: socket_add, name, bone} | {op: socket_remove, name} | {op: socket_rename, name, to} |
//                                      {op: socket_set, name, bone?, location?, rotation?, scale?, force_always_animated?} |
//                                      {op: virtual_bone_add, name, source, target} | {op: virtual_bone_remove, name} |
//                                      {op: virtual_bone_rename, name, to} | {op: blend_profile_add|blend_profile_remove, name} |
//                                      {op: blend_profile_rename, name, to} | {op: blend_profile_set, name, mode?, bones?{}} |
//                                      {op: retarget, bone, mode} | {op: curve_add|curve_remove, name} |
//                                      {op: curve_rename, name, to} | {op: curve_set, name, material?, morph_target?}
//   animation_asset.blend_space.read.v1 {} -> {skeleton, preview_mesh, axes[{name, min, max, grid, snap, wrap}],
//                                      samples[{animation, x, y?, rate_scale, single_frame?, analyse_all}]}
//   animation_asset.blend_space.edit.v1 {ops[]}: {op: axis, axis, name?, min?, max?, grid?, snap?, wrap?} |
//                                      {op: sample_remove, x, y?} | {op: sample_add, animation, x, y?} |
//                                      {op: sample_set, x, y?, animation?, rate_scale?, single_frame?, analyse_all?} |
//                                      {op: refresh} | {op: preview_mesh, path}
//   animation_asset.pose_asset.read.v1 {} -> {skeleton, preview_mesh, source_animation, poses[], additive, base_pose,
//                                      curves[], track_count}
//   animation_asset.pose_asset.edit.v1 {ops[]}: {op: rename, name, to} | {op: remove, names[]} |
//                                      {op: additive, additive, base_pose} | {op: update_source, animation} |
//                                      {op: preview_mesh, path}
//   animation_asset.composite.read.v1 {} -> {skeleton, preview_mesh, length, segments[{animation, start_pos, start,
//                                      end, rate, loops}]}
//   animation_asset.composite.edit.v1 {ops[]}: {op: segments, segments[{animation, start?, end?, rate?, loops?}]} |
//                                      {op: preview_mesh, path}
namespace UE::AssetDocument::Graph::AnimAssets
{
inline const TCHAR* ProviderId = TEXT("animation_asset");

TConstArrayView<FAssetDocumentPrimitiveDescriptor> GetDescriptors();
TConstArrayView<FBinding> GetBindings();

// ---- shared by the animation asset primitive files ------------------------------

bool Fail(FCall& Call, FStringView Code, FStringView Message, FStringView Path = TEXTVIEW("/"));

// Path of an object; "" for null.
FString PathOf(const UObject* Object);

// Loads the asset an op names (an object path) and checks its class; fails the op otherwise.
UObject* LoadReference(FReader& Op, const TCHAR* Key, UClass* Class, const TCHAR* What);

// The skeleton of an animation asset, and the preview mesh path.
void AddCommonFields(const UAnimationAsset& Asset, FObjectBuilder& Result);

// {op: preview_mesh, path}: the Persona preview scene's "apply to asset" (UAnimationAsset::SetPreviewMesh);
// "" clears it. The mesh must use a compatible skeleton.
bool SetPreviewMesh(FReader& Op, UAnimationAsset& Asset);

// Shortest decimal that reads back as the same float32 (stored floats read as 0.1, not 0.1000000015).
double ShortestFloat(float Value);
FValue FloatValue(double Value);

// Number field of an op, as float (finite).
bool ReadFloat(FReader& Op, const TCHAR* Key, double& Out, bool bRequired = true);

// Handlers.
bool HandleSkeletonRead(FCall& Call, FObjectBuilder& Result);
bool HandleSkeletonEdit(FCall& Call, FObjectBuilder& Result);
bool HandleBlendSpaceRead(FCall& Call, FObjectBuilder& Result);
bool HandleBlendSpaceEdit(FCall& Call, FObjectBuilder& Result);
bool HandlePoseAssetRead(FCall& Call, FObjectBuilder& Result);
bool HandlePoseAssetEdit(FCall& Call, FObjectBuilder& Result);
bool HandleCompositeRead(FCall& Call, FObjectBuilder& Result);
bool HandleCompositeEdit(FCall& Call, FObjectBuilder& Result);
} // namespace UE::AssetDocument::Graph::AnimAssets
