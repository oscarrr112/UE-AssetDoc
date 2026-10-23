// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// SoundCue "compile" (design §5): the Sound Cue editor has no compile button; every structural edit ends with
// USoundCue::CompileSoundNodesFromGraphNodes. The finalize of the graph host adapter runs that compile once more and
// then validates what runtime code assumes about a cue but nothing checks:
//   errors    a graph node without its sound node in AllNodes (outer: the cue); input pins != ChildNodes or outside
//             the node's child limits (SetChildNodes silently ignores such arrays); per-child arrays whose size does
//             not follow the child slots (Random Weights, Mixer / Concatenator InputVolume, crossfade CrossFadeInput,
//             Group Control GroupSizes: indexed unchecked at playback); a cycle; a Delay without its child slot (its
//             GetDuration reads ChildNodes[0]); a wave player whose wave is not a SoundWave.
//   warnings  a single-input node (Modulator, Attenuation, Looping, Delay, ...) or a multi-input node with nothing
//             connected; a wave player without a wave; Random weights all zero; a silent cue (no FirstNode).
//   infos     nodes not reachable from the Output node (kept, as the editor shows dangling nodes).
// The same validation is the "Compile" status Extract / Apply / Inspect report (UpToDate, UpToDateWithWarnings, Error),
// and the rebuilder AssetDocumentAudio.SoundCue refreshes the derived state after Core restored a cue from its package
// snapshot (the editor's undo: PostEditUndo by Core, then CacheAggregateValues and NotifyGraphChanged).

#include "AssetDocumentSoundCue.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "Runtime/AssetDocumentPackageSnapshot.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "Sound/SoundNodeConcatenator.h"
#include "Sound/SoundNodeDelay.h"
#include "Sound/SoundNodeDistanceCrossFade.h"
#include "Sound/SoundNodeGroupControl.h"
#include "Sound/SoundNodeMixer.h"
#include "Sound/SoundNodeRandom.h"
#include "Sound/SoundNodeWavePlayer.h"
#include "Sound/SoundWave.h"
#include "SoundCueGraph/SoundCueGraphNode.h"
#include "SoundCueGraph/SoundCueGraphNode_Root.h"
#include "UObject/SoftObjectPtr.h"

