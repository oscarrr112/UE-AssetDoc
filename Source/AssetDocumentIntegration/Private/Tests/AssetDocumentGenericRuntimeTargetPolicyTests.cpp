// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "AssetDocumentRuntimeTargetActivation.h"
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTargetPolicyMatrixTest,
 "UEAssetDoc.GenericRuntime.TargetLifecycle.PolicyMatrix",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentTargetPolicyMatrixTest::RunTest(const FString& Parameters)
{
 (void)Parameters;
 using P = EAssetDocumentRuntimeTargetPolicy;
 struct FCase { P Policy; const TCHAR* Action; bool Exists; bool Accepted; bool Create; const TCHAR* Code; };
 const FCase Cases[] = {
  {P::CreateOrUpdate,TEXT("Create"),false,true,true,TEXT("")},
  {P::CreateOrUpdate,TEXT("Create"),true,false,false,TEXT("GenericRuntime.CreateTargetExists")},
  {P::CreateOrUpdate,TEXT("Update"),false,false,false,TEXT("GenericRuntime.UpdateTargetMissing")},
  {P::CreateOrUpdate,TEXT("Update"),true,true,false,TEXT("")},
  {P::CreateOrUpdate,TEXT("CreateOrUpdate"),false,true,true,TEXT("")},
  {P::CreateOrUpdate,TEXT("CreateOrUpdate"),true,true,false,TEXT("")},
  {P::UpdateOnly,TEXT("Create"),false,false,false,TEXT("GenericRuntime.UpdateOnlyActionRequired")},
  {P::UpdateOnly,TEXT("Create"),true,false,false,TEXT("GenericRuntime.UpdateOnlyActionRequired")},
  {P::UpdateOnly,TEXT("Update"),false,false,false,TEXT("GenericRuntime.UpdateTargetMissing")},
  {P::UpdateOnly,TEXT("Update"),true,true,false,TEXT("")},
  {P::UpdateOnly,TEXT("CreateOrUpdate"),false,false,false,TEXT("GenericRuntime.UpdateOnlyActionRequired")},
  {P::UpdateOnly,TEXT("CreateOrUpdate"),true,false,false,TEXT("GenericRuntime.UpdateOnlyActionRequired")},
  {P::CreateOrUpdate,TEXT("update"),true,false,false,TEXT("GenericRuntime.TargetActivationInvalid")},
  {static_cast<P>(255),TEXT("Update"),true,false,false,TEXT("GenericRuntime.TargetPolicyInvalid")}};
 for (const auto& C : Cases)
 {
  TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
  const auto Result = SelectAssetDocumentRuntimeTargetDecision(C.Policy, C.Action, C.Exists, Diagnostics);
  const FString Label = FString::Printf(TEXT("policy=%d action=%s exists=%d"), static_cast<int32>(C.Policy), C.Action, C.Exists);
  TestEqual(*Label, Result.IsSet(), C.Accepted);
  if (C.Accepted && Result.IsSet()) { TestEqual(*(Label + TEXT(" decision")), Result.GetValue(), C.Create); TestTrue(TEXT("accepted has no diagnostics"), Diagnostics.IsEmpty()); }
  else if (!C.Accepted) { TestEqual(*(Label + TEXT(" diagnostic")), Diagnostics.IsEmpty() ? FString() : Diagnostics[0].Code, FString(C.Code)); }
 }
 return true;
}
#endif
