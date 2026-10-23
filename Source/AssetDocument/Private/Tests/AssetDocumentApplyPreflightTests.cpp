// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "AssetDocumentEditorSync.h"
#include "AssetDocumentSidecar.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "UObject/UObjectHash.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentApplyPreflightTests
{
struct FProfileCase
{
	const TCHAR* Label;
	const TCHAR* ClassPath;
	const TCHAR* InvalidBodyKey;
};

const FProfileCase ProfileCases[] = {
	{TEXT("BehaviorTree"), TEXT("/Script/AIModule.BehaviorTree"), TEXT("Tree")},
	{TEXT("BlackboardData"), TEXT("/Script/AIModule.BlackboardData"), TEXT("Keys")},
};

struct FBlueprintCreationCase
{
	const TCHAR* Label;
	const TCHAR* ClassPath;
	// For the Python route (UBlueprint, WidgetBlueprint, AnimBlueprint) the
	// "<Code>: <Message>" lead that the Message must start with.
	const TCHAR* MissingBodyMessage;
	const TCHAR* MissingParentClassMessage;
};

const FBlueprintCreationCase BlueprintCreationCases[] = {
	{
		// Python route (profile.ublueprint.v2): the Message leads with the first diagnostic.
		TEXT("UBlueprint"),
		TEXT("/Script/Engine.Blueprint"),
		TEXT("UBlueprint.ParentClassRequired: UBlueprint creation requires Body.ParentClass"),
		TEXT("UBlueprint.ParentClassRequired: UBlueprint creation requires Body.ParentClass"),
	},
	{
		// Python route (profile.widget_blueprint.v1).
		TEXT("WidgetBlueprint"),
		TEXT("/Script/UMGEditor.WidgetBlueprint"),
		TEXT("Widget.ParentClassRequired: Widget Blueprint creation requires Body.ParentClass"),
		TEXT("Widget.ParentClassRequired: Widget Blueprint creation requires Body.ParentClass"),
	},
	{
		// Python route (profile.anim_blueprint.v1).
		TEXT("AnimBlueprint"),
		TEXT("/Script/Engine.AnimBlueprint"),
		TEXT("AnimBlueprint.ParentClassRequired: AnimBlueprint creation requires Body.ParentClass"),
		TEXT("AnimBlueprint.ParentClassRequired: AnimBlueprint creation requires Body.ParentClass"),
	},
};

FString MakeUniqueTarget(const TCHAR* AssetStem)
{
	return FString::Printf(
		TEXT("/Game/AssetDocumentTests/ApplyPreflight/%s_%s"),
		AssetStem,
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

FString ToObjectPath(const FString& Target)
{
	return FString::Printf(TEXT("%s.%s"), *Target, *FPackageName::GetLongPackageAssetName(Target));
}

FString ToPackageFileName(const FString& Target)
{
	return FPackageName::LongPackageNameToFilename(Target, FPackageName::GetAssetPackageExtension());
}

TSharedRef<FJsonObject> MakeAssetRef(const FString& ObjectPath)
{
	TSharedRef<FJsonObject> AssetRef = MakeShared<FJsonObject>();
	AssetRef->SetStringField(TEXT("Kind"), TEXT("AssetRef"));
	AssetRef->SetStringField(TEXT("Path"), ObjectPath);
	return AssetRef;
}

void CleanupAsset(const FString& Target)
{
	const FString ObjectPath = ToObjectPath(Target);
	const FString PackageFileName = ToPackageFileName(Target);
	const FString SidecarPath = FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(Target);
	{
		FAssetDocumentEditorSync::FScopedSidecarWrite Guard(SidecarPath);
		IFileManager::Get().Delete(*SidecarPath, false, true);
	}

	UObject* ExistingAsset = FindObject<UObject>(nullptr, *ObjectPath);
	if (!ExistingAsset && IFileManager::Get().FileExists(*PackageFileName))
	{
		ExistingAsset = LoadObject<UObject>(nullptr, *ObjectPath);
	}

	if (ExistingAsset)
	{
		TArray<UObject*> ObjectsToDelete{ExistingAsset};
		ObjectTools::DeleteObjectsUnchecked(ObjectsToDelete);
	}

	IFileManager::Get().Delete(*PackageFileName, false, true);
}

class FScopedCanonicalSidecarFixture
{
public:
	explicit FScopedCanonicalSidecarFixture(const FString& InTarget)
		: Target(InTarget)
		, SidecarPath(FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(InTarget))
	{
		CleanupAsset(Target);
	}

	~FScopedCanonicalSidecarFixture()
	{
		CleanupAsset(Target);
	}

	FAssetDocumentResult Apply(FAssetDocumentService& Service, const TSharedPtr<FJsonObject>& Document) const
	{
		if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(SidecarPath), true))
		{
			return FAssetDocumentResult::Failure(
				FString::Printf(TEXT("Failed to create canonical sidecar directory for '%s'"), *SidecarPath));
		}

		FString WriteError;
		{
			FAssetDocumentEditorSync::FScopedSidecarWrite Guard(SidecarPath);
			if (!FAssetDocumentSidecar::WriteJsonFile(SidecarPath, Document, WriteError))
			{
				return FAssetDocumentResult::Failure(WriteError);
			}
		}

		FAssetDocumentApplyFileRequest Request;
		Request.FilePath = SidecarPath;
		Request.bSaveAsset = true;
		return Service.ApplyFile(Request);
	}

private:
	FString Target;
	FString SidecarPath;
};

TSharedPtr<FJsonObject> MakeTemplateDocument(
	FAutomationTestBase& Test,
	const FAssetDocumentService& Service,
	const FString& Target,
	const FString& ClassPath,
	const FString& Action)
{
	FAssetDocumentTemplateRequest Request;
	Request.Class = ClassPath;
	Request.Target = Target;
	const FAssetDocumentResult Result = Service.CreateTemplate(Request);
	Test.TestTrue(*FString::Printf(TEXT("CreateTemplate succeeds for %s"), *ClassPath), Result.IsSuccess());
	Test.TestTrue(*FString::Printf(TEXT("CreateTemplate returns a document for %s"), *ClassPath), Result.Payload.IsValid());
	if (!Result.Payload.IsValid())
	{
		return nullptr;
	}

	Result.Payload->SetStringField(TEXT("Action"), Action);
	return Result.Payload;
}

bool InvalidateKnownBodyRegion(
	FAutomationTestBase& Test,
	const TSharedPtr<FJsonObject>& Document,
	const FString& BodyKey)
{
	const TSharedPtr<FJsonObject>* Body = nullptr;
	const bool bHasBody = Document.IsValid()
		&& Document->TryGetObjectField(TEXT("Body"), Body)
		&& Body
		&& Body->IsValid();
	Test.TestTrue(*FString::Printf(TEXT("Template contains Body for invalid %s preflight"), *BodyKey), bHasBody);
	if (!bHasBody)
	{
		return false;
	}

	Test.TestTrue(*FString::Printf(TEXT("Template contains registered Body.%s region"), *BodyKey), (*Body)->HasField(BodyKey));
	(*Body)->SetStringField(BodyKey, TEXT("invalid-region-shape"));
	return true;
}

class FScopedAssetAddedCounter
{
public:
	explicit FScopedAssetAddedCounter(const FString& InExpectedObjectPath)
		: AssetRegistry(FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get())
		, ExpectedObjectPath(InExpectedObjectPath)
	{
		Handle = AssetRegistry.OnAssetAdded().AddLambda([this](const FAssetData& AssetData)
		{
			if (AssetData.GetObjectPathString() == ExpectedObjectPath)
			{
				++MatchingAssetAddedCount;
			}
		});
	}

	~FScopedAssetAddedCounter()
	{
		AssetRegistry.OnAssetAdded().Remove(Handle);
	}

	int32 GetMatchingAssetAddedCount() const
	{
		return MatchingAssetAddedCount;
	}

private:
	IAssetRegistry& AssetRegistry;
	FString ExpectedObjectPath;
	FDelegateHandle Handle;
	int32 MatchingAssetAddedCount = 0;
};

bool RegistryContainsObjectPath(const FString& ObjectPath)
{
	IAssetRegistry& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	return AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid();
}

struct FSurfaceResults
{
	FAssetDocumentResult Validate;
	FAssetDocumentResult Diff;
	FAssetDocumentResult Apply;
};

FSurfaceResults RunPublicSurfaces(
	FAssetDocumentService& Service,
	FScopedCanonicalSidecarFixture& SidecarFixture,
	const TSharedPtr<FJsonObject>& Document)
{
	FSurfaceResults Results;

	FAssetDocumentValidateRequest ValidateRequest;
	ValidateRequest.Document = Document;
	Results.Validate = Service.Validate(ValidateRequest);

	FAssetDocumentDiffRequest DiffRequest;
	DiffRequest.Document = Document;
	Results.Diff = Service.Diff(DiffRequest);

	Results.Apply = SidecarFixture.Apply(Service, Document);
	return Results;
}

void ExpectSurfaceFailureMessageParity(
	FAutomationTestBase& Test,
	const FString& CaseLabel,
	const FSurfaceResults& Results,
	const FString& ExpectedMessage)
{
	Test.TestFalse(*FString::Printf(TEXT("%s Validate fails"), *CaseLabel), Results.Validate.IsSuccess());
	Test.TestFalse(*FString::Printf(TEXT("%s Diff fails"), *CaseLabel), Results.Diff.IsSuccess());
	Test.TestFalse(*FString::Printf(TEXT("%s Apply fails"), *CaseLabel), Results.Apply.IsSuccess());
	Test.TestEqual(*FString::Printf(TEXT("%s Validate message is canonical"), *CaseLabel), Results.Validate.Message, ExpectedMessage);
	Test.TestEqual(*FString::Printf(TEXT("%s Diff message matches Validate"), *CaseLabel), Results.Diff.Message, Results.Validate.Message);
	Test.TestEqual(*FString::Printf(TEXT("%s Apply message matches Validate"), *CaseLabel), Results.Apply.Message, Results.Validate.Message);
}

// Python-route failures lead their Message with the first concrete diagnostic:
// "<Code>: <Message> (at <Path>)". Every surface must carry that same lead.
void ExpectSurfaceFailureMessageLeads(
	FAutomationTestBase& Test,
	const FString& CaseLabel,
	const FSurfaceResults& Results,
	const FString& ExpectedLead)
{
	Test.TestFalse(*FString::Printf(TEXT("%s Validate fails"), *CaseLabel), Results.Validate.IsSuccess());
	Test.TestFalse(*FString::Printf(TEXT("%s Diff fails"), *CaseLabel), Results.Diff.IsSuccess());
	Test.TestFalse(*FString::Printf(TEXT("%s Apply fails"), *CaseLabel), Results.Apply.IsSuccess());
	const TPair<const TCHAR*, const FAssetDocumentResult*> Surfaces[] = {
		{TEXT("Validate"), &Results.Validate}, {TEXT("Diff"), &Results.Diff}, {TEXT("Apply"), &Results.Apply}};
	for (const TPair<const TCHAR*, const FAssetDocumentResult*>& Surface : Surfaces)
	{
		Test.TestTrue(
			*FString::Printf(TEXT("%s %s message leads with the concrete diagnostic '%s' (was '%s')"),
				*CaseLabel, Surface.Key, *ExpectedLead, *Surface.Value->Message),
			Surface.Value->Message.StartsWith(ExpectedLead));
		Test.TestFalse(
			*FString::Printf(TEXT("%s %s message is not the generic runtime text"), *CaseLabel, Surface.Key),
			Surface.Value->Message.Contains(TEXT("Generic AssetDocument runtime failed")));
	}
}

void ExpectNoMaterializedTarget(
	FAutomationTestBase& Test,
	const FString& CaseLabel,
	const FString& Target,
	const FScopedAssetAddedCounter& AssetAddedCounter)
{
	const FString ObjectPath = ToObjectPath(Target);
	const FString PackageFileName = ToPackageFileName(Target);
	Test.TestEqual(
		*FString::Printf(TEXT("%s never announces a real asset"), *CaseLabel),
		AssetAddedCounter.GetMatchingAssetAddedCount(),
		0);
	UPackage* Package = FindPackage(nullptr, *Target);
	Test.TestTrue(
		*FString::Printf(TEXT("%s leaves no durable package state"), *CaseLabel),
		!Package || (UPackage::IsEmptyPackage(Package) && !Package->IsDirty()));
	Test.TestNull(*FString::Printf(TEXT("%s leaves no real asset"), *CaseLabel), FindObject<UObject>(nullptr, *ObjectPath));
	Test.TestFalse(*FString::Printf(TEXT("%s leaves no AssetRegistry entry"), *CaseLabel), RegistryContainsObjectPath(ObjectPath));
	Test.TestFalse(
		*FString::Printf(TEXT("%s writes no uasset"), *CaseLabel),
		IFileManager::Get().FileExists(*PackageFileName));
}

int32 CountOwnedObjects(UObject* Asset)
{
	TArray<UObject*> OwnedObjects;
	if (Asset)
	{
		GetObjectsWithOuter(Asset, OwnedObjects);
	}
	return OwnedObjects.Num();
}

bool ResultHasBodyDiagnostic(const FAssetDocumentResult& Result)
{
	return Result.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Path == TEXT("/Body") || Diagnostic.Path.StartsWith(TEXT("/Body/"));
	});
}

