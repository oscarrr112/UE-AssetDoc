// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Applying a dependency must leave the assets that use it on its current types, as the editor's
// compile of the dependency does: no object, widget, notify or property of a dependent is left on a
// replaced class or struct (REINST_ / TRASHCLASS_ / STRUCT_REINST_ / a class with a newer version),
// and a struct property keeps the struct's current size.
//
// Each family is built by documents, saved, unloaded and loaded back from disk (an editor restart);
// then the dependency is changed twice by apply (structural edits that recompile it):
//   NestedUserWidget  a Widget Blueprint whose tree holds a child Widget Blueprint (with a value set
//                     on the nested widget); the child gains variables.
//   UserDefinedTypes  a Blueprint with variables of a user struct, of a user struct holding it and of
//                     a user enum; the struct gains members, the enum gains enumerators.
//   AnimNotifyClass   a montage and a sequence holding instances of a Blueprint AnimNotify and a
//                     Blueprint AnimNotifyState (with a value set); the notify Blueprints gain variables.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "AssetDocumentTypes.h"

#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Algo/AnyOf.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "Engine/UserDefinedEnum.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "ReferenceSkeleton.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"
#include "Utils/AssetDocumentEngineCompat.h"
#include "WidgetBlueprint.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentDependentClassTests
{
FAssetDocumentService& Service()
{
	return FAssetDocumentModule::Get().GetService();
}

struct FAsset
{
	FString Name;
	FString PackageName;
	FString ObjectPath;
	FString ClassPath;
	FString SidecarPath;

	FAsset(const FString& Root, const TCHAR* InName)
		: Name(InName)
		, PackageName(Root / InName)
		, ObjectPath(PackageName + TEXT(".") + InName)
		, ClassPath(ObjectPath + TEXT("_C"))
		, SidecarPath(FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(PackageName, TEXT(".assetdoc.json"))))
	{
	}

	UObject* Get() const { return FSoftObjectPath(ObjectPath).ResolveObject(); }
	template <typename T> T* Get() const { return Cast<T>(Get()); }
	UClass* Generated() const { const UBlueprint* Blueprint = Get<UBlueprint>(); return Blueprint ? Blueprint->GeneratedClass.Get() : nullptr; }
	UPackage* Package() const { return FindPackage(nullptr, *PackageName); }
};

bool IsReplacedName(const FString& Name)
{
	for (const TCHAR* Prefix : {TEXT("REINST_"), TEXT("TRASHCLASS_"), TEXT("HOTRELOADED_"), TEXT("STRUCT_REINST_"), TEXT("TRASHSTRUCT_"), TEXT("PLACEHOLDER-")})
	{
		if (Name.StartsWith(Prefix))
		{
			return true;
		}
	}
	return false;
}

// A class, struct or enum that a recompile replaced (or is about to collect).
bool IsReplaced(const UObject* Type)
{
	if (Type == nullptr)
	{
		return false;
	}
	if (!IsValid(Type))
	{
		return true;
	}
	const UClass* Class = Cast<UClass>(Type);
	return (Class != nullptr && Class->HasAnyClassFlags(CLASS_NewerVersionExists)) || IsReplacedName(Type->GetName())
		|| Type->GetOutermost() == GetTransientPackage();
}

void CollectStaleProperty(const FProperty* Property, const FString& Owner, TArray<FString>& Out)
{
	if (Property == nullptr)
	{
		return;
	}
	const auto Note = [Property, &Owner, &Out](const UObject* Type)
	{
		if (IsReplaced(Type))
		{
			Out.Add(FString::Printf(TEXT("property %s:%s refers to replaced %s"), *Owner, *Property->GetName(), *GetPathNameSafe(Type)));
		}
	};
	if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(Property))
	{
		Note(Object->PropertyClass);
	}
	if (const FClassProperty* Class = CastField<FClassProperty>(Property))
	{
		Note(Class->MetaClass);
	}
	if (const FSoftClassProperty* SoftClass = CastField<FSoftClassProperty>(Property))
	{
		Note(SoftClass->MetaClass);
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
	{
		Note(Struct->Struct);
		if (Struct->Struct != nullptr && Struct->GetElementSize() != Struct->Struct->GetStructureSize())
		{
			Out.Add(FString::Printf(TEXT("property %s:%s has size %d, its struct %s has size %d"), *Owner, *Property->GetName(),
				Struct->GetElementSize(), *GetPathNameSafe(Struct->Struct), Struct->Struct->GetStructureSize()));
		}
	}
	if (const FByteProperty* Byte = CastField<FByteProperty>(Property))
	{
		Note(Byte->Enum);
	}
	if (const FEnumProperty* Enum = CastField<FEnumProperty>(Property))
	{
		Note(Enum->GetEnum());
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(Property))
	{
		CollectStaleProperty(Array->Inner, Owner, Out);
	}
	if (const FSetProperty* Set = CastField<FSetProperty>(Property))
	{
		CollectStaleProperty(Set->ElementProp, Owner, Out);
	}
	if (const FMapProperty* Map = CastField<FMapProperty>(Property))
	{
		CollectStaleProperty(Map->KeyProp, Owner, Out);
		CollectStaleProperty(Map->ValueProp, Owner, Out);
	}
}

