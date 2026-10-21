// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Modular rig ops of rig.controlrig.edit.v1 (docs/architecture/asset-document-controlrig-design.md §3.7, §4.4 step 9,
// phase CR4) through UModularRigController, the controller the Module Assembly panel, the module drag and drop, the
// connector widgets and the module Details use, plus the hierarchy import of "Create Control Rig" on a mesh. Undo is
// off (the edit primitive's own transaction covers the call). The modular controller reports refusals through
// LogControlRig, which the op captures; its checks (CanConnectConnectorToElements, CanBindModuleVariable,
// CanRenameModule) run first so a refusal carries the editor's own message.
//
//   module.add {name, class, parent}          AddModule ("parent" "" = a root module)
//   module.remove {name}                      DeleteModule (with its child modules, as the editor's delete)
//   module.rename {name, to}                  RenameModule (connections and bindings follow)
//   module.reparent {name, parent}            ReparentModule
//   module.reorder {name, index}              ReorderModule (index among its siblings)
//   module.swap {name, class}                 SwapModuleClass ("Swap Module Class")
//   module.config {name, path, value}         SetConfigValueInModule (value typed through the module class's property
//                                             at path) / ResetConfigValueInModule (value null: the override is cleared)
//   module.bind {name, variable, source}      BindModuleVariable ("" = UnBindModuleVariable)
//   connect {connector, targets}              ConnectConnectorToElements without auto-resolve (the document lists every
//                                             connection); the debugged rig is constructed first, as the editor's live
//                                             preview is, since the connection rules match against its hierarchy
//   disconnect {connector}                    DisconnectConnector
//   modular.settings {auto_resolve}           ModularRigSettings.bAutoResolve (the Module Assembly "Auto Resolve" toggle)
//   module.dependents {}                      on a rig module: refused while a loaded modular rig that uses it has unsaved
//                                             changes (ControlRig.DependentHasUnsavedChanges); the others are a note
//                                             (ControlRig.DependentsUpdated): the module's compile refreshes them
//   hierarchy.import {mesh}                   "Create Control Rig" / "Create Modular Rig" on a mesh or skeleton
//                                             (CreateControlRigFromSkeletalMeshOrSkeleton after the factory): ImportBones
//                                             + ImportCurves, the source imports and the preview mesh
//   modular.preview_mesh {mesh}               a modular rig's new preview mesh, as its editor handles it
//                                             (HandlePreviewMeshChanged: connectors / sockets removed, bones, curves and
//                                             sockets imported, the rig's own connectors / sockets added back); nothing
//                                             when the mesh is the preview mesh already

#include "AssetDocumentControlRig.h"
#include "AssetDocumentNameSuggestions.h"

#include "Animation/Skeleton.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "ControlRig.h"
#include "ControlRigBlueprintLegacy.h"
#include "ControlRigOverride.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/PackageName.h"
#include "ModularRig.h"
#include "ModularRigController.h"
#include "ModularRigModel.h"
#include "ModularRigRuleManager.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "UObject/UObjectIterator.h"
#include "Units/Execution/RigUnit_PrepareForExecution.h"

namespace UE::AssetDocument::Graph::Rig::ControlRig
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
// Newer modules reference a Control Rig asset (a Blueprint's generated class, or a runtime asset).
FSoftObjectPath GetModuleClassPath(const FRigModuleReference& Module) { return Module.ControlRigAssetReference.ToSoftObjectPath(); }
UClass* FindModuleClass(const FRigModuleReference& Module) { return Cast<UClass>(Module.ControlRigAssetReference.Get()); }
UClass* LoadModuleClass(const FRigModuleReference& Module) { return Cast<UClass>(Module.ControlRigAssetReference.LoadSynchronous()); }
#else
FSoftObjectPath GetModuleClassPath(const FRigModuleReference& Module) { return Module.Class.ToSoftObjectPath(); }
UClass* FindModuleClass(const FRigModuleReference& Module) { return Module.Class.Get(); }
UClass* LoadModuleClass(const FRigModuleReference& Module) { return Module.Class.LoadSynchronous(); }
#endif

namespace
{
// Errors LogControlRig reports while an op runs (UModularRigController logs its refusals).
class FLogCapture final : public FOutputDevice
{
public:
	FLogCapture() { GLog->AddOutputDevice(this); }
	virtual ~FLogCapture() override { GLog->RemoveOutputDevice(this); }
	FLogCapture(const FLogCapture&) = delete;
	FLogCapture& operator=(const FLogCapture&) = delete;

	virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override
	{
		static const FName ControlRigCategory(TEXT("LogControlRig"));
		if (Verbosity <= ELogVerbosity::Error && Category == ControlRigCategory && Message != nullptr)
		{
			FScopeLock Lock(&Guard);
			Errors.Add(Message);
		}
	}
	virtual bool CanBeUsedOnAnyThread() const override { return true; }

	FString Text()
	{
		FScopeLock Lock(&Guard);
		return FString::Join(Errors, TEXT("; ")).Left(800);
	}

private:
	FCriticalSection Guard;
	TArray<FString> Errors;
};

struct FModularEdit
{
	UControlRigBlueprint& Blueprint;
	FReader& Op;
	bool& bChanged;
	TArray<FValue>& Notes;
	int32 OpIndex = 0;