void ExpectUnknownBodyRegionFailure(
	FAutomationTestBase& Test,
	const FString& CaseLabel,
	const FString& Surface,
	const FAssetDocumentResult& Result)
{
	Test.TestFalse(*FString::Printf(TEXT("%s %s rejects an unknown Body region"), *CaseLabel, *Surface), Result.IsSuccess());
	Test.TestTrue(
		*FString::Printf(TEXT("%s %s reports the canonical unknown-region diagnostic"), *CaseLabel, *Surface),
		Result.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
		{
			return Diagnostic.Path == TEXT("/Body/UnknownRegion")
				&& Diagnostic.Code == TEXT("UnknownBodyKey");
		}));
}

bool RunUnknownBodyRegionCase(FAutomationTestBase& Test, const FProfileCase& ProfileCase)
{
	const FString Target = MakeUniqueTarget(*FString::Printf(TEXT("%s_UnknownRegion"), ProfileCase.Label));
	const FString ObjectPath = ToObjectPath(Target);
	const FString PackageFileName = ToPackageFileName(Target);
	CleanupAsset(Target);
	FScopedCanonicalSidecarFixture SidecarFixture(Target);

	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	TSharedPtr<FJsonObject> Document = MakeTemplateDocument(
		Test,
		Service,
		Target,
		ProfileCase.ClassPath,
		TEXT("CreateOrUpdate"));
	if (!Document.IsValid())
	{
		return false;
	}

	TSharedRef<FJsonObject> UnknownBody = MakeShared<FJsonObject>();
	UnknownBody->SetObjectField(TEXT("UnknownRegion"), MakeShared<FJsonObject>());
	Document->SetObjectField(TEXT("Body"), UnknownBody);

	FScopedAssetAddedCounter AssetAddedCounter(ObjectPath);

	FAssetDocumentValidateRequest ValidateRequest;
	ValidateRequest.Document = Document;
	const FAssetDocumentResult ValidateResult = Service.Validate(ValidateRequest);

	FAssetDocumentDiffRequest DiffRequest;
	DiffRequest.Document = Document;
	const FAssetDocumentResult DiffResult = Service.Diff(DiffRequest);

	const FAssetDocumentResult ApplyResult = SidecarFixture.Apply(Service, Document);

	const FString CaseLabel(ProfileCase.Label);
	ExpectUnknownBodyRegionFailure(Test, CaseLabel, TEXT("Validate"), ValidateResult);
	ExpectUnknownBodyRegionFailure(Test, CaseLabel, TEXT("Diff"), DiffResult);
	ExpectUnknownBodyRegionFailure(Test, CaseLabel, TEXT("Apply"), ApplyResult);
	Test.TestEqual(
		*FString::Printf(TEXT("%s Diff unknown-region message matches Validate"), ProfileCase.Label),
		DiffResult.Message,
		ValidateResult.Message);
	Test.TestEqual(
		*FString::Printf(TEXT("%s Apply unknown-region message matches Validate"), ProfileCase.Label),
		ApplyResult.Message,
		ValidateResult.Message);

	Test.TestEqual(
		*FString::Printf(TEXT("%s unknown-region surfaces never announce a real asset"), ProfileCase.Label),
		AssetAddedCounter.GetMatchingAssetAddedCount(),
		0);
	UPackage* Package = FindPackage(nullptr, *Target);
	Test.TestTrue(
		*FString::Printf(TEXT("%s unknown-region surfaces leave no durable package state"), ProfileCase.Label),
		!Package || (UPackage::IsEmptyPackage(Package) && !Package->IsDirty()));
	Test.TestNull(
		*FString::Printf(TEXT("%s unknown-region surfaces leave no real asset"), ProfileCase.Label),
		FindObject<UObject>(nullptr, *ObjectPath));
	Test.TestFalse(
		*FString::Printf(TEXT("%s unknown-region surfaces leave no AssetRegistry entry"), ProfileCase.Label),
		RegistryContainsObjectPath(ObjectPath));
	Test.TestFalse(
		*FString::Printf(TEXT("%s unknown-region surfaces write no uasset"), ProfileCase.Label),
		IFileManager::Get().FileExists(*PackageFileName));

	CleanupAsset(Target);
	return true;
}

