// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// User parameters, the way the system's User Parameters panel (FNiagaraSystemUserParameterBuilder) and its
// hierarchy editor (UNiagaraUserParametersHierarchyViewModel) edit them. Raw editor-equivalent code, isolated
// here: the panel's own entry points are not exported (FNiagaraEditorUtilities::AddParameter,
// FNiagaraSystemViewModel::RenameParameter / DeleteParameter and the reference helpers of
// FNiagaraEditorUtilities::UserParameters), so their steps are replayed with the exported pieces they call:
//   add     FNiagaraEditorUtilities::AddParameter: ResetVariableToDefaultValue + ExposedParameters.AddParameter
//           (the panel refuses duplicate names instead of making them unique).
//   delete  FNiagaraSystemViewModel::DeleteParameter: inputs linked to the parameter are reset and given the
//           parameter's value (DeleteUserParameterReferences), the parameter is removed from the exposed store,
//           UNiagaraSystem::HandleVariableRemoved clears data interface / renderer bindings; the hierarchy
//           drops the parameter's element (the hierarchy view model's refresh after the sync).
//   rename  FNiagaraSystemViewModel::RenameParameter: the user script variable and the exposed store are
//           renamed, each graph that references the parameter renames it (UNiagaraGraph::RenameParameter: map
//           get pins of linked inputs), UNiagaraSystem::HandleVariableRenamed renames bindings.
//   value   FNiagaraSystemUserParameterBuilder::OnDisplayDataPostChange / OnObjectAssetChanged: the exposed
//           store's SetParameterData / SetUObject; a data interface's Details edit (Modify, PreEditChange,
//           write, PostEditChangeProperty).
//   description  the Details panel of the parameter's UNiagaraScriptVariable (Metadata.Description).
//   category     the hierarchy editor: categories (UHierarchyCategory) under the system editor data's
//                UserParameterHierarchy, the parameter's element copied from the hierarchy view model's source
//                items into its category (UHierarchyElement::CopyAndAddItemUnderParentIdentity).
// Behaviour tests: UEAssetDoc.Niagara.Apply.UserParameters (document vs these editor paths).

#include "AssetDocumentNiagaraPrimitives.h"

#include "DataHierarchyViewModelBase.h"
#include "NiagaraDataInterface.h"
#include "NiagaraEditorDataBase.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraScriptSource.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemEditorData.h"
#include "NiagaraTypeRegistry.h"
#include "UObject/Package.h"
#include "UObject/StructOnScope.h"
#include "ViewModels/HierarchyEditor/NiagaraUserParametersHierarchyViewModel.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/Stack/NiagaraStackEntry.h"
#include "ViewModels/Stack/NiagaraStackFunctionInput.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"

