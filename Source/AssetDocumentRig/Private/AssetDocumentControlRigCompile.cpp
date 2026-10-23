// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// rig.controlrig.compile.v1 and the Control Rig rollback rebuilder (docs/architecture/asset-document-controlrig-design.md
// §5.1, §5.3). Compile is the editor's Compile button: auto VM recompile back on, the editor graphs of a rig made
// headlessly bound to the asset first (as opening the editor does: without them the compiler has no nodes to report
// on and the VM is built from an empty graph view), RecompileModularRig for modular rigs, the Blueprint compile, then
// the hierarchy / draw-container propagation to live instances. The runtime-settings propagation the editor also runs
// is not exported by ControlRigDeveloper and is skipped (instances read the class default's settings on their next
// init). Result {compile: <compile status>}.
// A function reference node whose function the store does not have (a local function removed, or another rig's that
// no longer exists) is not compiled: RigVM reports it ("Could not find graph function data") and later edits of the
// rig can assert in RigVM. Compile reports each as an Error message instead (ControlRig.DanglingFunctionReference
// text, the node's path) with Status Error, and leaves the rig uncompiled; Apply fails on it when the apply made it.
// The rebuilder runs after Core restored a package snapshot (rollback to a functionally equivalent state).
// The models and editor graphs are back as they were (bytes and object identities); derived state is rebuilt the way
// the editor does after an Undo, without a registry refresh (RebuildGraphFromModel would replace the restored
// collapse / function editor graphs with new objects):
//   - the RigVM client's controllers and action stack are editing helpers, not authored (Derived: the snapshot keeps
//     them alive and in place; removed as new objects, a controller made before the apply kept the removed action
//     stack, and its next undo-enabled edit asserted in URigVMController::GetActionStack); every live controller is
//     pointed at the client's action stack again;
//   - the generated class's function store (what function reference nodes resolve through: removing a library
//     function removes its entry; the restored library and reference nodes need it back) is restored from the
//     capture, and the restored reference nodes are registered with the RigVM build data again (removing them
//     unregistered them; the editor finds the references of a function there, e.g. to remove them with it);
//   - the editor graphs' node caches are cleared (they can name editor nodes the restore dropped);
//   - the variable-change baseline is re-synced (URigVMBlueprint keeps the last variable set in transient state; left
//     at the failed apply's set, the next change notification would replay its renames / retypes on the nodes);
//   - one compile (modular recompile, VM, generated class);
//   - function reference variable remaps the restore lost are set again: the reference nodes repopulate while the
//     restored objects get PostEditUndo, against the generated class of the failed apply, and drop mappings whose
//     types no longer match (URigVMController::RepopulatePinsOnNode); then a second compile;
//   - member variable defaults (on the class default object, which a compile copies forward from the current one)
//     are written back from the capture;
//   - the compile status must not be worse than before the apply.

#include "AssetDocumentControlRig.h"

#include "ControlRigBlueprintLegacy.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/RigVMEdGraph.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "RigVMCore/RigVMGraphFunctionHost.h"
#include "RigVMModel/Nodes/RigVMFunctionReferenceNode.h"
#include "RigVMModel/Nodes/RigVMLibraryNode.h"
#include "RigVMModel/RigVMBuildData.h"
#include "RigVMModel/RigVMClient.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMControllerActions.h"
#include "RigVMModel/RigVMFunctionLibrary.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMPin.h"
#include "Runtime/AssetDocumentPackageSnapshot.h"

