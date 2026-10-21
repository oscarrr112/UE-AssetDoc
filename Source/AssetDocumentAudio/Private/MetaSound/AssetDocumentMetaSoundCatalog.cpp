// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Registered MetaSound node classes, data types and interfaces (design §3.6), for list_types
// (kind=metasound_node / metasound_datatype / metasound_interface, AssetDocumentTypeListers.h) and for the
// audio.metasound.catalog.v1 primitive ({kind: node|datatype|interface, name?, path?, offset?, limit?} ->
// {total, next_offset, rows[]}, the same rows). Sources: ISearchEngine::FindAllClasses (highest version
// of each class; data-type member classes such as inputs, outputs, literals and variables are left out),
// IDataTypeRegistry::IterateDataTypeInfo, ISearchEngine::FindAllInterfaces. Nothing is registered.
//
// A node family, classes "<namespace>.<name>.<variant>" that differ only in the data type their variant names
// ("Array.Random Get.Float:Array", "...Int32:Array", ...), is one row: "<T>" in its ClassName, DisplayName and pins
// stands for each of its "Variants" ("Float", "Int32", ...). A filter matching one class lists it whole.

#include "AssetDocumentMetaSound.h"

#include "AssetDocumentAudioPrimitives.h"
#include "MetasoundAssetKey.h"
#include "MetasoundAssetManager.h"
#include "MetasoundFrontendDataTypeRegistry.h"
#include "MetasoundFrontendSearchEngine.h"

namespace UE::AssetDocument::Graph::Audio::MetaSound
{
namespace
{
FString QueryValue(const TMap<FString, FString>& Query, const TCHAR* Key)
{
	const FString* Value = Query.Find(Key);
	return Value != nullptr ? Value->TrimStartAndEnd() : FString();
}

bool Matches(const FString& Path, const FString& DisplayName, const TMap<FString, FString>& Query)
{
	const FString Pattern = QueryValue(Query, TEXT("name"));
	if (!Pattern.IsEmpty())
	{
		const bool bWildcard = Pattern.Contains(TEXT("*")) || Pattern.Contains(TEXT("?"));
		auto Test = [&Pattern, bWildcard](const FString& Text)
		{
			return bWildcard ? Text.MatchesWildcard(Pattern, ESearchCase::IgnoreCase) : Text.Contains(Pattern, ESearchCase::IgnoreCase);
		};
		if (!Test(Path) && !Test(DisplayName))
		{
			return false;
		}
	}
	const FString Prefix = QueryValue(Query, TEXT("path"));
	return Prefix.IsEmpty() || Path.StartsWith(Prefix, ESearchCase::IgnoreCase);
}

TArray<TSharedPtr<FJsonValue>> StringValues(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const FString& Value : Values)
	{
		Out.Add(MakeShared<FJsonValueString>(Value));
	}
	return Out;
}

FString JoinTexts(const TArray<FText>& Texts, const TCHAR* Separator)
{
	TArray<FString> Values;
	for (const FText& Text : Texts)
	{
		Values.Add(Text.ToString());
	}
	return FString::Join(Values, Separator);
}

// "Name: Type" / "Name: Type = default" (the default as compact document JSON).
FString DescribeInput(const FMetasoundFrontendClassInput& Input)
{
	FString Text = FString::Printf(TEXT("%s: %s"), *Input.Name.ToString(), *Input.TypeName.ToString());
	if (Input.AccessType == EMetasoundFrontendVertexAccessType::Value)
	{
		Text += TEXT(" (constructor)");
	}
	if (const FMetasoundFrontendLiteral* Default = Input.FindConstDefault(Metasound::Frontend::DefaultPageID))
	{
		const FValue Value = EncodeLiteral(*Default, Input.TypeName);
		if (Value.Kind != EAssetDocumentRuntimeValueKind::Null)
		{
			Text += TEXT(" = ") + ValueToJsonString(Value, false);
		}
	}
	return Text;
}

FString DescribeOutput(const FMetasoundFrontendClassOutput& Output)
{
	FString Text = FString::Printf(TEXT("%s: %s"), *Output.Name.ToString(), *Output.TypeName.ToString());
	if (Output.AccessType == EMetasoundFrontendVertexAccessType::Value)
	{
		Text += TEXT(" (constructor)");
	}
	return Text;
}

bool IsListedNodeClass(EMetasoundFrontendClassType Type)
{
	return Type == EMetasoundFrontendClassType::External || Type == EMetasoundFrontendClassType::Template
		|| Type == EMetasoundFrontendClassType::Graph;
}

} // namespace

