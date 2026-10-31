// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Function ops of rig.controlrig.edit.v1 (docs/architecture/asset-document-controlrig-design.md §3.5, §4.1, §5.4):
// the local function library through its controller (what My Blueprint's Functions "+", rename, delete
// and the function's Details "Access" / category / keywords / description / colour do), the exposed pins of a function
// or collapse graph and the local variables of a function through that graph's controller (the Details panel's Inputs
// / Outputs and Local Variables), and the dependents report of public functions. Undo and Python print are off (the
// edit primitive's transaction covers the call); controller reporting is on so refusals fail the op.
//
// A function or collapse graph is named as the read's scopes are: ["Functions", <function>] or a graph scope ending in
// ["Nodes", <collapse node>, "Collapse"] (FGraphTarget). Ops:
//   function.add {name, mutable}      AddFunctionToLibrary (mutable: the editor's "New Function", with an execute pin)
//   function.remove {name}  function.rename {name, to}
//   function.set {graph, public?, color?, category?, keywords?, description?, tags?}   public / color / tags (the
//       Variant tags, in order, by name; a new one from the project's tag list): functions only
//   function.pin.add {graph, name, direction, type, default?}   direction Input / Output / IO
//   function.pin.remove {graph, name}  function.pin.rename {graph, name, to}  function.pin.type {graph, name, type}
//   function.pin.index {graph, name, index}   index among all exposed pins
//   function.pin.default {graph, name, value} a function's argument default (null resets)
//   function.layout {graph, categories?: [{name, pins?, expanded?}], display_names?: {pin: label}}   the Details panel's
//       pin layout: categories in order (unlisted ones removed), each category's pins in order, pin labels ("" clears);
//       written while node layouts are off too (RigVM.EnableNodeLayouts is turned on for the op and restored)
//   local.add {graph, name, type, default?}  local.remove {graph, name}  local.rename {graph, name, to}
//   local.type {graph, name, type}  local.default {graph, name, value}
//   function.dependents {functions: [{name, change}]}   notes ControlRig.DependentsAffected for each public function
//       other packages reference (warn only: other packages are never edited, decision §5.4)

#include "AssetDocumentControlRig.h"
#include "AssetDocumentNameSuggestions.h"

#include "ControlRigBlueprintLegacy.h"
#include "HAL/IConsoleManager.h"
#include "Misc/EngineVersionComparison.h"
#include "RigVMCore/RigVMExecuteContext.h"
#include "RigVMCore/RigVMNodeLayout.h"
#include "RigVMCore/RigVMStruct.h"
#include "RigVMCore/RigVMVariant.h"
#include "RigVMModel/Nodes/RigVMCollapseNode.h"
#include "RigVMModel/Nodes/RigVMFunctionReferenceNode.h"
#include "RigVMModel/Nodes/RigVMLibraryNode.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMFunctionLibrary.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMPin.h"
#include "RigVMModel/RigVMSchema.h"
#include "RigVMSettings.h"

namespace UE::AssetDocument::Graph::Rig::ControlRig
{
namespace
{
URigVMFunctionLibrary* LibraryOf(UControlRigBlueprint& Blueprint)
{
	return static_cast<URigVMBlueprint&>(Blueprint).GetLocalFunctionLibrary();
}

bool RequireLibrary(FGraphEditState& State, FReader& Op, URigVMFunctionLibrary*& OutLibrary, URigVMController*& OutController)
{
	OutLibrary = LibraryOf(State.Blueprint);
	OutController = GetLibraryController(State.Blueprint);
	if (OutLibrary == nullptr || OutController == nullptr)
	{
		return Op.Fail(TEXT("ControlRig.EditFailed"), TEXT("The rig has no function library."));
	}
	return true;
}

// The function or collapse graph an op edits, the library / collapse node that owns it and the controller of the
// graph that node is in (the library for a function).
struct FInterfaceTarget
{
	FGraphTarget Inner;
	URigVMCollapseNode* Owner = nullptr;
	URigVMController* OuterController = nullptr;
	bool bFunction = false;
};

bool ReadInterfaceTarget(FGraphEditState& State, FReader& Op, FInterfaceTarget& Out)
{
	if (!ReadGraphField(State, Op, Out.Inner))
	{
		return false;
	}
	Out.Owner = Cast<URigVMCollapseNode>(Out.Inner.Graph->GetOuter());
	if (Out.Owner == nullptr || Out.Owner->GetGraph() == nullptr)
	{
		return Op.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("'%s' is an event graph: exposed pins, local variables and "
			"headers belong to functions and collapse nodes."), *Out.Inner.GraphName), TEXT("graph"));
	}
	Out.bFunction = Out.Owner->GetGraph()->IsA<URigVMFunctionLibrary>();
	Out.OuterController = static_cast<URigVMBlueprint&>(State.Blueprint).GetOrCreateController(Out.Owner->GetGraph());
	if (Out.OuterController == nullptr)
	{
		return Op.Fail(TEXT("ControlRig.EditFailed"), FString::Printf(TEXT("The graph of '%s' has no controller."), *Out.Owner->GetName()),
			TEXT("graph"));
	}
	return true;
}

bool ReadType(FReader& Op, const TCHAR* Field, FString& OutCPPType, UObject*& OutTypeObject)
{
	FString Type;
	if (!Op.String(Field, Type))
	{
		return false;
	}
	if (!ParseTypeName(Type, OutCPPType, OutTypeObject) || OutCPPType.IsEmpty())
	{
		return Op.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("'%s' is not a RigVM type (list_types kind rigvm_type)."), *Type),
			Field);
	}
	return true;
}

FName TypeObjectPath(const UObject* TypeObject)
{
	return TypeObject != nullptr ? FName(*TypeObject->GetPathName()) : NAME_None;
}

FTextValues& ValuesOf(FGraphEditState& State)
{
	if (!State.Values.IsValid())
	{
		State.Values = MakeUnique<FTextValues>();
	}
	return *State.Values;
}

// A document value as export text of the type ("" for an execute / wildcard type or no value).
// bKeepText: a string the type does not import is kept as the value's text. Local variable defaults are the rig's
// text, which for some types (arrays of structs saved by earlier engine versions) does not import into the type; the
// read keeps such a default as that text, and writing it back unchanged keeps the rig's value.
bool DecodeValue(FGraphEditState& State, FReader& Op, const TCHAR* Field, const FString& CPPType, UObject* TypeObject, const FString& Current,
	FString& OutText, bool bKeepText = false)
{
	const FValue* Value = Op.Find(Field);
	OutText = Current;
	if (Value == nullptr || Value->Kind == EAssetDocumentRuntimeValueKind::Null)
	{
		return true;
	}
	if (IsValuelessType(CPPType, TypeObject))
	{
		return Op.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("A %s has no value."), *TypeName(CPPType, TypeObject)), Field);
	}
	FString Error;
	if (!ValuesOf(State).Decode(CPPType, TypeObject, Current, *Value, OutText, Error))
	{
		if (bKeepText && Value->Kind == EAssetDocumentRuntimeValueKind::String)
		{
			OutText = Value->StringValue;
			return true;
		}
		return Op.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("%s: %s"), *TypeName(CPPType, TypeObject), *Error), Field);
	}
	return true;
}

