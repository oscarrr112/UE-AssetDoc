// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Blueprint member primitives (provider "blueprint", owner Blueprint). Each handler does
// what the Blueprint editor does for the same edit (My Blueprint panel,
// Components panel, Class Settings, Class Defaults); there is no inverse code:
// Core rolls back the whole package snapshot and the Blueprint derived-state
// rebuilder regenerates the class.
//
// DTOs (all carry "schema_version": 1):
//   blueprint.member.query.v1        {} -> {parent_class, generated_class, status, interfaces[], variables[],
//                                       components[], class_defaults{}, properties{}};
//                                    {defaults_of: [class paths]} -> {defaults_of: {path: {property: value}}}
//   blueprint.member.parent.set.v1   {parent_class}
//   blueprint.member.interface.add.v1 / .remove.v1 {interface}
//   blueprint.member.variable.set.v1 {name, type, category?, tooltip?, instance_editable?,
//                                     blueprint_read_only?, expose_on_spawn?, private?, default?}
//   blueprint.member.variable.remove.v1 {name}
//   blueprint.member.component.add.v1    {name, class, attach_to?, attach_socket?}
//   blueprint.member.component.attach.v1 {name, attach_to?, attach_socket?}
//   blueprint.member.component.remove.v1 {name, scope?: "owned"|"inherited"}
//   blueprint.member.property.set.v1 {target: "blueprint"|"class_defaults"|"component",
//                                     component?: {name, scope}, property, value? | reset: true}
//   blueprint.member.setting.set.v1  {name, value} -> {settings}: class-specific settings of a
//                                     registered IAssetDocumentBlueprintSettingsAdapter (query: "settings")
//   blueprint.member.dispatcher.* / blueprint.member.timeline.*: AssetDocumentBlueprintDispatcherPrimitives.cpp /
//                                     AssetDocumentBlueprintTimelinePrimitives.cpp (query: "dispatchers", "timelines";
//                                     "variables" lists no dispatcher)

#include "AssetDocumentGraphPrimitives.h"
#include "AssetDocumentGraphEditorConfirmations.h"
#include "AssetDocumentGraphPropertyCodec.h"
#include "Blueprint/AssetDocumentBlueprintMemberExtensions.h"
#include "AssetDocumentEditorPickerValues.h"

#include "BlueprintActionDatabase.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/StringOutputDevice.h"
#include "UObject/EnumProperty.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::Graph
{
namespace
{
constexpr uint32 MemberProviderVersion = 1;

// ---- descriptors --------------------------------------------------------------

FAssetDocumentPrimitiveDescriptor MakeMemberDescriptor(const TCHAR* PrimitiveId, bool bMutating, const TCHAR* AuthoritativeApi)
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = PrimitiveId;
	Descriptor.Version = MemberProviderVersion;
	Descriptor.ProviderId = BlueprintMemberProviderId;
	Descriptor.Owner = EAssetDocumentPrimitiveOwner::Blueprint;
	Descriptor.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
	Descriptor.Access = bMutating ? EAssetDocumentPrimitiveAccess::Mutating : EAssetDocumentPrimitiveAccess::ReadOnly;
	if (bMutating)
	{
		Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair};
		Descriptor.MutationFootprintSchemaId = TEXT("graph.footprint.whole-package.v1");
	}
	else
	{
		Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Validate, EAssetDocumentRuntimeRequestPhase::Preflight,
			EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair,
			EAssetDocumentRuntimeRequestPhase::Verify};
	}
	Descriptor.bRequiresActiveTransaction = bMutating;
	Descriptor.InputSchemaId = Descriptor.PrimitiveId + TEXT(".input.v1");
	Descriptor.OutputSchemaId = Descriptor.PrimitiveId + TEXT(".output.v1");
	Descriptor.StableDiagnosticCodes = {
		TEXT("AD.RUNTIME.PRIMITIVE_FAILED"), TEXT("Graph.UnknownField"), TEXT("Graph.InvalidArgument"),
		TEXT("Graph.UnsupportedSchemaVersion"), TEXT("Graph.PieActive"), TEXT("Graph.ResultTooLarge"),
		TEXT("BlueprintMember.UnsupportedAsset"), TEXT("BlueprintMember.ClassNotFound"), TEXT("BlueprintMember.BlueprintTypeUnsupported"),
		TEXT("BlueprintMember.InvalidParentClass"), TEXT("BlueprintMember.InvalidInterface"),
		TEXT("BlueprintMember.InvalidName"), TEXT("BlueprintMember.VariableNotFound"),
		TEXT("BlueprintMember.InvalidDefault"), TEXT("BlueprintMember.ComponentsUnsupported"),
		TEXT("BlueprintMember.ComponentNotFound"), TEXT("BlueprintMember.InvalidComponentClass"),
		TEXT("BlueprintMember.AttachParentNotFound"), TEXT("BlueprintMember.InvalidAttach"),
		TEXT("BlueprintMember.PropertyNotFound"), TEXT("BlueprintMember.PropertyNotEditable"),
		TEXT("BlueprintMember.InvalidValue"), TEXT("BlueprintMember.UseVariables"),
		TEXT("BlueprintMember.SettingNotFound"), TEXT("BlueprintMember.DispatchersUnsupported"),
		TEXT("BlueprintMember.DispatcherNotFound"), TEXT("BlueprintMember.TimelinesUnsupported"),
		TEXT("BlueprintMember.TimelineNotFound"), TEXT("BlueprintMember.InvalidTrack"), TEXT("Graph.UserPinsUnsupported")};
	Descriptor.RequiredConsumerIds = {TEXT("route.generic.v1")};
	Descriptor.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
	Descriptor.HandleArgumentSchemas.Add({TEXT("asset"), EAssetDocumentHandleKind::Asset,
		bMutating ? EAssetDocumentHandleAccess::Mutable : EAssetDocumentHandleAccess::ReadOnly, true});

	FAssetDocumentPrimitiveAdmissionEvidence& Evidence = Descriptor.Admission;
	Evidence.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
	Evidence.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper;
	Evidence.ExactEngineVersion = TEXT("5.7.4");
	Evidence.PhysicalOwner = EAssetDocumentPrimitiveOwner::Blueprint;
	Evidence.PublicModule = TEXT("AssetDocumentGraph");
	Evidence.PublicHeader = TEXT("AssetDocumentGraphProvider.h");
	Evidence.ExportedAuthoritativeApi = AuthoritativeApi;
	Evidence.AuthorityContextId = TEXT("assetdocument.editor.game-thread.editor-ready.v1");
	Evidence.PrimaryOperationId = Descriptor.PrimitiveId;
	Evidence.IdentityLifetimeContractId = TEXT("identity.request-transaction-revision-bound.v1");
	Evidence.TransactionContractId = TEXT("transaction.descriptor-governed.v1");
	Evidence.RollbackContractId = TEXT("rollback.core-package-snapshot.v1");
	Evidence.CompletionContractId = Descriptor.PrimitiveId + TEXT(".completion.v1");
	Evidence.SaveReloadEvidenceId = TEXT("UEAssetDoc.UBlueprint.Python");
	Evidence.FailureInjectionTestIds = {TEXT("UEAssetDoc.UBlueprint.Python")};
	Evidence.VerificationTestIds = {TEXT("UEAssetDoc.UBlueprint.Python")};
	Evidence.AuthorityFingerprint = FString::Printf(TEXT("ue-5.7.4|blueprint_member|%s|%s"), PrimitiveId, AuthoritativeApi);
	return Descriptor;
}

// ---- shared helpers -------------------------------------------------------------

bool MemberFail(FCall& Call, FStringView Code, FStringView Message, FStringView Path = TEXTVIEW("/"))
{
	if (Call.Error.Code.IsEmpty())
	{
		Call.Error.Set(Code, Message, Path);
	}
	return false;
}

UBlueprint* GetBlueprint(FCall& Call)
{
	// Any Blueprint asset class (UBlueprint, UWidgetBlueprint, UAnimBlueprint...): members are UBlueprint machinery;
	// the profile that drives these primitives decides which asset classes it accepts.
	UBlueprint* Blueprint = Cast<UBlueprint>(&Call.Asset);
	if (Blueprint == nullptr)
	{
		MemberFail(Call, TEXT("BlueprintMember.UnsupportedAsset"), TEXT("Blueprint member primitives edit Blueprint assets."),
			TEXT("/HandleArguments/asset"));
		return nullptr;
	}
	return Blueprint;
}

// "/Script/Engine.Actor", "/Game/BP_Base.BP_Base_C" or the Blueprint asset path "/Game/BP_Base".
UClass* ResolveClass(const FString& Path)
{
	if (Path.IsEmpty())
	{
		return nullptr;
	}
	if (UClass* Class = FindObject<UClass>(nullptr, *Path))
	{
		return Class;
	}
	UObject* Object = FSoftObjectPath(Path).TryLoad();
	if (UClass* Class = Cast<UClass>(Object))
	{
		return Class;
	}
	if (UBlueprint* Blueprint = Cast<UBlueprint>(Object))
	{
		return Blueprint->GeneratedClass;
	}
	if (!Path.Contains(TEXT(".")))
	{
		const FString Short = FPackageName::GetShortName(Path);
		return ResolveClass(Path + TEXT(".") + Short);
	}
	return nullptr;
}

bool ReadClass(FCall& Call, FStringView Key, UClass*& OutClass)
{
	FString Path;
	if (!Call.Args.String(Key, Path))
	{
		return false;
	}
	OutClass = ResolveClass(Path);
	if (OutClass == nullptr)
	{
		return Call.Args.Fail(TEXT("BlueprintMember.ClassNotFound"), FString::Printf(TEXT("Class '%s' does not resolve."), *Path), Key);
	}
	return true;
}

bool ReadName(FCall& Call, FStringView Key, FName& OutName)
{
	FString Text;
	if (!Call.Args.String(Key, Text))
	{
		return false;
	}
	if (Text.IsEmpty())
	{
		return Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("Name must not be empty."), Key);
	}
	OutName = FName(*Text);
	return true;
}

void CompileIfNeeded(UBlueprint& Blueprint)
{
	if (Blueprint.GeneratedClass != nullptr && Blueprint.GeneratedClass->GetDefaultObject(false) != nullptr
		&& (Blueprint.Status == BS_UpToDate || Blueprint.Status == BS_UpToDateWithWarnings || Blueprint.Status == BS_Error))
	{
		return;
	}
	FCompilerResultsLog Log;
	Log.bSilentMode = true;
	FKismetEditorUtilities::CompileBlueprint(&Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Log);
}

void NotifyStructural(UBlueprint& Blueprint)
{
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
	// New members must be visible to graph.node.spawn.v1 in the same apply (graph contract §8).
	FBlueprintActionDatabase::Get().RefreshAssetActions(&Blueprint);
}

bool ValidateNewName(FCall& Call, UBlueprint& Blueprint, FName Name, FStringView Key)
{
	FKismetNameValidator Validator(&Blueprint);
	const EValidatorResult Result = Validator.IsValid(Name.ToString());
	if (Result == EValidatorResult::Ok)
	{
		return true;
	}
	return Call.Args.Fail(TEXT("BlueprintMember.InvalidName"),
		INameValidatorInterface::GetErrorString(Name.ToString(), Result), Key);
}

// ---- reflected properties ---------------------------------------------------------

// A property holding instanced subobjects (an Instanced UPROPERTY, alone or in a container or struct):
// the Details panel's class picker makes each object inside the property's owner (a Mover component's
// movement modes). Such an object is a value: {"Class": <class path>, "Properties": {<changed editable
// properties>}}, or null.
bool HoldsInstances(const FProperty& Property)
{
	return Property.HasAnyPropertyFlags(CPF_InstancedReference | CPF_ContainsInstancedReference | CPF_PersistentInstance);
}

bool IsInstanceReference(const FProperty& Property)
{
	return Property.IsA<FObjectProperty>() && Property.HasAnyPropertyFlags(CPF_InstancedReference | CPF_PersistentInstance);
}

// Components are reached through their own component entries, not as instanced values.
bool ReferencesComponents(const FProperty& Property)
{
	const FProperty* Inner = &Property;
	if (const FArrayProperty* Array = CastField<FArrayProperty>(Inner))
	{
		Inner = Array->Inner;
	}
	else if (const FSetProperty* Set = CastField<FSetProperty>(Inner))
	{
		Inner = Set->ElementProp;
	}
	else if (const FMapProperty* Map = CastField<FMapProperty>(Inner))
	{
		Inner = Map->ValueProp;
	}
	const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(Inner);
	return Object != nullptr && Object->PropertyClass != nullptr && Object->PropertyClass->IsChildOf(UActorComponent::StaticClass());
}

// Properties a Details panel lets a user edit on a template/default object and that
// the typed codec round-trips.
bool IsEditableProperty(const FProperty& Property)
{
	return Property.HasAnyPropertyFlags(CPF_Edit)
		&& !Property.HasAnyPropertyFlags(CPF_EditConst | CPF_Transient | CPF_Deprecated | CPF_DisableEditOnTemplate)
		&& !(HoldsInstances(Property) && ReferencesComponents(Property));
}

