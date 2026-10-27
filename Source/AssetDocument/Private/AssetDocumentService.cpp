// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentService.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"
#include "Runtime/AssetDocumentUndoHistory.h"
#include "Runtime/AssetDocumentOpenEditor.h"

#include "AssetDocumentClassResolver.h"
#include "AssetDocumentDocumentChanges.h"
#include "AssetDocumentDuplicate.h"
#include "AssetDocumentEditorSync.h"
#include "AssetDocumentFileWatcher.h"
#include "AssetDocumentIngressQueue.h"
#include "AssetDocumentJson.h"
#include "AssetDocumentModule.h"
#include "AssetDocumentPolicyRegistry.h"
#include "AssetDocumentSidecar.h"
#include "AssetDocumentSidecarApplyLedger.h"
#include "AssetDocumentSidecarReader.h"
#include "AssetDocumentSidecarSync.h"
#include "AssetDocumentSidecarSyncRunner.h"
#include "AssetDocumentSidecarSyncState.h"
#include "Discovery/AssetDocumentDiscovery.h"
#include "AssetDocumentPendingNotices.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/UObjectGlobals.h"
#include "Runtime/AssetDocumentStagedPackagePurge.h"

#include <initializer_list>

// The generic Python runtime route (IAssetDocumentGenericRuntimeRoute) is the only route:
// every class is served by a Python Profile. The service validates the document envelope,
// rejects non-exact class paths up front and dispatches.

namespace ServiceLocal
{
// A diff payload's entries (the generic "changed" list, and a profile receipt's "added" and "removed").
TArray<TSharedPtr<FJsonValue>> CollectDiffEntries(const FJsonObject& Payload)
{
	TArray<TSharedPtr<FJsonValue>> Entries;
	for (const TCHAR* Bucket : {TEXT("changed"), TEXT("added"), TEXT("removed")})
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Payload.TryGetArrayField(Bucket, Values))
		{
			Entries.Append(*Values);
		}
	}
	return Entries;
}

// A sidecar that does not load: Document.DuplicateKey as a diagnostic at the key when that is the cause (two keys of
// one object that are equal ignoring letter case), otherwise the load error. The text is read again only through the
// sidecar check, so a file outside Content is never read for the diagnostic either.
FAssetDocumentResult ServiceSidecarLoadFailure(const FString& FilePath, const FString& Error)
{
	FString Text;
	FString Path;
	FString First;
	FString Second;
	FString ReadError;
	FAssetDocumentResult Result = FAssetDocumentResult::Failure(Error);
	if (FAssetDocumentSidecar::LoadText(FilePath, Text, ReadError) && FAssetDocumentJson::FindDuplicateKey(Text, Path, First, Second))
	{
		FAssetDocumentDiagnostic Diagnostic;
		Diagnostic.Code = TEXT("Document.DuplicateKey");
		Diagnostic.Path = Path;
		Diagnostic.Message = FAssetDocumentJson::DuplicateKeyMessage(Path, First, Second);
		Diagnostic.Severity = TEXT("error");
		Result = FAssetDocumentResult::FailureFromDiagnostics({Diagnostic}, Error);
	}
	Result.SidecarFilePath = FilePath;
	return Result;
}

FString NormalizeValidateFilePath(const FString& FilePath)
{
	if (FilePath.IsEmpty())
	{
		return FString();
	}

	FString NormalizedPath = FilePath;
	FPaths::NormalizeFilename(NormalizedPath);
	if (FPaths::IsRelative(NormalizedPath) && NormalizedPath.StartsWith(TEXT("Content/"), ESearchCase::IgnoreCase))
	{
		NormalizedPath = FPaths::Combine(FPaths::ProjectDir(), NormalizedPath);
	}

	NormalizedPath = FPaths::ConvertRelativePathToFull(NormalizedPath);
	FPaths::NormalizeFilename(NormalizedPath);
	return NormalizedPath;
}

FString NormalizeValidateTarget(const FString& Target)
{
	FString NormalizedTarget = Target;
	FPaths::NormalizeFilename(NormalizedTarget);
	NormalizedTarget.TrimStartAndEndInline();

	FString PackagePath;
	FString ObjectName;
	if (NormalizedTarget.Split(TEXT("."), &PackagePath, &ObjectName))
	{
		NormalizedTarget = PackagePath;
	}

	return NormalizedTarget;
}

TSharedPtr<FJsonValue> ProjectGenericRuntimeValue(
	const FAssetDocumentRuntimeValueDto& Value)
{
	return ProjectAssetDocumentRuntimeValue(Value);
}

FAssetDocumentResult ProjectGenericReadOnlyResult(
	const FAssetDocumentRuntimeResultDto& Runtime,
	const TCHAR* SuccessMessage)
{
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	for (const FAssetDocumentRuntimeDiagnostic& Source : Runtime.Diagnostics)
	{
		Diagnostics.Add(ProjectAssetDocumentRuntimeDiagnostic(Source));
	}
	FAssetDocumentResult Result = Runtime.TerminalStatus == EAssetDocumentRuntimeTerminalStatus::Succeeded
		? FAssetDocumentResult::Success(SuccessMessage)
		: FAssetDocumentResult::FailureFromDiagnostics(
			Diagnostics, TEXT("Generic AssetDocument runtime failed"));
	if (Result.IsSuccess())
	{
		Result.Diagnostics = MoveTemp(Diagnostics);
	}
	if (!Result.IsSuccess()) return Result;
	if (!Runtime.CanonicalValue.IsSet())
	{
		return FAssetDocumentResult::Failure(TEXT("Generic read-only runtime omitted its payload"));
	}
	const TSharedPtr<FJsonValue> Payload = ProjectGenericRuntimeValue(Runtime.CanonicalValue.GetValue());
	if (!Payload.IsValid() || Payload->Type != EJson::Object)
	{
		return FAssetDocumentResult::Failure(TEXT("Generic read-only runtime returned an invalid payload"));
	}
	Result.Payload = Payload->AsObject();
	return Result;
}

// The generic runtime admits only exact class paths ("/Script/Module.Class"). A short
// name the native resolver accepts would otherwise fail inside the runtime with an
// opaque request error, so reject it up front and name the exact path.
TOptional<FAssetDocumentResult> RejectNonExactGenericClassPath(
	const FString& ClassName,
	const UClass* ResolvedClass,
	const FString& Target,
	const FString& SidecarFilePath)
{
	if (ResolvedClass == nullptr || ClassName == ResolvedClass->GetPathName())
	{
		return {};
	}
	FAssetDocumentResult Result = FAssetDocumentResult::Failure(FString::Printf(
		TEXT("Class '%s' must be the exact class path '%s'"), *ClassName, *ResolvedClass->GetPathName()));
	Result.Target = NormalizeValidateTarget(Target);
	Result.SidecarFilePath = SidecarFilePath;
	FAssetDocumentDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/Class");
	Diagnostic.Code = TEXT("Generic.NonExactClassPath");
	Diagnostic.Message = Result.Message;
	return Result;
}

// Engine and plugin content (outside /Game) can be extracted, inspected and duplicated, never
// authored: the first diagnostic says so and how to get an editable copy.
TOptional<FAssetDocumentResult> RejectReadOnlyTarget(const TSharedPtr<FJsonObject>& Document, const FString& SidecarFilePath)
{
	FString Target;
	if (!Document.IsValid() || !Document->TryGetStringField(TEXT("Target"), Target))
	{
		return {};
	}
	Target = NormalizeValidateTarget(Target);
	if (!UE::AssetDocument::IsReadOnlyContentPath(Target))
	{
		return {};
	}
	const FString Message = FString::Printf(
		TEXT("'%s' is engine or plugin content (outside /Game), which is read-only. To edit a copy, set Target to a new /Game path ")
		TEXT("and Action to Create (its extract is a valid starting document), or copy the asset with duplicate_asset."), *Target);
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	Diagnostics.Add({TEXT("/Target"), TEXT("Meta.ReadOnlyTarget"), Message, TEXT("error")});
	FAssetDocumentResult Result = FAssetDocumentResult::FailureFromDiagnostics(Diagnostics, Message);
	Result.Target = Target;
	Result.SidecarFilePath = SidecarFilePath;
	return Result;
}

// The document envelope rule shared by Validate and Apply: a document either has a Body (or
// Definitions) or declares "AssetType": "GenericAsset" (a Properties-only document). The failure
// is a coded diagnostic; for a class whose profile has a Body it points at /Body.
TOptional<FAssetDocumentResult> CheckDocumentEnvelope(
	const FJsonObject& Document,
	const TSharedPtr<IAssetDocumentGenericRuntimeRoute>& Route)
{
	FString AssetType;
	const bool bHasAssetType = Document.TryGetStringField(TEXT("AssetType"), AssetType);
	const bool bHasStructuredShape = Document.HasField(TEXT("Body")) || Document.HasField(TEXT("Definitions"));
	if (bHasAssetType ? AssetType == TEXT("GenericAsset") : bHasStructuredShape)
	{
		return {};
	}
	FAssetDocumentDiagnostic Diagnostic;
	if (bHasAssetType)
	{
		Diagnostic.Path = TEXT("/AssetType");
		Diagnostic.Code = TEXT("Meta.InvalidAssetType");
		Diagnostic.Message = FString::Printf(
			TEXT("AssetType must be GenericAsset when present (got '%s'); omit it for a document with a Body."), *AssetType);
	}
	else
	{
		// Name the profile when the class has one with a Body (an exact profile).
		FString ClassName;
		UClass* ResolvedClass = nullptr;
		FString Error;
		FString ErrorCode;
		TArray<FString> Sections;
		if (Route.IsValid() && Document.TryGetStringField(TEXT("Class"), ClassName)
			&& FAssetDocumentClassResolver::ResolveClass(ClassName, ResolvedClass, Error, ErrorCode) && ResolvedClass != nullptr)
		{
			FAssetDocumentProfileRequest ProfileRequest;
			ProfileRequest.ClassOrAsset = ResolvedClass->GetPathName();
			const FAssetDocumentResult Profile = ProjectGenericReadOnlyResult(
				Route->DispatchInspectProfile(ProfileRequest, ResolvedClass->GetPathName()), TEXT("profile"));
			const TArray<TSharedPtr<FJsonValue>>* BodySections = nullptr;
			if (Profile.IsSuccess() && Profile.Payload.IsValid() && Profile.Payload->TryGetArrayField(TEXT("BodySections"), BodySections))
			{
				for (const TSharedPtr<FJsonValue>& Section : *BodySections)
				{
					FString Name;
					if (Section.IsValid() && Section->TryGetString(Name))
					{
						Sections.Add(Name);
					}
				}
			}
		}
		if (!Sections.IsEmpty())
		{
			Diagnostic.Path = TEXT("/Body");
			Diagnostic.Code = TEXT("Meta.BodyRequired");
			Diagnostic.Message = FString::Printf(
				TEXT("A %s document has a Body (sections %s; get_asset_document_schema describes them): add \"Body\". "
					"Only a Properties-only document of a class without a Body profile uses \"AssetType\": \"GenericAsset\"."),
				*ResolvedClass->GetName(), *FString::Join(Sections, TEXT(", ")));
		}
		else
		{
			Diagnostic.Path = TEXT("/AssetType");
			Diagnostic.Code = TEXT("Meta.AssetTypeRequired");
			Diagnostic.Message = TEXT("A document without Body or Definitions must declare \"AssetType\": \"GenericAsset\" (AssetType must be GenericAsset).");
		}
	}
	FAssetDocumentResult Result = FAssetDocumentResult::Failure(Diagnostic.Message);
	Result.Diagnostics.Add(MoveTemp(Diagnostic));
	return Result;
}

