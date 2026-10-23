// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// edit_level apply and verify (design §3.1 steps 5-6). Runs inside the batch
// transaction. Actors and components are re-resolved by path / name at every step:
// a construction script rerun recreates Blueprint components, and the editor may
// collect garbage between steps.

#include "LevelEditTypes.h"
#include "AssetDocumentNameSuggestions.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "EditorActorFolders.h"
#include "Folder.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet2/ComponentEditorUtils.h"
#include "LevelToolResult.h"
#include "LevelWorldIndex.h"
#include "UObject/SoftObjectPath.h"

namespace UE::AssetDocumentLevel::Private
{
namespace ObjectProperties = UE::AssetDocument::EditorObjectProperties;

namespace
{
using EKind = EAssetDocumentRuntimeValueKind;

FLevelValue MakeNumber(double Number)
{
	FLevelValue Value;
	Value.Kind = EKind::Number;
	Value.NumberValue = Number;
	return Value;
}

FLevelValue MakeStruct(TConstArrayView<const TCHAR*> Keys, TConstArrayView<double> Numbers)
{
	FLevelValue Value;
	Value.Kind = EKind::Object;
	for (int32 Index = 0; Index < Keys.Num(); ++Index)
	{
		Value.ObjectKeys.Add(Keys[Index]);
		Value.Elements.Add(MakeNumber(Numbers[Index]));
	}
	return Value;
}

// The member of an authored partial struct, or Fallback.
double MemberOr(const FLevelValue& Authored, const TCHAR* Key, double Fallback)
{
	for (int32 Index = 0; Index < Authored.ObjectKeys.Num() && Index < Authored.Elements.Num(); ++Index)
	{
		if (Authored.ObjectKeys[Index] == Key)
		{
			const FLevelValue& Member = Authored.Elements[Index];
			return Member.Kind == EKind::Integer ? static_cast<double>(Member.IntegerValue) : Member.NumberValue;
		}
	}
	return Fallback;
}

// The root's relative transform as verify reads it back. A construction script rerun
// (any later write to a Blueprint actor) restores the transform through a quaternion, so
// a rotation comes back as the same orientation with last-bit noise (Yaw
// 89.99999999999999 for 90, -90 for 270), as in the Details panel. Rotations compare as
// orientations, location and scale within 1e-6 relative.
bool RootTransformMemberMatches(const USceneComponent& Root, const FString& Property, const FLevelValue& Expected)
{
	if (Property == TEXT("RelativeRotation"))
	{
		const FRotator Stored = Root.GetRelativeRotation();
		const FRotator Wanted(MemberOr(Expected, TEXT("Pitch"), Stored.Pitch), MemberOr(Expected, TEXT("Yaw"), Stored.Yaw),
			MemberOr(Expected, TEXT("Roll"), Stored.Roll));
		return Wanted.Quaternion().Equals(Stored.Quaternion(), 1.0e-9);
	}
	const FVector Stored = Property == TEXT("RelativeLocation") ? Root.GetRelativeLocation() : Root.GetRelativeScale3D();
	const FVector Wanted(MemberOr(Expected, TEXT("X"), Stored.X), MemberOr(Expected, TEXT("Y"), Stored.Y),
		MemberOr(Expected, TEXT("Z"), Stored.Z));
	return Wanted.Equals(Stored, 1.0e-6 * FMath::Max(1.0, Wanted.GetAbsMax()));
}

bool IsRootTransformMember(const FString& Property)
{
	return Property == TEXT("RelativeLocation") || Property == TEXT("RelativeRotation") || Property == TEXT("RelativeScale3D");
}

FString ExpectationKey(const FString& ActorPath, const FString& Component, bool bRoot, const FString& Property)
{
	return FString::Printf(TEXT("%s|%s|%d|%s"), *ActorPath, *Component, bRoot ? 1 : 0, *Property);
}

FString FolderText(const AActor& Actor)
{
	const FName Folder = Actor.GetFolderPath();
	return Folder.IsNone() ? FString() : Folder.ToString();
}
}

UWorld* FLevelEditApply::GetWorld() const
{
	return GEditor != nullptr ? GEditor->GetEditorWorldContext().World() : nullptr;
}

AActor* FLevelEditApply::ResolvePath(const FString& Path) const
{
	if (Path.IsEmpty())
	{
		return nullptr;
	}
	AActor* Actor = Cast<AActor>(FSoftObjectPath(Path).ResolveObject());
	return Actor != nullptr && IsValid(Actor) && !Actor->IsActorBeingDestroyed() ? Actor : nullptr;
}

UObject* FLevelEditApply::ResolveBatchObject(FStringView Id) const
{
	const FString* Path = BatchPaths.Find(FString(Id));
	return Path != nullptr ? ResolvePath(*Path) : nullptr;
}

FActorRef FLevelEditApply::PinRef(const FActorRef& Ref, const FString& Pointer) const
{
	const FString* Path = Ref.Kind != EActorRefKind::BatchId ? RefPaths.Find(Pointer) : nullptr;
	if (Path == nullptr)
	{
		return Ref;
	}
	FActorRef PathRef;
	PathRef.Kind = EActorRefKind::Path;
	PathRef.Value = *Path;
	return PathRef;
}

AActor* FLevelEditApply::ResolveRef(const FActorRef& InRef, const FString& Pointer)
{
	const FActorRef Ref = PinRef(InRef, Pointer);
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		AddError(Pointer, TEXT("Level.TransactionLost"), TEXT("The editor world went away during the batch."));
		return nullptr;
	}
	const FActorRefResolution Resolution = ResolveActorRef(*World, Ref, [this](FStringView Id)
	{
		return Cast<AActor>(ResolveBatchObject(Id));
	});
	AActor* Actor = Resolution.bResolved ? Resolution.Actor.Get() : nullptr;
	if (Actor == nullptr)
	{
		AddError(Pointer, Resolution.bResolved ? FString(TEXT("Level.ActorNotLoaded")) : Resolution.Code,
			Resolution.bResolved ? FString::Printf(TEXT("'%s' is not loaded."), *Resolution.Path) : Resolution.Message);
	}
	return Actor;
}

