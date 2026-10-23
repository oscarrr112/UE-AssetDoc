// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Target lifecycle of Blueprint assets (catalog rows blueprint_exact.v1, widget_blueprint.v1 and
// anim_blueprint_exact.v1; the asset_class handle picks the row's exact Blueprint class):
//   editor.blueprint.scratch.clone.v1   Preflight  source_asset                                 -> scratch
//   editor.blueprint.scratch.create.v1  Preflight  asset_class, parent_class[, factory_context][, factory_class] -> scratch
//   editor.blueprint.asset.create.v1    Apply      asset_class, parent_class[, factory_context][, factory_class] -> asset_read, asset_mutable, package
// Rollback of a created Blueprint is the shared editor.asset.rollback_created.v1 seed.
//
// Creation is what Content Browser > Blueprint Class (User Interface > Widget Blueprint,
// Animation > Animation Blueprint) does: the asset class's factory (UBlueprintFactory,
// UWidgetBlueprintFactory, UAnimBlueprintFactory) with the chosen parent class through
// IAssetTools::CreateAsset. An Animation Blueprint also takes the target skeleton
// (factory_context; without it the Animation Blueprint is a template). The parent is
// validated first because the factories report an invalid parent with a modal dialog.
// An Anim Layer Interface (Content Browser > Animation > Animation Layer Interface,
// UAnimLayerInterfaceFactory) is an Animation Blueprint with parent class
// /Script/Engine.AnimLayerInterface: the same factory with BlueprintType = BPTYPE_Interface,
// no skeleton and no template flag.
// The other Blueprint types of /Script/Engine.Blueprint are the factory_class handle: Content
// Browser > Blueprint > Blueprint Macro Library (UBlueprintMacroFactory, parent picked like a
// Blueprint Class but no Level Script / AnimInstance / Interface child), Blueprint Interface
// (UBlueprintInterfaceFactory, parent UInterface) and Blueprint Function Library
// (UBlueprintFunctionLibraryFactory, parent UBlueprintFunctionLibrary); each factory keeps its
// own BlueprintType.
//
// A Blueprint edit cannot be previewed on a detached copy without compiling a
// duplicate class, so the Blueprint profile validates in preflight without a
// scratch target (its edits are covered by the whole-package snapshot). The
// scratch these primitives issue is therefore an inert transient
// UBlueprintFactory that records the parent class; nothing is ever written to it.

#include "Provider/Lifecycle/AssetDocumentEditorLifecycleHandlers.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/Skeleton.h"
#include "Engine/Blueprint.h"
#include "Engine/LevelScriptActor.h"
#include "UObject/Interface.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Factories/BlueprintFactory.h"
#include "UObject/UnrealType.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Provider/AssetDocumentEditorNativeIdentity.h"
#include "Provider/AssetDocumentEditorValueDto.h"
#include "Provider/Cutover/AssetDocumentEditorMutationFootprintBuilders.h"
#include "Provider/Lifecycle/AssetDocumentEditorEditorSafety.h"
#include "Provider/Lifecycle/AssetDocumentEditorLifecyclePayloads.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"

namespace EditorBlueprintLifecycleHandlersLocal
{
using namespace UE::AssetDocument::EditorPrimitive;

constexpr const TCHAR* BlueprintFactoryId = TEXT("blueprint_factory.v1");

FAssetDocumentPrimitiveOutcome Failure(const TCHAR* Code, const FString& Message,
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics = {})
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	if (!Diagnostics.IsEmpty())
	{
		Outcome.Diagnostics.Append(Diagnostics.GetData(), Diagnostics.Num());
		return Outcome;
	}
	FAssetDocumentRuntimeDiagnostic& Diagnostic = Outcome.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/Primitive");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Outcome;
}