bool ParseDirection(FReader& Op, ERigVMPinDirection& Out)
{
	FString Text;
	if (!Op.String(TEXT("direction"), Text))
	{
		return false;
	}
	if (Text == TEXT("Input"))
	{
		Out = ERigVMPinDirection::Input;
	}
	else if (Text == TEXT("Output"))
	{
		Out = ERigVMPinDirection::Output;
	}
	else if (Text == TEXT("IO"))
	{
		Out = ERigVMPinDirection::IO;
	}
	else
	{
		return Op.Fail(TEXT("ControlRig.InvalidValue"), TEXT("direction is Input, Output or IO (an execute pin)."), TEXT("direction"));
	}
	return true;
}

URigVMPin* RequireExposedPin(FReader& Op, const FInterfaceTarget& Target, const FString& Name, const TCHAR* Field = TEXT("name"))
{
	URigVMPin* Pin = Target.Owner->FindPin(Name);
	if (Pin == nullptr)
	{
		TArray<FString> Names;
		for (const URigVMPin* Each : Target.Owner->GetPins())
		{
			Names.Add(Each->GetName());
		}
		Op.Fail(TEXT("ControlRig.PinNotFound"), FString::Printf(TEXT("'%s' has no exposed pin '%s'.%s"), *Target.Owner->GetName(), *Name,
			*UE::AssetDocument::DescribeNameCandidates(Name, Names, TEXT("Pins"))), Field);
	}
	return Pin;
}

bool HasLocal(const URigVMGraph& Graph, const FName& Name)
{
	for (const FRigVMGraphVariableDescription& Local : Graph.GetLocalVariables(false))
	{
		if (Local.Name == Name)
		{
			return true;
		}
	}
	return false;
}

// ---- functions ------------------------------------------------------------------------------------------------

bool OpFunctionAdd(FGraphEditState& State, FReader& Op)
{
	FString Name;
	bool bMutable = false;
	URigVMFunctionLibrary* Library = nullptr;
	URigVMController* Controller = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("mutable")}) || !Op.String(TEXT("name"), Name) || !Op.Bool(TEXT("mutable"), bMutable)
		|| !RequireLibrary(State, Op, Library, Controller))
	{
		return false;
	}
	if (Name.IsEmpty() || Library->FindFunction(FName(*Name)) != nullptr)
	{
		return Op.Fail(TEXT("ControlRig.NameTaken"), FString::Printf(TEXT("The rig already has a function '%s'."), *Name), TEXT("name"));
	}
	const FReportingScope Reporting(*Controller);
	const URigVMLibraryNode* Function = Controller->AddFunctionToLibrary(FName(*Name), bMutable, FVector2D::ZeroVector, false, false);
	if (!CheckReported(State, Op, FString::Printf(TEXT("Adding function '%s'"), *Name), TEXT("ControlRig.EditRefused"), TEXT("name")))
	{
		return false;
	}
	if (Function == nullptr)
	{
		return Op.Fail(TEXT("ControlRig.EditFailed"), FString::Printf(TEXT("Function '%s' could not be added."), *Name), TEXT("name"));
	}
	State.bChanged = true;
	if (Function->GetName() != Name)
	{
		return Op.Fail(TEXT("ControlRig.InvalidNodeName"), FString::Printf(TEXT("The new function was named '%s', not '%s' (function names "
			"are node names: letters, digits, spaces and '_')."), *Function->GetName(), *Name), TEXT("name"));
	}
	return true;
}

bool OpFunctionRemove(FGraphEditState& State, FReader& Op)
{
	FString Name;
	URigVMFunctionLibrary* Library = nullptr;
	URigVMController* Controller = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name")}) || !Op.String(TEXT("name"), Name) || !RequireLibrary(State, Op, Library, Controller))
	{
		return false;
	}
	if (Library->FindFunction(FName(*Name)) == nullptr)
	{
		return Op.Fail(TEXT("ControlRig.FunctionNotFound"), FString::Printf(TEXT("The rig has no function '%s'."), *Name), TEXT("name"));
	}
	const FReportingScope Reporting(*Controller);
	const bool bRemoved = Controller->RemoveFunctionFromLibrary(FName(*Name), false, false);
	if (!CheckReported(State, Op, FString::Printf(TEXT("Removing function '%s'"), *Name)))
	{
		return false;
	}
	if (!bRemoved)
	{
		return Op.Fail(TEXT("ControlRig.EditFailed"), FString::Printf(TEXT("Function '%s' could not be removed."), *Name), TEXT("name"));
	}
	State.bChanged = true;
	return true;
}

bool OpFunctionRename(FGraphEditState& State, FReader& Op)
{
	FString Name;
	FString To;
	URigVMFunctionLibrary* Library = nullptr;
	URigVMController* Controller = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("to")}) || !Op.String(TEXT("name"), Name) || !Op.String(TEXT("to"), To)
		|| !RequireLibrary(State, Op, Library, Controller))
	{
		return false;
	}
	if (Library->FindFunction(FName(*Name)) == nullptr)
	{
		return Op.Fail(TEXT("ControlRig.FunctionNotFound"), FString::Printf(TEXT("The rig has no function '%s'."), *Name), TEXT("name"));
	}
	if (To.IsEmpty() || Library->FindFunction(FName(*To)) != nullptr)
	{
		return Op.Fail(TEXT("ControlRig.NameTaken"), FString::Printf(TEXT("The rig already has a function '%s'."), *To), TEXT("to"));
	}
	const FReportingScope Reporting(*Controller);
	Controller->RenameFunction(FName(*Name), FName(*To), false, false);
	if (!CheckReported(State, Op, FString::Printf(TEXT("Renaming function '%s'"), *Name), TEXT("ControlRig.EditRefused"), TEXT("to")))
	{
		return false;
	}
	State.bChanged = true;
	if (Library->FindFunction(FName(*To)) == nullptr)
	{
		return Op.Fail(TEXT("ControlRig.InvalidNodeName"), FString::Printf(TEXT("Function '%s' could not be renamed to '%s' (function names are "
			"node names: letters, digits, spaces and '_')."), *Name, *To), TEXT("to"));
	}
	return true;
}