// Every live object of the package that is an instance of a replaced class, and every property of
// the package's classes, functions and structs that refers to a replaced type.
TArray<FString> StaleInPackage(const UPackage* Package)
{
	TArray<FString> Out;
	if (Package == nullptr)
	{
		return Out;
	}
	ForEachObjectWithPackage(Package, [&Out](UObject* Object)
	{
		if (!IsValid(Object))
		{
			return true;
		}
		if (IsReplaced(Object->GetClass()))
		{
			Out.Add(FString::Printf(TEXT("%s is an instance of replaced %s"), *Object->GetPathName(), *GetPathNameSafe(Object->GetClass())));
		}
		if (const UStruct* Struct = Cast<UStruct>(Object))
		{
			for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::ExcludeSuper); It; ++It)
			{
				CollectStaleProperty(*It, Struct->GetPathName(), Out);
			}
		}
		return true;
	});
	return Out;
}

bool ReadFloat(const UObject* Object, const TCHAR* Name, double& OutValue)
{
	const FNumericProperty* Property = Object ? CastField<FNumericProperty>(Object->GetClass()->FindPropertyByName(Name)) : nullptr;
	if (Property == nullptr || !Property->IsFloatingPoint())
	{
		return false;
	}
	OutValue = Property->GetFloatingPointPropertyValue(Property->ContainerPtrToValuePtr<void>(Object));
	return true;
}

struct FFixture
{
	FAutomationTestBase& Test;
	const FString Root = FString(TEXT("/Game/AssetDocumentTests/DependentClasses/")) + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	TArray<FString> PackageNames;

	explicit FFixture(FAutomationTestBase& InTest)
		: Test(InTest)
	{
	}

