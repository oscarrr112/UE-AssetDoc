// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/Selector/AssetDocumentAISelectorEvidence.h"

#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "IDetailTreeNode.h"
#include "IPropertyRowGenerator.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

#include <initializer_list>

namespace AssetDocumentAISelectorEvidence
{
struct FSubject
{
	FString Kind;
	FString ExactClassPath;
	FString AssetPath;
	UObject* Object = nullptr;
};

struct FSelectorRow
{
	FString PropertyPath;
	bool bNoneAllowed = false;
	TArray<TPair<FString, FString>> AllowedTypes;
};

FAssetDocumentRuntimeValueDto StringValue(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto BoolValue(bool Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Boolean;
	Result.BooleanValue = Value;
	return Result;
}

FAssetDocumentRuntimeValueDto ArrayValue(TArray<FAssetDocumentRuntimeValueDto> Elements)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Array;
	Result.Elements = MoveTemp(Elements);
	return Result;
}

FAssetDocumentRuntimeValueDto ObjectValue(
	TArray<FString> Keys,
	TArray<FAssetDocumentRuntimeValueDto> Elements)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Object;
	Result.ObjectKeys = MoveTemp(Keys);
	Result.Elements = MoveTemp(Elements);
	return Result;
}

FAssetDocumentPrimitiveOutcome Failure(const TCHAR* Code, const TCHAR* Message)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	FAssetDocumentRuntimeDiagnostic& Diagnostic = Outcome.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/Primitive");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Outcome;
}

const FAssetDocumentRuntimeValueDto* FindField(
	const FAssetDocumentRuntimeValueDto& Object,
	const TCHAR* Name)
{
	for (int32 Index = 0; Index < Object.ObjectKeys.Num(); ++Index)
	{
		if (Object.ObjectKeys[Index] == Name)
		{
			return &Object.Elements[Index];
		}
	}
	return nullptr;
}

bool HasExactKeys(
	const FAssetDocumentRuntimeValueDto& Object,
	std::initializer_list<const TCHAR*> Keys)
{
	if (Object.Kind != EAssetDocumentRuntimeValueKind::Object
		|| Object.ObjectKeys.Num() != static_cast<int32>(Keys.size()))
	{
		return false;
	}
	for (const TCHAR* Key : Keys)
	{
		if (FindField(Object, Key) == nullptr)
		{
			return false;
		}
	}
	return true;
}

bool ReadSubjectArguments(const FAssetDocumentRuntimeValueDto& Value, FSubject& OutSubject)
{
	const FAssetDocumentRuntimeValueDto* Mode = FindField(Value, TEXT("mode"));
	const FAssetDocumentRuntimeValueDto* ClassPath = FindField(Value, TEXT("exact_asset_class_path"));
	if (!Mode || !ClassPath || Mode->Kind != EAssetDocumentRuntimeValueKind::String
		|| ClassPath->Kind != EAssetDocumentRuntimeValueKind::String)
	{
		return false;
	}
	if (Mode->StringValue == TEXT("class_defaults")
		&& HasExactKeys(Value, {TEXT("mode"), TEXT("exact_asset_class_path")}))
	{
		OutSubject.Kind = TEXT("class_defaults");
		OutSubject.ExactClassPath = ClassPath->StringValue;
		return true;
	}
	const FAssetDocumentRuntimeValueDto* ObjectPath = FindField(Value, TEXT("exact_asset_object_path"));
	if (Mode->StringValue != TEXT("live_asset") || !ObjectPath
		|| ObjectPath->Kind != EAssetDocumentRuntimeValueKind::String
		|| !HasExactKeys(Value, {TEXT("mode"), TEXT("exact_asset_class_path"), TEXT("exact_asset_object_path")}))
	{
		return false;
	}
	OutSubject.Kind = TEXT("live_asset");
	OutSubject.ExactClassPath = ClassPath->StringValue;
	OutSubject.AssetPath = ObjectPath->StringValue;
	return true;
}

bool IsCanonicalClassPath(const FString& Path)
{
	const FSoftClassPath ClassPath(Path);
	return ClassPath.IsValid() && ClassPath.ToString() == Path
		&& FPackageName::IsValidObjectPath(Path);
}

bool IsCanonicalObjectPath(const FString& Path)
{
	const FSoftObjectPath ObjectPath(Path);
	return ObjectPath.IsValid() && ObjectPath.IsAsset() && ObjectPath.ToString() == Path
		&& FPackageName::IsValidObjectPath(Path);
}

