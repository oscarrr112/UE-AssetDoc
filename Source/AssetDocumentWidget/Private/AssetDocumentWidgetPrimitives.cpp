// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Widget Blueprint primitives (provider "umg", owner UMG). Each handler does what the
// Widget Blueprint editor does for the same edit (Palette drop, Hierarchy drag/delete,
// Details panel, "Is Variable", Navigation customization, property Bind menu); the
// raw editor-equivalent paths live in AssetDocumentWidgetRawEdits.cpp. There is no
// inverse code: Core rolls back the whole package snapshot.
//
// DTOs (all carry "schema_version": 1):
//   umg.tree.query.v1          {offset?, limit?, defaults?} -> {root, total, next_offset, widgets[]; at offset 0 also
//                                bindings[], inherited_named_slots[], desired_focus, palette_category,
//                                design_size{mode, width, height}}
//                               widget: {name, class, parent, slot_name, index, is_variable, capacity, named_slots[],
//                                properties{}, slot_class, slot{}, navigation{}; with defaults: true also
//                                property_defaults{}, slot_defaults{} (the Designer values at their class default)}
//   umg.tree.add.v1            {name, class, parent?, slot_name?, index?, is_variable?}
//   umg.tree.move.v1           {name, parent?, slot_name?, index?} | {name, detach: true}
//   umg.tree.remove.v1         {name, keep_references?}
//   umg.widget.rename.v1       {name, new_name} -> {name, dependents[]{package, kind}}  (the Hierarchy / Details
//                               name box; dependents: the other loaded Blueprints whose graphs the rename updated,
//                               left unsaved as in the editor; refused while one of them that references the widget
//                               has unsaved changes)
//   umg.widget.variable.set.v1 {name, is_variable}
//   umg.widget.property.set.v1 {name, target: "widget"|"slot", property, value? | reset: true}
//   umg.widget.navigation.set.v1 {name, navigation: {Up|Down|Left|Right|Next|Previous: {rule, widget_to_focus?},
//                                routing_policy?}}
//   umg.binding.set.v1         {widget, property, kind: "Function"|"Property", function? | source_path[]?}
//   umg.binding.remove.v1      {widget, property}
//   umg.desired_focus.set.v1   {widget}          ("" clears)
//   umg.palette_category.set.v1 {category}       ("" = the UUserWidget default)
//   umg.design_size.set.v1     {mode, width, height}
// Animation primitives: AssetDocumentWidgetAnimationPrimitives.cpp.

#include "AssetDocumentWidgetPrimitives.h"

#include "AssetDocumentEditorPickerValues.h"
#include "AssetDocumentGraphModel.h"
#include "AssetDocumentGraphPropertyCodec.h"
#include "AssetDocumentWidgetRawEdits.h"

#include "Animation/WidgetAnimation.h"
#include "Blueprint/UserWidget.h"
#include "WidgetBlueprintEditorUtils.h"
#include "Blueprint/WidgetNavigation.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"

namespace UE::AssetDocument::Graph::Widget
{
namespace
{
constexpr uint32 WidgetProviderVersion = 1;
constexpr int32 DefaultQueryLimit = 400;
} // namespace

namespace AssetDocumentWidgetPrimitivesLocal
{
bool QueryBindingChecks(FCall& Call, UWidgetBlueprint& Blueprint, FObjectBuilder& Result);
}

// ---- descriptors / shared helpers ------------------------------------------------------

FAssetDocumentPrimitiveDescriptor MakeDescriptor(const TCHAR* PrimitiveId, bool bMutating, const TCHAR* AuthoritativeApi)
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = PrimitiveId;
	Descriptor.Version = WidgetProviderVersion;
	Descriptor.ProviderId = ProviderId;
	Descriptor.Owner = EAssetDocumentPrimitiveOwner::UMG;
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
		TEXT("Widget.UnsupportedAsset"), TEXT("Widget.ClassNotFound"), TEXT("Widget.InvalidClass"), TEXT("Widget.InvalidName"),
		TEXT("Widget.NameInUse"), TEXT("Widget.NotFound"), TEXT("Widget.ParentNotFound"), TEXT("Widget.InvalidPlacement"),
		TEXT("Widget.CircularReference"), TEXT("Widget.NoSlot"), TEXT("Widget.PropertyNotFound"),
		TEXT("Widget.PropertyNotEditable"), TEXT("Widget.InvalidValue"), TEXT("Widget.InvalidNavigation"),
		TEXT("Widget.BindingTargetNotFound"), TEXT("Widget.BindingFunctionNotFound"), TEXT("Widget.InvalidBindingFunction"),
		TEXT("Widget.InvalidBindingSourcePath"), TEXT("Widget.BindingNotFound"), TEXT("Widget.AnimationNotFound"),
		TEXT("Widget.InvalidAnimationName"), TEXT("Widget.InvalidFrameRate"), TEXT("Widget.InvalidTrack"),
		TEXT("Widget.TrackNotFound"), TEXT("Widget.TrackReadOnly"), TEXT("Widget.InvalidKey")};
	Descriptor.RequiredConsumerIds = {TEXT("route.generic.v1")};
	Descriptor.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
	Descriptor.HandleArgumentSchemas.Add({TEXT("asset"), EAssetDocumentHandleKind::Asset,
		bMutating ? EAssetDocumentHandleAccess::Mutable : EAssetDocumentHandleAccess::ReadOnly, true});

	FAssetDocumentPrimitiveAdmissionEvidence& Evidence = Descriptor.Admission;
	Evidence.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
	Evidence.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper;
	Evidence.ExactEngineVersion = TEXT("5.7.4");
	Evidence.PhysicalOwner = EAssetDocumentPrimitiveOwner::UMG;
	Evidence.PublicModule = TEXT("AssetDocumentWidget");
	Evidence.PublicHeader = TEXT("AssetDocumentWidgetProvider.h");
	Evidence.ExportedAuthoritativeApi = AuthoritativeApi;
	Evidence.AuthorityContextId = TEXT("assetdocument.editor.game-thread.editor-ready.v1");
	Evidence.PrimaryOperationId = Descriptor.PrimitiveId;
	Evidence.IdentityLifetimeContractId = TEXT("identity.request-transaction-revision-bound.v1");
	Evidence.TransactionContractId = TEXT("transaction.descriptor-governed.v1");
	Evidence.RollbackContractId = TEXT("rollback.core-package-snapshot.v1");
	Evidence.CompletionContractId = Descriptor.PrimitiveId + TEXT(".completion.v1");
	Evidence.SaveReloadEvidenceId = TEXT("UEAssetDoc.WidgetBlueprint.Python");
	Evidence.FailureInjectionTestIds = {TEXT("UEAssetDoc.WidgetBlueprint.Python")};
	Evidence.VerificationTestIds = {TEXT("UEAssetDoc.WidgetBlueprint.Python")};
	Evidence.AuthorityFingerprint = FString::Printf(TEXT("ue-5.7.4|umg|%s|%s"), PrimitiveId, AuthoritativeApi);
	return Descriptor;
}

bool Fail(FCall& Call, FStringView Code, FStringView Message, FStringView Path)
{
	if (Call.Error.Code.IsEmpty())
	{
		Call.Error.Set(Code, Message, Path);
	}
	return false;
}

UWidgetBlueprint* GetWidgetBlueprint(FCall& Call)
{
	UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(&Call.Asset);
	if (Blueprint == nullptr || Blueprint->WidgetTree == nullptr)
	{
		Fail(Call, TEXT("Widget.UnsupportedAsset"), TEXT("Widget primitives edit Widget Blueprint assets."), TEXT("/HandleArguments/asset"));
		return nullptr;
	}
	return Blueprint;
}

UWidget* FindSourceWidget(UWidgetBlueprint& Blueprint, FName Name)
{
	if (Blueprint.WidgetTree == nullptr || Name.IsNone())
	{
		return nullptr;
	}
	return FindObjectFast<UWidget>(Blueprint.WidgetTree, Name);
}

bool IsDesignerEditable(const FProperty& Property)
{
	return Property.HasAnyPropertyFlags(CPF_Edit)
		&& !Property.HasAnyPropertyFlags(CPF_EditConst | CPF_Transient | CPF_Deprecated | CPF_DisableEditOnInstance
			| CPF_InstancedReference | CPF_ContainsInstancedReference | CPF_PersistentInstance);
}

double TicksToDisplay(int32 Ticks, const FFrameRate& Tick, const FFrameRate& Display)
{
	const double Frames = FFrameRate::TransformTime(FFrameTime(FFrameNumber(Ticks)), Tick, Display).AsDecimal();
	return FMath::RoundToDouble(Frames * 10000.0) / 10000.0;
}

int32 DisplayToTicks(double Frames, const FFrameRate& Tick, const FFrameRate& Display)
{
	return FFrameRate::TransformTime(FFrameTime::FromDecimal(Frames), Display, Tick).RoundToFrame().Value;
}

namespace
{
// ---- reading arguments ---------------------------------------------------------------------

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
		return ResolveClass(Path + TEXT(".") + FPackageName::GetShortName(Path));
	}
	return nullptr;
}

bool ReadName(FCall& Call, FStringView Key, FName& OutName, bool bRequired = true)
{
	FString Text;
	if (!Call.Args.String(Key, Text, bRequired))
	{
		return false;
	}
	if (Text.IsEmpty())
	{
		return bRequired ? Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("Name must not be empty."), Key) : true;
	}
	OutName = FName(*Text);
	return true;
}

