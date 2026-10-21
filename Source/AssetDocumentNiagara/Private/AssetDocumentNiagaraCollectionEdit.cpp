// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// fx.niagara.edit.v1 on a Niagara parameter collection (N5): the collection editor's parameter list and default values.
// Raw editor-equivalent code: the collection editor's view models (FNiagaraParameterCollectionAssetViewModel,
// FNiagaraCollectionParameterViewModel) are private to NiagaraEditor, so their effect is made here with the exported
// collection API. Behaviour tests: UEAssetDoc.Niagara.Collection.*.
//
// Parameters are named as the editor lists them: without the collection's "NPC.<Namespace>." prefix. Ops run in order;
// the first refused op fails the call (Core rolls the package back):
//   {op: "add_parameter", name, type}
//        "+" (AddParameter: Modify, UNiagaraParameterCollection::AddParameter, the type's default value in the default
//        instance) and the name the user types (VerifyNodeNameTextChanged: not empty, not taken); then CollectionChanged.
//   {op: "delete_parameter", name}          DeleteParameters: RemoveParameter, CollectionChanged with the system update
//   {op: "rename_parameter", name, to}      the name box (OnParameterNameChanged): the parameter and its default
//        instance value are renamed (UNiagaraParameterCollectionInstance::RenameParameter), CollectionChanged.
//   {op: "retype_parameter", name, type}    the type menu (OnParameterTypeChanged): the value is reset to the new
//        type's default, CollectionChanged with the system update.
//   {op: "parameter", name, value, reset?}  the value widget (NotifyDefaultValueChanged / ...PropertyChanged): plain
//        data through the Niagara value codec, {"DataInterface": {Properties}} (the data interface's Details; reset:
//        properties back to the class defaults), {"Object": path}; then OnParameterValueChangedInternal (system update,
//        OnParameterValuesEdited, the store's Tick).
// Parameters a source material parameter collection drives (IsExternallyDriven) are not editable in the editor
// (Niagara.CollectionParameterOwned). CollectionChanged (FNiagaraParameterCollectionAssetViewModel::CollectionChanged):
// every loaded instance of the collection syncs with it; a delete or retype also reinitializes the components whose
// systems use the collection (FNiagaraSystemUpdateContext). Namespace and SourceMaterialCollection are Details
// properties (generic property admission: PostEditChangeProperty renames the parameters / pulls the material
// collection's parameters in).
// Output {changed, created: []}.

#include "AssetDocumentNiagaraPrimitives.h"

#include "AssetDocumentEditorNonInteractiveScope.h"

#include "Editor.h"
#include "NiagaraCommon.h"
#include "NiagaraDataInterface.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraParameterCollection.h"
#include "NiagaraTypes.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectIterator.h"

#define LOCTEXT_NAMESPACE "AssetDocumentNiagaraCollectionEdit"