namespace UE::AssetDocument::Niagara
{
namespace
{
const TCHAR* UserPrefix = TEXT("User.");

// The panel's user script variables (UNiagaraSystemEditorData::UserParameterMetaData is private; read through
// reflection, as the read does).
TArray<const UNiagaraScriptVariable*> UserScriptVariables(const UNiagaraSystem& System)
{
	TArray<const UNiagaraScriptVariable*> Out;
	const UNiagaraSystemEditorData* EditorData = Cast<UNiagaraSystemEditorData>(System.GetEditorData());
	const FArrayProperty* MetaData = CastField<FArrayProperty>(UNiagaraSystemEditorData::StaticClass()->FindPropertyByName(TEXT("UserParameterMetaData")));
	const FObjectPropertyBase* Inner = MetaData != nullptr ? CastField<FObjectPropertyBase>(MetaData->Inner) : nullptr;
	if (EditorData == nullptr || Inner == nullptr)
	{
		return Out;
	}
	FScriptArrayHelper Helper(MetaData, MetaData->ContainerPtrToValuePtr<void>(EditorData));
	for (int32 Index = 0; Index < Helper.Num(); ++Index)
	{
		if (const UNiagaraScriptVariable* Variable = Cast<UNiagaraScriptVariable>(Inner->GetObjectPropertyValue(Helper.GetRawPtr(Index))))
		{
			Out.Add(Variable);
		}
	}
	return Out;
}

FHierarchyElementIdentity ParameterIdentity(const FGuid& Guid)
{
	return FHierarchyElementIdentity({Guid}, {});
}

// Category path of every element below Element, by identity guid.
void CollectCategories(const UHierarchyElement& Element, const FString& Path, TMap<FGuid, FString>& Out)
{
	for (const UHierarchyElement* Child : Element.GetChildren())
	{
		if (Child == nullptr)
		{
			continue;
		}
		if (const UHierarchyCategory* Category = Cast<UHierarchyCategory>(Child))
		{
			const FString Name = Category->GetCategoryName().ToString();
			CollectCategories(*Child, Path.IsEmpty() ? Name : Path + TEXT("|") + Name, Out);
			continue;
		}
		const FHierarchyElementIdentity Identity = Child->GetPersistentIdentity();
		if (Identity.Guids.Num() == 1 && !Path.IsEmpty())
		{
			Out.Add(Identity.Guids[0], Path);
		}
		CollectCategories(*Child, Path, Out);
	}
}

// The element holding a child with Identity (searched recursively), or null.
UHierarchyElement* FindParent(UHierarchyElement& Element, const FHierarchyElementIdentity& Identity)
{
	for (UHierarchyElement* Child : Element.GetChildrenMutable())
	{
		if (Child == nullptr)
		{
			continue;
		}
		if (Child->GetPersistentIdentity() == Identity)
		{
			return &Element;
		}
		if (UHierarchyElement* Found = FindParent(*Child, Identity))
		{
			return Found;
		}
	}
	return nullptr;
}

struct FUserParameterContext
{
	FNiagaraSystemViewModel& ViewModel;
	UNiagaraSystem& System;
	UNiagaraSystemEditorData& EditorData;
	FNiagaraUserRedirectionParameterStore& Store;
};

// The exposed store's user parameters with their full "User." names (GetUserParameters lists the short
// redirection keys).
TArray<FNiagaraVariable> FullUserParameters(const FUserParameterContext& Context)
{
	TArray<FNiagaraVariable> Out;
	for (const FNiagaraVariableWithOffset& Parameter : Context.Store.ReadParameterVariables())
	{
		if (Parameter.GetName().ToString().StartsWith(UserPrefix, ESearchCase::CaseSensitive))
		{
			Out.Add(FNiagaraVariable(Parameter.GetType(), Parameter.GetName()));
		}
	}
	return Out;
}

bool FindUserParameter(const FUserParameterContext& Context, const FString& Name, FNiagaraVariable& OutVariable)
{
	for (const FNiagaraVariable& Parameter : FullUserParameters(Context))
	{
		if (Parameter.GetName().ToString() == Name)
		{
			OutVariable = Parameter;
			return true;
		}
	}
	return false;
}

bool NameTaken(const FUserParameterContext& Context, const FString& Name)
{
	return FullUserParameters(Context).ContainsByPredicate([&Name](const FNiagaraVariable& Parameter)
	{
		return Parameter.GetName().ToString().Equals(Name, ESearchCase::IgnoreCase);
	});
}

bool ValidName(Graph::FReader& Op, const FString& Name, const TCHAR* Field)
{
	if (!Name.StartsWith(UserPrefix, ESearchCase::CaseSensitive) || Name.Len() <= FCString::Strlen(UserPrefix)
		|| Name.TrimStartAndEnd() != Name || Name.Contains(TEXT("\n")))
	{
		return Op.Fail(TEXT("Niagara.InvalidUserParameterName"), FString::Printf(
			TEXT("'%s' is not a user parameter name: user parameters are named \"User.<name>\"."), *Name), Field);
	}
	return true;
}

// The editor graphs whose parameter references a rename follows: the system graph and each emitter graph.
TArray<UNiagaraGraph*> Graphs(UNiagaraSystem& System)
{
	TArray<UNiagaraGraph*> Out;
	auto AddSource = [&Out](UNiagaraScriptSourceBase* SourceBase)
	{
		const UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(SourceBase);
		if (Source != nullptr && Source->NodeGraph != nullptr)
		{
			Out.AddUnique(Source->NodeGraph);
		}
	};
	if (System.GetSystemSpawnScript() != nullptr)
	{
		AddSource(System.GetSystemSpawnScript()->GetLatestSource());
	}
	for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
	{
		if (FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData())
		{
			AddSource(Data->GraphSource);
		}
	}
	return Out;
}

// Stack inputs linked to a parameter (system stack and every emitter stack), as
// FNiagaraEditorUtilities::UserParameters::GetStackFunctionInputsWithLinkedParameter gathers them.
TArray<UNiagaraStackFunctionInput*> LinkedInputs(FNiagaraSystemViewModel& ViewModel, const FNiagaraVariable& Parameter)
{
	TArray<UNiagaraStackFunctionInput*> All;
	if (UNiagaraStackViewModel* Stack = ViewModel.GetSystemStackViewModel())
	{
		if (Stack->GetRootEntry() != nullptr)
		{
			Stack->GetRootEntry()->GetUnfilteredChildrenOfType(All, true);
		}
	}
	for (const TSharedRef<FNiagaraEmitterHandleViewModel>& Handle : ViewModel.GetEmitterHandleViewModels())
	{
		UNiagaraStackViewModel* Stack = Handle->GetEmitterStackViewModel();
		if (Stack != nullptr && Stack->GetRootEntry() != nullptr)
		{
			Stack->GetRootEntry()->GetUnfilteredChildrenOfType(All, true);
		}
	}
	TArray<UNiagaraStackFunctionInput*> Out;
	for (UNiagaraStackFunctionInput* Input : All)
	{
		if (Input != nullptr && Input->GetValueMode() == UNiagaraStackFunctionInput::EValueMode::Linked
			&& Input->GetLinkedParameterValue() == Parameter)
		{
			Out.Add(Input);
		}
	}
	return Out;
}

bool Add(Graph::FReader& Op, FUserParameterContext& Context, bool& bOutChanged)
{
	FString Name;
	FString TypeName;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("type")}) || !Op.String(TEXT("name"), Name) || !Op.String(TEXT("type"), TypeName)
		|| !ValidName(Op, Name, TEXT("name")))
	{
		return false;
	}
	if (NameTaken(Context, Name))
	{
		return Op.Fail(TEXT("Niagara.UserParameterExists"), FString::Printf(TEXT("A user parameter named '%s' already exists."), *Name), TEXT("name"));
	}
	// The types the panel's "+" menu offers.
	FNiagaraTypeDefinition Type;
	bool bOffered = false;
	TArray<FString> Offered;
	for (const FNiagaraTypeDefinition& Candidate : FNiagaraTypeRegistry::GetRegisteredUserVariableTypes())
	{
		const FString Token = TypeToken(Candidate);
		Offered.AddUnique(Token);
		if (!bOffered && Token == TypeName)
		{
			Type = Candidate;
			bOffered = true;
		}
	}
	if (!bOffered)
	{
		Offered.Sort();
		return Op.Fail(TEXT("Niagara.UnknownUserParameterType"), FString::Printf(
			TEXT("'%s' is not a user parameter type. Types: %s."), *TypeName, *FString::Join(Offered, TEXT(", "))), TEXT("type"));
	}
	Context.System.Modify();
	FNiagaraVariable Variable(Type, FName(*Name));
	FNiagaraEditorUtilities::ResetVariableToDefaultValue(Variable);
	FNiagaraParameterStore& Store = Context.Store;
	if (!Store.AddParameter(Variable))
	{
		return Op.Fail(TEXT("Niagara.UserParameterExists"), FString::Printf(TEXT("'%s' could not be added."), *Name), TEXT("name"));
	}
	Context.EditorData.Modify();
	Context.EditorData.FindOrAddUserScriptVariable(Variable, Context.System);
	bOutChanged = true;
	return true;
}

