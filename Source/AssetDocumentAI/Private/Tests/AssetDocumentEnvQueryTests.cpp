// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// EnvQuery profile (docs/architecture/asset-document-eqs-design.md):
// UEAssetDoc.EnvQuery.ListTypes: list_types kinds env_query_generator / env_query_test (the palette and
//   the Add Test menu, with item types and new-node Details), filters, contexts through kind class.
// UEAssetDoc.EnvQuery.Read: Extract / Inspect of an editor-built query (keys, toggles, comments,
//   bindings, params, status, node facts); reading changes nothing; an unknown class names close candidates.
// UEAssetDoc.EnvQuery.Fixtures: the engine / plugin corpus (counted) and the editor-built fixtures
//   (every palette generator, every Add Test class, a factory-made query without graph): Create from the extract gives
//   the same document, graph, runtime options and params; Diff is empty, the re-apply a no-op, the reload stable.
// UEAssetDoc.EnvQuery.Runtime: an editor-built query and its re-created copy return the same items
//   with the same scores in a game world (UEnvQueryManager::RunInstantQuery).
// UEAssetDoc.EnvQuery.FreeEdits: a query written by hand equals the editor-built one; reorder / add /
//   remove tests, filter and score settings, generator switch, query-param binding, toggles, wires, moving a test.
// UEAssetDoc.EnvQuery.Rollback: refused applies (validation, a bad value mid-way, unknown class,
//   contradicting order, failed Create) leave the query as it was.
// UEAssetDoc.EnvQuery.Undo: human edits, AI applies, undo / redo stay whole.
// UEAssetDoc.EnvQuery.OpenEditorApply: with the query open in its editor an apply lands in the open editor
//   (graph panel and selection refreshed as its Undo does), a failed apply rolls back, the editor's own undo / redo after
//   the apply stay whole, and the editor closes.
// UEAssetDoc.EnvQuery.BehaviorTree: a BehaviorTree built through the BT profile runs the new query
//   (Run EQS Query) and stores its result; the query lists the tree as dependent and warns when its params change.

#include "AssetDocumentEnvQueryTestSupport.h"
#include "AssetDocumentOpenEditorTestSupport.h"

