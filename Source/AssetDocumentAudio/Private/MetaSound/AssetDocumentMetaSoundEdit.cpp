// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// audio.metasound.edit.v1: edits of a MetaSound Source / Patch made the way the MetaSound editor makes them
// (asset-document-metasound-design.md §4.1). One transaction per call, Asset->Modify() (and
// the open editor's graph Modify(), as the editor's own edits), then the asset's own document builder (the one the
// editor uses: FDocumentBuilderRegistry::FindOrBeginBuilding). With the asset open in the MetaSound editor the
// edits reach its graph as the editor's own do: the builder fills the document's modify context, and the editor's
// tick synchronizes the graph (forced by the registration). Registration is not done here;
// audio.metasound.compile.v1 registers once at the end of an apply, as the editor registers after its edits.
//
// Node references in ops are frontend node IDs (32 hex digits) or "output:<member>" (the vertex node of a graph
// output: interface outputs get their IDs from ModifyInterfaces, so a document names them by member).
//
// Pages: graph ops (normalize, variable.*, node.*, connect, comment.*) take "page" (a page name: "Default", a page of
// the project's UMetaSoundSettings, or the 32 hex digits the read gives a page the settings no longer define; default
// "Default"). The editor edits the page a user focuses (SetFocusedPage = the builder's build page); the focus is
// editor view state, so every op names its page and the build page is left alone.
//   {op: page.add, name}                    Pages Details "+" (FMetasoundPagesDetailCustomization): AddGraphPage(ID,
//                                           bDuplicateLastGraph=true, bSetAsBuildGraph=false) of a configured page
//   {op: page.remove, name}                 the page's delete button: RemoveGraphPage (not Default)
//
//   {op: normalize}                         the editor's open-time FInputNodeTemplate::Inject(false), with the
//                                           template node IDs the read shows (MakeInputTemplateNodeId)
//   {op: interfaces, add[], remove[]}       Interfaces panel: ModifyInterfaces, bSetDefaultNodeLocations=false
//   {op: metadata, display_name?, description?, author?, keywords?, categories?, referenceable?, deprecated?}
//   {op: input.add, name, type, constructor?, node_id?, <fields>}   Members panel add (CreateUniqueClassInput)
//   {op: input.set, name, rename_to?, type?, constructor?, <fields>}   member Details
//   {op: input.remove, name}                ClearMemberMetadata + RemoveGraphInput
//   {op: input.widget, name, values{}}      the input's literal object Details (FindOrCreateGraphInputMetadata)
//   {op: output.add|output.set|output.remove, ...}   the same for outputs (no default)
//     <fields>: display_name?, description?, sort_order?, advanced?, default? (inputs), page_defaults?{page: value}
//     (inputs, complete over the pages other than Default: the member's "Add Page Default Value" / per-page value /
//     remove, SetGraphInputDefault / RemoveGraphInputDefault per page)
//   {op: variable.add, name, type, default?, display_name?, description?} / variable.set (rename_to?) /
//   {op: variable.remove, name}
//   {op: node.create, id, kind: class|asset|input|get|get_delayed|set|reroute, class?, major?, asset?, member?,
//    from?, from_pin?, to?, to_pin?, position?}
//   {op: node.delete, id}                   RemoveNode (+ RemoveUnusedDependencies); an output node keeps its
//                                           vertex node and loses its edges and location, as the editor's delete
//   {op: node.set, id, position?, hidden?, configuration?{type?, properties{}} (complete), defaults?{pin: value}
//    (complete), comment?, comment_visible?, hide_unconnected?}
//                                           configuration: the node Details of a configurable class
//                                           (SetNodeConfiguration -> UpdateNodeInterfaceFromConfiguration)
//   {op: node.update, id, major}            the editor's Update Node (ReplaceNodeWithVersion) to the highest
//                                           registered version of the node's class
//   {op: input.inherit, name, inherits}     a preset input's "override inherited default" check box
//   {op: preset.convert_from}               the editor's Convert From Preset
//   {op: connect, id, pin, from (node ref | null), from_pin?}   IsValidEdge + loop check, RemoveEdgeToNodeInput + AddEdge
//   {op: comment.set, id, text, position, size, color?, font_size?, color_bubble?, move_mode?} / {op: comment.remove, id}
//   -> {created[], deleted[], changed, warnings[]}

#include "AssetDocumentMetaSound.h"

#include "AssetDocumentAudioPrimitives.h"
#include "AssetDocumentEditorNonInteractiveScope.h"
#include "AssetDocumentEditorPropertyValueCodec.h"
#include "AssetDocumentEditorTrialScope.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphNode_Comment.h"
#include "Editor.h"
#include "Interfaces/MetasoundFrontendInterfaceRegistry.h"
#include "MetasoundAssetBase.h"
#include "MetasoundAssetManager.h"
#include "MetasoundBuilderBase.h"
#include "MetasoundDocumentBuilderRegistry.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundEditorSubsystem.h"
#include "MetasoundFrontendDataTypeRegistry.h"
#include "MetasoundFrontendDocumentBuilder.h"
#include "MetasoundFrontendNodeTemplateRegistry.h"
#include "MetasoundFrontendNodeClassRegistry.h"
#include "MetasoundFrontendSearchEngine.h"
#include "MetasoundAudioBuffer.h"
#include "NodeTemplates/MetasoundFrontendNodeTemplateAudioAnalyzer.h"
#include "NodeTemplates/MetasoundFrontendNodeTemplateInput.h"
#include "NodeTemplates/MetasoundFrontendNodeTemplateReroute.h"
#include "ScopedTransaction.h"
#include "Subsystems/AssetEditorSubsystem.h"

#define LOCTEXT_NAMESPACE "AssetDocumentMetaSound"

namespace UE::AssetDocument::Graph::Audio
{
namespace MetaSoundEditLocal
{
using namespace MetaSound;
using EValueKind = EAssetDocumentRuntimeValueKind;

FString GuidText(const FGuid& Guid)
{
	return Guid.ToString(EGuidFormats::Digits);
}

FGuid LocationGuid(const FGuid& NodeID)
{
	return FGuid::NewDeterministicGuid(GuidText(NodeID) + TEXT(":Location"));
}

FGuid MemberGuid(const TCHAR* Kind, FName Name, const TCHAR* Part)
{
	return FGuid::NewDeterministicGuid(FString::Printf(TEXT("AssetDocument.MetaSound.%s:%s:%s"), Kind, *Name.ToString(), Part));
}

bool IsVariableFamily(EMetasoundFrontendClassType Type)
{
	return Type == EMetasoundFrontendClassType::Variable || Type == EMetasoundFrontendClassType::VariableAccessor
		|| Type == EMetasoundFrontendClassType::VariableDeferredAccessor || Type == EMetasoundFrontendClassType::VariableMutator;
}

struct FEdit
{
	FCall& Call;
	UObject& Asset;
	UMetaSoundBuilderBase& BuilderObject;
	FMetaSoundFrontendDocumentBuilder& Builder;
	FGuid PageID;
	TArray<FString> Created;
	TArray<FString> Deleted;
	TArray<FString> Warnings;
	bool bChanged = false;
	bool bRemoveUnusedDependencies = false;

	FEdit(FCall& InCall, UMetaSoundBuilderBase& InBuilderObject)
		: Call(InCall), Asset(InCall.Asset), BuilderObject(InBuilderObject), Builder(InBuilderObject.GetBuilder()), PageID(Metasound::Frontend::DefaultPageID)
	{
	}

	EMetasoundFrontendClassType TypeOf(const FMetasoundFrontendNode& Node) const
	{
		const FMetasoundFrontendClass* Class = Builder.FindDependency(Node.ClassID);
		return Class != nullptr ? Class->Metadata.GetType() : EMetasoundFrontendClassType::Invalid;
	}

	// The editor moves a node under its editor node's key (the location's existing key); a node without a location
	// gets the deterministic key.
	void Place(const FGuid& NodeID, const FVector2D& Location)
	{
		FGuid Key = LocationGuid(NodeID);
		if (const FMetasoundFrontendNode* Node = Builder.FindNode(NodeID, &PageID))
		{
			const TMap<FGuid, FVector2D>& Locations = Node->Style.Display.Locations;
			if (Locations.Num() == 1)
			{
				Key = Locations.CreateConstIterator()->Key;
			}
		}
		Builder.SetNodeLocation(NodeID, Location, &Key, &PageID);
	}
};

// ---- readers ------------------------------------------------------------------------------------------------

bool ReadName(FReader& Op, FStringView Key, FName& Out, bool bRequired = true)
{
	FString Text;
	if (!Op.String(Key, Text, bRequired))
	{
		return false;
	}
	if (!bRequired && !Op.Has(Key))
	{
		return true;
	}
	if (Text.IsEmpty() || Text.Len() >= NAME_SIZE)
	{
		return Op.Fail(TEXT("MetaSound.InvalidName"), TEXT("A member name is a non-empty name."), Key);
	}
	Out = FName(*Text);
	return true;
}

bool ReadPoint(FReader& Op, FStringView Key, FVector2D& Out, bool& bOutHas)
{
	bOutHas = false;
	const FValue* Value = Op.Find(Key);
	if (Value == nullptr || Value->Kind == EValueKind::Null)
	{
		return true;
	}
	auto IsNumber = [](const FValue& Element) { return Element.Kind == EValueKind::Integer || Element.Kind == EValueKind::Number; };
	auto ToDouble = [](const FValue& Element) { return Element.Kind == EValueKind::Integer ? static_cast<double>(Element.IntegerValue) : Element.NumberValue; };
	if (Value->Kind != EValueKind::Array || Value->Elements.Num() != 2 || !IsNumber(Value->Elements[0]) || !IsNumber(Value->Elements[1]))
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("A position or size is [x, y]."), Key);
	}
	Out = FVector2D(ToDouble(Value->Elements[0]), ToDouble(Value->Elements[1]));
	bOutHas = true;
	return true;
}

bool ReadTexts(FReader& Op, FStringView Key, TArray<FText>& Out)
{
	const FValue* Values = nullptr;
	if (!Op.Array(Key, Values))
	{
		return false;
	}
	for (const FValue& Element : Values->Elements)
	{
		if (Element.Kind != EValueKind::String)
		{
			return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("A list of strings."), Key);
		}
		Out.Add(FText::FromString(Element.StringValue));
	}
	return true;
}

bool ReadNames(FReader& Op, FStringView Key, TArray<FName>& Out)
{
	const FValue* Values = nullptr;
	if (!Op.Array(Key, Values, false))
	{
		return false;
	}
	if (Values == nullptr)
	{
		return true;
	}
	for (const FValue& Element : Values->Elements)
	{
		if (Element.Kind != EValueKind::String || Element.StringValue.IsEmpty())
		{
			return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("A list of names."), Key);
		}
		Out.Add(FName(*Element.StringValue));
	}
	return true;
}

FString JoinNames(const TArray<FString>& Names, int32 Max = 40)
{
	TArray<FString> Shown(Names);
	Shown.Sort();
	const int32 Total = Shown.Num();
	if (Total > Max)
	{
		Shown.SetNum(Max);
		Shown.Add(FString::Printf(TEXT("... (%d more)"), Total - Max));
	}
	return Shown.IsEmpty() ? FString(TEXT("(none)")) : FString::Join(Shown, TEXT(", "));
}

// A node reference: a frontend node ID or "output:<member>".
bool ResolveNode(FEdit& Edit, FReader& Op, FStringView Key, FGuid& OutID)
{
	FString Text;
	if (!Op.String(Key, Text))
	{
		return false;
	}
	if (Text.StartsWith(TEXT("output:")))
	{
		const FName Member(*Text.RightChop(7));
		const FMetasoundFrontendNode* Node = Edit.Builder.FindGraphOutputNode(Member, &Edit.PageID);
		if (Node == nullptr)
		{
			return Op.Fail(TEXT("MetaSound.UnknownMember"), FString::Printf(TEXT("The MetaSound has no output '%s'."), *Member.ToString()), Key);
		}
		OutID = Node->GetID();
		return true;
	}
	if (!TryParseGuid(Text, OutID) || !Edit.Builder.ContainsNode(OutID, &Edit.PageID))
	{
		return Op.Fail(TEXT("MetaSound.NodeNotFound"), FString::Printf(TEXT("No node '%s' on the graph."), *Text), Key);
	}
	return true;
}

TArray<FString> PinNames(const TArray<FMetasoundFrontendVertex>& Vertices)
{
	TArray<FString> Names;
	for (const FMetasoundFrontendVertex& Vertex : Vertices)
	{
		Names.Add(Vertex.Name.ToString());
	}
	return Names;
}

const FMetasoundFrontendVertex* FindPin(FEdit& Edit, FReader& Op, FStringView Key, const FGuid& NodeID, bool bInput, bool bRequired)
{
	const FMetasoundFrontendNode* Node = Edit.Builder.FindNode(NodeID, &Edit.PageID);
	const TArray<FMetasoundFrontendVertex>& Vertices = bInput ? Node->Interface.Inputs : Node->Interface.Outputs;
	FString Name;
	if (!Op.Has(Key) || (Op.Find(Key)->Kind == EValueKind::Null))
	{
		if (!bRequired && Vertices.Num() == 1)
		{
			return &Vertices[0];
		}
		Op.Fail(TEXT("MetaSound.PinNotFound"), FString::Printf(TEXT("Name the %s pin; the node has %s."), bInput ? TEXT("input") : TEXT("output"),
			*JoinNames(PinNames(Vertices))), Key);
		return nullptr;
	}
	if (!Op.String(Key, Name))
	{
		return nullptr;
	}
	const FMetasoundFrontendVertex* Vertex = Vertices.FindByPredicate([&Name](const FMetasoundFrontendVertex& Candidate) { return Candidate.Name.ToString() == Name; });
	if (Vertex == nullptr)
	{
		Op.Fail(TEXT("MetaSound.PinNotFound"), FString::Printf(TEXT("The node has no %s pin '%s'. Its %s pins: %s."), bInput ? TEXT("input") : TEXT("output"),
			*Name, bInput ? TEXT("input") : TEXT("output"), *JoinNames(PinNames(Vertices))), Key);
	}
	return Vertex;
}