void FLevelEditApply::AddError(const FString& Path, const FString& Code, const FString& Message, TSharedPtr<FJsonObject> Details)
{
	Diagnostics.Add(MakeLevelDiagnostic(Path, Code, Message, MoveTemp(Details)));
}

void FLevelEditApply::AddWarning(const FString& Path, const FString& Code, const FString& Message, TSharedPtr<FJsonObject> Details)
{
	FAssetDocumentDiagnostic Diagnostic = MakeLevelDiagnostic(Path, Code, Message, MoveTemp(Details));
	Diagnostic.Severity = TEXT("warning");
	Warnings.Add(MoveTemp(Diagnostic));
}

void FLevelEditApply::Touch(const AActor& Actor)
{
	TouchedActorPaths.AddUnique(GetActorPath(Actor));
}

void FLevelEditApply::ExpectAlive(const FString& Path, UClass* Class, const FString& Pointer)
{
	ActorExpectations.RemoveAll([&Path](const FActorExpectation& Existing) { return Existing.Path == Path; });
	FActorExpectation& Expectation = ActorExpectations.AddDefaulted_GetRef();
	Expectation.Path = Path;
	Expectation.Pointer = Pointer;
	Expectation.bAlive = true;
	Expectation.Class = Class;
}

void FLevelEditApply::ExpectGone(const FString& Path, const FString& Pointer)
{
	ActorExpectations.RemoveAll([&Path](const FActorExpectation& Existing) { return Existing.Path == Path; });
	FActorExpectation& Expectation = ActorExpectations.AddDefaulted_GetRef();
	Expectation.Path = Path;
	Expectation.Pointer = Pointer;
	Expectation.bAlive = false;
	DeletedPaths.Add(Path);
}

UObject* FLevelEditApply::ResolveTarget(const FString& ActorPath, const FString& Component, bool bRoot) const
{
	AActor* Actor = ResolvePath(ActorPath);
	if (Actor == nullptr)
	{
		return nullptr;
	}
	if (bRoot)
	{
		return Actor->GetRootComponent();
	}
	return Component.IsEmpty() ? static_cast<UObject*>(Actor) : FindLevelActorComponent(*Actor, Component);
}

