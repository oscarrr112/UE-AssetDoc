// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Discovery/AssetDocumentDiscovery.h"
#include "Discovery/AssetDocumentUsageSearchInternal.h"

#include "AssetDocumentAssetFacts.h"
#include "AssetDocumentServerAssetFacts.h"
#include "AssetDocumentCompileStatus.h"
#include "AssetDocumentTypeDiscovery.h"
#include "AssetDocumentTypeListers.h"

#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Engine/DataTable.h"
#include "Engine/Font.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/StaticMesh.h"
#include "Sound/SoundBase.h"
#include "Engine/UserDefinedEnum.h"
#include "StructUtils/UserDefinedStruct.h"
#include "Logging/TokenizedMessage.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "UObject/EnumProperty.h"
#include "UObject/ObjectRedirector.h"
#include "RHIShaderPlatform.h"
#include "UObject/UObjectIterator.h"

namespace
{
TArray<TPair<FName, FAssetDocumentCompileStatusDescriber>>& GetCompileStatusDescribers()
{
	static TArray<TPair<FName, FAssetDocumentCompileStatusDescriber>> Describers;
	return Describers;
}

TArray<TPair<FName, FAssetDocumentAssetFactsDescriber>>& GetAssetFactsDescribers()
{
	static TArray<TPair<FName, FAssetDocumentAssetFactsDescriber>> Describers;
	return Describers;
}

TArray<TPair<FName, FAssetDocumentTypeDiscoveryHooks>>& GetTypeDiscoveryHooks()
{
	static TArray<TPair<FName, FAssetDocumentTypeDiscoveryHooks>> Hooks;
	return Hooks;
}
}

bool RegisterAssetDocumentTypeDiscoveryHooks(FName HooksId, const FAssetDocumentTypeDiscoveryHooks& Hooks)
{
	check(IsInGameThread());
	TArray<TPair<FName, FAssetDocumentTypeDiscoveryHooks>>& Registered = GetTypeDiscoveryHooks();
	if (HooksId.IsNone()
		|| Registered.ContainsByPredicate([HooksId](const TPair<FName, FAssetDocumentTypeDiscoveryHooks>& Each) { return Each.Key == HooksId; }))
	{
		return false;
	}
	Registered.Emplace(HooksId, Hooks);
	return true;
}

bool UnregisterAssetDocumentTypeDiscoveryHooks(FName HooksId)
{
	check(IsInGameThread());
	return GetTypeDiscoveryHooks().RemoveAll([HooksId](const TPair<FName, FAssetDocumentTypeDiscoveryHooks>& Each)
	{
		return Each.Key == HooksId;
	}) > 0;
}

bool RegisterAssetDocumentCompileStatusDescriber(FName DescriberId, FAssetDocumentCompileStatusDescriber Describer)
{
	check(IsInGameThread());
	TArray<TPair<FName, FAssetDocumentCompileStatusDescriber>>& Describers = GetCompileStatusDescribers();
	if (DescriberId.IsNone() || Describer == nullptr
		|| Describers.ContainsByPredicate([DescriberId](const TPair<FName, FAssetDocumentCompileStatusDescriber>& Each) { return Each.Key == DescriberId; }))
	{
		return false;
	}
	Describers.Emplace(DescriberId, Describer);
	return true;
}

bool UnregisterAssetDocumentCompileStatusDescriber(FName DescriberId)
{
	check(IsInGameThread());
	return GetCompileStatusDescribers().RemoveAll([DescriberId](const TPair<FName, FAssetDocumentCompileStatusDescriber>& Each)
	{
		return Each.Key == DescriberId;
	}) > 0;
}

bool RegisterAssetDocumentAssetFactsDescriber(FName FactsKey, FAssetDocumentAssetFactsDescriber Describer)
{
	check(IsInGameThread());
	TArray<TPair<FName, FAssetDocumentAssetFactsDescriber>>& Describers = GetAssetFactsDescribers();
	if (FactsKey.IsNone() || Describer == nullptr
		|| Describers.ContainsByPredicate([FactsKey](const TPair<FName, FAssetDocumentAssetFactsDescriber>& Each) { return Each.Key == FactsKey; }))
	{
		return false;
	}
	Describers.Emplace(FactsKey, Describer);
	return true;
}

bool UnregisterAssetDocumentAssetFactsDescriber(FName FactsKey)
{
	check(IsInGameThread());
	return GetAssetFactsDescribers().RemoveAll([FactsKey](const TPair<FName, FAssetDocumentAssetFactsDescriber>& Each)
	{
		return Each.Key == FactsKey;
	}) > 0;
}

namespace
{
TMap<FName, FAssetDocumentTypeLister>& GetTypeListers()
{
	static TMap<FName, FAssetDocumentTypeLister> Listers;
	return Listers;
}

bool IsBuiltInTypeKind(FName Kind)
{
	return Kind == FName(TEXT("class")) || Kind == FName(TEXT("struct")) || Kind == FName(TEXT("enum"));
}
}

bool RegisterAssetDocumentTypeLister(FName Kind, FAssetDocumentTypeLister Lister)
{
	check(IsInGameThread());
	if (Kind.IsNone() || Lister == nullptr || IsBuiltInTypeKind(Kind) || GetTypeListers().Contains(Kind))
	{
		return false;
	}
	GetTypeListers().Add(Kind, Lister);
	return true;
}

bool UnregisterAssetDocumentTypeLister(FName Kind)
{
	check(IsInGameThread());
	return GetTypeListers().Remove(Kind) > 0;
}