bool RunBlueprintCreationPrerequisiteCase(
	FAutomationTestBase& Test,
	const FBlueprintCreationCase& CreationCase,
	bool bRemoveWholeBody)
{
	const FString Variant = bRemoveWholeBody ? TEXT("MissingBody") : TEXT("MissingParentClass");
	const FString CaseLabel = FString::Printf(TEXT("%s %s"), CreationCase.Label, *Variant);
	const FString Target = MakeUniqueTarget(*FString::Printf(TEXT("%s_%s"), CreationCase.Label, *Variant));
	CleanupAsset(Target);
	FScopedCanonicalSidecarFixture SidecarFixture(Target);

	// The module service carries the generic route (UBlueprint is served by the Python Profile).
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	TSharedPtr<FJsonObject> Document = MakeTemplateDocument(
		Test,
		Service,
		Target,
		CreationCase.ClassPath,
		TEXT("Create"));
	if (!Document.IsValid())
	{
		return false;
	}

	if (bRemoveWholeBody)
	{
		Document->RemoveField(TEXT("Body"));
	}
	else
	{
		const TSharedPtr<FJsonObject>* Body = nullptr;
		if (!Document->TryGetObjectField(TEXT("Body"), Body) || !Body || !Body->IsValid())
		{
			Test.AddError(FString::Printf(TEXT("%s template has no Body"), CreationCase.Label));
			return false;
		}
		(*Body)->RemoveField(TEXT("ParentClass"));
	}

	FScopedAssetAddedCounter AssetAddedCounter(ToObjectPath(Target));
	const FSurfaceResults Results = RunPublicSurfaces(Service, SidecarFixture, Document);
	const TCHAR* const ExpectedMessage =
		bRemoveWholeBody ? CreationCase.MissingBodyMessage : CreationCase.MissingParentClassMessage;
	if (FString(ExpectedMessage).Contains(TEXT(".ParentClassRequired: ")))
	{
		// Python route: "<Code>: <Message>" lead.
		ExpectSurfaceFailureMessageLeads(Test, CaseLabel, Results, ExpectedMessage);
	}
	else
	{
		ExpectSurfaceFailureMessageParity(Test, CaseLabel, Results, ExpectedMessage);
	}
	if (FString(CreationCase.ClassPath) == TEXT("/Script/Engine.Blueprint"))
	{
		for (const FAssetDocumentResult* Surface : {&Results.Validate, &Results.Diff, &Results.Apply})
		{
			Test.TestTrue(*FString::Printf(TEXT("%s reports UBlueprint.ParentClassRequired"), *CaseLabel),
				Surface->Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
				{
					return Diagnostic.Code == TEXT("UBlueprint.ParentClassRequired")
						&& Diagnostic.Message == TEXT("UBlueprint creation requires Body.ParentClass");
				}));
		}
	}
	ExpectNoMaterializedTarget(Test, CaseLabel, Target, AssetAddedCounter);

	CleanupAsset(Target);
	return true;
}

