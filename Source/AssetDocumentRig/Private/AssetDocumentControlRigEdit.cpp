// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// rig.controlrig.edit.v1: edits of a Control Rig as the Control Rig editor makes them (docs/architecture/
// asset-document-controlrig-design.md §4.1): the hierarchy tab and element Details through
// URigHierarchyController / URigHierarchy, My Blueprint member variables through URigVMBlueprint, the asset's
// commands (Turn into Rig Module / Standalone Rig, preview mesh, module settings) and the Forwards Solve event the
// editor's first open adds to an empty rig. One transaction; controller undo and Python-print flags off; auto VM
// recompile off for the call (rig.controlrig.compile.v1 finalizes). Controller refusals (ReportError) are captured
// per op and fail that op with the controller's message.
//
// Args {ops: [...]}, each {op, ...}:
//   preview_mesh {mesh}                       Class Settings "Preview Skeletal Mesh" (SetPreviewMesh; "" clears)
//   source_import {property, path}            Class Settings "Source Hierarchy Import" / "Source Curve Import" (a
//                                             Skeleton or Skeletal Mesh; "" clears). Duplicate-transient, so not a
//                                             generic Property: the profile owns them.
//   graph.forwards_solve {}                   RigUnit_BeginExecution on the default model (the editor's first open)
//   rig.to_module {}  rig.to_standalone {}    "Turn into Rig Module" / "Turn into Standalone Rig"
//   module.settings {settings}                RigModuleSettings {Name?, Type?, Icon?, Category?, Keywords?, Description?}
//   variable.add {name, type}  variable.remove {name}  variable.rename {name, to}  variable.type {name, type}
//   variable.set {name, public?, read_only?, expose?, category?, tooltip?, default?}   (default null resets)
//   variable.order {names}
//   element.add {entry}                       a new element (document entry; its first parent only)
//   element.set {entry}                       an element's parents, pose, settings, values, spaces, hosts, metadata
//   element.remove {key}  element.rename {key, to}  element.order {keys}
//   graph.add / graph.remove / graph.rename, node.add / node.remove / node.rename / node.set / node.types /
//   node.aggregate / node.traits / node.remap, link.add / link.break, pin.default / pin.bind / pin.array
//                                             graph ops (AssetDocumentControlRigGraphEdit.cpp)
//   function.add / remove / rename / set, function.pin.add / remove / rename / type / index / default,
//   local.add / remove / rename / type / default, function.dependents
//                                             function ops (AssetDocumentControlRigFunctionEdit.cpp)
//   module.add / remove / rename / reparent / reorder / swap / config / bind, connect, disconnect, modular.settings,
//   hierarchy.import                          modular ops and the mesh import
//                                             (AssetDocumentControlRigModularEdit.cpp)
// Result {changed, notes?: [{op, field, code, message}]} (notes: warnings of graph and function ops, e.g. a deprecated
// unit kept, dependents of a changed public function).

#include "AssetDocumentControlRig.h"
#include "AssetDocumentNameSuggestions.h"

#include "AssetDocumentEditorNonInteractiveScope.h"
#include "ControlRigBlueprintLegacy.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/CoreMisc.h"
#include "Misc/EngineVersionComparison.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "Rigs/RigHierarchyElements.h"
#include "Rigs/RigHierarchyMetadata.h"
#include "RigVMCore/RigVMExternalVariable.h"
#include "RigVMDeveloperTypeUtils.h"
#include "RigVMModel/Nodes/RigVMVariableNode.h"
#include "RigVMModel/RigVMBuildData.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMFunctionLibrary.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMTypeUtils.h"
#include "ScopedTransaction.h"
#include "Units/Execution/RigUnit_BeginExecution.h"
#include "Units/RigUnit.h"

#define LOCTEXT_NAMESPACE "AssetDocumentControlRigEdit"

namespace UE::AssetDocument::Graph::Rig::ControlRig
{
void RunPreVariableChange(URigVMBlueprint& Blueprint)
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	Blueprint.HandlePreVariableChange(&Blueprint);
#else
	Blueprint.OnPreVariableChange(&Blueprint);
#endif
}

void RunPostVariableChange(URigVMBlueprint& Blueprint)
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	Blueprint.HandlePostVariableChange(&Blueprint);
#else
	Blueprint.OnPostVariableChange(&Blueprint);
#endif
}

void PointVariableNodesAtVariables(URigVMBlueprint& Blueprint)
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	for (URigVMGraph* Model : Blueprint.GetAllModels())
	{
		URigVMController* Controller = Model != nullptr ? Blueprint.GetOrCreateController(Model) : nullptr;
		if (Controller == nullptr)
		{
			continue;
		}
		for (URigVMNode* Node : Model->GetNodes())
		{
			const URigVMVariableNode* VariableNode = Cast<URigVMVariableNode>(Node);
			if (VariableNode == nullptr || VariableNode->IsLocalVariable() || VariableNode->IsInputArgument())
			{
				continue;
			}
			const FRigVMExternalVariable Variable = Controller->GetVariableByName(VariableNode->GetVariableName());
			if (Variable.GetGuid().IsValid() && Variable.GetGuid() != VariableNode->GetVariableGuid())
			{
				Controller->RefreshVariableNode(VariableNode->GetFName(), Variable.GetGuid(), VariableNode->GetVariableName(), FString(), nullptr,
					false, false);
			}
		}
	}
#endif
}

namespace ControlRigEditLocal
{
struct FEdit
{
	UControlRigBlueprint& Blueprint;
	bool bChanged = false;
	TArray<FString> Errors;

	URigHierarchy* Hierarchy() const { return Blueprint.GetHierarchy(); }
	URigHierarchyController* Controller() const
	{
		URigHierarchy* Found = Blueprint.GetHierarchy();
		return Found != nullptr ? Found->GetController(true) : nullptr;
	}
};

// Messages of one op: the hierarchy and RigVM controllers report refusals (ReportError / ReportWarning) through the
// script exception handler; this op's handler keeps the errors (failing the op with them) and drops the warnings
// instead of logging them.
class FCaptureScope
{
public:
	explicit FCaptureScope(FEdit& InEdit)
		: Handler([Errors = &InEdit.Errors](ELogVerbosity::Type Verbosity, const TCHAR* Message, const TCHAR*)
		{
			if (Verbosity <= ELogVerbosity::Error)
			{
				// "<package path> : <message>"
				FString Text(Message);
				const int32 Separator = Text.Find(TEXT(" : "));
				if (Separator != INDEX_NONE && Text.StartsWith(TEXT("/")))
				{
					Text.RightChopInline(Separator + 3);
				}
				Errors->Add(Text);
			}
		})
	{
	}

private:
	FScopedScriptExceptionHandler Handler;
};

// Auto VM recompile is off while the ops run (each hierarchy / variable change would otherwise recompile the VM).
class FAutoRecompileScope
{
public:
	explicit FAutoRecompileScope(URigVMBlueprint& InBlueprint)
		: Blueprint(InBlueprint), bPrevious(InBlueprint.GetAutoVMRecompile())
	{
		Blueprint.SetAutoVMRecompile(false);
	}
	~FAutoRecompileScope() { Blueprint.SetAutoVMRecompile(bPrevious); }
	FAutoRecompileScope(const FAutoRecompileScope&) = delete;
	FAutoRecompileScope& operator=(const FAutoRecompileScope&) = delete;

private:
	URigVMBlueprint& Blueprint;
	bool bPrevious;
};

FString KeyText(const FRigElementKey& Key)
{
	return Key.IsValid() ? Key.ToString() : FString();
}

bool ReadName(FReader& Op, const TCHAR* Field, FName& Out)
{
	FString Text;
	if (!Op.String(Field, Text))
	{
		return false;
	}
	if (Text.IsEmpty())
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("%s is a non-empty name."), Field), Field);
	}
	Out = FName(*Text);
	return true;
}

bool ReadKey(FReader& Reader, const TCHAR* Field, FRigElementKey& Out, bool bAllowEmpty = false)
{
	FString Text;
	if (!Reader.String(Field, Text))
	{
		return false;
	}
	if (!ParseElementKey(Text, Out) || (!bAllowEmpty && !Out.IsValid()))
	{
		return Reader.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("'%s' is not an element key (\"Type(Name)\", e.g. "
			"\"Bone(spine_01)\")."), *Text), Field);
	}
	return true;
}

bool KeyFromValue(FReader& Reader, const FValue& Value, const TCHAR* Field, FRigElementKey& Out)
{
	if (Value.Kind != EAssetDocumentRuntimeValueKind::String || !ParseElementKey(Value.StringValue, Out) || !Out.IsValid())
	{
		return Reader.Fail(TEXT("ControlRig.InvalidValue"), TEXT("An element key is \"Type(Name)\" (\"Bone(spine_01)\")."), Field);
	}
	return true;
}

bool RequireElement(FEdit& Edit, FReader& Reader, const TCHAR* Field, const FRigElementKey& Key)
{
	const URigHierarchy* Hierarchy = Edit.Hierarchy();
	if (Hierarchy != nullptr && Hierarchy->Contains(Key))
	{
		return true;
	}
	TArray<FString> Keys;
	if (Hierarchy != nullptr)
	{
		for (const FRigElementKey& Each : Hierarchy->GetAllKeys())
		{
			Keys.Add(KeyText(Each));
		}
	}
	return Reader.Fail(TEXT("ControlRig.ElementNotFound"), FString::Printf(TEXT("The hierarchy has no element %s.%s"), *KeyText(Key),
		*UE::AssetDocument::DescribeNameCandidates(KeyText(Key), Keys, TEXT("Elements"))), Field);
}

// Fails the op when the controller reported errors during it.
bool CheckReported(FEdit& Edit, FReader& Op, const TCHAR* What)
{
	if (Edit.Errors.IsEmpty())
	{
		return true;
	}
	const FString Message = FString::Printf(TEXT("%s was refused by the hierarchy controller: %s"), What, *FString::Join(Edit.Errors, TEXT(" | ")));
	Edit.Errors.Reset();
	return Op.Fail(TEXT("ControlRig.EditRefused"), Message);
}

// ---- values --------------------------------------------------------------------------------------------------------

bool NumberOf(const FValue& Value, double& Out)
{
	if (Value.Kind == EAssetDocumentRuntimeValueKind::Number)
	{
		Out = Value.NumberValue;
		return true;
	}
	if (Value.Kind == EAssetDocumentRuntimeValueKind::Integer)
	{
		Out = static_cast<double>(Value.IntegerValue);
		return true;
	}
	return false;
}

bool Vector3Of(const FValue& Value, FVector3f& Out, const TCHAR* const* Names, FString& OutError)
{
	if (Value.Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		OutError = FString::Printf(TEXT("expected {%s, %s, %s}"), Names[0], Names[1], Names[2]);
		return false;
	}
	float Parts[3] = {0.f, 0.f, 0.f};
	for (int32 Index = 0; Index < Value.ObjectKeys.Num(); ++Index)
	{
		int32 Slot = INDEX_NONE;
		for (int32 Name = 0; Name < 3; ++Name)
		{
			if (Value.ObjectKeys[Index] == Names[Name])
			{
				Slot = Name;
			}
		}
		double Number = 0.0;
		if (Slot == INDEX_NONE || !NumberOf(Value.Elements[Index], Number))
		{
			OutError = FString::Printf(TEXT("expected {%s, %s, %s} numbers, got '%s'"), Names[0], Names[1], Names[2], *Value.ObjectKeys[Index]);
			return false;
		}
		Parts[Slot] = static_cast<float>(Number);
	}
	for (int32 Name = 0; Name < 3; ++Name)
	{
		if (FindField(Value, Names[Name]) == nullptr)
		{
			OutError = FString::Printf(TEXT("expected {%s, %s, %s}: %s is missing"), Names[0], Names[1], Names[2], Names[Name]);
			return false;
		}
	}
	Out = FVector3f(Parts[0], Parts[1], Parts[2]);
	return true;
}

