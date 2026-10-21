// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Bindings/AssetDocumentUMGDesignerBindingHandlers.h"

#include "WidgetBlueprint.h"
#include "Provider/AssetDocumentUMGMutationContract.h"
#include "Provider/AssetDocumentUMGNativePayloads.h"
#include "Provider/AssetDocumentUMGValueDto.h"

namespace AssetDocumentUMGDesignerBindingHandlersPrivate
{
FAssetDocumentPrimitiveOutcome FailDesigner(
	const TCHAR* Code,
	const TCHAR* Message,
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics = {})
{
	return MakeAssetDocumentUMGFailure(Code, Message, Diagnostics);
}

void AddDiagnostic(
	TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics,
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentRuntimeDiagnostic& Diagnostic = Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/DesignerBinding");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
}

FAssetDocumentPrimitiveOutcome FailDesignerAfterMutation(
	IAssetDocumentAtomicTransaction& Transaction,
	const TCHAR* Message)
{
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	ValidateWidgetBlueprintMutationCompletion(false, Message, Diagnostics);
	RollbackAssetDocumentUMGMutation(
		Transaction, TEXT("AD.UMG.PROVIDER.POSTCONDITION_FAILED"), Diagnostics);
	return FailDesigner(TEXT("AD.UMG.PROVIDER.POSTCONDITION_FAILED"), Message, Diagnostics);
}

bool EnlistDesigner(
	IAssetDocumentAtomicTransaction& Transaction,
	UWidgetBlueprint& Blueprint,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	const TArray<UObject*> Touched;
	const FAssetDocumentContractValidationResult Result =
		EnlistWidgetBlueprintMutationFootprint(Transaction, Blueprint, Touched);
	if (Result.bValid)
	{
		return true;
	}
	OutFailure = FailDesigner(TEXT("AD.UMG.PROVIDER.INVALID_TRANSACTION"),
		TEXT("The UMG mutation footprint could not be enlisted."), Result.Diagnostics);
	return false;
}

FString BindingKey(const FDelegateEditorBinding& Binding)
{
	return Binding.ObjectName + TEXT("\x1f") + Binding.PropertyName.ToString();
}

FAssetDocumentRuntimeValueDto MakeBindingFact(const FDelegateEditorBinding& Binding)
{
	return MakeAssetDocumentUMGObject({
		{TEXT("widget_name"), MakeAssetDocumentUMGString(Binding.ObjectName)},
		{TEXT("property_name"), MakeAssetDocumentUMGString(Binding.PropertyName.ToString())},
		{TEXT("function_name"), MakeAssetDocumentUMGString(Binding.FunctionName.ToString())},
		{TEXT("source_path"), MakeAssetDocumentUMGString(Binding.SourcePath.GetDisplayText().ToString())},
		{TEXT("member_guid"), Binding.MemberGuid.IsValid()
			? MakeAssetDocumentUMGString(Binding.MemberGuid.ToString(EGuidFormats::DigitsWithHyphens))
			: MakeAssetDocumentUMGNull()},
	});
}

FString VariableOperationName(EAssetDocumentUMGVariableNotifyOperation Operation)
{
	switch (Operation)
	{
	case EAssetDocumentUMGVariableNotifyOperation::Add:
		return TEXT("add");
	case EAssetDocumentUMGVariableNotifyOperation::Rename:
		return TEXT("rename");
	case EAssetDocumentUMGVariableNotifyOperation::Remove:
		return TEXT("remove");
	}
	return TEXT("unknown");
}

bool IsDesignerMutationStateValid(IAssetDocumentAtomicTransaction& Transaction)
{
	return Transaction.GetState() == EAssetDocumentAtomicState::Mutating;
}
} // namespace AssetDocumentUMGDesignerBindingHandlersPrivate


