// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Import/AssetImportTool.h"

#include "Algo/AnyOf.h"
#include "AssetCompilingManager.h"
#include "AssetDocumentEditorNonInteractiveScope.h"
#include "AssetDocumentEditorObjectProperties.h"
#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Dom/JsonValue.h"
#include "EditorReimportHandler.h"
#include "Engine/Texture.h"
#include "Factories/Factory.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "InterchangeManager.h"
#include "InterchangePipelineBase.h"
#include "InterchangeProjectSettings.h"
#include "InterchangeResult.h"
#include "InterchangeResultsContainer.h"
#include "InterchangeSourceData.h"
#include "InterchangeTranslatorBase.h"
#include "LevelEditTypes.h"
#include "LevelEditValues.h"
#include "LevelQuery.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Modules/ModuleManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"

namespace UE::AssetDocumentLevel::Private
{
namespace
{
namespace ObjectProperties = UE::AssetDocument::EditorObjectProperties;

FAssetDocumentDiagnostic Error(const FString& Path, const TCHAR* Code, const FString& Message, TSharedPtr<FJsonObject> Details = nullptr)
{
	return MakeLevelDiagnostic(Path, Code, Message, MoveTemp(Details));
}

FAssetDocumentDiagnostic Warning(const FString& Path, const TCHAR* Code, const FString& Message)
{
	FAssetDocumentDiagnostic Diagnostic = MakeLevelDiagnostic(Path, Code, Message);
	Diagnostic.Severity = TEXT("warning");
	return Diagnostic;
}

TArray<TSharedPtr<FJsonValue>> ImportStringsJson(const TArray<FString>& Strings)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FString& String : Strings)
	{
		Values.Add(MakeShared<FJsonValueString>(String));
	}
	return Values;
}

// ---------------------------------------------------------------------------------------------
// Request

// A friendly option: the pipeline property paths it sets (the first path a pipeline of the
// file's stack has wins), or a texture property set after the import.
struct FFriendlyOption
{
	const TCHAR* Name;
	EJson Type;
	TArray<TArray<FString>> Paths;
	bool bTexture = false;
};

const TArray<FFriendlyOption>& FriendlyOptions()
{
	static const TArray<FFriendlyOption> Options = {
		{TEXT("skeleton"), EJson::String, {{TEXT("CommonSkeletalMeshesAndAnimationsProperties"), TEXT("Skeleton")}}},
		{TEXT("animations_only"), EJson::Boolean, {{TEXT("CommonSkeletalMeshesAndAnimationsProperties"), TEXT("bImportOnlyAnimations")}}},
		{TEXT("mesh_type"), EJson::String, {{TEXT("CommonMeshesProperties"), TEXT("ForceAllMeshAsType")}}},
		{TEXT("combine_meshes"), EJson::Boolean, {{TEXT("MeshPipeline"), TEXT("bCombineStaticMeshes")},
			{TEXT("MeshPipeline"), TEXT("CombineStaticMeshesBehavior")}}},
		{TEXT("collision"), EJson::Boolean, {{TEXT("MeshPipeline"), TEXT("bCollision")}}},
		{TEXT("collision_type"), EJson::String, {{TEXT("MeshPipeline"), TEXT("Collision")}}},
		{TEXT("create_physics_asset"), EJson::Boolean, {{TEXT("MeshPipeline"), TEXT("bCreatePhysicsAsset")}}},
		{TEXT("nanite"), EJson::Boolean, {{TEXT("MeshPipeline"), TEXT("bBuildNanite")}}},
		{TEXT("import_materials"), EJson::Boolean, {{TEXT("MaterialPipeline"), TEXT("bImportMaterials")}}},
		{TEXT("import_textures"), EJson::Boolean, {{TEXT("MaterialPipeline"), TEXT("TexturePipeline"), TEXT("bImportTextures")},
			{TEXT("bImportTextures")}}},
		{TEXT("import_animations"), EJson::Boolean, {{TEXT("AnimationPipeline"), TEXT("bImportAnimations")}}},
		{TEXT("uniform_scale"), EJson::Number, {{TEXT("ImportOffsetUniformScale")}}},
		{TEXT("flip_normal_green"), EJson::Boolean, {{TEXT("MaterialPipeline"), TEXT("TexturePipeline"), TEXT("bFlipNormalMapGreenChannel")},
			{TEXT("bFlipNormalMapGreenChannel")}}},
		{TEXT("srgb"), EJson::Boolean, {}, true},
		{TEXT("compression"), EJson::String, {}, true},
		{TEXT("normal_map"), EJson::Boolean, {}, true},
	};
	return Options;
}

const FFriendlyOption* FindFriendlyOption(const FString& Name)
{
	return FriendlyOptions().FindByPredicate([&Name](const FFriendlyOption& Option) { return Name == Option.Name; });
}

FString FriendlyOptionNames()
{
	TArray<FString> Names;
	for (const FFriendlyOption& Option : FriendlyOptions())
	{
		Names.Add(Option.Name);
	}
	return FString::Join(Names, TEXT(", "));
}

struct FImportRequest
{
	FString Action = TEXT("import");
	TArray<FString> Files;
	FString Destination;
	FString Name;
	TSharedPtr<FJsonObject> Options;
	TSharedPtr<FJsonObject> Pipeline;
	bool bReplaceExisting = false;
	TArray<FString> Assets;
	FString Source;
	bool bSave = true;
};

bool ReadStringArray(const FJsonObject& Json, const TCHAR* Field, TArray<FString>& Out, TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Json.TryGetArrayField(Field, Values))
	{
		Diagnostics.Add(Error(FString(TEXT("/")) + Field, TEXT("Import.InvalidRequest"), FString::Printf(TEXT("'%s' must be a list of "
			"strings."), Field)));
		return false;
	}
	for (int32 Index = 0; Index < Values->Num(); ++Index)
	{
		FString Text;
		if (!(*Values)[Index].IsValid() || !(*Values)[Index]->TryGetString(Text) || Text.IsEmpty())
		{
			Diagnostics.Add(Error(FString::Printf(TEXT("/%s/%d"), Field, Index), TEXT("Import.InvalidRequest"), FString::Printf(
				TEXT("'%s' must be a list of non-empty strings."), Field)));
			return false;
		}
		Out.Add(Text);
	}
	return true;
}

