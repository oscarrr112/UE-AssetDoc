// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

// Source files for the import tests, generated in code (no binary fixtures): OBJ text, a
// skinned and animated glTF with an embedded buffer, images through FImageUtils, 16-bit PCM
// WAV. FBX sources are exported from imported assets by the editor's FBX exporters.

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Exporters/Exporter.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Import/AssetImportTool.h"
#include "LevelToolResult.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentImportTests
{
using UE::AssetDocumentLevel::Private::FLevelToolResult;

inline TSharedPtr<FJsonObject> ParseJson(const FString& Text)
{
	TSharedPtr<FJsonObject> Json;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	FJsonSerializer::Deserialize(Reader, Json);
	return Json;
}

inline FString Quote(const FString& Text)
{
	return TEXT("\"") + Text.ReplaceCharWithEscapedChar() + TEXT("\"");
}

inline bool HasDiagnostic(const FLevelToolResult& Result, const TCHAR* Code, const TCHAR* Path = nullptr)
{
	return Result.Diagnostics.ContainsByPredicate([Code, Path](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == Code && (Path == nullptr || Diagnostic.Path == Path);
	});
}

// One test's sources (Saved/AssetDocumentServerTests/Import/T<id>) and assets
// (/Game/__AssetDocumentTests__/Import/T<id>); the destructor unloads and deletes both.
class FScopedImportTest
{
public:
	FScopedImportTest()
	{
		const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
		Root = FString::Printf(TEXT("/Game/__AssetDocumentTests__/Import/T%s"), *Id);
		SourceDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("AssetDocumentServerTests/Import") / (TEXT("T") + Id));
		IFileManager::Get().MakeDirectory(*SourceDir, true);
	}

	~FScopedImportTest()
	{
		if (GEditor != nullptr && GEditor->Trans != nullptr)
		{
			GEditor->Trans->Reset(FText::FromString(TEXT("Import test cleanup")));
		}
		TArray<FAssetData> Assets;
		FAssetRegistryModule::GetRegistry().GetAssetsByPath(FName(*Root), Assets, /*bRecursive*/ true, /*bIncludeOnlyOnDiskAssets*/ false);
		TArray<UPackage*> Packages;
		for (const FAssetData& Asset : Assets)
		{
			UPackage* Package = FindPackage(nullptr, *Asset.PackageName.ToString());
			if (Package == nullptr)
			{
				continue;
			}
			if (UObject* Object = Asset.FastGetAsset(false); Object != nullptr && GEditor != nullptr)
			{
				if (UAssetEditorSubsystem* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
				{
					Editors->CloseAllEditorsForAsset(Object);
				}
			}
			Package->ClearDirtyFlag();
			Packages.AddUnique(Package);
		}
		if (!Packages.IsEmpty())
		{
			UPackageTools::UnloadPackages(Packages);
		}
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		const FString ContentDir = FPaths::ProjectContentDir() / Root.RightChop(FString(TEXT("/Game/")).Len());
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *ContentDir, TEXT("*.*"), true, false, false);
		IFileManager::Get().DeleteDirectory(*ContentDir, false, true);
		if (!Files.IsEmpty())
		{
			FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles(Files);
		}
		IFileManager::Get().DeleteDirectory(*SourceDir, false, true);
	}

	FScopedImportTest(const FScopedImportTest&) = delete;
	FScopedImportTest& operator=(const FScopedImportTest&) = delete;

	FString Folder(const TCHAR* Name) const { return Root / Name; }
	FString Source(const TCHAR* Name) const { return SourceDir / Name; }

	FString Root;
	FString SourceDir;
};

// ---------------------------------------------------------------------------------------------
// Source files

