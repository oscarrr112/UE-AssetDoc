// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// End-to-end Widget Blueprint through the production Service and the Python Profile
// (profile.widget_blueprint.v1): one ApplyFile creates and populates a Widget Blueprint
// (Designer tree with panel, single-content and nested widgets, widget and slot properties,
// navigation, a variable flag, a function binding, desired focus, palette category, design
// size, a button OnClicked event, an animation with float / transform / slot margin / enum tracks, an
// event graph and a pure function); Diff/Extract agree with the document; re-applying the
// extraction changes nothing; a Designer layout-only edit applies; a second apply
// reparents, reorders, removes and adds widgets, unbinds and rekeys; failures in the
// middle of Apply and at compile roll back; the saved package reloads to the same
// document; failed creates leave nothing behind.
//
// UEAssetDoc.WidgetBlueprint.Undo: a human Designer edit + save, then an AI
// apply; Ctrl+Z afterwards must not corrupt the Widget Blueprint (AssetDocumentUndoHistory.h).
//
// UEAssetDoc.WidgetBlueprint.Rename: RenamedFrom is the Designer's rename, including the
// references in another loaded Blueprint (reported; refused while it is unsaved; reloaded after a failed apply).
//
// UEAssetDoc.WidgetBlueprint.BindingSignature: validate refuses a Function
// binding whose signature is not the property's delegate (a double output for ProgressBar Percent, an input, an impure
// function) with Widget.InvalidBindingFunction at the binding, naming what differs and the fix; the right signature
// validates and applies; an update that changes the bound function's signature is refused the same way.
//
// UEAssetDoc.WidgetBlueprint.RawEdits checks the editor-equivalent raw
// edits (AssetDocumentWidgetRawEdits.h) against the editor's own operations.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "AssetDocumentWidgetRawEdits.h"

