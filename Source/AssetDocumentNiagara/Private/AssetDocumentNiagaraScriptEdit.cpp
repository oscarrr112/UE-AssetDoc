// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Script-level ops of fx.niagara.edit.v1: module / function / dynamic input assets and the scratch pads of a
// system. The graphs themselves are edited through the shared graph.* primitives (AssetDocumentNiagaraGraphAdapters.cpp);
// these ops are the script editor's other panels and its Apply.
//
// On a script asset (the script editor; an editor open on the asset is closed for the apply and reopened,
// AssetDocumentNiagaraOpenEditor.cpp):
//   {op: "check_dependents"}
//        refuses with Niagara.DependentHasUnsavedChanges when a loaded asset whose graphs call the script has unsaved
//        changes (an apply refreshes and recompiles them, and its rollback cannot give their unsaved state back).
//        Output "dependents": the loaded assets whose graphs call the script.
//   {op: "initialize_script", usage}
//        a script created by the generic factory lifecycle gets the usage the document says: the New-asset factory of
//        that usage (Module / Function / DynamicInput: UNiagaraModuleScriptFactory / UNiagaraFunctionScriptFactory /
//        UNiagaraDynamicInputScriptFactory, called through UFactory::FactoryCreateNew) creates the script in the
//        transient package, and its state (properties and inner objects: source, graph) moves into the asset, which
//        keeps its identity (the apply's handles hold it). Only for a package created by this apply; an existing
//        script's usage is immutable (Niagara.UsageImmutable).
//   {op: "versions", versions: [{number: "M.m", guid?, exposed?, change_description?, deprecated?, deprecation_message?,
//                                visible?}]}
//        the version menu (SNiagaraVersionWidget): "Enable versioning" (EnableVersioning), "Add version"
//        (AddNewVersion), the version Details (change description, deprecation, visibility in the selector),
//        "Expose version" (ExposeVersion) and "Delete version" (DeleteVersion; never the exposed version or 1.0). The
//        list is complete: versions it leaves out are deleted. Version numbers are not editable.
//   {op: "update_dependents"}
//        the script editor Apply's follow-up (FNiagaraEditorUtilities::RefreshAllScriptsFromExternalChanges, not
//        exported): every loaded script calling the script refreshes its calls (RefreshFromExternalChanges), and the
//        systems they belong to and the standalone scripts calling it recompile. Output "dependents": their assets.
// On a system (the system editor's Scratch Pad panel; emitter-level scratch pads are edited in place but not added,
// removed or renamed: Niagara.ScratchPadScope):
//   {op: "add_scratch_pad", name, usage, target_usage?}
//        "+ New scratch pad module / dynamic input": UNiagaraScratchPadViewModel::CreateNewScript, then the panel's
//        rename (FNiagaraScratchPadScriptViewModel::SetScriptName) to name.
//   {op: "delete_scratch_pad", name}        SetActiveScriptViewModel + DeleteActiveScript
//   {op: "rename_scratch_pad", name, to}    FNiagaraScratchPadScriptViewModel::SetScriptName
//   {op: "refresh_scratch_pad", scope}      the scratch pad's Apply after graph edits: its calls refresh
//        (RefreshFromExternalChanges + MarkNodeRequiresSynchronization, as FNiagaraScratchPadScriptViewModel::ApplyChanges)
//   {op: "scratch_pad_guids"}               graph and node guids that collide between scratch pads (pads made from the
//        same template copy them) are made unique, so that graph.* can address each graph and node.
// On both (scope: "" = the script asset, "/<pad>" a system scratch pad, "<emitter>/<pad>" an emitter's):
//   {op: "script_settings", scope?, properties?, reset?}
//        the script Details (the exposed version's FVersionedNiagaraScriptData): Modify, write,
//        UNiagaraScript::PostEditChangeVersionedProperty.
//   {op: "script_parameter", scope?, name, default?, default_mode?, default_binding?, static_switch_default?, meta?}
//        the parameter's Details (UNiagaraScriptVariable): default value, default mode, default binding, static
//        switch default (as FNiagaraScriptVariableDetails::OnStaticSwitchValueChanged) and metadata
//        ({properties, reset} over FNiagaraVariableMetaData); Modify, write, PostEditChangeProperty. A parameter the
//        graph does not reference has no entry (Niagara.ParameterNotReferenced).
//   {op: "purge_parameters", scope?}
//        the load-time rule of the parameter list (ManageScriptVariables, AssetDocumentNiagaraScriptVariableRules.cpp).
// Compile (fx.niagara.compile.v1 on a script asset) is the script editor's Compile: UNiagaraScript::RequestCompile of
// the exposed version (synchronous), messages attributed to their node.

#include "AssetDocumentNiagaraPrimitives.h"

#include "AssetDocumentCompileStatus.h"
#include "AssetDocumentEditorNonInteractiveScope.h"
#include "Runtime/AssetDocumentPackageSnapshot.h"

#include "EdGraph/EdGraphPin.h"
#include "Editor.h"
#include "Serialization/ObjectReader.h"
#include "Serialization/ObjectWriter.h"
#include "Factories/Factory.h"
#include "Misc/FeedbackContext.h"
#include "NiagaraCommon.h"
#include "NiagaraDataInterface.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScratchPadContainer.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraSystem.h"
#include "NiagaraVariableMetaData.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"
#include "ViewModels/NiagaraScratchPadScriptViewModel.h"
#include "ViewModels/NiagaraScratchPadViewModel.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "Utils/AssetDocumentEngineCompat.h"

#define LOCTEXT_NAMESPACE "AssetDocumentNiagaraScriptEdit"

namespace UE::AssetDocument::Niagara
{
namespace
{
using Graph::FReader;

const FName ScriptCompileId(TEXT("AssetDocumentNiagara.Script"));

FString UsageName(ENiagaraScriptUsage Usage)
{
	return StaticEnum<ENiagaraScriptUsage>()->GetNameStringByValue(static_cast<int64>(Usage));
}

bool ParseUsage(const FString& Name, ENiagaraScriptUsage& OutUsage)
{
	const int64 Value = StaticEnum<ENiagaraScriptUsage>()->GetValueByNameString(Name);
	if (Value == INDEX_NONE)
	{
		return false;
	}
	OutUsage = static_cast<ENiagaraScriptUsage>(Value);
	return true;
}

FGuid ExposedVersion(const UNiagaraScript& Script)
{
	return Script.IsVersioningEnabled() ? Script.GetExposedVersion().VersionGuid : FGuid();
}

FVersionedNiagaraScriptData* EditData(UNiagaraScript& Script)
{
	const FGuid Version = ExposedVersion(Script);
	return Version.IsValid() ? Script.GetScriptData(Version) : Script.GetLatestScriptData();
}

// ---- scopes -----------------------------------------------------------------------------------------------

UNiagaraScript* FindByName(TConstArrayView<TObjectPtr<UNiagaraScript>> Scripts, const FString& Name)
{
	for (UNiagaraScript* Script : Scripts)
	{
		if (Script != nullptr && Script->GetName() == Name)
		{
			return Script;
		}
	}
	return nullptr;
}

// The emitter-level scratch pad list of the system's emitter named Emitter (null when there is none).
TArray<TObjectPtr<UNiagaraScript>>* EmitterPads(UNiagaraSystem& System, const FString& Emitter)
{
	for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
	{
		FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
		if (Handle.GetName().ToString() == Emitter && Data != nullptr && Data->ScratchPads != nullptr)
		{
			return &Data->ScratchPads->Scripts;
		}
	}
	return nullptr;
}

// The script an op's scope names: the asset itself ("" / omitted), or a scratch pad of the system.
bool ResolveScope(FReader& Op, UObject& Asset, UNiagaraScript*& OutScript)
{
	FString Scope;
	if (!Op.String(TEXT("scope"), Scope, false))
	{
		return false;
	}
	if (UNiagaraScript* Script = Cast<UNiagaraScript>(&Asset))
	{
		if (!Scope.IsEmpty())
		{
			return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("A script asset's own script has scope \"\"."), TEXT("scope"));
		}
		OutScript = Script;
		return true;
	}
	UNiagaraSystem* System = Cast<UNiagaraSystem>(&Asset);
	FString Owner;
	FString Name;
	if (System == nullptr || !Scope.Split(TEXT("/"), &Owner, &Name, ESearchCase::CaseSensitive, ESearchDir::FromEnd) || Name.IsEmpty())
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("A scratch pad scope is \"/<scratch pad>\" or \"<emitter>/<scratch pad>\"."), TEXT("scope"));
	}
	TArray<TObjectPtr<UNiagaraScript>>* Pads = Owner.IsEmpty() ? &System->ScratchPadScripts : EmitterPads(*System, Owner);
	OutScript = Pads != nullptr ? FindByName(*Pads, Name) : nullptr;
	if (OutScript == nullptr)
	{
		return Op.Fail(TEXT("Niagara.ScratchPadNotFound"), FString::Printf(TEXT("The system has no scratch pad '%s'."), *Scope), TEXT("scope"));
	}
	return true;
}