bool FLevelEditApply::ApplyPropertyEdit(const FString& ActorPath, const FString& Component, bool bRoot, const FString& Property,
	const FLevelValue& Value, const FString& Pointer, const FString& ChangeOp, const TCHAR* ChangeObject, const FString& ChangeProperty)
{
	AActor* Actor = ResolvePath(ActorPath);
	if (Actor == nullptr)
	{
		AddError(Pointer, TEXT("Level.VerificationMismatch"), FString::Printf(TEXT("Actor %s no longer exists."), *ActorPath));
		return false;
	}
	UObject* Target = ResolveTarget(ActorPath, Component, bRoot);
	if (Target == nullptr)
	{
		AddError(Pointer, bRoot ? TEXT("Level.InvalidRequest") : TEXT("Level.ComponentNotFound"), bRoot
			? FString::Printf(TEXT("%s has no root component, so it has no transform."), *ActorPath)
			: FString::Printf(TEXT("%s has no component '%s'.%s"), *ActorPath, *Component,
				*UE::AssetDocument::DescribeNameCandidates(Component, ListLevelActorComponentNames(*Actor), TEXT("Components"))));
		return false;
	}
	if (UActorComponent* AsComponent = Cast<UActorComponent>(Target); AsComponent != nullptr && !bRoot)
	{
		const USceneComponent* Scene = Cast<USceneComponent>(AsComponent);
		const USceneComponent* Parent = Scene != nullptr ? Scene->GetAttachParent() : nullptr;
		if (AsComponent->CreationMethod == EComponentCreationMethod::UserConstructionScript
			|| !FComponentEditorUtils::CanEditComponentInstance(AsComponent, Parent, false))
		{
			AddError(Pointer, TEXT("Level.ComponentNotEditable"), FString::Printf(TEXT("Component '%s' of %s is %s; its values "
				"cannot be edited on this actor."), *Component, *ActorPath,
				AsComponent->CreationMethod == EComponentCreationMethod::UserConstructionScript
					? TEXT("made by the construction script (rebuilt on every change)") : TEXT("read-only in the Details panel")));
			return false;
		}
	}

	FLevelValue Before;
	const bool bHasBefore = ObjectProperties::Read(*Target, Property, Before);
	const ObjectProperties::FReferencePolicy Policy = MakeLevelReferencePolicy(Actor->GetLevel(),
		[this](FStringView Id) { return ResolveBatchObject(Id); });
	ObjectProperties::FPreparedWrite Prepared;
	TArray<FAssetDocumentRuntimeDiagnostic> Problems;
	FLevelValue Stored;
	const bool bWritten = ObjectProperties::Decode(*Target, Property, Value, Policy, Prepared, Problems)
		&& ObjectProperties::Commit(Prepared, Problems, &Stored);
	if (!bWritten)
	{
		for (const FAssetDocumentRuntimeDiagnostic& Problem : Problems)
		{
			Diagnostics.Add(ProjectLevelPropertyDiagnostic(Problem, Pointer, Property));
		}
		if (Problems.IsEmpty())
		{
			AddError(Pointer, TEXT("PropertyWrite.CommitFailed"), FString::Printf(TEXT("Writing '%s' failed."), *Property));
		}
		return false;
	}
	++ValuesSet;
	if (AActor* Owner = ResolvePath(ActorPath))
	{
		Touch(*Owner);
	}

	if (!bHasBefore || !LevelValuesEqual(Before, Stored))
	{
		FLevelChange& Change = Changes.AddDefaulted_GetRef();
		Change.Op = ChangeOp;
		Change.Kind = TEXT("Replace");
		Change.Object = ChangeObject;
		Change.Actor = ActorPath;
		Change.Component = bRoot ? FString() : Component;
		Change.Property = ChangeProperty;
		Change.Before = bHasBefore ? LevelValueToJson(Before) : MakeShared<FJsonValueNull>();
		Change.After = LevelValueToJson(Stored);
	}

	// The final authored value of each target is what verify checks (struct edits merge).
	const FString Key = ExpectationKey(ActorPath, Component, bRoot, Property);
	FPropertyExpectation* Existing = PropertyExpectations.FindByPredicate([&Key](const FPropertyExpectation& Expectation)
	{
		return ExpectationKey(Expectation.ActorPath, Expectation.Component, Expectation.bRoot, Expectation.Property) == Key;
	});
	if (Existing != nullptr)
	{
		Existing->Authored = MergeAuthoredValue(Existing->Authored, Value);
		Existing->Pointer = Pointer;
	}
	else
	{
		FPropertyExpectation& Expectation = PropertyExpectations.AddDefaulted_GetRef();
		Expectation.ActorPath = ActorPath;
		Expectation.Component = Component;
		Expectation.bRoot = bRoot;
		Expectation.Property = Property;
		Expectation.Authored = Value;
		Expectation.Pointer = Pointer;
	}
	return true;
}

