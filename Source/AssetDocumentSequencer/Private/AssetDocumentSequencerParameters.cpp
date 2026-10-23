// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Parameter sections (design §3.5.3): sections whose channels live in arrays of per-parameter structs, created per
// parameter as the editor's "+ Parameter" menus (UMovieSceneParameterSection::AddScalarParameterKey and siblings, the
// component material section's ParameterInfo variants, custom primitive data, particle parameters, MPC) or per fixture
// patch / function (DMX library sections). The arrays are found by reflection: every stored TArray of a struct that
// holds channels is a parameter set, so plugin sections of the same shape need no code. Each element is one Parameters
// entry {Type?, Name?, Properties?, Channels?, Parameters?}; its channels are read and written in the element's memory
// with the channel codec of AssetDocumentSequencerChannels.cpp (the section's channel proxy only lists them, and
// DMX's proxy leaves out disabled functions). A write rebuilds every set from the document and then rebuilds the
// channel proxy (PostEditImport, as a paste of the section does), since the proxy points into the arrays.

#include "AssetDocumentSequencerPrimitives.h"

#include "Channels/MovieSceneChannel.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MovieSceneSection.h"
#include "SceneTypes.h"
#include "Sections/MovieSceneCustomPrimitiveDataSection.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::Graph::Sequencer
{
namespace
{
// One parameter set: an array of structs holding channels.
struct FParameterSet
{
	const FArrayProperty* Array = nullptr;
	const UScriptStruct* Struct = nullptr;
	FString Type;
};

// One channel member of a parameter struct ("Name[i]" for a fixed array).
struct FChannelMember
{
	const FStructProperty* Property = nullptr;
	int32 Index = 0;
	FString Name;
};

// The element's name field: ParameterName, or ParameterInfo.Name (component material parameters).
struct FNameField
{
	const FNameProperty* Name = nullptr;
	const FStructProperty* Outer = nullptr;

	FName* In(void* Element) const
	{
		void* Container = Outer != nullptr ? Outer->ContainerPtrToValuePtr<void>(Element) : Element;
		return Name != nullptr ? Name->ContainerPtrToValuePtr<FName>(Container) : nullptr;
	}
};

struct FStructBuffer
{
	explicit FStructBuffer(const UScriptStruct& InStruct)
		: Struct(InStruct)
	{
		Memory = static_cast<uint8*>(FMemory::Malloc(FMath::Max(Struct.GetStructureSize(), 1), Struct.GetMinAlignment()));
		Struct.InitializeStruct(Memory);
	}
	~FStructBuffer()
	{
		Struct.DestroyStruct(Memory);
		FMemory::Free(Memory);
	}
	FStructBuffer(const FStructBuffer&) = delete;
	FStructBuffer& operator=(const FStructBuffer&) = delete;

	const UScriptStruct& Struct;
	uint8* Memory = nullptr;
};

bool IsChannelStruct(const UScriptStruct* Struct)
{
	return Struct != nullptr && Struct->IsChildOf(FMovieSceneChannel::StaticStruct());
}

FString SetType(const FString& ArrayName)
{
	static const TCHAR* Suffixes[] = {TEXT("ParameterNamesAndCurves"), TEXT("ParameterNamesAndCurve"), TEXT("ParameterInfosAndCurves"),
		TEXT("ParameterInfosAndCurve")};
	for (const TCHAR* Suffix : Suffixes)
	{
		if (ArrayName.EndsWith(Suffix, ESearchCase::CaseSensitive) && ArrayName.Len() > FCString::Strlen(Suffix))
		{
			return ArrayName.LeftChop(FCString::Strlen(Suffix));
		}
	}
	return ArrayName;
}

TArray<FParameterSet> SetsOf(const UStruct& Owner)
{
	TArray<FParameterSet> Sets;
	for (TFieldIterator<FProperty> It(&Owner); It; ++It)
	{
		const FArrayProperty* Array = CastField<FArrayProperty>(*It);
		const FStructProperty* Inner = Array != nullptr ? CastField<FStructProperty>(Array->Inner) : nullptr;
		if (Inner == nullptr || !IsStoredField(**It) || IsChannelStruct(Inner->Struct) || !HoldsChannel(**It))
		{
			continue;
		}
		Sets.Add({Array, Inner->Struct, SetType(Array->GetName())});
	}
	return Sets;
}

bool IsControlRigSection(const UClass& Class)
{
	return Class.GetName().Contains(TEXT("ControlRig"));
}

TArray<FChannelMember> ChannelMembers(const UScriptStruct& Struct)
{
	TArray<FChannelMember> Members;
	for (TFieldIterator<FProperty> It(&Struct); It; ++It)
	{
		const FStructProperty* Property = CastField<FStructProperty>(*It);
		if (Property == nullptr || !IsChannelStruct(Property->Struct) || !IsStoredField(*Property))
		{
			continue;
		}
		for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
		{
			Members.Add({Property, Index, Property->ArrayDim > 1 ? FString::Printf(TEXT("%s[%d]"), *Property->GetName(), Index) : Property->GetName()});
		}
	}
	return Members;
}

FNameField NameFieldOf(const UScriptStruct& Struct)
{
	FNameField Field;
	Field.Name = CastField<FNameProperty>(Struct.FindPropertyByName(TEXT("ParameterName")));
	if (Field.Name == nullptr)
	{
		const FStructProperty* Info = CastField<FStructProperty>(Struct.FindPropertyByName(TEXT("ParameterInfo")));
		const FNameProperty* Name = Info != nullptr && Info->Struct != nullptr ? CastField<FNameProperty>(Info->Struct->FindPropertyByName(TEXT("Name"))) : nullptr;
		if (Name != nullptr)
		{
			Field.Name = Name;
			Field.Outer = Info;
		}
	}
	return Field;
}

void* MemberPtr(const FChannelMember& Member, void* Element)
{
	return Member.Property->ContainerPtrToValuePtr<void>(Element, Member.Index);
}

bool IsElementProperty(const FProperty& Property)
{
	return !IsChannelField(Property);
}

FString JoinNames(const TArray<FString>& Names)
{
	return Names.IsEmpty() ? FString(TEXT("(none)")) : FString::Join(Names, TEXT(", "));
}

// ---- read ------------------------------------------------------------------------------------------------------------

void ReadSets(const UStruct& Owner, const void* Memory, TArray<int32>& Path, const FString& DocumentPath, FEncodeContext& Context,
	TArray<FValue>& OutEntries, TArray<FValue>& OutRecords);

FValue ReadElement(const FParameterSet& Set, bool bTyped, const void* Element, TArray<int32>& Path, const FString& EntryPath, FEncodeContext& Context,
	TArray<FValue>& OutRecords)
{
	const UScriptStruct& Struct = *Set.Struct;
	FObjectBuilder Entry;
	if (bTyped)
	{
		Entry.AddString(TEXT("Type"), Set.Type);
	}
	// Properties: the element's other fields against a fresh element; the name is a key of its own (the defaults take
	// the element's name, so it never shows among the Properties).
	FStructBuffer Defaults(Struct);
	const FNameField NameField = NameFieldOf(Struct);
	if (const FName* Name = NameField.In(const_cast<void*>(Element)))
	{
		Entry.Add(TEXT("Name"), MakeName(*Name));
		*NameField.In(Defaults.Memory) = *Name;
	}
	FEncodeContext PropertiesContext{Context.Model, Context.Time, Context.Warnings, EntryPath + TEXT("/Properties")};
	FValue Properties = EncodeSparseObject(Struct, Element, Defaults.Memory, &IsElementProperty, PropertiesContext);
	if (!Properties.ObjectKeys.IsEmpty())
	{
		Entry.Add(TEXT("Properties"), MoveTemp(Properties));
	}
	const TArray<FChannelMember> Members = ChannelMembers(Struct);
	if (!Members.IsEmpty())
	{
		FObjectBuilder Channels;
		FEncodeContext ChannelContext{Context.Model, Context.Time, Context.Warnings, EntryPath};
		for (const FChannelMember& Member : Members)
		{
			ReadChannelValue(Member.Property->Struct->GetFName(), MemberPtr(Member, const_cast<void*>(Element)), Member.Name, Path, ChannelContext, Channels,
				OutRecords);
		}
		Entry.Add(TEXT("Channels"), Channels.Build());
	}
	TArray<FValue> Nested;
	ReadSets(Struct, Element, Path, EntryPath + TEXT("/Parameters"), Context, Nested, OutRecords);
	if (!Nested.IsEmpty())
	{
		Entry.Add(TEXT("Parameters"), MakeArray(MoveTemp(Nested)));
	}
	return Entry.Build();
}

void ReadSets(const UStruct& Owner, const void* Memory, TArray<int32>& Path, const FString& DocumentPath, FEncodeContext& Context,
	TArray<FValue>& OutEntries, TArray<FValue>& OutRecords)
{
	const TArray<FParameterSet> Sets = SetsOf(Owner);
	for (const FParameterSet& Set : Sets)
	{
		FScriptArrayHelper Helper(Set.Array, Set.Array->ContainerPtrToValuePtr<void>(Memory));
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			const int32 EntryIndex = OutEntries.Num();
			Path.Add(EntryIndex);
			OutEntries.Add(ReadElement(Set, Sets.Num() > 1, Helper.GetRawPtr(Index), Path, FString::Printf(TEXT("%s/%d"), *DocumentPath, EntryIndex), Context,
				OutRecords));
			Path.Pop();
		}
	}
}

void CollectAddresses(const UStruct& Owner, const void* Memory, TSet<const void*>& Out)
{
	for (const FParameterSet& Set : SetsOf(Owner))
	{
		const TArray<FChannelMember> Members = ChannelMembers(*Set.Struct);
		FScriptArrayHelper Helper(Set.Array, Set.Array->ContainerPtrToValuePtr<void>(Memory));
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			for (const FChannelMember& Member : Members)
			{
				Out.Add(MemberPtr(Member, Helper.GetRawPtr(Index)));
			}
			CollectAddresses(*Set.Struct, Helper.GetRawPtr(Index), Out);
		}
	}
}