bool ParseRequest(const FJsonObject& Json, FImportRequest& Out, TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	static const TSet<FString> KnownFields = {TEXT("action"), TEXT("files"), TEXT("destination"), TEXT("name"), TEXT("options"),
		TEXT("pipeline"), TEXT("replace_existing"), TEXT("assets"), TEXT("source"), TEXT("save")};
	for (const auto& Field : Json.Values)
	{
		if (!KnownFields.Contains(FString(*Field.Key)))
		{
			Diagnostics.Add(Error(TEXT("/") + FString(*Field.Key), TEXT("Import.InvalidRequest"), FString::Printf(TEXT("Unknown field '%s' (fields: "
				"action, files, destination, name, options, pipeline, replace_existing, assets, source, save)."), *Field.Key)));
		}
	}
	if (!Diagnostics.IsEmpty())
	{
		return false;
	}
	if (Json.HasField(TEXT("action")) && !Json.TryGetStringField(TEXT("action"), Out.Action))
	{
		Diagnostics.Add(Error(TEXT("/action"), TEXT("Import.InvalidRequest"), TEXT("'action' must be a string.")));
		return false;
	}
	if (Out.Action != TEXT("import") && Out.Action != TEXT("reimport") && Out.Action != TEXT("options"))
	{
		Diagnostics.Add(Error(TEXT("/action"), TEXT("Import.InvalidRequest"), FString::Printf(TEXT("'action' is \"import\", \"reimport\" "
			"or \"options\", not '%s'."), *Out.Action)));
		return false;
	}
	auto ReadString = [&Json, &Diagnostics](const TCHAR* Field, FString& Value)
	{
		if (Json.HasField(Field) && !Json.TryGetStringField(Field, Value))
		{
			Diagnostics.Add(Error(FString(TEXT("/")) + Field, TEXT("Import.InvalidRequest"), FString::Printf(TEXT("'%s' must be a string."),
				Field)));
		}
	};
	auto ReadBool = [&Json, &Diagnostics](const TCHAR* Field, bool& Value)
	{
		if (Json.HasField(Field) && !Json.TryGetBoolField(Field, Value))
		{
			Diagnostics.Add(Error(FString(TEXT("/")) + Field, TEXT("Import.InvalidRequest"), FString::Printf(TEXT("'%s' must be a boolean."),
				Field)));
		}
	};
	auto ReadObject = [&Json, &Diagnostics](const TCHAR* Field, TSharedPtr<FJsonObject>& Value)
	{
		if (Json.HasField(Field))
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!Json.TryGetObjectField(Field, Object))
			{
				Diagnostics.Add(Error(FString(TEXT("/")) + Field, TEXT("Import.InvalidRequest"), FString::Printf(TEXT("'%s' must be an "
					"object."), Field)));
				return;
			}
			Value = *Object;
		}
	};
	ReadString(TEXT("destination"), Out.Destination);
	ReadString(TEXT("name"), Out.Name);
	ReadString(TEXT("source"), Out.Source);
	ReadBool(TEXT("replace_existing"), Out.bReplaceExisting);
	ReadBool(TEXT("save"), Out.bSave);
	ReadObject(TEXT("options"), Out.Options);
	ReadObject(TEXT("pipeline"), Out.Pipeline);
	if (Json.HasField(TEXT("files")))
	{
		ReadStringArray(Json, TEXT("files"), Out.Files, Diagnostics);
	}
	if (Json.HasField(TEXT("assets")))
	{
		ReadStringArray(Json, TEXT("assets"), Out.Assets, Diagnostics);
	}
	if (!Diagnostics.IsEmpty())
	{
		return false;
	}

	if (Out.Action == TEXT("reimport"))
	{
		if (Out.Assets.IsEmpty())
		{
			Diagnostics.Add(Error(TEXT("/assets"), TEXT("Import.InvalidRequest"), TEXT("reimport needs 'assets': the /Game assets to "
				"reimport from their source files.")));
		}
		for (const TCHAR* Field : {TEXT("files"), TEXT("destination"), TEXT("name"), TEXT("pipeline"), TEXT("replace_existing")})
		{
			if (Json.HasField(Field))
			{
				Diagnostics.Add(Error(FString(TEXT("/")) + Field, TEXT("Import.InvalidRequest"), FString::Printf(TEXT("'%s' does not "
					"apply to reimport: a reimport uses the import settings stored in the asset (as the editor's Reimport); 'source' "
					"reimports from another file."), Field)));
			}
		}
		if (!Out.Source.IsEmpty() && Out.Assets.Num() != 1)
		{
			Diagnostics.Add(Error(TEXT("/source"), TEXT("Import.InvalidRequest"), TEXT("'source' reimports one asset from a new file; "
				"list exactly one asset.")));
		}
	}
	else
	{
		if (Out.Files.IsEmpty())
		{
			Diagnostics.Add(Error(TEXT("/files"), TEXT("Import.InvalidRequest"), TEXT("'files' lists the source files to import "
				"(absolute local paths the user named).")));
		}
		if (Out.Action == TEXT("options") && Out.Files.Num() > 1)
		{
			Diagnostics.Add(Error(TEXT("/files"), TEXT("Import.InvalidRequest"), TEXT("options describes one file at a time.")));
		}
		if (Out.Action == TEXT("import") && Out.Destination.IsEmpty())
		{
			Diagnostics.Add(Error(TEXT("/destination"), TEXT("Import.InvalidRequest"), TEXT("'destination' is the /Game folder to import "
				"into (e.g. /Game/Characters/Hero).")));
		}
		if (!Out.Name.IsEmpty() && Out.Files.Num() > 1)
		{
			Diagnostics.Add(Error(TEXT("/name"), TEXT("Import.InvalidRequest"), TEXT("'name' renames the asset of one file; import one "
				"file per request to name it.")));
		}
		for (const TCHAR* Field : {TEXT("assets"), TEXT("source")})
		{
			if (Json.HasField(Field))
			{
				Diagnostics.Add(Error(FString(TEXT("/")) + Field, TEXT("Import.InvalidRequest"), FString::Printf(TEXT("'%s' belongs to "
					"action \"reimport\"."), Field)));
			}
		}
	}
	if (Out.Options.IsValid())
	{
		for (const auto& Option : Out.Options->Values)
		{
			const FFriendlyOption* Known = FindFriendlyOption(FString(*Option.Key));
			const FString Pointer = TEXT("/options/") + EscapePointerSegment(FString(*Option.Key));
			if (Known == nullptr)
			{
				Diagnostics.Add(Error(Pointer, TEXT("Import.UnknownOption"), FString::Printf(TEXT("Unknown option '%s' (options: %s; any "
					"other import dialog setting goes in 'pipeline', see action \"options\")."), *Option.Key, *FriendlyOptionNames())));
			}
			else if (!Option.Value.IsValid() || Option.Value->Type != Known->Type)
			{
				Diagnostics.Add(Error(Pointer, TEXT("Import.InvalidOption"), FString::Printf(TEXT("Option '%s' takes a %s."), *Option.Key,
					Known->Type == EJson::Boolean ? TEXT("boolean") : Known->Type == EJson::Number ? TEXT("number") : TEXT("string"))));
			}
			else if (Out.Action == TEXT("reimport") && !Known->bTexture)
			{
				Diagnostics.Add(Error(Pointer, TEXT("Import.InvalidRequest"), FString::Printf(TEXT("Option '%s' does not apply to reimport "
					"(it uses the settings stored in the asset); only the texture options srgb, compression and normal_map do."),
					*Option.Key)));
			}
		}
		FString MeshType;
		if (Out.Options->TryGetStringField(TEXT("mesh_type"), MeshType) && MeshType != TEXT("auto") && MeshType != TEXT("static")
			&& MeshType != TEXT("skeletal"))
		{
			Diagnostics.Add(Error(TEXT("/options/mesh_type"), TEXT("Import.InvalidOption"), TEXT("'mesh_type' is \"auto\", \"static\" or "
				"\"skeletal\".")));
		}
	}
	return Diagnostics.IsEmpty();
}

// ---------------------------------------------------------------------------------------------
// Sources and routes

// Where a source path points. Separators are unified first, so "\\server\share", "//server/share", "/\server/share" and the
// Windows long-path form "\\?\UNC\server\share" all classify the same.
enum class ESourceKind : uint8
{
	Local,
	// A URL, or a path on a network share.
	NonLocal,
	// A Windows device-namespace path on a drive ("\\?\C:\..."): not supported, the plain drive path names the same file.
	DeviceDrive,
};

ESourceKind ClassifySource(const FString& Path)
{
	if (Path.Contains(TEXT("://")))
	{
		return ESourceKind::NonLocal;
	}
	FString Unified = Path;
	Unified.ReplaceInline(TEXT("\\"), TEXT("/"));
	if (!Unified.StartsWith(TEXT("//")))
	{
		return ESourceKind::Local;
	}
	const bool bDevice = Unified.StartsWith(TEXT("//?/")) || Unified.StartsWith(TEXT("//./"));
	const bool bDeviceUnc = Unified.StartsWith(TEXT("//?/UNC/"), ESearchCase::IgnoreCase)
		|| Unified.StartsWith(TEXT("//./UNC/"), ESearchCase::IgnoreCase);
	return bDevice && !bDeviceUnc ? ESourceKind::DeviceDrive : ESourceKind::NonLocal;
}

bool IsNonLocalSource(const FString& Path)
{
	return ClassifySource(Path) != ESourceKind::Local;
}

bool CheckSourceFile(const FString& Path, const FString& Pointer, FString& OutFull, TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	const auto Refuse = [&Pointer, &Diagnostics, &Path](ESourceKind Kind)
	{
		if (Kind == ESourceKind::DeviceDrive)
		{
			Diagnostics.Add(Error(Pointer, TEXT("Import.SourceNotLocal"), FString::Printf(TEXT("'%s' is a Windows device path, which is "
				"not supported: pass the plain absolute path (for example C:/Art/Hero.fbx)."), *Path)));
		}
		else
		{
			Diagnostics.Add(Error(Pointer, TEXT("Import.SourceNotLocal"), FString::Printf(TEXT("'%s' is not a local file: only files on this "
				"machine are imported (no URLs or network shares). Download the file first and pass its local path."), *Path)));
		}
		return false;
	};
	if (const ESourceKind Kind = ClassifySource(Path); Kind != ESourceKind::Local)
	{
		return Refuse(Kind);
	}
	if (FPaths::IsRelative(Path))
	{
		Diagnostics.Add(Error(Pointer, TEXT("Import.InvalidRequest"), FString::Printf(TEXT("'%s' is a relative path: pass the absolute "
			"path of the file."), *Path)));
		return false;
	}
	FString Full = FPaths::ConvertRelativePathToFull(Path);
	FPaths::NormalizeFilename(Full);
	if (const ESourceKind Kind = ClassifySource(Full); Kind != ESourceKind::Local)
	{
		return Refuse(Kind);
	}
	if (IFileManager::Get().DirectoryExists(*Full))
	{
		Diagnostics.Add(Error(Pointer, TEXT("Import.SourceNotFile"), FString::Printf(TEXT("'%s' is a folder; list the files to import."),
			*Path)));
		return false;
	}
	if (!IFileManager::Get().FileExists(*Full))
	{
		Diagnostics.Add(Error(Pointer, TEXT("Import.SourceNotFound"), FString::Printf(TEXT("No file '%s'."), *Path)));
		return false;
	}
	OutFull = Full;
	return true;
}

