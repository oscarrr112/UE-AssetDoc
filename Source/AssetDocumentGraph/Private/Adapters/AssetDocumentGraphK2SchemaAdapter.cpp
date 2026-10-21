// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// K2 (Blueprint) schema adapter: action keys over FBlueprintActionDatabase,
// spawn through UBlueprintNodeSpawner::Invoke, node description, user pins,
// dynamic pins and function signature flags.
// Contract: docs/architecture/asset-document-graph-primitives.md §5-§6.

#include "Adapters/AssetDocumentGraphBuiltinAdapters.h"
#include "AssetDocumentGraphEditorConfirmations.h"
#include "Adapters/AssetDocumentGraphSortedKeys.h"

#include "AssetDocumentGraphModel.h"
#include "AssetDocumentGraphRawEdits.h"
#include "AssetDocumentGraphValue.h"
#include "BlueprintActionDatabase.h"
#include "BlueprintActionFilter.h"
#include "BlueprintEditorSettings.h"
#include "BlueprintBoundEventNodeSpawner.h"
#include "BlueprintBoundNodeSpawner.h"
#include "BlueprintComponentNodeSpawner.h"
#include "BlueprintDelegateNodeSpawner.h"
#include "BlueprintEventNodeSpawner.h"
#include "BlueprintFieldNodeSpawner.h"
#include "BlueprintFunctionNodeSpawner.h"
#include "BlueprintNodeSpawner.h"
#include "BlueprintVariableNodeSpawner.h"
#include "Components/ActorComponent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_AddComponent.h"
#include "K2Node_AddPinInterface.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_CommutativeAssociativeBinaryOperator.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_Composite.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_ClassDynamicCast.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_EditablePinBase.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_InputAction.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_PromotableOperator.h"
#include "K2Node_Select.h"
#include "K2Node_SetFieldsInStruct.h"
#include "K2Node_StructOperation.h"
#include "NodeDependingOnEnumInterface.h"
#include "BlueprintTypePromotion.h"
#include "UObject/SoftObjectPath.h"
#include "K2Node_Tunnel.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/WildcardNodeUtils.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "EdGraphSchema_K2_Actions.h"
#include "GraphEditorSettings.h"
#include "AssetDocumentGraphPropertyCodec.h"
#include "Settings/BlueprintEditorProjectSettings.h"

namespace UE::AssetDocument::Graph
{
namespace K2Keys
{
FString OwnerPath(const UObject* Object)
{
	if (Object == nullptr)
	{
		return FString();
	}
	if (const UClass* Class = Cast<UClass>(Object))
	{
		const UClass* Authoritative = const_cast<UClass*>(Class)->GetAuthoritativeClass();
		return (Authoritative != nullptr ? Authoritative : Class)->GetPathName();
	}
	return Object->GetPathName();
}

FString KindOf(const UClass* NodeClass)
{
	FString Kind = NodeClass != nullptr ? NodeClass->GetName() : FString();
	Kind.RemoveFromStart(TEXT("K2Node_"));
	return Kind;
}

namespace
{
FString JoinKey(const FString& Kind, const FString& Owner, const FString& Field)
{
	FString Key = TEXT("K2:") + Kind;
	if (!Owner.IsEmpty())
	{
		Key += TEXT(":") + Owner;
	}
	if (!Field.IsEmpty())
	{
		Key += TEXT(":") + Field;
	}
	return Key;
}

FString FieldOwnerPath(const FField* Field)
{
	return Field != nullptr ? OwnerPath(Field->GetOwnerStruct()) : FString();
}

FString FunctionOwnerPath(const UFunction* Function)
{
	if (Function == nullptr)
	{
		return FString();
	}
	const UClass* OwnerClass = Function->GetOwnerClass();
	return OwnerClass != nullptr ? OwnerPath(OwnerClass) : OwnerPath(Function->GetOuter());
}

FString LocalScopeOwner(const UK2Node_Variable& Node)
{
	UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForNode(&Node);
	const UObject* Generated = Blueprint != nullptr && Blueprint->GeneratedClass != nullptr
		? static_cast<const UObject*>(Blueprint->GeneratedClass.Get()) : static_cast<const UObject*>(Blueprint);
	return OwnerPath(Generated) + TEXT(":") + Node.VariableReference.GetMemberScopeName();
}

FString MacroOwner(const UK2Node_MacroInstance& Node, FString& OutField)
{
	UEdGraph* MacroGraph = Node.GetMacroGraph();
	if (MacroGraph == nullptr)
	{
		return FString();
	}
	OutField = MacroGraph->GetName();
	return OwnerPath(FBlueprintEditorUtils::FindBlueprintForGraph(MacroGraph));
}
// The macro graph of a MacroInstance spawner. The database binds it into the
// spawner's CustomizeNodeDelegate (FBlueprintNodeSpawnerFactory::MakeMacroNodeSpawner);
// applying the delegate to a scratch node reads it back without the template
// cache (which may refuse templates). Cached per spawner.
const UEdGraph* MacroGraphOf(const UBlueprintNodeSpawner& Spawner)
{
	static TMap<TWeakObjectPtr<const UBlueprintNodeSpawner>, TWeakObjectPtr<UEdGraph>> Cache;
	if (const TWeakObjectPtr<UEdGraph>* Cached = Cache.Find(&Spawner))
	{
		if (Cached->IsValid())
		{
			return Cached->Get();
		}
	}
	if (!Spawner.CustomizeNodeDelegate.IsBound())
	{
		return nullptr;
	}
	UK2Node_MacroInstance* Scratch = NewObject<UK2Node_MacroInstance>(GetTransientPackage(), NAME_None, RF_Transient);
	Spawner.CustomizeNodeDelegate.Execute(Scratch, /*bIsTemplateNode=*/true);
	UEdGraph* MacroGraph = Scratch->GetMacroGraph();
	Scratch->MarkAsGarbage();
	if (Cache.Num() > 4096)
	{
		Cache.Reset();
	}
	Cache.Add(&Spawner, MacroGraph);
	return MacroGraph;
}

// The struct/enum a field-subject node operates on (Make/Break/SetFields struct,
// enum literal/switch/cast/loop/count): the field the node's spawner reports.
const UObject* NodeFieldSubject(const UEdGraphNode& Node)
{
	if (const UK2Node_StructOperation* Struct = Cast<UK2Node_StructOperation>(&Node))
	{
		return Struct->StructType;
	}
	if (const INodeDependingOnEnumInterface* EnumNode = Cast<INodeDependingOnEnumInterface>(const_cast<UEdGraphNode*>(&Node)))
	{
		return EnumNode->GetEnum();
	}
	return nullptr;
}

bool HasFieldSpawner(const UClass* NodeClass, const UObject* Subject)
{
	for (const UObject* Owner : {static_cast<const UObject*>(NodeClass), Subject})
	{
		const FBlueprintActionDatabase::FActionList* Actions = FBlueprintActionDatabase::Get().GetAllActions().Find(FObjectKey(Owner));
		if (Actions == nullptr)
		{
			continue;
		}
		for (const UBlueprintNodeSpawner* Spawner : *Actions)
		{
			const UBlueprintFieldNodeSpawner* Field = Cast<UBlueprintFieldNodeSpawner>(Spawner);
			if (Field != nullptr && Field->NodeClass == NodeClass && Field->GetField().ToUObject() == Subject)
			{
				return true;
			}
		}
	}
	return false;
}

// Properties a node class adds below UK2Node: what a spawner's CustomizeNodeDelegate may set.
bool IsCustomizableProperty(const FProperty& Property)
{
	const UClass* Owner = Property.GetOwnerClass();
	return Owner != nullptr && Owner->IsChildOf(UK2Node::StaticClass()) && Owner != UK2Node::StaticClass()
		&& !Property.HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated);
}

FString IdentityValueText(const FProperty& Property, const void* Container)
{
	if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(&Property))
	{
		return OwnerPath(Object->GetObjectPropertyValue_InContainer(Container));
	}
	FString Text;
	Property.ExportTextItem_InContainer(Text, Container, nullptr, nullptr, PPF_None);
	return Text;
}

struct FCustomizedValues
{
	TArray<const FProperty*> Properties;
	TArray<FString> Values;
};

// The values a plain spawner's CustomizeNodeDelegate gives its template node, where they differ from the
// class defaults. Cached per spawner.
const FCustomizedValues& CustomizedValuesOf(const UBlueprintNodeSpawner& Spawner)
{
	static TMap<TWeakObjectPtr<const UBlueprintNodeSpawner>, FCustomizedValues> Cache;
	if (const FCustomizedValues* Cached = Cache.Find(&Spawner))
	{
		return *Cached;
	}
	if (Cache.Num() > 4096)
	{
		Cache.Reset();
	}
	FCustomizedValues& Out = Cache.Add(&Spawner);
	UClass* NodeClass = Spawner.NodeClass.Get();
	if (NodeClass == nullptr || !Spawner.CustomizeNodeDelegate.IsBound() || NodeClass->HasAnyClassFlags(CLASS_Abstract))
	{
		return Out;
	}
	// The editor's own template node (spawned into its template graph, customized as a template).
	const UEdGraphNode* Scratch = Spawner.GetTemplateNode();
	if (Scratch == nullptr)
	{
		return Out;
	}
	const UObject* Defaults = NodeClass->GetDefaultObject();
	for (TFieldIterator<FProperty> It(NodeClass); It; ++It)
	{
		if (IsCustomizableProperty(**It) && !It->Identical_InContainer(Scratch, Defaults, 0, PPF_None))
		{
			Out.Properties.Add(*It);
			Out.Values.Add(IdentityValueText(**It, Scratch));
		}
	}
	return Out;
}

// Nodes the database files under an object the node names (an Enhanced Input action event under
// its UInputAction asset): keyed by that object, as ComputeSpawnerBaseKey keys their spawners.
FString SubjectObjectKey(const UEdGraphNode& Node, const FString& Kind)
{
	const UClass* NodeClass = Node.GetClass();
	for (TFieldIterator<FObjectPropertyBase> It(NodeClass); It; ++It)
	{
		const UObject* Subject = It->GetObjectPropertyValue_InContainer(&Node);
		if (Subject == nullptr || Subject->IsA<UEdGraph>() || Subject->IsA<UEdGraphNode>())
		{
			continue;
		}
		const FBlueprintActionDatabase::FActionList* Actions = FBlueprintActionDatabase::Get().GetAllActions().Find(FObjectKey(Subject));
		if (Actions != nullptr && Actions->ContainsByPredicate([NodeClass](const UBlueprintNodeSpawner* Spawner)
			{ return Spawner != nullptr && Spawner->NodeClass == NodeClass; }))
		{
			const UBlueprint* SubjectBlueprint = Cast<UBlueprint>(Subject);
			return JoinKey(Kind, SubjectBlueprint != nullptr && SubjectBlueprint->GeneratedClass != nullptr
				? OwnerPath(SubjectBlueprint->GeneratedClass.Get()) : OwnerPath(Subject), FString());
		}
	}
	return FString();
}
} // namespace

// Signature used to tell apart actions sharing a key. For plain spawners it is
// the template node's signature. The shared template cache only builds a
// template once some graph has targeted it (otherwise GetSpawnerSignature()
// degrades to the bare node class), so prime it with the calling graph when the
// editor menu could (direct, non-transient Blueprint graph of a compatible schema).
FGuid ActionSignature(const UBlueprintNodeSpawner& Spawner, UEdGraph* Graph)
{
	if (Spawner.GetClass() == UBlueprintNodeSpawner::StaticClass() && Graph != nullptr && Spawner.NodeClass != nullptr
		&& !Graph->HasAnyFlags(RF_Transient) && Graph->GetSchema() != nullptr
		&& Graph->GetOuter() == FBlueprintEditorUtils::FindBlueprintForGraph(Graph)
		&& Spawner.NodeClass->GetDefaultObject<UEdGraphNode>()->CanCreateUnderSpecifiedSchema(Graph->GetSchema()))
	{
		if (const UK2Node* Template = Cast<UK2Node>(Spawner.GetTemplateNode(Graph)))
		{
			const FBlueprintNodeSignature Signature = Template->GetSignature();
			if (Signature.IsValid())
			{
				return Signature.AsGuid();
			}
		}
	}
	return Spawner.GetSpawnerSignature().AsGuid();
}

bool ComputeSpawnerBaseKey(const UObject* DatabaseOwner, const UBlueprintNodeSpawner& Spawner, UEdGraph* Graph, FString& OutKey)
{
	const UClass* NodeClass = Spawner.NodeClass.Get();
	if (NodeClass == nullptr)
	{
		return false;
	}
	const FString Kind = KindOf(NodeClass);
	FString Owner;
	FString Field;
	if (const UBlueprintBoundEventNodeSpawner* Bound = Cast<UBlueprintBoundEventNodeSpawner>(&Spawner))
	{
		const FMulticastDelegateProperty* Delegate = Bound->GetEventDelegate();
		if (Delegate == nullptr)
		{
			return false;
		}
		Owner = FieldOwnerPath(Delegate);
		Field = Delegate->GetName();
	}
	else if (const UBlueprintEventNodeSpawner* Event = Cast<UBlueprintEventNodeSpawner>(&Spawner))
	{
		if (!Event->IsForCustomEvent())
		{
			const UFunction* Function = Event->GetEventFunction();
			if (Function == nullptr)
			{
				return false;
			}
			Owner = FunctionOwnerPath(Function);
			Field = Function->GetName();
		}
	}
	else if (const UBlueprintFunctionNodeSpawner* FunctionSpawner = Cast<UBlueprintFunctionNodeSpawner>(&Spawner))
	{
		const UFunction* Function = FunctionSpawner->GetFunction();
		if (Function == nullptr)
		{
			return false;
		}
		if (NodeClass->IsChildOf(UK2Node_PromotableOperator::StaticClass()))
		{
			// One shared spawner per operator; its UFunction is whichever overload
			// registered first (load-order dependent), so key by operator name.
			Field = FTypePromotion::GetOpNameFromFunction(Function).ToString();
		}
		else
		{
			Owner = FunctionOwnerPath(Function);
			Field = Function->GetName();
		}
	}
	else if (const UBlueprintVariableNodeSpawner* Variable = Cast<UBlueprintVariableNodeSpawner>(&Spawner))
	{
		if (Variable->IsLocalVariable())
		{
			// Local variables are keyed "<GeneratedClass>:<Function>:<Var>" (same as
			// ComputeNodeKey); no template node is needed (the template cache may refuse).
			const FProperty* Property = Variable->GetVarProperty();
			const UFunction* Function = Property != nullptr ? Cast<UFunction>(Property->GetOwnerStruct()) : nullptr;
			if (Function == nullptr)
			{
				return false;
			}
			Owner = FunctionOwnerPath(Function) + TEXT(":") + Function->GetName();
			Field = Property->GetName();
		}
		else
		{
			const FProperty* Property = Variable->GetVarProperty();
			if (Property == nullptr)
			{
				return false;
			}
			Owner = FieldOwnerPath(Property);
			Field = Property->GetName();
		}
	}
	else if (const UBlueprintDelegateNodeSpawner* DelegateSpawner = Cast<UBlueprintDelegateNodeSpawner>(&Spawner))
	{
		const FMulticastDelegateProperty* Delegate = DelegateSpawner->GetDelegateProperty();
		if (Delegate == nullptr)
		{
			return false;
		}
		Owner = FieldOwnerPath(Delegate);
		Field = Delegate->GetName();
	}
	else if (const UBlueprintFieldNodeSpawner* FieldSpawner = Cast<UBlueprintFieldNodeSpawner>(&Spawner))
	{
		// Struct/enum nodes (MakeStruct, EnumLiteral, ForEachElementInEnum, ...):
		// one spawner per field, keyed by the field's path.
		const FFieldVariant FieldVariant = FieldSpawner->GetField();
		if (const UObject* FieldObject = FieldVariant.ToUObject())
		{
			Owner = OwnerPath(FieldObject);
		}
		else if (const FField* FieldProperty = FieldVariant.ToField())
		{
			Owner = FieldOwnerPath(FieldProperty);
			Field = FieldProperty->GetName();
		}
		else
		{
			return false;
		}
	}
	else if (const UBlueprintComponentNodeSpawner* Component = Cast<UBlueprintComponentNodeSpawner>(&Spawner))
	{
		Owner = OwnerPath(Component->GetComponentClass().Get());
	}
	else if (NodeClass->IsChildOf(UK2Node_MacroInstance::StaticClass()))
	{
		const UEdGraph* MacroGraph = MacroGraphOf(Spawner);
		if (MacroGraph == nullptr)
		{
			return false;
		}
		Field = MacroGraph->GetName();
		Owner = OwnerPath(FBlueprintEditorUtils::FindBlueprintForGraph(MacroGraph));
		if (Owner.IsEmpty())
		{
			return false;
		}
	}
	else if (DatabaseOwner != nullptr && DatabaseOwner != NodeClass)
	{
		// The database files a Blueprint class's actions (casts to it) under the Blueprint asset;
		// the key names the class, as the node's own key does (K2:DynamicCast:/Game/BP.BP_C).
		const UBlueprint* OwnerBlueprint = Cast<UBlueprint>(DatabaseOwner);
		Owner = OwnerBlueprint != nullptr && OwnerBlueprint->GeneratedClass != nullptr
			? OwnerPath(OwnerBlueprint->GeneratedClass.Get()) : OwnerPath(DatabaseOwner);
	}
	OutKey = JoinKey(Kind, Owner, Field);
	return true;
}

// The function a node's key names: the declaration the action database lists (the topmost
// overridden function; for an event, the interface function it implements), not a Blueprint
// class's override or event stub (content may hold self-context references to those).
static const UFunction* DeclaringFunction(const UFunction* Function, const UEdGraphNode* InterfacesOf)
{
	if (Function == nullptr)
	{
		return nullptr;
	}
	while (const UFunction* Super = Function->GetSuperFunction())
	{
		Function = Super;
	}
	const UClass* Owner = Function->GetOwnerClass();
	if (InterfacesOf == nullptr || Owner == nullptr || Owner->HasAnyClassFlags(CLASS_Interface))
	{
		return Function;
	}
	// Interfaces of the owner class and its parents, and those the Blueprint declares (its skeleton
	// class may not list them yet).
	TArray<const UClass*> Interfaces;
	for (const UClass* Class = Owner; Class != nullptr; Class = Class->GetSuperClass())
	{
		for (const FImplementedInterface& Interface : Class->Interfaces)
		{
			Interfaces.AddUnique(Interface.Class);
		}
	}
	if (const UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForNode(InterfacesOf))
	{
		for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
		{
			Interfaces.AddUnique(Interface.Interface.Get());
		}
	}
	for (const UClass* Interface : Interfaces)
	{
		if (Interface != nullptr)
		{
			if (const UFunction* Declared = Interface->FindFunctionByName(Function->GetFName()))
			{
				return Declared;
			}
		}
	}
	return Function;
}

FString ComputeNodeKey(const UEdGraphNode& Node)
{
	const UClass* NodeClass = Node.GetClass();
	const FString Kind = KindOf(NodeClass);
	if (const UK2Node_ComponentBoundEvent* Bound = Cast<UK2Node_ComponentBoundEvent>(&Node))
	{
		return JoinKey(Kind, OwnerPath(Bound->DelegateOwnerClass), Bound->DelegatePropertyName.ToString())
			+ TEXT("@") + Bound->ComponentPropertyName.ToString();
	}
	if (Node.IsA<UK2Node_CustomEvent>())
	{
		return JoinKey(Kind, FString(), FString());
	}
	if (NodeClass == UK2Node_Composite::StaticClass())
	{
		// A collapsed graph ("Collapse Nodes"): no palette action; K2:Composite places an empty one.
		return JoinKey(Kind, FString(), FString());
	}
	if (const UK2Node_Event* Event = Cast<UK2Node_Event>(&Node))
	{
		const UFunction* Function = DeclaringFunction(Event->FindEventSignatureFunction(), &Node);
		return Function != nullptr ? JoinKey(Kind, FunctionOwnerPath(Function), Function->GetName()) : FString();
	}
	if (const UK2Node_AddComponent* AddComponent = Cast<UK2Node_AddComponent>(&Node))
	{
		return AddComponent->TemplateType != nullptr
			? JoinKey(Kind, OwnerPath(AddComponent->TemplateType), FString()) : FString();
	}
	if (const UK2Node_PromotableOperator* Operator = Cast<UK2Node_PromotableOperator>(&Node))
	{
		return JoinKey(Kind, FString(), Operator->GetOperationName().ToString());
	}
	if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(&Node))
	{
		// "Parent: <Function>" names the parent's implementation it calls.
		const UFunction* Function = Node.IsA<UK2Node_CallParentFunction>()
			? Call->GetTargetFunction() : DeclaringFunction(Call->GetTargetFunction(), nullptr);
		return Function != nullptr ? JoinKey(Kind, FunctionOwnerPath(Function), Function->GetName()) : FString();
	}
	// Make / Break / Set Members struct nodes derive from UK2Node_Variable but name no variable:
	// they are keyed by their struct (below).
	if (const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(&Node); Variable != nullptr && !Node.IsA<UK2Node_StructOperation>())
	{
		if (Variable->VariableReference.IsLocalScope())
		{
			return JoinKey(Kind, LocalScopeOwner(*Variable), Variable->GetVarName().ToString());
		}
		const FProperty* Property = Variable->GetPropertyForVariable();
		return Property != nullptr ? JoinKey(Kind, FieldOwnerPath(Property), Property->GetName()) : FString();
	}
	if (const UK2Node_BaseMCDelegate* Delegate = Cast<UK2Node_BaseMCDelegate>(&Node))
	{
		const FProperty* Property = Delegate->GetProperty();
		return Property != nullptr ? JoinKey(Kind, FieldOwnerPath(Property), Property->GetName()) : FString();
	}
	if (const UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(&Node))
	{
		FString Field;
		const FString Owner = MacroOwner(*Macro, Field);
		return Owner.IsEmpty() ? FString() : JoinKey(Kind, Owner, Field);
	}
	if (const UK2Node_DynamicCast* Cast_ = Cast<UK2Node_DynamicCast>(&Node))
	{
		return Cast_->TargetType != nullptr ? JoinKey(Kind, OwnerPath(Cast_->TargetType.Get()), FString()) : FString();
	}
	if (const UObject* Subject = NodeFieldSubject(Node); Subject != nullptr && HasFieldSpawner(NodeClass, Subject))
	{
		return JoinKey(Kind, OwnerPath(Subject), FString());
	}
	// Plain node classes: the key is "K2:<Kind>" only when the database holds
	// exactly one plain action for this class (or the node signature picks one).
	const FBlueprintActionDatabase::FActionList* Actions =
		FBlueprintActionDatabase::Get().GetAllActions().Find(FObjectKey(NodeClass));
	if (Actions == nullptr)
	{
		return SubjectObjectKey(Node, Kind);
	}
	TArray<const UBlueprintNodeSpawner*> Plain;
	for (const UBlueprintNodeSpawner* Spawner : *Actions)
	{
		if (Spawner != nullptr && Spawner->NodeClass == NodeClass && Spawner->GetClass() == UBlueprintNodeSpawner::StaticClass())
		{
			Plain.Add(Spawner);
		}
	}
	if (Plain.Num() == 1)
	{
		return JoinKey(Kind, FString(), FString());
	}
	if (const UK2Node* K2Node = Cast<UK2Node>(&Node); K2Node != nullptr && Plain.Num() > 1)
	{
		const FGuid NodeSignature = K2Node->GetSignature().AsGuid();
		TArray<const UBlueprintNodeSpawner*> Matching;
		for (const UBlueprintNodeSpawner* Spawner : Plain)
		{
			if (ActionSignature(*Spawner, Node.GetGraph()) == NodeSignature)
			{
				Matching.Add(Spawner);
			}
		}
		if (Matching.Num() > 1)
		{
			// Spawners the signature does not tell apart (Get Engine Subsystem, one per subsystem class):
			// the one whose customization the node carries, keyed by what it customizes.
			const UBlueprintNodeSpawner* Best = nullptr;
			int32 BestCount = 0;
			for (const UBlueprintNodeSpawner* Spawner : Matching)
			{
				const FCustomizedValues& Values = CustomizedValuesOf(*Spawner);
				bool bCarries = Values.Properties.Num() > BestCount;
				for (int32 Index = 0; bCarries && Index < Values.Properties.Num(); ++Index)
				{
					bCarries = IdentityValueText(*Values.Properties[Index], &Node) == Values.Values[Index];
				}
				if (bCarries)
				{
					Best = Spawner;
					BestCount = Values.Properties.Num();
				}
			}
			if (Best != nullptr)
			{
				return JoinKey(Kind, CustomizedIdentity(*Best), FString());
			}
		}
		if (!Matching.IsEmpty())
		{
			return JoinKey(Kind, FString(), FString()) + TEXT("#")
				+ NodeSignature.ToString(EGuidFormats::Digits).Left(8);
		}
	}
	return SubjectObjectKey(Node, Kind);
}
FString CustomizedIdentity(const UBlueprintNodeSpawner& Spawner)
{
	return FString::Join(CustomizedValuesOf(Spawner).Values, TEXT(","));
}
} // namespace K2Keys

