// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Open-editor apply of the Persona assets (Runtime/AssetDocumentOpenEditor.h, AnimAssets/AssetDocumentAnimAssetOpenEditor.cpp).
// Each test opens the asset in its Persona editor, applies a representative change through ApplyFile, and checks the
// asset and what the open editor holds; then a failed apply (rolled back) with the editor open, the editor's own undo /
// redo of a human edit after the apply, and closing the editor:
// UEAssetDoc.AnimMontage.OpenEditorApply: a section, a notify and a doubled length; the Animation
//   editor's document tab is rebuilt, its Montage Sections tab lists the new section, and the preview instance plays the
//   montage with the new section links.
// UEAssetDoc.AnimSequence.OpenEditorApply: a notify and a curve; the document tab is rebuilt and the
//   preview plays the sequence.
// UEAssetDoc.AnimationAssets.OpenEditorApply.BlendSpace / .PoseAsset: samples and axes / a renamed pose and
//   additive settings; the document tab is rebuilt and the preview plays the asset.
// UEAssetDoc.AnimationAssets.OpenEditorApply.AnimComposite: trimmed, re-rated and added segments; the
//   document tab is rebuilt and the preview plays the composite with its new length.
// UEAssetDoc.AnimationAssets.OpenEditorApply.Skeleton: a socket; the Skeleton editor's skeleton tree lists
//   it (and not the socket of a failed apply).
// Widget rows that Slate builds only when it draws a window (timeline notify nodes, list rows) are reported, not
// asserted: an unattended session may never draw them.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"

#include "AnimPreviewInstance.h"
#include "Animation/AnimComposite.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/DebugSkelMeshComponent.h"
#include "Animation/PoseAsset.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Factories/AnimMontageFactory.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/FileManager.h"
#include "IAnimationEditor.h"
#include "IPersonaToolkit.h"
#include "ISkeletonTree.h"
#include "ISkeletonTreeItem.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "PersonaTabs.h"
#include "Runtime/AssetDocumentUndoHistory.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "TickableEditorObject.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Text/STextBlock.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentAnimationOpenEditorTests
{
const TCHAR* const FolderName = TEXT("AssetDocumentAnimationOpenEditor");
const TCHAR* const EngineSkeleton = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton");
const TCHAR* const PreviewMesh = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP");
const TCHAR* const IdleAnim = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle.Tutorial_Idle");
const TCHAR* const WalkAnim = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Walk_Fwd.Tutorial_Walk_Fwd");

void ReleaseUndoHistory()
{
	if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting && !GEditor->IsTransactionActive()
		&& GEditor->Trans->GetQueueLength() > 0)
	{
		GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentAnimationOpenEditorTests", "Cleanup", "AssetDocument animation open-editor test cleanup"));
	}
}

UAssetEditorSubsystem* Editors()
{
	return GEditor != nullptr ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
}

void TickEditorUi()
{
	for (int32 Tick = 0; Tick < 3; ++Tick)
	{
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().Tick();
		}
		FTickableEditorObject::TickObjects(0.f);
	}
}

bool SaveAsset(UObject& Asset)
{
	UPackage* Package = Asset.GetOutermost();
	const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	return UPackage::SavePackage(Package, &Asset, *File, Args);
}

struct FTarget
{
	FString AssetName;
	FString PackageName;
	FString ObjectPath;
	FString PackageFile;
	FString SidecarPath;

	explicit FTarget(const TCHAR* Prefix)
		: AssetName(FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits)))
	{
		PackageName = FString(TEXT("/Game/")) + FolderName + TEXT("/") + AssetName;
		ObjectPath = PackageName + TEXT(".") + AssetName;
		PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		SidecarPath = FPaths::Combine(FPaths::ProjectContentDir(), FolderName, AssetName + TEXT(".assetdoc.json"));
	}
	FTarget(const FTarget&) = delete;
	FTarget& operator=(const FTarget&) = delete;

	UObject* Get() const { return FSoftObjectPath(ObjectPath).ResolveObject(); }

	template <typename T>
	T* Get() const { return Cast<T>(Get()); }

	// A saved copy of an engine asset (the engine asset itself is never edited).
	template <typename T>
	T* CopyOf(const TCHAR* SourcePath) const
	{
		T* Source = LoadObject<T>(nullptr, SourcePath);
		if (Source == nullptr)
		{
			return nullptr;
		}
		T* Copy = DuplicateObject<T>(Source, CreatePackage(*PackageName), *AssetName);
		Copy->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Copy);
		return SaveAsset(*Copy) ? Copy : nullptr;
	}

	// Editors closed; saved assets unloaded and unsaved ones deleted, as the Content Browser does.
	~FTarget()
	{
		ReleaseUndoHistory();
		if (UObject* Asset = Get())
		{
			if (UAssetEditorSubsystem* Subsystem = Editors())
			{
				Subsystem->CloseAllEditorsForAsset(Asset);
			}
			Asset->GetOutermost()->ClearDirtyFlag();
			if (IFileManager::Get().FileExists(*PackageFile))
			{
				UPackageTools::UnloadPackages({Asset->GetOutermost()});
			}
			else
			{
				ObjectTools::DeleteObjects({Asset}, false, ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
			}
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		if (IFileManager::Get().FileExists(*PackageFile))
		{
			IFileManager::Get().Delete(*PackageFile, false, true);
			FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({PackageFile});
		}
		IFileManager::Get().Delete(*SidecarPath, false, true);
	}
};

FString Serialize(const TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	if (Object.IsValid())
	{
		const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}
	return Text;
}

TSharedPtr<FJsonObject> Parse(const FString& Json)
{
	TSharedPtr<FJsonObject> Object;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object);
	return Object;
}

void LogResult(FAutomationTestBase& Test, const TCHAR* Stage, const FAssetDocumentResult& Result)
{
	Test.AddInfo(FString::Printf(TEXT("%s: %s"), Stage, *Result.Message));
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Test.AddInfo(FString::Printf(TEXT("%s: [%s] %s %s"), Stage, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
	}
}

FAssetDocumentResult Apply(FAutomationTestBase& Test, const FTarget& Target, const FString& Json, const TCHAR* Stage)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target.SidecarPath), true);
	FFileHelper::SaveStringToFile(Json, *Target.SidecarPath);
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Target.SidecarPath;
	Request.bSaveAsset = true;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
	LogResult(Test, Stage, Result);
	return Result;
}

TSharedPtr<FJsonObject> Extract(FAutomationTestBase& Test, const FTarget& Target, const TCHAR* Stage)
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = Target.ObjectPath;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Extract(Request);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: extract succeeds"), Stage), Result.IsSuccess() && Result.Payload.IsValid()))
	{
		LogResult(Test, Stage, Result);
		return nullptr;
	}
	return Result.Payload;
}

FString ExtractText(FAutomationTestBase& Test, const FTarget& Target, const TCHAR* Stage)
{
	return Serialize(Extract(Test, Target, Stage));
}

