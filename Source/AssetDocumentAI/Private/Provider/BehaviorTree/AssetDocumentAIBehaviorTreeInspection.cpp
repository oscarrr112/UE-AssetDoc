// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreePrimitives.h"

#include "Provider/AssetDocumentAIProviderContracts.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredReadbackInternal.h"

#include "BehaviorTree/BehaviorTree.h"
#include "AIGraphNode.h"
#include "BehaviorTreeGraph.h"
#include "Editor.h"
#include "IDetailTreeNode.h"
#include "IPropertyRowGenerator.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

#include <initializer_list>

namespace AIBehaviorTreeInspectionLocal
{
FAssetDocumentPrimitiveOutcome Failure(const TCHAR* Code, const TCHAR* Message)
{
	return MakeAssetDocumentAIPrimitiveFailure(
		EAssetDocumentPrimitiveOutcomeStatus::Failed, Code, Message);
}

FAssetDocumentRuntimeValueDto StringValue(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto ObjectValue(
	TArray<FString> Keys,
	TArray<FAssetDocumentRuntimeValueDto> Values)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Object;
	Result.ObjectKeys = MoveTemp(Keys);
	Result.Elements = MoveTemp(Values);
	return Result;
}

FAssetDocumentRuntimeValueDto ArrayValue(TArray<FAssetDocumentRuntimeValueDto> Values)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Array;
	Result.Elements = MoveTemp(Values);
	return Result;
}

const FAssetDocumentRuntimeValueDto* FindField(
	const FAssetDocumentRuntimeValueDto& Object,
	const TCHAR* Name)
{
	if (Object.Kind != EAssetDocumentRuntimeValueKind::Object
		|| Object.ObjectKeys.Num() != Object.Elements.Num())
	{
		return nullptr;
	}
	for (int32 Index = 0; Index < Object.ObjectKeys.Num(); ++Index)
	{
		if (Object.ObjectKeys[Index] == Name)
		{
			return &Object.Elements[Index];
		}
	}
	return nullptr;
}

bool HasExactFields(
	const FAssetDocumentRuntimeValueDto& Object,
	std::initializer_list<const TCHAR*> Names)
{
	if (Object.Kind != EAssetDocumentRuntimeValueKind::Object
		|| Object.ObjectKeys.Num() != Object.Elements.Num()
		|| Object.ObjectKeys.Num() != static_cast<int32>(Names.size()))
	{
		return false;
	}
	TSet<FString> Seen;
	for (const FString& Key : Object.ObjectKeys)
	{
		if (Key.IsEmpty() || Seen.Contains(Key))
		{
			return false;
		}
		Seen.Add(Key);
	}
	for (const TCHAR* Name : Names)
	{
		if (FindField(Object, Name) == nullptr)
		{
			return false;
		}
	}
	return true;
}

bool IsCanonicalObjectPath(const FAssetDocumentRuntimeValueDto* Value)
{
	if (Value == nullptr || Value->Kind != EAssetDocumentRuntimeValueKind::String
		|| Value->StringValue.IsEmpty())
	{
		return false;
	}
	const FSoftObjectPath Path(Value->StringValue);
	return Path.IsValid() && Path.GetSubPathString().IsEmpty()
		&& Path.ToString() == Value->StringValue;
}

bool RuntimeValueEquals(
	const FAssetDocumentRuntimeValueDto& Left,
	const FAssetDocumentRuntimeValueDto& Right)
{
	if (Left.Kind != Right.Kind || Left.BooleanValue != Right.BooleanValue
		|| Left.IntegerValue != Right.IntegerValue
		|| Left.NumberValue != Right.NumberValue
		|| Left.StringValue != Right.StringValue
		|| Left.BytesValue != Right.BytesValue
		|| Left.ObjectKeys != Right.ObjectKeys
		|| Left.Elements.Num() != Right.Elements.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < Left.Elements.Num(); ++Index)
	{
		if (!RuntimeValueEquals(Left.Elements[Index], Right.Elements[Index]))
		{
			return false;
		}
	}
	return true;
}

bool IsPropertyContext(const UObject* Object)
{
	if (!IsInGameThread() || !GIsEditor || IsRunningCommandlet()
		|| IsGarbageCollecting() || Object == nullptr
		|| Object->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		return false;
	}
	const UPackage* Package = Object->GetOutermost();
	return Package != nullptr
		&& !Package->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed);
}

