// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Data table family primitives (provider "data", owner Data). The editor-equivalent paths:
//   DataTable rows      -> FDataTableEditorUtils (AddRow/RemoveRow/RenameRow/MoveRow) and the
//                          row editor's Modify / BroadcastPreChange(RowData) / HandleDataTableChanged /
//                          MarkPackageDirty / BroadcastPostChange(RowData) sequence (SRowEditor);
//   ParentTables        -> the Details panel (Modify, PreEditChange, PostEditChangeProperty(ValueSet)),
//                          inside FAssetDocumentEditorNonInteractiveScope (WriteParentTables);
//   CurveTable curves   -> FCurveTableEditor (Modify, Add*Curve / RenameRow / DeleteRow,
//                          PostEditChangeProperty(ValueSet));
//   StringTable entries -> FStringTableEditor (Modify, FStringTable::SetNamespace / SetSourceString /
//                          RemoveSourceString; metadata as the CSV import sets it).
// There is no inverse code: Core rolls back the whole package snapshot.
//
// DTOs (all carry "schema_version": 1):
//   data.data_table.read.v1   {offset?, limit?} -> {class, row_struct, row_struct_valid, composite, total, next_offset,
//                               rows[{name, values{}}]; at offset 0 also fields[{name, type, codec, supported,
//                               enum_values?}] and parent_tables[] (composite)}
//   data.data_table.edit.v1   {row_struct, ops[]}: {op: remove|add, row} | {op: rename, row, to} |
//                               {op: set, row, values{}} | {op: order, rows[], at?} | {op: parent_tables, tables[]}
//                               (one editor transaction per call; order with "at" places a segment)
//   data.curve_table.read.v1  {offset?, limit?} -> {class, mode, composite, total, next_offset,
//                               rows[{name, curve{}}]; at offset 0 also parent_tables[] (composite)}
//   data.curve_table.edit.v1  {ops[]}: {op: reset, mode} | {op: remove, row} | {op: rename, row, to} |
//                               {op: set, row, mode, curve{}} | {op: parent_tables, tables[]}
//   data.string_table.read.v1 {offset?, limit?} -> {namespace, total, next_offset, entries[{key, source, metadata{}}]}
//   data.string_table.edit.v1 {namespace?, ops[]}: {op: set, key, source} | {op: remove, key} |
//                               {op: meta_set, key, id, value} | {op: meta_remove, key, id}
//   data.user_struct.read.v1  {dependents?} -> {status, error_message, tooltip, members[{guid, name, type{pin type},
//                               tooltip, instance_editable, save_game, multi_line(+_allowed), widget_3d(+_allowed),
//                               value_range_allowed, invalid, metadata{ClampMin..UIMax}, cpp_type, codec, supported,
//                               default}], dependents?[{package, kind, dirty}]}
//   data.user_struct.edit.v1  {ops[]}: {op: add, name, type} | {op: rename, name, to} | {op: retype, name, type} |
//                               {op: remove, name} | {op: order, names[]} | {op: default, name, value} |
//                               {op: set, name, tooltip?, instance_editable?, save_game?, multi_line?, widget_3d?,
//                               metadata?{}} | {op: tooltip, tooltip}  -> {members, dependents[{package, kind, kept_clean}]}
//   data.user_enum.read.v1    {dependents?} -> {tooltip, bitflags, enumerators[{name, display_name, tooltip, value}],
//                               dependents?}
//   data.user_enum.edit.v1    {ops[]}: {op: add, name} | {op: rename, name, to} | {op: remove, name} |
//                               {op: order, names[]} | {op: tooltip, name, tooltip} | {op: description, tooltip} |
//                               {op: bitflags, value}  -> {enumerators, dependents[]}
//                             (members / enumerators by the name the editor shows; see AssetDocumentDataUserTypes.h
//                             for the dependents rule)
//   data.material_instance.read.v1 / edit.v1: parent and parameter overrides of a MaterialInstanceConstant
//                               (AssetDocumentDataMaterialInstancePrimitives.cpp)
//   data.material.read.v1 / edit.v1 / compile.v1: expression graph of a Material / Material Function
//                               (AssetDocumentDataMaterialPrimitives.cpp)