bool FLevelEditApply::ApplyTransform(const FString& ActorPath, const FLevelTransformEdit& Transform)
{
	for (int32 Member = 0; Member < 3; ++Member)
	{
		if (!Transform.Members[Member].IsSet())
		{
			continue;
		}
		const ELevelTransformMember Kind = static_cast<ELevelTransformMember>(Member);
		const FString MemberPointer = Transform.Pointer + TEXT("/") + FLevelTransformEdit::MemberField(Kind);
		AActor* Actor = ResolvePath(ActorPath);
		USceneComponent* Root = Actor != nullptr ? Actor->GetRootComponent() : nullptr;
		if (Root == nullptr)
		{
			AddError(Transform.Pointer, TEXT("Level.InvalidRequest"), FString::Printf(TEXT("%s has no root component, so it has "
				"no transform."), *ActorPath));
			return false;
		}
		FLevelValue Value = Transform.Members[Member].GetValue();
		const USceneComponent* Parent = Root->GetAttachParent();
		if (Transform.bWorld && Parent != nullptr)
		{
			// The Details panel's world mode: the given world members, the others kept, as
			// a relative value under the attach parent (socket).
			const FTransform ParentWorld = Parent->GetSocketTransform(Root->GetAttachSocketName());
			const FTransform Current = Root->GetComponentTransform();
			switch (Kind)
			{
			case ELevelTransformMember::Location:
			{
				const FVector Now = Current.GetLocation();
				const FVector World(MemberOr(Value, TEXT("X"), Now.X), MemberOr(Value, TEXT("Y"), Now.Y), MemberOr(Value, TEXT("Z"), Now.Z));
				const FVector Relative = Root->IsUsingAbsoluteLocation() ? World : ParentWorld.InverseTransformPosition(World);
				Value = MakeStruct({TEXT("X"), TEXT("Y"), TEXT("Z")}, {Relative.X, Relative.Y, Relative.Z});
				break;
			}
			case ELevelTransformMember::Rotation:
			{
				const FRotator Now = Current.Rotator();
				const FRotator World(MemberOr(Value, TEXT("Pitch"), Now.Pitch), MemberOr(Value, TEXT("Yaw"), Now.Yaw),
					MemberOr(Value, TEXT("Roll"), Now.Roll));
				const FRotator Relative = Root->IsUsingAbsoluteRotation() ? World
					: (ParentWorld.GetRotation().Inverse() * World.Quaternion()).Rotator();
				Value = MakeStruct({TEXT("Pitch"), TEXT("Yaw"), TEXT("Roll")}, {Relative.Pitch, Relative.Yaw, Relative.Roll});
				break;
			}
			case ELevelTransformMember::Scale:
			default:
			{
				const FVector Now = Current.GetScale3D();
				const FVector World(MemberOr(Value, TEXT("X"), Now.X), MemberOr(Value, TEXT("Y"), Now.Y), MemberOr(Value, TEXT("Z"), Now.Z));
				const FVector ParentScale = ParentWorld.GetScale3D();
				const FVector Relative = Root->IsUsingAbsoluteScale() ? World : FVector(
					FMath::IsNearlyZero(ParentScale.X) ? World.X : World.X / ParentScale.X,
					FMath::IsNearlyZero(ParentScale.Y) ? World.Y : World.Y / ParentScale.Y,
					FMath::IsNearlyZero(ParentScale.Z) ? World.Z : World.Z / ParentScale.Z);
				Value = MakeStruct({TEXT("X"), TEXT("Y"), TEXT("Z")}, {Relative.X, Relative.Y, Relative.Z});
				break;
			}
			}
		}
		if (!ApplyPropertyEdit(ActorPath, FString(), true, FLevelTransformEdit::MemberProperty(Kind), Value, MemberPointer,
			MemberPointer, TEXT("transform"), FLevelTransformEdit::MemberField(Kind)))
		{
			return false;
		}
	}
	return true;
}

