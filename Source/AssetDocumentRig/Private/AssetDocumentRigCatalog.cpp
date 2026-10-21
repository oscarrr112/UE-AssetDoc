// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Solver and retarget op types (design §3.6) for list_types (kind=ikrig_solver / ikretarget_op,
// AssetDocumentTypeListers.h) and for rig.catalog.v1 ({kind: solver|op, name?, path?, offset?, limit?} ->
// {total, next_offset, rows[]}, the same rows). The types are what the editors' "Add New" menus list
// (SIKRigSolverStack::CacheSolverMetaData, SRetargetOpList::CacheOpTypeMetaData: every child struct, instantiated
// once to read its metadata). Field defaults are a new instance's values (ops after nothing is added to a stack, so
// auto-filled bones are empty here).

#include "AssetDocumentRigPrimitives.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Retargeter/IKRetargetOps.h"
#include "Rig/Solvers/IKRigSolverBase.h"
#include "StructUtils/InstancedStruct.h"

namespace UE::AssetDocument::Graph::Rig
{
namespace
{
FString QueryValue(const TMap<FString, FString>& Query, const TCHAR* Key)
{
	const FString* Value = Query.Find(Key);
	return Value != nullptr ? Value->TrimStartAndEnd() : FString();
}

bool Matches(const FString& Path, const FString& Name, const FString& DisplayName, const TMap<FString, FString>& Query)
{
	const FString Pattern = QueryValue(Query, TEXT("name"));
	if (!Pattern.IsEmpty())
	{
		const bool bWildcard = Pattern.Contains(TEXT("*")) || Pattern.Contains(TEXT("?"));
		auto Test = [&Pattern, bWildcard](const FString& Text)
		{
			return bWildcard ? Text.MatchesWildcard(Pattern, ESearchCase::IgnoreCase) : Text.Contains(Pattern, ESearchCase::IgnoreCase);
		};
		if (!Test(Path) && !Test(Name) && !Test(DisplayName))
		{
			return false;
		}
	}
	const FString Prefix = QueryValue(Query, TEXT("path"));
	return Prefix.IsEmpty() || Path.StartsWith(Prefix, ESearchCase::IgnoreCase);
}

bool NoSkip(const FProperty&)
{
	return false;
}

void SetFields(FJsonObject& Row, const TCHAR* Key, const UScriptStruct* Type, const void* Defaults, TFunctionRef<bool(const FProperty&)> Skip)
{
	if (Type == nullptr)
	{
		return;
	}
	if (Defaults != nullptr)
	{
		Row.SetArrayField(Key, DescribeFields(*Type, Defaults, Skip));
		return;
	}
	const FStructInstance Instance(Type);
	Row.SetArrayField(Key, DescribeFields(*Type, Instance.Get(), Skip));
}

bool CollectRows(FStringView Kind, const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError)
{
	if (Kind == TEXTVIEW("solver"))
	{
		return ListSolverTypes(Query, OutRows, OutError);
	}
	if (Kind == TEXTVIEW("op"))
	{
		return ListOpTypes(Query, OutRows, OutError);
	}
	OutError = FString::Printf(TEXT("kind must be solver or op, got '%s'."), *FString(Kind));
	return false;
}
} // namespace

bool ListSolverTypes(const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError)
{
	for (UScriptStruct* Type : GetSolverTypes())
	{
		FInstancedStruct Instance(Type);
		FIKRigSolverBase& Solver = Instance.GetMutable<FIKRigSolverBase>();
		const FString Path = Type->GetPathName();
		const FString Name = GetSolverTypeName(*Type);
		const FString DisplayName = Solver.GetNiceName().ToString();
		if (!Matches(Path, Name, DisplayName, Query))
		{
			continue;
		}
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Path"), Path);
		Row->SetStringField(TEXT("Name"), Name);
		Row->SetStringField(TEXT("DisplayName"), DisplayName);
		Row->SetBoolField(TEXT("UsesStartBone"), Solver.UsesStartBone());
		Row->SetBoolField(TEXT("UsesEndBone"), Solver.UsesEndBone());
		if (const UScriptStruct* SettingsType = Solver.GetSolverSettingsType())
		{
			SetFields(*Row, TEXT("Settings"), SettingsType, Solver.GetSolverSettings(), &NoSkip);
		}
		if (Solver.UsesCustomGoalSettings())
		{
			SetFields(*Row, TEXT("GoalSettings"), Solver.GetGoalSettingsType(), nullptr, &NoSkip);
		}
		if (Solver.UsesCustomBoneSettings())
		{
			SetFields(*Row, TEXT("BoneSettings"), Solver.GetBoneSettingsType(), nullptr, &NoSkip);
		}
		OutRows.Add(Row);
	}
	return true;
}

bool ListOpTypes(const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError)
{
	for (UScriptStruct* Type : GetOpTypes())
	{
		FInstancedStruct Instance(Type);
		FIKRetargetOpBase& Op = Instance.GetMutable<FIKRetargetOpBase>();
		const FString Path = Type->GetPathName();
		const FString Name = GetOpTypeName(*Type);
		const FString DisplayName = Op.GetDefaultName().ToString();
		if (!Matches(Path, Name, DisplayName, Query))
		{
			continue;
		}
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Path"), Path);
		Row->SetStringField(TEXT("Name"), Name);
		Row->SetStringField(TEXT("DisplayName"), DisplayName);
		Row->SetBoolField(TEXT("Singleton"), Op.IsSingleton());
		Row->SetBoolField(TEXT("CanHaveChildren"), Op.CanHaveChildOps());
		if (const UScriptStruct* ParentType = Op.GetParentOpType())
		{
			Row->SetStringField(TEXT("ParentType"), GetOpTypeName(*ParentType));
		}
		Row->SetBoolField(TEXT("ChainMapping"), static_cast<const FIKRetargetOpBase&>(Op).GetChainMapping() != nullptr);
		if (const UScriptStruct* SettingsType = Op.GetSettingsType())
		{
			const void* Settings = Op.GetSettings();
			SetFields(*Row, TEXT("Settings"), SettingsType, Settings, [](const FProperty& Property) { return IsOpOwnedField(Property); });
			bool bTargetRig = false;
			for (TFieldIterator<FProperty> It(SettingsType); It; ++It)
			{
				bTargetRig |= IsTargetRigField(**It);
				if (IsChainRowArray(**It))
				{
					const FStructProperty* Element = CastField<FStructProperty>(CastField<FArrayProperty>(*It)->Inner);
					const FProperty* NameProperty = Element->Struct->FindPropertyByName(TEXT("TargetChainName"));
					SetFields(*Row, TEXT("ChainSettings"), Element->Struct, nullptr, [NameProperty](const FProperty& Property) { return &Property == NameProperty; });
				}
			}
			Row->SetBoolField(TEXT("TargetIKRig"), bTargetRig);
		}
		OutRows.Add(Row);
	}
	return true;
}

bool HandleCatalog(FCall& Call, FObjectBuilder& Result)
{
	FString Kind;
	FString Name;
	FString Path;
	int64 Offset = 0;
	int64 Limit = EnumerateDefaultLimit;
	if (!Call.Args.OnlyFields({TEXT("kind"), TEXT("name"), TEXT("path"), TEXT("offset"), TEXT("limit")})
		|| !Call.Args.String(TEXT("kind"), Kind) || !Call.Args.String(TEXT("name"), Name, false)
		|| !Call.Args.String(TEXT("path"), Path, false) || !Call.Args.Int(TEXT("offset"), Offset, false)
		|| !Call.Args.Int(TEXT("limit"), Limit, false))
	{
		return false;
	}
	if (Offset < 0 || Limit < 1 || Limit > EnumerateMaximumLimit)
	{
		return Call.Args.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("offset must be >= 0 and limit 1..%d."), EnumerateMaximumLimit), TEXT("offset"));
	}
	TMap<FString, FString> Query;
	Query.Add(TEXT("name"), Name);
	Query.Add(TEXT("path"), Path);
	TArray<TSharedPtr<FJsonObject>> Rows;
	FString Error;
	if (!CollectRows(Kind, Query, Rows, Error))
	{
		return Call.Args.Fail(TEXT("IKRig.UnknownCatalogKind"), Error, TEXT("kind"));
	}
	Rows.Sort([](const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B)
	{
		return A->GetStringField(TEXT("Path")) < B->GetStringField(TEXT("Path"));
	});
	TArray<FValue> Page;
	int32 NodeCount = 8;
	int32 Index = static_cast<int32>(FMath::Min<int64>(Offset, Rows.Num()));
	for (; Index < Rows.Num() && Page.Num() < Limit; ++Index)
	{
		FValue Row = JsonToValue(MakeShared<FJsonValueObject>(Rows[Index]));
		const int32 RowNodes = CountValueNodes(Row);
		if (!Page.IsEmpty() && NodeCount + RowNodes > QueryValueNodeBudget)
		{
			break;
		}
		NodeCount += RowNodes;
		Page.Add(MoveTemp(Row));
	}
	Result.AddInt(TEXT("total"), Rows.Num());
	Result.AddInt(TEXT("next_offset"), Index < Rows.Num() ? Index : -1);
	Result.Add(TEXT("rows"), MakeArray(MoveTemp(Page)));
	return true;
}
} // namespace UE::AssetDocument::Graph::Rig
