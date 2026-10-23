// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Play/PlaySession.h"

#include "AssetDocumentViewportCapture.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/Base64.h"
#include "Modules/ModuleManager.h"
#include "Play/PlayQuery.h"
#include "RenderingThread.h"
#include "UnrealClient.h"
#include "Widgets/SViewport.h"

namespace UE::AssetDocumentLevel::Private::Play
{
namespace PlayCaptureLocal
{
// get_viewport_screenshot sizes: a 1280 px JPEG at quality 85 was 106 KB of base64. The defaults
// keep a readable image at about a third of that; "format": "png" is lossless.
constexpr int32 DefaultMaxWidth = 1024;
constexpr int32 MinMaxWidth = 64;
constexpr int32 MaxMaxWidth = 3840;
constexpr int32 DefaultJpegQuality = 70;

// Scales Pixels down (never up) to at most MaxWidth pixels wide, averaging the source pixels each target pixel
// covers (a box filter: no aliasing, so the JPEG stays small and text stays legible).
void Downsample(TArray<FColor>& Pixels, FIntPoint& Size, int32 MaxWidth)
{
	if (Size.X <= MaxWidth)
	{
		return;
	}
	const double Scale = static_cast<double>(MaxWidth) / Size.X;
	const int32 Width = MaxWidth;
	const int32 Height = FMath::Max(1, FMath::RoundToInt32(Size.Y * Scale));
	TArray<FColor> Scaled;
	Scaled.SetNumUninitialized(Width * Height);
	for (int32 Y = 0; Y < Height; ++Y)
	{
		const int32 Top = FMath::Clamp(FMath::FloorToInt32(Y / Scale), 0, Size.Y - 1);
		const int32 Bottom = FMath::Clamp(FMath::CeilToInt32((Y + 1) / Scale), Top + 1, Size.Y);
		for (int32 X = 0; X < Width; ++X)
		{
			const int32 Left = FMath::Clamp(FMath::FloorToInt32(X / Scale), 0, Size.X - 1);
			const int32 Right = FMath::Clamp(FMath::CeilToInt32((X + 1) / Scale), Left + 1, Size.X);
			uint32 Sum[4] = {0, 0, 0, 0};
			for (int32 SourceY = Top; SourceY < Bottom; ++SourceY)
			{
				for (int32 SourceX = Left; SourceX < Right; ++SourceX)
				{
					const FColor& Source = Pixels[SourceY * Size.X + SourceX];
					Sum[0] += Source.R;
					Sum[1] += Source.G;
					Sum[2] += Source.B;
					Sum[3] += Source.A;
				}
			}
			const uint32 Count = static_cast<uint32>((Bottom - Top) * (Right - Left));
			Scaled[Y * Width + X] = FColor(static_cast<uint8>(Sum[0] / Count), static_cast<uint8>(Sum[1] / Count),
				static_cast<uint8>(Sum[2] / Count), static_cast<uint8>(Sum[3] / Count));
		}
	}
	Pixels = MoveTemp(Scaled);
	Size = FIntPoint(Width, Height);
}

// The game viewport's own rendering: the world as the player camera sees it plus Canvas HUD
// drawing, without Slate (UMG) widgets.
bool CaptureGameViewport(UGameViewportClient& Client, TArray<FColor>& OutPixels, FIntPoint& OutSize, FString& OutError)
{
	FViewport* Viewport = Client.Viewport;
	if (Viewport == nullptr)
	{
		OutError = TEXT("The play session has no game viewport.");
		return false;
	}
	OutSize = Viewport->GetSizeXY();
	if (OutSize.X <= 0 || OutSize.Y <= 0)
	{
		OutError = TEXT("The game viewport has zero size.");
		return false;
	}
	Viewport->Draw(/*bShouldPresent*/ true);
	FlushRenderingCommands();
	if (!Viewport->ReadPixels(OutPixels) || OutPixels.Num() != OutSize.X * OutSize.Y)
	{
		OutError = TEXT("Reading the game viewport's pixels failed.");
		return false;
	}
	return true;
}

// The game viewport widget as the player sees it, UMG widgets included.
bool CaptureGameWidget(UGameViewportClient& Client, TArray<FColor>& OutPixels, FIntPoint& OutSize)
{
	const TSharedPtr<SViewport> Widget = Client.GetGameViewportWidget();
	if (!Widget.IsValid() || !FSlateApplication::IsInitialized())
	{
		return false;
	}
	FIntVector Size;
	if (!FSlateApplication::Get().TakeScreenshot(Widget.ToSharedRef(), OutPixels, Size) || Size.X <= 0 || Size.Y <= 0
		|| OutPixels.Num() != Size.X * Size.Y)
	{
		return false;
	}
	OutSize = FIntPoint(Size.X, Size.Y);
	// A widget that was not drawn (hidden window, off-screen rendering) reads back fully transparent.
	for (const FColor& Pixel : OutPixels)
	{
		if (Pixel.A != 0 || Pixel.R != 0 || Pixel.G != 0 || Pixel.B != 0)
		{
			return true;
		}
	}
	return false;
}

FLevelToolResult CaptureFailure(const FString& Code, const FString& Message)
{
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	Diagnostics.Add(MakePlayDiagnostic(TEXT(""), Code, Message));
	return FLevelToolResult::Failure(MoveTemp(Diagnostics));
}

// An integer request field in [Min, Max]; a diagnostic otherwise.
bool ReadScreenshotInteger(const FJsonObject& Request, const TCHAR* Field, int32 Min, int32 Max, int32& InOutValue,
	TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	if (!Request.HasField(Field))
	{
		return true;
	}
	double Number = 0.0;
	if (!Request.TryGetNumberField(Field, Number) || Number != FMath::FloorToDouble(Number) || Number < Min || Number > Max)
	{
		Diagnostics.Add(MakePlayDiagnostic(FString(TEXT("/")) + Field, TEXT("Play.InvalidRequest"),
			FString::Printf(TEXT("'%s' is a whole number from %d to %d."), Field, Min, Max)));
		return false;
	}
	InOutValue = static_cast<int32>(Number);
	return true;
}
} // namespace PlayCaptureLocal

FLevelToolResult RunPlayScreenshotFromBody(const FString& RequestBody)
{
	check(IsInGameThread());
	using namespace PlayCaptureLocal;
	TSharedPtr<FJsonObject> Request;
	FLevelToolResult Failure;
	if (!ParsePlayBody(RequestBody, Request, Failure))
	{
		return Failure;
	}
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	for (const auto& Field : Request->Values)
	{
		if (Field.Key != TEXT("view") && Field.Key != TEXT("hud") && Field.Key != TEXT("format") && Field.Key != TEXT("quality")
			&& Field.Key != TEXT("max_width"))
		{
			Diagnostics.Add(MakePlayDiagnostic(TEXT("/") + FString(*Field.Key), TEXT("Play.InvalidRequest"),
				FString::Printf(TEXT("Unknown screenshot field '%s'. Fields: view, hud, format, quality, max_width."), *Field.Key)));
		}
	}
	FString View = TEXT("auto");
	Request->TryGetStringField(TEXT("view"), View);
	if (View != TEXT("auto") && View != TEXT("game") && View != TEXT("editor"))
	{
		Diagnostics.Add(MakePlayDiagnostic(TEXT("/view"), TEXT("Play.InvalidRequest"),
			TEXT("'view' is \"auto\" (the game while a play session runs, else the editor), \"game\" or \"editor\".")));
	}
	bool bHud = true;
	Request->TryGetBoolField(TEXT("hud"), bHud);
	FString Format = TEXT("jpeg");
	Request->TryGetStringField(TEXT("format"), Format);
	Format = Format.ToLower();
	if (Format == TEXT("jpg"))
	{
		Format = TEXT("jpeg");
	}
	if (Format != TEXT("jpeg") && Format != TEXT("png"))
	{
		Diagnostics.Add(MakePlayDiagnostic(TEXT("/format"), TEXT("Play.InvalidRequest"),
			TEXT("'format' is \"jpeg\" (default; 'quality' sets its compression) or \"png\" (lossless, several times larger).")));
	}
	int32 Quality = DefaultJpegQuality;
	ReadScreenshotInteger(*Request, TEXT("quality"), 1, 100, Quality, Diagnostics);
	if (Format == TEXT("png") && Request->HasField(TEXT("quality")))
	{
		Diagnostics.Add(MakePlayDiagnostic(TEXT("/quality"), TEXT("Play.InvalidRequest"),
			TEXT("'quality' is the JPEG compression; a PNG is lossless.")));
	}
	int32 MaxWidth = DefaultMaxWidth;
	ReadScreenshotInteger(*Request, TEXT("max_width"), MinMaxWidth, MaxMaxWidth, MaxWidth, Diagnostics);
	if (!Diagnostics.IsEmpty())
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}