UWidget* ReadWidget(FCall& Call, UWidgetBlueprint& Blueprint, FStringView Key, FName& OutName)
{
	if (!ReadName(Call, Key, OutName))
	{
		return nullptr;
	}
	UWidget* Widget = FindSourceWidget(Blueprint, OutName);
	if (Widget == nullptr)
	{
		Call.Args.Fail(TEXT("Widget.NotFound"), FString::Printf(TEXT("No widget named '%s' in the widget tree."), *OutName.ToString()), Key);
	}
	return Widget;
}

// Typed values of Designer-editable properties of Object that differ from Baseline (or, with
// bAtDefault, the ones equal to it).
FValue EncodeChanged(UObject& Object, const UObject* Baseline, bool bAtDefault = false)
{
	FObjectBuilder Builder;
	for (TFieldIterator<FProperty> It(Object.GetClass()); It; ++It)
	{
		const FProperty& Property = **It;
		if (!IsDesignerEditable(Property))
		{
			continue;
		}
		const bool bIdentical = Baseline != nullptr && Baseline->GetClass()->IsChildOf(Property.GetOwnerClass())
			&& Property.Identical_InContainer(&Object, Baseline, 0, PPF_None);
		if (bIdentical != bAtDefault)
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

const TCHAR* NavigationDirections[] = {TEXT("Up"), TEXT("Down"), TEXT("Left"), TEXT("Right"), TEXT("Next"), TEXT("Previous")};

FWidgetNavigationData& Direction(UWidgetNavigation& Navigation, int32 Index)
{
	FWidgetNavigationData* Data[] = {&Navigation.Up, &Navigation.Down, &Navigation.Left, &Navigation.Right, &Navigation.Next, &Navigation.Previous};
	return *Data[Index];
}

FValue EncodeNavigation(UWidgetNavigation& Navigation)
{
	FObjectBuilder Builder;
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(NavigationDirections); ++Index)
	{
		const FWidgetNavigationData& Data = Direction(Navigation, Index);
		if (Data.Rule == EUINavigationRule::Escape && Data.WidgetToFocus.IsNone())
		{
			continue;
		}
		FObjectBuilder Entry;
		Entry.AddString(TEXT("rule"), StaticEnum<EUINavigationRule>()->GetNameStringByValue(static_cast<int64>(Data.Rule)));
		if (!Data.WidgetToFocus.IsNone())
		{
			Entry.AddString(TEXT("widget_to_focus"), Data.WidgetToFocus.ToString());
		}
		Builder.Add(NavigationDirections[Index], Entry.Build());
	}
	if (Navigation.RoutingPolicy != GetDefault<UWidgetNavigation>()->RoutingPolicy)
	{
		Builder.AddString(TEXT("routing_policy"),
			StaticEnum<EWidgetNavigationRoutingPolicy>()->GetNameStringByValue(static_cast<int64>(Navigation.RoutingPolicy)));
	}
	if (Navigation.NavigationMethod.IsValid())
	{
		Builder.AddString(TEXT("navigation_method"), Navigation.NavigationMethod.GetScriptStruct()->GetPathName());
	}
	return Builder.Build();
}

FValue EncodeWidget(UWidgetBlueprint& Blueprint, UWidget& Widget, const FString& Parent, FName SlotName, int32 Index,
	bool bDefaults)
{
	FObjectBuilder Builder;
	Builder.AddString(TEXT("name"), Widget.GetName())
		.AddString(TEXT("class"), Widget.GetClass()->GetPathName())
		.AddString(TEXT("parent"), Parent)
		.AddString(TEXT("slot_name"), SlotName.IsNone() ? FString() : SlotName.ToString())
		.AddInt(TEXT("index"), Index)
		.AddBool(TEXT("is_variable"), Widget.bIsVariable)
		.AddString(TEXT("capacity"), Raw::ChildCapacity(Widget));
	TArray<FString> NamedSlots;
	for (const FName Name : Raw::NamedSlotNames(Widget))
	{
		NamedSlots.Add(Name.ToString());
	}
	if (!NamedSlots.IsEmpty())
	{
		Builder.Add(TEXT("named_slots"), MakeStringArray(NamedSlots));
	}
	Builder.Add(TEXT("properties"), EncodeChanged(Widget, Widget.GetClass()->GetDefaultObject()));
	if (Widget.Slot != nullptr)
	{
		Builder.AddString(TEXT("slot_class"), Widget.Slot->GetClass()->GetPathName());
		Builder.Add(TEXT("slot"), EncodeChanged(*Widget.Slot, Widget.Slot->GetClass()->GetDefaultObject()));
	}
	if (bDefaults)
	{
		// What a Diff shows as the current value of a property the document sets but the asset
		// holds at its default (the rows above list changed values only).
		Builder.Add(TEXT("property_defaults"), EncodeChanged(Widget, Widget.GetClass()->GetDefaultObject(), true));
		if (Widget.Slot != nullptr)
		{
			Builder.Add(TEXT("slot_defaults"), EncodeChanged(*Widget.Slot, Widget.Slot->GetClass()->GetDefaultObject(), true));
		}
	}
	if (Widget.Navigation != nullptr)
	{
		Builder.Add(TEXT("navigation"), EncodeNavigation(*Widget.Navigation));
	}
	return Builder.Build();
}

FValue EncodeBinding(const FDelegateEditorBinding& Binding)
{
	FObjectBuilder Builder;
	Builder.AddString(TEXT("widget"), Binding.ObjectName)
		.AddString(TEXT("property"), Binding.PropertyName.ToString())
		.AddString(TEXT("kind"), Binding.Kind == EBindingKind::Function ? TEXT("Function") : TEXT("Property"));
	if (Binding.Kind == EBindingKind::Function)
	{
		Builder.AddString(TEXT("function"), Binding.FunctionName.ToString());
	}
	else
	{
		TArray<FString> Path;
		for (const FEditorPropertyPathSegment& Segment : Binding.SourcePath.Segments)
		{
			Path.Add(Segment.GetMemberName().ToString());
		}
		if (Path.IsEmpty() && !Binding.SourceProperty.IsNone())
		{
			Path.Add(Binding.SourceProperty.ToString());
		}
		Builder.Add(TEXT("source_path"), MakeStringArray(Path));
	}
	return Builder.Build();
}

// ---- tree query ------------------------------------------------------------------------------

FName DesiredFocus(const UWidgetBlueprint& Blueprint);

struct FRow
{
	UWidget* Widget = nullptr;
	FString Parent;
	FName SlotName;
	int32 Index = 0;
};

void CollectRows(UWidget& Widget, const FString& Parent, FName SlotName, int32 Index, TArray<FRow>& OutRows)
{
	OutRows.Add({&Widget, Parent, SlotName, Index});
	TArray<TPair<FName, UWidget*>> Children;
	Raw::GetChildren(Widget, Children);
	int32 PanelIndex = 0;
	for (const TPair<FName, UWidget*>& Child : Children)
	{
		CollectRows(*Child.Value, Widget.GetName(), Child.Key, Child.Key.IsNone() ? PanelIndex++ : 0, OutRows);
	}
}

bool HandleTreeQuery(FCall& Call, FObjectBuilder& Result)
{
	UWidgetBlueprint* Blueprint = GetWidgetBlueprint(Call);
	if (Blueprint != nullptr && Call.Args.Has(TEXT("check_bindings")))
	{
		return AssetDocumentWidgetPrimitivesLocal::QueryBindingChecks(Call, *Blueprint, Result);
	}
	int64 Offset = 0;
	int64 Limit = DefaultQueryLimit;
	bool bDefaults = false;
	if (Blueprint == nullptr || !Call.Args.OnlyFields({TEXT("offset"), TEXT("limit"), TEXT("defaults")})
		|| !Call.Args.Int(TEXT("offset"), Offset, false) || !Call.Args.Int(TEXT("limit"), Limit, false)
		|| !Call.Args.Bool(TEXT("defaults"), bDefaults, false))
	{
		return false;
	}
	if (Offset < 0 || Limit < 1)
	{
		return Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("offset >= 0 and limit >= 1."), TEXT("offset"));
	}
	UWidgetTree* Tree = Blueprint->WidgetTree;
	TArray<FRow> Rows;
	if (Tree->RootWidget != nullptr)
	{
		CollectRows(*Tree->RootWidget, FString(), NAME_None, 0, Rows);
	}
	TArray<FName> SlotNames;
	Tree->NamedSlotBindings.GetKeys(SlotNames);
	SlotNames.Sort(FNameLexicalLess());
	for (const FName SlotName : SlotNames)
	{
		if (UWidget* Content = Tree->NamedSlotBindings.FindRef(SlotName))
		{
			CollectRows(*Content, FString(), SlotName, 0, Rows);
		}
	}
	Result.AddString(TEXT("root"), Tree->RootWidget != nullptr ? Tree->RootWidget->GetName() : FString())
		.AddInt(TEXT("total"), Rows.Num());
	int32 Budget = QueryValueNodeBudget;
	if (Offset == 0)
	{
		TArray<FDelegateEditorBinding> Sorted = Blueprint->Bindings;
		Sorted.Sort([](const FDelegateEditorBinding& A, const FDelegateEditorBinding& B)
		{
			return A.ObjectName != B.ObjectName ? A.ObjectName < B.ObjectName : A.PropertyName.LexicalLess(B.PropertyName);
		});
		TArray<FValue> Bindings;
		for (const FDelegateEditorBinding& Binding : Sorted)
		{
			Bindings.Add(EncodeBinding(Binding));
		}
		FValue BindingsValue = MakeArray(MoveTemp(Bindings));
		Budget -= CountValueNodes(BindingsValue);
		Result.Add(TEXT("bindings"), MoveTemp(BindingsValue));
		TArray<FString> Inherited;
		if (Blueprint->GeneratedClass != nullptr)
		{
			for (const FName Name : Blueprint->GetInheritedAvailableNamedSlots())
			{
				Inherited.Add(Name.ToString());
			}
		}
		Result.Add(TEXT("inherited_named_slots"), MakeStringArray(Inherited));
		const FName Focus = DesiredFocus(*Blueprint);
		Result.AddString(TEXT("desired_focus"), Focus.IsNone() ? FString() : Focus.ToString());
		Result.AddString(TEXT("palette_category"), Raw::GetPaletteCategory(*Blueprint));
		Raw::FDesignSize Size;
		if (Raw::GetDesignSize(*Blueprint, Size))
		{
			Result.Add(TEXT("design_size"), FObjectBuilder().AddString(TEXT("mode"), Size.Mode)
				.AddInt(TEXT("width"), Size.Width).AddInt(TEXT("height"), Size.Height).Build());
		}
	}
	TArray<FValue> Widgets;
	int64 Next = Offset;
	for (; Next < Rows.Num() && Next < Offset + Limit; ++Next)
	{
		const FRow& Row = Rows[Next];
		FValue Encoded = EncodeWidget(*Blueprint, *Row.Widget, Row.Parent, Row.SlotName, Row.Index, bDefaults);
		const int32 Nodes = CountValueNodes(Encoded);
		if (!Widgets.IsEmpty() && Nodes > Budget)
		{
			break;
		}
		Budget -= Nodes;
		Widgets.Add(MoveTemp(Encoded));
	}
	Result.Add(TEXT("widgets"), MakeArray(MoveTemp(Widgets)));
	Result.AddInt(TEXT("next_offset"), Next < Rows.Num() ? Next : -1);
	return true;
}