bool ValidateApplyTarget(const FString& Target, FString& OutError)
{
	if (Target.IsEmpty())
	{
		OutError = TEXT("Target is required");
		return false;
	}

	if (!Target.StartsWith(TEXT("/Game/")))
	{
		OutError = FString::Printf(TEXT("Target '%s' must be a long package name under /Game"), *Target);
		return false;
	}

	FText PackageNameReason;
	if (!FPackageName::IsValidLongPackageName(Target, false, &PackageNameReason))
	{
		OutError = FString::Printf(TEXT("Target '%s' is not a valid long package name: %s"), *Target, *PackageNameReason.ToString());
		return false;
	}

	if (FPackageName::GetLongPackageAssetName(Target).IsEmpty())
	{
		OutError = FString::Printf(TEXT("Target '%s' must include a non-empty asset name"), *Target);
		return false;
	}

	return true;
}

FString ToObjectPath(const FString& PackageOrObjectPath)
{
	if (PackageOrObjectPath.Contains(TEXT(".")))
	{
		return PackageOrObjectPath;
	}

	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageOrObjectPath);
	if (AssetName.IsEmpty())
	{
		return PackageOrObjectPath;
	}

	return FString::Printf(TEXT("%s.%s"), *PackageOrObjectPath, *AssetName);
}

// The loaded asset, or the asset loaded from its package on disk; nullptr when neither
// exists. A read never loads a package that is not on disk: StaticLoadObject creates the
// outer package before the load fails, and that empty in-memory package would make a
// later Create of the same path see an existing target.
UObject* LoadAssetFromPackageOrObjectPath(const FString& PackageOrObjectPath)
{
	if (PackageOrObjectPath.IsEmpty())
	{
		return nullptr;
	}

	const FString ObjectPath = ToObjectPath(PackageOrObjectPath);
	if (UObject* Loaded = StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath))
	{
		return Loaded;
	}
	const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
	if (!FPackageName::IsValidLongPackageName(PackageName) || !FPackageName::DoesPackageExist(PackageName))
	{
		return nullptr;
	}
	return LoadObject<UObject>(nullptr, *ObjectPath);
}

TSharedPtr<FJsonObject> MakeDiffDocument(TSharedPtr<FJsonObject> Document)
{
	if (!Document.IsValid())
	{
		return Document;
	}

	const TSharedPtr<FJsonObject>* BodyObject = nullptr;
	if (!Document->TryGetObjectField(TEXT("Body"), BodyObject) || !BodyObject || !BodyObject->IsValid() || !(*BodyObject)->HasField(TEXT("_Skipped")))
	{
		return Document;
	}

	TSharedPtr<FJsonObject> DiffDocument = MakeShared<FJsonObject>();
	DiffDocument->Values = Document->Values;

	TSharedPtr<FJsonObject> DiffBody = MakeShared<FJsonObject>();
	DiffBody->Values = (*BodyObject)->Values;
	DiffBody->RemoveField(TEXT("_Skipped"));
	DiffDocument->SetObjectField(TEXT("Body"), DiffBody);
	return DiffDocument;
}

struct FAssetDocumentResolvedTarget
{
	UObject* Asset = nullptr;
	UClass* Class = nullptr;
	FString Target;
	FString AssetPath;
	// An abstract class (Inspect only): its facts are read, but no document can author it.
	bool bAbstractClass = false;
};

FAssetDocumentResult ResolveClassOrAssetTarget(const FString& ClassOrAsset, const FString& OperationName, FAssetDocumentResolvedTarget& OutTarget)
{
	if (ClassOrAsset.IsEmpty())
	{
		return FAssetDocumentResult::Failure(FString::Printf(TEXT("%s requires a class name or asset path"), *OperationName));
	}

	OutTarget.Target = ClassOrAsset;
	if (ClassOrAsset.StartsWith(TEXT("/Game/")) && ClassOrAsset.EndsWith(TEXT("_C")))
	{
		FString ClassError;
		if (FAssetDocumentClassResolver::ResolveClass(ClassOrAsset, OutTarget.Class, ClassError))
		{
			return FAssetDocumentResult::Success(TEXT("Resolved generated class target"));
		}
	}

	if (ClassOrAsset.StartsWith(TEXT("/Game/")))
	{
		OutTarget.Asset = LoadAssetFromPackageOrObjectPath(ClassOrAsset);
		if (!OutTarget.Asset)
		{
			FAssetDocumentResult Result = FAssetDocumentResult::Failure(FString::Printf(TEXT("Failed to load asset '%s'"), *ClassOrAsset));
			Result.Target = ClassOrAsset;
			Result.AssetPath = ToObjectPath(ClassOrAsset);
			Result.Diagnostics.Add({TEXT("/class_or_asset"), TEXT("Inspect.AssetNotFound"),
				FString::Printf(TEXT("No asset at '%s'. find_assets lists existing assets by path, class and name."), *ClassOrAsset),
				TEXT("error")});
			return Result;
		}

		OutTarget.Class = OutTarget.Asset->GetClass();
		OutTarget.Target = NormalizeValidateTarget(ClassOrAsset);
		OutTarget.AssetPath = OutTarget.Asset->GetPathName();
		return FAssetDocumentResult::Success(TEXT("Resolved class or asset target"));
	}

	FString Error;
	FString Code;
	const bool bResolved = FAssetDocumentClassResolver::ResolveClass(ClassOrAsset, OutTarget.Class, Error, Code);
	FString ReferenceError;
	if (!bResolved && Code == TEXT("FactoryCreate.AbstractClass") && OperationName == TEXT("Inspect")
		&& FAssetDocumentClassResolver::ResolveClassReference(ClassOrAsset, OutTarget.Class, ReferenceError) && OutTarget.Class != nullptr)
	{
		// Inspecting /Script/Engine.CameraShakeBase (the parent class of a camera shake
		// Blueprint) must not be refused. An abstract class is described read-only.
		OutTarget.bAbstractClass = true;
		return FAssetDocumentResult::Success(TEXT("Resolved abstract class target"));
	}
	if (!bResolved)
	{
		FAssetDocumentResult Result = FAssetDocumentResult::Failure(
			FString::Printf(TEXT("%s: '%s'"), *Error, *ClassOrAsset));
		Result.Target = ClassOrAsset;
		const FString Hint = Code == TEXT("ClassResolve.Unresolved")
			? TEXT(" Pass an asset class path (/Script/Engine.CurveFloat), a /Game asset path, or a /Script struct or enum path; list_types and find_assets list what exists.")
			: FString();
		Result.Diagnostics.Add({TEXT("/class_or_asset"), Code.IsEmpty() ? TEXT("ClassResolve.Unresolved") : Code, Error + Hint, TEXT("error")});
		return Result;
	}

	return FAssetDocumentResult::Success(TEXT("Resolved class or asset target"));
}

// A native struct or enum path (/Script/...): described without the AssetDocument runtime.
TOptional<FAssetDocumentResult> InspectNativeType(const FString& Path)
{
	if (TSharedPtr<FJsonObject> Type = UE::AssetDocument::Discovery::DescribeType(Path))
	{
		FAssetDocumentResult Result = FAssetDocumentResult::Success(TEXT("Type described"));
		Result.Target = Path;
		Result.Payload = Type;
		return Result;
	}
	return {};
}

// An asset path outside /Game and /Script: engine or plugin content (read-only).
bool IsReadOnlyAssetPath(const FString& Path)
{
	return Path.StartsWith(TEXT("/")) && !Path.StartsWith(TEXT("/Game/")) && !Path.StartsWith(TEXT("/Script/"))
		&& UE::AssetDocument::IsReadOnlyContentPath(NormalizeValidateTarget(Path));
}

// The asset name of "/Root/Dir/Name" or "/Root/Dir/Name.Name".
FString AssetNameOfPath(const FString& Path)
{
	return FPackageName::GetShortName(NormalizeValidateTarget(Path));
}

// Assets named like the last segment of an unresolved path, under the same mount root
// ("/Niagara/Enums/ENiagara_X" finds "/Niagara/Enums/SpriteRenderer/ENiagara_X"), or, for a bare
// name, User Defined Enums and Structs of that name anywhere.
TArray<FAssetData> FindAssetsNamedLike(const FString& PathOrName)
{
	TArray<FAssetData> Matches;
	IAssetRegistry& Registry = IAssetRegistry::GetChecked();
	if (PathOrName.StartsWith(TEXT("/")))
	{
		const FString Name = AssetNameOfPath(PathOrName);
		FString Root = PathOrName.Mid(1);
		int32 Slash = INDEX_NONE;
		if (Name.IsEmpty() || !Root.FindChar(TEXT('/'), Slash))
		{
			return Matches;
		}
		Root = TEXT("/") + Root.Left(Slash);
		FARFilter Filter;
		Filter.PackagePaths.Add(FName(*Root));
		Filter.bRecursivePaths = true;
		Registry.EnumerateAssets(Filter, [&Matches, &Name](const FAssetData& Asset)
		{
			if (Asset.AssetName.ToString() == Name)
			{
				Matches.Add(Asset);
			}
			return true;
		});
		return Matches;
	}
	FARFilter Filter;
	Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("UserDefinedEnum")));
	Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("UserDefinedStruct")));
	Registry.EnumerateAssets(Filter, [&Matches, &PathOrName](const FAssetData& Asset)
	{
		if (Asset.AssetName.ToString() == PathOrName)
		{
			Matches.Add(Asset);
		}
		return true;
	});
	return Matches;
}

FString JoinCandidatePaths(const TArray<FAssetData>& Candidates)
{
	TArray<FString> Paths;
	for (const FAssetData& Candidate : Candidates)
	{
		Paths.Add(Candidate.GetObjectPathString());
	}
	Paths.Sort();
	return FString::Join(Paths, TEXT(", "));
}

FAssetDocumentResult ReadOnlyAssetNotFound(const FString& Path, const TArray<FAssetData>& Candidates)
{
	FAssetDocumentResult Result = FAssetDocumentResult::Failure(FString::Printf(TEXT("No asset at '%s'"), *Path));
	Result.Target = Path;
	const FString Message = Candidates.IsEmpty()
		? FString::Printf(TEXT("No asset at '%s'. find_assets lists existing assets by path, class and name."), *Path)
		: FString::Printf(TEXT("No asset at '%s'. Assets with that name: %s."), *Path, *JoinCandidatePaths(Candidates));
	Result.Diagnostics.Add({TEXT("/class_or_asset"), TEXT("Inspect.AssetNotFound"), Message, TEXT("error")});
	return Result;
}