bool Delete(Graph::FReader& Op, FUserParameterContext& Context, bool& bOutChanged)
{
	FString Name;
	FNiagaraVariable Variable;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name")}) || !Op.String(TEXT("name"), Name))
	{
		return false;
	}
	if (!FindUserParameter(Context, Name, Variable))
	{
		return Op.Fail(TEXT("Niagara.UserParameterNotFound"), FString::Printf(TEXT("No user parameter '%s'."), *Name), TEXT("name"));
	}
	Context.System.Modify();
	Context.EditorData.Modify();
	UHierarchyRoot* Hierarchy = Context.EditorData.UserParameterHierarchy;
	FGuid Guid;
	if (const UNiagaraScriptVariable* ScriptVariable = Context.EditorData.FindOrAddUserScriptVariable(Variable, Context.System))
	{
		Guid = ScriptVariable->Metadata.GetVariableGuid();
	}
	// Linked inputs keep the parameter's value as a local value (DeleteUserParameterReferences).
	for (UNiagaraStackFunctionInput* Input : LinkedInputs(Context.ViewModel, Variable))
	{
		Input->Reset();
		const FNiagaraTypeDefinition& InputType = Input->GetInputType();
		if (InputType.IsDataInterface())
		{
			const UNiagaraDataInterface* Current = Context.Store.GetDataInterface(Variable);
			if (Current != nullptr && Current->GetClass() == InputType.GetClass())
			{
				Input->SetDataInterfaceValue(Current->GetClass());
			}
		}
		else if (!InputType.IsUObject())
		{
			const UScriptStruct* LocalStruct = InputType.GetScriptStruct();
			const uint8* Memory = Context.Store.GetParameterData(Variable);
			if (LocalStruct != nullptr && Memory != nullptr)
			{
				const TSharedRef<FStructOnScope> Local = MakeShared<FStructOnScope>(LocalStruct);
				FMemory::Memcpy(Local->GetStructMemory(), Memory, LocalStruct->GetStructureSize());
				Input->SetLocalValue(Local);
			}
		}
	}
	FNiagaraParameterStore& Store = Context.Store;
	Store.RemoveParameter(Variable);
	Context.System.HandleVariableRemoved(Variable, true);
	Context.EditorData.RemoveUserScriptVariable(Variable);
	if (Hierarchy != nullptr && Guid.IsValid())
	{
		if (UHierarchyElement* Parent = FindParent(*Hierarchy, ParameterIdentity(Guid)))
		{
			Parent->Modify();
			Parent->RemoveChildWithIdentity(ParameterIdentity(Guid), false);
		}
	}
	bOutChanged = true;
	return true;
}