bool RunAnimMontageInvalidReferenceCase(FAutomationTestBase& Test)
{
	const FString CaseLabel(TEXT("AnimMontage wrong-class Skeleton creation preflight"));
	const FString Target = MakeUniqueTarget(TEXT("AnimMontage_InvalidSkeleton"));
	const FString WrongClassSkeletonPath(TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
	CleanupAsset(Target);
	FScopedCanonicalSidecarFixture SidecarFixture(Target);

	// AnimMontage is served only by the production Generic Runtime route (Python profile).
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	TSharedPtr<FJsonObject> Document = MakeTemplateDocument(
		Test,
		Service,
		Target,
		TEXT("/Script/Engine.AnimMontage"),
		TEXT("Create"));
	if (!Document.IsValid())
	{
		return false;
	}
	Document->GetObjectField(TEXT("Body"))->SetObjectField(TEXT("Skeleton"), MakeAssetRef(WrongClassSkeletonPath));

	FScopedAssetAddedCounter AssetAddedCounter(ToObjectPath(Target));
	const FAssetDocumentResult ApplyResult = SidecarFixture.Apply(Service, Document);
	Test.TestFalse(TEXT("AnimMontage wrong-class Skeleton is rejected before creation"), ApplyResult.IsSuccess());
	Test.TestTrue(
		TEXT("AnimMontage wrong-class Skeleton failure is attributed to Body.Skeleton"),
		ApplyResult.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
		{
			return Diagnostic.Path == TEXT("/Body/Skeleton");
		}));
	ExpectNoMaterializedTarget(Test, CaseLabel, Target, AssetAddedCounter);

	CleanupAsset(Target);
	return true;
}

