// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Literal codec, class / page names and interface facts shared by the MetaSound read, catalog and
// compile status (design §3.4, §3.2).

#include "AssetDocumentMetaSound.h"

#include "AssetDocumentAudioPrimitives.h"
#include "Interfaces/MetasoundFrontendInterfaceRegistry.h"
#include "MetasoundAssetKey.h"
#include "MetasoundAssetManager.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundFrontendDataTypeRegistry.h"
#include "MetasoundFrontendDocumentBuilder.h"
#include "MetasoundFrontendNodeClassRegistry.h"
#include "MetasoundFrontendPages.h"
#include "MetasoundFrontendSearchEngine.h"
#include "MetasoundSettings.h"
#include "Misc/EngineVersionComparison.h"
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
#include "DocumentTemplates/MetasoundFrontendPresetTemplate.h"
#endif
#include "Algo/AllOf.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::Graph::Audio::MetaSound
{
namespace
{
using Metasound::ELiteralType;

const TCHAR* ArraySuffix = TEXT(":Array");

// The scalar literal kind a data type wants (Float for "Float:Array").
ELiteralType GetDesiredElementType(FName DataType)
{
	switch (Metasound::Frontend::IDataTypeRegistry::Get().GetDesiredLiteralType(DataType))
	{
	case ELiteralType::None:
	case ELiteralType::NoneArray:
		return ELiteralType::None;
	case ELiteralType::Boolean:
	case ELiteralType::BooleanArray:
		return ELiteralType::Boolean;
	case ELiteralType::Integer:
	case ELiteralType::IntegerArray:
		return ELiteralType::Integer;
	case ELiteralType::Float:
	case ELiteralType::FloatArray:
		return ELiteralType::Float;
	case ELiteralType::String:
	case ELiteralType::StringArray:
		return ELiteralType::String;
	case ELiteralType::UObjectProxy:
	case ELiteralType::UObjectProxyArray:
		return ELiteralType::UObjectProxy;
	default:
		return ELiteralType::Invalid;
	}
}

FString EnumEntryName(FName EntryName)
{
	FString Name = EntryName.ToString();
	int32 Separator = INDEX_NONE;
	if (Name.FindLastChar(TEXT(':'), Separator))
	{
		Name.RightChopInline(Separator + 1);
	}
	return Name;
}

struct FElementCodec
{
	ELiteralType Desired = ELiteralType::Invalid;
	TSharedPtr<const Metasound::Frontend::IEnumDataTypeInterface> Enum;

	FValue Int(int32 Value) const
	{
		if (Enum.IsValid())
		{
			for (const Metasound::Frontend::IEnumDataTypeInterface::FGenericInt32Entry& Entry : Enum->GetAllEntries())
			{
				if (Entry.Value == Value)
				{
					return MakeString(EnumEntryName(Entry.Name));
				}
			}
			return MakeInt(Value);
		}
		if (Desired == ELiteralType::Float)
		{
			return MakeNumber(static_cast<double>(Value));
		}
		if (Desired == ELiteralType::Boolean)
		{
			return MakeBool(Value != 0);
		}
		return MakeInt(Value);
	}

	FValue Float(float Value) const
	{
		if (Desired == ELiteralType::Integer && FMath::IsFinite(Value) && FMath::RoundToFloat(Value) == Value && !Enum.IsValid())
		{
			return MakeInt(static_cast<int64>(Value));
		}
		return MakeNumber(ShortestFloat(Value));
	}

	FValue Bool(bool bValue) const
	{
		if (Desired == ELiteralType::Integer || Desired == ELiteralType::Float)
		{
			return Desired == ELiteralType::Integer ? MakeInt(bValue ? 1 : 0) : MakeNumber(bValue ? 1.0 : 0.0);
		}
		return MakeBool(bValue);
	}

	static FValue Object(const UObject* Object)
	{
		return Object != nullptr ? MakeString(Object->GetPathName()) : MakeNull();
	}
};

template <typename T, typename FEncode>
FValue EncodeArray(const FMetasoundFrontendLiteral& Literal, FEncode Encode)
{
	TArray<T> Values;
	Literal.TryGet(Values);
	TArray<FValue> Elements;
	Elements.Reserve(Values.Num());
	for (const T& Value : Values)
	{
		Elements.Add(Encode(Value));
	}
	return MakeArray(MoveTemp(Elements));
}
} // namespace

FName GetElementDataType(FName DataType, bool* bOutIsArray)
{
	FString Name = DataType.ToString();
	const bool bIsArray = Name.EndsWith(ArraySuffix, ESearchCase::CaseSensitive);
	if (bOutIsArray != nullptr)
	{
		*bOutIsArray = bIsArray;
	}
	if (!bIsArray)
	{
		return DataType;
	}
	Name.LeftChopInline(FCString::Strlen(ArraySuffix));
	return FName(*Name);
}

FValue EncodeLiteral(const FMetasoundFrontendLiteral& Literal, FName DataType)
{
	FElementCodec Codec;
	Codec.Desired = GetDesiredElementType(DataType);
	Codec.Enum = Metasound::Frontend::IDataTypeRegistry::Get().GetEnumInterfaceForDataType(GetElementDataType(DataType));
	switch (Literal.GetType())
	{
	case EMetasoundFrontendLiteralType::Boolean:
	{
		bool bValue = false;
		Literal.TryGet(bValue);
		return Codec.Bool(bValue);
	}
	case EMetasoundFrontendLiteralType::Integer:
	{
		int32 Value = 0;
		Literal.TryGet(Value);
		return Codec.Int(Value);
	}
	case EMetasoundFrontendLiteralType::Float:
	{
		float Value = 0.f;
		Literal.TryGet(Value);
		return Codec.Float(Value);
	}
	case EMetasoundFrontendLiteralType::String:
	{
		FString Value;
		Literal.TryGet(Value);
		return MakeString(MoveTemp(Value));
	}
	case EMetasoundFrontendLiteralType::UObject:
	{
		UObject* Value = nullptr;
		Literal.TryGet(Value);
		return FElementCodec::Object(Value);
	}
	case EMetasoundFrontendLiteralType::NoneArray:
	{
		TArray<FValue> Elements;
		Elements.Init(MakeNull(), Literal.GetArrayNum());
		return MakeArray(MoveTemp(Elements));
	}
	case EMetasoundFrontendLiteralType::BooleanArray:
		return EncodeArray<bool>(Literal, [&Codec](bool bValue) { return Codec.Bool(bValue); });
	case EMetasoundFrontendLiteralType::IntegerArray:
		return EncodeArray<int32>(Literal, [&Codec](int32 Value) { return Codec.Int(Value); });
	case EMetasoundFrontendLiteralType::FloatArray:
		return EncodeArray<float>(Literal, [&Codec](float Value) { return Codec.Float(Value); });
	case EMetasoundFrontendLiteralType::StringArray:
		return EncodeArray<FString>(Literal, [](const FString& Value) { return MakeString(Value); });
	case EMetasoundFrontendLiteralType::UObjectArray:
		return EncodeArray<UObject*>(Literal, [](const UObject* Value) { return FElementCodec::Object(Value); });
	case EMetasoundFrontendLiteralType::None:
	default:
		return MakeNull();
	}
}

namespace
{
using EValueKind = EAssetDocumentRuntimeValueKind;

FString DescribeValue(const FValue& Value)
{
	return ValueToJsonString(Value, false).Left(120);
}

struct FElementDecoder
{
	FName DataType;
	FName ElementType;
	ELiteralType Desired = ELiteralType::Invalid;
	TSharedPtr<const Metasound::Frontend::IEnumDataTypeInterface> Enum;
	UClass* ObjectClass = nullptr;

	bool Bool(const FValue& Value, bool& Out, FString& OutProblem) const
	{
		if (Value.Kind != EValueKind::Boolean)
		{
			OutProblem = FString::Printf(TEXT("%s takes true or false, not %s."), *DataType.ToString(), *DescribeValue(Value));
			return false;
		}
		Out = Value.BooleanValue;
		return true;
	}

	bool Int(const FValue& Value, int32& Out, FString& OutProblem) const
	{
		if (Enum.IsValid())
		{
			TArray<FString> Names;
			for (const Metasound::Frontend::IEnumDataTypeInterface::FGenericInt32Entry& Entry : Enum->GetAllEntries())
			{
				const FString Name = EnumEntryName(Entry.Name);
				if ((Value.Kind == EValueKind::String && (Value.StringValue == Name || Value.StringValue == Entry.Name.ToString()))
					|| (Value.Kind == EValueKind::Integer && Value.IntegerValue == Entry.Value))
				{
					Out = Entry.Value;
					return true;
				}
				Names.Add(Name);
			}
			OutProblem = FString::Printf(TEXT("%s takes one of %s, not %s."), *DataType.ToString(), *FString::Join(Names, TEXT(", ")), *DescribeValue(Value));
			return false;
		}
		double Number = 0.0;
		if (Value.Kind == EValueKind::Integer)
		{
			Number = static_cast<double>(Value.IntegerValue);
		}
		else if (Value.Kind == EValueKind::Number && FMath::IsFinite(Value.NumberValue) && FMath::RoundToDouble(Value.NumberValue) == Value.NumberValue)
		{
			Number = Value.NumberValue;
		}
		else
		{
			OutProblem = FString::Printf(TEXT("%s takes an integer, not %s."), *DataType.ToString(), *DescribeValue(Value));
			return false;
		}
		if (Number < static_cast<double>(MIN_int32) || Number > static_cast<double>(MAX_int32))
		{
			OutProblem = FString::Printf(TEXT("%s takes a 32-bit integer; %s is out of range."), *DataType.ToString(), *DescribeValue(Value));
			return false;
		}
		Out = static_cast<int32>(Number);
		return true;
	}

	bool Float(const FValue& Value, float& Out, FString& OutProblem) const
	{
		if (Value.Kind == EValueKind::Integer)
		{
			Out = static_cast<float>(Value.IntegerValue);
			return true;
		}
		if (Value.Kind == EValueKind::Number && FMath::IsFinite(Value.NumberValue))
		{
			Out = static_cast<float>(Value.NumberValue);
			return true;
		}
		OutProblem = FString::Printf(TEXT("%s takes a number, not %s."), *DataType.ToString(), *DescribeValue(Value));
		return false;
	}

	bool String(const FValue& Value, FString& Out, FString& OutProblem) const
	{
		if (Value.Kind != EValueKind::String)
		{
			OutProblem = FString::Printf(TEXT("%s takes a string, not %s."), *DataType.ToString(), *DescribeValue(Value));
			return false;
		}
		Out = Value.StringValue;
		return true;
	}

	bool Object(const FValue& Value, UObject*& Out, FString& OutProblem) const
	{
		Out = nullptr;
		if (Value.Kind == EValueKind::Null)
		{
			return true;
		}
		if (Value.Kind != EValueKind::String)
		{
			OutProblem = FString::Printf(TEXT("%s takes an object path or null, not %s."), *DataType.ToString(), *DescribeValue(Value));
			return false;
		}
		if (Value.StringValue.IsEmpty() || Value.StringValue == TEXT("None"))
		{
			return true;
		}
		Out = LoadObject<UObject>(nullptr, *Value.StringValue, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (Out == nullptr)
		{
			OutProblem = FString::Printf(TEXT("%s: no object '%s' (an object path such as /Game/Audio/Wave.Wave)."), *DataType.ToString(), *Value.StringValue);
			return false;
		}
		if (ObjectClass != nullptr && !Out->IsA(ObjectClass))
		{
			OutProblem = FString::Printf(TEXT("%s takes a %s; '%s' is a %s."), *DataType.ToString(), *ObjectClass->GetName(), *Value.StringValue, *Out->GetClass()->GetName());
			Out = nullptr;
			return false;
		}
		return true;
	}
};

template <typename T, typename FDecode>
bool DecodeArray(const FValue& Value, FName DataType, FMetasoundFrontendLiteral& OutLiteral, FString& OutProblem, FDecode Decode)
{
	if (Value.Kind != EValueKind::Array)
	{
		OutProblem = FString::Printf(TEXT("%s takes an array, not %s."), *DataType.ToString(), *DescribeValue(Value));
		return false;
	}
	TArray<T> Values;
	Values.Reserve(Value.Elements.Num());
	for (int32 Index = 0; Index < Value.Elements.Num(); ++Index)
	{
		T Element{};
		if (!Decode(Value.Elements[Index], Element, OutProblem))
		{
			OutProblem = FString::Printf(TEXT("[%d] %s"), Index, *OutProblem);
			return false;
		}
		Values.Add(Element);
	}
	OutLiteral.Set(Values);
	return true;
}
} // namespace

bool DecodeLiteral(const FValue& Value, FName DataType, FMetasoundFrontendLiteral& OutLiteral, FString& OutProblem)
{
	using namespace Metasound::Frontend;
	IDataTypeRegistry& Registry = IDataTypeRegistry::Get();
	if (!Registry.IsRegistered(DataType))
	{
		OutProblem = FString::Printf(TEXT("'%s' is not a registered MetaSound data type."), *DataType.ToString());
		return false;
	}
	bool bIsArray = false;
	FElementDecoder Decoder;
	Decoder.DataType = DataType;
	Decoder.ElementType = GetElementDataType(DataType, &bIsArray);
	Decoder.Desired = GetDesiredElementType(DataType);
	Decoder.Enum = Registry.GetEnumInterfaceForDataType(Decoder.ElementType);
	if (Decoder.Desired == ELiteralType::UObjectProxy)
	{
		Decoder.ObjectClass = Registry.GetUClassForDataType(Decoder.ElementType);
	}
	OutLiteral = FMetasoundFrontendLiteral();
	switch (Decoder.Desired)
	{
	case ELiteralType::None:
		if (Value.Kind == EValueKind::Null)
		{
			return true;
		}
		if (bIsArray && Value.Kind == EValueKind::Array && Algo::AllOf(Value.Elements, [](const FValue& Element) { return Element.Kind == EValueKind::Null; }))
		{
			OutLiteral.Set(FMetasoundFrontendLiteral::FDefaultArray{Value.Elements.Num()});
			return true;
		}
		OutProblem = FString::Printf(TEXT("%s takes no value (null)."), *DataType.ToString());
		return false;
	case ELiteralType::Boolean:
		if (bIsArray)
		{
			return DecodeArray<bool>(Value, DataType, OutLiteral, OutProblem, [&Decoder](const FValue& Element, bool& Out, FString& Problem) { return Decoder.Bool(Element, Out, Problem); });
		}
		else
		{
			bool bValue = false;
			if (!Decoder.Bool(Value, bValue, OutProblem))
			{
				return false;
			}
			OutLiteral.Set(bValue);
			return true;
		}
	case ELiteralType::Integer:
		if (bIsArray)
		{
			return DecodeArray<int32>(Value, DataType, OutLiteral, OutProblem, [&Decoder](const FValue& Element, int32& Out, FString& Problem) { return Decoder.Int(Element, Out, Problem); });
		}
		else
		{
			int32 IntValue = 0;
			if (!Decoder.Int(Value, IntValue, OutProblem))
			{
				return false;
			}
			OutLiteral.Set(IntValue);
			return true;
		}
	case ELiteralType::Float:
		if (bIsArray)
		{
			return DecodeArray<float>(Value, DataType, OutLiteral, OutProblem, [&Decoder](const FValue& Element, float& Out, FString& Problem) { return Decoder.Float(Element, Out, Problem); });
		}
		else
		{
			float FloatValue = 0.f;
			if (!Decoder.Float(Value, FloatValue, OutProblem))
			{
				return false;
			}
			OutLiteral.Set(FloatValue);
			return true;
		}
	case ELiteralType::String:
		if (bIsArray)
		{
			return DecodeArray<FString>(Value, DataType, OutLiteral, OutProblem, [&Decoder](const FValue& Element, FString& Out, FString& Problem) { return Decoder.String(Element, Out, Problem); });
		}
		else
		{
			FString StringValue;
			if (!Decoder.String(Value, StringValue, OutProblem))
			{
				return false;
			}
			OutLiteral.Set(StringValue);
			return true;
		}
	case ELiteralType::UObjectProxy:
		if (bIsArray)
		{
			return DecodeArray<UObject*>(Value, DataType, OutLiteral, OutProblem, [&Decoder](const FValue& Element, UObject*& Out, FString& Problem) { return Decoder.Object(Element, Out, Problem); });
		}
		else
		{
			UObject* ObjectValue = nullptr;
			if (!Decoder.Object(Value, ObjectValue, OutProblem))
			{
				return false;
			}
			OutLiteral.Set(ObjectValue);
			return true;
		}
	default:
		OutProblem = FString::Printf(TEXT("%s has no literal form."), *DataType.ToString());
		return false;
	}
}

bool LiteralsEqual(const FMetasoundFrontendLiteral& A, const FMetasoundFrontendLiteral& B, FName DataType)
{
	return ValueToJsonString(EncodeLiteral(A, DataType), false) == ValueToJsonString(EncodeLiteral(B, DataType), false);
}

const FMetasoundFrontendLiteral* FindNodeInputClassDefault(const FMetaSoundFrontendDocumentBuilder& Builder, const FGuid& NodeID,
	const FGuid& VertexID, const FGuid* PageID)
{
	const FMetasoundFrontendNode* Node = Builder.FindNode(NodeID, PageID);
	if (Node == nullptr)
	{
		return nullptr;
	}
	const FMetasoundFrontendVertex* Vertex = Node->Interface.Inputs.FindByPredicate(
		[&VertexID](const FMetasoundFrontendVertex& Candidate) { return Candidate.VertexID == VertexID; });
	const FMetasoundFrontendClass* Class = Vertex != nullptr ? Builder.FindDependency(Node->ClassID) : nullptr;
	if (Class == nullptr)
	{
		return nullptr;
	}
	const FMetasoundFrontendClassInterface& ClassInterface = Class->GetInterfaceForNode(*Node);
	if (Class->Metadata.GetType() == EMetasoundFrontendClassType::Output)
	{
		return ClassInterface.Inputs.IsEmpty() ? nullptr : ClassInterface.Inputs.Last().FindConstDefault(Metasound::Frontend::DefaultPageID);
	}
	const FMetasoundFrontendClassInput* ClassInput = ClassInterface.Inputs.FindByPredicate(
		[Name = Vertex->Name](const FMetasoundFrontendClassInput& Candidate) { return Candidate.Name == Name; });
	return ClassInput != nullptr ? ClassInput->FindConstDefault(Metasound::Frontend::DefaultPageID) : nullptr;
}

TArray<FMetasoundFrontendClass> FindAllRegisteredClasses()
{
	using namespace Metasound::Frontend;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	TArray<FMetasoundFrontendClass> Classes;
	for (const FMetaSoundClassInfo& Info : ISearchEngine::Get().FindAllClasses(ISearchEngine::EResultVersion::Highest))
	{
		FMetasoundFrontendClass Class;
		if (INodeClassRegistry::Get()->FindFrontendClassFromRegistered(FNodeClassRegistryKey(Info.ClassType, Info.ClassName, Info.Version), Class))
		{
			Classes.Add(MoveTemp(Class));
		}
	}
	return Classes;
#else
	constexpr bool bIncludeAllVersions = false;
	return ISearchEngine::Get().FindAllClasses(bIncludeAllVersions);
#endif
}

bool FindRegisteredNodeClass(const FMetasoundFrontendClassName& ClassName, int32 Major, FMetasoundFrontendClass& OutClass)
{
	using namespace Metasound::Frontend;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	FMetaSoundClassInfo Info;
	return ISearchEngine::Get().FindRegisteredClass(ClassName, Major, Info)
		&& INodeClassRegistry::Get()->FindFrontendClassFromRegistered(FNodeClassRegistryKey(Info.ClassType, Info.ClassName, Info.Version), OutClass);
#else
	return ISearchEngine::Get().FindClassWithHighestMinorVersion(ClassName, Major, OutClass);
#endif
}

TArray<FMetasoundFrontendInterface> FindAllRegisteredInterfaces()
{
	using namespace Metasound::Frontend;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	TArray<FMetasoundFrontendInterface> Interfaces;
	for (const FMetasoundFrontendVersion& Version : ISearchEngine::Get().FindAllInterfaceVersions())
	{
		FMetasoundFrontendInterface Interface;
		if (IInterfaceRegistry::Get().FindInterface(Version, Interface))
		{
			Interfaces.Add(MoveTemp(Interface));
		}
	}
	return Interfaces;
#else
	return ISearchEngine::Get().FindAllInterfaces();
#endif
}

bool FindRegisteredInterface(const FMetasoundFrontendVersion& Version, FMetasoundFrontendInterface& OutInterface)
{
	using namespace Metasound::Frontend;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	return IInterfaceRegistry::Get().FindInterface(Version, OutInterface);
#else
	return IInterfaceRegistry::Get().FindInterface(GetInterfaceRegistryKey(Version), OutInterface);
#endif
}

TSet<FName> GetInputsInheritingDefault(const FMetaSoundFrontendDocumentBuilder& Builder)
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	// The preset template keeps one vertex metadata per inheritable input; an input inherits unless it overrides.
	// Inputs without one (triggers, inputs without a default value) have no value of their own to override and are
	// reported as inheriting.
	TSet<FName> Names;
	if (const FMetaSoundFrontendPresetTemplate* Template = Builder.GetConstTemplateAs<FMetaSoundFrontendPresetTemplate>())
	{
		for (const FMetasoundFrontendClassInput& Input : Builder.GetConstDocumentChecked().RootGraph.GetDefaultInterface().Inputs)
		{
			const FMetaSoundFrontendPresetVertexMetadata* Metadata =
				Template->FindConstVertexMetadata<FMetaSoundFrontendPresetVertexMetadata>(Input.NodeID);
			if (Metadata == nullptr || !Metadata->bOverrideInheritedDefault)
			{
				Names.Add(Input.Name);
			}
		}
	}
	return Names;
#else
	const TSet<FName>* Names = Builder.GetGraphInputsInheritingDefault();
	return Names != nullptr ? *Names : TSet<FName>();
#endif
}

bool SetInputInheritsDefault(FMetaSoundFrontendDocumentBuilder& Builder, FName InputName, bool bInherits)
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	// As the editor does it: resetting the input's default makes it inherit again, writing a default makes it override.
	const FMetasoundFrontendClassInput* Input = Builder.FindGraphInput(InputName);
	if (Input == nullptr || GetInputsInheritingDefault(Builder).Contains(InputName) == bInherits)
	{
		return false;
	}
	if (bInherits)
	{
		return Builder.ResetGraphInputDefault(InputName);
	}
	TArray<FMetasoundFrontendClassInputDefault> Defaults = Input->GetDefaults();
	return Builder.SetGraphInputDefaults(InputName, MoveTemp(Defaults));
#else
	return Builder.SetGraphInputInheritsDefault(InputName, bInherits);
#endif
}