bool Rename(Graph::FReader& Op, FUserParameterContext& Context, bool& bOutChanged)
{
	FString Name;
	FString To;
	FNiagaraVariable Variable;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("to")}) || !Op.String(TEXT("name"), Name) || !Op.String(TEXT("to"), To)
		|| !ValidName(Op, To, TEXT("to")))
	{
		return false;
	}
	if (!FindUserParameter(Context, Name, Variable))
	{
		return Op.Fail(TEXT("Niagara.UserParameterNotFound"), FString::Printf(TEXT("No user parameter '%s'."), *Name), TEXT("name"));
	}
	if (Name == To)
	{
		return true;
	}
	if (!Name.Equals(To, ESearchCase::IgnoreCase) && NameTaken(Context, To))
	{
		return Op.Fail(TEXT("Niagara.UserParameterExists"), FString::Printf(TEXT("A user parameter named '%s' already exists."), *To), TEXT("to"));
	}
	const FName NewName(*To);
	FNiagaraVariable Renamed = Variable;
	Renamed.SetName(NewName);
	Context.System.Modify();
	Context.EditorData.Modify();
	Context.EditorData.FindOrAddUserScriptVariable(Variable, Context.System);
	Context.EditorData.RenameUserScriptVariable(Variable, NewName);
	Context.Store.RenameParameter(Variable, NewName);
	for (UNiagaraGraph* Graph : Graphs(Context.System))
	{
		if (Graph->GetAllMetaData().Contains(Variable))
		{
			Graph->RenameParameter(Variable, NewName, true);
		}
	}
	Context.System.HandleVariableRenamed(Variable, Renamed, true);
	bOutChanged = true;
	return true;
}