constexpr int32 MaxInstanceDepth = 8;

FValue EncodeChangedProperties(UObject& Object, const UObject* Baseline, TFunctionRef<bool(const FProperty&)> Filter, int32 Depth = 0);

bool IsKeyedByText(const FProperty& KeyProperty)
{
	return KeyProperty.IsA<FNameProperty>() || KeyProperty.IsA<FStrProperty>();
}

// EncodePropertyValue, with instanced subobjects as {"Class", "Properties"} values. A map of them keyed
// by names or strings is an object; other maps are arrays of {"Key", "Value"}.
FValue EncodeValueWithInstances(const FProperty& Property, const void* ValuePtr, int32 Depth)
{
	if (!HoldsInstances(Property) || Property.ArrayDim > 1 || Depth > MaxInstanceDepth)
	{
		return EncodePropertyValue(Property, ValuePtr);
	}
	if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(&Property))
	{
		UObject* Instance = Object->GetObjectPropertyValue(ValuePtr);
		if (Instance == nullptr || !IsInstanceReference(Property))
		{
			return EncodePropertyValue(Property, ValuePtr);
		}
		return FObjectBuilder()
			.AddString(TEXT("Class"), ObjectPathOrEmpty(Instance->GetClass()))
			.Add(TEXT("Properties"), EncodeChangedProperties(*Instance, Instance->GetClass()->GetDefaultObject(),
				[](const FProperty&) { return true; }, Depth + 1))
			.Build();
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		FScriptArrayHelper Helper(Array, ValuePtr);
		TArray<FValue> Elements;
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			Elements.Add(EncodeValueWithInstances(*Array->Inner, Helper.GetRawPtr(Index), Depth));
		}
		return MakeArray(MoveTemp(Elements));
	}
	if (const FSetProperty* Set = CastField<FSetProperty>(&Property))
	{
		FScriptSetHelper Helper(Set, ValuePtr);
		TArray<FValue> Elements;
		for (FScriptSetHelper::FIterator It(Helper); It; ++It)
		{
			Elements.Add(EncodeValueWithInstances(*Set->ElementProp, Helper.GetElementPtr(It), Depth));
		}
		return MakeArray(MoveTemp(Elements));
	}
	if (const FMapProperty* Map = CastField<FMapProperty>(&Property))
	{
		FScriptMapHelper Helper(Map, ValuePtr);
		if (IsKeyedByText(*Map->KeyProp))
		{
			FObjectBuilder Pairs;
			for (FScriptMapHelper::FIterator It(Helper); It; ++It)
			{
				FValue Key = EncodePropertyValue(*Map->KeyProp, Helper.GetKeyPtr(It));
				Pairs.Add(Key.StringValue, EncodeValueWithInstances(*Map->ValueProp, Helper.GetValuePtr(It), Depth));
			}
			return Pairs.Build();
		}
		TArray<FValue> Pairs;
		for (FScriptMapHelper::FIterator It(Helper); It; ++It)
		{
			Pairs.Add(FObjectBuilder()
				.Add(TEXT("Key"), EncodePropertyValue(*Map->KeyProp, Helper.GetKeyPtr(It)))
				.Add(TEXT("Value"), EncodeValueWithInstances(*Map->ValueProp, Helper.GetValuePtr(It), Depth))
				.Build());
		}
		return MakeArray(MoveTemp(Pairs));
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		FObjectBuilder Fields;
		for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
		{
			Fields.Add(It->GetName(), EncodeValueWithInstances(**It, It->ContainerPtrToValuePtr<void>(ValuePtr), Depth));
		}
		return Fields.Build();
	}
	return EncodePropertyValue(Property, ValuePtr);
}

// Typed values of editable properties of Object that differ from Baseline.
FValue EncodeChangedProperties(UObject& Object, const UObject* Baseline, TFunctionRef<bool(const FProperty&)> Filter, int32 Depth)
{
	FObjectBuilder Builder;
	for (TFieldIterator<FProperty> It(Object.GetClass()); It; ++It)
	{
		const FProperty& Property = **It;
		if (!IsEditableProperty(Property) || !Filter(Property))
		{
			continue;
		}
		const bool bBaselineHas = Baseline != nullptr && Baseline->GetClass()->IsChildOf(Property.GetOwnerClass());
		if (HoldsInstances(Property))
		{
			// Instances differ by identity: compare what they hold.
			FValue Value = EncodeValueWithInstances(Property, Property.ContainerPtrToValuePtr<void>(&Object), Depth);
			if (bBaselineHas && ValueToJsonString(Value, false) == ValueToJsonString(
				EncodeValueWithInstances(Property, Property.ContainerPtrToValuePtr<void>(Baseline), Depth), false))
			{
				continue;
			}
			Builder.Add(Property.GetName(), MoveTemp(Value));
			continue;
		}
		if (bBaselineHas && Property.Identical_InContainer(&Object, Baseline, 0, PPF_None))
		{
			continue;
		}
		Builder.Add(Property.GetName(), EncodePropertyValue(Property, Property.ContainerPtrToValuePtr<void>(&Object)));
	}
	return Builder.Build();
}

// value: typed JSON (codec) or, for non-string properties, UE import text.
bool DecodeValue(const FProperty& Property, void* ValuePtr, const FValue& Value, FString& OutError)
{
	const bool bTextual = Property.IsA<FStrProperty>() || Property.IsA<FNameProperty>() || Property.IsA<FTextProperty>()
		|| Property.IsA<FEnumProperty>() || Property.IsA<FObjectPropertyBase>() || Property.IsA<FSoftObjectProperty>()
		|| (Property.IsA<FByteProperty>() && CastField<FByteProperty>(&Property)->Enum != nullptr);
	if (Value.Kind == EAssetDocumentRuntimeValueKind::String && !bTextual)
	{
		// Numeric ImportText is lenient ("abc" -> 0); the Details panel rejects non-numbers.
		const bool bBadNumber = Property.IsA<FNumericProperty>() && !Value.StringValue.TrimStartAndEnd().IsNumeric();
		if (bBadNumber || Property.ImportText_Direct(*Value.StringValue, ValuePtr, nullptr, PPF_None) == nullptr)
		{
			OutError = FString::Printf(TEXT("'%s' is not a valid value for %s."), *Value.StringValue, *Property.GetName());
			return false;
		}
		return true;
	}
	return DecodePropertyValue(Property, ValuePtr, Value, OutError);
}

bool DecodeValueWithInstances(const FProperty& Property, void* ValuePtr, const FValue& Value, UObject& Outer, FString& OutError, int32 Depth = 0);

// Copies Source's value into Dest; instanced subobjects are copied into Outer (as the Details panel's
// reset to default instances the default's objects) rather than shared with Source's owner.
bool CopyValueWithInstances(const FProperty& Property, void* Dest, const void* Source, UObject& Outer, FString& OutError, int32 Depth = 0)
{
	if (!HoldsInstances(Property) || Property.ArrayDim > 1 || Depth > MaxInstanceDepth)
	{
		Property.CopyCompleteValue(Dest, Source);
		return true;
	}
	return DecodeValueWithInstances(Property, Dest, EncodeValueWithInstances(Property, Source, Depth), Outer, OutError, Depth);
}

// Sets an instance's properties as the document states them; the ones it omits are the class's defaults.
bool DecodeInstanceProperties(UObject& Instance, const FValue& Properties, FString& OutError, int32 Depth)
{
	const UObject* Defaults = Instance.GetClass()->GetDefaultObject();
	for (const FString& Key : Properties.ObjectKeys)
	{
		const FProperty* Property = Instance.GetClass()->FindPropertyByName(FName(*Key));
		if (Property == nullptr || !IsEditableProperty(*Property))
		{
			OutError = FString::Printf(TEXT("%s has no editable property '%s'."), *Instance.GetClass()->GetName(), *Key);
			return false;
		}
	}
	for (TFieldIterator<FProperty> It(Instance.GetClass()); It; ++It)
	{
		const FProperty& Property = **It;
		if (!IsEditableProperty(Property))
		{
			continue;
		}
		void* PropertyPtr = Property.ContainerPtrToValuePtr<void>(&Instance);
		const FValue* Wanted = FindField(Properties, Property.GetName());
		if (Wanted == nullptr)
		{
			if (Defaults != &Instance && !CopyValueWithInstances(Property, PropertyPtr, Property.ContainerPtrToValuePtr<void>(Defaults),
				Instance, OutError, Depth + 1))
			{
				return false;
			}
			continue;
		}
		if (!DecodeValueWithInstances(Property, PropertyPtr, *Wanted, Instance, OutError, Depth + 1))
		{
			OutError = FString::Printf(TEXT("%s.%s: %s"), *Instance.GetClass()->GetName(), *Property.GetName(), *OutError);
			return false;
		}
	}
	return true;
}

