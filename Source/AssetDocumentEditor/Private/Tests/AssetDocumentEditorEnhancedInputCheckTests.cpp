// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// The generic profile's Enhanced Input checks (editor.enhanced_input.check_domain.v1): warnings
// from Validate and Apply, never refusals (the editor allows all of these):
//   - a Swizzle modifier on a Boolean / Axis1D action (mapping or action modifiers): the action reads only X;
//   - a mapping without an action;
//   - an Input Action ValueType change while loaded Blueprint nodes bind the action with the old value type, and a
//     mapping context whose action no longer matches the Blueprint nodes using it.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorEnhancedInputCheckTests
{
const TCHAR* InputActionClass = TEXT("/Script/EnhancedInput.InputAction");
const TCHAR* MappingContextClass = TEXT("/Script/EnhancedInput.InputMappingContext");
const TCHAR* Swizzle = TEXT("{\"Class\":\"/Script/EnhancedInput.InputModifierSwizzleAxis\",\"Properties\":{}}");

struct FInputTarget
{
	FString AssetName;
	FString PackageName;
	FString ObjectPath;
	FString PackageFile;
	FString SidecarPath;

	FInputTarget(const FString& Folder, const TCHAR* Name)
		: AssetName(Name)
	{
		PackageName = Folder / AssetName;
		ObjectPath = PackageName + TEXT(".") + AssetName;
		PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		SidecarPath = FPaths::Combine(FPaths::ProjectContentDir(), Folder.RightChop(6), AssetName + TEXT(".assetdoc.json"));
	}

	UObject* Get() const { return FSoftObjectPath(ObjectPath).ResolveObject(); }

	FString Generic(const TCHAR* Class, const FString& Properties) const
	{
		return FString::Printf(TEXT("{\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\",\"Target\":\"%s\",\"Class\":\"%s\",")
			TEXT("\"Action\":\"CreateOrUpdate\",\"Properties\":%s}"), *PackageName, Class, *Properties);
	}

	void Cleanup() const
	{
		if (UObject* Asset = Get())
		{
			if (IFileManager::Get().FileExists(*PackageFile))
			{
				UPackageTools::UnloadPackages({Asset->GetOutermost()});
			}
			else
			{
				Asset->GetOutermost()->ClearDirtyFlag();
				ObjectTools::DeleteObjects({Asset}, false, ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
			}
		}
		if (IFileManager::Get().FileExists(*PackageFile))
		{
			IFileManager::Get().Delete(*PackageFile, false, true);
			FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({PackageFile});
		}
		if (UPackage* Stale = FindPackage(nullptr, *PackageName))
		{
			Stale->ClearDirtyFlag();
			Stale->MarkAsGarbage();
		}
		IFileManager::Get().Delete(*SidecarPath, false, true);
	}
};

void Report(FAutomationTestBase& Test, const TCHAR* Stage, const FAssetDocumentResult& Result)
{
	Test.AddInfo(FString::Printf(TEXT("%s: %s"), Stage, *Result.Message));
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Test.AddInfo(FString::Printf(TEXT("%s: [%s %s] %s %s"), Stage, *Diagnostic.Severity, *Diagnostic.Code, *Diagnostic.Path,
			*Diagnostic.Message));
	}
}

FAssetDocumentResult Apply(FAutomationTestBase& Test, const FInputTarget& Target, const FString& Json, const TCHAR* Stage)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target.SidecarPath), true);
	if (!Test.TestTrue(TEXT("writes sidecar"), FFileHelper::SaveStringToFile(Json, *Target.SidecarPath)))
	{
		return FAssetDocumentResult::Failure(TEXT("sidecar not written"));
	}
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Target.SidecarPath;
	Request.bSaveAsset = true;
	FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
	Report(Test, Stage, Result);
	return Result;
}

FAssetDocumentResult Validate(FAutomationTestBase& Test, const FString& Json, const TCHAR* Stage)
{
	FAssetDocumentValidateRequest Request;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Request.Document);
	FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Validate(Request);
	Report(Test, Stage, Result);
	return Result;
}