// Resolves an engine or plugin asset for a read-only Inspect. An exact path wins; a path that
// names no asset resolves to the only asset of that name under the same mount root (the
// result's message says so). Unset with OutFailure set when it does not resolve.
UObject* ResolveReadOnlyAsset(const FString& Path, FString& OutResolvedNote, TOptional<FAssetDocumentResult>& OutFailure)
{
	IAssetRegistry& Registry = IAssetRegistry::GetChecked();
	FAssetData AssetData = Registry.GetAssetByObjectPath(FSoftObjectPath(ToObjectPath(Path)));
	if (!AssetData.IsValid())
	{
		const TArray<FAssetData> Candidates = FindAssetsNamedLike(Path);
		if (Candidates.Num() != 1)
		{
			OutFailure = ReadOnlyAssetNotFound(Path, Candidates);
			return nullptr;
		}
		AssetData = Candidates[0];
		OutResolvedNote = FString::Printf(TEXT(" (no asset at '%s'; resolved to '%s', the only asset of that name)"),
			*Path, *AssetData.GetObjectPathString());
	}
	UObject* Asset = AssetData.GetAsset();
	if (Asset == nullptr)
	{
		FAssetDocumentResult Result = FAssetDocumentResult::Failure(
			FString::Printf(TEXT("Failed to load asset '%s'"), *AssetData.GetObjectPathString()));
		Result.Target = Path;
		Result.AssetPath = AssetData.GetObjectPathString();
		OutFailure = MoveTemp(Result);
	}
	return Asset;
}

const TCHAR* ProfileRuntimeRegistrationField = TEXT("RuntimeRegistration");

FAssetDocumentResult ResolveClassTarget(const FString& ClassName, UClass*& OutClass)
{
	if (ClassName.IsEmpty())
	{
		return FAssetDocumentResult::Failure(TEXT("CreateTemplate requires a class"));
	}

	FString Error;
	if (!FAssetDocumentClassResolver::ResolveClass(ClassName, OutClass, Error))
	{
		FAssetDocumentResult Result = FAssetDocumentResult::Failure(Error);
		Result.Target = ClassName;
		return Result;
	}

	return FAssetDocumentResult::Success(TEXT("Resolved class target"));
}

TArray<TSharedPtr<FJsonValue>> ServiceMakeStringArray(std::initializer_list<const TCHAR*> Values)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	for (const TCHAR* Value : Values)
	{
		Result.Add(MakeShared<FJsonValueString>(Value));
	}
	return Result;
}

TArray<TSharedPtr<FJsonValue>> MakeFragmentKindArray()
{
	return ServiceMakeStringArray({
		TEXT("AssetRef"),
		TEXT("ClassRef"),
		TEXT("StructValue"),
		TEXT("EmbeddedObject"),
		TEXT("DefinitionRef"),
	});
}

TArray<TSharedPtr<FJsonValue>> MakeRegionPolicyPresetArray()
{
	TArray<TSharedPtr<FJsonValue>> Result;
	for (const FAssetDocumentRegionPolicyPreset& Preset : FAssetDocumentPolicyRegistry::GetBuiltinPresets())
	{
		Result.Add(MakeShared<FJsonValueObject>(FAssetDocumentPolicyRegistry::ExportPresetToJson(Preset)));
	}
	return Result;
}

bool TryParseLifecycleAction(const FString& ActionName, FString& OutError)
{
	if (ActionName == TEXT("Create") || ActionName == TEXT("Update") || ActionName == TEXT("CreateOrUpdate"))
	{
		return true;
	}
	OutError = FString::Printf(TEXT("Unsupported Action '%s'"), *ActionName);
	return false;
}

FAssetDocumentResult MakeRouteUnavailable(const FString& Target, const FString& AssetPath, const FString& SidecarFilePath)
{
	FAssetDocumentResult Result = FAssetDocumentResult::Failure(
		TEXT("Generic AssetDocument runtime route is unavailable"));
	Result.Target = Target;
	Result.AssetPath = AssetPath;
	Result.SidecarFilePath = SidecarFilePath;
	FAssetDocumentDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/GenericRuntime");
	Diagnostic.Code = TEXT("GenericRuntime.Unavailable");
	Diagnostic.Message = Result.Message;
	return Result;
}

// Validation of a document whose class the runtime cannot take (missing or unresolvable): the
// envelope rules in order, ending with the class resolution failure.
FAssetDocumentResult ValidateUnroutableDocument(const TSharedPtr<FJsonObject>& Document, const FString& NormalizedFilePath)
{
	if (!Document.IsValid())
	{
		FAssetDocumentResult Result = FAssetDocumentResult::Failure(TEXT("Validate requires a JSON document or sidecar file path"));
		Result.SidecarFilePath = NormalizedFilePath;
		return Result;
	}

	FString Target;
	auto MakeFailure = [&Target, &NormalizedFilePath](const FString& Message)
	{
		FAssetDocumentResult Result = FAssetDocumentResult::Failure(Message);
		Result.Target = Target;
		Result.SidecarFilePath = NormalizedFilePath;
		return Result;
	};

	double SchemaVersion = 0.0;
	if (!Document->TryGetNumberField(TEXT("SchemaVersion"), SchemaVersion) || SchemaVersion != 1.0)
	{
		return MakeFailure(TEXT("SchemaVersion must be 1"));
	}

	if (TOptional<FAssetDocumentResult> Envelope = CheckDocumentEnvelope(*Document, nullptr))
	{
		Envelope->SidecarFilePath = NormalizedFilePath;
		return MoveTemp(Envelope.GetValue());
	}

	if (!Document->TryGetStringField(TEXT("Target"), Target) || Target.IsEmpty())
	{
		return MakeFailure(TEXT("Target is required"));
	}
	Target = NormalizeValidateTarget(Target);

	FString Error;
	if (!ValidateApplyTarget(Target, Error))
	{
		return MakeFailure(Error);
	}

	if (!FAssetDocumentSidecar::ValidateTargetMatchesSidecar(NormalizedFilePath, Document, Error))
	{
		return MakeFailure(Error);
	}

	FString ClassName;
	if (!Document->TryGetStringField(TEXT("Class"), ClassName) || ClassName.IsEmpty())
	{
		return MakeFailure(TEXT("Class is required"));
	}

	FString ActionName;
	if (!Document->TryGetStringField(TEXT("Action"), ActionName) || ActionName.IsEmpty())
	{
		return MakeFailure(TEXT("Action is required"));
	}
	if (!TryParseLifecycleAction(ActionName, Error))
	{
		return MakeFailure(Error);
	}

	UClass* DeclaredClass = nullptr;
	if (!FAssetDocumentClassResolver::ResolveClassReference(ClassName, DeclaredClass, Error))
	{
		return MakeFailure(Error);
	}

	UObject* ExistingAsset = LoadAssetFromPackageOrObjectPath(Target);
	if (ExistingAsset && ExistingAsset->GetClass() != DeclaredClass)
	{
		FAssetDocumentResult Result = MakeFailure(FString::Printf(
			TEXT("Document class '%s' does not match target asset class '%s'"),
			*DeclaredClass->GetPathName(),
			*ExistingAsset->GetClass()->GetPathName()));
		FAssetDocumentDiagnostic Diagnostic;
		Diagnostic.Path = TEXT("/Class");
		Diagnostic.Code = TEXT("AssetClassMismatch");
		Diagnostic.Message = Result.Message;
		Result.Diagnostics.Add(MoveTemp(Diagnostic));
		return Result;
	}

	UClass* ResolvedClass = nullptr;
	if (!FAssetDocumentClassResolver::ResolveClass(ClassName, ResolvedClass, Error))
	{
		return MakeFailure(Error);
	}
	return MakeFailure(FString::Printf(TEXT("Class '%s' is not served by the generic runtime"), *ClassName));
}
}

FAssetDocumentService::FAssetDocumentService()
	: SidecarApplyLedger(MakeUnique<FAssetDocumentSidecarApplyLedger>())
{
	SidecarSync = MakeUnique<FAssetDocumentSidecarSync>(*this);
	SidecarSyncRunner = MakeUnique<FAssetDocumentSidecarSyncRunner>(*this);
}

FAssetDocumentService::~FAssetDocumentService() = default;

void FAssetDocumentService::BindGenericRuntimeRoute(
	const TSharedRef<IAssetDocumentGenericRuntimeRoute>& Route)
{
	GenericRuntimeRoute = Route;
}

void FAssetDocumentService::UnbindGenericRuntimeRoute(
	const TSharedRef<IAssetDocumentGenericRuntimeRoute>& Route)
{
	if (GenericRuntimeRoute.Pin() == Route)
	{
		GenericRuntimeRoute.Reset();
	}
}

TOptional<FString> FAssetDocumentService::QueueMirroredSidecar(const FString& SidecarPath)
{
	using namespace UE::AssetDocument::SidecarSync;
	if (!IsMirroringEnabled())
	{
		return {};
	}
	const FString Target = FAssetDocumentSidecar::ResolveObjectPathFromSidecar(SidecarPath);
	if (Target.IsEmpty() || !IsInMirrorScope(Target))
	{
		return {};
	}
	const FString PackageName = ToPackageName(Target);
	SidecarSyncRunner->Enqueue(PackageName, EAssetDocumentSidecarSyncTrigger::SidecarChanged);
	return PackageName;
}

