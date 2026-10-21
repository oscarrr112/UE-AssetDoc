// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Generic-route coverage audit. This is a MEASUREMENT, not a regression test: it enumerates every
// asset class the editor can create through the generic factory lifecycle (factory_generic.v1) and
// drives each one through the production service (create template, durable create, extract, edit,
// save/unload/reload, unchanged re-apply), recording what passes, which diagnostics appear and which
// Details-panel properties never reach the document. It only asserts that it ran.
//
//   Run it explicitly: the automation test "UEAssetDoc.Coverage.GenericRouteAudit"
//
// Results land in <Project>/Saved/AssetDocumentCoverage/ (results.jsonl while running,
// coverage-audit.json when finished). Environment knobs:
//   ASSETDOC_COVERAGE_ONLY=a,b    only classes whose path contains one of the substrings
//   ASSETDOC_COVERAGE_SKIP=a,b    extra substrings to skip
//   ASSETDOC_COVERAGE_RESUME=1    keep results.jsonl of a crashed run and continue after it
//                                 (the class that was in flight is recorded as a crash)
//   ASSETDOC_COVERAGE_LIMIT=N     audit at most N classes
//   ASSETDOC_COVERAGE_IGNORE_SKIPLIST=1  also run the classes on the built-in skip list (to recheck
//                                 whether a recorded crash/hang is still reachable)

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "Factories/Factory.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/AutomationTest.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Provider/AssetDocumentEditorTypedValueCodec.h"
#include "Provider/Lifecycle/AssetDocumentEditorGenericFactory.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Class.h"
#include "UObject/StructOnScope.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "Utils/AssetDocumentJsonKeys.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentCoverageAudit
{
using FJsonObjectPtr = TSharedPtr<FJsonObject>;
using FJsonValuePtr = TSharedPtr<FJsonValue>;

// Classes known to crash or hang the editor when driven through the audit, with the reason. Filled
// from observed crashes; the audit records them as skipped rather than running them.
struct FSkipEntry
{
	const TCHAR* ClassPath;
	const TCHAR* Reason;
};
// Empty since P4 round 2: every class once listed here (GroomAsset modal factory, LightWeightInstance /
// Skeleton / SlateWidgetStyle factories missing dialog context, SoundEffectSourcePresetChain without audio,
// MetaSoundSource/Patch scratch clone) is now refused by the provider with a stable diagnostic
// (FactoryCreate.InteractiveFactory, FactoryCreate.MissingFactoryContext,
// PropertyWrite.EditorServiceUnavailable, Scratch.UnsupportedSource). Recheck an entry with
// ASSETDOC_COVERAGE_IGNORE_SKIPLIST=1 before adding it.
const TArray<FSkipEntry> SkipList;

// Exact classes that own a specialized Python profile (or, for CurveFloat, a declared generic property
// component). A class is also counted as specialized when its template has a non-empty Body or its Extract lacks
// the generic "AssetType": "GenericAsset" marker; this list covers classes that reach neither.
// Blueprint classes are refused by the generic lifecycle itself.
const TCHAR* const SpecializedClassPaths[] = {
	TEXT("/Script/AIModule.BehaviorTree"),
	TEXT("/Script/AIModule.BlackboardData"),
	TEXT("/Script/Engine.CurveFloat"),
	TEXT("/Script/Engine.AnimMontage"),
	TEXT("/Script/Engine.AnimSequence"),
	TEXT("/Script/Engine.DataTable"),
	TEXT("/Script/Engine.CompositeDataTable"),
	TEXT("/Script/Engine.CurveTable"),
	TEXT("/Script/Engine.CompositeCurveTable"),
	TEXT("/Script/Engine.MirrorDataTable"),
	TEXT("/Script/Engine.StringTable"),
	TEXT("/Script/CoreUObject.UserDefinedStruct"),
	TEXT("/Script/Engine.UserDefinedEnum"),
};

const TCHAR* const AuditRoot = TEXT("/Game/AssetDocumentCoverage");

// ---------------------------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------------------------

FString ToJsonString(const FJsonObjectPtr& Object, bool bPretty)
{
	FString Text;
	if (!Object.IsValid())
	{
		return Text;
	}
	if (bPretty)
	{
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}
	else
	{
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}
	return Text;
}

FJsonValuePtr CloneValue(const FJsonValuePtr& Source);

FString ValueToString(const FJsonValuePtr& Value, int32 MaxLength = 240)
{
	if (!Value.IsValid())
	{
		return TEXT("<missing>");
	}
	// Wrap in an object so scalars and containers serialize uniformly, then strip the wrapper.
	FJsonObjectPtr Wrapper = MakeShared<FJsonObject>();
	Wrapper->SetField(TEXT("v"), CloneValue(Value));
	FString Text = ToJsonString(Wrapper, false);
	Text.RemoveFromStart(TEXT("{\"v\":"));
	Text.RemoveFromEnd(TEXT("}"));
	return Text.Len() > MaxLength ? Text.Left(MaxLength) + TEXT("...") : Text;
}

FJsonObjectPtr CloneObject(const FJsonObjectPtr& Source)
{
	if (!Source.IsValid())
	{
		return MakeShared<FJsonObject>();
	}
	FJsonObjectPtr Clone;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ToJsonString(Source, false)), Clone);
	return Clone.IsValid() ? Clone : MakeShared<FJsonObject>();
}

FJsonValuePtr CloneValue(const FJsonValuePtr& Source)
{
	if (!Source.IsValid())
	{
		return nullptr;
	}
	switch (Source->Type)
	{
	case EJson::Object:
	{
		return MakeShared<FJsonValueObject>(CloneObject(Source->AsObject()));
	}
	case EJson::Array:
	{
		TArray<FJsonValuePtr> Items;
		for (const FJsonValuePtr& Item : Source->AsArray())
		{
			Items.Add(CloneValue(Item));
		}
		return MakeShared<FJsonValueArray>(MoveTemp(Items));
	}
	case EJson::String:
		return MakeShared<FJsonValueString>(Source->AsString());
	case EJson::Number:
		return MakeShared<FJsonValueNumber>(Source->AsNumber());
	case EJson::Boolean:
		return MakeShared<FJsonValueBoolean>(Source->AsBool());
	default:
		return MakeShared<FJsonValueNull>();
	}
}

bool JsonEquals(const FJsonValuePtr& A, const FJsonValuePtr& B)
{
	if (!A.IsValid() || !B.IsValid())
	{
		return A.IsValid() == B.IsValid();
	}
	if (A->Type != B->Type)
	{
		// ints may travel as numeric strings
		if ((A->Type == EJson::String && B->Type == EJson::Number)
			|| (A->Type == EJson::Number && B->Type == EJson::String))
		{
			const FString Text = A->Type == EJson::String ? A->AsString() : B->AsString();
			const double Number = A->Type == EJson::Number ? A->AsNumber() : B->AsNumber();
			return Text.IsNumeric() && FMath::Abs(FCString::Atod(*Text) - Number) < 1e-4;
		}
		return false;
	}
	switch (A->Type)
	{
	case EJson::Number:
	{
		const double Left = A->AsNumber();
		const double Right = B->AsNumber();
		return FMath::Abs(Left - Right) <= 1e-4 * FMath::Max(1.0, FMath::Max(FMath::Abs(Left), FMath::Abs(Right)));
	}
	case EJson::String:
		return A->AsString() == B->AsString();
	case EJson::Boolean:
		return A->AsBool() == B->AsBool();
	case EJson::Null:
		return true;
	case EJson::Array:
	{
		const TArray<FJsonValuePtr>& Left = A->AsArray();
		const TArray<FJsonValuePtr>& Right = B->AsArray();
		if (Left.Num() != Right.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < Left.Num(); ++Index)
		{
			if (!JsonEquals(Left[Index], Right[Index]))
			{
				return false;
			}
		}
		return true;
	}
	case EJson::Object:
	{
		const FJsonObjectPtr Left = A->AsObject();
		const FJsonObjectPtr Right = B->AsObject();
		if (Left->Values.Num() != Right->Values.Num())
		{
			return false;
		}
		for (const TPair<FString, FJsonValuePtr>& Pair : Left->Values)
		{
			if (!JsonEquals(Pair.Value, Right->TryGetField(Pair.Key)))
			{
				return false;
			}
		}
		return true;
	}
	default:
		return true;
	}
}

FJsonValuePtr MakeStringArray(const TArray<FString>& Items)
{
	TArray<FJsonValuePtr> Values;
	for (const FString& Item : Items)
	{
		Values.Add(MakeShared<FJsonValueString>(Item));
	}
	return MakeShared<FJsonValueArray>(MoveTemp(Values));
}

FString Trim(const FString& Text, int32 MaxLength = 1000)
{
	FString Clean = Text.Replace(TEXT("\n"), TEXT(" ")).Replace(TEXT("\r"), TEXT(" "));
	return Clean.Len() > MaxLength ? Clean.Left(MaxLength) + TEXT("...") : Clean;
}

// ---------------------------------------------------------------------------------------------
// Property kind classification (what the Details panel shows vs what the typed codec admits)
// ---------------------------------------------------------------------------------------------

bool IsInstancedProperty(const FProperty* Property)
{
	return Property->HasAnyPropertyFlags(CPF_InstancedReference | CPF_ContainsInstancedReference);
}

FString DescribeKind(const FProperty* Property);

