// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#if WITH_DEV_AUTOMATION_TESTS

#include "Tests/Fixtures/AssetDocumentAITestFixture.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTreeEditorModule.h"
#include "BehaviorTreeFactory.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "Editor.h"
#include "IBehaviorTreeEditor.h"
#include "IDetailTreeNode.h"
#include "IPropertyRowGenerator.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace AssetDocumentAITestFixture
{
TSharedPtr<IPropertyHandle> FindPropertyHandle(
	const TArray<TSharedRef<IDetailTreeNode>>& Nodes,
	const FName PropertyName)
{
	for (const TSharedRef<IDetailTreeNode>& Node : Nodes)
	{
		const TSharedPtr<IPropertyHandle> Handle = Node->CreatePropertyHandle();
		if (Handle.IsValid() && Handle->GetProperty()
			&& Handle->GetProperty()->GetFName() == PropertyName)
		{
			return Handle;
		}
		TArray<TSharedRef<IDetailTreeNode>> Children;
		Node->GetChildren(Children, true);
		if (const TSharedPtr<IPropertyHandle> Child = FindPropertyHandle(Children, PropertyName))
		{
			return Child;
		}
	}
	return nullptr;
}

UBehaviorTree* CreateTransientBehaviorTree(const TCHAR* BaseName)
{
	UBehaviorTreeFactory* Factory = UBehaviorTreeFactory::StaticClass()
		->GetDefaultObject<UBehaviorTreeFactory>();
	if (!Factory)
	{
		return nullptr;
	}
	const FName UniqueName = MakeUniqueObjectName(
		GetTransientPackage(), UBehaviorTree::StaticClass(), FName(BaseName));
	return Cast<UBehaviorTree>(Factory->FactoryCreateNew(
		UBehaviorTree::StaticClass(),
		GetTransientPackage(),
		UniqueName,
		RF_Transient | RF_Transactional,
		nullptr,
		GWarn));
}
} // namespace AssetDocumentAITestFixture

bool FAssetDocumentAITestFixture::CreateFactoryOwnedScratchBundle(FAutomationTestBase& Test)
{
	using namespace AssetDocumentAITestFixture;
	ScratchTree.Reset(CreateTransientBehaviorTree(TEXT("AssetDocumentAIT0ScratchTree")));
	if (!Test.TestNotNull(TEXT("FactoryCreateNew returned an exact BehaviorTree"), ScratchTree.Get()))
	{
		return false;
	}
	Test.TestTrue(TEXT("scratch tree exact class"), ScratchTree->GetClass() == UBehaviorTree::StaticClass());
	Test.TestTrue(TEXT("scratch tree outer is transient package"), ScratchTree->GetOuter() == GetTransientPackage());
	Test.TestTrue(TEXT("scratch tree is transient"), ScratchTree->HasAnyFlags(RF_Transient));
	Test.TestTrue(TEXT("scratch tree is transactional"), ScratchTree->HasAnyFlags(RF_Transactional));
	Test.TestFalse(TEXT("scratch tree is not a durable asset"), ScratchTree->IsAsset());
	Test.TestNull(TEXT("factory does not install a graph"), ScratchTree->BTGraph);

	UEdGraph* CreatedGraph = FBlueprintEditorUtils::CreateNewGraph(
		ScratchTree.Get(),
		MakeUniqueObjectName(ScratchTree.Get(), UBehaviorTreeGraph::StaticClass(), TEXT("AssetDocumentAIT0Graph")),
		UBehaviorTreeGraph::StaticClass(),
		UEdGraphSchema_BehaviorTree::StaticClass());
	ScratchGraph.Reset(Cast<UBehaviorTreeGraph>(CreatedGraph));
	if (!Test.TestNotNull(TEXT("CreateNewGraph returned an exact BehaviorTreeGraph"), ScratchGraph.Get()))
	{
		return false;
	}
	ScratchGraph->SetFlags(RF_Transient);
	Test.TestTrue(TEXT("scratch graph outer is scratch tree"), ScratchGraph->GetOuter() == ScratchTree.Get());
	Test.TestTrue(TEXT("scratch graph is transient"), ScratchGraph->HasAnyFlags(RF_Transient));
	Test.TestNull(TEXT("graph remains uninstalled"), ScratchTree->BTGraph);

	const UEdGraphSchema_BehaviorTree* Schema =
		GetDefault<UEdGraphSchema_BehaviorTree>();
	Schema->CreateDefaultNodesForGraph(*ScratchGraph);
	TArray<UBehaviorTreeGraphNode_Root*> Roots;
	for (UEdGraphNode* Node : ScratchGraph->Nodes)
	{
		if (UBehaviorTreeGraphNode_Root* Root = Cast<UBehaviorTreeGraphNode_Root>(Node))
		{
			Roots.Add(Root);
		}
	}
	Test.TestEqual(TEXT("defaults create exactly one root"), Roots.Num(), 1);
	if (Roots.Num() != 1)
	{
		return false;
	}
	RootNode.Reset(Roots[0]);
	// Stock PostPlacedNewNode may select any loaded Blackboard. This fixture
	// starts from an explicitly empty selector, independent of test execution order.
	RootNode->BlackboardAsset = nullptr;
	RootNode->UpdateBlackboard();
	Test.TestTrue(TEXT("root belongs to scratch graph"), RootNode->GetGraph() == ScratchGraph.Get());
	Test.TestNull(TEXT("defaults do not install scratch graph"), ScratchTree->BTGraph);
	return true;
}