const TCHAR* const XYZ[] = {TEXT("X"), TEXT("Y"), TEXT("Z")};
const TCHAR* const PYR[] = {TEXT("Pitch"), TEXT("Yaw"), TEXT("Roll")};

// A control value typed by its control type (the inverse of the read's EncodeControlValue). OutRotator is the
// authored rotator of Rotator / EulerTransform values (written as the preferred rotator, so it reads back as written).
bool DecodeControlValue(const FValue& Value, const FRigControlSettings& Settings, FRigControlValue& Out, TOptional<FRotator>& OutRotator,
	FString& OutError)
{
	OutRotator.Reset();
	switch (Settings.ControlType)
	{
	case ERigControlType::Bool:
		if (Value.Kind != EAssetDocumentRuntimeValueKind::Boolean)
		{
			OutError = TEXT("a Bool control's value is true / false");
			return false;
		}
		Out = FRigControlValue::Make<bool>(Value.BooleanValue);
		return true;
	case ERigControlType::Float:
	case ERigControlType::ScaleFloat:
	{
		double Number = 0.0;
		if (!NumberOf(Value, Number))
		{
			OutError = TEXT("a Float control's value is a number");
			return false;
		}
		Out = FRigControlValue::Make<float>(static_cast<float>(Number));
		return true;
	}
	case ERigControlType::Integer:
	{
		if (Value.Kind == EAssetDocumentRuntimeValueKind::String && Settings.ControlEnum != nullptr)
		{
			const int64 Entry = Settings.ControlEnum->GetValueByNameString(Value.StringValue);
			if (Entry == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("'%s' is not an entry of %s"), *Value.StringValue, *Settings.ControlEnum->GetName());
				return false;
			}
			Out = FRigControlValue::Make<int32>(static_cast<int32>(Entry));
			return true;
		}
		if (Value.Kind != EAssetDocumentRuntimeValueKind::Integer)
		{
			OutError = TEXT("an Integer control's value is an integer (or an entry name of its ControlEnum)");
			return false;
		}
		Out = FRigControlValue::Make<int32>(static_cast<int32>(Value.IntegerValue));
		return true;
	}
	case ERigControlType::Vector2D:
	{
		if (Value.Kind != EAssetDocumentRuntimeValueKind::Object || Value.ObjectKeys.Num() != 2)
		{
			OutError = TEXT("a Vector2D control's value is {X, Y}");
			return false;
		}
		const FValue* X = FindField(Value, TEXT("X"));
		const FValue* Y = FindField(Value, TEXT("Y"));
		double XValue = 0.0;
		double YValue = 0.0;
		if (X == nullptr || Y == nullptr || !NumberOf(*X, XValue) || !NumberOf(*Y, YValue))
		{
			OutError = TEXT("a Vector2D control's value is {X, Y}");
			return false;
		}
		Out = FRigControlValue::Make<FVector3f>(FVector3f(static_cast<float>(XValue), static_cast<float>(YValue), 0.f));
		return true;
	}
	case ERigControlType::Position:
	case ERigControlType::Scale:
	{
		FVector3f Vector;
		if (!Vector3Of(Value, Vector, XYZ, OutError))
		{
			return false;
		}
		Out = FRigControlValue::Make<FVector3f>(Vector);
		return true;
	}
	case ERigControlType::Rotator:
	{
		FVector3f Rotator;
		if (!Vector3Of(Value, Rotator, PYR, OutError))
		{
			return false;
		}
		// Stored as Euler: X = roll, Y = pitch, Z = yaw.
		Out = FRigControlValue::Make<FVector3f>(FVector3f(Rotator.Z, Rotator.X, Rotator.Y));
		OutRotator = FRotator(Rotator.X, Rotator.Y, Rotator.Z);
		return true;
	}
	case ERigControlType::Transform:
	case ERigControlType::TransformNoScale:
	{
		FTransform Transform;
		if (!DecodeTransform(Value, Transform, OutError, true))
		{
			return false;
		}
		if (Settings.ControlType == ERigControlType::Transform)
		{
			Out = FRigControlValue::Make<FRigControlValue::FTransform_Float>(FRigControlValue::FTransform_Float(Transform));
		}
		else
		{
			Out = FRigControlValue::Make<FRigControlValue::FTransformNoScale_Float>(
				FRigControlValue::FTransformNoScale_Float(FTransformNoScale(Transform)));
		}
		return true;
	}
	case ERigControlType::EulerTransform:
	{
		if (Value.Kind != EAssetDocumentRuntimeValueKind::Object)
		{
			OutError = TEXT("an EulerTransform control's value is {Location?, Rotation?, Scale?}");
			return false;
		}
		FEulerTransform Euler(FVector::ZeroVector, FRotator::ZeroRotator, FVector::OneVector);
		for (int32 Index = 0; Index < Value.ObjectKeys.Num(); ++Index)
		{
			const FString& Key = Value.ObjectKeys[Index];
			FVector3f Part;
			if (Key == TEXT("Location") || Key == TEXT("Scale"))
			{
				if (!Vector3Of(Value.Elements[Index], Part, XYZ, OutError))
				{
					return false;
				}
				(Key == TEXT("Location") ? Euler.Location : Euler.Scale) = FVector(Part);
			}
			else if (Key == TEXT("Rotation"))
			{
				if (!Vector3Of(Value.Elements[Index], Part, PYR, OutError))
				{
					return false;
				}
				Euler.Rotation = FRotator(Part.X, Part.Y, Part.Z);
			}
			else
			{
				OutError = FString::Printf(TEXT("an EulerTransform control's value has no field '%s' (Location, Rotation, Scale)"), *Key);
				return false;
			}
		}
		Out = FRigControlValue::Make<FRigControlValue::FEulerTransform_Float>(FRigControlValue::FEulerTransform_Float(Euler));
		OutRotator = Euler.Rotation;
		return true;
	}
	default:
		OutError = TEXT("the control type holds no value");
		return false;
	}
}

// FRigControlSettings of a document entry: a new control's settings of that ControlType, the sparse document fields
// over them, and the fields other keys own (limits, spaces) or that are never authored taken from Live.
bool DecodeControlSettings(FReader& Entry, const FValue* Document, const FRigControlSettings* Live, FRigControlSettings& Out)
{
	Out = FRigControlSettings();
	FString Error;
	if (Document != nullptr)
	{
		if (Document->Kind != EAssetDocumentRuntimeValueKind::Object)
		{
			return Entry.Fail(TEXT("ControlRig.InvalidValue"), TEXT("Settings is an object."), TEXT("Settings"));
		}
		if (const FValue* Type = FindField(*Document, TEXT("ControlType")))
		{
			const FProperty* TypeProperty = FRigControlSettings::StaticStruct()->FindPropertyByName(TEXT("ControlType"));
			if (TypeProperty == nullptr || !DecodeProperty(*TypeProperty, TypeProperty->ContainerPtrToValuePtr<void>(&Out), *Type, Error))
			{
				return Entry.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("Settings.ControlType: %s"), *Error), TEXT("Settings"));
			}
		}
	}
	Out.SetupLimitArrayForType(false, false, false);
	if (Document != nullptr && !DecodeStructFields(*FRigControlSettings::StaticStruct(), &Out, *Document, &IsSkippedControlField, Error))
	{
		return Entry.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("Settings: %s"), *Error), TEXT("Settings"));
	}
	if (Live != nullptr)
	{
		for (TFieldIterator<FProperty> It(FRigControlSettings::StaticStruct()); It; ++It)
		{
			if (IsSkippedControlField(**It))
			{
				It->CopyCompleteValue_InContainer(&Out, Live);
			}
		}
	}
	return true;
}

bool DecodeConnectorSettings(FReader& Entry, const FValue* Document, const FRigConnectorSettings* Live, FRigConnectorSettings& Out)
{
	Out = FRigConnectorSettings::DefaultSettings();
	auto Skip = [](const FProperty& Property) { return Property.HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated); };
	FString Error;
	if (Document != nullptr && !DecodeStructFields(*FRigConnectorSettings::StaticStruct(), &Out, *Document, Skip, Error))
	{
		return Entry.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("Settings: %s"), *Error), TEXT("Settings"));
	}
	if (Live != nullptr)
	{
		for (TFieldIterator<FProperty> It(FRigConnectorSettings::StaticStruct()); It; ++It)
		{
			if (Skip(**It))
			{
				It->CopyCompleteValue_InContainer(&Out, Live);
			}
		}
	}
	return true;
}

bool DecodeColor(FReader& Entry, const FValue& Value, FLinearColor& Out)
{
	const FProperty* Property = FRigControlSettings::StaticStruct()->FindPropertyByName(TEXT("ShapeColor"));
	FString Error;
	Out = FLinearColor::White;
	if (Property == nullptr || !DecodeProperty(*Property, &Out, Value, Error))
	{
		return Entry.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("Color: %s"), *Error), TEXT("Color"));
	}
	return true;
}

bool ReadTransform(FReader& Entry, const TCHAR* Field, FTransform& Out)
{
	const FValue* Value = Entry.Find(Field);
	Out = FTransform::Identity;
	FString Error;
	if (Value != nullptr && !DecodeTransform(*Value, Out, Error, true))
	{
		return Entry.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("%s: %s"), Field, *Error), Field);
	}
	return true;
}

// ---- entries -------------------------------------------------------------------------------------------------------

struct FParentSpec
{
	FRigElementKey Key;
	FRigElementWeight Weight = FRigElementWeight(1.f);
	FName Label = NAME_None;
};

bool AllowedFields(FReader& Entry, const FValue& Value, ERigElementType Type)
{
	TArray<const TCHAR*> Allowed = {TEXT("Type"), TEXT("Name"), TEXT("Metadata")};
	switch (Type)
	{
	case ERigElementType::Bone: Allowed.Append({TEXT("Parent"), TEXT("Imported"), TEXT("Transform")}); break;
	case ERigElementType::Null: Allowed.Append({TEXT("Parent"), TEXT("Parents"), TEXT("Transform")}); break;
	case ERigElementType::Socket: Allowed.Append({TEXT("Parent"), TEXT("Transform"), TEXT("Color"), TEXT("Description")}); break;
	case ERigElementType::Control:
		Allowed.Append({TEXT("Parent"), TEXT("Parents"), TEXT("Settings"), TEXT("Offset"), TEXT("Shape"), TEXT("Value"), TEXT("Minimum"),
			TEXT("Maximum"), TEXT("Spaces"), TEXT("Hosts")});
		break;
	case ERigElementType::Curve: Allowed.Add(TEXT("Value")); break;
	case ERigElementType::Connector: Allowed.Add(TEXT("Settings")); break;
	default: break;
	}
	for (const FString& Key : Value.ObjectKeys)
	{
		if (!Allowed.ContainsByPredicate([&Key](const TCHAR* Each) { return Key == Each; }))
		{
			TArray<FString> Names;
			for (const TCHAR* Each : Allowed)
			{
				Names.Add(Each);
			}
			return Entry.Fail(TEXT("Graph.UnknownField"), FString::Printf(TEXT("A %s element has no field '%s' (fields: %s)."),
				*StaticEnum<ERigElementType>()->GetNameStringByValue(static_cast<int64>(Type)), *Key, *FString::Join(Names, TEXT(", "))), Key);
		}
	}
	return true;
}

