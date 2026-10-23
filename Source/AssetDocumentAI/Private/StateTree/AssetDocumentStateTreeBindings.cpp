// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// StateTree property bindings: what the binding menu of a property offers and writes. A source is
// accepted only when the editor would list it for the target (UStateTreeEditorData::GetBindableStructs,
// the structs updated before the target in the execution path) and its property is compatible
// (UE::PropertyBinding::GetPropertyCompatibility); the write is FPropertyBindingBindingCollection::
// AddBinding or FStateTreeEditorPropertyBindings::AddFunctionBinding (property functions, whose own
// inputs are bound the same way). Output bindings are preserved and not authored.

#include "StateTree/AssetDocumentStateTreePrimitives.h"

#include "Kismet2/StructureEditorUtils.h"
#include "PropertyBindingDataView.h"
#include "PropertyBindingPath.h"
#include "PropertyBindingTypes.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeEditorPropertyBindings.h"
#include "StateTreePropertyBindings.h"
#include "StateTreePropertyFunctionBase.h"
#include "StateTreeSchema.h"
#include "StructUtils/UserDefinedStruct.h"

namespace UE::AssetDocument::Graph::StateTree
{
namespace
{
using ESearchMode = FPropertyBindingBindingCollection::ESearchMode;

// The path as the document writes it: property names (a user defined struct's members by their friendly
// names), not the display names the engine may show for them.
FString BindingPathText(const FPropertyBindingPath& Path)
{
	TStringBuilder<256> Result;
	const TConstArrayView<FPropertyBindingPathSegment> Segments = Path.GetSegments();
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FPropertyBindingPathSegment& Segment = Segments[Index];
		if (Index > 0)
		{
			Result << TEXT('.');
		}
		const UUserDefinedStruct* Parent = Index > 0 ? Cast<const UUserDefinedStruct>(Segments[Index - 1].GetInstanceStruct()) : nullptr;
		const FString FriendlyName = Parent != nullptr ? FStructureEditorUtils::GetVariableFriendlyName(Parent, Segment.GetPropertyGuid()) : FString();
		if (!FriendlyName.IsEmpty())
		{
			Result << FriendlyName;
		}
		else
		{
			Result << Segment.GetName();
		}
		if (Segment.GetArrayIndex() >= 0)
		{
			Result << TEXT('[') << Segment.GetArrayIndex() << TEXT(']');
		}
	}
	return Result.ToString();
}

FValue MakePathValue(const FPropertyBindingPath& Path)
{
	return FObjectBuilder().AddString(TEXT("struct"), GuidString(Path.GetStructID())).AddString(TEXT("path"), BindingPathText(Path)).Build();
}

const FStateTreeEditorNode* GetFunctionNode(const FStateTreePropertyPathBinding& Binding)
{
	const FConstStructView Function = Binding.GetPropertyFunctionNode();
	return Function.IsValid() ? Function.GetPtr<const FStateTreeEditorNode>() : nullptr;
}

FStateTreeEditorNode* GetMutableFunctionNode(FStateTreePropertyPathBinding& Binding)
{
	const FStructView Function = Binding.GetMutablePropertyFunctionNode();
	return Function.IsValid() ? Function.GetPtr<FStateTreeEditorNode>() : nullptr;
}

FStateTreePropertyPathBinding* FindMutableBinding(FStateTreeEditorPropertyBindings& Bindings, const FPropertyBindingPath& Target)
{
	for (FStateTreePropertyPathBinding& Binding : Bindings.GetMutableBindings())
	{
		if (!Binding.IsOutputBinding() && Binding.GetTargetPath() == Target)
		{
			return &Binding;
		}
	}
	return nullptr;
}

// A path within one bindable struct: struct id + property path, resolved against the struct's value.
// An empty source path binds the whole struct / object (a context object such as the Actor).
struct FResolvedPath
{
	FPropertyBindingPath Path;
	const FProperty* Leaf = nullptr;
	const UStruct* WholeStruct = nullptr;
};

// DefaultStruct: the struct of a path written without "struct" (the inputs of a property function,
// whose node id the caller does not know before the function is created).
bool ResolvePath(FReader& Reader, FStringView Key, const UStateTreeEditorData& EditorData, FResolvedPath& Out, bool bAllowWhole = false,
	const FGuid* DefaultStruct = nullptr)
{
	const FValue* Value = nullptr;
	if (!Reader.Object(Key, Value))
	{
		return false;
	}
	FReader PathReader = Reader.Child(Key, *Value);
	FGuid StructId;
	FString PathText;
	if (!PathReader.OnlyFields({TEXT("struct"), TEXT("path")}) || !PathReader.Guid(TEXT("struct"), StructId, DefaultStruct == nullptr)
		|| !PathReader.String(TEXT("path"), PathText))
	{
		return false;
	}
	if (!StructId.IsValid() && DefaultStruct != nullptr)
	{
		StructId = *DefaultStruct;
	}
	// Context data has a type but no value in the editor (UStateTreeEditorData::VisitGlobalNodes): such
	// paths resolve against the type, as the binding menu does.
	FPropertyBindingDataView View;
	if (!EditorData.GetBindingDataViewByID(StructId, View) || View.GetStruct() == nullptr)
	{
		return PathReader.Fail(TEXT("StateTree.InvalidBinding"), TEXT("The bound struct does not exist in this tree."), TEXT("struct"));
	}
	Out.Path = FPropertyBindingPath(StructId);
	if (PathText.IsEmpty() && bAllowWhole)
	{
		Out.WholeStruct = View.GetStruct();
		return Out.WholeStruct != nullptr ? true
			: PathReader.Fail(TEXT("StateTree.InvalidBinding"), TEXT("The bound struct has no type."), TEXT("struct"));
	}
	if (PathText.IsEmpty() || !Out.Path.FromString(PathText))
	{
		return PathReader.Fail(TEXT("StateTree.InvalidBinding"), FString::Printf(TEXT("'%s' is not a property path."), *PathText), TEXT("path"));
	}
	FString Error;
	TArray<FPropertyBindingPathIndirection> Indirections;
	const bool bResolved = View.IsValid()
		? Out.Path.UpdateSegmentsFromValue(View, &Error) && Out.Path.ResolveIndirectionsWithValue(View, Indirections, &Error)
		: Out.Path.UpdateSegments(View.GetStruct(), &Error) && Out.Path.ResolveIndirections(View.GetStruct(), Indirections, &Error);
	if (!bResolved || Indirections.IsEmpty())
	{
		TArray<FString> Names;
		for (TFieldIterator<FProperty> It(View.GetStruct()); It; ++It)
		{
			Names.Add(It->GetAuthoredName());
		}
		return PathReader.Fail(TEXT("StateTree.InvalidBinding"), FString::Printf(TEXT("'%s' does not resolve in %s: %s Properties: %s."),
			*PathText, *GetNameSafe(View.GetStruct()), *Error, *FString::Join(Names, TEXT(", "))), TEXT("path"));
	}
	Out.Leaf = Indirections.Last().GetProperty();
	return Out.Leaf != nullptr ? true : PathReader.Fail(TEXT("StateTree.InvalidBinding"), TEXT("The path names no property."), TEXT("path"));
}

bool IsAccessible(const UStateTreeEditorData& EditorData, const FGuid& TargetStruct, const FGuid& SourceStruct)
{
	TArray<TInstancedStruct<FPropertyBindingBindableStructDescriptor>> Accessible;
	EditorData.GetBindableStructs(TargetStruct, Accessible);
	return Accessible.ContainsByPredicate([&SourceStruct](const TInstancedStruct<FPropertyBindingBindableStructDescriptor>& Desc)
	{
		return Desc.IsValid() && Desc.Get().ID == SourceStruct;
	});
}

bool CheckCompatible(FReader& Item, const FResolvedPath& Source, const FResolvedPath& Target)
{
	if (Source.WholeStruct != nullptr)
	{
		// FCachedBindingData::ArePropertyAndContextStructCompatible: the same struct, or an object
		// property whose class the bound object is.
		const FStructProperty* TargetStruct = CastField<FStructProperty>(Target.Leaf);
		const FObjectProperty* TargetObject = CastField<FObjectProperty>(Target.Leaf);
		const bool bCompatible = TargetStruct != nullptr ? TargetStruct->Struct == Source.WholeStruct
			: TargetObject != nullptr && Source.WholeStruct->IsChildOf(TargetObject->PropertyClass);
		return bCompatible ? true : Item.Fail(TEXT("StateTree.BindingTypeMismatch"), FString::Printf(
			TEXT("Cannot bind %s to %s '%s': the types are not compatible."), *Source.WholeStruct->GetName(),
			*Target.Leaf->GetCPPType(), *BindingPathText(Target.Path)), TEXT("source"));
	}
	if (UE::PropertyBinding::GetPropertyCompatibility(Source.Leaf, Target.Leaf) == UE::PropertyBinding::EPropertyCompatibility::Incompatible)
	{
		return Item.Fail(TEXT("StateTree.BindingTypeMismatch"), FString::Printf(TEXT("Cannot bind %s '%s' to %s '%s': the types are not compatible."),
			*Source.Leaf->GetCPPType(), *BindingPathText(Source.Path), *Target.Leaf->GetCPPType(), *BindingPathText(Target.Path)), TEXT("source"));
	}
	return true;
}

// Removes the bindings that target StructId (and, recursively, those of property functions that go away).
void RemoveBindingsOf(FStateTreeEditorPropertyBindings& Bindings, const FGuid& StructId, TFunctionRef<bool(const FStateTreePropertyPathBinding&)> Keep)
{
	TArray<FGuid> RemovedFunctions;
	for (const FStateTreePropertyPathBinding& Binding : Bindings.GetBindings())
	{
		if (!Binding.IsOutputBinding() && Binding.GetTargetPath().GetStructID() == StructId && !Keep(Binding))
		{
			if (const FStateTreeEditorNode* Function = GetFunctionNode(Binding))
			{
				RemovedFunctions.Add(Function->ID);
			}
		}
	}
	Bindings.RemoveBindings([&StructId, &Keep](FPropertyBindingBinding& Binding)
	{
		const FStateTreePropertyPathBinding& StateTreeBinding = static_cast<const FStateTreePropertyPathBinding&>(Binding);
		return !StateTreeBinding.IsOutputBinding() && Binding.GetTargetPath().GetStructID() == StructId && !Keep(StateTreeBinding);
	});
	for (const FGuid& Function : RemovedFunctions)
	{
		RemoveBindingsOf(Bindings, Function, [](const FStateTreePropertyPathBinding&) { return false; });
	}
}

bool ApplyBindings(FCall& Call, FReader& ListReader, const FValue& List, UStateTreeEditorData& EditorData, const TSet<FGuid>& Targets,
	const FGuid* DefaultTarget);

bool ApplyFunctionBinding(FCall& Call, FReader& Item, const FResolvedPath& Target, UStateTreeEditorData& EditorData)
{
	const FValue* FunctionValue = nullptr;
	if (!Item.Object(TEXT("function"), FunctionValue))
	{
		return false;
	}
	FReader Function = Item.Child(TEXT("function"), *FunctionValue);
	FString TypeText;
	const FValue* Nested = nullptr;
	if (!Function.OnlyFields({TEXT("type"), TEXT("instance"), TEXT("bindings")}) || !Function.String(TEXT("type"), TypeText)
		|| !Function.Array(TEXT("bindings"), Nested, false))
	{
		return false;
	}
	const UScriptStruct* FunctionStruct = Cast<UScriptStruct>(ResolveType(TypeText, FStateTreePropertyFunctionBase::StaticStruct(), nullptr));
	if (FunctionStruct == nullptr || FunctionStruct->HasMetaData(TEXT("Hidden")))
	{
		return Function.Fail(TEXT("StateTree.InvalidPropertyFunction"), FString::Printf(TEXT("'%s' is not a property function struct "
			"(list_types kind struct, base /Script/StateTreeModule.StateTreePropertyFunctionBase)."), *TypeText), TEXT("type"));
	}
	if (EditorData.Schema != nullptr && !EditorData.Schema->IsStructAllowed(FunctionStruct))
	{
		return Function.Fail(TEXT("StateTree.NodeNotAllowed"), FString::Printf(TEXT("The schema does not allow %s."), *FunctionStruct->GetPathName()),
			TEXT("type"));
	}
	FInstancedStruct Probe(FunctionStruct);
	const UStruct* InstanceType = Probe.Get<FStateTreePropertyFunctionBase>().GetInstanceDataType();
	const FProperty* Output = InstanceType != nullptr ? UE::StateTree::GetStructSingleOutputProperty(*InstanceType) : nullptr;
	if (Output == nullptr)
	{
		return Function.Fail(TEXT("StateTree.InvalidPropertyFunction"), FString::Printf(TEXT("%s has no single output property."),
			*FunctionStruct->GetName()), TEXT("type"));
	}
	if (UE::PropertyBinding::GetPropertyCompatibility(Output, Target.Leaf) == UE::PropertyBinding::EPropertyCompatibility::Incompatible)
	{
		return Function.Fail(TEXT("StateTree.BindingTypeMismatch"), FString::Printf(TEXT("%s outputs %s, which cannot bind to %s '%s'."),
			*FunctionStruct->GetName(), *Output->GetCPPType(), *Target.Leaf->GetCPPType(), *BindingPathText(Target.Path)), TEXT("type"));
	}
	FStateTreeEditorPropertyBindings& Bindings = EditorData.EditorBindings;
	FStateTreePropertyPathBinding* Existing = FindMutableBinding(Bindings, Target.Path);
	const FStateTreeEditorNode* ExistingNode = Existing != nullptr ? GetFunctionNode(*Existing) : nullptr;
	if (ExistingNode == nullptr || ExistingNode->Node.GetScriptStruct() != FunctionStruct)
	{
		const TArray<FPropertyBindingPathSegment> Segments = {FPropertyBindingPathSegment(Output->GetFName())};
		Bindings.AddFunctionBinding(FunctionStruct, Segments, Target.Path);
		Existing = FindMutableBinding(Bindings, Target.Path);
	}
	FStateTreeEditorNode* Node = Existing != nullptr ? GetMutableFunctionNode(*Existing) : nullptr;
	if (Node == nullptr)
	{
		return Function.Fail(TEXT("StateTree.InvalidPropertyFunction"), TEXT("The property function binding could not be created."), TEXT("type"));
	}
	const FGuid NodeId = Node->ID;
	if (const UScriptStruct* InstanceStruct = Node->Instance.GetScriptStruct())
	{
		if (!DecodeValues(Function, TEXT("instance"), *InstanceStruct, Node->Instance.GetMutableMemory(), &EditorData, EValueOwner::InstanceStruct))
		{
			return false;
		}
	}
	TSet<FGuid> NestedTargets = {NodeId};
	FValue Empty = MakeArray({});
	FReader NestedReader = Function.Child(TEXT("bindings"), Nested != nullptr ? *Nested : Empty);
	return ApplyBindings(Call, NestedReader, Nested != nullptr ? *Nested : Empty, EditorData, NestedTargets, &NodeId);
}

bool ApplyBindings(FCall& Call, FReader& ListReader, const FValue& List, UStateTreeEditorData& EditorData, const TSet<FGuid>& Targets,
	const FGuid* DefaultTarget)
{
	struct FDesired
	{
		FResolvedPath Target;
		FResolvedPath Source;
		bool bFunction = false;
		FString FunctionType;
		int32 Index = 0;
	};
	TArray<FDesired> Desired;
	for (int32 Index = 0; Index < List.Elements.Num(); ++Index)
	{
		FReader Item = ListReader.Child(FString::FromInt(Index), List.Elements[Index]);
		FDesired& Entry = Desired.AddDefaulted_GetRef();
		Entry.Index = Index;
		if (!Item.IsObject())
		{
			return Item.Fail(TEXT("StateTree.InvalidBinding"), TEXT("Each binding is an object."));
		}
		if (!Item.OnlyFields({TEXT("target"), TEXT("source"), TEXT("function")}) || !ResolvePath(Item, TEXT("target"), EditorData, Entry.Target, false, DefaultTarget))
		{
			return false;
		}
		if (!Targets.Contains(Entry.Target.Path.GetStructID()))
		{
			return Item.Fail(TEXT("StateTree.InvalidBinding"), TEXT("The binding target is not one of the op's targets."), TEXT("target"));
		}
		if (UE::StateTree::GetUsageFromMetaData(Entry.Target.Leaf) == EStateTreePropertyUsage::Output)
		{
			return Item.Fail(TEXT("StateTree.InvalidBinding"), FString::Printf(TEXT("'%s' is an output; outputs are bound from, not to."),
				*BindingPathText(Entry.Target.Path)), TEXT("target"));
		}
		if (Desired.ContainsByPredicate([&Entry](const FDesired& Other) { return &Other != &Entry && Other.Target.Path == Entry.Target.Path; }))
		{
			return Item.Fail(TEXT("StateTree.InvalidBinding"), FString::Printf(TEXT("'%s' is bound twice."), *BindingPathText(Entry.Target.Path)), TEXT("target"));
		}
		Entry.bFunction = Item.Has(TEXT("function"));
		if (Entry.bFunction == Item.Has(TEXT("source")))
		{
			return Item.Fail(TEXT("StateTree.InvalidBinding"), TEXT("A binding has either a source or a property function."), TEXT("source"));
		}
		if (Entry.bFunction)
		{
			const FValue* Function = Item.Find(TEXT("function"));
			const FValue* Type = Function != nullptr ? FindField(*Function, TEXT("type")) : nullptr;
			Entry.FunctionType = Type != nullptr ? Type->StringValue : FString();
			continue;
		}
		if (!ResolvePath(Item, TEXT("source"), EditorData, Entry.Source, true))
		{
			return false;
		}
		if (!IsAccessible(EditorData, Entry.Target.Path.GetStructID(), Entry.Source.Path.GetStructID()))
		{
			return Item.Fail(TEXT("StateTree.BindingSourceNotAccessible"), TEXT("The binding menu does not offer this source for the target: "
				"a node can bind to the context, the tree parameters, evaluators, global tasks and the parameters, events and tasks of "
				"the states that are active before it (parents and earlier tasks)."), TEXT("source"));
		}
		if (!CheckCompatible(Item, Entry.Source, Entry.Target))
		{
			return false;
		}
	}
	FStateTreeEditorPropertyBindings& Bindings = EditorData.EditorBindings;
	for (const FGuid& Target : Targets)
	{
		RemoveBindingsOf(Bindings, Target, [&Desired](const FStateTreePropertyPathBinding& Binding)
		{
			const FDesired* Match = Desired.FindByPredicate([&Binding](const FDesired& Each) { return Each.Target.Path == Binding.GetTargetPath(); });
			if (Match == nullptr)
			{
				return false;
			}
			const FStateTreeEditorNode* Function = GetFunctionNode(Binding);
			if (Match->bFunction)
			{
				// Kept when it is the same property function; ApplyFunctionBinding updates it.
				return Function != nullptr && Function->Node.GetScriptStruct() != nullptr
					&& Function->Node.GetScriptStruct() == ResolveType(Match->FunctionType, FStateTreePropertyFunctionBase::StaticStruct(), nullptr);
			}
			return Function == nullptr && Binding.GetSourcePath() == Match->Source.Path;
		});
	}
	for (const FDesired& Entry : Desired)
	{
		FReader Item = ListReader.Child(FString::FromInt(Entry.Index), List.Elements[Entry.Index]);
		if (Entry.bFunction)
		{
			if (!ApplyFunctionBinding(Call, Item, Entry.Target, EditorData))
			{
				return false;
			}
		}
		else if (!Bindings.HasBinding(Entry.Target.Path, ESearchMode::Exact))
		{
			Bindings.AddBinding(Entry.Source.Path, Entry.Target.Path);
		}
	}
	return true;
}
} // namespace