// ---- tree edits --------------------------------------------------------------------------------

bool ReadPlacement(FCall& Call, UWidgetBlueprint& Blueprint, UWidget*& OutParent, FName& OutSlot, int32& OutIndex)
{
	FName ParentName;
	FName SlotName;
	int64 Index = INDEX_NONE;
	if (!ReadName(Call, TEXT("parent"), ParentName, false) || !ReadName(Call, TEXT("slot_name"), SlotName, false)
		|| !Call.Args.Int(TEXT("index"), Index, false))
	{
		return false;
	}
	OutParent = nullptr;
	if (!ParentName.IsNone())
	{
		OutParent = FindSourceWidget(Blueprint, ParentName);
		if (OutParent == nullptr)
		{
			return Call.Args.Fail(TEXT("Widget.ParentNotFound"), FString::Printf(TEXT("No widget named '%s' to place into."), *ParentName.ToString()), TEXT("parent"));
		}
	}
	OutSlot = SlotName;
	OutIndex = static_cast<int32>(Index);
	return true;
}

bool ValidateNewWidgetName(FCall& Call, UWidgetBlueprint& Blueprint, FName Name)
{
	const FString Text = Name.ToString();
	FText Reason;
	if (!FName::IsValidXName(Text, INVALID_OBJECTNAME_CHARACTERS, &Reason) || Text.Contains(TEXT(" ")))
	{
		return Call.Args.Fail(TEXT("Widget.InvalidName"), FString::Printf(TEXT("'%s' is not a valid widget name: %s"), *Text,
			Reason.IsEmpty() ? TEXT("spaces are not allowed") : *Reason.ToString()), TEXT("name"));
	}
	if (UWidget* Existing = FindSourceWidget(Blueprint, Name))
	{
		if (Blueprint.GetAllSourceWidgets().Contains(Existing))
		{
			return Call.Args.Fail(TEXT("Widget.NameInUse"), FString::Printf(TEXT("A widget named '%s' already exists."), *Text), TEXT("name"));
		}
		// A stale, unreachable template: move it out of the way as DeleteWidgets would have.
		Existing->Rename(nullptr, GetTransientPackage());
	}
	const bool bVariable = FBlueprintEditorUtils::FindNewVariableIndex(&Blueprint, Name) != INDEX_NONE;
	const bool bAnimation = Blueprint.Animations.ContainsByPredicate([Name](const UWidgetAnimation* Animation) { return Animation && Animation->GetFName() == Name; });
	TArray<UEdGraph*> Graphs;
	Blueprint.GetAllGraphs(Graphs);
	const bool bGraph = Graphs.ContainsByPredicate([Name](const UEdGraph* Graph) { return Graph && Graph->GetFName() == Name; });
	if (bVariable || bAnimation || bGraph)
	{
		return Call.Args.Fail(TEXT("Widget.NameInUse"), FString::Printf(TEXT("'%s' is already a %s of this Blueprint."), *Text,
			bVariable ? TEXT("variable") : bAnimation ? TEXT("animation") : TEXT("graph")), TEXT("name"));
	}
	return true;
}

