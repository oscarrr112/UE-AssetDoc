// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentModule.h"
#include "AssetDocumentEditorSync.h"
#include "AssetDocumentService.h"
#include "AssetDocumentSidecar.h"
#include "Profiles/UBlueprintAssetDocumentCapability.h"
#include "Regions/AssetDocumentDeferredRegionAdapter.h"

#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Components/SphereComponent.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Blueprint.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "K2Node_IfThenElse.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
class FUBlueprintTestCanonicalSidecarFixture
{
public:
	explicit FUBlueprintTestCanonicalSidecarFixture(const FString& InTarget)
		: Target(InTarget)
		, SidecarPath(FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(InTarget))
		, PackageFilePath(FPackageName::LongPackageNameToFilename(
			InTarget,
			FPackageName::GetAssetPackageExtension()))
		, ObjectPath(FString::Printf(
			TEXT("%s.%s"),
			*InTarget,
			*FPackageName::GetLongPackageAssetName(InTarget)))
		, bPreexistingTarget(
			FindObject<UObject>(nullptr, *ObjectPath) != nullptr
			|| FindObject<UPackage>(nullptr, *InTarget) != nullptr
			|| IFileManager::Get().FileExists(*PackageFilePath)
			|| IFileManager::Get().FileExists(*SidecarPath))
	{
	}

	~FUBlueprintTestCanonicalSidecarFixture()
	{
		if (!bOwnsTarget)
		{
			return;
		}

		FAssetDocumentEditorSync::FScopedSidecarWrite Guard(SidecarPath);
		IFileManager::Get().Delete(*SidecarPath, false, true);
		UObject* ExistingAsset = FindObject<UObject>(nullptr, *ObjectPath);
		if (!ExistingAsset && IFileManager::Get().FileExists(*PackageFilePath))
		{
			ExistingAsset = LoadObject<UObject>(nullptr, *ObjectPath);
		}
		if (ExistingAsset)
		{
			TArray<UObject*> ObjectsToDelete{ExistingAsset};
			ObjectTools::DeleteObjectsUnchecked(ObjectsToDelete);
		}
		IFileManager::Get().Delete(*PackageFilePath, false, true);
	}

	FAssetDocumentResult Apply(
		FAssetDocumentService& Service,
		const TSharedPtr<FJsonObject>& Document)
	{
		if (!IsUniqueTarget() || bPreexistingTarget)
		{
			return FAssetDocumentResult::Failure(
				TEXT("ApplyFile fixture requires a new unique target"));
		}
		if (SidecarPath.IsEmpty()
			|| !IFileManager::Get().MakeDirectory(*FPaths::GetPath(SidecarPath), true))
		{
			return FAssetDocumentResult::Failure(
				TEXT("ApplyFile failed to create canonical sidecar directory"));
		}

		FString WriteError;
		{
			FAssetDocumentEditorSync::FScopedSidecarWrite Guard(SidecarPath);
			if (!FAssetDocumentSidecar::WriteJsonFile(SidecarPath, Document, WriteError))
			{
				return FAssetDocumentResult::Failure(FString::Printf(
					TEXT("ApplyFile failed to write canonical sidecar: %s"),
					*WriteError));
			}
		}
		bOwnsTarget = true;

		FAssetDocumentApplyFileRequest Request;
		Request.FilePath = SidecarPath;
		Request.bSaveAsset = true;
		return Service.ApplyFile(Request);
	}

private:
	bool IsUniqueTarget() const
	{
		const FString AssetName = FPackageName::GetLongPackageAssetName(Target);
		int32 SeparatorIndex = INDEX_NONE;
		if (!AssetName.FindLastChar(TEXT('_'), SeparatorIndex))
		{
			return false;
		}
		FGuid UniqueId;
		return FGuid::ParseExact(
			AssetName.Mid(SeparatorIndex + 1),
			EGuidFormats::Digits,
			UniqueId);
	}

	FString Target;
	FString SidecarPath;
	FString PackageFilePath;
	FString ObjectPath;
	bool bPreexistingTarget = false;
	bool bOwnsTarget = false;
};

struct FUBlueprintTestDocument
{
	explicit FUBlueprintTestDocument(const FString& Target)
		: SidecarFixture(MakeShared<FUBlueprintTestCanonicalSidecarFixture>(Target))
	{
	}

	FUBlueprintTestDocument(const FUBlueprintTestDocument& Existing)
		: SidecarFixture(Existing.SidecarFixture)
	{
	}

	FAssetDocumentResult Apply(FAssetDocumentService& Service)
	{
		if (!Document.IsValid())
		{
			return FAssetDocumentResult::Failure(TEXT("ApplyFile requires a valid document"));
		}
		return SidecarFixture->Apply(Service, Document);
	}

	TSharedPtr<FJsonObject> Document;

private:
	TSharedRef<FUBlueprintTestCanonicalSidecarFixture> SidecarFixture;
};

const FAssetDocumentRegionPolicy* FindUBlueprintPolicyByRegionId(const TArray<FAssetDocumentRegionPolicy>& Policies, FName RegionId)
{
	for (const FAssetDocumentRegionPolicy& Policy : Policies)
	{
		if (Policy.RegionId == RegionId)
		{
			return &Policy;
		}
	}
	return nullptr;
}

TSharedRef<FJsonValue> UBlueprintTestMakeBodyValue(const TSharedRef<FJsonObject>& Body)
{
	return StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueObject>(Body));
}

TSharedRef<FJsonObject> UBlueprintTestMakeActorParentClassRef()
{
	TSharedRef<FJsonObject> ParentClass = MakeShared<FJsonObject>();
	ParentClass->SetStringField(TEXT("Kind"), TEXT("ClassRef"));
	ParentClass->SetStringField(TEXT("Class"), TEXT("/Script/Engine.Actor"));
	return ParentClass;
}

TSharedRef<FJsonObject> MakeUBlueprintClassRefObject(const FString& ClassPath)
{
	TSharedRef<FJsonObject> ClassRef = MakeShared<FJsonObject>();
	ClassRef->SetStringField(TEXT("Kind"), TEXT("ClassRef"));
	ClassRef->SetStringField(TEXT("Class"), ClassPath);
	return ClassRef;
}

TSharedRef<FJsonObject> MakeUBlueprintFloatPinType()
{
	TSharedRef<FJsonObject> Type = MakeShared<FJsonObject>();
	Type->SetStringField(TEXT("PinCategory"), TEXT("real"));
	Type->SetStringField(TEXT("PinSubCategory"), TEXT("float"));
	return Type;
}

TSharedPtr<FJsonObject> MakeUBlueprintFloatVariable(const TCHAR* Name, const TCHAR* DefaultValue, const TCHAR* Category = nullptr, const TCHAR* Tooltip = nullptr)
{
	TSharedPtr<FJsonObject> Variable = MakeShared<FJsonObject>();
	Variable->SetStringField(TEXT("Name"), Name);
	Variable->SetObjectField(TEXT("Type"), MakeUBlueprintFloatPinType());
	Variable->SetStringField(TEXT("DefaultValue"), DefaultValue);
	if (Category)
	{
		Variable->SetStringField(TEXT("Category"), Category);
	}
	if (Tooltip)
	{
		Variable->SetStringField(TEXT("Tooltip"), Tooltip);
	}
	return Variable;
}

TArray<TSharedPtr<FJsonValue>> MakeUBlueprintVariableArray(std::initializer_list<TSharedPtr<FJsonObject>> Variables)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	for (const TSharedPtr<FJsonObject>& Variable : Variables)
	{
		Result.Add(MakeShared<FJsonValueObject>(Variable));
	}
	return Result;
}

TSharedPtr<FJsonObject> MakeUBlueprintDocument(
	const FString& Target,
	const FString& ParentClassPath,
	const TOptional<TArray<TSharedPtr<FJsonValue>>>& Variables,
	const TOptional<TArray<TSharedPtr<FJsonValue>>>& ImplementedInterfaces,
	const TOptional<TArray<TSharedPtr<FJsonValue>>>& Components = TOptional<TArray<TSharedPtr<FJsonValue>>>(),
	TSharedPtr<FJsonObject> ClassDefaults = nullptr)
{
	TSharedPtr<FJsonObject> Document = MakeShared<FJsonObject>();
	Document->SetNumberField(TEXT("SchemaVersion"), 1);
	Document->SetStringField(TEXT("Target"), Target);
	Document->SetStringField(TEXT("Class"), TEXT("/Script/Engine.Blueprint"));
	Document->SetStringField(TEXT("Action"), TEXT("CreateOrUpdate"));
	Document->SetObjectField(TEXT("Definitions"), MakeShared<FJsonObject>());
	Document->SetObjectField(TEXT("Properties"), MakeShared<FJsonObject>());

	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetObjectField(TEXT("ParentClass"), MakeUBlueprintClassRefObject(ParentClassPath));
	if (ImplementedInterfaces.IsSet())
	{
		Body->SetArrayField(TEXT("ImplementedInterfaces"), ImplementedInterfaces.GetValue());
	}
	if (Variables.IsSet())
	{
		Body->SetArrayField(TEXT("Variables"), Variables.GetValue());
	}
	Body->SetArrayField(TEXT("Components"), Components.IsSet() ? Components.GetValue() : TArray<TSharedPtr<FJsonValue>>{});
	Body->SetObjectField(TEXT("ClassDefaults"), ClassDefaults.IsValid() ? ClassDefaults : MakeShared<FJsonObject>());
	Body->SetArrayField(TEXT("UbergraphPages"), {});
	Body->SetArrayField(TEXT("FunctionGraphs"), {});
	Body->SetArrayField(TEXT("MacroGraphs"), {});
	Body->SetArrayField(TEXT("Timelines"), {});
	Document->SetObjectField(TEXT("Body"), Body);
	return Document;
}

TSharedPtr<FJsonObject> LoadBlueprintDocumentBody(TSharedPtr<FJsonObject> Document)
{
	const TSharedPtr<FJsonObject>* Body = nullptr;
	return Document.IsValid() && Document->TryGetObjectField(TEXT("Body"), Body) && Body && Body->IsValid()
		? *Body
		: nullptr;
}

UBlueprint* LoadBlueprintForTarget(const FString& Target)
{
	return LoadObject<UBlueprint>(nullptr, *FString::Printf(TEXT("%s.%s"), *Target, *FPackageName::GetLongPackageAssetName(Target)));
}

TSharedPtr<FJsonObject> MakeComponentKey(const TCHAR* Name, const TCHAR* OwnerClass = TEXT("Self"))
{
	TSharedPtr<FJsonObject> Key = MakeShared<FJsonObject>();
	Key->SetStringField(TEXT("Name"), Name);
	Key->SetStringField(TEXT("OwnerClass"), OwnerClass);
	return Key;
}

TSharedPtr<FJsonObject> MakeOwnedSphereComponent(const TCHAR* Name, double SphereRadius)
{
	TSharedPtr<FJsonObject> Component = MakeShared<FJsonObject>();
	Component->SetObjectField(TEXT("Key"), MakeComponentKey(Name));
	Component->SetStringField(TEXT("Scope"), TEXT("OwnedSCS"));
	Component->SetStringField(TEXT("Class"), TEXT("/Script/Engine.SphereComponent"));
	Component->SetObjectField(TEXT("AttachTo"), MakeComponentKey(TEXT("DefaultSceneRoot")));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetNumberField(TEXT("SphereRadius"), SphereRadius);
	Component->SetObjectField(TEXT("Properties"), Properties);
	return Component;
}

TSharedPtr<FJsonObject> MakeOwnedSceneComponent(const TCHAR* Name)
{
	TSharedPtr<FJsonObject> Component = MakeShared<FJsonObject>();
	Component->SetObjectField(TEXT("Key"), MakeComponentKey(Name));
	Component->SetStringField(TEXT("Scope"), TEXT("OwnedSCS"));
	Component->SetStringField(TEXT("Class"), TEXT("/Script/Engine.SceneComponent"));
	Component->SetObjectField(TEXT("AttachTo"), MakeComponentKey(TEXT("DefaultSceneRoot")));
	Component->SetObjectField(TEXT("Properties"), MakeShared<FJsonObject>());
	return Component;
}

TSharedPtr<FJsonObject> MakeReferencedComponent(const TCHAR* Scope, const TCHAR* Name, const TCHAR* OwnerClass)
{
	TSharedPtr<FJsonObject> Component = MakeShared<FJsonObject>();
	Component->SetObjectField(TEXT("Key"), MakeComponentKey(Name, OwnerClass));
	Component->SetStringField(TEXT("Scope"), Scope);
	Component->SetStringField(TEXT("Class"), TEXT("/Script/Engine.SceneComponent"));
	Component->SetObjectField(TEXT("Properties"), MakeShared<FJsonObject>());
	return Component;
}

TArray<TSharedPtr<FJsonValue>> MakeComponentArray(std::initializer_list<TSharedPtr<FJsonObject>> Components)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	for (const TSharedPtr<FJsonObject>& Component : Components)
	{
		Result.Add(MakeShared<FJsonValueObject>(Component));
	}
	return Result;
}

USCS_Node* FindSCSNodeByVariableName(const UBlueprint* Blueprint, FName VariableName)
{
	if (!Blueprint || !Blueprint->SimpleConstructionScript)
	{
		return nullptr;
	}

	for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
	{
		if (Node && Node->GetVariableName() == VariableName)
		{
			return Node;
		}
	}
	return nullptr;
}

bool HasUBlueprintVariable(const UBlueprint* Blueprint, FName Name)
{
	return Blueprint && Blueprint->NewVariables.ContainsByPredicate([Name](const FBPVariableDescription& Variable)
	{
		return Variable.VarName == Name;
	});
}

