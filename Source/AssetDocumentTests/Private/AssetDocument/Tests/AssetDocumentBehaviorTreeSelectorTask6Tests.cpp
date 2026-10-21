// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentModule.h"
#include "AssetDocumentEditorSync.h"
#include "AssetDocumentRuntimeModule.h"
#include "AssetDocumentService.h"
#include "AssetDocumentJsonRegionUtils.h"
#include "AssetDocumentPropertyAdapter.h"
#include "AssetDocumentSidecar.h"
#include "Tests/AssetDocumentReflectedPropertyTestTypes.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AISystem.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/Decorators/BTDecorator_Blackboard.h"
#include "BehaviorTree/Services/BTService_DefaultFocus.h"
#include "BehaviorTree/Tasks/BTTask_MoveTo.h"
#include "BehaviorTree/Tasks/BTTask_RunBehavior.h"
#include "BehaviorTree/Tasks/BTTask_RunBehaviorDynamic.h"
#include "BehaviorTree/Tasks/BTTask_Wait.h"
#include "BehaviorTree/Tasks/BTTask_WaitBlackboardTime.h"
#include "BehaviorTree/ValueOrBBKey.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "HAL/FileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "PackageTools.h"
#include "UObject/SavePackage.h"
#include "Tests/AssetDocumentBehaviorTreeDurableDependencies.h"
#include "UObject/UnrealType.h"
#include "Utils/AssetDocumentEngineCompat.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentBehaviorTreeSelectorTask6Tests
{
constexpr const TCHAR* GraphGuid = TEXT("60606060111111112222222233333333");
constexpr const TCHAR* RootGuid = TEXT("60606060444444445555555566666666");
constexpr const TCHAR* TaskGuid = TEXT("60606060777777778888888899999999");
constexpr const TCHAR* ServiceGuid = TEXT("60606060AAAAAAAABBBBBBBBCCCCCCCC");
constexpr const TCHAR* DecoratorGuid = TEXT("60606060DDDDDDDDEEEEEEEEFFFFFFFF");
constexpr const TCHAR* CompositeGuid = TEXT("61616161111111112222222233333333");
constexpr const TCHAR* CompositeGraphGuid = TEXT("61616161444444445555555566666666");
constexpr const TCHAR* CompositeSinkGuid = TEXT("61616161777777778888888899999999");
constexpr const TCHAR* CompositeTestGuid = TEXT("61616161AAAAAAAABBBBBBBBCCCCCCCC");

FString UniqueTarget(const TCHAR* Stem)
{
	return FString::Printf(
		TEXT("/Game/AssetDocumentTests/Task6/%s_%s"),
		Stem,
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

FString ObjectPath(const FString& PackagePath)
{
	return FString::Printf(TEXT("%s.%s"), *PackagePath, *FPackageName::GetLongPackageAssetName(PackagePath));
}

TSharedPtr<FJsonValue> ObjectValue(const TSharedPtr<FJsonObject>& Object)
{
	return MakeShared<FJsonValueObject>(Object.ToSharedRef());
}

TSharedPtr<FJsonObject> AssetRef(const UObject* Asset)
{
	TSharedPtr<FJsonObject> Ref = MakeShared<FJsonObject>();
	Ref->SetStringField(TEXT("Kind"), TEXT("AssetRef"));
	Ref->SetStringField(TEXT("Path"), Asset ? Asset->GetPathName() : FString());
	return Ref;
}

TSharedPtr<FJsonObject> Selector(const FString& Key, const FString& DerivedField = FString())
{
	TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
	Value->SetStringField(TEXT("Key"), Key);
	if (DerivedField == TEXT("AllowedTypes"))
	{
		Value->SetArrayField(DerivedField, {});
	}
	else if (DerivedField == TEXT("SelectedKeyID"))
	{
		Value->SetNumberField(DerivedField, 7);
	}
	else if (DerivedField == TEXT("bNoneIsAllowedValue"))
	{
		Value->SetBoolField(DerivedField, true);
	}
	else if (!DerivedField.IsEmpty())
	{
		Value->SetStringField(DerivedField, TEXT("derived"));
	}
	return Value;
}

TSharedPtr<FJsonObject> ValueOrFloat(const FString& Key, float DefaultValue)
{
	TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
	Value->SetStringField(TEXT("Key"), Key);
	Value->SetNumberField(TEXT("DefaultValue"), DefaultValue);
	return Value;
}

TSharedPtr<FJsonObject> Node(
	const FString& Id,
	const FString& ClassPath,
	const TSharedPtr<FJsonObject>& Properties = MakeShared<FJsonObject>())
{
	TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
	Value->SetStringField(TEXT("Id"), Id);
	Value->SetStringField(TEXT("Class"), ClassPath);
	Value->SetObjectField(TEXT("Properties"), Properties);
	Value->SetArrayField(TEXT("Decorators"), {});
	Value->SetArrayField(TEXT("Services"), {});
	Value->SetArrayField(TEXT("Children"), {});
	return Value;
}

TSharedPtr<FJsonObject> TreeWithTask(
	const TSharedPtr<FJsonObject>& TaskProperties,
	const FString& TaskClass = UAssetDocumentSelectorTaskTestNode::StaticClass()->GetPathName())
{
	TSharedPtr<FJsonObject> Root = Node(RootGuid, TEXT("/Script/AIModule.BTComposite_Selector"));
	Root->SetArrayField(TEXT("Children"), {ObjectValue(Node(TaskGuid, TaskClass, TaskProperties))});

	TSharedPtr<FJsonObject> Tree = MakeShared<FJsonObject>();
	Tree->SetStringField(TEXT("GraphGuid"), GraphGuid);
	Tree->SetObjectField(TEXT("Root"), Root);
	Tree->SetArrayField(TEXT("Comments"), {});
	return Tree;
}

TSharedPtr<FJsonObject> Body(
	const TSharedPtr<FJsonObject>& Tree,
	const TSharedPtr<FJsonValue>& BlackboardValue,
	bool bIncludeBlackboard = true)
{
	TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
	if (bIncludeBlackboard)
	{
		Value->SetField(TEXT("BlackboardAsset"), BlackboardValue.IsValid() ? BlackboardValue : MakeShared<FJsonValueNull>());
	}
	Value->SetObjectField(TEXT("Tree"), Tree);
	return Value;
}

TSharedPtr<FJsonObject> Document(
	const FString& Target,
	const TSharedPtr<FJsonObject>& BodyValue,
	const FString& Action = TEXT("CreateOrUpdate"))
{
	TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
	Value->SetNumberField(TEXT("SchemaVersion"), 1);
	Value->SetStringField(TEXT("AssetType"), TEXT("GenericAsset"));
	Value->SetStringField(TEXT("Target"), Target);
	Value->SetStringField(TEXT("Class"), TEXT("/Script/AIModule.BehaviorTree"));
	Value->SetStringField(TEXT("Action"), Action);
	Value->SetObjectField(TEXT("Properties"), MakeShared<FJsonObject>());
	Value->SetObjectField(TEXT("Body"), BodyValue);
	return Value;
}

FAssetDocumentResult Validate(FAssetDocumentService& Service, const TSharedPtr<FJsonObject>& Value)
{
	FAssetDocumentValidateRequest Request;
	Request.Document = Value;
	return Service.Validate(Request);
}

FAssetDocumentResult Diff(FAssetDocumentService& Service, const TSharedPtr<FJsonObject>& Value)
{
	FAssetDocumentDiffRequest Request;
	Request.Document = Value;
	return Service.Diff(Request);
}

FAssetDocumentResult Apply(FAssetDocumentService& Service, const TSharedPtr<FJsonObject>& Value, bool bRetainDurablePair = false)
{
	FScopedBehaviorTreeDurableDependencies Dependencies;
	if (!Dependencies.Prepare(Value))
		return FAssetDocumentResult::Failure(TEXT("ApplyFile fixture could not save its BehaviorTree dependencies"));
	if (!Value.IsValid())
	{
		return FAssetDocumentResult::Failure(TEXT("Apply requires a valid document"));
	}

	FString Target;
	if (!Value->TryGetStringField(TEXT("Target"), Target) || Target.IsEmpty())
	{
		return FAssetDocumentResult::Failure(TEXT("Apply requires a non-empty Target"));
	}

	const FString SidecarPath = FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(Target);
	const FString SidecarDirectory = FPaths::GetPath(SidecarPath);
	const FString PackageFileName = FPackageName::LongPackageNameToFilename(Target, FPackageName::GetAssetPackageExtension());
	if (SidecarPath.IsEmpty() || SidecarDirectory.IsEmpty() || !IFileManager::Get().MakeDirectory(*SidecarDirectory, true))
	{
		return FAssetDocumentResult::Failure(TEXT("Apply failed to create canonical sidecar directory"));
	}

	FString WriteError;
	if (!FAssetDocumentSidecar::WriteJsonFile(SidecarPath, Value, WriteError))
	{
		return FAssetDocumentResult::Failure(FString::Printf(
			TEXT("Apply failed to write canonical sidecar: %s"),
			*WriteError));
	}

	ON_SCOPE_EXIT
	{
		if (!bRetainDurablePair)
		{
			FAssetDocumentEditorSync::FScopedSidecarWrite Guard(SidecarPath);
			FScopedBehaviorTreeDurableDependencies::FlushFileEvents();
			IFileManager::Get().Delete(*SidecarPath, false, true);
			IFileManager::Get().Delete(*PackageFileName, false, true);
			IFileManager::Get().DeleteDirectory(*SidecarDirectory, false, false);
			FScopedBehaviorTreeDurableDependencies::FlushFileEvents();
		}
	};

	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = SidecarPath;
	Request.bSaveAsset = true;
	FAssetDocumentResult Result = Service.ApplyFile(Request);
	if (!Result.IsSuccess())
		for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
			UE_LOG(LogTemp, Display, TEXT("BT ApplyFile fixture failure: [%s %s: %s]"),
				*Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message);
	return Result;
}

bool HasDiagnostic(const FAssetDocumentResult& Result, const FString& Path, const FString& Code)
{
	return Result.Diagnostics.ContainsByPredicate([&Path, &Code](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Path == Path && Diagnostic.Code == Code;
	});
}

bool HasEmptyPayloadArray(const FAssetDocumentResult& Result, const FString& FieldName)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	return Result.Payload.IsValid()
		&& Result.Payload->TryGetArrayField(FieldName, Values)
		&& Values
		&& Values->IsEmpty();
}

FString DescribeResult(const FAssetDocumentResult& Result)
{
	FString Description = Result.Message;
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Description += FString::Printf(
			TEXT(" [%s %s: %s]"),
			*Diagnostic.Code,
			*Diagnostic.Path,
			*Diagnostic.Message);
	}
	return Description;
}

template <typename TKeyType>
void AddKey(UBlackboardData* Blackboard, FName Name)
{
	FBlackboardEntry Entry;
	Entry.EntryName = Name;
	Entry.KeyType = NewObject<TKeyType>(Blackboard);
	Blackboard->Keys.Add(Entry);
}

void AddObjectKey(UBlackboardData* Blackboard, FName Name, UClass* BaseClass)
{
	FBlackboardEntry Entry;
	Entry.EntryName = Name;
	UBlackboardKeyType_Object* KeyType = NewObject<UBlackboardKeyType_Object>(Blackboard);
	KeyType->BaseClass = BaseClass;
	Entry.KeyType = KeyType;
	Blackboard->Keys.Add(Entry);
}

void AddEnumKey(UBlackboardData* Blackboard, FName Name, UEnum* EnumType)
{
	FBlackboardEntry Entry;
	Entry.EntryName = Name;
	UBlackboardKeyType_Enum* KeyType = NewObject<UBlackboardKeyType_Enum>(Blackboard);
	KeyType->EnumType = EnumType;
	Entry.KeyType = KeyType;
	Blackboard->Keys.Add(Entry);
}

UEnum* TestEnum(
	const FString& Target,
	int64 SharedValue,
	const FName SharedName = TEXT("SharedValue"))
{
	UPackage* Package = CreatePackage(*Target);
	UEnum* Enum = NewObject<UEnum>(
		Package,
		*FPackageName::GetLongPackageAssetName(Target),
		RF_Public | RF_Standalone | RF_Transactional);
	TArray<TPair<FName, int64>> Names{
		{SharedName, SharedValue},
		{TEXT("OtherValue"), SharedValue + 1},
	};
	return Enum && AssetDocumentEnums::SetEnums(*Enum, Names, UEnum::ECppForm::EnumClass, EEnumFlags::None, false)
		? Enum
		: nullptr;
}

UWorld* FindAutomationWorldWithAISystem()
{
	if (!GEngine)
	{
		return nullptr;
	}
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		UWorld* World = Context.World();
		if (World && UAISystem::GetCurrentSafe(World))
		{
			return World;
		}
	}
	return nullptr;
}

UBlackboardData* Blackboard(const FString& Target)
{
	return NewObject<UBlackboardData>(
		CreatePackage(*Target),
		*FPackageName::GetLongPackageAssetName(Target),
		RF_Public | RF_Standalone | RF_Transactional);
}

UBehaviorTree* BehaviorTree(const FString& Target, UBlackboardData* BlackboardAsset = nullptr)
{
	UBehaviorTree* Tree = NewObject<UBehaviorTree>(
		CreatePackage(*Target),
		*FPackageName::GetLongPackageAssetName(Target),
		RF_Public | RF_Standalone | RF_Transactional);
	Tree->BlackboardAsset = BlackboardAsset;
	return Tree;
}

TSharedPtr<FJsonObject> NestedSelectorValue(const FString& DerivedField)
{
	TSharedPtr<FJsonObject> Nested = MakeShared<FJsonObject>();
	Nested->SetStringField(TEXT("Identity"), TEXT("Nested"));
	Nested->SetObjectField(TEXT("Selector"), Selector(TEXT("TargetActor"), DerivedField));
	return Nested;
}

enum class EDerivedSelectorLocation : uint8
{
	Direct,
	Service,
	Decorator,
	CompositeDecorator,
	NestedContainer,
	InstancedStruct,
};