// Node families (named per file: unity builds, KI-8).
namespace MetaSoundCatalogFamilies
{
struct FPin
{
	FString Name;
	FString Type;
	FString Text;
	bool bConstructor = false;
};

struct FNodeRow
{
	TSharedPtr<FJsonObject> Row;
	FString Family;   // the class name without its variant ("Array.Random Get"); empty: never in a family
	FString Variant;  // its last segment ("Float:Array")
	FString DisplayName;
	TArray<FPin> Inputs;
	TArray<FPin> Outputs;
};

// The data type a variant names: "Float:Array" and "Float" name Float.
FString VariantType(const FString& Variant)
{
	return Variant.EndsWith(TEXT(":Array")) ? Variant.LeftChop(6) : Variant;
}

// A type written with T for the variant's data type ("<T>", "<T>:Array"); empty when the type is not the variant's.
FString Templated(const FString& Type, const FString& Variant)
{
	const FString T = VariantType(Variant);
	if (Type == T)
	{
		return TEXT("<T>");
	}
	return Type == T + TEXT(":Array") ? FString(TEXT("<T>:Array")) : FString();
}

// The key rows of one family share: everything but the variant and the types of its pins.
FString FamilyKey(const FNodeRow& Node)
{
	FString Key = Node.Family + TEXT("\n") + Templated(Node.Variant, Node.Variant);
	for (const TCHAR* Field : {TEXT("Version"), TEXT("Minor"), TEXT("Category"), TEXT("Description"), TEXT("Deprecated"), TEXT("Template")})
	{
		FString Value;
		Node.Row->TryGetStringField(Field, Value);
		Key += TEXT("\n") + Value;
	}
	for (const TArray<FPin>* Pins : {&Node.Inputs, &Node.Outputs})
	{
		Key += TEXT("\n");
		for (const FPin& Pin : *Pins)
		{
			Key += Pin.Name + (Pin.bConstructor ? TEXT("(c);") : TEXT(";"));
		}
	}
	return Key;
}

// The family's pins: a pin the same in every row stays whole; one whose type is each row's variant type is written
// with T (without a default, which is per type). False when a pin differs otherwise.
bool TemplatedPins(const TArray<const FNodeRow*>& Members, const TArray<FPin> FNodeRow::*Pins, TArray<FString>& OutPins)
{
	const TArray<FPin>& First = Members[0]->*Pins;
	for (int32 Index = 0; Index < First.Num(); ++Index)
	{
		bool bSame = true;
		for (const FNodeRow* Member : Members)
		{
			bSame &= (Member->*Pins)[Index].Text == First[Index].Text;
		}
		if (bSame)
		{
			OutPins.Add(First[Index].Text);
			continue;
		}
		const FString Type = Templated(First[Index].Type, Members[0]->Variant);
		if (Type.IsEmpty())
		{
			return false;
		}
		for (const FNodeRow* Member : Members)
		{
			if (Templated((Member->*Pins)[Index].Type, Member->Variant) != Type)
			{
				return false;
			}
		}
		OutPins.Add(FString::Printf(TEXT("%s: %s%s"), *First[Index].Name, *Type, First[Index].bConstructor ? TEXT(" (constructor)") : TEXT("")));
	}
	return true;
}

TArray<FString> PinTexts(const TArray<FPin>& Pins)
{
	TArray<FString> Texts;
	for (const FPin& Pin : Pins)
	{
		Texts.Add(Pin.Text);
	}
	return Texts;
}

// The family row, or null when the rows are not one family.
TSharedPtr<FJsonObject> FamilyRow(const TArray<const FNodeRow*>& Members)
{
	const FNodeRow& First = *Members[0];
	const FString TemplatedVariant = Templated(First.Variant, First.Variant);
	const FString DisplayName = First.DisplayName.Replace(*First.Variant, *TemplatedVariant, ESearchCase::CaseSensitive);
	TArray<FString> Variants;
	for (const FNodeRow* Member : Members)
	{
		if (Member->DisplayName.Replace(*Member->Variant, *TemplatedVariant, ESearchCase::CaseSensitive) != DisplayName)
		{
			return nullptr;
		}
		Variants.Add(VariantType(Member->Variant));
	}
	TArray<FString> Inputs;
	TArray<FString> Outputs;
	if (!TemplatedPins(Members, &FNodeRow::Inputs, Inputs) || !TemplatedPins(Members, &FNodeRow::Outputs, Outputs))
	{
		return nullptr;
	}
	Variants.Sort();
	const FString ClassName = First.Family + TEXT(".") + TemplatedVariant;
	TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
	Row->Values = First.Row->Values;
	Row->SetStringField(TEXT("Path"), ClassName);
	Row->SetStringField(TEXT("Class"), ClassName);
	Row->SetStringField(TEXT("ClassName"), ClassName);
	Row->SetStringField(TEXT("DisplayName"), DisplayName);
	Row->SetArrayField(TEXT("Variants"), StringValues(Variants));
	Row->SetArrayField(TEXT("Inputs"), StringValues(Inputs));
	Row->SetArrayField(TEXT("Outputs"), StringValues(Outputs));
	return Row;
}

TArray<TSharedPtr<FJsonObject>> CollapseFamilies(const TArray<FNodeRow>& Rows)
{
	TMap<FString, TArray<const FNodeRow*>> Families;
	for (const FNodeRow& Node : Rows)
	{
		if (!Node.Family.IsEmpty() && !Templated(Node.Variant, Node.Variant).IsEmpty())
		{
			Families.FindOrAdd(FamilyKey(Node)).Add(&Node);
		}
	}
	TArray<TSharedPtr<FJsonObject>> Out;
	TSet<const FNodeRow*> InFamily;
	for (const TPair<FString, TArray<const FNodeRow*>>& Family : Families)
	{
		if (Family.Value.Num() < 2)
		{
			continue;
		}
		if (TSharedPtr<FJsonObject> Row = FamilyRow(Family.Value))
		{
			Out.Add(Row);
			InFamily.Append(Family.Value);
		}
	}
	for (const FNodeRow& Node : Rows)
	{
		if (!InFamily.Contains(&Node))
		{
			Node.Row->SetArrayField(TEXT("Inputs"), StringValues(PinTexts(Node.Inputs)));
			Node.Row->SetArrayField(TEXT("Outputs"), StringValues(PinTexts(Node.Outputs)));
			Out.Add(Node.Row);
		}
	}
	return Out;
}
} // namespace MetaSoundCatalogFamilies