FAssetDocumentResult FAssetDocumentService::HandleIngressEvent(
	const FAssetDocumentIngressEvent& Event)
{
	const auto ApplyWatchedSidecar =
		[this](const FString& SidecarPath)
		-> FAssetDocumentResult
	{
		// A sidecar deleted before its change is handled has nothing left to apply: no apply, no failure notice.
		if (!FPaths::FileExists(SidecarPath))
		{
			return FAssetDocumentResult::Success(
				TEXT("The changed AssetDocument sidecar no longer exists"));
		}

		if (FAssetDocumentEditorSync::IsSidecarWriteSuppressed(
			SidecarPath))
		{
			return FAssetDocumentResult::Success(
				TEXT("Suppressed AssetDocument sidecar change"));
		}

		const FString Target =
			FAssetDocumentSidecar::ResolveObjectPathFromSidecar(
				SidecarPath);
		FString Warning;
		if (FAssetDocumentFileWatcher::ShouldSkipAutoApplyForDirtyPackage(
			Target,
			Warning))
		{
			UE_LOG(LogAssetDocument, Warning, TEXT("%s"), *Warning);
			// The edit was not applied: as a failed watcher apply, it stays a pending notice until the file is applied.
			FAssetDocumentPendingNotices::Post(WatcherApplyFailedNotice, SidecarPath,
				TEXT("AssetDocument.WatcherSkippedDirtyTarget"), Warning, TEXT("AssetDocument: ") + Warning);
			return FAssetDocumentResult::Success(Warning);
		}

		// With mirroring on, a mirrored asset's pass runs in the runner's time-sliced queue, not in this event: many
		// changed files (a checkout) are many passes over many ticks.
		if (TOptional<FString> Queued = QueueMirroredSidecar(SidecarPath))
		{
			return FAssetDocumentResult::Success(FString::Printf(TEXT("Queued the sidecar sync of %s"), *Queued.GetValue()));
		}

		FAssetDocumentApplyFileRequest Request;
		Request.FilePath = SidecarPath;
		Request.bSaveAsset = true;
		Request.bTriggeredByWatcher = true;
		return ApplyFile(Request);
	};

	switch (Event.Type)
	{
	case EAssetDocumentIngressEventType::ApplyFile:
		return ApplyFile(Event.ApplyFileRequest);
	case EAssetDocumentIngressEventType::SidecarChanged:
		return ApplyWatchedSidecar(Event.PrimaryPath);
	case EAssetDocumentIngressEventType::SidecarRescanRequested:
	{
		TArray<FString> SidecarPaths;
		FAssetDocumentFileWatcher::
			CollectAssetDocumentSidecarsUnderDirectory(
				Event.PrimaryPath,
				SidecarPaths);
		if (SidecarPaths.IsEmpty())
		{
			return FAssetDocumentResult::Success(
				TEXT("AssetDocument sidecar rescan found no files"));
		}

		FAssetDocumentResult RescanResult =
			FAssetDocumentResult::Success(
				TEXT("AssetDocument sidecar rescan completed"));
		for (const FString& SidecarPath : SidecarPaths)
		{
			// A mirrored asset's sidecar is only queued: the runner's pass decides, from the sync index first,
			// whether it changed.
			if (QueueMirroredSidecar(SidecarPath).IsSet())
			{
				continue;
			}
			const FAssetDocumentResult SidecarResult =
				ApplyWatchedSidecar(SidecarPath);
			if (!SidecarResult.IsSuccess())
			{
				RescanResult = SidecarResult;
				UE_LOG(
					LogAssetDocument,
					Warning,
					TEXT("AssetDocument sidecar rescan failed for '%s': %s"),
					*SidecarPath,
					*SidecarResult.Message);
			}
		}
		return RescanResult;
	}
	case EAssetDocumentIngressEventType::AssetRenamedOrMoved:
		return FAssetDocumentEditorSync::SyncSidecarForRenameOrMove(
			Event.PrimaryPath,
			Event.SecondaryPath)
			? FAssetDocumentResult::Success(
				TEXT("AssetDocument sidecar rename/move synchronized"))
			: FAssetDocumentResult::Failure(
				TEXT("AssetDocument sidecar rename/move was not synchronized"));
	case EAssetDocumentIngressEventType::AssetAdded:
	{
		const FString SourceObjectPath =
			FAssetDocumentEditorSync::InferDuplicateSourceObjectPath(
				Event.PrimaryPath);
		if (SourceObjectPath.IsEmpty())
		{
			return FAssetDocumentResult::Success(
				TEXT("Added asset is not an inferred duplicate"));
		}
		return FAssetDocumentEditorSync::SyncSidecarForDuplicate(
			SourceObjectPath,
			Event.PrimaryPath)
			? FAssetDocumentResult::Success(
				TEXT("AssetDocument duplicate sidecar synchronized"))
			: FAssetDocumentResult::Failure(
				TEXT("AssetDocument duplicate sidecar was not synchronized"));
	}
	case EAssetDocumentIngressEventType::AssetRemoved:
	{
		const FString SidecarPath =
			FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(
				Event.PrimaryPath);
		if (!SidecarPath.IsEmpty()
			&& FAssetDocumentEditorSync::IsSidecarWriteSuppressed(
				SidecarPath))
		{
			return FAssetDocumentResult::Success(
				TEXT("Suppressed AssetDocument deleted sidecar archive"));
		}
		return FAssetDocumentEditorSync::ArchiveSidecarForDeletedAsset(
			Event.PrimaryPath)
			? FAssetDocumentResult::Success(
				TEXT("AssetDocument deleted sidecar archived"))
			: FAssetDocumentResult::Failure(
				TEXT("AssetDocument deleted sidecar was not archived"));
	}
	default:
		return FAssetDocumentResult::Failure(
			TEXT("Unknown AssetDocument ingress event"));
	}
}


FAssetDocumentResult FAssetDocumentService::ApplyParsedDocument(
	const FAssetDocumentApplyRequest& Request)
{
	using namespace ServiceLocal;
	if (!Request.Document.IsValid())
	{
		return FAssetDocumentResult::Failure(TEXT("Apply requires a JSON document"));
	}

	const FString NormalizedSourceDocumentPath = NormalizeValidateFilePath(Request.SourceDocumentPath);

	double SchemaVersion = 0.0;
	if (!Request.Document->TryGetNumberField(TEXT("SchemaVersion"), SchemaVersion) || SchemaVersion != 1.0)
	{
		return FAssetDocumentResult::Failure(TEXT("SchemaVersion must be 1"));
	}

	if (TOptional<FAssetDocumentResult> Envelope = CheckDocumentEnvelope(*Request.Document, GenericRuntimeRoute.Pin()))
	{
		Request.Document->TryGetStringField(TEXT("Target"), Envelope->Target);
		Envelope->Target = NormalizeValidateTarget(Envelope->Target);
		return MoveTemp(Envelope.GetValue());
	}

	FString Target;
	if (!Request.Document->TryGetStringField(TEXT("Target"), Target) || Target.IsEmpty())
	{
		return FAssetDocumentResult::Failure(TEXT("Target is required"));
	}
	Target = NormalizeValidateTarget(Target);
	if (TOptional<FAssetDocumentResult> ReadOnly = RejectReadOnlyTarget(Request.Document, NormalizedSourceDocumentPath))
	{
		return MoveTemp(*ReadOnly);
	}

	FString Error;
	if (!ValidateApplyTarget(Target, Error))
	{
		FAssetDocumentResult Result = FAssetDocumentResult::Failure(Error);
		Result.Target = Target;
		return Result;
	}

	FString ClassName;
	if (!Request.Document->TryGetStringField(TEXT("Class"), ClassName) || ClassName.IsEmpty())
	{
		FAssetDocumentResult Result = FAssetDocumentResult::Failure(TEXT("Class is required"));
		Result.Target = Target;
		return Result;
	}

	FString ActionName;
	if (!Request.Document->TryGetStringField(TEXT("Action"), ActionName) || ActionName.IsEmpty())
	{
		FAssetDocumentResult Result = FAssetDocumentResult::Failure(TEXT("Action is required"));
		Result.Target = Target;
		return Result;
	}

	if (!TryParseLifecycleAction(ActionName, Error))
	{
		FAssetDocumentResult Result = FAssetDocumentResult::Failure(Error);
		Result.Target = Target;
		return Result;
	}

	UClass* ResolvedClass = nullptr;
	FString ClassErrorCode;
	if (!FAssetDocumentClassResolver::ResolveClass(ClassName, ResolvedClass, Error, ClassErrorCode))
	{
		FAssetDocumentResult Result = FAssetDocumentResult::Failure(Error);
		Result.Target = Target;
		FAssetDocumentDiagnostic Diagnostic;
		Diagnostic.Path = TEXT("/Class");
		Diagnostic.Code = ClassErrorCode;
		Diagnostic.Message = Error;
		Result.Diagnostics.Add(MoveTemp(Diagnostic));
		return Result;
	}

	if (TOptional<FAssetDocumentResult> NonExact = RejectNonExactGenericClassPath(
			ClassName, ResolvedClass, Target, NormalizedSourceDocumentPath))
	{
		return MoveTemp(NonExact.GetValue());
	}
	if (const TSharedPtr<IAssetDocumentGenericRuntimeRoute> Route = GenericRuntimeRoute.Pin())
	{
		FAssetDocumentApplyFileRequest FileRequest;
		FileRequest.FilePath = NormalizedSourceDocumentPath;
		FileRequest.bSaveAsset = Request.bSaveAsset;
		return Route->DispatchApplyFile(FileRequest);
	}
	return MakeRouteUnavailable(Target, FString(), NormalizedSourceDocumentPath);
}

namespace ServiceLocal
{
// Undo history after an apply (behaviour in AssetDocumentUndoHistory.h): the apply's own
// entries and, after a change (or a rolled-back apply that recorded entries), the entries
// that touch the target's package are removed; everything else is kept. The result says when
// the user's history changed.
void FinishApplyUndoHistory(
	const FAssetDocumentUndoHistoryMark& BeforeApply,
	const FJsonObject& Document,
	FAssetDocumentResult& Result)
{
	FString Target;
	Document.TryGetStringField(TEXT("Target"), Target);
	const FString PackageName = Target.Contains(TEXT("."))
		? FPackageName::ObjectPathToPackageName(Target)
		: Target;
	const UPackage* Package = PackageName.IsEmpty() ? nullptr : FindPackage(nullptr, *PackageName);
	const bool bAppliedChanges = Result.IsSuccess()
		&& (Result.bSavedAsset || (Package != nullptr && Package->IsDirty()));
	const FAssetDocumentUndoHistoryOutcome Outcome =
		UE::AssetDocument::UndoHistory::FinishApply(BeforeApply, PackageName, bAppliedChanges);
	if (!Outcome.ChangedUserHistory())
	{
		return;
	}
	FAssetDocumentDiagnostic Diagnostic;
	Diagnostic.Path = TEXT("/Target");
	if (Outcome.bCleared)
	{
		Diagnostic.Code = TEXT("Undo.HistoryCleared");
		Diagnostic.Severity = TEXT("warning");
		Diagnostic.Message = FString::Printf(TEXT("The editor's Undo history (%d entries) was cleared: its entries could not be removed one by one."),
			Outcome.ClearedEntries);
	}
	else
	{
		TArray<FString> Quoted;
		for (const FString& Title : Outcome.RemovedTitles)
		{
			Quoted.Add(FString::Printf(TEXT("'%s'"), *Title));
		}
		Diagnostic.Code = TEXT("Undo.HistoryTrimmed");
		Diagnostic.Severity = TEXT("info");
		Diagnostic.Message = FString::Printf(TEXT("Removed %d Undo entr%s that recorded or referenced %s (%s%s): undoing them after "
			"this apply would mix old and new states. Kept %d other entr%s."),
			Outcome.RemovedOtherEntries, Outcome.RemovedOtherEntries == 1 ? TEXT("y") : TEXT("ies"), *PackageName,
			*FString::Join(Quoted, TEXT(", ")), Outcome.RemovedOtherEntries > Quoted.Num() ? TEXT(", ...") : TEXT(""),
			Outcome.KeptEntries, Outcome.KeptEntries == 1 ? TEXT("y") : TEXT("ies"));
	}
	Result.Diagnostics.Add(MoveTemp(Diagnostic));
	if (!Result.Payload.IsValid())
	{
		Result.Payload = MakeShared<FJsonObject>();
	}
	const TSharedRef<FJsonObject> History = MakeShared<FJsonObject>();
	History->SetBoolField(TEXT("cleared"), Outcome.bCleared);
	History->SetNumberField(TEXT("removed"), Outcome.bCleared ? Outcome.ClearedEntries : Outcome.RemovedOtherEntries);
	History->SetNumberField(TEXT("kept"), Outcome.KeptEntries);
	Result.Payload->SetObjectField(TEXT("undo_history"), History);
}
}

namespace ServiceLocal
{
// A failed file watcher apply has no caller to report to: it stays a pending notice (get_editor_context, the next
// tool result, an editor notification) until the sidecar is applied again, by an explicit apply (which reports its
// own result) or a watcher apply that succeeds.
void TrackWatcherOutcome(const FAssetDocumentApplyFileRequest& Request, const FAssetDocumentResult& Result)
{
	const FString SidecarPath = !Result.SidecarFilePath.IsEmpty() ? Result.SidecarFilePath : NormalizeValidateFilePath(Request.FilePath);
	if (SidecarPath.IsEmpty())
	{
		return;
	}
	if (!Request.bTriggeredByWatcher || Result.IsSuccess())
	{
		FAssetDocumentPendingNotices::Resolve(FAssetDocumentService::WatcherApplyFailedNotice, SidecarPath);
		return;
	}
	const FAssetDocumentDiagnostic* First = Result.Diagnostics.FindByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Severity.IsEmpty() || Diagnostic.Severity == TEXT("error");
	});
	const FString Code = First != nullptr && !First->Code.IsEmpty() ? First->Code : FString(TEXT("AssetDocument.WatcherApplyFailed"));
	const FString Message = First != nullptr && !First->Path.IsEmpty() && !Result.Message.Contains(First->Path)
		? FString::Printf(TEXT("%s (at %s)"), *Result.Message, *First->Path)
		: Result.Message;
	UE_LOG(LogAssetDocument, Warning, TEXT("AssetDocument file watcher apply of '%s' failed: [%s] %s"), *SidecarPath, *Code, *Message);
	FAssetDocumentPendingNotices::Post(FAssetDocumentService::WatcherApplyFailedNotice, SidecarPath, Code, Message,
		FString::Printf(TEXT("AssetDocument: applying the changed sidecar %s failed: %s"), *FPaths::GetCleanFilename(SidecarPath), *Message));
}
} // namespace ServiceLocal

