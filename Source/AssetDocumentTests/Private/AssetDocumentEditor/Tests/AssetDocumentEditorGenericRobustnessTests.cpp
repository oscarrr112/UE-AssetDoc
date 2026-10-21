// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Production-Service regressions for the generic route found by the coverage
// audit (docs/architecture/asset-document-coverage-audit.md):
//   - IntegralNumbers: Extract output (UE JSON prints 1.0 as "1") re-applies as a
//     no-op, and an integer literal is accepted for a float property. A float
//     property stores the nearest float32 of any number (0.3 extracts as 0.3).
//   - InteractiveFactory: a factory that opens a modal dialog is refused without
//     blocking the editor and without residue.
//   - SlowTaskWindow: the non-interactive scope lets a slow task's progress window
//     through (it never blocks) and still refuses a blocking modal, also one opened
//     while a progress window is up.
//   - UnsafeEditorPaths: factories and callbacks that crash the editor without
//     their New-asset dialog context (or without audio) are refused cleanly.
//   - FactoryConfiguration: what the New-asset dialog would set is supplied
//     (an ancestor factory's class setting, a document's FactoryContext) or the
//     create is refused with a diagnostic naming what is missing.
//   - StagingPackagesReleased: the staged copy each durable apply loads for its
//     fresh verification is released and collected when the apply returns, after
//     success and after a failed apply (rollback): no provider walking loaded
//     objects sees it (also a staged Blueprint's class) and it pins nothing.
//   - ScratchClonePostLoadFlags: assets whose PostLoad makes them Standalone
//     (IKRigDefinition, IKRetargeter) are previewed and edited through the scratch clone.
//   - ScratchFactoryAssetFlags: factories that add RF_Public | RF_Standalone to their
//     object (ChaosSolver, ChooserTable, ProxyTable) still give a valid preflight scratch.
//   - StructWriteKeepsNativeState: a struct value is written member by member, so native
//     state such as FDataflowInstance's owner delegate survives (GeometryCollection).
//   - ScratchReleasesNativeBindings: a dropped scratch clone of a GeometryCollection leaves no
//     FDataflowInstance property-change binding to its memory and keeps the class default's.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "AssetDocumentEditorFactoryFailureTestTypes.h"
#include "AssetDocumentTestDirectoryBlock.h"
#include "AssetDocumentEditorNonInteractiveScope.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Factories/Factory.h"
#include "Provider/Lifecycle/AssetDocumentEditorGenericFactory.h"
#include "Provider/Lifecycle/AssetDocumentEditorLifecyclePayloads.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/Engine.h"
#include "Engine/ObjectLibrary.h"
#include "FeedbackContextEditor.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/SlowTask.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Styling/SlateWidgetStyleAsset.h"
#include "Styling/SlateWidgetStyleContainerBase.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/SWindow.h"
#include "Utils/AssetDocumentEngineCompat.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorGenericRobustnessTests
{
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
		PackageName = FString(TEXT("/Game/AssetDocumentRobustness/")) + AssetName;
		ObjectPath = PackageName + TEXT(".") + AssetName;
		PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		SidecarPath = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("AssetDocumentRobustness"),
			AssetName + TEXT(".assetdoc.json"));
	}

	FString Document(const FString& Class, const TCHAR* Action, const FString& Properties) const
	{
		return Document(Class, Action, Properties, FString());
	}

	FString Document(const FString& Class, const TCHAR* Action, const FString& Properties, const FString& FactoryContext) const
	{
		return FString::Printf(
			TEXT("{\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\",\"Target\":\"%s\",\"Class\":\"%s\",")
			TEXT("\"Action\":\"%s\",\"Properties\":%s%s}"),
			*PackageName, *Class, Action, *Properties,
			FactoryContext.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(",\"FactoryContext\":\"%s\""), *FactoryContext));
	}

	void Cleanup() const
	{
		UObject* Asset = FSoftObjectPath(ObjectPath).ResolveObject();
		if (IFileManager::Get().FileExists(*PackageFile))
		{
			if (Asset != nullptr)
			{
				UPackageTools::UnloadPackages({Asset->GetOutermost()});
				CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			}
			IFileManager::Get().Delete(*PackageFile, false, true);
			FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({PackageFile});
		}
		else if (Asset != nullptr)
		{
			ObjectTools::DeleteObjects({Asset}, false, ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
		}
		IFileManager::Get().Delete(*SidecarPath, false, true);
	}
};

FString Serialize(const TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	if (Object.IsValid())
	{
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}
	return Text;
}

FAssetDocumentResult ApplyJson(FAutomationTestBase& Test, const FTarget& Target, const FString& Json, const TCHAR* Stage)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target.SidecarPath), true);
	Test.TestTrue(*FString::Printf(TEXT("%s writes sidecar"), Stage), FFileHelper::SaveStringToFile(Json, *Target.SidecarPath));
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Target.SidecarPath;
	Request.bSaveAsset = true;
	FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
	Test.AddInfo(FString::Printf(TEXT("%s: %s"), Stage, *Result.Message));
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Test.AddInfo(FString::Printf(TEXT("%s: [%s] %s %s"), Stage, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
	}
	return Result;
}

FAssetDocumentResult ExtractAll(const FTarget& Target)
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = Target.ObjectPath;
	Request.bDiffOnly = false;
	Request.bIncludeAllWritable = true;
	return FAssetDocumentModule::Get().GetService().Extract(Request);
}

bool HasCode(const FAssetDocumentResult& Result, const FString& Code)
{
	return Result.Diagnostics.ContainsByPredicate(
		[&Code](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == Code; });
}

void TestNoLeftovers(FAutomationTestBase& Test, const FTarget& Target, const TCHAR* Stage)
{
	Test.TestNull(*FString::Printf(TEXT("%s: no object"), Stage), FSoftObjectPath(Target.ObjectPath).ResolveObject());
	const UPackage* Package = FindPackage(nullptr, *Target.PackageName);
	Test.TestTrue(*FString::Printf(TEXT("%s: no live package"), Stage), Package == nullptr || !Package->IsDirty());
	Test.TestFalse(*FString::Printf(TEXT("%s: no registry entry"), Stage),
		FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(Target.ObjectPath)).IsValid());
	Test.TestFalse(*FString::Printf(TEXT("%s: no package file"), Stage), IFileManager::Get().FileExists(*Target.PackageFile));
}

// Create fails with ExpectedCode, the message names it, and nothing is left behind.
void TestCreateRefused(FAutomationTestBase& Test, const TCHAR* Prefix, const FString& Class, const FString& Properties,
	const TCHAR* ExpectedCode, const TCHAR* ExpectedMessageText, const FString& FactoryContext = FString())
{
	const FTarget Target(Prefix);
	ON_SCOPE_EXIT { Target.Cleanup(); };
	const FAssetDocumentResult Result =
		ApplyJson(Test, Target, Target.Document(Class, TEXT("Create"), Properties, FactoryContext), ExpectedCode);
	Test.TestFalse(*FString::Printf(TEXT("%s create is refused"), *Class), Result.IsSuccess());
	Test.TestTrue(*FString::Printf(TEXT("%s reports %s"), *Class, ExpectedCode), HasCode(Result, ExpectedCode));
	Test.TestTrue(*FString::Printf(TEXT("%s message explains the refusal (was '%s')"), *Class, *Result.Message),
		Result.Message.Contains(ExpectedMessageText));
	TestNoLeftovers(Test, Target, ExpectedCode);
}

