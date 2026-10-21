// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Values and names of the Control Rig document (design §3.3 "Pin values", §3.2 "Variables"): RigVM stores pin and
// variable defaults as export text; the document holds typed JSON through the Editor typed value codec, keyed by
// the value's RigVM type (a property of that type in a RigVM memory storage, the same storage the VM's literal
// memory uses), plus RigVM's short forms (element keys as "Bone(spine_01)").

#include "AssetDocumentControlRig.h"

#include "AssetDocumentEditorPropertyValueCodec.h"
#include "RigVMCore/RigVMExecuteContext.h"
#include "RigVMCore/RigVMMemoryStorageStruct.h"
#include "RigVMCore/RigVMStruct.h"
#include "RigVMCore/RigVMUnknownType.h"
#include "Rigs/RigHierarchyDefines.h"
#include "RigVMTypeUtils.h"
#include "UObject/UObjectIterator.h"
#include "Misc/StringOutputDevice.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::Graph::Rig::ControlRig
{
namespace
{
double Clean(double Value)
{
	// -0 reads as 0 so re-extracts are textually stable.
	return Value == 0.0 ? 0.0 : Value;
}

FValue Vector(double X, double Y, double Z)
{
	FObjectBuilder Object;
	Object.Add(TEXT("X"), MakeNumber(Clean(X)));
	Object.Add(TEXT("Y"), MakeNumber(Clean(Y)));
	Object.Add(TEXT("Z"), MakeNumber(Clean(Z)));
	return Object.Build();
}

int32 FindKey(const FValue& Object, const FProperty& Property)
{
	const FString Name = Property.GetName();
	const FString Authored = Property.GetAuthoredName();
	for (int32 Index = 0; Index < Object.ObjectKeys.Num(); ++Index)
	{
		if (Object.ObjectKeys[Index] == Name || Object.ObjectKeys[Index] == Authored)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

// RigVM's document forms over the codec's output (walks the value alongside the codec's JSON; parts the walk does
// not recognise keep the codec's form).
void Refine(const FProperty& Property, const void* ValuePtr, FValue& Value)
{
	if (const FFloatProperty* Float = CastField<FFloatProperty>(&Property))
	{
		if (Value.Kind == EAssetDocumentRuntimeValueKind::Number)
		{
			Value = MakeNumber(Clean(ShortestFloat(Float->GetPropertyValue(ValuePtr))));
		}
		return;
	}
	if (CastField<FDoubleProperty>(&Property) != nullptr)
	{
		if (Value.Kind == EAssetDocumentRuntimeValueKind::Number)
		{
			Value.NumberValue = Clean(Value.NumberValue);
		}
		return;
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		if (Struct->Struct == TBaseStructure<FTransform>::Get())
		{
			Value = EncodeTransform(*static_cast<const FTransform*>(ValuePtr));
			return;
		}
		if (Struct->Struct == FRigElementKey::StaticStruct())
		{
			const FRigElementKey& Key = *static_cast<const FRigElementKey*>(ValuePtr);
			Value = MakeString(ElementKeyText(Key));
			return;
		}
		if (Struct->Struct == FRigElementKeyCollection::StaticStruct())
		{
			const FRigElementKeyCollection& Collection = *static_cast<const FRigElementKeyCollection*>(ValuePtr);
			TArray<FString> Keys;
			for (int32 Index = 0; Index < Collection.Num(); ++Index)
			{
				Keys.Add(Collection[Index].IsValid() ? Collection[Index].ToString() : FString());
			}
			Value = MakeStringArray(Keys);
			return;
		}
		if (Value.Kind != EAssetDocumentRuntimeValueKind::Object)
		{
			return;
		}
		for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
		{
			const int32 Index = FindKey(Value, **It);
			if (Index != INDEX_NONE && Value.Elements.IsValidIndex(Index) && It->ArrayDim == 1)
			{
				Refine(**It, It->ContainerPtrToValuePtr<void>(ValuePtr), Value.Elements[Index]);
			}
		}
		return;
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		FScriptArrayHelper Helper(Array, ValuePtr);
		if (Value.Kind != EAssetDocumentRuntimeValueKind::Array || Value.Elements.Num() != Helper.Num())
		{
			return;
		}
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			Refine(*Array->Inner, Helper.GetRawPtr(Index), Value.Elements[Index]);
		}
	}
}

bool IsStringType(const FString& CPPType)
{
	return CPPType == TEXT("FString") || CPPType == TEXT("FName");
}

// Short struct name -> number of loaded FRigVMStructs with it.
struct FUnitNames
{
	TMap<FName, int32> Counts;
	TSet<const UScriptStruct*> Known;

	void Rebuild()
	{
		Counts.Reset();
		Known.Reset();
		for (TObjectIterator<UScriptStruct> It; It; ++It)
		{
			if (It->IsChildOf(FRigVMStruct::StaticStruct()))
			{
				Counts.FindOrAdd(It->GetFName())++;
				Known.Add(*It);
			}
		}
	}
};
} // namespace

FString TypeName(const FString& CPPType, const UObject* CPPTypeObject)
{
	if (CPPTypeObject == nullptr)
	{
		return CPPType;
	}
	static const TCHAR* const Wrappers[] = {TEXT("TArray<"), TEXT("TObjectPtr<"), TEXT("TSubclassOf<"), TEXT("TSoftObjectPtr<"),
		TEXT("TSoftClassPtr<"), TEXT("TScriptInterface<")};
	FString Prefix;
	FString Suffix;
	FString Inner = CPPType;
	bool bUnwrapped = true;
	while (bUnwrapped)
	{
		bUnwrapped = false;
		for (const TCHAR* Wrapper : Wrappers)
		{
			const int32 Length = FCString::Strlen(Wrapper);
			if (Inner.StartsWith(Wrapper, ESearchCase::CaseSensitive) && Inner.EndsWith(TEXT(">"), ESearchCase::CaseSensitive))
			{
				Prefix += Wrapper;
				Suffix = TEXT(">") + Suffix;
				Inner = Inner.Mid(Length, Inner.Len() - Length - 1).TrimStartAndEnd();
				bUnwrapped = true;
				break;
			}
		}
	}
	return Prefix + CPPTypeObject->GetPathName() + Suffix;
}

bool IsValuelessType(const FString& CPPType, const UObject* CPPTypeObject)
{
	if (CPPType.IsEmpty())
	{
		return true;
	}
	if (const UScriptStruct* Struct = Cast<UScriptStruct>(CPPTypeObject))
	{
		return Struct->IsChildOf(FRigVMExecuteContext::StaticStruct()) || Struct == FRigVMUnknownType::StaticStruct();
	}
	return false;
}

FValue EncodeTransform(const FTransform& Transform)
{
	FObjectBuilder Object;
	const FVector Translation = Transform.GetTranslation();
	if (!(Translation.X == 0.0 && Translation.Y == 0.0 && Translation.Z == 0.0))
	{
		Object.Add(TEXT("Translation"), Vector(Translation.X, Translation.Y, Translation.Z));
	}
	const FQuat Rotation = Transform.GetRotation();
	if (!(Rotation.X == 0.0 && Rotation.Y == 0.0 && Rotation.Z == 0.0 && Rotation.W == 1.0))
	{
		FObjectBuilder Quat;
		Quat.Add(TEXT("X"), MakeNumber(Clean(Rotation.X)));
		Quat.Add(TEXT("Y"), MakeNumber(Clean(Rotation.Y)));
		Quat.Add(TEXT("Z"), MakeNumber(Clean(Rotation.Z)));
		Quat.Add(TEXT("W"), MakeNumber(Clean(Rotation.W)));
		Object.Add(TEXT("Rotation"), Quat.Build());
	}
	const FVector Scale = Transform.GetScale3D();
	if (!(Scale.X == 1.0 && Scale.Y == 1.0 && Scale.Z == 1.0))
	{
		Object.Add(TEXT("Scale3D"), Vector(Scale.X, Scale.Y, Scale.Z));
	}
	return Object.Build();
}

bool EncodeProperty(const FProperty& Property, const void* ValuePtr, FValue& OutValue, FString& OutError)
{
	FString Code;
	if (!UE::AssetDocument::EditorPropertyValueCodec::Encode(const_cast<FProperty&>(Property), ValuePtr, OutValue, Code))
	{
		OutError = FString::Printf(TEXT("%s cannot be represented (%s)."), *Property.GetName(), *Code);
		return false;
	}
	Refine(Property, ValuePtr, OutValue);
	return true;
}

// ---- decode ------------------------------------------------------------------------------------------------------

namespace
{
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

// {X,Y,Z} or {X,Y,Z,W} with every listed component a number (all required).
bool Components(const FValue& Value, TConstArrayView<const TCHAR*> Names, double* Out, const TCHAR* What, FString& OutError)
{
	if (Value.Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		OutError = FString::Printf(TEXT("%s is an object {%s}."), What, *FString::Join(Names, TEXT(", ")));
		return false;
	}
	for (const FString& Key : Value.ObjectKeys)
	{
		if (!Names.ContainsByPredicate([&Key](const TCHAR* Name) { return Key == Name; }))
		{
			OutError = FString::Printf(TEXT("%s has no field '%s' (fields: %s)."), What, *Key, *FString::Join(Names, TEXT(", ")));
			return false;
		}
	}
	for (int32 Index = 0; Index < Names.Num(); ++Index)
	{
		const FValue* Field = FindField(Value, Names[Index]);
		if (Field == nullptr || !NumberOf(*Field, Out[Index]))
		{
			OutError = FString::Printf(TEXT("%s.%s is a number."), What, Names[Index]);
			return false;
		}
	}
	return true;
}

double Rounded(double Value, bool bFloat)
{
	return bFloat ? static_cast<double>(static_cast<float>(Value)) : Value;
}

bool IsTextualLeaf(const FProperty& Property)
{
	return Property.IsA<FStrProperty>() || Property.IsA<FNameProperty>() || Property.IsA<FTextProperty>();
}
} // namespace

bool ParseElementKey(const FString& Text, FRigElementKey& OutKey)
{
	OutKey = FRigElementKey();
	if (Text.IsEmpty())
	{
		return true;
	}
	int32 Open = INDEX_NONE;
	if (!Text.FindChar(TEXT('('), Open) || !Text.EndsWith(TEXT(")")) || Open == 0)
	{
		return false;
	}
	const FString TypeText = Text.Left(Open);
	// "Bone()": a key of a type without a name (a pin's default such as Target Space before an element is picked).
	const FString NameText = Text.Mid(Open + 1, Text.Len() - Open - 2);
	const int64 Type = StaticEnum<ERigElementType>()->GetValueByNameString(TypeText);
	if (Type == INDEX_NONE)
	{
		return false;
	}
	const ERigElementType ElementType = static_cast<ERigElementType>(Type);
	if (ElementType == ERigElementType::All)
	{
		return false;
	}
	OutKey = FRigElementKey(NameText.IsEmpty() ? FName(NAME_None) : FName(*NameText), ElementType);
	return true;
}

FString ElementKeyText(const FRigElementKey& Key)
{
	if (Key.IsValid())
	{
		return Key.ToString();
	}
	if ((Key.Type == ERigElementType::None || Key.Type == ERigElementType::All) && Key.Name.IsNone())
	{
		return FString();
	}
	// An incomplete key keeps both parts: "Bone()" (a typed key without a name), "None(upperarm_r)" (a name without a
	// type, as some pins hold it).
	return StaticEnum<ERigElementType>()->GetNameStringByValue(static_cast<int64>(Key.Type)) + TEXT("(")
		+ (Key.Name.IsNone() ? FString() : Key.Name.ToString()) + TEXT(")");
}

bool DecodeTransform(const FValue& Value, FTransform& OutTransform, FString& OutError, bool bFloat)
{
	if (Value.Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		OutError = TEXT("A transform is an object {Translation?, Rotation?, Scale3D?}.");
		return false;
	}
	OutTransform = FTransform::Identity;
	for (int32 Index = 0; Index < Value.ObjectKeys.Num(); ++Index)
	{
		const FString& Key = Value.ObjectKeys[Index];
		const FValue& Part = Value.Elements[Index];
		double Numbers[4] = {0.0, 0.0, 0.0, 1.0};
		if (Key == TEXT("Translation") || Key == TEXT("Scale3D"))
		{
			static const TCHAR* const Names[] = {TEXT("X"), TEXT("Y"), TEXT("Z")};
			if (!Components(Part, Names, Numbers, *Key, OutError))
			{
				return false;
			}
			const FVector Vector(Rounded(Numbers[0], bFloat), Rounded(Numbers[1], bFloat), Rounded(Numbers[2], bFloat));
			if (Key == TEXT("Translation"))
			{
				OutTransform.SetTranslation(Vector);
			}
			else
			{
				OutTransform.SetScale3D(Vector);
			}
		}
		else if (Key == TEXT("Rotation"))
		{
			static const TCHAR* const Names[] = {TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("W")};
			if (!Components(Part, Names, Numbers, TEXT("Rotation"), OutError))
			{
				return false;
			}
			OutTransform.SetRotation(FQuat(Rounded(Numbers[0], bFloat), Rounded(Numbers[1], bFloat), Rounded(Numbers[2], bFloat),
				Rounded(Numbers[3], bFloat)));
		}
		else
		{
			OutError = FString::Printf(TEXT("A transform has no field '%s' (fields: Translation, Rotation, Scale3D)."), *Key);
			return false;
		}
	}
	return true;
}

bool DecodeStructFields(const UScriptStruct& Struct, void* Instance, const FValue& Object, TFunctionRef<bool(const FProperty&)> Skip,
	FString& OutError)
{
	if (Object.Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		OutError = FString::Printf(TEXT("%s is an object."), *Struct.GetName());
		return false;
	}
	for (int32 Index = 0; Index < Object.ObjectKeys.Num(); ++Index)
	{
		const FString& Key = Object.ObjectKeys[Index];
		const FProperty* Found = nullptr;
		for (TFieldIterator<FProperty> It(&Struct); It; ++It)
		{
			if ((It->GetName() == Key || It->GetAuthoredName() == Key) && !Skip(**It))
			{
				Found = *It;
				break;
			}
		}
		if (Found == nullptr)
		{
			OutError = FString::Printf(TEXT("%s has no authored field '%s'."), *Struct.GetName(), *Key);
			return false;
		}
		if (Found->ArrayDim != 1)
		{
			OutError = FString::Printf(TEXT("%s.%s is a fixed-size array, which documents do not write."), *Struct.GetName(), *Key);
			return false;
		}
		FString Error;
		if (!DecodeProperty(*Found, Found->ContainerPtrToValuePtr<void>(Instance), Object.Elements[Index], Error))
		{
			OutError = FString::Printf(TEXT("%s: %s"), *Key, *Error);
			return false;
		}
	}
	return true;
}

bool DecodeProperty(const FProperty& Property, void* ValuePtr, const FValue& Value, FString& OutError)
{
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		if (Struct->Struct == TBaseStructure<FTransform>::Get())
		{
			return DecodeTransform(Value, *static_cast<FTransform*>(ValuePtr), OutError);
		}
		if (Struct->Struct == FRigElementKey::StaticStruct())
		{
			FRigElementKey& Key = *static_cast<FRigElementKey*>(ValuePtr);
			if (Value.Kind != EAssetDocumentRuntimeValueKind::String || !ParseElementKey(Value.StringValue, Key))
			{
				OutError = TEXT("An element key is \"Type(Name)\" (\"Bone(spine_01)\") or \"\".");
				return false;
			}
			return true;
		}
		if (Struct->Struct == FRigElementKeyCollection::StaticStruct())
		{
			if (Value.Kind != EAssetDocumentRuntimeValueKind::Array)
			{
				OutError = TEXT("An element key collection is a list of \"Type(Name)\" keys.");
				return false;
			}
			FRigElementKeyCollection Collection;
			for (const FValue& Item : Value.Elements)
			{
				FRigElementKey Key;
				if (Item.Kind != EAssetDocumentRuntimeValueKind::String || !ParseElementKey(Item.StringValue, Key))
				{
					OutError = TEXT("An element key collection is a list of \"Type(Name)\" keys.");
					return false;
				}
				Collection.Add(Key);
			}
			*static_cast<FRigElementKeyCollection*>(ValuePtr) = MoveTemp(Collection);
			return true;
		}
		// Field by field when every key names a field (the encoder's form), so nested RigVM short forms decode too.
		if (Value.Kind == EAssetDocumentRuntimeValueKind::Object)
		{
			bool bFields = true;
			for (const FString& Key : Value.ObjectKeys)
			{
				bool bFound = false;
				for (TFieldIterator<FProperty> It(Struct->Struct); It && !bFound; ++It)
				{
					bFound = (It->GetName() == Key || It->GetAuthoredName() == Key) && It->ArrayDim == 1;
				}
				bFields = bFields && bFound;
			}
			if (bFields)
			{
				return DecodeStructFields(*Struct->Struct, ValuePtr, Value, [](const FProperty&) { return false; }, OutError);
			}
		}
	}
	else if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		if (Value.Kind == EAssetDocumentRuntimeValueKind::Array)
		{
			FScriptArrayHelper Helper(Array, ValuePtr);
			Helper.EmptyAndAddValues(Value.Elements.Num());
			for (int32 Index = 0; Index < Value.Elements.Num(); ++Index)
			{
				FString Error;
				if (!DecodeProperty(*Array->Inner, Helper.GetRawPtr(Index), Value.Elements[Index], Error))
				{
					OutError = FString::Printf(TEXT("[%d]: %s"), Index, *Error);
					return false;
				}
			}
			return true;
		}
	}
	else if (const FFloatProperty* Float = CastField<FFloatProperty>(&Property))
	{
		double Number = 0.0;
		if (NumberOf(Value, Number))
		{
			Float->SetPropertyValue(ValuePtr, static_cast<float>(Number));
			return true;
		}
	}
	else if (const FDoubleProperty* Double = CastField<FDoubleProperty>(&Property))
	{
		double Number = 0.0;
		if (NumberOf(Value, Number))
		{
			Double->SetPropertyValue(ValuePtr, Number);
			return true;
		}
	}
	else if (IsTextualLeaf(Property) && Value.Kind != EAssetDocumentRuntimeValueKind::String)
	{
		OutError = FString::Printf(TEXT("%s is a string."), *Property.GetName());
		return false;
	}
	FString Code;
	if (!UE::AssetDocument::EditorPropertyValueCodec::Decode(const_cast<FProperty&>(Property), Value, ValuePtr, Code))
	{
		OutError = FString::Printf(TEXT("not a %s value (%s)"), *Property.GetCPPType(), *Code);
		return false;
	}
	return true;
}