bool ReadParents(FReader& Entry, TArray<FParentSpec>& Out)
{
	Out.Reset();
	const FValue* Single = Entry.Find(TEXT("Parent"));
	const FValue* Multi = Entry.Find(TEXT("Parents"));
	if (Single != nullptr && Multi != nullptr)
	{
		return Entry.Fail(TEXT("ControlRig.InvalidValue"), TEXT("An element has either Parent or Parents, not both."), TEXT("Parents"));
	}
	if (Single != nullptr)
	{
		FParentSpec& Spec = Out.AddDefaulted_GetRef();
		return KeyFromValue(Entry, *Single, TEXT("Parent"), Spec.Key);
	}
	if (Multi == nullptr)
	{
		return true;
	}
	if (Multi->Kind != EAssetDocumentRuntimeValueKind::Array)
	{
		return Entry.Fail(TEXT("ControlRig.InvalidValue"), TEXT("Parents is a list of {Parent, Weight?, Label?}."), TEXT("Parents"));
	}
	for (int32 Index = 0; Index < Multi->Elements.Num(); ++Index)
	{
		const FValue& Item = Multi->Elements[Index];
		FReader Row = Entry.Child(FString::Printf(TEXT("Parents/%d"), Index), Item);
		if (!Row.IsObject() || !Row.OnlyFields({TEXT("Parent"), TEXT("Weight"), TEXT("Label")}))
		{
			return Row.Fail(TEXT("ControlRig.InvalidValue"), TEXT("A parent is {Parent, Weight?, Label?}."));
		}
		FParentSpec& Spec = Out.AddDefaulted_GetRef();
		if (!ReadKey(Row, TEXT("Parent"), Spec.Key))
		{
			return false;
		}
		if (const FValue* Weight = Row.Find(TEXT("Weight")))
		{
			double Number = 0.0;
			if (NumberOf(*Weight, Number))
			{
				Spec.Weight = FRigElementWeight(static_cast<float>(Number));
			}
			else
			{
				FVector3f Split;
				FString Error;
				static const TCHAR* const LRS[] = {TEXT("Location"), TEXT("Rotation"), TEXT("Scale")};
				if (!Vector3Of(*Weight, Split, LRS, Error))
				{
					return Row.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("Weight is a number or {Location, Rotation, Scale}: %s"), *Error), TEXT("Weight"));
				}
				Spec.Weight = FRigElementWeight(Split.X, Split.Y, Split.Z);
			}
		}
		FString Label;
		if (!Row.String(TEXT("Label"), Label, false))
		{
			return false;
		}
		Spec.Label = Label.IsEmpty() ? NAME_None : FName(*Label);
	}
	for (int32 Index = 0; Index < Out.Num(); ++Index)
	{
		for (int32 Other = 0; Other < Index; ++Other)
		{
			if (Out[Other].Key == Out[Index].Key)
			{
				return Entry.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("%s is listed twice."), *KeyText(Out[Index].Key)),
					TEXT("Parents"));
			}
		}
	}
	return true;
}

// Spaces / Hosts: ["Bone(x)", {"Space": "Null(y)", "Label": "World"}].
bool ReadSpaces(FReader& Entry, const TCHAR* Field, TArray<FRigElementKeyWithLabel>& Out)
{
	Out.Reset();
	const FValue* List = Entry.Find(Field);
	if (List == nullptr)
	{
		return true;
	}
	if (List->Kind != EAssetDocumentRuntimeValueKind::Array)
	{
		return Entry.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("%s is a list of element keys or {Space, Label}."), Field), Field);
	}
	for (int32 Index = 0; Index < List->Elements.Num(); ++Index)
	{
		const FValue& Item = List->Elements[Index];
		FReader Row = Entry.Child(FString::Printf(TEXT("%s/%d"), Field, Index), Item);
		FRigElementKeyWithLabel& Space = Out.AddDefaulted_GetRef();
		if (Item.Kind == EAssetDocumentRuntimeValueKind::String)
		{
			if (!KeyFromValue(Row, Item, TEXT(""), Space.Key))
			{
				return false;
			}
			continue;
		}
		FString Label;
		if (!Row.IsObject() || !Row.OnlyFields({TEXT("Space"), TEXT("Label")}) || !ReadKey(Row, TEXT("Space"), Space.Key)
			|| !Row.String(TEXT("Label"), Label, false))
		{
			return Row.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("A %s entry is an element key or {Space, Label}."), Field));
		}
		Space.Label = Label.IsEmpty() ? NAME_None : FName(*Label);
	}
	return true;
}

// ---- reconcile -----------------------------------------------------------------------------------------------------

bool SameWeight(const FRigElementWeight& A, const FRigElementWeight& B)
{
	return A.Location == B.Location && A.Rotation == B.Rotation && A.Scale == B.Scale;
}

FName ParentLabel(URigHierarchy& Hierarchy, const FRigElementKey& Child, const FRigElementKey& Parent)
{
	if (const FRigMultiParentElement* MultiParent = Hierarchy.Find<FRigMultiParentElement>(Child))
	{
		const int32* Index = MultiParent->IndexLookup.Find(Parent);
		if (Index != nullptr && MultiParent->ParentConstraints.IsValidIndex(*Index))
		{
			return MultiParent->ParentConstraints[*Index].DisplayLabel;
		}
	}
	return NAME_None;
}

// Parents (keep global, as the hierarchy tab's drag and drop) then weights: a changed parent list or label is
// rebuilt (no label setter exists; AddParent takes it).
bool ReconcileParents(FEdit& Edit, FReader& Entry, const FRigElementKey& Key, const TArray<FParentSpec>& Wanted, bool bWeighted)
{
	URigHierarchy& Hierarchy = *Edit.Hierarchy();
	URigHierarchyController& Controller = *Edit.Controller();
	for (const FParentSpec& Spec : Wanted)
	{
		if (!RequireElement(Edit, Entry, bWeighted ? TEXT("Parents") : TEXT("Parent"), Spec.Key))
		{
			return false;
		}
	}
	const TArray<FRigElementKey> Live = Hierarchy.GetParents(Key);
	bool bSame = Live.Num() == Wanted.Num();
	for (int32 Index = 0; bSame && Index < Live.Num(); ++Index)
	{
		bSame = Live[Index] == Wanted[Index].Key && (!bWeighted || ParentLabel(Hierarchy, Key, Live[Index]) == Wanted[Index].Label);
	}
	if (!bSame)
	{
		if (Wanted.IsEmpty())
		{
			Controller.RemoveAllParents(Key, true, false, false);
		}
		else if (!bWeighted)
		{
			Controller.SetParent(Key, Wanted[0].Key, true, false, false);
		}
		else
		{
			Controller.RemoveAllParents(Key, true, false, false);
			for (const FParentSpec& Spec : Wanted)
			{
				Controller.AddParent(Key, Spec.Key, Spec.Weight.Location, true, Spec.Label, false);
			}
		}
		if (!CheckReported(Edit, Entry, TEXT("The reparent")))
		{
			return false;
		}
		const TArray<FRigElementKey> After = Hierarchy.GetParents(Key);
		bool bApplied = After.Num() == Wanted.Num();
		for (int32 Index = 0; bApplied && Index < After.Num(); ++Index)
		{
			bApplied = After[Index] == Wanted[Index].Key;
		}
		if (!bApplied)
		{
			return Entry.Fail(TEXT("ControlRig.InvalidParent"), FString::Printf(TEXT("%s cannot have the parents %s (a cycle, or a parent "
				"of a type it cannot have)."), *KeyText(Key), *FString::JoinBy(Wanted, TEXT(", "), [](const FParentSpec& Spec) { return KeyText(Spec.Key); })),
				bWeighted ? TEXT("Parents") : TEXT("Parent"));
		}
		Edit.bChanged = true;
	}
	// An animation channel's parent weights are fixed (SetParentWeight refuses them: a channel moves nothing; the
	// hierarchy controller adds one at weight 0, older versions at 1), and the read leaves them out.
	const FRigControlElement* Channel = Hierarchy.Find<FRigControlElement>(Key);
	if (Channel != nullptr && Channel->IsAnimationChannel())
	{
		for (const FParentSpec& Spec : Wanted)
		{
			if (!SameWeight(Spec.Weight, FRigElementWeight(1.f)))
			{
				return Entry.Fail(TEXT("ControlRig.InvalidValue"), TEXT("An animation channel's parent weight cannot be set (the editor keeps it)."),
					TEXT("Parents"));
			}
		}
	}
	else if (bWeighted && !Wanted.IsEmpty())
	{
		const FRigBaseElement* Element = Hierarchy.Find(Key);
		const TArray<FRigElementWeight> Initial = Hierarchy.GetParentWeightArray(Element, true);
		const TArray<FRigElementWeight> Current = Hierarchy.GetParentWeightArray(Element, false);
		for (int32 Index = 0; Index < Wanted.Num(); ++Index)
		{
			if (!Initial.IsValidIndex(Index) || !SameWeight(Initial[Index], Wanted[Index].Weight))
			{
				Hierarchy.SetParentWeight(Key, Wanted[Index].Key, Wanted[Index].Weight, true, true);
				Edit.bChanged = true;
			}
			if (!Current.IsValidIndex(Index) || !SameWeight(Current[Index], Wanted[Index].Weight))
			{
				Hierarchy.SetParentWeight(Key, Wanted[Index].Key, Wanted[Index].Weight, false, true);
				Edit.bChanged = true;
			}
		}
	}
	return true;
}

void SetLocal(FEdit& Edit, const FRigElementKey& Key, const FTransform& Transform)
{
	URigHierarchy& Hierarchy = *Edit.Hierarchy();
	if (!Hierarchy.GetLocalTransform(Key, true).Equals(Transform, 0.0))
	{
		Hierarchy.SetLocalTransform(Key, Transform, true, true, false, false);
		Edit.bChanged = true;
	}
	if (!Hierarchy.GetLocalTransform(Key, false).Equals(Transform, 0.0))
	{
		Hierarchy.SetLocalTransform(Key, Transform, false, true, false, false);
		Edit.bChanged = true;
	}
}

