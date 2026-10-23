// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// data.user_struct.read.v1 / data.user_struct.edit.v1: members of a User Defined Struct as the
// Struct editor edits them (FStructureEditorUtils; the default value as the editor's Default
// Values panel writes it: PropertyValueToString of the edited struct memory, then
// ChangeVariableDefaultValue). A member is addressed by its friendly name (the name the editor
// shows); its identity is its VarGuid, which every edit keeps, so a rename keeps the data of
// every asset that uses the struct. Dependents: AssetDocumentDataUserTypes.h.

#include "AssetDocumentDataUserTypes.h"
#include "AssetDocumentEditorPickerValues.h"

#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/StructureEditorUtils.h"
#include "ScopedTransaction.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/StructOnScope.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"

#define LOCTEXT_NAMESPACE "AssetDocumentUserStruct"

namespace UE::AssetDocument::Graph::Data
{
namespace
{
const FName ValueRangeKeys[] = {TEXT("ClampMin"), TEXT("ClampMax"), TEXT("UIMin"), TEXT("UIMax")};

UUserDefinedStruct* GetStruct(FCall& Call)
{
	UUserDefinedStruct* Struct = Cast<UUserDefinedStruct>(&Call.Asset);
	if (Struct == nullptr || Cast<UUserDefinedStructEditorData>(Struct->EditorData) == nullptr)
	{
		Fail(Call, TEXT("Data.UnsupportedAsset"), TEXT("UserDefinedStruct primitives edit User Defined Struct assets (with editor data)."),
			TEXT("/HandleArguments/asset"));
		return nullptr;
	}
	return Struct;
}

FStructVariableDescription* FindMember(UUserDefinedStruct& Struct, const FString& Name)
{
	return FStructureEditorUtils::GetVarDesc(&Struct).FindByPredicate(
		[&Name](const FStructVariableDescription& Var) { return Var.FriendlyName.Equals(Name, ESearchCase::CaseSensitive); });
}

FString StatusText(const UUserDefinedStruct& Struct)
{
	switch (Struct.Status.GetValue())
	{
	case EUserDefinedStructureStatus::UDSS_UpToDate: return TEXT("up_to_date");
	case EUserDefinedStructureStatus::UDSS_Dirty: return TEXT("dirty");
	case EUserDefinedStructureStatus::UDSS_Error: return TEXT("error");
	default: return TEXT("other");
	}
}

// The types the Struct editor's type picker offers for a member (no exec / wildcard /
// delegates; references and const are pin-only).
bool CheckTerminal(FName Category, FName SubCategory, const UObject* Object, FString& OutWhy)
{
	static const TSet<FName> Plain = {UEdGraphSchema_K2::PC_Boolean, UEdGraphSchema_K2::PC_Int, UEdGraphSchema_K2::PC_Int64,
		UEdGraphSchema_K2::PC_Name, UEdGraphSchema_K2::PC_String, UEdGraphSchema_K2::PC_Text};
	static const TSet<FName> Classes = {UEdGraphSchema_K2::PC_Object, UEdGraphSchema_K2::PC_Class, UEdGraphSchema_K2::PC_SoftObject,
		UEdGraphSchema_K2::PC_SoftClass, UEdGraphSchema_K2::PC_Interface};
	if (Plain.Contains(Category))
	{
		if (Object != nullptr || !SubCategory.IsNone())
		{
			OutWhy = FString::Printf(TEXT("'%s' takes no sub category or object."), *Category.ToString());
			return false;
		}
		return true;
	}
	if (Category == UEdGraphSchema_K2::PC_Real)
	{
		if (SubCategory != UEdGraphSchema_K2::PC_Float && SubCategory != UEdGraphSchema_K2::PC_Double)
		{
			OutWhy = TEXT("'real' needs sub_category float or double (shorthand \"float\" / \"double\").");
			return false;
		}
		return true;
	}
	if (Category == UEdGraphSchema_K2::PC_Byte || Category == UEdGraphSchema_K2::PC_Enum)
	{
		if (Object != nullptr && !Object->IsA<UEnum>())
		{
			OutWhy = FString::Printf(TEXT("'%s' is not an enum."), *Object->GetPathName());
			return false;
		}
		if (Category == UEdGraphSchema_K2::PC_Enum && Object == nullptr)
		{
			OutWhy = TEXT("'enum' needs the enum object (shorthand \"byte:/Game/E_Kind.E_Kind\").");
			return false;
		}
		return true;
	}
	if (Category == UEdGraphSchema_K2::PC_Struct)
	{
		if (Cast<UScriptStruct>(Object) == nullptr)
		{
			OutWhy = TEXT("'struct' needs a struct object, e.g. \"struct:/Script/CoreUObject.Vector\".");
			return false;
		}
		return true;
	}
	if (Classes.Contains(Category))
	{
		if (Cast<UClass>(Object) == nullptr)
		{
			OutWhy = FString::Printf(TEXT("'%s' needs a class object, e.g. \"%s:/Script/Engine.Actor\"."), *Category.ToString(), *Category.ToString());
			return false;
		}
		return true;
	}
	OutWhy = FString::Printf(TEXT("'%s' is not a struct member type (bool, byte, int, int64, real/float/double, name, string, text, "
		"struct, enum, object, class, softobject, softclass, interface; containers array, set, map)."), *Category.ToString());
	return false;
}

bool ReadMemberType(FReader& Op, UUserDefinedStruct& Struct, FEdGraphPinType& OutType)
{
	const FValue* TypeValue = nullptr;
	if (!Op.Object(TEXT("type"), TypeValue))
	{
		return false;
	}
	FReader TypeReader = Op.Child(TEXT("type"), *TypeValue);
	if (!ReadPinType(TypeReader, OutType))
	{
		return false;
	}
	FString Why;
	if (OutType.bIsReference || OutType.bIsConst)
	{
		Why = TEXT("A struct member is not a reference or const.");
	}
	else if (CheckTerminal(OutType.PinCategory, OutType.PinSubCategory, OutType.PinSubCategoryObject.Get(), Why)
		&& OutType.ContainerType == EPinContainerType::Map)
	{
		const FEdGraphTerminalType& Value = OutType.PinValueType;
		if (CheckTerminal(Value.TerminalCategory, Value.TerminalSubCategory, Value.TerminalSubCategoryObject.Get(), Why))
		{
			Why.Reset();
		}
		else
		{
			Why = TEXT("Map value: ") + Why;
		}
	}
	if (Why.IsEmpty() && !FStructureEditorUtils::CanHaveAMemberVariableOfType(&Struct, OutType, &Why))
	{
		Why = Why.IsEmpty() ? FString(TEXT("The Struct editor does not accept this member type.")) : Why;
	}
	return Why.IsEmpty() || Op.Fail(TEXT("UserDefinedStruct.InvalidMemberType"), Why, TEXT("type"));
}

bool ReadNewName(FReader& Op, const TCHAR* Key, FString& Out)
{
	if (!Op.String(Key, Out))
	{
		return false;
	}
	if (Out.IsEmpty() || Out.TrimStartAndEnd() != Out || Out.Len() > 100 || Out.Contains(TEXT("\n")) || Out.Contains(TEXT("\r")))
	{
		return Op.Fail(TEXT("UserDefinedStruct.InvalidMemberName"),
			FString::Printf(TEXT("'%s' is not a member name: use 1 to 100 characters on one line without leading or trailing spaces."), *Out), Key);
	}
	return true;
}

FStructVariableDescription* RequireMember(FReader& Op, UUserDefinedStruct& Struct, const FString& Name, const TCHAR* Key = TEXT("name"))
{
	FStructVariableDescription* Var = FindMember(Struct, Name);
	if (Var == nullptr)
	{
		TArray<FString> Names;
		for (const FStructVariableDescription& Each : FStructureEditorUtils::GetVarDesc(&Struct))
		{
			Names.Add(Each.FriendlyName);
		}
		Op.Fail(TEXT("UserDefinedStruct.MemberNotFound"), FString::Printf(TEXT("The struct has no member '%s' (members: %s)."),
			*Name, *FString::Join(Names, TEXT(", "))), Key);
	}
	return Var;
}

// Renames as the editor does; a case-only rename (the editor compares names ignoring case)
// takes an intermediate unique name.
bool RenameMember(FReader& Op, UUserDefinedStruct& Struct, const FGuid Guid, const FString& Current, const FString& To, const TCHAR* Key)
{
	if (Current.Equals(To, ESearchCase::CaseSensitive))
	{
		return true;
	}
	if (!Current.Equals(To, ESearchCase::IgnoreCase) && !FStructureEditorUtils::IsUniqueVariableFriendlyName(&Struct, To))
	{
		return Op.Fail(TEXT("UserDefinedStruct.MemberExists"), FString::Printf(
			TEXT("The struct already has a member '%s' (member names are unique ignoring case)."), *To), Key);
	}
	if (Current.Equals(To, ESearchCase::IgnoreCase))
	{
		FString Temporary;
		for (int32 Suffix = 0; Temporary.IsEmpty() || !FStructureEditorUtils::IsUniqueVariableFriendlyName(&Struct, Temporary); ++Suffix)
		{
			Temporary = FString::Printf(TEXT("%s_Rename%d"), *To, Suffix);
		}
		FStructureEditorUtils::RenameVariable(&Struct, Guid, Temporary);
	}
	if (!FStructureEditorUtils::RenameVariable(&Struct, Guid, To))
	{
		return Op.Fail(TEXT("UserDefinedStruct.InvalidMemberName"), FString::Printf(TEXT("The Struct editor refused the name '%s'."), *To), Key);
	}
	return true;
}

bool SetFlag(FReader& Op, const TCHAR* Key, bool bCurrent, bool bAllowed, const TCHAR* NotAllowedCode, const TCHAR* NotAllowedWhy,
	TFunctionRef<bool(bool)> Change)
{
	bool bValue = false;
	if (!Op.Has(Key))
	{
		return true;
	}
	if (!Op.Bool(Key, bValue))
	{
		return false;
	}
	if (bValue == bCurrent)
	{
		return true;
	}
	if (!bAllowed)
	{
		return Op.Fail(NotAllowedCode, NotAllowedWhy, Key);
	}
	return Change(bValue) || Op.Fail(TEXT("UserDefinedStruct.InvalidMember"), FString::Printf(TEXT("The Struct editor did not change %s."), Key), Key);
}

bool SetMember(FReader& Op, UUserDefinedStruct& Struct, FStructVariableDescription& Var)
{
	const FGuid Guid = Var.VarGuid;
	FString Tooltip;
	if (Op.Has(TEXT("tooltip")))
	{
		if (!Op.String(TEXT("tooltip"), Tooltip))
		{
			return false;
		}
		FStructureEditorUtils::ChangeVariableTooltip(&Struct, Guid, Tooltip);
	}
	const FStructVariableDescription* Current = FStructureEditorUtils::GetVarDescByGuid(&Struct, Guid);
	if (!SetFlag(Op, TEXT("instance_editable"), !Current->bDontEditOnInstance, true, TEXT(""), TEXT(""),
			[&](bool bValue) { return FStructureEditorUtils::ChangeEditableOnBPInstance(&Struct, Guid, bValue); })
		|| !SetFlag(Op, TEXT("save_game"), FStructureEditorUtils::GetVarDescByGuid(&Struct, Guid)->bEnableSaveGame, true, TEXT(""), TEXT(""),
			[&](bool bValue) { return FStructureEditorUtils::ChangeSaveGameEnabled(&Struct, Guid, bValue); })
		|| !SetFlag(Op, TEXT("multi_line"), FStructureEditorUtils::IsMultiLineTextEnabled(&Struct, Guid),
			FStructureEditorUtils::CanEnableMultiLineText(&Struct, Guid), TEXT("UserDefinedStruct.MultiLineNotAllowed"),
			TEXT("Multi-line editing applies to string and text members (and arrays of them) only."),
			[&](bool bValue) { return FStructureEditorUtils::ChangeMultiLineTextEnabled(&Struct, Guid, bValue); })
		|| !SetFlag(Op, TEXT("widget_3d"), FStructureEditorUtils::Is3dWidgetEnabled(&Struct, Guid),
			FStructureEditorUtils::CanEnable3dWidget(&Struct, Guid), TEXT("UserDefinedStruct.Widget3DNotAllowed"),
			TEXT("The 3D widget applies to Vector and Transform members only."),
			[&](bool bValue) { return FStructureEditorUtils::Change3dWidgetEnabled(&Struct, Guid, bValue); }))
	{
		return false;
	}
	const FValue* MetaData = nullptr;
	if (Op.Has(TEXT("metadata")))
	{
		if (!Op.Object(TEXT("metadata"), MetaData))
		{
			return false;
		}
		FReader Meta = Op.Child(TEXT("metadata"), *MetaData);
		if (!Meta.OnlyFields({TEXT("ClampMin"), TEXT("ClampMax"), TEXT("UIMin"), TEXT("UIMax")}))
		{
			return false;
		}
		for (const FName& Key : ValueRangeKeys)
		{
			const FString KeyText = Key.ToString();
			FString Value;
			if (!Meta.Has(KeyText))
			{
				continue;
			}
			if (!Meta.String(KeyText, Value))
			{
				return false;
			}
			const FString* Existing = FStructureEditorUtils::GetMetaData(&Struct, Guid, Key);
			if ((Existing != nullptr ? *Existing : FString()).Equals(Value, ESearchCase::CaseSensitive))
			{
				continue;
			}
			if (!Value.IsEmpty() && !FStructureEditorUtils::CanEditValueRange(&Struct, Guid))
			{
				return Meta.Fail(TEXT("UserDefinedStruct.ValueRangeNotAllowed"), TEXT("Value ranges apply to numeric members only."), KeyText);
			}
			if (!Value.IsEmpty() && !Value.IsNumeric())
			{
				return Meta.Fail(TEXT("UserDefinedStruct.InvalidMember"), FString::Printf(TEXT("%s must be a number (got '%s')."), *KeyText, *Value), KeyText);
			}
			FStructureEditorUtils::SetMetaData(&Struct, Guid, Key, Value);
		}
	}
	return true;
}

bool SetDefault(FReader& Op, UUserDefinedStruct& Struct, FStructVariableDescription& Var)
{
	const FGuid Guid = Var.VarGuid;
	const FValue* Value = Op.Find(TEXT("value"));
	if (Value == nullptr)
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("value is required (null resets the member to its type's default)."), TEXT("value"));
	}
	if (Value->Kind == EAssetDocumentRuntimeValueKind::Null)
	{
		if (!Var.DefaultValue.IsEmpty())
		{
			FStructureEditorUtils::ChangeVariableDefaultValue(&Struct, Guid, FString());
		}
		return true;
	}
	FProperty* Property = FStructureEditorUtils::GetPropertyByGuid(&Struct, Guid);
	const uint8* Defaults = Struct.GetDefaultInstance();
	if (Property == nullptr || Defaults == nullptr || GetMemberCodecId(*Property).IsEmpty())
	{
		return Op.Fail(TEXT("UserDefinedStruct.UnsupportedDefault"), FString::Printf(
			TEXT("The default value of '%s' (%s) is not representable by the value codec; leave DefaultValue out."),
			*Var.FriendlyName, Property != nullptr ? *Property->GetCPPType() : TEXT("not compiled")), TEXT("value"));
	}
	FStructOnScope Memory(&Struct);
	FString Code;
	if (!DecodeMember(*Property, *Value, Memory.GetStructMemory(), Code, &Struct))
	{
		return Op.Fail(TEXT("UserDefinedStruct.InvalidDefault"), FString::Printf(TEXT("Not a %s value for '%s' (%s)."),
			*Property->GetCPPType(), *Var.FriendlyName, *Code), TEXT("value"));
	}
	// A default naming a table row or a key that does not exist (the Struct editor's pickers list only existing ones).
	UE::AssetDocument::EditorPickerValues::FInvalidPick Invalid;
	if (UE::AssetDocument::EditorPickerValues::FindInvalidPick(*Property, Property->ContainerPtrToValuePtr<void>(Memory.GetStructMemory()),
		Property->ContainerPtrToValuePtr<void>(Defaults), Invalid))
	{
		return Op.Fail(Invalid.Code, FString::Printf(TEXT("Default of '%s'%s: %s"),
			*Var.FriendlyName, *Invalid.ValuePath, *Invalid.Message), TEXT("value"));
	}
	if (Property->Identical_InContainer(Memory.GetStructMemory(), Defaults))
	{
		return true;
	}
	FString Text;
	if (!FBlueprintEditorUtils::PropertyValueToString(Property, Memory.GetStructMemory(), Text, &Struct)
		|| !FStructureEditorUtils::ChangeVariableDefaultValue(&Struct, Guid, Text))
	{
		return Op.Fail(TEXT("UserDefinedStruct.InvalidDefault"), FString::Printf(TEXT("The Struct editor refused the default '%s' for '%s'."),
			*Text, *Var.FriendlyName), TEXT("value"));
	}
	return true;
}