#include "AIController.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/Tasks/BTTask_RunEQSQuery.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "DataProviders/AIDataProvider_QueryParams.h"
#include "GameFramework/DefaultPawn.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_Composite.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_OnCircle.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_SimpleGrid.h"
#include "Misc/ScopeExit.h"
#include "Runtime/AssetDocumentOpenEditor.h"
#include "Runtime/AssetDocumentUndoHistory.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEnvQueryTests
{
// An option's Details value bound to a query param (the Details panel's data binding picker: a new
// AIDataProvider_QueryParams instanced in the generator / test).
inline bool BindParam(FAutomationTestBase& Test, UObject& Owner, const TCHAR* Property, const TCHAR* Param)
{
	FStructProperty* Field = CastField<FStructProperty>(Owner.GetClass()->FindPropertyByName(Property));
	if (!Test.TestNotNull(*FString::Printf(TEXT("%s is a data provider value"), Property), Field) || !Field->Struct->IsChildOf(FAIDataProviderValue::StaticStruct()))
	{
		return false;
	}
	const FScopedTransaction Transaction(NSLOCTEXT("PropertyEditor", "EditPropertyTransaction", "Edit Property"));
	Owner.Modify();
	FAIDataProviderValue* Value = Field->ContainerPtrToValuePtr<FAIDataProviderValue>(&Owner);
	UAIDataProvider_QueryParams* Binding = NewObject<UAIDataProvider_QueryParams>(&Owner, NAME_None, RF_Transactional);
	Binding->ParamName = Param;
	Value->DataBinding = Binding;
	Value->DataField = TEXT("FloatValue");
	FPropertyChangedEvent Event(Field, EPropertyChangeType::ValueSet);
	Owner.PostEditChangeProperty(Event);
	return true;
}

inline UObject* GeneratorOf(const FEditorQuery& Editor, const FString& Option)
{
	UAIGraphNode* Node = Editor.Option(Option);
	UEnvQueryOption* Made = Node != nullptr ? EQ::GetOption(*Node) : nullptr;
	return Made != nullptr ? Made->Generator.Get() : nullptr;
}

inline UObject* TestOf(const FEditorQuery& Editor, const FString& Name)
{
	UAIGraphNode* Node = Editor.Test(Name);
	return Node != nullptr ? Node->NodeInstance.Get() : nullptr;
}

// EQS_AllGenerators: every palette generator as an option, wired left to right (the Composite one combines two point
// generators, as a human sets it up: an empty Composite is an error).
inline UEnvQuery* MakeAllGenerators(FAutomationTestBase& Test, const FTarget& Target, int32& OutCount)
{
	UEnvQuery* Query = CreateQuery(Test, Target);
	if (Query == nullptr)
	{
		return nullptr;
	}
	FEditorQuery Editor(*Query);
	OutCount = 0;
	FString First;
	for (const EQ::FPaletteClass& Entry : EQ::GetPalette(EQ::ENodeKind::Generator))
	{
		const FString Option = Editor.AddOption(*Entry.Label, 320 * OutCount, 160);
		++OutCount;
		if (First.IsEmpty())
		{
			First = Option;
		}
		if (UEnvQueryGenerator_Composite* Composite = Cast<UEnvQueryGenerator_Composite>(GeneratorOf(Editor, Option)))
		{
			const FScopedTransaction Transaction(NSLOCTEXT("PropertyEditor", "EditPropertyTransaction", "Edit Property"));
			Composite->Modify();
			Composite->Generators.Add(NewObject<UEnvQueryGenerator_OnCircle>(Composite, NAME_None, RF_Transactional));
			Composite->Generators.Add(NewObject<UEnvQueryGenerator_SimpleGrid>(Composite, NAME_None, RF_Transactional));
			FPropertyChangedEvent Event(Composite->GetClass()->FindPropertyByName(TEXT("Generators")), EPropertyChangeType::ValueSet);
			Composite->PostEditChangeProperty(Event);
			Editor.Graph().UpdateAsset();
		}
	}
	Editor.AddTest(First, TEXT("Distance"));
	return Editor.SaveQuery(Test, TEXT("EQS_AllGenerators")) ? Query : nullptr;
}

// EQS_AllTests: a grid option with every Add Test class (one disabled, several with non-default filter / score
// settings), its grid size bound to a query param, a second unwired option, node comments, a comment box, Root moved.
inline UEnvQuery* MakeAllTests(FAutomationTestBase& Test, const FTarget& Target, int32& OutCount)
{
	UEnvQuery* Query = CreateQuery(Test, Target);
	if (Query == nullptr)
	{
		return nullptr;
	}
	FEditorQuery Editor(*Query);
	Editor.Move(*EQ::FindRoot(Editor.Graph()), 32, -48);
	const FString Grid = Editor.AddOption(TEXT("SimpleGrid"), 0, 160);
	const FString Spare = Editor.AddOption(TEXT("ActorsOfClass"), 480, 160, false);
	OutCount = 0;
	TArray<FString> Tests;
	for (const EQ::FPaletteClass& Entry : EQ::GetPalette(EQ::ENodeKind::Test))
	{
		Tests.Add(Editor.AddTest(Grid, *Entry.Label));
		++OutCount;
	}
	const FString Distance = Editor.AddTest(Grid, TEXT("Distance"));
	Editor.SetProperty(Distance, TEXT("TestPurpose"), TEXT("Score"));
	Editor.SetProperty(Distance, TEXT("ScoringEquation"), TEXT("InverseLinear"));
	Editor.SetProperty(Distance, TEXT("ScoringFactor"), TEXT("(DefaultValue=2.500000)"));
	const FString Dot = Editor.AddTest(Grid, TEXT("Dot"));
	Editor.SetProperty(Dot, TEXT("TestPurpose"), TEXT("Filter"));
	Editor.SetProperty(Dot, TEXT("FilterType"), TEXT("Range"));
	Editor.SetProperty(Dot, TEXT("FloatValueMin"), TEXT("(DefaultValue=0.250000)"));
	Editor.SetProperty(Dot, TEXT("FloatValueMax"), TEXT("(DefaultValue=0.750000)"));
	if (!Tests.IsEmpty())
	{
		Editor.Toggle(Tests.Last(), false);
	}
	Editor.AddTest(Spare, TEXT("Distance"));
	if (UObject* Generator = GeneratorOf(Editor, Grid))
	{
		BindParam(Test, *Generator, TEXT("GridSize"), TEXT("Radius"));
	}
	Editor.SetProperty(Grid, TEXT("SpaceBetween"), TEXT("(DefaultValue=150.000000)"));
	if (UAIGraphNode* Node = Editor.Option(Grid))
	{
		Editor.NodeComment(*Node, TEXT("main grid"), true);
	}
	if (UAIGraphNode* Node = Editor.Test(Distance))
	{
		Editor.NodeComment(*Node, TEXT("closer is better"), false);
	}
	Editor.AddComment(TEXT("Cover search"), -64, -160, 960, 640);
	return Editor.SaveQuery(Test, TEXT("EQS_AllTests")) ? Query : nullptr;
}

// EQS_Cover: the small query the free edits start from (grid + actors, three tests), every object named by the editor.
struct FCover
{
	UEnvQuery* Query = nullptr;
	FString Grid;
	FString Actors;
	FString Distance;
	FString Trace;
	FString Dot;
};

inline FCover MakeCover(FAutomationTestBase& Test, const FTarget& Target)
{
	FCover Cover;
	Cover.Query = CreateQuery(Test, Target);
	if (Cover.Query == nullptr)
	{
		return Cover;
	}
	FEditorQuery Editor(*Cover.Query);
	Cover.Grid = Editor.AddOption(TEXT("SimpleGrid"), 0, 160);
	Cover.Actors = Editor.AddOption(TEXT("ActorsOfClass"), 320, 160);
	Cover.Distance = Editor.AddTest(Cover.Grid, TEXT("Distance"));
	Cover.Trace = Editor.AddTest(Cover.Grid, TEXT("Trace"));
	Cover.Dot = Editor.AddTest(Cover.Actors, TEXT("Dot"));
	Editor.SetProperty(Cover.Distance, TEXT("TestPurpose"), TEXT("Score"));
	Editor.SetProperty(Cover.Grid, TEXT("GridSize"), TEXT("(DefaultValue=800.000000)"));
	Editor.SetProperty(Cover.Actors, TEXT("SearchedActorClass"), TEXT("/Script/Engine.TargetPoint"));
	if (!Editor.SaveQuery(Test, TEXT("EQS_Cover")))
	{
		Cover.Query = nullptr;
	}
	return Cover;
}

inline TSharedPtr<FJsonObject> OptionAt(const TSharedPtr<FJsonObject>& Document, int32 Index)
{
	const TArray<TSharedPtr<FJsonValue>> Options = OptionsOf(Document);
	return Options.IsValidIndex(Index) ? Options[Index]->AsObject() : MakeShared<FJsonObject>();
}

inline FString RuntimeTests(const UEnvQuery& Query, int32 OptionIndex)
{
	TArray<FString> Parts;
	if (Query.GetOptions().IsValidIndex(OptionIndex) && Query.GetOptions()[OptionIndex] != nullptr)
	{
		for (const UEnvQueryTest* Test : Query.GetOptions()[OptionIndex]->Tests)
		{
			Parts.Add(Test != nullptr ? FString::Printf(TEXT("%s:%s"), *Test->GetName(), *Test->GetClass()->GetName()) : TEXT("null"));
		}
	}
	return FString::Join(Parts, TEXT(","));
}

inline FString RuntimeGenerators(const UEnvQuery& Query)
{
	TArray<FString> Parts;
	for (const UEnvQueryOption* Option : Query.GetOptions())
	{
		Parts.Add(Option != nullptr && Option->Generator != nullptr ? FString::Printf(TEXT("%s:%s"), *Option->GetName(), *Option->Generator->GetClass()->GetName()) : TEXT("null"));
	}
	return FString::Join(Parts, TEXT(","));
}

// The document applies, Diff against it is then empty.
inline bool ApplyAndDiff(FAutomationTestBase& Test, const FTarget& Target, const TSharedPtr<FJsonObject>& Document, const FString& Stage)
{
	if (!ApplyOk(Test, Target, Document, Stage))
	{
		return false;
	}
	FString Text;
	const int32 Changes = DiffCount(Document, Text);
	if (!Test.TestEqual(*FString::Printf(TEXT("%s: Diff empty"), *Stage), Changes, 0))
	{
		Test.AddInfo(Text.Left(1500));
	}
	return true;
}
} // namespace AssetDocumentEnvQueryTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentEnvQueryListTypesTest, "UEAssetDoc.EnvQuery.ListTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEnvQueryListTypesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentEnvQueryTests;
	const FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	auto List = [&](const TMap<FString, FString>& Query, const TCHAR* Stage) -> TArray<TSharedPtr<FJsonValue>>
	{
		const FAssetDocumentResult Result = Service.ListTypes(Query);
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (!TestTrue(*FString::Printf(TEXT("%s: lists"), Stage), Result.IsSuccess() && Result.Payload.IsValid() && Result.Payload->TryGetArrayField(TEXT("Types"), Rows)))
		{
			AddInfo(DiagnosticsText(Result));
			return {};
		}
		return *Rows;
	};
	auto Find = [](const TArray<TSharedPtr<FJsonValue>>& Rows, const TCHAR* Class) -> TSharedPtr<FJsonObject>
	{
		for (const TSharedPtr<FJsonValue>& Row : Rows)
		{
			if (Row->AsObject()->GetStringField(TEXT("Class")) == Class)
			{
				return Row->AsObject();
			}
		}
		return nullptr;
	};
	const TArray<TSharedPtr<FJsonValue>> Generators = List({{TEXT("kind"), TEXT("env_query_generator")}, {TEXT("limit"), TEXT("200")}}, TEXT("generators"));
	AddInfo(FString::Printf(TEXT("%d generator classes, palette %d"), Generators.Num(), EQ::GetPalette(EQ::ENodeKind::Generator).Num()));
	for (const TCHAR* Class : {TEXT("SimpleGrid"), TEXT("ActorsOfClass"), TEXT("Donut"), TEXT("OnCircle"), TEXT("PathingGrid"), TEXT("Cone"),
		TEXT("CurrentLocation"), TEXT("Composite"), TEXT("PerceivedActors")})
	{
		TestTrue(*FString::Printf(TEXT("generator %s is listed"), Class), Find(Generators, Class).IsValid());
	}
	if (const TSharedPtr<FJsonObject> Grid = Find(Generators, TEXT("SimpleGrid")))
	{
		AddInfo(FString::Printf(TEXT("SimpleGrid row: %s"), *Serialize(Grid).Left(1500)));
		TestEqual(TEXT("SimpleGrid path"), Grid->GetStringField(TEXT("Path")), FString(TEXT("/Script/AIModule.EnvQueryGenerator_SimpleGrid")));
		TestTrue(TEXT("SimpleGrid makes points"), Grid->GetStringField(TEXT("ItemType")).Contains(TEXT("Point")));
		TestTrue(TEXT("SimpleGrid defaults list GridSize"), Child(Grid, TEXT("Properties"))->HasField(TEXT("GridSize")));
	}
	const TArray<TSharedPtr<FJsonValue>> Tests = List({{TEXT("kind"), TEXT("env_query_test")}, {TEXT("limit"), TEXT("200")}}, TEXT("tests"));
	AddInfo(FString::Printf(TEXT("%d test classes"), Tests.Num()));
	for (const TCHAR* Class : {TEXT("Distance"), TEXT("Dot"), TEXT("Trace"), TEXT("Pathfinding"), TEXT("PathfindingBatch"), TEXT("Project"),
		TEXT("Overlap"), TEXT("GameplayTags"), TEXT("Volume")})
	{
		TestTrue(*FString::Printf(TEXT("test %s is listed"), Class), Find(Tests, Class).IsValid());
	}
	// FGraphNodeClassHelper leaves DeprecatedNode classes out of Add Test, as the editor's menu does.
	TestFalse(TEXT("deprecated test Random is not offered"), Find(Tests, TEXT("Random")).IsValid());
	if (const TSharedPtr<FJsonObject> Distance = Find(Tests, TEXT("Distance")))
	{
		TestTrue(TEXT("Distance defaults list TestPurpose"), Child(Distance, TEXT("Properties"))->HasField(TEXT("TestPurpose")));
		TestTrue(TEXT("Distance defaults list DistanceTo"), Child(Distance, TEXT("Properties"))->HasField(TEXT("DistanceTo")));
	}
	const TArray<TSharedPtr<FJsonValue>> Named = List({{TEXT("kind"), TEXT("env_query_test")}, {TEXT("name"), TEXT("Path*")}}, TEXT("named"));
	TestEqual(TEXT("name wildcard"), Named.Num(), 2);
	const TArray<TSharedPtr<FJsonValue>> Contexts = List({{TEXT("kind"), TEXT("class")}, {TEXT("base"), TEXT("/Script/AIModule.EnvQueryContext")}, {TEXT("limit"), TEXT("200")}}, TEXT("contexts"));
	bool bQuerier = false;
	for (const TSharedPtr<FJsonValue>& Row : Contexts)
	{
		bQuerier |= Serialize(Row->AsObject()).Contains(TEXT("EnvQueryContext_Querier"));
	}
	TestTrue(TEXT("contexts list EnvQueryContext_Querier"), bQuerier);
	const FAssetDocumentResult Unknown = Service.ListTypes({{TEXT("kind"), TEXT("env_query")}});
	TestFalse(TEXT("unknown kind fails"), Unknown.IsSuccess());
	TestTrue(TEXT("unknown kind names env_query_test"), Unknown.Message.Contains(TEXT("env_query_test")) || DiagnosticsText(Unknown).Contains(TEXT("env_query_test")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentEnvQueryReadTest, "UEAssetDoc.EnvQuery.Read",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEnvQueryReadTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentEnvQueryTests;
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("EQS_Read"));
	int32 TestCount = 0;
	UEnvQuery* Query = MakeAllTests(*this, Target, TestCount);
	if (!TestNotNull(TEXT("EQS_AllTests"), Query))
	{
		return false;
	}
	const FString Before = QueryForm(*Query);
	const TSharedPtr<FJsonObject> Doc = Extract(*this, Target.ObjectPath, TEXT("read"));
	if (!Doc.IsValid())
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("document: %s"), *Serialize(Doc).Left(4000)));
	TestEqual(TEXT("two options"), OptionsOf(Doc).Num(), 2);
	const TSharedPtr<FJsonObject> Grid = OptionAt(Doc, 0);
	TestEqual(TEXT("grid first"), Child(Grid, TEXT("Generator"))->GetStringField(TEXT("Class")), FString(TEXT("SimpleGrid")));
	TestEqual(TEXT("every test listed"), ArrayOf(Grid, TEXT("Tests")).Num(), TestCount + 2);
	TestEqual(TEXT("grid comment"), Grid->GetStringField(TEXT("Comment")), FString(TEXT("main grid")));
	const TSharedPtr<FJsonObject> Spare = OptionAt(Doc, 1);
	TestFalse(TEXT("spare is unwired"), Spare->GetBoolField(TEXT("Connected")));
	TestEqual(TEXT("root position"), Serialize(Child(BodyOf(Doc), TEXT("Root"))), FString(TEXT("{\"Position\":[32,-48]}")));
	TestTrue(TEXT("a disabled test"), Serialize(Grid).Contains(TEXT("\"Enabled\":false")));
	TestTrue(TEXT("the binding is in the grid size"), Serialize(Child(Grid, TEXT("Generator"))).Contains(TEXT("Radius")));
	TestEqual(TEXT("one comment box"), Child(BodyOf(Doc), TEXT("Comments"))->Values.Num(), 1);
	// Keys are the editor's object names.
	TestTrue(TEXT("option keys are object names"), Grid->GetStringField(TEXT("Key")).StartsWith(TEXT("EnvQueryOption")));

	const FAssetDocumentResult Inspected = Inspect(Target.ObjectPath);
	if (TestTrue(TEXT("inspect succeeds"), Inspected.IsSuccess() && Inspected.Payload.IsValid()))
	{
		const FString Text = Serialize(Inspected.Payload);
		AddInfo(FString::Printf(TEXT("inspect: %s"), *Text.Right(2500)));
		TestTrue(TEXT("inspect lists the Radius param"), Text.Contains(TEXT("\"Name\":\"Radius\"")));
		TestTrue(TEXT("inspect has node facts"), Inspected.Payload->HasField(TEXT("NodeFacts")));
		TestTrue(TEXT("inspect has a status"), Child(Inspected.Payload, TEXT("Status"))->HasField(TEXT("Status")));
	}
	TestFalse(TEXT("reading leaves the package clean"), Query->GetOutermost()->IsDirty());
	TestEqual(TEXT("reading changes nothing"), QueryForm(*Query), Before);

	// An unknown class names close candidates.
	const TSharedPtr<FJsonObject> Bad = ForTarget(Doc, Target, TEXT("Update"));
	OptionAt(Bad, 0)->SetArrayField(TEXT("Tests"), {MakeShared<FJsonValueObject>(Parse(TEXT(R"({"Class": "Distanse"})")))});
	const FAssetDocumentResult Refused = Apply(Target, Bad);
	const FAssetDocumentDiagnostic* Found = Refused.Diagnostics.FindByPredicate([](const FAssetDocumentDiagnostic& Item) { return Item.Code == TEXT("EnvQuery.UnknownClass"); });
	if (TestNotNull(TEXT("unknown class refused"), Found))
	{
		TestEqual(TEXT("unknown class path"), Found->Path, FString(TEXT("/Body/Options/0/Tests/0/Class")));
		TestTrue(TEXT("candidates name Distance"), (Found->Message + Serialize(Found->Details)).Contains(TEXT("Distance")));
	}
	else
	{
		AddInfo(DiagnosticsText(Refused));
	}
	TestEqual(TEXT("refusal changes nothing"), QueryForm(*Target.Query()), Before);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentEnvQueryFixturesTest, "UEAssetDoc.EnvQuery.Fixtures",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEnvQueryFixturesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentEnvQueryTests;
	// The corpus: Environment Queries shipped in engine and enabled plugin content.
	TArray<FAssetData> Shipped;
	FAssetRegistryModule::GetRegistry().GetAssetsByClass(UEnvQuery::StaticClass()->GetClassPathName(), Shipped, true);
	Shipped.RemoveAll([](const FAssetData& Asset) { return Asset.PackageName.ToString().StartsWith(TEXT("/Game/")); });
	AddInfo(FString::Printf(TEXT("corpus: %d Environment Query assets in engine and enabled plugin content"), Shipped.Num()));
	FFixtures Fixtures;
	struct FCase
	{
		FString Name;
		UEnvQuery* Query = nullptr;
		FString Facts;
	};
	TArray<FCase> Cases;
	for (const FAssetData& Asset : Shipped)
	{
		const FTarget& Copy = Fixtures.Add(TEXT("EQS_Corpus"));
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		UObject* Duplicate = AssetTools.DuplicateAsset(Copy.AssetName, FPackageName::GetLongPackagePath(Copy.PackageName), Asset.GetAsset());
		if (TestNotNull(*FString::Printf(TEXT("duplicate %s"), *Asset.GetObjectPathString()), Duplicate))
		{
			Save(*this, Duplicate, Copy.AssetName);
			Cases.Add({Asset.GetObjectPathString(), Cast<UEnvQuery>(Duplicate), TEXT("shipped")});
		}
	}
	int32 Generators = 0;
	int32 Tests = 0;
	const FTarget& AllGenerators = Fixtures.Add(TEXT("EQS_AllGenerators"));
	Cases.Add({TEXT("EQS_AllGenerators"), MakeAllGenerators(*this, AllGenerators, Generators), TEXT("every palette generator")});
	const FTarget& AllTests = Fixtures.Add(TEXT("EQS_AllTests"));
	Cases.Add({TEXT("EQS_AllTests"), MakeAllTests(*this, AllTests, Tests), TEXT("every Add Test class")});
	const FTarget& Factory = Fixtures.Add(TEXT("EQS_Factory"));
	Cases.Add({TEXT("EQS_Factory"), CreateQuery(*this, Factory), TEXT("factory-made, no graph")});
	AddInfo(FString::Printf(TEXT("fixtures cover %d generator classes and %d test classes"), Generators, Tests));

	for (const FCase& Case : Cases)
	{
		if (!TestNotNull(*FString::Printf(TEXT("%s built"), *Case.Name), Case.Query))
		{
			continue;
		}
		const FString Path = Case.Query->GetPathName();
		double Start = FPlatformTime::Seconds();
		const FAssetDocumentResult First = ExtractResult(Path);
		const double ExtractMs = Milliseconds(Start);
		if (!TestTrue(*FString::Printf(TEXT("%s: extract"), *Case.Name), First.IsSuccess() && First.Payload.IsValid()))
		{
			AddError(FString::Printf(TEXT("%s: %s"), *Case.Name, *DiagnosticsText(First)));
			continue;
		}
		TestEqual(*FString::Printf(TEXT("%s: extract is stable"), *Case.Name), Serialize(Extract(*this, Path, Case.Name + TEXT(" again"))), Serialize(First.Payload));
		TestFalse(*FString::Printf(TEXT("%s: extract leaves the package clean"), *Case.Name), Case.Query->GetOutermost()->IsDirty());
		const FTarget& Created = Fixtures.Add(TEXT("EQS_Out"));
		const FRoundTrip Trip = RoundTrip(*this, *Case.Query, First.Payload, Created, Case.Name);
		int32 TestTotal = 0;
		for (const TSharedPtr<FJsonValue>& Option : OptionsOf(First.Payload))
		{
			TestTotal += ArrayOf(Option->AsObject(), TEXT("Tests")).Num();
		}
		AddInfo(FString::Printf(TEXT("%s (%s): %d options, %d tests; extract %.0f ms, %s"), *Case.Name, *Case.Facts, OptionsOf(First.Payload).Num(), TestTotal,
			ExtractMs, *Trip.Timings()));
		for (const FString& Problem : Trip.Problems)
		{
			AddError(FString::Printf(TEXT("%s: %s"), *Case.Name, *Problem));
		}
		if (!Trip.Problems.IsEmpty())
		{
			AddInfo(FString::Printf(TEXT("%s document: %s"), *Case.Name, *Serialize(First.Payload).Left(4000)));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentEnvQueryRuntimeTest, "UEAssetDoc.EnvQuery.Runtime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEnvQueryRuntimeTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentEnvQueryTests;
	FFixtures Fixtures;
	// 1. Actors around the querier, scored by distance and filtered to 1500 units.
	const FTarget& ActorsTarget = Fixtures.Add(TEXT("EQS_RunActors"));
	UEnvQuery* Actors = CreateQuery(*this, ActorsTarget);
	// 2. Grid points (no navigation projection), scored by distance with a binding to a query param default.
	const FTarget& GridTarget = Fixtures.Add(TEXT("EQS_RunGrid"));
	UEnvQuery* Grid = CreateQuery(*this, GridTarget);
	if (!TestNotNull(TEXT("queries"), Actors) || !TestNotNull(TEXT("grid query"), Grid))
	{
		return false;
	}
	{
		FEditorQuery Editor(*Actors);
		const FString Option = Editor.AddOption(TEXT("ActorsOfClass"), 0, 160);
		Editor.SetProperty(Option, TEXT("SearchedActorClass"), TEXT("/Script/Engine.TargetPoint"));
		Editor.SetProperty(Option, TEXT("SearchRadius"), TEXT("(DefaultValue=5000.000000)"));
		const FString Score = Editor.AddTest(Option, TEXT("Distance"));
		Editor.SetProperty(Score, TEXT("TestPurpose"), TEXT("Score"));
		Editor.SetProperty(Score, TEXT("ScoringEquation"), TEXT("InverseLinear"));
		const FString Filter = Editor.AddTest(Option, TEXT("Distance"));
		Editor.SetProperty(Filter, TEXT("TestPurpose"), TEXT("Filter"));
		Editor.SetProperty(Filter, TEXT("FilterType"), TEXT("Maximum"));
		Editor.SetProperty(Filter, TEXT("FloatValueMax"), TEXT("(DefaultValue=1500.000000)"));
		Editor.SaveQuery(*this, TEXT("EQS_RunActors"));
	}
	{
		FEditorQuery Editor(*Grid);
		const FString Option = Editor.AddOption(TEXT("SimpleGrid"), 0, 160);
		Editor.SetProperty(Option, TEXT("GridSize"), TEXT("(DefaultValue=300.000000)"));
		Editor.SetProperty(Option, TEXT("SpaceBetween"), TEXT("(DefaultValue=150.000000)"));
		Editor.SetProperty(Option, TEXT("ProjectionData"), TEXT("(TraceMode=None)"));
		const FString Score = Editor.AddTest(Option, TEXT("Distance"));
		Editor.SetProperty(Score, TEXT("TestPurpose"), TEXT("Score"));
		const FString Disabled = Editor.AddTest(Option, TEXT("Distance"));
		Editor.SetProperty(Disabled, TEXT("TestPurpose"), TEXT("Filter"));
		Editor.SetProperty(Disabled, TEXT("FilterType"), TEXT("Maximum"));
		Editor.SetProperty(Disabled, TEXT("FloatValueMax"), TEXT("(DefaultValue=10.000000)"));
		Editor.Toggle(Disabled, false);
		Editor.SaveQuery(*this, TEXT("EQS_RunGrid"));
	}
	FQueryWorld World;
	if (!TestNotNull(TEXT("game world"), World.World) || !TestNotNull(TEXT("EQS manager"), UEnvQueryManager::GetCurrent(World.World)))
	{
		return false;
	}
	for (const FVector& Location : {FVector(200, 0, 0), FVector(0, 700, 0), FVector(-1000, -300, 0), FVector(1200, 900, 0), FVector(3000, 0, 0)})
	{
		World.Spawn(Location);
	}
	for (UEnvQuery* Source : {Actors, Grid})
	{
		const FString Name = Source->GetName();
		const TSharedPtr<FJsonObject> Doc = Extract(*this, Source->GetPathName(), Name);
		const FTarget& Created = Fixtures.Add(TEXT("EQS_RunCopy"));
		const FRoundTrip Trip = RoundTrip(*this, *Source, Doc, Created, Name);
		for (const FString& Problem : Trip.Problems)
		{
			AddError(FString::Printf(TEXT("%s: %s"), *Name, *Problem));
		}
		UEnvQuery* Copy = Created.Query();
		if (!TestNotNull(*FString::Printf(TEXT("%s: copy"), *Name), Copy))
		{
			continue;
		}
		const TArray<FString> Expected = World.Run(*Source);
		const TArray<FString> Actual = World.Run(*Copy);
		AddInfo(FString::Printf(TEXT("%s: %d items: %s"), *Name, Expected.Num() - 1, *FString::Join(Expected, TEXT(" | ")).Left(1500)));
		TestTrue(*FString::Printf(TEXT("%s returns items"), *Name), Expected.Num() > 1);
		TestEqual(*FString::Printf(TEXT("%s: the re-created query returns the same items and scores"), *Name), FString::Join(Actual, TEXT("\n")), FString::Join(Expected, TEXT("\n")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentEnvQueryFreeEditsTest, "UEAssetDoc.EnvQuery.FreeEdits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEnvQueryFreeEditsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentEnvQueryTests;
	FFixtures Fixtures;
	const FTarget& EditorTarget = Fixtures.Add(TEXT("EQS_Cover"));
	const FCover Cover = MakeCover(*this, EditorTarget);
	if (!TestNotNull(TEXT("EQS_Cover"), Cover.Query))
	{
		return false;
	}
	// 1. Written by hand (keys of its own): the same query as the editor-built one.
	const FTarget& Target = Fixtures.Add(TEXT("EQS_Free"));
	const TSharedPtr<FJsonObject> Hand = Document(Target, TEXT(R"({
		"Root": {"Position": [0, 0]},
		"Options": [
			{"Key": "Grid", "Generator": {"Class": "SimpleGrid", "Properties": {"GridSize": {"DefaultValue": 800.0}}}, "Position": [0, 160],
			 "Tests": [{"Key": "Near", "Class": "Distance", "Properties": {"TestPurpose": "Score"}}, {"Key": "Vis", "Class": "Trace"}]},
			{"Key": "Points", "Generator": {"Class": "ActorsOfClass", "Properties": {"SearchedActorClass": "/Script/Engine.TargetPoint"}}, "Position": [320, 160],
			 "Tests": [{"Key": "Facing", "Class": "Dot"}]}
		],
		"Comments": {}})"));
	if (!ApplyAndDiff(*this, Target, Hand, TEXT("hand-written")))
	{
		AddInfo(FString::Printf(TEXT("editor-built: %s"), *Serialize(Extract(*this, EditorTarget.ObjectPath, TEXT("editor-built")))));
		return false;
	}
	FFormOptions NoNames;
	NoNames.bNames = false;
	TestEqual(TEXT("hand-written = editor-built (graph, runtime options, params)"), QueryForm(*Target.Query(), NoNames), QueryForm(*Cover.Query, NoNames));
	if (QueryForm(*Target.Query(), NoNames) != QueryForm(*Cover.Query, NoNames))
	{
		AddInfo(FirstLineDifference(QueryForm(*Cover.Query, NoNames), QueryForm(*Target.Query(), NoNames)));
	}
	TestEqual(TEXT("keys name the objects"), RuntimeTests(*Target.Query(), 0), FString(TEXT("Near:EnvQueryTest_Distance,Vis:EnvQueryTest_Trace")));

	auto Current = [this, &Target]() { return ForTarget(Extract(*this, Target.ObjectPath, TEXT("current")), Target, TEXT("Update")); };
	auto TestsOf = [](const TSharedPtr<FJsonObject>& Option) { return ArrayOf(Option, TEXT("Tests")); };

	// 2. Reorder the tests, add one (keyless) and remove one.
	{
		const TSharedPtr<FJsonObject> Doc = Current();
		TArray<TSharedPtr<FJsonValue>> Tests = TestsOf(OptionAt(Doc, 0));
		Tests.Swap(0, 1);
		Tests.Add(MakeShared<FJsonValueObject>(Parse(TEXT(R"({"Class": "Project"})"))));
		OptionAt(Doc, 0)->SetArrayField(TEXT("Tests"), Tests);
		OptionAt(Doc, 1)->SetArrayField(TEXT("Tests"), {});
		if (ApplyAndDiff(*this, Target, Doc, TEXT("reorder / add / remove")))
		{
			const FString Runtime = RuntimeTests(*Target.Query(), 0);
			TestTrue(TEXT("reordered"), Runtime.StartsWith(TEXT("Vis:EnvQueryTest_Trace,Near:EnvQueryTest_Distance,")));
			TestTrue(TEXT("added"), Runtime.EndsWith(TEXT("EnvQueryTest_Project")));
			TestEqual(TEXT("removed"), RuntimeTests(*Target.Query(), 1), FString());
			TestEqual(TEXT("test order follows the list"), Target.Query()->GetOptions()[0]->Tests[2]->TestOrder, 2);
		}
	}
	// 3. Filter and score settings.
	{
		const TSharedPtr<FJsonObject> Doc = Current();
		const TSharedPtr<FJsonObject> Near = TestsOf(OptionAt(Doc, 0))[1]->AsObject();
		Near->SetObjectField(TEXT("Properties"), Parse(TEXT(R"({"TestPurpose": "FilterAndScore", "FilterType": "Range",
			"FloatValueMin": {"DefaultValue": 100.0}, "FloatValueMax": {"DefaultValue": 900.0}, "ScoringEquation": "Square",
			"ScoringFactor": {"DefaultValue": -1.0}})")));
		if (ApplyAndDiff(*this, Target, Doc, TEXT("filter / score")))
		{
			const UEnvQueryTest* Test = Target.Query()->GetOptions()[0]->Tests[1];
			TestEqual(TEXT("purpose"), static_cast<int32>(Test->TestPurpose), static_cast<int32>(EEnvTestPurpose::FilterAndScore));
			TestEqual(TEXT("filter type"), static_cast<int32>(Test->FilterType), static_cast<int32>(EEnvTestFilterType::Range));
			TestEqual(TEXT("max"), Test->FloatValueMax.DefaultValue, 900.0f);
			TestEqual(TEXT("equation"), static_cast<int32>(Test->ScoringEquation), static_cast<int32>(EEnvTestScoreEquation::Square));
			TestEqual(TEXT("factor"), Test->ScoringFactor.DefaultValue, -1.0f);
		}
	}
	// 4. Switch the second option's generator (its tests move to the new one), bind the grid size to a query param.
	{
		const TSharedPtr<FJsonObject> Doc = Current();
		OptionAt(Doc, 1)->SetObjectField(TEXT("Generator"), Parse(TEXT(R"({"Class": "Donut", "Properties": {"OuterRadius": {"DefaultValue": 1200.0}}})")));
		OptionAt(Doc, 1)->SetArrayField(TEXT("Tests"), {MakeShared<FJsonValueObject>(Parse(TEXT(R"({"Key": "Facing", "Class": "Dot"})")))});
		Child(OptionAt(Doc, 0), TEXT("Generator"))->SetObjectField(TEXT("Properties"), Parse(TEXT(R"({"GridSize": {"DefaultValue": 800.0,
			"DataBinding": {"Class": "/Script/AIModule.AIDataProvider_QueryParams", "Properties": {"ParamName": "Radius"}}, "DataField": "FloatValue"}})")));
		if (ApplyAndDiff(*this, Target, Doc, TEXT("switch generator / bind param")))
		{
			TestEqual(TEXT("generators"), RuntimeGenerators(*Target.Query()), FString(TEXT("Grid:EnvQueryGenerator_SimpleGrid,Points:EnvQueryGenerator_Donut")));
			TestEqual(TEXT("test kept on the new generator"), RuntimeTests(*Target.Query(), 1), FString(TEXT("Facing:EnvQueryTest_Dot")));
			TArray<FAIDynamicParam> Params;
			Target.Query()->CollectQueryParams(*GetTransientPackage(), Params);
			TestTrue(TEXT("Radius is a query param"), Params.ContainsByPredicate([](const FAIDynamicParam& Param) { return Param.ParamName == TEXT("Radius"); }));
			AddInfo(FString::Printf(TEXT("after switch: %s"), *Serialize(Extract(*this, Target.ObjectPath, TEXT("after switch"))).Left(3000)));
		}
	}
	// 5. Toggle, unwire, swap the run order, move a test to the other option, comments.
	{
		const TSharedPtr<FJsonObject> Doc = Current();
		const TSharedPtr<FJsonObject> Grid = OptionAt(Doc, 0);
		const TSharedPtr<FJsonObject> Points = OptionAt(Doc, 1);
		TArray<TSharedPtr<FJsonValue>> GridTests = TestsOf(Grid);
		GridTests[0]->AsObject()->SetBoolField(TEXT("Enabled"), false);
		TArray<TSharedPtr<FJsonValue>> PointTests = TestsOf(Points);
		PointTests.Add(GridTests.Pop());
		Grid->SetArrayField(TEXT("Tests"), GridTests);
		Points->SetArrayField(TEXT("Tests"), PointTests);
		Points->SetArrayField(TEXT("Position"), {MakeShared<FJsonValueNumber>(-320), MakeShared<FJsonValueNumber>(160)});
		Points->SetStringField(TEXT("Comment"), TEXT("ring first"));
		BodyOf(Doc)->SetArrayField(TEXT("Options"), {MakeShared<FJsonValueObject>(Points), MakeShared<FJsonValueObject>(Grid)});
		BodyOf(Doc)->SetObjectField(TEXT("Comments"), Parse(TEXT(R"({"Note": {"Text": "fallback order", "Position": [-400, -100], "Size": [900, 500]}})")));
		if (ApplyAndDiff(*this, Target, Doc, TEXT("toggle / order / move / comments")))
		{
			TestEqual(TEXT("ring runs first"), RuntimeGenerators(*Target.Query()), FString(TEXT("Points:EnvQueryGenerator_Donut,Grid:EnvQueryGenerator_SimpleGrid")));
			TestTrue(TEXT("moved test"), RuntimeTests(*Target.Query(), 0).EndsWith(TEXT("EnvQueryTest_Project")));
			TestFalse(TEXT("disabled test left the runtime options"), RuntimeTests(*Target.Query(), 1).Contains(TEXT("Vis:")));
		}
	}
	// 6. Unwire the grid: only the ring runs; the grid stays in the graph.
	{
		const TSharedPtr<FJsonObject> Doc = Current();
		OptionAt(Doc, 1)->SetBoolField(TEXT("Connected"), false);
		if (ApplyAndDiff(*this, Target, Doc, TEXT("unwire")))
		{
			TestEqual(TEXT("one runtime option"), Target.Query()->GetOptions().Num(), 1);
			TestEqual(TEXT("two options in the graph"), OptionsOf(Extract(*this, Target.ObjectPath, TEXT("unwired"))).Num(), 2);
		}
	}
	// The final state survives a reload.
	const FString Final = Serialize(Extract(*this, Target.ObjectPath, TEXT("final")));
	ReleaseUndoHistory();
	UPackageTools::UnloadPackages({Target.Get()->GetOutermost()});
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestNotNull(TEXT("reload"), LoadObject<UEnvQuery>(nullptr, *Target.ObjectPath));
	TestEqual(TEXT("reload is stable"), Serialize(Extract(*this, Target.ObjectPath, TEXT("reloaded"))), Final);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentEnvQueryRollbackTest, "UEAssetDoc.EnvQuery.Rollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEnvQueryRollbackTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentEnvQueryTests;
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("EQS_Rollback"));
	const FCover Cover = MakeCover(*this, Target);
	if (!TestNotNull(TEXT("EQS_Cover"), Cover.Query))
	{
		return false;
	}
	const FString Before = QueryForm(*Cover.Query);
	const TSharedPtr<FJsonObject> Base = ForTarget(Extract(*this, Target.ObjectPath, TEXT("rollback base")), Target, TEXT("Update"));
	// Edits that succeed (a new option, a test, a property, a comment), then a Composite whose generators make different
	// item types: only the query's validation (finalize) finds it.
	{
		const TSharedPtr<FJsonObject> Doc = Parse(Serialize(Base));
		TArray<TSharedPtr<FJsonValue>> Options = OptionsOf(Doc);
		Options.Add(MakeShared<FJsonValueObject>(Parse(TEXT(R"({"Key": "Mixed", "Generator": {"Class": "Composite", "Properties": {"Generators": [
			{"Class": "/Script/AIModule.EnvQueryGenerator_SimpleGrid"}, {"Class": "/Script/AIModule.EnvQueryGenerator_ActorsOfClass"}]}},
			"Tests": [{"Class": "Distance"}]})"))));
		BodyOf(Doc)->SetArrayField(TEXT("Options"), Options);
		Child(OptionAt(Doc, 0), TEXT("Generator"))->SetObjectField(TEXT("Properties"), Parse(TEXT(R"({"GridSize": {"DefaultValue": 1234.0}})")));
		BodyOf(Doc)->SetObjectField(TEXT("Comments"), Parse(TEXT(R"({"Note": {"Text": "x", "Position": [0, 0]}})")));
		ExpectRefused(*this, Target, Doc, TEXT("EnvQuery.ValidationFailed"), TEXT("/Body/Options/2"), TEXT("failed validation"));
		TestEqual(TEXT("failed validation: query as before"), QueryForm(*Target.Query()), Before);
	}
	// A value the Details panel refuses, after a new option and a test reorder.
	{
		const TSharedPtr<FJsonObject> Doc = Parse(Serialize(Base));
		TArray<TSharedPtr<FJsonValue>> Options = OptionsOf(Doc);
		Options.Add(MakeShared<FJsonValueObject>(Parse(TEXT(R"({"Key": "Ring", "Generator": {"Class": "Donut"}})"))));
		BodyOf(Doc)->SetArrayField(TEXT("Options"), Options);
		TArray<TSharedPtr<FJsonValue>> Tests = ArrayOf(OptionAt(Doc, 0), TEXT("Tests"));
		Tests.Swap(0, 1);
		Tests[1]->AsObject()->SetObjectField(TEXT("Properties"), Parse(TEXT(R"({"TestPurpose": "Bogus"})")));
		OptionAt(Doc, 0)->SetArrayField(TEXT("Tests"), Tests);
		ExpectRefused(*this, Target, Doc, nullptr, TEXT("/Body/Options/0/Tests/1/Properties/TestPurpose"), TEXT("bad value"));
		TestEqual(TEXT("bad value: query as before"), QueryForm(*Target.Query()), Before);
	}
	// An unknown property and a contradicting run order are refused before any edit.
	{
		const TSharedPtr<FJsonObject> Doc = Parse(Serialize(Base));
		Child(OptionAt(Doc, 1), TEXT("Generator"))->SetObjectField(TEXT("Properties"), Parse(TEXT(R"({"Radius": 1.0})")));
		ExpectRefused(*this, Target, Doc, TEXT("EnvQuery.UnknownProperty"), TEXT("/Body/Options/1/Generator/Properties/Radius"), TEXT("unknown property"));
		const TSharedPtr<FJsonObject> Order = Parse(Serialize(Base));
		OptionAt(Order, 1)->SetArrayField(TEXT("Position"), {MakeShared<FJsonValueNumber>(-640), MakeShared<FJsonValueNumber>(160)});
		ExpectRefused(*this, Target, Order, TEXT("EnvQuery.OptionOrder"), TEXT("/Body/Options/1/Position"), TEXT("contradicting order"));
	}
	// A failed Create leaves nothing behind.
	{
		const FTarget& Failed = Fixtures.Add(TEXT("EQS_FailedCreate"));
		const TSharedPtr<FJsonObject> Doc = ForTarget(Base, Failed, TEXT("Create"));
		Child(OptionAt(Doc, 0), TEXT("Generator"))->SetObjectField(TEXT("Properties"), Parse(TEXT(R"({"GridSize": {"DefaultValue": "big"}})")));
		const FAssetDocumentResult Result = Apply(Failed, Doc);
		TestFalse(TEXT("failed create"), Result.IsSuccess());
		AddInfo(FString::Printf(TEXT("failed create: %s"), *DiagnosticsText(Result)));
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		TestNull(TEXT("failed create: no object left"), Failed.Get());
		TestFalse(TEXT("failed create: no package file"), IFileManager::Get().FileExists(*Failed.PackageFile));
	}
	// The query still applies normally afterwards.
	const TSharedPtr<FJsonObject> Doc = Parse(Serialize(Base));
	Child(OptionAt(Doc, 0), TEXT("Generator"))->SetObjectField(TEXT("Properties"), Parse(TEXT(R"({"GridSize": {"DefaultValue": 1000.0}})")));
	TestTrue(TEXT("apply after the rollbacks"), ApplyAndDiff(*this, Target, Doc, TEXT("after rollbacks")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentEnvQueryUndoTest, "UEAssetDoc.EnvQuery.Undo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEnvQueryUndoTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentEnvQueryTests;
	if (GEditor == nullptr || GEditor->Trans == nullptr)
	{
		return false;
	}
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("EQS_Undo"));
	const FCover Cover = MakeCover(*this, Target);
	if (!TestNotNull(TEXT("EQS_Cover"), Cover.Query))
	{
		return false;
	}
	auto UndoRedoAll = [this, &Target](const TCHAR* Stage)
	{
		int32 Undone = 0;
		while (Undone < 24 && GEditor->Trans->CanUndo() && GEditor->UndoTransaction())
		{
			++Undone;
			TestTrue(*FString::Printf(TEXT("%s: whole after undo %d"), Stage, Undone), ExtractResult(Target.ObjectPath).IsSuccess());
		}
		int32 Redone = 0;
		while (Redone < Undone && GEditor->Trans->CanRedo() && GEditor->RedoTransaction())
		{
			++Redone;
		}
		TestEqual(*FString::Printf(TEXT("%s: every undone step redoes"), Stage), Redone, Undone);
	};
	// 1. The human adds a test and saves; the AI edits: history referencing the query is cleared.
	FEditorQuery Editor(*Target.Query());
	Editor.AddTest(Cover.Actors, TEXT("Overlap"));
	Save(*this, Target.Get(), TEXT("human save"));
	TestTrue(TEXT("the human edit is in the undo history"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	const TSharedPtr<FJsonObject> Doc = ForTarget(Extract(*this, Target.ObjectPath, TEXT("after human edit")), Target, TEXT("Update"));
	TArray<TSharedPtr<FJsonValue>> Options = OptionsOf(Doc);
	Options.Add(MakeShared<FJsonValueObject>(Parse(TEXT(R"({"Key": "Ring", "Generator": {"Class": "Donut"}, "Tests": [{"Class": "Distance"}]})"))));
	BodyOf(Doc)->SetArrayField(TEXT("Options"), Options);
	if (!ApplyOk(*this, Target, Doc, TEXT("AI edit")))
	{
		return false;
	}
	TestFalse(TEXT("after AI edit: no undo history references the query"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	const FString AfterEdit = Serialize(Extract(*this, Target.ObjectPath, TEXT("after AI edit")));
	TestTrue(TEXT("human test kept"), AfterEdit.Contains(TEXT("\"Overlap\"")));
	UndoRedoAll(TEXT("after AI edit"));
	TestEqual(TEXT("after AI edit + undo/redo"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after undo/redo"))), AfterEdit);

	// 2. The human edits again; an AI apply fails in validation and rolls back to the human state.
	ReleaseUndoHistory();
	FEditorQuery Again(*Target.Query());
	if (UAIGraphNode* Node = Again.Option(Cover.Grid))
	{
		Again.Move(*Node, -64, 160);
	}
	const FString SecondHuman = Serialize(Extract(*this, Target.ObjectPath, TEXT("after second human edit")));
	const TSharedPtr<FJsonObject> Failing = ForTarget(Parse(SecondHuman), Target, TEXT("Update"));
	TArray<TSharedPtr<FJsonValue>> More = OptionsOf(Failing);
	More.Add(MakeShared<FJsonValueObject>(Parse(TEXT(R"({"Key": "Mixed", "Generator": {"Class": "Composite", "Properties": {"Generators": [
		{"Class": "/Script/AIModule.EnvQueryGenerator_SimpleGrid"}, {"Class": "/Script/AIModule.EnvQueryGenerator_ActorsOfClass"}]}}})"))));
	BodyOf(Failing)->SetArrayField(TEXT("Options"), More);
	const FAssetDocumentResult Failed = Apply(Target, Failing);
	TestFalse(TEXT("failing apply fails"), Failed.IsSuccess());
	TestEqual(TEXT("after rollback: the human state"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after rollback"))), SecondHuman);
	UndoRedoAll(TEXT("after rollback"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentEnvQueryOpenEditorApplyTest, "UEAssetDoc.EnvQuery.OpenEditorApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEnvQueryOpenEditorApplyTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentEnvQueryTests;
	UAssetEditorSubsystem* Editors = GEditor != nullptr ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("asset editor subsystem"), Editors) || GEditor->Trans == nullptr)
	{
		return false;
	}
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("EQS_Open"));
	const FCover Cover = MakeCover(*this, Target);
	if (!TestNotNull(TEXT("EQS_Cover"), Cover.Query))
	{
		return false;
	}
	const bool bOpened = Editors->OpenEditorForAsset(Cover.Query);
	UE::AssetDocument::OpenEditorTests::TickEditorUi();
	if (!bOpened)
	{
		AddWarning(TEXT("The Environment Query editor did not open in this session; the open-editor apply was not exercised."));
		return true;
	}
	ON_SCOPE_EXIT
	{
		if (UObject* Open = Target.Get())
		{
			Editors->CloseAllEditorsForAsset(Open);
		}
	};
	// Opening runs the graph's Initialize: CalculateAllWeights updates the test weights the graph displays
	// (SetDisplayedWeight's Modify), which dirties the package although the human changed nothing. The human does not
	// save: the apply accepts these open-time changes (the apply's finalize recomputes the weights).
	const UPackage* OpenedPackage = Cover.Query->GetPackage();
	TestTrue(TEXT("opening the editor recomputes the weights and dirties the package"), OpenedPackage->IsDirty());
	TestTrue(TEXT("the package is dirty only from the editor's open"), UE::AssetDocument::OpenEditor::HasOnlyOpenTimeChanges(*OpenedPackage));
	ReleaseUndoHistory();
	UE::AssetDocument::OpenEditorTests::TickEditorUi();
	UEnvQuery* Query = Target.Query();
	UEdGraph* Graph = Query != nullptr ? Query->EdGraph.Get() : nullptr;
	TSharedPtr<SGraphEditor> GraphEditor = Graph != nullptr ? SGraphEditor::FindGraphEditorForGraph(Graph) : nullptr;
	if (!TestTrue(TEXT("the open editor shows the query's graph"), GraphEditor.IsValid() && GraphEditor->GetCurrentGraph() == Graph))
	{
		return false;
	}
	const bool bPainted = UE::AssetDocument::OpenEditorTests::PanelShowsEveryNode(*Graph);
	AddInfo(FString::Printf(TEXT("the graph panel %s"), bPainted ? TEXT("is drawn (node widgets checked)") : TEXT("is not drawn in this session (node widgets not checked)")));

	// The human selects the actors option in the open editor; an apply adds a wired Donut option.
	GraphEditor->SetNodeSelection(FEditorQuery(*Query).Option(Cover.Actors), true);
	TestEqual(TEXT("the human's selection"), GraphEditor->GetSelectedNodes().Num(), 1);
	const int32 OptionsBefore = Query->GetOptions().Num();
	const TSharedPtr<FJsonObject> Doc = ForTarget(Extract(*this, Target.ObjectPath, TEXT("open base")), Target, TEXT("Update"));
	TArray<TSharedPtr<FJsonValue>> Options = OptionsOf(Doc);
	Options.Add(MakeShared<FJsonValueObject>(Parse(TEXT(R"({"Key": "Ring", "Generator": {"Class": "Donut"}, "Tests": [{"Class": "Distance"}]})"))));
	BodyOf(Doc)->SetArrayField(TEXT("Options"), Options);
	if (!ApplyOk(*this, Target, Doc, TEXT("apply while open")))
	{
		return false;
	}
	Query = Target.Query();
	Graph = Query != nullptr ? Query->EdGraph.Get() : nullptr;
	if (!TestNotNull(TEXT("the query's graph after the apply"), Graph))
	{
		return false;
	}
	TestEqual(TEXT("the query runs the new option"), Query->GetOptions().Num(), OptionsBefore + 1);
	TestFalse(TEXT("the apply saved the editor's open-time changes with its own"), Query->GetPackage()->IsDirty());
	TestFalse(TEXT("the apply is not an undo step"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	TestTrue(TEXT("the editor is still open"), Editors->FindEditorForAsset(Query, false) != nullptr);
	GraphEditor = SGraphEditor::FindGraphEditorForGraph(Graph);
	TestTrue(TEXT("the open editor still shows the query's graph"), GraphEditor.IsValid() && GraphEditor->GetCurrentGraph() == Graph);
	TestEqual(TEXT("the open editor dropped its selection (as its Undo does)"), GraphEditor.IsValid() ? GraphEditor->GetSelectedNodes().Num() : -1, 0);
	const UAIGraphNode* Ring = FEditorQuery(*Query).Option(TEXT("Ring"));
	TestNotNull(TEXT("the graph has the new option node"), Ring);
	const FGuid RingGuid = Ring != nullptr ? Ring->NodeGuid : FGuid();
	UE::AssetDocument::OpenEditorTests::TickEditorUi();
	if (bPainted)
	{
		TestTrue(TEXT("the graph panel shows every node after the apply"), UE::AssetDocument::OpenEditorTests::PanelShowsEveryNode(*Graph));
	}
	const FString Applied = QueryForm(*Query);
	const FString AppliedDocument = Serialize(Extract(*this, Target.ObjectPath, TEXT("after apply while open")));

	// A value the Details panel refuses, after a test reorder: rolled back with the editor open.
	{
		const TSharedPtr<FJsonObject> Failing = Parse(AppliedDocument);
		TArray<TSharedPtr<FJsonValue>> Tests = ArrayOf(OptionAt(Failing, 0), TEXT("Tests"));
		Tests.Swap(0, 1);
		Tests[1]->AsObject()->SetObjectField(TEXT("Properties"), Parse(TEXT(R"({"TestPurpose": "Bogus"})")));
		OptionAt(Failing, 0)->SetArrayField(TEXT("Tests"), Tests);
		Failing->SetStringField(TEXT("Action"), TEXT("Update"));
		ExpectRefused(*this, Target, Failing, nullptr, TEXT("/Body/Options/0/Tests/1/Properties/TestPurpose"), TEXT("failing apply while open"));
		Query = Target.Query();
		Graph = Query != nullptr ? Query->EdGraph.Get() : nullptr;
		if (!TestNotNull(TEXT("the query's graph after the rollback"), Graph))
		{
			return false;
		}
		TestEqual(TEXT("after the rollback the query is as applied"), QueryForm(*Query), Applied);
		GraphEditor = SGraphEditor::FindGraphEditorForGraph(Graph);
		TestTrue(TEXT("after the rollback the open editor shows the query's graph"), GraphEditor.IsValid() && GraphEditor->GetCurrentGraph() == Graph);
		UE::AssetDocument::OpenEditorTests::TickEditorUi();
		if (bPainted)
		{
			TestTrue(TEXT("after the rollback the graph panel shows every node"), UE::AssetDocument::OpenEditorTests::PanelShowsEveryNode(*Graph));
		}
	}

	// The editor's own edits and undo / redo after the apply. Undo / Redo renames the moved nodes' instances
	// (UAIGraphNode::PostEditUndo -> ResetNodeOwner); the keys (the document's "Ring", the human's option's) stay.
	{
		const UEnvQueryOption* RingOption = Ring != nullptr ? EQ::GetOption(*Ring) : nullptr;
		const FString RingInstance = RingOption != nullptr ? RingOption->GetName() : FString();
		if (UAIGraphNode* Node = FEditorQuery(*Query).Option(Cover.Grid))
		{
			FEditorQuery(*Query).Move(*Node, -64, 160);
		}
		if (UAIGraphNode* Node = FEditorQuery(*Query).Option(TEXT("Ring")))
		{
			FEditorQuery(*Query).Move(*Node, 900, 420);
		}
		const FString Human = Serialize(Extract(*this, Target.ObjectPath, TEXT("after the human edit")));
		TestNotEqual(TEXT("the human edit landed"), Human, AppliedDocument);
		TestTrue(TEXT("the human edits undo"), GEditor->Trans->CanUndo() && GEditor->UndoTransaction() && GEditor->UndoTransaction());
		const UAIGraphNode* Undone = FEditorQuery(*Query).Option(TEXT("Ring"));
		const UEnvQueryOption* UndoneOption = Undone != nullptr ? EQ::GetOption(*Undone) : nullptr;
		TestTrue(TEXT("undo renamed the Ring option's instance (the engine's ResetNodeOwner)"),
			UndoneOption != nullptr && UndoneOption->GetName() != RingInstance);
		TestEqual(TEXT("undo restores the applied query, keys included"),
			Serialize(Extract(*this, Target.ObjectPath, TEXT("after undo"))), AppliedDocument);
		TestTrue(TEXT("the human edits redo"), GEditor->Trans->CanRedo() && GEditor->RedoTransaction() && GEditor->RedoTransaction());
		TestEqual(TEXT("redo restores the human edits, keys included"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after redo"))), Human);
		UE::AssetDocument::OpenEditorTests::TickEditorUi();
		// The human edit is a change of the human's own: an apply now refuses the unsaved package, as for any asset.
		const UPackage* EditedPackage = Target.Get() != nullptr ? Target.Get()->GetPackage() : nullptr;
		TestTrue(TEXT("the human edit left the package unsaved"), EditedPackage != nullptr && EditedPackage->IsDirty()
			&& !UE::AssetDocument::OpenEditor::HasOnlyOpenTimeChanges(*EditedPackage));
		const FAssetDocumentResult Unsaved = Apply(Target, ForTarget(Extract(*this, Target.ObjectPath, TEXT("unsaved edit")), Target, TEXT("Update")));
		TestTrue(TEXT("an apply refuses the unsaved human edit"), !Unsaved.IsSuccess() && Unsaved.Diagnostics.ContainsByPredicate(
			[](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == TEXT("DirtyExistingAssetTransactionUnsupported"); }));
		TestTrue(TEXT("after the refusal the package is still unsaved"), EditedPackage != nullptr && EditedPackage->IsDirty());
		Save(*this, Target.Get(), TEXT("the human edit"));
	}

	// A second apply removes the new option: the panel drops its widget.
	{
		const TSharedPtr<FJsonObject> Again = ForTarget(Extract(*this, Target.ObjectPath, TEXT("before the second apply")), Target, TEXT("Update"));
		TArray<TSharedPtr<FJsonValue>> Kept = OptionsOf(Again);
		Kept.RemoveAll([](const TSharedPtr<FJsonValue>& Option) { return Option->AsObject()->GetStringField(TEXT("Key")) == TEXT("Ring"); });
		BodyOf(Again)->SetArrayField(TEXT("Options"), Kept);
		const bool bAppliedAgain = ApplyOk(*this, Target, Again, TEXT("second apply while open"));
		Query = Target.Query();
		Graph = Query != nullptr ? Query->EdGraph.Get() : nullptr;
		if (bAppliedAgain && TestNotNull(TEXT("the query's graph after the second apply"), Graph))
		{
			TestEqual(TEXT("the query drops the option"), Query->GetOptions().Num(), OptionsBefore);
			UE::AssetDocument::OpenEditorTests::TickEditorUi();
			TestFalse(TEXT("the graph panel holds no widget of the removed option"), UE::AssetDocument::OpenEditorTests::PanelHasNodeWidget(*Graph, RingGuid));
			if (bPainted)
			{
				TestTrue(TEXT("after the second apply the graph panel shows every node"), UE::AssetDocument::OpenEditorTests::PanelShowsEveryNode(*Graph));
			}
		}
	}
	if (Query != nullptr)
	{
		TestTrue(TEXT("the query is still open in its editor"), Editors->FindEditorForAsset(Query, false) != nullptr);
		Editors->CloseAllEditorsForAsset(Query);
		TestNull(TEXT("the editor closed"), Editors->FindEditorForAsset(Query, false));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentEnvQueryBehaviorTreeTest, "UEAssetDoc.EnvQuery.BehaviorTree",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEnvQueryBehaviorTreeTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentEnvQueryTests;
	FFixtures Fixtures;
	const FTarget& QueryTarget = Fixtures.Add(TEXT("EQS_Nearest"));
	const FTarget& BlackboardTarget = Fixtures.Add(TEXT("BB_Nearest"));
	const FTarget& TreeTarget = Fixtures.Add(TEXT("BT_Nearest"));
	// The query: target points within 3000 units, nearest first.
	const TSharedPtr<FJsonObject> QueryDoc = Document(QueryTarget, TEXT(R"({"Options": [
		{"Key": "Points", "Generator": {"Class": "ActorsOfClass", "Properties": {"SearchedActorClass": "/Script/Engine.TargetPoint", "SearchRadius": {"DefaultValue": 3000.0}}},
		 "Tests": [{"Key": "Nearest", "Class": "Distance", "Properties": {"TestPurpose": "Score", "ScoringEquation": "InverseLinear"}}]}]})"));
	if (!ApplyAndDiff(*this, QueryTarget, QueryDoc, TEXT("query")))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> BlackboardDoc = Parse(FString::Printf(TEXT(R"({"SchemaVersion": 1, "Target": "%s", "Class": "/Script/AIModule.BlackboardData",
		"Action": "CreateOrUpdate", "Definitions": {}, "Properties": {}, "Body": {"Parent": null, "Keys": [
		{"Name": "Target", "Category": "", "Description": "", "bInstanceSynced": false,
		 "KeyTypeClass": {"Kind": "ClassRef", "Path": "/Script/AIModule.BlackboardKeyType_Object"},
		 "KeyTypeProperties": {"BaseClass": {"Kind": "ClassRef", "Path": "/Script/Engine.Actor"}, "DefaultValue": null}}]}})"), *BlackboardTarget.PackageName));
	if (!ApplyOk(*this, BlackboardTarget, BlackboardDoc, TEXT("blackboard")))
	{
		return false;
	}
	// Known BehaviorTree-profile gap: a new Run EQS Query task cannot be authored. Its EQSQueryBlackboardKey is
	// edit-disabled while the query template is used (bUseBBKeyForQueryTemplate, an inline toggle without a row of its
	// own) and the editor leaves it None, but the BT profile requires every selector of a new node to name a key
	// (MissingBlackboardKey), refuses to write the disabled selector when one is given, and its selector completion
	// requires an editable selector that allows None. When this refusal goes away, author the task below through
	// the BT profile instead of the editor graph.
	const FString TreeText = FString::Printf(TEXT(R"({"SchemaVersion": 1, "Target": "%s", "Class": "/Script/AIModule.BehaviorTree",
		"Action": "CreateOrUpdate", "Definitions": {}, "Properties": {}, "Body": {
		"BlackboardAsset": {"Kind": "AssetRef", "Path": "%s"},
		"Tree": {"Root": {"Id": "00000000000000000000000000000001", "Class": "/Script/AIModule.BTComposite_Sequence", "Children": [%s
			{"Id": "00000000000000000000000000000003", "Class": "/Script/AIModule.BTTask_Wait", "Editor": {"Position": {"X": 300, "Y": 200}}}]}}}})"),
		*TreeTarget.PackageName, *BlackboardTarget.ObjectPath, TEXT("%s"));
	const FString EqsTask = FString::Printf(TEXT(R"({"Id": "00000000000000000000000000000002", "Class": "/Script/AIModule.BTTask_RunEQSQuery", "Editor": {"Position": {"X": 0, "Y": 200}},
			 "Properties": {"BlackboardKey": {"Key": "Target"}, "EQSRequest": {"QueryTemplate": {"Kind": "AssetRef", "Path": "%s"}, "RunMode": "SingleResult"}}},)"),
		*QueryTarget.ObjectPath);
	{
		const FAssetDocumentResult Refused = Apply(TreeTarget, Parse(TreeText.Replace(TEXT("%s"), *EqsTask)));
		TestFalse(TEXT("BT profile: a new Run EQS Query task is refused (known gap)"), Refused.IsSuccess());
		TestTrue(TEXT("BT profile gap: MissingBlackboardKey on EQSQueryBlackboardKey"), HasCode(Refused, TEXT("MissingBlackboardKey")));
		AddInfo(FString::Printf(TEXT("BT profile, Run EQS Query task: %s"), *DiagnosticsText(Refused)));
	}
	// The rest of the tree through the BT profile; the task from the BT editor's palette (FAISchemaAction_NewNode with
	// a BehaviorTreeGraphNode_Task template, dragged from the Sequence), its Details (query template, result key), saved.
	if (!ApplyOk(*this, TreeTarget, Parse(TreeText.Replace(TEXT("%s"), TEXT(""))), TEXT("behavior tree")))
	{
		return false;
	}
	UBehaviorTree* Tree = Cast<UBehaviorTree>(TreeTarget.Get());
	UBlackboardData* Blackboard = Cast<UBlackboardData>(BlackboardTarget.Get());
	UAIGraph* TreeGraph = Tree != nullptr ? Cast<UAIGraph>(Tree->BTGraph) : nullptr;
	UClass* TaskNodeClass = FindObject<UClass>(nullptr, TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Task"));
	if (!TestNotNull(TEXT("tree"), Tree) || !TestNotNull(TEXT("blackboard"), Blackboard) || !TestNotNull(TEXT("tree graph"), TreeGraph)
		|| !TestNotNull(TEXT("BT task node class"), TaskNodeClass))
	{
		return false;
	}
	UEdGraphPin* SequenceOut = nullptr;
	for (UEdGraphNode* Node : TreeGraph->Nodes)
	{
		const UAIGraphNode* AINode = Cast<UAIGraphNode>(Node);
		if (AINode != nullptr && AINode->NodeInstance != nullptr && AINode->NodeInstance->IsA<UBTComposite_Sequence>())
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				SequenceOut = Pin->Direction == EGPD_Output ? Pin : SequenceOut;
			}
		}
	}
	if (!TestNotNull(TEXT("Sequence output pin"), SequenceOut))
	{
		return false;
	}
	{
		UAIGraphNode* Template = NewObject<UAIGraphNode>(TreeGraph, TaskNodeClass);
		Template->ClassData = FGraphNodeClassData(UBTTask_RunEQSQuery::StaticClass(), FString());
		FAISchemaAction_NewNode Action;
		Action.NodeTemplate = Template;
		UAIGraphNode* TaskNode = Cast<UAIGraphNode>(Action.PerformAction(TreeGraph, SequenceOut, FVector2f(-200.0f, 200.0f), true));
		UBTTask_RunEQSQuery* NewTask = TaskNode != nullptr ? Cast<UBTTask_RunEQSQuery>(TaskNode->NodeInstance) : nullptr;
		if (!TestNotNull(TEXT("Run EQS Query task from the palette"), NewTask))
		{
			return false;
		}
		const FScopedTransaction Transaction(NSLOCTEXT("PropertyEditor", "EditPropertyTransaction", "Edit Property"));
		NewTask->Modify();
		NewTask->EQSRequest.QueryTemplate = QueryTarget.Query();
		NewTask->EQSRequest.RunMode = EEnvQueryRunMode::SingleResult;
		// The Details panel's key picker (BlackboardKey is protected: through its property, as the panel writes it).
		if (const FStructProperty* KeyProperty = CastField<FStructProperty>(UBTTask_RunEQSQuery::StaticClass()->FindPropertyByName(TEXT("BlackboardKey"))))
		{
			KeyProperty->ContainerPtrToValuePtr<FBlackboardKeySelector>(NewTask)->SelectedKeyName = TEXT("Target");
		}
		FPropertyChangedEvent Event(UBTTask_RunEQSQuery::StaticClass()->FindPropertyByName(TEXT("EQSRequest")), EPropertyChangeType::ValueSet);
		NewTask->PostEditChangeProperty(Event);
		TreeGraph->UpdateAsset();
		if (!Save(*this, Tree, TEXT("behavior tree with the task")))
		{
			return false;
		}
		// The asset registry learns the saved tree's reference to the query when it scans the file (the editor's
		// directory watcher does that a few ticks later).
		FAssetRegistryModule::GetRegistry().ScanFilesSynchronous({FPaths::ConvertRelativePathToFull(TreeTarget.PackageFile)}, true);
	}
	// The task references the query.
	const UBTCompositeNode* Root = Tree->RootNode;
	const UBTTask_RunEQSQuery* Task = nullptr;
	for (int32 Index = 0; Root != nullptr && Index < Root->Children.Num() && Task == nullptr; ++Index)
	{
		Task = Cast<UBTTask_RunEQSQuery>(Root->Children[Index].ChildTask);
	}
	if (!TestNotNull(TEXT("Run EQS Query task"), Task))
	{
		return false;
	}
	TestTrue(TEXT("the task runs the query"), Task->EQSRequest.QueryTemplate.Get() == QueryTarget.Query());
	TestTrue(TEXT("the task runs first"), Root->Children.Num() == 2 && Root->Children[0].ChildTask == Task);
	// Same gap: the BT profile does not re-apply its own extract of a tree with an editor-made Run EQS Query task
	// (EQSQueryBlackboardKey None again). Recorded so the test notices when the BT profile is fixed.
	{
		const TSharedPtr<FJsonObject> TreeExtract = Extract(*this, TreeTarget.ObjectPath, TEXT("tree with the task"));
		const FAssetDocumentResult Reapplied = Apply(TreeTarget, ForTarget(TreeExtract, TreeTarget, TEXT("Update")));
		TestTrue(TEXT("BT profile gap: its extract with the task is refused with MissingBlackboardKey"), !Reapplied.IsSuccess() && HasCode(Reapplied, TEXT("MissingBlackboardKey")));
		AddInfo(FString::Printf(TEXT("BT re-apply of its extract: %s"), *DiagnosticsText(Reapplied)));
	}

	// The tree runs: an AI controller with the blackboard stores the nearest target point.
	{
		FQueryWorld World;
		if (!TestNotNull(TEXT("game world"), World.World))
		{
			return false;
		}
		AActor* Near = World.Spawn(FVector(400, 0, 0));
		World.Spawn(FVector(-1500, 200, 0));
		World.Spawn(FVector(0, 2500, 0));
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AAIController* Controller = World.World->SpawnActor<AAIController>(AAIController::StaticClass(), FTransform::Identity, SpawnParameters);
		// The task queries around the controller's pawn (BTTask_RunEQSQuery::ExecuteTask); the world's querier target
		// point at the origin is 600 away, Near 200.
		// (ADefaultPawn: a bare APawn has no root component, so no location.)
		APawn* Pawn = World.World->SpawnActor<ADefaultPawn>(ADefaultPawn::StaticClass(), FTransform(FVector(600, 0, 0)), SpawnParameters);
		if (!TestNotNull(TEXT("pawn"), Pawn) || Controller == nullptr)
		{
			return false;
		}
		Controller->Possess(Pawn);
		UBlackboardComponent* BlackboardComponent = nullptr;
		if (!TestNotNull(TEXT("AI controller"), Controller) || !TestTrue(TEXT("blackboard in use"), Controller->UseBlackboard(Blackboard, BlackboardComponent)))
		{
			return false;
		}
		TestTrue(TEXT("tree runs"), Controller->RunBehaviorTree(Tree));
		UBehaviorTreeComponent* Brain = Cast<UBehaviorTreeComponent>(Controller->GetBrainComponent());
		// A game world that never began play: the brain component created by RunBehaviorTree is not initialized
		// (UBrainComponent::InitializeComponent caches the blackboard), so cache it as that would.
		if (Brain != nullptr && Brain->GetBlackboardComponent() == nullptr)
		{
			Brain->CacheBlackboardComponent(BlackboardComponent);
		}
		if (!TestTrue(TEXT("the tree's brain has the blackboard"), Brain != nullptr && Brain->GetBlackboardComponent() == BlackboardComponent))
		{
			return false;
		}
		UEnvQueryManager* Manager = UEnvQueryManager::GetCurrent(World.World);
		for (int32 Step = 0; Step < 20 && Brain != nullptr && Manager != nullptr && BlackboardComponent->GetValueAsObject(TEXT("Target")) == nullptr; ++Step)
		{
			Brain->TickComponent(0.1f, LEVELTICK_All, nullptr);
			Manager->Tick(0.1f);
		}
		const UObject* Stored = BlackboardComponent->GetValueAsObject(TEXT("Target"));
		TestTrue(TEXT("the tree stored the query's best item"), Stored == Near);
		AddInfo(FString::Printf(TEXT("Target = %s, pawn at %s"), Stored != nullptr ? *Stored->GetName() : TEXT("None"), *Pawn->GetActorLocation().ToString()));
		if (Brain != nullptr)
		{
			Brain->StopTree(EBTStopMode::Forced);
		}
	}

	// The query lists the tree as dependent and warns when its params change.
	const FAssetDocumentResult Inspected = Inspect(QueryTarget.ObjectPath);
	TestTrue(TEXT("inspect lists the tree"), Inspected.IsSuccess() && Serialize(Inspected.Payload).Contains(TreeTarget.AssetName));
	const TSharedPtr<FJsonObject> Bound = ForTarget(Extract(*this, QueryTarget.ObjectPath, TEXT("query")), QueryTarget, TEXT("Update"));
	Child(OptionAt(Bound, 0), TEXT("Generator"))->SetObjectField(TEXT("Properties"), Parse(TEXT(R"({"SearchedActorClass": "/Script/Engine.TargetPoint",
		"SearchRadius": {"DefaultValue": 3000.0, "DataBinding": {"Class": "/Script/AIModule.AIDataProvider_QueryParams", "Properties": {"ParamName": "Range"}}, "DataField": "FloatValue"}})")));
	const FAssetDocumentResult Changed = Apply(QueryTarget, Bound);
	TestTrue(TEXT("param change applies"), Changed.IsSuccess());
	TestTrue(TEXT("param change warns about the tree"), HasCode(Changed, TEXT("EnvQuery.DependentParams")));
	if (!HasCode(Changed, TEXT("EnvQuery.DependentParams")))
	{
		AddInfo(DiagnosticsText(Changed));
	}
	return true;
}
#endif // WITH_DEV_AUTOMATION_TESTS