UBlueprint* UBlueprintTestCreateTransientActorBlueprint(const TCHAR* NamePrefix)
{
	const FName BlueprintName(*FString::Printf(TEXT("%s_%s"), NamePrefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		BlueprintName,
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	if (Blueprint)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}
	return Blueprint;
}

template <typename NodeType>
NodeType* UBlueprintTestAddK2Node(UEdGraph* Graph, int32 X, int32 Y)
{
	NodeType* Node = Graph ? NewObject<NodeType>(Graph) : nullptr;
	if (!Node)
	{
		return nullptr;
	}

	Graph->AddNode(Node, true, false);
	Node->NodePosX = X;
	Node->NodePosY = Y;
	return Node;
}

UK2Node_IfThenElse* UBlueprintTestAddUnsupportedBranchNode(UEdGraph* Graph, int32 X, int32 Y)
{
	UK2Node_IfThenElse* Node = UBlueprintTestAddK2Node<UK2Node_IfThenElse>(Graph, X, Y);
	if (Node)
	{
		Node->AllocateDefaultPins();
	}
	return Node;
}

bool UBlueprintTestResultHasDiagnostic(const FAssetDocumentCapabilityResult& Result, const FString& Path, const FString& Code)
{
	return Result.Diagnostics.ContainsByPredicate([&Path, &Code](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Path == Path && Diagnostic.Code == Code;
	});
}

bool UBlueprintTestResultHasDiagnosticCode(const FAssetDocumentCapabilityResult& Result, const FString& Code)
{
	return Result.Diagnostics.ContainsByPredicate([&Code](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == Code;
	});
}

// Production diff (module service, Python route): the live asset equals Document.
void TestUBlueprintServiceNoDiff(FAutomationTestBase& Test, const TCHAR* What, const TSharedPtr<FJsonObject>& Document);

FString DescribeUBlueprintTestResult(const FAssetDocumentResult& Result)
{
	FString Text = Result.Message;
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Text += FString::Printf(TEXT(" | %s %s: %s"), *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message);
	}
	return Text;
}

void TestUBlueprintServiceNoDiff(FAutomationTestBase& Test, const TCHAR* What, const TSharedPtr<FJsonObject>& Document)
{
	FAssetDocumentDiffRequest Request;
	Request.Document = Document;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Diff(Request);
	const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: diff succeeds"), What),
			Result.IsSuccess() && Result.Payload.IsValid() && Result.Payload->TryGetArrayField(TEXT("changed"), Changed)))
	{
		Test.AddError(DescribeUBlueprintTestResult(Result));
		return;
	}
	Test.TestEqual(*FString::Printf(TEXT("%s: no changed diff entries"), What), Changed->Num(), 0);
}

bool UBlueprintTestResultHasDiagnostic(const FAssetDocumentResult& Result, const FString& Code)
{
	return Result.Diagnostics.ContainsByPredicate([&Code](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == Code;
	});
}

TSharedPtr<FJsonObject> FindUBlueprintJsonObjectByStringField(const TArray<TSharedPtr<FJsonValue>>& Values, const FString& FieldName, const FString& ExpectedValue)
{
	for (const TSharedPtr<FJsonValue>& Value : Values)
	{
		const TSharedPtr<FJsonObject> Object = Value.IsValid() ? Value->AsObject() : nullptr;
		FString ActualValue;
		if (Object.IsValid() && Object->TryGetStringField(FieldName, ActualValue) && ActualValue == ExpectedValue)
		{
			return Object;
		}
	}
	return nullptr;
}

TSharedPtr<FJsonObject> FindComponentByScopeAndKey(
	const TArray<TSharedPtr<FJsonValue>>& Values,
	const FString& Scope,
	const FString& Name,
	const FString& OwnerClass)
{
	for (const TSharedPtr<FJsonValue>& Value : Values)
	{
		const TSharedPtr<FJsonObject> Object = Value.IsValid() ? Value->AsObject() : nullptr;
		if (!Object.IsValid())
		{
			continue;
		}

		FString ActualScope;
		if (!Object->TryGetStringField(TEXT("Scope"), ActualScope) || ActualScope != Scope)
		{
			continue;
		}

		const TSharedPtr<FJsonObject>* Key = nullptr;
		if (!Object->TryGetObjectField(TEXT("Key"), Key) || !Key || !Key->IsValid())
		{
			continue;
		}

		FString ActualName;
		FString ActualOwnerClass;
		if ((*Key)->TryGetStringField(TEXT("Name"), ActualName)
			&& (*Key)->TryGetStringField(TEXT("OwnerClass"), ActualOwnerClass)
			&& ActualName == Name
			&& ActualOwnerClass == OwnerClass)
		{
			return Object;
		}
	}
	return nullptr;
}

TSharedPtr<FJsonObject> FindUBlueprintDiffEntryByPath(const TArray<TSharedPtr<FJsonValue>>& Values, const FString& ExpectedPath)
{
	for (const TSharedPtr<FJsonValue>& Value : Values)
	{
		const TSharedPtr<FJsonObject> Object = Value.IsValid() ? Value->AsObject() : nullptr;
		FString Path;
		if (Object.IsValid() && Object->TryGetStringField(TEXT("path"), Path) && Path == ExpectedPath)
		{
			return Object;
		}
	}
	return nullptr;
}

TSharedPtr<FJsonObject> FindUBlueprintDiffEntryByExactPath(const TArray<TSharedPtr<FJsonValue>>& Values, const FString& ExpectedPath)
{
	for (const TSharedPtr<FJsonValue>& Value : Values)
	{
		const TSharedPtr<FJsonObject> Object = Value.IsValid() ? Value->AsObject() : nullptr;
		FString Path;
		if (Object.IsValid()
			&& Object->TryGetStringField(TEXT("path"), Path)
			&& Path.Equals(ExpectedPath, ESearchCase::CaseSensitive))
		{
			return Object;
		}
	}
	return nullptr;
}

void TestUBlueprintDiffEntryFieldIsNull(FAutomationTestBase* Test, const TSharedPtr<FJsonObject>& Entry, const TCHAR* FieldName)
{
	const TSharedPtr<FJsonValue> Value = Entry.IsValid() ? Entry->TryGetField(FieldName) : nullptr;
	Test->TestTrue(
		FString::Printf(TEXT("%s is null"), FieldName),
		Value.IsValid() && Value->IsNull());
}

void TestUBlueprintInterfaceDiffValue(FAutomationTestBase* Test, const TSharedPtr<FJsonObject>& Entry, const TCHAR* FieldName, const FString& ExpectedClassPath)
{
	const TSharedPtr<FJsonValue> Value = Entry.IsValid() ? Entry->TryGetField(FieldName) : nullptr;
	const TSharedPtr<FJsonObject> ValueObject = Value.IsValid() ? Value->AsObject() : nullptr;
	const TSharedPtr<FJsonObject>* InterfaceObject = nullptr;
	Test->TestTrue(
		FString::Printf(TEXT("%s is an interface object"), FieldName),
		ValueObject.IsValid()
			&& ValueObject->TryGetObjectField(TEXT("Interface"), InterfaceObject)
			&& InterfaceObject
			&& InterfaceObject->IsValid());
	if (!InterfaceObject || !InterfaceObject->IsValid())
	{
		return;
	}

	FString Kind;
	FString ClassPath;
	Test->TestTrue(
		FString::Printf(TEXT("%s Interface has Kind"), FieldName),
		(*InterfaceObject)->TryGetStringField(TEXT("Kind"), Kind));
	Test->TestEqual(
		FString::Printf(TEXT("%s Interface Kind is ClassRef"), FieldName),
		Kind,
		FString(TEXT("ClassRef")));
	Test->TestTrue(
		FString::Printf(TEXT("%s Interface has Class"), FieldName),
		(*InterfaceObject)->TryGetStringField(TEXT("Class"), ClassPath));
	Test->TestEqual(
		FString::Printf(TEXT("%s Interface Class path"), FieldName),
		ClassPath,
		ExpectedClassPath);
}

void TestVariableDiffValue(
	FAutomationTestBase* Test,
	const TSharedPtr<FJsonObject>& Entry,
	const TCHAR* FieldName,
	const FString& ExpectedName,
	const FString& ExpectedDefaultValue)
{
	const TSharedPtr<FJsonValue> Value = Entry.IsValid() ? Entry->TryGetField(FieldName) : nullptr;
	const TSharedPtr<FJsonObject> ValueObject = Value.IsValid() ? Value->AsObject() : nullptr;
	Test->TestTrue(FString::Printf(TEXT("%s is a variable object"), FieldName), ValueObject.IsValid());
	if (!ValueObject.IsValid())
	{
		return;
	}

	FString Name;
	Test->TestTrue(FString::Printf(TEXT("%s variable has Name"), FieldName), ValueObject->TryGetStringField(TEXT("Name"), Name));
	Test->TestEqual(FString::Printf(TEXT("%s variable Name"), FieldName), Name, ExpectedName);

	FString DefaultValue;
	Test->TestTrue(FString::Printf(TEXT("%s variable has DefaultValue"), FieldName), ValueObject->TryGetStringField(TEXT("DefaultValue"), DefaultValue));
	Test->TestEqual(FString::Printf(TEXT("%s variable DefaultValue"), FieldName), DefaultValue, ExpectedDefaultValue);

	const TSharedPtr<FJsonObject>* Type = nullptr;
	Test->TestTrue(
		FString::Printf(TEXT("%s variable has Type"), FieldName),
		ValueObject->TryGetObjectField(TEXT("Type"), Type) && Type && Type->IsValid());
	if (!Type || !Type->IsValid())
	{
		return;
	}

	FString PinCategory;
	FString PinSubCategory;
	Test->TestTrue(FString::Printf(TEXT("%s variable Type has PinCategory"), FieldName), (*Type)->TryGetStringField(TEXT("PinCategory"), PinCategory));
	Test->TestTrue(FString::Printf(TEXT("%s variable Type has PinSubCategory"), FieldName), (*Type)->TryGetStringField(TEXT("PinSubCategory"), PinSubCategory));
	Test->TestEqual(FString::Printf(TEXT("%s variable PinCategory"), FieldName), PinCategory, FString(TEXT("real")));
	Test->TestEqual(FString::Printf(TEXT("%s variable PinSubCategory"), FieldName), PinSubCategory, FString(TEXT("float")));
}