bool FLevelEditApply::ApplyFields(const FString& ActorPath, const FLevelActorFieldEdits& Edits, const FString& OpPointer)
{
	if (Edits.Transform.IsSet() && !ApplyTransform(ActorPath, Edits.Transform.GetValue()))
	{
		return false;
	}

	if (Edits.Label.IsSet())
	{
		AActor* Actor = ResolvePath(ActorPath);
		if (Actor == nullptr)
		{
			AddError(Edits.LabelPointer, TEXT("Level.VerificationMismatch"), FString::Printf(TEXT("Actor %s no longer exists."), *ActorPath));
			return false;
		}
		const FString Before = Actor->GetActorLabel(false);
		if (!Before.Equals(Edits.Label.GetValue(), ESearchCase::CaseSensitive))
		{
			// The Outliner rename (fixes soft references to the actor; labels are not made unique).
			FActorLabelUtilities::RenameExistingActor(Actor, Edits.Label.GetValue(), false);
			FLevelChange& Change = Changes.AddDefaulted_GetRef();
			Change.Op = Edits.LabelPointer;
			Change.Kind = TEXT("Replace");
			Change.Object = TEXT("label");
			Change.Actor = ActorPath;
			Change.Before = MakeShared<FJsonValueString>(Before);
			Change.After = MakeShared<FJsonValueString>(Edits.Label.GetValue());
			Touch(*Actor);
		}
		FieldExpectations.RemoveAll([&ActorPath](const FFieldExpectation& Existing)
		{
			return Existing.ActorPath == ActorPath && Existing.Field == TEXT("label");
		});
		FieldExpectations.Add({ActorPath, TEXT("label"), Edits.Label.GetValue(), Edits.LabelPointer});
	}

	if (Edits.Folder.IsSet())
	{
		AActor* Actor = ResolvePath(ActorPath);
		if (Actor == nullptr)
		{
			AddError(Edits.FolderPointer, TEXT("Level.VerificationMismatch"), FString::Printf(TEXT("Actor %s no longer exists."), *ActorPath));
			return false;
		}
		const FString Before = FolderText(*Actor);
		if (!Before.Equals(Edits.Folder.GetValue(), ESearchCase::CaseSensitive))
		{
			// The Outliner's drag into a folder: attached children follow; the folder is
			// created when it does not exist.
			Actor->SetFolderPath_Recursively(Edits.Folder.GetValue().IsEmpty() ? FName(NAME_None) : FName(*Edits.Folder.GetValue()));
			FLevelChange& Change = Changes.AddDefaulted_GetRef();
			Change.Op = Edits.FolderPointer;
			Change.Kind = TEXT("Replace");
			Change.Object = TEXT("folder");
			Change.Actor = ActorPath;
			Change.Before = MakeShared<FJsonValueString>(Before);
			Change.After = MakeShared<FJsonValueString>(Edits.Folder.GetValue());
			Touch(*Actor);
		}
		FieldExpectations.RemoveAll([&ActorPath](const FFieldExpectation& Existing)
		{
			return Existing.ActorPath == ActorPath && Existing.Field == TEXT("folder");
		});
		FieldExpectations.Add({ActorPath, TEXT("folder"), Edits.Folder.GetValue(), Edits.FolderPointer});
	}

	if (Edits.Tags.IsSet() && !ApplyPropertyEdit(ActorPath, FString(), false, TEXT("Tags"), Edits.Tags.GetValue(), Edits.TagsPointer,
		Edits.TagsPointer, TEXT("tags"), FString()))
	{
		return false;
	}
	for (const FLevelPropertyEdit& Edit : Edits.Properties)
	{
		if (!ApplyPropertyEdit(ActorPath, FString(), false, Edit.Name, Edit.Value, Edit.Pointer, Edit.Pointer, TEXT("property"), Edit.Name))
		{
			return false;
		}
	}
	for (const FLevelComponentEdit& Component : Edits.Components)
	{
		for (const FLevelPropertyEdit& Edit : Component.Properties)
		{
			if (!ApplyPropertyEdit(ActorPath, Component.Name, false, Edit.Name, Edit.Value, Edit.Pointer, Edit.Pointer,
				TEXT("property"), Edit.Name))
			{
				return false;
			}
		}
	}
	return ApplyLayers(ActorPath, Edits);
}

bool FLevelEditApply::VerifyProperty(const FPropertyExpectation& Expectation)
{
	const int32 Before = Diagnostics.Num();
	if (DeletedPaths.Contains(Expectation.ActorPath))
	{
		return true;
	}
	AActor* Actor = ResolvePath(Expectation.ActorPath);
	UObject* Target = ResolveTarget(Expectation.ActorPath, Expectation.Component, Expectation.bRoot);
	if (Actor == nullptr || Target == nullptr)
	{
		AddError(Expectation.Pointer, TEXT("Level.VerificationMismatch"), FString::Printf(TEXT("%s%s no longer exists after the "
			"batch (a construction script rerun removed it), so '%s' could not be read back."), *Expectation.ActorPath,
			Expectation.Component.IsEmpty() ? TEXT("") : *(TEXT(" component ") + Expectation.Component), *Expectation.Property));
		return false;
	}
	const ObjectProperties::FReferencePolicy Policy = MakeLevelReferencePolicy(Actor->GetLevel(),
		[this](FStringView Id) { return ResolveBatchObject(Id); });
	FLevelValue Expected;
	TArray<FAssetDocumentRuntimeDiagnostic> Problems;
	FLevelValue Stored;
	if (!ObjectProperties::PrepareExpected(*Target, Expectation.Property, Expectation.Authored, Policy, Expected, Problems)
		|| !ObjectProperties::EncodeAsAuthored(*Target, Expectation.Property, Expected, Stored))
	{
		for (const FAssetDocumentRuntimeDiagnostic& Problem : Problems)
		{
			Diagnostics.Add(ProjectLevelPropertyDiagnostic(Problem, Expectation.Pointer, Expectation.Property));
		}
		if (Problems.IsEmpty())
		{
			AddError(Expectation.Pointer, TEXT("Level.VerificationMismatch"), FString::Printf(TEXT("'%s' could not be read back."),
				*Expectation.Property));
		}
		return false;
	}
	FString Difference;
	const USceneComponent* Root = Expectation.bRoot && IsRootTransformMember(Expectation.Property) ? Cast<USceneComponent>(Target) : nullptr;
	const bool bMatches = Root != nullptr ? RootTransformMemberMatches(*Root, Expectation.Property, Expected)
		: LevelValuesEqual(Expected, Stored, &Difference);
	if (!bMatches)
	{
		TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetField(TEXT("expected"), LevelValueToJson(Expected));
		Details->SetField(TEXT("stored"), LevelValueToJson(Stored));
		AddError(Expectation.Pointer + Difference, TEXT("Level.PropertyValueNotStoredExactly"), FString::Printf(TEXT("'%s' holds %s "
			"after the batch, not the requested %s: the editor changed it after the write (a clamp or PostEditChange rule, or the "
			"Blueprint construction script). Write the stored value, or change what overrides it."), *Expectation.Property,
			*LevelValueToText(Stored), *LevelValueToText(Expected)), Details);
	}
	return Diagnostics.Num() == Before;
}

