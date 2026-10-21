// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Function ops of rig.controlrig.edit.v1 (docs/architecture/asset-document-controlrig-design.md §3.5, §4.1, §5.4,
// phase CR3): the local function library through its controller (what My Blueprint's Functions "+", rename, delete
// and the function's Details "Access" / category / keywords / description / colour do), the exposed pins of a function
// or collapse graph and the local variables of a function through that graph's controller (the Details panel's Inputs
// / Outputs and Local Variables), and the dependents report of public functions. Undo and Python print are off (the
// edit primitive's transaction covers the call); controller reporting is on so refusals fail the op.
//
// A function or collapse graph is named as the read's scopes are: ["Functions", <function>] or a graph scope ending in
// ["Nodes", <collapse node>, "Collapse"] (FGraphTarget). Ops:
//   function.add {name, mutable}      AddFunctionToLibrary (mutable: the editor's "New Function", with an execute pin)
//   function.remove {name}  function.rename {name, to}
//   function.set {graph, public?, color?, category?, keywords?, description?}   public / color: functions only
//   function.pin.add {graph, name, direction, type, default?}   direction Input / Output / IO
//   function.pin.remove {graph, name}  function.pin.rename {graph, name, to}  function.pin.type {graph, name, type}
//   function.pin.index {graph, name, index}   index among all exposed pins
//   function.pin.default {graph, name, value} a function's argument default (null resets)
//   local.add {graph, name, type, default?}  local.remove {graph, name}  local.rename {graph, name, to}
//   local.type {graph, name, type}  local.default {graph, name, value}
//   function.dependents {functions: [{name, change}]}   notes ControlRig.DependentsAffected for each public function
//       other packages reference (warn only: other packages are never edited, decision §5.4)

#include "AssetDocumentControlRig.h"
#include "AssetDocumentNameSuggestions.h"

#include "ControlRigBlueprintLegacy.h"
#include "Misc/EngineVersionComparison.h"
#include "RigVMCore/RigVMExecuteContext.h"
#include "RigVMCore/RigVMStruct.h"
#include "RigVMModel/Nodes/RigVMCollapseNode.h"
#include "RigVMModel/Nodes/RigVMFunctionReferenceNode.h"
#include "RigVMModel/Nodes/RigVMLibraryNode.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMFunctionLibrary.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMPin.h"

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

bool OpFunctionSet(FGraphEditState& State, FReader& Op)
{
	FInterfaceTarget Target;
	if (!Op.OnlyFields({TEXT("op"), TEXT("graph"), TEXT("public"), TEXT("color"), TEXT("category"), TEXT("keywords"), TEXT("description")})
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
	if (Kind == TEXT("function.dependents")) { return OpFunctionDependents(State, Op, OpIndex); }
	if (Kind == TEXT("local.add")) { return OpLocalAdd(State, Op); }
	if (Kind == TEXT("local.remove")) { return OpLocalRemove(State, Op); }
	if (Kind == TEXT("local.rename")) { return OpLocalRename(State, Op); }
	if (Kind == TEXT("local.type")) { return OpLocalType(State, Op); }
	if (Kind == TEXT("local.default")) { return OpLocalDefault(State, Op); }
	return Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("Unknown op '%s'."), *Kind), TEXT("op"));
}
} // namespace UE::AssetDocument::Graph::Rig::ControlRig
