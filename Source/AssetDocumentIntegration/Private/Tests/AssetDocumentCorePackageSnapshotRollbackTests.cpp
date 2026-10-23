// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Core package-snapshot rollback through the real generic ApplyFile route.
// Each row mutates the existing target outside of any typed inverse, then
// fails at one of the four rollback boundaries (mid-apply, repair, staged
// save, atomic install) and checks the restored asset is functionally equivalent.

#include "AssetDocumentGenericRuntimeRoute.h"
#include "AssetDocumentTestDirectoryBlock.h"

#include "AssetDocumentAIPrimitiveProvider.h"
#include "AssetDocumentCoreRuntimeComposition.h"
#include "AssetDocumentEditorMutationPreparation.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "AssetDocumentGenericRuntimeBootstrap.h"
#include "AssetDocumentPythonRuntime.h"
#include "AssetDocumentRequiredPrimitiveProviderView.h"
#include "AssetDocumentService.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"
#include "Utils/AssetDocumentEngineCompat.h"
#include "AssetDocumentPluginPaths.h"

#if WITH_DEV_AUTOMATION_TESTS

FAssetDocumentGenericRuntimeBootstrap* GetAssetDocumentActiveGenericRuntimeBootstrapForTests();
TArray<FAssetDocumentPrimitiveRequirement> GetAssetDocumentLegacyGenericRequirementsForTests();

namespace AssetDocumentCorePackageSnapshotRollbackTests
{
using EPhase = EAssetDocumentRuntimeRequestPhase;

const TCHAR* const InjectedFailureCode = TEXT("Test.CorePackageSnapshotFailure");
const TCHAR* const DesiredString = TEXT("Core snapshot desired");

struct FFileTree
{
	TArray<FString> Paths;
	TArray<TArray<uint8>> Contents;
	bool operator==(const FFileTree& Other) const { return Paths == Other.Paths && Contents == Other.Contents; }
};

FFileTree SnapshotFiles(const FString& Root)
{
	FFileTree Tree;
	IFileManager::Get().FindFilesRecursive(Tree.Paths, *Root, TEXT("*"), true, false, false);
	Tree.Paths.Sort();
	for (const FString& Path : Tree.Paths)
	{
		FFileHelper::LoadFileToArray(Tree.Contents.AddDefaulted_GetRef(), *Path);
	}
	return Tree;
}

FAssetDocumentRuntimeResultDto MakeInjectedFailure(const FAssetDocumentRuntimeRequestDto& Request)
{
	FAssetDocumentRuntimeResultDto Result;
	Result.RequestId = Request.RequestId;
	Result.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Failed;
	Result.FinalPhase = EPhase::Failed;
	Result.RuntimeRevision = Request.ExpectedRuntimeRevision;
	FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/Test/CorePackageSnapshot");
	Diagnostic.Code = InjectedFailureCode;
	Diagnostic.Message = TEXT("Injected failure after the target was mutated.");
	return Result;
}

// The production generic scratch Preflight has no mutation payload; this
// test-owned producer schedules one editor property batch step for TestString.
FAssetDocumentRuntimeValueDto MakeMutationProjection(const FAssetDocumentRuntimeRequestDto& Request, const FString& Hash)
{
	using FValue = FAssetDocumentRuntimeValueDto;
	auto Text = [](const FString& Value) { FValue R; R.Kind = EAssetDocumentRuntimeValueKind::String; R.StringValue = Value; return R; };
	auto Number = [](int64 Value) { FValue R; R.Kind = EAssetDocumentRuntimeValueKind::Integer; R.IntegerValue = Value; return R; };
	auto Array = [](TArray<FValue> Values) { FValue R; R.Kind = EAssetDocumentRuntimeValueKind::Array; R.Elements = MoveTemp(Values); return R; };
	auto Object = [](TArray<FString> Keys, TArray<FValue> Values)
	{
		FValue R; R.Kind = EAssetDocumentRuntimeValueKind::Object; R.ObjectKeys = MoveTemp(Keys); R.Elements = MoveTemp(Values); return R;
	};
	const FValue& Properties = Request.CanonicalDocument.Elements[Request.CanonicalDocument.ObjectKeys.IndexOfByKey(TEXT("Properties"))];
	const FValue& Desired = Properties.Elements[Properties.ObjectKeys.IndexOfByKey(TEXT("TestString"))];
	const FValue Intent = Object({TEXT("properties")},
		{Array({Object({TEXT("name"), TEXT("value")}, {Text(TEXT("TestString")), Desired})})});
	const FValue Step = Object(
		{TEXT("Index"), TEXT("PrimitiveId"), TEXT("Version"), TEXT("Phase"), TEXT("InputSchemaId"), TEXT("Intent")},
		{Number(0), Text(TEXT("editor.property.write_admitted_batch.v1")), Number(1), Text(TEXT("Apply")),
			Text(TEXT("editor.property.write_admitted_batch.v1.input.v1")), Intent});
	return Object(
		{TEXT("Kind"), TEXT("RequestId"), TEXT("RuntimeRevision"), TEXT("ExactAssetClassPath"), TEXT("TargetAssetPath"),
			TEXT("Operation"), TEXT("SemanticHash"), TEXT("Steps")},
		{Text(TEXT("MutationIntentProjectionV1")), Text(Request.RequestId.ToString(EGuidFormats::DigitsWithHyphensLower)),
			Number(Request.ExpectedRuntimeRevision), Text(Request.RequestedAssetClassPath), Text(Request.TargetAssetPath),
			Text(TEXT("Apply")), Text(Hash), Array({Step})});
}

class FInjectingHost final : public IAssetDocumentPythonRuntimeHost
{
public:
	explicit FInjectingHost(IAssetDocumentPythonRuntimeHost& InInner) : Inner(InInner) {}