// One instanced subobject value: reuses the object there when it has the stated class, else makes one
// inside Outer as the Details panel's class picker does (SPropertyEditorEditInline).
bool DecodeInstance(const FObjectPropertyBase& Property, void* ValuePtr, const FValue& Value, UObject& Outer, FString& OutError, int32 Depth)
{
	if (Value.Kind == EAssetDocumentRuntimeValueKind::Null)
	{
		Property.SetObjectPropertyValue(ValuePtr, nullptr);
		return true;
	}
	const FValue* ClassValue = Value.Kind == EAssetDocumentRuntimeValueKind::Object ? FindField(Value, TEXT("Class")) : nullptr;
	const FValue* Properties = Value.Kind == EAssetDocumentRuntimeValueKind::Object ? FindField(Value, TEXT("Properties")) : nullptr;
	const bool bOnlyKnown = Value.Kind == EAssetDocumentRuntimeValueKind::Object && !Value.ObjectKeys.ContainsByPredicate(
		[](const FString& Key) { return Key != TEXT("Class") && Key != TEXT("Properties"); });
	if (ClassValue == nullptr || ClassValue->Kind != EAssetDocumentRuntimeValueKind::String || !bOnlyKnown
		|| (Properties != nullptr && Properties->Kind != EAssetDocumentRuntimeValueKind::Object))
	{
		OutError = FString::Printf(TEXT("%s holds an instanced object: {\"Class\": <class path>, \"Properties\": {...}} or null."),
			*Property.GetName());
		return false;
	}
	UClass* Class = FindObject<UClass>(nullptr, *ClassValue->StringValue);
	if (Class == nullptr)
	{
		Class = Cast<UClass>(FSoftObjectPath(ClassValue->StringValue).TryLoad());
	}
	if (Class == nullptr || Property.PropertyClass == nullptr || !Class->IsChildOf(Property.PropertyClass)
		|| Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
	{
		OutError = FString::Printf(TEXT("'%s' is not a concrete %s class."), *ClassValue->StringValue,
			Property.PropertyClass != nullptr ? *Property.PropertyClass->GetName() : TEXT("UObject"));
		return false;
	}
	UObject* Instance = Property.GetObjectPropertyValue(ValuePtr);
	if (Instance == nullptr || Instance->GetClass() != Class || Instance->GetOuter() != &Outer)
	{
		EObjectFlags Flags = Outer.GetMaskedFlags(RF_PropagateToSubObjects) | RF_Transactional;
		if (Outer.HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
		{
			Flags |= RF_ArchetypeObject;
		}
		Instance = NewObject<UObject>(&Outer, Class, NAME_None, Flags);
	}
	else
	{
		Instance->Modify();
	}
	static const FValue NoProperties = MakeEmptyObject();
	if (!DecodeInstanceProperties(*Instance, Properties != nullptr ? *Properties : NoProperties, OutError, Depth))
	{
		return false;
	}
	Property.SetObjectPropertyValue(ValuePtr, Instance);
	return true;
}

// DecodeValue, with instanced subobjects as {"Class", "Properties"} values (EncodeValueWithInstances).
bool DecodeValueWithInstances(const FProperty& Property, void* ValuePtr, const FValue& Value, UObject& Outer, FString& OutError, int32 Depth)
{
	if (!HoldsInstances(Property) || Property.ArrayDim > 1 || Depth > MaxInstanceDepth)
	{
		return DecodeValue(Property, ValuePtr, Value, OutError);
	}
	if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(&Property))
	{
		return IsInstanceReference(Property) ? DecodeInstance(*Object, ValuePtr, Value, Outer, OutError, Depth)
			: DecodeValue(Property, ValuePtr, Value, OutError);
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		if (Value.Kind != EAssetDocumentRuntimeValueKind::Array)
		{
			OutError = FString::Printf(TEXT("%s expects an array."), *Property.GetName());
			return false;
		}
		FScriptArrayHelper Helper(Array, ValuePtr);
		Helper.Resize(Value.Elements.Num());
		for (int32 Index = 0; Index < Value.Elements.Num(); ++Index)
		{
			if (!DecodeValueWithInstances(*Array->Inner, Helper.GetRawPtr(Index), Value.Elements[Index], Outer, OutError, Depth))
			{
				return false;
			}
		}
		return true;
	}
	if (const FSetProperty* Set = CastField<FSetProperty>(&Property))
	{
		if (Value.Kind != EAssetDocumentRuntimeValueKind::Array)
		{
			OutError = FString::Printf(TEXT("%s expects an array."), *Property.GetName());
			return false;
		}
		FScriptSetHelper Helper(Set, ValuePtr);
		Helper.EmptyElements(Value.Elements.Num());
		for (const FValue& Element : Value.Elements)
		{
			const int32 Index = Helper.AddDefaultValue_Invalid_NeedsRehash();
			if (!DecodeValueWithInstances(*Set->ElementProp, Helper.GetElementPtr(Index), Element, Outer, OutError, Depth))
			{
				Helper.Rehash();
				return false;
			}
		}
		Helper.Rehash();
		return true;
	}
	if (const FMapProperty* Map = CastField<FMapProperty>(&Property))
	{
		const bool bByText = IsKeyedByText(*Map->KeyProp);
		if (Value.Kind != (bByText ? EAssetDocumentRuntimeValueKind::Object : EAssetDocumentRuntimeValueKind::Array))
		{
			OutError = Property.GetName() + (bByText ? TEXT(" expects an object keyed by the map's keys.")
				: TEXT(" expects an array of {\"Key\", \"Value\"}."));
			return false;
		}
		FScriptMapHelper Helper(Map, ValuePtr);
		Helper.EmptyValues(bByText ? Value.ObjectKeys.Num() : Value.Elements.Num());
		const int32 Count = bByText ? Value.ObjectKeys.Num() : Value.Elements.Num();
		for (int32 Pair = 0; Pair < Count; ++Pair)
		{
			const FValue KeyText = bByText ? MakeString(Value.ObjectKeys[Pair]) : FValue();
			const FValue* Key = bByText ? &KeyText : FindField(Value.Elements[Pair], TEXT("Key"));
			const FValue* Item = bByText ? &Value.Elements[Pair] : FindField(Value.Elements[Pair], TEXT("Value"));
			if (Key == nullptr || Item == nullptr)
			{
				Helper.Rehash();
				OutError = FString::Printf(TEXT("%s expects an array of {\"Key\", \"Value\"}."), *Property.GetName());
				return false;
			}
			const int32 Index = Helper.AddDefaultValue_Invalid_NeedsRehash();
			if (!DecodeValue(*Map->KeyProp, Helper.GetKeyPtr(Index), *Key, OutError)
				|| !DecodeValueWithInstances(*Map->ValueProp, Helper.GetValuePtr(Index), *Item, Outer, OutError, Depth))
			{
				Helper.Rehash();
				return false;
			}
		}
		Helper.Rehash();
		return true;
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		if (Value.Kind != EAssetDocumentRuntimeValueKind::Object)
		{
			OutError = FString::Printf(TEXT("%s expects an object of struct fields."), *Property.GetName());
			return false;
		}
		for (int32 Index = 0; Index < Value.ObjectKeys.Num(); ++Index)
		{
			const FProperty* Field = Struct->Struct->FindPropertyByName(FName(*Value.ObjectKeys[Index]));
			if (Field == nullptr)
			{
				OutError = FString::Printf(TEXT("Struct %s has no field '%s'."), *Struct->Struct->GetName(), *Value.ObjectKeys[Index]);
				return false;
			}
			if (!DecodeValueWithInstances(*Field, Field->ContainerPtrToValuePtr<void>(ValuePtr), Value.Elements[Index], Outer, OutError, Depth))
			{
				return false;
			}
		}
		return true;
	}
	return DecodeValue(Property, ValuePtr, Value, OutError);
}

// Sets (or resets to Baseline) one property path of Target the way the Details panel does.
bool SetObjectProperty(FCall& Call, UObject& Target, const UObject* Baseline, const FString& Path, const FValue* Value,
	bool bReset, TFunctionRef<bool(const FProperty&)> Filter, FObjectBuilder& Result)
{
	FResolvedPropertyPath Resolved;
	FString Error;
	if (!ResolvePropertyPath(Target, Path, Resolved, Error))
	{
		return Call.Args.Fail(TEXT("BlueprintMember.PropertyNotFound"), Error, TEXT("property"));
	}
	if (!IsEditableProperty(*Resolved.TopLevel) || !Filter(*Resolved.TopLevel))
	{
		return Call.Args.Fail(TEXT("BlueprintMember.PropertyNotEditable"),
			FString::Printf(TEXT("%s is not an editable default of %s."), *Path, *Target.GetClass()->GetName()), TEXT("property"));
	}
	Target.PreEditChange(Resolved.TopLevel);
	bool bOk = true;
	if (bReset)
	{
		FResolvedPropertyPath BaselineResolved;
		UObject* BaselineObject = const_cast<UObject*>(Baseline);
		if (BaselineObject != nullptr && ResolvePropertyPath(*BaselineObject, Path, BaselineResolved, Error)
			&& BaselineResolved.Leaf == Resolved.Leaf)
		{
			bOk = CopyValueWithInstances(*Resolved.Leaf, Resolved.LeafPtr, BaselineResolved.LeafPtr, Target, Error)
				|| Call.Args.Fail(TEXT("BlueprintMember.InvalidValue"), Error, TEXT("property"));
		}
		else
		{
			Resolved.Leaf->InitializeValue(Resolved.LeafPtr);
		}
	}
	else
	{
		// The row handles and keys the property holds now are kept; a written one must be offered by the Details
		// panel's pickers.
		const TArray<FString> Kept = EditorPickerValues::CollectPicks(*Resolved.Leaf, Resolved.LeafPtr);
		EditorPickerValues::FInvalidPick Invalid;
		if (Value == nullptr || !DecodeValueWithInstances(*Resolved.Leaf, Resolved.LeafPtr, *Value, Target, Error))
		{
			bOk = Call.Args.Fail(TEXT("BlueprintMember.InvalidValue"), Error.IsEmpty() ? FString(TEXT("A value is required.")) : Error, TEXT("value"));
		}
		else if (EditorPickerValues::FindInvalidPick(*Resolved.Leaf, Resolved.LeafPtr, Kept, Invalid))
		{
			bOk = MemberFail(Call, *Invalid.Code, FString::Printf(TEXT("'%s'%s: %s"), *Path, *Invalid.ValuePath, *Invalid.Message),
				*(TEXT("/value") + Invalid.ValuePath));
		}
	}
	FPropertyChangedEvent Event(Resolved.TopLevel, EPropertyChangeType::ValueSet);
	Target.PostEditChangeProperty(Event);
	if (bOk)
	{
		Result.Add(TEXT("value"), EncodeValueWithInstances(*Resolved.Leaf, Resolved.LeafPtr, 0));
	}
	return bOk;
}

// ---- components -----------------------------------------------------------------------

UObject* ParentDefaults(UBlueprint& Blueprint)
{
	return Blueprint.ParentClass != nullptr ? Blueprint.ParentClass->GetDefaultObject() : nullptr;
}

// Native component of the parent class CDO by property name ("CharacterMovement") or subobject name ("CharMoveComp").
UActorComponent* FindNativeComponent(UObject* Defaults, FName Name, FName* OutPropertyName = nullptr)
{
	if (Defaults == nullptr)
	{
		return nullptr;
	}
	if (FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(Defaults->GetClass(), Name))
	{
		if (UActorComponent* Component = Cast<UActorComponent>(Property->GetObjectPropertyValue_InContainer(Defaults)))
		{
			if (OutPropertyName)
			{
				*OutPropertyName = Property->GetFName();
			}
			return Component;
		}
	}
	UActorComponent* Component = Cast<UActorComponent>(Defaults->GetDefaultSubobjectByName(Name));
	if (Component != nullptr && OutPropertyName)
	{
		*OutPropertyName = Component->GetFName();
	}
	return Component;
}

// The user-facing name of a native component: its member property, else its subobject name.
FName NativeComponentAlias(UObject& Defaults, const UActorComponent& Component)
{
	for (TFieldIterator<FObjectPropertyBase> It(Defaults.GetClass()); It; ++It)
	{
		if (It->GetObjectPropertyValue_InContainer(&Defaults) == &Component)
		{
			return It->GetFName();
		}
	}
	return Component.GetFName();
}

// SCS node of a parent Blueprint class (inherited component).
USCS_Node* FindInheritedNode(UBlueprint& Blueprint, FName Name)
{
	for (UClass* Class = Blueprint.ParentClass; Class != nullptr; Class = Class->GetSuperClass())
	{
		UBlueprintGeneratedClass* Generated = Cast<UBlueprintGeneratedClass>(Class);
		if (Generated != nullptr && Generated->SimpleConstructionScript != nullptr)
		{
			if (USCS_Node* Node = Generated->SimpleConstructionScript->FindSCSNode(Name))
			{
				return Node;
			}
		}
	}
	return nullptr;
}

USimpleConstructionScript* GetSCS(FCall& Call, UBlueprint& Blueprint)
{
	if (Blueprint.SimpleConstructionScript == nullptr)
	{
		MemberFail(Call, TEXT("BlueprintMember.ComponentsUnsupported"), TEXT("Only Actor Blueprints have components."), TEXT("/"));
	}
	return Blueprint.SimpleConstructionScript;
}

bool IsDescendant(USCS_Node* Node, USCS_Node* Candidate)
{
	for (USCS_Node* Child : Node->GetChildNodes())
	{
		if (Child == Candidate || IsDescendant(Child, Candidate))
		{
			return true;
		}
	}
	return false;
}

// Attaches a node that is currently not part of the tree (SSCSEditor semantics).
bool AttachNode(FCall& Call, UBlueprint& Blueprint, USimpleConstructionScript& SCS, USCS_Node& Node, const FString& AttachTo)
{
	const bool bScene = Node.ComponentTemplate != nullptr && Node.ComponentTemplate->IsA<USceneComponent>();
	if (AttachTo.IsEmpty())
	{
		// An SCS has one scene root: the Components panel's Add with no parent puts a scene component under the
		// owned scene root (SubobjectDataSubsystem attaches to the scene root), and AddNode's ValidateSceneRootNodes
		// lets the first one replace the DefaultSceneRoot. A second parentless root node would be reparented under
		// the root on load (FixupOrphanedNodes). A native or inherited root is not one of this SCS's root nodes:
		// the node stays a root node and attaches to it at construction.
		USCS_Node* Root = nullptr;
		if (bScene)
		{
			SCS.GetSceneRootComponentTemplate(false, &Root);
		}
		if (Root != nullptr && Root != &Node && Root != SCS.GetDefaultSceneRootNode() && SCS.GetRootNodes().Contains(Root))
		{
			Root->AddChildNode(&Node);
			return true;
		}
		SCS.AddNode(&Node);
		return true;
	}
	if (!bScene)
	{
		return Call.Args.Fail(TEXT("BlueprintMember.InvalidAttach"), TEXT("Only scene components can be attached."), TEXT("attach_to"));
	}
	const FName ParentName(*AttachTo);
	if (USCS_Node* Parent = SCS.FindSCSNode(ParentName))
	{
		if (Parent == &Node || IsDescendant(&Node, Parent))
		{
			return Call.Args.Fail(TEXT("BlueprintMember.InvalidAttach"), TEXT("A component cannot be attached to itself or its child."), TEXT("attach_to"));
		}
		if (Parent->ComponentTemplate == nullptr || !Parent->ComponentTemplate->IsA<USceneComponent>())
		{
			return Call.Args.Fail(TEXT("BlueprintMember.InvalidAttach"), FString::Printf(TEXT("'%s' is not a scene component."), *AttachTo), TEXT("attach_to"));
		}
		Parent->AddChildNode(&Node);
		return true;
	}
	if (USCS_Node* Inherited = FindInheritedNode(Blueprint, ParentName))
	{
		SCS.AddNode(&Node);
		Node.SetParent(Inherited);
		return true;
	}
	if (USceneComponent* Native = Cast<USceneComponent>(FindNativeComponent(ParentDefaults(Blueprint), ParentName)))
	{
		SCS.AddNode(&Node);
		Node.SetParent(Native);
		return true;
	}
	return Call.Args.Fail(TEXT("BlueprintMember.AttachParentNotFound"),
		FString::Printf(TEXT("No scene component named '%s' (own, inherited or native)."), *AttachTo), TEXT("attach_to"));
}

FString AttachName(UBlueprint& Blueprint, USimpleConstructionScript& SCS, USCS_Node& Node);

// The template of the scene component Node is attached to, as the Blueprint editor shows it (with this
// Blueprint's overrides of an inherited or native parent).
USceneComponent* AttachParentTemplate(UBlueprint& Blueprint, USimpleConstructionScript& SCS, USCS_Node& Node)
{
	if (USCS_Node* Parent = SCS.FindParentNode(&Node))
	{
		return Cast<USceneComponent>(Parent->ComponentTemplate);
	}
	if (Node.ParentComponentOrVariableName.IsNone())
	{
		return nullptr;
	}
	if (Node.bIsParentComponentNative)
	{
		UObject* Own = Blueprint.GeneratedClass != nullptr ? Blueprint.GeneratedClass->GetDefaultObject(false) : nullptr;
		UActorComponent* Native = FindNativeComponent(Own, Node.ParentComponentOrVariableName);
		return Cast<USceneComponent>(Native != nullptr ? Native : FindNativeComponent(ParentDefaults(Blueprint), Node.ParentComponentOrVariableName));
	}
	USCS_Node* Inherited = FindInheritedNode(Blueprint, Node.ParentComponentOrVariableName);
	if (Inherited == nullptr)
	{
		return nullptr;
	}
	if (UInheritableComponentHandler* Handler = Blueprint.GetInheritableComponentHandler(false))
	{
		const FComponentKey Key(Inherited);
		if (UActorComponent* Override = Handler->GetOverridenComponentTemplate(Key))
		{
			return Cast<USceneComponent>(Override);
		}
		return Cast<USceneComponent>(Handler->FindBestArchetype(Key));
	}
	return Cast<USceneComponent>(Inherited->ComponentTemplate);
}

// The Details panel's Parent Socket (FBlueprintComponentDetails::OnSocketSelection): a socket (or bone)
// of the parent component, picked from the parent's sockets; none clears it (OnClearSocket).
bool SetAttachSocket(FCall& Call, UBlueprint& Blueprint, USimpleConstructionScript& SCS, USCS_Node& Node, const FString& Socket)
{
	const FName SocketName = Socket.IsEmpty() ? NAME_None : FName(*Socket);
	if (!SocketName.IsNone())
	{
		USceneComponent* Parent = AttachParentTemplate(Blueprint, SCS, Node);
		if (Parent == nullptr)
		{
			return Call.Args.Fail(TEXT("BlueprintMember.InvalidAttach"),
				TEXT("AttachSocket names a socket of the AttachTo component: give AttachTo too."), TEXT("attach_socket"));
		}
		if (!Parent->DoesSocketExist(SocketName))
		{
			TArray<FName> Names = Parent->GetAllSocketNames();
			Names.Sort(FNameLexicalLess());
			TArray<FString> Shown;
			for (int32 Index = 0; Index < Names.Num() && Index < 12; ++Index)
			{
				Shown.Add(Names[Index].ToString());
			}
			return Call.Args.Fail(TEXT("BlueprintMember.InvalidAttach"), FString::Printf(
				TEXT("Component '%s' has no socket or bone '%s'%s"), *AttachName(Blueprint, SCS, Node), *Socket,
				Names.IsEmpty() ? TEXT(" (it has no sockets: set its mesh first).")
					: *FString::Printf(TEXT(". Sockets: %s%s."), *FString::Join(Shown, TEXT(", ")), Names.Num() > Shown.Num() ? TEXT(", ...") : TEXT(""))),
				TEXT("attach_socket"));
		}
	}
	if (Node.AttachToName != SocketName)
	{
		Node.Modify();
		Node.AttachToName = SocketName;
	}
	return true;
}

void DetachNode(USimpleConstructionScript& SCS, USCS_Node& Node)
{
	if (USCS_Node* Parent = SCS.FindParentNode(&Node))
	{
		Parent->RemoveChildNode(&Node);
	}
	else
	{
		SCS.RemoveNode(&Node, /*bValidateSceneRootNodes=*/false);
	}
	Node.bIsParentComponentNative = false;
	Node.ParentComponentOrVariableName = NAME_None;
	Node.ParentComponentOwnerClassName = NAME_None;
}

FString AttachName(UBlueprint& Blueprint, USimpleConstructionScript& SCS, USCS_Node& Node)
{
	if (USCS_Node* Parent = SCS.FindParentNode(&Node))
	{
		return Parent->GetVariableName().ToString();
	}
	if (Node.ParentComponentOrVariableName.IsNone())
	{
		return FString();
	}
	if (Node.bIsParentComponentNative)
	{
		UObject* Defaults = ParentDefaults(Blueprint);
		if (UActorComponent* Native = FindNativeComponent(Defaults, Node.ParentComponentOrVariableName))
		{
			return NativeComponentAlias(*Defaults, *Native).ToString();
		}
	}
	return Node.ParentComponentOrVariableName.ToString();
}

// Resolves the component template a property edit targets and its baseline.
bool ResolveComponentTarget(FCall& Call, UBlueprint& Blueprint, const FValue& Address, UObject*& OutTarget, const UObject*& OutBaseline)
{
	FReader Reader = Call.Args.Child(TEXT("component"), Address);
	FString Name;
	FString Scope = TEXT("owned");
	if (!Reader.OnlyFields({TEXT("name"), TEXT("scope")}) || !Reader.String(TEXT("name"), Name)
		|| !Reader.String(TEXT("scope"), Scope, false))
	{
		return false;
	}
	if (Scope == TEXT("owned"))
	{
		USimpleConstructionScript* SCS = GetSCS(Call, Blueprint);
		USCS_Node* Node = SCS ? SCS->FindSCSNode(FName(*Name)) : nullptr;
		if (Node == nullptr || Node->ComponentTemplate == nullptr)
		{
			return Reader.Fail(TEXT("BlueprintMember.ComponentNotFound"), FString::Printf(TEXT("No own component '%s'."), *Name), TEXT("name"));
		}
		OutTarget = Node->ComponentTemplate;
		OutBaseline = Node->ComponentClass ? Node->ComponentClass->GetDefaultObject() : nullptr;
		return true;
	}
	if (Scope == TEXT("inherited"))
	{
		USCS_Node* Inherited = FindInheritedNode(Blueprint, FName(*Name));
		UBlueprintGeneratedClass* Generated = Cast<UBlueprintGeneratedClass>(Blueprint.GeneratedClass);
		if (Inherited == nullptr || Generated == nullptr)
		{
			return Reader.Fail(TEXT("BlueprintMember.ComponentNotFound"), FString::Printf(TEXT("No inherited component '%s'."), *Name), TEXT("name"));
		}
		const FComponentKey Key(Inherited);
		UInheritableComponentHandler* Handler = Blueprint.GetInheritableComponentHandler(true);
		OutTarget = Handler ? Handler->CreateOverridenComponentTemplate(Key) : nullptr;
		// What the component inherits: the closest parent's override, else the original template
		// (the Details panel's reset-to-default value).
		OutBaseline = Handler ? Handler->FindBestArchetype(Key) : nullptr;
		if (OutTarget == nullptr)
		{
			return Reader.Fail(TEXT("BlueprintMember.ComponentNotFound"), FString::Printf(TEXT("Cannot override inherited component '%s'."), *Name), TEXT("name"));
		}
		return true;
	}
	if (Scope == TEXT("native"))
	{
		CompileIfNeeded(Blueprint);
		UObject* Defaults = Blueprint.GeneratedClass ? Blueprint.GeneratedClass->GetDefaultObject() : nullptr;
		FName SubobjectName;
		UActorComponent* Native = FindNativeComponent(Defaults, FName(*Name), &SubobjectName);
		UActorComponent* Baseline = Native ? Cast<UActorComponent>(ParentDefaults(Blueprint) ? ParentDefaults(Blueprint)->GetDefaultSubobjectByName(Native->GetFName()) : nullptr) : nullptr;
		if (Native == nullptr)
		{
			return Reader.Fail(TEXT("BlueprintMember.ComponentNotFound"), FString::Printf(TEXT("No native component '%s'."), *Name), TEXT("name"));
		}
		OutTarget = Native;
		OutBaseline = Baseline;
		return true;
	}
	return Reader.Fail(TEXT("Graph.InvalidArgument"), TEXT("scope is owned, inherited or native."), TEXT("scope"));
}

// ---- query ------------------------------------------------------------------------------

FValue EncodeVariable(UBlueprint& Blueprint, const FBPVariableDescription& Variable)
{
	FObjectBuilder Builder;
	Builder.AddString(TEXT("name"), Variable.VarName.ToString())
		.AddString(TEXT("guid"), Variable.VarGuid.ToString(EGuidFormats::Digits))
		.Add(TEXT("type"), MakePinTypeValue(Variable.VarType))
		.AddString(TEXT("category"), Variable.Category.ToString())
		.AddString(TEXT("tooltip"), Variable.HasMetaData(FBlueprintMetadata::MD_Tooltip) ? Variable.GetMetaData(FBlueprintMetadata::MD_Tooltip) : FString())
		.AddBool(TEXT("instance_editable"), (Variable.PropertyFlags & CPF_DisableEditOnInstance) == 0)
		.AddBool(TEXT("blueprint_read_only"), (Variable.PropertyFlags & CPF_BlueprintReadOnly) != 0)
		.AddBool(TEXT("expose_on_spawn"), Variable.HasMetaData(FBlueprintMetadata::MD_ExposeOnSpawn)
			&& Variable.GetMetaData(FBlueprintMetadata::MD_ExposeOnSpawn) == TEXT("true"))
		.AddBool(TEXT("private"), Variable.HasMetaData(FBlueprintMetadata::MD_Private)
			&& Variable.GetMetaData(FBlueprintMetadata::MD_Private) == TEXT("true"));
	UObject* Defaults = Blueprint.GeneratedClass ? Blueprint.GeneratedClass->GetDefaultObject(false) : nullptr;
	FProperty* Property = Blueprint.GeneratedClass ? FindFProperty<FProperty>(Blueprint.GeneratedClass, Variable.VarName) : nullptr;
	if (Defaults != nullptr && Property != nullptr)
	{
		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Defaults);
		Builder.Add(TEXT("default"), EncodePropertyValue(*Property, ValuePtr));
		// A User Defined Enum's value reads as its display name; its enumerator name is the same value.
		const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property);
		const FByteProperty* ByteProperty = CastField<FByteProperty>(Property);
		const UEnum* Enum = EnumProperty != nullptr ? EnumProperty->GetEnum() : ByteProperty != nullptr ? ByteProperty->Enum.Get() : nullptr;
		if (Enum != nullptr)
		{
			const int64 Raw = EnumProperty != nullptr
				? EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr) : static_cast<int64>(*static_cast<const uint8*>(ValuePtr));
			const FString Name = Enum->GetNameStringByValue(Raw);
			if (!Name.IsEmpty() && Name != Enum->GetAuthoredNameStringByValue(Raw))
			{
				Builder.AddString(TEXT("default_name"), Name);
			}
		}
	}
	else
	{
		Builder.Add(TEXT("default"), Variable.DefaultValue.IsEmpty() ? MakeNull() : MakeString(Variable.DefaultValue));
	}
	return Builder.Build();
}