bool RunAnimMontageMissingTimeStretchCurveCase(FAutomationTestBase& Test)
{
	const FString CaseLabel(TEXT("AnimMontage missing TimeStretch curve creation preflight"));
	const FString Target = MakeUniqueTarget(TEXT("AnimMontage_MissingTimeStretchCurve"));
	CleanupAsset(Target);
	FScopedCanonicalSidecarFixture SidecarFixture(Target);

	// AnimMontage is served only by the production Generic Runtime route (Python profile).
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	TSharedPtr<FJsonObject> Document = MakeTemplateDocument(
		Test,
		Service,
		Target,
		TEXT("/Script/Engine.AnimMontage"),
		TEXT("Create"));
	if (!Document.IsValid())
	{
		return false;
	}

	TSharedPtr<FJsonObject> Body = Document->GetObjectField(TEXT("Body"));
	Body->RemoveField(TEXT("Curves"));
	Body->GetObjectField(TEXT("TimeStretch"))->SetStringField(TEXT("TimeStretchCurveName"), TEXT("MissingCurve"));

	FScopedAssetAddedCounter AssetAddedCounter(ToObjectPath(Target));
	const FSurfaceResults Results = RunPublicSurfaces(Service, SidecarFixture, Document);
	// The Python profile's diagnostic is surfaced in Message. A Create targets a new
	// Montage, so omitted Curves means it owns none.
	ExpectSurfaceFailureMessageLeads(
		Test,
		CaseLabel,
		Results,
		TEXT("AnimMontage.MissingTimeStretchCurve: TimeStretchCurveName must name an authored float curve (at /Body/TimeStretch/TimeStretchCurveName)"));

	auto HasExactDiagnostic = [](const FAssetDocumentResult& Result)
	{
		return Result.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
		{
			return Diagnostic.Path == TEXT("/Body/TimeStretch/TimeStretchCurveName")
				&& Diagnostic.Code == TEXT("AnimMontage.MissingTimeStretchCurve")
				&& Diagnostic.Message == TEXT("TimeStretchCurveName must name an authored float curve");
		});
	};
	Test.TestTrue(TEXT("Validate reports exact MissingTimeStretchCurve diagnostic"), HasExactDiagnostic(Results.Validate));
	Test.TestTrue(TEXT("Diff reports exact MissingTimeStretchCurve diagnostic"), HasExactDiagnostic(Results.Diff));
	Test.TestTrue(TEXT("Apply reports exact MissingTimeStretchCurve diagnostic"), HasExactDiagnostic(Results.Apply));
	ExpectNoMaterializedTarget(Test, CaseLabel, Target, AssetAddedCounter);

	CleanupAsset(Target);
	return true;
}

bool HasDiagnosticCode(const FAssetDocumentResult& Result, const TCHAR* Code)
{
	return Result.Diagnostics.ContainsByPredicate([Code](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == Code;
	});
}

