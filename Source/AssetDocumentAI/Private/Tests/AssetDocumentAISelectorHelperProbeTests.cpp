// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "IDetailTreeNode.h"
#include "IPropertyRowGenerator.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Package.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace AssetDocumentAISelectorHelperProbeTests
{

struct FSelectorPolicyEvidence
{
	bool bReadOnly = false;
	bool bNoneAllowed = false;
	FString FilterType;
	FString BaseClass;
};

const TArray<FString>& RequiredSchemaPaths()
{
	static const TArray<FString> Paths = {
		TEXT("ArrayValues/*/Selector"),
		TEXT("DirectSelector"),
		TEXT("DynamicValue/Properties/Selector"),
		TEXT("MapValues/*/Selector"),
		TEXT("NestedValue/Selector"),
		TEXT("SetValues/*/Selector"),
	};
	return Paths;
}

FString NormalizePropertyPath(FString Path, bool bNestedInInstancedStruct = false)
{
	Path.ReplaceInline(TEXT("."), TEXT("/"));
	Path.ReplaceInline(TEXT("->"), bNestedInInstancedStruct ? TEXT("/Properties/") : TEXT("/"));
	return Path;
}

bool IsBlackboardSelectorHandle(const TSharedPtr<IPropertyHandle>& Handle)
{
	const FStructProperty* StructProperty = Handle.IsValid()
		? CastField<FStructProperty>(Handle->GetProperty())
		: nullptr;
	return StructProperty && StructProperty->Struct == FBlackboardKeySelector::StaticStruct();
}

bool IsInstancedStructHandle(const TSharedPtr<IPropertyHandle>& Handle)
{
	const FStructProperty* StructProperty = Handle.IsValid()
		? CastField<FStructProperty>(Handle->GetProperty())
		: nullptr;
	return StructProperty
		&& StructProperty->Struct
		&& StructProperty->Struct == FInstancedStruct::StaticStruct();
}

struct FObservedPropertyHandle
{
	TSharedPtr<IPropertyHandle> Handle;
	bool bNestedInInstancedStruct = false;
};

void CollectPropertyHandles(
	const TArray<TSharedRef<IDetailTreeNode>>& Nodes,
	TArray<FObservedPropertyHandle>& OutHandles,
	bool bNestedInInstancedStruct = false)
{
	for (const TSharedRef<IDetailTreeNode>& Node : Nodes)
	{
		const TSharedPtr<IPropertyHandle> Handle = Node->CreatePropertyHandle();
		if (Handle.IsValid())
		{
			OutHandles.Add({Handle, bNestedInInstancedStruct});
		}

		TArray<TSharedRef<IDetailTreeNode>> Children;
		Node->GetChildren(Children, true);
		CollectPropertyHandles(Children, OutHandles, bNestedInInstancedStruct || IsInstancedStructHandle(Handle));
	}
}

struct FCollectedPropertyHandles
{
	TSharedPtr<IPropertyRowGenerator> Generator;
	TArray<FObservedPropertyHandle> Handles;
};

FCollectedPropertyHandles CollectPropertyHandles(UObject& Subject)
{
	FPropertyRowGeneratorArgs Args;
	Args.bAllowEditingClassDefaultObjects = true;
	FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	FCollectedPropertyHandles Result;
	Result.Generator = PropertyEditor.CreatePropertyRowGenerator(Args);
	Result.Generator->OnRowsRefreshed().AddLambda([]() {});
	Result.Generator->SetObjects({&Subject});

	CollectPropertyHandles(Result.Generator->GetRootTreeNodes(), Result.Handles);
	return Result;
}

TArray<FString> CollectStructSelectorPaths(UScriptStruct* Struct)
{
	if (!Struct)
	{
		return {};
	}

	FPropertyRowGeneratorArgs Args;
	FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	const TSharedRef<IPropertyRowGenerator> Generator = PropertyEditor.CreatePropertyRowGenerator(Args);
	Generator->OnRowsRefreshed().AddLambda([]() {});
	Generator->SetStructure(MakeShared<FStructOnScope>(Struct));

	TArray<FObservedPropertyHandle> Handles;
	CollectPropertyHandles(Generator->GetRootTreeNodes(), Handles);

	TArray<FString> Paths;
	for (const FObservedPropertyHandle& ObservedHandle : Handles)
	{
		if (IsBlackboardSelectorHandle(ObservedHandle.Handle))
		{
			Paths.Add(NormalizePropertyPath(
				FString(ObservedHandle.Handle->GetPropertyPath()),
				ObservedHandle.bNestedInInstancedStruct));
		}
	}
	return Paths;
}

const FStructProperty* ContainerElementStruct(const TSharedPtr<IPropertyHandle>& Handle)
{
	if (!Handle.IsValid())
	{
		return nullptr;
	}

	if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Handle->GetProperty()))
	{
		if (const TSharedPtr<IPropertyHandleArray> ArrayHandle = Handle->AsArray())
		{
			uint32 ElementCount = 0;
			ArrayHandle->GetNumElements(ElementCount);
		}
		return CastField<FStructProperty>(ArrayProperty->Inner);
	}

	if (const FMapProperty* MapProperty = CastField<FMapProperty>(Handle->GetProperty()))
	{
		if (const TSharedPtr<IPropertyHandleMap> MapHandle = Handle->AsMap())
		{
			uint32 ElementCount = 0;
			MapHandle->GetNumElements(ElementCount);
		}
		return CastField<FStructProperty>(MapProperty->ValueProp);
	}

	if (const FSetProperty* SetProperty = CastField<FSetProperty>(Handle->GetProperty()))
	{
		if (const TSharedPtr<IPropertyHandleSet> SetHandle = Handle->AsSet())
		{
			uint32 ElementCount = 0;
			SetHandle->GetNumElements(ElementCount);
		}
		return CastField<FStructProperty>(SetProperty->ElementProp);
	}

	return nullptr;
}