	UModularRigController* Controller() const { return Blueprint.GetModularRigController(); }
	FModularRigModel& Model() const { return Blueprint.GetModularRigModel(); }
};

bool Failed(FModularEdit& Edit, FLogCapture& Log, const TCHAR* Code, const FString& What, const TCHAR* Field = TEXT(""))
{
	const FString Reported = Log.Text();
	return Edit.Op.Fail(Code, Reported.IsEmpty() ? What + TEXT(".") : FString::Printf(TEXT("%s: %s"), *What, *Reported), Field);
}

FString ModuleCandidates(const FModularRigModel& Model, const FString& Wanted)
{
	TArray<FString> Names;
	for (const FRigModuleReference& Module : Model.Modules)
	{
		Names.Add(Module.Name.ToString());
	}
	return UE::AssetDocument::DescribeNameCandidates(Wanted, Names, TEXT("Modules"));
}

FRigModuleReference* RequireModule(FModularEdit& Edit, const FString& Name, const TCHAR* Field = TEXT("name"))
{
	FRigModuleReference* Module = Edit.Controller() != nullptr ? Edit.Controller()->FindModule(FName(*Name)) : nullptr;
	if (Module == nullptr)
	{
		Edit.Op.Fail(TEXT("ControlRig.ModuleNotFound"), FString::Printf(TEXT("The rig has no module '%s'.%s"), *Name,
			*ModuleCandidates(Edit.Model(), Name)), Field);
	}
	return Module;
}

// A rig module class from its path ("/ControlRigModules/Modules56/Arm.Arm_C", or the module asset's path).
UClass* RequireModuleClass(FModularEdit& Edit, const FString& Path, const TCHAR* Field)
{
	UClass* Class = LoadObject<UClass>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (Class == nullptr && !Path.EndsWith(TEXT("_C")))
	{
		if (const UControlRigBlueprint* Asset = LoadObject<UControlRigBlueprint>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			Class = Asset->GeneratedClass.Get();
		}
	}
	if (Class == nullptr || !Class->IsChildOf(UControlRig::StaticClass()))
	{
		Edit.Op.Fail(TEXT("ControlRig.ClassNotFound"), FString::Printf(TEXT("'%s' is not a Control Rig class (list_types kind "
			"controlrig_module lists the rig modules)."), *Path), Field);
		return nullptr;
	}
	const UControlRig* Default = Class->GetDefaultObject<UControlRig>();
	if (Default == nullptr || !Default->IsRigModule())
	{
		Edit.Op.Fail(TEXT("ControlRig.ClassNotARigModule"), FString::Printf(TEXT("%s is not a rig module (AddModule takes rig modules "
			"only; list_types kind controlrig_module lists them)."), *Class->GetPathName()), Field);
		return nullptr;
	}
	return Class;
}

bool RequireController(FModularEdit& Edit)
{
	if (!Edit.Blueprint.IsModularRig() || Edit.Controller() == nullptr)
	{
		return Edit.Op.Fail(TEXT("ControlRig.NotModular"), TEXT("Modules and connections belong to modular rigs."));
	}
	return true;
}

// The editor's live preview instance constructs every tick; the connection rules (FindMatches) and the connector
// elements the controller looks up live in that instance's hierarchy. HandlePreviewMeshChanged does the same before it
// reconnects: RequestConstruction, then the construction event.
void ConstructDebuggedRig(UControlRigBlueprint& Blueprint)
{
	if (Blueprint.UBlueprint::GetObjectBeingDebugged() == nullptr)
	{
		Blueprint.RecompileModularRig();
	}
	if (UControlRig* Debugged = Cast<UControlRig>(Blueprint.UBlueprint::GetObjectBeingDebugged()))
	{
		Debugged->RequestConstruction();
		Debugged->Execute(FRigUnit_PrepareForExecution::EventName);
	}
}

bool ParseKey(FModularEdit& Edit, const FValue& Value, const TCHAR* Field, FRigElementKey& Out)
{
	if (Value.Kind != EAssetDocumentRuntimeValueKind::String || !ParseElementKey(Value.StringValue, Out) || !Out.IsValid())
	{
		return Edit.Op.Fail(TEXT("ControlRig.InvalidValue"), TEXT("An element key is \"Type(Name)\" (\"Bone(spine_01)\")."), Field);
	}
	return true;
}

// ---- ops --------------------------------------------------------------------------------------------------------

bool OpAdd(FModularEdit& Edit)
{
	FString Name;
	FString ClassPath;
	FString Parent;
	if (!Edit.Op.String(TEXT("name"), Name) || !Edit.Op.String(TEXT("class"), ClassPath) || !Edit.Op.String(TEXT("parent"), Parent, false))
	{
		return false;
	}
	UClass* Class = RequireModuleClass(Edit, ClassPath, TEXT("class"));
	if (Class == nullptr)
	{
		return false;
	}
	if (!Parent.IsEmpty() && RequireModule(Edit, Parent, TEXT("parent")) == nullptr)
	{
		return false;
	}
	FString Reason;
	if (!Edit.Controller()->IsNameAvailable(FRigName(Name), &Reason))
	{
		return Edit.Op.Fail(TEXT("ControlRig.NameTaken"), FString::Printf(TEXT("A module cannot be named '%s': %s"), *Name, *Reason), TEXT("name"));
	}
	FLogCapture Log;
	const FName Added = Edit.Controller()->AddModule(FName(*Name), TSubclassOf<UControlRig>(Class), Parent.IsEmpty() ? NAME_None : FName(*Parent), false);
	if (Added.IsNone())
	{
		return Failed(Edit, Log, TEXT("ControlRig.EditFailed"), FString::Printf(TEXT("Module '%s' could not be added"), *Name));
	}
	if (Added.ToString() != Name)
	{
		return Edit.Op.Fail(TEXT("ControlRig.InvalidName"), FString::Printf(TEXT("'%s' is not a valid module name (the editor makes it '%s')."),
			*Name, *Added.ToString()), TEXT("name"));
	}
	Edit.bChanged = true;
	return true;
}

bool OpRemove(FModularEdit& Edit)
{
	FString Name;
	if (!Edit.Op.String(TEXT("name"), Name) || RequireModule(Edit, Name) == nullptr)
	{
		return false;
	}
	FLogCapture Log;
	// DeleteModule returns false even when it deleted (UE 5.7): the model says whether the module is gone.
	(void)Edit.Controller()->DeleteModule(FName(*Name), false);
	if (Edit.Controller()->FindModule(FName(*Name)) != nullptr)
	{
		return Failed(Edit, Log, TEXT("ControlRig.EditFailed"), FString::Printf(TEXT("Module '%s' could not be deleted"), *Name));
	}
	Edit.bChanged = true;
	return true;
}

bool OpRename(FModularEdit& Edit)
{
	FString Name;
	FString To;
	if (!Edit.Op.String(TEXT("name"), Name) || !Edit.Op.String(TEXT("to"), To) || RequireModule(Edit, Name) == nullptr)
	{
		return false;
	}
	FText Reason;
	if (!Edit.Controller()->CanRenameModule(FName(*Name), FName(*To), Reason))
	{
		return Edit.Op.Fail(TEXT("ControlRig.NameTaken"), FString::Printf(TEXT("Module '%s' cannot be renamed to '%s': %s"), *Name, *To,
			*Reason.ToString()), TEXT("to"));
	}
	FLogCapture Log;
	if (Edit.Controller()->RenameModule(FName(*Name), FName(*To), false).ToString() != To)
	{
		return Failed(Edit, Log, TEXT("ControlRig.EditFailed"), FString::Printf(TEXT("Module '%s' could not be renamed"), *Name), TEXT("to"));
	}
	Edit.bChanged = true;
	return true;
}

bool OpReparent(FModularEdit& Edit)
{
	FString Name;
	FString Parent;
	if (!Edit.Op.String(TEXT("name"), Name) || !Edit.Op.String(TEXT("parent"), Parent, false))
	{
		return false;
	}
	FRigModuleReference* Module = RequireModule(Edit, Name);
	if (Module == nullptr || (!Parent.IsEmpty() && RequireModule(Edit, Parent, TEXT("parent")) == nullptr))
	{
		return false;
	}
	const FName NewParent = Parent.IsEmpty() ? NAME_None : FName(*Parent);
	if (Module->ParentModuleName == NewParent)
	{
		return true;
	}
	// A module cannot move under its own descendants (the Module Assembly drop refuses it).
	for (const FRigModuleReference* Each = Edit.Controller()->FindModule(NewParent); Each != nullptr; Each = Each->GetParentModule())
	{
		if (Each->Name == Module->Name)
		{
			return Edit.Op.Fail(TEXT("ControlRig.ModuleCycle"), FString::Printf(TEXT("Module '%s' cannot be the child of its descendant '%s'."),
				*Name, *Parent), TEXT("parent"));
		}
	}
	FLogCapture Log;
	if (!Edit.Controller()->ReparentModule(FName(*Name), NewParent, false))
	{
		return Failed(Edit, Log, TEXT("ControlRig.EditFailed"), FString::Printf(TEXT("Module '%s' could not be reparented"), *Name), TEXT("parent"));
	}
	Edit.bChanged = true;
	return true;
}

bool OpReorder(FModularEdit& Edit)
{
	FString Name;
	int64 Index = 0;
	if (!Edit.Op.String(TEXT("name"), Name) || !Edit.Op.Int(TEXT("index"), Index) || RequireModule(Edit, Name) == nullptr)
	{
		return false;
	}
	FLogCapture Log;
	if (!Edit.Controller()->ReorderModule(FName(*Name), static_cast<int32>(Index), false))
	{
		return Failed(Edit, Log, TEXT("ControlRig.EditFailed"), FString::Printf(TEXT("Module '%s' could not be moved to sibling index %lld"),
			*Name, Index), TEXT("index"));
	}
	Edit.bChanged = true;
	return true;
}

bool OpSwap(FModularEdit& Edit)
{
	FString Name;
	FString ClassPath;
	if (!Edit.Op.String(TEXT("name"), Name) || !Edit.Op.String(TEXT("class"), ClassPath) || RequireModule(Edit, Name) == nullptr)
	{
		return false;
	}
	UClass* Class = RequireModuleClass(Edit, ClassPath, TEXT("class"));
	if (Class == nullptr)
	{
		return false;
	}
	FLogCapture Log;
	if (!Edit.Controller()->SwapModuleClass(FName(*Name), TSubclassOf<UControlRig>(Class), false))
	{
		return Failed(Edit, Log, TEXT("ControlRig.EditFailed"), FString::Printf(TEXT("Module '%s' could not be swapped to %s"), *Name,
			*Class->GetPathName()), TEXT("class"));
	}
	Edit.bChanged = true;
	return true;
}

// The config properties of a module class (public, writable variables), for not-found messages.
TArray<FString> ConfigNames(const UClass& Class)
{
	TArray<FString> Names;
	for (TFieldIterator<FProperty> It(&Class); It; ++It)
	{
		if (It->GetOwnerUObject() == &Class && !It->HasAnyPropertyFlags(CPF_BlueprintReadOnly | CPF_Transient)
			&& It->HasAnyPropertyFlags(CPF_Edit))
		{
			Names.Add(It->GetName());
		}
	}
	return Names;
}

bool OpConfig(FModularEdit& Edit)
{
	FString Name;
	FString Path;
	if (!Edit.Op.String(TEXT("name"), Name) || !Edit.Op.String(TEXT("path"), Path))
	{
		return false;
	}
	FRigModuleReference* Module = RequireModule(Edit, Name);
	if (Module == nullptr)
	{
		return false;
	}
	UClass* Class = LoadModuleClass(*Module);
	if (Class == nullptr)
	{
		return Edit.Op.Fail(TEXT("ControlRig.ClassNotFound"), FString::Printf(TEXT("The class of module '%s' does not load."), *Name), TEXT("name"));
	}
	const FValue* Value = Edit.Op.Find(TEXT("value"));
	FLogCapture Log;
	if (Value == nullptr || Value->Kind == EAssetDocumentRuntimeValueKind::Null)
	{
		if (Module->ConfigOverrides.Find(Path, Module->Name) == nullptr)
		{
			return true;
		}
		if (!Edit.Controller()->ResetConfigValueInModule(Module->Name, Path, true, false))
		{
			return Failed(Edit, Log, TEXT("ControlRig.EditFailed"), FString::Printf(TEXT("Config '%s' of module '%s' could not be reset"), *Path, *Name),
				TEXT("path"));
		}
		Edit.bChanged = true;
		return true;
	}
	FControlRigOverrideValue Override(Path, Class, Class->GetDefaultObject(), Module->Name);
	if (!Override.IsValid() || Override.GetLeafProperty() == nullptr || Override.GetData() == nullptr)
	{
		return Edit.Op.Fail(TEXT("ControlRig.ConfigNotFound"), FString::Printf(TEXT("Module class %s has no config property '%s'.%s"),
			*Class->GetPathName(), *Path, *UE::AssetDocument::DescribeNameCandidates(Path, ConfigNames(*Class), TEXT("Config"))), TEXT("path"));
	}
	if (const FControlRigOverrideValue* Current = Module->ConfigOverrides.Find(Path, Module->Name))
	{
		// Start from the current override, so a struct value that leaves fields out keeps them.
		Override = *Current;
	}
	FString Error;
	if (!DecodeProperty(*Override.GetLeafProperty(), Override.GetData(), *Value, Error))
	{
		return Edit.Op.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("Config '%s' of module '%s': %s"), *Path, *Name, *Error), TEXT("value"));
	}
	// The override keeps its value as text too (what it serializes and compares): set it from the decoded data, as the
	// Module Assembly details panel sets it from the property's exported text.
	FString Text;
	Override.GetLeafProperty()->ExportText_Direct(Text, Override.GetData(), nullptr, nullptr, PPF_None);
	if (Text != Override.ToString() && !Override.SetFromString(Text))
	{
		return Edit.Op.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("Config '%s' of module '%s': the value '%s' does not import"), *Path,
			*Name, *Text), TEXT("value"));
	}
	if (const FControlRigOverrideValue* Current = Module->ConfigOverrides.Find(Path, Module->Name);
		Current != nullptr && Current->Identical(Override))
	{
		return true;
	}
	if (!Edit.Controller()->SetConfigValueInModule(Module->Name, Override, false))
	{
		return Failed(Edit, Log, TEXT("ControlRig.ConfigRefused"), FString::Printf(TEXT("Config '%s' of module '%s' was refused"), *Path, *Name),
			TEXT("path"));
	}
	Edit.bChanged = true;
	return true;
}