bool HandleTreeAdd(FCall& Call, FObjectBuilder& Result)
{
	UWidgetBlueprint* Blueprint = GetWidgetBlueprint(Call);
	FName Name;
	FString ClassPath;
	bool bIsVariable = false;
	if (Blueprint == nullptr
		|| !Call.Args.OnlyFields({TEXT("name"), TEXT("class"), TEXT("parent"), TEXT("slot_name"), TEXT("index"), TEXT("is_variable")})
		|| !ReadName(Call, TEXT("name"), Name) || !Call.Args.String(TEXT("class"), ClassPath)
		|| !Call.Args.Bool(TEXT("is_variable"), bIsVariable))
	{
		return false;
	}
	UClass* Class = ResolveClass(ClassPath);
	if (Class == nullptr)
	{
		return Call.Args.Fail(TEXT("Widget.ClassNotFound"), FString::Printf(TEXT("Widget class '%s' does not resolve."), *ClassPath), TEXT("class"));
	}
	if (!Class->IsChildOf(UWidget::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
	{
		return Call.Args.Fail(TEXT("Widget.InvalidClass"), FString::Printf(TEXT("%s is not a concrete widget class."), *Class->GetPathName()), TEXT("class"));
	}
	if (Blueprint->GeneratedClass != nullptr && Class->IsChildOf(Blueprint->GeneratedClass))
	{
		return Call.Args.Fail(TEXT("Widget.CircularReference"), TEXT("A Widget Blueprint cannot contain itself."), TEXT("class"));
	}
	UWidget* Parent = nullptr;
	FName SlotName;
	int32 Index = INDEX_NONE;
	if (!ValidateNewWidgetName(Call, *Blueprint, Name) || !ReadPlacement(Call, *Blueprint, Parent, SlotName, Index))
	{
		return false;
	}
	UWidgetTree* Tree = Blueprint->WidgetTree;
	Tree->SetFlags(RF_Transactional);
	Tree->Modify();
	UWidget* Widget = Tree->ConstructWidget<UWidget>(Class, Name);
	if (Widget == nullptr)
	{
		return Call.Args.Fail(TEXT("Widget.InvalidClass"), FString::Printf(TEXT("%s could not be constructed."), *Class->GetPathName()), TEXT("class"));
	}
	if (UUserWidget* UserWidget = Cast<UUserWidget>(Widget); UserWidget != nullptr && !Blueprint->IsWidgetFreeFromCircularReferences(UserWidget))
	{
		Widget->Rename(nullptr, GetTransientPackage());
		return Call.Args.Fail(TEXT("Widget.CircularReference"), FString::Printf(TEXT("%s would reference this Widget Blueprint in a cycle."), *Class->GetPathName()), TEXT("class"));
	}
	if (Call.Args.Has(TEXT("is_variable")))
	{
		Widget->bIsVariable = bIsVariable;
	}
	FString Error;
	if (!Raw::Place(*Blueprint, *Widget, Parent, SlotName, Index, Error))
	{
		Widget->Rename(nullptr, GetTransientPackage());
		return Call.Args.Fail(TEXT("Widget.InvalidPlacement"), Error, Parent != nullptr ? TEXT("parent") : TEXT("slot_name"));
	}
	// UEditorUtilityLibrary::AddSourceWidget / palette drop.
	Blueprint->OnVariableAdded(Widget->GetFName());
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Result.AddString(TEXT("name"), Widget->GetName());
	return true;
}

bool HandleTreeMove(FCall& Call, FObjectBuilder& Result)
{
	UWidgetBlueprint* Blueprint = GetWidgetBlueprint(Call);
	FName Name;
	bool bDetach = false;
	if (Blueprint == nullptr
		|| !Call.Args.OnlyFields({TEXT("name"), TEXT("parent"), TEXT("slot_name"), TEXT("index"), TEXT("detach")})
		|| !Call.Args.Bool(TEXT("detach"), bDetach))
	{
		return false;
	}
	UWidget* Widget = ReadWidget(Call, *Blueprint, TEXT("name"), Name);
	if (Widget == nullptr)
	{
		return false;
	}
	if (bDetach)
	{
		if (Call.Args.Has(TEXT("parent")) || Call.Args.Has(TEXT("slot_name")) || Call.Args.Has(TEXT("index")))
		{
			return Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("detach takes no placement."), TEXT("detach"));
		}
		Blueprint->WidgetTree->Modify();
		if (Raw::Detach(*Blueprint, *Widget))
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		}
		return true;
	}
	UWidget* Parent = nullptr;
	FName SlotName;
	int32 Index = INDEX_NONE;
	if (!ReadPlacement(Call, *Blueprint, Parent, SlotName, Index))
	{
		return false;
	}
	Blueprint->WidgetTree->Modify();
	FString Error;
	if (!Raw::Place(*Blueprint, *Widget, Parent, SlotName, Index, Error))
	{
		return Call.Args.Fail(TEXT("Widget.InvalidPlacement"), Error, Parent != nullptr ? TEXT("parent") : TEXT("slot_name"));
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	return true;
}

bool HandleTreeRemove(FCall& Call, FObjectBuilder& Result)
{
	UWidgetBlueprint* Blueprint = GetWidgetBlueprint(Call);
	FName Name;
	bool bKeepReferences = false;
	if (Blueprint == nullptr || !Call.Args.OnlyFields({TEXT("name"), TEXT("keep_references")})
		|| !Call.Args.Bool(TEXT("keep_references"), bKeepReferences))
	{
		return false;
	}
	UWidget* Widget = ReadWidget(Call, *Blueprint, TEXT("name"), Name);
	if (Widget == nullptr)
	{
		return false;
	}
	Raw::DeleteWidget(*Blueprint, *Widget, bKeepReferences);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	return true;
}

// Loaded Blueprints other than Blueprint whose graphs FBlueprintEditorUtils::ReplaceVariableReferences
// visits (FindDependentBlueprints), with whether a node of theirs names the variable.
struct FRenameDependent
{
	UBlueprint* Blueprint = nullptr;
	bool bReferences = false;
};

TArray<FRenameDependent> FindRenameDependents(UWidgetBlueprint& Blueprint, FName VariableName)
{
	TArray<UBlueprint*> Found;
	FBlueprintEditorUtils::FindDependentBlueprints(&Blueprint, Found);
	TArray<FRenameDependent> Out;
	for (UBlueprint* Dependent : Found)
	{
		if (Dependent == nullptr || Dependent->GetOutermost() == Blueprint.GetOutermost())
		{
			continue;
		}
		FRenameDependent& Row = Out.AddDefaulted_GetRef();
		Row.Blueprint = Dependent;
		TArray<UEdGraph*> Graphs;
		Dependent->GetAllGraphs(Graphs);
		for (int32 Index = 0; Index < Graphs.Num() && !Row.bReferences; ++Index)
		{
			for (const UEdGraphNode* Node : Graphs[Index]->Nodes)
			{
				const UK2Node* K2Node = Cast<UK2Node>(Node);
				if (K2Node != nullptr && K2Node->ReferencesVariable(VariableName, nullptr))
				{
					Row.bReferences = true;
					break;
				}
			}
		}
	}
	return Out;
}

bool HandleWidgetRename(FCall& Call, FObjectBuilder& Result)
{
	UWidgetBlueprint* Blueprint = GetWidgetBlueprint(Call);
	FString NewText;
	if (Blueprint == nullptr || !Call.Args.OnlyFields({TEXT("name"), TEXT("new_name")}) || !Call.Args.String(TEXT("new_name"), NewText))
	{
		return false;
	}
	FName Name;
	UWidget* Widget = ReadWidget(Call, *Blueprint, TEXT("name"), Name);
	if (Widget == nullptr)
	{
		return false;
	}
	const FString OldText = Widget->GetName();
	// FString::operator== ignores case, so the no-change test compares case-sensitively.
	if (OldText.Equals(NewText, ESearchCase::CaseSensitive))
	{
		Result.AddString(TEXT("name"), OldText);
		Result.Add(TEXT("dependents"), MakeArray(TArray<FValue>()));
		return true;
	}
	// The editor's name box (FWidgetBlueprintEditorUtils::RenameWidget): FKismetNameValidator with the widget's own
	// name as the existing name (so a case-only change is Ok), or a BindWidget property of the parent class that
	// the widget's class can fill.
	const FName NewName(*NewText);
	FKismetNameValidator Validator(Blueprint, Widget->GetFName());
	bool bValidName = Validator.IsValid(NewName) == EValidatorResult::Ok;
	if (!bValidName && Blueprint->ParentClass != nullptr)
	{
		const FObjectPropertyBase* Existing = CastField<FObjectPropertyBase>(Blueprint->ParentClass->FindPropertyByName(NewName));
		bValidName = Existing != nullptr && FWidgetBlueprintEditorUtils::IsBindWidgetProperty(Existing) && Widget->IsA(Existing->PropertyClass);
	}
	if (!bValidName)
	{
		return Call.Args.Fail(TEXT("Widget.InvalidName"), FString::Printf(TEXT("'%s' is not a valid widget name here (it is "
			"empty, has invalid characters or names another widget, variable or function)."), *NewText), TEXT("new_name"));
	}
	// ReplaceVariableReferences renames the references in the loaded Blueprints that depend on this one and marks them
	// modified. One with unsaved changes could not be restored from its file if the apply failed: refuse first.
	const TArray<FRenameDependent> Dependents = FindRenameDependents(*Blueprint, Widget->GetFName());
	TArray<FString> Unsaved;
	TSet<const UPackage*> DirtyBefore;
	for (const FRenameDependent& Dependent : Dependents)
	{
		const UPackage* Package = Dependent.Blueprint->GetOutermost();
		if (Package->IsDirty())
		{
			DirtyBefore.Add(Package);
			if (Dependent.bReferences)
			{
				Unsaved.Add(Package->GetName());
			}
		}
	}
	if (!Unsaved.IsEmpty())
	{
		return Call.Args.Fail(TEXT("Widget.DependentHasUnsavedChanges"), FString::Printf(TEXT("Renaming '%s' makes the editor rename "
			"its references in the loaded Blueprints that use it, and %s %s unsaved changes that could not be restored if the "
			"apply failed. Save (or revert) %s first, then apply again."), *OldText, *FString::Join(Unsaved, TEXT(", ")),
			Unsaved.Num() == 1 ? TEXT("has") : TEXT("have"), Unsaved.Num() == 1 ? TEXT("it") : TEXT("them")), TEXT("name"));
	}
	Raw::RenameWidget(*Blueprint, *Widget, NewName);
	if (!Widget->GetName().Equals(NewText, ESearchCase::CaseSensitive))
	{
		Call.Error.Set(TEXT("Widget.RenameFailed"), FString::Printf(TEXT("The editor did not rename widget '%s' to '%s' (now '%s')."),
			*OldText, *NewText, *Widget->GetName()), TEXT("/name"));
		return false;
	}
	TArray<FValue> Updated;
	for (const FRenameDependent& Dependent : Dependents)
	{
		const UPackage* Package = Dependent.Blueprint->GetOutermost();
		if (Package->IsDirty() && !DirtyBefore.Contains(Package))
		{
			Updated.Add(FObjectBuilder().AddString(TEXT("package"), Package->GetName()).AddString(TEXT("kind"), TEXT("Blueprint")).Build());
		}
	}
	Result.AddString(TEXT("name"), Widget->GetName());
	Result.Add(TEXT("dependents"), MakeArray(MoveTemp(Updated)));
	return true;
}

bool HandleVariableSet(FCall& Call, FObjectBuilder& Result)
{
	UWidgetBlueprint* Blueprint = GetWidgetBlueprint(Call);
	FName Name;
	bool bIsVariable = false;
	if (Blueprint == nullptr || !Call.Args.OnlyFields({TEXT("name"), TEXT("is_variable")})
		|| !Call.Args.Bool(TEXT("is_variable"), bIsVariable, true))
	{
		return false;
	}
	UWidget* Widget = ReadWidget(Call, *Blueprint, TEXT("name"), Name);
	if (Widget == nullptr)
	{
		return false;
	}
	if (Widget->bIsVariable != bIsVariable)
	{
		// SWidgetDetailsView::HandleIsVariableChanged.
		Widget->Modify();
		Widget->bIsVariable = bIsVariable;
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}
	return true;
}

bool HandlePropertySet(FCall& Call, FObjectBuilder& Result)
{
	UWidgetBlueprint* Blueprint = GetWidgetBlueprint(Call);
	FName Name;
	FString Target = TEXT("widget");
	FString Path;
	bool bReset = false;
	if (Blueprint == nullptr
		|| !Call.Args.OnlyFields({TEXT("name"), TEXT("target"), TEXT("property"), TEXT("value"), TEXT("reset")})
		|| !Call.Args.String(TEXT("target"), Target, false) || !Call.Args.String(TEXT("property"), Path)
		|| !Call.Args.Bool(TEXT("reset"), bReset))
	{
		return false;
	}
	UWidget* Widget = ReadWidget(Call, *Blueprint, TEXT("name"), Name);
	if (Widget == nullptr)
	{
		return false;
	}
	const FValue* Value = Call.Args.Find(TEXT("value"));
	if (!bReset && Value == nullptr)
	{
		return Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("Give a value or reset:true."), TEXT("value"));
	}
	UObject* Object = Widget;
	if (Target == TEXT("slot"))
	{
		Object = Widget->Slot;
		if (Object == nullptr)
		{
			return Call.Args.Fail(TEXT("Widget.NoSlot"), FString::Printf(TEXT("'%s' is not in a panel and has no slot."), *Name.ToString()), TEXT("target"));
		}
	}
	else if (Target != TEXT("widget"))
	{
		return Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("target is widget or slot."), TEXT("target"));
	}
	const UObject* Baseline = Object->GetClass()->GetDefaultObject();
	FResolvedPropertyPath Resolved;
	FString Error;
	if (!ResolvePropertyPath(*Object, Path, Resolved, Error))
	{
		return Call.Args.Fail(TEXT("Widget.PropertyNotFound"), Error, TEXT("property"));
	}
	if (!IsDesignerEditable(*Resolved.TopLevel))
	{
		return Call.Args.Fail(TEXT("Widget.PropertyNotEditable"),
			FString::Printf(TEXT("%s is not editable in the Designer Details panel of %s."), *Path, *Object->GetClass()->GetName()), TEXT("property"));
	}
	Object->SetFlags(RF_Transactional);
	Object->Modify();
	Object->PreEditChange(Resolved.TopLevel);
	bool bOk = true;
	if (bReset)
	{
		FResolvedPropertyPath BaselineResolved;
		if (ResolvePropertyPath(*const_cast<UObject*>(Baseline), Path, BaselineResolved, Error) && BaselineResolved.Leaf == Resolved.Leaf)
		{
			Resolved.Leaf->CopyCompleteValue(Resolved.LeafPtr, BaselineResolved.LeafPtr);
		}
		else
		{
			Resolved.Leaf->InitializeValue(Resolved.LeafPtr);
		}
	}
	else
	{
		// The row handles and keys the property holds now are kept; a written one must be offered by the Designer
		// Details panel's pickers.
		const TArray<FString> Kept = EditorPickerValues::CollectPicks(*Resolved.Leaf, Resolved.LeafPtr);
		EditorPickerValues::FInvalidPick Invalid;
		if (!DecodeValue(*Resolved.Leaf, Resolved.LeafPtr, *Value, Error))
		{
			bOk = Call.Args.Fail(TEXT("Widget.InvalidValue"), Error, TEXT("value"));
		}
		else if (EditorPickerValues::FindInvalidPick(*Resolved.Leaf, Resolved.LeafPtr, Kept, Invalid))
		{
			bOk = Call.Args.Fail(Invalid.Code, FString::Printf(TEXT("'%s'%s: %s"), *Path, *Invalid.ValuePath, *Invalid.Message),
				TEXT("value"));
		}
	}
	FPropertyChangedEvent Event(Resolved.TopLevel, EPropertyChangeType::ValueSet);
	Object->PostEditChangeProperty(Event);
	if (!bOk)
	{
		return false;
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	Result.Add(TEXT("value"), EncodePropertyValue(*Resolved.Leaf, Resolved.LeafPtr));
	return true;
}

