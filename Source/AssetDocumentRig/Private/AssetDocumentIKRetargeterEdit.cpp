// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// rig.ikretargeter.edit.v1: edits of an IK Retargeter as the IK Retarget editor makes them (design §7.2). Every
// structural edit is the UIKRetargeterController call the editor's UI makes (each its own transaction and
// re-initialization, nested in the request's transaction); op settings and chain settings are the Details panel's
// struct writes (UIKRigStructViewer: Owner->Modify(), write, re-initialize the op for the fields that ask for it);
// retarget poses are the edit-pose mode's writes of the pose. Every op is checked before the controller is called,
// so a refused op names its reason and the controller never logs a warning.
//
// Args {ops: [...]}, each {op, ...}:
//   rig.set {side, rig}                Asset Details Source / Target IK Rig ("" clears it), then the replace dialog's
//                                      "assign to all ops" (UIKRetargeterController::AssignIKRigToAllOps)
//   preview_mesh {side, mesh}          Asset Details Source / Target Preview Mesh ("" clears it)
//   op.add {type, name, parent?}       op stack "Add New" (under the selected parent op), then renamed to name
//   op.remove {name}  op.rename {name, to}  op.move {name, to} (stack index; the stack then re-sorts children
//   before their parent)  op.parent {name, parent}  op.enable {name, enabled}
//   op.target_rig {name, rig}          op Details "IK Rig Asset" ("" = the retargeter's target IK Rig)
//   op.settings {name, settings}       op Details (complete sparse over what "Add New" makes; struct write)
//   op.map {name, mapping}             chain mapping list (complete: {<target chain>: <source chain> | null})
//   op.chain_settings {name, chain, settings}  a chain's settings in the op Details (complete sparse; struct write)
//   pose.add {side, pose}  pose.remove {side, pose}  pose.rename {side, pose, to}  pose.current {side, pose}
//   pose.set {side, pose, root?, bones} edit-pose mode: the pose's bone rotation offsets (complete: unlisted bones
//                                      are reset) and its vertical root offset
// Result {changed}.

#include "AssetDocumentEditorNonInteractiveScope.h"
#include "AssetDocumentIKRigStructWrites.h"
#include "AssetDocumentNameSuggestions.h"
#include "AssetDocumentRigControllerScope.h"
#include "AssetDocumentRigPrimitives.h"
#include "Engine/SkeletalMesh.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Retargeter/IKRetargetChainMapping.h"
#include "Retargeter/IKRetargeter.h"
#include "Retargeter/IKRetargetOps.h"
#include "Rig/IKRigDefinition.h"
#include "IKRig.h"
#include "ScopedTransaction.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/UnrealType.h"

#include <cmath>

#define LOCTEXT_NAMESPACE "AssetDocumentIKRetargeterEdit"

namespace UE::AssetDocument::Graph::Rig
{
namespace IKRetargeterEditLocal
{
struct FEdit
{
	UIKRetargeter& Retargeter;
	UIKRetargeterController& Controller;
	bool bChanged = false;
};

// Retarget pose quaternions whose length is off by more than this are normalized (an approximate rotation written
// by hand); others are stored as written, so an extracted pose reads back bit for bit.
constexpr double QuatNormalizeTolerance = 1.0e-4;

FString JoinNames(const TArray<FString>& Names, int32 Limit = 40)
{
	if (Names.Num() <= Limit)
	{
		return FString::Join(Names, TEXT(", "));
	}
	TArray<FString> Head(Names.GetData(), Limit);
	return FString::Join(Head, TEXT(", ")) + FString::Printf(TEXT(" (+%d more)"), Names.Num() - Limit);
}

bool SameText(FName A, FName B)
{
	return A.ToString().Equals(B.ToString(), ESearchCase::CaseSensitive);
}

// ---- reading op fields -----------------------------------------------------------------------------------------

bool ReadName(FReader& Op, const TCHAR* Key, FName& Out, bool bAllowEmpty = false)
{
	FString Text;
	if (!Op.String(Key, Text))
	{
		return false;
	}
	if (Text.IsEmpty() && !bAllowEmpty)
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("%s is a non-empty name."), Key), Key);
	}
	Out = Text.IsEmpty() ? NAME_None : FName(*Text);
	return true;
}

bool ReadSide(FReader& Op, ERetargetSourceOrTarget& Out)
{
	FString Text;
	if (!Op.String(TEXT("side"), Text))
	{
		return false;
	}
	if (Text == TEXT("Source") || Text == TEXT("Target"))
	{
		Out = Text == TEXT("Source") ? ERetargetSourceOrTarget::Source : ERetargetSourceOrTarget::Target;
		return true;
	}
	return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("side is Source or Target."), TEXT("side"));
}

bool ReadRig(FReader& Op, const TCHAR* Key, UIKRigDefinition*& Out)
{
	FString Path;
	if (!Op.String(Key, Path))
	{
		return false;
	}
	Out = nullptr;
	if (Path.IsEmpty())
	{
		return true;
	}
	Out = LoadObject<UIKRigDefinition>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (Out == nullptr)
	{
		return Op.Fail(TEXT("IKRetargeter.UnknownIKRig"), FString::Printf(TEXT("'%s' is not an IK Rig."), *Path), Key);
	}
	return true;
}

TArray<FString> OpNames(const UIKRetargeter& Retargeter)
{
	TArray<FString> Names;
	for (const FInstancedStruct& OpStruct : Retargeter.GetRetargetOps())
	{
		if (const FIKRetargetOpBase* RetargetOp = OpStruct.GetPtr<FIKRetargetOpBase>())
		{
			Names.Add(RetargetOp->GetName().ToString());
		}
	}
	return Names;
}

// The stack index of the op named Name (op names compare case-insensitively, as the op stack compares them).
bool RequireOp(FEdit& Edit, FReader& Op, const TCHAR* Key, FName Name, int32& OutIndex)
{
	OutIndex = Edit.Controller.GetIndexOfOpByName(Name);
	if (OutIndex != INDEX_NONE && Edit.Controller.GetRetargetOpByIndex(OutIndex) != nullptr)
	{
		return true;
	}
	return Op.Fail(TEXT("IKRetargeter.OpNotFound"), FString::Printf(TEXT("The op stack has no op '%s'. Ops: %s."), *Name.ToString(),
		*JoinNames(OpNames(Edit.Retargeter))), Key);
}