namespace
{
// What a node without an action names that does not exist, so no palette action can place it: a
// variable or function the Blueprint no longer has (left behind by a rename in native code, or in a
// graph that never compiles), or an input action the project's Input settings do not define.
FString UnresolvedReference(const UEdGraphNode& Node)
{
	if (const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(&Node);
		Variable != nullptr && !Node.IsA<UK2Node_StructOperation>() && Variable->GetPropertyForVariable() == nullptr)
	{
		return FString::Printf(TEXT("variable '%s'"), *Variable->GetVarName().ToString());
	}
	if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(&Node); Call != nullptr && Call->GetTargetFunction() == nullptr)
	{
		return FString::Printf(TEXT("function '%s'"), *Call->FunctionReference.GetMemberName().ToString());
	}
	if (const UK2Node_InputAction* Input = Cast<UK2Node_InputAction>(&Node))
	{
		return FString::Printf(TEXT("input action '%s' (Project Settings > Input does not define it)"), *Input->InputActionName.ToString());
	}
	return FString();
}

struct FK2Candidate
{
	const UObject* DatabaseOwner = nullptr;
	UBlueprintNodeSpawner* Spawner = nullptr;
	IBlueprintNodeBinder::FBindingSet Bindings;
	FString Key;
	bool bBound = false;
};

FString SignaturePrefix(const UBlueprintNodeSpawner& Spawner, UEdGraph& Graph)
{
	return K2Keys::ActionSignature(Spawner, &Graph).ToString(EGuidFormats::Digits).Left(8);
}

// The Blueprint members bound events are offered for: component variables (and members of registered
// bound-event sources, e.g. widget variables: the Details panel's "On Clicked [+]" in a Widget Blueprint).
struct FK2BoundEventMembers
{
	TArray<FObjectProperty*> ComponentProperties;
	TMap<const FObjectProperty*, const UClass*> ComponentInstanceClasses;
	TArray<TSharedRef<IAssetDocumentBoundEventSourceAdapter>> ExtraSources;

	bool IsExtraSourceClass(const UClass* Class) const
	{
		return ExtraSources.ContainsByPredicate([Class](const TSharedRef<IAssetDocumentBoundEventSourceAdapter>& Source)
			{ return Source->GetBoundObjectClass() != nullptr && Class->IsChildOf(Source->GetBoundObjectClass()); });
	}
};