bool OpBind(FModularEdit& Edit)
{
	FString Name;
	FString Variable;
	FString Source;
	if (!Edit.Op.String(TEXT("name"), Name) || !Edit.Op.String(TEXT("variable"), Variable) || !Edit.Op.String(TEXT("source"), Source, false))
	{
		return false;
	}
	FRigModuleReference* Module = RequireModule(Edit, Name);
	if (Module == nullptr)
	{
		return false;
	}
	const FString* Current = Module->Bindings.Find(FName(*Variable));
	FLogCapture Log;
	if (Source.IsEmpty())
	{
		if (Current == nullptr)
		{
			return true;
		}
		if (!Edit.Controller()->UnBindModuleVariable(Module->Name, FName(*Variable), false))
		{
			return Failed(Edit, Log, TEXT("ControlRig.EditFailed"), FString::Printf(TEXT("Variable '%s' of module '%s' could not be unbound"),
				*Variable, *Name), TEXT("variable"));
		}
		Edit.bChanged = true;
		return true;
	}
	if (Current != nullptr && *Current == Source)
	{
		return true;
	}
	FText Reason;
	if (!Edit.Controller()->CanBindModuleVariable(Module->Name, FName(*Variable), Source, Reason))
	{
		const TArray<FString> Possible = Edit.Controller()->GetPossibleBindings(Module->Name, FName(*Variable));
		return Edit.Op.Fail(TEXT("ControlRig.BindingRefused"), FString::Printf(TEXT("Variable '%s' of module '%s' cannot be bound to '%s': %s%s"),
			*Variable, *Name, *Source, *Reason.ToString(), *UE::AssetDocument::DescribeNameCandidates(Source, Possible, TEXT("Possible sources"))),
			TEXT("source"));
	}
	if (!Edit.Controller()->BindModuleVariable(Module->Name, FName(*Variable), Source, false))
	{
		return Failed(Edit, Log, TEXT("ControlRig.BindingRefused"), FString::Printf(TEXT("Variable '%s' of module '%s' could not be bound"),
			*Variable, *Name), TEXT("source"));
	}
	Edit.bChanged = true;
	return true;
}