FString Document(const FTarget& Target, const TCHAR* Class, const TCHAR* Action, const FString& Body, const FString& Properties = TEXT("{}"))
{
	return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"%s\",\"Action\":\"%s\",\"Properties\":%s,\"Body\":{%s}}"),
		*Target.PackageName, Class, Action, *Properties, *Body);
}

// ---- what the open Persona editor holds ------------------------------------------------------------------------------

IAssetEditorInstance* EditorOf(UObject& Asset)
{
	UAssetEditorSubsystem* Subsystem = Editors();
	return Subsystem != nullptr ? Subsystem->FindEditorForAsset(&Asset, false) : nullptr;
}

// The Animation editor (FAnimationEditor) open on Asset, or null.
IAnimationEditor* AnimationEditorOf(UObject& Asset)
{
	IAssetEditorInstance* Instance = EditorOf(Asset);
	if (Instance == nullptr || Instance->GetEditorName() != FName(TEXT("AnimationEditor")))
	{
		return nullptr;
	}
	return static_cast<IAnimationEditor*>(static_cast<FAssetEditorToolkit*>(Instance));
}

TSharedPtr<SDockTab> LiveTab(IAssetEditorInstance& Instance, const TCHAR* TabId)
{
	const TSharedPtr<FTabManager> TabManager = Instance.GetAssociatedTabManager();
	return TabManager.IsValid() ? TabManager->FindExistingLiveTab(FTabId(TabId)) : nullptr;
}

// The widget the Animation editor's document tab shows (the timeline / blend space / pose editor of the asset).
const SWidget* DocumentContent(UObject& Asset)
{
	IAssetEditorInstance* Instance = AnimationEditorOf(Asset) != nullptr ? EditorOf(Asset) : nullptr;
	const TSharedPtr<SDockTab> Tab = Instance != nullptr ? LiveTab(*Instance, TEXT("Document")) : nullptr;
	return Tab.IsValid() ? &Tab->GetContent().Get() : nullptr;
}

UAnimPreviewInstance* PreviewOf(UObject& Asset)
{
	IAnimationEditor* Editor = AnimationEditorOf(Asset);
	UDebugSkelMeshComponent* Mesh = Editor != nullptr ? Editor->GetPersonaToolkit()->GetPreviewMeshComponent() : nullptr;
	return Mesh != nullptr ? Mesh->PreviewInstance.Get() : nullptr;
}

// Widgets below Root (depth-limited): the count of widgets of Type and the texts of the text blocks.
void WalkWidgets(const SWidget& Root, const FName Type, int32 Depth, int32& OutCount, TArray<FString>& OutTexts)
{
	if (Root.GetType() == Type)
	{
		++OutCount;
	}
	if (Root.GetType() == FName(TEXT("STextBlock")))
	{
		OutTexts.Add(static_cast<const STextBlock&>(Root).GetText().ToString());
	}
	FChildren* Children = Depth > 0 ? const_cast<SWidget&>(Root).GetChildren() : nullptr;
	for (int32 Index = 0; Children != nullptr && Index < Children->Num(); ++Index)
	{
		WalkWidgets(Children->GetChildAt(Index).Get(), Type, Depth - 1, OutCount, OutTexts);
	}
}

TArray<FString> TabTexts(UObject& Asset, const TCHAR* TabId)
{
	TArray<FString> Texts;
	int32 Ignored = 0;
	IAssetEditorInstance* Instance = EditorOf(Asset);
	if (const TSharedPtr<SDockTab> Tab = Instance != nullptr ? LiveTab(*Instance, TabId) : nullptr)
	{
		WalkWidgets(Tab->GetContent().Get(), NAME_None, 64, Ignored, Texts);
	}
	return Texts;
}

int32 DocumentWidgetCount(UObject& Asset, const TCHAR* Type)
{
	int32 Count = 0;
	TArray<FString> Ignored;
	if (const SWidget* Content = DocumentContent(Asset))
	{
		WalkWidgets(*Content, FName(Type), 64, Count, Ignored);
	}
	return Count;
}

// The item names the skeleton tree of the editor open on Asset lists.
TSharedPtr<ISkeletonTree> SkeletonTreeIn(const SWidget& Widget, int32 Depth)
{
	if (Widget.GetType() == FName(TEXT("SSkeletonTree")))
	{
		return StaticCastSharedRef<ISkeletonTree>(const_cast<SWidget&>(Widget).AsShared());
	}
	FChildren* Children = Depth > 0 ? const_cast<SWidget&>(Widget).GetChildren() : nullptr;
	for (int32 Index = 0; Children != nullptr && Index < Children->Num(); ++Index)
	{
		if (TSharedPtr<ISkeletonTree> Found = SkeletonTreeIn(Children->GetChildAt(Index).Get(), Depth - 1))
		{
			return Found;
		}
	}
	return nullptr;
}

TOptional<TSet<FName>> SkeletonTreeItems(UObject& Asset)
{
	IAssetEditorInstance* Instance = EditorOf(Asset);
	const TSharedPtr<SDockTab> Tab = Instance != nullptr ? LiveTab(*Instance, *FPersonaTabs::SkeletonTreeViewID.ToString()) : nullptr;
	const TSharedPtr<ISkeletonTree> Tree = Tab.IsValid() ? SkeletonTreeIn(Tab->GetContent().Get(), 6) : nullptr;
	if (!Tree.IsValid())
	{
		return {};
	}
	TSet<FName> Names;
	Tree->SelectItemsBy([&Names](const TSharedRef<ISkeletonTreeItem>& Item, bool& bInOutExpand)
	{
		Names.Add(Item->GetRowItemName());
		return false;
	});
	return Names;
}

// Opens Asset in its editor; false (with a warning) when no editor opens in this session.
bool OpenEditor(FAutomationTestBase& Test, UObject& Asset, const TCHAR* EditorName)
{
	UAssetEditorSubsystem* Subsystem = Editors();
	if (Subsystem == nullptr || !Subsystem->OpenEditorForAsset(&Asset))
	{
		Test.AddWarning(FString::Printf(TEXT("The %s did not open in this session; the open-editor apply was not exercised."), EditorName));
		return false;
	}
	TickEditorUi();
	// The human does not save after opening: the animation editors change nothing while opening.
	Test.TestFalse(FString::Printf(TEXT("opening the %s leaves the package clean"), EditorName), Asset.GetOutermost()->IsDirty());
	ReleaseUndoHistory();
	return true;
}

void CloseEditor(FAutomationTestBase& Test, const FTarget& Target)
{
	if (UObject* Asset = Target.Get())
	{
		Test.TestNotNull(TEXT("the asset is still open in its editor"), EditorOf(*Asset));
		Editors()->CloseAllEditorsForAsset(Asset);
		Test.TestNull(TEXT("the editor closed"), EditorOf(*Asset));
	}
}

