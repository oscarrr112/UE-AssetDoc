// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/AssetDocumentEditorPrimitiveHandler.h"

#include "Provider/AssetDocumentEditorPrimitiveCatalog.h"
#include "Provider/ClassReference/AssetDocumentEditorClassReferenceHandlers.h"
#include "Provider/Embedded/AssetDocumentEditorEmbeddedHandler.h"
#include "Provider/Lifecycle/AssetDocumentEditorLifecycleHandlers.h"
#include "Provider/Property/AssetDocumentEditorPropertyHandlers.h"
#include "Provider/Struct/AssetDocumentEditorStructHandler.h"

namespace AssetDocumentEditorProviderAssembly
{
void AppendBindings(
	TArray<FAssetDocumentEditorPrimitiveBinding>& OutBindings,
	TConstArrayView<FAssetDocumentEditorPrimitiveBinding> Bindings)
{
	OutBindings.Append(Bindings.GetData(), Bindings.Num());
}

TArray<FAssetDocumentEditorPrimitiveBinding> BuildBindings()
{
	TArray<FAssetDocumentEditorPrimitiveBinding> Bindings;
	Bindings.Reserve(13);
	AppendBindings(Bindings, GetAssetDocumentEditorClassReferencePrimitiveBindings());
	AppendBindings(Bindings, GetAssetDocumentEditorPropertyPrimitiveBindings());
	AppendBindings(Bindings, GetAssetDocumentEditorStructPrimitiveBindings());
	AppendBindings(Bindings, GetAssetDocumentEditorEmbeddedPrimitiveBindings());
	AppendBindings(Bindings, GetAssetDocumentEditorLifecyclePrimitiveBindings());
	return Bindings;
}

int32 CountBindings(
	TConstArrayView<FAssetDocumentEditorPrimitiveBinding> Bindings,
	FStringView PrimitiveId,
	uint32 Version)
{
	int32 Count = 0;
	for (const FAssetDocumentEditorPrimitiveBinding& Binding : Bindings)
	{
		Count += Binding.PrimitiveId == PrimitiveId
			&& Binding.Version == Version
			&& Binding.Handler != nullptr;
	}
	return Count;
}

int32 CountDescriptors(
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors,
	const FAssetDocumentEditorPrimitiveBinding& Binding)
{
	int32 Count = 0;
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Descriptors)
	{
		Count += Descriptor.PrimitiveId == Binding.PrimitiveId
			&& Descriptor.Version == Binding.Version;
	}
	return Count;
}

bool AreDescriptorsBound(
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors,
	TConstArrayView<FAssetDocumentEditorPrimitiveBinding> Bindings)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Descriptors)
	{
		if (CountBindings(Bindings, Descriptor.PrimitiveId, Descriptor.Version) != 1)
		{
			return false;
		}
	}
	return true;
}

bool AreBindingsDescribed(
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors,
	TConstArrayView<FAssetDocumentEditorPrimitiveBinding> Bindings)
{
	for (const FAssetDocumentEditorPrimitiveBinding& Binding : Bindings)
	{
		if (CountDescriptors(Descriptors, Binding) != 1)
		{
			return false;
		}
	}
	return true;
}
} // namespace AssetDocumentEditorProviderAssembly

TConstArrayView<FAssetDocumentEditorPrimitiveBinding>
GetAssetDocumentEditorPrimitiveBindings()
{
	static const TArray<FAssetDocumentEditorPrimitiveBinding> Bindings =
		AssetDocumentEditorProviderAssembly::BuildBindings();
	return Bindings;
}

bool IsAssetDocumentEditorPrimitiveAssemblyValid()
{
	const TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors =
		GetAssetDocumentEditorPrimitiveDescriptors();
	const TConstArrayView<FAssetDocumentEditorPrimitiveBinding> Bindings =
		GetAssetDocumentEditorPrimitiveBindings();
	if (Descriptors.Num() != Bindings.Num())
	{
		return false;
	}
	return AssetDocumentEditorProviderAssembly::AreDescriptorsBound(
		Descriptors, Bindings)
		&& AssetDocumentEditorProviderAssembly::AreBindingsDescribed(
			Descriptors, Bindings);
}
