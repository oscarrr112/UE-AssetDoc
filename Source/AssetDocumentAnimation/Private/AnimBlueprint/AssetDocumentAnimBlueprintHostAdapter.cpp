// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// UAnimBlueprint host adapter: the Blueprint host adapter plus anim layers.
// Top-level anim graphs other than "AnimGraph" (own layers and implemented
// layer-interface graphs) get role "anim_layer"; graph.create.v1 role
// "anim_layer" is My Blueprint > Add Animation Layer.
//
// UAnimBlueprint settings adapter (blueprint.member.query.v1 "settings",
// blueprint.member.setting.set.v1): the Animation Blueprint editor settings that
// are not editable reflected properties:
//   preview_mesh, preview_animation_blueprint,
//   preview_animation_blueprint_application_method, preview_animation_blueprint_tag
//                                (Preview Scene Settings / Anim Blueprint preview setters)
//   sync_groups                 [name] (UAnimBlueprint::Groups)
//   parent_asset_overrides      [{parent_node_guid, asset}] (Asset Override editor of a child ABP)
//   thumbnail                   {OrbitPitch, OrbitYaw, OrbitZoom} (thumbnail camera)
//
// UAnimBlueprint rebuild adapter: anim compile output (UAnimBlueprintExtension objects, objects
// under the generated class) is derived state for the package snapshot.
//   is_template, blueprint_type read-only.

#include "AnimBlueprint/AssetDocumentAnimBlueprintAdapters.h"

