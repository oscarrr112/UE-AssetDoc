// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// The parameter panel of a module / function / dynamic input script after a reload. Each time the editor loads such a
// graph, its script variables (the panel's entries with their defaults and metadata) are brought in line with the
// parameters the graph references:
//   - a Map Get's output and a Map Set's input parameter pins (sub-category "ParameterPin"),
//   - a static switch's parameter (not one set by a compiler constant or by a pin),
//   - the static switch parameters a function call propagates,
//   - transitively, the parameters those default-bind to (DefaultMode Binding);
// entries for anything else are dropped and missing entries are created with default metadata. An apply brings the
// graph to the same state right after its edits so that the document reads the same before and after a reload (the
// editor itself only does it on load: a parameter whose last pin was removed stays in the panel until then).
// Behaviour tests: AssetDocumentNiagaraScriptApplyTests.cpp ("ManageScriptVariables" cases compare with a saved and
// reloaded copy).

#include "AssetDocumentNiagaraPrimitives.h"

#include "Algo/AnyOf.h"
#include "Algo/Copy.h"
#include "Algo/Transform.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_Niagara.h"
#include "NiagaraConstants.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScriptVariable.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::Niagara
{
namespace
{
const FName ParameterPinSubCategory(TEXT("ParameterPin"));

bool IsClass(const UObject& Object, const TCHAR* ClassName)
{
	for (const UClass* Class = Object.GetClass(); Class != nullptr; Class = Class->GetSuperClass())
	{
		if (Class->GetName() == ClassName)
		{
			return true;
		}
	}
	return false;
}

template <typename T>
const T* Member(const UStruct& Struct, const void* Container, const TCHAR* Name)
{
	const FProperty* Property = FindFProperty<FProperty>(&Struct, Name);
	return Property != nullptr ? Property->ContainerPtrToValuePtr<T>(Container) : nullptr;
}

// UNiagaraNodeStaticSwitch (private class): the parameter of a switch the user sets (not a compiler constant, a
// pin or the debug switch), read through reflection: FNiagaraVariable(GetInputType(), InputParameterName).
bool StaticSwitchParameter(const UEdGraphNode& Node, FNiagaraVariable& OutVariable)
{
	const UClass& Class = *Node.GetClass();
	const FName* Name = Member<FName>(Class, &Node, TEXT("InputParameterName"));
	const FStructProperty* DataProperty = FindFProperty<FStructProperty>(&Class, TEXT("SwitchTypeData"));
	if (Name == nullptr || DataProperty == nullptr)
	{
		return false;
	}
	const void* Data = DataProperty->ContainerPtrToValuePtr<void>(&Node);
	const UScriptStruct& DataStruct = *DataProperty->Struct;
	const uint8* SwitchType = Member<uint8>(DataStruct, Data, TEXT("SwitchType"));
	const TObjectPtr<UEnum>* Enum = Member<TObjectPtr<UEnum>>(DataStruct, Data, TEXT("Enum"));
	const FName* SwitchConstant = Member<FName>(DataStruct, Data, TEXT("SwitchConstant"));
	const bool* bExposeAsPin = Member<bool>(DataStruct, Data, TEXT("bExposeAsPin"));
	if (SwitchType == nullptr || Enum == nullptr || SwitchConstant == nullptr || bExposeAsPin == nullptr)
	{
		return false;
	}
	constexpr uint8 Bool = 0;
	constexpr uint8 Integer = 1;
	constexpr uint8 EnumType = 2;
	const bool bSetByCompiler = !SwitchConstant->IsNone();
	const bool bDebug = *SwitchType == EnumType && *SwitchConstant == FName(TEXT("Function.DebugState"))
		&& Enum->Get() == FNiagaraTypeDefinition::GetFunctionDebugStateEnum();
	const bool bSetByPin = *bExposeAsPin && !bSetByCompiler && !bDebug;
	if (bSetByCompiler || bSetByPin)
	{
		return false;
	}
	FNiagaraTypeDefinition Type;
	if (*SwitchType == Bool)
	{
		Type = FNiagaraTypeDefinition::GetBoolDef();
	}
	else if (*SwitchType == Integer)
	{
		Type = FNiagaraTypeDefinition::GetIntDef();
	}
	else if (*SwitchType == EnumType && Enum->Get() != nullptr)
	{
		Type = FNiagaraTypeDefinition(Enum->Get());
	}
	OutVariable = FNiagaraVariable(Type, *Name);
	return true;
}

// The parameters one node references directly.
void CollectNodeParameters(const UEdGraphNode& Node, TArray<FNiagaraVariable>& Out)
{
	if (IsClass(Node, TEXT("NiagaraNodeStaticSwitch")))
	{
		FNiagaraVariable Switch;
		if (StaticSwitchParameter(Node, Switch))
		{
			Out.Add(Switch);
		}
		return;
	}
	if (const UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(&Node))
	{
		Algo::Transform(Call->PropagatedStaticSwitchParameters, Out, [](const FNiagaraPropagatedVariable& Propagated)
		{
			return Propagated.ToVariable();
		});
		return;
	}
	// A Map Get reads its output parameter pins, a Map Set writes its input parameter pins.
	EEdGraphPinDirection ParameterSide = EGPD_MAX;
	if (IsClass(Node, TEXT("NiagaraNodeParameterMapGet")))
	{
		ParameterSide = EGPD_Output;
	}
	else if (IsClass(Node, TEXT("NiagaraNodeParameterMapSet")))
	{
		ParameterSide = EGPD_Input;
	}
	else
	{
		return;
	}
	for (const UEdGraphPin* Pin : Node.Pins)
	{
		if (Pin != nullptr && Pin->Direction == ParameterSide && Pin->PinType.PinSubCategory == ParameterPinSubCategory)
		{
			Out.Add(UEdGraphSchema_Niagara::PinToNiagaraVariable(Pin, false));
		}
	}
}

// The parameter a referenced parameter's entry defaults to (DefaultMode Binding), if any. A vector bound to a name
// that is a position parameter today is read as a position, as old assets store it.
bool DefaultBindingTarget(const UNiagaraGraph::FScriptVariableMap& Entries, const FNiagaraVariable& Parameter, FNiagaraVariable& OutTarget)
{
	const TObjectPtr<UNiagaraScriptVariable>* Entry = Entries.Find(Parameter);
	const UNiagaraScriptVariable* Variable = Entry != nullptr ? Entry->Get() : nullptr;
	if (Variable == nullptr || Variable->DefaultMode != ENiagaraDefaultMode::Binding || !Variable->DefaultBinding.IsValid())
	{
		return false;
	}
	const FName TargetName = Variable->DefaultBinding.GetName();
	const FNiagaraTypeDefinition& DeclaredType = Variable->Variable.GetType();
	const bool bLegacyPosition = FNiagaraConstants::GetOldPositionTypeVariables().Contains(FNiagaraVariable(DeclaredType, TargetName));
	OutTarget = FNiagaraVariable(bLegacyPosition ? FNiagaraTypeDefinition::GetPositionDef() : DeclaredType, TargetName);
	return true;
}

// Every parameter the graph references: the nodes' own parameters, closed over default bindings.
TSet<FNiagaraVariable> ReferencedVariables(const UNiagaraGraph& Graph)
{
	TArray<FNiagaraVariable> Worklist;
	for (const UEdGraphNode* Node : Graph.Nodes)
	{
		if (Node != nullptr)
		{
			CollectNodeParameters(*Node, Worklist);
		}
	}
	const UNiagaraGraph::FScriptVariableMap& Entries = Graph.GetAllMetaData();
	TSet<FNiagaraVariable> Referenced;
	for (int32 Cursor = 0; Cursor < Worklist.Num(); ++Cursor)
	{
		bool bKnown = false;
		Referenced.Add(Worklist[Cursor], &bKnown);
		FNiagaraVariable Target;
		if (!bKnown && DefaultBindingTarget(Entries, Worklist[Cursor], Target))
		{
			Worklist.Add(Target);
		}
	}
	return Referenced;
}

// Only scripts used as a module, function or dynamic input keep a parameter panel of their own.
bool HasParameterPanel(UNiagaraGraph& Graph)
{
	TArray<UNiagaraNodeOutput*> Outputs;
	Graph.GetNodesOfClass(Outputs);
	return Algo::AnyOf(Outputs, [](const UNiagaraNodeOutput* Output)
	{
		switch (Output->GetUsage())
		{
		case ENiagaraScriptUsage::Module:
		case ENiagaraScriptUsage::Function:
		case ENiagaraScriptUsage::DynamicInput:
			return true;
		default:
			return false;
		}
	});
}

// A new panel entry with default metadata and the type's default value. Module inputs are always written by the
// module, so they default to their value; any other parameter must be set before the script reads it.
UNiagaraScriptVariable* NewPanelEntry(UNiagaraGraph& Graph, const FNiagaraVariable& Parameter)
{
	UNiagaraScriptVariable* Entry = NewObject<UNiagaraScriptVariable>(&Graph, NAME_None, RF_Transactional);
	Entry->Init(Parameter, FNiagaraVariableMetaData());
	Entry->SetIsStaticSwitch(false);
	FNiagaraEditorUtilities::ResetVariableToDefaultValue(Entry->Variable);
	const bool bModuleInput = Entry->Variable.IsInNameSpace(FNiagaraConstants::ModuleNamespace);
	Entry->DefaultMode = bModuleInput ? ENiagaraDefaultMode::Value : ENiagaraDefaultMode::FailIfPreviouslyNotSet;
	return Entry;
}
} // namespace

bool ManageScriptVariables(UNiagaraGraph& Graph, TArray<FString>* OutRemoved, TArray<FString>* OutCreated)
{
	const TSet<FNiagaraVariable> Referenced = HasParameterPanel(Graph) ? ReferencedVariables(Graph) : TSet<FNiagaraVariable>();
	UNiagaraGraph::FScriptVariableMap& Entries = Graph.GetAllMetaData();
	TArray<FNiagaraVariable> Stale;
	for (const TPair<FNiagaraVariable, TObjectPtr<UNiagaraScriptVariable>>& Entry : Entries)
	{
		if (!Referenced.Contains(Entry.Key))
		{
			Stale.Add(Entry.Key);
		}
	}
	TArray<FNiagaraVariable> Missing;
	Algo::CopyIf(Referenced, Missing, [&Entries](const FNiagaraVariable& Parameter) { return !Entries.Contains(Parameter); });
	if (Stale.IsEmpty() && Missing.IsEmpty())
	{
		return false;
	}

	Graph.Modify();
	for (const FNiagaraVariable& Parameter : Stale)
	{
		Entries.Remove(Parameter);
		if (OutRemoved != nullptr)
		{
			OutRemoved->Add(Parameter.GetName().ToString());
		}
	}
	for (const FNiagaraVariable& Parameter : Missing)
	{
		Entries.Add(Parameter, NewPanelEntry(Graph, Parameter));
		if (OutCreated != nullptr)
		{
			OutCreated->Add(Parameter.GetName().ToString());
		}
	}
	Graph.NotifyGraphChanged();
	return true;
}
} // namespace UE::AssetDocument::Niagara