bool ReadLiteral(FReader& Op, FStringView Key, FName DataType, FMetasoundFrontendLiteral& Out)
{
	FString Problem;
	if (!DecodeLiteral(*Op.Find(Key), DataType, Out, Problem))
	{
		return Op.Fail(TEXT("MetaSound.InvalidLiteral"), Problem, Key);
	}
	return true;
}

// ---- normalize (open-time input template injection) ------------------------------------------------------

// FInputNodeTemplate::Inject(bForceNodeCreation=false) / InputNodeTemplatePrivate::InitTemplateNode with the
// template node ID the read gives it, so the asset gets what the read showed.
const FMetasoundFrontendNode* AddInputTemplate(FEdit& Edit, FName InputName, const FGuid& NodeID, const TArray<FMetasoundFrontendVertexHandle>& Connected)
{
	using namespace Metasound::Frontend;
	FMetaSoundFrontendDocumentBuilder& Builder = Edit.Builder;
	const INodeTemplate* Template = INodeTemplateRegistry::Get().FindTemplate(FInputNodeTemplate::ClassName);
	const FMetasoundFrontendNode* InputNode = Builder.FindGraphInputNode(InputName, &Edit.PageID);
	if (Template == nullptr || InputNode == nullptr || InputNode->Interface.Outputs.IsEmpty())
	{
		return nullptr;
	}
	const FName TypeName = InputNode->Interface.Outputs.Last().TypeName;
	FMetasoundFrontendEdge NewEdge;
	NewEdge.FromNodeID = InputNode->GetID();
	NewEdge.FromVertexID = InputNode->Interface.Outputs.Last().VertexID;
	const TMap<FGuid, FVector2D> Locations = InputNode->Style.Display.Locations;

	FNodeTemplateGenerateInterfaceParams Params{{}, {TypeName}};
	const FMetasoundFrontendNode* TemplateNode = Builder.AddNodeByTemplate(*Template, MoveTemp(Params), NodeID, &Edit.PageID);
	if (TemplateNode == nullptr)
	{
		return nullptr;
	}
	const FGuid TemplateID = TemplateNode->GetID();
	const FGuid TemplateInput = TemplateNode->Interface.Inputs.Last().VertexID;
	const FGuid TemplateOutput = TemplateNode->Interface.Outputs.Last().VertexID;
	NewEdge.ToNodeID = TemplateID;
	NewEdge.ToVertexID = TemplateInput;
	if (Locations.IsEmpty())
	{
		if (!Connected.IsEmpty())
		{
			FVector2D NewLocation = FVector2D::ZeroVector;
			if (const FMetasoundFrontendNode* Node = Builder.FindNode(Connected.Last().NodeID, &Edit.PageID))
			{
				if (!Node->Style.Display.Locations.IsEmpty())
				{
					const FGuid ConnectedVertexID = Connected.Last().VertexID;
					const int32 Index = Node->Interface.Inputs.IndexOfByPredicate([&ConnectedVertexID](const FMetasoundFrontendVertex& Input) { return Input.VertexID == ConnectedVertexID; });
					NewLocation = Node->Style.Display.Locations.CreateConstIterator()->Value;
					NewLocation -= Metasound::Frontend::DisplayStyle::NodeLayout::DefaultOffsetX;
					NewLocation += static_cast<double>(static_cast<uint32>(Index)) * Metasound::Frontend::DisplayStyle::NodeLayout::DefaultOffsetY;
				}
			}
			Edit.Place(TemplateID, NewLocation);
		}
	}
	else
	{
		for (const TPair<FGuid, FVector2D>& Pair : Locations)
		{
			Edit.Place(TemplateID, Pair.Value);
		}
	}
	Builder.AddEdge(MoveTemp(NewEdge), &Edit.PageID);
	for (const FMetasoundFrontendVertexHandle& Vertex : Connected)
	{
		FMetasoundFrontendEdge EdgeToRemove;
		EdgeToRemove.FromNodeID = NewEdge.FromNodeID;
		EdgeToRemove.FromVertexID = NewEdge.FromVertexID;
		EdgeToRemove.ToNodeID = Vertex.NodeID;
		EdgeToRemove.ToVertexID = Vertex.VertexID;
		Builder.RemoveEdge(EdgeToRemove, &Edit.PageID);
		FMetasoundFrontendEdge Moved;
		Moved.FromNodeID = TemplateID;
		Moved.FromVertexID = TemplateOutput;
		Moved.ToNodeID = Vertex.NodeID;
		Moved.ToVertexID = Vertex.VertexID;
		Builder.AddEdge(MoveTemp(Moved), &Edit.PageID);
	}
	return Builder.FindNode(TemplateID, &Edit.PageID);
}

bool OpNormalize(FEdit& Edit, FReader& Op)
{
	using namespace Metasound::Frontend;
	if (!Op.OnlyFields({TEXT("op")}))
	{
		return false;
	}
	FMetaSoundFrontendDocumentBuilder& Builder = Edit.Builder;
	TArray<FName> Names;
	for (const FMetasoundFrontendClassInput& Input : Builder.GetConstDocumentChecked().RootGraph.GetDefaultInterface().Inputs)
	{
		Names.Add(Input.Name);
	}
	for (const FName& Name : Names)
	{
		const FMetasoundFrontendNode* InputNode = Builder.FindGraphInputNode(Name, &Edit.PageID);
		if (InputNode == nullptr || InputNode->Interface.Outputs.IsEmpty())
		{
			continue;
		}
		const FGuid InputNodeID = InputNode->GetID();
		TArray<const FMetasoundFrontendNode*> ConnectedNodes;
		const TArray<const FMetasoundFrontendVertex*> ConnectedInputs = Builder.FindNodeInputsConnectedToNodeOutput(InputNodeID,
			InputNode->Interface.Outputs.Last().VertexID, &ConnectedNodes, &Edit.PageID);
		TArray<FMetasoundFrontendVertexHandle> Connected;
		for (int32 Index = 0; Index < ConnectedInputs.Num(); ++Index)
		{
			const FMetasoundFrontendClass* Class = Builder.FindDependency(ConnectedNodes[Index]->ClassID);
			if (Class != nullptr && Class->Metadata.GetClassName() == FInputNodeTemplate::ClassName)
			{
				continue;
			}
			FMetasoundFrontendVertexHandle& Handle = Connected.AddDefaulted_GetRef();
			Handle.NodeID = ConnectedNodes[Index]->GetID();
			Handle.VertexID = ConnectedInputs[Index]->VertexID;
		}
		if (Connected.IsEmpty())
		{
			continue;
		}
		FGuid TemplateID = MakeInputTemplateNodeId(InputNodeID);
		if (Builder.ContainsNode(TemplateID, &Edit.PageID))
		{
			TemplateID = FGuid::NewDeterministicGuid(GuidText(TemplateID) + TEXT(":Next"));
		}
		if (AddInputTemplate(Edit, Name, TemplateID, Connected) == nullptr)
		{
			return Op.Fail(TEXT("MetaSound.EditFailed"), FString::Printf(TEXT("Could not add the input template node of '%s'."), *Name.ToString()));
		}
		Edit.Created.Add(GuidText(TemplateID));
		Edit.bChanged = true;
	}
	return true;
}

// ---- interfaces / metadata ----------------------------------------------------------------------------------

// What the Interfaces Details panel lets a user add or remove for the asset's class.
bool CanAddOrRemoveInterface(const FMetasoundFrontendVersion& Version, const UClass& AssetClass)
{
	using namespace Metasound::Frontend;
	FMetasoundFrontendInterface Interface;
	if (!FindRegisteredInterface(Version, Interface))
	{
		return false;
	}
	const FTopLevelAssetPath ClassPath = AssetClass.GetClassPathName();
	const FMetasoundFrontendInterfaceUClassOptions* Options = Interface.Metadata.UClassOptions.FindByPredicate(
		[&ClassPath](const FMetasoundFrontendInterfaceUClassOptions& Candidate) { return Candidate.ClassPath == ClassPath; });
	return Options == nullptr || Options->bIsModifiable;
}

bool OpInterfaces(FEdit& Edit, FReader& Op)
{
	using namespace Metasound::Frontend;
	TArray<FName> Add;
	TArray<FName> Remove;
	if (!Op.OnlyFields({TEXT("op"), TEXT("add"), TEXT("remove")}) || !ReadNames(Op, TEXT("add"), Add) || !ReadNames(Op, TEXT("remove"), Remove))
	{
		return false;
	}
	const UClass& AssetClass = *Edit.Asset.GetClass();
	const TSet<FMetasoundFrontendVersion>& Declared = Edit.Builder.GetConstDocumentChecked().Interfaces;
	TArray<FMetasoundFrontendVersion> ToRemove;
	for (const FName& Name : Remove)
	{
		const FMetasoundFrontendVersion* Version = Algo::FindByPredicate(Declared, [&Name](const FMetasoundFrontendVersion& Candidate) { return Candidate.Name == Name; });
		if (Version == nullptr)
		{
			continue;
		}
		if (!CanAddOrRemoveInterface(*Version, AssetClass))
		{
			return Op.Fail(TEXT("MetaSound.InterfaceNotModifiable"), FString::Printf(TEXT("Interface '%s' cannot be removed from a %s (the Interfaces panel does not offer it)."),
				*Name.ToString(), *AssetClass.GetName()), TEXT("remove"));
		}
		ToRemove.Add(*Version);
	}
	TArray<FMetasoundFrontendVersion> ToAdd;
	for (const FName& Name : Add)
	{
		if (Algo::FindByPredicate(Declared, [&Name](const FMetasoundFrontendVersion& Candidate) { return Candidate.Name == Name; }) != nullptr)
		{
			continue;
		}
		FMetasoundFrontendInterface Interface;
		if (!ISearchEngine::Get().FindInterfaceWithHighestVersion(Name, Interface))
		{
			TArray<FString> Names;
			for (const FMetasoundFrontendInterface& Candidate : FindAllRegisteredInterfaces())
			{
				if (CanAddOrRemoveInterface(Candidate.Metadata.Version, AssetClass))
				{
					Names.AddUnique(Candidate.Metadata.Version.Name.ToString());
				}
			}
			return Op.Fail(TEXT("MetaSound.UnknownInterface"), FString::Printf(TEXT("No registered interface '%s'. Interfaces a %s can add: %s."),
				*Name.ToString(), *AssetClass.GetName(), *JoinNames(Names)), TEXT("add"));
		}
		if (!CanAddOrRemoveInterface(Interface.Metadata.Version, AssetClass))
		{
			return Op.Fail(TEXT("MetaSound.InterfaceNotModifiable"), FString::Printf(TEXT("Interface '%s' cannot be added to a %s (the Interfaces panel does not offer it)."),
				*Name.ToString(), *AssetClass.GetName()), TEXT("add"));
		}
		ToAdd.Add(Interface.Metadata.Version);
	}
	if (ToAdd.IsEmpty() && ToRemove.IsEmpty())
	{
		return true;
	}
	FModifyInterfaceOptions Options(ToRemove, ToAdd);
	Options.bSetDefaultNodeLocations = false;
	if (!Edit.Builder.ModifyInterfaces(MoveTemp(Options)))
	{
		return Op.Fail(TEXT("MetaSound.EditFailed"), TEXT("ModifyInterfaces failed (see the log)."));
	}
	Edit.bChanged = true;
	return true;
}

bool OpMetadata(FEdit& Edit, FReader& Op)
{
	if (!Op.OnlyFields({TEXT("op"), TEXT("display_name"), TEXT("description"), TEXT("author"), TEXT("keywords"), TEXT("categories"),
		TEXT("referenceable"), TEXT("deprecated")}))
	{
		return false;
	}
	FMetaSoundFrontendDocumentBuilder& Builder = Edit.Builder;
	FString Text;
	if (Op.Has(TEXT("display_name")))
	{
		if (!Op.String(TEXT("display_name"), Text))
		{
			return false;
		}
		Builder.SetDisplayName(FText::FromString(Text));
	}
	if (Op.Has(TEXT("description")))
	{
		if (!Op.String(TEXT("description"), Text))
		{
			return false;
		}
		Builder.SetDescription(FText::FromString(Text));
	}
	if (Op.Has(TEXT("author")))
	{
		if (!Op.String(TEXT("author"), Text))
		{
			return false;
		}
		Builder.SetAuthor(Text);
	}
	if (Op.Has(TEXT("keywords")))
	{
		TArray<FText> Keywords;
		if (!ReadTexts(Op, TEXT("keywords"), Keywords))
		{
			return false;
		}
		Builder.SetKeywords(Keywords);
	}
	if (Op.Has(TEXT("categories")))
	{
		TArray<FText> Categories;
		if (!ReadTexts(Op, TEXT("categories"), Categories))
		{
			return false;
		}
		Builder.SetCategoryHierarchy(Categories);
	}
	bool bFlag = false;
	if (Op.Has(TEXT("referenceable")))
	{
		if (!Op.Bool(TEXT("referenceable"), bFlag, true))
		{
			return false;
		}
		if (bFlag)
		{
			Builder.AddAccessFlags(EMetasoundFrontendClassAccessFlags::Referenceable);
		}
		else
		{
			Builder.RemoveAccessFlags(EMetasoundFrontendClassAccessFlags::Referenceable);
		}
	}
	if (Op.Has(TEXT("deprecated")))
	{
		if (!Op.Bool(TEXT("deprecated"), bFlag, true))
		{
			return false;
		}
		if (bFlag)
		{
			Builder.AddAccessFlags(EMetasoundFrontendClassAccessFlags::Deprecated);
		}
		else
		{
			Builder.RemoveAccessFlags(EMetasoundFrontendClassAccessFlags::Deprecated);
		}
	}
	Edit.bChanged = true;
	return true;
}