enum class EImportRoute : uint8
{
	None,
	Interchange,
	Factory,
};

struct FFileRoute
{
	EImportRoute Route = EImportRoute::None;
	// The Interchange translator class or the legacy factory class.
	FString Importer;
};

void AddExtensions(const TArray<FString>& Formats, TSet<FString>& Out)
{
	for (const FString& Format : Formats)
	{
		FString Extensions;
		Format.Split(TEXT(";"), &Extensions, nullptr);
		TArray<FString> Parts;
		(Extensions.IsEmpty() ? Format : Extensions).ParseIntoArray(Parts, TEXT(","));
		for (const FString& Part : Parts)
		{
			Out.Add(Part.TrimStartAndEnd().ToLower());
		}
	}
}

// The legacy factories the Content Browser import falls back to, best import priority first.
TArray<UFactory*> GetImportFactories(const FString& Extension)
{
	TArray<UFactory*> Factories;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (!It->IsChildOf(UFactory::StaticClass()) || It->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			continue;
		}
		UFactory* Factory = It->GetDefaultObject<UFactory>();
		if (Factory == nullptr || !Factory->bEditorImport)
		{
			continue;
		}
		TArray<FString> Extensions;
		Factory->GetSupportedFileExtensions(Extensions);
		if (Extension.IsEmpty() || Extensions.ContainsByPredicate([&Extension](const FString& Candidate)
			{
				return Candidate.Equals(Extension, ESearchCase::IgnoreCase);
			}))
		{
			Factories.Add(Factory);
		}
	}
	Factories.Sort([](const UFactory& A, const UFactory& B) { return A.ImportPriority > B.ImportPriority; });
	return Factories;
}

FFileRoute FindImportRoute(const FString& File)
{
	FFileRoute Route;
	if (UInterchangeManager::IsInterchangeImportEnabled())
	{
		UInterchangeManager& Manager = UInterchangeManager::GetInterchangeManager();
		UE::Interchange::FScopedSourceData Source(File);
		if (Manager.CanTranslateSourceData(Source.GetSourceData()))
		{
			Route.Route = EImportRoute::Interchange;
			if (UInterchangeTranslatorBase* Translator = Manager.GetTranslatorForSourceData(Source.GetSourceData()))
			{
				Route.Importer = Translator->GetClass()->GetName();
				Translator->ReleaseSource();
			}
			return Route;
		}
	}
	for (UFactory* Factory : GetImportFactories(FPaths::GetExtension(File)))
	{
		if (Factory->FactoryCanImport(File))
		{
			Route.Route = EImportRoute::Factory;
			Route.Importer = Factory->GetClass()->GetName();
			return Route;
		}
	}
	return Route;
}

TArray<FString> SupportedExtensions()
{
	TSet<FString> Extensions;
	if (UInterchangeManager::IsInterchangeImportEnabled())
	{
		AddExtensions(UInterchangeManager::GetInterchangeManager().GetSupportedFormats(EInterchangeTranslatorType::Assets), Extensions);
	}
	for (UFactory* Factory : GetImportFactories(FString()))
	{
		TArray<FString> FactoryExtensions;
		Factory->GetSupportedFileExtensions(FactoryExtensions);
		for (const FString& Extension : FactoryExtensions)
		{
			Extensions.Add(Extension.ToLower());
		}
	}
	TArray<FString> Sorted = Extensions.Array();
	Sorted.Remove(FString());
	Sorted.Sort();
	return Sorted;
}

// ---------------------------------------------------------------------------------------------
// Pipelines

struct FPipelineSet
{
	FString Stack;
	TArray<TStrongObjectPtr<UInterchangePipelineBase>> Pipelines;
};

FString PipelineName(const UInterchangePipelineBase& Pipeline)
{
#if WITH_EDITORONLY_DATA
	if (Pipeline.OriginalPipelinePath.IsValid())
	{
		return Pipeline.OriginalPipelinePath.GetAssetName();
	}
#endif
	return Pipeline.GetClass()->GetName();
}

// The project's default pipeline stack for File, as the import dialog shows it (generated
// instances of the stack's pipelines, per-translator overrides applied).
bool MakeDefaultPipelines(const FString& File, FPipelineSet& Out)
{
	UE::Interchange::FScopedSourceData Source(File);
	const FInterchangeImportSettings& Settings = FInterchangeProjectSettingsUtils::GetDefaultImportSettings(/*bIsSceneImport*/ false);
	const FName StackName = FInterchangeProjectSettingsUtils::GetDefaultPipelineStackName(/*bIsSceneImport*/ false, *Source.GetSourceData());
	const FInterchangePipelineStack* Stack = Settings.PipelineStacks.Find(StackName);
	Out.Stack = StackName.ToString();
	if (Stack == nullptr)
	{
		for (const TPair<FName, FInterchangePipelineStack>& Entry : Settings.PipelineStacks)
		{
			if (!Entry.Value.Pipelines.IsEmpty())
			{
				Stack = &Entry.Value;
				Out.Stack = Entry.Key.ToString();
				break;
			}
		}
	}
	if (Stack == nullptr)
	{
		return false;
	}
	const TArray<FSoftObjectPath>* Paths = &Stack->Pipelines;
	{
		UE::Interchange::FScopedTranslator Translator(Source.GetSourceData());
		for (const FInterchangeTranslatorPipelines& PerTranslator : Stack->PerTranslatorPipelines)
		{
			const UClass* TranslatorClass = PerTranslator.Translator.LoadSynchronous();
			if (TranslatorClass != nullptr && Translator.GetTranslator() != nullptr && Translator.GetTranslator()->IsA(TranslatorClass))
			{
				Paths = &PerTranslator.Pipelines;
				break;
			}
		}
	}
	for (const FSoftObjectPath& Path : *Paths)
	{
		if (UInterchangePipelineBase* Pipeline = UE::Interchange::GeneratePipelineInstance(Path))
		{
			Out.Pipelines.Emplace(Pipeline);
		}
	}
	return !Out.Pipelines.IsEmpty();
}

// An instanced sub-pipeline (or other subobject) the object owns under Name; null otherwise.
UObject* FindSubobject(UObject& Object, const FString& Name)
{
	const FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(Object.GetClass(), *Name);
	if (Property == nullptr || Property->IsA<FSoftObjectProperty>() || Property->IsA<FWeakObjectProperty>())
	{
		return nullptr;
	}
	UObject* Value = Property->GetObjectPropertyValue_InContainer(&Object);
	return Value != nullptr && Value->IsIn(&Object) ? Value : nullptr;
}

enum class ESetResult : uint8
{
	Set,
	NotFound,
	Failed,
};

// Some engines keep the target Skeleton of the pipeline as a soft reference; it still names an asset that must exist.
bool IsSkeletonReference(const FSoftObjectProperty& Property)
{
	return Property.PropertyClass != nullptr && Property.PropertyClass->GetFName() == TEXT("Skeleton");
}