// The Blueprint type the editor created the asset as (Content Browser > Blueprint Class / Blueprint
// Macro Library / Blueprint Interface / Blueprint Function Library; "const" = a Blueprint Class of a
// const parent class, FKismetEditorUtilities::CreateBlueprint).
const TCHAR* BlueprintTypeName(EBlueprintType Type)
{
	switch (Type)
	{
	case BPTYPE_Const: return TEXT("const");
	case BPTYPE_MacroLibrary: return TEXT("macro_library");
	case BPTYPE_Interface: return TEXT("interface");
	case BPTYPE_LevelScript: return TEXT("level_script");
	case BPTYPE_FunctionLibrary: return TEXT("function_library");
	default: return TEXT("normal");
	}
}

// One entry of a paged member query: an element of an array section or a field of an object section.
struct FQueryItem
{
	const TCHAR* Section = nullptr;
	FString Key;   // object sections only
	FValue Value;
};

// blueprint.member.query.v1 with "offset": the array / object sections (variables, dispatchers,
// timelines, components, class_defaults, properties) are cut into pages of whole entries that fit
// the runtime value budget; every page repeats the scalar header. "next_offset" is null on the
// last page. Without "offset" the whole result is returned (or Graph.ResultTooLarge).
bool AddQuerySections(FCall& Call, FObjectBuilder& Result, TArray<FQueryItem>& Items, const TCHAR* const* ArraySections,
	int32 NumArraySections, const TCHAR* const* ObjectSections, int32 NumObjectSections, TOptional<int64> Offset, int32 HeaderNodes)
{
	TArray<int32> Selected;
	int32 Next = Items.Num();
	if (!Offset.IsSet())
	{
		for (int32 Index = 0; Index < Items.Num(); ++Index)
		{
			Selected.Add(Index);
		}
	}
	else
	{
		int32 Budget = QueryValueNodeBudget - HeaderNodes - 4 * (NumArraySections + NumObjectSections) - 8;
		for (int32 Index = static_cast<int32>(FMath::Min<int64>(Offset.GetValue(), Items.Num())); Index < Items.Num(); ++Index)
		{
			const int32 Cost = CountValueNodes(Items[Index].Value) + 1;
			if (Cost > Budget)
			{
				if (Selected.IsEmpty())
				{
					const FString Label = Items[Index].Key.IsEmpty() ? FString::Printf(TEXT("#%d"), Index) : Items[Index].Key;
					return MemberFail(Call, TEXT("Graph.ResultTooLarge"), FString::Printf(
						TEXT("The %s entry %s alone exceeds the runtime value budget."), Items[Index].Section, *Label), TEXT("/offset"));
				}
				Next = Index;
				break;
			}
			Budget -= Cost;
			Selected.Add(Index);
		}
	}
	for (int32 Section = 0; Section < NumArraySections; ++Section)
	{
		TArray<FValue> Elements;
		for (const int32 Index : Selected)
		{
			if (FCString::Strcmp(Items[Index].Section, ArraySections[Section]) == 0)
			{
				Elements.Add(MoveTemp(Items[Index].Value));
			}
		}
		Result.Add(ArraySections[Section], MakeArray(MoveTemp(Elements)));
	}
	for (int32 Section = 0; Section < NumObjectSections; ++Section)
	{
		FObjectBuilder Fields;
		for (const int32 Index : Selected)
		{
			if (FCString::Strcmp(Items[Index].Section, ObjectSections[Section]) == 0)
			{
				Fields.Add(Items[Index].Key, MoveTemp(Items[Index].Value));
			}
		}
		Result.Add(ObjectSections[Section], Fields.Build());
	}
	if (Offset.IsSet())
	{
		Result.AddInt(TEXT("total"), Items.Num());
		Result.Add(TEXT("next_offset"), Next < Items.Num() ? MakeInt(Next) : MakeNull());
	}
	return true;
}