namespace UE::AssetDocument::Discovery
{
namespace
{
constexpr int32 DefaultLimit = 50;
constexpr int32 MaxLimit = 500;
constexpr int32 MaxBones = 2000;

FString QueryValue(const TMap<FString, FString>& Query, const TCHAR* Key)
{
	const FString* Value = Query.Find(Key);
	return Value ? Value->TrimStartAndEnd() : FString();
}

bool QueryBool(const TMap<FString, FString>& Query, const TCHAR* Key)
{
	const FString Value = QueryValue(Query, Key);
	return Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Value == TEXT("1");
}

bool ReadPaging(const TMap<FString, FString>& Query, int32& OutLimit, int32& OutOffset, FString& OutError)
{
	OutLimit = DefaultLimit;
	OutOffset = 0;
	for (const TPair<const TCHAR*, int32*> Entry : {TPair<const TCHAR*, int32*>(TEXT("limit"), &OutLimit), TPair<const TCHAR*, int32*>(TEXT("offset"), &OutOffset)})
	{
		const FString Text = QueryValue(Query, Entry.Key);
		if (Text.IsEmpty())
		{
			continue;
		}
		if (!Text.IsNumeric() || Text.Contains(TEXT(".")) || Text.StartsWith(TEXT("-")))
		{
			OutError = FString::Printf(TEXT("'%s' must be a non-negative integer, got '%s'."), Entry.Key, *Text);
			return false;
		}
		*Entry.Value = FCString::Atoi(*Text);
	}
	OutLimit = FMath::Clamp(OutLimit, 1, MaxLimit);
	return true;
}

// Substring match, or a wildcard match when the pattern has * or ?; case-insensitive.
bool MatchesName(const FString& Name, const FString& Pattern)
{
	if (Pattern.IsEmpty())
	{
		return true;
	}
	if (Pattern.Contains(TEXT("*")) || Pattern.Contains(TEXT("?")))
	{
		return Name.MatchesWildcard(Pattern, ESearchCase::IgnoreCase);
	}
	return Name.Contains(Pattern, ESearchCase::IgnoreCase);
}

bool MatchesPathPrefix(const FString& Path, const FString& Prefix)
{
	if (Prefix.IsEmpty())
	{
		return true;
	}
	if (!Path.StartsWith(Prefix, ESearchCase::IgnoreCase))
	{
		return false;
	}
	// "/Game/AI" matches "/Game/AI/BT" and "/Game/AI.X", not "/Game/AIDemo".
	if (Path.Len() == Prefix.Len() || Prefix.EndsWith(TEXT("/")) || Prefix.EndsWith(TEXT(".")))
	{
		return true;
	}
	const TCHAR Next = Path[Prefix.Len()];
	return Next == TEXT('/') || Next == TEXT('.');
}

// Paging over a sorted list of rows: {Total, Offset, <Key>[...], NextOffset?}.
TSharedPtr<FJsonObject> MakePage(const TCHAR* Key, TArray<TSharedPtr<FJsonObject>>&& Rows, int32 Limit, int32 Offset)
{
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetNumberField(TEXT("Total"), Rows.Num());
	Payload->SetNumberField(TEXT("Offset"), Offset);
	TArray<TSharedPtr<FJsonValue>> Values;
	for (int32 Index = Offset; Index < Rows.Num() && Index < Offset + Limit; ++Index)
	{
		Values.Add(MakeShared<FJsonValueObject>(Rows[Index]));
	}
	Payload->SetArrayField(Key, Values);
	if (Offset + Limit < Rows.Num())
	{
		Payload->SetNumberField(TEXT("NextOffset"), Offset + Limit);
	}
	return Payload;
}

TArray<TSharedPtr<FJsonValue>> StringArray(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	for (const FString& Value : Values)
	{
		Result.Add(MakeShared<FJsonValueString>(Value));
	}
	return Result;
}

IAssetRegistry& Registry()
{
	return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
}

// A class named by path ("/Script/Engine.Actor", "/Game/BP.BP_C") or native short name ("Actor").
// Silent on failure: discovery answers with a diagnostic, not with load warnings.
// A short name is looked up natives first and without logging (KI-5: UClass::TryFindTypeSlow warns
// "Short type name ... provided for TryFindType" with a callstack for every short name).
UClass* FindClass(const FString& Name)
{
	if (!Name.StartsWith(TEXT("/")))
	{
		return FindFirstObject<UClass>(*Name, EFindFirstObjectOptions::ExactClass | EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging);
	}
	if (UClass* Loaded = FindObject<UClass>(nullptr, *Name))
	{
		return Loaded;
	}
	// An unloaded Blueprint class: load it only when the Asset Registry lists its Blueprint.
	if (Name.EndsWith(TEXT("_C")) && Registry().GetAssetByObjectPath(FSoftObjectPath(Name.LeftChop(2))).IsValid())
	{
		return LoadObject<UClass>(nullptr, *Name);
	}
	return nullptr;
}

UScriptStruct* FindStruct(const FString& Name)
{
	if (Name.StartsWith(TEXT("/")))
	{
		return FindObject<UScriptStruct>(nullptr, *Name);
	}
	return FindFirstObject<UScriptStruct>(*Name, EFindFirstObjectOptions::ExactClass | EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging);
}

// Editor-internal and stale types the pickers never offer.
bool IsTransientTypeName(const FString& Name)
{
	return Name.StartsWith(TEXT("SKEL_")) || Name.StartsWith(TEXT("REINST_")) || Name.StartsWith(TEXT("TRASHCLASS_"))
		|| Name.StartsWith(TEXT("HOTRELOADED_")) || Name.StartsWith(TEXT("PLACEHOLDER-")) || Name.StartsWith(TEXT("STRUCT_REINST_"))
		|| Name.StartsWith(TEXT("TRASHSTRUCT_")) || Name.StartsWith(TEXT("LIVECODING_"));
}

FString ShortName(const FString& Path)
{
	int32 Dot = INDEX_NONE;
	return Path.FindLastChar(TEXT('.'), Dot) ? Path.Mid(Dot + 1) : FPaths::GetBaseFilename(Path);
}

TArray<FString> EnumNames(const UEnum& Enum)
{
	TArray<FString> Names;
	for (int32 Index = 0; Index < Enum.NumEnums(); ++Index)
	{
		if (Enum.HasMetaData(TEXT("Hidden"), Index) || (Index == Enum.NumEnums() - 1 && Enum.ContainsExistingMax()))
		{
			continue;
		}
		Names.Add(Enum.GetNameStringByIndex(Index));
	}
	return Names;
}

const UEnum* PropertyEnum(const FProperty& Property)
{
	if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(&Property))
	{
		return EnumProperty->GetEnum();
	}
	if (const FByteProperty* ByteProperty = CastField<FByteProperty>(&Property))
	{
		return ByteProperty->Enum;
	}
	return nullptr;
}

TSharedPtr<FJsonObject> DescribeField(const FProperty& Property)
{
	TSharedPtr<FJsonObject> Field = MakeShared<FJsonObject>();
	Field->SetStringField(TEXT("Name"), Property.GetAuthoredName());
	Field->SetStringField(TEXT("Type"), Property.ArrayDim > 1
		? FString::Printf(TEXT("%s[%d]"), *Property.GetCPPType(), Property.ArrayDim) : Property.GetCPPType());
	if (const UEnum* Enum = PropertyEnum(Property))
	{
		Field->SetArrayField(TEXT("EnumValues"), StringArray(EnumNames(*Enum)));
	}
	return Field;
}

TSharedPtr<FJsonObject> DescribeStruct(const UScriptStruct& Struct)
{
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("Kind"), TEXT("Struct"));
	Payload->SetStringField(TEXT("Path"), Struct.GetPathName());
	if (const UScriptStruct* Super = Cast<UScriptStruct>(Struct.GetSuperStruct()))
	{
		Payload->SetStringField(TEXT("Super"), Super->GetPathName());
	}
	// What the New Data Table dialog accepts as row struct.
	Payload->SetBoolField(TEXT("RowStruct"), Struct.IsA<UUserDefinedStruct>() || Struct.IsChildOf(FTableRowBase::StaticStruct()));
	TArray<TSharedPtr<FJsonValue>> Fields;
	for (TFieldIterator<FProperty> It(&Struct); It; ++It)
	{
		if (!It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
		{
			Fields.Add(MakeShared<FJsonValueObject>(DescribeField(**It)));
		}
	}
	Payload->SetArrayField(TEXT("Fields"), Fields);
	for (const TPair<FName, FAssetDocumentTypeDiscoveryHooks>& Hooks : GetTypeDiscoveryHooks())
	{
		if (Hooks.Value.DescribeStruct != nullptr && Hooks.Value.DescribeStruct(Struct, *Payload))
		{
			break;
		}
	}
	return Payload;
}

TSharedPtr<FJsonObject> DescribeEnum(const UEnum& Enum)
{
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("Kind"), TEXT("Enum"));
	Payload->SetStringField(TEXT("Path"), Enum.GetPathName());
	Payload->SetArrayField(TEXT("Values"), StringArray(EnumNames(Enum)));
	return Payload;
}