	~FFixture()
	{
		TArray<UPackage*> Packages;
		for (const FString& Name : PackageNames)
		{
			if (UPackage* Package = FindPackage(nullptr, *Name))
			{
				Package->SetDirtyFlag(false);
				Packages.Add(Package);
			}
		}
		if (!Packages.IsEmpty())
		{
			UPackageTools::UnloadPackages(Packages);
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		TArray<FString> Files;
		for (const FString& Name : PackageNames)
		{
			Files.Add(FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(Name, FPackageName::GetAssetPackageExtension())));
		}
		IFileManager::Get().DeleteDirectory(*FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(Root)), false, true);
		FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles(Files);
	}

	FAsset Add(const TCHAR* Name)
	{
		FAsset Asset(Root, Name);
		PackageNames.Add(Asset.PackageName);
		return Asset;
	}

	FAssetDocumentResult Apply(const FAsset& Asset, const FString& Json, const FString& Stage) const
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Asset.SidecarPath), true);
		FFileHelper::SaveStringToFile(Json, *Asset.SidecarPath);
		FAssetDocumentApplyFileRequest Request;
		Request.FilePath = Asset.SidecarPath;
		Request.bSaveAsset = true;
		const FAssetDocumentResult Result = Service().ApplyFile(Request);
		Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *Result.Message));
		for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
		{
			Test.AddInfo(FString::Printf(TEXT("%s: [%s] %s %s %s"), *Stage, *Diagnostic.Severity, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
		}
		Test.TestFalse(*FString::Printf(TEXT("%s: no unrepairable stale template"), *Stage),
			Result.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& D) { return D.Code == TEXT("Graph.DependentTemplateStale"); }));
		return Result;
	}

	bool ApplyOk(const FAsset& Asset, const FString& Json, const FString& Stage) const
	{
		return Test.TestTrue(*FString::Printf(TEXT("%s: applies"), *Stage), Apply(Asset, Json, Stage).IsSuccess());
	}

	bool Save(UObject& Object) const
	{
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		const FString File = FPaths::ConvertRelativePathToFull(
			FPackageName::LongPackageNameToFilename(Object.GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension()));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
		return Test.TestTrue(*FString::Printf(TEXT("save %s"), *Object.GetName()), UPackage::SavePackage(Object.GetOutermost(), &Object, *File, Args));
	}

	// Every fixture asset is saved; all are unloaded (each asset object is released), then LoadBack
	// are loaded from disk (they load what they use), as after an editor restart. MayStay names
	// assets the unload does not always release (user-defined structs and enums).
	bool Reload(std::initializer_list<const FAsset*> LoadBack, std::initializer_list<const FAsset*> MayStay = {}) const
	{
		TArray<UPackage*> Packages;
		TArray<TPair<FString, TWeakObjectPtr<UObject>>> Assets;
		for (const FString& Name : PackageNames)
		{
			if (UPackage* Package = FindPackage(nullptr, *Name))
			{
				if (!Test.TestFalse(*FString::Printf(TEXT("%s is saved"), *Name), Package->IsDirty()))
				{
					return false;
				}
				Packages.Add(Package);
				ForEachObjectWithPackage(Package, [&Assets, &Name](UObject* Object)
				{
					Assets.Emplace(Name, Object);
					return true;
				}, AssetDocumentObjects::DirectOnly);
			}
		}
		// The undo history holds the objects the applies replaced; an editor restart has none.
		if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting && !GEditor->IsTransactionActive()
			&& GEditor->Trans->GetQueueLength() > 0)
		{
			GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentDependentClassTests", "Reload", "AssetDocument dependent class test reload"));
		}
		UPackageTools::UnloadPackages(Packages);
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		for (const TPair<FString, TWeakObjectPtr<UObject>>& Asset : Assets)
		{
			const bool bMayStay = Algo::AnyOf(MayStay, [&Asset](const FAsset* Kept) { return Kept->PackageName == Asset.Key; });
			if (bMayStay)
			{
				Test.AddInfo(FString::Printf(TEXT("%s %s"), *Asset.Key, Asset.Value.IsValid() ? TEXT("stays loaded") : TEXT("is unloaded")));
			}
			else if (!Test.TestFalse(*FString::Printf(TEXT("%s is unloaded"), *Asset.Key), Asset.Value.IsValid()))
			{
				return false;
			}
		}
		for (const FAsset* Asset : LoadBack)
		{
			if (!Test.TestNotNull(*FString::Printf(TEXT("%s loads from disk"), *Asset->Name), LoadObject<UObject>(nullptr, *Asset->ObjectPath)))
			{
				return false;
			}
		}
		return true;
	}

	// The dependency's own class is current, and no dependent package holds anything stale.
	void CheckPackages(const FString& Stage, std::initializer_list<const FAsset*> Dependents) const
	{
		for (const FAsset* Asset : Dependents)
		{
			const TArray<FString> Stale = StaleInPackage(Asset->Package());
			if (!Test.TestEqual(*FString::Printf(TEXT("%s: %s holds nothing of a replaced type"), *Stage, *Asset->Name), Stale.Num(), 0))
			{
				for (const FString& Entry : Stale)
				{
					Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *Entry));
				}
			}
		}
	}

	bool CheckCurrentClass(const FString& Stage, const FAsset& Dependency, const UClass* Class, const TCHAR* What) const
	{
		const UClass* Current = Dependency.Generated();
		if (Test.TestTrue(*FString::Printf(TEXT("%s: %s is of the current %s class"), *Stage, What, *Dependency.Name),
			Class != nullptr && Current != nullptr && Class == Current && !IsReplaced(Class)))
		{
			return true;
		}
		Test.AddInfo(FString::Printf(TEXT("%s: %s class %s, current %s"), *Stage, What, *GetPathNameSafe(Class), *GetPathNameSafe(Current)));
		return false;
	}
};

FString BlueprintDocument(const FAsset& Target, const TCHAR* Class, const TCHAR* Action, const FString& Body)
{
	return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"%s\",\"Action\":\"%s\",\"Body\":{%s}}"),
		*Target.PackageName, Class, Action, *Body);
}