FString DescribeLiteralForm(FName DataType)
{
	bool bIsArray = false;
	const FName Element = GetElementDataType(DataType, &bIsArray);
	FString Form;
	if (Metasound::Frontend::IDataTypeRegistry::Get().GetEnumInterfaceForDataType(Element).IsValid())
	{
		Form = TEXT("enum");
	}
	else
	{
		switch (GetDesiredElementType(DataType))
		{
		case ELiteralType::Boolean: Form = TEXT("bool"); break;
		case ELiteralType::Integer: Form = TEXT("integer"); break;
		case ELiteralType::Float: Form = TEXT("number"); break;
		case ELiteralType::String: Form = TEXT("string"); break;
		case ELiteralType::UObjectProxy: Form = TEXT("object"); break;
		case ELiteralType::None: Form = TEXT("none"); break;
		default: Form = TEXT("invalid"); break;
		}
	}
	return bIsArray ? Form + TEXT("[]") : Form;
}

FString GetClassFullName(const FMetasoundFrontendClassName& ClassName)
{
	return ClassName.ToString();
}

FAssetClassPaths::FAssetClassPaths(const UObject& Asset)
{
	const FSetProperty* Property = FindFProperty<FSetProperty>(Asset.GetClass(), TEXT("ReferencedAssetClassObjects"));
	const FObjectPropertyBase* Element = Property != nullptr ? CastField<FObjectPropertyBase>(Property->ElementProp) : nullptr;
	if (Element == nullptr)
	{
		return;
	}
	FScriptSetHelper Helper(Property, Property->ContainerPtrToValuePtr<void>(&Asset));
	for (FScriptSetHelper::FIterator It(Helper); It; ++It)
	{
		const UObject* Referenced = Element->GetObjectPropertyValue(Helper.GetElementPtr(It));
		const IMetaSoundDocumentInterface* Document = Referenced != nullptr ? AsMetaSound(*Referenced) : nullptr;
		if (Document != nullptr)
		{
			PathsByClassName.Add(GetClassFullName(Document->GetConstDocument().RootGraph.Metadata.GetClassName()), Referenced->GetPathName());
		}
	}
}