// The function's Variant tags as the Details panel's Variant row edits them (the library controller's
// AddDefaultTagToFunctionVariant / RemoveTagFromFunctionVariant on the function's name): tags left out are removed, a
// new tag comes from the project's tag list (Project Settings > RigVM), and an order the appends cannot reach re-adds
// the tags in order (each keeps its own data).
bool SetVariantTags(FGraphEditState& State, FReader& Op, const FInterfaceTarget& Target)
{
	const FValue* Value = nullptr;
	if (!Op.Array(TEXT("tags"), Value))
	{
		return false;
	}
	if (!Target.bFunction)
	{
		return Op.Fail(TEXT("ControlRig.InvalidValue"), TEXT("Variant tags belong to functions."), TEXT("tags"));
	}
	TArray<FName> Wanted;
	for (int32 Index = 0; Index < Value->Elements.Num(); ++Index)
	{
		const FValue& Item = Value->Elements[Index];
		if (Item.Kind != EAssetDocumentRuntimeValueKind::String || Item.StringValue.IsEmpty())
		{
			return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("tags lists tag names."), *FString::Printf(TEXT("tags/%d"), Index));
		}
		Wanted.AddUnique(FName(*Item.StringValue));
	}
	URigVMController& Controller = *Target.OuterController;
	const FName Function(*Target.Owner->GetFunctionHeader().LibraryPointer.GetFunctionName());
	const TArray<FRigVMTag> Current = Target.Owner->GetFunctionVariant().Tags;
	const URigVMProjectSettings* Settings = GetDefault<URigVMProjectSettings>();
	TArray<FString> Known;
	for (const FRigVMTag& Tag : Settings->VariantTags)
	{
		Known.Add(Tag.Name.ToString());
	}
	for (int32 Index = 0; Index < Wanted.Num(); ++Index)
	{
		const FName Name = Wanted[Index];
		if (!Current.ContainsByPredicate([Name](const FRigVMTag& Tag) { return Tag.Name == Name; }) && Settings->FindTag(Name) == nullptr)
		{
			return Op.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("'%s' is not a variant tag of this project (Project Settings > "
				"RigVM > Variant Tags).%s"), *Name.ToString(), *UE::AssetDocument::DescribeNameCandidates(Name.ToString(), Known, TEXT("Tags"))),
				*FString::Printf(TEXT("tags/%d"), Index));
		}
	}
	TArray<FName> Remaining;
	for (const FRigVMTag& Tag : Current)
	{
		if (!Wanted.Contains(Tag.Name))
		{
			Controller.RemoveTagFromFunctionVariant(Function, Tag.Name, false, false);
		}
		else
		{
			Remaining.Add(Tag.Name);
		}
	}
	TArray<FName> Appended = Remaining;
	for (const FName& Name : Wanted)
	{
		if (!Remaining.Contains(Name))
		{
			Appended.Add(Name);
		}
	}
	if (Appended != Wanted)
	{
		for (const FName& Name : Remaining)
		{
			Controller.RemoveTagFromFunctionVariant(Function, Name, false, false);
		}
		Remaining.Reset();
	}
	for (const FName& Name : Wanted)
	{
		if (Remaining.Contains(Name))
		{
			continue;
		}
		if (const FRigVMTag* Own = Current.FindByPredicate([Name](const FRigVMTag& Tag) { return Tag.Name == Name; }))
		{
			Controller.AddTagToFunctionVariant(Function, *Own, false, false);
		}
		else
		{
			Controller.AddDefaultTagToFunctionVariant(Function, Name, false, false);
		}
	}
	return CheckReported(State, Op, TEXT("The variant tags"), TEXT("ControlRig.InvalidValue"), TEXT("tags"));
}

bool OpFunctionSet(FGraphEditState& State, FReader& Op)
{
	FInterfaceTarget Target;
	if (!Op.OnlyFields({TEXT("op"), TEXT("graph"), TEXT("public"), TEXT("color"), TEXT("category"), TEXT("keywords"), TEXT("description"),
			TEXT("tags")})
		|| !ReadInterfaceTarget(State, Op, Target))
	{
		return false;
	}
	// SetNodeColor takes a color; there is no reset, so a null is refused rather than dropped.
	if (!Op.RefuseNull(TEXT("color"), TEXT("ControlRig.InvalidValue"), TEXT("give {R, G, B, A}, or omit color to keep the current one.")))
	{
		return false;
	}
	URigVMController& Controller = *Target.OuterController;
	const FReportingScope Reporting(Controller);
	URigVMCollapseNode* Node = Target.Owner;
	if (Op.Has(TEXT("public")))
	{
		bool bPublic = false;
		if (!Op.Bool(TEXT("public"), bPublic))
		{
			return false;
		}
		if (!Target.bFunction)
		{
			return Op.Fail(TEXT("ControlRig.InvalidValue"), TEXT("Public belongs to functions; a collapse node is part of its graph."), TEXT("public"));
		}
		// The Details panel's Access: the asset's MarkFunctionPublic, which calls this on the library controller.
		Controller.MarkFunctionAsPublic(Node->GetFName(), bPublic, false, false);
		if (!CheckReported(State, Op, TEXT("The function's access"), TEXT("ControlRig.EditRefused"), TEXT("public")))
		{
			return false;
		}
	}
	if (Op.Has(TEXT("color")))
	{
		const FValue* Value = Op.Find(TEXT("color"));
		FLinearColor Color = FLinearColor::White;
		FString Error = TEXT("not an object");
		if (Value == nullptr || Value->Kind != EAssetDocumentRuntimeValueKind::Object
			|| !DecodeStructFields(*TBaseStructure<FLinearColor>::Get(), &Color, *Value, [](const FProperty&) { return false; }, Error))
		{
			return Op.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("color is {R, G, B, A}: %s"), *Error), TEXT("color"));
		}
		Controller.SetNodeColor(Node, Color, false, false, false);
		if (!CheckReported(State, Op, TEXT("The color"), TEXT("ControlRig.InvalidValue"), TEXT("color")))
		{
			return false;
		}
	}
	FString Text;
	if (Op.Has(TEXT("category")))
	{
		if (!Op.String(TEXT("category"), Text))
		{
			return false;
		}
		Controller.SetNodeCategory(Node, Text, false, false, false);
		if (!CheckReported(State, Op, TEXT("The category"), TEXT("ControlRig.InvalidValue"), TEXT("category")))
		{
			return false;
		}
	}
	if (Op.Has(TEXT("keywords")))
	{
		if (!Op.String(TEXT("keywords"), Text))
		{
			return false;
		}
		Controller.SetNodeKeywords(Node, Text, false, false, false);
		if (!CheckReported(State, Op, TEXT("The keywords"), TEXT("ControlRig.InvalidValue"), TEXT("keywords")))
		{
			return false;
		}
	}
	if (Op.Has(TEXT("description")))
	{
		if (!Op.String(TEXT("description"), Text))
		{
			return false;
		}
		Controller.SetNodeDescription(Node, Text, false, false, false);
		if (!CheckReported(State, Op, TEXT("The description"), TEXT("ControlRig.InvalidValue"), TEXT("description")))
		{
			return false;
		}
	}
	if (Op.Has(TEXT("tags")) && !SetVariantTags(State, Op, Target))
	{
		return false;
	}
	State.bChanged = true;
	return true;
}

// ---- exposed pins ---------------------------------------------------------------------------------------------

const URigVMPin* FindExecuteContextPin(const URigVMCollapseNode& Owner)
{
	for (const URigVMPin* Pin : Owner.GetPins())
	{
		if (Pin->IsExecuteContext())
		{
			return Pin;
		}
	}
	return nullptr;
}

bool IsExecuteType(const UObject* TypeObject)
{
	const UScriptStruct* Struct = Cast<UScriptStruct>(TypeObject);
	return Struct != nullptr && Struct->IsChildOf(FRigVMExecutePin::StaticStruct());
}

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
// Engines whose functions take their execute pin only from the function's mutability (the library's SetFunctionIsPure,
// which adds an IO pin named ExecuteContext and removes the pin of that name): the pin is added that way and renamed,
// and renamed back before it is removed.
bool AddFunctionExecutePin(FGraphEditState& State, FReader& Op, const FInterfaceTarget& Target, const FString& Name, ERigVMPinDirection Direction)
{
	if (const URigVMPin* Existing = FindExecuteContextPin(*Target.Owner))
	{
		return Op.Fail(TEXT("ControlRig.EditRefused"), FString::Printf(TEXT("Pin '%s' could not be added: the editor gives a function or "
			"collapse node one execute pin, and '%s' has '%s'."), *Name, *Target.Owner->GetName(), *Existing->GetName()), TEXT("name"));
	}
	if (Direction != ERigVMPinDirection::IO)
	{
		return Op.Fail(TEXT("ControlRig.EditRefused"), FString::Printf(TEXT("Pin '%s' could not be added: a function's execute pin is IO "
			"(the editor adds it when the function becomes mutable)."), *Name), TEXT("direction"));
	}
	{
		const FReportingScope Reporting(*Target.OuterController);
		Target.OuterController->SetFunctionIsPure(Target.Owner->GetFName(), false, false, false);
		if (!CheckReported(State, Op, FString::Printf(TEXT("Adding pin '%s'"), *Name), TEXT("ControlRig.EditRefused"), TEXT("name")))
		{
			return false;
		}
	}
	const URigVMPin* Added = FindExecuteContextPin(*Target.Owner);
	if (Added == nullptr)
	{
		return Op.Fail(TEXT("ControlRig.EditFailed"), FString::Printf(TEXT("Pin '%s' could not be added."), *Name), TEXT("name"));
	}
	State.bChanged = true;
	if (Added->GetName() != Name)
	{
		const FReportingScope Reporting(*Target.Inner.Controller);
		const bool bRenamed = Target.Inner.Controller->RenameExposedPin(Added->GetFName(), FName(*Name), false, false);
		if (!CheckReported(State, Op, FString::Printf(TEXT("Adding pin '%s'"), *Name), TEXT("ControlRig.EditRefused"), TEXT("name")))
		{
			return false;
		}
		if (!bRenamed || Target.Owner->FindPin(Name) == nullptr)
		{
			return Op.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("The new pin was named '%s', not '%s' (pin names are "
				"letters, digits and '_')."), *Added->GetName(), *Name), TEXT("name"));
		}
	}
	return true;
}