bool SetValue(Graph::FReader& Op, FUserParameterContext& Context, const FNiagaraVariable& Variable, const FValue& Value, bool& bOutChanged)
{
	const FNiagaraTypeDefinition& Type = Variable.GetType();
	if (Type.IsDataInterface())
	{
		const FValue* Spec = Value.Kind == EAssetDocumentRuntimeValueKind::Object ? Graph::FindField(Value, TEXT("DataInterface")) : nullptr;
		UNiagaraDataInterface* DataInterface = Context.Store.GetDataInterface(Variable);
		if (Spec == nullptr || Spec->Kind != EAssetDocumentRuntimeValueKind::Object || DataInterface == nullptr)
		{
			return Op.Fail(TEXT("Niagara.InvalidUserParameterValue"), TEXT(
				"A data interface parameter's value is {\"DataInterface\": {\"Class\": ..., \"Properties\": {...}}}."), TEXT("value"));
		}
		const FValue* ClassValue = Graph::FindField(*Spec, TEXT("Class"));
		const FString Label = ClassLabel(DataInterface->GetClass(), TEXT("NiagaraDataInterface"), TEXT(""));
		if (ClassValue != nullptr && (ClassValue->Kind != EAssetDocumentRuntimeValueKind::String
			|| (ClassValue->StringValue != Label && ClassValue->StringValue != DataInterface->GetClass()->GetPathName())))
		{
			return Op.Fail(TEXT("Niagara.InvalidUserParameterValue"), FString::Printf(
				TEXT("The data interface class is the parameter's type (%s, not %s); change the type by deleting and re-adding the parameter."), *Label,
				ClassValue->Kind == EAssetDocumentRuntimeValueKind::String ? *ClassValue->StringValue : TEXT("a non-string")),
				TEXT("value/DataInterface/Class"));
		}
		// "reset" (op field): data interface properties set back to the class defaults (the extract leaves them out).
		const FValue* Reset = nullptr;
		if (Op.Has(TEXT("reset")) && !Op.Array(TEXT("reset"), Reset))
		{
			return false;
		}
		const UObject* Defaults = DataInterface->GetClass()->GetDefaultObject();
		for (int32 Index = 0; Reset != nullptr && Index < Reset->Elements.Num(); ++Index)
		{
			const FValue& ResetName = Reset->Elements[Index];
			const FString Where = FString::Printf(TEXT("reset/%d"), Index);
			FProperty* Property = ResetName.Kind == EAssetDocumentRuntimeValueKind::String
				? FindEditableProperty(*DataInterface->GetClass(), ResetName.StringValue) : nullptr;
			if (Property == nullptr)
			{
				return Op.Fail(TEXT("Niagara.UnknownProperty"), FString::Printf(TEXT("%s has no editable property '%s'."),
					*DataInterface->GetClass()->GetName(), *ResetName.StringValue), Where);
			}
			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(DataInterface);
			const void* DefaultPtr = Property->ContainerPtrToValuePtr<void>(Defaults);
			if (PropertyMatchesDefault(*Property, ValuePtr, DefaultPtr))
			{
				continue;
			}
			if (Property->ContainsInstancedObjectProperty())
			{
				return Op.Fail(TEXT("Niagara.InvalidPropertyValue"), FString::Printf(TEXT(
					"'%s' holds instanced objects; author its value instead of leaving it out."), *ResetName.StringValue), Where);
			}
			DataInterface->Modify();
			DataInterface->PreEditChange(Property);
			Property->CopyCompleteValue(ValuePtr, DefaultPtr);
			FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
			DataInterface->PostEditChangeProperty(Event);
			bOutChanged = true;
		}
		const FValue* Properties = Graph::FindField(*Spec, TEXT("Properties"));
		if (Properties == nullptr)
		{
			return true;
		}
		if (Properties->Kind != EAssetDocumentRuntimeValueKind::Object)
		{
			return Op.Fail(TEXT("Niagara.InvalidUserParameterValue"), TEXT("Properties is an object."), TEXT("value/DataInterface/Properties"));
		}
		// The Details panel of the data interface.
		for (int32 Index = 0; Index < Properties->ObjectKeys.Num(); ++Index)
		{
			const FString& Key = Properties->ObjectKeys[Index];
			const FString Where = FString(TEXT("value/DataInterface/Properties/")) + Key;
			FProperty* Property = FindEditableProperty(*DataInterface->GetClass(), Key);
			if (Property == nullptr)
			{
				return Op.Fail(TEXT("Niagara.UnknownProperty"), FString::Printf(TEXT("%s has no editable property '%s'."),
					*DataInterface->GetClass()->GetName(), *Key), Where);
			}
			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(DataInterface);
			// Complete at every depth: the value edits the class default's (the read is sparse over it).
			const void* DefaultPtr = Property->ContainerPtrToValuePtr<void>(DataInterface->GetClass()->GetDefaultObject());
			bool bSame = false;
			FString Code;
			if (!DecodePropertyIsSame(*Property, Properties->Elements[Index], ValuePtr, DataInterface, bSame, Code, DefaultPtr))
			{
				return Op.Fail(TEXT("Niagara.InvalidPropertyValue"), FString::Printf(TEXT("'%s' does not fit the property (%s)."), *Key, *Code), Where);
			}
			if (bSame)
			{
				continue;
			}
			DataInterface->Modify();
			DataInterface->PreEditChange(Property);
			DecodeProperty(*Property, Properties->Elements[Index], ValuePtr, DataInterface, Code, DefaultPtr);
			FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
			DataInterface->PostEditChangeProperty(Event);
			bOutChanged = true;
		}
		return true;
	}
	if (Type.IsUObject())
	{
		const FValue* Path = Value.Kind == EAssetDocumentRuntimeValueKind::Object ? Graph::FindField(Value, TEXT("Object")) : nullptr;
		if (Path == nullptr || Path->Kind != EAssetDocumentRuntimeValueKind::String)
		{
			return Op.Fail(TEXT("Niagara.InvalidUserParameterValue"), TEXT("An object parameter's value is {\"Object\": \"<object path>\"} (\"\" for none)."), TEXT("value"));
		}
		UObject* Object = nullptr;
		if (!Path->StringValue.IsEmpty())
		{
			Object = LoadObject<UObject>(nullptr, *Path->StringValue, nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (Object == nullptr || (Type.GetClass() != nullptr && !Object->IsA(Type.GetClass())))
			{
				return Op.Fail(TEXT("Niagara.InvalidUserParameterValue"), FString::Printf(TEXT("'%s' is not a %s."), *Path->StringValue,
					*GetNameSafe(Type.GetClass())), TEXT("value/Object"));
			}
		}
		if (Context.Store.GetUObject(Variable) == Object)
		{
			return true;
		}
		Context.System.Modify();
		Context.Store.SetUObject(Object, Variable);
		bOutChanged = true;
		return true;
	}
	const uint8* Current = Context.Store.GetParameterData(Variable);
	const int32 Size = Type.GetSize();
	if (Current == nullptr || Size <= 0)
	{
		return Op.Fail(TEXT("Niagara.InvalidUserParameterValue"), TEXT("The parameter has no value storage."), TEXT("value"));
	}
	TArray<uint8> Data(Current, Size);
	FString Message;
	if (!DecodeValue(Type, Value, Data.GetData(), Message))
	{
		return Op.Fail(TEXT("Niagara.InvalidUserParameterValue"), Message, TEXT("value"));
	}
	if (FMemory::Memcmp(Data.GetData(), Current, Size) == 0)
	{
		return true;
	}
	Context.System.Modify();
	Context.Store.SetParameterData(Data.GetData(), Variable);
	bOutChanged = true;
	return true;
}

bool SetDescription(FUserParameterContext& Context, const FNiagaraVariable& Variable, const FString& Description, bool& bOutChanged)
{
	Context.EditorData.Modify();
	UNiagaraScriptVariable* ScriptVariable = Context.EditorData.FindOrAddUserScriptVariable(Variable, Context.System);
	if (ScriptVariable == nullptr || ScriptVariable->Metadata.Description.ToString() == Description)
	{
		return true;
	}
	ScriptVariable->Modify();
	ScriptVariable->Metadata.Description = FText::FromString(Description);
	FPropertyChangedEvent Event(UNiagaraScriptVariable::StaticClass()->FindPropertyByName(TEXT("Metadata")), EPropertyChangeType::ValueSet);
	ScriptVariable->PostEditChangeProperty(Event);
	bOutChanged = true;
	return true;
}

bool SetCategory(Graph::FReader& Op, FUserParameterContext& Context, const FNiagaraVariable& Variable, const FString& Category, bool& bOutChanged)
{
	UHierarchyRoot* Root = Context.EditorData.UserParameterHierarchy;
	if (Root == nullptr)
	{
		return Op.Fail(TEXT("Niagara.UserParameterHierarchyUnavailable"), TEXT("The system has no user parameter hierarchy."), TEXT("category"));
	}
	// "" is no category (out of any category).
	TArray<FString> Segments;
	if (!Category.IsEmpty())
	{
		Category.ParseIntoArray(Segments, TEXT("|"), false);
	}
	for (const FString& Segment : Segments)
	{
		if (Segment.IsEmpty() || Segment.TrimStartAndEnd() != Segment)
		{
			return Op.Fail(TEXT("Niagara.InvalidCategory"), FString::Printf(
				TEXT("'%s' is not a category path: names separated by '|', without empty names or surrounding spaces."), *Category), TEXT("category"));
		}
	}
	Context.EditorData.Modify();
	const UNiagaraScriptVariable* ScriptVariable = Context.EditorData.FindOrAddUserScriptVariable(Variable, Context.System);
	if (ScriptVariable == nullptr)
	{
		return Op.Fail(TEXT("Niagara.UserParameterNotFound"), TEXT("The parameter has no user script variable."), TEXT("name"));
	}
	const FHierarchyElementIdentity Identity = ParameterIdentity(ScriptVariable->Metadata.GetVariableGuid());
	TMap<FGuid, FString> Current;
	CollectCategories(*Root, FString(), Current);
	const FString* Was = Current.Find(ScriptVariable->Metadata.GetVariableGuid());
	if ((Was != nullptr ? *Was : FString()) == Category)
	{
		return true;
	}
	// Out of its current category (or the hierarchy root).
	if (UHierarchyElement* Parent = FindParent(*Root, Identity))
	{
		Parent->Modify();
		Parent->RemoveChildWithIdentity(Identity, false);
	}
	bOutChanged = true;
	if (Segments.IsEmpty())
	{
		return true;
	}
	// The category chain, created as the hierarchy editor's "Add Category" creates categories.
	UHierarchyElement* Holder = Root;
	for (const FString& Segment : Segments)
	{
		UHierarchyElement* Found = nullptr;
		for (UHierarchyElement* Child : Holder->GetChildrenMutable())
		{
			const UHierarchyCategory* Existing = Cast<UHierarchyCategory>(Child);
			if (Existing != nullptr && Existing->GetCategoryName().ToString() == Segment)
			{
				Found = Child;
				break;
			}
		}
		if (Found == nullptr)
		{
			Holder->Modify();
			UHierarchyCategory* Created = NewObject<UHierarchyCategory>(Holder, NAME_None, RF_Transactional);
			Created->SetCategoryName(FName(*Segment));
			Created->SetIdentity(UHierarchyCategory::ConstructIdentity());
			Holder->GetChildrenMutable().Add(Created);
			Found = Created;
		}
		Holder = Found;
	}
	// The parameter's element as the hierarchy view model makes it (its source items), copied into the category.
	UDataHierarchyViewModelBase* HierarchyViewModel = Context.ViewModel.GetUserParametersHierarchyViewModel();
	if (HierarchyViewModel == nullptr)
	{
		return Op.Fail(TEXT("Niagara.UserParameterHierarchyUnavailable"), TEXT("The user parameter hierarchy view model is unavailable."), TEXT("category"));
	}
	UHierarchyRoot* Source = NewObject<UHierarchyRoot>(GetTransientPackage(), NAME_None, RF_Transient);
	HierarchyViewModel->PrepareSourceItems(Source, nullptr);
	const UHierarchyElement* Item = Source->FindChildWithIdentity(Identity, true);
	if (Item == nullptr)
	{
		return Op.Fail(TEXT("Niagara.UserParameterHierarchyUnavailable"), TEXT("The parameter has no hierarchy item."), TEXT("category"));
	}
	Holder->Modify();
	Root->CopyAndAddItemUnderParentIdentity(*Item, Holder->GetPersistentIdentity());
	return true;
}

bool Set(Graph::FReader& Op, FUserParameterContext& Context, bool& bOutChanged)
{
	FString Name;
	FNiagaraVariable Variable;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("value"), TEXT("reset"), TEXT("description"), TEXT("category")}) || !Op.String(TEXT("name"), Name))
	{
		return false;
	}
	if (!FindUserParameter(Context, Name, Variable))
	{
		return Op.Fail(TEXT("Niagara.UserParameterNotFound"), FString::Printf(TEXT("No user parameter '%s'."), *Name), TEXT("name"));
	}
	if (const FValue* Value = Op.Find(TEXT("value")))
	{
		if (!SetValue(Op, Context, Variable, *Value, bOutChanged))
		{
			return false;
		}
	}
	FString Description;
	if (Op.Has(TEXT("description")))
	{
		if (!Op.String(TEXT("description"), Description) || !SetDescription(Context, Variable, Description, bOutChanged))
		{
			return false;
		}
	}
	FString Category;
	if (Op.Has(TEXT("category")))
	{
		if (!Op.String(TEXT("category"), Category) || !SetCategory(Op, Context, Variable, Category, bOutChanged))
		{
			return false;
		}
	}
	return true;
}
// ---- declared parameters ----------------------------------------------------------------------------------
// The system Parameters panel's declared (editor-only) parameters: UNiagaraEditorParametersAdapter's
// EditorOnlyScriptVars (FNiagaraSystemToolkitParameterPanelViewModel::AddParameter / DeleteParameters; the adapter
// class is not exported, so its list is reached through reflection on UNiagaraSystem::GetEditorParameters()).