namespace UE::AssetDocument::Graph::Rig::ControlRig
{
namespace
{
// An editor graph caches its editor node per model node name (URigVMEdGraph::ModelNodePathToEdNode, transient, not
// seen by the garbage collector). The editor keeps it in step through model notifications; a package snapshot restore
// puts the graph's Nodes back without them, so the cache can name editor nodes the restore dropped, which the next
// garbage collection frees, and the next notification of that node name reads freed memory. GraphChanged is the one
// notification that clears the cache; it would also remove the graph's nodes, so they are set aside around it.
void ForgetEditorNodes(UControlRigBlueprint& Blueprint)
{
	TArray<UEdGraph*> Graphs;
	Blueprint.GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		URigVMEdGraph* RigGraph = Cast<URigVMEdGraph>(Graph);
		URigVMGraph* Model = RigGraph != nullptr ? RigGraph->GetModel() : nullptr;
		if (Model == nullptr)
		{
			continue;
		}
		TArray<TObjectPtr<UEdGraphNode>> Kept = MoveTemp(RigGraph->Nodes);
		RigGraph->Nodes.Reset();
		const TGuardValue<bool> Listening(RigGraph->bSuspendModelNotifications, false);
		RigGraph->HandleModifiedEvent(ERigVMGraphNotifType::GraphChanged, Model, Model);
		RigGraph->Nodes = MoveTemp(Kept);
	}
}

// Editor graphs of a rig created or converted headlessly hold no nodes while the models do: bind them to the asset.
void BindEditorGraphs(UControlRigBlueprint& Blueprint)
{
	// Per editor graph: one whose model has nodes while it has none was never bound (a rig or a graph made headless
	// in this session; a loaded or editor-made graph is bound and follows its model's notifications).
	TArray<UEdGraph*> Graphs;
	Blueprint.GetAllGraphs(Graphs);
	bool bBound = false;
	for (UEdGraph* Graph : Graphs)
	{
		URigVMEdGraph* RigGraph = Cast<URigVMEdGraph>(Graph);
		if (RigGraph == nullptr || !RigGraph->Nodes.IsEmpty())
		{
			continue;
		}
		const URigVMGraph* Model = RigGraph->GetModel();
		if (Model == nullptr || Model->GetNodes().IsEmpty())
		{
			continue;
		}
		RigGraph->InitializeFromAsset(FRigVMAssetInterfacePtr(&Blueprint));
		bBound = true;
	}
	if (bBound)
	{
		ForgetEditorNodes(Blueprint);
		static_cast<URigVMBlueprint&>(Blueprint).OnRigVMRegistryChanged();
	}
}

struct FRigDerivedState
{
	EBlueprintStatus Status = BS_Unknown;
	// Member variable defaults (export text of the class default object's Blueprint-declared properties).
	TMap<FName, FString> Defaults;
	// Variable remaps per function reference node path.
	TMap<FString, TMap<FName, FName>> Remaps;
	// The generated class's function store.
	bool bHasFunctionStore = false;
	FRigVMGraphFunctionStore FunctionStore;
};

// Properties a Blueprint declares (member variables), not the native ones of the rig class.
template <typename FuncType>
void ForEachMemberVariable(UObject& Defaults, FuncType&& Func)
{
	for (TFieldIterator<FProperty> It(Defaults.GetClass()); It; ++It)
	{
		const UClass* Owner = It->GetOwnerClass();
		if (Owner != nullptr && !Owner->HasAnyClassFlags(CLASS_Native) && !It->HasAnyPropertyFlags(CPF_Transient))
		{
			Func(**It);
		}
	}
}

template <typename FuncType>
void ForEachFunctionReference(UControlRigBlueprint& Blueprint, FuncType&& Func)
{
	for (URigVMGraph* Model : static_cast<URigVMBlueprint&>(Blueprint).GetAllModels())
	{
		for (URigVMNode* Node : Model != nullptr ? Model->GetNodes() : TArray<URigVMNode*>())
		{
			if (URigVMFunctionReferenceNode* Reference = Cast<URigVMFunctionReferenceNode>(Node))
			{
				Func(*Model, *Reference);
			}
		}
	}
}

UObject* ClassDefaults(UControlRigBlueprint& Blueprint)
{
	return Blueprint.GeneratedClass != nullptr ? Blueprint.GeneratedClass->GetDefaultObject(false) : nullptr;
}

FRigVMGraphFunctionStore* RigFunctionStore(UControlRigBlueprint& Blueprint)
{
	IRigVMGraphFunctionHost* Host = Blueprint.GeneratedClass != nullptr ? Cast<IRigVMGraphFunctionHost>(Blueprint.GeneratedClass) : nullptr;
	return Host != nullptr ? Host->GetRigVMGraphFunctionStore() : nullptr;
}

// The RigVM client's action stack (FRigVMClient::ActionStack: transient, private; read through its property).
URigVMActionStack* ClientActionStack(UControlRigBlueprint& Blueprint, FRigVMClient& Client)
{
	const FStructProperty* ClientProperty = FindFProperty<FStructProperty>(Blueprint.GetClass(), TEXT("RigVMClient"));
	const FObjectProperty* StackProperty = ClientProperty != nullptr && ClientProperty->Struct != nullptr
		? FindFProperty<FObjectProperty>(ClientProperty->Struct, TEXT("ActionStack")) : nullptr;
	return StackProperty != nullptr ? Cast<URigVMActionStack>(StackProperty->GetObjectPropertyValue_InContainer(&Client)) : nullptr;
}

// Live controllers (a controller of a model the restore removed is stale: the client drops it on lookup) use the
// client's action stack; when that is gone, they are dropped, and the client makes them again (with a new action
// stack) when they are next needed.
void ResyncControllers(UControlRigBlueprint& Blueprint)
{
	URigVMBlueprint& RigVM = Blueprint;
	FRigVMClient* Client = RigVM.GetRigVMClient();
	if (Client == nullptr)
	{
		return;
	}
	URigVMActionStack* Stack = ClientActionStack(Blueprint, *Client);
	const bool bUsable = IsValid(Stack) && Stack->GetOuter() == &Blueprint;
	for (URigVMGraph* Model : RigVM.GetAllModels())
	{
		if (Model == nullptr || Client->GetController(Model) == nullptr)
		{
			continue;
		}
		if (bUsable)
		{
			Client->GetController(Model)->SetActionStack(Stack);
		}
		else
		{
			Client->RemoveController(Model);
		}
	}
}

// Function reference nodes whose function the store does not have, as compile messages ({Severity, Message, Graph,
// Node, Path}).
TArray<TSharedPtr<FJsonValue>> DanglingReferenceMessages(UControlRigBlueprint& Blueprint)
{
	TArray<TSharedPtr<FJsonValue>> Rows;
	ForEachFunctionReference(Blueprint, [&Blueprint, &Rows](URigVMGraph&, URigVMFunctionReferenceNode& Reference)
	{
		if (Reference.GetReferencedFunctionData(true) != nullptr)
		{
			return;
		}
		const FRigVMGraphFunctionHeader& Header = Reference.GetReferencedFunctionHeader();
		FString Graph;
		const FString Path = DocumentNodePath(Blueprint, Reference, Graph);
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Severity"), TEXT("Error"));
		Row->SetStringField(TEXT("Message"), FString::Printf(TEXT("ControlRig.DanglingFunctionReference: function node %s calls "
			"'%s' (%s), which no longer exists; remove the node or call an existing function (the rig is not compiled while "
			"it is there)."), *Reference.GetName(), *Header.Name.ToString(), *Header.LibraryPointer.GetLibraryNodePath()));
		Row->SetStringField(TEXT("Graph"), Graph);
		Row->SetStringField(TEXT("Node"), Reference.GetName());
		Row->SetStringField(TEXT("Path"), Path);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	});
	return Rows;
}

class FControlRigRebuilder final : public IAssetDocumentDerivedStateRebuilder
{
public:
	virtual FName GetId() const override { return TEXT("AssetDocumentRig.ControlRig"); }
	virtual bool Supports(const UObject& PrimaryAsset) const override { return PrimaryAsset.IsA<UControlRigBlueprint>(); }

