// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardLoadedConsumerCompletion.h"

#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/ValueOrBBKey.h"
#include "Misc/PackageName.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

using namespace AssetDocumentAIBlackboardValueOrKeyCacheInvalidation;

namespace AssetDocumentAIBlackboardLoadedConsumerCompletion::Private
{
namespace
{
constexpr FStringView LeafLocatorDigestCodec = TEXTVIEW("assetdocument.ai.blackboard.bt_leaf_locator_digest.v1");
constexpr int32 MaximumDepth = 8;
constexpr int32 MaximumFields = 256;
constexpr int32 MaximumElements = 1024;
constexpr int32 MaximumBytes = 1024 * 1024;
}

int32 LocatorMaximumDepth() { return MaximumDepth; }
int32 LocatorMaximumFields() { return MaximumFields; }
int32 LocatorMaximumElements() { return MaximumElements; }
int32 LocatorMaximumBytes() { return MaximumBytes; }

bool SameSegment(const FTypedPathSegment& Left, const FTypedPathSegment& Right)
{
	return Left.Kind == Right.Kind && Left.DeclaringOwnerPath == Right.DeclaringOwnerPath
		&& Left.PropertyName == Right.PropertyName && Left.PropertyClassPath == Right.PropertyClassPath
		&& Left.StaticArrayOrdinal == Right.StaticArrayOrdinal && Left.ContainedStructPath == Right.ContainedStructPath && Left.Digest == Right.Digest;
}

bool SameLeafLocator(const FLeafLocator& Left, const FLeafLocator& Right)
{
	if (Left.GraphGuid != Right.GraphGuid || Left.NodeGuid != Right.NodeGuid || Left.Kind != Right.Kind || Left.Family != Right.Family
		|| Left.Segments.Num() != Right.Segments.Num()) return false;
	for (int32 Index = 0; Index < Left.Segments.Num(); ++Index)
		if (!SameSegment(Left.Segments[Index], Right.Segments[Index])) return false;
	return true;
}

FString LeafLocatorSortKey(const FLeafLocator& Locator)
{
	FString Result = Locator.GraphGuid.ToString(EGuidFormats::Digits) + TEXT("|") + Locator.NodeGuid.ToString(EGuidFormats::Digits)
		+ FString::Printf(TEXT("|%d|%d"),
			static_cast<int32>(Locator.Kind), static_cast<int32>(Locator.Family));
	for (const FTypedPathSegment& Segment : Locator.Segments)
	{
		Result += FString::Printf(TEXT("|%d|%s|%s|%s|%d|%s|%s"), static_cast<int32>(Segment.Kind),
			*Segment.DeclaringOwnerPath, *Segment.PropertyName.ToString(), *Segment.PropertyClassPath,
			Segment.StaticArrayOrdinal, *Segment.ContainedStructPath, *Segment.Digest);
	}
	return Result;
}

bool AppendBytes(TConstArrayView<uint8> Bytes, TArray<uint8>& Out, FCodecBudget& Budget)
{
	if (Bytes.Num() < 0 || Budget.Bytes > MaximumBytes - Bytes.Num()) return false;
	Budget.Bytes += Bytes.Num();
	Out.Append(Bytes.GetData(), Bytes.Num());
	return true;
}

bool AppendByte(const uint8 Byte, TArray<uint8>& Out, FCodecBudget& Budget)
{
	return AppendBytes(MakeArrayView(&Byte, 1), Out, Budget);
}

bool AppendBigEndian(uint64 Value, const int32 Width, TArray<uint8>& Out, FCodecBudget& Budget)
{
	if (Width < 1 || Width > 8) return false;
	uint8 Encoded[8];
	for (int32 Index = 0; Index < Width; ++Index)
		Encoded[Index] = static_cast<uint8>(Value >> ((Width - Index - 1) * 8));
	return AppendBytes(MakeArrayView(Encoded, Width), Out, Budget);
}

bool AppendUtf8(FStringView Text, TArray<uint8>& Out, FCodecBudget& Budget)
{
	FTCHARToUTF8 Utf8(Text.GetData(), Text.Len());
	return Utf8.Length() >= 0 && AppendBigEndian(static_cast<uint32>(Utf8.Length()), 4, Out, Budget)
		&& AppendBytes(MakeArrayView(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()), Out, Budget);
}

bool HashBytes(TConstArrayView<uint8> Bytes, FString& OutHash,
	FCodecBudget* ConsumerPreimageBudget = nullptr)
{
	static constexpr uint32 RoundConstants[64] = {
		0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
		0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
		0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
		0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
		0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
		0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
		0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
		0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
		0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
		0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
		0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
		0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
		0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
		0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
		0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
		0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
	};
	const auto RotateRight = [](const uint32 Value, const uint32 Amount)
	{
		return (Value >> Amount) | (Value << (32u - Amount));
	};
	if (Bytes.Num() < 0 || Bytes.Num() > MaximumBytes) return false;
	if (ConsumerPreimageBudget != nullptr
		&& ConsumerPreimageBudget->Bytes > MaximumBytes - Bytes.Num()) return false;
	if (ConsumerPreimageBudget != nullptr) ConsumerPreimageBudget->Bytes += Bytes.Num();
	const int32 PaddedSize = ((Bytes.Num() + 9 + 63) / 64) * 64;
	TArray<uint8> Message;
	Message.SetNumZeroed(PaddedSize);
	if (!Bytes.IsEmpty()) FMemory::Memcpy(Message.GetData(), Bytes.GetData(), Bytes.Num());
	Message[Bytes.Num()] = 0x80u;
	const uint64 BitLength = static_cast<uint64>(Bytes.Num()) * 8u;
	for (int32 Index = 0; Index < 8; ++Index)
		Message[PaddedSize - 8 + Index] = static_cast<uint8>(BitLength >> ((7 - Index) * 8));

	uint32 State[8] = {
		0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
		0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
	};
	for (int32 Offset = 0; Offset < PaddedSize; Offset += 64)
	{
		uint32 Schedule[16];
		uint32 Working[8];
		FMemory::Memcpy(Working, State, sizeof(State));
		for (int32 Index = 0; Index < 64; ++Index)
		{
			const int32 Slot = Index & 15;
			if (Index < 16)
			{
				const int32 WordOffset = Offset + Index * 4;
				Schedule[Slot] = (static_cast<uint32>(Message[WordOffset]) << 24)
					| (static_cast<uint32>(Message[WordOffset + 1]) << 16)
					| (static_cast<uint32>(Message[WordOffset + 2]) << 8)
					| static_cast<uint32>(Message[WordOffset + 3]);
			}
			else
			{
				const uint32 S0 = RotateRight(Schedule[(Slot + 1) & 15], 7)
					^ RotateRight(Schedule[(Slot + 1) & 15], 18) ^ (Schedule[(Slot + 1) & 15] >> 3);
				const uint32 S1 = RotateRight(Schedule[(Slot + 14) & 15], 17)
					^ RotateRight(Schedule[(Slot + 14) & 15], 19) ^ (Schedule[(Slot + 14) & 15] >> 10);
				Schedule[Slot] += S0 + Schedule[(Slot + 9) & 15] + S1;
			}
			const uint32 S1 = RotateRight(Working[4], 6)
				^ RotateRight(Working[4], 11) ^ RotateRight(Working[4], 25);
			const uint32 Choice = (Working[4] & Working[5])
				^ (~Working[4] & Working[6]);
			const uint32 Temp1 = Working[7] + S1 + Choice + RoundConstants[Index] + Schedule[Slot];
			const uint32 S0 = RotateRight(Working[0], 2)
				^ RotateRight(Working[0], 13) ^ RotateRight(Working[0], 22);
			const uint32 Majority = (Working[0] & Working[1])
				^ (Working[0] & Working[2]) ^ (Working[1] & Working[2]);
			const uint32 Temp2 = S0 + Majority;
			for (int32 StateIndex = 7; StateIndex > 0; --StateIndex)
				Working[StateIndex] = Working[StateIndex - 1];
			Working[4] += Temp1;
			Working[0] = Temp1 + Temp2;
		}
		for (int32 Index = 0; Index < 8; ++Index) State[Index] += Working[Index];
	}
	OutHash.Reset(64);
	for (const uint32 Word : State) OutHash += FString::Printf(TEXT("%08x"), Word);
	return OutHash.Len() == 64;
}

bool HashUtf8(FStringView Text, FString& OutHash)
{
	FTCHARToUTF8 Utf8(Text.GetData(), Text.Len());
	return HashBytes(MakeArrayView(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()), OutHash);
}

bool IsExcludedProperty(const FProperty& Property)
{
	return Property.HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient
		| CPF_TextExportTransient | CPF_SkipSerialization);
}