// ---- members ------------------------------------------------------------------------------------------------

bool CheckDataType(FReader& Op, FName Type)
{
	if (!Metasound::Frontend::IDataTypeRegistry::Get().IsRegistered(Type))
	{
		return Op.Fail(TEXT("MetaSound.UnknownDataType"), FString::Printf(TEXT("'%s' is not a registered MetaSound data type (list_types kind "
			"metasound_datatype lists them)."), *Type.ToString()), TEXT("type"));
	}
	return true;
}

FString JoinPageNames(const TArray<FString>& Names)
{
	return FString::Join(Names, TEXT(", "));
}

TArray<FString> DocumentPageNames(const FEdit& Edit)
{
	TArray<FString> Names;
	for (const FGuid& PageID : GetOrderedPageIDs(Edit.Builder.GetConstDocumentChecked().RootGraph))
	{
		Names.Add(GetPageName(PageID));
	}
	return Names;
}

// An input's values for the pages other than Default (complete): the member Details' "Add Page Default Value"
// (UpdateFrontendDefaultLiteral with the page: SetGraphInputDefault), a page value's edit (the same) and its removal
// (RemoveGraphInputDefault). The editor offers the configured pages; a value the input already has for a page the
// settings no longer define can be kept or removed.
bool SetPageDefaults(FEdit& Edit, FReader& Op, FName Name)
{
	FMetaSoundFrontendDocumentBuilder& Builder = Edit.Builder;
	const FValue* Values = nullptr;
	if (!Op.Object(TEXT("page_defaults"), Values))
	{
		return false;
	}
	const FMetasoundFrontendClassInput* Input = Builder.FindGraphInput(Name);
	if (Input == nullptr)
	{
		return Op.Fail(TEXT("MetaSound.UnknownMember"), FString::Printf(TEXT("The MetaSound has no input '%s'."), *Name.ToString()), TEXT("name"));
	}
	const FName TypeName = Input->TypeName;
	const FGuid MemberID = Input->NodeID;
	TArray<FGuid> Existing;
	Input->IterateDefaults([&Existing](const FGuid& PageID, const FMetasoundFrontendLiteral&) { Existing.Add(PageID); });
	FReader PageValues = Op.Child(TEXT("page_defaults"), *Values);
	TArray<TPair<FGuid, FMetasoundFrontendLiteral>> Wanted;
	for (const FString& Key : Values->ObjectKeys)
	{
		FGuid PageID;
		bool bConfigured = false;
		if (!FindPageID(Key, PageID, &bConfigured) || PageID == Metasound::Frontend::DefaultPageID || (!bConfigured && !Existing.Contains(PageID)))
		{
			return PageValues.Fail(TEXT("MetaSound.UnknownPage"), FString::Printf(TEXT("'%s' is not a page an input can have a value for: the project's "
				"MetaSound page settings (Project Settings > MetaSound > Pages) define %s; Default is the input's Default."), *Key,
				*JoinPageNames(GetConfiguredPageNames())), Key);
		}
		FMetasoundFrontendLiteral Literal;
		FString Problem;
		if (!DecodeLiteral(*PageValues.Find(Key), TypeName, Literal, Problem))
		{
			return PageValues.Fail(TEXT("MetaSound.InvalidLiteral"), Problem, Key);
		}
		Wanted.Emplace(PageID, MoveTemp(Literal));
	}
	bool bChanged = false;
	for (const FGuid& PageID : Existing)
	{
		if (PageID != Metasound::Frontend::DefaultPageID && !Wanted.ContainsByPredicate([&PageID](const TPair<FGuid, FMetasoundFrontendLiteral>& Pair) { return Pair.Key == PageID; }))
		{
			bChanged |= Builder.RemoveGraphInputDefault(Name, PageID);
		}
	}
	for (TPair<FGuid, FMetasoundFrontendLiteral>& Pair : Wanted)
	{
		const FMetasoundFrontendLiteral* Current = Builder.GetGraphInputDefault(Name, &Pair.Key);
		if (Current == nullptr || !LiteralsEqual(*Current, Pair.Value, TypeName))
		{
			Builder.SetGraphInputDefault(Name, MoveTemp(Pair.Value), &Pair.Key);
			bChanged = true;
		}
	}
	if (bChanged)
	{
		// The editor marks the member modified (MetasoundDefaultLiteralCustomization: AddMemberIDsModified), so an open
		// editor refreshes the member's literal object.
		const Metasound::Frontend::IMetaSoundAssetManager* Manager = Metasound::Frontend::IMetaSoundAssetManager::Get();
		if (FMetasoundAssetBase* AssetBase = Manager != nullptr ? Manager->GetAsAsset(Edit.Asset) : nullptr)
		{
			AssetBase->GetModifyContext().AddMemberIDsModified({MemberID});
		}
		Edit.bChanged = true;
	}
	return true;
}

// Member Details fields shared by inputs and outputs.
bool SetVertexFields(FEdit& Edit, FReader& Op, FName Name, bool bInput)
{
	FMetaSoundFrontendDocumentBuilder& Builder = Edit.Builder;
	FString Text;
	if (Op.Has(TEXT("display_name")))
	{
		if (!Op.String(TEXT("display_name"), Text))
		{
			return false;
		}
		bInput ? Builder.SetGraphInputDisplayName(Name, FText::FromString(Text)) : Builder.SetGraphOutputDisplayName(Name, FText::FromString(Text));
	}
	if (Op.Has(TEXT("description")))
	{
		if (!Op.String(TEXT("description"), Text))
		{
			return false;
		}
		bInput ? Builder.SetGraphInputDescription(Name, FText::FromString(Text)) : Builder.SetGraphOutputDescription(Name, FText::FromString(Text));
	}
	if (Op.Has(TEXT("sort_order")))
	{
		int64 Order = 0;
		if (!Op.Int(TEXT("sort_order"), Order))
		{
			return false;
		}
		if (Order < MIN_int32 || Order > MAX_int32)
		{
			return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("SortOrder is a 32-bit integer."), TEXT("sort_order"));
		}
		bInput ? Builder.SetGraphInputSortOrderIndex(Name, static_cast<int32>(Order)) : Builder.SetGraphOutputSortOrderIndex(Name, static_cast<int32>(Order));
	}
	if (Op.Has(TEXT("advanced")))
	{
		bool bAdvanced = false;
		if (!Op.Bool(TEXT("advanced"), bAdvanced, true))
		{
			return false;
		}
		bInput ? Builder.SetGraphInputAdvancedDisplay(Name, bAdvanced) : Builder.SetGraphOutputAdvancedDisplay(Name, bAdvanced);
	}
	// Find, not Has: null is an object input's "no object" (Has() is false for null).
	if (bInput && Op.Find(TEXT("default")) != nullptr)
	{
		const FMetasoundFrontendClassInput* Input = Builder.FindGraphInput(Name);
		FMetasoundFrontendLiteral Literal;
		if (Input == nullptr || !ReadLiteral(Op, TEXT("default"), Input->TypeName, Literal))
		{
			return false;
		}
		Builder.SetGraphInputDefault(Name, MoveTemp(Literal), &Metasound::Frontend::DefaultPageID);
	}
	if (bInput && Op.Has(TEXT("page_defaults")))
	{
		return SetPageDefaults(Edit, Op, Name);
	}
	return true;
}

bool SetAccess(FEdit& Edit, FReader& Op, FName Name, bool bInput)
{
	if (!Op.Has(TEXT("constructor")))
	{
		return true;
	}
	bool bConstructor = false;
	if (!Op.Bool(TEXT("constructor"), bConstructor, true))
	{
		return false;
	}
	const EMetasoundFrontendVertexAccessType Access = bConstructor ? EMetasoundFrontendVertexAccessType::Value : EMetasoundFrontendVertexAccessType::Reference;
	const bool bSet = bInput ? Edit.Builder.SetGraphInputAccessType(Name, Access) : Edit.Builder.SetGraphOutputAccessType(Name, Access);
	if (!bSet)
	{
		const FMetasoundFrontendClassVertex* Vertex = bInput ? static_cast<const FMetasoundFrontendClassVertex*>(Edit.Builder.FindGraphInput(Name))
			: static_cast<const FMetasoundFrontendClassVertex*>(Edit.Builder.FindGraphOutput(Name));
		if (Vertex == nullptr || Vertex->AccessType != Access)
		{
			return Op.Fail(TEXT("MetaSound.InvalidMember"), FString::Printf(TEXT("'%s' cannot be a %s member of its type."), *Name.ToString(),
				bConstructor ? TEXT("constructor") : TEXT("non-constructor")), TEXT("constructor"));
		}
	}
	return true;
}

bool OpMemberAdd(FEdit& Edit, FReader& Op, bool bInput)
{
	FName Name;
	FName Type;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("type"), TEXT("constructor"), TEXT("node_id"), TEXT("display_name"), TEXT("description"),
		TEXT("sort_order"), TEXT("advanced"), TEXT("default"), TEXT("page_defaults")}) || !ReadName(Op, TEXT("name"), Name) || !ReadName(Op, TEXT("type"), Type)
		|| !CheckDataType(Op, Type))
	{
		return false;
	}
	FMetaSoundFrontendDocumentBuilder& Builder = Edit.Builder;
	if ((bInput && Builder.FindGraphInput(Name) != nullptr) || (!bInput && Builder.FindGraphOutput(Name) != nullptr))
	{
		return Op.Fail(TEXT("MetaSound.DuplicateMember"), FString::Printf(TEXT("The MetaSound already has an %s '%s'."), bInput ? TEXT("input") : TEXT("output"),
			*Name.ToString()), TEXT("name"));
	}
	bool bConstructor = false;
	if (!Op.Bool(TEXT("constructor"), bConstructor, false))
	{
		return false;
	}
	FGuid NodeID = MemberGuid(bInput ? TEXT("Input") : TEXT("Output"), Name, TEXT("Node"));
	if (Op.Has(TEXT("node_id")))
	{
		if (!Op.Guid(TEXT("node_id"), NodeID))
		{
			return false;
		}
	}
	if (Builder.ContainsNode(NodeID, &Edit.PageID))
	{
		NodeID = FGuid::NewDeterministicGuid(GuidText(NodeID) + TEXT(":Member"));
	}
	const EMetasoundFrontendVertexAccessType Access = bConstructor ? EMetasoundFrontendVertexAccessType::Value : EMetasoundFrontendVertexAccessType::Reference;
	const FMetasoundFrontendNode* NewNode = nullptr;
	if (bInput)
	{
		// FGraphBuilder::CreateUniqueClassInput with the document's name.
		FMetasoundFrontendClassInput ClassInput;
		ClassInput.Name = Name;
		ClassInput.TypeName = Type;
		ClassInput.VertexID = MemberGuid(TEXT("Input"), Name, TEXT("Vertex"));
		ClassInput.NodeID = NodeID;
		ClassInput.AccessType = Access;
		ClassInput.InitDefault();
		NewNode = Builder.AddGraphInput(MoveTemp(ClassInput), &Edit.PageID);
	}
	else
	{
		FMetasoundFrontendClassOutput ClassOutput;
		ClassOutput.Name = Name;
		ClassOutput.TypeName = Type;
		ClassOutput.VertexID = MemberGuid(TEXT("Output"), Name, TEXT("Vertex"));
		ClassOutput.NodeID = NodeID;
		ClassOutput.AccessType = Access;
		NewNode = Builder.AddGraphOutput(MoveTemp(ClassOutput), &Edit.PageID);
	}
	if (NewNode == nullptr)
	{
		return Op.Fail(TEXT("MetaSound.InvalidMember"), FString::Printf(TEXT("Could not add %s '%s' of type %s%s."), bInput ? TEXT("input") : TEXT("output"),
			*Name.ToString(), *Type.ToString(), bConstructor ? TEXT(" as a constructor member (the type has no constructor form)") : TEXT("")), TEXT("type"));
	}
	Edit.bChanged = true;
	return SetVertexFields(Edit, Op, Name, bInput);
}

