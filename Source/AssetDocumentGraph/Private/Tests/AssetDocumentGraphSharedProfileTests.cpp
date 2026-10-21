// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// The Blueprint graph features the Widget, Anim and Level Sequence Director Blueprint Profiles share with the
// Blueprint Class Profile, on the production route (FAssetDocumentService -> Generic Runtime -> the Python Profile ->
// graph primitives).
// KI-11: a document whose only change is "Layout": "Auto" is a change. Each RelayoutDiff test piles a
// graph's nodes on one spot, then sends the same graph without Positions and with "Layout": "Auto": Diff reports the
// relayout at the graph's own path (/Body/<section>/<n>/Layout, a subgraph's or nested graph's path), the apply runs
// and saves (it is not skipped as a no-op), Diff is empty afterwards and the apply again changes nothing.
//   Blueprint.Layout.RelayoutDiffCollapsed   a Blueprint Class collapsed graph (K2:Composite subgraph).
//   WidgetBlueprint.Layout.RelayoutDiff      a Widget Blueprint's event graph and a collapsed graph in it.
//   AnimBlueprint.Layout.RelayoutDiff        an Anim Blueprint's state machine graph (nested anim graph) and its
//                                            event graph.
//   Sequencer.Director.RelayoutDiff          a Level Sequence Director Blueprint's function graph.
// Validate's graph check (node keys, pin names, conversion links) of the K2 graphs, on a new asset's
// trial Blueprint and on the existing target, as for a Blueprint Class (Blueprint.Authoring.ValidateGraphs).
//   WidgetBlueprint.Authoring.ValidateGraphs  also: a bound event of a widget the same document adds is left to Apply.
//   AnimBlueprint.Authoring.ValidateGraphs
//   Blueprint.Authoring.ValidateComponentBoundEvents  an Actor Blueprint's bound event of a component the same
//                                            document adds is left to Apply.
// (A Level Sequence's Validate already runs its whole apply, Director included, on a transient copy.)

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentGraphSharedProfileTestsLocal
{
struct FRelayoutFixture
{
	FAutomationTestBase& Test;
	FString Folder;
	FString PackageName;
	FString ObjectPath;
	FString PackageFile;
	FString SidecarPath;
	FString Class;

	FRelayoutFixture(FAutomationTestBase& InTest, const TCHAR* Name, const TCHAR* InClass)
		: Test(InTest)
		, Folder(FString(TEXT("AssetDocumentP83/")) + FGuid::NewGuid().ToString(EGuidFormats::Digits))
		, Class(InClass)
	{
		PackageName = FString(TEXT("/Game/")) / Folder / Name;
		ObjectPath = PackageName + TEXT(".") + Name;
		PackageFile = FPaths::ConvertRelativePathToFull(
			FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()));
		SidecarPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectContentDir(), Folder, FString(Name) + TEXT(".assetdoc.json")));
	}

	~FRelayoutFixture()
	{
		if (UPackage* Package = FindPackage(nullptr, *PackageName))
		{
			Package->SetDirtyFlag(false);
			UPackageTools::UnloadPackages({Package});
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		IFileManager::Get().Delete(*PackageFile, false, true);
		IFileManager::Get().Delete(*SidecarPath, false, true);
		FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({PackageFile});
		IFileManager::Get().DeleteDirectory(*FPaths::Combine(FPaths::ProjectContentDir(), Folder), false, true);
	}

	FString Document(const TCHAR* Action, const FString& Body) const
	{
		return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"%s\",\"Action\":\"%s\",\"Properties\":{},\"Body\":{%s}}"),
			*PackageName, *Class, Action, *Body);
	}

	void Report(const FString& Stage, const FAssetDocumentResult& Result) const
	{
		Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *Result.Message));
		for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
		{
			Test.AddInfo(FString::Printf(TEXT("%s: [%s] %s %s %s"), *Stage, *Diagnostic.Severity, *Diagnostic.Code,
				*Diagnostic.Path, *Diagnostic.Message));
		}
	}

	FAssetDocumentResult Apply(const FString& Json, const FString& Stage) const
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(SidecarPath), true);
		FFileHelper::SaveStringToFile(Json, *SidecarPath);
		FAssetDocumentApplyFileRequest Request;
		Request.FilePath = SidecarPath;
		Request.bSaveAsset = true;
		const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
		Report(Stage, Result);
		return Result;
	}

	FAssetDocumentResult Validate(const FString& Json, const FString& Stage) const
	{
		TSharedPtr<FJsonObject> Parsed;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!Test.TestTrue(*FString::Printf(TEXT("%s: document parses"), *Stage), FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid()))
		{
			return FAssetDocumentResult();
		}
		FAssetDocumentValidateRequest Request;
		Request.Document = Parsed;
		const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Validate(Request);
		Report(Stage, Result);
		return Result;
	}

	bool Exists() const
	{
		return FindPackage(nullptr, *PackageName) != nullptr || IFileManager::Get().FileExists(*PackageFile);
	}

	// Validate of the new asset reports the bad pin, the unknown key and the conversion link of Bad and creates
	// nothing; Fixed validates, applies and validates again on the existing target, where BadPin (Fixed with a pin the
	// node does not have) is reported.
	void ValidateGraphs(const FString& Bad, const FString& Fixed, const FString& BadPin) const
	{
		const FAssetDocumentResult Validated = Validate(Document(TEXT("Create"), Bad), TEXT("bad graph validate"));
		Test.TestFalse(TEXT("a new asset with bad links is not valid"), Validated.IsSuccess());
		Test.TestTrue(TEXT("Validate reports the missing pin"), HasDiagnostic(Validated, TEXT("Graph.PinNotFound"), TEXT("/Links/0"), TEXT("Bogus")));
		Test.TestTrue(TEXT("Validate reports the unknown key with suggestions"),
			HasDiagnostic(Validated, TEXT("Graph.ActionNotFound"), TEXT("/Nodes/1/Action"), TEXT("PrintString")));
		Test.TestTrue(TEXT("Validate reports the conversion link"), HasDiagnostic(Validated, TEXT("Graph.ConnectNeedsConversion"), TEXT("/Links/1")));
		Test.TestFalse(TEXT("Validate creates nothing"), Exists());
		const FAssetDocumentResult FixedValid = Validate(Document(TEXT("Create"), Fixed), TEXT("fixed graph validate"));
		Test.TestTrue(TEXT("the fixed document validates"), FixedValid.IsSuccess());
		if (!Test.TestTrue(TEXT("the fixed document applies"), Apply(Document(TEXT("Create"), Fixed), TEXT("fixed graph")).IsSuccess()))
		{
			return;
		}
		Test.TestTrue(TEXT("the fixed document validates on the existing target"),
			Validate(Document(TEXT("Update"), Fixed), TEXT("existing target validate")).IsSuccess());
		const FAssetDocumentResult Existing = Validate(Document(TEXT("Update"), BadPin), TEXT("existing target bad pin"));
		Test.TestTrue(TEXT("Validate of the existing target reports the missing pin"), !Existing.IsSuccess()
			&& HasDiagnostic(Existing, TEXT("Graph.PinNotFound"), TEXT("/Links/"), TEXT("Go")));
	}

	static bool HasDiagnostic(const FAssetDocumentResult& Result, const TCHAR* Code, const TCHAR* PathPart, const TCHAR* MessagePart = TEXT(""))
	{
		return Result.Diagnostics.ContainsByPredicate([Code, PathPart, MessagePart](const FAssetDocumentDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == Code && Diagnostic.Path.Contains(PathPart) && Diagnostic.Message.Contains(MessagePart);
		});
	}

	// The paths Diff reports for Json (null: Diff failed).
	TOptional<TArray<FString>> DiffPaths(const FString& Json, const FString& Stage) const
	{
		TSharedPtr<FJsonObject> Parsed;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!Test.TestTrue(*FString::Printf(TEXT("%s: document parses"), *Stage), FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid()))
		{
			return {};
		}
		FAssetDocumentDiffRequest Request;
		Request.Document = Parsed;
		const FAssetDocumentResult Diff = FAssetDocumentModule::Get().GetService().Diff(Request);
		const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
		if (!Test.TestTrue(*FString::Printf(TEXT("%s: Diff succeeds"), *Stage), Diff.IsSuccess() && Diff.Payload.IsValid()
			&& Diff.Payload->TryGetArrayField(TEXT("changed"), Changed)))
		{
			Report(Stage, Diff);
			return {};
		}
		FString Text;
		const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Diff.Payload.ToSharedRef(), Writer);
		Test.AddInfo(FString::Printf(TEXT("%s: diff %s"), *Stage, *Text.Left(2000)));
		TArray<FString> Paths;
		for (const TSharedPtr<FJsonValue>& Entry : *Changed)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			FString Path;
			if (Entry->TryGetObject(Object) && (*Object)->TryGetStringField(TEXT("path"), Path))
			{
				Paths.Add(Path);
			}
		}
		Paths.Sort();
		return Paths;
	}

	// Piled (every node at one spot) is created; Relayout (no Positions, "Layout": "Auto" on the graphs of Expected)
	// diffs as a relayout of exactly those graphs, applies and saves, then diffs empty and re-applies unchanged.
	bool Run(const FString& Piled, const FString& Unplaced, const FString& Relayout, TArray<FString> Expected) const
	{
		if (!Test.TestTrue(TEXT("piled graph: applies"), Apply(Document(TEXT("Create"), Piled), TEXT("piled graph")).IsSuccess()))
		{
			return false;
		}
		const TOptional<TArray<FString>> Kept = DiffPaths(Document(TEXT("Update"), Unplaced), TEXT("no positions"));
		Test.TestTrue(TEXT("no positions, no Layout: nothing to apply (the piled nodes stay)"), Kept.IsSet() && Kept->IsEmpty());
		const FString Auto = Document(TEXT("Update"), Relayout);
		const TOptional<TArray<FString>> Pending = DiffPaths(Auto, TEXT("relayout pending"));
		Expected.Sort();
		Test.TestEqual(TEXT("Diff reports the relayout of each graph at its Layout"), Pending.Get(TArray<FString>()), Expected);
		const FAssetDocumentResult Applied = Apply(Auto, TEXT("relayout"));
		Test.TestTrue(TEXT("the Layout-only document is applied and saved (not skipped as a no-op)"), Applied.IsSuccess() && Applied.bSavedAsset);
		const TOptional<TArray<FString>> After = DiffPaths(Auto, TEXT("after relayout"));
		Test.TestTrue(TEXT("Diff after the relayout is empty"), After.IsSet() && After->IsEmpty());
		const FAssetDocumentResult Again = Apply(Auto, TEXT("relayout again"));
		return Test.TestTrue(TEXT("the relayout applied again changes nothing"), Again.IsSuccess() && !Again.bSavedAsset);
	}
};