namespace UE::AssetDocument::Graph::Audio::SoundCue
{
namespace
{
FString Label(const UEdGraphNode* Node)
{
	const FString Key = Node != nullptr ? NodeIdentity(*Node) : FString();
	return Key.IsEmpty() ? FString(TEXT("Output")) : FString::Printf(TEXT("'%s'"), *Key);
}

void Add(TArray<FValidationMessage>& Out, const TCHAR* Severity, const UEdGraphNode* Node, FString Message, FString Pin = FString())
{
	FValidationMessage& Entry = Out.AddDefaulted_GetRef();
	Entry.Severity = Severity;
	Entry.Message = MoveTemp(Message);
	Entry.Node = Node;
	Entry.Pin = MoveTemp(Pin);
}

void CheckChildArray(TArray<FValidationMessage>& Out, const UEdGraphNode* GraphNode, const TCHAR* Property, int32 Size, int32 Expected)
{
	if (Size != Expected)
	{
		Add(Out, TEXT("error"), GraphNode, FString::Printf(TEXT("%s: %s has %d entries but the node has %d child slot(s) (expected %d entries)."),
			*Label(GraphNode), Property, Size, Expected - (FCString::Strcmp(Property, TEXT("GroupSizes")) == 0 ? -1 : 0), Expected));
	}
}

FSoftObjectPath WaveOf(const USoundNodeWavePlayer& Player)
{
	const FSoftObjectProperty* Property = CastField<FSoftObjectProperty>(Player.GetClass()->FindPropertyByName(TEXT("SoundWaveAssetPtr")));
	return Property != nullptr ? Property->GetPropertyValue_InContainer(&Player).ToSoftObjectPath() : FSoftObjectPath();
}

// Depth-first search over ChildNodes; true when a node reaches itself.
bool HasCycle(USoundNode* Node, TSet<USoundNode*>& Done, TArray<USoundNode*>& Stack, TArray<USoundNode*>& OutCycle)
{
	if (Done.Contains(Node))
	{
		return false;
	}
	const int32 OnStack = Stack.Find(Node);
	if (OnStack != INDEX_NONE)
	{
		OutCycle = TArray<USoundNode*>(Stack.GetData() + OnStack, Stack.Num() - OnStack);
		return true;
	}
	Stack.Add(Node);
	for (USoundNode* Child : Node->ChildNodes)
	{
		if (Child != nullptr && HasCycle(Child, Done, Stack, OutCycle))
		{
			return true;
		}
	}
	Stack.Pop();
	Done.Add(Node);
	return false;
}

FString PathOfNode(const UEdGraphNode* Node)
{
	if (Node == nullptr)
	{
		return FString();
	}
	if (Node->IsA<USoundCueGraphNode_Root>())
	{
		return TEXT("/Body/Root");
	}
	const FString Key = NodeIdentity(*Node);
	if (Key.IsEmpty())
	{
		return FString();
	}
	const FString Escaped = Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
	return (Node->IsA<UEdGraphNode_Comment>() ? TEXT("/Body/Comments/") : TEXT("/Body/Nodes/")) + Escaped;
}

class FSoundCueRebuilder final : public IAssetDocumentDerivedStateRebuilder
{
public:
	virtual FName GetId() const override { return TEXT("AssetDocumentAudio.SoundCue"); }
	virtual bool Supports(const UObject& PrimaryAsset) const override { return PrimaryAsset.IsA<USoundCue>(); }
	virtual FAssetDocumentContractValidationResult Rebuild(UObject& PrimaryAsset) override
	{
		// The restored runtime tree (FirstNode, ChildNodes) is consistent with the restored graph already; the editor's
		// undo refreshes the cached aggregate values (PostEditUndo -> PostEditChange) and its graph panel.
		USoundCue& Cue = *CastChecked<USoundCue>(&PrimaryAsset);
		Cue.CacheAggregateValues();
		if (Cue.SoundCueGraph != nullptr)
		{
			Cue.SoundCueGraph->NotifyGraphChanged();
		}
		FAssetDocumentContractValidationResult Result;
		Result.bValid = true;
		return Result;
	}
};
} // namespace

void Validate(const USoundCue& Cue, TArray<FValidationMessage>& OutMessages)
{
	const UEdGraph* Graph = Cue.SoundCueGraph;
	if (Graph == nullptr)
	{
		Add(OutMessages, TEXT("error"), nullptr, TEXT("The cue has no Sound Cue graph (editor-only data was stripped)."));
		return;
	}
	const USoundCueGraphNode_Root* Root = nullptr;
	TArray<const USoundCueGraphNode*> SoundGraphNodes;
	for (const UEdGraphNode* Node : Graph->Nodes)
	{
		if (const USoundCueGraphNode_Root* AsRoot = Cast<USoundCueGraphNode_Root>(Node))
		{
			Root = AsRoot;
		}
		else if (const USoundCueGraphNode* AsSound = Cast<USoundCueGraphNode>(Node))
		{
			SoundGraphNodes.Add(AsSound);
		}
	}
	for (const USoundCueGraphNode* GraphNode : SoundGraphNodes)
	{
		USoundNode* SoundNode = GraphNode->SoundNode;
		if (SoundNode == nullptr || !Cue.AllNodes.Contains(SoundNode) || SoundNode->GetOuter() != &Cue)
		{
			Add(OutMessages, TEXT("error"), GraphNode, FString::Printf(TEXT("%s: the graph node's sound node is missing from the cue's AllNodes or is not owned by the cue."),
				*Label(GraphNode)));
			continue;
		}
		TArray<UEdGraphPin*> Inputs;
		const_cast<USoundCueGraphNode*>(GraphNode)->GetInputPins(Inputs);
		const int32 Children = SoundNode->ChildNodes.Num();
		const int32 Min = SoundNode->GetMinChildNodes();
		const int32 Max = SoundNode->GetMaxChildNodes();
		if (Inputs.Num() != Children)
		{
			Add(OutMessages, TEXT("error"), GraphNode, FString::Printf(TEXT("%s: %d input pin(s) but %d child slot(s) (allowed %d..%d inputs)."),
				*Label(GraphNode), Inputs.Num(), Children, Min, Max));
		}
		else if (Children < Min || Children > Max)
		{
			Add(OutMessages, TEXT("error"), GraphNode, FString::Printf(TEXT("%s: %d input(s), allowed %d..%d."), *Label(GraphNode), Children, Min, Max));
		}
		if (const USoundNodeRandom* Random = Cast<USoundNodeRandom>(SoundNode))
		{
			CheckChildArray(OutMessages, GraphNode, TEXT("Weights"), Random->Weights.Num(), Children);
			bool bAnyWeight = Random->Weights.IsEmpty();
			for (const float Weight : Random->Weights)
			{
				bAnyWeight |= Weight > 0.0f;
			}
			if (!bAnyWeight)
			{
				Add(OutMessages, TEXT("warning"), GraphNode, FString::Printf(TEXT("%s: every Random weight is zero; the node always picks input 0."), *Label(GraphNode)));
			}
		}
		else if (const USoundNodeMixer* Mixer = Cast<USoundNodeMixer>(SoundNode))
		{
			CheckChildArray(OutMessages, GraphNode, TEXT("InputVolume"), Mixer->InputVolume.Num(), Children);
		}
		else if (const USoundNodeConcatenator* Concatenator = Cast<USoundNodeConcatenator>(SoundNode))
		{
			CheckChildArray(OutMessages, GraphNode, TEXT("InputVolume"), Concatenator->InputVolume.Num(), Children);
		}
		else if (const USoundNodeDistanceCrossFade* CrossFade = Cast<USoundNodeDistanceCrossFade>(SoundNode))
		{
			CheckChildArray(OutMessages, GraphNode, TEXT("CrossFadeInput"), CrossFade->CrossFadeInput.Num(), Children);
		}
		else if (const USoundNodeGroupControl* Group = Cast<USoundNodeGroupControl>(SoundNode))
		{
			CheckChildArray(OutMessages, GraphNode, TEXT("GroupSizes"), Group->GroupSizes.Num(), FMath::Max(Children - 1, 0));
		}
		else if (SoundNode->IsA<USoundNodeDelay>() && Children == 0)
		{
			Add(OutMessages, TEXT("error"), GraphNode, FString::Printf(TEXT("%s: a Delay needs its input slot (its duration reads input 0)."), *Label(GraphNode)));
		}
		else if (const USoundNodeWavePlayer* Player = Cast<USoundNodeWavePlayer>(SoundNode))
		{
			const FSoftObjectPath Wave = WaveOf(*Player);
			if (Wave.IsNull())
			{
				Add(OutMessages, TEXT("warning"), GraphNode, FString::Printf(TEXT("%s: the wave player has no Sound Wave."), *Label(GraphNode)));
			}
			else if (Cast<USoundWave>(Wave.TryLoad()) == nullptr)
			{
				Add(OutMessages, TEXT("error"), GraphNode, FString::Printf(TEXT("%s: '%s' does not load as a Sound Wave asset."), *Label(GraphNode), *Wave.ToString()));
			}
		}
		int32 Connected = 0;
		for (const USoundNode* Child : SoundNode->ChildNodes)
		{
			Connected += Child != nullptr ? 1 : 0;
		}
		if (Max > 0 && Children > 0 && Connected == 0)
		{
			Add(OutMessages, TEXT("warning"), GraphNode, Max == 1
				? FString::Printf(TEXT("%s: its input is not connected; the node plays nothing."), *Label(GraphNode))
				: FString::Printf(TEXT("%s: none of its %d inputs is connected; the node plays nothing."), *Label(GraphNode), Children));
		}
	}
	// Cycles (the schema refuses a looping connection; a cue edited otherwise could still have one).
	{
		TSet<USoundNode*> Done;
		for (const USoundCueGraphNode* GraphNode : SoundGraphNodes)
		{
			TArray<USoundNode*> Stack;
			TArray<USoundNode*> Cycle;
			if (GraphNode->SoundNode != nullptr && HasCycle(GraphNode->SoundNode, Done, Stack, Cycle))
			{
				TArray<FString> Names;
				for (const USoundNode* Node : Cycle)
				{
					Names.Add(Node->GetName());
				}
				Add(OutMessages, TEXT("error"), GraphNode, FString::Printf(TEXT("Connection cycle: %s -> %s."), *FString::Join(Names, TEXT(" -> ")), *Names[0]));
				break;
			}
		}
	}
	// Reachability from the Output node (graph links).
	TSet<const UEdGraphNode*> Reachable;
	{
		TArray<const UEdGraphNode*> Pending;
		if (Root != nullptr)
		{
			Pending.Add(Root);
		}
		while (!Pending.IsEmpty())
		{
			const UEdGraphNode* Node = Pending.Pop();
			if (Reachable.Contains(Node))
			{
				continue;
			}
			Reachable.Add(Node);
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin != nullptr && Pin->Direction == EGPD_Input)
				{
					for (const UEdGraphPin* Linked : Pin->LinkedTo)
					{
						if (Linked != nullptr && Linked->GetOwningNodeUnchecked() != nullptr)
						{
							Pending.Add(Linked->GetOwningNodeUnchecked());
						}
					}
				}
			}
		}
	}
	for (const USoundCueGraphNode* GraphNode : SoundGraphNodes)
	{
		if (!Reachable.Contains(GraphNode))
		{
			Add(OutMessages, TEXT("info"), GraphNode, FString::Printf(TEXT("%s is not connected to the Output node (it does not play)."), *Label(GraphNode)));
		}
	}
	if (Cue.FirstNode == nullptr)
	{
		Add(OutMessages, TEXT("warning"), Root, TEXT("Nothing is connected to the Output node: the cue is silent."));
	}
}