bool IsUnsignedIntegralProperty(const FNumericProperty& Property)
{
	return Property.IsA<FByteProperty>() || Property.IsA<FUInt16Property>() || Property.IsA<FUInt32Property>()
		|| Property.IsA<FUInt64Property>();
}

FString PropertyClassIdentity(const FProperty& Property)
{
	return TEXT("FFieldClass:") + Property.GetClass()->GetName();
}

bool ReadSelectorAllowNone(const FBlackboardKeySelector& Selector, bool& bOutAllowNone)
{
	const FProperty* Candidate = FindFProperty<FProperty>(FBlackboardKeySelector::StaticStruct(), TEXT("bNoneIsAllowedValue"));
	const FBoolProperty* AllowNone = CastField<FBoolProperty>(Candidate);
	if (AllowNone == nullptr || Candidate->GetOwnerStruct() != FBlackboardKeySelector::StaticStruct()
		|| Candidate->GetClass() != FBoolProperty::StaticClass()) return false;
	bOutAllowNone = AllowNone->GetPropertyValue_InContainer(&Selector);
	return true;
}

UScriptStruct* ExactValueOrStruct(const UScriptStruct& Struct, EExactValueOrBlackboardKeyFamily& OutFamily)
{
#define ASSETDOCUMENT_VALUE_OR_FAMILY(Type, Name) \
	if (&Struct == Type::StaticStruct()) { OutFamily = EExactValueOrBlackboardKeyFamily::Name; return Type::StaticStruct(); }
	ASSETDOCUMENT_VALUE_OR_FAMILY(FValueOrBBKey_Bool, Bool)
	ASSETDOCUMENT_VALUE_OR_FAMILY(FValueOrBBKey_Class, Class)
	ASSETDOCUMENT_VALUE_OR_FAMILY(FValueOrBBKey_Enum, Enum)
	ASSETDOCUMENT_VALUE_OR_FAMILY(FValueOrBBKey_Float, Float)
	ASSETDOCUMENT_VALUE_OR_FAMILY(FValueOrBBKey_Int32, Int32)
	ASSETDOCUMENT_VALUE_OR_FAMILY(FValueOrBBKey_Name, Name)
	ASSETDOCUMENT_VALUE_OR_FAMILY(FValueOrBBKey_String, String)
	ASSETDOCUMENT_VALUE_OR_FAMILY(FValueOrBBKey_Object, Object)
	ASSETDOCUMENT_VALUE_OR_FAMILY(FValueOrBBKey_Rotator, Rotator)
	ASSETDOCUMENT_VALUE_OR_FAMILY(FValueOrBBKey_Struct, Struct)
	ASSETDOCUMENT_VALUE_OR_FAMILY(FValueOrBBKey_Vector, Vector)
#undef ASSETDOCUMENT_VALUE_OR_FAMILY
	return nullptr;
}

