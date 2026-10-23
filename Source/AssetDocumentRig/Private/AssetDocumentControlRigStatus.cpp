// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Compile status of a Control Rig without compiling (design §5.2) and the Inspect facts of a Shape Library
// (design §3.8, generic route). The status is what the editor shows after its last compile (the VM is recompiled on
// load): UBlueprint::Status, the compiler's per-node messages on the rig EdGraph nodes (placed at the document path
// of their model node) and the compile log, plus the CDO VM's hash and instruction count as cheap fidelity facts.
// Nothing is compiled or initialized: the VM is read from the class default object's VM property as is.

#include "AssetDocumentControlRig.h"

#include "ControlRigBlueprintLegacy.h"
#include "ControlRigGizmoLibrary.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/RigVMEdGraph.h"
#include "EdGraph/RigVMEdGraphNode.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/EngineVersionComparison.h"
#include "RigVMCore/RigVM.h"
#include "RigVMHost.h"
#include "RigVMModel/Nodes/RigVMCollapseNode.h"
#include "RigVMModel/RigVMFunctionLibrary.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMNode.h"

namespace UE::AssetDocument::Graph::Rig::ControlRig
{
namespace
{
const TCHAR* StatusName(EBlueprintStatus Status)
{
	switch (Status)
	{
	case BS_Dirty: return TEXT("Dirty");
	case BS_Error: return TEXT("Error");
	case BS_UpToDate: return TEXT("UpToDate");
	case BS_BeingCreated: return TEXT("BeingCreated");
	case BS_UpToDateWithWarnings: return TEXT("UpToDateWithWarnings");
	default: return TEXT("Unknown");
	}
}

const TCHAR* SeverityName(int32 Severity)
{
	if (Severity <= EMessageSeverity::Error)
	{
		return TEXT("Error");
	}
	return Severity == EMessageSeverity::Info ? TEXT("Note") : TEXT("Warning");
}

FString EscapeKey(const FString& Key)
{
	return Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
}

// Document graph key of a model ("Rig" for the default model).
FString GraphKey(const UControlRigBlueprint& Blueprint, const URigVMGraph& Model)
{
	const FRigVMClient* Client = static_cast<const URigVMBlueprint&>(Blueprint).GetRigVMClient();
	FString Name = Model.GetName();
	if ((Client != nullptr && Client->GetDefaultModel() == &Model) || Name == TEXT("RigVMModel"))
	{
		return TEXT("Rig");
	}
	Name.RemoveFromStart(TEXT("RigVMModel "), ESearchCase::CaseSensitive);
	return Name;
}

// Document path of a model node: /Body/Graphs/<Graph>/Nodes/<Node>, /Body/Functions/<F>/Nodes/<Node>, collapsed
// graphs nest as .../Nodes/<Collapse>/Collapse/Nodes/<Node>. Sets OutGraph to the top-level graph or function.
FString NodePath(const UControlRigBlueprint& Blueprint, const URigVMNode& Node, FString& OutGraph)
{
	TArray<FString> Segments = {TEXT("Nodes"), EscapeKey(Node.GetName())};
	const URigVMGraph* Graph = Node.GetGraph();
	while (Graph != nullptr)
	{
		const URigVMCollapseNode* Owner = Cast<URigVMCollapseNode>(Graph->GetOuter());
		if (Owner == nullptr)
		{
			OutGraph = GraphKey(Blueprint, *Graph);
			Segments.Insert({TEXT("Graphs"), EscapeKey(OutGraph)}, 0);
			break;
		}
		if (Owner->GetGraph() != nullptr && Owner->GetGraph()->IsA<URigVMFunctionLibrary>())
		{
			OutGraph = Owner->GetName();
			Segments.Insert({TEXT("Functions"), EscapeKey(OutGraph)}, 0);
			break;
		}
		Segments.Insert({TEXT("Nodes"), EscapeKey(Owner->GetName()), TEXT("Collapse")}, 0);
		Graph = Owner->GetGraph();
	}
	return TEXT("/Body/") + FString::Join(Segments, TEXT("/"));
}

URigVM* GetClassVM(const UControlRigBlueprint& Blueprint)
{
	static const FObjectProperty* VMProperty = FindFProperty<FObjectProperty>(URigVMHost::StaticClass(), TEXT("VM"));
	const UObject* Defaults = Blueprint.GeneratedClass != nullptr ? Blueprint.GeneratedClass->GetDefaultObject(false) : nullptr;
	if (VMProperty == nullptr || Defaults == nullptr || !Defaults->IsA<URigVMHost>())
	{
		return nullptr;
	}
	return Cast<URigVM>(VMProperty->GetObjectPropertyValue_InContainer(Defaults));
}
} // namespace

FString DocumentNodePath(const UControlRigBlueprint& Blueprint, const URigVMNode& Node, FString& OutGraph)
{
	return NodePath(Blueprint, Node, OutGraph);
}

TSharedPtr<FJsonObject> DescribeStatus(const UObject& Asset)
{
	const UControlRigBlueprint* Blueprint = Cast<UControlRigBlueprint>(&Asset);
	if (Blueprint == nullptr)
	{
		return nullptr;
	}
	TSharedPtr<FJsonObject> Compile = MakeShared<FJsonObject>();
	Compile->SetStringField(TEXT("Status"), StatusName(Blueprint->Status));
	TArray<TSharedPtr<FJsonValue>> Messages;
	TSet<FString> Seen;
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (const UEdGraph* Graph : Graphs)
	{
		if (Graph == nullptr)
		{
			continue;
		}
		for (const UEdGraphNode* EdNode : Graph->Nodes)
		{
			if (EdNode == nullptr || !EdNode->bHasCompilerMessage || EdNode->ErrorMsg.IsEmpty())
			{
				continue;
			}
			TArray<FString> Lines;
			EdNode->ErrorMsg.ParseIntoArrayLines(Lines);
			TArray<FString> Unique;
			for (const FString& Line : Lines)
			{
				Unique.AddUnique(Line.TrimStartAndEnd());
			}
			const FString Message = FString::Join(Unique, TEXT("\n"));
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("Severity"), SeverityName(EdNode->ErrorType));
			Row->SetStringField(TEXT("Message"), Message);
			const URigVMEdGraphNode* RigNode = Cast<URigVMEdGraphNode>(EdNode);
			const URigVMNode* ModelNode = RigNode != nullptr ? RigNode->GetModelNode() : nullptr;
			if (ModelNode != nullptr)
			{
				FString GraphName;
				const FString Path = NodePath(*Blueprint, *ModelNode, GraphName);
				Row->SetStringField(TEXT("Graph"), GraphName);
				Row->SetStringField(TEXT("Node"), ModelNode->GetName());
				Row->SetStringField(TEXT("Path"), Path);
			}
			else if (RigNode != nullptr && !RigNode->GetModelNodePath().IsEmpty())
			{
				Row->SetStringField(TEXT("Node"), RigNode->GetModelNodePath());
			}
			Seen.Add(Message);
			Messages.Add(MakeShared<FJsonValueObject>(Row));
		}
	}
	for (const TSharedRef<FTokenizedMessage>& Logged : Blueprint->GetCompileLog().Messages)
	{
		const EMessageSeverity::Type Severity = Logged->GetSeverity();
		if (Severity >= EMessageSeverity::Info)
		{
			continue;
		}
		const FString Message = Logged->ToText().ToString().TrimStartAndEnd();
		if (Message.IsEmpty() || Seen.Contains(Message))
		{
			continue;
		}
		Seen.Add(Message);
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Severity"), SeverityName(Severity));
		Row->SetStringField(TEXT("Message"), Message);
		Messages.Add(MakeShared<FJsonValueObject>(Row));
	}
	Compile->SetArrayField(TEXT("Messages"), Messages);
	if (const URigVM* VM = GetClassVM(*Blueprint))
	{
		Compile->SetNumberField(TEXT("VMHash"), static_cast<double>(VM->ComputeVMHash()));
		Compile->SetNumberField(TEXT("Instructions"), VM->GetByteCode().GetNumInstructions());
	}
	return Compile;
}