namespace
{
bool CollectRows(FStringView Kind, const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError)
{
	if (Kind == TEXTVIEW("node"))
	{
		return ListNodeTypes(Query, OutRows, OutError);
	}
	if (Kind == TEXTVIEW("datatype"))
	{
		return ListDataTypes(Query, OutRows, OutError);
	}
	if (Kind == TEXTVIEW("interface"))
	{
		return ListInterfaces(Query, OutRows, OutError);
	}
	OutError = FString::Printf(TEXT("kind must be node, datatype or interface, got '%s'."), *FString(Kind));
	return false;
}
} // namespace

bool ListNodeTypes(const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError)
{
	using namespace Metasound::Frontend;
	using namespace MetaSoundCatalogFamilies;
	const IMetaSoundAssetManager* Assets = IMetaSoundAssetManager::Get();
	TArray<FNodeRow> Rows;
	for (const FMetasoundFrontendClass& Class : FindAllRegisteredClasses())
	{
		const FMetasoundFrontendClassMetadata& Metadata = Class.Metadata;
		if (!IsListedNodeClass(Metadata.GetType()))
		{
			continue;
		}
		const FString FullName = GetClassFullName(Metadata.GetClassName());
		FString Path = FullName;
		FString AssetPath;
		if (Assets != nullptr && Assets->IsAssetClass(Metadata))
		{
			const FTopLevelAssetPath Found = Assets->FindAssetPath(FMetaSoundAssetKey(Metadata));
			if (Found.IsValid())
			{
				AssetPath = Found.ToString();
				Path = AssetPath;
			}
		}
		const FString DisplayName = Metadata.GetDisplayName().ToString();
		if (!Matches(Path, DisplayName, Query))
		{
			continue;
		}
		FNodeRow& Node = Rows.AddDefaulted_GetRef();
		Node.Row = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject>& Row = Node.Row;
		Row->SetStringField(TEXT("Path"), Path);
		Row->SetStringField(TEXT("Class"), AssetPath.IsEmpty() ? FullName : AssetPath);
		Row->SetStringField(TEXT("ClassName"), FullName);
		Row->SetNumberField(TEXT("Version"), Metadata.GetVersion().Major);
		Row->SetNumberField(TEXT("Minor"), Metadata.GetVersion().Minor);
		Row->SetStringField(TEXT("DisplayName"), DisplayName);
		Row->SetStringField(TEXT("Category"), JoinTexts(Metadata.GetCategoryHierarchy(), TEXT("|")));
		if (!Metadata.GetDescription().IsEmpty())
		{
			Row->SetStringField(TEXT("Description"), Metadata.GetDescription().ToString());
		}
		const EMetasoundFrontendClassAccessFlags Flags = Metadata.GetAccessFlags();
		if (EnumHasAnyFlags(Flags, EMetasoundFrontendClassAccessFlags::Deprecated))
		{
			Row->SetBoolField(TEXT("Deprecated"), true);
		}
		if (Metadata.GetType() == EMetasoundFrontendClassType::Template)
		{
			Row->SetBoolField(TEXT("Template"), true);
		}
		const FMetasoundFrontendClassInterface& Interface = Class.GetDefaultInterface();
		for (const FMetasoundFrontendClassInput& Input : Interface.Inputs)
		{
			Node.Inputs.Add({Input.Name.ToString(), Input.TypeName.ToString(), DescribeInput(Input),
				Input.AccessType == EMetasoundFrontendVertexAccessType::Value});
		}
		for (const FMetasoundFrontendClassOutput& Output : Interface.Outputs)
		{
			Node.Outputs.Add({Output.Name.ToString(), Output.TypeName.ToString(), DescribeOutput(Output),
				Output.AccessType == EMetasoundFrontendVertexAccessType::Value});
		}
		const FString VariantName = Metadata.GetClassName().Variant.ToString();
		if (AssetPath.IsEmpty() && !VariantName.IsEmpty() && FullName.EndsWith(TEXT(".") + VariantName))
		{
			Node.Family = FullName.LeftChop(VariantName.Len() + 1);
			Node.Variant = VariantName;
			Node.DisplayName = DisplayName;
		}
	}
	OutRows.Append(CollapseFamilies(Rows));
	return true;
}