// ---- dependents ---------------------------------------------------------------------------------------------

bool IsCallTo(const UEdGraphNode* Node, const UNiagaraScript& Script)
{
	const UNiagaraNode* NiagaraNode = Cast<UNiagaraNode>(Node);
	return NiagaraNode != nullptr && NiagaraNode->GetReferencedAsset() == &Script;
}

// The function call nodes of loaded graphs that call Script (FNiagaraEditorUtilities::GetReferencingFunctionCallNodes,
// not exported).
TArray<UNiagaraNodeFunctionCall*> ReferencingCalls(const UNiagaraScript& Script)
{
	TArray<UNiagaraNodeFunctionCall*> Out;
	for (TObjectIterator<UNiagaraNodeFunctionCall> It; It; ++It)
	{
		if (IsValid(*It) && It->FunctionScript == &Script)
		{
			Out.Add(*It);
		}
	}
	return Out;
}

// The packages (other than the script's and transient ones) whose graphs call the script.
TArray<UPackage*> DependentPackages(const UNiagaraScript& Script)
{
	TArray<UPackage*> Out;
	for (const UNiagaraNodeFunctionCall* Call : ReferencingCalls(Script))
	{
		UPackage* Package = Call->GetPackage();
		if (Package != nullptr && Package != Script.GetPackage() && Package != GetTransientPackage())
		{
			Out.AddUnique(Package);
		}
	}
	return Out;
}

bool CheckDependents(FReader& Op, UNiagaraScript& Script, TArray<FString>& OutDependents)
{
	TArray<FString> Dirty;
	for (const UPackage* Package : DependentPackages(Script))
	{
		OutDependents.AddUnique(Package->GetName());
		if (Package->IsDirty())
		{
			Dirty.Add(Package->GetName());
		}
	}
	if (Dirty.IsEmpty())
	{
		return true;
	}
	Dirty.Sort();
	return Op.Fail(TEXT("Niagara.DependentHasUnsavedChanges"), FString::Printf(TEXT(
		"Assets that use %s have unsaved changes: %s. Applying the script refreshes and recompiles them, and a rollback "
		"could not give their unsaved state back; save or discard them first."), *Script.GetName(), *FString::Join(Dirty, TEXT(", "))));
}

UNiagaraGraph* CalledGraph(const UNiagaraNodeFunctionCall& Call)
{
	if (Call.FunctionScript == nullptr)
	{
		return nullptr;
	}
	const FGuid Version = Call.SelectedScriptVersion.IsValid() ? Call.SelectedScriptVersion : ExposedVersion(*Call.FunctionScript);
	const UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Version.IsValid() ? Call.FunctionScript->GetSource(Version)
		: Call.FunctionScript->GetLatestSource());
	return Source != nullptr ? Source->NodeGraph : nullptr;
}

// UNiagaraGraph::GetAllReferencedGraphs (not exported): does Graph call Target, directly or through other scripts?
bool ReferencesGraph(const UNiagaraGraph& Graph, const UNiagaraGraph& Target, TSet<const UNiagaraGraph*>& Visited)
{
	bool bAlreadyVisited = false;
	Visited.Add(&Graph, &bAlreadyVisited);
	if (bAlreadyVisited)
	{
		return false;
	}
	TArray<UNiagaraNodeFunctionCall*> Calls;
	Graph.GetNodesOfClass(Calls);
	for (const UNiagaraNodeFunctionCall* Call : Calls)
	{
		const UNiagaraGraph* Called = CalledGraph(*Call);
		if (Called != nullptr && (Called == &Target || ReferencesGraph(*Called, Target, Visited)))
		{
			return true;
		}
	}
	return false;
}

// FNiagaraEditorUtilities::RefreshAllScriptsFromExternalChanges + CompileExistingEmitters (neither exported).
void UpdateDependents(UNiagaraScript& Script, TArray<FString>& OutDependents)
{
	const UNiagaraGraph* ScriptGraph = ScriptEditGraph(Script);
	TArray<UNiagaraScript*> Affected;
	for (TObjectIterator<UNiagaraScript> It; It; ++It)
	{
		UNiagaraScript* Other = *It;
		if (Other == &Script || !IsValid(Other))
		{
			continue;
		}
		UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Other->GetLatestSource());
		if (Source == nullptr || Source->NodeGraph == nullptr)
		{
			continue;
		}
		TArray<UNiagaraNode*> Nodes;
		Source->NodeGraph->GetNodesOfClass(Nodes);
		bool bRefreshed = false;
		for (UNiagaraNode* Node : Nodes)
		{
			if (IsCallTo(Node, Script))
			{
				Node->RefreshFromExternalChanges();
				bRefreshed = true;
			}
		}
		TSet<const UNiagaraGraph*> Visited;
		if (bRefreshed || (ScriptGraph != nullptr && ReferencesGraph(*Source->NodeGraph, *ScriptGraph, Visited)))
		{
			Affected.AddUnique(Other);
		}
	}
	TArray<UNiagaraSystem*> Systems;
	for (UNiagaraScript* Other : Affected)
	{
		if (Other->IsParticleScript() || Other->IsEmitterSpawnScript() || Other->IsEmitterUpdateScript())
		{
			const FVersionedNiagaraEmitter Emitter = Other->GetOuterEmitter();
			if (Emitter.Emitter == nullptr)
			{
				continue;
			}
			for (TObjectIterator<UNiagaraSystem> It; It; ++It)
			{
				if (IsValid(*It) && It->ReferencesInstanceEmitter(Emitter))
				{
					Systems.AddUnique(*It);
				}
			}
		}
		else if (Other->IsSystemSpawnScript() || Other->IsSystemUpdateScript())
		{
			if (UNiagaraSystem* System = Other->GetTypedOuter<UNiagaraSystem>())
			{
				Systems.AddUnique(System);
			}
		}
		else if (Other->IsStandaloneScript() && Other->IsCompilable())
		{
			Other->RequestCompile(ExposedVersion(*Other), false);
			if (Other->GetPackage() != GetTransientPackage())
			{
				OutDependents.AddUnique(Other->GetOutermostObject()->GetPathName());
			}
		}
	}
	for (UNiagaraSystem* System : Systems)
	{
		System->RequestCompile(false);
		SettleCompilation(*System);
		if (System->GetPackage() != GetTransientPackage())
		{
			OutDependents.AddUnique(System->GetPathName());
		}
	}
	OutDependents.Sort();
}