void AddDiagnostics(FAutomationTestBase& Test, const FString& Label, const FAssetDocumentResult& Result)
{
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Test.AddInfo(FString::Printf(TEXT("%s diagnostic %s %s: %s"), *Label, *Diagnostic.Path, *Diagnostic.Code, *Diagnostic.Message));
	}
}

// Generic-route lifecycle: Validate is pure, so an Action that disagrees with the
// target's existence or class is rejected by ApplyFile, which must leave the existing
// asset untouched and must not create a missing one.
bool RunLifecycleMismatchCase(FAutomationTestBase& Test, const FProfileCase& ProfileCase)
{
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	const FString ExpectedCode = FString::Printf(TEXT("%s.TargetActionMismatch"), ProfileCase.Label);

	const FString ExistingTarget = MakeUniqueTarget(*FString::Printf(TEXT("%s_CreateExisting"), ProfileCase.Label));
	{
		const FString CaseLabel = FString::Printf(TEXT("%s Create existing"), ProfileCase.Label);
		FScopedCanonicalSidecarFixture SidecarFixture(ExistingTarget);
		const TSharedPtr<FJsonObject> Document =
			MakeTemplateDocument(Test, Service, ExistingTarget, ProfileCase.ClassPath, TEXT("Create"));
		const FAssetDocumentResult FixtureResult = SidecarFixture.Apply(Service, Document);
		UObject* ExistingAsset = FindObject<UObject>(nullptr, *ToObjectPath(ExistingTarget));
		Test.TestTrue(*FString::Printf(TEXT("%s fixture is created"), *CaseLabel), FixtureResult.IsSuccess() && ExistingAsset);
		if (ExistingAsset)
		{
			UPackage* ExistingPackage = ExistingAsset->GetOutermost();
			ExistingPackage->ClearDirtyFlag();
			const int32 OwnedBefore = CountOwnedObjects(ExistingAsset);
			// The same document again, so only the lifecycle disagrees.
			const FAssetDocumentResult Result = SidecarFixture.Apply(Service, Document);
			Test.TestFalse(*FString::Printf(TEXT("%s ApplyFile fails"), *CaseLabel), Result.IsSuccess());
			if (!Test.TestTrue(*FString::Printf(TEXT("%s reports the existing target"), *CaseLabel),
				HasDiagnosticCode(Result, *ExpectedCode)))
			{
				AddDiagnostics(Test, CaseLabel, Result);
			}
			Test.TestFalse(*FString::Printf(TEXT("%s leaves the package clean"), *CaseLabel), ExistingPackage->IsDirty());
			Test.TestEqual(*FString::Printf(TEXT("%s keeps the owned objects"), *CaseLabel), CountOwnedObjects(ExistingAsset), OwnedBefore);
		}
	}

	const FString MissingTarget = MakeUniqueTarget(*FString::Printf(TEXT("%s_UpdateMissing"), ProfileCase.Label));
	{
		const FString CaseLabel = FString::Printf(TEXT("%s Update missing"), ProfileCase.Label);
		FScopedCanonicalSidecarFixture SidecarFixture(MissingTarget);
		FScopedAssetAddedCounter AssetAddedCounter(ToObjectPath(MissingTarget));
		const FAssetDocumentResult Result = SidecarFixture.Apply(
			Service, MakeTemplateDocument(Test, Service, MissingTarget, ProfileCase.ClassPath, TEXT("Update")));
		Test.TestFalse(*FString::Printf(TEXT("%s ApplyFile fails"), *CaseLabel), Result.IsSuccess());
		if (!Test.TestTrue(*FString::Printf(TEXT("%s reports the missing target"), *CaseLabel),
			HasDiagnosticCode(Result, *ExpectedCode)))
		{
			AddDiagnostics(Test, CaseLabel, Result);
		}
		ExpectNoMaterializedTarget(Test, CaseLabel, MissingTarget, AssetAddedCounter);
	}
	return true;
}