FAssetDocumentResult FAssetDocumentService::ApplyFile(const FAssetDocumentApplyFileRequest& Request)
{
	if (TOptional<FAssetDocumentResult> Synced = ApplyFileThroughSidecarSync(Request))
	{
		return MoveTemp(*Synced);
	}
	FAssetDocumentResult Result = ApplyFileOnce(Request);
	ServiceLocal::TrackWatcherOutcome(Request, Result);
	return Result;
}

TOptional<FAssetDocumentResult> FAssetDocumentService::ApplyFileThroughSidecarSync(const FAssetDocumentApplyFileRequest& Request)
{
	using namespace UE::AssetDocument::SidecarSync;
	if (Request.bBypassSidecarSync || !Request.bSaveAsset || !IsMirroringEnabled())
	{
		return {};
	}
	const FString FilePath = ServiceLocal::NormalizeValidateFilePath(Request.FilePath);
	const FString Target = FAssetDocumentSidecar::ResolveObjectPathFromSidecar(FilePath);
	if (Target.IsEmpty() || !IsInMirrorScope(Target))
	{
		return {};
	}
	const FString PackageName = ToPackageName(Target);
	if (!Request.bTriggeredByWatcher)
	{
		// An explicit apply right after the watcher's pass of the same file returns that pass's apply.
		const FAssetDocumentSidecarApplyLedger::FRecord* Prior = SidecarApplyLedger->FindMatching(
			FilePath, FAssetDocumentSidecarApplyLedger::HashFile(FilePath), PackageName);
		if (Prior != nullptr && Prior->bByWatcher && Prior->Result.IsSuccess())
		{
			return FAssetDocumentSidecarApplyLedger::MakeReplayedWatcherResult(*Prior);
		}
	}
	FAssetDocumentSidecarSyncRequest SyncRequest;
	SyncRequest.PackageName = PackageName;
	SyncRequest.Trigger = Request.bTriggeredByWatcher ? EAssetDocumentSidecarSyncTrigger::SidecarChanged : EAssetDocumentSidecarSyncTrigger::Explicit;
	const FAssetDocumentSidecarSyncOutcome Outcome = SidecarSync->Sync(SyncRequest);
	FAssetDocumentResult Result = FAssetDocumentSidecarSync::ToApplyResult(Outcome);
	Result.Payload->SetStringField(TEXT("sidecar_file_path"), FilePath);
	Result.Payload->SetBoolField(TEXT("triggered_by_watcher"), Request.bTriggeredByWatcher);
	if (!Outcome.Apply.IsSet() && Result.IsSuccess())
	{
		// Nothing left to apply: an earlier failed watcher apply of this file is settled.
		FAssetDocumentPendingNotices::Resolve(WatcherApplyFailedNotice, FilePath);
	}
	return Result;
}

FAssetDocumentResult FAssetDocumentService::ApplyFileOnce(const FAssetDocumentApplyFileRequest& Request)
{
	using namespace ServiceLocal;
	// The staged copies this apply loads are collected when it returns.
	const UE::AssetDocument::StagedPackages::FScopedRequest StagedCopies;
	const FString NormalizedFilePath = NormalizeValidateFilePath(Request.FilePath);
	if (NormalizedFilePath.IsEmpty())
	{
		return FAssetDocumentResult::Failure(TEXT("ApplyFile requires a sidecar file path"));
	}

	// The content this apply is of, taken before it is read: the apply's rewrite of the file replaces only that.
	const FString ReadDigest = UE::AssetDocument::SidecarSync::DigestSidecarFile(NormalizedFilePath);
	TSharedPtr<FJsonObject> Document;
	FString Error;
	if (!FAssetDocumentSidecarReader::LoadValidatedDocument(
			NormalizedFilePath,
			Document,
			Error))
	{
		// A sidecar whose Target is engine or plugin content: say that first.
		TSharedPtr<FJsonObject> Unvalidated;
		FString LoadError;
		if (FAssetDocumentSidecar::LoadJsonFile(NormalizedFilePath, Unvalidated, LoadError))
		{
			if (TOptional<FAssetDocumentResult> ReadOnly = RejectReadOnlyTarget(Unvalidated, NormalizedFilePath))
			{
				return MoveTemp(*ReadOnly);
			}
		}
		return ServiceSidecarLoadFailure(NormalizedFilePath, Error);
	}

	// One application per sidecar content (AssetDocumentSidecarApplyLedger.h): the file
	// watcher and an explicit apply of the same unchanged file do not both apply it.
	FString LedgerTarget;
	Document->TryGetStringField(TEXT("Target"), LedgerTarget);
	const FString LedgerPackageName = NormalizeValidateTarget(LedgerTarget);
	const FString AppliedContentHash = Request.bSaveAsset
		? FAssetDocumentSidecarApplyLedger::HashFile(NormalizedFilePath)
		: FString();
	if (const FAssetDocumentSidecarApplyLedger::FRecord* Prior = Request.bSaveAsset
			? SidecarApplyLedger->FindMatching(NormalizedFilePath, AppliedContentHash, LedgerPackageName)
			: nullptr)
	{
		if (Request.bTriggeredByWatcher && (!Prior->bByWatcher || Prior->Result.IsSuccess()))
		{
			UE_LOG(LogAssetDocument, Log, TEXT("AssetDocument watcher skipped '%s': content already applied"), *NormalizedFilePath);
			return FAssetDocumentSidecarApplyLedger::MakeSkippedWatcherResult(*Prior, NormalizedFilePath);
		}
		if (!Request.bTriggeredByWatcher && Prior->bByWatcher && Prior->Result.IsSuccess())
		{
			return FAssetDocumentSidecarApplyLedger::MakeReplayedWatcherResult(*Prior);
		}
	}

	// A document edited from an older extract of a changed asset: refused before anything changes.
	if (TOptional<FAssetDocumentResult> Stale = CheckBaseRevision(Document, NormalizedFilePath))
	{
		return MoveTemp(*Stale);
	}

	// Only the apply's own rewrite of the file is hidden from the file watcher afterwards, and it replaces the file only
	// while the file still has the content applied: an edit someone makes while the apply runs stays, as a change.
	FAssetDocumentEditorSync::FScopedSidecarWrite ActiveCanonicalSidecarWrite(NormalizedFilePath,
		FAssetDocumentEditorSync::FScopedSidecarWrite::ERecord::OwnWrites, ReadDigest);
	const uint64 WritesBefore = FAssetDocumentEditorSync::GetSidecarWriteSerial();

	FAssetDocumentApplyRequest ApplyRequest;
	ApplyRequest.Document = Document;
	ApplyRequest.SourceDocumentPath = NormalizedFilePath;
	ApplyRequest.bSaveAsset = Request.bSaveAsset;

	const FAssetDocumentUndoHistoryMark UndoHistoryBeforeApply = UE::AssetDocument::UndoHistory::Capture();
	// An asset open in its editor (AssetDocumentOpenEditor.h): refused, closed and reopened, or refreshed after the apply.
	UE::AssetDocument::OpenEditor::FScopedApply OpenEditor(LedgerTarget.Contains(TEXT(".")) ? LedgerTarget : LedgerPackageName);
	FAssetDocumentResult Result = OpenEditor.GetRefusal().IsSet() ? OpenEditor.GetRefusal().GetValue() : ApplyParsedDocument(ApplyRequest);
	Result.SidecarFilePath = NormalizedFilePath;
	FinishApplyUndoHistory(UndoHistoryBeforeApply, *Document, Result);
	OpenEditor.Finish(Result);
	RememberAppliedRevision(*Document, Result);
	{
		// Blueprint family: the compile status the editor shows after this apply (rejected applies included).
		FString AppliedPath = Result.AssetPath;
		if (AppliedPath.IsEmpty() && Document->TryGetStringField(TEXT("Target"), AppliedPath))
		{
			AppliedPath = ToObjectPath(NormalizeValidateTarget(AppliedPath));
		}
		if (const UObject* Applied = AppliedPath.IsEmpty() ? nullptr : FindObject<UObject>(nullptr, *AppliedPath))
		{
			Result.Compile = UE::AssetDocument::Discovery::DescribeCompileStatus(*Applied);
		}
	}

	if (!Result.Payload.IsValid())
	{
		Result.Payload = MakeShared<FJsonObject>();
	}
	Result.Payload->SetStringField(TEXT("sidecar_file_path"), NormalizedFilePath);
	Result.Payload->SetBoolField(TEXT("triggered_by_watcher"), Request.bTriggeredByWatcher);
	// A failed create leaves no asset, but the sidecar file stays: it is the author's document (the
	// editor never deletes a source file because importing or creating from it failed). Say so, so
	// the client fixes or deletes it rather than leaving an orphan behind.
	if (!Result.IsSuccess() && !LedgerPackageName.IsEmpty()
		&& FPackageName::IsValidLongPackageName(LedgerPackageName)
		&& !FPackageName::DoesPackageExist(LedgerPackageName)
		&& FindPackage(nullptr, *LedgerPackageName) == nullptr)
	{
		Result.Payload->SetBoolField(TEXT("sidecar_kept"), true);
		Result.Payload->SetStringField(TEXT("sidecar_note"), TEXT(
			"No asset was created; the sidecar file is left in place (it is your document). Fix it and apply again "
			"(the editor's file watcher also applies it when the file changes), or delete the file."));
	}
	if (Result.IsSuccess()
		&& !Request.bSaveAsset
		&& !Result.bWroteSidecar)
	{
		Result.Payload->SetBoolField(TEXT("sidecar_sync_update_skipped"), true);
		Result.Payload->SetStringField(
			TEXT("sidecar_sync_update_skip_reason"),
			TEXT("Sidecar sync rewrite requires a durable asset save"));
	}

	if (Request.bSaveAsset)
	{
		// The applied content and, when the apply rewrote the sidecar, the content it wrote (not what the file has
		// now: an edit made meanwhile was not applied).
		TArray<FString> AppliedHashes = {AppliedContentHash};
		if (const TOptional<FString> Written = FAssetDocumentEditorSync::FindSidecarWriteSince(NormalizedFilePath, WritesBefore))
		{
			AppliedHashes.Add(Written.GetValue());
		}
		SidecarApplyLedger->Record(NormalizedFilePath, MoveTemp(AppliedHashes), LedgerPackageName, Result, Request.bTriggeredByWatcher);
	}
	return Result;
}