bool RemoveFunctionExecutePin(FGraphEditState& State, FReader& Op, const FInterfaceTarget& Target, const FString& Name)
{
	const FName ExecuteContextName = FRigVMStruct::ExecuteContextName;
	if (FName(*Name) != ExecuteContextName)
	{
		if (Target.Owner->FindPin(ExecuteContextName.ToString()) != nullptr)
		{
			return Op.Fail(TEXT("ControlRig.EditRefused"), FString::Printf(TEXT("Pin '%s' could not be removed: the editor removes a "
				"function's execute pin by making the function pure, and another pin of '%s' is named '%s'."), *Name,
				*Target.Owner->GetName(), *ExecuteContextName.ToString()), TEXT("name"));
		}
		const FReportingScope Reporting(*Target.Inner.Controller);
		Target.Inner.Controller->RenameExposedPin(FName(*Name), ExecuteContextName, false, false);
		if (!CheckReported(State, Op, FString::Printf(TEXT("Removing pin '%s'"), *Name)))
		{
			return false;
		}
	}
	const FReportingScope Reporting(*Target.OuterController);
	Target.OuterController->SetFunctionIsPure(Target.Owner->GetFName(), true, false, false);
	if (!CheckReported(State, Op, FString::Printf(TEXT("Removing pin '%s'"), *Name)))
	{
		return false;
	}
	State.bChanged = true;
	if (FindExecuteContextPin(*Target.Owner) != nullptr)
	{
		return Op.Fail(TEXT("ControlRig.EditFailed"), FString::Printf(TEXT("Pin '%s' could not be removed."), *Name), TEXT("name"));
	}
	return true;
}
#endif

bool OpExposedPinAdd(FGraphEditState& State, FReader& Op)
{
	FInterfaceTarget Target;
	FString Name;
	ERigVMPinDirection Direction = ERigVMPinDirection::Input;
	FString CPPType;
	UObject* TypeObject = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("graph"), TEXT("name"), TEXT("direction"), TEXT("type"), TEXT("default")})
		|| !ReadInterfaceTarget(State, Op, Target) || !Op.String(TEXT("name"), Name) || !ParseDirection(Op, Direction)
		|| !ReadType(Op, TEXT("type"), CPPType, TypeObject))
	{
		return false;
	}
	if (Target.Owner->FindPin(Name) != nullptr)
	{
		return Op.Fail(TEXT("ControlRig.NameTaken"), FString::Printf(TEXT("'%s' already has a pin '%s'."), *Target.Owner->GetName(), *Name),
			TEXT("name"));
	}
	FString Default;
	if (Op.Has(TEXT("default")) && !Target.bFunction)
	{
		return Op.Fail(TEXT("ControlRig.InvalidValue"), TEXT("A collapse node's pins take their values on the node (Defaults)."), TEXT("default"));
	}
	if (!DecodeValue(State, Op, TEXT("default"), CPPType, TypeObject, FString(), Default))
	{
		return false;
	}
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	if (Target.bFunction && IsExecuteType(TypeObject))
	{
		return AddFunctionExecutePin(State, Op, Target, Name, Direction);
	}
#endif
	const FReportingScope Reporting(*Target.Inner.Controller);
	const FName Added = Target.Inner.Controller->AddExposedPin(FName(*Name), Direction, CPPType, TypeObjectPath(TypeObject), Default, false, false);
	if (!CheckReported(State, Op, FString::Printf(TEXT("Adding pin '%s'"), *Name), TEXT("ControlRig.EditRefused"), TEXT("name")))
	{
		return false;
	}
	if (Added.IsNone())
	{
		const URigVMPin* Existing = FindExecuteContextPin(*Target.Owner);
		if (Existing != nullptr && IsExecuteType(TypeObject))
		{
			return Op.Fail(TEXT("ControlRig.EditRefused"), FString::Printf(TEXT("Pin '%s' could not be added: the editor gives a function or "
				"collapse node one execute pin, and '%s' has '%s'."), *Name, *Target.Owner->GetName(), *Existing->GetName()), TEXT("name"));
		}
		return Op.Fail(TEXT("ControlRig.EditFailed"), FString::Printf(TEXT("Pin '%s' could not be added."), *Name), TEXT("name"));
	}
	State.bChanged = true;
	if (Added.ToString() != Name)
	{
		return Op.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("The new pin was named '%s', not '%s' (pin names are letters, "
			"digits and '_')."), *Added.ToString(), *Name), TEXT("name"));
	}
	return true;
}

bool OpExposedPinRemove(FGraphEditState& State, FReader& Op)
{
	FInterfaceTarget Target;
	FString Name;
	if (!Op.OnlyFields({TEXT("op"), TEXT("graph"), TEXT("name")}) || !ReadInterfaceTarget(State, Op, Target) || !Op.String(TEXT("name"), Name)
		|| RequireExposedPin(Op, Target, Name) == nullptr)
	{
		return false;
	}
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	if (Target.bFunction && Target.Owner->FindPin(Name)->IsExecuteContext())
	{
		return RemoveFunctionExecutePin(State, Op, Target, Name);
	}
#endif
	const FReportingScope Reporting(*Target.Inner.Controller);
	const bool bRemoved = Target.Inner.Controller->RemoveExposedPin(FName(*Name), false, false);
	if (!CheckReported(State, Op, FString::Printf(TEXT("Removing pin '%s'"), *Name)))
	{
		return false;
	}
	if (!bRemoved)
	{
		return Op.Fail(TEXT("ControlRig.EditFailed"), FString::Printf(TEXT("Pin '%s' could not be removed."), *Name), TEXT("name"));
	}
	State.bChanged = true;
	return true;
}