bool MakeRows(UObject& Object, TSharedPtr<IPropertyRowGenerator>& OutRows)
{
	if (!IsPropertyContext(&Object))
	{
		return false;
	}
	FPropertyEditorModule* PropertyEditor =
		FModuleManager::LoadModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor"));
	if (PropertyEditor == nullptr)
	{
		return false;
	}
	FPropertyRowGeneratorArgs Args;
	Args.bAllowEditingClassDefaultObjects = true;
	OutRows = PropertyEditor->CreatePropertyRowGenerator(Args);
	OutRows->SetObjects({&Object});
	return OutRows.IsValid();
}

bool IsAdmittedLeaf(const IPropertyHandle& Handle)
{
	const FProperty* Property = Handle.GetProperty();
	return Property != nullptr && Property->GetOwnerProperty() == Property
		&& Property->HasAnyPropertyFlags(CPF_Edit)
		&& !Property->HasAnyPropertyFlags(
			CPF_Transient | CPF_EditConst | CPF_InstancedReference)
		&& Handle.IsValidHandle() && Handle.IsEditable()
		&& CastField<FArrayProperty>(Property) == nullptr
		&& CastField<FSetProperty>(Property) == nullptr
		&& CastField<FMapProperty>(Property) == nullptr;
}

bool ReadLeaf(const IPropertyHandle& Handle, FAssetDocumentRuntimeValueDto& OutValue,
	FString* OutKind = nullptr)
{
	const FProperty* Property = Handle.GetProperty();
	if (!UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::ReadScalarProperty(
		Handle, OutValue)) return false;
	if (OutKind == nullptr) return true;
	*OutKind = CastField<FNameProperty>(Property) != nullptr ? TEXT("name")
		: CastField<FStrProperty>(Property) != nullptr ? TEXT("string")
		: CastField<FBoolProperty>(Property) != nullptr ? TEXT("boolean")
		: CastField<FFloatProperty>(Property) != nullptr || CastField<FDoubleProperty>(Property) != nullptr
			? TEXT("number") : TEXT("integer");
	return true;
}

void GatherLeaves(
	const TArray<TSharedRef<IDetailTreeNode>>& Nodes,
	UObject& Owner,
	TMap<FString, TSharedRef<IPropertyHandle>>& OutHandles)
{
	for (const TSharedRef<IDetailTreeNode>& Node : Nodes)
	{
		const TSharedPtr<IPropertyHandle> Handle = Node->CreatePropertyHandle();
		TArray<UObject*> Outers;
		if (Handle.IsValid()) Handle->GetOuterObjects(Outers);
		if (Handle.IsValid() && IsAdmittedLeaf(*Handle) && Outers.Num() == 1
			&& Outers[0] == &Owner)
		{
			OutHandles.Add(Handle->GeneratePathToProperty(), Handle.ToSharedRef());
		}
		TArray<TSharedRef<IDetailTreeNode>> Children;
		Node->GetChildren(Children, true);
		GatherLeaves(Children, Owner, OutHandles);
	}
}

bool MakeCanonicalProperties(
	UObject& Object,
	FAssetDocumentRuntimeValueDto& OutProperties,
	FAssetDocumentRuntimeValueDto* OutSchema = nullptr)
{
	TSharedPtr<IPropertyRowGenerator> Rows;
	if (!MakeRows(Object, Rows)) return false;
	TMap<FString, TSharedRef<IPropertyHandle>> Handles;
	GatherLeaves(Rows->GetRootTreeNodes(), Object, Handles);
	TArray<FString> Names;
	Handles.GetKeys(Names);
	Names.Sort();
	TArray<FAssetDocumentRuntimeValueDto> Values;
	TArray<FAssetDocumentRuntimeValueDto> Schema;
	for (const FString& Name : Names)
	{
		FAssetDocumentRuntimeValueDto Value;
		FString Kind;
		if (!ReadLeaf(*Handles.FindChecked(Name), Value, &Kind)) return false;
		Values.Add(MoveTemp(Value));
		Schema.Add(ObjectValue({TEXT("kind"), TEXT("name")},
			{StringValue(MoveTemp(Kind)), StringValue(Name)}));
	}
	OutProperties = ObjectValue(MoveTemp(Names), MoveTemp(Values));
	if (OutSchema != nullptr) *OutSchema = ArrayValue(MoveTemp(Schema));
	return true;
}

