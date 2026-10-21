// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Details-panel writes of IK Rig settings structs and goals (design §4.1, "struct writes"): the one place that writes
// reflected memory of the rig directly. Everything else goes through UIKRigController.
//
// What the editor does (UE 5.7):
//   * Solver / goal / bone settings: the IK Rig editor shows a UIKRigStructWrapperBase (IKRigStructViewer.cpp) whose
//     Settings member is a copy of the solver's struct. A Details edit runs GEditor->BeginTransaction,
//     Owner->Modify() (the rig asset), writes the wrapper's copy, EndTransaction, and the wrapper's
//     PostEditChangeProperty copies the whole struct back into the solver's memory (CopyScriptStruct). Nothing is
//     re-initialized for the rig (no OnStructNeedsReinit binding in FIKRigEditorController): its running processor
//     copies settings from the asset each tick. So nothing is re-initialized here either.
//   * Goals are UObjects shown directly in the Details panel: an ordinary Details edit (PreEditChange / Modify,
//     write, PostEditChangeProperty).
// Here: the listed fields are decoded with the Editor typed value codec, unlisted editable fields go back to the
// defaults of a new instance (the document is the complete sparse state), and only fields whose value changes are
// written, after one Modify() of the owner. Non-editable fields (CopyAllEditableProperties rules: not
// BlueprintReadOnly / Transient / EditConst / deprecated) and fields owned by other document keys are refused with
// IKRig.FieldNotEditable. AssetDocumentIKRigStructWritesTests.cpp compares the result with the real struct wrapper.

#include "AssetDocumentIKRigStructWrites.h"

#include "AssetDocumentEditorPropertyValueCodec.h"
#include "AssetDocumentRigPrimitives.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::Graph::Rig
{
namespace
{
// One property value outside its container (initialized, destroyed with the scope).
struct FPropertyValue
{
	explicit FPropertyValue(const FProperty& InProperty)
		: Property(InProperty)
		, Memory(static_cast<uint8*>(FMemory::Malloc(FMath::Max(InProperty.GetSize(), 1), InProperty.GetMinAlignment())))
	{
		Property.InitializeValue(Memory);
	}
	~FPropertyValue()
	{
		Property.DestroyValue(Memory);
		FMemory::Free(Memory);
	}
	FPropertyValue(const FPropertyValue&) = delete;
	FPropertyValue& operator=(const FPropertyValue&) = delete;

	const FProperty& Property;
	uint8* Memory;
};

struct FPlannedWrite
{
	FProperty* Property;
	TUniquePtr<FPropertyValue> Value;
};

// The new value of every editable field of Type (listed: decoded over the default; unlisted: the default) that
// differs from the current one.
bool PlanWrites(FReader& Op, const TCHAR* Key, const FValue& Settings, const UStruct& Type, const void* Container,
	const void* Defaults, UObject* Owner, TFunctionRef<bool(const FProperty&)> Skip, TArray<FPlannedWrite>& OutWrites)
{
	if (Settings.Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("%s is an object of settings."), Key), Key);
	}
	TMap<FName, const FValue*> Listed;
	for (int32 Index = 0; Index < Settings.ObjectKeys.Num(); ++Index)
	{
		const FString& Name = Settings.ObjectKeys[Index];
		const FProperty* Property = Type.FindPropertyByName(FName(*Name));
		if (Property == nullptr || !IsEditableField(*Property) || Skip(*Property))
		{
			TArray<FString> Editable;
			for (TFieldIterator<FProperty> It(&Type); It; ++It)
			{
				if (IsEditableField(**It) && !Skip(**It))
				{
					Editable.Add(It->GetName());
				}
			}
			return Op.Fail(TEXT("IKRig.FieldNotEditable"), FString::Printf(TEXT("%s has no editable field '%s'%s. Editable: %s."),
				*Type.GetName(), *Name, Property != nullptr && Skip(*Property) ? TEXT(" (it has its own document key)") : TEXT(""),
				*FString::Join(Editable, TEXT(", "))), FString::Printf(TEXT("%s/%s"), Key, *Name));
		}
		Listed.Add(Property->GetFName(), &Settings.Elements[Index]);
	}
	for (TFieldIterator<FProperty> It(&Type); It; ++It)
	{
		FProperty& Property = **It;
		if (!IsEditableField(Property) || Skip(Property))
		{
			continue;
		}
		TUniquePtr<FPropertyValue> Value = MakeUnique<FPropertyValue>(Property);
		Property.CopyCompleteValue(Value->Memory, Property.ContainerPtrToValuePtr<void>(Defaults));
		if (const FValue* const* Authored = Listed.Find(Property.GetFName()))
		{
			FString Code;
			if (!UE::AssetDocument::EditorPropertyValueCodec::Decode(Property, **Authored, Value->Memory, Code, Owner))
			{
				return Op.Fail(TEXT("IKRig.InvalidValue"), FString::Printf(TEXT("The value of '%s' does not fit the field (%s)."),
					*Property.GetName(), *Code), FString::Printf(TEXT("%s/%s"), Key, *Property.GetName()));
			}
		}
		if (!Property.Identical(Property.ContainerPtrToValuePtr<void>(Container), Value->Memory, PPF_None))
		{
			OutWrites.Add({&Property, MoveTemp(Value)});
		}
	}
	return true;
}
} // namespace