struct FDeclaredList
{
	UObject* Adapter = nullptr;
	FArrayProperty* Array = nullptr;
	FObjectPropertyBase* Inner = nullptr;
};

FDeclaredList DeclaredList(UNiagaraSystem& System)
{
	FDeclaredList List;
	List.Adapter = System.GetEditorParameters();
	List.Array = List.Adapter != nullptr ? CastField<FArrayProperty>(List.Adapter->GetClass()->FindPropertyByName(TEXT("EditorOnlyScriptVars"))) : nullptr;
	List.Inner = List.Array != nullptr ? CastField<FObjectPropertyBase>(List.Array->Inner) : nullptr;
	return List;
}

bool RequireDeclaredList(Graph::FReader& Op, UNiagaraSystem& System, FDeclaredList& OutList)
{
	OutList = DeclaredList(System);
	if (OutList.Inner == nullptr)
	{
		return Op.Fail(TEXT("Niagara.ViewModelUnavailable"), TEXT("The system has no editor parameter list (UNiagaraEditorParametersAdapter::EditorOnlyScriptVars)."));
	}
	return true;
}

UNiagaraScriptVariable* FindDeclared(const FDeclaredList& List, const FString& Name, int32* OutIndex = nullptr)
{
	FScriptArrayHelper Helper(List.Array, List.Array->ContainerPtrToValuePtr<void>(List.Adapter));
	for (int32 Index = 0; Index < Helper.Num(); ++Index)
	{
		UNiagaraScriptVariable* Variable = Cast<UNiagaraScriptVariable>(List.Inner->GetObjectPropertyValue(Helper.GetRawPtr(Index)));
		if (Variable != nullptr && Variable->Variable.GetName().ToString() == Name)
		{
			if (OutIndex != nullptr)
			{
				*OutIndex = Index;
			}
			return Variable;
		}
	}
	return nullptr;
}