FK2BoundEventMembers CollectBoundEventMembers(UBlueprint& Blueprint)
{
	FK2BoundEventMembers Members;
	Members.ExtraSources = FindAssetDocumentBoundEventSourceAdapters(&Blueprint);
	if (UClass* Skeleton = Blueprint.SkeletonGeneratedClass)
	{
		const UObject* Defaults = Blueprint.GeneratedClass != nullptr ? Blueprint.GeneratedClass->GetDefaultObject(false) : nullptr;
		for (TFieldIterator<FObjectProperty> It(Skeleton, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			if (It->PropertyClass != nullptr && (It->PropertyClass->IsChildOf(UActorComponent::StaticClass())
				|| Members.IsExtraSourceClass(It->PropertyClass)))
			{
				Members.ComponentProperties.Add(*It);
				const FObjectProperty* Generated = Defaults != nullptr ? FindFProperty<FObjectProperty>(Defaults->GetClass(), It->GetFName()) : nullptr;
				const UObject* Instance = Generated != nullptr ? Generated->GetObjectPropertyValue_InContainer(Defaults) : nullptr;
				if (Instance != nullptr && Instance->GetClass() != It->PropertyClass)
				{
					Members.ComponentInstanceClasses.Add(*It, Instance->GetClass());
				}
			}
		}
	}
	return Members;
}

// UBlueprintBoundEventNodeSpawner::IsBindingCompatible for a component property: for a component-bound
// event spawner it depends only on the property's class (the delegate's owner class and its hidden
// categories against that class), so the answer is kept per spawner and class. Every node lookup asks
// it for each bound event spawner and component member.
bool IsBoundEventCompatible(const UBlueprintBoundEventNodeSpawner& Bound, FObjectProperty& Property)
{
	static TMap<TPair<TObjectKey<UBlueprintBoundEventNodeSpawner>, TObjectKey<UClass>>, bool> Compatible;
	const TPair<TObjectKey<UBlueprintBoundEventNodeSpawner>, TObjectKey<UClass>> Key(&Bound, Property.PropertyClass.Get());
	if (const bool* Known = Compatible.Find(Key))
	{
		return *Known;
	}
	if (Compatible.Num() >= 100000)
	{
		Compatible.Reset();
	}
	return Compatible.Add(Key, Bound.IsBindingCompatible(FBindingObject(&Property)));
}

// One candidate per component member a component-bound event spawner applies to.
template <typename AddType>
void ExpandBoundEvent(const FK2BoundEventMembers& Members, const UBlueprintBoundEventNodeSpawner& Bound, const FString& Base, const AddType& Add)
{
	const FMulticastDelegateProperty* Delegate = Bound.GetEventDelegate();
	for (int32 Index = 0; Index < Members.ComponentProperties.Num(); ++Index)
	{
		FObjectProperty* Property = Members.ComponentProperties[Index];
		// The Details panel lists the events of the component object itself, which may be a
		// subclass of the property's type (a native component a parent class's constructor
		// replaced with SetDefaultSubobjectClass).
		const UClass* const* Instance = Members.ComponentInstanceClasses.Find(Property);
		const bool bInstanceHasEvent = Instance != nullptr && Delegate != nullptr && Delegate->GetOwnerClass() != nullptr
			&& (*Instance)->GetAuthoritativeClass()->IsChildOf(Delegate->GetOwnerClass()->GetAuthoritativeClass());
		if (!bInstanceHasEvent && !IsBoundEventCompatible(Bound, *Property))
		{
			continue;
		}
		Add(Property, Base + TEXT("@") + Property->GetName(), Index);
	}
}

// The component-bound event spawner for a delegate of an extra bound-event source, created once and
// shared like the action database's own spawners.
UBlueprintBoundEventNodeSpawner* ExtraSourceEventSpawner(FMulticastDelegateProperty& Delegate)
{
	static TMap<FString, TWeakObjectPtr<UBlueprintBoundEventNodeSpawner>> Spawners;
	TWeakObjectPtr<UBlueprintBoundEventNodeSpawner>& Cached = Spawners.FindOrAdd(FFieldPath(&Delegate).ToString());
	if (!Cached.IsValid())
	{
		UBlueprintBoundEventNodeSpawner* Created = UBlueprintBoundEventNodeSpawner::Create(
			UK2Node_ComponentBoundEvent::StaticClass(), &Delegate, GetTransientPackage());
		Created->AddToRoot();
		Cached = Created;
	}
	return Cached.Get();
}

// Extra-source events (the Details panel's "On Clicked [+]" of a widget,
// FKismetEditorUtilities::CreateNewBoundEventForClass): the action database registers component-bound
// event spawners for UActorComponent delegates only, so the same UBlueprintBoundEventNodeSpawner is
// created for each assignable delegate of a registered source's objects.
template <typename AddType>
void ExpandExtraSourceEvents(const FK2BoundEventMembers& Members, UEdGraph& Graph, const AddType& Add)
{
	if (Members.ExtraSources.IsEmpty())
	{
		return;
	}
	for (FObjectProperty* Property : Members.ComponentProperties)
	{
		if (!Members.IsExtraSourceClass(Property->PropertyClass))
		{
			continue;
		}
		for (TFieldIterator<FMulticastDelegateProperty> It(Property->PropertyClass); It; ++It)
		{
			if (!It->HasAnyPropertyFlags(CPF_BlueprintAssignable))
			{
				continue;
			}
			UBlueprintBoundEventNodeSpawner* Spawner = ExtraSourceEventSpawner(**It);
			FString Base;
			if (!IsBoundEventCompatible(*Spawner, *Property)
				|| !K2Keys::ComputeSpawnerBaseKey(It->GetOwnerClass(), *Spawner, &Graph, Base))
			{
				continue;
			}
			Add(It->GetOwnerClass(), Spawner, Property, Base + TEXT("@") + Property->GetName());
		}
	}
}

// Every action of the database with its canonical key. Keys never depend on the
// graph context, so the same action always gets the same key (also across
// editor restarts: no pointer/order-dependent parts). This walks and keys the whole database;
// CollectCandidates gives the same list from what changed since its last call.
void ComputeK2Candidates(UBlueprint& Blueprint, UEdGraph& Graph, TArray<FK2Candidate>& Out)
{
	Out.Reset();
	const FK2BoundEventMembers Members = CollectBoundEventMembers(Blueprint);
	for (const TPair<FObjectKey, FBlueprintActionDatabase::FActionList>& Pair : FBlueprintActionDatabase::Get().GetAllActions())
	{
		const UObject* Owner = Pair.Key.ResolveObjectPtr();
		if (Owner == nullptr)
		{
			continue;
		}
		for (UBlueprintNodeSpawner* Spawner : Pair.Value)
		{
			// Bound node spawners need level/editor selection; not addressable by value.
			if (Spawner == nullptr || Spawner->NodeClass == nullptr || Spawner->IsA<UBlueprintBoundNodeSpawner>())
			{
				continue;
			}
			FString Base;
			if (!K2Keys::ComputeSpawnerBaseKey(Owner, *Spawner, &Graph, Base))
			{
				continue;
			}
			if (const UBlueprintBoundEventNodeSpawner* Bound = Cast<UBlueprintBoundEventNodeSpawner>(Spawner))
			{
				if (Spawner->NodeClass->IsChildOf(UK2Node_ComponentBoundEvent::StaticClass()))
				{
					ExpandBoundEvent(Members, *Bound, Base, [&Out, Owner, Spawner](FObjectProperty* Property, FString&& Key, int32)
						{
							FK2Candidate& Candidate = Out.AddDefaulted_GetRef();
							Candidate.DatabaseOwner = Owner;
							Candidate.Spawner = Spawner;
							Candidate.Bindings.Add(FBindingObject(Property));
							Candidate.Key = MoveTemp(Key);
							Candidate.bBound = true;
						});
				}
				continue;
			}
			FK2Candidate& Candidate = Out.AddDefaulted_GetRef();
			Candidate.DatabaseOwner = Owner;
			Candidate.Spawner = Spawner;
			Candidate.Key = MoveTemp(Base);
		}
	}
	ExpandExtraSourceEvents(Members, Graph, [&Out](const UObject* Owner, UBlueprintNodeSpawner* Spawner, FObjectProperty* Property, FString&& Key)
		{
			FK2Candidate& Candidate = Out.AddDefaulted_GetRef();
			Candidate.DatabaseOwner = Owner;
			Candidate.Spawner = Spawner;
			Candidate.Bindings.Add(FBindingObject(Property));
			Candidate.Key = MoveTemp(Key);
			Candidate.bBound = true;
		});
	// Disambiguate collisions with the deterministic spawner-signature prefix.
	TMap<FString, int32> Counts;
	Counts.Reserve(Out.Num());
	for (const FK2Candidate& Candidate : Out)
	{
		++Counts.FindOrAdd(Candidate.Key);
	}
	TArray<FString> BaseKeys;
	BaseKeys.Reserve(Out.Num());
	for (FK2Candidate& Candidate : Out)
	{
		BaseKeys.Add(Candidate.Key);
		if (Counts.FindChecked(Candidate.Key) > 1)
		{
			Candidate.Key += TEXT("#") + SignaturePrefix(*Candidate.Spawner, Graph);
		}
	}
	// Plain spawners the signature does not tell apart (one Get Engine Subsystem per subsystem class) are
	// keyed by what their CustomizeNodeDelegate sets, as ComputeNodeKey keys their nodes.
	TMap<FString, int32> SignatureCounts;
	SignatureCounts.Reserve(Out.Num());
	for (const FK2Candidate& Candidate : Out)
	{
		++SignatureCounts.FindOrAdd(Candidate.Key);
	}
	for (int32 Index = 0; Index < Out.Num(); ++Index)
	{
		FK2Candidate& Candidate = Out[Index];
		if (SignatureCounts.FindChecked(Candidate.Key) > 1 && !Candidate.bBound
			&& Candidate.Spawner->GetClass() == UBlueprintNodeSpawner::StaticClass())
		{
			const FString Identity = K2Keys::CustomizedIdentity(*Candidate.Spawner);
			if (!Identity.IsEmpty())
			{
				Candidate.Key = BaseKeys[Index] + TEXT(":") + Identity;
			}
		}
	}
	// Identical signatures are the same action registered twice: keep one.
	SortUniqueByKey(Out, [](const FK2Candidate& Candidate) -> const FString& { return Candidate.Key; });
}

// The incremental form of ComputeK2Candidates. Every node a document places looks its action up in
// this list, and keying the whole database (~44,000 spawners) took a noticeable fraction of a second
// per node: minutes for a large graph. Placing a node changes only the edited Blueprint's own database
// entry, so the list is kept per graph and only what changed is keyed again:
// - each database owner's spawner keys are kept while its spawner list is unchanged (same spawner
//   objects; a spawner the database recreates is a new object);
// - a key's group (the actions sharing that base key, told apart by the signature or customization)
//   is resolved again when one of its members changed;
// - bound events depend on the Blueprint's current members and are expanded on every call.
// Steps 2-4 of ComputeK2Candidates only compare candidates of one group, so resolving groups
// separately and merging them by key gives the same list.
namespace K2CandidateCache
{
struct FRaw
{
	const UObject* Owner = nullptr;
	UBlueprintNodeSpawner* Spawner = nullptr;
	IBlueprintNodeBinder::FBindingSet Bindings;
	FString Key;
	bool bBound = false;
	// Where ComputeK2Candidates lists it (the tie-break of equal final keys): the owner's position in the
	// database, then Local. Cached entries hold Local only, as owners' positions move.
	int64 Local = 0;
	int64 Ordinal = 0;
};

struct FBoundSpawner
{
	const UObject* Owner = nullptr;
	UBlueprintBoundEventNodeSpawner* Spawner = nullptr;
	FString Base;
	int64 Local = 0;
};

struct FOwnerKeys
{
	uint64 Hash = 0;
	TArray<FRaw> Plain;
	TArray<FBoundSpawner> Bound;
};

struct FFinal
{
	FK2Candidate Candidate;
	int64 Ordinal = 0;
	int32 Group = INDEX_NONE;
	int32 Owner = INDEX_NONE;  // baseline owner index (entries of FGraphState::Stable)
};

struct FMember
{
	int32 Owner = INDEX_NONE;  // index into FGraphState::Owners
	int32 Entry = INDEX_NONE;  // index into that owner's Plain
};

// One graph's list at its baseline (the database when it was last keyed in full).
struct FGraphState
{
	TArray<TSharedPtr<const FOwnerKeys>> Owners;
	TMap<FObjectKey, int32> OwnerIndex;
	TArray<TArray<int32>> OwnerGroups;
	TArray<FString> GroupKeys;
	TMap<FString, int32> GroupOfKey;
	TArray<TArray<FMember>> GroupMembers;
	TArray<FFinal> Stable;  // every group resolved, sorted by key and ordinal
	bool bValid = false;
};

constexpr int64 ExtraSourceOwnerOrdinal = (int64(1) << 23) - 1;
constexpr int64 LocalMask = (int64(1) << 40) - 1;

int64 MakeLocal(int64 Spawner, int64 Binding)
{
	return (Spawner << 16) | Binding;
}

int64 MakeOrdinal(int64 OwnerPosition, int64 Local)
{
	return (OwnerPosition << 40) | Local;
}

uint64 HashSpawners(const UObject* Owner, const FBlueprintActionDatabase::FActionList& Spawners)
{
	uint64 Hash = 14695981039346656037ull;
	const auto Mix = [&Hash](uint64 Value)
	{
		Hash = (Hash ^ Value) * 1099511628211ull;
	};
	Mix(static_cast<uint64>(reinterpret_cast<UPTRINT>(Owner)));
	Mix(static_cast<uint64>(Spawners.Num()));
	for (const UBlueprintNodeSpawner* Spawner : Spawners)
	{
		Mix(static_cast<uint64>(reinterpret_cast<UPTRINT>(Spawner)));
		if (Spawner != nullptr)
		{
			const FName Name = Spawner->GetFName();
			Mix((static_cast<uint64>(Name.GetComparisonIndex().ToUnstableInt()) << 32) | static_cast<uint32>(Name.GetNumber()));
			Mix(static_cast<uint64>(reinterpret_cast<UPTRINT>(Spawner->NodeClass.Get())));
		}
	}
	return Hash;
}

TSharedPtr<const FOwnerKeys> KeyOwner(const UObject* Owner, const FBlueprintActionDatabase::FActionList& Spawners, uint64 Hash,
	UEdGraph& Graph)
{
	TSharedPtr<FOwnerKeys> Keys = MakeShared<FOwnerKeys>();
	Keys->Hash = Hash;
	for (int32 Index = 0; Index < Spawners.Num(); ++Index)
	{
		UBlueprintNodeSpawner* Spawner = Spawners[Index];
		// Bound node spawners need level/editor selection; not addressable by value.
		if (Spawner == nullptr || Spawner->NodeClass == nullptr || Spawner->IsA<UBlueprintBoundNodeSpawner>())
		{
			continue;
		}
		FString Base;
		if (!K2Keys::ComputeSpawnerBaseKey(Owner, *Spawner, &Graph, Base))
		{
			continue;
		}
		if (UBlueprintBoundEventNodeSpawner* Bound = Cast<UBlueprintBoundEventNodeSpawner>(Spawner))
		{
			if (Spawner->NodeClass->IsChildOf(UK2Node_ComponentBoundEvent::StaticClass()))
			{
				Keys->Bound.Add({Owner, Bound, MoveTemp(Base), MakeLocal(Index, 0)});
			}
			continue;
		}
		FRaw& Raw = Keys->Plain.AddDefaulted_GetRef();
		Raw.Owner = Owner;
		Raw.Spawner = Spawner;
		Raw.Key = MoveTemp(Base);
		Raw.Local = MakeLocal(Index, 0);
	}
	return Keys;
}

bool KeyLess(const FString& KeyA, int64 OrdinalA, const FString& KeyB, int64 OrdinalB)
{
	if (KeyA < KeyB)
	{
		return true;
	}
	return !(KeyB < KeyA) && OrdinalA < OrdinalB;
}

// Steps 2 and 3 of ComputeK2Candidates for the members of one group (one base key), in their order.
void ResolveGroup(TArray<FRaw>& Members, UEdGraph& Graph, int32 Group, TArray<FFinal>& Out, const TArray<int32>* Owners = nullptr)
{
	Members.Sort([](const FRaw& A, const FRaw& B) { return A.Ordinal < B.Ordinal; });
	const int32 First = Out.Num();
	for (int32 Index = 0; Index < Members.Num(); ++Index)
	{
		const FRaw& Raw = Members[Index];
		FFinal& Final = Out.AddDefaulted_GetRef();
		Final.Candidate.DatabaseOwner = Raw.Owner;
		Final.Candidate.Spawner = Raw.Spawner;
		Final.Candidate.Bindings = Raw.Bindings;
		Final.Candidate.bBound = Raw.bBound;
		Final.Candidate.Key = Members.Num() > 1 ? Raw.Key + TEXT("#") + SignaturePrefix(*Raw.Spawner, Graph) : Raw.Key;
		Final.Ordinal = Raw.Ordinal;
		Final.Group = Group;
		Final.Owner = Owners != nullptr ? (*Owners)[Index] : INDEX_NONE;
	}
	if (Members.Num() <= 1)
	{
		return;
	}
	TMap<FString, int32> SignatureCounts;
	for (int32 Index = First; Index < Out.Num(); ++Index)
	{
		++SignatureCounts.FindOrAdd(Out[Index].Candidate.Key);
	}
	for (int32 Index = First; Index < Out.Num(); ++Index)
	{
		FK2Candidate& Candidate = Out[Index].Candidate;
		if (SignatureCounts.FindChecked(Candidate.Key) > 1 && !Candidate.bBound
			&& Candidate.Spawner->GetClass() == UBlueprintNodeSpawner::StaticClass())
		{
			const FString Identity = K2Keys::CustomizedIdentity(*Candidate.Spawner);
			if (!Identity.IsEmpty())
			{
				Candidate.Key = Members[Index - First].Key + TEXT(":") + Identity;
			}
		}
	}
}

void SortFinals(TArray<FFinal>& Finals)
{
	Finals.Sort([](const FFinal& A, const FFinal& B) { return KeyLess(A.Candidate.Key, A.Ordinal, B.Candidate.Key, B.Ordinal); });
}

// Keys the whole database for Graph: the new baseline.
void Rebuild(FGraphState& State, const TArray<TPair<FObjectKey, TSharedPtr<const FOwnerKeys>>>& Current, UEdGraph& Graph)
{
	State = FGraphState();
	State.Owners.Reserve(Current.Num());
	State.OwnerGroups.SetNum(Current.Num());
	for (int32 OwnerIndex = 0; OwnerIndex < Current.Num(); ++OwnerIndex)
	{
		State.Owners.Add(Current[OwnerIndex].Value);
		State.OwnerIndex.Add(Current[OwnerIndex].Key, OwnerIndex);
		const TArray<FRaw>& Plain = Current[OwnerIndex].Value->Plain;
		for (int32 Entry = 0; Entry < Plain.Num(); ++Entry)
		{
			int32& Group = State.GroupOfKey.FindOrAdd(Plain[Entry].Key, INDEX_NONE);
			if (Group == INDEX_NONE)
			{
				Group = State.GroupKeys.Add(Plain[Entry].Key);
				State.GroupMembers.AddDefaulted();
			}
			State.GroupMembers[Group].Add({OwnerIndex, Entry});
			if (State.OwnerGroups[OwnerIndex].IsEmpty() || State.OwnerGroups[OwnerIndex].Last() != Group)
			{
				State.OwnerGroups[OwnerIndex].Add(Group);
			}
		}
	}
	State.Stable.Reserve(State.GroupOfKey.Num() + 64);
	TArray<FRaw> Members;
	TArray<int32> MemberOwners;
	for (int32 Group = 0; Group < State.GroupMembers.Num(); ++Group)
	{
		Members.Reset();
		for (const FMember& Member : State.GroupMembers[Group])
		{
			FRaw& Raw = Members.Add_GetRef(State.Owners[Member.Owner]->Plain[Member.Entry]);
			Raw.Ordinal = MakeOrdinal(Member.Owner, Raw.Local);
		}
		// ResolveGroup lists the members by ordinal, whose owner part is the owner index here.
		Members.Sort([](const FRaw& A, const FRaw& B) { return A.Ordinal < B.Ordinal; });
		MemberOwners.Reset();
		for (const FRaw& Raw : Members)
		{
			MemberOwners.Add(static_cast<int32>(Raw.Ordinal >> 40));
		}
		ResolveGroup(Members, Graph, Group, State.Stable, &MemberOwners);
	}
	SortFinals(State.Stable);
	State.bValid = true;
}
} // namespace K2CandidateCache

void CollectCandidates(UBlueprint& Blueprint, UEdGraph& Graph, TArray<FK2Candidate>& Out)
{
	using namespace K2CandidateCache;
	// Keyed spawners of each database owner, shared by every graph (a spawner's base key does not
	// depend on the graph).
	static TMap<FObjectKey, TSharedPtr<const FOwnerKeys>> OwnerCache;
	static TMap<TObjectKey<UEdGraph>, FGraphState> States;
	FGraphState* State = States.Find(&Graph);
	if (State == nullptr)
	{
		// Each state holds the whole database's keys: a few graphs at a time.
		if (States.Num() >= 4)
		{
			States.Reset();
		}
		State = &States.Add(&Graph);
	}

	TArray<TPair<FObjectKey, TSharedPtr<const FOwnerKeys>>> Current;
	TArray<bool> CurrentChanged;
	int32 Changed = 0;
	for (const TPair<FObjectKey, FBlueprintActionDatabase::FActionList>& Pair : FBlueprintActionDatabase::Get().GetAllActions())
	{
		const UObject* Owner = Pair.Key.ResolveObjectPtr();
		if (Owner == nullptr)
		{
			continue;
		}
		const uint64 Hash = HashSpawners(Owner, Pair.Value);
		TSharedPtr<const FOwnerKeys>& Cached = OwnerCache.FindOrAdd(Pair.Key);
		if (!Cached.IsValid() || Cached->Hash != Hash)
		{
			Cached = KeyOwner(Owner, Pair.Value, Hash, Graph);
		}
		const int32* Baseline = State->OwnerIndex.Find(Pair.Key);
		const bool bChanged = Baseline == nullptr || State->Owners[*Baseline]->Hash != Hash;
		Changed += bChanged ? 1 : 0;
		Current.Emplace(Pair.Key, Cached);
		CurrentChanged.Add(bChanged);
	}
	// Owners of the baseline the database no longer has.
	TBitArray<> BaselineChanged(false, State->Owners.Num());
	TArray<int32> CurrentOfBaseline;  // each baseline owner's position in the database now
	CurrentOfBaseline.Init(INDEX_NONE, State->Owners.Num());
	{
		TBitArray<> Seen(false, State->Owners.Num());
		for (int32 Index = 0; Index < Current.Num(); ++Index)
		{
			if (const int32* Baseline = State->OwnerIndex.Find(Current[Index].Key))
			{
				Seen[*Baseline] = true;
				BaselineChanged[*Baseline] = CurrentChanged[Index];
				CurrentOfBaseline[*Baseline] = Index;
			}
		}
		for (int32 Index = 0; Index < State->Owners.Num(); ++Index)
		{
			if (!Seen[Index])
			{
				BaselineChanged[Index] = true;
				++Changed;
			}
		}
	}
	if (!State->bValid || Changed > 64)
	{
		Rebuild(*State, Current, Graph);
		BaselineChanged.Init(false, State->Owners.Num());
		CurrentChanged.Init(false, Current.Num());
		CurrentOfBaseline.SetNumUninitialized(State->Owners.Num());
		for (int32 Index = 0; Index < CurrentOfBaseline.Num(); ++Index)
		{
			CurrentOfBaseline[Index] = Index;
		}
		// Drop the keys of owners the database no longer lists.
		if (OwnerCache.Num() > Current.Num())
		{
			TSet<FObjectKey> Live;
			for (const TPair<FObjectKey, TSharedPtr<const FOwnerKeys>>& Each : Current)
			{
				Live.Add(Each.Key);
			}
			for (auto It = OwnerCache.CreateIterator(); It; ++It)
			{
				if (!Live.Contains(It.Key()))
				{
					It.RemoveCurrent();
				}
			}
		}
	}

	// What changed since the baseline: changed owners' entries, and every bound event.
	TArray<FRaw> Volatile;
	const FK2BoundEventMembers Members = CollectBoundEventMembers(Blueprint);
	for (int32 Index = 0; Index < Current.Num(); ++Index)
	{
		const FOwnerKeys& Keys = *Current[Index].Value;
		if (CurrentChanged[Index])
		{
			for (const FRaw& Plain : Keys.Plain)
			{
				Volatile.Add_GetRef(Plain).Ordinal = MakeOrdinal(Index, Plain.Local);
			}
		}
		for (const FBoundSpawner& Bound : Keys.Bound)
		{
			ExpandBoundEvent(Members, *Bound.Spawner, Bound.Base, [&Volatile, &Bound, Index](FObjectProperty* Property, FString&& Key, int32 PropertyIndex)
				{
					FRaw& Raw = Volatile.AddDefaulted_GetRef();
					Raw.Owner = Bound.Owner;
					Raw.Spawner = Bound.Spawner;
					Raw.Bindings.Add(FBindingObject(Property));
					Raw.Key = MoveTemp(Key);
					Raw.bBound = true;
					Raw.Ordinal = MakeOrdinal(Index, Bound.Local + PropertyIndex);
				});
		}
	}
	int64 ExtraIndex = 0;
	ExpandExtraSourceEvents(Members, Graph, [&Volatile, &ExtraIndex](const UObject* Owner, UBlueprintNodeSpawner* Spawner, FObjectProperty* Property, FString&& Key)
		{
			FRaw& Raw = Volatile.AddDefaulted_GetRef();
			Raw.Owner = Owner;
			Raw.Spawner = Spawner;
			Raw.Bindings.Add(FBindingObject(Property));
			Raw.Key = MoveTemp(Key);
			Raw.bBound = true;
			Raw.Ordinal = MakeOrdinal(ExtraSourceOwnerOrdinal, ExtraIndex++);
		});

	// Groups to resolve again: those holding a changed owner's baseline entries or a volatile entry's key.
	TBitArray<> Excluded(false, State->GroupKeys.Num());
	TMap<FString, TArray<FRaw>> Regroup;
	const auto Exclude = [&State, &Excluded, &Regroup, &BaselineChanged, &CurrentOfBaseline](int32 Group)
	{
		if (Excluded[Group])
		{
			return;
		}
		Excluded[Group] = true;
		TArray<FRaw>& Kept = Regroup.FindOrAdd(State->GroupKeys[Group]);
		for (const FMember& Member : State->GroupMembers[Group])
		{
			if (!BaselineChanged[Member.Owner])
			{
				FRaw& Raw = Kept.Add_GetRef(State->Owners[Member.Owner]->Plain[Member.Entry]);
				Raw.Ordinal = MakeOrdinal(CurrentOfBaseline[Member.Owner], Raw.Local);
			}
		}
	};
	for (int32 Owner = 0; Owner < State->Owners.Num(); ++Owner)
	{
		if (BaselineChanged[Owner])
		{
			for (const int32 Group : State->OwnerGroups[Owner])
			{
				Exclude(Group);
			}
		}
	}
	for (FRaw& Raw : Volatile)
	{
		if (const int32* Group = State->GroupOfKey.Find(Raw.Key))
		{
			Exclude(*Group);
		}
	}
	for (FRaw& Raw : Volatile)
	{
		Regroup.FindOrAdd(Raw.Key).Add(MoveTemp(Raw));
	}
	TArray<FFinal> Resolved;
	for (TPair<FString, TArray<FRaw>>& Group : Regroup)
	{
		ResolveGroup(Group.Value, Graph, INDEX_NONE, Resolved);
	}
	SortFinals(Resolved);

	// Merge the baseline's untouched groups with the resolved ones; equal keys keep the first listed. An
	// untouched entry's owner is still in the database, and orders by the owner's position now.
	const auto StableOrdinal = [&CurrentOfBaseline](const FFinal& Final)
	{
		return MakeOrdinal(CurrentOfBaseline[Final.Owner], Final.Ordinal & LocalMask);
	};
	Out.Reset(State->Stable.Num() + Resolved.Num());
	const auto Append = [&Out](const FFinal& Final)
	{
		if (Out.IsEmpty() || !(Out.Last().Key == Final.Candidate.Key))
		{
			Out.Add(Final.Candidate);
		}
	};
	int32 StableIndex = 0;
	int32 ResolvedIndex = 0;
	while (true)
	{
		while (StableIndex < State->Stable.Num() && Excluded[State->Stable[StableIndex].Group])
		{
			++StableIndex;
		}
		const bool bStable = StableIndex < State->Stable.Num();
		const bool bResolved = ResolvedIndex < Resolved.Num();
		if (!bStable && !bResolved)
		{
			break;
		}
		if (bStable && (!bResolved || KeyLess(State->Stable[StableIndex].Candidate.Key, StableOrdinal(State->Stable[StableIndex]),
			Resolved[ResolvedIndex].Candidate.Key, Resolved[ResolvedIndex].Ordinal)))
		{
			Append(State->Stable[StableIndex++]);
		}
		else
		{
			Append(Resolved[ResolvedIndex++]);
		}
	}
}

UClass* PinObjectClass(const UEdGraphPin& Pin, UBlueprint& Blueprint)
{
	if (Pin.PinType.PinSubCategory == UEdGraphSchema_K2::PSC_Self)
	{
		return Blueprint.SkeletonGeneratedClass;
	}
	return Cast<UClass>(Pin.PinType.PinSubCategoryObject.Get());
}

void InitFilter(FBlueprintActionFilter& Filter, UBlueprint& Blueprint, UEdGraph& Graph, UEdGraphPin* FromPin)
{
	// The context the graph context menu's main (context-sensitive) section filters by,
	// without editor selection (non-deterministic) and asset-reference policy.
	Filter.Context.Blueprints.Add(&Blueprint);
	Filter.Context.Graphs.Add(&Graph);
	if (Blueprint.SkeletonGeneratedClass != nullptr)
	{
		FBlueprintActionFilter::AddUnique(Filter.TargetClasses, Blueprint.SkeletonGeneratedClass);
	}
	if (FromPin != nullptr)
	{
		Filter.Context.Pins.Add(FromPin);
		if (UClass* PinClass = PinObjectClass(*FromPin, Blueprint))
		{
			FBlueprintActionFilter::AddUnique(Filter.TargetClasses, PinClass);
		}
	}
}

bool IsAllowed(FBlueprintActionFilter& Filter, const FK2Candidate& Candidate)
{
	if (Candidate.bBound)
	{
		return true;
	}
	FBlueprintActionInfo Info(Candidate.DatabaseOwner, Candidate.Spawner);
	return !Filter.IsFiltered(Info);
}

// A function call the menu lists with Context Sensitive off whose key or display name holds every word (cheap
// checks first: most of the database fails the words).
bool IsContextFreeCall(FBlueprintActionFilter& ContextFree, const FK2Candidate& Candidate, const TArray<FString>& Words)
{
	const UBlueprintFunctionNodeSpawner* Spawner = Cast<UBlueprintFunctionNodeSpawner>(Candidate.Spawner);
	const UFunction* Function = Spawner != nullptr && !Candidate.bBound ? Spawner->GetFunction() : nullptr;
	if (Function == nullptr)
	{
		return false;
	}
	const FString DisplayName = Function->GetMetaData(FBlueprintMetadata::MD_DisplayName);
	for (const FString& Word : Words)
	{
		if (!Candidate.Key.Contains(Word, ESearchCase::IgnoreCase) && !DisplayName.Contains(Word, ESearchCase::IgnoreCase))
		{
			return false;
		}
	}
	return IsAllowed(ContextFree, Candidate);
}

// The member an action places (an event's or a field's name), to find the same member under another key.
FName ActionMemberName(const UBlueprintNodeSpawner& Spawner)
{
	if (const UBlueprintEventNodeSpawner* Event = Cast<UBlueprintEventNodeSpawner>(&Spawner))
	{
		const UFunction* Function = Event->GetEventFunction();
		return Function != nullptr ? Function->GetFName() : NAME_None;
	}
	if (const UBlueprintFieldNodeSpawner* Field = Cast<UBlueprintFieldNodeSpawner>(&Spawner))
	{
		const FFieldVariant Member = Field->GetField();
		return Member.IsValid() ? Member.GetFName() : NAME_None;
	}
	return NAME_None;
}

// One candidate action as the palette shows it: its key, menu name, owner (the class or asset that registers it) and
// node class, and its default pins ("in: ...; out: ...") from the palette's template node.
FString DescribeCandidate(const FK2Candidate& Candidate, UEdGraph& Graph)
{
	FString Text = Candidate.Key;
	if (Candidate.Spawner == nullptr)
	{
		return Text;
	}
	const FBlueprintActionUiSpec& Ui = Candidate.Spawner->PrimeDefaultUiSpec(&Graph);
	if (!Ui.MenuName.IsEmpty())
	{
		Text += FString::Printf(TEXT(" \"%s\""), *Ui.MenuName.ToString());
	}
	TArray<FString> Parts;
	if (const UClass* OwnerClass = Cast<UClass>(Candidate.DatabaseOwner))
	{
		Parts.Add(FString::Printf(TEXT("owner %s"), *K2Keys::OwnerPath(OwnerClass)));
	}
	else if (Candidate.DatabaseOwner != nullptr)
	{
		Parts.Add(FString::Printf(TEXT("owner %s"), *Candidate.DatabaseOwner->GetPathName()));
	}
	if (Candidate.Spawner->NodeClass != nullptr && Candidate.Spawner->NodeClass != Candidate.DatabaseOwner)
	{
		Parts.Add(Candidate.Spawner->NodeClass->GetName());
	}
	UEdGraphNode* Template = !Candidate.bBound && Candidate.Spawner->NodeClass != nullptr && Graph.GetSchema() != nullptr
		&& !Graph.HasAnyFlags(RF_Transient)
		&& Candidate.Spawner->NodeClass->GetDefaultObject<UEdGraphNode>()->CanCreateUnderSpecifiedSchema(Graph.GetSchema())
		? Candidate.Spawner->GetTemplateNode(&Graph) : nullptr;
	if (Template != nullptr)
	{
		// Templates are spawned without pins (as GetTemplateNodes).
		if (Template->Pins.IsEmpty())
		{
			Template->AllocateDefaultPins();
		}
		TArray<FString> Inputs;
		TArray<FString> Outputs;
		for (const UEdGraphPin* Pin : Template->Pins)
		{
			if (Pin != nullptr && !Pin->bHidden)
			{
				(Pin->Direction == EGPD_Input ? Inputs : Outputs).Add(Pin->PinName.ToString());
			}
		}
		Parts.Add(FString::Printf(TEXT("in: %s; out: %s"), Inputs.IsEmpty() ? TEXT("-") : *FString::Join(Inputs, TEXT(", ")),
			Outputs.IsEmpty() ? TEXT("-") : *FString::Join(Outputs, TEXT(", "))));
	}
	return Parts.IsEmpty() ? Text : FString::Printf(TEXT("%s (%s)"), *Text, *FString::Join(Parts, TEXT("; ")));
}

// The editor's palette with "Context Sensitive" off (FBlueprintActionMenuUtils::MakeContextMenu with
// bIsContextSensitive = false): no target classes, no pin, no context-only rejections. It offers member
// calls on other classes (a component function in a construction script, a function of a variable's
// class), which the context-sensitive menu shows only when dragging from a pin of that class. A node
// the editor can place this way is placeable without context; its target is wired by the links.
bool IsInFullPalette(UBlueprint& Blueprint, UEdGraph& Graph, const FK2Candidate& Candidate)
{
	FBlueprintActionFilter Palette(FBlueprintActionFilter::BPFILTER_NoFlags);
	Palette.Context.Blueprints.Add(&Blueprint);
	Palette.Context.Graphs.Add(&Graph);
	return IsAllowed(Palette, Candidate);
}

// My Blueprint's "Implement event" (SMyBlueprint::ImplementFunction) on an inherited or interface
// function that can be placed as an event: it adds the event to the event graph without the palette's
// filter, so an event the palette hides (an editor-only interface event in a non-editor Blueprint, as in
// ChaosVD's BP_CVD_SkySphere) is still implementable there.
bool IsImplementableEvent(UBlueprint& Blueprint, UEdGraph& Graph, const FK2Candidate& Candidate)
{
	const UBlueprintEventNodeSpawner* Event = Cast<UBlueprintEventNodeSpawner>(Candidate.Spawner);
	const UFunction* EventFunction = Event != nullptr ? Event->GetEventFunction() : nullptr;
	if (EventFunction == nullptr || !Blueprint.UbergraphPages.Contains(&Graph)
		|| Blueprint.BlueprintType == BPTYPE_MacroLibrary)
	{
		return false;
	}
	UFunction* OverrideFunction = nullptr;
	const UClass* OverrideClass = FBlueprintEditorUtils::GetOverrideFunctionClass(&Blueprint, EventFunction->GetFName(), &OverrideFunction);
	return OverrideClass != nullptr && OverrideFunction != nullptr
		&& OverrideFunction->GetOwnerClass()->GetAuthoritativeClass() == EventFunction->GetOwnerClass()->GetAuthoritativeClass()
		&& UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(OverrideFunction);
}

// The event dispatcher "Event" entry (FKismetDelegateDragDropAction::MakeEvent): a custom event with the
// dispatcher's signature, offered in event graphs for every assignable multicast delegate of the
// Blueprint's class. The database has no spawner for it, so it gets the key
// "K2:CustomEvent:<DelegateOwner>:<Delegate>" (the spawned node itself is a plain K2:CustomEvent).
struct FDispatcherEvent
{
	FString Key;
	FMulticastDelegateProperty* Delegate = nullptr;
};

void CollectDispatcherEvents(UBlueprint& Blueprint, UEdGraph& Graph, TArray<FDispatcherEvent>& Out)
{
	if (!FBlueprintEditorUtils::DoesSupportEventGraphs(&Blueprint) || !Blueprint.UbergraphPages.Contains(&Graph)
		|| Blueprint.SkeletonGeneratedClass == nullptr)
	{
		return;
	}
	const FString Kind = K2Keys::KindOf(UK2Node_CustomEvent::StaticClass());
	for (TFieldIterator<FMulticastDelegateProperty> It(Blueprint.SkeletonGeneratedClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		if (It->HasAllPropertyFlags(CPF_BlueprintAssignable) && It->SignatureFunction != nullptr)
		{
			Out.Add({TEXT("K2:") + Kind + TEXT(":") + K2Keys::OwnerPath(It->GetOwnerStruct()) + TEXT(":") + It->GetName(), *It});
		}
	}
}

UBlueprint* BlueprintOf(UObject* Asset, UEdGraph* Graph)
{
	if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
	{
		return Blueprint;
	}
	return Graph != nullptr ? FBlueprintEditorUtils::FindBlueprintForGraph(Graph) : nullptr;
}

FString AccessToString(int32 Flags)
{
	if ((Flags & FUNC_Private) != 0)
	{
		return TEXT("private");
	}
	if ((Flags & FUNC_Protected) != 0)
	{
		return TEXT("protected");
	}
	return TEXT("public");
}

// A pin whose type the user picks on the pin itself (DescribeNode extras "typed_pins").
bool IsTypeSelectorPin(const UEdGraphNode& Node, const UEdGraphPin& Pin)
{
	const UK2Node_Select* Select = Cast<UK2Node_Select>(&Node);
	// UK2Node_Select::GetIndexPinUnchecked.
	return Select != nullptr && Select->FindPin(TEXT("Index")) == &Pin;
}

FValue MakeSignatureFlags(const UK2Node_FunctionEntry& Entry)
{
	const int32 Flags = Entry.GetFunctionFlags();
	return FObjectBuilder()
		.AddBool(TEXT("pure"), (Flags & FUNC_BlueprintPure) != 0)
		.AddBool(TEXT("const"), (Flags & FUNC_Const) != 0)
		.AddBool(TEXT("call_in_editor"), Entry.MetaData.bCallInEditor)
		.AddString(TEXT("access"), AccessToString(Flags))
		.AddString(TEXT("category"), Entry.MetaData.Category.ToString())
		.AddString(TEXT("tooltip"), Entry.MetaData.ToolTip.ToString())
		.AddString(TEXT("keywords"), Entry.MetaData.Keywords.ToString())
		.Build();
}

// A colour as the Details panel edits it, its channels rounded to 6 decimals (float noise left out).
FValue K2CommentBoxColor(const FLinearColor& Color)
{
	auto Channel = [](float Value) { return MakeNumber(FMath::RoundToDouble(static_cast<double>(Value) * 1e6) / 1e6); };
	return FObjectBuilder().Add(TEXT("R"), Channel(Color.R)).Add(TEXT("G"), Channel(Color.G)).Add(TEXT("B"), Channel(Color.B))
		.Add(TEXT("A"), Channel(Color.A)).Build();
}

FValue K2CommentBoxEnum(const UObject& Owner, FName PropertyName)
{
	const FProperty* Property = Owner.GetClass()->FindPropertyByName(PropertyName);
	return Property != nullptr ? EncodePropertyValue(*Property, Property->ContainerPtrToValuePtr<void>(&Owner)) : MakeNull();
}

// A comment box's Details-panel settings (UEdGraphNode_Comment, category Comment) and the values a newly
// placed comment box has (UEdGraphNode_Comment::PostPlacedNewNode takes them from the Graph Editor settings).
void K2DescribeCommentBox(const UEdGraphNode_Comment& Comment, FObjectBuilder& Settings, FObjectBuilder& Defaults)
{
	const UGraphEditorSettings* Editor = GetDefault<UGraphEditorSettings>();
	const UEdGraphNode_Comment* Fresh = GetDefault<UEdGraphNode_Comment>();
	Settings.Add(TEXT("CommentColor"), K2CommentBoxColor(Comment.CommentColor));
	Defaults.Add(TEXT("CommentColor"), K2CommentBoxColor(Editor->DefaultCommentNodeTitleColor));
	Settings.AddInt(TEXT("FontSize"), Comment.FontSize);
	Defaults.AddInt(TEXT("FontSize"), Fresh->FontSize);
	Settings.AddBool(TEXT("bCommentBubbleVisible_InDetailsPanel"), Comment.bCommentBubbleVisible_InDetailsPanel);
	Defaults.AddBool(TEXT("bCommentBubbleVisible_InDetailsPanel"), Editor->bShowCommentBubbleWhenZoomedOut);
	Settings.AddBool(TEXT("bColorCommentBubble"), Comment.bColorCommentBubble);
	Defaults.AddBool(TEXT("bColorCommentBubble"), Fresh->bColorCommentBubble);
	Settings.Add(TEXT("MoveMode"), K2CommentBoxEnum(Comment, GET_MEMBER_NAME_CHECKED(UEdGraphNode_Comment, MoveMode)));
	Defaults.Add(TEXT("MoveMode"), K2CommentBoxEnum(*Editor, GET_MEMBER_NAME_CHECKED(UGraphEditorSettings, DefaultCommentNodeMoveMode)));
}

FString AuthoredName(const UEdGraphNode& Node)
{
	if (const UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(&Node))
	{
		return CustomEvent->CustomFunctionName.ToString();
	}
	if (Node.IsA<UEdGraphNode_Comment>())
	{
		return Node.NodeComment;
	}
	return Node.GetNodeTitle(ENodeTitleType::EditableTitle).ToString();
}

UK2Node_FunctionEntry* FindFunctionEntry(UEdGraph& Graph)
{
	TArray<UK2Node_FunctionEntry*> Entries;
	Graph.GetNodesOfClass(Entries);
	return Entries.Num() > 0 ? Entries[0] : nullptr;
}

// FBaseBlueprintGraphActionDetails::OnParamsChanged.
void RefreshSignatureNode(UK2Node_EditablePinBase& Node)
{
	const bool bPrevious = Node.bDisableOrphanPinSaving;
	Node.bDisableOrphanPinSaving = true;
	Node.ReconstructNode();
	Node.bDisableOrphanPinSaving = bPrevious;
	GetDefault<UEdGraphSchema_K2>()->HandleParameterDefaultValueChanged(&Node);
}

// "K2:<Kind>:<OwnerPath>..." -> loads OwnerPath when it is an asset (not /Script/)
// and registers its Blueprint actions with the action database.
void LoadKeyOwnerBlueprint(const FString& Key)
{
	TArray<FString> Parts;
	Key.ParseIntoArray(Parts, TEXT(":"));
	if (Parts.Num() < 3 || !Parts[2].StartsWith(TEXT("/")) || Parts[2].StartsWith(TEXT("/Script/")))
	{
		return;
	}
	FString OwnerPath = Parts[2];
	int32 At = INDEX_NONE;
	if (OwnerPath.FindChar(TEXT('@'), At))
	{
		OwnerPath.LeftInline(At);
	}
	UObject* Owner = FSoftObjectPath(OwnerPath).TryLoad();
	UBlueprint* OwnerBlueprint = Cast<UBlueprint>(Owner);
	if (OwnerBlueprint == nullptr)
	{
		if (const UClass* Class = Cast<UClass>(Owner))
		{
			OwnerBlueprint = Cast<UBlueprint>(Class->ClassGeneratedBy);
		}
	}
	if (OwnerBlueprint != nullptr)
	{
		FBlueprintActionDatabase::Get().RefreshAssetActions(OwnerBlueprint);
	}
	else if (Owner != nullptr && !Owner->IsA<UClass>())
	{
		// An asset a node names (an Enhanced Input action): its actions are filed under it.
		FBlueprintActionDatabase::Get().RefreshAssetActions(Owner);
	}
}

// ---- Purity ("Convert to pure Get" / "Show Exec pins") -------------------------------------------
// Both node context menu entries are bound to private members (UK2Node_VariableGet::TogglePurity,
// UK2Node_CallFunction::ToggleNodePurityOverride) and FToolMenuEntry keeps its action private, so the
// command is run as its body: Modify, set the reflected variation / override, ReconstructNode.
bool SetReflectedEnum(UObject& Object, FName PropertyName, int64 Value)
{
	FProperty* Property = Object.GetClass()->FindPropertyByName(PropertyName);
	void* Memory = Property != nullptr ? Property->ContainerPtrToValuePtr<void>(&Object) : nullptr;
	if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
	{
		EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(Memory, Value);
		return true;
	}
	if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property); Numeric != nullptr && Numeric->IsInteger())
	{
		Numeric->SetIntPropertyValue(Memory, Value);
		return true;
	}
	return false;
}