#include "AnimationGraph.h"
#include "AnimationGraphSchema.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "AnimBlueprintExtension.h"
#include "Animation/AnimationAsset.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/SkeletalMesh.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "ThumbnailRendering/SceneThumbnailInfo.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::AnimBlueprint
{
namespace HostAdapterDetail
{
TSharedPtr<IAssetDocumentGraphHostAdapter> BlueprintHost()
{
	return FindAssetDocumentGraphHostAdapter(GetDefault<UBlueprint>());
}

class FAnimBlueprintHostAdapter final : public IAssetDocumentGraphHostAdapter
{
public:
	virtual UClass* GetAssetClass() const override
	{
		return UAnimBlueprint::StaticClass();
	}

	virtual void EnumerateGraphs(UObject& Asset, TArray<FAssetDocumentGraphInfo>& OutGraphs) const override
	{
		const TSharedPtr<IAssetDocumentGraphHostAdapter> Host = BlueprintHost();
		if (!Host.IsValid())
		{
			return;
		}
		const int32 First = OutGraphs.Num();
		Host->EnumerateGraphs(Asset, OutGraphs);
		for (int32 Index = First; Index < OutGraphs.Num(); ++Index)
		{
			FAssetDocumentGraphInfo& Info = OutGraphs[Index];
			if (Info.Role == TEXT("anim_graph") && Info.Graph != nullptr && Info.Graph->GetFName() != UEdGraphSchema_K2::GN_AnimGraph)
			{
				Info.Role = TEXT("anim_layer");
			}
		}
	}

	virtual bool CreateGraph(UObject& Asset, FStringView Role, FStringView Name, UEdGraph* ParentGraph, UEdGraph*& OutGraph,
		FAssetDocumentGraphError& OutError) const override
	{
		if (Role != TEXTVIEW("anim_layer"))
		{
			const TSharedPtr<IAssetDocumentGraphHostAdapter> Host = BlueprintHost();
			return Host.IsValid() && Host->CreateGraph(Asset, Role, Name, ParentGraph, OutGraph, OutError);
		}
		UAnimBlueprint& Blueprint = *CastChecked<UAnimBlueprint>(&Asset);
		if (ParentGraph != nullptr || Blueprint.BlueprintType == BPTYPE_MacroLibrary)
		{
			OutError.Set(TEXT("Graph.GraphOperationUnsupported"), TEXT("Anim layers are top-level graphs of an Animation Blueprint."), TEXT("/parent"));
			return false;
		}
		FKismetNameValidator Validator(&Blueprint);
		const EValidatorResult Valid = Validator.IsValid(FString(Name));
		if (Valid != EValidatorResult::Ok)
		{
			OutError.Set(TEXT("Graph.InvalidArgument"), INameValidatorInterface::GetErrorString(FString(Name), Valid), TEXT("/name"));
			return false;
		}
		// FBlueprintEditor::NewDocument_OnClicked(CGT_NewAnimationLayer).
		Blueprint.Modify();
		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
			&Blueprint, FName(Name), UAnimationGraph::StaticClass(), UAnimationGraphSchema::StaticClass());
		FBlueprintEditorUtils::AddDomainSpecificGraph(&Blueprint, Graph);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
		OutGraph = Graph;
		return true;
	}

	virtual bool RemoveGraph(UObject& Asset, UEdGraph& Graph, FAssetDocumentGraphError& OutError) const override
	{
		const TSharedPtr<IAssetDocumentGraphHostAdapter> Host = BlueprintHost();
		return Host.IsValid() && Host->RemoveGraph(Asset, Graph, OutError);
	}

	virtual bool RenameGraph(UObject& Asset, UEdGraph& Graph, FStringView NewName, FAssetDocumentGraphError& OutError) const override
	{
		const TSharedPtr<IAssetDocumentGraphHostAdapter> Host = BlueprintHost();
		return Host.IsValid() && Host->RenameGraph(Asset, Graph, NewName, OutError);
	}

	virtual bool Finalize(UObject& Asset, FAssetDocumentGraphFinalizeResult& OutResult, FAssetDocumentGraphError& OutError) const override
	{
		const TSharedPtr<IAssetDocumentGraphHostAdapter> Host = BlueprintHost();
		if (!Host.IsValid())
		{
			OutError.Set(TEXT("Graph.GraphOperationUnsupported"), TEXT("The Blueprint host adapter is not registered."));
			return false;
		}
		return Host->Finalize(Asset, OutResult, OutError);
	}

	virtual void NotifyGraphChanged(UObject& Asset, UEdGraph* Graph, bool bStructural) const override
	{
		if (const TSharedPtr<IAssetDocumentGraphHostAdapter> Host = BlueprintHost())
		{
			Host->NotifyGraphChanged(Asset, Graph, bStructural);
		}
	}
};

// ---------------------------------------------------------------------------

FValue EncodeMember(const UAnimBlueprint& Blueprint, const TCHAR* PropertyName)
{
	const FProperty* Property = FindFProperty<FProperty>(UAnimBlueprint::StaticClass(), PropertyName);
	return Property != nullptr
		? EncodeAssetDocumentGraphPropertyValue(*Property, Property->ContainerPtrToValuePtr<void>(&Blueprint)) : FValue();
}

bool ReadObjectPath(const FValue& Value, FString& OutPath)
{
	if (Value.Kind == EAssetDocumentRuntimeValueKind::Null)
	{
		OutPath.Reset();
		return true;
	}
	if (Value.Kind == EAssetDocumentRuntimeValueKind::String)
	{
		OutPath = Value.StringValue;
		return true;
	}
	return false;
}

template <typename T>
bool LoadOptional(const FValue& Value, FStringView Setting, T*& OutObject, FAssetDocumentGraphError& OutError)
{
	FString Path;
	OutObject = nullptr;
	if (!ReadObjectPath(Value, Path))
	{
		OutError.Set(TEXT("BlueprintMember.InvalidValue"), FString::Printf(TEXT("%.*s is an object path or null."),
			Setting.Len(), Setting.GetData()), TEXT("/value"));
		return false;
	}
	if (Path.IsEmpty())
	{
		return true;
	}
	OutObject = LoadObject<T>(nullptr, *Path);
	if (OutObject == nullptr)
	{
		OutError.Set(TEXT("BlueprintMember.InvalidValue"), FString::Printf(TEXT("%.*s: %s is not a %s."),
			Setting.Len(), Setting.GetData(), *Path, *T::StaticClass()->GetName()), TEXT("/value"));
		return false;
	}
	return true;
}

class FAnimBlueprintSettingsAdapter final : public IAssetDocumentBlueprintSettingsAdapter
{
public:
	virtual UClass* GetBlueprintClass() const override
	{
		return UAnimBlueprint::StaticClass();
	}

	virtual FAssetDocumentRuntimeValueDto QuerySettings(UBlueprint& InBlueprint) const override
	{
		UAnimBlueprint& Blueprint = *CastChecked<UAnimBlueprint>(&InBlueprint);
		FValue Out = MakeObjectValue();
		SetField(Out, TEXT("preview_mesh"), EncodeMember(Blueprint, TEXT("PreviewSkeletalMesh")));
		SetField(Out, TEXT("preview_animation_blueprint"), EncodeMember(Blueprint, TEXT("PreviewAnimationBlueprint")));
		SetField(Out, TEXT("preview_animation_blueprint_application_method"),
			EncodeMember(Blueprint, TEXT("PreviewAnimationBlueprintApplicationMethod")));
		const FName PreviewTag = Blueprint.GetPreviewAnimationBlueprintTag();
		// NAME_None reads as "" (FName::ToString would yield "None").
		SetField(Out, TEXT("preview_animation_blueprint_tag"), MakeStringValue(PreviewTag.IsNone() ? FString() : PreviewTag.ToString()));
		TArray<FValue> Groups;
		for (const FAnimGroupInfo& Group : Blueprint.Groups)
		{
			Groups.Add(MakeStringValue(Group.Name.ToString()));
		}
		SetField(Out, TEXT("sync_groups"), MakeArrayValue(MoveTemp(Groups)));
		TArray<FAnimParentNodeAssetOverride> Overrides = Blueprint.ParentAssetOverrides;
		Overrides.Sort([](const FAnimParentNodeAssetOverride& A, const FAnimParentNodeAssetOverride& B)
		{
			return FormatAssetDocumentGraphGuid(A.ParentNodeGuid) < FormatAssetDocumentGraphGuid(B.ParentNodeGuid);
		});
		TArray<FValue> OverrideValues;
		for (const FAnimParentNodeAssetOverride& Override : Overrides)
		{
			FValue Entry = MakeObjectValue();
			SetField(Entry, TEXT("parent_node_guid"), MakeStringValue(FormatAssetDocumentGraphGuid(Override.ParentNodeGuid)));
			SetField(Entry, TEXT("asset"), Override.NewAsset != nullptr ? MakeStringValue(Override.NewAsset->GetPathName()) : FValue());
			OverrideValues.Add(MoveTemp(Entry));
		}
		SetField(Out, TEXT("parent_asset_overrides"), MakeArrayValue(MoveTemp(OverrideValues)));
		FValue Thumbnail;
		if (const USceneThumbnailInfo* Info = Cast<USceneThumbnailInfo>(Blueprint.ThumbnailInfo))
		{
			Thumbnail = MakeObjectValue();
			SetField(Thumbnail, TEXT("OrbitPitch"), MakeNumberValue(Info->OrbitPitch));
			SetField(Thumbnail, TEXT("OrbitYaw"), MakeNumberValue(Info->OrbitYaw));
			SetField(Thumbnail, TEXT("OrbitZoom"), MakeNumberValue(Info->OrbitZoom));
		}
		SetField(Out, TEXT("thumbnail"), MoveTemp(Thumbnail));
		SetField(Out, TEXT("is_template"), MakeBoolValue(Blueprint.bIsTemplate));
		SetField(Out, TEXT("blueprint_type"), EncodeMember(Blueprint, TEXT("BlueprintType")));
		return Out;
	}

	virtual bool SetSetting(UBlueprint& InBlueprint, FStringView Name, const FAssetDocumentRuntimeValueDto& Value,
		FAssetDocumentGraphError& OutError) const override
	{
		UAnimBlueprint& Blueprint = *CastChecked<UAnimBlueprint>(&InBlueprint);
		if (Name == TEXTVIEW("preview_mesh"))
		{
			USkeletalMesh* Mesh = nullptr;
			if (!LoadOptional(Value, Name, Mesh, OutError))
			{
				return false;
			}
			Blueprint.SetPreviewMesh(Mesh, /*bMarkAsDirty=*/true);
			return true;
		}
		if (Name == TEXTVIEW("preview_animation_blueprint"))
		{
			UAnimBlueprint* Preview = nullptr;
			if (!LoadOptional(Value, Name, Preview, OutError))
			{
				return false;
			}
			Blueprint.Modify();
			Blueprint.SetPreviewAnimationBlueprint(Preview);
			return true;
		}
		if (Name == TEXTVIEW("preview_animation_blueprint_application_method"))
		{
			const FEnumProperty* Property = FindFProperty<FEnumProperty>(UAnimBlueprint::StaticClass(), TEXT("PreviewAnimationBlueprintApplicationMethod"));
			const int64 Enumerator = Property != nullptr && Value.Kind == EAssetDocumentRuntimeValueKind::String
				? Property->GetEnum()->GetValueByNameString(Value.StringValue) : INDEX_NONE;
			if (Enumerator == INDEX_NONE)
			{
				OutError.Set(TEXT("BlueprintMember.InvalidValue"),
					TEXT("preview_animation_blueprint_application_method is LinkedLayers or LinkedAnimGraph."), TEXT("/value"));
				return false;
			}
			Blueprint.Modify();
			Blueprint.SetPreviewAnimationBlueprintApplicationMethod(static_cast<EPreviewAnimationBlueprintApplicationMethod>(Enumerator));
			return true;
		}
		if (Name == TEXTVIEW("preview_animation_blueprint_tag"))
		{
			if (Value.Kind != EAssetDocumentRuntimeValueKind::String)
			{
				OutError.Set(TEXT("BlueprintMember.InvalidValue"), TEXT("preview_animation_blueprint_tag is a name."), TEXT("/value"));
				return false;
			}
			Blueprint.Modify();
			Blueprint.SetPreviewAnimationBlueprintTag(FName(Value.StringValue));
			return true;
		}
		if (Name == TEXTVIEW("sync_groups"))
		{
			TArray<FName> Names;
			for (const FValue& Item : Value.Elements)
			{
				if (Item.Kind != EAssetDocumentRuntimeValueKind::String || Item.StringValue.IsEmpty() || Names.Contains(FName(Item.StringValue)))
				{
					OutError.Set(TEXT("BlueprintMember.InvalidValue"), TEXT("sync_groups is an array of unique, non-empty group names."), TEXT("/value"));
					return false;
				}
				Names.Add(FName(Item.StringValue));
			}
			if (Value.Kind != EAssetDocumentRuntimeValueKind::Array)
			{
				OutError.Set(TEXT("BlueprintMember.InvalidValue"), TEXT("sync_groups is an array of group names."), TEXT("/value"));
				return false;
			}
			Blueprint.Modify();
			TArray<FAnimGroupInfo> Groups;
			for (const FName Group : Names)
			{
				const FAnimGroupInfo* Existing = Blueprint.Groups.FindByPredicate([Group](const FAnimGroupInfo& Info) { return Info.Name == Group; });
				FAnimGroupInfo& Info = Groups.Add_GetRef(Existing != nullptr ? *Existing : FAnimGroupInfo());
				Info.Name = Group;
			}
			Blueprint.Groups = MoveTemp(Groups);
			return true;
		}
		if (Name == TEXTVIEW("parent_asset_overrides"))
		{
			if (Value.Kind != EAssetDocumentRuntimeValueKind::Array)
			{
				OutError.Set(TEXT("BlueprintMember.InvalidValue"), TEXT("parent_asset_overrides is an array of {parent_node_guid, asset}."), TEXT("/value"));
				return false;
			}
			TArray<FAnimParentNodeAssetOverride> Overrides;
			for (const FValue& Item : Value.Elements)
			{
				const FValue* Guid = FindValueField(Item, TEXT("parent_node_guid"));
				const FValue* Asset = FindValueField(Item, TEXT("asset"));
				FGuid NodeGuid;
				UAnimationAsset* NewAsset = nullptr;
				if (Guid == nullptr || Guid->Kind != EAssetDocumentRuntimeValueKind::String
					|| !FGuid::ParseExact(Guid->StringValue, EGuidFormats::Digits, NodeGuid)
					|| Overrides.ContainsByPredicate([NodeGuid](const FAnimParentNodeAssetOverride& O) { return O.ParentNodeGuid == NodeGuid; }))
				{
					OutError.Set(TEXT("BlueprintMember.InvalidValue"),
						TEXT("Each parent asset override needs a unique parent_node_guid (32 hex digits)."), TEXT("/value"));
					return false;
				}
				if (!LoadOptional(Asset != nullptr ? *Asset : FValue(), TEXTVIEW("parent_asset_overrides.asset"), NewAsset, OutError))
				{
					return false;
				}
				Overrides.Emplace(NodeGuid, NewAsset);
			}
			// SAnimBlueprintParentPlayerList: edit the override, then notify its listeners.
			Blueprint.Modify();
			Blueprint.ParentAssetOverrides = MoveTemp(Overrides);
			for (FAnimParentNodeAssetOverride& Override : Blueprint.ParentAssetOverrides)
			{
				Blueprint.NotifyOverrideChange(Override);
			}
			return true;
		}
		if (Name == TEXTVIEW("thumbnail"))
		{
			double Pitch = 0.0;
			double Yaw = 0.0;
			double Zoom = 0.0;
			const FValue* PitchValue = FindValueField(Value, TEXT("OrbitPitch"));
			const FValue* YawValue = FindValueField(Value, TEXT("OrbitYaw"));
			const FValue* ZoomValue = FindValueField(Value, TEXT("OrbitZoom"));
			if (PitchValue == nullptr || YawValue == nullptr || ZoomValue == nullptr || Value.ObjectKeys.Num() != 3
				|| !ReadNumber(*PitchValue, Pitch) || !ReadNumber(*YawValue, Yaw) || !ReadNumber(*ZoomValue, Zoom))
			{
				OutError.Set(TEXT("BlueprintMember.InvalidValue"), TEXT("thumbnail is {\"OrbitPitch\", \"OrbitYaw\", \"OrbitZoom\"} numbers."), TEXT("/value"));
				return false;
			}
			// FAssetTypeActions_AnimBlueprint::GetThumbnailInfo creates the scene thumbnail info on demand.
			Blueprint.Modify();
			USceneThumbnailInfo* Info = Cast<USceneThumbnailInfo>(Blueprint.ThumbnailInfo);
			if (Info == nullptr)
			{
				Info = NewObject<USceneThumbnailInfo>(&Blueprint, NAME_None, RF_Transactional);
				Blueprint.ThumbnailInfo = Info;
			}
			Info->Modify();
			Info->OrbitPitch = static_cast<float>(Pitch);
			Info->OrbitYaw = static_cast<float>(Yaw);
			Info->OrbitZoom = static_cast<float>(Zoom);
			return true;
		}
		if (Name == TEXTVIEW("is_template") || Name == TEXTVIEW("blueprint_type"))
		{
			OutError.Set(TEXT("BlueprintMember.InvalidValue"), FString::Printf(
				TEXT("%.*s is fixed when the Animation Blueprint is created."), Name.Len(), Name.GetData()), TEXT("/name"));
			return false;
		}
		OutError.Set(TEXT("BlueprintMember.SettingNotFound"), FString::Printf(
			TEXT("Unknown Animation Blueprint setting '%.*s'."), Name.Len(), Name.GetData()), TEXT("/name"));
		return false;
	}
};
} // namespace HostAdapterDetail

