// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// rig.controlrig.read.v1: the authored state of a Control Rig (docs/architecture/asset-document-controlrig-design.md
// §2.1, §3) as document-ready records. The rig is read as stored after load (PostLoad / HandlePackageDone already
// patched it, as the editor shows it); nothing compiles, initializes a VM, refreshes a model or dirties the package.
//
// Args {offset?, detail?: "document" | "inspect", token?}. Records (paged under the value budget; offset 0 builds
// the records and returns a token, later pages with that token reuse them):
//   {kind: "header", rig_type, parent_class, source_hierarchy_import, source_curve_import, module?, modular_settings?, primary_connectors?, counts{...}, skipped?{Components}, facts?}
//   {kind: "variable", entry: {Name, Type, Default?, Public?, ReadOnly?, ExposeToCinematics?, Category?, Tooltip?}}
//   {kind: "element", entry: {Type, Name, ...}, global?}           (Rig / Module only; design §3.4)
//   {kind: "graph", name}                                          (one per model, default model "Rig")
//   {kind: "function", name, entry: {Public?, Mutable?, ..., Inputs, Outputs, LocalVariables?}}
//   {kind: "node", scope[], name, entry, pins?}                     (AssetDocumentControlRigGraphRead.cpp)
//   {kind: "interface", scope[], name: "Entry" | "Return", entry: {Position}}
//   {kind: "module", entry: {Name, Class, Parent?, Config?, Bindings?}}
//   {kind: "connections", entries: {"<Module>/<Connector>": key | [keys]}}
// detail "inspect" adds the global initial transform of each transformable element, root pins per node and header
// facts {shapes[], public_functions[], events[], compile, modular?{auto_connect, candidates, unresolved_primary}}.

#include "AssetDocumentControlRig.h"

#include "AssetDocumentGraphValue.h"
#include "ControlRig.h"
#include "ControlRigBlueprintLegacy.h"
#include "ControlRigGizmoLibrary.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "ModularRigModel.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyElements.h"
#include "Rigs/RigHierarchyMetadata.h"
#include "Rigs/RigModuleDefines.h"
#include "RigVMModel/RigVMFunctionLibrary.h"
#include "RigVMModel/RigVMGraph.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"