FName ValueOrKey(void* Value, const EExactValueOrBlackboardKeyFamily)
{
	return Value == nullptr ? NAME_None : static_cast<FValueOrBlackboardKeyBase*>(Value)->GetKey();
}

enum class ECodecValueKind : uint8
{
	Invalid,
	Bool = 1,
	SignedInteger,
	UnsignedInteger,
	Float32,
	Float64,
	Enum,
	Name,
	String,
	HardObjectPath,
	SoftObjectPath,
	Struct,
	InstancedStruct,
	Array,
	Map,
	Selector,
	ExactValueOr,
};

bool EncodePropertyValue(const FProperty& Property, const void* Value, int32 StaticArrayOrdinal, int32 Depth, bool bMapKey,
	TArray<uint8>& Out, FCodecBudget& Budget);

bool LessBytes(TConstArrayView<uint8> Left, TConstArrayView<uint8> Right)
{
	const int32 Common = FMath::Min(Left.Num(), Right.Num());
	const int32 Comparison = Common == 0 ? 0 : FMemory::Memcmp(Left.GetData(), Right.GetData(), Common);
	return Comparison == 0 ? Left.Num() < Right.Num() : Comparison < 0;
}

bool AppendPersistentPath(const FSoftObjectPath& Path, TArray<uint8>& Out, FCodecBudget& Budget)
{
	if (Path.IsNull()) return AppendByte(0, Out, Budget);
	const FString Normalized = Path.ToString();
	const FString PackageName = Path.GetLongPackageName();
	if (!Path.IsValid() || Normalized.IsEmpty() || Normalized.Contains(TEXT("PIE_")) || !FPackageName::IsValidLongPackageName(PackageName, true)
		|| FSoftObjectPath(Normalized) != Path) return false;
	return AppendByte(1, Out, Budget) && AppendUtf8(Normalized, Out, Budget);
}

