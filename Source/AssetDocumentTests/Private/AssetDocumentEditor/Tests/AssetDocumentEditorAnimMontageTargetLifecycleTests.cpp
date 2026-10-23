// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
// AnimMontage on the shared generic factory lifecycle: the Skeleton is the
// optional factory_context of editor.scratch.create_by_factory.v1 /
// editor.asset.create_by_factory.v2, and editor.asset.rollback_created.v1 (or
// the Core rollback seed) deletes a created Montage including its controller,
// even when the undo buffer references it.
#include "AssetDocumentEditorTargetLifecycleCatalog.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "AssetDocumentEditorCutoverTransactionPreparation.h"
#include "AssetDocumentEditorProviderTestSupport.h"
#include "Provider/Lifecycle/AssetDocumentEditorGenericFactory.h"
#include "Provider/Lifecycle/AssetDocumentEditorLifecyclePayloads.h"
#include "Provider/Cutover/AssetDocumentEditorRollbackSeed.h"
#include "Animation/AnimMontage.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Engine/SkeletalMesh.h"
#include "Factories/AnimMontageFactory.h"
#include "Misc/AutomationTest.h"
#include "ObjectTools.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "AssetDocumentEditorNativeStateTestTypes.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentMontageLifecycleTests
{
using namespace UE::AssetDocument::EditorPrimitive;
class FClassPayload final : public INativeIdentityPayload
{
public:
 FName GetPayloadTypeId() const override { return ClassPayloadTypeId; }
 bool IsAlive() const override { return true; }
 bool TryGetClassIdentity(FClassIdentityView& V) const override
 { V.Class=UAnimMontage::StaticClass(); V.CanonicalClassPath=Path; return true; }
 // CanonicalClassPath is a view: it must point at storage that outlives the call (a temporary dangles on MSVC).
 const FString Path=UAnimMontage::StaticClass()->GetPathName();
};
FAssetDocumentPrimitiveInvocation Invocation(const FAssetDocumentEditorProviderTestContext& C, const TCHAR* Id, uint32 Version, EAssetDocumentRuntimeRequestPhase Phase)
{
 FAssetDocumentPrimitiveInvocation I; I.PrimitiveId=Id; I.Version=Version; I.RequestId=C.RequestId; I.TransactionId=C.TransactionId; I.RuntimeRevision=C.RuntimeRevision; I.Phase=Phase; I.ValueArguments.Kind=EAssetDocumentRuntimeValueKind::Object; return I;
}
FAssetDocumentRuntimeValueDto String(const FString& S)
{ FAssetDocumentRuntimeValueDto V; V.Kind=EAssetDocumentRuntimeValueKind::String; V.StringValue=S; return V; }
FString FirstCode(const FAssetDocumentPrimitiveOutcome& O) { return O.Diagnostics.IsEmpty() ? FString() : O.Diagnostics[0].Code; }
FString Code(const TCHAR* Value) { return Value ? FString(Value) : FString(); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMontageGenericFactoryContextTest,"UEAssetDoc.MontageLifecycle.GenericFactoryContext",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAssetDocumentMontageGenericFactoryContextTest::RunTest(const FString&)
{
 using namespace AssetDocumentMontageLifecycleTests;
 const FString MontageClass=UAnimMontage::StaticClass()->GetPathName();
 // One class-neutral lifecycle row serves AnimMontage too.
 const auto Catalog=GetAssetDocumentEditorTargetLifecycleCatalog();
 TestEqual(TEXT("the Blueprint, Widget Blueprint and AnimBlueprint rows and the class-neutral generic row"),Catalog.Num(),4);
 TestTrue(TEXT("generic row is last"),Catalog.Last().IsClassNeutral());
 const auto* Lifecycle=FindAssetDocumentEditorTargetLifecycle(MontageClass);
 if (!TestNotNull(TEXT("Montage resolves the generic lifecycle"),Lifecycle)) return false;
 TestTrue(TEXT("generic row"),Lifecycle->IsClassNeutral());
 TestEqual(TEXT("generic lifecycle id"),Lifecycle->LifecycleId,FString(AssetDocumentEditorGenericFactoryLifecycleId));
 TestTrue(TEXT("Montage is creatable by its editor factory"),CanAssetDocumentEditorTargetLifecycleCreate(*Lifecycle,MontageClass));

 TStrongObjectPtr<USkeleton> Skeleton(NewObject<USkeleton>(GetTransientPackage(),NAME_None,RF_Transient));
 // Raw reflected configuration (AssetDocumentEditorGenericFactoryContext.cpp).
 {
  const auto Factory=NewAssetDocumentEditorGenericFactoryInstance(*UAnimMontageFactory::StaticClass());
  if (!TestTrue(TEXT("transient factory"),Factory.IsValid())) return false;
  TestEqual(TEXT("context accepted"),Code(ConfigureAssetDocumentEditorGenericFactoryContext(*Factory.Get(),*Skeleton.Get())),FString());
  TestTrue(TEXT("Skeleton lands in TargetSkeleton"),CastChecked<UAnimMontageFactory>(Factory.Get())->TargetSkeleton==Skeleton.Get());
  TestNull(TEXT("CDO untouched"),GetDefault<UAnimMontageFactory>()->TargetSkeleton.Get());
  TStrongObjectPtr<USkeletalMesh> Mesh(NewObject<USkeletalMesh>(GetTransientPackage(),NAME_None,RF_Transient));
  TestEqual(TEXT("a non-scriptable property (PreviewSkeletalMesh) is never a context"),
   Code(ConfigureAssetDocumentEditorGenericFactoryContext(*Factory.Get(),*Mesh.Get())),FString(TEXT("FactoryCreate.ContextUnsupported")));
  TestNull(TEXT("rejected context leaves the factory unchanged"),CastChecked<UAnimMontageFactory>(Factory.Get())->PreviewSkeletalMesh.Get());
  TestEqual(TEXT("the CDO is never configured"),
   Code(ConfigureAssetDocumentEditorGenericFactoryContext(*GetMutableDefault<UAnimMontageFactory>(),*Skeleton.Get())),FString(TEXT("FactoryCreate.ContextUnsupported")));
 }

 auto Provider=CreateAssetDocumentEditorPrimitiveProvider();
 FAssetDocumentEditorProviderTestContext C; FAssetDocumentEditorProviderTestHandleResolver H(C); FAssetDocumentEditorProviderTestTransaction Tx(C);
 auto Class=H.Issue(EAssetDocumentHandleKind::Class,EAssetDocumentHandleAccess::ReadOnly,MakeUnique<FClassPayload>());
 auto Skel=H.Issue(EAssetDocumentHandleKind::Asset,EAssetDocumentHandleAccess::ReadOnly,MakeAssetDocumentEditorResolvedAssetPayload(*Skeleton));
 if (!TestTrue(TEXT("handles issued"),Class.Handle.IsSet() && Skel.Handle.IsSet())) return false;

 // Scratch preview: without the context the editor factory creates nothing.
 auto Scratch=Invocation(C,TEXT("editor.scratch.create_by_factory.v1"),1,EAssetDocumentRuntimeRequestPhase::Preflight);
 Scratch.HandleArguments={{TEXT("asset_class"),Class.Handle.GetValue()}};
 auto NoContext=Provider->Invoke(Scratch,H,nullptr);
 TestEqual(TEXT("Montage factory needs its Skeleton"),FirstCode(NoContext),FString(TEXT("FactoryCreate.MissingFactoryContext")));
 Scratch.HandleArguments.Add({TEXT("factory_context"),Skel.Handle.GetValue()});
 auto Scratched=Provider->Invoke(Scratch,H,nullptr);
 for (const auto& D:Scratched.Diagnostics) AddInfo(TEXT("scratch: ")+D.Code+TEXT(" ")+D.Message);
 TestTrue(TEXT("configured factory scratch"),Scratched.Status==EAssetDocumentPrimitiveOutcomeStatus::Succeeded);

 const FString Name=TEXT("Montage_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
 const FString Path=TEXT("/Game/AssetDocumentMontageLifecycle");
 const FString ObjectPath=Path+TEXT("/")+Name+TEXT(".")+Name;
 FAssetDocumentEditorMutationIdentity Identity; TryBuildAssetDocumentEditorMutationIdentity(ObjectPath,Identity);
 auto Create=Invocation(C,TEXT("editor.asset.create_by_factory.v2"),2,EAssetDocumentRuntimeRequestPhase::Apply);
 Create.ValueArguments.ObjectKeys={TEXT("asset_name"),TEXT("package_path")}; Create.ValueArguments.Elements={String(Name),String(Path)};
 Create.HandleArguments={{TEXT("asset_class"),Class.Handle.GetValue()},{TEXT("factory_context"),Skel.Handle.GetValue()}};
 auto Created=Provider->Invoke(Create,H,&Tx);
 for (const auto& D:Created.Diagnostics) AddInfo(TEXT("create: ")+D.Code+TEXT(" ")+D.Message);
 if (!TestTrue(TEXT("AssetTools creates the Montage with its Skeleton"),Created.Status==EAssetDocumentPrimitiveOutcomeStatus::Succeeded && Created.HandleResults.Num()==3)) return false;
 auto* Montage=Cast<UAnimMontage>(FSoftObjectPath(ObjectPath).ResolveObject());
 if (!TestNotNull(TEXT("exact created Montage"),Montage)) return false;
 TestTrue(TEXT("factory context applied"),Montage->GetSkeleton()==Skeleton.Get());
 TestEqual(TEXT("factory default section"),Montage->CompositeSections.Num(),1);

 auto Clone=Invocation(C,TEXT("editor.scratch.clone_asset.v2"),2,EAssetDocumentRuntimeRequestPhase::Preflight);
 Clone.HandleArguments={{TEXT("source_asset"),Created.HandleResults[0].Handles[0]}};
 TestTrue(TEXT("generic clone previews a Montage"),Provider->Invoke(Clone,H,nullptr).Status==EAssetDocumentPrimitiveOutcomeStatus::Succeeded);

 // Created-asset rollback: a refused delete restores the controller binding.
 auto Rollback=Invocation(C,TEXT("editor.asset.rollback_created.v1"),1,EAssetDocumentRuntimeRequestPhase::Failed);
 Rollback.HandleArguments={{TEXT("asset"),Created.HandleResults[1].Handles[0]},{TEXT("package"),Created.HandleResults[2].Handles[0]}};
 UObject* OriginalModel=Montage->GetDataModelInterface().GetObject();
 IAnimationDataController* OriginalController=&Montage->GetController();
 TStrongObjectPtr<UAssetDocumentEditorNativeStateTestObject> Holder(NewObject<UAssetDocumentEditorNativeStateTestObject>(GetTransientPackage(),NAME_None,RF_Transient));
 Holder->AuthoredReference=Montage;
 TestTrue(TEXT("a live external reference refuses deletion"),Provider->Invoke(Rollback,H,&Tx).Status==EAssetDocumentPrimitiveOutcomeStatus::Failed);
 TestTrue(TEXT("refused deletion keeps the asset"),FSoftObjectPath(ObjectPath).ResolveObject()==Montage);
 TestTrue(TEXT("same controller"),&Montage->GetController()==OriginalController);
 TestTrue(TEXT("controller rebound to its model"),OriginalController->GetModelInterface().GetObject()==OriginalModel);
 Holder->AuthoredReference=nullptr; Holder.Reset();
 // A transacted edit (as populate steps make) leaves only an undo reference;
 // deletion resets the history like the editor's own delete.
 TestTrue(TEXT("transacted controller edit"),Montage->GetController().AddCurve(
  FAnimationCurveIdentifier(TEXT("UndoProbe"),ERawCurveTrackTypes::RCT_Float),AACF_Editable,true));
 bool bReferenced=false, bReferencedByUndo=false;
 ObjectTools::GatherObjectReferencersForDeletion(Montage,bReferenced,bReferencedByUndo);
 // (The controller's model binding also counts as a reference here; deletion unbinds it first.)
 TestTrue(TEXT("the undo buffer references the created Montage"),bReferencedByUndo);
 auto Rolled=Provider->Invoke(Rollback,H,&Tx);
 for (const auto& D:Rolled.Diagnostics) AddInfo(TEXT("rollback: ")+D.Code+TEXT(" ")+D.Message);
 TestTrue(TEXT("authoritative rollback"),Rolled.Status==EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
 TestTrue(TEXT("object/package/registry/file absent"),IsAssetDocumentEditorCreatedTargetFullyAbsent(CaptureAssetDocumentEditorCreatedTargetAbsence(Identity)));

 // The Core rollback seed of the generic row deletes a factory-created Montage.
 FAssetDocumentEditorProviderTestContext C2; FAssetDocumentEditorProviderTestHandleResolver H2(C2); FAssetDocumentEditorProviderTestTransaction Tx2(C2);
 const FString Name2=Name+TEXT("_Seed");
 const FString ObjectPath2=Path+TEXT("/")+Name2+TEXT(".")+Name2;
 FAssetDocumentEditorMutationIdentity Identity2; TryBuildAssetDocumentEditorMutationIdentity(ObjectPath2,Identity2);
 FAssetDocumentEditorCutoverMutationIntent Intent; Intent.RequestId=C2.RequestId; Intent.RuntimeRevision=C2.RuntimeRevision;
 Intent.CanonicalObjectPath=ObjectPath2; Intent.ExactAssetClassPath=MontageClass; Intent.FactoryId=Lifecycle->FactoryId; Intent.bCreateByFactory=true;
 auto Prepared=PrepareAssetDocumentEditorCutoverTransaction(Intent);
 if (!TestTrue(TEXT("generic create preparation"),Prepared.bPrepared && Prepared.RollbackSeeds.Num()==1)) return false;
 auto Capture=Prepared.RollbackSeeds[0]->OpenMutationCapture(C2.TransactionId);
 if (!TestTrue(TEXT("seed capture"),Capture.IsValid())) return false;
 auto Class2=H2.Issue(EAssetDocumentHandleKind::Class,EAssetDocumentHandleAccess::ReadOnly,MakeUnique<FClassPayload>());
 auto Skel2=H2.Issue(EAssetDocumentHandleKind::Asset,EAssetDocumentHandleAccess::ReadOnly,MakeAssetDocumentEditorResolvedAssetPayload(*Skeleton));
 auto Create2=Invocation(C2,TEXT("editor.asset.create_by_factory.v2"),2,EAssetDocumentRuntimeRequestPhase::Apply);
 Create2.ValueArguments.ObjectKeys={TEXT("asset_name"),TEXT("package_path")}; Create2.ValueArguments.Elements={String(Name2),String(Path)};
 Create2.HandleArguments={{TEXT("asset_class"),Class2.Handle.GetValue()},{TEXT("factory_context"),Skel2.Handle.GetValue()}};
 TestTrue(TEXT("second create"),Provider->Invoke(Create2,H2,&Tx2).Status==EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
 Capture.Reset();
 FAssetDocumentHandleInvalidation Event; Event.RequestId=C2.RequestId; Event.TransactionId=C2.TransactionId; Event.RuntimeRevision=C2.RuntimeRevision; Event.ProviderId=TEXT("editor"); Event.Reason=EAssetDocumentHandleInvalidationReason::Rollback;
 H2.Invalidate(Event);
 auto Seeded=Prepared.RollbackSeeds[0]->Rollback(TEXT("test.core.failure"));
 for (const auto& D:Seeded.Diagnostics) AddInfo(TEXT("seed: ")+D.Code+TEXT(" ")+D.Message);
 TestTrue(TEXT("Core seed rollback deletes the created Montage"),Seeded.bValid);
 TestTrue(TEXT("seed absence"),IsAssetDocumentEditorCreatedTargetFullyAbsent(CaptureAssetDocumentEditorCreatedTargetAbsence(Identity2)));
 return !HasAnyErrors();
}
#endif