// The connectors a module's class exposes, as connector keys ("<Module>/<Connector>").
TArray<FString> ConnectorNames(const FModularRigModel& Model)
{
	TArray<FString> Names;
	for (const FRigModuleReference& Module : Model.Modules)
	{
		const UClass* Class = FindModuleClass(Module);
		const UControlRig* Default = Class != nullptr ? Class->GetDefaultObject<UControlRig>() : nullptr;
		if (Default != nullptr)
		{
			for (const FRigModuleConnector& Connector : Default->GetRigModuleSettings().ExposedConnectors)
			{
				Names.Add(FString::Printf(TEXT("%s/%s"), *Module.Name.ToString(), *Connector.Name));
			}
		}
	}
	return Names;
}

bool OpConnect(FModularEdit& Edit)
{
	FString Connector;
	const FValue* Targets = nullptr;
	if (!Edit.Op.String(TEXT("connector"), Connector) || !Edit.Op.Array(TEXT("targets"), Targets))
	{
		return false;
	}
	const FRigElementKey ConnectorKey(FName(*Connector), ERigElementType::Connector);
	if (!ConnectorNames(Edit.Model()).Contains(Connector))
	{
		return Edit.Op.Fail(TEXT("ControlRig.ConnectorNotFound"), FString::Printf(TEXT("No module exposes connector '%s' (a connection key is "
			"\"<module>/<connector>\").%s"), *Connector, *UE::AssetDocument::DescribeNameCandidates(Connector, ConnectorNames(Edit.Model()),
			TEXT("Connectors"))), TEXT("connector"));
	}
	TArray<FRigElementKey> Keys;
	for (int32 Index = 0; Index < Targets->Elements.Num(); ++Index)
	{
		FRigElementKey Key;
		if (!ParseKey(Edit, Targets->Elements[Index], *FString::Printf(TEXT("targets/%d"), Index), Key))
		{
			return false;
		}
		Keys.Add(Key);
	}
	if (Keys.IsEmpty())
	{
		return Edit.Op.Fail(TEXT("ControlRig.InvalidValue"), TEXT("A connection names at least one target."), TEXT("targets"));
	}
	if (Edit.Model().Connections.FindTargetsFromConnector(ConnectorKey) == Keys)
	{
		return true;
	}
	ConstructDebuggedRig(Edit.Blueprint);
	FText Reason;
	if (!Edit.Controller()->CanConnectConnectorToElements(ConnectorKey, Keys, Reason))
	{
		FString Hint;
		if (const UModularRig* Debugged = Cast<UModularRig>(Edit.Blueprint.UBlueprint::GetObjectBeingDebugged()))
		{
			TArray<FString> Elements;
			for (const FRigElementKey& Each : Debugged->GetHierarchy()->GetAllKeys())
			{
				Elements.Add(ElementKeyText(Each));
			}
			for (const FRigElementKey& Key : Keys)
			{
				if (!Debugged->GetHierarchy()->Contains(Key))
				{
					Hint += FString::Printf(TEXT(" The rig has no element %s.%s"), *ElementKeyText(Key),
						*UE::AssetDocument::DescribeNameCandidates(ElementKeyText(Key), Elements, TEXT("Elements")));
					break;
				}
			}
		}
		return Edit.Op.Fail(TEXT("ControlRig.ConnectionRefused"), FString::Printf(TEXT("%s cannot connect to %s: %s.%s"), *Connector,
			*FString::JoinBy(Keys, TEXT(", "), [](const FRigElementKey& Key) { return ElementKeyText(Key); }), *Reason.ToString(), *Hint), TEXT("targets"));
	}
	FLogCapture Log;
	if (!Edit.Controller()->ConnectConnectorToElements(ConnectorKey, Keys, false, false, true))
	{
		return Failed(Edit, Log, TEXT("ControlRig.ConnectionRefused"), FString::Printf(TEXT("%s could not be connected"), *Connector), TEXT("targets"));
	}
	Edit.bChanged = true;
	return true;
}

// How a connector's match was picked: the only match, or the module's default among several (the others listed).
FString DescribePick(const FModularRigResolveResult& Result, const FRigElementKey& Picked)
{
	const TArray<FRigElementResolveResult>& Matches = Result.GetMatches();
	if (Matches.Num() <= 1)
	{
		return TEXT("the only match");
	}
	TArray<FString> Others;
	for (const FRigElementResolveResult& Match : Matches)
	{
		if (Match.GetKey() != Picked)
		{
			if (Others.Num() == 8)
			{
				Others.Add(FString::Printf(TEXT("+%d more"), Matches.Num() - 9));
				break;
			}
			Others.Add(ElementKeyText(Match.GetKey()));
		}
	}
	return FString::Printf(TEXT("the module's default of %d matches (others: %s)"), Matches.Num(), *FString::Join(Others, TEXT(", ")));
}