bool AppendPropertyAtom(const FProperty& Property, const int32 StaticArrayOrdinal, const ECodecValueKind Kind,
	TConstArrayView<uint8> Payload, TArray<uint8>& Out, FCodecBudget& Budget)
{
	if (Property.GetOwnerStruct() == nullptr || StaticArrayOrdinal < 0 || StaticArrayOrdinal >= Property.ArrayDim || Payload.Num() < 0
		|| Payload.Num() > MaximumBytes) return false;
	if (!AppendByte(1, Out, Budget)
		|| !AppendUtf8(PropertyClassIdentity(Property), Out, Budget)
		|| !AppendUtf8(Property.GetOwnerStruct()->GetPathName(), Out, Budget)
		|| !AppendUtf8(Property.GetName(), Out, Budget)
		|| !AppendBigEndian(static_cast<uint32>(Property.ArrayDim), 4, Out, Budget)
		|| !AppendBigEndian(static_cast<uint32>(StaticArrayOrdinal), 4, Out, Budget) || !AppendByte(static_cast<uint8>(Kind), Out, Budget)
		|| !AppendByte(Property.HasAnyPropertyFlags(CPF_EditorOnly) ? 1 : 0, Out, Budget)
		|| !AppendBigEndian(static_cast<uint32>(Payload.Num()), 4, Out, Budget)) return false;
	Out.Append(Payload.GetData(), Payload.Num());
	return true;
}

bool EncodeStructValue(const UStruct& Struct, const void* Value, const int32 Depth,
	TArray<uint8>& Out, FCodecBudget& Budget)
{
	if (Depth > MaximumDepth || Value == nullptr
		|| !AppendUtf8(Struct.GetPathName(), Out, Budget)) return false;
	TArray<const FProperty*> Fields;
	for (TFieldIterator<FProperty> It(&Struct); It; ++It) Fields.Add(*It);
	Fields.Sort([](const FProperty& Left, const FProperty& Right)
	{
		const FString LeftKey = Left.GetOwnerStruct()->GetPathName() + TEXT("|") + Left.GetName() + TEXT("|") + PropertyClassIdentity(Left);
		const FString RightKey = Right.GetOwnerStruct()->GetPathName() + TEXT("|") + Right.GetName() + TEXT("|") + PropertyClassIdentity(Right);
		return LeftKey < RightKey;
	});
	for (const FProperty* Field : Fields)
	{
		if (++Budget.Fields > MaximumFields) return false;
		if (IsExcludedProperty(*Field))
		{
			for (int32 StaticIndex = 0; StaticIndex < Field->ArrayDim; ++StaticIndex)
				if (ExcludedValueContainsAuthoredLeaf(*Field,
					Field->ContainerPtrToValuePtr<void>(Value, StaticIndex), Depth + 1, Budget)) return false;
			continue;
		}
		for (int32 StaticIndex = 0; StaticIndex < Field->ArrayDim; ++StaticIndex)
			if (!EncodePropertyValue(*Field, Field->ContainerPtrToValuePtr<void>(Value, StaticIndex),
				StaticIndex, Depth + 1, false, Out, Budget)) return false;
	}
	return true;
}

bool AppendNamePayload(const FName Name, TArray<uint8>& Out, FCodecBudget& Budget)
{
	return AppendUtf8(Name.GetPlainNameString().ToLower(), Out, Budget)
		&& AppendBigEndian(static_cast<uint32>(Name.GetNumber()), 4, Out, Budget);
}