FAssetDocumentPrimitiveOutcome ResolveSubject(FSubject& Subject)
{
	if (!IsCanonicalClassPath(Subject.ExactClassPath)
		|| (Subject.Kind == TEXT("live_asset") && !IsCanonicalObjectPath(Subject.AssetPath)))
	{
		return Failure(TEXT("AISelector.NonCanonicalSubject"), TEXT("The selector subject paths are not canonical."));
	}
	UClass* Class = FSoftClassPath(Subject.ExactClassPath).TryLoadClass<UObject>();
	if (!Class || Class->GetPathName() != Subject.ExactClassPath)
	{
		return Failure(TEXT("AISelector.SubjectUnavailable"), TEXT("The exact selector class is unavailable."));
	}
	if (Subject.Kind == TEXT("class_defaults"))
	{
		Subject.Object = Class->GetDefaultObject();
	}
	else
	{
		Subject.Object = FSoftObjectPath(Subject.AssetPath).TryLoad();
	}
	if (!Subject.Object)
	{
		return Failure(TEXT("AISelector.SubjectUnavailable"), TEXT("The exact selector subject is unavailable."));
	}
	if (Subject.Object->GetClass() != Class)
	{
		return Failure(TEXT("AISelector.ClassMismatch"), TEXT("The live selector subject has the wrong exact class."));
	}
	FAssetDocumentPrimitiveOutcome Success;
	Success.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Success.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	return Success;
}

FString NormalizePropertyPath(FString Path, bool bNestedInInstancedStruct)
{
	Path.ReplaceInline(TEXT("."), TEXT("/"));
	Path.ReplaceInline(TEXT("->"), bNestedInInstancedStruct ? TEXT("/Properties/") : TEXT("/"));
	return Path;
}

bool IsSelectorHandle(const TSharedPtr<IPropertyHandle>& Handle)
{
	const FStructProperty* Struct = Handle.IsValid() ? CastField<FStructProperty>(Handle->GetProperty()) : nullptr;
	return Struct && Struct->Struct == FBlackboardKeySelector::StaticStruct();
}

bool IsInstancedStructHandle(const TSharedPtr<IPropertyHandle>& Handle)
{
	const FStructProperty* Struct = Handle.IsValid() ? CastField<FStructProperty>(Handle->GetProperty()) : nullptr;
	return Struct && Struct->Struct == FInstancedStruct::StaticStruct();
}

void CollectHandles(
	const TArray<TSharedRef<IDetailTreeNode>>& Nodes,
	TArray<TPair<TSharedPtr<IPropertyHandle>, bool>>& OutHandles,
	bool bNestedInInstancedStruct = false)
{
	for (const TSharedRef<IDetailTreeNode>& Node : Nodes)
	{
		const TSharedPtr<IPropertyHandle> Handle = Node->CreatePropertyHandle();
		if (Handle.IsValid())
		{
			OutHandles.Add({Handle, bNestedInInstancedStruct});
		}
		TArray<TSharedRef<IDetailTreeNode>> Children;
		Node->GetChildren(Children, true);
		CollectHandles(Children, OutHandles, bNestedInInstancedStruct || IsInstancedStructHandle(Handle));
	}
}

TSharedRef<IPropertyRowGenerator> MakeGenerator()
{
	FPropertyRowGeneratorArgs Args;
	Args.bAllowEditingClassDefaultObjects = true;
	FPropertyEditorModule& PropertyEditor =
		FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	return PropertyEditor.CreatePropertyRowGenerator(Args);
}