#include "AssetDocumentDataPrimitives.h"
#include "AssetDocumentDataUserTypes.h"

#include "AssetDocumentEditorNonInteractiveScope.h"
#include "AssetDocumentEditorPropertyValueCodec.h"
#include "ScopedTransaction.h"
#include "UObject/Class.h"
#include "UObject/EnumProperty.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::Graph::Data
{
namespace
{
constexpr uint32 DataProviderVersion = 1;

FAssetDocumentPrimitiveDescriptor MakeDescriptor(const TCHAR* PrimitiveId, bool bMutating, const TCHAR* AuthoritativeApi)
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = PrimitiveId;
	Descriptor.Version = DataProviderVersion;
	Descriptor.ProviderId = ProviderId;
	Descriptor.Owner = EAssetDocumentPrimitiveOwner::Data;
	Descriptor.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
	Descriptor.Access = bMutating ? EAssetDocumentPrimitiveAccess::Mutating : EAssetDocumentPrimitiveAccess::ReadOnly;
	if (bMutating)
	{
		Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair};
		Descriptor.MutationFootprintSchemaId = TEXT("graph.footprint.whole-package.v1");
	}
	else
	{
		Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Validate, EAssetDocumentRuntimeRequestPhase::Preflight,
			EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair,
			EAssetDocumentRuntimeRequestPhase::Verify};
	}
	Descriptor.bRequiresActiveTransaction = bMutating;
	Descriptor.InputSchemaId = Descriptor.PrimitiveId + TEXT(".input.v1");
	Descriptor.OutputSchemaId = Descriptor.PrimitiveId + TEXT(".output.v1");
	Descriptor.StableDiagnosticCodes = {
		TEXT("AD.RUNTIME.PRIMITIVE_FAILED"), TEXT("Graph.UnknownField"), TEXT("Graph.InvalidArgument"),
		TEXT("Graph.UnsupportedSchemaVersion"), TEXT("Graph.PieActive"), TEXT("Graph.ResultTooLarge"),
		TEXT("Data.UnsupportedAsset"), TEXT("Data.InvalidOp"), TEXT("Data.EncodeFailed"),
		TEXT("DataTable.RowStructMissing"), TEXT("DataTable.InvalidRowStruct"), TEXT("DataTable.RowStructImmutable"),
		TEXT("DataTable.RowNotFound"), TEXT("DataTable.RowExists"), TEXT("DataTable.InvalidRowName"),
		TEXT("DataTable.UnknownField"), TEXT("DataTable.UnsupportedField"), TEXT("DataTable.InvalidValue"), TEXT("Property.RowNotFound"), TEXT("Property.KeyNotFound"),
		TEXT("DataTable.InvalidOrder"), TEXT("DataTable.CompositeRowsReadOnly"), TEXT("DataTable.NotComposite"),
		TEXT("Composite.ParentNotFound"), TEXT("Composite.ParentRowStructMismatch"), TEXT("Composite.ParentCycle"),
		TEXT("CurveTable.RowNotFound"), TEXT("CurveTable.RowExists"), TEXT("CurveTable.InvalidRowName"),
		TEXT("CurveTable.InvalidMode"), TEXT("CurveTable.ModeMismatch"), TEXT("CurveTable.InvalidCurve"),
		TEXT("CurveTable.CompositeRowsReadOnly"), TEXT("StringTable.InvalidNamespace"), TEXT("StringTable.InvalidKey"),
		TEXT("StringTable.EmptySourceString"), TEXT("StringTable.KeyNotFound"), TEXT("StringTable.InvalidMetaDataId"),
		TEXT("UserDefinedStruct.MemberNotFound"), TEXT("UserDefinedStruct.MemberExists"), TEXT("UserDefinedStruct.InvalidMemberName"),
		TEXT("UserDefinedStruct.InvalidMemberType"), TEXT("UserDefinedStruct.LastMember"), TEXT("UserDefinedStruct.InvalidOrder"),
		TEXT("UserDefinedStruct.MultiLineNotAllowed"), TEXT("UserDefinedStruct.Widget3DNotAllowed"),
		TEXT("UserDefinedStruct.ValueRangeNotAllowed"), TEXT("UserDefinedStruct.InvalidMember"), TEXT("UserDefinedStruct.InvalidDefault"),
		TEXT("UserDefinedStruct.UnsupportedDefault"), TEXT("UserDefinedStruct.CompileFailed"),
		TEXT("UserDefinedStruct.DependentHasUnsavedChanges"), TEXT("UserDefinedEnum.EnumeratorNotFound"),
		TEXT("UserDefinedEnum.EnumeratorExists"), TEXT("UserDefinedEnum.InvalidEnumeratorName"), TEXT("UserDefinedEnum.InvalidOrder"),
		TEXT("UserDefinedEnum.DependentHasUnsavedChanges"),
		TEXT("MaterialInstance.UnknownParameter"), TEXT("MaterialInstance.AmbiguousParameter"), TEXT("MaterialInstance.InvalidValue"),
		TEXT("MaterialInstance.AssetNotFound"), TEXT("MaterialInstance.ParentNotFound"), TEXT("MaterialInstance.InvalidParent"),
		TEXT("MaterialInstance.ParentCycle"), TEXT("MaterialInstance.InteractiveEditRefused"),
		TEXT("Material.InvalidKey"), TEXT("Material.UnknownExpressionClass"), TEXT("Material.ExpressionNotAllowed"),
		TEXT("Material.CreateFailed"), TEXT("Material.ExpressionNotFound"), TEXT("Material.PinNotFound"),
		TEXT("Material.IncompatibleConnection"), TEXT("Material.ConnectionCycle"), TEXT("Material.UnknownProperty"),
		TEXT("Material.InvalidPropertyValue"), TEXT("Material.OutputsOnFunction"), TEXT("Material.InteractiveEditRefused")};
	Descriptor.RequiredConsumerIds = {TEXT("route.generic.v1")};
	Descriptor.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
	Descriptor.HandleArgumentSchemas.Add({TEXT("asset"), EAssetDocumentHandleKind::Asset,
		bMutating ? EAssetDocumentHandleAccess::Mutable : EAssetDocumentHandleAccess::ReadOnly, true});

	FAssetDocumentPrimitiveAdmissionEvidence& Evidence = Descriptor.Admission;
	Evidence.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
	Evidence.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper;
	Evidence.ExactEngineVersion = TEXT("5.7.4");
	Evidence.PhysicalOwner = EAssetDocumentPrimitiveOwner::Data;
	Evidence.PublicModule = TEXT("AssetDocumentData");
	Evidence.PublicHeader = TEXT("AssetDocumentDataProvider.h");
	Evidence.ExportedAuthoritativeApi = AuthoritativeApi;
	Evidence.AuthorityContextId = TEXT("assetdocument.editor.game-thread.editor-ready.v1");
	Evidence.PrimaryOperationId = Descriptor.PrimitiveId;
	Evidence.IdentityLifetimeContractId = TEXT("identity.request-transaction-revision-bound.v1");
	Evidence.TransactionContractId = TEXT("transaction.descriptor-governed.v1");
	Evidence.RollbackContractId = TEXT("rollback.core-package-snapshot.v1");
	Evidence.CompletionContractId = Descriptor.PrimitiveId + TEXT(".completion.v1");
	Evidence.SaveReloadEvidenceId = TEXT("UEAssetDoc.DataTable.Python");
	Evidence.FailureInjectionTestIds = {TEXT("UEAssetDoc.DataTable.Python")};
	Evidence.VerificationTestIds = {TEXT("UEAssetDoc.DataTable.Python")};
	Evidence.AuthorityFingerprint = FString::Printf(TEXT("ue-5.7.4|data|%s|%s"), PrimitiveId, AuthoritativeApi);
	return Descriptor;
}

