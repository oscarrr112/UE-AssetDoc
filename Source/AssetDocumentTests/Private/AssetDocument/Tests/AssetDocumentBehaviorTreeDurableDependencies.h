// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_NativeEnum.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/SavePackage.h"

#if WITH_DEV_AUTOMATION_TESTS
// Private test seams: remove owned files before stock loaded-asset deletion.
ASSETDOCUMENT_API void FlushBehaviorTreeFixtureFileEvents();
ASSETDOCUMENT_API void DeleteBehaviorTreeFixtureLoadedAsset(UObject* Asset);

// Public ApplyFile verifies its saved package. Persist memory-only legacy test
// dependencies for that window. Existing files retain their surrounding owner.
class FScopedBehaviorTreeDurableDependencies
{
public:
 static void FlushFileEvents() { FlushBehaviorTreeFixtureFileEvents(); }
 ~FScopedBehaviorTreeDurableDependencies()
 {
  if (CreatedFiles.IsEmpty()) return;
  FlushFileEvents();
  for (const FString& Filename : CreatedFiles) IFileManager::Get().Delete(*Filename, false, true);
  FlushFileEvents();
 }
 bool Prepare(const TSharedPtr<FJsonObject>& Document)
 {
  TSet<const UObject*> Seen;
  TFunction<bool(UObject*)> SaveAsset;
  SaveAsset = [&](UObject* Asset)
  {
   if (!Asset || Seen.Contains(Asset)) return Asset != nullptr;
   Seen.Add(Asset);
   if (const auto* Tree = Cast<UBehaviorTree>(Asset))
    if (Tree->GetBlackboardAsset() && !SaveAsset(Tree->GetBlackboardAsset())) return false;
   if (const auto* Blackboard = Cast<UBlackboardData>(Asset))
   {
    if (Blackboard->Parent && !SaveAsset(Blackboard->Parent)) return false;
    for (const auto& Entry : Blackboard->Keys)
    {
     if (const auto* Type = Cast<UBlackboardKeyType_Enum>(Entry.KeyType))
      if (Type->EnumType && !SaveAsset(Type->EnumType)) return false;
     if (const auto* Type = Cast<UBlackboardKeyType_NativeEnum>(Entry.KeyType))
      if (Type->EnumType && !SaveAsset(Type->EnumType)) return false;
    }
   }
   const FString PackageName = Asset->GetOutermost()->GetName();
   if (!PackageName.StartsWith(TEXT("/Game/AssetDocumentTests/"))) return true;
   if (Asset->GetOuter() != Asset->GetOutermost()) return false;
   const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
   const bool bExisted = IFileManager::Get().FileExists(*Filename);
   if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true)) return false;
   FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
   if (!UPackage::SavePackage(Asset->GetOutermost(), Asset, *Filename, Args)) return false;
   if (!bExisted) CreatedFiles.AddUnique(Filename);
   return true;
  };
  TFunction<bool(const TSharedPtr<FJsonValue>&)> Scan;
  Scan = [&](const auto& Value)
  {
   if (!Value.IsValid()) return false;
   if (Value->Type == EJson::Array)
   { for (const auto& Item : Value->AsArray()) if (!Scan(Item)) return false; }
   if (Value->Type != EJson::Object) return true;
   const auto Object = Value->AsObject();
   FString Kind, Path;
   if (Object->TryGetStringField(TEXT("Kind"), Kind) && Kind == TEXT("AssetRef") && Object->TryGetStringField(TEXT("Path"), Path))
   {
    UObject* Asset = FindObject<UObject>(nullptr, *Path);
    if (Asset && (Asset->IsA<UBehaviorTree>() || Asset->IsA<UBlackboardData>()) && !SaveAsset(Asset)) return false;
   }
   for (const auto& Pair : Object->Values) if (!Scan(Pair.Value)) return false;
   return true;
  };
  return Document.IsValid() && Scan(MakeShared<FJsonValueObject>(Document));
 }
private:
 TArray<FString> CreatedFiles;
};
#endif