bool AppendSelectorRow(
	const TSharedPtr<IPropertyHandle>& Handle,
	const FString& PropertyPath,
	TMap<FString, FSelectorRow>& OutRows)
{
	bool bNoneAllowed = false;
	const TSharedPtr<IPropertyHandle> NoneAllowed = Handle->GetChildHandle(TEXT("bNoneIsAllowedValue"));
	const TSharedPtr<IPropertyHandle> AllowedTypes = Handle->GetChildHandle(TEXT("AllowedTypes"));
	const TSharedPtr<IPropertyHandleArray> AllowedTypeArray = AllowedTypes.IsValid() ? AllowedTypes->AsArray() : nullptr;
	uint32 Count = 0;
	if (!NoneAllowed.IsValid() || NoneAllowed->GetValue(bNoneAllowed) != FPropertyAccess::Success
		|| !AllowedTypeArray.IsValid() || AllowedTypeArray->GetNumElements(Count) != FPropertyAccess::Success)
	{
		return false;
	}
	FSelectorRow Row;
	Row.PropertyPath = PropertyPath;
	Row.bNoneAllowed = bNoneAllowed;
	for (uint32 Index = 0; Index < Count; ++Index)
	{
		const TSharedPtr<IPropertyHandle> TypeHandle = AllowedTypeArray->GetElement(Index);
		UObject* TypeObject = nullptr;
		if (!TypeHandle.IsValid() || TypeHandle->GetValue(TypeObject) != FPropertyAccess::Success || !TypeObject)
		{
			return false;
		}
		FString BaseClassPath;
		if (const UBlackboardKeyType_Object* ObjectType = Cast<UBlackboardKeyType_Object>(TypeObject))
		{
			BaseClassPath = ObjectType->BaseClass ? ObjectType->BaseClass->GetPathName() : FString();
		}
		else if (const UBlackboardKeyType_Class* ClassType = Cast<UBlackboardKeyType_Class>(TypeObject))
		{
			BaseClassPath = ClassType->BaseClass ? ClassType->BaseClass->GetPathName() : FString();
		}
		Row.AllowedTypes.Add({TypeObject->GetClass()->GetPathName(), MoveTemp(BaseClassPath)});
	}
	Row.AllowedTypes.Sort([](const TPair<FString, FString>& Left, const TPair<FString, FString>& Right)
	{
		return Left.Key == Right.Key ? Left.Value < Right.Value : Left.Key < Right.Key;
	});
	OutRows.Add(Row.PropertyPath, MoveTemp(Row));
	return true;
}

const FStructProperty* GetContainerElementStruct(const TSharedPtr<IPropertyHandle>& Handle)
{
	if (!Handle.IsValid())
	{
		return nullptr;
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(Handle->GetProperty()))
	{
		return CastField<FStructProperty>(Array->Inner);
	}
	if (const FMapProperty* Map = CastField<FMapProperty>(Handle->GetProperty()))
	{
		return CastField<FStructProperty>(Map->ValueProp);
	}
	if (const FSetProperty* Set = CastField<FSetProperty>(Handle->GetProperty()))
	{
		return CastField<FStructProperty>(Set->ElementProp);
	}
	return nullptr;
}

bool IsContainerHandle(const TSharedPtr<IPropertyHandle>& Handle)
{
	if (!Handle.IsValid())
	{
		return false;
	}
	const FProperty* Property = Handle->GetProperty();
	return CastField<FArrayProperty>(Property)
		|| CastField<FMapProperty>(Property)
		|| CastField<FSetProperty>(Property);
}

bool IsContainerDescendant(const TSharedPtr<IPropertyHandle>& Handle)
{
	if (!Handle.IsValid())
	{
		return false;
	}
	TSharedPtr<IPropertyHandle> Parent = Handle->GetParentHandle();
	while (Parent.IsValid())
	{
		if (IsContainerHandle(Parent))
		{
			return true;
		}
		Parent = Parent->GetParentHandle();
	}
	return false;
}

bool AppendStructSelectorRows(
	UScriptStruct& Struct,
	const FString& Prefix,
	TMap<FString, FSelectorRow>& OutRows);

bool AppendDirectSelectorRow(
	const TPair<TSharedPtr<IPropertyHandle>, bool>& Observed,
	TMap<FString, FSelectorRow>& OutRows)
{
	if (!IsSelectorHandle(Observed.Key))
	{
		return true;
	}
	return AppendSelectorRow(
		Observed.Key,
		NormalizePropertyPath(FString(Observed.Key->GetPropertyPath()), Observed.Value),
		OutRows);
}

bool AppendContainerSelectorRows(
	const TPair<TSharedPtr<IPropertyHandle>, bool>& Observed,
	TMap<FString, FSelectorRow>& OutRows)
{
	const FStructProperty* ElementStruct = GetContainerElementStruct(Observed.Key);
	if (!ElementStruct || !ElementStruct->Struct)
	{
		return true;
	}
	return AppendStructSelectorRows(*ElementStruct->Struct, Observed.Key->GetProperty()->GetName(), OutRows);
}

bool AppendStructSelectorRows(
	UScriptStruct& Struct,
	const FString& Prefix,
	TMap<FString, FSelectorRow>& OutRows)
{
	const TSharedRef<IPropertyRowGenerator> Generator = MakeGenerator();
	Generator->OnRowsRefreshed().AddLambda([]() {});
	Generator->SetStructure(MakeShared<FStructOnScope>(&Struct));
	TArray<TPair<TSharedPtr<IPropertyHandle>, bool>> Handles;
	CollectHandles(Generator->GetRootTreeNodes(), Handles);
	for (const TPair<TSharedPtr<IPropertyHandle>, bool>& Observed : Handles)
	{
		if (IsSelectorHandle(Observed.Key) && !AppendSelectorRow(
			Observed.Key,
			FString::Printf(TEXT("%s/*/%s"), *Prefix,
				*NormalizePropertyPath(FString(Observed.Key->GetPropertyPath()), Observed.Value)),
			OutRows))
		{
			return false;
		}
	}
	return true;
}