// ---- write -----------------------------------------------------------------------------------------------------------

bool CheckEntryKeys(const FValue& Entry, FDecodeContext& Context)
{
	if (Entry.Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("A parameter is an object {Type?, Name?, Properties?, Channels?, Parameters?}."),
			Context.Path);
	}
	for (const FString& Key : Entry.ObjectKeys)
	{
		if (Key != TEXT("Type") && Key != TEXT("Name") && Key != TEXT("Properties") && Key != TEXT("Channels") && Key != TEXT("Parameters"))
		{
			return Context.Reader.Fail(TEXT("Graph.UnknownField"), FString::Printf(TEXT("A parameter takes Type, Name, Properties, Channels and "
				"Parameters; '%s' is not one of them."), *Key), Context.Path + TEXT("/") + Escape(Key));
		}
	}
	return true;
}

// Custom primitive data parameters are named by their first float index (as "+ Custom Primitive Data" names them); the
// section's channel proxy asserts that, so a name that is not an index in range is refused here.
bool CheckCustomPrimitiveDataName(const FParameterSet& Set, const FString& Name, FDecodeContext& Context)
{
	int32 Width = 0;
	if (Set.Type == TEXT("Scalar"))
	{
		Width = 1;
	}
	else if (Set.Type == TEXT("Vector2D"))
	{
		Width = 2;
	}
	else if (Set.Type == TEXT("Vector"))
	{
		Width = 3;
	}
	else if (Set.Type == TEXT("Color"))
	{
		Width = 4;
	}
	else
	{
		return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), FString::Printf(TEXT("Custom primitive data takes Scalar, Vector2D, Vector and Color "
			"parameters, not %s."), *Set.Type), Context.Path + TEXT("/Type"));
	}
	const int32 Limit = FCustomPrimitiveData::NumCustomPrimitiveDataFloats - Width;
	if (!Name.IsNumeric() || Name.Contains(TEXT(".")) || Name.Contains(TEXT("-")) || FCString::Atoi(*Name) > Limit)
	{
		return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), FString::Printf(TEXT("A custom primitive data %s parameter is named by its first "
			"float index, 0 to %d."), *Set.Type, Limit), Context.Path + TEXT("/Name"));
	}
	return true;
}

