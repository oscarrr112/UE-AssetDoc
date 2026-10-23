// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Provider "fx": descriptors, bindings and the paged read handler. fx.niagara.read.v1 reads;
// fx.niagara.edit.v1 and fx.niagara.compile.v1 author systems at system level (emitters, emitter
// properties, renderers, user parameters; AssetDocumentNiagaraEdit.cpp, AssetDocumentNiagaraCompile.cpp).

#include "AssetDocumentNiagaraPrimitives.h"

#include "AssetDocumentGraphProvider.h"
#include "AssetDocumentNiagaraProvider.h"

#include "NiagaraEmitter.h"
#include "NiagaraParameterCollection.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "Utils/AssetDocumentEngineCompat.h"

namespace UE::AssetDocument::Niagara
{
namespace
{
constexpr uint32 NiagaraProviderVersion = 1;
constexpr const TCHAR* ReadPrimitiveId = TEXT("fx.niagara.read.v1");
constexpr const TCHAR* EditPrimitiveId = TEXT("fx.niagara.edit.v1");
constexpr const TCHAR* CompilePrimitiveId = TEXT("fx.niagara.compile.v1");

FAssetDocumentPrimitiveDescriptor MakeDescriptor(const TCHAR* PrimitiveId, bool bMutating, const TCHAR* AuthoritativeApi)
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = PrimitiveId;
	Descriptor.Version = NiagaraProviderVersion;
	Descriptor.ProviderId = ProviderId;
	Descriptor.Owner = EAssetDocumentPrimitiveOwner::FX;
	Descriptor.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
	Descriptor.Access = bMutating ? EAssetDocumentPrimitiveAccess::Mutating : EAssetDocumentPrimitiveAccess::ReadOnly;
	if (bMutating)
	{
		Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair};
		Descriptor.MutationFootprintSchemaId = TEXT("graph.footprint.whole-package.v1");
	}
	else
	{
		Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Validate, EAssetDocumentRuntimeRequestPhase::Preflight,
			EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair,
			EAssetDocumentRuntimeRequestPhase::Verify};
	}
	Descriptor.bRequiresActiveTransaction = bMutating;
	Descriptor.InputSchemaId = Descriptor.PrimitiveId + TEXT(".input.v1");
	Descriptor.OutputSchemaId = Descriptor.PrimitiveId + TEXT(".output.v1");
	Descriptor.StableDiagnosticCodes = {
		TEXT("AD.RUNTIME.PRIMITIVE_FAILED"), TEXT("Graph.UnknownField"), TEXT("Graph.InvalidArgument"),
		TEXT("Graph.UnsupportedSchemaVersion"), TEXT("Graph.PieActive"), TEXT("Graph.ResultTooLarge"),
		TEXT("Niagara.AddEmitterFailed"), TEXT("Niagara.AddModuleFailed"), TEXT("Niagara.AddRendererFailed"), TEXT("Niagara.AddStageFailed"),
		TEXT("Niagara.DynamicInputNotFound"), TEXT("Niagara.DynamicInputType"), TEXT("Niagara.EmitterAssetNotFound"),
		TEXT("Niagara.EmitterExists"), TEXT("Niagara.EmitterNotFound"), TEXT("Niagara.EmitterVersionNotFound"),
		TEXT("Niagara.EventHandlerNotDeletable"), TEXT("Niagara.EventHandlerNotFound"), TEXT("Niagara.InheritedEventHandler"),
		TEXT("Niagara.InheritedModule"), TEXT("Niagara.InheritedRenderer"), TEXT("Niagara.InheritedStage"), TEXT("Niagara.InputIsDefault"),
		TEXT("Niagara.InputNotFound"),
		TEXT("Niagara.InteractiveEditRefused"), TEXT("Niagara.InteractiveReadRefused"), TEXT("Niagara.InvalidAssignmentTarget"),
		TEXT("Niagara.InvalidCategory"), TEXT("Niagara.InvalidDisplayName"), TEXT("Niagara.InvalidEmitterName"),
		TEXT("Niagara.InvalidInputValue"), TEXT("Niagara.InvalidModuleName"), TEXT("Niagara.InvalidParameterName"),
		TEXT("Niagara.InvalidPropertyValue"), TEXT("Niagara.InvalidRendererName"), TEXT("Niagara.InvalidStageName"),
		TEXT("Niagara.InvalidUserParameterName"), TEXT("Niagara.InvalidUserParameterValue"), TEXT("Niagara.LinkTypeMismatch"),
		TEXT("Niagara.ModeChange"),
		TEXT("Niagara.ModuleExists"), TEXT("Niagara.ModuleNotDeletable"), TEXT("Niagara.ModuleNotFound"), TEXT("Niagara.ModuleNotMovable"),
		TEXT("Niagara.ModuleScriptNotFound"), TEXT("Niagara.ModuleUsage"), TEXT("Niagara.MoveModuleFailed"), TEXT("Niagara.ObjectNotFound"),
		TEXT("Niagara.ParameterExists"), TEXT("Niagara.ParameterNotFound"), TEXT("Niagara.ParentNotVersioned"),
		TEXT("Niagara.ParentVersionFailed"), TEXT("Niagara.RendererNameTaken"), TEXT("Niagara.RendererNotDeletable"),
		TEXT("Niagara.RendererNotFound"), TEXT("Niagara.RendererOrder"), TEXT("Niagara.ScriptVersionNotFound"),
		TEXT("Niagara.AddScratchPadFailed"), TEXT("Niagara.DependentHasUnsavedChanges"), TEXT("Niagara.InvalidPinName"),
		TEXT("Niagara.NodeNotAllowed"), TEXT("Niagara.ParameterNotReferenced"), TEXT("Niagara.PinNotEditable"), TEXT("Niagara.PinTypeRequired"),
		TEXT("Niagara.ScratchPadName"), TEXT("Niagara.ScratchPadNotFound"), TEXT("Niagara.ScratchPadScope"), TEXT("Niagara.UnknownVersion"),
		TEXT("Niagara.UsageImmutable"), TEXT("Niagara.VersionNotDeletable"), TEXT("Niagara.VersionNumber"),
		TEXT("Niagara.StageExists"), TEXT("Niagara.StageNotDeletable"), TEXT("Niagara.StageNotFound"), TEXT("Niagara.StageNotMovable"),
		TEXT("Niagara.StatelessEmitter"), TEXT("Niagara.StatelessModuleNotFound"), TEXT("Niagara.StatelessReflection"),
		TEXT("Niagara.UnknownDataInterfaceClass"), TEXT("Niagara.UnknownParameterType"), TEXT("Niagara.UnknownProperty"),
		TEXT("Niagara.UnknownRendererClass"), TEXT("Niagara.UnknownStackGroup"), TEXT("Niagara.UnknownStageClass"),
		TEXT("Niagara.UnknownUserParameterType"), TEXT("Niagara.UnsupportedAsset"), TEXT("Niagara.UnsupportedInputValue"),
		TEXT("Niagara.UserParameterExists"), TEXT("Niagara.UserParameterHierarchyUnavailable"), TEXT("Niagara.UserParameterNotFound"),
		TEXT("Niagara.ViewModelUnavailable"), TEXT("Niagara.NotInheritable"),
		TEXT("Niagara.ParentImmutable"), TEXT("Niagara.CollectionParameterOwned"), TEXT("Niagara.InvalidCollectionValue")};
	Descriptor.RequiredConsumerIds = {TEXT("route.generic.v1")};
	Descriptor.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
	Descriptor.HandleArgumentSchemas.Add({TEXT("asset"), EAssetDocumentHandleKind::Asset,
		bMutating ? EAssetDocumentHandleAccess::Mutable : EAssetDocumentHandleAccess::ReadOnly, true});

	FAssetDocumentPrimitiveAdmissionEvidence& Evidence = Descriptor.Admission;
	Evidence.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
	Evidence.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper;
	Evidence.ExactEngineVersion = TEXT("5.7.4");
	Evidence.PhysicalOwner = EAssetDocumentPrimitiveOwner::FX;
	Evidence.PublicModule = TEXT("AssetDocumentNiagara");
	Evidence.PublicHeader = TEXT("AssetDocumentNiagaraProvider.h");
	Evidence.ExportedAuthoritativeApi = AuthoritativeApi;
	Evidence.AuthorityContextId = TEXT("assetdocument.editor.game-thread.editor-ready.v1");
	Evidence.PrimaryOperationId = Descriptor.PrimitiveId;
	Evidence.IdentityLifetimeContractId = TEXT("identity.request-transaction-revision-bound.v1");
	Evidence.TransactionContractId = TEXT("transaction.descriptor-governed.v1");
	Evidence.RollbackContractId = TEXT("rollback.core-package-snapshot.v1");
	Evidence.CompletionContractId = Descriptor.PrimitiveId + TEXT(".completion.v1");
	Evidence.SaveReloadEvidenceId = TEXT("UEAssetDoc.Niagara");
	Evidence.FailureInjectionTestIds = {TEXT("UEAssetDoc.Niagara")};
	Evidence.VerificationTestIds = {TEXT("UEAssetDoc.Niagara")};
	Evidence.AuthorityFingerprint = FString::Printf(TEXT("ue-5.7.4|fx|%s|%s"), PrimitiveId, AuthoritativeApi);
	return Descriptor;
}