bool Reorder(FReader& Op, UUserDefinedStruct& Struct)
{
	const FValue* Names = nullptr;
	if (!Op.Array(TEXT("names"), Names))
	{
		return false;
	}
	TArray<FStructVariableDescription>& Vars = FStructureEditorUtils::GetVarDesc(&Struct);
	TArray<FGuid> Order;
	for (int32 Index = 0; Index < Names->Elements.Num(); ++Index)
	{
		const FValue& Name = Names->Elements[Index];
		const FStructVariableDescription* Var = Name.Kind == EAssetDocumentRuntimeValueKind::String ? FindMember(Struct, Name.StringValue) : nullptr;
		if (Var == nullptr || Order.Contains(Var->VarGuid))
		{
			return Op.Fail(TEXT("UserDefinedStruct.InvalidOrder"), TEXT("names lists every member once, by name."), FString::Printf(TEXT("names/%d"), Index));
		}
		Order.Add(Var->VarGuid);
	}
	if (Order.Num() != Vars.Num())
	{
		return Op.Fail(TEXT("UserDefinedStruct.InvalidOrder"), TEXT("names lists every member once, by name."), TEXT("names"));
	}
	for (int32 Index = 0; Index < Order.Num(); ++Index)
	{
		TArray<FStructVariableDescription>& Current = FStructureEditorUtils::GetVarDesc(&Struct);
		if (Current[Index].VarGuid != Order[Index])
		{
			FStructureEditorUtils::MoveVariable(&Struct, Order[Index], Current[Index].VarGuid, FStructureEditorUtils::PositionAbove);
		}
	}
	return true;
}