int32 CountWarnings(const FAssetDocumentResult& Result, const TCHAR* Code, const TCHAR* PathPart = TEXT(""),
	const TCHAR* MessagePart = TEXT(""))
{
	int32 Count = 0;
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Count += Diagnostic.Code == Code && Diagnostic.Severity.Equals(TEXT("warning"), ESearchCase::IgnoreCase)
			&& Diagnostic.Path.Contains(PathPart) && Diagnostic.Message.Contains(MessagePart);
	}
	return Count;
}

// Validate and Apply (when bApply) both succeed with exactly the expected warnings (count per code; others none).
void TestWarns(FAutomationTestBase& Test, const FInputTarget& Target, const FString& Json, bool bApply, const TCHAR* Stage,
	TFunctionRef<void(const FAssetDocumentResult&, const FString&)> Check)
{
	const FAssetDocumentResult Checked = Validate(Test, Json, Stage);
	Test.TestTrue(*FString::Printf(TEXT("%s: Validate succeeds"), Stage), Checked.IsSuccess());
	Check(Checked, FString::Printf(TEXT("%s (validate)"), Stage));
	if (bApply)
	{
		const FAssetDocumentResult Applied = Apply(Test, Target, Json, Stage);
		Test.TestTrue(*FString::Printf(TEXT("%s: Apply succeeds"), Stage), Applied.IsSuccess());
		Check(Applied, FString::Printf(TEXT("%s (apply)"), Stage));
	}
}
} // namespace AssetDocumentEditorEnhancedInputCheckTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentEnhancedInputDomainWarningsTest,
	"UEAssetDoc.GenericProvider.EnhancedInput.DomainWarnings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEnhancedInputDomainWarningsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorEnhancedInputCheckTests;
	if (FindObject<UClass>(nullptr, MappingContextClass) == nullptr)
	{
		AddError(TEXT("Enhanced Input is not loaded"));
		return false;
	}
	const FString Folder = FString(TEXT("/Game/AssetDocumentEnhancedInputChecks/"))
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(10);
	const FInputTarget Move(Folder, TEXT("IA_Move"));
	const FInputTarget Sprint(Folder, TEXT("IA_Sprint"));
	const FInputTarget Context(Folder, TEXT("IMC_Default"));
	const FInputTarget Pawn(Folder, TEXT("BP_InputPawn"));
	ON_SCOPE_EXIT
	{
		if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting && !GEditor->IsTransactionActive())
		{
			GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentEnhancedInputCheckTests", "Cleanup", "Enhanced Input check test cleanup"));
		}
		Pawn.Cleanup();
		Context.Cleanup();
		Sprint.Cleanup();
		Move.Cleanup();
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	};
	if (!TestTrue(TEXT("IA_Move (Axis2D) applies"), Apply(*this, Move,
			Move.Generic(InputActionClass, TEXT("{\"ValueType\":\"Axis2D\"}")), TEXT("IA_Move")).IsSuccess())
		|| !TestTrue(TEXT("IA_Sprint (Boolean) applies"), Apply(*this, Sprint,
			Sprint.Generic(InputActionClass, TEXT("{\"ValueType\":\"Boolean\"}")), TEXT("IA_Sprint")).IsSuccess()))
	{
		return false;
	}
	// A Blueprint binds IA_Move: its event's ActionValue is a Vector2D.
	const FString PawnJson = FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",")
		TEXT("\"Action\":\"Create\",\"Body\":{\"ParentClass\":\"/Script/Engine.Pawn\",\"UbergraphPages\":[{\"Name\":\"EventGraph\",")
		TEXT("\"Nodes\":[{\"Id\":\"Move\",\"Action\":\"K2:EnhancedInputAction:%s\"}]}]}}"), *Pawn.PackageName, *Move.ObjectPath);
	if (!TestTrue(TEXT("BP_InputPawn applies"), Apply(*this, Pawn, PawnJson, TEXT("BP_InputPawn")).IsSuccess()))
	{
		return false;
	}

	// A Swizzle on the Boolean LeftShift -> IA_Sprint mapping; a Swizzle on the 2D action is fine.
	const FString ContextJson = Context.Generic(MappingContextClass, FString::Printf(
		TEXT("{\"DefaultKeyMappings\":{\"Mappings\":[{\"Action\":\"%s\",\"Key\":\"W\",\"Modifiers\":[%s]},")
		TEXT("{\"Action\":\"%s\",\"Key\":\"LeftShift\",\"Modifiers\":[%s]},{\"Key\":\"E\"}]}}"),
		*Move.ObjectPath, Swizzle, *Sprint.ObjectPath, Swizzle));
	TestWarns(*this, Context, ContextJson, true, TEXT("mapping context"), [this](const FAssetDocumentResult& Result, const FString& Stage)
	{
		TestEqual(*(Stage + TEXT(": the Boolean mapping's Swizzle warns")), CountWarnings(Result, TEXT("EnhancedInput.ModifierDropsValue"),
			TEXT("DefaultKeyMappings/Mappings/1/Modifiers/0"), TEXT("Boolean")), 1);
		TestEqual(*(Stage + TEXT(": the Axis2D mapping's Swizzle does not")), CountWarnings(Result, TEXT("EnhancedInput.ModifierDropsValue")), 1);
		TestEqual(*(Stage + TEXT(": the mapping without an action warns")), CountWarnings(Result, TEXT("EnhancedInput.MappingWithoutAction"),
			TEXT("DefaultKeyMappings/Mappings/2/Action")), 1);
		TestEqual(*(Stage + TEXT(": the Blueprint matches IA_Move")), CountWarnings(Result, TEXT("EnhancedInput.BlueprintValueTypeMismatch")), 0);
	});

	// An action's own Swizzle on a Boolean action.
	TestWarns(*this, Sprint, Sprint.Generic(InputActionClass, FString::Printf(TEXT("{\"ValueType\":\"Boolean\",\"Modifiers\":[%s]}"), Swizzle)),
		false, TEXT("action modifier"), [this](const FAssetDocumentResult& Result, const FString& Stage)
	{
		TestEqual(*(Stage + TEXT(": the action's Swizzle warns")), CountWarnings(Result, TEXT("EnhancedInput.ModifierDropsValue"),
			TEXT("/Properties/Modifiers/0"), TEXT("IA_Sprint")), 1);
	});

	// IA_Move's ValueType changed to Boolean under the Blueprint's Vector2D event.
	TestWarns(*this, Move, Move.Generic(InputActionClass, TEXT("{\"ValueType\":\"Boolean\"}")), true, TEXT("value type change"),
		[this](const FAssetDocumentResult& Result, const FString& Stage)
	{
		TestEqual(*(Stage + TEXT(": the Blueprint binding the action is named")), CountWarnings(Result,
			TEXT("EnhancedInput.BlueprintValueTypeMismatch"), TEXT("/Properties/ValueType"), TEXT("BP_InputPawn")), 1);
	});
	// The mapping context now maps IA_Move (Boolean) used by the stale Blueprint node; its W Swizzle now drops the value.
	TestWarns(*this, Context, ContextJson, false, TEXT("mapping context after the change"),
		[this](const FAssetDocumentResult& Result, const FString& Stage)
	{
		TestEqual(*(Stage + TEXT(": the stale Blueprint is named at the mapping")), CountWarnings(Result,
			TEXT("EnhancedInput.BlueprintValueTypeMismatch"), TEXT("DefaultKeyMappings/Mappings/0/Action"), TEXT("BP_InputPawn")), 1);
		TestEqual(*(Stage + TEXT(": both Swizzles warn now")), CountWarnings(Result, TEXT("EnhancedInput.ModifierDropsValue")), 2);
	});
	return true;
}
#endif