bool IsUnchangedDiffEntry(const TSharedPtr<FJsonObject>& Entry)
{
	if (!Entry.IsValid())
	{
		return false;
	}

	FString Status;
	return Entry->TryGetStringField(TEXT("status"), Status)
		&& Status == TEXT("unchanged")
		&& !Entry->HasField(TEXT("change"));
}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintUnsupportedGraphProtectionTest,
	"UEAssetDoc.UBlueprint.UnsupportedGraphProtection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintUnsupportedGraphProtectionTest::RunTest(const FString&)
{
	const FUBlueprintAssetDocumentCapability Capability;
	FAssetDocumentCapabilityContext Context;
	Context.AssetClass = UBlueprint::StaticClass();

	const TArray<FString> ProtectedRegions = {
		TEXT("Timelines"),
	};

	for (const FString& Region : ProtectedRegions)
	{
		TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetObjectField(TEXT("ParentClass"), UBlueprintTestMakeActorParentClassRef());
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Add(MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()));
		Body->SetArrayField(Region, Values);

		const FAssetDocumentCapabilityResult Result = Capability.Validate(Context, UBlueprintTestMakeBodyValue(Body));
		TestFalse(FString::Printf(TEXT("Non-empty %s fails validation"), *Region), Result.bSuccess);
		TestTrue(FString::Printf(TEXT("%s failure mentions unsupported region"), *Region), Result.Message.Contains(Region));
		TestTrue(FString::Printf(TEXT("%s diagnostic is precise"), *Region), UBlueprintTestResultHasDiagnostic(Result, FString::Printf(TEXT("/Body/%s"), *Region), TEXT("UnsupportedUBlueprintRegion")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintCreateTest,
	"UEAssetDoc.UBlueprint.Create",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintCreateTest::RunTest(const FString&)
{
	const FString Target = FString::Printf(TEXT("/Game/AssetDocumentTests/BP_AD_Create_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));

	auto MakeCreateDocument = [](const FString& InTarget, const FString& ParentClassPath)
	{
		TSharedPtr<FJsonObject> Document = MakeShared<FJsonObject>();
		Document->SetNumberField(TEXT("SchemaVersion"), 1);
		Document->SetStringField(TEXT("Target"), InTarget);
		Document->SetStringField(TEXT("Class"), TEXT("/Script/Engine.Blueprint"));
		Document->SetStringField(TEXT("Action"), TEXT("CreateOrUpdate"));
		Document->SetObjectField(TEXT("Definitions"), MakeShared<FJsonObject>());
		Document->SetObjectField(TEXT("Properties"), MakeShared<FJsonObject>());

		TSharedPtr<FJsonObject> ParentClass = MakeShared<FJsonObject>();
		ParentClass->SetStringField(TEXT("Kind"), TEXT("ClassRef"));
		ParentClass->SetStringField(TEXT("Class"), ParentClassPath);

		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetObjectField(TEXT("ParentClass"), ParentClass);
		Body->SetArrayField(TEXT("ImplementedInterfaces"), {});
		Body->SetArrayField(TEXT("Variables"), {});
		Body->SetArrayField(TEXT("Components"), {});
		Body->SetObjectField(TEXT("ClassDefaults"), MakeShared<FJsonObject>());
		Body->SetArrayField(TEXT("UbergraphPages"), {});
		Body->SetArrayField(TEXT("FunctionGraphs"), {});
		Body->SetArrayField(TEXT("MacroGraphs"), {});
		Body->SetArrayField(TEXT("Timelines"), {});
		Document->SetObjectField(TEXT("Body"), Body);
		return Document;
	};

	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	FUBlueprintTestDocument Request(Target);
	Request.Document = MakeCreateDocument(Target, TEXT("/Script/Engine.Actor"));

	const FAssetDocumentResult Result = Request.Apply(Service);
	if (!Result.IsSuccess())
	{
		AddError(FString::Printf(TEXT("Apply failed: %s"), *DescribeUBlueprintTestResult(Result)));
	}
	TestTrue(TEXT("Blueprint apply succeeds"), Result.IsSuccess());

	UObject* Created = LoadObject<UObject>(nullptr, *FString::Printf(TEXT("%s.%s"), *Target, *FPackageName::GetLongPackageAssetName(Target)));
	UBlueprint* Blueprint = Cast<UBlueprint>(Created);
	TestNotNull(TEXT("Created asset is UBlueprint"), Blueprint);
	if (Blueprint)
	{
		TestEqual(TEXT("Parent class is Actor"), Blueprint->ParentClass.Get(), AActor::StaticClass());
	}

	const FString PawnTarget = FString::Printf(TEXT("/Game/AssetDocumentTests/BP_AD_CreatePawn_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FUBlueprintTestDocument PawnRequest(PawnTarget);
	PawnRequest.Document = MakeCreateDocument(PawnTarget, TEXT("/Script/Engine.Pawn"));

	const FAssetDocumentResult PawnResult = PawnRequest.Apply(Service);
	if (!PawnResult.IsSuccess())
	{
		AddError(FString::Printf(TEXT("Pawn apply failed: %s"), *DescribeUBlueprintTestResult(PawnResult)));
	}
	TestTrue(TEXT("Blueprint apply with Pawn parent succeeds"), PawnResult.IsSuccess());

	UObject* CreatedPawnBlueprint = LoadObject<UObject>(nullptr, *FString::Printf(TEXT("%s.%s"), *PawnTarget, *FPackageName::GetLongPackageAssetName(PawnTarget)));
	UBlueprint* PawnBlueprint = Cast<UBlueprint>(CreatedPawnBlueprint);
	TestNotNull(TEXT("Created Pawn-parent asset is UBlueprint"), PawnBlueprint);
	if (PawnBlueprint)
	{
		TestEqual(TEXT("Parent class is Pawn"), PawnBlueprint->ParentClass.Get(), APawn::StaticClass());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintRequiresParentClassTest,
	"UEAssetDoc.UBlueprint.RequiresParentClass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintRequiresParentClassTest::RunTest(const FString&)
{
	const FString Target = FString::Printf(TEXT("/Game/AssetDocumentTests/BP_AD_MissingParent_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));

	TSharedPtr<FJsonObject> Document = MakeShared<FJsonObject>();
	Document->SetNumberField(TEXT("SchemaVersion"), 1);
	Document->SetStringField(TEXT("Target"), Target);
	Document->SetStringField(TEXT("Class"), TEXT("/Script/Engine.Blueprint"));
	Document->SetStringField(TEXT("Action"), TEXT("CreateOrUpdate"));
	Document->SetObjectField(TEXT("Definitions"), MakeShared<FJsonObject>());
	Document->SetObjectField(TEXT("Properties"), MakeShared<FJsonObject>());

	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetArrayField(TEXT("ImplementedInterfaces"), {});
	Body->SetArrayField(TEXT("Variables"), {});
	Body->SetArrayField(TEXT("Components"), {});
	Body->SetObjectField(TEXT("ClassDefaults"), MakeShared<FJsonObject>());
	Body->SetArrayField(TEXT("UbergraphPages"), {});
	Body->SetArrayField(TEXT("FunctionGraphs"), {});
	Body->SetArrayField(TEXT("MacroGraphs"), {});
	Body->SetArrayField(TEXT("Timelines"), {});
	Document->SetObjectField(TEXT("Body"), Body);

	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	FUBlueprintTestDocument Request(Target);
	Request.Document = Document;

	const FAssetDocumentResult Result = Request.Apply(Service);
	TestFalse(TEXT("Blueprint apply without ParentClass fails"), Result.IsSuccess());
	// The generic route reports a generic Message; the detail is the diagnostic.
	TestTrue(TEXT("Failure mentions ParentClass"), DescribeUBlueprintTestResult(Result).Contains(TEXT("UBlueprint creation requires Body.ParentClass")));

	UObject* Created = FindObject<UObject>(nullptr, *FString::Printf(TEXT("%s.%s"), *Target, *FPackageName::GetLongPackageAssetName(Target)));
	TestNull(TEXT("Missing ParentClass does not create a Blueprint asset"), Created);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintVariablesAuthoritativeTest,
	"UEAssetDoc.UBlueprint.VariablesAuthoritative",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintVariablesAuthoritativeTest::RunTest(const FString&)
{
	const FString Target = FString::Printf(TEXT("/Game/AssetDocumentTests/BP_AD_Vars_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));

	auto MakeDocument = [&Target](bool bIncludeStamina)
	{
		TSharedPtr<FJsonObject> Document = MakeShared<FJsonObject>();
		Document->SetNumberField(TEXT("SchemaVersion"), 1);
		Document->SetStringField(TEXT("Target"), Target);
		Document->SetStringField(TEXT("Class"), TEXT("/Script/Engine.Blueprint"));
		Document->SetStringField(TEXT("Action"), TEXT("CreateOrUpdate"));
		Document->SetObjectField(TEXT("Definitions"), MakeShared<FJsonObject>());
		Document->SetObjectField(TEXT("Properties"), MakeShared<FJsonObject>());

		TSharedPtr<FJsonObject> ParentClass = MakeShared<FJsonObject>();
		ParentClass->SetStringField(TEXT("Kind"), TEXT("ClassRef"));
		ParentClass->SetStringField(TEXT("Class"), TEXT("/Script/Engine.Actor"));

		TArray<TSharedPtr<FJsonValue>> Variables;
		auto AddFloatVariable = [&Variables](const TCHAR* Name, const TCHAR* DefaultValue)
		{
			TSharedPtr<FJsonObject> Type = MakeShared<FJsonObject>();
			Type->SetStringField(TEXT("PinCategory"), TEXT("real"));
			Type->SetStringField(TEXT("PinSubCategory"), TEXT("float"));

			TSharedPtr<FJsonObject> Variable = MakeShared<FJsonObject>();
			Variable->SetStringField(TEXT("Name"), Name);
			Variable->SetObjectField(TEXT("Type"), Type);
			Variable->SetStringField(TEXT("DefaultValue"), DefaultValue);
			Variables.Add(MakeShared<FJsonValueObject>(Variable));
		};

		AddFloatVariable(TEXT("Health"), TEXT("100.0"));
		if (bIncludeStamina)
		{
			AddFloatVariable(TEXT("Stamina"), TEXT("50.0"));
		}

		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetObjectField(TEXT("ParentClass"), ParentClass);
		Body->SetArrayField(TEXT("ImplementedInterfaces"), {});
		Body->SetArrayField(TEXT("Variables"), Variables);
		Body->SetArrayField(TEXT("Components"), {});
		Body->SetObjectField(TEXT("ClassDefaults"), MakeShared<FJsonObject>());
		Body->SetArrayField(TEXT("UbergraphPages"), {});
		Body->SetArrayField(TEXT("FunctionGraphs"), {});
		Body->SetArrayField(TEXT("MacroGraphs"), {});
		Body->SetArrayField(TEXT("Timelines"), {});
		Document->SetObjectField(TEXT("Body"), Body);
		return Document;
	};

	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();

	FUBlueprintTestDocument FirstRequest(Target);
	FirstRequest.Document = MakeDocument(true);
	const FAssetDocumentResult FirstResult = FirstRequest.Apply(Service);
	if (!FirstResult.IsSuccess())
	{
		AddError(FString::Printf(TEXT("Initial apply failed: %s"), *DescribeUBlueprintTestResult(FirstResult)));
	}
	TestTrue(TEXT("Initial variable apply succeeds"), FirstResult.IsSuccess());

	FUBlueprintTestDocument SecondRequest(FirstRequest);
	SecondRequest.Document = MakeDocument(false);
	const FAssetDocumentResult SecondResult = SecondRequest.Apply(Service);
	if (!SecondResult.IsSuccess())
	{
		AddError(FString::Printf(TEXT("Second apply failed: %s"), *DescribeUBlueprintTestResult(SecondResult)));
	}
	TestTrue(TEXT("Second variable apply succeeds"), SecondResult.IsSuccess());

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *FString::Printf(TEXT("%s.%s"), *Target, *FPackageName::GetLongPackageAssetName(Target)));
	TestNotNull(TEXT("Blueprint exists"), Blueprint);
	if (Blueprint)
	{
		const FBPVariableDescription* Health = Blueprint->NewVariables.FindByPredicate([](const FBPVariableDescription& Variable)
		{
			return Variable.VarName == TEXT("Health");
		});
		TestNotNull(TEXT("Health remains"), Health);
		if (Health)
		{
			// Like the editor's Details panel, the Python route keeps the default on the class
			// defaults object (the compiled source of truth), not only in the variable description.
			const FProperty* Property = Blueprint->GeneratedClass ? FindFProperty<FProperty>(Blueprint->GeneratedClass, TEXT("Health")) : nullptr;
			FString DefaultText;
			if (Property)
			{
				Property->ExportText_InContainer(0, DefaultText, Blueprint->GeneratedClass->GetDefaultObject(), nullptr, nullptr, PPF_None);
			}
			TestEqual(TEXT("Health default value is persisted on the class defaults"), FCString::Atod(*DefaultText), 100.0);
		}
		TestFalse(TEXT("Stamina was removed"), Blueprint->NewVariables.ContainsByPredicate([](const FBPVariableDescription& Variable)
		{
			return Variable.VarName == TEXT("Stamina");
		}));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintOwnedSCSComponentsAuthoritativeTest,
	"UEAssetDoc.UBlueprint.OwnedSCSComponentsAuthoritative",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintOwnedSCSComponentsAuthoritativeTest::RunTest(const FString&)
{
	const FString Target = FString::Printf(TEXT("/Game/AssetDocumentTests/BP_AD_Components_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();

	FUBlueprintTestDocument FirstRequest(Target);
	FirstRequest.Document = MakeUBlueprintDocument(
		Target,
		TEXT("/Script/Engine.Actor"),
		TArray<TSharedPtr<FJsonValue>>{},
		TArray<TSharedPtr<FJsonValue>>{});
	LoadBlueprintDocumentBody(FirstRequest.Document)->SetArrayField(
		TEXT("Components"),
		MakeComponentArray({
			MakeOwnedSphereComponent(TEXT("Sensor"), 500.0),
			MakeOwnedSphereComponent(TEXT("Vision"), 250.0),
		}));

	const FAssetDocumentResult FirstResult = FirstRequest.Apply(Service);
	if (!FirstResult.IsSuccess())
	{
		AddError(FString::Printf(TEXT("Initial component apply failed: %s"), *DescribeUBlueprintTestResult(FirstResult)));
	}
	TestTrue(TEXT("Initial component apply succeeds"), FirstResult.IsSuccess());

	FUBlueprintTestDocument SecondRequest(FirstRequest);
	SecondRequest.Document = MakeUBlueprintDocument(
		Target,
		TEXT("/Script/Engine.Actor"),
		TArray<TSharedPtr<FJsonValue>>{},
		TArray<TSharedPtr<FJsonValue>>{});
	LoadBlueprintDocumentBody(SecondRequest.Document)->SetArrayField(
		TEXT("Components"),
		MakeComponentArray({MakeOwnedSphereComponent(TEXT("Sensor"), 500.0)}));

	const FAssetDocumentResult SecondResult = SecondRequest.Apply(Service);
	if (!SecondResult.IsSuccess())
	{
		AddError(FString::Printf(TEXT("Second component apply failed: %s"), *DescribeUBlueprintTestResult(SecondResult)));
	}
	TestTrue(TEXT("Second component apply succeeds"), SecondResult.IsSuccess());

	UBlueprint* Blueprint = LoadBlueprintForTarget(Target);
	TestNotNull(TEXT("Blueprint exists"), Blueprint);
	if (Blueprint)
	{
		USCS_Node* Sensor = FindSCSNodeByVariableName(Blueprint, TEXT("Sensor"));
		USCS_Node* Vision = FindSCSNodeByVariableName(Blueprint, TEXT("Vision"));
		USCS_Node* DefaultSceneRoot = FindSCSNodeByVariableName(Blueprint, USceneComponent::GetDefaultSceneRootVariableName());

		TestNotNull(TEXT("Sensor component remains"), Sensor);
		TestNull(TEXT("Vision component was removed"), Vision);
		TestNotNull(TEXT("DefaultSceneRoot exists"), DefaultSceneRoot);
		if (Sensor)
		{
			TestEqual(TEXT("Sensor class is SphereComponent"), Sensor->ComponentClass.Get(), USphereComponent::StaticClass());
			const USphereComponent* SensorTemplate = Cast<USphereComponent>(Sensor->ComponentTemplate);
			TestNotNull(TEXT("Sensor has SphereComponent template"), SensorTemplate);
			if (SensorTemplate)
			{
				TestEqual(TEXT("SphereRadius property was applied"), SensorTemplate->GetUnscaledSphereRadius(), 500.0f);
			}
			TestEqual(TEXT("Sensor attaches to DefaultSceneRoot"), Blueprint->SimpleConstructionScript->FindParentNode(Sensor), DefaultSceneRoot);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintComponentsAuthoritativeDeletesOwnedSCSTest,
	"UEAssetDoc.UBlueprint.ComponentsAuthoritativeDeletesOwnedSCS",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintComponentsAuthoritativeDeletesOwnedSCSTest::RunTest(const FString&)
{
	const FString Target = FString::Printf(TEXT("/Game/AssetDocumentTests/BP_AD_ComponentScopes_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();

	FUBlueprintTestDocument CreateRequest(Target);
	CreateRequest.Document = MakeUBlueprintDocument(
		Target,
		TEXT("/Script/Engine.Actor"),
		TArray<TSharedPtr<FJsonValue>>{},
		TArray<TSharedPtr<FJsonValue>>{});
	LoadBlueprintDocumentBody(CreateRequest.Document)->SetArrayField(
		TEXT("Components"),
		MakeComponentArray({MakeOwnedSphereComponent(TEXT("Sensor"), 500.0)}));

	const FAssetDocumentResult CreateResult = CreateRequest.Apply(Service);
	TestTrue(TEXT("Initial owned component apply succeeds"), CreateResult.IsSuccess());

	FUBlueprintTestDocument EmptyComponentsRequest(CreateRequest);
	EmptyComponentsRequest.Document = MakeUBlueprintDocument(
		Target,
		TEXT("/Script/Engine.Actor"),
		TArray<TSharedPtr<FJsonValue>>{},
		TArray<TSharedPtr<FJsonValue>>{});
	LoadBlueprintDocumentBody(EmptyComponentsRequest.Document)->SetArrayField(TEXT("Components"), TArray<TSharedPtr<FJsonValue>>{});

	const FAssetDocumentResult EmptyComponentsResult = EmptyComponentsRequest.Apply(Service);
	TestTrue(TEXT("Empty Components apply succeeds"), EmptyComponentsResult.IsSuccess());

	UBlueprint* Blueprint = LoadBlueprintForTarget(Target);
	TestNotNull(TEXT("Blueprint exists"), Blueprint);
	if (Blueprint)
	{
		TestNull(TEXT("Explicit empty Components deletes owned Sensor"), FindSCSNodeByVariableName(Blueprint, TEXT("Sensor")));
	}

	FUBlueprintTestDocument RecreateRequest(CreateRequest);
	RecreateRequest.Document = MakeUBlueprintDocument(
		Target,
		TEXT("/Script/Engine.Actor"),
		TArray<TSharedPtr<FJsonValue>>{},
		TArray<TSharedPtr<FJsonValue>>{});
	LoadBlueprintDocumentBody(RecreateRequest.Document)->SetArrayField(
		TEXT("Components"),
		MakeComponentArray({MakeOwnedSphereComponent(TEXT("Sensor"), 500.0)}));
	TestTrue(TEXT("Recreated owned component apply succeeds"), RecreateRequest.Apply(Service).IsSuccess());

	FUBlueprintTestDocument OmittedComponentsRequest(CreateRequest);
	OmittedComponentsRequest.Document = MakeUBlueprintDocument(
		Target,
		TEXT("/Script/Engine.Actor"),
		TArray<TSharedPtr<FJsonValue>>{},
		TArray<TSharedPtr<FJsonValue>>{});
	LoadBlueprintDocumentBody(OmittedComponentsRequest.Document)->RemoveField(TEXT("Components"));
	const FAssetDocumentResult OmittedComponentsResult = OmittedComponentsRequest.Apply(Service);
	TestTrue(TEXT("Omitted Components apply succeeds"), OmittedComponentsResult.IsSuccess());

	Blueprint = LoadBlueprintForTarget(Target);
	TestNotNull(TEXT("Blueprint still exists"), Blueprint);
	if (Blueprint)
	{
		// Python route (profile.ublueprint.v2): an omitted Components region is preserved.
		TestNotNull(TEXT("Omitted Components preserves owned Sensor"), FindSCSNodeByVariableName(Blueprint, TEXT("Sensor")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintOwnedSCSComponentsStructuralPreflightTest,
	"UEAssetDoc.UBlueprint.OwnedSCSComponentsStructuralPreflight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintOwnedSCSComponentsStructuralPreflightTest::RunTest(const FString&)
{
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();

	{
		const FString Target = FString::Printf(TEXT("/Game/AssetDocumentTests/BP_AD_ComponentBadAttach_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
		FUBlueprintTestDocument CreateRequest(Target);
		CreateRequest.Document = MakeUBlueprintDocument(
			Target,
			TEXT("/Script/Engine.Actor"),
			TArray<TSharedPtr<FJsonValue>>{},
			TArray<TSharedPtr<FJsonValue>>{});
		LoadBlueprintDocumentBody(CreateRequest.Document)->SetArrayField(
			TEXT("Components"),
			MakeComponentArray({MakeOwnedSphereComponent(TEXT("Sensor"), 500.0)}));
		TestTrue(TEXT("Initial Sensor apply succeeds"), CreateRequest.Apply(Service).IsSuccess());

		TSharedPtr<FJsonObject> BadChild = MakeOwnedSceneComponent(TEXT("BadChild"));
		BadChild->SetObjectField(TEXT("AttachTo"), MakeComponentKey(TEXT("MissingParent")));

		FUBlueprintTestDocument BadRequest(CreateRequest);
		BadRequest.Document = MakeUBlueprintDocument(
			Target,
			TEXT("/Script/Engine.Actor"),
			TArray<TSharedPtr<FJsonValue>>{},
			TArray<TSharedPtr<FJsonValue>>{});
		LoadBlueprintDocumentBody(BadRequest.Document)->SetArrayField(TEXT("Components"), MakeComponentArray({BadChild}));

		const FAssetDocumentResult BadResult = BadRequest.Apply(Service);
		TestFalse(TEXT("Missing AttachTo parent rejects apply"), BadResult.IsSuccess());

		UBlueprint* Blueprint = LoadBlueprintForTarget(Target);
		TestNotNull(TEXT("Blueprint remains loadable after bad apply"), Blueprint);
		if (Blueprint)
		{
			TestNotNull(TEXT("Existing Sensor survives failed apply"), FindSCSNodeByVariableName(Blueprint, TEXT("Sensor")));
			TestNull(TEXT("BadChild is not left behind after failed apply"), FindSCSNodeByVariableName(Blueprint, TEXT("BadChild")));
		}
	}

	{
		const FString Target = FString::Printf(TEXT("/Game/AssetDocumentTests/BP_AD_ComponentStaleAttach_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
		FUBlueprintTestDocument CreateRequest(Target);
		CreateRequest.Document = MakeUBlueprintDocument(
			Target,
			TEXT("/Script/Engine.Actor"),
			TArray<TSharedPtr<FJsonValue>>{},
			TArray<TSharedPtr<FJsonValue>>{});
		LoadBlueprintDocumentBody(CreateRequest.Document)->SetArrayField(
			TEXT("Components"),
			MakeComponentArray({MakeOwnedSphereComponent(TEXT("Sensor"), 500.0)}));
		TestTrue(TEXT("Initial stale-parent Sensor apply succeeds"), CreateRequest.Apply(Service).IsSuccess());

		TSharedPtr<FJsonObject> BadChild = MakeOwnedSceneComponent(TEXT("BadChild"));
		BadChild->SetObjectField(TEXT("AttachTo"), MakeComponentKey(TEXT("Sensor")));

		FUBlueprintTestDocument BadRequest(CreateRequest);
		BadRequest.Document = MakeUBlueprintDocument(
			Target,
			TEXT("/Script/Engine.Actor"),
			TArray<TSharedPtr<FJsonValue>>{},
			TArray<TSharedPtr<FJsonValue>>{});
		LoadBlueprintDocumentBody(BadRequest.Document)->SetArrayField(TEXT("Components"), MakeComponentArray({BadChild}));

		const FAssetDocumentResult BadResult = BadRequest.Apply(Service);
		TestFalse(TEXT("AttachTo removed owned parent rejects apply"), BadResult.IsSuccess());

		UBlueprint* Blueprint = LoadBlueprintForTarget(Target);
		TestNotNull(TEXT("Blueprint remains loadable after stale parent apply"), Blueprint);
		if (Blueprint)
		{
			TestNotNull(TEXT("Existing Sensor survives stale parent apply"), FindSCSNodeByVariableName(Blueprint, TEXT("Sensor")));
			TestNull(TEXT("BadChild is not left behind after stale parent apply"), FindSCSNodeByVariableName(Blueprint, TEXT("BadChild")));
		}
	}

	{
		const FString Target = FString::Printf(TEXT("/Game/AssetDocumentTests/BP_AD_ComponentCycle_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
		TSharedPtr<FJsonObject> A = MakeOwnedSceneComponent(TEXT("A"));
		TSharedPtr<FJsonObject> B = MakeOwnedSceneComponent(TEXT("B"));
		A->SetObjectField(TEXT("AttachTo"), MakeComponentKey(TEXT("B")));
		B->SetObjectField(TEXT("AttachTo"), MakeComponentKey(TEXT("A")));

		FUBlueprintTestDocument Request(Target);
		Request.Document = MakeUBlueprintDocument(Target, TEXT("/Script/Engine.Actor"), TArray<TSharedPtr<FJsonValue>>{}, TArray<TSharedPtr<FJsonValue>>{});
		LoadBlueprintDocumentBody(Request.Document)->SetArrayField(TEXT("Components"), MakeComponentArray({A, B}));

		const FAssetDocumentResult Result = Request.Apply(Service);
		TestFalse(TEXT("Attach cycle rejects apply"), Result.IsSuccess());
	}

	{
		const FString Target = FString::Printf(TEXT("/Game/AssetDocumentTests/BP_AD_ComponentProtectedName_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
		FUBlueprintTestDocument Request(Target);
		Request.Document = MakeUBlueprintDocument(Target, TEXT("/Script/Engine.Actor"), TArray<TSharedPtr<FJsonValue>>{}, TArray<TSharedPtr<FJsonValue>>{});
		LoadBlueprintDocumentBody(Request.Document)->SetArrayField(
			TEXT("Components"),
			MakeComponentArray({MakeOwnedSceneComponent(*USceneComponent::GetDefaultSceneRootVariableName().ToString())}));

		const FAssetDocumentResult Result = Request.Apply(Service);
		TestFalse(TEXT("DefaultSceneRoot protected component name rejects apply"), Result.IsSuccess());
	}

	{
		const FString Target = FString::Printf(TEXT("/Game/AssetDocumentTests/BP_AD_ComponentRootRules_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
		TSharedPtr<FJsonObject> A = MakeOwnedSceneComponent(TEXT("A"));
		TSharedPtr<FJsonObject> B = MakeOwnedSceneComponent(TEXT("B"));
		A->SetBoolField(TEXT("Root"), true);
		B->SetBoolField(TEXT("Root"), true);

		FUBlueprintTestDocument MultiRootRequest(Target);
		MultiRootRequest.Document = MakeUBlueprintDocument(Target, TEXT("/Script/Engine.Actor"), TArray<TSharedPtr<FJsonValue>>{}, TArray<TSharedPtr<FJsonValue>>{});
		LoadBlueprintDocumentBody(MultiRootRequest.Document)->SetArrayField(TEXT("Components"), MakeComponentArray({A, B}));
		TestFalse(TEXT("Multiple Root components reject apply"), MultiRootRequest.Apply(Service).IsSuccess());

		TSharedPtr<FJsonObject> RootWithAttach = MakeOwnedSceneComponent(TEXT("RootWithAttach"));
		RootWithAttach->SetBoolField(TEXT("Root"), true);

		FUBlueprintTestDocument RootAttachRequest(MultiRootRequest);
		RootAttachRequest.Document = MakeUBlueprintDocument(Target, TEXT("/Script/Engine.Actor"), TArray<TSharedPtr<FJsonValue>>{}, TArray<TSharedPtr<FJsonValue>>{});
		LoadBlueprintDocumentBody(RootAttachRequest.Document)->SetArrayField(TEXT("Components"), MakeComponentArray({RootWithAttach}));
		TestFalse(TEXT("Root with AttachTo rejects apply"), RootAttachRequest.Apply(Service).IsSuccess());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintOwnedSCSComponentsValidationAndDiffTest,
	"UEAssetDoc.UBlueprint.OwnedSCSComponentsValidationAndDiff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintOwnedSCSComponentsValidationAndDiffTest::RunTest(const FString&)
{
	const FUBlueprintAssetDocumentCapability Capability;
	FAssetDocumentCapabilityContext ValidationContext;
	ValidationContext.AssetClass = UBlueprint::StaticClass();

	TSharedPtr<FJsonObject> InvalidDocument = MakeUBlueprintDocument(
		TEXT("/Game/AssetDocumentTests/BP_AD_InvalidComponentParent"),
		TEXT("/Script/Engine.GameInstance"),
		TArray<TSharedPtr<FJsonValue>>{},
		TArray<TSharedPtr<FJsonValue>>{});
	LoadBlueprintDocumentBody(InvalidDocument)->SetArrayField(
		TEXT("Components"),
		MakeComponentArray({MakeOwnedSphereComponent(TEXT("Sensor"), 500.0)}));

	const FAssetDocumentCapabilityResult InvalidResult = Capability.Validate(
		ValidationContext,
		UBlueprintTestMakeBodyValue(LoadBlueprintDocumentBody(InvalidDocument).ToSharedRef()));
	TestFalse(TEXT("OwnedSCS component with non-Actor parent fails validation"), InvalidResult.bSuccess);
	const bool bHasExpectedInvalidParentDiagnostic = UBlueprintTestResultHasDiagnostic(InvalidResult, TEXT("/Body/Components"), TEXT("OwnedSCSRequiresActorParent"));
	if (!bHasExpectedInvalidParentDiagnostic)
	{
		for (const FAssetDocumentDiagnostic& Diagnostic : InvalidResult.Diagnostics)
		{
			AddError(FString::Printf(TEXT("Unexpected non-Actor component diagnostic: Path=%s Code=%s Message=%s"), *Diagnostic.Path, *Diagnostic.Code, *Diagnostic.Message));
		}
	}
	TestTrue(TEXT("Non-Actor component failure is reported at Components"), bHasExpectedInvalidParentDiagnostic);

	const FString Target = FString::Printf(TEXT("/Game/AssetDocumentTests/BP_AD_ComponentDiff_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	TSharedPtr<FJsonObject> Document = MakeUBlueprintDocument(
		Target,
		TEXT("/Script/Engine.Actor"),
		TArray<TSharedPtr<FJsonValue>>{},
		TArray<TSharedPtr<FJsonValue>>{});
	LoadBlueprintDocumentBody(Document)->SetArrayField(
		TEXT("Components"),
		MakeComponentArray({MakeOwnedSphereComponent(TEXT("Sensor"), 500.0)}));

	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	FUBlueprintTestDocument Request(Target);
	Request.Document = Document;
	const FAssetDocumentResult ApplyResult = Request.Apply(Service);
	if (!ApplyResult.IsSuccess())
	{
		AddError(FString::Printf(TEXT("Component diff fixture apply failed: %s"), *DescribeUBlueprintTestResult(ApplyResult)));
	}
	TestTrue(TEXT("Component diff fixture apply succeeds"), ApplyResult.IsSuccess());

	UBlueprint* Blueprint = LoadBlueprintForTarget(Target);
	TestNotNull(TEXT("Component diff Blueprint exists"), Blueprint);
	if (Blueprint)
	{
		FAssetDocumentCapabilityContext Context;
		Context.Asset = Blueprint;
		Context.AssetClass = UBlueprint::StaticClass();

		TSharedRef<FJsonObject> ExtractedBody = MakeShared<FJsonObject>();
		const FAssetDocumentCapabilityResult ExtractResult = Capability.Extract(Context, ExtractedBody);
		TestTrue(TEXT("Component extract succeeds"), ExtractResult.bSuccess);

		const TArray<TSharedPtr<FJsonValue>>* ExtractedComponents = nullptr;
		TestTrue(TEXT("Extract includes Components"), ExtractedBody->TryGetArrayField(TEXT("Components"), ExtractedComponents));
		TSharedPtr<FJsonObject> Sensor = ExtractedComponents ? FindUBlueprintJsonObjectByStringField(*ExtractedComponents, TEXT("Scope"), TEXT("OwnedSCS")) : nullptr;
		TestTrue(TEXT("Extract includes an OwnedSCS component"), Sensor.IsValid());
		if (Sensor.IsValid())
		{
			const TSharedPtr<FJsonObject>* Key = nullptr;
			TestTrue(TEXT("Extracted component includes Key"), Sensor->TryGetObjectField(TEXT("Key"), Key));
			if (Key && Key->IsValid())
			{
				TestEqual(TEXT("Extracted component Key.Name is Sensor"), (*Key)->GetStringField(TEXT("Name")), FString(TEXT("Sensor")));
				TestEqual(TEXT("Extracted component Key.OwnerClass is Self"), (*Key)->GetStringField(TEXT("OwnerClass")), FString(TEXT("Self")));
			}
			TestEqual(TEXT("Extracted component class is SphereComponent"), Sensor->GetStringField(TEXT("Class")), FString(TEXT("/Script/Engine.SphereComponent")));
			const TSharedPtr<FJsonObject>* AttachTo = nullptr;
			TestTrue(TEXT("Extracted component includes AttachTo"), Sensor->TryGetObjectField(TEXT("AttachTo"), AttachTo));
			if (AttachTo && AttachTo->IsValid())
			{
				TestEqual(TEXT("Extracted AttachTo.Name is DefaultSceneRoot"), (*AttachTo)->GetStringField(TEXT("Name")), FString(TEXT("DefaultSceneRoot")));
				TestEqual(TEXT("Extracted AttachTo.OwnerClass is Self"), (*AttachTo)->GetStringField(TEXT("OwnerClass")), FString(TEXT("Self")));
			}
			const TSharedPtr<FJsonObject>* Properties = nullptr;
			TestTrue(TEXT("Extracted component includes Properties"), Sensor->TryGetObjectField(TEXT("Properties"), Properties));
			if (Properties && Properties->IsValid())
			{
				TestEqual(TEXT("Extracted SphereRadius is default diff"), (*Properties)->GetNumberField(TEXT("SphereRadius")), 500.0);
			}
		}

		TArray<TSharedPtr<FJsonValue>> DiffEntries;
		const FAssetDocumentCapabilityResult DiffResult = Capability.Diff(Context, UBlueprintTestMakeBodyValue(LoadBlueprintDocumentBody(Document).ToSharedRef()), DiffEntries);
		TestTrue(TEXT("Component diff succeeds"), DiffResult.bSuccess);
		TSharedPtr<FJsonObject> SensorDiff = FindUBlueprintDiffEntryByPath(DiffEntries, TEXT("/Body/Components/Self:Sensor"));
		TestTrue(TEXT("Component diff includes Sensor"), SensorDiff.IsValid());
		if (SensorDiff.IsValid())
		{
			TestEqual(TEXT("Sensor component diff is unchanged"), SensorDiff->GetStringField(TEXT("status")), FString(TEXT("unchanged")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintInvalidAuthoritativeArraysTest,
	"UEAssetDoc.UBlueprint.InvalidAuthoritativeArrays",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintInvalidAuthoritativeArraysTest::RunTest(const FString&)
{
	const FString Target = FString::Printf(TEXT("/Game/AssetDocumentTests/BP_AD_InvalidArrays_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	FUBlueprintTestDocument FixtureRequest(Target);

	auto ApplyValidHealth = [&]()
	{
		FUBlueprintTestDocument Request(FixtureRequest);
		Request.Document = MakeUBlueprintDocument(
			Target,
			TEXT("/Script/Engine.Actor"),
			MakeUBlueprintVariableArray({MakeUBlueprintFloatVariable(TEXT("Health"), TEXT("100.0"))}),
			TArray<TSharedPtr<FJsonValue>>{});
		const FAssetDocumentResult Result = Request.Apply(Service);
		TestTrue(TEXT("Valid Health apply succeeds"), Result.IsSuccess());
	};

	ApplyValidHealth();

	TArray<TSharedPtr<FJsonValue>> InvalidVariables;
	InvalidVariables.Add(MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()));
	InvalidVariables.Add(MakeShared<FJsonValueString>(TEXT("not an array")));
	InvalidVariables.Add(MakeShared<FJsonValueNull>());

	for (const TSharedPtr<FJsonValue>& InvalidValue : InvalidVariables)
	{
		ApplyValidHealth();

		TSharedPtr<FJsonObject> Document = MakeUBlueprintDocument(
			Target,
			TEXT("/Script/Engine.Actor"),
			MakeUBlueprintVariableArray({MakeUBlueprintFloatVariable(TEXT("Health"), TEXT("100.0"))}),
			TArray<TSharedPtr<FJsonValue>>{});
		TSharedPtr<FJsonObject> Body = LoadBlueprintDocumentBody(Document);
		check(Body.IsValid());
		Body->SetField(TEXT("Variables"), InvalidValue);

		FUBlueprintTestDocument Request(FixtureRequest);
		Request.Document = Document;
		const FAssetDocumentResult Result = Request.Apply(Service);
		TestFalse(TEXT("Invalid Variables field fails apply"), Result.IsSuccess());

		UBlueprint* Blueprint = LoadBlueprintForTarget(Target);
		TestTrue(TEXT("Health remains after invalid Variables field"), HasUBlueprintVariable(Blueprint, TEXT("Health")));
	}

	const FUBlueprintAssetDocumentCapability Capability;
	FAssetDocumentCapabilityContext Context;
	Context.AssetClass = UBlueprint::StaticClass();

	TSharedRef<FJsonObject> InterfaceObjectBody = MakeShared<FJsonObject>();
	InterfaceObjectBody->SetObjectField(TEXT("ParentClass"), UBlueprintTestMakeActorParentClassRef());
	InterfaceObjectBody->SetObjectField(TEXT("ImplementedInterfaces"), MakeShared<FJsonObject>());
	TestFalse(TEXT("ImplementedInterfaces object fails validation"), Capability.Validate(Context, UBlueprintTestMakeBodyValue(InterfaceObjectBody)).bSuccess);

	TSharedRef<FJsonObject> InterfaceStringBody = MakeShared<FJsonObject>();
	InterfaceStringBody->SetObjectField(TEXT("ParentClass"), UBlueprintTestMakeActorParentClassRef());
	InterfaceStringBody->SetStringField(TEXT("ImplementedInterfaces"), TEXT("not an array"));
	TestFalse(TEXT("ImplementedInterfaces string fails validation"), Capability.Validate(Context, UBlueprintTestMakeBodyValue(InterfaceStringBody)).bSuccess);

	TSharedRef<FJsonObject> InterfaceNullBody = MakeShared<FJsonObject>();
	InterfaceNullBody->SetObjectField(TEXT("ParentClass"), UBlueprintTestMakeActorParentClassRef());
	InterfaceNullBody->SetField(TEXT("ImplementedInterfaces"), MakeShared<FJsonValueNull>());
	TestFalse(TEXT("ImplementedInterfaces null fails validation"), Capability.Validate(Context, UBlueprintTestMakeBodyValue(InterfaceNullBody)).bSuccess);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintInheritedSCSOverrideTest,
	"UEAssetDoc.UBlueprint.InheritedSCSOverride",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintInheritedSCSOverrideTest::RunTest(const FString&)
{
	const FString ParentTarget = FString::Printf(TEXT("/Game/AssetDocumentTests/BP_AD_InheritedParent_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString ChildTarget = FString::Printf(TEXT("/Game/AssetDocumentTests/BP_AD_InheritedChild_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();

	FUBlueprintTestDocument ParentRequest(ParentTarget);
	ParentRequest.Document = MakeUBlueprintDocument(
		ParentTarget,
		TEXT("/Script/Engine.Actor"),
		TArray<TSharedPtr<FJsonValue>>{},
		TArray<TSharedPtr<FJsonValue>>{},
		MakeComponentArray({MakeOwnedSphereComponent(TEXT("ParentSensor"), 150.0)}));
	const FAssetDocumentResult ParentResult = ParentRequest.Apply(Service);
	if (!ParentResult.IsSuccess())
	{
		AddError(FString::Printf(TEXT("Parent apply failed: %s"), *DescribeUBlueprintTestResult(ParentResult)));
	}
	TestTrue(TEXT("Parent Blueprint with owned component applies"), ParentResult.IsSuccess());

	UBlueprint* ParentBlueprint = LoadBlueprintForTarget(ParentTarget);
	TestNotNull(TEXT("Parent Blueprint exists"), ParentBlueprint);
	if (!ParentBlueprint || !ParentBlueprint->GeneratedClass)
	{
		return true;
	}

	USCS_Node* ParentSensorNode = FindSCSNodeByVariableName(ParentBlueprint, TEXT("ParentSensor"));
	TestNotNull(TEXT("Parent SCS component exists"), ParentSensorNode);
	if (!ParentSensorNode)
	{
		return true;
	}

	const FString ParentGeneratedClassPath = ParentBlueprint->GeneratedClass->GetPathName();
	TSharedPtr<FJsonObject> InheritedComponent = MakeReferencedComponent(TEXT("Inherited"), TEXT("ParentSensor"), *ParentGeneratedClassPath);
	InheritedComponent->SetStringField(TEXT("Class"), TEXT("/Script/Engine.SphereComponent"));
	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetNumberField(TEXT("SphereRadius"), 900.0);
	InheritedComponent->SetObjectField(TEXT("Properties"), Properties);

	FUBlueprintTestDocument ChildOverrideRequest(ChildTarget);
	ChildOverrideRequest.Document = MakeUBlueprintDocument(
		ChildTarget,
		ParentGeneratedClassPath,
		TArray<TSharedPtr<FJsonValue>>{},
		TArray<TSharedPtr<FJsonValue>>{},
		MakeComponentArray({InheritedComponent}));
	const FAssetDocumentResult OverrideResult = ChildOverrideRequest.Apply(Service);
	if (!OverrideResult.IsSuccess())
	{
		AddError(FString::Printf(TEXT("Inherited override apply failed: %s"), *DescribeUBlueprintTestResult(OverrideResult)));
	}
	TestTrue(TEXT("Inherited SCS component override applies"), OverrideResult.IsSuccess());

	UBlueprint* ChildBlueprint = LoadBlueprintForTarget(ChildTarget);
	TestNotNull(TEXT("Child Blueprint exists"), ChildBlueprint);
	if (ChildBlueprint)
	{
		const FComponentKey ParentSensorKey(ParentSensorNode);
		UInheritableComponentHandler* Handler = ChildBlueprint->GetInheritableComponentHandler(false);
		TestNotNull(TEXT("Child has inheritable component handler after override"), Handler);
		UActorComponent* OverrideTemplate = Handler ? Handler->GetOverridenComponentTemplate(ParentSensorKey) : nullptr;
		USphereComponent* SphereOverride = Cast<USphereComponent>(OverrideTemplate);
		TestNotNull(TEXT("Inherited override template is a SphereComponent"), SphereOverride);
		if (SphereOverride)
		{
			TestEqual(TEXT("Inherited SphereRadius override is applied"), SphereOverride->GetUnscaledSphereRadius(), 900.0f);
		}
	}

	if (ChildBlueprint)
	{
		const FUBlueprintAssetDocumentCapability Capability;
		FAssetDocumentCapabilityContext Context;
		Context.Asset = ChildBlueprint;
		Context.AssetClass = UBlueprint::StaticClass();

		TSharedRef<FJsonObject> ExtractedBody = MakeShared<FJsonObject>();
		const FAssetDocumentCapabilityResult ExtractResult = Capability.Extract(Context, ExtractedBody);
		TestTrue(TEXT("Inherited component extract succeeds"), ExtractResult.bSuccess);
		const TArray<TSharedPtr<FJsonValue>>* ExtractedComponents = nullptr;
		TestTrue(TEXT("Extract includes inherited Components"), ExtractedBody->TryGetArrayField(TEXT("Components"), ExtractedComponents));
		TSharedPtr<FJsonObject> ExtractedInherited = ExtractedComponents
			? FindComponentByScopeAndKey(*ExtractedComponents, TEXT("Inherited"), TEXT("ParentSensor"), ParentGeneratedClassPath)
			: nullptr;
		TestTrue(TEXT("Extract includes inherited override component"), ExtractedInherited.IsValid());

		TArray<TSharedPtr<FJsonValue>> DiffEntries;
		const FAssetDocumentCapabilityResult DiffResult =
			Capability.Diff(Context, UBlueprintTestMakeBodyValue(LoadBlueprintDocumentBody(ChildOverrideRequest.Document).ToSharedRef()), DiffEntries);
		TestTrue(TEXT("Inherited component diff succeeds"), DiffResult.bSuccess);
		TSharedPtr<FJsonObject> InheritedDiff = FindUBlueprintDiffEntryByPath(DiffEntries, FString::Printf(TEXT("/Body/Components/%s:ParentSensor"), *ParentGeneratedClassPath));
		TestNotNull(TEXT("Inherited component diff includes override path"), InheritedDiff.Get());
		TestTrue(TEXT("Inherited component roundtrip diff is unchanged"), IsUnchangedDiffEntry(InheritedDiff));
		// Retained C++ capability diff is not the production diff any more; check the production one.
		TestUBlueprintServiceNoDiff(*this, TEXT("Inherited component roundtrip"), ChildOverrideRequest.Document);
	}

	FUBlueprintTestDocument ChildResetRequest(ChildOverrideRequest);
	ChildResetRequest.Document = MakeUBlueprintDocument(
		ChildTarget,
		ParentGeneratedClassPath,
		TArray<TSharedPtr<FJsonValue>>{},
		TArray<TSharedPtr<FJsonValue>>{},
		TArray<TSharedPtr<FJsonValue>>{});
	const FAssetDocumentResult ResetResult = ChildResetRequest.Apply(Service);
	if (!ResetResult.IsSuccess())
	{
		AddError(FString::Printf(TEXT("Inherited override reset apply failed: %s"), *DescribeUBlueprintTestResult(ResetResult)));
	}
	TestTrue(TEXT("Omitting inherited component clears override"), ResetResult.IsSuccess());

	ChildBlueprint = LoadBlueprintForTarget(ChildTarget);
	if (ChildBlueprint)
	{
		const FComponentKey ParentSensorKey(ParentSensorNode);
		UInheritableComponentHandler* Handler = ChildBlueprint->GetInheritableComponentHandler(false);
		UActorComponent* OverrideTemplate = Handler ? Handler->GetOverridenComponentTemplate(ParentSensorKey) : nullptr;
		TestNull(TEXT("Inherited override template is removed when omitted"), OverrideTemplate);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintNativeComponentOverrideTest,
	"UEAssetDoc.UBlueprint.NativeComponentOverride",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintNativeComponentOverrideTest::RunTest(const FString&)
{
	const FString Target = FString::Printf(TEXT("/Game/AssetDocumentTests/BP_AD_NativeComponent_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();

	TSharedPtr<FJsonObject> NativeMovement = MakeReferencedComponent(TEXT("Native"), TEXT("CharacterMovement"), TEXT("/Script/Engine.Character"));
	NativeMovement->SetStringField(TEXT("Class"), TEXT("/Script/Engine.CharacterMovementComponent"));
	TSharedPtr<FJsonObject> MovementProperties = MakeShared<FJsonObject>();
	MovementProperties->SetNumberField(TEXT("MaxWalkSpeed"), 700.0);
	NativeMovement->SetObjectField(TEXT("Properties"), MovementProperties);

	FUBlueprintTestDocument OverrideRequest(Target);
	OverrideRequest.Document = MakeUBlueprintDocument(
		Target,
		TEXT("/Script/Engine.Character"),
		TArray<TSharedPtr<FJsonValue>>{},
		TArray<TSharedPtr<FJsonValue>>{},
		MakeComponentArray({NativeMovement}));
	const FAssetDocumentResult OverrideResult = OverrideRequest.Apply(Service);
	if (!OverrideResult.IsSuccess())
	{
		AddError(FString::Printf(TEXT("Native component override apply failed: %s"), *DescribeUBlueprintTestResult(OverrideResult)));
	}
	TestTrue(TEXT("Native CharacterMovement override applies"), OverrideResult.IsSuccess());

	UBlueprint* Blueprint = LoadBlueprintForTarget(Target);
	TestNotNull(TEXT("Native component Blueprint exists"), Blueprint);
	if (Blueprint && Blueprint->GeneratedClass)
	{
		ACharacter* CDO = Cast<ACharacter>(Blueprint->GeneratedClass->GetDefaultObject());
		TestNotNull(TEXT("Generated CDO is Character"), CDO);
		UCharacterMovementComponent* Movement = CDO ? CDO->GetCharacterMovement() : nullptr;
		TestNotNull(TEXT("Generated CDO has CharacterMovement"), Movement);
		if (Movement)
		{
			TestEqual(TEXT("Native MaxWalkSpeed override is applied"), Movement->MaxWalkSpeed, 700.0f);
		}
	}

	if (Blueprint)
	{
		const FUBlueprintAssetDocumentCapability Capability;
		FAssetDocumentCapabilityContext Context;
		Context.Asset = Blueprint;
		Context.AssetClass = UBlueprint::StaticClass();

		TSharedRef<FJsonObject> ExtractedBody = MakeShared<FJsonObject>();
		const FAssetDocumentCapabilityResult ExtractResult = Capability.Extract(Context, ExtractedBody);
		TestTrue(TEXT("Native component extract succeeds"), ExtractResult.bSuccess);
		const TArray<TSharedPtr<FJsonValue>>* ExtractedComponents = nullptr;
		TestTrue(TEXT("Extract includes native Components"), ExtractedBody->TryGetArrayField(TEXT("Components"), ExtractedComponents));
		TSharedPtr<FJsonObject> ExtractedNative = ExtractedComponents
			? FindComponentByScopeAndKey(*ExtractedComponents, TEXT("Native"), TEXT("CharacterMovement"), TEXT("/Script/Engine.Character"))
			: nullptr;
		TestTrue(TEXT("Extract includes native CharacterMovement override"), ExtractedNative.IsValid());

		TArray<TSharedPtr<FJsonValue>> DiffEntries;
		const FAssetDocumentCapabilityResult DiffResult =
			Capability.Diff(Context, UBlueprintTestMakeBodyValue(LoadBlueprintDocumentBody(OverrideRequest.Document).ToSharedRef()), DiffEntries);
		TestTrue(TEXT("Native component diff succeeds"), DiffResult.bSuccess);
		TSharedPtr<FJsonObject> NativeDiff = FindUBlueprintDiffEntryByPath(DiffEntries, TEXT("/Body/Components//Script/Engine.Character:CharacterMovement"));
		TestNotNull(TEXT("Native component diff includes alias path"), NativeDiff.Get());
		TestTrue(TEXT("Native component roundtrip diff is unchanged"), IsUnchangedDiffEntry(NativeDiff));
		// Retained C++ capability diff is not the production diff any more; check the production one.
		TestUBlueprintServiceNoDiff(*this, TEXT("Native component roundtrip"), OverrideRequest.Document);

		TSharedPtr<FJsonObject> NativeMovementByObjectName = MakeReferencedComponent(TEXT("Native"), TEXT("CharMoveComp"), TEXT("/Script/Engine.Character"));
		NativeMovementByObjectName->SetStringField(TEXT("Class"), TEXT("/Script/Engine.CharacterMovementComponent"));
		NativeMovementByObjectName->SetObjectField(TEXT("Properties"), MovementProperties);
		TSharedPtr<FJsonObject> ObjectNameDocument = MakeUBlueprintDocument(
			Target,
			TEXT("/Script/Engine.Character"),
			TArray<TSharedPtr<FJsonValue>>{},
			TArray<TSharedPtr<FJsonValue>>{},
			MakeComponentArray({NativeMovementByObjectName}));
		TSharedPtr<FJsonObject> ObjectNameBody = LoadBlueprintDocumentBody(ObjectNameDocument);

		TArray<TSharedPtr<FJsonValue>> ObjectNameDiffEntries;
		const FAssetDocumentCapabilityResult ObjectNameDiffResult = Capability.Diff(Context, UBlueprintTestMakeBodyValue(ObjectNameBody.ToSharedRef()), ObjectNameDiffEntries);
		TestTrue(TEXT("Native object-name component diff succeeds"), ObjectNameDiffResult.bSuccess);
		TSharedPtr<FJsonObject> ObjectNameNativeDiff =
			FindUBlueprintDiffEntryByPath(ObjectNameDiffEntries, TEXT("/Body/Components//Script/Engine.Character:CharacterMovement"));
		TestNotNull(TEXT("Native object-name diff canonicalizes to alias path"), ObjectNameNativeDiff.Get());
		TestTrue(TEXT("Native object-name desired diff is unchanged"), IsUnchangedDiffEntry(ObjectNameNativeDiff));
		// Production diff names native components by their property alias (CharacterMovement);
		// the subobject name (CharMoveComp) is accepted by apply but not by diff.
	}

	const ACharacter* ParentCDO = GetDefault<ACharacter>();
	const float ParentMaxWalkSpeed = ParentCDO && ParentCDO->GetCharacterMovement()
		? ParentCDO->GetCharacterMovement()->MaxWalkSpeed
		: 600.0f;

	FUBlueprintTestDocument ResetRequest(OverrideRequest);
	ResetRequest.Document = MakeUBlueprintDocument(
		Target,
		TEXT("/Script/Engine.Character"),
		TArray<TSharedPtr<FJsonValue>>{},
		TArray<TSharedPtr<FJsonValue>>{},
		TArray<TSharedPtr<FJsonValue>>{});
	const FAssetDocumentResult ResetResult = ResetRequest.Apply(Service);
	if (!ResetResult.IsSuccess())
	{
		AddError(FString::Printf(TEXT("Native component reset apply failed: %s"), *DescribeUBlueprintTestResult(ResetResult)));
	}
	TestTrue(TEXT("Omitting native component resets override"), ResetResult.IsSuccess());

	Blueprint = LoadBlueprintForTarget(Target);
	if (Blueprint && Blueprint->GeneratedClass)
	{
		ACharacter* CDO = Cast<ACharacter>(Blueprint->GeneratedClass->GetDefaultObject());
		UCharacterMovementComponent* Movement = CDO ? CDO->GetCharacterMovement() : nullptr;
		TestNotNull(TEXT("Generated CDO still has CharacterMovement after reset"), Movement);
		if (Movement)
		{
			TestEqual(TEXT("Native MaxWalkSpeed resets to parent CDO baseline"), Movement->MaxWalkSpeed, ParentMaxWalkSpeed);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintParentChangePreflightsBeforeOwnedSCSMutationTest,
	"UEAssetDoc.UBlueprint.ParentChangePreflightsBeforeOwnedSCSMutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintParentChangePreflightsBeforeOwnedSCSMutationTest::RunTest(const FString&)
{
	const FString Target = FString::Printf(TEXT("/Game/AssetDocumentTests/BP_AD_ParentPreflight_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();

	FUBlueprintTestDocument CreateRequest(Target);
	CreateRequest.Document = MakeUBlueprintDocument(
		Target,
		TEXT("/Script/Engine.Actor"),
		TArray<TSharedPtr<FJsonValue>>{},
		TArray<TSharedPtr<FJsonValue>>{},
		TArray<TSharedPtr<FJsonValue>>{});
	const FAssetDocumentResult CreateResult = CreateRequest.Apply(Service);
	TestTrue(TEXT("Initial Blueprint applies"), CreateResult.IsSuccess());

	TSharedPtr<FJsonObject> BadClassDefaults = MakeShared<FJsonObject>();
	BadClassDefaults->SetNumberField(TEXT("NotARealClassDefault"), 1.0);

	FUBlueprintTestDocument BadRequest(CreateRequest);
	BadRequest.Document = MakeUBlueprintDocument(
		Target,
		TEXT("/Script/Engine.Pawn"),
		TArray<TSharedPtr<FJsonValue>>{},
		TArray<TSharedPtr<FJsonValue>>{},
		MakeComponentArray({MakeOwnedSphereComponent(TEXT("ShouldNotPersist"), 64.0)}),
		BadClassDefaults);
	const FAssetDocumentResult BadResult = BadRequest.Apply(Service);
	TestFalse(TEXT("Parent change with invalid post-compile class default fails"), BadResult.IsSuccess());

	UBlueprint* Blueprint = LoadBlueprintForTarget(Target);
	TestNotNull(TEXT("Blueprint still exists after failed parent change"), Blueprint);
	if (Blueprint)
	{
		TestEqual(TEXT("Parent class is restored after failed parent change"), Blueprint->ParentClass.Get(), AActor::StaticClass());
		TestNull(TEXT("OwnedSCS component is not mutated before post-compile preflight"), FindSCSNodeByVariableName(Blueprint, TEXT("ShouldNotPersist")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintInheritedNativeAttachRootUnsupportedTest,
	"UEAssetDoc.UBlueprint.InheritedNativeAttachRootUnsupported",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintInheritedNativeAttachRootUnsupportedTest::RunTest(const FString&)
{
	const FUBlueprintAssetDocumentCapability Capability;
	FAssetDocumentCapabilityContext Context;
	Context.AssetClass = UBlueprint::StaticClass();

	TSharedPtr<FJsonObject> NativeWithAttach = MakeReferencedComponent(TEXT("Native"), TEXT("CharacterMovement"), TEXT("/Script/Engine.Character"));
	NativeWithAttach->SetStringField(TEXT("Class"), TEXT("/Script/Engine.CharacterMovementComponent"));
	NativeWithAttach->SetObjectField(TEXT("AttachTo"), MakeComponentKey(TEXT("CapsuleComponent"), TEXT("/Script/Engine.Character")));

	TSharedRef<FJsonObject> NativeBody = MakeShared<FJsonObject>();
	NativeBody->SetObjectField(TEXT("ParentClass"), MakeUBlueprintClassRefObject(TEXT("/Script/Engine.Character")));
	NativeBody->SetArrayField(TEXT("Components"), MakeComponentArray({NativeWithAttach}));
	const FAssetDocumentCapabilityResult NativeResult = Capability.Validate(Context, UBlueprintTestMakeBodyValue(NativeBody));
	TestFalse(TEXT("Native component AttachTo fails validation"), NativeResult.bSuccess);
	TestTrue(TEXT("Native AttachTo uses unsupported attach/root diagnostic"), UBlueprintTestResultHasDiagnosticCode(NativeResult, TEXT("UnsupportedInheritedComponentAttachRoot")));

	TSharedPtr<FJsonObject> InheritedWithRoot = MakeReferencedComponent(TEXT("Inherited"), TEXT("ParentSensor"), TEXT("/Game/AssetDocumentTests/BP_UnresolvedParent.BP_UnresolvedParent_C"));
	InheritedWithRoot->SetStringField(TEXT("Class"), TEXT("/Script/Engine.SphereComponent"));
	InheritedWithRoot->SetBoolField(TEXT("Root"), true);

	TSharedRef<FJsonObject> InheritedBody = MakeShared<FJsonObject>();
	InheritedBody->SetObjectField(TEXT("ParentClass"), UBlueprintTestMakeActorParentClassRef());
	InheritedBody->SetArrayField(TEXT("Components"), MakeComponentArray({InheritedWithRoot}));
	const FAssetDocumentCapabilityResult InheritedResult = Capability.Validate(Context, UBlueprintTestMakeBodyValue(InheritedBody));
	TestFalse(TEXT("Inherited component Root fails validation"), InheritedResult.bSuccess);
	TestTrue(TEXT("Inherited Root uses unsupported attach/root diagnostic"), UBlueprintTestResultHasDiagnosticCode(InheritedResult, TEXT("UnsupportedInheritedComponentAttachRoot")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintClassDefaultsAuthoritativeTest,
	"UEAssetDoc.UBlueprint.ClassDefaultsAuthoritative",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintClassDefaultsAuthoritativeTest::RunTest(const FString&)
{
	const FString Target = FString::Printf(TEXT("/Game/AssetDocumentTests/BP_AD_ClassDefaults_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();

	TSharedPtr<FJsonObject> ClassDefaults = MakeShared<FJsonObject>();
	ClassDefaults->SetNumberField(TEXT("InitialLifeSpan"), 12.5);

	FUBlueprintTestDocument ApplyRequest(Target);
	ApplyRequest.Document = MakeUBlueprintDocument(
		Target,
		TEXT("/Script/Engine.Actor"),
		TArray<TSharedPtr<FJsonValue>>{},
		TArray<TSharedPtr<FJsonValue>>{},
		TArray<TSharedPtr<FJsonValue>>{},
		ClassDefaults);
	const FAssetDocumentResult ApplyResult = ApplyRequest.Apply(Service);
	if (!ApplyResult.IsSuccess())
	{
		AddError(FString::Printf(TEXT("ClassDefaults apply failed: %s"), *DescribeUBlueprintTestResult(ApplyResult)));
	}
	TestTrue(TEXT("ClassDefaults apply succeeds"), ApplyResult.IsSuccess());

	UBlueprint* Blueprint = LoadBlueprintForTarget(Target);
	TestNotNull(TEXT("ClassDefaults Blueprint exists"), Blueprint);
	if (Blueprint && Blueprint->GeneratedClass)
	{
		AActor* CDO = Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject());
		TestNotNull(TEXT("Generated CDO is Actor"), CDO);
		if (CDO)
		{
			TestEqual(TEXT("InitialLifeSpan class default is applied"), CDO->InitialLifeSpan, 12.5f);
		}
	}

	const float ParentInitialLifeSpan = GetDefault<AActor>()->InitialLifeSpan;
	FUBlueprintTestDocument ResetRequest(ApplyRequest);
	ResetRequest.Document = MakeUBlueprintDocument(
		Target,
		TEXT("/Script/Engine.Actor"),
		TArray<TSharedPtr<FJsonValue>>{},
		TArray<TSharedPtr<FJsonValue>>{},
		TArray<TSharedPtr<FJsonValue>>{},
		MakeShared<FJsonObject>());
	const FAssetDocumentResult ResetResult = ResetRequest.Apply(Service);
	if (!ResetResult.IsSuccess())
	{
		AddError(FString::Printf(TEXT("ClassDefaults reset failed: %s"), *DescribeUBlueprintTestResult(ResetResult)));
	}
	TestTrue(TEXT("Omitted ClassDefaults property resets to parent CDO"), ResetResult.IsSuccess());

	Blueprint = LoadBlueprintForTarget(Target);
	if (Blueprint && Blueprint->GeneratedClass)
	{
		AActor* CDO = Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject());
		TestNotNull(TEXT("Generated CDO still exists after reset"), CDO);
		if (CDO)
		{
			TestEqual(TEXT("InitialLifeSpan resets to parent CDO baseline"), CDO->InitialLifeSpan, ParentInitialLifeSpan);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintOmittedRegionsAreAuthoritativeTest,
	"UEAssetDoc.UBlueprint.OmittedRegionsAreAuthoritative",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintOmittedRegionsAreAuthoritativeTest::RunTest(const FString&)
{
	const FString Target = FString::Printf(TEXT("/Game/AssetDocumentTests/BP_AD_ParentGuard_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();

	FUBlueprintTestDocument CreateRequest(Target);
	CreateRequest.Document = MakeUBlueprintDocument(
		Target,
		TEXT("/Script/Engine.Actor"),
		MakeUBlueprintVariableArray({MakeUBlueprintFloatVariable(TEXT("Health"), TEXT("100.0"))}),
		TArray<TSharedPtr<FJsonValue>>{});
	TestTrue(TEXT("Initial actor parent apply succeeds"), CreateRequest.Apply(Service).IsSuccess());

	FUBlueprintTestDocument ParentOnlyRequest(CreateRequest);
	ParentOnlyRequest.Document = MakeUBlueprintDocument(Target, TEXT("/Script/Engine.Pawn"), TOptional<TArray<TSharedPtr<FJsonValue>>>(), TOptional<TArray<TSharedPtr<FJsonValue>>>());
	const FAssetDocumentResult ParentOnlyResult = ParentOnlyRequest.Apply(Service);
	TestTrue(TEXT("Parent-only apply succeeds with omitted authoritative regions"), ParentOnlyResult.IsSuccess());

	UBlueprint* Blueprint = LoadBlueprintForTarget(Target);
	TestNotNull(TEXT("Blueprint still exists"), Blueprint);
	if (Blueprint)
	{
		TestEqual(TEXT("Parent changes to Pawn"), Blueprint->ParentClass.Get(), APawn::StaticClass());
		// Python route (profile.ublueprint.v2): an omitted member region is preserved, a present
		// region is authoritative (the legacy C++ profile deleted omitted regions).
		TestTrue(TEXT("Omitted Variables region preserves Health"), HasUBlueprintVariable(Blueprint, TEXT("Health")));
		TestEqual(TEXT("Omitted ImplementedInterfaces region leaves no interfaces"), Blueprint->ImplementedInterfaces.Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintApplyFailureRollsBackTest,
	"UEAssetDoc.UBlueprint.ApplyFailureRollsBack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintApplyFailureRollsBackTest::RunTest(const FString&)
{
	const FString Target = FString::Printf(TEXT("/Game/AssetDocumentTests/BP_AD_Rollback_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();

	FUBlueprintTestDocument CreateRequest(Target);
	CreateRequest.Document = MakeUBlueprintDocument(
		Target,
		TEXT("/Script/Engine.Actor"),
		MakeUBlueprintVariableArray({MakeUBlueprintFloatVariable(TEXT("Health"), TEXT("100.0"))}),
		TArray<TSharedPtr<FJsonValue>>{});
	TestTrue(TEXT("Initial rollback fixture apply succeeds"), CreateRequest.Apply(Service).IsSuccess());

	TSharedPtr<FJsonObject> InvalidDefaultVariable = MakeUBlueprintFloatVariable(TEXT("RollbackFloat"), TEXT("not-a-float"));
	FUBlueprintTestDocument BadRequest(CreateRequest);
	BadRequest.Document = MakeUBlueprintDocument(
		Target,
		TEXT("/Script/Engine.Pawn"),
		MakeUBlueprintVariableArray({InvalidDefaultVariable}),
		TArray<TSharedPtr<FJsonValue>>{});
	const FAssetDocumentResult BadResult = BadRequest.Apply(Service);
	TestFalse(TEXT("Invalid default apply fails after mutation"), BadResult.IsSuccess());
	// Python route reports the editor primitive's code (legacy C++: InvalidVariableDefaultValue).
	if (!TestTrue(TEXT("Failure comes from default sync after mutation"), UBlueprintTestResultHasDiagnostic(BadResult, TEXT("BlueprintMember.InvalidDefault"))))
	{
		AddError(DescribeUBlueprintTestResult(BadResult));
	}

	UBlueprint* Blueprint = LoadBlueprintForTarget(Target);
	TestNotNull(TEXT("Blueprint still exists after rollback failure"), Blueprint);
	if (Blueprint)
	{
		TestEqual(TEXT("Parent rolled back to Actor"), Blueprint->ParentClass.Get(), AActor::StaticClass());
		TestTrue(TEXT("Health remains after rollback failure"), HasUBlueprintVariable(Blueprint, TEXT("Health")));
		TestFalse(TEXT("RollbackFloat was not added after rollback failure"), HasUBlueprintVariable(Blueprint, TEXT("RollbackFloat")));
		TestEqual(TEXT("Implemented interfaces remain empty after rollback failure"), Blueprint->ImplementedInterfaces.Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintVariableMetadataAuthoritativeTest,
	"UEAssetDoc.UBlueprint.VariableMetadataAuthoritative",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintVariableMetadataAuthoritativeTest::RunTest(const FString&)
{
	const FString Target = FString::Printf(TEXT("/Game/AssetDocumentTests/BP_AD_Metadata_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();

	FUBlueprintTestDocument FirstRequest(Target);
	FirstRequest.Document = MakeUBlueprintDocument(
		Target,
		TEXT("/Script/Engine.Actor"),
		MakeUBlueprintVariableArray({MakeUBlueprintFloatVariable(TEXT("Health"), TEXT("100.0"), TEXT("Stats"), TEXT("Hit points"))}),
		TArray<TSharedPtr<FJsonValue>>{});
	TestTrue(TEXT("Initial metadata apply succeeds"), FirstRequest.Apply(Service).IsSuccess());

	FUBlueprintTestDocument SecondRequest(FirstRequest);
	SecondRequest.Document = MakeUBlueprintDocument(
		Target,
		TEXT("/Script/Engine.Actor"),
		MakeUBlueprintVariableArray({MakeUBlueprintFloatVariable(TEXT("Health"), TEXT("100.0"))}),
		TArray<TSharedPtr<FJsonValue>>{});
	TestTrue(TEXT("Metadata-clearing apply succeeds"), SecondRequest.Apply(Service).IsSuccess());

	UBlueprint* Blueprint = LoadBlueprintForTarget(Target);
	TestNotNull(TEXT("Metadata Blueprint exists"), Blueprint);
	if (Blueprint)
	{
		const FBPVariableDescription* Health = Blueprint->NewVariables.FindByPredicate([](const FBPVariableDescription& Variable)
		{
			return Variable.VarName == TEXT("Health");
		});
		TestNotNull(TEXT("Health variable exists"), Health);
		if (Health)
		{
			// The editor's "cleared" category is the K2 default category.
			TestTrue(TEXT("Category is cleared"), Health->Category.IsEmpty() || Health->Category.EqualTo(UEdGraphSchema_K2::VR_DefaultCategory));
			TestFalse(TEXT("Tooltip metadata is cleared"), Health->HasMetaData(FBlueprintMetadata::MD_Tooltip));
		}

		FAssetDocumentDiffRequest DiffRequest;
		DiffRequest.Document = MakeUBlueprintDocument(
			Target,
			TEXT("/Script/Engine.Actor"),
			MakeUBlueprintVariableArray({MakeUBlueprintFloatVariable(TEXT("Health"), TEXT("100.0"))}),
			TArray<TSharedPtr<FJsonValue>>{});
		const FAssetDocumentResult DiffResult = Service.Diff(DiffRequest);
		TestTrue(TEXT("Metadata-cleared diff succeeds"), DiffResult.IsSuccess());
		const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
		if (DiffResult.Payload.IsValid() && DiffResult.Payload->TryGetArrayField(TEXT("changed"), Changed))
		{
			TestEqual(TEXT("Metadata-cleared variable diff is unchanged"), Changed->Num(), 0);
		}
		else
		{
			AddError(FString::Printf(TEXT("Diff payload has no changed array: %s"), *DescribeUBlueprintTestResult(DiffResult)));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintDiffExplicitRegionsTest,
	"UEAssetDoc.UBlueprint.DiffExplicitRegions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintDiffExplicitRegionsTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = NewObject<UBlueprint>(GetTransientPackage(), UBlueprint::StaticClass());
	Blueprint->ParentClass = AActor::StaticClass();
	FBPVariableDescription Health;
	Health.VarName = TEXT("Health");
	Health.VarGuid = FGuid::NewGuid();
	Health.VarType.PinCategory = UEdGraphSchema_K2::PC_Real;
	Health.VarType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
	Health.DefaultValue = TEXT("100.0");
	Blueprint->NewVariables.Add(Health);

	const FUBlueprintAssetDocumentCapability Capability;
	FAssetDocumentCapabilityContext Context;
	Context.Asset = Blueprint;
	Context.AssetClass = UBlueprint::StaticClass();

	TSharedRef<FJsonObject> ParentOnlyBody = MakeShared<FJsonObject>();
	ParentOnlyBody->SetObjectField(TEXT("ParentClass"), UBlueprintTestMakeActorParentClassRef());
	TArray<TSharedPtr<FJsonValue>> ParentOnlyDiffEntries;
	const FAssetDocumentCapabilityResult ParentOnlyDiffResult = Capability.Diff(Context, UBlueprintTestMakeBodyValue(ParentOnlyBody), ParentOnlyDiffEntries);
	TestTrue(TEXT("Parent-only diff succeeds"), ParentOnlyDiffResult.bSuccess);
	TSharedPtr<FJsonObject> HealthDiff = FindUBlueprintDiffEntryByPath(ParentOnlyDiffEntries, TEXT("/Body/Variables/Health"));
	TestTrue(TEXT("Omitted Variables region reports existing variable as extra"), HealthDiff.IsValid());
	if (HealthDiff.IsValid())
	{
		TestEqual(TEXT("Extra variable diff is changed"), HealthDiff->GetStringField(TEXT("status")), FString(TEXT("changed")));
		TestEqual(TEXT("Extra variable keeps extra change"), HealthDiff->GetStringField(TEXT("change")), FString(TEXT("extra")));
		TestTrue(TEXT("Extra variable diff has current"), HealthDiff->HasField(TEXT("current")));
		TestVariableDiffValue(this, HealthDiff, TEXT("current"), TEXT("Health"), TEXT("100.0"));
		TestUBlueprintDiffEntryFieldIsNull(this, HealthDiff, TEXT("desired"));
	}

	TSharedRef<FJsonObject> ChangedVariableBody = MakeShared<FJsonObject>();
	ChangedVariableBody->SetObjectField(TEXT("ParentClass"), UBlueprintTestMakeActorParentClassRef());
	ChangedVariableBody->SetArrayField(TEXT("Variables"), MakeUBlueprintVariableArray({MakeUBlueprintFloatVariable(TEXT("Health"), TEXT("125.0"))}));
	TArray<TSharedPtr<FJsonValue>> ChangedVariableDiffEntries;
	const FAssetDocumentCapabilityResult ChangedVariableDiffResult =
		Capability.Diff(Context, UBlueprintTestMakeBodyValue(ChangedVariableBody), ChangedVariableDiffEntries);
	TestTrue(TEXT("Matched changed variable diff succeeds"), ChangedVariableDiffResult.bSuccess);
	TSharedPtr<FJsonObject> ChangedHealthDiff = FindUBlueprintDiffEntryByPath(ChangedVariableDiffEntries, TEXT("/Body/Variables/Health"));
	TestTrue(TEXT("Changed variable uses semantic path"), ChangedHealthDiff.IsValid());
	if (ChangedHealthDiff.IsValid())
	{
		TestEqual(TEXT("Changed variable diff is changed"), ChangedHealthDiff->GetStringField(TEXT("status")), FString(TEXT("changed")));
		TestEqual(TEXT("Changed variable keeps changed change"), ChangedHealthDiff->GetStringField(TEXT("change")), FString(TEXT("changed")));
		TestTrue(TEXT("Changed variable diff has current"), ChangedHealthDiff->HasField(TEXT("current")));
		TestTrue(TEXT("Changed variable diff has desired"), ChangedHealthDiff->HasField(TEXT("desired")));
		TestVariableDiffValue(this, ChangedHealthDiff, TEXT("current"), TEXT("Health"), TEXT("100.0"));
		TestVariableDiffValue(this, ChangedHealthDiff, TEXT("desired"), TEXT("Health"), TEXT("125.0"));
	}

	TSharedRef<FJsonObject> MatchedVariableBody = MakeShared<FJsonObject>();
	MatchedVariableBody->SetObjectField(TEXT("ParentClass"), UBlueprintTestMakeActorParentClassRef());
	MatchedVariableBody->SetArrayField(TEXT("Variables"), MakeUBlueprintVariableArray({MakeUBlueprintFloatVariable(TEXT("Health"), TEXT("100.0"))}));
	TArray<TSharedPtr<FJsonValue>> MatchedVariableDiffEntries;
	const FAssetDocumentCapabilityResult MatchedVariableDiffResult =
		Capability.Diff(Context, UBlueprintTestMakeBodyValue(MatchedVariableBody), MatchedVariableDiffEntries);
	TestTrue(TEXT("Matched unchanged variable diff succeeds"), MatchedVariableDiffResult.bSuccess);
	TSharedPtr<FJsonObject> MatchedHealthDiff = FindUBlueprintDiffEntryByPath(MatchedVariableDiffEntries, TEXT("/Body/Variables/Health"));
	TestTrue(TEXT("Matched unchanged variable uses semantic path"), MatchedHealthDiff.IsValid());
	if (MatchedHealthDiff.IsValid())
	{
		TestTrue(TEXT("Matched unchanged variable has no change"), IsUnchangedDiffEntry(MatchedHealthDiff));
		TestVariableDiffValue(this, MatchedHealthDiff, TEXT("current"), TEXT("Health"), TEXT("100.0"));
		TestVariableDiffValue(this, MatchedHealthDiff, TEXT("desired"), TEXT("Health"), TEXT("100.0"));
	}

	TSharedRef<FJsonObject> CaseVariantVariableBody = MakeShared<FJsonObject>();
	CaseVariantVariableBody->SetObjectField(TEXT("ParentClass"), UBlueprintTestMakeActorParentClassRef());
	CaseVariantVariableBody->SetArrayField(TEXT("Variables"), MakeUBlueprintVariableArray({MakeUBlueprintFloatVariable(TEXT("health"), TEXT("100.0"))}));
	TArray<TSharedPtr<FJsonValue>> CaseVariantVariableDiffEntries;
	const FAssetDocumentCapabilityResult CaseVariantVariableDiffResult =
		Capability.Diff(Context, UBlueprintTestMakeBodyValue(CaseVariantVariableBody), CaseVariantVariableDiffEntries);
	TestTrue(TEXT("Case-variant variable diff succeeds"), CaseVariantVariableDiffResult.bSuccess);
	TSharedPtr<FJsonObject> CaseVariantHealthDiff = FindUBlueprintDiffEntryByPath(CaseVariantVariableDiffEntries, TEXT("/Body/Variables/Health"));
	TestTrue(TEXT("Case-variant variable keeps current semantic path"), CaseVariantHealthDiff.IsValid());
	if (CaseVariantHealthDiff.IsValid())
	{
		TestTrue(TEXT("Case-variant variable matches as unchanged"), IsUnchangedDiffEntry(CaseVariantHealthDiff));
		TestVariableDiffValue(this, CaseVariantHealthDiff, TEXT("current"), TEXT("Health"), TEXT("100.0"));
		TestVariableDiffValue(this, CaseVariantHealthDiff, TEXT("desired"), TEXT("health"), TEXT("100.0"));
	}
	TestFalse(
		TEXT("Case-variant variable does not emit desired-only lowercase path"),
		FindUBlueprintDiffEntryByExactPath(CaseVariantVariableDiffEntries, TEXT("/Body/Variables/health")).IsValid());

	TSharedRef<FJsonObject> DesiredVariableBody = MakeShared<FJsonObject>();
	DesiredVariableBody->SetObjectField(TEXT("ParentClass"), UBlueprintTestMakeActorParentClassRef());
	DesiredVariableBody->SetArrayField(TEXT("Variables"), MakeUBlueprintVariableArray({MakeUBlueprintFloatVariable(TEXT("NewScore"), TEXT("7.0"))}));
	TArray<TSharedPtr<FJsonValue>> DesiredVariableDiffEntries;
	const FAssetDocumentCapabilityResult DesiredVariableDiffResult =
		Capability.Diff(Context, UBlueprintTestMakeBodyValue(DesiredVariableBody), DesiredVariableDiffEntries);
	TestTrue(TEXT("Desired-only variable diff succeeds"), DesiredVariableDiffResult.bSuccess);
	TSharedPtr<FJsonObject> NewVariableDiff = FindUBlueprintDiffEntryByPath(DesiredVariableDiffEntries, TEXT("/Body/Variables/NewScore"));
	TestTrue(TEXT("Desired-only variable uses semantic path"), NewVariableDiff.IsValid());
	if (NewVariableDiff.IsValid())
	{
		TestEqual(TEXT("Desired-only variable is changed"), NewVariableDiff->GetStringField(TEXT("status")), FString(TEXT("changed")));
		TestEqual(TEXT("Desired-only variable keeps missing change"), NewVariableDiff->GetStringField(TEXT("change")), FString(TEXT("missing")));
		TestUBlueprintDiffEntryFieldIsNull(this, NewVariableDiff, TEXT("current"));
		TestTrue(TEXT("Desired-only variable diff has desired"), NewVariableDiff->HasField(TEXT("desired")));
		TestVariableDiffValue(this, NewVariableDiff, TEXT("desired"), TEXT("NewScore"), TEXT("7.0"));
	}

	const FString InterfacePath = TEXT("/Script/Engine.ActorSoundParameterInterface");
	TSharedPtr<FJsonObject> InterfaceEntry = MakeShared<FJsonObject>();
	InterfaceEntry->SetObjectField(TEXT("Interface"), MakeUBlueprintClassRefObject(InterfacePath));
	TArray<TSharedPtr<FJsonValue>> DesiredInterfaces;
	DesiredInterfaces.Add(MakeShared<FJsonValueObject>(InterfaceEntry));

	TSharedRef<FJsonObject> InterfaceBody = MakeShared<FJsonObject>();
	InterfaceBody->SetObjectField(TEXT("ParentClass"), UBlueprintTestMakeActorParentClassRef());
	InterfaceBody->SetArrayField(TEXT("ImplementedInterfaces"), DesiredInterfaces);
	TArray<TSharedPtr<FJsonValue>> InterfaceDiffEntries;
	const FAssetDocumentCapabilityResult InterfaceDiffResult = Capability.Diff(Context, UBlueprintTestMakeBodyValue(InterfaceBody), InterfaceDiffEntries);
	TestTrue(TEXT("Interface diff succeeds"), InterfaceDiffResult.bSuccess);
	TSharedPtr<FJsonObject> InterfaceDiff = FindUBlueprintDiffEntryByPath(InterfaceDiffEntries, TEXT("/Body/ImplementedInterfaces//Script/Engine.ActorSoundParameterInterface"));
	TestTrue(TEXT("Missing desired interface is reported"), InterfaceDiff.IsValid());
	if (InterfaceDiff.IsValid())
	{
		TestEqual(TEXT("Missing interface diff is changed"), InterfaceDiff->GetStringField(TEXT("status")), FString(TEXT("changed")));
		TestEqual(TEXT("Missing interface diff keeps missing change"), InterfaceDiff->GetStringField(TEXT("change")), FString(TEXT("missing")));
		TestUBlueprintDiffEntryFieldIsNull(this, InterfaceDiff, TEXT("current"));
		TestUBlueprintInterfaceDiffValue(this, InterfaceDiff, TEXT("desired"), InterfacePath);
	}

	UClass* InterfaceClass = LoadObject<UClass>(nullptr, *InterfacePath);
	TestNotNull(TEXT("Interface class loads for matched diff"), InterfaceClass);
	if (InterfaceClass)
	{
		FBPInterfaceDescription CurrentInterface;
		CurrentInterface.Interface = InterfaceClass;
		Blueprint->ImplementedInterfaces.Add(CurrentInterface);

		TArray<TSharedPtr<FJsonValue>> MatchedInterfaceDiffEntries;
		const FAssetDocumentCapabilityResult MatchedInterfaceDiffResult =
			Capability.Diff(Context, UBlueprintTestMakeBodyValue(InterfaceBody), MatchedInterfaceDiffEntries);
		TestTrue(TEXT("Matched interface diff succeeds"), MatchedInterfaceDiffResult.bSuccess);
		TSharedPtr<FJsonObject> MatchedInterfaceDiff =
			FindUBlueprintDiffEntryByPath(MatchedInterfaceDiffEntries, FString::Printf(TEXT("/Body/ImplementedInterfaces/%s"), *InterfacePath));
		TestTrue(TEXT("Matched interface is reported as unchanged"), MatchedInterfaceDiff.IsValid());
		if (MatchedInterfaceDiff.IsValid())
		{
			TestTrue(TEXT("Matched interface diff is unchanged without change"), IsUnchangedDiffEntry(MatchedInterfaceDiff));
			TestUBlueprintInterfaceDiffValue(this, MatchedInterfaceDiff, TEXT("current"), InterfacePath);
			TestUBlueprintInterfaceDiffValue(this, MatchedInterfaceDiff, TEXT("desired"), InterfacePath);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUBlueprintExtractSkipsUnsupportedPinTypesTest,
	"UEAssetDoc.UBlueprint.ExtractSkipsUnsupportedPinTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintExtractSkipsUnsupportedPinTypesTest::RunTest(const FString&)
{
	UBlueprint* Blueprint = UBlueprintTestCreateTransientActorBlueprint(TEXT("BP_ExtractSkippedEvidence"));
	TestNotNull(TEXT("Transient actor Blueprint is created"), Blueprint);
	UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	TestNotNull(TEXT("Transient actor Blueprint has an EventGraph"), Graph);
	if (!Blueprint || !Graph)
	{
		return false;
	}

	FBPVariableDescription Scores;
	Scores.VarName = TEXT("Scores");
	Scores.VarGuid = FGuid::NewGuid();
	Scores.VarType.PinCategory = UEdGraphSchema_K2::PC_Int;
	Scores.VarType.ContainerType = EPinContainerType::Array;
	Blueprint->NewVariables.Add(Scores);
	UBlueprintTestAddUnsupportedBranchNode(Graph, 320, 0);

	const FUBlueprintAssetDocumentCapability Capability;
	FAssetDocumentCapabilityContext Context;
	Context.Asset = Blueprint;
	Context.AssetClass = UBlueprint::StaticClass();

	TSharedRef<FJsonObject> ExtractedBody = MakeShared<FJsonObject>();
	const FAssetDocumentCapabilityResult ExtractResult = Capability.Extract(Context, ExtractedBody);
	TestTrue(TEXT("Extract succeeds with unsupported variable pin type"), ExtractResult.bSuccess);

	const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
	TestTrue(TEXT("Extract writes Variables array"), ExtractedBody->TryGetArrayField(TEXT("Variables"), Variables));
	TestEqual(TEXT("Unsupported variable is not emitted as simplified Type"), Variables ? Variables->Num() : -1, 0);

	const TSharedPtr<FJsonObject>* Skipped = nullptr;
	TestTrue(TEXT("Extract records skipped diagnostics"), ExtractedBody->TryGetObjectField(TEXT("_Skipped"), Skipped));
	if (Skipped && Skipped->IsValid())
	{
		TestTrue(TEXT("Skipped diagnostics include Variables"), (*Skipped)->HasField(TEXT("Variables")));
		TestTrue(TEXT("Skipped diagnostics include Graphs"), (*Skipped)->HasField(TEXT("Graphs")));
	}
	return true;
}

#endif