// The matches of a connector on the constructed debugged rig (empty when the connector is not there).
FModularRigResolveResult MatchesOf(UControlRigBlueprint& Blueprint, const FRigElementKey& Key)
{
	ConstructDebuggedRig(Blueprint);
	UModularRig* Debugged = Cast<UModularRig>(Blueprint.UBlueprint::GetObjectBeingDebugged());
	URigHierarchy* Hierarchy = Debugged != nullptr ? Debugged->GetHierarchy() : nullptr;
	const FRigConnectorElement* Connector = Hierarchy != nullptr ? Hierarchy->Find<FRigConnectorElement>(Key) : nullptr;
	if (Connector == nullptr || Hierarchy->GetRuleManager() == nullptr)
	{
		return FModularRigResolveResult();
	}
	const FRigModuleInstance* Instance = Debugged->FindModule(Hierarchy->GetModuleFName(Key));
	return Hierarchy->GetRuleManager()->FindMatches(Connector, Instance, Debugged->GetElementKeyRedirector());
}

// Why an auto-connect suggestion deserves a look, or empty: a bone above where the module attaches (its primary
// connector's bone, or the bone its socket sits on) lies outside the module's own chain. The editor's drop makes the
// same pick: an Arm dropped on a Spine shoulder socket (no Shoulder module) takes the clavicle as its Elbow Bone.
FString AboveAttach(UControlRigBlueprint& Blueprint, const FRigElementKey& Connector, const FRigElementKey& Pick)
{
	UModularRig* Debugged = Cast<UModularRig>(Blueprint.UBlueprint::GetObjectBeingDebugged());
	URigHierarchy* Hierarchy = Debugged != nullptr ? Debugged->GetHierarchy() : nullptr;
	const FRigConnectorElement* Element = Hierarchy != nullptr ? Hierarchy->Find<FRigConnectorElement>(Connector) : nullptr;
	if (Element == nullptr || Element->IsPrimary() || Pick.Type != ERigElementType::Bone)
	{
		return FString();
	}
	const FRigModuleReference* Module = Blueprint.GetModularRigModel().FindModule(Hierarchy->GetModuleFName(Connector));
	const FRigConnectorElement* Primary = Module != nullptr ? Module->FindPrimaryConnector(Hierarchy) : nullptr;
	if (Primary == nullptr)
	{
		return FString();
	}
	const FRigElementKey Target = Blueprint.GetModularRigModel().Connections.FindTargetFromConnector(Primary->GetKey());
	const FRigElementKey Attach = Target.Type == ERigElementType::Bone ? Target : Hierarchy->GetFirstParent(Target);
	if (!Attach.IsValid() || Attach == Pick || !Hierarchy->IsParentedTo(Attach, Pick))
	{
		return FString();
	}
	return FString::Printf(TEXT("above where the module attaches (%s%s), outside its own chain: check it"), *ElementKeyText(Target),
		Target == Attach ? TEXT("") : *FString::Printf(TEXT(" on %s"), *ElementKeyText(Attach)));
}

bool OpDisconnect(FModularEdit& Edit)
{
	FString Connector;
	if (!Edit.Op.String(TEXT("connector"), Connector))
	{
		return false;
	}
	const FRigElementKey ConnectorKey(FName(*Connector), ERigElementType::Connector);
	if (Edit.Model().Connections.FindTargetsFromConnector(ConnectorKey).IsEmpty())
	{
		return true;
	}
	FLogCapture Log;
	if (!Edit.Controller()->DisconnectConnector(ConnectorKey, false, false))
	{
		return Failed(Edit, Log, TEXT("ControlRig.EditFailed"), FString::Printf(TEXT("%s could not be disconnected"), *Connector), TEXT("connector"));
	}
	Edit.bChanged = true;
	return true;
}

bool OpSettings(FModularEdit& Edit)
{
	bool bAutoResolve = true;
	if (!Edit.Op.Bool(TEXT("auto_resolve"), bAutoResolve, true))
	{
		return false;
	}
	FModularRigSettings& Settings = Edit.Blueprint.GetModularRigSettings();
	if (Settings.bAutoResolve == bAutoResolve)
	{
		return true;
	}
	static_cast<UObject&>(Edit.Blueprint).Modify();
	Settings.bAutoResolve = bAutoResolve;
	Edit.bChanged = true;
	return true;
}

// Modular rigs that use this rig module (design §5.4). The asset refreshes every loaded modular rig that uses a module
// when the module compiles (IControlRigAssetInterface::OnModularDependencyChanged), as the editor does: a modular rig
// with unsaved changes would get the refresh mixed into its own edits, so the apply is refused; the others are reported.
bool OpDependents(FModularEdit& Edit)
{
	const UClass* Generated = Edit.Blueprint.GeneratedClass.Get();
	if (Generated == nullptr || !Edit.Blueprint.IsControlRigModule())
	{
		return true;
	}
	const FSoftObjectPath ClassPath(Generated);
	TArray<FString> Unsaved;
	TArray<FString> Loaded;
	for (TObjectIterator<UControlRigBlueprint> It; It; ++It)
	{
		UControlRigBlueprint* Other = *It;
		if (Other == &Edit.Blueprint || !IsValid(Other) || Other->HasAnyFlags(RF_ClassDefaultObject | RF_Transient)
			|| Other->GetOutermost() == GetTransientPackage() || !Other->IsModularRig())
		{
			continue;
		}
		const bool bUses = Other->GetModularRigModel().Modules.ContainsByPredicate([&ClassPath](const FRigModuleReference& Module)
		{
			return GetModuleClassPath(Module) == ClassPath;
		});
		if (bUses)
		{
			(Other->GetPackage()->IsDirty() ? Unsaved : Loaded).Add(Other->GetPackage()->GetName());
		}
	}
	Unsaved.Sort();
	Loaded.Sort();
	if (!Unsaved.IsEmpty())
	{
		return Edit.Op.Fail(TEXT("ControlRig.DependentHasUnsavedChanges"), FString::Printf(TEXT("Modular rigs that use this module have "
			"unsaved changes: %s. Editing the module refreshes them (as the editor does when the module compiles); save or revert them first."),
			*FString::Join(Unsaved, TEXT(", "))));
	}
	if (!Loaded.IsEmpty())
	{
		FObjectBuilder Note;
		Note.AddInt(TEXT("op"), Edit.OpIndex);
		Note.AddString(TEXT("field"), FString());
		Note.AddString(TEXT("code"), TEXT("ControlRig.DependentsUpdated"));
		Note.AddString(TEXT("message"), FString::Printf(TEXT("Loaded modular rigs use this module and take its new version when it "
			"compiles, as in the editor (they may be left with unsaved changes; save them to keep it): %s."), *FString::Join(Loaded, TEXT(", "))));
		Edit.Notes.Add(Note.Build());
	}
	return true;
}