FString DescribeLeafKind(const FProperty* Property)
{
	if (CastField<FBoolProperty>(Property))
	{
		return TEXT("bool");
	}
	if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
	{
		return FString::Printf(TEXT("enum:%s"), EnumProperty->GetEnum() ? *EnumProperty->GetEnum()->GetName() : TEXT("?"));
	}
	if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
	{
		if (Numeric->GetIntPropertyEnum() != nullptr)
		{
			return FString::Printf(TEXT("enum:%s"), *Numeric->GetIntPropertyEnum()->GetName());
		}
		return Numeric->IsFloatingPoint() ? TEXT("float") : TEXT("int");
	}
	if (CastField<FStrProperty>(Property))
	{
		return TEXT("string");
	}
	if (CastField<FNameProperty>(Property))
	{
		return TEXT("name");
	}
	if (CastField<FTextProperty>(Property))
	{
		return TEXT("text");
	}
	if (CastField<FSoftClassProperty>(Property))
	{
		return TEXT("soft_class_ref");
	}
	if (CastField<FSoftObjectProperty>(Property))
	{
		return TEXT("soft_object_ref");
	}
	if (CastField<FClassProperty>(Property))
	{
		return TEXT("class_ref");
	}
	if (CastField<FObjectPropertyBase>(Property))
	{
		return IsInstancedProperty(Property) ? TEXT("instanced_subobject")
			: CastField<FWeakObjectProperty>(Property) ? TEXT("weak_object_ref")
			: TEXT("object_ref");
	}
	if (CastField<FInterfaceProperty>(Property))
	{
		return TEXT("interface_ref");
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
	{
		return FString::Printf(TEXT("struct:%s"), *Struct->Struct->GetName());
	}
	if (CastField<FDelegateProperty>(Property) || CastField<FMulticastDelegateProperty>(Property))
	{
		return TEXT("delegate");
	}
	if (CastField<FFieldPathProperty>(Property))
	{
		return TEXT("field_path");
	}
	return Property->GetClass()->GetName();
}

FString DescribeKind(const FProperty* Property)
{
	if (const FArrayProperty* Array = CastField<FArrayProperty>(Property))
	{
		return FString::Printf(TEXT("array<%s>"), *DescribeKind(Array->Inner));
	}
	if (const FSetProperty* Set = CastField<FSetProperty>(Property))
	{
		return FString::Printf(TEXT("set<%s>"), *DescribeKind(Set->ElementProp));
	}
	if (const FMapProperty* Map = CastField<FMapProperty>(Property))
	{
		return FString::Printf(TEXT("map<%s,%s>"), *DescribeKind(Map->KeyProp), *DescribeKind(Map->ValueProp));
	}
	const FString Leaf = DescribeLeafKind(Property);
	return Property->ArrayDim > 1 ? FString::Printf(TEXT("fixed[%d]<%s>"), Property->ArrayDim, *Leaf) : Leaf;
}

// Coarse bucket used for the edit picks and the summary tables.
FString CoarseKind(const FProperty* Property)
{
	if (CastField<FArrayProperty>(Property))
	{
		return TEXT("array");
	}
	if (CastField<FSetProperty>(Property))
	{
		return TEXT("set");
	}
	if (CastField<FMapProperty>(Property))
	{
		return TEXT("map");
	}
	if (Property->ArrayDim > 1)
	{
		return TEXT("fixed_array");
	}
	const FString Leaf = DescribeLeafKind(Property);
	if (Leaf.StartsWith(TEXT("enum:")))
	{
		return TEXT("enum");
	}
	if (Leaf.StartsWith(TEXT("struct:")))
	{
		return TEXT("struct");
	}
	return Leaf;
}

// Static description of the leaf kind that keeps a property out of the codec
// (used only to label a property the production codec rejects).
FString DescribeCodecBlocker(const FProperty* Property, bool bIgnoreStatic, int32 Depth)
{
	if (Depth > 12)
	{
		return TEXT("nesting too deep");
	}
	if (!bIgnoreStatic && Property->ArrayDim > 1)
	{
		return DescribeCodecBlocker(Property, true, Depth + 1);
	}
	if (CastField<FBoolProperty>(Property) || CastField<FNumericProperty>(Property)
		|| CastField<FEnumProperty>(Property) || CastField<FStrProperty>(Property)
		|| CastField<FNameProperty>(Property) || CastField<FSoftClassProperty>(Property)
		|| CastField<FSoftObjectProperty>(Property))
	{
		return FString();
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
	{
		for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
			{
				continue;
			}
			const FString Inner = DescribeCodecBlocker(*It, false, Depth + 1);
			if (!Inner.IsEmpty())
			{
				return FString::Printf(TEXT("%s.%s: %s"), *Struct->Struct->GetName(), *It->GetName(), *Inner);
			}
		}
		return FString();
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(Property))
	{
		return DescribeCodecBlocker(Array->Inner, false, Depth + 1);
	}
	if (const FSetProperty* Set = CastField<FSetProperty>(Property))
	{
		return DescribeCodecBlocker(Set->ElementProp, false, Depth + 1);
	}
	if (const FMapProperty* Map = CastField<FMapProperty>(Property))
	{
		const FNumericProperty* IntegerKey = CastField<FNumericProperty>(Map->KeyProp);
		if (!CastField<FStrProperty>(Map->KeyProp) && !CastField<FNameProperty>(Map->KeyProp)
			&& !(IntegerKey != nullptr && IntegerKey->IsInteger()))
		{
			return FString::Printf(TEXT("map key %s (only string/name/integer keys)"), *DescribeKind(Map->KeyProp));
		}
		return DescribeCodecBlocker(Map->ValueProp, false, Depth + 1);
	}
	return DescribeKind(Property);
}

// Empty when the production typed value codec admits the property.
FString FindCodecBlocker(const FProperty* Property, bool bIgnoreStatic, int32 Depth)
{
	const FAssetDocumentEditorTypedValueCodecResult Admission =
		InspectAssetDocumentEditorTypedValueProperty(*const_cast<FProperty*>(Property));
	if (Admission.bSucceeded)
	{
		return FString();
	}
	const FString Blocker = DescribeCodecBlocker(Property, bIgnoreStatic, Depth);
	return Blocker.IsEmpty() ? FString::Printf(TEXT("%s (%s)"), *DescribeKind(Property), *Admission.DiagnosticCode) : Blocker;
}

// ---------------------------------------------------------------------------------------------
// Document-form value synthesis for the edit step
// ---------------------------------------------------------------------------------------------

FJsonValuePtr DefaultJson(const FProperty* Property, int32 Depth);

FJsonValuePtr DefaultLeafJson(const FProperty* Property, int32 Depth)
{
	if (CastField<FBoolProperty>(Property))
	{
		return MakeShared<FJsonValueBoolean>(false);
	}
	const UEnum* Enum = nullptr;
	if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
	{
		Enum = EnumProperty->GetEnum();
	}
	else if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
	{
		Enum = Numeric->GetIntPropertyEnum();
		if (Enum == nullptr)
		{
			return MakeShared<FJsonValueNumber>(0.0);
		}
	}
	if (Enum != nullptr)
	{
		return MakeShared<FJsonValueString>(Enum->NumEnums() > 0 ? Enum->GetAuthoredNameStringByIndex(0) : FString());
	}
	if (CastField<FNameProperty>(Property))
	{
		return MakeShared<FJsonValueString>(TEXT("None"));
	}
	if (CastField<FSoftObjectProperty>(Property) || CastField<FSoftClassProperty>(Property))
	{
		// An empty soft reference reads (and is written) as "None", like the Details panel shows it.
		return MakeShared<FJsonValueString>(TEXT("None"));
	}
	if (CastField<FTextProperty>(Property))
	{
		return MakeShared<FJsonValueString>(FString());
	}
	if ((CastField<FObjectPropertyBase>(Property) && !CastField<FSoftObjectProperty>(Property))
		|| CastField<FInterfaceProperty>(Property))
	{
		return MakeShared<FJsonValueNull>();
	}
	if (const FStructProperty* TextForm = CastField<FStructProperty>(Property))
	{
		if (TextForm->Struct == FInstancedStruct::StaticStruct())
		{
			return MakeShared<FJsonValueNull>();
		}
		UScriptStruct::ICppStructOps* Ops = TextForm->Struct->GetCppStructOps();
		if (Ops != nullptr && Ops->HasExportTextItem() && Ops->HasImportTextItem())
		{
			// Structs with a native text form travel as that text (codec struct_text.v1).
			FStructOnScope Default(TextForm->Struct);
			FString Text;
			Ops->ExportTextItem(Text, Default.GetStructMemory(), nullptr, nullptr, PPF_None, nullptr);
			return MakeShared<FJsonValueString>(Text);
		}
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
	{
		FJsonObjectPtr Object = MakeShared<FJsonObject>();
		if (Depth < 8)
		{
			for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
			{
				if (!It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
				{
					Object->SetField(It->GetAuthoredName(), DefaultJson(*It, Depth + 1));
				}
			}
		}
		return MakeShared<FJsonValueObject>(Object);
	}
	return MakeShared<FJsonValueString>(FString());
}

FJsonValuePtr DefaultJson(const FProperty* Property, int32 Depth)
{
	if (CastField<FArrayProperty>(Property) || CastField<FSetProperty>(Property))
	{
		return MakeShared<FJsonValueArray>(TArray<FJsonValuePtr>());
	}
	if (CastField<FMapProperty>(Property))
	{
		return MakeShared<FJsonValueObject>(MakeShared<FJsonObject>());
	}
	if (Property->ArrayDim > 1)
	{
		TArray<FJsonValuePtr> Items;
		for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
		{
			Items.Add(DefaultLeafJson(Property, Depth));
		}
		return MakeShared<FJsonValueArray>(MoveTemp(Items));
	}
	return DefaultLeafJson(Property, Depth);
}

bool TryGetMetaNumber(const FProperty* Property, const TCHAR* Key, double& OutValue)
{
	if (!Property->HasMetaData(Key))
	{
		return false;
	}
	const FString& Text = Property->GetMetaData(Key);
	if (!Text.IsNumeric())
	{
		return false;
	}
	OutValue = FCString::Atod(*Text);
	return true;
}

// The classes a reference property's picker offers: its AllowedClasses metadata (on the property, or on the
// container that owns an element property), else its declared class.
TArray<const UClass*> GetPickerClasses(const FObjectPropertyBase& Reference)
{
	TArray<const UClass*> Classes;
	const FProperty* Owner = Reference.GetOwnerProperty();
	const FString Allowed = Reference.HasMetaData(TEXT("AllowedClasses")) ? Reference.GetMetaData(TEXT("AllowedClasses"))
		: Owner != nullptr && Owner->HasMetaData(TEXT("AllowedClasses")) ? Owner->GetMetaData(TEXT("AllowedClasses")) : FString();
	TArray<FString> Paths;
	Allowed.ParseIntoArray(Paths, TEXT(","), true);
	for (FString& Path : Paths)
	{
		Path.TrimStartAndEndInline();
		if (const UClass* Class = FindObject<UClass>(nullptr, *Path))
		{
			Classes.Add(Class);
		}
	}
	if (Classes.IsEmpty() && Reference.PropertyClass != nullptr)
	{
		Classes.Add(Reference.PropertyClass);
	}
	return Classes;
}

// A native class a class picker would list under Base: concrete, not hidden, first by path.
const UClass* FirstPickableClass(const UClass& Base, const FString& Current)
{
	const UClass* Found = nullptr;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (It->IsChildOf(&Base) && It->IsNative() && !It->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated
			| CLASS_NewerVersionExists | CLASS_Hidden | CLASS_HideDropDown) && It->GetPathName() != Current
			&& (Found == nullptr || It->GetPathName() < Found->GetPathName()))
		{
			Found = *It;
		}
	}
	return Found;
}

FString GetClassRefMetaData(const FProperty& Property, const TCHAR* Key)
{
	if (Property.HasMetaData(Key))
	{
		return Property.GetMetaData(Key);
	}
	const FProperty* Owner = Property.GetOwnerProperty();
	return Owner != nullptr && Owner->HasMetaData(Key) ? Owner->GetMetaData(Key) : FString();
}

const UClass* FindClassByMetaName(FString Name)
{
	Name.TrimStartAndEndInline();
	if (Name.IsEmpty())
	{
		return nullptr;
	}
	if (const UClass* Class = FindObject<UClass>(nullptr, *Name))
	{
		return Class;
	}
	return FindFirstObject<UClass>(*Name, EFindFirstObjectOptions::NativeFirst);
}

// The class a class picker for this TSubclassOf / UClass* field offers: the metaclass itself when it is
// concrete and nothing narrows it; otherwise the first native class by path that is a child of the metaclass
// (or of one of its AllowedClasses), implements its MustImplement interface and is not abstract or hidden.
const UClass* PickClassForClassRef(const FClassProperty& Property, const FString& Existing)
{
	const UClass* Meta = Property.MetaClass != nullptr ? Property.MetaClass.Get() : UObject::StaticClass();
	const UClass* Interface = FindClassByMetaName(GetClassRefMetaData(Property, TEXT("MustImplement")));
	TArray<const UClass*> Bases;
	TArray<FString> Allowed;
	GetClassRefMetaData(Property, TEXT("AllowedClasses")).ParseIntoArray(Allowed, TEXT(","), true);
	for (const FString& Name : Allowed)
	{
		if (const UClass* Class = FindClassByMetaName(Name); Class != nullptr && Class->IsChildOf(Meta))
		{
			Bases.Add(Class);
		}
	}
	const bool bAllowAbstract = !GetClassRefMetaData(Property, TEXT("AllowAbstract")).IsEmpty()
		&& GetClassRefMetaData(Property, TEXT("AllowAbstract")) != TEXT("false");
	if (Bases.IsEmpty() && Interface == nullptr && (bAllowAbstract || !Meta->HasAnyClassFlags(CLASS_Abstract)))
	{
		return Meta->GetPathName() == Existing ? nullptr : Meta;
	}
	if (Bases.IsEmpty())
	{
		Bases.Add(Meta);
	}
	const UClass* Found = nullptr;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		const UClass* Class = *It;
		if (!Class->IsNative() || Class->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists | CLASS_Hidden
			| CLASS_HideDropDown | CLASS_Interface) || (!bAllowAbstract && Class->HasAnyClassFlags(CLASS_Abstract))
			|| (Interface != nullptr && !Class->ImplementsInterface(Interface)) || Class->GetPathName() == Existing)
		{
			continue;
		}
		if (Bases.ContainsByPredicate([Class](const UClass* Base) { return Class->IsChildOf(Base); })
			&& (Found == nullptr || Class->GetPathName() < Found->GetPathName()))
		{
			Found = Class;
		}
	}
	return Found;
}

FString PickOtherAssetPath(const FProperty* Property, const FString& Current)
{
	const FObjectPropertyBase* Soft = CastField<FObjectPropertyBase>(Property);
	if (Soft == nullptr || Soft->PropertyClass == nullptr || CastField<FClassProperty>(Property) != nullptr
		|| CastField<FSoftClassProperty>(Property) != nullptr)
	{
		return FString();
	}
	const TArray<const UClass*> PickerClasses = GetPickerClasses(*Soft);
	// A reference typed UClass (TArray<TObjectPtr<UClass>> with AllowedClasses: a class picker) takes a class.
	if (Soft->PropertyClass->IsChildOf(UClass::StaticClass()))
	{
		const UClass* Base = PickerClasses.Num() == 1 && PickerClasses[0] != UClass::StaticClass() ? PickerClasses[0] : UObject::StaticClass();
		const UClass* Picked = FirstPickableClass(*Base, Current);
		return Picked != nullptr ? Picked->GetPathName() : FString();
	}
	TArray<FAssetData> Assets;
	for (const UClass* PickerClass : PickerClasses)
	{
		IAssetRegistry::GetChecked().GetAssetsByClass(PickerClass->GetClassPathName(), Assets, true);
	}
	Assets.Sort([](const FAssetData& L, const FAssetData& R) { return L.GetObjectPathString() < R.GetObjectPathString(); });
	for (const FAssetData& Asset : Assets)
	{
		const FString Path = Asset.GetObjectPathString();
		if (Path != Current && !Path.StartsWith(AuditRoot))
		{
			return Path;
		}
	}
	return FString();
}