	virtual FAssetDocumentRuntimeResultDto Dispatch(
		const FAssetDocumentRuntimeRequestDto& Request,
		IAssetDocumentNativeRequestContext& Context) override
	{
		const EPhase Phase = Context.GetMetadata().Phase;
		if (FailPhase.IsSet() && FailPhase.GetValue() == Phase && Phase != EPhase::Apply)
		{
			return MakeInjectedFailure(Request);
		}
		FAssetDocumentRuntimeResultDto Result = Inner.Dispatch(Request, Context);
		if (Result.TerminalStatus != EAssetDocumentRuntimeTerminalStatus::Succeeded)
		{
			return Result;
		}
		if (bMutationProjection && Phase == EPhase::Validate)
		{
			ValidatedHash = Result.SemanticHash;
			ValidatedBytes = Result.CanonicalBytes;
			ValidatedCodecVersion = Result.CodecVersion;
		}
		if (bMutationProjection && Phase == EPhase::Preflight)
		{
			Result.SemanticHash = ValidatedHash;
			Result.CanonicalBytes = ValidatedBytes;
			Result.CodecVersion = ValidatedCodecVersion;
			Result.CanonicalValue = MakeMutationProjection(Request, ValidatedHash);
		}
		if (Phase == EPhase::Apply)
		{
			UObject* Target = FSoftObjectPath(Request.TargetAssetPath).ResolveObject();
			FStrProperty* Property = Target ? FindFProperty<FStrProperty>(Target->GetClass(), TEXT("TestString")) : nullptr;
			bObservedDesiredString = Property && Property->GetPropertyValue_InContainer(Target) == DesiredString;
			if (Target && UntypedMutation)
			{
				// Outside every provider primitive: only the package snapshot covers this.
				UntypedMutation(*Target);
				bMutated = true;
			}
			if (FailPhase.IsSet() && FailPhase.GetValue() == EPhase::Apply)
			{
				return MakeInjectedFailure(Request);
			}
		}
		return Result;
	}