namespace HostAdapterDetail
{
// The graph module's Blueprint rebuilder restores Animation Blueprint packages; these package
// objects are compile output, regenerated from the authored graphs by every compile.
class FAnimBlueprintRebuildAdapter final : public IAssetDocumentBlueprintRebuildAdapter
{
public:
	virtual UClass* GetBlueprintClass() const override { return UAnimBlueprint::StaticClass(); }

	virtual bool IsDerivedObject(const UObject& Object) const override
	{
		for (const UObject* Current = &Object; Current != nullptr && !Current->IsA<UPackage>(); Current = Current->GetOuter())
		{
			// Anim Blueprint extensions: UAnimBlueprintExtension::RefreshExtensions creates and
			// drops them from the graph nodes before each compile.
			if (Current->IsA<UAnimBlueprintExtension>())
			{
				return true;
			}
			// Objects the anim compiler duplicates into the generated class, e.g. each Blend
			// Space Graph node's internal blend space ("<ABP>_C:BlendSpace_0").
			if (Current != &Object && Current->IsA<UAnimBlueprintGeneratedClass>())
			{
				return true;
			}
		}
		return false;
	}
};
} // namespace HostAdapterDetail

TSharedRef<IAssetDocumentBlueprintRebuildAdapter> MakeAnimBlueprintRebuildAdapter()
{
	return MakeShared<HostAdapterDetail::FAnimBlueprintRebuildAdapter>();
}

TSharedRef<IAssetDocumentGraphHostAdapter> MakeAnimBlueprintHostAdapter()
{
	return MakeShared<HostAdapterDetail::FAnimBlueprintHostAdapter>();
}

TSharedRef<IAssetDocumentBlueprintSettingsAdapter> MakeAnimBlueprintSettingsAdapter()
{
	return MakeShared<HostAdapterDetail::FAnimBlueprintSettingsAdapter>();
}
} // namespace UE::AssetDocument::AnimBlueprint