FAssetDocumentResult FAssetDocumentService::Inspect(const FAssetDocumentInspectRequest& Request) const
{
	using namespace ServiceLocal;
	FString Path = Request.ClassOrAsset.TrimStartAndEnd();
	if (TOptional<FAssetDocumentResult> Native = InspectNativeType(Path))
	{
		return MoveTemp(*Native);
	}
	FAssetDocumentInspectRequest Resolved = Request;
	if (!Path.IsEmpty() && !Path.Contains(TEXT("/")) && !Path.Contains(TEXT("."))
		&& FindFirstObject<UClass>(*Path, EFindFirstObjectOptions::ExactClass | EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging) == nullptr)
	{
		// A bare name that is no native class: a User Defined Enum or Struct of that name
		// (the name a module input or a struct field shows) is inspected by its path.
		const TArray<FAssetData> Candidates = FindAssetsNamedLike(Path);
		if (Candidates.Num() > 1)
		{
			FAssetDocumentResult Result = FAssetDocumentResult::Failure(FString::Printf(TEXT("'%s' names several assets"), *Path));
			Result.Target = Path;
			Result.Diagnostics.Add({TEXT("/class_or_asset"), TEXT("Inspect.AmbiguousName"),
				FString::Printf(TEXT("User Defined Enums/Structs named '%s': %s. Inspect one by path."), *Path,
					*JoinCandidatePaths(Candidates)), TEXT("error")});
			return Result;
		}
		if (Candidates.Num() == 1)
		{
			Path = Candidates[0].GetObjectPathString();
			Resolved.ClassOrAsset = Path;
		}
	}
	if (IsReadOnlyAssetPath(Path))
	{
		return InspectReadOnlyAsset(Resolved, Path);
	}

	FAssetDocumentResolvedTarget ResolvedTarget;
	FAssetDocumentResult ResolveResult = ResolveClassOrAssetTarget(Resolved.ClassOrAsset, TEXT("Inspect"), ResolvedTarget);
	if (!ResolveResult.IsSuccess())
	{
		return ResolveResult;
	}

	if (const TSharedPtr<IAssetDocumentGenericRuntimeRoute> Route = GenericRuntimeRoute.Pin())
	{
		FAssetDocumentResult Result = ProjectGenericReadOnlyResult(
			Route->DispatchInspect(
				Resolved,
				ResolvedTarget.Class->GetPathName(),
				ResolvedTarget.AssetPath),
			TEXT("AssetDocument inspected"));
		if (Result.IsSuccess() && ResolvedTarget.Asset != nullptr)
		{
			if (!Result.Payload.IsValid())
			{
				Result.Payload = MakeShared<FJsonObject>();
			}
			UE::AssetDocument::Discovery::AddAssetFacts(*ResolvedTarget.Asset, *Result.Payload);
		}
		if (Result.IsSuccess() && ResolvedTarget.bAbstractClass)
		{
			if (!Result.Payload.IsValid())
			{
				Result.Payload = MakeShared<FJsonObject>();
			}
			Result.Payload->SetBoolField(TEXT("Abstract"), true);
			Result.Payload->SetBoolField(TEXT("ReadOnly"), true);
			Result.bReadOnly = true;
			Result.Message = FString::Printf(TEXT("Class inspected, read-only: '%s' is abstract, so it cannot be a document's Class. ")
				TEXT("Author a concrete subclass (list_types with base), or a Blueprint with it as ParentClass (its properties are the Blueprint's ClassDefaults)."),
				*ResolvedTarget.Class->GetPathName());
		}
		Result.Target = ResolvedTarget.Target;
		Result.AssetPath = ResolvedTarget.AssetPath;
		Result.bSavedAsset = false;
		Result.bWroteSidecar = false;
		return Result;
	}
	return MakeRouteUnavailable(ResolvedTarget.Target, ResolvedTarget.AssetPath, FString());
}

// Engine or plugin content: the profile's inspect reads it as it would a /Game asset (the
// runtime admits the read-only target for this read, like Extract), so templates and plugin
// types (Niagara systems, User Defined Enums ...) show their profile details. When the
// profile cannot describe it, the asset facts are returned with the profile's diagnostics as
// warnings.
FAssetDocumentResult FAssetDocumentService::InspectReadOnlyAsset(
	const FAssetDocumentInspectRequest& Request,
	const FString& Path) const
{
	using namespace ServiceLocal;
	FString ResolvedNote;
	TOptional<FAssetDocumentResult> Failure;
	UObject* const Asset = ResolveReadOnlyAsset(Path, ResolvedNote, Failure);
	if (Asset == nullptr)
	{
		return Failure.IsSet() ? MoveTemp(*Failure) : ReadOnlyAssetNotFound(Path, {});
	}
	const FString ObjectPath = Asset->GetPathName();
	FAssetDocumentResult Result = FAssetDocumentResult::Failure(TEXT("Generic AssetDocument runtime is unavailable"));
	if (const TSharedPtr<IAssetDocumentGenericRuntimeRoute> Route = GenericRuntimeRoute.Pin())
	{
		FAssetDocumentInspectRequest Resolved = Request;
		Resolved.ClassOrAsset = ObjectPath;
		Result = ProjectGenericReadOnlyResult(
			Route->DispatchInspect(Resolved, Asset->GetClass()->GetPathName(), ObjectPath),
			TEXT("AssetDocument inspected"));
	}
	if (!Result.IsSuccess())
	{
		TArray<FAssetDocumentDiagnostic> Warnings = MoveTemp(Result.Diagnostics);
		for (FAssetDocumentDiagnostic& Warning : Warnings)
		{
			Warning.Severity = TEXT("warning");
		}
		Result = FAssetDocumentResult::Success(TEXT("Asset facts described (read-only; the profile could not describe it)"));
		Result.Diagnostics = MoveTemp(Warnings);
	}
	else
	{
		Result.Message = TEXT("AssetDocument inspected, read-only: engine or plugin content. To edit a copy, ")
			TEXT("extract it, set Target to a new /Game path and apply it (Action Create), or use duplicate_asset.");
	}
	Result.Message += ResolvedNote;
	if (!Result.Payload.IsValid())
	{
		Result.Payload = MakeShared<FJsonObject>();
	}
	if (!Result.Payload->HasField(TEXT("Class")))
	{
		Result.Payload->SetStringField(TEXT("Class"), Asset->GetClass()->GetPathName());
	}
	Result.Payload->SetBoolField(TEXT("ReadOnly"), true);
	UE::AssetDocument::Discovery::AddAssetFacts(*Asset, *Result.Payload);
	Result.Target = NormalizeValidateTarget(ObjectPath);
	Result.AssetPath = ObjectPath;
	Result.bReadOnly = true;
	Result.bSavedAsset = false;
	Result.bWroteSidecar = false;
	return Result;
}

FAssetDocumentResult FAssetDocumentService::InspectProfile(const FAssetDocumentProfileRequest& Request) const
{
	using namespace ServiceLocal;
	FAssetDocumentResolvedTarget ResolvedTarget;
	FAssetDocumentResult ResolveResult = ResolveClassOrAssetTarget(Request.ClassOrAsset, TEXT("InspectProfile"), ResolvedTarget);
	if (!ResolveResult.IsSuccess())
	{
		return ResolveResult;
	}

	if (const TSharedPtr<IAssetDocumentGenericRuntimeRoute> Route = GenericRuntimeRoute.Pin())
	{
		FAssetDocumentResult Result = ProjectGenericReadOnlyResult(
			Route->DispatchInspectProfile(
				Request,
				ResolvedTarget.Class->GetPathName()),
			TEXT("AssetDocument profile inspected"));
		// Runtime wiring (registration ids, required primitives) is for runtime debugging only.
		if (!Request.bIncludeRuntimeRegistration && Result.Payload.IsValid())
		{
			Result.Payload->RemoveField(ProfileRuntimeRegistrationField);
		}
		Result.Target = ResolvedTarget.Target;
		Result.AssetPath = ResolvedTarget.AssetPath;
		Result.bSavedAsset = false;
		Result.bWroteSidecar = false;
		return Result;
	}
	return MakeRouteUnavailable(ResolvedTarget.Target, ResolvedTarget.AssetPath, FString());
}

FAssetDocumentResult FAssetDocumentService::CreateTemplate(const FAssetDocumentTemplateRequest& Request) const
{
	using namespace ServiceLocal;
	const FString NormalizedTarget = NormalizeValidateTarget(Request.Target);
	if (NormalizedTarget.IsEmpty())
	{
		return FAssetDocumentResult::Failure(TEXT("CreateTemplate requires a target"));
	}

	{
		TSharedPtr<FJsonObject> TargetOnly = MakeShared<FJsonObject>();
		TargetOnly->SetStringField(TEXT("Target"), NormalizedTarget);
		if (TOptional<FAssetDocumentResult> ReadOnly = RejectReadOnlyTarget(TargetOnly, FString()))
		{
			return MoveTemp(*ReadOnly);
		}
	}
	FString Error;
	if (!ValidateApplyTarget(NormalizedTarget, Error))
	{
		FAssetDocumentResult Result = FAssetDocumentResult::Failure(Error);
		Result.Target = NormalizedTarget;
		return Result;
	}

	UClass* ResolvedClass = nullptr;
	FAssetDocumentResult ResolveResult = ResolveClassTarget(Request.Class, ResolvedClass);
	if (!ResolveResult.IsSuccess())
	{
		return ResolveResult;
	}

	if (const TSharedPtr<IAssetDocumentGenericRuntimeRoute> Route = GenericRuntimeRoute.Pin())
	{
		FAssetDocumentResult Result = ProjectGenericReadOnlyResult(
			Route->DispatchCreateTemplate(
				Request,
				NormalizedTarget,
				ResolvedClass->GetPathName()),
			TEXT("AssetDocument template created"));
		Result.Target = NormalizedTarget;
		Result.bSavedAsset = false;
		Result.bWroteSidecar = false;
		return Result;
	}
	return MakeRouteUnavailable(NormalizedTarget, FString(), FString());
}

FAssetDocumentResult FAssetDocumentService::GetSchema() const
{
	using namespace ServiceLocal;
	TSharedPtr<FJsonObject> FieldNaming = MakeShared<FJsonObject>();
	FieldNaming->SetBoolField(TEXT("ban_abbreviations"), true);
	FieldNaming->SetBoolField(TEXT("use_ue_stable_field_names"), true);

	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetNumberField(TEXT("schema_version"), 1);
	Payload->SetStringField(TEXT("asset_type"), TEXT("GenericAsset"));
	Payload->SetArrayField(TEXT("asset_document_tools"), ServiceMakeStringArray({
		TEXT("get_asset_document_schema"),
		TEXT("inspect_asset_document_target"),
		TEXT("inspect_asset_document_profile"),
		TEXT("create_asset_document_template"),
		TEXT("extract_asset_document"),
		TEXT("validate_asset_document"),
		TEXT("diff_asset_document"),
		TEXT("apply_asset_document_file"),
		TEXT("find_assets"),
		TEXT("list_types"),
		TEXT("duplicate_asset"),
	}));
	Payload->SetArrayField(TEXT("fragment_kinds"), MakeFragmentKindArray());
	Payload->SetObjectField(TEXT("field_naming"), FieldNaming);
	Payload->SetArrayField(TEXT("RegionPolicyPresets"), MakeRegionPolicyPresetArray());
	// Every class is served by a Python Profile through the generic runtime; no native profile
	// is registered.
	Payload->SetArrayField(TEXT("registered_profiles"), TArray<TSharedPtr<FJsonValue>>());

	FAssetDocumentResult Result = FAssetDocumentResult::Success(TEXT("AssetDocument schema described"));
	Result.Payload = Payload;
	return Result;
}