	TOptional<EPhase> FailPhase;
	TFunction<void(UObject&)> UntypedMutation;
	bool bMutationProjection = false;
	bool bMutated = false;
	bool bObservedDesiredString = false;

private:
	IAssetDocumentPythonRuntimeHost& Inner;
	FString ValidatedHash;
	TArray<uint8> ValidatedBytes;
	uint32 ValidatedCodecVersion = 0;
};

class FInjectingRuntime final : public IAssetDocumentPythonRuntime
{
	TUniquePtr<IAssetDocumentPythonRuntime> Inner;

public:
	explicit FInjectingRuntime(const FAssetDocumentPythonRuntimeConfiguration& Configuration)
		: Inner(CreateAssetDocumentPythonRuntime(Configuration))
		, Host(Inner->GetHost())
	{
	}
	virtual FAssetDocumentContractValidationResult InitializePythonBinding() override { return Inner->InitializePythonBinding(); }
	virtual FAssetDocumentPythonRevisionActivationResult TryActivateRevision(const FAssetDocumentPythonRevisionCandidate& Candidate) override
	{
		return Inner->TryActivateRevision(Candidate);
	}
	virtual uint64 GetActiveRuntimeRevision() const override { return Inner->GetActiveRuntimeRevision(); }
	virtual bool HasActiveRequests() const override { return Inner->HasActiveRequests(); }
	virtual IAssetDocumentPythonRuntimeHost& GetHost() override { return Host; }
	virtual const IAssetDocumentPrimitiveRegistry* GetActivePrimitiveRegistry() const override { return Inner->GetActivePrimitiveRegistry(); }
	virtual FAssetDocumentPythonRequestContextCreateResult CreateRequestContext(
		const FAssetDocumentRequestContextMetadata& Metadata,
		const FGuid& TransactionId,
		IAssetDocumentAtomicTransaction* ActiveTransaction) override
	{
		return Inner->CreateRequestContext(Metadata, TransactionId, ActiveTransaction);
	}
	virtual FAssetDocumentHandleInvalidationResult InvalidateHandles(const FAssetDocumentHandleInvalidation& Event) override
	{
		RollbackInvalidations += Event.Reason == EAssetDocumentHandleInvalidationReason::Rollback ? 1 : 0;
		CommitInvalidations += Event.Reason == EAssetDocumentHandleInvalidationReason::Commit ? 1 : 0;
		return Inner->InvalidateHandles(Event);
	}
	virtual void ShutdownPythonBinding() override { Inner->ShutdownPythonBinding(); }

	FInjectingHost Host;
	int32 RollbackInvalidations = 0;
	int32 CommitInvalidations = 0;
};

class FRegistryView final : public IAssetDocumentPrimitiveRegistry
{
public:
	explicit FRegistryView(const IAssetDocumentPrimitiveRegistry& InRegistry) : Registry(InRegistry) {}
	virtual FAssetDocumentPrimitiveRegistrySnapshot Snapshot() const override { return Registry.Snapshot(); }
	virtual FAssetDocumentPrimitiveResolution Resolve(const FAssetDocumentPrimitiveRequirement& Requirement) const override
	{
		return Registry.Resolve(Requirement);
	}

private:
	const IAssetDocumentPrimitiveRegistry& Registry;
};

class FNoCancellation final : public IAssetDocumentCancellationView
{
public:
	virtual bool IsCancellationRequested() const override { return false; }
};

class FNullObserver final : public IAssetDocumentRequestObserver
{
public:
	virtual void OnPhaseChanged(FGuid, EPhase) override {}
};

// Makes a directory read-only for the lifetime of the guard (restored on exit).
class FReadOnlyDirectory final
{
public:
	bool Block(const FString& InRoot)
	{
		Root = InRoot;
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		bCreated = !PlatformFile.DirectoryExists(*Root) && PlatformFile.CreateDirectoryTree(*Root);
		bBlocked = AssetDocumentTestDirectoryBlock::SetDirectoryReadOnly(PlatformFile, *Root, true) && PlatformFile.IsReadOnly(*Root);
		return bBlocked;
	}
	bool Restore()
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		if (bBlocked && !AssetDocumentTestDirectoryBlock::SetDirectoryReadOnly(PlatformFile, *Root, false))
		{
			return false;
		}
		bBlocked = false;
		if (bCreated)
		{
			bCreated = false;
			return IFileManager::Get().DeleteDirectory(*Root, false, false);
		}
		return true;
	}
	~FReadOnlyDirectory() { Restore(); }

private:
	FString Root;
	bool bCreated = false;
	bool bBlocked = false;
};