bool EncodeSelectorValue(const FBlackboardKeySelector& Selector, TArray<uint8>& Out,
	FCodecBudget& Budget, const int32 Depth)
{
	bool bAllowNone = false;
	if (!ReadSelectorAllowNone(Selector, bAllowNone) || !AppendNamePayload(Selector.SelectedKeyName, Out, Budget)
		|| !AppendByte(bAllowNone ? 1 : 0, Out, Budget)
		|| !AppendBigEndian(static_cast<uint32>(Selector.AllowedTypes.Num()), 4, Out, Budget)) return false;
	for (const UBlackboardKeyType* Allowed : Selector.AllowedTypes)
		if (Allowed == nullptr || Allowed->GetClass()->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists)
			|| !EncodeStructValue(*Allowed->GetClass(), Allowed, Depth + 1, Out, Budget)) return false;
	return true;
}

bool EncodeIntegralValue(const FNumericProperty& Number, const void* Value, TArray<uint8>& Out, FCodecBudget& Budget)
{
	const int32 Width = Number.GetSize();
	if (Width != 1 && Width != 2 && Width != 4 && Width != 8) return false;
	const uint64 Bits = IsUnsignedIntegralProperty(Number) ? Number.GetUnsignedIntPropertyValue(Value)
		: static_cast<uint64>(Number.GetSignedIntPropertyValue(Value));
	return AppendBigEndian(Bits, Width, Out, Budget);
}

bool EncodeEnumPayload(const FProperty& Property, const void* Value, TArray<uint8>& Out, FCodecBudget& Budget)
{
	const FEnumProperty* EnumProperty = CastField<FEnumProperty>(&Property);
	const FByteProperty* ByteProperty = CastField<FByteProperty>(&Property);
	const UEnum* Enum = EnumProperty != nullptr ? EnumProperty->GetEnum()
		: ByteProperty != nullptr ? ByteProperty->Enum.Get() : nullptr;
	const FNumericProperty* Number = EnumProperty != nullptr ? EnumProperty->GetUnderlyingProperty() : ByteProperty;
	return Enum != nullptr && Number != nullptr && AppendUtf8(Enum->GetPathName(), Out, Budget)
		&& EncodeIntegralValue(*Number, Value, Out, Budget);
}

bool EncodeFloatPayload(const FNumericProperty& Number, const void* Value, const ECodecValueKind Kind,
	TArray<uint8>& Out, FCodecBudget& Budget)
{
	double Numeric = Number.GetFloatingPointPropertyValue(Value);
	if (!FMath::IsFinite(Numeric)) return false;
	if (Numeric == 0.0) Numeric = 0.0;
	if (Kind == ECodecValueKind::Float32)
	{
		return AppendBigEndian(FPlatformMath::AsUInt(static_cast<float>(Numeric)), 4, Out, Budget);
	}
	return Kind == ECodecValueKind::Float64 && AppendBigEndian(FPlatformMath::AsUInt(Numeric), 8, Out, Budget);
}

bool EncodeHardObjectPathPayload(const FObjectProperty& Property, const void* Value,
	TArray<uint8>& Out, FCodecBudget& Budget)
{
	const UObject* Object = Property.GetObjectPropertyValue(Value);
	FSoftObjectPath Path;
	if (Object != nullptr)
	{
		const UPackage* Package = Object->GetOutermost();
		const bool bStableNativeField = Object->IsA<UField>() && Object->IsNative() && Package != nullptr
			&& Package->GetName().StartsWith(TEXT("/Script/"));
		if (Package == nullptr || Package == GetTransientPackage()
			|| (Object->HasAnyFlags(RF_Transient) && !bStableNativeField)) return false;
		Path = FSoftObjectPath(Object);
	}
	return AppendPersistentPath(Path, Out, Budget);
}

bool EncodeContainerHeader(const int32 Num, TArray<uint8>& Out, FCodecBudget& Budget)
{
	if (Num < 0 || Budget.Elements > MaximumElements - Num || !AppendBigEndian(static_cast<uint32>(Num), 4, Out, Budget)) return false;
	Budget.Elements += Num;
	return true;
}