// Types of test-only modules (e.g. StateTreeTestSuite, loaded in the editor for automation): no picker
// an author uses offers them.
bool IsTestModuleType(const FString& Path)
{
	if (!Path.StartsWith(TEXT("/Script/")))
	{
		return false;
	}
	FString Module = Path.Mid(8);
	int32 Dot = INDEX_NONE;
	if (Module.FindChar(TEXT('.'), Dot))
	{
		Module.LeftInline(Dot);
	}
	return Module.EndsWith(TEXT("Test")) || Module.EndsWith(TEXT("Tests")) || Module.EndsWith(TEXT("TestSuite"));
}

// The picker filter of a schema ('schema' of list_types): a provider hook decides (StateTree:
// UStateTreeSchema::IsStructAllowed / IsClassAllowed, as the node picker).
struct FSchemaFilter
{
	const UClass* Schema = nullptr;

	bool Allows(const UStruct& Type) const
	{
		if (Schema == nullptr)
		{
			return true;
		}
		for (const TPair<FName, FAssetDocumentTypeDiscoveryHooks>& Hooks : GetTypeDiscoveryHooks())
		{
			bool bAllowed = false;
			if (Hooks.Value.IsTypeAllowed != nullptr && Hooks.Value.IsTypeAllowed(*Schema, Type, bAllowed))
			{
				return bAllowed;
			}
		}
		return false;
	}
};

bool ResolveSchemaFilter(const FString& SchemaName, FSchemaFilter& OutFilter, FString& OutError)
{
	if (SchemaName.IsEmpty())
	{
		return true;
	}
	OutFilter.Schema = FindClass(SchemaName);
	bool bAllowed = false;
	const bool bKnown = OutFilter.Schema != nullptr && GetTypeDiscoveryHooks().ContainsByPredicate(
		[&OutFilter, &bAllowed](const TPair<FName, FAssetDocumentTypeDiscoveryHooks>& Each)
		{
			return Each.Value.IsTypeAllowed != nullptr && Each.Value.IsTypeAllowed(*OutFilter.Schema, *UObject::StaticClass(), bAllowed);
		});
	if (!bKnown)
	{
		OutError = FString::Printf(TEXT("'schema' '%s' is not a schema class whose node picker discovery knows (e.g. /Script/GameplayStateTreeModule.StateTreeComponentSchema)."), *SchemaName);
		return false;
	}
	return true;
}

void AddTypeRow(TMap<FString, TSharedPtr<FJsonObject>>& Rows, const FString& Path, TFunctionRef<void(FJsonObject&)> Fill)
{
	if (Path.IsEmpty() || Rows.Contains(Path))
	{
		return;
	}
	TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
	Row->SetStringField(TEXT("Path"), Path);
	Fill(*Row);
	Rows.Add(Path, Row);
}

bool ListClasses(const FString& BaseName, bool bIncludeAbstract, const FSchemaFilter& Filter, TMap<FString, TSharedPtr<FJsonObject>>& OutRows, FString& OutError)
{
	UClass* Base = UObject::StaticClass();
	if (!BaseName.IsEmpty())
	{
		Base = FindClass(BaseName);
		if (Base == nullptr)
		{
			OutError = FString::Printf(TEXT("Base class '%s' does not resolve; use a class path such as /Script/Engine.Actor."), *BaseName);
			return false;
		}
	}
	// The Asset Registry knows native classes and every Blueprint class, loaded or not (class picker).
	TSet<FTopLevelAssetPath> Derived;
	Registry().GetDerivedClassNames({Base->GetClassPathName()}, {}, Derived);
	for (const FTopLevelAssetPath& ClassPath : Derived)
	{
		const FString Path = ClassPath.ToString();
		if (IsTransientTypeName(ClassPath.GetAssetName().ToString()) || IsTestModuleType(Path))
		{
			continue;
		}
		const UClass* Loaded = FindObject<UClass>(nullptr, *Path);
		// The class viewer skips Hidden classes; a schema's node picker also HideDropDown / meta Hidden ones.
		if (Loaded != nullptr && (Loaded->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists | CLASS_Hidden)
			|| (Filter.Schema != nullptr && (Loaded->HasAnyClassFlags(CLASS_HideDropDown) || Loaded->HasMetaData(TEXT("Hidden")) || !Filter.Allows(*Loaded)))))
		{
			continue;
		}
		const bool bAbstract = Loaded != nullptr && Loaded->HasAnyClassFlags(CLASS_Abstract);
		if (bAbstract && !bIncludeAbstract)
		{
			continue;
		}
		const bool bBlueprint = !Path.StartsWith(TEXT("/Script/"));
		AddTypeRow(OutRows, Path, [bAbstract, bBlueprint](FJsonObject& Row)
		{
			if (bAbstract) Row.SetBoolField(TEXT("Abstract"), true);
			if (bBlueprint) Row.SetBoolField(TEXT("Blueprint"), true);
		});
	}
	return true;
}

