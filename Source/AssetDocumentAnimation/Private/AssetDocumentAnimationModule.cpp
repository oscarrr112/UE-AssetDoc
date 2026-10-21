#include "AnimAssets/AssetDocumentAnimAssetOpenEditor.h"
#include "AnimBlueprint/AssetDocumentAnimBlueprintAdapters.h"
#include "AnimData/AnimationBoneSampling.h"
#include "AssetDocumentAssetFacts.h"
#include "AssetDocumentHttpServer.h"
#include "AnimationConduitGraphSchema.h"
#include "AnimationGraphSchema.h"
#include "AnimationTransitionSchema.h"
#include "AssetDocumentRuntimeModule.h"
#include "Modules/ModuleManager.h"

class FAssetDocumentAnimationModule final : public IAssetDocumentRuntimeModule
{
public:
	void StartupModule() override
	{
		check(!HasActiveRuntimeRegistration());
		// Animation Blueprint adapters of the generic graph provider (contract §5).
		using namespace UE::AssetDocument::AnimBlueprint;
		SchemaAdapters = {
			MakeAnimGraphSchemaAdapter(UAnimationGraphSchema::StaticClass()),
			MakeAnimGraphSchemaAdapter(UAnimationTransitionSchema::StaticClass()),
			MakeAnimGraphSchemaAdapter(UAnimationConduitGraphSchema::StaticClass()),
			MakeStateMachineSchemaAdapter()};
		for (const TSharedRef<IAssetDocumentGraphSchemaAdapter>& Adapter : SchemaAdapters)
		{
			RegisterAssetDocumentGraphSchemaAdapter(Adapter);
		}
		HostAdapter = MakeAnimBlueprintHostAdapter();
		RegisterAssetDocumentGraphHostAdapter(HostAdapter.ToSharedRef());
		SettingsAdapter = MakeAnimBlueprintSettingsAdapter();
		RegisterAssetDocumentBlueprintSettingsAdapter(SettingsAdapter.ToSharedRef());
		RebuildAdapter = MakeAnimBlueprintRebuildAdapter();
		RegisterAssetDocumentBlueprintRebuildAdapter(RebuildAdapter.ToSharedRef());
		// Animation data (read-only): Inspect's Motion facts of an AnimSequence, and sample_animation_bones.
		RegisterAssetDocumentAssetFactsDescriber(MotionFactsKey, &UE::AssetDocument::AnimData::DescribeMotion);
		FAssetDocumentExternalRoute Route;
		Route.Path = TEXT("/assetdoc/animation/sample-bones");
		Route.Handler = FAssetDocumentExternalRouteHandler::CreateStatic(&UE::AssetDocument::AnimData::HandleSampleBonesRoute);
		Route.MethodText = TEXT("POST");
		Route.Description = TEXT("Bone and socket transforms of an animation at given times (sample_animation_bones)");
		Route.bRequiresBody = true;
		SampleRoute = FAssetDocumentHttpServer::RegisterExternalRoute(Route);
		// Open-editor apply: refresh an open Persona editor (animation assets, Skeleton) after an apply.
		RegisterAssetDocumentAnimAssetOpenEditorHandlers();
	}

	void ShutdownModule() override
	{
		UnregisterAssetDocumentAnimAssetOpenEditorHandlers();
		if (SampleRoute.IsValid())
		{
			FAssetDocumentHttpServer::UnregisterExternalRoute(SampleRoute);
			SampleRoute.Reset();
		}
		UnregisterAssetDocumentAssetFactsDescriber(MotionFactsKey);
		if (RebuildAdapter.IsValid())
		{
			UnregisterAssetDocumentBlueprintRebuildAdapter(RebuildAdapter.ToSharedRef());
		}
		if (SettingsAdapter.IsValid())
		{
			UnregisterAssetDocumentBlueprintSettingsAdapter(SettingsAdapter.ToSharedRef());
		}
		if (HostAdapter.IsValid())
		{
			UnregisterAssetDocumentGraphHostAdapter(HostAdapter.ToSharedRef());
		}
		for (const TSharedRef<IAssetDocumentGraphSchemaAdapter>& Adapter : SchemaAdapters)
		{
			UnregisterAssetDocumentGraphSchemaAdapter(Adapter);
		}
		SchemaAdapters.Reset();
	}

	FName GetRuntimeModuleName() const override { return TEXT("AssetDocumentAnimation"); }

private:
	TArray<TSharedRef<IAssetDocumentGraphSchemaAdapter>> SchemaAdapters;
	TSharedPtr<IAssetDocumentGraphHostAdapter> HostAdapter;
	TSharedPtr<IAssetDocumentBlueprintSettingsAdapter> SettingsAdapter;
	TSharedPtr<IAssetDocumentBlueprintRebuildAdapter> RebuildAdapter;
	FDelegateHandle SampleRoute;
	static inline const FName MotionFactsKey = TEXT("Motion");
};

IMPLEMENT_MODULE(FAssetDocumentAnimationModule, AssetDocumentAnimation)