bool ReadOp(FEdit& Edit, FReader& Op, int32& OutIndex, FIKRetargetOpBase*& OutOp)
{
	FName Name;
	if (!ReadName(Op, TEXT("name"), Name) || !RequireOp(Edit, Op, TEXT("name"), Name, OutIndex))
	{
		return false;
	}
	OutOp = Edit.Controller.GetRetargetOpByIndex(OutIndex);
	return true;
}

// Name is free for an op (other than the op at Self): op names are case-insensitive.
bool CheckOpNameFree(FEdit& Edit, FReader& Op, const TCHAR* Key, FName Name, int32 Self = INDEX_NONE)
{
	const int32 Existing = Edit.Controller.GetIndexOfOpByName(Name);
	if (Existing == INDEX_NONE || Existing == Self)
	{
		return true;
	}
	return Op.Fail(TEXT("IKRetargeter.NameTaken"), FString::Printf(TEXT("The op stack already has an op named '%s' (op names are "
		"case-insensitive)."), *Edit.Controller.GetOpName(Existing).ToString()), Key);
}

// The field of an op's settings that names the op's target IK Rig (FK Chains, Run IK Rig, ...); null for an op that
// uses its parent's (child ops) or none.
FObjectPropertyBase* FindTargetRigField(const UScriptStruct* SettingsType)
{
	for (TFieldIterator<FProperty> It(SettingsType); It; ++It)
	{
		if (IsTargetRigField(**It))
		{
			return CastField<FObjectPropertyBase>(*It);
		}
	}
	return nullptr;
}

FArrayProperty* FindChainRows(const UScriptStruct* SettingsType)
{
	for (TFieldIterator<FProperty> It(SettingsType); It; ++It)
	{
		if (IsChainRowArray(**It))
		{
			return CastField<FArrayProperty>(*It);
		}
	}
	return nullptr;
}

// UIKRigStructViewer::TriggerReinitIfNeeded: a field marked ReinitializeOnEdit, a name, or a bone name (FBoneReference's
// BoneName; here also a whole bone reference or an array of them / of names, which the Details panel edits element by
// element).
bool NeedsReinit(const FProperty& Property)
{
	if (Property.HasMetaData(IKRigReinitOnEditMetaLabel) || CastField<FNameProperty>(&Property) != nullptr
		|| Property.GetFName() == TEXT("BoneName"))
	{
		return true;
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		return Struct->Struct != nullptr && Struct->Struct->FindPropertyByName(TEXT("BoneName")) != nullptr;
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		return NeedsReinit(*Array->Inner);
	}
	return false;
}

void ReinitAfterWrite(FEdit& Edit, FName OpName, const FStructWriteResult& Written)
{
	for (const FProperty* Property : Written.Changed)
	{
		if (NeedsReinit(*Property))
		{
			const FPropertyChangedEvent Event(const_cast<FProperty*>(Property), EPropertyChangeType::ValueSet);
			Edit.Controller.OnOpPropertyChanged(OpName, Event);
		}
	}
}

// ---- sides -----------------------------------------------------------------------------------------------------

bool OpRigSet(FEdit& Edit, FReader& Op)
{
	ERetargetSourceOrTarget Side;
	UIKRigDefinition* Rig = nullptr;
	if (!ReadSide(Op, Side) || !ReadRig(Op, TEXT("rig"), Rig))
	{
		return false;
	}
	if (Edit.Retargeter.GetIKRig(Side) == Rig)
	{
		return true;
	}
	// The Details edit: the property change is transacted (SetIKRig itself does not Modify), then the replace dialog's
	// answer "assign to all ops" (every op takes the new rig for that side and auto-maps unmapped chains).
	Edit.Retargeter.Modify();
	Edit.Controller.SetIKRig(Side, Rig);
	if (Rig != nullptr)
	{
		Edit.Controller.AssignIKRigToAllOps(Side, Rig);
	}
	Edit.bChanged = true;
	return true;
}

bool OpPreviewMesh(FEdit& Edit, FReader& Op)
{
	ERetargetSourceOrTarget Side;
	FString Path;
	if (!ReadSide(Op, Side) || !Op.String(TEXT("mesh"), Path))
	{
		return false;
	}
	USkeletalMesh* Mesh = Path.IsEmpty() ? nullptr : LoadObject<USkeletalMesh>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Path.IsEmpty() && Mesh == nullptr)
	{
		return Op.Fail(TEXT("IKRetargeter.UnknownMesh"), FString::Printf(TEXT("'%s' is not a Skeletal Mesh."), *Path), TEXT("mesh"));
	}
	if (Edit.Controller.GetPreviewMesh(Side) == Mesh)
	{
		return true;
	}
	Edit.Retargeter.Modify();
	Edit.Controller.SetPreviewMesh(Side, Mesh);
	Edit.bChanged = true;
	return true;
}

// ---- op stack --------------------------------------------------------------------------------------------------

TArray<FString> OpTypeNames()
{
	TArray<FString> Names;
	for (const UScriptStruct* Type : GetOpTypes())
	{
		Names.Add(GetOpTypeName(*Type));
	}
	return Names;
}