// The variation a Variable Get offers besides pure (K2Node_VariableGetImpl::GetNodeVariation): Branch for
// a bool, Validated Get for an object / class / soft reference, none (pure) otherwise or for a container.
EGetNodeVariation SupportedVariation(const UK2Node_VariableGet& Get)
{
	const UEdGraphPin* Value = Get.GetValuePin();
	if (Value == nullptr || Value->PinType.IsContainer())
	{
		return EGetNodeVariation::Pure;
	}
	const FName Category = Value->PinType.PinCategory;
	if (Category == UEdGraphSchema_K2::PC_Boolean)
	{
		return EGetNodeVariation::Branch;
	}
	return Category == UEdGraphSchema_K2::PC_Object || Category == UEdGraphSchema_K2::PC_Class
		|| Category == UEdGraphSchema_K2::PC_SoftObject || Category == UEdGraphSchema_K2::PC_SoftClass
		? EGetNodeVariation::ValidatedObject : EGetNodeVariation::Pure;
}

// UK2Node_VariableGet::GetNodeContextMenuActions shows the toggle in a graph allowing impure functions.
bool VariableGetCanTogglePurity(const UK2Node_VariableGet& Get)
{
	const UEdGraph* Graph = Get.GetGraph();
	const UEdGraphSchema_K2* Schema = Graph != nullptr ? Cast<UEdGraphSchema_K2>(Graph->GetSchema()) : nullptr;
	return Schema != nullptr && Schema->DoesGraphSupportImpureFunctions(Graph) && SupportedVariation(Get) != EGetNodeVariation::Pure;
}

// UK2Node_CallFunction::FunctionHasOutputs / CanToggleNodePurity (both not callable from here).
bool CallCanTogglePurity(const UK2Node_CallFunction& Call)
{
	if (Call.IsA<UK2Node_PromotableOperator>())
	{
		return false;  // UK2Node_PromotableOperator::CanToggleNodePurity
	}
	bool bHasOutputs = false;
	if (const UFunction* Function = Call.GetTargetFunction())
	{
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm))
			{
				bHasOutputs = true;
				break;
			}
		}
	}
	return bHasOutputs && (Call.bDefaultsToPureFunc != 0 || GetDefault<UBlueprintEditorProjectSettings>()->bAllowImpureToPureNodeConversion);
}

// Whether Node reports "Pure" (DescribeNode extras properties / property_defaults) and its default.
bool DescribePurity(const UEdGraphNode& Node, bool& bOutPure, bool& bOutDefault)
{
	if (const UK2Node_VariableGet* Get = Cast<UK2Node_VariableGet>(&Node))
	{
		if (!VariableGetCanTogglePurity(*Get))
		{
			return false;
		}
		bOutPure = Get->IsNodePure();
		bOutDefault = true;
		return true;
	}
	const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(&Node);
	if (Call == nullptr || !CallCanTogglePurity(*Call))
	{
		return false;
	}
	bOutPure = Call->IsNodePure();
	bOutDefault = Call->bDefaultsToPureFunc != 0;
	return true;
}

// "Convert to Validated Get / Branch / pure Get" or "Show / Hide Exec pins".
bool TogglePurity(UEdGraphNode& Node)
{
	if (UK2Node_VariableGet* Get = Cast<UK2Node_VariableGet>(&Node))
	{
		if (!VariableGetCanTogglePurity(*Get))
		{
			return false;
		}
		const EGetNodeVariation Pending = Get->IsNodePure() ? SupportedVariation(*Get) : EGetNodeVariation::Pure;
		Get->Modify();
		if (!SetReflectedEnum(*Get, TEXT("CurrentVariation"), static_cast<int64>(Pending)))
		{
			return false;
		}
		Get->ReconstructNode();
		return true;
	}
	UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(&Node);
	if (Call == nullptr || !CallCanTogglePurity(*Call))
	{
		return false;
	}
	// ToggleNodePurityOverride: Unset -> the opposite of the default; Pure <-> Impure.
	const bool bPureNow = Call->IsNodePure();
	Call->Modify();
	if (!SetReflectedEnum(*Call, TEXT("NodePurityOverride"),
		static_cast<int64>(bPureNow ? ENodePurityOverride::Impure : ENodePurityOverride::Pure)))
	{
		return false;
	}
	Call->ReconstructNode();
	return true;
}

// ---- Set Members in <struct> ("Fields") ----------------------------------------------------------
TArray<FString> SetFieldsShown(const UK2Node_SetFieldsInStruct& Node)
{
	TArray<FString> Shown;
	for (const FOptionalPinFromProperty& Field : Node.ShowPinForProperties)
	{
		if (Field.bShowPin)
		{
			Shown.Add(Field.PropertyName.ToString());
		}
	}
	return Shown;
}

// The Details panel's member checkboxes (ShowPinForProperties[i].bShowPin): exactly the listed members get pins.
// The Create Event node's function combo (SGraphNodeK2CreateDelegate::OnFunctionSelected): SetFunction, then
// HandleAnyChange, which keeps the name while the delegate pin is linked (the document sets it after the
// links) and clears it on an unlinked node.
bool SetDelegateFunction(UK2Node_CreateDelegate& Node, const FValue& Value, FAssetDocumentGraphError& OutError)
{
	if (Value.Kind != EAssetDocumentRuntimeValueKind::String)
	{
		OutError.Set(TEXT("Graph.InvalidArgument"), TEXT("Function is the name of a function or event (\"\" for none)."), TEXT("/value"));
		return false;
	}
	const FName Wanted = Value.StringValue.IsEmpty() ? NAME_None : FName(*Value.StringValue);
	Node.Modify();
	Node.SetFunction(Wanted);
	Node.HandleAnyChange(true);
	if (Node.GetFunctionName() != Wanted)
	{
		OutError.Set(TEXT("Graph.PropertyNotEditable"), FString::Printf(
			TEXT("Create Event keeps no function '%s' while its delegate pin is unlinked; link OutputDelegate first."),
			*Value.StringValue), TEXT("/value"));
		return false;
	}
	return true;
}

bool SetSetFieldsShown(UK2Node_SetFieldsInStruct& Node, const FValue& Value, FAssetDocumentGraphError& OutError)
{
	TSet<FName> Wanted;
	if (Value.Kind != EAssetDocumentRuntimeValueKind::Array)
	{
		OutError.Set(TEXT("Graph.InvalidArgument"), TEXT("Fields is an array of member pin names."), TEXT("/value"));
		return false;
	}
	for (int32 Index = 0; Index < Value.Elements.Num(); ++Index)
	{
		const FValue& Element = Value.Elements[Index];
		const FName Name = Element.Kind == EAssetDocumentRuntimeValueKind::String ? FName(*Element.StringValue) : NAME_None;
		if (Name.IsNone() || !Node.ShowPinForProperties.ContainsByPredicate(
			[Name](const FOptionalPinFromProperty& Field) { return Field.PropertyName == Name; }))
		{
			TArray<FString> Members;
			for (const FOptionalPinFromProperty& Field : Node.ShowPinForProperties)
			{
				Members.Add(Field.PropertyName.ToString());
			}
			OutError.Set(TEXT("Graph.PropertyNotEditable"), FString::Printf(TEXT("'%s' is not a member of this struct. Members: %s."),
				Element.Kind == EAssetDocumentRuntimeValueKind::String ? *Element.StringValue : TEXT("(not a string)"),
				*FString::Join(Members, TEXT(", "))), *FString::Printf(TEXT("/value/%d"), Index));
			return false;
		}
		Wanted.Add(Name);
	}
	// As the Details panel's checkbox edits it: a bShowPin change caches the shown pins, then
	// UK2Node_StructMemberSet::PostEditChangeProperty reconstructs the node with the new member pins.
	FProperty* Property = FOptionalPinFromProperty::StaticStruct()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(FOptionalPinFromProperty, bShowPin));
	Node.Modify();
	Node.PreEditChange(Property);
	for (FOptionalPinFromProperty& Field : Node.ShowPinForProperties)
	{
		Field.bShowPin = Wanted.Contains(Field.PropertyName);
	}
	FPropertyChangedEvent Changed(Property, EPropertyChangeType::ValueSet);
	Node.PostEditChangeProperty(Changed);
	return true;
}

// ---- Promotable operators (Add, Multiply, Less, ...) -------------------------------------------
// The pins whose type the right-click "Pin Conversions" menu changes (UK2Node_PromotableOperator::
// CreateConversionMenu): every operand and the result, not the error tolerance nor a comparison's bool.
bool IsPromotableTypePin(const UK2Node_PromotableOperator& Operator, const UEdGraphPin& Pin)
{
	if (Pin.ParentPin != nullptr || Pin.PinType.PinCategory == UEdGraphSchema_K2::PC_Exec || Pin.PinName == TEXT("ErrorTolerance"))
	{
		return false;
	}
	return Pin.Direction == EGPD_Input || !FTypePromotion::IsComparisonOpName(Operator.GetOperationName());
}

// A type the "Pin Conversions" menu offers for Pin: any parameter type of the operator's functions when
// the pin is a wildcard, else one the pin's type promotes to or from.
bool IsOfferedConversion(const UK2Node_PromotableOperator& Operator, const UEdGraphPin& Pin, const FEdGraphPinType& Type)
{
	TArray<UFunction*> Functions;
	FTypePromotion::GetAllFuncsForOp(Operator.GetOperationName(), Functions);
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	const bool bWildcard = Pin.PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard;
	for (const UFunction* Function : Functions)
	{
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			FEdGraphPinType ParamType;
			if (Schema->ConvertPropertyToPinType(*It, ParamType) && ParamType == Type
				&& (bWildcard || FTypePromotion::IsValidPromotion(ParamType, Pin.PinType) || FTypePromotion::IsValidPromotion(Pin.PinType, ParamType)))
			{
				return true;
			}
		}
	}
	return false;
}

// ---- Function local variables (My Blueprint > Local Variables) --------------------------------
UFunction* LocalVariableScope(UBlueprint& Blueprint, const UEdGraph& Graph)
{
	return Blueprint.SkeletonGeneratedClass != nullptr ? Blueprint.SkeletonGeneratedClass->FindFunctionByName(Graph.GetFName()) : nullptr;
}

FValue MakeLocalVariables(const UK2Node_FunctionEntry& Entry)
{
	TArray<FValue> Rows;
	for (const FBPVariableDescription& Variable : Entry.LocalVariables)
	{
		const FString Category = Variable.Category.ToString();
		Rows.Add(FObjectBuilder()
			.AddString(TEXT("name"), Variable.VarName.ToString())
			.Add(TEXT("type"), MakePinTypeValue(Variable.VarType))
			.AddString(TEXT("default"), Variable.DefaultValue)
			.AddString(TEXT("category"), Category == UEdGraphSchema_K2::VR_DefaultCategory.ToString() ? FString() : Category)
			.AddString(TEXT("tooltip"), Variable.HasMetaData(FBlueprintMetadata::MD_Tooltip) ? Variable.GetMetaData(FBlueprintMetadata::MD_Tooltip) : FString())
			.Build());
	}
	return MakeArray(MoveTemp(Rows));
}

struct FWantedLocal
{
	FName Name;
	FEdGraphPinType Type;
	FString Default;
	FString Category;
	FString Tooltip;
};

// The editor's My Blueprint / Details edits of a function's local variables (FBlueprintEditorUtils
// Add / Remove / ChangeLocalVariableType, the default value box, Category and Description), then
// the list order. Value: [{name, type, default?, category?, tooltip?}].
bool SetLocalVariables(UK2Node_FunctionEntry& Entry, const FValue& Value, FAssetDocumentGraphError& OutError)
{
	UEdGraph* Graph = Entry.GetGraph();
	UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForNode(&Entry);
	if (Graph == nullptr || Blueprint == nullptr || !FBlueprintEditorUtils::DoesSupportLocalVariables(Graph))
	{
		OutError.Set(TEXT("Graph.GraphOperationUnsupported"), TEXT("Only function graphs have local variables."), TEXT("/value"));
		return false;
	}
	if (Value.Kind != EAssetDocumentRuntimeValueKind::Array)
	{
		OutError.Set(TEXT("Graph.InvalidArgument"), TEXT("LocalVariables is an array of {name, type, default, category, tooltip}."), TEXT("/value"));
		return false;
	}
	TArray<FWantedLocal> Wanted;
	for (int32 Index = 0; Index < Value.Elements.Num(); ++Index)
	{
		FReader Row(Value.Elements[Index], FString::Printf(TEXT("/value/%d"), Index), OutError);
		FString Name;
		const FValue* TypeValue = nullptr;
		FWantedLocal& Local = Wanted.AddDefaulted_GetRef();
		if (!Row.IsObject() || !Row.OnlyFields({TEXT("name"), TEXT("type"), TEXT("default"), TEXT("category"), TEXT("tooltip")})
			|| !Row.String(TEXT("name"), Name) || !Row.Object(TEXT("type"), TypeValue)
			|| !Row.String(TEXT("default"), Local.Default, false) || !Row.String(TEXT("category"), Local.Category, false)
			|| !Row.String(TEXT("tooltip"), Local.Tooltip, false))
		{
			return false;
		}
		FReader TypeReader = Row.Child(TEXT("type"), *TypeValue);
		if (!ReadPinType(TypeReader, Local.Type))
		{
			return false;
		}
		Local.Name = FName(*Name);
		if (Name.IsEmpty() || Wanted.FilterByPredicate([&Local](const FWantedLocal& Other) { return Other.Name == Local.Name; }).Num() > 1)
		{
			return Row.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("Local variable names must be non-empty and unique ('%s')."), *Name), TEXT("name"));
		}
	}
	auto Scope = [Blueprint, Graph]() { return LocalVariableScope(*Blueprint, *Graph); };
	if (Scope() == nullptr)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}
	TArray<FName> Removed;
	for (const FBPVariableDescription& Existing : Entry.LocalVariables)
	{
		if (!Wanted.ContainsByPredicate([&Existing](const FWantedLocal& Local) { return Local.Name == Existing.VarName; }))
		{
			Removed.Add(Existing.VarName);
		}
	}
	for (const FName Name : Removed)
	{
		FBlueprintEditorUtils::RemoveLocalVariable(Blueprint, Scope(), Name);
	}
	for (const FWantedLocal& Local : Wanted)
	{
		const FBPVariableDescription* Existing = Entry.LocalVariables.FindByPredicate(
			[&Local](const FBPVariableDescription& Variable) { return Variable.VarName == Local.Name; });
		if (Existing == nullptr)
		{
			if (!FBlueprintEditorUtils::AddLocalVariable(Blueprint, Graph, Local.Name, Local.Type, Local.Default))
			{
				OutError.Set(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("Local variable '%s' could not be added."), *Local.Name.ToString()), TEXT("/value"));
				return false;
			}
			continue;
		}
		if (Existing->VarType != Local.Type)
		{
			// The document states the new type: the editor's confirmed "Change Variable Type" path, without its dialog.
			const EditorConfirmations::FScopedConfirmedDialog Confirmed(EditorConfirmations::ChangeVariableType);
			FBlueprintEditorUtils::ChangeLocalVariableType(Blueprint, Scope(), Local.Name, Local.Type);
		}
	}
	Entry.Modify();
	bool bChanged = false;
	for (const FWantedLocal& Local : Wanted)
	{
		FBPVariableDescription* Variable = Entry.LocalVariables.FindByPredicate(
			[&Local](const FBPVariableDescription& Item) { return Item.VarName == Local.Name; });
		if (Variable == nullptr)
		{
			OutError.Set(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("Local variable '%s' was not kept by the editor."), *Local.Name.ToString()), TEXT("/value"));
			return false;
		}
		if (Variable->DefaultValue != Local.Default)
		{
			Variable->DefaultValue = Local.Default;
			bChanged = true;
		}
		const FText Category = Local.Category.IsEmpty() ? UEdGraphSchema_K2::VR_DefaultCategory : FText::FromString(Local.Category);
		if (!Variable->Category.EqualTo(Category))
		{
			Variable->Category = Category;
			bChanged = true;
		}
		const FString Tooltip = Variable->HasMetaData(FBlueprintMetadata::MD_Tooltip) ? Variable->GetMetaData(FBlueprintMetadata::MD_Tooltip) : FString();
		if (Tooltip != Local.Tooltip)
		{
			if (Local.Tooltip.IsEmpty())
			{
				Variable->RemoveMetaData(FBlueprintMetadata::MD_Tooltip);
			}
			else
			{
				Variable->SetMetaData(FBlueprintMetadata::MD_Tooltip, Local.Tooltip);
			}
			bChanged = true;
		}
	}
	// My Blueprint lists the locals in this order (drag to reorder).
	TArray<FName> Order;
	for (const FWantedLocal& Local : Wanted)
	{
		Order.Add(Local.Name);
	}
	TArray<FBPVariableDescription> Sorted = Entry.LocalVariables;
	Sorted.StableSort([&Order](const FBPVariableDescription& A, const FBPVariableDescription& B)
	{
		return Order.IndexOfByKey(A.VarName) < Order.IndexOfByKey(B.VarName);
	});
	for (int32 Index = 0; Index < Sorted.Num(); ++Index)
	{
		if (Sorted[Index].VarName != Entry.LocalVariables[Index].VarName)
		{
			Entry.LocalVariables = MoveTemp(Sorted);
			bChanged = true;
			break;
		}
	}
	if (bChanged || !Removed.IsEmpty())
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}
	// Their get / set actions are keyed by the skeleton function's properties.
	FBlueprintActionDatabase::Get().RefreshAssetActions(Blueprint);
	return true;
}