TArray<FAssetDocumentPrimitiveDescriptor> BuildDescriptors()
{
	return {
		MakeDescriptor(ReadPrimitiveId, false,
			TEXT("FNiagaraSystemViewModel::Initialize;FNiagaraSystemViewModel::GetSystemStackViewModel;")
			TEXT("FNiagaraEmitterHandleViewModel::GetEmitterStackViewModel;UNiagaraStackEntry::GetUnfilteredChildren;")
			TEXT("UNiagaraStackModuleItem::GetParameterInputs;UNiagaraStackFunctionInput::GetValueMode;")
			TEXT("UNiagaraStackFunctionInput::CanReset;UNiagaraScript::GetLatestSource;UNiagaraGraph::GetAllMetaData")),
		MakeDescriptor(EditPrimitiveId, true,
			TEXT("FNiagaraSystemViewModel::AddEmitter;FNiagaraSystemViewModel::AddEmptyEmitter;FNiagaraSystemViewModel::AddMinimalEmitter;")
			TEXT("FNiagaraSystemViewModel::DeleteEmitters;FNiagaraEmitterHandleViewModel::SetName;FNiagaraEmitterHandleViewModel::SetIsEnabled;")
			TEXT("FNiagaraEmitterViewModel::RemoveParentEmitter;UNiagaraEmitter::PostEditChangeVersionedProperty;")
			TEXT("INiagaraStackItemGroupAddUtilities::ExecuteAddAction;UNiagaraStackRendererItem::Delete;UNiagaraStackRendererItem::MoveRendererUp;")
			TEXT("UNiagaraStackItem::SetIsEnabled;FNiagaraEditorUtilities::ResetVariableToDefaultValue;FNiagaraParameterStore::AddParameter;")
			TEXT("FNiagaraParameterStore::RemoveParameter;FNiagaraParameterStore::RenameParameter;FNiagaraParameterStore::SetParameterData;")
			TEXT("UNiagaraSystem::HandleVariableRenamed;UNiagaraSystem::HandleVariableRemoved;UNiagaraGraph::RenameParameter;")
			TEXT("UNiagaraSystemEditorData::RenameUserScriptVariable;UHierarchyElement::CopyAndAddItemUnderParentIdentity;")
			TEXT("FNiagaraSystemViewModel::AddStatelessEmitter;FNiagaraEditorUtilities::SwitchParentEmitterVersion;UNiagaraScriptVariable::Init;")
			TEXT("FNiagaraStackGraphUtilities::AddScriptModuleToStack;FNiagaraStackGraphUtilities::AddParameterModuleToStack;")
			TEXT("UNiagaraStackModuleItem::AddInput;UNiagaraStackModuleItem::Delete;UNiagaraStackModuleItem::ChangeScriptVersion;UNiagaraStackEntry::Drop;")
			TEXT("UNiagaraStackEntry::OnRenamed;UNiagaraStackEntry::SetStackNoteData;UNiagaraStackFunctionInput::SetLocalValue;")
			TEXT("UNiagaraStackFunctionInput::SetLinkedParameterValue;UNiagaraStackFunctionInput::SetDynamicInput;")
			TEXT("UNiagaraStackFunctionInput::SetCustomExpression;UNiagaraStackFunctionInput::SetDataInterfaceValue;")
			TEXT("UNiagaraStackFunctionInput::SetObjectAssetValue;UNiagaraStackFunctionInput::Reset;UNiagaraStackFunctionInput::DeleteInput;")
			TEXT("UNiagaraStackEventScriptItemGroup::Delete;UNiagaraStackSimulationStageGroup::Delete;")
			TEXT("UNiagaraSystemFactoryNew::InitializeSystem;UNiagaraSystem::DuplicateEmitterHandle;UNiagaraEmitter::CreateWithParentAndOwner;")
			TEXT("UNiagaraEmitter::MergeChangesFromParent;UNiagaraScript::ClearResolvedData;FNiagaraVersionedObject::AddNewVersion;")
			TEXT("UNiagaraParameterCollection::AddParameter;UNiagaraParameterCollection::RemoveParameter;")
			TEXT("UNiagaraParameterCollectionInstance::RenameParameter;UNiagaraParameterCollectionInstance::SyncWithCollection")),
		MakeDescriptor(CompilePrimitiveId, true,
			TEXT("UNiagaraSystem::RequestCompile;UNiagaraSystem::WaitForCompilationComplete;UNiagaraScript::GetLastCompileStatus")),
	};
}

