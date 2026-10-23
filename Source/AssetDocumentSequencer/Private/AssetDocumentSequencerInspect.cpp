// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// seq.inspect.v1: the facts an author of a sequence document needs (design §3.8). Read only; never loads a map.
//
// Args {offset?}. Records (paged under the value budget):
//   {kind: "binding", path, document_path, guid, binding_kind: Possessable | Spawnable | CustomBinding | Unsupported,
//    class?, locators: [{Locator, Resolved, Object?, Actor?, Class?}]}
//        Locators are resolved in the world open in the editor (GEditor's editor world context) with
//        FUniversalObjectLocator::SyncFind, which finds without loading; a child binding's locator is resolved
//        against its parent's first resolved object, as playback does. A spawnable's parent object is its saved actor
//        template (the actor exists only while the sequence plays): those rows carry SpawnableTemplate: true.
//        Template / Camera Animation Sequences: the root is binding_kind Root (class: the spawned class); a component
//        binding is binding_kind Component with one row {Component, Resolved, Class?, SpawnableTemplate: true}: the
//        component of that name an actor of the root's class has.
//   {kind: "properties", class, properties: [{Name, Type}]}
//        The animatable properties of a bound class (Sequencer's "+ Track" property rule: Interp / setter properties
//        whose type a property track animates, ISequencerModule::CanAnimateProperty; top-level properties only).
//   {kind: "track_class", class, type, sections: [{Class, Channels: [{Name, Type}], Properties: [names]}]}
//        Per track class used in the sequence: its section classes as they occur, their channels and writable fields.
//   {kind: "director", director}  {kind: "references", assets: [{Path, Exists}]}  {kind: "status", compile}