bool OpMemberSet(FEdit& Edit, FReader& Op, bool bInput)
{
	FName Name;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("rename_to"), TEXT("type"), TEXT("constructor"), TEXT("display_name"), TEXT("description"),
		TEXT("sort_order"), TEXT("advanced"), TEXT("default"), TEXT("page_defaults")}) || !ReadName(Op, TEXT("name"), Name))
	{
		return false;
	}
	FMetaSoundFrontendDocumentBuilder& Builder = Edit.Builder;
	if ((bInput && Builder.FindGraphInput(Name) == nullptr) || (!bInput && Builder.FindGraphOutput(Name) == nullptr))
	{
		return Op.Fail(TEXT("MetaSound.UnknownMember"), FString::Printf(TEXT("The MetaSound has no %s '%s'."), bInput ? TEXT("input") : TEXT("output"), *Name.ToString()), TEXT("name"));
	}
	if (Op.Has(TEXT("rename_to")))
	{
		FName NewName;
		if (!ReadName(Op, TEXT("rename_to"), NewName))
		{
			return false;
		}
		if ((bInput && Builder.FindGraphInput(NewName) != nullptr) || (!bInput && Builder.FindGraphOutput(NewName) != nullptr))
		{
			return Op.Fail(TEXT("MetaSound.DuplicateMember"), FString::Printf(TEXT("The MetaSound already has a member '%s'."), *NewName.ToString()), TEXT("rename_to"));
		}
		if (!(bInput ? Builder.SetGraphInputName(Name, NewName) : Builder.SetGraphOutputName(Name, NewName)))
		{
			return Op.Fail(TEXT("MetaSound.EditFailed"), FString::Printf(TEXT("Could not rename '%s'."), *Name.ToString()), TEXT("rename_to"));
		}
		Name = NewName;
	}
	if (Op.Has(TEXT("type")))
	{
		FName Type;
		if (!ReadName(Op, TEXT("type"), Type) || !CheckDataType(Op, Type))
		{
			return false;
		}
		const FMetasoundFrontendClassInput* Existing = bInput ? Builder.FindGraphInput(Name) : nullptr;
		if (Existing != nullptr && Existing->TypeName != Type)
		{
			// UMetasoundEditorGraphInput::SetDataType re-initialises the member's literal object for the new type
			// (InitializeLiteral); without an editor graph the old type's object is dropped and the next widget
			// edit or editor open creates the new type's default one.
			Builder.ClearMemberMetadata(Existing->NodeID);
		}
		if (!(bInput ? Builder.SetGraphInputDataType(Name, Type) : Builder.SetGraphOutputDataType(Name, Type)))
		{
			return Op.Fail(TEXT("MetaSound.InvalidMember"), FString::Printf(TEXT("Could not change the type of '%s' to %s."), *Name.ToString(), *Type.ToString()), TEXT("type"));
		}
	}
	if (!SetAccess(Edit, Op, Name, bInput) || !SetVertexFields(Edit, Op, Name, bInput))
	{
		return false;
	}
	Edit.bChanged = true;
	return true;
}

bool OpMemberRemove(FEdit& Edit, FReader& Op, bool bInput)
{
	FName Name;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name")}) || !ReadName(Op, TEXT("name"), Name))
	{
		return false;
	}
	FMetaSoundFrontendDocumentBuilder& Builder = Edit.Builder;
	const FMetasoundFrontendClassVertex* Vertex = bInput ? static_cast<const FMetasoundFrontendClassVertex*>(Builder.FindGraphInput(Name))
		: static_cast<const FMetasoundFrontendClassVertex*>(Builder.FindGraphOutput(Name));
	if (Vertex == nullptr)
	{
		return true;
	}
	Builder.ClearMemberMetadata(Vertex->NodeID);
	if (!(bInput ? Builder.RemoveGraphInput(Name) : Builder.RemoveGraphOutput(Name)))
	{
		return Op.Fail(TEXT("MetaSound.EditFailed"), FString::Printf(TEXT("Could not remove '%s'."), *Name.ToString()), TEXT("name"));
	}
	Edit.bChanged = true;
	return true;
}

// The member Details of an input's literal object (UMetasoundEditorGraphMemberDefault<Type>: range, widget, ...):
// UMetaSoundEditorSubsystem::FindOrCreateGraphInputMetadata, then each property written as the Details panel
// writes it (Modify, PreEditChange, write, PostEditChangeChainProperty). "values" is complete: an omitted property
// gets the class default (a new member's value). A write can make the object adjust others (a new widget type
// resets the range), so changed properties are written until the object holds the document's values.
bool OpInputWidget(FEdit& Edit, FReader& Op)
{
	FName Name;
	const FValue* Values = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("values")}) || !ReadName(Op, TEXT("name"), Name) || !Op.Object(TEXT("values"), Values))
	{
		return false;
	}
	const FMetasoundFrontendClassInput* Input = Edit.Builder.FindGraphInput(Name);
	if (Input == nullptr)
	{
		return Op.Fail(TEXT("MetaSound.UnknownMember"), FString::Printf(TEXT("The MetaSound has no input '%s'."), *Name.ToString()), TEXT("name"));
	}
	if (Values->ObjectKeys.IsEmpty() && Edit.Builder.FindMemberMetadata(Input->NodeID) == nullptr)
	{
		return true;
	}
	EMetaSoundBuilderResult BuilderResult = EMetaSoundBuilderResult::Failed;
	UMetaSoundFrontendMemberMetadata* Metadata = UMetaSoundEditorSubsystem::GetChecked().FindOrCreateGraphInputMetadata(&Edit.BuilderObject, Name, BuilderResult);
	if (Metadata == nullptr)
	{
		return Op.Fail(TEXT("MetaSound.EditFailed"), FString::Printf(TEXT("Input '%s' has no member settings object for its type."), *Name.ToString()), TEXT("values"));
	}
	// An omitted setting is written as the editor makes a new settings object (a Float input's WidgetType from the
	// editor setting), so opening the editor, which re-initializes it, changes nothing.
	const TStrongObjectPtr<UObject> EditorDefaults = MakeEditorWidgetDefaults(*Metadata->GetClass(), Edit.Builder.IsPreset());
	const UObject* Defaults = EditorDefaults.Get();
	// The Details panel edits these with the MetaSound editor open, so the asset has its (transient) editor graph and
	// the literal object's PostEditChange refreshes the member's nodes in it. Without an open editor there is no editor
	// graph: a value-set change then hits ensure(Member) (UMetasoundEditorGraphMemberDefaultFloat / Int), so the change
	// is posted as the Details panel posts a slider drag (Interactive), which makes the same adjustments except the
	// Range order fix (refused below as an invalid value) and skips only the node refresh of the absent graph.
	const Metasound::Frontend::IMetaSoundAssetManager* Manager = Metasound::Frontend::IMetaSoundAssetManager::Get();
	const FMetasoundAssetBase* AssetBase = Manager != nullptr ? Manager->GetAsAsset(static_cast<const UObject&>(Edit.Asset)) : nullptr;
	const bool bHasEditorGraph = AssetBase != nullptr && AssetBase->GetGraph() != nullptr;
	const EPropertyChangeType::Type ChangeType = bHasEditorGraph ? EPropertyChangeType::ValueSet : EPropertyChangeType::Interactive;
	TArray<FProperty*> Properties;
	TArray<FString> Names;
	for (TFieldIterator<FProperty> It(Metadata->GetClass()); It; ++It)
	{
		FProperty* Property = *It;
		if (!Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)
			|| Property->GetOwnerStruct() == UObject::StaticClass())
		{
			continue;
		}
		Properties.Add(Property);
		Names.Add(Property->GetAuthoredName());
	}
	FReader ValueReader = Op.Child(TEXT("values"), *Values);
	for (const FString& Key : Values->ObjectKeys)
	{
		if (!Names.Contains(Key))
		{
			return ValueReader.Fail(TEXT("MetaSound.UnknownWidgetProperty"), FString::Printf(TEXT("'%s' is not a setting of a %s input. Settings: %s."), *Key,
				*Input->TypeName.ToString(), *JoinNames(Names)), Key);
		}
	}
	for (int32 Pass = 0; Pass < 4; ++Pass)
	{
		bool bWrote = false;
		for (FProperty* Property : Properties)
		{
			void* Current = Property->ContainerPtrToValuePtr<void>(Metadata);
			void* Target = FMemory::Malloc(FMath::Max(Property->GetSize(), 1), static_cast<uint32>(Property->GetMinAlignment()));
			Property->InitializeValue(Target);
			const FString Key = Property->GetAuthoredName();
			const FValue* Value = FindField(*Values, Key);
			FString Code;
			bool bDecoded = true;
			if (Value != nullptr)
			{
				Property->CopyCompleteValue(Target, Current);
				bDecoded = UE::AssetDocument::EditorPropertyValueCodec::Decode(*Property, *Value, Target, Code, &Edit.Asset);
				const FStructProperty* RangeProperty = CastField<FStructProperty>(Property);
				if (bDecoded && RangeProperty != nullptr && RangeProperty->Struct == TBaseStructure<FVector2D>::Get() && Key.EndsWith(TEXT("Range"))
					&& static_cast<const FVector2D*>(Target)->X > static_cast<const FVector2D*>(Target)->Y)
				{
					bDecoded = false;
					Code = TEXT("X must not be greater than Y");
				}
			}
			else
			{
				Property->CopyCompleteValue(Target, Property->ContainerPtrToValuePtr<void>(Defaults));
			}
			if (bDecoded && !Property->Identical(Current, Target))
			{
				FEditPropertyChain Chain;
				Chain.AddHead(Property);
				Chain.SetActiveMemberPropertyNode(Property);
				Chain.SetActivePropertyNode(Property);
				Metadata->Modify();
				Metadata->PreEditChange(Chain);
				Property->CopyCompleteValue(Current, Target);
				FPropertyChangedEvent Event(Property, ChangeType);
				FPropertyChangedChainEvent ChainEvent(Chain, Event);
				Metadata->PostEditChangeChainProperty(ChainEvent);
				bWrote = true;
			}
			Property->DestroyValue(Target);
			FMemory::Free(Target);
			if (!bDecoded)
			{
				return ValueReader.Fail(TEXT("MetaSound.InvalidWidgetValue"), FString::Printf(TEXT("Invalid value for '%s' (%s)."), *Key, *Code), Key);
			}
		}
		if (!bWrote)
		{
			break;
		}
		Edit.bChanged = true;
	}
	return true;
}

bool OpVariable(FEdit& Edit, FReader& Op, const FString& Kind)
{
	FMetaSoundFrontendDocumentBuilder& Builder = Edit.Builder;
	FName Name;
	if (Kind == TEXT("variable.remove"))
	{
		if (!Op.OnlyFields({TEXT("op"), TEXT("name")}) || !ReadName(Op, TEXT("name"), Name))
		{
			return false;
		}
		if (Builder.FindGraphVariable(Name, &Edit.PageID) != nullptr)
		{
			if (!Builder.RemoveGraphVariable(Name, &Edit.PageID))
			{
				return Op.Fail(TEXT("MetaSound.EditFailed"), FString::Printf(TEXT("Could not remove variable '%s'."), *Name.ToString()), TEXT("name"));
			}
			Edit.bChanged = true;
		}
		return true;
	}
	const bool bAdd = Kind == TEXT("variable.add");
	if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("type"), TEXT("rename_to"), TEXT("default"), TEXT("display_name"), TEXT("description")})
		|| !ReadName(Op, TEXT("name"), Name))
	{
		return false;
	}
	FString Text;
	if (bAdd)
	{
		FName Type;
		if (!ReadName(Op, TEXT("type"), Type) || !CheckDataType(Op, Type))
		{
			return false;
		}
		if (Builder.FindGraphVariable(Name, &Edit.PageID) != nullptr)
		{
			return Op.Fail(TEXT("MetaSound.DuplicateMember"), FString::Printf(TEXT("The graph already has a variable '%s'."), *Name.ToString()), TEXT("name"));
		}
		if (Builder.AddGraphVariable(Name, Type, nullptr, nullptr, nullptr, &Edit.PageID) == nullptr)
		{
			return Op.Fail(TEXT("MetaSound.InvalidMember"), FString::Printf(TEXT("Could not add variable '%s' of type %s."), *Name.ToString(), *Type.ToString()), TEXT("type"));
		}
	}
	else
	{
		if (Builder.FindGraphVariable(Name, &Edit.PageID) == nullptr)
		{
			return Op.Fail(TEXT("MetaSound.UnknownMember"), FString::Printf(TEXT("The graph has no variable '%s'."), *Name.ToString()), TEXT("name"));
		}
		if (Op.Has(TEXT("rename_to")))
		{
			FName NewName;
			if (!ReadName(Op, TEXT("rename_to"), NewName))
			{
				return false;
			}
			if (Builder.FindGraphVariable(NewName, &Edit.PageID) != nullptr || !Builder.SetGraphVariableName(Name, NewName, &Edit.PageID))
			{
				return Op.Fail(TEXT("MetaSound.DuplicateMember"), FString::Printf(TEXT("Could not rename variable '%s' to '%s'."), *Name.ToString(), *NewName.ToString()), TEXT("rename_to"));
			}
			Name = NewName;
		}
	}
	const FMetasoundFrontendVariable* Variable = Builder.FindGraphVariable(Name, &Edit.PageID);
	if (Op.Find(TEXT("default")) != nullptr)
	{
		FMetasoundFrontendLiteral Literal;
		if (!ReadLiteral(Op, TEXT("default"), Variable->TypeName, Literal))
		{
			return false;
		}
		Builder.SetGraphVariableDefault(Name, MoveTemp(Literal), &Edit.PageID);
	}
	if (Op.Has(TEXT("display_name")))
	{
		if (!Op.String(TEXT("display_name"), Text))
		{
			return false;
		}
		Builder.SetGraphVariableDisplayName(Name, FText::FromString(Text), &Edit.PageID);
	}
	if (Op.Has(TEXT("description")))
	{
		if (!Op.String(TEXT("description"), Text))
		{
			return false;
		}
		Builder.SetGraphVariableDescription(Name, FText::FromString(Text), &Edit.PageID);
	}
	Edit.bChanged = true;
	return true;
}

// ---- nodes --------------------------------------------------------------------------------------------------

TArray<FString> SuggestClasses(const FString& Unknown)
{
	using namespace Metasound::Frontend;
	TArray<FString> Names;
	for (const FMetasoundFrontendClass& Class : FindAllRegisteredClasses())
	{
		if (Class.Metadata.GetType() == EMetasoundFrontendClassType::External)
		{
			Names.AddUnique(GetClassFullName(Class.Metadata.GetClassName()));
		}
	}
	return SuggestActionKeys(Unknown, Names, 6);
}

