// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#if WITH_DEV_AUTOMATION_TESTS
#include "Fixtures/AssetDocumentAIBehaviorTreeLegacyEmptyKeyProbeTypes.h"
#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

namespace AssetDocumentAIBehaviorTreeLegacyEmptyKeyProbeTests
{
TSharedRef<FJsonObject> Node(const FString& Class, int32 Y)
{
 auto Result = MakeShared<FJsonObject>();
 Result->SetStringField(TEXT("Id"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
 Result->SetStringField(TEXT("Class"), Class);
 Result->SetObjectField(TEXT("Properties"), MakeShared<FJsonObject>());
 Result->SetArrayField(TEXT("Children"), {});
 Result->SetArrayField(TEXT("Decorators"), {});
 Result->SetArrayField(TEXT("Services"), {});
 auto Position = MakeShared<FJsonObject>();
 Position->SetNumberField(TEXT("X"), 0); Position->SetNumberField(TEXT("Y"), Y);
 auto Editor = MakeShared<FJsonObject>();
 Editor->SetObjectField(TEXT("Position"), Position);
 Result->SetObjectField(TEXT("Editor"), Editor);
 return Result;
}
FString Json(const TSharedRef<FJsonObject>& Value)
{
 FString Text;
 FJsonSerializer::Serialize(Value, TJsonWriterFactory<>::Create(&Text));
 return Text;
}
const FJsonObject* Object(const FJsonObject* Parent, const TCHAR* Name)
{
 const TSharedPtr<FJsonObject>* Value = nullptr;
 return Parent && Parent->TryGetObjectField(Name, Value) && Value && Value->IsValid() ? Value->Get() : nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAILegacyEmptyKeyPublicProbe,
 "UEAssetDoc.AIProvider.BehaviorTree.LegacyEmptyKey.PublicApplyFile",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAILegacyEmptyKeyPublicProbe::RunTest(const FString&)
{
 using namespace AssetDocumentAIBehaviorTreeLegacyEmptyKeyProbeTests;
 const FString Target = TEXT("/Game/AssetDocumentTests/BTProductionLegacyEmptyKey_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
 const FString ObjectPath = Target + TEXT(".") + FPackageName::GetLongPackageAssetName(Target);
 const FString PackageFile = FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(Target, FPackageName::GetAssetPackageExtension()));
 const FString Sidecar = FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(Target, TEXT(".assetdoc.json")));
 if (FindObject<UObject>(nullptr, *ObjectPath) || FindPackage(nullptr, *Target)
  || IFileManager::Get().FileExists(*PackageFile) || IFileManager::Get().FileExists(*Sidecar))
 { AddError(TEXT("GUID probe target unexpectedly preexists; no writes or cleanup performed")); return false; }
 if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Sidecar), true)) return false;
 // Only this invocation's GUID target is owned. Remove its sidecar before
 // deleting the asset, so asset-removal sync cannot archive the probe sidecar.
 ON_SCOPE_EXIT
 {
  IFileManager::Get().Delete(*Sidecar, false, true);
  if (auto* Asset = FindObject<UObject>(nullptr, *ObjectPath)) ObjectTools::DeleteObjectsUnchecked(TArray<UObject*>{Asset});
  if (auto* Package = FindPackage(nullptr, *Target))
  { Package->SetDirtyFlag(false); UPackageTools::UnloadPackages(TArray<UPackage*>{Package}); }
  IFileManager::Get().Delete(*PackageFile, false, true);
 };
 auto Task = Node(UAssetDocumentAILegacyEmptyKeyProbeTask::StaticClass()->GetPathName(), 300);
 auto Map = MakeShared<FJsonObject>(); Map->SetNumberField(TEXT(""), 7);
 auto TaskProperties = MakeShared<FJsonObject>(); TaskProperties->SetObjectField(TEXT("Map"), Map);
 Task->SetObjectField(TEXT("Properties"), TaskProperties);
 auto Root = Node(TEXT("/Script/AIModule.BTComposite_Sequence"), 0);
 Root->SetArrayField(TEXT("Children"), {MakeShared<FJsonValueObject>(Task)});
 auto Tree = MakeShared<FJsonObject>();
 Tree->SetStringField(TEXT("GraphGuid"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
 Tree->SetObjectField(TEXT("Root"), Root); Tree->SetArrayField(TEXT("Comments"), {});
 auto Body = MakeShared<FJsonObject>();
 Body->SetField(TEXT("BlackboardAsset"), MakeShared<FJsonValueNull>()); Body->SetObjectField(TEXT("Tree"), Tree);
 auto Document = MakeShared<FJsonObject>();
 Document->SetNumberField(TEXT("SchemaVersion"), 1);
 Document->SetStringField(TEXT("Class"), TEXT("/Script/AIModule.BehaviorTree"));
 Document->SetStringField(TEXT("Target"), Target);
 Document->SetStringField(TEXT("Action"), TEXT("CreateOrUpdate"));
 Document->SetObjectField(TEXT("Definitions"), MakeShared<FJsonObject>());
 Document->SetObjectField(TEXT("Properties"), MakeShared<FJsonObject>());
 Document->SetObjectField(TEXT("Body"), Body);
 if (!FFileHelper::SaveStringToFile(Json(Document), *Sidecar, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;

 // This is the real module Service, including its current public routing. No
 // profile re-registration, internal materializer call, or route replacement.
 auto& Service = FAssetDocumentModule::Get().GetService();
 FAssetDocumentValidateRequest ValidateRequest; ValidateRequest.FilePath = Sidecar;
 const auto Validated = Service.Validate(ValidateRequest);
 AddInfo(TEXT("BT_LEGACY_EMPTY_KEY Validate: ") + Json(Validated.ToJson().ToSharedRef()));
 TestTrue(TEXT("legacy public Validate accepts FString map empty key"), Validated.IsSuccess());
 FAssetDocumentApplyFileRequest ApplyRequest;
 ApplyRequest.FilePath = Sidecar; ApplyRequest.bSaveAsset = true;
 const auto Applied = Service.ApplyFile(ApplyRequest);
 AddInfo(TEXT("BT_LEGACY_EMPTY_KEY ApplyFile: ") + Json(Applied.ToJson().ToSharedRef()));
 if (!TestTrue(TEXT("legacy public ApplyFile accepts FString map empty key"), Applied.IsSuccess())) return false;
 TestTrue(TEXT("public ApplyFile durably saved the asset"), Applied.bSavedAsset && IFileManager::Get().FileExists(*PackageFile));
 TestTrue(TEXT("public ApplyFile wrote canonical sidecar sync"), Applied.bWroteSidecar);
 auto* Asset = LoadObject<UBehaviorTree>(nullptr, *ObjectPath);
 const auto* NativeTask = Asset && Asset->RootNode && Asset->RootNode->Children.Num() == 1
  ? Cast<UAssetDocumentAILegacyEmptyKeyProbeTask>(Asset->RootNode->Children[0].ChildTask) : nullptr;
 const int32* NativeValue = NativeTask ? NativeTask->Map.Find(FString()) : nullptr;
 TestTrue(TEXT("native runtime task Map.Find(empty) returns 7"), NativeValue && *NativeValue == 7);
 FAssetDocumentExtractRequest ExtractRequest;
 ExtractRequest.AssetPath = ObjectPath; ExtractRequest.bDiffOnly = false; ExtractRequest.bIncludeAllWritable = true;
 const auto Extracted = Service.Extract(ExtractRequest);
 AddInfo(TEXT("BT_LEGACY_EMPTY_KEY Extract: ") + Json(Extracted.ToJson().ToSharedRef()));
 if (!TestTrue(TEXT("legacy public Extract succeeds"), Extracted.IsSuccess())) return false;
 const auto* ExtractedRoot = Object(Object(Object(Extracted.Payload.Get(), TEXT("Body")), TEXT("Tree")), TEXT("Root"));
 const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
 const FJsonObject* ExtractedTask = ExtractedRoot && ExtractedRoot->TryGetArrayField(TEXT("Children"), Children)
  && Children && Children->Num() == 1 && (*Children)[0].IsValid() ? (*Children)[0]->AsObject().Get() : nullptr;
 const auto* ExtractedMap = Object(Object(ExtractedTask, TEXT("Properties")), TEXT("Map"));
 double ExtractedValue = 0;
 TestTrue(TEXT("public extracted canonical retains exact empty key and value 7"), ExtractedMap
  && ExtractedMap->TryGetNumberField(TEXT(""), ExtractedValue) && ExtractedValue == 7);
 return !HasAnyErrors();
}
#endif
