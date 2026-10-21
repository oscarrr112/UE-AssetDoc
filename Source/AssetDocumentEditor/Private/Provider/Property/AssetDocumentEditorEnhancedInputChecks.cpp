// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Property/AssetDocumentEditorEnhancedInputChecks.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EnhancedActionKeyMapping.h"
#include "Engine/Blueprint.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "Misc/PackageName.h"
#include "Provider/AssetDocumentEditorNativeIdentity.h"
#include "Provider/AssetDocumentEditorValueDto.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"

namespace EditorEnhancedInputChecksLocal
{
// The Blueprint nodes the editor types from an action's ValueType (InputBlueprintNodes; found by name so this
// module does not link the editor-only node module): the event's ActionValue, the getter's ReturnValue.
struct FActionNodeClass
{
	const TCHAR* ClassPath;
	const TCHAR* ValuePin;
};

constexpr FActionNodeClass ActionNodeClasses[] = {
	{TEXT("/Script/InputBlueprintNodes.K2Node_EnhancedInputAction"), TEXT("ActionValue")},
	{TEXT("/Script/InputBlueprintNodes.K2Node_GetInputActionValue"), TEXT("ReturnValue")},
};

constexpr int32 MaxListedNodes = 5;

FString ValueTypeName(EInputActionValueType Type)
{
	switch (Type)
	{
	case EInputActionValueType::Boolean: return TEXT("Boolean (bool)");
	case EInputActionValueType::Axis1D: return TEXT("Axis1D (float)");
	case EInputActionValueType::Axis2D: return TEXT("Axis2D (Vector2D)");
	default: return TEXT("Axis3D (Vector)");
	}
}

// The pin type the editor gives a node of an action of this ValueType (UK2Node_GetInputActionValue's lookup).
bool PinMatchesValueType(const FEdGraphPinType& PinType, EInputActionValueType Type)
{
	switch (Type)
	{
	case EInputActionValueType::Boolean: return PinType.PinCategory == TEXT("bool");
	case EInputActionValueType::Axis1D: return PinType.PinCategory == TEXT("real");
	case EInputActionValueType::Axis2D:
		return PinType.PinCategory == TEXT("struct")
			&& PinType.PinSubCategoryObject.Get() == TBaseStructure<FVector2D>::Get();
	default:
		return PinType.PinCategory == TEXT("struct")
			&& PinType.PinSubCategoryObject.Get() == TBaseStructure<FVector>::Get();
	}
}

FString PinTypeName(const FEdGraphPinType& PinType)
{
	if (PinType.PinCategory == TEXT("bool"))
	{
		return TEXT("bool");
	}
	if (PinType.PinCategory == TEXT("real"))
	{
		return TEXT("float");
	}
	if (const UObject* SubObject = PinType.PinSubCategoryObject.Get())
	{
		return SubObject->GetName();
	}
	return PinType.PinCategory.ToString();
}

bool SamePinValueType(const FEdGraphPinType& A, const FEdGraphPinType& B)
{
	return A.PinCategory == B.PinCategory
		&& (A.PinCategory != TEXT("struct") || A.PinSubCategoryObject == B.PinSubCategoryObject);
}

// What no longer fits on a loaded node of Action, given the action's (new) ValueType; empty when it fits.
FString DescribeNodeProblem(const UEdGraphNode& Node, const TCHAR* ValuePinName, EInputActionValueType Type)
{
	const UEdGraphPin* ValuePin = Node.FindPin(ValuePinName, EGPD_Output);
	if (ValuePin == nullptr)
	{
		return FString();
	}
	if (!PinMatchesValueType(ValuePin->PinType, Type))
	{
		return FString::Printf(TEXT("its %s pin is %s"), ValuePinName, *PinTypeName(ValuePin->PinType));
	}
	for (const UEdGraphPin* Linked : ValuePin->LinkedTo)
	{
		if (Linked != nullptr && Linked->PinType.PinCategory != TEXT("wildcard")
			&& !SamePinValueType(Linked->PinType, ValuePin->PinType))
		{
			const UEdGraphNode* Other = Linked->GetOwningNodeUnchecked();
			return FString::Printf(TEXT("its %s pin (%s) is linked to %s.%s, a %s pin"), ValuePinName,
				*PinTypeName(ValuePin->PinType),
				Other ? *Other->GetNodeTitle(ENodeTitleType::ListView).ToString() : TEXT("?"),
				*Linked->PinName.ToString(), *PinTypeName(Linked->PinType));
		}
	}
	for (const UEdGraphPin* Pin : Node.Pins)
	{
		if (Pin != nullptr && Pin->bOrphanedPin)
		{
			return FString::Printf(TEXT("it has an orphaned pin %s (a link of the old value type)"),
				*Pin->PinName.ToString());
		}
	}
	return FString();
}

bool IsAuthoredBlueprintNode(const UEdGraphNode& Node)
{
	const UPackage* Package = Node.GetOutermost();
	if (Package == nullptr || Package == GetTransientPackage() || Package->HasAnyFlags(RF_Transient)
		|| Package->GetName().StartsWith(TEXT("/Temp/")))
	{
		return false;
	}
	return Node.GetTypedOuter<UBlueprint>() != nullptr && Node.GetGraph() != nullptr;
}

// The loaded Blueprint input-action nodes of the action that IsAction accepts (the editor updates these
// nodes when an action's ValueType changes; it reads only loaded nodes too) that no longer fit Type.
TArray<FString> FindMismatchedNodes(TFunctionRef<bool(const UObject*)> IsAction, EInputActionValueType Type)
{
	TArray<FString> Found;
	for (const FActionNodeClass& NodeClass : ActionNodeClasses)
	{
		UClass* Class = FindObject<UClass>(nullptr, NodeClass.ClassPath);
		const FObjectPropertyBase* ActionProperty =
			Class ? CastField<FObjectPropertyBase>(Class->FindPropertyByName(TEXT("InputAction"))) : nullptr;
		if (ActionProperty == nullptr)
		{
			continue;
		}
		ForEachObjectOfClass(Class, [&](UObject* Object)
		{
			const UEdGraphNode* Node = Cast<UEdGraphNode>(Object);
			if (Node == nullptr || Object->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)
				|| !IsAuthoredBlueprintNode(*Node)
				|| !IsAction(ActionProperty->GetObjectPropertyValue_InContainer(Object)))
			{
				return;
			}
			const FString Problem = DescribeNodeProblem(*Node, NodeClass.ValuePin, Type);
			if (!Problem.IsEmpty())
			{
				Found.Add(FString::Printf(TEXT("%s (graph %s, node '%s'): %s"),
					*Node->GetTypedOuter<UBlueprint>()->GetPathName(), *Node->GetGraph()->GetName(),
					*Node->GetNodeTitle(ENodeTitleType::ListView).ToString(), *Problem));
			}
		});
	}
	Found.Sort();
	return Found;
}

