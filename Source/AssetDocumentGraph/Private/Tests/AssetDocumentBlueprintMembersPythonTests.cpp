// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Event dispatchers and timelines through the production Service and the Python Profiles.
//
// UEAssetDoc.UBlueprint.EventDispatchers / .Timelines /
// WidgetBlueprint.EventDispatchers / AnimBlueprint.EventDispatchers: one apply creates and
// populates the asset (dispatchers with signatures plus Call / Bind / Unbind / Unbind all /
// Assign / dispatcher-event nodes; timelines with float / vector / linear color / event tracks,
// keys and an external curve, and their nodes); Diff and Extract agree with the document;
// re-applying the extraction changes nothing (no compile, no save, same file); a second apply
// edits and removes; failures in the middle of Apply and at compile roll back; the saved
// package reloads to the same document.
//
// UEAssetDoc.UBlueprint.MemberUndo: a human edit + save, then an AI apply;
// Ctrl+Z afterwards must not corrupt the Blueprint (AssetDocumentUndoHistory.h).
//
// UEAssetDoc.UBlueprint.MemberRawEdits checks the editor-equivalent raw edits
// (Blueprint/AssetDocumentBlueprintMemberRawEdits.h) against what the editor's actions produce.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "Blueprint/AssetDocumentBlueprintMemberRawEdits.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/CurveVector.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/TimelineTemplate.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/ComponentDelegateBinding.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_Timeline.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Runtime/AssetDocumentUndoHistory.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentBlueprintMembersPythonTests
{
const TCHAR* Folder = TEXT("/Game/AssetDocumentBlueprintMembers");
const TCHAR* BlueprintClass = TEXT("/Script/Engine.Blueprint");
const TCHAR* WidgetBlueprintClass = TEXT("/Script/UMGEditor.WidgetBlueprint");
const TCHAR* AnimBlueprintClass = TEXT("/Script/Engine.AnimBlueprint");

// Blueprint edits open transactions; clear the undo buffer before force-deleting fixtures.
void ReleaseUndoHistory()
{
	if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting && !GEditor->IsTransactionActive()
		&& GEditor->Trans->GetQueueLength() > 0)
	{
		GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentBlueprintMembersPythonTests", "Cleanup", "AssetDocument Blueprint members test cleanup"));
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
		SidecarPath = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("AssetDocumentBlueprintMembers"), AssetName + TEXT(".assetdoc.json"));
	}

	UObject* GetObject() const { return FSoftObjectPath(ObjectPath).ResolveObject(); }
	UBlueprint* Get() const { return Cast<UBlueprint>(GetObject()); }

	// Unloads (saved) or deletes (unsaved) the asset, then removes its files.
	void Cleanup() const
	{
		ReleaseUndoHistory();
		if (UObject* Asset = GetObject())
		{
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

bool HasDiagnostic(const FAssetDocumentResult& Result, const TCHAR* Code)
{
	return Result.Diagnostics.ContainsByPredicate([Code](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == Code; });
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

bool SavePackage(UObject* Asset)
{
	UPackage* Package = Asset->GetOutermost();
	const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	return UPackage::SavePackage(Package, Asset, *File, Args);
}

FString Document(const FTarget& Target, const TCHAR* Class, const TCHAR* Action, const FString& Body)
{
	return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"%s\",\"Action\":\"%s\",\"Body\":{%s}}"),
		*Target.PackageName, Class, Action, *Body);
}

struct FLifecycle
{
	FString Created;
	FString Edited;
	FString MidApplyFailure;
	FString CompileFailure;
};

// Create -> no-op re-apply of the extraction -> edit -> mid-apply / compile failure roll back ->
// reload from disk. Checks run after create and after edit. Returns false when create failed.
bool RunLifecycle(FAutomationTestBase& Test, const FTarget& Target, const FLifecycle& Docs,
	TFunctionRef<void(UBlueprint&)> CheckCreated, TFunctionRef<void(UBlueprint&)> CheckEdited)
{
	// 1. One apply creates and populates the asset.
	if (!Test.TestTrue(TEXT("create applies"), Apply(Test, Target, Docs.Created, TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	UBlueprint* Blueprint = Target.Get();
	if (!Test.TestNotNull(TEXT("asset exists"), Blueprint))
	{
		return false;
	}
	Test.TestTrue(TEXT("created asset compiles"), CompilesClean(Blueprint));
	CheckCreated(*Blueprint);
	TestNoDiff(Test, Docs.Created, TEXT("after create"));

	// 2. Re-applying the extraction changes nothing: no compile, no dirty package, no save.
	const TSharedPtr<FJsonObject> First = Extract(Test, Target, TEXT("first extract"));
	if (!First.IsValid())
	{
		return false;
	}
	First->SetStringField(TEXT("Action"), TEXT("Update"));
	const FString Extracted = Serialize(First);
	Test.AddInfo(FString::Printf(TEXT("extracted: %s"), *Extracted));
	Test.TestFalse(TEXT("created package is saved clean"), Blueprint->GetOutermost()->IsDirty());
	const FDateTime SavedStamp = IFileManager::Get().GetTimeStamp(*Target.PackageFile);
	int32 Compiles = 0;
	const FDelegateHandle CompiledHandle = Blueprint->OnCompiled().AddLambda([&Compiles](UBlueprint*) { ++Compiles; });
	const FAssetDocumentResult Reapplied = Apply(Test, Target, Extracted, TEXT("Reapply"));
	Blueprint->OnCompiled().Remove(CompiledHandle);
	Test.TestTrue(TEXT("extraction re-applies"), Reapplied.IsSuccess());
	Test.TestEqual(TEXT("no-op re-apply does not recompile"), Compiles, 0);
	Test.TestFalse(TEXT("no-op re-apply leaves the package clean"), Blueprint->GetOutermost()->IsDirty());
	Test.TestFalse(TEXT("no-op re-apply reports no save"), Reapplied.bSavedAsset);
	Test.TestEqual(TEXT("no-op re-apply does not rewrite the package file"), IFileManager::Get().GetTimeStamp(*Target.PackageFile), SavedStamp);
	First->SetStringField(TEXT("Action"), TEXT("CreateOrUpdate"));
	ExtractEquals(Test, Target, Serialize(First), TEXT("after re-apply"));
	Test.TestTrue(TEXT("same asset object"), Target.Get() == Blueprint);

	// 3. A second apply edits and removes.
	if (Test.TestTrue(TEXT("edit applies"), Apply(Test, Target, Docs.Edited, TEXT("Edit")).IsSuccess()))
	{
		TestNoDiff(Test, Docs.Edited, TEXT("after edit"));
		Blueprint = Target.Get();
		Test.TestTrue(TEXT("edited asset compiles"), CompilesClean(Blueprint));
		if (Blueprint != nullptr)
		{
			CheckEdited(*Blueprint);
		}
	}
	const FString EditedBaseline = Serialize(Extract(Test, Target, TEXT("edited extract")));

	// 4. A failure in the middle of Apply (members already changed) rolls back.
	const FAssetDocumentResult MidApply = Apply(Test, Target, Docs.MidApplyFailure, TEXT("MidApplyFailure"));
	Test.TestFalse(TEXT("mid-apply failure fails"), MidApply.IsSuccess());
	Blueprint = Target.Get();
	Test.TestTrue(TEXT("rolled-back asset compiles"), CompilesClean(Blueprint));
	Test.TestTrue(TEXT("rolled-back package is clean"), Blueprint != nullptr && !Blueprint->GetOutermost()->IsDirty());
	ExtractEquals(Test, Target, EditedBaseline, TEXT("after mid-apply rollback"));

	// 5. A failure at compile rolls back.
	const FAssetDocumentResult CompileFailed = Apply(Test, Target, Docs.CompileFailure, TEXT("CompileFailure"));
	Test.TestFalse(TEXT("compile failure fails"), CompileFailed.IsSuccess());
	Test.TestTrue(TEXT("failure is the compiler's"), HasDiagnostic(CompileFailed, TEXT("Graph.FinalizeFailed")));
	Blueprint = Target.Get();
	Test.TestTrue(TEXT("asset compiles after compile-failure rollback"), CompilesClean(Blueprint));
	ExtractEquals(Test, Target, EditedBaseline, TEXT("after compile rollback"));

	// 6. The saved package, unloaded and loaded back from disk, extracts to the same document.
	if (Blueprint != nullptr)
	{
		Test.TestFalse(TEXT("package is clean before unload"), Blueprint->GetOutermost()->IsDirty());
		ReleaseUndoHistory();
		UPackageTools::UnloadPackages({Blueprint->GetOutermost()});
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		Test.TestNull(TEXT("asset is unloaded"), Target.GetObject());
		ExtractEquals(Test, Target, EditedBaseline, TEXT("after reload from disk"));
		Test.TestTrue(TEXT("reloaded asset compiles"), CompilesClean(Target.Get()));
	}
	return true;
}

UEdGraph* FindGraph(const TArray<TObjectPtr<UEdGraph>>& Graphs, const TCHAR* Name)
{
	for (UEdGraph* Graph : Graphs)
	{
		if (Graph != nullptr && Graph->GetName() == Name)
		{
			return Graph;
		}
	}
	return nullptr;
}

FMulticastDelegateProperty* FindDispatcher(const UBlueprint& Blueprint, const TCHAR* Name)
{
	return Blueprint.GeneratedClass ? FindFProperty<FMulticastDelegateProperty>(Blueprint.GeneratedClass, Name) : nullptr;
}

TArray<FString> SignatureParams(const FMulticastDelegateProperty* Property)
{
	TArray<FString> Out;
	if (Property != nullptr && Property->SignatureFunction != nullptr)
	{
		for (TFieldIterator<FProperty> It(Property->SignatureFunction); It && (It->PropertyFlags & CPF_Parm); ++It)
		{
			Out.Add(It->GetName());
		}
	}
	return Out;
}

TArray<FString> CustomEventNames(UBlueprint& Blueprint)
{
	TArray<UK2Node_CustomEvent*> Events;
	FBlueprintEditorUtils::GetAllNodesOfClass(&Blueprint, Events);
	TArray<FString> Out;
	for (const UK2Node_CustomEvent* Event : Events)
	{
		Out.Add(Event->CustomFunctionName.ToString());
	}
	Out.Sort();
	return Out;
}

TArray<FString> DisplayTrackNames(UTimelineTemplate* Timeline)
{
	TArray<FString> Out;
	for (int32 Index = 0; Timeline != nullptr && Index < Timeline->GetNumDisplayTracks(); ++Index)
	{
		const FTTTrackId Id = Timeline->GetDisplayTrackId(Index);
		switch (static_cast<FTTTrackBase::ETrackType>(Id.TrackType))
		{
		case FTTTrackBase::TT_Event: Out.Add(Timeline->EventTracks[Id.TrackIndex].GetTrackName().ToString()); break;
		case FTTTrackBase::TT_FloatInterp: Out.Add(Timeline->FloatTracks[Id.TrackIndex].GetTrackName().ToString()); break;
		case FTTTrackBase::TT_VectorInterp: Out.Add(Timeline->VectorTracks[Id.TrackIndex].GetTrackName().ToString()); break;
		case FTTTrackBase::TT_LinearColorInterp: Out.Add(Timeline->LinearColorTracks[Id.TrackIndex].GetTrackName().ToString()); break;
		}
	}
	return Out;
}

FString Join(const TArray<FString>& Values)
{
	return FString::Join(Values, TEXT(","));
}

// A saved linear color curve asset an external timeline track uses.
UCurveLinearColor* CreateCurveAsset(const FTarget& Target)
{
	UPackage* Package = CreatePackage(*Target.PackageName);
	UCurveLinearColor* Curve = NewObject<UCurveLinearColor>(Package, FName(*Target.AssetName), RF_Public | RF_Standalone);
	Curve->FloatCurves[0].AddKey(0.f, 1.f);
	Curve->FloatCurves[0].AddKey(1.f, 0.f);
	FAssetRegistryModule::AssetCreated(Curve);
	return SavePackage(Curve) ? Curve : nullptr;
}

// ---- Event dispatcher documents (UBlueprint) --------------------------------

const TCHAR* DispatcherGraph = TEXT(
	"\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":["
	"{\"Id\":\"Begin\",\"Action\":\"K2:BeginPlay\"},"
	"{\"Id\":\"Handler\",\"Action\":\"K2:CustomEvent:OnDied\"},"
	"{\"Id\":\"Bind\",\"Action\":\"K2:AddDelegate:OnDied\"},"
	"{\"Id\":\"Assign\",\"Action\":\"K2:AssignDelegate:OnOpened\"},"
	"{\"Id\":\"Opened\",\"Action\":\"K2:CustomEvent\",\"Name\":\"OnOpened_Event\"},"
	"{\"Id\":\"Call\",\"Action\":\"K2:CallDelegate:OnDied\",\"PinOverrides\":[{\"Pin\":\"Amount\",\"DefaultValue\":\"5\"}]},"
	"{\"Id\":\"Unbind\",\"Action\":\"K2:RemoveDelegate:OnDied\"},"
	"{\"Id\":\"Clear\",\"Action\":\"K2:ClearDelegate:OnOpened\"},"
	"{\"Id\":\"Say\",\"Action\":\"K2:PrintString\"}],"
	"\"Links\":["
	"{\"From\":\"Begin:then\",\"To\":\"Bind:execute\"},"
	"{\"From\":\"Handler:OutputDelegate\",\"To\":\"Bind:Delegate\"},"
	"{\"From\":\"Bind:then\",\"To\":\"Assign:execute\"},"
	"{\"From\":\"Opened:OutputDelegate\",\"To\":\"Assign:Delegate\"},"
	"{\"From\":\"Assign:then\",\"To\":\"Call:execute\"},"
	"{\"From\":\"Call:then\",\"To\":\"Unbind:execute\"},"
	"{\"From\":\"Handler:OutputDelegate\",\"To\":\"Unbind:Delegate\"},"
	"{\"From\":\"Unbind:then\",\"To\":\"Clear:execute\"},"
	"{\"From\":\"Handler:then\",\"To\":\"Say:execute\"}]}]");

const TCHAR* DispatchersCreated = TEXT(
	"\"ParentClass\":\"/Script/Engine.Actor\","
	"\"Variables\":[{\"Name\":\"Health\",\"Type\":\"float\",\"DefaultValue\":100}],"
	"\"EventDispatchers\":["
	"{\"Name\":\"OnDied\",\"Inputs\":[{\"Name\":\"Killer\",\"Type\":\"object:/Script/Engine.Actor\"},{\"Name\":\"Amount\",\"Type\":\"float\"}],"
	"\"Category\":\"Combat\",\"Tooltip\":\"Broadcast when health reaches zero.\"},"
	"{\"Name\":\"OnOpened\"}],");

const TCHAR* DispatchersEdited = TEXT(
	"\"EventDispatchers\":["
	"{\"Name\":\"OnDied\",\"Inputs\":[{\"Name\":\"Killer\",\"Type\":\"object:/Script/Engine.Actor\"},{\"Name\":\"bHeadshot\",\"Type\":\"bool\"}],"
	"\"Category\":\"Combat\"},"
	"{\"Name\":\"OnClosed\",\"Inputs\":[{\"Name\":\"Speed\",\"Type\":\"int\"}]}],"
	"\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":["
	"{\"Id\":\"Begin\",\"Action\":\"K2:BeginPlay\"},"
	"{\"Id\":\"Handler\",\"Action\":\"K2:CustomEvent:OnDied\"},"
	"{\"Id\":\"Bind\",\"Action\":\"K2:AddDelegate:OnDied\"},"
	"{\"Id\":\"Call\",\"Action\":\"K2:CallDelegate:OnDied\",\"PinOverrides\":[{\"Pin\":\"bHeadshot\",\"DefaultValue\":\"true\"}]},"
	"{\"Id\":\"Close\",\"Action\":\"K2:CallDelegate:OnClosed\",\"PinOverrides\":[{\"Pin\":\"Speed\",\"DefaultValue\":\"3\"}]},"
	"{\"Id\":\"Say\",\"Action\":\"K2:PrintString\"}],"
	"\"Links\":["
	"{\"From\":\"Begin:then\",\"To\":\"Bind:execute\"},"
	"{\"From\":\"Handler:OutputDelegate\",\"To\":\"Bind:Delegate\"},"
	"{\"From\":\"Bind:then\",\"To\":\"Call:execute\"},"
	"{\"From\":\"Call:then\",\"To\":\"Close:execute\"},"
	"{\"From\":\"Handler:then\",\"To\":\"Say:execute\"}]}]");

// Dispatchers change, then an unknown node action fails the graph stage.
const TCHAR* DispatchersMidApply = TEXT(
	"\"EventDispatchers\":[{\"Name\":\"OnBroken\"}],"
	"\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":[{\"Id\":\"X\",\"Action\":\"K2:NoSuchAction\"}]}]");

// A ForEachLoop whose Array type is undetermined is an editor compiler error.
FString CompileFailureBody(const TCHAR* Members, const TCHAR* EventNode, const TCHAR* EventPin)
{
	return FString::Printf(TEXT(
		"%s\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":[%s,"
		"{\"Id\":\"Loop\",\"Action\":\"K2:ForEachLoop\"}],"
		"\"Links\":[{\"From\":\"%s\",\"To\":\"Loop:Exec\"}]}]"), Members, EventNode, EventPin);
}

// ---- Timeline documents (UBlueprint) -----------------------------------------

FString TimelinesCreated(const FString& CurvePath)
{
	return FString::Printf(TEXT(
		"\"ParentClass\":\"/Script/Engine.Actor\","
		"\"Timelines\":["
		"{\"Name\":\"Fade\",\"Length\":2.0,\"AutoPlay\":true,\"Loop\":true,\"TickGroup\":\"PostPhysics\",\"Tracks\":["
		"{\"Name\":\"Alpha\",\"Type\":\"Float\",\"Keys\":[{\"Time\":0,\"Value\":0},{\"Time\":1,\"Value\":0.5,\"InterpMode\":\"Cubic\"},{\"Time\":2,\"Value\":1}]},"
		"{\"Name\":\"Offset\",\"Type\":\"Vector\",\"Keys\":{\"Z\":[{\"Time\":0},{\"Time\":2,\"Value\":50,\"InterpMode\":\"Constant\"}]}},"
		"{\"Name\":\"Tint\",\"Type\":\"LinearColor\",\"Curve\":\"%s\"},"
		"{\"Name\":\"Halfway\",\"Type\":\"Event\",\"Keys\":[{\"Time\":1}]}]},"
		"{\"Name\":\"Spin\",\"UseLastKeyframe\":true,\"Replicated\":true,\"IgnoreTimeDilation\":true}],"
		"\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":["
		"{\"Id\":\"Begin\",\"Action\":\"K2:BeginPlay\"},"
		"{\"Id\":\"Fade\",\"Action\":\"K2:Timeline\",\"Name\":\"Fade\",\"Position\":{\"X\":400,\"Y\":0}},"
		"{\"Id\":\"Spin\",\"Action\":\"K2:Timeline\",\"Name\":\"Spin\",\"Position\":{\"X\":800,\"Y\":0}},"
		"{\"Id\":\"Say\",\"Action\":\"K2:PrintString\"}],"
		"\"Links\":["
		"{\"From\":\"Begin:then\",\"To\":\"Fade:PlayFromStart\"},"
		"{\"From\":\"Fade:Halfway\",\"To\":\"Say:execute\"},"
		"{\"From\":\"Fade:Finished\",\"To\":\"Spin:Play\"}]}]"), *CurvePath);
}

const TCHAR* TimelinesEdited = TEXT(
	"\"Timelines\":["
	"{\"Name\":\"Fade\",\"Length\":3.0,\"AutoPlay\":true,\"Tracks\":["
	"{\"Name\":\"Halfway\",\"Type\":\"Event\",\"Keys\":[{\"Time\":1.5}]},"
	"{\"Name\":\"Alpha\",\"Type\":\"Float\",\"Keys\":[{\"Time\":0,\"Value\":1},"
	"{\"Time\":1,\"Value\":0.25,\"InterpMode\":\"Cubic\",\"TangentMode\":\"User\",\"ArriveTangent\":-0.5,\"LeaveTangent\":-0.5},{\"Time\":3,\"Value\":0}]},"
	"{\"Name\":\"Tint\",\"Type\":\"LinearColor\",\"Keys\":{\"R\":[{\"Time\":0,\"Value\":1}],\"A\":[{\"Time\":0,\"Value\":1}]}}]},"
	"{\"Name\":\"Bob\",\"Loop\":true,\"Tracks\":[{\"Name\":\"Height\",\"Type\":\"Float\",\"Keys\":[{\"Time\":0},{\"Time\":5,\"Value\":20}]}]}],"
	"\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":["
	"{\"Id\":\"Begin\",\"Action\":\"K2:BeginPlay\"},"
	"{\"Id\":\"Fade\",\"Action\":\"K2:Timeline\",\"Name\":\"Fade\",\"Position\":{\"X\":400,\"Y\":0}},"
	"{\"Id\":\"Bob\",\"Action\":\"K2:Timeline\",\"Name\":\"Bob\",\"Position\":{\"X\":800,\"Y\":200}},"
	"{\"Id\":\"Say\",\"Action\":\"K2:PrintString\"}],"
	"\"Links\":["
	"{\"From\":\"Begin:then\",\"To\":\"Fade:Play\"},"
	"{\"From\":\"Fade:Halfway\",\"To\":\"Say:execute\"},"
	"{\"From\":\"Fade:Finished\",\"To\":\"Bob:Reverse\"}]}]");

const TCHAR* TimelinesMidApply = TEXT(
	"\"Timelines\":[{\"Name\":\"Fade\",\"Length\":9.0},{\"Name\":\"Broken\"}],"
	"\"Components\":[{\"Name\":\"Broken\",\"Class\":\"/Script/Engine.Actor\"}]");

UTimelineTemplate* FindTimeline(UBlueprint& Blueprint, const TCHAR* Name)
{
	return Blueprint.FindTimelineTemplateByVariableName(FName(Name));
}
} // namespace AssetDocumentBlueprintMembersPythonTests


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUBlueprintEventDispatchersTest, "UEAssetDoc.UBlueprint.EventDispatchers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintEventDispatchersTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentBlueprintMembersPythonTests;
	const FTarget Target(TEXT("BP_Dispatchers"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	FLifecycle Docs;
	Docs.Created = Document(Target, BlueprintClass, TEXT("Create"), FString(DispatchersCreated) + DispatcherGraph);
	Docs.Edited = Document(Target, BlueprintClass, TEXT("Update"), DispatchersEdited);
	Docs.MidApplyFailure = Document(Target, BlueprintClass, TEXT("Update"), DispatchersMidApply);
	Docs.CompileFailure = Document(Target, BlueprintClass, TEXT("Update"), CompileFailureBody(
		TEXT("\"EventDispatchers\":[{\"Name\":\"OnTemp\"}],"), TEXT("{\"Id\":\"Begin\",\"Action\":\"K2:BeginPlay\"}"), TEXT("Begin:then")));
	RunLifecycle(*this, Target, Docs,
		[this](UBlueprint& Blueprint)
		{
			TestEqual(TEXT("two signature graphs"), Blueprint.DelegateSignatureGraphs.Num(), 2);
			TestEqual(TEXT("variable plus two dispatcher members"), Blueprint.NewVariables.Num(), 3);
			const FMulticastDelegateProperty* OnDied = FindDispatcher(Blueprint, TEXT("OnDied"));
			TestNotNull(TEXT("OnDied is a multicast delegate of the class"), OnDied);
			TestEqual(TEXT("OnDied signature"), Join(SignatureParams(OnDied)), FString(TEXT("Killer,Amount")));
			TestEqual(TEXT("OnDied category"), FBlueprintEditorUtils::GetBlueprintVariableCategory(&Blueprint, TEXT("OnDied"), nullptr).ToString(),
				FString(TEXT("Combat")));
			const UEdGraph* Signature = FindGraph(Blueprint.DelegateSignatureGraphs, TEXT("OnDied"));
			TestTrue(TEXT("signature graph is not editable as a graph"), Signature != nullptr && !Signature->bEditable);
			// The Assign node's own event was adopted for "Opened": no duplicate custom event.
			TestEqual(TEXT("custom events"), Join(CustomEventNames(Blueprint)), FString(TEXT("OnDied_Event,OnOpened_Event")));
		},
		[this](UBlueprint& Blueprint)
		{
			TestEqual(TEXT("OnOpened removed with its signature graph"), Blueprint.DelegateSignatureGraphs.Num(), 2);
			TestNull(TEXT("OnOpened member is gone"), FindDispatcher(Blueprint, TEXT("OnOpened")));
			TestTrue(TEXT("OnOpened signature graph is gone"), FindGraph(Blueprint.DelegateSignatureGraphs, TEXT("OnOpened")) == nullptr);
			TestEqual(TEXT("OnDied signature edited"), Join(SignatureParams(FindDispatcher(Blueprint, TEXT("OnDied")))),
				FString(TEXT("Killer,bHeadshot")));
			TestEqual(TEXT("OnClosed signature"), Join(SignatureParams(FindDispatcher(Blueprint, TEXT("OnClosed")))), FString(TEXT("Speed")));
			TestEqual(TEXT("Health variable kept (Variables omitted)"), Blueprint.NewVariables.Num(), 3);
			TestEqual(TEXT("custom events after edit"), Join(CustomEventNames(Blueprint)), FString(TEXT("OnDied_Event")));
		});

	// A dispatcher cannot take a variable's name; Interface Blueprints have no dispatchers.
	const FAssetDocumentResult Clash = Apply(*this, Target, Document(Target, BlueprintClass, TEXT("Update"),
		TEXT("\"Variables\":[{\"Name\":\"Health\",\"Type\":\"float\"}],\"EventDispatchers\":[{\"Name\":\"Health\"}]")), TEXT("Clash"));
	TestTrue(TEXT("variable / dispatcher name clash is rejected"), !Clash.IsSuccess() && HasDiagnostic(Clash, TEXT("UBlueprint.DuplicateName")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUBlueprintTimelinesTest, "UEAssetDoc.UBlueprint.Timelines",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintTimelinesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentBlueprintMembersPythonTests;
	const FTarget CurveTarget(TEXT("C_Tint"));
	const FTarget Target(TEXT("BP_Timelines"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
		CurveTarget.Cleanup();
	};
	UCurveLinearColor* Curve = CreateCurveAsset(CurveTarget);
	if (!TestNotNull(TEXT("fixture curve is created and saved"), Curve))
	{
		return false;
	}
	FLifecycle Docs;
	Docs.Created = Document(Target, BlueprintClass, TEXT("Create"), TimelinesCreated(Curve->GetPathName()));
	Docs.Edited = Document(Target, BlueprintClass, TEXT("Update"), TimelinesEdited);
	Docs.MidApplyFailure = Document(Target, BlueprintClass, TEXT("Update"), TimelinesMidApply);
	// Timelines omitted: the graph alone removes both timeline nodes (and so their timelines).
	Docs.CompileFailure = Document(Target, BlueprintClass, TEXT("Update"), CompileFailureBody(
		TEXT(""), TEXT("{\"Id\":\"Begin\",\"Action\":\"K2:BeginPlay\"}"), TEXT("Begin:then")));
	RunLifecycle(*this, Target, Docs,
		[this, Curve](UBlueprint& Blueprint)
		{
			TestEqual(TEXT("two timelines"), Blueprint.Timelines.Num(), 2);
			UTimelineTemplate* Fade = FindTimeline(Blueprint, TEXT("Fade"));
			if (!TestNotNull(TEXT("Fade exists"), Fade))
			{
				return;
			}
			TestEqual(TEXT("Fade length"), Fade->TimelineLength, 2.f);
			TestTrue(TEXT("Fade autoplays and loops"), Fade->bAutoPlay && Fade->bLoop && !Fade->bReplicated);
			TestEqual(TEXT("Fade tick group"), static_cast<int32>(Fade->TimelineTickGroup.GetValue()), static_cast<int32>(TG_PostPhysics));
			TestEqual(TEXT("Fade tracks in display order"), Join(DisplayTrackNames(Fade)), FString(TEXT("Alpha,Offset,Tint,Halfway")));
			TestTrue(TEXT("Tint uses the external curve"), Fade->LinearColorTracks.Num() == 1
				&& Fade->LinearColorTracks[0].CurveLinearColor == Curve && Fade->LinearColorTracks[0].bIsExternalCurve);
			TestTrue(TEXT("Halfway is an event curve with one key"), Fade->EventTracks.Num() == 1 && Fade->EventTracks[0].CurveKeys
				&& Fade->EventTracks[0].CurveKeys->bIsEventCurve && Fade->EventTracks[0].CurveKeys->FloatCurve.GetNumKeys() == 1);
			TestTrue(TEXT("Alpha holds three keys"), Fade->FloatTracks.Num() == 1 && Fade->FloatTracks[0].CurveFloat
				&& Fade->FloatTracks[0].CurveFloat->FloatCurve.GetNumKeys() == 3);
			UK2Node_Timeline* Node = FBlueprintEditorUtils::FindNodeForTimeline(&Blueprint, Fade);
			TestTrue(TEXT("Fade node placed by the document"), Node != nullptr && Node->NodePosX == 400 && Node->bAutoPlay && Node->bLoop);
			TestTrue(TEXT("Fade node has track pins"), Node != nullptr && Node->FindPin(TEXT("Alpha")) && Node->FindPin(TEXT("Halfway")));
			UTimelineTemplate* Spin = FindTimeline(Blueprint, TEXT("Spin"));
			TestTrue(TEXT("Spin settings"), Spin != nullptr && Spin->LengthMode == TL_LastKeyFrame && Spin->bReplicated && Spin->bIgnoreTimeDilation);
		},
		[this](UBlueprint& Blueprint)
		{
			TestEqual(TEXT("two timelines after edit"), Blueprint.Timelines.Num(), 2);
			TestNull(TEXT("Spin removed"), FindTimeline(Blueprint, TEXT("Spin")));
			UTimelineTemplate* Fade = FindTimeline(Blueprint, TEXT("Fade"));
			UTimelineTemplate* Bob = FindTimeline(Blueprint, TEXT("Bob"));
			TestTrue(TEXT("Bob created with its node"), Bob != nullptr && Bob->bLoop && FBlueprintEditorUtils::FindNodeForTimeline(&Blueprint, Bob));
			if (Fade != nullptr)
			{
				TestEqual(TEXT("Fade length edited"), Fade->TimelineLength, 3.f);
				TestFalse(TEXT("Fade no longer loops"), Fade->bLoop);
				TestEqual(TEXT("Fade tracks reordered and removed"), Join(DisplayTrackNames(Fade)), FString(TEXT("Halfway,Alpha,Tint")));
				TestTrue(TEXT("Tint switched to an internal curve"), Fade->LinearColorTracks.Num() == 1
					&& !Fade->LinearColorTracks[0].bIsExternalCurve && Fade->LinearColorTracks[0].CurveLinearColor
					&& Fade->LinearColorTracks[0].CurveLinearColor->GetOuter() == Blueprint.GeneratedClass);
				const FRichCurve* Alpha = Fade->FloatTracks.Num() == 1 && Fade->FloatTracks[0].CurveFloat
					? &Fade->FloatTracks[0].CurveFloat->FloatCurve : nullptr;
				TestTrue(TEXT("Alpha user tangent kept"), Alpha != nullptr && Alpha->GetNumKeys() == 3
					&& FMath::IsNearlyEqual(Alpha->GetConstRefOfKeys()[1].LeaveTangent, -0.5f));
			}
		});

	// Timeline names are member names; nodes and the Timelines region must agree.
	const FAssetDocumentResult Mismatch = Apply(*this, Target, Document(Target, BlueprintClass, TEXT("Update"), TEXT(
		"\"Timelines\":[{\"Name\":\"Other\"}],"
		"\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":[{\"Id\":\"Fade\",\"Action\":\"K2:Timeline\",\"Name\":\"Fade\"}]}]")),
		TEXT("Mismatch"));
	TestTrue(TEXT("timeline / node mismatch is rejected"), !Mismatch.IsSuccess() && HasDiagnostic(Mismatch, TEXT("UBlueprint.TimelineNodeMismatch")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUBlueprintMemberUndoTest, "UEAssetDoc.UBlueprint.MemberUndo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintMemberUndoTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentBlueprintMembersPythonTests;
	const FTarget Target(TEXT("BP_MemberUndo"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	auto Body = [](double Length, const TCHAR* Inputs, const TCHAR* Extra)
	{
		return FString::Printf(TEXT(
			"\"ParentClass\":\"/Script/Engine.Actor\",\"Variables\":[],"
			"\"EventDispatchers\":[{\"Name\":\"OnPing\",\"Inputs\":[%s]}],"
			"\"Timelines\":[{\"Name\":\"Fade\",\"Length\":%g,\"Tracks\":[{\"Name\":\"Alpha\",\"Type\":\"Float\",\"Keys\":[{\"Time\":0},{\"Time\":1,\"Value\":1}]}]}]%s"),
			Inputs, Length, Extra);
	};
	if (GEditor == nullptr || GEditor->Trans == nullptr
		|| !TestTrue(TEXT("create applies"), Apply(*this, Target, Document(Target, BlueprintClass, TEXT("Create"), Body(2.0, TEXT(""), TEXT(""))),
			TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	ReleaseUndoHistory();
	auto Live = [&Target]() { return Target.Get(); };
	auto Length = [&Live]() { UTimelineTemplate* Fade = Live() ? FindTimeline(*Live(), TEXT("Fade")) : nullptr; return Fade ? Fade->TimelineLength : -1.f; };
	auto UndoRedoAll = [this, &Live](const TCHAR* Stage)
	{
		int32 Undone = 0;
		while (Undone < 16 && GEditor->Trans->CanUndo() && GEditor->UndoTransaction())
		{
			++Undone;
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			UBlueprint* Blueprint = Live();
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
	};

	// 1. The human adds a variable and lengthens the timeline in one transaction, and saves.
	{
		UBlueprint* Blueprint = Live();
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentBlueprintMembersPythonTests", "UserEdit", "Edit Members"));
		Blueprint->Modify();
		FEdGraphPinType IntType;
		IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
		FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("HumanNote"), IntType);
		UTimelineTemplate* Fade = FindTimeline(*Blueprint, TEXT("Fade"));
		Fade->Modify();
		Fade->TimelineLength = 4.f;
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}
	TestTrue(TEXT("human save"), UEditorLoadingAndSavingUtils::SavePackages({Live()->GetOutermost()}, false));
	TestTrue(TEXT("the human edit is in the undo history"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));

	// 2. The AI rewrites the members (HumanNote is not in the document): history referencing it is cleared.
	const FString Edited = Document(Target, BlueprintClass, TEXT("Update"), Body(6.0, TEXT("{\"Name\":\"Count\",\"Type\":\"int\"}"), TEXT("")));
	if (!TestTrue(TEXT("AI edit applies"), Apply(*this, Target, Edited, TEXT("Edit")).IsSuccess()))
	{
		return false;
	}
	TestEqual(TEXT("AI edit removed HumanNote"), Live()->NewVariables.ContainsByPredicate(
		[](const FBPVariableDescription& Variable) { return Variable.VarName == TEXT("HumanNote"); }), false);
	TestFalse(TEXT("after AI edit: no undo history references the asset"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	const FString AfterEdit = Serialize(Extract(*this, Target, TEXT("after AI edit")));
	UndoRedoAll(TEXT("after AI edit"));
	ExtractEquals(*this, Target, AfterEdit, TEXT("after AI edit + undo/redo"));

	// 3. The human lengthens the timeline again, then an AI apply fails mid-way and rolls back: the
	//    human's step stays undoable and undoes onto the restored (functionally equal) objects.
	ReleaseUndoHistory();
	{
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentBlueprintMembersPythonTests", "UserLength", "Timeline Length"));
		UTimelineTemplate* Fade = FindTimeline(*Live(), TEXT("Fade"));
		Fade->Modify();
		Fade->TimelineLength = 7.f;
		FBlueprintEditorUtils::MarkBlueprintAsModified(Live());
	}
	const int32 HumanSteps = GEditor->Trans->GetQueueLength();
	const FString MidApply = Document(Target, BlueprintClass, TEXT("Update"), Body(8.0, TEXT(""),
		TEXT(",\"Components\":[{\"Name\":\"Broken\",\"Class\":\"/Script/Engine.Actor\"}]")));
	TestFalse(TEXT("mid-apply failure fails"), Apply(*this, Target, MidApply, TEXT("MidApplyFailure")).IsSuccess());
	TestEqual(TEXT("after rollback: the human history is kept"), GEditor->Trans->GetQueueLength(), HumanSteps);
	TestEqual(TEXT("after rollback: the human length is kept"), Length(), 7.f);
	TestTrue(TEXT("Ctrl+Z undoes the human length"), GEditor->UndoTransaction());
	TestEqual(TEXT("after undo: the AI length is back"), Length(), 6.f);
	FKismetEditorUtilities::CompileBlueprint(Live());
	TestTrue(TEXT("after undo: compiles"), CompilesClean(Live()));
	ExtractEquals(*this, Target, AfterEdit, TEXT("after rollback + undo"));
	TestTrue(TEXT("Ctrl+Y redoes the human length"), GEditor->RedoTransaction());
	TestEqual(TEXT("after redo: the human length again"), Length(), 7.f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUBlueprintMemberRawEditsTest, "UEAssetDoc.UBlueprint.MemberRawEdits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// BlueprintRawEdits against the editor: "+ Event Dispatcher" / delete, the timeline menu action /
// node delete, and the Timeline editor's settings, tracks and key edits.
bool FAssetDocumentUBlueprintMemberRawEditsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentBlueprintMembersPythonTests;
	namespace Raw = UE::AssetDocument::Graph::BlueprintRawEdits;
	const FTarget Target(TEXT("BP_Raw"));
	const FTarget LibraryTarget(TEXT("BFL_Raw"));
	const FTarget ObjectTarget(TEXT("BP_RawObject"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
		LibraryTarget.Cleanup();
		ObjectTarget.Cleanup();
	};
	auto Create = [](const FTarget& At, UClass* Parent, EBlueprintType Type)
	{
		UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(Parent, CreatePackage(*At.PackageName), FName(*At.AssetName), Type,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		if (Blueprint != nullptr)
		{
			FAssetRegistryModule::AssetCreated(Blueprint);
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
		}
		return Blueprint;
	};
	UBlueprint* Blueprint = Create(Target, AActor::StaticClass(), BPTYPE_Normal);
	UBlueprint* Library = Create(LibraryTarget, UBlueprintFunctionLibrary::StaticClass(), BPTYPE_FunctionLibrary);
	UBlueprint* ObjectBlueprint = Create(ObjectTarget, UObject::StaticClass(), BPTYPE_Normal);
	if (!TestNotNull(TEXT("actor Blueprint"), Blueprint) || !TestNotNull(TEXT("library"), Library) || !TestNotNull(TEXT("object Blueprint"), ObjectBlueprint))
	{
		return false;
	}

	// Event dispatchers: FBlueprintEditor::OnAddNewDelegate / SMyBlueprint::OnDeleteDelegate.
	TestTrue(TEXT("actor Blueprints have dispatchers"), Raw::SupportsEventDispatchers(*Blueprint));
	TestFalse(TEXT("function libraries have none"), Raw::SupportsEventDispatchers(*Library));
	UEdGraph* Signature = Raw::AddEventDispatcher(*Blueprint, TEXT("OnPing"));
	if (!TestNotNull(TEXT("dispatcher added"), Signature))
	{
		return false;
	}
	const FBPVariableDescription* Member = Blueprint->NewVariables.FindByPredicate(
		[](const FBPVariableDescription& Variable) { return Variable.VarName == TEXT("OnPing"); });
	TestTrue(TEXT("member variable is a multicast delegate"), Member != nullptr && Member->VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate);
	TestTrue(TEXT("signature graph registered and not editable"), Blueprint->DelegateSignatureGraphs.Contains(Signature) && !Signature->bEditable);
	TArray<UK2Node_FunctionEntry*> Entries;
	Signature->GetNodesOfClass(Entries);
	TestTrue(TEXT("editable entry with editor flags"), Entries.Num() == 1 && Entries[0]->IsEditable()
		&& (Entries[0]->GetFunctionFlags() & (FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public))
			== (FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public));
	TestNull(TEXT("a taken name adds nothing"), Raw::AddEventDispatcher(*Blueprint, TEXT("OnPing")));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	TestTrue(TEXT("compiles with the dispatcher"), CompilesClean(Blueprint) && FindDispatcher(*Blueprint, TEXT("OnPing")) != nullptr);
	Raw::RemoveEventDispatcher(*Blueprint, *Signature);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	TestTrue(TEXT("member and signature graph removed together"), Blueprint->DelegateSignatureGraphs.Num() == 0
		&& !Blueprint->NewVariables.ContainsByPredicate([](const FBPVariableDescription& Variable) { return Variable.VarName == TEXT("OnPing"); })
		&& FindDispatcher(*Blueprint, TEXT("OnPing")) == nullptr);

	// Timelines: the K2Node_Timeline menu action, then the Timeline editor.
	UEdGraph* EventGraph = Blueprint->UbergraphPages.Num() > 0 ? Blueprint->UbergraphPages[0].Get() : nullptr;
	if (!TestNotNull(TEXT("event graph"), EventGraph))
	{
		return false;
	}
	TestNull(TEXT("no timelines outside actor Blueprints"), ObjectBlueprint->UbergraphPages.Num() > 0
		? Raw::AddTimeline(*ObjectBlueprint, *ObjectBlueprint->UbergraphPages[0], TEXT("Nope"), FVector2D::ZeroVector) : nullptr);
	UK2Node_Timeline* Node = Raw::AddTimeline(*Blueprint, *EventGraph, TEXT("Fade"), FVector2D(120, 240));
	UTimelineTemplate* Timeline = Blueprint->FindTimelineTemplateByVariableName(TEXT("Fade"));
	if (!TestNotNull(TEXT("timeline node"), Node) || !TestNotNull(TEXT("timeline template"), Timeline))
	{
		return false;
	}
	TestTrue(TEXT("node placed and named"), Node->NodePosX == 120 && Node->NodePosY == 240 && Node->TimelineName == TEXT("Fade")
		&& FBlueprintEditorUtils::FindNodeForTimeline(Blueprint, Timeline) == Node);
	TestTrue(TEXT("node has the fixed pins"), Node->GetPlayPin() != nullptr && Node->GetFinishedPin() != nullptr);
	Raw::SetTimelineFlag(*Blueprint, *Timeline, Raw::ETimelineFlag::AutoPlay, true);
	Raw::SetTimelineFlag(*Blueprint, *Timeline, Raw::ETimelineFlag::Loop, true);
	Raw::SetTimelineFlag(*Blueprint, *Timeline, Raw::ETimelineFlag::Replicated, true);
	Raw::SetTimelineFlag(*Blueprint, *Timeline, Raw::ETimelineFlag::IgnoreTimeDilation, true);
	TestTrue(TEXT("flags on the template and the node cache"), Timeline->bAutoPlay && Timeline->bLoop && Timeline->bReplicated
		&& Timeline->bIgnoreTimeDilation && Node->bAutoPlay && Node->bLoop && Node->bReplicated && Node->bIgnoreTimeDilation);
	Raw::SetTimelineLength(*Blueprint, *Timeline, 3.f);
	Raw::SetTimelineUseLastKeyframe(*Blueprint, *Timeline, true);
	Raw::SetTimelineTickGroup(*Blueprint, *Timeline, TG_PostUpdateWork);
	TestTrue(TEXT("length, length mode and tick group"), Timeline->TimelineLength == 3.f && Timeline->LengthMode == TL_LastKeyFrame
		&& Timeline->TimelineTickGroup == TG_PostUpdateWork);

	UCurveFloat* External = NewObject<UCurveFloat>(GetTransientPackage());
	TestEqual(TEXT("float track"), Raw::AddTimelineTrack(*Blueprint, *Timeline, FTTTrackBase::TT_FloatInterp, TEXT("Alpha"), nullptr), 0);
	TestEqual(TEXT("vector track"), Raw::AddTimelineTrack(*Blueprint, *Timeline, FTTTrackBase::TT_VectorInterp, TEXT("Offset"), nullptr), 1);
	TestEqual(TEXT("color track"), Raw::AddTimelineTrack(*Blueprint, *Timeline, FTTTrackBase::TT_LinearColorInterp, TEXT("Tint"), nullptr), 2);
	TestEqual(TEXT("event track"), Raw::AddTimelineTrack(*Blueprint, *Timeline, FTTTrackBase::TT_Event, TEXT("Hit"), nullptr), 3);
	TestEqual(TEXT("external float track"), Raw::AddTimelineTrack(*Blueprint, *Timeline, FTTTrackBase::TT_FloatInterp, TEXT("Ext"), External), 4);
	TestEqual(TEXT("a taken track name adds nothing"), Raw::AddTimelineTrack(*Blueprint, *Timeline, FTTTrackBase::TT_FloatInterp, TEXT("Alpha"), nullptr),
		static_cast<int32>(INDEX_NONE));
	bool bExternal = false;
	UCurveBase* AlphaCurve = Raw::GetTrackCurve(*Timeline, 0, &bExternal);
	TestTrue(TEXT("internal curves belong to the generated class"), AlphaCurve != nullptr && !bExternal && AlphaCurve->GetOuter() == Blueprint->GeneratedClass);
	UCurveFloat* EventCurve = Cast<UCurveFloat>(Raw::GetTrackCurve(*Timeline, 3));
	TestTrue(TEXT("event curves are marked"), EventCurve != nullptr && EventCurve->bIsEventCurve);
	TestTrue(TEXT("external curve kept and flagged"), Raw::GetTrackCurve(*Timeline, 4, &bExternal) == External && bExternal);
	Raw::MoveTimelineTrack(*Timeline, 1, -1);
	TestEqual(TEXT("move swaps display order"), Join(DisplayTrackNames(Timeline)), FString(TEXT("Offset,Alpha,Tint,Hit,Ext")));
	Raw::RemoveTimelineTrack(*Timeline, 4);
	TestTrue(TEXT("remove drops the display entry and the track"), Join(DisplayTrackNames(Timeline)) == TEXT("Offset,Alpha,Tint,Hit")
		&& Timeline->FloatTracks.Num() == 1);
	// Internal <-> external switch (STimelineEdTrack).
	UCurveFloat* Internal = Cast<UCurveFloat>(Raw::GetTrackCurve(*Timeline, 1));
	Raw::SetTrackCurve(*Timeline, 1, *External, true);
	TestTrue(TEXT("switched to external"), Raw::GetTrackCurve(*Timeline, 1, &bExternal) == External && bExternal);
	Raw::SetTrackCurve(*Timeline, 1, *Internal, false);
	TestTrue(TEXT("switched back to internal"), Raw::GetTrackCurve(*Timeline, 1, &bExternal) == Internal && !bExternal);

	// Key edits recompute auto tangents like the curve editor.
	TArray<FRichCurveKey> Keys;
	Keys.Add(FRichCurveKey(0.f, 0.f));
	Keys.Add(FRichCurveKey(1.f, 1.f));
	Keys.Add(FRichCurveKey(2.f, 0.f));
	Keys[1].InterpMode = RCIM_Cubic;
	Keys[1].ArriveTangent = Keys[1].LeaveTangent = 42.f;
	Keys[0].LeaveTangent = 3.f;
	Raw::SetCurveKeys(*Internal, Internal->FloatCurve, Keys);
	FRichCurve Expected;
	Expected.SetKeys(Keys);
	Expected.AutoSetTangents();
	const TArray<FRichCurveKey>& Have = Internal->FloatCurve.GetConstRefOfKeys();
	TestTrue(TEXT("keys set with recomputed tangents"), Have.Num() == 3 && Have[1].ArriveTangent == Expected.GetConstRefOfKeys()[1].ArriveTangent
		&& Have[1].ArriveTangent != 42.f && Have[0].LeaveTangent == 0.f);

	Raw::RefreshTimelineNode(*Blueprint, *Timeline);
	TestTrue(TEXT("node shows the tracks"), Node->FindPin(TEXT("Alpha")) && Node->FindPin(TEXT("Offset")) && Node->FindPin(TEXT("Hit")));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	TestTrue(TEXT("compiles with the timeline"), CompilesClean(Blueprint));

	// Deleting the node removes the timeline (UK2Node_Timeline::DestroyNode).
	FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
	TestNull(TEXT("timeline removed with its node"), Blueprint->FindTimelineTemplateByVariableName(TEXT("Fade")));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	TestTrue(TEXT("compiles without the timeline"), CompilesClean(Blueprint));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentWidgetBlueprintEventDispatchersTest, "UEAssetDoc.WidgetBlueprint.EventDispatchers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentWidgetBlueprintEventDispatchersTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentBlueprintMembersPythonTests;
	const FTarget Target(TEXT("WBP_Dispatchers"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	const TCHAR* Event = TEXT("{\"Id\":\"Pick\",\"Action\":\"K2:CustomEvent\",\"Name\":\"Pick\"}");
	FLifecycle Docs;
	Docs.Created = Document(Target, WidgetBlueprintClass, TEXT("Create"), TEXT(
		"\"ParentClass\":\"/Script/UMG.UserWidget\","
		"\"EventDispatchers\":[{\"Name\":\"OnPicked\",\"Inputs\":[{\"Name\":\"Index\",\"Type\":\"int\"}],\"Tooltip\":\"An entry was picked.\"},"
		"{\"Name\":\"OnClosed\"}],"
		"\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":["
		"{\"Id\":\"Pick\",\"Action\":\"K2:CustomEvent\",\"Name\":\"Pick\"},"
		"{\"Id\":\"Call\",\"Action\":\"K2:CallDelegate:OnPicked\",\"PinOverrides\":[{\"Pin\":\"Index\",\"DefaultValue\":\"2\"}]},"
		"{\"Id\":\"Closed\",\"Action\":\"K2:CustomEvent:OnClosed\"},"
		"{\"Id\":\"Bind\",\"Action\":\"K2:AddDelegate:OnClosed\"}],"
		"\"Links\":[{\"From\":\"Pick:then\",\"To\":\"Call:execute\"},{\"From\":\"Call:then\",\"To\":\"Bind:execute\"},"
		"{\"From\":\"Closed:OutputDelegate\",\"To\":\"Bind:Delegate\"}]}]"));
	Docs.Edited = Document(Target, WidgetBlueprintClass, TEXT("Update"), TEXT(
		"\"EventDispatchers\":[{\"Name\":\"OnPicked\",\"Inputs\":[{\"Name\":\"Index\",\"Type\":\"int\"},{\"Name\":\"Label\",\"Type\":\"string\"}],"
		"\"Category\":\"List\"}],"
		"\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":["
		"{\"Id\":\"Pick\",\"Action\":\"K2:CustomEvent\",\"Name\":\"Pick\"},"
		"{\"Id\":\"Call\",\"Action\":\"K2:CallDelegate:OnPicked\",\"PinOverrides\":[{\"Pin\":\"Label\",\"DefaultValue\":\"first\"}]}],"
		"\"Links\":[{\"From\":\"Pick:then\",\"To\":\"Call:execute\"}]}]"));
	Docs.MidApplyFailure = Document(Target, WidgetBlueprintClass, TEXT("Update"), DispatchersMidApply);
	Docs.CompileFailure = Document(Target, WidgetBlueprintClass, TEXT("Update"), CompileFailureBody(
		TEXT("\"EventDispatchers\":[{\"Name\":\"OnTemp\"}],"), Event, TEXT("Pick:then")));
	RunLifecycle(*this, Target, Docs,
		[this](UBlueprint& Blueprint)
		{
			TestEqual(TEXT("two dispatchers"), Blueprint.DelegateSignatureGraphs.Num(), 2);
			TestEqual(TEXT("OnPicked signature"), Join(SignatureParams(FindDispatcher(Blueprint, TEXT("OnPicked")))), FString(TEXT("Index")));
		},
		[this](UBlueprint& Blueprint)
		{
			TestEqual(TEXT("OnClosed removed"), Blueprint.DelegateSignatureGraphs.Num(), 1);
			TestEqual(TEXT("OnPicked signature edited"), Join(SignatureParams(FindDispatcher(Blueprint, TEXT("OnPicked")))), FString(TEXT("Index,Label")));
		});
	const FAssetDocumentResult Timelines = Apply(*this, Target, Document(Target, WidgetBlueprintClass, TEXT("Update"),
		TEXT("\"Timelines\":[{\"Name\":\"Fade\"}]")), TEXT("Timelines"));
	TestTrue(TEXT("timelines are rejected in Widget Blueprints"), !Timelines.IsSuccess() && HasDiagnostic(Timelines, TEXT("Widget.UnsupportedRegion")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimBlueprintEventDispatchersTest, "UEAssetDoc.AnimBlueprint.EventDispatchers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimBlueprintEventDispatchersTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentBlueprintMembersPythonTests;
	const FTarget Target(TEXT("ABP_Dispatchers"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	const TCHAR* Update = TEXT("{\"Id\":\"Update\",\"Action\":\"K2:Event:/Script/Engine.AnimInstance:BlueprintUpdateAnimation\"}");
	FLifecycle Docs;
	// A template Animation Blueprint needs no skeleton fixture.
	Docs.Created = Document(Target, AnimBlueprintClass, TEXT("Create"), TEXT(
		"\"ParentClass\":\"/Script/Engine.AnimInstance\",\"Template\":true,"
		"\"EventDispatchers\":[{\"Name\":\"OnLanded\",\"Inputs\":[{\"Name\":\"Speed\",\"Type\":\"float\"}]},{\"Name\":\"OnJumped\"}],"
		"\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":["
		"{\"Id\":\"Update\",\"Action\":\"K2:Event:/Script/Engine.AnimInstance:BlueprintUpdateAnimation\"},"
		"{\"Id\":\"Call\",\"Action\":\"K2:CallDelegate:OnLanded\",\"PinOverrides\":[{\"Pin\":\"Speed\",\"DefaultValue\":\"1.5\"}]}],"
		"\"Links\":[{\"From\":\"Update:then\",\"To\":\"Call:execute\"}]}]"));
	// The legacy key, in its graph shape, edits the same region.
	Docs.Edited = Document(Target, AnimBlueprintClass, TEXT("Update"), TEXT(
		"\"DelegateSignatureGraphs\":[{\"Name\":\"OnLanded\",\"Signature\":{\"Inputs\":[{\"Name\":\"Speed\",\"Type\":\"float\"},"
		"{\"Name\":\"bHard\",\"Type\":\"bool\"}]},\"Nodes\":[{\"Id\":\"Entry\",\"Implicit\":\"entry\"}],\"Links\":[]}],"
		"\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":["
		"{\"Id\":\"Update\",\"Action\":\"K2:Event:/Script/Engine.AnimInstance:BlueprintUpdateAnimation\"},"
		"{\"Id\":\"Call\",\"Action\":\"K2:CallDelegate:OnLanded\",\"PinOverrides\":[{\"Pin\":\"bHard\",\"DefaultValue\":\"true\"}]}],"
		"\"Links\":[{\"From\":\"Update:then\",\"To\":\"Call:execute\"}]}]"));
	Docs.MidApplyFailure = Document(Target, AnimBlueprintClass, TEXT("Update"), DispatchersMidApply);
	Docs.CompileFailure = Document(Target, AnimBlueprintClass, TEXT("Update"), CompileFailureBody(
		TEXT("\"EventDispatchers\":[{\"Name\":\"OnTemp\"}],"), Update, TEXT("Update:then")));
	RunLifecycle(*this, Target, Docs,
		[this](UBlueprint& Blueprint)
		{
			TestEqual(TEXT("two dispatchers"), Blueprint.DelegateSignatureGraphs.Num(), 2);
			TestEqual(TEXT("OnLanded signature"), Join(SignatureParams(FindDispatcher(Blueprint, TEXT("OnLanded")))), FString(TEXT("Speed")));
		},
		[this](UBlueprint& Blueprint)
		{
			TestEqual(TEXT("OnJumped removed"), Blueprint.DelegateSignatureGraphs.Num(), 1);
			TestEqual(TEXT("OnLanded signature edited"), Join(SignatureParams(FindDispatcher(Blueprint, TEXT("OnLanded")))), FString(TEXT("Speed,bHard")));
		});
	const FAssetDocumentResult Timelines = Apply(*this, Target, Document(Target, AnimBlueprintClass, TEXT("Update"),
		TEXT("\"Timelines\":[{\"Name\":\"Fade\"}]")), TEXT("Timelines"));
	TestTrue(TEXT("timelines are rejected in Animation Blueprints"), !Timelines.IsSuccess() && HasDiagnostic(Timelines, TEXT("AnimBlueprint.UnsupportedRegion")));
	return !HasAnyErrors();
}

// Removing a dispatcher input that nodes are wired to (editor: UK2Node::RewireOldPinsToNewPins keeps the pin
// as an orphan; UK2Node::ValidateOrphanPin makes a linked orphan a compile error and a valued unlinked one a
// warning; UEdGraphNode::PinConnectionListChanged drops an unlinked orphan).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUBlueprintOrphanPinsTest, "UEAssetDoc.UBlueprint.OrphanPins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace AssetDocumentBlueprintMembersPythonTests
{
FString OrphanGraph(bool bAmountLink, int32 Call2Mode)
{
	FString Nodes = TEXT("{\"Id\":\"Begin\",\"Action\":\"K2:BeginPlay\"},{\"Id\":\"Health\",\"Action\":\"K2:Get:Health\"},{\"Id\":\"Call\",\"Action\":\"K2:CallDelegate:OnDied\"}");
	FString Links = TEXT("{\"From\":\"Begin:then\",\"To\":\"Call:execute\"}");
	if (bAmountLink)
	{
		Links += TEXT(",{\"From\":\"Health:Health\",\"To\":\"Call:Amount\"}");
	}
	if (Call2Mode > 0)
	{
		Nodes += Call2Mode == 2
			? TEXT(",{\"Id\":\"Call2\",\"Action\":\"K2:CallDelegate:OnDied\",\"PinOverrides\":[{\"Pin\":\"Amount\",\"DefaultValue\":\"5\"}]}")
			: TEXT(",{\"Id\":\"Call2\",\"Action\":\"K2:CallDelegate:OnDied\"}");
		Links += TEXT(",{\"From\":\"Call:then\",\"To\":\"Call2:execute\"}");
	}
	return FString::Printf(TEXT("\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":[%s],\"Links\":[%s]}]"), *Nodes, *Links);
}

TArray<FString> OrphanedPins(const UBlueprint* Blueprint)
{
	TArray<FString> Out;
	for (const UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin->bOrphanedPin)
				{
					Out.Add(Node->GetNodeTitle(ENodeTitleType::ListView).ToString() + TEXT(":") + Pin->PinName.ToString());
				}
			}
		}
	}
	return Out;
}

const FAssetDocumentDiagnostic* FindDiagnostic(const FAssetDocumentResult& Result, const TCHAR* Code)
{
	return Result.Diagnostics.FindByPredicate([Code](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == Code; });
}

void ReloadAndExtractEquals(FAutomationTestBase& Test, const FTarget& Target, const FString& Expected, const TCHAR* Stage)
{
	if (UBlueprint* Blueprint = Target.Get())
	{
		ReleaseUndoHistory();
		UPackageTools::UnloadPackages({Blueprint->GetOutermost()});
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		Test.TestNull(*FString::Printf(TEXT("%s: unloaded"), Stage), Target.GetObject());
	}
	ExtractEquals(Test, Target, Expected, Stage);
}

// Re-applying an extraction: succeeds, saves nothing, and Diff agrees.
void ReapplyIsNoOp(FAutomationTestBase& Test, const FTarget& Target, const TCHAR* Stage)
{
	const TSharedPtr<FJsonObject> Extracted = Extract(Test, Target, Stage);
	if (!Extracted.IsValid())
	{
		return;
	}
	Extracted->SetStringField(TEXT("Action"), TEXT("Update"));
	const FAssetDocumentResult Reapplied = Apply(Test, Target, Serialize(Extracted), Stage);
	Test.TestTrue(*FString::Printf(TEXT("%s: extraction re-applies"), Stage), Reapplied.IsSuccess());
	Test.TestFalse(*FString::Printf(TEXT("%s: re-apply is a no-op"), Stage), Reapplied.bSavedAsset);
	Test.TestEqual(*FString::Printf(TEXT("%s: re-apply reports no diagnostics"), Stage), Reapplied.Diagnostics.Num(), 0);
	TestNoDiff(Test, Serialize(Extracted), Stage);
}
} // namespace AssetDocumentBlueprintMembersPythonTests

bool FAssetDocumentUBlueprintOrphanPinsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentBlueprintMembersPythonTests;
	const FTarget Target(TEXT("BP_Orphans"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	const FString Members = TEXT("\"ParentClass\":\"/Script/Engine.Actor\",\"Variables\":[{\"Name\":\"Health\",\"Type\":\"float\"}],");
	const FString TwoInputs = TEXT("\"EventDispatchers\":[{\"Name\":\"OnDied\",\"Inputs\":[{\"Name\":\"Killer\",\"Type\":\"object:/Script/Engine.Actor\"},{\"Name\":\"Amount\",\"Type\":\"float\"}]}],");
	const FString OneInput = TEXT("\"EventDispatchers\":[{\"Name\":\"OnDied\",\"Inputs\":[{\"Name\":\"Killer\",\"Type\":\"object:/Script/Engine.Actor\"}]}],");
	auto Update = [&](const FString& Body, const TCHAR* Stage) { return Apply(*this, Target, Document(Target, BlueprintClass, TEXT("Update"), Body), Stage); };
	if (!TestTrue(TEXT("create"), Apply(*this, Target, Document(Target, BlueprintClass, TEXT("Create"), Members + TwoInputs + OrphanGraph(true, 0)), TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	const FString Baseline = Serialize(Extract(*this, Target, TEXT("baseline")));

	// (a) Removing the input while a link still ends on it is what the editor refuses to compile: the apply
	// fails with a diagnostic on the Call node naming the pin, and rolls back.
	const FAssetDocumentResult Keep = Update(Members + OneInput + OrphanGraph(true, 0), TEXT("RemoveInputKeepLink"));
	TestFalse(TEXT("input removal with a link left is refused"), Keep.IsSuccess());
	const FAssetDocumentDiagnostic* Linked = FindDiagnostic(Keep, TEXT("Graph.OrphanedPinLinked"));
	if (TestNotNull(TEXT("refusal is Graph.OrphanedPinLinked"), Linked))
	{
		TestTrue(TEXT("refusal points at the Call node"), Linked->Path == TEXT("/Body/UbergraphPages/0/Nodes/2"));
		TestTrue(TEXT("refusal names the pin"), Linked->Message.Contains(TEXT("'Amount'")));
		TestTrue(TEXT("refusal names the node"), Linked->Message.Contains(TEXT("'Call'")));
		TestTrue(TEXT("Result.Message leads with it"), Keep.Message.StartsWith(TEXT("Graph.OrphanedPinLinked:")));
	}
	ExtractEquals(*this, Target, Baseline, TEXT("after refused removal"));
	TestTrue(TEXT("rolled back asset compiles"), CompilesClean(Target.Get()));
	// The same refusal when the document leaves the graphs out (kept as they are): the node is still named, by the
	// Id the document gave it (extract keeps authored Ids).
	const FAssetDocumentResult KeepOmitted = Update(Members + OneInput + TEXT("\"Components\":[]"), TEXT("RemoveInputGraphOmitted"));
	TestFalse(TEXT("refused when the graph is omitted"), KeepOmitted.IsSuccess());
	const FAssetDocumentDiagnostic* Omitted = FindDiagnostic(KeepOmitted, TEXT("Graph.OrphanedPinLinked"));
	if (TestNotNull(TEXT("omitted-graph refusal is Graph.OrphanedPinLinked"), Omitted))
	{
		TestTrue(TEXT("omitted-graph refusal points at a graph node"), Omitted->Path.StartsWith(TEXT("/Body/UbergraphPages/0/Nodes/")));
		TestTrue(TEXT("omitted-graph refusal names the pin and the node id"), Omitted->Message.Contains(TEXT("'Amount'")) && Omitted->Message.Contains(TEXT("'Call'")));
	}
	ExtractEquals(*this, Target, Baseline, TEXT("after refused removal (graph omitted)"));

	// (c) Removing the input and the links to it in one apply leaves no orphan pin and no diagnostics.
	const FAssetDocumentResult Both = Update(Members + OneInput + OrphanGraph(false, 0), TEXT("RemoveInputAndLink"));
	TestTrue(TEXT("input and link removed together"), Both.IsSuccess());
	TestEqual(TEXT("no diagnostics"), Both.Diagnostics.Num(), 0);
	TestEqual(TEXT("no orphan pins"), OrphanedPins(Target.Get()).Num(), 0);
	TestTrue(TEXT("compiles clean"), CompilesClean(Target.Get()) && Target.Get()->Status == BS_UpToDate);
	const FString Removed = Serialize(Extract(*this, Target, TEXT("after both")));
	TestFalse(TEXT("extraction no longer links to the pin"), Removed.Contains(TEXT(":Amount")));
	ReapplyIsNoOp(*this, Target, TEXT("ReapplyAfterBoth"));
	ReloadAndExtractEquals(*this, Target, Removed, TEXT("after reload (both)"));

	// A valued, unlinked pin is the editor's warning (ValidateOrphanPin): the apply succeeds and says so. (A
	// document that spells the graph out without the value resets the pin first, so there is no orphan.)
	TestTrue(TEXT("restore"), Update(Members + TwoInputs + OrphanGraph(false, 2), TEXT("RestoreWithDefault")).IsSuccess());
	const FAssetDocumentResult Valued = Update(Members + OneInput + TEXT("\"Components\":[]"), TEXT("RemoveInputKeepValue"));
	TestTrue(TEXT("input removal with only a saved value succeeds"), Valued.IsSuccess());
	const FAssetDocumentDiagnostic* Default = FindDiagnostic(Valued, TEXT("Graph.OrphanedPinDefault"));
	if (TestNotNull(TEXT("warning is Graph.OrphanedPinDefault"), Default))
	{
		TestTrue(TEXT("warning points at a graph node"), Default->Path.StartsWith(TEXT("/Body/UbergraphPages/0/Nodes/")));
		TestTrue(TEXT("warning names pin and node"), Default->Message.Contains(TEXT("'Amount'")) && Default->Message.Contains(TEXT("'Call2'")));
	}
	TestEqual(TEXT("the valued node keeps its orphan"), OrphanedPins(Target.Get()).Num(), 1);
	const FString Valueless = Serialize(Extract(*this, Target, TEXT("after valued removal")));
	TestFalse(TEXT("orphan pins stay out of the extraction"), Valueless.Contains(TEXT("PinOverrides")));
	ReapplyIsNoOp(*this, Target, TEXT("ReapplyAfterValued"));
	ReloadAndExtractEquals(*this, Target, Valueless, TEXT("after reload (valued)"));

	// (b) The editor path: removing the input from the signature leaves the link on an orphan pin. Extract keeps
	// the link, and re-applying the extraction is a no-op that neither fails nor loops.
	TestTrue(TEXT("restore again"), Update(Members + TwoInputs + OrphanGraph(true, 0), TEXT("RestoreForEditor")).IsSuccess());
	UBlueprint* Blueprint = Target.Get();
	if (!TestNotNull(TEXT("asset"), Blueprint))
	{
		return false;
	}
	UEdGraph* Signature = FBlueprintEditorUtils::GetDelegateSignatureGraphByName(Blueprint, TEXT("OnDied"));
	TArray<UK2Node_FunctionEntry*> Entries;
	if (Signature != nullptr)
	{
		Signature->GetNodesOfClass(Entries);
	}
	if (!TestEqual(TEXT("one signature entry"), Entries.Num(), 1))
	{
		return false;
	}
	AddExpectedError(TEXT("In use pin"), EAutomationExpectedErrorFlags::Contains, 0);
	Entries[0]->Modify();
	Entries[0]->RemoveUserDefinedPinByName(TEXT("Amount"));
	// FBaseBlueprintGraphActionDetails::OnParamsChanged, then the editor's structural refresh and compile.
	Entries[0]->bDisableOrphanPinSaving = true;
	Entries[0]->ReconstructNode();
	Entries[0]->bDisableOrphanPinSaving = false;
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	TestEqual(TEXT("the editor keeps the linked orphan"), OrphanedPins(Blueprint).Num(), 1);
	SavePackage(Blueprint);
	const FString Human = Serialize(Extract(*this, Target, TEXT("human edit")));
	TestTrue(TEXT("extraction keeps the link to the orphan pin"), Human.Contains(TEXT("\"To\":\"Call:Amount\"")));
	ReapplyIsNoOp(*this, Target, TEXT("ReapplyHuman"));
	ReloadAndExtractEquals(*this, Target, Human, TEXT("after reload (editor edit)"));
	return !HasAnyErrors();
}

// Case-only renames (blueprint.member.rename.v1 for variables, event dispatchers, own components and
// timelines; graph.rename.v1 for functions and macros): the same member under a new casing, like the editor.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUBlueprintCaseRenameTest, "UEAssetDoc.UBlueprint.CaseRename",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace AssetDocumentBlueprintMembersPythonTests
{
// One document, two spellings: Names[0..] are lower-case when bRenamed is false, capitalised when true.
FString CaseRenameBody(bool bRenamed)
{
	auto N = [bRenamed](const TCHAR* Lower, const TCHAR* Upper) { return FString(bRenamed ? Upper : Lower); };
	return FString::Printf(TEXT(
		"\"ParentClass\":\"/Script/Engine.Actor\","
		"\"Variables\":[{\"Name\":\"%s\",\"Type\":\"float\"}],"
		"\"Components\":[{\"Name\":\"%s\",\"Class\":\"/Script/Engine.SceneComponent\"}],"
		"\"EventDispatchers\":[{\"Name\":\"%s\",\"Inputs\":[{\"Name\":\"Amount\",\"Type\":\"float\"}]}],"
		"\"Timelines\":[{\"Name\":\"%s\",\"Length\":2.0,\"Tracks\":[{\"Name\":\"Alpha\",\"Type\":\"Float\",\"Keys\":[{\"Time\":0,\"Value\":0},{\"Time\":2,\"Value\":1}]}]}],"
		"\"FunctionGraphs\":[{\"Name\":\"%s\"}],"
		"\"MacroGraphs\":[{\"Name\":\"%s\"}],"
		"\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":["
		"{\"Id\":\"Begin\",\"Action\":\"K2:BeginPlay\"},"
		"{\"Id\":\"Value\",\"Action\":\"K2:Get:%s\"},"
		"{\"Id\":\"Broadcast\",\"Action\":\"K2:CallDelegate:%s\"},"
		"{\"Id\":\"Timeline\",\"Action\":\"K2:Timeline\",\"Name\":\"%s\"}],"
		"\"Links\":[{\"From\":\"Begin:then\",\"To\":\"Broadcast:execute\"},{\"From\":\"Value:%s\",\"To\":\"Broadcast:Amount\"},"
		"{\"From\":\"Broadcast:then\",\"To\":\"Timeline:Play\"}]}]"),
		*N(TEXT("health"), TEXT("Health")), *N(TEXT("pivot"), TEXT("Pivot")), *N(TEXT("ondied"), TEXT("OnDied")),
		*N(TEXT("fade"), TEXT("Fade")), *N(TEXT("dofoo"), TEXT("DoFoo")), *N(TEXT("domacro"), TEXT("DoMacro")),
		*N(TEXT("health"), TEXT("Health")), *N(TEXT("ondied"), TEXT("OnDied")), *N(TEXT("fade"), TEXT("Fade")),
		*N(TEXT("health"), TEXT("Health")));
}

// What a member's identity is made of, captured before and compared after.
struct FMemberIdentity
{
	FGuid VariableGuid;
	FGuid DispatcherGuid;
	const UEdGraph* SignatureGraph = nullptr;
	const USCS_Node* Component = nullptr;
	const UTimelineTemplate* Timeline = nullptr;
	const UEdGraph* Function = nullptr;
	const UEdGraph* Macro = nullptr;
	TSet<FGuid> EventGraphNodes;

	static FMemberIdentity Capture(UBlueprint& Blueprint, bool bRenamed)
	{
		FMemberIdentity Out;
		const FName Variable(bRenamed ? TEXT("Health") : TEXT("health"));
		const FName Dispatcher(bRenamed ? TEXT("OnDied") : TEXT("ondied"));
		const int32 VariableIndex = FBlueprintEditorUtils::FindNewVariableIndex(&Blueprint, Variable);
		const int32 DispatcherIndex = FBlueprintEditorUtils::FindNewVariableIndex(&Blueprint, Dispatcher);
		Out.VariableGuid = VariableIndex != INDEX_NONE ? Blueprint.NewVariables[VariableIndex].VarGuid : FGuid();
		Out.DispatcherGuid = DispatcherIndex != INDEX_NONE ? Blueprint.NewVariables[DispatcherIndex].VarGuid : FGuid();
		Out.SignatureGraph = FBlueprintEditorUtils::GetDelegateSignatureGraphByName(&Blueprint, Dispatcher);
		if (Blueprint.SimpleConstructionScript != nullptr)
		{
			for (const USCS_Node* Node : Blueprint.SimpleConstructionScript->GetAllNodes())
			{
				if (Node->GetVariableName() == FName(bRenamed ? TEXT("Pivot") : TEXT("pivot")))
				{
					Out.Component = Node;
				}
			}
		}
		Out.Timeline = Blueprint.FindTimelineTemplateByVariableName(FName(bRenamed ? TEXT("Fade") : TEXT("fade")));
		Out.Function = FindGraph(Blueprint.FunctionGraphs, bRenamed ? TEXT("DoFoo") : TEXT("dofoo"));
		Out.Macro = FindGraph(Blueprint.MacroGraphs, bRenamed ? TEXT("DoMacro") : TEXT("domacro"));
		for (const UEdGraph* Graph : Blueprint.UbergraphPages)
		{
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				Out.EventGraphNodes.Add(Node->NodeGuid);
			}
		}
		return Out;
	}
};
} // namespace AssetDocumentBlueprintMembersPythonTests

bool FAssetDocumentUBlueprintCaseRenameTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentBlueprintMembersPythonTests;
	const FTarget Target(TEXT("BP_CaseRename"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	if (!TestTrue(TEXT("create"), Apply(*this, Target, Document(Target, BlueprintClass, TEXT("Create"), CaseRenameBody(false)), TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	UBlueprint* Blueprint = Target.Get();
	if (!TestNotNull(TEXT("asset"), Blueprint))
	{
		return false;
	}
	const FMemberIdentity Before = FMemberIdentity::Capture(*Blueprint, false);
	TestTrue(TEXT("variable exists"), Before.VariableGuid.IsValid());
	TestTrue(TEXT("dispatcher exists"), Before.DispatcherGuid.IsValid() && Before.SignatureGraph != nullptr);
	TestNotNull(TEXT("component exists"), Before.Component);
	TestNotNull(TEXT("timeline exists"), Before.Timeline);
	TestNotNull(TEXT("function exists"), Before.Function);
	TestNotNull(TEXT("macro exists"), Before.Macro);
	TestTrue(TEXT("created asset compiles"), CompilesClean(Blueprint));
	const FString Renamed = Document(Target, BlueprintClass, TEXT("Update"), CaseRenameBody(true));

	// Diff sees the new casing before anything is applied.
	FAssetDocumentDiffRequest DiffRequest;
	DiffRequest.Document = Parse(Renamed);
	const FAssetDocumentResult Diffed = FAssetDocumentModule::Get().GetService().Diff(DiffRequest);
	const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
	if (TestTrue(TEXT("diff succeeds"), Diffed.IsSuccess() && Diffed.Payload.IsValid() && Diffed.Payload->TryGetArrayField(TEXT("changed"), Changed)))
	{
		TestTrue(TEXT("diff reports the case-only changes"), Changed->Num() > 0);
	}

	// Apply renames the members themselves.
	const FAssetDocumentResult Applied = Apply(*this, Target, Renamed, TEXT("Rename"));
	if (!TestTrue(TEXT("rename applies"), Applied.IsSuccess()))
	{
		return false;
	}
	TestTrue(TEXT("rename is saved"), Applied.bSavedAsset);
	Blueprint = Target.Get();
	TestTrue(TEXT("renamed asset compiles"), CompilesClean(Blueprint));
	const FMemberIdentity After = FMemberIdentity::Capture(*Blueprint, true);
	TestEqual(TEXT("variable keeps its guid"), After.VariableGuid, Before.VariableGuid);
	TestTrue(TEXT("variable has the new casing"), Blueprint->NewVariables.ContainsByPredicate(
		[](const FBPVariableDescription& Variable) { return Variable.VarName.ToString().Equals(TEXT("Health"), ESearchCase::CaseSensitive); }));
	TestEqual(TEXT("dispatcher keeps its guid"), After.DispatcherGuid, Before.DispatcherGuid);
	TestTrue(TEXT("dispatcher signature graph is the same object"), After.SignatureGraph == Before.SignatureGraph);
	TestTrue(TEXT("dispatcher signature graph has the new casing"), After.SignatureGraph != nullptr && After.SignatureGraph->GetName().Equals(TEXT("OnDied"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("component is the same node"), After.Component == Before.Component);
	TestTrue(TEXT("component has the new casing"), After.Component != nullptr && After.Component->GetVariableName().ToString().Equals(TEXT("Pivot"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("timeline is the same template"), After.Timeline == Before.Timeline);
	TestTrue(TEXT("timeline has the new casing"), After.Timeline != nullptr && After.Timeline->GetVariableName().ToString().Equals(TEXT("Fade"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("function is the same graph"), After.Function == Before.Function);
	TestTrue(TEXT("function has the new casing"), After.Function != nullptr && After.Function->GetName().Equals(TEXT("DoFoo"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("macro is the same graph"), After.Macro == Before.Macro);
	TestTrue(TEXT("macro has the new casing"), After.Macro != nullptr && After.Macro->GetName().Equals(TEXT("DoMacro"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("event graph nodes are the same nodes (references survived)"), After.EventGraphNodes.Num() == Before.EventGraphNodes.Num()
		&& After.EventGraphNodes.Includes(Before.EventGraphNodes));

	// Extract shows the new casing, the document and the asset agree, and re-applying the extraction is a no-op.
	TestNoDiff(*this, Renamed, TEXT("after rename"));
	const FString Extracted = Serialize(Extract(*this, Target, TEXT("extract after rename")));
	for (const TCHAR* Name : {TEXT("\"Name\":\"Health\""), TEXT("\"Name\":\"Pivot\""), TEXT("\"Name\":\"OnDied\""), TEXT("\"Name\":\"Fade\""),
		TEXT("\"Name\":\"DoFoo\""), TEXT("\"Name\":\"DoMacro\"")})
	{
		TestTrue(*FString::Printf(TEXT("extraction contains %s"), Name), Extracted.Contains(Name, ESearchCase::CaseSensitive));
	}
	for (const TCHAR* Name : {TEXT("\"Name\":\"health\""), TEXT("\"Name\":\"pivot\""), TEXT("\"Name\":\"ondied\""), TEXT("\"Name\":\"fade\""),
		TEXT("\"Name\":\"dofoo\""), TEXT("\"Name\":\"domacro\"")})
	{
		TestFalse(*FString::Printf(TEXT("extraction no longer contains %s"), Name), Extracted.Contains(Name, ESearchCase::CaseSensitive));
	}
	ReapplyIsNoOp(*this, Target, TEXT("ReapplyAfterRename"));
	const FAssetDocumentResult Again = Apply(*this, Target, Renamed, TEXT("RenameAgain"));
	TestTrue(TEXT("the renamed document re-applies"), Again.IsSuccess());
	TestFalse(TEXT("the renamed document re-applied is a no-op"), Again.bSavedAsset);

	// The saved package, loaded back from disk, extracts to the same document.
	ReloadAndExtractEquals(*this, Target, Extracted, TEXT("after reload"));

	// And back again: renaming is symmetric.
	TestTrue(TEXT("renames back"), Apply(*this, Target, Document(Target, BlueprintClass, TEXT("Update"), CaseRenameBody(false)), TEXT("RenameBack")).IsSuccess());
	if (UBlueprint* Back = Target.Get())
	{
		const FMemberIdentity Restored = FMemberIdentity::Capture(*Back, false);
		TestEqual(TEXT("variable guid survives the round trip"), Restored.VariableGuid, Before.VariableGuid);
		TestEqual(TEXT("dispatcher guid survives the round trip"), Restored.DispatcherGuid, Before.DispatcherGuid);
	}
	return !HasAnyErrors();
}

// Names a member declares for its own parts (inputs of a dispatcher or function, a custom event) follow a
// case-only change as well; none of them may be left at the old casing without a diagnostic.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUBlueprintCaseRenameDeclaredTest, "UEAssetDoc.UBlueprint.CaseRenameDeclared",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintCaseRenameDeclaredTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentBlueprintMembersPythonTests;
	const FTarget Target(TEXT("BP_CaseDeclared"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	auto Body = [](bool bUpper)
	{
		auto N = [bUpper](const TCHAR* Lower, const TCHAR* Upper) { return FString(bUpper ? Upper : Lower); };
		return FString::Printf(TEXT(
			"\"ParentClass\":\"/Script/Engine.Actor\","
			"\"EventDispatchers\":[{\"Name\":\"Died\",\"Inputs\":[{\"Name\":\"%s\",\"Type\":\"float\"}]}],"
			"\"FunctionGraphs\":[{\"Name\":\"Calc\",\"Signature\":{\"Inputs\":[{\"Name\":\"%s\",\"Type\":\"float\"}],"
			"\"Outputs\":[{\"Name\":\"%s\",\"Type\":\"float\"}],\"Pure\":true},"
			"\"Links\":[{\"From\":\"Entry:then\",\"To\":\"Result:execute\"}]}],"
			"\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":[{\"Id\":\"Evt\",\"Action\":\"K2:CustomEvent\",\"Name\":\"%s\"}]}]"),
			*N(TEXT("amount"), TEXT("Amount")), *N(TEXT("value"), TEXT("Value")), *N(TEXT("result"), TEXT("Result")),
			*N(TEXT("ping"), TEXT("Ping")));
	};
	if (!TestTrue(TEXT("create"), Apply(*this, Target, Document(Target, BlueprintClass, TEXT("Create"), Body(false)), TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	const FString Renamed = Document(Target, BlueprintClass, TEXT("Update"), Body(true));
	const FAssetDocumentResult Applied = Apply(*this, Target, Renamed, TEXT("Rename"));
	if (!TestTrue(TEXT("the declared-name rename applies"), Applied.IsSuccess()))
	{
		return false;
	}
	const FString Extracted = Serialize(Extract(*this, Target, TEXT("extract after rename")));
	for (const TCHAR* Name : {TEXT("\"Name\":\"Amount\""), TEXT("\"Name\":\"Value\""), TEXT("\"Name\":\"Result\""), TEXT("\"Name\":\"Ping\"")})
	{
		TestTrue(*FString::Printf(TEXT("extraction contains %s"), Name), Extracted.Contains(Name, ESearchCase::CaseSensitive));
	}
	for (const TCHAR* Name : {TEXT("\"Name\":\"amount\""), TEXT("\"Name\":\"value\""), TEXT("\"Name\":\"result\""), TEXT("\"Name\":\"ping\"")})
	{
		TestFalse(*FString::Printf(TEXT("extraction no longer contains %s"), Name), Extracted.Contains(Name, ESearchCase::CaseSensitive));
	}
	ReapplyIsNoOp(*this, Target, TEXT("ReapplyAfterRename"));
	ReloadAndExtractEquals(*this, Target, Extracted, TEXT("after reload"));
	return !HasAnyErrors();
}

// A component renamed only by case (Mesh -> mesh) whose overlap event is bound in the event graph
// (Details > Events > On Component Begin Overlap [+]). The editor's rename
// (FBlueprintComponentDetails::OnVariableTextCommitted -> RenameComponentMemberVariable) keeps the
// component and the bound event; the bound event must follow the new casing and the Blueprint compile.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUBlueprintCaseRenameBoundComponentTest,
	"UEAssetDoc.UBlueprint.CaseRenameBoundComponent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUBlueprintCaseRenameBoundComponentTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentBlueprintMembersPythonTests;
	using namespace AssetDocumentBlueprintMembersPythonTests;
	const FTarget Target(TEXT("BP_CaseRenameBound"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	auto Body = [](const TCHAR* Name)
	{
		return FString::Printf(TEXT(
			"\"ParentClass\":\"/Script/Engine.Actor\","
			"\"Variables\":[{\"Name\":\"Hits\",\"Type\":\"int\"}],"
			"\"Components\":[{\"Name\":\"%s\",\"Class\":\"/Script/Engine.StaticMeshComponent\"}],"
			"\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":["
			"{\"Id\":\"Overlap\",\"Action\":\"K2:ComponentBoundEvent:/Script/Engine.PrimitiveComponent:OnComponentBeginOverlap@%s\"},"
			"{\"Id\":\"Set\",\"Action\":\"K2:Set:Hits\",\"PinOverrides\":[{\"Pin\":\"Hits\",\"DefaultValue\":\"1\"}]}],"
			"\"Links\":[{\"From\":\"Overlap:then\",\"To\":\"Set:execute\"}]}]"), Name, Name);
	};
	auto FindBound = [](UBlueprint* Blueprint) -> UK2Node_ComponentBoundEvent*
	{
		TArray<UK2Node_ComponentBoundEvent*> Nodes;
		if (Blueprint != nullptr)
		{
			FBlueprintEditorUtils::GetAllNodesOfClass<UK2Node_ComponentBoundEvent>(Blueprint, Nodes);
		}
		return Nodes.Num() == 1 ? Nodes[0] : nullptr;
	};
	auto FindComponent = [](UBlueprint* Blueprint) -> USCS_Node*
	{
		return Blueprint != nullptr && Blueprint->SimpleConstructionScript != nullptr && Blueprint->SimpleConstructionScript->GetAllNodes().Num() == 1
			? Blueprint->SimpleConstructionScript->GetAllNodes()[0] : nullptr;
	};
	if (!TestTrue(TEXT("create"), Apply(*this, Target, Document(Target, BlueprintClass, TEXT("Create"), Body(TEXT("Mesh"))), TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	UBlueprint* Blueprint = Target.Get();
	const USCS_Node* Component = FindComponent(Blueprint);
	const UK2Node_ComponentBoundEvent* Bound = FindBound(Blueprint);
	if (!TestNotNull(TEXT("component"), Component) || !TestNotNull(TEXT("bound event"), Bound))
	{
		return false;
	}
	const FGuid BoundGuid = Bound->NodeGuid;
	TestTrue(TEXT("created Blueprint compiles"), CompilesClean(Blueprint));

	const FString Renamed = Document(Target, BlueprintClass, TEXT("Update"), Body(TEXT("mesh")));
	const FAssetDocumentResult Applied = Apply(*this, Target, Renamed, TEXT("Rename"));
	if (!TestTrue(TEXT("case rename applies"), Applied.IsSuccess()))
	{
		return false;
	}
	TestTrue(TEXT("case rename is saved"), Applied.bSavedAsset);
	TestEqual(TEXT("case rename reports no diagnostics"), Applied.Diagnostics.Num(), 0);
	Blueprint = Target.Get();
	TestTrue(TEXT("same component node"), FindComponent(Blueprint) == Component);
	TestTrue(TEXT("component has the new casing"), Component->GetVariableName().ToString().Equals(TEXT("mesh"), ESearchCase::CaseSensitive));
	Bound = FindBound(Blueprint);
	TestTrue(TEXT("same bound event node"), Bound != nullptr && Bound->NodeGuid == BoundGuid);
	TestTrue(TEXT("bound event names the new casing"), Bound != nullptr
		&& Bound->ComponentPropertyName.ToString().Equals(TEXT("mesh"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("renamed Blueprint compiles"), CompilesClean(Blueprint));
	// The compiled class binds the event to the renamed component property.
	const UBlueprintGeneratedClass* Generated = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
	bool bBindingFollows = false;
	for (const UDynamicBlueprintBinding* Binding : Generated != nullptr ? Generated->DynamicBindingObjects : TArray<TObjectPtr<UDynamicBlueprintBinding>>())
	{
		if (const UComponentDelegateBinding* Delegates = Cast<UComponentDelegateBinding>(Binding))
		{
			for (const FBlueprintComponentDelegateBinding& Entry : Delegates->ComponentDelegateBindings)
			{
				bBindingFollows |= Entry.ComponentPropertyName == TEXT("mesh") && Entry.DelegatePropertyName == TEXT("OnComponentBeginOverlap");
			}
		}
	}
	TestTrue(TEXT("the compiled class binds the event to the renamed component"), bBindingFollows);
	TestNoDiff(*this, Renamed, TEXT("after case rename"));
	const FString Extracted = Serialize(Extract(*this, Target, TEXT("extract after case rename")));
	TestTrue(TEXT("extraction names the bound event with the new casing"), Extracted.Contains(TEXT("OnComponentBeginOverlap@mesh"), ESearchCase::CaseSensitive));
	TestFalse(TEXT("extraction no longer has the old casing"), Extracted.Contains(TEXT("@Mesh"), ESearchCase::CaseSensitive));
	ReapplyIsNoOp(*this, Target, TEXT("ReapplyAfterRename"));
	ReloadAndExtractEquals(*this, Target, Extracted, TEXT("after reload"));
	if (UBlueprint* Reloaded = Target.Get())
	{
		TestTrue(TEXT("reloaded Blueprint compiles"), CompilesClean(Reloaded));
	}

	// And back.
	const FAssetDocumentResult Back = Apply(*this, Target, Document(Target, BlueprintClass, TEXT("Update"), Body(TEXT("Mesh"))), TEXT("RenameBack"));
	TestTrue(TEXT("rename back applies"), Back.IsSuccess());
	Bound = FindBound(Target.Get());
	TestTrue(TEXT("bound event back to the old casing, same node"), Bound != nullptr && Bound->NodeGuid == BoundGuid
		&& Bound->ComponentPropertyName.ToString().Equals(TEXT("Mesh"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("Blueprint compiles after renaming back"), CompilesClean(Target.Get()));
	return !HasAnyErrors();
}

#endif