// An axis-aligned box from Min to Max as OBJ text (object and group Name: Interchange makes a
// mesh per group); IndexBase is the number of
// vertices written before it in the same file. Each face carries texture coordinates (four per box).
inline FString ObjBox(const TCHAR* Name, const FVector& Min, const FVector& Max, int32 IndexBase)
{
	FString Text = FString::Printf(TEXT("o %s\ng %s\n"), Name, Name);
	for (int32 Corner = 0; Corner < 8; ++Corner)
	{
		Text += FString::Printf(TEXT("v %g %g %g\n"), (Corner & 1) ? Max.X : Min.X, (Corner & 2) ? Max.Y : Min.Y, (Corner & 4) ? Max.Z : Min.Z);
	}
	Text += TEXT("vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n");
	const int32 UVBase = IndexBase / 8 * 4;
	static const int32 Faces[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
	for (const auto& Face : Faces)
	{
		Text += FString::Printf(TEXT("f %d/%d %d/%d %d/%d %d/%d\n"), IndexBase + Face[0] + 1, UVBase + 1, IndexBase + Face[1] + 1, UVBase + 2,
			IndexBase + Face[2] + 1, UVBase + 3, IndexBase + Face[3] + 1, UVBase + 4);
	}
	return Text;
}

inline bool WriteObjCube(const FString& File, double Size)
{
	return FFileHelper::SaveStringToFile(ObjBox(TEXT("Crate"), FVector(-Size / 2.0), FVector(Size / 2.0), 0), *File);
}

// Two separate boxes (groups "Left" and "Right").
inline bool WriteObjTwoBoxes(const FString& File)
{
	const FString Text = ObjBox(TEXT("Left"), FVector(-30.0, -10.0, -10.0), FVector(-10.0, 10.0, 10.0), 0)
		+ ObjBox(TEXT("Right"), FVector(10.0, -10.0, -10.0), FVector(30.0, 10.0, 10.0), 8);
	return FFileHelper::SaveStringToFile(Text, *File);
}

class FByteWriter
{
public:
	TArray<uint8> Bytes;

	int32 Offset() const { return Bytes.Num(); }
	void Float(float Value) { Bytes.Append(reinterpret_cast<const uint8*>(&Value), sizeof(Value)); }
	void U16(uint16 Value) { Bytes.Append(reinterpret_cast<const uint8*>(&Value), sizeof(Value)); }
	void U8(uint8 Value) { Bytes.Add(Value); }
	void Pad4()
	{
		while (Bytes.Num() % 4 != 0)
		{
			Bytes.Add(0);
		}
	}
};

// A skinned quad ("Flag": joints Root and Tip, the top half weighted to Tip) with a one-second
// animation "Wave" rotating Tip by 90 degrees, as one .gltf with an embedded buffer.
inline bool WriteGltfSkinnedFlag(const FString& File)
{
	FByteWriter Buffer;
	const int32 Positions = Buffer.Offset();
	const float Corners[4][3] = {{-0.5f, 0.0f, 0.0f}, {0.5f, 0.0f, 0.0f}, {0.5f, 1.0f, 0.0f}, {-0.5f, 1.0f, 0.0f}};
	for (const auto& Corner : Corners)
	{
		Buffer.Float(Corner[0]);
		Buffer.Float(Corner[1]);
		Buffer.Float(Corner[2]);
	}
	const int32 Normals = Buffer.Offset();
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Buffer.Float(0.0f);
		Buffer.Float(0.0f);
		Buffer.Float(1.0f);
	}
	const int32 Joints = Buffer.Offset();
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Buffer.U8(static_cast<uint8>(Index < 2 ? 0 : 1));
		Buffer.U8(0);
		Buffer.U8(0);
		Buffer.U8(0);
	}
	const int32 Weights = Buffer.Offset();
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Buffer.Float(1.0f);
		Buffer.Float(0.0f);
		Buffer.Float(0.0f);
		Buffer.Float(0.0f);
	}
	const int32 Indices = Buffer.Offset();
	for (const int32 Index : {0, 1, 2, 0, 2, 3})
	{
		Buffer.U16(static_cast<uint16>(Index));
	}
	Buffer.Pad4();
	const int32 InverseBind = Buffer.Offset();
	for (int32 Joint = 0; Joint < 2; ++Joint)
	{
		for (int32 Element = 0; Element < 16; ++Element)
		{
			// Column-major; Tip is at (0, 0.5, 0), its inverse bind translates by -0.5 on Y.
			const bool bDiagonal = Element % 5 == 0;
			Buffer.Float(bDiagonal ? 1.0f : (Joint == 1 && Element == 13) ? -0.5f : 0.0f);
		}
	}
	const int32 Times = Buffer.Offset();
	Buffer.Float(0.0f);
	Buffer.Float(1.0f);
	const int32 Rotations = Buffer.Offset();
	for (const float Value : {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.70710678f, 0.70710678f})
	{
		Buffer.Float(Value);
	}
	const int32 Length = Buffer.Offset();

	auto View = [](int32 Offset, int32 Bytes)
	{
		return FString::Printf(TEXT("{\"buffer\": 0, \"byteOffset\": %d, \"byteLength\": %d}"), Offset, Bytes);
	};
	const FString Json = FString::Printf(TEXT(R"({
  "asset": {"version": "2.0", "generator": "UEAssetDoc import test"},
  "scene": 0,
  "scenes": [{"nodes": [0, 1]}],
  "nodes": [
    {"name": "Flag", "mesh": 0, "skin": 0},
    {"name": "Root", "children": [2]},
    {"name": "Tip", "translation": [0, 0.5, 0]}
  ],
  "meshes": [{"name": "Flag", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "JOINTS_0": 2, "WEIGHTS_0": 3}, "indices": 4}]}],
  "skins": [{"name": "FlagSkin", "inverseBindMatrices": 5, "joints": [1, 2], "skeleton": 1}],
  "animations": [{"name": "Wave", "samplers": [{"input": 6, "output": 7, "interpolation": "LINEAR"}],
    "channels": [{"sampler": 0, "target": {"node": 2, "path": "rotation"}}]}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": [-0.5, 0, 0], "max": [0.5, 1, 0]},
    {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
    {"bufferView": 2, "componentType": 5121, "count": 4, "type": "VEC4"},
    {"bufferView": 3, "componentType": 5126, "count": 4, "type": "VEC4"},
    {"bufferView": 4, "componentType": 5123, "count": 6, "type": "SCALAR"},
    {"bufferView": 5, "componentType": 5126, "count": 2, "type": "MAT4"},
    {"bufferView": 6, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [0], "max": [1]},
    {"bufferView": 7, "componentType": 5126, "count": 2, "type": "VEC4"}
  ],
  "bufferViews": [%s, %s, %s, %s, %s, %s, %s, %s],
  "buffers": [{"byteLength": %d, "uri": "data:application/octet-stream;base64,%s"}]
})"), *View(Positions, Normals - Positions), *View(Normals, Joints - Normals), *View(Joints, Weights - Joints), *View(Weights, Indices - Weights),
		*View(Indices, 12), *View(InverseBind, Times - InverseBind), *View(Times, Rotations - Times), *View(Rotations, Length - Rotations), Length,
		*FBase64::Encode(Buffer.Bytes));
	return FFileHelper::SaveStringToFile(Json, *File);
}