FString JoinListed(const TArray<FString>& Items)
{
	TArray<FString> Shown(Items.GetData(), FMath::Min(Items.Num(), MaxListedNodes));
	FString Text = FString::Join(Shown, TEXT("; "));
	if (Items.Num() > MaxListedNodes)
	{
		Text += FString::Printf(TEXT("; +%d more"), Items.Num() - MaxListedNodes);
	}
	return Text;
}

FAssetDocumentRuntimeDiagnostic MakeWarning(const FString& Path, const TCHAR* Code, const FString& Message)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = Path;
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	Diagnostic.Severity = EAssetDocumentDiagnosticSeverity::Warning;
	return Diagnostic;
}

// A Swizzle that moves the X component away: a Boolean / Axis1D action reads only X, so it gets 0.
void CheckModifiers(
	const TArray<TObjectPtr<UInputModifier>>& Modifiers,
	const UInputAction& Action,
	const FString& ModifiersPath,
	const FString& Where,
	TArray<FAssetDocumentRuntimeDiagnostic>& Out)
{
	if (Action.ValueType != EInputActionValueType::Boolean && Action.ValueType != EInputActionValueType::Axis1D)
	{
		return;
	}
	for (int32 Index = 0; Index < Modifiers.Num(); ++Index)
	{
		const UInputModifierSwizzleAxis* Swizzle = Cast<UInputModifierSwizzleAxis>(Modifiers[Index].Get());
		if (Swizzle == nullptr || Swizzle->Order == EInputAxisSwizzle::XZY)
		{
			continue;
		}
		Out.Add(MakeWarning(FString::Printf(TEXT("%s/%d"), *ModifiersPath, Index),
			TEXT("EnhancedInput.ModifierDropsValue"),
			FString::Printf(TEXT("%s: the Swizzle modifier (Order %s) moves the input's X value to another axis, but "
				"%s is a %s action (its ValueType), which reads only X, so this input never reaches it. Remove the "
				"Swizzle (it is for 2D/3D actions) or change the action's ValueType."),
				*Where, *StaticEnum<EInputAxisSwizzle>()->GetNameStringByValue(static_cast<int64>(Swizzle->Order)),
				*Action.GetName(), *ValueTypeName(Action.ValueType))));
	}
}