void AddRegistryAssets(const UClass& AssetClass, TMap<FString, TSharedPtr<FJsonObject>>& OutRows)
{
	TArray<FAssetData> Assets;
	Registry().GetAssetsByClass(AssetClass.GetClassPathName(), Assets, true);
	for (const FAssetData& Asset : Assets)
	{
		AddTypeRow(OutRows, Asset.GetObjectPathString(), [](FJsonObject& Row) { Row.SetBoolField(TEXT("UserDefined"), true); });
	}
}

bool ListStructs(const FString& BaseName, const FSchemaFilter& Filter, TMap<FString, TSharedPtr<FJsonObject>>& OutRows, FString& OutError)
{
	const UScriptStruct* Base = nullptr;
	if (!BaseName.IsEmpty())
	{
		Base = FindStruct(BaseName);
		if (Base == nullptr)
		{
			OutError = FString::Printf(TEXT("Base struct '%s' does not resolve; use a struct path such as /Script/Engine.TableRowBase."), *BaseName);
			return false;
		}
	}
	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		// Structs with meta Hidden are the abstract bases the node pickers never offer (StateTreeTaskBase ...).
		if (IsTransientTypeName(It->GetName()) || It->IsA<UUserDefinedStruct>() || It->HasMetaData(TEXT("Hidden"))
			|| IsTestModuleType(It->GetPathName()) || !Filter.Allows(**It))
		{
			continue;
		}
		if (Base == nullptr || It->IsChildOf(Base))
		{
			AddTypeRow(OutRows, It->GetPathName(), [](FJsonObject&) {});
		}
	}
	// User Defined Structs (also unloaded ones): listed for every query, and for TableRowBase because
	// the DataTable row struct picker offers them next to the FTableRowBase subclasses.
	if (Filter.Schema == nullptr && (Base == nullptr || Base == FTableRowBase::StaticStruct()))
	{
		AddRegistryAssets(*UUserDefinedStruct::StaticClass(), OutRows);
	}
	return true;
}

void ListEnums(TMap<FString, TSharedPtr<FJsonObject>>& OutRows)
{
	for (TObjectIterator<UEnum> It; It; ++It)
	{
		if (!IsTransientTypeName(It->GetName()) && !It->IsA<UUserDefinedEnum>())
		{
			AddTypeRow(OutRows, It->GetPathName(), [](FJsonObject&) {});
		}
	}
	AddRegistryAssets(*UUserDefinedEnum::StaticClass(), OutRows);
}

// ---------------------------------------------------------------------------
// Asset facts
// ---------------------------------------------------------------------------
template<typename SocketArrayType>
TArray<TSharedPtr<FJsonValue>> SkeletonSockets(const SocketArrayType& Sockets)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	for (const USkeletalMeshSocket* Socket : Sockets)
	{
		if (Socket != nullptr)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("Name"), Socket->SocketName.ToString());
			Row->SetStringField(TEXT("Bone"), Socket->BoneName.ToString());
			Result.Add(MakeShared<FJsonValueObject>(Row));
		}
	}
	return Result;
}

TSharedPtr<FJsonObject> SkeletonFacts(const USkeleton& Skeleton)
{
	TSharedPtr<FJsonObject> Facts = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Groups;
	for (const FAnimSlotGroup& Group : Skeleton.GetSlotGroups())
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Group"), Group.GroupName.ToString());
		TArray<FString> Slots;
		for (const FName& Slot : Group.SlotNames)
		{
			Slots.Add(Slot.ToString());
		}
		Row->SetArrayField(TEXT("Slots"), StringArray(Slots));
		Groups.Add(MakeShared<FJsonValueObject>(Row));
	}
	Facts->SetArrayField(TEXT("SlotGroups"), Groups);
	Facts->SetArrayField(TEXT("Sockets"), SkeletonSockets(Skeleton.Sockets));
	const FReferenceSkeleton& Reference = Skeleton.GetReferenceSkeleton();
	TArray<TSharedPtr<FJsonValue>> Bones;
	for (int32 Index = 0; Index < Reference.GetRawBoneNum() && Index < MaxBones; ++Index)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Name"), Reference.GetBoneName(Index).ToString());
		const int32 Parent = Reference.GetParentIndex(Index);
		if (Parent != INDEX_NONE)
		{
			Row->SetStringField(TEXT("Parent"), Reference.GetBoneName(Parent).ToString());
		}
		Bones.Add(MakeShared<FJsonValueObject>(Row));
	}
	Facts->SetArrayField(TEXT("Bones"), Bones);
	TArray<TSharedPtr<FJsonValue>> VirtualBones;
	for (const FVirtualBone& Bone : Skeleton.GetVirtualBones())
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Name"), Bone.VirtualBoneName.ToString());
		Row->SetStringField(TEXT("Source"), Bone.SourceBoneName.ToString());
		Row->SetStringField(TEXT("Target"), Bone.TargetBoneName.ToString());
		VirtualBones.Add(MakeShared<FJsonValueObject>(Row));
	}
	if (!VirtualBones.IsEmpty())
	{
		Facts->SetArrayField(TEXT("VirtualBones"), VirtualBones);
	}
	return Facts;
}

FString PathOf(const UObject* Object)
{
	return Object != nullptr ? Object->GetPathName() : FString();
}