bool WriteSettingsStruct(FReader& Op, const TCHAR* Key, const UScriptStruct& Type, void* Memory, const void* Defaults,
	UObject& Owner, TFunctionRef<bool(const FProperty&)> Skip, FStructWriteResult& OutResult)
{
	const FValue* Settings = nullptr;
	if (!Op.Object(Key, Settings))
	{
		return false;
	}
	TArray<FPlannedWrite> Writes;
	if (!PlanWrites(Op, Key, *Settings, Type, Memory, Defaults, &Owner, Skip, Writes))
	{
		return false;
	}
	if (Writes.IsEmpty())
	{
		return true;
	}
	// The struct viewer's PreChange: Owner->Modify() inside the (caller's) transaction; then the wrapper's copy.
	Owner.Modify();
	for (const FPlannedWrite& Write : Writes)
	{
		Write.Property->CopyCompleteValue(Write.Property->ContainerPtrToValuePtr<void>(Memory), Write.Value->Memory);
		OutResult.Changed.Add(Write.Property);
	}
	OutResult.bChanged = true;
	return true;
}

bool WriteObjectFields(FReader& Op, const TCHAR* Key, UObject& Object, TConstArrayView<FName> Fields,
	const TMap<FString, FName>& DocumentKeys, FStructWriteResult& OutResult)
{
	const FValue* Settings = nullptr;
	if (!Op.Object(Key, Settings))
	{
		return false;
	}
	// Document keys (ExposePosition) to property names (bExposePosition); the rest of the object's fields are not
	// authored here.
	FValue Renamed = MakeEmptyObject();
	for (int32 Index = 0; Index < Settings->ObjectKeys.Num(); ++Index)
	{
		const FName* Property = DocumentKeys.Find(Settings->ObjectKeys[Index]);
		if (Property == nullptr)
		{
			TArray<FString> Keys;
			DocumentKeys.GetKeys(Keys);
			return Op.Fail(TEXT("IKRig.FieldNotEditable"), FString::Printf(TEXT("'%s' is not a goal setting. Settings: %s."),
				*Settings->ObjectKeys[Index], *FString::Join(Keys, TEXT(", "))), FString::Printf(TEXT("%s/%s"), Key, *Settings->ObjectKeys[Index]));
		}
		Renamed.ObjectKeys.Add(Property->ToString());
		Renamed.Elements.Add(Settings->Elements[Index]);
	}
	const UObject* Defaults = Object.GetClass()->GetDefaultObject();
	TArray<FPlannedWrite> Writes;
	auto NotListed = [Fields](const FProperty& Property) { return !Fields.Contains(Property.GetFName()); };
	if (!PlanWrites(Op, Key, Renamed, *Object.GetClass(), &Object, Defaults, &Object, NotListed, Writes))
	{
		return false;
	}
	for (const FPlannedWrite& Write : Writes)
	{
		// An ordinary Details edit of the object.
		Object.PreEditChange(Write.Property);
		Write.Property->CopyCompleteValue(Write.Property->ContainerPtrToValuePtr<void>(&Object), Write.Value->Memory);
		FPropertyChangedEvent Event(Write.Property, EPropertyChangeType::ValueSet);
		Object.PostEditChangeProperty(Event);
		OutResult.bChanged = true;
	}
	return true;
}
} // namespace UE::AssetDocument::Graph::Rig