FAssetDocumentResult FAssetDocumentService::Extract(const FAssetDocumentExtractRequest& Request) const
{
	using namespace ServiceLocal;
	if (Request.AssetPath.IsEmpty())
	{
		return FAssetDocumentResult::Failure(TEXT("Extract requires an asset path"));
	}

	const FString Target = NormalizeValidateTarget(Request.AssetPath);
	UObject* Asset = LoadAssetFromPackageOrObjectPath(Request.AssetPath);
	if (!Asset)
	{
		FAssetDocumentResult Result = FAssetDocumentResult::Failure(FString::Printf(TEXT("Failed to load asset '%s'"), *Request.AssetPath));
		Result.Target = Target;
		Result.AssetPath = ToObjectPath(Request.AssetPath);
		return Result;
	}

	if (const TSharedPtr<IAssetDocumentGenericRuntimeRoute> Route = GenericRuntimeRoute.Pin())
	{
		const bool bReadOnly = UE::AssetDocument::IsReadOnlyContentPath(Target);
		FAssetDocumentResult Result = ProjectGenericReadOnlyResult(
			Route->DispatchExtract(Request, Asset->GetClass()->GetPathName()),
			bReadOnly
				? TEXT("AssetDocument extracted, read-only: engine or plugin content cannot be applied to its own Target. ")
				  TEXT("To edit a copy, set Target to a new /Game path and apply it (Action Create), or use duplicate_asset.")
				: TEXT("AssetDocument extracted"));
		Result.bReadOnly = bReadOnly && Result.IsSuccess();
		if (Request.bIncludeBaseRevision && !bReadOnly && Result.IsSuccess() && Result.Payload.IsValid())
		{
			// The revision is that of the default extract (diff_only), whatever this extract's options.
			TSharedPtr<FJsonObject> DefaultExtract = Result.Payload;
			if (!Request.bDiffOnly || Request.bIncludeAllWritable)
			{
				FAssetDocumentExtractRequest DefaultRequest;
				DefaultRequest.AssetPath = Request.AssetPath;
				const FAssetDocumentResult Default = Extract(DefaultRequest);
				DefaultExtract = Default.IsSuccess() ? Default.Payload : nullptr;
			}
			if (DefaultExtract.IsValid())
			{
				using namespace UE::AssetDocument::DocumentChanges;
				const FString Revision = ComputeRevision(*DefaultExtract);
				RememberRevision(Revision, Target, DefaultExtract);
				Result.Payload = WithRevision(*Result.Payload, Revision);
			}
		}
		Result.Compile = UE::AssetDocument::Discovery::DescribeCompileStatus(*Asset);
		Result.Target = Target;
		Result.AssetPath = Asset->GetPathName();
		Result.bSavedAsset = false;
		Result.bWroteSidecar = false;
		return Result;
	}
	return MakeRouteUnavailable(Target, Asset->GetPathName(), FString());
}

TOptional<FAssetDocumentResult> FAssetDocumentService::CheckBaseRevision(
	const TSharedPtr<FJsonObject>& Document,
	const FString& NormalizedFilePath) const
{
	using namespace ServiceLocal;
	using namespace UE::AssetDocument::DocumentChanges;
	FString Base;
	FString Target;
	if (!Document.IsValid() || !Document->TryGetStringField(AssetDocumentBaseRevisionField, Base)
		|| !Document->TryGetStringField(TEXT("Target"), Target))
	{
		return {};
	}
	Target = NormalizeValidateTarget(Target);
	if (Target.IsEmpty() || (!FPackageName::DoesPackageExist(Target) && FindPackage(nullptr, *Target) == nullptr))
	{
		// No asset (any more): nothing to be stale against; the apply creates it or fails as without BaseRevision.
		return {};
	}
	FAssetDocumentExtractRequest LiveRequest;
	LiveRequest.AssetPath = Target;
	const FAssetDocumentResult LiveExtract = Extract(LiveRequest);
	if (!LiveExtract.IsSuccess() || !LiveExtract.Payload.IsValid())
	{
		return {};
	}
	const FString Live = ComputeRevision(*LiveExtract.Payload);
	if (Live == Base)
	{
		return {};
	}
	FString BaseTarget;
	const TSharedPtr<FJsonObject> BaseExtract = RecallRevision(Base, BaseTarget);
	if (BaseExtract.IsValid() && BaseTarget != Target)
	{
		// An extract of another asset (a copy authored from it): there is no base of this asset to be stale against.
		return {};
	}
	// A document that already matches the asset changes nothing (applying the same file again is a no-op).
	FAssetDocumentDiffRequest DiffRequest;
	DiffRequest.Document = Document;
	DiffRequest.FilePath = NormalizedFilePath;
	const FAssetDocumentResult NoOp = Diff(DiffRequest);
	if (NoOp.IsSuccess() && NoOp.Payload.IsValid() && CollectDiffEntries(*NoOp.Payload).IsEmpty())
	{
		return {};
	}

	TArray<FString> Changed;
	if (BaseExtract.IsValid())
	{
		Changed = CollectChangedPaths(*BaseExtract, *LiveExtract.Payload, 9);
	}
	const FString What = !BaseExtract.IsValid()
		? FString(TEXT(" (the editor no longer has that extract, so it cannot list the changes)"))
		: Changed.IsEmpty() ? FString()
		: FString::Printf(TEXT(": %s%s"), *FString::Join(TArrayView<const FString>(Changed).Left(8), TEXT(", ")),
			Changed.Num() > 8 ? TEXT(", ...") : TEXT(""));
	// The change came from an apply of a document with this same BaseRevision: this document's own earlier apply (its
	// next apply carries the base_revision that apply returned), or another client's edit of the same extract.
	const bool bAppliedFromSameBase = WasAppliedFrom(Target, Base, Live);
	const FString SameBase = bAppliedFromSameBase
		? FString::Printf(TEXT(" The change was made by an apply of a document with this same BaseRevision: if that was this ")
			TEXT("document's own earlier apply, set BaseRevision to the base_revision that apply returned (%s) and apply again."), *Live)
		: FString();
	FAssetDocumentDiagnostic Diagnostic;
	Diagnostic.Path = FString(TEXT("/")) + AssetDocumentBaseRevisionField;
	Diagnostic.Code = TEXT("Document.StaleBase");
	Diagnostic.Message = FString::Printf(TEXT("%s changed since the extract this document was edited from (BaseRevision %s, now %s)%s. ")
		TEXT("Applying it would undo those changes. Extract the asset again and redo your edit on that extract; to overwrite ")
		TEXT("the changes on purpose, leave BaseRevision out.%s"), *Target, *Base, *Live, *What, *SameBase);
	FAssetDocumentResult Result = FAssetDocumentResult::FailureFromDiagnostics({Diagnostic}, Diagnostic.Message);
	Result.Target = Target;
	Result.AssetPath = ToObjectPath(Target);
	Result.SidecarFilePath = NormalizedFilePath;
	Result.Payload = MakeShared<FJsonObject>();
	Result.Payload->SetStringField(TEXT("base_revision"), Base);
	Result.Payload->SetStringField(TEXT("current_revision"), Live);
	TArray<TSharedPtr<FJsonValue>> ChangedJson;
	for (const FString& Path : Changed)
	{
		ChangedJson.Add(MakeShared<FJsonValueString>(Path));
	}
	Result.Payload->SetArrayField(TEXT("changed_since_base"), ChangedJson);
	if (bAppliedFromSameBase)
	{
		Result.Payload->SetBoolField(TEXT("changed_by_apply_of_same_base"), true);
	}
	return Result;
}

void FAssetDocumentService::RememberAppliedRevision(const FJsonObject& Document, FAssetDocumentResult& Result) const
{
	using namespace ServiceLocal;
	using namespace UE::AssetDocument::DocumentChanges;
	FString Base;
	FString Target;
	if (!Result.IsSuccess() || !Document.TryGetStringField(TEXT("Target"), Target))
	{
		return;
	}
	// Every apply returns the revision to keep, not only one of a document that carried one.
	const bool bHadBase = Document.TryGetStringField(AssetDocumentBaseRevisionField, Base);
	Target = NormalizeValidateTarget(Target);
	FAssetDocumentExtractRequest LiveRequest;
	LiveRequest.AssetPath = Target;
	const FAssetDocumentResult LiveExtract = Extract(LiveRequest);
	if (!LiveExtract.IsSuccess() || !LiveExtract.Payload.IsValid())
	{
		return;
	}
	const FString Live = ComputeRevision(*LiveExtract.Payload);
	if (bHadBase)
	{
		RememberApply(Target, Base, Live);
	}
	RememberRevision(Live, Target, LiveExtract.Payload);
	if (!Result.Payload.IsValid())
	{
		Result.Payload = MakeShared<FJsonObject>();
	}
	Result.Payload->SetStringField(TEXT("base_revision"), Live);
}

// A valid document's result says what Apply would do: create the target, or change the
// N values the Diff lists (also returned under "changes"), or nothing.
FAssetDocumentResult FAssetDocumentService::SummarizeValidApply(
	FAssetDocumentResult Result,
	const TSharedPtr<FJsonObject>& Document,
	const FString& NormalizedFilePath) const
{
	bool bWouldCreate = false;
	if (!Result.Payload.IsValid() || !Result.Payload->TryGetBoolField(TEXT("would_create"), bWouldCreate))
	{
		// Not the production route's preflight answer: keep the route's result.
		return Result;
	}
	if (bWouldCreate)
	{
		Result.Message = FString::Printf(TEXT("AssetDocument valid; Apply would create '%s'"), *Result.Target);
		return Result;
	}
	FAssetDocumentDiffRequest DiffRequest;
	DiffRequest.Document = Document;
	DiffRequest.FilePath = NormalizedFilePath;
	const FAssetDocumentResult DiffResult = Diff(DiffRequest);
	if (!DiffResult.IsSuccess())
	{
		return DiffResult;
	}
	TArray<TSharedPtr<FJsonValue>> Changes = DiffResult.Payload.IsValid()
		? ServiceLocal::CollectDiffEntries(*DiffResult.Payload) : TArray<TSharedPtr<FJsonValue>>();
	const TArray<UE::AssetDocument::DocumentChanges::FRemoval> Removals = UE::AssetDocument::DocumentChanges::CollectRemovals(Changes);
	Result.Message = Changes.IsEmpty()
		? FString(TEXT("AssetDocument valid; the asset already matches, Apply would change nothing"))
		: FString::Printf(TEXT("AssetDocument valid; Apply would change %d value(s)%s%s"), Changes.Num(),
			*UE::AssetDocument::DocumentChanges::DescribeChangedRows(Changes),
			*UE::AssetDocument::DocumentChanges::DescribeRemovals(Removals));
	Result.Payload->SetNumberField(TEXT("change_count"), Changes.Num());
	if (!Removals.IsEmpty())
	{
		Result.Payload->SetArrayField(TEXT("removals"), UE::AssetDocument::DocumentChanges::RemovalsToJson(Removals));
	}
	Result.Payload->SetArrayField(TEXT("changes"), MoveTemp(Changes));
	return Result;
}

