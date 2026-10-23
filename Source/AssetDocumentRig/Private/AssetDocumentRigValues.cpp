// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Settings values (design §3.4) and type names of the rig primitives. Settings structs go through the Editor typed
// value codec, so they read and write exactly as Properties do on the generic route.

#include "AssetDocumentRigPrimitives.h"

#include "AssetDocumentEditorPropertyValueCodec.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/EngineVersionComparison.h"
#include "Retargeter/IKRetargetChainMapping.h"
#include "Retargeter/IKRetargetOps.h"
#include "Rig/IKRigDefinition.h"
#include "Rig/Solvers/IKRigSolverBase.h"
#include "UObject/UObjectIterator.h"

namespace UE::AssetDocument::Graph::Rig
{
namespace
{
struct FShortName
{
	const TCHAR* Short;
	const TCHAR* Path;
};

// The six engine solvers (design §3.3).
const FShortName SolverShortNames[] = {
	{TEXT("FullBodyIK"), TEXT("/Script/IKRig.IKRigFullBodyIKSolver")},
	{TEXT("LimbIK"), TEXT("/Script/IKRig.IKRigLimbSolver")},
	{TEXT("Pole"), TEXT("/Script/IKRig.IKRigPoleSolver")},
	{TEXT("BodyMover"), TEXT("/Script/IKRig.IKRigBodyMoverSolver")},
	{TEXT("SetTransform"), TEXT("/Script/IKRig.IKRigSetTransform")},
	{TEXT("StretchLimb"), TEXT("/Script/IKRig.IKRigStretchLimbSolver")},
};

const TCHAR* const IKRigModulePrefix = TEXT("/Script/IKRig.");

TArray<UScriptStruct*> ChildStructs(const UScriptStruct* Base)
{
	TArray<UScriptStruct*> Types;
	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		UScriptStruct* Struct = *It;
		if (Struct != Base && Struct->IsChildOf(Base) && !Struct->GetPathName().StartsWith(TEXT("/Engine/Transient")))
		{
			Types.Add(Struct);
		}
	}
	Types.Sort([](const UScriptStruct& A, const UScriptStruct& B) { return A.GetPathName() < B.GetPathName(); });
	return Types;
}
} // namespace

FStructInstance::FStructInstance(const UScriptStruct* InType)
	: Type(InType)
{
	if (Type != nullptr)
	{
		Memory.SetNumZeroed(FMath::Max(Type->GetStructureSize(), 1));
		Type->InitializeStruct(Memory.GetData());
	}
}

FStructInstance::~FStructInstance()
{
	if (Type != nullptr)
	{
		Type->DestroyStruct(Memory.GetData());
	}
}

FValue MakeName(FName Name)
{
	return MakeString(Name.IsNone() ? FString() : Name.ToString());
}

bool IsEditableField(const FProperty& Property)
{
	return Property.HasAnyPropertyFlags(CPF_Edit)
		&& !Property.HasAnyPropertyFlags(CPF_EditConst | CPF_BlueprintReadOnly | CPF_Transient | CPF_Deprecated);
}

bool EncodeField(const FProperty& Property, const void* Container, FValue& OutValue, FString& OutError)
{
	FString Code;
	FProperty& Mutable = const_cast<FProperty&>(Property);
	if (!UE::AssetDocument::EditorPropertyValueCodec::Encode(Mutable, Property.ContainerPtrToValuePtr<void>(Container), OutValue, Code))
	{
		OutError = FString::Printf(TEXT("%s cannot be represented (%s)."), *Property.GetName(), *Code);
		return false;
	}
	return true;
}

bool EncodeSparse(const UStruct& Struct, const void* Value, const void* Defaults, FValue& OutObject, FString& OutError,
	TFunctionRef<bool(const FProperty&)> Skip)
{
	FObjectBuilder Object;
	for (TFieldIterator<FProperty> It(&Struct); It; ++It)
	{
		const FProperty& Property = **It;
		if (!IsEditableField(Property) || Skip(Property))
		{
			continue;
		}
		if (Defaults != nullptr && Property.Identical_InContainer(Value, Defaults, 0, PPF_None))
		{
			continue;
		}
		FValue Field;
		if (!EncodeField(Property, Value, Field, OutError))
		{
			return false;
		}
		Object.Add(Property.GetName(), MoveTemp(Field));
	}
	OutObject = Object.Build();
	return true;
}

bool EncodeSparse(const UStruct& Struct, const void* Value, const void* Defaults, FValue& OutObject, FString& OutError)
{
	return EncodeSparse(Struct, Value, Defaults, OutObject, OutError, [](const FProperty&) { return false; });
}