TArray<FAssetDocumentPrimitiveDescriptor> BuildDescriptors()
{
	return {
		MakeDescriptor(TEXT("data.data_table.read.v1"), false,
			TEXT("UDataTable::GetRowMap;UDataTable::RowStruct;UCompositeDataTable::ParentTables;FDataTableEditorUtils::IsValidTableStruct")),
		MakeDescriptor(TEXT("data.data_table.edit.v1"), true,
			TEXT("FDataTableEditorUtils::AddRow;FDataTableEditorUtils::RemoveRow;FDataTableEditorUtils::RenameRow;FDataTableEditorUtils::MoveRow;FDataTableEditorUtils::BroadcastPreChange;FDataTableEditorUtils::BroadcastPostChange;UDataTable::HandleDataTableChanged;UObject::PostEditChangeProperty")),
		MakeDescriptor(TEXT("data.curve_table.read.v1"), false,
			TEXT("UCurveTable::GetRowMap;UCurveTable::GetCurveTableMode;UCompositeCurveTable::ParentTables")),
		MakeDescriptor(TEXT("data.curve_table.edit.v1"), true,
			TEXT("UCurveTable::AddRichCurve;UCurveTable::AddSimpleCurve;UCurveTable::RenameRow;UCurveTable::DeleteRow;UCurveTable::EmptyTable;UObject::PostEditChangeProperty")),
		MakeDescriptor(TEXT("data.string_table.read.v1"), false,
			TEXT("UStringTable::GetStringTable;FStringTable::EnumerateSourceStrings;FStringTable::EnumerateMetaData")),
		MakeDescriptor(TEXT("data.string_table.edit.v1"), true,
			TEXT("UStringTable::GetMutableStringTable;FStringTable::SetNamespace;FStringTable::SetSourceString;FStringTable::RemoveSourceString;FStringTable::SetMetaData;FStringTable::RemoveMetaData")),
		MakeDescriptor(TEXT("data.user_struct.read.v1"), false,
			TEXT("FStructureEditorUtils::GetVarDesc;FStructureEditorUtils::GetTooltip;FStructureEditorUtils::GetPropertyByGuid;UUserDefinedStruct::GetDefaultInstance")),
		MakeDescriptor(TEXT("data.user_struct.edit.v1"), true,
			TEXT("FStructureEditorUtils::AddVariable;FStructureEditorUtils::RemoveVariable;FStructureEditorUtils::RenameVariable;FStructureEditorUtils::ChangeVariableType;FStructureEditorUtils::ChangeVariableDefaultValue;FStructureEditorUtils::MoveVariable;FStructureEditorUtils::ChangeVariableTooltip;FStructureEditorUtils::ChangeEditableOnBPInstance;FStructureEditorUtils::ChangeSaveGameEnabled;FStructureEditorUtils::ChangeMultiLineTextEnabled;FStructureEditorUtils::Change3dWidgetEnabled;FStructureEditorUtils::SetMetaData;FStructureEditorUtils::ChangeTooltip;FBlueprintEditorUtils::PropertyValueToString")),
		MakeDescriptor(TEXT("data.user_enum.read.v1"), false,
			TEXT("UEnum::GetDisplayNameTextByIndex;UEnum::GetMetaData;UUserDefinedEnum::EnumDescription;FEnumEditorUtils::IsEnumeratorBitflagsType")),
		MakeDescriptor(TEXT("data.user_enum.edit.v1"), true,
			TEXT("FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum;FEnumEditorUtils::RemoveEnumeratorFromUserDefinedEnum;FEnumEditorUtils::MoveEnumeratorInUserDefinedEnum;FEnumEditorUtils::SetEnumeratorDisplayName;FEnumEditorUtils::SetEnumeratorBitflagsTypeState;UEnum::SetMetaData;UObject::PostEditChangeProperty")),
		MakeDescriptor(TEXT("data.material_instance.read.v1"), false,
			TEXT("UMaterialInstance::GetAllParametersOfType;UMaterialInstance::GetParameterValue;UMaterialInstance::GetStaticParameters;UMaterialInstance::GetMaterialLayers")),
		MakeDescriptor(TEXT("data.material_instance.edit.v1"), true,
			TEXT("UMaterialInstanceConstant::SetParentEditorOnly;FMaterialInstanceParameterUpdateContext::SetParameterValueEditorOnly;FMaterialInstanceParameterUpdateContext::SetMaterialLayers;UMaterialInstance::UpdateStaticPermutation;UObject::PostEditChange")),
		MakeDescriptor(TEXT("data.material.read.v1"), false,
			TEXT("UMaterial::GetExpressionCollection;UMaterialFunction::GetExpressionCollection;UMaterialExpression::GetInput;UMaterialExpression::GetOutputs;FMaterialResource::GetCompileErrors")),
		MakeDescriptor(TEXT("data.material.edit.v1"), true,
			TEXT("UMaterialEditingLibrary::CreateMaterialExpressionEx;UMaterialEditingLibrary::DeleteMaterialExpression;UMaterialEditingLibrary::DeleteMaterialExpressionInFunction;FExpressionInput::Connect;CanConnectMaterialValueTypes;UObject::PostEditChangeProperty;UMaterial::UpdateExpressionParameterName;UMaterial::PropagateExpressionParameterChanges")),
		MakeDescriptor(TEXT("data.material.compile.v1"), true,
			TEXT("UMaterialEditingLibrary::RecompileMaterial;UMaterialEditingLibrary::UpdateMaterialFunction;FMaterialResource::GetCompileErrors")),
	};
}