FString CanonicalObjectPath(const FString& TargetPath)
{
	if (TargetPath.Contains(TEXT(".")))
	{
		return TargetPath;
	}
	return TargetPath + TEXT(".") + FPackageName::GetShortName(TargetPath);
}

void CheckInputAction(const UInputAction& Action, const FString& TargetPath, TArray<FAssetDocumentRuntimeDiagnostic>& Out)
{
	CheckModifiers(Action.Modifiers, Action, TEXT("/Properties/Modifiers"),
		FString::Printf(TEXT("Modifiers of %s"), *FPackageName::GetShortName(TargetPath)), Out);
	const FString ObjectPath = CanonicalObjectPath(TargetPath);
	const TArray<FString> Nodes = FindMismatchedNodes(
		[&ObjectPath](const UObject* Bound) { return Bound != nullptr && Bound->GetPathName() == ObjectPath; },
		Action.ValueType);
	if (!Nodes.IsEmpty())
	{
		Out.Add(MakeWarning(TEXT("/Properties/ValueType"), TEXT("EnhancedInput.BlueprintValueTypeMismatch"),
			FString::Printf(TEXT("%s is %s (its ValueType), but %d loaded Blueprint node(s) bind it with another value "
				"type: %s. The editor rebuilds these nodes for the new type and links of the old type break; update "
				"those graphs and compile them (or keep the ValueType)."),
				*FPackageName::GetShortName(TargetPath), *ValueTypeName(Action.ValueType), Nodes.Num(),
				*JoinListed(Nodes))));
	}
}

void CheckMappingContext(const UInputMappingContext& Context, TArray<FAssetDocumentRuntimeDiagnostic>& Out)
{
	const TArray<FEnhancedActionKeyMapping>& Mappings = Context.GetMappings();
	TSet<const UInputAction*> Checked;
	for (int32 Index = 0; Index < Mappings.Num(); ++Index)
	{
		const FEnhancedActionKeyMapping& Mapping = Mappings[Index];
		const FString MappingPath = FString::Printf(TEXT("/Properties/DefaultKeyMappings/Mappings/%d"), Index);
		const UInputAction* Action = Mapping.Action.Get();
		if (Action == nullptr)
		{
			Out.Add(MakeWarning(MappingPath + TEXT("/Action"), TEXT("EnhancedInput.MappingWithoutAction"),
				FString::Printf(TEXT("Mapping %d (%s) has no Action, so the key drives nothing."), Index,
					*Mapping.Key.ToString())));
			continue;
		}
		const FString Where = FString::Printf(TEXT("Mapping %d (%s -> %s)"), Index, *Mapping.Key.ToString(),
			*Action->GetName());
		CheckModifiers(Mapping.Modifiers, *Action, MappingPath + TEXT("/Modifiers"), Where, Out);
		if (Checked.Contains(Action))
		{
			continue;
		}
		Checked.Add(Action);
		const TArray<FString> Nodes = FindMismatchedNodes(
			[Action](const UObject* Bound) { return Bound == Action; }, Action->ValueType);
		if (!Nodes.IsEmpty())
		{
			Out.Add(MakeWarning(MappingPath + TEXT("/Action"), TEXT("EnhancedInput.BlueprintValueTypeMismatch"),
				FString::Printf(TEXT("%s: %s is %s (its ValueType), but %d loaded Blueprint node(s) using it no "
					"longer match: %s. Update those graphs for the action's value type and compile them."),
					*Where, *Action->GetName(), *ValueTypeName(Action->ValueType), Nodes.Num(), *JoinListed(Nodes))));
		}
	}
}