bool WriteSets(const UStruct& Owner, void* Memory, const FValue& List, bool bCustomPrimitiveData, UObject* Section, FDecodeContext& Context);

bool WriteElement(const FParameterSet& Set, void* Element, const FValue& Entry, bool bCustomPrimitiveData, UObject* Section, FDecodeContext& Context)
{
	const UScriptStruct& Struct = *Set.Struct;
	const FValue Empty = MakeEmptyObject();
	const FValue* Properties = FindField(Entry, TEXT("Properties"));
	FDecodeContext PropertiesContext = Context;
	PropertiesContext.Path = Context.Path + TEXT("/Properties");
	PropertiesContext.Owner = Section;
	const FStructBuffer Defaults(Struct);
	if (!DecodeSparseObject(Struct, Properties != nullptr ? *Properties : Empty, Element, Defaults.Memory, &IsElementProperty, PropertiesContext))
	{
		return false;
	}
	const FNameField NameField = NameFieldOf(Struct);
	const FValue* Name = FindField(Entry, TEXT("Name"));
	if (Name != nullptr && (NameField.Name == nullptr || Name->Kind != EAssetDocumentRuntimeValueKind::String))
	{
		return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), NameField.Name == nullptr ? FString::Printf(TEXT("A %s parameter has no Name (its "
			"fields are Properties)."), *Set.Type) : FString(TEXT("Name is a string.")), Context.Path + TEXT("/Name"));
	}
	if (NameField.Name != nullptr)
	{
		const FString Text = Name != nullptr ? Name->StringValue : FString();
		if (bCustomPrimitiveData && !CheckCustomPrimitiveDataName(Set, Text, Context))
		{
			return false;
		}
		*NameField.In(Element) = Text.IsEmpty() ? NAME_None : FName(*Text);
	}
	const TArray<FChannelMember> Members = ChannelMembers(Struct);
	if (const FValue* Channels = FindField(Entry, TEXT("Channels")))
	{
		if (Channels->Kind != EAssetDocumentRuntimeValueKind::Object)
		{
			return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("Channels is an object of channel name -> channel."), Context.Path + TEXT("/Channels"));
		}
		for (int32 Index = 0; Index < Channels->ObjectKeys.Num(); ++Index)
		{
			const FString& ChannelName = Channels->ObjectKeys[Index];
			const FChannelMember* Member = Members.FindByPredicate([&ChannelName](const FChannelMember& Item) { return Item.Name == ChannelName; });
			FDecodeContext ChannelContext = Context;
			ChannelContext.Path = Context.Path + TEXT("/Channels/") + Escape(ChannelName);
			ChannelContext.Owner = Section;
			if (Member == nullptr)
			{
				TArray<FString> Names;
				for (const FChannelMember& Item : Members)
				{
					Names.Add(Item.Name);
				}
				return Context.Reader.Fail(TEXT("Sequencer.UnknownChannel"), FString::Printf(TEXT("A %s parameter has no channel '%s'. Channels: %s."),
					*Set.Type, *ChannelName, *JoinNames(Names)), ChannelContext.Path);
			}
			if (!WriteChannelValue(Member->Property->Struct->GetFName(), MemberPtr(*Member, Element), Channels->Elements[Index], false, {}, ChannelContext))
			{
				return false;
			}
		}
	}
	const FValue EmptyList = MakeArray({});
	const FValue* Nested = FindField(Entry, TEXT("Parameters"));
	FDecodeContext NestedContext = Context;
	NestedContext.Path = Context.Path + TEXT("/Parameters");
	return WriteSets(Struct, Element, Nested != nullptr ? *Nested : EmptyList, false, Section, NestedContext);
}