namespace UE::AssetDocument::Graph::Rig::ControlRig
{
namespace ControlRigReadLocal
{
double Clean(double Value)
{
	return Value == 0.0 ? 0.0 : Value;
}

FValue Number(double Value)
{
	return MakeNumber(Clean(ShortestFloat(Value)));
}

FValue Vector3(double X, double Y, double Z)
{
	FObjectBuilder Object;
	Object.Add(TEXT("X"), Number(X));
	Object.Add(TEXT("Y"), Number(Y));
	Object.Add(TEXT("Z"), Number(Z));
	return Object.Build();
}

FValue Rotator(double Pitch, double Yaw, double Roll)
{
	FObjectBuilder Object;
	Object.Add(TEXT("Pitch"), Number(Pitch));
	Object.Add(TEXT("Yaw"), Number(Yaw));
	Object.Add(TEXT("Roll"), Number(Roll));
	return Object.Build();
}

FString KeyString(const FRigElementKey& Key)
{
	return Key.IsValid() ? Key.ToString() : FString();
}

// Identity at the float precision hierarchy transforms are stored at (see ElementTransform).
bool IsIdentity(const FTransform& Transform)
{
	const FVector Translation = Transform.GetTranslation();
	const FQuat Rotation = Transform.GetRotation();
	const FVector Scale = Transform.GetScale3D();
	return static_cast<float>(Translation.X) == 0.f && static_cast<float>(Translation.Y) == 0.f && static_cast<float>(Translation.Z) == 0.f
		&& static_cast<float>(Rotation.X) == 0.f && static_cast<float>(Rotation.Y) == 0.f && static_cast<float>(Rotation.Z) == 0.f
		&& static_cast<float>(Rotation.W) == 1.f
		&& static_cast<float>(Scale.X) == 1.f && static_cast<float>(Scale.Y) == 1.f && static_cast<float>(Scale.Z) == 1.f;
}

// Hierarchy transforms are stored at float precision: a transform computed in memory (an element added in global
// space) reads back rounded after a save and reload. They are written as the shortest decimal of that float, so a
// read before and after reload agree.
FValue ElementTransform(const FTransform& Transform)
{
	FObjectBuilder Object;
	const FVector Translation = Transform.GetTranslation();
	if (!(static_cast<float>(Translation.X) == 0.f && static_cast<float>(Translation.Y) == 0.f && static_cast<float>(Translation.Z) == 0.f))
	{
		Object.Add(TEXT("Translation"), Vector3(Translation.X, Translation.Y, Translation.Z));
	}
	const FQuat Rotation = Transform.GetRotation();
	if (!(static_cast<float>(Rotation.X) == 0.f && static_cast<float>(Rotation.Y) == 0.f && static_cast<float>(Rotation.Z) == 0.f
		&& static_cast<float>(Rotation.W) == 1.f))
	{
		FObjectBuilder Quat;
		Quat.Add(TEXT("X"), Number(Rotation.X));
		Quat.Add(TEXT("Y"), Number(Rotation.Y));
		Quat.Add(TEXT("Z"), Number(Rotation.Z));
		Quat.Add(TEXT("W"), Number(Rotation.W));
		Object.Add(TEXT("Rotation"), Quat.Build());
	}
	const FVector Scale = Transform.GetScale3D();
	if (!(static_cast<float>(Scale.X) == 1.f && static_cast<float>(Scale.Y) == 1.f && static_cast<float>(Scale.Z) == 1.f))
	{
		Object.Add(TEXT("Scale3D"), Vector3(Scale.X, Scale.Y, Scale.Z));
	}
	return Object.Build();
}

FString TypeLabel(ERigElementType Type)
{
	return StaticEnum<ERigElementType>()->GetNameStringByValue(static_cast<int64>(Type));
}

// ---- control values ------------------------------------------------------------------------------------------

constexpr float ValueNoise = 1e-5f;

// A control value typed by its control type (design §3.4): numbers, {X,Y}, {X,Y,Z}, {Pitch,Yaw,Roll}, transforms.
FValue EncodeControlValue(const FRigControlValue& Value, const FRigControlSettings& Settings)
{
	switch (Settings.ControlType)
	{
	case ERigControlType::Bool:
		return MakeBool(Value.Get<bool>());
	case ERigControlType::Float:
	case ERigControlType::ScaleFloat:
		return Number(Value.Get<float>());
	case ERigControlType::Integer:
	{
		const int32 Integer = Value.Get<int32>();
		if (Settings.ControlEnum != nullptr)
		{
			const FString Entry = Settings.ControlEnum->GetNameStringByValue(Integer);
			if (!Entry.IsEmpty())
			{
				return MakeString(Entry);
			}
		}
		return MakeInt(Integer);
	}
	case ERigControlType::Vector2D:
	{
		const FVector3f Vector = Value.Get<FVector3f>();
		FObjectBuilder Object;
		Object.Add(TEXT("X"), Number(Vector.X));
		Object.Add(TEXT("Y"), Number(Vector.Y));
		return Object.Build();
	}
	case ERigControlType::Position:
	case ERigControlType::Scale:
	{
		const FVector3f Vector = Value.Get<FVector3f>();
		return Vector3(Vector.X, Vector.Y, Vector.Z);
	}
	case ERigControlType::Rotator:
	{
		// Stored as Euler (X = roll, Y = pitch, Z = yaw: FRotator::MakeFromEuler in GetAsTransform).
		const FVector3f Euler = Value.Get<FVector3f>();
		return Rotator(Euler.Y, Euler.Z, Euler.X);
	}
	case ERigControlType::Transform:
		return EncodeTransform(Value.Get<FRigControlValue::FTransform_Float>().ToTransform());
	case ERigControlType::TransformNoScale:
		return EncodeTransform(Value.Get<FRigControlValue::FTransformNoScale_Float>().ToTransform().ToFTransform());
	case ERigControlType::EulerTransform:
	{
		// The hierarchy derives a control's value from its stored pose on load: a zero comes back as float noise
		// (3e-7) and reads as zero.
		FRigControlValue::FEulerTransform_Float Euler = Value.GetRef<FRigControlValue::FEulerTransform_Float>();
		for (float* Component : {&Euler.TranslationX, &Euler.TranslationY, &Euler.TranslationZ, &Euler.RotationPitch, &Euler.RotationYaw,
			&Euler.RotationRoll})
		{
			*Component = FMath::Abs(*Component) < ValueNoise ? 0.f : *Component;
		}
		FObjectBuilder Object;
		if (!(Euler.TranslationX == 0.f && Euler.TranslationY == 0.f && Euler.TranslationZ == 0.f))
		{
			Object.Add(TEXT("Location"), Vector3(Euler.TranslationX, Euler.TranslationY, Euler.TranslationZ));
		}
		if (!(Euler.RotationPitch == 0.f && Euler.RotationYaw == 0.f && Euler.RotationRoll == 0.f))
		{
			Object.Add(TEXT("Rotation"), Rotator(Euler.RotationPitch, Euler.RotationYaw, Euler.RotationRoll));
		}
		if (!(Euler.ScaleX == 1.f && Euler.ScaleY == 1.f && Euler.ScaleZ == 1.f))
		{
			Object.Add(TEXT("Scale"), Vector3(Euler.ScaleX, Euler.ScaleY, Euler.ScaleZ));
		}
		return Object.Build();
	}
	default:
		return MakeNull();
	}
}

// The identity value of a control type (what an omitted Value means).
FValue IdentityControlValue(const FRigControlSettings& Settings)
{
	switch (Settings.ControlType)
	{
	case ERigControlType::Bool: return MakeBool(false);
	case ERigControlType::Float: return Number(0.0);
	case ERigControlType::ScaleFloat: return Number(1.0);
	case ERigControlType::Integer:
		if (Settings.ControlEnum != nullptr)
		{
			const FString Entry = Settings.ControlEnum->GetNameStringByValue(0);
			if (!Entry.IsEmpty())
			{
				return MakeString(Entry);
			}
		}
		return MakeInt(0);
	case ERigControlType::Vector2D:
	{
		FObjectBuilder Object;
		Object.Add(TEXT("X"), Number(0.0));
		Object.Add(TEXT("Y"), Number(0.0));
		return Object.Build();
	}
	case ERigControlType::Position: return Vector3(0.0, 0.0, 0.0);
	case ERigControlType::Scale: return Vector3(1.0, 1.0, 1.0);
	case ERigControlType::Rotator: return Rotator(0.0, 0.0, 0.0);
	default: return MakeEmptyObject();
	}
}

bool SameValue(const FValue& A, const FValue& B)
{
	return ValueToJsonString(A, false) == ValueToJsonString(B, false);
}

} // namespace

bool IsSkippedControlField(const FProperty& Property)
{
	static const FName Skipped[] = {TEXT("MinimumValue"), TEXT("MaximumValue"), TEXT("Customization"), TEXT("bIsCurve"),
		TEXT("bIsTransientControl"), TEXT("ShapeTransform"), TEXT("PreviouslyDrivenControls")};
	if (Property.HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
	{
		return true;
	}
	for (const FName& Name : Skipped)
	{
		if (Property.GetFName() == Name)
		{
			return true;
		}
	}
	return false;
}

namespace ControlRigReadLocal
{
// Every field of Struct whose value differs from Defaults, plus the Always fields, in declaration order.
bool EncodeStructSparse(const UScriptStruct& Struct, const void* Value, const void* Defaults, TConstArrayView<FName> Always,
	TFunctionRef<bool(const FProperty&)> Skip, FValue& OutObject, FString& OutError)
{
	FObjectBuilder Object;
	for (TFieldIterator<FProperty> It(&Struct); It; ++It)
	{
		const FProperty& Property = **It;
		if (Skip(Property))
		{
			continue;
		}
		const bool bAlways = Always.Contains(Property.GetFName());
		if (!bAlways && Property.Identical_InContainer(Value, Defaults, 0, PPF_None))
		{
			continue;
		}
		FValue Field;
		if (!EncodeProperty(Property, Property.ContainerPtrToValuePtr<void>(Value), Field, OutError))
		{
			return false;
		}
		Object.Add(Property.GetName(), MoveTemp(Field));
	}
	OutObject = Object.Build();
	return true;
}

// ---- hierarchy -------------------------------------------------------------------------------------------------

void AddParents(URigHierarchy& Hierarchy, const FRigBaseElement& Element, bool bWeighted, FObjectBuilder& Entry)
{
	const FRigElementKey Key = Element.GetKey();
	const TArray<FRigElementKey> Parents = Hierarchy.GetParents(Key);
	if (Parents.IsEmpty())
	{
		return;
	}
	if (!bWeighted)
	{
		Entry.AddString(TEXT("Parent"), KeyString(Parents[0]));
		return;
	}
	// An animation channel's parent weights are fixed (URigHierarchy::SetParentWeight refuses them) and move nothing:
	// the hierarchy controller adds a channel at weight 0, older versions at 1. They are not read.
	const FRigControlElement* Control = Cast<FRigControlElement>(&Element);
	const bool bChannel = Control != nullptr && Control->IsAnimationChannel();
	const TArray<FRigElementWeight> Weights = bChannel ? TArray<FRigElementWeight>() : Hierarchy.GetParentWeightArray(&Element, true);
	bool bPlain = Parents.Num() == 1;
	TArray<FValue> List;
	for (int32 Index = 0; Index < Parents.Num(); ++Index)
	{
		FObjectBuilder Parent;
		Parent.AddString(TEXT("Parent"), KeyString(Parents[Index]));
		if (Weights.IsValidIndex(Index))
		{
			const FRigElementWeight& Weight = Weights[Index];
			if (Weight.Location == Weight.Rotation && Weight.Location == Weight.Scale)
			{
				Parent.Add(TEXT("Weight"), Number(Weight.Location));
				bPlain = bPlain && Weight.Location == 1.f;
			}
			else
			{
				FObjectBuilder Split;
				Split.Add(TEXT("Location"), Number(Weight.Location));
				Split.Add(TEXT("Rotation"), Number(Weight.Rotation));
				Split.Add(TEXT("Scale"), Number(Weight.Scale));
				Parent.Add(TEXT("Weight"), Split.Build());
				bPlain = false;
			}
		}
		// The authored label of the parent constraint (GetDisplayLabelForParent falls back to "Parent" or the parent's
		// name, which is display text, not data).
		FName Label = NAME_None;
		if (const FRigMultiParentElement* MultiParent = Hierarchy.Find<FRigMultiParentElement>(Key))
		{
			const int32* ConstraintIndex = MultiParent->IndexLookup.Find(Parents[Index]);
			if (ConstraintIndex != nullptr && MultiParent->ParentConstraints.IsValidIndex(*ConstraintIndex))
			{
				Label = MultiParent->ParentConstraints[*ConstraintIndex].DisplayLabel;
			}
		}
		if (!Label.IsNone())
		{
			Parent.Add(TEXT("Label"), MakeName(Label));
			bPlain = false;
		}
		List.Add(Parent.Build());
	}
	if (bPlain)
	{
		Entry.AddString(TEXT("Parent"), KeyString(Parents[0]));
	}
	else
	{
		Entry.Add(TEXT("Parents"), MakeArray(MoveTemp(List)));
	}
}

FValue SpaceList(const TArray<FRigElementKeyWithLabel>& Spaces)
{
	TArray<FValue> List;
	for (const FRigElementKeyWithLabel& Space : Spaces)
	{
		if (Space.Label.IsNone())
		{
			List.Add(MakeString(KeyString(Space.Key)));
		}
		else
		{
			FObjectBuilder Labeled;
			Labeled.AddString(TEXT("Space"), KeyString(Space.Key));
			Labeled.Add(TEXT("Label"), MakeName(Space.Label));
			List.Add(Labeled.Build());
		}
	}
	return MakeArray(MoveTemp(List));
}

bool AddControl(URigHierarchy& Hierarchy, FRigControlElement& Control, FObjectBuilder& Entry, FString& OutError)
{
	const FRigControlSettings& Settings = Control.Settings;
	FRigControlSettings Defaults;
	Defaults.ControlType = Settings.ControlType;
	Defaults.SetupLimitArrayForType(false, false, false);
	FValue SettingsValue;
	const FName Always[] = {TEXT("ControlType")};
	if (!EncodeStructSparse(*FRigControlSettings::StaticStruct(), &Settings, &Defaults, Always, &IsSkippedControlField,
		SettingsValue, OutError))
	{
		return false;
	}
	Entry.Add(TEXT("Settings"), MoveTemp(SettingsValue));

	const FTransform Offset = Hierarchy.GetControlOffsetTransform(&Control, ERigTransformType::InitialLocal);
	if (!IsIdentity(Offset))
	{
		Entry.Add(TEXT("Offset"), ElementTransform(Offset));
	}
	const FTransform Shape = Hierarchy.GetControlShapeTransform(&Control, ERigTransformType::InitialLocal);
	if (!IsIdentity(Shape))
	{
		Entry.Add(TEXT("Shape"), ElementTransform(Shape));
	}
	FValue Value = EncodeControlValue(Hierarchy.GetControlValue(&Control, ERigControlValueType::Initial), Settings);
	if (!SameValue(Value, IdentityControlValue(Settings)))
	{
		Entry.Add(TEXT("Value"), MoveTemp(Value));
	}
	bool bLimited = false;
	for (const FRigControlLimitEnabled& Limit : Settings.LimitEnabled)
	{
		bLimited = bLimited || Limit.IsOn();
	}
	if (bLimited)
	{
		Entry.Add(TEXT("Minimum"), EncodeControlValue(Hierarchy.GetControlValue(&Control, ERigControlValueType::Minimum), Settings));
		Entry.Add(TEXT("Maximum"), EncodeControlValue(Hierarchy.GetControlValue(&Control, ERigControlValueType::Maximum), Settings));
	}
	const TArray<FRigElementKeyWithLabel>& Spaces = Settings.Customization.AvailableSpaces;
	if (!Spaces.IsEmpty())
	{
		Entry.Add(Control.IsAnimationChannel() ? TEXT("Hosts") : TEXT("Spaces"), SpaceList(Spaces));
	}
	return true;
}

bool AddMetadata(URigHierarchy& Hierarchy, const FRigBaseElement& Element, FObjectBuilder& Entry, FString& OutError)
{
	const FRigElementKey Key = Element.GetKey();
	TArray<FName> Names = Hierarchy.GetMetadataNames(Key);
	if (Element.GetType() == ERigElementType::Socket)
	{
		Names.Remove(FRigSocketElement::ColorMetaName);
		Names.Remove(FRigSocketElement::DescriptionMetaName);
	}
	if (Names.IsEmpty())
	{
		return true;
	}
	Names.Sort(FNameLexicalLess());
	FObjectBuilder Metadata;
	for (const FName& Name : Names)
	{
		const ERigMetadataType Type = Hierarchy.GetMetadataType(Key, Name);
		const FRigBaseMetadata* Found = Hierarchy.FindMetadataForElement(&Element, Name, Type);
		const UScriptStruct* MetadataStruct = Found != nullptr ? Found->GetMetadataStruct() : nullptr;
		const FProperty* ValueProperty = MetadataStruct != nullptr ? MetadataStruct->FindPropertyByName(TEXT("Value")) : nullptr;
		if (ValueProperty == nullptr)
		{
			continue;
		}
		FValue Value;
		if (!EncodeProperty(*ValueProperty, ValueProperty->ContainerPtrToValuePtr<void>(Found), Value, OutError))
		{
			return false;
		}
		FObjectBuilder Item;
		Item.AddString(TEXT("Type"), StaticEnum<ERigMetadataType>()->GetNameStringByValue(static_cast<int64>(Type)));
		Item.Add(TEXT("Value"), MoveTemp(Value));
		Metadata.Add(Name.ToString(), Item.Build());
	}
	Entry.Add(TEXT("Metadata"), Metadata.Build());
	return true;
}

bool AddElements(UControlRigBlueprint& Blueprint, bool bInspect, FCall& Call, TArray<FValue>& Records, int32& OutCount)
{
	URigHierarchy* Hierarchy = Blueprint.GetHierarchy();
	if (Hierarchy == nullptr)
	{
		return true;
	}
	const FSourcePose SourcePose(Blueprint);
	for (const FRigElementKey& Key : Hierarchy->GetAllKeys(true))
	{
		FRigBaseElement* Element = Hierarchy->Find(Key);
		if (Element == nullptr)
		{
			continue;
		}
		FObjectBuilder Entry;
		Entry.AddString(TEXT("Type"), TypeLabel(Key.Type));
		Entry.Add(TEXT("Name"), MakeName(Key.Name));
		FString Error;
		bool bTransform = false;
		FValue Reference;
		bool bHasReference = false;
		switch (Key.Type)
		{
		case ERigElementType::Bone:
		{
			const FRigBoneElement* Bone = static_cast<const FRigBoneElement*>(Element);
			AddParents(*Hierarchy, *Element, false, Entry);
			const bool bImported = Bone->BoneType == ERigBoneType::Imported;
			if (bImported)
			{
				Entry.AddBool(TEXT("Imported"), true);
			}
			const FTransform Local = Hierarchy->GetLocalTransform(Key, true);
			const bool bReference = bImported && SourcePose.bValid && SourcePose.Matches(Key.Name, Local);
			if (!bReference && !IsIdentity(Local))
			{
				Entry.Add(TEXT("Transform"), ElementTransform(Local));
			}
			else if (bReference && !IsIdentity(Local))
			{
				// Left out at the reference pose: Diff compares an authored Transform with it.
				Reference = ElementTransform(Local);
				bHasReference = true;
			}
			bTransform = true;
			break;
		}
		case ERigElementType::Null:
		case ERigElementType::Reference:
		{
			AddParents(*Hierarchy, *Element, Key.Type == ERigElementType::Null, Entry);
			const FTransform Local = Hierarchy->GetLocalTransform(Key, true);
			if (!IsIdentity(Local))
			{
				Entry.Add(TEXT("Transform"), ElementTransform(Local));
			}
			bTransform = true;
			break;
		}
		case ERigElementType::Socket:
		{
			const FRigSocketElement* Socket = static_cast<const FRigSocketElement*>(Element);
			AddParents(*Hierarchy, *Element, false, Entry);
			const FTransform Local = Hierarchy->GetLocalTransform(Key, true);
			if (!IsIdentity(Local))
			{
				Entry.Add(TEXT("Transform"), ElementTransform(Local));
			}
			if (Hierarchy->GetMetadataNames(Key).Contains(FRigSocketElement::ColorMetaName))
			{
				const FLinearColor Color = Socket->GetColor(Hierarchy);
				FValue ColorValue;
				if (!EncodeProperty(*FindFProperty<FStructProperty>(FRigControlSettings::StaticStruct(), TEXT("ShapeColor")), &Color,
					ColorValue, Error))
				{
					return Fail(Call, TEXT("ControlRig.ReadFailed"), Error);
				}
				Entry.Add(TEXT("Color"), MoveTemp(ColorValue));
			}
			const FString Description = Socket->GetDescription(Hierarchy);
			if (!Description.IsEmpty())
			{
				Entry.AddString(TEXT("Description"), Description);
			}
			bTransform = true;
			break;
		}
		case ERigElementType::Control:
		{
			FRigControlElement* Control = static_cast<FRigControlElement*>(Element);
			AddParents(*Hierarchy, *Element, true, Entry);
			if (!AddControl(*Hierarchy, *Control, Entry, Error))
			{
				return Fail(Call, TEXT("ControlRig.ReadFailed"), FString::Printf(TEXT("%s: %s"), *KeyString(Key), *Error));
			}
			bTransform = true;
			break;
		}
		case ERigElementType::Curve:
		{
			const FRigCurveElement* Curve = static_cast<const FRigCurveElement*>(Element);
			if (Curve->IsValueSet())
			{
				Entry.Add(TEXT("Value"), Number(Curve->Get()));
			}
			break;
		}
		case ERigElementType::Connector:
		{
			const FRigConnectorElement* Connector = static_cast<const FRigConnectorElement*>(Element);
			const FRigConnectorSettings Defaults = FRigConnectorSettings::DefaultSettings();
			FValue SettingsValue;
			if (!EncodeStructSparse(*FRigConnectorSettings::StaticStruct(), &Connector->Settings, &Defaults, {},
				[](const FProperty& Property) { return Property.HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated); },
				SettingsValue, Error))
			{
				return Fail(Call, TEXT("ControlRig.ReadFailed"), FString::Printf(TEXT("%s: %s"), *KeyString(Key), *Error));
			}
			if (!SettingsValue.ObjectKeys.IsEmpty())
			{
				Entry.Add(TEXT("Settings"), MoveTemp(SettingsValue));
			}
			break;
		}
		default:
			break;
		}
		if (!AddMetadata(*Hierarchy, *Element, Entry, Error))
		{
			return Fail(Call, TEXT("ControlRig.ReadFailed"), FString::Printf(TEXT("%s: %s"), *KeyString(Key), *Error));
		}
		FObjectBuilder Record;
		Record.AddString(TEXT("kind"), TEXT("element"));
		Record.Add(TEXT("entry"), Entry.Build());
		if (bHasReference)
		{
			Record.Add(TEXT("reference"), MoveTemp(Reference));
		}
		if (bInspect && bTransform)
		{
			Record.Add(TEXT("global"), ElementTransform(Hierarchy->GetGlobalTransform(Key, true)));
		}
		Records.Add(Record.Build());
		++OutCount;
	}
	return true;
}

// ---- variables -------------------------------------------------------------------------------------------------

bool AddVariables(UControlRigBlueprint& Blueprint, FTextValues& Values, TArray<FValue>& Records, int32& OutCount)
{
	const UObject* ClassDefaults = Blueprint.GeneratedClass != nullptr ? Blueprint.GeneratedClass->GetDefaultObject(false) : nullptr;
	for (const FRigVMGraphVariableDescription& Variable : Blueprint.GetAssetVariables())
	{
		FObjectBuilder Entry;
		Entry.Add(TEXT("Name"), MakeName(Variable.Name));
		Entry.AddString(TEXT("Type"), TypeName(Variable.CPPType, Variable.CPPTypeObject));
		FValue Default;
		bool bHasDefault = false;
		FString Error;
		const FProperty* Property = ClassDefaults != nullptr ? FindFProperty<FProperty>(ClassDefaults->GetClass(), Variable.Name) : nullptr;
		if (Property != nullptr && EncodeProperty(*Property, Property->ContainerPtrToValuePtr<void>(ClassDefaults), Default, Error))
		{
			// The compiled class default (what the Details panel shows); a value that reads as the type's own default is
			// omitted (an element key without a name reads "" whatever its type, as a new variable's does).
			TArray<uint8, TAlignedHeapAllocator<16>> Memory;
			Memory.SetNumZeroed(Property->GetSize());
			Property->InitializeValue(Memory.GetData());
			FValue Initial;
			FString InitialError;
			bHasDefault = EncodeProperty(*Property, Memory.GetData(), Initial, InitialError)
				? !SameValue(Default, Initial)
				: !Property->Identical(Property->ContainerPtrToValuePtr<void>(ClassDefaults), Memory.GetData(), PPF_None);
			Property->DestroyValue(Memory.GetData());
		}
		else if (!Variable.DefaultValue.IsEmpty())
		{
			bHasDefault = Values.Encode(Variable.CPPType, Variable.CPPTypeObject, Variable.DefaultValue, Default, Error);
			if (!bHasDefault)
			{
				Default = MakeString(Variable.DefaultValue);
				bHasDefault = true;
			}
		}
		if (bHasDefault)
		{
			Entry.Add(TEXT("Default"), MoveTemp(Default));
		}
		if (Variable.bPublic)
		{
			Entry.AddBool(TEXT("Public"), true);
		}
		const FBPVariableDescription* Description = Blueprint.NewVariables.FindByPredicate(
			[&Variable](const FBPVariableDescription& Candidate) { return Candidate.VarName == Variable.Name; });
		if (Description != nullptr && (Description->PropertyFlags & CPF_BlueprintReadOnly) != 0)
		{
			Entry.AddBool(TEXT("ReadOnly"), true);
		}
		if (Variable.bExposeToCinematics)
		{
			Entry.AddBool(TEXT("ExposeToCinematics"), true);
		}
		const FString Category = Variable.Category.ToString();
		if (!Category.IsEmpty() && Category != TEXT("Default"))
		{
			Entry.AddString(TEXT("Category"), Category);
		}
		const FString Tooltip = Variable.Tooltip.ToString();
		if (!Tooltip.IsEmpty())
		{
			Entry.AddString(TEXT("Tooltip"), Tooltip);
		}
		FObjectBuilder Record;
		Record.AddString(TEXT("kind"), TEXT("variable"));
		Record.Add(TEXT("entry"), Entry.Build());
		Records.Add(Record.Build());
		++OutCount;
	}
	return true;
}

// ---- modular ---------------------------------------------------------------------------------------------------

bool AddModules(UControlRigBlueprint& Blueprint, FCall& Call, TArray<FValue>& Records, int32& OutModules, int32& OutConnections)
{
	const FModularRigModel& Model = Blueprint.GetModularRigModel();
	for (const FRigModuleReference& Module : Model.Modules)
	{
		FObjectBuilder Entry;
		Entry.Add(TEXT("Name"), MakeName(Module.Name));
		Entry.AddString(TEXT("Class"), GetModuleClassPath(Module).ToString());
		if (!Module.ParentModuleName.IsNone())
		{
			Entry.Add(TEXT("Parent"), MakeName(Module.ParentModuleName));
		}
		if (Module.ConfigOverrides.Num() > 0)
		{
			FObjectBuilder Config;
			for (const FControlRigOverrideValue& Override : Module.ConfigOverrides)
			{
				FValue Value;
				FString Error;
				const FProperty* Leaf = Override.GetLeafProperty();
				if (Leaf == nullptr || Override.GetData() == nullptr || !EncodeProperty(*Leaf, Override.GetData(), Value, Error))
				{
					Value = MakeString(Override.ToString());
				}
				Config.Add(Override.GetPath(), MoveTemp(Value));
			}
			Entry.Add(TEXT("Config"), Config.Build());
		}
		if (!Module.Bindings.IsEmpty())
		{
			TArray<FName> Names;
			Module.Bindings.GetKeys(Names);
			Names.Sort(FNameLexicalLess());
			FObjectBuilder Bindings;
			for (const FName& Name : Names)
			{
				Bindings.AddString(Name.ToString(), Module.Bindings.FindChecked(Name));
			}
			Entry.Add(TEXT("Bindings"), Bindings.Build());
		}
		FObjectBuilder Record;
		Record.AddString(TEXT("kind"), TEXT("module"));
		Record.Add(TEXT("entry"), Entry.Build());
		Records.Add(Record.Build());
		++OutModules;
	}
	FObjectBuilder Connections;
	for (const FModularRigSingleConnection& Connection : Model.Connections.GetConnectionList())
	{
		if (Connection.Targets.Num() == 1)
		{
			Connections.AddString(Connection.Connector.Name.ToString(), KeyString(Connection.Targets[0]));
		}
		else
		{
			TArray<FString> Targets;
			for (const FRigElementKey& Target : Connection.Targets)
			{
				Targets.Add(KeyString(Target));
			}
			Connections.Add(Connection.Connector.Name.ToString(), MakeStringArray(Targets));
		}
		++OutConnections;
	}
	FObjectBuilder Record;
	Record.AddString(TEXT("kind"), TEXT("connections"));
	Record.Add(TEXT("entries"), Connections.Build());
	Records.Add(Record.Build());
	return true;
}

FValue ModuleSettings(const UControlRigBlueprint& Blueprint)
{
	const FRigModuleSettings& Settings = const_cast<UControlRigBlueprint&>(Blueprint).GetRigModuleSettings();
	const FRigModuleSettings Defaults;
	FObjectBuilder Object;
	if (!Settings.Identifier.Name.IsEmpty() && Settings.Identifier.Name != Blueprint.GetName())
	{
		Object.AddString(TEXT("Name"), Settings.Identifier.Name);
	}
	if (Settings.Identifier.Type != Defaults.Identifier.Type)
	{
		Object.AddString(TEXT("Type"), Settings.Identifier.Type);
	}
	if (Settings.Icon != Defaults.Icon)
	{
		Object.AddString(TEXT("Icon"), Settings.Icon.ToString());
	}
	if (Settings.Category != Defaults.Category)
	{
		Object.AddString(TEXT("Category"), Settings.Category);
	}
	if (Settings.Keywords != Defaults.Keywords)
	{
		Object.AddString(TEXT("Keywords"), Settings.Keywords);
	}
	if (Settings.Description != Defaults.Description)
	{
		Object.AddString(TEXT("Description"), Settings.Description);
	}
	return Object.Build();
}

// ---- inspect facts ---------------------------------------------------------------------------------------------

FValue Facts(UControlRigBlueprint& Blueprint)
{
	FObjectBuilder Facts;
	TArray<FString> Shapes;
	const TArray<TSoftObjectPtr<UControlRigShapeLibrary>>& Libraries = Blueprint.GetShapeLibraries();
	const TMap<FString, FString> NoNameMap;
	for (const TSoftObjectPtr<UControlRigShapeLibrary>& LibraryPtr : Libraries)
	{
		if (const UControlRigShapeLibrary* Library = LoadIfPresent(LibraryPtr))
		{
			for (const FControlRigShapeDefinition& Shape : Library->Shapes)
			{
				Shapes.Add(UControlRigShapeLibrary::GetShapeName(Library, Libraries.Num() > 1, NoNameMap, Shape));
			}
		}
	}
	Facts.Add(TEXT("shapes"), MakeStringArray(Shapes));

	TArray<FString> Public;
	if (const URigVMFunctionLibrary* Library = Blueprint.GetLocalFunctionLibrary())
	{
		for (const URigVMLibraryNode* Function : Library->GetFunctions())
		{
			if (Function != nullptr && Blueprint.IsFunctionPublic(Function->GetFName()))
			{
				Public.Add(Function->GetName());
			}
		}
	}
	Facts.Add(TEXT("public_functions"), MakeStringArray(Public));

	TArray<FString> Events;
	if (const FRigVMClient* Client = static_cast<URigVMBlueprint&>(Blueprint).GetRigVMClient())
	{
		for (const URigVMGraph* Model : Client->GetModels())
		{
			if (Model != nullptr)
			{
				for (const FName& Event : Model->GetEventNames())
				{
					Events.AddUnique(Event.ToString());
				}
			}
		}
	}
	Facts.Add(TEXT("events"), MakeStringArray(Events));
	if (Blueprint.IsModularRig())
	{
		Facts.Add(TEXT("modular"), AutoConnectFacts(Blueprint));
	}

	if (const TSharedPtr<FJsonObject> Status = DescribeStatus(Blueprint))
	{
		Facts.Add(TEXT("compile"), JsonToValue(MakeShared<FJsonValueObject>(Status)));
	}
	return Facts.Build();
}

// ---- token cache -----------------------------------------------------------------------------------------------

// The records of the last offset-0 read: later pages of the same read reuse them (a 17 MB rig is not rebuilt per
// page). Game-thread only, as every primitive.
struct FReadCache
{
	TWeakObjectPtr<UObject> Asset;
	FString Token;
	bool bInspect = false;
	TArray<FValue> Records;
};

FReadCache& GetCache()
{
	static FReadCache Cache;
	return Cache;
}
} // namespace

FSourcePose::FSourcePose(UControlRigBlueprint& Blueprint)
{
	UObject* Source = LoadIfPresent(Blueprint.GetSourceHierarchyImport());
	const FReferenceSkeleton* Skeleton = nullptr;
	if (const USkeleton* AsSkeleton = Cast<USkeleton>(Source))
	{
		Skeleton = &AsSkeleton->GetReferenceSkeleton();
	}
	else if (const USkeletalMesh* AsMesh = Cast<USkeletalMesh>(Source))
	{
		Skeleton = &AsMesh->GetRefSkeleton();
	}
	if (Skeleton == nullptr)
	{
		return;
	}
	bValid = true;
	const TArray<FTransform>& Pose = Skeleton->GetRefBonePose();
	for (int32 Index = 0; Index < Skeleton->GetRawBoneNum(); ++Index)
	{
		if (Pose.IsValidIndex(Index))
		{
			Local.Add(Skeleton->GetBoneName(Index), Pose[Index]);
		}
	}
}

bool FSourcePose::Matches(FName Bone, const FTransform& Transform) const
{
	const FTransform* Found = Local.Find(Bone);
	return Found != nullptr && Found->Equals(Transform, Tolerance);
}

FString GetRigType(const UControlRigBlueprint& Blueprint)
{
	if (Blueprint.IsModularRig())
	{
		return TEXT("Modular");
	}
	return Blueprint.IsControlRigModule() ? TEXT("Module") : TEXT("Rig");
}

bool BuildRecords(UControlRigBlueprint& Blueprint, bool bInspect, FCall& Call, TArray<FValue>& Records)
{
	using namespace ControlRigReadLocal;
	const FString RigType = GetRigType(Blueprint);
	FTextValues Values;
	TArray<FValue> Body;
	int32 Variables = 0;
	int32 Elements = 0;
	int32 Modules = 0;
	int32 Connections = 0;
	if (!AddVariables(Blueprint, Values, Body, Variables))
	{
		return false;
	}
	if (RigType == TEXT("Modular"))
	{
		if (!AddModules(Blueprint, Call, Body, Modules, Connections))
		{
			return false;
		}
	}
	else
	{
		if (!AddElements(Blueprint, bInspect, Call, Body, Elements))
		{
			return false;
		}
		if (!BuildGraphRecords(Blueprint, bInspect, Values, Call, Body))
		{
			return false;
		}
	}

	int32 Graphs = 0;
	int32 Functions = 0;
	int32 Nodes = 0;
	for (const FValue& Record : Body)
	{
		const FValue* Kind = FindField(Record, TEXT("kind"));
		const FString KindName = Kind != nullptr ? Kind->StringValue : FString();
		Graphs += KindName == TEXT("graph") ? 1 : 0;
		Functions += KindName == TEXT("function") ? 1 : 0;
		Nodes += KindName == TEXT("node") ? 1 : 0;
	}

	FObjectBuilder Header;
	Header.AddString(TEXT("kind"), TEXT("header"));
	Header.AddString(TEXT("rig_type"), RigType);
	Header.AddString(TEXT("parent_class"), PathOf(Blueprint.ParentClass));
	// Class Settings import sources: duplicate-transient, so not generic Properties (the profile writes them there).
	Header.AddString(TEXT("source_hierarchy_import"), Blueprint.GetSourceHierarchyImport().ToSoftObjectPath().ToString());
	Header.AddString(TEXT("source_curve_import"), Blueprint.GetSourceCurveImport().ToSoftObjectPath().ToString());
	if (RigType == TEXT("Module"))
	{
		Header.Add(TEXT("module"), ModuleSettings(Blueprint));
	}
	if (RigType == TEXT("Modular"))
	{
		FObjectBuilder Settings;
		if (!Blueprint.GetModularRigSettings().bAutoResolve)
		{
			Settings.AddBool(TEXT("AutoResolve"), false);
		}
		Header.Add(TEXT("modular_settings"), Settings.Build());
		// Each module's primary connector ("<module>/<connector>"): the apply connects a module's primary connector
		// before its others (the module's sockets and its secondary connectors' matches follow from it).
		TArray<FString> Primaries;
		for (const FRigModuleReference& Module : Blueprint.GetModularRigModel().Modules)
		{
			const UClass* Class = FindModuleClass(Module);
			const UControlRig* Default = Class != nullptr ? Class->GetDefaultObject<UControlRig>() : nullptr;
			for (const FRigModuleConnector& Connector : Default != nullptr ? Default->GetRigModuleSettings().ExposedConnectors : TArray<FRigModuleConnector>())
			{
				if (Connector.IsPrimary())
				{
					Primaries.Add(FString::Printf(TEXT("%s/%s"), *Module.Name.ToString(), *Connector.Name));
				}
			}
		}
		Header.Add(TEXT("primary_connectors"), MakeStringArray(Primaries));
	}
	FObjectBuilder Counts;
	Counts.AddInt(TEXT("variables"), Variables);
	Counts.AddInt(TEXT("elements"), Elements);
	Counts.AddInt(TEXT("graphs"), Graphs);
	Counts.AddInt(TEXT("functions"), Functions);
	Counts.AddInt(TEXT("nodes"), Nodes);
	Counts.AddInt(TEXT("modules"), Modules);
	Counts.AddInt(TEXT("connections"), Connections);
	Header.Add(TEXT("counts"), Counts.Build());
	const URigHierarchy* Hierarchy = Blueprint.GetHierarchy();
	const int32 Components = Hierarchy != nullptr && RigType != TEXT("Modular") ? Hierarchy->NumComponents() : 0;
	if (Components > 0)
	{
		FObjectBuilder Skipped;
		Skipped.AddInt(TEXT("Components"), Components);
		Header.Add(TEXT("skipped"), Skipped.Build());
	}
	if (bInspect)
	{
		Header.Add(TEXT("facts"), Facts(Blueprint));
	}
	Records.Reset();
	Records.Add(Header.Build());
	Records.Append(MoveTemp(Body));
	return true;
}

bool HandleRead(FCall& Call, FObjectBuilder& Result)
{
	using namespace ControlRigReadLocal;
	int64 Offset = 0;
	FString Detail = TEXT("document");
	FString Token;
	if (!Call.Args.OnlyFields({TEXT("offset"), TEXT("detail"), TEXT("token")}) || !Call.Args.Int(TEXT("offset"), Offset, false)
		|| !Call.Args.String(TEXT("detail"), Detail, false) || !Call.Args.String(TEXT("token"), Token, false))
	{
		return false;
	}
	if (Offset < 0 || (Detail != TEXT("document") && Detail != TEXT("inspect")))
	{
		return Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("offset must be >= 0 and detail document or inspect."), TEXT("detail"));
	}
	UControlRigBlueprint* Blueprint = Cast<UControlRigBlueprint>(&Call.Asset);
	if (Blueprint == nullptr)
	{
		return Fail(Call, TEXT("ControlRig.UnsupportedAsset"), TEXT("rig.controlrig.read.v1 reads Control Rig assets."), TEXT("/HandleArguments/asset"));
	}
	const bool bInspect = Detail == TEXT("inspect");
	FReadCache& Cache = GetCache();
	const bool bReuse = Offset > 0 && !Token.IsEmpty() && Token == Cache.Token && Cache.Asset.Get() == Blueprint
		&& Cache.bInspect == bInspect;
	if (!bReuse)
	{
		Cache = FReadCache();
		TArray<FValue> Records;
		if (!BuildRecords(*Blueprint, bInspect, Call, Records))
		{
			return false;
		}
		Cache.Asset = Blueprint;
		Cache.Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		Cache.bInspect = bInspect;
		Cache.Records = MoveTemp(Records);
	}

	TArray<FValue> Page;
	int32 NodeCount = 16;
	int32 Index = static_cast<int32>(FMath::Clamp<int64>(Offset, 0, Cache.Records.Num()));
	for (; Index < Cache.Records.Num(); ++Index)
	{
		const int32 RowNodes = CountValueNodes(Cache.Records[Index]);
		if (!Page.IsEmpty() && NodeCount + RowNodes > QueryValueNodeBudget)
		{
			break;
		}
		NodeCount += RowNodes;
		Page.Add(Cache.Records[Index]);
	}
	Result.AddInt(TEXT("total"), Cache.Records.Num());
	Result.AddInt(TEXT("next_offset"), Index < Cache.Records.Num() ? Index : -1);
	Result.AddString(TEXT("token"), Cache.Token);
	Result.Add(TEXT("records"), MakeArray(MoveTemp(Page)));
	if (Index >= Cache.Records.Num())
	{
		// Last page: release the records.
		Cache = FReadCache();
	}
	return true;
}
} // namespace UE::AssetDocument::Graph::Rig::ControlRig