class FHarness final
{
public:
	FHarness() : Runtime(MakeConfiguration()) {}

	~FHarness()
	{
		if (Service.IsValid() && Route.IsValid())
		{
			Service->UnbindGenericRuntimeRoute(Route.ToSharedRef());
		}
		if (Route.IsValid())
		{
			Route->UnbindComposition();
		}
		Composition.Reset();
		Registry.Reset();
		Route.Reset();
		Service.Reset();
		if (bBindingInitialized)
		{
			Runtime.ShutdownPythonBinding();
		}
	}

	// Preparation mode freezes the selected providers; without an Editor
	// preparation binding the Editor provider behaves like any provider that
	// has no binding (e.g. the AI provider) and relies on the fallback.
	bool Initialize(FAutomationTestBase& Test, bool bPreparationMode)
	{
		bBindingInitialized = Runtime.InitializePythonBinding().bValid;
		if (!Test.TestTrue(TEXT("runtime binding initializes"), bBindingInitialized))
		{
			return false;
		}
		const TArray<TSharedRef<IAssetDocumentPrimitiveProvider>> Providers{
			CreateAssetDocumentEditorPrimitiveProvider(), CreateAssetDocumentAIPrimitiveProvider()};
		Route = MakeShared<FAssetDocumentGenericRuntimeRoute>(Runtime);
		FAssetDocumentPreparedInvocation PreparedInvoke;
		if (bPreparationMode)
		{
			PreparedInvoke = [this](const FString& ProviderId, const FAssetDocumentPrimitiveInvocation& Invocation,
				IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction* Transaction,
				TFunctionRef<FAssetDocumentPrimitiveOutcome()> Forward)
			{
				return Route->InvokePreparedMutation(ProviderId, Invocation, Handles, Transaction, Forward);
			};
		}
		// Editor+AI seed: the full generic seed also requires the Animation provider.
		const FAssetDocumentRequiredPrimitiveProjectionResult Projected = BuildAssetDocumentRequiredPrimitiveProjection(
			Providers, GetAssetDocumentLegacyGenericRequirementsForTests(), GetFrozenAuthorityExceptionRecords(),
			{}, MoveTemp(PreparedInvoke));
		for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Projected.Diagnostics)
		{
			Test.AddInfo(Diagnostic.Code + TEXT(": ") + Diagnostic.Message);
		}
		if (!Test.TestTrue(TEXT("provider projection freezes"), Projected.Projection.IsValid()))
		{
			return false;
		}
		FAssetDocumentPythonRevisionCandidate Candidate;
		Candidate.RuntimeRevision = 1;
		Candidate.EntryModuleRelativePath = TEXT("asset_document/bootstrap.py");
		Candidate.RequiredPrimitives = Projected.Projection->Requirements;
		Candidate.Providers = Projected.Projection->Providers;
		Candidate.AuthorityCatalog.Append(GetFrozenAuthorityExceptionRecords());
		if (!Test.TestTrue(TEXT("revision activates"), Runtime.TryActivateRevision(Candidate).bActivated))
		{
			return false;
		}
		Registry = MakeUnique<FRegistryView>(*Runtime.GetActivePrimitiveRegistry());
		if (bPreparationMode)
		{
			Route->BindMutationPreparationProjection(Projected.Projection);
		}
		FAssetDocumentCoreRuntimeDependencies Dependencies{
			*Route, *Registry, Cancellation, Observer,
			[this](const FAssetDocumentRequestContextMetadata& Metadata, const FGuid& TransactionId,
				IAssetDocumentAtomicTransaction* ActiveTransaction, TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
			{
				IAssetDocumentAtomicTransaction* Routed = Route->ResolveTransactionForPhase(Metadata, ActiveTransaction, OutDiagnostics);
				if (ActiveTransaction != nullptr && Routed == nullptr)
				{
					return TUniquePtr<IAssetDocumentNativeRequestContext>();
				}
				FAssetDocumentPythonRequestContextCreateResult Result = Runtime.CreateRequestContext(
					Metadata, TransactionId.IsValid() ? TransactionId : FGuid::NewGuid(), Routed);
				OutDiagnostics.Append(Result.Diagnostics);
				return MoveTemp(Result.Context);
			},
			[this](const FAssetDocumentHandleInvalidation& Event) { return Runtime.InvalidateHandles(Event); }};
		Composition = MakeUnique<FAssetDocumentCoreRuntimeComposition>(MoveTemp(Dependencies));
		if (!Test.TestTrue(TEXT("Core composition is valid"), Composition->IsValid()))
		{
			return false;
		}
		Route->BindComposition(*Composition);
		Service = MakeShared<FAssetDocumentService>();
		Service->BindGenericRuntimeRoute(Route.ToSharedRef());
		return true;
	}