// Available spaces (controls) or channel hosts (animation channels), in order, with labels.
bool ReconcileSpaces(FEdit& Edit, FReader& Entry, const FRigElementKey& Key, const TCHAR* Field, bool bHosts)
{
	TArray<FRigElementKeyWithLabel> Wanted;
	if (!ReadSpaces(Entry, Field, Wanted))
	{
		return false;
	}
	for (int32 Index = 0; Index < Wanted.Num(); ++Index)
	{
		if (!RequireElement(Edit, Entry, Field, Wanted[Index].Key))
		{
			return false;
		}
	}
	URigHierarchy& Hierarchy = *Edit.Hierarchy();
	URigHierarchyController& Controller = *Edit.Controller();
	auto Live = [&Hierarchy, &Key]() -> TArray<FRigElementKeyWithLabel>
	{
		const FRigControlElement* Control = Hierarchy.Find<FRigControlElement>(Key);
		return Control != nullptr ? Control->Settings.Customization.AvailableSpaces : TArray<FRigElementKeyWithLabel>();
	};
	for (const FRigElementKeyWithLabel& Have : Live())
	{
		if (!Wanted.ContainsByPredicate([&Have](const FRigElementKeyWithLabel& Each) { return Each.Key == Have.Key; }))
		{
			if (bHosts)
			{
				Controller.RemoveChannelHost(Key, Have.Key, false, false);
			}
			else
			{
				Controller.RemoveAvailableSpace(Key, Have.Key, false, false);
			}
			Edit.bChanged = true;
		}
	}
	for (int32 Index = 0; Index < Wanted.Num(); ++Index)
	{
		const FRigElementKeyWithLabel& Want = Wanted[Index];
		TArray<FRigElementKeyWithLabel> Have = Live();
		const int32 Found = Have.IndexOfByPredicate([&Want](const FRigElementKeyWithLabel& Each) { return Each.Key == Want.Key; });
		if (Found == INDEX_NONE)
		{
			const bool bAdded = bHosts ? Controller.AddChannelHost(Key, Want.Key, false, false)
				: Controller.AddAvailableSpace(Key, Want.Key, Want.Label, false, false);
			if (!bAdded)
			{
				CheckReported(Edit, Entry, bHosts ? TEXT("Adding the host") : TEXT("Adding the space"));
				return Entry.Fail(TEXT("ControlRig.InvalidParent"), FString::Printf(TEXT("%s cannot be a %s of %s."), *KeyText(Want.Key),
					bHosts ? TEXT("host") : TEXT("space"), *KeyText(Key)), Field);
			}
			Edit.bChanged = true;
			Have = Live();
		}
		const int32 Position = Have.IndexOfByPredicate([&Want](const FRigElementKeyWithLabel& Each) { return Each.Key == Want.Key; });
		if (Position != Index && Position != INDEX_NONE)
		{
			Controller.SetAvailableSpaceIndex(Key, Want.Key, Index, false, false);
			Edit.bChanged = true;
		}
		if (Position != INDEX_NONE && Have[Position].Label != Want.Label && !bHosts)
		{
			Controller.SetAvailableSpaceLabel(Key, Want.Key, Want.Label, false, false);
			Edit.bChanged = true;
		}
	}
	return CheckReported(Edit, Entry, bHosts ? TEXT("The channel hosts") : TEXT("The spaces"));
}

bool ReconcileMetadata(FEdit& Edit, FReader& Entry, const FRigElementKey& Key)
{
	URigHierarchy& Hierarchy = *Edit.Hierarchy();
	const FValue* Wanted = Entry.Find(TEXT("Metadata"));
	if (Wanted != nullptr && Wanted->Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return Entry.Fail(TEXT("ControlRig.InvalidValue"), TEXT("Metadata is an object {name: {Type, Value}}."), TEXT("Metadata"));
	}
	TArray<FName> Names = Hierarchy.GetMetadataNames(Key);
	if (Key.Type == ERigElementType::Socket)
	{
		Names.Remove(FRigSocketElement::ColorMetaName);
		Names.Remove(FRigSocketElement::DescriptionMetaName);
	}
	for (const FName& Name : Names)
	{
		if (Wanted == nullptr || !Wanted->ObjectKeys.Contains(Name.ToString()))
		{
			Hierarchy.RemoveMetadataForElement(Hierarchy.Find(Key), Name);
			Edit.bChanged = true;
		}
	}
	if (Wanted == nullptr)
	{
		return true;
	}
	for (int32 Index = 0; Index < Wanted->ObjectKeys.Num(); ++Index)
	{
		const FString& Name = Wanted->ObjectKeys[Index];
		FReader Item = Entry.Child(TEXT("Metadata/") + Name.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")), Wanted->Elements[Index]);
		FString TypeText;
		if (!Item.IsObject() || !Item.OnlyFields({TEXT("Type"), TEXT("Value")}) || !Item.String(TEXT("Type"), TypeText) || !Item.Has(TEXT("Value")))
		{
			return Item.Fail(TEXT("ControlRig.InvalidValue"), TEXT("A metadata entry is {Type, Value}."));
		}
		const int64 TypeValue = StaticEnum<ERigMetadataType>()->GetValueByNameString(TypeText);
		const ERigMetadataType Type = static_cast<ERigMetadataType>(TypeValue);
		if (TypeValue == INDEX_NONE || Type == ERigMetadataType::Invalid)
		{
			return Item.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("'%s' is not a metadata type (Bool, Float, Int32, Name, "
				"Vector, Rotator, Quat, Transform, LinearColor, RigElementKey and their Array forms)."), *TypeText), TEXT("Type"));
		}
		if (Key.Type == ERigElementType::Socket && (FName(*Name) == FRigSocketElement::ColorMetaName || FName(*Name) == FRigSocketElement::DescriptionMetaName))
		{
			return Item.Fail(TEXT("ControlRig.InvalidValue"), TEXT("A socket's color and description are its Color / Description fields."));
		}
		FRigBaseElement* Element = Hierarchy.Find(Key);
		const FRigBaseMetadata* Existing = Hierarchy.FindMetadataForElement(Element, FName(*Name), Type);
		FRigBaseMetadata* Metadata = Hierarchy.GetMetadataForElement(Element, FName(*Name), Type, true);
		const UScriptStruct* MetadataStruct = Metadata != nullptr ? Metadata->GetMetadataStruct() : nullptr;
		const FProperty* ValueProperty = MetadataStruct != nullptr ? MetadataStruct->FindPropertyByName(TEXT("Value")) : nullptr;
		if (ValueProperty == nullptr)
		{
			return Item.Fail(TEXT("ControlRig.EditFailed"), FString::Printf(TEXT("Metadata '%s' could not be created."), *Name));
		}
		TArray<uint8, TAlignedHeapAllocator<16>> Memory;
		Memory.SetNumZeroed(ValueProperty->GetElementSize());
		ValueProperty->InitializeValue(Memory.GetData());
		ValueProperty->CopyCompleteValue(Memory.GetData(), static_cast<const FRigBaseMetadata*>(Metadata)->GetValueData());
		FString Error;
		const bool bDecoded = DecodeProperty(*ValueProperty, Memory.GetData(), *Item.Find(TEXT("Value")), Error);
		const bool bSet = bDecoded && Metadata->SetValueData(Memory.GetData(), ValueProperty->GetElementSize());
		ValueProperty->DestroyValue(Memory.GetData());
		if (!bDecoded)
		{
			return Item.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("Value: %s"), *Error), TEXT("Value"));
		}
		Edit.bChanged = Edit.bChanged || bSet || Existing == nullptr;
	}
	return true;
}

bool ReconcileControl(FEdit& Edit, FReader& Entry, const FRigElementKey& Key)
{
	URigHierarchy& Hierarchy = *Edit.Hierarchy();
	FRigControlElement* Control = Hierarchy.Find<FRigControlElement>(Key);
	FRigControlSettings Settings;
	if (!DecodeControlSettings(Entry, Entry.Find(TEXT("Settings")), &Control->Settings, Settings))
	{
		return false;
	}
	if (!FRigControlSettings::StaticStruct()->CompareScriptStruct(&Settings, &Control->Settings, PPF_None))
	{
		Hierarchy.SetControlSettings(Key, Settings, false, true, false);
		Edit.bChanged = true;
		Control = Hierarchy.Find<FRigControlElement>(Key);
	}
	FTransform Offset;
	FTransform Shape;
	if (!ReadTransform(Entry, TEXT("Offset"), Offset) || !ReadTransform(Entry, TEXT("Shape"), Shape))
	{
		return false;
	}
	for (const bool bInitial : {true, false})
	{
		const ERigTransformType::Type Type = bInitial ? ERigTransformType::InitialLocal : ERigTransformType::CurrentLocal;
		if (!Hierarchy.GetControlOffsetTransform(Control, Type).Equals(Offset, 0.0))
		{
			Hierarchy.SetControlOffsetTransform(Key, Offset, bInitial, true, false, false);
			Edit.bChanged = true;
		}
		if (!Hierarchy.GetControlShapeTransform(Control, Type).Equals(Shape, 0.0))
		{
			Hierarchy.SetControlShapeTransform(Key, Shape, bInitial, false);
			Edit.bChanged = true;
		}
	}
	FString Error;
	for (const TPair<const TCHAR*, ERigControlValueType>& Limit : {TPair<const TCHAR*, ERigControlValueType>(TEXT("Minimum"), ERigControlValueType::Minimum),
		TPair<const TCHAR*, ERigControlValueType>(TEXT("Maximum"), ERigControlValueType::Maximum)})
	{
		const FValue* Value = Entry.Find(Limit.Key);
		if (Value == nullptr)
		{
			continue;
		}
		FRigControlValue Decoded;
		TOptional<FRotator> Rotator;
		if (!DecodeControlValue(*Value, Settings, Decoded, Rotator, Error))
		{
			return Entry.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("%s: %s"), Limit.Key, *Error), Limit.Key);
		}
		Hierarchy.SetControlValue(Key, Decoded, Limit.Value, false, false);
		Edit.bChanged = true;
	}
	FRigControlValue Value = Settings.GetIdentityValue();
	TOptional<FRotator> Rotator;
	if (const FValue* Authored = Entry.Find(TEXT("Value")))
	{
		if (!DecodeControlValue(*Authored, Settings, Value, Rotator, Error))
		{
			return Entry.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("Value: %s"), *Error), TEXT("Value"));
		}
	}
	else if (Settings.ControlType == ERigControlType::Rotator || Settings.ControlType == ERigControlType::EulerTransform)
	{
		Rotator = FRotator::ZeroRotator;
	}
	Hierarchy.SetControlValue(Key, Value, ERigControlValueType::Initial, false, false);
	Hierarchy.SetControlValue(Key, Value, ERigControlValueType::Current, false, false);
	if (Rotator.IsSet())
	{
		Hierarchy.SetControlPreferredRotator(Key, Rotator.GetValue(), true, false);
		Hierarchy.SetControlPreferredRotator(Key, Rotator.GetValue(), false, false);
	}
	Edit.bChanged = true;
	Control = Hierarchy.Find<FRigControlElement>(Key);
	const bool bChannel = Control != nullptr && Control->IsAnimationChannel();
	if (Entry.Has(bChannel ? TEXT("Spaces") : TEXT("Hosts")))
	{
		return Entry.Fail(TEXT("ControlRig.InvalidValue"), bChannel ? TEXT("An animation channel has Hosts, not Spaces.")
			: TEXT("Only animation channels have Hosts; a control has Spaces."), bChannel ? TEXT("Spaces") : TEXT("Hosts"));
	}
	return ReconcileSpaces(Edit, Entry, Key, bChannel ? TEXT("Hosts") : TEXT("Spaces"), bChannel);
}