namespace UE::AssetDocument::Niagara
{
namespace
{
using Graph::FReader;

struct FCollectionEdit
{
	UNiagaraParameterCollection& Collection;
	UNiagaraParameterCollectionInstance& Instance;
	bool bChanged = false;
};

int32 IndexOfFriendly(const UNiagaraParameterCollection& Collection, const FString& Name)
{
	const FName Full = Collection.ParameterNameFromFriendlyString(Name);
	return Collection.GetParameters().IndexOfByPredicate([&Full](const FNiagaraVariable& Variable) { return Variable.GetName() == Full; });
}

bool RequireParameter(FReader& Op, FCollectionEdit& Edit, const TCHAR* Field, int32& OutIndex, bool bEditable = true)
{
	FString Name;
	if (!Op.String(Field, Name))
	{
		return false;
	}
	OutIndex = IndexOfFriendly(Edit.Collection, Name);
	if (OutIndex == INDEX_NONE)
	{
		return Op.Fail(TEXT("Niagara.ParameterNotFound"), FString::Printf(TEXT("The collection has no parameter '%s'."), *Name), Field);
	}
	if (bEditable && Edit.Instance.IsExternallyDriven(Edit.Collection.GetParameters()[OutIndex]))
	{
		return Op.Fail(TEXT("Niagara.CollectionParameterOwned"), FString::Printf(TEXT(
			"'%s' comes from the source material parameter collection, which owns its name, type and value."), *Name), Field);
	}
	return true;
}

// FNiagaraCollectionParameterViewModel::VerifyNodeNameTextChanged.
bool ValidNewName(FReader& Op, const UNiagaraParameterCollection& Collection, const FString& Name, const TCHAR* Field)
{
	if (Name.IsEmpty())
	{
		return Op.Fail(TEXT("Niagara.InvalidParameterName"), TEXT("A parameter name cannot be empty."), Field);
	}
	if (IndexOfFriendly(Collection, Name) != INDEX_NONE)
	{
		return Op.Fail(TEXT("Niagara.ParameterExists"), FString::Printf(TEXT("\"%s\" is already the name of another parameter in this collection."),
			*Name), Field);
	}
	return true;
}

// FNiagaraParameterCollectionAssetViewModel::SupportsType and the type list of the "+" menu.
bool RequireType(FReader& Op, const TCHAR* Field, FNiagaraTypeDefinition& OutType)
{
	FString Token;
	if (!Op.String(Field, Token))
	{
		return false;
	}
	if (!TypeFromToken(Token, OutType) || OutType == FNiagaraTypeDefinition::GetGenericNumericDef() || OutType.IsInternalType())
	{
		return Op.Fail(TEXT("Niagara.UnknownParameterType"), FString::Printf(TEXT("'%s' is not a parameter collection type."), *Token), Field);
	}
	return true;
}

void SetTypeDefault(UNiagaraParameterCollectionInstance& Instance, const FNiagaraVariable& Variable)
{
	const FNiagaraTypeDefinition& Type = Variable.GetType();
	if (!Type.IsDataInterface() && !Type.IsUObject())
	{
		TArray<uint8> DefaultData;
		FNiagaraEditorUtilities::GetTypeDefaultValue(Type, DefaultData);
		Instance.GetParameterStore().SetParameterData(DefaultData.GetData(), Variable);
	}
}

// FNiagaraParameterCollectionAssetViewModel::CollectionChanged.
void CollectionChanged(UNiagaraParameterCollection& Collection, bool bRecompile)
{
	for (TObjectIterator<UNiagaraParameterCollectionInstance> It; It; ++It)
	{
		if (IsValid(*It) && It->Collection == &Collection)
		{
			It->SyncWithCollection();
		}
	}
	if (bRecompile)
	{
		FNiagaraSystemUpdateContext Context(&Collection, true);
	}
}

bool AddParameter(FReader& Op, FCollectionEdit& Edit)
{
	FString Name;
	FNiagaraTypeDefinition Type;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("type")}) || !Op.String(TEXT("name"), Name) || !RequireType(Op, TEXT("type"), Type)
		|| !ValidNewName(Op, Edit.Collection, Name, TEXT("name")))
	{
		return false;
	}
	const FScopedTransaction Transaction(LOCTEXT("AddNPCParameter", "Add Parameter"));
	Edit.Collection.Modify();
	Edit.Instance.Modify();
	const int32 Index = Edit.Collection.AddParameter(Edit.Collection.ParameterNameFromFriendlyString(Name), Type);
	if (!Edit.Collection.GetParameters().IsValidIndex(Index))
	{
		return Op.Fail(TEXT("Niagara.ParameterExists"), FString::Printf(TEXT("'%s' could not be added."), *Name), TEXT("name"));
	}
	SetTypeDefault(Edit.Instance, Edit.Collection.GetParameters()[Index]);
	CollectionChanged(Edit.Collection, false);
	Edit.bChanged = true;
	return true;
}

bool DeleteParameter(FReader& Op, FCollectionEdit& Edit)
{
	int32 Index = INDEX_NONE;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name")}) || !RequireParameter(Op, Edit, TEXT("name"), Index))
	{
		return false;
	}
	const FScopedTransaction Transaction(LOCTEXT("DeleteNPCParameter", "Delete Parameter"));
	Edit.Instance.Modify();
	const FNiagaraVariable Variable = Edit.Collection.GetParameters()[Index];
	Edit.Collection.RemoveParameter(Variable);
	CollectionChanged(Edit.Collection, true);
	Edit.bChanged = true;
	return true;
}

bool RenameParameter(FReader& Op, FCollectionEdit& Edit)
{
	int32 Index = INDEX_NONE;
	FString To;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("to")}) || !RequireParameter(Op, Edit, TEXT("name"), Index) || !Op.String(TEXT("to"), To))
	{
		return false;
	}
	const FName NewName = Edit.Collection.ParameterNameFromFriendlyString(To);
	const FNiagaraVariable Variable = Edit.Collection.GetParameters()[Index];
	if (Variable.GetName().IsEqual(NewName, ENameCase::CaseSensitive))
	{
		return true;
	}
	if (!ValidNewName(Op, Edit.Collection, To, TEXT("to")))
	{
		return false;
	}
	const FScopedTransaction Transaction(LOCTEXT("EditInputName", "Edit input name"));
	Edit.Collection.Modify();
	Edit.Instance.Modify();
	Edit.Collection.GetParameters()[Index].SetName(NewName);
	Edit.Instance.RenameParameter(Variable, NewName);
	CollectionChanged(Edit.Collection, false);
	Edit.bChanged = true;
	return true;
}

