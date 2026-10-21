// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Validate of a new asset makes its trial target with the class's editor factory, as the Content Browser's New asset
// does. Editor.ValidateCreate.NoEnsure: for each class an exact profile creates, Validate of a minimal create document
// succeeds and logs no ensure and no error (factories that ensure the flags the Content Browser passes, such as
// UDataTableFactory's RF_Public, get them).

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"

#include "Dom/JsonObject.h"
#include "HAL/IConsoleManager.h"
#include "Logging/LogVerbosity.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorValidateCreateLogTests
{
const TCHAR* const Skeleton = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton");
const TCHAR* const SkeletalMesh = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP");
const TCHAR* const Sequence = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle.Tutorial_Idle");

// Errors and ensures logged while it is alive, from any thread.
class FErrorCapture final : public FOutputDevice
{
public:
	FErrorCapture() { GLog->AddOutputDevice(this); }
	virtual ~FErrorCapture() override { GLog->RemoveOutputDevice(this); }

	virtual bool CanBeUsedOnAnyThread() const override { return true; }
	virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

	virtual void Serialize(const TCHAR* Text, ELogVerbosity::Type Verbosity, const FName& Category) override
	{
		const ELogVerbosity::Type Level = static_cast<ELogVerbosity::Type>(Verbosity & ELogVerbosity::VerbosityMask);
		const FString Line(Text);
		if (Level == ELogVerbosity::Error || Level == ELogVerbosity::Fatal || Line.Contains(TEXT("Ensure condition failed")))
		{
			FScopeLock Lock(&Mutex);
			Lines.Add(FString::Printf(TEXT("%s: %s"), *Category.ToString(), *Line.Left(300)));
		}
	}

	TArray<FString> Take()
	{
		FScopeLock Lock(&Mutex);
		return MoveTemp(Lines);
	}

private:
	FCriticalSection Mutex;
	TArray<FString> Lines;
};

struct FCreateCase
{
	const TCHAR* Class;
	// The Body, and other top-level fields (with a leading comma).
	FString Body;
	FString Extra;
};

TArray<FCreateCase> Cases()
{
	const FString OnSkeleton = FString::Printf(TEXT("{\"Skeleton\":\"%s\"}"), Skeleton);
	const FString RowStruct = TEXT("{\"RowStruct\":\"/Script/GameplayTags.GameplayTagTableRow\"}");
	return {
		{TEXT("/Script/Engine.Blueprint"), TEXT("{\"ParentClass\":\"/Script/Engine.Actor\"}")},
		{TEXT("/Script/Engine.AnimBlueprint"), FString::Printf(TEXT("{\"ParentClass\":\"/Script/Engine.AnimInstance\",\"TargetSkeleton\":\"%s\"}"), Skeleton)},
		{TEXT("/Script/UMGEditor.WidgetBlueprint"), TEXT("{\"ParentClass\":\"/Script/UMG.UserWidget\"}")},
		{TEXT("/Script/ControlRigDeveloper.ControlRigBlueprint"), TEXT("{}")},
		{TEXT("/Script/Engine.AnimComposite"), OnSkeleton},
		{TEXT("/Script/Engine.AnimMontage"), FString::Printf(TEXT("{\"Skeleton\":\"%s\",\"SlotAnimTracks\":[]}"), Skeleton)},
		{TEXT("/Script/Engine.AnimSequence"), FString::Printf(TEXT("{\"References\":{\"Skeleton\":\"%s\"}}"), Skeleton)},
		{TEXT("/Script/Engine.BlendSpace"), OnSkeleton},
		{TEXT("/Script/Engine.BlendSpace1D"), OnSkeleton},
		{TEXT("/Script/Engine.AimOffsetBlendSpace"), OnSkeleton},
		{TEXT("/Script/Engine.AimOffsetBlendSpace1D"), OnSkeleton},
		{TEXT("/Script/Engine.PoseAsset"), FString::Printf(TEXT("{\"SourceAnimation\":\"%s\"}"), Sequence)},
		{TEXT("/Script/AIModule.BehaviorTree"), TEXT("{\"Tree\":{\"Root\":{}}}"), TEXT(",\"Definitions\":{},\"Properties\":{}")},
		{TEXT("/Script/AIModule.BlackboardData"), TEXT("{\"Parent\":null,\"Keys\":[]}"), TEXT(",\"Definitions\":{},\"Properties\":{}")},
		{TEXT("/Script/AIModule.EnvQuery"), TEXT("{}")},
		{TEXT("/Script/Engine.CurveFloat"), TEXT("{}")},
		{TEXT("/Script/Engine.DataTable"), RowStruct},
		{TEXT("/Script/Engine.CompositeDataTable"), RowStruct},
		{TEXT("/Script/Engine.CurveTable"), TEXT("{}")},
		{TEXT("/Script/Engine.CompositeCurveTable"), TEXT("{}")},
		{TEXT("/Script/Engine.StringTable"), TEXT("{}")},
		{TEXT("/Script/IKRig.IKRigDefinition"), TEXT("{}")},
		{TEXT("/Script/IKRig.IKRetargeter"), TEXT("{}")},
		{TEXT("/Script/Engine.Material"), TEXT("{}")},
		{TEXT("/Script/Engine.MaterialFunction"), TEXT("{}")},
		{TEXT("/Script/Engine.MaterialFunctionMaterialLayer"), TEXT("{}")},
		{TEXT("/Script/Engine.MaterialFunctionMaterialLayerBlend"), TEXT("{}")},
		{TEXT("/Script/Engine.MaterialInstanceConstant"), TEXT("{\"Parent\":\"/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial\"}")},
		{TEXT("/Script/MetasoundEngine.MetaSoundSource"), TEXT("{\"Interfaces\":[\"UE.Source.OneShot\"]}")},
		{TEXT("/Script/MetasoundEngine.MetaSoundPatch"), TEXT("{}")},
		{TEXT("/Script/Niagara.NiagaraSystem"), TEXT("{}")},
		{TEXT("/Script/Niagara.NiagaraEmitter"), TEXT("{}")},
		{TEXT("/Script/Niagara.NiagaraScript"), TEXT("{}")},
		{TEXT("/Script/Niagara.NiagaraParameterCollection"), TEXT("{}")},
		{TEXT("/Script/Engine.PhysicsAsset"), TEXT("{}"), FString::Printf(TEXT(",\"FactoryContext\":\"%s\""), SkeletalMesh)},
		{TEXT("/Script/LevelSequence.LevelSequence"), TEXT("{}")},
		{TEXT("/Script/TemplateSequence.TemplateSequence"), TEXT("{\"Root\":{\"Class\":\"/Script/Engine.Actor\"}}")},
		{TEXT("/Script/TemplateSequence.CameraAnimationSequence"), TEXT("{}")},
		{TEXT("/Script/Engine.SoundCue"), TEXT("{}")},
		{TEXT("/Script/StateTreeModule.StateTree"), TEXT("{\"Schema\":{\"Class\":\"/Script/GameplayStateTreeModule.StateTreeComponentSchema\"}}")},
		{TEXT("/Script/CoreUObject.UserDefinedStruct"), TEXT("{}")},
		{TEXT("/Script/Engine.UserDefinedEnum"), TEXT("{}")},
	};
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentEditorValidateCreateNoEnsureTest,
	"UEAssetDoc.Editor.ValidateCreate.NoEnsure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentEditorValidateCreateNoEnsureTest::RunTest(const FString&)
{
	using namespace AssetDocumentEditorValidateCreateLogTests;
	IConsoleObject* const ResetEnsures = IConsoleManager::Get().FindConsoleObject(TEXT("core.ResetEnsureState"));
	const FString Folder = FString::Printf(TEXT("/Game/AssetDocumentValidateCreate_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	for (const FCreateCase& Case : Cases())
	{
		if (FindObject<UClass>(nullptr, Case.Class) == nullptr)
		{
			AddInfo(FString::Printf(TEXT("%s: class not loaded (its plugin is off)"), Case.Class));
			continue;
		}
		// Every ensure reports again (an ensure fires once per session otherwise).
		if (ResetEnsures != nullptr && ResetEnsures->AsCommand() != nullptr)
		{
			ResetEnsures->AsCommand()->Execute({}, nullptr, *GLog);
		}
		const FString Short = FString(Case.Class).RightChop(FString(Case.Class).Find(TEXT(".")) + 1);
		const FString Json = FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s/%s_New\",\"Class\":\"%s\",\"Action\":\"Create\",\"Body\":%s%s}"),
			*Folder, *Short, Case.Class, *Case.Body, *Case.Extra);
		FAssetDocumentValidateRequest Request;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Request.Document);
		if (!TestTrue(FString::Printf(TEXT("%s: document parses"), *Short), Request.Document.IsValid()))
		{
			continue;
		}
		TArray<FString> Logged;
		FAssetDocumentResult Result;
		{
			FErrorCapture Capture;
			Result = FAssetDocumentModule::Get().GetService().Validate(Request);
			Logged = Capture.Take();
		}
		TArray<FString> Codes;
		for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
		{
			Codes.Add(FString::Printf(TEXT("%s %s: %s"), *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message.Left(200)));
		}
		TestTrue(FString::Printf(TEXT("%s: Validate of a create succeeds (%s; %s)"), *Short, *Result.Message, *FString::Join(Codes, TEXT(" | "))),
			Result.IsSuccess());
		TestTrue(FString::Printf(TEXT("%s: no ensure or error is logged (%s)"), *Short, *FString::Join(Logged, TEXT(" | "))), Logged.IsEmpty());
	}
	return true;
}
#endif