	virtual EAssetDocumentSnapshotObjectRole Classify(const UObject& Object) const override
	{
		if (Object.IsA<URigVMController>() || Object.IsA<URigVMActionStack>())
		{
			return EAssetDocumentSnapshotObjectRole::Derived;
		}
		return IAssetDocumentDerivedStateRebuilder::Classify(Object);
	}

	virtual TSharedPtr<void> CaptureDerivedState(UObject& PrimaryAsset) const override
	{
		UControlRigBlueprint& Blueprint = *CastChecked<UControlRigBlueprint>(&PrimaryAsset);
		TSharedRef<FRigDerivedState> State = MakeShared<FRigDerivedState>();
		State->Status = Blueprint.Status;
		if (UObject* Defaults = ClassDefaults(Blueprint))
		{
			ForEachMemberVariable(*Defaults, [Defaults, &State](const FProperty& Property)
			{
				FString Text;
				Property.ExportText_InContainer(0, Text, Defaults, Defaults, Defaults, PPF_None);
				State->Defaults.Add(Property.GetFName(), MoveTemp(Text));
			});
		}
		ForEachFunctionReference(Blueprint, [&State](URigVMGraph&, URigVMFunctionReferenceNode& Reference)
		{
			State->Remaps.Add(Reference.GetNodePath(true), Reference.GetVariableMap());
		});
		if (const FRigVMGraphFunctionStore* Store = RigFunctionStore(Blueprint))
		{
			State->bHasFunctionStore = true;
			State->FunctionStore = *Store;
		}
		return State;
	}