TSharedPtr<FJsonObject> DerivedFieldTree(EDerivedSelectorLocation Location, const FString& DerivedField, FString& OutPath)
{
	TSharedPtr<FJsonObject> TaskProperties = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Tree = TreeWithTask(TaskProperties);
	TSharedPtr<FJsonObject> Root = Tree->GetObjectField(TEXT("Root"));
	TSharedPtr<FJsonObject> Task = Root->GetArrayField(TEXT("Children"))[0]->AsObject();
	if (Location == EDerivedSelectorLocation::Direct)
	{
		TaskProperties->SetObjectField(TEXT("DirectSelector"), Selector(TEXT("TargetActor"), DerivedField));
		OutPath = FString::Printf(TEXT("/Body/Tree/Root/Children/0/Properties/DirectSelector/%s"), *DerivedField);
	}
	else if (Location == EDerivedSelectorLocation::NestedContainer)
	{
		TaskProperties->SetArrayField(TEXT("ArrayValues"), {ObjectValue(NestedSelectorValue(DerivedField))});
		OutPath = FString::Printf(TEXT("/Body/Tree/Root/Children/0/Properties/ArrayValues/0/Selector/%s"), *DerivedField);
	}
	else if (Location == EDerivedSelectorLocation::InstancedStruct)
	{
		TSharedPtr<FJsonObject> Dynamic = MakeShared<FJsonObject>();
		Dynamic->SetStringField(TEXT("Struct"), FAssetDocumentReflectedPropertyNestedTestValue::StaticStruct()->GetPathName());
		Dynamic->SetObjectField(TEXT("Properties"), NestedSelectorValue(DerivedField));
		TaskProperties->SetObjectField(TEXT("DynamicValue"), Dynamic);
		OutPath = FString::Printf(TEXT("/Body/Tree/Root/Children/0/Properties/DynamicValue/Properties/Selector/%s"), *DerivedField);
	}
	else if (Location == EDerivedSelectorLocation::Service)
	{
		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		Properties->SetObjectField(TEXT("BlackboardKey"), Selector(TEXT("TargetActor"), DerivedField));
		TSharedPtr<FJsonObject> Service = MakeShared<FJsonObject>();
		Service->SetStringField(TEXT("Id"), ServiceGuid);
		Service->SetStringField(TEXT("Class"), UBTService_DefaultFocus::StaticClass()->GetPathName());
		Service->SetObjectField(TEXT("Properties"), Properties);
		Root->SetArrayField(TEXT("Services"), {ObjectValue(Service)});
		OutPath = FString::Printf(TEXT("/Body/Tree/Root/Services/0/Properties/BlackboardKey/%s"), *DerivedField);
	}
	else if (Location == EDerivedSelectorLocation::Decorator)
	{
		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		Properties->SetObjectField(TEXT("BlackboardKey"), Selector(TEXT("TargetActor"), DerivedField));
		TSharedPtr<FJsonObject> Decorator = MakeShared<FJsonObject>();
		Decorator->SetStringField(TEXT("Id"), DecoratorGuid);
		Decorator->SetStringField(TEXT("Class"), UBTDecorator_Blackboard::StaticClass()->GetPathName());
		Decorator->SetObjectField(TEXT("Properties"), Properties);
		Task->SetArrayField(TEXT("Decorators"), {ObjectValue(Decorator)});
		OutPath = FString::Printf(TEXT("/Body/Tree/Root/Children/0/Decorators/0/Properties/BlackboardKey/%s"), *DerivedField);
	}
	else
	{
		TSharedPtr<FJsonObject> TestProperties = MakeShared<FJsonObject>();
		TestProperties->SetObjectField(TEXT("BlackboardKey"), Selector(TEXT("TargetActor"), DerivedField));
		TSharedPtr<FJsonObject> Sink = MakeShared<FJsonObject>();
		Sink->SetStringField(TEXT("Id"), CompositeSinkGuid);
		Sink->SetStringField(TEXT("Kind"), TEXT("Sink"));
		TSharedPtr<FJsonObject> Test = MakeShared<FJsonObject>();
		Test->SetStringField(TEXT("Id"), CompositeTestGuid);
		Test->SetStringField(TEXT("Kind"), TEXT("Test"));
		Test->SetStringField(TEXT("Class"), UBTDecorator_Blackboard::StaticClass()->GetPathName());
		Test->SetObjectField(TEXT("Properties"), TestProperties);
		TSharedPtr<FJsonObject> Link = MakeShared<FJsonObject>();
		Link->SetStringField(TEXT("From"), CompositeTestGuid);
		Link->SetStringField(TEXT("To"), CompositeSinkGuid);
		Link->SetNumberField(TEXT("ToInput"), 0);
		TSharedPtr<FJsonObject> BoundGraph = MakeShared<FJsonObject>();
		BoundGraph->SetStringField(TEXT("GraphGuid"), CompositeGraphGuid);
		BoundGraph->SetArrayField(TEXT("Nodes"), {ObjectValue(Sink), ObjectValue(Test)});
		BoundGraph->SetArrayField(TEXT("Links"), {ObjectValue(Link)});
		TSharedPtr<FJsonObject> WrapperProperties = MakeShared<FJsonObject>();
		WrapperProperties->SetStringField(TEXT("CompositeName"), TEXT("Task6 selector expression"));
		WrapperProperties->SetBoolField(TEXT("bShowOperations"), true);
		TSharedPtr<FJsonObject> Wrapper = MakeShared<FJsonObject>();
		Wrapper->SetStringField(TEXT("Id"), CompositeGuid);
		Wrapper->SetStringField(TEXT("Kind"), TEXT("Composite"));
		Wrapper->SetObjectField(TEXT("Properties"), WrapperProperties);
		Wrapper->SetObjectField(TEXT("BoundGraph"), BoundGraph);
		Task->SetArrayField(TEXT("Decorators"), {ObjectValue(Wrapper)});
		OutPath = FString::Printf(TEXT("/Body/Tree/Root/Children/0/Decorators/0/BoundGraph/Nodes/1/Properties/BlackboardKey/%s"), *DerivedField);
	}
	return Tree;
}

const TSharedPtr<FJsonObject> FindSelectorPolicy(const TSharedPtr<FJsonObject>& Payload, const FString& Path)
{
	const TArray<TSharedPtr<FJsonValue>>* Policies = nullptr;
	if (!Payload.IsValid() || !Payload->TryGetArrayField(TEXT("selector_policies"), Policies) || !Policies)
	{
		return nullptr;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Policies)
	{
		TSharedPtr<FJsonObject> Policy = Value.IsValid() ? Value->AsObject() : nullptr;
		FString Candidate;
		if (Policy.IsValid() && Policy->TryGetStringField(TEXT("path"), Candidate) && Candidate == Path)
		{
			return Policy;
		}
	}
	return nullptr;
}

UBTNode* FindTaskInstance(UBehaviorTree* Tree, const FString& Id = TaskGuid)
{
	const UBehaviorTreeGraph* Graph = Tree ? Cast<UBehaviorTreeGraph>(Tree->BTGraph) : nullptr;
	if (!Graph)
	{
		return nullptr;
	}
	for (UEdGraphNode* GraphNode : Graph->Nodes)
	{
		UBehaviorTreeGraphNode* BehaviorNode = Cast<UBehaviorTreeGraphNode>(GraphNode);
		if (BehaviorNode && BehaviorNode->NodeGuid.ToString(EGuidFormats::Digits) == Id)
		{
			return Cast<UBTNode>(BehaviorNode->NodeInstance);
		}
	}
	return nullptr;
}

FBlackboardKeySelector* FindBlackboardKeySelector(UObject* Node)
{
	FStructProperty* Property = FindFProperty<FStructProperty>(
		Node ? Node->GetClass() : nullptr,
		TEXT("BlackboardKey"));
	return Property && Property->Struct == FBlackboardKeySelector::StaticStruct()
		? Property->ContainerPtrToValuePtr<FBlackboardKeySelector>(Node)
		: nullptr;
}

UBTDecorator_Blackboard* FindBlackboardDecoratorInstance(UBehaviorTree* Tree, const FString& Id = DecoratorGuid)
{
	const UBehaviorTreeGraph* Graph = Tree ? Cast<UBehaviorTreeGraph>(Tree->BTGraph) : nullptr;
	if (!Graph)
	{
		return nullptr;
	}
	for (UEdGraphNode* GraphNode : Graph->Nodes)
	{
		UBehaviorTreeGraphNode* BehaviorNode = Cast<UBehaviorTreeGraphNode>(GraphNode);
		if (!BehaviorNode)
		{
			continue;
		}
		for (UAIGraphNode* SubNode : BehaviorNode->SubNodes)
		{
			if (SubNode && SubNode->NodeGuid.ToString(EGuidFormats::Digits) == Id)
			{
				return Cast<UBTDecorator_Blackboard>(SubNode->NodeInstance);
			}
		}
	}
	return nullptr;
}

FValueOrBBKey_Float* FindFloatValueOrBlackboardKey(UObject* Node, const FName PropertyName)
{
	FStructProperty* Property = FindFProperty<FStructProperty>(
		Node ? Node->GetClass() : nullptr,
		PropertyName);
	return Property && Property->Struct == FValueOrBBKey_Float::StaticStruct()
		? Property->ContainerPtrToValuePtr<FValueOrBBKey_Float>(Node)
		: nullptr;
}

TSharedPtr<FJsonObject> SubtreeTaskTree(UBehaviorTree* Subtree, bool bDynamic)
{
	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetField(
		bDynamic ? TEXT("DefaultBehaviorAsset") : TEXT("BehaviorAsset"),
		Subtree ? ObjectValue(AssetRef(Subtree)) : MakeShared<FJsonValueNull>());
	return TreeWithTask(
		Properties,
		bDynamic ? UBTTask_RunBehaviorDynamic::StaticClass()->GetPathName() : UBTTask_RunBehavior::StaticClass()->GetPathName());
}

int32 CountRecursiveObjects(UObject* Outer)
{
	int32 Count = 0;
	ForEachObjectWithOuter(Outer, [&Count](UObject*) { ++Count; });
	return Count;
}

