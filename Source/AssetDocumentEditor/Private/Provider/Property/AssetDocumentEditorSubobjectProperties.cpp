// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "AssetDocumentEditorNativeState.h"
#include "Provider/Property/AssetDocumentEditorPropertyAdmission.h"
#include "Provider/Property/AssetDocumentEditorPropertyEdit.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace EditorSubobjectPropertiesLocal
{
bool Fail(TArray<FAssetDocumentRuntimeDiagnostic>& Out, const TCHAR* Code, const FString& Message)
{
 auto& D = Out.AddDefaulted_GetRef();
 D.Code = Code; D.Path = TEXT("/Subobject/Properties"); D.Message = Message;
 return false;
}
bool Fail(TArray<FAssetDocumentRuntimeDiagnostic>& Out, TArray<FAssetDocumentRuntimeDiagnostic>&& Diagnostics, const TCHAR* Code)
{
 if (Diagnostics.IsEmpty()) return Fail(Out, Code, TEXT("Subobject property access failed."));
 Out.Append(MoveTemp(Diagnostics));
 return false;
}
bool Live(const UObject* Object)
{
 return IsValid(Object) && !Object->IsTemplate() && !Object->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad);
}
TUniquePtr<FAssetDocumentEditorPropertyTokenPayload> MakeToken(const FAssetDocumentEditorPropertyBatchTokenFieldView& Field)
{
 return MakeUnique<FAssetDocumentEditorPropertyTokenPayload>(*Field.DeclaringClass, *Field.Property,
  Field.CodecId, Field.bReadable, Field.bWritable, Field.TypeRevision);
}
struct FPreparedRow
{
 FString Name;
 TUniquePtr<FAssetDocumentEditorPropertyTokenPayload> Token;
 FAssetDocumentEditorPreparedPropertyWrite Write;
};
// Decodes every authored row against the class admission without touching an object.
bool PrepareRows(UClass& Class, const FAssetDocumentRuntimeValueDto& Properties, TArray<FPreparedRow>& OutRows,
 TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
 if (Properties.Kind != EAssetDocumentRuntimeValueKind::Object || Properties.ObjectKeys.Num() != Properties.Elements.Num()
  || !ValidateAssetDocumentRuntimeValue(Properties).bValid)
  return Fail(OutDiagnostics, TEXT("PrimitiveInvocation.InvalidValueShape"), TEXT("Subobject properties require an object of authored field names and typed values."));
 const auto Fields = GetAssetDocumentEditorAdmittedPropertyFields(Class);
 TSet<FString> Seen;
 for (int32 Index = 0; Index < Properties.ObjectKeys.Num(); ++Index)
 {
  const FString& Name = Properties.ObjectKeys[Index];
  if (Seen.Contains(Name)) return Fail(OutDiagnostics, TEXT("PropertyBatchWrite.DuplicateProperty"), FString::Printf(TEXT("Property '%s' is duplicated."), *Name));
  Seen.Add(Name);
  const auto* Field = Fields.FindByPredicate([&](const auto& Candidate)
   { return Candidate.Property && Candidate.Property->GetAuthoredName() == Name; });
  if (!Field || !Field->DeclaringClass || !Field->bWritable)
   return Fail(OutDiagnostics, TEXT("PropertyBatchWrite.UnknownProperty"),
    FString::Printf(TEXT("Property '%s' is not an admitted writable property of %s."), *Name, *Class.GetName()));
  auto& Row = OutRows.AddDefaulted_GetRef();
  Row.Name = Name; Row.Token = MakeToken(*Field);
 }
 for (int32 Index = 0; Index < OutRows.Num(); ++Index)
 {
  FAssetDocumentEditorPropertyTokenView Token;
  if (!OutRows[Index].Token->TryGetView(Token))
   return Fail(OutDiagnostics, TEXT("PropertyToken.InvalidPayload"), TEXT("The subobject property token is no longer current."));
  FAssetDocumentEditorPropertyEditResult Failure;
  if (!TryPrepareAssetDocumentEditorAdmittedPropertyValue(Token, Properties.Elements[Index], OutRows[Index].Write, Failure))
   return Fail(OutDiagnostics, MoveTemp(Failure.Diagnostics), TEXT("PropertyWrite.DecodeFailed"));
 }
 return true;
}
}