bool OpAdd(FEdit& Edit, FReader& Op)
{
	FString TypeName;
	FName Name;
	FName Parent;
	if (!Op.String(TEXT("type"), TypeName) || !ReadName(Op, TEXT("name"), Name)
		|| (Op.Has(TEXT("parent")) && !ReadName(Op, TEXT("parent"), Parent, true)))
	{
		return false;
	}
	UScriptStruct* Type = FindOpType(TypeName);
	if (Type == nullptr)
	{
		return Op.Fail(TEXT("IKRetargeter.UnknownOpType"), FString::Printf(TEXT("'%s' is not a retarget op type. Types: %s "
			"(list_types kind ikretarget_op)."), *TypeName, *JoinNames(OpTypeNames())), TEXT("type"));
	}
	const FInstancedStruct Probe(Type);
	const FIKRetargetOpBase& New = Probe.Get<FIKRetargetOpBase>();
	if (New.IsSingleton())
	{
		for (const FInstancedStruct& Existing : Edit.Retargeter.GetRetargetOps())
		{
			if (Existing.GetScriptStruct() == Type)
			{
				return Op.Fail(TEXT("IKRetargeter.SingletonOp"), FString::Printf(TEXT("A retargeter has at most one '%s' op and this one "
					"has '%s'."), *TypeName, *Existing.Get<FIKRetargetOpBase>().GetName().ToString()), TEXT("type"));
			}
		}
	}
	if (!CheckOpNameFree(Edit, Op, TEXT("name"), Name))
	{
		return false;
	}
	if (const UScriptStruct* ParentType = New.GetParentOpType())
	{
		if (Parent.IsNone())
		{
			return Op.Fail(TEXT("IKRetargeter.InvalidParentOp"), FString::Printf(TEXT("A '%s' op runs under a '%s' op: list it in that "
				"op's Children."), *TypeName, *GetOpTypeName(*ParentType)), TEXT("parent"));
		}
		int32 ParentIndex = INDEX_NONE;
		if (!RequireOp(Edit, Op, TEXT("parent"), Parent, ParentIndex))
		{
			return false;
		}
		const FIKRetargetOpBase* ParentOp = Edit.Controller.GetRetargetOpByIndex(ParentIndex);
		if (ParentOp->GetType() != ParentType)
		{
			return Op.Fail(TEXT("IKRetargeter.InvalidParentOp"), FString::Printf(TEXT("A '%s' op runs under a '%s' op; '%s' is a '%s' op."),
				*TypeName, *GetOpTypeName(*ParentType), *Parent.ToString(), *GetOpTypeName(*ParentOp->GetType())), TEXT("parent"));
		}
		Parent = ParentOp->GetName();
	}
	else if (!Parent.IsNone())
	{
		return Op.Fail(TEXT("IKRetargeter.InvalidParentOp"), FString::Printf(TEXT("A '%s' op is a top-level op (it has no parent op)."),
			*TypeName), TEXT("parent"));
	}
	const int32 Index = Edit.Controller.AddRetargetOp(Type, Parent);
	if (Index == INDEX_NONE)
	{
		return Op.Fail(TEXT("IKRetargeter.EditFailed"), FString::Printf(TEXT("The retargeter did not add a '%s' op."), *TypeName), TEXT("type"));
	}
	Edit.bChanged = true;
	if (!SameText(Edit.Controller.GetOpName(Index), Name))
	{
		Edit.Controller.SetOpName(Name, Index);
	}
	return true;
}

bool OpRemove(FEdit& Edit, FReader& Op)
{
	int32 Index = INDEX_NONE;
	FIKRetargetOpBase* RetargetOp = nullptr;
	if (!ReadOp(Edit, Op, Index, RetargetOp))
	{
		return false;
	}
	Edit.bChanged |= Edit.Controller.RemoveRetargetOp(Index);
	return true;
}

bool OpRename(FEdit& Edit, FReader& Op)
{
	int32 Index = INDEX_NONE;
	FIKRetargetOpBase* RetargetOp = nullptr;
	FName To;
	if (!ReadOp(Edit, Op, Index, RetargetOp) || !ReadName(Op, TEXT("to"), To) || !CheckOpNameFree(Edit, Op, TEXT("to"), To, Index))
	{
		return false;
	}
	if (SameText(RetargetOp->GetName(), To))
	{
		return true;
	}
	Edit.Controller.SetOpName(To, Index);
	Edit.bChanged = true;
	return true;
}

bool OpMove(FEdit& Edit, FReader& Op)
{
	int32 Index = INDEX_NONE;
	FIKRetargetOpBase* RetargetOp = nullptr;
	int64 To = 0;
	if (!ReadOp(Edit, Op, Index, RetargetOp) || !Op.Int(TEXT("to"), To))
	{
		return false;
	}
	const int32 Count = Edit.Controller.GetNumRetargetOps();
	if (To < 0 || To >= Count)
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("to is a stack index 0..%d."), Count - 1), TEXT("to"));
	}
	if (To == Index)
	{
		return true;
	}
	Edit.bChanged |= Edit.Controller.MoveRetargetOpInStack(Index, static_cast<int32>(To));
	return true;
}

bool OpParent(FEdit& Edit, FReader& Op)
{
	int32 Index = INDEX_NONE;
	FIKRetargetOpBase* RetargetOp = nullptr;
	FName Parent;
	int32 ParentIndex = INDEX_NONE;
	if (!ReadOp(Edit, Op, Index, RetargetOp) || !ReadName(Op, TEXT("parent"), Parent)
		|| !RequireOp(Edit, Op, TEXT("parent"), Parent, ParentIndex))
	{
		return false;
	}
	const FIKRetargetOpBase* ParentOp = Edit.Controller.GetRetargetOpByIndex(ParentIndex);
	const UScriptStruct* ParentType = RetargetOp->GetParentOpType();
	if (ParentType == nullptr || ParentOp->GetType() != ParentType)
	{
		return Op.Fail(TEXT("IKRetargeter.InvalidParentOp"), ParentType == nullptr
			? FString::Printf(TEXT("'%s' is a top-level op (it has no parent op)."), *RetargetOp->GetName().ToString())
			: FString::Printf(TEXT("'%s' runs under a '%s' op; '%s' is a '%s' op."), *RetargetOp->GetName().ToString(),
				*GetOpTypeName(*ParentType), *Parent.ToString(), *GetOpTypeName(*ParentOp->GetType())), TEXT("parent"));
	}
	if (RetargetOp->GetParentOpName() == ParentOp->GetName())
	{
		return true;
	}
	Edit.bChanged |= Edit.Controller.SetParentOpByName(RetargetOp->GetName(), ParentOp->GetName());
	return true;
}

bool OpEnable(FEdit& Edit, FReader& Op)
{
	int32 Index = INDEX_NONE;
	FIKRetargetOpBase* RetargetOp = nullptr;
	bool bEnabled = true;
	if (!ReadOp(Edit, Op, Index, RetargetOp) || !Op.Bool(TEXT("enabled"), bEnabled, true))
	{
		return false;
	}
	if (RetargetOp->IsEnabled() == bEnabled)
	{
		return true;
	}
	Edit.bChanged |= Edit.Controller.SetRetargetOpEnabled(Index, bEnabled);
	return true;
}