bool RunDiskExistingCreateCase(FAutomationTestBase& Test)
{
	const FString CaseLabel(TEXT("Create disk-existing target"));
	const FString Target = MakeUniqueTarget(TEXT("DiskExisting"));
	const FString ObjectPath = ToObjectPath(Target);
	const FString PackageFileName = ToPackageFileName(Target);
	const FString SentinelContents(TEXT("disk-existing sentinel"));
	CleanupAsset(Target);
	FScopedCanonicalSidecarFixture SidecarFixture(Target);

	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	TSharedPtr<FJsonObject> Document = MakeTemplateDocument(
		Test,
		Service,
		Target,
		ProfileCases[0].ClassPath,
		TEXT("Create"));
	if (!Document.IsValid())
	{
		return false;
	}

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(PackageFileName), true);
	const bool bWroteSentinel = FFileHelper::SaveStringToFile(SentinelContents, *PackageFileName);
	Test.TestTrue(TEXT("Disk-existing fixture writes an unloaded package sentinel"), bWroteSentinel);
	Test.TestNull(TEXT("Disk-existing fixture starts without a loaded package"), FindPackage(nullptr, *Target));
	if (!bWroteSentinel)
	{
		return false;
	}

	FScopedAssetAddedCounter AssetAddedCounter(ObjectPath);
	const FAssetDocumentResult ApplyFileResult = SidecarFixture.Apply(Service, Document);
	Test.TestFalse(TEXT("ApplyFile rejects a disk-existing Create target"), ApplyFileResult.IsSuccess());
	Test.TestTrue(
		TEXT("ApplyFile reports the disk-existing lifecycle failure"),
		HasDiagnosticCode(ApplyFileResult, TEXT("CutoverPreparation.TargetExists")));

	FString ContentsAfter;
	Test.TestTrue(TEXT("Disk-existing sentinel remains readable"), FFileHelper::LoadFileToString(ContentsAfter, *PackageFileName));
	Test.TestEqual(TEXT("Disk-existing sentinel remains unchanged"), ContentsAfter, SentinelContents);
	UPackage* Package = FindPackage(nullptr, *Target);
	Test.TestTrue(
		TEXT("Create disk-existing lookup leaves no dirty package state"),
		!Package || !Package->IsDirty());
	Test.TestNull(TEXT("Create disk-existing lookup creates no asset"), FindObject<UObject>(nullptr, *ObjectPath));
	Test.TestFalse(TEXT("Create disk-existing lookup registers no asset"), RegistryContainsObjectPath(ObjectPath));
	Test.TestEqual(TEXT("Create disk-existing lookup announces no asset"), AssetAddedCounter.GetMatchingAssetAddedCount(), 0);

	IFileManager::Get().Delete(*PackageFileName, false, true);
	CleanupAsset(Target);
	return true;
}

