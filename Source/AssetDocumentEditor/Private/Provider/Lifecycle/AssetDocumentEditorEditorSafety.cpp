// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Lifecycle/AssetDocumentEditorEditorSafety.h"

#include "Engine/Engine.h"
#include "Factories/Factory.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/CoreDelegates.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/PackageName.h"
#include "Provider/Lifecycle/AssetDocumentEditorOrphanPackageCleanup.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "Widgets/SWindow.h"

namespace
{
// Logged by FSlateApplication::AddModalWindow when GIsRunningUnattendedScript
// cancels a modal window.
constexpr const TCHAR* CanceledModalLogText =
	TEXT("A modal window tried to take control while running in unattended script mode");

// Factory property that FactoryCreateNew dereferences without a null check and
// that only the factory's ConfigureProperties dialog (or a factory_context) sets.
struct FFactoryRequirement
{
	const TCHAR* FactoryClassPath;
	const TCHAR* PropertyName;
	const TCHAR* Needed;
};

constexpr FFactoryRequirement FactoryRequirements[] = {
	// FKismetCompilerModule::GetBlueprintTypesForClass(nullptr) asserts.
	{TEXT("/Script/UnrealEd.LightWeightInstanceFactory"), TEXT("ParentClass"),
		TEXT("the parent actor class chosen in its New-asset dialog")},
	// NewObject<USlateWidgetStyleContainerBase>(.., nullptr class) asserts.
	{TEXT("/Script/UnrealEd.SlateWidgetStyleAssetFactory"), TEXT("StyleType"),
		TEXT("the widget style class chosen in its New-asset dialog")},
	// UStaticMesh::GetBoundingBox on a null StaticMesh crashes.
	{TEXT("/Script/SkeletalMeshModelingTools.SkeletonFromStaticMeshFactory"), TEXT("StaticMesh"),
		TEXT("the static mesh to build the skeleton from")},
	// Reports a modal error and returns null without a skeletal mesh.
	{TEXT("/Script/UnrealEd.SkeletonFactory"), TEXT("TargetSkeletalMesh"),
		TEXT("the skeletal mesh to build the skeleton from")},
	// Reports a modal error and returns null without a skeletal mesh.
	{TEXT("/Script/UnrealEd.PhysicsAssetFactory"), TEXT("TargetSkeletalMesh"),
		TEXT("the skeletal mesh the physics asset is made for")},
};

bool IsClassOrChildOfPath(const UClass* Class, const TCHAR* ClassPath)
{
	for (const UClass* Current = Class; Current != nullptr; Current = Current->GetSuperClass())
	{
		if (Current->GetPathName() == ClassPath)
		{
			return true;
		}
	}
	return false;
}

bool IsFactoryPropertySet(const UFactory& Factory, const TCHAR* PropertyName)
{
	const FProperty* Property = Factory.GetClass()->FindPropertyByName(PropertyName);
	if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
	{
		return ObjectProperty->GetObjectPropertyValue_InContainer(&Factory) != nullptr;
	}
	// A renamed or retyped engine property: do not guess, let the factory run.
	return true;
}
}

FDelegateHandle FAssetDocumentEditorNonInteractiveScope::AddPreSlateModalHandler(TFunction<void()> Handler)
{
#if WITH_EDITOR
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	return FCoreDelegates::PreSlateModalWithContext.AddLambda(
		[Handler = MoveTemp(Handler)](const FCoreDelegates::FModalWindowContext&) { Handler(); });
#else
	return FCoreDelegates::PreSlateModal.AddLambda(MoveTemp(Handler));
#endif
#else
	(void)Handler;
	return FDelegateHandle();
#endif
}

void FAssetDocumentEditorNonInteractiveScope::RemovePreSlateModalHandler(FDelegateHandle Handle)
{
#if WITH_EDITOR
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	FCoreDelegates::PreSlateModalWithContext.Remove(Handle);
#else
	FCoreDelegates::PreSlateModal.Remove(Handle);
#endif
#else
	(void)Handle;
#endif
}