// Loads the object a reference value names (an object path, or a package path of an asset).
UObject* LoadReference(UClass* Class, const FString& Path)
{
	UObject* Loaded = StaticLoadObject(Class, nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (Loaded == nullptr && !Path.Contains(TEXT(".")) && FPackageName::IsValidLongPackageName(Path))
	{
		Loaded = StaticLoadObject(Class, nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	return Loaded;
}

ESetResult SetValue(UObject& Object, const FString& Name, const TSharedPtr<FJsonValue>& Json, const FString& Pointer,
	TArray<FAssetDocumentDiagnostic>& Diagnostics);

// Writes every member of an object value into Object (a pipeline or sub-pipeline).
ESetResult SetMembers(UObject& Object, const FJsonObject& Members, const FString& Pointer, TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	ESetResult Result = ESetResult::Set;
	for (const auto& Member : Members.Values)
	{
		const FString MemberPointer = Pointer + TEXT("/") + EscapePointerSegment(FString(*Member.Key));
		const ESetResult Set = SetValue(Object, FString(*Member.Key), Member.Value, MemberPointer, Diagnostics);
		if (Set == ESetResult::NotFound)
		{
			Diagnostics.Add(Error(MemberPointer, TEXT("Import.UnknownPipelineProperty"), FString::Printf(TEXT("%s has no setting '%s' "
				"(action \"options\" lists them)."), *Object.GetClass()->GetName(), *Member.Key)));
			Result = ESetResult::Failed;
		}
		else if (Set == ESetResult::Failed)
		{
			Result = ESetResult::Failed;
		}
	}
	return Result;
}

ESetResult SetValue(UObject& Object, const FString& Name, const TSharedPtr<FJsonValue>& Json, const FString& Pointer,
	TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	FProperty* Property = FindFProperty<FProperty>(Object.GetClass(), *Name);
	if (Property == nullptr || !Property->HasAnyPropertyFlags(CPF_Edit))
	{
		return ESetResult::NotFound;
	}
	if (UObject* Subobject = FindSubobject(Object, Name))
	{
		if (!Json.IsValid() || Json->Type != EJson::Object)
		{
			Diagnostics.Add(Error(Pointer, TEXT("Import.InvalidOption"), FString::Printf(TEXT("'%s' is a group of settings: give an object "
				"of its settings."), *Name)));
			return ESetResult::Failed;
		}
		return SetMembers(*Subobject, *Json->AsObject(), Pointer, Diagnostics);
	}
	FObjectPropertyBase* Reference = CastField<FObjectPropertyBase>(Property);
	FSoftObjectProperty* SoftReference = CastField<FSoftObjectProperty>(Property);
	if (Reference != nullptr && (SoftReference == nullptr || IsSkeletonReference(*SoftReference)))
	{
		// An asset reference (weak, hard, or the soft target Skeleton), such as the target Skeleton.
		FString Path;
		if (Json.IsValid() && Json->Type == EJson::Null)
		{
			Path.Reset();
		}
		else if (!Json.IsValid() || !Json->TryGetString(Path))
		{
			Diagnostics.Add(Error(Pointer, TEXT("Import.InvalidOption"), FString::Printf(TEXT("'%s' takes an asset path (or null)."), *Name)));
			return ESetResult::Failed;
		}
		UObject* Loaded = nullptr;
		if (!Path.IsEmpty())
		{
			Loaded = LoadReference(Reference->PropertyClass, Path);
			if (Loaded == nullptr)
			{
				Diagnostics.Add(Error(Pointer, Reference->PropertyClass->GetFName() == TEXT("Skeleton") ? TEXT("Import.SkeletonNotFound")
					: TEXT("Import.InvalidOption"), FString::Printf(TEXT("'%s' names no %s asset (%s)."), *Path,
						*Reference->PropertyClass->GetName(), *Name)));
				return ESetResult::Failed;
			}
		}
		Object.PreEditChange(Property);
		if (SoftReference != nullptr)
		{
			SoftReference->SetPropertyValue_InContainer(&Object, FSoftObjectPtr(Loaded));
		}
		else
		{
			Reference->SetObjectPropertyValue_InContainer(&Object, Loaded);
		}
		FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
		Object.PostEditChangeProperty(Event);
		return ESetResult::Set;
	}
	ObjectProperties::FPreparedWrite Prepared;
	TArray<FAssetDocumentRuntimeDiagnostic> Problems;
	if (!ObjectProperties::Decode(Object, Name, JsonToLevelValue(Json), ObjectProperties::FReferencePolicy(), Prepared, Problems)
		|| !ObjectProperties::Commit(Prepared, Problems))
	{
		for (const FAssetDocumentRuntimeDiagnostic& Problem : Problems)
		{
			FAssetDocumentDiagnostic Projected = ProjectLevelPropertyDiagnostic(Problem, Pointer, Name);
			if (Projected.Code == TEXT("Generic.UnknownProperty"))
			{
				return ESetResult::NotFound;
			}
			Diagnostics.Add(MoveTemp(Projected));
		}
		return ESetResult::Failed;
	}
	return ESetResult::Set;
}

// Sets Path (sub-pipeline names, then the property) on the first pipeline of the set that has it.
ESetResult SetInPipelines(FPipelineSet& Set, const TArray<FString>& Path, const TSharedPtr<FJsonValue>& Value, const FString& Pointer,
	TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	for (const TStrongObjectPtr<UInterchangePipelineBase>& Pipeline : Set.Pipelines)
	{
		UObject* Target = Pipeline.Get();
		for (int32 Index = 0; Target != nullptr && Index + 1 < Path.Num(); ++Index)
		{
			Target = FindSubobject(*Target, Path[Index]);
		}
		if (Target == nullptr)
		{
			continue;
		}
		const ESetResult Result = SetValue(*Target, Path.Last(), Value, Pointer, Diagnostics);
		if (Result != ESetResult::NotFound)
		{
			return Result;
		}
	}
	return ESetResult::NotFound;
}

// The value a friendly option's path takes: where the mesh pipeline has a combine behaviour instead of a
// combine flag, true combines all static meshes and false none.
TSharedPtr<FJsonValue> FriendlyPathValue(const TArray<FString>& Path, const TSharedPtr<FJsonValue>& Value)
{
	if (!Path.IsEmpty() && Path.Last() == TEXT("CombineStaticMeshesBehavior") && Value.IsValid() && Value->Type == EJson::Boolean)
	{
		return MakeShared<FJsonValueString>(Value->AsBool() ? TEXT("All") : TEXT("DoNotCombine"));
	}
	return Value;
}

// Applies the friendly options and the raw "pipeline" settings to one file's pipelines.
// Settings no pipeline of the file has are collected in OutNotApplicable.
void ApplyPipelineSettings(FPipelineSet& Set, const FImportRequest& Request, TArray<FString>& OutNotApplicable,
	TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	if (Request.Options.IsValid())
	{
		for (const auto& Option : Request.Options->Values)
		{
			const FFriendlyOption* Known = FindFriendlyOption(FString(*Option.Key));
			if (Known == nullptr || Known->bTexture)
			{
				continue;
			}
			const FString Pointer = TEXT("/options/") + FString(*Option.Key);
			TSharedPtr<FJsonValue> Value = Option.Value;
			if (Option.Key == TEXT("mesh_type"))
			{
				const FString Type = Value->AsString();
				Value = MakeShared<FJsonValueString>(Type == TEXT("static") ? TEXT("IFMT_StaticMesh")
					: Type == TEXT("skeletal") ? TEXT("IFMT_SkeletalMesh") : TEXT("IFMT_None"));
			}
			ESetResult Result = ESetResult::NotFound;
			for (const TArray<FString>& Path : Known->Paths)
			{
				Result = SetInPipelines(Set, Path, FriendlyPathValue(Path, Value), Pointer, Diagnostics);
				if (Result != ESetResult::NotFound)
				{
					break;
				}
			}
			if (Result == ESetResult::Set && Option.Key == TEXT("collision_type"))
			{
				SetInPipelines(Set, {TEXT("MeshPipeline"), TEXT("bCollision")}, MakeShared<FJsonValueBoolean>(true), Pointer, Diagnostics);
			}
			if (Result == ESetResult::NotFound)
			{
				OutNotApplicable.Add(Pointer);
			}
		}
	}
	if (Request.Pipeline.IsValid())
	{
		for (const auto& Member : Request.Pipeline->Values)
		{
			const FString Pointer = TEXT("/pipeline/") + EscapePointerSegment(FString(*Member.Key));
			// A pipeline of the stack by name: {"DefaultGLTFPipeline": {...}}.
			const TStrongObjectPtr<UInterchangePipelineBase>* Named = Set.Pipelines.FindByPredicate(
				[&Member](const TStrongObjectPtr<UInterchangePipelineBase>& Pipeline) { return PipelineName(*Pipeline) == FString(*Member.Key); });
			if (Named != nullptr)
			{
				if (!Member.Value.IsValid() || Member.Value->Type != EJson::Object)
				{
					Diagnostics.Add(Error(Pointer, TEXT("Import.InvalidOption"), FString::Printf(TEXT("'%s' is a pipeline: give an object "
						"of its settings."), *Member.Key)));
					continue;
				}
				SetMembers(*Named->Get(), *Member.Value->AsObject(), Pointer, Diagnostics);
				continue;
			}
			if (SetInPipelines(Set, {FString(*Member.Key)}, Member.Value, Pointer, Diagnostics) == ESetResult::NotFound)
			{
				OutNotApplicable.Add(Pointer);
			}
		}
	}
}

// The editable settings of a pipeline (or sub-pipeline), sub-pipelines nested by name, as
// "pipeline" takes them.
TSharedRef<FJsonObject> DescribeSettings(UObject& Object, int32 Depth)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> It(Object.GetClass()); It; ++It)
	{
		if (!It->HasAnyPropertyFlags(CPF_Edit) || It->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient))
		{
			continue;
		}
		const FString Name = It->GetName();
		if (UObject* Subobject = FindSubobject(Object, Name))
		{
			if (Depth < 4)
			{
				Json->SetObjectField(Name, DescribeSettings(*Subobject, Depth + 1));
			}
			continue;
		}
		const FObjectPropertyBase* Reference = CastField<FObjectPropertyBase>(*It);
		if (Reference != nullptr && !It->IsA<FSoftObjectProperty>())
		{
			const UObject* Value = Reference->GetObjectPropertyValue_InContainer(&Object);
			if (Value == nullptr || !Value->IsIn(&Object))
			{
				Json->SetField(Name, Value != nullptr ? StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueString>(Value->GetPathName()))
					: StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueNull>()));
			}
			continue;
		}
		FAssetDocumentRuntimeValueDto Value;
		if (ObjectProperties::Read(Object, Name, Value))
		{
			Json->SetField(Name, LevelValueToJson(Value));
		}
	}
	return Json;
}