bool HandleNavigationSet(FCall& Call, FObjectBuilder& Result)
{
	UWidgetBlueprint* Blueprint = GetWidgetBlueprint(Call);
	FName Name;
	const FValue* Navigation = nullptr;
	if (Blueprint == nullptr || !Call.Args.OnlyFields({TEXT("name"), TEXT("navigation")})
		|| !Call.Args.Object(TEXT("navigation"), Navigation))
	{
		return false;
	}
	UWidget* Widget = ReadWidget(Call, *Blueprint, TEXT("name"), Name);
	if (Widget == nullptr)
	{
		return false;
	}
	FReader Reader = Call.Args.Child(TEXT("navigation"), *Navigation);
	if (!Reader.OnlyFields({TEXT("Up"), TEXT("Down"), TEXT("Left"), TEXT("Right"), TEXT("Next"), TEXT("Previous"), TEXT("routing_policy")}))
	{
		return false;
	}
	// Validate everything before touching the widget.
	EUINavigationRule Rules[UE_ARRAY_COUNT(NavigationDirections)];
	FName Targets[UE_ARRAY_COUNT(NavigationDirections)];
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(NavigationDirections); ++Index)
	{
		Rules[Index] = EUINavigationRule::Escape;
		const FValue* Entry = nullptr;
		if (!Reader.Object(NavigationDirections[Index], Entry, false))
		{
			return false;
		}
		if (Entry == nullptr)
		{
			continue;
		}
		FReader EntryReader = Reader.Child(NavigationDirections[Index], *Entry);
		FString Rule;
		FString Focus;
		if (!EntryReader.OnlyFields({TEXT("rule"), TEXT("widget_to_focus")}) || !EntryReader.String(TEXT("rule"), Rule)
			|| !EntryReader.String(TEXT("widget_to_focus"), Focus, false))
		{
			return false;
		}
		const int64 RuleValue = StaticEnum<EUINavigationRule>()->GetValueByNameString(Rule);
		if (RuleValue == INDEX_NONE)
		{
			return EntryReader.Fail(TEXT("Widget.InvalidNavigation"),
				FString::Printf(TEXT("'%s' is not a navigation rule (Escape, Explicit, Wrap, Stop, Custom, CustomBoundary, Invalid)."), *Rule), TEXT("rule"));
		}
		Rules[Index] = static_cast<EUINavigationRule>(RuleValue);
		Targets[Index] = Focus.IsEmpty() ? NAME_None : FName(*Focus);
	}
	EWidgetNavigationRoutingPolicy Policy = GetDefault<UWidgetNavigation>()->RoutingPolicy;
	FString PolicyText;
	if (!Reader.String(TEXT("routing_policy"), PolicyText, false))
	{
		return false;
	}
	if (!PolicyText.IsEmpty())
	{
		const int64 PolicyValue = StaticEnum<EWidgetNavigationRoutingPolicy>()->GetValueByNameString(PolicyText);
		if (PolicyValue == INDEX_NONE)
		{
			return Reader.Fail(TEXT("Widget.InvalidNavigation"), FString::Printf(TEXT("'%s' is not a routing policy."), *PolicyText), TEXT("routing_policy"));
		}
		Policy = static_cast<EWidgetNavigationRoutingPolicy>(PolicyValue);
	}
	// FWidgetNavigationCustomization::SetNav.
	Widget->SetFlags(RF_Transactional);
	Widget->Modify();
	UWidgetNavigation* WidgetNavigation = Widget->Navigation;
	if (WidgetNavigation == nullptr)
	{
		WidgetNavigation = NewObject<UWidgetNavigation>(Widget);
		WidgetNavigation->SetFlags(RF_Transactional);
	}
	WidgetNavigation->Modify();
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(NavigationDirections); ++Index)
	{
		FWidgetNavigationData& Data = Direction(*WidgetNavigation, Index);
		Data.Rule = Rules[Index];
		Data.WidgetToFocus = Targets[Index];
	}
	WidgetNavigation->RoutingPolicy = Policy;
	Widget->Navigation = WidgetNavigation->IsDefaultNavigation() ? nullptr : WidgetNavigation;
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	return true;
}

// ---- property bindings ---------------------------------------------------------------------

FDelegateProperty* FindBindableDelegate(const UClass& WidgetClass, FName Property, bool& bOutRequiresPure)
{
	FDelegateProperty* Delegate = FindFProperty<FDelegateProperty>(&WidgetClass, FName(*(Property.ToString() + TEXT("Delegate"))));
	bOutRequiresPure = Delegate != nullptr;
	return Delegate != nullptr ? Delegate : FindFProperty<FDelegateProperty>(&WidgetClass, Property);
}

UFunction* FindBindingFunction(UWidgetBlueprint& Blueprint, FName Name)
{
	for (UClass* Class : {Blueprint.SkeletonGeneratedClass.Get(), Blueprint.GeneratedClass.Get(), Blueprint.ParentClass.Get()})
	{
		if (UFunction* Function = Class != nullptr ? Class->FindFunctionByName(Name, EIncludeSuperFlag::IncludeSuper) : nullptr)
		{
			return Function;
		}
	}
	return nullptr;
}

} // namespace