FAssetDocumentEditorNonInteractiveScope::FAssetDocumentEditorNonInteractiveScope()
	: UnattendedScript(GIsRunningUnattendedScript, true)
{
	check(IsInGameThread());
	GLog->AddOutputDevice(this);
	GUObjectArray.AddUObjectCreateListener(this);
	PreSlateModalHandle = AddPreSlateModalHandler([this]()
	{
		// A slow task's progress window (FScopedSlowTask::MakeDialog -> FFeedbackContextEditor::StartSlowTask,
		// e.g. "Updating Texture Streaming Data" in every material recompile) is added with
		// AddModalWindow(bSlowTaskWindow = true): unattended mode lets it through and it broadcasts
		// the pre-Slate-modal event, but it never blocks and asks nothing. FFeedbackContext::StartSlowTask sets GIsSlowTask
		// before the window is added. A blocking modal never reaches this broadcast inside the scope (the
		// unattended check cancels it first, see Serialize), or, if the flag was cleared, its modal loop ticks
		// (OnModalLoopTick); both still refuse it, also while a progress window is up.
		if (!GIsSlowTask)
		{
			bInteractionRefused = true;
		}
	});
	if (FSlateApplication::IsInitialized())
	{
		ModalLoopTickHandle = FSlateApplication::Get().GetOnModalLoopTickEvent().AddRaw(
			this, &FAssetDocumentEditorNonInteractiveScope::OnModalLoopTick);
	}
	bAttached = true;
}

FAssetDocumentEditorNonInteractiveScope::~FAssetDocumentEditorNonInteractiveScope()
{
	Detach();
}

void FAssetDocumentEditorNonInteractiveScope::Detach()
{
	if (!bAttached)
	{
		return;
	}
	bAttached = false;
	if (ModalLoopTickHandle.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().GetOnModalLoopTickEvent().Remove(ModalLoopTickHandle);
	}
	RemovePreSlateModalHandler(PreSlateModalHandle);
	GUObjectArray.RemoveUObjectCreateListener(this);
	if (GLog != nullptr)
	{
		GLog->RemoveOutputDevice(this);
	}
}

bool FAssetDocumentEditorNonInteractiveScope::WasInteractionRefused() const
{
	return bInteractionRefused;
}

void FAssetDocumentEditorNonInteractiveScope::Serialize(
	const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category)
{
	(void)Verbosity;
	static const FName SlateCategory(TEXT("LogSlate"));
	if (Message != nullptr && Category == SlateCategory
		&& FCString::Strifind(Message, CanceledModalLogText) != nullptr)
	{
		bInteractionRefused = true;
	}
}

void FAssetDocumentEditorNonInteractiveScope::NotifyUObjectCreated(const UObjectBase* Object, int32 Index)
{
	if (Object != nullptr && Object->GetClass() == UPackage::StaticClass())
	{
		FScopeLock Lock(&CreatedLock);
		CreatedPackageIndices.Add(Index);
	}
}

void FAssetDocumentEditorNonInteractiveScope::OnUObjectArrayShutdown()
{
	GUObjectArray.RemoveUObjectCreateListener(this);
}

void FAssetDocumentEditorNonInteractiveScope::OnModalLoopTick(float DeltaTime)
{
	(void)DeltaTime;
	// A modal loop started despite the unattended flag (for example code that
	// cleared it): close the window on its first tick so the call returns.
	bInteractionRefused = true;
	if (const TSharedPtr<SWindow> Modal = FSlateApplication::Get().GetActiveModalWindow())
	{
		Modal->RequestDestroyWindow();
	}
}

bool FAssetDocumentEditorNonInteractiveScope::DiscardCreatedPackages()
{
	TArray<int32> Indices;
	{
		FScopeLock Lock(&CreatedLock);
		Indices = CreatedPackageIndices;
	}
	bool bAllDiscarded = true;
	for (const int32 Index : Indices)
	{
		const FUObjectItem* Item = GUObjectArray.IndexToObject(Index);
		UPackage* Package = Item != nullptr ? Cast<UPackage>(static_cast<UObject*>(Item->GetObject())) : nullptr;
		if (Package == nullptr || Package == GetTransientPackage() || !IsValid(Package))
		{
			continue;
		}
		bAllDiscarded &= DiscardAssetDocumentEditorOrphanCreatedPackage(Package->GetName());
	}
	return bAllDiscarded;
}