bool ReadAssetDocumentEditorSubobjectProperties(UObject& Subobject, FAssetDocumentRuntimeValueDto& OutProperties,
 TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
 using namespace EditorSubobjectPropertiesLocal;
 OutProperties = {};
 OutProperties.Kind = EAssetDocumentRuntimeValueKind::Object;
 if (!IsInGameThread() || !Live(&Subobject))
  return Fail(OutDiagnostics, TEXT("PropertyRead.AccessFailed"), TEXT("Subobject reads require a live object on the game thread."));
 TStrongObjectPtr<UObject> Pinned(&Subobject);
 for (const auto& Field : GetAssetDocumentEditorAdmittedPropertyFields(*Subobject.GetClass()))
 {
  if (!Field.bReadable || !Field.bWritable) continue;
  if (!Field.Property || !Field.DeclaringClass)
   return Fail(OutDiagnostics, TEXT("PropertyToken.InvalidPayload"), TEXT("An admitted property field is unavailable."));
  const auto Payload = MakeToken(Field);
  FAssetDocumentEditorPropertyTokenView Token;
  if (!Payload->TryGetView(Token) || !Subobject.IsA(Token.DeclaringClass))
   return Fail(OutDiagnostics, TEXT("PropertyToken.InvalidPayload"), TEXT("The subobject property token is not current."));
  auto Read = ReadAssetDocumentEditorAdmittedProperty(Subobject, Token);
  if (!Read.bSucceeded) return Fail(OutDiagnostics, MoveTemp(Read.Diagnostics), TEXT("PropertyRead.AccessFailed"));
  const FString Name = Field.Property->GetAuthoredName();
  const int32 ValueIndex = Read.Value.ObjectKeys.IndexOfByKey(TEXT("value"));
  if (Read.Value.Kind != EAssetDocumentRuntimeValueKind::Object || !Read.Value.Elements.IsValidIndex(ValueIndex)
   || OutProperties.ObjectKeys.Contains(Name))
   return Fail(OutDiagnostics, TEXT("PropertyRead.EncodeFailed"), TEXT("The admitted property readback is not unique and complete."));
  OutProperties.ObjectKeys.Add(Name);
  OutProperties.Elements.Add(MoveTemp(Read.Value.Elements[ValueIndex]));
 }
 return true;
}

bool ValidateAssetDocumentEditorSubobjectProperties(UClass& SubobjectClass, const FAssetDocumentRuntimeValueDto& Properties,
 TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
 using namespace EditorSubobjectPropertiesLocal;
 TArray<FPreparedRow> Rows;
 return PrepareRows(SubobjectClass, Properties, Rows, OutDiagnostics);
}

bool WriteAssetDocumentEditorSubobjectProperties(UObject& Subobject, const FAssetDocumentRuntimeValueDto& Properties,
 FStringView AuthorityId, TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
 using namespace EditorSubobjectPropertiesLocal;
 if (!IsInGameThread() || !Live(&Subobject))
  return Fail(OutDiagnostics, TEXT("PropertyWrite.AccessFailed"), TEXT("Subobject writes require a live object on the game thread."));
 TStrongObjectPtr<UObject> Pinned(&Subobject);
 TArray<FPreparedRow> Rows;
 if (!PrepareRows(*Subobject.GetClass(), Properties, Rows, OutDiagnostics)) return false;
 for (auto& Row : Rows)
 {
  FAssetDocumentEditorPropertyTokenView Token;
  if (!Live(&Subobject) || !Row.Token->TryGetView(Token) || !Token.bWritable || !Subobject.IsA(Token.DeclaringClass))
   return Fail(OutDiagnostics, TEXT("PropertyToken.InvalidPayload"), TEXT("The subobject property token is no longer current."));
  auto Edit = ApplyAssetDocumentEditorPreparedPropertyWriteWithoutEnlist(Subobject, Token, MoveTemp(Row.Write), AuthorityId);
  if (!Edit.bSucceeded) return Fail(OutDiagnostics, MoveTemp(Edit.Diagnostics), TEXT("PropertyWrite.AccessFailed"));
 }
 return true;
}