// Revision N of a dependency's variables: the first one, then one more per revision.
FString Variables(int32 Revision, const TCHAR* First)
{
	FString Out = FString::Printf(TEXT("\"Variables\":[%s"), First);
	for (int32 Index = 1; Index <= Revision; ++Index)
	{
		Out += FString::Printf(TEXT(",{\"Name\":\"Added%d\",\"Type\":\"int\",\"DefaultValue\":%d}"), Index, Index);
	}
	return Out + TEXT("]");
}
} // namespace AssetDocumentDependentClassTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentDependentClassNestedUserWidgetTest,
	"UEAssetDoc.Blueprint.DependentClasses.NestedUserWidget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentDependentClassNestedUserWidgetTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentDependentClassTests;
	FFixture Fixture(*this);
	const FAsset Child = Fixture.Add(TEXT("WBP_Child"));
	const FAsset Parent = Fixture.Add(TEXT("WBP_Parent"));
	const TCHAR* WidgetBlueprintClass = TEXT("/Script/UMGEditor.WidgetBlueprint");
	const auto ChildDocument = [&](const TCHAR* Action, int32 Revision)
	{
		return BlueprintDocument(Child, WidgetBlueprintClass, Action, TEXT("\"ParentClass\":\"/Script/UMG.UserWidget\",")
			+ Variables(Revision, TEXT("{\"Name\":\"Health\",\"Type\":\"float\",\"DefaultValue\":0.5,\"InstanceEditable\":true}"))
			+ TEXT(",\"WidgetTree\":{\"RootWidget\":{\"Name\":\"Root\",\"Class\":\"/Script/UMG.CanvasPanel\",\"IsVariable\":false,\"Children\":["
				"{\"Name\":\"Label\",\"Class\":\"/Script/UMG.TextBlock\",\"IsVariable\":true}]},\"NamedSlotBindings\":{}}"));
	};
	const FString ParentDocument = BlueprintDocument(Parent, WidgetBlueprintClass, TEXT("Create"), FString::Printf(TEXT(
		"\"ParentClass\":\"/Script/UMG.UserWidget\","
		"\"WidgetTree\":{\"RootWidget\":{\"Name\":\"Root\",\"Class\":\"/Script/UMG.CanvasPanel\",\"IsVariable\":false,\"Children\":["
		"{\"Name\":\"Card\",\"Class\":\"%s\",\"IsVariable\":true,\"Properties\":{\"Health\":0.75}}]},\"NamedSlotBindings\":{}}"), *Child.ClassPath));
	if (!Fixture.ApplyOk(Child, ChildDocument(TEXT("Create"), 0), TEXT("create the child"))
		|| !Fixture.ApplyOk(Parent, ParentDocument, TEXT("create the parent holding the child"))
		|| !Fixture.Reload({&Parent}))
	{
		return false;
	}
	const auto Check = [&](const FString& Stage)
	{
		const UWidgetBlueprint* Blueprint = Parent.Get<UWidgetBlueprint>();
		const UWidget* Designer = Blueprint && Blueprint->WidgetTree ? Blueprint->WidgetTree->FindWidget(TEXT("Card")) : nullptr;
		const UWidgetBlueprintGeneratedClass* Generated = Cast<UWidgetBlueprintGeneratedClass>(Parent.Generated());
		const UWidgetTree* Archetype = Generated ? Generated->GetWidgetTreeArchetype() : nullptr;
		const UWidget* Compiled = Archetype ? Archetype->FindWidget(TEXT("Card")) : nullptr;
		if (TestNotNull(*FString::Printf(TEXT("%s: the designer tree holds Card"), *Stage), Designer))
		{
			Fixture.CheckCurrentClass(Stage, Child, Designer->GetClass(), TEXT("the designer Card widget"));
			double Health = 0.0;
			TestTrue(*FString::Printf(TEXT("%s: the designer Card keeps its Health"), *Stage), ReadFloat(Designer, TEXT("Health"), Health) && Health == 0.75);
		}
		if (TestNotNull(*FString::Printf(TEXT("%s: the generated class's tree holds Card"), *Stage), Compiled))
		{
			Fixture.CheckCurrentClass(Stage, Child, Compiled->GetClass(), TEXT("the generated Card widget"));
			double Health = 0.0;
			TestTrue(*FString::Printf(TEXT("%s: the generated Card keeps its Health"), *Stage), ReadFloat(Compiled, TEXT("Health"), Health) && Health == 0.75);
		}
		const FObjectPropertyBase* Variable = Generated ? CastField<FObjectPropertyBase>(Generated->FindPropertyByName(TEXT("Card"))) : nullptr;
		if (TestNotNull(*FString::Printf(TEXT("%s: the parent has a Card variable"), *Stage), Variable))
		{
			Fixture.CheckCurrentClass(Stage, Child, Variable->PropertyClass, TEXT("the Card variable"));
		}
		TestTrue(*FString::Printf(TEXT("%s: the parent compiles"), *Stage), Blueprint && Blueprint->Status != BS_Error);
		Fixture.CheckPackages(Stage, {&Parent});
	};
	Check(TEXT("loaded from disk"));
	if (Fixture.ApplyOk(Child, ChildDocument(TEXT("Update"), 1), TEXT("child gains a variable")))
	{
		Check(TEXT("after the first child edit"));
	}
	if (Fixture.ApplyOk(Child, ChildDocument(TEXT("Update"), 2), TEXT("child gains another variable")))
	{
		Check(TEXT("after the second child edit"));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentDependentClassUserDefinedTypesTest,
	"UEAssetDoc.Blueprint.DependentClasses.UserDefinedTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentDependentClassUserDefinedTypesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentDependentClassTests;
	FFixture Fixture(*this);
	const FAsset Enum = Fixture.Add(TEXT("E_Kind"));
	const FAsset Struct = Fixture.Add(TEXT("S_Data"));
	const FAsset Outer = Fixture.Add(TEXT("S_Outer"));
	const FAsset User = Fixture.Add(TEXT("BP_User"));
	const TCHAR* StructClass = TEXT("/Script/CoreUObject.UserDefinedStruct");
	const TCHAR* EnumClass = TEXT("/Script/Engine.UserDefinedEnum");
	const auto EnumDocument = [&](const TCHAR* Action, int32 Revision)
	{
		FString Enumerators = TEXT("{\"Name\":\"Melee\"},{\"Name\":\"Bow\"}");
		for (int32 Index = 1; Index <= Revision; ++Index)
		{
			Enumerators += FString::Printf(TEXT(",{\"Name\":\"Added%d\"}"), Index);
		}
		return BlueprintDocument(Enum, EnumClass, Action, FString::Printf(TEXT("\"Enumerators\":[%s]"), *Enumerators));
	};
	const auto StructDocument = [&](const TCHAR* Action, int32 Revision)
	{
		FString Members = FString::Printf(TEXT("{\"Name\":\"Score\",\"Type\":\"int\"},{\"Name\":\"Kind\",\"Type\":\"byte:%s\"}"), *Enum.ObjectPath);
		for (int32 Index = 1; Index <= Revision; ++Index)
		{
			Members += FString::Printf(TEXT(",{\"Name\":\"Added%d\",\"Type\":\"string\"}"), Index);
		}
		return BlueprintDocument(Struct, StructClass, Action, FString::Printf(TEXT("\"Members\":[%s]"), *Members));
	};
	const FString OuterDocument = BlueprintDocument(Outer, StructClass, TEXT("Create"), FString::Printf(TEXT(
		"\"Members\":[{\"Name\":\"Inner\",\"Type\":\"struct:%s\"},{\"Name\":\"Weight\",\"Type\":\"float\"}]"), *Struct.ObjectPath));
	const FString UserDocument = BlueprintDocument(User, TEXT("/Script/Engine.Blueprint"), TEXT("Create"), FString::Printf(TEXT(
		"\"ParentClass\":\"/Script/Engine.Actor\",\"Variables\":["
		"{\"Name\":\"Data\",\"Type\":\"struct:%s\"},{\"Name\":\"Wrapped\",\"Type\":\"struct:%s\"},{\"Name\":\"Kind\",\"Type\":\"byte:%s\"}]"),
		*Struct.ObjectPath, *Outer.ObjectPath, *Enum.ObjectPath));
	if (!Fixture.ApplyOk(Enum, EnumDocument(TEXT("Create"), 0), TEXT("create the enum"))
		|| !Fixture.ApplyOk(Struct, StructDocument(TEXT("Create"), 0), TEXT("create the struct"))
		|| !Fixture.ApplyOk(Outer, OuterDocument, TEXT("create the struct holding the struct"))
		|| !Fixture.ApplyOk(User, UserDocument, TEXT("create the Blueprint using them"))
		|| !Fixture.Reload({&User}, {&Enum, &Struct, &Outer}))
	{
		return false;
	}
	const auto CheckStructProperty = [&](const FString& Stage, const UStruct* Owner, const TCHAR* Name, const FAsset& Expected)
	{
		// A user struct's member properties have generated names; the authored name is the member's.
		const FStructProperty* Property = nullptr;
		for (TFieldIterator<FStructProperty> It(Owner); Owner != nullptr && It && Property == nullptr; ++It)
		{
			Property = It->GetAuthoredName() == Name ? *It : nullptr;
		}
		const UUserDefinedStruct* Current = Expected.Get<UUserDefinedStruct>();
		if (TestNotNull(*FString::Printf(TEXT("%s: %s has struct property %s"), *Stage, *GetNameSafe(Owner), Name), Property)
			&& !TestTrue(*FString::Printf(TEXT("%s: %s:%s is the current %s at its current size"), *Stage, *GetNameSafe(Owner), Name, *Expected.Name),
				Current != nullptr && Property->Struct.Get() == Current && Property->GetElementSize() == Current->GetStructureSize()))
		{
			AddInfo(FString::Printf(TEXT("%s: struct %s size %d, current %s size %d"), *Stage, *GetPathNameSafe(Property->Struct),
				Property->GetElementSize(), *GetPathNameSafe(Current), Current ? Current->GetStructureSize() : -1));
		}
	};
	const auto Check = [&](const FString& Stage)
	{
		const UBlueprint* Blueprint = User.Get<UBlueprint>();
		TestTrue(*FString::Printf(TEXT("%s: the Blueprint is compiled"), *Stage), Blueprint && Blueprint->Status == BS_UpToDate);
		TestFalse(*FString::Printf(TEXT("%s: the Blueprint stays saved"), *Stage), Blueprint && Blueprint->GetOutermost()->IsDirty());
		for (const UStruct* Class : {static_cast<const UStruct*>(User.Generated()), static_cast<const UStruct*>(Blueprint ? Blueprint->SkeletonGeneratedClass.Get() : nullptr)})
		{
			CheckStructProperty(Stage, Class, TEXT("Data"), Struct);
			CheckStructProperty(Stage, Class, TEXT("Wrapped"), Outer);
			const FByteProperty* Kind = Class ? CastField<FByteProperty>(Class->FindPropertyByName(TEXT("Kind"))) : nullptr;
			TestTrue(*FString::Printf(TEXT("%s: %s:Kind is of the current enum"), *Stage, *GetNameSafe(Class)),
				Kind != nullptr && Kind->Enum != nullptr && Kind->Enum.Get() == Enum.Get<UUserDefinedEnum>());
		}
		CheckStructProperty(Stage, Outer.Get<UUserDefinedStruct>(), TEXT("Inner"), Struct);
		const UUserDefinedStruct* OuterStruct = Outer.Get<UUserDefinedStruct>();
		TestTrue(*FString::Printf(TEXT("%s: the outer struct is up to date"), *Stage), OuterStruct && OuterStruct->Status == UDSS_UpToDate);
		Fixture.CheckPackages(Stage, {&User, &Outer});
	};
	Check(TEXT("loaded from disk"));
	if (Fixture.ApplyOk(Struct, StructDocument(TEXT("Update"), 1), TEXT("struct gains a member")))
	{
		Check(TEXT("after the first struct edit"));
	}
	if (Fixture.ApplyOk(Enum, EnumDocument(TEXT("Update"), 1), TEXT("enum gains an enumerator")))
	{
		Check(TEXT("after the first enum edit"));
	}
	if (Fixture.ApplyOk(Struct, StructDocument(TEXT("Update"), 2), TEXT("struct gains another member")))
	{
		Check(TEXT("after the second struct edit"));
	}
	if (Fixture.ApplyOk(Enum, EnumDocument(TEXT("Update"), 2), TEXT("enum gains another enumerator")))
	{
		Check(TEXT("after the second enum edit"));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentDependentClassAnimNotifyTest,
	"UEAssetDoc.Blueprint.DependentClasses.AnimNotifyClass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentDependentClassAnimNotifyTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentDependentClassTests;
	FFixture Fixture(*this);
	const FAsset Notify = Fixture.Add(TEXT("BP_HitNotify"));
	const FAsset State = Fixture.Add(TEXT("BP_TrailState"));
	const FAsset SkeletonAsset = Fixture.Add(TEXT("Skeleton"));
	const FAsset Sequence = Fixture.Add(TEXT("Sequence"));
	const FAsset Montage = Fixture.Add(TEXT("Montage"));
	const auto NotifyDocument = [&](const FAsset& Target, const TCHAR* ParentClass, const TCHAR* Action, int32 Revision)
	{
		return BlueprintDocument(Target, TEXT("/Script/Engine.Blueprint"), Action, FString::Printf(TEXT("\"ParentClass\":\"%s\","), ParentClass)
			+ Variables(Revision, TEXT("{\"Name\":\"Strength\",\"Type\":\"float\",\"DefaultValue\":1.0,\"InstanceEditable\":true}")));
	};
	if (!Fixture.ApplyOk(Notify, NotifyDocument(Notify, TEXT("/Script/Engine.AnimNotify"), TEXT("Create"), 0), TEXT("create the notify"))
		|| !Fixture.ApplyOk(State, NotifyDocument(State, TEXT("/Script/Engine.AnimNotifyState"), TEXT("Create"), 0), TEXT("create the notify state")))
	{
		return false;
	}

	// A one-bone skeleton with DefaultSlot, a one-second sequence and a montage playing it.
	USkeleton* Skeleton = NewObject<USkeleton>(CreatePackage(*SkeletonAsset.PackageName), *SkeletonAsset.Name, RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Skeleton);
	{
		FReferenceSkeletonModifier Modifier(Skeleton);
		Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
	}
	Skeleton->RegisterSlotNode(TEXT("DefaultSlot"));
	UAnimSequence* Source = NewObject<UAnimSequence>(CreatePackage(*Sequence.PackageName), *Sequence.Name, RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Source);
	Source->SetSkeleton(Skeleton);
	{
		IAnimationDataController& Controller = Source->GetController();
		Controller.InitializeModel();
		Controller.SetFrameRate(FFrameRate(30, 1), false);
		Controller.SetNumberOfFrames(FFrameNumber(30), false);
		if (!TestTrue(TEXT("sequence root track"), Controller.AddBoneCurve(TEXT("root"), false)))
		{
			return false;
		}
		TArray<FVector3f> Positions, Scales;
		TArray<FQuat4f> Rotations;
		Positions.Init(FVector3f::ZeroVector, 31);
		Scales.Init(FVector3f::OneVector, 31);
		Rotations.Init(FQuat4f::Identity, 31);
		Controller.SetBoneTrackKeys(TEXT("root"), Positions, Rotations, Scales, false);
		Controller.NotifyPopulated();
	}
	Source->WaitOnExistingCompression();
	UAnimMontage* MontageObject = NewObject<UAnimMontage>(CreatePackage(*Montage.PackageName), *Montage.Name, RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(MontageObject);
	MontageObject->SetSkeleton(Skeleton);
	{
		IAnimationDataController& Controller = MontageObject->GetController();
		Controller.InitializeModel();
		Controller.SetFrameRate(FFrameRate(30, 1), false);
		Controller.SetNumberOfFrames(FFrameNumber(30), false);
		Controller.NotifyPopulated();
	}
	MontageObject->SlotAnimTracks.Reset();
	FSlotAnimationTrack& Slot = MontageObject->SlotAnimTracks.AddDefaulted_GetRef();
	Slot.SlotName = TEXT("DefaultSlot");
	FAnimSegment Segment;
	Segment.SetAnimReference(Source);
	Segment.StartPos = 0.f;
	Segment.AnimStartTime = 0.f;
	Segment.AnimEndTime = 1.f;
	Segment.AnimPlayRate = 1.f;
	Segment.LoopingCount = 1;
	Slot.AnimTrack.AnimSegments.Add(Segment);
	MontageObject->CompositeSections.Reset();
	MontageObject->AddAnimCompositeSection(TEXT("Default"), 0.f);
	MontageObject->RefreshCacheData();
	if (!Fixture.Save(*Skeleton) || !Fixture.Save(*Source) || !Fixture.Save(*MontageObject))
	{
		return false;
	}

	// Both anim assets hold an instance of each notify Blueprint, with Strength set on the instance.
	const auto AnimDocument = [](const FAsset& Target, const TCHAR* Class, const FString& Body)
	{
		return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"%s\",\"Action\":\"Update\",\"Definitions\":{},\"Properties\":{},\"Body\":{%s}}"),
			*Target.PackageName, Class, *Body);
	};
	const FString MontageDocument = AnimDocument(Montage, TEXT("/Script/Engine.AnimMontage"), FString::Printf(TEXT(
		"\"Notifies\":[{\"Time\":0.25,\"TrackIndex\":0,\"Object\":{\"Class\":\"%s\",\"Properties\":{\"Strength\":2.5}}}],"
		"\"NotifyStates\":[{\"Time\":0.5,\"Duration\":0.25,\"TrackIndex\":0,\"Object\":{\"Class\":\"%s\",\"Properties\":{\"Strength\":3.5}}}]"),
		*Notify.ClassPath, *State.ClassPath));
	const FString SequenceDocument = AnimDocument(Sequence, TEXT("/Script/Engine.AnimSequence"), FString::Printf(TEXT(
		"\"NotifyTracks\":[{\"Name\":\"Hits\"}],"
		"\"Notifies\":[{\"Time\":0.25,\"Track\":\"Hits\",\"Class\":\"%s\",\"Properties\":{\"Strength\":2.5}}],"
		"\"NotifyStates\":[{\"Time\":0.5,\"Duration\":0.25,\"Track\":\"Hits\",\"Class\":\"%s\",\"Properties\":{\"Strength\":3.5}}]"),
		*Notify.ClassPath, *State.ClassPath));
	if (!Fixture.ApplyOk(Montage, MontageDocument, TEXT("the montage holds the notifies"))
		|| !Fixture.ApplyOk(Sequence, SequenceDocument, TEXT("the sequence holds the notifies"))
		|| !Fixture.Reload({&Montage, &Sequence}))
	{
		return false;
	}
	const auto Check = [&](const FString& Stage)
	{
		for (const FAsset* Asset : {&Montage, &Sequence})
		{
			const UAnimSequenceBase* Anim = Asset->Get<UAnimSequenceBase>();
			if (!TestNotNull(*FString::Printf(TEXT("%s: %s is loaded"), *Stage, *Asset->Name), Anim))
			{
				continue;
			}
			int32 Notifies = 0;
			int32 States = 0;
			for (const FAnimNotifyEvent& Event : Anim->Notifies)
			{
				const UObject* Instance = Event.Notify ? static_cast<const UObject*>(Event.Notify.Get()) : static_cast<const UObject*>(Event.NotifyStateClass.Get());
				if (Instance == nullptr)
				{
					continue;
				}
				const bool bState = Event.NotifyStateClass != nullptr;
				(bState ? States : Notifies)++;
				const FString What = FString::Printf(TEXT("%s's %s instance"), *Asset->Name, bState ? TEXT("notify state") : TEXT("notify"));
				Fixture.CheckCurrentClass(Stage, bState ? State : Notify, Instance->GetClass(), *What);
				double Strength = 0.0;
				TestTrue(*FString::Printf(TEXT("%s: %s keeps its Strength"), *Stage, *What),
					ReadFloat(Instance, TEXT("Strength"), Strength) && Strength == (bState ? 3.5 : 2.5));
			}
			TestEqual(*FString::Printf(TEXT("%s: %s holds one notify"), *Stage, *Asset->Name), Notifies, 1);
			TestEqual(*FString::Printf(TEXT("%s: %s holds one notify state"), *Stage, *Asset->Name), States, 1);
		}
		Fixture.CheckPackages(Stage, {&Montage, &Sequence});
	};
	Check(TEXT("loaded from disk"));
	for (int32 Revision = 1; Revision <= 2; ++Revision)
	{
		if (Fixture.ApplyOk(Notify, NotifyDocument(Notify, TEXT("/Script/Engine.AnimNotify"), TEXT("Update"), Revision),
				FString::Printf(TEXT("notify edit %d"), Revision)))
		{
			Check(FString::Printf(TEXT("after notify edit %d"), Revision));
		}
		if (Fixture.ApplyOk(State, NotifyDocument(State, TEXT("/Script/Engine.AnimNotifyState"), TEXT("Update"), Revision),
				FString::Printf(TEXT("notify state edit %d"), Revision)))
		{
			Check(FString::Printf(TEXT("after notify state edit %d"), Revision));
		}
	}
	return !HasAnyErrors();
}
#endif