TSharedPtr<FJsonObject> AnimationFacts(const UAnimSequenceBase& Animation)
{
	TSharedPtr<FJsonObject> Facts = MakeShared<FJsonObject>();
	Facts->SetStringField(TEXT("Skeleton"), PathOf(Animation.GetSkeleton()));
	Facts->SetNumberField(TEXT("PlayLength"), Animation.GetPlayLength());
	if (const UAnimSequence* Sequence = Cast<UAnimSequence>(&Animation))
	{
		Facts->SetNumberField(TEXT("Frames"), Sequence->GetNumberOfSampledKeys());
		Facts->SetNumberField(TEXT("FrameRate"), Sequence->GetSamplingFrameRate().AsDecimal());
	}
	// RateScale: PlayLength is the asset's timeline (where its notifies are); playing takes PlayLength / |RateScale|.
	Facts->SetNumberField(TEXT("RateScale"), Animation.RateScale);
	if (!FMath::IsNearlyEqual(Animation.RateScale, 1.f) && !FMath::IsNearlyZero(Animation.RateScale))
	{
		Facts->SetNumberField(TEXT("PlayDuration"), FMath::RoundToDouble(Animation.GetPlayLength() / FMath::Abs(Animation.RateScale) * 10000.0) / 10000.0);
	}
	if (const UAnimMontage* Montage = Cast<UAnimMontage>(&Animation))
	{
		TArray<FString> Slots;
		// Each segment's real span on the montage timeline: (AnimEndTime - AnimStartTime) * LoopingCount divided by
		// |AnimPlayRate * the sequence's RateScale| (FAnimSegment::GetLength), the length the editor lays it out with.
		constexpr int32 MaxSegments = 64;
		auto Rounded = [](double Value) { return FMath::RoundToDouble(Value * 10000.0) / 10000.0; };
		TArray<TSharedPtr<FJsonValue>> Segments;
		for (const FSlotAnimationTrack& Track : Montage->SlotAnimTracks)
		{
			Slots.Add(Track.SlotName.ToString());
			for (int32 Index = 0; Index < Track.AnimTrack.AnimSegments.Num() && Segments.Num() < MaxSegments; ++Index)
			{
				const FAnimSegment& Segment = Track.AnimTrack.AnimSegments[Index];
				const UAnimSequenceBase* Reference = Segment.GetAnimReference();
				TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("Slot"), Track.SlotName.ToString());
				Row->SetNumberField(TEXT("Index"), Index);
				Row->SetStringField(TEXT("AnimReference"), PathOf(Reference));
				Row->SetNumberField(TEXT("StartPos"), Rounded(Segment.StartPos));
				Row->SetNumberField(TEXT("AnimPlayRate"), Segment.AnimPlayRate);
				Row->SetNumberField(TEXT("AnimRateScale"), Reference != nullptr ? Reference->RateScale : 1.f);
				Row->SetNumberField(TEXT("Duration"), Rounded(Segment.GetLength()));
				Row->SetNumberField(TEXT("EndPos"), Rounded(Segment.GetEndPos()));
				Segments.Add(MakeShared<FJsonValueObject>(Row));
			}
		}
		Facts->SetArrayField(TEXT("SlotNames"), StringArray(Slots));
		Facts->SetArrayField(TEXT("Segments"), Segments);
	}
	return Facts;
}

const TCHAR* MaterialParameterTypeName(EMaterialParameterType Type)
{
	switch (Type)
	{
	case EMaterialParameterType::Scalar: return TEXT("Scalar");
	case EMaterialParameterType::Vector: return TEXT("Vector");
	case EMaterialParameterType::DoubleVector: return TEXT("DoubleVector");
	case EMaterialParameterType::Texture: return TEXT("Texture");
	case EMaterialParameterType::TextureCollection: return TEXT("TextureCollection");
	case EMaterialParameterType::Font: return TEXT("Font");
	case EMaterialParameterType::RuntimeVirtualTexture: return TEXT("RuntimeVirtualTexture");
	case EMaterialParameterType::SparseVolumeTexture: return TEXT("SparseVolumeTexture");
	case EMaterialParameterType::StaticSwitch: return TEXT("StaticSwitch");
	case EMaterialParameterType::ParameterCollection: return TEXT("ParameterCollection");
	case EMaterialParameterType::StaticComponentMask: return TEXT("StaticComponentMask");
	default: return TEXT("Unknown");
	}
}

TSharedPtr<FJsonValue> ColorValue(double R, double G, double B, double A)
{
	TSharedPtr<FJsonObject> Color = MakeShared<FJsonObject>();
	Color->SetNumberField(TEXT("R"), R);
	Color->SetNumberField(TEXT("G"), G);
	Color->SetNumberField(TEXT("B"), B);
	Color->SetNumberField(TEXT("A"), A);
	return MakeShared<FJsonValueObject>(Color);
}

TSharedPtr<FJsonValue> MaterialParameterValue(const FMaterialParameterValue& Value)
{
	switch (Value.Type)
	{
	case EMaterialParameterType::Scalar: return MakeShared<FJsonValueNumber>(Value.AsScalar());
	case EMaterialParameterType::Vector:
	{
		const FLinearColor Color = Value.AsLinearColor();
		return ColorValue(Color.R, Color.G, Color.B, Color.A);
	}
	case EMaterialParameterType::DoubleVector:
	{
		const FVector4d Vector = Value.AsVector4d();
		return ColorValue(Vector.X, Vector.Y, Vector.Z, Vector.W);
	}
	case EMaterialParameterType::StaticSwitch: return MakeShared<FJsonValueBoolean>(Value.AsStaticSwitch());
	case EMaterialParameterType::StaticComponentMask:
	{
		const FStaticComponentMaskValue Mask = Value.AsStaticComponentMask();
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetBoolField(TEXT("R"), Mask.R);
		Object->SetBoolField(TEXT("G"), Mask.G);
		Object->SetBoolField(TEXT("B"), Mask.B);
		Object->SetBoolField(TEXT("A"), Mask.A);
		return MakeShared<FJsonValueObject>(Object);
	}
	case EMaterialParameterType::Font:
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("Font"), PathOf(Value.Font.Value));
		Object->SetNumberField(TEXT("Page"), Value.Font.Page);
		return MakeShared<FJsonValueObject>(Object);
	}
	case EMaterialParameterType::ParameterCollection:
		return MakeShared<FJsonValueString>(PathOf(Value.ParameterCollection));
	case EMaterialParameterType::Texture:
	case EMaterialParameterType::TextureCollection:
	case EMaterialParameterType::RuntimeVirtualTexture:
	case EMaterialParameterType::SparseVolumeTexture:
	{
		if (const UObject* Object = Value.AsTextureObject())
		{
			return MakeShared<FJsonValueString>(Object->GetPathName());
		}
		return MakeShared<FJsonValueNull>();
	}
	default: return MakeShared<FJsonValueNull>();
	}
}

const TCHAR* AssociationName(EMaterialParameterAssociation Association)
{
	switch (Association)
	{
	case EMaterialParameterAssociation::LayerParameter: return TEXT("LayerParameter");
	case EMaterialParameterAssociation::BlendParameter: return TEXT("BlendParameter");
	default: return TEXT("GlobalParameter");
	}
}