// Text with each {Key} replaced.
FString Fill(const TCHAR* Text, std::initializer_list<TPair<const TCHAR*, const TCHAR*>> Values)
{
	FString Out(Text);
	for (const TPair<const TCHAR*, const TCHAR*>& Value : Values)
	{
		Out.ReplaceInline(Value.Key, Value.Value, ESearchCase::CaseSensitive);
	}
	return Out;
}

const TCHAR* const At = TEXT(",\"Position\":{\"X\":2000,\"Y\":2000}");
const TCHAR* const Auto = TEXT(",\"Layout\":\"Auto\"");

// An event graph whose Print, Branch, Yes and No nodes (and a collapsed graph's Inner / Then nodes) carry {At}; {Top}
// / {Sub} are the event graph's / the collapsed graph's Layout.
const TCHAR* const CollapsedEventGraph = TEXT(R"JSON("UbergraphPages":[{"Name":"EventGraph"{Top},"Nodes":[
{"Id":"Print","Action":"K2:PrintString"{At}},
{"Id":"Branch","Action":"K2:Branch"{At}},
{"Id":"Yes","Action":"K2:PrintString"{At}},
{"Id":"No","Action":"K2:PrintString"{At}},
{"Id":"Setup","Action":"K2:Composite","Name":"Setup"}],
"Links":[{"From":"Print:then","To":"Branch:execute"},{"From":"Branch:then","To":"Yes:execute"},{"From":"Branch:else","To":"No:execute"},
{"From":"No:then","To":"Setup:execute"}],
"Subgraphs":[{"Name":"Setup","Owner":"Setup"{Sub},"Signature":{"Inputs":[{"Name":"execute","Type":"exec"}],"Outputs":[{"Name":"then","Type":"exec"}]},
"Nodes":[{"Id":"Inner","Action":"K2:PrintString"{At}},{"Id":"Then","Action":"K2:Branch"{At}}],
"Links":[{"From":"Entry:execute","To":"Inner:execute"},{"From":"Inner:then","To":"Then:execute"},{"From":"Then:then","To":"Result:then"}]}]}])JSON");
} // namespace AssetDocumentGraphSharedProfileTestsLocal

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGraphRelayoutDiffCollapsedTest,
	"UEAssetDoc.Blueprint.Layout.RelayoutDiffCollapsed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentGraphRelayoutDiffCollapsedTest::RunTest(const FString&)
{
	using namespace AssetDocumentGraphSharedProfileTestsLocal;
	const FRelayoutFixture Fixture(*this, TEXT("BP_RelayoutCollapsed"), TEXT("/Script/Engine.Blueprint"));
	const FString Parent = TEXT("\"ParentClass\":\"/Script/Engine.Actor\",");
	return Fixture.Run(Parent + Fill(CollapsedEventGraph, {{TEXT("{Top}"), TEXT("")}, {TEXT("{Sub}"), TEXT("")}, {TEXT("{At}"), At}}),
		Parent + Fill(CollapsedEventGraph, {{TEXT("{Top}"), TEXT("")}, {TEXT("{Sub}"), TEXT("")}, {TEXT("{At}"), TEXT("")}}),
		Parent + Fill(CollapsedEventGraph, {{TEXT("{Top}"), TEXT("")}, {TEXT("{Sub}"), Auto}, {TEXT("{At}"), TEXT("")}}),
		{TEXT("/Body/UbergraphPages/0/Subgraphs/0/Layout")});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGraphRelayoutDiffWidgetTest,
	"UEAssetDoc.WidgetBlueprint.Layout.RelayoutDiff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentGraphRelayoutDiffWidgetTest::RunTest(const FString&)
{
	using namespace AssetDocumentGraphSharedProfileTestsLocal;
	const FRelayoutFixture Fixture(*this, TEXT("WBP_Relayout"), TEXT("/Script/UMGEditor.WidgetBlueprint"));
	const FString Parent = TEXT("\"ParentClass\":\"/Script/UMG.UserWidget\",");
	return Fixture.Run(Parent + Fill(CollapsedEventGraph, {{TEXT("{Top}"), TEXT("")}, {TEXT("{Sub}"), TEXT("")}, {TEXT("{At}"), At}}),
		Parent + Fill(CollapsedEventGraph, {{TEXT("{Top}"), TEXT("")}, {TEXT("{Sub}"), TEXT("")}, {TEXT("{At}"), TEXT("")}}),
		Parent + Fill(CollapsedEventGraph, {{TEXT("{Top}"), Auto}, {TEXT("{Sub}"), Auto}, {TEXT("{At}"), TEXT("")}}),
		{TEXT("/Body/UbergraphPages/0/Layout"), TEXT("/Body/UbergraphPages/0/Subgraphs/0/Layout")});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGraphRelayoutDiffAnimTest,
	"UEAssetDoc.AnimBlueprint.Layout.RelayoutDiff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentGraphRelayoutDiffAnimTest::RunTest(const FString&)
{
	using namespace AssetDocumentGraphSharedProfileTestsLocal;
	const FRelayoutFixture Fixture(*this, TEXT("ABP_Relayout"), TEXT("/Script/Engine.AnimBlueprint"));
	// A template Anim Blueprint: a state machine whose states are piled ({Machine}: its graph's Layout), and an event
	// graph ({Top}).
	const TCHAR* const Body = TEXT(R"JSON("ParentClass":"/Script/Engine.AnimInstance","Template":true,
"AnimGraph":{"Nodes":[{"Id":"Machine","Action":"Anim:StateMachine","Graph":{"Nodes":[
{"Id":"Idle","Action":"SM:State"{At}},
{"Id":"Walk","Action":"SM:State"{At}},
{"Id":"Run","Action":"SM:State"{At}},
{"Id":"ToWalk","Action":"SM:Transition","From":"Idle","To":"Walk"},
{"Id":"ToRun","Action":"SM:Transition","From":"Walk","To":"Run"}],
"Links":[{"From":"Entry:Entry","To":"Idle:In"}]{Machine}}}],
"Links":[{"From":"Machine:Pose","To":"Output:Result"}]},
"UbergraphPages":[{"Name":"EventGraph"{Top},"Nodes":[
{"Id":"Print","Action":"K2:PrintString"{At}},
{"Id":"Branch","Action":"K2:Branch"{At}},
{"Id":"Yes","Action":"K2:PrintString"{At}}],
"Links":[{"From":"Print:then","To":"Branch:execute"},{"From":"Branch:then","To":"Yes:execute"}]}])JSON");
	return Fixture.Run(Fill(Body, {{TEXT("{Machine}"), TEXT("")}, {TEXT("{Top}"), TEXT("")}, {TEXT("{At}"), At}}),
		Fill(Body, {{TEXT("{Machine}"), TEXT("")}, {TEXT("{Top}"), TEXT("")}, {TEXT("{At}"), TEXT("")}}),
		Fill(Body, {{TEXT("{Machine}"), Auto}, {TEXT("{Top}"), Auto}, {TEXT("{At}"), TEXT("")}}),
		{TEXT("/Body/AnimGraph/Nodes/0/Graph/Layout"), TEXT("/Body/UbergraphPages/0/Layout")});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGraphRelayoutDiffDirectorTest,
	"UEAssetDoc.Sequencer.Director.RelayoutDiff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentGraphRelayoutDiffDirectorTest::RunTest(const FString&)
{
	using namespace AssetDocumentGraphSharedProfileTestsLocal;
	const FRelayoutFixture Fixture(*this, TEXT("LS_Relayout"), TEXT("/Script/LevelSequence.LevelSequence"));
	const TCHAR* const Body = TEXT(R"JSON("Director":{"FunctionGraphs":[{"Name":"Tidy"{Layout},"Nodes":[
{"Id":"Print","Action":"K2:PrintString"{At}},
{"Id":"Branch","Action":"K2:Branch"{At}},
{"Id":"Yes","Action":"K2:PrintString"{At}},
{"Id":"No","Action":"K2:PrintString"{At}}],
"Links":[{"From":"Entry:then","To":"Print:execute"},{"From":"Print:then","To":"Branch:execute"},
{"From":"Branch:then","To":"Yes:execute"},{"From":"Branch:else","To":"No:execute"}]}]})JSON");
	return Fixture.Run(Fill(Body, {{TEXT("{Layout}"), TEXT("")}, {TEXT("{At}"), At}}),
		Fill(Body, {{TEXT("{Layout}"), TEXT("")}, {TEXT("{At}"), TEXT("")}}),
		Fill(Body, {{TEXT("{Layout}"), Auto}, {TEXT("{At}"), TEXT("")}}),
		{TEXT("/Body/Director/FunctionGraphs/0/Layout")});
}