// A mesh or skeleton by object path or, as the editor's asset picker resolves a package, by package path (the
// package's Skeletal Mesh or Skeleton).
UObject* LoadMeshOrSkeleton(const FString& Path)
{
	if (UObject* Found = LoadObject<UObject>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet))
	{
		return Found;
	}
	if (Path.Contains(TEXT(".")) || !FPackageName::IsValidLongPackageName(Path))
	{
		return nullptr;
	}
	TArray<FAssetData> Assets;
	IAssetRegistry::GetChecked().GetAssetsByPackageName(FName(*Path), Assets);
	for (const FAssetData& Asset : Assets)
	{
		if (Asset.IsInstanceOf(USkeletalMesh::StaticClass()) || Asset.IsInstanceOf(USkeleton::StaticClass()))
		{
			return Asset.GetAsset();
		}
	}
	return nullptr;
}

bool OpHierarchyImport(FModularEdit& Edit, bool bPreviewMesh)
{
	FString Path;
	if (!Edit.Op.String(TEXT("mesh"), Path))
	{
		return false;
	}
	UObject* Source = LoadMeshOrSkeleton(Path);
	USkeletalMesh* Mesh = Cast<USkeletalMesh>(Source);
	USkeleton* Skeleton = Mesh != nullptr ? Mesh->GetSkeleton() : Cast<USkeleton>(Source);
	if (Skeleton == nullptr)
	{
		return Edit.Op.Fail(TEXT("ControlRig.UnknownMesh"), FString::Printf(TEXT("'%s' is not a Skeletal Mesh or Skeleton (with a skeleton)."), *Path),
			TEXT("mesh"));
	}
	URigHierarchy* Hierarchy = Edit.Blueprint.GetHierarchy();
	URigHierarchyController* Controller = Hierarchy != nullptr ? Hierarchy->GetController(true) : nullptr;
	if (Controller == nullptr)
	{
		return Edit.Op.Fail(TEXT("ControlRig.EditFailed"), TEXT("The rig has no hierarchy."));
	}
	FLogCapture Log;
	if (!bPreviewMesh)
	{
		// FControlRigBlueprintActions::CreateControlRigFromSkeletalMeshOrSkeleton after the factory (modular or not).
		const FReferenceSkeleton& Reference = Mesh != nullptr ? Mesh->GetRefSkeleton() : Skeleton->GetReferenceSkeleton();
		Controller->ImportBones(Reference, NAME_None, false, false, false, false);
		if (Mesh != nullptr)
		{
			Controller->ImportCurvesFromSkeletalMesh(Mesh, NAME_None, false, false);
		}
		else
		{
			Controller->ImportCurves(Skeleton, NAME_None, false, false);
		}
		static_cast<UObject&>(Edit.Blueprint).Modify();
		Edit.Blueprint.GetSourceHierarchyImport() = Skeleton;
		Edit.Blueprint.GetSourceCurveImport() = Skeleton;
		Edit.Blueprint.PropagateHierarchyFromBPToInstances();
		if (Mesh != nullptr)
		{
			Edit.Blueprint.SetPreviewMesh(Mesh, true);
		}
		Edit.bChanged = true;
		return true;
	}
	// A modular rig's new preview mesh: FControlRigBaseEditor::HandlePreviewMeshChanged.
	if (!Edit.Blueprint.IsModularRig())
	{
		return Edit.Op.Fail(TEXT("ControlRig.NotModular"), TEXT("modular.preview_mesh is the preview mesh change of a modular rig."));
	}
	if (Mesh == nullptr)
	{
		return Edit.Op.Fail(TEXT("ControlRig.UnknownMesh"), TEXT("A modular rig imports its hierarchy from a Skeletal Mesh (its preview mesh)."), TEXT("mesh"));
	}
	if (Edit.Blueprint.GetPreviewMesh() == Mesh)
	{
		return true; // the preview mesh is already this one (the editor imports on a change only)
	}
	static_cast<UObject&>(Edit.Blueprint).Modify();
	Edit.Blueprint.SetPreviewMesh(Mesh, true);
	{
		// The editor also suspends the asset's notifications here (a protected member it is a friend of); with auto VM
		// recompile off for the edit, the end state is the same.
		TArray<FRigElementKey> ConnectorsAndSockets = Hierarchy->GetConnectorKeys();
		ConnectorsAndSockets.Append(Hierarchy->GetSocketKeys());
		TArray<TTuple<FRigElementKey, FRigElementKey, FTransform>> Manual;
		for (const FRigElementKey& Key : ConnectorsAndSockets)
		{
			if (Hierarchy->GetModuleFName(Key).IsNone())
			{
				Manual.Emplace(Key, Hierarchy->GetDefaultParent(Key), Hierarchy->GetLocalTransform(Key));
			}
			(void)Controller->RemoveElement(Key, false, false);
		}
		Controller->ImportBones(Skeleton, NAME_None, true, true, false, false, false);
		Controller->ImportCurvesFromSkeletalMesh(Mesh, NAME_None, false, false, false);
		Controller->ImportSocketsFromSkeletalMesh(Mesh, NAME_None, true, true, false, false, false);
		for (const TTuple<FRigElementKey, FRigElementKey, FTransform>& Entry : Manual)
		{
			const FRigElementKey& Key = Entry.Get<0>();
			const FRigElementKey& Parent = Entry.Get<1>();
			if (!Parent.IsValid() || Hierarchy->Contains(Parent))
			{
				if (Key.Type == ERigElementType::Socket)
				{
					Controller->AddSocket(Key.Name, Parent, Entry.Get<2>(), false, FLinearColor::White, FString(), false);
				}
				else if (Key.Type == ERigElementType::Connector)
				{
					Controller->AddConnector(Key.Name, FRigConnectorSettings(), false);
				}
			}
		}
	}
	Edit.Blueprint.PropagateHierarchyFromBPToInstances();
	if (UControlRig* Debugged = Cast<UControlRig>(Edit.Blueprint.UBlueprint::GetObjectBeingDebugged()))
	{
		Debugged->GetHierarchy()->Notify(ERigHierarchyNotification::HierarchyReset, {});
		Debugged->Initialize(true);
	}
	Edit.bChanged = true;
	return true;
}
} // namespace

bool IsModularOp(const FString& Kind)
{
	return (Kind.StartsWith(TEXT("module.")) && Kind != TEXT("module.settings")) || Kind == TEXT("connect") || Kind == TEXT("disconnect")
		|| Kind == TEXT("modular.settings") || Kind == TEXT("hierarchy.import") || Kind == TEXT("modular.preview_mesh");
}