// What the audit passes as FactoryContext when a create reports FactoryCreate.MissingFactoryContext: for each
// dialog field the factory declares (unset object fields first, then unset class settings), an existing registry
// asset of the field's class (outside the audit root) or a class its picker lists. Empty when nothing fits.
FString PickFactoryContext(const FString& FactoryPath)
{
	const UClass* FactoryClass = FindObject<UClass>(nullptr, *FactoryPath);
	if (FactoryClass == nullptr)
	{
		return FString();
	}
	const UObject* Defaults = FactoryClass->GetDefaultObject();
	for (const FObjectPropertyBase* Field : GetAssetDocumentEditorGenericFactoryContextFields(*FactoryClass))
	{
		if (Field->GetObjectPropertyValue_InContainer(Defaults) == nullptr)
		{
			const FString Path = PickOtherAssetPath(Field, FString());
			if (!Path.IsEmpty())
			{
				return Path;
			}
		}
	}
	for (TFieldIterator<FClassProperty> It(FactoryClass); It; ++It)
	{
		const UClass* Owner = It->GetOwnerClass();
		if (Owner != nullptr && Owner != UFactory::StaticClass() && Owner->IsChildOf(UFactory::StaticClass())
			&& It->ArrayDim == 1 && It->MetaClass != nullptr && It->GetObjectPropertyValue_InContainer(Defaults) == nullptr)
		{
			if (const UClass* Picked = FirstPickableClass(*It->MetaClass, FString()))
			{
				return Picked->GetPathName();
			}
		}
	}
	return FString();
}

// Actual restricted to the shape of Requested (objects keep only the requested keys, equal-length
// arrays project element-wise), the way a document compares a partial value.
FJsonValuePtr ProjectOntoAuthored(const FJsonValuePtr& Actual, const FJsonValuePtr& Requested)
{
	if (!Actual.IsValid() || !Requested.IsValid())
	{
		return Actual;
	}
	if (Actual->Type == EJson::Object && Requested->Type == EJson::Object)
	{
		FJsonObjectPtr Out = MakeShared<FJsonObject>();
		for (const TPair<FString, FJsonValuePtr>& Entry : Requested->AsObject()->Values)
		{
			if (const FJsonValuePtr Field = Actual->AsObject()->TryGetField(Entry.Key))
			{
				Out->SetField(Entry.Key, ProjectOntoAuthored(Field, Entry.Value));
			}
		}
		return MakeShared<FJsonValueObject>(Out);
	}
	if (Actual->Type == EJson::Array && Requested->Type == EJson::Array
		&& Actual->AsArray().Num() == Requested->AsArray().Num())
	{
		TArray<FJsonValuePtr> Items;
		for (int32 Index = 0; Index < Actual->AsArray().Num(); ++Index)
		{
			Items.Add(ProjectOntoAuthored(Actual->AsArray()[Index], Requested->AsArray()[Index]));
		}
		return MakeShared<FJsonValueArray>(MoveTemp(Items));
	}
	return Actual;
}

FJsonValuePtr MutateJson(const FProperty* Property, const FJsonValuePtr& Current, bool bAsElement, int32 Depth);

// Mutates one leaf of a non-container value; null when it cannot produce a different value.
FJsonValuePtr MutateLeaf(const FProperty* Property, const FJsonValuePtr& Current, int32 Depth)
{
	if (CastField<FBoolProperty>(Property))
	{
		if (!Current.IsValid() || Current->Type != EJson::Boolean)
		{
			return nullptr;
		}
		return MakeShared<FJsonValueBoolean>(!Current->AsBool());
	}
	const UEnum* Enum = nullptr;
	if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
	{
		Enum = EnumProperty->GetEnum();
	}
	else if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
	{
		Enum = Numeric->GetIntPropertyEnum();
	}
	if (Enum != nullptr)
	{
		if (!Current.IsValid() || Current->Type != EJson::String)
		{
			return nullptr;
		}
		const FString Existing = Current->AsString();
		int32 Separator = INDEX_NONE;
		const bool bPrefixed = Existing.FindLastChar(TEXT(':'), Separator);
		const FString Prefix = bPrefixed ? Existing.Left(Separator + 1) : FString();
		const FString ExistingBare = bPrefixed ? Existing.Mid(Separator + 1) : Existing;
		for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
		{
			const FString Name = Enum->GetAuthoredNameStringByIndex(Index);
			if (Enum->HasMetaData(TEXT("Hidden"), Index) || Name.EndsWith(TEXT("_MAX")) || Name == ExistingBare)
			{
				continue;
			}
			return MakeShared<FJsonValueString>(Prefix + Name);
		}
		return nullptr;
	}
	if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
	{
		const bool bNumberString = Current.IsValid() && Current->Type == EJson::String;
		if (!Current.IsValid() || (Current->Type != EJson::Number && !bNumberString))
		{
			return nullptr;
		}
		const double Value = bNumberString ? FCString::Atod(*Current->AsString()) : Current->AsNumber();
		const double Step = Numeric->IsFloatingPoint() ? 0.5 : 1.0;
		double Min = -TNumericLimits<double>::Max();
		double Max = TNumericLimits<double>::Max();
		TryGetMetaNumber(Property, TEXT("ClampMin"), Min);
		TryGetMetaNumber(Property, TEXT("ClampMax"), Max);
		const bool bUnsigned = CastField<FByteProperty>(Property) || CastField<FUInt16Property>(Property)
			|| CastField<FUInt32Property>(Property) || CastField<FUInt64Property>(Property);
		if (bUnsigned)
		{
			Min = FMath::Max(Min, 0.0);
		}
		if (CastField<FByteProperty>(Property))
		{
			Max = FMath::Min(Max, 255.0);
		}
		double Candidates[] = {Value + Step, Value - Step, Value + 2.0 * Step, Value - 2.0 * Step};
		for (double& Candidate : Candidates)
		{
			if (Numeric->IsFloatingPoint())
			{
				// Authors write short decimals (0.6), not the float32 widening of one.
				Candidate = FMath::RoundToDouble(Candidate * 1000.0) / 1000.0;
			}
		}
		for (const double Candidate : Candidates)
		{
			if (Candidate >= Min && Candidate <= Max && Candidate != Value)
			{
				if (bNumberString)
				{
					return MakeShared<FJsonValueString>(Numeric->IsFloatingPoint() ? FString::SanitizeFloat(Candidate) : FString::Printf(TEXT("%lld"), static_cast<int64>(Candidate)));
				}
				return MakeShared<FJsonValueNumber>(Candidate);
			}
		}
		return nullptr;
	}
	if (CastField<FStrProperty>(Property) || CastField<FNameProperty>(Property))
	{
		if (!Current.IsValid() || Current->Type != EJson::String)
		{
			return nullptr;
		}
		const FString Existing = Current->AsString();
		return MakeShared<FJsonValueString>(Existing == TEXT("None") || Existing.IsEmpty() ? FString(TEXT("AuditValue")) : Existing + TEXT("_Audit"));
	}
	if (CastField<FSoftClassProperty>(Property))
	{
		const FSoftClassProperty* SoftClass = CastField<FSoftClassProperty>(Property);
		const FString Existing = Current.IsValid() && Current->Type == EJson::String ? Current->AsString() : FString();
		const UClass* Meta = SoftClass->MetaClass != nullptr ? SoftClass->MetaClass.Get() : UObject::StaticClass();
		const FString Path = Meta->GetClassPathName().ToString();
		if (Path == Existing)
		{
			return nullptr;
		}
		return MakeShared<FJsonValueString>(Path);
	}
	if (CastField<FTextProperty>(Property))
	{
		const FString Existing = Current.IsValid() && Current->Type == EJson::String ? Current->AsString() : FString();
		return MakeShared<FJsonValueString>(Existing.IsEmpty() ? FString(TEXT("Audit text")) : Existing + TEXT(" (audit)"));
	}
	if (const FClassProperty* ClassRef = CastField<FClassProperty>(Property))
	{
		const FString Existing = Current.IsValid() && Current->Type == EJson::String ? Current->AsString() : FString();
		const UClass* Picked = PickClassForClassRef(*ClassRef, Existing);
		return Picked != nullptr ? FJsonValuePtr(MakeShared<FJsonValueString>(Picked->GetPathName())) : FJsonValuePtr();
	}
	if (const FObjectPropertyBase* Instanced = CastField<FObjectPropertyBase>(Property);
		Instanced != nullptr && IsInstancedProperty(Property) && Instanced->PropertyClass != nullptr)
	{
		if (Current.IsValid() && Current->Type != EJson::Null)
		{
			return MakeShared<FJsonValueNull>();
		}
		// First concrete subclass (sorted by path), as a Details class picker would offer it.
		TArray<UClass*> Candidates;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->IsChildOf(Instanced->PropertyClass) && !It->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated
				| CLASS_NewerVersionExists | CLASS_Hidden | CLASS_HideDropDown) && It->HasAnyClassFlags(CLASS_EditInlineNew)
				&& !It->GetName().StartsWith(TEXT("SKEL_")) && !It->GetName().StartsWith(TEXT("REINST_"))
				&& (It->ClassWithin == nullptr || It->ClassWithin == UObject::StaticClass()))
			{
				Candidates.Add(*It);
			}
		}
		if (Candidates.IsEmpty())
		{
			return nullptr;
		}
		Candidates.Sort([](const UClass& L, const UClass& R) { return L.GetPathName() < R.GetPathName(); });
		FJsonObjectPtr Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("Class"), Candidates[0]->GetPathName());
		Object->SetObjectField(TEXT("Properties"), MakeShared<FJsonObject>());
		return MakeShared<FJsonValueObject>(Object);
	}
	if (CastField<FObjectPropertyBase>(Property) && !CastField<FSoftClassProperty>(Property))
	{
		const FString Existing = Current.IsValid() && Current->Type == EJson::String ? Current->AsString() : FString();
		const FString Path = PickOtherAssetPath(Property, Existing);
		if (Path.IsEmpty())
		{
			return nullptr;
		}
		return MakeShared<FJsonValueString>(Path);
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
	{
		if (Depth > 6 || !Current.IsValid() || Current->Type != EJson::Object)
		{
			return nullptr;
		}
		FJsonObjectPtr Copy = CloneObject(Current->AsObject());
		for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditConst))
			{
				continue;
			}
			FJsonValuePtr FieldCurrent = Copy->TryGetField(It->GetAuthoredName());
			if (!FieldCurrent.IsValid())
			{
				FieldCurrent = DefaultJson(*It, Depth + 1);
			}
			const FJsonValuePtr Mutated = MutateJson(*It, FieldCurrent, false, Depth + 1);
			if (Mutated.IsValid())
			{
				Copy->SetField(It->GetAuthoredName(), Mutated);
				return MakeShared<FJsonValueObject>(Copy);
			}
		}
		return nullptr;
	}
	return nullptr;
}

