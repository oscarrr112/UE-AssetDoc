// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// audio.metasound.read.v1: the document of a MetaSound Source / Patch as the MetaSound editor shows it after
// opening (FNormalizedDocument), as flat records the Python profile assembles into the Body (design §3).
//
//   {validate: true} -> {status, messages[{severity, message, node_id, node_ids[], page, source}]}: validation without
//   registration (source "validation", every page) and the operator build check of the graph registered before
//   (source "build", when the MetaSound is registered)
//   {offset?, pins?} -> {total, next_offset, records[]}; records (in this order):
//   {kind: header, class, preset, preset_parent, inherits[], interfaces[{name, version, modifiable, registered}],
//    metadata{display_name, description, author, keywords[], categories[], referenceable, deprecated},
//    pages[{id, name}] (Default, configured pages in settings order, other pages), build_page, registered_page,
//    configured_pages[] (Default + UMetaSoundSettings pages), injected[], audio_device_required?}
//   {kind: input, name, type, constructor, display_name, description, sort_order, advanced, node_id, interface,
//    defaults[{page, value}], interface_def?{default?, sort_order, advanced}, widget{}, widget_defaults?{}}
//   {kind: output, name, type, constructor, display_name, description, sort_order, advanced, node_id, interface,
//    interface_def?{sort_order, advanced}}
//   {kind: variable, page, name, type, default, display_name, description, id}
//   {kind: node, page, id, class, class_name, class_type, major, minor, member, location?, locations?, inputs[],
//    outputs[], defaults{}, configuration?{type, properties{}, defaults{}}, comment, comment_visible, hide_unconnected,
//    derived, pins?{inputs[{name, type, access, default?}], outputs[{name, type, access}]}}
//   {kind: edge, page, from, from_pin, to, to_pin, derived}
//   {kind: comment, page, id, text, position, size, color{R,G,B,A}, font_size, depth, color_bubble, move_mode}
//
// Node "class" is how a document names it: "Input" (input template node, or the hidden input node, which is
// derived), "Output", "Get" / "GetDelayed" / "Set" (variable accessor / deferred accessor / mutator),
// "Variable" (derived), a MetaSound object path for an asset class, otherwise the class full name.
// "defaults" holds the node's input literals that differ from the class default; "derived" nodes and edges
// (hidden input nodes, variable nodes, the input -> template edges and the variable stack) are what the
// editor recreates from the members and are not authored.

#include "AssetDocumentMetaSound.h"