const TArray<FString>* ModularOpFields(const FString& Kind)
{
	static const TMap<FString, TArray<FString>> Fields = {
		{TEXT("module.add"), {TEXT("name"), TEXT("class"), TEXT("parent")}}, {TEXT("module.remove"), {TEXT("name")}},
		{TEXT("module.rename"), {TEXT("name"), TEXT("to")}}, {TEXT("module.reparent"), {TEXT("name"), TEXT("parent")}},
		{TEXT("module.reorder"), {TEXT("name"), TEXT("index")}}, {TEXT("module.swap"), {TEXT("name"), TEXT("class")}},
		{TEXT("module.config"), {TEXT("name"), TEXT("path"), TEXT("value")}},
		{TEXT("module.bind"), {TEXT("name"), TEXT("variable"), TEXT("source")}},
		{TEXT("connect"), {TEXT("connector"), TEXT("targets")}}, {TEXT("disconnect"), {TEXT("connector")}},
		{TEXT("modular.settings"), {TEXT("auto_resolve")}}, {TEXT("hierarchy.import"), {TEXT("mesh")}},
		{TEXT("modular.preview_mesh"), {TEXT("mesh")}},
		{TEXT("module.dependents"), {}}};
	return Fields.Find(Kind);
}

bool RunModularOp(UControlRigBlueprint& Blueprint, FReader& Op, const FString& Kind, bool& bChanged, TArray<FValue>& Notes, int32 OpIndex)
{
	FModularEdit Edit{Blueprint, Op, bChanged, Notes, OpIndex};
	if (Kind == TEXT("hierarchy.import") || Kind == TEXT("modular.preview_mesh"))
	{
		return OpHierarchyImport(Edit, Kind == TEXT("modular.preview_mesh"));
	}
	if (Kind == TEXT("module.dependents"))
	{
		return OpDependents(Edit);
	}
	if (!RequireController(Edit))
	{
		return false;
	}
	if (Kind == TEXT("module.add")) { return OpAdd(Edit); }
	if (Kind == TEXT("module.remove")) { return OpRemove(Edit); }
	if (Kind == TEXT("module.rename")) { return OpRename(Edit); }
	if (Kind == TEXT("module.reparent")) { return OpReparent(Edit); }
	if (Kind == TEXT("module.reorder")) { return OpReorder(Edit); }
	if (Kind == TEXT("module.swap")) { return OpSwap(Edit); }
	if (Kind == TEXT("module.config")) { return OpConfig(Edit); }
	if (Kind == TEXT("module.bind")) { return OpBind(Edit); }
	if (Kind == TEXT("connect")) { return OpConnect(Edit); }
	if (Kind == TEXT("disconnect")) { return OpDisconnect(Edit); }
	if (Kind == TEXT("modular.settings")) { return OpSettings(Edit); }
	return Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("Unknown op '%s'."), *Kind), TEXT("op"));
}

namespace
{
// The possible targets of the connectors still unconnected in Model, on a constructed instance of the rig: primary
// connectors (the editor never resolves them: a module is dropped onto one) and the secondary connectors auto-resolve
// leaves; a secondary connector of a module whose primary connector is unresolved goes to Waiting.
void ListCandidates(UModularRig& Rig, const FModularRigModel& Model, FObjectBuilder& Candidates, TArray<FString>& Waiting)
{
	URigHierarchy* Hierarchy = Rig.GetHierarchy();
	const UModularRigRuleManager* Rules = Hierarchy != nullptr ? Hierarchy->GetRuleManager() : nullptr;
	if (Rules == nullptr)
	{
		return;
	}
	for (const FRigModuleReference& Module : Model.Modules)
	{
		const FRigModuleInstance* Instance = Rig.FindModule(Module.Name);
		const FRigConnectorElement* Primary = Module.FindPrimaryConnector(Hierarchy);
		const bool bPrimaryResolved = Primary != nullptr && Model.Connections.HasConnection(Primary->GetKey());
		// Keys first: matching may construct the rig again, which replaces the connector elements.
		TArray<FRigElementKey> Keys;
		for (const FRigConnectorElement* Connector : Module.FindConnectors(Hierarchy))
		{
			Keys.Add(Connector->GetKey());
		}
		for (const FRigElementKey& Key : Keys)
		{
			const FRigConnectorElement* Connector = Hierarchy->Find<FRigConnectorElement>(Key);
			if (Connector == nullptr || Model.Connections.HasConnection(Key))
			{
				continue;
			}
			if (!Connector->IsPrimary() && !bPrimaryResolved)
			{
				Waiting.Add(Key.Name.ToString());
				continue;
			}
			const FModularRigResolveResult Result = Rules->FindMatches(Connector, Instance, Rig.GetElementKeyRedirector());
			const TArray<FRigElementResolveResult>& Matches = Result.GetMatches();
			TArray<FString> Targets;
			for (const FRigElementResolveResult& Match : Matches)
			{
				if (Targets.Num() == 12)
				{
					Targets.Add(FString::Printf(TEXT("(+%d more)"), Matches.Num() - 12));
					break;
				}
				Targets.Add(ElementKeyText(Match.GetKey()));
			}
			Candidates.Add(Key.Name.ToString(), MakeStringArray(Targets));
		}
	}
}

// Modules whose primary connector is connected and that have another connector still unconnected: what the editor's
// auto-resolve (AutoConnectModules) works on.
TArray<FName> ModulesToAutoConnect(const FModularRigModel& Model)
{
	TArray<FName> Names;
	for (const FRigModuleReference& Module : Model.Modules)
	{
		const UClass* Class = FindModuleClass(Module);
		const UControlRig* Default = Class != nullptr ? Class->GetDefaultObject<UControlRig>() : nullptr;
		if (Default == nullptr)
		{
			continue;
		}
		bool bPrimaryConnected = false;
		bool bOpen = false;
		for (const FRigModuleConnector& Connector : Default->GetRigModuleSettings().ExposedConnectors)
		{
			const bool bConnected = Model.Connections.HasConnection(
				FRigElementKey(*FString::Printf(TEXT("%s/%s"), *Module.Name.ToString(), *Connector.Name), ERigElementType::Connector));
			bPrimaryConnected |= Connector.IsPrimary() && bConnected;
			bOpen |= !Connector.IsPrimary() && !bConnected;
		}
		if (bPrimaryConnected && bOpen)
		{
			Names.Add(Module.Name);
		}
	}
	return Names;
}
double RoundTo(double Value, double Step)
{
	const double Rounded = FMath::RoundToDouble(Value / Step) * Step;
	return Rounded == 0.0 ? 0.0 : Rounded;
}

// What the modules build (Inspect of a modular rig; the asset's own hierarchy is only the mesh's): a new instance of
// the compiled rig, constructed as the editor's preview is, and the initial global transform of each element in a
// module's namespace (controls, nulls, sockets; 0.001 cm, 1e-5 quaternion), plus the hierarchy's element count.
void AddModuleElements(UControlRigBlueprint& Blueprint, FObjectBuilder& Facts)
{
	UClass* Class = Blueprint.GeneratedClass.Get();
	if (Class == nullptr || !Class->IsChildOf(UModularRig::StaticClass()))
	{
		return;
	}
	UModularRig* Rig = NewObject<UModularRig>(GetTransientPackage(), Class, NAME_None, RF_Transient);
	Rig->Initialize(true);
	Rig->RequestConstruction();
	Rig->Execute(FRigUnit_PrepareForExecution::EventName);
	FObjectBuilder Globals;
	int32 Count = 0;
	if (URigHierarchy* Hierarchy = Rig->GetHierarchy())
	{
		for (const FRigElementKey& Key : Hierarchy->GetAllKeys(true))
		{
			++Count;
			if (Key.Type == ERigElementType::Connector || Key.Type == ERigElementType::Curve || Hierarchy->GetModuleFName(Key).IsNone())
			{
				continue;
			}
			const FTransform Global = Hierarchy->GetGlobalTransform(Key, true);
			const FVector T = Global.GetTranslation();
			const FQuat Q = Global.GetRotation();
			const FVector S = Global.GetScale3D();
			auto Components = [](std::initializer_list<TPair<const TCHAR*, double>> Values)
			{
				FObjectBuilder Object;
				for (const TPair<const TCHAR*, double>& Value : Values)
				{
					Object.Add(Value.Key, MakeNumber(Value.Value));
				}
				return Object.Build();
			};
			FObjectBuilder Entry;
			if (!T.IsNearlyZero(5e-4))
			{
				Entry.Add(TEXT("Translation"), Components({{TEXT("X"), RoundTo(T.X, 1e-3)}, {TEXT("Y"), RoundTo(T.Y, 1e-3)}, {TEXT("Z"), RoundTo(T.Z, 1e-3)}}));
			}
			if (!Q.Equals(FQuat::Identity, 5e-6))
			{
				Entry.Add(TEXT("Rotation"), Components({{TEXT("X"), RoundTo(Q.X, 1e-5)}, {TEXT("Y"), RoundTo(Q.Y, 1e-5)}, {TEXT("Z"), RoundTo(Q.Z, 1e-5)},
					{TEXT("W"), RoundTo(Q.W, 1e-5)}}));
			}
			if (!S.Equals(FVector::OneVector, 5e-5))
			{
				Entry.Add(TEXT("Scale3D"), Components({{TEXT("X"), RoundTo(S.X, 1e-4)}, {TEXT("Y"), RoundTo(S.Y, 1e-4)}, {TEXT("Z"), RoundTo(S.Z, 1e-4)}}));
			}
			Globals.Add(ElementKeyText(Key), Entry.Build());
		}
	}
	Rig->MarkAsGarbage();
	Facts.Add(TEXT("elements"), Globals.Build());
	Facts.AddInt(TEXT("element_count"), Count);
}
} // namespace