const FBinding Bindings[] = {
	{TEXT("data.data_table.read.v1"), &HandleDataTableRead},
	{TEXT("data.data_table.edit.v1"), &HandleDataTableEdit},
	{TEXT("data.curve_table.read.v1"), &HandleCurveTableRead},
	{TEXT("data.curve_table.edit.v1"), &HandleCurveTableEdit},
	{TEXT("data.string_table.read.v1"), &HandleStringTableRead},
	{TEXT("data.string_table.edit.v1"), &HandleStringTableEdit},
	{TEXT("data.user_struct.read.v1"), &HandleUserStructRead},
	{TEXT("data.user_struct.edit.v1"), &HandleUserStructEdit},
	{TEXT("data.user_enum.read.v1"), &HandleUserEnumRead},
	{TEXT("data.user_enum.edit.v1"), &HandleUserEnumEdit},
	{TEXT("data.material_instance.read.v1"), &HandleMaterialInstanceRead},
	{TEXT("data.material_instance.edit.v1"), &HandleMaterialInstanceEdit},
	{TEXT("data.material.read.v1"), &HandleMaterialRead},
	{TEXT("data.material.edit.v1"), &HandleMaterialEdit},
	{TEXT("data.material.compile.v1"), &HandleMaterialCompile},
};

const FProperty* FindStructField(const UScriptStruct& Struct, FStringView Key)
{
	return FindRowMember(Struct, Key);
}