// Unity-safe per-file helpers: the binding signature check Apply runs (Widget.InvalidBindingFunction), described
// and also run by Validate (umg.tree.query.v1 check_bindings).
namespace AssetDocumentWidgetPrimitivesLocal
{
// A binding function's or a binding delegate's parameters as Blueprint pin types.
struct FBindingSignature
{
	TArray<FEdGraphPinType> Inputs;
	TArray<FEdGraphPinType> Outputs;
	bool bPure = false;
};

FBindingSignature DescribeBindingFunction(const UFunction& Function)
{
	FBindingSignature Signature;
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	for (TFieldIterator<FProperty> It(&Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		FEdGraphPinType Type;
		Schema->ConvertPropertyToPinType(*It, Type);
		const bool bOutput = It->HasAnyPropertyFlags(CPF_ReturnParm)
			|| (It->HasAnyPropertyFlags(CPF_OutParm) && !It->HasAnyPropertyFlags(CPF_ReferenceParm));
		(bOutput ? Signature.Outputs : Signature.Inputs).Add(Type);
	}
	Signature.bPure = Function.HasAnyFunctionFlags(FUNC_Const | FUNC_BlueprintPure);
	return Signature;
}

// An enum is a byte pin with its enum, whichever way the type was written.
FName BindingPinCategory(const FEdGraphPinType& Type)
{
	return Type.PinCategory == UEdGraphSchema_K2::PC_Enum ? UEdGraphSchema_K2::PC_Byte : Type.PinCategory;
}

bool SameBindingPinType(const FEdGraphPinType& A, const FEdGraphPinType& B)
{
	const bool bReal = A.PinCategory == UEdGraphSchema_K2::PC_Real;
	return BindingPinCategory(A) == BindingPinCategory(B) && A.PinSubCategoryObject == B.PinSubCategoryObject
		&& A.ContainerType == B.ContainerType && (!bReal || A.PinSubCategory == B.PinSubCategory);
}

// The type as a document's Type writes it: "float" is a double, "float32" a float.
FString BindingPinTypeText(const FEdGraphPinType& Type)
{
	FString Text;
	if (Type.PinCategory == UEdGraphSchema_K2::PC_Real)
	{
		Text = Type.PinSubCategory == UEdGraphSchema_K2::PC_Float ? TEXT("float32") : TEXT("double (float)");
	}
	else if (const UObject* Object = Type.PinSubCategoryObject.Get())
	{
		const FName Category = BindingPinCategory(Type);
		Text = Category == UEdGraphSchema_K2::PC_Struct || Category == UEdGraphSchema_K2::PC_Byte
			? Object->GetName() : FString::Printf(TEXT("%s %s"), *Object->GetName(), *Category.ToString());
	}
	else
	{
		Text = Type.PinCategory.ToString();
	}
	return Type.IsArray() ? TEXT("array of ") + Text : Type.IsSet() ? TEXT("set of ") + Text : Type.IsMap() ? TEXT("map of ") + Text : Text;
}

// The fix-it name of a type ("return float32").
FString BindingPinTypeFix(const FEdGraphPinType& Type)
{
	return Type.PinCategory == UEdGraphSchema_K2::PC_Real && Type.PinSubCategory != UEdGraphSchema_K2::PC_Float
		? FString(TEXT("float")) : BindingPinTypeText(Type);
}

FString BindingPinTypesText(const TArray<FEdGraphPinType>& Types)
{
	TArray<FString> Texts;
	for (const FEdGraphPinType& Type : Types)
	{
		Texts.Add(BindingPinTypeText(Type));
	}
	return Texts.IsEmpty() ? FString(TEXT("none")) : FString::Join(Texts, TEXT(", "));
}

// What differs between a function and the binding delegate, each with its fix ("`GetHealthPercent` returns double (float);
// `Percent` binds float32: return float32"); empty when they match.
FString DescribeBindingMismatch(const FBindingSignature& Function, const FBindingSignature& Delegate, bool bRequiresPure, FName Property)
{
	TArray<FString> Differences;
	if (Function.Outputs.Num() != Delegate.Outputs.Num())
	{
		Differences.Add(FString::Printf(TEXT("it has %d output(s) (%s); %s binds %d (%s): %s"), Function.Outputs.Num(),
			*BindingPinTypesText(Function.Outputs), *Property.ToString(), Delegate.Outputs.Num(), *BindingPinTypesText(Delegate.Outputs),
			Delegate.Outputs.Num() == 1 ? *FString::Printf(TEXT("give it one output, ReturnValue of type %s"), *BindingPinTypeFix(Delegate.Outputs[0]))
				: TEXT("match its outputs")));
	}
	else
	{
		for (int32 Index = 0; Index < Function.Outputs.Num(); ++Index)
		{
			if (!SameBindingPinType(Function.Outputs[Index], Delegate.Outputs[Index]))
			{
				Differences.Add(FString::Printf(TEXT("it returns %s; %s binds %s: return %s"), *BindingPinTypeText(Function.Outputs[Index]),
					*Property.ToString(), *BindingPinTypeText(Delegate.Outputs[Index]), *BindingPinTypeFix(Delegate.Outputs[Index])));
			}
		}
	}
	if (Function.Inputs.Num() != Delegate.Inputs.Num())
	{
		Differences.Add(FString::Printf(TEXT("it takes %d input(s) (%s); the binding passes %d (%s): %s"), Function.Inputs.Num(),
			*BindingPinTypesText(Function.Inputs), Delegate.Inputs.Num(), *BindingPinTypesText(Delegate.Inputs),
			Delegate.Inputs.IsEmpty() ? TEXT("remove its inputs") : TEXT("give it those inputs")));
	}
	else
	{
		for (int32 Index = 0; Index < Function.Inputs.Num(); ++Index)
		{
			if (!SameBindingPinType(Function.Inputs[Index], Delegate.Inputs[Index]))
			{
				Differences.Add(FString::Printf(TEXT("input %d is %s; the binding passes %s: make it %s"), Index,
					*BindingPinTypeText(Function.Inputs[Index]), *BindingPinTypeText(Delegate.Inputs[Index]), *BindingPinTypeFix(Delegate.Inputs[Index])));
			}
		}
	}
	if (bRequiresPure && !Function.bPure)
	{
		Differences.Add(FString::Printf(TEXT("it is not pure; a %s binding calls a pure function: set Signature Pure: true"), *Property.ToString()));
	}
	return Differences.IsEmpty() ? FString() : FString::Join(Differences, TEXT("; ")) + TEXT(".");
}

const FValue* FindBindingCheckField(const FValue& Object, FStringView Key)
{
	if (Object.Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return nullptr;
	}
	const int32 Index = Object.ObjectKeys.IndexOfByPredicate([Key](const FString& Candidate) { return Candidate == Key; });
	return Index == INDEX_NONE ? nullptr : &Object.Elements[Index];
}

FString BindingCheckString(const FValue& Object, FStringView Key)
{
	const FValue* Field = FindBindingCheckField(Object, Key);
	return Field != nullptr && Field->Kind == EAssetDocumentRuntimeValueKind::String ? Field->StringValue : FString();
}

// A document Signature ({inputs: [pin type], outputs: [pin type], pure}); false when a type does not read.
bool ReadBindingSignature(FCall& Call, const FValue& Value, FBindingSignature& OutSignature)
{
	for (const TCHAR* Key : {TEXT("inputs"), TEXT("outputs")})
	{
		const FValue* Types = FindBindingCheckField(Value, Key);
		for (int32 Index = 0; Types != nullptr && Types->Kind == EAssetDocumentRuntimeValueKind::Array && Index < Types->Elements.Num(); ++Index)
		{
			FEdGraphPinType Type;
			FReader TypeReader = Call.Args.Child(Key, Types->Elements[Index]);
			if (!ReadPinType(TypeReader, Type))
			{
				Call.Error = FAssetDocumentGraphError();
				return false;
			}
			(FCString::Strcmp(Key, TEXT("inputs")) == 0 ? OutSignature.Inputs : OutSignature.Outputs).Add(Type);
		}
	}
	const FValue* Pure = FindBindingCheckField(Value, TEXT("pure"));
	OutSignature.bPure = Pure != nullptr && Pure->Kind == EAssetDocumentRuntimeValueKind::Boolean && Pure->BooleanValue;
	return true;
}

// check_bindings: Validate's check of the document's Function bindings, as Apply's umg.binding.set.v1 checks them.
// Items: {at, widget, widget_class?, property, function, signature?} (signature: the document's function graph
// Signature, else the Blueprint's function); -> check_bindings[] {at, code, message}.
bool QueryBindingChecks(FCall& Call, UWidgetBlueprint& Blueprint, FObjectBuilder& Result)
{
	const FValue* Items = nullptr;
	if (!Call.Args.OnlyFields({TEXT("check_bindings")}) || !Call.Args.Array(TEXT("check_bindings"), Items))
	{
		return false;
	}
	TArray<FValue> Problems;
	const auto AddProblem = [&Problems](const FValue& Item, const TCHAR* Code, const FString& Message)
		{
			FObjectBuilder Problem;
			Problem.AddString(TEXT("at"), BindingCheckString(Item, TEXT("at")));
			Problem.AddString(TEXT("code"), Code);
			Problem.AddString(TEXT("message"), Message);
			Problems.Add(Problem.Build());
		};
	for (const FValue& Item : Items->Elements)
	{
		const FString WidgetName = BindingCheckString(Item, TEXT("widget"));
		const FName Property(*BindingCheckString(Item, TEXT("property")));
		const FName FunctionName(*BindingCheckString(Item, TEXT("function")));
		const FString ClassPath = BindingCheckString(Item, TEXT("widget_class"));
		const UClass* WidgetClass = ClassPath.IsEmpty() ? nullptr : FindObject<UClass>(nullptr, *ClassPath);
		if (WidgetClass == nullptr && !ClassPath.IsEmpty() && FPackageName::IsValidObjectPath(ClassPath))
		{
			WidgetClass = LoadObject<UClass>(nullptr, *ClassPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		if (WidgetClass == nullptr && Blueprint.WidgetTree != nullptr)
		{
			const UWidget* Live = Blueprint.WidgetTree->FindWidget(FName(*WidgetName));
			WidgetClass = Live != nullptr ? Live->GetClass() : nullptr;
		}
		if (WidgetClass == nullptr || Property.IsNone() || FunctionName.IsNone())
		{
			continue;  // Apply reports the widget
		}
		bool bRequiresPure = false;
		FDelegateProperty* Delegate = FindBindableDelegate(*WidgetClass, Property, bRequiresPure);
		if (Delegate == nullptr || Delegate->SignatureFunction == nullptr)
		{
			AddProblem(Item, TEXT("Widget.BindingTargetNotFound"), FString::Printf(TEXT("%s.%s cannot be bound (%s has no %sDelegate)."),
				*WidgetName, *Property.ToString(), *WidgetClass->GetName(), *Property.ToString()));
			continue;
		}
		FBindingSignature Function;
		if (const FValue* Signature = FindBindingCheckField(Item, TEXT("signature")))
		{
			if (!ReadBindingSignature(Call, *Signature, Function))
			{
				continue;  // Apply reports the type
			}
		}
		else if (const UFunction* Existing = FindBindingFunction(Blueprint, FunctionName))
		{
			Function = DescribeBindingFunction(*Existing);
		}
		else
		{
			continue;  // Apply reports the function
		}
		const FString Differences = DescribeBindingMismatch(Function, DescribeBindingFunction(*Delegate->SignatureFunction), bRequiresPure, Property);
		if (!Differences.IsEmpty())
		{
			AddProblem(Item, TEXT("Widget.InvalidBindingFunction"), FString::Printf(TEXT("'%s' cannot bind %s.%s (%s): %s"),
				*FunctionName.ToString(), *WidgetName, *Property.ToString(), *Delegate->SignatureFunction->GetName(), *Differences));
		}
	}
	Result.Add(TEXT("check_bindings"), MakeArray(MoveTemp(Problems)));
	return true;
}
} // namespace AssetDocumentWidgetPrimitivesLocal

namespace
{
bool HandleBindingSet(FCall& Call, FObjectBuilder& Result)
{
	UWidgetBlueprint* Blueprint = GetWidgetBlueprint(Call);
	FName WidgetName;
	FName PropertyName;
	FString Kind;
	if (Blueprint == nullptr
		|| !Call.Args.OnlyFields({TEXT("widget"), TEXT("property"), TEXT("kind"), TEXT("function"), TEXT("source_path")})
		|| !ReadName(Call, TEXT("property"), PropertyName) || !Call.Args.String(TEXT("kind"), Kind))
	{
		return false;
	}
	UWidget* Widget = ReadWidget(Call, *Blueprint, TEXT("widget"), WidgetName);
	if (Widget == nullptr)
	{
		return false;
	}
	bool bRequiresPure = false;
	FDelegateProperty* Delegate = FindBindableDelegate(*Widget->GetClass(), PropertyName, bRequiresPure);
	if (Delegate == nullptr)
	{
		return Call.Args.Fail(TEXT("Widget.BindingTargetNotFound"),
			FString::Printf(TEXT("%s.%s cannot be bound (no %sDelegate)."), *WidgetName.ToString(), *PropertyName.ToString(), *PropertyName.ToString()), TEXT("property"));
	}
	FDelegateEditorBinding Binding;
	Binding.ObjectName = WidgetName.ToString();
	Binding.PropertyName = PropertyName;
	if (Kind == TEXT("Function"))
	{
		FName FunctionName;
		if (Call.Args.Has(TEXT("source_path")) || !ReadName(Call, TEXT("function"), FunctionName))
		{
			return Call.Args.Has(TEXT("source_path"))
				? Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("Function bindings take function, not source_path."), TEXT("source_path"))
				: false;
		}
		UFunction* Function = FindBindingFunction(*Blueprint, FunctionName);
		if (Function == nullptr)
		{
			return Call.Args.Fail(TEXT("Widget.BindingFunctionNotFound"), FString::Printf(TEXT("No function '%s' on this Blueprint."), *FunctionName.ToString()), TEXT("function"));
		}
		if (!Function->IsSignatureCompatibleWith(Delegate->SignatureFunction, UFunction::GetDefaultIgnoredSignatureCompatibilityFlags() | CPF_ReturnParm)
			|| (bRequiresPure && !Function->HasAnyFunctionFlags(FUNC_Const | FUNC_BlueprintPure)))
		{
			// What differs, as Validate reports it (AssetDocumentWidgetPrimitivesLocal::DescribeBindingMismatch).
			using namespace AssetDocumentWidgetPrimitivesLocal;
			const FString Differences = DescribeBindingMismatch(DescribeBindingFunction(*Function), DescribeBindingFunction(*Delegate->SignatureFunction),
				bRequiresPure, PropertyName);
			return Call.Args.Fail(TEXT("Widget.InvalidBindingFunction"), FString::Printf(TEXT("'%s' cannot bind %s.%s (%s): %s"),
				*FunctionName.ToString(), *WidgetName.ToString(), *PropertyName.ToString(), *Delegate->SignatureFunction->GetName(),
				Differences.IsEmpty() ? TEXT("its parameters do not match the binding's.") : *Differences), TEXT("function"));
		}
		Binding.Kind = EBindingKind::Function;
		Binding.FunctionName = FunctionName;
		// SPropertyBinding::HandleAddFunctionBinding records the function graph's GUID.
		UBlueprint::GetGuidFromClassByFieldName<UFunction>(Blueprint->SkeletonGeneratedClass, FunctionName, Binding.MemberGuid);
	}
	else if (Kind == TEXT("Property"))
	{
		const FValue* Path = nullptr;
		if (Call.Args.Has(TEXT("function")) || !Call.Args.Array(TEXT("source_path"), Path))
		{
			return Call.Args.Has(TEXT("function"))
				? Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("Property bindings take source_path, not function."), TEXT("function"))
				: false;
		}
		if (Path->Elements.IsEmpty())
		{
			return Call.Args.Fail(TEXT("Widget.InvalidBindingSourcePath"), TEXT("source_path needs at least one member."), TEXT("source_path"));
		}
		UStruct* Container = Blueprint->SkeletonGeneratedClass != nullptr ? Blueprint->SkeletonGeneratedClass.Get() : Blueprint->GeneratedClass.Get();
		TArray<FFieldVariant> Chain;
		for (int32 Index = 0; Index < Path->Elements.Num(); ++Index)
		{
			const FValue& Segment = Path->Elements[Index];
			FProperty* Property = Container != nullptr && Segment.Kind == EAssetDocumentRuntimeValueKind::String
				? FindFProperty<FProperty>(Container, FName(*Segment.StringValue)) : nullptr;
			if (Property == nullptr)
			{
				return Call.Args.Fail(TEXT("Widget.InvalidBindingSourcePath"), FString::Printf(TEXT("source_path[%d] '%s' is not a member of %s."), Index,
					*Segment.StringValue, *GetNameSafe(Container)), TEXT("source_path"));
			}
			Chain.Add(FFieldVariant(Property));
			const FStructProperty* Struct = CastField<FStructProperty>(Property);
			const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(Property);
			Container = Struct != nullptr ? static_cast<UStruct*>(Struct->Struct) : Object != nullptr ? static_cast<UStruct*>(Object->PropertyClass) : nullptr;
		}
		Binding.Kind = EBindingKind::Property;
		Binding.SourcePath = FEditorPropertyPath(Chain);
		FText Error;
		if (!Binding.SourcePath.Validate(Delegate, Error))
		{
			return Call.Args.Fail(TEXT("Widget.InvalidBindingSourcePath"), FString::Printf(TEXT("source_path does not fit %s.%s: %s"),
				*WidgetName.ToString(), *PropertyName.ToString(), *Error.ToString()), TEXT("source_path"));
		}
	}
	else
	{
		return Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("kind is Function or Property."), TEXT("kind"));
	}
	// SPropertyBinding: replace the binding of this widget property.
	Blueprint->Modify();
	Blueprint->Bindings.RemoveAll([&Binding](const FDelegateEditorBinding& Existing)
	{
		return Existing.ObjectName == Binding.ObjectName && Existing.PropertyName == Binding.PropertyName;
	});
	Blueprint->Bindings.Add(Binding);
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	Result.Add(TEXT("binding"), EncodeBinding(Binding));
	return true;
}

bool HandleBindingRemove(FCall& Call, FObjectBuilder& Result)
{
	UWidgetBlueprint* Blueprint = GetWidgetBlueprint(Call);
	FName WidgetName;
	FName PropertyName;
	if (Blueprint == nullptr || !Call.Args.OnlyFields({TEXT("widget"), TEXT("property")}) || !ReadName(Call, TEXT("widget"), WidgetName)
		|| !ReadName(Call, TEXT("property"), PropertyName))
	{
		return false;
	}
	const FString Object = WidgetName.ToString();
	const bool bPresent = Blueprint->Bindings.ContainsByPredicate([&](const FDelegateEditorBinding& Existing)
	{
		return Existing.ObjectName == Object && Existing.PropertyName == PropertyName;
	});
	if (!bPresent)
	{
		return Call.Args.Fail(TEXT("Widget.BindingNotFound"), FString::Printf(TEXT("%s.%s is not bound."), *Object, *PropertyName.ToString()), TEXT("property"));
	}
	Blueprint->Modify();
	Blueprint->Bindings.RemoveAll([&](const FDelegateEditorBinding& Existing)
	{
		return Existing.ObjectName == Object && Existing.PropertyName == PropertyName;
	});
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	return true;
}

// ---- desired focus -----------------------------------------------------------------------------

FName DesiredFocus(const UWidgetBlueprint& Blueprint)
{
	const UUserWidget* Defaults = Blueprint.GeneratedClass != nullptr ? Blueprint.GeneratedClass->GetDefaultObject<UUserWidget>() : nullptr;
	return Defaults != nullptr ? Defaults->GetDesiredFocusWidgetName() : NAME_None;
}

// Class Defaults > Interaction > Desired Focus Widget (FWidgetBlueprintEditorUtils::SetDesiredFocus).
bool HandleDesiredFocusSet(FCall& Call, FObjectBuilder& Result)
{
	UWidgetBlueprint* Blueprint = GetWidgetBlueprint(Call);
	FString Name;
	if (Blueprint == nullptr || !Call.Args.OnlyFields({TEXT("widget")}) || !Call.Args.String(TEXT("widget"), Name))
	{
		return false;
	}
	if (!Name.IsEmpty() && Blueprint->WidgetTree->FindWidget(FName(*Name)) == nullptr)
	{
		return Call.Args.Fail(TEXT("Widget.NotFound"), FString::Printf(TEXT("No widget named '%s' to focus."), *Name), TEXT("widget"));
	}
	if (Blueprint->GeneratedClass == nullptr)
	{
		return Call.Args.Fail(TEXT("Widget.UnsupportedAsset"), TEXT("The Widget Blueprint has no generated class yet."), TEXT("widget"));
	}
	if (DesiredFocus(*Blueprint) != FName(*Name))
	{
		FWidgetBlueprintEditorUtils::SetDesiredFocus(Blueprint, FName(*Name));
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}
	return true;
}

bool HandlePaletteCategorySet(FCall& Call, FObjectBuilder& Result)
{
	UWidgetBlueprint* Blueprint = GetWidgetBlueprint(Call);
	FString Category;
	if (Blueprint == nullptr || !Call.Args.OnlyFields({TEXT("category")}) || !Call.Args.String(TEXT("category"), Category))
	{
		return false;
	}
	if (Blueprint->GeneratedClass == nullptr)
	{
		return Call.Args.Fail(TEXT("Widget.UnsupportedAsset"), TEXT("The Widget Blueprint has no generated class yet."), TEXT("category"));
	}
	Category.TrimStartAndEndInline();
	if (Raw::GetPaletteCategory(*Blueprint) != Category)
	{
		Raw::SetPaletteCategory(*Blueprint, Category);
	}
	return true;
}

bool HandleDesignSizeSet(FCall& Call, FObjectBuilder& Result)
{
	UWidgetBlueprint* Blueprint = GetWidgetBlueprint(Call);
	Raw::FDesignSize Size;
	int64 Width = 0;
	int64 Height = 0;
	if (Blueprint == nullptr || !Call.Args.OnlyFields({TEXT("mode"), TEXT("width"), TEXT("height")})
		|| !Call.Args.String(TEXT("mode"), Size.Mode) || !Call.Args.Int(TEXT("width"), Width) || !Call.Args.Int(TEXT("height"), Height))
	{
		return false;
	}
	if (Width < 1 || Height < 1 || Width > 100000 || Height > 100000)
	{
		return Call.Args.Fail(TEXT("Widget.InvalidValue"), TEXT("The design size needs a width and height between 1 and 100000."), TEXT("width"));
	}
	Size.Width = static_cast<int32>(Width);
	Size.Height = static_cast<int32>(Height);
	if (Blueprint->GeneratedClass == nullptr)
	{
		return Call.Args.Fail(TEXT("Widget.UnsupportedAsset"), TEXT("The Widget Blueprint has no generated class yet."), TEXT("mode"));
	}
	Raw::FDesignSize Current;
	if (Raw::GetDesignSize(*Blueprint, Current) && Current.Mode == Size.Mode && Current.Width == Size.Width && Current.Height == Size.Height)
	{
		return true;
	}
	if (!Raw::SetDesignSize(*Blueprint, Size))
	{
		return Call.Args.Fail(TEXT("Widget.InvalidValue"), FString::Printf(
			TEXT("'%s' is not a design size mode (FillScreen, Custom, CustomOnScreen, Desired, DesiredOnScreen)."), *Size.Mode), TEXT("mode"));
	}
	return true;
}
} // namespace