// Every parameter the material (or its parent chain) exposes, as the Material Instance Editor lists them.
TArray<TSharedPtr<FJsonValue>> MaterialParameters(const UMaterialInterface& Material)
{
	TArray<TSharedPtr<FJsonObject>> Rows;
	for (uint8 TypeIndex = 0; TypeIndex < static_cast<uint8>(EMaterialParameterType::Num); ++TypeIndex)
	{
		const EMaterialParameterType Type = static_cast<EMaterialParameterType>(TypeIndex);
		TMap<FMaterialParameterInfo, FMaterialParameterMetadata> Parameters;
		Material.GetAllParametersOfType(Type, Parameters);
		for (const TPair<FMaterialParameterInfo, FMaterialParameterMetadata>& Entry : Parameters)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("Name"), Entry.Key.Name.ToString());
			Row->SetStringField(TEXT("Type"), MaterialParameterTypeName(Type));
			if (Entry.Key.Association != EMaterialParameterAssociation::GlobalParameter)
			{
				Row->SetStringField(TEXT("Association"), AssociationName(Entry.Key.Association));
				Row->SetNumberField(TEXT("Index"), Entry.Key.Index);
			}
			if (!Entry.Value.Group.IsNone())
			{
				Row->SetStringField(TEXT("Group"), Entry.Value.Group.ToString());
			}
			if (const TSharedPtr<FJsonValue> Value = MaterialParameterValue(Entry.Value.Value))
			{
				Row->SetField(TEXT("Value"), Value);
			}
			Rows.Add(Row);
		}
	}
	Rows.Sort([](const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B)
	{
		return A->GetStringField(TEXT("Name")) < B->GetStringField(TEXT("Name"));
	});
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const TSharedPtr<FJsonObject>& Row : Rows)
	{
		Values.Add(MakeShared<FJsonValueObject>(Row));
	}
	return Values;
}

const TCHAR* BlueprintStatusName(EBlueprintStatus Status)
{
	switch (Status)
	{
	case BS_Dirty: return TEXT("Dirty");
	case BS_Error: return TEXT("Error");
	case BS_UpToDate: return TEXT("UpToDate");
	case BS_BeingCreated: return TEXT("BeingCreated");
	case BS_UpToDateWithWarnings: return TEXT("UpToDateWithWarnings");
	default: return TEXT("Unknown");
	}
}

const TCHAR* NodeSeverityName(int32 ErrorType)
{
	if (ErrorType <= EMessageSeverity::Error)
	{
		return TEXT("Error");
	}
	return ErrorType == EMessageSeverity::Info ? TEXT("Note") : TEXT("Warning");
}
}

bool FindAssets(const TMap<FString, FString>& Query, TSharedPtr<FJsonObject>& OutPayload, FString& OutError)
{
	int32 Limit = 0;
	int32 Offset = 0;
	if (!ReadPaging(Query, Limit, Offset, OutError))
	{
		return false;
	}
	FString Path = QueryValue(Query, TEXT("path"));
	const FString ClassName = QueryValue(Query, TEXT("class"));
	const FString NamePattern = QueryValue(Query, TEXT("name"));
	while (Path.Len() > 1 && Path.EndsWith(TEXT("/")))
	{
		Path.LeftChopInline(1);
	}
	if (!Path.IsEmpty() && !Path.StartsWith(TEXT("/")))
	{
		OutError = FString::Printf(TEXT("'path' must be a package path such as /Game/AI or /Engine, got '%s'."), *Path);
		return false;
	}

	FARFilter Filter;
	if (!Path.IsEmpty() && Path != TEXT("/"))
	{
		Filter.PackagePaths.Add(FName(*Path));
		Filter.bRecursivePaths = true;
	}
	if (!ClassName.IsEmpty())
	{
		const UClass* Class = FindClass(ClassName);
		if (Class == nullptr)
		{
			OutError = FString::Printf(TEXT("Class '%s' does not resolve; use an asset class path such as /Script/Engine.Skeleton (list_types kind=class lists them)."), *ClassName);
			return false;
		}
		Filter.ClassPaths.Add(Class->GetClassPathName());
		Filter.bRecursiveClasses = !QueryBool(Query, TEXT("exact_class"));
	}
	TArray<FAssetData> Assets;
	if (Filter.IsEmpty())
	{
		Registry().GetAllAssets(Assets, true);
	}
	else
	{
		Registry().GetAssets(Filter, Assets);
	}

	const FTopLevelAssetPath RedirectorClass = UObjectRedirector::StaticClass()->GetClassPathName();
	TArray<TPair<FString, int32>> Order;
	for (int32 Index = 0; Index < Assets.Num(); ++Index)
	{
		const FAssetData& Asset = Assets[Index];
		if (Asset.AssetClassPath != RedirectorClass && MatchesName(Asset.AssetName.ToString(), NamePattern))
		{
			Order.Emplace(Asset.GetObjectPathString(), Index);
		}
	}
	Order.Sort([](const TPair<FString, int32>& A, const TPair<FString, int32>& B)
	{
		return A.Key < B.Key;
	});
	TArray<FAssetData> Candidates;
	Candidates.Reserve(Order.Num());
	for (const TPair<FString, int32>& Each : Order)
	{
		Candidates.Add(MoveTemp(Assets[Each.Value]));
	}
	TArray<TSharedPtr<FJsonObject>> Rows;
	TSharedPtr<FJsonObject> UsageSummary;
	const FString Uses = QueryValue(Query, TEXT("uses"));
	if (!Uses.IsEmpty())
	{
		if (!FilterAssetsByUsage(Uses, Candidates, Rows, UsageSummary, OutError))
		{
			return false;
		}
	}
	else
	{
		for (const FAssetData& Asset : Candidates)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("ObjectPath"), Asset.GetObjectPathString());
			Row->SetStringField(TEXT("Class"), Asset.AssetClassPath.ToString());
			Rows.Add(Row);
		}
	}
	OutPayload = MakePage(TEXT("Assets"), MoveTemp(Rows), Limit, Offset);
	if (UsageSummary.IsValid())
	{
		for (const auto& Field : UsageSummary->Values)
		{
			OutPayload->SetField(Field.Key, Field.Value);
		}
	}
	return true;
}