FJsonValuePtr MutateJson(const FProperty* Property, const FJsonValuePtr& Current, bool bAsElement, int32 Depth)
{
	if (Depth > 8 || !Current.IsValid())
	{
		return nullptr;
	}
	if (!bAsElement && Property->ArrayDim > 1)
	{
		if (Current->Type != EJson::Array || Current->AsArray().IsEmpty())
		{
			return nullptr;
		}
		TArray<FJsonValuePtr> Items;
		for (const FJsonValuePtr& Item : Current->AsArray())
		{
			Items.Add(CloneValue(Item));
		}
		const FJsonValuePtr Mutated = MutateJson(Property, Items[0], true, Depth + 1);
		if (!Mutated.IsValid())
		{
			return nullptr;
		}
		Items[0] = Mutated;
		return MakeShared<FJsonValueArray>(MoveTemp(Items));
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(Property))
	{
		if (Current->Type != EJson::Array)
		{
			return nullptr;
		}
		TArray<FJsonValuePtr> Items;
		for (const FJsonValuePtr& Item : Current->AsArray())
		{
			Items.Add(CloneValue(Item));
		}
		if (!Items.IsEmpty())
		{
			// An instanced element is edited, not cleared: the Details panel offers no null entry in such arrays.
			const FJsonValuePtr Mutated = MutateJson(Array->Inner, Items[0], false, Depth + 1);
			if (Mutated.IsValid() && !(IsInstancedProperty(Array->Inner) && Mutated->Type == EJson::Null))
			{
				Items[0] = Mutated;
				return MakeShared<FJsonValueArray>(MoveTemp(Items));
			}
		}
		FJsonValuePtr Added = DefaultJson(Array->Inner, Depth + 1);
		if (const FJsonValuePtr Mutated = MutateJson(Array->Inner, Added, false, Depth + 1))
		{
			Added = Mutated;
		}
		if (IsInstancedProperty(Array->Inner) && (!Added.IsValid() || Added->Type == EJson::Null))
		{
			// Instanced arrays may not gain null entries; no creatable class means no edit.
			return nullptr;
		}
		Items.Add(Added);
		return MakeShared<FJsonValueArray>(MoveTemp(Items));
	}
	if (const FSetProperty* Set = CastField<FSetProperty>(Property))
	{
		if (Current->Type != EJson::Array)
		{
			return nullptr;
		}
		TArray<FJsonValuePtr> Items;
		for (const FJsonValuePtr& Item : Current->AsArray())
		{
			Items.Add(CloneValue(Item));
		}
		FJsonValuePtr Added = Items.IsEmpty() ? DefaultJson(Set->ElementProp, Depth + 1) : Items.Last();
		Added = MutateJson(Set->ElementProp, Added, false, Depth + 1);
		if (!Added.IsValid())
		{
			return nullptr;
		}
		Items.Add(Added);
		return MakeShared<FJsonValueArray>(MoveTemp(Items));
	}
	if (const FMapProperty* Map = CastField<FMapProperty>(Property))
	{
		if (Current->Type != EJson::Object)
		{
			return nullptr;
		}
		FJsonObjectPtr Copy = CloneObject(Current->AsObject());
		for (const TPair<FString, FJsonValuePtr>& Pair : Copy->Values)
		{
			if (const FJsonValuePtr Mutated = MutateJson(Map->ValueProp, Pair.Value, false, Depth + 1))
			{
				Copy->SetField(Pair.Key, Mutated);
				return MakeShared<FJsonValueObject>(Copy);
			}
		}
		FJsonValuePtr Added = DefaultJson(Map->ValueProp, Depth + 1);
		if (const FJsonValuePtr Mutated = MutateJson(Map->ValueProp, Added, false, Depth + 1))
		{
			Added = Mutated;
		}
		// Non-string keys (integer, enum, object path, text-form struct) use a valid key in their document form;
		// an integer key is its decimal text.
		FString Key = TEXT("AuditKey");
		const FNumericProperty* IntegerKey = CastField<FNumericProperty>(Map->KeyProp);
		if (IntegerKey != nullptr && IntegerKey->IsInteger() && IntegerKey->GetIntPropertyEnum() == nullptr)
		{
			int32 Candidate = 0;
			while (Copy->HasField(FString::FromInt(Candidate)) && Candidate < 64)
			{
				++Candidate;
			}
			Key = FString::FromInt(Candidate);
		}
		else if (!CastField<FStrProperty>(Map->KeyProp) && !CastField<FNameProperty>(Map->KeyProp))
		{
			FJsonValuePtr KeyJson = DefaultJson(Map->KeyProp, Depth + 1);
			if (KeyJson.IsValid() && KeyJson->Type == EJson::String && Copy->HasField(KeyJson->AsString()))
			{
				KeyJson = MutateLeaf(Map->KeyProp, KeyJson, Depth + 1);
			}
			if (!KeyJson.IsValid() || KeyJson->Type != EJson::String || Copy->HasField(KeyJson->AsString()))
			{
				return nullptr;
			}
			Key = KeyJson->AsString();
		}
		Copy->SetField(Key, Added);
		return MakeShared<FJsonValueObject>(Copy);
	}
	return MutateLeaf(Property, Current, Depth);
}

// The JSON writer prints an integral double as "1", which the document reader then types as an Integer;
// nested float fields reject that. This rewrites integral float leaves as "1.0" literals so an author that
// keeps the decimal point can be told apart from the writer's formatting.
FJsonValuePtr WithFloatLiterals(const FProperty* Property, const FJsonValuePtr& Value, bool bAsElement, int32 Depth)
{
	if (!Value.IsValid() || Depth > 10)
	{
		return CloneValue(Value);
	}
	if (!bAsElement && Property->ArrayDim > 1 && Value->Type == EJson::Array)
	{
		TArray<FJsonValuePtr> Items;
		for (const FJsonValuePtr& Item : Value->AsArray())
		{
			Items.Add(WithFloatLiterals(Property, Item, true, Depth + 1));
		}
		return MakeShared<FJsonValueArray>(MoveTemp(Items));
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(Property))
	{
		TArray<FJsonValuePtr> Items;
		if (Value->Type == EJson::Array)
		{
			for (const FJsonValuePtr& Item : Value->AsArray())
			{
				Items.Add(WithFloatLiterals(Array->Inner, Item, false, Depth + 1));
			}
		}
		return MakeShared<FJsonValueArray>(MoveTemp(Items));
	}
	if (const FSetProperty* Set = CastField<FSetProperty>(Property))
	{
		TArray<FJsonValuePtr> Items;
		if (Value->Type == EJson::Array)
		{
			for (const FJsonValuePtr& Item : Value->AsArray())
			{
				Items.Add(WithFloatLiterals(Set->ElementProp, Item, false, Depth + 1));
			}
		}
		return MakeShared<FJsonValueArray>(MoveTemp(Items));
	}
	if (const FMapProperty* Map = CastField<FMapProperty>(Property))
	{
		FJsonObjectPtr Copy = MakeShared<FJsonObject>();
		if (Value->Type == EJson::Object)
		{
			for (const TPair<FString, FJsonValuePtr>& Pair : Value->AsObject()->Values)
			{
				Copy->SetField(Pair.Key, WithFloatLiterals(Map->ValueProp, Pair.Value, false, Depth + 1));
			}
		}
		return MakeShared<FJsonValueObject>(Copy);
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
	{
		if (Value->Type != EJson::Object)
		{
			return CloneValue(Value);
		}
		FJsonObjectPtr Copy = MakeShared<FJsonObject>();
		for (const TPair<FString, FJsonValuePtr>& Pair : Value->AsObject()->Values)
		{
			const FProperty* Field = nullptr;
			for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
			{
				if (It->GetAuthoredName() == Pair.Key)
				{
					Field = *It;
					break;
				}
			}
			Copy->SetField(Pair.Key, Field != nullptr ? WithFloatLiterals(Field, Pair.Value, false, Depth + 1) : CloneValue(Pair.Value));
		}
		return MakeShared<FJsonValueObject>(Copy);
	}
	if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
	{
		if (Numeric->IsFloatingPoint() && Value->Type == EJson::Number)
		{
			const double Number = Value->AsNumber();
			if (Number == FMath::FloorToDouble(Number) && FMath::Abs(Number) < 1e15)
			{
				return MakeShared<FJsonValueNumberString>(FString::Printf(TEXT("%.1f"), Number));
			}
		}
	}
	return CloneValue(Value);
}

// ---------------------------------------------------------------------------------------------
// Class enumeration (same rules as SelectAssetDocumentEditorGenericFactory)
// ---------------------------------------------------------------------------------------------

struct FAuditClass
{
	UClass* Class = nullptr;
	FString FactoryPath;
	bool bExactFactory = false;
	bool bFactoryInNewMenu = false;
	bool bSpecialized = false;
};

FString ModuleOf(const UClass* Class)
{
	const FString Path = Class->GetClassPathName().GetPackageName().ToString();
	return Path.StartsWith(TEXT("/Script/")) ? Path.RightChop(8) : Path;
}

struct FEnumeration
{
	TArray<FAuditClass> Classes;
	TArray<FJsonValuePtr> Factories;
	TArray<FJsonValuePtr> Refused;
	TArray<FJsonValuePtr> Creatable;
};

FEnumeration EnumerateCreatableClasses()
{
	FEnumeration Result;
	struct FAdmittedFactory
	{
		const UClass* Supported;
		bool bShown;
	};
	TArray<FAdmittedFactory> Admitted;

	TArray<UClass*> FactoryClasses;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (It->IsChildOf(UFactory::StaticClass()))
		{
			FactoryClasses.Add(*It);
		}
	}
	FactoryClasses.Sort([](const UClass& L, const UClass& R) { return L.GetPathName() < R.GetPathName(); });
	for (UClass* FactoryClass : FactoryClasses)
	{
		if (FactoryClass->HasAnyClassFlags(CLASS_Abstract))
		{
			continue;
		}
		const UFactory* Factory = FactoryClass->GetDefaultObject<UFactory>();
		const UClass* Supported = Factory != nullptr ? Factory->GetSupportedClass() : nullptr;
		FString Verdict = TEXT("admitted");
		if (FactoryClass->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			Verdict = TEXT("refused: deprecated factory");
		}
		else if (Supported == nullptr)
		{
			Verdict = TEXT("refused: no SupportedClass");
		}
		else if (!Factory->CanCreateNew())
		{
			Verdict = TEXT("refused: bCreateNew=false (import/open-only factory)");
		}
		else if (!EnumHasAnyFlags(Factory->GetSupportedWorkflows(), EFactoryCreateWorkflow::Default))
		{
			Verdict = TEXT("refused: no Default workflow");
		}
		FJsonObjectPtr Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("factory"), FactoryClass->GetPathName());
		Row->SetStringField(TEXT("supported_class"), Supported != nullptr ? Supported->GetPathName() : FString());
		Row->SetBoolField(TEXT("create_new"), Factory != nullptr && Factory->CanCreateNew());
		Row->SetBoolField(TEXT("show_in_new_menu"), Factory != nullptr && Factory->ShouldShowInNewMenu());
		Row->SetStringField(TEXT("verdict"), Verdict);
		Result.Factories.Add(MakeShared<FJsonValueObject>(Row));
		if (Verdict == TEXT("admitted") && Supported != UObject::StaticClass())
		{
			Admitted.Add({Supported, Factory->ShouldShowInNewMenu()});
		}
	}

	TArray<UClass*> AllClasses;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		const FString Name = Class->GetName();
		if (!Class->HasAnyClassFlags(CLASS_Native) || Name.StartsWith(TEXT("SKEL_")) || Name.StartsWith(TEXT("REINST_"))
			|| Name.StartsWith(TEXT("HOTRELOADED_")) || Name.StartsWith(TEXT("TRASHCLASS_")))
		{
			continue;
		}
		AllClasses.Add(Class);
	}
	AllClasses.Sort([](const UClass& L, const UClass& R) { return L.GetPathName() < R.GetPathName(); });

	for (UClass* Class : AllClasses)
	{
		bool bRelated = false;
		bool bExactSupported = false;
		for (const FAdmittedFactory& Factory : Admitted)
		{
			if (Class->IsChildOf(Factory.Supported))
			{
				bRelated = true;
				bExactSupported |= Class == Factory.Supported;
			}
		}
		if (!bRelated)
		{
			continue;
		}
		const FAssetDocumentEditorGenericFactorySelection Selection = SelectAssetDocumentEditorGenericFactory(Class);
		if (!Selection.IsSelected())
		{
			// Only exact factory-supported classes (and Blueprint classes) are worth listing as refusals.
			if (bExactSupported || Class->IsChildOf(UBlueprint::StaticClass()))
			{
				FJsonObjectPtr Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("class"), Class->GetPathName());
				Row->SetStringField(TEXT("code"), Selection.GetDiagnosticCode());
				Row->SetStringField(TEXT("reason"), Selection.GetDiagnosticMessage(Class));
				Result.Refused.Add(MakeShared<FJsonValueObject>(Row));
			}
			continue;
		}
		if (!bExactSupported && Class->HasAnyClassFlags(CLASS_HideDropDown))
		{
			FJsonObjectPtr Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("class"), Class->GetPathName());
			Row->SetStringField(TEXT("code"), TEXT("NotAudited.HideDropDown"));
			Row->SetStringField(TEXT("reason"), TEXT("Subclass reachable only through an ancestor factory and hidden from the class picker (HideDropDown)."));
			Result.Refused.Add(MakeShared<FJsonValueObject>(Row));
			continue;
		}
		FAuditClass Entry;
		Entry.Class = Class;
		Entry.FactoryPath = Selection.FactoryClass->GetPathName();
		Entry.bExactFactory = Selection.bExactSupportedClass;
		Entry.bFactoryInNewMenu = Selection.FactoryClass->GetDefaultObject<UFactory>()->ShouldShowInNewMenu();
		for (const TCHAR* Specialized : SpecializedClassPaths)
		{
			Entry.bSpecialized |= Class->GetPathName() == Specialized;
		}
		FJsonObjectPtr Listed = MakeShared<FJsonObject>();
		Listed->SetStringField(TEXT("class"), Class->GetPathName());
		Listed->SetStringField(TEXT("module"), ModuleOf(Class));
		Listed->SetStringField(TEXT("factory"), Entry.FactoryPath);
		Listed->SetBoolField(TEXT("factory_exact"), Entry.bExactFactory);
		Result.Creatable.Add(MakeShared<FJsonValueObject>(Listed));
		Result.Classes.Add(MoveTemp(Entry));
	}
	return Result;
}