// ---------------------------------------------------------------------------------------------
// Import

// The audio decoder modules ([Audio] AudioInfoModules) an editor with an audio device registers
// at startup. Without them (an editor started with -nosound) a sound import hits an ensure
// in USoundWave ("Decoder for AudioFormat ... not found"); load them as the audio device would.
void EnsureAudioDecoders()
{
	static bool bLoaded = false;
	if (bLoaded)
	{
		return;
	}
	bLoaded = true;
	TArray<FString> Modules;
	if (GConfig == nullptr || !GConfig->GetArray(TEXT("Audio"), TEXT("AudioInfoModules"), Modules, GEngineIni))
	{
		Modules = {TEXT("OpusAudioDecoder"), TEXT("VorbisAudioDecoder"), TEXT("AdpcmAudioDecoder"), TEXT("BinkAudioDecoder"),
			TEXT("RadAudioDecoder")};
	}
	for (const FString& Module : Modules)
	{
		if (FModuleManager::Get().ModuleExists(*Module))
		{
			FModuleManager::Get().LoadModule(*Module);
		}
	}
}

// Warning and error log lines while an import runs (factories and Interchange log their problems).
class FImportLogCapture final : public FOutputDevice
{
public:
	FImportLogCapture()
	{
		GLog->AddOutputDevice(this);
	}

	virtual ~FImportLogCapture() override
	{
		GLog->RemoveOutputDevice(this);
	}

	virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override
	{
		const ELogVerbosity::Type Level = static_cast<ELogVerbosity::Type>(Verbosity & ELogVerbosity::VerbosityMask);
		if (Level > ELogVerbosity::Warning || Message == nullptr)
		{
			return;
		}
		FScopeLock Lock(&Mutex);
		if (Lines.Num() < 40)
		{
			Lines.AddUnique(FString::Printf(TEXT("%s: %s"), *Category.ToString(), Message));
		}
	}

	virtual bool CanBeUsedOnAnyThread() const override { return true; }
	virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

	TArray<FString> TakeLines()
	{
		FScopeLock Lock(&Mutex);
		return MoveTemp(Lines);
	}

private:
	FCriticalSection Mutex;
	TArray<FString> Lines;
};

void AddInterchangeResults(const UE::Interchange::FImportResult& Result, const FString& File, TArray<FString>& OutWarnings,
	TArray<FString>& OutErrors)
{
	const UInterchangeResultsContainer* Container = Result.GetResults();
	if (Container == nullptr)
	{
		return;
	}
	for (const UInterchangeResult* Entry : Container->GetResults())
	{
		if (Entry == nullptr || Entry->GetResultType() == EInterchangeResultType::Success)
		{
			continue;
		}
		FString Line = Entry->GetText().ToString();
		if (!Entry->DestinationAssetName.IsEmpty())
		{
			Line += FString::Printf(TEXT(" (%s)"), *Entry->DestinationAssetName);
		}
		(Entry->GetResultType() == EInterchangeResultType::Error ? OutErrors : OutWarnings).AddUnique(
			FString::Printf(TEXT("%s: %s"), *FPaths::GetCleanFilename(File), *Line));
	}
}

// Interchange reports "There was no data to import" when its pipelines made no asset of the file. For a model file
// that is a file of bones without a mesh (a skeleton-only or animation-only FBX): Interchange, as the import dialog,
// creates a Skeleton only with a Skeletal Mesh, and an animation needs a Skeleton to go onto. The explanation the
// diagnostic adds, or empty when Interchange said something else.
FString ImportNothingImportedExplanation(const FImportRequest& Request, const TArray<FString>& Errors)
{
	// Interchange reports it while parsing the file or, on some engines, when the import completes.
	const FString NoData[] = {
		NSLOCTEXT("InterchangeTaskParsing", "TranslatorNoAssetImported", "There was no data to import in the provided source data.").ToString(),
		NSLOCTEXT("InterchangeTaskCompletion", "NothingImported",
			"There was nothing to import from the provided source data using the chosen pipeline options.").ToString(),
	};
	auto SaysNoData = [&NoData](const FString& Line)
	{
		return Algo::AnyOf(NoData, [&Line](const FString& Text) { return Line.Contains(Text); });
	};
	if (!Errors.ContainsByPredicate(SaysNoData))
	{
		return FString();
	}
	if (Request.Options.IsValid() && Request.Options->HasField(TEXT("skeleton")))
	{
		return TEXT(" The file has nothing to import onto the Skeleton named by options.skeleton: no animation (or "
			"options.import_animations is false) and no mesh (or options.animations_only is true).");
	}
	return TEXT(" The file has no mesh: a file of bones only (a skeleton-only or animation-only FBX) makes no asset by itself, "
		"because a Skeleton is created with a Skeletal Mesh. To import its animations, pass options.skeleton (an existing "
		"Skeleton asset, e.g. the one of the character's Skeletal Mesh) with options.animations_only: true; to get a Skeleton, "
		"import the file that has the skinned mesh.");
}

TSet<FString> GatherAssetPaths(const FString& Folder)
{
	TSet<FString> Paths;
	TArray<FAssetData> Assets;
	FAssetRegistryModule::GetRegistry().GetAssetsByPath(FName(*Folder), Assets, /*bRecursive*/ true, /*bIncludeOnlyOnDiskAssets*/ false);
	for (const FAssetData& Asset : Assets)
	{
		Paths.Add(Asset.GetObjectPathString());
	}
	return Paths;
}

// Texture options after the import, as the texture editor sets them.
void ApplyTextureOptions(UTexture& Texture, const FImportRequest& Request, TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	if (!Request.Options.IsValid())
	{
		return;
	}
	TArray<TPair<FString, FLevelValue>> Writes;
	auto Add = [&Writes](const TCHAR* Property, const TSharedPtr<FJsonValue>& Value)
	{
		Writes.RemoveAll([Property](const TPair<FString, FLevelValue>& Entry) { return Entry.Key == Property; });
		Writes.Emplace(Property, JsonToLevelValue(Value));
	};
	bool bNormal = false;
	if (Request.Options->TryGetBoolField(TEXT("normal_map"), bNormal) && bNormal)
	{
		Add(TEXT("CompressionSettings"), MakeShared<FJsonValueString>(TEXT("TC_Normalmap")));
		Add(TEXT("SRGB"), MakeShared<FJsonValueBoolean>(false));
		Add(TEXT("LODGroup"), MakeShared<FJsonValueString>(TEXT("TEXTUREGROUP_WorldNormalMap")));
	}
	if (const TSharedPtr<FJsonValue> Srgb = Request.Options->TryGetField(TEXT("srgb")))
	{
		Add(TEXT("SRGB"), Srgb);
	}
	FString Compression;
	if (Request.Options->TryGetStringField(TEXT("compression"), Compression))
	{
		if (!Compression.StartsWith(TEXT("TC_")))
		{
			Compression = TEXT("TC_") + Compression;
		}
		Add(TEXT("CompressionSettings"), MakeShared<FJsonValueString>(Compression));
	}
	for (const TPair<FString, FLevelValue>& Write : Writes)
	{
		const FString Pointer = Write.Key == TEXT("SRGB") ? TEXT("/options/srgb")
			: Write.Key == TEXT("CompressionSettings") && Compression.IsEmpty() ? TEXT("/options/normal_map")
			: Write.Key == TEXT("CompressionSettings") ? TEXT("/options/compression") : TEXT("/options/normal_map");
		ObjectProperties::FPreparedWrite Prepared;
		TArray<FAssetDocumentRuntimeDiagnostic> Problems;
		if (!ObjectProperties::Decode(Texture, Write.Key, Write.Value, ObjectProperties::FReferencePolicy(), Prepared, Problems)
			|| !ObjectProperties::Commit(Prepared, Problems))
		{
			for (const FAssetDocumentRuntimeDiagnostic& Problem : Problems)
			{
				FAssetDocumentDiagnostic Projected = ProjectLevelPropertyDiagnostic(Problem, Pointer, Write.Key);
				Projected.Code = TEXT("Import.InvalidOption");
				Projected.Message = FString::Printf(TEXT("%s: %s"), *Texture.GetName(), *Projected.Message);
				Diagnostics.Add(MoveTemp(Projected));
			}
		}
	}
}