bool FLevelEditApply::Verify()
{
	const int32 Before = Diagnostics.Num();
	for (const FActorExpectation& Expectation : ActorExpectations)
	{
		AActor* Actor = ResolvePath(Expectation.Path);
		if (Expectation.bAlive)
		{
			UClass* Class = Expectation.Class.Get();
			if (Actor == nullptr || !IsListedActor(*Actor))
			{
				AddError(Expectation.Pointer, TEXT("Level.VerificationMismatch"), FString::Printf(TEXT("Actor %s does not exist after "
					"the batch (an editor step after the op removed it)."), *Expectation.Path));
			}
			else if (Class != nullptr && Actor->GetClass() != Class)
			{
				AddError(Expectation.Pointer, TEXT("Level.VerificationMismatch"), FString::Printf(TEXT("Actor %s is a %s, expected %s."),
					*Expectation.Path, *Actor->GetClass()->GetPathName(), *Class->GetPathName()));
			}
		}
		else if (Actor != nullptr && IsListedActor(*Actor))
		{
			AddError(Expectation.Pointer, TEXT("Level.VerificationMismatch"), FString::Printf(TEXT("Actor %s still exists after its "
				"delete (the editor refused to delete it)."), *Expectation.Path));
		}
	}

	for (const FFieldExpectation& Expectation : FieldExpectations)
	{
		if (DeletedPaths.Contains(Expectation.ActorPath))
		{
			continue;
		}
		AActor* Actor = ResolvePath(Expectation.ActorPath);
		if (Actor == nullptr)
		{
			continue;
		}
		const FString Stored = Expectation.Field == TEXT("label") ? Actor->GetActorLabel(false) : FolderText(*Actor);
		if (!Stored.Equals(Expectation.Value, ESearchCase::CaseSensitive))
		{
			TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetStringField(TEXT("expected"), Expectation.Value);
			Details->SetStringField(TEXT("stored"), Stored);
			AddError(Expectation.Pointer, TEXT("Level.VerificationMismatch"), FString::Printf(TEXT("The %s of %s is '%s' after the "
				"batch, not '%s'."), *Expectation.Field, *Expectation.ActorPath, *Stored, *Expectation.Value), Details);
		}
	}

	for (const FPropertyExpectation& Expectation : PropertyExpectations)
	{
		VerifyProperty(Expectation);
	}
	VerifyStructure();
	return Diagnostics.Num() == Before;
}

FLevelChange& FLevelEditApply::AddChange(const FString& Op, const TCHAR* Kind, const TCHAR* Object, const FString& Actor)
{
	FLevelChange& Change = Changes.AddDefaulted_GetRef();
	Change.Op = Op;
	Change.Kind = Kind;
	Change.Object = Object;
	Change.Actor = Actor;
	return Change;
}

void FLevelEditApply::ExpectComponent(const FString& ActorPath, const FString& Name, UClass* Class, const FString& Parent,
	const FString& Socket, const FString& Pointer)
{
	ComponentExpectations.RemoveAll([&](const FComponentExpectation& Existing)
	{
		return Existing.ActorPath == ActorPath && Existing.Name == Name;
	});
	FComponentExpectation& Expectation = ComponentExpectations.AddDefaulted_GetRef();
	Expectation.ActorPath = ActorPath;
	Expectation.Name = Name;
	Expectation.bAlive = true;
	Expectation.Class = Class;
	Expectation.Parent = Parent;
	Expectation.Socket = Socket;
	Expectation.Pointer = Pointer;
}

void FLevelEditApply::ExpectComponentGone(const FString& ActorPath, const FString& Name, const FString& Pointer)
{
	ComponentExpectations.RemoveAll([&](const FComponentExpectation& Existing)
	{
		return Existing.ActorPath == ActorPath && Existing.Name == Name;
	});
	FComponentExpectation& Expectation = ComponentExpectations.AddDefaulted_GetRef();
	Expectation.ActorPath = ActorPath;
	Expectation.Name = Name;
	Expectation.bAlive = false;
	Expectation.Pointer = Pointer;
	RemovedComponents.Add(ActorPath + TEXT("|") + Name);
}