// The checks every Animation-editor test makes after an apply (or a rolled-back apply) with the editor open.
void TestAnimationEditorShows(FAutomationTestBase& Test, const FTarget& Target, const SWidget* ContentBefore, const TCHAR* Stage)
{
	UObject* Asset = Target.Get();
	if (!Test.TestNotNull(*FString::Printf(TEXT("%s: asset"), Stage), Asset))
	{
		return;
	}
	Test.TestNotNull(*FString::Printf(TEXT("%s: the Animation editor is still open"), Stage), AnimationEditorOf(*Asset));
	const SWidget* Content = DocumentContent(*Asset);
	Test.TestNotNull(*FString::Printf(TEXT("%s: the editor has its document tab"), Stage), Content);
	Test.TestTrue(*FString::Printf(TEXT("%s: the document tab was rebuilt from the asset"), Stage), Content != nullptr && Content != ContentBefore);
	UAnimPreviewInstance* Preview = PreviewOf(*Asset);
	Test.TestTrue(*FString::Printf(TEXT("%s: the preview plays the asset"), Stage), Preview != nullptr && Preview->GetCurrentAsset() == Asset);
}

// Undo / redo of a human edit made after the apply (an editor transaction), with the editor open.
void TestHumanUndoRedo(FAutomationTestBase& Test, const FTarget& Target, const FString& Applied, TFunctionRef<void(UObject&)> Edit)
{
	UObject* Asset = Target.Get();
	if (!Test.TestNotNull(TEXT("asset for the human edit"), Asset))
	{
		return;
	}
	ReleaseUndoHistory();
	{
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentAnimationOpenEditorTests", "HumanEdit", "Edit"));
		Edit(*Asset);
	}
	const FString Human = ExtractText(Test, Target, TEXT("after the human edit"));
	Test.TestNotEqual(TEXT("the human edit landed"), Human, Applied);
	Test.TestTrue(TEXT("the human edit undoes"), GEditor->Trans->CanUndo() && GEditor->UndoTransaction());
	TickEditorUi();
	Test.TestEqual(TEXT("undo restores the applied asset"), ExtractText(Test, Target, TEXT("after undo")), Applied);
	Test.TestTrue(TEXT("the human edit redoes"), GEditor->Trans->CanRedo() && GEditor->RedoTransaction());
	TickEditorUi();
	Test.TestEqual(TEXT("redo restores the human edit"), ExtractText(Test, Target, TEXT("after redo")), Human);
	Test.TestNotNull(TEXT("after undo / redo the editor is open"), EditorOf(*Asset));
}

TSharedPtr<FJsonObject> BodyOf(const TSharedPtr<FJsonObject>& Document)
{
	const TSharedPtr<FJsonObject>* Body = nullptr;
	return Document.IsValid() && Document->TryGetObjectField(TEXT("Body"), Body) ? *Body : MakeShared<FJsonObject>();
}

TArray<TSharedPtr<FJsonValue>> ArrayOf(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	return Object.IsValid() && Object->TryGetArrayField(Field, Values) ? *Values : TArray<TSharedPtr<FJsonValue>>();
}

// The section the preview's montage instance plays after Section (INDEX_NONE: none), or NoMontageInstance.
constexpr int32 NoMontageInstance = -2;
int32 MontagePreviewNextSection(UObject& Montage, int32 Section)
{
	const UAnimPreviewInstance* Preview = PreviewOf(Montage);
	const FAnimMontageInstance* Instance = Preview != nullptr ? Preview->GetActiveMontageInstance() : nullptr;
	return Instance != nullptr ? Instance->GetNextSectionID(Section) : NoMontageInstance;
}
} // namespace AssetDocumentAnimationOpenEditorTests

// Test bodies name the helpers through this alias: other test files of the module pull their own helper namespaces
// into the global scope, which unity builds would make ambiguous.
namespace AnimOpenEd = AssetDocumentAnimationOpenEditorTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimMontageOpenEditorApplyTest, "UEAssetDoc.AnimMontage.OpenEditorApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimMontageOpenEditorApplyTest::RunTest(const FString& Parameters)
{
	if (GEditor == nullptr || GEditor->Trans == nullptr)
	{
		return false;
	}
	const AnimOpenEd::FTarget Target(TEXT("AM_Open"));
	{
		UAnimMontageFactory* Factory = NewObject<UAnimMontageFactory>();
		Factory->SourceAnimation = LoadObject<UAnimSequence>(nullptr, AnimOpenEd::IdleAnim);
		Factory->PreviewSkeletalMesh = LoadObject<USkeletalMesh>(nullptr, AnimOpenEd::PreviewMesh);
		UObject* Created = Factory->SourceAnimation != nullptr ? Factory->FactoryCreateNew(UAnimMontage::StaticClass(), CreatePackage(*Target.PackageName),
			*Target.AssetName, RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn) : nullptr;
		if (!TestNotNull(TEXT("montage from the factory"), Created))
		{
			return false;
		}
		FAssetRegistryModule::AssetCreated(Created);
		TestTrue(TEXT("montage saved"), AnimOpenEd::SaveAsset(*Created));
	}
	UAnimMontage* Montage = Target.Get<UAnimMontage>();
	if (!TestNotNull(TEXT("montage"), Montage) || !AnimOpenEd::OpenEditor(*this, *Montage, TEXT("Animation editor")))
	{
		return true;
	}
	ON_SCOPE_EXIT
	{
		if (UObject* Open = Target.Get())
		{
			AnimOpenEd::Editors()->CloseAllEditorsForAsset(Open);
		}
	};
	const float LengthBefore = Montage->GetPlayLength();
	const int32 NotifiesBefore = Montage->Notifies.Num();
	// The Montage editor previews with every section looping: the one section links to itself.
	TestEqual(TEXT("the preview plays the montage, its one section looping"), AnimOpenEd::MontagePreviewNextSection(*Montage, 0), 0);
	const int32 NotifyNodesBefore = AnimOpenEd::DocumentWidgetCount(*Montage, TEXT("SAnimNotifyNode"));
	const SWidget* ContentBefore = AnimOpenEd::DocumentContent(*Montage);