bool OpTargetRig(FEdit& Edit, FReader& Op)
{
	int32 Index = INDEX_NONE;
	FIKRetargetOpBase* RetargetOp = nullptr;
	UIKRigDefinition* Rig = nullptr;
	if (!ReadOp(Edit, Op, Index, RetargetOp) || !ReadRig(Op, TEXT("rig"), Rig))
	{
		return false;
	}
	const UScriptStruct* SettingsType = RetargetOp->GetSettingsType();
	FObjectPropertyBase* Field = SettingsType != nullptr ? FindTargetRigField(SettingsType) : nullptr;
	FIKRetargetOpSettingsBase* Settings = Field != nullptr ? RetargetOp->GetSettings() : nullptr;
	if (Settings == nullptr)
	{
		return Op.Fail(TEXT("IKRetargeter.NoTargetIKRig"), FString::Printf(TEXT("A '%s' op has no target IK Rig of its own (a child op "
			"uses its parent's)."), *GetOpTypeName(*RetargetOp->GetType())), TEXT("rig"));
	}
	if (Rig == nullptr)
	{
		Rig = Edit.Retargeter.GetIKRigWriteable(ERetargetSourceOrTarget::Target);
	}
	if (Field->GetObjectPropertyValue_InContainer(Settings) == Rig)
	{
		return true;
	}
	// The op Details edit of "IK Rig Asset" (ReinitializeOnEdit): Modify, write, re-initialize the op.
	Edit.Retargeter.Modify();
	Field->SetObjectPropertyValue_InContainer(Settings, Rig);
	const FPropertyChangedEvent Event(Field, EPropertyChangeType::ValueSet);
	Edit.Controller.OnOpPropertyChanged(RetargetOp->GetName(), Event);
	Edit.bChanged = true;
	return true;
}

bool OpSettings(FEdit& Edit, FReader& Op)
{
	int32 Index = INDEX_NONE;
	FIKRetargetOpBase* RetargetOp = nullptr;
	if (!ReadOp(Edit, Op, Index, RetargetOp))
	{
		return false;
	}
	const UScriptStruct* SettingsType = RetargetOp->GetSettingsType();
	FIKRetargetOpSettingsBase* Settings = SettingsType != nullptr ? RetargetOp->GetSettings() : nullptr;
	if (Settings == nullptr)
	{
		return Op.Fail(TEXT("IKRig.FieldNotEditable"), FString::Printf(TEXT("A '%s' op has no settings."),
			*GetOpTypeName(*RetargetOp->GetType())), TEXT("settings"));
	}
	FInstancedStruct Defaults = MakeAddedOp(Edit.Retargeter, *RetargetOp->GetType(), RetargetOp->GetParentOpName());
	const FName OpName = RetargetOp->GetName();
	FStructWriteResult Written;
	if (!WriteSettingsStruct(Op, TEXT("settings"), *SettingsType, Settings, Defaults.GetMutable<FIKRetargetOpBase>().GetSettings(),
		Edit.Retargeter, [](const FProperty& Property) { return IsOpOwnedField(Property); }, Written))
	{
		return false;
	}
	ReinitAfterWrite(Edit, OpName, Written);
	Edit.bChanged |= Written.bChanged;
	return true;
}

TArray<FString> MappingChains(const FRetargetChainMapping& Mapping, ERetargetSourceOrTarget Side)
{
	TArray<FString> Names;
	if (const UIKRigDefinition* Rig = const_cast<FRetargetChainMapping&>(Mapping).GetIKRig(Side))
	{
		for (const FBoneChain& Chain : Rig->GetRetargetChains())
		{
			Names.Add(Chain.ChainName.ToString());
		}
	}
	return Names;
}

bool OpMap(FEdit& Edit, FReader& Op)
{
	int32 Index = INDEX_NONE;
	FIKRetargetOpBase* RetargetOp = nullptr;
	const FValue* Mapping = nullptr;
	if (!ReadOp(Edit, Op, Index, RetargetOp) || !Op.Object(TEXT("mapping"), Mapping))
	{
		return false;
	}
	const FRetargetChainMapping* Current = RetargetOp->GetChainMapping();
	if (Current == nullptr)
	{
		return Op.Fail(TEXT("IKRetargeter.NoChainMapping"), FString::Printf(TEXT("A '%s' op has no chain mapping (a child op uses its "
			"parent's)."), *GetOpTypeName(*RetargetOp->GetType())), TEXT("mapping"));
	}
	TMap<FName, FName> Wanted;
	for (int32 Pair = 0; Pair < Mapping->ObjectKeys.Num(); ++Pair)
	{
		const FName Target(*Mapping->ObjectKeys[Pair]);
		const FValue& Value = Mapping->Elements[Pair];
		const FString Key = FString::Printf(TEXT("mapping/%s"), *Mapping->ObjectKeys[Pair]);
		if (!Current->HasChain(Target, ERetargetSourceOrTarget::Target))
		{
			return Op.Fail(TEXT("IKRetargeter.UnknownTargetChain"), FString::Printf(TEXT("The op's target IK Rig has no retarget chain '%s'. "
				"Chains: %s."), *Target.ToString(), *JoinNames(MappingChains(*Current, ERetargetSourceOrTarget::Target))), Key);
		}
		FName Source;
		if (Value.Kind == EAssetDocumentRuntimeValueKind::String && !Value.StringValue.IsEmpty())
		{
			Source = FName(*Value.StringValue);
			if (!Current->HasChain(Source, ERetargetSourceOrTarget::Source))
			{
				return Op.Fail(TEXT("IKRetargeter.ChainNotFound"), FString::Printf(TEXT("The source IK Rig has no retarget chain '%s'. "
					"Chains: %s."), *Source.ToString(), *JoinNames(MappingChains(*Current, ERetargetSourceOrTarget::Source))), Key);
			}
		}
		else if (Value.Kind != EAssetDocumentRuntimeValueKind::Null && Value.Kind != EAssetDocumentRuntimeValueKind::String)
		{
			return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("A mapping value is a source chain name or null (unmapped)."), Key);
		}
		Wanted.Add(Target, Source);
	}
	TArray<TPair<FName, FName>> Changes;
	for (const FRetargetChainPair& Pair : Current->GetChainPairs())
	{
		const FName* Source = Wanted.Find(Pair.TargetChainName);
		const FName NewSource = Source != nullptr ? *Source : NAME_None;
		if (!SameText(Pair.SourceChainName, NewSource))
		{
			Changes.Emplace(Pair.TargetChainName, NewSource);
		}
	}
	const FName OpName = RetargetOp->GetName();
	for (const TPair<FName, FName>& Change : Changes)
	{
		Edit.bChanged |= Edit.Controller.SetSourceChain(Change.Value, Change.Key, OpName);
	}
	return true;
}