// The editor's "Add Audio Analyzer Node..." (FMetasoundGraphSchemaAction_NewAudioAnalyzer, offered when a wire is dragged
// from an audio output): AddNodeByTemplate with the dragged pin's data type (audio). Its one input ("Value") is wired by
// the document's Inputs like any edge. The node is editor-only: registration strips it (the template's transform).
const FMetasoundFrontendNode* CreateAudioAnalyzerNode(FEdit& Edit, FReader& Op, const FGuid& NodeID, int64 Major)
{
	using namespace Metasound::Frontend;
	if (Major != FAudioAnalyzerNodeTemplate::VersionNumber.Major)
	{
		Op.Fail(TEXT("MetaSound.UnknownNodeClass"), FString::Printf(TEXT("'%s' has version %d only."),
			*GetClassFullName(FAudioAnalyzerNodeTemplate::ClassName), FAudioAnalyzerNodeTemplate::VersionNumber.Major), TEXT("major"));
		return nullptr;
	}
	const INodeTemplate* Template = INodeTemplateRegistry::Get().FindTemplate(FAudioAnalyzerNodeTemplate::ClassName);
	FNodeTemplateGenerateInterfaceParams Params;
	Params.InputsToConnect.Add(Metasound::GetMetasoundDataTypeName<Metasound::FAudioBuffer>());
	const FMetasoundFrontendNode* Node = Template != nullptr ? Edit.Builder.AddNodeByTemplate(*Template, MoveTemp(Params), NodeID, &Edit.PageID) : nullptr;
	if (Node == nullptr)
	{
		Op.Fail(TEXT("MetaSound.EditFailed"), TEXT("Could not add an Audio Analyzer node."), TEXT("class"));
	}
	return Node;
}

const FMetasoundFrontendNode* CreateClassNode(FEdit& Edit, FReader& Op, const FGuid& NodeID)
{
	using namespace Metasound::Frontend;
	FString ClassText;
	int64 Major = 1;
	if (!Op.String(TEXT("class"), ClassText) || !Op.Int(TEXT("major"), Major, false))
	{
		return nullptr;
	}
	FMetasoundFrontendClassName ClassName;
	if (!FMetasoundFrontendClassName::Parse(ClassText, ClassName))
	{
		Op.Fail(TEXT("MetaSound.UnknownNodeClass"), FString::Printf(TEXT("'%s' is not a node class name (Namespace.Name.Variant, e.g. UE.Sine.Audio). "
			"Close names: %s."), *ClassText, *JoinNames(SuggestClasses(ClassText))), TEXT("class"));
		return nullptr;
	}
	if (ClassName == FAudioAnalyzerNodeTemplate::ClassName)
	{
		return CreateAudioAnalyzerNode(Edit, Op, NodeID, Major);
	}
	if (INodeTemplateRegistry::Get().FindTemplate(ClassName) != nullptr)
	{
		Op.Fail(TEXT("MetaSound.UnsupportedNodeClass"), FString::Printf(TEXT("'%s' is a template node; documents place reroutes as Class \"UE.Reroute\" "
			"and input nodes as Class \"Input\"."), *ClassText), TEXT("class"));
		return nullptr;
	}
	FMetasoundFrontendClass Class;
	if (Major < 0 || Major > MAX_int32 || !FindRegisteredNodeClass(ClassName, static_cast<int32>(Major), Class))
	{
		FMetasoundFrontendClass Highest;
		if (ISearchEngine::Get().FindClassWithHighestVersion(ClassName, Highest))
		{
			// The editor cannot place a node of an unregistered version either; on an existing node it shows "class not
			// found", and its Update Node moves the node to the registered version.
			Op.Fail(TEXT("MetaSound.UnknownNodeClass"), FString::Printf(TEXT("Node class '%s' has no registered major version %lld; the registered "
				"version is %d. Set Version to %d (on an existing node this is the editor's Update Node)."), *ClassText, Major,
				Highest.Metadata.GetVersion().Major, Highest.Metadata.GetVersion().Major), TEXT("major"));
			return nullptr;
		}
		Op.Fail(TEXT("MetaSound.UnknownNodeClass"), FString::Printf(TEXT("No registered node class '%s' with major version %lld. Close names: %s (list_types kind "
			"metasound_node lists the classes)."), *ClassText, Major, *JoinNames(SuggestClasses(ClassText))), TEXT("class"));
		return nullptr;
	}
	const EMetasoundFrontendClassType Type = Class.Metadata.GetType();
	if (Type != EMetasoundFrontendClassType::External && Type != EMetasoundFrontendClassType::Graph)
	{
		Op.Fail(TEXT("MetaSound.UnsupportedNodeClass"), FString::Printf(TEXT("'%s' is a member node class; use Class Input / Output / Get / GetDelayed / Set "
			"with Member."), *ClassText), TEXT("class"));
		return nullptr;
	}
	const FMetasoundFrontendNode* Node = Edit.Builder.AddNodeByClassName(ClassName, static_cast<int32>(Major), NodeID, &Edit.PageID);
	if (Node == nullptr)
	{
		Op.Fail(TEXT("MetaSound.EditFailed"), FString::Printf(TEXT("Could not add a '%s' node."), *ClassText), TEXT("class"));
	}
	return Node;
}

// UMetaSoundBuilderBase::AddNode (the editor's asset drop) registers the referenced MetaSound when it has unregistered
// builder transactions before adding its node; that step is protected, so it is reached through this never-instantiated
// derived class.
struct FBuilderAccess : public UMetaSoundBuilderBase
{
	using UMetaSoundBuilderBase::RegisterGraphIfOutstandingTransactions;
};

// The editor's asset drop (DroppedAssetsOnGraph / UMetaSoundBuilderBase::AddNode): a referenceable MetaSound
// asset that does not reference this one.
const FMetasoundFrontendNode* CreateAssetNode(FEdit& Edit, FReader& Op, const FGuid& NodeID)
{
	using namespace Metasound::Frontend;
	FString Path;
	if (!Op.String(TEXT("asset"), Path))
	{
		return nullptr;
	}
	UObject* Object = LoadObject<UObject>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	const IMetaSoundDocumentInterface* Document = Object != nullptr ? AsMetaSound(*Object) : nullptr;
	if (Document == nullptr || !Object->IsAsset())
	{
		Op.Fail(TEXT("MetaSound.UnknownAsset"), FString::Printf(TEXT("'%s' is not a MetaSound Source or Patch asset (an object path such as "
			"/Game/Audio/MSP_Mix.MSP_Mix)."), *Path), TEXT("asset"));
		return nullptr;
	}
	// Validate's trial edits a staged copy (AssetDocumentMetaSoundTrial.cpp): the checks are those of the asset it copies.
	const UObject* TrialSource = FAssetDocumentEditorTrialScope::FindTrialSource(Edit.Asset);
	const UObject& Self = TrialSource != nullptr ? *TrialSource : static_cast<const UObject&>(Edit.Asset);
	if (Object == &Edit.Asset || Object == &Self)
	{
		Op.Fail(TEXT("MetaSound.ReferenceCycle"), TEXT("A MetaSound cannot use itself as a node."), TEXT("asset"));
		return nullptr;
	}
	const FMetasoundFrontendGraphClass& RootGraph = Document->GetConstDocument().RootGraph;
	if (!EnumHasAnyFlags(RootGraph.Metadata.GetAccessFlags(), EMetasoundFrontendClassAccessFlags::Referenceable))
	{
		Op.Fail(TEXT("MetaSound.NotReferenceable"), FString::Printf(TEXT("%s is not Referenceable (its Metadata.Referenceable is false), so the editor does "
			"not let other MetaSounds use it."), *Path), TEXT("asset"));
		return nullptr;
	}
	IMetaSoundAssetManager* Manager = IMetaSoundAssetManager::Get();
	const FMetasoundAssetBase* This = Manager != nullptr ? Manager->GetAsAsset(Self) : nullptr;
	const FMetasoundAssetBase* Referenced = Manager != nullptr ? Manager->GetAsAsset(static_cast<const UObject&>(*Object)) : nullptr;
	if (This != nullptr && Referenced != nullptr && This->AddingReferenceCausesLoop(*Referenced))
	{
		Op.Fail(TEXT("MetaSound.ReferenceCycle"), FString::Printf(TEXT("%s uses this MetaSound (directly or through other MetaSounds); adding it would "
			"create a reference cycle."), *Path), TEXT("asset"));
		return nullptr;
	}
	const FString NeedsAudio = MetaSound::FindLiteralNeedingAudioDevice(*Object);
	if (!NeedsAudio.IsEmpty())
	{
		Op.Fail(TEXT("MetaSound.AudioDeviceRequired"), FString::Printf(TEXT("%s uses %s; registering it without an audio device (an editor "
			"started with -NoSound) would crash the editor. Run the editor with audio to use it."), *Path, *NeedsAudio), TEXT("asset"));
		return nullptr;
	}
	FBuilderAccess::RegisterGraphIfOutstandingTransactions(*Object);
	const FMetasoundFrontendNode* Node = Edit.Builder.AddGraphNode(Document->GetConstDocument().RootGraph, NodeID, &Edit.PageID);
	if (Node == nullptr)
	{
		Op.Fail(TEXT("MetaSound.EditFailed"), FString::Printf(TEXT("Could not add a node of %s."), *Path), TEXT("asset"));
	}
	return Node;
}

const FMetasoundFrontendNode* CreateRerouteNode(FEdit& Edit, FReader& Op, const FGuid& NodeID)
{
	using namespace Metasound::Frontend;
	FNodeTemplateGenerateInterfaceParams Params;
	if (Op.Has(TEXT("from")))
	{
		FGuid From;
		if (!ResolveNode(Edit, Op, TEXT("from"), From))
		{
			return nullptr;
		}
		const FMetasoundFrontendVertex* Pin = FindPin(Edit, Op, TEXT("from_pin"), From, false, false);
		if (Pin == nullptr)
		{
			return nullptr;
		}
		Params.InputsToConnect.Add(Pin->TypeName);
	}
	else if (Op.Has(TEXT("to")))
	{
		FGuid To;
		if (!ResolveNode(Edit, Op, TEXT("to"), To))
		{
			return nullptr;
		}
		const FMetasoundFrontendVertex* Pin = FindPin(Edit, Op, TEXT("to_pin"), To, true, true);
		if (Pin == nullptr)
		{
			return nullptr;
		}
		Params.OutputsToConnect.Add(Pin->TypeName);
	}
	else
	{
		Op.Fail(TEXT("MetaSound.RerouteType"), TEXT("A reroute takes the data type of what it connects: give it an input (Inputs.Value) or connect "
			"one of its consumers to it."));
		return nullptr;
	}
	const INodeTemplate* Template = INodeTemplateRegistry::Get().FindTemplate(FRerouteNodeTemplate::ClassName);
	const FMetasoundFrontendNode* Node = Template != nullptr ? Edit.Builder.AddNodeByTemplate(*Template, MoveTemp(Params), NodeID, &Edit.PageID) : nullptr;
	if (Node == nullptr)
	{
		Op.Fail(TEXT("MetaSound.EditFailed"), TEXT("Could not add a reroute node."));
	}
	return Node;
}

bool OpNodeCreate(FEdit& Edit, FReader& Op)
{
	FGuid NodeID;
	FString Kind;
	if (!Op.OnlyFields({TEXT("op"), TEXT("id"), TEXT("kind"), TEXT("class"), TEXT("major"), TEXT("asset"), TEXT("member"), TEXT("from"), TEXT("from_pin"),
		TEXT("to"), TEXT("to_pin"), TEXT("position")}) || !Op.Guid(TEXT("id"), NodeID) || !Op.String(TEXT("kind"), Kind))
	{
		return false;
	}
	FMetaSoundFrontendDocumentBuilder& Builder = Edit.Builder;
	if (Builder.ContainsNode(NodeID, &Edit.PageID))
	{
		return Op.Fail(TEXT("MetaSound.DuplicateNodeId"), FString::Printf(TEXT("A node with Id %s already exists."), *GuidText(NodeID)), TEXT("id"));
	}
	const FMetasoundFrontendNode* Node = nullptr;
	if (Kind == TEXT("class"))
	{
		Node = CreateClassNode(Edit, Op, NodeID);
	}
	else if (Kind == TEXT("asset"))
	{
		Node = CreateAssetNode(Edit, Op, NodeID);
	}
	else if (Kind == TEXT("reroute"))
	{
		Node = CreateRerouteNode(Edit, Op, NodeID);
	}
	else if (Kind == TEXT("input"))
	{
		FName Member;
		if (!ReadName(Op, TEXT("member"), Member))
		{
			return false;
		}
		if (Builder.FindGraphInput(Member) == nullptr || Builder.FindGraphInputNode(Member, &Edit.PageID) == nullptr)
		{
			return Op.Fail(TEXT("MetaSound.UnknownMember"), FString::Printf(TEXT("The MetaSound has no input '%s'."), *Member.ToString()), TEXT("member"));
		}
		// FInputNodeTemplate::CreateNode (the Members panel drag of an input) with the document's ID.
		Node = AddInputTemplate(Edit, Member, NodeID, {});
		if (Node == nullptr)
		{
			return Op.Fail(TEXT("MetaSound.EditFailed"), FString::Printf(TEXT("Could not add an input node of '%s'."), *Member.ToString()), TEXT("member"));
		}
	}
	else if (Kind == TEXT("get") || Kind == TEXT("get_delayed") || Kind == TEXT("set"))
	{
		FName Member;
		if (!ReadName(Op, TEXT("member"), Member))
		{
			return false;
		}
		const FMetasoundFrontendVariable* Variable = Builder.FindGraphVariable(Member, &Edit.PageID);
		if (Variable == nullptr)
		{
			return Op.Fail(TEXT("MetaSound.UnknownMember"), FString::Printf(TEXT("The graph has no variable '%s'."), *Member.ToString()), TEXT("member"));
		}
		if (Kind == TEXT("set") && Variable->MutatorNodeID.IsValid() && Builder.ContainsNode(Variable->MutatorNodeID, &Edit.PageID))
		{
			return Op.Fail(TEXT("MetaSound.DuplicateSetNode"), FString::Printf(TEXT("Variable '%s' already has a Set node; a variable has at most one."),
				*Member.ToString()), TEXT("member"));
		}
		Node = Kind == TEXT("get") ? Builder.AddGraphVariableAccessorNode(Member, NodeID, &Edit.PageID)
			: Kind == TEXT("get_delayed") ? Builder.AddGraphVariableDeferredAccessorNode(Member, NodeID, &Edit.PageID)
			: Builder.AddGraphVariableMutatorNode(Member, NodeID, &Edit.PageID);
		if (Node == nullptr)
		{
			return Op.Fail(TEXT("MetaSound.EditFailed"), FString::Printf(TEXT("Could not add a %s node of variable '%s'."), *Kind, *Member.ToString()), TEXT("member"));
		}
	}
	else
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("Unknown node kind '%s'."), *Kind), TEXT("kind"));
	}
	if (Node == nullptr)
	{
		return false;
	}
	FVector2D Position;
	bool bHasPosition = false;
	if (!ReadPoint(Op, TEXT("position"), Position, bHasPosition))
	{
		return false;
	}
	if (bHasPosition)
	{
		Edit.Place(NodeID, Position);
	}
	Edit.Created.Add(GuidText(NodeID));
	Edit.bChanged = true;
	return true;
}