const Graph::FBinding Bindings[] = {
	{ReadPrimitiveId, &HandleRead},
	{EditPrimitiveId, &HandleEdit},
	{CompilePrimitiveId, &HandleCompile},
};

// Paging over the node budget: rows from Offset while the page stays under the budget (at least one row).
int32 AppendPaged(TArray<FValue>& Out, TConstArrayView<FValue> Rows, int32 Offset, int32 UsedNodes)
{
	int32 Nodes = UsedNodes;
	for (int32 Index = Offset; Index < Rows.Num(); ++Index)
	{
		const int32 RowNodes = Graph::CountValueNodes(Rows[Index]);
		if (!Out.IsEmpty() && Nodes + RowNodes > Graph::QueryValueNodeBudget)
		{
			return Index;
		}
		Out.Add(Rows[Index]);
		Nodes += RowNodes;
	}
	return -1;
}
} // namespace

TConstArrayView<FAssetDocumentPrimitiveDescriptor> GetDescriptors()
{
	static const TArray<FAssetDocumentPrimitiveDescriptor> Descriptors = BuildDescriptors();
	return Descriptors;
}

TConstArrayView<Graph::FBinding> GetBindings()
{
	return Bindings;
}

bool Fail(FCall& Call, FStringView Code, FStringView Message, FStringView Path)
{
	if (Call.Error.Code.IsEmpty())
	{
		Call.Error.Set(Code, Message, Path);
	}
	return false;
}