// Animation handlers (AssetDocumentWidgetAnimationPrimitives.cpp).
bool HandleAnimationQuery(FCall& Call, FObjectBuilder& Result);
bool HandleAnimationSet(FCall& Call, FObjectBuilder& Result);
bool HandleAnimationRemove(FCall& Call, FObjectBuilder& Result);
bool HandleAnimationTrackSet(FCall& Call, FObjectBuilder& Result);
bool HandleAnimationTrackRemove(FCall& Call, FObjectBuilder& Result);

namespace
{
TArray<FAssetDocumentPrimitiveDescriptor> BuildDescriptors()
{
	return {
		MakeDescriptor(TEXT("umg.tree.query.v1"), false, TEXT("UWidgetTree::RootWidget;UWidgetTree::NamedSlotBindings;UPanelWidget::GetChildAt;INamedSlotInterface::GetContentForSlot")),
		MakeDescriptor(TEXT("umg.tree.add.v1"), true, TEXT("UWidgetTree::ConstructWidget;UPanelWidget::InsertChildAt;INamedSlotInterface::SetContentForSlot;UWidgetBlueprint::OnVariableAdded")),
		MakeDescriptor(TEXT("umg.tree.move.v1"), true, TEXT("UPanelWidget::ShiftChild;UPanelWidget::InsertChildAt;FWidgetBlueprintEditorUtils::RemoveNamedSlotHostContent")),
		MakeDescriptor(TEXT("umg.tree.remove.v1"), true, TEXT("FWidgetBlueprintEditorUtils::DeleteWidgets")),
		MakeDescriptor(TEXT("umg.widget.rename.v1"), true, TEXT("FWidgetBlueprintEditorUtils::RenameWidget;FBlueprintEditorUtils::ReplaceVariableReferences")),
		MakeDescriptor(TEXT("umg.widget.variable.set.v1"), true, TEXT("UWidget::bIsVariable;FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified")),
		MakeDescriptor(TEXT("umg.widget.property.set.v1"), true, TEXT("UObject::PreEditChange;UObject::PostEditChangeProperty")),
		MakeDescriptor(TEXT("umg.widget.navigation.set.v1"), true, TEXT("UWidget::Navigation;UWidgetNavigation::IsDefaultNavigation")),
		MakeDescriptor(TEXT("umg.binding.set.v1"), true, TEXT("UWidgetBlueprint::Bindings;FEditorPropertyPath::Validate;UFunction::IsSignatureCompatibleWith")),
		MakeDescriptor(TEXT("umg.binding.remove.v1"), true, TEXT("UWidgetBlueprint::Bindings")),
		MakeDescriptor(TEXT("umg.desired_focus.set.v1"), true, TEXT("FWidgetBlueprintEditorUtils::SetDesiredFocus")),
		MakeDescriptor(TEXT("umg.palette_category.set.v1"), true, TEXT("UUserWidget::PaletteCategory;UWidgetBlueprint::PaletteCategory")),
		MakeDescriptor(TEXT("umg.design_size.set.v1"), true, TEXT("SDesignerView::OnScreenFillRuleSelected;SDesignerView::OnCustomResolutionWidthChanged")),
		MakeDescriptor(TEXT("umg.anim.query.v1"), false, TEXT("UWidgetBlueprint::Animations;UMovieScene::GetBindings;FMovieSceneChannelProxy")),
		MakeDescriptor(TEXT("umg.anim.set.v1"), true, TEXT("UWidgetAnimation;UMovieScene::SetDisplayRate;UMovieScene::SetPlaybackRange;UWidgetBlueprint::OnVariableAdded")),
		MakeDescriptor(TEXT("umg.anim.remove.v1"), true, TEXT("UWidgetBlueprint::Animations;UWidgetBlueprint::OnVariableRemoved")),
		MakeDescriptor(TEXT("umg.anim.track.set.v1"), true, TEXT("UMovieScene::AddPossessable;UMovieScene::AddTrack;UMovieScenePropertyTrack::SetPropertyNameAndPath;TMovieSceneChannelData::AddKey")),
		MakeDescriptor(TEXT("umg.anim.track.remove.v1"), true, TEXT("UMovieScene::RemoveTrack;UMovieScene::RemovePossessable")),
	};
}

const FBinding Bindings[] = {
	{TEXT("umg.tree.query.v1"), &HandleTreeQuery},
	{TEXT("umg.tree.add.v1"), &HandleTreeAdd},
	{TEXT("umg.tree.move.v1"), &HandleTreeMove},
	{TEXT("umg.tree.remove.v1"), &HandleTreeRemove},
	{TEXT("umg.widget.rename.v1"), &HandleWidgetRename},
	{TEXT("umg.widget.variable.set.v1"), &HandleVariableSet},
	{TEXT("umg.widget.property.set.v1"), &HandlePropertySet},
	{TEXT("umg.widget.navigation.set.v1"), &HandleNavigationSet},
	{TEXT("umg.binding.set.v1"), &HandleBindingSet},
	{TEXT("umg.binding.remove.v1"), &HandleBindingRemove},
	{TEXT("umg.desired_focus.set.v1"), &HandleDesiredFocusSet},
	{TEXT("umg.palette_category.set.v1"), &HandlePaletteCategorySet},
	{TEXT("umg.design_size.set.v1"), &HandleDesignSizeSet},
	{TEXT("umg.anim.query.v1"), &HandleAnimationQuery},
	{TEXT("umg.anim.set.v1"), &HandleAnimationSet},
	{TEXT("umg.anim.remove.v1"), &HandleAnimationRemove},
	{TEXT("umg.anim.track.set.v1"), &HandleAnimationTrackSet},
	{TEXT("umg.anim.track.remove.v1"), &HandleAnimationTrackRemove},
};
} // namespace

TConstArrayView<FAssetDocumentPrimitiveDescriptor> GetDescriptors()
{
	static const TArray<FAssetDocumentPrimitiveDescriptor> Descriptors = BuildDescriptors();
	return Descriptors;
}

TConstArrayView<FBinding> GetBindings()
{
	return Bindings;
}
} // namespace UE::AssetDocument::Graph::Widget
