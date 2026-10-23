// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Anim graph schema adapter (UAnimationGraphSchema and the K2-derived rule-graph
// schemas). Anim graph nodes (UAnimGraphNode_Base) are keyed "Anim:<Kind>[:<Arg>]":
//   Anim:<Kind>                    any anim node class the editor menu offers in the graph
//   Anim:SaveCachedPose:<Cache>    "New Save cached pose" + rename to <Cache>
//   Anim:UseCachedPose:<Cache>     "Use cached pose '<Cache>'"
//   Anim:LinkedAnimLayer:<Layer>   "<Layer> - Linked Anim Layer" (self or interface layer)
//   Anim:BlendSpaceGraph:<Class>   "Blend Space" / "Blend Space 1D" (a blend space embedded in the
//                                  Animation Blueprint; RotationOffsetBlendSpaceGraph for aim offsets)
//   Anim:LinkedInputPose           in an anim layer: the layer's Inputs "+" (Details of the layer graph)
// Every other node (variable getters, math, transition rule getters...) is a K2
// node and is delegated to the K2 adapter, so its keys stay "K2:...".
// DescribeNode reports extras.properties: the node's non-default editable values as
// "Node.<Field>" / "<Property>", "ShowPin.<Property>" for exposed-pin toggles and
// "Bindings" for property bindings; SetNodeSetting applies ShowPin.* and Bindings.
// A Control Rig node (AnimGraphNode_ControlRig) offers the rig's public variables and animatable controls as input
// pins only once they are exposed (Details > Input, the "use pin" checkbox: CustomPinProperties, not exposed by
// default, also for a variable the rig gets later): "ShowPin.<Variable>" reads and writes that checkbox. Writing it
// first refreshes the node's list from the rig as the Details panel does when it opens.
// Blend Space Graph nodes add their blend space's settings ("BlendSpace.*") and "Samples"
// (AssetDocumentAnimBlendSpaceGraphRawEdits); the sample graphs are reported as the node's owned
// subgraphs (role "blend_space_sample"). The Output Pose of an anim layer reports "LayerGroup"
// (the layer graph's Details > Group) when it is not the default shared group.
// Implicit roles: "root" (Output Pose, state result, custom blend result, blend space sample
// result), "result" (transition rule result), "source_pose" / "destination_pose" (the two pose
// evaluators of a custom transition blend graph).

#include "AnimBlueprint/AssetDocumentAnimBlueprintAdapters.h"
#include "AnimBlueprint/AssetDocumentAnimBlendSpaceGraphRawEdits.h"
#include "AnimBlueprint/AssetDocumentAnimGraphNodeBindingRawEdits.h"

