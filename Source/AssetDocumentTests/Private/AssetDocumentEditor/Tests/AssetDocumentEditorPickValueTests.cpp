// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Values the editor's pickers cannot produce (AssetDocumentEditorPickerValues.h): a table row
// handle whose RowName is not a row of its DataTable, and an input key that is not a registered key. Each entry point
// refuses them at Validate (before anything changes) and at Apply, naming the close names:
//   - GenericProperties: a generic asset's Properties (the shared property writer), arrays included; a handle with no
//     table, or no row name, is not checked; a stored stale handle the document keeps is not refused.
//   - InputMappingContext: a mapping's Key (generic Properties of an Enhanced Input mapping context).
//   - DataTableCells: row handle and key cells; a row this document adds to the same table is a row.
//   - Blueprint: variable defaults, ClassDefaults and component templates, on create and on update.
// edit_level is covered in AssetDocumentLevel (LevelEditPickValuesTests.cpp).

#include "AssetDocumentEditorPickValueTestTypes.h"
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
namespace AssetDocumentEditorPickValueTests
{
const TCHAR* PickAssetClass = TEXT("/Script/AssetDocumentTests.AssetDocumentPickTestAsset");
const TCHAR* PickRow = TEXT("/Script/AssetDocumentTests.AssetDocumentPickTestRow");
const TCHAR* PickActor = TEXT("/Script/AssetDocumentTests.AssetDocumentPickTestActor");
const TCHAR* PickComponent = TEXT("/Script/AssetDocumentTests.AssetDocumentPickTestComponent");

struct FPickTarget
{
	FString AssetName;
	FString PackageName;
	FString ObjectPath;
	FString PackageFile;
	FString SidecarPath;

	explicit FPickTarget(const TCHAR* Prefix)
		: AssetName(FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12)))
	{
		PackageName = FString(TEXT("/Game/AssetDocumentPickValues/")) + AssetName;
		ObjectPath = PackageName + TEXT(".") + AssetName;
		PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		SidecarPath = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("AssetDocumentPickValues"), AssetName + TEXT(".assetdoc.json"));
	}

	UObject* Get() const { return FSoftObjectPath(ObjectPath).ResolveObject(); }

	FString Generic(const TCHAR* Class, const TCHAR* Action, const FString& Properties) const
	{
		return FString::Printf(TEXT("{\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\",\"Target\":\"%s\",\"Class\":\"%s\",")
			TEXT("\"Action\":\"%s\",\"Properties\":%s}"), *PackageName, Class, Action, *Properties);
	}

	FString DataTable(const TCHAR* Action, const FString& Rows) const
	{
		return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.DataTable\",\"Action\":\"%s\",")
			TEXT("\"Properties\":{},\"Body\":{\"RowStruct\":\"%s\",\"Rows\":%s}}"), *PackageName, Action, PickRow, *Rows);
	}

	FString Blueprint(const TCHAR* Action, const FString& Body) const
	{
		return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"%s\",")
			TEXT("\"Body\":{\"ParentClass\":\"%s\",%s}}"), *PackageName, Action, PickActor, *Body);
	}

	// A row handle of this table as a document writes it.
	FString Row(const TCHAR* RowName) const
	{
		return FString::Printf(TEXT("{\"DataTable\":\"%s\",\"RowName\":\"%s\"}"), *ObjectPath, RowName);
	}

	void Cleanup() const
	{
		if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting && !GEditor->IsTransactionActive())
		{
			GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentPickValueTests", "Cleanup", "AssetDocument pick value test cleanup"));
		}
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
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
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
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		IFileManager::Get().Delete(*SidecarPath, false, true);
	}
};