// ---- Collapsed graphs ----------------------------------------------------------------------
const TCHAR* const CompositeKey = TEXT("K2:Composite");

bool CanPlaceComposite(const UEdGraph& Graph)
{
	const UEdGraphSchema* Schema = Graph.GetSchema();
	return Schema != nullptr && GetDefault<UK2Node_Composite>()->CanCreateUnderSpecifiedSchema(Schema);
}

// FBlueprintEditor::OnAddParentNode ("Add Call to Parent Function" on a function entry or event): a
// call to the parent class's version of a function this graph implements. The palette has no such
// action, so the key "K2:CallParentFunction:<parent owner>:<function>" is resolved here.
const TCHAR* const ParentCallPrefix = TEXT("K2:CallParentFunction:");

bool SpawnParentCall(UBlueprint& Blueprint, const FAssetDocumentGraphSpawnRequest& Request,
	FAssetDocumentGraphSpawnResult& OutResult, FAssetDocumentGraphError& OutError)
{
	FString Rest = Request.ActionKey.RightChop(FCString::Strlen(ParentCallPrefix));
	FString Owner;
	FString FunctionName;
	if (!Rest.Split(TEXT(":"), &Owner, &FunctionName, ESearchCase::CaseSensitive, ESearchDir::FromEnd) || Request.FromPin != nullptr)
	{
		return false;
	}
	// The node that represents the function in this graph: its entry, or its event.
	bool bImplemented = false;
	for (UEdGraphNode* Node : Request.Graph->Nodes)
	{
		if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
		{
			bImplemented |= Request.Graph->GetFName() == FName(*FunctionName) || Entry->FunctionReference.GetMemberName() == FName(*FunctionName);
		}
		else if (const UK2Node_Event* Event = Cast<UK2Node_Event>(Node); Event != nullptr && !Node->IsA<UK2Node_CustomEvent>())
		{
			bImplemented |= Event->GetFunctionName() == FName(*FunctionName);
		}
	}
	UClass* Skeleton = Blueprint.SkeletonGeneratedClass;
	UFunction* Function = Skeleton != nullptr ? Skeleton->FindFunctionByName(FName(*FunctionName)) : nullptr;
	UFunction* Parent = bImplemented && Function != nullptr ? UEdGraphSchema_K2::GetCallableParentFunction(Function) : nullptr;
	if (Parent == nullptr || K2Keys::OwnerPath(Parent->GetOwnerClass()) != Owner)
	{
		return false;
	}
	const FVector2D Location = Request.Position.IsSet() ? Request.Position.GetValue() : FVector2D::ZeroVector;
	Request.Graph->Modify();
	FGraphNodeCreator<UK2Node_CallParentFunction> Creator(*Request.Graph);
	UK2Node_CallParentFunction* Node = Creator.CreateNode();
	Node->SetFromFunction(Parent);
	Node->AllocateDefaultPins();
	Node->NodePosX = FMath::RoundToInt(Location.X);
	Node->NodePosY = FMath::RoundToInt(Location.Y);
	Creator.Finalize();
	OutResult.Node = Node;
	OutResult.bReused = false;
	OutError = FAssetDocumentGraphError();
	return true;
}

// FBlueprintEditor::CollapseNodes without moving nodes into it: the gateway node, renamable, whose new
// bound graph gets a unique "CollapseGraph" name; its entry / exit tunnels hold the signature.
bool SpawnComposite(UBlueprint& Blueprint, const FAssetDocumentGraphSpawnRequest& Request,
	FAssetDocumentGraphSpawnResult& OutResult, FAssetDocumentGraphError& OutError)
{
	if (!CanPlaceComposite(*Request.Graph) || Request.FromPin != nullptr)
	{
		OutError.Set(TEXT("Graph.ActionRequiresContext"), FString::Printf(
			TEXT("Graph %s cannot hold a collapsed graph%s."), *Request.Graph->GetName(),
			Request.FromPin != nullptr ? TEXT(" placed from a pin") : TEXT("")));
		return false;
	}
	const FVector2D Location = Request.Position.IsSet() ? Request.Position.GetValue() : FVector2D::ZeroVector;
	Request.Graph->Modify();
	UK2Node_Composite* Gateway = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_Composite>(Request.Graph, Location, EK2NewNodeFlags::None);
	if (Gateway == nullptr || Gateway->BoundGraph == nullptr)
	{
		OutError.Set(TEXT("Graph.ActionNotFound"), TEXT("The collapsed graph node was not created."));
		return false;
	}
	Gateway->bCanRenameNode = true;
	const FName GraphName = FBlueprintEditorUtils::FindUniqueKismetName(&Blueprint, TEXT("CollapseGraph"));
	const TSharedPtr<INameValidatorInterface> Validator = MakeShareable(new FKismetNameValidator(&Blueprint, GraphName));
	FBlueprintEditorUtils::RenameGraphWithSuggestion(Gateway->BoundGraph, Validator, GraphName.ToString());
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
	OutResult.Node = Gateway;
	OutResult.bReused = false;
	return true;
}

class FK2SchemaAdapter final : public IAssetDocumentGraphSchemaAdapter
{
public:
	virtual UClass* GetSchemaClass() const override
	{
		return UEdGraphSchema_K2::StaticClass();
	}

	virtual FString Tag(const UEdGraph& Graph) const override
	{
		return TEXT("K2");
	}