// Each entry's set (entries are listed set by set, in the class's order, as Extract lists them).
bool SetsOfEntries(const UStruct& Owner, const TArray<FParameterSet>& Sets, const FValue& List, FDecodeContext& Context, TArray<int32>& SetOfEntry)
{
	if (List.Kind != EAssetDocumentRuntimeValueKind::Array)
	{
		return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("Parameters is an array of parameters."), Context.Path);
	}
	if (Sets.IsEmpty() && !List.Elements.IsEmpty())
	{
		return Context.Reader.Fail(TEXT("Sequencer.FieldNotEditable"), FString::Printf(TEXT("%s has no parameter sets."), *Owner.GetName()), Context.Path);
	}
	TArray<FString> Order;
	for (const FParameterSet& Set : Sets)
	{
		Order.Add(Set.Type);
	}
	for (int32 Index = 0; Index < List.Elements.Num(); ++Index)
	{
		const FValue& Entry = List.Elements[Index];
		FDecodeContext EntryContext = Context;
		EntryContext.Path = FString::Printf(TEXT("%s/%d"), *Context.Path, Index);
		if (!CheckEntryKeys(Entry, EntryContext))
		{
			return false;
		}
		const FValue* Type = FindField(Entry, TEXT("Type"));
		int32 SetIndex = Sets.Num() == 1 ? 0 : INDEX_NONE;
		if (Type != nullptr)
		{
			SetIndex = Sets.IndexOfByPredicate([Type](const FParameterSet& Set)
			{
				return Type->Kind == EAssetDocumentRuntimeValueKind::String && (Type->StringValue == Set.Type || Type->StringValue == Set.Array->GetName());
			});
		}
		if (SetIndex == INDEX_NONE)
		{
			return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), FString::Printf(TEXT("A parameter of %s has a Type: %s."), *Owner.GetName(),
				*JoinNames(Order)), EntryContext.Path + TEXT("/Type"));
		}
		if (!SetOfEntry.IsEmpty() && SetIndex < SetOfEntry.Last())
		{
			return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), FString::Printf(TEXT("Parameters are listed type by type in this order: %s (a %s "
				"parameter cannot follow a %s one)."), *JoinNames(Order), *Sets[SetIndex].Type, *Sets[SetOfEntry.Last()].Type), EntryContext.Path + TEXT("/Type"));
		}
		SetOfEntry.Add(SetIndex);
	}
	return true;
}