TSharedPtr<FJsonObject> DescribeShapeLibrary(const UObject& Asset)
{
	const UControlRigShapeLibrary* Library = Cast<UControlRigShapeLibrary>(&Asset);
	if (Library == nullptr)
	{
		return nullptr;
	}
	auto Describe = [](const FControlRigShapeDefinition& Shape, const FString& Name)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Name"), Name);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
		// Newer shapes reference a static or skeletal mesh.
		Row->SetStringField(TEXT("Mesh"), Shape.ShapeProxy.ToSoftObjectPath().ToString());
#else
		Row->SetStringField(TEXT("Mesh"), Shape.StaticMesh.ToSoftObjectPath().ToString());
#endif
		if (!Shape.Transform.Equals(FTransform::Identity, 0.0))
		{
			const FVector Translation = Shape.Transform.GetTranslation();
			const FRotator Rotation = Shape.Transform.Rotator();
			const FVector Scale = Shape.Transform.GetScale3D();
			TSharedPtr<FJsonObject> Transform = MakeShared<FJsonObject>();
			Transform->SetStringField(TEXT("Translation"), Translation.ToString());
			Transform->SetStringField(TEXT("Rotation"), Rotation.ToString());
			Transform->SetStringField(TEXT("Scale3D"), Scale.ToString());
			Row->SetObjectField(TEXT("Transform"), Transform);
		}
		return MakeShared<FJsonValueObject>(Row);
	};
	TSharedPtr<FJsonObject> Facts = MakeShared<FJsonObject>();
	Facts->SetObjectField(TEXT("DefaultShape"), Describe(Library->DefaultShape, Library->DefaultShape.ShapeName.ToString())->AsObject());
	TArray<TSharedPtr<FJsonValue>> Shapes;
	const TMap<FString, FString> NoNameMap;
	for (const FControlRigShapeDefinition& Shape : Library->Shapes)
	{
		Shapes.Add(Describe(Shape, UControlRigShapeLibrary::GetShapeName(Library, false, NoNameMap, Shape)));
	}
	Facts->SetArrayField(TEXT("Shapes"), Shapes);
	return Facts;
}
} // namespace UE::AssetDocument::Graph::Rig::ControlRig
