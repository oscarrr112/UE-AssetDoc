// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentViewportCapture.h"

#include "Editor.h"
#include "LevelEditorViewport.h"
#include "RenderingThread.h"
#include "UnrealClient.h"

FLevelEditorViewportClient* FAssetDocumentViewportCapture::FindViewportClient()
{
	if (GEditor == nullptr)
	{
		return nullptr;
	}
	if (GCurrentLevelEditingViewportClient != nullptr && GCurrentLevelEditingViewportClient->Viewport != nullptr
		&& GEditor->GetLevelViewportClients().Contains(GCurrentLevelEditingViewportClient))
	{
		return GCurrentLevelEditingViewportClient;
	}
	FLevelEditorViewportClient* Found = nullptr;
	for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
	{
		if (Client != nullptr && Client->Viewport != nullptr)
		{
			if (Client->IsPerspective())
			{
				return Client;
			}
			Found = Found != nullptr ? Found : Client;
		}
	}
	return Found;
}

bool FAssetDocumentViewportCapture::Capture(FLevelEditorViewportClient* Client, TArray<FColor>& OutPixels, FIntPoint& OutSize,
	int32& OutHttpStatus, FString& OutError)
{
	if (Client == nullptr || Client->Viewport == nullptr)
	{
		OutHttpStatus = 503;
		OutError = TEXT("No Level Viewport available");
		return false;
	}
	FViewport* Viewport = Client->Viewport;
	OutSize = Viewport->GetSizeXY();
	if (OutSize.X <= 0 || OutSize.Y <= 0)
	{
		OutHttpStatus = 503;
		OutError = TEXT("Viewport has zero size");
		return false;
	}
	// Draw the current state of the world now instead of reading the last presented frame.
	Client->Invalidate();
	Viewport->Draw(/*bShouldPresent*/ true);
	FlushRenderingCommands();
	if (!Viewport->ReadPixels(OutPixels) || OutPixels.Num() != OutSize.X * OutSize.Y)
	{
		OutHttpStatus = 500;
		OutError = OutPixels.Num() == 0 ? TEXT("Failed to read viewport pixels") : TEXT("Pixel buffer size mismatch");
		return false;
	}
	return true;
}