// ---------------------------------------------------------------------------------------------
// Service wrappers
// ---------------------------------------------------------------------------------------------

struct FStep
{
	FString Status = TEXT("skipped");
	FString Message;
	TArray<FString> Codes;
	bool bSavedAsset = false;
};

FJsonObjectPtr StepToJson(const FStep& Step)
{
	FJsonObjectPtr Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("status"), Step.Status);
	if (!Step.Message.IsEmpty())
	{
		Object->SetStringField(TEXT("message"), Trim(Step.Message));
	}
	if (!Step.Codes.IsEmpty())
	{
		Object->SetField(TEXT("codes"), MakeStringArray(Step.Codes));
	}
	return Object;
}

void FillFromResult(FStep& Step, const FAssetDocumentResult& Result)
{
	Step.bSavedAsset = Result.bSavedAsset;
	Step.Message = Result.Message;
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Step.Codes.AddUnique(Diagnostic.Code);
		if (Step.Message.Len() < 1400)
		{
			Step.Message += FString::Printf(TEXT(" | [%s] %s"), *Diagnostic.Code, *Trim(Diagnostic.Message, 400));
		}
	}
}

FString GetEnv(const TCHAR* Name)
{
	return FPlatformMisc::GetEnvironmentVariable(Name);
}

TArray<FString> SplitList(const FString& Text)
{
	TArray<FString> Items;
	Text.ParseIntoArray(Items, TEXT(","), true);
	for (FString& Item : Items)
	{
		Item.TrimStartAndEndInline();
	}
	return Items;
}

class FAuditRun
{
public:
	explicit FAuditRun(FAutomationTestBase& InTest)
		: Test(InTest)
		, Service(FAssetDocumentModule::Get().GetService())
		, OutputDir(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AssetDocumentCoverage")))
	{
	}

	void Run();

private:
	FAutomationTestBase& Test;
	FAssetDocumentService& Service;
	FString OutputDir;

	struct FTarget
	{
		FString Directory;      // /Game/AssetDocumentCoverage/C007
		FString PackageName;    // /Game/AssetDocumentCoverage/C007/Asset
		FString ObjectPath;
		FString SidecarPath;
		FString ContentDir;
	};

	FTarget MakeTarget(int32 Index, const UClass& Class) const;
	void WriteSidecar(const FTarget& Target, const FJsonObjectPtr& Document) const;
	FAssetDocumentResult Apply(const FTarget& Target, const FJsonObjectPtr& Document, bool bSave, bool bFixFloats = false) const;
	FJsonObjectPtr FixFloatLiterals(const FJsonObjectPtr& Document) const;
	void DumpExtract(const FTarget& Target, const UClass& Class, const FJsonObjectPtr& Payload) const;
	void BisectProperties(const FTarget& Target, const FJsonObjectPtr& Full, TArray<FJsonValuePtr>& OutFailures) const;
	mutable const UClass* CurrentClass = nullptr;
	FAssetDocumentResult ExtractAll(const FTarget& Target) const;
	FAssetDocumentResult ExtractDiff(const FTarget& Target) const;
	void CleanupTarget(const FTarget& Target) const;
	FJsonObjectPtr AuditClass(const FAuditClass& Entry, int32 Index) const;
	void AnalyzeProperties(const FAuditClass& Entry, UObject* Asset, const FJsonObjectPtr& ExtractAllPayload,
		const FJsonObjectPtr& InspectPayload, FJsonObjectPtr& OutProperties) const;
	void EditStep(const FAuditClass& Entry, const FTarget& Target, const FJsonObjectPtr& ExtractAllPayload,
		FStep& OutStep, TArray<FJsonValuePtr>& OutEdits) const;
	void AppendLine(const FString& FileName, const FString& Line) const;
	void ScanDerivedReferences(const UObject* Asset, const TCHAR* Stage, TMap<FString, FJsonObjectPtr>& InOutFound) const;
};

FAuditRun::FTarget FAuditRun::MakeTarget(int32 Index, const UClass& Class) const
{
	FTarget Target;
	Target.Directory = FString::Printf(TEXT("%s/C%03d"), AuditRoot, Index);
	FString Short = Class.GetName();
	Short.ReplaceInline(TEXT("-"), TEXT("_"));
	Target.PackageName = FString::Printf(TEXT("%s/A_%s"), *Target.Directory, *Short.Left(48));
	Target.ObjectPath = FString::Printf(TEXT("%s.A_%s"), *Target.PackageName, *Short.Left(48));
	Target.ContentDir = FPackageName::LongPackageNameToFilename(Target.Directory + TEXT("/"));
	// The sidecar must sit beside the asset's package path (<Target>.assetdoc.json under Content).
	Target.SidecarPath = FPackageName::LongPackageNameToFilename(Target.PackageName, TEXT(".assetdoc.json"));
	return Target;
}

void FAuditRun::WriteSidecar(const FTarget& Target, const FJsonObjectPtr& Document) const
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target.SidecarPath), true);
	FFileHelper::SaveStringToFile(ToJsonString(Document, true), *Target.SidecarPath);
}

FJsonObjectPtr FAuditRun::FixFloatLiterals(const FJsonObjectPtr& Document) const
{
	FJsonObjectPtr Copy = CloneObject(Document);
	if (CurrentClass == nullptr || !Copy->HasTypedField<EJson::Object>(TEXT("Properties")))
	{
		return Copy;
	}
	FJsonObjectPtr Fixed = MakeShared<FJsonObject>();
	for (const TPair<FString, FJsonValuePtr>& Pair : Copy->GetObjectField(TEXT("Properties"))->Values)
	{
		const FProperty* Property = CurrentClass->FindPropertyByName(*Pair.Key);
		Fixed->SetField(Pair.Key, Property != nullptr ? WithFloatLiterals(Property, Pair.Value, false, 0) : Pair.Value);
	}
	Copy->SetObjectField(TEXT("Properties"), Fixed);
	return Copy;
}

void FAuditRun::DumpExtract(const FTarget& Target, const UClass& Class, const FJsonObjectPtr& Payload) const
{
	(void)Target;
	FFileHelper::SaveStringToFile(ToJsonString(Payload, true),
		*FPaths::Combine(OutputDir, TEXT("extracts"), Class.GetName() + TEXT(".json")),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

// Applies every emitted property on its own to name the ones whose unchanged value is rejected.
void FAuditRun::BisectProperties(const FTarget& Target, const FJsonObjectPtr& Full, TArray<FJsonValuePtr>& OutFailures) const
{
	if (!Full.IsValid() || !Full->HasTypedField<EJson::Object>(TEXT("Properties")))
	{
		return;
	}
	const FJsonObjectPtr Properties = Full->GetObjectField(TEXT("Properties"));
	TArray<FString> Names;
	AssetDocumentServerJson::GetKeys(*Properties, Names);
	Names.Sort();
	int32 Tested = 0;
	for (const FString& Name : Names)
	{
		if (++Tested > 80)
		{
			break;
		}
		FJsonObjectPtr Document = CloneObject(Full);
		FJsonObjectPtr Only = MakeShared<FJsonObject>();
		Only->SetField(Name, CloneValue(Properties->TryGetField(Name)));
		Document->SetObjectField(TEXT("Properties"), Only);
		const FAssetDocumentResult Result = Apply(Target, Document, true);
		if (Result.IsSuccess() && !Result.bSavedAsset)
		{
			continue;
		}
		FStep Step;
		FillFromResult(Step, Result);
		FJsonObjectPtr Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"), Name);
		const FProperty* Property = CurrentClass != nullptr ? CurrentClass->FindPropertyByName(*Name) : nullptr;
		Row->SetStringField(TEXT("kind"), Property != nullptr ? DescribeKind(Property) : FString());
		Row->SetStringField(TEXT("value"), ValueToString(Properties->TryGetField(Name), 160));
		Row->SetStringField(TEXT("result"), Result.IsSuccess() ? TEXT("saved-although-unchanged") : TEXT("rejected"));
		if (!Step.Codes.IsEmpty())
		{
			Row->SetField(TEXT("codes"), MakeStringArray(Step.Codes));
		}
		if (Result.IsSuccess() == false && Property != nullptr)
		{
			const FAssetDocumentResult Fixed = Apply(Target, FixFloatLiterals(Document), true);
			Row->SetBoolField(TEXT("accepted_with_float_literals"), Fixed.IsSuccess() && !Fixed.bSavedAsset);
		}
		OutFailures.Add(MakeShared<FJsonValueObject>(Row));
	}
}

FAssetDocumentResult FAuditRun::Apply(const FTarget& Target, const FJsonObjectPtr& InDocument, bool bSave, bool bFixFloats) const
{
	const FJsonObjectPtr Document = bFixFloats ? FixFloatLiterals(InDocument) : InDocument;
	WriteSidecar(Target, Document);
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Target.SidecarPath;
	Request.bSaveAsset = bSave;
	return Service.ApplyFile(Request);
}

FAssetDocumentResult FAuditRun::ExtractAll(const FTarget& Target) const
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = Target.ObjectPath;
	Request.bDiffOnly = false;
	Request.bIncludeAllWritable = true;
	return Service.Extract(Request);
}

FAssetDocumentResult FAuditRun::ExtractDiff(const FTarget& Target) const
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = Target.ObjectPath;
	return Service.Extract(Request);
}

void FAuditRun::AppendLine(const FString& FileName, const FString& Line) const
{
	const FString Path = FPaths::Combine(OutputDir, FileName);
	FFileHelper::SaveStringToFile(Line + TEXT("\n"), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
		&IFileManager::Get(), FILEWRITE_Append);
}

void FAuditRun::CleanupTarget(const FTarget& Target) const
{
	if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting && !GEditor->IsTransactionActive()
		&& GEditor->Trans->GetQueueLength() > 0)
	{
		GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentCoverageAudit", "Cleanup", "AssetDocument coverage audit cleanup"));
	}
	TArray<UPackage*> Packages;
	for (TObjectIterator<UPackage> It; It; ++It)
	{
		if (It->GetName().StartsWith(Target.Directory))
		{
			Packages.Add(*It);
		}
	}
	if (!Packages.IsEmpty())
	{
		for (UPackage* Package : Packages)
		{
			Package->ClearDirtyFlag();
		}
		UPackageTools::UnloadPackages(Packages);
	}
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	// Some assets outlive an unload (FStringTableEngineBridge keeps string tables alive until they
	// are garbage); delete those as the editor's delete does.
	if (UObject* Leftover = FSoftObjectPath(Target.ObjectPath).ResolveObject())
	{
		ObjectTools::ForceDeleteObjects({Leftover}, false);
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *Target.ContentDir, TEXT("*"), true, false, false);
	// The registry only understands package files; a sidecar path would trip FilenameToLongPackageName.
	Files.RemoveAll([](const FString& File)
	{
		return !(File.EndsWith(TEXT(".uasset")) || File.EndsWith(TEXT(".umap")));
	});
	IFileManager::Get().DeleteDirectory(*Target.ContentDir, false, true);
	IFileManager::Get().Delete(*Target.SidecarPath, false, true);
	if (!Files.IsEmpty())
	{
		FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles(Files);
	}
}

// ---------------------------------------------------------------------------------------------
// Property analysis: Details-visible properties vs what Extract/Inspect expose
// ---------------------------------------------------------------------------------------------