bool IsClassLoaded(const TCHAR* ClassPath)
{
	return FindObject<UClass>(nullptr, ClassPath) != nullptr;
}

// The engine's unattended-modal cancellation logs a warning plus an error-level
// stack dump (FDebug::DumpStackTraceToLog); both are the expected evidence.
void ExpectCancelledModalLog(FAutomationTestBase& Test)
{
	Test.AddExpectedMessage(TEXT("A modal window tried to take control while running in unattended script mode"),
		ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0, false);
	Test.AddExpectedMessage(TEXT("FDebug::DumpStackTrace"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, -1, false);
	Test.AddExpectedMessage(TEXT("[Callstack]"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, -1, false);
	Test.AddExpectedMessage(TEXT(""), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Exact, -1, false);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericRobustnessIntegralNumbersTest,
	"UEAssetDoc.GenericRuntime.Robustness.IntegralNumbers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericRobustnessIntegralNumbersTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericRobustnessTests;
	// Audit classes whose unchanged re-apply failed with PropertyPreflight.DecodeFailed.
	struct FCase
	{
		const TCHAR* Prefix;
		const TCHAR* Class;
		const TCHAR* CreateProperties; // integer literals for float properties
	};
	const FCase Cases[] = {
		{TEXT("Reverb_Integral"), TEXT("/Script/Engine.ReverbEffect"), TEXT("{\"Density\":1,\"Diffusion\":0,\"Gain\":0.25}")},
		{TEXT("PhysMat_Integral"), TEXT("/Script/PhysicsCore.PhysicalMaterial"),
			TEXT("{\"Density\":2,\"DebugColor\":{\"R\":1,\"G\":0,\"B\":0,\"A\":1}}")},
	};
	for (const FCase& Case : Cases)
	{
		const FTarget Target(Case.Prefix);
		ON_SCOPE_EXIT { Target.Cleanup(); };
		const FAssetDocumentResult Created =
			ApplyJson(*this, Target, Target.Document(Case.Class, TEXT("Create"), Case.CreateProperties), Case.Prefix);
		if (!TestTrue(FString::Printf(TEXT("%s accepts integer literals for float properties"), Case.Class), Created.IsSuccess()))
		{
			continue;
		}
		const FAssetDocumentResult Extracted = ExtractAll(Target);
		if (!TestTrue(TEXT("extract succeeds"), Extracted.IsSuccess() && Extracted.Payload.IsValid()))
		{
			continue;
		}
		// The MCP hands Extract to clients through UE JSON, which prints 1.0 as "1".
		const FString ExtractJson = Serialize(Extracted.Payload);
		AddInfo(ExtractJson);
		const FAssetDocumentResult Reapplied = ApplyJson(*this, Target, ExtractJson, TEXT("unchanged re-apply"));
		TestTrue(FString::Printf(TEXT("%s Extract output re-applies"), Case.Class), Reapplied.IsSuccess());
		TestFalse(FString::Printf(TEXT("%s unchanged re-apply is a no-op"), Case.Class), Reapplied.bSavedAsset);
	}

	{
		// A float property stores the nearest float32 of an authored number, as the
		// Details panel does: 0.51 and 0.3 are accepted, extract
		// as written (not 0.50999999 / 0.30000001) and re-apply unchanged; the widened
		// spelling of the same float is the same value.
		const FTarget Target(TEXT("Reverb_Precision"));
		ON_SCOPE_EXIT { Target.Cleanup(); };
		const FAssetDocumentResult Created = ApplyJson(*this, Target,
			Target.Document(TEXT("/Script/Engine.ReverbEffect"), TEXT("Create"), TEXT("{\"Density\":0.51,\"Gain\":0.3}")),
			TEXT("float precision"));
		if (TestTrue(TEXT("0.51 and 0.3 are accepted for float properties"), Created.IsSuccess()))
		{
			const FAssetDocumentResult Extracted = ExtractAll(Target);
			const FString ExtractJson = Extracted.Payload.IsValid() ? Serialize(Extracted.Payload) : FString();
			TestTrue(TEXT("extract writes the shortest decimal of each stored float"),
				ExtractJson.Contains(TEXT("\"Density\":0.51,")) && ExtractJson.Contains(TEXT("\"Gain\":0.3,")));
			TestFalse(TEXT("extract does not widen floats"),
				ExtractJson.Contains(TEXT("0.50999999")) || ExtractJson.Contains(TEXT("0.30000001")));
			const FAssetDocumentResult Reapplied = ApplyJson(*this, Target, ExtractJson, TEXT("precision re-apply"));
			TestTrue(TEXT("extract re-applies"), Reapplied.IsSuccess());
			TestFalse(TEXT("extract re-apply is a no-op"), Reapplied.bSavedAsset);
			const FAssetDocumentResult Widened = ApplyJson(*this, Target,
				Target.Document(TEXT("/Script/Engine.ReverbEffect"), TEXT("Update"),
					TEXT("{\"Density\":0.50999999046325684,\"Gain\":0.30000001192092896}")), TEXT("widened spelling"));
			TestTrue(TEXT("the widened spelling of the stored floats is accepted"), Widened.IsSuccess());
			TestFalse(TEXT("the widened spelling is the same value (no-op)"), Widened.bSavedAsset);
			const FAssetDocumentResult Finer = ApplyJson(*this, Target,
				Target.Document(TEXT("/Script/Engine.ReverbEffect"), TEXT("Update"),
					TEXT("{\"Density\":0.510000001,\"Gain\":0.3}")), TEXT("finer than float"));
			TestTrue(TEXT("a number finer than float precision is accepted"), Finer.IsSuccess());
			TestFalse(TEXT("and compares equal to the stored float (no-op)"), Finer.bSavedAsset);
		}
	}
	{
		// An integral number is accepted for an integer property; a fraction is not truncated.
		const FTarget Target(TEXT("DA_IntegralInt"));
		ON_SCOPE_EXIT { Target.Cleanup(); };
		const FAssetDocumentResult Result = ApplyJson(*this, Target,
			Target.Document(TEXT("/Script/AssetDocumentTests.TestDataAsset"), TEXT("Create"), TEXT("{\"TestInt\":3.0}")), TEXT("int 3.0"));
		TestTrue(TEXT("3.0 is accepted for an int property"), Result.IsSuccess());
		const FTarget Fraction(TEXT("DA_FractionInt"));
		ON_SCOPE_EXIT { Fraction.Cleanup(); };
		const FAssetDocumentResult Rejected = ApplyJson(*this, Fraction,
			Fraction.Document(TEXT("/Script/AssetDocumentTests.TestDataAsset"), TEXT("Create"), TEXT("{\"TestInt\":2.5}")), TEXT("int 2.5"));
		TestFalse(TEXT("2.5 is rejected for an int property"), Rejected.IsSuccess());
		TestTrue(TEXT("rejection says an integral number is expected"),
			HasCode(Rejected, TEXT("PropertyPreflight.DecodeFailed")) && Rejected.Message.Contains(TEXT("integral")));
		TestNoLeftovers(*this, Fraction, TEXT("int 2.5"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericRobustnessInteractiveFactoryTest,
	"UEAssetDoc.GenericRuntime.Robustness.InteractiveFactory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericRobustnessInteractiveFactoryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericRobustnessTests;
	ExpectCancelledModalLog(*this);
	UAssetDocumentEditorModalTestFactory::SidePackageName.Reset();
	TestCreateRefused(*this, TEXT("ModalFactory"),
		UAssetDocumentEditorInteractiveTestAsset::StaticClass()->GetPathName(), TEXT("{\"TestFloat\":1}"),
		TEXT("FactoryCreate.InteractiveFactory"), TEXT("tried to open a modal editor dialog"));
	TestFalse(TEXT("the modal factory ran"), UAssetDocumentEditorModalTestFactory::SidePackageName.IsEmpty());
	TestNull(TEXT("the package the refused factory created is discarded"),
		FindPackage(nullptr, *UAssetDocumentEditorModalTestFactory::SidePackageName));

	// The audit's real case: GroomFactory shows SMessageDialog::ShowModal.
	if (IsClassLoaded(TEXT("/Script/HairStrandsCore.GroomAsset")))
	{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
		// Here the groom factory skips its Dataflow template picker while a script runs unattended, which an apply
		// always does: the create runs without a dialog and the groom gets no Dataflow, as when the picker is closed.
		{
			const FTarget Groom(TEXT("GA_Groom"));
			ON_SCOPE_EXIT { Groom.Cleanup(); };
			const FAssetDocumentResult Created = ApplyJson(*this, Groom,
				Groom.Document(TEXT("/Script/HairStrandsCore.GroomAsset"), TEXT("Create"), TEXT("{}")), TEXT("unattended groom"));
			TestTrue(TEXT("an unattended groom create runs without a dialog"), Created.IsSuccess());
			TestFalse(TEXT("the unattended groom create answers no dialog"), HasCode(Created, TEXT("GenericRuntime.EditorDialogAnswered")));
		}
#else
		TestCreateRefused(*this, TEXT("GA_Groom"), TEXT("/Script/HairStrandsCore.GroomAsset"), TEXT("{}"),
			TEXT("FactoryCreate.InteractiveFactory"), TEXT("GroomFactory"));
#endif
		TestNull(TEXT("no Dataflow side package for the scratch groom"),
			FindPackage(nullptr, TEXT("/Engine/DF_Scratch_GroomAsset")));
	}
	else
	{
		AddInfo(TEXT("HairStrands is not loaded; the Groom case is skipped."));
	}
	return true;
}

// FScopedSlowTask::MakeDialog with the editor's feedback context (automation swaps GWarn for one that opens no
// window) opens the progress window through AddModalWindow(bSlowTaskWindow = true), which unattended mode lets
// through and which broadcasts the pre-Slate-modal event: the Material Editor's Apply does this on every
// compile ("Updating Texture Streaming Data"). The window never blocks and asks nothing, so the scope does not
// refuse it; a blocking modal stays refused, also while a progress window is up.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericRobustnessSlowTaskWindowTest,
	"UEAssetDoc.GenericRuntime.Robustness.SlowTaskWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericRobustnessSlowTaskWindowTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericRobustnessTests;
	if (!FSlateApplication::IsInitialized() || !FSlateApplication::Get().CanDisplayWindows() || GIsSlowTask)
	{
		AddError(TEXT("This editor cannot open a progress window (no Slate renderer, or a slow task is already running)."));
		return false;
	}
	static FFeedbackContextEditor EditorFeedback;
	ExpectCancelledModalLog(*this);

	{
		FAssetDocumentEditorNonInteractiveScope NonInteractive;
		bool bProgressWindowOpened = false;
		{
			FScopedSlowTask SlowTask(1.f, INVTEXT("AssetDocument progress window test"), true, EditorFeedback);
			SlowTask.MakeDialog(true);
			bProgressWindowOpened = GIsSlowTask && FSlateApplication::Get().GetActiveModalWindow().IsValid();
			SlowTask.EnterProgressFrame(1.f);
		}
		TestTrue(TEXT("the slow task opened its progress window (the case this test covers)"), bProgressWindowOpened);
		TestFalse(TEXT("a progress window is not refused as an editor dialog"), NonInteractive.WasInteractionRefused());
	}

	const auto OpenBlockingModal = []()
	{
		FSlateApplication::Get().AddModalWindow(
			SNew(SWindow).Title(INVTEXT("AssetDocument modal test")).ClientSize(FVector2D(200.f, 100.f)), nullptr);
	};
	{
		FAssetDocumentEditorNonInteractiveScope NonInteractive;
		OpenBlockingModal();
		TestTrue(TEXT("a blocking modal window is refused"), NonInteractive.WasInteractionRefused());
	}
	{
		FAssetDocumentEditorNonInteractiveScope NonInteractive;
		FScopedSlowTask SlowTask(1.f, INVTEXT("AssetDocument progress window test"), true, EditorFeedback);
		SlowTask.MakeDialog(true);
		OpenBlockingModal();
		TestTrue(TEXT("a blocking modal window opened under a progress window is refused"),
			NonInteractive.WasInteractionRefused());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericRobustnessUnsafeEditorPathsTest,
	"UEAssetDoc.GenericRuntime.Robustness.UnsafeEditorPaths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericRobustnessUnsafeEditorPathsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericRobustnessTests;
	// Each of these crashed the editor through the generic route before.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	// Light weight instance managers are deprecated classes here: class resolution refuses them.
	TestCreateRefused(*this, TEXT("LWI"), TEXT("/Script/Engine.LightWeightInstanceManager"), TEXT("{}"),
		TEXT("ClassResolve.Deprecated"), TEXT("deprecated"));
	TestCreateRefused(*this, TEXT("LWISM"), TEXT("/Script/Engine.LightWeightInstanceStaticMeshManager"), TEXT("{}"),
		TEXT("ClassResolve.Deprecated"), TEXT("deprecated"));
#else
	// Light weight instance managers are Actors: their factory makes a Blueprint with that parent class.
	TestCreateRefused(*this, TEXT("LWI"), TEXT("/Script/Engine.LightWeightInstanceManager"), TEXT("{}"),
		AssetDocumentEditorBlueprintClassUnsupportedCode, TEXT("Actor class"));
	TestCreateRefused(*this, TEXT("LWISM"), TEXT("/Script/Engine.LightWeightInstanceStaticMeshManager"), TEXT("{}"),
		AssetDocumentEditorBlueprintClassUnsupportedCode, TEXT("Blueprint profile"));
#endif
	// A slate widget style asset needs the style type its New-asset dialog picks; a FactoryContext class path
	// supplies it (AssetDocumentEditorGenericFactoryContext.cpp) and the asset holds that style.
	TestCreateRefused(*this, TEXT("SWS"), TEXT("/Script/SlateCore.SlateWidgetStyleAsset"), TEXT("{}"),
		TEXT("FactoryCreate.MissingFactoryContext"), TEXT("StyleType"));
	{
		const FTarget Target(TEXT("SWSCtx"));
		ON_SCOPE_EXIT { Target.Cleanup(); };
		const FAssetDocumentResult Created = ApplyJson(*this, Target,
			Target.Document(TEXT("/Script/SlateCore.SlateWidgetStyleAsset"), TEXT("Create"), TEXT("{}"),
				TEXT("/Script/Slate.ButtonWidgetStyle")), TEXT("style with context"));
		TestTrue(TEXT("SlateWidgetStyleAsset is created from a class FactoryContext"), Created.IsSuccess());
		// Re-resolved by path: the persisted apply ended with a full editor GC.
		const USlateWidgetStyleAsset* Style = Cast<USlateWidgetStyleAsset>(FSoftObjectPath(Target.ObjectPath).ResolveObject());
		TestTrue(TEXT("the created asset holds the context's style type"),
			Style != nullptr && Style->CustomStyle != nullptr
				&& Style->CustomStyle->GetClass()->GetPathName() == TEXT("/Script/Slate.ButtonWidgetStyle"));
	}
	// A class path that names no class fails before the factory runs; a class the dialog's picker would not
	// offer (outside StyleType's meta class) is refused by the factory configuration.
	AddExpectedMessage(TEXT("Failed to find object 'Class /Script/Slate.NoSuchWidgetStyle'"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 0, false);
	TestCreateRefused(*this, TEXT("SWSUnknown"), TEXT("/Script/SlateCore.SlateWidgetStyleAsset"), TEXT("{}"),
		TEXT("Generic.FactoryContextMissing"), TEXT("NoSuchWidgetStyle"), TEXT("/Script/Slate.NoSuchWidgetStyle"));
	TestCreateRefused(*this, TEXT("SWSWrong"), TEXT("/Script/SlateCore.SlateWidgetStyleAsset"), TEXT("{}"),
		TEXT("FactoryCreate.ContextUnsupported"), TEXT("StaticMesh"), TEXT("/Script/Engine.StaticMesh"));
	// (Skeleton creation is refused by its own profile: AnimationAssets.Skeleton.)

	// SoundEffectSourcePresetChain::PostEditChangeProperty needs the audio device manager.
	{
		const FTarget Target(TEXT("SrcChain"));
		ON_SCOPE_EXIT { Target.Cleanup(); };
		const FAssetDocumentResult Result = ApplyJson(*this, Target,
			Target.Document(TEXT("/Script/Engine.SoundEffectSourcePresetChain"), TEXT("Create"),
				TEXT("{\"bPlayEffectChainTails\":false}")), TEXT("source effect chain"));
		if (GEngine == nullptr || GEngine->GetAudioDeviceManager() == nullptr)
		{
			TestFalse(TEXT("write without audio is refused"), Result.IsSuccess());
			TestTrue(TEXT("refusal names the missing service"), HasCode(Result, TEXT("PropertyWrite.EditorServiceUnavailable")));
			TestNoLeftovers(*this, Target, TEXT("source effect chain"));
		}
		else
		{
			TestTrue(TEXT("write with audio succeeds"), Result.IsSuccess());
		}
	}

	// MetaSounds have their own Profile (profile.metasound_source.v1): created from Properties, and a Details
	// setting of an existing one is written as the Details panel writes it (no editor crash without audio).
	if (IsClassLoaded(TEXT("/Script/MetasoundEngine.MetaSoundSource")))
	{
		const FTarget Target(TEXT("MSS"));
		ON_SCOPE_EXIT { Target.Cleanup(); };
		const FString Class = TEXT("/Script/MetasoundEngine.MetaSoundSource");
		// An exact profile's document: an (empty) Body instead of the generic AssetType marker.
		auto MetaSoundDocument = [&Target, &Class](const TCHAR* Action, const FString& Properties)
		{
			return Target.Document(Class, Action, Properties).Replace(TEXT("\"AssetType\":\"GenericAsset\","), TEXT("\"Body\":{},"));
		};
		const FAssetDocumentResult Created =
			ApplyJson(*this, Target, MetaSoundDocument(TEXT("Create"), TEXT("{}")), TEXT("metasound create"));
		const FAssetDocumentResult Extracted = Created.IsSuccess() ? ExtractAll(Target) : FAssetDocumentResult();
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		FString Flipped;
		FString FlippedKey;
		bool bFlippedValue = false;
		if (Extracted.Payload.IsValid() && Extracted.Payload->TryGetObjectField(TEXT("Properties"), Properties))
		{
			for (const auto& Field : (*Properties)->Values)
			{
				bool bValue = false;
				if (Field.Value.IsValid() && Field.Value->TryGetBool(bValue) && Field.Value->Type == EJson::Boolean)
				{
					Flipped = FString::Printf(TEXT("{\"%s\":%s}"), *Field.Key, bValue ? TEXT("false") : TEXT("true"));
					FlippedKey = Field.Key;
					bFlippedValue = !bValue;
					break;
				}
			}
		}
		if (TestTrue(TEXT("MetaSoundSource is created and has a boolean property to edit"), !Flipped.IsEmpty()))
		{
			const FAssetDocumentResult Updated =
				ApplyJson(*this, Target, MetaSoundDocument(TEXT("Update"), Flipped), TEXT("metasound update"));
			TestTrue(TEXT("MetaSound Details update applies"), Updated.IsSuccess());
			const FAssetDocumentResult Reread = Updated.IsSuccess() ? ExtractAll(Target) : FAssetDocumentResult();
			const TSharedPtr<FJsonObject>* After = nullptr;
			bool bValueAfter = !bFlippedValue;
			const bool bFlipped = Reread.Payload.IsValid() && Reread.Payload->TryGetObjectField(TEXT("Properties"), After)
				&& (*After)->TryGetBoolField(FlippedKey, bValueAfter) && bValueAfter == bFlippedValue;
			TestTrue(TEXT("the Details setting reads back changed"), bFlipped);
		}
	}
	else
	{
		AddInfo(TEXT("Metasound is not loaded; the MetaSound case is skipped."));
	}
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericRobustnessFactoryConfigurationTest,
	"UEAssetDoc.GenericRuntime.Robustness.FactoryConfiguration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericRobustnessFactoryConfigurationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericRobustnessTests;

	// Raw reflected configuration (AssetDocumentEditorGenericFactoryClass.cpp):
	// the ancestor factory's class picker setting receives the requested class.
	UClass* SubmixFactoryClass = FindObject<UClass>(nullptr, TEXT("/Script/AudioEditor.SoundSubmixEffectFactory"));
	UClass* ReverbClass = FindObject<UClass>(nullptr, TEXT("/Script/AudioMixer.SubmixEffectReverbPreset"));
	if (TestTrue(TEXT("submix effect factory and preset are loaded"), SubmixFactoryClass != nullptr && ReverbClass != nullptr))
	{
		const FClassProperty* Setting = CastField<FClassProperty>(
			SubmixFactoryClass->FindPropertyByName(TEXT("SoundEffectSubmixPresetClass")));
		const TStrongObjectPtr<UFactory> Factory = NewAssetDocumentEditorGenericFactoryInstance(*SubmixFactoryClass);
		FString Code;
		FString Message;
		if (TestNotNull(TEXT("class setting exists"), Setting) && TestTrue(TEXT("factory instance"), Factory.IsValid()))
		{
			TestTrue(TEXT("class setting configured"),
				ConfigureAssetDocumentEditorGenericFactoryAssetClass(*Factory.Get(), *ReverbClass, Code, Message));
			TestTrue(TEXT("setting holds the requested class"),
				Setting->GetObjectPropertyValue_InContainer(Factory.Get()) == ReverbClass);
			TestNull(TEXT("CDO untouched"), Setting->GetObjectPropertyValue_InContainer(SubmixFactoryClass->GetDefaultObject()));
		}
	}

	// Ancestor factories with a class setting create the exact subclass.
	for (const TCHAR* Class : {TEXT("/Script/AudioMixer.SubmixEffectReverbPreset"), TEXT("/Script/Synthesis.SourceEffectEQPreset"),
		TEXT("/Script/AudioSynesthesia.LoudnessNRT")})
	{
		if (!IsClassLoaded(Class))
		{
			AddInfo(FString::Printf(TEXT("%s is not loaded; skipped."), Class));
			continue;
		}
		const FTarget Target(TEXT("Subclass"));
		ON_SCOPE_EXIT { Target.Cleanup(); };
		const FAssetDocumentResult Created = ApplyJson(*this, Target, Target.Document(Class, TEXT("Create"), TEXT("{}")), Class);
		const UObject* Asset = FSoftObjectPath(Target.ObjectPath).ResolveObject();
		TestTrue(FString::Printf(TEXT("%s is created through its ancestor factory"), Class), Created.IsSuccess());
		TestTrue(FString::Printf(TEXT("%s has the exact class"), Class), Asset != nullptr && Asset->GetClass()->GetPathName() == Class);
	}

	// A class whose own factory is switched off (UExternalDataLayerFactory::CanCreateNew() without its experimental
	// setting) is not created through an ancestor factory either: the editor offers no way to create it.
	TestCreateRefused(*this, TEXT("EDL"), TEXT("/Script/Engine.ExternalDataLayerAsset"), TEXT("{}"),
		AssetDocumentEditorFactoryDisabledCode, TEXT("ExternalDataLayerFactory"));
	// A factory that makes Blueprints of its supported class (UNamingTokensFactory) does not create a plain
	// subclass asset: the editor makes none.
	if (IsClassLoaded(TEXT("/Script/NamingTokens.GlobalNamingTokens")))
	{
		TestCreateRefused(*this, TEXT("Tokens"), TEXT("/Script/NamingTokens.GlobalNamingTokens"), TEXT("{}"),
			AssetDocumentEditorBlueprintClassUnsupportedCode, TEXT("creates a Blueprint"));
	}
	// The Interchange Pipeline factory's dialog is a class picker that keeps the class in unreflected state
	// (AssetDocumentEditorClassPickerFactories.cpp): the document's Class is the picked class; sub-pipelines the
	// picker hides (HideDropDown) are refused.
	if (IsClassLoaded(TEXT("/Script/InterchangePipelines.InterchangeGenericAssetsPipeline")))
	{
		const TCHAR* Class = TEXT("/Script/InterchangePipelines.InterchangeGenericAssetsPipeline");
		const FTarget Target(TEXT("Pipeline"));
		ON_SCOPE_EXIT { Target.Cleanup(); };
		const FAssetDocumentResult Created = ApplyJson(*this, Target, Target.Document(Class, TEXT("Create"), TEXT("{}")), Class);
		const UObject* Asset = FSoftObjectPath(Target.ObjectPath).ResolveObject();
		TestTrue(TEXT("the picked pipeline class is created"), Created.IsSuccess());
		TestTrue(TEXT("the pipeline has the picked class"), Asset != nullptr && Asset->GetClass()->GetPathName() == Class);
		TestCreateRefused(*this, TEXT("SubPipeline"), TEXT("/Script/InterchangePipelines.InterchangeGenericMeshPipeline"),
			TEXT("{}"), TEXT("FactoryCreate.DialogFactoryInitFailed"), TEXT("class picker"));
	}

	// Exact factories name the New-asset dialog field a FactoryContext fills. (The blend space,
	// pose asset and composite classes have their own profiles, which pass the same factory
	// context: UEAssetDoc.AnimationAssets.*.)
	// A class context (the New State Tree dialog's schema class picker, which StateTree documents
	// supply as Body.Schema.Class) fills the single unset class setting whose meta class accepts it;
	// abstract classes (the picker hides them), classes the meta class refuses and settings already
	// made are refused.
	UClass* StateTreeFactoryClass = FindObject<UClass>(nullptr, TEXT("/Script/StateTreeEditorModule.StateTreeFactory"));
	UClass* SchemaBaseClass = FindObject<UClass>(nullptr, TEXT("/Script/StateTreeModule.StateTreeSchema"));
	UClass* ComponentSchemaClass = FindObject<UClass>(nullptr, TEXT("/Script/GameplayStateTreeModule.StateTreeComponentSchema"));
	UClass* DataAssetFactoryClass = FindObject<UClass>(nullptr, TEXT("/Script/UnrealEd.DataAssetFactory"));
	if (StateTreeFactoryClass != nullptr && SchemaBaseClass != nullptr && ComponentSchemaClass != nullptr && DataAssetFactoryClass != nullptr)
	{
		const FClassProperty* SchemaSetting = CastField<FClassProperty>(StateTreeFactoryClass->FindPropertyByName(TEXT("StateTreeSchemaClass")));
		const TStrongObjectPtr<UFactory> StateTreeFactory = NewAssetDocumentEditorGenericFactoryInstance(*StateTreeFactoryClass);
		const TStrongObjectPtr<UFactory> DataAssetFactory = NewAssetDocumentEditorGenericFactoryInstance(*DataAssetFactoryClass);
		auto Configure = [](UFactory& Factory, UObject& Context)
		{
			const TCHAR* Code = ConfigureAssetDocumentEditorGenericFactoryContext(Factory, Context);
			return FString(Code != nullptr ? Code : TEXT(""));
		};
		if (TestNotNull(TEXT("schema class setting exists"), SchemaSetting)
			&& TestTrue(TEXT("class context factories"), StateTreeFactory.IsValid() && DataAssetFactory.IsValid()))
		{
			TestEqual(TEXT("abstract class context is refused"), Configure(*StateTreeFactory.Get(), *SchemaBaseClass),
				FString(TEXT("FactoryCreate.ContextUnsupported")));
			TestEqual(TEXT("class outside the meta class is refused"), Configure(*DataAssetFactory.Get(), *ComponentSchemaClass),
				FString(TEXT("FactoryCreate.ContextUnsupported")));
			TestEqual(TEXT("schema class context is accepted"), Configure(*StateTreeFactory.Get(), *ComponentSchemaClass), FString());
			TestTrue(TEXT("setting holds the schema class"), SchemaSetting->GetObjectPropertyValue_InContainer(StateTreeFactory.Get()) == ComponentSchemaClass);
			TestNull(TEXT("factory CDO untouched"), SchemaSetting->GetObjectPropertyValue_InContainer(StateTreeFactoryClass->GetDefaultObject()));
			TestEqual(TEXT("a made setting is not overwritten"), Configure(*StateTreeFactory.Get(), *ComponentSchemaClass),
				FString(TEXT("FactoryCreate.ContextUnsupported")));
		}
	}
	else
	{
		AddInfo(TEXT("StateTree factory or schema classes are not loaded; class context skipped."));
	}
	return true;
}

namespace AssetDocumentEditorGenericRobustnessTests
{
// Packages a durable apply loaded from its staging directory (Saved/AssetDocument/Runtime/Staging
// is mounted as /Temp/AssetDocument/Runtime/Staging).
const TCHAR* const StagingRoot = TEXT("/Temp/AssetDocument/Runtime/Staging/");

int32 CountStagingPackages()
{
	int32 Count = 0;
	for (TObjectIterator<UPackage> It; It; ++It)
	{
		Count += It->GetName().StartsWith(StagingRoot) ? 1 : 0;
	}
	return Count;
}

// What a provider walking loaded objects would see of the staged copies (TObjectIterator includes
// objects marked garbage until they are collected).
TArray<FString> StagedObjectsOfClass(UClass& Class)
{
	TArray<FString> Found;
	for (FThreadSafeObjectIterator It(&Class); It; ++It)
	{
		if (It->GetOutermost()->GetName().StartsWith(StagingRoot))
		{
			Found.Add(It->GetPathName());
		}
	}
	return Found;
}

// Records the staged copies the applies load for their fresh verification.
struct FStagedLoads
{
	TSet<FString> Packages;
	FDelegateHandle Handle;
	FStagedLoads()
	{
		Handle = FCoreUObjectDelegates::OnEndLoadPackage.AddLambda([this](const FEndLoadPackageContext& Context)
		{
			for (const UPackage* Package : Context.LoadedPackages)
			{
				if (Package != nullptr && Package->GetName().StartsWith(StagingRoot))
				{
					Packages.Add(Package->GetName());
				}
			}
		});
	}
	~FStagedLoads()
	{
		FCoreUObjectDelegates::OnEndLoadPackage.Remove(Handle);
	}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericRobustnessStagingPackagesReleasedTest,
	"UEAssetDoc.GenericRuntime.Robustness.StagingPackagesReleased",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericRobustnessStagingPackagesReleasedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericRobustnessTests;
	const TCHAR* LibraryClass = TEXT("/Script/Engine.ObjectLibrary");
	const FTarget Referenced(TEXT("Staging_Referenced"));
	const FTarget Library(TEXT("Staging_Library"));
	const FTarget Blueprint(TEXT("Staging_Blueprint"));
	ON_SCOPE_EXIT
	{
		Blueprint.Cleanup();
		Library.Cleanup();
		Referenced.Cleanup();
	};
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestEqual(TEXT("no staged copy is loaded before the applies"), CountStagingPackages(), 0);
	FStagedLoads Loads;

	// A library holding a hard reference to another saved asset; every apply stages, loads and
	// verifies a copy of it, and that copy references the other asset too.
	if (!TestTrue(TEXT("referenced asset created"), ApplyJson(*this, Referenced,
			Referenced.Document(LibraryClass, TEXT("Create"), TEXT("{}")), TEXT("referenced")).IsSuccess())
		|| !TestTrue(TEXT("library created"), ApplyJson(*this, Library, Library.Document(LibraryClass, TEXT("Create"),
			FString::Printf(TEXT("{\"Objects\":[\"%s\"]}"), *Referenced.ObjectPath)), TEXT("library")).IsSuccess()))
	{
		return false;
	}
	for (int32 Index = 0; Index < 3; ++Index)
	{
		const FString Objects = Index % 2 == 0
			? FString::Printf(TEXT("[\"%s\",\"%s\"]"), *Referenced.ObjectPath, *Referenced.ObjectPath)
			: FString::Printf(TEXT("[\"%s\"]"), *Referenced.ObjectPath);
		const FAssetDocumentResult Updated = ApplyJson(*this, Library,
			Library.Document(LibraryClass, TEXT("Update"), FString::Printf(TEXT("{\"Objects\":%s}"), *Objects)), TEXT("update"));
		TestTrue(FString::Printf(TEXT("update %d applies and saves"), Index), Updated.IsSuccess() && Updated.bSavedAsset);
	}
	TestTrue(FString::Printf(TEXT("each apply loaded its staged copy (%d)"), Loads.Packages.Num()), Loads.Packages.Num() >= 5);
	// Collected when the apply returns, not left for a later garbage collection: no provider or
	// editor-wide iterator (dependents, list_types, an enumerator edit's struct recompile) sees them.
	TestEqual(TEXT("after successful applies: no staged copy is loaded"), CountStagingPackages(), 0);
	TestEqual(TEXT("after successful applies: no object of a staged copy is reachable"),
		FString::Join(StagedObjectsOfClass(*UObject::StaticClass()), TEXT(", ")), FString());

	// A Blueprint's staged copy carries its own generated class: no class picker or Blueprint
	// dependents walk sees it.
	const int32 LoadsBeforeBlueprint = Loads.Packages.Num();
	const FString BlueprintDocument = FString::Printf(
		TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"Create\",")
		TEXT("\"Properties\":{},\"Body\":{\"ParentClass\":\"/Script/Engine.Actor\"}}"), *Blueprint.PackageName);
	if (TestTrue(TEXT("Blueprint created"), ApplyJson(*this, Blueprint, BlueprintDocument, TEXT("blueprint")).IsSuccess()))
	{
		TestTrue(TEXT("the Blueprint apply loaded its staged copy"), Loads.Packages.Num() > LoadsBeforeBlueprint);
		TestEqual(TEXT("no staged Blueprint class is loaded"), FString::Join(StagedObjectsOfClass(*UClass::StaticClass()), TEXT(", ")), FString());
		TestEqual(TEXT("no staged Blueprint is loaded"), FString::Join(StagedObjectsOfClass(*UBlueprint::StaticClass()), TEXT(", ")), FString());
	}

	// An apply that fails after its fresh verification (the install cannot write the asset folder)
	// rolls back; the staged copy it loaded is collected too.
	{
		const int32 LoadsBeforeFailure = Loads.Packages.Num();
		IPlatformFile& Files = FPlatformFileManager::Get().GetPlatformFile();
		const FString AssetFolder = FPaths::ConvertRelativePathToFull(FPaths::GetPath(Library.PackageFile));
		const bool bBlocked = AssetDocumentTestDirectoryBlock::SetDirectoryReadOnly(Files, *AssetFolder, true);
		FAssetDocumentResult Failed;
		{
			ON_SCOPE_EXIT
			{
				if (bBlocked)
				{
					AssetDocumentTestDirectoryBlock::SetDirectoryReadOnly(Files, *AssetFolder, false);
				}
			};
			if (TestTrue(TEXT("asset folder blocked"), bBlocked))
			{
				Failed = ApplyJson(*this, Library, Library.Document(LibraryClass, TEXT("Update"),
					FString::Printf(TEXT("{\"Objects\":[\"%s\",\"%s\",\"%s\"]}"), *Referenced.ObjectPath, *Referenced.ObjectPath,
						*Referenced.ObjectPath)), TEXT("failed install"));
			}
		}
		TestFalse(TEXT("the apply fails at install and rolls back"), Failed.IsSuccess());
		TestTrue(TEXT("the failed apply loaded a staged copy"), Loads.Packages.Num() > LoadsBeforeFailure);
	}
	TestEqual(TEXT("after a failed apply: no staged copy is loaded"), CountStagingPackages(), 0);

	// Nothing the applies loaded keeps the referenced asset: once the library is unloaded, it unloads.
	UObject* const LibraryAsset = FSoftObjectPath(Library.ObjectPath).ResolveObject();
	const TWeakObjectPtr<UObject> ReferencedAsset = FSoftObjectPath(Referenced.ObjectPath).ResolveObject();
	if (!TestNotNull(TEXT("library loaded"), LibraryAsset) || !TestTrue(TEXT("referenced asset loaded"), ReferencedAsset.IsValid()))
	{
		return false;
	}
	if (GEditor != nullptr)
	{
		GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentRobustnessTests", "StagingRelease", "Staging release test"));
	}
	UPackageTools::UnloadPackages({LibraryAsset->GetOutermost()});
	UPackageTools::UnloadPackages({ReferencedAsset->GetOutermost()});
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestFalse(TEXT("the referenced asset unloads (no staged copy pins it)"), ReferencedAsset.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericRobustnessScratchClonePostLoadFlagsTest,
	"UEAssetDoc.GenericRuntime.Robustness.ScratchClonePostLoadFlags",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericRobustnessScratchClonePostLoadFlagsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericRobustnessTests;
	// Duplicating these assets for the preflight scratch routes their PostLoad, which sets
	// RF_Standalone; the coverage audit saw every edit refused with Scratch.CloneFailed.
	struct FCase
	{
		const TCHAR* Prefix;
		const TCHAR* Class;
		const TCHAR* Edit;
		const TCHAR* Expected;
	};
	const FCase Cases[] = {
		{TEXT("IKRig"), TEXT("/Script/IKRig.IKRigDefinition"), TEXT("{\"DrawGoals\":false,\"GoalSize\":7.5}"), TEXT("\"GoalSize\":7.5")},
		{TEXT("IKRetargeter"), TEXT("/Script/IKRig.IKRetargeter"), TEXT("{\"bDebugDraw\":false,\"TargetMeshScale\":1.5}"), TEXT("\"TargetMeshScale\":1.5")},
	};
	for (const FCase& Case : Cases)
	{
		if (!IsClassLoaded(Case.Class))
		{
			AddWarning(FString::Printf(TEXT("%s is not loaded (IKRig plugin disabled); skipped."), Case.Class));
			continue;
		}
		const FTarget Target(Case.Prefix);
		ON_SCOPE_EXIT { Target.Cleanup(); };
		// Both classes have their own Profile (AssetDocumentRig): its documents carry an (empty) Body instead of the
		// generic AssetType.
		auto RigDocument = [&Target, &Case](const TCHAR* Action, const TCHAR* Properties)
		{
			return Target.Document(Case.Class, Action, Properties).Replace(TEXT("\"AssetType\":\"GenericAsset\","), TEXT("\"Body\":{},"));
		};
		if (!TestTrue(FString::Printf(TEXT("%s is created"), Case.Class),
			ApplyJson(*this, Target, RigDocument(TEXT("Create"), TEXT("{}")), Case.Prefix).IsSuccess()))
		{
			continue;
		}
		const FAssetDocumentResult Edited = ApplyJson(*this, Target, RigDocument(TEXT("Update"), Case.Edit), TEXT("edit"));
		TestTrue(FString::Printf(TEXT("%s edit applies"), Case.Class), Edited.IsSuccess() && Edited.bSavedAsset);
		TestFalse(FString::Printf(TEXT("%s edit is not refused by the scratch clone"), Case.Class), HasCode(Edited, TEXT("Scratch.CloneFailed")));
		const FAssetDocumentResult Extracted = ExtractAll(Target);
		const FString ExtractJson = Extracted.Payload.IsValid() ? Serialize(Extracted.Payload) : FString();
		TestTrue(FString::Printf(TEXT("%s extract shows the edit"), Case.Class), ExtractJson.Contains(Case.Expected));
		if (!ExtractJson.IsEmpty())
		{
			const FAssetDocumentResult Reapplied = ApplyJson(*this, Target, ExtractJson, TEXT("unchanged re-apply"));
			TestTrue(FString::Printf(TEXT("%s extract re-applies"), Case.Class), Reapplied.IsSuccess());
			TestFalse(FString::Printf(TEXT("%s unchanged re-apply is a no-op"), Case.Class), Reapplied.bSavedAsset);
		}
		// The previews leave no standalone objects behind in the transient package.
		int32 StandaloneScratches = 0;
		ForEachObjectWithPackage(GetTransientPackage(), [&StandaloneScratches, &Case](UObject* Object)
		{
			StandaloneScratches += Object->GetClass()->GetPathName() == Case.Class && Object->HasAnyFlags(RF_Standalone) ? 1 : 0;
			return true;
		}, AssetDocumentObjects::DirectOnly);
		TestEqual(FString::Printf(TEXT("%s scratches are not standalone"), Case.Class), StandaloneScratches, 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericRobustnessScratchFactoryAssetFlagsTest,
	"UEAssetDoc.GenericRuntime.Robustness.ScratchFactoryAssetFlags",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericRobustnessScratchFactoryAssetFlagsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericRobustnessTests;
	// These factories add RF_Public | RF_Standalone to what they create whatever flags they are given; the coverage
	// audit saw every create refused with ScratchCreate.InvalidIdentity. The preflight scratch they make in the
	// transient package is still created, edited and released.
	struct FCase
	{
		const TCHAR* Prefix;
		const TCHAR* Class;
	};
	const FCase Cases[] = {
		{TEXT("Solver"), TEXT("/Script/ChaosSolverEngine.ChaosSolver")},
		{TEXT("Chooser"), TEXT("/Script/Chooser.ChooserTable")},
		{TEXT("ProxyTable"), TEXT("/Script/ProxyTable.ProxyTable")},
	};
	int32 Ran = 0;
	for (const FCase& Case : Cases)
	{
		if (!IsClassLoaded(Case.Class))
		{
			AddInfo(FString::Printf(TEXT("%s is not loaded; skipped."), Case.Class));
			continue;
		}
		++Ran;
		const FTarget Target(Case.Prefix);
		ON_SCOPE_EXIT { Target.Cleanup(); };
		const FAssetDocumentResult Created =
			ApplyJson(*this, Target, Target.Document(Case.Class, TEXT("Create"), TEXT("{}")), Case.Prefix);
		TestTrue(FString::Printf(TEXT("%s is created"), Case.Class), Created.IsSuccess());
		TestFalse(FString::Printf(TEXT("%s scratch identity is accepted"), Case.Class),
			HasCode(Created, TEXT("ScratchCreate.InvalidIdentity")));
		const UObject* Asset = FSoftObjectPath(Target.ObjectPath).ResolveObject();
		TestTrue(FString::Printf(TEXT("%s has the exact class"), Case.Class),
			Asset != nullptr && Asset->GetClass()->GetPathName() == Case.Class);
		const FAssetDocumentResult Extracted = Created.IsSuccess() ? ExtractAll(Target) : FAssetDocumentResult();
		const FString ExtractJson = Extracted.Payload.IsValid() ? Serialize(Extracted.Payload) : FString();
		if (TestFalse(FString::Printf(TEXT("%s extracts"), Case.Class), ExtractJson.IsEmpty()))
		{
			const FAssetDocumentResult Reapplied = ApplyJson(*this, Target, ExtractJson, TEXT("unchanged re-apply"));
			TestTrue(FString::Printf(TEXT("%s extract re-applies"), Case.Class), Reapplied.IsSuccess());
			TestFalse(FString::Printf(TEXT("%s unchanged re-apply is a no-op"), Case.Class), Reapplied.bSavedAsset);
		}
		// The scratches are released: none is left standalone or public in the transient package.
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		int32 KeptScratches = 0;
		ForEachObjectWithPackage(GetTransientPackage(), [&KeptScratches, &Case](UObject* Object)
		{
			KeptScratches += Object->GetClass()->GetPathName() == Case.Class && Object->HasAnyFlags(RF_Standalone | RF_Public) ? 1 : 0;
			return true;
		}, AssetDocumentObjects::DirectOnly);
		TestEqual(FString::Printf(TEXT("%s scratches are not kept as assets"), Case.Class), KeptScratches, 0);
	}
	TestTrue(TEXT("at least one factory that adds asset flags is loaded"), Ran > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericRobustnessStructWriteKeepsNativeStateTest,
	"UEAssetDoc.GenericRuntime.Robustness.StructWriteKeepsNativeState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericRobustnessStructWriteKeepsNativeStateTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericRobustnessTests;
	// UGeometryCollection::DataflowInstance (FDataflowInstance) registers a raw owner-change delegate in its
	// constructor and keeps the handle in native state. A struct value written by C++ assignment replaces that
	// handle (and the owner), so the asset's binding outlives its handle and dangles once the asset is unloaded.
	// Struct values are written member by member, as the Details panel does. (Preview scratches: see
	// ScratchReleasesNativeBindings.)
	const TCHAR* Class = TEXT("/Script/GeometryCollectionEngine.GeometryCollection");
	if (!IsClassLoaded(Class))
	{
		AddInfo(TEXT("GeometryCollectionEngine is not loaded; skipped."));
		return true;
	}
	const FTarget Target(TEXT("GC"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	if (!TestTrue(TEXT("geometry collection is created"),
		ApplyJson(*this, Target, Target.Document(Class, TEXT("Create"), TEXT("{}")), TEXT("create")).IsSuccess()))
	{
		return false;
	}
	const FAssetDocumentResult Edited = ApplyJson(*this, Target, Target.Document(Class, TEXT("Update"),
		TEXT("{\"DataflowInstance\":{\"DataflowTerminal\":\"AuditTerminal\"}}")), TEXT("struct edit"));
	TestTrue(TEXT("the struct member edit applies"), Edited.IsSuccess() && Edited.bSavedAsset);
	// The preview scratches are gone; a later change notification must not reach a stale binding.
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	const FAssetDocumentResult Second = ApplyJson(*this, Target, Target.Document(Class, TEXT("Update"),
		TEXT("{\"DataflowInstance\":{\"DataflowTerminal\":\"SecondTerminal\"},\"EnableClustering\":false}")), TEXT("second edit"));
	TestTrue(TEXT("a second edit applies after the scratches were collected"), Second.IsSuccess());
	const FString Extracted = Serialize(ExtractAll(Target).Payload);
	TestTrue(TEXT("the terminal reads back"), Extracted.Contains(TEXT("\"DataflowTerminal\":\"SecondTerminal\"")));
	TestTrue(TEXT("the instance keeps its owner"), Extracted.Contains(FString::Printf(TEXT("\"Owner\":\"%s\""), *Target.ObjectPath)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericRobustnessScratchReleasesNativeBindingsTest,
	"UEAssetDoc.GenericRuntime.Robustness.ScratchReleasesNativeBindings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericRobustnessScratchReleasesNativeBindingsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericRobustnessTests;
	// A duplicated FDataflowInstance owner holds the class default's delegate handle while its own raw
	// OnObjectPropertyChanged binding is held by nobody (engine: the copy is initialised by assignment from the
	// class default). The coverage audit crashed in the next change notification after a GeometryCollection preview
	// scratch was collected. Dropping a scratch payload detaches the scratch's binding and leaves the default's.
	const TCHAR* ClassPath = TEXT("/Script/GeometryCollectionEngine.GeometryCollection");
	if (!IsClassLoaded(ClassPath))
	{
		AddInfo(TEXT("GeometryCollectionEngine is not loaded; skipped."));
		return true;
	}
	UClass* const Class = FindObject<UClass>(nullptr, ClassPath);
	const FStructProperty* const Instance = Class != nullptr ? FindFProperty<FStructProperty>(Class, TEXT("DataflowInstance")) : nullptr;
	if (!TestNotNull(TEXT("GeometryCollection has a DataflowInstance struct"), Instance))
	{
		return false;
	}
	const FTarget Target(TEXT("GCScratch"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	if (!TestTrue(TEXT("geometry collection is created"),
		ApplyJson(*this, Target, Target.Document(ClassPath, TEXT("Create"), TEXT("{}")), TEXT("create")).IsSuccess()))
	{
		return false;
	}
	TStrongObjectPtr<UObject> Asset(StaticLoadObject(Class, nullptr, *Target.ObjectPath));
	if (!TestTrue(TEXT("the asset loads"), Asset.IsValid()))
	{
		return false;
	}
	const void* const DefaultValue = Instance->ContainerPtrToValuePtr<void>(Class->GetDefaultObject());
	const bool bDefaultBound = FCoreUObjectDelegates::OnObjectPropertyChanged.IsBoundToObject(DefaultValue);
	const void* ScratchValue = nullptr;
	{
		TStrongObjectPtr<UObject> Scratch(DuplicateObject<UObject>(Asset.Get(), GetTransientPackage(), NAME_None));
		ScratchValue = Instance->ContainerPtrToValuePtr<void>(Scratch.Get());
		TestTrue(TEXT("the duplicated instance is bound to property changes"),
			FCoreUObjectDelegates::OnObjectPropertyChanged.IsBoundToObject(ScratchValue));
		{
			const FAssetDocumentEditorScratchPayload Payload(*Scratch.Get());
		}
		TestFalse(TEXT("dropping the scratch detaches its binding"),
			FCoreUObjectDelegates::OnObjectPropertyChanged.IsBoundToObject(ScratchValue));
		Scratch->ClearFlags(RF_Public | RF_Standalone);
	}
	Asset.Reset();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestFalse(TEXT("no binding to the collected scratch remains"),
		FCoreUObjectDelegates::OnObjectPropertyChanged.IsBoundToObject(ScratchValue));
	TestEqual(TEXT("the class default keeps its binding"),
		FCoreUObjectDelegates::OnObjectPropertyChanged.IsBoundToObject(DefaultValue), bDefaultBound);
	// The real preview path: an update's validate trial clones the asset into a scratch and drops it.
	const FAssetDocumentResult Edited = ApplyJson(*this, Target, Target.Document(ClassPath, TEXT("Update"),
		TEXT("{\"DataflowInstance\":{\"DataflowTerminal\":\"ScratchTerminal\"}}")), TEXT("struct edit"));
	TestTrue(TEXT("the edit applies"), Edited.IsSuccess());
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestEqual(TEXT("the class default still keeps its binding after the preview scratch is collected"),
		FCoreUObjectDelegates::OnObjectPropertyChanged.IsBoundToObject(DefaultValue), bDefaultBound);
	return true;
}
#endif