bool BuildSelectorRows(UObject& Subject, TMap<FString, FSelectorRow>& OutRows)
{
	const TSharedRef<IPropertyRowGenerator> Generator = MakeGenerator();
	Generator->OnRowsRefreshed().AddLambda([]() {});
	Generator->SetObjects({&Subject});
	TArray<TPair<TSharedPtr<IPropertyHandle>, bool>> Handles;
	CollectHandles(Generator->GetRootTreeNodes(), Handles);
	for (const TPair<TSharedPtr<IPropertyHandle>, bool>& Observed : Handles)
	{
		if (IsContainerDescendant(Observed.Key))
		{
			continue;
		}
		if (!AppendDirectSelectorRow(Observed, OutRows))
		{
			return false;
		}
		if (!AppendContainerSelectorRows(Observed, OutRows))
		{
			return false;
		}
	}
	return true;
}

FAssetDocumentRuntimeValueDto BuildSubjectValue(const FSubject& Subject)
{
	TArray<FString> Keys{TEXT("kind"), TEXT("exact_class_path")};
	TArray<FAssetDocumentRuntimeValueDto> Values{StringValue(Subject.Kind), StringValue(Subject.ExactClassPath)};
	if (Subject.Kind == TEXT("live_asset"))
	{
		Keys.Add(TEXT("asset_path"));
		Values.Add(StringValue(Subject.AssetPath));
	}
	return ObjectValue(MoveTemp(Keys), MoveTemp(Values));
}

FAssetDocumentRuntimeValueDto BuildSelectorValue(const TMap<FString, FSelectorRow>& Rows)
{
	TArray<FString> Paths;
	Rows.GetKeys(Paths);
	Paths.Sort();
	TArray<FAssetDocumentRuntimeValueDto> Values;
	for (const FString& Path : Paths)
	{
		const FSelectorRow& Row = Rows.FindChecked(Path);
		TArray<FAssetDocumentRuntimeValueDto> AllowedTypes;
		for (const TPair<FString, FString>& AllowedType : Row.AllowedTypes)
		{
			TArray<FString> Keys{TEXT("filter_type_path")};
			TArray<FAssetDocumentRuntimeValueDto> Fields{StringValue(AllowedType.Key)};
			if (!AllowedType.Value.IsEmpty())
			{
				Keys.Add(TEXT("base_class_path"));
				Fields.Add(StringValue(AllowedType.Value));
			}
			AllowedTypes.Add(ObjectValue(MoveTemp(Keys), MoveTemp(Fields)));
		}
		Values.Add(ObjectValue(
			{TEXT("property_path"), TEXT("authored_field_name"), TEXT("read_only"), TEXT("none_allowed"), TEXT("allowed_types")},
			{StringValue(Row.PropertyPath), StringValue(TEXT("Key")), BoolValue(true), BoolValue(Row.bNoneAllowed), ArrayValue(MoveTemp(AllowedTypes))}));
	}
	return ArrayValue(MoveTemp(Values));
}
} // namespace AssetDocumentAISelectorEvidence

FAssetDocumentPrimitiveOutcome BuildAssetDocumentAISelectorEvidence(
	const FAssetDocumentPrimitiveInvocation& Invocation)
{
	using namespace AssetDocumentAISelectorEvidence;
	FSubject Subject;
	if (!ReadSubjectArguments(Invocation.ValueArguments, Subject))
	{
		return Failure(TEXT("PrimitiveInvocation.InvalidValueShape"), TEXT("The selector primitive input does not match its exact mode shape."));
	}
	FAssetDocumentPrimitiveOutcome Resolution = ResolveSubject(Subject);
	if (Resolution.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
	{
		return Resolution;
	}
	TMap<FString, FSelectorRow> Rows;
	if (!BuildSelectorRows(*Subject.Object, Rows))
	{
		return Failure(TEXT("AISelector.EvidenceBuildFailed"), TEXT("PropertyEditor could not build complete selector evidence."));
	}
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value = ObjectValue(
		{TEXT("subject"), TEXT("selectors")},
		{BuildSubjectValue(Subject), BuildSelectorValue(Rows)});
	return Outcome;
}