	UWorld* World = GetPlayWorld();
	if (View == TEXT("auto"))
	{
		View = World != nullptr && World->GetGameViewport() != nullptr ? TEXT("game") : TEXT("editor");
	}
	TArray<FColor> Pixels;
	FIntPoint Size(0, 0);
	TArray<FAssetDocumentDiagnostic> Warnings;
	bool bHudCaptured = false;
	if (View == TEXT("game"))
	{
		if (World == nullptr)
		{
			return CaptureFailure(TEXT("Play.NoSession"), TEXT("No play session is running: there is no game view (\"view\": \"editor\" shows the Level Editor)."));
		}
		UGameViewportClient* Client = World->GetGameViewport();
		if (Client == nullptr)
		{
			return CaptureFailure(TEXT("Play.NoGameViewport"), TEXT("The play session has no game viewport (Simulate shows the world in the Level Editor viewport: use \"view\": \"editor\")."));
		}
		if (bHud)
		{
			bHudCaptured = CaptureGameWidget(*Client, Pixels, Size);
			if (!bHudCaptured)
			{
				FAssetDocumentDiagnostic Warning = MakePlayDiagnostic(TEXT("/hud"), TEXT("Play.HudNotCaptured"),
					TEXT("The game window could not be captured with its UMG widgets (it is not drawn on screen); this is the game viewport without them."));
				Warning.Severity = TEXT("warning");
				Warnings.Add(Warning);
			}
		}
		FString Error;
		if (!bHudCaptured && !CaptureGameViewport(*Client, Pixels, Size, Error))
		{
			return CaptureFailure(TEXT("Play.CaptureFailed"), Error);
		}
	}
	else
	{
		int32 Status = 0;
		FString Error;
		if (!FAssetDocumentViewportCapture::Capture(FAssetDocumentViewportCapture::FindViewportClient(), Pixels, Size, Status, Error))
		{
			return CaptureFailure(TEXT("Play.CaptureFailed"), Error);
		}
	}

