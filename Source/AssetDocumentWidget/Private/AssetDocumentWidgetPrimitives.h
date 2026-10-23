// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "AssetDocumentGraphPrimitives.h"

class UPanelSlot;
class UWidget;
class UWidgetBlueprint;
class UWidgetTree;

// Widget Blueprint primitives (canonical provider "umg", owner UMG): the Designer
// widget tree, widget/slot/navigation properties, property bindings and widget
// animations of a UWidgetBlueprint. Same conventions as the graph and member
// providers: one "asset" handle, value DTOs with schema_version 1, whole-package
// footprint, no typed inverse (Core restores the package snapshot on rollback).
namespace UE::AssetDocument::Graph::Widget
{
inline const TCHAR* ProviderId = TEXT("umg");

TConstArrayView<FAssetDocumentPrimitiveDescriptor> GetDescriptors();
TConstArrayView<FBinding> GetBindings();

// ---- shared by the widget primitive files -------------------------------------

FAssetDocumentPrimitiveDescriptor MakeDescriptor(const TCHAR* PrimitiveId, bool bMutating, const TCHAR* AuthoritativeApi);
bool Fail(FCall& Call, FStringView Code, FStringView Message, FStringView Path = TEXTVIEW("/"));
UWidgetBlueprint* GetWidgetBlueprint(FCall& Call);

// A widget of the Blueprint's source tree by name, including one that is currently
// detached (an apply moves it again before it finishes).
UWidget* FindSourceWidget(UWidgetBlueprint& Blueprint, FName Name);

// Properties the Designer Details panel edits on a widget or slot template and the
// typed codec round-trips (instanced subobjects are reached through Slot / Navigation).
bool IsDesignerEditable(const FProperty& Property);

// Display frames <-> tick frames of a movie scene.
double TicksToDisplay(int32 Ticks, const FFrameRate& Tick, const FFrameRate& Display);
int32 DisplayToTicks(double Frames, const FFrameRate& Tick, const FFrameRate& Display);
} // namespace UE::AssetDocument::Graph::Widget
