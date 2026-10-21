// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentAssetFacts.h"
#include "AssetDocumentCompileStatus.h"
#include "AssetDocumentControlRig.h"
#include "AssetDocumentEditorDialogFactory.h"
#include "AssetDocumentIKRetargetBatch.h"
#include "AssetDocumentHttpServer.h"
#include "AssetDocumentExactBlueprintClasses.h"
#include "AssetDocumentRigOpenEditor.h"
#include "AssetDocumentRigPrimitives.h"
#include "AssetDocumentRuntimeModule.h"
#include "AssetDocumentTypeListers.h"
#include "ControlRigBlueprintLegacy.h"
#include "Misc/EngineVersionComparison.h"
#include "Modules/ModuleManager.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Retargeter/IKRetargeter.h"
#include "Runtime/AssetDocumentPackageSnapshot.h"

namespace AssetDocumentRigModule
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
FTopLevelAssetPath RetargetFactoryPath()
{
	return FTopLevelAssetPath(TEXT("/Script/IKRigEditor"), TEXT("IKRetargetFactory"));
}

// A created IK Retargeter starts with the editor's default op stack (the op stack's "Add Default Ops"). The
// retargeter factory here leaves the stack empty; a document that omits Ops keeps this stack, one that lists Ops
// states its own.
bool InitRetargeterWithDefaultOps(const UFactory& Factory, UObject& NewAsset, FString& OutMessage)
{
	UIKRetargeter* const Retargeter = Cast<UIKRetargeter>(&NewAsset);
	UIKRetargeterController* const Controller = Retargeter != nullptr ? UIKRetargeterController::GetController(Retargeter) : nullptr;
	if (Controller == nullptr)
	{
		OutMessage = TEXT("the IK Retargeter factory makes IK Retargeters only");
		return false;
	}
	Controller->AddDefaultOps();
	return true;
}
#endif
}

// The "rig" provider is assembled by the runtime bootstrap. The module registers the IK Rig / IK Retargeter
// status describer (processor initialization, design §5), the list_types kinds ikrig_solver and ikretarget_op,
// and the rollback rebuilder (the editors' reinitialization after Undo); for Control Rig the compile status
// describer (reads the last compile, never compiles), the list_types kinds rigvm_node / rigvm_type /
// controlrig_module, the ControlRigShapeLibrary Inspect facts, and ControlRigBlueprint as a Blueprint-derived class
// served by an exact profile (its documents resolve for Diff / Validate / Apply, and its editor factory creates it),
// plus the Control Rig rollback rebuilder (the editor's recompile after Undo), Validate's Control Rig trial target, the
// Control Rig duplicate fixup, the open-editor apply handlers of the three profiles (AssetDocumentRigOpenEditor.cpp),
// and, where the retargeter factory makes an empty op stack, the default op stack a created IK Retargeter starts with.
// The class-keyed registrations are by class path, taken at startup: on a normal editor exit the UObject exit purge runs
// before ShutdownModule, so ShutdownModule must not touch UControlRigBlueprint::StaticClass().
class FAssetDocumentRigModule final : public IAssetDocumentRuntimeModule
{
public:
	virtual void StartupModule() override
	{
		using namespace UE::AssetDocument::Graph::Rig;
		RegisterAssetDocumentCompileStatusDescriber(CompileDescriberId, &DescribeStatus);
		RegisterAssetDocumentTypeLister(TEXT("ikrig_solver"), &ListSolverTypes);
		RegisterAssetDocumentTypeLister(TEXT("ikretarget_op"), &ListOpTypes);
		RegisterAssetDocumentCompileStatusDescriber(ControlRigDescriberId, &ControlRig::DescribeStatus);
		RegisterAssetDocumentTypeLister(TEXT("rigvm_node"), &ControlRig::ListNodeTypes);
		RegisterAssetDocumentTypeLister(TEXT("rigvm_type"), &ControlRig::ListValueTypes);
		RegisterAssetDocumentTypeLister(TEXT("controlrig_module"), &ControlRig::ListModules);
		RegisterAssetDocumentAssetFactsDescriber(ShapeLibraryFactsKey, &ControlRig::DescribeShapeLibrary);
		ControlRigBlueprintPath = UControlRigBlueprint::StaticClass()->GetClassPathName();
		RegisterAssetDocumentExactBlueprintClass(ControlRigBlueprintPath);
		Rebuilder = MakeRigRebuilder();
		RegisterAssetDocumentDerivedStateRebuilder(Rebuilder.ToSharedRef());
		ControlRigRebuilder = ControlRig::MakeRebuilder();
		RegisterAssetDocumentDerivedStateRebuilder(ControlRigRebuilder.ToSharedRef());
		ControlRig::RegisterTrialTargetMaker(ControlRigBlueprintPath);
		RegisterAssetDocumentRigOpenEditorHandlers();
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
		RegisterAssetDocumentEditorDialogFactory(AssetDocumentRigModule::RetargetFactoryPath(), &AssetDocumentRigModule::InitRetargeterWithDefaultOps);
#endif
		FAssetDocumentExternalRoute Route;
		Route.Path = TEXT("/assetdoc/animation/retarget");
		Route.Handler = FAssetDocumentExternalRouteHandler::CreateStatic(&Batch::HandleRetargetRoute);
		Route.MethodText = TEXT("POST");
		Route.Description = TEXT("Duplicate and retarget animations through an IK Retargeter, as its Export window (retarget_animations)");
		Route.bRequiresBody = true;
		RetargetRoute = FAssetDocumentHttpServer::RegisterExternalRoute(Route);
	}

