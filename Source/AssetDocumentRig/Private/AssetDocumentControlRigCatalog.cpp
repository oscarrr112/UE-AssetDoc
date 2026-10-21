// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Control Rig discovery (design §3.8) for list_types (kind = rigvm_node / rigvm_type / controlrig_module,
// AssetDocumentTypeListers.h) and rig.controlrig.catalog.v1 ({kind: node|type|module, name?, path?, offset?,
// limit?} -> {total, next_offset, rows[]}, the same rows):
//   rigvm_node         units and events (every FRigVMStruct the graph palette offers), dispatch factories and
//                      multi-permutation templates from the RigVM registry, with their pins as a new node has them;
//   rigvm_type         the single value types of the registry (variable / pin types; arrays are TArray<...> of them);
//   controlrig_module  rig module assets from the asset registry tags (identifier, category, exposed connectors).
// Everything is read from the registry / asset registry; nothing is loaded except the module tags' struct import.

#include "AssetDocumentControlRig.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "ControlRigBlueprintLegacy.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "RigVMCore/RigVMDispatchFactory.h"
#include "RigVMCore/RigVMExecuteContext.h"
#include "RigVMCore/RigVMRegistry.h"
#include "RigVMCore/RigVMStruct.h"
#include "RigVMCore/RigVMTemplate.h"
#include "RigVMTypeUtils.h"
#include "Rigs/RigModuleDefines.h"
#include "Misc/StringOutputDevice.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::Graph::Rig::ControlRig
{
namespace
{
FString QueryValue(const TMap<FString, FString>& Query, const TCHAR* Key)
{
	const FString* Value = Query.Find(Key);
	return Value != nullptr ? Value->TrimStartAndEnd() : FString();
}

bool Matches(const TArray<FString>& Texts, const FString& Path, const TMap<FString, FString>& Query)
{
	const FString Pattern = QueryValue(Query, TEXT("name"));
	if (!Pattern.IsEmpty())
	{
		const bool bWildcard = Pattern.Contains(TEXT("*")) || Pattern.Contains(TEXT("?"));
		bool bAny = false;
		for (const FString& Text : Texts)
		{
			bAny = bAny || (bWildcard ? Text.MatchesWildcard(Pattern, ESearchCase::IgnoreCase) : Text.Contains(Pattern, ESearchCase::IgnoreCase));
		}
		if (!bAny)
		{
			return false;
		}
	}
	const FString Prefix = QueryValue(Query, TEXT("path"));
	return Prefix.IsEmpty() || Path.StartsWith(Prefix, ESearchCase::IgnoreCase);
}

UObject* TypeObjectOf(const FProperty* Property)
{
	if (const FArrayProperty* Array = CastField<FArrayProperty>(Property))
	{
		Property = Array->Inner;
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
	{
		return Struct->Struct;
	}
	if (const FEnumProperty* Enum = CastField<FEnumProperty>(Property))
	{
		return Enum->GetEnum();
	}
	if (const FByteProperty* Byte = CastField<FByteProperty>(Property))
	{
		return Byte->Enum;
	}
	if (const FClassProperty* Class = CastField<FClassProperty>(Property))
	{
		return Class->MetaClass;
	}
	if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(Property))
	{
		return Object->PropertyClass;
	}
	if (const FInterfaceProperty* Interface = CastField<FInterfaceProperty>(Property))
	{
		return Interface->InterfaceClass;
	}
	return nullptr;
}

TSharedPtr<FJsonObject> UnitRow(UScriptStruct& Struct)
{
	TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
	Row->SetStringField(TEXT("Path"), Struct.GetPathName());
	Row->SetStringField(TEXT("Name"), UnitName(Struct));
	Row->SetStringField(TEXT("Kind"), TEXT("Unit"));
	Row->SetStringField(TEXT("DisplayName"), Struct.GetDisplayNameText().ToString());
	FString Category;
	Struct.GetStringMetaDataHierarchical(TEXT("Category"), &Category);
	if (!Category.IsEmpty())
	{
		Row->SetStringField(TEXT("Category"), Category);
	}
	FString Keywords;
	Struct.GetStringMetaDataHierarchical(TEXT("Keywords"), &Keywords);
	if (!Keywords.IsEmpty())
	{
		Row->SetStringField(TEXT("Keywords"), Keywords);
	}
	if (Struct.HasMetaData(TEXT("Deprecated")))
	{
		Row->SetBoolField(TEXT("Deprecated"), true);
	}
	const FStructInstance Instance(&Struct);
	if (const FRigVMStruct* Unit = static_cast<const FRigVMStruct*>(Instance.Get()))
	{
		if (!Unit->GetEventName().IsNone())
		{
			Row->SetStringField(TEXT("Event"), Unit->GetEventName().ToString());
		}
		if (Unit->CanOnlyExistOnce())
		{
			Row->SetBoolField(TEXT("CanOnlyExistOnce"), true);
		}
	}
	Row->SetArrayField(TEXT("Pins"), DescribeUnitPins(Struct));
	return Row;
}

TSharedPtr<FJsonObject> TemplateRow(const FString& Name, const FString& Kind, const FString& Category, const FString& Keywords,
	const FRigVMTemplate* Template)
{
	TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
	Row->SetStringField(TEXT("Path"), Name);
	Row->SetStringField(TEXT("Name"), Name);
	Row->SetStringField(TEXT("Kind"), Kind);
	if (!Category.IsEmpty())
	{
		Row->SetStringField(TEXT("Category"), Category);
	}
	if (!Keywords.IsEmpty())
	{
		Row->SetStringField(TEXT("Keywords"), Keywords);
	}
	if (Template != nullptr)
	{
		Row->SetStringField(TEXT("Notation"), Template->GetNotation().ToString());
		TArray<TSharedPtr<FJsonValue>> Arguments;
		for (int32 Index = 0; Index < Template->NumArguments(); ++Index)
		{
			const FRigVMTemplateArgument* Argument = Template->GetArgument(Index);
			TSharedPtr<FJsonObject> Pin = MakeShared<FJsonObject>();
			Pin->SetStringField(TEXT("Name"), Argument->GetName().ToString());
			Pin->SetStringField(TEXT("Direction"), DirectionName(static_cast<uint8>(Argument->GetDirection())));
			Arguments.Add(MakeShared<FJsonValueObject>(Pin));
		}
		Row->SetArrayField(TEXT("Pins"), Arguments);
		Row->SetNumberField(TEXT("Permutations"), Template->NumPermutations());
	}
	return Row;
}

// FRigVMTemplate::GetCategory, reading only the permutations that are backed by a function. A
// template keeps a slot for every permutation, and a slot can be empty (no function): the
// templates the legacy If and Select nodes register once such a node is loaded have no function
// behind any permutation, and a slot is emptied when the registry drops a type the permutation
// used (a deleted user-defined struct or enum). The engine's GetCategory reads every slot's
// function and crashes on an empty one.
FString TemplateCategory(const FRigVMTemplate& Template)
{
	TArray<const UScriptStruct*> Structs;
	for (int32 Index = 0; Index < Template.NumPermutations(); ++Index)
	{
		const FRigVMFunction* Function = Template.GetPermutation(Index);
		if (Function != nullptr && Function->Struct != nullptr)
		{
			Structs.Add(Function->Struct);
		}
	}
	FString Category;
	if (Structs.IsEmpty() || !Structs[0]->GetStringMetaDataHierarchical(FRigVMStruct::CategoryMetaName, &Category) || Category.IsEmpty())
	{
		return FString();
	}
	for (int32 Index = 1; Index < Structs.Num(); ++Index)
	{
		FString OtherCategory;
		if (!Structs[Index]->GetStringMetaDataHierarchical(FRigVMStruct::CategoryMetaName, &OtherCategory))
		{
			continue;
		}
		while (!OtherCategory.StartsWith(Category, ESearchCase::IgnoreCase))
		{
			FString Left;
			if (!Category.Split(TEXT("|"), &Left, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
			{
				return FString();
			}
			Category = Left;
		}
	}
	return Category;
}

bool CollectRows(FStringView Kind, const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError)
{
	if (Kind == TEXTVIEW("node"))
	{
		return ListNodeTypes(Query, OutRows, OutError);
	}
	if (Kind == TEXTVIEW("type"))
	{
		return ListValueTypes(Query, OutRows, OutError);
	}
	if (Kind == TEXTVIEW("module"))
	{
		return ListModules(Query, OutRows, OutError);
	}
	OutError = FString::Printf(TEXT("kind must be node, type or module, got '%s'."), *FString(Kind));
	return false;
}
} // namespace

TArray<TSharedPtr<FJsonValue>> DescribeUnitPins(UScriptStruct& Struct)
{
	TArray<TSharedPtr<FJsonValue>> Pins;
	const FStructInstance Instance(&Struct);
	const FRigVMStruct* Unit = static_cast<const FRigVMStruct*>(Instance.Get());
	if (Unit == nullptr)
	{
		return Pins;
	}
	// Pins come in the order the controller creates them: base struct fields first (the execute pin of
	// FRigUnitMutable leads), then each derived struct's own fields.
	TArray<const UStruct*> Chain;
	for (const UStruct* Each = &Struct; Each != nullptr; Each = Each->GetSuperStruct())
	{
		Chain.Insert(Each, 0);
	}
	TArray<FProperty*> Properties;
	for (const UStruct* Each : Chain)
	{
		for (TFieldIterator<FProperty> It(Each, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			Properties.Add(*It);
		}
	}
	for (FProperty* Property : Properties)
	{
		if (!Unit->ShouldCreatePinForProperty(Property))
		{
			continue;
		}
		UObject* TypeObject = TypeObjectOf(Property);
		FString CPPType = RigVMTypeUtils::PostProcessCPPType(RigVMTypeUtils::GetCPPTypeFromProperty(Property), TypeObject);
		// An execute pin (FRigVMExecutePin) is an execute context pin on the node.
		const UScriptStruct* PinStruct = Cast<UScriptStruct>(TypeObject);
		if (PinStruct != nullptr && PinStruct->IsChildOf(FRigVMExecutePin::StaticStruct()))
		{
			TypeObject = FRigVMExecuteContext::StaticStruct();
			CPPType = RigVMTypeUtils::GetUniqueStructTypeName(FRigVMExecuteContext::StaticStruct());
		}
		TSharedPtr<FJsonObject> Pin = MakeShared<FJsonObject>();
		Pin->SetStringField(TEXT("Name"), Property->GetName());
		Pin->SetStringField(TEXT("Type"), TypeName(CPPType, TypeObject));
		Pin->SetStringField(TEXT("Direction"), DirectionName(static_cast<uint8>(FRigVMStruct::GetPinDirectionFromProperty(Property))));
		if (!IsValuelessType(CPPType, TypeObject))
		{
			FValue Value;
			FString Error;
			if (EncodeProperty(*Property, Property->ContainerPtrToValuePtr<void>(Instance.Get()), Value, Error))
			{
				Pin->SetField(TEXT("Default"), ValueToJson(Value));
			}
		}
		if (Property->HasMetaData(FRigVMStruct::ExpandPinByDefaultMetaName))
		{
			Pin->SetBoolField(TEXT("Expand"), true);
		}
		Pins.Add(MakeShared<FJsonValueObject>(Pin));
	}
	return Pins;
}

bool ListNodeTypes(const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError)
{
	for (UScriptStruct* Struct : GetUnitStructs())
	{
		const FString Path = Struct->GetPathName();
		if (Matches({Path, UnitName(*Struct), Struct->GetDisplayNameText().ToString()}, Path, Query))
		{
			OutRows.Add(UnitRow(*Struct));
		}
	}
	const FRigVMRegistry& Registry = FRigVMRegistry::Get();
	for (const FRigVMDispatchFactory* Factory : Registry.GetFactories())
	{
		if (Factory == nullptr)
		{
			continue;
		}
		const FString Name = Factory->GetFactoryName().ToString();
		if (Matches({Name}, Name, Query))
		{
			OutRows.Add(TemplateRow(Name, TEXT("Dispatch"), Factory->GetCategory(), Factory->GetKeywords(), Factory->GetTemplate()));
		}
	}
	const TChunkedArray<FRigVMTemplate>& Templates = Registry.GetTemplates();
	for (int32 TemplateIndex = 0; TemplateIndex < Templates.Num(); ++TemplateIndex)
	{
		const FRigVMTemplate& Template = Templates[TemplateIndex];
		// As the graph palette lists templates: several permutations, not a dispatch, and a function
		// behind the first permutation.
		if (Template.NumPermutations() <= 1 || Template.UsesDispatch() || Template.GetPermutation(0) == nullptr)
		{
			continue;
		}
		const FString Name = Template.GetNotation().ToString();
		if (Matches({Name}, Name, Query))
		{
			OutRows.Add(TemplateRow(Name, TEXT("Template"), TemplateCategory(Template), Template.GetKeywords(), &Template));
		}
	}
	return true;
}

bool ListValueTypes(const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError)
{
	const FRigVMRegistry& Registry = FRigVMRegistry::Get();
	TSet<FString> Seen;
	for (const TRigVMTypeIndex Index : Registry.GetTypesForCategory(FRigVMTemplateArgument::ETypeCategory_SingleAnyValue))
	{
		const FRigVMTemplateArgumentType& Type = Registry.GetType(Index);
		const FString CPPType = Type.CPPType.ToString();
		if (IsValuelessType(CPPType, Type.CPPTypeObject))
		{
			continue;
		}
		const FString Name = TypeName(CPPType, Type.CPPTypeObject);
		if (Seen.Contains(Name) || !Matches({Name, CPPType}, Name, Query))
		{
			continue;
		}
		Seen.Add(Name);
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Path"), Name);
		Row->SetStringField(TEXT("CPPType"), CPPType);
		if (Type.CPPTypeObject != nullptr)
		{
			Row->SetStringField(TEXT("Object"), Type.CPPTypeObject->GetPathName());
			Row->SetStringField(TEXT("ObjectKind"), Type.CPPTypeObject->IsA<UScriptStruct>() ? TEXT("Struct")
				: (Type.CPPTypeObject->IsA<UEnum>() ? TEXT("Enum") : TEXT("Class")));
		}
		OutRows.Add(Row);
	}
	return true;
}

// Rig modules (the Module Assembly panel's browser). A row is {Path, Class, Name, Category?, Connectors: ["Root
// (primary)", "Twist Bones (optional, array)", ...]}; with the query's detail=true also Type, Keywords, Description and
// connectors as {Name, Type, Optional?, Array?, Description?} (the descriptions are most of a full listing).
bool ListModules(const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError)
{
	const FString Detail = QueryValue(Query, TEXT("detail"));
	const bool bDetail = Detail.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Detail == TEXT("1");
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.ClassPaths.Add(UControlRigBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);
	static const FName TypeTag = TEXT("ControlRigType");
	static const FName SettingsTag = TEXT("RigModuleSettings");
	for (const FAssetData& Asset : Assets)
	{
		FString Type;
		if (!Asset.GetTagValue(TypeTag, Type) || !Type.Contains(TEXT("RigModule")))
		{
			continue;
		}
		FRigModuleSettings Settings;
		FString SettingsText;
		if (Asset.GetTagValue(SettingsTag, SettingsText))
		{
			FStringOutputDevice Errors;
			FRigModuleSettings::StaticStruct()->ImportText(*SettingsText, &Settings, nullptr, PPF_None, &Errors, TEXT("RigModuleSettings"));
		}
		const FString Path = Asset.GetObjectPathString();
		if (!Matches({Path, Asset.AssetName.ToString(), Settings.Identifier.Name, Settings.Category, Settings.Keywords}, Path, Query))
		{
			continue;
		}
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Path"), Path);
		Row->SetStringField(TEXT("Class"), Path + TEXT("_C"));
		Row->SetStringField(TEXT("Name"), Settings.Identifier.Name.IsEmpty() ? Asset.AssetName.ToString() : Settings.Identifier.Name);
		if (!Settings.Category.IsEmpty())
		{
			Row->SetStringField(TEXT("Category"), Settings.Category);
		}
		TArray<TSharedPtr<FJsonValue>> Connectors;
		if (!bDetail)
		{
			// The browser's list: connector names, the primary one and optional / array ones marked.
			for (const FRigModuleConnector& Connector : Settings.ExposedConnectors)
			{
				TArray<FString> Marks;
				if (Connector.Settings.Type == EConnectorType::Primary)
				{
					Marks.Add(TEXT("primary"));
				}
				if (Connector.Settings.bOptional)
				{
					Marks.Add(TEXT("optional"));
				}
				if (Connector.Settings.bIsArray)
				{
					Marks.Add(TEXT("array"));
				}
				Connectors.Add(MakeShared<FJsonValueString>(Marks.IsEmpty() ? Connector.Name
					: FString::Printf(TEXT("%s (%s)"), *Connector.Name, *FString::Join(Marks, TEXT(", ")))));
			}
			Row->SetArrayField(TEXT("Connectors"), Connectors);
			OutRows.Add(Row);
			continue;
		}
		if (!Settings.Identifier.Type.IsEmpty())
		{
			Row->SetStringField(TEXT("Type"), Settings.Identifier.Type);
		}
		if (!Settings.Keywords.IsEmpty())
		{
			Row->SetStringField(TEXT("Keywords"), Settings.Keywords);
		}
		if (!Settings.Description.IsEmpty())
		{
			Row->SetStringField(TEXT("Description"), Settings.Description);
		}
		for (const FRigModuleConnector& Connector : Settings.ExposedConnectors)
		{
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("Name"), Connector.Name);
			Item->SetStringField(TEXT("Type"), StaticEnum<EConnectorType>()->GetNameStringByValue(static_cast<int64>(Connector.Settings.Type)));
			if (Connector.Settings.bOptional)
			{
				Item->SetBoolField(TEXT("Optional"), true);
			}
			if (Connector.Settings.bIsArray)
			{
				Item->SetBoolField(TEXT("Array"), true);
			}
			if (!Connector.Settings.Description.IsEmpty())
			{
				Item->SetStringField(TEXT("Description"), Connector.Settings.Description);
			}
			Connectors.Add(MakeShared<FJsonValueObject>(Item));
		}
		Row->SetArrayField(TEXT("Connectors"), Connectors);
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
		return Call.Args.Fail(TEXT("ControlRig.UnknownCatalogKind"), Error, TEXT("kind"));
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
} // namespace UE::AssetDocument::Graph::Rig::ControlRig