bool WriteSets(const UStruct& Owner, void* Memory, const FValue& List, bool bCustomPrimitiveData, UObject* Section, FDecodeContext& Context)
{
	const TArray<FParameterSet> Sets = SetsOf(Owner);
	TArray<int32> SetOfEntry;
	if (!SetsOfEntries(Owner, Sets, List, Context, SetOfEntry))
	{
		return false;
	}
	TArray<int32> Counts;
	Counts.Init(0, Sets.Num());
	for (const int32 SetIndex : SetOfEntry)
	{
		++Counts[SetIndex];
	}
	for (int32 SetIndex = 0; SetIndex < Sets.Num(); ++SetIndex)
	{
		FScriptArrayHelper Helper(Sets[SetIndex].Array, Sets[SetIndex].Array->ContainerPtrToValuePtr<void>(Memory));
		Helper.EmptyAndAddValues(Counts[SetIndex]);
	}
	TArray<int32> Next;
	Next.Init(0, Sets.Num());
	for (int32 Index = 0; Index < List.Elements.Num(); ++Index)
	{
		const FParameterSet& Set = Sets[SetOfEntry[Index]];
		FScriptArrayHelper Helper(Set.Array, Set.Array->ContainerPtrToValuePtr<void>(Memory));
		FDecodeContext EntryContext = Context;
		EntryContext.Path = FString::Printf(TEXT("%s/%d"), *Context.Path, Index);
		if (!WriteElement(Set, Helper.GetRawPtr(Next[SetOfEntry[Index]]++), List.Elements[Index], bCustomPrimitiveData, Section, EntryContext))
		{
			return false;
		}
	}
	return true;
}

