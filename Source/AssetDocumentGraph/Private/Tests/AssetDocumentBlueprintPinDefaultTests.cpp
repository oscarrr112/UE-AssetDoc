// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Graph edits Apply refuses are refused by Validate too, with the same code, on the production route.
//   ByRefDefault    A literal PinOverride on a by-ref input (the Blackboard's SetValueAsObject KeyName) fails Validate
//                   and Apply alike, with Graph.PinDefaultInvalid at the override and the Make Literal node to link
//                   instead; that node linked into the pin validates and applies.
//   LinkDisallowed  A link between pins the editor never joins (a Vector into a Branch's Condition) fails Validate
//                   and Apply alike, with Graph.ConnectDisallowed at the link.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentBlueprintPinDefaultTests
{
const TCHAR* const MakeLiteralNameKey = TEXT("K2:CallFunction:/Script/Engine.KismetSystemLibrary:MakeLiteralName");

struct FPinDefaultTarget
{
	FString PackageName;
	FString PackageFile;
	FString SidecarPath;
	FString Folder;

	FPinDefaultTarget()
		: Folder(FString(TEXT("AssetDocumentPinDefault/")) + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12))
	{
		PackageName = FString(TEXT("/Game/")) / Folder / TEXT("BP_KeyWriter");
		PackageFile = FPaths::ConvertRelativePathToFull(
			FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()));
		SidecarPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectContentDir(), Folder, TEXT("BP_KeyWriter.assetdoc.json")));
	}

	~FPinDefaultTarget()
	{
		if (UPackage* Package = FindPackage(nullptr, *PackageName))
		{
			Package->SetDirtyFlag(false);
			UPackageTools::UnloadPackages({Package});
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		IFileManager::Get().Delete(*PackageFile, false, true);
		IFileManager::Get().Delete(*SidecarPath, false, true);
		FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({PackageFile});
		IFileManager::Get().DeleteDirectory(*FPaths::Combine(FPaths::ProjectContentDir(), Folder), false, true);
	}

	// BeginPlay -> SetValueAsObject on the Blueprint's blackboard, its KeyName given as a literal or by a
	// Make Literal Name node.
	FString Document(bool bLinkedKey) const
	{
		const FString KeyNode = bLinkedKey
			? FString::Printf(TEXT(",{\"Id\":\"Key\",\"Action\":\"%s\",\"PinOverrides\":[{\"Pin\":\"Value\",\"DefaultValue\":\"TargetActor\"}]}"), MakeLiteralNameKey)
			: FString();
		const FString KeyOverride = bLinkedKey ? FString() : FString(TEXT(",\"PinOverrides\":[{\"Pin\":\"KeyName\",\"DefaultValue\":\"TargetActor\"}]"));
		const FString KeyLink = bLinkedKey ? FString(TEXT(",{\"From\":\"Key:ReturnValue\",\"To\":\"Set:KeyName\"}")) : FString();
		return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"Create\",")
			TEXT("\"Body\":{\"ParentClass\":\"/Script/Engine.Actor\",\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":[")
			TEXT("{\"Id\":\"Begin\",\"Action\":\"K2:BeginPlay\"},")
			TEXT("{\"Id\":\"Board\",\"Action\":\"K2:CallFunction:/Script/AIModule.AIBlueprintHelperLibrary:GetBlackboard\"},")
			TEXT("{\"Id\":\"Set\",\"Action\":\"K2:CallFunction:/Script/AIModule.BlackboardComponent:SetValueAsObject\"%s}%s],")
			TEXT("\"Links\":[{\"From\":\"Begin:then\",\"To\":\"Set:execute\"},{\"From\":\"Board:ReturnValue\",\"To\":\"Set:self\"}%s]}]}}"),
			*PackageName, *KeyOverride, *KeyNode, *KeyLink);
	}
};

void Report(FAutomationTestBase& Test, const FString& Stage, const FAssetDocumentResult& Result)
{
	Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *Result.Message));
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Test.AddInfo(FString::Printf(TEXT("%s: [%s] %s %s %s"), *Stage, *Diagnostic.Severity, *Diagnostic.Code, *Diagnostic.Path,
			*Diagnostic.Message));
	}
}

FAssetDocumentResult Validate(FAutomationTestBase& Test, const FString& Json, const FString& Stage)
{
	FAssetDocumentValidateRequest Request;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Request.Document);
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Validate(Request);
	Report(Test, Stage, Result);
	return Result;
}

