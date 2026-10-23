// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// ai.state_tree.compile.v1 and the StateTree editor hooks:
//   * compile: the editor's Compile button (UStateTreeEditingSubsystem::CompileStateTree, which runs
//     ValidateStateTree first: editor data fix-ups, unused binding removal, linked state names and
//     parameters). The log's messages are returned with the state path and node of each message;
//     a failed compile leaves the tree reset (the editor's behavior), and the Python profile fails
//     the apply so Core rolls the package back.
//   * compile status: the Blueprint-style {Status, Messages} the Apply / Extract / Inspect results
//     carry (RegisterAssetDocumentCompileStatusDescriber); messages are those of the last compile run
//     here, while the editor data still hashes the same.
//   * rollback: Core restores the package bytes, which do not relink the compiled tree (UStateTree::
//     Link runs on load); the rebuilder recompiles the restored editor data when the tree was
//     compiled before (the editor's PIE / reinstancing path does the same). The compile output
//     outside the editor data is classified derived (a compile replaces it).

#include "StateTree/AssetDocumentStateTreePrimitives.h"

#include "AssetDocumentCompileStatus.h"
#include "Dom/JsonObject.h"
#include "Logging/TokenizedMessage.h"
#include "Runtime/AssetDocumentPackageSnapshot.h"
#include "StateTree.h"
#include "StateTreeCompilerLog.h"
#include "StateTreeEditingSubsystem.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "UObject/ObjectKey.h"

namespace UE::AssetDocument::Graph::StateTree
{
namespace
{
// The compiler log keeps its messages protected; a derived log reads them.
struct FReadableCompilerLog : public FStateTreeCompilerLog
{
	TConstArrayView<FStateTreeCompilerLogMessage> GetMessages() const
	{
		return Messages;
	}
};

struct FCompileMessage
{
	FString Severity;
	FString Message;
	FString StateId;
	FString StatePath;
	FString ItemId;
	FString ItemName;
};

struct FCompileRecord
{
	uint32 EditorHash = 0;
	bool bSucceeded = false;
	TArray<FCompileMessage> Messages;
};

TMap<FObjectKey, FCompileRecord>& GetCompileRecords()
{
	static TMap<FObjectKey, FCompileRecord> Records;
	return Records;
}

const TCHAR* SeverityName(int32 Severity)
{
	if (Severity <= EMessageSeverity::Error)
	{
		return TEXT("Error");
	}
	return Severity == EMessageSeverity::Info ? TEXT("Note") : TEXT("Warning");
}

const TCHAR* StatusName(const FCompileRecord& Record)
{
	if (!Record.bSucceeded)
	{
		return TEXT("Error");
	}
	return Record.Messages.ContainsByPredicate([](const FCompileMessage& Message) { return Message.Severity == TEXT("Warning"); })
		? TEXT("UpToDateWithWarnings") : TEXT("UpToDate");
}

FCompileRecord CompileTree(UStateTree& Tree)
{
	FReadableCompilerLog Log;
	FCompileRecord Record;
	Record.bSucceeded = UStateTreeEditingSubsystem::CompileStateTree(&Tree, Log);
	Record.EditorHash = UStateTreeEditingSubsystem::CalculateStateTreeHash(&Tree);
	for (const FStateTreeCompilerLogMessage& Message : Log.GetMessages())
	{
		FCompileMessage& Out = Record.Messages.AddDefaulted_GetRef();
		Out.Severity = SeverityName(Message.Severity);
		Out.Message = Message.Message;
		if (const UStateTreeState* State = Message.State.Get())
		{
			Out.StateId = GuidString(State->ID);
			Out.StatePath = State->GetPath();
		}
		Out.ItemId = GuidString(Message.Item.ID);
		Out.ItemName = Message.Item.Name.IsNone() ? FString() : Message.Item.Name.ToString();
		if (Out.StatePath.IsEmpty() && !Message.Item.StatePath.IsEmpty())
		{
			Out.StatePath = Message.Item.StatePath;
		}
	}
	GetCompileRecords().Add(FObjectKey(&Tree), Record);
	return Record;
}

TSharedPtr<FJsonObject> DescribeStateTreeCompileStatus(const UObject& Asset)
{
	const UStateTree* Tree = Cast<UStateTree>(&Asset);
	if (Tree == nullptr || Cast<UStateTreeEditorData>(Tree->EditorData) == nullptr)
	{
		return nullptr;
	}
	const uint32 EditorHash = UStateTreeEditingSubsystem::CalculateStateTreeHash(Tree);
	const FCompileRecord* Record = GetCompileRecords().Find(FObjectKey(Tree));
	if (Record != nullptr && Record->EditorHash != EditorHash)
	{
		Record = nullptr;
	}
	FString Status;
	if (Tree->LastCompiledEditorDataHash != 0 && Tree->LastCompiledEditorDataHash == EditorHash)
	{
		Status = Record != nullptr && Record->bSucceeded ? StatusName(*Record) : TEXT("UpToDate");
	}
	else
	{
		Status = Record != nullptr && !Record->bSucceeded ? TEXT("Error") : TEXT("Dirty");
	}
	TSharedPtr<FJsonObject> Compile = MakeShared<FJsonObject>();
	Compile->SetStringField(TEXT("Status"), Status);
	TArray<TSharedPtr<FJsonValue>> Messages;
	if (Record != nullptr)
	{
		for (const FCompileMessage& Message : Record->Messages)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("Severity"), Message.Severity);
			Row->SetStringField(TEXT("Message"), Message.Message);
			Row->SetStringField(TEXT("Graph"), Message.StatePath.IsEmpty() ? FString(TEXT("Tree")) : Message.StatePath);
			if (!Message.ItemName.IsEmpty())
			{
				Row->SetStringField(TEXT("Node"), Message.ItemName);
			}
			if (!Message.ItemId.IsEmpty())
			{
				Row->SetStringField(TEXT("NodeGuid"), Message.ItemId);
			}
			Messages.Add(MakeShared<FJsonValueObject>(Row));
		}
	}
	Compile->SetArrayField(TEXT("Messages"), Messages);
	return Compile;
}