int32 CountRegistryAssets(FName PackageName)
{
	TArray<FAssetData> Assets;
	FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssetsByPackageName(PackageName, Assets);
	return Assets.Num();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeTask6DerivedSelectorBoundaryTest,
	"UEAssetDoc.BehaviorTree.Task6.Selectors.DerivedFieldsAllSurfaces",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeTask6DerivedSelectorBoundaryTest::RunTest(const FString&)
{
	using namespace AssetDocumentBehaviorTreeSelectorTask6Tests;
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	for (const EDerivedSelectorLocation Location : {
		EDerivedSelectorLocation::Direct,
		EDerivedSelectorLocation::Service,
		EDerivedSelectorLocation::Decorator,
		EDerivedSelectorLocation::CompositeDecorator,
		EDerivedSelectorLocation::NestedContainer,
		EDerivedSelectorLocation::InstancedStruct})
	{
		for (const FString& Field : {TEXT("SelectedKeyName"), TEXT("AllowedTypes"), TEXT("SelectedKeyType"), TEXT("SelectedKeyID"), TEXT("bNoneIsAllowedValue")})
		{
			FString ExpectedPath;
			const TSharedPtr<FJsonObject> Tree = DerivedFieldTree(Location, Field, ExpectedPath);
			const TSharedPtr<FJsonObject> Value = Document(
				UniqueTarget(TEXT("DerivedField")),
				Body(Tree, MakeShared<FJsonValueNull>()),
				TEXT("Create"));
			const FAssetDocumentResult ValidateResult = Validate(Service, Value);
			const FAssetDocumentResult DiffResult = Diff(Service, Value);
			const FAssetDocumentResult ApplyResult = Apply(Service, Value);
			TestFalse(*FString::Printf(TEXT("Validate rejects %s at selector location %d"), *Field, static_cast<int32>(Location)), ValidateResult.IsSuccess());
			TestFalse(*FString::Printf(TEXT("Diff rejects %s at selector location %d"), *Field, static_cast<int32>(Location)), DiffResult.IsSuccess());
			TestFalse(*FString::Printf(TEXT("Apply rejects %s at selector location %d"), *Field, static_cast<int32>(Location)), ApplyResult.IsSuccess());
			TestTrue(TEXT("Validate reports exact selector authored boundary"), HasDiagnostic(ValidateResult, ExpectedPath, TEXT("NonAuthoredProperty")));
			TestTrue(TEXT("Diff reports identical selector authored boundary"), HasDiagnostic(DiffResult, ExpectedPath, TEXT("NonAuthoredProperty")));
			TestTrue(TEXT("Apply reports identical selector authored boundary"), HasDiagnostic(ApplyResult, ExpectedPath, TEXT("NonAuthoredProperty")));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeTask6SelectorResolutionMatrixTest,
	"UEAssetDoc.BehaviorTree.Task6.Selectors.ResolutionMatrix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeTask6SelectorResolutionMatrixTest::RunTest(const FString&)
{
	using namespace AssetDocumentBehaviorTreeSelectorTask6Tests;
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	UBlackboardData* Parent = Blackboard(UniqueTarget(TEXT("ParentBB")));
	UBlackboardData* Child = Blackboard(UniqueTarget(TEXT("ChildBB")));
	AddObjectKey(Parent, TEXT("InheritedActor"), APawn::StaticClass());
	AddObjectKey(Child, TEXT("LocalActor"), APawn::StaticClass());
	AddObjectKey(Child, TEXT("TooBroadObject"), UObject::StaticClass());
	AddKey<UBlackboardKeyType_Vector>(Child, TEXT("VectorKey"));
	AddKey<UBlackboardKeyType_Bool>(Child, TEXT("BoolKey"));
	Child->Parent = Parent;

	struct FCase
	{
		const TCHAR* Label;
		UBlackboardData* BlackboardAsset;
		const TCHAR* Property;
		const TCHAR* Key;
		bool bExpectedSuccess;
		const TCHAR* ExpectedCode;
	};
	const TArray<FCase> Cases = {
		{TEXT("local"), Child, TEXT("DirectSelector"), TEXT("LocalActor"), true, TEXT("")},
		{TEXT("inherited"), Child, TEXT("DirectSelector"), TEXT("InheritedActor"), true, TEXT("")},
		{TEXT("unknown"), Child, TEXT("DirectSelector"), TEXT("MissingActor"), false, TEXT("UnknownBlackboardKey")},
		{TEXT("wrong-type"), Child, TEXT("DirectSelector"), TEXT("VectorKey"), false, TEXT("IncompatibleBlackboardKeyType")},
		{TEXT("base-class"), Child, TEXT("DirectSelector"), TEXT("TooBroadObject"), false, TEXT("IncompatibleBlackboardKeyType")},
		{TEXT("none-allowed"), Child, TEXT("OptionalSelector"), TEXT(""), true, TEXT("")},
		{TEXT("none-disallowed"), Child, TEXT("DirectSelector"), TEXT(""), false, TEXT("MissingBlackboardKey")},
		{TEXT("missing-blackboard"), nullptr, TEXT("DirectSelector"), TEXT("LocalActor"), false, TEXT("MissingBehaviorTreeBlackboard")},
	};
	for (const FCase& Case : Cases)
	{
		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		if (FString(Case.Property) != TEXT("DirectSelector"))
		{
			Properties->SetObjectField(TEXT("DirectSelector"), Selector(TEXT("LocalActor")));
		}
		Properties->SetObjectField(Case.Property, Selector(Case.Key));
		const TSharedPtr<FJsonObject> Value = Document(
			UniqueTarget(TEXT("SelectorMatrix")),
			Body(TreeWithTask(Properties), Case.BlackboardAsset ? ObjectValue(AssetRef(Case.BlackboardAsset)) : MakeShared<FJsonValueNull>()),
			TEXT("Create"));
		const FAssetDocumentResult Result = Validate(Service, Value);
		TestEqual(*FString::Printf(TEXT("%s selector matrix result"), Case.Label), Result.IsSuccess(), Case.bExpectedSuccess);
		if (!Case.bExpectedSuccess)
		{
			const FString ExpectedPath = Case.BlackboardAsset
				? FString::Printf(TEXT("/Body/Tree/Root/Children/0/Properties/%s/Key"), Case.Property)
				: FString(TEXT("/Body/BlackboardAsset"));
			TestTrue(*FString::Printf(TEXT("%s selector diagnostic is exact"), Case.Label), HasDiagnostic(Result, ExpectedPath, Case.ExpectedCode));
		}
	}

	TSharedPtr<FJsonObject> RecursiveProperties = MakeShared<FJsonObject>();
	RecursiveProperties->SetObjectField(TEXT("DirectSelector"), Selector(TEXT("LocalActor")));
	RecursiveProperties->SetObjectField(TEXT("NestedValue"), NestedSelectorValue(TEXT("")));
	RecursiveProperties->SetArrayField(TEXT("ArrayValues"), {ObjectValue(NestedSelectorValue(TEXT("")))});
	TSharedPtr<FJsonObject> MapValues = MakeShared<FJsonObject>();
	MapValues->SetObjectField(TEXT("NamedEntry"), NestedSelectorValue(TEXT("")));
	RecursiveProperties->SetObjectField(TEXT("MapValues"), MapValues);
	TSharedPtr<FJsonObject> SetEntry = NestedSelectorValue(TEXT(""));
	SetEntry->SetStringField(TEXT("Identity"), TEXT("SetEntry"));
	RecursiveProperties->SetArrayField(TEXT("SetValues"), {ObjectValue(SetEntry)});
	TSharedPtr<FJsonObject> Dynamic = MakeShared<FJsonObject>();
	Dynamic->SetStringField(TEXT("Struct"), FAssetDocumentReflectedPropertyNestedTestValue::StaticStruct()->GetPathName());
	Dynamic->SetObjectField(TEXT("Properties"), NestedSelectorValue(TEXT("")));
	RecursiveProperties->SetObjectField(TEXT("DynamicValue"), Dynamic);
	for (const FString& Property : {TEXT("NestedValue"), TEXT("ArrayValues"), TEXT("MapValues"), TEXT("SetValues"), TEXT("DynamicValue")})
	{
		TSharedPtr<FJsonObject> RecursiveSelector;
		if (Property == TEXT("NestedValue"))
		{
			RecursiveSelector = RecursiveProperties->GetObjectField(Property)->GetObjectField(TEXT("Selector"));
		}
		else if (Property == TEXT("ArrayValues") || Property == TEXT("SetValues"))
		{
			RecursiveSelector = RecursiveProperties->GetArrayField(Property)[0]->AsObject()->GetObjectField(TEXT("Selector"));
		}
		else if (Property == TEXT("MapValues"))
		{
			RecursiveSelector = RecursiveProperties->GetObjectField(Property)->GetObjectField(TEXT("NamedEntry"))->GetObjectField(TEXT("Selector"));
		}
		else
		{
			RecursiveSelector = RecursiveProperties->GetObjectField(Property)->GetObjectField(TEXT("Properties"))->GetObjectField(TEXT("Selector"));
		}
		RecursiveSelector->SetStringField(TEXT("Key"), TEXT("InheritedActor"));
	}
	const TSharedPtr<FJsonObject> RecursiveDocument = Document(
		UniqueTarget(TEXT("RecursiveSelectors")),
		Body(TreeWithTask(RecursiveProperties), ObjectValue(AssetRef(Child))),
		TEXT("Create"));
	TestTrue(TEXT("struct/array/map/set/FInstancedStruct selectors resolve recursively"), Validate(Service, RecursiveDocument).IsSuccess());

	MapValues->GetObjectField(TEXT("NamedEntry"))->GetObjectField(TEXT("Selector"))->SetStringField(TEXT("Key"), TEXT("MissingActor"));
	const FAssetDocumentResult RecursiveFailure = Validate(Service, RecursiveDocument);
	TestFalse(TEXT("recursive map selector mismatch is rejected"), RecursiveFailure.IsSuccess());
	TestTrue(TEXT("recursive map selector diagnostic path is exact"), HasDiagnostic(
		RecursiveFailure,
		TEXT("/Body/Tree/Root/Children/0/Properties/MapValues/NamedEntry/Selector/Key"),
		TEXT("UnknownBlackboardKey")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeTask6OmittedRequiredSelectorTest,
	"UEAssetDoc.BehaviorTree.Task6.Selectors.OmittedRequiredKeyParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeTask6OmittedRequiredSelectorTest::RunTest(const FString&)
{
	using namespace AssetDocumentBehaviorTreeSelectorTask6Tests;
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	UBlackboardData* TestBlackboard = Blackboard(UniqueTarget(TEXT("OmittedSelectorBB")));
	AddObjectKey(TestBlackboard, TEXT("TargetActor"), APawn::StaticClass());

	const auto SelectorProperties = [](bool bIncludeDirectSelector)
	{
		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		if (bIncludeDirectSelector)
		{
			Properties->SetObjectField(TEXT("DirectSelector"), Selector(TEXT("TargetActor")));
		}
		return Properties;
	};
	const auto CreateDocument = [&TestBlackboard, &SelectorProperties](const TCHAR* Stem, bool bIncludeDirectSelector)
	{
		return Document(
			UniqueTarget(Stem),
			Body(TreeWithTask(SelectorProperties(bIncludeDirectSelector)), ObjectValue(AssetRef(TestBlackboard))),
			TEXT("Create"));
	};
	const auto UpdateDocument = [&TestBlackboard, &SelectorProperties](const TCHAR* Stem, bool bIncludeDirectSelector)
	{
		const FString Target = UniqueTarget(Stem);
		BehaviorTree(Target, TestBlackboard);
		return Document(
			Target,
			Body(TreeWithTask(SelectorProperties(bIncludeDirectSelector)), ObjectValue(AssetRef(TestBlackboard))),
			TEXT("Update"));
	};

	const FString RequiredKeyPath = TEXT("/Body/Tree/Root/Children/0/Properties/DirectSelector/Key");
	for (const FAssetDocumentResult& Result : {
		Validate(Service, CreateDocument(TEXT("OmittedRequiredValidate"), false)),
		Diff(Service, UpdateDocument(TEXT("OmittedRequiredDiff"), false)),
		Apply(Service, CreateDocument(TEXT("OmittedRequiredApply"), false))})
	{
		TestFalse(TEXT("omitted required selector is rejected on every surface"), Result.IsSuccess());
		TestTrue(TEXT("omitted required selector diagnostic is stable"), HasDiagnostic(Result, RequiredKeyPath, TEXT("MissingBlackboardKey")));
	}

	for (const FAssetDocumentResult& Result : {
		Validate(Service, CreateDocument(TEXT("OmittedOptionalValidate"), true)),
		Diff(Service, UpdateDocument(TEXT("OmittedOptionalDiff"), true)),
		Apply(Service, CreateDocument(TEXT("OmittedOptionalApply"), true))})
	{
		TestTrue(
			*FString::Printf(TEXT("omitted selector remains valid when the node policy allows None: %s"), *DescribeResult(Result)),
			Result.IsSuccess());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeTask6DesiredBlackboardOverlayTest,
	"UEAssetDoc.BehaviorTree.Task6.BlackboardOverlay.ValidateDiffApplyParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeTask6DesiredBlackboardOverlayTest::RunTest(const FString&)
{
	using namespace AssetDocumentBehaviorTreeSelectorTask6Tests;
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	UBlackboardData* BlackboardA = Blackboard(UniqueTarget(TEXT("OverlayA")));
	UBlackboardData* BlackboardB = Blackboard(UniqueTarget(TEXT("OverlayB")));
	AddObjectKey(BlackboardA, TEXT("OnlyA"), AActor::StaticClass());
	AddObjectKey(BlackboardB, TEXT("OnlyB"), AActor::StaticClass());
	const FString ExistingTarget = UniqueTarget(TEXT("OverlayExisting"));
	BehaviorTree(ExistingTarget, BlackboardA);

	TSharedPtr<FJsonObject> BProperties = MakeShared<FJsonObject>();
	BProperties->SetObjectField(TEXT("DirectSelector"), Selector(TEXT("OnlyB")));
	const TSharedPtr<FJsonObject> ExplicitB = Document(
		ExistingTarget,
		Body(TreeWithTask(BProperties), ObjectValue(AssetRef(BlackboardB))),
		TEXT("Update"));

	TSharedPtr<FJsonObject> AProperties = MakeShared<FJsonObject>();
	AProperties->SetObjectField(TEXT("DirectSelector"), Selector(TEXT("OnlyA")));
	const TSharedPtr<FJsonObject> TreeOnlyUpdate = Document(
		ExistingTarget,
		Body(TreeWithTask(AProperties), nullptr, false),
		TEXT("Update"));
	TestTrue(TEXT("Tree-only Update inherits existing Blackboard"), Validate(Service, TreeOnlyUpdate).IsSuccess());

	const FAssetDocumentResult ExplicitBValidate = Validate(Service, ExplicitB);
	const FAssetDocumentResult ExplicitBDiff = Diff(Service, ExplicitB);
	const FAssetDocumentResult ExplicitBApply = Apply(Service, ExplicitB);
	TestTrue(TEXT("Validate uses explicit desired Blackboard B instead of existing Blackboard A"), ExplicitBValidate.IsSuccess());
	TestTrue(TEXT("Diff completes for explicit desired Blackboard B"), ExplicitBDiff.IsSuccess());
	TestTrue(TEXT("Diff has no hidden failed regions for explicit desired Blackboard B"), HasEmptyPayloadArray(ExplicitBDiff, TEXT("failed")));
	TestTrue(TEXT("Apply uses explicit desired Blackboard B instead of existing Blackboard A"), ExplicitBApply.IsSuccess());

	for (const bool bExplicitNull : {false, true})
	{
		const TSharedPtr<FJsonObject> Missing = Document(
			UniqueTarget(bExplicitNull ? TEXT("ExplicitNullCreate") : TEXT("OmittedCreate")),
			Body(TreeWithTask(AProperties), MakeShared<FJsonValueNull>(), bExplicitNull),
			TEXT("Create"));
		const FAssetDocumentResult ValidateResult = Validate(Service, Missing);
		const FAssetDocumentResult DiffResult = Diff(Service, Missing);
		const FAssetDocumentResult ApplyResult = Apply(Service, Missing);
		for (const FAssetDocumentResult* Result : {&ValidateResult, &DiffResult, &ApplyResult})
		{
			TestFalse(TEXT("Create without an effective Blackboard fails"), Result->IsSuccess());
			TestTrue(TEXT("missing effective Blackboard diagnostic is stable"), HasDiagnostic(*Result, TEXT("/Body/BlackboardAsset"), TEXT("MissingBehaviorTreeBlackboard")));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeTask6SparseBlackboardUpdateTest,
	"UEAssetDoc.BehaviorTree.Task6.BlackboardOverlay.SparseUpdateValidatesAndRefreshesExistingTree",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeTask6SparseBlackboardUpdateTest::RunTest(const FString&)
{
	using namespace AssetDocumentBehaviorTreeSelectorTask6Tests;
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	UBlackboardData* BlackboardA = Blackboard(UniqueTarget(TEXT("SparseA")));
	UBlackboardData* IncompatibleBlackboard = Blackboard(UniqueTarget(TEXT("SparseIncompatible")));
	UBlackboardData* CompatibleBlackboard = Blackboard(UniqueTarget(TEXT("SparseCompatible")));
	AddObjectKey(BlackboardA, TEXT("OnlyA"), APawn::StaticClass());
	AddKey<UBlackboardKeyType_Bool>(IncompatibleBlackboard, TEXT("OtherKey"));
	AddKey<UBlackboardKeyType_Bool>(CompatibleBlackboard, TEXT("LeadingKey"));
	AddObjectKey(CompatibleBlackboard, TEXT("OnlyA"), APawn::StaticClass());

	TSharedPtr<FJsonObject> InitialProperties = MakeShared<FJsonObject>();
	InitialProperties->SetObjectField(TEXT("DirectSelector"), Selector(TEXT("OnlyA")));
	const FString Target = UniqueTarget(TEXT("SparseExisting"));
	const FAssetDocumentResult InitialApply = Apply(Service, Document(
		Target,
		Body(TreeWithTask(InitialProperties), ObjectValue(AssetRef(BlackboardA))),
		TEXT("Create")));
	TestTrue(TEXT("sparse Blackboard fixture creates a semantic tree"), InitialApply.IsSuccess());
	if (!InitialApply.IsSuccess())
	{
		return false;
	}

	auto SparseUpdate = [&Target](UBlackboardData* DesiredBlackboard)
	{
		TSharedPtr<FJsonObject> SparseBody = MakeShared<FJsonObject>();
		SparseBody->SetObjectField(TEXT("BlackboardAsset"), AssetRef(DesiredBlackboard));
		return Document(Target, SparseBody, TEXT("Update"));
	};

	const TSharedPtr<FJsonObject> IncompatibleUpdate = SparseUpdate(IncompatibleBlackboard);
	const FString SelectorPath = TEXT("/Body/Tree/Root/Children/0/Properties/DirectSelector/Key");
	for (const FAssetDocumentResult& Result : {
		Validate(Service, IncompatibleUpdate),
		Diff(Service, IncompatibleUpdate),
		Apply(Service, IncompatibleUpdate)})
	{
		TestFalse(TEXT("sparse incompatible Blackboard update is rejected"), Result.IsSuccess());
		TestTrue(TEXT("sparse incompatible update reports the retained selector path"), HasDiagnostic(Result, SelectorPath, TEXT("UnknownBlackboardKey")));
	}
	UBehaviorTree* Existing = LoadObject<UBehaviorTree>(nullptr, *ObjectPath(Target));
	TestEqual(TEXT("failed sparse update preserves the existing Blackboard"), Existing ? Existing->BlackboardAsset.Get() : nullptr, BlackboardA);

	const FAssetDocumentResult CompatibleApply = Apply(Service, SparseUpdate(CompatibleBlackboard));
	TestTrue(
		*FString::Printf(TEXT("sparse compatible Blackboard update applies: %s"), *DescribeResult(CompatibleApply)),
		CompatibleApply.IsSuccess());
	Existing = LoadObject<UBehaviorTree>(nullptr, *ObjectPath(Target));
	UAssetDocumentSelectorTaskTestNode* ExistingTask = Cast<UAssetDocumentSelectorTaskTestNode>(FindTaskInstance(Existing));
	TestEqual(TEXT("sparse compatible update swaps the Blackboard"), Existing ? Existing->BlackboardAsset.Get() : nullptr, CompatibleBlackboard);
	TestNotNull(TEXT("sparse compatible update retains the existing task"), ExistingTask);
	if (ExistingTask)
	{
		TestEqual(TEXT("sparse compatible update preserves the authored key"), ExistingTask->DirectSelector.SelectedKeyName, FName(TEXT("OnlyA")));
		TestEqual(TEXT("sparse compatible update refreshes the selector key ID"), ExistingTask->DirectSelector.GetSelectedKeyID(), CompatibleBlackboard->GetKeyID(TEXT("OnlyA")));
		TestEqual(TEXT("sparse compatible update refreshes the selector key type"), ExistingTask->DirectSelector.SelectedKeyType.Get(), UBlackboardKeyType_Object::StaticClass());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeTask6RetainedEmptySelectorTest,
	"UEAssetDoc.BehaviorTree.Task6.BlackboardOverlay.RetainedEmptySelectorRemainsUnAuthored",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeTask6RetainedEmptySelectorTest::RunTest(const FString&)
{
	using namespace AssetDocumentBehaviorTreeSelectorTask6Tests;
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	UBlackboardData* InitialBlackboard = Blackboard(UniqueTarget(TEXT("RetainedEmptyInitial")));
	UBlackboardData* DesiredBlackboard = Blackboard(UniqueTarget(TEXT("RetainedEmptyDesired")));
	AddObjectKey(InitialBlackboard, TEXT("TargetActor"), APawn::StaticClass());
	AddObjectKey(DesiredBlackboard, TEXT("TargetActor"), APawn::StaticClass());

	TSharedPtr<FJsonObject> InitialProperties = MakeShared<FJsonObject>();
	InitialProperties->SetObjectField(TEXT("DirectSelector"), Selector(TEXT("TargetActor")));
	const FString Target = UniqueTarget(TEXT("RetainedEmptyExisting"));
	const FAssetDocumentResult InitialApply = Apply(Service, Document(
		Target,
		Body(TreeWithTask(InitialProperties), ObjectValue(AssetRef(InitialBlackboard))),
		TEXT("Create")));
	TestTrue(TEXT("retained-empty fixture creates a semantic tree"), InitialApply.IsSuccess());
	if (!InitialApply.IsSuccess())
	{
		return false;
	}

	UBehaviorTree* Existing = LoadObject<UBehaviorTree>(nullptr, *ObjectPath(Target));
	UAssetDocumentSelectorTaskTestNode* ExistingTask = Cast<UAssetDocumentSelectorTaskTestNode>(FindTaskInstance(Existing));
	TestNotNull(TEXT("retained-empty fixture has its task instance"), ExistingTask);
	if (!ExistingTask)
	{
		return false;
	}
	ExistingTask->DirectSelector.SelectedKeyName = NAME_None;
	ExistingTask->DirectSelector.InvalidateResolvedKey();
	ExistingTask->DirectSelector.SelectedKeyType = nullptr;

	TSharedPtr<FJsonObject> SparseBody = MakeShared<FJsonObject>();
	SparseBody->SetObjectField(TEXT("BlackboardAsset"), AssetRef(DesiredBlackboard));
	const TSharedPtr<FJsonObject> SparseUpdate = Document(Target, SparseBody, TEXT("Update"));
	const FAssetDocumentResult ValidateResult = Validate(Service, SparseUpdate);
	const FAssetDocumentResult DiffResult = Diff(Service, SparseUpdate);
	const FAssetDocumentResult ApplyResult = Apply(Service, SparseUpdate);
	TestTrue(
		*FString::Printf(TEXT("retained empty selector is not re-authored by Validate: %s"), *DescribeResult(ValidateResult)),
		ValidateResult.IsSuccess());
	TestTrue(
		*FString::Printf(TEXT("retained empty selector is not re-authored by Diff: %s"), *DescribeResult(DiffResult)),
		DiffResult.IsSuccess() && HasEmptyPayloadArray(DiffResult, TEXT("failed")));
	TestTrue(
		*FString::Printf(TEXT("retained empty selector is not re-authored by Apply: %s"), *DescribeResult(ApplyResult)),
		ApplyResult.IsSuccess());

	Existing = LoadObject<UBehaviorTree>(nullptr, *ObjectPath(Target));
	ExistingTask = Cast<UAssetDocumentSelectorTaskTestNode>(FindTaskInstance(Existing));
	TestEqual(TEXT("retained-empty sparse update swaps the Blackboard"), Existing ? Existing->BlackboardAsset.Get() : nullptr, DesiredBlackboard);
	TestNotNull(TEXT("retained-empty sparse update keeps the task"), ExistingTask);
	if (ExistingTask)
	{
		TestTrue(TEXT("retained-empty sparse update does not synthesize a selector name"), ExistingTask->DirectSelector.SelectedKeyName.IsNone());
		TestEqual(TEXT("retained-empty sparse update keeps the selector ID invalid"), ExistingTask->DirectSelector.GetSelectedKeyID(), FBlackboard::InvalidKey);
		TestNull(TEXT("retained-empty sparse update keeps the selector type empty"), ExistingTask->DirectSelector.SelectedKeyType.Get());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeTask6NativeRetainedEmptySelectorsTest,
	"UEAssetDoc.BehaviorTree.Task6.BlackboardOverlay.NativeRetainedEmptySelectorsRemainUnAuthored",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeTask6NativeRetainedEmptySelectorsTest::RunTest(const FString&)
{
	using namespace AssetDocumentBehaviorTreeSelectorTask6Tests;
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	UBlackboardData* InitialBlackboard = Blackboard(UniqueTarget(TEXT("NativeRetainedEmptyInitial")));
	UBlackboardData* DesiredBlackboard = Blackboard(UniqueTarget(TEXT("NativeRetainedEmptyDesired")));
	AddObjectKey(InitialBlackboard, TEXT("TargetActor"), APawn::StaticClass());
	AddObjectKey(DesiredBlackboard, TEXT("TargetActor"), APawn::StaticClass());

	TSharedPtr<FJsonObject> MoveToProperties = MakeShared<FJsonObject>();
	MoveToProperties->SetObjectField(TEXT("BlackboardKey"), Selector(TEXT("TargetActor")));
	TSharedPtr<FJsonObject> NativeTree = TreeWithTask(
		MoveToProperties,
		UBTTask_MoveTo::StaticClass()->GetPathName());
	TSharedPtr<FJsonObject> ServiceProperties = MakeShared<FJsonObject>();
	ServiceProperties->SetObjectField(TEXT("BlackboardKey"), Selector(TEXT("TargetActor")));
	TSharedPtr<FJsonObject> ServiceNode = MakeShared<FJsonObject>();
	ServiceNode->SetStringField(TEXT("Id"), ServiceGuid);
	ServiceNode->SetStringField(TEXT("Class"), UBTService_DefaultFocus::StaticClass()->GetPathName());
	ServiceNode->SetObjectField(TEXT("Properties"), ServiceProperties);
	NativeTree->GetObjectField(TEXT("Root"))->SetArrayField(TEXT("Services"), {ObjectValue(ServiceNode)});

	const FString Target = UniqueTarget(TEXT("NativeRetainedEmptyExisting"));
	const FAssetDocumentResult InitialApply = Apply(Service, Document(
		Target,
		Body(NativeTree, ObjectValue(AssetRef(InitialBlackboard))),
		TEXT("Create")));
	TestTrue(TEXT("native retained-empty fixture creates a semantic tree"), InitialApply.IsSuccess());
	if (!InitialApply.IsSuccess())
	{
		return false;
	}

	UBehaviorTree* Existing = LoadObject<UBehaviorTree>(nullptr, *ObjectPath(Target));
	UBTTask_MoveTo* ExistingTask = Cast<UBTTask_MoveTo>(FindTaskInstance(Existing));
	UBTService_DefaultFocus* ExistingService = Existing
		&& Existing->RootNode
		&& !Existing->RootNode->Services.IsEmpty()
		? Cast<UBTService_DefaultFocus>(Existing->RootNode->Services[0].Get())
		: nullptr;
	TestNotNull(TEXT("native retained-empty fixture has MoveTo"), ExistingTask);
	TestNotNull(TEXT("native retained-empty fixture has DefaultFocus"), ExistingService);
	FBlackboardKeySelector* ExistingTaskSelector = FindBlackboardKeySelector(ExistingTask);
	FBlackboardKeySelector* ExistingServiceSelector = FindBlackboardKeySelector(ExistingService);
	TestNotNull(TEXT("native retained-empty fixture reflects MoveTo selector"), ExistingTaskSelector);
	TestNotNull(TEXT("native retained-empty fixture reflects DefaultFocus selector"), ExistingServiceSelector);
	if (!ExistingTaskSelector || !ExistingServiceSelector)
	{
		return false;
	}

	for (FBlackboardKeySelector* SelectorValue : {ExistingTaskSelector, ExistingServiceSelector})
	{
		SelectorValue->SelectedKeyName = NAME_None;
		SelectorValue->InvalidateResolvedKey();
		SelectorValue->SelectedKeyType = nullptr;
	}

	TSharedPtr<FJsonObject> SparseBody = MakeShared<FJsonObject>();
	SparseBody->SetObjectField(TEXT("BlackboardAsset"), AssetRef(DesiredBlackboard));
	const TSharedPtr<FJsonObject> SparseUpdate = Document(Target, SparseBody, TEXT("Update"));
	const FAssetDocumentResult ValidateResult = Validate(Service, SparseUpdate);
	TestTrue(
		*FString::Printf(TEXT("native retained-empty sparse Validate succeeds: %s"), *DescribeResult(ValidateResult)),
		ValidateResult.IsSuccess());
	TestTrue(TEXT("Validate does not author MoveTo selector"), ExistingTaskSelector->SelectedKeyName.IsNone());
	TestTrue(TEXT("Validate does not author DefaultFocus selector"), ExistingServiceSelector->SelectedKeyName.IsNone());

	const FAssetDocumentResult DiffResult = Diff(Service, SparseUpdate);
	TestTrue(
		*FString::Printf(TEXT("native retained-empty sparse Diff succeeds: %s"), *DescribeResult(DiffResult)),
		DiffResult.IsSuccess() && HasEmptyPayloadArray(DiffResult, TEXT("failed")));
	TestTrue(TEXT("Diff does not author MoveTo selector"), ExistingTaskSelector->SelectedKeyName.IsNone());
	TestTrue(TEXT("Diff does not author DefaultFocus selector"), ExistingServiceSelector->SelectedKeyName.IsNone());

	const FAssetDocumentResult ApplyResult = Apply(Service, SparseUpdate);
	TestTrue(
		*FString::Printf(TEXT("native retained-empty sparse Apply succeeds: %s"), *DescribeResult(ApplyResult)),
		ApplyResult.IsSuccess());
	Existing = LoadObject<UBehaviorTree>(nullptr, *ObjectPath(Target));
	ExistingTask = Cast<UBTTask_MoveTo>(FindTaskInstance(Existing));
	ExistingService = Existing
		&& Existing->RootNode
		&& !Existing->RootNode->Services.IsEmpty()
		? Cast<UBTService_DefaultFocus>(Existing->RootNode->Services[0].Get())
		: nullptr;
	ExistingTaskSelector = FindBlackboardKeySelector(ExistingTask);
	ExistingServiceSelector = FindBlackboardKeySelector(ExistingService);
	TestEqual(TEXT("native retained-empty sparse update swaps the Blackboard"), Existing ? Existing->BlackboardAsset.Get() : nullptr, DesiredBlackboard);
	TestNotNull(TEXT("native retained-empty sparse update keeps MoveTo"), ExistingTask);
	TestNotNull(TEXT("native retained-empty sparse update keeps DefaultFocus"), ExistingService);
	TestNotNull(TEXT("native retained-empty sparse update reflects MoveTo selector"), ExistingTaskSelector);
	TestNotNull(TEXT("native retained-empty sparse update reflects DefaultFocus selector"), ExistingServiceSelector);
	if (ExistingTaskSelector && ExistingServiceSelector)
	{
		for (const TPair<FString, FBlackboardKeySelector*> SelectorCase : {
			TPair<FString, FBlackboardKeySelector*>(TEXT("MoveTo"), ExistingTaskSelector),
			TPair<FString, FBlackboardKeySelector*>(TEXT("DefaultFocus"), ExistingServiceSelector)})
		{
			TestTrue(*FString::Printf(TEXT("%s selector name remains un-authored"), *SelectorCase.Key), SelectorCase.Value->SelectedKeyName.IsNone());
			TestEqual(*FString::Printf(TEXT("%s selector ID remains invalid"), *SelectorCase.Key), SelectorCase.Value->GetSelectedKeyID(), FBlackboard::InvalidKey);
			TestNull(*FString::Printf(TEXT("%s selector type remains empty"), *SelectorCase.Key), SelectorCase.Value->SelectedKeyType.Get());
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeTask6WaitBlackboardTimeAuthoredStateTest,
	"UEAssetDoc.BehaviorTree.Task6.BlackboardOverlay.WaitBlackboardTimeAuthoredStateIsPreserved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeTask6WaitBlackboardTimeAuthoredStateTest::RunTest(const FString&)
{
	using namespace AssetDocumentBehaviorTreeSelectorTask6Tests;
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	UBlackboardData* InitialBlackboard = Blackboard(UniqueTarget(TEXT("WaitAuthoredInitial")));
	UBlackboardData* DesiredBlackboard = Blackboard(UniqueTarget(TEXT("WaitAuthoredDesired")));
	AddKey<UBlackboardKeyType_Float>(InitialBlackboard, TEXT("WaitDuration"));
	AddKey<UBlackboardKeyType_Float>(DesiredBlackboard, TEXT("AutoSelectedDuration"));

	TSharedPtr<FJsonObject> InitialProperties = MakeShared<FJsonObject>();
	InitialProperties->SetObjectField(TEXT("BlackboardKey"), Selector(TEXT("WaitDuration")));
	const FString Target = UniqueTarget(TEXT("WaitAuthoredExisting"));
	const FAssetDocumentResult InitialApply = Apply(Service, Document(
		Target,
		Body(TreeWithTask(InitialProperties, UBTTask_WaitBlackboardTime::StaticClass()->GetPathName()), ObjectValue(AssetRef(InitialBlackboard))),
		TEXT("Create")));
	TestTrue(TEXT("WaitBlackboardTime fixture creates a semantic tree"), InitialApply.IsSuccess());
	if (!InitialApply.IsSuccess())
	{
		return false;
	}

	UBehaviorTree* Existing = LoadObject<UBehaviorTree>(nullptr, *ObjectPath(Target));
	UBTTask_WaitBlackboardTime* ExistingTask = Cast<UBTTask_WaitBlackboardTime>(FindTaskInstance(Existing));
	FBlackboardKeySelector* ExistingSelector = FindBlackboardKeySelector(ExistingTask);
	FValueOrBBKey_Float* ExistingWaitTime = FindFloatValueOrBlackboardKey(ExistingTask, TEXT("WaitTime"));
	TestNotNull(TEXT("WaitBlackboardTime fixture has its task"), ExistingTask);
	TestNotNull(TEXT("WaitBlackboardTime fixture reflects its selector"), ExistingSelector);
	TestNotNull(TEXT("WaitBlackboardTime fixture reflects WaitTime"), ExistingWaitTime);
	if (!ExistingSelector || !ExistingWaitTime)
	{
		return false;
	}
	ExistingSelector->SelectedKeyName = NAME_None;
	ExistingSelector->InvalidateResolvedKey();
	ExistingSelector->SelectedKeyType = nullptr;
	*ExistingWaitTime = FValueOrBBKey_Float(17.25f);

	TSharedPtr<FJsonObject> SparseBody = MakeShared<FJsonObject>();
	SparseBody->SetObjectField(TEXT("BlackboardAsset"), AssetRef(DesiredBlackboard));
	const FAssetDocumentResult ApplyResult = Apply(Service, Document(Target, SparseBody, TEXT("Update")));
	TestTrue(
		*FString::Printf(TEXT("WaitBlackboardTime sparse update applies: %s"), *DescribeResult(ApplyResult)),
		ApplyResult.IsSuccess());

	Existing = LoadObject<UBehaviorTree>(nullptr, *ObjectPath(Target));
	ExistingTask = Cast<UBTTask_WaitBlackboardTime>(FindTaskInstance(Existing));
	ExistingSelector = FindBlackboardKeySelector(ExistingTask);
	ExistingWaitTime = FindFloatValueOrBlackboardKey(ExistingTask, TEXT("WaitTime"));
	TestTrue(TEXT("sparse update does not synthesize the retained selector"), ExistingSelector && ExistingSelector->SelectedKeyName.IsNone());
	TestTrue(TEXT("sparse update preserves the authored WaitTime value mode"), ExistingWaitTime && ExistingWaitTime->GetKey().IsNone());
	TestEqual(
		TEXT("sparse update preserves the authored WaitTime default"),
		ExistingWaitTime ? ExistingWaitTime->GetValue(static_cast<const UBlackboardComponent*>(nullptr)) : 0.f,
		17.25f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeTask6SparseUpdateSkipsProjectLifecycleTest,
	"UEAssetDoc.BehaviorTree.Task6.BlackboardOverlay.SparseUpdateSkipsArbitraryProjectLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeTask6SparseUpdateSkipsProjectLifecycleTest::RunTest(const FString&)
{
	using namespace AssetDocumentBehaviorTreeSelectorTask6Tests;
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	UBlackboardData* InitialBlackboard = Blackboard(UniqueTarget(TEXT("LifecycleInitial")));
	UBlackboardData* DesiredBlackboard = Blackboard(UniqueTarget(TEXT("LifecycleDesired")));
	AddKey<UBlackboardKeyType_Bool>(InitialBlackboard, TEXT("Flag"));
	AddKey<UBlackboardKeyType_Bool>(DesiredBlackboard, TEXT("Flag"));

	const FString Target = UniqueTarget(TEXT("LifecycleExisting"));
	const FAssetDocumentResult InitialApply = Apply(Service, Document(
		Target,
		Body(
			TreeWithTask(MakeShared<FJsonObject>(), UAssetDocumentSelectorLifecycleMutationTaskTestNode::StaticClass()->GetPathName()),
			ObjectValue(AssetRef(InitialBlackboard))),
		TEXT("Create")));
	TestTrue(TEXT("lifecycle fixture creates a semantic tree"), InitialApply.IsSuccess());
	if (!InitialApply.IsSuccess())
	{
		return false;
	}

	UBehaviorTree* Existing = LoadObject<UBehaviorTree>(nullptr, *ObjectPath(Target));
	UAssetDocumentSelectorLifecycleMutationTaskTestNode* ExistingTask =
		Cast<UAssetDocumentSelectorLifecycleMutationTaskTestNode>(FindTaskInstance(Existing));
	TestNotNull(TEXT("lifecycle fixture has its task"), ExistingTask);
	if (!ExistingTask)
	{
		return false;
	}
	FAssetDocumentReflectedPropertyNestedTestValue StableEntry;
	StableEntry.Identity = TEXT("StableEntry");
	StableEntry.PreservedText = TEXT("Must remain live");
	ExistingTask->ArrayValues = {MoveTemp(StableEntry)};
	const int32 LifecycleCallsBefore = ExistingTask->InitializeFromAssetCallCountForTest;
	ExistingTask->bMutateContainersOnInitializeForTest = true;

	TSharedPtr<FJsonObject> SparseBody = MakeShared<FJsonObject>();
	SparseBody->SetObjectField(TEXT("BlackboardAsset"), AssetRef(DesiredBlackboard));
	const TSharedPtr<FJsonObject> SparseUpdate = Document(Target, SparseBody, TEXT("Update"));
	int32 GlobalLifecycleCallsBefore =
		UAssetDocumentSelectorLifecycleMutationTaskTestNode::GlobalInitializeFromAssetCallCountForTest();
	const FAssetDocumentResult ValidateResult = Validate(Service, SparseUpdate);
	TestTrue(
		*FString::Printf(TEXT("project lifecycle sparse Validate succeeds: %s"), *DescribeResult(ValidateResult)),
		ValidateResult.IsSuccess());
	TestEqual(
		TEXT("sparse Validate does not call arbitrary project InitializeFromAsset on a transient preview"),
		UAssetDocumentSelectorLifecycleMutationTaskTestNode::GlobalInitializeFromAssetCallCountForTest(),
		GlobalLifecycleCallsBefore);

	GlobalLifecycleCallsBefore =
		UAssetDocumentSelectorLifecycleMutationTaskTestNode::GlobalInitializeFromAssetCallCountForTest();
	const FAssetDocumentResult DiffResult = Diff(Service, SparseUpdate);
	TestTrue(
		*FString::Printf(TEXT("project lifecycle sparse Diff succeeds: %s"), *DescribeResult(DiffResult)),
		DiffResult.IsSuccess() && HasEmptyPayloadArray(DiffResult, TEXT("failed")));
	TestEqual(
		TEXT("sparse Diff does not call arbitrary project InitializeFromAsset on a transient preview"),
		UAssetDocumentSelectorLifecycleMutationTaskTestNode::GlobalInitializeFromAssetCallCountForTest(),
		GlobalLifecycleCallsBefore);

	GlobalLifecycleCallsBefore =
		UAssetDocumentSelectorLifecycleMutationTaskTestNode::GlobalInitializeFromAssetCallCountForTest();
	const FAssetDocumentResult ApplyResult = Apply(Service, SparseUpdate);
	TestTrue(
		*FString::Printf(TEXT("project lifecycle sparse update applies: %s"), *DescribeResult(ApplyResult)),
		ApplyResult.IsSuccess());
	TestEqual(
		TEXT("sparse Apply does not call arbitrary project InitializeFromAsset on a transient preview"),
		UAssetDocumentSelectorLifecycleMutationTaskTestNode::GlobalInitializeFromAssetCallCountForTest(),
		GlobalLifecycleCallsBefore);
	TestEqual(
		TEXT("sparse Blackboard-only update does not call arbitrary project InitializeFromAsset"),
		ExistingTask->InitializeFromAssetCallCountForTest,
		LifecycleCallsBefore);
	TestTrue(
		TEXT("sparse Blackboard-only update preserves project container topology"),
		ExistingTask->ArrayValues.Num() == 1
		&& ExistingTask->ArrayValues[0].Identity == TEXT("StableEntry")
		&& ExistingTask->ArrayValues[0].PreservedText == TEXT("Must remain live"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeTask6BlackboardDecoratorDerivedOperationTest,
	"UEAssetDoc.BehaviorTree.Task6.BlackboardOverlay.BlackboardDecoratorDerivedOperationRefreshes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeTask6BlackboardDecoratorDerivedOperationTest::RunTest(const FString&)
{
	using namespace AssetDocumentBehaviorTreeSelectorTask6Tests;
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	UBlackboardData* BlackboardA = Blackboard(UniqueTarget(TEXT("DecoratorOperationBasic")));
	UBlackboardData* BlackboardB = Blackboard(UniqueTarget(TEXT("DecoratorOperationArithmetic")));
	AddKey<UBlackboardKeyType_Bool>(BlackboardA, TEXT("Mode"));
	AddKey<UBlackboardKeyType_Int>(BlackboardB, TEXT("Mode"));
	BlackboardA->UpdateParentKeys();
	BlackboardA->UpdateKeyIDs();
	BlackboardB->UpdateParentKeys();
	BlackboardB->UpdateKeyIDs();

	TSharedPtr<FJsonObject> DecoratorProperties = MakeShared<FJsonObject>();
	DecoratorProperties->SetObjectField(TEXT("BlackboardKey"), Selector(TEXT("Mode")));
	DecoratorProperties->SetStringField(TEXT("BasicOperation"), TEXT("NotSet"));
	DecoratorProperties->SetStringField(TEXT("ArithmeticOperation"), TEXT("Equal"));
	DecoratorProperties->SetStringField(TEXT("TextOperation"), TEXT("NotContain"));
	DecoratorProperties->SetNumberField(TEXT("IntValue"), 10);
	DecoratorProperties->SetStringField(TEXT("StringValue"), TEXT("AuthoredText"));
	TSharedPtr<FJsonObject> Decorator = MakeShared<FJsonObject>();
	Decorator->SetStringField(TEXT("Id"), DecoratorGuid);
	Decorator->SetStringField(TEXT("Class"), UBTDecorator_Blackboard::StaticClass()->GetPathName());
	Decorator->SetObjectField(TEXT("Properties"), DecoratorProperties);
	TSharedPtr<FJsonObject> Tree = TreeWithTask(
		MakeShared<FJsonObject>(),
		UBTTask_Wait::StaticClass()->GetPathName());
	Tree->GetObjectField(TEXT("Root"))->GetArrayField(TEXT("Children"))[0]->AsObject()->SetArrayField(
		TEXT("Decorators"),
		{ObjectValue(Decorator)});

	const FString Target = UniqueTarget(TEXT("DecoratorOperationExisting"));
	const FAssetDocumentResult InitialApply = Apply(Service, Document(
		Target,
		Body(Tree, ObjectValue(AssetRef(BlackboardA))),
		TEXT("Create")));
	TestTrue(
		*FString::Printf(TEXT("decorator operation fixture creates: %s"), *DescribeResult(InitialApply)),
		InitialApply.IsSuccess());
	if (!InitialApply.IsSuccess())
	{
		return false;
	}

	UBehaviorTree* Existing = LoadObject<UBehaviorTree>(nullptr, *ObjectPath(Target));
	UBTDecorator_Blackboard* ExistingDecorator = FindBlackboardDecoratorInstance(Existing);
	FByteProperty* OperationTypeProperty = FindFProperty<FByteProperty>(
		ExistingDecorator ? ExistingDecorator->GetClass() : nullptr,
		TEXT("OperationType"));
	FStrProperty* CachedDescriptionProperty = FindFProperty<FStrProperty>(
		ExistingDecorator ? ExistingDecorator->GetClass() : nullptr,
		TEXT("CachedDescription"));
	FIntProperty* IntValueProperty = FindFProperty<FIntProperty>(
		ExistingDecorator ? ExistingDecorator->GetClass() : nullptr,
		TEXT("IntValue"));
	FStrProperty* StringValueProperty = FindFProperty<FStrProperty>(
		ExistingDecorator ? ExistingDecorator->GetClass() : nullptr,
		TEXT("StringValue"));
	TestNotNull(TEXT("decorator operation fixture has its decorator"), ExistingDecorator);
	TestNotNull(TEXT("decorator operation fixture reflects OperationType"), OperationTypeProperty);
	TestNotNull(TEXT("decorator operation fixture reflects CachedDescription"), CachedDescriptionProperty);
	if (!ExistingDecorator || !OperationTypeProperty || !CachedDescriptionProperty || !IntValueProperty || !StringValueProperty)
	{
		return false;
	}
	TestEqual(
		TEXT("create projects non-default BasicOperation into OperationType"),
		OperationTypeProperty->GetPropertyValue_InContainer(ExistingDecorator),
		static_cast<uint8>(EBasicKeyOperation::NotSet));
	const FString CreatedCachedDescription = CachedDescriptionProperty->GetPropertyValue_InContainer(ExistingDecorator);
	const FString ExpectedBasicOperationDescription = StaticEnum<EBasicKeyOperation::Type>()
		->GetDisplayNameTextByValue(EBasicKeyOperation::NotSet)
		.ToString();
	TestTrue(
		*FString::Printf(
			TEXT("create rebuilds CachedDescription from the non-default basic operation (actual: '%s')"),
			*CreatedCachedDescription),
		CreatedCachedDescription.Contains(ExpectedBasicOperationDescription));

	// Preserve a valid baseline even while this test is RED, so the sparse
	// transition independently proves that the old Basic operation is replaced.
	OperationTypeProperty->SetPropertyValue_InContainer(
		ExistingDecorator,
		static_cast<uint8>(EBasicKeyOperation::NotSet));
	const int32 AuthoredIntValueBefore = IntValueProperty->GetPropertyValue_InContainer(ExistingDecorator);
	const FString AuthoredStringValueBefore = StringValueProperty->GetPropertyValue_InContainer(ExistingDecorator);

	TSharedPtr<FJsonObject> SparseBody = MakeShared<FJsonObject>();
	SparseBody->SetObjectField(TEXT("BlackboardAsset"), AssetRef(BlackboardB));
	const FAssetDocumentResult ApplyResult = Apply(Service, Document(Target, SparseBody, TEXT("Update")));
	TestTrue(
		*FString::Printf(TEXT("decorator operation sparse update applies: %s"), *DescribeResult(ApplyResult)),
		ApplyResult.IsSuccess());

	Existing = LoadObject<UBehaviorTree>(nullptr, *ObjectPath(Target));
	ExistingDecorator = FindBlackboardDecoratorInstance(Existing);
	FBlackboardKeySelector* ExistingSelector = FindBlackboardKeySelector(ExistingDecorator);
	OperationTypeProperty = FindFProperty<FByteProperty>(
		ExistingDecorator ? ExistingDecorator->GetClass() : nullptr,
		TEXT("OperationType"));
	CachedDescriptionProperty = FindFProperty<FStrProperty>(
		ExistingDecorator ? ExistingDecorator->GetClass() : nullptr,
		TEXT("CachedDescription"));
	IntValueProperty = FindFProperty<FIntProperty>(
		ExistingDecorator ? ExistingDecorator->GetClass() : nullptr,
		TEXT("IntValue"));
	StringValueProperty = FindFProperty<FStrProperty>(
		ExistingDecorator ? ExistingDecorator->GetClass() : nullptr,
		TEXT("StringValue"));
	TestEqual(TEXT("sparse update resolves the retained selector to Int"), ExistingSelector ? ExistingSelector->SelectedKeyType.Get() : nullptr, UBlackboardKeyType_Int::StaticClass());
	TestEqual(
		TEXT("sparse update projects retained ArithmeticOperation into OperationType"),
		OperationTypeProperty && ExistingDecorator ? OperationTypeProperty->GetPropertyValue_InContainer(ExistingDecorator) : MAX_uint8,
		static_cast<uint8>(EArithmeticKeyOperation::Equal));
	const FString SparseCachedDescription = CachedDescriptionProperty && ExistingDecorator
		? CachedDescriptionProperty->GetPropertyValue_InContainer(ExistingDecorator)
		: FString();
	const FString ExpectedArithmeticOperationDescription = StaticEnum<EArithmeticKeyOperation::Type>()
		->GetDisplayNameTextByValue(EArithmeticKeyOperation::Equal)
		.ToString();
	TestTrue(
		*FString::Printf(
			TEXT("sparse update rebuilds arithmetic CachedDescription (actual: '%s')"),
			*SparseCachedDescription),
		SparseCachedDescription.Contains(ExpectedArithmeticOperationDescription));
	TestEqual(
		TEXT("derived operation refresh preserves authored IntValue"),
		IntValueProperty && ExistingDecorator ? IntValueProperty->GetPropertyValue_InContainer(ExistingDecorator) : INDEX_NONE,
		AuthoredIntValueBefore);
	TestEqual(
		TEXT("derived operation refresh preserves authored StringValue"),
		StringValueProperty && ExistingDecorator ? StringValueProperty->GetPropertyValue_InContainer(ExistingDecorator) : FString(),
		AuthoredStringValueBefore);

	UWorld* TestWorld = FindAutomationWorldWithAISystem();
	TestNotNull(TEXT("decorator predicate test has an AI world"), TestWorld);
	if (TestWorld && ExistingDecorator)
	{
		UBlackboardComponent* BlackboardComponent = NewObject<UBlackboardComponent>(TestWorld);
		TestTrue(TEXT("decorator predicate Blackboard initializes"), BlackboardComponent->InitializeBlackboard(*BlackboardB));
		BlackboardComponent->SetValueAsInt(TEXT("Mode"), 10);
		UBehaviorTreeComponent* BehaviorComponent = NewObject<UBehaviorTreeComponent>(TestWorld);
		BehaviorComponent->CacheBlackboardComponent(BlackboardComponent);
		TestTrue(
			TEXT("runtime predicate uses retained arithmetic Equal instead of stale basic NotSet"),
			ExistingDecorator->CalculateRawConditionValue(*BehaviorComponent, nullptr));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeTask6ValueOrBlackboardKeyCacheReorderTest,
	"UEAssetDoc.BehaviorTree.Task6.BlackboardOverlay.ValueOrBlackboardKeyCacheRefreshesAfterKeyReorder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeTask6ValueOrBlackboardKeyCacheReorderTest::RunTest(const FString&)
{
	using namespace AssetDocumentBehaviorTreeSelectorTask6Tests;
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	UBlackboardData* BlackboardA = Blackboard(UniqueTarget(TEXT("ValueOrCacheA")));
	UBlackboardData* BlackboardB = Blackboard(UniqueTarget(TEXT("ValueOrCacheB")));
	AddKey<UBlackboardKeyType_Float>(BlackboardA, TEXT("WaitDuration"));
	AddKey<UBlackboardKeyType_Bool>(BlackboardB, TEXT("LeadingFlag"));
	AddKey<UBlackboardKeyType_Float>(BlackboardB, TEXT("WaitDuration"));
	BlackboardA->UpdateParentKeys();
	BlackboardA->UpdateKeyIDs();
	BlackboardB->UpdateParentKeys();
	BlackboardB->UpdateKeyIDs();

	TSharedPtr<FJsonObject> InitialProperties = MakeShared<FJsonObject>();
	InitialProperties->SetObjectField(TEXT("BlackboardKey"), Selector(TEXT("WaitDuration")));
	const FString Target = UniqueTarget(TEXT("ValueOrCacheExisting"));
	const FAssetDocumentResult InitialApply = Apply(Service, Document(
		Target,
		Body(TreeWithTask(InitialProperties, UBTTask_WaitBlackboardTime::StaticClass()->GetPathName()), ObjectValue(AssetRef(BlackboardA))),
		TEXT("Create")));
	TestTrue(TEXT("ValueOrBlackboardKey cache fixture creates a semantic tree"), InitialApply.IsSuccess());
	if (!InitialApply.IsSuccess())
	{
		return false;
	}

	UBehaviorTree* Existing = LoadObject<UBehaviorTree>(nullptr, *ObjectPath(Target));
	UBTTask_WaitBlackboardTime* ExistingTask = Cast<UBTTask_WaitBlackboardTime>(FindTaskInstance(Existing));
	FValueOrBBKey_Float* ExistingWaitTime = FindFloatValueOrBlackboardKey(ExistingTask, TEXT("WaitTime"));
	TestNotNull(TEXT("ValueOrBlackboardKey cache fixture reflects WaitTime"), ExistingWaitTime);
	UWorld* TestWorld = FindAutomationWorldWithAISystem();
	TestNotNull(TEXT("automation world provides an AI system"), TestWorld);
	if (!ExistingWaitTime || !TestWorld)
	{
		return false;
	}
	UBlackboardComponent* BlackboardComponentA = NewObject<UBlackboardComponent>(TestWorld);
	TestTrue(TEXT("Blackboard A component initializes"), BlackboardComponentA->InitializeBlackboard(*BlackboardA));
	BlackboardComponentA->SetValueAsFloat(TEXT("WaitDuration"), 11.25f);
	TestEqual(TEXT("initial read primes the hidden ValueOrBlackboardKey KeyId"), ExistingWaitTime->GetValue(*BlackboardComponentA), 11.25f);

	TSharedPtr<FJsonObject> SparseBody = MakeShared<FJsonObject>();
	SparseBody->SetObjectField(TEXT("BlackboardAsset"), AssetRef(BlackboardB));
	const FAssetDocumentResult ApplyResult = Apply(Service, Document(Target, SparseBody, TEXT("Update")));
	TestTrue(
		*FString::Printf(TEXT("ValueOrBlackboardKey reorder sparse update applies: %s"), *DescribeResult(ApplyResult)),
		ApplyResult.IsSuccess());

	Existing = LoadObject<UBehaviorTree>(nullptr, *ObjectPath(Target));
	ExistingTask = Cast<UBTTask_WaitBlackboardTime>(FindTaskInstance(Existing));
	ExistingWaitTime = FindFloatValueOrBlackboardKey(ExistingTask, TEXT("WaitTime"));
	UBlackboardComponent* BlackboardComponentB = NewObject<UBlackboardComponent>(TestWorld);
	TestTrue(TEXT("Blackboard B component initializes"), BlackboardComponentB->InitializeBlackboard(*BlackboardB));
	BlackboardComponentB->SetValueAsFloat(TEXT("WaitDuration"), 23.5f);
	TestEqual(
		TEXT("ValueOrBlackboardKey resolves the same-name key at its new ID"),
		ExistingWaitTime ? ExistingWaitTime->GetValue(*BlackboardComponentB) : 0.f,
		23.5f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeTask6ValueOrBlackboardKeyValidationTest,
	"UEAssetDoc.BehaviorTree.Task6.BlackboardOverlay.ValueOrBlackboardKeyValidationParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeTask6ValueOrBlackboardKeyValidationTest::RunTest(const FString&)
{
	using namespace AssetDocumentBehaviorTreeSelectorTask6Tests;
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	UBlackboardData* TestBlackboard = Blackboard(UniqueTarget(TEXT("ValueOrValidationBB")));
	UBlackboardData* MissingKeyBlackboard = Blackboard(UniqueTarget(TEXT("ValueOrMissingKeyBB")));
	UBlackboardData* WrongTypeBlackboard = Blackboard(UniqueTarget(TEXT("ValueOrWrongTypeBB")));
	AddKey<UBlackboardKeyType_Float>(TestBlackboard, TEXT("GoodDuration"));
	AddKey<UBlackboardKeyType_Bool>(TestBlackboard, TEXT("WrongDuration"));
	AddKey<UBlackboardKeyType_Bool>(MissingKeyBlackboard, TEXT("OtherKey"));
	AddKey<UBlackboardKeyType_Bool>(WrongTypeBlackboard, TEXT("GoodDuration"));
	TestBlackboard->UpdateParentKeys();
	TestBlackboard->UpdateKeyIDs();
	MissingKeyBlackboard->UpdateParentKeys();
	MissingKeyBlackboard->UpdateKeyIDs();
	WrongTypeBlackboard->UpdateParentKeys();
	WrongTypeBlackboard->UpdateKeyIDs();

	TSharedPtr<FJsonObject> LiteralProperties = MakeShared<FJsonObject>();
	LiteralProperties->SetObjectField(TEXT("WaitTime"), ValueOrFloat(TEXT(""), 2.75f));
	const FString LiteralTarget = UniqueTarget(TEXT("ValueOrLiteral"));
	BehaviorTree(LiteralTarget, TestBlackboard);
	const TSharedPtr<FJsonObject> LiteralDocument = Document(
		LiteralTarget,
		Body(
			TreeWithTask(LiteralProperties, UBTTask_Wait::StaticClass()->GetPathName()),
			ObjectValue(AssetRef(TestBlackboard))),
		TEXT("Update"));
	const FAssetDocumentResult LiteralValidate = Validate(Service, LiteralDocument);
	const FAssetDocumentResult LiteralDiff = Diff(Service, LiteralDocument);
	const FAssetDocumentResult LiteralApply = Apply(Service, LiteralDocument);
	TestTrue(
		*FString::Printf(TEXT("literal ValueOrBlackboardKey validates: %s"), *DescribeResult(LiteralValidate)),
		LiteralValidate.IsSuccess());
	TestTrue(
		*FString::Printf(TEXT("literal ValueOrBlackboardKey diffs: %s"), *DescribeResult(LiteralDiff)),
		LiteralDiff.IsSuccess() && HasEmptyPayloadArray(LiteralDiff, TEXT("failed")));
	TestTrue(
		*FString::Printf(TEXT("literal ValueOrBlackboardKey applies: %s"), *DescribeResult(LiteralApply)),
		LiteralApply.IsSuccess());

	struct FInvalidCase
	{
		const TCHAR* Label;
		UBlackboardData* DesiredBlackboard;
		const TCHAR* Code;
	};
	const FString KeyPath = TEXT("/Body/Tree/Root/Children/0/Properties/WaitTime/Key");
	for (const FInvalidCase& Case : {
		FInvalidCase{TEXT("missing"), MissingKeyBlackboard, TEXT("UnknownValueOrBlackboardKey")},
		FInvalidCase{TEXT("wrong-type"), WrongTypeBlackboard, TEXT("IncompatibleValueOrBlackboardKeyType")}})
	{
		const FString Target = UniqueTarget(TEXT("ValueOrInvalid"));
		TSharedPtr<FJsonObject> InitialProperties = MakeShared<FJsonObject>();
		InitialProperties->SetObjectField(TEXT("WaitTime"), ValueOrFloat(TEXT("GoodDuration"), 4.5f));
		const FAssetDocumentResult InitialApply = Apply(Service, Document(
			Target,
			Body(
				TreeWithTask(InitialProperties, UBTTask_Wait::StaticClass()->GetPathName()),
				ObjectValue(AssetRef(TestBlackboard))),
			TEXT("Create")));
		TestTrue(
			*FString::Printf(TEXT("%s ValueOrBlackboardKey fixture creates: %s"), Case.Label, *DescribeResult(InitialApply)),
			InitialApply.IsSuccess());
		if (!InitialApply.IsSuccess())
		{
			continue;
		}

		TSharedPtr<FJsonObject> SparseBody = MakeShared<FJsonObject>();
		SparseBody->SetObjectField(TEXT("BlackboardAsset"), AssetRef(Case.DesiredBlackboard));
		const TSharedPtr<FJsonObject> InvalidUpdate = Document(
			Target,
			SparseBody,
			TEXT("Update"));

		const FAssetDocumentResult ValidateResult = Validate(Service, InvalidUpdate);
		TestFalse(
			*FString::Printf(TEXT("%s ValueOrBlackboardKey Validate rejects invalid authored key"), Case.Label),
			ValidateResult.IsSuccess());
		TestTrue(
			*FString::Printf(TEXT("%s ValueOrBlackboardKey Validate diagnostic is exact"), Case.Label),
			HasDiagnostic(ValidateResult, KeyPath, Case.Code));

		const FAssetDocumentResult DiffResult = Diff(Service, InvalidUpdate);
		TestFalse(
			*FString::Printf(TEXT("%s ValueOrBlackboardKey Diff rejects invalid authored key"), Case.Label),
			DiffResult.IsSuccess());
		TestTrue(
			*FString::Printf(TEXT("%s ValueOrBlackboardKey Diff diagnostic is exact"), Case.Label),
			HasDiagnostic(DiffResult, KeyPath, Case.Code));

		const FAssetDocumentResult ApplyResult = Apply(Service, InvalidUpdate);
		TestFalse(
			*FString::Printf(TEXT("%s ValueOrBlackboardKey Apply rejects invalid authored key"), Case.Label),
			ApplyResult.IsSuccess());
		TestTrue(
			*FString::Printf(TEXT("%s ValueOrBlackboardKey Apply diagnostic is exact"), Case.Label),
			HasDiagnostic(ApplyResult, KeyPath, Case.Code));

		UBehaviorTree* Existing = LoadObject<UBehaviorTree>(nullptr, *ObjectPath(Target));
		UBTTask_Wait* ExistingTask = Cast<UBTTask_Wait>(FindTaskInstance(Existing));
		FValueOrBBKey_Float* ExistingWaitTime = FindFloatValueOrBlackboardKey(ExistingTask, TEXT("WaitTime"));
		TestEqual(
			*FString::Printf(TEXT("%s failed ValueOrBlackboardKey update preserves live Blackboard"), Case.Label),
			Existing ? Existing->BlackboardAsset.Get() : nullptr,
			TestBlackboard);
		TestEqual(
			*FString::Printf(TEXT("%s failed ValueOrBlackboardKey update preserves live authored key"), Case.Label),
			ExistingWaitTime ? ExistingWaitTime->GetKey() : NAME_None,
			FName(TEXT("GoodDuration")));
		TestEqual(
			*FString::Printf(TEXT("%s failed ValueOrBlackboardKey update preserves live authored default"), Case.Label),
			ExistingWaitTime ? ExistingWaitTime->GetValue(static_cast<const UBlackboardComponent*>(nullptr)) : 0.f,
			4.5f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeTask6EnumDecoratorSparseMismatchTest,
	"UEAssetDoc.BehaviorTree.Task6.BlackboardOverlay.EnumDecoratorMappingRequiresTreeUpdate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeTask6EnumDecoratorSparseMismatchTest::RunTest(const FString&)
{
	using namespace AssetDocumentBehaviorTreeSelectorTask6Tests;
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	UEnum* EnumA = TestEnum(UniqueTarget(TEXT("EnumMappingA")), 1);
	UEnum* EnumB = TestEnum(UniqueTarget(TEXT("EnumMappingB")), 7);
	UEnum* EnumC = TestEnum(UniqueTarget(TEXT("EnumMappingC")), 9, TEXT("ReplacementValue"));
	TestNotNull(TEXT("enum mapping A fixture creates"), EnumA);
	TestNotNull(TEXT("enum mapping B fixture creates"), EnumB);
	TestNotNull(TEXT("enum mapping C fixture creates"), EnumC);
	if (!EnumA || !EnumB || !EnumC)
	{
		return false;
	}
	UBlackboardData* BlackboardA = Blackboard(UniqueTarget(TEXT("EnumBlackboardA")));
	UBlackboardData* BlackboardB = Blackboard(UniqueTarget(TEXT("EnumBlackboardB")));
	UBlackboardData* BlackboardC = Blackboard(UniqueTarget(TEXT("EnumBlackboardC")));
	AddEnumKey(BlackboardA, TEXT("Mode"), EnumA);
	AddEnumKey(BlackboardB, TEXT("Mode"), EnumB);
	AddEnumKey(BlackboardC, TEXT("Mode"), EnumC);

	TSharedPtr<FJsonObject> DecoratorProperties = MakeShared<FJsonObject>();
	DecoratorProperties->SetObjectField(TEXT("BlackboardKey"), Selector(TEXT("Mode")));
	DecoratorProperties->SetStringField(TEXT("StringValue"), TEXT("SharedValue"));
	DecoratorProperties->SetNumberField(TEXT("IntValue"), 1);
	TSharedPtr<FJsonObject> Decorator = MakeShared<FJsonObject>();
	Decorator->SetStringField(TEXT("Id"), DecoratorGuid);
	Decorator->SetStringField(TEXT("Class"), UBTDecorator_Blackboard::StaticClass()->GetPathName());
	Decorator->SetObjectField(TEXT("Properties"), DecoratorProperties);
	TSharedPtr<FJsonObject> Tree = TreeWithTask(
		MakeShared<FJsonObject>(),
		UBTTask_Wait::StaticClass()->GetPathName());
	Tree->GetObjectField(TEXT("Root"))->GetArrayField(TEXT("Children"))[0]->AsObject()->SetArrayField(
		TEXT("Decorators"),
		{ObjectValue(Decorator)});

	const FString Target = UniqueTarget(TEXT("EnumDecoratorExisting"));
	const FAssetDocumentResult InitialApply = Apply(Service, Document(
		Target,
		Body(Tree, ObjectValue(AssetRef(BlackboardA))),
		TEXT("Create")));
	TestTrue(
		*FString::Printf(TEXT("enum decorator fixture creates: %s"), *DescribeResult(InitialApply)),
		InitialApply.IsSuccess());
	if (!InitialApply.IsSuccess())
	{
		return false;
	}

	TSharedPtr<FJsonObject> SparseBody = MakeShared<FJsonObject>();
	SparseBody->SetObjectField(TEXT("BlackboardAsset"), AssetRef(BlackboardB));
	const TSharedPtr<FJsonObject> SparseUpdate = Document(Target, SparseBody, TEXT("Update"));
	const FString IntValuePath = TEXT("/Body/Tree/Root/Children/0/Decorators/0/Properties/IntValue");
	const FString DiagnosticCode = TEXT("BehaviorTreeEnumDecoratorValueOutOfSync");
	for (const FAssetDocumentResult& Result : {
		Validate(Service, SparseUpdate),
		Diff(Service, SparseUpdate),
		Apply(Service, SparseUpdate)})
	{
		TestFalse(TEXT("sparse enum remap requires an authored Tree update"), Result.IsSuccess());
		TestTrue(TEXT("sparse enum remap reports the exact stale IntValue"), HasDiagnostic(Result, IntValuePath, DiagnosticCode));
	}

	UBehaviorTree* Existing = LoadObject<UBehaviorTree>(nullptr, *ObjectPath(Target));
	UBTDecorator_Blackboard* ExistingDecorator = FindBlackboardDecoratorInstance(Existing);
	FIntProperty* IntValueProperty = FindFProperty<FIntProperty>(
		ExistingDecorator ? ExistingDecorator->GetClass() : nullptr,
		TEXT("IntValue"));
	TestEqual(TEXT("rejected enum remap preserves the existing Blackboard"), Existing ? Existing->BlackboardAsset.Get() : nullptr, BlackboardA);
	TestEqual(
		TEXT("rejected enum remap preserves authored decorator IntValue"),
		IntValueProperty && ExistingDecorator ? IntValueProperty->GetPropertyValue_InContainer(ExistingDecorator) : INDEX_NONE,
		1);

	TSharedPtr<FJsonObject> MissingValueBody = MakeShared<FJsonObject>();
	MissingValueBody->SetObjectField(TEXT("BlackboardAsset"), AssetRef(BlackboardC));
	const TSharedPtr<FJsonObject> MissingValueUpdate = Document(Target, MissingValueBody, TEXT("Update"));
	const FString StringValuePath = TEXT("/Body/Tree/Root/Children/0/Decorators/0/Properties/StringValue");
	for (const FAssetDocumentResult& Result : {
		Validate(Service, MissingValueUpdate),
		Diff(Service, MissingValueUpdate),
		Apply(Service, MissingValueUpdate)})
	{
		TestFalse(TEXT("sparse enum replacement rejects a missing retained StringValue"), Result.IsSuccess());
		TestTrue(TEXT("missing enum value reports the exact retained StringValue"), HasDiagnostic(
			Result,
			StringValuePath,
			TEXT("UnknownBehaviorTreeEnumDecoratorValue")));
	}

	Existing = LoadObject<UBehaviorTree>(nullptr, *ObjectPath(Target));
	ExistingDecorator = FindBlackboardDecoratorInstance(Existing);
	IntValueProperty = FindFProperty<FIntProperty>(
		ExistingDecorator ? ExistingDecorator->GetClass() : nullptr,
		TEXT("IntValue"));
	TestEqual(TEXT("rejected missing enum value preserves the existing Blackboard"), Existing ? Existing->BlackboardAsset.Get() : nullptr, BlackboardA);
	TestEqual(
		TEXT("rejected missing enum value preserves authored decorator IntValue"),
		IntValueProperty && ExistingDecorator ? IntValueProperty->GetPropertyValue_InContainer(ExistingDecorator) : INDEX_NONE,
		1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeTask6SelectorPreSaveReloadTest,
	"UEAssetDoc.BehaviorTree.Task6.Selectors.PreSaveAndReloadStability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeTask6SelectorPreSaveReloadTest::RunTest(const FString&)
{
	using namespace AssetDocumentBehaviorTreeSelectorTask6Tests;
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	UBlackboardData* BlackboardAsset = Blackboard(UniqueTarget(TEXT("ReloadBB")));
	AddObjectKey(BlackboardAsset, TEXT("RequestedActor"), APawn::StaticClass());
	UPackage* BlackboardPackage = BlackboardAsset->GetOutermost();
	FAssetRegistryModule::AssetCreated(BlackboardAsset);
	BlackboardPackage->MarkPackageDirty();
	FSavePackageArgs SaveBlackboardArgs;
	SaveBlackboardArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveBlackboardArgs.SaveFlags = SAVE_NoError;
	const FString BlackboardFileName = FPackageName::LongPackageNameToFilename(
		BlackboardPackage->GetName(),
		FPackageName::GetAssetPackageExtension());
	const bool bSavedBlackboard =
		UPackage::SavePackage(BlackboardPackage, BlackboardAsset, *BlackboardFileName, SaveBlackboardArgs);
	TestTrue(
		TEXT("fresh-reload Blackboard fixture saves independently"),
		bSavedBlackboard);
	if (!bSavedBlackboard)
	{
		return false;
	}
	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetObjectField(TEXT("DirectSelector"), Selector(TEXT("RequestedActor")));
	const FString Target = UniqueTarget(TEXT("SelectorReload"));
	const FString PackageFileName = FPackageName::LongPackageNameToFilename(
		Target,
		FPackageName::GetAssetPackageExtension());
	ON_SCOPE_EXIT
	{
		const FString SidecarPath = FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(Target);
		FAssetDocumentEditorSync::FScopedSidecarWrite Guard(SidecarPath);
		FScopedBehaviorTreeDurableDependencies::FlushFileEvents();
		IFileManager::Get().Delete(*SidecarPath, false, true);
		IFileManager::Get().Delete(*PackageFileName, false, true);
		IFileManager::Get().Delete(*BlackboardFileName, false, true);
		IFileManager::Get().DeleteDirectory(*FPaths::GetPath(SidecarPath), false, false);
		FScopedBehaviorTreeDurableDependencies::FlushFileEvents();
	};
	const TSharedPtr<FJsonObject> Value = Document(
		Target,
		Body(TreeWithTask(Properties), ObjectValue(AssetRef(BlackboardAsset))),
		TEXT("Create"));
	const FAssetDocumentResult ApplyResult = Apply(Service, Value, true);
	TestTrue(TEXT("valid selector saves without PreSave healing"), ApplyResult.IsSuccess());
	if (!ApplyResult.IsSuccess())
	{
		return false;
	}

	UBehaviorTree* BeforeReload = LoadObject<UBehaviorTree>(nullptr, *ObjectPath(Target));
	UAssetDocumentSelectorTaskTestNode* BeforeTask = Cast<UAssetDocumentSelectorTaskTestNode>(FindTaskInstance(BeforeReload));
	TestNotNull(TEXT("saved selector task exists"), BeforeTask);
	if (BeforeTask)
	{
		TestEqual(TEXT("PreSave keeps exact requested key"), BeforeTask->DirectSelector.SelectedKeyName, FName(TEXT("RequestedActor")));
		TestTrue(TEXT("resolved selector ID is cached before reload"), BeforeTask->DirectSelector.GetSelectedKeyID() != FBlackboard::InvalidKey);
		TestEqual(TEXT("resolved selector type is cached before reload"), BeforeTask->DirectSelector.SelectedKeyType.Get(), UBlackboardKeyType_Object::StaticClass());
	}

	UPackage* Package = BeforeReload ? BeforeReload->GetOutermost() : nullptr;
	TestTrue(TEXT("selector package unloads for a fresh read"), Package && UPackageTools::UnloadPackages({Package}));
	UBehaviorTree* Reloaded = LoadObject<UBehaviorTree>(nullptr, *ObjectPath(Target));
	UAssetDocumentSelectorTaskTestNode* ReloadedTask = Cast<UAssetDocumentSelectorTaskTestNode>(FindTaskInstance(Reloaded));
	TestNotNull(TEXT("selector task survives fresh reload"), ReloadedTask);
	if (ReloadedTask)
	{
		TestEqual(TEXT("fresh reload keeps authored key"), ReloadedTask->DirectSelector.SelectedKeyName, FName(TEXT("RequestedActor")));
		UPackage* ReloadedPackage = Reloaded->GetOutermost();
		const bool bWasDirty = ReloadedPackage->IsDirty();
		// An omitted-Tree public update runs the provider's selector completion
		// against this freshly loaded graph without replacing its authored nodes.
		const FAssetDocumentResult RefreshResult = Apply(
			Service, Document(Target, MakeShared<FJsonObject>(), TEXT("Update")), true);
		FString RefreshDetails = RefreshResult.Message;
		for (const FAssetDocumentDiagnostic& Diagnostic : RefreshResult.Diagnostics)
		{
			RefreshDetails += FString::Printf(
				TEXT(" [%s %s: %s]"),
				*Diagnostic.Code,
				*Diagnostic.Path,
				*Diagnostic.Message);
		}
		TestTrue(
			*FString::Printf(TEXT("explicit editor lifecycle refresh succeeds: %s"), *RefreshDetails),
			RefreshResult.IsSuccess());
		TestEqual(TEXT("derived refresh preserves authored key"), ReloadedTask->DirectSelector.SelectedKeyName, FName(TEXT("RequestedActor")));
		TestTrue(TEXT("derived refresh restores resolved key ID"), ReloadedTask->DirectSelector.GetSelectedKeyID() != FBlackboard::InvalidKey);
		TestEqual(TEXT("derived refresh restores resolved key type"), ReloadedTask->DirectSelector.SelectedKeyType.Get(), UBlackboardKeyType_Object::StaticClass());
		TestEqual(TEXT("derived refresh preserves package dirty state"), ReloadedPackage->IsDirty(), bWasDirty);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeTask6SubtreeMatrixTest,
	"UEAssetDoc.BehaviorTree.Task6.Subtrees.ConservativeMatrix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeTask6SubtreeMatrixTest::RunTest(const FString&)
{
	using namespace AssetDocumentBehaviorTreeSelectorTask6Tests;
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	UBlackboardData* Parent = Blackboard(UniqueTarget(TEXT("SubtreeParent")));
	UBlackboardData* Child = Blackboard(UniqueTarget(TEXT("SubtreeChild")));
	UBlackboardData* Sibling = Blackboard(UniqueTarget(TEXT("SubtreeSibling")));
	UBlackboardData* StructuralTwin = Blackboard(UniqueTarget(TEXT("SubtreeStructuralTwin")));
	AddObjectKey(Parent, TEXT("Actor"), AActor::StaticClass());
	AddKey<UBlackboardKeyType_Bool>(Child, TEXT("ChildOnly"));
	AddKey<UBlackboardKeyType_Vector>(Sibling, TEXT("SiblingOnly"));
	AddObjectKey(StructuralTwin, TEXT("Actor"), AActor::StaticClass());
	Child->Parent = Parent;
	Sibling->Parent = Parent;
	// Stock Parent editing removes each child's local persistent SelfActor key.
	Child->UpdateParentKeys();
	Sibling->UpdateParentKeys();

	UBehaviorTree* SameTree = BehaviorTree(UniqueTarget(TEXT("SameSubtree")), Child);
	UBehaviorTree* ParentTree = BehaviorTree(UniqueTarget(TEXT("ParentSubtree")), Parent);
	UBehaviorTree* ChildTree = BehaviorTree(UniqueTarget(TEXT("ChildSubtree")), Child);
	UBehaviorTree* SiblingTree = BehaviorTree(UniqueTarget(TEXT("SiblingSubtree")), Sibling);
	UBehaviorTree* StructuralTree = BehaviorTree(UniqueTarget(TEXT("StructuralSubtree")), StructuralTwin);
	UBehaviorTree* NullTree = BehaviorTree(UniqueTarget(TEXT("NullSubtree")), nullptr);

	struct FCase
	{
		const TCHAR* Label;
		UBlackboardData* Desired;
		UBehaviorTree* Subtree;
		bool bExpectedSuccess;
	};
	const TArray<FCase> Cases = {
		{TEXT("same"), Child, SameTree, true},
		{TEXT("desired-descends-from-subtree"), Child, ParentTree, true},
		{TEXT("reverse-ancestry"), Parent, ChildTree, false},
		{TEXT("siblings"), Child, SiblingTree, false},
		{TEXT("structural-only"), Parent, StructuralTree, false},
		{TEXT("static-null-blackboard"), Parent, NullTree, false},
	};
	for (const FCase& Case : Cases)
	{
		const TSharedPtr<FJsonObject> Value = Document(
			UniqueTarget(TEXT("SubtreeMatrix")),
			Body(SubtreeTaskTree(Case.Subtree, false), ObjectValue(AssetRef(Case.Desired))),
			TEXT("Create"));
		const FAssetDocumentResult Result = Validate(Service, Value);
		TestEqual(*FString::Printf(TEXT("%s subtree compatibility"), Case.Label), Result.IsSuccess(), Case.bExpectedSuccess);
		if (!Case.bExpectedSuccess)
		{
			TestTrue(
				*FString::Printf(TEXT("%s subtree diagnostic is exact"), Case.Label),
				HasDiagnostic(Result, TEXT("/Body/Tree/Root/Children/0/Properties/BehaviorAsset"), TEXT("IncompatibleBehaviorTreeBlackboard")));
		}
	}

	UBehaviorTree* DecoratedDynamicTree = BehaviorTree(UniqueTarget(TEXT("DecoratedDynamicSubtree")), Parent);
	DecoratedDynamicTree->RootDecorators.Add(NewObject<UBTDecorator_Blackboard>(DecoratedDynamicTree));
	const TSharedPtr<FJsonObject> DynamicDecorated = Document(
		UniqueTarget(TEXT("DynamicDecorated")),
		Body(SubtreeTaskTree(DecoratedDynamicTree, true), ObjectValue(AssetRef(Parent))),
		TEXT("Create"));
	const FAssetDocumentResult DynamicResult = Validate(Service, DynamicDecorated);
	TestFalse(TEXT("dynamic subtree rejects root decorators"), DynamicResult.IsSuccess());
	TestTrue(TEXT("dynamic root-decorator diagnostic is exact"), HasDiagnostic(
		DynamicResult,
		TEXT("/Body/Tree/Root/Children/0/Properties/DefaultBehaviorAsset"),
		TEXT("DynamicSubtreeRootDecoratorsUnsupported")));

	const TSharedPtr<FJsonObject> DynamicNull = Document(
		UniqueTarget(TEXT("DynamicNull")),
		Body(SubtreeTaskTree(nullptr, true), ObjectValue(AssetRef(Parent))),
		TEXT("Create"));
	TestTrue(TEXT("dynamic subtree allows an explicit null default"), Validate(Service, DynamicNull).IsSuccess());

	ParentTree->RootDecorators.Add(NewObject<UBTDecorator_Blackboard>(ParentTree));
	const FString RepeatedTarget = UniqueTarget(TEXT("RepeatedSubtree"));
	const TSharedPtr<FJsonObject> Repeated = Document(
		RepeatedTarget,
		Body(SubtreeTaskTree(ParentTree, false), ObjectValue(AssetRef(Child))),
		TEXT("CreateOrUpdate"));
	TestTrue(TEXT("first compatible subtree apply succeeds"), Apply(Service, Repeated).IsSuccess());
	TestTrue(TEXT("repeated compatible subtree apply succeeds"), Apply(Service, Repeated).IsSuccess());
	const FAssetDocumentResult RepeatedDiff = Diff(Service, Repeated);
	TestTrue(TEXT("repeated subtree diff succeeds"), RepeatedDiff.IsSuccess());
	FAssetDocumentExtractRequest StaticExtractRequest;
	StaticExtractRequest.AssetPath = RepeatedTarget;
	StaticExtractRequest.bDiffOnly = false;
	StaticExtractRequest.bIncludeAllWritable = true;
	const FAssetDocumentResult StaticExtract = Service.Extract(StaticExtractRequest);
	TestTrue(TEXT("static subtree extract succeeds after injected preview refresh"), StaticExtract.IsSuccess());
	const TSharedPtr<FJsonObject> StaticBody = StaticExtract.Payload.IsValid() ? StaticExtract.Payload->GetObjectField(TEXT("Body")) : nullptr;
	const TSharedPtr<FJsonObject> StaticTree = StaticBody.IsValid() ? StaticBody->GetObjectField(TEXT("Tree")) : nullptr;
	const TSharedPtr<FJsonObject> StaticRoot = StaticTree.IsValid() ? StaticTree->GetObjectField(TEXT("Root")) : nullptr;
	const TArray<TSharedPtr<FJsonValue>>* StaticChildren = nullptr;
	const TSharedPtr<FJsonObject> StaticTask = StaticRoot.IsValid()
		&& StaticRoot->TryGetArrayField(TEXT("Children"), StaticChildren)
		&& StaticChildren
		&& StaticChildren->Num() == 1
		? (*StaticChildren)[0]->AsObject()
		: nullptr;
	const TArray<TSharedPtr<FJsonValue>>* StaticDecorators = nullptr;
	TestTrue(TEXT("injected static subtree decorators stay out of canonical extract"),
		StaticTask.IsValid()
		&& StaticTask->TryGetArrayField(TEXT("Decorators"), StaticDecorators)
		&& StaticDecorators
		&& StaticDecorators->Num() == 0);

	const FString DynamicTarget = UniqueTarget(TEXT("DynamicRuntimeOwned"));
	const TSharedPtr<FJsonObject> DynamicDocument = Document(
		DynamicTarget,
		Body(SubtreeTaskTree(SameTree, true), ObjectValue(AssetRef(Child))),
		TEXT("Create"));
	TestTrue(TEXT("dynamic subtree default applies"), Apply(Service, DynamicDocument).IsSuccess());
	UBehaviorTree* DynamicAsset = LoadObject<UBehaviorTree>(nullptr, *ObjectPath(DynamicTarget));
	UBTTask_RunBehaviorDynamic* DynamicTask = Cast<UBTTask_RunBehaviorDynamic>(FindTaskInstance(DynamicAsset));
	TestNotNull(TEXT("dynamic subtree task exists"), DynamicTask);
	TestTrue(TEXT("runtime BehaviorAsset can differ from authored default"), DynamicTask && DynamicTask->SetBehaviorAsset(ParentTree));
	FAssetDocumentExtractRequest DynamicExtractRequest;
	DynamicExtractRequest.AssetPath = DynamicTarget;
	DynamicExtractRequest.bDiffOnly = false;
	DynamicExtractRequest.bIncludeAllWritable = true;
	const FAssetDocumentResult DynamicExtract = Service.Extract(DynamicExtractRequest);
	const TSharedPtr<FJsonObject> DynamicBody = DynamicExtract.Payload.IsValid() ? DynamicExtract.Payload->GetObjectField(TEXT("Body")) : nullptr;
	const TSharedPtr<FJsonObject> DynamicTree = DynamicBody.IsValid() ? DynamicBody->GetObjectField(TEXT("Tree")) : nullptr;
	const TSharedPtr<FJsonObject> DynamicRoot = DynamicTree.IsValid() ? DynamicTree->GetObjectField(TEXT("Root")) : nullptr;
	const TArray<TSharedPtr<FJsonValue>>* DynamicChildren = nullptr;
	const TSharedPtr<FJsonObject> ExtractedDynamicTask = DynamicRoot.IsValid()
		&& DynamicRoot->TryGetArrayField(TEXT("Children"), DynamicChildren)
		&& DynamicChildren
		&& DynamicChildren->Num() == 1
		? (*DynamicChildren)[0]->AsObject()
		: nullptr;
	const TSharedPtr<FJsonObject> DynamicProperties = ExtractedDynamicTask.IsValid()
		? ExtractedDynamicTask->GetObjectField(TEXT("Properties"))
		: nullptr;
	TestTrue(TEXT("dynamic extract keeps authored DefaultBehaviorAsset"), DynamicProperties.IsValid() && DynamicProperties->HasField(TEXT("DefaultBehaviorAsset")));
	TestFalse(TEXT("dynamic extract excludes runtime-owned BehaviorAsset"), DynamicProperties.IsValid() && DynamicProperties->HasField(TEXT("BehaviorAsset")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeTask6FailedPreflightPollutionTest,
	"UEAssetDoc.BehaviorTree.Task6.Preflight.FailureIsPollutionFree",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeTask6FailedPreflightPollutionTest::RunTest(const FString&)
{
	using namespace AssetDocumentBehaviorTreeSelectorTask6Tests;
	UBlackboardData* BlackboardAsset = Blackboard(UniqueTarget(TEXT("PollutionBB")));
	AddKey<UBlackboardKeyType_Vector>(BlackboardAsset, TEXT("WrongType"));
	const FString Target = UniqueTarget(TEXT("PollutionExisting"));
	UBehaviorTree* Existing = BehaviorTree(Target, BlackboardAsset);
	UPackage* Package = Existing->GetOutermost();
	Package->SetDirtyFlag(false);
	const int32 ObjectsBefore = CountRecursiveObjects(Existing);
	const int32 RegistryBefore = CountRegistryAssets(Package->GetFName());
	const bool bDirtyBefore = Package->IsDirty();

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetObjectField(TEXT("DirectSelector"), Selector(TEXT("WrongType")));
	const TSharedPtr<FJsonObject> BodyValue = Body(TreeWithTask(Properties), ObjectValue(AssetRef(BlackboardAsset)));
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	const FAssetDocumentResult Result = Validate(Service, Document(Target, BodyValue, TEXT("Update")));
	TestFalse(TEXT("public validation rejects cross-region selector mismatch before mutation"), Result.IsSuccess());
	TestEqual(TEXT("failed preflight creates no recursive production objects"), CountRecursiveObjects(Existing), ObjectsBefore);
	TestEqual(TEXT("failed preflight leaves package dirty state unchanged"), Package->IsDirty(), bDirtyBefore);
	TestEqual(TEXT("failed preflight does not register assets"), CountRegistryAssets(Package->GetFName()), RegistryBefore);
	TestNull(TEXT("failed preflight leaves production BTGraph absent"), Existing->BTGraph.Get());
	TestNull(TEXT("failed preflight leaves production runtime root absent"), Existing->RootNode.Get());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBehaviorTreeTask6SelectorInspectionTest,
	"UEAssetDoc.BehaviorTree.Task6.Selectors.InspectNativeBlueprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeTask6SelectorInspectionTest::RunTest(const FString&)
{
	using namespace AssetDocumentBehaviorTreeSelectorTask6Tests;
	IAssetDocumentRuntimeModule* Integration =
		FModuleManager::LoadModulePtr<IAssetDocumentRuntimeModule>(
			TEXT("AssetDocumentIntegration"));
	TestNotNull(
		TEXT("selector Inspect loads the Integration runtime owner"),
		Integration);
	TestTrue(
		TEXT("selector Inspect uses the active Integration runtime"),
		Integration && Integration->HasActiveRuntimeRegistration());
	if (!Integration || !Integration->HasActiveRuntimeRegistration())
	{
		return false;
	}
	FAssetDocumentService& Service =
		FAssetDocumentModule::Get().GetService();
	TArray<UClass*> Classes{UAssetDocumentSelectorTaskTestNode::StaticClass()};

	const FString BlueprintTarget = UniqueTarget(TEXT("SelectorBlueprint"));
	UPackage* BlueprintPackage = CreatePackage(*BlueprintTarget);
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		UAssetDocumentSelectorTaskTestNode::StaticClass(),
		BlueprintPackage,
		FName(*FPackageName::GetLongPackageAssetName(BlueprintTarget)),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	ON_SCOPE_EXIT
	{
		FScopedBehaviorTreeDurableDependencies::FlushFileEvents();
		DeleteBehaviorTreeFixtureLoadedAsset(Blueprint);
		FScopedBehaviorTreeDurableDependencies::FlushFileEvents();
	};
	TestNotNull(
		TEXT("project Blueprint selector fixture compiles"),
		Blueprint && Blueprint->GeneratedClass ? Blueprint->GeneratedClass.Get() : nullptr);
	if (Blueprint && Blueprint->GeneratedClass)
	{
		Classes.Add(Blueprint->GeneratedClass);
	}

	for (UClass* Class : Classes)
	{
		FAssetDocumentInspectRequest Request;
		Request.ClassOrAsset = Class->GetPathName();
		const FAssetDocumentResult Result = Service.Inspect(Request);
		TestTrue(
			*FString::Printf(
				TEXT("selector Inspect succeeds for %s: %s"),
				*Class->GetPathName(),
				*DescribeResult(Result)),
			Result.IsSuccess());
		const TSharedPtr<FJsonObject> Policy = FindSelectorPolicy(Result.Payload, TEXT("DirectSelector"));
		TestNotNull(*FString::Printf(TEXT("selector Inspect exposes policy for %s"), *Class->GetPathName()), Policy.Get());
		if (Policy)
		{
			TestEqual(TEXT("selector policy has stable key authoring field"), Policy->GetStringField(TEXT("authored_field")), FString(TEXT("Key")));
			TestTrue(TEXT("selector policy is explicitly read-only metadata"), Policy->GetBoolField(TEXT("read_only")));
			const FString Canonical = FAssetDocumentJsonRegionUtils::JsonValueToComparableString(ObjectValue(Policy));
			TestFalse(TEXT("selector policy contains no transient object path"), Canonical.Contains(TEXT("/Engine/Transient")));
		}
		for (const FString& RecursivePath : {
			TEXT("NestedValue/Selector"),
			TEXT("ArrayValues/*/Selector"),
			TEXT("MapValues/*/Selector"),
			TEXT("SetValues/*/Selector"),
			TEXT("DynamicValue/Properties/Selector")})
		{
			TestNotNull(
				*FString::Printf(TEXT("selector Inspect exposes stable recursive schema path %s for %s"), *RecursivePath, *Class->GetPathName()),
				FindSelectorPolicy(Result.Payload, RecursivePath).Get());
		}
	}

	UAssetDocumentSelectorTaskTestNode* PopulatedInstance = NewObject<UAssetDocumentSelectorTaskTestNode>(GetTransientPackage());
	FAssetDocumentReflectedPropertyNestedTestValue MapEntry;
	MapEntry.Identity = TEXT("EscapedKeyProbe");
	PopulatedInstance->MapValues.Add(FName(TEXT("A/B~C")), MapEntry);
	const TSharedPtr<FJsonObject> PopulatedPayload = FAssetDocumentPropertyAdapter::InspectProperties(
		UAssetDocumentSelectorTaskTestNode::StaticClass(),
		PopulatedInstance);
	TestNotNull(
		TEXT("populated map Inspect keeps the selector schema wildcard"),
		FindSelectorPolicy(PopulatedPayload, TEXT("MapValues/*/Selector")).Get());
	const TArray<TSharedPtr<FJsonValue>>* PopulatedPolicies = nullptr;
	bool bLeaksRawMapKey = false;
	bool bBakesEscapedMapKey = false;
	if (PopulatedPayload.IsValid()
		&& PopulatedPayload->TryGetArrayField(TEXT("selector_policies"), PopulatedPolicies)
		&& PopulatedPolicies)
	{
		for (const TSharedPtr<FJsonValue>& PolicyValue : *PopulatedPolicies)
		{
			FString Path;
			const TSharedPtr<FJsonObject> Policy = PolicyValue.IsValid() ? PolicyValue->AsObject() : nullptr;
			if (Policy.IsValid() && Policy->TryGetStringField(TEXT("path"), Path))
			{
				bLeaksRawMapKey |= Path.Contains(TEXT("A/B~C"));
				bBakesEscapedMapKey |= Path.Contains(TEXT("A~1B~0C"));
			}
		}
	}
	TestFalse(TEXT("selector policy paths do not leak a raw map key"), bLeaksRawMapKey);
	TestFalse(TEXT("selector policy paths do not bake an escaped map key"), bBakesEscapedMapKey);
	return true;
}

#endif