// ---- FTextValues -------------------------------------------------------------------------------------------------

struct FTextValues::FSlot
{
	TUniquePtr<FRigVMMemoryStorageStruct> Storage;
	FProperty* A = nullptr;
	FProperty* B = nullptr;

	void* Ptr(const FProperty* Property) const
	{
		return Property->ContainerPtrToValuePtr<void>(Storage->GetContainerPtr());
	}

	bool Import(FProperty* Property, const FString& Text) const
	{
		void* Value = Ptr(Property);
		Property->ClearValue(Value);
		if (Text.IsEmpty())
		{
			return true;
		}
		FStringOutputDevice Errors;
		return Property->ImportText_Direct(*Text, Value, nullptr, PPF_None, &Errors) != nullptr;
	}
};

FTextValues::FTextValues() = default;
FTextValues::~FTextValues() = default;

FTextValues::FSlot* FTextValues::Find(const FString& CPPType, UObject* CPPTypeObject)
{
	const FString Key = CPPType + TEXT("|") + (CPPTypeObject != nullptr ? CPPTypeObject->GetPathName() : FString());
	if (TUniquePtr<FSlot>* Found = Slots.Find(Key))
	{
		return Found->Get();
	}
	TUniquePtr<FSlot> Slot = MakeUnique<FSlot>();
	if (!IsValuelessType(CPPType, CPPTypeObject))
	{
		const TArray<FRigVMPropertyDescription> Descriptions = {
			FRigVMPropertyDescription(TEXT("A"), CPPType, CPPTypeObject, FString()),
			FRigVMPropertyDescription(TEXT("B"), CPPType, CPPTypeObject, FString())};
		Slot->Storage = MakeUnique<FRigVMMemoryStorageStruct>(ERigVMMemoryType::Literal, Descriptions);
		Slot->A = Slot->Storage->FindPropertyByName(TEXT("A"));
		Slot->B = Slot->Storage->FindPropertyByName(TEXT("B"));
	}
	return Slots.Add(Key, MoveTemp(Slot)).Get();
}