bool OpNodeDelete(FEdit& Edit, FReader& Op)
{
	FGuid NodeID;
	if (!Op.OnlyFields({TEXT("op"), TEXT("id")}) || !ResolveNode(Edit, Op, TEXT("id"), NodeID))
	{
		return false;
	}
	FMetaSoundFrontendDocumentBuilder& Builder = Edit.Builder;
	const FMetasoundFrontendNode* Node = Builder.FindNode(NodeID, &Edit.PageID);
	switch (Edit.TypeOf(*Node))
	{
	case EMetasoundFrontendClassType::Output:
		// The editor's delete of an output node (RemoveFromDocument): its edges and location go; every page keeps
		// the output's vertex node.
		Builder.RemoveEdges(NodeID, &Edit.PageID);
		Builder.RemoveNodeLocation(NodeID, nullptr, &Edit.PageID);
		break;
	case EMetasoundFrontendClassType::Input:
	case EMetasoundFrontendClassType::Variable:
		return Op.Fail(TEXT("MetaSound.NodeNotDeletable"), TEXT("This node is derived from a member; remove the member instead."), TEXT("id"));
	default:
		if (!Builder.RemoveNode(NodeID, &Edit.PageID))
		{
			return Op.Fail(TEXT("MetaSound.EditFailed"), FString::Printf(TEXT("Could not remove node %s."), *GuidText(NodeID)), TEXT("id"));
		}
		Edit.bRemoveUnusedDependencies = true;
		break;
	}
	Edit.Deleted.Add(GuidText(NodeID));
	Edit.bChanged = true;
	return true;
}

bool SetNodeDefaults(FEdit& Edit, FReader& Op, const FGuid& NodeID)
{
	const FValue* Defaults = nullptr;
	if (!Op.Object(TEXT("defaults"), Defaults))
	{
		return false;
	}
	FMetaSoundFrontendDocumentBuilder& Builder = Edit.Builder;
	FReader Values = Op.Child(TEXT("defaults"), *Defaults);
	const TArray<FMetasoundFrontendVertex> Inputs = Builder.FindNode(NodeID, &Edit.PageID)->Interface.Inputs;
	for (const FString& Key : Defaults->ObjectKeys)
	{
		if (!Inputs.ContainsByPredicate([&Key](const FMetasoundFrontendVertex& Vertex) { return Vertex.Name.ToString() == Key; }))
		{
			return Values.Fail(TEXT("MetaSound.PinNotFound"), FString::Printf(TEXT("The node has no input pin '%s'. Its input pins: %s."), *Key,
				*JoinNames(PinNames(Inputs))), Key);
		}
	}
	for (const FMetasoundFrontendVertex& Vertex : Inputs)
	{
		const FString Key = Vertex.Name.ToString();
		const FMetasoundFrontendVertexLiteral* Current = Builder.FindNodeInputDefault(NodeID, Vertex.VertexID, &Edit.PageID);
		if (!Values.Has(Key))
		{
			if (Current != nullptr)
			{
				Builder.RemoveNodeInputDefault(NodeID, Vertex.VertexID, &Edit.PageID);
			}
			continue;
		}
		FMetasoundFrontendLiteral Literal;
		if (!ReadLiteral(Values, Key, Vertex.TypeName, Literal))
		{
			return false;
		}
		// The pin's "reset to default": a value equal to the class default is no literal.
		const FMetasoundFrontendLiteral* ClassDefault = FindNodeInputClassDefault(Builder, NodeID, Vertex.VertexID, &Edit.PageID);
		if (ClassDefault != nullptr && LiteralsEqual(*ClassDefault, Literal, Vertex.TypeName))
		{
			if (Current != nullptr)
			{
				Builder.RemoveNodeInputDefault(NodeID, Vertex.VertexID, &Edit.PageID);
			}
			continue;
		}
		if (Current == nullptr || !LiteralsEqual(Current->Value, Literal, Vertex.TypeName))
		{
			Builder.SetNodeInputDefault(NodeID, Vertex.VertexID, Literal, &Edit.PageID);
		}
	}
	return true;
}

// A numeric property's ClampMin / ClampMax (the Details panel's value range).
bool IsWithinClamp(const FProperty& Property, const void* Value, FString& OutRange)
{
	const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property);
	if (Numeric == nullptr || Numeric->IsEnum())
	{
		return true;
	}
	const double Number = Numeric->IsFloatingPoint() ? Numeric->GetFloatingPointPropertyValue(Value) : static_cast<double>(Numeric->GetSignedIntPropertyValue(Value));
	const FString& Min = Property.GetMetaData(TEXT("ClampMin"));
	const FString& Max = Property.GetMetaData(TEXT("ClampMax"));
	OutRange = FString::Printf(TEXT("%s..%s"), Min.IsEmpty() ? TEXT("") : *Min, Max.IsEmpty() ? TEXT("") : *Max);
	return (Min.IsEmpty() || Number >= FCString::Atod(*Min)) && (Max.IsEmpty() || Number <= FCString::Atod(*Max));
}

// The node Details of a configurable node class (FMetaSoundNodeConfigurationDataDetails): the configuration struct's
// editable properties, then UpdateNodeInterfaceFromConfiguration (the class interface override the configuration
// derives; connections to pins it removes go). "properties" is complete: an omitted property has the value a new
// node of the class gets.
bool SetNodeConfiguration(FEdit& Edit, FReader& Op, const FGuid& NodeID)
{
	const FValue* Value = nullptr;
	if (!Op.Object(TEXT("configuration"), Value))
	{
		return false;
	}
	FReader Config = Op.Child(TEXT("configuration"), *Value);
	if (!Config.OnlyFields({TEXT("type"), TEXT("properties")}))
	{
		return false;
	}
	FMetaSoundFrontendDocumentBuilder& Builder = Edit.Builder;
	const FMetasoundFrontendNode* Node = Builder.FindNode(NodeID, &Edit.PageID);
	const FMetasoundFrontendClass* Class = Builder.FindDependency(Node->ClassID);
	TInstancedStruct<FMetaSoundFrontendNodeConfiguration> Wanted = Class != nullptr ? MakeDefaultNodeConfiguration(Class->Metadata)
		: TInstancedStruct<FMetaSoundFrontendNodeConfiguration>();
	if (!Wanted.IsValid())
	{
		return Op.Fail(TEXT("MetaSound.InvalidConfiguration"), FString::Printf(TEXT("Nodes of %s have no configuration; leave Configuration out."),
			Class != nullptr ? *GetClassFullName(Class->Metadata.GetClassName()) : TEXT("this class")), TEXT("configuration"));
	}
	const UScriptStruct* Struct = Wanted.GetScriptStruct();
	if (Config.Has(TEXT("type")))
	{
		FString Type;
		if (!Config.String(TEXT("type"), Type))
		{
			return false;
		}
		if (Type != Struct->GetPathName())
		{
			return Config.Fail(TEXT("MetaSound.InvalidConfiguration"), FString::Printf(TEXT("Nodes of %s are configured by %s (got '%s')."),
				*GetClassFullName(Class->Metadata.GetClassName()), *Struct->GetPathName(), *Type), TEXT("type"));
		}
	}
	const FValue* Properties = nullptr;
	if (Config.Has(TEXT("properties")) && !Config.Object(TEXT("properties"), Properties))
	{
		return false;
	}
	if (Properties != nullptr)
	{
		TArray<FString> Names;
		TMap<FString, FProperty*> ByName;
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_Edit) && !It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditConst))
			{
				Names.Add(It->GetAuthoredName());
				ByName.Add(It->GetAuthoredName(), *It);
			}
		}
		FReader Values = Config.Child(TEXT("properties"), *Properties);
		for (const FString& Key : Properties->ObjectKeys)
		{
			FProperty* const* Found = ByName.Find(Key);
			if (Found == nullptr)
			{
				return Values.Fail(TEXT("MetaSound.InvalidConfiguration"), FString::Printf(TEXT("'%s' is not a setting of %s. Settings: %s."), *Key,
					*Struct->GetName(), *JoinNames(Names)), Key);
			}
			void* Target = (*Found)->ContainerPtrToValuePtr<void>(Wanted.GetMutableMemory());
			FString Code;
			if (!UE::AssetDocument::EditorPropertyValueCodec::Decode(**Found, *FindField(*Properties, Key), Target, Code, &Edit.Asset))
			{
				return Values.Fail(TEXT("MetaSound.InvalidConfiguration"), FString::Printf(TEXT("Invalid value for '%s' (%s)."), *Key, *Code), Key);
			}
			FString Range;
			if (!IsWithinClamp(**Found, Target, Range))
			{
				return Values.Fail(TEXT("MetaSound.InvalidConfiguration"), FString::Printf(TEXT("'%s' is out of its range %s."), *Key, *Range), Key);
			}
		}
	}
	if (Node->Configuration.IsValid() && Node->Configuration.GetScriptStruct() == Struct
		&& Struct->CompareScriptStruct(Node->Configuration.GetMemory(), Wanted.GetMemory(), PPF_None))
	{
		return true;
	}
	if (!Builder.SetNodeConfiguration(NodeID, MoveTemp(Wanted), &Edit.PageID))
	{
		return Op.Fail(TEXT("MetaSound.EditFailed"), TEXT("Could not set the node's configuration."), TEXT("configuration"));
	}
	Edit.bChanged = true;
	return true;
}

bool OpNodeSet(FEdit& Edit, FReader& Op)
{
	FGuid NodeID;
	if (!Op.OnlyFields({TEXT("op"), TEXT("id"), TEXT("position"), TEXT("hidden"), TEXT("configuration"), TEXT("defaults"), TEXT("comment"),
		TEXT("comment_visible"), TEXT("hide_unconnected")}) || !ResolveNode(Edit, Op, TEXT("id"), NodeID))
	{
		return false;
	}
	FMetaSoundFrontendDocumentBuilder& Builder = Edit.Builder;
	FVector2D Position;
	bool bHasPosition = false;
	if (!ReadPoint(Op, TEXT("position"), Position, bHasPosition))
	{
		return false;
	}
	if (bHasPosition)
	{
		Edit.Place(NodeID, Position);
	}
	bool bFlag = false;
	if (Op.Has(TEXT("hidden")))
	{
		if (!Op.Bool(TEXT("hidden"), bFlag, true))
		{
			return false;
		}
		if (bFlag)
		{
			Builder.RemoveNodeLocation(NodeID, nullptr, &Edit.PageID);
		}
	}
	if (Op.Has(TEXT("configuration")) && !SetNodeConfiguration(Edit, Op, NodeID))
	{
		return false;
	}
	if (Op.Has(TEXT("defaults")) && !SetNodeDefaults(Edit, Op, NodeID))
	{
		return false;
	}
	if (Op.Has(TEXT("comment")))
	{
		FString Comment;
		if (!Op.String(TEXT("comment"), Comment))
		{
			return false;
		}
		Builder.SetNodeComment(NodeID, MoveTemp(Comment), &Edit.PageID);
	}
	if (Op.Has(TEXT("comment_visible")))
	{
		if (!Op.Bool(TEXT("comment_visible"), bFlag, true))
		{
			return false;
		}
		Builder.SetNodeCommentVisible(NodeID, bFlag, &Edit.PageID);
	}
	if (Op.Has(TEXT("hide_unconnected")))
	{
		if (!Op.Bool(TEXT("hide_unconnected"), bFlag, true))
		{
			return false;
		}
		Builder.SetNodeUnconnectedPinsHidden(NodeID, bFlag, &Edit.PageID);
	}
	Edit.bChanged = true;
	return true;
}