FAssetDocumentPrimitiveOutcome InvokeDesignerBindingQuery(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction*)
{
	using namespace AssetDocumentUMGDesignerBindingHandlersPrivate;
	FAssetDocumentUMGDesignerBindingQueryInput Input;
	FString DecodeDiagnostic;
	if (!DecodeDesignerBindingQueryInput(Invocation.ValueArguments, Input, DecodeDiagnostic))
	{
		return FailDesigner(TEXT("AD.UMG.PROVIDER.INVALID_INVOCATION"), *DecodeDiagnostic);
	}
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	UWidgetBlueprint* Blueprint = ResolveWidgetBlueprint(Invocation, Handles,
		TEXT("widget_blueprint"), EAssetDocumentHandleAccess::ReadOnly, Diagnostics);
	if (Blueprint == nullptr)
	{
		return FailDesigner(TEXT("AD.UMG.PROVIDER.INVALID_HANDLE"),
			TEXT("The widget blueprint handle is invalid."), Diagnostics);
	}
	TSet<FString> Identities;
	TArray<FAssetDocumentRuntimeValueDto> Facts;
	for (const FDelegateEditorBinding& Binding : Blueprint->Bindings)
	{
		if ((Input.WidgetName.IsSet() && Binding.ObjectName != Input.WidgetName->ToString())
			|| (Input.PropertyName.IsSet() && Binding.PropertyName != Input.PropertyName.GetValue()))
		{
			continue;
		}
		const FString Identity = BindingKey(Binding);
		if (Identities.Contains(Identity))
		{
			return FailDesigner(TEXT("AD.UMG.DESIGNER_BINDING.DUPLICATE_IDENTITY"),
				TEXT("The requested binding evidence contains a duplicate widget/property identity."));
		}
		Identities.Add(Identity);
		Facts.Add(MakeBindingFact(Binding));
	}
	return MakeAssetDocumentUMGSuccess(MakeAssetDocumentUMGObject({
		{TEXT("bindings"), MakeAssetDocumentUMGArray(MoveTemp(Facts))},
	}));
}