	virtual FAssetDocumentContractValidationResult Rebuild(UObject& PrimaryAsset) override
	{
		return RebuildFromState(PrimaryAsset, nullptr);
	}

	virtual FAssetDocumentContractValidationResult RebuildFromState(UObject& PrimaryAsset, const TSharedPtr<void>& StatePtr) override
	{
		FAssetDocumentContractValidationResult Result;
		UControlRigBlueprint* Blueprint = Cast<UControlRigBlueprint>(&PrimaryAsset);
		if (Blueprint == nullptr)
		{
			Result.bValid = true;
			return Result;
		}
		const FRigDerivedState* State = static_cast<const FRigDerivedState*>(StatePtr.Get());
		URigVMBlueprint& RigVM = *Blueprint;
		RunPreVariableChange(RigVM);
		ForgetEditorNodes(*Blueprint);
		ResyncControllers(*Blueprint);
		if (FRigVMGraphFunctionStore* Store = RigFunctionStore(*Blueprint))
		{
			if (State != nullptr && State->bHasFunctionStore)
			{
				*Store = State->FunctionStore;
			}
			// A library function the store still lacks gets its entry as adding it to the library does
			// (FRigVMClient::HandleGraphModifiedEvent, NodeAdded); the compile fills in its compilation data.
			URigVMFunctionLibrary* Library = RigVM.GetLocalFunctionLibrary();
			IRigVMGraphFunctionHost* Host = Cast<IRigVMGraphFunctionHost>(Blueprint->GeneratedClass);
			for (URigVMLibraryNode* Function : Library != nullptr && Host != nullptr ? Library->GetFunctions() : TArray<URigVMLibraryNode*>())
			{
				if (!Store->ContainsFunction(Function->GetFunctionIdentifier()))
				{
					Store->AddFunction(Function->GetFunctionHeader(Host), Library->IsFunctionPublic(Function->GetFName()));
				}
			}
		}
		ForEachFunctionReference(*Blueprint, [](URigVMGraph&, URigVMFunctionReferenceNode& Reference)
		{
			URigVMBuildData::Get()->RegisterFunctionReference(Reference.GetReferencedFunctionHeader().LibraryPointer, &Reference);
		});
		Finalize(*Blueprint, false);
		if (State != nullptr)
		{
			bool bRemapped = false;
			ForEachFunctionReference(*Blueprint, [&RigVM, State, &bRemapped](URigVMGraph& Model, URigVMFunctionReferenceNode& Reference)
			{
				const TMap<FName, FName>* Captured = State->Remaps.Find(Reference.GetNodePath(true));
				if (Captured == nullptr)
				{
					return;
				}
				URigVMController* Controller = RigVM.GetOrCreateController(&Model);
				for (const TPair<FName, FName>& Pair : *Captured)
				{
					const FName* Current = Reference.GetVariableMap().Find(Pair.Key);
					if (Controller != nullptr && (Current == nullptr || *Current != Pair.Value))
					{
						bRemapped |= SetFunctionVariableMapping(*Controller, Reference, Pair.Key, Pair.Value, false);
					}
				}
			});
			if (bRemapped)
			{
				Finalize(*Blueprint, false);
			}
			if (UObject* Defaults = ClassDefaults(*Blueprint))
			{
				ForEachMemberVariable(*Defaults, [Defaults, State, &Result](const FProperty& Property)
				{
					const FString* Captured = State->Defaults.Find(Property.GetFName());
					FString Current;
					Property.ExportText_InContainer(0, Current, Defaults, Defaults, Defaults, PPF_None);
					if (Captured == nullptr || Current == *Captured)
					{
						return;
					}
					if (Property.ImportText_InContainer(**Captured, Defaults, Defaults, PPF_None) == nullptr)
					{
						FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
						Diagnostic.Path = TEXT("/PackageSnapshot");
						Diagnostic.Code = TEXT("ControlRig.DefaultsRestoreFailed");
						Diagnostic.Severity = EAssetDocumentDiagnosticSeverity::Warning;
						Diagnostic.Message = FString::Printf(TEXT("Could not restore the default of member variable '%s'."), *Property.GetName());
					}
				});
			}
		}
		const bool bWasError = State != nullptr && State->Status == BS_Error;
		if (Blueprint->Status == BS_Error && !bWasError)
		{
			FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
			Diagnostic.Path = TEXT("/PackageSnapshot");
			Diagnostic.Code = TEXT("ControlRig.RebuildFailed");
			Diagnostic.Message = FString::Printf(TEXT("The restored Control Rig '%s' does not compile."), *Blueprint->GetPathName());
			return Result;
		}
		Result.bValid = true;
		return Result;
	}
};
} // namespace