// Edges the editor graph does not show (the variable stack the frontend keeps between a variable's nodes, into
// their hidden "Variable" pin), as the read marks them derived.
bool IsDerivedEdge(FEdit& Edit, const FMetasoundFrontendEdge& Edge)
{
	const FMetasoundFrontendNode* From = Edit.Builder.FindNode(Edge.FromNodeID, &Edit.PageID);
	const FMetasoundFrontendNode* To = Edit.Builder.FindNode(Edge.ToNodeID, &Edit.PageID);
	if (From == nullptr || To == nullptr)
	{
		return true;
	}
	const EMetasoundFrontendClassType FromType = Edit.TypeOf(*From);
	const EMetasoundFrontendClassType ToType = Edit.TypeOf(*To);
	if (FromType == EMetasoundFrontendClassType::Variable)
	{
		return true;
	}
	const FMetasoundFrontendVertex* ToVertex = To->Interface.Inputs.FindByPredicate(
		[&Edge](const FMetasoundFrontendVertex& Vertex) { return Vertex.VertexID == Edge.ToVertexID; });
	return IsVariableFamily(ToType) && ToType != EMetasoundFrontendClassType::Variable && ToVertex != nullptr
		&& ToVertex->Name == FName(TEXT("Variable"));
}

// The editor's ConnectionCausesLoop on the edges it shows: is From reachable downstream of To?
bool CausesLoop(FEdit& Edit, const FGuid& From, const FGuid& To)
{
	const FMetasoundFrontendGraph& Graph = Edit.Builder.FindConstBuildGraphChecked();
	TArray<FGuid> Stack = {To};
	TSet<FGuid> Visited;
	while (!Stack.IsEmpty())
	{
		const FGuid Current = Stack.Pop(EAllowShrinking::No);
		if (Current == From)
		{
			return true;
		}
		if (Visited.Contains(Current))
		{
			continue;
		}
		Visited.Add(Current);
		for (const FMetasoundFrontendEdge& Edge : Graph.Edges)
		{
			if (Edge.FromNodeID == Current && !IsDerivedEdge(Edit, Edge))
			{
				Stack.Add(Edge.ToNodeID);
			}
		}
	}
	return false;
}

bool OpConnect(FEdit& Edit, FReader& Op)
{
	using namespace Metasound::Frontend;
	FGuid NodeID;
	if (!Op.OnlyFields({TEXT("op"), TEXT("id"), TEXT("pin"), TEXT("from"), TEXT("from_pin")}) || !ResolveNode(Edit, Op, TEXT("id"), NodeID))
	{
		return false;
	}
	FMetaSoundFrontendDocumentBuilder& Builder = Edit.Builder;
	const FMetasoundFrontendVertex* Input = FindPin(Edit, Op, TEXT("pin"), NodeID, true, true);
	if (Input == nullptr)
	{
		return false;
	}
	const FGuid InputVertexID = Input->VertexID;
	const FName InputType = Input->TypeName;
	const FString InputName = Input->Name.ToString();
	const FValue* FromValue = Op.Find(TEXT("from"));
	if (FromValue == nullptr || FromValue->Kind == EValueKind::Null)
	{
		if (Builder.IsNodeInputConnected(NodeID, InputVertexID, &Edit.PageID))
		{
			Builder.RemoveEdgeToNodeInput(NodeID, InputVertexID, &Edit.PageID);
			Edit.bChanged = true;
		}
		return true;
	}
	FGuid FromID;
	if (!ResolveNode(Edit, Op, TEXT("from"), FromID))
	{
		return false;
	}
	const FMetasoundFrontendVertex* Output = FindPin(Edit, Op, TEXT("from_pin"), FromID, false, false);
	if (Output == nullptr)
	{
		return false;
	}
	FMetasoundFrontendEdge Edge;
	Edge.FromNodeID = FromID;
	Edge.FromVertexID = Output->VertexID;
	Edge.ToNodeID = NodeID;
	Edge.ToVertexID = InputVertexID;
	if (Builder.ContainsEdge(Edge, &Edit.PageID))
	{
		return true;
	}
	const EInvalidEdgeReason Reason = Builder.IsValidEdge(Edge, &Edit.PageID);
	if (Reason != EInvalidEdgeReason::None)
	{
		FString Message;
		if (Reason == EInvalidEdgeReason::MismatchedAccessType)
		{
			Message = FString::Printf(TEXT("'%s' is a constructor input; constructor input pins can only be connected to constructor output pins "
				"(and '%s' is not one)."), *InputName, *Output->Name.ToString());
		}
		else
		{
			// The node's other variants (the same Namespace.Name, as the editor's node menu groups them) whose input of
			// this name takes the source's type: "UE.Multiply.Audio by Float" for a Float into UE.Multiply.Audio.
			TArray<FString> Overloads;
			TArray<FString> KeepOthers;
			const FMetasoundFrontendNode* Target = Builder.FindNode(NodeID, &Edit.PageID);
			const FMetasoundFrontendClass* TargetClass = Target != nullptr ? Builder.FindDependency(Target->ClassID) : nullptr;
			if (TargetClass != nullptr && TargetClass->Metadata.GetType() == EMetasoundFrontendClassType::External)
			{
				const FMetasoundFrontendClassName& Family = TargetClass->Metadata.GetClassName();
				const FName PinName = Input->Name;
				const FName SourceType = Output->TypeName;
				for (const FMetasoundFrontendClass& Candidate : FindAllRegisteredClasses())
				{
					const FMetasoundFrontendClassName& Name = Candidate.Metadata.GetClassName();
					if (Candidate.Metadata.GetType() == EMetasoundFrontendClassType::External && Name.Namespace == Family.Namespace
						&& Name.Name == Family.Name && Name.Variant != Family.Variant
						&& Candidate.GetDefaultInterface().Inputs.ContainsByPredicate([PinName, SourceType](const FMetasoundFrontendClassInput& Each)
						{
							return Each.Name == PinName && Each.TypeName == SourceType;
						}))
					{
						Overloads.AddUnique(GetClassFullName(Name));
						// Preferred: the variants whose other inputs keep their types (Audio by Float, not Float).
						const bool bKeepsOthers = !TargetClass->GetDefaultInterface().Inputs.ContainsByPredicate(
							[&Candidate, PinName](const FMetasoundFrontendClassInput& Other)
						{
							return Other.Name != PinName && !Candidate.GetDefaultInterface().Inputs.ContainsByPredicate(
								[&Other](const FMetasoundFrontendClassInput& Each) { return Each.Name == Other.Name && Each.TypeName == Other.TypeName; });
						});
						if (bKeepsOthers)
						{
							KeepOthers.AddUnique(GetClassFullName(Name));
						}
					}
				}
			}
			if (!KeepOthers.IsEmpty())
			{
				Overloads = MoveTemp(KeepOthers);
			}
			const FString Hint = Overloads.IsEmpty()
				? FString(TEXT("Put a converter node between them (list_types kind metasound_node, name filter on the two types)."))
				: FString::Printf(TEXT("The same node whose '%s' takes %s: %s (use it as the node's Class), or put a converter node between them."),
					*InputName, *Output->TypeName.ToString(), *JoinNames(Overloads));
			Message = FString::Printf(TEXT("Cannot connect %s (%s) to input '%s' (%s): %s. %s"), *Output->Name.ToString(), *Output->TypeName.ToString(),
				*InputName, *InputType.ToString(), *LexToString(Reason), *Hint);
		}
		return Op.Fail(TEXT("MetaSound.IncompatibleConnection"), Message, TEXT("from"));
	}
	if (FromID == NodeID || CausesLoop(Edit, FromID, NodeID))
	{
		return Op.Fail(TEXT("MetaSound.ConnectionCycle"), FString::Printf(TEXT("Connecting input '%s' to this source would create a loop "
			"(use a GetDelayed variable node for feedback)."), *InputName), TEXT("from"));
	}
	Builder.RemoveEdgeToNodeInput(NodeID, InputVertexID, &Edit.PageID);
	Builder.AddEdge(MoveTemp(Edge), &Edit.PageID);
	Edit.bChanged = true;
	return true;
}

// ---- comments -----------------------------------------------------------------------------------------------

bool OpComment(FEdit& Edit, FReader& Op, bool bRemove)
{
	FGuid CommentID;
	if (bRemove)
	{
		if (!Op.OnlyFields({TEXT("op"), TEXT("id")}) || !Op.Guid(TEXT("id"), CommentID))
		{
			return false;
		}
		if (Edit.Builder.RemoveGraphComment(CommentID, &Edit.PageID))
		{
			Edit.Deleted.Add(GuidText(CommentID));
			Edit.bChanged = true;
		}
		return true;
	}
	FString Text;
	if (!Op.OnlyFields({TEXT("op"), TEXT("id"), TEXT("text"), TEXT("position"), TEXT("size"), TEXT("color"), TEXT("font_size"), TEXT("color_bubble"),
		TEXT("move_mode")}) || !Op.Guid(TEXT("id"), CommentID) || !Op.String(TEXT("text"), Text, false))
	{
		return false;
	}
	FVector2D Position;
	FVector2D Size;
	bool bHasPosition = false;
	bool bHasSize = false;
	if (!ReadPoint(Op, TEXT("position"), Position, bHasPosition) || !ReadPoint(Op, TEXT("size"), Size, bHasSize))
	{
		return false;
	}
	const bool bNew = Edit.Builder.FindGraphComment(CommentID, &Edit.PageID) == nullptr;
	FMetaSoundFrontendGraphComment& Comment = Edit.Builder.FindOrAddGraphComment(CommentID, &Edit.PageID);
	if (bNew)
	{
		// FMetasoundGraphSchemaAction_NewComment: a new comment box's values (UEdGraphNode_Comment defaults).
		const UEdGraphNode_Comment* Defaults = GetDefault<UEdGraphNode_Comment>();
		Comment.Comment = Defaults->NodeComment;
		Comment.Color = Defaults->CommentColor;
		Comment.FontSize = Defaults->FontSize;
		Comment.Size = FIntVector2(Defaults->NodeWidth, Defaults->NodeHeight);
		Comment.bColorBubble = Defaults->bColorCommentBubble ? 1 : 0;
		Edit.Created.Add(GuidText(CommentID));
	}
	if (Op.Has(TEXT("text")))
	{
		Comment.Comment = Text;
	}
	if (bHasPosition)
	{
		Comment.Position = FIntVector2(FMath::RoundToInt32(Position.X), FMath::RoundToInt32(Position.Y));
	}
	if (bHasSize)
	{
		Comment.Size = FIntVector2(FMath::RoundToInt32(Size.X), FMath::RoundToInt32(Size.Y));
	}
	if (const FValue* Color = Op.Find(TEXT("color")))
	{
		FReader ColorReader = Op.Child(TEXT("color"), *Color);
		double R = 0.0;
		double G = 0.0;
		double B = 0.0;
		double A = 1.0;
		if (!ColorReader.OnlyFields({TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A")}) || !ColorReader.Number(TEXT("R"), R) || !ColorReader.Number(TEXT("G"), G)
			|| !ColorReader.Number(TEXT("B"), B) || !ColorReader.Number(TEXT("A"), A, false))
		{
			return false;
		}
		Comment.Color = FLinearColor(static_cast<float>(R), static_cast<float>(G), static_cast<float>(B), static_cast<float>(A));
	}
	if (Op.Has(TEXT("font_size")))
	{
		int64 FontSize = 0;
		if (!Op.Int(TEXT("font_size"), FontSize))
		{
			return false;
		}
		Comment.FontSize = static_cast<int32>(FMath::Clamp<int64>(FontSize, 1, 1000));
	}
	if (Op.Has(TEXT("color_bubble")))
	{
		bool bBubble = false;
		if (!Op.Bool(TEXT("color_bubble"), bBubble, true))
		{
			return false;
		}
		Comment.bColorBubble = bBubble ? 1 : 0;
	}
	if (Op.Has(TEXT("move_mode")))
	{
		FString Mode;
		if (!Op.String(TEXT("move_mode"), Mode))
		{
			return false;
		}
		const UEnum* Modes = StaticEnum<EMetaSoundFrontendGraphCommentMoveMode>();
		const int64 Value = Modes != nullptr ? Modes->GetValueByNameString(Mode) : INDEX_NONE;
		if (Value == INDEX_NONE)
		{
			return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("MoveMode is GroupMovement or NoGroupMovement."), TEXT("move_mode"));
		}
		Comment.MoveMode = static_cast<EMetaSoundFrontendGraphCommentMoveMode>(Value);
	}
	Edit.bChanged = true;
	return true;
}

// The editor's Update Node on one node: the highest registered version of its class, which must have a higher major
// than the node (minor versions are the registration's auto-update, not authored).
bool OpNodeUpdate(FEdit& Edit, FReader& Op)
{
	using namespace Metasound::Frontend;
	FGuid NodeID;
	int64 Major = 0;
	if (!Op.OnlyFields({TEXT("op"), TEXT("id"), TEXT("major")}) || !ResolveNode(Edit, Op, TEXT("id"), NodeID) || !Op.Int(TEXT("major"), Major))
	{
		return false;
	}
	const FMetasoundFrontendNode* Node = Edit.Builder.FindNode(NodeID, &Edit.PageID);
	const FMetasoundFrontendClass* Class = Edit.Builder.FindDependency(Node->ClassID);
	if (Edit.PageID != Metasound::Frontend::DefaultPageID)
	{
		const int64 CurrentMajor = Class != nullptr ? Class->Metadata.GetVersion().Major : Major;
		if (Major == CurrentMajor)
		{
			return true;
		}
		// FEditor::UpdateSelectedNodeClasses goes through the frontend controllers, which only edit the default page.
		return Op.Fail(TEXT("MetaSound.UpdateNodeNotSupported"), FString::Printf(TEXT("The editor's Update Node changes nodes of the Default page "
			"only; on page '%s' delete the node and add one of the version you want under another key."), *GetPageName(Edit.PageID)), TEXT("major"));
	}
	if (Class == nullptr || Class->Metadata.GetType() != EMetasoundFrontendClassType::External)
	{
		return Op.Fail(TEXT("MetaSound.UpdateNodeNotSupported"), TEXT("Update Node changes the version of node classes; this node's version is fixed."), TEXT("major"));
	}
	const FMetasoundFrontendVersionNumber Current = Class->Metadata.GetVersion();
	if (Major == Current.Major)
	{
		return true;
	}
	const FString ClassText = GetClassFullName(Class->Metadata.GetClassName());
	FMetasoundFrontendClass Highest;
	if (!ISearchEngine::Get().FindClassWithHighestVersion(Class->Metadata.GetClassName(), Highest))
	{
		return Op.Fail(TEXT("MetaSound.UnknownNodeClass"), FString::Printf(TEXT("No version of node class '%s' is registered."), *ClassText), TEXT("major"));
	}
	const FMetasoundFrontendVersionNumber Target = Highest.Metadata.GetVersion();
	if (Target.Major != Major || !(Target > Current))
	{
		return Op.Fail(TEXT("MetaSound.UpdateNodeNotSupported"), FString::Printf(TEXT("The editor's Update Node replaces a node with the highest registered "
			"version of its class: '%s' %d -> %d. Set Version to %d, or delete the node and add one of the version you want under another key."),
			*ClassText, Current.Major, Target.Major, Target.Major), TEXT("major"));
	}
	FString Error;
	if (!ReplaceNodeWithVersion(Edit.Asset, NodeID, Target, Error))
	{
		return Op.Fail(TEXT("MetaSound.EditFailed"), FString::Printf(TEXT("Update Node failed: %s"), *Error), TEXT("major"));
	}
	Edit.bChanged = true;
	return true;
}

// A preset input's "override inherited default" check box (FMetasoundInputDetailCustomization::Clear/
// SetInputInheritsDefault): SetInputInheritsDefault. The registration at the end of the apply rebuilds the preset
// graph (FRebuildPresetRootGraph), which gives an inheriting input the parent's default again.
bool OpInputInherit(FEdit& Edit, FReader& Op)
{
	FName Name;
	bool bInherits = false;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("inherits")}) || !ReadName(Op, TEXT("name"), Name) || !Op.Bool(TEXT("inherits"), bInherits, true))
	{
		return false;
	}
	if (!Edit.Builder.IsPreset())
	{
		return Op.Fail(TEXT("MetaSound.NotAPreset"), TEXT("Only a preset's inputs inherit their parent's defaults."), TEXT("name"));
	}
	if (Edit.Builder.FindGraphInput(Name) == nullptr)
	{
		return Op.Fail(TEXT("MetaSound.UnknownMember"), FString::Printf(TEXT("The preset has no input '%s'."), *Name.ToString()), TEXT("name"));
	}
	if (SetInputInheritsDefault(Edit.Builder, Name, bInherits))
	{
		Edit.bChanged = true;
	}
	return true;
}