FString FAssetClassPaths::Find(const FMetasoundFrontendClassMetadata& Metadata) const
{
	if (const FString* Path = PathsByClassName.Find(GetClassFullName(Metadata.GetClassName())))
	{
		return *Path;
	}
	using Metasound::Frontend::IMetaSoundAssetManager;
	if (const IMetaSoundAssetManager* Manager = IMetaSoundAssetManager::Get())
	{
		if (Manager->IsAssetClass(Metadata))
		{
			const FTopLevelAssetPath Path = Manager->FindAssetPath(FMetaSoundAssetKey(Metadata));
			if (Path.IsValid())
			{
				return Path.ToString();
			}
		}
	}
	return FString();
}

TInstancedStruct<FMetaSoundFrontendNodeConfiguration> MakeDefaultNodeConfiguration(const FMetasoundFrontendClassMetadata& Metadata)
{
	using namespace Metasound::Frontend;
	if (Metadata.GetType() != EMetasoundFrontendClassType::External)
	{
		return {};
	}
	INodeClassRegistry* Registry = INodeClassRegistry::Get();
	return Registry != nullptr ? Registry->CreateFrontendNodeConfiguration(FNodeClassRegistryKey(Metadata)) : TInstancedStruct<FMetaSoundFrontendNodeConfiguration>();
}