void AppendBindingRecords(TArray<FValue>& Out, const UStateTreeEditorData& EditorData)
{
	for (const FStateTreePropertyPathBinding& Binding : EditorData.EditorBindings.GetBindings())
	{
		FObjectBuilder Record;
		Record.AddString(TEXT("kind"), TEXT("binding"));
		Record.Add(TEXT("source"), MakePathValue(Binding.GetSourcePath()));
		Record.Add(TEXT("target"), MakePathValue(Binding.GetTargetPath()));
		Record.AddBool(TEXT("output"), Binding.IsOutputBinding());
		if (const FStateTreeEditorNode* Function = GetFunctionNode(Binding))
		{
			FObjectBuilder Value;
			Value.AddString(TEXT("id"), GuidString(Function->ID));
			Value.AddString(TEXT("node_id"), GuidString(Function->GetNodeID()));
			Value.AddString(TEXT("type"), PathOf(Function->Node.GetScriptStruct()));
			TArray<FString> Defaults;
			TArray<FString> Unsupported;
			if (const UScriptStruct* InstanceStruct = Function->Instance.GetScriptStruct())
			{
				FInstancedStruct Default(InstanceStruct);
				Value.Add(TEXT("instance"), EncodeValues(*InstanceStruct, Function->Instance.GetMemory(), Default.GetMemory(),
					EValueOwner::InstanceStruct, Defaults, Unsupported));
			}
			else
			{
				Value.Add(TEXT("instance"), MakeEmptyObject());
			}
			Value.Add(TEXT("instance_defaults"), MakeStringArray(Defaults));
			Value.Add(TEXT("unsupported"), MakeStringArray(Unsupported));
			Record.Add(TEXT("function"), Value.Build());
		}
		Out.Add(Record.Build());
	}
}

bool EditBindings(FCall& Call, FReader& Op, UStateTreeEditorData& EditorData)
{
	const FValue* TargetValues = nullptr;
	const FValue* List = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("targets"), TEXT("bindings")}) || !Op.Array(TEXT("targets"), TargetValues)
		|| !Op.Array(TEXT("bindings"), List))
	{
		return false;
	}
	TSet<FGuid> Targets;
	for (int32 Index = 0; Index < TargetValues->Elements.Num(); ++Index)
	{
		FGuid Id;
		if (TargetValues->Elements[Index].Kind != EAssetDocumentRuntimeValueKind::String || !TryParseGuid(TargetValues->Elements[Index].StringValue, Id))
		{
			return Op.Fail(TEXT("StateTree.InvalidBinding"), TEXT("targets lists struct ids."), FString::Printf(TEXT("targets/%d"), Index));
		}
		Targets.Add(Id);
	}
	EditorData.Modify();
	FReader ListReader = Op.Child(TEXT("bindings"), *List);
	return ApplyBindings(Call, ListReader, *List, EditorData, Targets, nullptr);
}
} // namespace UE::AssetDocument::Graph::StateTree