namespace ControlRigCompileLocal
{
// An error the RigVM compiler reported (IRigVMAssetInterface::HandleReportFromCompiler) and its model node.
struct FVMCompilerError
{
	TWeakObjectPtr<URigVMNode> Node;
	FString Message;
};

// KI-10: the Blueprint status of the compile is decided before the VM is compiled (OnPostCDOCompiled), and the VM
// compiler's reports went to the class default object (see Finalize): an error such as a function node with unmapped
// variables was in none of the places the compile status reads. Its reports are collected while the compile runs and
// returned as compile messages ({Severity, Message, Graph, Node, Path}), the message's "@@" naming the node as the
// editor's results log does.
TArray<TSharedPtr<FJsonValue>> VMCompilerErrorMessages(const UControlRigBlueprint& Blueprint, const TArray<FVMCompilerError>& Errors)
{
	TArray<TSharedPtr<FJsonValue>> Rows;
	TSet<FString> Seen;
	for (const FVMCompilerError& Error : Errors)
	{
		const URigVMNode* Node = Error.Node.Get();
		FString Graph;
		const FString Path = Node != nullptr ? DocumentNodePath(Blueprint, *Node, Graph) : FString();
		const FString Message = Error.Message.Replace(TEXT("@@"), Node != nullptr ? *Node->GetName() : TEXT("a node")).TrimStartAndEnd();
		if (Message.IsEmpty() || Seen.Contains(Path + TEXT("\n") + Message))
		{
			continue;
		}
		Seen.Add(Path + TEXT("\n") + Message);
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Severity"), TEXT("Error"));
		Row->SetStringField(TEXT("Message"), Message);
		if (Node != nullptr)
		{
			Row->SetStringField(TEXT("Graph"), Graph);
			Row->SetStringField(TEXT("Node"), Node->GetName());
			Row->SetStringField(TEXT("Path"), Path);
		}
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	return Rows;
}

// The compile status with the VM compiler's errors added (those not already there at their node) and Status Error.
void AddVMCompilerErrors(FJsonObject& Status, const TArray<TSharedPtr<FJsonValue>>& Errors)
{
	TArray<TSharedPtr<FJsonValue>> Messages;
	const TArray<TSharedPtr<FJsonValue>>* Existing = nullptr;
	if (Status.TryGetArrayField(TEXT("Messages"), Existing))
	{
		Messages = *Existing;
	}
	bool bAdded = false;
	for (const TSharedPtr<FJsonValue>& Error : Errors)
	{
		const TSharedPtr<FJsonObject> Row = Error->AsObject();
		FString Path;
		FString Message;
		Row->TryGetStringField(TEXT("Path"), Path);
		Row->TryGetStringField(TEXT("Message"), Message);
		const bool bKnown = Messages.ContainsByPredicate([&Path, &Message](const TSharedPtr<FJsonValue>& Value)
		{
			const TSharedPtr<FJsonObject> Other = Value.IsValid() ? Value->AsObject() : nullptr;
			FString Severity;
			FString OtherPath;
			FString OtherMessage;
			if (!Other.IsValid() || !Other->TryGetStringField(TEXT("Severity"), Severity) || Severity != TEXT("Error"))
			{
				return false;
			}
			Other->TryGetStringField(TEXT("Path"), OtherPath);
			Other->TryGetStringField(TEXT("Message"), OtherMessage);
			return Path.IsEmpty() ? OtherMessage == Message : OtherPath == Path;
		});
		if (!bKnown)
		{
			Messages.Add(Error);
			bAdded = true;
		}
	}
	if (bAdded)
	{
		Status.SetArrayField(TEXT("Messages"), Messages);
	}
	Status.SetStringField(TEXT("Status"), TEXT("Error"));
}
} // namespace ControlRigCompileLocal

TArray<TSharedPtr<FJsonValue>> Finalize(UControlRigBlueprint& Blueprint, bool bBindEditorGraphs)
{
	using namespace ControlRigCompileLocal;
	URigVMBlueprint& RigVM = Blueprint;
	IRigVMAssetInterface& Asset = RigVM;
	TArray<FVMCompilerError> Errors;
	bool bCollect = false;
	FRigVMReportDelegate& ReportDelegate = RigVM.GetVMCompileSettings().ASTSettings.ReportDelegate;
	// The VM compiler reports through the rig's compile settings. The rig's constructor binds their report delegate to
	// its own HandleReportFromCompiler, but the settings are then initialized from the class default object's, delegate
	// included: a rig's reports go to Default__ControlRigBlueprint (its log, status and report event), not to the rig.
	// For the compile, the reports go to the rig's HandleReportFromCompiler (its log, status and node annotation, as
	// the constructor intends), and its errors are collected.
	TGuardValue<FRigVMReportDelegate> Reports(ReportDelegate, FRigVMReportDelegate::CreateLambda(
		[&Errors, &bCollect, &Asset](EMessageSeverity::Type Severity, UObject* Subject, const FString& Message)
		{
			if (bCollect && Severity == EMessageSeverity::Error)
			{
				URigVMNode* Node = Cast<URigVMNode>(Subject);
				if (const URigVMPin* Pin = Node == nullptr ? Cast<URigVMPin>(Subject) : nullptr)
				{
					Node = Pin->GetNode();
				}
				Errors.Add({Node, Message});
			}
			Asset.HandleReportFromCompiler(Severity, Subject, Message);
		}));
	RigVM.SetAutoVMRecompile(true);
	if (bBindEditorGraphs)
	{
		BindEditorGraphs(Blueprint);
	}
	if (Blueprint.IsModularRig())
	{
		Blueprint.RecompileModularRig();
	}
	// The Blueprint compile compiles the VM again (the one the rig keeps): only its reports count.
	bCollect = true;
	FCompilerResultsLog Log;
	Log.bSilentMode = true;
	Log.bAnnotateMentionedNodes = false;
	FKismetEditorUtilities::CompileBlueprint(&Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Log);
	bCollect = false;
	Blueprint.PropagateHierarchyFromBPToInstances();
	Blueprint.PropagateDrawInstructionsFromBPToInstances();
	return VMCompilerErrorMessages(Blueprint, Errors);
}

bool HandleCompile(FCall& Call, FObjectBuilder& Result)
{
	if (!Call.Args.OnlyFields({}))
	{
		return false;
	}
	UControlRigBlueprint* Blueprint = Cast<UControlRigBlueprint>(&Call.Asset);
	if (Blueprint == nullptr)
	{
		return Fail(Call, TEXT("ControlRig.UnsupportedAsset"), TEXT("rig.controlrig.compile.v1 compiles Control Rig assets."),
			TEXT("/HandleArguments/asset"));
	}
	const TArray<TSharedPtr<FJsonValue>> Dangling = DanglingReferenceMessages(*Blueprint);
	TArray<TSharedPtr<FJsonValue>> CompilerErrors;
	if (Dangling.IsEmpty())
	{
		CompilerErrors = Finalize(*Blueprint);
	}
	const TSharedPtr<FJsonObject> Status = DescribeStatus(*Blueprint);
	if (Status.IsValid() && !Dangling.IsEmpty())
	{
		Status->SetStringField(TEXT("Status"), TEXT("Error"));
		Status->SetArrayField(TEXT("Messages"), Dangling);
	}
	else if (Status.IsValid() && !CompilerErrors.IsEmpty())
	{
		ControlRigCompileLocal::AddVMCompilerErrors(*Status, CompilerErrors);
	}
	Result.Add(TEXT("compile"), Status.IsValid() ? JsonToValue(MakeShared<FJsonValueObject>(Status)) : MakeNull());
	return true;
}

TSharedRef<IAssetDocumentDerivedStateRebuilder> MakeRebuilder()
{
	return MakeShared<FControlRigRebuilder>();
}
} // namespace UE::AssetDocument::Graph::Rig::ControlRig