bool StructOpUpdatesDependents(const FValue& Op)
{
	const FValue* Kind = FindField(Op, TEXT("op"));
	if (Kind == nullptr || Kind->Kind != EAssetDocumentRuntimeValueKind::String || Kind->StringValue == TEXT("tooltip"))
	{
		return false;
	}
	if (Kind->StringValue == TEXT("set"))
	{
		// Tooltip and 3D widget edits do not recompile the struct.
		for (const FString& Key : Op.ObjectKeys)
		{
			if (Key != TEXT("op") && Key != TEXT("name") && Key != TEXT("tooltip") && Key != TEXT("widget_3d"))
			{
				return true;
			}
		}
		return false;
	}
	return true;
}
bool EditMembers(FCall& Call, UUserDefinedStruct& Struct, const FValue& Ops);
} // namespace

bool HandleUserStructRead(FCall& Call, FObjectBuilder& Result)
{
	UUserDefinedStruct* Struct = GetStruct(Call);
	bool bDependents = false;
	if (Struct == nullptr || !Call.Args.OnlyFields({TEXT("dependents")}) || !Call.Args.Bool(TEXT("dependents"), bDependents))
	{
		return false;
	}
	Result.AddString(TEXT("status"), StatusText(*Struct));
	Result.AddString(TEXT("error_message"), Struct->Status == EUserDefinedStructureStatus::UDSS_Error ? Struct->ErrorMessage : FString());
	Result.AddString(TEXT("tooltip"), FStructureEditorUtils::GetTooltip(Struct));
	const uint8* Defaults = Struct->GetDefaultInstance();
	TArray<FValue> Members;
	for (const FStructVariableDescription& Var : FStructureEditorUtils::GetVarDesc(Struct))
	{
		const FGuid Guid = Var.VarGuid;
		FObjectBuilder Row;
		Row.AddString(TEXT("guid"), Guid.ToString(EGuidFormats::DigitsWithHyphens));
		Row.AddString(TEXT("name"), Var.FriendlyName);
		Row.Add(TEXT("type"), MakePinTypeValue(Var.ToPinType()));
		Row.AddString(TEXT("tooltip"), Var.ToolTip);
		Row.AddBool(TEXT("instance_editable"), !Var.bDontEditOnInstance);
		Row.AddBool(TEXT("save_game"), Var.bEnableSaveGame);
		Row.AddBool(TEXT("multi_line"), FStructureEditorUtils::IsMultiLineTextEnabled(Struct, Guid));
		Row.AddBool(TEXT("multi_line_allowed"), FStructureEditorUtils::CanEnableMultiLineText(Struct, Guid));
		Row.AddBool(TEXT("widget_3d"), FStructureEditorUtils::Is3dWidgetEnabled(Struct, Guid));
		Row.AddBool(TEXT("widget_3d_allowed"), FStructureEditorUtils::CanEnable3dWidget(Struct, Guid));
		Row.AddBool(TEXT("value_range_allowed"), FStructureEditorUtils::CanEditValueRange(Struct, Guid));
		Row.AddBool(TEXT("invalid"), Var.bInvalidMember);
		FObjectBuilder Meta;
		for (const FName& Key : ValueRangeKeys)
		{
			if (const FString* Value = Var.MetaData.Find(Key); Value != nullptr && !Value->IsEmpty())
			{
				Meta.AddString(Key.ToString(), *Value);
			}
		}
		Row.Add(TEXT("metadata"), Meta.Build());
		FProperty* Property = FStructureEditorUtils::GetPropertyByGuid(Struct, Guid);
		const FString CodecId = Property != nullptr ? GetMemberCodecId(*Property) : FString();
		Row.AddString(TEXT("cpp_type"), Property != nullptr ? Property->GetCPPType() : FString());
		FValue Default = MakeNull();
		FString Code;
		bool bSupported = !CodecId.IsEmpty() && Defaults != nullptr;
		if (bSupported && !EncodeMember(*Property, Defaults, Default, Code))
		{
			Default = MakeNull();
			bSupported = false;
		}
		Row.AddString(TEXT("codec"), CodecId);
		Row.AddBool(TEXT("supported"), bSupported);
		Row.Add(TEXT("default"), MoveTemp(Default));
		Members.Add(Row.Build());
	}
	Result.Add(TEXT("members"), MakeArray(MoveTemp(Members)));
	if (bDependents)
	{
		Result.Add(TEXT("dependents"), UserTypes::DescribeDependents(UserTypes::FindLoadedDependents(*Struct)));
	}
	return true;
}