// The editor's Convert From Preset (FEditor::ConvertFromPreset: UMetaSoundBuilderBase::ConvertFromPreset): the graph
// the preset was rebuilt with becomes the MetaSound's own, editable graph.
bool OpConvertFromPreset(FEdit& Edit, FReader& Op)
{
	if (!Op.OnlyFields({TEXT("op")}))
	{
		return false;
	}
	if (!Edit.Builder.IsPreset())
	{
		return true;
	}
	EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
	Edit.BuilderObject.ConvertFromPreset(Result);
	if (Result != EMetaSoundBuilderResult::Succeeded)
	{
		return Op.Fail(TEXT("MetaSound.EditFailed"), TEXT("Could not convert the preset."));
	}
	Edit.bChanged = true;
	return true;
}

// ---- pages ---------------------------------------------------------------------------------------------------

// The Pages Details of a MetaSound (FMetasoundPagesDetailCustomization): "+" adds a page of the project settings that
// the MetaSound does not implement yet, as a copy of its last page (AddGraphPage with bDuplicateLastGraph; the
// editor's bSetAsBuildGraph focuses the new page, view state an op does not change); a page's delete button removes
// it (RemoveGraphPage). Both Modify() the MetaSound and its builder object inside the call's transaction.
bool OpPage(FEdit& Edit, FReader& Op, bool bRemove)
{
	FString Name;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name")}) || !Op.String(TEXT("name"), Name))
	{
		return false;
	}
	if (Name == Metasound::Frontend::DefaultPageName)
	{
		return Op.Fail(TEXT("MetaSound.DefaultPageRequired"), TEXT("Every MetaSound has the Default page; it cannot be added or removed."), TEXT("name"));
	}
	FGuid PageID;
	bool bConfigured = false;
	const bool bKnown = FindPageID(Name, PageID, &bConfigured);
	const bool bHas = bKnown && Edit.Builder.GetConstDocumentChecked().RootGraph.ContainsGraphPage(PageID);
	if (bRemove)
	{
		if (!bHas)
		{
			return true;
		}
		Edit.BuilderObject.Modify();
		if (!Edit.Builder.RemoveGraphPage(PageID))
		{
			return Op.Fail(TEXT("MetaSound.EditFailed"), FString::Printf(TEXT("Could not remove page '%s'."), *Name), TEXT("name"));
		}
		Edit.bChanged = true;
		return true;
	}
	if (!bKnown || !bConfigured)
	{
		return Op.Fail(TEXT("MetaSound.UnknownPage"), FString::Printf(TEXT("The project's MetaSound page settings (Project Settings > MetaSound > "
			"Pages) define no page '%s'; pages: %s."), *Name, *JoinPageNames(GetConfiguredPageNames())), TEXT("name"));
	}
	if (bHas)
	{
		return true;
	}
	Edit.BuilderObject.Modify();
	constexpr bool bDuplicateLastGraph = true;
	constexpr bool bSetAsBuildGraph = false;
	Edit.Builder.AddGraphPage(PageID, bDuplicateLastGraph, bSetAsBuildGraph);
	Edit.bChanged = true;
	return true;
}

bool IsPageOp(const FString& Kind)
{
	return Kind == TEXT("normalize") || Kind.StartsWith(TEXT("variable.")) || Kind.StartsWith(TEXT("node.")) || Kind == TEXT("connect")
		|| Kind.StartsWith(TEXT("comment."));
}

// The page a graph op names (its "page"; Default without one).
bool ResolveOpPage(FEdit& Edit, FReader& Op, const FString& Kind)
{
	Edit.PageID = Metasound::Frontend::DefaultPageID;
	if (!Op.Has(TEXT("page")))
	{
		return true;
	}
	FString Name;
	if (!Op.String(TEXT("page"), Name))
	{
		return false;
	}
	if (!IsPageOp(Kind))
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("'%s' edits the whole MetaSound; only graph ops take a page."), *Kind), TEXT("page"));
	}
	FGuid PageID;
	if (!FindPageID(Name, PageID) || !Edit.Builder.GetConstDocumentChecked().RootGraph.ContainsGraphPage(PageID))
	{
		return Op.Fail(TEXT("MetaSound.UnknownPage"), FString::Printf(TEXT("The MetaSound has no page '%s'; its pages: %s."), *Name,
			*JoinPageNames(DocumentPageNames(Edit))), TEXT("page"));
	}
	Edit.PageID = PageID;
	return true;
}

bool RunOp(FEdit& Edit, FReader& Op, const FString& Kind)
{
	if (Kind == TEXT("page.add")) { return OpPage(Edit, Op, false); }
	if (Kind == TEXT("page.remove")) { return OpPage(Edit, Op, true); }
	if (Kind == TEXT("normalize")) { return OpNormalize(Edit, Op); }
	if (Kind == TEXT("interfaces")) { return OpInterfaces(Edit, Op); }
	if (Kind == TEXT("metadata")) { return OpMetadata(Edit, Op); }
	if (Kind == TEXT("input.add")) { return OpMemberAdd(Edit, Op, true); }
	if (Kind == TEXT("input.set")) { return OpMemberSet(Edit, Op, true); }
	if (Kind == TEXT("input.remove")) { return OpMemberRemove(Edit, Op, true); }
	if (Kind == TEXT("input.widget")) { return OpInputWidget(Edit, Op); }
	if (Kind == TEXT("output.add")) { return OpMemberAdd(Edit, Op, false); }
	if (Kind == TEXT("output.set")) { return OpMemberSet(Edit, Op, false); }
	if (Kind == TEXT("output.remove")) { return OpMemberRemove(Edit, Op, false); }
	if (Kind.StartsWith(TEXT("variable."))) { return OpVariable(Edit, Op, Kind); }
	if (Kind == TEXT("node.create")) { return OpNodeCreate(Edit, Op); }
	if (Kind == TEXT("node.delete")) { return OpNodeDelete(Edit, Op); }
	if (Kind == TEXT("node.set")) { return OpNodeSet(Edit, Op); }
	if (Kind == TEXT("node.update")) { return OpNodeUpdate(Edit, Op); }
	if (Kind == TEXT("input.inherit")) { return OpInputInherit(Edit, Op); }
	if (Kind == TEXT("preset.convert_from")) { return OpConvertFromPreset(Edit, Op); }
	if (Kind == TEXT("connect")) { return OpConnect(Edit, Op); }
	if (Kind == TEXT("comment.set")) { return OpComment(Edit, Op, false); }
	if (Kind == TEXT("comment.remove")) { return OpComment(Edit, Op, true); }
	return Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("Unknown op '%s'."), *Kind), TEXT("op"));
}
} // namespace

namespace MetaSound
{
bool IsOpenInMetaSoundEditor(const UObject& Asset)
{
	using namespace MetaSoundEditLocal;
	UAssetEditorSubsystem* AssetEditors = GEditor != nullptr ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	return AssetEditors != nullptr && AssetEditors->FindEditorForAsset(const_cast<UObject*>(&Asset), false) != nullptr;
}
} // namespace MetaSound

bool HandleMetaSoundEdit(FCall& Call, FObjectBuilder& Result)
{
	using namespace MetaSoundEditLocal;
	const FValue* Ops = nullptr;
	if (!Call.Args.OnlyFields({TEXT("ops")}) || !Call.Args.Array(TEXT("ops"), Ops))
	{
		return false;
	}
	if (AsMetaSound(Call.Asset) == nullptr)
	{
		return Fail(Call, TEXT("MetaSound.UnsupportedAsset"), TEXT("MetaSound primitives edit MetaSound Sources and Patches."), TEXT("/HandleArguments/asset"));
	}
	FAssetDocumentEditorNonInteractiveScope NonInteractive;
	const FScopedTransaction Transaction(LOCTEXT("EditMetaSound", "Edit MetaSound"));
	Call.Asset.Modify();
	// The open editor's graph, as the editor's own edits (FEditor: MetaSound->Modify(), Graph->Modify()).
	const Metasound::Frontend::IMetaSoundAssetManager* Manager = Metasound::Frontend::IMetaSoundAssetManager::Get();
	FMetasoundAssetBase* AssetBase = Manager != nullptr ? Manager->GetAsAsset(Call.Asset) : nullptr;
	if (UEdGraph* EditorGraph = AssetBase != nullptr && IsOpenInMetaSoundEditor(Call.Asset) ? AssetBase->GetGraph() : nullptr)
	{
		EditorGraph->Modify();
	}
	UMetaSoundBuilderBase& BuilderObject = Metasound::Engine::FDocumentBuilderRegistry::GetChecked().FindOrBeginBuilding(Call.Asset);
	FEdit Edit(Call, BuilderObject);
	for (int32 Index = 0; Index < Ops->Elements.Num(); ++Index)
	{
		const FString OpPath = FString::Printf(TEXT("ops/%d"), Index);
		FReader Op = Call.Args.Child(OpPath, Ops->Elements[Index]);
		FString Kind;
		if (!Op.IsObject())
		{
			return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("Each op is an object."));
		}
		if (!Op.String(TEXT("op"), Kind) || !ResolveOpPage(Edit, Op, Kind))
		{
			return false;
		}
		// The op's own fields, without "page" (resolved above).
		FValue Fields = Ops->Elements[Index];
		const int32 PageField = Fields.ObjectKeys.IndexOfByKey(TEXT("page"));
		if (PageField != INDEX_NONE)
		{
			Fields.ObjectKeys.RemoveAt(PageField);
			Fields.Elements.RemoveAt(PageField);
		}
		FReader OpFields = Call.Args.Child(OpPath, Fields);
		if (!RunOp(Edit, OpFields, Kind))
		{
			return false;
		}
		if (NonInteractive.WasInteractionRefused())
		{
			NonInteractive.DiscardCreatedPackages();
			return Op.Fail(TEXT("MetaSound.InteractiveEditRefused"), TEXT("The edit tried to open an editor dialog, which an MCP request cannot answer; "
				"the edit was refused."));
		}
	}
	if (Edit.bRemoveUnusedDependencies)
	{
		Edit.Builder.RemoveUnusedDependencies();
	}
	if (Edit.bChanged)
	{
		Call.Asset.MarkPackageDirty();
	}
	Result.Add(TEXT("created"), MakeStringArray(Edit.Created));
	Result.Add(TEXT("deleted"), MakeStringArray(Edit.Deleted));
	Result.AddBool(TEXT("changed"), Edit.bChanged);
	Result.Add(TEXT("warnings"), MakeStringArray(Edit.Warnings));
	return true;
}
} // namespace UE::AssetDocument::Graph::Audio

#undef LOCTEXT_NAMESPACE