// Inspect's ModularRigSuggestions (design §3.8). AutoConnect: the connections the editor's auto-resolve makes after a
// module's primary connector is connected (UModularRigController::AutoConnectModules: each secondary connector with one
// match or a default target, one after the other, the rig constructed again after each), run on a transient copy of
// the rig (Validate's trial target), so the asset and its debugged instance are untouched. Candidates: the possible
// targets of the connectors left unconnected once AutoConnect is accepted (primary connectors always: the editor
// connects them where a module is dropped). Without anything to auto-connect, a transient instance of the compiled rig
// lists the candidates.
FValue AutoConnectFacts(UControlRigBlueprint& Blueprint)
{
	FObjectBuilder Facts;
	UClass* Class = Blueprint.GeneratedClass.Get();
	if (!Blueprint.IsModularRig() || Class == nullptr || !Class->IsChildOf(UModularRig::StaticClass()))
	{
		return Facts.Build();
	}
	const FModularRigModel& Model = Blueprint.GetModularRigModel();
	FObjectBuilder Suggested;
	FObjectBuilder Candidates;
	FObjectBuilder Check;
	TArray<FString> Waiting;
	const TArray<FName> Ready = ModulesToAutoConnect(Model);
	FString Message;
	UControlRigBlueprint* Trial = nullptr;
	if (!Ready.IsEmpty())
	{
		UPackage* Package = Blueprint.GetOutermost();
		const bool bWasDirty = Package->IsDirty();
		Trial = MakeTransientCopy(Blueprint, Message);
		Package->SetDirtyFlag(bWasDirty);
	}
	if (Trial != nullptr)
	{
		ConstructDebuggedRig(*Trial);
		if (UModularRigController* Controller = Trial->GetModularRigController())
		{
			(void)Controller->AutoConnectModules(Ready, false, false);
		}
		const FModularRigModel& TrialModel = Trial->GetModularRigModel();
		TArray<TPair<FRigElementKey, FRigElementKey>> Picked;
		for (const FModularRigSingleConnection& Connection : TrialModel.Connections.GetConnectionList())
		{
			if (!Model.Connections.HasConnection(Connection.Connector) && Connection.Targets.Num() == 1)
			{
				Suggested.AddString(Connection.Connector.Name.ToString(), ElementKeyText(Connection.Targets[0]));
				Picked.Emplace(Connection.Connector, Connection.Targets[0]);
			}
		}
		// Picks above where their module attaches (an Arm on a Spine socket takes the clavicle as its Elbow Bone, as the
		// editor's drop does): Check says why, with the other matches.
		ConstructDebuggedRig(*Trial);
		for (const TPair<FRigElementKey, FRigElementKey>& Pick : Picked)
		{
			const FString Reason = AboveAttach(*Trial, Pick.Key, Pick.Value);
			if (!Reason.IsEmpty())
			{
				Check.AddString(Pick.Key.Name.ToString(), FString::Printf(TEXT("%s (%s)"), *Reason, *DescribePick(MatchesOf(*Trial, Pick.Key), Pick.Value)));
			}
		}
		ConstructDebuggedRig(*Trial);
		if (UModularRig* Rig = Cast<UModularRig>(Trial->UBlueprint::GetObjectBeingDebugged()))
		{
			ListCandidates(*Rig, TrialModel, Candidates, Waiting);
		}
	}
	else
	{
		UModularRig* Rig = NewObject<UModularRig>(GetTransientPackage(), Class, NAME_None, RF_Transient);
		Rig->Initialize(true);
		Rig->RequestConstruction();
		Rig->Execute(FRigUnit_PrepareForExecution::EventName);
		ListCandidates(*Rig, Model, Candidates, Waiting);
		Rig->MarkAsGarbage();
	}
	Facts.Add(TEXT("auto_connect"), Suggested.Build());
	Facts.Add(TEXT("candidates"), Candidates.Build());
	Facts.Add(TEXT("check"), Check.Build());
	Facts.Add(TEXT("unresolved_primary"), MakeStringArray(Waiting));
	AddModuleElements(Blueprint, Facts);
	if (!Message.IsEmpty())
	{
		Facts.AddString(TEXT("error"), Message);
	}
	return Facts.Build();
}

void ConstructModularPreview(UControlRigBlueprint& Blueprint)
{
	if (Blueprint.IsModularRig())
	{
		ConstructDebuggedRig(Blueprint);
	}
}
} // namespace UE::AssetDocument::Graph::Rig::ControlRig