FString GetPageName(const FGuid& PageID)
{
	if (PageID == Metasound::Frontend::DefaultPageID)
	{
		return Metasound::Frontend::DefaultPageName;
	}
	if (const UMetaSoundSettings* Settings = GetDefault<UMetaSoundSettings>())
	{
		if (const FMetaSoundPageSettings* Page = Settings->FindPageSettings(PageID))
		{
			return Page->Name.ToString();
		}
	}
	return PageID.ToString(EGuidFormats::Digits);
}

TArray<FString> GetConfiguredPageNames()
{
	TArray<FString> Names;
	Names.Add(Metasound::Frontend::DefaultPageName);
	if (const UMetaSoundSettings* Settings = GetDefault<UMetaSoundSettings>())
	{
		for (const FMetaSoundPageSettings& Page : Settings->GetProjectPageSettings())
		{
			Names.Add(Page.Name.ToString());
		}
	}
	return Names;
}

bool FindPageID(const FString& Name, FGuid& OutPageID, bool* bOutConfigured)
{
	if (bOutConfigured != nullptr)
	{
		*bOutConfigured = true;
	}
	if (Name == Metasound::Frontend::DefaultPageName)
	{
		OutPageID = Metasound::Frontend::DefaultPageID;
		return true;
	}
	if (const UMetaSoundSettings* Settings = GetDefault<UMetaSoundSettings>())
	{
		for (const FMetaSoundPageSettings& Page : Settings->GetProjectPageSettings())
		{
			if (Page.Name.ToString() == Name)
			{
				OutPageID = Page.UniqueId;
				return true;
			}
		}
	}
	FGuid Parsed;
	if (Name.Len() == 32 && FGuid::ParseExact(Name, EGuidFormats::Digits, Parsed) && GetPageName(Parsed) == Name)
	{
		OutPageID = Parsed;
		if (bOutConfigured != nullptr)
		{
			*bOutConfigured = false;
		}
		return true;
	}
	return false;
}