bool OpExposedPinRename(FGraphEditState& State, FReader& Op)
{
	FInterfaceTarget Target;
	FString Name;
	FString To;
	if (!Op.OnlyFields({TEXT("op"), TEXT("graph"), TEXT("name"), TEXT("to")}) || !ReadInterfaceTarget(State, Op, Target) || !Op.String(TEXT("name"), Name)
		|| !Op.String(TEXT("to"), To) || RequireExposedPin(Op, Target, Name) == nullptr)
	{
		return false;
	}
	if (Target.Owner->FindPin(To) != nullptr)
	{
		return Op.Fail(TEXT("ControlRig.NameTaken"), FString::Printf(TEXT("'%s' already has a pin '%s'."), *Target.Owner->GetName(), *To), TEXT("to"));
	}
	const FReportingScope Reporting(*Target.Inner.Controller);
	const bool bRenamed = Target.Inner.Controller->RenameExposedPin(FName(*Name), FName(*To), false, false);
	if (!CheckReported(State, Op, FString::Printf(TEXT("Renaming pin '%s'"), *Name), TEXT("ControlRig.EditRefused"), TEXT("to")))
	{
		return false;
	}
	if (!bRenamed || Target.Owner->FindPin(To) == nullptr)
	{
		return Op.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("Pin '%s' could not be renamed to '%s'."), *Name, *To), TEXT("to"));
	}
	State.bChanged = true;
	return true;
}

bool OpExposedPinType(FGraphEditState& State, FReader& Op)
{
	FInterfaceTarget Target;
	FString Name;
	FString CPPType;
	UObject* TypeObject = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("graph"), TEXT("name"), TEXT("type")}) || !ReadInterfaceTarget(State, Op, Target) || !Op.String(TEXT("name"), Name)
		|| RequireExposedPin(Op, Target, Name) == nullptr || !ReadType(Op, TEXT("type"), CPPType, TypeObject))
	{
		return false;
	}
	const FReportingScope Reporting(*Target.Inner.Controller);
	// What the Details panel's type picker does (orphan pins off, as it passes).
	bool bSetupUndo = false;
	const bool bChanged = Target.Inner.Controller->ChangeExposedPinType(FName(*Name), CPPType, TypeObjectPath(TypeObject), bSetupUndo, false, false);
	if (!CheckReported(State, Op, FString::Printf(TEXT("The type of pin '%s'"), *Name), TEXT("ControlRig.InvalidValue"), TEXT("type")))
	{
		return false;
	}
	if (!bChanged)
	{
		return Op.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("Pin '%s' could not take type %s."), *Name,
			*TypeName(CPPType, TypeObject)), TEXT("type"));
	}
	State.bChanged = true;
	return true;
}

bool OpExposedPinIndex(FGraphEditState& State, FReader& Op)
{
	FInterfaceTarget Target;
	FString Name;
	int64 Index = 0;
	if (!Op.OnlyFields({TEXT("op"), TEXT("graph"), TEXT("name"), TEXT("index")}) || !ReadInterfaceTarget(State, Op, Target) || !Op.String(TEXT("name"), Name)
		|| !Op.Int(TEXT("index"), Index) || RequireExposedPin(Op, Target, Name) == nullptr)
	{
		return false;
	}
	const FReportingScope Reporting(*Target.Inner.Controller);
	const bool bMoved = Target.Inner.Controller->SetExposedPinIndex(FName(*Name), static_cast<int32>(Index), false, false);
	if (!CheckReported(State, Op, FString::Printf(TEXT("Moving pin '%s'"), *Name), TEXT("ControlRig.InvalidValue"), TEXT("index")))
	{
		return false;
	}
	if (!bMoved)
	{
		return Op.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("Pin '%s' could not be moved to %lld."), *Name, Index), TEXT("index"));
	}
	State.bChanged = true;
	return true;
}

// ---- pin layout ------------------------------------------------------------------------------------------------

struct FLayoutCategory
{
	FString Name;
	TArray<FString> Pins;
	bool bExpanded = false;
};

bool ReadLayoutCategories(FReader& Op, const FInterfaceTarget& Target, TArray<FLayoutCategory>& Out)
{
	const FValue* Categories = nullptr;
	if (!Op.Array(TEXT("categories"), Categories))
	{
		return false;
	}
	for (int32 Index = 0; Index < Categories->Elements.Num(); ++Index)
	{
		const FValue& Item = Categories->Elements[Index];
		const FString Field = FString::Printf(TEXT("categories/%d"), Index);
		const FValue* Name = Item.Kind == EAssetDocumentRuntimeValueKind::Object ? FindField(Item, TEXT("name")) : nullptr;
		if (Name == nullptr || Name->Kind != EAssetDocumentRuntimeValueKind::String || Name->StringValue.IsEmpty())
		{
			return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("categories lists {name, pins?, expanded?}."), *Field);
		}
		FLayoutCategory& Category = Out.AddDefaulted_GetRef();
		Category.Name = Name->StringValue;
		if (const FValue* Expanded = FindField(Item, TEXT("expanded")))
		{
			Category.bExpanded = Expanded->Kind == EAssetDocumentRuntimeValueKind::Boolean && Expanded->BooleanValue;
		}
		const FValue* Pins = FindField(Item, TEXT("pins"));
		if (Pins == nullptr)
		{
			continue;
		}
		if (Pins->Kind != EAssetDocumentRuntimeValueKind::Array)
		{
			return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("pins lists pin paths."), *(Field + TEXT("/pins")));
		}
		for (int32 PinIndex = 0; PinIndex < Pins->Elements.Num(); ++PinIndex)
		{
			const FValue& Pin = Pins->Elements[PinIndex];
			const FString PinField = FString::Printf(TEXT("%s/pins/%d"), *Field, PinIndex);
			if (Pin.Kind != EAssetDocumentRuntimeValueKind::String)
			{
				return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("pins lists pin paths."), *PinField);
			}
			if (RequireExposedPin(Op, Target, Pin.StringValue, *PinField) == nullptr)
			{
				return false;
			}
			Category.Pins.Add(Pin.StringValue);
		}
	}
	return true;
}

// The category each pin path is in, from the node's layout.
TMap<FString, FString> LayoutPinCategories(const URigVMNode& Node)
{
	TMap<FString, FString> Out;
	const FRigVMNodeLayout Layout = Node.GetNodeLayout(true);
	for (const FRigVMPinCategory& Category : Layout.Categories)
	{
		for (const FString& Element : Category.Elements)
		{
			Out.Add(Element, Category.Path);
		}
	}
	return Out;
}