// ---- initialize -----------------------------------------------------------------------------------------------

const TCHAR* FactoryClassPath(ENiagaraScriptUsage Usage)
{
	switch (Usage)
	{
	case ENiagaraScriptUsage::Module: return TEXT("/Script/NiagaraEditor.NiagaraModuleScriptFactory");
	case ENiagaraScriptUsage::Function: return TEXT("/Script/NiagaraEditor.NiagaraFunctionScriptFactory");
	case ENiagaraScriptUsage::DynamicInput: return TEXT("/Script/NiagaraEditor.NiagaraDynamicInputScriptFactory");
	default: return nullptr;
	}
}

bool InitializeScript(FReader& Op, UNiagaraScript& Script, bool& bOutChanged)
{
	FString UsageText;
	ENiagaraScriptUsage Usage = ENiagaraScriptUsage::Module;
	if (!Op.OnlyFields({TEXT("op"), TEXT("usage")}) || !Op.String(TEXT("usage"), UsageText))
	{
		return false;
	}
	const TCHAR* FactoryPath = ParseUsage(UsageText, Usage) ? FactoryClassPath(Usage) : nullptr;
	if (FactoryPath == nullptr)
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("usage is Module, Function or DynamicInput."), TEXT("usage"));
	}
	if (Script.GetUsage() == Usage)
	{
		return true;
	}
	UPackage* Package = Script.GetPackage();
	if (!Package->HasAnyPackageFlags(PKG_NewlyCreated))
	{
		return Op.Fail(TEXT("Niagara.UsageImmutable"), FString::Printf(TEXT(
			"%s is a %s script; a script's usage is chosen when it is created (create a new asset for a %s)."), *Script.GetName(),
			*UsageName(Script.GetUsage()), *UsageText), TEXT("usage"));
	}
	UClass* FactoryClass = FindObject<UClass>(nullptr, FactoryPath);
	UFactory* Factory = FactoryClass != nullptr ? NewObject<UFactory>(GetTransientPackage(), FactoryClass) : nullptr;
	if (Factory == nullptr)
	{
		return Op.Fail(TEXT("Niagara.UsageImmutable"), FString::Printf(TEXT("The editor has no factory %s."), FactoryPath), TEXT("usage"));
	}
	// The factory makes the script in the transient package (constructing it in place over the asset is not possible:
	// the apply's handles hold the asset). Its state then moves into the asset: the previous inner objects go out of
	// the way, every property is copied (no delta: values at the class default are copied too), and the new inner
	// objects (the source and its graph) move under the asset, so that the copied references point into it.
	const FName TempName = MakeUniqueObjectName(GetTransientPackage(), Script.GetClass(), FName(*(Script.GetName() + TEXT("_Initialize"))));
	UNiagaraScript* Created = Cast<UNiagaraScript>(Factory->FactoryCreateNew(Script.GetClass(), GetTransientPackage(), TempName, RF_Transactional, nullptr, GWarn));
	if (Created == nullptr || Created->GetUsage() != Usage)
	{
		return Op.Fail(TEXT("Niagara.UsageImmutable"), FString::Printf(TEXT("The %s factory did not create a script."), *UsageText), TEXT("usage"));
	}
	TArray<UObject*> Previous;
	GetObjectsWithOuter(&Script, Previous, AssetDocumentObjects::DirectOnly);
	for (UObject* Object : Previous)
	{
		Object->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
		Object->MarkAsGarbage();
	}
	{
		TArray<uint8> Bytes;
		FObjectWriter Writer(Created, Bytes, false, false, /*bDoDelta*/ false);
		FObjectReader Reader(&Script, Bytes, false, false);
	}
	TArray<UObject*> Moved;
	GetObjectsWithOuter(Created, Moved, AssetDocumentObjects::DirectOnly);
	for (UObject* Object : Moved)
	{
		Object->Rename(nullptr, &Script, REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
	}
	Created->ClearFlags(RF_Public | RF_Standalone);
	Created->MarkAsGarbage();
	if (Script.GetUsage() != Usage || Script.GetLatestSource() == nullptr || Script.GetLatestSource()->GetOuter() != &Script)
	{
		return Op.Fail(TEXT("Niagara.UsageImmutable"), FString::Printf(TEXT("The %s factory's script did not move into the asset."), *UsageText), TEXT("usage"));
	}
	Script.SetFlags(RF_Transactional);
	Script.MarkPackageDirty();
	bOutChanged = true;
	return true;
}

// ---- versions -------------------------------------------------------------------------------------------------

struct FVersionEntry
{
	int32 Major = 1;
	int32 Minor = 0;
	FGuid Guid;
	bool bExposed = false;
	FString ChangeDescription;
	bool bDeprecated = false;
	FString DeprecationMessage;
	bool bVisible = true;
	FString Where;
};

bool ParseNumber(const FString& Text, int32& OutMajor, int32& OutMinor)
{
	FString Major;
	FString Minor;
	if (!Text.Split(TEXT("."), &Major, &Minor) || !Major.IsNumeric() || !Minor.IsNumeric() || Major.Contains(TEXT("-")) || Minor.Contains(TEXT("-")))
	{
		return false;
	}
	OutMajor = FCString::Atoi(*Major);
	OutMinor = FCString::Atoi(*Minor);
	return OutMajor >= 1 && OutMinor >= 0;
}

bool ReadVersionEntries(FReader& Op, const FValue& List, TArray<FVersionEntry>& Out)
{
	for (int32 Index = 0; Index < List.Elements.Num(); ++Index)
	{
		FVersionEntry& Entry = Out.AddDefaulted_GetRef();
		Entry.Where = FString::Printf(TEXT("versions/%d"), Index);
		FReader Item = Op.Child(Entry.Where, List.Elements[Index]);
		FString Number;
		FString Guid;
		if (!Item.IsObject() || !Item.OnlyFields({TEXT("number"), TEXT("guid"), TEXT("exposed"), TEXT("change_description"), TEXT("deprecated"),
			TEXT("deprecation_message"), TEXT("visible")}) || !Item.String(TEXT("number"), Number) || !Item.String(TEXT("guid"), Guid, false)
			|| !Item.Bool(TEXT("exposed"), Entry.bExposed) || !Item.String(TEXT("change_description"), Entry.ChangeDescription, false)
			|| !Item.Bool(TEXT("deprecated"), Entry.bDeprecated) || !Item.String(TEXT("deprecation_message"), Entry.DeprecationMessage, false))
		{
			return Item.IsObject() ? false : Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("A version is an object."), Entry.Where);
		}
		if (Item.Has(TEXT("visible")) && !Item.Bool(TEXT("visible"), Entry.bVisible))
		{
			return false;
		}
		if (!ParseNumber(Number, Entry.Major, Entry.Minor))
		{
			return Item.Fail(TEXT("Graph.InvalidArgument"), TEXT("number is \"<major>.<minor>\" (major >= 1)."), TEXT("number"));
		}
		if (!Guid.IsEmpty() && !FGuid::Parse(Guid, Entry.Guid))
		{
			return Item.Fail(TEXT("Graph.InvalidArgument"), TEXT("guid is a GUID."), TEXT("guid"));
		}
		for (int32 Other = 0; Other < Index; ++Other)
		{
			if (Out[Other].Major == Entry.Major && Out[Other].Minor == Entry.Minor)
			{
				return Item.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("Version %s is listed twice."), *Number), TEXT("number"));
			}
		}
	}
	return true;
}