void FAuditRun::AnalyzeProperties(const FAuditClass& Entry, UObject* Asset, const FJsonObjectPtr& ExtractAllPayload,
	const FJsonObjectPtr& InspectPayload, FJsonObjectPtr& OutProperties) const
{
	OutProperties = MakeShared<FJsonObject>();
	const UClass* Class = Entry.Class;
	FJsonObjectPtr Emitted = ExtractAllPayload.IsValid() && ExtractAllPayload->HasTypedField<EJson::Object>(TEXT("Properties"))
		? ExtractAllPayload->GetObjectField(TEXT("Properties")) : MakeShared<FJsonObject>();

	TMap<FString, bool> InspectAdmittedWritable;
	TMap<FString, FString> InspectSkipped;
	const TArray<FJsonValuePtr>* AdmittedRows = nullptr;
	const TArray<FJsonValuePtr>* SkippedRows = nullptr;
	if (InspectPayload.IsValid())
	{
		if (InspectPayload->TryGetArrayField(TEXT("properties"), AdmittedRows))
		{
			for (const FJsonValuePtr& Row : *AdmittedRows)
			{
				const FJsonObjectPtr Object = Row->AsObject();
				if (Object.IsValid())
				{
					InspectAdmittedWritable.Add(Object->GetStringField(TEXT("name")),
						Object->HasField(TEXT("writable")) && Object->GetBoolField(TEXT("writable")));
				}
			}
		}
		if (InspectPayload->TryGetArrayField(TEXT("skipped"), SkippedRows))
		{
			for (const FJsonValuePtr& Row : *SkippedRows)
			{
				const FJsonObjectPtr Object = Row->AsObject();
				if (Object.IsValid())
				{
					InspectSkipped.Add(Object->GetStringField(TEXT("name")), Object->GetStringField(TEXT("reason")));
				}
			}
		}
	}

	int32 DetailsVisible = 0;
	int32 EmittedCount = 0;
	int32 EditConstCount = 0;
	int32 EditorDerivedCount = 0;
	TMap<FString, int32> DroppedByKind;
	TMap<FString, int32> DroppedByReason;
	TArray<FJsonValuePtr> Dropped;
	TArray<FJsonValuePtr> HiddenStructure;
	for (TFieldIterator<FProperty> It(Class); It; ++It)
	{
		const FProperty* Property = *It;
		if (!Property->HasAnyPropertyFlags(CPF_Edit))
		{
			// Serialized non-edit state that is a container/reference: candidate hidden structure.
			if (!Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_SkipSerialization | CPF_DuplicateTransient))
			{
				const FString Kind = DescribeKind(Property);
				const bool bStructural = Kind.Contains(TEXT("object_ref")) || Kind.Contains(TEXT("instanced"))
					|| Kind.Contains(TEXT("array<")) || Kind.Contains(TEXT("map<")) || Kind.Contains(TEXT("set<"))
					|| Kind.Contains(TEXT("struct:"));
				if (bStructural && HiddenStructure.Num() < 40)
				{
					FJsonObjectPtr Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("name"), Property->GetName());
					Row->SetStringField(TEXT("kind"), Kind);
					Row->SetStringField(TEXT("declared_in"), Property->GetOwnerClass() ? Property->GetOwnerClass()->GetName() : FString());
					HiddenStructure.Add(MakeShared<FJsonValueObject>(Row));
				}
			}
			continue;
		}
		++DetailsVisible;
		const FString AuthoredName = Property->GetAuthoredName();
		if (Emitted->HasField(AuthoredName))
		{
			++EmittedCount;
			continue;
		}
		FString Reason;
		if (const FString* Skipped = InspectSkipped.Find(Property->GetName()))
		{
			Reason = FString::Printf(TEXT("inspect-skipped:%s"), **Skipped);
		}
		else if (const bool* Writable = InspectAdmittedWritable.Find(Property->GetName()))
		{
			const FString Blocker = FindCodecBlocker(Property, false, 0);
			if (!*Writable && Blocker.IsEmpty())
			{
				Reason = TEXT("read-only (EditConst)");
				++EditConstCount;
			}
			else if (!Blocker.IsEmpty())
			{
				Reason = FString::Printf(TEXT("no write codec: %s"), *Blocker);
			}
			else
			{
				Reason = TEXT("admitted by Inspect but absent from Extract");
			}
		}
		else
		{
			const FString Blocker = FindCodecBlocker(Property, false, 0);
			Reason = Blocker.IsEmpty() ? TEXT("absent from Inspect and Extract") : FString::Printf(TEXT("no write codec: %s"), *Blocker);
		}
		if (Reason == TEXT("read-only (EditConst)"))
		{
			// Visible but intentionally not authorable; counted separately, not a gap.
			continue;
		}
		if (Reason == TEXT("inspect-skipped:editor-derived-reference"))
		{
			// Holds a subobject the asset's own code creates (not authorable); not a gap.
			++EditorDerivedCount;
			continue;
		}
		const FString Coarse = CoarseKind(Property);
		DroppedByKind.FindOrAdd(Coarse)++;
		DroppedByReason.FindOrAdd(Reason.Left(60))++;
		FJsonObjectPtr Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"), Property->GetName());
		Row->SetStringField(TEXT("kind"), DescribeKind(Property));
		Row->SetStringField(TEXT("coarse"), Coarse);
		Row->SetStringField(TEXT("reason"), Reason);
		Row->SetStringField(TEXT("declared_in"), Property->GetOwnerClass() ? Property->GetOwnerClass()->GetName() : FString());
		Dropped.Add(MakeShared<FJsonValueObject>(Row));
	}
	OutProperties->SetNumberField(TEXT("details_visible"), DetailsVisible);
	OutProperties->SetNumberField(TEXT("emitted"), EmittedCount);
	OutProperties->SetNumberField(TEXT("edit_const_not_authorable"), EditConstCount);
	OutProperties->SetNumberField(TEXT("editor_derived_not_authorable"), EditorDerivedCount);
	OutProperties->SetNumberField(TEXT("dropped"), Dropped.Num());
	FJsonObjectPtr ByKind = MakeShared<FJsonObject>();
	for (const TPair<FString, int32>& Pair : DroppedByKind)
	{
		ByKind->SetNumberField(Pair.Key, Pair.Value);
	}
	OutProperties->SetObjectField(TEXT("dropped_by_kind"), ByKind);
	FJsonObjectPtr ByReason = MakeShared<FJsonObject>();
	for (const TPair<FString, int32>& Pair : DroppedByReason)
	{
		ByReason->SetNumberField(Pair.Key, Pair.Value);
	}
	OutProperties->SetObjectField(TEXT("dropped_by_reason"), ByReason);
	OutProperties->SetArrayField(TEXT("dropped_list"), Dropped);
	OutProperties->SetArrayField(TEXT("hidden_structure_candidates"), HiddenStructure);

	// Inner objects the freshly created asset already owns (graph/node/expression style structure).
	TMap<FString, int32> InnerClasses;
	if (Asset != nullptr)
	{
		TArray<UObject*> Inner;
		GetObjectsWithOuter(Asset, Inner);
		for (const UObject* Object : Inner)
		{
			if (Object != nullptr && !Object->HasAnyFlags(RF_Transient))
			{
				InnerClasses.FindOrAdd(Object->GetClass()->GetName())++;
			}
		}
	}
	FJsonObjectPtr InnerObject = MakeShared<FJsonObject>();
	for (const TPair<FString, int32>& Pair : InnerClasses)
	{
		InnerObject->SetNumberField(Pair.Key, Pair.Value);
	}
	OutProperties->SetObjectField(TEXT("default_inner_objects"), InnerObject);
}

// Details-visible properties whose value holds an editor-derived reference (a non-instanced
// reference to an object inside the asset; see FindAssetDocumentEditorDerivedReference).
// "omitted" is false when the reference sits inside an instanced subobject/struct, whose
// fields documents still author in place.
void FAuditRun::ScanDerivedReferences(const UObject* Asset, const TCHAR* Stage, TMap<FString, FJsonObjectPtr>& InOutFound) const
{
	if (Asset == nullptr)
	{
		return;
	}
	for (TFieldIterator<FProperty> It(Asset->GetClass()); It; ++It)
	{
		if (!It->HasAnyPropertyFlags(CPF_Edit) || InOutFound.Contains(It->GetName()))
		{
			continue;
		}
		const void* Value = It->ContainerPtrToValuePtr<void>(Asset);
		FString ValuePath;
		const UObject* Derived = FindAssetDocumentEditorDerivedReference(**It, Value, *Asset, ValuePath);
		const bool bOmitted = Derived != nullptr;
		if (Derived == nullptr)
		{
			Derived = FindAssetDocumentEditorDerivedReference(**It, Value, *Asset, ValuePath, true);
		}
		if (Derived == nullptr)
		{
			continue;
		}
		FJsonObjectPtr Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"), It->GetName());
		Row->SetStringField(TEXT("kind"), DescribeKind(*It));
		Row->SetStringField(TEXT("value_path"), ValuePath);
		Row->SetStringField(TEXT("object_class"), Derived->GetClass()->GetPathName());
		Row->SetBoolField(TEXT("omitted"), bOmitted);
		Row->SetStringField(TEXT("stage"), Stage);
		InOutFound.Add(It->GetName(), Row);
	}
}

// ---------------------------------------------------------------------------------------------
// Edit step
// ---------------------------------------------------------------------------------------------

void FAuditRun::EditStep(const FAuditClass& Entry, const FTarget& Target, const FJsonObjectPtr& ExtractAllPayload,
	FStep& OutStep, TArray<FJsonValuePtr>& OutEdits) const
{
	if (!ExtractAllPayload.IsValid() || !ExtractAllPayload->HasTypedField<EJson::Object>(TEXT("Properties")))
	{
		OutStep.Status = TEXT("skipped");
		OutStep.Message = TEXT("no Extract properties");
		return;
	}
	const FJsonObjectPtr Current = ExtractAllPayload->GetObjectField(TEXT("Properties"));
	struct FPick
	{
		FString Name;
		FString Kind;
		FString Coarse;
		FJsonValuePtr Before;
		FJsonValuePtr Requested;
	};
	TArray<FPick> Picks;
	// Bucket order gives variety; one pick per bucket group.
	const TArray<TArray<FString>> Buckets = {
		{TEXT("bool")}, {TEXT("int"), TEXT("float")}, {TEXT("enum")}, {TEXT("string"), TEXT("name")},
		{TEXT("struct")}, {TEXT("array")}, {TEXT("map")}, {TEXT("set")},
		{TEXT("soft_object_ref"), TEXT("soft_class_ref")}, {TEXT("fixed_array")},
		{TEXT("object_ref"), TEXT("class_ref")}, {TEXT("instanced_subobject")}, {TEXT("text")}};
	TArray<FString> Names;
	AssetDocumentServerJson::GetKeys(*Current, Names);
	Names.Sort();
	for (const TArray<FString>& Bucket : Buckets)
	{
		if (Picks.Num() >= 8)
		{
			break;
		}
		for (const FString& Name : Names)
		{
			const FProperty* Property = Entry.Class->FindPropertyByName(*Name);
			if (Property == nullptr || Property->HasAnyPropertyFlags(CPF_EditConst)
				|| !Bucket.Contains(CoarseKind(Property)))
			{
				continue;
			}
			const FJsonValuePtr Before = Current->TryGetField(Name);
			const FJsonValuePtr Requested = MutateJson(Property, Before, false, 0);
			if (!Requested.IsValid() || JsonEquals(Before, Requested))
			{
				continue;
			}
			Picks.Add({Name, DescribeKind(Property), CoarseKind(Property), CloneValue(Before), Requested});
			break;
		}
	}
	if (Picks.IsEmpty())
	{
		OutStep.Status = TEXT("n/a");
		OutStep.Message = TEXT("no writable property could be mutated (none emitted or no mutable kind)");
		return;
	}

	auto BuildDocument = [&](const TArray<const FPick*>& Chosen)
	{
		FJsonObjectPtr Document = CloneObject(ExtractAllPayload);
		Document->SetStringField(TEXT("Action"), TEXT("Update"));
		FJsonObjectPtr Properties = MakeShared<FJsonObject>();
		for (const FPick* Pick : Chosen)
		{
			Properties->SetField(Pick->Name, CloneValue(Pick->Requested));
		}
		Document->SetObjectField(TEXT("Properties"), Properties);
		return Document;
	};
	auto Verify = [&](const TArray<const FPick*>& Chosen, TArray<FString>& OutStatuses, TArray<FJsonValuePtr>& OutAfters) -> bool
	{
		const FAssetDocumentResult After = ExtractAll(Target);
		bool bAll = true;
		const FJsonObjectPtr AfterProps = After.IsSuccess() && After.Payload.IsValid()
			&& After.Payload->HasTypedField<EJson::Object>(TEXT("Properties"))
			? After.Payload->GetObjectField(TEXT("Properties")) : nullptr;
		for (const FPick* Pick : Chosen)
		{
			FString Status;
			FJsonValuePtr Value;
			if (!AfterProps.IsValid())
			{
				Status = TEXT("extract-failed");
			}
			else
			{
				Value = AfterProps->TryGetField(Pick->Name);
				if (!Value.IsValid())
				{
					Status = TEXT("vanished-after-apply");
				}
				else if (JsonEquals(ProjectOntoAuthored(Value, Pick->Requested), Pick->Requested))
				{
					// Partial values: members the request omits keep (or get) the editor's value.
					Status = TEXT("match");
				}
				else if (JsonEquals(Value, Pick->Before))
				{
					Status = TEXT("silently-ignored");
				}
				else
				{
					Status = TEXT("altered");
				}
			}
			bAll &= Status == TEXT("match");
			OutStatuses.Add(Status);
			OutAfters.Add(Value);
		}
		return bAll;
	};

	TArray<const FPick*> All;
	for (const FPick& Pick : Picks)
	{
		All.Add(&Pick);
	}
	TArray<FString> Statuses;
	TArray<FJsonValuePtr> Afters;
	FStep Combined;
	const FAssetDocumentResult CombinedResult = Apply(Target, BuildDocument(All), true);
	FillFromResult(Combined, CombinedResult);
	bool bPerPick = false;
	if (CombinedResult.IsSuccess())
	{
		const bool bAllMatch = Verify(All, Statuses, Afters);
		OutStep.Status = bAllMatch ? TEXT("pass") : TEXT("fail");
		OutStep.Message = bAllMatch ? FString() : TEXT("some edits did not read back as requested");
		OutStep.bSavedAsset = CombinedResult.bSavedAsset;
	}
	else
	{
		OutStep.Message = FString::Printf(TEXT("combined apply failed: %s"), *Combined.Message);
		OutStep.Codes = Combined.Codes;
		bPerPick = true;
	}
	for (int32 Index = 0; Index < Picks.Num(); ++Index)
	{
		const FPick& Pick = Picks[Index];
		FJsonObjectPtr Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"), Pick.Name);
		Row->SetStringField(TEXT("kind"), Pick.Kind);
		Row->SetStringField(TEXT("coarse"), Pick.Coarse);
		Row->SetStringField(TEXT("before"), ValueToString(Pick.Before));
		Row->SetStringField(TEXT("requested"), ValueToString(Pick.Requested));
		if (!bPerPick)
		{
			Row->SetStringField(TEXT("result"), Statuses[Index]);
			Row->SetStringField(TEXT("after"), ValueToString(Afters[Index]));
		}
		else
		{
			TArray<const FPick*> Single = {&Pick};
			FStep SingleStep;
			FAssetDocumentResult SingleResult = Apply(Target, BuildDocument(Single), true);
			bool bFixedFloats = false;
			if (!SingleResult.IsSuccess())
			{
				const FAssetDocumentResult Retry = Apply(Target, BuildDocument(Single), true, true);
				if (Retry.IsSuccess())
				{
					SingleResult = Retry;
					bFixedFloats = true;
				}
			}
			FillFromResult(SingleStep, SingleResult);
			if (SingleResult.IsSuccess())
			{
				TArray<FString> SingleStatuses;
				TArray<FJsonValuePtr> SingleAfters;
				Verify(Single, SingleStatuses, SingleAfters);
				Row->SetStringField(TEXT("result"), SingleStatuses[0]);
				Row->SetStringField(TEXT("after"), ValueToString(SingleAfters[0]));
				if (bFixedFloats)
				{
					Row->SetBoolField(TEXT("needed_float_literal_fix"), true);
				}
			}
			else
			{
				Row->SetStringField(TEXT("result"), TEXT("apply-rejected"));
				Row->SetStringField(TEXT("error"), Trim(SingleStep.Message, 300));
				if (!SingleStep.Codes.IsEmpty())
				{
					Row->SetField(TEXT("codes"), MakeStringArray(SingleStep.Codes));
				}
			}
		}
		OutEdits.Add(MakeShared<FJsonValueObject>(Row));
	}
	if (bPerPick)
	{
		int32 Matches = 0;
		for (const FJsonValuePtr& Row : OutEdits)
		{
			Matches += Row->AsObject()->GetStringField(TEXT("result")) == TEXT("match") ? 1 : 0;
		}
		OutStep.Status = Matches == OutEdits.Num() ? TEXT("pass") : (Matches > 0 ? TEXT("partial") : TEXT("fail"));
	}
}