#include "Animation/WidgetAnimation.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Editor/Transactor.h"
#include "HAL/FileManager.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "MovieScene.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Runtime/AssetDocumentUndoHistory.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditorUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentWidgetBlueprintPythonTests
{
const TCHAR* Folder = TEXT("/Game/AssetDocumentWidgetBlueprintPython");

// Editor operations open transactions; clear the undo buffer before force-deleting fixtures.
void ReleaseUndoHistory()
{
	if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting && !GEditor->IsTransactionActive()
		&& GEditor->Trans->GetQueueLength() > 0)
	{
		GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentWidgetBlueprintPythonTests", "Cleanup", "AssetDocument Widget Blueprint test cleanup"));
	}
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
		PackageName = FString(Folder) / AssetName;
		ObjectPath = PackageName + TEXT(".") + AssetName;
		PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		SidecarPath = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("AssetDocumentWidgetBlueprintPython"), AssetName + TEXT(".assetdoc.json"));
	}

	UWidgetBlueprint* Get() const { return Cast<UWidgetBlueprint>(FSoftObjectPath(ObjectPath).ResolveObject()); }

	void Cleanup() const
	{
		ReleaseUndoHistory();
		if (UWidgetBlueprint* Blueprint = Get())
		{
			if (IFileManager::Get().FileExists(*PackageFile))
			{
				UPackageTools::UnloadPackages({Blueprint->GetOutermost()});
			}
			else
			{
				ObjectTools::DeleteObjects({Blueprint}, false, ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
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

void TestNoDiff(FAutomationTestBase& Test, const FString& Json, const TCHAR* Stage)
{
	FAssetDocumentDiffRequest Request;
	Request.Document = Parse(Json);
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Diff(Request);
	const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
	const bool bOk = Result.IsSuccess() && Result.Payload.IsValid() && Result.Payload->TryGetArrayField(TEXT("changed"), Changed);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: diff succeeds"), Stage), bOk))
	{
		LogResult(Test, Stage, Result);
		return;
	}
	if (!Test.TestEqual(*FString::Printf(TEXT("%s: live asset equals the document"), Stage), Changed->Num(), 0))
	{
		Test.AddInfo(Serialize(Result.Payload));
	}
}

bool ExtractEquals(FAutomationTestBase& Test, const FTarget& Target, const FString& Expected, const TCHAR* Stage)
{
	const FString Actual = Serialize(Extract(Test, Target, Stage));
	if (!Test.TestEqual(*FString::Printf(TEXT("%s: extraction is unchanged"), Stage), Actual, Expected))
	{
		Test.AddInfo(FString::Printf(TEXT("expected %s"), *Expected));
		Test.AddInfo(FString::Printf(TEXT("actual   %s"), *Actual));
		return false;
	}
	return true;
}

bool CompilesClean(const UBlueprint* Blueprint)
{
	return Blueprint != nullptr && (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
}

FString Document(const FTarget& Target, const TCHAR* Action, const FString& Body)
{
	return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/UMGEditor.WidgetBlueprint\",\"Action\":\"%s\",")
		TEXT("\"Properties\":{\"BlueprintDescription\":\"Menu made by AssetDocument\"},\"Body\":{%s}}"),
		*Target.PackageName, Action, *Body);
}

const TCHAR* Graphs = TEXT(R"json(
"UbergraphPages":[{"Name":"EventGraph","Nodes":[
  {"Id":"Ping","Action":"K2:CustomEvent","Name":"Ping"},
  {"Id":"Say","Action":"K2:PrintString","PinOverrides":[{"Pin":"InString","DefaultValue":"ping"}]},
  {"Id":"Clicked","Action":"K2:ComponentBoundEvent:/Script/UMG.Button:OnClicked@OkButton"},
  {"Id":"Tell","Action":"K2:PrintString","PinOverrides":[{"Pin":"InString","DefaultValue":"ok"}]}],
  "Links":[{"From":"Ping:then","To":"Say:execute"},{"From":"Clicked:then","To":"Tell:execute"}]}],
"FunctionGraphs":[{"Name":"GetTitle","Signature":{"Outputs":[{"Name":"ReturnValue","Type":"text"}],"Pure":true},
  "Nodes":[{"Id":"Get","Action":"K2:Get:Caption"}],
  "Links":[{"From":"Entry:then","To":"Result:execute"},{"From":"Get:Caption","To":"Result:ReturnValue"}]}]
)json");

// The full menu: every Designer region at once.
FString InitialBody()
{
	return FString(TEXT(R"json(
"ParentClass":"/Script/UMG.UserWidget",
"Variables":[{"Name":"Caption","Type":"text"},{"Name":"Health","Type":"float","DefaultValue":0.5}],
"ClassDefaults":{},"PaletteCategory":"Menus","DesiredFocus":"OkButton",
"DesignSize":{"Mode":"Custom","Width":1280,"Height":720},
"WidgetTree":{"RootWidget":{"Name":"Root","Class":"/Script/UMG.CanvasPanel","IsVariable":false,"Children":[
    {"Name":"Title","Class":"/Script/UMG.TextBlock","IsVariable":true,"Properties":{"Text":"Welcome"},"Slot":{"ZOrder":2}},
    {"Name":"Box","Class":"/Script/UMG.VerticalBox","IsVariable":false,"Children":[
      {"Name":"OkButton","Class":"/Script/UMG.Button","IsVariable":true,
       "Navigation":{"Down":{"Rule":"Explicit","WidgetToFocus":"CancelButton"}},
       "Children":[{"Name":"OkLabel","Class":"/Script/UMG.TextBlock","IsVariable":false,"Properties":{"Text":"OK"}}]},
      {"Name":"CancelButton","Class":"/Script/UMG.Button","IsVariable":true}]}]},
  "NamedSlotBindings":{}},
"Bindings":[{"Widget":"Title","Property":"Text","Kind":"Function","Function":"GetTitle"}],
"Animations":[{"Name":"Intro","FrameRate":{"Numerator":30,"Denominator":1},"PlaybackRange":{"StartFrame":0,"EndFrame":30},"Tracks":[
  {"Widget":"Title","Property":"RenderOpacity","Type":"Float","Keys":[{"Frame":0,"Value":0},{"Frame":30,"Value":1,"Interp":"Cubic"}]},
  {"Widget":"OkButton","Property":"RenderTransform","Type":"Transform",
   "Channels":[{"Name":"Translation.Y","Keys":[{"Frame":0,"Value":40},{"Frame":20,"Value":0}]}]},
  {"Widget":"Title","Target":"Slot","Property":"LayoutData.Offsets","Type":"Margin",
   "Channels":[{"Name":"Left","Keys":[{"Frame":0,"Value":10},{"Frame":30,"Value":50}]}]},
  {"Widget":"CancelButton","Property":"Visibility","Type":"Enum","Keys":[{"Frame":0,"Value":"Hidden"},{"Frame":15,"Value":"Visible"}]}]}],
)json")) + Graphs;
}

// Reparent OkButton to the root, reorder, remove CancelButton (and its track), add Hint,
// change a widget property, drop the binding, rekey and add an animation.
FString EditedBody()
{
	return FString(TEXT(R"json(
"ParentClass":"/Script/UMG.UserWidget",
"Variables":[{"Name":"Caption","Type":"text"},{"Name":"Health","Type":"float","DefaultValue":0.75}],
"ClassDefaults":{},"PaletteCategory":"Menus","DesiredFocus":"OkButton",
"DesignSize":{"Mode":"DesiredOnScreen","Width":1280,"Height":720},
"WidgetTree":{"RootWidget":{"Name":"Root","Class":"/Script/UMG.CanvasPanel","IsVariable":false,"Children":[
    {"Name":"OkButton","Class":"/Script/UMG.Button","IsVariable":true,
     "Children":[{"Name":"OkLabel","Class":"/Script/UMG.TextBlock","IsVariable":false,"Properties":{"Text":"Continue"}}]},
    {"Name":"Box","Class":"/Script/UMG.VerticalBox","IsVariable":false,"Children":[
      {"Name":"Hint","Class":"/Script/UMG.TextBlock","IsVariable":false,"Properties":{"Text":"Press OK"}},
      {"Name":"Title","Class":"/Script/UMG.TextBlock","IsVariable":true,"Properties":{"Text":"Welcome back"}}]}]},
  "NamedSlotBindings":{}},
"Bindings":[],
"Animations":[
  {"Name":"Intro","FrameRate":{"Numerator":30,"Denominator":1},"PlaybackRange":{"StartFrame":0,"EndFrame":45},"Tracks":[
    {"Widget":"Title","Property":"RenderOpacity","Type":"Float","Keys":[{"Frame":0,"Value":0.25},{"Frame":45,"Value":1}]}]},
  {"Name":"Outro","Tracks":[
    {"Widget":"OkButton","Property":"RenderTransform","Type":"Transform",
     "Channels":[{"Name":"Angle","Keys":[{"Frame":0,"Value":0},{"Frame":10,"Value":15}]}]}]}],
)json")) + Graphs;
}

UWidget* FindWidget(const UWidgetBlueprint* Blueprint, const TCHAR* Name)
{
	return Blueprint != nullptr && Blueprint->WidgetTree != nullptr ? Blueprint->WidgetTree->FindWidget(FName(Name)) : nullptr;
}

UWidgetAnimation* FindAnimation(const UWidgetBlueprint* Blueprint, const TCHAR* Name)
{
	for (UWidgetAnimation* Animation : Blueprint != nullptr ? Blueprint->Animations : TArray<TObjectPtr<UWidgetAnimation>>())
	{
		if (Animation != nullptr && Animation->GetName() == Name)
		{
			return Animation;
		}
	}
	return nullptr;
}

int32 TrackCount(const UWidgetAnimation* Animation)
{
	int32 Count = 0;
	if (Animation != nullptr && Animation->GetMovieScene() != nullptr)
	{
		for (const FMovieSceneBinding& Binding : static_cast<const UMovieScene*>(Animation->GetMovieScene())->GetBindings())
		{
			Count += Binding.GetTracks().Num();
		}
	}
	return Count;
}
} // namespace AssetDocumentWidgetBlueprintPythonTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentWidgetBlueprintPythonTest, "UEAssetDoc.WidgetBlueprint.Python",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentWidgetBlueprintPythonTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentWidgetBlueprintPythonTests;
	const FTarget Target(TEXT("WBP_Menu"));
	const FTarget FailedCreate(TEXT("WBP_Failed"));
	const FTarget WrongParent(TEXT("WBP_WrongParent"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
		FailedCreate.Cleanup();
		WrongParent.Cleanup();
	};

	// 1. One apply creates and populates the Widget Blueprint.
	const FString Created = Document(Target, TEXT("Create"), InitialBody());
	if (!TestTrue(TEXT("create applies"), Apply(*this, Target, Created, TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	UWidgetBlueprint* Blueprint = Target.Get();
	if (!TestNotNull(TEXT("Widget Blueprint exists"), Blueprint))
	{
		return false;
	}
	TestTrue(TEXT("created Widget Blueprint compiles"), CompilesClean(Blueprint));
	TestTrue(TEXT("package is saved"), IFileManager::Get().FileExists(*Target.PackageFile));
	UCanvasPanel* Root = Cast<UCanvasPanel>(Blueprint->WidgetTree->RootWidget);
	UVerticalBox* Box = Cast<UVerticalBox>(FindWidget(Blueprint, TEXT("Box")));
	UButton* Ok = Cast<UButton>(FindWidget(Blueprint, TEXT("OkButton")));
	UTextBlock* Title = Cast<UTextBlock>(FindWidget(Blueprint, TEXT("Title")));
	TestTrue(TEXT("root canvas holds Title then Box"), Root != nullptr && Root->GetChildrenCount() == 2
		&& Root->GetChildAt(0) == Title && Root->GetChildAt(1) == Box);
	TestTrue(TEXT("Box holds OkButton then CancelButton"), Box != nullptr && Box->GetChildrenCount() == 2
		&& Box->GetChildAt(0) == Ok && Box->GetChildAt(1) == FindWidget(Blueprint, TEXT("CancelButton")));
	TestTrue(TEXT("OkButton holds its label"), Ok != nullptr && Ok->GetContent() == FindWidget(Blueprint, TEXT("OkLabel")));
	TestTrue(TEXT("Title text and slot ZOrder"), Title != nullptr && Title->GetText().ToString() == TEXT("Welcome")
		&& Cast<UCanvasPanelSlot>(Title->Slot) != nullptr && Cast<UCanvasPanelSlot>(Title->Slot)->GetZOrder() == 2);
	TestTrue(TEXT("explicit navigation"), Ok != nullptr && Ok->Navigation != nullptr
		&& Ok->Navigation->Down.Rule == EUINavigationRule::Explicit && Ok->Navigation->Down.WidgetToFocus == TEXT("CancelButton"));
	TestTrue(TEXT("function binding"), Blueprint->Bindings.Num() == 1 && Blueprint->Bindings[0].FunctionName == TEXT("GetTitle"));
	TestEqual(TEXT("four animation tracks"), TrackCount(FindAnimation(Blueprint, TEXT("Intro"))), 4);
	const UUserWidget* Defaults = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject<UUserWidget>() : nullptr;
	TestTrue(TEXT("desired focus default"), Defaults != nullptr && Defaults->GetDesiredFocusWidgetName() == TEXT("OkButton"));
	TestTrue(TEXT("palette category"), Defaults != nullptr && Defaults->PaletteCategory.ToString() == TEXT("Menus")
		&& Blueprint->PaletteCategory == TEXT("Menus"));
	TestTrue(TEXT("design size"), Defaults != nullptr && Defaults->DesignSizeMode == EDesignPreviewSizeMode::Custom
		&& Defaults->DesignTimeSize == FVector2D(1280, 720));
	TestTrue(TEXT("OkButton OnClicked event (Details panel '+')"), Blueprint->UbergraphPages.Num() > 0
		&& Blueprint->UbergraphPages[0]->Nodes.ContainsByPredicate([](const UEdGraphNode* Node)
		{
			const UK2Node_ComponentBoundEvent* Bound = Cast<UK2Node_ComponentBoundEvent>(Node);
			return Bound != nullptr && Bound->ComponentPropertyName == TEXT("OkButton") && Bound->DelegatePropertyName == TEXT("OnClicked");
		}));
	TestTrue(TEXT("Title is a generated variable"), Blueprint->GeneratedClass && FindFProperty<FObjectProperty>(Blueprint->GeneratedClass, TEXT("Title")));
	TestTrue(TEXT("Intro is a generated variable"), Blueprint->GeneratedClass && FindFProperty<FObjectProperty>(Blueprint->GeneratedClass, TEXT("Intro")));
	TestNoDiff(*this, Created, TEXT("after create"));

	// 2. Re-applying the extraction is a no-op.
	const TSharedPtr<FJsonObject> First = Extract(*this, Target, TEXT("first extract"));
	if (!First.IsValid())
	{
		return false;
	}
	First->SetStringField(TEXT("Action"), TEXT("Update"));
	const FString Extracted = Serialize(First);
	AddInfo(FString::Printf(TEXT("extracted: %s"), *Extracted));
	TestFalse(TEXT("created package is saved clean"), Blueprint->GetOutermost()->IsDirty());
	const FDateTime SavedStamp = IFileManager::Get().GetTimeStamp(*Target.PackageFile);
	int32 Compiles = 0;
	const FDelegateHandle CompiledHandle = Blueprint->OnCompiled().AddLambda([&Compiles](UBlueprint*) { ++Compiles; });
	const FAssetDocumentResult Reapplied = Apply(*this, Target, Extracted, TEXT("Reapply"));
	Blueprint->OnCompiled().Remove(CompiledHandle);
	TestTrue(TEXT("extraction re-applies"), Reapplied.IsSuccess());
	TestEqual(TEXT("no-op re-apply does not recompile"), Compiles, 0);
	TestFalse(TEXT("no-op re-apply leaves the package clean"), Blueprint->GetOutermost()->IsDirty());
	TestFalse(TEXT("no-op re-apply reports no save"), Reapplied.bSavedAsset);
	TestEqual(TEXT("no-op re-apply does not rewrite the package file"), IFileManager::Get().GetTimeStamp(*Target.PackageFile), SavedStamp);
	First->SetStringField(TEXT("Action"), TEXT("CreateOrUpdate"));
	const FString Baseline = Serialize(First);
	ExtractEquals(*this, Target, Baseline, TEXT("after re-apply"));
	TestTrue(TEXT("same Widget Blueprint object"), Target.Get() == Blueprint);
	TestNoDiff(*this, Extracted, TEXT("extraction"));

	// 2b. A layout-only Designer edit (the Title slot's ZOrder) of the extraction applies.
	{
		const TSharedPtr<FJsonObject> Moved = Parse(Extracted);
		const TSharedPtr<FJsonObject> RootNode = Moved->GetObjectField(TEXT("Body"))->GetObjectField(TEXT("WidgetTree"))->GetObjectField(TEXT("RootWidget"));
		const TSharedPtr<FJsonObject> TitleNode = RootNode->GetArrayField(TEXT("Children"))[0]->AsObject();
		TitleNode->GetObjectField(TEXT("Slot"))->SetNumberField(TEXT("ZOrder"), 7);
		const FAssetDocumentResult MovedResult = Apply(*this, Target, Serialize(Moved), TEXT("Layout"));
		TestTrue(TEXT("layout-only edit applies"), MovedResult.IsSuccess());
		TestTrue(TEXT("layout-only edit is saved"), MovedResult.bSavedAsset);
		Title = Cast<UTextBlock>(FindWidget(Blueprint, TEXT("Title")));
		TestTrue(TEXT("layout-only edit moved Title forward"),
			Title != nullptr && Cast<UCanvasPanelSlot>(Title->Slot) != nullptr && Cast<UCanvasPanelSlot>(Title->Slot)->GetZOrder() == 7);
		TestNoDiff(*this, Serialize(Moved), TEXT("after layout edit"));
	}

	// 2c. A property the document sets that the asset holds at its class default (the extraction omits
	//     it) diffs as Replace with the real current value; setting exactly the default is no change.
	{
		const TSharedPtr<FJsonObject> Current = Extract(*this, Target, TEXT("before default diff"));
		const TSharedPtr<FJsonObject> TitleNode = Current.IsValid() ? Current->GetObjectField(TEXT("Body"))->GetObjectField(TEXT("WidgetTree"))
			->GetObjectField(TEXT("RootWidget"))->GetArrayField(TEXT("Children"))[0]->AsObject() : nullptr;
		if (TestTrue(TEXT("Title node is the first root child"), TitleNode.IsValid() && TitleNode->GetStringField(TEXT("Name")) == TEXT("Title")))
		{
			TestFalse(TEXT("AutoWrapText (default) is not extracted"), TitleNode->GetObjectField(TEXT("Properties"))->HasField(TEXT("AutoWrapText")));
			TitleNode->GetObjectField(TEXT("Properties"))->SetBoolField(TEXT("AutoWrapText"), true);
			FAssetDocumentDiffRequest Request;
			Request.Document = Current;
			const FAssetDocumentResult Diffed = FAssetDocumentModule::Get().GetService().Diff(Request);
			const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
			if (TestTrue(TEXT("default diff succeeds"), Diffed.IsSuccess() && Diffed.Payload.IsValid() && Diffed.Payload->TryGetArrayField(TEXT("changed"), Changed)))
			{
				AddInfo(Serialize(Diffed.Payload));
				const TSharedPtr<FJsonObject> Entry = Changed->Num() == 1 ? (*Changed)[0]->AsObject() : nullptr;
				bool bBefore = true;
				bool bAfter = false;
				TestTrue(TEXT("one Replace at the property with the real before value"), Entry.IsValid()
					&& Entry->GetStringField(TEXT("path")) == TEXT("/Body/WidgetTree/RootWidget/Children/0/Properties/AutoWrapText")
					&& Entry->GetStringField(TEXT("kind")) == TEXT("Replace")
					&& Entry->TryGetBoolField(TEXT("before"), bBefore) && !bBefore
					&& Entry->TryGetBoolField(TEXT("after"), bAfter) && bAfter);
			}
			TitleNode->GetObjectField(TEXT("Properties"))->SetBoolField(TEXT("AutoWrapText"), false);
			TestNoDiff(*this, Serialize(Current), TEXT("document sets the default"));
		}
	}

	// 3. A second apply reparents, reorders, removes, adds, unbinds and rekeys.
	const FString Edited = Document(Target, TEXT("Update"), EditedBody());
	if (TestTrue(TEXT("edit applies"), Apply(*this, Target, Edited, TEXT("Edit")).IsSuccess()))
	{
		TestNoDiff(*this, Edited, TEXT("after edit"));
		Root = Cast<UCanvasPanel>(Blueprint->WidgetTree->RootWidget);
		Box = Cast<UVerticalBox>(FindWidget(Blueprint, TEXT("Box")));
		TestTrue(TEXT("OkButton moved to the root"), Root != nullptr && Root->GetChildAt(0) == FindWidget(Blueprint, TEXT("OkButton")));
		TestTrue(TEXT("Box holds Hint then Title"), Box != nullptr && Box->GetChildrenCount() == 2
			&& Box->GetChildAt(0) == FindWidget(Blueprint, TEXT("Hint")) && Box->GetChildAt(1) == FindWidget(Blueprint, TEXT("Title")));
		TestNull(TEXT("CancelButton removed"), FindWidget(Blueprint, TEXT("CancelButton")));
		TestEqual(TEXT("binding removed"), Blueprint->Bindings.Num(), 0);
		TestEqual(TEXT("Intro keeps one track"), TrackCount(FindAnimation(Blueprint, TEXT("Intro"))), 1);
		TestEqual(TEXT("Outro added"), TrackCount(FindAnimation(Blueprint, TEXT("Outro"))), 1);
		TestTrue(TEXT("design size mode changed"),
			Blueprint->GeneratedClass->GetDefaultObject<UUserWidget>()->DesignSizeMode == EDesignPreviewSizeMode::DesiredOnScreen);
		TestTrue(TEXT("edited Widget Blueprint compiles"), CompilesClean(Blueprint));
	}
	const FString EditedBaseline = Serialize(Extract(*this, Target, TEXT("edited extract")));

	// 4. Failure in the middle of Apply (the tree is already edited, then a binding to a missing function).
	const FString MidApply = Document(Target, TEXT("Update"), TEXT(
		"\"WidgetTree\":{\"RootWidget\":{\"Name\":\"Other\",\"Class\":\"/Script/UMG.Overlay\"}},"
		"\"Bindings\":[{\"Widget\":\"Other\",\"Property\":\"Visibility\",\"Kind\":\"Function\",\"Function\":\"Missing\"}]"));
	const FAssetDocumentResult MidFailed = Apply(*this, Target, MidApply, TEXT("MidApplyFailure"));
	TestFalse(TEXT("mid-apply failure fails"), MidFailed.IsSuccess());
	TestTrue(TEXT("failure names the binding"), MidFailed.Diagnostics.ContainsByPredicate(
		[](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == TEXT("Widget.BindingFunctionNotFound"); }));
	Blueprint = Target.Get();
	TestTrue(TEXT("rolled-back Widget Blueprint compiles"), CompilesClean(Blueprint));
	TestTrue(TEXT("rolled-back package is clean"), Blueprint != nullptr && !Blueprint->GetOutermost()->IsDirty());
	ExtractEquals(*this, Target, EditedBaseline, TEXT("after mid-apply rollback"));

	// 5. Failure at compile: a ForEachLoop whose Array type is undetermined (editor compiler error).
	const FString CompileError = Document(Target, TEXT("Update"), TEXT(
		"\"WidgetTree\":{\"RootWidget\":null},"
		"\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":["
		"{\"Id\":\"Begin\",\"Action\":\"K2:CustomEvent\",\"Name\":\"Begin\"},"
		"{\"Id\":\"Loop\",\"Action\":\"K2:ForEachLoop\"}],"
		"\"Links\":[{\"From\":\"Begin:then\",\"To\":\"Loop:Exec\"}]}]"));
	const FAssetDocumentResult CompileFailed = Apply(*this, Target, CompileError, TEXT("CompileFailure"));
	TestFalse(TEXT("compile failure fails"), CompileFailed.IsSuccess());
	TestTrue(TEXT("failure is the compiler's"), CompileFailed.Diagnostics.ContainsByPredicate(
		[](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == TEXT("Graph.FinalizeFailed"); }));
	Blueprint = Target.Get();
	TestTrue(TEXT("Widget Blueprint compiles after compile-failure rollback"), CompilesClean(Blueprint));
	ExtractEquals(*this, Target, EditedBaseline, TEXT("after compile rollback"));

	// 6. The saved package, unloaded and loaded back from disk, extracts to the same document.
	if (Blueprint != nullptr)
	{
		TestFalse(TEXT("package is clean before unload"), Blueprint->GetOutermost()->IsDirty());
		ReleaseUndoHistory();
		UPackageTools::UnloadPackages({Blueprint->GetOutermost()});
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		Blueprint = nullptr;
		TestNull(TEXT("Widget Blueprint is unloaded"), Target.Get());
		ExtractEquals(*this, Target, EditedBaseline, TEXT("after reload from disk"));
		TestTrue(TEXT("reloaded Widget Blueprint compiles"), CompilesClean(Target.Get()));
	}

	// 7. Failed creates leave no asset, package file or registry entry.
	const FString BadCreate = Document(FailedCreate, TEXT("Create"), TEXT(
		"\"ParentClass\":\"/Script/UMG.UserWidget\","
		"\"WidgetTree\":{\"RootWidget\":{\"Name\":\"Root\",\"Class\":\"/Script/UMG.CanvasPanel\",\"Children\":["
		"{\"Name\":\"Broken\",\"Class\":\"/Script/Engine.Actor\"}]}}"));
	TestFalse(TEXT("failed create fails"), Apply(*this, FailedCreate, BadCreate, TEXT("FailedCreate")).IsSuccess());
	TestNull(TEXT("no created Widget Blueprint remains"), FailedCreate.Get());
	TestFalse(TEXT("no package file"), IFileManager::Get().FileExists(*FailedCreate.PackageFile));
	TestFalse(TEXT("no registry entry"),
		FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(FailedCreate.ObjectPath)).IsValid());
	const FString ActorParent = Document(WrongParent, TEXT("Create"), TEXT("\"ParentClass\":\"/Script/Engine.Actor\""));
	TestFalse(TEXT("an Actor parent is rejected"), Apply(*this, WrongParent, ActorParent, TEXT("WrongParent")).IsSuccess());
	TestNull(TEXT("no Widget Blueprint with a wrong parent"), WrongParent.Get());
	TestFalse(TEXT("no package file for the wrong parent"), IFileManager::Get().FileExists(*WrongParent.PackageFile));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentWidgetBlueprintUndoTest, "UEAssetDoc.WidgetBlueprint.Undo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentWidgetBlueprintUndoTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentWidgetBlueprintPythonTests;
	const FTarget Target(TEXT("WBP_Undo"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	if (GEditor == nullptr || GEditor->Trans == nullptr
		|| !TestTrue(TEXT("create applies"), Apply(*this, Target, Document(Target, TEXT("Create"), InitialBody()), TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	ReleaseUndoHistory();
	auto Live = [&Target]() { return Target.Get(); };
	// Undo (then redo) everything the history holds; the asset must stay whole at every step.
	auto UndoRedoAll = [this, &Live](const TCHAR* Stage)
	{
		int32 Undone = 0;
		while (Undone < 16 && GEditor->Trans->CanUndo() && GEditor->UndoTransaction())
		{
			++Undone;
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			UWidgetBlueprint* Blueprint = Live();
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
			TestTrue(*FString::Printf(TEXT("%s: compiles after undo %d"), Stage, Undone), CompilesClean(Blueprint));
		}
		int32 Redone = 0;
		while (Redone < Undone && GEditor->Trans->CanRedo() && GEditor->RedoTransaction())
		{
			++Redone;
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		TestEqual(*FString::Printf(TEXT("%s: every undone step redoes"), Stage), Redone, Undone);
		return Undone;
	};

	// 1. The human adds a widget and fades Title in one Designer transaction, and saves.
	{
		UWidgetBlueprint* Blueprint = Live();
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentWidgetBlueprintPythonTests", "UserEdit", "Add Widget"));
		Blueprint->Modify();
		Blueprint->WidgetTree->SetFlags(RF_Transactional);
		Blueprint->WidgetTree->Modify();
		UVerticalBox* Box = Cast<UVerticalBox>(FindWidget(Blueprint, TEXT("Box")));
		Box->Modify();
		UTextBlock* Added = Blueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("UserHint"));
		Box->AddChild(Added);
		Blueprint->OnVariableAdded(Added->GetFName());
		UWidget* Title = FindWidget(Blueprint, TEXT("Title"));
		Title->Modify();
		Title->SetRenderOpacity(0.4f);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}
	TestTrue(TEXT("human save"), UEditorLoadingAndSavingUtils::SavePackages({Live()->GetOutermost()}, false));
	TestTrue(TEXT("the human edit is in the undo history"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));

	// 2. The AI rewrites the Designer (UserHint is not in the document): history referencing it is cleared.
	if (!TestTrue(TEXT("AI edit applies"), Apply(*this, Target, Document(Target, TEXT("Update"), EditedBody()), TEXT("Edit")).IsSuccess()))
	{
		return false;
	}
	TestNull(TEXT("AI edit removed UserHint"), FindWidget(Live(), TEXT("UserHint")));
	TestFalse(TEXT("after AI edit: no undo history references the asset"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	const FString AfterEdit = Serialize(Extract(*this, Target, TEXT("after AI edit")));
	const float AIOpacity = FindWidget(Live(), TEXT("Title"))->GetRenderOpacity();
	UndoRedoAll(TEXT("after AI edit"));
	ExtractEquals(*this, Target, AfterEdit, TEXT("after AI edit + undo/redo"));

	// 3. The human fades Title again, then an AI apply fails mid-way and rolls back: the human's
	//    step stays undoable and undoes onto the restored (functionally equal) objects.
	ReleaseUndoHistory();
	{
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentWidgetBlueprintPythonTests", "UserFade", "Fade Title"));
		UWidget* Title = FindWidget(Live(), TEXT("Title"));
		Title->Modify();
		Title->SetRenderOpacity(0.6f);
		FBlueprintEditorUtils::MarkBlueprintAsModified(Live());
	}
	const int32 HumanSteps = GEditor->Trans->GetQueueLength();
	const FString MidApply = Document(Target, TEXT("Update"), TEXT(
		"\"WidgetTree\":{\"RootWidget\":{\"Name\":\"Other\",\"Class\":\"/Script/UMG.Overlay\"}},"
		"\"Bindings\":[{\"Widget\":\"Other\",\"Property\":\"Visibility\",\"Kind\":\"Function\",\"Function\":\"Missing\"}]"));
	TestFalse(TEXT("mid-apply failure fails"), Apply(*this, Target, MidApply, TEXT("MidApplyFailure")).IsSuccess());
	TestEqual(TEXT("after rollback: the human history is kept"), GEditor->Trans->GetQueueLength(), HumanSteps);
	TestTrue(TEXT("after rollback: Title keeps the human fade"), FindWidget(Live(), TEXT("Title")) != nullptr
		&& FMath::IsNearlyEqual(FindWidget(Live(), TEXT("Title"))->GetRenderOpacity(), 0.6f));
	TestTrue(TEXT("Ctrl+Z undoes the human fade"), GEditor->UndoTransaction());
	UWidget* Title = FindWidget(Live(), TEXT("Title"));
	TestTrue(TEXT("after undo: Title is back to the AI state"), Title != nullptr && FMath::IsNearlyEqual(Title->GetRenderOpacity(), AIOpacity));
	FKismetEditorUtilities::CompileBlueprint(Live());
	TestTrue(TEXT("after undo: compiles"), CompilesClean(Live()));
	ExtractEquals(*this, Target, AfterEdit, TEXT("after rollback + undo"));
	TestTrue(TEXT("Ctrl+Y redoes the human fade"), GEditor->RedoTransaction());
	Title = FindWidget(Live(), TEXT("Title"));
	TestTrue(TEXT("after redo: Title is faded again"), Title != nullptr && FMath::IsNearlyEqual(Title->GetRenderOpacity(), 0.6f));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentWidgetBlueprintBindingSignatureTest, "UEAssetDoc.WidgetBlueprint.BindingSignature",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentWidgetBlueprintBindingSignatureTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentWidgetBlueprintPythonTests;
	const FTarget Target(TEXT("WBP_Health"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
	};
	// A health bar whose Percent binds GetHealthPercent with Signature (and a Text binding that is right throughout).
	const auto Body = [](const TCHAR* Signature)
		{
			return FString::Printf(TEXT(R"json(
"ParentClass":"/Script/UMG.UserWidget",
"Variables":[{"Name":"Health","Type":"float32","DefaultValue":0.5}],
"WidgetTree":{"RootWidget":{"Name":"Root","Class":"/Script/UMG.CanvasPanel","IsVariable":false,"Children":[
    {"Name":"Bar","Class":"/Script/UMG.ProgressBar","IsVariable":true},
    {"Name":"Label","Class":"/Script/UMG.TextBlock","IsVariable":true}]},"NamedSlotBindings":{}},
"Bindings":[{"Widget":"Bar","Property":"Percent","Kind":"Function","Function":"GetHealthPercent"},
  {"Widget":"Label","Property":"Text","Kind":"Function","Function":"GetLabel"}],
"FunctionGraphs":[{"Name":"GetLabel","Signature":{"Outputs":[{"Name":"ReturnValue","Type":"text"}],"Pure":true},
  "Links":[{"From":"Entry:then","To":"Result:execute"}]},
 {"Name":"GetHealthPercent","Signature":%s,
  "Nodes":[{"Id":"Get","Action":"K2:Get:Health"}],
  "Links":[{"From":"Entry:then","To":"Result:execute"}]}]
)json"), Signature);
		};
	const auto Validate = [this](const FString& Json, const TCHAR* Stage)
		{
			FAssetDocumentValidateRequest Request;
			Request.Document = Parse(Json);
			const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Validate(Request);
			LogResult(*this, Stage, Result);
			return Result;
		};
	const auto TestRefused = [this](const FAssetDocumentResult& Result, const TCHAR* Stage, std::initializer_list<const TCHAR*> Mentions)
		{
			TestFalse(*FString::Printf(TEXT("%s: refused"), Stage), Result.IsSuccess());
			const FAssetDocumentDiagnostic* Problem = Result.Diagnostics.FindByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
				{
					return Diagnostic.Code == TEXT("Widget.InvalidBindingFunction");
				});
			if (!TestNotNull(*FString::Printf(TEXT("%s: Widget.InvalidBindingFunction"), Stage), Problem))
			{
				return;
			}
			TestEqual(*FString::Printf(TEXT("%s: at the binding"), Stage), Problem->Path, FString(TEXT("/Body/Bindings/0/Function")));
			for (const TCHAR* Mention : Mentions)
			{
				TestTrue(*FString::Printf(TEXT("%s: the message says '%s'"), Stage, Mention), Problem->Message.Contains(Mention));
			}
		};
	const TCHAR* DoubleOutput = TEXT(R"json({"Outputs":[{"Name":"ReturnValue","Type":"float"}],"Pure":true})json");
	const TCHAR* WithInput = TEXT(R"json({"Inputs":[{"Name":"Scale","Type":"float32"}],"Outputs":[{"Name":"ReturnValue","Type":"float32"}],"Pure":true})json");
	const TCHAR* Impure = TEXT(R"json({"Outputs":[{"Name":"ReturnValue","Type":"float32"}]})json");
	const TCHAR* Right = TEXT(R"json({"Outputs":[{"Name":"ReturnValue","Type":"float32"}],"Pure":true})json");

	// 1. Create: validate names each difference and changes nothing.
	TestRefused(Validate(Document(Target, TEXT("Create"), Body(DoubleOutput)), TEXT("double output")),
		TEXT("double output"), {TEXT("GetHealthPercent"), TEXT("returns double (float)"), TEXT("Percent binds float32"), TEXT("return float32")});
	TestRefused(Validate(Document(Target, TEXT("Create"), Body(WithInput)), TEXT("input")),
		TEXT("input"), {TEXT("takes 1 input(s)"), TEXT("remove its inputs")});
	TestRefused(Validate(Document(Target, TEXT("Create"), Body(Impure)), TEXT("impure")),
		TEXT("impure"), {TEXT("not pure"), TEXT("Pure: true")});
	TestNull(TEXT("validate created nothing"), Target.Get());

	// 2. The right signature validates and applies.
	TestTrue(TEXT("right signature validates"), Validate(Document(Target, TEXT("Create"), Body(Right)), TEXT("right")).IsSuccess());
	if (!TestTrue(TEXT("right signature applies"), Apply(*this, Target, Document(Target, TEXT("Create"), Body(Right)), TEXT("create")).IsSuccess()))
	{
		return false;
	}

	// 3. Update: the bound function's new signature is checked too.
	TestRefused(Validate(Document(Target, TEXT("Update"), Body(DoubleOutput)), TEXT("update double output")),
		TEXT("update double output"), {TEXT("returns double (float)"), TEXT("return float32")});
	TestTrue(TEXT("unchanged update validates"), Validate(Document(Target, TEXT("Update"), Body(Right)), TEXT("update right")).IsSuccess());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentWidgetBlueprintRawEditsTest, "UEAssetDoc.WidgetBlueprint.RawEdits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// Raw::DeleteWidget does what FWidgetBlueprintEditorUtils::DeleteWidgets(DeleteSilently) does
// (minus the transaction); Raw::Place/Detach move widgets like the hierarchy drag and drop;
// Raw::CreateAnimation/DeleteAnimation match the Animations tab.
bool FAssetDocumentWidgetBlueprintRawEditsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentWidgetBlueprintPythonTests;
	namespace Raw = UE::AssetDocument::Graph::Widget::Raw;
	TArray<UWidgetBlueprint*> Fixtures;
	ON_SCOPE_EXIT
	{
		ReleaseUndoHistory();
		for (UWidgetBlueprint* Fixture : Fixtures)
		{
			ObjectTools::DeleteObjects({Fixture}, false, ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
		}
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	};
	// Root canvas > Panel (vertical box) > Label; Label is bound, focused and read in the graph.
	auto MakeFixture = [&Fixtures](const TCHAR* Name)
	{
		UPackage* Package = CreatePackage(*(FString(Folder) / FString::Printf(TEXT("%s_%s"), Name, *FGuid::NewGuid().ToString(EGuidFormats::Digits))));
		UWidgetBlueprint* Blueprint = CastChecked<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(UUserWidget::StaticClass(), Package,
			FName(Name), BPTYPE_Normal, UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass()));
		Fixtures.Add(Blueprint);
		UCanvasPanel* Root = Blueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
		Blueprint->WidgetTree->RootWidget = Root;
		UVerticalBox* Panel = Blueprint->WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Panel"));
		Root->AddChild(Panel);
		UTextBlock* Label = Blueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Label"));
		Label->bIsVariable = true;
		Panel->AddChild(Label);
		Blueprint->OnVariableAdded(Root->GetFName());
		Blueprint->OnVariableAdded(Panel->GetFName());
		Blueprint->OnVariableAdded(Label->GetFName());
		FDelegateEditorBinding Binding;
		Binding.ObjectName = TEXT("Label");
		Binding.PropertyName = TEXT("Text");
		Binding.Kind = EBindingKind::Function;
		Binding.FunctionName = TEXT("GetLabel");
		Blueprint->Bindings.Add(Binding);
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		FWidgetBlueprintEditorUtils::SetDesiredFocus(Blueprint, FName(TEXT("Label")));
		UEdGraph* EventGraph = Blueprint->UbergraphPages[0];
		UK2Node_VariableGet* Get = NewObject<UK2Node_VariableGet>(EventGraph);
		Get->VariableReference.SetSelfMember(TEXT("Label"));
		EventGraph->AddNode(Get, false, false);
		Get->AllocateDefaultPins();
		return Blueprint;
	};
	auto LabelReads = [](const UWidgetBlueprint* Blueprint)
	{
		int32 Count = 0;
		for (const UEdGraphNode* Node : Blueprint->UbergraphPages[0]->Nodes)
		{
			const UK2Node_VariableGet* Get = Cast<UK2Node_VariableGet>(Node);
			Count += Get != nullptr && Get->GetVarName() == TEXT("Label") ? 1 : 0;
		}
		return Count;
	};
	auto Focus = [](const UWidgetBlueprint* Blueprint)
	{
		return Blueprint->GeneratedClass->GetDefaultObject<UUserWidget>()->GetDesiredFocusWidgetName();
	};

	UWidgetBlueprint* Editor = MakeFixture(TEXT("WBP_EditorDelete"));
	UWidgetBlueprint* Ours = MakeFixture(TEXT("WBP_RawDelete"));
	TestEqual(TEXT("fixture reads Label in the graph"), LabelReads(Ours), 1);
	TestEqual(TEXT("fixture focuses Label"), Focus(Ours), FName(TEXT("Label")));

	FWidgetBlueprintEditorUtils::DeleteWidgets(Editor, {Editor->WidgetTree->FindWidget(TEXT("Panel"))},
		FWidgetBlueprintEditorUtils::EDeleteWidgetWarningType::DeleteSilently);
	UWidget* Panel = Ours->WidgetTree->FindWidget(TEXT("Panel"));
	UWidget* Label = Ours->WidgetTree->FindWidget(TEXT("Label"));
	Raw::DeleteWidget(*Ours, *Panel, false);
	// (Checked before compiling: the compile collects garbage and the deleted widgets with it.)
	TestEqual(TEXT("deleted widgets move to the transient package"), Label->GetOutermost(), GetTransientPackage());
	for (UWidgetBlueprint* Blueprint : {Editor, Ours})
	{
		const FString Who = Blueprint == Editor ? TEXT("editor") : TEXT("raw");
		TestNull(*(Who + TEXT(": Panel gone from the tree")), Blueprint->WidgetTree->FindWidget(TEXT("Panel")));
		TestNull(*(Who + TEXT(": Label gone from the tree")), Blueprint->WidgetTree->FindWidget(TEXT("Label")));
		TestEqual(*(Who + TEXT(": root keeps no children")), Cast<UCanvasPanel>(Blueprint->WidgetTree->RootWidget)->GetChildrenCount(), 0);
		TestEqual(*(Who + TEXT(": graph reads of deleted widgets removed")), LabelReads(Blueprint), 0);
		TestTrue(*(Who + TEXT(": deleted widget names are free")), FindObjectFast<UWidget>(Blueprint->WidgetTree, TEXT("Label")) == nullptr);
		TestFalse(*(Who + TEXT(": variable GUID dropped")), Blueprint->WidgetVariableNameToGuidMap.Contains(TEXT("Label")));
	}
	// Only the deleted widget's own bindings and focus are cleaned up; a descendant's stay (editor behaviour).
	TestEqual(TEXT("raw keeps the same bindings as the editor"), Ours->Bindings.Num(), Editor->Bindings.Num());
	FKismetEditorUtilities::CompileBlueprint(Editor);
	FKismetEditorUtilities::CompileBlueprint(Ours);
	TestEqual(TEXT("raw keeps the same desired focus as the editor"), Focus(Ours), Focus(Editor));
	{
		// Deleting the focused, bound widget itself clears both.
		UWidgetBlueprint* Direct = MakeFixture(TEXT("WBP_RawDeleteDirect"));
		Raw::DeleteWidget(*Direct, *Direct->WidgetTree->FindWidget(TEXT("Label")), false);
		TestEqual(TEXT("direct delete removes the binding"), Direct->Bindings.Num(), 0);
		TestEqual(TEXT("direct delete clears the desired focus"), Focus(Direct), FName());
		UWidgetBlueprint* Kept = MakeFixture(TEXT("WBP_RawDeleteKeep"));
		Raw::DeleteWidget(*Kept, *Kept->WidgetTree->FindWidget(TEXT("Label")), true);
		TestEqual(TEXT("keep_references keeps the binding"), Kept->Bindings.Num(), 1);
		TestEqual(TEXT("keep_references keeps the graph read"), LabelReads(Kept), 1);
		TestEqual(TEXT("keep_references keeps the focus"), Focus(Kept), FName(TEXT("Label")));
		TestTrue(TEXT("keep_references keeps the variable GUID"), Kept->WidgetVariableNameToGuidMap.Contains(TEXT("Label")));
		UTextBlock* Again = Kept->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Label"));
		Cast<UPanelWidget>(Kept->WidgetTree->FindWidget(TEXT("Panel")))->AddChild(Again);
		FKismetEditorUtilities::CompileBlueprint(Kept);
		TestTrue(TEXT("a re-created widget takes the name back"), CompilesClean(Kept));
	}

	// Detach + Place: the hierarchy drag and drop between panels and the root.
	UWidgetBlueprint* Moves = MakeFixture(TEXT("WBP_RawPlace"));
	UWidget* MoveLabel = Moves->WidgetTree->FindWidget(TEXT("Label"));
	UWidget* MovePanel = Moves->WidgetTree->FindWidget(TEXT("Panel"));
	UWidget* MoveRoot = Moves->WidgetTree->RootWidget;
	FString Error;
	TestTrue(TEXT("Label moves to the root canvas at index 0"), Raw::Place(*Moves, *MoveLabel, MoveRoot, NAME_None, 0, Error));
	TestTrue(TEXT("Label is the first root child"), Cast<UPanelWidget>(MoveRoot)->GetChildAt(0) == MoveLabel);
	TestEqual(TEXT("Panel is empty"), Cast<UPanelWidget>(MovePanel)->GetChildrenCount(), 0);
	TestFalse(TEXT("a widget cannot go into itself"), Raw::Place(*Moves, *MovePanel, MovePanel, NAME_None, 0, Error));
	TestFalse(TEXT("a text block takes no children"), Raw::Place(*Moves, *MovePanel, MoveLabel, NAME_None, 0, Error));
	TestTrue(TEXT("detach the root"), Raw::Detach(*Moves, *MoveRoot));
	TestNull(TEXT("no root"), Moves->WidgetTree->RootWidget.Get());
	Raw::Detach(*Moves, *MovePanel); // a failed Place may already have detached it (the apply then rolls back)
	TestTrue(TEXT("Panel becomes the root"), Raw::Place(*Moves, *MovePanel, nullptr, NAME_None, INDEX_NONE, Error));
	TestTrue(TEXT("root replaced"), Moves->WidgetTree->RootWidget == MovePanel);
	TestFalse(TEXT("an occupied root is not overwritten"), Raw::Place(*Moves, *MoveRoot, nullptr, NAME_None, INDEX_NONE, Error));
	Raw::Detach(*Moves, *MoveLabel);
	TestTrue(TEXT("Label goes back into Panel"), Raw::Place(*Moves, *MoveLabel, MovePanel, NAME_None, INDEX_NONE, Error));
	Raw::DeleteWidget(*Moves, *MoveRoot, false);
	FKismetEditorUtilities::CompileBlueprint(Moves);
	TestTrue(TEXT("the rearranged tree compiles"), CompilesClean(Moves));

	// Palette Category row: generated CDO + asset registry copy; "" is the UUserWidget default.
	Raw::SetPaletteCategory(*Moves, TEXT("Menus"));
	TestEqual(TEXT("category read back"), Raw::GetPaletteCategory(*Moves), FString(TEXT("Menus")));
	TestEqual(TEXT("asset registry copy"), Moves->PaletteCategory, FString(TEXT("Menus")));
	FKismetEditorUtilities::CompileBlueprint(Moves);
	TestEqual(TEXT("category survives a compile"), Raw::GetPaletteCategory(*Moves), FString(TEXT("Menus")));
	Raw::SetPaletteCategory(*Moves, FString());
	TestEqual(TEXT("empty restores the default"), Moves->GeneratedClass->GetDefaultObject<UUserWidget>()->PaletteCategory.ToString(),
		GetDefault<UUserWidget>()->PaletteCategory.ToString());

	// Designer toolbar size: CDO DesignSizeMode/DesignTimeSize, marked modified without a recompile.
	FKismetEditorUtilities::CompileBlueprint(Moves);
	Raw::FDesignSize Size;
	TestTrue(TEXT("design size reads"), Raw::GetDesignSize(*Moves, Size));
	TestTrue(TEXT("default design size"), Size.Mode == TEXT("FillScreen") && Size.Width == 100 && Size.Height == 100);
	const UClass* CompiledClass = Moves->GeneratedClass;
	TestTrue(TEXT("design size sets"), Raw::SetDesignSize(*Moves, {TEXT("CustomOnScreen"), 800, 600}));
	const UUserWidget* MovesDefaults = Moves->GeneratedClass->GetDefaultObject<UUserWidget>();
	TestTrue(TEXT("CDO size fields"), MovesDefaults->DesignSizeMode == EDesignPreviewSizeMode::CustomOnScreen
		&& MovesDefaults->DesignTimeSize == FVector2D(800, 600));
	TestTrue(TEXT("size edit does not recompile"), Moves->GeneratedClass == CompiledClass && Moves->Status == BS_Dirty);
	FKismetEditorUtilities::CompileBlueprint(Moves);
	TestTrue(TEXT("size survives a compile"), Raw::GetDesignSize(*Moves, Size) && Size.Mode == TEXT("CustomOnScreen") && Size.Width == 800);
	TestFalse(TEXT("an unknown mode is rejected"), Raw::SetDesignSize(*Moves, {TEXT("Huge"), 1, 1}));

	// Animations tab: New Animation + rename, Delete Animation.
	UWidgetAnimation* Animation = Raw::CreateAnimation(*Moves, TEXT("Blink"));
	TestTrue(TEXT("animation added"), Animation != nullptr && Moves->Animations.Contains(Animation) && Animation->GetMovieScene() != nullptr);
	TestEqual(TEXT("editor display rate"), Animation->GetMovieScene()->GetDisplayRate(), FFrameRate(20, 1));
	FKismetEditorUtilities::CompileBlueprint(Moves);
	TestTrue(TEXT("animation variable generated"), FindFProperty<FObjectProperty>(Moves->GeneratedClass, TEXT("Blink")) != nullptr);
	Raw::DeleteAnimation(*Moves, *Animation);
	TestFalse(TEXT("animation removed"), Moves->Animations.Contains(Animation));
	TestEqual(TEXT("deleted animation moves to the transient package"), Animation->GetOutermost(), GetTransientPackage());
	FKismetEditorUtilities::CompileBlueprint(Moves);
	TestNull(TEXT("animation variable gone"), FindFProperty<FObjectProperty>(Moves->GeneratedClass, TEXT("Blink")));
	return !HasAnyErrors();
}

// A case-only widget name change (OkButton -> OKButton, ...) is the editor's rename of the same widget:
// the variable GUID, bindings, animation bindings, desired focus, navigation and graph references follow.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentWidgetBlueprintCaseRenameTest, "UEAssetDoc.WidgetBlueprint.CaseRename",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentWidgetBlueprintCaseRenameTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentWidgetBlueprintPythonTests;
	const FTarget Target(TEXT("WBP_CaseRename"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	if (!TestTrue(TEXT("create applies"), Apply(*this, Target, Document(Target, TEXT("Create"), InitialBody()), TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	UWidgetBlueprint* Blueprint = Target.Get();
	if (!TestNotNull(TEXT("Widget Blueprint exists"), Blueprint))
	{
		return false;
	}
	UWidget* Ok = FindWidget(Blueprint, TEXT("OkButton"));
	UWidget* Cancel = FindWidget(Blueprint, TEXT("CancelButton"));
	UWidget* Title = FindWidget(Blueprint, TEXT("Title"));
	const FGuid OkGuid = Blueprint->WidgetVariableNameToGuidMap.FindRef(TEXT("OkButton"));
	const FGuid TitleGuid = Blueprint->WidgetVariableNameToGuidMap.FindRef(TEXT("Title"));
	TestTrue(TEXT("widgets have variable GUIDs"), OkGuid.IsValid() && TitleGuid.IsValid());
	const UK2Node_ComponentBoundEvent* BoundBefore = nullptr;
	for (const UEdGraphNode* Node : Blueprint->UbergraphPages[0]->Nodes)
	{
		BoundBefore = BoundBefore != nullptr ? BoundBefore : Cast<UK2Node_ComponentBoundEvent>(Node);
	}
	const FGuid BoundGuid = BoundBefore != nullptr ? BoundBefore->NodeGuid : FGuid();
	TestTrue(TEXT("the bound event exists"), BoundGuid.IsValid());

	const FString Renamed = Document(Target, TEXT("Update"), InitialBody()
		.Replace(TEXT("OkButton"), TEXT("OKButton"), ESearchCase::CaseSensitive)
		.Replace(TEXT("\"CancelButton\""), TEXT("\"cancelButton\""), ESearchCase::CaseSensitive)
		.Replace(TEXT("\"Title\""), TEXT("\"TITLE\""), ESearchCase::CaseSensitive));
	{
		FAssetDocumentDiffRequest Request;
		Request.Document = Parse(Renamed);
		const FAssetDocumentResult Diff = FAssetDocumentModule::Get().GetService().Diff(Request);
		const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
		TestTrue(TEXT("Diff sees the case-only rename"), Diff.IsSuccess() && Diff.Payload.IsValid()
			&& Diff.Payload->TryGetArrayField(TEXT("changed"), Changed) && Changed->Num() > 0);
	}
	const FAssetDocumentResult Result = Apply(*this, Target, Renamed, TEXT("Rename"));
	if (!TestTrue(TEXT("rename applies"), Result.IsSuccess()))
	{
		return false;
	}
	Blueprint = Target.Get();
	TestTrue(TEXT("OkButton is the same widget with the new casing"),
		Blueprint->WidgetTree->FindWidget(TEXT("OKButton")) == Ok && Ok->GetName().Equals(TEXT("OKButton"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("CancelButton is the same widget with the new casing"),
		Blueprint->WidgetTree->FindWidget(TEXT("cancelButton")) == Cancel && Cancel->GetName().Equals(TEXT("cancelButton"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("Title is the same widget with the new casing"),
		Blueprint->WidgetTree->FindWidget(TEXT("TITLE")) == Title && Title->GetName().Equals(TEXT("TITLE"), ESearchCase::CaseSensitive));
	TestEqual(TEXT("OkButton keeps its variable GUID"), Blueprint->WidgetVariableNameToGuidMap.FindRef(TEXT("OKButton")), OkGuid);
	TestEqual(TEXT("Title keeps its variable GUID"), Blueprint->WidgetVariableNameToGuidMap.FindRef(TEXT("TITLE")), TitleGuid);
	TestTrue(TEXT("the binding follows the widget"), Blueprint->Bindings.Num() == 1
		&& Blueprint->Bindings[0].ObjectName.Equals(TEXT("TITLE"), ESearchCase::CaseSensitive));
	const UUserWidget* Defaults = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject<UUserWidget>() : nullptr;
	TestTrue(TEXT("desired focus follows the widget"), Defaults != nullptr
		&& Defaults->GetDesiredFocusWidgetName().ToString().Equals(TEXT("OKButton"), ESearchCase::CaseSensitive));
	const UButton* OkButton = Cast<UButton>(Ok);
	TestTrue(TEXT("navigation follows the widget"), OkButton != nullptr && OkButton->Navigation != nullptr
		&& OkButton->Navigation->Down.WidgetToFocus.ToString().Equals(TEXT("cancelButton"), ESearchCase::CaseSensitive));
	const UWidgetAnimation* Intro = FindAnimation(Blueprint, TEXT("Intro"));
	TestEqual(TEXT("the animation keeps its four tracks"), TrackCount(Intro), 4);
	bool bAnimationNamesFollow = Intro != nullptr;
	for (const FWidgetAnimationBinding& Binding : Intro != nullptr ? Intro->AnimationBindings : TArray<FWidgetAnimationBinding>())
	{
		const FString Name = Binding.WidgetName.ToString();
		bAnimationNamesFollow &= Name == TEXT("TITLE") || Name == TEXT("OKButton") || Name == TEXT("cancelButton");
		bAnimationNamesFollow &= !Name.Equals(TEXT("Title"), ESearchCase::CaseSensitive) && !Name.Equals(TEXT("OkButton"), ESearchCase::CaseSensitive)
			&& !Name.Equals(TEXT("CancelButton"), ESearchCase::CaseSensitive);
	}
	TestTrue(TEXT("animation bindings follow the widgets"), bAnimationNamesFollow);
	const UK2Node_ComponentBoundEvent* Bound = nullptr;
	for (const UEdGraphNode* Node : Blueprint->UbergraphPages[0]->Nodes)
	{
		Bound = Bound != nullptr ? Bound : Cast<UK2Node_ComponentBoundEvent>(Node);
	}
	TestTrue(TEXT("the bound event is the same node on the renamed widget"), Bound != nullptr && Bound->NodeGuid == BoundGuid
		&& Bound->ComponentPropertyName.ToString().Equals(TEXT("OKButton"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("the renamed Widget Blueprint compiles"), CompilesClean(Blueprint));

	const FString After = Serialize(Extract(*this, Target, TEXT("extract after")));
	for (const TCHAR* Name : {TEXT("\"Name\":\"OKButton\""), TEXT("\"Name\":\"cancelButton\""), TEXT("\"Name\":\"TITLE\"")})
	{
		TestTrue(*FString::Printf(TEXT("extraction contains %s"), Name), After.Contains(Name, ESearchCase::CaseSensitive));
	}
	for (const TCHAR* Name : {TEXT("\"Name\":\"OkButton\""), TEXT("\"Name\":\"CancelButton\""), TEXT("\"Name\":\"Title\"")})
	{
		TestFalse(*FString::Printf(TEXT("extraction no longer contains %s"), Name), After.Contains(Name, ESearchCase::CaseSensitive));
	}
	TestNoDiff(*this, Renamed, TEXT("after rename"));
	const FAssetDocumentResult Again = Apply(*this, Target, Renamed, TEXT("ReapplyAfterRename"));
	TestTrue(TEXT("re-apply is a no-op"), Again.IsSuccess() && Again.Message.Contains(TEXT("unchanged")));
	ExtractEquals(*this, Target, After, TEXT("after re-apply"));

	ReleaseUndoHistory();
	UPackageTools::UnloadPackages({Blueprint->GetOutermost()});
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	ExtractEquals(*this, Target, After, TEXT("after reload from disk"));

	// Back to the original casing: the same rename the other way.
	const FAssetDocumentResult Back = Apply(*this, Target, Document(Target, TEXT("Update"), InitialBody()), TEXT("RenameBack"));
	TestTrue(TEXT("rename back applies"), Back.IsSuccess());
	const FString RestoredText = Serialize(Extract(*this, Target, TEXT("extract after rename back")));
	for (const TCHAR* Name : {TEXT("\"Name\":\"OkButton\""), TEXT("\"Name\":\"CancelButton\""), TEXT("\"Name\":\"Title\"")})
	{
		TestTrue(*FString::Printf(TEXT("extraction contains %s again"), Name), RestoredText.Contains(Name, ESearchCase::CaseSensitive));
	}
	TestEqual(TEXT("OkButton keeps its variable GUID through both renames"),
		Target.Get()->WidgetVariableNameToGuidMap.FindRef(TEXT("OkButton")), OkGuid);
	return !HasAnyErrors();
}
// A widget renamed through RenamedFrom (OkButton -> ConfirmButton, CancelButton -> DismissButton, Title -> Heading)
// is the Designer's rename (FWidgetBlueprintEditorUtils::RenameWidget) of the same widget: variable GUID, property
// binding, animation bindings and possessables, desired focus, navigation, the bound event, a getter in the event
// graph, and a getter in another loaded Blueprint that uses the widget (left unsaved and reported, as in the editor)
// follow. A dependent with unsaved changes refuses the rename; a failed apply reloads the dependent from its file.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentWidgetBlueprintRenameTest, "UEAssetDoc.WidgetBlueprint.Rename",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentWidgetBlueprintRenameTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentWidgetBlueprintPythonTests;
	const FTarget Target(TEXT("WBP_Rename"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	// The dependent Actor Blueprint (a Menu variable of the widget class and a getter of Menu.<button>).
	const FString UserName = FString::Printf(TEXT("BP_RenameUser_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString UserPackageName = FString(Folder) / UserName;
	const FString UserObjectPath = UserPackageName + TEXT(".") + UserName;
	const FString UserFile = FPackageName::LongPackageNameToFilename(UserPackageName, FPackageName::GetAssetPackageExtension());
	auto User = [&UserObjectPath]() { return Cast<UBlueprint>(FSoftObjectPath(UserObjectPath).ResolveObject()); };
	ON_SCOPE_EXIT
	{
		ReleaseUndoHistory();
		if (UPackage* Package = FindPackage(nullptr, *UserPackageName))
		{
			// The last rename left it with unsaved changes; unload it anyway so its file can go.
			FText Error;
			UPackageTools::UnloadPackages({Package}, Error, true);
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		if (IFileManager::Get().FileExists(*UserFile))
		{
			IFileManager::Get().Delete(*UserFile, false, true);
			FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({UserFile});
		}
	};
	auto UserGetter = [&User]() -> UK2Node_VariableGet*
	{
		UBlueprint* Loaded = User();
		UEdGraph* Graph = Loaded != nullptr ? FBlueprintEditorUtils::FindEventGraph(Loaded) : nullptr;
		for (UEdGraphNode* Node : Graph != nullptr ? Graph->Nodes : TArray<TObjectPtr<UEdGraphNode>>())
		{
			UK2Node_VariableGet* Get = Cast<UK2Node_VariableGet>(Node);
			if (Get != nullptr && !Get->VariableReference.IsSelfContext())
			{
				return Get;
			}
		}
		return nullptr;
	};
	auto HasCode = [](const FAssetDocumentResult& Result, const TCHAR* Code)
	{
		return Result.Diagnostics.ContainsByPredicate([Code](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == Code; });
	};

	// A getter of OkButton in the event graph next to the bound OnClicked event.
	const FString Initial = InitialBody().Replace(TEXT("{\"Id\":\"Ping\",\"Action\":\"K2:CustomEvent\",\"Name\":\"Ping\"},"),
		TEXT("{\"Id\":\"Ping\",\"Action\":\"K2:CustomEvent\",\"Name\":\"Ping\"},{\"Id\":\"Read\",\"Action\":\"K2:Get:OkButton\"},"));
	if (!TestTrue(TEXT("create applies"), Apply(*this, Target, Document(Target, TEXT("Create"), Initial), TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	UWidgetBlueprint* Blueprint = Target.Get();
	if (!TestNotNull(TEXT("Widget Blueprint exists"), Blueprint))
	{
		return false;
	}
	{
		UBlueprint* Created = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), CreatePackage(*UserPackageName), FName(*UserName),
			BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		FAssetRegistryModule::AssetCreated(Created);
		FEdGraphPinType MenuType;
		MenuType.PinCategory = UEdGraphSchema_K2::PC_Object;
		MenuType.PinSubCategoryObject = Blueprint->GeneratedClass;
		FBlueprintEditorUtils::AddMemberVariable(Created, TEXT("Menu"), MenuType);
		UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Created);
		auto Spawn = [Graph](FName Member, UClass* Owner)
		{
			UK2Node_VariableGet* Node = NewObject<UK2Node_VariableGet>(Graph);
			if (Owner != nullptr)
			{
				Node->VariableReference.SetExternalMember(Member, Owner);
			}
			else
			{
				Node->VariableReference.SetSelfMember(Member);
			}
			Graph->AddNode(Node, false, false);
			Node->CreateNewGuid();
			Node->PostPlacedNewNode();
			Node->AllocateDefaultPins();
			return Node;
		};
		UK2Node_VariableGet* Menu = Spawn(TEXT("Menu"), nullptr);
		UK2Node_VariableGet* Ok = Spawn(TEXT("OkButton"), Blueprint->GeneratedClass);
		TestTrue(TEXT("Menu.OkButton getter is wired"), GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(
			Menu->FindPin(TEXT("Menu")), Ok->FindPin(UEdGraphSchema_K2::PN_Self)));
		FKismetEditorUtilities::CompileBlueprint(Created);
		TestTrue(TEXT("dependent compiles"), CompilesClean(Created));
		TestTrue(TEXT("dependent saves"), UEditorLoadingAndSavingUtils::SavePackages({Created->GetOutermost()}, false));
	}
	UWidget* Ok = FindWidget(Blueprint, TEXT("OkButton"));
	const FGuid OkGuid = Blueprint->WidgetVariableNameToGuidMap.FindRef(TEXT("OkButton"));
	const FGuid TitleGuid = Blueprint->WidgetVariableNameToGuidMap.FindRef(TEXT("Title"));
	FGuid BoundGuid;
	FGuid ReadGuid;
	for (const UEdGraphNode* Node : Blueprint->UbergraphPages[0]->Nodes)
	{
		BoundGuid = Node->IsA<UK2Node_ComponentBoundEvent>() ? Node->NodeGuid : BoundGuid;
		ReadGuid = Node->IsA<UK2Node_VariableGet>() ? Node->NodeGuid : ReadGuid;
	}
	TestTrue(TEXT("variable GUIDs, bound event and getter exist"), OkGuid.IsValid() && TitleGuid.IsValid() && BoundGuid.IsValid() && ReadGuid.IsValid());

	// The renamed document: every reference uses the new names, the widgets say where they come from.
	auto RenamedBody = [&Initial](const TCHAR* Extra)
	{
		return Initial.Replace(TEXT("OkButton"), TEXT("ConfirmButton"), ESearchCase::CaseSensitive)
			.Replace(TEXT("CancelButton"), TEXT("DismissButton"), ESearchCase::CaseSensitive)
			.Replace(TEXT("\"Title\""), TEXT("\"Heading\""), ESearchCase::CaseSensitive)
			.Replace(TEXT("\"Name\":\"ConfirmButton\""), TEXT("\"Name\":\"ConfirmButton\",\"RenamedFrom\":\"OkButton\""), ESearchCase::CaseSensitive)
			.Replace(TEXT("\"Name\":\"DismissButton\""), TEXT("\"Name\":\"DismissButton\",\"RenamedFrom\":\"CancelButton\""), ESearchCase::CaseSensitive)
			.Replace(TEXT("\"Name\":\"Heading\""), TEXT("\"Name\":\"Heading\",\"RenamedFrom\":\"Title\""), ESearchCase::CaseSensitive)
			+ Extra;
	};
	const FString Renamed = Document(Target, TEXT("Update"), RenamedBody(TEXT("")));
	const FAssetDocumentResult Result = Apply(*this, Target, Renamed, TEXT("Rename"));
	if (!TestTrue(TEXT("rename applies"), Result.IsSuccess()))
	{
		return false;
	}
	TestTrue(TEXT("the updated dependent is reported"), HasCode(Result, TEXT("Widget.DependentsUpdated")));
	Blueprint = Target.Get();
	TestTrue(TEXT("OkButton is the same widget under its new name"), FindWidget(Blueprint, TEXT("ConfirmButton")) == Ok && FindWidget(Blueprint, TEXT("OkButton")) == nullptr);
	TestEqual(TEXT("the variable GUID moves to the new name"), Blueprint->WidgetVariableNameToGuidMap.FindRef(TEXT("ConfirmButton")), OkGuid);
	TestEqual(TEXT("Title's variable GUID moves to Heading"), Blueprint->WidgetVariableNameToGuidMap.FindRef(TEXT("Heading")), TitleGuid);
	TestFalse(TEXT("the old names have no variable GUID"), Blueprint->WidgetVariableNameToGuidMap.Contains(TEXT("OkButton")) || Blueprint->WidgetVariableNameToGuidMap.Contains(TEXT("Title")));
	TestTrue(TEXT("the property binding follows"), Blueprint->Bindings.Num() == 1 && Blueprint->Bindings[0].ObjectName == TEXT("Heading"));
	const UUserWidget* Defaults = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject<UUserWidget>() : nullptr;
	TestTrue(TEXT("desired focus follows"), Defaults != nullptr && Defaults->GetDesiredFocusWidgetName() == TEXT("ConfirmButton"));
	const UButton* OkButton = Cast<UButton>(Ok);
	TestTrue(TEXT("navigation follows"), OkButton != nullptr && OkButton->Navigation != nullptr && OkButton->Navigation->Down.WidgetToFocus == TEXT("DismissButton"));
	const UWidgetAnimation* Intro = FindAnimation(Blueprint, TEXT("Intro"));
	TestEqual(TEXT("the animation keeps its four tracks"), TrackCount(Intro), 4);
	TSet<FString> Bound;
	bool bPossessablesFollow = Intro != nullptr;
	for (const FWidgetAnimationBinding& Binding : Intro != nullptr ? Intro->AnimationBindings : TArray<FWidgetAnimationBinding>())
	{
		Bound.Add(Binding.WidgetName.ToString());
		const FMovieScenePossessable* Possessable = Binding.SlotWidgetName == NAME_None ? Intro->MovieScene->FindPossessable(Binding.AnimationGuid) : nullptr;
		bPossessablesFollow &= Binding.SlotWidgetName != NAME_None || (Possessable != nullptr && Possessable->GetName() == Binding.WidgetName.ToString());
	}
	TestTrue(TEXT("animation bindings follow"), Bound.Contains(TEXT("ConfirmButton")) && Bound.Contains(TEXT("Heading")) && Bound.Contains(TEXT("DismissButton"))
		&& !Bound.Contains(TEXT("OkButton")) && !Bound.Contains(TEXT("Title")) && !Bound.Contains(TEXT("CancelButton")));
	TestTrue(TEXT("animation possessables follow"), bPossessablesFollow);
	bool bBoundFollows = false;
	bool bReadFollows = false;
	for (const UEdGraphNode* Node : Blueprint->UbergraphPages[0]->Nodes)
	{
		if (const UK2Node_ComponentBoundEvent* Event = Cast<UK2Node_ComponentBoundEvent>(Node))
		{
			bBoundFollows = Event->NodeGuid == BoundGuid && Event->ComponentPropertyName == TEXT("ConfirmButton");
		}
		if (const UK2Node_VariableGet* Get = Cast<UK2Node_VariableGet>(Node))
		{
			bReadFollows = Get->NodeGuid == ReadGuid && Get->GetVarName() == TEXT("ConfirmButton");
		}
	}
	TestTrue(TEXT("the bound event is the same node on the renamed widget"), bBoundFollows);
	TestTrue(TEXT("the getter is the same node reading the renamed widget"), bReadFollows);
	TestTrue(TEXT("the renamed Widget Blueprint compiles"), CompilesClean(Blueprint));
	const UK2Node_VariableGet* External = UserGetter();
	TestTrue(TEXT("the dependent's getter follows"), External != nullptr && External->GetVarName() == TEXT("ConfirmButton"));
	TestTrue(TEXT("the dependent is left unsaved (as in the editor)"), User() != nullptr && User()->GetOutermost()->IsDirty());
	const FString After = Serialize(Extract(*this, Target, TEXT("extract after rename")));
	TestTrue(TEXT("extraction has the renamed getter and bound event"), After.Contains(TEXT("K2:Get:ConfirmButton")) && After.Contains(TEXT("@ConfirmButton")));
	TestNoDiff(*this, Renamed, TEXT("after rename"));
	const FAssetDocumentResult Again = Apply(*this, Target, Renamed, TEXT("ReapplyAfterRename"));
	TestTrue(TEXT("re-apply (RenamedFrom ignored) is a no-op"), Again.IsSuccess() && Again.Message.Contains(TEXT("unchanged")));

	// Rename back while the dependent has unsaved changes: refused, nothing changes.
	auto BackBody = [&Initial](const TCHAR* Extra)
	{
		return Initial.Replace(TEXT("\"Name\":\"OkButton\""), TEXT("\"Name\":\"OkButton\",\"RenamedFrom\":\"ConfirmButton\""), ESearchCase::CaseSensitive)
			.Replace(TEXT("\"Name\":\"CancelButton\""), TEXT("\"Name\":\"CancelButton\",\"RenamedFrom\":\"DismissButton\""), ESearchCase::CaseSensitive)
			.Replace(TEXT("\"Name\":\"Title\""), TEXT("\"Name\":\"Title\",\"RenamedFrom\":\"Heading\""), ESearchCase::CaseSensitive)
			+ Extra;
	};
	const FAssetDocumentResult Refused = Apply(*this, Target, Document(Target, TEXT("Update"), BackBody(TEXT(""))), TEXT("RefusedRename"));
	TestFalse(TEXT("rename with an unsaved dependent fails"), Refused.IsSuccess());
	TestTrue(TEXT("refusal names the unsaved dependent"), HasCode(Refused, TEXT("Widget.DependentHasUnsavedChanges")));
	ExtractEquals(*this, Target, After, TEXT("after refused rename"));

	// Saved dependent, then a rename back that fails later in the apply: the dependent is restored in memory
	// from its snapshot (the same objects: a reload from disk would replace them under the editor).
	TestTrue(TEXT("dependent saves"), User() != nullptr && UEditorLoadingAndSavingUtils::SavePackages({User()->GetOutermost()}, false));
	const TWeakObjectPtr<const UK2Node_VariableGet> GetterBefore = UserGetter();
	const FString Failing = Document(Target, TEXT("Update"), BackBody(TEXT(""))
		.Replace(TEXT("\"Function\":\"GetTitle\""), TEXT("\"Function\":\"Missing\""), ESearchCase::CaseSensitive));
	const FAssetDocumentResult Failed = Apply(*this, Target, Failing, TEXT("FailedRename"));
	TestFalse(TEXT("rename followed by a failure fails"), Failed.IsSuccess());
	TestTrue(TEXT("the failure is the binding"), HasCode(Failed, TEXT("Widget.BindingFunctionNotFound")));
	if (!HasCode(Failed, TEXT("Graph.DependentsRestored")))
	{
		// The failure can come before the editor updated the dependent: then it only gets its clean flag back.
		AddInfo(TEXT("rollback reported no Graph.DependentsRestored diagnostic"));
	}
	TestFalse(TEXT("no dependent is left unrestored"), HasCode(Failed, TEXT("Graph.DependentNotRestored")));
	ExtractEquals(*this, Target, After, TEXT("after failed rename"));
	TestTrue(TEXT("rolled-back Widget Blueprint compiles"), CompilesClean(Target.Get()));
	External = UserGetter();
	TestTrue(TEXT("the dependent is restored"), External != nullptr && External->GetVarName() == TEXT("ConfirmButton")
		&& !User()->GetOutermost()->IsDirty());
	TestTrue(TEXT("the dependent's getter is the same node (restored in place, not reloaded)"), GetterBefore.IsValid() && External == GetterBefore.Get());

	// The rename back with a saved dependent: applies, and the saved Widget Blueprint reloads to the same document.
	const FString Back = Document(Target, TEXT("Update"), BackBody(TEXT("")));
	TestTrue(TEXT("rename back applies"), Apply(*this, Target, Back, TEXT("RenameBack")).IsSuccess());
	External = UserGetter();
	TestTrue(TEXT("the dependent's getter follows back"), External != nullptr && External->GetVarName() == TEXT("OkButton"));
	TestEqual(TEXT("OkButton keeps its variable GUID through both renames"), Target.Get()->WidgetVariableNameToGuidMap.FindRef(TEXT("OkButton")), OkGuid);
	TestNoDiff(*this, Back, TEXT("after rename back"));
	const FString BackText = Serialize(Extract(*this, Target, TEXT("extract after rename back")));
	ReleaseUndoHistory();
	UPackageTools::UnloadPackages({Target.Get()->GetOutermost()});
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	ExtractEquals(*this, Target, BackText, TEXT("after reload from disk"));
	return !HasAnyErrors();
}
#endif