bool SetText(FText& Field, const FString& Value)
{
	if (Field.ToString() == Value)
	{
		return false;
	}
	Field = FText::FromString(Value);
	return true;
}

} // namespace

bool ApplyVersions(FReader& Op, UObject& Asset, FNiagaraVersionedObject& Versioned, bool& bOutChanged)
{
	const FValue* List = nullptr;
	TArray<FVersionEntry> Entries;
	if (!Op.OnlyFields({TEXT("op"), TEXT("versions")}) || !Op.Array(TEXT("versions"), List) || !ReadVersionEntries(Op, *List, Entries))
	{
		return false;
	}
	if (Entries.IsEmpty())
	{
		return !Versioned.IsVersioningEnabled() || Op.Fail(TEXT("Niagara.VersionNotDeletable"), TEXT(
			"The version menu cannot turn versioning off: list the asset's versions."), TEXT("versions"));
	}
	if (Entries.FilterByPredicate([](const FVersionEntry& Entry) { return Entry.bExposed; }).Num() > 1)
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("At most one version is exposed."), TEXT("versions"));
	}
	if (!Versioned.IsVersioningEnabled())
	{
		const FScopedTransaction Transaction(LOCTEXT("EnableVersioning", "Enable versioning"));
		Asset.Modify();
		Versioned.EnableVersioning();
		bOutChanged = true;
	}
	// Match each entry to a version (by guid, else by number); "Add version" for the others.
	TArray<FGuid> Resolved;
	for (const FVersionEntry& Entry : Entries)
	{
		const FNiagaraAssetVersion* Match = nullptr;
		const TArray<FNiagaraAssetVersion> Available = Versioned.GetAllAvailableVersions();
		for (const FNiagaraAssetVersion& Version : Available)
		{
			if (Entry.Guid.IsValid() && Version.VersionGuid == Entry.Guid)
			{
				Match = &Version;
			}
		}
		if (Match != nullptr && (Match->MajorVersion != Entry.Major || Match->MinorVersion != Entry.Minor))
		{
			return Op.Fail(TEXT("Niagara.VersionNumber"), FString::Printf(TEXT("Version %s is %d.%d; version numbers cannot be changed."),
				*Entry.Guid.ToString(EGuidFormats::DigitsWithHyphensLower), Match->MajorVersion, Match->MinorVersion), Entry.Where);
		}
		for (const FNiagaraAssetVersion& Version : Available)
		{
			if (Match == nullptr && Version.MajorVersion == Entry.Major && Version.MinorVersion == Entry.Minor)
			{
				Match = &Version;
			}
		}
		if (Match != nullptr)
		{
			Resolved.Add(Match->VersionGuid);
			continue;
		}
		const FScopedTransaction Transaction(LOCTEXT("AddVersion", "Add version"));
		Asset.Modify();
		Resolved.Add(Versioned.AddNewVersion(Entry.Major, Entry.Minor));
		bOutChanged = true;
	}
	// The version Details (SNiagaraVersionWidget::NotifyPostChange).
	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		const FVersionEntry& Entry = Entries[Index];
		const TSharedPtr<FNiagaraVersionDataAccessor> Data = Versioned.GetVersionDataAccessor(Resolved[Index]);
		if (!Data.IsValid())
		{
			return Op.Fail(TEXT("Niagara.UnknownVersion"), TEXT("The version was not created."), Entry.Where);
		}
		const bool bDifferent = Data->GetVersionChangeDescription().ToString() != Entry.ChangeDescription || Data->IsDeprecated() != Entry.bDeprecated
			|| Data->GetDeprecationMessage().ToString() != Entry.DeprecationMessage
			|| Data->GetObjectVersion().bIsVisibleInVersionSelector != Entry.bVisible;
		if (!bDifferent)
		{
			continue;
		}
		const FScopedTransaction Transaction(LOCTEXT("EditVersion", "Edit version"));
		Asset.Modify();
		SetText(Data->GetVersionChangeDescription(), Entry.ChangeDescription);
		Data->IsDeprecated() = Entry.bDeprecated;
		SetText(Data->GetDeprecationMessage(), Entry.DeprecationMessage);
		Data->GetObjectVersion().bIsVisibleInVersionSelector = Entry.bVisible;
		bOutChanged = true;
	}
	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		if (Entries[Index].bExposed && Resolved[Index] != Versioned.GetExposedVersion().VersionGuid)
		{
			const FScopedTransaction Transaction(LOCTEXT("ExposeVersion", "Expose version"));
			Asset.Modify();
			Versioned.ExposeVersion(Resolved[Index]);
			bOutChanged = true;
		}
	}
	for (const FNiagaraAssetVersion& Version : Versioned.GetAllAvailableVersions())
	{
		if (Resolved.Contains(Version.VersionGuid))
		{
			continue;
		}
		// SNiagaraVersionWidget::CanExecuteDeleteAction.
		if (Version.VersionGuid == Versioned.GetExposedVersion().VersionGuid || (Version.MajorVersion == 1 && Version.MinorVersion == 0))
		{
			return Op.Fail(TEXT("Niagara.VersionNotDeletable"), FString::Printf(TEXT(
				"Version %d.%d is %s and cannot be deleted; list it."), Version.MajorVersion, Version.MinorVersion,
				Version.MajorVersion == 1 && Version.MinorVersion == 0 ? TEXT("the first version") : TEXT("the exposed version")), TEXT("versions"));
		}
		const FScopedTransaction Transaction(LOCTEXT("DeleteVersion", "Delete version"));
		Asset.Modify();
		Versioned.DeleteVersion(Version.VersionGuid);
		bOutChanged = true;
	}
	return true;
}

namespace
{
// ---- settings and parameters ------------------------------------------------------------------------------------

bool ScriptSettings(FReader& Op, UObject& Asset, bool& bOutChanged)
{
	UNiagaraScript* Script = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("scope"), TEXT("properties"), TEXT("reset")}) || !ResolveScope(Op, Asset, Script))
	{
		return false;
	}
	FVersionedNiagaraScriptData* Data = EditData(*Script);
	if (Data == nullptr)
	{
		return Op.Fail(TEXT("Niagara.UnknownVersion"), TEXT("The script has no version data."));
	}
	const FGuid Version = Data->Version.VersionGuid;
	const FVersionedNiagaraScriptData Defaults;
	const FScopedTransaction Transaction(LOCTEXT("EditScriptSettings", "Edit script settings"));
	return WriteProperties(Op, *FVersionedNiagaraScriptData::StaticStruct(), Data, &Defaults, Script,
		{TEXT("Version"), TEXT("VersionChangeDescription"), TEXT("bDeprecated"), TEXT("DeprecationMessage")}, bOutChanged,
		[Script](FProperty&) { Script->Modify(); },
		[Script, Version](FProperty& Property)
		{
			FPropertyChangedEvent Event(&Property, EPropertyChangeType::ValueSet);
			Script->PostEditChangeVersionedProperty(Event, Version);
		});
}