// Pin categories as the Details panel's pin layout edits them: unlisted categories removed, missing ones added, pins
// moved into their category (out of the one they leave first), the pins of each category and the categories put in
// order, the expansion set.
bool ApplyLayoutCategories(FGraphEditState& State, FReader& Op, const FInterfaceTarget& Target, const TArray<FLayoutCategory>& Wanted)
{
	URigVMController& Controller = *Target.OuterController;
	URigVMCollapseNode* Node = Target.Owner;
	const FName NodeName = Node->GetFName();
	TMap<FString, FString> WantedCategoryOf;
	TArray<FString> WantedNames;
	for (const FLayoutCategory& Category : Wanted)
	{
		WantedNames.Add(Category.Name);
		for (const FString& Pin : Category.Pins)
		{
			WantedCategoryOf.Add(Pin, Category.Name);
		}
	}
	for (const FString& Existing : Node->GetPinCategories())
	{
		if (!WantedNames.Contains(Existing) && Node->GetPinCategories().Contains(Existing))
		{
			Controller.RemovePinCategory(NodeName, Existing, false, false);
		}
	}
	for (const FString& Name : WantedNames)
	{
		if (!Node->GetPinCategories().Contains(Name))
		{
			Controller.AddEmptyPinCategory(NodeName, Name, false, false);
		}
	}
	if (!CheckReported(State, Op, TEXT("The pin categories"), TEXT("ControlRig.InvalidValue"), TEXT("categories")))
	{
		return false;
	}
	for (const TPair<FString, FString>& Current : LayoutPinCategories(*Node))
	{
		const FString* Category = WantedCategoryOf.Find(Current.Key);
		const URigVMPin* Pin = Node->FindPin(Current.Key);
		if (Pin != nullptr && (Category == nullptr || *Category != Current.Value))
		{
			Controller.ClearPinCategory(Pin->GetPinPath(), false, false);
		}
	}
	for (int32 Index = 0; Index < Wanted.Num(); ++Index)
	{
		for (const FString& PinPath : Wanted[Index].Pins)
		{
			const URigVMPin* Pin = Node->FindPin(PinPath);
			if (Pin != nullptr && LayoutPinCategories(*Node).FindRef(PinPath) != Wanted[Index].Name)
			{
				Controller.SetPinCategory(Pin->GetPinPath(), Wanted[Index].Name, false, false);
				if (!CheckReported(State, Op, FString::Printf(TEXT("Putting pin '%s' in category '%s'"), *PinPath, *Wanted[Index].Name),
					TEXT("ControlRig.InvalidValue"), *FString::Printf(TEXT("categories/%d/pins"), Index)))
				{
					return false;
				}
			}
		}
	}
	for (const FLayoutCategory& Category : Wanted)
	{
		const FRigVMNodeLayout Layout = Node->GetNodeLayout(true);
		const FRigVMPinCategory* Current = Layout.Categories.FindByPredicate(
			[&Category](const FRigVMPinCategory& Each) { return Each.Path == Category.Name; });
		if (Current != nullptr && Current->Elements != Category.Pins)
		{
			for (int32 Index = 0; Index < Category.Pins.Num(); ++Index)
			{
				if (const URigVMPin* Pin = Node->FindPin(Category.Pins[Index]))
				{
					Controller.SetPinIndexInCategory(Pin->GetPinPath(), Index, false, false);
				}
			}
		}
	}
	if (Node->GetPinCategories() != WantedNames)
	{
		for (int32 Index = 0; Index < WantedNames.Num(); ++Index)
		{
			Controller.SetPinCategoryIndex(NodeName, WantedNames[Index], Index, false, false);
		}
	}
	for (const FLayoutCategory& Category : Wanted)
	{
		if (!Category.Name.Equals(FRigVMPinCategory::GetDefaultCategoryName(), ESearchCase::IgnoreCase)
			&& Node->IsPinCategoryExpanded(Category.Name) != Category.bExpanded)
		{
			Controller.SetPinCategoryExpansion(NodeName, Category.Name, Category.bExpanded, false, false);
		}
	}
	return CheckReported(State, Op, TEXT("The pin category order"), TEXT("ControlRig.InvalidValue"), TEXT("categories"));
}

// RigVM.EnableNodeLayouts on for a scope (the controller writes pin layouts only while it is on). Afterwards the variable
// has its old value and its old SetBy priority again, so ini, device profile and console settings keep working.
// A value set from the console outranks code and stays as it is.
struct FFunctionEditNodeLayoutsScope
{
	IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("RigVM.EnableNodeLayouts"));
	EConsoleVariableFlags OriginalFlags = ECVF_Default;
	bool bTurnedOn = false;

	FFunctionEditNodeLayoutsScope()
	{
		if (Variable != nullptr && !Variable->GetBool())
		{
			OriginalFlags = Variable->GetFlags();
			Variable->Set(true, ECVF_SetByCode);
			bTurnedOn = Variable->GetBool();
		}
	}
	~FFunctionEditNodeLayoutsScope()
	{
		if (!bTurnedOn)
		{
			return;
		}
		// With priority history the variable drops back to the value of the priority below; without it Unset does nothing.
		Variable->Unset(ECVF_SetByCode);
		const EConsoleVariableFlags SetBy = static_cast<EConsoleVariableFlags>(Variable->GetFlags() & ECVF_SetByMask);
		if (SetBy == ECVF_SetByCode && (Variable->GetBool() || (OriginalFlags & ECVF_SetByMask) != ECVF_SetByCode))
		{
			Variable->Set(false, ECVF_SetByCode);
			Variable->SetFlags(OriginalFlags);
		}
	}
	FFunctionEditNodeLayoutsScope(const FFunctionEditNodeLayoutsScope&) = delete;
	FFunctionEditNodeLayoutsScope& operator=(const FFunctionEditNodeLayoutsScope&) = delete;
};

bool OpFunctionLayout(FGraphEditState& State, FReader& Op)
{
	FInterfaceTarget Target;
	if (!Op.OnlyFields({TEXT("op"), TEXT("graph"), TEXT("categories"), TEXT("display_names")}) || !ReadInterfaceTarget(State, Op, Target))
	{
		return false;
	}
	TArray<FLayoutCategory> Wanted;
	if (Op.Has(TEXT("categories")) && !ReadLayoutCategories(Op, Target, Wanted))
	{
		return false;
	}
	const FValue* Labels = nullptr;
	if (Op.Has(TEXT("display_names")) && !Op.Object(TEXT("display_names"), Labels))
	{
		return false;
	}
	// The Details panel edits the pin layout while node layouts are on (RigVM.EnableNodeLayouts); a rig holds its layout
	// either way, so the op turns them on for itself. Refused only when they stay off (set to 0 from the console).
	const FFunctionEditNodeLayoutsScope NodeLayouts;
	const URigVMSchema* Schema = Target.Owner->GetGraph()->GetSchema();
	if (Schema == nullptr || !Schema->SupportsNodeLayouts(Target.Owner->GetGraph()))
	{
		return Op.Fail(TEXT("ControlRig.EditRefused"), FString::Printf(TEXT("The pin layout of '%s' (PinCategories, pin DisplayName) is "
			"written only while node layouts are on, and the console variable RigVM.EnableNodeLayouts is held at 0: set it to 1, or leave "
			"PinCategories and DisplayName as the rig has them."), *Target.Owner->GetName()), Op.Has(TEXT("categories")) ? TEXT("categories")
			: TEXT("display_names"));
	}
	URigVMController& Controller = *Target.OuterController;
	const FReportingScope Reporting(Controller);
	if (Op.Has(TEXT("categories")) && !ApplyLayoutCategories(State, Op, Target, Wanted))
	{
		return false;
	}
	if (Labels != nullptr)
	{
		for (int32 Index = 0; Index < Labels->ObjectKeys.Num() && Index < Labels->Elements.Num(); ++Index)
		{
			const FString& PinPath = Labels->ObjectKeys[Index];
			const FValue& Label = Labels->Elements[Index];
			const FString Field = FString::Printf(TEXT("display_names/%s"), *PinPath);
			if (Label.Kind != EAssetDocumentRuntimeValueKind::String)
			{
				return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("A pin label is a string (\"\" clears it)."), *Field);
			}
			const URigVMPin* Pin = RequireExposedPin(Op, Target, PinPath, *Field);
			if (Pin == nullptr)
			{
				return false;
			}
			Controller.SetPinDisplayName(Pin->GetPinPath(), Label.StringValue, false, false);
			if (!CheckReported(State, Op, FString::Printf(TEXT("The label of pin '%s'"), *PinPath), TEXT("ControlRig.InvalidValue"), *Field))
			{
				return false;
			}
		}
	}
	State.bChanged = true;
	return true;
}

