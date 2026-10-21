// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentIKRetargetBatch.h"

#include "AssetDocumentRigPrimitives.h"

#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimationAsset.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Async/Async.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorAnimUtils.h"
#include "Engine/SkeletalMesh.h"
#include "FileHelpers.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/PackageName.h"
#include "RetargetEditor/IKRetargetBatchOperation.h"
#include "Retargeter/IKRetargeter.h"
#include "Rig/IKRigDefinition.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::AssetDocument::Graph::Rig::Batch
{
namespace RetargetBatchPrivate
{
constexpr int32 MaxAssets = 1000;
constexpr int32 MaxListed = 20;

FAssetDocumentDiagnostic Problem(const FString& Path, const FString& Code, const FString& Message)
{
	FAssetDocumentDiagnostic Diagnostic;
	Diagnostic.Path = Path;
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	Diagnostic.Severity = TEXT("error");
	return Diagnostic;
}

FAssetDocumentResult Refuse(const FString& Path, const FString& Code, const FString& Message)
{
	return FAssetDocumentResult::FailureFromDiagnostics({Problem(Path, Code, Message)}, Message);
}

FString ObjectPathOf(const FString& Path)
{
	const FString Trimmed = Path.TrimStartAndEnd();
	return Trimmed.Contains(TEXT("."))
		? Trimmed
		: FString::Printf(TEXT("%s.%s"), *Trimmed, *FPackageName::GetLongPackageAssetName(Trimmed));
}

UObject* LoadAsset(const FString& Path)
{
	const FString Trimmed = Path.TrimStartAndEnd();
	if (!Trimmed.StartsWith(TEXT("/")) || Trimmed.StartsWith(TEXT("/Script/")))
	{
		return nullptr;
	}
	return LoadObject<UObject>(nullptr, *ObjectPathOf(Trimmed), nullptr, LOAD_NoWarn | LOAD_Quiet);
}

// A package exists (on disk or in memory) at PackageName.
bool AssetExists(const FString& PackageName)
{
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	if (FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid())
	{
		return true;
	}
	return FindObject<UObject>(nullptr, *ObjectPath) != nullptr;
}

bool SkeletonMatches(const USkeleton* Expected, const UAnimationAsset& Asset)
{
	const USkeleton* Skeleton = Asset.GetSkeleton();
	return Expected != nullptr && Skeleton != nullptr && (Skeleton == Expected || Expected->IsCompatibleForEditor(Skeleton));
}

FString StripTrailingDigits(const FString& Name)
{
	int32 End = Name.Len();
	while (End > 0 && FChar::IsDigit(Name[End - 1]))
	{
		--End;
	}
	return Name.Left(End);
}

// What UIKRetargetBatchOperation::GenerateAssetLists duplicates: the inputs, each Montage's segment animations and
// preview pose, and (bIncludeReferenced) everything the assets reference (blend space samples, ...), recursively.
TArray<UAnimationAsset*> AssetsToDuplicate(const TArray<UAnimationAsset*>& Inputs, bool bIncludeReferenced)
{
	TArray<UAnimationAsset*> Assets;
	for (UAnimationAsset* Asset : Inputs)
	{
		Assets.AddUnique(Asset);
		if (const UAnimMontage* Montage = Cast<UAnimMontage>(Asset))
		{
			for (const FSlotAnimationTrack& Track : Montage->SlotAnimTracks)
			{
				for (const FAnimSegment& Segment : Track.AnimTrack.AnimSegments)
				{
					if (Segment.IsValid() && Segment.GetAnimReference() != nullptr)
					{
						Assets.AddUnique(Segment.GetAnimReference());
					}
				}
			}
			if (Montage->PreviewBasePose != nullptr)
			{
				Assets.AddUnique(Montage->PreviewBasePose);
			}
		}
	}
	if (bIncludeReferenced)
	{
		for (int32 Index = 0; Index < Assets.Num(); ++Index)
		{
			Assets[Index]->HandleAnimReferenceCollection(Assets, true);
		}
	}
	return Assets;
}

TSharedRef<FJsonObject> ResultJson(const FAssetDocumentResult& Result)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetBoolField(TEXT("success"), Result.IsSuccess());
	Json->SetStringField(TEXT("message"), Result.Message);
	TArray<TSharedPtr<FJsonValue>> Diagnostics;
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("path"), Diagnostic.Path);
		Row->SetStringField(TEXT("code"), Diagnostic.Code);
		Row->SetStringField(TEXT("message"), Diagnostic.Message);
		Diagnostics.Add(MakeShared<FJsonValueObject>(Row));
	}
	Json->SetArrayField(TEXT("diagnostics"), Diagnostics);
	if (Result.Payload.IsValid())
	{
		Json->SetObjectField(TEXT("payload"), Result.Payload);
	}
	return Json;
}