bool ListDataTypes(const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError)
{
	using namespace Metasound::Frontend;
	IDataTypeRegistry& Registry = IDataTypeRegistry::Get();
	Registry.IterateDataTypeInfo([&](const FDataTypeRegistryInfo& Info)
	{
		const FString Name = Info.DataTypeName.ToString();
		const FString DisplayName = Info.DataTypeDisplayText.ToString();
		if (!Matches(Name, DisplayName, Query))
		{
			return;
		}
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Path"), Name);
		Row->SetStringField(TEXT("DisplayName"), DisplayName);
		Row->SetStringField(TEXT("Literal"), DescribeLiteralForm(Info.DataTypeName));
		Row->SetBoolField(TEXT("Array"), Info.bIsArrayType);
		Row->SetBoolField(TEXT("Constructor"), Info.bIsConstructorType);
		Row->SetBoolField(TEXT("Variable"), Info.bIsVariable);
		if (Info.ProxyGeneratorClass != nullptr)
		{
			Row->SetStringField(TEXT("ObjectClass"), Info.ProxyGeneratorClass->GetPathName());
		}
		if (const TSharedPtr<const IEnumDataTypeInterface> Enum = Registry.GetEnumInterfaceForDataType(Info.DataTypeName))
		{
			TArray<FString> Entries;
			FString DefaultEntry;
			for (const IEnumDataTypeInterface::FGenericInt32Entry& Entry : Enum->GetAllEntries())
			{
				FString EntryName = Entry.Name.ToString();
				int32 Separator = INDEX_NONE;
				if (EntryName.FindLastChar(TEXT(':'), Separator))
				{
					EntryName.RightChopInline(Separator + 1);
				}
				if (Entry.Value == Enum->GetDefaultValue())
				{
					DefaultEntry = EntryName;
				}
				Entries.Add(EntryName);
			}
			Row->SetArrayField(TEXT("Enum"), StringValues(Entries));
			Row->SetStringField(TEXT("Default"), DefaultEntry);
		}
		OutRows.Add(Row);
	});
	return true;
}