bool FTextValues::Encode(const FString& CPPType, UObject* CPPTypeObject, const FString& Text, FValue& OutValue, FString& OutError)
{
	if (IsStringType(CPPType))
	{
		OutValue = MakeString(Text);
		return true;
	}
	FSlot* Slot = Find(CPPType, CPPTypeObject);
	if (Slot == nullptr || Slot->A == nullptr)
	{
		OutError = FString::Printf(TEXT("type %s holds no value"), *TypeName(CPPType, CPPTypeObject));
		return false;
	}
	if (!Slot->Import(Slot->A, Text))
	{
		OutError = FString::Printf(TEXT("'%s' is not a %s value"), *Text, *TypeName(CPPType, CPPTypeObject));
		return false;
	}
	return EncodeProperty(*Slot->A, Slot->Ptr(Slot->A), OutValue, OutError);
}

bool FTextValues::Same(const FString& CPPType, UObject* CPPTypeObject, const FString& A, const FString& B)
{
	if (A == B)
	{
		return true;
	}
	if (IsStringType(CPPType))
	{
		return false;
	}
	FSlot* Slot = Find(CPPType, CPPTypeObject);
	if (Slot == nullptr || Slot->A == nullptr || Slot->B == nullptr || !Slot->Import(Slot->A, A) || !Slot->Import(Slot->B, B))
	{
		return false;
	}
	return Slot->A->Identical(Slot->Ptr(Slot->A), Slot->Ptr(Slot->B), PPF_None);
}