	virtual bool EnumerateActions(
		const FAssetDocumentGraphActionQuery& Query,
		TArray<FAssetDocumentGraphActionRecord>& OutActions,
		FAssetDocumentGraphError& OutError) const override
	{
		UBlueprint* Blueprint = BlueprintOf(Query.Asset, Query.Graph);
		if (Blueprint == nullptr || Query.Graph == nullptr)
		{
			OutError.Set(TEXT("Graph.GraphOperationUnsupported"), TEXT("K2 actions need a Blueprint-owned graph."));
			return false;
		}
		// The engine's standard macro library is part of every K2 menu in the
		// editor; make sure its actions are registered before listing.
		static bool bStandardMacrosLoaded = false;
		if (!bStandardMacrosLoaded)
		{
			bStandardMacrosLoaded = true;
			LoadKeyOwnerBlueprint(TEXT("K2:MacroInstance:/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));
		}
		TArray<FK2Candidate> Candidates;
		CollectCandidates(*Blueprint, *Query.Graph, Candidates);
		FBlueprintActionFilter Filter(FBlueprintActionFilter::BPFILTER_RejectIncompatibleThreadSafety);
		InitFilter(Filter, *Blueprint, *Query.Graph, Query.FromPin);
		for (UClass* Target : Query.TargetClasses)
		{
			if (Target != nullptr)
			{
				FBlueprintActionFilter::AddUnique(Filter.TargetClasses, Target);
			}
		}
		// The menu with Context Sensitive off: no target class, so members of any class pass.
		FBlueprintActionFilter ContextFree(FBlueprintActionFilter::BPFILTER_NoFlags);
		ContextFree.Context.Blueprints.Add(Blueprint);
		ContextFree.Context.Graphs.Add(Query.Graph);
		TArray<FString> Words;
		Query.Text.ParseIntoArrayWS(Words);
		const TSet<FString> Exact(Query.ExactKeys);
		for (const FK2Candidate& Candidate : Candidates)
		{
			if (!Query.KeyPrefix.IsEmpty() && !Candidate.Key.StartsWith(Query.KeyPrefix))
			{
				continue;
			}
			const bool bExact = Exact.Contains(Candidate.Key);
			if (!Exact.IsEmpty() && !bExact)
			{
				continue;
			}
			const bool bAllowed = IsAllowed(Filter, Candidate);
			if (!bAllowed && !bExact && !(Query.bContextFreeCalls && IsContextFreeCall(ContextFree, Candidate, Words)))
			{
				continue;
			}
			FAssetDocumentGraphActionRecord& Record = OutActions.AddDefaulted_GetRef();
			Record.ActionKey = Candidate.Key;
			Record.NodeClassPath = Candidate.Spawner->NodeClass->GetPathName();
			Record.bRequiresContext = !bAllowed;
			if (Query.bWantUiText || !Query.Text.IsEmpty())
			{
				const FBlueprintActionUiSpec& Ui = Candidate.Spawner->PrimeDefaultUiSpec(Query.Graph);
				Record.DisplayName = Ui.MenuName.ToString();
				Record.Category = Ui.Category.ToString();
				Record.Tooltip = Ui.Tooltip.ToString();
				if (Candidate.bBound && Candidate.Bindings.Num() == 1)
				{
					Record.DisplayName += FString::Printf(TEXT(" (%s)"),
						*Candidate.Bindings.Array()[0].GetFName().ToString());
				}
			}
		}
		if (CanPlaceComposite(*Query.Graph) && (Query.KeyPrefix.IsEmpty() || FString(CompositeKey).StartsWith(Query.KeyPrefix))
			&& (Exact.IsEmpty() || Exact.Contains(CompositeKey)))
		{
			FAssetDocumentGraphActionRecord& Record = OutActions.AddDefaulted_GetRef();
			Record.ActionKey = CompositeKey;
			Record.NodeClassPath = UK2Node_Composite::StaticClass()->GetPathName();
			if (Query.bWantUiText || !Query.Text.IsEmpty())
			{
				Record.DisplayName = TEXT("Collapsed Graph");
				Record.Category = TEXT("Utilities");
				Record.Tooltip = TEXT("An empty collapsed graph (the node \"Collapse Nodes\" makes); its subgraph holds the nodes.");
			}
		}
		TArray<FDispatcherEvent> Events;
		CollectDispatcherEvents(*Blueprint, *Query.Graph, Events);
		for (const FDispatcherEvent& Event : Events)
		{
			if ((!Query.KeyPrefix.IsEmpty() && !Event.Key.StartsWith(Query.KeyPrefix)) || (!Exact.IsEmpty() && !Exact.Contains(Event.Key)))
			{
				continue;
			}
			FAssetDocumentGraphActionRecord& Record = OutActions.AddDefaulted_GetRef();
			Record.ActionKey = Event.Key;
			Record.NodeClassPath = UK2Node_CustomEvent::StaticClass()->GetPathName();
			if (Query.bWantUiText || !Query.Text.IsEmpty())
			{
				Record.DisplayName = FString::Printf(TEXT("Event (%s)"), *Event.Delegate->GetName());
				Record.Category = TEXT("Event Dispatchers");
				Record.Tooltip = FString::Printf(TEXT("Create event with the %s signature"), *Event.Delegate->GetName());
			}
		}
		return true;
	}

	virtual bool ResolveAndSpawn(
		const FAssetDocumentGraphSpawnRequest& Request,
		FAssetDocumentGraphSpawnResult& OutResult,
		FAssetDocumentGraphError& OutError) const override
	{
		UBlueprint* Blueprint = BlueprintOf(Request.Asset, Request.Graph);
		if (Blueprint == nullptr || Request.Graph == nullptr)
		{
			OutError.Set(TEXT("Graph.GraphOperationUnsupported"), TEXT("K2 actions need a Blueprint-owned graph."));
			return false;
		}
		if (Request.ActionKey.StartsWith(TEXT("K2:CustomEvent:")))
		{
			return SpawnDispatcherEvent(*Blueprint, Request, OutResult, OutError);
		}
		if (Request.ActionKey == CompositeKey)
		{
			return SpawnComposite(*Blueprint, Request, OutResult, OutError);
		}
		TArray<FK2Candidate> Candidates;
		const FK2Candidate* Match = FindCandidate(*Blueprint, *Request.Graph, Request.ActionKey, Candidates, OutError);
		if (Match == nullptr && OutError.Code == TEXT("Graph.ActionNotFound"))
		{
			// Members created earlier in the same apply are only known after a refresh (§8).
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			FBlueprintActionDatabase::Get().RefreshAssetActions(Blueprint);
			// Keys name their owner asset: load a not-yet-loaded owner Blueprint
			// (macro library, Blueprint class...) so its actions get registered.
			LoadKeyOwnerBlueprint(Request.ActionKey);
			OutError = FAssetDocumentGraphError();
			Match = FindCandidate(*Blueprint, *Request.Graph, Request.ActionKey, Candidates, OutError);
		}
		if (Match == nullptr)
		{
			return Request.ActionKey.StartsWith(ParentCallPrefix) && OutError.Code == TEXT("Graph.ActionNotFound")
				&& SpawnParentCall(*Blueprint, Request, OutResult, OutError);
		}
		FBlueprintActionFilter Filter(FBlueprintActionFilter::BPFILTER_RejectIncompatibleThreadSafety);
		InitFilter(Filter, *Blueprint, *Request.Graph, Request.FromPin);
		// Outside the palette the editor still places a node by Paste (or by dragging a local variable from
		// My Blueprint): an editor-only call in a runtime graph, a member the palette hides (a deprecated
		// function's locals, a call another class's palette filters). Such a node is kept when the graph
		// accepts it as a paste (UEdGraphNode::CanPasteHere, the editor's own paste check).
		const bool bInPalette = IsAllowed(Filter, *Match) || (Request.FromPin == nullptr
			&& (IsInFullPalette(*Blueprint, *Request.Graph, *Match) || IsImplementableEvent(*Blueprint, *Request.Graph, *Match)));
		const auto Refuse = [&Request, &OutError, &Candidates, &Filter, Match, Blueprint]()
		{
			// The same member under the key this graph offers (K2:BeginPlay is the Actor's event; a component's
			// Begin Play is K2:Event:/Script/Engine.ActorComponent:ReceiveBeginPlay), or the same menu entry.
			const FName Member = ActionMemberName(*Match->Spawner);
			const FString MenuName = Match->Spawner->PrimeDefaultUiSpec(Request.Graph).MenuName.ToString();
			TArray<FString> Alternatives;
			for (const FK2Candidate& Candidate : Candidates)
			{
				if (&Candidate == Match || Candidate.Spawner == nullptr || Candidate.Spawner->NodeClass != Match->Spawner->NodeClass
					|| Alternatives.Num() >= 5)
				{
					continue;
				}
				const bool bSame = Member != NAME_None ? ActionMemberName(*Candidate.Spawner) == Member
					: (!MenuName.IsEmpty() && Candidate.Spawner->PrimeDefaultUiSpec(Request.Graph).MenuName.ToString() == MenuName);
				if (bSame && (IsAllowed(Filter, Candidate) || (Request.FromPin == nullptr && IsInFullPalette(*Blueprint, *Request.Graph, Candidate))))
				{
					Alternatives.Add(Candidate.Key);
				}
			}
			const FString Use = Alternatives.IsEmpty() ? FString()
				: FString::Printf(TEXT(" This graph offers the same action as %s."), *FString::Join(Alternatives, TEXT(", ")));
			OutError.Set(TEXT("Graph.ActionRequiresContext"), Request.FromPin != nullptr
				? FString::Printf(TEXT("Action %s exists but is not available in graph %s from the given pin (the editor's context menu for that pin filters it out). "
					"Use an action listed by graph.actions.enumerate.v1 with the same context, or spawn without context.from_pin."),
					*Request.ActionKey, *Request.Graph->GetName())
				: FString::Printf(TEXT("Action %s exists but cannot be placed in graph %s (the editor's palette filters it out for this graph, "
					"even with Context Sensitive off, and the graph does not accept it as a paste)."),
					*Request.ActionKey, *Request.Graph->GetName()));
			if (!Alternatives.IsEmpty())
			{
				OutError.Message += Use;
				OutError.AddContext(TEXT("use"), MakeStringArray(Alternatives));
			}
			return false;
		};
		if (!bInPalette && Request.FromPin != nullptr)
		{
			return Refuse();
		}
		const FVector2D Location = Request.Position.IsSet() ? Request.Position.GetValue() : DefaultLocation(*Request.Graph);

		// One event node per event per Blueprint: reuse an existing (ghost) node.
		if (const UBlueprintEventNodeSpawner* EventSpawner = bInPalette ? Cast<UBlueprintEventNodeSpawner>(Match->Spawner) : nullptr)
		{
			if (const UK2Node_Event* Existing = EventSpawner->FindPreExistingEvent(Blueprint, Match->Bindings))
			{
				UK2Node_Event* Reused = const_cast<UK2Node_Event*>(Existing);
				RawEdits::ConvertGhostNodeToRealNode(*Reused);
				if (Request.Position.IsSet())
				{
					RawEdits::SetNodePosition(*Reused, FMath::RoundToInt(Location.X), FMath::RoundToInt(Location.Y));
				}
				OutResult.Node = Reused;
				OutResult.bReused = true;
				return true;
			}
		}
		TSet<UEdGraphNode*> Before(Request.Graph->Nodes);
		UEdGraphNode* Node = Match->Spawner->Invoke(Request.Graph, Match->Bindings, Location);
		if (Node == nullptr)
		{
			OutError.Set(TEXT("Graph.ActionNotFound"),
				FString::Printf(TEXT("Action %s did not produce a node."), *Request.ActionKey));
			return false;
		}
		if (!bInPalette && !Node->CanPasteHere(Request.Graph))
		{
			// The editor's own reason (UK2Node_CallFunction::CanPasteHere): an editor-only function (one of
			// an editor module, or of a Blueprint class derived from an editor-only class) is never placed
			// in a runtime Blueprint's graph, by palette or paste.
			const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
			const UFunction* Function = Call != nullptr ? Call->GetTargetFunction() : nullptr;
			const bool bEditorOnly = Function != nullptr && !UK2Node_CallFunction::CanEditorOnlyFunctionBeCalled(Function, Request.Graph);
			// That check asks the graph's outer for the Blueprint, which a collapsed graph's is not: the editor
			// places such a call where its Blueprint graph accepts it and collapses it (LandmassBrushManager's
			// "Shape Drawing"). Its top-level graph decides.
			UEdGraph* TopLevel = FBlueprintEditorUtils::GetTopLevelGraph(Request.Graph);
			if (bEditorOnly && TopLevel != nullptr && TopLevel != Request.Graph && Node->CanPasteHere(TopLevel))
			{
				OutResult.Node = Node;
				OutResult.bReused = Before.Contains(Node) || Node->GetGraph() != Request.Graph;
				return true;
			}
			if (!Before.Contains(Node))
			{
				FBlueprintEditorUtils::RemoveNode(Blueprint, Node, /*bDontRecompile=*/true);
			}
			if (bEditorOnly)
			{
				OutError.Set(TEXT("Graph.EditorOnlyFunction"), FString::Printf(
					TEXT("Action %s calls the editor-only function %s, which the editor does not place in graph %s of a runtime Blueprint "
						"(\"Cannot paste editor only functions into runtime graphs\")."),
					*Request.ActionKey, *Function->GetPathName(), *Request.Graph->GetName()));
				return false;
			}
			return Refuse();
		}
		OutResult.Node = Node;
		OutResult.bReused = Before.Contains(Node) || Node->GetGraph() != Request.Graph;
		if (!OutResult.bReused && Request.FromPin != nullptr)
		{
			Node->AutowireNewNode(Request.FromPin);
		}
		return true;
	}

	virtual void DescribeNode(const UEdGraphNode& Node, FAssetDocumentGraphNodeDescription& Out) const override
	{
		Out.ActionKey = K2Keys::ComputeNodeKey(Node);
		FObjectBuilder Extras;
		if (Out.ActionKey.IsEmpty())
		{
			const FString Unresolved = UnresolvedReference(Node);
			if (!Unresolved.IsEmpty())
			{
				Extras.AddString(TEXT("unresolved"), Unresolved);
			}
		}
		if (Node.IsA<UK2Node_FunctionEntry>())
		{
			Out.ImplicitRole = TEXT("entry");
		}
		else if (Node.IsA<UK2Node_FunctionResult>())
		{
			Out.ImplicitRole = TEXT("result");
		}
		else if (const UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(&Node); Tunnel != nullptr && Tunnel->GetClass() == UK2Node_Tunnel::StaticClass())
		{
			Out.ImplicitRole = Tunnel->bCanHaveOutputs && !Tunnel->bCanHaveInputs ? TEXT("entry") : TEXT("result");
		}
		if (Node.IsAutomaticallyPlacedGhostNode())
		{
			Extras.AddBool(TEXT("ghost"), true);
		}
		if (Node.GetCanRenameNode())
		{
			Extras.AddString(TEXT("name"), AuthoredName(Node));
		}
		// User-defined pins exist on Entry/Result/CustomEvent/Tunnel; stock events
		// derive from EditablePinBase too but their signature is fixed.
		const UK2Node_EditablePinBase* Editable = Cast<UK2Node_EditablePinBase>(&Node);
		if (Editable != nullptr && (!Node.IsA<UK2Node_Event>() || Node.IsA<UK2Node_CustomEvent>()))
		{
			TArray<FString> Names;
			for (const TSharedPtr<FUserPinInfo>& Pin : Editable->UserDefinedPins)
			{
				if (Pin.IsValid())
				{
					Names.Add(Pin->PinName.ToString());
				}
			}
			Extras.Add(TEXT("user_pins"), MakeStringArray(Names));
		}
		if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(&Node))
		{
			Extras.Add(TEXT("signature_flags"), MakeSignatureFlags(*Entry));
			if (FBlueprintEditorUtils::DoesSupportLocalVariables(Node.GetGraph()))
			{
				Extras.Add(TEXT("local_variables"), MakeLocalVariables(*Entry));
			}
		}
		if (Node.IsA<UK2Node_Select>())
		{
			if (const UEdGraphPin* Index = Node.FindPin(TEXT("Index")))
			{
				const TArray<FString> TypedPins = {Index->PinName.ToString()};
				Extras.Add(TEXT("typed_pins"), MakeStringArray(TypedPins));
			}
		}
		if (const UK2Node_PromotableOperator* Operator = Cast<UK2Node_PromotableOperator>(&Node))
		{
			// "Pin Conversions" picks these pins' types (operands of differing types, e.g. vector * float).
			TArray<FString> TypedPins;
			bool bWiredToOperator = false;
			for (const UEdGraphPin* Pin : Operator->Pins)
			{
				if (Pin == nullptr)
				{
					continue;
				}
				if (IsPromotableTypePin(*Operator, *Pin))
				{
					TypedPins.Add(Pin->PinName.ToString());
				}
				for (const UEdGraphPin* Linked : Pin->LinkedTo)
				{
					bWiredToOperator |= Linked != nullptr && Linked->GetOwningNodeUnchecked() != nullptr
						&& Linked->GetOwningNodeUnchecked()->IsA<UK2Node_PromotableOperator>();
				}
			}
			Extras.Add(TEXT("typed_pins"), MakeStringArray(TypedPins));
			Extras.AddBool(TEXT("promotable"), true);
			// Wired to another operator: while both are wildcards, the types each one takes depend on the
			// order the links are made, so the document states them.
			Extras.AddBool(TEXT("wired_to_operator"), bWiredToOperator);
		}
		// Adapter-owned node settings (SetNodeSetting), reported with their default values.
		FObjectBuilder Settings;
		FObjectBuilder SettingDefaults;
		bool bSettings = false;
		bool bPure = false;
		bool bPureDefault = false;
		if (DescribePurity(Node, bPure, bPureDefault))
		{
			// "Pure": "Convert to pure Get" / "Show Exec pins".
			Settings.AddBool(TEXT("Pure"), bPure);
			SettingDefaults.AddBool(TEXT("Pure"), bPureDefault);
			bSettings = true;
		}
		if (const UK2Node_SetFieldsInStruct* SetFields = Cast<UK2Node_SetFieldsInStruct>(&Node))
		{
			// "Fields": the members the node sets (the Details panel's per-member checkboxes; none by default).
			Settings.Add(TEXT("Fields"), MakeStringArray(SetFieldsShown(*SetFields)));
			SettingDefaults.Add(TEXT("Fields"), MakeStringArray(TArray<FString>()));
			bSettings = true;
		}
		if (const UK2Node_CreateDelegate* CreateDelegate = Cast<UK2Node_CreateDelegate>(&Node))
		{
			// "Function": the function or event the node's combo box picked (none on a new node).
			const FName Function = CreateDelegate->GetFunctionName();
			Settings.AddString(TEXT("Function"), Function.IsNone() ? FString() : Function.ToString());
			SettingDefaults.AddString(TEXT("Function"), FString());
			bSettings = true;
		}
		if (const UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(&Node))
		{
			// A comment box's colour, font size, bubble and move mode (its Details panel).
			K2DescribeCommentBox(*Comment, Settings, SettingDefaults);
			bSettings = true;
		}
		if (bSettings)
		{
			Extras.Add(TEXT("properties"), Settings.Build());
			Extras.Add(TEXT("property_defaults"), SettingDefaults.Build());
		}
		Out.Extras = Extras.Build();
	}

	virtual UObject* ResolvePropertyContainer(UEdGraphNode& Node, FStringView Container) const override
	{
		if (Container == TEXTVIEW("template"))
		{
			if (const UK2Node_AddComponent* AddComponent = Cast<UK2Node_AddComponent>(&Node))
			{
				return AddComponent->GetTemplateFromNode();
			}
			return nullptr;
		}
		return IAssetDocumentGraphSchemaAdapter::ResolvePropertyContainer(Node, Container);
	}

	virtual bool RemoveNode(UObject& Asset, UEdGraphNode& Node, FAssetDocumentGraphError& OutError) const override
	{
		if (!Node.CanUserDeleteNode())
		{
			OutError.Set(TEXT("Graph.NodeNotDeletable"), FString::Printf(
				TEXT("Node %s (%s) cannot be deleted by the user."),
				*FormatAssetDocumentGraphGuid(Node.NodeGuid), *Node.GetClass()->GetName()));
			return false;
		}
		UBlueprint* Blueprint = BlueprintOf(&Asset, Node.GetGraph());
		if (Blueprint == nullptr)
		{
			OutError.Set(TEXT("Graph.GraphOperationUnsupported"), TEXT("K2 node removal needs a Blueprint."));
			return false;
		}
		// FBlueprintEditor::DeleteSelectedNodes path.
		FBlueprintEditorUtils::RemoveNode(Blueprint, &Node, /*bDontRecompile=*/true);
		return true;
	}

	virtual FString ClassifySubgraph(const UEdGraph& Subgraph, const UEdGraphNode& OwnerNode) const override
	{
		return TEXT("other");
	}

	// A function graph named like a parent class's function or event (ReceiveHit) is the editor's override
	// of it: its entry takes the parent's signature, which no Details panel edits.
	static bool DescribeOverrideSignature(const UEdGraphNode& Node, FAssetDocumentGraphError& OutError)
	{
		const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(&Node);
		const UBlueprint* Blueprint = Entry != nullptr ? FBlueprintEditorUtils::FindBlueprintForNode(Entry) : nullptr;
		const UEdGraph* Graph = Entry != nullptr ? Entry->GetGraph() : nullptr;
		const UFunction* Parent = Blueprint != nullptr && Graph != nullptr && Blueprint->ParentClass != nullptr
			? Blueprint->ParentClass->FindFunctionByName(Graph->GetFName()) : nullptr;
		const UClass* Owner = Parent != nullptr ? Parent->GetOwnerClass() : nullptr;
		if (Owner == nullptr)
		{
			return false;
		}
		const FString OwnerName = Owner->ClassGeneratedBy != nullptr ? Owner->GetName() : FString(Owner->GetPrefixCPP()) + Owner->GetName();
		// The editor's own rule for an overridable function it places as an event (Event Hit for ReceiveHit).
		const bool bEvent = UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(Parent);
		const FString Advice = bEvent
			? FString::Printf(TEXT("To implement the event, place K2:Event:%s:%s in the EventGraph; for a function of your own, pick another name."),
				*K2Keys::OwnerPath(Owner), *Parent->GetName())
			: FString(TEXT("For a function of your own, pick another name."));
		OutError.Set(TEXT("Graph.UserPinsUnsupported"), FString::Printf(
			TEXT("Function '%s' has the same name as parent %s %s::%s, so the editor treats it as an override: it takes the parent's signature, which cannot be edited (leave Signature Inputs/Outputs out). %s"),
			*Graph->GetName(), bEvent ? TEXT("event") : TEXT("function"), *OwnerName, *Parent->GetName(), *Advice));
		OutError.AddContextString(TEXT("overrides"), K2Keys::OwnerPath(Owner) + TEXT(":") + Parent->GetName());
		return true;
	}

	virtual bool SetUserPins(
		UEdGraphNode& Node, TConstArrayView<FAssetDocumentGraphUserPin> Pins, FAssetDocumentGraphError& OutError) const override
	{
		UK2Node_EditablePinBase* Editable = Cast<UK2Node_EditablePinBase>(&Node);
		if (Editable == nullptr || !Editable->IsEditable() || (Node.IsA<UK2Node_Event>() && !Node.IsA<UK2Node_CustomEvent>()))
		{
			if (DescribeOverrideSignature(Node, OutError))
			{
				return false;
			}
			return IAssetDocumentGraphSchemaAdapter::SetUserPins(Node, Pins, OutError);
		}
		TSet<FName> Seen;
		for (const FAssetDocumentGraphUserPin& Pin : Pins)
		{
			if (Seen.Contains(Pin.Name))
			{
				OutError.Set(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("Duplicate user pin '%s'."), *Pin.Name.ToString()), TEXT("/pins"));
				return false;
			}
			Seen.Add(Pin.Name);
		}
		Editable->Modify();
		// A parameter whose name differs only by case is the same parameter: the Details panel's name box
		// (FBlueprintGraphActionDetails::OnParamNameTextCommitted) renames it in place, so links and the
		// getters of the parameter survive. FName equality below would otherwise treat it as unchanged.
		for (const TSharedPtr<FUserPinInfo>& Existing : Editable->UserDefinedPins)
		{
			const FAssetDocumentGraphUserPin* Wanted = Existing.IsValid() ? Pins.FindByPredicate(
				[&Existing](const FAssetDocumentGraphUserPin& Pin) { return Pin.Name == Existing->PinName; }) : nullptr;
			if (Wanted == nullptr || Wanted->Name.ToString().Equals(Existing->PinName.ToString(), ESearchCase::CaseSensitive)
				|| Wanted->Direction != Existing->DesiredPinDirection || Wanted->Type != Existing->PinType)
			{
				continue;
			}
			const FName OldName = Existing->PinName;
			if (Editable->RenameUserDefinedPin(OldName, Wanted->Name) != ERenamePinResult::ERenamePinResult_Success)
			{
				continue;
			}
			Existing->PinName = Wanted->Name;
			if (UEdGraph* Graph = Editable->GetGraph())
			{
				for (UEdGraphNode* GraphNode : Graph->Nodes)
				{
					if (UK2Node_VariableGet* Getter = Cast<UK2Node_VariableGet>(GraphNode))
					{
						if (Getter->ReferencesVariable(OldName, nullptr))
						{
							Getter->HandleVariableRenamed(Editable->GetBlueprint(), Editable->GetBlueprint()->GeneratedClass, Graph, OldName, Wanted->Name);
						}
					}
				}
			}
		}
		// Remove pins that are gone or whose signature changes.
		TArray<FName> ToRemove;
		for (const TSharedPtr<FUserPinInfo>& Existing : Editable->UserDefinedPins)
		{
			const FAssetDocumentGraphUserPin* Wanted = Pins.FindByPredicate(
				[&Existing](const FAssetDocumentGraphUserPin& Pin) { return Pin.Name == Existing->PinName; });
			if (Wanted == nullptr || Wanted->Direction != Existing->DesiredPinDirection || Wanted->Type != Existing->PinType)
			{
				ToRemove.Add(Existing->PinName);
			}
		}
		for (const FName Name : ToRemove)
		{
			Editable->RemoveUserDefinedPinByName(Name);
		}
		for (const FAssetDocumentGraphUserPin& Pin : Pins)
		{
			const bool bExists = Editable->UserDefinedPins.ContainsByPredicate(
				[&Pin](const TSharedPtr<FUserPinInfo>& Existing) { return Existing->PinName == Pin.Name; });
			if (!bExists)
			{
				FText Reason;
				if (!Editable->CanCreateUserDefinedPin(Pin.Type, Pin.Direction, Reason))
				{
					OutError.Set(TEXT("Graph.UserPinsUnsupported"), FString::Printf(
						TEXT("%s cannot have a %s user pin '%s': %s"), *Node.GetClass()->GetName(),
						Pin.Direction == EGPD_Output ? TEXT("output") : TEXT("input"), *Pin.Name.ToString(), *Reason.ToString()), TEXT("/pins"));
					return false;
				}
				if (Editable->CreateUserDefinedPin(Pin.Name, Pin.Type, Pin.Direction, /*bUseUniqueName=*/false) == nullptr)
				{
					OutError.Set(TEXT("Graph.UserPinsUnsupported"), FString::Printf(
						TEXT("Creating user pin '%s' failed."), *Pin.Name.ToString()), TEXT("/pins"));
					return false;
				}
			}
		}
		// Keep the requested order.
		Editable->UserDefinedPins.StableSort([Pins](const TSharedPtr<FUserPinInfo>& A, const TSharedPtr<FUserPinInfo>& B)
		{
			const int32 IndexA = Pins.IndexOfByPredicate([&A](const FAssetDocumentGraphUserPin& Pin) { return Pin.Name == A->PinName; });
			const int32 IndexB = Pins.IndexOfByPredicate([&B](const FAssetDocumentGraphUserPin& Pin) { return Pin.Name == B->PinName; });
			return IndexA < IndexB;
		});
		for (const FAssetDocumentGraphUserPin& Pin : Pins)
		{
			if (!Pin.DefaultValue.IsSet())
			{
				continue;
			}
			for (const TSharedPtr<FUserPinInfo>& Existing : Editable->UserDefinedPins)
			{
				if (Existing->PinName == Pin.Name)
				{
					Editable->ModifyUserDefinedPinDefaultValue(Existing, Pin.DefaultValue.GetValue());
				}
			}
		}
		RefreshSignatureNode(*Editable);
		return true;
	}

	virtual bool CanAddDynamicPin(const UEdGraphNode& Node) const override
	{
		const IK2Node_AddPinInterface* AddPin = Cast<IK2Node_AddPinInterface>(const_cast<UEdGraphNode*>(&Node));
		return AddPin != nullptr && AddPin->CanAddPin();
	}

	virtual bool AddDynamicPin(UEdGraphNode& Node, FAssetDocumentGraphError& OutError) const override
	{
		IK2Node_AddPinInterface* AddPin = Cast<IK2Node_AddPinInterface>(&Node);
		if (AddPin == nullptr || !AddPin->CanAddPin())
		{
			return IAssetDocumentGraphSchemaAdapter::AddDynamicPin(Node, OutError);
		}
		Node.Modify();
		AddPin->AddInputPin();
		return true;
	}

	virtual bool RemoveDynamicPin(UEdGraphNode& Node, UEdGraphPin* Pin, FAssetDocumentGraphError& OutError) const override
	{
		IK2Node_AddPinInterface* AddPin = Cast<IK2Node_AddPinInterface>(&Node);
		if (AddPin == nullptr || Pin == nullptr || !AddPin->CanRemovePin(Pin))
		{
			return IAssetDocumentGraphSchemaAdapter::RemoveDynamicPin(Node, Pin, OutError);
		}
		Node.Modify();
		AddPin->RemoveInputPin(Pin);
		return true;
	}

	// A promotable operator re-picks its function from every link and pin on each connection
	// (EvaluatePinsFromChange -> FindBestMatchingFunc) and conforms its pins to it: which function it lands
	// on depends on the order the links are made in, and the Pin Conversions the author made (Multiply's B a
	// float next to a vector A) can be undone by a later link. An end whose types the caller states takes
	// them back as long as every link of the node fits them, and the function the editor picks for those
	// pins and links, which is how the author's node holds them.
	static bool LinkFits(const UEdGraphPin& Pin, const FEdGraphPinType& Type, const UEdGraphPin& Link,
		const TMap<FName, FEdGraphPinType>* LinkStated)
	{
		const FEdGraphPinType* StatedLinkType = LinkStated != nullptr ? LinkStated->Find(Link.PinName) : nullptr;
		const FEdGraphPinType& LinkType = StatedLinkType != nullptr ? *StatedLinkType : Link.PinType;
		const FEdGraphPinType& Output = Pin.Direction == EGPD_Output ? Type : LinkType;
		const FEdGraphPinType& Input = Pin.Direction == EGPD_Output ? LinkType : Type;
		if (Output == Input || (Output.PinCategory == UEdGraphSchema_K2::PC_Real && Input.PinCategory == UEdGraphSchema_K2::PC_Real
			&& Output.ContainerType == Input.ContainerType))
		{
			return true;
		}
		const UEdGraphPin& InputPin = Pin.Direction == EGPD_Input ? Pin : Link;
		const UEdGraphPin& OutputPin = Pin.Direction == EGPD_Input ? Link : Pin;
		return GetDefault<UEdGraphSchema_K2>()->ArePinTypesCompatible(Output, Input) || FTypePromotion::IsValidPromotion(Output, Input)
			|| FTypePromotion::HasStructConversion(&InputPin, &OutputPin);
	}

	// A link by its pins' ids (a connection may rebuild a node's pins).
	struct FPriorLink
	{
		FGuid PinId;
		TWeakObjectPtr<UEdGraphNode> LinkNode;
		FGuid LinkPinId;
	};
	using FPriorLinks = TArray<FPriorLink>;

	// The links of the stated pins of an end's promotable operator before a connection re-evaluates it.
	static FPriorLinks LinksOfStatedPins(const UEdGraphPin& End, const TMap<FName, FEdGraphPinType>* Stated)
	{
		FPriorLinks Links;
		const UK2Node_PromotableOperator* Operator = Stated != nullptr ? Cast<UK2Node_PromotableOperator>(End.GetOwningNode()) : nullptr;
		for (const UEdGraphPin* Pin : Operator != nullptr ? Operator->Pins : TArray<UEdGraphPin*>())
		{
			if (Pin != nullptr && Pin->ParentPin == nullptr && Stated->Contains(Pin->PinName))
			{
				for (const UEdGraphPin* Link : Pin->LinkedTo)
				{
					if (Link != nullptr && Link->GetOwningNodeUnchecked() != nullptr)
					{
						Links.Add({Pin->PinId, Link->GetOwningNodeUnchecked(), Link->PinId});
					}
				}
			}
		}
		return Links;
	}

	// The connection made to Other, whose types the caller states too, holds them: a link to it fits
	// by them, not by what the connection left it with (Add's double result into Add_2's A, which the
	// connection has not yet promoted from int when Add takes its types back).
	static void RestoreStatedTypes(UEdGraphPin& End, const TMap<FName, FEdGraphPinType>* Stated,
		const UEdGraphPin& Other, const TMap<FName, FEdGraphPinType>* OtherStated, const FPriorLinks& Prior)
	{
		UK2Node_PromotableOperator* Operator = Stated != nullptr ? Cast<UK2Node_PromotableOperator>(End.GetOwningNode()) : nullptr;
		if (Operator == nullptr)
		{
			return;
		}
		TArray<TPair<UEdGraphPin*, FEdGraphPinType>> Wanted;
		bool bChanged = false;
		for (const TPair<FName, FEdGraphPinType>& Item : *Stated)
		{
			UEdGraphPin* Pin = nullptr;
			for (UEdGraphPin* Each : Operator->Pins)
			{
				if (Each != nullptr && Each->ParentPin == nullptr && Each->PinName == Item.Key && IsPromotableTypePin(*Operator, *Each))
				{
					Pin = Each;
				}
			}
			if (Pin == nullptr || Pin->SubPins.Num() > 0)
			{
				return;  // a pin the node does not have (yet) or split: nothing to hold on to
			}
			for (const UEdGraphPin* Link : Pin->LinkedTo)
			{
				if (Link == nullptr || !LinkFits(*Pin, Item.Value, *Link, Link == &Other ? OtherStated : nullptr))
				{
					return;
				}
			}
			bChanged |= Pin->PinType != Item.Value;
			Wanted.Emplace(Pin, Item.Value);
		}
		// The types go back first: the function is picked for them.
		TArray<TPair<UEdGraphPin*, FEdGraphPinType>> Had;
		for (const TPair<UEdGraphPin*, FEdGraphPinType>& Item : Wanted)
		{
			Had.Emplace(Item.Key, Item.Key->PinType);
			Item.Key->PinType = Item.Value;
		}
		// The function the operator would pick for these pins and links (GetPinsToConsider: a linked pin
		// by its links, an unlinked typed pin by its type; no tolerance pin, no comparison result).
		const bool bComparison = FTypePromotion::IsComparisonOpName(Operator->GetOperationName());
		TArray<UEdGraphPin*> Considered;
		for (UEdGraphPin* Pin : Operator->Pins)
		{
			if (Pin == nullptr || Pin->ParentPin != nullptr || Pin->PinName == TEXT("ErrorTolerance")
				|| (bComparison && Pin->Direction == EGPD_Output) || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				continue;
			}
			if (Pin->LinkedTo.Num() > 0)
			{
				Considered.Append(Pin->LinkedTo);
			}
			else if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
			{
				Considered.Add(Pin);
			}
		}
		// Of those, one that loading the node keeps them with (PostReconstructNode -> UpdatePinsFromFunction
		// conforms the pins to the function and breaks links that cannot promote to it): the best match for
		// Add's int A and B with a float result is Add (int, int), which a reload makes an int result
		// whose float link breaks (Landmass_FunctionLibrary).
		UFunction* Best = FTypePromotion::FindBestMatchingFunc(Operator->GetOperationName(), Considered);
		UFunction* Chosen = Best != nullptr && KeepsTypesOnReload(*Operator, *Best) ? Best : nullptr;
		if (Chosen == nullptr)
		{
			// The one whose parameters are the pins' own types the most (Add (double, double) for int
			// operands and a double result, not Add (vector, int), into which they promote too).
			TArray<UFunction*> Candidates;
			FTypePromotion::GetAllFuncsForOp(Operator->GetOperationName(), Candidates);
			int32 BestMatches = -1;
			for (UFunction* Candidate : Candidates)
			{
				const int32 Matches = Candidate != nullptr && KeepsTypesOnReload(*Operator, *Candidate) ? MatchingParameters(*Operator, *Candidate) : -1;
				if (Matches > BestMatches)
				{
					Chosen = Candidate;
					BestMatches = Matches;
				}
			}
			Chosen = Chosen != nullptr ? Chosen : Best;
		}
		// A function the node keeps the types with is not enough when it is not the one the editor picks
		// for them: Multiply's double pins left on Multiply (vector, vector) by an earlier link expand
		// into a vector multiply whose double links do not build (LandmassBrush_MeshStamp).
		const bool bRetarget = Chosen != nullptr && Chosen != Operator->GetTargetFunction();
		if (bChanged || bRetarget)
		{
			for (const TPair<UEdGraphPin*, FEdGraphPinType>& Item : Had)
			{
				Item.Key->PinType = Item.Value;
			}
			Operator->Modify();
			for (const TPair<UEdGraphPin*, FEdGraphPinType>& Item : Wanted)
			{
				Item.Key->PinType = Item.Value;
			}
		}
		if (bRetarget)
		{
			Operator->SetFromFunction(Chosen);
		}
		// A link of a stated pin the connection broke (re-picking Add (int, int) broke its double result's
		// link into Multiply) is the caller's, and fits the types the pin has back.
		for (const FPriorLink& Link : Prior)
		{
			UEdGraphPin* Pin = Operator->FindPinById(Link.PinId);
			UEdGraphNode* LinkNode = Link.LinkNode.Get();
			UEdGraphPin* LinkPin = LinkNode != nullptr ? LinkNode->FindPinById(Link.LinkPinId) : nullptr;
			if (Pin != nullptr && LinkPin != nullptr && !Pin->LinkedTo.Contains(LinkPin) && LinkFits(*Pin, Pin->PinType, *LinkPin, nullptr))
			{
				LinkNode->Modify();
				Pin->MakeLinkTo(LinkPin);
			}
		}
	}