TArray<TSharedPtr<FJsonValue>> DescribeFields(const UStruct& Struct, const void* Defaults, TFunctionRef<bool(const FProperty&)> Skip)
{
	TArray<TSharedPtr<FJsonValue>> Fields;
	for (TFieldIterator<FProperty> It(&Struct); It; ++It)
	{
		const FProperty& Property = **It;
		if (!IsEditableField(Property) || Skip(Property))
		{
			continue;
		}
		const TSharedRef<FJsonObject> Field = MakeShared<FJsonObject>();
		Field->SetStringField(TEXT("Name"), Property.GetName());
		Field->SetStringField(TEXT("Type"), Property.GetCPPType());
		FString Error;
		FValue Default;
		if (Defaults != nullptr && EncodeField(Property, Defaults, Default, Error))
		{
			Field->SetField(TEXT("Default"), ValueToJson(Default));
		}
		if (Property.HasMetaData(TEXT("ReinitializeOnEdit")))
		{
			Field->SetBoolField(TEXT("Reinitializes"), true);
		}
		Fields.Add(MakeShared<FJsonValueObject>(Field));
	}
	return Fields;
}

FString GetSolverTypeName(const UScriptStruct& Type)
{
	const FString Path = Type.GetPathName();
	for (const FShortName& Entry : SolverShortNames)
	{
		if (Path == Entry.Path)
		{
			return Entry.Short;
		}
	}
	return Path;
}

UScriptStruct* FindSolverType(const FString& Name)
{
	FString Path = Name;
	for (const FShortName& Entry : SolverShortNames)
	{
		if (Name == Entry.Short)
		{
			Path = Entry.Path;
			break;
		}
	}
	if (!Path.StartsWith(TEXT("/")))
	{
		return nullptr;
	}
	UScriptStruct* Type = FindObject<UScriptStruct>(nullptr, *Path);
	return Type != nullptr && Type != FIKRigSolverBase::StaticStruct() && Type->IsChildOf(FIKRigSolverBase::StaticStruct()) ? Type : nullptr;
}

TArray<UScriptStruct*> GetSolverTypes()
{
	return ChildStructs(FIKRigSolverBase::StaticStruct());
}

FString GetOpTypeName(const UScriptStruct& Type)
{
	const FString Path = Type.GetPathName();
	if (Path.StartsWith(IKRigModulePrefix))
	{
		FString Name = Type.GetName();
		if (Name.RemoveFromStart(TEXT("IKRetarget")) && Name.RemoveFromEnd(TEXT("Op")) && !Name.IsEmpty())
		{
			return Name;
		}
	}
	return Path;
}

UScriptStruct* FindOpType(const FString& Name)
{
	if (Name.IsEmpty())
	{
		return nullptr;
	}
	const FString Path = Name.StartsWith(TEXT("/")) ? Name : FString::Printf(TEXT("%sIKRetarget%sOp"), IKRigModulePrefix, *Name);
	UScriptStruct* Type = FindObject<UScriptStruct>(nullptr, *Path);
	return Type != nullptr && Type != FIKRetargetOpBase::StaticStruct() && Type->IsChildOf(FIKRetargetOpBase::StaticStruct())
		&& !IsAbstractOpType(*Type) ? Type : nullptr;
}

bool IsAbstractOpType(const UScriptStruct& Type)
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	// An abstract op type only classifies others; the op stack never adds one.
	const FInstancedStruct Instance(&Type);
	return Instance.Get<FIKRetargetOpBase>().IsAbstract();
#else
	return false;
#endif
}

TArray<UScriptStruct*> GetOpTypes()
{
	TArray<UScriptStruct*> Types = ChildStructs(FIKRetargetOpBase::StaticStruct());
	Types.RemoveAll([](const UScriptStruct* Type) { return IsAbstractOpType(*Type); });
	return Types;
}

bool IsChainRowArray(const FProperty& Property)
{
	const FArrayProperty* Array = CastField<FArrayProperty>(&Property);
	const FStructProperty* Element = Array != nullptr ? CastField<FStructProperty>(Array->Inner) : nullptr;
	return Element != nullptr && Element->Struct != nullptr && Element->Struct->FindPropertyByName(TEXT("TargetChainName")) != nullptr;
}

bool IsChainMappingField(const FProperty& Property)
{
	const FStructProperty* Struct = CastField<FStructProperty>(&Property);
	return Struct != nullptr && Struct->Struct == FRetargetChainMapping::StaticStruct();
}

bool IsTargetRigField(const FProperty& Property)
{
	const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(&Property);
	return Object != nullptr && Object->PropertyClass != nullptr && Object->PropertyClass->IsChildOf(UIKRigDefinition::StaticClass());
}

// An op's on/off switch, on engines that keep it in the op's settings: the document writes it as the op's Enabled.
static bool IsOpEnabledField(const FProperty& Property)
{
	return Property.GetOwnerStruct() == FIKRetargetOpSettingsBase::StaticStruct() && Property.GetFName() == TEXT("bEnabled");
}

bool IsOpOwnedField(const FProperty& Property)
{
	return IsChainRowArray(Property) || IsChainMappingField(Property) || IsTargetRigField(Property) || IsOpEnabledField(Property);
}
} // namespace UE::AssetDocument::Graph::Rig