bool ListTypes(const TMap<FString, FString>& Query, TSharedPtr<FJsonObject>& OutPayload, FString& OutError)
{
	int32 Limit = 0;
	int32 Offset = 0;
	if (!ReadPaging(Query, Limit, Offset, OutError))
	{
		return false;
	}
	const FString Kind = QueryValue(Query, TEXT("kind"));
	const FString Base = QueryValue(Query, TEXT("base"));
	FSchemaFilter Filter;
	if (!ResolveSchemaFilter(QueryValue(Query, TEXT("schema")), Filter, OutError))
	{
		return false;
	}
	// A name with spaces as the editor shows it ("Camera Cut") also finds the type whose name has
	// none (CameraCut) when nothing matches it as written; the page says so in Note.
	const FString SpacedName = QueryValue(Query, TEXT("name"));
	const FString UnspacedName = SpacedName.Replace(TEXT(" "), TEXT(""));
	auto NoteUnspaced = [&SpacedName, &UnspacedName](FJsonObject& Page)
	{
		Page.SetStringField(TEXT("Note"), FString::Printf(TEXT("Nothing is named '%s'; these match '%s'."), *SpacedName, *UnspacedName));
	};
	TMap<FString, TSharedPtr<FJsonObject>> Found;
	if (Kind.Equals(TEXT("class"), ESearchCase::IgnoreCase))
	{
		if (!ListClasses(Base, QueryBool(Query, TEXT("include_abstract")), Filter, Found, OutError))
		{
			return false;
		}
	}
	else if (Kind.Equals(TEXT("struct"), ESearchCase::IgnoreCase))
	{
		if (!ListStructs(Base, Filter, Found, OutError))
		{
			return false;
		}
	}
	else if (Kind.Equals(TEXT("enum"), ESearchCase::IgnoreCase))
	{
		if (!Base.IsEmpty() || Filter.Schema != nullptr)
		{
			OutError = TEXT("'base' and 'schema' do not apply to enums.");
			return false;
		}
		ListEnums(Found);
	}
	else if (const FAssetDocumentTypeLister* Lister = GetTypeListers().Find(FName(*Kind)))
	{
		// A provider module's kind (e.g. metasound_node): the lister filters, the rows are paged here.
		TArray<TSharedPtr<FJsonObject>> Rows;
		if (!(*Lister)(Query, Rows, OutError))
		{
			return false;
		}
		bool bUnspaced = false;
		if (Rows.IsEmpty() && UnspacedName != SpacedName && !UnspacedName.IsEmpty())
		{
			TMap<FString, FString> Unspaced = Query;
			Unspaced.Add(TEXT("name"), UnspacedName);
			if (!(*Lister)(Unspaced, Rows, OutError))
			{
				return false;
			}
			bUnspaced = !Rows.IsEmpty();
		}
		// By path; a lister that ranks its rows (a search's best matches first) gives each a Rank, which orders
		// them first and is not shown.
		const auto RowRank = [](const TSharedPtr<FJsonObject>& Row)
		{
			int32 Rank = 0;
			Row->TryGetNumberField(TEXT("Rank"), Rank);
			return Rank;
		};
		Rows.Sort([&RowRank](const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B)
		{
			const int32 RankA = RowRank(A);
			const int32 RankB = RowRank(B);
			return RankA != RankB ? RankA < RankB : A->GetStringField(TEXT("Path")) < B->GetStringField(TEXT("Path"));
		});
		for (const TSharedPtr<FJsonObject>& Row : Rows)
		{
			Row->RemoveField(TEXT("Rank"));
		}
		OutPayload = MakePage(TEXT("Types"), MoveTemp(Rows), Limit, Offset);
		if (bUnspaced)
		{
			NoteUnspaced(*OutPayload);
		}
		return true;
	}
	else
	{
		TArray<FString> Kinds = {TEXT("class"), TEXT("struct"), TEXT("enum")};
		TArray<FName> Extra;
		GetTypeListers().GetKeys(Extra);
		Extra.Sort(FNameLexicalLess());
		for (const FName& Name : Extra)
		{
			Kinds.Add(Name.ToString());
		}
		OutError = FString::Printf(TEXT("'kind' must be one of %s, got '%s'."), *FString::Join(Kinds, TEXT(", ")), *Kind);
		return false;
	}

	const FString PathPrefix = QueryValue(Query, TEXT("path"));
	TArray<TSharedPtr<FJsonObject>> Rows;
	auto Collect = [&Found, &PathPrefix, &Rows](const FString& NamePattern)
	{
		for (TPair<FString, TSharedPtr<FJsonObject>>& Entry : Found)
		{
			if (MatchesName(ShortName(Entry.Key), NamePattern) && MatchesPathPrefix(Entry.Key, PathPrefix))
			{
				Rows.Add(Entry.Value);
			}
		}
	};
	Collect(SpacedName);
	const bool bUnspaced = Rows.IsEmpty() && UnspacedName != SpacedName && !UnspacedName.IsEmpty();
	if (bUnspaced)
	{
		Collect(UnspacedName);
	}
	Rows.Sort([](const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B)
	{
		return A->GetStringField(TEXT("Path")) < B->GetStringField(TEXT("Path"));
	});
	const bool bNoted = bUnspaced && !Rows.IsEmpty();
	OutPayload = MakePage(TEXT("Types"), MoveTemp(Rows), Limit, Offset);
	if (bNoted)
	{
		NoteUnspaced(*OutPayload);
	}
	return true;
}

TSharedPtr<FJsonObject> DescribeType(const FString& Path)
{
	const FString Name = Path.TrimStartAndEnd();
	if (!Name.StartsWith(TEXT("/Script/")))
	{
		return nullptr;
	}
	if (const UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, *Name))
	{
		return DescribeStruct(*Struct);
	}
	if (const UEnum* Enum = FindObject<UEnum>(nullptr, *Name))
	{
		return DescribeEnum(*Enum);
	}
	return nullptr;
}