bool RunInvalidCreateCase(FAutomationTestBase& Test, const FProfileCase& ProfileCase)
{
	const FString Target = MakeUniqueTarget(*FString::Printf(TEXT("%s_InvalidPreflight"), ProfileCase.Label));
	const FString ObjectPath = ToObjectPath(Target);
	const FString PackageFileName = ToPackageFileName(Target);
	CleanupAsset(Target);
	FScopedCanonicalSidecarFixture SidecarFixture(Target);

	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	TSharedPtr<FJsonObject> InvalidDocument = MakeTemplateDocument(
		Test,
		Service,
		Target,
		ProfileCase.ClassPath,
		TEXT("Create"));
	if (!InvalidateKnownBodyRegion(Test, InvalidDocument, ProfileCase.InvalidBodyKey))
	{
		CleanupAsset(Target);
		return false;
	}

	FScopedAssetAddedCounter AssetAddedCounter(ObjectPath);
	const FAssetDocumentResult InvalidResult = SidecarFixture.Apply(Service, InvalidDocument);

	Test.TestFalse(
		*FString::Printf(TEXT("%s invalid Body preflight fails"), ProfileCase.Label),
		InvalidResult.IsSuccess());
	Test.TestTrue(
		*FString::Printf(TEXT("%s failure comes from Body/profile preflight"), ProfileCase.Label),
		ResultHasBodyDiagnostic(InvalidResult));
	Test.TestEqual(
		*FString::Printf(TEXT("%s invalid preflight never announces a real asset to AssetRegistry"), ProfileCase.Label),
		AssetAddedCounter.GetMatchingAssetAddedCount(),
		0);
	Test.TestNull(
		*FString::Printf(TEXT("%s invalid preflight leaves no real package"), ProfileCase.Label),
		FindPackage(nullptr, *Target));
	Test.TestNull(
		*FString::Printf(TEXT("%s invalid preflight leaves no real asset"), ProfileCase.Label),
		FindObject<UObject>(nullptr, *ObjectPath));
	Test.TestFalse(
		*FString::Printf(TEXT("%s invalid preflight leaves no AssetRegistry entry"), ProfileCase.Label),
		RegistryContainsObjectPath(ObjectPath));
	Test.TestFalse(
		*FString::Printf(TEXT("%s invalid preflight writes no uasset"), ProfileCase.Label),
		IFileManager::Get().FileExists(*PackageFileName));

	TSharedPtr<FJsonObject> ValidDocument = MakeTemplateDocument(
		Test,
		Service,
		Target,
		ProfileCase.ClassPath,
		TEXT("Create"));
	const FAssetDocumentResult RetryResult = SidecarFixture.Apply(Service, ValidDocument);
	Test.TestTrue(
		*FString::Printf(TEXT("%s valid retry succeeds after rejected preflight"), ProfileCase.Label),
		RetryResult.IsSuccess());
	Test.TestNotNull(
		*FString::Printf(TEXT("%s valid retry creates the requested asset"), ProfileCase.Label),
		FindObject<UObject>(nullptr, *ObjectPath));

	CleanupAsset(Target);
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentApplyPreflightBehaviorTreeInvalidBodyTest,
	"UEAssetDoc.ApplyPreflight.BehaviorTree.InvalidBodyDoesNotCreate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentApplyPreflightBehaviorTreeInvalidBodyTest::RunTest(const FString& Parameters)
{
	return AssetDocumentApplyPreflightTests::RunInvalidCreateCase(
		*this,
		AssetDocumentApplyPreflightTests::ProfileCases[0]);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentApplyPreflightBlackboardDataInvalidBodyTest,
	"UEAssetDoc.ApplyPreflight.BlackboardData.InvalidBodyDoesNotCreate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentApplyPreflightBlackboardDataInvalidBodyTest::RunTest(const FString& Parameters)
{
	return AssetDocumentApplyPreflightTests::RunInvalidCreateCase(
		*this,
		AssetDocumentApplyPreflightTests::ProfileCases[1]);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentApplyPreflightUnknownBodyRegionTest,
	"UEAssetDoc.ApplyPreflight.UnknownBodyRegion.SurfaceParityDoesNotCreate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentApplyPreflightUnknownBodyRegionTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentApplyPreflightTests;

	bool bSuccess = true;
	for (const FProfileCase& ProfileCase : ProfileCases)
	{
		bSuccess &= RunUnknownBodyRegionCase(*this, ProfileCase);
	}
	return bSuccess;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentApplyPreflightBlueprintCreationPrerequisitesTest,
	"UEAssetDoc.ApplyPreflight.CreationPrerequisites.BlueprintFamilies",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentApplyPreflightBlueprintCreationPrerequisitesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentApplyPreflightTests;

	bool bSuccess = true;
	for (const FBlueprintCreationCase& CreationCase : BlueprintCreationCases)
	{
		bSuccess &= RunBlueprintCreationPrerequisiteCase(*this, CreationCase, true);
		bSuccess &= RunBlueprintCreationPrerequisiteCase(*this, CreationCase, false);
	}
	return bSuccess;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentApplyPreflightAnimMontageFragmentResolutionTest,
	"UEAssetDoc.ApplyPreflight.AnimMontage.InvalidFragmentDoesNotCreate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentApplyPreflightAnimMontageFragmentResolutionTest::RunTest(const FString& Parameters)
{
	return AssetDocumentApplyPreflightTests::RunAnimMontageInvalidReferenceCase(*this);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentApplyPreflightAnimMontageMissingTimeStretchCurveTest,
	"UEAssetDoc.ApplyPreflight.AnimMontage.MissingTimeStretchCurveDoesNotCreate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentApplyPreflightAnimMontageMissingTimeStretchCurveTest::RunTest(const FString& Parameters)
{
	return AssetDocumentApplyPreflightTests::RunAnimMontageMissingTimeStretchCurveCase(*this);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentApplyPreflightDiskExistingCreateTest,
	"UEAssetDoc.ApplyPreflight.Lifecycle.DiskExistingCreateStaysUnloadedAndClean",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentApplyPreflightDiskExistingCreateTest::RunTest(const FString& Parameters)
{
	return AssetDocumentApplyPreflightTests::RunDiskExistingCreateCase(*this);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentApplyPreflightLifecycleMismatchTest,
	"UEAssetDoc.ApplyPreflight.Lifecycle.MismatchRejectsWithoutMutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentApplyPreflightLifecycleMismatchTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentApplyPreflightTests;
	bool bSuccess = true;
	for (const FProfileCase& ProfileCase : ProfileCases)
	{
		bSuccess &= RunLifecycleMismatchCase(*this, ProfileCase);
	}

	// A BehaviorTree document aimed at an existing BlackboardData leaves it untouched.
	const FString Target = MakeUniqueTarget(TEXT("ClassMismatch"));
	FScopedCanonicalSidecarFixture SidecarFixture(Target);
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	TestTrue(TEXT("Class-mismatch BlackboardData fixture is created"), SidecarFixture.Apply(
		Service, MakeTemplateDocument(*this, Service, Target, ProfileCases[1].ClassPath, TEXT("Create"))).IsSuccess());
	UObject* Blackboard = FindObject<UObject>(nullptr, *ToObjectPath(Target));
	if (Blackboard)
	{
		Blackboard->GetOutermost()->ClearDirtyFlag();
		const FAssetDocumentResult Result = SidecarFixture.Apply(
			Service, MakeTemplateDocument(*this, Service, Target, ProfileCases[0].ClassPath, TEXT("CreateOrUpdate")));
		TestFalse(TEXT("Class mismatch ApplyFile fails"), Result.IsSuccess());
		if (!TestTrue(TEXT("Class mismatch reports the existing class"),
			HasDiagnosticCode(Result, TEXT("ResolveExisting.ExpectedClassMismatch"))))
		{
			AddDiagnostics(*this, TEXT("Class mismatch"), Result);
		}
		TestTrue(TEXT("Class mismatch keeps the BlackboardData asset"),
			FindObject<UObject>(nullptr, *ToObjectPath(Target)) == Blackboard
			&& Blackboard->GetClass()->GetPathName() == ProfileCases[1].ClassPath);
		TestFalse(TEXT("Class mismatch leaves the package clean"), Blackboard->GetOutermost()->IsDirty());
	}
	return bSuccess;
}

#endif