	static FAssetDocumentPythonRuntimeConfiguration MakeConfiguration()
	{
		FAssetDocumentPythonRuntimeConfiguration Configuration;
		Configuration.RepositoryRoot = GetAssetDocumentPluginBaseDir();
		Configuration.PackageRootRelativePath = TEXT("Content/Python");
		return Configuration;
	}

	FInjectingRuntime Runtime;
	TSharedPtr<FAssetDocumentService> Service;

private:
	TSharedPtr<FAssetDocumentGenericRuntimeRoute> Route;
	TUniquePtr<FRegistryView> Registry;
	TUniquePtr<FAssetDocumentCoreRuntimeComposition> Composition;
	FNoCancellation Cancellation;
	FNullObserver Observer;
	bool bBindingInitialized = false;
};

enum class EBoundary : uint8
{
	MidApply,
	Repair,
	StagedSave,
	AtomicInstall,
};

struct FRow
{
	const TCHAR* Name;
	EBoundary Boundary;
	const TCHAR* ExpectedDiagnostic;
	bool bSavedOnDisk = false;
	bool bPreparationFallback = false;
	// Canonical-visible untyped edits would fail staged verification before install.
	bool bCanonicalUntypedEdits = true;
};

FString SerializeCanonical(const FAssetDocumentResult& Extracted)
{
	FString Text;
	if (Extracted.Payload.IsValid())
	{
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Extracted.Payload.ToSharedRef(), Writer);
	}
	return Text;
}

FString DescribeCodes(const FAssetDocumentResult& Result)
{
	TArray<FString> Codes;
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Codes.Add(Diagnostic.Code);
	}
	return Result.Message + TEXT(" [") + FString::Join(Codes, TEXT(", ")) + TEXT("]");
}

TSet<UObject*> CollectPackageObjects(UPackage& Package)
{
	TArray<UObject*> Objects;
	GetObjectsWithOuter(&Package, Objects, AssetDocumentObjects::IncludeNested, RF_NoFlags, EInternalObjectFlags::Garbage);
	return TSet<UObject*>(Objects);
}

int32 CountPackagesUnder(const FString& Root)
{
	int32 Count = 0;
	ForEachObjectOfClass(UPackage::StaticClass(), [&Count, &Root](UObject* Object)
	{
		Count += Object->GetName().StartsWith(Root) ? 1 : 0;
	});
	return Count;
}