bool RetypeParameter(FReader& Op, FCollectionEdit& Edit)
{
	int32 Index = INDEX_NONE;
	FNiagaraTypeDefinition Type;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("type")}) || !RequireParameter(Op, Edit, TEXT("name"), Index) || !RequireType(Op, TEXT("type"), Type))
	{
		return false;
	}
	const FNiagaraVariable Variable = Edit.Collection.GetParameters()[Index];
	if (Variable.GetType() == Type)
	{
		return true;
	}
	const FScopedTransaction Transaction(LOCTEXT("EditParameterType", "Edit type"));
	Edit.Collection.Modify();
	Edit.Instance.Modify();
	Edit.Instance.RemoveParameter(Variable);
	Edit.Collection.GetParameters()[Index].SetType(Type);
	Edit.Instance.AddParameter(Edit.Collection.GetParameters()[Index]);
	SetTypeDefault(Edit.Instance, Edit.Collection.GetParameters()[Index]);
	CollectionChanged(Edit.Collection, true);
	Edit.bChanged = true;
	return true;
}

bool SetDataInterface(FReader& Op, UNiagaraDataInterface& DataInterface, const FValue& Value, bool& bOutChanged)
{
	const FValue* Spec = Value.Kind == EAssetDocumentRuntimeValueKind::Object ? Graph::FindField(Value, TEXT("DataInterface")) : nullptr;
	if (Spec == nullptr || Spec->Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return Op.Fail(TEXT("Niagara.InvalidCollectionValue"), TEXT(
			"A data interface parameter's value is {\"DataInterface\": {\"Class\": ..., \"Properties\": {...}}}."), TEXT("value"));
	}
	const FValue* ClassValue = Graph::FindField(*Spec, TEXT("Class"));
	const FString Label = ClassLabel(DataInterface.GetClass(), TEXT("NiagaraDataInterface"), TEXT(""));
	if (ClassValue != nullptr && (ClassValue->Kind != EAssetDocumentRuntimeValueKind::String
		|| (ClassValue->StringValue != Label && ClassValue->StringValue != DataInterface.GetClass()->GetPathName())))
	{
		return Op.Fail(TEXT("Niagara.InvalidCollectionValue"), FString::Printf(TEXT(
			"The data interface class is the parameter's type (%s); change the Type to change it."), *Label), TEXT("value/DataInterface/Class"));
	}
	// NotifyDefaultValuePropertyChanged: the data interface's Details.
	const UObject* Defaults = DataInterface.GetClass()->GetDefaultObject();
	auto Write = [&DataInterface, &bOutChanged](FProperty& Property, TFunctionRef<void(void*)> Set)
	{
		DataInterface.Modify();
		DataInterface.PreEditChange(&Property);
		Set(Property.ContainerPtrToValuePtr<void>(&DataInterface));
		FPropertyChangedEvent Event(&Property, EPropertyChangeType::ValueSet);
		DataInterface.PostEditChangeProperty(Event);
		bOutChanged = true;
	};
	const FValue* Reset = nullptr;
	if (Op.Has(TEXT("reset")) && !Op.Array(TEXT("reset"), Reset))
	{
		return false;
	}
	for (int32 Index = 0; Reset != nullptr && Index < Reset->Elements.Num(); ++Index)
	{
		const FValue& ResetName = Reset->Elements[Index];
		const FString Where = FString::Printf(TEXT("reset/%d"), Index);
		FProperty* Property = ResetName.Kind == EAssetDocumentRuntimeValueKind::String ? FindEditableProperty(*DataInterface.GetClass(), ResetName.StringValue) : nullptr;
		if (Property == nullptr)
		{
			return Op.Fail(TEXT("Niagara.UnknownProperty"), FString::Printf(TEXT("%s has no editable property '%s'."),
				*DataInterface.GetClass()->GetName(), *ResetName.StringValue), Where);
		}
		const void* DefaultPtr = Property->ContainerPtrToValuePtr<void>(Defaults);
		if (PropertyMatchesDefault(*Property, Property->ContainerPtrToValuePtr<void>(&DataInterface), DefaultPtr))
		{
			continue;
		}
		if (Property->ContainsInstancedObjectProperty())
		{
			return Op.Fail(TEXT("Niagara.InvalidPropertyValue"), FString::Printf(TEXT(
				"'%s' holds instanced objects; author its value instead of leaving it out."), *ResetName.StringValue), Where);
		}
		Write(*Property, [Property, DefaultPtr](void* ValuePtr) { Property->CopyCompleteValue(ValuePtr, DefaultPtr); });
	}
	const FValue* Properties = Graph::FindField(*Spec, TEXT("Properties"));
	if (Properties == nullptr)
	{
		return true;
	}
	if (Properties->Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return Op.Fail(TEXT("Niagara.InvalidCollectionValue"), TEXT("Properties is an object."), TEXT("value/DataInterface/Properties"));
	}
	for (int32 Index = 0; Index < Properties->ObjectKeys.Num(); ++Index)
	{
		const FString& Key = Properties->ObjectKeys[Index];
		const FString Where = FString(TEXT("value/DataInterface/Properties/")) + Key;
		FProperty* Property = FindEditableProperty(*DataInterface.GetClass(), Key);
		if (Property == nullptr)
		{
			return Op.Fail(TEXT("Niagara.UnknownProperty"), FString::Printf(TEXT("%s has no editable property '%s'."),
				*DataInterface.GetClass()->GetName(), *Key), Where);
		}
		// Complete at every depth: the value edits the class default's (the read is sparse over it).
		const void* DefaultPtr = Property->ContainerPtrToValuePtr<void>(DataInterface.GetClass()->GetDefaultObject());
		bool bSame = false;
		FString Code;
		if (!DecodePropertyIsSame(*Property, Properties->Elements[Index], Property->ContainerPtrToValuePtr<void>(&DataInterface), &DataInterface, bSame, Code,
			DefaultPtr))
		{
			return Op.Fail(TEXT("Niagara.InvalidPropertyValue"), FString::Printf(TEXT("'%s' does not fit the property (%s)."), *Key, *Code), Where);
		}
		if (!bSame)
		{
			const FValue& Element = Properties->Elements[Index];
			Write(*Property, [Property, &Element, &DataInterface, DefaultPtr](void* ValuePtr)
			{
				FString Ignored;
				DecodeProperty(*Property, Element, ValuePtr, &DataInterface, Ignored, DefaultPtr);
			});
		}
	}
	return true;
}