// Adds the entries at the end of their sets (the rest of a list WriteSets began; a set after one that already has
// elements cannot take more, as the list is set by set).
bool AppendSets(const UStruct& Owner, void* Memory, const FValue& List, bool bCustomPrimitiveData, UObject* Section, FDecodeContext& Context)
{
	const TArray<FParameterSet> Sets = SetsOf(Owner);
	TArray<int32> SetOfEntry;
	if (!SetsOfEntries(Owner, Sets, List, Context, SetOfEntry))
	{
		return false;
	}
	for (int32 SetIndex = 0; SetIndex < Sets.Num(); ++SetIndex)
	{
		const int32 Num = FScriptArrayHelper(Sets[SetIndex].Array, Sets[SetIndex].Array->ContainerPtrToValuePtr<void>(Memory)).Num();
		if (!SetOfEntry.IsEmpty() && SetIndex > SetOfEntry[0] && Num > 0)
		{
			return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), FString::Printf(TEXT("Parameters are listed type by type: a %s parameter "
				"cannot follow the %s ones already there."), *Sets[SetOfEntry[0]].Type, *Sets[SetIndex].Type), Context.Path + TEXT("/0/Type"));
		}
	}
	for (int32 Index = 0; Index < List.Elements.Num(); ++Index)
	{
		const FParameterSet& Set = Sets[SetOfEntry[Index]];
		FScriptArrayHelper Helper(Set.Array, Set.Array->ContainerPtrToValuePtr<void>(Memory));
		const int32 Added = Helper.AddValue();
		FDecodeContext EntryContext = Context;
		EntryContext.Path = FString::Printf(TEXT("%s/%d"), *Context.Path, Index);
		if (!WriteElement(Set, Helper.GetRawPtr(Added), List.Elements[Index], bCustomPrimitiveData, Section, EntryContext))
		{
			return false;
		}
	}
	return true;
}

// The parameter element at an index path (each index counts through the owner's sets in order); an empty path is the
// section itself.
bool ResolveElement(UMovieSceneSection& Section, TConstArrayView<int32> Path, const UStruct*& OutOwner, void*& OutMemory, FString& OutError)
{
	OutOwner = Section.GetClass();
	OutMemory = &Section;
	for (const int32 Wanted : Path)
	{
		int32 Index = Wanted;
		void* Element = nullptr;
		const UScriptStruct* Struct = nullptr;
		for (const FParameterSet& Set : SetsOf(*OutOwner))
		{
			FScriptArrayHelper Helper(Set.Array, Set.Array->ContainerPtrToValuePtr<void>(OutMemory));
			if (Index >= 0 && Index < Helper.Num())
			{
				Element = Helper.GetRawPtr(Index);
				Struct = Set.Struct;
				break;
			}
			Index -= Helper.Num();
		}
		if (Element == nullptr)
		{
			OutError = FString::Printf(TEXT("There is no parameter %d there."), Wanted);
			return false;
		}
		OutOwner = Struct;
		OutMemory = Element;
	}
	return true;
}

void EmptySets(UMovieSceneSection& Section)
{
	for (const FParameterSet& Set : SetsOf(*Section.GetClass()))
	{
		FScriptArrayHelper(Set.Array, Set.Array->ContainerPtrToValuePtr<void>(&Section)).EmptyValues();
	}
}

TArray<TSharedPtr<FJsonValue>> DescribeSets(const UStruct& Owner)
{
	TArray<TSharedPtr<FJsonValue>> Rows;
	const TArray<FParameterSet> Sets = SetsOf(Owner);
	for (const FParameterSet& Set : Sets)
	{
		const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		if (Sets.Num() > 1)
		{
			Row->SetStringField(TEXT("Type"), Set.Type);
		}
		const FNameField NameField = NameFieldOf(*Set.Struct);
		Row->SetBoolField(TEXT("Name"), NameField.Name != nullptr);
		TArray<TSharedPtr<FJsonValue>> Channels;
		for (const FChannelMember& Member : ChannelMembers(*Set.Struct))
		{
			Channels.Add(MakeShared<FJsonValueString>(Member.Name));
		}
		Row->SetArrayField(TEXT("Channels"), Channels);
		TArray<TSharedPtr<FJsonValue>> Fields;
		for (TFieldIterator<FProperty> It(Set.Struct); It; ++It)
		{
			if (IsStoredField(**It) && IsElementProperty(**It) && *It != NameField.Name && *It != NameField.Outer)
			{
				Fields.Add(MakeShared<FJsonValueString>(It->GetName()));
			}
		}
		Row->SetArrayField(TEXT("Properties"), Fields);
		TArray<TSharedPtr<FJsonValue>> Nested = DescribeSets(*Set.Struct);
		if (!Nested.IsEmpty())
		{
			Row->SetArrayField(TEXT("Parameters"), Nested);
		}
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	return Rows;
}
} // namespace

bool UsesParameters(const UMovieSceneSection& Section)
{
	return !IsControlRigSection(*Section.GetClass()) && !SetsOf(*Section.GetClass()).IsEmpty();
}