bool ListInterfaces(const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError)
{
	using namespace Metasound::Frontend;
	for (FMetasoundFrontendInterface& Interface : FindAllRegisteredInterfaces())
	{
		const FString Name = Interface.Metadata.Version.Name.ToString();
		if (!Matches(Name, Name, Query))
		{
			continue;
		}
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Path"), Name);
		Row->SetStringField(TEXT("Version"), Interface.Metadata.Version.Number.ToString());
		TArray<FString> Inputs;
		TArray<FString> Outputs;
		TArray<FString> Required;
		for (const FMetasoundFrontendClassInput& Input : Interface.Inputs)
		{
			Inputs.Add(DescribeInput(Input));
		}
		for (const FMetasoundFrontendClassOutput& Output : Interface.Outputs)
		{
			Outputs.Add(DescribeOutput(Output));
			FText Reason;
			if (Interface.IsMemberOutputRequired(Output.Name, Reason))
			{
				Required.Add(Output.Name.ToString());
			}
		}
		Row->SetArrayField(TEXT("Inputs"), StringValues(Inputs));
		Row->SetArrayField(TEXT("Outputs"), StringValues(Outputs));
		if (!Required.IsEmpty())
		{
			Row->SetArrayField(TEXT("RequiredOutputs"), StringValues(Required));
		}
		TArray<TSharedPtr<FJsonValue>> Classes;
		for (const FMetasoundFrontendInterfaceUClassOptions& Options : Interface.Metadata.UClassOptions)
		{
			TSharedPtr<FJsonObject> Option = MakeShared<FJsonObject>();
			Option->SetStringField(TEXT("Class"), Options.ClassPath.ToString());
			Option->SetBoolField(TEXT("Modifiable"), Options.bIsModifiable);
			Option->SetBoolField(TEXT("Default"), Options.bIsDefault);
			Classes.Add(MakeShared<FJsonValueObject>(Option));
		}
		// No options: any MetaSound class may add or remove the interface.
		Row->SetArrayField(TEXT("Classes"), Classes);
		OutRows.Add(Row);
	}
	return true;
}
} // namespace UE::AssetDocument::Graph::Audio::MetaSound

namespace UE::AssetDocument::Graph::Audio
{
bool HandleMetaSoundCatalog(FCall& Call, FObjectBuilder& Result)
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
	if (!MetaSound::CollectRows(Kind, Query, Rows, Error))
	{
		return Call.Args.Fail(TEXT("MetaSound.UnknownCatalogKind"), Error, TEXT("kind"));
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
} // namespace UE::AssetDocument::Graph::Audio
