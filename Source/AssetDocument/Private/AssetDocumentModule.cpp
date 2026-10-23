// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentModule.h"
#include "AssetDocumentEditorSync.h"
#include "AssetDocumentFileWatcher.h"
#include "AssetDocumentHttpRoutes.h"
#include "AssetDocumentIngressQueue.h"
#include "AssetDocumentService.h"

DEFINE_LOG_CATEGORY(LogAssetDocument);

FAssetDocumentModule::FAssetDocumentModule() = default;
FAssetDocumentModule::~FAssetDocumentModule() = default;

void FAssetDocumentModule::StartupModule()
{
	UE_LOG(LogAssetDocument, Log, TEXT("AssetDocument module starting up"));
	// Every supported class is served by a Python Profile through the generic runtime route;
	// there are no C++ exact profiles to register.
	Service = MakeShared<FAssetDocumentService>();
	IngressQueue = MakeShared<FAssetDocumentIngressQueue>(
		Service.ToSharedRef());
	HttpRoutes = MakeUnique<FAssetDocumentHttpRoutes>(Service.ToSharedRef());
	HttpRoutes->Register();
	EditorSync = MakeUnique<FAssetDocumentEditorSync>(
		IngressQueue.ToSharedRef());
	EditorSync->Register();
	FileWatcher = MakeUnique<FAssetDocumentFileWatcher>(
		IngressQueue.ToSharedRef());
	FileWatcher->Register();
}

void FAssetDocumentModule::ShutdownModule()
{
	FileWatcher.Reset();
	EditorSync.Reset();
	HttpRoutes.Reset();
	IngressQueue.Reset();
	Service.Reset();
	UE_LOG(LogAssetDocument, Log, TEXT("AssetDocument module shut down"));
}

FAssetDocumentModule& FAssetDocumentModule::Get()
{
	return FModuleManager::LoadModuleChecked<FAssetDocumentModule>(TEXT("AssetDocument"));
}

bool FAssetDocumentModule::IsAvailable()
{
	return FModuleManager::Get().IsModuleLoaded(TEXT("AssetDocument"));
}

FAssetDocumentService& FAssetDocumentModule::GetService()
{
	check(Service.IsValid());
	return *Service;
}

IMPLEMENT_MODULE(FAssetDocumentModule, AssetDocument)