void AddArrayItems(TArray<FQueryItem>& Items, const TCHAR* Section, FValue Array)
{
	for (FValue& Element : Array.Elements)
	{
		Items.Add({Section, FString(), MoveTemp(Element)});
	}
}

void AddObjectItems(TArray<FQueryItem>& Items, const TCHAR* Section, FValue Object)
{
	for (int32 Index = 0; Index < Object.ObjectKeys.Num(); ++Index)
	{
		Items.Add({Section, Object.ObjectKeys[Index], MoveTemp(Object.Elements[Index])});
	}
}

// blueprint.member.query.v1 with "defaults_of": [class paths] -> {"defaults_of": {path: {property: value}}}: the editable
// properties of each class's default object, all of them (instanced values as the query writes them, sparse over their
// own class's defaults). The query writes an instanced value's Properties sparse over its class's defaults; Verify / Diff
// read these to tell an authored value equal to its default (left out) from a lost one. A path that names no class is
// left out.
bool QueryClassDefaults(FCall& Call, FObjectBuilder& Result)
{
	const FValue* Paths = nullptr;
	if (!Call.Args.OnlyFields({TEXT("defaults_of")}) || !Call.Args.Array(TEXT("defaults_of"), Paths))
	{
		return false;
	}
	FObjectBuilder Classes;
	TSet<FString> Seen;
	for (const FValue& Path : Paths->Elements)
	{
		if (Path.Kind != EAssetDocumentRuntimeValueKind::String)
		{
			return Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("defaults_of lists class paths."), TEXT("defaults_of"));
		}
		bool bSeen = false;
		Seen.Add(Path.StringValue, &bSeen);
		UClass* Class = bSeen ? nullptr : FindObject<UClass>(nullptr, *Path.StringValue);
		if (Class == nullptr && !bSeen && FPackageName::IsValidObjectPath(Path.StringValue))
		{
			Class = LoadObject<UClass>(nullptr, *Path.StringValue, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		if (UObject* Defaults = Class != nullptr ? Class->GetDefaultObject() : nullptr)
		{
			Classes.Add(Path.StringValue, EncodeChangedProperties(*Defaults, nullptr, [](const FProperty&) { return true; }, 1));
		}
	}
	Result.Add(TEXT("defaults_of"), Classes.Build());
	return true;
}

// ---- check_picks: Validate's check of written row handles and keys ---------------------
} // namespace

// Unity-safe per-file helpers.
namespace AssetDocumentBlueprintMemberPrimitivesLocal
{

const FValue* FindPickCheckField(const FValue& Object, FStringView Key)
{
	if (Object.Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return nullptr;
	}
	const int32 Index = Object.ObjectKeys.IndexOfByPredicate([Key](const FString& Candidate) { return Candidate == Key; });
	return Index == INDEX_NONE ? nullptr : &Object.Elements[Index];
}

FString PickCheckString(const FValue& Object, FStringView Key)
{
	const FValue* Field = FindPickCheckField(Object, Key);
	return Field != nullptr && Field->Kind == EAssetDocumentRuntimeValueKind::String ? Field->StringValue : FString();
}

// A struct value without a property (a new variable's default): UE text or an object of fields.
bool DecodePickCheckStruct(const UScriptStruct& Struct, void* Memory, const FValue& Value)
{
	if (Value.Kind == EAssetDocumentRuntimeValueKind::String)
	{
		FStringOutputDevice Errors;
		return Struct.ImportText(*Value.StringValue, Memory, nullptr, PPF_None, &Errors, Struct.GetName()) != nullptr;
	}
	if (Value.Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return false;
	}
	for (int32 Index = 0; Index < Value.ObjectKeys.Num(); ++Index)
	{
		FProperty* Field = Struct.FindPropertyByName(FName(*Value.ObjectKeys[Index]));
		FString Error;
		if (Field == nullptr || !DecodeValue(*Field, Field->ContainerPtrToValuePtr<void>(Memory), Value.Elements[Index], Error))
		{
			return false;
		}
	}
	return true;
}

// The object whose property a check_picks item writes, read only (an inherited component's override is not made): the
// Blueprint, its class defaults (the parent's before the first compile) or a component template (an own component
// the Blueprint does not have yet: its class's defaults).
UObject* FindPickCheckObject(UBlueprint& Blueprint, const FValue& Item)
{
	UObject* Defaults = Blueprint.GeneratedClass != nullptr ? Blueprint.GeneratedClass->GetDefaultObject() : ParentDefaults(Blueprint);
	const FString Target = PickCheckString(Item, TEXT("target"));
	if (Target == TEXT("blueprint"))
	{
		return &Blueprint;
	}
	if (Target == TEXT("class_defaults"))
	{
		return Defaults;
	}
	const FValue* Component = FindPickCheckField(Item, TEXT("component"));
	if (Target != TEXT("component") || Component == nullptr)
	{
		return nullptr;
	}
	const FName Name(*PickCheckString(*Component, TEXT("name")));
	const FString Scope = PickCheckString(*Component, TEXT("scope"));
	if (Scope == TEXT("inherited"))
	{
		USCS_Node* Inherited = FindInheritedNode(Blueprint, Name);
		UInheritableComponentHandler* Handler = Blueprint.GetInheritableComponentHandler(false);
		UActorComponent* Override = Inherited != nullptr && Handler != nullptr
			? Handler->GetOverridenComponentTemplate(FComponentKey(Inherited)) : nullptr;
		return Override != nullptr ? Override : Inherited != nullptr ? Inherited->ComponentTemplate.Get() : nullptr;
	}
	if (Scope == TEXT("native"))
	{
		return FindNativeComponent(Defaults, Name);
	}
	USCS_Node* Node = Blueprint.SimpleConstructionScript != nullptr ? Blueprint.SimpleConstructionScript->FindSCSNode(Name) : nullptr;
	if (Node != nullptr && Node->ComponentTemplate != nullptr)
	{
		return Node->ComponentTemplate;
	}
	const FString ClassPath = PickCheckString(Item, TEXT("class"));
	UClass* Class = ClassPath.IsEmpty() ? nullptr : FindObject<UClass>(nullptr, *ClassPath);
	if (Class == nullptr && FPackageName::IsValidObjectPath(ClassPath))
	{
		Class = LoadObject<UClass>(nullptr, *ClassPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	return Class != nullptr ? Class->GetDefaultObject() : nullptr;
}

// One property item: Value decoded onto a copy of the property's current value; the picks it holds now are kept.
bool CheckPickProperty(UObject& Object, const FString& Path, const FValue& Value, EditorPickerValues::FInvalidPick& OutInvalid)
{
	FResolvedPropertyPath Resolved;
	FString Error;
	if (!ResolvePropertyPath(Object, Path, Resolved, Error))
	{
		return false;  // Apply reports the property
	}
	const FProperty& Leaf = *Resolved.Leaf;
	void* Copy = FMemory::Malloc(FMath::Max(Leaf.GetSize(), 1), Leaf.GetMinAlignment());
	Leaf.InitializeValue(Copy);
	Leaf.CopyCompleteValue(Copy, Resolved.LeafPtr);
	const bool bInvalid = DecodeValue(Leaf, Copy, Value, Error)
		&& EditorPickerValues::FindInvalidPick(Leaf, Copy, static_cast<const void*>(Resolved.LeafPtr), OutInvalid);
	Leaf.DestroyValue(Copy);
	FMemory::Free(Copy);
	return bInvalid;
}

// One variable default: onto the compiled variable when the class has it, else by its declared type (a struct, or an
// array / set of one).
bool CheckPickVariable(FCall& Call, UBlueprint& Blueprint, const FValue& Item, const FValue& Value, EditorPickerValues::FInvalidPick& OutInvalid)
{
	const FName Name(*PickCheckString(Item, TEXT("name")));
	UObject* Defaults = Blueprint.GeneratedClass != nullptr ? Blueprint.GeneratedClass->GetDefaultObject() : nullptr;
	if (Defaults != nullptr && FindFProperty<FProperty>(Blueprint.GeneratedClass, Name) != nullptr
		&& CheckPickProperty(*Defaults, Name.ToString(), Value, OutInvalid))
	{
		return true;
	}
	const FValue* TypeValue = FindPickCheckField(Item, TEXT("type"));
	FEdGraphPinType Type;
	if (TypeValue == nullptr)
	{
		return false;
	}
	FReader TypeReader = Call.Args.Child(TEXT("type"), *TypeValue);
	const UScriptStruct* Struct = ReadPinType(TypeReader, Type) && Type.PinCategory == UEdGraphSchema_K2::PC_Struct
		? Cast<UScriptStruct>(Type.PinSubCategoryObject.Get()) : nullptr;
	Call.Error = FAssetDocumentGraphError();
	if (Struct == nullptr || Type.IsMap())
	{
		return false;
	}
	const bool bListed = Type.IsArray() || Type.IsSet();
	if (bListed && Value.Kind != EAssetDocumentRuntimeValueKind::Array)
	{
		return false;
	}
	const int32 Count = bListed ? Value.Elements.Num() : 1;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		FStructOnScope Memory(Struct);
		if (DecodePickCheckStruct(*Struct, Memory.GetStructMemory(), bListed ? Value.Elements[Index] : Value)
			&& EditorPickerValues::FindInvalidPickInStruct(*Struct, Memory.GetStructMemory(), {}, OutInvalid))
		{
			OutInvalid.ValuePath = (bListed ? FString::Printf(TEXT("/%d"), Index) : FString()) + OutInvalid.ValuePath;
			return true;
		}
	}
	return false;
}

// check_picks: the values a document writes (variable defaults, class defaults, component and Blueprint properties)
// that name a table row or a key no Details picker offers, for Validate (Apply refuses them as it writes).
// Items: {"at", "value", "name", "type"} (a variable) or {"at", "value", "target", "component"?, "class"?, "property"}.
bool QueryPickChecks(FCall& Call, UBlueprint& Blueprint, FObjectBuilder& Result)
{
	const FValue* Items = nullptr;
	if (!Call.Args.OnlyFields({TEXT("check_picks")}) || !Call.Args.Array(TEXT("check_picks"), Items))
	{
		return false;
	}
	TArray<FValue> Problems;
	for (const FValue& Item : Items->Elements)
	{
		const FValue* Value = FindPickCheckField(Item, TEXT("value"));
		if (Value == nullptr)
		{
			continue;
		}
		EditorPickerValues::FInvalidPick Invalid;
		FString Subject;
		if (FindPickCheckField(Item, TEXT("type")) != nullptr)
		{
			if (!CheckPickVariable(Call, Blueprint, Item, *Value, Invalid))
			{
				continue;
			}
			Subject = FString::Printf(TEXT("Default of '%s'"), *PickCheckString(Item, TEXT("name")));
		}
		else
		{
			UObject* Object = FindPickCheckObject(Blueprint, Item);
			const FString Path = PickCheckString(Item, TEXT("property"));
			if (Object == nullptr || Path.IsEmpty() || !CheckPickProperty(*Object, Path, *Value, Invalid))
			{
				continue;
			}
			Subject = FString::Printf(TEXT("'%s'"), *Path);
		}
		FObjectBuilder Problem;
		Problem.AddString(TEXT("at"), PickCheckString(Item, TEXT("at")) + Invalid.ValuePath);
		Problem.AddString(TEXT("code"), Invalid.Code);
		Problem.AddString(TEXT("message"), FString::Printf(TEXT("%s%s: %s"), *Subject, *Invalid.ValuePath, *Invalid.Message));
		Problems.Add(Problem.Build());
	}
	Result.Add(TEXT("check_picks"), MakeArray(MoveTemp(Problems)));
	return true;
}
} // namespace AssetDocumentBlueprintMemberPrimitivesLocal

namespace
{

bool MemberHandleQuery(FCall& Call, FObjectBuilder& Result)
{
	UBlueprint* Blueprint = GetBlueprint(Call);
	if (Blueprint != nullptr && Call.Args.Has(TEXT("defaults_of")))
	{
		return QueryClassDefaults(Call, Result);
	}
	if (Blueprint != nullptr && Call.Args.Has(TEXT("check_picks")))
	{
		return AssetDocumentBlueprintMemberPrimitivesLocal::QueryPickChecks(Call, *Blueprint, Result);
	}
	int64 OffsetValue = 0;
	if (Blueprint == nullptr || !Call.Args.OnlyFields({TEXT("offset")})
		|| !Call.Args.RefuseNull(TEXT("offset"), TEXT("Graph.InvalidArgument"),
			TEXT("send a number to page (0 for the first page), or omit offset to read everything; next_offset is null on the last page."))
		|| !Call.Args.Int(TEXT("offset"), OffsetValue, false))
	{
		return false;
	}
	TOptional<int64> Offset;
	if (Call.Args.Has(TEXT("offset")))
	{
		if (OffsetValue < 0)
		{
			return Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("offset must be >= 0."), TEXT("offset"));
		}
		Offset = OffsetValue;
	}
	const TCHAR* Status = TEXT("unknown");
	switch (Blueprint->Status)
	{
	case BS_UpToDate: Status = TEXT("up_to_date"); break;
	case BS_UpToDateWithWarnings: Status = TEXT("up_to_date_with_warnings"); break;
	case BS_Dirty: Status = TEXT("dirty"); break;
	case BS_Error: Status = TEXT("error"); break;
	default: break;
	}
	TArray<FString> Interfaces;
	for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
	{
		if (Interface.Interface != nullptr)
		{
			Interfaces.Add(Interface.Interface->GetPathName());
		}
	}
	FValue InterfacesValue = MakeStringArray(Interfaces);
	FValue SettingsValue = MakeNull();
	const TSharedPtr<IAssetDocumentBlueprintSettingsAdapter> Settings = FindAssetDocumentBlueprintSettingsAdapter(Blueprint);
	if (Settings.IsValid())
	{
		SettingsValue = Settings->QuerySettings(*Blueprint);
	}
	// The native scene components' names: AttachTo names one by its component (member) name or its subobject
	// name (a Character's capsule: CapsuleComponent / CollisionCylinder), and an owned component without a
	// parent attaches to the native root.
	FString SceneRoot;
	FObjectBuilder AliasesBuilder;
	if (UObject* NativeDefaults = ParentDefaults(*Blueprint))
	{
		if (const AActor* ActorDefaults = Cast<AActor>(NativeDefaults))
		{
			if (const USceneComponent* Root = ActorDefaults->GetRootComponent())
			{
				SceneRoot = NativeComponentAlias(*NativeDefaults, *Root).ToString();
			}
		}
		TArray<UObject*> NativeSubobjects;
		NativeDefaults->GetDefaultSubobjects(NativeSubobjects);
		for (const UObject* Subobject : NativeSubobjects)
		{
			if (const USceneComponent* Native = Cast<USceneComponent>(Subobject))
			{
				const FName Alias = NativeComponentAlias(*NativeDefaults, *Native);
				if (Alias != Native->GetFName())
				{
					AliasesBuilder.AddString(Native->GetName(), Alias.ToString());
				}
			}
		}
	}
	// Without a native root, the owned scene root (the one a parentless scene component attaches to; AttachNode).
	if (SceneRoot.IsEmpty() && Blueprint->SimpleConstructionScript != nullptr)
	{
		USCS_Node* RootNode = nullptr;
		Blueprint->SimpleConstructionScript->GetSceneRootComponentTemplate(false, &RootNode);
		if (RootNode != nullptr && RootNode != Blueprint->SimpleConstructionScript->GetDefaultSceneRootNode()
			&& Blueprint->SimpleConstructionScript->GetRootNodes().Contains(RootNode))
		{
			SceneRoot = RootNode->GetVariableName().ToString();
		}
	}
	FValue AliasesValue = AliasesBuilder.Build();
	const int32 HeaderNodes = CountValueNodes(InterfacesValue) + CountValueNodes(SettingsValue) + CountValueNodes(AliasesValue) + 14;
	Result.AddString(TEXT("parent_class"), ObjectPathOrEmpty(Blueprint->ParentClass))
		.AddString(TEXT("generated_class"), ObjectPathOrEmpty(Blueprint->GeneratedClass))
		.AddString(TEXT("status"), Status)
		.AddString(TEXT("blueprint_type"), BlueprintTypeName(Blueprint->BlueprintType))
		.Add(TEXT("interfaces"), MoveTemp(InterfacesValue))
		.AddString(TEXT("scene_root"), SceneRoot)
		.Add(TEXT("native_component_names"), MoveTemp(AliasesValue));
	if (Settings.IsValid())
	{
		Result.Add(TEXT("settings"), MoveTemp(SettingsValue));
	}
	TArray<FQueryItem> Items;
	TArray<FValue> Variables;
	for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
	{
		if (Variable.VarType.PinCategory != UEdGraphSchema_K2::PC_MCDelegate)  // dispatchers are listed below
		{
			Variables.Add(EncodeVariable(*Blueprint, Variable));
		}
	}
	AddArrayItems(Items, TEXT("variables"), MakeArray(MoveTemp(Variables)));
	AddArrayItems(Items, TEXT("dispatchers"), BlueprintDispatchers::Query(*Blueprint));
	AddArrayItems(Items, TEXT("timelines"), BlueprintTimelines::Query(*Blueprint));

