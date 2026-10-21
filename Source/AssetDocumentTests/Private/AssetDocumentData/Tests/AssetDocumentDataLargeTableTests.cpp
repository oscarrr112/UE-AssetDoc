// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// UEAssetDoc.DataTable.LargeTable / LargeRichTable: a 5,000 row x 20 field
// DataTable round-trips through the production Service: Extract, Diff, an unchanged re-apply
// (no-op, no save), a one-row edit, and a create from the full document. The plain table has
// scalar fields; the rich one has FText, hard and soft object references (slower value codecs).
// Times and resident memory are logged ("[large] ..."). Documents are limited by the
// whole-document budget, primitive calls by the per-call budget (see the DataTable matrix doc).
// UEAssetDoc.DataTable.LargeTableScaling: a create from a full document stays
// linear in the rows (1,000 and 25,000 rows; the 25,000-row document is over the old 8 MiB
// transport limit).

#include "AssetDocumentDataTableTestSupport.h"
#include "AssetDocumentDataTestRows.h"

#include "Engine/DataTable.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "Materials/Material.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentDataLargeTableTests
{
using namespace AssetDocumentDataTableTests;

constexpr int32 RowCount = 5000;

FName RowName(int32 Index)
{
	return FName(*FString::Printf(TEXT("Row_%05d"), Index));
}

FAssetDocumentDataTestWideRow MakeWideRow(int32 Index)
{
	FAssetDocumentDataTestWideRow Row;
	Row.Name = FString::Printf(TEXT("Item %d"), Index);
	Row.Description = FString::Printf(TEXT("Generated item number %d"), Index);
	Row.Category = FName(*FString::Printf(TEXT("Category_%d"), Index % 17));
	Row.Rarity = FName(*FString::Printf(TEXT("Rarity_%d"), Index % 5));
	Row.Level = Index % 100;
	Row.Price = Index * 3;
	Row.StackSize = 1 + Index % 99;
	Row.Durability = 1000 - Index % 1000;
	Row.Id = 1000000000LL + Index;
	Row.Weight = 0.25f * (Index % 40);
	Row.Damage = 1.5f + Index % 300;
	Row.Speed = 0.1f * (Index % 10);
	Row.Range = 100.f + Index;
	Row.Chance = 1.0 / (1 + Index % 7);
	Row.bTradable = Index % 2 == 0;
	Row.bQuest = Index % 11 == 0;
	Row.bConsumable = Index % 3 == 0;
	Row.Kind = static_cast<EAssetDocumentDataTestKind>(Index % 3);
	Row.Tier = static_cast<uint8>(Index % 8);
	Row.Icon = FString::Printf(TEXT("/Game/Icons/T_Item_%d.T_Item_%d"), Index, Index);
	return Row;
}

struct FRichAssets
{
	UObject* Meshes[2] = {};
	UObject* Materials[2] = {};
	UObject* Texture = nullptr;
};

FRichAssets LoadRichAssets()
{
	FRichAssets Assets;
	Assets.Meshes[0] = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	Assets.Meshes[1] = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	Assets.Materials[0] = LoadObject<UMaterial>(nullptr, TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
	Assets.Materials[1] = LoadObject<UMaterial>(nullptr, TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"));
	Assets.Texture = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
	return Assets;
}

FAssetDocumentDataTestRichRow MakeRichRow(const FRichAssets& Assets, int32 Index)
{
	FAssetDocumentDataTestRichRow Row;
	Row.DisplayName = FText::FromString(FString::Printf(TEXT("Item %d"), Index));
	Row.Description = FText::FromString(FString::Printf(TEXT("Generated item number %d"), Index));
	Row.Tooltip = FText::FromString(FString::Printf(TEXT("Tooltip %d"), Index % 50));
	Row.Flavor = Index % 4 == 0 ? FText::GetEmpty() : FText::FromString(TEXT("A plain item."));
	Row.Mesh = Assets.Meshes[Index % 2];
	Row.Material = Assets.Materials[Index % 2];
	Row.Texture = Index % 3 == 0 ? nullptr : Assets.Texture;
	Row.SpawnClass = Index % 2 == 0 ? AActor::StaticClass() : nullptr;
	Row.SoftMesh = Assets.Meshes[(Index + 1) % 2];
	Row.SoftMaterial = Assets.Materials[(Index + 1) % 2];
	Row.SoftTexture = TSoftObjectPtr<UObject>(FSoftObjectPath(FString::Printf(TEXT("/Game/Icons/T_Item_%d.T_Item_%d"), Index, Index)));
	Row.Name = FString::Printf(TEXT("Item %d"), Index);
	Row.Category = FName(*FString::Printf(TEXT("Category_%d"), Index % 17));
	Row.Level = Index % 100;
	Row.Price = Index * 3;
	Row.StackSize = 1 + Index % 99;
	Row.Weight = 0.25f * (Index % 40);
	Row.Damage = 1.5f + Index % 300;
	Row.bTradable = Index % 2 == 0;
	Row.Kind = static_cast<EAssetDocumentDataTestKind>(Index % 3);
	return Row;
}

// A saved table of Count rows, made the way an imported table is (rows added in C++).
template <typename RowType, typename MakeRowType>
UDataTable* MakeTable(const FTarget& Target, int32 Count, const MakeRowType& MakeRow)
{
	UPackage* Package = CreatePackage(*Target.PackageName);
	UDataTable* Table = NewObject<UDataTable>(Package, FName(*Target.AssetName), RF_Public | RF_Standalone | RF_Transactional);
	Table->RowStruct = RowType::StaticStruct();
	for (int32 Index = 0; Index < Count; ++Index)
	{
		Table->AddRow(RowName(Index), MakeRow(Index));
	}
	FAssetRegistryModule::AssetCreated(Table);
	UEditorLoadingAndSavingUtils::SavePackages({Package}, false);
	return Table;
}

double ResidentMiB()
{
	return static_cast<double>(FPlatformMemory::GetStats().UsedPhysical) / (1024.0 * 1024.0);
}

struct FTimer
{
	FAutomationTestBase& Test;
	const TCHAR* Stage;
	double Start = FPlatformTime::Seconds();
	double StartMiB = ResidentMiB();
	~FTimer()
	{
		Test.AddInfo(FString::Printf(TEXT("[large] %s: %.2f s, resident %+.0f MiB (%.0f MiB)"), Stage,
			FPlatformTime::Seconds() - Start, ResidentMiB() - StartMiB, ResidentMiB()));
	}
};

template <typename RowType, typename MakeRowType>
void RunLargeTable(FAutomationTestBase& Test, const TCHAR* Name, const MakeRowType& MakeRow)
{
	const FTarget Target(Name);
	const FTarget Copy(*FString::Printf(TEXT("%sCopy"), Name));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
		Copy.Cleanup();
	};
	UDataTable* Table = MakeTable<RowType>(Target, RowCount, MakeRow);
	ReleaseUndoHistory();

	// 1. Extract.
	TSharedPtr<FJsonObject> Extracted;
	{
		FTimer Timer{Test, TEXT("Extract")};
		Extracted = Extract(Test, Target, TEXT("Extract"));
	}
	if (!Extracted.IsValid())
	{
		return;
	}
	Test.TestEqual(TEXT("every row is extracted"), BodyArray(Extracted, TEXT("Rows")).Num(), RowCount);
	Test.AddInfo(FString::Printf(TEXT("[large] document: %d rows x 20 fields (%s), %d KiB JSON"), RowCount,
		*RowType::StaticStruct()->GetName(), Serialize(Extracted).Len() / 1024));

	// 2. Diff of the extraction and an unchanged re-apply (no-op).
	Extracted->SetStringField(TEXT("Action"), TEXT("Update"));
	const FString Update = Serialize(Extracted);
	{
		FTimer Timer{Test, TEXT("Diff")};
		TestNoDiff(Test, Update, TEXT("Diff"));
	}
	const FDateTime SavedStamp = IFileManager::Get().GetTimeStamp(*Target.PackageFile);
	{
		FTimer Timer{Test, TEXT("Unchanged re-apply")};
		const FAssetDocumentResult Reapplied = Apply(Test, Target, Update, TEXT("Reapply"));
		Test.TestTrue(TEXT("extraction re-applies"), Reapplied.IsSuccess());
		Test.TestFalse(TEXT("no-op re-apply reports no save"), Reapplied.bSavedAsset);
	}
	Test.TestFalse(TEXT("no-op re-apply leaves the package clean"), Table->GetOutermost()->IsDirty());
	Test.TestEqual(TEXT("no-op re-apply does not rewrite the package file"), IFileManager::Get().GetTimeStamp(*Target.PackageFile), SavedStamp);

	// 3. Edit one row (the last one) in the full document.
	{
		const TArray<TSharedPtr<FJsonValue>> Rows = BodyArray(Extracted, TEXT("Rows"));
		Rows.Last()->AsObject()->GetObjectField(TEXT("Values"))->SetNumberField(TEXT("Price"), 77);
		FTimer Timer{Test, TEXT("One-row edit")};
		const FAssetDocumentResult Edited = Apply(Test, Target, Serialize(Extracted), TEXT("Edit"));
		Test.TestTrue(TEXT("one-row edit applies"), Edited.IsSuccess() && Edited.bSavedAsset);
	}
	const RowType* Last = Table->FindRow<RowType>(RowName(RowCount - 1), TEXT("LargeTable"), false);
	Test.TestTrue(TEXT("the edited row changed"), Last != nullptr && Last->Price == 77);
	const RowType* First = Table->FindRow<RowType>(RowName(0), TEXT("LargeTable"), false);
	const RowType Expected = MakeRow(0);
	Test.TestTrue(TEXT("other rows are unchanged"), First != nullptr && RowType::StaticStruct()->CompareScriptStruct(First, &Expected, PPF_None));
	TestNoDiff(Test, Serialize(Extracted), TEXT("after edit"));

	// 4. Create a copy from the full document in one apply.
	Extracted->SetStringField(TEXT("Target"), Copy.PackageName);
	Extracted->SetStringField(TEXT("Action"), TEXT("Create"));
	{
		FTimer Timer{Test, TEXT("Create from document")};
		Test.TestTrue(TEXT("create from the full document applies"), Apply(Test, Copy, Serialize(Extracted), TEXT("Create")).IsSuccess());
	}
	const UDataTable* Created = Copy.Get<UDataTable>();
	Test.TestEqual(TEXT("created table has every row"), Created != nullptr ? Created->GetRowMap().Num() : 0, RowCount);
	const RowType* CreatedLast = Created != nullptr ? Created->FindRow<RowType>(RowName(RowCount - 1), TEXT("LargeTable"), false) : nullptr;
	Test.TestTrue(TEXT("created rows carry the document values"), CreatedLast != nullptr && Last != nullptr
		&& RowType::StaticStruct()->CompareScriptStruct(CreatedLast, Last, PPF_None));
}
} // namespace AssetDocumentDataLargeTableTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentDataLargeTableTest, "UEAssetDoc.DataTable.LargeTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentDataLargeTableTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentDataLargeTableTests;
	RunLargeTable<FAssetDocumentDataTestWideRow>(*this, TEXT("DT_Large"), &MakeWideRow);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentDataLargeRichTableTest, "UEAssetDoc.DataTable.LargeRichTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentDataLargeRichTableTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentDataLargeTableTests;
	const FRichAssets Assets = LoadRichAssets();
	if (!TestTrue(TEXT("engine assets load"), Assets.Meshes[0] && Assets.Meshes[1] && Assets.Materials[0] && Assets.Materials[1] && Assets.Texture))
	{
		return false;
	}
	RunLargeTable<FAssetDocumentDataTestRichRow>(*this, TEXT("DT_LargeRich"), [&Assets](int32 Index) { return MakeRichRow(Assets, Index); });
	return !HasAnyErrors();
}