namespace AssetDocumentGraphSharedProfileTestsLocal
{
// An event graph with three independent mistakes: a pin the Branch does not have (Links/0), a key that names no action
// (Nodes/1) and a link the editor makes only through a conversion node (Links/1); ValidateGraph(true, ...) fixes them.
// {Go}: the Yes node's input pin (execute, or Go: not a pin); {Nodes} / {Links}: more nodes and links.
const TCHAR* const ValidateEventGraph = TEXT(R"JSON("UbergraphPages":[{"Name":"EventGraph",
 "Nodes":[{"Id":"B","Action":"K2:Branch"},{"Id":"Print","Action":"K2:CallFunction:/Script/Engine.KismetSystemLibrary:PrintStrin{Fix}"},
          {"Id":"Rand","Action":"K2:CallFunction:/Script/Engine.KismetMathLibrary:RandomInteger"},
          {"Id":"Yes","Action":"K2:PrintString"}{Nodes}],
 "Links":[{"From":"Print:then","To":"B:{Pin}"},{"From":"Rand:ReturnValue","To":"B:Condition"},
          {"From":"B:then","To":"Yes:{Go}"}{Links}]}])JSON");

FString ValidateGraph(bool bFixed, const TCHAR* Go, const TCHAR* Nodes = TEXT(""), const TCHAR* Links = TEXT(""))
{
	FString Out = Fill(ValidateEventGraph, {{TEXT("{Fix}"), bFixed ? TEXT("g") : TEXT("")}, {TEXT("{Pin}"), bFixed ? TEXT("execute") : TEXT("Bogus")},
		{TEXT("{Go}"), Go}, {TEXT("{Nodes}"), Nodes}, {TEXT("{Links}"), Links}});
	if (bFixed)
	{
		Out.ReplaceInline(TEXT("{\"From\":\"Rand:ReturnValue\",\"To\":\"B:Condition\"},"), TEXT(""), ESearchCase::CaseSensitive);
	}
	return Out;
}
} // namespace AssetDocumentGraphSharedProfileTestsLocal

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGraphSharedProfileWidgetValidateTest,
	"UEAssetDoc.WidgetBlueprint.Authoring.ValidateGraphs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentGraphSharedProfileWidgetValidateTest::RunTest(const FString&)
{
	using namespace AssetDocumentGraphSharedProfileTestsLocal;
	const FRelayoutFixture Fixture(*this, TEXT("WBP_Validate"), TEXT("/Script/UMGEditor.WidgetBlueprint"));
	// The fixed document also adds a button and binds its OnClicked (a key Validate leaves to Apply: the button is new).
	const FString Parent = TEXT("\"ParentClass\":\"/Script/UMG.UserWidget\",");
	const FString Tree = TEXT(R"JSON("WidgetTree":{"RootWidget":{"Name":"Root","Class":"/Script/UMG.CanvasPanel","IsVariable":false,
"Children":[{"Name":"OkButton","Class":"/Script/UMG.Button","IsVariable":true}]},"NamedSlotBindings":{}},)JSON");
	const TCHAR* const Click = TEXT(R"JSON(,{"Id":"Click","Action":"K2:ComponentBoundEvent:/Script/UMG.Button:OnClicked@OkButton"})JSON");
	const TCHAR* const ClickLink = TEXT(R"JSON(,{"From":"Click:then","To":"Print:execute"})JSON");
	Fixture.ValidateGraphs(Parent + ValidateGraph(false, TEXT("execute")), Parent + Tree + ValidateGraph(true, TEXT("execute"), Click, ClickLink),
		Parent + Tree + ValidateGraph(true, TEXT("Go"), Click, ClickLink));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGraphSharedProfileAnimValidateTest,
	"UEAssetDoc.AnimBlueprint.Authoring.ValidateGraphs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentGraphSharedProfileAnimValidateTest::RunTest(const FString&)
{
	using namespace AssetDocumentGraphSharedProfileTestsLocal;
	const FRelayoutFixture Fixture(*this, TEXT("ABP_Validate"), TEXT("/Script/Engine.AnimBlueprint"));
	const FString Parent = TEXT("\"ParentClass\":\"/Script/Engine.AnimInstance\",\"Template\":true,");
	Fixture.ValidateGraphs(Parent + ValidateGraph(false, TEXT("execute")), Parent + ValidateGraph(true, TEXT("execute")),
		Parent + ValidateGraph(true, TEXT("Go")));
	return !HasAnyErrors();
}
// An Actor Blueprint document that adds a component and binds one of its events
// (K2:ComponentBoundEvent:...@Trigger) validates: the Blueprint Validate reads (the new asset's trial Blueprint of
// the parent class, or the existing target) does not have the component yet, so the key is left to Apply, as for a
// Widget Blueprint's new widget. Also on the existing target, for a second component the document adds.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGraphSharedProfileBlueprintBoundEventValidateTest,
	"UEAssetDoc.Blueprint.Authoring.ValidateComponentBoundEvents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentGraphSharedProfileBlueprintBoundEventValidateTest::RunTest(const FString&)
{
	using namespace AssetDocumentGraphSharedProfileTestsLocal;
	const FRelayoutFixture Fixture(*this, TEXT("BP_ValidateBound"), TEXT("/Script/Engine.Blueprint"));
	const FString Parent = TEXT("\"ParentClass\":\"/Script/Engine.Actor\",");
	const FString Trigger = TEXT(R"JSON("Components":[{"Name":"Trigger","Class":"/Script/Engine.BoxComponent"}],)JSON");
	const TCHAR* const Overlap = TEXT(R"JSON(,{"Id":"Overlap","Action":"K2:ComponentBoundEvent:/Script/Engine.PrimitiveComponent:OnComponentBeginOverlap@Trigger"})JSON");
	const TCHAR* const OverlapLink = TEXT(R"JSON(,{"From":"Overlap:then","To":"Print:execute"})JSON");
	Fixture.ValidateGraphs(Parent + ValidateGraph(false, TEXT("execute")), Parent + Trigger + ValidateGraph(true, TEXT("execute"), Overlap, OverlapLink),
		Parent + Trigger + ValidateGraph(true, TEXT("Go"), Overlap, OverlapLink));
	if (!Fixture.Exists())
	{
		return !HasAnyErrors();
	}
	const FString Both = TEXT(R"JSON("Components":[{"Name":"Trigger","Class":"/Script/Engine.BoxComponent"},
{"Name":"Sensor","Class":"/Script/Engine.SphereComponent","AttachTo":"Trigger"}],)JSON");
	const TCHAR* const Hit = TEXT(R"JSON(,{"Id":"Overlap","Action":"K2:ComponentBoundEvent:/Script/Engine.PrimitiveComponent:OnComponentBeginOverlap@Trigger"},
{"Id":"Sensed","Action":"K2:ComponentBoundEvent:/Script/Engine.PrimitiveComponent:OnComponentEndOverlap@Sensor"})JSON");
	const FString Added = Fixture.Document(TEXT("Update"), Parent + Both + ValidateGraph(true, TEXT("execute"), Hit, OverlapLink));
	const FAssetDocumentResult Validated = Fixture.Validate(Added, TEXT("existing target, new component's bound event"));
	TestTrue(TEXT("a bound event of a component the document adds to the existing Blueprint validates"), Validated.IsSuccess());
	TestTrue(TEXT("and applies"), Fixture.Apply(Added, TEXT("add Sensor and its bound event")).IsSuccess());
	return !HasAnyErrors();
}
#endif // WITH_DEV_AUTOMATION_TESTS