bool OpExposedPinDefault(FGraphEditState& State, FReader& Op)
{
	FInterfaceTarget Target;
	FString Name;
	if (!Op.OnlyFields({TEXT("op"), TEXT("graph"), TEXT("name"), TEXT("value")}) || !ReadInterfaceTarget(State, Op, Target)
		|| !Op.String(TEXT("name"), Name))
	{
		return false;
	}
	URigVMPin* Pin = RequireExposedPin(Op, Target, Name);
	if (Pin == nullptr)
	{
		return false;
	}
	if (!Target.bFunction)
	{
		return Op.Fail(TEXT("ControlRig.InvalidValue"), TEXT("A collapse node's pins take their values on the node (Defaults)."), TEXT("value"));
	}
	const FValue* Value = Op.Find(TEXT("value"));
	if (Value == nullptr)
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("function.pin.default takes a value (null resets)."), TEXT("value"));
	}
	// The argument default of the Details panel: the function node's pin value in the library.
	const FString Path = Pin->GetPinPath();
	const FReportingScope Reporting(*Target.OuterController);
	bool bDone = false;
	if (Value->Kind == EAssetDocumentRuntimeValueKind::Null)
	{
		bDone = Target.OuterController->ResetPinDefaultValue(Path, false, false);
	}
	else
	{
		FString Text;
		if (!DecodeValue(State, Op, TEXT("value"), Pin->GetCPPType(), Pin->GetCPPTypeObject(), Pin->GetDefaultValue(), Text))
		{
			return false;
		}
		bDone = Target.OuterController->SetPinDefaultValue(Path, Text, true, false, false, false);
	}
	if (!CheckReported(State, Op, FString::Printf(TEXT("The default of pin '%s'"), *Name), TEXT("ControlRig.InvalidValue"), TEXT("value")))
	{
		return false;
	}
	if (!bDone && Value->Kind != EAssetDocumentRuntimeValueKind::Null)
	{
		return Op.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("Pin '%s' takes no default (outputs and execute pins have none)."),
			*Name), TEXT("value"));
	}
	State.bChanged = true;
	return true;
}

// ---- local variables ------------------------------------------------------------------------------------------

bool ReadLocalTarget(FGraphEditState& State, FReader& Op, FInterfaceTarget& Out)
{
	if (!ReadInterfaceTarget(State, Op, Out))
	{
		return false;
	}
	if (!Out.bFunction)
	{
		return Op.Fail(TEXT("ControlRig.InvalidValue"), TEXT("Local variables belong to functions."), TEXT("graph"));
	}
	return true;
}

bool OpLocalAdd(FGraphEditState& State, FReader& Op)
{
	FInterfaceTarget Target;
	FString Name;
	FString CPPType;
	UObject* TypeObject = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("graph"), TEXT("name"), TEXT("type"), TEXT("default")}) || !ReadLocalTarget(State, Op, Target)
		|| !Op.String(TEXT("name"), Name) || !ReadType(Op, TEXT("type"), CPPType, TypeObject))
	{
		return false;
	}
	if (HasLocal(*Target.Inner.Graph, FName(*Name)))
	{
		return Op.Fail(TEXT("ControlRig.NameTaken"), FString::Printf(TEXT("Function '%s' already has a local variable '%s'."),
			*Target.Owner->GetName(), *Name), TEXT("name"));
	}
	FString Default;
	if (!DecodeValue(State, Op, TEXT("default"), CPPType, TypeObject, FString(), Default, true))
	{
		return false;
	}
	const FReportingScope Reporting(*Target.Inner.Controller);
	const FRigVMGraphVariableDescription Added = Target.Inner.Controller->AddLocalVariable(FName(*Name), CPPType, TypeObject, Default, false, false);
	if (!CheckReported(State, Op, FString::Printf(TEXT("Adding local variable '%s'"), *Name), TEXT("ControlRig.EditRefused"), TEXT("name")))
	{
		return false;
	}
	if (Added.Name.IsNone())
	{
		return Op.Fail(TEXT("ControlRig.EditFailed"), FString::Printf(TEXT("Local variable '%s' could not be added (its name may be taken by "
			"an input or a member variable)."), *Name), TEXT("name"));
	}
	State.bChanged = true;
	if (Added.Name.ToString() != Name)
	{
		return Op.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("The new local variable was named '%s', not '%s'."),
			*Added.Name.ToString(), *Name), TEXT("name"));
	}
	return true;
}

bool OpLocalRemove(FGraphEditState& State, FReader& Op)
{
	FInterfaceTarget Target;
	FString Name;
	if (!Op.OnlyFields({TEXT("op"), TEXT("graph"), TEXT("name")}) || !ReadLocalTarget(State, Op, Target) || !Op.String(TEXT("name"), Name))
	{
		return false;
	}
	if (!HasLocal(*Target.Inner.Graph, FName(*Name)))
	{
		return Op.Fail(TEXT("ControlRig.VariableNotFound"), FString::Printf(TEXT("Function '%s' has no local variable '%s'."),
			*Target.Owner->GetName(), *Name), TEXT("name"));
	}
	const FReportingScope Reporting(*Target.Inner.Controller);
	const bool bRemoved = Target.Inner.Controller->RemoveLocalVariable(FName(*Name), false, false);
	if (!CheckReported(State, Op, FString::Printf(TEXT("Removing local variable '%s'"), *Name)))
	{
		return false;
	}
	if (!bRemoved)
	{
		return Op.Fail(TEXT("ControlRig.EditFailed"), FString::Printf(TEXT("Local variable '%s' could not be removed."), *Name), TEXT("name"));
	}
	State.bChanged = true;
	return true;
}

bool OpLocalRename(FGraphEditState& State, FReader& Op)
{
	FInterfaceTarget Target;
	FString Name;
	FString To;
	if (!Op.OnlyFields({TEXT("op"), TEXT("graph"), TEXT("name"), TEXT("to")}) || !ReadLocalTarget(State, Op, Target) || !Op.String(TEXT("name"), Name)
		|| !Op.String(TEXT("to"), To))
	{
		return false;
	}
	if (!HasLocal(*Target.Inner.Graph, FName(*Name)))
	{
		return Op.Fail(TEXT("ControlRig.VariableNotFound"), FString::Printf(TEXT("Function '%s' has no local variable '%s'."),
			*Target.Owner->GetName(), *Name), TEXT("name"));
	}
	const FReportingScope Reporting(*Target.Inner.Controller);
	const bool bRenamed = Target.Inner.Controller->RenameLocalVariable(FName(*Name), FName(*To), false, false);
	if (!CheckReported(State, Op, FString::Printf(TEXT("Renaming local variable '%s'"), *Name), TEXT("ControlRig.EditRefused"), TEXT("to")))
	{
		return false;
	}
	if (!bRenamed || !HasLocal(*Target.Inner.Graph, FName(*To)))
	{
		return Op.Fail(TEXT("ControlRig.NameTaken"), FString::Printf(TEXT("Local variable '%s' could not be renamed to '%s' (the name may be "
			"taken)."), *Name, *To), TEXT("to"));
	}
	State.bChanged = true;
	return true;
}

bool OpLocalType(FGraphEditState& State, FReader& Op)
{
	FInterfaceTarget Target;
	FString Name;
	FString CPPType;
	UObject* TypeObject = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("graph"), TEXT("name"), TEXT("type")}) || !ReadLocalTarget(State, Op, Target) || !Op.String(TEXT("name"), Name)
		|| !ReadType(Op, TEXT("type"), CPPType, TypeObject))
	{
		return false;
	}
	if (!HasLocal(*Target.Inner.Graph, FName(*Name)))
	{
		return Op.Fail(TEXT("ControlRig.VariableNotFound"), FString::Printf(TEXT("Function '%s' has no local variable '%s'."),
			*Target.Owner->GetName(), *Name), TEXT("name"));
	}
	const FReportingScope Reporting(*Target.Inner.Controller);
	const bool bChanged = Target.Inner.Controller->SetLocalVariableType(FName(*Name), CPPType, TypeObject, false, false);
	if (!CheckReported(State, Op, FString::Printf(TEXT("The type of local variable '%s'"), *Name), TEXT("ControlRig.InvalidValue"), TEXT("type")))
	{
		return false;
	}
	if (!bChanged)
	{
		return Op.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("Local variable '%s' could not take type %s."), *Name,
			*TypeName(CPPType, TypeObject)), TEXT("type"));
	}
	State.bChanged = true;
	return true;
}