bool FTextValues::Decode(const FString& CPPType, UObject* CPPTypeObject, const FString& Current, const FValue& Value, FString& OutText,
	FString& OutError)
{
	if (IsStringType(CPPType))
	{
		if (Value.Kind != EAssetDocumentRuntimeValueKind::String)
		{
			OutError = FString::Printf(TEXT("a %s value is a string"), *CPPType);
			return false;
		}
		OutText = Value.StringValue;
		return true;
	}
	FSlot* Slot = Find(CPPType, CPPTypeObject);
	if (Slot == nullptr || Slot->A == nullptr)
	{
		OutError = FString::Printf(TEXT("type %s holds no value"), *TypeName(CPPType, CPPTypeObject));
		return false;
	}
	void* Ptr = Slot->Ptr(Slot->A);
	if (!Slot->Import(Slot->A, Current))
	{
		Slot->A->ClearValue(Ptr);
	}
	FString Error;
	if (!DecodeProperty(*Slot->A, Ptr, Value, Error))
	{
		// Export text the read kept as text (the typed codec cannot represent the value).
		if (Value.Kind != EAssetDocumentRuntimeValueKind::String || !Slot->Import(Slot->A, Value.StringValue))
		{
			OutError = Error;
			return false;
		}
	}
	OutText.Reset();
	// The value itself as the delta base: every field is written. Without a base, struct fields that hold zero (a 0
	// weight, an enum's first value) are left out, and the model keeps the pin's current value for a field the text
	// does not list.
	Slot->A->ExportTextItem_Direct(OutText, Ptr, Ptr, nullptr, PPF_None);
	return true;
}