#include "AssetDocumentSequencerPrimitives.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "ISequencerModule.h"
#include "MovieScene.h"
#include "MovieSceneBindingReferences.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSection.h"
#include "MovieSceneSpawnable.h"
#include "MovieSceneSequence.h"
#include "MovieSceneTrack.h"
#include "Bindings/MovieSceneCustomBinding.h"
#include "Bindings/MovieSceneSpawnableActorBinding.h"
#include "Bindings/MovieSceneSpawnableBinding.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "UniversalObjectLocator.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::Graph::Sequencer
{
namespace
{
FString TypeToken(const FProperty& Property)
{
	if (CastField<FFloatProperty>(&Property)) { return TEXT("Float"); }
	if (CastField<FDoubleProperty>(&Property)) { return TEXT("Double"); }
	if (CastField<FBoolProperty>(&Property)) { return TEXT("Bool"); }
	if (CastField<FEnumProperty>(&Property)) { return TEXT("Enum"); }
	if (const FByteProperty* Byte = CastField<FByteProperty>(&Property)) { return Byte->Enum != nullptr ? TEXT("Enum") : TEXT("Byte"); }
	if (CastField<FIntProperty>(&Property)) { return TEXT("Integer"); }
	if (CastField<FStrProperty>(&Property)) { return TEXT("String"); }
	if (CastField<FTextProperty>(&Property)) { return TEXT("Text"); }
	if (CastField<FObjectPropertyBase>(&Property)) { return TEXT("ObjectProperty"); }
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		const FName Name = Struct->Struct != nullptr ? Struct->Struct->GetFName() : NAME_None;
		if (Name == TEXT("Vector")) { return TEXT("Vector"); }
		if (Name == TEXT("Vector3f")) { return TEXT("FloatVector"); }
		if (Name == TEXT("Color") || Name == TEXT("LinearColor")) { return TEXT("Color"); }
		if (Name == TEXT("Rotator")) { return TEXT("Rotator"); }
		if (Name == TEXT("Transform")) { return TEXT("Transform"); }
		if (Name == TEXT("EulerTransform")) { return TEXT("EulerTransform"); }
		return Struct->Struct != nullptr ? Struct->Struct->GetPathName() : FString();
	}
	return Property.GetClass()->GetName();
}

TArray<FValue> AnimatableProperties(const UClass& Class)
{
	ISequencerModule& Sequencer = FModuleManager::LoadModuleChecked<ISequencerModule>(TEXT("Sequencer"));
	TArray<FValue> Rows;
	for (TFieldIterator<FProperty> It(&Class); It; ++It)
	{
		FProperty* Property = *It;
		if (Property->HasAnyPropertyFlags(CPF_Deprecated) || Property->HasMetaData(TEXT("DeprecatedProperty")))
		{
			continue;
		}
		const bool bKeyable = Property->HasAnyPropertyFlags(CPF_Interp) || Property->HasSetter()
			|| Class.FindFunctionByName(FName(*(TEXT("Set") + Property->GetName()))) != nullptr;
		if (!bKeyable || !Sequencer.CanAnimateProperty(Property))
		{
			continue;
		}
		FObjectBuilder Row;
		Row.AddString(TEXT("Name"), Property->GetName());
		Row.AddString(TEXT("Type"), TypeToken(*Property));
		Rows.Add(Row.Build());
	}
	return Rows;
}

FString LocatorText(const FUniversalObjectLocator& Locator)
{
	return LocatorString(Locator);
}

FString PossessedClass(const FMovieScenePossessable& Possessable)
{
	static const FSoftClassProperty* Property = FindFProperty<FSoftClassProperty>(FMovieScenePossessable::StaticStruct(), TEXT("PossessedObjectClass"));
	return Property != nullptr ? Property->GetPropertyValue_InContainer(&Possessable).ToSoftObjectPath().ToString() : FString();
}

// The root spawnable and the component bindings of a template sequence.
void AddTemplateBindings(const FSequenceModel& Model, TArray<FValue>& Records, TArray<const UClass*>& OutClasses)
{
	UMovieScene& MovieScene = *const_cast<UMovieScene*>(Model.MovieScene);
	const FMovieSceneSpawnable* Spawnable = Model.Root.IsValid() ? MovieScene.FindSpawnable(Model.Root) : nullptr;
	const AActor* Actor = Spawnable != nullptr ? Cast<AActor>(Spawnable->GetObjectTemplate()) : nullptr;
	for (const FGuid& Guid : Model.Order)
	{
		const FBindingInfo& Info = Model.Bindings[Guid];
		FObjectBuilder Record;
		Record.AddString(TEXT("kind"), TEXT("binding"));
		Record.AddString(TEXT("path"), Info.Path);
		Record.AddString(TEXT("document_path"), Info.DocumentPath);
		Record.AddString(TEXT("guid"), Guid.ToString(EGuidFormats::Digits));
		Record.AddString(TEXT("binding_kind"), Info.bRoot ? TEXT("Root") : TEXT("Component"));
		TArray<FValue> Rows;
		if (Info.bRoot)
		{
			if (Actor != nullptr)
			{
				Record.AddString(TEXT("class"), Actor->GetClass()->GetPathName());
				OutClasses.AddUnique(Actor->GetClass());
			}
		}
		else
		{
			const FMovieScenePossessable* Possessable = MovieScene.FindPossessable(Guid);
			const FString ClassPath = Possessable != nullptr ? PossessedClass(*Possessable) : FString();
			if (!ClassPath.IsEmpty())
			{
				Record.AddString(TEXT("class"), ClassPath);
			}
			const FString Component = TemplateComponentName(*Model.Sequence, Guid);
			UClass* Found = Actor != nullptr && !Component.IsEmpty() && Info.Parent == Model.Root
				? FindTemplateComponentClass(*Actor, FName(*Component)) : nullptr;
			FObjectBuilder Row;
			Row.AddString(TEXT("Component"), Component);
			Row.AddBool(TEXT("Resolved"), Found != nullptr);
			if (Found != nullptr)
			{
				Row.AddString(TEXT("Class"), Found->GetPathName());
				Row.AddBool(TEXT("SpawnableTemplate"), true);
				OutClasses.AddUnique(Found);
			}
			Rows.Add(Row.Build());
		}
		Record.Add(TEXT("locators"), MakeArray(MoveTemp(Rows)));
		Records.Add(Record.Build());
	}
}

void AddBindings(const FSequenceModel& Model, TArray<FValue>& Records, TArray<const UClass*>& OutClasses)
{
	if (Model.bTemplate)
	{
		AddTemplateBindings(Model, Records, OutClasses);
		return;
	}
	UWorld* World = GEditor != nullptr ? GEditor->GetEditorWorldContext().World() : nullptr;
	const FMovieSceneBindingReferences* References = Model.Sequence->GetBindingReferences();
	UMovieScene& MovieScene = *const_cast<UMovieScene*>(Model.MovieScene);
	TMap<FGuid, UObject*> Resolved;
	TSet<FGuid> InTemplate;
	for (const FGuid& Guid : Model.Order)
	{
		const FBindingInfo& Info = Model.Bindings[Guid];
		const TArrayView<const FMovieSceneBindingReference> Refs = References != nullptr ? References->GetReferences(Guid)
			: TArrayView<const FMovieSceneBindingReference>();
		int32 Custom = 0;
		for (const FMovieSceneBindingReference& Reference : Refs)
		{
			Custom += Reference.CustomBinding != nullptr ? 1 : 0;
		}
		FString Kind = TEXT("Possessable");
		const UClass* Class = nullptr;
		if (Custom == 1 && Refs.Num() == 1)
		{
			Kind = Refs[0].CustomBinding->IsA<UMovieSceneSpawnableActorBinding>() ? TEXT("Spawnable") : TEXT("CustomBinding");
			Class = Refs[0].CustomBinding->GetBoundObjectClass();
			UMovieSceneSpawnableBindingBase* Spawnable = Cast<UMovieSceneSpawnableBindingBase>(Refs[0].CustomBinding.Get());
			if (UObject* Template = Spawnable != nullptr ? Spawnable->GetObjectTemplate() : nullptr)
			{
				Resolved.Add(Guid, Template);
				InTemplate.Add(Guid);
			}
		}
		else if (Custom > 0)
		{
			Kind = TEXT("Unsupported");
		}
		FObjectBuilder Record;
		Record.AddString(TEXT("kind"), TEXT("binding"));
		Record.AddString(TEXT("path"), Info.Path);
		Record.AddString(TEXT("document_path"), Info.DocumentPath);
		Record.AddString(TEXT("guid"), Guid.ToString(EGuidFormats::Digits));
		Record.AddString(TEXT("binding_kind"), Kind);
		const FMovieScenePossessable* Possessable = MovieScene.FindPossessable(Guid);
		const FString ClassPath = Class != nullptr ? Class->GetPathName() : Possessable != nullptr ? PossessedClass(*Possessable) : FString();
		if (!ClassPath.IsEmpty())
		{
			Record.AddString(TEXT("class"), ClassPath);
			if (Class == nullptr)
			{
				Class = LoadObject<UClass>(nullptr, *ClassPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
			}
		}
		if (Class != nullptr)
		{
			OutClasses.AddUnique(Class);
		}
		TArray<FValue> Locators;
		const bool bChild = Info.Parent.IsValid() && Model.Bindings.Contains(Info.Parent);
		UObject* Context = bChild ? Resolved.FindRef(Info.Parent) : static_cast<UObject*>(World);
		const bool bInTemplate = bChild && InTemplate.Contains(Info.Parent);
		for (const FMovieSceneBindingReference& Reference : Refs)
		{
			if (Reference.CustomBinding != nullptr)
			{
				continue;
			}
			FObjectBuilder Row;
			Row.AddString(TEXT("Locator"), LocatorText(Reference.Locator));
			UObject* Object = Context != nullptr ? Reference.Locator.SyncFind(Context) : nullptr;
			Row.AddBool(TEXT("Resolved"), Object != nullptr);
			if (Object != nullptr && bInTemplate)
			{
				Row.AddBool(TEXT("SpawnableTemplate"), true);
			}
			if (Object != nullptr)
			{
				Row.AddString(TEXT("Object"), Object->GetPathName());
				Row.AddString(TEXT("Class"), Object->GetClass()->GetPathName());
				if (const AActor* Actor = Cast<AActor>(Object))
				{
					Row.AddString(TEXT("Actor"), Actor->GetActorLabel());
				}
				if (!Resolved.Contains(Guid))
				{
					Resolved.Add(Guid, Object);
					if (bInTemplate)
					{
						InTemplate.Add(Guid);
					}
				}
			}
			Locators.Add(Row.Build());
		}
		Record.Add(TEXT("locators"), MakeArray(MoveTemp(Locators)));
		Records.Add(Record.Build());
	}
}

void AddTrackClasses(const FSequenceModel& Model, TArray<FValue>& Records)
{
	TArray<const UClass*> TrackClasses;
	TMap<const UClass*, TArray<const UMovieSceneSection*>> Sections;
	for (const FTrackInfo& Track : Model.Tracks)
	{
		const UClass* Class = Track.Track->GetClass();
		TrackClasses.AddUnique(Class);
		TArray<const UMovieSceneSection*>& Samples = Sections.FindOrAdd(Class);
		for (const UMovieSceneSection* Section : Track.Track->GetAllSections())
		{
			if (Section != nullptr && !Samples.ContainsByPredicate([Section](const UMovieSceneSection* Sample) { return Sample->GetClass() == Section->GetClass(); }))
			{
				Samples.Add(Section);
			}
		}
	}
	for (const UClass* Class : TrackClasses)
	{
		TArray<FValue> Rows;
		for (const UMovieSceneSection* Section : Sections[Class])
		{
			TArray<FString> Fields;
			for (TFieldIterator<FProperty> It(Section->GetClass()); It; ++It)
			{
				if (IsSectionPropertyField(**It))
				{
					Fields.Add(It->GetName());
				}
			}
			FObjectBuilder Row;
			Row.AddString(TEXT("Class"), Section->GetClass()->GetPathName());
			Row.AddString(TEXT("Token"), SectionClassToken(*Section->GetClass(), *Class));
			TArray<TSharedPtr<FJsonValue>> Channels = DescribeChannels(*Section);
			Row.Add(TEXT("Channels"), JsonToValue(MakeShared<FJsonValueArray>(Channels)));
			Row.Add(TEXT("Properties"), MakeStringArray(Fields));
			Rows.Add(Row.Build());
		}
		FObjectBuilder Record;
		Record.AddString(TEXT("kind"), TEXT("track_class"));
		Record.AddString(TEXT("class"), Class->GetPathName());
		Record.AddString(TEXT("type"), TrackTypeToken(*Class));
		Record.Add(TEXT("sections"), MakeArray(MoveTemp(Rows)));
		Records.Add(Record.Build());
	}
}

void AddReferences(const UMovieSceneSequence& Sequence, TArray<FValue>& Records)
{
	IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
	TArray<FName> Dependencies;
	Registry.GetDependencies(Sequence.GetOutermost()->GetFName(), Dependencies);
	Dependencies.Sort(FNameLexicalLess());
	TArray<FValue> Rows;
	for (const FName& Package : Dependencies)
	{
		const FString Name = Package.ToString();
		if (Name.StartsWith(TEXT("/Script/")))
		{
			continue;
		}
		FObjectBuilder Row;
		Row.AddString(TEXT("Path"), Name);
		Row.AddBool(TEXT("Exists"), FPackageName::DoesPackageExist(Name));
		Rows.Add(Row.Build());
	}
	FObjectBuilder Record;
	Record.AddString(TEXT("kind"), TEXT("references"));
	Record.Add(TEXT("assets"), MakeArray(MoveTemp(Rows)));
	Records.Add(Record.Build());
}
} // namespace

bool HandleInspect(FCall& Call, FObjectBuilder& Result)
{
	int64 Offset = 0;
	if (!Call.Args.OnlyFields({TEXT("offset")}) || !Call.Args.Int(TEXT("offset"), Offset, false))
	{
		return false;
	}
	if (Offset < 0)
	{
		return Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("offset must be >= 0."), TEXT("offset"));
	}
	const UMovieSceneSequence* Sequence = Cast<UMovieSceneSequence>(&Call.Asset);
	if (Sequence == nullptr)
	{
		return Fail(Call, TEXT("Sequencer.UnsupportedAsset"), TEXT("seq.inspect.v1 reads Level and Template Sequences."), TEXT("/HandleArguments/asset"));
	}
	FSequenceModel Model;
	FString Error;
	if (!BuildModel(*Sequence, Model, Error))
	{
		return Fail(Call, TEXT("Sequencer.ReadFailed"), Error);
	}
	TArray<FValue> Records;
	TArray<const UClass*> Classes;
	AddBindings(Model, Records, Classes);
	for (const UClass* Class : Classes)
	{
		FObjectBuilder Record;
		Record.AddString(TEXT("kind"), TEXT("properties"));
		Record.AddString(TEXT("class"), Class->GetPathName());
		Record.Add(TEXT("properties"), MakeArray(AnimatableProperties(*Class)));
		Records.Add(Record.Build());
	}
	AddTrackClasses(Model, Records);
	{
		FObjectBuilder Record;
		Record.AddString(TEXT("kind"), TEXT("director"));
		Record.Add(TEXT("director"), DescribeDirector(*Sequence));
		Records.Add(Record.Build());
	}
	AddReferences(*Sequence, Records);
	{
		FValue Status;
		DescribeStatusValue(*Sequence, Status);
		FObjectBuilder Record;
		Record.AddString(TEXT("kind"), TEXT("status"));
		Record.Add(TEXT("compile"), MoveTemp(Status));
		Records.Add(Record.Build());
	}
	AddRecordPage(Records, Offset, Result);
	return true;
}
} // namespace UE::AssetDocument::Graph::Sequencer