TSet<const void*> ParameterChannelAddresses(const UMovieSceneSection& Section)
{
	TSet<const void*> Addresses;
	if (UsesParameters(Section))
	{
		CollectAddresses(*Section.GetClass(), &Section, Addresses);
	}
	return Addresses;
}

FValue ReadParameters(const UMovieSceneSection& Section, FEncodeContext& Context, TArray<FValue>& OutKeyRecords)
{
	TArray<FValue> Entries;
	TArray<int32> Path;
	ReadSets(*Section.GetClass(), &Section, Path, Context.Path + TEXT("/Parameters"), Context, Entries, OutKeyRecords);
	return MakeArray(MoveTemp(Entries));
}

bool WriteParameters(UMovieSceneSection& Section, const FValue* Entries, FDecodeContext& Context)
{
	if (!UsesParameters(Section))
	{
		return Entries == nullptr || Context.Reader.Fail(TEXT("Sequencer.FieldNotEditable"), FString::Printf(TEXT("A %s has no Parameters (its channels "
			"are in Channels)."), *Section.GetClass()->GetName()), Context.Path);
	}
	const FValue EmptyList = MakeArray({});
	const bool bCustomPrimitiveData = Section.IsA<UMovieSceneCustomPrimitiveDataSection>();
	if (!WriteSets(*Section.GetClass(), &Section, Entries != nullptr ? *Entries : EmptyList, bCustomPrimitiveData, &Section, Context))
	{
		// The sets may be half written (elements without their names, which the proxy rebuild asserts on): they are
		// emptied so the proxy keeps no pointers into the old arrays (Core rolls the package back).
		EmptySets(Section);
		static_cast<UObject&>(Section).PostEditImport();
		return false;
	}
	// The channel proxy points into the arrays: rebuild it as a paste of the section does.
	static_cast<UObject&>(Section).PostEditImport();
	return true;
}

bool AppendParameters(UMovieSceneSection& Section, TConstArrayView<int32> Path, const FValue& Entries, FDecodeContext& Context)
{
	const UStruct* Owner = nullptr;
	void* Memory = nullptr;
	FString Error;
	if (!UsesParameters(Section) || !ResolveElement(Section, Path, Owner, Memory, Error))
	{
		return Context.Reader.Fail(TEXT("Sequencer.FieldNotEditable"), UsesParameters(Section) ? Error : FString::Printf(TEXT("A %s has no Parameters."),
			*Section.GetClass()->GetName()), TEXT("parameter"));
	}
	if (!AppendSets(*Owner, Memory, Entries, Section.IsA<UMovieSceneCustomPrimitiveDataSection>() && Path.IsEmpty(), &Section, Context))
	{
		EmptySets(Section);
		static_cast<UObject&>(Section).PostEditImport();
		return false;
	}
	static_cast<UObject&>(Section).PostEditImport();
	return true;
}

bool FindParameterChannel(UMovieSceneSection& Section, TConstArrayView<int32> Path, const FString& Name, FName& OutType, void*& OutChannel,
	FString& OutError)
{
	const UStruct* Owner = nullptr;
	void* Memory = nullptr;
	if (!UsesParameters(Section) || Path.IsEmpty())
	{
		OutError = FString::Printf(TEXT("%s has no parameter at that path."), *Section.GetClass()->GetName());
		return false;
	}
	if (!ResolveElement(Section, Path, Owner, Memory, OutError))
	{
		return false;
	}
	const UScriptStruct* Struct = CastChecked<UScriptStruct>(Owner);
	for (const FChannelMember& Member : ChannelMembers(*Struct))
	{
		if (Member.Name == Name)
		{
			OutType = Member.Property->Struct->GetFName();
			OutChannel = MemberPtr(Member, Memory);
			return true;
		}
	}
	OutError = FString::Printf(TEXT("The parameter has no channel '%s'."), *Name);
	return false;
}

TArray<TSharedPtr<FJsonValue>> DescribeParameters(const UClass& SectionClass)
{
	if (IsControlRigSection(SectionClass))
	{
		return {};
	}
	return DescribeSets(SectionClass);
}
} // namespace UE::AssetDocument::Graph::Sequencer