void AddRequiredPath(const FString& CandidatePath, TSet<FString>& OutPaths)
{
	for (const FString& RequiredPath : RequiredSchemaPaths())
	{
		if (CandidatePath.EndsWith(RequiredPath))
		{
			OutPaths.Add(RequiredPath);
		}
	}
}

TArray<FString> ProbeSelectorSchemaPaths(UObject& Subject)
{
	const FCollectedPropertyHandles CollectedHandles = CollectPropertyHandles(Subject);
	TSet<FString> Paths;
	for (const FObservedPropertyHandle& ObservedHandle : CollectedHandles.Handles)
	{
		const TSharedPtr<IPropertyHandle>& Handle = ObservedHandle.Handle;
		if (IsBlackboardSelectorHandle(Handle))
		{
			AddRequiredPath(
				NormalizePropertyPath(FString(Handle->GetPropertyPath()), ObservedHandle.bNestedInInstancedStruct),
				Paths);
		}

		const FStructProperty* ElementStruct = ContainerElementStruct(Handle);
		if (!ElementStruct)
		{
			continue;
		}

		for (const FString& RelativePath : CollectStructSelectorPaths(ElementStruct->Struct))
		{
			AddRequiredPath(
				FString::Printf(TEXT("%s/*/%s"), *Handle->GetProperty()->GetName(), *RelativePath),
				Paths);
		}
	}

	TArray<FString> Result = Paths.Array();
	Result.Sort();
	return Result;
}

TSharedPtr<IPropertyHandle> FindSelectorHandle(
	const FCollectedPropertyHandles& CollectedHandles,
	const FString& SchemaPath)
{
	for (const FObservedPropertyHandle& ObservedHandle : CollectedHandles.Handles)
	{
		if (IsBlackboardSelectorHandle(ObservedHandle.Handle)
			&& NormalizePropertyPath(
				FString(ObservedHandle.Handle->GetPropertyPath()),
				ObservedHandle.bNestedInInstancedStruct).EndsWith(SchemaPath))
		{
			return ObservedHandle.Handle;
		}
	}
	return nullptr;
}

FSelectorPolicyEvidence ProbeSelectorPolicy(UObject& Subject, const FString& SchemaPath)
{
	FSelectorPolicyEvidence Evidence;
	const FCollectedPropertyHandles CollectedHandles = CollectPropertyHandles(Subject);
	const TSharedPtr<IPropertyHandle> SelectorHandle = FindSelectorHandle(CollectedHandles, SchemaPath);
	if (!SelectorHandle.IsValid())
	{
		return Evidence;
	}

	Evidence.bReadOnly = true;
	if (const TSharedPtr<IPropertyHandle> NoneAllowedHandle = SelectorHandle->GetChildHandle(TEXT("bNoneIsAllowedValue")))
	{
		NoneAllowedHandle->GetValue(Evidence.bNoneAllowed);
	}

	const TSharedPtr<IPropertyHandle> AllowedTypesHandle = SelectorHandle->GetChildHandle(TEXT("AllowedTypes"));
	const TSharedPtr<IPropertyHandleArray> AllowedTypes = AllowedTypesHandle.IsValid() ? AllowedTypesHandle->AsArray() : nullptr;
	uint32 AllowedTypeCount = 0;
	if (!AllowedTypes.IsValid() || AllowedTypes->GetNumElements(AllowedTypeCount) != FPropertyAccess::Success || AllowedTypeCount == 0)
	{
		return Evidence;
	}

	const TSharedPtr<IPropertyHandle> AllowedTypeHandle = AllowedTypes->GetElement(0);
	UObject* AllowedType = nullptr;
	if (!AllowedTypeHandle.IsValid() || AllowedTypeHandle->GetValue(AllowedType) != FPropertyAccess::Success)
	{
		return Evidence;
	}

	const UBlackboardKeyType_Object* ObjectType = Cast<UBlackboardKeyType_Object>(AllowedType);
	if (ObjectType)
	{
		Evidence.FilterType = ObjectType->GetClass()->GetPathName();
		Evidence.BaseClass = ObjectType->BaseClass ? ObjectType->BaseClass->GetPathName() : FString();
	}
	return Evidence;
}