	UClass* Generated = Blueprint->GeneratedClass;
	UObject* Defaults = Generated ? Generated->GetDefaultObject(false) : nullptr;
	UObject* Parent = ParentDefaults(*Blueprint);
	auto Any = [](const FProperty&) { return true; };
	TArray<FValue> Components;
	if (USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript)
	{
		for (USCS_Node* Node : SCS->GetAllNodes())
		{
			if (Node == nullptr || Node == SCS->GetDefaultSceneRootNode() || Node->ComponentTemplate == nullptr)
			{
				continue;
			}
			Components.Add(FObjectBuilder()
				.AddString(TEXT("name"), Node->GetVariableName().ToString())
				.AddString(TEXT("scope"), TEXT("owned"))
				.AddString(TEXT("class"), ObjectPathOrEmpty(Node->ComponentClass))
				.AddString(TEXT("attach_to"), AttachName(*Blueprint, *SCS, *Node))
				.AddString(TEXT("attach_socket"), Node->AttachToName.IsNone() ? FString() : Node->AttachToName.ToString())
				.Add(TEXT("properties"), EncodeChangedProperties(*Node->ComponentTemplate,
					Node->ComponentClass ? Node->ComponentClass->GetDefaultObject() : nullptr, Any))
				.Build());
		}
	}
	if (UInheritableComponentHandler* Handler = Blueprint->GetInheritableComponentHandler(false))
	{
		for (auto It = Handler->CreateRecordIterator(); It; ++It)
		{
			const FComponentKey& Key = It->ComponentKey;
			// An override lists what differs from what the component inherits: the closest parent
			// Blueprint's override of it, else the original template (a grandchild that sets a value
			// back to the original template's still overrides its parent's value).
			UActorComponent* Inherited = Handler->FindBestArchetype(Key);
			if (It->ComponentTemplate == nullptr || Inherited == nullptr || Key.GetOriginalTemplate() == nullptr || !Key.IsSCSKey())
			{
				continue;
			}
			const FValue Properties = EncodeChangedProperties(*It->ComponentTemplate, Inherited, Any);
			if (Properties.ObjectKeys.IsEmpty())
			{
				continue;
			}
			Components.Add(FObjectBuilder()
				.AddString(TEXT("name"), Key.GetSCSVariableName().ToString())
				.AddString(TEXT("scope"), TEXT("inherited"))
				.AddString(TEXT("class"), ObjectPathOrEmpty(It->ComponentTemplate->GetClass()))
				.AddString(TEXT("owner_class"), ObjectPathOrEmpty(Key.GetComponentOwner()))
				.Add(TEXT("properties"), Properties)
				.Build());
		}
	}
	if (Defaults != nullptr && Parent != nullptr)
	{
		TArray<UObject*> Subobjects;
		Defaults->GetDefaultSubobjects(Subobjects);
		for (UObject* Subobject : Subobjects)
		{
			UActorComponent* Component = Cast<UActorComponent>(Subobject);
			UObject* Baseline = Component ? Parent->GetDefaultSubobjectByName(Component->GetFName()) : nullptr;
			if (Component == nullptr || Baseline == nullptr || Baseline->GetClass() != Component->GetClass())
			{
				continue;
			}
			const FValue Properties = EncodeChangedProperties(*Component, Baseline, Any);
			if (Properties.ObjectKeys.IsEmpty())
			{
				continue;
			}
			const FName Alias = NativeComponentAlias(*Defaults, *Component);
			FObjectProperty* AliasProperty = FindFProperty<FObjectProperty>(Defaults->GetClass(), Alias);
			Components.Add(FObjectBuilder()
				.AddString(TEXT("name"), Alias.ToString())
				.AddString(TEXT("subobject"), Component->GetName())
				.AddString(TEXT("scope"), TEXT("native"))
				.AddString(TEXT("class"), ObjectPathOrEmpty(Component->GetClass()))
				.AddString(TEXT("owner_class"), ObjectPathOrEmpty(AliasProperty ? AliasProperty->GetOwnerClass() : Parent->GetClass()))
				.Add(TEXT("properties"), Properties)
				.Build());
		}
	}
	AddArrayItems(Items, TEXT("components"), MakeArray(MoveTemp(Components)));
	AddObjectItems(Items, TEXT("class_defaults"), Defaults != nullptr
		? EncodeChangedProperties(*Defaults, Parent, [Generated](const FProperty& Property) { return Property.GetOwnerClass() != Generated; })
		: MakeEmptyObject());
	// Class Settings baseline: the asset class's defaults (UBlueprint, UAnimBlueprint...).
	AddObjectItems(Items, TEXT("properties"), EncodeChangedProperties(*Blueprint, Blueprint->GetClass()->GetDefaultObject(), Any));
	static const TCHAR* const ArraySections[] = {TEXT("variables"), TEXT("dispatchers"), TEXT("timelines"), TEXT("components")};
	static const TCHAR* const ObjectSections[] = {TEXT("class_defaults"), TEXT("properties")};
	return AddQuerySections(Call, Result, Items, ArraySections, UE_ARRAY_COUNT(ArraySections), ObjectSections,
		UE_ARRAY_COUNT(ObjectSections), Offset, HeaderNodes);
}

// ---- parent / interfaces ------------------------------------------------------------------

bool HandleParentSet(FCall& Call, FObjectBuilder& Result)
{
	UBlueprint* Blueprint = GetBlueprint(Call);
	UClass* Parent = nullptr;
	if (Blueprint == nullptr || !Call.Args.OnlyFields({TEXT("parent_class")}) || !ReadClass(Call, TEXT("parent_class"), Parent))
	{
		return false;
	}
	// The reparent menu lists only classes the Blueprint type allows (UBlueprint::GetReparentingRules;
	// a Widget Blueprint keeps a UUserWidget parent).
	TSet<const UClass*> Allowed;
	TSet<const UClass*> Disallowed;
	Blueprint->GetReparentingRules(Allowed, Disallowed);
	const auto IsChildOfAny = [Parent](const TSet<const UClass*>& Classes)
	{
		return Classes.Array().ContainsByPredicate([Parent](const UClass* Class) { return Parent->IsChildOf(Class); });
	};
	if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(Parent)
		|| (Blueprint->GeneratedClass != nullptr && Parent->IsChildOf(Blueprint->GeneratedClass))
		|| (!Allowed.IsEmpty() && !IsChildOfAny(Allowed)) || IsChildOfAny(Disallowed))
	{
		return Call.Args.Fail(TEXT("BlueprintMember.InvalidParentClass"),
			FString::Printf(TEXT("%s cannot be the parent of this Blueprint."), *Parent->GetPathName()), TEXT("parent_class"));
	}
	const bool bHasComponents = Blueprint->SimpleConstructionScript != nullptr
		&& Blueprint->SimpleConstructionScript->GetAllNodes().ContainsByPredicate([Blueprint](USCS_Node* Node)
			{ return Node != Blueprint->SimpleConstructionScript->GetDefaultSceneRootNode(); });
	if (bHasComponents && !Parent->IsChildOf<AActor>())
	{
		return Call.Args.Fail(TEXT("BlueprintMember.InvalidParentClass"),
			TEXT("A Blueprint with components needs an Actor parent class; remove the components first."), TEXT("parent_class"));
	}
	if (Blueprint->ParentClass != Parent)
	{
		// FBlueprintEditor::ReparentBlueprint_NewParentChosen.
		Blueprint->Modify();
		Blueprint->ParentClass = Parent;
		FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
		NotifyStructural(*Blueprint);
		CompileIfNeeded(*Blueprint);
	}
	Result.AddString(TEXT("parent_class"), Parent->GetPathName());
	return true;
}

