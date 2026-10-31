// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// An editor-made Environment Query with content the document has no form for: a project test whose Details hold a
// field path (UAssetDocumentEnvQueryFieldTest of the AssetDocumentTests module, found by path), a test and a
// generator whose classes no longer load (as a Blueprint test or generator deleted without a redirector loads: a node
// without instance that keeps its class data). Each reads as a marker in place with a warning, the extract re-applies
// as a no-op, another edit keeps them, and an edited marker is refused.

#include "AssetDocumentEnvQueryTestSupport.h"
#include "AssetDocumentTolerantTestSupport.h"

#include "AIGraphTypes.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentTolerantEnvQueryTestsLocal
{
const TCHAR* const FieldTestClass = TEXT("/Script/AssetDocumentTests.AssetDocumentEnvQueryFieldTest");

TSharedPtr<FJsonObject> TestEntryAt(const TSharedPtr<FJsonObject>& Document, int32 Option, int32 Test)
{
	return Document->GetObjectField(TEXT("Body"))->GetArrayField(TEXT("Options"))[Option]->AsObject()
		->GetArrayField(TEXT("Tests"))[Test]->AsObject();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantEnvQueryTest, "UEAssetDoc.Tolerant.EnvQuery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantEnvQueryTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentTolerantEnvQueryTestsLocal;
	namespace EQ = UE::AssetDocument::Graph::EnvQuery;
	const FTarget Target(TEXT("EQS_Tolerant"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	// The engine reports the test node whose class does not load on every save (UAIGraph::UpdateErrorMessages).
	AddExpectedErrorPlain(TEXT("EQT_Gone class missing"), EAutomationExpectedErrorFlags::Contains, 0);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	if (!TestNotNull(TEXT("the test EQS class is loaded"), FindObject<UClass>(nullptr, FieldTestClass)))
	{
		return false;
	}

	// Content Browser: Add > Artificial Intelligence > Environment Query.
	EQ::EnsureEditorModule();
	UClass* FactoryClass = FindObject<UClass>(nullptr, TEXT("/Script/EnvironmentQueryEditor.EnvironmentQueryFactory"));
	UFactory* Factory = FactoryClass != nullptr ? NewObject<UFactory>(GetTransientPackage(), FactoryClass) : nullptr;
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UEnvQuery* Query = Factory != nullptr ? Cast<UEnvQuery>(AssetTools.CreateAsset(Target.AssetName,
		FPackageName::GetLongPackagePath(Target.PackageName), UEnvQuery::StaticClass(), Factory)) : nullptr;
	if (!TestNotNull(TEXT("query created by the factory"), Query))
	{
		return false;
	}

	const AssetDocumentEnvQueryTests::FEditorQuery Editor(*Query);
	const FString Grid = Editor.AddOption(TEXT("SimpleGrid"), 0, 160);
	Editor.AddTest(Grid, TEXT("Distance"));
	const FString Field = Editor.AddTest(Grid, FieldTestClass);
	Editor.SetProperty(Field, TEXT("Field"), TEXT("/Script/Engine.Actor:Tags"));
	const FString Lost = Editor.AddTest(Grid, TEXT("Dot"));
	const FString Gone = Editor.AddOption(TEXT("Donut"), 600, 160, false);
	UAIGraphNode* LostNode = Editor.Test(Lost);
	UAIGraphNode* GoneNode = Editor.Option(Gone);
	UEnvQueryOption* GoneOption = GoneNode != nullptr ? EQ::GetOption(*GoneNode) : nullptr;
	if (!TestTrue(TEXT("graph built"), !Grid.IsEmpty() && !Field.IsEmpty() && LostNode != nullptr && GoneOption != nullptr))
	{
		return false;
	}
	// The classes no longer load: the nodes keep their class data, their instances are gone.
	LostNode->Modify();
	LostNode->NodeInstance = nullptr;
	LostNode->ClassData = FGraphNodeClassData(TEXT("EQT_Gone"), TEXT("/Game/AssetDocumentTolerant/EQT_Gone"), TEXT("EQT_Gone_C"), nullptr);
	GoneNode->Modify();
	GoneOption->Modify();
	GoneOption->Generator = nullptr;
	GoneNode->ClassData = FGraphNodeClassData(TEXT("EQG_Gone"), TEXT("/Game/AssetDocumentTolerant/EQG_Gone"), TEXT("EQG_Gone_C"), nullptr);
	if (!Editor.SaveQuery(*this, TEXT("EQS_Tolerant")))
	{
		return false;
	}

	const FString FieldMarker = TEXT("/Body/Options/0/Tests/1/Properties/Field");
	const FString LostMarker = TEXT("/Body/Options/0/Tests/2");
	const FString GoneMarker = TEXT("/Body/Options/1");
	FExpected Expected;
	Expected.Warnings = {{TEXT("EnvQuery.Unrepresented"), FieldMarker}, {TEXT("EnvQuery.Unrepresented"), LostMarker},
		{TEXT("EnvQuery.Unrepresented"), GoneMarker}};
	Expected.Markers = {FieldMarker, LostMarker, GoneMarker};
	if (!TestTolerantRoundTrip(*this, Target, Expected, TEXT("Environment Query")).IsValid())
	{
		return false;
	}

	if (TestTrue(TEXT("another test changes"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		TestEntryAt(Document, 0, 0)->SetBoolField(TEXT("Enabled"), false);
		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		if (TestEntryAt(Document, 0, 1)->HasField(TEXT("Properties")))
		{
			Properties = TestEntryAt(Document, 0, 1)->GetObjectField(TEXT("Properties"));
		}
		Properties->SetNumberField(TEXT("Bias"), 2.0);
		TestEntryAt(Document, 0, 1)->SetObjectField(TEXT("Properties"), Properties);
	}, TEXT("Edit")).IsSuccess()))
	{
		UEnvQuery* Edited = Target.Get<UEnvQuery>();
		const AssetDocumentEnvQueryTests::FEditorQuery After(*Edited);
		const UAIGraphNode* FieldNode = After.Test(Field);
		const UObject* FieldTest = FieldNode != nullptr ? FieldNode->NodeInstance.Get() : nullptr;
		const FProperty* FieldProperty = FieldTest != nullptr ? FieldTest->GetClass()->FindPropertyByName(TEXT("Field")) : nullptr;
		FString Kept;
		if (FieldProperty != nullptr)
		{
			FieldProperty->ExportText_InContainer(0, Kept, FieldTest, nullptr, nullptr, PPF_None);
		}
		TestTrue(TEXT("the field path kept"), Kept.Contains(TEXT("Tags")));
		const UAIGraphNode* GridNode = After.Option(Grid);
		TestTrue(TEXT("the test without class kept"), GridNode != nullptr && GridNode->SubNodes.Num() == 3
			&& GridNode->SubNodes[2] != nullptr && GridNode->SubNodes[2]->NodeInstance == nullptr);
		const UAIGraphNode* GoneAfter = After.Option(Gone);
		TestTrue(TEXT("the option without generator kept"), GoneAfter != nullptr && EQ::GetOption(*GoneAfter) != nullptr
			&& EQ::GetOption(*GoneAfter)->Generator == nullptr);
		TestStillShows(*this, Target, TEXT("EnvQuery.Unrepresented"), FieldMarker, TEXT("After edit"));
		TestStillShows(*this, Target, TEXT("EnvQuery.Unrepresented"), LostMarker, TEXT("After edit"));
	}

	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		TestEntryAt(Document, 0, 1)->GetObjectField(TEXT("Properties"))->GetObjectField(TEXT("Field"))->SetStringField(TEXT("Text"), TEXT("None"));
	}, TEXT("Edited marker")), TEXT("EnvQuery.UnrepresentedValue"), FieldMarker, TEXT("Edited marker"));
	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		TestEntryAt(Document, 0, 2)->SetStringField(TEXT("Class"), TEXT("Dot"));
	}, TEXT("Edited marked test")), TEXT("EnvQuery.UnrepresentedValue"), LostMarker, TEXT("Edited marked test"));
	return !HasAnyErrors();
}

#endif