void AssertDirectSelectorPolicy(FAutomationTestBase& Test, UObject& Subject, const FString& SchemaPath)
{
	const FSelectorPolicyEvidence Policy = ProbeSelectorPolicy(Subject, SchemaPath);
	Test.TestTrue(TEXT("selector evidence is read-only"), Policy.bReadOnly);
	Test.TestEqual(TEXT("selector object filter type is stable"), Policy.FilterType, FString(TEXT("/Script/AIModule.BlackboardKeyType_Object")));
	Test.TestEqual(TEXT("selector object base class is stable"), Policy.BaseClass, FString(TEXT("/Script/Engine.Actor")));
}

void AssertNativeSelectorEvidence(FAutomationTestBase& Test, UObject& Subject)
{
	Test.TestEqual(TEXT("helper exposes exact selector schema"),
		ProbeSelectorSchemaPaths(Subject),
		TArray<FString>({
			TEXT("ArrayValues/*/Selector"),
			TEXT("DirectSelector"),
			TEXT("DynamicValue/Properties/Selector"),
			TEXT("MapValues/*/Selector"),
			TEXT("NestedValue/Selector"),
			TEXT("SetValues/*/Selector"),
		}));

	AssertDirectSelectorPolicy(Test, Subject, TEXT("DirectSelector"));
	const FSelectorPolicyEvidence OptionalPolicy = ProbeSelectorPolicy(Subject, TEXT("OptionalSelector"));
	Test.TestTrue(TEXT("optional selector admits none"), OptionalPolicy.bNoneAllowed);
	const FSelectorPolicyEvidence NestedPolicy = ProbeSelectorPolicy(Subject, TEXT("NestedValue/Selector"));
	Test.TestTrue(TEXT("nested selector admits none"), NestedPolicy.bNoneAllowed);
}

} // namespace AssetDocumentAISelectorHelperProbeTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAISelectorHelperProbeTest,
	"UEAssetDoc.AI.SelectorEvidence.HelperFirst",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAISelectorHelperProbeTest::RunTest(const FString&)
{
	using namespace AssetDocumentAISelectorHelperProbeTests;

	UClass* NativeClass = LoadClass<UObject>(nullptr, TEXT("/Script/AssetDocumentTests.AssetDocumentSelectorTaskTestNode"));
	TestNotNull(TEXT("native selector fixture resolves by exact class path"), NativeClass);
	UObject* NativeCdo = NativeClass ? NativeClass->GetDefaultObject() : nullptr;
	TestNotNull(TEXT("native selector fixture has a CDO"), NativeCdo);
	if (NativeCdo)
	{
		AssertNativeSelectorEvidence(*this, *NativeCdo);
	}

	const FString BlueprintPackageName = FString::Printf(
		TEXT("/Temp/AssetDocumentAI/SelectorHelperProbe_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UPackage* BlueprintPackage = CreatePackage(*BlueprintPackageName);
	UBlueprint* Blueprint = NativeClass && BlueprintPackage
		? FKismetEditorUtilities::CreateBlueprint(
			NativeClass,
			BlueprintPackage,
			FName(*FPackageName::GetLongPackageAssetName(BlueprintPackageName)),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass())
		: nullptr;
	TestNotNull(TEXT("test-owned Blueprint selector fixture is created"), Blueprint);
	if (Blueprint)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}
	UObject* BlueprintCdo = Blueprint && Blueprint->GeneratedClass
		? Blueprint->GeneratedClass->GetDefaultObject()
		: nullptr;
	TestNotNull(TEXT("test-owned Blueprint selector fixture compiles"), BlueprintCdo);
	if (BlueprintCdo)
	{
		AssertNativeSelectorEvidence(*this, *BlueprintCdo);
	}

	if (BlueprintPackage)
	{
		BlueprintPackage->MarkAsGarbage();
	}

	return true;
}

#endif