bool OpChainSettings(FEdit& Edit, FReader& Op)
{
	int32 Index = INDEX_NONE;
	FIKRetargetOpBase* RetargetOp = nullptr;
	FName Chain;
	if (!ReadOp(Edit, Op, Index, RetargetOp) || !ReadName(Op, TEXT("chain"), Chain))
	{
		return false;
	}
	const UScriptStruct* SettingsType = RetargetOp->GetSettingsType();
	FArrayProperty* Rows = SettingsType != nullptr ? FindChainRows(SettingsType) : nullptr;
	FIKRetargetOpSettingsBase* Settings = Rows != nullptr ? RetargetOp->GetSettings() : nullptr;
	if (Settings == nullptr)
	{
		return Op.Fail(TEXT("IKRetargeter.NoChainSettings"), FString::Printf(TEXT("A '%s' op has no per-chain settings."),
			*GetOpTypeName(*RetargetOp->GetType())), TEXT("chain"));
	}
	const UScriptStruct* RowType = CastField<FStructProperty>(Rows->Inner)->Struct;
	const FNameProperty* NameField = CastField<FNameProperty>(RowType->FindPropertyByName(TEXT("TargetChainName")));
	FScriptArrayHelper Helper(Rows, Rows->ContainerPtrToValuePtr<void>(Settings));
	uint8* Row = nullptr;
	TArray<FString> Names;
	for (int32 Each = 0; Each < Helper.Num(); ++Each)
	{
		const FName RowChain = NameField != nullptr ? NameField->GetPropertyValue_InContainer(Helper.GetRawPtr(Each)) : NAME_None;
		Names.Add(RowChain.ToString());
		if (Row == nullptr && RowChain == Chain)
		{
			Row = Helper.GetRawPtr(Each);
		}
	}
	if (Row == nullptr)
	{
		return Op.Fail(TEXT("IKRetargeter.ChainNotFound"), FString::Printf(TEXT("The op has no settings for target chain '%s' (it has a row "
			"per chain of its target IK Rig). Chains: %s."), *Chain.ToString(), *JoinNames(Names)), TEXT("chain"));
	}
	const FStructInstance Defaults(RowType);
	const FName OpName = RetargetOp->GetName();
	FStructWriteResult Written;
	if (!WriteSettingsStruct(Op, TEXT("settings"), *RowType, Row, Defaults.Get(), Edit.Retargeter,
		[NameField](const FProperty& Property) { return &Property == NameField; }, Written))
	{
		return false;
	}
	ReinitAfterWrite(Edit, OpName, Written);
	Edit.bChanged |= Written.bChanged;
	return true;
}

// ---- retarget poses ----------------------------------------------------------------------------------------------

bool ReadPose(FEdit& Edit, FReader& Op, ERetargetSourceOrTarget& OutSide, FName& OutPose, const TCHAR* Key = TEXT("pose"))
{
	if (!ReadSide(Op, OutSide) || !ReadName(Op, Key, OutPose))
	{
		return false;
	}
	if (Edit.Controller.GetRetargetPoses(OutSide).Contains(OutPose))
	{
		return true;
	}
	TArray<FName> Names;
	Edit.Controller.GetRetargetPoses(OutSide).GetKeys(Names);
	TArray<FString> Text;
	for (const FName& Name : Names)
	{
		Text.Add(Name.ToString());
	}
	return Op.Fail(TEXT("IKRetargeter.PoseNotFound"), FString::Printf(TEXT("The %s side has no retarget pose '%s'. Poses: %s."),
		RetargetSideName(OutSide), *OutPose.ToString(), *JoinNames(Text)), Key);
}

bool OpPoseAdd(FEdit& Edit, FReader& Op)
{
	ERetargetSourceOrTarget Side;
	FName Pose;
	if (!ReadSide(Op, Side) || !ReadName(Op, TEXT("pose"), Pose))
	{
		return false;
	}
	if (Edit.Controller.GetRetargetPoses(Side).Contains(Pose))
	{
		return Op.Fail(TEXT("IKRetargeter.NameTaken"), FString::Printf(TEXT("The %s side already has a retarget pose '%s' (pose names are "
			"case-insensitive)."), RetargetSideName(Side), *Pose.ToString()), TEXT("pose"));
	}
	if (!SameText(Edit.Controller.CreateRetargetPose(Pose, Side), Pose))
	{
		return Op.Fail(TEXT("IKRetargeter.EditFailed"), FString::Printf(TEXT("The retargeter did not create pose '%s'."), *Pose.ToString()), TEXT("pose"));
	}
	Edit.bChanged = true;
	return true;
}

bool OpPoseRemove(FEdit& Edit, FReader& Op)
{
	ERetargetSourceOrTarget Side;
	FName Pose;
	if (!ReadPose(Edit, Op, Side, Pose))
	{
		return false;
	}
	if (Pose == UIKRetargeter::GetDefaultPoseName())
	{
		return Op.Fail(TEXT("IKRetargeter.DefaultPoseRequired"), TEXT("The default pose cannot be removed (the pose menu keeps it)."), TEXT("pose"));
	}
	Edit.bChanged |= Edit.Controller.RemoveRetargetPose(Pose, Side);
	return true;
}