bool EncodeArrayPayload(const FArrayProperty& Property, const void* Value, const int32 Depth,
	TArray<uint8>& Out, FCodecBudget& Budget)
{
	FScriptArrayHelper Helper(&Property, Value);
	if (!EncodeContainerHeader(Helper.Num(), Out, Budget)) return false;
	for (int32 Index = 0; Index < Helper.Num(); ++Index)
		if (!EncodePropertyValue(*Property.Inner, Helper.GetRawPtr(Index),
			0, Depth + 1, false, Out, Budget)) return false;
	return true;
}

using FEncodedMapEntry = TPair<TArray<uint8>, TArray<uint8>>;

bool EncodeMapPayload(const FMapProperty& Property, const void* Value, const int32 Depth,
	TArray<uint8>& Out, FCodecBudget& Budget)
{
	FScriptMapHelper Helper(&Property, Value);
	if (!EncodeContainerHeader(Helper.Num(), Out, Budget)) return false;
	TArray<FEncodedMapEntry> Entries;
	for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
	{
		if (!Helper.IsValidIndex(Index)) continue;
		FEncodedMapEntry& Entry = Entries.AddDefaulted_GetRef();
		if (!EncodePropertyValue(*Property.KeyProp, Helper.GetKeyPtr(Index), 0, Depth + 1, true, Entry.Key, Budget)
			|| !EncodePropertyValue(*Property.ValueProp, Helper.GetValuePtr(Index), 0, Depth + 1, false, Entry.Value, Budget)) return false;
	}
	Entries.Sort([](const FEncodedMapEntry& Left, const FEncodedMapEntry& Right)
	{
		return LessBytes(Left.Key, Right.Key);
	});
	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		if (Index > 0 && Entries[Index].Key == Entries[Index - 1].Key) return false;
		Out.Append(Entries[Index].Key);
		Out.Append(Entries[Index].Value);
	}
	return true;
}

bool EncodeAs(const ECodecValueKind Kind, ECodecValueKind& OutKind, const bool bEncoded)
{
	OutKind = Kind;
	return bEncoded;
}