TArray<FGuid> GetOrderedPageIDs(const FMetasoundFrontendGraphClass& RootGraph)
{
	TArray<FGuid> Ordered;
	if (RootGraph.ContainsGraphPage(Metasound::Frontend::DefaultPageID))
	{
		Ordered.Add(Metasound::Frontend::DefaultPageID);
	}
	if (const UMetaSoundSettings* Settings = GetDefault<UMetaSoundSettings>())
	{
		for (const FMetaSoundPageSettings& Page : Settings->GetProjectPageSettings())
		{
			if (RootGraph.ContainsGraphPage(Page.UniqueId))
			{
				Ordered.AddUnique(Page.UniqueId);
			}
		}
	}
	for (const FMetasoundFrontendGraph& Graph : RootGraph.GetConstGraphPages())
	{
		Ordered.AddUnique(Graph.PageID);
	}
	return Ordered;
}

FGuid GetRegisteredPageID(const FMetasoundFrontendGraphClass& RootGraph)
{
	if (const FMetasoundFrontendGraph* Graph = Metasound::Frontend::FindPreferredPage(RootGraph.GetConstGraphPages(), UMetaSoundSettings::GetPageOrder()))
	{
		return Graph->PageID;
	}
	return Metasound::Frontend::DefaultPageID;
}