bool OpPoseRename(FEdit& Edit, FReader& Op)
{
	ERetargetSourceOrTarget Side;
	FName Pose;
	FName To;
	if (!ReadPose(Edit, Op, Side, Pose) || !ReadName(Op, TEXT("to"), To))
	{
		return false;
	}
	if (Pose == UIKRetargeter::GetDefaultPoseName() || To == UIKRetargeter::GetDefaultPoseName())
	{
		return Op.Fail(TEXT("IKRetargeter.DefaultPoseRequired"), FString::Printf(TEXT("The default pose ('%s') cannot be renamed, and no "
			"other pose can take its name."), *UIKRetargeter::GetDefaultPoseName().ToString()), TEXT("to"));
	}
	if (SameText(Pose, To))
	{
		return true;
	}
	if (Edit.Controller.GetRetargetPoses(Side).Contains(To))
	{
		return Op.Fail(TEXT("IKRetargeter.NameTaken"), Pose == To
			? FString::Printf(TEXT("Pose names are case-insensitive: '%s' cannot be renamed to '%s' directly; rename it to another name "
				"first."), *Pose.ToString(), *To.ToString())
			: FString::Printf(TEXT("The %s side already has a retarget pose '%s'."), RetargetSideName(Side), *To.ToString()), TEXT("to"));
	}
	Edit.bChanged |= Edit.Controller.RenameRetargetPose(Pose, To, Side);
	return true;
}

bool OpPoseCurrent(FEdit& Edit, FReader& Op)
{
	ERetargetSourceOrTarget Side;
	FName Pose;
	if (!ReadPose(Edit, Op, Side, Pose))
	{
		return false;
	}
	if (SameText(Edit.Controller.GetCurrentRetargetPoseName(Side), Pose))
	{
		return true;
	}
	// The stored key (a pose named in another case is the same pose).
	for (const TPair<FName, FIKRetargetPose>& Each : Edit.Controller.GetRetargetPoses(Side))
	{
		if (Each.Key == Pose)
		{
			Pose = Each.Key;
		}
	}
	Edit.bChanged |= Edit.Controller.SetCurrentRetargetPose(Pose, Side);
	return true;
}

bool SameQuat(const FQuat& A, const FQuat& B)
{
	return A.X == B.X && A.Y == B.Y && A.Z == B.Z && A.W == B.W;
}

// A document rotation as the quaternion stored: a rotator converts (FRotator::Quaternion), a quaternion off unit length
// by more than QuatNormalizeTolerance is normalized, unless it is exactly the rotation the pose already holds (an
// extracted pose re-applied keeps its values).
bool ReadRotation(FReader& Op, const FString& Key, const FValue& Value, const FQuat* Existing, FQuat& Out)
{
	FReader Rotation = Op.Child(Key, Value);
	if (!Rotation.IsObject())
	{
		return Op.Fail(TEXT("IKRetargeter.InvalidValue"), TEXT("A bone rotation offset is a quaternion {X, Y, Z, W} or a rotator "
			"{Pitch, Yaw, Roll} (degrees)."), Key);
	}
	if (Rotation.Has(TEXT("W")) || Rotation.Has(TEXT("X")))
	{
		double X = 0.0, Y = 0.0, Z = 0.0, W = 0.0;
		if (!Rotation.OnlyFields({TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("W")}) || !Rotation.Number(TEXT("X"), X)
			|| !Rotation.Number(TEXT("Y"), Y) || !Rotation.Number(TEXT("Z"), Z) || !Rotation.Number(TEXT("W"), W))
		{
			return false;
		}
		Out = FQuat(X, Y, Z, W);
		if (Existing != nullptr && SameQuat(*Existing, Out))
		{
			return true;
		}
	}
	else
	{
		double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
		if (!Rotation.OnlyFields({TEXT("Pitch"), TEXT("Yaw"), TEXT("Roll")}) || !Rotation.Number(TEXT("Pitch"), Pitch, false)
			|| !Rotation.Number(TEXT("Yaw"), Yaw, false) || !Rotation.Number(TEXT("Roll"), Roll, false))
		{
			return false;
		}
		Out = FRotator(Pitch, Yaw, Roll).Quaternion();
	}
	const double SizeSquared = Out.SizeSquared();
	if (!FMath::IsFinite(SizeSquared) || SizeSquared < UE_SMALL_NUMBER)
	{
		return Op.Fail(TEXT("IKRetargeter.InvalidValue"), TEXT("A bone rotation offset is a non-zero quaternion."), Key);
	}
	if (FMath::Abs(SizeSquared - 1.0) > QuatNormalizeTolerance)
	{
		Out.Normalize();
	}
	return true;
}

bool ReadRoot(FReader& Op, FVector& Out)
{
	const FValue* Value = Op.Find(TEXT("root"));
	Out = FVector::ZeroVector;
	if (Value == nullptr || Value->Kind == EAssetDocumentRuntimeValueKind::Null)
	{
		return true;
	}
	if (Value->Kind == EAssetDocumentRuntimeValueKind::Object)
	{
		FReader Vector = Op.Child(TEXT("root"), *Value);
		return Vector.OnlyFields({TEXT("X"), TEXT("Y"), TEXT("Z")}) && Vector.Number(TEXT("X"), Out.X, false)
			&& Vector.Number(TEXT("Y"), Out.Y, false) && Vector.Number(TEXT("Z"), Out.Z, false);
	}
	return Op.Number(TEXT("root"), Out.Z);
}