	// How many of the operator's operands and result have the type of the function's parameter for them.
	static int32 MatchingParameters(const UK2Node_PromotableOperator& Operator, const UFunction& Function)
	{
		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		TArray<FEdGraphPinType> Inputs;
		FEdGraphPinType ReturnType;
		for (TFieldIterator<FProperty> It(&Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			FEdGraphPinType Type;
			if (Schema->ConvertPropertyToPinType(*It, Type))
			{
				(It->HasAnyPropertyFlags(CPF_ReturnParm) ? ReturnType : Inputs.AddDefaulted_GetRef()) = Type;
			}
		}
		const UEdGraphPin* Tolerance = Operator.FindPin(TEXT("ErrorTolerance"), EGPD_Input);
		int32 InputIndex = 0;
		int32 Matches = 0;
		for (const UEdGraphPin* Pin : Operator.Pins)
		{
			if (Pin == nullptr || Pin->ParentPin != nullptr || Pin == Tolerance || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				continue;
			}
			const FEdGraphPinType* Type = Pin->Direction == EGPD_Output ? &ReturnType : Inputs.IsValidIndex(InputIndex) ? &Inputs[InputIndex++] : nullptr;
			// The result counts twice: its type is what the node's consumers see.
			Matches += Type != nullptr && Type->PinCategory == Pin->PinType.PinCategory && Type->PinSubCategoryObject == Pin->PinType.PinSubCategoryObject
				? (Pin->Direction == EGPD_Output ? 2 : 1) : 0;
		}
		return Matches;
	}

	// Whether UK2Node_PromotableOperator::UpdatePinsFromFunction(Function), which loading the node runs,
	// leaves every pin with its current type and its links (its ConformPinLambda, pin by pin).
	static bool KeepsTypesOnReload(const UK2Node_PromotableOperator& Operator, const UFunction& Function)
	{
		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		FEdGraphPinType ReturnType;
		FEdGraphPinType HighestInput;
		TArray<FEdGraphPinType> Inputs;
		for (TFieldIterator<FProperty> It(&Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			FEdGraphPinType Type;
			if (!Schema->ConvertPropertyToPinType(*It, Type))
			{
				continue;
			}
			if (It->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				ReturnType = Type;
			}
			else
			{
				if (FTypePromotion::GetHigherType(HighestInput, Type) == FTypePromotion::ETypeComparisonResult::TypeBHigher)
				{
					HighestInput = Type;
				}
				Inputs.Add(Type);
			}
		}
		const UEdGraphPin* Tolerance = Operator.FindPin(TEXT("ErrorTolerance"), EGPD_Input);  // (FindTolerancePin is not exported)
		int32 InputIndex = 0;
		for (const UEdGraphPin* Pin : Operator.Pins)
		{
			if (Pin == nullptr || Pin->ParentPin != nullptr || Pin == Tolerance || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				continue;
			}
			const FEdGraphPinType& FunctionType = Pin->Direction == EGPD_Output ? ReturnType
				: Inputs.IsValidIndex(InputIndex) ? Inputs[InputIndex++] : HighestInput;  // an additional input: the highest
			if (FWildcardNodeUtils::IsLinkedToWildcard(Pin))
			{
				continue;
			}
			const bool bLinked = Pin->LinkedTo.Num() > 0;
			const FEdGraphPinType Determining = bLinked ? FTypePromotion::GetPromotedType(Pin->LinkedTo) : Pin->PinType;
			const bool bStructConversion = bLinked && FTypePromotion::HasStructConversion(Pin, Pin->LinkedTo[0]);  // as the engine asks
			const bool bDifferingStructs = Determining.PinCategory == UEdGraphSchema_K2::PC_Struct
				&& FunctionType.PinCategory == UEdGraphSchema_K2::PC_Struct
				&& Determining.PinSubCategoryObject != FunctionType.PinSubCategoryObject && !bStructConversion;
			FEdGraphPinType Result = FunctionType;
			if ((bLinked || !Pin->DoesDefaultValueMatchAutogenerated())
				&& (Determining.PinCategory != FunctionType.PinCategory || bDifferingStructs))
			{
				if (!FTypePromotion::IsValidPromotion(Determining, FunctionType) && !bStructConversion)
				{
					return false;  // its links break
				}
				Result = Determining;
			}
			if (Result != Pin->PinType)
			{
				return false;
			}
		}
		return true;
	}

	// The node UEdGraphSchema_K2::CreateAutomaticConversionNodeAndConnections inserts (same search order):
	// an autocast function call (Conv_IntToBool), else a specialized conversion node (Make Array, enum to
	// name, ...). The node is made here only to read its key: it is transient and in no graph.
	virtual bool DescribeConversion(const UEdGraphPin& Output, const UEdGraphPin& Input,
		FAssetDocumentGraphConversion& OutConversion) const override
	{
		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		if (const TOptional<UEdGraphSchema_K2::FSearchForAutocastFunctionResults> Autocast =
			Schema->SearchForAutocastFunction(Output.PinType, Input.PinType))
		{
			UFunction* Function = Autocast->FunctionOwner != nullptr
				? Autocast->FunctionOwner->FindFunctionByName(Autocast->TargetFunction) : nullptr;
			if (Function == nullptr)
			{
				return false;
			}
			UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(GetTransientPackage(), NAME_None, RF_Transient);
			Node->FunctionReference.SetExternalMember(Autocast->TargetFunction, Autocast->FunctionOwner);
			OutConversion.ActionKey = K2Keys::ComputeNodeKey(*Node);
			OutConversion.NodeClassPath = UK2Node_CallFunction::StaticClass()->GetPathName();
			// AutowireConversionNode: the first input parameter and the return value.
			for (TFieldIterator<FProperty> It(Function); It && (It->PropertyFlags & CPF_Parm); ++It)
			{
				const bool bOutput = It->HasAnyPropertyFlags(CPF_ReturnParm)
					|| (It->HasAnyPropertyFlags(CPF_OutParm) && !It->HasAnyPropertyFlags(CPF_ReferenceParm));
				if (bOutput && OutConversion.OutputPin.IsNone())
				{
					OutConversion.OutputPin = It->GetFName();
				}
				else if (!bOutput && OutConversion.InputPin.IsNone())
				{
					OutConversion.InputPin = It->GetFName();
				}
			}
			return true;
		}
		if (const TOptional<UEdGraphSchema_K2::FFindSpecializedConversionNodeResults> Specialized =
			Schema->FindSpecializedConversionNode(Output.PinType, Input, /*bCreateNode=*/true);
			Specialized.IsSet() && Specialized->TargetNode != nullptr)
		{
			OutConversion.ActionKey = K2Keys::ComputeNodeKey(*Specialized->TargetNode);
			OutConversion.NodeClassPath = Specialized->TargetNode->GetClass()->GetPathName();
			return true;
		}
		return false;
	}

	virtual bool CheckActionKey(UObject& Asset, UEdGraph& Graph, const FString& Key, FAssetDocumentGraphError& OutError) const override
	{
		UBlueprint* Blueprint = BlueprintOf(&Asset, &Graph);
		// Keys placed by their own paths (dispatcher events, collapsed graphs, parent calls) are judged at spawn.
		if (Blueprint == nullptr || Key.StartsWith(TEXT("K2:CustomEvent:")) || Key == CompositeKey || Key.StartsWith(ParentCallPrefix))
		{
			return true;
		}
		TArray<FK2Candidate> Candidates;
		if (FindCandidate(*Blueprint, Graph, Key, Candidates, OutError) != nullptr)
		{
			return true;
		}
		if (OutError.Code == TEXT("Graph.ActionNotFound"))
		{
			// A key's owner Blueprint may not be loaded yet (ResolveAndSpawn loads it, §8).
			LoadKeyOwnerBlueprint(Key);
			OutError = FAssetDocumentGraphError();
			return FindCandidate(*Blueprint, Graph, Key, Candidates, OutError) != nullptr;
		}
		return false;
	}

	// The palette's template nodes with their default pins, as its pin-context filtering reads them
	// (BlueprintActionFilterImpl::HasMatchingPin). The template cache places a template in a graph of the
	// Blueprint's kind; the graph must be a Blueprint graph the node can be created under, or the cache finds
	// no outer for it.
	virtual void GetTemplateNodes(UObject& Asset, UEdGraph& Graph, TConstArrayView<FString> Keys,
		TMap<FString, UEdGraphNode*>& OutNodes) const override
	{
		UBlueprint* Blueprint = BlueprintOf(&Asset, &Graph);
		if (Blueprint == nullptr || Keys.IsEmpty() || Graph.GetOuter() != Blueprint || Graph.HasAnyFlags(RF_Transient)
			|| Graph.GetSchema() == nullptr)
		{
			return;
		}
		TArray<FK2Candidate> Candidates;
		CollectCandidates(*Blueprint, Graph, Candidates);
		for (const FString& Key : Keys)
		{
			const FK2Candidate* Match = Candidates.FindByPredicate([&Key](const FK2Candidate& Candidate) { return Candidate.Key == Key; });
			if (Match == nullptr)
			{
				Match = AddOperatorFunctionCandidate(Key, Graph, Candidates);
			}
			if (Match == nullptr)
			{
				// A call keyed by a class that inherits or implements the function: spawn places the declaring
				// class's action (FindCandidate), so Validate reads that action's template too.
				FAssetDocumentGraphError Ignored;
				Match = FindByDeclaringClass(Key, Candidates, Ignored);
			}
			if (Match == nullptr || Match->Spawner == nullptr || Match->bBound || Match->Spawner->NodeClass == nullptr
				|| !Match->Spawner->NodeClass->GetDefaultObject<UEdGraphNode>()->CanCreateUnderSpecifiedSchema(Graph.GetSchema()))
			{
				continue;
			}
			if (UEdGraphNode* Template = Match->Spawner->GetTemplateNode(&Graph))
			{
				// Templates are spawned without pins.
				if (Template->Pins.IsEmpty())
				{
					Template->AllocateDefaultPins();
				}
				OutNodes.Add(Key, Template);
			}
		}
	}

	virtual bool ConnectPins(UEdGraphPin& A, UEdGraphPin& B,
		const TMap<FName, FEdGraphPinType>* KeepTypesA, const TMap<FName, FEdGraphPinType>* KeepTypesB) const override
	{
		const UEdGraph* Graph = A.GetOwningNode()->GetGraph();
		const FPriorLinks PriorA = LinksOfStatedPins(A, KeepTypesA);
		const FPriorLinks PriorB = LinksOfStatedPins(B, KeepTypesB);
		if (Graph == nullptr || Graph->GetSchema() == nullptr || !Graph->GetSchema()->TryCreateConnection(&A, &B))
		{
			return false;
		}
		RestoreStatedTypes(A, KeepTypesA, B, KeepTypesB, PriorA);
		RestoreStatedTypes(B, KeepTypesB, A, KeepTypesA, PriorB);
		// A's function was picked while B still had the connection's types: Multiply's double result
		// into Multiply's A, a vector until B takes its double back, picks Multiply (vector, vector)
		// (LandmassBrush_MeshStamp). Picked again for B's own types.
		RestoreStatedTypes(A, KeepTypesA, B, KeepTypesB, PriorA);
		return true;
	}

	virtual bool SetPinType(UEdGraphNode& Node, UEdGraphPin& Pin, const FEdGraphPinType& Type, FAssetDocumentGraphError& OutError) const override
	{
		if (UK2Node_PromotableOperator* Operator = Cast<UK2Node_PromotableOperator>(&Node))
		{
			return ConvertPromotablePin(*Operator, Pin, Type, OutError);
		}
		// The Select node's Index pin shows a type selector (SGraphPinIndex, NodeFactory: a wildcard
		// pin of sub-category "index") while it is unlinked; its tree offers the index types
		// (ETypeTreeFilter::IndexTypesOnly: bool, byte / enum, int, int64), no containers.
		if (!IsTypeSelectorPin(Node, Pin))
		{
			return IAssetDocumentGraphSchemaAdapter::SetPinType(Node, Pin, Type, OutError);
		}
		if (Pin.LinkedTo.Num() > 0 || Pin.PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard
			|| Pin.PinType.PinSubCategory != UEdGraphSchema_K2::PSC_Index)
		{
			OutError.Set(TEXT("Graph.PinTypeNotEditable"), FString::Printf(
				TEXT("The %s pin's type can only be picked while it is an unlinked wildcard; it is %s (a typed Select is a new node)."),
				*Pin.PinName.ToString(), *UEdGraphSchema_K2::TypeToText(Pin.PinType).ToString()), TEXT("/pin_name"));
			return false;
		}
		const FName Category = Type.PinCategory;
		if (Type.IsContainer() || !(Category == UEdGraphSchema_K2::PC_Boolean || Category == UEdGraphSchema_K2::PC_Byte
			|| Category == UEdGraphSchema_K2::PC_Int || Category == UEdGraphSchema_K2::PC_Int64))
		{
			OutError.Set(TEXT("Graph.InvalidArgument"), TEXT("An index type is bool, byte, an enum (byte:<enum path>), int or int64."), TEXT("/type"));
			return false;
		}
		// SGraphPinIndex::OnTypeChanged.
		Pin.Modify();
		Pin.PinType = Type;
		Node.PinTypeChanged(&Pin);
		Node.ReconstructNode();
		return true;
	}

	// Pin Conversions > To <type> (UK2Node_PromotableOperator::ConvertPinType): breaks the pin's links,
	// sets its type and lets the operator pick its function and conform the other pins.
	static bool ConvertPromotablePin(UK2Node_PromotableOperator& Operator, UEdGraphPin& Pin, const FEdGraphPinType& Type,
		FAssetDocumentGraphError& OutError)
	{
		if (!IsPromotableTypePin(Operator, Pin))
		{
			OutError.Set(TEXT("Graph.PinTypeNotEditable"), FString::Printf(TEXT("Pin %s of %s offers no Pin Conversions."),
				*Pin.PinName.ToString(), *Operator.GetNodeTitle(ENodeTitleType::ListView).ToString()), TEXT("/pin_name"));
			return false;
		}
		if (Pin.PinType == Type)
		{
			return true;
		}
		if (!IsOfferedConversion(Operator, Pin, Type))
		{
			OutError.Set(TEXT("Graph.PinTypeNotEditable"), FString::Printf(TEXT("Pin Conversions of %s.%s (%s) offer no %s."),
				*Operator.GetNodeTitle(ENodeTitleType::ListView).ToString(), *Pin.PinName.ToString(),
				*UEdGraphSchema_K2::TypeToText(Pin.PinType).ToString(), *UEdGraphSchema_K2::TypeToText(Type).ToString()), TEXT("/type"));
			return false;
		}
		const FName PinName = Pin.PinName;
		const EEdGraphPinDirection Direction = Pin.Direction;
		Operator.ConvertPinType(&Pin, Type);
		const UEdGraphPin* After = Operator.FindPin(PinName, Direction);
		if (After == nullptr || After->PinType != Type)
		{
			OutError.Set(TEXT("Graph.PinTypeNotEditable"), FString::Printf(TEXT("Converting %s to %s left it %s."),
				*PinName.ToString(), *UEdGraphSchema_K2::TypeToText(Type).ToString(),
				After != nullptr ? *UEdGraphSchema_K2::TypeToText(After->PinType).ToString() : TEXT("removed")), TEXT("/type"));
			return false;
		}
		return true;
	}

	virtual bool SetNodeSetting(UObject& Asset, UEdGraphNode& Node, FStringView Path, const FValue& Value,
		bool& bOutHandled, FAssetDocumentGraphError& OutError) const override
	{
		bOutHandled = false;
		if (Path == TEXTVIEW("LocalVariables"))
		{
			UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(&Node);
			if (Entry == nullptr)
			{
				return true;
			}
			bOutHandled = true;
			return SetLocalVariables(*Entry, Value, OutError);
		}
		if (Path == TEXTVIEW("Function"))
		{
			UK2Node_CreateDelegate* CreateDelegate = Cast<UK2Node_CreateDelegate>(&Node);
			if (CreateDelegate == nullptr)
			{
				return true;
			}
			bOutHandled = true;
			return SetDelegateFunction(*CreateDelegate, Value, OutError);
		}
		if (Path == TEXTVIEW("Fields"))
		{
			UK2Node_SetFieldsInStruct* SetFields = Cast<UK2Node_SetFieldsInStruct>(&Node);
			if (SetFields == nullptr)
			{
				return true;
			}
			bOutHandled = true;
			return SetSetFieldsShown(*SetFields, Value, OutError);
		}
		if (Path != TEXTVIEW("Pure"))
		{
			return true;
		}
		bool bPure = false;
		bool bDefault = false;
		if (!DescribePurity(Node, bPure, bDefault) && !Node.IsA<UK2Node_CallFunction>() && !Node.IsA<UK2Node_VariableGet>())
		{
			return true;
		}
		bOutHandled = true;
		if (Value.Kind != EAssetDocumentRuntimeValueKind::Boolean)
		{
			OutError.Set(TEXT("Graph.InvalidArgument"), TEXT("Pure is true or false."), TEXT("/value"));
			return false;
		}
		const UK2Node* K2Node = Cast<UK2Node>(&Node);
		if (K2Node == nullptr || K2Node->IsNodePure() == Value.BooleanValue)
		{
			return true;
		}
		if (!TogglePurity(Node) || K2Node->IsNodePure() != Value.BooleanValue)
		{
			OutError.Set(TEXT("Graph.PropertyNotEditable"), FString::Printf(
				TEXT("The editor offers no purity toggle for %s here (a Variable Get toggles for a bool / object / class variable "
					"in a graph that allows exec pins; a function call when it defaults to pure and has outputs)."),
				*Node.GetNodeTitle(ENodeTitleType::ListView).ToString()), TEXT("/value"));
			return false;
		}
		return true;
	}

	virtual bool SetFunctionFlags(
		UObject& Asset, UEdGraph& Graph, const FValue& Flags, FValue& OutFlags, FAssetDocumentGraphError& OutError) const override
	{
		UBlueprint* Blueprint = BlueprintOf(&Asset, &Graph);
		UK2Node_FunctionEntry* Entry = FindFunctionEntry(Graph);
		if (Blueprint == nullptr || Entry == nullptr)
		{
			return IAssetDocumentGraphSchemaAdapter::SetFunctionFlags(Asset, Graph, Flags, OutFlags, OutError);
		}
		FReader Reader(Flags, TEXT("/flags"), OutError);
		if (!Reader.OnlyFields({TEXT("pure"), TEXT("const"), TEXT("call_in_editor"), TEXT("access"),
			TEXT("category"), TEXT("tooltip"), TEXT("keywords")}))
		{
			return false;
		}
		if (!Entry->IsEditable())
		{
			// An override of a native function: its flags come from the function it overrides (the editor
			// offers no signature details for it). Stating them as they are is a no-op.
			OutFlags = MakeSignatureFlags(*Entry);
			const FValue Current = OutFlags;
			FAssetDocumentGraphError Ignored;
			FReader Have(Current, TEXT("/flags"), Ignored);
			for (const TCHAR* Key : {TEXT("pure"), TEXT("const"), TEXT("call_in_editor")})
			{
				bool bWanted = false;
				bool bHave = false;
				if (Reader.Has(Key) && (!Reader.Bool(Key, bWanted) || !Have.Bool(Key, bHave) || bWanted != bHave))
				{
					return Reader.Fail(TEXT("Graph.GraphOperationUnsupported"), FString::Printf(
						TEXT("Graph %s overrides an inherited function: its %s flag is the overridden function's."), *Graph.GetName(), Key), Key);
				}
			}
			for (const TCHAR* Key : {TEXT("access"), TEXT("category"), TEXT("tooltip"), TEXT("keywords")})
			{
				FString Wanted;
				FString HaveValue;
				if (Reader.Has(Key) && (!Reader.String(Key, Wanted) || !Have.String(Key, HaveValue) || Wanted != HaveValue))
				{
					return Reader.Fail(TEXT("Graph.GraphOperationUnsupported"), FString::Printf(
						TEXT("Graph %s overrides an inherited function: its %s is the overridden function's."), *Graph.GetName(), Key), Key);
				}
			}
			return true;
		}
		UFunction* Function = Blueprint->SkeletonGeneratedClass != nullptr
			? Blueprint->SkeletonGeneratedClass->FindFunctionByName(Graph.GetFName()) : nullptr;
		Entry->Modify();
		if (Function != nullptr)
		{
			Function->Modify();
		}
		bool bStructural = false;
		auto SetFlag = [&](const TCHAR* Key, EFunctionFlags Flag) -> bool
		{
			bool bValue = false;
			if (!Reader.Has(Key))
			{
				return true;
			}
			if (!Reader.Bool(Key, bValue))
			{
				return false;
			}
			const bool bCurrent = (Entry->GetExtraFlags() & Flag) != 0;
			if (bCurrent != bValue)
			{
				// FBlueprintGraphActionDetails::OnIsPureFunctionModified / OnIsConstFunctionModified.
				Entry->SetExtraFlags(Entry->GetExtraFlags() ^ Flag);
				if (Function != nullptr)
				{
					Function->FunctionFlags ^= Flag;
				}
				bStructural = true;
			}
			return true;
		};
		if (!SetFlag(TEXT("pure"), FUNC_BlueprintPure) || !SetFlag(TEXT("const"), FUNC_Const))
		{
			return false;
		}
		if (Reader.Has(TEXT("call_in_editor")))
		{
			bool bValue = false;
			if (!Reader.Bool(TEXT("call_in_editor"), bValue))
			{
				return false;
			}
			Entry->MetaData.bCallInEditor = bValue;
		}
		if (Reader.Has(TEXT("access")))
		{
			FString Access;
			if (!Reader.String(TEXT("access"), Access))
			{
				return false;
			}
			EFunctionFlags Specifier = FUNC_Public;
			if (Access == TEXT("protected"))
			{
				Specifier = FUNC_Protected;
			}
			else if (Access == TEXT("private"))
			{
				Specifier = FUNC_Private;
			}
			else if (Access != TEXT("public"))
			{
				return Reader.Fail(TEXT("Graph.InvalidArgument"), TEXT("access must be public, protected or private."), TEXT("access"));
			}
			// FBlueprintGraphActionDetails::OnAccessSpecifierSelected.
			Entry->SetExtraFlags((Entry->GetExtraFlags() & ~FUNC_AccessSpecifiers) | Specifier);
			if (Function != nullptr)
			{
				Function->FunctionFlags = (Function->FunctionFlags & ~FUNC_AccessSpecifiers) | Specifier;
			}
			bStructural = true;
		}
		if (Reader.Has(TEXT("category")))
		{
			FString Category;
			if (!Reader.String(TEXT("category"), Category))
			{
				return false;
			}
			FBlueprintEditorUtils::SetBlueprintFunctionOrMacroCategory(
				&Graph, FText::TrimPrecedingAndTrailing(FText::FromString(Category)), /*bDontRecompile=*/true);
		}
		if (Reader.Has(TEXT("tooltip")))
		{
			FString Tooltip;
			if (!Reader.String(TEXT("tooltip"), Tooltip))
			{
				return false;
			}
			Entry->MetaData.ToolTip = FText::FromString(Tooltip);
			if (Function != nullptr)
			{
				Function->SetMetaData(FBlueprintMetadata::MD_Tooltip, *Tooltip);
			}
		}
		if (Reader.Has(TEXT("keywords")))
		{
			FString Keywords;
			if (!Reader.String(TEXT("keywords"), Keywords))
			{
				return false;
			}
			const FText Trimmed = FText::TrimPrecedingAndTrailing(FText::FromString(Keywords));
			Entry->MetaData.Keywords = Trimmed;
			if (Function != nullptr)
			{
				Function->SetMetaData(FBlueprintMetadata::MD_FunctionKeywords, *Trimmed.ToString());
			}
		}
		RefreshSignatureNode(*Entry);
		if (bStructural)
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		}
		else
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		}
		OutFlags = MakeSignatureFlags(*Entry);
		return true;
	}

private:
	// FKismetDelegateDragDropAction::MakeEvent (name made unique like the Assign node's companion event).
	static bool SpawnDispatcherEvent(UBlueprint& Blueprint, const FAssetDocumentGraphSpawnRequest& Request,
		FAssetDocumentGraphSpawnResult& OutResult, FAssetDocumentGraphError& OutError)
	{
		TArray<FDispatcherEvent> Events;
		CollectDispatcherEvents(Blueprint, *Request.Graph, Events);
		const FDispatcherEvent* Event = Events.FindByPredicate([&Request](const FDispatcherEvent& Item) { return Item.Key == Request.ActionKey; });
		for (const FString& Inherited : InheritedMemberKeys(Blueprint, Request.ActionKey))
		{
			if (Event == nullptr)
			{
				Event = Events.FindByPredicate([&Inherited](const FDispatcherEvent& Item) { return Item.Key == Inherited; });
			}
		}
		if (Event == nullptr)
		{
			OutError.Set(TEXT("Graph.ActionNotFound"), Blueprint.UbergraphPages.Contains(Request.Graph)
				? FString::Printf(TEXT("No K2 action has key %s (custom event keys with an owner name an assignable event dispatcher: K2:CustomEvent:<Owner>:<Dispatcher>)."), *Request.ActionKey)
				: FString::Printf(TEXT("Action %s creates a dispatcher event, which only event graphs can hold."), *Request.ActionKey));
			return false;
		}
		const FVector2D Location = Request.Position.IsSet() ? Request.Position.GetValue() : DefaultLocation(*Request.Graph);
		const FName Name = FBlueprintEditorUtils::FindUniqueKismetName(&Blueprint, Event->Delegate->GetName() + TEXT("_Event"));
		UK2Node_CustomEvent* Node = UK2Node_CustomEvent::CreateFromFunction(Location, Request.Graph, Name.ToString(),
			Event->Delegate->SignatureFunction, /*bSelectNewNode=*/false);
		if (Node == nullptr)
		{
			OutError.Set(TEXT("Graph.ActionNotFound"), FString::Printf(TEXT("Action %s did not produce a node."), *Request.ActionKey));
			return false;
		}
		if (Request.Position.IsSet())
		{
			RawEdits::SetNodePosition(*Node, FMath::RoundToInt(Location.X), FMath::RoundToInt(Location.Y));  // CreateFromFunction snaps to the grid
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
		OutResult.Node = Node;
		OutResult.bReused = false;
		return true;
	}

	static FVector2D DefaultLocation(const UEdGraph& Graph)
	{
		int32 MaxX = TNumericLimits<int32>::Lowest();
		for (const UEdGraphNode* Node : Graph.Nodes)
		{
			if (Node != nullptr)
			{
				MaxX = FMath::Max(MaxX, Node->NodePosX);
			}
		}
		return FVector2D(MaxX == TNumericLimits<int32>::Lowest() ? 0 : MaxX + 400, 0);
	}

	// "K2:<Kind>:<Class>:<Function>" of an operator function that type promotion covers
	// (Less_DoubleDouble as K2:CallFunction, Add_DoubleDouble as K2:CommutativeAssociativeBinaryOperator,
	// ...). The action database leaves out the function's own spawner only to avoid duplicate menu
	// entries next to the promotable operator (BlueprintActionDatabaseImpl::AddClassFunctionActions),
	// yet such nodes exist in content made before type promotion and the editor places them (palette
	// with type promotion off, copy/paste). The candidate is the spawner the database registers for
	// the function with type promotion off. Likewise a plain K2:CallFunction of a commutative operator
	// (BooleanAND, Add_IntInt in the engine's StandardMacros): older content, copy/paste.
	// The spawner the action database registers for an operator function when the editor setting
	// "Enable Type Promotion" (UBlueprintEditorSettings::bEnableTypePromotion) is off:
	// UBlueprintFunctionNodeSpawner::Create reads the setting to choose between the shared promotable
	// operator spawner and the function's own one (CommutativeAssociativeBinaryOperator or CallFunction
	// node). The setting is switched off only for the duration of the call (not saved).
	// The node class is the one the key names: content from before UE5 also holds plain CallFunction
	// nodes of commutative operators (Add_IntInt in the engine's StandardMacros), which behave the same.
	static UBlueprintFunctionNodeSpawner* MakeOperatorFunctionSpawner(const UFunction& Function, TSubclassOf<UK2Node_CallFunction> NodeClass)
	{
		UBlueprintEditorSettings* Settings = GetMutableDefault<UBlueprintEditorSettings>();
		const bool bTypePromotion = Settings->bEnableTypePromotion;
		Settings->bEnableTypePromotion = false;
		UBlueprintFunctionNodeSpawner* Spawner = UBlueprintFunctionNodeSpawner::Create(NodeClass, &Function, GetTransientPackage());
		Settings->bEnableTypePromotion = bTypePromotion;
		if (Spawner != nullptr)
		{
			Spawner->AddToRoot();  // shared like the action database's own spawners
		}
		return Spawner;
	}

	static const FK2Candidate* AddOperatorFunctionCandidate(const FString& Key, UEdGraph& Graph, TArray<FK2Candidate>& Candidates)
	{
		TArray<FString> Parts;
		Key.ParseIntoArray(Parts, TEXT(":"));
		if (Parts.Num() != 4 || Parts[0] != TEXT("K2") || Key.Contains(TEXT("#")) || Key.Contains(TEXT("@")))
		{
			return nullptr;
		}
		const UClass* Owner = FindObject<UClass>(nullptr, *Parts[2]);
		UFunction* Function = Owner != nullptr ? Owner->FindFunctionByName(FName(*Parts[3]), EIncludeSuperFlag::ExcludeSuper) : nullptr;
		const bool bCommutativeOperator = Function != nullptr
			&& Function->HasMetaData(FBlueprintMetadata::MD_CommutativeAssociativeBinaryOperator) && Function->HasAllFunctionFlags(FUNC_BlueprintPure);
		// Not IsOperatorSpawnerRegistered: action database refreshes clear and re-register the shared
		// operator spawners, and the database never lists the function's own spawner either way.
		const bool bPromotable = Function != nullptr && FTypePromotion::IsFunctionPromotionReady(Function);
		if (Function == nullptr || !(bPromotable || bCommutativeOperator)
			|| !UEdGraphSchema_K2::CanUserKismetCallFunction(Function)
			|| !FBlueprintActionDatabase::IsFunctionAllowed(Function, FBlueprintActionDatabase::EPermissionsContext::Node))
		{
			return nullptr;
		}
		TSubclassOf<UK2Node_CallFunction> NodeClass;
		if (Parts[1] == K2Keys::KindOf(UK2Node_CallFunction::StaticClass()))
		{
			NodeClass = UK2Node_CallFunction::StaticClass();
		}
		else if (Parts[1] == K2Keys::KindOf(UK2Node_CommutativeAssociativeBinaryOperator::StaticClass()) && bCommutativeOperator && bPromotable)
		{
			NodeClass = UK2Node_CommutativeAssociativeBinaryOperator::StaticClass();
		}
		else
		{
			return nullptr;
		}
		static TMap<FString, TWeakObjectPtr<UBlueprintFunctionNodeSpawner>> OperatorFunctionSpawners;
		TWeakObjectPtr<UBlueprintFunctionNodeSpawner>& Cached = OperatorFunctionSpawners.FindOrAdd(Parts[1] + TEXT(":") + Function->GetPathName());
		if (!Cached.IsValid())
		{
			Cached = MakeOperatorFunctionSpawner(*Function, NodeClass);
		}
		FString SpawnerKey;
		if (!Cached.IsValid() || !K2Keys::ComputeSpawnerBaseKey(Owner, *Cached.Get(), &Graph, SpawnerKey) || SpawnerKey != Key)
		{
			return nullptr;
		}
		FK2Candidate& Candidate = Candidates.AddDefaulted_GetRef();
		Candidate.DatabaseOwner = Owner;
		Candidate.Spawner = Cached.Get();
		Candidate.Key = Key;
		return &Candidate;
	}

	// "K2:<Kind>:<Own class>:<Member>" as each ancestor class would key it, nearest first.
	static TArray<FString> InheritedMemberKeys(const UBlueprint& Blueprint, const FString& Key)
	{
		TArray<FString> Keys;
		TArray<FString> Parts;
		Key.ParseIntoArray(Parts, TEXT(":"), false);
		if (Parts.Num() != 4 || Blueprint.GeneratedClass == nullptr
			|| !Parts[2].Equals(K2Keys::OwnerPath(Blueprint.GeneratedClass.Get()), ESearchCase::IgnoreCase))
		{
			return Keys;
		}
		for (const UClass* Ancestor = Blueprint.ParentClass; Ancestor != nullptr; Ancestor = Ancestor->GetSuperClass())
		{
			Parts[2] = K2Keys::OwnerPath(Ancestor);
			Keys.Add(FString::Join(Parts, TEXT(":")));
		}
		return Keys;
	}

	// "K2:CallFunction:<Class>:<Function>" naming a class that inherits the function: the palette files a native or
	// Blueprint function under the class declaring it ("K2:CallFunction:/Script/Niagara.NiagaraComponent:Deactivate"
	// is the ActorComponent's Deactivate), so the key is looked up there. A name the class has no Blueprint-callable
	// function of (a C++ function that is not a UFUNCTION(BlueprintCallable), such as
	// UNiagaraComponent::DeactivateImmediate) fails here with the reason and the callable functions named like it.
	static const FK2Candidate* FindByDeclaringClass(const FString& Key, TArray<FK2Candidate>& Candidates, FAssetDocumentGraphError& OutError)
	{
		TArray<FString> Parts;
		Key.ParseIntoArray(Parts, TEXT(":"), false);
		if (Parts.Num() != 4 || Parts[1] != K2Keys::KindOf(UK2Node_CallFunction::StaticClass()) || !Parts[2].StartsWith(TEXT("/"))
			|| Parts[2].Contains(TEXT("@")) || Parts[3].IsEmpty())
		{
			return nullptr;
		}
		const UClass* Class = FSoftClassPath(Parts[2]).ResolveClass();
		if (Class == nullptr)
		{
			return nullptr;
		}
		const UFunction* Function = Class->FindFunctionByName(FName(*Parts[3]));
		if (Function != nullptr && Function->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure))
		{
			const UClass* Declaring = Function->GetOwnerClass();
			if (Declaring == nullptr || K2Keys::OwnerPath(Declaring) == K2Keys::OwnerPath(Class))
			{
				return nullptr;
			}
			Parts[2] = K2Keys::OwnerPath(Declaring);
			const FString DeclaredKey = FString::Join(Parts, TEXT(":"));
			return Candidates.FindByPredicate([&DeclaredKey](const FK2Candidate& C) { return C.Key == DeclaredKey; });
		}
		// The first word of the name ("Deactivate" of "DeactivateImmediate") finds the callable relatives.
		int32 WordEnd = 1;
		while (WordEnd < Parts[3].Len() && !FChar::IsUpper(Parts[3][WordEnd]))
		{
			++WordEnd;
		}
		const FString Word = Parts[3].Left(WordEnd);
		TArray<FString> Related;
		for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::IncludeSuper); It && Word.Len() >= 3; ++It)
		{
			if (It->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure) && It->GetName().Contains(Word)
				&& !It->HasMetaData(TEXT("DeprecatedFunction")))
			{
				Related.AddUnique(FString::Printf(TEXT("K2:%s:%s:%s"), *Parts[1], *K2Keys::OwnerPath(It->GetOwnerClass()), *It->GetName()));
			}
		}
		Related.Sort();
		OutError.Set(TEXT("Graph.ActionNotFound"), FString::Printf(TEXT("No K2 action has key %s: %s has no function %s that a "
			"Blueprint can call. %s; the editor's action palette does not list it either, so no Blueprint node calls it.%s"),
			*Key, *Class->GetName(), *Parts[3], Function != nullptr ? TEXT("It is a UFUNCTION without BlueprintCallable")
				: TEXT("A C++ function that is not a UFUNCTION(BlueprintCallable) is not reflected to Blueprints"),
			Related.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" Blueprint-callable functions of %s named like it: %s."), *Class->GetName(),
				*FString::Join(Related, TEXT(", ")))));
		OutError.AddContext(TEXT("related"), MakeStringArray(Related));
		return nullptr;
	}

	static const FK2Candidate* FindCandidate(
		UBlueprint& Blueprint, UEdGraph& Graph, const FString& Key, TArray<FK2Candidate>& Candidates,
		FAssetDocumentGraphError& OutError)
	{
		CollectCandidates(Blueprint, Graph, Candidates);
		if (const FK2Candidate* Exact = Candidates.FindByPredicate([&Key](const FK2Candidate& C) { return C.Key == Key; }))
		{
			return Exact;
		}
		if (const FK2Candidate* Operator = AddOperatorFunctionCandidate(Key, Graph, Candidates))
		{
			return Operator;
		}
		// A cast keyed by the Blueprint asset (the key form before casts named the generated class).
		TArray<FString> Parts;
		Key.ParseIntoArray(Parts, TEXT(":"));
		if (Parts.Num() == 3 && (Parts[1] == K2Keys::KindOf(UK2Node_DynamicCast::StaticClass())
			|| Parts[1] == K2Keys::KindOf(UK2Node_ClassDynamicCast::StaticClass())))
		{
			if (const UBlueprint* CastBlueprint = FindObject<UBlueprint>(nullptr, *Parts[2]); CastBlueprint != nullptr && CastBlueprint->GeneratedClass != nullptr)
			{
				const FString ClassKey = FString::Printf(TEXT("K2:%s:%s"), *Parts[1], *K2Keys::OwnerPath(CastBlueprint->GeneratedClass.Get()));
				if (const FK2Candidate* Cast_ = Candidates.FindByPredicate([&ClassKey](const FK2Candidate& C) { return C.Key == ClassKey; }))
				{
					return Cast_;
				}
			}
		}
		// A member of the Blueprint's own class named by a short form ("K2:Get:Combat",
		// "K2:CallFunction:Self:Fire", "K2:AddDelegate:OnHit") that a parent class declares: the palette
		// lists inherited members, keyed by the class declaring them.
		for (const FString& Inherited : InheritedMemberKeys(Blueprint, Key))
		{
			if (const FK2Candidate* Found = Candidates.FindByPredicate([&Inherited](const FK2Candidate& C) { return C.Key == Inherited; }))
			{
				return Found;
			}
		}
		if (const FK2Candidate* Declared = FindByDeclaringClass(Key, Candidates, OutError))
		{
			return Declared;
		}
		if (!OutError.Code.IsEmpty())
		{
			return nullptr;
		}
		if (!Key.Contains(TEXT("#")))
		{
			TArray<FString> Variants;
			const FString Prefix = Key + TEXT("#");
			for (const FK2Candidate& Candidate : Candidates)
			{
				if (Candidate.Key.StartsWith(Prefix))
				{
					Variants.Add(Candidate.Key);
				}
			}
			if (Variants.Num() > 0)
			{
				TArray<FString> Described;
				for (const FString& Variant : Variants)
				{
					if (const FK2Candidate* Each = Candidates.FindByPredicate([&Variant](const FK2Candidate& C) { return C.Key == Variant; }))
					{
						Described.Add(DescribeCandidate(*Each, Graph));
					}
				}
				OutError.Set(TEXT("Graph.ActionAmbiguous"), FString::Printf(
					TEXT("Action key %s matches %d actions; use one of these keys: %s."), *Key, Variants.Num(), *FString::Join(Described, TEXT(" | "))));
				OutError.AddContext(TEXT("candidates"), MakeStringArray(Variants));
				OutError.AddContext(TEXT("described"), MakeStringArray(Described));
				return nullptr;
			}
		}
		OutError.Set(TEXT("Graph.ActionNotFound"), FString::Printf(TEXT("No K2 action has key %s."), *Key));
		return nullptr;
	}
};
} // namespace