// Walks a codec value alongside its property: float32 numbers -> shortest decimal
// (encode) or integral numbers -> numbers for float/double members (decode).
void Walk(const FProperty& Property, FValue& Value, bool bElement, bool bEncode)
{
	if (!bElement && Property.ArrayDim > 1)
	{
		if (Value.Kind == EAssetDocumentRuntimeValueKind::Array)
		{
			for (FValue& Element : Value.Elements)
			{
				Walk(Property, Element, true, bEncode);
			}
		}
		return;
	}
	if (Property.IsA<FFloatProperty>() || Property.IsA<FDoubleProperty>())
	{
		if (!bEncode && Value.Kind == EAssetDocumentRuntimeValueKind::Integer)
		{
			Value.Kind = EAssetDocumentRuntimeValueKind::Number;
			Value.NumberValue = static_cast<double>(Value.IntegerValue);
			Value.IntegerValue = 0;
		}
		if (bEncode && Property.IsA<FFloatProperty>() && Value.Kind == EAssetDocumentRuntimeValueKind::Number)
		{
			Value.NumberValue = ShortestFloat(static_cast<float>(Value.NumberValue));
		}
		return;
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		if (Value.Kind == EAssetDocumentRuntimeValueKind::Object && Struct->Struct != nullptr)
		{
			for (int32 Index = 0; Index < Value.ObjectKeys.Num() && Index < Value.Elements.Num(); ++Index)
			{
				if (const FProperty* Field = FindStructField(*Struct->Struct, Value.ObjectKeys[Index]))
				{
					Walk(*Field, Value.Elements[Index], false, bEncode);
				}
			}
		}
		return;
	}
	const FProperty* Inner = nullptr;
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		Inner = Array->Inner;
	}
	else if (const FSetProperty* Set = CastField<FSetProperty>(&Property))
	{
		Inner = Set->ElementProp;
	}
	else if (const FMapProperty* Map = CastField<FMapProperty>(&Property))
	{
		Inner = Map->ValueProp;
	}
	if (Inner != nullptr && (Value.Kind == EAssetDocumentRuntimeValueKind::Array || Value.Kind == EAssetDocumentRuntimeValueKind::Object))
	{
		for (FValue& Element : Value.Elements)
		{
			Walk(*Inner, Element, false, bEncode);
		}
	}
}