bool HasTextureOptions(const FImportRequest& Request)
{
	return Request.Options.IsValid() && (Request.Options->HasField(TEXT("srgb")) || Request.Options->HasField(TEXT("compression"))
		|| Request.Options->HasField(TEXT("normal_map")));
}

struct FImportOutcome
{
	TArray<TWeakObjectPtr<UObject>> Assets;
	TArray<FString> Warnings;
	TArray<FString> Errors;
	bool bDialogRefused = false;
};

TSharedRef<FJsonObject> AssetJson(UObject& Asset, const TSet<FString>& Before)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("path"), Asset.GetPathName());
	Json->SetStringField(TEXT("class"), Asset.GetClass()->GetPathName());
	Json->SetStringField(TEXT("status"), Before.Contains(Asset.GetPathName()) ? TEXT("updated") : TEXT("created"));
	Json->SetObjectField(TEXT("facts"), MakeImportedAssetFacts(Asset));
	return Json;
}

// The result shared by import and reimport: asset rows, warnings, save.
FLevelToolResult Finish(const FImportRequest& Request, TArray<FImportOutcome>& Outcomes, const TSet<FString>& Before,
	TArray<TSharedPtr<FJsonValue>> FileRows, TArray<FAssetDocumentDiagnostic> Diagnostics, int32 FailedCount)
{
	FAssetCompilingManager::Get().FinishAllCompilation();
	TArray<UObject*> Assets;
	for (const FImportOutcome& Outcome : Outcomes)
	{
		for (const TWeakObjectPtr<UObject>& Weak : Outcome.Assets)
		{
			if (UObject* Asset = Weak.Get())
			{
				Assets.AddUnique(Asset);
			}
		}
	}
	if (HasTextureOptions(Request))
	{
		bool bAnyTexture = false;
		for (UObject* Asset : Assets)
		{
			if (UTexture* Texture = Cast<UTexture>(Asset))
			{
				bAnyTexture = true;
				ApplyTextureOptions(*Texture, Request, Diagnostics);
			}
		}
		if (!bAnyTexture)
		{
			Diagnostics.Add(Warning(TEXT("/options"), TEXT("Import.OptionNotApplicable"), TEXT("The texture options (srgb, compression, "
				"normal_map) applied to nothing: no texture was imported.")));
		}
		FAssetCompilingManager::Get().FinishAllCompilation();
	}
	TArray<TSharedPtr<FJsonValue>> AssetRows;
	TArray<FString> Summary;
	for (UObject* Asset : Assets)
	{
		AssetRows.Add(MakeShared<FJsonValueObject>(AssetJson(*Asset, Before)));
		Summary.Add(FString::Printf(TEXT("%s %s"), *Asset->GetClass()->GetName(), *Asset->GetPathName()));
	}
	TArray<FString> Warnings;
	bool bDialogRefused = false;
	for (const FImportOutcome& Outcome : Outcomes)
	{
		for (const FString& Line : Outcome.Errors)
		{
			Warnings.AddUnique(TEXT("[Error] ") + Line);
		}
		for (const FString& Line : Outcome.Warnings)
		{
			Warnings.AddUnique(Line);
		}
		bDialogRefused |= Outcome.bDialogRefused;
	}
	if (bDialogRefused)
	{
		Diagnostics.Add(Warning(TEXT(""), TEXT("Import.EditorDialogRefused"), TEXT("An import step tried to open a dialog, which import "
			"refuses (it never waits for a person); check the listed assets.")));
	}

	TArray<FString> Saved;
	int32 HttpStatus = FailedCount > 0 ? 207 : 200;
	if (Request.bSave)
	{
		TArray<UPackage*> Packages;
		for (UObject* Asset : Assets)
		{
			if (Asset->GetOutermost()->IsDirty())
			{
				Packages.AddUnique(Asset->GetOutermost());
			}
		}
		if (!Packages.IsEmpty())
		{
			{
				FAssetDocumentEditorNonInteractiveScope NonInteractive;
				UEditorLoadingAndSavingUtils::SavePackages(Packages, /*bOnlyDirty*/ true);
			}
			TArray<FString> NotSaved;
			for (UPackage* Package : Packages)
			{
				(Package->IsDirty() ? NotSaved : Saved).Add(Package->GetName());
			}
			if (!NotSaved.IsEmpty())
			{
				TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
				Details->SetArrayField(TEXT("not_saved"), ImportStringsJson(NotSaved));
				Diagnostics.Add(Error(TEXT("/save"), TEXT("Import.SaveFailed"), FString::Printf(TEXT("%d imported package(s) were not saved "
					"(details.not_saved); save them from the editor."), NotSaved.Num()), Details));
				HttpStatus = 207;
			}
		}
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("action"), Request.Action);
	Payload->SetArrayField(TEXT("assets"), AssetRows);
	if (!FileRows.IsEmpty())
	{
		Payload->SetArrayField(TEXT("files"), FileRows);
	}
	Payload->SetArrayField(TEXT("warnings"), ImportStringsJson(Warnings));
	Payload->SetArrayField(TEXT("saved_packages"), ImportStringsJson(Saved));

	if (Assets.IsEmpty() && FailedCount > 0)
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics), Payload);
	}
	FString Message = FString::Printf(TEXT("%s %d asset(s)%s%s"), Request.Action == TEXT("reimport") ? TEXT("Reimported") : TEXT("Imported"),
		Assets.Num(), Summary.IsEmpty() ? TEXT("") : TEXT(": "), *FString::Join(Summary, TEXT(", ")));
	if (!Warnings.IsEmpty())
	{
		Message += FString::Printf(TEXT(" (%d warning(s), payload.warnings)"), Warnings.Num());
	}
	Message += Saved.IsEmpty() ? TEXT(".") : FString::Printf(TEXT("; saved %d package(s)."), Saved.Num());
	// An error after the import (a texture option the texture refused): imported, partly as asked.
	if (Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Severity != TEXT("warning"); }))
	{
		HttpStatus = 207;
	}
	FLevelToolResult Result = FLevelToolResult::Success(Message, Payload);
	Result.HttpStatus = HttpStatus;
	Result.bSuccess = HttpStatus == 200;
	Result.Diagnostics = FAssetDocumentResult::OrderRootCauseFirst(Diagnostics);
	return Result;
}

bool CheckDestination(const FString& Destination, TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	FText ReasonText;
	const bool bGame = Destination == TEXT("/Game") || Destination.StartsWith(TEXT("/Game/"));
	const bool bValid = bGame && !Destination.EndsWith(TEXT("/")) && !Destination.Contains(TEXT("."))
		&& FPackageName::IsValidLongPackageName(Destination + TEXT("/A"), /*bIncludeReadOnlyRoots*/ false, &ReasonText);
	if (!bValid)
	{
		const FString Reason = ReasonText.ToString();
		Diagnostics.Add(Error(TEXT("/destination"), TEXT("Import.InvalidDestination"), FString::Printf(TEXT("'%s' is not a /Game folder "
			"path (e.g. /Game/Characters/Hero)%s%s."), *Destination, Reason.IsEmpty() ? TEXT("") : TEXT(": "), *Reason)));
		return false;
	}
	return true;
}