// Every field of an element entry written over the live element (element.set; element.add after creating it).
bool Reconcile(FEdit& Edit, FReader& Entry, const FRigElementKey& Key)
{
	URigHierarchy& Hierarchy = *Edit.Hierarchy();
	TArray<FParentSpec> Parents;
	if (!ReadParents(Entry, Parents))
	{
		return false;
	}
	const bool bWeighted = Key.Type == ERigElementType::Null || Key.Type == ERigElementType::Control;
	if (Key.Type != ERigElementType::Curve && Key.Type != ERigElementType::Connector
		&& !ReconcileParents(Edit, Entry, Key, Parents, bWeighted))
	{
		return false;
	}
	switch (Key.Type)
	{
	case ERigElementType::Bone:
	{
		bool bImported = false;
		if (!Entry.Bool(TEXT("Imported"), bImported, false))
		{
			return false;
		}
		FRigBoneElement* Bone = Hierarchy.Find<FRigBoneElement>(Key);
		const ERigBoneType Type = bImported ? ERigBoneType::Imported : ERigBoneType::User;
		if (Bone->BoneType != Type)
		{
			Bone->BoneType = Type;
			Edit.bChanged = true;
		}
		FTransform Local;
		if (!ReadTransform(Entry, TEXT("Transform"), Local))
		{
			return false;
		}
		if (bImported && !Entry.Has(TEXT("Transform")))
		{
			const FSourcePose Pose(Edit.Blueprint);
			if (const FTransform* Reference = Pose.Local.Find(Key.Name))
			{
				Local = *Reference;
			}
		}
		SetLocal(Edit, Key, Local);
		break;
	}
	case ERigElementType::Null:
	{
		FTransform Local;
		if (!ReadTransform(Entry, TEXT("Transform"), Local))
		{
			return false;
		}
		SetLocal(Edit, Key, Local);
		break;
	}
	case ERigElementType::Socket:
	{
		FTransform Local;
		if (!ReadTransform(Entry, TEXT("Transform"), Local))
		{
			return false;
		}
		SetLocal(Edit, Key, Local);
		FRigSocketElement* Socket = Hierarchy.Find<FRigSocketElement>(Key);
		if (const FValue* ColorValue = Entry.Find(TEXT("Color")))
		{
			FLinearColor Color;
			if (!DecodeColor(Entry, *ColorValue, Color))
			{
				return false;
			}
			if (!Hierarchy.GetMetadataNames(Key).Contains(FRigSocketElement::ColorMetaName) || Socket->GetColor(&Hierarchy) != Color)
			{
				Socket->SetColor(Color, &Hierarchy);
				Edit.bChanged = true;
			}
		}
		else if (Hierarchy.GetMetadataNames(Key).Contains(FRigSocketElement::ColorMetaName))
		{
			Hierarchy.RemoveMetadataForElement(Socket, FRigSocketElement::ColorMetaName);
			Edit.bChanged = true;
		}
		FString Description;
		if (!Entry.String(TEXT("Description"), Description, false))
		{
			return false;
		}
		if (Socket->GetDescription(&Hierarchy) != Description)
		{
			Socket->SetDescription(Description, &Hierarchy);
			Edit.bChanged = true;
		}
		break;
	}
	case ERigElementType::Control:
		if (!ReconcileControl(Edit, Entry, Key))
		{
			return false;
		}
		break;
	case ERigElementType::Curve:
	{
		const FValue* Value = Entry.Find(TEXT("Value"));
		double Number = 0.0;
		if (Value != nullptr && !NumberOf(*Value, Number))
		{
			return Entry.Fail(TEXT("ControlRig.InvalidValue"), TEXT("A curve's Value is a number."), TEXT("Value"));
		}
		const FRigCurveElement* Curve = Hierarchy.Find<FRigCurveElement>(Key);
		if (Value != nullptr && (!Curve->IsValueSet() || Curve->Get() != static_cast<float>(Number)))
		{
			Hierarchy.SetCurveValue(Key, static_cast<float>(Number), false);
			Edit.bChanged = true;
		}
		else if (Value == nullptr && Curve->IsValueSet())
		{
			Hierarchy.UnsetCurveValue(Key, false);
			Edit.bChanged = true;
		}
		break;
	}
	case ERigElementType::Connector:
	{
		const FRigConnectorElement* Connector = Hierarchy.Find<FRigConnectorElement>(Key);
		FRigConnectorSettings Settings;
		if (!DecodeConnectorSettings(Entry, Entry.Find(TEXT("Settings")), &Connector->Settings, Settings))
		{
			return false;
		}
		if (!FRigConnectorSettings::StaticStruct()->CompareScriptStruct(&Settings, &Connector->Settings, PPF_None))
		{
			Hierarchy.SetConnectorSettings(Key, Settings, false, true, false);
			Edit.bChanged = true;
		}
		break;
	}
	default:
		break;
	}
	if (!ReconcileMetadata(Edit, Entry, Key))
	{
		return false;
	}
	return CheckReported(Edit, Entry, *FString::Printf(TEXT("Writing %s"), *KeyText(Key)));
}

bool ReadEntry(FReader& Op, const FValue*& OutValue, FRigElementKey& OutKey, ERigElementType& OutType)
{
	if (!Op.Object(TEXT("entry"), OutValue))
	{
		return false;
	}
	FReader Entry = Op.Child(TEXT("entry"), *OutValue);
	FString TypeText;
	FString NameText;
	if (!Entry.String(TEXT("Type"), TypeText) || !Entry.String(TEXT("Name"), NameText))
	{
		return false;
	}
	const int64 TypeValue = StaticEnum<ERigElementType>()->GetValueByNameString(TypeText);
	OutType = static_cast<ERigElementType>(TypeValue);
	if (TypeValue == INDEX_NONE || OutType == ERigElementType::None || OutType == ERigElementType::All)
	{
		return Entry.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("'%s' is not an element type (Bone, Null, Control, "
			"Curve, Connector, Socket)."), *TypeText), TEXT("Type"));
	}
	if (OutType == ERigElementType::Reference)
	{
		return Entry.Fail(TEXT("ControlRig.ProceduralElement"), TEXT("Reference elements are made by graph code at runtime, never authored."), TEXT("Type"));
	}
	if (NameText.IsEmpty())
	{
		return Entry.Fail(TEXT("ControlRig.InvalidValue"), TEXT("Name is a non-empty element name."), TEXT("Name"));
	}
	OutKey = FRigElementKey(FName(*NameText), OutType);
	return AllowedFields(Entry, *OutValue, OutType);
}

// ---- element ops -----------------------------------------------------------------------------------------------------

bool OpElementAdd(FEdit& Edit, FReader& Op)
{
	const FValue* Value = nullptr;
	FRigElementKey Key;
	ERigElementType Type = ERigElementType::None;
	if (!ReadEntry(Op, Value, Key, Type))
	{
		return false;
	}
	FReader Entry = Op.Child(TEXT("entry"), *Value);
	URigHierarchy& Hierarchy = *Edit.Hierarchy();
	URigHierarchyController& Controller = *Edit.Controller();
	if (Hierarchy.Contains(Key))
	{
		return Entry.Fail(TEXT("ControlRig.NameTaken"), FString::Printf(TEXT("The hierarchy already has %s."), *KeyText(Key)), TEXT("Name"));
	}
	TArray<FParentSpec> Parents;
	if (!ReadParents(Entry, Parents))
	{
		return false;
	}
	const FRigElementKey Parent = Parents.IsEmpty() ? FRigElementKey() : Parents[0].Key;
	if (Parent.IsValid() && !RequireElement(Edit, Entry, Entry.Has(TEXT("Parents")) ? TEXT("Parents") : TEXT("Parent"), Parent))
	{
		return false;
	}
	FTransform Local;
	if (!ReadTransform(Entry, TEXT("Transform"), Local))
	{
		return false;
	}
	FRigElementKey Added;
	switch (Type)
	{
	case ERigElementType::Bone:
	{
		bool bImported = false;
		if (!Entry.Bool(TEXT("Imported"), bImported, false))
		{
			return false;
		}
		if (bImported && !Entry.Has(TEXT("Transform")))
		{
			const FSourcePose Pose(Edit.Blueprint);
			if (const FTransform* Reference = Pose.Local.Find(Key.Name))
			{
				Local = *Reference;
			}
		}
		Added = Controller.AddBone(Key.Name, Parent, Local, false, bImported ? ERigBoneType::Imported : ERigBoneType::User, false, false);
		break;
	}
	case ERigElementType::Null:
		Added = Controller.AddNull(Key.Name, Parent, Local, false, false, false);
		break;
	case ERigElementType::Control:
	{
		FRigControlSettings Settings;
		if (!DecodeControlSettings(Entry, Entry.Find(TEXT("Settings")), nullptr, Settings))
		{
			return false;
		}
		if (Settings.AnimationType == ERigControlAnimationType::AnimationChannel)
		{
			if (Parent.Type != ERigElementType::Control)
			{
				return Entry.Fail(TEXT("ControlRig.InvalidParent"), TEXT("An animation channel's parent is a control."), TEXT("Parent"));
			}
			Added = Controller.AddAnimationChannel(Key.Name, Parent, Settings, false, false);
		}
		else
		{
			FTransform Offset;
			FTransform Shape;
			if (!ReadTransform(Entry, TEXT("Offset"), Offset) || !ReadTransform(Entry, TEXT("Shape"), Shape))
			{
				return false;
			}
			Added = Controller.AddControl(Key.Name, Parent, Settings, Settings.GetIdentityValue(), Offset, Shape, false, false);
		}
		break;
	}
	case ERigElementType::Curve:
		Added = Controller.AddCurve(Key.Name, 0.f, false, false);
		break;
	case ERigElementType::Connector:
	{
		FRigConnectorSettings Settings;
		if (!DecodeConnectorSettings(Entry, Entry.Find(TEXT("Settings")), nullptr, Settings))
		{
			return false;
		}
		Added = Controller.AddConnector(Key.Name, Settings, false, false);
		break;
	}
	case ERigElementType::Socket:
	{
		FLinearColor Color = FRigSocketElement::SocketDefaultColor;
		if (const FValue* ColorValue = Entry.Find(TEXT("Color")))
		{
			if (!DecodeColor(Entry, *ColorValue, Color))
			{
				return false;
			}
		}
		FString Description;
		if (!Entry.String(TEXT("Description"), Description, false))
		{
			return false;
		}
		Added = Controller.AddSocket(Key.Name, Parent, Local, false, Color, Description, false, false);
		break;
	}
	default:
		break;
	}
	if (!CheckReported(Edit, Entry, *FString::Printf(TEXT("Adding %s"), *KeyText(Key))))
	{
		return false;
	}
	if (!Added.IsValid())
	{
		return Entry.Fail(TEXT("ControlRig.EditFailed"), FString::Printf(TEXT("%s could not be added."), *KeyText(Key)));
	}
	Edit.bChanged = true;
	if (Added != Key)
	{
		return Entry.Fail(TEXT("ControlRig.NameTaken"), FString::Printf(TEXT("The hierarchy named the new element %s, not %s (the name is "
			"taken by another element)."), *KeyText(Added), *KeyText(Key)), TEXT("Name"));
	}
	return Reconcile(Edit, Entry, Key);
}

bool OpElementSet(FEdit& Edit, FReader& Op)
{
	const FValue* Value = nullptr;
	FRigElementKey Key;
	ERigElementType Type = ERigElementType::None;
	if (!ReadEntry(Op, Value, Key, Type))
	{
		return false;
	}
	FReader Entry = Op.Child(TEXT("entry"), *Value);
	return RequireElement(Edit, Entry, TEXT("Name"), Key) && Reconcile(Edit, Entry, Key);
}

bool OpElementRemove(FEdit& Edit, FReader& Op)
{
	FRigElementKey Key;
	if (!ReadKey(Op, TEXT("key"), Key) || !RequireElement(Edit, Op, TEXT("key"), Key))
	{
		return false;
	}
	const bool bRemoved = Edit.Controller()->RemoveElement(Key, false, false);
	if (!CheckReported(Edit, Op, *FString::Printf(TEXT("Removing %s"), *KeyText(Key))))
	{
		return false;
	}
	if (!bRemoved)
	{
		return Op.Fail(TEXT("ControlRig.EditFailed"), FString::Printf(TEXT("%s could not be removed."), *KeyText(Key)), TEXT("key"));
	}
	Edit.bChanged = true;
	return true;
}