void AddAssetFacts(const UObject& Asset, FJsonObject& InOutPayload)
{
	auto SetIfAbsent = [&InOutPayload](const TCHAR* Key, const TSharedPtr<FJsonValue>& Value)
	{
		if (Value.IsValid() && !InOutPayload.HasField(Key))
		{
			InOutPayload.SetField(Key, Value);
		}
	};
	if (const USkeleton* Skeleton = Cast<USkeleton>(&Asset))
	{
		SetIfAbsent(TEXT("Skeleton"), MakeShared<FJsonValueObject>(SkeletonFacts(*Skeleton)));
	}
	else if (const USkeletalMesh* Mesh = Cast<USkeletalMesh>(&Asset))
	{
		TSharedPtr<FJsonObject> Facts = MakeShared<FJsonObject>();
		Facts->SetStringField(TEXT("Skeleton"), PathOf(Mesh->GetSkeleton()));
		Facts->SetArrayField(TEXT("Sockets"), SkeletonSockets(Mesh->GetMeshOnlySocketList()));
		Facts->SetObjectField(TEXT("Bounds"), AssetDocumentServerAssetFacts::DescribeBounds(Mesh->GetImportedBounds().GetBox()));
		Facts->SetArrayField(TEXT("MaterialSlots"), AssetDocumentServerAssetFacts::DescribeMaterialSlots(*Mesh));
		SetIfAbsent(TEXT("SkeletalMesh"), MakeShared<FJsonValueObject>(Facts));
	}
	else if (const UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(&Asset))
	{
		const TSharedPtr<FJsonObject> Facts = AnimationFacts(*Animation);
		// Root motion and the notifies (AssetDocumentServerAssetFacts), next to the skeleton, length and frames.
		if (const TSharedPtr<FJsonObject> More = AssetDocumentServerAssetFacts::Describe(Asset))
		{
			for (const TCHAR* Key : {TEXT("RootMotion"), TEXT("NotifyCount"), TEXT("Notifies")})
			{
				if (More->HasField(Key))
				{
					Facts->SetField(Key, More->TryGetField(Key));
				}
			}
		}
		SetIfAbsent(TEXT("Animation"), MakeShared<FJsonValueObject>(Facts));
	}
	else if (Asset.IsA<UStaticMesh>() || Asset.IsA<USoundBase>())
	{
		if (const TSharedPtr<FJsonObject> Facts = AssetDocumentServerAssetFacts::Describe(Asset))
		{
			FString Kind;
			Facts->TryGetStringField(TEXT("Kind"), Kind);
			Facts->RemoveField(TEXT("Kind"));
			SetIfAbsent(*Kind, MakeShared<FJsonValueObject>(Facts));
		}
	}
	else if (const UMaterialInterface* Material = Cast<UMaterialInterface>(&Asset))
	{
		// A profile that lists parameters itself (MaterialInstanceConstant: ParameterSchema) wins.
		if (!InOutPayload.HasField(TEXT("ParameterSchema")))
		{
			SetIfAbsent(TEXT("MaterialParameters"), MakeShared<FJsonValueArray>(MaterialParameters(*Material)));
		}
	}
	else if (const UBlueprint* Blueprint = Cast<UBlueprint>(&Asset))
	{
		const TArray<TSharedPtr<FJsonValue>> Components = DescribeMeshComponents(*Blueprint);
		if (!Components.IsEmpty())
		{
			SetIfAbsent(TEXT("MeshComponents"), MakeShared<FJsonValueArray>(Components));
		}
	}
	for (const TPair<FName, FAssetDocumentAssetFactsDescriber>& Describer : GetAssetFactsDescribers())
	{
		if (const TSharedPtr<FJsonObject> Facts = Describer.Value(Asset))
		{
			const FString Key = Describer.Key.ToString();
			SetIfAbsent(*Key, MakeShared<FJsonValueObject>(Facts));
		}
	}
	if (const TSharedPtr<FJsonObject> Compile = DescribeCompileStatus(Asset))
	{
		SetIfAbsent(TEXT("Compile"), MakeShared<FJsonValueObject>(Compile));
	}
}

namespace
{
// The Material Editor's stats / error list for the editor's shader platform, without compiling: the
// translation errors of the material resource (UpToDate, Error, or Compiling while its shader map is
// still being built).
TSharedPtr<FJsonObject> DescribeMaterialCompileStatus(const UMaterial& Material)
{
	const FMaterialResource* Resource = Material.GetMaterialResource(GMaxRHIShaderPlatform);
	if (Resource == nullptr)
	{
		return nullptr;
	}
	TSharedPtr<FJsonObject> Compile = MakeShared<FJsonObject>();
	const TArray<FString>& Errors = Resource->GetCompileErrors();
	Compile->SetStringField(TEXT("Status"), !Errors.IsEmpty() ? TEXT("Error")
		: (Resource->IsCompilationFinished() ? TEXT("UpToDate") : TEXT("Compiling")));
	TArray<TSharedPtr<FJsonValue>> Messages;
	for (const FString& Error : Errors)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Severity"), TEXT("Error"));
		Row->SetStringField(TEXT("Message"), Error);
		Messages.Add(MakeShared<FJsonValueObject>(Row));
	}
	Compile->SetArrayField(TEXT("Messages"), Messages);
	TArray<TSharedPtr<FJsonValue>> Nodes;
	for (const UMaterialExpression* Expression : Resource->GetErrorExpressions())
	{
		if (Expression != nullptr && Expression->GetOuter() == &Material)
		{
			Nodes.Add(MakeShared<FJsonValueString>(Expression->GetName()));
		}
	}
	Compile->SetArrayField(TEXT("Nodes"), Nodes);
	return Compile;
}
} // namespace

TSharedPtr<FJsonObject> DescribeCompileStatus(const UObject& Asset)
{
	if (const UMaterial* Material = Cast<UMaterial>(&Asset))
	{
		return DescribeMaterialCompileStatus(*Material);
	}
	// Registered describers first: a provider may describe its own Blueprint subclass (Control Rig) in its document's
	// terms; describers return null for every other asset.
	for (const TPair<FName, FAssetDocumentCompileStatusDescriber>& Describer : GetCompileStatusDescribers())
	{
		if (TSharedPtr<FJsonObject> Compile = Describer.Value(Asset))
		{
			return Compile;
		}
	}
	const UBlueprint* Blueprint = Cast<UBlueprint>(&Asset);
	if (Blueprint == nullptr)
	{
		return nullptr;
	}
	TSharedPtr<FJsonObject> Compile = MakeShared<FJsonObject>();
	Compile->SetStringField(TEXT("Status"), BlueprintStatusName(Blueprint->Status));
	TArray<TSharedPtr<FJsonValue>> Messages;
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (const UEdGraph* Graph : Graphs)
	{
		if (Graph == nullptr)
		{
			continue;
		}
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node == nullptr || !Node->bHasCompilerMessage || Node->ErrorMsg.IsEmpty())
			{
				continue;
			}
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("Severity"), NodeSeverityName(Node->ErrorType));
			// The compiler appends one line per message, often the same line repeatedly.
			TArray<FString> Lines;
			Node->ErrorMsg.ParseIntoArrayLines(Lines);
			TArray<FString> UniqueLines;
			for (const FString& Line : Lines)
			{
				UniqueLines.AddUnique(Line.TrimStartAndEnd());
			}
			Row->SetStringField(TEXT("Message"), FString::Join(UniqueLines, TEXT("\n")));
			Row->SetStringField(TEXT("Graph"), Graph->GetName());
			Row->SetStringField(TEXT("Node"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
			Row->SetStringField(TEXT("NodeGuid"), Node->NodeGuid.ToString(EGuidFormats::Digits));
			Messages.Add(MakeShared<FJsonValueObject>(Row));
		}
	}
	Compile->SetArrayField(TEXT("Messages"), Messages);
	return Compile;
}
}

TSharedPtr<FJsonObject> DescribeAssetDocumentTypeField(const FProperty& Property)
{
	return UE::AssetDocument::Discovery::DescribeField(Property);
}