FLevelToolResult RunImport(const FImportRequest& Request)
{
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	bool bDestinationValid = true;
	if (Request.Action == TEXT("import"))
	{
		bDestinationValid = CheckDestination(Request.Destination, Diagnostics);
		if (!Request.Name.IsEmpty() && (ObjectTools::SanitizeObjectName(Request.Name) != Request.Name || Request.Name.Contains(TEXT("/"))))
		{
			Diagnostics.Add(Error(TEXT("/name"), TEXT("Import.InvalidRequest"), FString::Printf(TEXT("'%s' is not a valid asset name (no "
				"spaces, dots, slashes or other special characters)."), *Request.Name)));
		}
	}

	// Preflight every file: local, existing, importable, no clash; its pipelines with the options.
	struct FPlannedFile
	{
		FString File;
		FFileRoute Route;
		FPipelineSet Pipelines;
		FString MainAsset;
		TArray<FString> NotApplicable;
	};
	TArray<FPlannedFile> Plan;
	for (int32 Index = 0; Index < Request.Files.Num(); ++Index)
	{
		const FString Pointer = FString::Printf(TEXT("/files/%d"), Index);
		FPlannedFile Planned;
		if (!CheckSourceFile(Request.Files[Index], Pointer, Planned.File, Diagnostics))
		{
			continue;
		}
		Planned.Route = FindImportRoute(Planned.File);
		if (Planned.Route.Route == EImportRoute::None)
		{
			TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetArrayField(TEXT("supported"), ImportStringsJson(SupportedExtensions()));
			Diagnostics.Add(Error(Pointer, TEXT("Import.FormatNotSupported"), FString::Printf(TEXT("Neither Interchange nor an import "
				"factory of this editor imports '%s' (.%s); details.supported lists the extensions it imports."),
				*FPaths::GetCleanFilename(Planned.File), *FPaths::GetExtension(Planned.File)), Details));
			continue;
		}
		if (Request.Action == TEXT("import") && bDestinationValid)
		{
			const FString AssetName = Request.Name.IsEmpty() ? ObjectTools::SanitizeObjectName(FPaths::GetBaseFilename(Planned.File))
				: Request.Name;
			Planned.MainAsset = Request.Destination / AssetName;
			if (!Request.bReplaceExisting && FPackageName::DoesPackageExist(Planned.MainAsset))
			{
				Diagnostics.Add(Error(Pointer, TEXT("Import.AssetExists"), FString::Printf(TEXT("%s already exists. Pass "
					"\"replace_existing\": true to import over it (a reimport from this file, as the editor's Yes to 'override?'), or "
					"choose another 'name' or 'destination'."), *Planned.MainAsset)));
				continue;
			}
		}
		if (Planned.Route.Route == EImportRoute::Interchange)
		{
			if (!MakeDefaultPipelines(Planned.File, Planned.Pipelines))
			{
				Diagnostics.Add(Error(Pointer, TEXT("Import.NoPipelineStack"), TEXT("The project's Interchange settings define no import "
					"pipeline stack for this file (Project Settings > Interchange).")));
				continue;
			}
			ApplyPipelineSettings(Planned.Pipelines, Request, Planned.NotApplicable, Diagnostics);
		}
		else
		{
			if (Request.Pipeline.IsValid())
			{
				Diagnostics.Add(Error(TEXT("/pipeline"), TEXT("Import.InvalidOption"), FString::Printf(TEXT("%s is imported by the legacy "
					"factory %s (Interchange does not translate it in this editor): Interchange pipeline settings do not apply."),
					*FPaths::GetCleanFilename(Planned.File), *Planned.Route.Importer)));
			}
			if (Request.Options.IsValid())
			{
				for (const auto& Option : Request.Options->Values)
				{
					const FFriendlyOption* Known = FindFriendlyOption(FString(*Option.Key));
					if (Known != nullptr && !Known->bTexture)
					{
						Planned.NotApplicable.Add(TEXT("/options/") + FString(*Option.Key));
					}
				}
			}
		}
		Plan.Add(MoveTemp(Planned));
	}
	// A setting no file's pipelines have is an error (a typo); one only some files have is a warning.
	TMap<FString, int32> NotApplicableCount;
	for (const FPlannedFile& Planned : Plan)
	{
		for (const FString& Pointer : Planned.NotApplicable)
		{
			++NotApplicableCount.FindOrAdd(Pointer);
		}
	}
	for (const TPair<FString, int32>& Entry : NotApplicableCount)
	{
		if (Entry.Value == Plan.Num() && Entry.Key.StartsWith(TEXT("/pipeline/")))
		{
			Diagnostics.Add(Error(Entry.Key, TEXT("Import.UnknownPipelineProperty"), FString::Printf(TEXT("No import pipeline of the "
				"file(s) has the setting '%s' (action \"options\" lists the settings for a file)."), *Entry.Key.RightChop(10))));
		}
		else
		{
			Diagnostics.Add(Warning(Entry.Key, TEXT("Import.OptionNotApplicable"), FString::Printf(TEXT("'%s' does not apply to %d of the "
				"%d file(s) (their import pipelines have no such setting); it was ignored for them."), *Entry.Key, Entry.Value, Plan.Num())));
		}
	}
	const bool bErrors = Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Severity != TEXT("warning");
	});
	if (bErrors)
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}

	if (Request.Action == TEXT("options"))
	{
		const FPlannedFile& Planned = Plan[0];
		TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("file"), Planned.File);
		Payload->SetStringField(TEXT("importer"), Planned.Route.Route == EImportRoute::Interchange
			? FString::Printf(TEXT("Interchange (%s)"), *Planned.Route.Importer) : FString::Printf(TEXT("Factory (%s)"), *Planned.Route.Importer));
		if (Planned.Route.Route == EImportRoute::Interchange)
		{
			Payload->SetStringField(TEXT("stack"), Planned.Pipelines.Stack);
			TSharedRef<FJsonObject> Pipelines = MakeShared<FJsonObject>();
			for (const TStrongObjectPtr<UInterchangePipelineBase>& Pipeline : Planned.Pipelines.Pipelines)
			{
				Pipelines->SetObjectField(PipelineName(*Pipeline), DescribeSettings(*Pipeline, 0));
			}
			Payload->SetObjectField(TEXT("pipelines"), Pipelines);
		}
		FLevelToolResult Result = FLevelToolResult::Success(FString::Printf(TEXT("%s imports with %s%s. payload.pipelines lists every "
			"import dialog setting; pass changes as 'pipeline' (or the friendly 'options')."), *FPaths::GetCleanFilename(Planned.File),
			*Planned.Route.Importer, Planned.Pipelines.Stack.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", stack '%s'"),
				*Planned.Pipelines.Stack)), Payload);
		Result.Diagnostics = MoveTemp(Diagnostics);
		return Result;
	}

	const TSet<FString> Before = GatherAssetPaths(Request.Destination);
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	TArray<FImportOutcome> Outcomes;
	TArray<TSharedPtr<FJsonValue>> FileRows;
	int32 Failed = 0;
	for (int32 Index = 0; Index < Plan.Num(); ++Index)
	{
		FPlannedFile& Planned = Plan[Index];
		TStrongObjectPtr<UAssetImportTask> Task(NewObject<UAssetImportTask>());
		Task->Filename = Planned.File;
		Task->DestinationPath = Request.Destination;
		Task->DestinationName = Request.Name;
		Task->bAutomated = true;
		Task->bReplaceExisting = Request.bReplaceExisting;
		Task->bSave = false;
		Task->bAsync = false;
		TStrongObjectPtr<UInterchangePipelineStackOverride> Override;
		if (Planned.Route.Route == EImportRoute::Interchange)
		{
			Override.Reset(NewObject<UInterchangePipelineStackOverride>());
			for (const TStrongObjectPtr<UInterchangePipelineBase>& Pipeline : Planned.Pipelines.Pipelines)
			{
				Override->AddPipeline(Pipeline.Get());
			}
			Task->Options = Override.Get();
		}
		FImportOutcome& Outcome = Outcomes.AddDefaulted_GetRef();
		{
			FImportLogCapture Capture;
			FAssetDocumentEditorNonInteractiveScope NonInteractive;
			AssetTools.ImportAssetTasks({Task.Get()});
			for (UObject* Object : Task->GetObjects())
			{
				if (Object != nullptr && Object->IsAsset())
				{
					Outcome.Assets.AddUnique(Object);
				}
			}
			if (Task->AsyncResults.IsValid())
			{
				AddInterchangeResults(*Task->AsyncResults, Planned.File, Outcome.Warnings, Outcome.Errors);
			}
			Outcome.bDialogRefused = NonInteractive.WasInteractionRefused();
			for (const FString& Line : Capture.TakeLines())
			{
				if (!Line.Contains(TEXT("modal window tried to take control")))
				{
					Outcome.Warnings.AddUnique(FString::Printf(TEXT("%s: %s"), *FPaths::GetCleanFilename(Planned.File), *Line));
				}
			}
		}
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("file"), Planned.File);
		Row->SetStringField(TEXT("importer"), Planned.Route.Route == EImportRoute::Interchange
			? FString::Printf(TEXT("Interchange (%s)"), *Planned.Route.Importer) : FString::Printf(TEXT("Factory (%s)"), *Planned.Route.Importer));
		if (!Planned.Pipelines.Stack.IsEmpty())
		{
			Row->SetStringField(TEXT("stack"), Planned.Pipelines.Stack);
		}
		Row->SetNumberField(TEXT("assets"), Outcome.Assets.Num());
		FileRows.Add(MakeShared<FJsonValueObject>(Row));
		if (Outcome.Assets.IsEmpty())
		{
			++Failed;
			TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetArrayField(TEXT("errors"), ImportStringsJson(Outcome.Errors.IsEmpty() ? Outcome.Warnings : Outcome.Errors));
			Diagnostics.Add(Error(FString::Printf(TEXT("/files/%d"), Index), TEXT("Import.NothingImported"), FString::Printf(TEXT("%s "
				"imported no asset%s%s%s"), *FPaths::GetCleanFilename(Planned.File), Outcome.Errors.IsEmpty() ? TEXT(".") : TEXT(": "),
				Outcome.Errors.IsEmpty() ? TEXT("") : *Outcome.Errors[0], *ImportNothingImportedExplanation(Request, Outcome.Errors)),
				Details));
		}
	}
	return Finish(Request, Outcomes, Before, MoveTemp(FileRows), MoveTemp(Diagnostics), Failed);
}