bool OpElementRename(FEdit& Edit, FReader& Op)
{
	FRigElementKey Key;
	FName To;
	if (!ReadKey(Op, TEXT("key"), Key) || !ReadName(Op, TEXT("to"), To) || !RequireElement(Edit, Op, TEXT("key"), Key))
	{
		return false;
	}
	URigHierarchy& Hierarchy = *Edit.Hierarchy();
	const FRigElementKey Wanted(To, Key.Type);
	if (Hierarchy.Contains(Wanted))
	{
		return Op.Fail(TEXT("ControlRig.NameTaken"), FString::Printf(TEXT("The hierarchy already has %s."), *KeyText(Wanted)), TEXT("to"));
	}
	const FRigControlElement* Control = Hierarchy.Find<FRigControlElement>(Key);
	FName Result = NAME_None;
	if (Control != nullptr && Control->IsAnimationChannel())
	{
		// The hierarchy tab renames a channel through its display name.
		Edit.Controller()->SetDisplayName(Key, To, true, false, false);
		Result = Hierarchy.Contains(Wanted) ? To : NAME_None;
	}
	else
	{
		Result = Edit.Controller()->RenameElement(Key, To, false, false, false).Name;
	}
	if (!CheckReported(Edit, Op, *FString::Printf(TEXT("Renaming %s"), *KeyText(Key))))
	{
		return false;
	}
	if (Result != To)
	{
		return Op.Fail(TEXT("ControlRig.NameTaken"), FString::Printf(TEXT("%s could not be renamed to '%s' (the hierarchy named it '%s')."),
			*KeyText(Key), *To.ToString(), *Result.ToString()), TEXT("to"));
	}
	Edit.bChanged = true;
	return true;
}

// The listed siblings (children of one first parent, or roots) put in the listed relative order; siblings that are
// not listed keep their place.
bool OpElementOrder(FEdit& Edit, FReader& Op)
{
	const FValue* Keys = nullptr;
	if (!Op.Array(TEXT("keys"), Keys))
	{
		return false;
	}
	TArray<FRigElementKey> Ordered;
	for (int32 Index = 0; Index < Keys->Elements.Num(); ++Index)
	{
		FRigElementKey Key;
		if (!KeyFromValue(Op, Keys->Elements[Index], *FString::Printf(TEXT("keys/%d"), Index), Key)
			|| !RequireElement(Edit, Op, *FString::Printf(TEXT("keys/%d"), Index), Key))
		{
			return false;
		}
		Ordered.Add(Key);
	}
	URigHierarchy& Hierarchy = *Edit.Hierarchy();
	auto Siblings = [&Hierarchy](const FRigElementKey& Key) -> TArray<FRigElementKey>
	{
		TArray<FRigElementKey> Result;
		const FRigBaseElement* Element = Hierarchy.Find(Key);
		if (const FRigBaseElement* Parent = Hierarchy.GetFirstParent(Element))
		{
			for (const FRigBaseElement* Child : Hierarchy.GetChildren(Parent))
			{
				Result.Add(Child->GetKey());
			}
		}
		else
		{
			for (const FRigBaseElement* Root : Hierarchy.GetRootElements())
			{
				Result.Add(Root->GetKey());
			}
		}
		return Result;
	};
	for (int32 Index = 1; Index < Ordered.Num(); ++Index)
	{
		const TArray<FRigElementKey> Current = Siblings(Ordered[Index]);
		const int32 Previous = Current.Find(Ordered[Index - 1]);
		const int32 Position = Current.Find(Ordered[Index]);
		if (Previous == INDEX_NONE || Position == INDEX_NONE)
		{
			return Op.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("%s and %s are not siblings."), *KeyText(Ordered[Index - 1]),
				*KeyText(Ordered[Index])), TEXT("keys"));
		}
		if (Position == Previous + 1)
		{
			continue;
		}
		Edit.Controller()->ReorderElement(Ordered[Index], Position > Previous ? Previous + 1 : Previous, false, false);
		Edit.bChanged = true;
	}
	return CheckReported(Edit, Op, TEXT("The reorder"));
}

// ---- variables -------------------------------------------------------------------------------------------------------

// The RigVM CPP type paths My Blueprint's type picker offers (design §3.2): bool, int32, float, double, FName, FString,
// FText, a struct or enum object path, and TArray<> of those. Anything else would hit a check in the engine.
bool CheckVariableType(FReader& Op, const TCHAR* Field, const FString& Type)
{
	FString Inner = Type;
	if (Inner.StartsWith(TEXT("TArray<")) && Inner.EndsWith(TEXT(">")))
	{
		Inner = Inner.Mid(7, Inner.Len() - 8).TrimStartAndEnd();
	}
	static const TCHAR* const Plain[] = {TEXT("bool"), TEXT("int32"), TEXT("float"), TEXT("double"), TEXT("FName"), TEXT("FString"), TEXT("FText")};
	for (const TCHAR* Each : Plain)
	{
		if (Inner == Each)
		{
			return true;
		}
	}
	if (!Inner.StartsWith(TEXT("TArray<")) && !Inner.StartsWith(TEXT("TMap<")) && Inner.StartsWith(TEXT("/")))
	{
		const UObject* Found = RigVMTypeUtils::FindObjectFromCPPTypeObjectPath(Inner);
		if (Found != nullptr && (Found->IsA<UScriptStruct>() || Found->IsA<UEnum>()))
		{
			return true;
		}
	}
	return Op.Fail(TEXT("ControlRig.UnsupportedVariableType"), FString::Printf(TEXT("'%s' is not a member variable type: bool, int32, float, "
		"double, FName, FString, FText, a struct or enum path (/Script/CoreUObject.Vector) or TArray<> of one (list_types kind "
		"rigvm_type)."), *Type), Field);
}

bool RequireVariable(FEdit& Edit, FReader& Op, const TCHAR* Field, FName Name)
{
	if (FBlueprintEditorUtils::FindNewVariableIndex(&Edit.Blueprint, Name) != INDEX_NONE)
	{
		return true;
	}
	TArray<FString> Names;
	for (const FBPVariableDescription& Variable : Edit.Blueprint.NewVariables)
	{
		Names.Add(Variable.VarName.ToString());
	}
	return Op.Fail(TEXT("ControlRig.VariableNotFound"), FString::Printf(TEXT("The rig has no member variable '%s'.%s"), *Name.ToString(),
		*UE::AssetDocument::DescribeNameCandidates(Name.ToString(), Names, TEXT("Variables"))), Field);
}

bool OpVariableAdd(FEdit& Edit, FReader& Op)
{
	FName Name;
	FString Type;
	if (!ReadName(Op, TEXT("name"), Name) || !Op.String(TEXT("type"), Type) || !CheckVariableType(Op, TEXT("type"), Type))
	{
		return false;
	}
	if (FBlueprintEditorUtils::FindNewVariableIndex(&Edit.Blueprint, Name) != INDEX_NONE)
	{
		return Op.Fail(TEXT("ControlRig.NameTaken"), FString::Printf(TEXT("The rig already has a member variable '%s'."), *Name.ToString()), TEXT("name"));
	}
	const FName Added = static_cast<URigVMBlueprint&>(Edit.Blueprint).AddMemberVariable(Name, Type, false, false, FString());
	if (Added.IsNone())
	{
		return Op.Fail(TEXT("ControlRig.EditFailed"), FString::Printf(TEXT("Member variable '%s' (%s) could not be added."), *Name.ToString(), *Type));
	}
	Edit.bChanged = true;
	if (Added != Name)
	{
		return Op.Fail(TEXT("ControlRig.NameTaken"), FString::Printf(TEXT("The new variable was named '%s', not '%s' (the name is taken "
			"or not a valid member name)."), *Added.ToString(), *Name.ToString()), TEXT("name"));
	}
	return true;
}

bool OpVariableRemove(FEdit& Edit, FReader& Op)
{
	FName Name;
	if (!ReadName(Op, TEXT("name"), Name) || !RequireVariable(Edit, Op, TEXT("name"), Name))
	{
		return false;
	}
	if (!static_cast<URigVMBlueprint&>(Edit.Blueprint).RemoveMemberVariable(Name))
	{
		return Op.Fail(TEXT("ControlRig.EditFailed"), FString::Printf(TEXT("Member variable '%s' could not be removed."), *Name.ToString()), TEXT("name"));
	}
	Edit.bChanged = true;
	return true;
}

bool OpVariableRename(FEdit& Edit, FReader& Op)
{
	FName Name;
	FName To;
	if (!ReadName(Op, TEXT("name"), Name) || !ReadName(Op, TEXT("to"), To) || !RequireVariable(Edit, Op, TEXT("name"), Name))
	{
		return false;
	}
	if (FBlueprintEditorUtils::FindNewVariableIndex(&Edit.Blueprint, To) != INDEX_NONE)
	{
		return Op.Fail(TEXT("ControlRig.NameTaken"), FString::Printf(TEXT("The rig already has a member variable '%s'."), *To.ToString()), TEXT("to"));
	}
	if (!static_cast<URigVMBlueprint&>(Edit.Blueprint).RenameMemberVariable(Name, To)
		|| FBlueprintEditorUtils::FindNewVariableIndex(&Edit.Blueprint, To) == INDEX_NONE)
	{
		return Op.Fail(TEXT("ControlRig.NameTaken"), FString::Printf(TEXT("Member variable '%s' could not be renamed to '%s'."), *Name.ToString(),
			*To.ToString()), TEXT("to"));
	}
	Edit.bChanged = true;
	return true;
}

bool OpVariableType(FEdit& Edit, FReader& Op)
{
	FName Name;
	FString Type;
	if (!ReadName(Op, TEXT("name"), Name) || !Op.String(TEXT("type"), Type) || !RequireVariable(Edit, Op, TEXT("name"), Name)
		|| !CheckVariableType(Op, TEXT("type"), Type))
	{
		return false;
	}
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	// Only the pin type is used; the variable keeps its own guid.
	const FRigVMExternalVariable Variable = RigVMTypeUtils::ExternalVariableFromCPPTypePath(FGuid(), Name, Type);
#else
	const FRigVMExternalVariable Variable = RigVMTypeUtils::ExternalVariableFromCPPTypePath(Name, Type);
#endif
	const FEdGraphPinType PinType = RigVMTypeUtils::PinTypeFromExternalVariable(Variable);
	if (!PinType.PinCategory.IsValid() || !static_cast<URigVMBlueprint&>(Edit.Blueprint).ChangeMemberVariableType(Name, PinType))
	{
		return Op.Fail(TEXT("ControlRig.EditFailed"), FString::Printf(TEXT("Member variable '%s' could not change to %s."), *Name.ToString(), *Type),
			TEXT("type"));
	}
	Edit.bChanged = true;
	return true;
}

void CompileIfNeeded(UBlueprint& Blueprint, FName Variable)
{
	const bool bCompiled = Blueprint.GeneratedClass != nullptr && Blueprint.GeneratedClass->GetDefaultObject(false) != nullptr
		&& (Blueprint.Status == BS_UpToDate || Blueprint.Status == BS_UpToDateWithWarnings || Blueprint.Status == BS_Error)
		&& FindFProperty<FProperty>(Blueprint.GeneratedClass, Variable) != nullptr;
	if (!bCompiled)
	{
		FCompilerResultsLog Log;
		Log.bSilentMode = true;
		FKismetEditorUtilities::CompileBlueprint(&Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Log);
	}
}