// A SizeX x SizeY image (red/green gradient; float for .exr) saved by extension.
inline bool WriteImage(const FString& File, int32 SizeX, int32 SizeY)
{
	const bool bFloat = FPaths::GetExtension(File).Equals(TEXT("exr"), ESearchCase::IgnoreCase);
	FImage Image(SizeX, SizeY, 1, bFloat ? ERawImageFormat::RGBA16F : ERawImageFormat::BGRA8, bFloat ? EGammaSpace::Linear : EGammaSpace::sRGB);
	for (int32 Y = 0; Y < SizeY; ++Y)
	{
		for (int32 X = 0; X < SizeX; ++X)
		{
			const float R = static_cast<float>(X) / static_cast<float>(FMath::Max(SizeX - 1, 1));
			const float G = static_cast<float>(Y) / static_cast<float>(FMath::Max(SizeY - 1, 1));
			if (bFloat)
			{
				Image.AsRGBA16F()[Y * SizeX + X] = FFloat16Color(FLinearColor(R * 4.0f, G, 0.25f, 1.0f));
			}
			else
			{
				Image.AsBGRA8()[Y * SizeX + X] = FColor(static_cast<uint8>(R * 255.0f), static_cast<uint8>(G * 255.0f), 64, 255);
			}
		}
	}
	return FImageUtils::SaveImageByExtension(*File, Image);
}

// 16-bit PCM mono WAV: a 440 Hz tone of Seconds at SampleRate.
inline bool WriteWav(const FString& File, int32 SampleRate, double Seconds)
{
	const int32 Samples = static_cast<int32>(SampleRate * Seconds);
	FByteWriter Data;
	for (int32 Index = 0; Index < Samples; ++Index)
	{
		const double Value = FMath::Sin(2.0 * UE_DOUBLE_PI * 440.0 * Index / SampleRate) * 0.5;
		Data.U16(static_cast<uint16>(static_cast<int16>(Value * 32767.0)));
	}
	FByteWriter Wav;
	auto Tag = [&Wav](const char* Text)
	{
		for (int32 Index = 0; Index < 4; ++Index)
		{
			Wav.U8(static_cast<uint8>(Text[Index]));
		}
	};
	auto U32 = [&Wav](uint32 Value)
	{
		Wav.Bytes.Append(reinterpret_cast<const uint8*>(&Value), sizeof(Value));
	};
	const uint32 DataBytes = static_cast<uint32>(Data.Bytes.Num());
	Tag("RIFF");
	U32(36 + DataBytes);
	Tag("WAVE");
	Tag("fmt ");
	U32(16);
	Wav.U16(1); // PCM
	Wav.U16(1); // mono
	U32(static_cast<uint32>(SampleRate));
	U32(static_cast<uint32>(SampleRate * 2));
	Wav.U16(2);
	Wav.U16(16);
	Tag("data");
	U32(DataBytes);
	Wav.Bytes.Append(Data.Bytes);
	return FFileHelper::SaveArrayToFile(Wav.Bytes, *File);
}