#include "AssetDocumentAudioPrimitives.h"
#include "AssetDocumentEditorPropertyValueCodec.h"
#include "Dom/JsonObject.h"
#include "MetasoundFrontendDocumentBuilder.h"
#include "NodeTemplates/MetasoundFrontendNodeTemplateInput.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Package.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::Graph::Audio
{
namespace MetaSoundReadLocal
{
using namespace MetaSound;

FString GuidText(const FGuid& Guid)
{
	return Guid.ToString(EGuidFormats::Digits);
}

FString TextOf(const FText& Text)
{
	return Text.ToString();
}

FValue MakeTextArray(const TArray<FText>& Texts)
{
	TArray<FString> Values;
	for (const FText& Text : Texts)
	{
		Values.Add(Text.ToString());
	}
	return MakeStringArray(Values);
}

FValue MakePoint(double X, double Y)
{
	TArray<FValue> Elements;
	Elements.Add(MakeCoordinate(X));
	Elements.Add(MakeCoordinate(Y));
	return MakeArray(MoveTemp(Elements));
}

const TCHAR* ClassTypeName(EMetasoundFrontendClassType Type)
{
	switch (Type)
	{
	case EMetasoundFrontendClassType::External: return TEXT("External");
	case EMetasoundFrontendClassType::Graph: return TEXT("Graph");
	case EMetasoundFrontendClassType::Input: return TEXT("Input");
	case EMetasoundFrontendClassType::Output: return TEXT("Output");
	case EMetasoundFrontendClassType::Literal: return TEXT("Literal");
	case EMetasoundFrontendClassType::Variable: return TEXT("Variable");
	case EMetasoundFrontendClassType::VariableDeferredAccessor: return TEXT("VariableDeferredAccessor");
	case EMetasoundFrontendClassType::VariableAccessor: return TEXT("VariableAccessor");
	case EMetasoundFrontendClassType::VariableMutator: return TEXT("VariableMutator");
	case EMetasoundFrontendClassType::Template: return TEXT("Template");
	default: return TEXT("Invalid");
	}
}

// The stack input of variable accessor / mutator nodes (MetasoundVariableNodes.h InputVariable).
FName VariableStackPinName()
{
	return FName(TEXT("Variable"));
}

bool IsVariableFamily(EMetasoundFrontendClassType Type)
{
	return Type == EMetasoundFrontendClassType::Variable || Type == EMetasoundFrontendClassType::VariableAccessor
		|| Type == EMetasoundFrontendClassType::VariableDeferredAccessor || Type == EMetasoundFrontendClassType::VariableMutator;
}

const TCHAR* AccessName(EMetasoundFrontendVertexAccessType Access)
{
	switch (Access)
	{
	case EMetasoundFrontendVertexAccessType::Reference: return TEXT("Reference");
	case EMetasoundFrontendVertexAccessType::Value: return TEXT("Value");
	default: return TEXT("Unset");
	}
}

// Sparse values of the editable, non-transient properties of Object that differ from Defaults (same layout),
// through the typed value codec (the member-literal widget settings, node configurations).
FValue SparseProperties(const UStruct& Struct, const void* Container, const void* Defaults)
{
	FObjectBuilder Out;
	for (TFieldIterator<FProperty> It(&Struct); It; ++It)
	{
		FProperty* Property = *It;
		if (!Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
		{
			continue;
		}
		if (Property->GetOwnerStruct() == UObject::StaticClass())
		{
			continue;
		}
		const void* Value = Property->ContainerPtrToValuePtr<void>(Container);
		if (Defaults != nullptr && Property->Identical(Value, Property->ContainerPtrToValuePtr<void>(Defaults)))
		{
			continue;
		}
		FValue Encoded;
		FString Code;
		if (UE::AssetDocument::EditorPropertyValueCodec::Encode(*Property, Value, Encoded, Code))
		{
			Out.Add(Property->GetAuthoredName(), MoveTemp(Encoded));
		}
	}
	return Out.Build();
}

// The widget settings that differ from those the editor gives a new settings object (MakeEditorWidgetDefaults).
FValue DescribeWidget(const UObject* MemberMetadata, const UObject* EditorDefaults)
{
	if (MemberMetadata == nullptr)
	{
		return MakeEmptyObject();
	}
	return SparseProperties(*MemberMetadata->GetClass(), MemberMetadata, EditorDefaults);
}

// A node configuration: its struct, the editable properties that differ from the configuration a new node of the
// class gets (MakeDefaultNodeConfiguration; the struct's defaults when the class no longer provides that struct), and
// all editable properties of that default ("defaults": an authored value equal to it is no change).
FValue DescribeConfiguration(const TInstancedStruct<FMetaSoundFrontendNodeConfiguration>& Configuration, const FMetasoundFrontendClass* Class)
{
	const UScriptStruct* Struct = Configuration.GetScriptStruct();
	FObjectBuilder Out;
	Out.AddString(TEXT("type"), Struct->GetPathName());
	const TInstancedStruct<FMetaSoundFrontendNodeConfiguration> ClassDefault = Class != nullptr ? MakeDefaultNodeConfiguration(Class->Metadata)
		: TInstancedStruct<FMetaSoundFrontendNodeConfiguration>();
	if (ClassDefault.IsValid() && ClassDefault.GetScriptStruct() == Struct)
	{
		Out.Add(TEXT("properties"), SparseProperties(*Struct, Configuration.GetMemory(), ClassDefault.GetMemory()));
		Out.Add(TEXT("defaults"), SparseProperties(*Struct, ClassDefault.GetMemory(), nullptr));
	}
	else
	{
		FStructOnScope Defaults(Struct);
		Out.Add(TEXT("properties"), SparseProperties(*Struct, Configuration.GetMemory(), Defaults.GetStructMemory()));
		Out.Add(TEXT("defaults"), SparseProperties(*Struct, Defaults.GetStructMemory(), nullptr));
	}
	return Out.Build();
}

struct FReadContext
{
	const FNormalizedDocument& Normalized;
	const FMetaSoundFrontendDocumentBuilder& Builder;
	const FMetasoundFrontendDocument& Document;
	FAssetClassPaths AssetPaths;
	bool bPins = false;

	explicit FReadContext(const FNormalizedDocument& InNormalized, bool bInPins)
		: Normalized(InNormalized)
		, Builder(InNormalized.GetBuilder())
		, Document(InNormalized.GetDocument())
		, AssetPaths(InNormalized.GetAsset())
		, bPins(bInPins)
	{
	}
};

FValue MakeHeader(const FReadContext& Context)
{
	const FMetasoundFrontendDocument& Document = Context.Document;
	const FMetasoundFrontendGraphClass& Root = Document.RootGraph;
	FObjectBuilder Header;
	Header.AddString(TEXT("kind"), TEXT("header"));
	Header.AddString(TEXT("class"), Context.Normalized.GetAsset().GetClass()->GetPathName());
	const bool bPreset = Context.Builder.IsPreset();
	Header.AddBool(TEXT("preset"), bPreset);

	FString Parent;
	if (bPreset)
	{
		// A preset's root graph holds the one referenced parent node (FRebuildPresetRootGraph).
		for (const FMetasoundFrontendClass& Dependency : Document.Dependencies)
		{
			const EMetasoundFrontendClassType Type = Dependency.Metadata.GetType();
			if (Type == EMetasoundFrontendClassType::External || Type == EMetasoundFrontendClassType::Graph)
			{
				const FString Path = Context.AssetPaths.Find(Dependency.Metadata);
				if (!Path.IsEmpty())
				{
					Parent = Path;
					break;
				}
			}
		}
	}
	Header.AddString(TEXT("preset_parent"), Parent);
	TArray<FString> Inherits;
	for (const FName& Name : GetInputsInheritingDefault(Context.Builder))
	{
		Inherits.Add(Name.ToString());
	}
	Inherits.Sort();
	Header.Add(TEXT("inherits"), MakeStringArray(Inherits));

	TArray<FValue> Interfaces;
	for (const FInterfaceFacts& Facts : DescribeInterfaces(Document, *Context.Normalized.GetAsset().GetClass()))
	{
		FObjectBuilder Row;
		Row.AddString(TEXT("name"), Facts.Version.Name.ToString());
		Row.AddString(TEXT("version"), Facts.Version.Number.ToString());
		Row.AddBool(TEXT("modifiable"), Facts.bModifiable);
		Row.AddBool(TEXT("registered"), Facts.bRegistered);
		Interfaces.Add(Row.Build());
	}
	Header.Add(TEXT("interfaces"), MakeArray(MoveTemp(Interfaces)));

	const FMetasoundFrontendClassMetadata& Metadata = Root.Metadata;
	FObjectBuilder Meta;
	Meta.AddString(TEXT("display_name"), TextOf(Metadata.GetDisplayName()));
	Meta.AddString(TEXT("description"), TextOf(Metadata.GetDescription()));
	Meta.AddString(TEXT("author"), Metadata.GetAuthor());
	Meta.Add(TEXT("keywords"), MakeTextArray(Metadata.GetKeywords()));
	Meta.Add(TEXT("categories"), MakeTextArray(Metadata.GetCategoryHierarchy()));
	const EMetasoundFrontendClassAccessFlags Flags = Metadata.GetAccessFlags();
	Meta.AddBool(TEXT("referenceable"), EnumHasAnyFlags(Flags, EMetasoundFrontendClassAccessFlags::Referenceable));
	Meta.AddBool(TEXT("deprecated"), EnumHasAnyFlags(Flags, EMetasoundFrontendClassAccessFlags::Deprecated));
	Header.Add(TEXT("metadata"), Meta.Build());

	TArray<FValue> Pages;
	for (const FGuid& PageID : GetOrderedPageIDs(Root))
	{
		FObjectBuilder Row;
		Row.AddString(TEXT("id"), GuidText(PageID));
		Row.AddString(TEXT("name"), GetPageName(PageID));
		Pages.Add(Row.Build());
	}
	Header.Add(TEXT("pages"), MakeArray(MoveTemp(Pages)));
	Header.AddString(TEXT("build_page"), GetPageName(Context.Builder.GetBuildPageID()));
	Header.AddString(TEXT("registered_page"), GetPageName(GetRegisteredPageID(Root)));
	Header.Add(TEXT("configured_pages"), MakeStringArray(GetConfiguredPageNames()));
	TArray<FString> Injected;
	for (const FGuid& Id : Context.Normalized.GetInjectedNodeIds())
	{
		Injected.Add(GuidText(Id));
	}
	Header.Add(TEXT("injected"), MakeStringArray(Injected));
	// Registering this MetaSound (or making a preset of it) would crash an editor without audio (-NoSound).
	const FString NeedsAudio = FindLiteralNeedingAudioDevice(Context.Normalized.GetAsset());
	if (!NeedsAudio.IsEmpty())
	{
		Header.AddString(TEXT("audio_device_required"), NeedsAudio);
	}
	return Header.Build();
}

// Interface that declares a member name, with its definition.
const FMetasoundFrontendInterface* FindMemberInterface(const TArray<FInterfaceFacts>& Interfaces, FName Member, bool bInput)
{
	for (const FInterfaceFacts& Facts : Interfaces)
	{
		if (!Facts.Definition.IsSet())
		{
			continue;
		}
		const FMetasoundFrontendInterface& Definition = Facts.Definition.GetValue();
		const bool bFound = bInput
			? Definition.Inputs.ContainsByPredicate([Member](const FMetasoundFrontendClassInput& Input) { return Input.Name == Member; })
			: Definition.Outputs.ContainsByPredicate([Member](const FMetasoundFrontendClassOutput& Output) { return Output.Name == Member; });
		if (bFound)
		{
			return &Definition;
		}
	}
	return nullptr;
}

void AddVertexFields(FObjectBuilder& Row, const FMetasoundFrontendClassVertex& Vertex)
{
	Row.AddString(TEXT("name"), Vertex.Name.ToString());
	Row.AddString(TEXT("type"), Vertex.TypeName.ToString());
	Row.AddBool(TEXT("constructor"), Vertex.AccessType == EMetasoundFrontendVertexAccessType::Value);
	Row.AddString(TEXT("display_name"), TextOf(Vertex.Metadata.GetDisplayName()));
	Row.AddString(TEXT("description"), TextOf(Vertex.Metadata.GetDescription()));
	Row.AddInt(TEXT("sort_order"), Vertex.Metadata.SortOrderIndex);
	Row.AddBool(TEXT("advanced"), Vertex.Metadata.bIsAdvancedDisplay);
	Row.AddString(TEXT("node_id"), GuidText(Vertex.NodeID));
}

void AddMembers(const FReadContext& Context, TArray<FValue>& Records)
{
	const FMetasoundFrontendClassInterface& Interface = Context.Document.RootGraph.GetDefaultInterface();
	const TArray<FInterfaceFacts> Interfaces = DescribeInterfaces(Context.Document, *Context.Normalized.GetAsset().GetClass());
	for (const FMetasoundFrontendClassInput& Input : Interface.Inputs)
	{
		FObjectBuilder Row;
		Row.AddString(TEXT("kind"), TEXT("input"));
		AddVertexFields(Row, Input);
		const FMetasoundFrontendInterface* Owner = FindMemberInterface(Interfaces, Input.Name, true);
		Row.AddString(TEXT("interface"), Owner != nullptr ? Owner->Metadata.Version.Name.ToString() : FString());
		TArray<FValue> Defaults;
		for (const FMetasoundFrontendClassInputDefault& Default : Input.GetDefaults())
		{
			FObjectBuilder Page;
			Page.AddString(TEXT("page"), GetPageName(Default.PageID));
			Page.Add(TEXT("value"), EncodeLiteral(Default.Literal, Input.TypeName));
			Defaults.Add(Page.Build());
		}
		Row.Add(TEXT("defaults"), MakeArray(MoveTemp(Defaults)));
		if (Owner != nullptr)
		{
			const FMetasoundFrontendClassInput* Definition = Owner->Inputs.FindByPredicate(
				[&Input](const FMetasoundFrontendClassInput& Candidate) { return Candidate.Name == Input.Name; });
			FObjectBuilder Def;
			if (const FMetasoundFrontendLiteral* Literal = Definition->FindConstDefault(Metasound::Frontend::DefaultPageID))
			{
				Def.Add(TEXT("default"), EncodeLiteral(*Literal, Input.TypeName));
			}
			Def.AddInt(TEXT("sort_order"), Definition->Metadata.SortOrderIndex);
			Def.AddBool(TEXT("advanced"), Definition->Metadata.bIsAdvancedDisplay);
			Row.Add(TEXT("interface_def"), Def.Build());
		}
		const UObject* Widget = Context.Builder.FindMemberMetadata(Input.NodeID);
		const TStrongObjectPtr<UObject> EditorDefaults = Widget != nullptr
			? MakeEditorWidgetDefaults(*Widget->GetClass(), Context.Builder.IsPreset()) : nullptr;
		Row.Add(TEXT("widget"), DescribeWidget(Widget, EditorDefaults.Get()));
		if (Widget != nullptr)
		{
			// Every setting of a new settings object as the editor makes it (what an omitted one is written as): an
			// authored setting equal to it reads back omitted, and opening the editor, which re-initializes a Float
			// input's WidgetType, changes nothing the document shows.
			Row.Add(TEXT("widget_defaults"), SparseProperties(*Widget->GetClass(), EditorDefaults.Get(), nullptr));
		}
		Records.Add(Row.Build());
	}
	for (const FMetasoundFrontendClassOutput& Output : Interface.Outputs)
	{
		FObjectBuilder Row;
		Row.AddString(TEXT("kind"), TEXT("output"));
		AddVertexFields(Row, Output);
		const FMetasoundFrontendInterface* Owner = FindMemberInterface(Interfaces, Output.Name, false);
		Row.AddString(TEXT("interface"), Owner != nullptr ? Owner->Metadata.Version.Name.ToString() : FString());
		if (Owner != nullptr)
		{
			const FMetasoundFrontendClassOutput* Definition = Owner->Outputs.FindByPredicate(
				[&Output](const FMetasoundFrontendClassOutput& Candidate) { return Candidate.Name == Output.Name; });
			FObjectBuilder Def;
			Def.AddInt(TEXT("sort_order"), Definition->Metadata.SortOrderIndex);
			Def.AddBool(TEXT("advanced"), Definition->Metadata.bIsAdvancedDisplay);
			Row.Add(TEXT("interface_def"), Def.Build());
		}
		Records.Add(Row.Build());
	}
}

// Member a node stands for: the input of an input template / hidden input node, the output of an output
// node, the variable of a variable-family node.
FString FindMember(const FReadContext& Context, const FMetasoundFrontendGraph& Graph, const FMetasoundFrontendNode& Node, const FMetasoundFrontendClass& Class)
{
	const EMetasoundFrontendClassType Type = Class.Metadata.GetType();
	if (Type == EMetasoundFrontendClassType::Input || Type == EMetasoundFrontendClassType::Output)
	{
		return Node.Name.ToString();
	}
	if (Type == EMetasoundFrontendClassType::Template && Class.Metadata.GetClassName() == Metasound::Frontend::FInputNodeTemplate::ClassName)
	{
		for (const FMetasoundFrontendEdge& Edge : Graph.Edges)
		{
			if (Edge.ToNodeID != Node.GetID())
			{
				continue;
			}
			if (const FMetasoundFrontendNode* InputNode = Context.Builder.FindNode(Edge.FromNodeID, &Graph.PageID))
			{
				return InputNode->Name.ToString();
			}
		}
		return FString();
	}
	if (IsVariableFamily(Type))
	{
		for (const FMetasoundFrontendVariable& Variable : Graph.Variables)
		{
			if (Variable.VariableNodeID == Node.GetID() || Variable.MutatorNodeID == Node.GetID()
				|| Variable.AccessorNodeIDs.Contains(Node.GetID()) || Variable.DeferredAccessorNodeIDs.Contains(Node.GetID()))
			{
				return Variable.Name.ToString();
			}
		}
	}
	return FString();
}

FString ClassReference(const FReadContext& Context, const FMetasoundFrontendClass& Class)
{
	const FMetasoundFrontendClassMetadata& Metadata = Class.Metadata;
	switch (Metadata.GetType())
	{
	case EMetasoundFrontendClassType::Input: return TEXT("Input");
	case EMetasoundFrontendClassType::Output: return TEXT("Output");
	case EMetasoundFrontendClassType::Variable: return TEXT("Variable");
	case EMetasoundFrontendClassType::VariableAccessor: return TEXT("Get");
	case EMetasoundFrontendClassType::VariableDeferredAccessor: return TEXT("GetDelayed");
	case EMetasoundFrontendClassType::VariableMutator: return TEXT("Set");
	case EMetasoundFrontendClassType::Template:
		if (Metadata.GetClassName() == Metasound::Frontend::FInputNodeTemplate::ClassName)
		{
			return TEXT("Input");
		}
		break;
	case EMetasoundFrontendClassType::External:
	case EMetasoundFrontendClassType::Graph:
	{
		const FString Path = Context.AssetPaths.Find(Metadata);
		if (!Path.IsEmpty())
		{
			return Path;
		}
		break;
	}
	default:
		break;
	}
	return GetClassFullName(Metadata.GetClassName());
}

FValue MakeNode(const FReadContext& Context, const FMetasoundFrontendGraph& Graph, const FMetasoundFrontendNode& Node)
{
	const FMetaSoundFrontendDocumentBuilder& Builder = Context.Builder;
	const FGuid& PageID = Graph.PageID;
	const FMetasoundFrontendClass* Class = Builder.FindDependency(Node.ClassID);
	FObjectBuilder Row;
	Row.AddString(TEXT("kind"), TEXT("node"));
	Row.AddString(TEXT("page"), GetPageName(PageID));
	Row.AddString(TEXT("id"), GuidText(Node.GetID()));
	const EMetasoundFrontendClassType Type = Class != nullptr ? Class->Metadata.GetType() : EMetasoundFrontendClassType::Invalid;
	if (Class != nullptr)
	{
		Row.AddString(TEXT("class"), ClassReference(Context, *Class));
		Row.AddString(TEXT("class_name"), GetClassFullName(Class->Metadata.GetClassName()));
		Row.AddInt(TEXT("major"), Class->Metadata.GetVersion().Major);
		Row.AddInt(TEXT("minor"), Class->Metadata.GetVersion().Minor);
		Row.AddString(TEXT("member"), FindMember(Context, Graph, Node, *Class));
	}
	else
	{
		// A node whose class snapshot is missing (the editor reports "class not found").
		Row.AddString(TEXT("class"), FString());
		Row.AddString(TEXT("class_name"), FString());
		Row.AddInt(TEXT("major"), 0);
		Row.AddInt(TEXT("minor"), 0);
		Row.AddString(TEXT("member"), Node.Name.ToString());
	}
	Row.AddString(TEXT("class_type"), ClassTypeName(Type));

	const TMap<FGuid, FVector2D>& Locations = Node.Style.Display.Locations;
	if (!Locations.IsEmpty())
	{
		// One location since document 1.12; with more, the one with the lowest editor-node key.
		const TPair<FGuid, FVector2D>* First = nullptr;
		for (const TPair<FGuid, FVector2D>& Pair : Locations)
		{
			if (First == nullptr || GuidText(Pair.Key) < GuidText(First->Key))
			{
				First = &Pair;
			}
		}
		Row.Add(TEXT("location"), MakePoint(First->Value.X, First->Value.Y));
		if (Locations.Num() > 1)
		{
			Row.AddInt(TEXT("locations"), Locations.Num());
		}
	}

	TArray<FString> Inputs;
	for (const FMetasoundFrontendVertex& Vertex : Node.Interface.Inputs)
	{
		Inputs.Add(Vertex.Name.ToString());
	}
	TArray<FString> Outputs;
	for (const FMetasoundFrontendVertex& Vertex : Node.Interface.Outputs)
	{
		Outputs.Add(Vertex.Name.ToString());
	}
	Row.Add(TEXT("inputs"), MakeStringArray(Inputs));
	Row.Add(TEXT("outputs"), MakeStringArray(Outputs));

	FObjectBuilder Defaults;
	for (const FMetasoundFrontendVertexLiteral& Literal : Node.InputLiterals)
	{
		const FMetasoundFrontendVertex* Vertex = Node.Interface.Inputs.FindByPredicate(
			[&Literal](const FMetasoundFrontendVertex& Candidate) { return Candidate.VertexID == Literal.VertexID; });
		if (Vertex == nullptr)
		{
			continue;
		}
		const FMetasoundFrontendLiteral* ClassDefault = FindNodeInputClassDefault(Builder, Node.GetID(), Vertex->VertexID, &PageID);
		if (ClassDefault != nullptr && LiteralsEqual(*ClassDefault, Literal.Value, Vertex->TypeName))
		{
			continue;
		}
		Defaults.Add(Vertex->Name.ToString(), EncodeLiteral(Literal.Value, Vertex->TypeName));
	}
	Row.Add(TEXT("defaults"), Defaults.Build());

	if (Node.Configuration.IsValid())
	{
		Row.Add(TEXT("configuration"), DescribeConfiguration(Node.Configuration, Class));
	}
	Row.AddString(TEXT("comment"), Node.Style.Display.Comment);
	Row.AddBool(TEXT("comment_visible"), Node.Style.Display.bCommentVisible);
	Row.AddBool(TEXT("hide_unconnected"), Node.Style.bUnconnectedPinsHidden);
	Row.AddBool(TEXT("derived"), Type == EMetasoundFrontendClassType::Input || Type == EMetasoundFrontendClassType::Variable);

	if (Context.bPins)
	{
		TArray<FValue> InputPins;
		for (const FMetasoundFrontendVertex& Vertex : Node.Interface.Inputs)
		{
			FObjectBuilder Pin;
			Pin.AddString(TEXT("name"), Vertex.Name.ToString());
			Pin.AddString(TEXT("type"), Vertex.TypeName.ToString());
			Pin.AddString(TEXT("access"), AccessName(Builder.GetNodeInputAccessType(Node.GetID(), Vertex.VertexID, &PageID)));
			if (const FMetasoundFrontendLiteral* ClassDefault = FindNodeInputClassDefault(Builder, Node.GetID(), Vertex.VertexID, &PageID))
			{
				Pin.Add(TEXT("default"), EncodeLiteral(*ClassDefault, Vertex.TypeName));
			}
			InputPins.Add(Pin.Build());
		}
		TArray<FValue> OutputPins;
		for (const FMetasoundFrontendVertex& Vertex : Node.Interface.Outputs)
		{
			FObjectBuilder Pin;
			Pin.AddString(TEXT("name"), Vertex.Name.ToString());
			Pin.AddString(TEXT("type"), Vertex.TypeName.ToString());
			Pin.AddString(TEXT("access"), AccessName(Builder.GetNodeOutputAccessType(Node.GetID(), Vertex.VertexID, &PageID)));
			OutputPins.Add(Pin.Build());
		}
		FObjectBuilder Pins;
		Pins.Add(TEXT("inputs"), MakeArray(MoveTemp(InputPins)));
		Pins.Add(TEXT("outputs"), MakeArray(MoveTemp(OutputPins)));
		Row.Add(TEXT("pins"), Pins.Build());
	}
	return Row.Build();
}

void AddPage(const FReadContext& Context, const FMetasoundFrontendGraph& Graph, TArray<FValue>& Records)
{
	const FString Page = GetPageName(Graph.PageID);
	for (const FMetasoundFrontendVariable& Variable : Graph.Variables)
	{
		FObjectBuilder Row;
		Row.AddString(TEXT("kind"), TEXT("variable"));
		Row.AddString(TEXT("page"), Page);
		Row.AddString(TEXT("name"), Variable.Name.ToString());
		Row.AddString(TEXT("type"), Variable.TypeName.ToString());
		Row.Add(TEXT("default"), EncodeLiteral(Variable.Literal, Variable.TypeName));
		Row.AddString(TEXT("display_name"), TextOf(Variable.DisplayName));
		Row.AddString(TEXT("description"), TextOf(Variable.Description));
		Row.AddString(TEXT("id"), GuidText(Variable.ID));
		Records.Add(Row.Build());
	}

	TMap<FGuid, EMetasoundFrontendClassType> Types;
	TMap<FGuid, const FMetasoundFrontendNode*> Nodes;
	TArray<const FMetasoundFrontendNode*> Sorted;
	for (const FMetasoundFrontendNode& Node : Graph.Nodes)
	{
		const FMetasoundFrontendClass* Class = Context.Builder.FindDependency(Node.ClassID);
		Types.Add(Node.GetID(), Class != nullptr ? Class->Metadata.GetType() : EMetasoundFrontendClassType::Invalid);
		Nodes.Add(Node.GetID(), &Node);
		Sorted.Add(&Node);
	}
	Sorted.Sort([](const FMetasoundFrontendNode& A, const FMetasoundFrontendNode& B) { return GuidText(A.GetID()) < GuidText(B.GetID()); });
	for (const FMetasoundFrontendNode* Node : Sorted)
	{
		Records.Add(MakeNode(Context, Graph, *Node));
	}

	struct FEdgeRow
	{
		FString From, FromPin, To, ToPin;
		bool bDerived = false;
	};
	TArray<FEdgeRow> Edges;
	for (const FMetasoundFrontendEdge& Edge : Graph.Edges)
	{
		const FMetasoundFrontendNode* const* From = Nodes.Find(Edge.FromNodeID);
		const FMetasoundFrontendNode* const* To = Nodes.Find(Edge.ToNodeID);
		if (From == nullptr || To == nullptr)
		{
			continue;
		}
		const FMetasoundFrontendVertex* FromVertex = (*From)->Interface.Outputs.FindByPredicate(
			[&Edge](const FMetasoundFrontendVertex& Vertex) { return Vertex.VertexID == Edge.FromVertexID; });
		const FMetasoundFrontendVertex* ToVertex = (*To)->Interface.Inputs.FindByPredicate(
			[&Edge](const FMetasoundFrontendVertex& Vertex) { return Vertex.VertexID == Edge.ToVertexID; });
		if (FromVertex == nullptr || ToVertex == nullptr)
		{
			continue;
		}
		const EMetasoundFrontendClassType FromType = Types.FindRef(Edge.FromNodeID);
		const EMetasoundFrontendClassType ToType = Types.FindRef(Edge.ToNodeID);
		const FMetasoundFrontendClass* ToClass = Context.Builder.FindDependency((*To)->ClassID);
		const bool bToInputTemplate = ToClass != nullptr && ToType == EMetasoundFrontendClassType::Template
			&& ToClass->Metadata.GetClassName() == Metasound::Frontend::FInputNodeTemplate::ClassName;
		FEdgeRow& Row = Edges.AddDefaulted_GetRef();
		Row.From = GuidText(Edge.FromNodeID);
		Row.FromPin = FromVertex->Name.ToString();
		Row.To = GuidText(Edge.ToNodeID);
		Row.ToPin = ToVertex->Name.ToString();
		// Derived: an input's edge into its template node, and the variable stack the builder wires between a
		// variable's nodes (into their hidden "Variable" pin; the editor shows only Get's Value and Set's Value).
		Row.bDerived = (FromType == EMetasoundFrontendClassType::Input && bToInputTemplate)
			|| FromType == EMetasoundFrontendClassType::Variable
			|| (IsVariableFamily(ToType) && ToType != EMetasoundFrontendClassType::Variable && ToVertex->Name == VariableStackPinName());
	}
	Edges.Sort([](const FEdgeRow& A, const FEdgeRow& B)
	{
		if (A.To != B.To) { return A.To < B.To; }
		if (A.ToPin != B.ToPin) { return A.ToPin < B.ToPin; }
		if (A.From != B.From) { return A.From < B.From; }
		return A.FromPin < B.FromPin;
	});
	for (const FEdgeRow& Edge : Edges)
	{
		FObjectBuilder Row;
		Row.AddString(TEXT("kind"), TEXT("edge"));
		Row.AddString(TEXT("page"), Page);
		Row.AddString(TEXT("from"), Edge.From);
		Row.AddString(TEXT("from_pin"), Edge.FromPin);
		Row.AddString(TEXT("to"), Edge.To);
		Row.AddString(TEXT("to_pin"), Edge.ToPin);
		Row.AddBool(TEXT("derived"), Edge.bDerived);
		Records.Add(Row.Build());
	}

	TArray<FGuid> CommentIds;
	Graph.Style.Comments.GetKeys(CommentIds);
	CommentIds.Sort([](const FGuid& A, const FGuid& B) { return GuidText(A) < GuidText(B); });
	for (const FGuid& Id : CommentIds)
	{
		const FMetaSoundFrontendGraphComment& Comment = Graph.Style.Comments.FindChecked(Id);
		FObjectBuilder Row;
		Row.AddString(TEXT("kind"), TEXT("comment"));
		Row.AddString(TEXT("page"), Page);
		Row.AddString(TEXT("id"), GuidText(Id));
		Row.AddString(TEXT("text"), Comment.Comment);
		Row.Add(TEXT("position"), MakePoint(Comment.Position.X, Comment.Position.Y));
		Row.Add(TEXT("size"), MakePoint(Comment.Size.X, Comment.Size.Y));
		FObjectBuilder Color;
		Color.Add(TEXT("R"), MakeNumber(ShortestFloat(Comment.Color.R)));
		Color.Add(TEXT("G"), MakeNumber(ShortestFloat(Comment.Color.G)));
		Color.Add(TEXT("B"), MakeNumber(ShortestFloat(Comment.Color.B)));
		Color.Add(TEXT("A"), MakeNumber(ShortestFloat(Comment.Color.A)));
		Row.Add(TEXT("color"), Color.Build());
		Row.AddInt(TEXT("font_size"), Comment.FontSize);
		Row.AddInt(TEXT("depth"), Comment.Depth);
		Row.AddBool(TEXT("color_bubble"), Comment.bColorBubble != 0);
		const UEnum* MoveModes = StaticEnum<EMetaSoundFrontendGraphCommentMoveMode>();
		Row.AddString(TEXT("move_mode"), MoveModes != nullptr ? MoveModes->GetNameStringByValue(static_cast<int64>(Comment.MoveMode)) : FString());
		Records.Add(Row.Build());
	}
}
} // namespace

bool HandleMetaSoundRead(FCall& Call, FObjectBuilder& Result)
{
	using namespace MetaSoundReadLocal;
	int64 Offset = 0;
	bool bPins = false;
	bool bValidate = false;
	if (!Call.Args.OnlyFields({TEXT("offset"), TEXT("pins"), TEXT("validate")}) || !Call.Args.Int(TEXT("offset"), Offset, false)
		|| !Call.Args.Bool(TEXT("pins"), bPins, false) || !Call.Args.Bool(TEXT("validate"), bValidate, false))
	{
		return false;
	}
	if (Offset < 0)
	{
		return Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("offset must be >= 0."), TEXT("offset"));
	}
	if (AsMetaSound(Call.Asset) == nullptr)
	{
		return Fail(Call, TEXT("MetaSound.UnsupportedAsset"), TEXT("MetaSound primitives read MetaSound Sources and Patches."), TEXT("/HandleArguments/asset"));
	}
	if (bValidate)
	{
		// The validation messages of the current state, without registering (the before-state of an apply).
		const TSharedPtr<FJsonObject> Status = DescribeCompileStatus(Call.Asset);
		if (!Status.IsValid())
		{
			return Fail(Call, TEXT("MetaSound.ReadFailed"), FString::Printf(TEXT("Could not validate %s."), *Call.Asset.GetPathName()));
		}
		Result.AddString(TEXT("status"), Status->GetStringField(TEXT("Status")));
		TArray<FValue> Messages;
		for (const TSharedPtr<FJsonValue>& Row : Status->GetArrayField(TEXT("Messages")))
		{
			const TSharedPtr<FJsonObject> Object = Row->AsObject();
			FObjectBuilder Message;
			Message.AddString(TEXT("severity"), Object->GetStringField(TEXT("Severity")));
			Message.AddString(TEXT("message"), Object->GetStringField(TEXT("Message")));
			Message.AddString(TEXT("node_id"), Object->GetStringField(TEXT("NodeGuid")));
			Message.AddString(TEXT("page"), Object->GetStringField(TEXT("Graph")));
			Message.AddString(TEXT("source"), Object->GetStringField(TEXT("Source")));
			Message.Add(TEXT("node_ids"), MakeStringArray(GetMessageNodeIds(*Object)));
			Messages.Add(Message.Build());
		}
		Result.Add(TEXT("messages"), MakeArray(MoveTemp(Messages)));
		return true;
	}
	const FNormalizedDocument Normalized(Call.Asset);
	if (!Normalized.IsValid())
	{
		return Fail(Call, TEXT("MetaSound.ReadFailed"), FString::Printf(TEXT("Could not read the MetaSound document of %s."), *Call.Asset.GetPathName()));
	}
	const FReadContext Context(Normalized, bPins);
	TArray<FValue> Records;
	Records.Add(MakeHeader(Context));
	AddMembers(Context, Records);
	for (const FGuid& PageID : GetOrderedPageIDs(Context.Document.RootGraph))
	{
		AddPage(Context, Context.Document.RootGraph.FindConstGraphChecked(PageID), Records);
	}

	TArray<FValue> Page;
	int32 NodeCount = 8;
	int32 Index = static_cast<int32>(FMath::Min<int64>(Offset, Records.Num()));
	for (; Index < Records.Num(); ++Index)
	{
		const int32 RowNodes = CountValueNodes(Records[Index]);
		if (!Page.IsEmpty() && NodeCount + RowNodes > QueryValueNodeBudget)
		{
			break;
		}
		NodeCount += RowNodes;
		Page.Add(MoveTemp(Records[Index]));
	}
	Result.AddInt(TEXT("total"), Records.Num());
	Result.AddInt(TEXT("next_offset"), Index < Records.Num() ? Index : -1);
	Result.Add(TEXT("records"), MakeArray(MoveTemp(Page)));
	return true;
}

TStrongObjectPtr<UObject> MetaSound::MakeEditorWidgetDefaults(const UClass& Class, bool bPreset)
{
	UObject* const ClassDefault = Class.GetDefaultObject();
	TStrongObjectPtr<UObject> Defaults(NewObject<UObject>(GetTransientPackage(), const_cast<UClass*>(&Class), NAME_None, RF_Transient, ClassDefault));
	// The editor module's types are private: found by name, as the editor's Initialize reads them.
	const UClass* FloatClass = FindObject<UClass>(nullptr, TEXT("/Script/MetasoundEditor.MetasoundEditorGraphMemberDefaultFloat"));
	const UClass* SettingsClass = FindObject<UClass>(nullptr, TEXT("/Script/MetasoundEditor.MetasoundEditorSettings"));
	if (bPreset || FloatClass == nullptr || SettingsClass == nullptr || !Class.IsChildOf(FloatClass))
	{
		return Defaults;
	}
	const FProperty* Setting = SettingsClass->FindPropertyByName(TEXT("DefaultInputWidgetType"));
	const FProperty* WidgetType = Class.FindPropertyByName(TEXT("WidgetType"));
	if (Setting != nullptr && WidgetType != nullptr && Setting->SameType(WidgetType))
	{
		WidgetType->CopyCompleteValue(WidgetType->ContainerPtrToValuePtr<void>(Defaults.Get()),
			Setting->ContainerPtrToValuePtr<void>(SettingsClass->GetDefaultObject()));
	}
	return Defaults;
}
} // namespace UE::AssetDocument::Graph::Audio