bool SetParameterValue(FReader& Op, FCollectionEdit& Edit)
{
	int32 Index = INDEX_NONE;
	const FValue* Value = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("value"), TEXT("reset")}) || !RequireParameter(Op, Edit, TEXT("name"), Index))
	{
		return false;
	}
	Value = Op.Find(TEXT("value"));
	if (Value == nullptr)
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("value is required."), TEXT("value"));
	}
	const FNiagaraVariable Variable = Edit.Collection.GetParameters()[Index];
	const FNiagaraTypeDefinition& Type = Variable.GetType();
	FNiagaraParameterStore& Store = Edit.Instance.GetParameterStore();
	bool bChanged = false;
	const FScopedTransaction Transaction(LOCTEXT("EditParameterValue", "Edit parameter value"));
	if (Type.IsDataInterface())
	{
		UNiagaraDataInterface* DataInterface = Store.GetDataInterface(Variable);
		if (DataInterface == nullptr)
		{
			return Op.Fail(TEXT("Niagara.InvalidCollectionValue"), TEXT("The parameter has no data interface."), TEXT("value"));
		}
		Edit.Instance.Modify();
		if (!SetDataInterface(Op, *DataInterface, *Value, bChanged))
		{
			return false;
		}
	}
	else if (Type.IsUObject())
	{
		// SetDefaultValueObjectAsset (the asset picker).
		const FValue* Path = Value->Kind == EAssetDocumentRuntimeValueKind::Object ? Graph::FindField(*Value, TEXT("Object")) : nullptr;
		if (Path == nullptr || Path->Kind != EAssetDocumentRuntimeValueKind::String)
		{
			return Op.Fail(TEXT("Niagara.InvalidCollectionValue"), TEXT("An object parameter's value is {\"Object\": \"<object path>\"} (\"\" for none)."), TEXT("value"));
		}
		UObject* Object = nullptr;
		if (!Path->StringValue.IsEmpty())
		{
			Object = LoadObject<UObject>(nullptr, *Path->StringValue, nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (Object == nullptr || (Type.GetClass() != nullptr && !Object->IsA(Type.GetClass())))
			{
				return Op.Fail(TEXT("Niagara.InvalidCollectionValue"), FString::Printf(TEXT("'%s' is not a %s."), *Path->StringValue,
					*GetNameSafe(Type.GetClass())), TEXT("value/Object"));
			}
		}
		if (Store.GetUObject(Variable) != Object)
		{
			Edit.Instance.Modify();
			Store.SetUObject(Object, Variable);
			bChanged = true;
		}
	}
	else
	{
		const uint8* Current = Store.GetParameterData(Variable);
		const int32 Size = Type.GetSize();
		if (Current == nullptr || Size <= 0)
		{
			return Op.Fail(TEXT("Niagara.InvalidCollectionValue"), TEXT("The parameter has no value storage."), TEXT("value"));
		}
		TArray<uint8> Data(Current, Size);
		FString Message;
		if (!DecodeValue(Type, *Value, Data.GetData(), Message))
		{
			return Op.Fail(TEXT("Niagara.InvalidCollectionValue"), Message, TEXT("value"));
		}
		if (FMemory::Memcmp(Data.GetData(), Current, Size) != 0)
		{
			Edit.Instance.Modify();
			Store.SetParameterData(Data.GetData(), Variable);
			bChanged = true;
		}
	}
	if (bChanged)
	{
		// OnParameterValueChangedInternal.
		{
			FNiagaraSystemUpdateContext UpdateContext(&Edit.Collection, true);
		}
		Edit.Collection.OnParameterValuesEdited.Broadcast({Variable.GetName()});
		Store.Tick();
		Edit.bChanged = true;
	}
	return true;
}
} // namespace