bool ParseTypeName(const FString& Name, FString& OutCPPType, UObject*& OutCPPTypeObject)
{
	OutCPPTypeObject = nullptr;
	const int32 Slash = Name.Find(TEXT("/"));
	if (Slash == INDEX_NONE)
	{
		OutCPPType = Name;
		return !Name.IsEmpty();
	}
	FString Prefix = Name.Left(Slash);
	int32 End = Name.Find(TEXT(">"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Slash);
	if (End == INDEX_NONE)
	{
		End = Name.Len();
	}
	FString Suffix = Name.Mid(End);
	OutCPPTypeObject = RigVMTypeUtils::FindObjectFromCPPTypeObjectPath(Name.Mid(Slash, End - Slash));
	if (OutCPPTypeObject == nullptr)
	{
		return false;
	}
	// The innermost wrapper of an object type is part of its CPP type (TObjectPtr<UStaticMesh>, TSubclassOf<UObject>).
	bool bClass = false;
	for (const TCHAR* Wrapper : {TEXT("TObjectPtr<"), TEXT("TSubclassOf<"), TEXT("TScriptInterface<")})
	{
		if (Prefix.EndsWith(Wrapper, ESearchCase::CaseSensitive) && Suffix.StartsWith(TEXT(">")))
		{
			bClass = FCString::Strcmp(Wrapper, TEXT("TSubclassOf<")) == 0;
			Prefix.LeftChopInline(FCString::Strlen(Wrapper));
			Suffix.RightChopInline(1);
			break;
		}
	}
	const FString Inner = RigVMTypeUtils::CPPTypeFromObject(OutCPPTypeObject,
		bClass ? RigVMTypeUtils::EClassArgType::AsClass : RigVMTypeUtils::EClassArgType::AsObject);
	if (Inner.IsEmpty())
	{
		return false;
	}
	OutCPPType = Prefix + Inner + Suffix;
	return true;
}

// ---- names -------------------------------------------------------------------------------------------------------

FString UnitName(const UScriptStruct& Struct)
{
	static FUnitNames Names;
	if (!Names.Known.Contains(&Struct))
	{
		Names.Rebuild();
	}
	const int32* Count = Names.Counts.Find(Struct.GetFName());
	return Count != nullptr && *Count == 1 ? Struct.GetName() : Struct.GetPathName();
}

TArray<UScriptStruct*> GetUnitStructs()
{
	TArray<UScriptStruct*> Structs;
	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		UScriptStruct* Struct = *It;
		if (Struct == FRigVMStruct::StaticStruct() || !Struct->IsChildOf(FRigVMStruct::StaticStruct())
			|| Struct->HasMetaData(TEXT("Abstract")) || Struct->GetPathName().StartsWith(TEXT("/Engine/Transient")))
		{
			continue;
		}
		Structs.Add(Struct);
	}
	Structs.Sort([](const UScriptStruct& A, const UScriptStruct& B) { return A.GetPathName() < B.GetPathName(); });
	return Structs;
}

const TCHAR* DirectionName(uint8 Direction)
{
	switch (static_cast<ERigVMPinDirection>(Direction))
	{
	case ERigVMPinDirection::Input: return TEXT("Input");
	case ERigVMPinDirection::Output: return TEXT("Output");
	case ERigVMPinDirection::IO: return TEXT("IO");
	case ERigVMPinDirection::Visible: return TEXT("Visible");
	case ERigVMPinDirection::Hidden: return TEXT("Hidden");
	default: return TEXT("Invalid");
	}
}
} // namespace UE::AssetDocument::Graph::Rig::ControlRig