UNiagaraScriptVariable* FindVariable(UNiagaraGraph& Graph, const FString& Name)
{
	for (const TPair<FNiagaraVariable, TObjectPtr<UNiagaraScriptVariable>>& Entry : Graph.GetAllMetaData())
	{
		if (Entry.Value != nullptr && Entry.Key.GetName().ToString() == Name)
		{
			return Entry.Value.Get();
		}
	}
	return nullptr;
}

void PostEdit(UNiagaraScriptVariable& Variable, const TCHAR* PropertyName)
{
	FProperty* Property = FindFProperty<FProperty>(UNiagaraScriptVariable::StaticClass(), PropertyName);
	FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
	Variable.PostEditChangeProperty(Event);
}

bool ScriptParameter(FReader& Op, UObject& Asset, bool& bOutChanged)
{
	UNiagaraScript* Script = nullptr;
	FString Name;
	if (!Op.OnlyFields({TEXT("op"), TEXT("scope"), TEXT("name"), TEXT("default"), TEXT("default_mode"), TEXT("default_binding"),
		TEXT("static_switch_default"), TEXT("meta")}) || !ResolveScope(Op, Asset, Script) || !Op.String(TEXT("name"), Name))
	{
		return false;
	}
	UNiagaraGraph* Graph = ScriptEditGraph(*Script);
	UNiagaraScriptVariable* Variable = Graph != nullptr ? FindVariable(*Graph, Name) : nullptr;
	if (Variable == nullptr)
	{
		return Op.Fail(TEXT("Niagara.ParameterNotReferenced"), FString::Printf(TEXT(
			"The graph does not use a parameter '%s': a script's parameters are the ones its Map Get / Map Set nodes and static "
			"switches use (add it to a node's Pins)."), *Name), TEXT("name"));
	}
	const FScopedTransaction Transaction(LOCTEXT("EditScriptParameter", "Edit parameter"));
	const FNiagaraTypeDefinition& Type = Variable->Variable.GetType();
	FString ModeText;
	if (!Op.String(TEXT("default_mode"), ModeText, false))
	{
		return false;
	}
	if (Op.Has(TEXT("default_mode")) || Op.Has(TEXT("default")))
	{
		ENiagaraDefaultMode Mode = ENiagaraDefaultMode::Value;
		if (!ModeText.IsEmpty())
		{
			const int64 Value = StaticEnum<ENiagaraDefaultMode>()->GetValueByNameString(ModeText);
			if (Value == INDEX_NONE)
			{
				return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("default_mode is Value, Binding, Custom or FailIfPreviouslyNotSet."), TEXT("default_mode"));
			}
			Mode = static_cast<ENiagaraDefaultMode>(Value);
		}
		if (Variable->DefaultMode != Mode)
		{
			Variable->Modify();
			Variable->DefaultMode = Mode;
			PostEdit(*Variable, TEXT("DefaultMode"));
			bOutChanged = true;
		}
	}
	FString BindingText;
	if (!Op.String(TEXT("default_binding"), BindingText, false))
	{
		return false;
	}
	if (Op.Has(TEXT("default_binding")) && Variable->DefaultMode == ENiagaraDefaultMode::Binding
		&& Variable->DefaultBinding.GetName() != FName(*BindingText))
	{
		Variable->Modify();
		Variable->DefaultBinding.SetName(FName(*BindingText));
		PostEdit(*Variable, TEXT("DefaultBinding"));
		bOutChanged = true;
	}
	if (const FValue* Default = Op.Find(TEXT("default")))
	{
		if (Type.IsDataInterface() || Type.IsUObject() || Type.GetSize() <= 0)
		{
			return Op.Fail(TEXT("Niagara.InvalidPropertyValue"), FString::Printf(TEXT("'%s' (%s) has no plain default value."), *Name,
				*TypeToken(Type)), TEXT("default"));
		}
		TArray<uint8> Bytes;
		Bytes.SetNumZeroed(Type.GetSize());
		const uint8* Current = Variable->GetDefaultValueData();
		if (Current != nullptr && Variable->GetDefaultValueVariant().GetNumBytes() == Type.GetSize())
		{
			FMemory::Memcpy(Bytes.GetData(), Current, Bytes.Num());
		}
		FString Message;
		if (!DecodeValue(Type, *Default, Bytes.GetData(), Message))
		{
			return Op.Fail(TEXT("Niagara.InvalidPropertyValue"), FString::Printf(TEXT("The default of '%s' does not fit %s: %s"), *Name,
				*TypeToken(Type), *Message), TEXT("default"));
		}
		if (Current == nullptr || Variable->GetDefaultValueVariant().GetNumBytes() != Type.GetSize() || FMemory::Memcmp(Current, Bytes.GetData(), Bytes.Num()) != 0)
		{
			Variable->Modify();
			Variable->SetDefaultValueData(Bytes.GetData());
			PostEdit(*Variable, TEXT("DefaultValueVariant"));
			bOutChanged = true;
		}
	}
	if (Op.Has(TEXT("static_switch_default")))
	{
		int64 Value = 0;
		if (!Op.Int(TEXT("static_switch_default"), Value))
		{
			return false;
		}
		if (!Variable->GetIsStaticSwitch())
		{
			return Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("'%s' is not a static switch."), *Name), TEXT("static_switch_default"));
		}
		const int32 Switch = static_cast<int32>(Value);
		if (Variable->GetStaticSwitchDefaultValue() != Switch)
		{
			// FNiagaraScriptVariableDetails::OnStaticSwitchValueChanged.
			Variable->Modify();
			if (Variable->Variable.GetSizeInBytes() == sizeof(int32))
			{
				Variable->Variable.AllocateData();
				Variable->Variable.SetData(reinterpret_cast<const uint8*>(&Switch));
			}
			Variable->SetStaticSwitchDefaultValue(Switch);
			PostEdit(*Variable, TEXT("DefaultValueVariant"));
			bOutChanged = true;
		}
	}
	if (const FValue* Meta = Op.Find(TEXT("meta")))
	{
		if (Meta->Kind != EAssetDocumentRuntimeValueKind::Object)
		{
			return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("meta is {properties?, reset?}."), TEXT("meta"));
		}
		FReader MetaOp = Op.Child(TEXT("meta"), *Meta);
		if (!MetaOp.OnlyFields({TEXT("properties"), TEXT("reset")}))
		{
			return false;
		}
		const FNiagaraVariableMetaData Defaults;
		UNiagaraScriptVariable* Target = Variable;
		if (!WriteProperties(MetaOp, *FNiagaraVariableMetaData::StaticStruct(), &Variable->Metadata, &Defaults, Variable, {}, bOutChanged,
			[Target](FProperty&) { Target->Modify(); },
			[Target](FProperty&) { PostEdit(*Target, TEXT("Metadata")); }))
		{
			return false;
		}
	}
	return true;
}