// ---------------------------------------------------------------------------------------------
// One class
// ---------------------------------------------------------------------------------------------

FJsonObjectPtr FAuditRun::AuditClass(const FAuditClass& Entry, int32 Index) const
{
	const double StartSeconds = FPlatformTime::Seconds();
	const FString ClassPath = Entry.Class->GetPathName();
	const FTarget Target = MakeTarget(Index, *Entry.Class);
	FJsonObjectPtr Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("class"), ClassPath);
	Result->SetStringField(TEXT("module"), ModuleOf(Entry.Class));
	Result->SetStringField(TEXT("factory"), Entry.FactoryPath);
	Result->SetBoolField(TEXT("factory_exact"), Entry.bExactFactory);
	Result->SetBoolField(TEXT("factory_in_new_menu"), Entry.bFactoryInNewMenu);
	Result->SetStringField(TEXT("super"), Entry.Class->GetSuperClass() ? Entry.Class->GetSuperClass()->GetName() : FString());

	CurrentClass = Entry.Class;
	CleanupTarget(Target);

	// a. CreateTemplate
	FStep TemplateStep;
	FJsonObjectPtr TemplateDocument;
	{
		FAssetDocumentTemplateRequest Request;
		Request.Class = ClassPath;
		Request.Target = Target.PackageName;
		const FAssetDocumentResult Template = Service.CreateTemplate(Request);
		FillFromResult(TemplateStep, Template);
		const bool bValid = Template.IsSuccess() && Template.Payload.IsValid()
			&& Template.Payload->GetStringField(TEXT("Class")) == ClassPath;
		TemplateStep.Status = bValid ? TEXT("pass") : TEXT("fail");
		if (bValid)
		{
			TemplateDocument = CloneObject(Template.Payload);
		}
	}
	Result->SetObjectField(TEXT("template"), StepToJson(TemplateStep));
	// A class with its own Profile: its template carries a non-empty Body, and (decided below once the asset exists)
	// its Extract lacks the generic "AssetType": "GenericAsset" marker.
	const TSharedPtr<FJsonObject>* TemplateBody = nullptr;
	bool bSpecialized = Entry.bSpecialized || (TemplateDocument.IsValid()
		&& TemplateDocument->TryGetObjectField(TEXT("Body"), TemplateBody) && !(*TemplateBody)->Values.IsEmpty());

	// b. durable create
	FStep CreateStep;
	if (!TemplateDocument.IsValid())
	{
		TemplateDocument = MakeShared<FJsonObject>();
		TemplateDocument->SetNumberField(TEXT("SchemaVersion"), 1);
		TemplateDocument->SetStringField(TEXT("Target"), Target.PackageName);
		TemplateDocument->SetStringField(TEXT("Class"), ClassPath);
		TemplateDocument->SetObjectField(TEXT("Definitions"), MakeShared<FJsonObject>());
		TemplateDocument->SetObjectField(TEXT("Properties"), MakeShared<FJsonObject>());
	}
	TemplateDocument->SetStringField(TEXT("Action"), TEXT("Create"));
	FAssetDocumentResult Created = Apply(Target, TemplateDocument, true);
	// A generic create refused for the missing New-asset dialog choice is retried with the FactoryContext an author
	// would give (an existing asset or a picker class); the refusal itself is correct.
	if (!bSpecialized && !Created.IsSuccess() && Created.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
		{ return Diagnostic.Code == AssetDocumentEditorMissingFactoryContextCode; }))
	{
		const FString FactoryContext = PickFactoryContext(Entry.FactoryPath);
		Result->SetStringField(TEXT("factory_context"), FactoryContext.IsEmpty() ? FString(TEXT("(none available)")) : FactoryContext);
		if (!FactoryContext.IsEmpty())
		{
			CleanupTarget(Target);
			TemplateDocument->SetStringField(TEXT("FactoryContext"), FactoryContext);
			Created = Apply(Target, TemplateDocument, true);
		}
	}
	FillFromResult(CreateStep, Created);
	UObject* Asset = FSoftObjectPath(Target.ObjectPath).ResolveObject();
	const bool bFileOnDisk = IFileManager::Get().FileExists(
		*FPackageName::LongPackageNameToFilename(Target.PackageName, FPackageName::GetAssetPackageExtension()))
		|| IFileManager::Get().FileExists(
			*FPackageName::LongPackageNameToFilename(Target.PackageName, FPackageName::GetMapPackageExtension()));
	CreateStep.Status = Created.IsSuccess() && Asset != nullptr ? TEXT("pass") : TEXT("fail");
	Result->SetObjectField(TEXT("create"), StepToJson(CreateStep));
	Result->SetBoolField(TEXT("create_saved_to_disk"), bFileOnDisk);
	Result->SetBoolField(TEXT("create_reported_saved"), Created.bSavedAsset);
	if (Asset != nullptr)
	{
		Result->SetStringField(TEXT("created_class"), Asset->GetClass()->GetPathName());
	}

	FStep ExtractStep;
	FStep EditStepResult;
	FStep ReloadStep;
	FStep NoOpDiffStep;
	FStep NoOpAllStep;
	FStep InspectStep;
	FJsonObjectPtr ExtractAllPayload;
	FJsonObjectPtr InspectPayload;
	TArray<FJsonValuePtr> Edits;
	TMap<FString, FJsonObjectPtr> DerivedReferences;
	if (CreateStep.Status == TEXT("pass"))
	{
		ScanDerivedReferences(Asset, TEXT("create"), DerivedReferences);
		// c. Extract (all writable) + Inspect
		const FAssetDocumentResult Extracted = ExtractAll(Target);
		FillFromResult(ExtractStep, Extracted);
		ExtractStep.Status = Extracted.IsSuccess() && Extracted.Payload.IsValid() ? TEXT("pass") : TEXT("fail");
		if (ExtractStep.Status == TEXT("pass"))
		{
			ExtractAllPayload = CloneObject(Extracted.Payload);
			DumpExtract(Target, *Entry.Class, ExtractAllPayload);
			FString AssetType;
			bSpecialized |= !(ExtractAllPayload->TryGetStringField(TEXT("AssetType"), AssetType) && AssetType == TEXT("GenericAsset"));
		}
		FAssetDocumentInspectRequest InspectRequest;
		InspectRequest.ClassOrAsset = Target.ObjectPath;
		const FAssetDocumentResult Inspected = Service.Inspect(InspectRequest);
		FillFromResult(InspectStep, Inspected);
		InspectStep.Status = Inspected.IsSuccess() && Inspected.Payload.IsValid() ? TEXT("pass") : TEXT("fail");
		if (InspectStep.Status == TEXT("pass"))
		{
			InspectPayload = CloneObject(Inspected.Payload);
		}
		Result->SetObjectField(TEXT("inspect"), StepToJson(InspectStep));

		FJsonObjectPtr PropertyAnalysis;
		AnalyzeProperties(Entry, Asset, ExtractAllPayload, InspectPayload, PropertyAnalysis);
		Result->SetObjectField(TEXT("properties"), PropertyAnalysis);
		const FAssetDocumentResult DiffExtract = ExtractDiff(Target);
		if (DiffExtract.IsSuccess() && DiffExtract.Payload.IsValid()
			&& DiffExtract.Payload->HasTypedField<EJson::Object>(TEXT("Properties")))
		{
			Result->SetNumberField(TEXT("diff_extract_property_count"),
				DiffExtract.Payload->GetObjectField(TEXT("Properties"))->Values.Num());
		}

		// d. edit (specialized classes use profile-specific document forms; synthetic edits are not meaningful there)
		if (bSpecialized)
		{
			EditStepResult.Status = TEXT("n/a");
			EditStepResult.Message = TEXT("specialized profile: edit forms are covered by its own tests");
		}
		else if (ExtractStep.Status == TEXT("pass"))
		{
			EditStep(Entry, Target, ExtractAllPayload, EditStepResult, Edits);
		}
		else
		{
			EditStepResult.Message = TEXT("no Extract");
		}

		// e. reload from disk and compare
		UObject* Live = FSoftObjectPath(Target.ObjectPath).ResolveObject();
		ScanDerivedReferences(Live, TEXT("edit"), DerivedReferences);
		if (ExtractStep.Status == TEXT("pass") && Live != nullptr)
		{
			const FAssetDocumentResult BeforeReload = ExtractAll(Target);
			TArray<UPackage*> Packages;
			for (TObjectIterator<UPackage> It; It; ++It)
			{
				if (It->GetName().StartsWith(Target.Directory))
				{
					Packages.Add(*It);
				}
			}
			bool bDirty = false;
			for (const UPackage* Package : Packages)
			{
				bDirty |= Package->IsDirty();
			}
			if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting && !GEditor->IsTransactionActive()
				&& GEditor->Trans->GetQueueLength() > 0)
			{
				GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentCoverageAudit", "Reload", "AssetDocument coverage audit reload"));
			}
			UPackageTools::UnloadPackages(Packages);
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			const bool bStillLoaded = FSoftObjectPath(Target.ObjectPath).ResolveObject() != nullptr;
			const FAssetDocumentResult AfterReload = ExtractAll(Target);
			ScanDerivedReferences(FSoftObjectPath(Target.ObjectPath).ResolveObject(), TEXT("reload"), DerivedReferences);
			if (!BeforeReload.IsSuccess() || !AfterReload.IsSuccess())
			{
				ReloadStep.Status = TEXT("fail");
				FillFromResult(ReloadStep, AfterReload);
				ReloadStep.Message = FString::Printf(TEXT("extract after reload failed: %s"), *ReloadStep.Message);
			}
			else
			{
				const FJsonObjectPtr BeforeProps = BeforeReload.Payload->GetObjectField(TEXT("Properties"));
				const FJsonObjectPtr AfterProps = AfterReload.Payload->GetObjectField(TEXT("Properties"));
				TArray<FString> Differences;
				TSet<FString> Keys;
				for (const TPair<FString, FJsonValuePtr>& Pair : BeforeProps->Values) { Keys.Add(Pair.Key); }
				for (const TPair<FString, FJsonValuePtr>& Pair : AfterProps->Values) { Keys.Add(Pair.Key); }
				for (const FString& Key : Keys)
				{
					if (!JsonEquals(BeforeProps->TryGetField(Key), AfterProps->TryGetField(Key)))
					{
						Differences.Add(FString::Printf(TEXT("%s: %s -> %s"), *Key,
							*ValueToString(BeforeProps->TryGetField(Key), 2000), *ValueToString(AfterProps->TryGetField(Key), 2000)));
					}
				}
				ReloadStep.Status = Differences.IsEmpty() ? TEXT("pass") : TEXT("fail");
				if (!Differences.IsEmpty())
				{
					ReloadStep.Message = FString::Printf(TEXT("%d properties differ after reload; first: %s"), Differences.Num(),
						*Differences[0]);
					Result->SetField(TEXT("reload_differences"), MakeStringArray(
						TArray<FString>(Differences.GetData(), FMath::Min(Differences.Num(), 12))));
				}
				if (bDirty)
				{
					ReloadStep.Message += TEXT(" (package was dirty before unload)");
				}
				if (bStillLoaded)
				{
					ReloadStep.Message += TEXT(" (unload did not release the object; reload may have compared in-memory state)");
				}
				ReloadStep.Codes.Empty();
			}
			// f. unchanged re-apply is a no-op
			if (AfterReload.IsSuccess())
			{
				// f1: the default diff-only extract document
				const FAssetDocumentResult Diff = ExtractDiff(Target);
				if (Diff.IsSuccess() && Diff.Payload.IsValid())
				{
					const FAssetDocumentResult Reapplied = Apply(Target, CloneObject(Diff.Payload), true);
					FillFromResult(NoOpDiffStep, Reapplied);
					NoOpDiffStep.Status = Reapplied.IsSuccess() && !Reapplied.bSavedAsset ? TEXT("pass") : TEXT("fail");
					if (Reapplied.IsSuccess() && Reapplied.bSavedAsset)
					{
						NoOpDiffStep.Message += TEXT(" | re-apply of an unchanged document saved the asset");
					}
				}
				else
				{
					NoOpDiffStep.Status = TEXT("fail");
					NoOpDiffStep.Message = TEXT("diff extract failed");
				}
				// f2: the full include-all extract document
				const FAssetDocumentResult Full = ExtractAll(Target);
				if (Full.IsSuccess() && Full.Payload.IsValid())
				{
					const FAssetDocumentResult Reapplied = Apply(Target, CloneObject(Full.Payload), true);
					FillFromResult(NoOpAllStep, Reapplied);
					NoOpAllStep.Status = Reapplied.IsSuccess() && !Reapplied.bSavedAsset ? TEXT("pass") : TEXT("fail");
					if (Reapplied.IsSuccess() && Reapplied.bSavedAsset)
					{
						NoOpAllStep.Message += TEXT(" | re-apply of the unchanged full extract saved the asset");
					}
					if (NoOpAllStep.Status == TEXT("fail"))
					{
						const FAssetDocumentResult Fixed = Apply(Target, CloneObject(Full.Payload), true, true);
						Result->SetBoolField(TEXT("noop_full_ok_with_float_literals"), Fixed.IsSuccess() && !Fixed.bSavedAsset);
						TArray<FJsonValuePtr> Failures;
						BisectProperties(Target, CloneObject(Full.Payload), Failures);
						Result->SetArrayField(TEXT("noop_failing_properties"), Failures);
					}
				}
				else
				{
					NoOpAllStep.Status = TEXT("fail");
					NoOpAllStep.Message = TEXT("full extract failed");
				}
			}
		}
		else
		{
			ReloadStep.Message = TEXT("nothing to reload");
		}
	}
	Result->SetObjectField(TEXT("extract"), StepToJson(ExtractStep));
	Result->SetObjectField(TEXT("edit"), StepToJson(EditStepResult));
	Result->SetArrayField(TEXT("edits"), Edits);
	Result->SetObjectField(TEXT("reload"), StepToJson(ReloadStep));
	Result->SetObjectField(TEXT("noop_diff"), StepToJson(NoOpDiffStep));
	Result->SetObjectField(TEXT("noop_full"), StepToJson(NoOpAllStep));
	if (!DerivedReferences.IsEmpty())
	{
		TArray<FJsonValuePtr> Rows;
		for (const TPair<FString, FJsonObjectPtr>& Pair : DerivedReferences)
		{
			Rows.Add(MakeShared<FJsonValueObject>(Pair.Value));
		}
		Result->SetArrayField(TEXT("derived_references"), Rows);
	}

	Result->SetBoolField(TEXT("specialized_profile"), bSpecialized);

	// g. cleanup
	CleanupTarget(Target);
	const FAssetData Leftover = FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(Target.ObjectPath));
	const bool bCleanFiles = !IFileManager::Get().DirectoryExists(*Target.ContentDir);
	Result->SetBoolField(TEXT("cleanup_ok"), !Leftover.IsValid() && bCleanFiles
		&& FSoftObjectPath(Target.ObjectPath).ResolveObject() == nullptr);
	Result->SetNumberField(TEXT("seconds"), FPlatformTime::Seconds() - StartSeconds);
	return Result;
}