FAssetDocumentPrimitiveOutcome MakeFailure(const TCHAR* Path, const TCHAR* Code, const TCHAR* Message)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	FAssetDocumentRuntimeDiagnostic& Diagnostic = Outcome.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = Path;
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Outcome;
}

UObject* GetScratchObject(IAssetDocumentVerifiedHandleLease& Lease)
{
	using namespace UE::AssetDocument::EditorPrimitive;
	IAssetDocumentNativeHandlePayload& Payload = Lease.GetPayloadChecked();
	INativeIdentityPayload* Identity = TryGetNativeIdentityPayload(Payload, ScratchPayloadTypeId);
	FObjectIdentityView View;
	if (Identity == nullptr || !Payload.IsAlive() || !Identity->TryGetObjectIdentity(View))
	{
		return nullptr;
	}
	return View.Object;
}
} // namespace EditorEnhancedInputChecksLocal

TArray<FAssetDocumentRuntimeDiagnostic> CheckAssetDocumentEditorEnhancedInputDomain(
	const UObject& Asset,
	const FString& TargetPath)
{
	using namespace EditorEnhancedInputChecksLocal;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	if (const UInputAction* Action = Cast<UInputAction>(&Asset))
	{
		CheckInputAction(*Action, TargetPath, Diagnostics);
	}
	else if (const UInputMappingContext* Context = Cast<UInputMappingContext>(&Asset))
	{
		CheckMappingContext(*Context, Diagnostics);
	}
	return Diagnostics;
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorEnhancedInputCheckDomain(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace EditorEnhancedInputChecksLocal;
	(void)ActiveTransaction;
	const TArray<FAssetDocumentEditorValueFieldSpec> Fields = {
		{TEXT("target"), EAssetDocumentRuntimeValueKind::String},
	};
	TArray<const FAssetDocumentRuntimeValueDto*> Values;
	FString DiagnosticCode;
	if (!TryReadAssetDocumentEditorExactObject(Invocation.ValueArguments, Fields, Values, DiagnosticCode))
	{
		return MakeFailure(TEXT("/ValueArguments"), TEXT("PrimitiveInvocation.InvalidValueShape"),
			TEXT("The Enhanced Input check requires the exact target value argument."));
	}
	if (Invocation.HandleArguments.Num() != 1 || Invocation.HandleArguments[0].Name != TEXT("scratch"))
	{
		return MakeFailure(TEXT("/HandleArguments"), TEXT("PrimitiveInvocation.InvalidHandleArguments"),
			TEXT("The Enhanced Input check requires exactly the scratch handle."));
	}
	FAssetDocumentHandleValidationContext Context;
	Context.RequestId = Invocation.RequestId;
	Context.TransactionId = Invocation.TransactionId;
	Context.RuntimeRevision = Invocation.RuntimeRevision;
	Context.ProviderId = TEXT("editor");
	Context.Phase = Invocation.Phase;
	Context.RequiredKind = EAssetDocumentHandleKind::Scratch;
	Context.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
	FAssetDocumentVerifiedLeaseResult Lease =
		Handles.AcquireVerifiedLease(Invocation.HandleArguments[0].Handle, Context);
	if (!Lease.Lease || !Lease.Diagnostics.IsEmpty())
	{
		FAssetDocumentPrimitiveOutcome Outcome = MakeFailure(TEXT("/Handle"), TEXT("HandleMetadata.MissingLease"),
			TEXT("The scratch handle could not be leased."));
		if (!Lease.Diagnostics.IsEmpty())
		{
			Outcome.Diagnostics = MoveTemp(Lease.Diagnostics);
		}
		return Outcome;
	}
	const UObject* Scratch = GetScratchObject(*Lease.Lease);
	if (Scratch == nullptr)
	{
		return MakeFailure(TEXT("/Handle"), TEXT("HandleMetadata.InvalidPayload"),
			TEXT("The scratch handle does not name a live object."));
	}
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	Outcome.Diagnostics = CheckAssetDocumentEditorEnhancedInputDomain(*Scratch, Values[0]->StringValue);
	return Outcome;
}