bool PurgeParameters(FReader& Op, UObject& Asset, bool& bOutChanged)
{
	UNiagaraScript* Script = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("scope")}) || !ResolveScope(Op, Asset, Script))
	{
		return false;
	}
	UNiagaraGraph* Graph = ScriptEditGraph(*Script);
	if (Graph != nullptr && ManageScriptVariables(*Graph, nullptr, nullptr))
	{
		bOutChanged = true;
	}
	return true;
}

// ---- scratch pads ---------------------------------------------------------------------------------------------

// The scratch pads the Scratch Pad panel lists, and the object they live in (UNiagaraScratchPadViewModel's
// GetOuterAndTargetScripts): the system's own in the system editor, its only emitter's in the emitter editor (the
// placeholder system of an emitter asset).
TArray<TObjectPtr<UNiagaraScript>>* PanelPads(UNiagaraSystem& System, const FNiagaraSystemViewModel* ViewModel, UObject*& OutOuter)
{
	if (ViewModel != nullptr && ViewModel->GetEditMode() != ENiagaraSystemViewModelEditMode::SystemAsset)
	{
		const FVersionedNiagaraEmitterData* Data = System.GetEmitterHandles().Num() == 1 ? System.GetEmitterHandles()[0].GetEmitterData() : nullptr;
		OutOuter = Data != nullptr ? Data->ScratchPads : nullptr;
		return Data != nullptr && Data->ScratchPads != nullptr ? &Data->ScratchPads->Scripts : nullptr;
	}
	OutOuter = &System;
	return &System.ScratchPadScripts;
}

struct FPanel
{
	UNiagaraScratchPadViewModel* Pads = nullptr;
	TArray<TObjectPtr<UNiagaraScript>>* List = nullptr;
	UObject* Outer = nullptr;
};

bool RequireViewModel(FReader& Op, UNiagaraSystem& System, FNiagaraSystemViewModel* ViewModel, FPanel& OutPanel)
{
	OutPanel.Pads = ViewModel != nullptr ? EnsureScratchPadViewModels(*ViewModel) : nullptr;
	OutPanel.List = PanelPads(System, ViewModel, OutPanel.Outer);
	return (OutPanel.Pads != nullptr && OutPanel.List != nullptr && OutPanel.Outer != nullptr)
		|| Op.Fail(TEXT("Niagara.ViewModelUnavailable"), TEXT("The asset has no scratch pad view model."));
}

bool ValidPadName(FReader& Op, UObject& Outer, const FString& Name, const TCHAR* Field)
{
	if (Name.IsEmpty() || FNiagaraUtilities::SanitizeNameForObjectsAndPackages(Name) != Name)
	{
		return Op.Fail(TEXT("Niagara.ScratchPadName"), FString::Printf(TEXT("'%s' is not a valid scratch pad name (letters, digits and _)."), *Name), Field);
	}
	if (StaticFindObjectFast(nullptr, &Outer, FName(*Name)) != nullptr)
	{
		return Op.Fail(TEXT("Niagara.ScratchPadName"), FString::Printf(TEXT("The scratch pads' owner already has an object named '%s'."), *Name), Field);
	}
	return true;
}

TSharedPtr<FNiagaraScratchPadScriptViewModel> PadViewModel(FReader& Op, UNiagaraScratchPadViewModel& Pads, TConstArrayView<TObjectPtr<UNiagaraScript>> List,
	const TCHAR* Field)
{
	FString Name;
	if (!Op.String(Field, Name))
	{
		return nullptr;
	}
	UNiagaraScript* Script = FindByName(List, Name);
	TSharedPtr<FNiagaraScratchPadScriptViewModel> Pad = Script != nullptr ? Pads.GetViewModelForScript(Script) : nullptr;
	if (!Pad.IsValid())
	{
		Op.Fail(TEXT("Niagara.ScratchPadNotFound"), FString::Printf(TEXT("There is no scratch pad '%s'."), *Name), Field);
	}
	return Pad;
}

bool AddScratchPad(FReader& Op, UNiagaraSystem& System, FNiagaraSystemViewModel* ViewModel, bool& bOutChanged)
{
	FString Name;
	FString UsageText;
	FString TargetText;
	FPanel Panel;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("usage"), TEXT("target_usage")}) || !Op.String(TEXT("name"), Name)
		|| !Op.String(TEXT("usage"), UsageText) || !Op.String(TEXT("target_usage"), TargetText, false) || !RequireViewModel(Op, System, ViewModel, Panel)
		|| !ValidPadName(Op, *Panel.Outer, Name, TEXT("name")))
	{
		return false;
	}
	UNiagaraScratchPadViewModel* Pads = Panel.Pads;
	ENiagaraScriptUsage Usage = ENiagaraScriptUsage::Module;
	if (!ParseUsage(UsageText, Usage) || !Pads->GetAvailableUsages().Contains(Usage))
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("A scratch pad's usage is Module or DynamicInput."), TEXT("usage"));
	}
	ENiagaraScriptUsage Target = ENiagaraScriptUsage::ParticleUpdateScript;
	if (!TargetText.IsEmpty() && !ParseUsage(TargetText, Target))
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("target_usage is a script usage (ParticleUpdateScript, SystemSpawnScript, ...)."), TEXT("target_usage"));
	}
	const FScopedTransaction Transaction(LOCTEXT("AddScratchPad", "Add scratch pad script"));
	TSharedPtr<FNiagaraScratchPadScriptViewModel> Pad = Pads->CreateNewScript(Usage, Target, FNiagaraTypeDefinition());
	if (!Pad.IsValid() || Pad->GetOriginalScript() == nullptr)
	{
		return Op.Fail(TEXT("Niagara.AddScratchPadFailed"), TEXT("The scratch pad was not created."), TEXT("name"));
	}
	bOutChanged = true;
	Pad->SetScriptName(FText::FromString(Name));
	if (Pad->GetOriginalScript()->GetName() != Name)
	{
		return Op.Fail(TEXT("Niagara.ScratchPadName"), FString::Printf(TEXT("The scratch pad could not be named '%s'."), *Name), TEXT("name"));
	}
	return true;
}

bool DeleteScratchPad(FReader& Op, UNiagaraSystem& System, FNiagaraSystemViewModel* ViewModel, bool& bOutChanged)
{
	FPanel Panel;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name")}) || !RequireViewModel(Op, System, ViewModel, Panel))
	{
		return false;
	}
	TSharedPtr<FNiagaraScratchPadScriptViewModel> Pad = PadViewModel(Op, *Panel.Pads, *Panel.List, TEXT("name"));
	if (!Pad.IsValid())
	{
		return false;
	}
	Panel.Pads->SetActiveScriptViewModel(Pad.ToSharedRef());
	Panel.Pads->DeleteActiveScript();
	bOutChanged = true;
	return true;
}

bool RenameScratchPad(FReader& Op, UNiagaraSystem& System, FNiagaraSystemViewModel* ViewModel, bool& bOutChanged)
{
	FPanel Panel;
	FString To;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("to")}) || !RequireViewModel(Op, System, ViewModel, Panel) || !Op.String(TEXT("to"), To))
	{
		return false;
	}
	TSharedPtr<FNiagaraScratchPadScriptViewModel> Pad = PadViewModel(Op, *Panel.Pads, *Panel.List, TEXT("name"));
	if (!Pad.IsValid())
	{
		return false;
	}
	if (Pad->GetOriginalScript()->GetName() == To)
	{
		return true;
	}
	if (!ValidPadName(Op, *Panel.Outer, To, TEXT("to")))
	{
		return false;
	}
	Pad->SetScriptName(FText::FromString(To));
	bOutChanged = true;
	return Pad->GetOriginalScript()->GetName() == To
		|| Op.Fail(TEXT("Niagara.ScratchPadName"), FString::Printf(TEXT("The scratch pad could not be renamed to '%s'."), *To), TEXT("to"));
}