UBehaviorTree* ResolveBehaviorTree(const FString& Path)
{
	UBehaviorTree* Tree = Cast<UBehaviorTree>(FSoftObjectPath(Path).ResolveObject());
	return Tree != nullptr && Tree->GetPathName() == Path ? Tree : nullptr;
}

UEdGraphNode* FindExactNode(UBehaviorTreeGraph& Graph, const FString& StableId)
{
	FGuid NodeGuid;
	if (!FGuid::ParseExact(StableId, EGuidFormats::DigitsWithHyphensLower, NodeGuid))
	{
		return nullptr;
	}
	for (UEdGraphNode* Node : Graph.Nodes)
	{
		if (Node != nullptr && Node->NodeGuid == NodeGuid) return Node;
	}
	return nullptr;
}
} // namespace

namespace AIBehaviorTreeInspectionLocal
{
struct FNodeClassInspection
{
	UClass* NodeClass = nullptr;
	UClass* ExpectedBase = nullptr;
};

bool ResolveNodeClassInspection(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	FNodeClassInspection& OutInspection,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	const FAssetDocumentRuntimeValueDto& Value = Invocation.ValueArguments;
	if (!IsAssetDocumentAICanonicalRuntimeValue(Value)
		|| !HasExactFields(Value, {TEXT("expected_base_class_path"), TEXT("node_class_path")}))
	{
		OutFailure = Failure(TEXT("AIProvider.InvalidValueShape"), TEXT("The node-class inspection request is not canonical."));
		return false;
	}
	const FAssetDocumentRuntimeValueDto* NodePath = FindField(Value, TEXT("node_class_path"));
	const FAssetDocumentRuntimeValueDto* BasePath = FindField(Value, TEXT("expected_base_class_path"));
	if (!IsCanonicalObjectPath(NodePath) || !IsCanonicalObjectPath(BasePath))
	{
		OutFailure = Failure(TEXT("AIProvider.InvalidValueShape"), TEXT("The node-class paths must be canonical object paths."));
		return false;
	}
	OutInspection.NodeClass = FSoftClassPath(NodePath->StringValue).ResolveClass();
	OutInspection.ExpectedBase = FSoftClassPath(BasePath->StringValue).ResolveClass();
	if (OutInspection.NodeClass == nullptr || OutInspection.ExpectedBase == nullptr
		|| OutInspection.NodeClass->GetPathName() != NodePath->StringValue)
	{
		OutFailure = Failure(TEXT("AIProvider.BehaviorTree.NodeClassUnavailable"), TEXT("The requested BehaviorTree node class is not loaded."));
		return false;
	}
	if (!OutInspection.NodeClass->IsChildOf(OutInspection.ExpectedBase)
		|| OutInspection.NodeClass->HasAnyClassFlags(CLASS_Abstract))
	{
		OutFailure = Failure(TEXT("AIProvider.BehaviorTree.NodeClassRejected"), TEXT("The requested class is not an instantiable member of the expected base class."));
		return false;
	}
	return true;
}

bool BuildNodeClassEvidence(
	const FNodeClassInspection& Inspection,
	FAssetDocumentRuntimeValueDto& OutProperties,
	FAssetDocumentRuntimeValueDto& OutSchema,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	UObject* DefaultObject = Inspection.NodeClass->GetDefaultObject();
	if (DefaultObject == nullptr || !MakeCanonicalProperties(
		*DefaultObject, OutProperties, &OutSchema))
	{
		OutFailure = Failure(TEXT("AIProvider.BehaviorTree.PropertyEvidenceUnavailable"), TEXT("The PropertyEditor could not produce read-only node property evidence."));
		return false;
	}
	return true;
}

struct FInstanceInspection
{
	UObject* NodeInstance = nullptr;
	const FAssetDocumentRuntimeValueDto* ExpectedProperties = nullptr;
	FString StableNodeId;
};

bool ResolveInstanceInspection(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	FInstanceInspection& OutInspection,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	const FAssetDocumentRuntimeValueDto& Value = Invocation.ValueArguments;
	if (!IsAssetDocumentAICanonicalRuntimeValue(Value)
		|| !HasExactFields(Value, {TEXT("behavior_tree_path"), TEXT("canonical_properties"),
			TEXT("expected_class_path"), TEXT("stable_node_id")}))
	{
		OutFailure = Failure(TEXT("AIProvider.InvalidValueShape"), TEXT("The node-property validation request is not canonical."));
		return false;
	}
	const FAssetDocumentRuntimeValueDto* TreePath = FindField(Value, TEXT("behavior_tree_path"));
	const FAssetDocumentRuntimeValueDto* NodeId = FindField(Value, TEXT("stable_node_id"));
	const FAssetDocumentRuntimeValueDto* ClassPath = FindField(Value, TEXT("expected_class_path"));
	OutInspection.ExpectedProperties = FindField(Value, TEXT("canonical_properties"));
	if (!IsCanonicalObjectPath(TreePath) || !IsCanonicalObjectPath(ClassPath)
		|| NodeId == nullptr || NodeId->Kind != EAssetDocumentRuntimeValueKind::String
		|| OutInspection.ExpectedProperties == nullptr
		|| OutInspection.ExpectedProperties->Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		OutFailure = Failure(TEXT("AIProvider.InvalidValueShape"), TEXT("The node-property validation fields are invalid."));
		return false;
	}
	UBehaviorTree* Tree = ResolveBehaviorTree(TreePath->StringValue);
	UBehaviorTreeGraph* Graph = Tree != nullptr ? Cast<UBehaviorTreeGraph>(Tree->BTGraph) : nullptr;
	UEdGraphNode* Node = Graph != nullptr ? FindExactNode(*Graph, NodeId->StringValue) : nullptr;
	UAIGraphNode* AINode = Cast<UAIGraphNode>(Node);
	OutInspection.NodeInstance = AINode != nullptr ? AINode->NodeInstance.Get() : nullptr;
	if (OutInspection.NodeInstance == nullptr)
	{
		OutFailure = Failure(TEXT("AIProvider.BehaviorTree.NodeMissing"), TEXT("The requested existing BehaviorTree graph node is unavailable."));
		return false;
	}
	if (OutInspection.NodeInstance->GetClass()->GetPathName() != ClassPath->StringValue)
	{
		OutFailure = Failure(TEXT("AIProvider.BehaviorTree.NodeClassMismatch"), TEXT("The existing graph node class does not match the request."));
		return false;
	}
	OutInspection.StableNodeId = NodeId->StringValue;
	return true;
}

bool ValidateInstanceProperties(
	const FInstanceInspection& Inspection,
	FAssetDocumentRuntimeValueDto& OutProperties,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	if (!MakeCanonicalProperties(*Inspection.NodeInstance, OutProperties))
	{
		OutFailure = Failure(TEXT("AIProvider.BehaviorTree.PropertyNotAdmitted"), TEXT("The requested existing node properties are not read-only admitted."));
		return false;
	}
	if (!RuntimeValueEquals(OutProperties, *Inspection.ExpectedProperties))
	{
		OutFailure = Failure(TEXT("AIProvider.BehaviorTree.PropertyNotAdmitted"), TEXT("The requested property set does not match the admitted existing node surface."));
		return false;
	}
	return true;
}
} // namespace

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBehaviorTreeNodeClassInspect(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AIBehaviorTreeInspectionLocal;
	FNodeClassInspection Inspection;
	FAssetDocumentPrimitiveOutcome Outcome;
	if (!ResolveNodeClassInspection(Invocation, Inspection, Outcome)) return Outcome;
	FAssetDocumentRuntimeValueDto Properties;
	FAssetDocumentRuntimeValueDto PropertySchema;
	if (!BuildNodeClassEvidence(Inspection, Properties, PropertySchema, Outcome)) return Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value = ObjectValue(
		{TEXT("canonical_properties"), TEXT("node_class_path"), TEXT("node_kind"), TEXT("property_schema")},
		{MoveTemp(Properties), StringValue(Inspection.NodeClass->GetPathName()),
			StringValue(Inspection.ExpectedBase->GetPathName()), MoveTemp(PropertySchema)});
	return Outcome;
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBehaviorTreeInstanceValidateProperties(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AIBehaviorTreeInspectionLocal;
	FInstanceInspection Inspection;
	FAssetDocumentPrimitiveOutcome Outcome;
	if (!ResolveInstanceInspection(Invocation, Inspection, Outcome)) return Outcome;
	FAssetDocumentRuntimeValueDto Properties;
	if (!ValidateInstanceProperties(Inspection, Properties, Outcome)) return Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value = ObjectValue(
		{TEXT("canonical_properties"), TEXT("stable_node_id")},
		{MoveTemp(Properties), StringValue(Inspection.StableNodeId)});
	return Outcome;
}