void FAuditRun::Run()
{
	IFileManager::Get().MakeDirectory(*OutputDir, true);
	const bool bResume = GetEnv(TEXT("ASSETDOC_COVERAGE_RESUME")) == TEXT("1");
	const FString ResultsFile = FPaths::Combine(OutputDir, TEXT("results.jsonl"));
	const FString CurrentFile = FPaths::Combine(OutputDir, TEXT("current.txt"));
	TSet<FString> AlreadyDone;
	if (bResume)
	{
		FString PreviousCurrent;
		if (FFileHelper::LoadFileToString(PreviousCurrent, *CurrentFile) && !PreviousCurrent.TrimStartAndEnd().IsEmpty())
		{
			FJsonObjectPtr Crash = MakeShared<FJsonObject>();
			Crash->SetStringField(TEXT("class"), PreviousCurrent.TrimStartAndEnd());
			Crash->SetStringField(TEXT("skipped"), TEXT("editor crashed or hung while auditing this class (recorded on resume)"));
			AppendLine(TEXT("results.jsonl"), ToJsonString(Crash, false));
		}
		TArray<FString> Lines;
		FFileHelper::LoadFileToStringArray(Lines, *ResultsFile);
		for (const FString& Line : Lines)
		{
			FJsonObjectPtr Parsed;
			if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Line), Parsed) && Parsed.IsValid())
			{
				AlreadyDone.Add(Parsed->GetStringField(TEXT("class")));
			}
		}
	}
	else
	{
		IFileManager::Get().Delete(*ResultsFile, false, true);
	}
	IFileManager::Get().Delete(*CurrentFile, false, true);

	FEnumeration Enumeration = EnumerateCreatableClasses();
	Test.AddInfo(FString::Printf(TEXT("Coverage audit: %d creatable classes, %d refused, %d factories"),
		Enumeration.Classes.Num(), Enumeration.Refused.Num(), Enumeration.Factories.Num()));

	const TArray<FString> Only = SplitList(GetEnv(TEXT("ASSETDOC_COVERAGE_ONLY")));
	const TArray<FString> ExtraSkip = SplitList(GetEnv(TEXT("ASSETDOC_COVERAGE_SKIP")));
	const FString LimitText = GetEnv(TEXT("ASSETDOC_COVERAGE_LIMIT"));
	const bool bIgnoreSkipList = GetEnv(TEXT("ASSETDOC_COVERAGE_IGNORE_SKIPLIST")) == TEXT("1");
	const int32 Limit = LimitText.IsNumeric() ? FCString::Atoi(*LimitText) : MAX_int32;

	int32 Audited = 0;
	for (int32 Index = 0; Index < Enumeration.Classes.Num() && Audited < Limit; ++Index)
	{
		const FAuditClass& Entry = Enumeration.Classes[Index];
		const FString ClassPath = Entry.Class->GetPathName();
		if (AlreadyDone.Contains(ClassPath))
		{
			continue;
		}
		if (!Only.IsEmpty() && !Only.ContainsByPredicate([&](const FString& Part) { return ClassPath.Contains(Part); }))
		{
			continue;
		}
		FString SkipReason;
		for (const FSkipEntry& Skip : SkipList)
		{
			if (!bIgnoreSkipList && ClassPath == Skip.ClassPath)
			{
				SkipReason = Skip.Reason;
			}
		}
		if (ModuleOf(Entry.Class).StartsWith(TEXT("AssetDocument")))
		{
			SkipReason = TEXT("AssetDocument plugin test fixture class (not a product asset type)");
		}
		for (const FString& Part : ExtraSkip)
		{
			if (ClassPath.Contains(Part))
			{
				SkipReason = TEXT("skipped by ASSETDOC_COVERAGE_SKIP");
			}
		}
		if (!SkipReason.IsEmpty())
		{
			FJsonObjectPtr Skipped = MakeShared<FJsonObject>();
			Skipped->SetStringField(TEXT("class"), ClassPath);
			Skipped->SetStringField(TEXT("module"), ModuleOf(Entry.Class));
			Skipped->SetStringField(TEXT("factory"), Entry.FactoryPath);
			Skipped->SetStringField(TEXT("skipped"), SkipReason);
			AppendLine(TEXT("results.jsonl"), ToJsonString(Skipped, false));
			continue;
		}
		FFileHelper::SaveStringToFile(ClassPath, *CurrentFile);
		Test.AddInfo(FString::Printf(TEXT("Coverage audit [%d/%d] %s"), Index + 1, Enumeration.Classes.Num(), *ClassPath));
		UE_LOG(LogTemp, Display, TEXT("Coverage audit [%d/%d] %s"), Index + 1, Enumeration.Classes.Num(), *ClassPath);
		const FJsonObjectPtr ClassResult = AuditClass(Entry, Index);
		AppendLine(TEXT("results.jsonl"), ToJsonString(ClassResult, false));
		IFileManager::Get().Delete(*CurrentFile, false, true);
		++Audited;
	}

	// Aggregate everything recorded so far (including earlier resumed runs).
	TArray<FJsonValuePtr> ClassRows;
	TArray<FString> Lines;
	FFileHelper::LoadFileToStringArray(Lines, *ResultsFile);
	for (const FString& Line : Lines)
	{
		FJsonObjectPtr Parsed;
		if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Line), Parsed) && Parsed.IsValid())
		{
			ClassRows.Add(MakeShared<FJsonValueObject>(Parsed));
		}
	}
	FJsonObjectPtr Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("generated_utc"), FDateTime::UtcNow().ToIso8601());
	Root->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
	Root->SetNumberField(TEXT("creatable_class_count"), Enumeration.Classes.Num());
	Root->SetArrayField(TEXT("factories"), Enumeration.Factories);
	Root->SetArrayField(TEXT("refused_classes"), Enumeration.Refused);
	Root->SetArrayField(TEXT("creatable_classes"), Enumeration.Creatable);
	Root->SetArrayField(TEXT("classes"), ClassRows);
	FFileHelper::SaveStringToFile(ToJsonString(Root, true), *FPaths::Combine(OutputDir, TEXT("coverage-audit.json")),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	Test.AddInfo(FString::Printf(TEXT("Coverage audit wrote %d class rows to %s"), ClassRows.Num(), *OutputDir));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentCoverageGenericRouteAuditTest,
	"UEAssetDoc.Coverage.GenericRouteAudit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentCoverageGenericRouteAuditTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	// A measurement: engine/plugin logs raised while driving arbitrary factories must not fail the suite.
	AddExpectedError(TEXT("."), EAutomationExpectedErrorFlags::Contains, -1, true);
	AssetDocumentCoverageAudit::FAuditRun Run(*this);
	Run.Run();
	TestTrue(TEXT("coverage audit ran to completion"), true);
	return true;
}
#endif