bool RefreshScratchPad(FReader& Op, UObject& Asset, bool& bOutChanged)
{
	UNiagaraScript* Script = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("scope")}) || !ResolveScope(Op, Asset, Script))
	{
		return false;
	}
	// FNiagaraScratchPadScriptViewModel::ApplyChanges after the copy: the calls of the scratch pad refresh.
	for (UNiagaraNodeFunctionCall* Call : ReferencingCalls(*Script))
	{
		Call->RefreshFromExternalChanges();
		Call->MarkNodeRequiresSynchronization(TEXT("ScratchPadChangesApplied"), true);
	}
	bOutChanged = true;
	return true;
}

bool ScratchPadGuids(FReader& Op, UNiagaraSystem& System, bool& bOutChanged)
{
	if (!Op.OnlyFields({TEXT("op")}))
	{
		return false;
	}
	TArray<UNiagaraScript*> Scripts;
	for (UNiagaraScript* Script : System.ScratchPadScripts)
	{
		Scripts.Add(Script);
	}
	for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
	{
		const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
		if (Data != nullptr && Data->ScratchPads != nullptr)
		{
			for (UNiagaraScript* Script : Data->ScratchPads->Scripts)
			{
				Scripts.Add(Script);
			}
		}
	}
	TSet<FGuid> GraphGuids;
	TSet<FGuid> NodeGuids;
	for (UNiagaraScript* Script : Scripts)
	{
		UNiagaraGraph* Graph = Script != nullptr ? ScriptEditGraph(*Script) : nullptr;
		if (Graph == nullptr)
		{
			continue;
		}
		bool bSeen = false;
		GraphGuids.Add(Graph->GraphGuid, &bSeen);
		if (bSeen)
		{
			Graph->Modify();
			Graph->GraphGuid = FGuid::NewGuid();
			GraphGuids.Add(Graph->GraphGuid);
			bOutChanged = true;
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node == nullptr)
			{
				continue;
			}
			NodeGuids.Add(Node->NodeGuid, &bSeen);
			if (bSeen)
			{
				Node->Modify();
				Node->CreateNewGuid();
				NodeGuids.Add(Node->NodeGuid);
				bOutChanged = true;
			}
		}
	}
	return true;
}

// ---- compile -------------------------------------------------------------------------------------------------------

class FScriptRebuilder final : public IAssetDocumentDerivedStateRebuilder
{
public:
	virtual FName GetId() const override { return ScriptCompileId; }
	virtual bool Supports(const UObject& PrimaryAsset) const override { return PrimaryAsset.IsA<UNiagaraScript>(); }
	// A compile copies the data interfaces the script uses into the script (compile output, rebuilt below).
	virtual EAssetDocumentSnapshotObjectRole Classify(const UObject& Object) const override
	{
		if (Object.IsA<UNiagaraDataInterface>() && Object.GetOuter() != nullptr && Object.GetOuter()->IsA<UNiagaraScript>())
		{
			return EAssetDocumentSnapshotObjectRole::Derived;
		}
		return IAssetDocumentDerivedStateRebuilder::Classify(Object);
	}
	virtual FAssetDocumentContractValidationResult Rebuild(UObject& PrimaryAsset) override
	{
		FAssetDocumentContractValidationResult Result;
		if (UNiagaraScript* Script = Cast<UNiagaraScript>(&PrimaryAsset); Script != nullptr && Script->IsStandaloneScript() && Script->IsCompilable())
		{
			FAssetDocumentEditorNonInteractiveScope NonInteractive;
			Script->RequestCompile(ExposedVersion(*Script), true);
		}
		Result.bValid = true;
		return Result;
	}
};
} // namespace

UNiagaraGraph* ScriptEditGraph(const UNiagaraScript& Script)
{
	const FGuid Version = ExposedVersion(Script);
	const UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Version.IsValid() ? Script.GetSource(Version) : Script.GetLatestSource());
	return Source != nullptr ? Source->NodeGraph : nullptr;
}

FString ReadScriptCompileStatus(const UNiagaraScript& Script, TArray<FScriptCompileMessage>& OutMessages)
{
	const UNiagaraGraph* Graph = ScriptEditGraph(Script);
	for (const FNiagaraCompileEvent& Event : Script.GetVMExecutableData().LastCompileEvents)
	{
		if (Event.Severity != FNiagaraCompileEventSeverity::Warning && Event.Severity != FNiagaraCompileEventSeverity::Error)
		{
			continue;
		}
		FScriptCompileMessage& Message = OutMessages.AddDefaulted_GetRef();
		Message.Severity = Event.Severity == FNiagaraCompileEventSeverity::Error ? TEXT("Error") : TEXT("Warning");
		Message.Message = Event.Message;
		if (Graph != nullptr && Event.NodeGuid.IsValid())
		{
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node != nullptr && Node->NodeGuid == Event.NodeGuid)
				{
					Message.Node = Node->GetName();
					break;
				}
			}
		}
	}
	const ENiagaraScriptCompileStatus Status = Script.GetLastCompileStatus();
	// A finished compile that left the script without executable data (no status, no events): the Vector VM compile
	// failed and Unreal wrote its errors only to the Output Log (LogNiagaraCompiler).
	if (Status == ENiagaraScriptCompileStatus::NCS_Unknown && Script.AreScriptAndSourceSynchronized(ExposedVersion(Script))
		&& Script.GetVMExecutableData().LastCompileEvents.IsEmpty())
	{
		FScriptCompileMessage& Message = OutMessages.AddDefaulted_GetRef();
		Message.Severity = TEXT("Error");
		Message.Message = TEXT("The script did not compile: the Vector VM compile failed (its errors are in the Output Log, LogNiagaraCompiler).");
		return TEXT("Error");
	}
	if (!Script.AreScriptAndSourceSynchronized(ExposedVersion(Script)) || Status == ENiagaraScriptCompileStatus::NCS_Unknown
		|| Status == ENiagaraScriptCompileStatus::NCS_Dirty || Status == ENiagaraScriptCompileStatus::NCS_BeingCreated)
	{
		return TEXT("Dirty");
	}
	if (Status == ENiagaraScriptCompileStatus::NCS_Error)
	{
		return TEXT("Error");
	}
	return Status == ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings || Status == ENiagaraScriptCompileStatus::NCS_ComputeUpToDateWithWarnings
		? TEXT("UpToDateWithWarnings") : TEXT("UpToDate");
}