bool HandleUserStructEdit(FCall& Call, FObjectBuilder& Result)
{
	UUserDefinedStruct* Struct = GetStruct(Call);
	const FValue* Ops = nullptr;
	if (Struct == nullptr || !Call.Args.OnlyFields({TEXT("ops")}) || !Call.Args.Array(TEXT("ops"), Ops))
	{
		return false;
	}
	const bool bUpdatesDependents = Ops->Elements.ContainsByPredicate(&StructOpUpdatesDependents);
	UserTypes::FDependentUpdate Dependents(*Struct, bUpdatesDependents ? UserTypes::FindLoadedDependents(*Struct) : TArray<UserTypes::FDependent>());
	if (!Dependents.Admit(Call.Args, TEXT("UserDefinedStruct.DependentHasUnsavedChanges")))
	{
		return false;
	}
	const bool bWasUpToDate = Struct->Status == EUserDefinedStructureStatus::UDSS_UpToDate;
	if (!EditMembers(Call, *Struct, *Ops))
	{
		// A type the pin-type reader refuses (e.g. an object path that does not resolve) is a member type error.
		if (Call.Error.Code.StartsWith(TEXT("Graph.")) && Call.Error.Path.Contains(TEXT("/type")))
		{
			Call.Error.Code = TEXT("UserDefinedStruct.InvalidMemberType");
		}
		return false;
	}
	if (bWasUpToDate && Struct->Status != EUserDefinedStructureStatus::UDSS_UpToDate)
	{
		return Fail(Call, TEXT("UserDefinedStruct.CompileFailed"), FString::Printf(TEXT("The struct did not compile after the edit: %s"),
			*Struct->ErrorMessage), TEXT("/ops"));
	}
	Result.AddInt(TEXT("members"), FStructureEditorUtils::GetVarDesc(Struct).Num());
	Result.Add(TEXT("dependents"), Dependents.Finish());
	return true;
}