TArray<TSharedPtr<FJsonValue>> StringValues(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const FString& Value : Values)
	{
		Out.Add(MakeShared<FJsonValueString>(Value));
	}
	return Out;
}

USkeletalMesh* MeshArgument(const TSharedPtr<FJsonObject>& Request, const TCHAR* Field, USkeletalMesh* Default, TArray<FAssetDocumentDiagnostic>& Problems)
{
	FString Path;
	if (!Request->TryGetStringField(Field, Path) || Path.IsEmpty())
	{
		return Default;
	}
	USkeletalMesh* Mesh = Cast<USkeletalMesh>(LoadAsset(Path));
	if (Mesh == nullptr)
	{
		Problems.Add(Problem(FString::Printf(TEXT("/%s"), Field), TEXT("Retarget.MeshNotFound"), FString::Printf(TEXT("No skeletal mesh at '%s'."), *Path)));
	}
	return Mesh;
}
} // namespace RetargetBatchPrivate

FAssetDocumentResult RetargetAnimations(const TSharedPtr<FJsonObject>& Request)
{
	using namespace RetargetBatchPrivate;
	if (!Request.IsValid())
	{
		return Refuse(TEXT(""), TEXT("Retarget.InvalidArgument"), TEXT("The request must be a JSON object."));
	}
	if (GEditor != nullptr && GEditor->PlayWorld != nullptr)
	{
		return Refuse(TEXT(""), TEXT("Retarget.PieActive"), TEXT("Play In Editor is running: stop it (play stop) and retarget again."));
	}

	// The retargeter, its rigs and the meshes (default: the retargeter's preview meshes, as the export window).
	FString RetargeterPath;
	if (!Request->TryGetStringField(TEXT("retargeter"), RetargeterPath) || RetargeterPath.IsEmpty())
	{
		return Refuse(TEXT("/retargeter"), TEXT("Retarget.InvalidArgument"), TEXT("retargeter (an IK Retargeter path) is required."));
	}
	UIKRetargeter* Retargeter = Cast<UIKRetargeter>(LoadAsset(RetargeterPath));
	if (Retargeter == nullptr)
	{
		return Refuse(TEXT("/retargeter"), TEXT("Retarget.RetargeterNotFound"), FString::Printf(TEXT("No IK Retargeter at '%s' (find_assets class IKRetargeter)."), *RetargeterPath));
	}
	TArray<FAssetDocumentDiagnostic> Problems;
	for (const ERetargetSourceOrTarget Side : {ERetargetSourceOrTarget::Source, ERetargetSourceOrTarget::Target})
	{
		if (Retargeter->GetIKRig(Side) == nullptr)
		{
			Problems.Add(Problem(TEXT("/retargeter"), TEXT("Retarget.NoIKRig"), FString::Printf(TEXT("The retargeter has no %s IK Rig (Body.%s.IKRig in its document)."),
				Side == ERetargetSourceOrTarget::Source ? TEXT("source") : TEXT("target"), Side == ERetargetSourceOrTarget::Source ? TEXT("Source") : TEXT("Target"))));
		}
	}
	if (!Problems.IsEmpty())
	{
		return FAssetDocumentResult::FailureFromDiagnostics(Problems, TEXT("Nothing was retargeted."));
	}
	USkeletalMesh* SourceMesh = MeshArgument(Request, TEXT("source_mesh"), Retargeter->GetPreviewMesh(ERetargetSourceOrTarget::Source), Problems);
	USkeletalMesh* TargetMesh = MeshArgument(Request, TEXT("target_mesh"), Retargeter->GetPreviewMesh(ERetargetSourceOrTarget::Target), Problems);
	if (!Problems.IsEmpty())
	{
		return FAssetDocumentResult::FailureFromDiagnostics(Problems, TEXT("Nothing was retargeted."));
	}
	if (SourceMesh == nullptr || TargetMesh == nullptr)
	{
		return Refuse(SourceMesh == nullptr ? TEXT("/source_mesh") : TEXT("/target_mesh"), TEXT("Retarget.NoMesh"), FString::Printf(TEXT("No %s mesh: give %s or a preview mesh on the retargeter's %s IK Rig."),
			SourceMesh == nullptr ? TEXT("source") : TEXT("target"), SourceMesh == nullptr ? TEXT("source_mesh") : TEXT("target_mesh"), SourceMesh == nullptr ? TEXT("source") : TEXT("target")));
	}
	if (SourceMesh == TargetMesh)
	{
		return Refuse(TEXT("/target_mesh"), TEXT("Retarget.SameMesh"), FString::Printf(TEXT("Source and target mesh are both %s; the editor retargets between two meshes."), *SourceMesh->GetPathName()));
	}
	if (const TSharedPtr<FJsonObject> Status = DescribeStatus(*Retargeter))
	{
		FString State;
		if (Status->TryGetStringField(TEXT("Status"), State) && State == TEXT("Error"))
		{
			TArray<FString> Errors;
			const TArray<TSharedPtr<FJsonValue>>* Messages = nullptr;
			if (Status->TryGetArrayField(TEXT("Messages"), Messages))
			{
				for (const TSharedPtr<FJsonValue>& Message : *Messages)
				{
					const TSharedPtr<FJsonObject> Row = Message->AsObject();
					if (Row.IsValid() && Row->GetStringField(TEXT("Severity")) == TEXT("Error"))
					{
						Errors.Add(Row->GetStringField(TEXT("Message")));
					}
				}
			}
			return Refuse(TEXT("/retargeter"), TEXT("Retarget.RetargeterInvalid"), FString::Printf(TEXT("The retargeter does not initialize (inspect_asset_document_target shows its Compile): %s"),
				*FString::Join(Errors, TEXT("; "))));
		}
	}
	USkeleton* SourceSkeleton = SourceMesh->GetSkeleton();

	// The animations: a list, or the animation assets of a folder on the source skeleton.
	TArray<UAnimationAsset*> Inputs;
	TArray<TSharedPtr<FJsonValue>> Skipped;
	const TArray<TSharedPtr<FJsonValue>>* AssetValues = nullptr;
	FString Folder;
	const bool bHasAssets = Request->TryGetArrayField(TEXT("assets"), AssetValues);
	const bool bHasFolder = Request->TryGetStringField(TEXT("folder"), Folder) && !Folder.IsEmpty();
	if (bHasAssets == bHasFolder)
	{
		return Refuse(TEXT("/assets"), TEXT("Retarget.InvalidArgument"), TEXT("Give assets (animation asset paths) or folder (a content folder), one of them."));
	}
	if (bHasAssets)
	{
		for (int32 Index = 0; Index < AssetValues->Num(); ++Index)
		{
			const FString Path = FString::Printf(TEXT("/assets/%d"), Index);
			FString AssetPath;
			(*AssetValues)[Index]->TryGetString(AssetPath);
			UObject* Object = LoadAsset(AssetPath);
			UAnimationAsset* Asset = Cast<UAnimationAsset>(Object);
			if (Asset == nullptr)
			{
				Problems.Add(Problem(Path, Object == nullptr ? TEXT("Retarget.AssetNotFound") : TEXT("Retarget.NotAnimation"), Object == nullptr
					? FString::Printf(TEXT("No asset at '%s'."), *AssetPath)
					: FString::Printf(TEXT("'%s' is a %s, not an animation asset (sequence, montage, blend space, composite, pose asset)."), *AssetPath, *Object->GetClass()->GetName())));
			}
			else if (!SkeletonMatches(SourceSkeleton, *Asset))
			{
				Problems.Add(Problem(Path, TEXT("Retarget.WrongSkeleton"), FString::Printf(TEXT("'%s' uses Skeleton %s; the source mesh %s uses %s."), *AssetPath,
					*PathOf(Asset->GetSkeleton()), *SourceMesh->GetPathName(), *PathOf(SourceSkeleton))));
			}
			else
			{
				Inputs.AddUnique(Asset);
			}
		}
	}
	else
	{
		bool bRecursive = true;
		Request->TryGetBoolField(TEXT("recursive"), bRecursive);
		Folder = Folder.TrimStartAndEnd();
		Folder.RemoveFromEnd(TEXT("/"));
		FARFilter Filter;
		Filter.PackagePaths.Add(FName(*Folder));
		Filter.bRecursivePaths = bRecursive;
		Filter.ClassPaths.Add(UAnimationAsset::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		TArray<FAssetData> Found;
		FAssetRegistryModule::GetRegistry().GetAssets(Filter, Found);
		Found.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });
		for (const FAssetData& Data : Found)
		{
			UAnimationAsset* Asset = Cast<UAnimationAsset>(Data.GetAsset());
			if (Asset == nullptr)
			{
				continue;
			}
			if (!SkeletonMatches(SourceSkeleton, *Asset))
			{
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("Asset"), Asset->GetPathName());
				Row->SetStringField(TEXT("Reason"), FString::Printf(TEXT("Skeleton %s is not the source mesh's"), *PathOf(Asset->GetSkeleton())));
				Skipped.Add(MakeShared<FJsonValueObject>(Row));
				continue;
			}
			Inputs.AddUnique(Asset);
		}
		if (Inputs.IsEmpty())
		{
			Problems.Add(Problem(TEXT("/folder"), TEXT("Retarget.NoAnimations"), FString::Printf(TEXT("No animation asset on Skeleton %s under '%s'%s."),
				*PathOf(SourceSkeleton), *Folder, Skipped.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%d on other skeletons)"), Skipped.Num()))));
		}
	}
	if (!Problems.IsEmpty())
	{
		return FAssetDocumentResult::FailureFromDiagnostics(Problems, TEXT("Nothing was retargeted."));
	}
	if (Inputs.Num() > MaxAssets)
	{
		return Refuse(bHasAssets ? TEXT("/assets") : TEXT("/folder"), TEXT("Retarget.TooManyAssets"), FString::Printf(TEXT("%d animations; at most %d per call."), Inputs.Num(), MaxAssets));
	}

	// Names and folder (the window's rename rules).
	EditorAnimUtils::FNameDuplicationRule NameRule;
	Request->TryGetStringField(TEXT("prefix"), NameRule.Prefix);
	Request->TryGetStringField(TEXT("suffix"), NameRule.Suffix);
	Request->TryGetStringField(TEXT("search"), NameRule.ReplaceFrom);
	Request->TryGetStringField(TEXT("replace"), NameRule.ReplaceTo);
	if (NameRule.ReplaceFrom.IsEmpty() && !NameRule.ReplaceTo.IsEmpty())
	{
		return Refuse(TEXT("/replace"), TEXT("Retarget.InvalidArgument"), TEXT("replace needs search (the text it replaces in each name)."));
	}
	FString TargetFolder;
	const bool bUseSourcePath = !Request->TryGetStringField(TEXT("target_folder"), TargetFolder) || TargetFolder.IsEmpty();
	if (!bUseSourcePath)
	{
		TargetFolder = TargetFolder.TrimStartAndEnd();
		TargetFolder.RemoveFromEnd(TEXT("/"));
		FText Reason;
		if (!TargetFolder.StartsWith(TEXT("/Game")) || !FPackageName::IsValidLongPackageName(TargetFolder + TEXT("/A"), false, &Reason))
		{
			return Refuse(TEXT("/target_folder"), TEXT("Retarget.InvalidFolder"), FString::Printf(TEXT("target_folder '%s' must be a /Game content folder (%s)."), *TargetFolder, *Reason.ToString()));
		}
		NameRule.FolderPath = TargetFolder;
	}
	FString Existing = TEXT("error");
	Request->TryGetStringField(TEXT("existing"), Existing);
	if (Existing != TEXT("error") && Existing != TEXT("overwrite") && Existing != TEXT("unique"))
	{
		return Refuse(TEXT("/existing"), TEXT("Retarget.InvalidArgument"), FString::Printf(TEXT("existing '%s': use error, overwrite or unique."), *Existing));
	}
	bool bIncludeReferenced = true;
	Request->TryGetBoolField(TEXT("include_referenced"), bIncludeReferenced);
	bool bSave = true;
	Request->TryGetBoolField(TEXT("save"), bSave);

	// Every duplicate's name, before anything is made: existing assets are refused unless overwrite / unique.
	const TArray<UAnimationAsset*> Duplicated = AssetsToDuplicate(Inputs, bIncludeReferenced);
	TMap<FString, UAnimationAsset*> SourceByOutput;
	TArray<FString> Taken;
	for (UAnimationAsset* Asset : Duplicated)
	{
		const FString OutFolder = bUseSourcePath ? FPackageName::GetLongPackagePath(Asset->GetOutermost()->GetName()) : TargetFolder;
		const FString OutPackage = OutFolder / NameRule.Rename(Asset);
		if (UAnimationAsset** Other = SourceByOutput.Find(OutPackage))
		{
			return Refuse(TEXT("/assets"), TEXT("Retarget.NameClash"), FString::Printf(TEXT("%s and %s would both be retargeted to %s: use target folders per source folder, or prefix / suffix."),
				*(*Other)->GetPathName(), *Asset->GetPathName(), *OutPackage));
		}
		SourceByOutput.Add(OutPackage, Asset);
		if (AssetExists(OutPackage))
		{
			Taken.Add(OutPackage);
		}
	}
	if (!Taken.IsEmpty() && Existing == TEXT("error"))
	{
		TArray<FString> Listed(Taken.GetData(), FMath::Min(Taken.Num(), MaxListed));
		return Refuse(TEXT("/existing"), TEXT("Retarget.TargetExists"), FString::Printf(TEXT("%d output asset(s) exist: %s%s. Set existing: \"overwrite\" (the new asset replaces it and takes its references, as the window's Overwrite) or \"unique\" (a numbered name), or change target_folder / prefix / suffix."),
			Taken.Num(), *FString::Join(Listed, TEXT(", ")), Taken.Num() > MaxListed ? TEXT(", ...") : TEXT("")));
	}

	// Run the editor's batch operation; record the assets it creates.
	TArray<UPackage*> DirtyBefore;
	FEditorFileUtils::GetDirtyContentPackages(DirtyBefore);
	TArray<TWeakObjectPtr<UObject>> Created;
	IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
	const FDelegateHandle CreatedHandle = Registry.OnInMemoryAssetCreated().AddLambda([&Created](UObject* Object)
	{
		if (Object != nullptr && Object->IsA<UAnimationAsset>())
		{
			Created.AddUnique(Object);
		}
	});
	FIKRetargetBatchOperationContext Context;
	for (UAnimationAsset* Asset : Inputs)
	{
		Context.AssetsToRetarget.Add(Asset);
	}
	Context.SourceMesh = SourceMesh;
	Context.TargetMesh = TargetMesh;
	Context.IKRetargetAsset = Retargeter;
	Context.NameRule = NameRule;
	Context.bUseSourcePath = bUseSourcePath;
	Context.bOverwriteExistingFiles = Existing == TEXT("overwrite");
	Context.bIncludeReferencedAssets = bIncludeReferenced;
	UIKRetargetBatchOperation* Operation = NewObject<UIKRetargetBatchOperation>();
	Operation->AddToRoot();
	Operation->RunRetarget(Context);
	Operation->RemoveFromRoot();
	Registry.OnInMemoryAssetCreated().Remove(CreatedHandle);

	// Results: each new asset with the source it was made from.
	TArray<TSharedPtr<FJsonValue>> CreatedRows;
	TArray<UPackage*> ToSave;
	for (const TWeakObjectPtr<UObject>& Weak : Created)
	{
		UObject* Object = Weak.Get();
		if (Object == nullptr || !IsValid(Object))
		{
			continue;
		}
		const FString Package = Object->GetOutermost()->GetName();
		const UAnimationAsset* Source = nullptr;
		if (UAnimationAsset* const* Exact = SourceByOutput.Find(Package))
		{
			Source = *Exact;
		}
		else
		{
			for (const TPair<FString, UAnimationAsset*>& Pair : SourceByOutput)
			{
				if (FPackageName::GetLongPackagePath(Pair.Key) == FPackageName::GetLongPackagePath(Package)
					&& StripTrailingDigits(FPackageName::GetLongPackageAssetName(Pair.Key)) == StripTrailingDigits(Object->GetName()))
				{
					Source = Pair.Value;
					break;
				}
			}
		}
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Asset"), Object->GetPathName());
		Row->SetStringField(TEXT("Class"), Object->GetClass()->GetName());
		if (Source != nullptr)
		{
			Row->SetStringField(TEXT("Source"), Source->GetPathName());
		}
		CreatedRows.Add(MakeShared<FJsonValueObject>(Row));
		ToSave.AddUnique(Object->GetOutermost());
	}
	TArray<UPackage*> DirtyAfter;
	FEditorFileUtils::GetDirtyContentPackages(DirtyAfter);
	for (UPackage* Package : DirtyAfter)
	{
		if (!DirtyBefore.Contains(Package))
		{
			ToSave.AddUnique(Package); // references moved to an overwriting asset
		}
	}
	TArray<FString> Saved, NotSaved;
	if (bSave && !ToSave.IsEmpty())
	{
		UEditorLoadingAndSavingUtils::SavePackages(ToSave, false);
	}
	for (const UPackage* Package : ToSave)
	{
		(bSave && !Package->IsDirty() ? Saved : NotSaved).Add(Package->GetName());
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("Retargeter"), Retargeter->GetPathName());
	Payload->SetStringField(TEXT("SourceMesh"), SourceMesh->GetPathName());
	Payload->SetStringField(TEXT("TargetMesh"), TargetMesh->GetPathName());
	Payload->SetArrayField(TEXT("Created"), CreatedRows);
	if (Existing == TEXT("overwrite") && !Taken.IsEmpty())
	{
		Payload->SetArrayField(TEXT("Replaced"), StringValues(Taken));
	}
	if (!Skipped.IsEmpty())
	{
		Payload->SetArrayField(TEXT("Skipped"), Skipped);
	}
	Payload->SetArrayField(TEXT("Saved"), StringValues(Saved));
	if (!NotSaved.IsEmpty())
	{
		Payload->SetArrayField(TEXT("NotSaved"), StringValues(NotSaved));
	}
	if (CreatedRows.IsEmpty())
	{
		FAssetDocumentResult Result = Refuse(TEXT("/retargeter"), TEXT("Retarget.NothingCreated"), TEXT("The batch retarget created no asset (see the editor's output log)."));
		Result.Payload = Payload;
		return Result;
	}
	FAssetDocumentResult Result = FAssetDocumentResult::Success(FString::Printf(TEXT("%d animation asset(s) retargeted to %s"), CreatedRows.Num(), *TargetMesh->GetName()));
	Result.Payload = Payload;
	return Result;
}

TSharedPtr<FJsonObject> HandleRetargetRoute(const FString& RequestBody, const TMap<FString, FString>& QueryParams, int32& OutStatusCode)
{
	using namespace RetargetBatchPrivate;
	TSharedPtr<FJsonObject> Request;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(RequestBody), Request) || !Request.IsValid())
	{
		OutStatusCode = 400;
		return ResultJson(Refuse(TEXT(""), TEXT("Retarget.InvalidArgument"), TEXT("The request body must be a JSON object.")));
	}
	FAssetDocumentResult Result;
	auto Run = [&Request, &Result]() { Result = RetargetAnimations(Request); };
	if (IsInGameThread())
	{
		Run();
	}
	else
	{
		FEvent* Done = FPlatformProcess::GetSynchEventFromPool(true);
		AsyncTask(ENamedThreads::GameThread, [&Run, Done]()
		{
			Run();
			Done->Trigger();
		});
		Done->Wait();
		FPlatformProcess::ReturnSynchEventToPool(Done);
	}
	OutStatusCode = Result.IsSuccess() ? 200 : 400;
	return ResultJson(Result);
}
}