class FStateTreeRebuilder final : public IAssetDocumentDerivedStateRebuilder
{
public:
	virtual FName GetId() const override { return TEXT("AssetDocumentAI.StateTree"); }
	virtual bool Supports(const UObject& PrimaryAsset) const override { return PrimaryAsset.IsA<UStateTree>(); }

	// The compile output the tree owns outside its editor data (the schema copy, extensions, node
	// instance objects) is derived: every compile renames the previous output to the transient package
	// (UE::StateTree::Compiler::RenameObjectToTransientPackage) and creates it again. The editor data
	// and everything in it is authored.
	virtual EAssetDocumentSnapshotObjectRole Classify(const UObject& Object) const override
	{
		const UStateTree* Tree = Object.GetTypedOuter<UStateTree>();
		const UObject* EditorData = Tree != nullptr ? Tree->EditorData.Get() : nullptr;
		if (EditorData != nullptr && &Object != EditorData && !Object.IsIn(EditorData))
		{
			return EAssetDocumentSnapshotObjectRole::Derived;
		}
		return IAssetDocumentDerivedStateRebuilder::Classify(Object);
	}

	virtual FAssetDocumentContractValidationResult Rebuild(UObject& PrimaryAsset) override
	{
		FAssetDocumentContractValidationResult Result;
		Result.bValid = true;
		UStateTree& Tree = *CastChecked<UStateTree>(&PrimaryAsset);
		// Not compiled before the apply (new, or a failed compile): nothing to relink.
		if (Tree.LastCompiledEditorDataHash == 0 || Cast<UStateTreeEditorData>(Tree.EditorData) == nullptr)
		{
			return Result;
		}
		const FCompileRecord Record = CompileTree(Tree);
		if (!Record.bSucceeded)
		{
			Result.Diagnostics.Add({TEXT("/PackageSnapshot"), TEXT("StateTree.RebuildCompileFailed"),
				FString::Printf(TEXT("%s compiled before the apply but not after the restore."), *Tree.GetPathName()),
				EAssetDocumentDiagnosticSeverity::Warning});
		}
		return Result;
	}
};

TSharedPtr<IAssetDocumentDerivedStateRebuilder> GRebuilder;
const FName DescriberId(TEXT("AssetDocumentAI.StateTree"));
} // namespace

bool HandleCompile(FCall& Call, FObjectBuilder& Result)
{
	UStateTree* Tree = nullptr;
	UStateTreeEditorData* EditorData = nullptr;
	if (!GetTree(Call, Tree, EditorData) || !Call.Args.OnlyFields({}))
	{
		return false;
	}
	const FCompileRecord Record = CompileTree(*Tree);
	TArray<FValue> Messages;
	for (const FCompileMessage& Message : Record.Messages)
	{
		Messages.Add(FObjectBuilder()
			.AddString(TEXT("severity"), Message.Severity)
			.AddString(TEXT("message"), Message.Message)
			.AddString(TEXT("state_id"), Message.StateId)
			.AddString(TEXT("state_path"), Message.StatePath)
			.AddString(TEXT("item_id"), Message.ItemId)
			.AddString(TEXT("item_name"), Message.ItemName)
			.Build());
	}
	Result.AddBool(TEXT("succeeded"), Record.bSucceeded);
	Result.AddString(TEXT("status"), StatusName(Record));
	Result.Add(TEXT("messages"), MakeArray(MoveTemp(Messages)));
	return true;
}
} // namespace UE::AssetDocument::Graph::StateTree

void RegisterAssetDocumentStateTreeEditorHooks()
{
	using namespace UE::AssetDocument::Graph::StateTree;
	if (!GRebuilder.IsValid())
	{
		GRebuilder = MakeShared<FStateTreeRebuilder>();
		RegisterAssetDocumentDerivedStateRebuilder(GRebuilder.ToSharedRef());
	}
	RegisterAssetDocumentCompileStatusDescriber(DescriberId, &DescribeStateTreeCompileStatus);
	RegisterAssetDocumentStateTreeDiscoveryHooks();
}

void UnregisterAssetDocumentStateTreeEditorHooks()
{
	using namespace UE::AssetDocument::Graph::StateTree;
	UnregisterAssetDocumentStateTreeDiscoveryHooks();
	UnregisterAssetDocumentCompileStatusDescriber(DescriberId);
	if (GRebuilder.IsValid())
	{
		UnregisterAssetDocumentDerivedStateRebuilder(GRebuilder->GetId());
		GRebuilder.Reset();
	}
	GetCompileRecords().Reset();
}