namespace AssetDocumentDataLargeTableTests
{
// Create a table of Count rows from a full document (one ApplyFile) and return the seconds it took.
double TimeCreateFromDocument(FAutomationTestBase& Test, int32 Count)
{
	const FTarget Source(*FString::Printf(TEXT("DT_Scale%d"), Count));
	const FTarget Copy(*FString::Printf(TEXT("DT_Scale%dCopy"), Count));
	ON_SCOPE_EXIT
	{
		Source.Cleanup();
		Copy.Cleanup();
	};
	MakeTable<FAssetDocumentDataTestWideRow>(Source, Count, &MakeWideRow);
	ReleaseUndoHistory();
	const TSharedPtr<FJsonObject> Extracted = Extract(Test, Source, TEXT("scale extract"));
	if (!Extracted.IsValid())
	{
		return -1.0;
	}
	Extracted->SetStringField(TEXT("Target"), Copy.PackageName);
	Extracted->SetStringField(TEXT("Action"), TEXT("Create"));
	const FString Json = Serialize(Extracted);
	const double Start = FPlatformTime::Seconds();
	const FAssetDocumentResult Created = Apply(Test, Copy, Json, TEXT("scale create"));
	const double Seconds = FPlatformTime::Seconds() - Start;
	Test.TestTrue(*FString::Printf(TEXT("%d-row create applies"), Count), Created.IsSuccess());
	const UDataTable* Table = Copy.Get<UDataTable>();
	Test.TestEqual(*FString::Printf(TEXT("%d-row create has every row"), Count), Table != nullptr ? Table->GetRowMap().Num() : 0, Count);
	Test.AddInfo(FString::Printf(TEXT("[large] create %d rows (%d KiB JSON): %.2f s"), Count, Json.Len() / 1024, Seconds));
	return Seconds;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentDataLargeTableScalingTest, "UEAssetDoc.DataTable.LargeTableScaling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// A create from a full document grows linearly with the rows: the editor's table operations used
// to record the whole table into the undo buffer on every chunked primitive call (quadratic: 10,000
// rows took 47 s, 4.7 ms a row). A 25,000-row document (about 10 MiB) also passes the Python <-> C++
// transport, which stopped at 8 MiB (about 18,000 rows).
bool FAssetDocumentDataLargeTableScalingTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentDataLargeTableTests;
	const double Small = TimeCreateFromDocument(*this, 1000);
	const double Large = TimeCreateFromDocument(*this, 25000);
	if (Small > 0.0 && Large > 0.0)
	{
		const double Ratio = (Large / 25000.0) / (Small / 1000.0);
		AddInfo(FString::Printf(TEXT("[large] time per row, 25,000 vs 1,000 rows: x%.2f"), Ratio));
		TestTrue(FString::Printf(TEXT("create time per row stays flat (25,000 vs 1,000 rows: x%.2f, limit x2)"), Ratio), Ratio < 2.0);
	}
	return !HasAnyErrors();
}
#endif