// Exports Asset with the editor's exporter for the file's extension (FBX: no options dialog
// while automation tests run).
inline bool ExportAsset(UObject* Asset, const FString& File)
{
	if (Asset == nullptr)
	{
		return false;
	}
	UExporter* Exporter = UExporter::FindExporter(Asset, *FPaths::GetExtension(File));
	return Exporter != nullptr && UExporter::ExportToFile(Asset, Exporter, *File, /*InSelectedOnly*/ false, /*NoReplaceIdentical*/ false,
		/*Prompt*/ false) != 0 && IFileManager::Get().FileExists(*File);
}

// ---------------------------------------------------------------------------------------------
// Results

inline FLevelToolResult RunImport(FAutomationTestBase& Test, const FString& Json, const TCHAR* Stage)
{
	const FLevelToolResult Result = UE::AssetDocumentLevel::Private::RunAssetImport(ParseJson(Json));
	Test.AddInfo(FString::Printf(TEXT("%s: %d %s"), Stage, Result.HttpStatus, *Result.Message));
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Test.AddInfo(FString::Printf(TEXT("  %s %s at %s: %s"), *Diagnostic.Severity, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
	}
	return Result;
}

inline TArray<TSharedPtr<FJsonObject>> ResultAssets(const FLevelToolResult& Result, const TCHAR* ClassName = nullptr)
{
	TArray<TSharedPtr<FJsonObject>> Assets;
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	if (Result.Payload.IsValid() && Result.Payload->TryGetArrayField(TEXT("assets"), Rows))
	{
		for (const TSharedPtr<FJsonValue>& Row : *Rows)
		{
			const TSharedPtr<FJsonObject> Asset = Row->AsObject();
			if (ClassName == nullptr || Asset->GetStringField(TEXT("class")).EndsWith(FString(TEXT(".")) + ClassName))
			{
				Assets.Add(Asset);
			}
		}
	}
	return Assets;
}

inline TSharedPtr<FJsonObject> ResultAsset(const FLevelToolResult& Result, const TCHAR* ClassName)
{
	TArray<TSharedPtr<FJsonObject>> Assets = ResultAssets(Result, ClassName);
	return Assets.IsEmpty() ? nullptr : Assets[0];
}

inline TSharedPtr<FJsonObject> Facts(const TSharedPtr<FJsonObject>& Asset)
{
	return Asset.IsValid() ? Asset->GetObjectField(TEXT("facts")) : MakeShared<FJsonObject>();
}

inline FString AssetPath(const TSharedPtr<FJsonObject>& Asset)
{
	return Asset.IsValid() ? Asset->GetStringField(TEXT("path")) : FString();
}

inline double FactNumber(const TSharedPtr<FJsonObject>& Asset, const TCHAR* Field)
{
	double Value = -1.0;
	Facts(Asset)->TryGetNumberField(Field, Value);
	return Value;
}

inline FString FactString(const TSharedPtr<FJsonObject>& Asset, const TCHAR* Field)
{
	FString Value;
	Facts(Asset)->TryGetStringField(Field, Value);
	return Value;
}

inline bool FactBool(const TSharedPtr<FJsonObject>& Asset, const TCHAR* Field, bool bDefault)
{
	bool bValue = bDefault;
	Facts(Asset)->TryGetBoolField(Field, bValue);
	return bValue;
}

// facts.Bounds.Size (static and skeletal meshes) or facts.Size (textures) on Axis 0, 1, 2.
inline double FactSize(const TSharedPtr<FJsonObject>& Asset, int32 Axis)
{
	const TSharedPtr<FJsonObject> FactsJson = Facts(Asset);
	const TSharedPtr<FJsonObject>* Bounds = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Size = nullptr;
	if (FactsJson->TryGetObjectField(TEXT("Bounds"), Bounds))
	{
		(*Bounds)->TryGetArrayField(TEXT("Size"), Size);
	}
	else
	{
		FactsJson->TryGetArrayField(TEXT("Size"), Size);
	}
	return Size != nullptr && Size->IsValidIndex(Axis) ? (*Size)[Axis]->AsNumber() : -1.0;
}

inline TArray<FString> PayloadStrings(const FLevelToolResult& Result, const TCHAR* Field)
{
	TArray<FString> Strings;
	if (Result.Payload.IsValid())
	{
		Result.Payload->TryGetStringArrayField(Field, Strings);
	}
	return Strings;
}
}
#endif