#include "Algo/Find.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_CustomProperty.h"
#include "AnimGraphNode_BlendListByInt.h"
#include "AnimGraphNode_BlendSpaceGraphBase.h"
#include "AnimGraphNode_BlendSpaceSampleResult.h"
#include "AnimGraphNode_CustomTransitionResult.h"
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimGraphNode_LinkedAnimLayer.h"
#include "AnimGraphNode_LinkedInputPose.h"
#include "AnimGraphNode_MultiWayBlend.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimGraphNode_TransitionPoseEvaluator.h"
#include "AnimGraphNode_TransitionResult.h"
#include "AnimGraphNode_UseCachedPose.h"
#include "AnimationGraphSchema.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/BlendSpace.h"
#include "BlueprintActionDatabase.h"
#include "BlueprintNodeSpawner.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphSchema_K2_Actions.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::AnimBlueprint
{
TSharedPtr<IAssetDocumentGraphSchemaAdapter> FindK2SchemaAdapter()
{
	return FindAssetDocumentGraphSchemaAdapter(GetDefault<UEdGraphSchema_K2>());
}

namespace AnimGraphAdapterDetail
{
const FString AnimPrefix = TEXT("Anim:");

FString KindOf(const UClass* NodeClass)
{
	FString Kind = NodeClass != nullptr ? NodeClass->GetName() : FString();
	Kind.RemoveFromStart(TEXT("AnimGraphNode_"));
	return Kind;
}

bool IsAnimNode(const UEdGraphNode& Node)
{
	return Node.IsA<UAnimGraphNode_Base>();
}

// "Anim:<Kind>[:<Arg>]".
bool SplitKey(const FString& Key, FString& OutKind, FString& OutArg)
{
	if (!Key.StartsWith(AnimPrefix))
	{
		return false;
	}
	const FString Rest = Key.Mid(AnimPrefix.Len());
	if (!Rest.Split(TEXT(":"), &OutKind, &OutArg))
	{
		OutKind = Rest;
		OutArg.Reset();
	}
	return !OutKind.IsEmpty();
}

UBlueprint* BlueprintOf(UObject* Asset, const UEdGraph* Graph)
{
	if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
	{
		return Blueprint;
	}
	return Graph != nullptr ? FBlueprintEditorUtils::FindBlueprintForGraph(Graph) : nullptr;
}

FName LinkedLayerName(const UEdGraphNode& Node)
{
	const FStructProperty* NodeProperty = FindFProperty<FStructProperty>(Node.GetClass(), TEXT("Node"));
	const FNameProperty* Layer = NodeProperty != nullptr ? FindFProperty<FNameProperty>(NodeProperty->Struct, TEXT("Layer")) : nullptr;
	if (Layer == nullptr)
	{
		return NAME_None;
	}
	return Layer->GetPropertyValue_InContainer(NodeProperty->ContainerPtrToValuePtr<void>(&Node));
}

FString NodeKey(const UEdGraphNode& Node)
{
	const FString Kind = KindOf(Node.GetClass());
	if (const UAnimGraphNode_SaveCachedPose* Save = Cast<UAnimGraphNode_SaveCachedPose>(&Node))
	{
		return AnimPrefix + Kind + TEXT(":") + Save->CacheName;
	}
	if (const UAnimGraphNode_UseCachedPose* Use = Cast<UAnimGraphNode_UseCachedPose>(&Node))
	{
		return Use->SaveCachedPoseNode.IsValid() ? AnimPrefix + Kind + TEXT(":") + Use->SaveCachedPoseNode->CacheName : AnimPrefix + Kind;
	}
	if (Node.IsA<UAnimGraphNode_LinkedAnimLayer>())
	{
		const FName Layer = LinkedLayerName(Node);
		return Layer.IsNone() ? AnimPrefix + Kind : AnimPrefix + Kind + TEXT(":") + Layer.ToString();
	}
	if (const UAnimGraphNode_BlendSpaceGraphBase* BlendSpaceNode = Cast<UAnimGraphNode_BlendSpaceGraphBase>(&Node))
	{
		const UBlendSpace* BlendSpace = BlendSpaceRawEdits::GetBlendSpace(*BlendSpaceNode);
		return BlendSpace != nullptr ? AnimPrefix + Kind + TEXT(":") + BlendSpace->GetClass()->GetName() : AnimPrefix + Kind;
	}
	return AnimPrefix + Kind;
}

// The blend space classes UAnimGraphNode_AssetPlayerBase::GetMenuActionsHelper registers a
// Blend Space Graph node class for ("Blend Space", "Blend Space 1D", ...): plain spawners keyed
// by the blend space class whose customize delegate is SetupFromClass.
TMap<FString, UBlueprintNodeSpawner*> BlendSpaceClassSpawners(const UClass* NodeClass)
{
	TMap<FString, UBlueprintNodeSpawner*> Out;
	for (const TPair<FObjectKey, FBlueprintActionDatabase::FActionList>& Pair : FBlueprintActionDatabase::Get().GetAllActions())
	{
		const UClass* Key = Cast<UClass>(Pair.Key.ResolveObjectPtr());
		if (Key == nullptr || !Key->IsChildOf(UBlendSpace::StaticClass()))
		{
			continue;
		}
		for (UBlueprintNodeSpawner* Spawner : Pair.Value)
		{
			if (Spawner != nullptr && Spawner->NodeClass == NodeClass && Spawner->GetClass() == UBlueprintNodeSpawner::StaticClass())
			{
				Out.Add(Key->GetName(), Spawner);
			}
		}
	}
	Out.KeySort([](const FString& A, const FString& B) { return A < B; });
	return Out;
}

bool IsBlendSpaceGraphClass(const UClass* NodeClass)
{
	return NodeClass != nullptr && NodeClass->IsChildOf(UAnimGraphNode_BlendSpaceGraphBase::StaticClass());
}

// An Animation Blueprint's own anim layer (or an Anim Layer Interface's layer) whose inputs the
// graph's Details panel edits; implemented interface layers of an Animation Blueprint are read-only.
bool IsEditableLayerGraph(const UEdGraph& Graph)
{
	const UAnimBlueprint* Blueprint = Cast<UAnimBlueprint>(Graph.GetOuter());
	return Blueprint != nullptr && Graph.GetFName() != UEdGraphSchema_K2::GN_AnimGraph && Graph.bAllowDeletion
		&& Graph.Schema != nullptr && Graph.Schema->IsChildOf(UAnimationGraphSchema::StaticClass())
		&& Blueprint->FunctionGraphs.Contains(&Graph);
}

bool IsLayerRoot(const UEdGraphNode& Node)
{
	const UEdGraph* Graph = Node.GetGraph();
	return Node.GetClass() == UAnimGraphNode_Root::StaticClass() && Graph != nullptr && Graph->GetOuter()->IsA<UAnimBlueprint>()
		&& Graph->GetFName() != UEdGraphSchema_K2::GN_AnimGraph;
}

// A Control Rig anim node (ControlRigDeveloper, found by path: this module does not depend on the plugin).
bool IsControlRigNode(const UEdGraphNode& Node)
{
	const UClass* ControlRigNode = FindObject<UClass>(nullptr, TEXT("/Script/ControlRigDeveloper.AnimGraphNode_ControlRig"));
	return ControlRigNode != nullptr && Node.IsA(ControlRigNode);
}

// A Control Rig node's rig: some engine versions name it by class ("Node.ControlRigClass", a class path), others by
// an asset reference ("Node.ControlRigAssetReference": {"BlueprintRigClass": <class path>, "ControlRigAsset":
// <rig asset>}). A setting in either spelling is rewritten to the one the node has; a rig asset (ControlRigAsset)
// has no class spelling and is refused on a node that names its rig by class.
bool MapControlRigSetting(const UEdGraphNode& Node, FString& InOutPath, FAssetDocumentRuntimeValueDto& InOutValue,
	FAssetDocumentGraphError& OutError)
{
	static const FString ClassPath = TEXT("Node.ControlRigClass");
	static const FString ReferencePath = TEXT("Node.ControlRigAssetReference");
	const bool bClass = InOutPath == ClassPath;
	if ((!bClass && InOutPath != ReferencePath) || !IsControlRigNode(Node))
	{
		return true;
	}
	// The node names its rig by reference when it has the reference (it may keep a retired class field beside it).
	const FStructProperty* NodeProperty = FindFProperty<FStructProperty>(Node.GetClass(), TEXT("Node"));
	if (NodeProperty == nullptr || NodeProperty->Struct == nullptr
		|| (NodeProperty->Struct->FindPropertyByName(TEXT("ControlRigAssetReference")) != nullptr) != bClass)
	{
		return true;
	}
	if (bClass)
	{
		FAssetDocumentRuntimeValueDto Reference;
		Reference.Kind = EAssetDocumentRuntimeValueKind::Object;
		Reference.ObjectKeys = {TEXT("BlueprintRigClass"), TEXT("ControlRigAsset")};
		Reference.Elements = {MoveTemp(InOutValue), FAssetDocumentRuntimeValueDto()};
		InOutValue = MoveTemp(Reference);
		InOutPath = ReferencePath;
		return true;
	}
	FAssetDocumentRuntimeValueDto RigClass;
	if (InOutValue.Kind == EAssetDocumentRuntimeValueKind::Object)
	{
		for (int32 Index = 0; Index < InOutValue.ObjectKeys.Num(); ++Index)
		{
			const FString& Key = InOutValue.ObjectKeys[Index];
			const FAssetDocumentRuntimeValueDto& Item = InOutValue.Elements[Index];
			if (Key == TEXT("BlueprintRigClass"))
			{
				RigClass = Item;
			}
			else if (Key != TEXT("ControlRigAsset"))
			{
				OutError.Set(TEXT("Graph.InvalidArgument"), FString::Printf(
					TEXT("ControlRigAssetReference has the fields BlueprintRigClass and ControlRigAsset (not '%s')."), *Key),
					FString(TEXT("/value/")) + Key);
				return false;
			}
			else if (Item.Kind != EAssetDocumentRuntimeValueKind::Null)
			{
				OutError.Set(TEXT("Graph.InvalidArgument"),
					TEXT("This engine's Control Rig node runs a rig class (Node.ControlRigClass): a rig asset (ControlRigAsset) "
						"cannot be set; give the rig's class as BlueprintRigClass."), TEXT("/value/ControlRigAsset"));
				return false;
			}
		}
	}
	else if (InOutValue.Kind != EAssetDocumentRuntimeValueKind::Null)
	{
		OutError.Set(TEXT("Graph.InvalidArgument"),
			TEXT("ControlRigAssetReference is {\"BlueprintRigClass\": <rig class>, \"ControlRigAsset\": null} or null."), TEXT("/value"));
		return false;
	}
	InOutValue = MoveTemp(RigClass);
	InOutPath = ClassPath;
	return true;
}

// The rig variables and controls a Control Rig node can expose as pins, with their "use pin" state
// (UAnimGraphNode_CustomProperty::CustomPinProperties, protected; read through its property). Null for other nodes.
TArray<FOptionalPinFromProperty>* ExposablePins(const UEdGraphNode& Node)
{
	if (!IsControlRigNode(Node))
	{
		return nullptr;
	}
	const FArrayProperty* Property = CastField<FArrayProperty>(
		UAnimGraphNode_CustomProperty::StaticClass()->FindPropertyByName(TEXT("CustomPinProperties")));
	const FStructProperty* Inner = Property != nullptr ? CastField<FStructProperty>(Property->Inner) : nullptr;
	if (Inner == nullptr || Inner->Struct == nullptr || Inner->Struct->GetFName() != TEXT("OptionalPinFromProperty"))
	{
		return nullptr;
	}
	return const_cast<TArray<FOptionalPinFromProperty>*>(Property->ContainerPtrToValuePtr<TArray<FOptionalPinFromProperty>>(&Node));
}

int32 FindExposablePin(const TArray<FOptionalPinFromProperty>& Pins, FStringView Name)
{
	return Pins.IndexOfByPredicate([Name](const FOptionalPinFromProperty& Pin)
	{
		return Pin.bCanToggleVisibility && Pin.PropertyName.ToString() == Name;
	});
}

// Exposing a variable clears its curve mapping (FControlRigIOMapping::OnPropertyExposeCheckboxChanged:
// SetIOMapping(Input, Name, None)); FAnimNode_ControlRig::InputMapping is private, written through its property.
void ClearInputMapping(UAnimGraphNode_Base& Node, FName Name)
{
	const FStructProperty* NodeProperty = Node.GetFNodeProperty();
	const FMapProperty* Mapping = NodeProperty != nullptr
		? CastField<FMapProperty>(NodeProperty->Struct->FindPropertyByName(TEXT("InputMapping"))) : nullptr;
	if (Mapping == nullptr || !Mapping->KeyProp->IsA<FNameProperty>())
	{
		return;
	}
	FScriptMapHelper Map(Mapping, Mapping->ContainerPtrToValuePtr<void>(NodeProperty->ContainerPtrToValuePtr<void>(&Node)));
	Map.RemovePair(&Name);
}

// The Details panel's Input "use pin" checkbox of a Control Rig node (UAnimGraphNode_ControlRig::
// OnPropertyExposeCheckboxChanged): the state flips, the node is rebuilt without keeping the hidden pin as an orphan,
// an exposed variable loses its curve mapping and the Blueprint is structurally modified.
void SetExposedPin(UAnimGraphNode_Base& Node, int32 Index, bool bShow)
{
	TArray<FOptionalPinFromProperty>& Pins = *ExposablePins(Node);
	if (Pins[Index].bShowPin == bShow)
	{
		return;
	}
	const FName Name = Pins[Index].PropertyName;
	Node.Modify();
	Pins[Index].bShowPin = bShow;
	{
		FGuardValue_Bitfield(Node.bDisableOrphanPinSaving, true);
		Node.ReconstructNode();
	}
	if (bShow)
	{
		ClearInputMapping(Node, Name);
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Node.GetBlueprint());
}

// The first evaluator of each data source in a custom transition blend graph (the two the
// schema creates; they cannot be deleted or added by the user).
FString PoseEvaluatorRole(const UAnimGraphNode_TransitionPoseEvaluator& Node)
{
	const EEvaluatorDataSource::Type Source = Node.Node.DataSource;
	for (const UEdGraphNode* Other : Node.GetGraph()->Nodes)
	{
		const UAnimGraphNode_TransitionPoseEvaluator* Evaluator = Cast<UAnimGraphNode_TransitionPoseEvaluator>(Other);
		if (Evaluator != nullptr && Evaluator->Node.DataSource == Source)
		{
			if (Evaluator != &Node)
			{
				return FString();
			}
			break;
		}
	}
	return Source == EEvaluatorDataSource::EDS_DestinationPose ? TEXT("destination_pose") : TEXT("source_pose");
}

// The anim node classes the editor offers in Graph (registered actions, compatible with the graph).
void CollectAnimClasses(const UEdGraph& Graph, TArray<UClass*>& Out)
{
	Out.Reset();
	const UEdGraphSchema* Schema = Graph.GetSchema();
	for (const TPair<FObjectKey, FBlueprintActionDatabase::FActionList>& Pair : FBlueprintActionDatabase::Get().GetAllActions())
	{
		for (UBlueprintNodeSpawner* Spawner : Pair.Value)
		{
			UClass* NodeClass = Spawner != nullptr ? Spawner->NodeClass.Get() : nullptr;
			if (NodeClass == nullptr || Out.Contains(NodeClass) || !NodeClass->IsChildOf(UAnimGraphNode_Base::StaticClass())
				|| NodeClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				continue;
			}
			const UEdGraphNode* Defaults = NodeClass->GetDefaultObject<UEdGraphNode>();
			if (Schema != nullptr && Defaults->CanCreateUnderSpecifiedSchema(Schema) && Defaults->IsCompatibleWithGraph(&Graph))
			{
				Out.Add(NodeClass);
			}
		}
	}
	Out.Sort([](const UClass& A, const UClass& B) { return A.GetName() < B.GetName(); });
}

TArray<UAnimGraphNode_SaveCachedPose*> CachedPoses(UBlueprint& Blueprint)
{
	TArray<UAnimGraphNode_SaveCachedPose*> Nodes;
	FBlueprintEditorUtils::GetAllNodesOfClass<UAnimGraphNode_SaveCachedPose>(&Blueprint, Nodes);
	return Nodes;
}

// The layer spawners UAnimGraphNode_LinkedAnimLayer::GetMenuActions registers for Blueprint
// (FBlueprintActionDatabase::RefreshAssetActions): its own layers are keyed by the Animation
// Blueprint, implemented layer-interface layers by the interface Blueprint. The layer of a spawner
// is what its customize delegate (SetupFromLayerId) gives the spawner's template node.
TMap<FName, UBlueprintNodeSpawner*> LayerSpawners(UBlueprint& Blueprint)
{
	TSet<FObjectKey> Keys;
	Keys.Add(FObjectKey(&Blueprint));
	for (const FBPInterfaceDescription& Interface : Blueprint.ImplementedInterfaces)
	{
		if (UObject* InterfaceBlueprint = Interface.Interface != nullptr ? Interface.Interface->ClassGeneratedBy.Get() : nullptr)
		{
			Keys.Add(FObjectKey(InterfaceBlueprint));
		}
	}
	TMap<FName, UBlueprintNodeSpawner*> Out;
	const FBlueprintActionDatabase::FActionRegistry& Actions = FBlueprintActionDatabase::Get().GetAllActions();
	for (const FObjectKey& Key : Keys)
	{
		const FBlueprintActionDatabase::FActionList* List = Actions.Find(Key);
		for (UBlueprintNodeSpawner* Spawner : List != nullptr ? *List : FBlueprintActionDatabase::FActionList())
		{
			if (Spawner == nullptr || Spawner->NodeClass != UAnimGraphNode_LinkedAnimLayer::StaticClass())
			{
				continue;
			}
			const UEdGraphNode* Template = Spawner->GetTemplateNode();
			const FName Layer = Template != nullptr ? LinkedLayerName(*Template) : NAME_None;
			if (!Layer.IsNone())
			{
				Out.Add(Layer, Spawner);
			}
		}
	}
	return Out;
}

bool IsNodeEditable(const FProperty& Property)
{
	return Property.HasAnyPropertyFlags(CPF_Edit)
		&& !Property.HasAnyPropertyFlags(CPF_EditConst | CPF_Transient | CPF_Deprecated | CPF_DisableEditOnInstance);
}

bool DefaultShowPin(const UAnimGraphNode_Base& Node, FName PropertyName)
{
	const FStructProperty* NodeProperty = Node.GetFNodeProperty();
	const FProperty* Property = NodeProperty != nullptr ? NodeProperty->Struct->FindPropertyByName(PropertyName) : nullptr;
	return Property != nullptr && (Property->HasMetaData(TEXT("PinShownByDefault")) || Property->HasMetaData(TEXT("AlwaysAsPin")));
}

// Properties that are represented otherwise (key, pins, ShowPin.*, Bindings) or not authored.
bool IsSkippedTopLevel(const UAnimGraphNode_Base& Node, const FProperty& Property)
{
	static const FName Skipped[] = {
		TEXT("ShowPinForProperties"), TEXT("Binding"), TEXT("InitialUpdateFunction"), TEXT("BecomeRelevantFunction"),
		TEXT("UpdateFunction"), TEXT("BlueprintUsage"), TEXT("InterfaceGuid")};
	if (Algo::Find(Skipped, Property.GetFName()) != nullptr)
	{
		return true;
	}
	return Node.IsA<UAnimGraphNode_SaveCachedPose>() && Property.GetFName() == TEXT("CacheName");
}

bool IsSkippedNodeField(const UAnimGraphNode_Base& Node, const FProperty& Field)
{
	return (Node.IsA<UAnimGraphNode_LinkedAnimLayer>() && Field.GetFName() == TEXT("Layer"))
		|| (Node.IsA<UAnimGraphNode_TransitionPoseEvaluator>() && Field.GetFName() == TEXT("DataSource"));
}

// The node's settings that differ from a new node's (the node class defaults), and in OutDefaults
// the default of each reported setting that a document resets by omitting it (an omitted
// setting is the default). Structural settings (blend space samples, layer group) have none.
FValue CollectNodeProperties(const UAnimGraphNode_Base& Node, FValue& OutDefaults)
{
	FValue Out = MakeObjectValue();
	OutDefaults = MakeObjectValue();
	const UObject* Defaults = Node.GetClass()->GetDefaultObject();
	TSet<FName> Shown;
	for (const FOptionalPinFromProperty& Pin : Node.ShowPinForProperties)
	{
		if (Pin.bShowPin)
		{
			Shown.Add(Pin.PropertyName);
		}
	}
	const FStructProperty* NodeProperty = Node.GetFNodeProperty();
	for (TFieldIterator<FProperty> It(Node.GetClass()); It; ++It)
	{
		const FProperty& Property = **It;
		if (!IsNodeEditable(Property) || IsSkippedTopLevel(Node, Property))
		{
			continue;
		}
		const void* Value = Property.ContainerPtrToValuePtr<void>(&Node);
		const void* Default = Property.ContainerPtrToValuePtr<void>(Defaults);
		if (&Property == NodeProperty)
		{
			for (TFieldIterator<FProperty> Field(NodeProperty->Struct); Field; ++Field)
			{
				if (!IsNodeEditable(**Field) || Shown.Contains(Field->GetFName()) || IsSkippedNodeField(Node, **Field))
				{
					continue;
				}
				const void* FieldValue = Field->ContainerPtrToValuePtr<void>(Value);
				const void* FieldDefault = Field->ContainerPtrToValuePtr<void>(Default);
				if (!Field->Identical(FieldValue, FieldDefault, PPF_None))
				{
					SetField(Out, Property.GetName() + TEXT(".") + Field->GetName(), EncodeAssetDocumentGraphPropertyValue(**Field, FieldValue));
					SetField(OutDefaults, Property.GetName() + TEXT(".") + Field->GetName(), EncodeAssetDocumentGraphPropertyValue(**Field, FieldDefault));
				}
			}
			continue;
		}
		if (!Property.Identical(Value, Default, PPF_None))
		{
			SetField(Out, Property.GetName(), EncodeAssetDocumentGraphPropertyValue(Property, Value));
			SetField(OutDefaults, Property.GetName(), EncodeAssetDocumentGraphPropertyValue(Property, Default));
		}
	}
	for (const FOptionalPinFromProperty& Pin : Node.ShowPinForProperties)
	{
		if (Pin.bCanToggleVisibility && Pin.bShowPin != DefaultShowPin(Node, Pin.PropertyName))
		{
			SetField(Out, TEXT("ShowPin.") + Pin.PropertyName.ToString(), MakeBoolValue(Pin.bShowPin));
			SetField(OutDefaults, TEXT("ShowPin.") + Pin.PropertyName.ToString(), MakeBoolValue(!Pin.bShowPin));
		}
	}
	if (const TArray<FOptionalPinFromProperty>* Exposable = ExposablePins(Node))
	{
		for (const FOptionalPinFromProperty& Pin : *Exposable)
		{
			if (Pin.bCanToggleVisibility && Pin.bShowPin)
			{
				SetField(Out, TEXT("ShowPin.") + Pin.PropertyName.ToString(), MakeBoolValue(true));
				SetField(OutDefaults, TEXT("ShowPin.") + Pin.PropertyName.ToString(), MakeBoolValue(false));
			}
		}
	}
	FValue Bindings = BindingRawEdits::QueryNodeBindings(Node);
	if (Bindings.Kind == EAssetDocumentRuntimeValueKind::Object)
	{
		SetField(Out, TEXT("Bindings"), MoveTemp(Bindings));
		SetField(OutDefaults, TEXT("Bindings"), MakeObjectValue());
	}
	if (const UAnimGraphNode_BlendSpaceGraphBase* BlendSpaceNode = Cast<UAnimGraphNode_BlendSpaceGraphBase>(&Node))
	{
		BlendSpaceRawEdits::QueryBlendSpace(*BlendSpaceNode, Out);
	}
	if (IsLayerRoot(Node))
	{
		const FName Group = CastChecked<UAnimGraphNode_Root>(&Node)->Node.GetGroup();
		if (Group != FAnimNode_Root::DefaultSharedGroup)
		{
			SetField(Out, TEXT("LayerGroup"), MakeStringValue(Group.IsNone() ? FString() : Group.ToString()));
		}
	}
	return Out;
}

FVector2D SpawnLocation(const FAssetDocumentGraphSpawnRequest& Request)
{
	if (Request.Position.IsSet())
	{
		return Request.Position.GetValue();
	}
	int32 MaxX = 0;
	for (const UEdGraphNode* Node : Request.Graph->Nodes)
	{
		if (Node != nullptr)
		{
			MaxX = FMath::Max(MaxX, Node->NodePosX + 320);
		}
	}
	return FVector2D(MaxX, 0.0);
}

// The layer graph Details' Inputs "+" (FAnimGraphDetails::OnAddNewInputPoseClicked): a Linked
// Input Pose node (PostPlacedNewNode names it uniquely), then the self layer nodes are rebuilt
// with the same result as the editor's own rebuild after that click.
bool SpawnLayerInputPose(const FAssetDocumentGraphSpawnRequest& Request, UBlueprint& Blueprint,
	FAssetDocumentGraphSpawnResult& OutResult, FAssetDocumentGraphError& OutError)
{
	const bool bInterface = Blueprint.BlueprintType == BPTYPE_Interface;
	const FVector2D Location = Request.Position.IsSet() ? Request.Position.GetValue()
		: (bInterface ? FVector2D::ZeroVector : UAnimationGraphSchema::GetPositionForNewLinkedInputPoseNode(*Request.Graph));
	UAnimGraphNode_LinkedInputPose* Node = FEdGraphSchemaAction_K2NewNode::SpawnNode<UAnimGraphNode_LinkedInputPose>(
		Request.Graph, Location, EK2NewNodeFlags::None);
	if (Node == nullptr)
	{
		OutError.Set(TEXT("Graph.ActionNotFound"), FString::Printf(TEXT("Action %s did not produce a node."), *Request.ActionKey));
		return false;
	}
	// UAnimGraphNode_LinkedInputPose::ReconstructLayerNodes (private): self layer nodes pick up the new pose pin.
	TArray<UAnimGraphNode_LinkedAnimLayer*> LayerNodes;
	FBlueprintEditorUtils::GetAllNodesOfClass<UAnimGraphNode_LinkedAnimLayer>(&Blueprint, LayerNodes);
	for (UAnimGraphNode_LinkedAnimLayer* LayerNode : LayerNodes)
	{
		if (LayerNode->Node.Interface.Get() == nullptr)
		{
			LayerNode->ReconstructNode();
		}
	}
	OutResult.Node = Node;
	OutResult.bReused = false;
	return true;
}

class FAnimGraphSchemaAdapter final : public IAssetDocumentGraphSchemaAdapter
{
public:
	explicit FAnimGraphSchemaAdapter(UClass* InSchemaClass)
		: SchemaClass(InSchemaClass)
	{
	}

	virtual UClass* GetSchemaClass() const override
	{
		return SchemaClass;
	}

	virtual FString Tag(const UEdGraph& Graph) const override
	{
		return TEXT("Anim");
	}

	virtual bool EnumerateActions(
		const FAssetDocumentGraphActionQuery& Query,
		TArray<FAssetDocumentGraphActionRecord>& OutActions,
		FAssetDocumentGraphError& OutError) const override
	{
		const TSharedPtr<IAssetDocumentGraphSchemaAdapter> K2 = FindK2SchemaAdapter();
		UBlueprint* Blueprint = BlueprintOf(Query.Asset, Query.Graph);
		if (!K2.IsValid() || Blueprint == nullptr || Query.Graph == nullptr)
		{
			OutError.Set(TEXT("Graph.GraphOperationUnsupported"), TEXT("Anim graph actions need an Animation Blueprint graph."));
			return false;
		}
		// K2 actions of the graph (getters, math, rule getters); anim node actions get Anim: keys instead.
		TArray<FAssetDocumentGraphActionRecord> K2Actions;
		if (!K2->EnumerateActions(Query, K2Actions, OutError))
		{
			return false;
		}
		for (FAssetDocumentGraphActionRecord& Record : K2Actions)
		{
			const UClass* NodeClass = FindObject<UClass>(nullptr, *Record.NodeClassPath);
			if (NodeClass == nullptr || !NodeClass->IsChildOf(UAnimGraphNode_Base::StaticClass()))
			{
				OutActions.Add(MoveTemp(Record));
			}
		}
		const TSet<FString> Exact(Query.ExactKeys);
		auto Offer = [&](const FString& Key, const UClass* NodeClass, const FString& Display)
		{
			if ((!Query.KeyPrefix.IsEmpty() && !Key.StartsWith(Query.KeyPrefix)) || (!Exact.IsEmpty() && !Exact.Contains(Key)))
			{
				return;
			}
			FAssetDocumentGraphActionRecord& Record = OutActions.AddDefaulted_GetRef();
			Record.ActionKey = Key;
			Record.NodeClassPath = NodeClass->GetPathName();
			if (Query.bWantUiText || !Query.Text.IsEmpty())
			{
				Record.DisplayName = Display;
				Record.Category = TEXT("Animation");
				Record.Tooltip = NodeClass->GetToolTipText().ToString();
			}
		};
		TArray<UClass*> Classes;
		CollectAnimClasses(*Query.Graph, Classes);
		for (UClass* NodeClass : Classes)
		{
			if (!IsBlendSpaceGraphClass(NodeClass))
			{
				Offer(AnimPrefix + KindOf(NodeClass), NodeClass, NodeClass->GetDisplayNameText().ToString());
				continue;
			}
			for (const TPair<FString, UBlueprintNodeSpawner*>& Pair : BlendSpaceClassSpawners(NodeClass))
			{
				Offer(AnimPrefix + KindOf(NodeClass) + TEXT(":") + Pair.Key, NodeClass,
					Pair.Value->DefaultMenuSignature.MenuName.IsEmpty() ? Pair.Key : Pair.Value->DefaultMenuSignature.MenuName.ToString());
			}
		}
		if (IsEditableLayerGraph(*Query.Graph))
		{
			Offer(AnimPrefix + KindOf(UAnimGraphNode_LinkedInputPose::StaticClass()), UAnimGraphNode_LinkedInputPose::StaticClass(),
				TEXT("Input Pose"));
		}
		if (Classes.Contains(UAnimGraphNode_UseCachedPose::StaticClass()))
		{
			for (const UAnimGraphNode_SaveCachedPose* Save : CachedPoses(*Blueprint))
			{
				Offer(AnimPrefix + TEXT("UseCachedPose:") + Save->CacheName, UAnimGraphNode_UseCachedPose::StaticClass(),
					FString::Printf(TEXT("Use cached pose '%s'"), *Save->CacheName));
			}
		}
		if (Classes.Contains(UAnimGraphNode_LinkedAnimLayer::StaticClass()))
		{
			for (const TPair<FName, UBlueprintNodeSpawner*>& Pair : LayerSpawners(*Blueprint))
			{
				Offer(AnimPrefix + TEXT("LinkedAnimLayer:") + Pair.Key.ToString(), UAnimGraphNode_LinkedAnimLayer::StaticClass(),
					FString::Printf(TEXT("%s - Linked Anim Layer"), *Pair.Key.ToString()));
			}
		}
		return true;
	}

	virtual bool ResolveAndSpawn(
		const FAssetDocumentGraphSpawnRequest& Request,
		FAssetDocumentGraphSpawnResult& OutResult,
		FAssetDocumentGraphError& OutError) const override
	{
		FString Kind;
		FString Arg;
		if (!SplitKey(Request.ActionKey, Kind, Arg))
		{
			const TSharedPtr<IAssetDocumentGraphSchemaAdapter> K2 = FindK2SchemaAdapter();
			return K2.IsValid() && K2->ResolveAndSpawn(Request, OutResult, OutError);
		}
		UBlueprint* Blueprint = BlueprintOf(Request.Asset, Request.Graph);
		if (Blueprint == nullptr || Request.Graph == nullptr)
		{
			OutError.Set(TEXT("Graph.GraphOperationUnsupported"), TEXT("Anim graph actions need an Animation Blueprint graph."));
			return false;
		}
		if (Kind == KindOf(UAnimGraphNode_LinkedInputPose::StaticClass()) && IsEditableLayerGraph(*Request.Graph))
		{
			return SpawnLayerInputPose(Request, *Blueprint, OutResult, OutError);
		}
		TArray<UClass*> Classes;
		CollectAnimClasses(*Request.Graph, Classes);
		UClass* const* Found = Classes.FindByPredicate([&Kind](const UClass* Class) { return KindOf(Class) == Kind; });
		if (Found == nullptr)
		{
			OutError.Set(TEXT("Graph.ActionNotFound"), FString::Printf(
				TEXT("Action %s is not an anim node the editor offers in graph %s."), *Request.ActionKey, *Request.Graph->GetName()));
			return false;
		}
		UClass* NodeClass = *Found;
		const FVector2D Location = SpawnLocation(Request);
		UBlueprintNodeSpawner* Spawner = nullptr;
		TStrongObjectPtr<UBlueprintNodeSpawner> Transient;
		if (NodeClass == UAnimGraphNode_UseCachedPose::StaticClass())
		{
			const TArray<UAnimGraphNode_SaveCachedPose*> Caches = CachedPoses(*Blueprint);
			UAnimGraphNode_SaveCachedPose* const* Save = Caches.FindByPredicate(
				[&Arg](const UAnimGraphNode_SaveCachedPose* Node) { return Node->CacheName == Arg; });
			if (Save == nullptr)
			{
				OutError.Set(TEXT("Graph.ActionNotFound"), FString::Printf(
					TEXT("No Save Cached Pose node named '%s' exists; add Anim:SaveCachedPose:%s first."), *Arg, *Arg));
				return false;
			}
			// UAnimGraphNode_UseCachedPose::GetMenuActions: one spawner per cache.
			TWeakObjectPtr<UAnimGraphNode_SaveCachedPose> Weak(*Save);
			Transient.Reset(UBlueprintNodeSpawner::Create(NodeClass));
			Transient->CustomizeNodeDelegate = UBlueprintNodeSpawner::FCustomizeNodeDelegate::CreateLambda(
				[Weak](UEdGraphNode* NewNode, bool)
				{
					CastChecked<UAnimGraphNode_UseCachedPose>(NewNode)->SaveCachedPoseNode = Weak;
				});
			Spawner = Transient.Get();
		}
		else if (NodeClass == UAnimGraphNode_LinkedAnimLayer::StaticClass() && !Arg.IsEmpty())
		{
			FBlueprintActionDatabase::Get().RefreshAssetActions(Blueprint);
			const TMap<FName, UBlueprintNodeSpawner*> Layers = LayerSpawners(*Blueprint);
			UBlueprintNodeSpawner* const* Layer = Layers.Find(FName(Arg));
			if (Layer == nullptr)
			{
				TArray<FName> Names;
				Layers.GetKeys(Names);
				OutError.Set(TEXT("Graph.ActionNotFound"), FString::Printf(
					TEXT("No anim layer '%s' is available to %s (compiled layers: %s)."), *Arg, *Blueprint->GetName(),
					*FString::JoinBy(Names, TEXT(", "), [](FName Name) { return Name.ToString(); })));
				return false;
			}
			Spawner = *Layer;
		}
		else if (IsBlendSpaceGraphClass(NodeClass))
		{
			const TMap<FString, UBlueprintNodeSpawner*> Spawners = BlendSpaceClassSpawners(NodeClass);
			UBlueprintNodeSpawner* const* ClassSpawner = Spawners.Find(Arg);
			if (ClassSpawner == nullptr)
			{
				TArray<FString> Names;
				Spawners.GetKeys(Names);
				OutError.Set(TEXT("Graph.ActionNotFound"), FString::Printf(TEXT("%s needs a blend space class: %s."), *Request.ActionKey,
					*FString::JoinBy(Names, TEXT(", "), [&Kind](const FString& Name) { return AnimPrefix + Kind + TEXT(":") + Name; })));
				return false;
			}
			Spawner = *ClassSpawner;
		}
		else if (!Arg.IsEmpty() && NodeClass != UAnimGraphNode_SaveCachedPose::StaticClass())
		{
			OutError.Set(TEXT("Graph.ActionNotFound"), FString::Printf(TEXT("Action %s takes no argument; use %s%s."),
				*Request.ActionKey, *AnimPrefix, *Kind));
			return false;
		}
		if (Spawner == nullptr)
		{
			// The class's own plain action (UAnimGraphNode_Base::GetMenuActions), else the
			// same plain spawner the menu's 'empty' asset-player entry uses.
			const FBlueprintActionDatabase::FActionList* Actions = FBlueprintActionDatabase::Get().GetAllActions().Find(FObjectKey(NodeClass));
			TArray<UBlueprintNodeSpawner*> Plain;
			for (UBlueprintNodeSpawner* Candidate : Actions != nullptr ? *Actions : FBlueprintActionDatabase::FActionList())
			{
				if (Candidate != nullptr && Candidate->NodeClass == NodeClass && Candidate->GetClass() == UBlueprintNodeSpawner::StaticClass())
				{
					Plain.Add(Candidate);
				}
			}
			if (Plain.Num() == 1)
			{
				Spawner = Plain[0];
			}
			else
			{
				Transient.Reset(UBlueprintNodeSpawner::Create(NodeClass));
				Spawner = Transient.Get();
			}
		}
		if (NodeClass == UAnimGraphNode_SaveCachedPose::StaticClass() && !Arg.IsEmpty()
			&& CachedPoses(*Blueprint).ContainsByPredicate([&Arg](const UAnimGraphNode_SaveCachedPose* Node) { return Node->CacheName == Arg; }))
		{
			OutError.Set(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("A cached pose named '%s' already exists."), *Arg));
			return false;
		}
		UEdGraphNode* Node = Spawner->Invoke(Request.Graph, IBlueprintNodeBinder::FBindingSet(), Location);
		if (Node == nullptr)
		{
			OutError.Set(TEXT("Graph.ActionNotFound"), FString::Printf(TEXT("Action %s did not produce a node."), *Request.ActionKey));
			return false;
		}
		if (UAnimGraphNode_SaveCachedPose* Save = Cast<UAnimGraphNode_SaveCachedPose>(Node); Save != nullptr && !Arg.IsEmpty())
		{
			// Renaming the new cache node in the graph editor.
			Save->Modify();
			Save->OnRenameNode(Arg);
		}
		OutResult.Node = Node;
		OutResult.bReused = false;
		if (Request.FromPin != nullptr)
		{
			Node->AutowireNewNode(Request.FromPin);
		}
		return true;
	}

	virtual void DescribeNode(const UEdGraphNode& Node, FAssetDocumentGraphNodeDescription& Out) const override
	{
		const UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(&Node);
		if (AnimNode == nullptr)
		{
			if (const TSharedPtr<IAssetDocumentGraphSchemaAdapter> K2 = FindK2SchemaAdapter())
			{
				K2->DescribeNode(Node, Out);
			}
			return;
		}
		Out.ActionKey = NodeKey(Node);
		if (Node.IsA<UAnimGraphNode_Root>() || Node.IsA<UAnimGraphNode_StateResult>() || Node.IsA<UAnimGraphNode_BlendSpaceSampleResult>())
		{
			Out.ImplicitRole = TEXT("root");
		}
		else if (Node.IsA<UAnimGraphNode_TransitionResult>())
		{
			Out.ImplicitRole = TEXT("result");
		}
		else if (const UAnimGraphNode_TransitionPoseEvaluator* Evaluator = Cast<UAnimGraphNode_TransitionPoseEvaluator>(&Node))
		{
			Out.ImplicitRole = PoseEvaluatorRole(*Evaluator);
		}
		FValue Extras = MakeObjectValue();
		if (Node.GetCanRenameNode() && !Node.IsA<UAnimGraphNode_SaveCachedPose>())
		{
			SetField(Extras, TEXT("name"), MakeStringValue(Node.GetNodeTitle(ENodeTitleType::EditableTitle).ToString()));
		}
		FValue PropertyDefaults;
		SetField(Extras, TEXT("properties"), CollectNodeProperties(*AnimNode, PropertyDefaults));
		SetField(Extras, TEXT("property_defaults"), MoveTemp(PropertyDefaults));
		Out.Extras = MoveTemp(Extras);
	}

	virtual bool RemoveNode(UObject& Asset, UEdGraphNode& Node, FAssetDocumentGraphError& OutError) const override
	{
		const TSharedPtr<IAssetDocumentGraphSchemaAdapter> K2 = FindK2SchemaAdapter();
		return K2.IsValid() && K2->RemoveNode(Asset, Node, OutError);
	}

	virtual FString ClassifySubgraph(const UEdGraph& Subgraph, const UEdGraphNode& OwnerNode) const override
	{
		if (OwnerNode.IsA<UAnimGraphNode_BlendSpaceGraphBase>())
		{
			return TEXT("blend_space_sample");
		}
		return OwnerNode.IsA<UAnimGraphNode_StateMachineBase>() ? TEXT("state_machine") : TEXT("other");
	}

	// A Blend Space Graph node owns its sample graphs; the dummy blend space graph that holds
	// them (navigation only, never has nodes) is not reported.
	virtual void GetOwnedSubgraphs(const UEdGraphNode& Node, TArray<UEdGraph*>& OutGraphs) const override
	{
		if (const UAnimGraphNode_BlendSpaceGraphBase* BlendSpaceNode = Cast<UAnimGraphNode_BlendSpaceGraphBase>(&Node))
		{
			OutGraphs = BlendSpaceRawEdits::GetSampleGraphs(*BlendSpaceNode);
			return;
		}
		IAssetDocumentGraphSchemaAdapter::GetOwnedSubgraphs(Node, OutGraphs);
	}

	virtual bool SetUserPins(
		UEdGraphNode& Node, TConstArrayView<FAssetDocumentGraphUserPin> Pins, FAssetDocumentGraphError& OutError) const override
	{
		const TSharedPtr<IAssetDocumentGraphSchemaAdapter> K2 = FindK2SchemaAdapter();
		return K2.IsValid() && K2->SetUserPins(Node, Pins, OutError);
	}

	virtual bool CanAddDynamicPin(const UEdGraphNode& Node) const override
	{
		if (IsAnimNode(Node))
		{
			return Node.IsA<UAnimGraphNode_BlendListByInt>() || Node.IsA<UAnimGraphNode_LayeredBoneBlend>()
				|| Node.IsA<UAnimGraphNode_MultiWayBlend>();
		}
		const TSharedPtr<IAssetDocumentGraphSchemaAdapter> K2 = FindK2SchemaAdapter();
		return K2.IsValid() && K2->CanAddDynamicPin(Node);
	}

	virtual bool AddDynamicPin(UEdGraphNode& Node, FAssetDocumentGraphError& OutError) const override
	{
		// The node context menu's "Add Blend Pin" / "Add pin".
		if (UAnimGraphNode_LayeredBoneBlend* Layered = Cast<UAnimGraphNode_LayeredBoneBlend>(&Node))
		{
			Layered->AddPinToBlendByFilter();
			return true;
		}
		if (UAnimGraphNode_BlendListByInt* ByInt = Cast<UAnimGraphNode_BlendListByInt>(&Node))
		{
			ByInt->AddPinToBlendList();
			return true;
		}
		if (UAnimGraphNode_MultiWayBlend* MultiWay = Cast<UAnimGraphNode_MultiWayBlend>(&Node))
		{
			MultiWay->AddPinToBlendNode();
			return true;
		}
		const TSharedPtr<IAssetDocumentGraphSchemaAdapter> K2 = FindK2SchemaAdapter();
		return K2.IsValid() && K2->AddDynamicPin(Node, OutError);
	}

	virtual bool RemoveDynamicPin(UEdGraphNode& Node, UEdGraphPin* Pin, FAssetDocumentGraphError& OutError) const override
	{
		if (Pin != nullptr && Pin->GetOwningNode() == &Node)
		{
			if (UAnimGraphNode_LayeredBoneBlend* Layered = Cast<UAnimGraphNode_LayeredBoneBlend>(&Node))
			{
				Layered->RemovePinFromBlendByFilter(Pin);
				return true;
			}
			if (UAnimGraphNode_BlendListByInt* ByInt = Cast<UAnimGraphNode_BlendListByInt>(&Node))
			{
				ByInt->RemovePinFromBlendList(Pin);
				return true;
			}
			if (UAnimGraphNode_MultiWayBlend* MultiWay = Cast<UAnimGraphNode_MultiWayBlend>(&Node))
			{
				MultiWay->RemovePinFromBlendNode(Pin);
				return true;
			}
		}
		const TSharedPtr<IAssetDocumentGraphSchemaAdapter> K2 = FindK2SchemaAdapter();
		return K2.IsValid() && K2->RemoveDynamicPin(Node, Pin, OutError);
	}

	virtual bool SetFunctionFlags(UObject& Asset, UEdGraph& Graph, const FAssetDocumentRuntimeValueDto& Flags,
		FAssetDocumentRuntimeValueDto& OutFlags, FAssetDocumentGraphError& OutError) const override
	{
		OutError.Set(TEXT("Graph.GraphOperationUnsupported"), TEXT("Anim graphs have no function flags."));
		return false;
	}

	virtual bool MapNodeSetting(const UEdGraphNode& Node, FString& InOutPath, FAssetDocumentRuntimeValueDto& InOutValue,
		FAssetDocumentGraphError& OutError) const override
	{
		return MapControlRigSetting(Node, InOutPath, InOutValue, OutError);
	}

	virtual bool SetNodeSetting(UObject& Asset, UEdGraphNode& Node, FStringView Path, const FAssetDocumentRuntimeValueDto& Value,
		bool& bOutHandled, FAssetDocumentGraphError& OutError) const override
	{
		bOutHandled = false;
		UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(&Node);
		if (AnimNode == nullptr)
		{
			return true;
		}
		if (Path == TEXTVIEW("Bindings"))
		{
			bOutHandled = true;
			return BindingRawEdits::SetNodeBindings(*AnimNode, Value, OutError);
		}
		if (UAnimGraphNode_BlendSpaceGraphBase* BlendSpaceNode = Cast<UAnimGraphNode_BlendSpaceGraphBase>(AnimNode))
		{
			if (Path == TEXTVIEW("Samples"))
			{
				bOutHandled = true;
				return BlendSpaceRawEdits::SetSamples(*BlendSpaceNode, Value, OutError);
			}
			if (Path.StartsWith(TEXTVIEW("BlendSpace.")))
			{
				bOutHandled = true;
				return BlendSpaceRawEdits::SetBlendSpaceSetting(*BlendSpaceNode, Path, Value, OutError);
			}
		}
		if (Path == TEXTVIEW("LayerGroup"))
		{
			bOutHandled = true;
			if (!IsLayerRoot(Node) || Value.Kind != EAssetDocumentRuntimeValueKind::String)
			{
				OutError.Set(TEXT("Graph.InvalidArgument"),
					TEXT("LayerGroup is a setting of an anim layer's Output Pose: a group name, \"\" for ungrouped."), TEXT("/value"));
				return false;
			}
			// The layer graph's Details > Group (FAnimGraphDetails::OnGroupTextCommitted).
			FBlueprintEditorUtils::SetAnimationGraphLayerGroup(Node.GetGraph(), FText::FromString(Value.StringValue));
			return true;
		}
		FStringView Property = Path;
		if (!Property.StartsWith(TEXTVIEW("ShowPin.")))
		{
			return true;
		}
		bOutHandled = true;
		Property.RightChopInline(8);
		if (Value.Kind != EAssetDocumentRuntimeValueKind::Boolean)
		{
			OutError.Set(TEXT("Graph.InvalidArgument"), TEXT("ShowPin.* values are true or false."), TEXT("/value"));
			return false;
		}
		const int32 Index = AnimNode->ShowPinForProperties.IndexOfByPredicate(
			[Property](const FOptionalPinFromProperty& Pin) { return Pin.PropertyName.ToString() == Property; });
		if (Index == INDEX_NONE && ExposablePins(*AnimNode) != nullptr)
		{
			// A rig variable or control: the list the Details panel shows, rebuilt from the rig (a variable the rig got
			// after the node was last rebuilt is not in it yet).
			int32 Exposable = FindExposablePin(*ExposablePins(*AnimNode), Property);
			if (Exposable == INDEX_NONE)
			{
				AnimNode->ReconstructNode();
				Exposable = FindExposablePin(*ExposablePins(*AnimNode), Property);
			}
			if (Exposable != INDEX_NONE)
			{
				SetExposedPin(*AnimNode, Exposable, Value.BooleanValue);
				return true;
			}
		}
		if (Index == INDEX_NONE || !AnimNode->ShowPinForProperties[Index].bCanToggleVisibility)
		{
			TArray<FString> Names;
			for (const FOptionalPinFromProperty& Pin : AnimNode->ShowPinForProperties)
			{
				if (Pin.bCanToggleVisibility)
				{
					Names.Add(Pin.PropertyName.ToString());
				}
			}
			FString Exposable;
			if (const TArray<FOptionalPinFromProperty>* Pins = ExposablePins(*AnimNode))
			{
				TArray<FString> RigNames;
				for (const FOptionalPinFromProperty& Pin : *Pins)
				{
					if (Pin.bCanToggleVisibility)
					{
						RigNames.Add(Pin.PropertyName.ToString());
					}
				}
				Exposable = FString::Printf(TEXT("; the rig's public variables and controls: %s"),
					RigNames.IsEmpty() ? TEXT("none (make the variable Public in the rig)") : *FString::Join(RigNames, TEXT(", ")));
			}
			OutError.Set(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("%s has no optional pin '%.*s' (optional pins: %s%s)."),
				*Node.GetClass()->GetName(), Property.Len(), Property.GetData(), *FString::Join(Names, TEXT(", ")), *Exposable),
				TEXT("/property_path"));
			return false;
		}
		// The details panel's "show as pin" checkbox (FAnimGraphNodeDetails).
		AnimNode->Modify();
		AnimNode->SetPinVisibility(Value.BooleanValue, Index);
		return true;
	}

private:
	UClass* SchemaClass = nullptr;
};
} // namespace AnimGraphAdapterDetail

TSharedRef<IAssetDocumentGraphSchemaAdapter> MakeAnimGraphSchemaAdapter(UClass* SchemaClass)
{
	return MakeShared<AnimGraphAdapterDetail::FAnimGraphSchemaAdapter>(SchemaClass);
}
} // namespace UE::AssetDocument::AnimBlueprint