TSharedRef<IAssetDocumentGraphSchemaAdapter> MakeK2SchemaAdapter()
{
	return MakeShared<FK2SchemaAdapter>();
}

namespace K2Keys
{
bool CompareCachedActionList(UBlueprint& Blueprint, UEdGraph& Graph, FString& OutDifference)
{
	TArray<FK2Candidate> Cached;
	CollectCandidates(Blueprint, Graph, Cached);
	TArray<FK2Candidate> Full;
	ComputeK2Candidates(Blueprint, Graph, Full);
	const auto Describe = [](const FK2Candidate& Candidate)
	{
		return FString::Printf(TEXT("%s (%s, owner %s, %d binding(s))"), *Candidate.Key,
			Candidate.Spawner != nullptr ? *Candidate.Spawner->GetName() : TEXT("no spawner"),
			*OwnerPath(Candidate.DatabaseOwner), Candidate.Bindings.Num());
	};
	for (int32 Index = 0; Index < FMath::Max(Cached.Num(), Full.Num()); ++Index)
	{
		const FK2Candidate* A = Cached.IsValidIndex(Index) ? &Cached[Index] : nullptr;
		const FK2Candidate* B = Full.IsValidIndex(Index) ? &Full[Index] : nullptr;
		const bool bSame = A != nullptr && B != nullptr && A->Key.Equals(B->Key, ESearchCase::CaseSensitive) && A->Spawner == B->Spawner
			&& A->DatabaseOwner == B->DatabaseOwner && A->bBound == B->bBound && A->Bindings.Num() == B->Bindings.Num()
			&& A->Bindings.Includes(B->Bindings);
		if (!bSame)
		{
			OutDifference = FString::Printf(TEXT("entry %d of %d/%d: cached %s, full %s"), Index, Cached.Num(), Full.Num(),
				A != nullptr ? *Describe(*A) : TEXT("none"), B != nullptr ? *Describe(*B) : TEXT("none"));
			return false;
		}
	}
	OutDifference.Reset();
	return true;
}
} // namespace K2Keys
} // namespace UE::AssetDocument::Graph