bool ValidDeclaredName(Graph::FReader& Op, const FString& Name, const TCHAR* Field)
{
	int32 Dot = INDEX_NONE;
	if (Name.StartsWith(UserPrefix, ESearchCase::CaseSensitive) || !Name.FindChar(TEXT('.'), Dot) || Dot == 0 || Dot + 1 >= Name.Len()
		|| Name.TrimStartAndEnd() != Name || Name.Contains(TEXT("\n")))
	{
		return Op.Fail(TEXT("Niagara.InvalidParameterName"), FString::Printf(TEXT(
			"'%s' is not a declared parameter name: \"<Namespace>.<name>\" (System., Emitter., Particles., ...); user parameters are UserParameters."),
			*Name), Field);
	}
	return true;
}

bool SetDeclaredDescription(UNiagaraScriptVariable& Variable, const FString& Description, bool& bOutChanged)
{
	if (Variable.Metadata.Description.ToString() == Description)
	{
		return true;
	}
	Variable.Modify();
	Variable.Metadata.Description = FText::FromString(Description);
	FPropertyChangedEvent Event(UNiagaraScriptVariable::StaticClass()->FindPropertyByName(TEXT("Metadata")), EPropertyChangeType::ValueSet);
	Variable.PostEditChangeProperty(Event);
	bOutChanged = true;
	return true;
}

// {op: "add_parameter", name, type, description?}
bool AddDeclared(Graph::FReader& Op, FUserParameterContext& Context, bool& bOutChanged)
{
	FString Name;
	FString TypeName;
	FDeclaredList List;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("type"), TEXT("description")}) || !Op.String(TEXT("name"), Name) || !Op.String(TEXT("type"), TypeName)
		|| !ValidDeclaredName(Op, Name, TEXT("name")) || !RequireDeclaredList(Op, Context.System, List))
	{
		return false;
	}
	FNiagaraTypeDefinition Type;
	if (!TypeFromToken(TypeName, Type))
	{
		return Op.Fail(TEXT("Niagara.UnknownParameterType"), FString::Printf(TEXT("'%s' is not a Niagara type."), *TypeName), TEXT("type"));
	}
	if (FindDeclared(List, Name) != nullptr)
	{
		return Op.Fail(TEXT("Niagara.ParameterExists"), FString::Printf(TEXT("The system already declares '%s'."), *Name), TEXT("name"));
	}
	FString Description;
	if (!Op.String(TEXT("description"), Description, false))
	{
		return false;
	}
	Context.System.Modify();
	List.Adapter->Modify();
	UNiagaraScriptVariable* Variable = NewObject<UNiagaraScriptVariable>(List.Adapter, FName(), RF_Transactional);
	Variable->Init(FNiagaraVariable(Type, FName(*Name)), FNiagaraVariableMetaData());
	Variable->SetIsStaticSwitch(false);
	Variable->SetIsSubscribedToParameterDefinitions(false);
	FScriptArrayHelper Helper(List.Array, List.Array->ContainerPtrToValuePtr<void>(List.Adapter));
	const int32 Index = Helper.AddValue();
	List.Inner->SetObjectPropertyValue(Helper.GetRawPtr(Index), Variable);
	bOutChanged = true;
	if (!Description.IsEmpty())
	{
		Variable->Metadata.Description = FText::FromString(Description);
	}
	return true;
}