const UEnum* GetEnum(const FProperty& Property)
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
} // namespace

bool Fail(FCall& Call, FStringView Code, FStringView Message, FStringView Path)
{
	if (Call.Error.Code.IsEmpty())
	{
		Call.Error.Set(Code, Message, Path);
	}
	return false;
}

bool WriteParentTables(FReader& Op, UObject& Composite, FArrayProperty& Property, const TArray<UObject*>& Parents)
{
	FAssetDocumentEditorNonInteractiveScope NonInteractive;
	{
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentData", "SetParentTables", "Set Parent Tables"));
		Composite.Modify();
		Composite.PreEditChange(&Property);
		{
			const FObjectPropertyBase* Inner = CastField<FObjectPropertyBase>(Property.Inner);
			FScriptArrayHelper Helper(&Property, Property.ContainerPtrToValuePtr<void>(&Composite));
			Helper.Resize(Parents.Num());
			for (int32 Index = 0; Index < Parents.Num(); ++Index)
			{
				Inner->SetObjectPropertyValue(Helper.GetRawPtr(Index), Parents[Index]);
			}
		}
		FPropertyChangedEvent Event(&Property, EPropertyChangeType::ValueSet);
		Composite.PostEditChangeProperty(Event);
	}
	if (NonInteractive.WasInteractionRefused())
	{
		NonInteractive.DiscardCreatedPackages();
		return Op.Fail(TEXT("Composite.InteractiveEditRefused"), FString::Printf(
			TEXT("Setting ParentTables on %s tried to open an editor dialog, which an MCP request cannot answer; the edit was refused."),
			*Composite.GetClass()->GetName()), TEXT("tables"));
	}
	return true;
}

double ShortestFloat(float Value)
{
	if (!FMath::IsFinite(Value))
	{
		return Value;
	}
	for (int32 Digits = 1; Digits <= 9; ++Digits)
	{
		const FString Text = FString::Printf(TEXT("%.*g"), Digits, static_cast<double>(Value));
		const double Parsed = FCString::Atod(*Text);
		if (static_cast<float>(Parsed) == Value)
		{
			return Parsed;
		}
	}
	return Value;
}

TArray<FProperty*> GetRowMembers(const UScriptStruct& Struct)
{
	TArray<FProperty*> Members;
	for (TFieldIterator<FProperty> It(&Struct); It; ++It)
	{
		if (!It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
		{
			Members.Add(*It);
		}
	}
	return Members;
}

FProperty* FindRowMember(const UScriptStruct& Struct, FStringView AuthoredName)
{
	for (TFieldIterator<FProperty> It(&Struct); It; ++It)
	{
		if (!It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated) && FStringView(It->GetAuthoredName()).Equals(AuthoredName, ESearchCase::CaseSensitive))
		{
			return *It;
		}
	}
	return nullptr;
}