FAssetDocumentResult Apply(FAutomationTestBase& Test, const FPinDefaultTarget& Target, const FString& Json, const FString& Stage)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target.SidecarPath), true);
	FFileHelper::SaveStringToFile(Json, *Target.SidecarPath);
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Target.SidecarPath;
	Request.bSaveAsset = true;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
	Report(Test, Stage, Result);
	return Result;
}

// The diagnostic with the code, if any.
const FAssetDocumentDiagnostic* FindCode(const FAssetDocumentResult& Result, const TCHAR* Code)
{
	return Result.Diagnostics.FindByPredicate([Code](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == Code; });
}

// Graph.PinDefaultInvalid at the Set node's override, naming the Make Literal Name node to link instead.
void ExpectRefused(FAutomationTestBase& Test, const FString& Stage, const FAssetDocumentResult& Result)
{
	Test.TestFalse(*FString::Printf(TEXT("%s: refused"), *Stage), Result.IsSuccess());
	const FAssetDocumentDiagnostic* Found = FindCode(Result, TEXT("Graph.PinDefaultInvalid"));
	if (!Test.TestNotNull(*FString::Printf(TEXT("%s: Graph.PinDefaultInvalid"), *Stage), Found))
	{
		return;
	}
	Test.TestTrue(*FString::Printf(TEXT("%s: at the override (%s)"), *Stage, *Found->Path),
		Found->Path.EndsWith(TEXT("/UbergraphPages/0/Nodes/2/PinOverrides/0")));
	Test.TestTrue(*FString::Printf(TEXT("%s: names the Make Literal Name node and its link"), *Stage),
		Found->Message.Contains(MakeLiteralNameKey) && Found->Message.Contains(TEXT("\"To\": \"Set:KeyName\""))
		&& Found->Message.Contains(TEXT("\"DefaultValue\": \"TargetActor\"")));
}
} // namespace AssetDocumentBlueprintPinDefaultTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintByRefDefaultTest,
	"UEAssetDoc.Graph.Blueprint.ByRefDefault",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBlueprintByRefDefaultTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentBlueprintPinDefaultTests;
	const FPinDefaultTarget Target;
	ExpectRefused(*this, TEXT("validate literal"), Validate(*this, Target.Document(false), TEXT("validate literal")));
	ExpectRefused(*this, TEXT("apply literal"), Apply(*this, Target, Target.Document(false), TEXT("apply literal")));
	const FString Linked = Target.Document(true);
	TestTrue(TEXT("validate linked Make Literal Name"), Validate(*this, Linked, TEXT("validate linked")).IsSuccess());
	TestTrue(TEXT("apply linked Make Literal Name"), Apply(*this, Target, Linked, TEXT("apply linked")).IsSuccess());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintLinkDisallowedTest,
	"UEAssetDoc.Graph.Blueprint.LinkDisallowed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBlueprintLinkDisallowedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentBlueprintPinDefaultTests;
	const FPinDefaultTarget Target;
	const FString Json = FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"Create\",")
		TEXT("\"Body\":{\"ParentClass\":\"/Script/Engine.Actor\",\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":[")
		TEXT("{\"Id\":\"Begin\",\"Action\":\"K2:BeginPlay\"},{\"Id\":\"Branch\",\"Action\":\"K2:Branch\"},")
		TEXT("{\"Id\":\"Where\",\"Action\":\"K2:CallFunction:/Script/Engine.Actor:K2_GetActorLocation\"}],")
		TEXT("\"Links\":[{\"From\":\"Begin:then\",\"To\":\"Branch:execute\"},{\"From\":\"Where:ReturnValue\",\"To\":\"Branch:Condition\"}]}]}}"),
		*Target.PackageName);
	for (const bool bApply : {false, true})
	{
		const FString Stage = bApply ? TEXT("apply") : TEXT("validate");
		const FAssetDocumentResult Result = bApply ? Apply(*this, Target, Json, Stage) : Validate(*this, Json, Stage);
		TestFalse(*FString::Printf(TEXT("%s: refused"), *Stage), Result.IsSuccess());
		const FAssetDocumentDiagnostic* Found = FindCode(Result, TEXT("Graph.ConnectDisallowed"));
		if (TestNotNull(*FString::Printf(TEXT("%s: Graph.ConnectDisallowed"), *Stage), Found))
		{
			TestTrue(*FString::Printf(TEXT("%s: at the link (%s)"), *Stage, *Found->Path), Found->Path.EndsWith(TEXT("/UbergraphPages/0/Links/1")));
		}
	}
	return true;
}
#endif