bool OpPoseSet(FEdit& Edit, FReader& Op)
{
	ERetargetSourceOrTarget Side;
	FName PoseName;
	const FValue* Bones = nullptr;
	FVector Root;
	if (!ReadPose(Edit, Op, Side, PoseName) || !Op.Object(TEXT("bones"), Bones) || !ReadRoot(Op, Root))
	{
		return false;
	}
	const UIKRigDefinition* Rig = Edit.Retargeter.GetIKRig(Side);
	if (Rig == nullptr)
	{
		return Op.Fail(TEXT("IKRetargeter.IKRigRequired"), FString::Printf(TEXT("A %s retarget pose rotates bones of the %s IK Rig's "
			"skeleton: set Body/%s/IKRig first."), RetargetSideName(Side), RetargetSideName(Side), RetargetSideName(Side)), TEXT("bones"));
	}
	FIKRetargetPose& Pose = Edit.Controller.GetRetargetPoses(Side)[PoseName];
	const TMap<FName, FQuat>& Stored = Pose.GetAllDeltaRotations();
	const FIKRigSkeleton& Skeleton = Rig->GetSkeleton();
	TMap<FName, FQuat> Wanted;
	for (int32 Each = 0; Each < Bones->ObjectKeys.Num(); ++Each)
	{
		const FName Bone(*Bones->ObjectKeys[Each]);
		const FString Key = FString::Printf(TEXT("bones/%s"), *Bones->ObjectKeys[Each]);
		const FQuat* Existing = Stored.Find(Bone);
		FQuat Rotation;
		if (!ReadRotation(Op, Key, Bones->Elements[Each], Existing, Rotation))
		{
			return false;
		}
		// A bone the skeleton does not have is kept only when the pose already holds it with this rotation (a pose
		// saved against an older skeleton); the edit-pose mode can only rotate bones of the skeleton.
		if (Skeleton.GetBoneIndexFromName(Bone) == INDEX_NONE && (Existing == nullptr || !SameQuat(*Existing, Rotation)))
		{
			TArray<FString> BoneNames;
			for (const FName& SkeletonBone : Skeleton.BoneNames)
			{
				BoneNames.Add(SkeletonBone.ToString());
			}
			return Op.Fail(TEXT("IKRetargeter.BoneNotFound"), FString::Printf(TEXT("The %s IK Rig's skeleton has no bone '%s'.%s"),
				RetargetSideName(Side), *Bone.ToString(), *UE::AssetDocument::DescribeNameCandidates(Bone.ToString(), BoneNames, TEXT("Bones"))), Key);
		}
		Wanted.Add(Bone, Rotation);
	}
	TArray<FName> Reset;
	TArray<TPair<FName, FQuat>> Rotate;
	for (const TPair<FName, FQuat>& Each : Stored)
	{
		if (!Wanted.Contains(Each.Key))
		{
			Reset.Add(Each.Key);
		}
	}
	for (const TPair<FName, FQuat>& Each : Wanted)
	{
		const FQuat* Existing = Stored.Find(Each.Key);
		if (Existing == nullptr || !SameQuat(*Existing, Each.Value))
		{
			Rotate.Add(Each);
		}
	}
	const bool bRootChanged = !Pose.GetRootTranslationDelta().Equals(Root, 0.0);
	if (bRootChanged && (Root.X != 0.0 || Root.Y != 0.0))
	{
		return Op.Fail(TEXT("IKRetargeter.InvalidValue"), TEXT("A retarget pose offsets the root vertically only (RootOffset is the Z "
			"offset; the editor zeroes X and Y)."), TEXT("root"));
	}
	if (Reset.IsEmpty() && Rotate.IsEmpty() && !bRootChanged)
	{
		return true;
	}
	// The edit-pose mode's edits of this pose: Modify, the bones reset ("Reset Selected"), the rotation offsets of the
	// rotated bones and the root offset, offsets sorted leaf to root, then the retargeter re-initialized.
	Edit.Retargeter.Modify();
	const FScopedReinitializeIKRetargeter Reinitialize(&Edit.Controller);
	if (!Reset.IsEmpty())
	{
		Edit.Controller.ResetRetargetPose(PoseName, Reset, Side);
	}
	for (const TPair<FName, FQuat>& Each : Rotate)
	{
		Pose.SetDeltaRotationForBone(Each.Key, Each.Value);
	}
	if (!Pose.GetRootTranslationDelta().Equals(Root, 0.0))
	{
		Pose.SetRootTranslationDelta(Root);
	}
	Pose.SortHierarchically(Skeleton);
	Edit.bChanged = true;
	return true;
}

// ---- after the edit ----------------------------------------------------------------------------------------------

// A Speed Planting op whose settings have a Spring Strength gets it re-derived on load from its older, hidden Stiffness
// (Strength = sqrt(Stiffness) / 2pi). After an edit each such op's Stiffness is set to the one that loads back as its
// current Spring Strength, so what the edit leaves survives a save and reload.
void KeepSpeedPlantingStiffness(UIKRetargeter& Retargeter)
{
	for (const FInstancedStruct& OpStruct : Retargeter.GetRetargetOps())
	{
		FIKRetargetOpBase* Op = const_cast<FInstancedStruct&>(OpStruct).GetMutablePtr<FIKRetargetOpBase>();
		const UScriptStruct* SettingsType = Op != nullptr ? Op->GetSettingsType() : nullptr;
		if (SettingsType == nullptr || SettingsType->GetFName() != TEXT("IKRetargetSpeedPlantingOpSettings"))
		{
			continue;
		}
		const FDoubleProperty* Strength = CastField<FDoubleProperty>(SettingsType->FindPropertyByName(TEXT("SpringStrength")));
		const FDoubleProperty* Stiffness = CastField<FDoubleProperty>(SettingsType->FindPropertyByName(TEXT("Stiffness")));
		void* Settings = Op->GetSettings();
		if (Strength == nullptr || Stiffness == nullptr || Settings == nullptr)
		{
			continue;
		}
		const double Wanted = Strength->GetPropertyValue_InContainer(Settings);
		const auto Loaded = [](double Value) { return FMath::Sqrt(Value) / (2.0 * PI); };
		double Value = Stiffness->GetPropertyValue_InContainer(Settings);
		if (Wanted < 0.0 || Loaded(Value) == Wanted)
		{
			continue;
		}
		// The nearest Stiffness, then the neighbouring doubles until the load computes the strength exactly.
		Value = FMath::Square(Wanted * (2.0 * PI));
		for (int32 Step = 0; Step < 8 && Loaded(Value) != Wanted; ++Step)
		{
			Value = std::nextafter(Value, Loaded(Value) < Wanted ? TNumericLimits<double>::Max() : 0.0);
		}
		Stiffness->SetPropertyValue_InContainer(Settings, Value);
	}
}

// ---- dispatch ----------------------------------------------------------------------------------------------------