FAssetDocumentResult FAssetDocumentService::Validate(const FAssetDocumentValidateRequest& Request) const
{
	using namespace ServiceLocal;
	const FString NormalizedFilePath = NormalizeValidateFilePath(Request.FilePath);
	TSharedPtr<FJsonObject> Document = Request.Document;

	if (!Document.IsValid() && NormalizedFilePath.IsEmpty())
	{
		FAssetDocumentResult Result = FAssetDocumentResult::Failure(TEXT("Validate requires a JSON document or sidecar file path"));
		return Result;
	}

	if (!Document.IsValid())
	{
		FString Error;
		if (!FAssetDocumentSidecar::LoadJsonFile(NormalizedFilePath, Document, Error))
		{
			return ServiceSidecarLoadFailure(NormalizedFilePath, Error);
		}
	}

	if (TOptional<FAssetDocumentResult> ReadOnly = RejectReadOnlyTarget(Document, NormalizedFilePath))
	{
		return MoveTemp(*ReadOnly);
	}

	FString ClassName;
	FString Error;
	UClass* ResolvedClass = nullptr;
	if (Document->TryGetStringField(TEXT("Class"), ClassName)
		&& !ClassName.IsEmpty()
		&& FAssetDocumentClassResolver::ResolveClass(
			ClassName,
			ResolvedClass,
			Error))
	{
		FString RouteTarget;
		Document->TryGetStringField(TEXT("Target"), RouteTarget);
		// A sidecar file must describe the asset at its own location; the runtime
		// validates the document only.
		FString SidecarError;
		if (!FAssetDocumentSidecar::ValidateTargetMatchesSidecar(NormalizedFilePath, Document, SidecarError))
		{
			FAssetDocumentResult Result = FAssetDocumentResult::Failure(SidecarError);
			Result.Target = NormalizeValidateTarget(RouteTarget);
			Result.SidecarFilePath = NormalizedFilePath;
			FAssetDocumentDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
			Diagnostic.Path = TEXT("/Target");
			Diagnostic.Code = TEXT("SidecarTargetMismatch");
			Diagnostic.Message = SidecarError;
			return Result;
		}
		if (TOptional<FAssetDocumentResult> NonExact = RejectNonExactGenericClassPath(
				ClassName, ResolvedClass, RouteTarget, NormalizedFilePath))
		{
			return MoveTemp(NonExact.GetValue());
		}
		// Apply's envelope rule (ApplyParsedDocument): a document without Body or
		// Definitions must declare AssetType GenericAsset.
		if (TOptional<FAssetDocumentResult> Envelope = CheckDocumentEnvelope(*Document, GenericRuntimeRoute.Pin()))
		{
			Envelope->Target = NormalizeValidateTarget(RouteTarget);
			Envelope->SidecarFilePath = NormalizedFilePath;
			return MoveTemp(Envelope.GetValue());
		}
		FAssetDocumentValidateRequest RouteRequest = Request;
		RouteRequest.Document = Document;
		RouteRequest.FilePath = NormalizedFilePath;
		if (const TSharedPtr<IAssetDocumentGenericRuntimeRoute> Route = GenericRuntimeRoute.Pin())
		{
			if (TOptional<FAssetDocumentResult> Stale = CheckBaseRevision(Document, NormalizedFilePath))
			{
				return MoveTemp(*Stale);
			}
			FAssetDocumentResult Result = Route->DispatchValidate(RouteRequest);
			Result.Target = NormalizeValidateTarget(RouteTarget);
			Result.SidecarFilePath = NormalizedFilePath;
			if (!Result.IsSuccess())
			{
				return Result;
			}
			if (!Result.Payload.IsValid())
			{
				Result.Payload = MakeShared<FJsonObject>();
			}
			Result.Payload->SetStringField(TEXT("target"), Result.Target);
			Result.Payload->SetStringField(TEXT("sidecar_file_path"), NormalizedFilePath);
			return SummarizeValidApply(Result, Document, NormalizedFilePath);
		}
		return MakeRouteUnavailable(NormalizeValidateTarget(RouteTarget), FString(), NormalizedFilePath);
	}

	return ValidateUnroutableDocument(Document, NormalizedFilePath);
}

FAssetDocumentResult FAssetDocumentService::Diff(const FAssetDocumentDiffRequest& Request) const
{
	using namespace ServiceLocal;
	const FString NormalizedFilePath = NormalizeValidateFilePath(Request.FilePath);
	TSharedPtr<FJsonObject> Document = Request.Document;

	if (!Document.IsValid() && NormalizedFilePath.IsEmpty())
	{
		return FAssetDocumentResult::Failure(TEXT("Diff requires a JSON document or sidecar file path"));
	}

	if (!Document.IsValid())
	{
		FString Error;
		if (!FAssetDocumentSidecar::LoadJsonFile(NormalizedFilePath, Document, Error))
		{
			return ServiceSidecarLoadFailure(NormalizedFilePath, Error);
		}
	}

	if (TOptional<FAssetDocumentResult> ReadOnly = RejectReadOnlyTarget(Document, NormalizedFilePath))
	{
		return MoveTemp(*ReadOnly);
	}
	Document = MakeDiffDocument(Document);
	FString ClassName;
	FString ClassError;
	UClass* ResolvedClass = nullptr;
	if (Document->TryGetStringField(TEXT("Class"), ClassName)
		&& !ClassName.IsEmpty()
		&& FAssetDocumentClassResolver::ResolveClass(
			ClassName,
			ResolvedClass,
			ClassError))
	{
		FString DiffTarget;
		Document->TryGetStringField(TEXT("Target"), DiffTarget);
		if (TOptional<FAssetDocumentResult> NonExact = RejectNonExactGenericClassPath(
				ClassName, ResolvedClass, DiffTarget, NormalizedFilePath))
		{
			return MoveTemp(NonExact.GetValue());
		}
		FAssetDocumentDiffRequest RouteRequest = Request;
		RouteRequest.Document = Document;
		RouteRequest.FilePath = NormalizedFilePath;
		if (const TSharedPtr<IAssetDocumentGenericRuntimeRoute> Route = GenericRuntimeRoute.Pin())
		{
			FAssetDocumentResult Result = ProjectGenericReadOnlyResult(
				Route->DispatchDiff(RouteRequest),
				TEXT("AssetDocument diffed"));
			if (Result.IsSuccess() && Result.Payload.IsValid())
			{
				using namespace UE::AssetDocument::DocumentChanges;
				const TArray<TSharedPtr<FJsonValue>> Entries = CollectDiffEntries(*Result.Payload);
				const TArray<FRemoval> Removals = CollectRemovals(Entries);
				const FString Rows = DescribeChangedRows(Entries);
				Result.Message += Rows.IsEmpty() ? FString() : TEXT(", differences") + Rows;
				if (!Removals.IsEmpty())
				{
					Result.Message += DescribeRemovals(Removals);
					Result.Payload->SetArrayField(TEXT("removals"), RemovalsToJson(Removals));
				}
			}
			Result.Target = NormalizeValidateTarget(DiffTarget);
			Result.AssetPath = ToObjectPath(Result.Target);
			Result.SidecarFilePath = NormalizedFilePath;
			Result.bSavedAsset = false;
			Result.bWroteSidecar = false;
			return Result;
		}
		return MakeRouteUnavailable(NormalizeValidateTarget(DiffTarget), FString(), NormalizedFilePath);
	}

	return ValidateUnroutableDocument(Document, NormalizedFilePath);
}

namespace ServiceLocal
{
FAssetDocumentResult MakeDiscoveryResult(bool bOk, TSharedPtr<FJsonObject>&& Payload, const FString& Error, const TCHAR* SuccessMessage)
{
	if (!bOk)
	{
		FAssetDocumentResult Result = FAssetDocumentResult::Failure(Error);
		Result.Diagnostics.Add({TEXT("/query"), TEXT("Discovery.InvalidQuery"), Error, TEXT("error")});
		return Result;
	}
	FAssetDocumentResult Result = FAssetDocumentResult::Success(SuccessMessage);
	Result.Payload = MoveTemp(Payload);
	return Result;
}
}

FAssetDocumentResult FAssetDocumentService::FindAssets(const TMap<FString, FString>& Query) const
{
	using namespace ServiceLocal;
	TSharedPtr<FJsonObject> Payload;
	FString Error;
	const bool bOk = UE::AssetDocument::Discovery::FindAssets(Query, Payload, Error);
	return MakeDiscoveryResult(bOk, MoveTemp(Payload), Error, TEXT("Assets found"));
}

FAssetDocumentResult FAssetDocumentService::ListTypes(const TMap<FString, FString>& Query) const
{
	using namespace ServiceLocal;
	TSharedPtr<FJsonObject> Payload;
	FString Error;
	const bool bOk = UE::AssetDocument::Discovery::ListTypes(Query, Payload, Error);
	return MakeDiscoveryResult(bOk, MoveTemp(Payload), Error, TEXT("Types listed"));
}

FAssetDocumentResult FAssetDocumentService::Duplicate(const FAssetDocumentDuplicateRequest& Request)
{
	return UE::AssetDocument::DuplicateAsset(Request);
}

FAssetDocumentRuntimeResultDto IAssetDocumentGenericRuntimeRoute::DispatchSync(const TSharedPtr<FJsonObject>& Options)
{
	FAssetDocumentRuntimeResultDto Result;
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = TEXT("/");
	Diagnostic.Code = TEXT("GenericRuntime.Inactive");
	Diagnostic.Message = TEXT("This runtime route does not serve sidecar sync evidence.");
	Result.Diagnostics.Add(MoveTemp(Diagnostic));
	return Result;
}

namespace ServiceLocal
{
FAssetDocumentRuntimeResultDto MakeServiceRuntimeFailure(const TCHAR* Code, const FString& Message)
{
	FAssetDocumentRuntimeResultDto Result;
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = TEXT("/");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	Result.Diagnostics.Add(MoveTemp(Diagnostic));
	return Result;
}
} // namespace ServiceLocal

FAssetDocumentRuntimeResultDto FAssetDocumentService::DispatchSidecarSync(const TSharedRef<FJsonObject>& Options) const
{
	const TSharedPtr<IAssetDocumentGenericRuntimeRoute> Route = GenericRuntimeRoute.Pin();
	if (!Route.IsValid())
	{
		return ServiceLocal::MakeServiceRuntimeFailure(TEXT("GenericRuntime.Inactive"),
			TEXT("The AssetDocument Python runtime is not available."));
	}
	return Route->DispatchSync(Options);
}

FAssetDocumentRuntimeResultDto FAssetDocumentService::ExtractForMirror(const FString& PackageName) const
{
	using namespace ServiceLocal;
	const TSharedPtr<IAssetDocumentGenericRuntimeRoute> Route = GenericRuntimeRoute.Pin();
	if (!Route.IsValid())
	{
		return MakeServiceRuntimeFailure(TEXT("GenericRuntime.Inactive"), TEXT("The AssetDocument Python runtime is not available."));
	}
	const UObject* Asset = LoadAssetFromPackageOrObjectPath(PackageName);
	if (Asset == nullptr)
	{
		return MakeServiceRuntimeFailure(TEXT("Sync.AssetUnreadable"), FString::Printf(TEXT("Failed to load asset '%s'"), *PackageName));
	}
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = Asset->GetPathName();
	Request.bDiffOnly = true;
	return Route->DispatchExtract(Request, Asset->GetClass()->GetPathName());
}

void FAssetDocumentService::NoteSidecarRewrite(const FString& SidecarPath, const FString& ContentHash)
{
	const FString NormalizedFilePath = ServiceLocal::NormalizeValidateFilePath(SidecarPath);
	SidecarApplyLedger->AddContentHash(NormalizedFilePath, ContentHash);
}