	virtual void ShutdownModule() override
	{
		if (RetargetRoute.IsValid())
		{
			FAssetDocumentHttpServer::UnregisterExternalRoute(RetargetRoute);
			RetargetRoute.Reset();
		}
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
		UnregisterAssetDocumentEditorDialogFactory(AssetDocumentRigModule::RetargetFactoryPath());
#endif
		UnregisterAssetDocumentRigOpenEditorHandlers();
		UE::AssetDocument::Graph::Rig::ControlRig::UnregisterTrialTargetMaker(ControlRigBlueprintPath);
		if (ControlRigRebuilder.IsValid())
		{
			UnregisterAssetDocumentDerivedStateRebuilder(ControlRigRebuilder->GetId());
			ControlRigRebuilder.Reset();
		}
		if (Rebuilder.IsValid())
		{
			UnregisterAssetDocumentDerivedStateRebuilder(Rebuilder->GetId());
			Rebuilder.Reset();
		}
		UnregisterAssetDocumentExactBlueprintClass(ControlRigBlueprintPath);
		UnregisterAssetDocumentAssetFactsDescriber(ShapeLibraryFactsKey);
		UnregisterAssetDocumentTypeLister(TEXT("controlrig_module"));
		UnregisterAssetDocumentTypeLister(TEXT("rigvm_type"));
		UnregisterAssetDocumentTypeLister(TEXT("rigvm_node"));
		UnregisterAssetDocumentCompileStatusDescriber(ControlRigDescriberId);
		UnregisterAssetDocumentTypeLister(TEXT("ikretarget_op"));
		UnregisterAssetDocumentTypeLister(TEXT("ikrig_solver"));
		UnregisterAssetDocumentCompileStatusDescriber(CompileDescriberId);
	}

	virtual FName GetRuntimeModuleName() const override { return TEXT("AssetDocumentRig"); }

private:
	TSharedPtr<IAssetDocumentDerivedStateRebuilder> Rebuilder;
	TSharedPtr<IAssetDocumentDerivedStateRebuilder> ControlRigRebuilder;
	FTopLevelAssetPath ControlRigBlueprintPath;
	FDelegateHandle RetargetRoute;
	static inline const FName CompileDescriberId = TEXT("AssetDocumentRig.IKRig");
	static inline const FName ControlRigDescriberId = TEXT("AssetDocumentRig.ControlRig");
	static inline const FName ShapeLibraryFactsKey = TEXT("ShapeLibrary");
};

IMPLEMENT_MODULE(FAssetDocumentRigModule, AssetDocumentRig)