bool ReadInterface(FCall& Call, UClass*& OutInterface)
{
	if (!Call.Args.OnlyFields({TEXT("interface")}) || !ReadClass(Call, TEXT("interface"), OutInterface))
	{
		return false;
	}
	if (!OutInterface->HasAnyClassFlags(CLASS_Interface))
	{
		return Call.Args.Fail(TEXT("BlueprintMember.InvalidInterface"),
			FString::Printf(TEXT("%s is not an interface."), *OutInterface->GetPathName()), TEXT("interface"));
	}
	return true;
}

// My Blueprint offers no variables (FBlueprintEditor::NewDocument_IsVisibleForType CGT_NewVariable) and
// Class Settings no interfaces in a Function Library, Interface or Macro Library.
bool RefuseForBlueprintType(FCall& Call, const UBlueprint& Blueprint, const TCHAR* What)
{
	if (Blueprint.BlueprintType != BPTYPE_FunctionLibrary && Blueprint.BlueprintType != BPTYPE_Interface
		&& Blueprint.BlueprintType != BPTYPE_MacroLibrary)
	{
		return false;
	}
	MemberFail(Call, TEXT("BlueprintMember.BlueprintTypeUnsupported"), FString::Printf(TEXT("A %s Blueprint has no %s."),
		BlueprintTypeName(Blueprint.BlueprintType), What), TEXT("/"));
	return true;
}

bool HandleInterfaceAdd(FCall& Call, FObjectBuilder& Result)
{
	UBlueprint* Blueprint = GetBlueprint(Call);
	UClass* Interface = nullptr;
	if (Blueprint == nullptr || RefuseForBlueprintType(Call, *Blueprint, TEXT("implemented interfaces")) || !ReadInterface(Call, Interface))
	{
		return false;
	}
	const bool bPresent = Blueprint->ImplementedInterfaces.ContainsByPredicate(
		[Interface](const FBPInterfaceDescription& Existing) { return Existing.Interface == Interface; });
	if (!bPresent)
	{
		if (!FBlueprintEditorUtils::ImplementNewInterface(Blueprint, Interface->GetClassPathName()))
		{
			return Call.Args.Fail(TEXT("BlueprintMember.InvalidInterface"),
				FString::Printf(TEXT("%s cannot be implemented by this Blueprint."), *Interface->GetPathName()), TEXT("interface"));
		}
		NotifyStructural(*Blueprint);
	}
	return true;
}

bool HandleInterfaceRemove(FCall& Call, FObjectBuilder& Result)
{
	UBlueprint* Blueprint = GetBlueprint(Call);
	UClass* Interface = nullptr;
	if (Blueprint == nullptr || !ReadInterface(Call, Interface))
	{
		return false;
	}
	FBlueprintEditorUtils::RemoveInterface(Blueprint, Interface->GetClassPathName(), /*bPreserveFunctions=*/false);
	NotifyStructural(*Blueprint);
	return true;
}

// ---- variables ----------------------------------------------------------------------------

void SetMetaFlag(UBlueprint& Blueprint, FName Name, const FName& Key, bool bValue)
{
	if (bValue)
	{
		FBlueprintEditorUtils::SetBlueprintVariableMetaData(&Blueprint, Name, nullptr, Key, TEXT("true"));
	}
	else
	{
		FBlueprintEditorUtils::RemoveBlueprintVariableMetaData(&Blueprint, Name, nullptr, Key);
	}
}

bool HandleVariableSet(FCall& Call, FObjectBuilder& Result)
{
	UBlueprint* Blueprint = GetBlueprint(Call);
	FName Name;
	const FValue* TypeValue = nullptr;
	if (Blueprint == nullptr
		|| !Call.Args.OnlyFields({TEXT("name"), TEXT("type"), TEXT("category"), TEXT("tooltip"), TEXT("instance_editable"),
			TEXT("blueprint_read_only"), TEXT("expose_on_spawn"), TEXT("private"), TEXT("default")})
		|| !ReadName(Call, TEXT("name"), Name) || !Call.Args.Object(TEXT("type"), TypeValue)
		|| RefuseForBlueprintType(Call, *Blueprint, TEXT("member variables")))
	{
		return false;
	}
	FEdGraphPinType Type;
	FReader TypeReader = Call.Args.Child(TEXT("type"), *TypeValue);
	if (!ReadPinType(TypeReader, Type))
	{
		return false;
	}
	FString Category, Tooltip;
	bool bInstanceEditable = false, bReadOnly = false, bExpose = false, bPrivate = false;
	if (!Call.Args.String(TEXT("category"), Category, false) || !Call.Args.String(TEXT("tooltip"), Tooltip, false)
		|| !Call.Args.Bool(TEXT("instance_editable"), bInstanceEditable) || !Call.Args.Bool(TEXT("blueprint_read_only"), bReadOnly)
		|| !Call.Args.Bool(TEXT("expose_on_spawn"), bExpose) || !Call.Args.Bool(TEXT("private"), bPrivate))
	{
		return false;
	}
	int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, Name);
	if (Index != INDEX_NONE && Blueprint->NewVariables[Index].VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
	{
		return Call.Args.Fail(TEXT("BlueprintMember.InvalidName"), FString::Printf(
			TEXT("'%s' is an event dispatcher of this Blueprint, not a variable."), *Name.ToString()), TEXT("name"));
	}
	if (Index == INDEX_NONE)
	{
		if (!ValidateNewName(Call, *Blueprint, Name, TEXT("name")))
		{
			return false;
		}
		if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, Name, Type))
		{
			return Call.Args.Fail(TEXT("BlueprintMember.InvalidName"), FString::Printf(TEXT("Variable '%s' could not be added."), *Name.ToString()), TEXT("name"));
		}
		Index = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, Name);
		Result.AddBool(TEXT("created"), true);
	}
	else if (!(Blueprint->NewVariables[Index].VarType == Type))
	{
		// The document states the new type: the editor's confirmed "Change Variable Type" path, without its dialog.
		const EditorConfirmations::FScopedConfirmedDialog Confirmed(EditorConfirmations::ChangeVariableType);
		FBlueprintEditorUtils::ChangeMemberVariableType(Blueprint, Name, Type);
		if (!(Blueprint->NewVariables[Index].VarType == Type))
		{
			return Call.Args.Fail(TEXT("BlueprintMember.InvalidValue"), FString::Printf(
				TEXT("The editor did not change the type of variable '%s' (the type is not valid for a member variable)."),
				*Name.ToString()), TEXT("type"));
		}
	}
	if (Index == INDEX_NONE)
	{
		return MemberFail(Call, TEXT("BlueprintMember.VariableNotFound"), TEXT("The variable disappeared after it was added."));
	}
	// My Blueprint / Details panel edits of the variable.
	if (Call.Args.Has(TEXT("category")))
	{
		FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, Name, nullptr,
			Category.IsEmpty() ? UEdGraphSchema_K2::VR_DefaultCategory : FText::FromString(Category), /*bDontRecompile=*/true);
	}
	if (Call.Args.Has(TEXT("tooltip")))
	{
		if (Tooltip.IsEmpty())
		{
			FBlueprintEditorUtils::RemoveBlueprintVariableMetaData(Blueprint, Name, nullptr, FBlueprintMetadata::MD_Tooltip);
		}
		else
		{
			FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, Name, nullptr, FBlueprintMetadata::MD_Tooltip, Tooltip);
		}
	}
	const FBPVariableDescription& Current = Blueprint->NewVariables[Index];
	if (Call.Args.Has(TEXT("instance_editable")) && ((Current.PropertyFlags & CPF_DisableEditOnInstance) == 0) != bInstanceEditable)
	{
		FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, Name, !bInstanceEditable);
	}
	if (Call.Args.Has(TEXT("blueprint_read_only")) && ((Blueprint->NewVariables[Index].PropertyFlags & CPF_BlueprintReadOnly) != 0) != bReadOnly)
	{
		FBlueprintEditorUtils::SetBlueprintPropertyReadOnlyFlag(Blueprint, Name, bReadOnly);
	}
	if (Call.Args.Has(TEXT("expose_on_spawn")))
	{
		SetMetaFlag(*Blueprint, Name, FBlueprintMetadata::MD_ExposeOnSpawn, bExpose);
	}
	if (Call.Args.Has(TEXT("private")))
	{
		SetMetaFlag(*Blueprint, Name, FBlueprintMetadata::MD_Private, bPrivate);
	}
	NotifyStructural(*Blueprint);
	const FValue* Default = Call.Args.Find(TEXT("default"));
	if (Default != nullptr && Default->Kind != EAssetDocumentRuntimeValueKind::Null)
	{
		// The default lives on the CDO; like the editor, it needs a compiled class first.
		CompileIfNeeded(*Blueprint);
		UObject* Defaults = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject() : nullptr;
		FProperty* Property = Blueprint->GeneratedClass ? FindFProperty<FProperty>(Blueprint->GeneratedClass, Name) : nullptr;
		FString Error;
		if (Defaults == nullptr || Property == nullptr)
		{
			return Call.Args.Fail(TEXT("BlueprintMember.InvalidDefault"), TEXT("The Blueprint does not compile, so the default cannot be set."), TEXT("default"));
		}
		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Defaults);
		const TArray<FString> Kept = EditorPickerValues::CollectPicks(*Property, ValuePtr);
		if (!DecodeValue(*Property, ValuePtr, *Default, Error))
		{
			return Call.Args.Fail(TEXT("BlueprintMember.InvalidDefault"), Error, TEXT("default"));
		}
		EditorPickerValues::FInvalidPick Invalid;
		if (EditorPickerValues::FindInvalidPick(*Property, ValuePtr, Kept, Invalid))
		{
			return MemberFail(Call, *Invalid.Code, FString::Printf(TEXT("Default of '%s'%s: %s"), *Name.ToString(), *Invalid.ValuePath,
				*Invalid.Message), *(TEXT("/default") + Invalid.ValuePath));
		}
		// The class default object holds the value, as when the editor's Details panel edits a
		// variable's Default Value; recompiles carry it over. No text copy goes into the variable
		// description: the next full compile would import that text over the value, and the text
		// export rounds floating-point values (0.69999998807907104 -> "0.700000").
		const int32 After = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, Name);
		if (After != INDEX_NONE)
		{
			Blueprint->NewVariables[After].DefaultValue.Empty();
		}
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		Result.Add(TEXT("default"), EncodePropertyValue(*Property, ValuePtr));
	}
	return true;
}

bool HandleVariableRemove(FCall& Call, FObjectBuilder& Result)
{
	UBlueprint* Blueprint = GetBlueprint(Call);
	FName Name;
	if (Blueprint == nullptr || !Call.Args.OnlyFields({TEXT("name")}) || !ReadName(Call, TEXT("name"), Name))
	{
		return false;
	}
	const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, Name);
	if (Index == INDEX_NONE || Blueprint->NewVariables[Index].VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
	{
		return Call.Args.Fail(TEXT("BlueprintMember.VariableNotFound"), FString::Printf(TEXT("No variable '%s'."), *Name.ToString()), TEXT("name"));
	}
	FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, Name);
	NotifyStructural(*Blueprint);
	return true;
}

// ---- components ---------------------------------------------------------------------------