bool OpVariableSet(FEdit& Edit, FReader& Op)
{
	FName Name;
	if (!ReadName(Op, TEXT("name"), Name) || !RequireVariable(Edit, Op, TEXT("name"), Name))
	{
		return false;
	}
	// The variable Details setters are public on the asset interface (protected overrides on URigVMBlueprint).
	IRigVMAssetInterface& RigVM = Edit.Blueprint;
	bool bFlag = false;
	if (Op.Has(TEXT("public")))
	{
		if (!Op.Bool(TEXT("public"), bFlag))
		{
			return false;
		}
		// My Blueprint's eye toggle (Blueprint-only editable off = public). Not URigVMBlueprint::SetVariablePublic: in
		// UE 5.7 it passes the flag through unchanged as "blueprint only", the inverse of public.
		FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(&Edit.Blueprint, Name, !bFlag);
	}
	if (Op.Has(TEXT("read_only")))
	{
		if (!Op.Bool(TEXT("read_only"), bFlag))
		{
			return false;
		}
		FBlueprintEditorUtils::SetBlueprintPropertyReadOnlyFlag(&Edit.Blueprint, Name, bFlag);
	}
	if (Op.Has(TEXT("expose")))
	{
		if (!Op.Bool(TEXT("expose"), bFlag))
		{
			return false;
		}
		RigVM.SetVariableExposeToCinematics(Name, bFlag);
	}
	FString Text;
	if (Op.Has(TEXT("category")))
	{
		if (!Op.String(TEXT("category"), Text))
		{
			return false;
		}
		RigVM.SetVariableCategory(Name, Text.IsEmpty() ? FString(TEXT("Default")) : Text);
	}
	if (Op.Has(TEXT("tooltip")))
	{
		if (!Op.String(TEXT("tooltip"), Text))
		{
			return false;
		}
		RigVM.SetVariableTooltip(Name, FText::FromString(Text));
	}
	Edit.bChanged = true;
	const FValue* Default = Op.Find(TEXT("default"));
	if (Default == nullptr)
	{
		return true;
	}
	// The class default object holds the value (what the Details panel's Default Value edits); the text copy in the
	// variable description is cleared, as the next compile would import it over the value.
	CompileIfNeeded(Edit.Blueprint, Name);
	UObject* Defaults = Edit.Blueprint.GeneratedClass != nullptr ? Edit.Blueprint.GeneratedClass->GetDefaultObject() : nullptr;
	FProperty* Property = Edit.Blueprint.GeneratedClass != nullptr ? FindFProperty<FProperty>(Edit.Blueprint.GeneratedClass, Name) : nullptr;
	if (Defaults == nullptr || Property == nullptr)
	{
		return Op.Fail(TEXT("ControlRig.EditFailed"), TEXT("The rig does not compile, so the variable's default cannot be set."), TEXT("default"));
	}
	void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Defaults);
	Defaults->Modify();
	FString Error;
	if (Default->Kind == EAssetDocumentRuntimeValueKind::Null)
	{
		Property->ClearValue(ValuePtr);
	}
	else if (!DecodeProperty(*Property, ValuePtr, *Default, Error))
	{
		return Op.Fail(TEXT("ControlRig.InvalidValue"), FString::Printf(TEXT("default: %s"), *Error), TEXT("default"));
	}
	const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(&Edit.Blueprint, Name);
	if (Index != INDEX_NONE)
	{
		Edit.Blueprint.NewVariables[Index].DefaultValue.Empty();
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(&Edit.Blueprint);
	return true;
}

bool OpVariableOrder(FEdit& Edit, FReader& Op)
{
	const FValue* Names = nullptr;
	if (!Op.Array(TEXT("names"), Names))
	{
		return false;
	}
	TArray<FName> Ordered;
	for (int32 Index = 0; Index < Names->Elements.Num(); ++Index)
	{
		const FValue& Item = Names->Elements[Index];
		const FString Field = FString::Printf(TEXT("names/%d"), Index);
		if (Item.Kind != EAssetDocumentRuntimeValueKind::String || !RequireVariable(Edit, Op, *Field, FName(*Item.StringValue)))
		{
			return Item.Kind == EAssetDocumentRuntimeValueKind::String ? false : Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("names are strings."), *Field);
		}
		Ordered.Add(FName(*Item.StringValue));
	}
	for (int32 Index = Ordered.Num() - 2; Index >= 0; --Index)
	{
		FBlueprintEditorUtils::MoveVariableBeforeVariable(&Edit.Blueprint, nullptr, Ordered[Index], Ordered[Index + 1], true);
	}
	Edit.bChanged = true;
	return true;
}

// ---- asset ops -------------------------------------------------------------------------------------------------------

bool OpPreviewMesh(FEdit& Edit, FReader& Op)
{
	FString Path;
	if (!Op.String(TEXT("mesh"), Path))
	{
		return false;
	}
	USkeletalMesh* Mesh = nullptr;
	if (!Path.IsEmpty() && Path != TEXT("None"))
	{
		Mesh = LoadObject<USkeletalMesh>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (Mesh == nullptr)
		{
			return Op.Fail(TEXT("ControlRig.UnknownMesh"), FString::Printf(TEXT("'%s' is not a Skeletal Mesh."), *Path), TEXT("mesh"));
		}
	}
	// The picker sets only a different mesh (the Properties value is not readable in the Apply phase: compare here).
	if (Edit.Blueprint.GetPreviewMesh() == Mesh)
	{
		return true;
	}
	Edit.Blueprint.SetPreviewMesh(Mesh, true);
	Edit.bChanged = true;
	return true;
}

bool OpSourceImport(FEdit& Edit, FReader& Op)
{
	FString Property;
	FString Path;
	if (!Op.String(TEXT("property"), Property) || !Op.String(TEXT("path"), Path))
	{
		return false;
	}
	TSoftObjectPtr<UObject>* Slot = Property == TEXT("SourceHierarchyImport") ? &Edit.Blueprint.GetSourceHierarchyImport()
		: Property == TEXT("SourceCurveImport") ? &Edit.Blueprint.GetSourceCurveImport() : nullptr;
	if (Slot == nullptr)
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("'%s' is not an import source."), *Property), TEXT("property"));
	}
	UObject* Source = nullptr;
	if (!Path.IsEmpty() && Path != TEXT("None"))
	{
		Source = LoadObject<UObject>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (Source == nullptr || !(Source->IsA<USkeleton>() || Source->IsA<USkeletalMesh>()))
		{
			return Op.Fail(TEXT("ControlRig.UnknownMesh"), FString::Printf(TEXT("'%s' is not a Skeleton or Skeletal Mesh."), *Path), TEXT("path"));
		}
	}
	const FSoftObjectPath Wanted = Source != nullptr ? FSoftObjectPath(Source) : FSoftObjectPath();
	if (Slot->ToSoftObjectPath() == Wanted)
	{
		return true;
	}
	// The Details panel's write: the property itself (the import commands set it the same way).
	static_cast<UObject&>(Edit.Blueprint).Modify();
	*Slot = TSoftObjectPtr<UObject>(Wanted);
	Edit.bChanged = true;
	return true;
}

bool OpForwardsSolve(FEdit& Edit, FReader& Op)
{
	URigVMBlueprint& RigVM = Edit.Blueprint;
	URigVMController* Controller = RigVM.GetOrCreateController(RigVM.GetDefaultModel());
	if (Controller == nullptr)
	{
		return Op.Fail(TEXT("ControlRig.EditFailed"), TEXT("The rig has no default graph."));
	}
	// FControlRigBaseEditor::CreateEmptyGraphContentImpl (the editor's first open of a rig with an empty graph).
	if (Controller->AddUnitNode(FRigUnit_BeginExecution::StaticStruct(), FRigUnit::GetMethodName(), FVector2D::ZeroVector, FString(), false, false) == nullptr)
	{
		return Op.Fail(TEXT("ControlRig.EditFailed"), TEXT("The Forwards Solve event could not be added."));
	}
	Edit.bChanged = true;
	return true;
}

bool OpConvert(FEdit& Edit, FReader& Op, bool bToModule)
{
	FString Message;
	bool bConverted = false;
	if (bToModule)
	{
		bConverted = Edit.Blueprint.CanTurnIntoControlRigModule(false, &Message) && Edit.Blueprint.TurnIntoControlRigModule(false, &Message);
	}
	else
	{
		bConverted = Edit.Blueprint.CanTurnIntoStandaloneRig(&Message) && Edit.Blueprint.TurnIntoStandaloneRig(&Message);
	}
	if (!CheckReported(Edit, Op, bToModule ? TEXT("Turn into Rig Module") : TEXT("Turn into Standalone Rig")))
	{
		return false;
	}
	if (!bConverted)
	{
		return Op.Fail(TEXT("ControlRig.ConversionRefused"), FString::Printf(TEXT("%s is refused: %s"),
			bToModule ? TEXT("Turn into Rig Module") : TEXT("Turn into Standalone Rig"), Message.IsEmpty() ? TEXT("the rig cannot be converted.") : *Message));
	}
	Edit.bChanged = true;
	return true;
}

bool OpModuleSettings(FEdit& Edit, FReader& Op)
{
	const FValue* Settings = nullptr;
	if (!Op.Object(TEXT("settings"), Settings))
	{
		return false;
	}
	if (!Edit.Blueprint.IsControlRigModule())
	{
		return Op.Fail(TEXT("ControlRig.InvalidValue"), TEXT("Module settings belong to rig modules."), TEXT("settings"));
	}
	FReader Fields = Op.Child(TEXT("settings"), *Settings);
	if (!Fields.OnlyFields({TEXT("Name"), TEXT("Type"), TEXT("Icon"), TEXT("Category"), TEXT("Keywords"), TEXT("Description")}))
	{
		return false;
	}
	FRigModuleSettings Wanted;
	const FRigModuleSettings& Live = Edit.Blueprint.GetRigModuleSettings();
	Wanted = Live;
	const FRigModuleSettings Defaults;
	FString Text;
	Wanted.Identifier.Name = Edit.Blueprint.GetName();
	Wanted.Identifier.Type = Defaults.Identifier.Type;
	Wanted.Icon = Defaults.Icon;
	Wanted.Category = Defaults.Category;
	Wanted.Keywords = Defaults.Keywords;
	Wanted.Description = Defaults.Description;
	if (Fields.Has(TEXT("Name")))
	{
		if (!Fields.String(TEXT("Name"), Text))
		{
			return false;
		}
		Wanted.Identifier.Name = Text.IsEmpty() ? Edit.Blueprint.GetName() : Text;
	}
	if (Fields.Has(TEXT("Type")) && !Fields.String(TEXT("Type"), Wanted.Identifier.Type))
	{
		return false;
	}
	if (Fields.Has(TEXT("Icon")))
	{
		if (!Fields.String(TEXT("Icon"), Text))
		{
			return false;
		}
		Wanted.Icon = FSoftObjectPath(Text);
	}
	if ((Fields.Has(TEXT("Category")) && !Fields.String(TEXT("Category"), Wanted.Category))
		|| (Fields.Has(TEXT("Keywords")) && !Fields.String(TEXT("Keywords"), Wanted.Keywords))
		|| (Fields.Has(TEXT("Description")) && !Fields.String(TEXT("Description"), Wanted.Description)))
	{
		return false;
	}
	if (!FRigModuleSettings::StaticStruct()->CompareScriptStruct(&Wanted, &Live, PPF_None))
	{
		static_cast<UObject&>(Edit.Blueprint).Modify();
		Edit.Blueprint.GetRigModuleSettings() = Wanted;
		Edit.bChanged = true;
	}
	return true;
}