TArray<FInterfaceFacts> DescribeInterfaces(const FMetasoundFrontendDocument& Document, const UClass& AssetClass)
{
	using namespace Metasound::Frontend;
	const FTopLevelAssetPath ClassPath = AssetClass.GetClassPathName();
	TArray<FInterfaceFacts> Out;
	for (const FMetasoundFrontendVersion& Version : Document.Interfaces)
	{
		FInterfaceFacts& Facts = Out.AddDefaulted_GetRef();
		Facts.Version = Version;
		FMetasoundFrontendInterface Interface;
		if (FindRegisteredInterface(Version, Interface))
		{
			Facts.bRegistered = true;
			const FMetasoundFrontendInterfaceUClassOptions* Options = Interface.Metadata.UClassOptions.FindByPredicate(
				[&ClassPath](const FMetasoundFrontendInterfaceUClassOptions& Candidate) { return Candidate.ClassPath == ClassPath; });
			Facts.bModifiable = Options == nullptr || Options->bIsModifiable;
			Facts.Definition = MoveTemp(Interface);
		}
	}
	Out.Sort([](const FInterfaceFacts& A, const FInterfaceFacts& B)
	{
		const int32 Order = A.Version.Name.Compare(B.Version.Name);
		if (Order != 0)
		{
			return Order < 0;
		}
		return A.Version.Number.Major != B.Version.Number.Major ? A.Version.Number.Major < B.Version.Number.Major : A.Version.Number.Minor < B.Version.Number.Minor;
	});
	return Out;
}
} // namespace UE::AssetDocument::Graph::Audio::MetaSound