// {op: "delete_parameter", name}
bool DeleteDeclared(Graph::FReader& Op, FUserParameterContext& Context, bool& bOutChanged)
{
	FString Name;
	FDeclaredList List;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name")}) || !Op.String(TEXT("name"), Name) || !RequireDeclaredList(Op, Context.System, List))
	{
		return false;
	}
	int32 Index = INDEX_NONE;
	UNiagaraScriptVariable* Variable = FindDeclared(List, Name, &Index);
	if (Variable == nullptr)
	{
		return Op.Fail(TEXT("Niagara.ParameterNotFound"), FString::Printf(TEXT("The system declares no '%s'."), *Name), TEXT("name"));
	}
	const FNiagaraVariable Removed = Variable->Variable;
	Context.System.Modify();
	List.Adapter->Modify();
	FScriptArrayHelper Helper(List.Array, List.Array->ContainerPtrToValuePtr<void>(List.Adapter));
	Helper.RemoveValues(Index, 1);
	Context.System.HandleVariableRemoved(Removed, true);
	bOutChanged = true;
	return true;
}

// {op: "parameter", name, description}: the parameter's Details (Metadata.Description).
bool EditDeclared(Graph::FReader& Op, FUserParameterContext& Context, bool& bOutChanged)
{
	FString Name;
	FString Description;
	FDeclaredList List;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("description")}) || !Op.String(TEXT("name"), Name)
		|| !Op.String(TEXT("description"), Description, true) || !RequireDeclaredList(Op, Context.System, List))
	{
		return false;
	}
	UNiagaraScriptVariable* Variable = FindDeclared(List, Name);
	if (Variable == nullptr)
	{
		return Op.Fail(TEXT("Niagara.ParameterNotFound"), FString::Printf(TEXT("The system declares no '%s'."), *Name), TEXT("name"));
	}
	return SetDeclaredDescription(*Variable, Description, bOutChanged);
}
} // namespace

TMap<FName, FString> ReadUserParameterCategories(const UNiagaraSystem& System)
{
	TMap<FName, FString> Out;
	const UNiagaraSystemEditorData* EditorData = Cast<UNiagaraSystemEditorData>(System.GetEditorData());
	if (EditorData == nullptr || EditorData->UserParameterHierarchy == nullptr)
	{
		return Out;
	}
	TMap<FGuid, FString> ByGuid;
	CollectCategories(*EditorData->UserParameterHierarchy, FString(), ByGuid);
	if (ByGuid.IsEmpty())
	{
		return Out;
	}
	for (const UNiagaraScriptVariable* Variable : UserScriptVariables(System))
	{
		if (const FString* Path = ByGuid.Find(Variable->Metadata.GetVariableGuid()))
		{
			Out.Add(Variable->Variable.GetName(), *Path);
		}
	}
	return Out;
}

TArray<const UNiagaraScriptVariable*> ReadDeclaredParameters(UNiagaraSystem& System)
{
	TArray<const UNiagaraScriptVariable*> Out;
	const FDeclaredList List = DeclaredList(System);
	if (List.Inner == nullptr)
	{
		return Out;
	}
	FScriptArrayHelper Helper(List.Array, List.Array->ContainerPtrToValuePtr<void>(List.Adapter));
	for (int32 Index = 0; Index < Helper.Num(); ++Index)
	{
		if (const UNiagaraScriptVariable* Variable = Cast<UNiagaraScriptVariable>(List.Inner->GetObjectPropertyValue(Helper.GetRawPtr(Index))))
		{
			Out.Add(Variable);
		}
	}
	return Out;
}

bool ApplyUserParameterOp(Graph::FReader& Op, const FString& Kind, FNiagaraSystemViewModel& ViewModel, bool& bOutChanged)
{
	UNiagaraSystem& System = ViewModel.GetSystem();
	FUserParameterContext Context{ViewModel, System, ViewModel.GetEditorData(), System.GetExposedParameters()};
	if (Kind == TEXT("add_user_parameter"))
	{
		return Add(Op, Context, bOutChanged);
	}
	if (Kind == TEXT("delete_user_parameter"))
	{
		return Delete(Op, Context, bOutChanged);
	}
	if (Kind == TEXT("rename_user_parameter"))
	{
		return Rename(Op, Context, bOutChanged);
	}
	if (Kind == TEXT("add_parameter"))
	{
		return AddDeclared(Op, Context, bOutChanged);
	}
	if (Kind == TEXT("delete_parameter"))
	{
		return DeleteDeclared(Op, Context, bOutChanged);
	}
	if (Kind == TEXT("parameter"))
	{
		return EditDeclared(Op, Context, bOutChanged);
	}
	return Set(Op, Context, bOutChanged);
}
} // namespace UE::AssetDocument::Niagara