FString FAssetDocumentEditorNonInteractiveScope::DescribeRefusal(const UClass* FactoryClass, const UClass* AssetClass)
{
	return FString::Printf(
		TEXT("%s tried to open a modal editor dialog while creating %s. Interactive factories cannot run "
			"through the MCP (the call was cancelled and nothing was created); create this asset in the "
			"editor, then Update it with a document."),
		*GetPathNameSafe(FactoryClass), *GetPathNameSafe(AssetClass));
}

bool CheckAssetDocumentEditorFactoryRequirements(
	const UFactory& Factory, const UClass& AssetClass, FString& OutCode, FString& OutMessage)
{
	for (const FFactoryRequirement& Requirement : FactoryRequirements)
	{
		if (IsClassOrChildOfPath(Factory.GetClass(), Requirement.FactoryClassPath)
			&& !IsFactoryPropertySet(Factory, Requirement.PropertyName))
		{
			OutCode = TEXT("FactoryCreate.MissingFactoryContext");
			// What the document's FactoryContext names for that dialog field: a class for a class picker, else
			// an existing asset.
			const FProperty* Field = Factory.GetClass()->FindPropertyByName(Requirement.PropertyName);
			FString Supply;
			if (const FClassProperty* ClassField = CastField<FClassProperty>(Field))
			{
				Supply = FString::Printf(TEXT("a class path (/Script/Module.Class, or a Blueprint's ..._C class) of a concrete subclass of %s"),
					*GetPathNameSafe(ClassField->MetaClass));
			}
			else if (const FObjectPropertyBase* ObjectField = CastField<FObjectPropertyBase>(Field))
			{
				Supply = FString::Printf(TEXT("the object path of an existing %s asset"), *GetPathNameSafe(ObjectField->PropertyClass));
			}
			OutMessage = FString::Printf(
				TEXT("%s needs %s (factory property '%s'), which the editor asks for in its New-asset dialog, to create %s. %s"),
				*Factory.GetClass()->GetPathName(), Requirement.Needed, Requirement.PropertyName, *AssetClass.GetPathName(),
				Supply.IsEmpty()
					? TEXT("A document cannot supply it: create the asset in the editor, then Update it with a document.")
					: *FString::Printf(TEXT("Set the document's FactoryContext to %s."), *Supply));
			return false;
		}
	}
	return true;
}

bool CheckAssetDocumentEditorScratchCloneSupported(const UObject& Source, FString& OutMessage)
{
	static const TCHAR* MetaSoundDocumentInterface = TEXT("/Script/MetasoundFrontend.MetaSoundDocumentInterface");
	const UClass* Interface = FindObject<UClass>(nullptr, MetaSoundDocumentInterface);
	if (Interface != nullptr && Source.GetClass()->ImplementsInterface(Interface))
	{
		OutMessage = FString::Printf(
			TEXT("%s is a MetaSound document; MetaSound assets cannot be duplicated into a transient preview "
				"(the MetaSound builder requires a package asset). Validate's trial copies it as a staged asset "
				"instead; Apply edits the asset itself."),
			*Source.GetClass()->GetPathName());
		return false;
	}
	return true;
}

bool CheckAssetDocumentEditorChangeNotificationSupported(
	const UObject& Object, FString& OutCode, FString& OutMessage)
{
	// USoundEffectSourcePresetChain::PostEditChangeProperty calls
	// GEngine->GetAudioDeviceManager()->UpdateSourceEffectChain without a null check.
	if (IsClassOrChildOfPath(Object.GetClass(), TEXT("/Script/Engine.SoundEffectSourcePresetChain"))
		&& (GEngine == nullptr || GEngine->GetAudioDeviceManager() == nullptr))
	{
		OutCode = TEXT("PropertyWrite.EditorServiceUnavailable");
		OutMessage = FString::Printf(
			TEXT("%s notifies the audio device manager when a property changes, and this editor runs without "
				"audio (-NoSound or no audio device). Property writes are refused instead of crashing; run the "
				"editor with audio to edit this asset."),
			*Object.GetClass()->GetPathName());
		return false;
	}
	return true;
}
