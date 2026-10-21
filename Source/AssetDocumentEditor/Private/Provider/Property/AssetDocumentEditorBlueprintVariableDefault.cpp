// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "AssetDocumentEditorBlueprintVariableDefault.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Provider/Property/AssetDocumentEditorPropertyAdmission.h"
#include "Provider/Property/AssetDocumentEditorPropertyEdit.h"
#include "UObject/PropertyAccessUtil.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace EditorBlueprintVariableDefaultLocal
{
FAssetDocumentContractValidationResult Reject(const FString& Message)
{
 FAssetDocumentContractValidationResult Result;
 auto& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
 Diagnostic.Code = TEXT("AssetDocument.BlueprintProvider.VariableDefaultRejected");
 Diagnostic.Message = Message;
 return Result;
}
FAssetDocumentContractValidationResult Success()
{
 FAssetDocumentContractValidationResult Result; Result.bValid = true; return Result;
}
struct FResolved
{
 TStrongObjectPtr<UClass> Class;
 TStrongObjectPtr<UObject> CDO;
 TUniquePtr<FAssetDocumentEditorPropertyTokenPayload> Token;
};
bool ExactScalar(FProperty& Property, const FAssetDocumentRuntimeValueDto& Value)
{
 if (Property.ArrayDim != 1) return false;
 if (Property.IsA<FBoolProperty>()) return Value.Kind == EAssetDocumentRuntimeValueKind::Boolean;
 if (Property.IsA<FIntProperty>()) return Value.Kind == EAssetDocumentRuntimeValueKind::Integer
  && Value.IntegerValue >= MIN_int32 && Value.IntegerValue <= MAX_int32;
 if (Property.IsA<FDoubleProperty>()) return Value.Kind == EAssetDocumentRuntimeValueKind::Number && FMath::IsFinite(Value.NumberValue);
 return Property.IsA<FStrProperty>() && Value.Kind == EAssetDocumentRuntimeValueKind::String;
}
bool DeclarationMatchesProperty(const FEdGraphPinType& Declared, const FProperty& Property)
{
 // Stock K2 scalar category names; use the Engine pin type without adding a
 // BlueprintGraph module dependency to this Editor leaf.
 FEdGraphPinType Expected;
 if (Property.IsA<FBoolProperty>()) Expected.PinCategory = TEXT("bool");
 else if (Property.IsA<FIntProperty>()) Expected.PinCategory = TEXT("int");
 else if (Property.IsA<FDoubleProperty>())
 {
  Expected.PinCategory = TEXT("real"); Expected.PinSubCategory = TEXT("double");
 }
 else if (Property.IsA<FStrProperty>()) Expected.PinCategory = TEXT("string");
 else return false;
 return Declared == Expected && !Declared.bIsUObjectWrapper
  && !Declared.bSerializeAsSinglePrecisionFloat;
}
FAssetDocumentContractValidationResult Resolve(
 UBlueprint& BP, FName Name, const FGuid& Guid,
 const FAssetDocumentRuntimeValueDto& Value, FResolved& Out)
{
 if (!IsInGameThread() || !IsValid(&BP) || BP.GetClass() != UBlueprint::StaticClass()
  || BP.BlueprintType != BPTYPE_Normal || !Guid.IsValid() || Name.IsNone()
  || !IsValid(BP.GeneratedClass) || BP.GeneratedClass->ClassGeneratedBy != &BP
  || !Cast<UBlueprintGeneratedClass>(BP.GeneratedClass.Get()))
  return Reject(TEXT("Variable default requires an exact normal Blueprint and its current generated class."));
 int32 Matches = 0;
 FEdGraphPinType DeclaredType;
 for (const FBPVariableDescription& Variable : BP.NewVariables)
 {
  if (Variable.VarName == Name || Variable.VarGuid == Guid)
  {
   if (Variable.VarName != Name || Variable.VarGuid != Guid)
    return Reject(TEXT("Variable default declaration name/GUID binding is ambiguous."));
   DeclaredType = Variable.VarType;
   ++Matches;
  }
 }
 if (Matches != 1) return Reject(TEXT("Variable default requires one direct declaration with the exact name and GUID."));
 if (!ValidateAssetDocumentRuntimeValue(Value).bValid)
  return Reject(TEXT("Variable default requires a valid finite scalar DTO."));
 Out.Class.Reset(BP.GeneratedClass);
 Out.CDO.Reset(BP.GeneratedClass->GetDefaultObject(false));
 if (!IsValid(Out.CDO.Get()) || Out.CDO->GetClass() != Out.Class.Get()
  || !Out.CDO->HasAnyFlags(RF_ClassDefaultObject) || !Out.CDO->CanModify())
  return Reject(TEXT("Variable default requires an existing modifiable current CDO; no CDO is created."));
 const auto Fields = GetAssetDocumentEditorAdmittedPropertyFields(*Out.Class.Get());
 const auto* Field = Fields.FindByPredicate([&](const auto& Candidate)
 {
  return Candidate.Property && Candidate.Property->GetFName() == Name
   && Candidate.DeclaringClass == Out.Class.Get();
 });
 if (!Field || !Field->bReadable)
  return Reject(TEXT("Variable default requires a readable admitted property declared directly by the current generated class."));
 if (!ExactScalar(*Field->Property, Value))
  return Reject(TEXT("Variable default property must be bool, int32, double or string with a matching scalar DTO."));
 if (!DeclarationMatchesProperty(DeclaredType, *Field->Property))
  return Reject(TEXT("Variable default declaration type does not match its current generated property."));
 // The shared field projection reports instance writability. Defaults instead
 // use the existing CDO's template context, including DisableEditOnInstance.
 const bool bCDOWritable = PropertyAccessUtil::CanSetPropertyValue(Field->Property,
  PropertyAccessUtil::EditorReadOnlyFlags, Out.CDO->IsTemplate()) == EPropertyAccessResultFlags::Success;
 if (!bCDOWritable)
  return Reject(TEXT("Variable default property is not writable in the current CDO template context."));
 Out.Token = MakeUnique<FAssetDocumentEditorPropertyTokenPayload>(*Field->DeclaringClass,
  *Field->Property, Field->CodecId, Field->bReadable, bCDOWritable, Field->TypeRevision);
 return Success();
}
FAssetDocumentContractValidationResult Prepare(FResolved& Resolved,
 const FAssetDocumentRuntimeValueDto& Value, FAssetDocumentEditorPreparedPropertyWrite& Prepared)
{
 FAssetDocumentEditorPropertyTokenView Token;
 if (!Resolved.Token->TryGetView(Token)) return Reject(TEXT("Variable default property token is no longer current."));
 FAssetDocumentEditorPropertyEditResult Failure;
 if (!TryPrepareAssetDocumentEditorAdmittedPropertyValue(Token, Value, Prepared, Failure))
 {
  auto Result = Reject(TEXT("Variable default typed decoding failed."));
  Result.Diagnostics.Append(MoveTemp(Failure.Diagnostics)); return Result;
 }
 return Success();
}
bool EqualScalar(const FAssetDocumentRuntimeValueDto& A, const FAssetDocumentRuntimeValueDto& B)
{
 if (A.Kind != B.Kind) return false;
 switch (A.Kind)
 {
 case EAssetDocumentRuntimeValueKind::Boolean: return A.BooleanValue == B.BooleanValue;
 case EAssetDocumentRuntimeValueKind::Integer: return A.IntegerValue == B.IntegerValue;
 case EAssetDocumentRuntimeValueKind::Number: return A.NumberValue == B.NumberValue;
 case EAssetDocumentRuntimeValueKind::String: return A.StringValue == B.StringValue;
 default: return false;
 }
}
}
FAssetDocumentContractValidationResult PreflightAssetDocumentEditorBlueprintVariableDefault(
 UBlueprint& BP, FName Name, const FGuid& Guid, const FAssetDocumentRuntimeValueDto& Value)
{
 using namespace EditorBlueprintVariableDefaultLocal;
 FResolved Resolved;
 auto Result = Resolve(BP, Name, Guid, Value, Resolved);
 if (!Result.bValid) return Result;
 FAssetDocumentEditorPreparedPropertyWrite Prepared;
 return Prepare(Resolved, Value, Prepared);
}
FAssetDocumentContractValidationResult SetAssetDocumentEditorBlueprintVariableDefault(
 UBlueprint& BP, FName Name, const FGuid& Guid, const FAssetDocumentRuntimeValueDto& Value)
{
 using namespace EditorBlueprintVariableDefaultLocal;
 FResolved Resolved;
 auto Result = Resolve(BP, Name, Guid, Value, Resolved);
 if (!Result.bValid) return Result;
 FAssetDocumentEditorPreparedPropertyWrite Prepared;
 Result = Prepare(Resolved, Value, Prepared);
 if (!Result.bValid) return Result;
 FAssetDocumentEditorPropertyTokenView Token;
 if (!Resolved.Token->TryGetView(Token)) return Reject(TEXT("Variable default property token changed before writing."));
 auto Edit = ApplyAssetDocumentEditorPreparedPropertyWriteWithoutEnlist(*Resolved.CDO.Get(),
  Token, MoveTemp(Prepared), TEXT("blueprint.variable.default.set.v1"));
 if (!Edit.bSucceeded)
 {
  Result = Reject(TEXT("Variable default property write failed."));
  Result.Diagnostics.Append(MoveTemp(Edit.Diagnostics)); return Result;
 }
 FResolved Current;
 Result = Resolve(BP, Name, Guid, Value, Current);
 if (!Result.bValid) return Result;
 if (Current.Class.Get() != Resolved.Class.Get() || Current.CDO.Get() != Resolved.CDO.Get()
  || !Current.Token->TryGetView(Token))
  return Reject(TEXT("Variable default notification changed the generated class or CDO identity."));
 auto Readback = ReadAssetDocumentEditorAdmittedProperty(*Current.CDO.Get(), Token);
 const int32 Index = Readback.Value.ObjectKeys.IndexOfByKey(TEXT("value"));
 if (!Readback.bSucceeded || Readback.Value.Kind != EAssetDocumentRuntimeValueKind::Object
  || !Readback.Value.Elements.IsValidIndex(Index) || !EqualScalar(Readback.Value.Elements[Index], Value))
 {
  Result = Reject(TEXT("Variable default typed readback does not equal the requested value."));
  Result.Diagnostics.Append(MoveTemp(Readback.Diagnostics)); return Result;
 }
 return Success();
}