void AddScriptCompileResult(const UNiagaraScript& Script, FObjectBuilder& Result)
{
	TArray<FScriptCompileMessage> Messages;
	const FString Status = ReadScriptCompileStatus(Script, Messages);
	TArray<FString> Errors;
	TArray<FString> Warnings;
	TArray<FValue> Items;
	for (const FScriptCompileMessage& Message : Messages)
	{
		(Message.Severity == TEXT("Error") ? Errors : Warnings).AddUnique(Message.Message);
		FObjectBuilder Item;
		Item.AddString(TEXT("severity"), Message.Severity);
		Item.AddString(TEXT("message"), Message.Message);
		if (!Message.Node.IsEmpty())
		{
			Item.AddString(TEXT("node"), Message.Node);
		}
		Items.Add(Item.Build());
	}
	Result.AddString(TEXT("status"), Status);
	Result.Add(TEXT("errors"), Graph::MakeStringArray(Errors));
	Result.Add(TEXT("warnings"), Graph::MakeStringArray(Warnings));
	Result.Add(TEXT("messages"), Graph::MakeArray(MoveTemp(Items)));
}

bool CompileScript(FCall& Call, UNiagaraScript& Script, FObjectBuilder& Result)
{
	FAssetDocumentEditorNonInteractiveScope NonInteractive;
	if (Script.IsStandaloneScript() && Script.IsCompilable())
	{
		// FNiagaraScriptViewModel::CompileStandaloneScript (the script editor's Compile): synchronous.
		Script.RequestCompile(ExposedVersion(Script), false);
	}
	if (NonInteractive.WasInteractionRefused())
	{
		NonInteractive.DiscardCreatedPackages();
		return Call.Args.Fail(TEXT("Niagara.InteractiveEditRefused"), TEXT(
			"Compiling tried to open an editor dialog, which an MCP request cannot answer."));
	}
	AddScriptCompileResult(Script, Result);
	Result.AddBool(TEXT("timed_out"), false);
	return true;
}

bool IsScriptOp(const FString& Kind)
{
	static const TSet<FString> Kinds = {TEXT("add_scratch_pad"), TEXT("delete_scratch_pad"), TEXT("rename_scratch_pad"), TEXT("refresh_scratch_pad"),
		TEXT("scratch_pad_guids"), TEXT("script_settings"), TEXT("script_parameter"), TEXT("purge_parameters")};
	return Kinds.Contains(Kind);
}

bool ApplyScriptOp(FReader& Op, const FString& Kind, UObject& Asset, FNiagaraSystemViewModel* ViewModel, bool& bOutChanged)
{
	if (Kind == TEXT("script_settings"))
	{
		return ScriptSettings(Op, Asset, bOutChanged);
	}
	if (Kind == TEXT("script_parameter"))
	{
		return ScriptParameter(Op, Asset, bOutChanged);
	}
	if (Kind == TEXT("purge_parameters"))
	{
		return PurgeParameters(Op, Asset, bOutChanged);
	}
	if (Kind == TEXT("refresh_scratch_pad"))
	{
		return RefreshScratchPad(Op, Asset, bOutChanged);
	}
	UNiagaraSystem* System = Cast<UNiagaraSystem>(&Asset);
	if (System == nullptr)
	{
		return Op.Fail(TEXT("Niagara.ScratchPadScope"), FString::Printf(TEXT("'%s' edits a system's scratch pads."), *Kind), TEXT("op"));
	}
	if (Kind == TEXT("add_scratch_pad"))
	{
		return AddScratchPad(Op, *System, ViewModel, bOutChanged);
	}
	if (Kind == TEXT("delete_scratch_pad"))
	{
		return DeleteScratchPad(Op, *System, ViewModel, bOutChanged);
	}
	if (Kind == TEXT("rename_scratch_pad"))
	{
		return RenameScratchPad(Op, *System, ViewModel, bOutChanged);
	}
	return ScratchPadGuids(Op, *System, bOutChanged);
}

bool HandleScriptEdit(FCall& Call, UNiagaraScript& Script, const FValue& Ops, FObjectBuilder& Result)
{
	FCall& C = Call;
	FAssetDocumentEditorNonInteractiveScope NonInteractive;
	const FScopedTransaction Transaction(LOCTEXT("EditNiagaraScript", "Edit Niagara Script"));
	Script.Modify();
	bool bChanged = false;
	TArray<FString> Dependents;
	bool bOk = true;
	for (int32 Index = 0; Index < Ops.Elements.Num() && bOk; ++Index)
	{
		FReader Op = C.Args.Child(FString::Printf(TEXT("ops/%d"), Index), Ops.Elements[Index]);
		FString Kind;
		if (!Op.IsObject() || !Op.String(TEXT("op"), Kind))
		{
			bOk = Op.IsObject() ? false : Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("Each op is an object."));
			break;
		}
		if (Kind == TEXT("check_dependents"))
		{
			bOk = Op.OnlyFields({TEXT("op")}) && CheckDependents(Op, Script, Dependents);
		}
		else if (Kind == TEXT("initialize_script"))
		{
			bOk = InitializeScript(Op, Script, bChanged);
		}
		else if (Kind == TEXT("versions"))
		{
			bOk = ApplyVersions(Op, Script, Script, bChanged);
		}
		else if (Kind == TEXT("update_dependents"))
		{
			bOk = Op.OnlyFields({TEXT("op")});
			if (bOk)
			{
				UpdateDependents(Script, Dependents);
			}
		}
		else if (Kind == TEXT("script_settings") || Kind == TEXT("script_parameter") || Kind == TEXT("purge_parameters"))
		{
			bOk = ApplyScriptOp(Op, Kind, Script, nullptr, bChanged);
		}
		else
		{
			bOk = Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT(
				"Unknown script op '%s' (check_dependents, initialize_script, versions, script_settings, script_parameter, "
				"purge_parameters, update_dependents)."), *Kind), TEXT("op"));
		}
		if (bOk && NonInteractive.WasInteractionRefused())
		{
			bOk = Op.Fail(TEXT("Niagara.InteractiveEditRefused"), TEXT(
				"The edit tried to open an editor dialog, which an MCP request cannot answer; the edit was refused."));
		}
	}
	if (!bOk)
	{
		NonInteractive.DiscardCreatedPackages();
		return false;
	}
	if (bChanged)
	{
		Script.MarkPackageDirty();
	}
	Result.AddBool(TEXT("changed"), bChanged);
	Result.Add(TEXT("created"), Graph::MakeArray({}));
	Dependents.Sort();
	Result.Add(TEXT("dependents"), Graph::MakeStringArray(Dependents));
	return true;
}

TSharedRef<IAssetDocumentDerivedStateRebuilder> MakeScriptRebuilder()
{
	return MakeShared<FScriptRebuilder>();
}
UNiagaraScratchPadViewModel* EnsureScratchPadViewModels(FNiagaraSystemViewModel& ViewModel)
{
	UNiagaraScratchPadViewModel* Pads = ViewModel.GetScriptScratchPadViewModel();
	UObject* Outer = nullptr;
	const TArray<TObjectPtr<UNiagaraScript>>* List = ViewModel.GetEditMode() != ENiagaraSystemViewModelEditMode::EmitterDuringMerge
		? PanelPads(ViewModel.GetSystem(), &ViewModel, Outer) : nullptr;
	if (Pads != nullptr && Pads->GetScriptViewModels().IsEmpty() && List != nullptr && !List->IsEmpty())
	{
		Pads->RefreshScriptViewModels();
		if (!Pads->GetScriptViewModels().IsEmpty())
		{
			Pads->SetActiveScriptViewModel(Pads->GetScriptViewModels()[0]);
		}
	}
	return Pads;
}

} // namespace UE::AssetDocument::Niagara

#undef LOCTEXT_NAMESPACE