void FLevelEditApply::ForgetComponent(const FString& ActorPath, const FString& Name, const FString& NewParentForChildren)
{
	PropertyExpectations.RemoveAll([&](const FPropertyExpectation& Existing)
	{
		return Existing.ActorPath == ActorPath && !Existing.bRoot && Existing.Component == Name;
	});
	for (FComponentExpectation& Expectation : ComponentExpectations)
	{
		if (Expectation.ActorPath == ActorPath && Expectation.bAlive && Expectation.Parent == Name)
		{
			Expectation.Parent = NewParentForChildren;
		}
	}
}

void FLevelEditApply::ExpectAttachment(const FString& ActorPath, const FString& ParentPath, const FString& Socket,
	const FString& Component, const FString& Pointer)
{
	AttachExpectations.RemoveAll([&ActorPath](const FAttachExpectation& Existing) { return Existing.ActorPath == ActorPath; });
	AttachExpectations.Add({ActorPath, ParentPath, Socket, Component, Pointer});
}

void FLevelEditApply::ExpectFolder(const FString& Path, bool bExists, const FString& Pointer)
{
	FolderExpectations.RemoveAll([&Path](const FFolderExpectation& Existing) { return Existing.Path == Path; });
	FolderExpectations.Add({Path, bExists, Pointer});
}

void FLevelEditApply::MoveFolderExpectations(const FString& From, const FString& To)
{
	for (FFieldExpectation& Expectation : FieldExpectations)
	{
		if (Expectation.Field == TEXT("folder") && (Expectation.Value == From || Expectation.Value.StartsWith(From + TEXT("/"))))
		{
			const FString Rest = Expectation.Value.RightChop(From.Len());
			Expectation.Value = To.IsEmpty() ? Rest.RightChop(Rest.StartsWith(TEXT("/")) ? 1 : 0) : To + Rest;
		}
	}
	for (FFolderExpectation& Expectation : FolderExpectations)
	{
		if (Expectation.bExists && (Expectation.Path == From || Expectation.Path.StartsWith(From + TEXT("/"))))
		{
			const FString Rest = Expectation.Path.RightChop(From.Len());
			Expectation.Path = To.IsEmpty() ? Rest.RightChop(Rest.StartsWith(TEXT("/")) ? 1 : 0) : To + Rest;
		}
	}
	FolderExpectations.RemoveAll([](const FFolderExpectation& Expectation) { return Expectation.bExists && Expectation.Path.IsEmpty(); });
}

bool FLevelEditApply::SettleProperties(TFunctionRef<bool(const FPropertyExpectation&)> Filter)
{
	bool bOk = true;
	for (const FPropertyExpectation& Expectation : PropertyExpectations)
	{
		if (Filter(Expectation))
		{
			bOk &= VerifyProperty(Expectation);
		}
	}
	PropertyExpectations.RemoveAll([&Filter](const FPropertyExpectation& Expectation) { return Filter(Expectation); });
	return bOk;
}

bool FLevelEditApply::SettleBeforeReparent(AActor& Actor, const FString& Pointer)
{
	const FString Path = GetActorPath(Actor);
	// Folder expectations of the actor and of the actors attached below it give way to
	// the editor's rule (the topmost parent's folder).
	TArray<AActor*> Moved;
	Actor.GetAttachedActors(Moved, /*bResetArray*/ true, /*bRecursivelyIncludeAttachedActors*/ true);
	TSet<FString> MovedPaths;
	MovedPaths.Add(Path);
	for (const AActor* Attached : Moved)
	{
		if (Attached != nullptr)
		{
			MovedPaths.Add(GetActorPath(*Attached));
		}
	}
	FieldExpectations.RemoveAll([&MovedPaths](const FFieldExpectation& Expectation)
	{
		return Expectation.Field == TEXT("folder") && MovedPaths.Contains(Expectation.ActorPath);
	});
	return SettleProperties([&Path](const FPropertyExpectation& Expectation)
	{
		return Expectation.ActorPath == Path && Expectation.bRoot && IsRootTransformMember(Expectation.Property);
	});
}

bool FLevelEditApply::SettleComponentTransform(const FString& ActorPath, const FString& Name)
{
	AActor* Actor = ResolvePath(ActorPath);
	const bool bIsRoot = Actor != nullptr && Actor->GetRootComponent() != nullptr && Actor->GetRootComponent()->GetName() == Name;
	return SettleProperties([&](const FPropertyExpectation& Expectation)
	{
		return Expectation.ActorPath == ActorPath && IsRootTransformMember(Expectation.Property)
			&& ((!Expectation.bRoot && Expectation.Component == Name) || (Expectation.bRoot && bIsRoot));
	});
}