bool FAssetDocumentAITestFixture::RequireUniqueRootPropertyHandle(FAutomationTestBase& Test)
{
	if (!RootNode.IsValid())
	{
		Test.AddError(TEXT("Scratch root must exist before requesting its typed property handle."));
		return false;
	}
	FPropertyRowGeneratorArgs Args;
	FPropertyEditorModule& PropertyEditor =
		FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	PropertyRows = PropertyEditor.CreatePropertyRowGenerator(Args);
	PropertyRows->SetObjects({RootNode.Get()});
	BlackboardAssetHandle = AssetDocumentAITestFixture::FindPropertyHandle(
		PropertyRows->GetRootTreeNodes(), GET_MEMBER_NAME_CHECKED(UBehaviorTreeGraphNode_Root, BlackboardAsset));
	if (!Test.TestTrue(TEXT("unique root BlackboardAsset has a typed property handle"), BlackboardAssetHandle.IsValid()))
	{
		return false;
	}
	Test.TestEqual(TEXT("typed handle owns BlackboardAsset property"),
		BlackboardAssetHandle->GetProperty()->GetFName(),
		GET_MEMBER_NAME_CHECKED(UBehaviorTreeGraphNode_Root, BlackboardAsset));
	UObject* Current = nullptr;
	Test.TestEqual(TEXT("typed root property is readable"),
		BlackboardAssetHandle->GetValue(Current), FPropertyAccess::Success);
	Test.TestNull(TEXT("fresh root BlackboardAsset is null"), Current);
	return true;
}

bool FAssetDocumentAITestFixture::BootstrapBehaviorTreeEditorCache(FAutomationTestBase& Test)
{
	using namespace AssetDocumentAITestFixture;
	if (!IsInGameThread() || !GIsEditor || IsRunningCommandlet() || !GEditor)
	{
		Test.AddError(TEXT("BehaviorTree Editor authority requires a regular Editor GameThread context."));
		return false;
	}
	UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!Test.TestNotNull(TEXT("AssetEditorSubsystem is available"), Subsystem))
	{
		return false;
	}
	EditedAssetsBefore = Subsystem->GetAllEditedAssets();
	OpenEditorCountBefore = Subsystem->GetAllOpenEditors().Num();

	FBehaviorTreeEditorModule* Module =
		FModuleManager::LoadModulePtr<FBehaviorTreeEditorModule>(TEXT("BehaviorTreeEditor"));
	if (!Test.TestNotNull(TEXT("BehaviorTreeEditor module is public and loadable"), Module))
	{
		return false;
	}
	if (!Module->GetClassCache().IsValid())
	{
		BootstrapTree.Reset(CreateTransientBehaviorTree(TEXT("AssetDocumentAIT0BootstrapTree")));
		if (!Test.TestNotNull(TEXT("bootstrap factory tree exists"), BootstrapTree.Get()))
		{
			return false;
		}
		Toolkit = Module->CreateBehaviorTreeEditor(
			EToolkitMode::Standalone, nullptr, BootstrapTree.Get());
	}
	Test.TestTrue(TEXT("CreateBehaviorTreeEditor initializes the public class cache"),
		Module->GetClassCache().IsValid());
	const TSharedPtr<FGraphNodeClassHelper> FirstCache = Module->GetClassCache();
	Test.TestEqual(TEXT("public class cache identity is stable within readiness"),
		Module->GetClassCache().Get(), FirstCache.Get());
	return Module->GetClassCache().IsValid();
}

bool FAssetDocumentAITestFixture::CloseBehaviorTreeEditorToolkit(FAutomationTestBase& Test)
{
	if (!GEditor)
	{
		return false;
	}
	UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!Subsystem)
	{
		return false;
	}
	if (Toolkit.IsValid())
	{
		Toolkit->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed);
		Toolkit.Reset();
	}
	if (BootstrapTree.IsValid())
	{
		Test.TestTrue(TEXT("bootstrap editor registration is released"),
			Subsystem->FindEditorsForAsset(BootstrapTree.Get()).IsEmpty());
	}
	Test.TestEqual(TEXT("open editor count returns to baseline"),
		Subsystem->GetAllOpenEditors().Num(), OpenEditorCountBefore);
	Test.TestEqual(TEXT("edited asset count returns to baseline"),
		Subsystem->GetAllEditedAssets().Num(), EditedAssetsBefore.Num());
	return Subsystem->GetAllOpenEditors().Num() == OpenEditorCountBefore
		&& Subsystem->GetAllEditedAssets().Num() == EditedAssetsBefore.Num();
}

#endif // WITH_DEV_AUTOMATION_TESTS