namespace
{
bool EditMembers(FCall& Call, UUserDefinedStruct& StructRef, const FValue& OpsRef)
{
	UUserDefinedStruct* Struct = &StructRef;
	const FValue* Ops = &OpsRef;
	{
		// One editor transaction for the call: the Struct editor's per-edit transactions nest into it.
		const FScopedTransaction CallTransaction(LOCTEXT("EditStruct", "Edit Structure"));
		for (int32 Index = 0; Index < Ops->Elements.Num(); ++Index)
		{
			FReader Op = Call.Args.Child(FString::Printf(TEXT("ops/%d"), Index), Ops->Elements[Index]);
			FString Kind;
			if (!Op.IsObject())
			{
				return Op.Fail(TEXT("Data.InvalidOp"), TEXT("Each op is an object."));
			}
			if (!Op.String(TEXT("op"), Kind))
			{
				return false;
			}
			FString Name;
			if (Kind == TEXT("add"))
			{
				FEdGraphPinType Type;
				if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("type")}) || !ReadNewName(Op, TEXT("name"), Name) || !ReadMemberType(Op, *Struct, Type))
				{
					return false;
				}
				if (!FStructureEditorUtils::IsUniqueVariableFriendlyName(Struct, Name))
				{
					return Op.Fail(TEXT("UserDefinedStruct.MemberExists"), FString::Printf(
						TEXT("The struct already has a member '%s' (member names are unique ignoring case)."), *Name), TEXT("name"));
				}
				if (!FStructureEditorUtils::AddVariable(Struct, Type))
				{
					return Op.Fail(TEXT("UserDefinedStruct.InvalidMemberType"), TEXT("The Struct editor refused the member type."), TEXT("type"));
				}
				const FStructVariableDescription Added = FStructureEditorUtils::GetVarDesc(Struct).Last();
				if (!RenameMember(Op, *Struct, Added.VarGuid, Added.FriendlyName, Name, TEXT("name")))
				{
					return false;
				}
				continue;
			}
			if (Kind == TEXT("tooltip"))
			{
				FString Tooltip;
				if (!Op.OnlyFields({TEXT("op"), TEXT("tooltip")}) || !Op.String(TEXT("tooltip"), Tooltip))
				{
					return false;
				}
				FStructureEditorUtils::ChangeTooltip(Struct, Tooltip);
				continue;
			}
			if (Kind == TEXT("order"))
			{
				if (!Op.OnlyFields({TEXT("op"), TEXT("names")}) || !Reorder(Op, *Struct))
				{
					return false;
				}
				continue;
			}
			if (!Op.String(TEXT("name"), Name))
			{
				return false;
			}
			FStructVariableDescription* Var = RequireMember(Op, *Struct, Name);
			if (Var == nullptr)
			{
				return false;
			}
			const FGuid Guid = Var->VarGuid;
			if (Kind == TEXT("rename"))
			{
				FString To;
				if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("to")}) || !ReadNewName(Op, TEXT("to"), To)
					|| !RenameMember(Op, *Struct, Guid, Var->FriendlyName, To, TEXT("to")))
				{
					return false;
				}
			}
			else if (Kind == TEXT("retype"))
			{
				FEdGraphPinType Type;
				if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("type")}) || !ReadMemberType(Op, *Struct, Type))
				{
					return false;
				}
				if (!(Var->ToPinType() == Type) && !FStructureEditorUtils::ChangeVariableType(Struct, Guid, Type))
				{
					return Op.Fail(TEXT("UserDefinedStruct.InvalidMemberType"), TEXT("The Struct editor refused the member type."), TEXT("type"));
				}
			}
			else if (Kind == TEXT("remove"))
			{
				if (!Op.OnlyFields({TEXT("op"), TEXT("name")}))
				{
					return false;
				}
				if (FStructureEditorUtils::GetVarDesc(Struct).Num() <= 1)
				{
					return Op.Fail(TEXT("UserDefinedStruct.LastMember"), TEXT("A User Defined Struct keeps at least one member (the Struct editor "
						"cannot remove the last one); add the new member first."), TEXT("name"));
				}
				FStructureEditorUtils::RemoveVariable(Struct, Guid);
			}
			else if (Kind == TEXT("set"))
			{
				if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("tooltip"), TEXT("instance_editable"), TEXT("save_game"),
						TEXT("multi_line"), TEXT("widget_3d"), TEXT("metadata")})
					|| !SetMember(Op, *Struct, *Var))
				{
					return false;
				}
			}
			else if (Kind == TEXT("default"))
			{
				if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("value")}) || !SetDefault(Op, *Struct, *Var))
				{
					return false;
				}
			}
			else
			{
				return Op.Fail(TEXT("Data.InvalidOp"), FString::Printf(
					TEXT("Unknown op '%s' (add, rename, retype, remove, order, set, default, tooltip)."), *Kind), TEXT("op"));
			}
		}
	}
	return true;
}
} // namespace
} // namespace UE::AssetDocument::Graph::Data

#undef LOCTEXT_NAMESPACE