void Report(FAutomationTestBase& Test, const TCHAR* Stage, const FAssetDocumentResult& Result)
{
	Test.AddInfo(FString::Printf(TEXT("%s: %s"), Stage, *Result.Message));
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Test.AddInfo(FString::Printf(TEXT("%s: [%s] %s %s"), Stage, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
	}
}

FAssetDocumentResult Apply(FAutomationTestBase& Test, const FPickTarget& Target, const FString& Json, const TCHAR* Stage)
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

// The diagnostic with Code whose path contains PathPart and whose message contains MessagePart.
bool HasProblem(const FAssetDocumentResult& Result, const TCHAR* Code, const TCHAR* PathPart, const TCHAR* MessagePart)
{
	return Result.Diagnostics.ContainsByPredicate([&](const FAssetDocumentDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == Code && Diagnostic.Path.Contains(PathPart) && Diagnostic.Message.Contains(MessagePart);
		});
}

// Validate and Apply both refuse Json with Code at PathPart, the message naming MessagePart.
void TestRefused(FAutomationTestBase& Test, const FPickTarget& Target, const FString& Json, const TCHAR* Code, const TCHAR* PathPart,
	const TCHAR* MessagePart, const TCHAR* Stage)
{
	const FAssetDocumentResult Checked = Validate(Test, Json, Stage);
	Test.TestFalse(*FString::Printf(TEXT("%s: Validate refuses"), Stage), Checked.IsSuccess());
	Test.TestTrue(*FString::Printf(TEXT("%s: Validate names %s at %s with '%s'"), Stage, Code, PathPart, MessagePart),
		HasProblem(Checked, Code, PathPart, MessagePart));
	const FAssetDocumentResult Applied = Apply(Test, Target, Json, Stage);
	Test.TestFalse(*FString::Printf(TEXT("%s: Apply refuses"), Stage), Applied.IsSuccess());
	Test.TestTrue(*FString::Printf(TEXT("%s: Apply names %s"), Stage, Code), HasProblem(Applied, Code, TEXT(""), MessagePart));
}

// A table of the pick test rows: Boost, Heal and Shield.
bool MakeTable(FAutomationTestBase& Test, const FPickTarget& Table)
{
	return Test.TestTrue(TEXT("table created"), Apply(Test, Table, Table.DataTable(TEXT("Create"),
		TEXT("[{\"Name\":\"Boost\",\"Values\":{\"Amount\":1}},{\"Name\":\"Heal\",\"Values\":{\"Amount\":2}},{\"Name\":\"Shield\"}]")),
		TEXT("table")).IsSuccess());
}
} // namespace AssetDocumentEditorPickValueTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPickValuesGenericPropertiesTest,
	"UEAssetDoc.PickValues.GenericProperties",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPickValuesGenericPropertiesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorPickValueTests;
	const FPickTarget Table(TEXT("DT_Pickups"));
	const FPickTarget Asset(TEXT("DA_Pickup"));
	ON_SCOPE_EXIT
	{
		Asset.Cleanup();
		Table.Cleanup();
	};
	if (!MakeTable(*this, Table))
	{
		return false;
	}
	// "Bost" for "Boost".
	TestRefused(*this, Asset, Asset.Generic(PickAssetClass, TEXT("CreateOrUpdate"),
		FString::Printf(TEXT("{\"Pickup\":%s}"), *Table.Row(TEXT("Bost")))),
		TEXT("Property.RowNotFound"), TEXT("Pickup"), TEXT("Close: Boost"), TEXT("misspelt row"));
	TestNull(TEXT("the refused create left no asset"), Asset.Get());
	TestRefused(*this, Asset, Asset.Generic(PickAssetClass, TEXT("CreateOrUpdate"),
		FString::Printf(TEXT("{\"Rewards\":[%s,%s]}"), *Table.Row(TEXT("Heal")), *Table.Row(TEXT("Sheild")))),
		TEXT("Property.RowNotFound"), TEXT("Rewards/1"), TEXT("Close: Shield"), TEXT("array element"));
	TestRefused(*this, Asset, Asset.Generic(PickAssetClass, TEXT("CreateOrUpdate"), TEXT("{\"Key\":\"Spacbar\"}")),
		TEXT("Property.KeyNotFound"), TEXT("Key"), TEXT("SpaceBar"), TEXT("misspelt key"));

	// Valid values apply; a handle with no table, or no row name, is not checked (as before).
	TestTrue(TEXT("valid values apply"), Apply(*this, Asset, Asset.Generic(PickAssetClass, TEXT("CreateOrUpdate"),
		FString::Printf(TEXT("{\"Pickup\":%s,\"Rewards\":[%s,{\"DataTable\":null,\"RowName\":\"Anything\"},{\"DataTable\":\"%s\",\"RowName\":\"None\"}],")
			TEXT("\"Key\":\"SpaceBar\"}"), *Table.Row(TEXT("Heal")), *Table.Row(TEXT("Boost")), *Table.ObjectPath)), TEXT("valid")).IsSuccess());
	const UAssetDocumentPickTestAsset* Written = Cast<UAssetDocumentPickTestAsset>(Asset.Get());
	if (TestNotNull(TEXT("asset"), Written))
	{
		TestEqual(TEXT("row written"), Written->Pickup.RowName, FName(TEXT("Heal")));
		TestEqual(TEXT("key written"), Written->Key, EKeys::SpaceBar);
	}

	// A stale handle the asset already stores (its row was removed later) is kept: keeping a value is not writing it.
	TestTrue(TEXT("row Heal removed"), Apply(*this, Table, Table.DataTable(TEXT("CreateOrUpdate"),
		TEXT("[{\"Name\":\"Boost\",\"Values\":{\"Amount\":1}},{\"Name\":\"Shield\"}]")), TEXT("remove Heal")).IsSuccess());
	const FString Kept = Asset.Generic(PickAssetClass, TEXT("CreateOrUpdate"),
		FString::Printf(TEXT("{\"Pickup\":%s,\"Key\":\"SpaceBar\"}"), *Table.Row(TEXT("Heal"))));
	TestTrue(TEXT("the stored stale handle is kept"), Validate(*this, Kept, TEXT("kept")).IsSuccess());
	TestRefused(*this, Asset, Asset.Generic(PickAssetClass, TEXT("CreateOrUpdate"),
		FString::Printf(TEXT("{\"Pickup\":%s}"), *Table.Row(TEXT("Hea")))),
		TEXT("Property.RowNotFound"), TEXT("Pickup"), TEXT("Boost"), TEXT("a new missing row"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPickValuesInputMappingContextTest,
	"UEAssetDoc.PickValues.InputMappingContext",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPickValuesInputMappingContextTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorPickValueTests;
	const TCHAR* MappingContext = TEXT("/Script/EnhancedInput.InputMappingContext");
	if (FindObject<UClass>(nullptr, MappingContext) == nullptr)
	{
		AddError(TEXT("Enhanced Input is not loaded"));
		return false;
	}
	const FPickTarget Context(TEXT("IMC_Pickup"));
	ON_SCOPE_EXIT { Context.Cleanup(); };
	TestRefused(*this, Context, Context.Generic(MappingContext, TEXT("CreateOrUpdate"),
		TEXT("{\"DefaultKeyMappings\":{\"Mappings\":[{\"Key\":\"E\"},{\"Key\":\"Spacbar\"}]}}")),
		TEXT("Property.KeyNotFound"), TEXT("DefaultKeyMappings/Mappings/1/Key"), TEXT("SpaceBar"), TEXT("mapping key"));
	TestTrue(TEXT("valid keys apply"), Apply(*this, Context, Context.Generic(MappingContext, TEXT("CreateOrUpdate"),
		TEXT("{\"DefaultKeyMappings\":{\"Mappings\":[{\"Key\":\"E\"},{\"Key\":\"SpaceBar\"}]}}")), TEXT("valid")).IsSuccess());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPickValuesDataTableCellsTest,
	"UEAssetDoc.PickValues.DataTableCells",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPickValuesDataTableCellsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorPickValueTests;
	const FPickTarget Table(TEXT("DT_Pickups"));
	const FPickTarget Chain(TEXT("DT_Chain"));
	ON_SCOPE_EXIT
	{
		Chain.Cleanup();
		Table.Cleanup();
	};
	if (!MakeTable(*this, Table))
	{
		return false;
	}
	// The chain's cells name rows of a table that exists first.
	TestTrue(TEXT("valid cells apply"), Apply(*this, Chain, Chain.DataTable(TEXT("CreateOrUpdate"), FString::Printf(
		TEXT("[{\"Name\":\"A\",\"Values\":{\"Next\":%s,\"Key\":\"SpaceBar\"}}]"), *Table.Row(TEXT("Boost")))), TEXT("valid")).IsSuccess());
	TestRefused(*this, Chain, Chain.DataTable(TEXT("CreateOrUpdate"), FString::Printf(
		TEXT("[{\"Name\":\"A\",\"Values\":{\"Next\":%s}},{\"Name\":\"B\",\"Values\":{\"Next\":%s}}]"),
		*Table.Row(TEXT("Boost")), *Table.Row(TEXT("Bost")))),
		TEXT("Property.RowNotFound"), TEXT("/Body/Rows/1"), TEXT("Close: Boost"), TEXT("row handle cell"));
	TestRefused(*this, Chain, Chain.DataTable(TEXT("CreateOrUpdate"), TEXT("[{\"Name\":\"A\",\"Values\":{\"Key\":\"Spacbar\"}}]")),
		TEXT("Property.KeyNotFound"), TEXT("/Body/Rows/0"), TEXT("SpaceBar"), TEXT("key cell"));

	// A row naming a row this document adds to the same table: Validate's trial copy stands in for the table.
	const FString SelfReference = Table.DataTable(TEXT("CreateOrUpdate"), FString::Printf(
		TEXT("[{\"Name\":\"Boost\",\"Values\":{\"Amount\":1,\"Next\":%s}},{\"Name\":\"Heal\",\"Values\":{\"Amount\":2}},{\"Name\":\"Shield\"},")
		TEXT("{\"Name\":\"Mega\"}]"), *Table.Row(TEXT("Mega"))));
	TestTrue(TEXT("a row added by the same document validates"), Validate(*this, SelfReference, TEXT("self reference")).IsSuccess());
	TestTrue(TEXT("and applies"), Apply(*this, Table, SelfReference, TEXT("self reference apply")).IsSuccess());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPickValuesBlueprintTest,
	"UEAssetDoc.PickValues.Blueprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPickValuesBlueprintTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorPickValueTests;
	const FPickTarget Table(TEXT("DT_Pickups"));
	const FPickTarget Blueprint(TEXT("BP_Pickup"));
	ON_SCOPE_EXIT
	{
		Blueprint.Cleanup();
		Table.Cleanup();
	};
	if (!MakeTable(*this, Table))
	{
		return false;
	}
	const auto Body = [&](const TCHAR* VariableRow, const TCHAR* DefaultsRow, const TCHAR* ComponentKey)
		{
			return FString::Printf(TEXT("\"Variables\":[{\"Name\":\"Bonus\",\"Type\":\"struct:/Script/Engine.DataTableRowHandle\",")
				TEXT("\"DefaultValue\":%s},{\"Name\":\"Rewards\",\"Type\":\"array:struct:/Script/Engine.DataTableRowHandle\",\"DefaultValue\":[%s]}],")
				TEXT("\"ClassDefaults\":{\"Pickup\":%s},")
				TEXT("\"Components\":[{\"Name\":\"Picker\",\"Class\":\"%s\",\"Properties\":{\"Key\":\"%s\"}}]"),
				*Table.Row(VariableRow), *Table.Row(TEXT("Heal")), *Table.Row(DefaultsRow), PickComponent, ComponentKey);
		};

	// A new Blueprint: Validate reads a trial Blueprint of the parent class and reports every value.
	{
		const FString Bad = Blueprint.Blueprint(TEXT("CreateOrUpdate"), Body(TEXT("Bost"), TEXT("Sheild"), TEXT("Spacbar")));
		const FAssetDocumentResult Checked = Validate(*this, Bad, TEXT("create: validate"));
		TestFalse(TEXT("create: Validate refuses"), Checked.IsSuccess());
		TestTrue(TEXT("create: variable default"), HasProblem(Checked, TEXT("Property.RowNotFound"), TEXT("/Body/Variables/0/DefaultValue"),
			TEXT("Close: Boost")));
		TestTrue(TEXT("create: ClassDefaults"), HasProblem(Checked, TEXT("Property.RowNotFound"), TEXT("/Body/ClassDefaults/Pickup"),
			TEXT("Close: Shield")));
		TestTrue(TEXT("create: component template"), HasProblem(Checked, TEXT("Property.KeyNotFound"),
			TEXT("/Body/Components/0/Properties/Key"), TEXT("SpaceBar")));
		const FAssetDocumentResult Applied = Apply(*this, Blueprint, Bad, TEXT("create: apply"));
		TestFalse(TEXT("create: Apply refuses"), Applied.IsSuccess());
		TestNull(TEXT("create: no Blueprint left"), Blueprint.Get());
	}
	TestTrue(TEXT("valid values apply"), Apply(*this, Blueprint, Blueprint.Blueprint(TEXT("CreateOrUpdate"),
		Body(TEXT("Boost"), TEXT("Shield"), TEXT("SpaceBar"))), TEXT("valid")).IsSuccess());

	// The existing Blueprint: each place on its own, at Validate and Apply.
	TestRefused(*this, Blueprint, Blueprint.Blueprint(TEXT("CreateOrUpdate"), Body(TEXT("Bost"), TEXT("Shield"), TEXT("SpaceBar"))),
		TEXT("Property.RowNotFound"), TEXT("/Body/Variables/0/DefaultValue"), TEXT("Close: Boost"), TEXT("update: variable"));
	TestRefused(*this, Blueprint, Blueprint.Blueprint(TEXT("CreateOrUpdate"), Body(TEXT("Boost"), TEXT("Sheild"), TEXT("SpaceBar"))),
		TEXT("Property.RowNotFound"), TEXT("/Body/ClassDefaults/Pickup"), TEXT("Close: Shield"), TEXT("update: class defaults"));
	TestRefused(*this, Blueprint, Blueprint.Blueprint(TEXT("CreateOrUpdate"), Body(TEXT("Boost"), TEXT("Shield"), TEXT("Spacbar"))),
		TEXT("Property.KeyNotFound"), TEXT("/Body/Components/0/Properties/Key"), TEXT("SpaceBar"), TEXT("update: component"));
	TestTrue(TEXT("the valid document still validates"), Validate(*this, Blueprint.Blueprint(TEXT("CreateOrUpdate"),
		Body(TEXT("Boost"), TEXT("Shield"), TEXT("SpaceBar"))), TEXT("valid again")).IsSuccess());
	return true;
}
#endif