TSharedPtr<FJsonObject> DescribeCompileStatus(const UObject& Asset)
{
	const USoundCue* Cue = Cast<USoundCue>(&Asset);
	if (Cue == nullptr || !IsEditableCue(Asset))
	{
		return nullptr;
	}
	TArray<FValidationMessage> Messages;
	Validate(*Cue, Messages);
	bool bErrors = false;
	bool bWarnings = false;
	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const FValidationMessage& Message : Messages)
	{
		bErrors |= Message.Severity == TEXT("error");
		bWarnings |= Message.Severity == TEXT("warning");
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Severity"), Message.Severity == TEXT("error") ? TEXT("Error") : Message.Severity == TEXT("warning") ? TEXT("Warning") : TEXT("Info"));
		Row->SetStringField(TEXT("Message"), Message.Message);
		const FString Key = Message.Node != nullptr ? NodeIdentity(*Message.Node) : FString();
		if (!Key.IsEmpty())
		{
			Row->SetStringField(TEXT("Node"), Key);
		}
		const FString Path = PathOfNode(Message.Node);
		if (!Path.IsEmpty())
		{
			Row->SetStringField(TEXT("Path"), Path);
		}
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	TSharedPtr<FJsonObject> Status = MakeShared<FJsonObject>();
	Status->SetStringField(TEXT("Status"), bErrors ? TEXT("Error") : bWarnings ? TEXT("UpToDateWithWarnings") : TEXT("UpToDate"));
	Status->SetArrayField(TEXT("Messages"), Rows);
	return Status;
}

TSharedRef<IAssetDocumentDerivedStateRebuilder> MakeRebuilder()
{
	return MakeShared<FSoundCueRebuilder>();
}
} // namespace UE::AssetDocument::Graph::Audio::SoundCue