bool OpLocalDefault(FGraphEditState& State, FReader& Op)
{
	FInterfaceTarget Target;
	FString Name;
	if (!Op.OnlyFields({TEXT("op"), TEXT("graph"), TEXT("name"), TEXT("value")}) || !ReadLocalTarget(State, Op, Target) || !Op.String(TEXT("name"), Name))
	{
		return false;
	}
	const FRigVMGraphVariableDescription* Local = nullptr;
	const TArray<FRigVMGraphVariableDescription> Locals = Target.Inner.Graph->GetLocalVariables(false);
	for (const FRigVMGraphVariableDescription& Item : Locals)
	{
		if (Item.Name == FName(*Name))
		{
			Local = &Item;
		}
	}
	if (Local == nullptr)
	{
		return Op.Fail(TEXT("ControlRig.VariableNotFound"), FString::Printf(TEXT("Function '%s' has no local variable '%s'."),
			*Target.Owner->GetName(), *Name), TEXT("name"));
	}
	const FValue* Value = Op.Find(TEXT("value"));
	FString Text;
	if (Value == nullptr)
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("local.default takes a value (null resets)."), TEXT("value"));
	}
	if (Value->Kind != EAssetDocumentRuntimeValueKind::Null
		&& !DecodeValue(State, Op, TEXT("value"), Local->CPPType, Local->CPPTypeObject, Local->DefaultValue, Text, true))
	{
		return false;
	}
	const FReportingScope Reporting(*Target.Inner.Controller);
	Target.Inner.Controller->SetLocalVariableDefaultValue(FName(*Name), Text, false, false);
	if (!CheckReported(State, Op, FString::Printf(TEXT("The default of local variable '%s'"), *Name), TEXT("ControlRig.InvalidValue"), TEXT("value")))
	{
		return false;
	}
	State.bChanged = true;
	return true;
}

// ---- dependents -----------------------------------------------------------------------------------------------

// Rigs that call a public function of this rig keep the function's old header until their next load or compile
// (design §5.4). Each listed public function that other packages reference gives a ControlRig.DependentsAffected note
// with those packages; nothing else is edited.
bool OpFunctionDependents(FGraphEditState& State, FReader& Op, int32 OpIndex)
{
	const FValue* Functions = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("functions")}) || !Op.Array(TEXT("functions"), Functions))
	{
		return false;
	}
	const URigVMFunctionLibrary* Library = LibraryOf(State.Blueprint);
	const FString Own = State.Blueprint.GetPackage()->GetName();
	for (int32 Index = 0; Index < Functions->Elements.Num(); ++Index)
	{
		const FValue& Item = Functions->Elements[Index];
		const FValue* NameValue = Item.Kind == EAssetDocumentRuntimeValueKind::Object ? FindField(Item, TEXT("name")) : nullptr;
		const FValue* ChangeValue = Item.Kind == EAssetDocumentRuntimeValueKind::Object ? FindField(Item, TEXT("change")) : nullptr;
		if (NameValue == nullptr || NameValue->Kind != EAssetDocumentRuntimeValueKind::String)
		{
			return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("functions lists {name, change}."), *FString::Printf(TEXT("functions/%d"), Index));
		}
		const FName Name(*NameValue->StringValue);
		if (Library == nullptr || Library->FindFunction(Name) == nullptr || !Library->IsFunctionPublic(Name))
		{
			continue;
		}
		TArray<FString> Packages;
		Library->ForEachReferenceSoftPtr(Name, [&Packages, &Own](TSoftObjectPtr<URigVMFunctionReferenceNode> Reference)
		{
			const FString Package = Reference.ToSoftObjectPath().GetLongPackageName();
			// A Validate trial copy (transient package) that called the function is no dependent.
			if (!Package.IsEmpty() && Package != Own && !Package.StartsWith(TEXT("/Engine/Transient")))
			{
				Packages.AddUnique(Package);
			}
		});
		if (Packages.IsEmpty())
		{
			continue;
		}
		Packages.Sort();
		const FString Change = ChangeValue != nullptr && ChangeValue->Kind == EAssetDocumentRuntimeValueKind::String ? ChangeValue->StringValue
			: FString(TEXT("changed"));
		FObjectBuilder Note;
		Note.AddInt(TEXT("op"), OpIndex);
		Note.AddString(TEXT("field"), FString::Printf(TEXT("functions/%d"), Index));
		Note.AddString(TEXT("code"), TEXT("ControlRig.DependentsAffected"));
		Note.AddString(TEXT("message"), FString::Printf(TEXT("Public function '%s' is %s; %s call it and are not edited (they take the "
			"function's new header on their next load or compile; a function node of a removed or renamed function, or a pin it no longer has, "
			"breaks there): %s."), *NameValue->StringValue, *Change, Packages.Num() == 1 ? TEXT("1 other asset calls") : *FString::Printf(
			TEXT("%d other assets"), Packages.Num()), *FString::Join(Packages, TEXT(", "))));
		State.Notes.Add(Note.Build());
	}
	return true;
}
} // namespace

bool IsFunctionOp(const FString& Kind)
{
	return Kind.StartsWith(TEXT("function.")) || Kind.StartsWith(TEXT("local."));
}

bool RunFunctionOp(FGraphEditState& State, FReader& Op, const FString& Kind, int32 OpIndex)
{
	if (Kind == TEXT("function.add")) { return OpFunctionAdd(State, Op); }
	if (Kind == TEXT("function.remove")) { return OpFunctionRemove(State, Op); }
	if (Kind == TEXT("function.rename")) { return OpFunctionRename(State, Op); }
	if (Kind == TEXT("function.set")) { return OpFunctionSet(State, Op); }
	if (Kind == TEXT("function.pin.add")) { return OpExposedPinAdd(State, Op); }
	if (Kind == TEXT("function.pin.remove")) { return OpExposedPinRemove(State, Op); }
	if (Kind == TEXT("function.pin.rename")) { return OpExposedPinRename(State, Op); }
	if (Kind == TEXT("function.pin.type")) { return OpExposedPinType(State, Op); }
	if (Kind == TEXT("function.pin.index")) { return OpExposedPinIndex(State, Op); }
	if (Kind == TEXT("function.pin.default")) { return OpExposedPinDefault(State, Op); }
	if (Kind == TEXT("function.layout")) { return OpFunctionLayout(State, Op); }
	if (Kind == TEXT("function.dependents")) { return OpFunctionDependents(State, Op, OpIndex); }
	if (Kind == TEXT("local.add")) { return OpLocalAdd(State, Op); }
	if (Kind == TEXT("local.remove")) { return OpLocalRemove(State, Op); }
	if (Kind == TEXT("local.rename")) { return OpLocalRename(State, Op); }
	if (Kind == TEXT("local.type")) { return OpLocalType(State, Op); }
	if (Kind == TEXT("local.default")) { return OpLocalDefault(State, Op); }
	return Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("Unknown op '%s'."), *Kind), TEXT("op"));
}
} // namespace UE::AssetDocument::Graph::Rig::ControlRig