bool EncodePropertyPayload(const FProperty& Property, const void* Value, const int32 Depth,
	const bool bMapKey, ECodecValueKind& OutKind, TArray<uint8>& Out, FCodecBudget& Budget)
{
	if (const FBoolProperty* Bool = CastField<FBoolProperty>(&Property))
		return EncodeAs(ECodecValueKind::Bool, OutKind, AppendByte(Bool->GetPropertyValue(Value) ? 1 : 0, Out, Budget));
	if (Property.IsA<FEnumProperty>())
		return EncodeAs(ECodecValueKind::Enum, OutKind, EncodeEnumPayload(Property, Value, Out, Budget));
	if (const FNumericProperty* Number = CastField<FNumericProperty>(&Property))
	{
		if (Number->IsFloatingPoint())
		{
			if (bMapKey) return false;
			OutKind = Property.IsA<FFloatProperty>() ? ECodecValueKind::Float32 : Property.IsA<FDoubleProperty>()
				? ECodecValueKind::Float64 : ECodecValueKind::Invalid;
			return OutKind != ECodecValueKind::Invalid && EncodeFloatPayload(*Number, Value, OutKind, Out, Budget);
		}
		const FByteProperty* Byte = CastField<FByteProperty>(&Property);
		if (Byte != nullptr && Byte->Enum != nullptr)
			return EncodeAs(ECodecValueKind::Enum, OutKind, EncodeEnumPayload(Property, Value, Out, Budget));
		return EncodeAs(IsUnsignedIntegralProperty(*Number) ? ECodecValueKind::UnsignedInteger
			: ECodecValueKind::SignedInteger, OutKind, EncodeIntegralValue(*Number, Value, Out, Budget));
	}
	if (const FNameProperty* Name = CastField<FNameProperty>(&Property))
		return EncodeAs(ECodecValueKind::Name, OutKind, AppendNamePayload(Name->GetPropertyValue(Value), Out, Budget));
	if (const FStrProperty* String = CastField<FStrProperty>(&Property))
		return EncodeAs(ECodecValueKind::String, OutKind, AppendUtf8(String->GetPropertyValue(Value), Out, Budget));
	if (const FSoftObjectProperty* Soft = CastField<FSoftObjectProperty>(&Property))
		return EncodeAs(ECodecValueKind::SoftObjectPath, OutKind, AppendPersistentPath(Soft->GetPropertyValue(Value).ToSoftObjectPath(), Out, Budget));
	if (const FObjectProperty* Object = CastField<FObjectProperty>(&Property))
		return EncodeAs(ECodecValueKind::HardObjectPath, OutKind, EncodeHardObjectPathPayload(*Object, Value, Out, Budget));
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		if (bMapKey) return false;
		if (Struct->Struct == FBlackboardKeySelector::StaticStruct())
			return EncodeAs(ECodecValueKind::Selector, OutKind, EncodeSelectorValue(
				*static_cast<const FBlackboardKeySelector*>(Value), Out, Budget, Depth));
		EExactValueOrBlackboardKeyFamily Family;
		if (ExactValueOrStruct(*Struct->Struct, Family) != nullptr)
		{
			return EncodeAs(ECodecValueKind::ExactValueOr, OutKind,
				AppendByte(static_cast<uint8>(Family), Out, Budget)
					&& EncodeStructValue(*Struct->Struct, Value, Depth + 1, Out, Budget));
		}
		if (Struct->Struct == FInstancedStruct::StaticStruct())
		{
			OutKind = ECodecValueKind::InstancedStruct;
			const FInstancedStruct& Instance = *static_cast<const FInstancedStruct*>(Value);
			if (!AppendByte(Instance.IsValid() ? 1 : 0, Out, Budget)) return false;
			return !Instance.IsValid() || (AppendUtf8(Instance.GetScriptStruct()->GetPathName(), Out, Budget)
				&& EncodeStructValue(*Instance.GetScriptStruct(), Instance.GetMemory(), Depth + 1, Out, Budget));
		}
		return EncodeAs(ECodecValueKind::Struct, OutKind, EncodeStructValue(*Struct->Struct, Value, Depth + 1, Out, Budget));
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property); Array != nullptr && !bMapKey)
		return EncodeAs(ECodecValueKind::Array, OutKind, EncodeArrayPayload(*Array, Value, Depth, Out, Budget));
	if (const FMapProperty* Map = CastField<FMapProperty>(&Property); Map != nullptr && !bMapKey)
		return EncodeAs(ECodecValueKind::Map, OutKind, EncodeMapPayload(*Map, Value, Depth, Out, Budget));
	// Sets, text, delegates, interfaces, field paths, optionals, weak/lazy
	// identities and every unknown property class are deliberately rejected.
	return false;
}

bool EncodePropertyValue(const FProperty& Property, const void* Value, const int32 StaticArrayOrdinal,
	const int32 Depth, const bool bMapKey, TArray<uint8>& Out, FCodecBudget& Budget)
{
	if (Depth > MaximumDepth || Value == nullptr) return false;
	TArray<uint8> Payload;
	ECodecValueKind Kind = ECodecValueKind::Invalid;
	if (!EncodePropertyPayload(Property, Value, Depth, bMapKey, Kind, Payload, Budget)) return false;
	return AppendPropertyAtom(Property, StaticArrayOrdinal, Kind, Payload, Out, Budget);
}

bool EncodeBlackboardBTLeafLocatorDigestV1(const FProperty& Property, const void* Value, FString& OutDigest, FCodecBudget* ConsumerPreimageBudget)
{
	FCodecBudget Budget;
	TArray<uint8> Bytes;
	return AppendUtf8(LeafLocatorDigestCodec, Bytes, Budget)
		&& EncodePropertyValue(Property, Value, 0, 0, false, Bytes, Budget)
		&& HashBytes(Bytes, OutDigest, ConsumerPreimageBudget);
}

bool EncodeLeafAuthoredFingerprint(const FProperty& Property, const void* Value, FString& OutFingerprint, FCodecBudget* ConsumerPreimageBudget)
{
	FCodecBudget Budget;
	TArray<uint8> Bytes;
	return AppendUtf8(TEXTVIEW("assetdocument.ai.blackboard.bt_leaf_authored_fingerprint.v1"),
		Bytes, Budget)
		&& EncodePropertyValue(Property, Value, 0, 0, false, Bytes, Budget)
		&& HashBytes(Bytes, OutFingerprint, ConsumerPreimageBudget);
}
} // namespace AssetDocumentAIBlackboardLoadedConsumerCompletion::Private