int32 CountRegistryAssetsUnder(const FString& Root)
{
	TArray<FAssetData> Assets;
	IAssetRegistry::GetChecked().GetAssetsByPath(FName(*Root), Assets, true, true);
	return Assets.Num();
}

void RunRow(FAutomationTestBase& Test, const FRow& Row)
{
	FHarness Harness;
	if (!Harness.Initialize(Test, Row.bPreparationFallback))
	{
		return;
	}
	const FString Unique = FString::Printf(TEXT("DA_CoreSnapshot_%s_%s"), Row.Name, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString ContentRoot = TEXT("/Game/AssetDocumentTests/CorePackageSnapshot/") + Unique;
	// The package lives in its own folder so the install row can block it alone.
	const FString PackageName = ContentRoot + TEXT("/Package/") + Unique;
	const FString ObjectPath = PackageName + TEXT(".") + Unique;
	const FString FixtureDirectory = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("AssetDocumentTests/CorePackageSnapshot"), Unique);
	const FString PackageDirectory = FPaths::Combine(FixtureDirectory, TEXT("Package"));
	const FString PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	// A sidecar must sit beside its package.
	const FString SidecarPath = FPaths::Combine(PackageDirectory, Unique + TEXT(".assetdoc.json"));
	const FString RuntimeRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AssetDocument/Runtime"));
	UPackage* Package = nullptr;
	ON_SCOPE_EXIT
	{
		if (Package)
		{
			TArray<UObject*> Objects;
			GetObjectsWithOuter(Package, Objects);
			for (UObject* Object : Objects)
			{
				Object->ClearFlags(RF_Public | RF_Standalone);
			}
			Package->SetDirtyFlag(false);
		}
		IFileManager::Get().DeleteDirectory(*FixtureDirectory, false, true);
	};

	UClass* FixtureClass = LoadObject<UClass>(nullptr, TEXT("/Script/AssetDocumentTests.TestDataAsset"));
	if (!Test.TestNotNull(TEXT("fixture class loads"), FixtureClass))
	{
		return;
	}
	Package = CreatePackage(*PackageName);
	UObject* Asset = NewObject<UObject>(Package, FixtureClass, *Unique, RF_Public | RF_Standalone | RF_Transactional);
	FStrProperty* TestString = FindFProperty<FStrProperty>(FixtureClass, TEXT("TestString"));
	FIntProperty* TestInt = FindFProperty<FIntProperty>(FixtureClass, TEXT("TestInt"));
	FFloatProperty* TestFloat = FindFProperty<FFloatProperty>(FixtureClass, TEXT("TestFloat"));
	if (!Test.TestTrue(TEXT("fixture properties reflect"), TestString && TestInt && TestFloat))
	{
		return;
	}
	TestString->SetPropertyValue_InContainer(Asset, TEXT("Core snapshot baseline"));
	TestInt->SetPropertyValue_InContainer(Asset, 7);
	IFileManager::Get().MakeDirectory(*PackageDirectory, true);
	if (Row.bSavedOnDisk)
	{
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		if (!Test.TestTrue(TEXT("fixture package saves"), UPackage::SavePackage(Package, Asset, *PackageFile, SaveArgs)))
		{
			return;
		}
	}
	Package->SetDirtyFlag(false);

	const FString SidecarJson = FString::Printf(
		TEXT("{\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\",\"Target\":\"%s\",\"Class\":\"%s\",\"Action\":\"Update\",\"Properties\":{\"TestString\":\"%s\",\"TestInt\":4242}}"),
		*PackageName, *FixtureClass->GetPathName(), DesiredString);
	if (!Test.TestTrue(TEXT("sidecar writes"), FFileHelper::SaveStringToFile(SidecarJson, *SidecarPath)))
	{
		return;
	}

	FAssetDocumentExtractRequest ExtractRequest;
	ExtractRequest.AssetPath = ObjectPath;
	const FString CanonicalBefore = SerializeCanonical(Harness.Service->Extract(ExtractRequest));
	Test.TestFalse(TEXT("baseline extract is non-empty"), CanonicalBefore.IsEmpty());
	const TSet<UObject*> ObjectsBefore = CollectPackageObjects(*Package);
	const FFileTree FilesBefore = SnapshotFiles(FixtureDirectory);
	const FFileTree RuntimeBefore = SnapshotFiles(RuntimeRoot);
	const int32 PackagesBefore = CountPackagesUnder(ContentRoot);
	const int32 RegistryBefore = CountRegistryAssetsUnder(ContentRoot);

	FInjectingHost& Host = Harness.Runtime.Host;
	Host.bMutationProjection = Row.bPreparationFallback;
	Host.FailPhase = Row.Boundary == EBoundary::MidApply ? TOptional<EPhase>(EPhase::Apply)
		: Row.Boundary == EBoundary::Repair ? TOptional<EPhase>(EPhase::Repair) : TOptional<EPhase>();
	TWeakObjectPtr<UObject> StraySubobject;
	TWeakObjectPtr<UObject> StrayAsset;
	bool bMadeStraySubobject = false;
	Host.UntypedMutation = [&](UObject& Target)
	{
		StraySubobject = NewObject<UObject>(&Target, FixtureClass, TEXT("StraySubobject"));
		bMadeStraySubobject = true;
		if (Row.bCanonicalUntypedEdits)
		{
			TestFloat->SetPropertyValue_InContainer(&Target, 7.5f);
			StrayAsset = NewObject<UObject>(Target.GetPackage(), FixtureClass, TEXT("StrayAsset"), RF_Public | RF_Standalone);
		}
		Target.GetPackage()->SetDirtyFlag(true);
	};

	FAssetDocumentResult Result;
	{
		TUniquePtr<FReadOnlyDirectory> Blocker;
		if (Row.Boundary == EBoundary::StagedSave)
		{
			Blocker = MakeUnique<FReadOnlyDirectory>();
			Test.TestTrue(TEXT("staging root is blocked"), Blocker->Block(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AssetDocument/Runtime/Staging"))));
		}
		else if (Row.Boundary == EBoundary::AtomicInstall)
		{
			Blocker = MakeUnique<FReadOnlyDirectory>();
			Test.TestTrue(TEXT("package directory is blocked"), Blocker->Block(PackageDirectory));
		}
		FAssetDocumentApplyFileRequest Request;
		Request.FilePath = SidecarPath;
		Request.bSaveAsset = true;
		Result = Harness.Service->ApplyFile(Request);
		if (Blocker)
		{
			Test.TestTrue(TEXT("blocked directory is restored"), Blocker->Restore());
		}
	}

	const FString Label = Row.Name;
	Test.AddInfo(Label + TEXT(" diagnostics: ") + DescribeCodes(Result));
	Test.TestFalse(Label + TEXT(" fails"), Result.IsSuccess());
	Test.TestTrue(Label + TEXT(" fails at the expected boundary"),
		Result.Diagnostics.ContainsByPredicate([&Row](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == Row.ExpectedDiagnostic; }));
	Test.TestFalse(Label + TEXT(" rollback is not poisoned"),
		Result.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code.StartsWith(TEXT("Atomic.Poisoned")); }));
	Test.TestTrue(Label + TEXT(" target was mutated before failure"), Host.bMutated);
	if (Row.bPreparationFallback)
	{
		Test.TestTrue(Label + TEXT(" binding-less provider wrote the desired value"), Host.bObservedDesiredString);
	}
	Test.TestEqual(Label + TEXT(" rolls back once"), Harness.Runtime.RollbackInvalidations, 1);
	Test.TestEqual(Label + TEXT(" never commits"), Harness.Runtime.CommitInvalidations, 0);

	// 1. Logic unchanged.
	Test.TestTrue(Label + TEXT(" keeps the asset identity"), FSoftObjectPath(ObjectPath).ResolveObject() == Asset);
	Test.TestEqual(Label + TEXT(" canonical extract is restored"), SerializeCanonical(Harness.Service->Extract(ExtractRequest)), CanonicalBefore);
	Test.TestEqual(Label + TEXT(" TestFloat is restored"), TestFloat->GetPropertyValue_InContainer(Asset), 0.0f);
	// 4. No residue: same package objects, no new packages or registry entries.
	Test.TestTrue(Label + TEXT(" package object set is restored"), CollectPackageObjects(*Package).Num() == ObjectsBefore.Num()
		&& CollectPackageObjects(*Package).Includes(ObjectsBefore));
	// The apply collects garbage when it returns (staged copies), so a stray the rollback moved out of the package
	// may already be destroyed.
	Test.TestTrue(Label + TEXT(" the mutation made a stray subobject"), bMadeStraySubobject);
	Test.TestTrue(Label + TEXT(" stray subobject is gone"), !StraySubobject.IsValid() || StraySubobject->GetPackage() != Package);
	Test.TestTrue(Label + TEXT(" stray asset is gone"), !StrayAsset.IsValid() || StrayAsset->GetPackage() != Package);
	Test.TestEqual(Label + TEXT(" creates no packages"), CountPackagesUnder(ContentRoot), PackagesBefore);
	Test.TestEqual(Label + TEXT(" creates no registry entries"), CountRegistryAssetsUnder(ContentRoot), RegistryBefore);
	// 3. Disk untouched: package, sidecar and runtime journal/sync files.
	Test.TestTrue(Label + TEXT(" package and sidecar bytes are unchanged"), SnapshotFiles(FixtureDirectory) == FilesBefore);
	Test.TestTrue(Label + TEXT(" leaves no runtime files"), SnapshotFiles(RuntimeRoot) == RuntimeBefore);
	// 5. Dirty flag restored.
	Test.TestFalse(Label + TEXT(" clean package stays clean"), Package->IsDirty());
}
} // namespace AssetDocumentCorePackageSnapshotRollbackTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentCorePackageSnapshotRollbackTest,
	"UEAssetDoc.GenericRuntime.CorePackageSnapshotRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentCorePackageSnapshotRollbackTest::RunTest(const FString&)
{
	using namespace AssetDocumentCorePackageSnapshotRollbackTests;
	IConsoleVariable* SnapshotEnabled = IConsoleManager::Get().FindConsoleVariable(TEXT("AssetDocument.CorePackageSnapshot"));
	if (!TestNotNull(TEXT("snapshot CVar exists"), SnapshotEnabled) || !TestTrue(TEXT("snapshot rollback is on by default"), SnapshotEnabled->GetBool()))
	{
		return false;
	}
	FAssetDocumentGenericRuntimeBootstrap* Bootstrap = GetAssetDocumentActiveGenericRuntimeBootstrapForTests();
	if (!TestNotNull(TEXT("production bootstrap exists"), Bootstrap))
	{
		return false;
	}
	const bool bWasActive = Bootstrap->IsActive();
	if (bWasActive)
	{
		Bootstrap->Deactivate();
	}
	ON_SCOPE_EXIT
	{
		if (bWasActive)
		{
			TestTrue(TEXT("production bootstrap is restored"), Bootstrap->Activate());
		}
	};
	const TArray<FRow> Rows{
		{TEXT("MidApply"), EBoundary::MidApply, InjectedFailureCode, true},
		{TEXT("Repair"), EBoundary::Repair, InjectedFailureCode, true},
		{TEXT("StagedSave"), EBoundary::StagedSave, TEXT("Persistence.StagePackageFailed"), true},
		{TEXT("AtomicInstall"), EBoundary::AtomicInstall, TEXT("Persistence.AtomicInstallFailed"), false, false, false},
		{TEXT("BindingLessProvider"), EBoundary::Repair, InjectedFailureCode, false, true},
	};
	for (const FRow& Row : Rows)
	{
		RunRow(*this, Row);
	}
	return true;
}

#endif