// ---- dispatch --------------------------------------------------------------------------------------------------------

bool RunVariableOp(FEdit& Edit, FReader& Op, const FString& Kind)
{
	if (Kind == TEXT("variable.add")) { return OpVariableAdd(Edit, Op); }
	if (Kind == TEXT("variable.remove")) { return OpVariableRemove(Edit, Op); }
	if (Kind == TEXT("variable.rename")) { return OpVariableRename(Edit, Op); }
	if (Kind == TEXT("variable.type")) { return OpVariableType(Edit, Op); }
	if (Kind == TEXT("variable.set")) { return OpVariableSet(Edit, Op); }
	if (Kind == TEXT("variable.order")) { return OpVariableOrder(Edit, Op); }
	return Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("Unknown op '%s'."), *Kind), TEXT("op"));
}

bool RunOp(FEdit& Edit, FGraphEditState& Graph, FReader& Op, const FString& Kind, int32 OpIndex)
{
	if (IsGraphOp(Kind) || IsFunctionOp(Kind))
	{
		Graph.Errors = &Edit.Errors;
		const bool bOk = IsGraphOp(Kind) ? RunGraphOp(Graph, Op, Kind, OpIndex) : RunFunctionOp(Graph, Op, Kind, OpIndex);
		Edit.bChanged |= Graph.bChanged;
		return bOk;
	}
	if (Kind.StartsWith(TEXT("variable.")))
	{
		// URigVMBlueprint moves variable nodes and pin bindings along with a member variable rename / retype / removal
		// from its Modify / OnChanged handlers (RunPreVariableChange, RunPostVariableChange), which it binds in PostLoad
		// only: a rig made in this session would not follow. Run them around the edit (a second post call finds
		// nothing left to propagate when the handlers are bound).
		URigVMBlueprint& RigVM = Edit.Blueprint;
		RunPreVariableChange(RigVM);
		const bool bOk = RunVariableOp(Edit, Op, Kind);
		RunPostVariableChange(RigVM);
		return bOk;
	}
	if (Kind == TEXT("preview_mesh")) { return OpPreviewMesh(Edit, Op); }
	if (Kind == TEXT("source_import")) { return OpSourceImport(Edit, Op); }
	if (Kind == TEXT("graph.forwards_solve")) { return OpForwardsSolve(Edit, Op); }
	if (Kind == TEXT("rig.to_module")) { return OpConvert(Edit, Op, true); }
	if (Kind == TEXT("rig.to_standalone")) { return OpConvert(Edit, Op, false); }
	if (Kind == TEXT("module.settings")) { return OpModuleSettings(Edit, Op); }
	if (Kind == TEXT("element.add")) { return OpElementAdd(Edit, Op); }
	if (Kind == TEXT("element.set")) { return OpElementSet(Edit, Op); }
	if (Kind == TEXT("element.remove")) { return OpElementRemove(Edit, Op); }
	if (Kind == TEXT("element.rename")) { return OpElementRename(Edit, Op); }
	if (Kind == TEXT("element.order")) { return OpElementOrder(Edit, Op); }
	if (IsModularOp(Kind)) { return RunModularOp(Edit.Blueprint, Op, Kind, Edit.bChanged, Graph.Notes, OpIndex); }
	return Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("Unknown op '%s'."), *Kind), TEXT("op"));
}

// Function edits never reach other packages. The controller follows a function's rename, removal, pin and colour
// changes into every reference the build data lists, loading the referencing rigs if needed (as the editor does), which
// would edit packages this apply neither snapshots nor saves. Those references are unregistered for the edit and
// registered again under their old identifier, so the other rigs keep what they reference until their next load or
// compile, and the apply warns about them (ControlRig.DependentsAffected). References inside the rig still follow.
class FForeignFunctionReferenceScope
{
public:
	explicit FForeignFunctionReferenceScope(UControlRigBlueprint& Blueprint)
	{
		URigVMBuildData* BuildData = URigVMBuildData::Get();
		const URigVMFunctionLibrary* Library = static_cast<URigVMBlueprint&>(Blueprint).GetLocalFunctionLibrary();
		if (BuildData == nullptr || Library == nullptr)
		{
			return;
		}
		const FString PackageName = Blueprint.GetPackage()->GetName();
		for (const URigVMLibraryNode* Function : Library->GetFunctions())
		{
			const FRigVMGraphFunctionIdentifier Identifier = Function->GetFunctionIdentifier();
			const FRigVMFunctionReferenceArray* References = BuildData->FindFunctionReferences(Identifier);
			if (References == nullptr)
			{
				continue;
			}
			for (const TSoftObjectPtr<URigVMFunctionReferenceNode>& Reference : References->FunctionReferences)
			{
				if (Reference.ToSoftObjectPath().GetLongPackageName() != PackageName)
				{
					Foreign.Emplace(Identifier, Reference);
				}
			}
		}
		for (const TPair<FRigVMGraphFunctionIdentifier, TSoftObjectPtr<URigVMFunctionReferenceNode>>& Entry : Foreign)
		{
			BuildData->UnregisterFunctionReference(Entry.Key, Entry.Value);
		}
	}
	~FForeignFunctionReferenceScope()
	{
		if (URigVMBuildData* BuildData = URigVMBuildData::Get())
		{
			for (const TPair<FRigVMGraphFunctionIdentifier, TSoftObjectPtr<URigVMFunctionReferenceNode>>& Entry : Foreign)
			{
				BuildData->RegisterFunctionReference(Entry.Key, Entry.Value);
			}
		}
	}
	FForeignFunctionReferenceScope(const FForeignFunctionReferenceScope&) = delete;
	FForeignFunctionReferenceScope& operator=(const FForeignFunctionReferenceScope&) = delete;

private:
	TArray<TPair<FRigVMGraphFunctionIdentifier, TSoftObjectPtr<URigVMFunctionReferenceNode>>> Foreign;
};

bool CheckOpFields(FReader& Op, const FValue& Value, const FString& Kind)
{
	static const TMap<FString, TArray<FString>> Fields = {
		{TEXT("preview_mesh"), {TEXT("mesh")}}, {TEXT("source_import"), {TEXT("property"), TEXT("path")}}, {TEXT("graph.forwards_solve"), {}}, {TEXT("rig.to_module"), {}},
		{TEXT("rig.to_standalone"), {}}, {TEXT("module.settings"), {TEXT("settings")}},
		{TEXT("variable.add"), {TEXT("name"), TEXT("type")}}, {TEXT("variable.remove"), {TEXT("name")}},
		{TEXT("variable.rename"), {TEXT("name"), TEXT("to")}}, {TEXT("variable.type"), {TEXT("name"), TEXT("type")}},
		{TEXT("variable.set"), {TEXT("name"), TEXT("public"), TEXT("read_only"), TEXT("expose"), TEXT("category"), TEXT("tooltip"), TEXT("default")}},
		{TEXT("variable.order"), {TEXT("names")}}, {TEXT("element.add"), {TEXT("entry")}}, {TEXT("element.set"), {TEXT("entry")}},
		{TEXT("element.remove"), {TEXT("key")}}, {TEXT("element.rename"), {TEXT("key"), TEXT("to")}}, {TEXT("element.order"), {TEXT("keys")}}};
	const TArray<FString>* Allowed = Fields.Find(Kind);
	if (Allowed == nullptr)
	{
		Allowed = ModularOpFields(Kind);
	}
	if (Allowed == nullptr)
	{
		return true; // RunOp names the unknown op
	}
	for (const FString& Key : Value.ObjectKeys)
	{
		if (Key != TEXT("op") && !Allowed->Contains(Key))
		{
			return Op.Fail(TEXT("Graph.UnknownField"), FString::Printf(TEXT("'%s' takes %s; '%s' is not one of them."), *Kind,
				Allowed->IsEmpty() ? TEXT("no fields") : *FString::Join(*Allowed, TEXT(", ")), *Key), Key);
		}
	}
	return true;
}
} // namespace

bool HandleEdit(FCall& Call, FObjectBuilder& Result)
{
	using namespace ControlRigEditLocal;
	const FValue* Ops = nullptr;
	if (!Call.Args.OnlyFields({TEXT("ops")}) || !Call.Args.Array(TEXT("ops"), Ops))
	{
		return false;
	}
	UControlRigBlueprint* Blueprint = Cast<UControlRigBlueprint>(&Call.Asset);
	if (Blueprint == nullptr)
	{
		return Fail(Call, TEXT("ControlRig.UnsupportedAsset"), TEXT("rig.controlrig.edit.v1 edits Control Rig assets."), TEXT("/HandleArguments/asset"));
	}
	if (Blueprint->GetHierarchy() == nullptr)
	{
		return Fail(Call, TEXT("ControlRig.EditFailed"), TEXT("The rig has no hierarchy."));
	}
	FAssetDocumentEditorNonInteractiveScope NonInteractive;
	const FScopedTransaction Transaction(LOCTEXT("EditControlRig", "Edit Control Rig"));
	static_cast<UObject*>(Blueprint)->Modify();
	Blueprint->GetHierarchy()->Modify();
	const FAutoRecompileScope AutoRecompile(*Blueprint);
	PointVariableNodesAtVariables(*Blueprint);
	FEdit Edit{*Blueprint};
	FGraphEditState Graph(*Blueprint);
	// Set up after the leading function.dependents ops, which read the references this scope hides.
	TOptional<FForeignFunctionReferenceScope> ForeignReferences;
	for (int32 Index = 0; Index < Ops->Elements.Num(); ++Index)
	{
		const FString OpPath = FString::Printf(TEXT("ops/%d"), Index);
		FReader Op = Call.Args.Child(OpPath, Ops->Elements[Index]);
		FString Kind;
		if (!Op.IsObject())
		{
			return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("Each op is an object."));
		}
		Edit.Errors.Reset();
		bool bOk = false;
		{
			const FCaptureScope Capture(Edit);
			bOk = Op.String(TEXT("op"), Kind) && CheckOpFields(Op, Ops->Elements[Index], Kind);
			if (bOk && !ForeignReferences.IsSet() && Kind != TEXT("function.dependents"))
			{
				ForeignReferences.Emplace(*Blueprint);
			}
			bOk = bOk && RunOp(Edit, Graph, Op, Kind, Index);
		}
		if (!bOk)
		{
			if (Call.Error.Code.IsEmpty())
			{
				// An op that stopped without saying why: name it and what the editor reported.
				return Op.Fail(TEXT("ControlRig.EditFailed"), FString::Printf(TEXT("The %s op failed%s."), *Kind,
					Edit.Errors.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(": %s"), *FString::Join(Edit.Errors, TEXT("; ")).Left(1000))));
			}
			return false;
		}
		if (NonInteractive.WasInteractionRefused())
		{
			NonInteractive.DiscardCreatedPackages();
			return Op.Fail(TEXT("ControlRig.InteractiveEditRefused"), TEXT("The edit tried to open an editor dialog, which an MCP request "
				"cannot answer; the edit was refused."));
		}
	}
	if (Edit.bChanged)
	{
		Blueprint->MarkPackageDirty();
	}
	Result.AddBool(TEXT("changed"), Edit.bChanged);
	if (!Graph.Notes.IsEmpty())
	{
		Result.Add(TEXT("notes"), MakeArray(MoveTemp(Graph.Notes)));
	}
	return true;
}
} // namespace UE::AssetDocument::Graph::Rig::ControlRig

#undef LOCTEXT_NAMESPACE
