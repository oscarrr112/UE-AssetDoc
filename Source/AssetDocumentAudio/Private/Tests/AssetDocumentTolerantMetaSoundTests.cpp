// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// A MetaSound Source whose second page ("Low", added with Details > Add Page Graph) has a node fed directly by an
// input's hidden input node, with no input template node (an edge the frontend builder makes, as MetaSounds saved
// before input template nodes have): it reads as "Input:<member>", its own extract re-applies as a no-op, another
// edit of that page applies and keeps the edge, and a new such source is still refused.

#include "AssetDocumentMetaSoundApplyTestSupport.h"
#include "AssetDocumentTolerantTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentTolerantMetaSoundTestsLocal
{
using namespace AssetDocumentMetaSoundApplyTests;

const FName LowPage(TEXT("Low"));
const FGuid LowPageID(0x9A3F1C20, 0x4B7E4D11, 0x8C2A6E55, 0x1D0F0001);
const TCHAR* const Low = TEXT("Low");

// The tolerant-test view of a MetaSound fixture (same asset, package and sidecar).
AssetDocumentTolerantTestSupport::FTarget View(const FTarget& Target)
{
	AssetDocumentTolerantTestSupport::FTarget Out(TEXT("Unused"));
	Out.AssetName = Target.AssetName;
	Out.PackageName = Target.PackageName;
	Out.ObjectPath = Target.ObjectPath;
	Out.PackageFile = Target.PackageFile;
	Out.SidecarPath = Target.SidecarPath;
	return Out;
}

FString PointerToken(const FString& Name)
{
	return Name.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
}

// An input template node on Page and a node input it feeds: the template's key and member, the consumer's key and pin.
bool FindTemplateConsumer(const TSharedPtr<FJsonObject>& Document, const FString& Page, FString& OutTemplate, FString& OutMember,
	FString& OutConsumer, FString& OutPin)
{
	const TSharedPtr<FJsonObject> Nodes = PageNodes(Document, Page);
	for (const auto& Node : Nodes->Values)
	{
		const FString NodeKey(*Node.Key);
		const TSharedPtr<FJsonObject> Entry = Node.Value->AsObject();
		if (!Entry.IsValid() || Entry->GetStringField(TEXT("Class")) != TEXT("Input"))
		{
			continue;
		}
		for (const auto& Other : Nodes->Values)
		{
			const TSharedPtr<FJsonObject>* Inputs = nullptr;
			if (!Other.Value->AsObject()->TryGetObjectField(TEXT("Inputs"), Inputs))
			{
				continue;
			}
			for (const auto& Pin : (*Inputs)->Values)
			{
				FString Source;
				if (Pin.Value->TryGetString(Source) && (Source == NodeKey || Source.StartsWith(NodeKey + TEXT("."))))
				{
					OutTemplate = NodeKey;
					OutMember = Entry->GetStringField(TEXT("Member"));
					OutConsumer = FString(*Other.Key);
					OutPin = FString(*Pin.Key);
					return true;
				}
			}
		}
	}
	return false;
}

FString SourceAt(const TSharedPtr<FJsonObject>& Document, const FString& Pointer)
{
	const TSharedPtr<FJsonValue> Value = Document.IsValid() ? AssetDocumentTolerantTestSupport::At(Document, Pointer) : nullptr;
	return Value.IsValid() && Value->Type == EJson::String ? Value->AsString() : FString();
}
} // namespace AssetDocumentTolerantMetaSoundTestsLocal

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantMetaSoundTest, "UEAssetDoc.Tolerant.MetaSound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantMetaSoundTest::RunTest(const FString& Parameters)
{
	namespace Tolerant = AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentTolerantMetaSoundTestsLocal;
	TArray<TPair<FName, FGuid>> PageList;
	PageList.Emplace(LowPage, LowPageID);
	const FScopedProjectPages Pages(PageList);
	if (GEditor == nullptr || GEditor->Trans == nullptr || !TestTrue(TEXT("project pages set"), Pages.IsValid()))
	{
		return false;
	}
	FFixtures Fixtures;
	const FTarget& Fixture = Fixtures.Add(TEXT("MSS_Tolerant"));
	UObject* Asset = Duplicate(*this, LoadQuiet(OneShot), Fixture);
	if (!TestNotNull(TEXT("duplicate"), Asset))
	{
		return false;
	}
	// Opened once (its input template nodes injected, as an apply does first), then Details > Add Page Graph "Low".
	const TSharedPtr<FJsonObject> Start = Extract(*this, Fixture.ObjectPath, TEXT("start"));
	if (!Start.IsValid())
	{
		return false;
	}
	Child(BodyOf(Start), TEXT("Metadata"))->SetStringField(TEXT("Description"), TEXT("tolerant"));
	if (!ApplyOk(*this, Fixture, ForTarget(Start, Fixture, TEXT("Update")), TEXT("start")))
	{
		return false;
	}
	EditorAddPage(*Asset, LowPage);
	const TSharedPtr<FJsonObject> Paged = Extract(*this, Fixture.ObjectPath, TEXT("paged"));
	FString TemplateKey;
	FString Member;
	FString Consumer;
	FString Pin;
	if (!Paged.IsValid() || !TestTrue(TEXT("Low has an input template node feeding a node"),
		FindTemplateConsumer(Paged, Low, TemplateKey, Member, Consumer, Pin)))
	{
		return false;
	}
	// On Low, the builder wires the consumer to the member's hidden input node and the template node goes.
	{
		FEditorSide Side(*Asset);
		FMetaSoundFrontendDocumentBuilder& Builder = Side.Builder;
		const FGuid ConsumerID = NodeIdOnPage(Paged, Low, Consumer);
		Builder.RemoveNode(NodeIdOnPage(Paged, Low, TemplateKey), &LowPageID);
		const FMetasoundFrontendNode* InputNode = Builder.FindGraphInputNode(FName(*Member), &LowPageID);
		const FMetasoundFrontendVertex* In = Builder.FindNodeInput(ConsumerID, FName(*Pin), &LowPageID);
		if (!TestTrue(TEXT("the hidden input node and the consumer's pin"), InputNode != nullptr && !InputNode->Interface.Outputs.IsEmpty()
			&& In != nullptr))
		{
			return false;
		}
		FMetasoundFrontendEdge Edge;
		Edge.FromNodeID = InputNode->GetID();
		Edge.FromVertexID = InputNode->Interface.Outputs.Last().VertexID;
		Edge.ToNodeID = ConsumerID;
		Edge.ToVertexID = In->VertexID;
		Builder.RemoveEdgeToNodeInput(Edge.ToNodeID, Edge.ToVertexID, &LowPageID);
		Builder.AddEdge(MoveTemp(Edge), &LowPageID);
		// The editor's focus back on Default.
		Builder.SetBuildPageID(Metasound::Frontend::DefaultPageID);
	}
	if (!Save(*this, Asset, TEXT("fixture")))
	{
		return false;
	}
	const FString EdgePointer = FString::Printf(TEXT("/Body/Graphs/Low/Nodes/%s/Inputs/%s"), *PointerToken(Consumer), *PointerToken(Pin));
	const FString Direct = TEXT("Input:") + Member;
	const Tolerant::FTarget Target = View(Fixture);

	const TSharedPtr<FJsonObject> Extracted = Tolerant::TestTolerantRoundTrip(*this, Target, Tolerant::FExpected(), TEXT("MetaSound"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("the edge reads as Input:<member>"), SourceAt(Extracted, EdgePointer), Direct);

	// Another edit of the page (the consumer moved) applies and keeps the edge.
	const FAssetDocumentResult Moved = Tolerant::ApplyEdited(*this, Target, [&Consumer](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Node = Child(PageNodes(Document, Low), Consumer);
		const TArray<TSharedPtr<FJsonValue>>* Position = nullptr;
		const double X = Node->TryGetArrayField(TEXT("Position"), Position) && Position->Num() == 2 ? (*Position)[0]->AsNumber() : 0.0;
		const double Y = Position != nullptr && Position->Num() == 2 ? (*Position)[1]->AsNumber() : 0.0;
		Node->SetArrayField(TEXT("Position"), AssetDocumentMetaSoundApplyTests::Point(X + 64.0, Y));
	}, TEXT("Move"));
	if (TestTrue(TEXT("another edit of the page applies"), Moved.IsSuccess()))
	{
		TestEqual(TEXT("the edge is kept"), SourceAt(Tolerant::ExtractResult(Target).Payload, EdgePointer), Direct);
	}

	// A new source of that form (on Default, where the pin is fed by the template node) is still refused.
	const FString DefaultPointer = FString::Printf(TEXT("/Body/Graphs/Default/Nodes/%s/Inputs/%s"), *PointerToken(Consumer), *PointerToken(Pin));
	if (TestFalse(TEXT("Default's consumer is fed by a node"), SourceAt(Extracted, DefaultPointer).IsEmpty()))
	{
		const FAssetDocumentResult Refused = Tolerant::ApplyEdited(*this, Target, [&Consumer, &Pin, &Direct](const TSharedPtr<FJsonObject>& Document)
		{
			Child(Child(PageNodes(Document, TEXT("Default")), Consumer), TEXT("Inputs"))->SetStringField(Pin, Direct);
		}, TEXT("New direct source"));
		Tolerant::TestRefused(*this, Refused, TEXT("MetaSound.InvalidSource"), DefaultPointer, TEXT("New direct source"));
	}
	return !HasAnyErrors();
}

#endif
