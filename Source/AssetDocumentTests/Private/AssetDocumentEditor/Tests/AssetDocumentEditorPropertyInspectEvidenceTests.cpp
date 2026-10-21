// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentEditorPrimitiveProvider.h"

#include "AssetDocumentEditorPropertyInspectTestTypes.h"
#include "AssetDocumentEditorProviderTestSupport.h"
#include "Misc/AutomationTest.h"
#include "Provider/AssetDocumentEditorNativeIdentity.h"
#include "Provider/AssetDocumentEditorValueDto.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorPropertyInspectEvidenceTests
{
const FAssetDocumentPrimitiveDescriptor* FindDescriptor(
	const IAssetDocumentPrimitiveProvider& Provider,
	FStringView PrimitiveId)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Provider.DescribePrimitives())
	{
		if (Descriptor.PrimitiveId == PrimitiveId)
		{
			return &Descriptor;
		}
	}
	return nullptr;
}

FAssetDocumentRuntimeValueDto MakeString(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto MakeArguments(const TCHAR* Mode, FString ClassPath)
{
	FAssetDocumentRuntimeValueDto Arguments;
	FString DiagnosticCode;
	const TArray<FAssetDocumentEditorValueField> Fields = {
		{TEXT("mode"), MakeString(Mode)},
		{TEXT("exact_asset_class_path"), MakeString(MoveTemp(ClassPath))},
	};
	verify(TryBuildAssetDocumentEditorValueObject(Fields, Arguments, DiagnosticCode));
	return Arguments;
}

const FAssetDocumentRuntimeValueDto* FindField(
	const FAssetDocumentRuntimeValueDto& Object,
	FStringView Name)
{
	const int32 Index = Object.ObjectKeys.IndexOfByPredicate(
		[Name](const FString& Key) { return Key == Name; });
	return Object.Elements.IsValidIndex(Index) ? &Object.Elements[Index] : nullptr;
}

const FAssetDocumentRuntimeValueDto* FindRow(
	const FAssetDocumentRuntimeValueDto& Rows,
	FStringView Name)
{
	for (const FAssetDocumentRuntimeValueDto& Row : Rows.Elements)
	{
		const FAssetDocumentRuntimeValueDto* RowName = FindField(Row, TEXT("name"));
		if (RowName != nullptr && RowName->StringValue == Name)
		{
			return &Row;
		}
	}
	return nullptr;
}

const FAssetDocumentRuntimeValueDto* FindRowField(
	const FAssetDocumentRuntimeValueDto& Rows,
	FStringView RowName,
	FStringView FieldName)
{
	const FAssetDocumentRuntimeValueDto* Row = FindRow(Rows, RowName);
	return Row != nullptr ? FindField(*Row, FieldName) : nullptr;
}

bool AreValuesEqual(
	const FAssetDocumentRuntimeValueDto& Left,
	const FAssetDocumentRuntimeValueDto& Right)
{
	if (Left.Kind != Right.Kind || Left.BooleanValue != Right.BooleanValue
		|| Left.IntegerValue != Right.IntegerValue || Left.NumberValue != Right.NumberValue
		|| Left.StringValue != Right.StringValue || Left.ObjectKeys != Right.ObjectKeys
		|| Left.Elements.Num() != Right.Elements.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < Left.Elements.Num(); ++Index)
	{
		if (!AreValuesEqual(Left.Elements[Index], Right.Elements[Index]))
		{
			return false;
		}
	}
	return true;
}

class FInspectObjectPayload final
	: public UE::AssetDocument::EditorPrimitive::INativeIdentityPayload
{
public:
	explicit FInspectObjectPayload(UObject& InObject)
		: Object(&InObject)
		, CanonicalPath(InObject.GetPathName())
	{
	}

	virtual FName GetPayloadTypeId() const override
	{
		return UE::AssetDocument::EditorPrimitive::AssetPayloadTypeId;
	}

	virtual bool IsAlive() const override
	{
		return Object.IsValid();
	}

	virtual bool TryGetObjectIdentity(
		UE::AssetDocument::EditorPrimitive::FObjectIdentityView& OutView) const override
	{
		UObject* Resolved = Object.Get();
		if (Resolved == nullptr)
		{
			return false;
		}
		OutView.Object = Resolved;
		OutView.CanonicalObjectPath = CanonicalPath;
		OutView.TypeRevision = static_cast<uint64>(Resolved->GetUniqueID());
		return true;
	}

private:
	TWeakObjectPtr<UObject> Object;
	FString CanonicalPath;
};

FAssetDocumentPrimitiveOutcome Invoke(
	IAssetDocumentPrimitiveProvider& Provider,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentEditorProviderTestContext& Context,
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	FAssetDocumentRuntimeValueDto Arguments,
	TOptional<FAssetDocumentOpaqueHandle> Asset = {})
{
	FAssetDocumentPrimitiveInvocation Invocation =
		MakeAssetDocumentEditorProviderTestInvocation(Descriptor, Context);
	Invocation.ValueArguments = MoveTemp(Arguments);
	if (Asset.IsSet())
	{
		Invocation.HandleArguments.Add({TEXT("asset"), Asset.GetValue()});
	}
	return Provider.Invoke(Invocation, Handles, nullptr);
}

TOptional<FAssetDocumentOpaqueHandle> IssueReadOnlyAsset(
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	UObject& Object)
{
	return Handles.Issue(
		EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::ReadOnly,
		MakeUnique<FInspectObjectPayload>(Object)).Handle;
}

bool HasReason(
	const FAssetDocumentRuntimeValueDto& Rejected,
	FStringView Name,
	FStringView Reason)
{
	const FAssetDocumentRuntimeValueDto* Row = FindRow(Rejected, Name);
	const FAssetDocumentRuntimeValueDto* Code = Row != nullptr
		? FindField(*Row, TEXT("reason_code")) : nullptr;
	return Code != nullptr && Code->StringValue == Reason;
}

bool HasDiagnosticCode(
	const FAssetDocumentPrimitiveOutcome& Outcome,
	FStringView Code)
{
	return Outcome.Diagnostics.ContainsByPredicate(
		[Code](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == Code;
		});
}

bool IsNullOrCanonicalPath(
	const FAssetDocumentRuntimeValueDto* Value,
	FStringView CanonicalPath)
{
	return Value != nullptr
		&& (Value->Kind == EAssetDocumentRuntimeValueKind::Null
			|| (Value->Kind == EAssetDocumentRuntimeValueKind::String
				&& Value->StringValue == CanonicalPath));
}

bool IsSorted(const FAssetDocumentRuntimeValueDto& Rows)
{
	FString PreviousName;
	FString PreviousClass;
	for (const FAssetDocumentRuntimeValueDto& Row : Rows.Elements)
	{
		const FAssetDocumentRuntimeValueDto* Name = FindField(Row, TEXT("name"));
		const FAssetDocumentRuntimeValueDto* DeclaringClass =
			FindField(Row, TEXT("declaring_class"));
		if (Name == nullptr || DeclaringClass == nullptr
			|| (!PreviousName.IsEmpty()
				&& (Name->StringValue < PreviousName
					|| (Name->StringValue == PreviousName
						&& DeclaringClass->StringValue < PreviousClass))))
		{
			return false;
		}
		PreviousName = Name->StringValue;
		PreviousClass = DeclaringClass->StringValue;
	}
	return true;
}

class FScopedInspectionFixtureFlags final
{
public:
	FScopedInspectionFixtureFlags()
	{
		SetFlag(TEXT("EditConstValue"), CPF_EditConst);
		SetFlag(TEXT("DeprecatedValue"), CPF_Deprecated);
	}

	~FScopedInspectionFixtureFlags()
	{
		for (const TPair<FProperty*, EPropertyFlags>& ChangedFlag : ChangedFlags)
		{
			ChangedFlag.Key->ClearPropertyFlags(ChangedFlag.Value);
		}
	}

private:
	void SetFlag(FStringView Name, EPropertyFlags Flag)
	{
		for (TFieldIterator<FProperty> It(
			UAssetDocumentEditorPropertyInspectTestObject::StaticClass()); It; ++It)
		{
			FProperty& Property = **It;
			if (Property.GetName() == Name && !Property.HasAnyPropertyFlags(Flag))
			{
				Property.SetPropertyFlags(Flag);
				ChangedFlags.Add({&Property, Flag});
				return;
			}
		}
	}

	TArray<TPair<FProperty*, EPropertyFlags>> ChangedFlags;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderPropertyInspectEvidenceTest,
	"UEAssetDoc.GenericProvider.Property.InspectEvidence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderPropertyInspectEvidenceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorPropertyInspectEvidenceTests;
	FScopedInspectionFixtureFlags FixtureFlags;
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Descriptor = FindDescriptor(
		*Provider, TEXT("editor.property.inspect_evidence.v1"));
	if (!TestNotNull(TEXT("inspection descriptor exists"), Descriptor))
	{
		return false;
	}

	const FString ClassPath =
		UAssetDocumentEditorPropertyInspectTestObject::StaticClass()->GetPathName();
	const FAssetDocumentEditorProviderTestContext ClassContext;
	FAssetDocumentEditorProviderTestHandleResolver ClassHandles(ClassContext);
	const FAssetDocumentPrimitiveOutcome ClassOnly = Invoke(
		*Provider, *Descriptor, ClassContext, ClassHandles,
		MakeArguments(TEXT("class_defaults"), ClassPath));
	TestEqual(TEXT("class-only succeeds"), ClassOnly.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	TestTrue(TEXT("class-only has no handles"), ClassOnly.HandleResults.IsEmpty());
	TestTrue(TEXT("class-only has no changed set"), ClassOnly.ChangedSet.IsEmpty());
	TestTrue(TEXT("class-only has no diagnostics"), ClassOnly.Diagnostics.IsEmpty());

	TStrongObjectPtr<UAssetDocumentEditorPropertyInspectTestObject> Live(
		NewObject<UAssetDocumentEditorPropertyInspectTestObject>(GetTransientPackage()));
	Live->IntegerValue = 99;
	Live->TextValue = FText::FromString(TEXT("live text"));
	Live->HardObjectValue = Live.Get();
	Live->SoftObjectValue = Live.Get();
	Live->HardClassValue = UAssetDocumentEditorPropertyInspectTestObject::StaticClass();
	Live->SoftClassValue = UAssetDocumentEditorPropertyInspectTestObject::StaticClass();
	Live->ArrayValue = {11, 13};
	Live->StringMapValue.Add(TEXT("string"), 17);
	Live->NameMapValue.Add(TEXT("name"), 19);
	Live->NumericMapValue.Add(23, TEXT("reject"));
	Live->SetValue.Add(TEXT("set"));
	Live->DynamicValue.InitializeAs<FAssetDocumentEditorPropertyInspectUnsupportedValue>();
	Live->InlineObject = NewObject<UAssetDocumentEditorPropertyInspectInlineObject>(
		Live.Get(), NAME_None, RF_Transient);
	const bool bDirtyBefore = Live->GetOutermost()->IsDirty();

	const FAssetDocumentEditorProviderTestContext LiveContext;
	FAssetDocumentEditorProviderTestHandleResolver LiveHandles(LiveContext);
	const TOptional<FAssetDocumentOpaqueHandle> Asset = IssueReadOnlyAsset(LiveHandles, *Live);
	if (!TestTrue(TEXT("live read-only asset handle issues"), Asset.IsSet()))
	{
		return false;
	}
	const FAssetDocumentPrimitiveOutcome LiveResult = Invoke(
		*Provider, *Descriptor, LiveContext, LiveHandles,
		MakeArguments(TEXT("live_asset"), ClassPath), Asset);
	TestEqual(TEXT("live succeeds"), LiveResult.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	TestTrue(TEXT("live has no handles"), LiveResult.HandleResults.IsEmpty());
	TestTrue(TEXT("live has no changed set"), LiveResult.ChangedSet.IsEmpty());
	TestTrue(TEXT("live has no diagnostics"), LiveResult.Diagnostics.IsEmpty());
	TestEqual(TEXT("inspection does not dirty live package"),
		Live->GetOutermost()->IsDirty(), bDirtyBefore);

	const FAssetDocumentRuntimeValueDto* ClassAdmitted = FindField(ClassOnly.Value, TEXT("admitted"));
	const FAssetDocumentRuntimeValueDto* LiveAdmitted = FindField(LiveResult.Value, TEXT("admitted"));
	const FAssetDocumentRuntimeValueDto* LiveRejected = FindField(LiveResult.Value, TEXT("rejected"));
	TestNotNull(TEXT("class admitted rows"), ClassAdmitted);
	TestNotNull(TEXT("live admitted rows"), LiveAdmitted);
	TestNotNull(TEXT("live rejected rows"), LiveRejected);
	if (ClassAdmitted == nullptr || LiveAdmitted == nullptr || LiveRejected == nullptr)
	{
		return false;
	}
	TestTrue(TEXT("admitted rows sort deterministically"), IsSorted(*LiveAdmitted));
	TestTrue(TEXT("rejected rows sort deterministically"), IsSorted(*LiveRejected));
	const FAssetDocumentRuntimeValueDto* ClassInteger = FindRow(*ClassAdmitted, TEXT("IntegerValue"));
	const FAssetDocumentRuntimeValueDto* LiveInteger = FindRow(*LiveAdmitted, TEXT("IntegerValue"));
	TestNotNull(TEXT("class integer row"), ClassInteger);
	TestNotNull(TEXT("live integer row"), LiveInteger);
	if (ClassInteger != nullptr && LiveInteger != nullptr)
	{
		const FAssetDocumentRuntimeValueDto* ClassCurrent = FindField(*ClassInteger, TEXT("current_value"));
		const FAssetDocumentRuntimeValueDto* ClassDefault = FindField(*ClassInteger, TEXT("default_value"));
		const FAssetDocumentRuntimeValueDto* LiveCurrent = FindField(*LiveInteger, TEXT("current_value"));
		const FAssetDocumentRuntimeValueDto* LiveDefault = FindField(*LiveInteger, TEXT("default_value"));
		TestTrue(TEXT("class current equals default"), ClassCurrent != nullptr && ClassDefault != nullptr
			&& AreValuesEqual(*ClassCurrent, *ClassDefault));
		TestTrue(TEXT("integer remains integer DTO"), LiveCurrent != nullptr
			&& LiveCurrent->Kind == EAssetDocumentRuntimeValueKind::Integer);
		TestTrue(TEXT("live current differs from CDO default"), LiveCurrent != nullptr
			&& LiveDefault != nullptr && !AreValuesEqual(*LiveCurrent, *LiveDefault));
	}
	const FAssetDocumentRuntimeValueDto* Text = FindRow(*LiveAdmitted, TEXT("TextValue"));
	const FAssetDocumentRuntimeValueDto* TextCodec = Text != nullptr ? FindField(*Text, TEXT("codec_id")) : nullptr;
	TestTrue(TEXT("FText uses inspection-only text codec"), TextCodec != nullptr
		&& TextCodec->StringValue == TEXT("text.v1"));
	const FString LiveObjectPath = Live->GetPathName();
	const FString LiveClassPath =
		UAssetDocumentEditorPropertyInspectTestObject::StaticClass()->GetPathName();
	TestTrue(TEXT("hard object reference is null or canonical"),
		IsNullOrCanonicalPath(
			FindRowField(*LiveAdmitted, TEXT("HardObjectValue"), TEXT("current_value")),
			LiveObjectPath));
	TestTrue(TEXT("soft object reference is null or canonical"),
		IsNullOrCanonicalPath(
			FindRowField(*LiveAdmitted, TEXT("SoftObjectValue"), TEXT("current_value")),
			LiveObjectPath));
	TestTrue(TEXT("hard class reference is null or canonical"),
		IsNullOrCanonicalPath(
			FindRowField(*LiveAdmitted, TEXT("HardClassValue"), TEXT("current_value")),
			LiveClassPath));
	TestTrue(TEXT("soft class reference is null or canonical"),
		IsNullOrCanonicalPath(
			FindRowField(*LiveAdmitted, TEXT("SoftClassValue"), TEXT("current_value")),
			LiveClassPath));
	const FAssetDocumentRuntimeValueDto* Fixed = FindRow(*LiveAdmitted, TEXT("FixedArrayValue"));
	const FAssetDocumentRuntimeValueDto* FixedCurrent = Fixed != nullptr ? FindField(*Fixed, TEXT("current_value")) : nullptr;
	TestTrue(TEXT("fixed array retains both elements"), FixedCurrent != nullptr
		&& FixedCurrent->Kind == EAssetDocumentRuntimeValueKind::Array
		&& FixedCurrent->Elements.Num() == 2);
	TestNotNull(TEXT("string-key map is admitted"), FindRow(*LiveAdmitted, TEXT("StringMapValue")));
	TestNotNull(TEXT("name-key map is admitted"), FindRow(*LiveAdmitted, TEXT("NameMapValue")));
	// Integer map keys travel as their decimal text (FPerQualityLevelInt::PerQuality).
	TestNotNull(TEXT("integer-key map is admitted"), FindRow(*LiveAdmitted, TEXT("NumericMapValue")));
	// FInstancedStruct and instanced subobjects are admitted as one whole row each
	// ({"Struct"|"Class", "Properties"}); no partial child rows are reported.
	const auto CodecOf = [&LiveAdmitted](const TCHAR* Name)
	{
		const FAssetDocumentRuntimeValueDto* Row = FindRow(*LiveAdmitted, Name);
		const FAssetDocumentRuntimeValueDto* Codec = Row != nullptr ? FindField(*Row, TEXT("codec_id")) : nullptr;
		return Codec != nullptr ? Codec->StringValue : FString();
	};
	const auto StringFieldOf = [&LiveAdmitted](const TCHAR* Name, const TCHAR* Field)
	{
		const FAssetDocumentRuntimeValueDto* Current = FindRowField(*LiveAdmitted, Name, TEXT("current_value"));
		const FAssetDocumentRuntimeValueDto* Value = Current != nullptr ? FindField(*Current, Field) : nullptr;
		return Value != nullptr && Value->Kind == EAssetDocumentRuntimeValueKind::String ? Value->StringValue : FString();
	};
	TestEqual(TEXT("active dynamic value is one instanced-struct row"),
		CodecOf(TEXT("DynamicValue")), FString(TEXT("instanced_struct.v1")));
	TestEqual(TEXT("active dynamic value names its struct"),
		StringFieldOf(TEXT("DynamicValue"), TEXT("Struct")),
		FAssetDocumentEditorPropertyInspectUnsupportedValue::StaticStruct()->GetPathName());
	TestTrue(TEXT("active dynamic value has no partial child row"),
		FindRow(*LiveAdmitted, TEXT("DynamicValue/Properties")) == nullptr
		&& FindRow(*LiveRejected, TEXT("DynamicValue")) == nullptr);
	TestEqual(TEXT("inline object is one instanced-object row"),
		CodecOf(TEXT("InlineObject")), FString(TEXT("instanced_object.v1")));
	TestEqual(TEXT("inline object names its class"),
		StringFieldOf(TEXT("InlineObject"), TEXT("Class")),
		UAssetDocumentEditorPropertyInspectInlineObject::StaticClass()->GetPathName());
	TestTrue(TEXT("edit const stays visible but non-writable"),
		[] (const FAssetDocumentRuntimeValueDto* Row)
		{
			const FAssetDocumentRuntimeValueDto* Writable = Row != nullptr
				? FindField(*Row, TEXT("writable")) : nullptr;
			return Writable != nullptr && Writable->Kind == EAssetDocumentRuntimeValueKind::Boolean
				&& !Writable->BooleanValue;
		}(FindRow(*LiveAdmitted, TEXT("EditConstValue"))));
	TestTrue(TEXT("transient code is stable"),
		HasReason(*LiveRejected, TEXT("TransientValue"), TEXT("Transient")));
	TestTrue(TEXT("duplicate transient code is stable"),
		HasReason(*LiveRejected, TEXT("DuplicateTransientValue"), TEXT("DuplicateTransient")));
	TestTrue(TEXT("deprecated code is stable"),
		HasReason(*LiveRejected, TEXT("DeprecatedValue"), TEXT("Deprecated")));
	TestTrue(TEXT("skip serialization code is stable"),
		HasReason(*LiveRejected, TEXT("SkipSerializationValue"), TEXT("SkipSerialization")));

	const FAssetDocumentPrimitiveOutcome ClassOnlyWithHandle = Invoke(
		*Provider, *Descriptor, LiveContext, LiveHandles,
		MakeArguments(TEXT("class_defaults"), ClassPath), Asset);
	TestEqual(TEXT("class-only handle fails"), ClassOnlyWithHandle.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Failed);

	FAssetDocumentEditorProviderTestContext WrongProviderContext = LiveContext;
	WrongProviderContext.ProviderId = TEXT("ai");
	FAssetDocumentEditorProviderTestHandleResolver WrongProviderHandles(WrongProviderContext);
	const FAssetDocumentPrimitiveOutcome WrongProvider = Invoke(
		*Provider, *Descriptor, LiveContext, WrongProviderHandles,
		MakeArguments(TEXT("live_asset"), ClassPath),
		IssueReadOnlyAsset(WrongProviderHandles, *Live));
	TestEqual(TEXT("wrong provider fails"), WrongProvider.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Failed);
	TestTrue(TEXT("wrong provider keeps provider mismatch"),
		HasDiagnosticCode(WrongProvider, TEXT("HandleMetadata.ProviderMismatch")));

	const TOptional<FAssetDocumentOpaqueHandle> WrongHandle = LiveHandles.Issue(
		EAssetDocumentHandleKind::Property,
		EAssetDocumentHandleAccess::ReadOnly,
		MakeUnique<FInspectObjectPayload>(*Live)).Handle;
	const FAssetDocumentPrimitiveOutcome WrongKind = Invoke(
		*Provider, *Descriptor, LiveContext, LiveHandles,
		MakeArguments(TEXT("live_asset"), ClassPath), WrongHandle);
	TestEqual(TEXT("wrong handle fails"), WrongKind.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Failed);
	TestTrue(TEXT("wrong handle is rejected by the typed descriptor"),
		HasDiagnosticCode(WrongKind, TEXT("PrimitiveInvocation.InvalidHandleArguments")));

	const FAssetDocumentPrimitiveOutcome WrongClass = Invoke(
		*Provider, *Descriptor, LiveContext, LiveHandles,
		MakeArguments(TEXT("live_asset"), UObject::StaticClass()->GetPathName()), Asset);
	TestEqual(TEXT("wrong class fails"), WrongClass.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Failed);
	TestTrue(TEXT("wrong class keeps exact-class failure"),
		HasDiagnosticCode(WrongClass, TEXT("PropertyInspection.ClassMismatch")));
	return true;
}
#endif