bool HandleRead(FCall& Call, FObjectBuilder& Result)
{
	FCall& C = Call;
	if (!C.Args.OnlyFields({TEXT("offset"), TEXT("detail"), TEXT("renderers"), TEXT("data_interfaces"), TEXT("stages"), TEXT("stateless_modules"),
			TEXT("node_defaults")}))
	{
		return false;
	}
	int64 Offset = 0;
	FString Detail = TEXT("document");
	bool bNodeDefaults = false;
	if (!C.Args.Int(TEXT("offset"), Offset, false) || !C.Args.String(TEXT("detail"), Detail, false)
		|| !C.Args.Bool(TEXT("node_defaults"), bNodeDefaults, false))
	{
		return false;
	}
	if (Offset < 0 || (Detail != TEXT("document") && Detail != TEXT("inspect") && Detail != TEXT("compile") && Detail != TEXT("defaults")))
	{
		return C.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT(
			"offset is >= 0 and detail is \"document\", \"inspect\", \"compile\" or \"defaults\"."), Offset < 0 ? TEXT("offset") : TEXT("detail"));
	}
	if (Detail == TEXT("defaults"))
	{
		// The values that the document's sparse Properties leave out: an emitter's (a new FVersionedNiagaraEmitterData)
		// and those of the given renderer / data interface classes (their class default objects), by class label.
		const FVersionedNiagaraEmitterData EmitterDefaults;
		Result.Add(TEXT("emitter"), SparseProperties(*FVersionedNiagaraEmitterData::StaticStruct(), &EmitterDefaults, nullptr));
		// A script's Settings (the script Details of its version data, as ReadScriptRows reads them).
		const FVersionedNiagaraScriptData ScriptDefaults;
		Result.Add(TEXT("script"), SparseProperties(*FVersionedNiagaraScriptData::StaticStruct(), &ScriptDefaults, nullptr,
			ScriptSettingsSkip()));
		const FNiagaraEventScriptProperties EventDefaults;
		Result.Add(TEXT("event_handler"), SparseProperties(*FNiagaraEventScriptProperties::StaticStruct(), &EventDefaults, nullptr,
			{TEXT("Script"), TEXT("SourceEmitterID")}));
		// A lightweight emitter's (UNiagaraStatelessEmitter is engine-internal: found by path).
		if (const UClass* Stateless = FindObject<UClass>(nullptr, TEXT("/Script/Niagara.NiagaraStatelessEmitter")))
		{
			Result.Add(TEXT("stateless"), SparseProperties(*Stateless, Stateless->GetDefaultObject(), nullptr,
				{TEXT("Modules"), TEXT("RendererProperties")}));
		}
		struct FClassGroup
		{
			const TCHAR* Field;
			const TCHAR* Prefix;
			const TCHAR* Suffix;
			TArray<FName> Skip;
		};
		const FClassGroup Groups[] = {
			{TEXT("renderers"), TEXT("Niagara"), TEXT("Properties"), {TEXT("bIsEnabled")}},
			{TEXT("data_interfaces"), TEXT("NiagaraDataInterface"), TEXT(""), {}},
			{TEXT("stages"), TEXT("NiagaraSimulationStage"), TEXT(""), {TEXT("SimulationStageName"), TEXT("bEnabled")}},
			{TEXT("stateless_modules"), TEXT("NiagaraStatelessModule_"), TEXT(""), {}}};
		for (const FClassGroup& Group : Groups)
		{
			FObjectBuilder ByClass;
			if (const FValue* Labels = C.Args.Find(Group.Field))
			{
				if (Labels->Kind != EAssetDocumentRuntimeValueKind::Array)
				{
					return C.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("A class list is an array of class labels."), Group.Field);
				}
				for (const FValue& Label : Labels->Elements)
				{
					if (Label.Kind != EAssetDocumentRuntimeValueKind::String)
					{
						continue;
					}
					const FString Path = Label.StringValue.StartsWith(TEXT("/")) ? Label.StringValue
						: FString::Printf(TEXT("/Script/Niagara.%s%s%s"), Group.Prefix, *Label.StringValue, Group.Suffix);
					const UClass* Class = FindObject<UClass>(nullptr, *Path);
					if (Class != nullptr && ClassLabel(Class, Group.Prefix, Group.Suffix) == Label.StringValue)
					{
						ByClass.Add(Label.StringValue, SparseProperties(*Class, Class->GetDefaultObject(), nullptr, Group.Skip));
					}
				}
			}
			Result.Add(Group.Field, ByClass.Build());
		}
		Result.Add(TEXT("rows"), Graph::MakeArray({}));
		Result.AddInt(TEXT("next_offset"), -1);
		Result.AddInt(TEXT("total"), 0);
		return true;
	}
	if (Detail == TEXT("compile"))
	{
		// The compile status alone (the errors an apply compares against).
		if (const UNiagaraScript* CompiledScript = Cast<UNiagaraScript>(&C.Asset))
		{
			Result.AddString(TEXT("class"), TEXT("script"));
			AddScriptCompileResult(*CompiledScript, Result);
			Result.Add(TEXT("rows"), Graph::MakeArray({}));
			Result.AddInt(TEXT("next_offset"), -1);
			Result.AddInt(TEXT("total"), 0);
			return true;
		}
		if (UNiagaraEmitter* CompiledEmitter = Cast<UNiagaraEmitter>(&C.Asset))
		{
			// An emitter asset compiles through a working copy (not written back).
			if (!ReadEmitterCompile(C, *CompiledEmitter, Result))
			{
				return false;
			}
			Result.Add(TEXT("rows"), Graph::MakeArray({}));
			Result.AddInt(TEXT("next_offset"), -1);
			Result.AddInt(TEXT("total"), 0);
			return true;
		}
		UNiagaraSystem* CompiledSystem = Cast<UNiagaraSystem>(&C.Asset);
		if (CompiledSystem == nullptr)
		{
			return Fail(C, TEXT("Niagara.UnsupportedAsset"), TEXT("Only Niagara systems, emitters and scripts have a compile status."));
		}
		// A compile still running (the load-time one) settles first: its result is the status before an apply.
		SettleCompilation(*CompiledSystem);
		TArray<FCompileMessage> Messages;
		const FString Status = ReadCompileStatus(*CompiledSystem, Messages);
		TArray<FString> Errors;
		TArray<FString> Warnings;
		for (const FCompileMessage& Message : Messages)
		{
			(Message.Severity == TEXT("Error") ? Errors : Warnings).AddUnique(Message.Message);
		}
		Result.AddString(TEXT("class"), TEXT("system"));
		Result.AddString(TEXT("status"), Status);
		Result.Add(TEXT("errors"), Graph::MakeStringArray(Errors));
		Result.Add(TEXT("warnings"), Graph::MakeStringArray(Warnings));
		Result.Add(TEXT("rows"), Graph::MakeArray({}));
		Result.AddInt(TEXT("next_offset"), -1);
		Result.AddInt(TEXT("total"), 0);
		return true;
	}
	FReadOptions Options;
	Options.bInspect = Detail == TEXT("inspect");
	Options.bNodeDefaults = bNodeDefaults;
	TArray<FValue> Rows;
	FString Kind;
	if (UNiagaraSystem* System = Cast<UNiagaraSystem>(&C.Asset))
	{
		Kind = TEXT("system");
		if (!ReadSystemRows(C, *System, Options, Rows))
		{
			return false;
		}
	}
	else if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(&C.Asset))
	{
		Kind = TEXT("emitter");
		if (!ReadEmitterAssetRows(C, *Emitter, Options, Rows))
		{
			return false;
		}
	}
	else if (const UNiagaraScript* Script = Cast<UNiagaraScript>(&C.Asset))
	{
		Kind = TEXT("script");
		ReadScriptRows(*Script, FString(), Options, Rows);
	}
	else if (const UNiagaraParameterCollection* Collection = Cast<UNiagaraParameterCollection>(&C.Asset))
	{
		Kind = TEXT("parameter_collection");
		ReadParameterCollectionRows(*Collection, Rows);
	}
	else
	{
		return Fail(C, TEXT("Niagara.UnsupportedAsset"), FString::Printf(
			TEXT("%s is not a Niagara system, emitter, script or parameter collection."), *C.Asset.GetClass()->GetPathName()));
	}
	// The asset's own Details-panel properties (sparse against the class defaults), on the first page.
	int32 UsedNodes = 0;
	if (Offset == 0)
	{
		TArray<FString> Unsupported;
		FValue Properties = SparseObjectProperties(C.Asset, {}, &Unsupported);
		UsedNodes += Graph::CountValueNodes(Properties);
		Result.Add(TEXT("properties"), MoveTemp(Properties));
		if (Options.bInspect)
		{
			Unsupported.Sort();
			Result.Add(TEXT("unsupported_properties"), Graph::MakeStringArray(Unsupported));
		}
	}
	TArray<FValue> Page;
	const int32 Next = Offset < Rows.Num() ? AppendPaged(Page, Rows, static_cast<int32>(Offset), UsedNodes) : -1;
	Result.AddString(TEXT("class"), Kind);
	Result.Add(TEXT("rows"), Graph::MakeArray(MoveTemp(Page)));
	Result.AddInt(TEXT("next_offset"), Next);
	Result.AddInt(TEXT("total"), Rows.Num());
	return true;
}

void SettleMergeCopies(UPackage& Package)
{
	TArray<UObject*> Objects;
	GetObjectsWithPackage(&Package, Objects, AssetDocumentObjects::IncludeNested, RF_Transient, EInternalObjectFlags::Garbage);
	for (UObject* Object : Objects)
	{
		UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Object);
		if (Emitter != nullptr && Emitter->GetOuter()->IsA<UNiagaraEmitter>())
		{
			Emitter->UpdateEmitterAfterLoad();
		}
	}
}
} // namespace UE::AssetDocument::Niagara

TSharedRef<IAssetDocumentPrimitiveProvider> CreateAssetDocumentNiagaraPrimitiveProvider()
{
	using namespace UE::AssetDocument::Niagara;
	return CreateAssetDocumentAssetTablePrimitiveProvider(ProviderId, &GetDescriptors, &GetBindings);
}
