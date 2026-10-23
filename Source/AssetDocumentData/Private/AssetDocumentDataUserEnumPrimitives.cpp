// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// data.user_enum.read.v1 / data.user_enum.edit.v1: enumerators of a User Defined Enum as the
// Enum editor edits them (FEnumEditorUtils: add / remove / move / display name / bitflags; the
// enumerator description as the editor's Description box writes it, Modify + SetMetaData
// "ToolTip"; the enum description as the Details panel writes EnumDescription). An enumerator
// is addressed by its display name (the name the editor, Blueprints and data tables show); the
// internal names (NewEnumerator0, ...) never change, so a display-name rename keeps every
// stored value. Dependents: AssetDocumentDataUserTypes.h.

#include "AssetDocumentDataUserTypes.h"

#include "Engine/UserDefinedEnum.h"
#include "Internationalization/TextNamespaceUtil.h"
#include "Internationalization/TextPackageNamespaceUtil.h"
#include "Kismet2/EnumEditorUtils.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "AssetDocumentUserEnum"

namespace UE::AssetDocument::Graph::Data
{
namespace
{
const TCHAR* ToolTipKey = TEXT("ToolTip");

UUserDefinedEnum* GetEnum(FCall& Call)
{
	UUserDefinedEnum* Enum = Cast<UUserDefinedEnum>(&Call.Asset);
	if (Enum == nullptr)
	{
		Fail(Call, TEXT("Data.UnsupportedAsset"), TEXT("UserDefinedEnum primitives edit User Defined Enum assets."), TEXT("/HandleArguments/asset"));
	}
	return Enum;
}

// Enumerators without the implicit _MAX entry.
int32 NumEnumerators(const UEnum& Enum)
{
	return FMath::Max(Enum.NumEnums() - 1, 0);
}

FString DisplayName(const UEnum& Enum, int32 Index)
{
	return Enum.GetDisplayNameTextByIndex(Index).ToString();
}

int32 FindEnumerator(const UEnum& Enum, const FString& Name)
{
	for (int32 Index = 0; Index < NumEnumerators(Enum); ++Index)
	{
		if (DisplayName(Enum, Index).Equals(Name, ESearchCase::CaseSensitive))
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

int32 RequireEnumerator(FReader& Op, const UEnum& Enum, const FString& Name, const TCHAR* Key = TEXT("name"))
{
	const int32 Index = FindEnumerator(Enum, Name);
	if (Index == INDEX_NONE)
	{
		TArray<FString> Names;
		for (int32 Each = 0; Each < NumEnumerators(Enum); ++Each)
		{
			Names.Add(DisplayName(Enum, Each));
		}
		Op.Fail(TEXT("UserDefinedEnum.EnumeratorNotFound"), FString::Printf(TEXT("The enum has no enumerator '%s' (enumerators: %s)."),
			*Name, *FString::Join(Names, TEXT(", "))), Key);
	}
	return Index;
}

bool CheckDisplayName(FReader& Op, const UUserDefinedEnum& Enum, int32 Index, const FString& Name, const TCHAR* Key)
{
	if (Name.IsEmpty() || Name.TrimStartAndEnd() != Name || Name.Contains(TEXT("\n")) || Name.Contains(TEXT("\r")))
	{
		return Op.Fail(TEXT("UserDefinedEnum.InvalidEnumeratorName"), FString::Printf(
			TEXT("'%s' is not an enumerator name: use a non-empty single line without leading or trailing spaces."), *Name), Key);
	}
	for (int32 Other = 0; Other < NumEnumerators(Enum); ++Other)
	{
		if (Other != Index && DisplayName(Enum, Other).Equals(Name, ESearchCase::IgnoreCase))
		{
			return Op.Fail(TEXT("UserDefinedEnum.EnumeratorExists"), FString::Printf(
				TEXT("The enum already has an enumerator '%s' (display names are unique ignoring case)."), *Name), Key);
		}
	}
	if (!FEnumEditorUtils::IsEnumeratorDisplayNameValid(&Enum, Index, FText::FromString(Name)))
	{
		return Op.Fail(TEXT("UserDefinedEnum.InvalidEnumeratorName"), FString::Printf(TEXT("The Enum editor refuses the name '%s'."), *Name), Key);
	}
	return true;
}

bool SetDisplayName(FReader& Op, UUserDefinedEnum& Enum, int32 Index, const FString& Name, const TCHAR* Key)
{
	if (DisplayName(Enum, Index).Equals(Name, ESearchCase::CaseSensitive))
	{
		return true;
	}
	if (!CheckDisplayName(Op, Enum, Index, Name, Key))
	{
		return false;
	}
	return FEnumEditorUtils::SetEnumeratorDisplayName(&Enum, Index, FText::FromString(Name))
		|| Op.Fail(TEXT("UserDefinedEnum.InvalidEnumeratorName"), FString::Printf(TEXT("The Enum editor refused the name '%s'."), *Name), Key);
}

// The Details panel edit of EnumDescription (UUserDefinedEnum::PostEditChangeProperty copies it
// to the enum's ToolTip metadata), with a package-namespaced localization key as the editor's
// text box gives it.
void SetDescription(UUserDefinedEnum& Enum, const FString& Value)
{
	FProperty* Property = FindFProperty<FProperty>(UUserDefinedEnum::StaticClass(), GET_MEMBER_NAME_CHECKED(UUserDefinedEnum, EnumDescription));
	FText Text = FText::FromString(Value);
#if USE_STABLE_LOCALIZATION_KEYS
	const FString PackageNamespace = TextNamespaceUtil::EnsurePackageNamespace(&Enum);
	if (!Value.IsEmpty() && !PackageNamespace.IsEmpty())
	{
		Text = FText::ChangeKey(TextNamespaceUtil::BuildFullNamespace(FString(), PackageNamespace), FGuid::NewGuid().ToString(), Text);
	}
#endif
	const FScopedTransaction Transaction(LOCTEXT("SetDescription", "Set Enum Description"));
	Enum.Modify();
	Enum.PreEditChange(Property);
	Enum.EnumDescription = Value.IsEmpty() ? FText::GetEmpty() : Text;
	FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
	Enum.PostEditChangeProperty(Event);
}

bool Reorder(FReader& Op, UUserDefinedEnum& Enum)
{
	const FValue* Names = nullptr;
	if (!Op.Array(TEXT("names"), Names))
	{
		return false;
	}
	TArray<FName> Order;
	for (int32 Index = 0; Index < Names->Elements.Num(); ++Index)
	{
		const FValue& Name = Names->Elements[Index];
		const int32 Found = Name.Kind == EAssetDocumentRuntimeValueKind::String ? FindEnumerator(Enum, Name.StringValue) : INDEX_NONE;
		if (Found == INDEX_NONE || Order.Contains(Enum.GetNameByIndex(Found)))
		{
			return Op.Fail(TEXT("UserDefinedEnum.InvalidOrder"), TEXT("names lists every enumerator once, by display name."), FString::Printf(TEXT("names/%d"), Index));
		}
		Order.Add(Enum.GetNameByIndex(Found));
	}
	if (Order.Num() != NumEnumerators(Enum))
	{
		return Op.Fail(TEXT("UserDefinedEnum.InvalidOrder"), TEXT("names lists every enumerator once, by display name."), TEXT("names"));
	}
	for (int32 Index = 0; Index < Order.Num(); ++Index)
	{
		const int32 Current = Enum.GetIndexByName(Order[Index]);
		if (Current != Index)
		{
			FEnumEditorUtils::MoveEnumeratorInUserDefinedEnum(&Enum, Current, Index);
		}
	}
	return true;
}

bool EnumOpUpdatesDependents(const FValue& Op)
{
	const FValue* Kind = FindField(Op, TEXT("op"));
	return Kind != nullptr && Kind->Kind == EAssetDocumentRuntimeValueKind::String
		&& Kind->StringValue != TEXT("tooltip") && Kind->StringValue != TEXT("description");
}
} // namespace

bool HandleUserEnumRead(FCall& Call, FObjectBuilder& Result)
{
	UUserDefinedEnum* Enum = GetEnum(Call);
	bool bDependents = false;
	if (Enum == nullptr || !Call.Args.OnlyFields({TEXT("dependents")}) || !Call.Args.Bool(TEXT("dependents"), bDependents))
	{
		return false;
	}
	Result.AddString(TEXT("tooltip"), Enum->EnumDescription.ToString());
	Result.AddBool(TEXT("bitflags"), FEnumEditorUtils::IsEnumeratorBitflagsType(Enum));
	TArray<FValue> Enumerators;
	for (int32 Index = 0; Index < NumEnumerators(*Enum); ++Index)
	{
		FObjectBuilder Row;
		Row.AddString(TEXT("name"), Enum->GetNameStringByIndex(Index));
		Row.AddString(TEXT("display_name"), DisplayName(*Enum, Index));
		Row.AddString(TEXT("tooltip"), Enum->HasMetaData(ToolTipKey, Index) ? Enum->GetMetaData(ToolTipKey, Index) : FString());
		Row.AddInt(TEXT("value"), Enum->GetValueByIndex(Index));
		Enumerators.Add(Row.Build());
	}
	Result.Add(TEXT("enumerators"), MakeArray(MoveTemp(Enumerators)));
	if (bDependents)
	{
		Result.Add(TEXT("dependents"), UserTypes::DescribeDependents(UserTypes::FindLoadedDependents(*Enum)));
	}
	return true;
}

bool HandleUserEnumEdit(FCall& Call, FObjectBuilder& Result)
{
	UUserDefinedEnum* Enum = GetEnum(Call);
	const FValue* Ops = nullptr;
	if (Enum == nullptr || !Call.Args.OnlyFields({TEXT("ops")}) || !Call.Args.Array(TEXT("ops"), Ops))
	{
		return false;
	}
	const bool bUpdatesDependents = Ops->Elements.ContainsByPredicate(&EnumOpUpdatesDependents);
	UserTypes::FDependentUpdate Dependents(*Enum, bUpdatesDependents ? UserTypes::FindLoadedDependents(*Enum) : TArray<UserTypes::FDependent>());
	if (!Dependents.Admit(Call.Args, TEXT("UserDefinedEnum.DependentHasUnsavedChanges")))
	{
		return false;
	}
	// One editor transaction for the call: the Enum editor's per-edit transactions nest into it.
	const FScopedTransaction CallTransaction(LOCTEXT("EditEnum", "Edit Enum"));
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
			if (!Op.OnlyFields({TEXT("op"), TEXT("name")}) || !Op.String(TEXT("name"), Name) || !CheckDisplayName(Op, *Enum, INDEX_NONE, Name, TEXT("name")))
			{
				return false;
			}
			FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Enum);
			if (!SetDisplayName(Op, *Enum, NumEnumerators(*Enum) - 1, Name, TEXT("name")))
			{
				return false;
			}
		}
		else if (Kind == TEXT("description"))
		{
			FString Tooltip;
			if (!Op.OnlyFields({TEXT("op"), TEXT("tooltip")}) || !Op.String(TEXT("tooltip"), Tooltip))
			{
				return false;
			}
			if (!Enum->EnumDescription.ToString().Equals(Tooltip, ESearchCase::CaseSensitive))
			{
				SetDescription(*Enum, Tooltip);
			}
		}
		else if (Kind == TEXT("bitflags"))
		{
			bool bValue = false;
			if (!Op.OnlyFields({TEXT("op"), TEXT("value")}) || !Op.Bool(TEXT("value"), bValue, true))
			{
				return false;
			}
			if (FEnumEditorUtils::IsEnumeratorBitflagsType(Enum) != bValue)
			{
				FEnumEditorUtils::SetEnumeratorBitflagsTypeState(Enum, bValue);
			}
		}
		else if (Kind == TEXT("order"))
		{
			if (!Op.OnlyFields({TEXT("op"), TEXT("names")}) || !Reorder(Op, *Enum))
			{
				return false;
			}
		}
		else
		{
			if (!Op.String(TEXT("name"), Name))
			{
				return false;
			}
			const int32 Enumerator = RequireEnumerator(Op, *Enum, Name);
			if (Enumerator == INDEX_NONE)
			{
				return false;
			}
			if (Kind == TEXT("rename"))
			{
				FString To;
				if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("to")}) || !Op.String(TEXT("to"), To)
					|| !SetDisplayName(Op, *Enum, Enumerator, To, TEXT("to")))
				{
					return false;
				}
			}
			else if (Kind == TEXT("remove"))
			{
				if (!Op.OnlyFields({TEXT("op"), TEXT("name")}))
				{
					return false;
				}
				FEnumEditorUtils::RemoveEnumeratorFromUserDefinedEnum(Enum, Enumerator);
			}
			else if (Kind == TEXT("tooltip"))
			{
				FString Tooltip;
				if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("tooltip")}) || !Op.String(TEXT("tooltip"), Tooltip))
				{
					return false;
				}
				const FString Current = Enum->HasMetaData(ToolTipKey, Enumerator) ? Enum->GetMetaData(ToolTipKey, Enumerator) : FString();
				if (!Current.Equals(Tooltip, ESearchCase::CaseSensitive))
				{
					// The Enum editor's Description box (metadata is not transactional).
					Enum->Modify();
					Enum->SetMetaData(ToolTipKey, *Tooltip, Enumerator);
				}
			}
			else
			{
				return Op.Fail(TEXT("Data.InvalidOp"), FString::Printf(
					TEXT("Unknown op '%s' (add, rename, remove, order, tooltip, description, bitflags)."), *Kind), TEXT("op"));
			}
		}
	}
	Result.AddInt(TEXT("enumerators"), NumEnumerators(*Enum));
	Result.Add(TEXT("dependents"), Dependents.Finish());
	return true;
}
} // namespace UE::AssetDocument::Graph::Data

#undef LOCTEXT_NAMESPACE