FAssetDocumentPrimitiveOutcome InvokeVariableNotifyLifecycle(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AssetDocumentUMGDesignerBindingHandlersPrivate;
	FAssetDocumentUMGVariableNotifyInput Input;
	FString DecodeDiagnostic;
	if (!DecodeVariableNotifyInput(Invocation.ValueArguments, Input, DecodeDiagnostic))
	{
		return FailDesigner(TEXT("AD.UMG.PROVIDER.INVALID_INVOCATION"), *DecodeDiagnostic);
	}
	if (ActiveTransaction == nullptr)
	{
		return FailDesigner(TEXT("AD.UMG.PROVIDER.INVALID_TRANSACTION"),
			TEXT("A matching active transaction is required."));
	}
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	UWidgetBlueprint* Blueprint = ResolveWidgetBlueprint(Invocation, Handles,
		TEXT("widget_blueprint"), EAssetDocumentHandleAccess::Mutable, Diagnostics);
	if (Blueprint == nullptr)
	{
		return FailDesigner(TEXT("AD.UMG.PROVIDER.INVALID_HANDLE"),
			TEXT("The widget blueprint handle is invalid."), Diagnostics);
	}
	const FName ExistingName = Input.Operation == EAssetDocumentUMGVariableNotifyOperation::Rename
		? Input.OldName : Input.Name;
	const FName ResultName = Input.Operation == EAssetDocumentUMGVariableNotifyOperation::Rename
		? Input.NewName : Input.Name;
	const FGuid* ExistingGuid = Blueprint->WidgetVariableNameToGuidMap.Find(ExistingName);
	if (Input.Operation == EAssetDocumentUMGVariableNotifyOperation::Add
		&& ExistingGuid != nullptr)
	{
		return FailDesigner(TEXT("AD.UMG.VARIABLE.NAME_COLLISION"),
			TEXT("The requested variable name already has an Engine-observed GUID."));
	}
	if (Input.Operation != EAssetDocumentUMGVariableNotifyOperation::Add
		&& ExistingGuid == nullptr)
	{
		return FailDesigner(TEXT("AD.UMG.VARIABLE.NOT_FOUND"),
			TEXT("The requested existing variable name has no Engine-observed GUID."));
	}
	if (Input.Operation == EAssetDocumentUMGVariableNotifyOperation::Rename
		&& Blueprint->WidgetVariableNameToGuidMap.Contains(ResultName))
	{
		return FailDesigner(TEXT("AD.UMG.VARIABLE.NAME_COLLISION"),
			TEXT("The destination variable name already has an Engine-observed GUID."));
	}
	const FGuid ExpectedGuid = ExistingGuid != nullptr ? *ExistingGuid : FGuid();
	FAssetDocumentPrimitiveOutcome EnlistFailure;
	if (!EnlistDesigner(*ActiveTransaction, *Blueprint, EnlistFailure))
	{
		return EnlistFailure;
	}
	switch (Input.Operation)
	{
	case EAssetDocumentUMGVariableNotifyOperation::Add:
		Blueprint->OnVariableAdded(Input.Name);
		break;
	case EAssetDocumentUMGVariableNotifyOperation::Rename:
		Blueprint->OnVariableRenamed(Input.OldName, Input.NewName);
		break;
	case EAssetDocumentUMGVariableNotifyOperation::Remove:
		Blueprint->OnVariableRemoved(Input.Name);
		break;
	}
	if (!IsDesignerMutationStateValid(*ActiveTransaction))
	{
		return FailDesignerAfterMutation(*ActiveTransaction,
			TEXT("The active transaction became invalid after the variable lifecycle notification."));
	}
	const FGuid* ObservedGuid = Blueprint->WidgetVariableNameToGuidMap.Find(ResultName);
	const bool bPostcondition = Input.Operation == EAssetDocumentUMGVariableNotifyOperation::Add
		? ObservedGuid != nullptr && ObservedGuid->IsValid()
		: Input.Operation == EAssetDocumentUMGVariableNotifyOperation::Rename
			? ObservedGuid != nullptr && *ObservedGuid == ExpectedGuid
				&& !Blueprint->WidgetVariableNameToGuidMap.Contains(Input.OldName)
			: ObservedGuid == nullptr;
	if (!bPostcondition)
	{
		AddDiagnostic(Diagnostics, TEXT("AD.UMG.VARIABLE.OBSERVED_GUID_MISMATCH"),
			TEXT("The public variable lifecycle notification did not produce the expected observed GUID state."));
		RollbackAssetDocumentUMGMutation(*ActiveTransaction,
			TEXT("AD.UMG.VARIABLE.OBSERVED_GUID_MISMATCH"), Diagnostics);
		return FailDesigner(TEXT("AD.UMG.VARIABLE.OBSERVED_GUID_MISMATCH"),
			TEXT("The public variable lifecycle notification did not produce the expected observed GUID state."), Diagnostics);
	}
	FAssetDocumentPrimitiveOutcome Outcome = MakeAssetDocumentUMGSuccess(
		MakeAssetDocumentUMGObject({
			{TEXT("operation"), MakeAssetDocumentUMGString(VariableOperationName(Input.Operation))},
			{TEXT("observed_guid"), ObservedGuid != nullptr
				? MakeAssetDocumentUMGString(ObservedGuid->ToString(EGuidFormats::DigitsWithHyphens))
				: MakeAssetDocumentUMGNull()},
		}));
	Outcome.ChangedSet.Add(MakeAssetDocumentUMGChangedRecord(
		*Blueprint, TEXT("/WidgetVariables"), TEXT("lifecycle_notified")));
	return Outcome;
}

TConstArrayView<FAssetDocumentUMGPrimitiveBinding>
GetAssetDocumentUMGDesignerBindingPrimitiveBindings()
{
	static const FAssetDocumentUMGPrimitiveBinding Bindings[] = {
		{TEXT("umg.designer_binding.query.v1"), 1, &InvokeDesignerBindingQuery},
		{TEXT("umg.variable.notify_lifecycle.v1"), 1, &InvokeVariableNotifyLifecycle},
	};
	return Bindings;
}