bool HandleComponentAdd(FCall& Call, FObjectBuilder& Result)
{
	UBlueprint* Blueprint = GetBlueprint(Call);
	FName Name;
	UClass* Class = nullptr;
	FString AttachTo;
	FString Socket;
	if (Blueprint == nullptr || !Call.Args.OnlyFields({TEXT("name"), TEXT("class"), TEXT("attach_to"), TEXT("attach_socket")})
		|| !ReadName(Call, TEXT("name"), Name) || !ReadClass(Call, TEXT("class"), Class)
		|| !Call.Args.String(TEXT("attach_to"), AttachTo, false) || !Call.Args.String(TEXT("attach_socket"), Socket, false))
	{
		return false;
	}
	USimpleConstructionScript* SCS = GetSCS(Call, *Blueprint);
	if (SCS == nullptr)
	{
		return false;
	}
	if (!Class->IsChildOf<UActorComponent>() || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
	{
		return Call.Args.Fail(TEXT("BlueprintMember.InvalidComponentClass"),
			FString::Printf(TEXT("%s is not a concrete ActorComponent class."), *Class->GetPathName()), TEXT("class"));
	}
	if (!ValidateNewName(Call, *Blueprint, Name, TEXT("name")))
	{
		return false;
	}
	USCS_Node* Node = SCS->CreateNode(Class, Name);
	if (Node == nullptr || Node->GetVariableName() != Name)
	{
		return Call.Args.Fail(TEXT("BlueprintMember.InvalidName"), FString::Printf(TEXT("Component '%s' could not be created."), *Name.ToString()), TEXT("name"));
	}
	if (!AttachNode(Call, *Blueprint, *SCS, *Node, AttachTo) || !SetAttachSocket(Call, *Blueprint, *SCS, *Node, Socket))
	{
		return false;
	}
	NotifyStructural(*Blueprint);
	return true;
}

bool HandleComponentAttach(FCall& Call, FObjectBuilder& Result)
{
	UBlueprint* Blueprint = GetBlueprint(Call);
	FName Name;
	FString AttachTo;
	FString Socket;
	if (Blueprint == nullptr || !Call.Args.OnlyFields({TEXT("name"), TEXT("attach_to"), TEXT("attach_socket")})
		|| !ReadName(Call, TEXT("name"), Name) || !Call.Args.String(TEXT("attach_to"), AttachTo, false)
		|| !Call.Args.String(TEXT("attach_socket"), Socket, false))
	{
		return false;
	}
	USimpleConstructionScript* SCS = GetSCS(Call, *Blueprint);
	USCS_Node* Node = SCS ? SCS->FindSCSNode(Name) : nullptr;
	if (Node == nullptr || Node == SCS->GetDefaultSceneRootNode())
	{
		return SCS != nullptr && Call.Args.Fail(TEXT("BlueprintMember.ComponentNotFound"),
			FString::Printf(TEXT("No own component '%s'."), *Name.ToString()), TEXT("name"));
	}
	if (!AttachTo.IsEmpty())
	{
		USCS_Node* Parent = SCS->FindSCSNode(FName(*AttachTo));
		if (Parent == Node || (Parent != nullptr && IsDescendant(Node, Parent)))
		{
			return Call.Args.Fail(TEXT("BlueprintMember.InvalidAttach"), TEXT("A component cannot be attached to itself or its child."), TEXT("attach_to"));
		}
	}
	// Only the socket changes when the parent stays (the Details panel's Parent Socket): the component
	// keeps its place among its siblings.
	if (!AttachName(*Blueprint, *SCS, *Node).Equals(AttachTo, ESearchCase::IgnoreCase))
	{
		DetachNode(*SCS, *Node);
		if (!AttachNode(Call, *Blueprint, *SCS, *Node, AttachTo))
		{
			return false;
		}
	}
	if (!SetAttachSocket(Call, *Blueprint, *SCS, *Node, Socket))
	{
		return false;
	}
	SCS->ValidateSceneRootNodes();
	NotifyStructural(*Blueprint);
	return true;
}

bool HandleComponentRemove(FCall& Call, FObjectBuilder& Result)
{
	UBlueprint* Blueprint = GetBlueprint(Call);
	FName Name;
	FString Scope = TEXT("owned");
	if (Blueprint == nullptr || !Call.Args.OnlyFields({TEXT("name"), TEXT("scope")})
		|| !ReadName(Call, TEXT("name"), Name) || !Call.Args.String(TEXT("scope"), Scope, false))
	{
		return false;
	}
	if (Scope == TEXT("inherited"))
	{
		// Removing an inherited override reverts the component to the parent template.
		UInheritableComponentHandler* Handler = Blueprint->GetInheritableComponentHandler(false);
		USCS_Node* Inherited = FindInheritedNode(*Blueprint, Name);
		if (Handler != nullptr && Inherited != nullptr)
		{
			Handler->RemoveOverridenComponentTemplate(FComponentKey(Inherited));
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		}
		return true;
	}
	if (Scope != TEXT("owned"))
	{
		return Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("scope is owned or inherited."), TEXT("scope"));
	}
	USimpleConstructionScript* SCS = GetSCS(Call, *Blueprint);
	USCS_Node* Node = SCS ? SCS->FindSCSNode(Name) : nullptr;
	if (Node == nullptr || Node == SCS->GetDefaultSceneRootNode())
	{
		return SCS != nullptr && Call.Args.Fail(TEXT("BlueprintMember.ComponentNotFound"),
			FString::Printf(TEXT("No own component '%s'."), *Name.ToString()), TEXT("name"));
	}
	SCS->RemoveNodeAndPromoteChildren(Node);
	NotifyStructural(*Blueprint);
	return true;
}

// ---- properties ---------------------------------------------------------------------------

bool MemberHandlePropertySet(FCall& Call, FObjectBuilder& Result)
{
	UBlueprint* Blueprint = GetBlueprint(Call);
	FString Target, Path;
	bool bReset = false;
	if (Blueprint == nullptr
		|| !Call.Args.OnlyFields({TEXT("target"), TEXT("component"), TEXT("property"), TEXT("value"), TEXT("reset")})
		|| !Call.Args.String(TEXT("target"), Target) || !Call.Args.String(TEXT("property"), Path)
		|| !Call.Args.Bool(TEXT("reset"), bReset))
	{
		return false;
	}
	const FValue* Value = Call.Args.Find(TEXT("value"));
	if (!bReset && Value == nullptr)
	{
		return Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("Give a value or reset:true."), TEXT("value"));
	}
	UObject* Object = nullptr;
	const UObject* Baseline = nullptr;
	UClass* Generated = nullptr;
	if (Target == TEXT("blueprint"))
	{
		Object = Blueprint;
		Baseline = Blueprint->GetClass()->GetDefaultObject();
	}
	else if (Target == TEXT("class_defaults"))
	{
		CompileIfNeeded(*Blueprint);
		Generated = Blueprint->GeneratedClass;
		Object = Generated ? Generated->GetDefaultObject() : nullptr;
		Baseline = ParentDefaults(*Blueprint);
		if (Object == nullptr)
		{
			return MemberFail(Call, TEXT("BlueprintMember.InvalidValue"), TEXT("The Blueprint has no compiled class defaults."), TEXT("/target"));
		}
	}
	else if (Target == TEXT("component"))
	{
		const FValue* Address = nullptr;
		if (!Call.Args.Object(TEXT("component"), Address) || !ResolveComponentTarget(Call, *Blueprint, *Address, Object, Baseline))
		{
			return false;
		}
	}
	else
	{
		return Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("target is blueprint, class_defaults or component."), TEXT("target"));
	}
	const auto Filter = [Generated](const FProperty& Property) { return Generated == nullptr || Property.GetOwnerClass() != Generated; };
	if (Generated != nullptr)
	{
		FResolvedPropertyPath Resolved;
		FString Error;
		if (ResolvePropertyPath(*Object, Path, Resolved, Error) && Resolved.TopLevel->GetOwnerClass() == Generated)
		{
			return Call.Args.Fail(TEXT("BlueprintMember.UseVariables"),
				FString::Printf(TEXT("'%s' is a variable of this Blueprint; set its default in Variables."), *Path), TEXT("property"));
		}
	}
	if (!SetObjectProperty(Call, *Object, Baseline, Path, Value, bReset, Filter, Result))
	{
		return false;
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	return true;
}

// ---- class-specific settings ----------------------------------------------------------------

bool HandleSettingSet(FCall& Call, FObjectBuilder& Result)
{
	UBlueprint* Blueprint = GetBlueprint(Call);
	FString Name;
	if (Blueprint == nullptr || !Call.Args.OnlyFields({TEXT("name"), TEXT("value")}) || !Call.Args.String(TEXT("name"), Name))
	{
		return false;
	}
	const FValue* Value = Call.Args.Find(TEXT("value"));
	if (Value == nullptr)
	{
		return Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("Missing required field."), TEXT("value"));
	}
	const TSharedPtr<IAssetDocumentBlueprintSettingsAdapter> Settings = FindAssetDocumentBlueprintSettingsAdapter(Blueprint);
	if (!Settings.IsValid())
	{
		return MemberFail(Call, TEXT("BlueprintMember.SettingNotFound"), FString::Printf(
			TEXT("%s has no class-specific settings."), *Blueprint->GetClass()->GetName()), TEXT("/name"));
	}
	if (!Settings->SetSetting(*Blueprint, Name, *Value, Call.Error))
	{
		if (Call.Error.Code.IsEmpty())
		{
			Call.Error.Set(TEXT("BlueprintMember.InvalidValue"), FString::Printf(TEXT("Setting '%s' was rejected."), *Name), TEXT("/value"));
		}
		return false;
	}
	Result.Add(TEXT("settings"), Settings->QuerySettings(*Blueprint));
	return true;
}

TArray<FAssetDocumentPrimitiveDescriptor> BuildMemberDescriptors()
{
	return {
		MakeMemberDescriptor(TEXT("blueprint.member.query.v1"), false, TEXT("UBlueprint::NewVariables;USimpleConstructionScript::GetAllNodes;UInheritableComponentHandler")),
		MakeMemberDescriptor(TEXT("blueprint.member.parent.set.v1"), true, TEXT("UBlueprint::ParentClass;FBlueprintEditorUtils::RefreshAllNodes")),
		MakeMemberDescriptor(TEXT("blueprint.member.interface.add.v1"), true, TEXT("FBlueprintEditorUtils::ImplementNewInterface")),
		MakeMemberDescriptor(TEXT("blueprint.member.interface.remove.v1"), true, TEXT("FBlueprintEditorUtils::RemoveInterface")),
		MakeMemberDescriptor(TEXT("blueprint.member.variable.set.v1"), true, TEXT("FBlueprintEditorUtils::AddMemberVariable;FBlueprintEditorUtils::ChangeMemberVariableType;FBlueprintEditorUtils::SetBlueprintVariableMetaData")),
		MakeMemberDescriptor(TEXT("blueprint.member.variable.remove.v1"), true, TEXT("FBlueprintEditorUtils::RemoveMemberVariable")),
		MakeMemberDescriptor(TEXT("blueprint.member.component.add.v1"), true, TEXT("USimpleConstructionScript::CreateNode;USCS_Node::AddChildNode")),
		MakeMemberDescriptor(TEXT("blueprint.member.component.attach.v1"), true, TEXT("USCS_Node::AddChildNode;USCS_Node::SetParent")),
		MakeMemberDescriptor(TEXT("blueprint.member.component.remove.v1"), true, TEXT("USimpleConstructionScript::RemoveNodeAndPromoteChildren;UInheritableComponentHandler::RemoveOverridenComponentTemplate")),
		MakeMemberDescriptor(TEXT("blueprint.member.property.set.v1"), true, TEXT("UObject::PreEditChange;UObject::PostEditChangeProperty;UInheritableComponentHandler::CreateOverridenComponentTemplate")),
		MakeMemberDescriptor(TEXT("blueprint.member.setting.set.v1"), true, TEXT("IAssetDocumentBlueprintSettingsAdapter::SetSetting")),
	};
}

const FBinding BaseMemberBindings[] = {
	{TEXT("blueprint.member.query.v1"), &MemberHandleQuery},
	{TEXT("blueprint.member.parent.set.v1"), &HandleParentSet},
	{TEXT("blueprint.member.interface.add.v1"), &HandleInterfaceAdd},
	{TEXT("blueprint.member.interface.remove.v1"), &HandleInterfaceRemove},
	{TEXT("blueprint.member.variable.set.v1"), &HandleVariableSet},
	{TEXT("blueprint.member.variable.remove.v1"), &HandleVariableRemove},
	{TEXT("blueprint.member.component.add.v1"), &HandleComponentAdd},
	{TEXT("blueprint.member.component.attach.v1"), &HandleComponentAttach},
	{TEXT("blueprint.member.component.remove.v1"), &HandleComponentRemove},
	{TEXT("blueprint.member.property.set.v1"), &MemberHandlePropertySet},
	{TEXT("blueprint.member.setting.set.v1"), &HandleSettingSet},
};
} // namespace

FAssetDocumentPrimitiveDescriptor MakeBlueprintMemberDescriptor(const TCHAR* PrimitiveId, bool bMutating, const TCHAR* AuthoritativeApi)
{
	return MakeMemberDescriptor(PrimitiveId, bMutating, AuthoritativeApi);
}

UBlueprint* GetMemberBlueprint(FCall& Call)
{
	return GetBlueprint(Call);
}

void NotifyMemberStructural(UBlueprint& Blueprint)
{
	NotifyStructural(Blueprint);
}

bool ValidateNewMemberName(FCall& Call, UBlueprint& Blueprint, FName Name, FStringView Key)
{
	return ValidateNewName(Call, Blueprint, Name, Key);
}

TConstArrayView<FAssetDocumentPrimitiveDescriptor> GetBlueprintMemberPrimitiveDescriptors()
{
	static const TArray<FAssetDocumentPrimitiveDescriptor> Descriptors = []
	{
		TArray<FAssetDocumentPrimitiveDescriptor> Out = BuildMemberDescriptors();
		BlueprintDispatchers::AppendDescriptors(Out);
		BlueprintTimelines::AppendDescriptors(Out);
		BlueprintRenames::AppendDescriptors(Out);
		return Out;
	}();
	return Descriptors;
}

TConstArrayView<FBinding> GetBlueprintMemberPrimitiveBindings()
{
	static const TArray<FBinding> Bindings = []
	{
		TArray<FBinding> Out(BaseMemberBindings, UE_ARRAY_COUNT(BaseMemberBindings));
		BlueprintDispatchers::AppendBindings(Out);
		BlueprintTimelines::AppendBindings(Out);
		BlueprintRenames::AppendBindings(Out);
		return Out;
	}();
	return Bindings;
}
} // namespace UE::AssetDocument::Graph