bool RunOp(FEdit& Edit, FReader& Op, const FString& Kind)
{
	if (Kind == TEXT("rig.set")) { return OpRigSet(Edit, Op); }
	if (Kind == TEXT("preview_mesh")) { return OpPreviewMesh(Edit, Op); }
	if (Kind == TEXT("op.add")) { return OpAdd(Edit, Op); }
	if (Kind == TEXT("op.remove")) { return OpRemove(Edit, Op); }
	if (Kind == TEXT("op.rename")) { return OpRename(Edit, Op); }
	if (Kind == TEXT("op.move")) { return OpMove(Edit, Op); }
	if (Kind == TEXT("op.parent")) { return OpParent(Edit, Op); }
	if (Kind == TEXT("op.enable")) { return OpEnable(Edit, Op); }
	if (Kind == TEXT("op.target_rig")) { return OpTargetRig(Edit, Op); }
	if (Kind == TEXT("op.settings")) { return OpSettings(Edit, Op); }
	if (Kind == TEXT("op.map")) { return OpMap(Edit, Op); }
	if (Kind == TEXT("op.chain_settings")) { return OpChainSettings(Edit, Op); }
	if (Kind == TEXT("pose.add")) { return OpPoseAdd(Edit, Op); }
	if (Kind == TEXT("pose.remove")) { return OpPoseRemove(Edit, Op); }
	if (Kind == TEXT("pose.rename")) { return OpPoseRename(Edit, Op); }
	if (Kind == TEXT("pose.current")) { return OpPoseCurrent(Edit, Op); }
	if (Kind == TEXT("pose.set")) { return OpPoseSet(Edit, Op); }
	return Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("Unknown op '%s'."), *Kind), TEXT("op"));
}

bool CheckOpFields(FReader& Op, const FValue& Value, const FString& Kind)
{
	static const TMap<FString, TArray<FString>> Fields = {
		{TEXT("rig.set"), {TEXT("side"), TEXT("rig")}}, {TEXT("preview_mesh"), {TEXT("side"), TEXT("mesh")}},
		{TEXT("op.add"), {TEXT("type"), TEXT("name"), TEXT("parent")}}, {TEXT("op.remove"), {TEXT("name")}},
		{TEXT("op.rename"), {TEXT("name"), TEXT("to")}}, {TEXT("op.move"), {TEXT("name"), TEXT("to")}},
		{TEXT("op.parent"), {TEXT("name"), TEXT("parent")}}, {TEXT("op.enable"), {TEXT("name"), TEXT("enabled")}},
		{TEXT("op.target_rig"), {TEXT("name"), TEXT("rig")}}, {TEXT("op.settings"), {TEXT("name"), TEXT("settings")}},
		{TEXT("op.map"), {TEXT("name"), TEXT("mapping")}}, {TEXT("op.chain_settings"), {TEXT("name"), TEXT("chain"), TEXT("settings")}},
		{TEXT("pose.add"), {TEXT("side"), TEXT("pose")}}, {TEXT("pose.remove"), {TEXT("side"), TEXT("pose")}},
		{TEXT("pose.rename"), {TEXT("side"), TEXT("pose"), TEXT("to")}}, {TEXT("pose.current"), {TEXT("side"), TEXT("pose")}},
		{TEXT("pose.set"), {TEXT("side"), TEXT("pose"), TEXT("root"), TEXT("bones")}}};
	const TArray<FString>* Allowed = Fields.Find(Kind);
	if (Allowed == nullptr)
	{
		return true; // RunOp names the unknown op
	}
	for (const FString& Key : Value.ObjectKeys)
	{
		if (Key != TEXT("op") && !Allowed->Contains(Key))
		{
			return Op.Fail(TEXT("Graph.UnknownField"), FString::Printf(TEXT("'%s' takes %s; '%s' is not one of them."), *Kind,
				*FString::Join(*Allowed, TEXT(", ")), *Key), Key);
		}
	}
	return true;
}
} // namespace

bool HandleIKRetargeterEdit(FCall& Call, FObjectBuilder& Result)
{
	using namespace IKRetargeterEditLocal;
	const FValue* Ops = nullptr;
	if (!Call.Args.OnlyFields({TEXT("ops")}) || !Call.Args.Array(TEXT("ops"), Ops))
	{
		return false;
	}
	UIKRetargeter* Retargeter = Cast<UIKRetargeter>(&Call.Asset);
	if (Retargeter == nullptr)
	{
		return Fail(Call, TEXT("IKRetargeter.UnsupportedAsset"), TEXT("rig.ikretargeter.edit.v1 edits IK Retargeter assets."),
			TEXT("/HandleArguments/asset"));
	}
	// Declared first: the controller is released after the transaction and every controller call ended.
	const TScopedRigController<UIKRetargeter> ControllerScope(*Retargeter);
	UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
	if (Controller == nullptr)
	{
		return Fail(Call, TEXT("IKRetargeter.EditFailed"), TEXT("No IK Retargeter controller for the asset."));
	}
	FAssetDocumentEditorNonInteractiveScope NonInteractive;
	const FScopedTransaction Transaction(LOCTEXT("EditIKRetargeter", "Edit IK Retargeter"));
	FEdit Edit{*Retargeter, *Controller};
	for (int32 Index = 0; Index < Ops->Elements.Num(); ++Index)
	{
		FReader Op = Call.Args.Child(FString::Printf(TEXT("ops/%d"), Index), Ops->Elements[Index]);
		FString Kind;
		if (!Op.IsObject())
		{
			return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("Each op is an object."));
		}
		if (!Op.String(TEXT("op"), Kind) || !CheckOpFields(Op, Ops->Elements[Index], Kind) || !RunOp(Edit, Op, Kind))
		{
			return false;
		}
		if (NonInteractive.WasInteractionRefused())
		{
			NonInteractive.DiscardCreatedPackages();
			return Op.Fail(TEXT("IKRetargeter.InteractiveEditRefused"), TEXT("The edit tried to open an editor dialog, which an MCP request "
				"cannot answer; the edit was refused."));
		}
	}
	if (Edit.bChanged)
	{
		KeepSpeedPlantingStiffness(*Retargeter);
		Retargeter->MarkPackageDirty();
	}
	Result.AddBool(TEXT("changed"), Edit.bChanged);
	return true;
}
} // namespace UE::AssetDocument::Graph::Rig

#undef LOCTEXT_NAMESPACE