bool FLevelEditApply::VerifyStructure()
{
	const int32 Before = Diagnostics.Num();
	for (const FComponentExpectation& Expectation : ComponentExpectations)
	{
		AActor* Actor = DeletedPaths.Contains(Expectation.ActorPath) ? nullptr : ResolvePath(Expectation.ActorPath);
		if (Actor == nullptr)
		{
			continue;
		}
		UActorComponent* Component = FindLevelActorComponent(*Actor, Expectation.Name);
		if (!Expectation.bAlive)
		{
			if (Component != nullptr)
			{
				AddError(Expectation.Pointer, TEXT("Level.VerificationMismatch"), FString::Printf(TEXT("Component '%s' of %s still exists "
					"after its removal."), *Expectation.Name, *Expectation.ActorPath));
			}
			continue;
		}
		const USceneComponent* Scene = Cast<USceneComponent>(Component);
		const USceneComponent* Parent = Scene != nullptr ? Scene->GetAttachParent() : nullptr;
		const FString StoredParent = Parent != nullptr && Parent->GetOwner() == Actor ? Parent->GetName() : FString();
		const FString StoredSocket = Scene != nullptr && !Scene->GetAttachSocketName().IsNone() ? Scene->GetAttachSocketName().ToString() : FString();
		if (Component == nullptr)
		{
			AddError(Expectation.Pointer, TEXT("Level.VerificationMismatch"), FString::Printf(TEXT("Component '%s' of %s does not exist "
				"after the batch (a construction script rerun removed it)."), *Expectation.Name, *Expectation.ActorPath));
		}
		else if ((Expectation.Class.IsValid() && Component->GetClass() != Expectation.Class.Get()) || StoredParent != Expectation.Parent
			|| StoredSocket != Expectation.Socket)
		{
			AddError(Expectation.Pointer, TEXT("Level.VerificationMismatch"), FString::Printf(TEXT("Component '%s' of %s is a %s under "
				"'%s' (socket '%s') after the batch, not a %s under '%s' (socket '%s')."), *Expectation.Name, *Expectation.ActorPath,
				*Component->GetClass()->GetName(), *StoredParent, *StoredSocket,
				Expectation.Class.IsValid() ? *Expectation.Class->GetName() : TEXT("?"), *Expectation.Parent, *Expectation.Socket));
		}
	}

	for (const FAttachExpectation& Expectation : AttachExpectations)
	{
		AActor* Actor = DeletedPaths.Contains(Expectation.ActorPath) ? nullptr : ResolvePath(Expectation.ActorPath);
		if (Actor == nullptr)
		{
			continue;
		}
		const AActor* Parent = Actor->GetAttachParentActor();
		const FString StoredParent = Parent != nullptr ? GetActorPath(*Parent) : FString();
		const FName SocketName = Actor->GetAttachParentSocketName();
		const FString StoredSocket = SocketName.IsNone() ? FString() : SocketName.ToString();
		const USceneComponent* Root = Actor->GetRootComponent();
		const USceneComponent* AttachComponent = Root != nullptr ? Root->GetAttachParent() : nullptr;
		const bool bComponentMatches = Expectation.Component.IsEmpty()
			|| (AttachComponent != nullptr && AttachComponent->GetName() == Expectation.Component);
		if (StoredParent != Expectation.ParentPath || StoredSocket != Expectation.Socket || !bComponentMatches)
		{
			AddError(Expectation.Pointer, TEXT("Level.VerificationMismatch"), FString::Printf(TEXT("%s is attached to '%s' (socket '%s') "
				"after the batch, not to '%s' (socket '%s')."), *Expectation.ActorPath, *StoredParent, *StoredSocket,
				*Expectation.ParentPath, *Expectation.Socket));
		}
	}

	UWorld* World = GetWorld();
	for (const FFolderExpectation& Expectation : FolderExpectations)
	{
		const bool bExists = World != nullptr && FActorFolders::Get().ContainsFolder(*World,
			FFolder(FFolder::GetWorldRootFolder(World).GetRootObject(), FName(*Expectation.Path)));
		if (bExists != Expectation.bExists)
		{
			AddError(Expectation.Pointer, TEXT("Level.VerificationMismatch"), FString::Printf(TEXT("Folder '%s' %s after the batch."),
				*Expectation.Path, bExists ? TEXT("still exists") : TEXT("does not exist")));
		}
	}
	return Diagnostics.Num() == Before && VerifyLists();
}
}
