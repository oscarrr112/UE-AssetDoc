// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "AssetDocumentGraphPrimitives.h"

// Audio asset primitives (canonical provider "audio", owner Audio). Same conventions as the data
// provider: one "asset" handle, value DTOs with schema_version 1, whole-package footprint for mutating
// calls (edit / compile), errors as Graph.* / MetaSound.* codes.
namespace UE::AssetDocument::Graph::Audio
{
inline const TCHAR* ProviderId = TEXT("audio");

TConstArrayView<FAssetDocumentPrimitiveDescriptor> GetDescriptors();
TConstArrayView<FBinding> GetBindings();

bool Fail(FCall& Call, FStringView Code, FStringView Message, FStringView Path = TEXTVIEW("/"));

// Shortest decimal that reads back as the same float32 (0.1, not 0.10000000149011612).
double ShortestFloat(float Value);

// A number value: an integer when it is integral (positions, sizes), else the shortest float32 form.
FValue MakeCoordinate(double Value);

// Path of an object; "" for null.
FString PathOf(const UObject* Object);

// MetaSound handlers (MetaSound/AssetDocumentMetaSound{Read,Catalog,Edit,Compile}.cpp).
bool HandleMetaSoundRead(FCall& Call, FObjectBuilder& Result);
bool HandleMetaSoundCatalog(FCall& Call, FObjectBuilder& Result);
bool HandleMetaSoundEdit(FCall& Call, FObjectBuilder& Result);
bool HandleMetaSoundCompile(FCall& Call, FObjectBuilder& Result);

// SoundCue read (SoundCue/AssetDocumentSoundCueRead.cpp); SoundCue edits go through the shared graph primitives.
bool HandleSoundCueRead(FCall& Call, FObjectBuilder& Result);
} // namespace UE::AssetDocument::Graph::Audio