	Downsample(Pixels, Size, MaxWidth);
	const bool bPng = Format == TEXT("png");
	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	const TSharedPtr<IImageWrapper> Wrapper = ImageWrapperModule.CreateImageWrapper(bPng ? EImageFormat::PNG : EImageFormat::JPEG);
	if (!Wrapper.IsValid())
	{
		return CaptureFailure(TEXT("Play.CaptureFailed"), bPng ? TEXT("No PNG encoder.") : TEXT("No JPEG encoder."));
	}
	for (FColor& Pixel : Pixels)
	{
		Pixel.A = 255;
	}
	Wrapper->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), Size.X, Size.Y, ERGBFormat::BGRA, 8);
	// PNG: the encoder's default (lossless) compression.
	const TArray64<uint8> Compressed = Wrapper->GetCompressed(bPng ? 0 : Quality);
	if (Compressed.IsEmpty())
	{
		return CaptureFailure(TEXT("Play.CaptureFailed"), bPng ? TEXT("PNG compression produced no data.") : TEXT("JPEG compression produced no data."));
	}
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("view"), View);
	if (View == TEXT("game"))
	{
		Payload->SetBoolField(TEXT("hud"), bHudCaptured);
		Payload->SetNumberField(TEXT("world_time"), FMath::RoundToDouble(World->GetTimeSeconds() * 1000.0) / 1000.0);
	}
	Payload->SetNumberField(TEXT("width"), Size.X);
	Payload->SetNumberField(TEXT("height"), Size.Y);
	Payload->SetStringField(TEXT("mime_type"), bPng ? TEXT("image/png") : TEXT("image/jpeg"));
	if (!bPng)
	{
		Payload->SetNumberField(TEXT("quality"), Quality);
	}
	const FString Data = FBase64::Encode(Compressed.GetData(), static_cast<uint32>(Compressed.Num()));
	// What the image costs the caller: its base64 length.
	Payload->SetNumberField(TEXT("bytes"), Data.Len());
	Payload->SetStringField(TEXT("data"), Data);
	FLevelToolResult Result = FLevelToolResult::Success(FString::Printf(TEXT("%s view, %dx%d %s, %.1f KB."), *View, Size.X, Size.Y,
		bPng ? TEXT("PNG") : *FString::Printf(TEXT("JPEG q%d"), Quality), Data.Len() / 1024.0), Payload);
	Result.Diagnostics = MoveTemp(Warnings);
	return Result;
}
}