bool HandleCollectionEdit(FCall& Call, UNiagaraParameterCollection& Collection, const FValue& Ops, FObjectBuilder& Result)
{
	FCall& C = Call;
	UNiagaraParameterCollectionInstance* Instance = Collection.GetDefaultInstance();
	if (Instance == nullptr)
	{
		return Fail(C, TEXT("Niagara.ViewModelUnavailable"), FString::Printf(TEXT("%s has no default instance."), *Collection.GetPathName()));
	}
	FAssetDocumentEditorNonInteractiveScope NonInteractive;
	const FScopedTransaction Transaction(LOCTEXT("EditNiagaraParameterCollection", "Edit Niagara Parameter Collection"));
	Collection.Modify();
	FCollectionEdit Edit{Collection, *Instance};
	bool bOk = true;
	for (int32 Index = 0; Index < Ops.Elements.Num() && bOk; ++Index)
	{
		FReader Op = C.Args.Child(FString::Printf(TEXT("ops/%d"), Index), Ops.Elements[Index]);
		FString Kind;
		if (!Op.IsObject() || !Op.String(TEXT("op"), Kind))
		{
			bOk = Op.IsObject() ? false : Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("Each op is an object."));
			break;
		}
		if (Kind == TEXT("add_parameter"))
		{
			bOk = AddParameter(Op, Edit);
		}
		else if (Kind == TEXT("delete_parameter"))
		{
			bOk = DeleteParameter(Op, Edit);
		}
		else if (Kind == TEXT("rename_parameter"))
		{
			bOk = RenameParameter(Op, Edit);
		}
		else if (Kind == TEXT("retype_parameter"))
		{
			bOk = RetypeParameter(Op, Edit);
		}
		else if (Kind == TEXT("parameter"))
		{
			bOk = SetParameterValue(Op, Edit);
		}
		else
		{
			bOk = Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT(
				"Unknown parameter collection op '%s' (add_parameter, delete_parameter, rename_parameter, retype_parameter, parameter)."), *Kind),
				TEXT("op"));
		}
		if (bOk && NonInteractive.WasInteractionRefused())
		{
			bOk = Op.Fail(TEXT("Niagara.InteractiveEditRefused"), TEXT(
				"The edit tried to open an editor dialog, which an MCP request cannot answer; the edit was refused."));
		}
	}
	if (!bOk)
	{
		NonInteractive.DiscardCreatedPackages();
		return false;
	}
	if (Edit.bChanged)
	{
		Collection.MarkPackageDirty();
	}
	Result.AddBool(TEXT("changed"), Edit.bChanged);
	Result.Add(TEXT("created"), Graph::MakeArray({}));
	return true;
}
} // namespace UE::AssetDocument::Niagara

#undef LOCTEXT_NAMESPACE