	// A second section, a notify and a doubled length (the segment loops twice).
	const TSharedPtr<FJsonObject> Edit = AnimOpenEd::Extract(*this, Target, TEXT("montage base"));
	if (!Edit.IsValid())
	{
		return false;
	}
	Edit->SetStringField(TEXT("Action"), TEXT("Update"));
	const TSharedPtr<FJsonObject> Body = AnimOpenEd::BodyOf(Edit);
	const TArray<TSharedPtr<FJsonValue>> Sections = AnimOpenEd::ArrayOf(Body, TEXT("CompositeSections"));
	if (!TestEqual(TEXT("the montage has one section"), Sections.Num(), 1))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> First = AnimOpenEd::Parse(AnimOpenEd::Serialize(Sections[0]->AsObject()));
	First->SetStringField(TEXT("NextSectionName"), TEXT("Second"));
	const TSharedPtr<FJsonObject> Second = AnimOpenEd::Parse(AnimOpenEd::Serialize(Sections[0]->AsObject()));
	Second->SetStringField(TEXT("Name"), TEXT("Second"));
	Second->SetNumberField(TEXT("LinkableTime"), LengthBefore * 0.5);
	Body->SetArrayField(TEXT("CompositeSections"), {MakeShared<FJsonValueObject>(First), MakeShared<FJsonValueObject>(Second)});
	TArray<TSharedPtr<FJsonValue>> Notifies = AnimOpenEd::ArrayOf(Body, TEXT("Notifies"));
	Notifies.Add(MakeShared<FJsonValueObject>(AnimOpenEd::Parse(TEXT(R"({"Time": 0.25, "TrackIndex": 0,
		"Object": {"Class": "/Script/Engine.AnimNotify_PlaySound", "Properties": {"VolumeMultiplier": 0.5}}})"))));
	Body->SetArrayField(TEXT("Notifies"), Notifies);
	const TArray<TSharedPtr<FJsonValue>> Slots = AnimOpenEd::ArrayOf(Body, TEXT("SlotAnimTracks"));
	const TArray<TSharedPtr<FJsonValue>> Segments = Slots.IsEmpty() ? TArray<TSharedPtr<FJsonValue>>() : AnimOpenEd::ArrayOf(Slots[0]->AsObject(), TEXT("Segments"));
	if (!TestEqual(TEXT("the montage has one segment"), Segments.Num(), 1))
	{
		return false;
	}
	Segments[0]->AsObject()->SetNumberField(TEXT("LoopingCount"), 2);
	if (!TestTrue(TEXT("apply while open"), AnimOpenEd::Apply(*this, Target, AnimOpenEd::Serialize(Edit), TEXT("apply while open")).IsSuccess()))
	{
		return false;
	}
	Montage = Target.Get<UAnimMontage>();
	if (!TestNotNull(TEXT("montage after the apply"), Montage))
	{
		return false;
	}
	TestEqual(TEXT("two sections"), Montage->CompositeSections.Num(), 2);
	TestEqual(TEXT("one more notify"), Montage->Notifies.Num(), NotifiesBefore + 1);
	TestTrue(TEXT("the montage plays twice as long"), FMath::IsNearlyEqual(Montage->GetPlayLength(), LengthBefore * 2.f, 1e-3f));
	TestFalse(TEXT("the apply is not an undo step"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	AnimOpenEd::TestAnimationEditorShows(*this, Target, ContentBefore, TEXT("after the apply"));
	TestEqual(TEXT("the preview plays the new section link First -> Second"), AnimOpenEd::MontagePreviewNextSection(*Montage, 0), 1);
	TestTrue(TEXT("the Montage Sections tab lists the new section"), AnimOpenEd::TabTexts(*Montage, TEXT("AnimMontageSections")).Contains(TEXT("Second")));
	AnimOpenEd::TickEditorUi();
	const int32 NotifyNodesAfter = AnimOpenEd::DocumentWidgetCount(*Montage, TEXT("SAnimNotifyNode"));
	AddInfo(FString::Printf(TEXT("timeline notify widgets: %d before, %d after the apply"), NotifyNodesBefore, NotifyNodesAfter));
	if (NotifyNodesBefore > 0)
	{
		TestEqual(TEXT("the timeline shows the new notify"), NotifyNodesAfter, NotifyNodesBefore + 1);
	}
	const FString Applied = AnimOpenEd::ExtractText(*this, Target, TEXT("after apply while open"));

	// A failure in the middle of the apply (an unknown notify property) rolls back with the editor open.
	{
		const TSharedPtr<FJsonObject> Failing = AnimOpenEd::Parse(Applied);
		Failing->SetStringField(TEXT("Action"), TEXT("Update"));
		const TSharedPtr<FJsonObject> FailingBody = AnimOpenEd::BodyOf(Failing);
		FailingBody->SetArrayField(TEXT("CompositeSections"), {MakeShared<FJsonValueObject>(AnimOpenEd::Parse(AnimOpenEd::Serialize(Sections[0]->AsObject())))});
		TArray<TSharedPtr<FJsonValue>> Bad = AnimOpenEd::ArrayOf(FailingBody, TEXT("Notifies"));
		Bad.Add(MakeShared<FJsonValueObject>(AnimOpenEd::Parse(TEXT(R"({"Time": 0.5, "TrackIndex": 0,
			"Object": {"Class": "/Script/Engine.AnimNotify_PlaySound", "Properties": {"DoesNotExist": 1}}})"))));
		FailingBody->SetArrayField(TEXT("Notifies"), Bad);
		const SWidget* ContentApplied = AnimOpenEd::DocumentContent(*Montage);
		const FAssetDocumentResult Failed = AnimOpenEd::Apply(*this, Target, AnimOpenEd::Serialize(Failing), TEXT("failing apply while open"));
		TestFalse(TEXT("failing apply while open fails"), Failed.IsSuccess());
		Montage = Target.Get<UAnimMontage>();
		if (!TestNotNull(TEXT("montage after the rollback"), Montage))
		{
			return false;
		}
		TestEqual(TEXT("after the rollback the montage is as applied"), AnimOpenEd::ExtractText(*this, Target, TEXT("after the rollback")), Applied);
		TestFalse(TEXT("after the rollback the package is clean"), Montage->GetOutermost()->IsDirty());
		AnimOpenEd::TestAnimationEditorShows(*this, Target, ContentApplied, TEXT("after the rollback"));
		TestEqual(TEXT("after the rollback the preview plays the applied section link"), AnimOpenEd::MontagePreviewNextSection(*Montage, 0), 1);
		TestTrue(TEXT("after the rollback the Montage Sections tab lists the applied section"), AnimOpenEd::TabTexts(*Montage, TEXT("AnimMontageSections")).Contains(TEXT("Second")));
		AnimOpenEd::TickEditorUi();
	}

	AnimOpenEd::TestHumanUndoRedo(*this, Target, Applied, [](UObject& Asset)
	{
		UAnimMontage& Edited = *CastChecked<UAnimMontage>(&Asset);
		Edited.Modify();
		Edited.RateScale = 2.f;
	});
	AnimOpenEd::CloseEditor(*this, Target);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimSequenceOpenEditorApplyTest, "UEAssetDoc.AnimSequence.OpenEditorApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimSequenceOpenEditorApplyTest::RunTest(const FString& Parameters)
{
	if (GEditor == nullptr || GEditor->Trans == nullptr)
	{
		return false;
	}
	const AnimOpenEd::FTarget Target(TEXT("AS_Open"));
	UAnimSequence* Sequence = Target.CopyOf<UAnimSequence>(AnimOpenEd::IdleAnim);
	if (!TestNotNull(TEXT("sequence copy"), Sequence) || !AnimOpenEd::OpenEditor(*this, *Sequence, TEXT("Animation editor")))
	{
		return true;
	}
	ON_SCOPE_EXIT
	{
		if (UObject* Open = Target.Get())
		{
			AnimOpenEd::Editors()->CloseAllEditorsForAsset(Open);
		}
	};
	const int32 NotifiesBefore = Sequence->Notifies.Num();
	const int32 NotifyNodesBefore = AnimOpenEd::DocumentWidgetCount(*Sequence, TEXT("SAnimNotifyNode"));
	const SWidget* ContentBefore = AnimOpenEd::DocumentContent(*Sequence);
	const TCHAR* Class = TEXT("/Script/Engine.AnimSequence");
	const FString Edit = AnimOpenEd::Document(Target, Class, TEXT("Update"), TEXT(R"("Notifies": [{"Time": 0.25, "Name": "P77Step"}],
		"Curves": [{"Name": "P77Curve", "Keys": [{"Time": 0, "Value": 0, "InterpMode": "Linear"}, {"Time": 0.5, "Value": 1, "InterpMode": "Linear"}]}])"));
	if (!TestTrue(TEXT("apply while open"), AnimOpenEd::Apply(*this, Target, Edit, TEXT("apply while open")).IsSuccess()))
	{
		return false;
	}
	Sequence = Target.Get<UAnimSequence>();
	if (!TestNotNull(TEXT("sequence after the apply"), Sequence))
	{
		return false;
	}
	TestEqual(TEXT("one notify"), Sequence->Notifies.Num(), 1);
	TestTrue(TEXT("the notify"), Sequence->Notifies.Num() == 1 && Sequence->Notifies[0].NotifyName == FName(TEXT("P77Step")));
	TestTrue(TEXT("the curve"), Sequence->GetDataModel()->FindFloatCurve(FAnimationCurveIdentifier(TEXT("P77Curve"), ERawCurveTrackTypes::RCT_Float)) != nullptr);
	TestFalse(TEXT("the apply is not an undo step"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	AnimOpenEd::TestAnimationEditorShows(*this, Target, ContentBefore, TEXT("after the apply"));
	AnimOpenEd::TickEditorUi();
	const int32 NotifyNodesAfter = AnimOpenEd::DocumentWidgetCount(*Sequence, TEXT("SAnimNotifyNode"));
	AddInfo(FString::Printf(TEXT("timeline notify widgets: %d before (%d notifies), %d after the apply"), NotifyNodesBefore, NotifiesBefore, NotifyNodesAfter));
	if (NotifyNodesBefore > 0 || NotifyNodesAfter > 0)
	{
		TestEqual(TEXT("the timeline shows the applied notifies"), NotifyNodesAfter, 1);
	}
	const FString Applied = AnimOpenEd::ExtractText(*this, Target, TEXT("after apply while open"));

	// A failure in the middle of the apply (an unknown notify property) rolls back with the editor open.
	{
		const SWidget* ContentApplied = AnimOpenEd::DocumentContent(*Sequence);
		const FAssetDocumentResult Failed = AnimOpenEd::Apply(*this, Target, AnimOpenEd::Document(Target, Class, TEXT("Update"), TEXT(R"("Notifies": [
			{"Time": 0.25, "Name": "P77Step"}, {"Time": 0.5, "Class": "/Script/Engine.AnimNotify_PlaySound", "Properties": {"DoesNotExist": 1}}])")),
			TEXT("failing apply while open"));
		TestFalse(TEXT("failing apply while open fails"), Failed.IsSuccess());
		Sequence = Target.Get<UAnimSequence>();
		if (!TestNotNull(TEXT("sequence after the rollback"), Sequence))
		{
			return false;
		}
		TestEqual(TEXT("after the rollback the sequence is as applied"), AnimOpenEd::ExtractText(*this, Target, TEXT("after the rollback")), Applied);
		TestFalse(TEXT("after the rollback the package is clean"), Sequence->GetOutermost()->IsDirty());
		AnimOpenEd::TestAnimationEditorShows(*this, Target, ContentApplied, TEXT("after the rollback"));
		AnimOpenEd::TickEditorUi();
	}

	AnimOpenEd::TestHumanUndoRedo(*this, Target, Applied, [](UObject& Asset)
	{
		UAnimSequence& Edited = *CastChecked<UAnimSequence>(&Asset);
		Edited.Modify();
		Edited.RateScale = 2.f;
	});
	AnimOpenEd::CloseEditor(*this, Target);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimationAssetsOpenEditorApplyBlendSpaceTest, "UEAssetDoc.AnimationAssets.OpenEditorApply.BlendSpace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationAssetsOpenEditorApplyBlendSpaceTest::RunTest(const FString& Parameters)
{
	if (GEditor == nullptr || GEditor->Trans == nullptr)
	{
		return false;
	}
	const TCHAR* Class = TEXT("/Script/Engine.BlendSpace");
	const AnimOpenEd::FTarget Target(TEXT("BS_Open"));
	const FString Created = AnimOpenEd::Document(Target, Class, TEXT("Create"), FString::Printf(TEXT("\"Skeleton\":\"%s\",\"PreviewMesh\":\"%s\",")
		TEXT("\"Axes\":{\"X\":{\"Name\":\"Direction\",\"Min\":-180,\"Max\":180,\"GridDivisions\":4},\"Y\":{\"Name\":\"Speed\",\"Min\":0,\"Max\":600,\"GridDivisions\":4}},")
		TEXT("\"Samples\":[{\"Animation\":\"%s\",\"X\":0,\"Y\":0},{\"Animation\":\"%s\",\"X\":0,\"Y\":300}]"), AnimOpenEd::EngineSkeleton, AnimOpenEd::PreviewMesh, AnimOpenEd::IdleAnim, AnimOpenEd::WalkAnim));
	if (!TestTrue(TEXT("create applies"), AnimOpenEd::Apply(*this, Target, Created, TEXT("create")).IsSuccess()))
	{
		return false;
	}
	UBlendSpace* BlendSpace = Target.Get<UBlendSpace>();
	if (!TestNotNull(TEXT("blend space"), BlendSpace) || !AnimOpenEd::OpenEditor(*this, *BlendSpace, TEXT("Animation editor")))
	{
		return true;
	}
	ON_SCOPE_EXIT
	{
		if (UObject* Open = Target.Get())
		{
			AnimOpenEd::Editors()->CloseAllEditorsForAsset(Open);
		}
	};
	const SWidget* ContentBefore = AnimOpenEd::DocumentContent(*BlendSpace);
	// The walk sample moves, a third sample is added, the Y axis is renamed.
	const FString Edit = AnimOpenEd::Document(Target, Class, TEXT("Update"), FString::Printf(TEXT("\"Axes\":{\"Y\":{\"Name\":\"Velocity\"}},\"Samples\":[")
		TEXT("{\"Animation\":\"%s\",\"X\":0,\"Y\":0},{\"Animation\":\"%s\",\"X\":90,\"Y\":300},{\"Animation\":\"%s\",\"X\":-90,\"Y\":300}]"), AnimOpenEd::IdleAnim, AnimOpenEd::WalkAnim, AnimOpenEd::WalkAnim));
	if (!TestTrue(TEXT("apply while open"), AnimOpenEd::Apply(*this, Target, Edit, TEXT("apply while open")).IsSuccess()))
	{
		return false;
	}
	BlendSpace = Target.Get<UBlendSpace>();
	if (!TestNotNull(TEXT("blend space after the apply"), BlendSpace))
	{
		return false;
	}
	TestEqual(TEXT("three samples"), BlendSpace->GetNumberOfBlendSamples(), 3);
	TestEqual(TEXT("the renamed axis"), BlendSpace->GetBlendParameter(1).DisplayName, FString(TEXT("Velocity")));
	TestFalse(TEXT("the apply is not an undo step"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	AnimOpenEd::TestAnimationEditorShows(*this, Target, ContentBefore, TEXT("after the apply"));
	AnimOpenEd::TickEditorUi();
	const FString Applied = AnimOpenEd::ExtractText(*this, Target, TEXT("after apply while open"));

	// A failure in the middle of the apply (the axis edit runs first; the new sample's animation is a Skeleton).
	{
		const SWidget* ContentApplied = AnimOpenEd::DocumentContent(*BlendSpace);
		const FAssetDocumentResult Failed = AnimOpenEd::Apply(*this, Target, AnimOpenEd::Document(Target, Class, TEXT("Update"), FString::Printf(
			TEXT("\"Axes\":{\"Y\":{\"Max\":550}},\"Samples\":[{\"Animation\":\"%s\",\"X\":0,\"Y\":0},{\"Animation\":\"%s\",\"X\":90,\"Y\":250}]"),
			AnimOpenEd::IdleAnim, AnimOpenEd::EngineSkeleton)), TEXT("failing apply while open"));
		TestFalse(TEXT("failing apply while open fails"), Failed.IsSuccess());
		BlendSpace = Target.Get<UBlendSpace>();
		if (!TestNotNull(TEXT("blend space after the rollback"), BlendSpace))
		{
			return false;
		}
		TestEqual(TEXT("after the rollback the blend space is as applied"), AnimOpenEd::ExtractText(*this, Target, TEXT("after the rollback")), Applied);
		TestFalse(TEXT("after the rollback the package is clean"), BlendSpace->GetOutermost()->IsDirty());
		AnimOpenEd::TestAnimationEditorShows(*this, Target, ContentApplied, TEXT("after the rollback"));
		AnimOpenEd::TickEditorUi();
	}

	AnimOpenEd::TestHumanUndoRedo(*this, Target, Applied, [](UObject& Asset)
	{
		UBlendSpace& Edited = *CastChecked<UBlendSpace>(&Asset);
		Edited.Modify();
		Edited.TargetWeightInterpolationSpeedPerSec = 9.f;
	});
	AnimOpenEd::CloseEditor(*this, Target);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimationAssetsOpenEditorApplyAnimCompositeTest, "UEAssetDoc.AnimationAssets.OpenEditorApply.AnimComposite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationAssetsOpenEditorApplyAnimCompositeTest::RunTest(const FString& Parameters)
{
	if (GEditor == nullptr || GEditor->Trans == nullptr)
	{
		return false;
	}
	const TCHAR* Class = TEXT("/Script/Engine.AnimComposite");
	const AnimOpenEd::FTarget Target(TEXT("AC_Open"));
	const FString Created = AnimOpenEd::Document(Target, Class, TEXT("Create"), FString::Printf(TEXT("\"Skeleton\":\"%s\",\"PreviewMesh\":\"%s\",")
		TEXT("\"Segments\":[{\"Animation\":\"%s\"},{\"Animation\":\"%s\"}]"), AnimOpenEd::EngineSkeleton, AnimOpenEd::PreviewMesh, AnimOpenEd::IdleAnim, AnimOpenEd::WalkAnim));
	if (!TestTrue(TEXT("create applies"), AnimOpenEd::Apply(*this, Target, Created, TEXT("create")).IsSuccess()))
	{
		return false;
	}
	UAnimComposite* Composite = Target.Get<UAnimComposite>();
	if (!TestNotNull(TEXT("composite"), Composite) || !AnimOpenEd::OpenEditor(*this, *Composite, TEXT("Animation editor")))
	{
		return true;
	}
	ON_SCOPE_EXIT
	{
		if (UObject* Open = Target.Get())
		{
			AnimOpenEd::Editors()->CloseAllEditorsForAsset(Open);
		}
	};
	const float LengthBefore = Composite->GetPlayLength();
	const SWidget* ContentBefore = AnimOpenEd::DocumentContent(*Composite);
	// The first segment is trimmed, the walk plays twice as fast, and a third segment (walk, twice) is added.
	const FString Edit = AnimOpenEd::Document(Target, Class, TEXT("Update"), FString::Printf(TEXT("\"Segments\":[")
		TEXT("{\"Animation\":\"%s\",\"StartTime\":0.25,\"EndTime\":0.75},{\"Animation\":\"%s\",\"PlayRate\":2.0},{\"Animation\":\"%s\",\"LoopCount\":2}]"),
		AnimOpenEd::IdleAnim, AnimOpenEd::WalkAnim, AnimOpenEd::WalkAnim));
	if (!TestTrue(TEXT("apply while open"), AnimOpenEd::Apply(*this, Target, Edit, TEXT("apply while open")).IsSuccess()))
	{
		return false;
	}
	Composite = Target.Get<UAnimComposite>();
	if (!TestNotNull(TEXT("composite after the apply"), Composite))
	{
		return false;
	}
	const TArray<FAnimSegment>& Segments = Composite->AnimationTrack.AnimSegments;
	if (TestEqual(TEXT("three segments"), Segments.Num(), 3))
	{
		TestEqual(TEXT("the trimmed segment ends at 0.75"), Segments[0].AnimEndTime, 0.75f);
		TestEqual(TEXT("the walk plays twice as fast"), Segments[1].AnimPlayRate, 2.f);
		TestEqual(TEXT("the new segment loops twice"), Segments[2].LoopingCount, 2);
		TestEqual(TEXT("the composite length follows the segments"), Composite->GetPlayLength(), Segments[2].StartPos + Segments[2].GetLength(), 1e-3f);
	}
	TestNotEqual(TEXT("the length changed"), Composite->GetPlayLength(), LengthBefore);
	TestFalse(TEXT("the apply is not an undo step"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	AnimOpenEd::TestAnimationEditorShows(*this, Target, ContentBefore, TEXT("after the apply"));
	if (UAnimPreviewInstance* Preview = AnimOpenEd::PreviewOf(*Composite))
	{
		TestEqual(TEXT("the preview plays the applied length"), Preview->GetLength(), Composite->GetPlayLength(), 1e-3f);
	}
	AnimOpenEd::TickEditorUi();
	const FString Applied = AnimOpenEd::ExtractText(*this, Target, TEXT("after apply while open"));

	// A failure in the middle of the apply (the segment track is replaced first; the preview mesh is not a Skeletal Mesh).
	{
		const SWidget* ContentApplied = AnimOpenEd::DocumentContent(*Composite);
		const FAssetDocumentResult Failed = AnimOpenEd::Apply(*this, Target, AnimOpenEd::Document(Target, Class, TEXT("Update"), FString::Printf(
			TEXT("\"Segments\":[{\"Animation\":\"%s\"}],\"PreviewMesh\":\"%s\""), AnimOpenEd::IdleAnim, AnimOpenEd::EngineSkeleton)), TEXT("failing apply while open"));
		TestFalse(TEXT("failing apply while open fails"), Failed.IsSuccess());
		Composite = Target.Get<UAnimComposite>();
		if (!TestNotNull(TEXT("composite after the rollback"), Composite))
		{
			return false;
		}
		TestEqual(TEXT("after the rollback the composite is as applied"), AnimOpenEd::ExtractText(*this, Target, TEXT("after the rollback")), Applied);
		TestEqual(TEXT("after the rollback three segments"), Composite->AnimationTrack.AnimSegments.Num(), 3);
		TestFalse(TEXT("after the rollback the package is clean"), Composite->GetOutermost()->IsDirty());
		AnimOpenEd::TestAnimationEditorShows(*this, Target, ContentApplied, TEXT("after the rollback"));
		if (UAnimPreviewInstance* Preview = AnimOpenEd::PreviewOf(*Composite))
		{
			TestEqual(TEXT("after the rollback the preview plays the applied length"), Preview->GetLength(), Composite->GetPlayLength(), 1e-3f);
		}
		AnimOpenEd::TickEditorUi();
	}

	AnimOpenEd::TestHumanUndoRedo(*this, Target, Applied, [](UObject& Asset)
	{
		UAnimComposite& Edited = *CastChecked<UAnimComposite>(&Asset);
		Edited.Modify();
		Edited.AnimationTrack.AnimSegments[1].AnimPlayRate = 3.f;
	});
	AnimOpenEd::CloseEditor(*this, Target);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimationAssetsOpenEditorApplyPoseAssetTest, "UEAssetDoc.AnimationAssets.OpenEditorApply.PoseAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationAssetsOpenEditorApplyPoseAssetTest::RunTest(const FString& Parameters)
{
	if (GEditor == nullptr || GEditor->Trans == nullptr)
	{
		return false;
	}
	const TCHAR* Class = TEXT("/Script/Engine.PoseAsset");
	const AnimOpenEd::FTarget Target(TEXT("PA_Open"));
	if (!TestTrue(TEXT("create applies"), AnimOpenEd::Apply(*this, Target, AnimOpenEd::Document(Target, Class, TEXT("Create"),
		FString::Printf(TEXT("\"SourceAnimation\":\"%s\",\"PreviewMesh\":\"%s\""), AnimOpenEd::WalkAnim, AnimOpenEd::PreviewMesh)), TEXT("create")).IsSuccess()))
	{
		return false;
	}
	UPoseAsset* PoseAsset = Target.Get<UPoseAsset>();
	if (!TestNotNull(TEXT("pose asset"), PoseAsset) || !TestTrue(TEXT("at least two poses"), PoseAsset->GetNumPoses() >= 2)
		|| !AnimOpenEd::OpenEditor(*this, *PoseAsset, TEXT("Animation editor")))
	{
		return true;
	}
	ON_SCOPE_EXIT
	{
		if (UObject* Open = Target.Get())
		{
			AnimOpenEd::Editors()->CloseAllEditorsForAsset(Open);
		}
	};
	// Pose_0 renamed to Contact (the other poses kept as they are).
	const int32 Poses = PoseAsset->GetNumPoses();
	auto PoseRows = [&PoseAsset, Poses](const TCHAR* From, const TCHAR* To)
	{
		TArray<FString> Rows;
		for (int32 Index = 0; Index < Poses; ++Index)
		{
			Rows.Add(Index == 0 ? FString::Printf(TEXT("{\"Name\":\"%s\",\"RenamedFrom\":\"%s\"}"), To, From)
				: FString::Printf(TEXT("\"%s\""), *PoseAsset->GetPoseNameByIndex(Index).ToString()));
		}
		return FString::Printf(TEXT("\"Poses\":[%s]"), *FString::Join(Rows, TEXT(",")));
	};
	const SWidget* ContentBefore = AnimOpenEd::DocumentContent(*PoseAsset);
	const FString Edit = AnimOpenEd::Document(Target, Class, TEXT("Update"), PoseRows(TEXT("Pose_0"), TEXT("Contact")));
	if (!TestTrue(TEXT("apply while open"), AnimOpenEd::Apply(*this, Target, Edit, TEXT("apply while open")).IsSuccess()))
	{
		return false;
	}
	PoseAsset = Target.Get<UPoseAsset>();
	if (!TestNotNull(TEXT("pose asset after the apply"), PoseAsset))
	{
		return false;
	}
	TestEqual(TEXT("the renamed pose"), PoseAsset->GetPoseNameByIndex(0), FName(TEXT("Contact")));
	TestFalse(TEXT("the apply is not an undo step"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	AnimOpenEd::TestAnimationEditorShows(*this, Target, ContentBefore, TEXT("after the apply"));
	AnimOpenEd::TickEditorUi();
	{
		TArray<FString> Texts;
		int32 Ignored = 0;
		if (const SWidget* Content = AnimOpenEd::DocumentContent(*PoseAsset))
		{
			AnimOpenEd::WalkWidgets(*Content, NAME_None, 64, Ignored, Texts);
		}
		AddInfo(FString::Printf(TEXT("the pose list %s"), Texts.Contains(TEXT("Contact")) ? TEXT("shows the renamed pose")
			: Texts.Contains(TEXT("Pose_0")) ? TEXT("SHOWS THE OLD POSE NAME") : TEXT("rows are not built in this session")));
		TestFalse(TEXT("the pose list does not show the old pose name"), Texts.Contains(TEXT("Pose_0")));
	}
	const FString Applied = AnimOpenEd::ExtractText(*this, Target, TEXT("after apply while open"));

	// A failure in the middle of the apply (the rename runs first; the preview mesh is not a Skeletal Mesh).
	{
		const SWidget* ContentApplied = AnimOpenEd::DocumentContent(*PoseAsset);
		const FAssetDocumentResult Failed = AnimOpenEd::Apply(*this, Target, AnimOpenEd::Document(Target, Class, TEXT("Update"),
			PoseRows(TEXT("Contact"), TEXT("Plant")) + FString::Printf(TEXT(",\"PreviewMesh\":\"%s\""), AnimOpenEd::EngineSkeleton)), TEXT("failing apply while open"));
		TestFalse(TEXT("failing apply while open fails"), Failed.IsSuccess());
		PoseAsset = Target.Get<UPoseAsset>();
		if (!TestNotNull(TEXT("pose asset after the rollback"), PoseAsset))
		{
			return false;
		}
		TestEqual(TEXT("after the rollback the pose asset is as applied"), AnimOpenEd::ExtractText(*this, Target, TEXT("after the rollback")), Applied);
		TestFalse(TEXT("after the rollback the package is clean"), PoseAsset->GetOutermost()->IsDirty());
		AnimOpenEd::TestAnimationEditorShows(*this, Target, ContentApplied, TEXT("after the rollback"));
		AnimOpenEd::TickEditorUi();
	}

	AnimOpenEd::TestHumanUndoRedo(*this, Target, Applied, [](UObject& Asset)
	{
		UPoseAsset& Edited = *CastChecked<UPoseAsset>(&Asset);
		Edited.Modify();
		Edited.ModifyPoseName(Edited.GetPoseNameByIndex(1), TEXT("HumanPose"));
	});
	AnimOpenEd::CloseEditor(*this, Target);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimationAssetsOpenEditorApplySkeletonTest, "UEAssetDoc.AnimationAssets.OpenEditorApply.Skeleton",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationAssetsOpenEditorApplySkeletonTest::RunTest(const FString& Parameters)
{
	if (GEditor == nullptr || GEditor->Trans == nullptr)
	{
		return false;
	}
	const TCHAR* Class = TEXT("/Script/Engine.Skeleton");
	const AnimOpenEd::FTarget Target(TEXT("SK_Open"));
	USkeleton* Skeleton = Target.CopyOf<USkeleton>(AnimOpenEd::EngineSkeleton);
	if (!TestNotNull(TEXT("skeleton copy"), Skeleton) || !AnimOpenEd::OpenEditor(*this, *Skeleton, TEXT("Skeleton editor")))
	{
		return true;
	}
	ON_SCOPE_EXIT
	{
		if (UObject* Open = Target.Get())
		{
			AnimOpenEd::Editors()->CloseAllEditorsForAsset(Open);
		}
	};
	const TOptional<TSet<FName>> ItemsBefore = AnimOpenEd::SkeletonTreeItems(*Skeleton);
	if (!TestTrue(TEXT("the Skeleton editor has its skeleton tree"), ItemsBefore.IsSet() && ItemsBefore->Contains(FName(TEXT("hand_r")))))
	{
		return false;
	}
	TestFalse(TEXT("the tree has no p77 socket yet"), ItemsBefore->Contains(FName(TEXT("p77_socket"))));
	if (!TestTrue(TEXT("apply while open"), AnimOpenEd::Apply(*this, Target, AnimOpenEd::Document(Target, Class, TEXT("Update"),
		TEXT("\"Sockets\":[{\"Name\":\"p77_socket\",\"Bone\":\"hand_r\",\"Location\":{\"X\":0,\"Y\":5,\"Z\":0}}]")), TEXT("apply while open")).IsSuccess()))
	{
		return false;
	}
	Skeleton = Target.Get<USkeleton>();
	if (!TestNotNull(TEXT("skeleton after the apply"), Skeleton))
	{
		return false;
	}
	TestNotNull(TEXT("the socket"), Skeleton->FindSocket(TEXT("p77_socket")));
	TestFalse(TEXT("the apply is not an undo step"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	TestNotNull(TEXT("the Skeleton editor is still open"), AnimOpenEd::EditorOf(*Skeleton));
	const TOptional<TSet<FName>> ItemsAfter = AnimOpenEd::SkeletonTreeItems(*Skeleton);
	TestTrue(TEXT("the skeleton tree lists the new socket"), ItemsAfter.IsSet() && ItemsAfter->Contains(FName(TEXT("p77_socket"))));
	AnimOpenEd::TickEditorUi();
	const FString Applied = AnimOpenEd::ExtractText(*this, Target, TEXT("after apply while open"));

	// A failure in the middle of the apply (slot ops run first; the new socket's bone does not exist).
	{
		const FAssetDocumentResult Failed = AnimOpenEd::Apply(*this, Target, AnimOpenEd::Document(Target, Class, TEXT("Update"), TEXT(
			"\"SlotGroups\":[{\"Name\":\"DefaultGroup\",\"Slots\":[\"DefaultSlot\",\"P77Extra\"]}],"
			"\"Sockets\":[{\"Name\":\"p77_socket\",\"Bone\":\"hand_r\"},{\"Name\":\"p77_bad\",\"Bone\":\"NoSuchBone\"}]")), TEXT("failing apply while open"));
		TestFalse(TEXT("failing apply while open fails"), Failed.IsSuccess());
		Skeleton = Target.Get<USkeleton>();
		if (!TestNotNull(TEXT("skeleton after the rollback"), Skeleton))
		{
			return false;
		}
		TestEqual(TEXT("after the rollback the skeleton is as applied"), AnimOpenEd::ExtractText(*this, Target, TEXT("after the rollback")), Applied);
		TestFalse(TEXT("after the rollback the package is clean"), Skeleton->GetOutermost()->IsDirty());
		const TOptional<TSet<FName>> ItemsRolledBack = AnimOpenEd::SkeletonTreeItems(*Skeleton);
		TestTrue(TEXT("after the rollback the skeleton tree lists the applied socket"), ItemsRolledBack.IsSet() && ItemsRolledBack->Contains(FName(TEXT("p77_socket"))));
		TestFalse(TEXT("after the rollback the skeleton tree has no failed socket"), ItemsRolledBack.IsSet() && ItemsRolledBack->Contains(FName(TEXT("p77_bad"))));
		AnimOpenEd::TickEditorUi();
	}

	AnimOpenEd::TestHumanUndoRedo(*this, Target, Applied, [](UObject& Asset)
	{
		USkeleton& Edited = *CastChecked<USkeleton>(&Asset);
		Edited.Modify();
		Edited.SetSlotGroupName(TEXT("HumanSlot"), TEXT("DefaultGroup"));
	});
	if (USkeleton* Undone = Target.Get<USkeleton>())
	{
		const TOptional<TSet<FName>> ItemsUndone = AnimOpenEd::SkeletonTreeItems(*Undone);
		TestTrue(TEXT("after undo / redo the skeleton tree lists the socket"), ItemsUndone.IsSet() && ItemsUndone->Contains(FName(TEXT("p77_socket"))));
	}
	AnimOpenEd::CloseEditor(*this, Target);
	return true;
}
#endif // WITH_DEV_AUTOMATION_TESTS
