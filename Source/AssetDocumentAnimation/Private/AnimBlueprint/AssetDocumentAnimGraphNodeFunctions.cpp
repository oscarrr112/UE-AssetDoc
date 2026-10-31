// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AnimBlueprint/AssetDocumentAnimGraphNodeFunctions.h"

#include "AnimBlueprint/AssetDocumentAnimBlueprintAdapters.h"
#include "AnimGraphNode_Base.h"
#include "Animation/AnimBlueprint.h"
#include "EdGraph/EdGraph.h"
#include "Engine/MemberReference.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::AnimBlueprint::NodeFunctions
{
namespace NodeFunctionDetail
{
struct FSlot
{
	const TCHAR* Setting;
	FName Property;
};

const FSlot Slots[] = {
	{TEXT("Functions.OnInitialUpdate"), GET_MEMBER_NAME_CHECKED(UAnimGraphNode_Base, InitialUpdateFunction)},
	{TEXT("Functions.OnBecomeRelevant"), GET_MEMBER_NAME_CHECKED(UAnimGraphNode_Base, BecomeRelevantFunction)},
	{TEXT("Functions.OnUpdate"), GET_MEMBER_NAME_CHECKED(UAnimGraphNode_Base, UpdateFunction)},
};

const FSlot* FindSlot(FStringView Path)
{
	for (const FSlot& Slot : Slots)
	{
		if (Path == FStringView(Slot.Setting))
		{
			return &Slot;
		}
	}
	return nullptr;
}

FStructProperty* SlotProperty(const FSlot& Slot)
{
	FStructProperty* Property = FindFProperty<FStructProperty>(UAnimGraphNode_Base::StaticClass(), Slot.Property);
	return Property != nullptr && Property->Struct == FMemberReference::StaticStruct() ? Property : nullptr;
}

UClass* SelfScope(const UAnimGraphNode_Base& Node)
{
	const UBlueprint* Blueprint = Node.HasValidBlueprint() ? Node.GetBlueprint() : nullptr;
	return Blueprint != nullptr && Blueprint->SkeletonGeneratedClass != nullptr ? Blueprint->SkeletonGeneratedClass.Get()
		: Node.GetBlueprintClassFromNode();
}

// The prototype the function picker checks against (the property's PrototypeFunction metadata).
const UFunction* Prototype(const FStructProperty& Property)
{
	const FString& Name = Property.GetMetaData(TEXT("PrototypeFunction"));
	return Name.IsEmpty() ? nullptr : FindObject<UFunction>(nullptr, *Name);
}

// FBlueprintMemberReferenceDetails' OnCanBindFunction.
bool CanBind(const UFunction* PrototypeFunction, const UFunction& Function)
{
	return PrototypeFunction == nullptr
		|| (PrototypeFunction->IsSignatureCompatibleWith(&Function)
			&& FBlueprintEditorUtils::HasFunctionBlueprintThreadSafeMetaData(PrototypeFunction)
				== FBlueprintEditorUtils::HasFunctionBlueprintThreadSafeMetaData(&Function));
}

FString Describe(const FMemberReference& Reference, UClass* Scope)
{
	if (Reference.GetMemberName().IsNone())
	{
		return FString();
	}
	const UFunction* Function = Reference.ResolveMember<UFunction>(Scope);
	const FString Name = Function != nullptr ? Function->GetName() : Reference.GetMemberName().ToString();
	if (Reference.IsSelfContext())
	{
		return Name;
	}
	const UClass* Parent = Reference.GetMemberParentClass();
	return Parent != nullptr ? Parent->GetPathName() + TEXT(":") + Name : Name;
}

TArray<FString> Candidates(UClass* Scope, const UFunction* PrototypeFunction)
{
	TArray<FString> Names;
	for (TFieldIterator<UFunction> It(Scope, EFieldIteratorFlags::IncludeSuper); It && Scope != nullptr; ++It)
	{
		if (CanBind(PrototypeFunction, **It))
		{
			Names.AddUnique(It->GetName());
		}
	}
	Names.Sort();
	return Names;
}
} // namespace NodeFunctionDetail

void QueryNodeFunctions(const UAnimGraphNode_Base& Node, FAssetDocumentRuntimeValueDto& Settings)
{
	using namespace NodeFunctionDetail;
	UClass* Scope = SelfScope(Node);
	for (const FSlot& Slot : Slots)
	{
		const FStructProperty* Property = SlotProperty(Slot);
		const FMemberReference* Reference = Property != nullptr ? Property->ContainerPtrToValuePtr<FMemberReference>(&Node) : nullptr;
		const FString Text = Reference != nullptr ? Describe(*Reference, Scope) : FString();
		if (!Text.IsEmpty())
		{
			SetField(Settings, Slot.Setting, MakeStringValue(Text));
		}
	}
}

bool IsNodeFunctionSetting(FStringView Path)
{
	return NodeFunctionDetail::FindSlot(Path) != nullptr;
}

bool SetNodeFunction(UAnimGraphNode_Base& Node, FStringView Path, const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentGraphError& OutError)
{
	using namespace NodeFunctionDetail;
	const FSlot* Slot = FindSlot(Path);
	FStructProperty* Property = Slot != nullptr ? SlotProperty(*Slot) : nullptr;
	if (Property == nullptr)
	{
		OutError.Set(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("'%.*s' is not a node function setting."), Path.Len(), Path.GetData()),
			TEXT("/property_path"));
		return false;
	}
	if (Value.Kind != EAssetDocumentRuntimeValueKind::String && Value.Kind != EAssetDocumentRuntimeValueKind::Null)
	{
		OutError.Set(TEXT("Graph.InvalidArgument"), FString::Printf(
			TEXT("%s is a function name (a function of the Animation Blueprint), \"<class path>:<function>\" (a function library's) or \"\" (none)."),
			Slot->Setting), TEXT("/value"));
		return false;
	}
	UBlueprint* Blueprint = Node.HasValidBlueprint() ? Node.GetBlueprint() : nullptr;
	UClass* Scope = SelfScope(Node);
	const FString Text = Value.Kind == EAssetDocumentRuntimeValueKind::String ? Value.StringValue.TrimStartAndEnd() : FString();
	const UFunction* PrototypeFunction = Prototype(*Property);
	FMemberReference Wanted;
	if (!Text.IsEmpty())
	{
		FString ClassPath;
		FString FunctionName = Text;
		Text.Split(TEXT(":"), &ClassPath, &FunctionName, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		UClass* Owner = ClassPath.IsEmpty() ? Scope : LoadObject<UClass>(nullptr, *ClassPath);
		if (Owner == nullptr)
		{
			OutError.Set(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("%s: class '%s' does not load."), Slot->Setting, *ClassPath),
				TEXT("/value"));
			return false;
		}
		const UFunction* Function = Owner->FindFunctionByName(FName(*FunctionName));
		// A function graph of this Blueprint made in the same apply is not in the class until it compiles: it binds by
		// its graph, as the picker's "Create Binding" does, and the compile checks its signature.
		const TObjectPtr<UEdGraph>* Graph = ClassPath.IsEmpty() && Function == nullptr && Blueprint != nullptr
			? Blueprint->FunctionGraphs.FindByPredicate([&FunctionName](const UEdGraph* Candidate)
				{ return Candidate != nullptr && Candidate->GetName() == FunctionName; })
			: nullptr;
		if (Function == nullptr && Graph == nullptr)
		{
			OutError.Set(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("%s: '%s' is not a function of %s (functions it can bind: %s)."),
				Slot->Setting, *FunctionName, *Owner->GetName(), *FString::Join(Candidates(Scope, PrototypeFunction), TEXT(", "))),
				TEXT("/value"));
			return false;
		}
		if (Function != nullptr && !CanBind(PrototypeFunction, *Function))
		{
			OutError.Set(TEXT("Graph.InvalidArgument"), FString::Printf(
				TEXT("%s: '%s' cannot be bound: a node function is thread safe and takes (Context, Node) like the picker's prototype "
					 "(functions it can bind: %s)."),
				Slot->Setting, *FunctionName, *FString::Join(Candidates(Scope, PrototypeFunction), TEXT(", "))), TEXT("/value"));
			return false;
		}
		if (Function != nullptr)
		{
			// The picker's OnAddBinding: a function of the Blueprint or a parent class is self context.
			UClass* OwnerClass = Function->GetOwnerClass();
			const bool bSelfContext = OwnerClass != nullptr && Blueprint != nullptr
				&& ((Blueprint->GeneratedClass != nullptr && Blueprint->GeneratedClass->IsChildOf(OwnerClass))
					|| (Blueprint->SkeletonGeneratedClass != nullptr && Blueprint->SkeletonGeneratedClass->IsChildOf(OwnerClass)));
			Wanted.SetFromField<UFunction>(Function, bSelfContext);
		}
		else
		{
			Wanted.SetSelfMember(FName(*FunctionName), (*Graph)->GraphGuid);
		}
	}
	FMemberReference* Current = Property->ContainerPtrToValuePtr<FMemberReference>(&Node);
	if (Describe(*Current, Scope) == Describe(Wanted, Scope) && Current->GetMemberGuid() == Wanted.GetMemberGuid())
	{
		return true;
	}
	// The details panel's edit: PreEditChange / write / PostEditChangeProperty (which resolves the runtime node's
	// function and reconstructs the node).
	Node.Modify();
	Node.PreEditChange(Property);
	*Current = Wanted;
	FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
	Node.PostEditChangeProperty(Event);
	if (Blueprint != nullptr)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}
	return true;
}
} // namespace UE::AssetDocument::AnimBlueprint::NodeFunctions