FString GetMemberCodecId(FProperty& Member)
{
	FString CodecId;
	return EditorPropertyValueCodec::Inspect(Member, CodecId) ? CodecId : FString();
}

bool EncodeMember(FProperty& Member, const void* ContainerPtr, FValue& OutValue, FString& OutCode)
{
	if (!EditorPropertyValueCodec::Encode(Member, Member.ContainerPtrToValuePtr<void>(ContainerPtr), OutValue, OutCode))
	{
		return false;
	}
	Walk(Member, OutValue, false, true);
	return true;
}

bool DecodeMember(FProperty& Member, const FValue& Value, void* ContainerPtr, FString& OutCode, UObject* Owner)
{
	FValue Coerced = Value;
	Walk(Member, Coerced, false, false);
	return EditorPropertyValueCodec::Decode(Member, Coerced, Member.ContainerPtrToValuePtr<void>(ContainerPtr), OutCode, Owner);
}

FValue DescribeMember(FProperty& Member)
{
	const FString CodecId = GetMemberCodecId(Member);
	FObjectBuilder Row;
	Row.AddString(TEXT("name"), Member.GetAuthoredName());
	Row.AddString(TEXT("type"), Member.ArrayDim > 1
		? FString::Printf(TEXT("%s[%d]"), *Member.GetCPPType(), Member.ArrayDim) : Member.GetCPPType());
	Row.AddString(TEXT("codec"), CodecId);
	Row.AddBool(TEXT("supported"), !CodecId.IsEmpty());
	if (const UEnum* Enum = GetEnum(Member))
	{
		TArray<FString> Names;
		for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
		{
			if (Enum->HasMetaData(TEXT("Hidden"), Index) || (Index == Enum->NumEnums() - 1 && Enum->ContainsExistingMax()))
			{
				continue;
			}
			Names.Add(Enum->GetNameStringByIndex(Index));
		}
		Row.Add(TEXT("enum_values"), MakeStringArray(Names));
	}
	return Row.Build();
}

FString PathOf(const UObject* Object)
{
	return Object != nullptr ? Object->GetPathName() : FString();
}

bool IsValidRowName(const FString& Name, FString& OutReason)
{
	if (Name.IsEmpty() || Name.Len() >= NAME_SIZE)
	{
		OutReason = TEXT("A name must be 1 to 1023 characters.");
		return false;
	}
	FText Reason;
	if (!FName::IsValidXName(Name, INVALID_NAME_CHARACTERS, &Reason))
	{
		OutReason = Reason.ToString();
		return false;
	}
	if (FName(*Name).IsNone())
	{
		OutReason = TEXT("'None' is not a valid name.");
		return false;
	}
	return true;
}

int32 AppendPaged(TArray<FValue>& Out, int32& NodeCount, TFunctionRef<FValue(int32)> MakeRow, int32 Offset, int32 Total, int32 Limit)
{
	int32 Index = Offset;
	for (; Index < Total && Index - Offset < Limit; ++Index)
	{
		FValue Row = MakeRow(Index);
		const int32 RowNodes = CountValueNodes(Row);
		if (!Out.IsEmpty() && NodeCount + RowNodes > QueryValueNodeBudget)
		{
			break;
		}
		NodeCount += RowNodes;
		Out.Add(MoveTemp(Row));
	}
	return Index < Total ? Index : -1;
}

TConstArrayView<FAssetDocumentPrimitiveDescriptor> GetDescriptors()
{
	static const TArray<FAssetDocumentPrimitiveDescriptor> Descriptors = BuildDescriptors();
	return Descriptors;
}

TConstArrayView<FBinding> GetBindings()
{
	return Bindings;
}
} // namespace UE::AssetDocument::Graph::Data