FLevelToolResult RunReimport(const FImportRequest& Request)
{
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	FString NewSource;
	if (!Request.Source.IsEmpty())
	{
		CheckSourceFile(Request.Source, TEXT("/source"), NewSource, Diagnostics);
	}
	TArray<UObject*> Targets;
	for (int32 Index = 0; Index < Request.Assets.Num(); ++Index)
	{
		const FString Pointer = FString::Printf(TEXT("/assets/%d"), Index);
		const FString& Path = Request.Assets[Index];
		UObject* Asset = LoadReference(UObject::StaticClass(), Path);
		if (Asset == nullptr || !Asset->IsAsset())
		{
			Diagnostics.Add(Error(Pointer, TEXT("Import.AssetNotFound"), FString::Printf(TEXT("No asset '%s'."), *Path)));
			continue;
		}
		if (!Asset->GetPathName().StartsWith(TEXT("/Game/")))
		{
			Diagnostics.Add(Error(Pointer, TEXT("Import.AssetReadOnly"), FString::Printf(TEXT("%s is engine or plugin content; only /Game "
				"assets are reimported."), *Asset->GetPathName())));
			continue;
		}
		TArray<FString> Sources;
		if (!FReimportManager::Instance()->CanReimport(Asset, &Sources))
		{
			Diagnostics.Add(Error(Pointer, TEXT("Import.NoSourceFile"), FString::Printf(TEXT("%s was not imported from a file (it has no "
				"source file to reimport from)."), *Asset->GetPathName())));
			continue;
		}
		if (NewSource.IsEmpty())
		{
			const FString Stored = Sources.IsEmpty() ? FString() : Sources[0];
			FString StoredFull = Stored;
			if (!Stored.IsEmpty() && !FPaths::IsRelative(Stored))
			{
				StoredFull = FPaths::ConvertRelativePathToFull(Stored);
				FPaths::NormalizeFilename(StoredFull);
			}
			if (IsNonLocalSource(Stored) || IsNonLocalSource(StoredFull))
			{
				Diagnostics.Add(Error(Pointer, TEXT("Import.SourceNotLocal"), FString::Printf(TEXT("The source file of %s (%s) is not a "
					"local file: only files on this machine are imported (no URLs, network shares or Windows device paths). Pass 'source' "
					"with a local file to reimport it from there."), *Asset->GetPathName(), *Stored)));
				continue;
			}
			if (Stored.IsEmpty() || !IFileManager::Get().FileExists(*Stored))
			{
				Diagnostics.Add(Error(Pointer, TEXT("Import.SourceNotFound"), FString::Printf(TEXT("The source file of %s (%s) no longer "
					"exists; pass 'source' to reimport it from another file."), *Asset->GetPathName(),
					Stored.IsEmpty() ? TEXT("none recorded") : *Stored)));
				continue;
			}
		}
		Targets.Add(Asset);
	}
	if (!Diagnostics.IsEmpty())
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	TSet<FString> Before;
	for (UObject* Asset : Targets)
	{
		Before.Append(GatherAssetPaths(FPackageName::GetLongPackagePath(Asset->GetOutermost()->GetName())));
	}
	TArray<FImportOutcome> Outcomes;
	TArray<TStrongObjectPtr<UObject>> Held;
	for (UObject* Asset : Targets)
	{
		Held.Emplace(Asset);
	}
	int32 Failed = 0;
	for (int32 Index = 0; Index < Held.Num(); ++Index)
	{
		UObject* Asset = Held[Index].Get();
		FImportOutcome& Outcome = Outcomes.AddDefaulted_GetRef();
		bool bFailed = false;
		{
			FImportLogCapture Capture;
			FAssetDocumentEditorNonInteractiveScope NonInteractive;
			UE::Interchange::FAssetImportResultRef Result = FReimportManager::Instance()->ReimportAsync(Asset,
				/*bAskForNewFileIfMissing*/ false, /*bShowNotification*/ false, NewSource, /*SpecifiedReimportHandler*/ nullptr,
				INDEX_NONE, /*bForceNewFile*/ false, /*bAutomated*/ true);
			Result->WaitUntilDone();
			AddInterchangeResults(*Result, Asset->GetPathName(), Outcome.Warnings, Outcome.Errors);
			if (const UInterchangeResultsContainer* Container = Result->GetResults())
			{
				for (const UInterchangeResult* Entry : Container->GetResults())
				{
					bFailed |= Entry != nullptr && Entry->IsA<UInterchangeResultError_ReimportFail>();
				}
			}
			Outcome.Assets.AddUnique(Asset);
			for (UObject* Object : Result->GetImportedObjects())
			{
				if (Object != nullptr && Object->IsAsset())
				{
					Outcome.Assets.AddUnique(Object);
				}
			}
			Outcome.bDialogRefused = NonInteractive.WasInteractionRefused();
			for (const FString& Line : Capture.TakeLines())
			{
				if (!Line.Contains(TEXT("modal window tried to take control")))
				{
					Outcome.Warnings.AddUnique(FString::Printf(TEXT("%s: %s"), *Asset->GetName(), *Line));
				}
			}
		}
		if (bFailed)
		{
			++Failed;
			Outcome.Assets.Reset();
			TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetArrayField(TEXT("errors"), ImportStringsJson(Outcome.Errors.IsEmpty() ? Outcome.Warnings : Outcome.Errors));
			Diagnostics.Add(Error(FString::Printf(TEXT("/assets/%d"), Index), TEXT("Import.ReimportFailed"), FString::Printf(TEXT("The "
				"reimport of %s failed (details.errors)."), *Asset->GetPathName()), Details));
		}
	}
	return Finish(Request, Outcomes, Before, {}, MoveTemp(Diagnostics), Failed);
}
} // namespace

FLevelToolResult RunAssetImport(const TSharedPtr<FJsonObject>& RequestJson)
{
	check(IsInGameThread());
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	FImportRequest Request;
	if (!RequestJson.IsValid() || !ParseRequest(*RequestJson, Request, Diagnostics))
	{
		if (Diagnostics.IsEmpty())
		{
			Diagnostics.Add(Error(TEXT(""), TEXT("Import.InvalidRequest"), TEXT("The request must be a JSON object.")));
		}
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	if (!CheckLevelToolAdmission(Diagnostics))
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	if (UInterchangeManager::IsImporting() && Request.Action != TEXT("options"))
	{
		return FLevelToolResult::Failure({Error(TEXT(""), TEXT("Level.EditorBusy"), TEXT("Another import is running; retry when it is "
			"done."))});
	}
	if (Request.Action != TEXT("options"))
	{
		EnsureAudioDecoders();
	}
	return Request.Action == TEXT("reimport") ? RunReimport(Request) : RunImport(Request);
}

FLevelToolResult RunAssetImportFromBody(const FString& RequestBody)
{
	TSharedPtr<FJsonObject> Json;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RequestBody);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
	{
		return FLevelToolResult::Failure({MakeLevelDiagnostic(TEXT(""), TEXT("Import.InvalidRequest"), TEXT("The request body must be a "
			"JSON object ({\"files\": [...], \"destination\": \"/Game/...\"}).")) });
	}
	return RunAssetImport(Json);
}
}