FAssetDocumentRuntimeValueDto StringValue(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

const FAssetDocumentPrimitiveHandleArgument* FindArgument(const FAssetDocumentPrimitiveInvocation& Invocation, const TCHAR* Name)
{
	return Invocation.HandleArguments.FindByPredicate(
		[Name](const FAssetDocumentPrimitiveHandleArgument& Argument) { return Argument.Name == Name; });
}

FAssetDocumentVerifiedLeaseResult AcquireReadOnly(const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveHandleArgument& Argument, EAssetDocumentHandleKind Kind, IAssetDocumentHandleResolver& Handles)
{
	FAssetDocumentHandleValidationContext Context;
	Context.RequestId = Invocation.RequestId;
	Context.TransactionId = Invocation.TransactionId;
	Context.RuntimeRevision = Invocation.RuntimeRevision;
	Context.ProviderId = TEXT("editor");
	Context.Phase = Invocation.Phase;
	Context.RequiredKind = Kind;
	Context.RequiredAccess = EAssetDocumentHandleAccess::ReadOnly;
	return Handles.AcquireVerifiedLease(Argument.Handle, Context);
}

UClass* LeasedClass(IAssetDocumentVerifiedHandleLease& Lease)
{
	INativeIdentityPayload* Payload = TryGetNativeIdentityPayload(Lease.GetPayloadChecked(), ClassPayloadTypeId);
	FClassIdentityView Identity;
	return Payload != nullptr && Payload->TryGetClassIdentity(Identity) && Identity.Class != nullptr
			&& Identity.CanonicalClassPath == Identity.Class->GetPathName()
		? Identity.Class
		: nullptr;
}

UObject* LeasedAsset(IAssetDocumentVerifiedHandleLease& Lease)
{
	INativeIdentityPayload* Payload = TryGetNativeIdentityPayload(Lease.GetPayloadChecked(), AssetPayloadTypeId);
	if (Payload == nullptr)
	{
		Payload = TryGetNativeIdentityPayload(Lease.GetPayloadChecked(), CreatedAssetPayloadTypeId);
	}
	FObjectIdentityView Identity;
	return Payload != nullptr && Payload->TryGetObjectIdentity(Identity) && Identity.Object != nullptr
			&& Identity.CanonicalObjectPath == Identity.Object->GetPathName()
		? Identity.Object
		: nullptr;
}

// The New Asset factory of each Blueprint class this lifecycle creates. Each factory has a
// "ParentClass" class property and a "BlueprintType" byte property (set reflectively, so
// this module does not link the factory's editor module).
struct FBlueprintClassFactory
{
	const TCHAR* AssetClass;
	const TCHAR* Factory;
};
const FBlueprintClassFactory BlueprintClassFactories[] = {
	{TEXT("/Script/Engine.Blueprint"), TEXT("/Script/UnrealEd.BlueprintFactory")},
	{TEXT("/Script/UMGEditor.WidgetBlueprint"), TEXT("/Script/UMGEditor.WidgetBlueprintFactory")},
	{TEXT("/Script/Engine.AnimBlueprint"), TEXT("/Script/UnrealEd.AnimBlueprintFactory")},
};

// The Blueprint type factories of /Script/Engine.Blueprint (the factory_class handle).
struct FBlueprintTypeFactory
{
	const TCHAR* Factory;
	const TCHAR* RequiredParent;  // the only parent the factory accepts, or nullptr (picked parent)
};
const FBlueprintTypeFactory BlueprintTypeFactories[] = {
	{TEXT("/Script/UnrealEd.BlueprintMacroFactory"), nullptr},
	{TEXT("/Script/UnrealEd.BlueprintInterfaceFactory"), TEXT("/Script/CoreUObject.Interface")},
	{TEXT("/Script/UnrealEd.BlueprintFunctionLibraryFactory"), TEXT("/Script/Engine.BlueprintFunctionLibrary")},
};

const FBlueprintTypeFactory* FindTypeFactory(const UClass* Factory)
{
	for (const FBlueprintTypeFactory& Row : BlueprintTypeFactories)
	{
		if (Factory != nullptr && Factory->GetPathName() == Row.Factory)
		{
			return &Row;
		}
	}
	return nullptr;
}

UClass* FindFactoryClass(const UClass* AssetClass)
{
	for (const FBlueprintClassFactory& Row : BlueprintClassFactories)
	{
		if (AssetClass != nullptr && AssetClass->GetPathName() == Row.AssetClass)
		{
			UClass* Factory = FindObject<UClass>(nullptr, Row.Factory);
			return Factory != nullptr && Factory->IsChildOf(UFactory::StaticClass()) ? Factory : nullptr;
		}
	}
	return nullptr;
}

// FKismetEditorUtilities::CanCreateBlueprintOfClass plus the Blueprint type's own parent
// rules (UBlueprint::GetReparentingRules; a Widget Blueprint needs a UUserWidget parent).
bool IsAllowedParent(const UClass& AssetClass, const UClass* Parent)
{
	if (Parent == nullptr || !FKismetEditorUtilities::CanCreateBlueprintOfClass(Parent))
	{
		return false;
	}
	TSet<const UClass*> Allowed;
	TSet<const UClass*> Disallowed;
	CastChecked<UBlueprint>(AssetClass.GetDefaultObject())->GetReparentingRules(Allowed, Disallowed);
	const auto IsChildOfAny = [Parent](const TSet<const UClass*>& Classes)
	{
		return Classes.Array().ContainsByPredicate([Parent](const UClass* Class) { return Parent->IsChildOf(Class); });
	};
	return (Allowed.IsEmpty() || IsChildOfAny(Allowed)) && !IsChildOfAny(Disallowed);
}

bool HasNoValues(const FAssetDocumentRuntimeValueDto& Value)
{
	TArray<const FAssetDocumentRuntimeValueDto*> Values;
	FString Code;
	return TryReadAssetDocumentEditorExactObject(Value, {}, Values, Code);
}

struct FCreateClasses
{
	UClass* AssetClass = nullptr;
	UClass* Parent = nullptr;
	USkeleton* Skeleton = nullptr;
	UClass* TypeFactory = nullptr;  // factory_class (a Blueprint type factory), or nullptr
	bool bAnimLayerInterface = false;
};

const TCHAR* AnimLayerInterfaceClassPath = TEXT("/Script/Engine.AnimLayerInterface");

// asset_class must be a Blueprint class with a factory row and parent_class a class the editor
// offers as a parent of that Blueprint type; an Animation Blueprint may add factory_context (its
// target skeleton).
FAssetDocumentPrimitiveOutcome ReadCreateClasses(const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles, FCreateClasses& Out)
{
	const FAssetDocumentPrimitiveHandleArgument* AssetClassArgument = FindArgument(Invocation, TEXT("asset_class"));
	const FAssetDocumentPrimitiveHandleArgument* ParentArgument = FindArgument(Invocation, TEXT("parent_class"));
	const FAssetDocumentPrimitiveHandleArgument* ContextArgument = FindArgument(Invocation, TEXT("factory_context"));
	const FAssetDocumentPrimitiveHandleArgument* TypeFactoryArgument = FindArgument(Invocation, TEXT("factory_class"));
	const int32 Expected = 2 + (ContextArgument != nullptr ? 1 : 0) + (TypeFactoryArgument != nullptr ? 1 : 0);
	if (Invocation.HandleArguments.Num() != Expected || AssetClassArgument == nullptr || ParentArgument == nullptr)
	{
		return Failure(TEXT("BlueprintLifecycle.InvalidArguments"),
			TEXT("Blueprint creation requires exactly asset_class and parent_class handles (and optionally factory_context, the skeleton of an Animation Blueprint, or factory_class, the Blueprint type factory)."));
	}
	FAssetDocumentVerifiedLeaseResult AssetClassLease = AcquireReadOnly(Invocation, *AssetClassArgument, EAssetDocumentHandleKind::Class, Handles);
	FAssetDocumentVerifiedLeaseResult ParentLease = AcquireReadOnly(Invocation, *ParentArgument, EAssetDocumentHandleKind::Class, Handles);
	if (!AssetClassLease.Lease || !AssetClassLease.Diagnostics.IsEmpty())
	{
		return Failure(TEXT("BlueprintLifecycle.HandleRejected"), TEXT("The asset_class handle could not be leased."), AssetClassLease.Diagnostics);
	}
	if (!ParentLease.Lease || !ParentLease.Diagnostics.IsEmpty())
	{
		return Failure(TEXT("BlueprintLifecycle.HandleRejected"), TEXT("The parent_class handle could not be leased."), ParentLease.Diagnostics);
	}
	Out.AssetClass = LeasedClass(*AssetClassLease.Lease);
	if (FindFactoryClass(Out.AssetClass) == nullptr)
	{
		return Failure(TEXT("BlueprintLifecycle.TargetClassMismatch"),
			TEXT("This lifecycle creates exact /Script/Engine.Blueprint, /Script/UMGEditor.WidgetBlueprint and /Script/Engine.AnimBlueprint assets."));
	}
	const bool bAnim = Out.AssetClass == UAnimBlueprint::StaticClass();
	Out.Parent = LeasedClass(*ParentLease.Lease);
	if (TypeFactoryArgument != nullptr)
	{
		FAssetDocumentVerifiedLeaseResult FactoryLease = AcquireReadOnly(Invocation, *TypeFactoryArgument, EAssetDocumentHandleKind::Class, Handles);
		if (!FactoryLease.Lease || !FactoryLease.Diagnostics.IsEmpty())
		{
			return Failure(TEXT("BlueprintLifecycle.HandleRejected"), TEXT("The factory_class handle could not be leased."), FactoryLease.Diagnostics);
		}
		Out.TypeFactory = LeasedClass(*FactoryLease.Lease);
		const FBlueprintTypeFactory* Row = FindTypeFactory(Out.TypeFactory);
		if (Row == nullptr || Out.AssetClass != UBlueprint::StaticClass() || !Out.TypeFactory->IsChildOf(UBlueprintFactory::StaticClass()))
		{
			return Failure(TEXT("BlueprintLifecycle.InvalidFactory"),
				TEXT("factory_class is a Blueprint type factory of /Script/Engine.Blueprint: /Script/UnrealEd.BlueprintMacroFactory, /Script/UnrealEd.BlueprintInterfaceFactory or /Script/UnrealEd.BlueprintFunctionLibraryFactory."));
		}
		bool bParentAllowed = false;
		if (Out.Parent != nullptr && Row->RequiredParent != nullptr)
		{
			bParentAllowed = Out.Parent->GetPathName() == Row->RequiredParent;
		}
		else if (Out.Parent != nullptr)
		{
			// UBlueprintFactory::ConfigureProperties' class picker filter for a Macro Library.
			bParentAllowed = IsAllowedParent(*Out.AssetClass, Out.Parent) && !Out.Parent->HasAnyClassFlags(CLASS_Deprecated)
				&& !Out.Parent->IsChildOf(ALevelScriptActor::StaticClass()) && !Out.Parent->IsChildOf(UAnimInstance::StaticClass())
				&& !Out.Parent->IsChildOf(UInterface::StaticClass());
		}
		if (!bParentAllowed)
		{
			return Failure(TEXT("BlueprintLifecycle.InvalidParentClass"), FString::Printf(TEXT("%s cannot be the parent class of a Blueprint made by %s%s."),
				Out.Parent != nullptr ? *Out.Parent->GetPathName() : TEXT("The parent class"), *Out.TypeFactory->GetPathName(),
				Row->RequiredParent != nullptr ? *FString::Printf(TEXT(" (it must be %s)"), Row->RequiredParent) : TEXT("")));
		}
		if (ContextArgument != nullptr)
		{
			return Failure(TEXT("BlueprintLifecycle.InvalidArguments"), TEXT("Blueprint creation takes no factory_context."));
		}
		FAssetDocumentPrimitiveOutcome TypeOk;
		TypeOk.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
		return TypeOk;
	}
	Out.bAnimLayerInterface = bAnim && Out.Parent != nullptr && Out.Parent->GetPathName() == AnimLayerInterfaceClassPath;
	if (!Out.bAnimLayerInterface
		&& (!IsAllowedParent(*Out.AssetClass, Out.Parent) || (bAnim && !Out.Parent->IsChildOf(UAnimInstance::StaticClass()))))
	{
		return Failure(TEXT("BlueprintLifecycle.InvalidParentClass"), FString::Printf(TEXT("%s cannot be the parent class of %s."),
			Out.Parent != nullptr ? *Out.Parent->GetPathName() : TEXT("The parent class"),
			bAnim ? TEXT("an Animation Blueprint (it must be an AnimInstance class, or AnimLayerInterface for an Anim Layer Interface)")
				: TEXT("a Blueprint")));
	}
	if (Out.bAnimLayerInterface && ContextArgument != nullptr)
	{
		return Failure(TEXT("BlueprintLifecycle.InvalidArguments"), TEXT("An Anim Layer Interface has no target skeleton (factory_context)."));
	}
	// An Animation Blueprint without factory_context is a template (the factory dialog's "Template" option).
	if (!bAnim && ContextArgument != nullptr)
	{
		return Failure(TEXT("BlueprintLifecycle.InvalidArguments"), TEXT("Blueprint creation takes no factory_context."));
	}
	if (ContextArgument != nullptr)
	{
		FAssetDocumentVerifiedLeaseResult ContextLease = AcquireReadOnly(Invocation, *ContextArgument, EAssetDocumentHandleKind::Asset, Handles);
		if (!ContextLease.Lease || !ContextLease.Diagnostics.IsEmpty())
		{
			return Failure(TEXT("BlueprintLifecycle.HandleRejected"), TEXT("The factory_context handle could not be leased."), ContextLease.Diagnostics);
		}
		Out.Skeleton = Cast<USkeleton>(LeasedAsset(*ContextLease.Lease));
		if (Out.Skeleton == nullptr)
		{
			return Failure(TEXT("BlueprintLifecycle.InvalidSkeleton"), TEXT("The factory_context of an Animation Blueprint must be a Skeleton asset."));
		}
	}
	FAssetDocumentPrimitiveOutcome Ok;
	Ok.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	return Ok;
}

FAssetDocumentPrimitiveOutcome IssuePlaceholderScratch(UClass& Parent, IAssetDocumentHandleResolver& Handles)
{
	UBlueprintFactory* Placeholder = NewObject<UBlueprintFactory>(GetTransientPackage(), NAME_None, RF_Transient);
	Placeholder->ParentClass = &Parent;
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	const FAssetDocumentEditorValueField Fields[] = {{TEXT("parent_class"), StringValue(Parent.GetPathName())}};
	FString Code;
	if (!TryBuildAssetDocumentEditorValueObject(Fields, Outcome.Value, Code))
	{
		return Failure(TEXT("BlueprintLifecycle.InvalidOutcome"), TEXT("Scratch evidence could not be projected."));
	}
	FAssetDocumentIssuedHandleResult Issued = Handles.Issue(EAssetDocumentHandleKind::Scratch, EAssetDocumentHandleAccess::Mutable,
		MakeUnique<FAssetDocumentEditorScratchPayload>(*Placeholder));
	if (!Issued.Handle.IsSet() || !Issued.Diagnostics.IsEmpty())
	{
		return Failure(TEXT("BlueprintLifecycle.HandleIssueFailed"), TEXT("The request context rejected the scratch handle."), Issued.Diagnostics);
	}
	FAssetDocumentPrimitiveHandleResult& Result = Outcome.HandleResults.AddDefaulted_GetRef();
	Result.Name = TEXT("scratch");
	Result.Handles.Add(MoveTemp(Issued.Handle.GetValue()));
	return Outcome;
}

struct FCreateTarget
{
	FString PackagePath, AssetName, PackageName, CanonicalObjectPath, PackageFilename;
};

bool ReadCreateTarget(const FAssetDocumentRuntimeValueDto& Value, FCreateTarget& Out)
{
	const FAssetDocumentEditorValueFieldSpec Fields[] = {
		{TEXT("package_path"), EAssetDocumentRuntimeValueKind::String},
		{TEXT("asset_name"), EAssetDocumentRuntimeValueKind::String},
	};
	TArray<const FAssetDocumentRuntimeValueDto*> Values;
	FString Code;
	if (!TryReadAssetDocumentEditorExactObject(Value, Fields, Values, Code))
	{
		return false;
	}
	Out.PackagePath = Values[0]->StringValue;
	Out.AssetName = Values[1]->StringValue;
	Out.PackageName = Out.PackagePath + TEXT("/") + Out.AssetName;
	Out.CanonicalObjectPath = Out.PackageName + TEXT(".") + Out.AssetName;
	Out.PackageFilename = FPackageName::LongPackageNameToFilename(Out.PackageName, FPackageName::GetAssetPackageExtension());
	return FPackageName::IsValidLongPackageName(Out.PackagePath) && FPackageName::IsValidObjectPath(Out.CanonicalObjectPath)
		&& FSoftObjectPath(Out.CanonicalObjectPath).ToString() == Out.CanonicalObjectPath;
}
} // namespace

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorBlueprintScratchClone(
	const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction*)
{
	using namespace EditorBlueprintLifecycleHandlersLocal;
	const FAssetDocumentPrimitiveHandleArgument* Source = FindArgument(Invocation, TEXT("source_asset"));
	if (!HasNoValues(Invocation.ValueArguments) || Invocation.HandleArguments.Num() != 1 || Source == nullptr)
	{
		return Failure(TEXT("BlueprintLifecycle.InvalidArguments"), TEXT("Blueprint scratch clone requires exactly one source_asset handle."));
	}
	FAssetDocumentVerifiedLeaseResult Lease = AcquireReadOnly(Invocation, *Source, EAssetDocumentHandleKind::Asset, Handles);
	if (!Lease.Lease || !Lease.Diagnostics.IsEmpty())
	{
		return Failure(TEXT("BlueprintLifecycle.HandleRejected"), TEXT("The source_asset handle could not be leased."), Lease.Diagnostics);
	}
	UBlueprint* Blueprint = Cast<UBlueprint>(LeasedAsset(*Lease.Lease));
	if (Blueprint == nullptr || FindFactoryClass(Blueprint->GetClass()) == nullptr || Blueprint->ParentClass == nullptr)
	{
		return Failure(TEXT("BlueprintLifecycle.TargetClassMismatch"), TEXT("The source is not an exact UBlueprint, UWidgetBlueprint or UAnimBlueprint with a parent class."));
	}
	return IssuePlaceholderScratch(*Blueprint->ParentClass.Get(), Handles);
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorBlueprintScratchCreate(
	const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction*)
{
	using namespace EditorBlueprintLifecycleHandlersLocal;
	if (!HasNoValues(Invocation.ValueArguments))
	{
		return Failure(TEXT("BlueprintLifecycle.InvalidArguments"), TEXT("Blueprint scratch create takes no value arguments."));
	}
	FCreateClasses Classes;
	FAssetDocumentPrimitiveOutcome ClassRead = ReadCreateClasses(Invocation, Handles, Classes);
	if (ClassRead.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
	{
		return ClassRead;
	}
	return IssuePlaceholderScratch(*Classes.Parent, Handles);
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorBlueprintAssetCreate(
	const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction* Transaction)
{
	using namespace EditorBlueprintLifecycleHandlersLocal;
	FCreateTarget Target;
	if (!ReadCreateTarget(Invocation.ValueArguments, Target))
	{
		return Failure(TEXT("BlueprintLifecycle.InvalidArguments"), TEXT("Blueprint creation requires exact package_path and asset_name."));
	}
	if (Transaction == nullptr)
	{
		return Failure(TEXT("BlueprintLifecycle.TransactionRequired"), TEXT("Blueprint creation requires the active Apply transaction."));
	}
	FCreateClasses Classes;
	FAssetDocumentPrimitiveOutcome ClassRead = ReadCreateClasses(Invocation, Handles, Classes);
	if (ClassRead.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
	{
		return ClassRead;
	}
	UClass* Parent = Classes.Parent;
	UPackage* ExistingPackage = FindPackage(nullptr, *Target.PackageName);
	const bool bObjectExisted = FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(Target.CanonicalObjectPath)).IsValid();
	const bool bPackageExisted = ExistingPackage != nullptr || FPackageName::DoesPackageExist(Target.PackageName);
	const bool bFileExisted = IFileManager::Get().FileExists(*Target.PackageFilename);
	if (bObjectExisted || bPackageExisted || bFileExisted)
	{
		return Failure(TEXT("BlueprintLifecycle.TargetExists"), TEXT("The exact target already exists."));
	}
	FAssetDocumentEditorMutationIdentity Identity;
	if (!TryBuildAssetDocumentEditorMutationIdentity(Target.CanonicalObjectPath, Identity))
	{
		return Failure(TEXT("BlueprintLifecycle.InvalidTarget"), TEXT("The target identity is not canonical."));
	}
	const FAssetDocumentContractValidationResult Enlistment = Transaction->EnlistFootprint(BuildAssetDocumentEditorFactoryMutationFootprint(Identity));
	if (!Enlistment.bValid)
	{
		return Failure(TEXT("BlueprintLifecycle.FootprintRejected"), TEXT("The atomic transaction rejected the creation footprint."), Enlistment.Diagnostics);
	}
	// Content Browser > Blueprint Class / Widget Blueprint / Animation Blueprint: the class's
	// factory configured with the chosen parent (no class picker: IAssetTools::CreateAsset does
	// not ConfigureProperties).
	UClass* FactoryClass = Classes.TypeFactory != nullptr ? Classes.TypeFactory : FindFactoryClass(Classes.AssetClass);
	const TStrongObjectPtr<UFactory> Factory(NewObject<UFactory>(GetTransientPackage(), FactoryClass, NAME_None, RF_Transient));
	FClassProperty* ParentProperty = CastField<FClassProperty>(FactoryClass->FindPropertyByName(TEXT("ParentClass")));
	FByteProperty* TypeProperty = CastField<FByteProperty>(FactoryClass->FindPropertyByName(TEXT("BlueprintType")));
	if (ParentProperty == nullptr || TypeProperty == nullptr || (!Classes.bAnimLayerInterface && !Parent->IsChildOf(ParentProperty->MetaClass)))
	{
		return Failure(TEXT("BlueprintLifecycle.InvalidFactory"), FString::Printf(TEXT("%s cannot be configured with parent %s."),
			*FactoryClass->GetPathName(), *Parent->GetPathName()));
	}
	if (!Classes.bAnimLayerInterface)
	{
		ParentProperty->SetObjectPropertyValue_InContainer(Factory.Get(), Parent);
	}
	// UAnimLayerInterfaceFactory is UAnimBlueprintFactory with BlueprintType = BPTYPE_Interface (the
	// factory then uses UAnimLayerInterface as the parent). A Blueprint type factory keeps its own type.
	if (Classes.TypeFactory == nullptr)
	{
		TypeProperty->SetIntPropertyValue(TypeProperty->ContainerPtrToValuePtr<void>(Factory.Get()),
			static_cast<uint64>(Classes.bAnimLayerInterface ? BPTYPE_Interface : BPTYPE_Normal));
	}
	if (UAnimBlueprintFactory* AnimFactory = Cast<UAnimBlueprintFactory>(Factory.Get()))
	{
		// The target skeleton, or a template (the factory dialog's "Template" option) without one;
		// an Anim Layer Interface has neither.
		AnimFactory->TargetSkeleton = Classes.Skeleton;
		AnimFactory->bTemplate = Classes.Skeleton == nullptr && !Classes.bAnimLayerInterface;
	}
	else if (UBlueprintFactory* BlueprintFactory = Cast<UBlueprintFactory>(Factory.Get()))
	{
		BlueprintFactory->bSkipClassPicker = true;
	}
	UObject* Asset = nullptr;
	{
		// The target package is new (checked above), so a refused factory call
		// discards everything it created, the target package included.
		FAssetDocumentEditorNonInteractiveScope NonInteractive;
		Asset = IAssetTools::Get().CreateAsset(Target.AssetName, Target.PackagePath, Classes.AssetClass, Factory.Get(),
			TEXT("AssetDocumentEditorBlueprintLifecycle"));
		if (NonInteractive.WasInteractionRefused())
		{
			NonInteractive.DiscardCreatedPackages();
			return Failure(AssetDocumentEditorInteractiveFactoryCode,
				FAssetDocumentEditorNonInteractiveScope::DescribeRefusal(FactoryClass, Classes.AssetClass));
		}
	}
	UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
	if (Blueprint == nullptr || Blueprint->GetClass() != Classes.AssetClass || Blueprint->GetPathName() != Target.CanonicalObjectPath
		|| Blueprint->GetOutermost()->GetName() != Target.PackageName || Blueprint->ParentClass != Parent)
	{
		return Failure(TEXT("BlueprintLifecycle.IncompleteCreation"), FString::Printf(TEXT("%s did not create the exact target."), *FactoryClass->GetName()));
	}
	const FAssetData Registered = FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(Target.CanonicalObjectPath));
	if (!Registered.IsValid() || Registered.GetAsset() != Blueprint)
	{
		return Failure(TEXT("BlueprintLifecycle.RegistryMismatch"), TEXT("The created Blueprint was not registered exactly."));
	}

	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	const FAssetDocumentEditorValueField Fields[] = {
		{TEXT("canonical_path"), StringValue(Target.CanonicalObjectPath)},
		{TEXT("canonical_class"), StringValue(Classes.AssetClass->GetPathName())},
		{TEXT("parent_class"), StringValue(Parent->GetPathName())},
		{TEXT("package_name"), StringValue(Target.PackageName)},
		{TEXT("package_filename"), StringValue(Target.PackageFilename)},
	};
	FString Code;
	if (!TryBuildAssetDocumentEditorValueObject(Fields, Outcome.Value, Code))
	{
		return Failure(TEXT("BlueprintLifecycle.InvalidOutcome"), TEXT("Created-asset evidence could not be projected."));
	}
	const TSharedRef<const FAssetDocumentEditorCreationRecord> Record = MakeShared<FAssetDocumentEditorCreationRecord>(
		Invocation.RequestId, Invocation.TransactionId, Invocation.RuntimeRevision, BlueprintFactoryId,
		Target.CanonicalObjectPath, Target.PackageName, Target.PackageFilename,
		bObjectExisted, bPackageExisted, bFileExisted, ExistingPackage != nullptr && ExistingPackage->IsDirty(),
		*Blueprint, *Blueprint->GetOutermost());
	FAssetDocumentIssuedHandleResult Read = Handles.Issue(EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::ReadOnly,
		MakeUnique<FAssetDocumentEditorCreatedAssetPayload>(*Blueprint, Record));
	FAssetDocumentIssuedHandleResult Mutable = Handles.Issue(EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable,
		MakeUnique<FAssetDocumentEditorCreatedAssetPayload>(*Blueprint, Record));
	FAssetDocumentIssuedHandleResult Package = Handles.Issue(EAssetDocumentHandleKind::Package, EAssetDocumentHandleAccess::Mutable,
		MakeUnique<FAssetDocumentEditorCreatedPackagePayload>(*Blueprint->GetOutermost(), Record));
	for (FAssetDocumentIssuedHandleResult* Issued : {&Read, &Mutable, &Package})
	{
		if (!Issued->Handle.IsSet() || !Issued->Diagnostics.IsEmpty())
		{
			return Failure(TEXT("BlueprintLifecycle.HandleIssueFailed"), TEXT("The request context rejected a created identity."), Issued->Diagnostics);
		}
	}
	const TPair<const TCHAR*, FAssetDocumentIssuedHandleResult*> Results[] = {
		{TEXT("asset_read"), &Read}, {TEXT("asset_mutable"), &Mutable}, {TEXT("package"), &Package}};
	for (const auto& Entry : Results)
	{
		FAssetDocumentPrimitiveHandleResult& Result = Outcome.HandleResults.AddDefaulted_GetRef();
		Result.Name = Entry.Key;
		Result.Handles.Add(MoveTemp(Entry.Value->Handle.GetValue()));
	}
	return Outcome;
}
