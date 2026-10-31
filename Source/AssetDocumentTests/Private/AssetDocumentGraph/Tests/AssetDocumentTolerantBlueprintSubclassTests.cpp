// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Editor-made assets of Blueprint subclasses that have no profile of their own: an Editor Utility Blueprint is read and
// edited by the Blueprint profile, an Editor Utility Widget Blueprint by the Widget Blueprint profile. The document
// keeps the asset's own class, its extract re-applies as a no-op, an edit keeps the asset's class, and Create of such a
// class is refused with a diagnostic that says how to make one. A Blueprint class no profile reads is refused. Saved
// properties a subclass adds that the document has no place for are named in a read warning and kept by an apply; a
// subclass that keeps graphs of its own is read but not applied to.

#include "AssetDocumentTolerantTestSupport.h"
#include "AssetDocumentBlueprintSubclassTestTypes.h"

#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "EdGraphSchema_K2.h"
#include "EditorUtilityBlueprint.h"
#include "EditorUtilityObject.h"
#include "EditorUtilityWidget.h"
#include "EditorUtilityWidgetBlueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentTolerantBlueprintSubclassTestsLocal
{
using namespace AssetDocumentTolerantTestSupport;

bool HasMemberVariable(const UBlueprint* Blueprint, const TCHAR* Name)
{
	return Blueprint != nullptr && Blueprint->NewVariables.ContainsByPredicate([Name](const FBPVariableDescription& Variable)
	{
		return Variable.VarName == FName(Name);
	});
}

// The variable edit of the subclass tests: adds Speed (float, 2.5) to Variables.
void AddSpeedVariable(const TSharedPtr<FJsonObject>& Document)
{
	const TSharedPtr<FJsonObject> Type = MakeShared<FJsonObject>();
	Type->SetStringField(TEXT("PinCategory"), TEXT("real"));
	Type->SetStringField(TEXT("PinSubCategory"), TEXT("float"));
	const TSharedPtr<FJsonObject> Variable = MakeShared<FJsonObject>();
	Variable->SetStringField(TEXT("Name"), TEXT("Speed"));
	Variable->SetObjectField(TEXT("Type"), Type);
	Variable->SetStringField(TEXT("DefaultValue"), TEXT("2.5"));
	TArray<TSharedPtr<FJsonValue>> Variables = Document->GetObjectField(TEXT("Body"))->GetArrayField(TEXT("Variables"));
	Variables.Add(MakeShared<FJsonValueObject>(Variable));
	Document->GetObjectField(TEXT("Body"))->SetArrayField(TEXT("Variables"), Variables);
}

// A read warning at /Class whose message names Name.
bool WarnsAbout(const FAssetDocumentResult& Result, const FString& Code, const TCHAR* Name)
{
	return Result.Diagnostics.ContainsByPredicate([&](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == Code && Diagnostic.Path == TEXT("/Class") && Diagnostic.Severity == TEXT("warning")
			&& Diagnostic.Message.Contains(Name);
	});
}

// Applies Extracted as a Create of a new asset at NewTarget and expects Code at /Class; nothing is created.
void TestCreateRefused(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Extracted, const FTarget& NewTarget,
	const FString& Code)
{
	const TSharedPtr<FJsonObject> Document = Parse(Serialize(Extracted));
	Document->SetStringField(TEXT("Target"), NewTarget.PackageName);
	Document->SetStringField(TEXT("Action"), TEXT("Create"));
	TestRefused(Test, ApplyText(Test, NewTarget, Serialize(Document), TEXT("Create")), Code, TEXT("/Class"), TEXT("Create"));
	Test.TestNull(TEXT("Create: no asset made"), NewTarget.Get());
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantEditorUtilityBlueprintTest,
	"UEAssetDoc.Tolerant.BlueprintSubclass.EditorUtilityBlueprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantEditorUtilityBlueprintTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantBlueprintSubclassTestsLocal;
	const FTarget Target(TEXT("EUB_Tool"));
	const FTarget NewTarget(TEXT("EUB_New"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); NewTarget.Cleanup(); };

	// Content Browser > Editor Utilities > Editor Utility Blueprint, with one variable.
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(UEditorUtilityObject::StaticClass(),
		CreatePackage(*Target.PackageName), FName(*Target.AssetName), BPTYPE_Normal, UEditorUtilityBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	FAssetRegistryModule::AssetCreated(Blueprint);
	const FEdGraphPinType IntType(UEdGraphSchema_K2::PC_Int, NAME_None, nullptr, EPinContainerType::None, false, FEdGraphTerminalType());
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Count"), IntType);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Editor Utility Blueprint"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("the document keeps the asset's class"), Extracted->GetStringField(TEXT("Class")),
		UEditorUtilityBlueprint::StaticClass()->GetPathName());
	TestEqual(TEXT("the parent class is read"), StringAt(Extracted, TEXT("/Body/ParentClass")),
		UEditorUtilityObject::StaticClass()->GetPathName());

	if (TestTrue(TEXT("a new variable applies"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Type = MakeShared<FJsonObject>();
		Type->SetStringField(TEXT("PinCategory"), TEXT("real"));
		Type->SetStringField(TEXT("PinSubCategory"), TEXT("float"));
		const TSharedPtr<FJsonObject> Variable = MakeShared<FJsonObject>();
		Variable->SetStringField(TEXT("Name"), TEXT("Speed"));
		Variable->SetObjectField(TEXT("Type"), Type);
		Variable->SetStringField(TEXT("DefaultValue"), TEXT("2.5"));
		TArray<TSharedPtr<FJsonValue>> Variables = Document->GetObjectField(TEXT("Body"))->GetArrayField(TEXT("Variables"));
		Variables.Add(MakeShared<FJsonValueObject>(Variable));
		Document->GetObjectField(TEXT("Body"))->SetArrayField(TEXT("Variables"), Variables);
	}, TEXT("Edit")).IsSuccess()))
	{
		Blueprint = Target.Get<UBlueprint>();
		TestTrue(TEXT("still an Editor Utility Blueprint"), Blueprint != nullptr && Blueprint->GetClass() == UEditorUtilityBlueprint::StaticClass());
		TestTrue(TEXT("both variables"), HasMemberVariable(Blueprint, TEXT("Count")) && HasMemberVariable(Blueprint, TEXT("Speed")));
	}

	TestCreateRefused(*this, Extracted, NewTarget, TEXT("UBlueprint.SubclassCreateUnsupported"));

	// Inspect of the class is answered by the profile that reads it, as for an ordinary Blueprint's class; a Blueprint
	// class no profile reads is refused.
	const auto InspectClass = [](const FString& ClassPath)
	{
		FAssetDocumentInspectRequest ClassInspect;
		ClassInspect.ClassOrAsset = ClassPath;
		return FAssetDocumentModule::Get().GetService().Inspect(ClassInspect);
	};
	const auto Codes = [](const FAssetDocumentResult& Result)
	{
		TArray<FString> Out;
		for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics) Out.Add(Diagnostic.Code);
		return FString::Join(Out, TEXT(","));
	};
	const FAssetDocumentResult Inspected = InspectClass(UEditorUtilityBlueprint::StaticClass()->GetPathName());
	const FAssetDocumentResult Ordinary = InspectClass(UBlueprint::StaticClass()->GetPathName());
	TestEqual(TEXT("class inspect answers as for Blueprint"), Inspected.IsSuccess(), Ordinary.IsSuccess());
	TestEqual(TEXT("class inspect diagnostics match Blueprint's"), Codes(Inspected), Codes(Ordinary));
	FAssetDocumentInspectRequest LevelScriptInspect;
	LevelScriptInspect.ClassOrAsset = TEXT("/Script/Engine.LevelScriptBlueprint");
	TestRefused(*this, FAssetDocumentModule::Get().GetService().Inspect(LevelScriptInspect), TEXT("ClassResolve.ProfileRequired"),
		FString(), TEXT("Level Script Blueprint"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantEditorUtilityWidgetBlueprintTest,
	"UEAssetDoc.Tolerant.BlueprintSubclass.EditorUtilityWidgetBlueprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantEditorUtilityWidgetBlueprintTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantBlueprintSubclassTestsLocal;
	const FTarget Target(TEXT("EUW_Tool"));
	const FTarget NewTarget(TEXT("EUW_New"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); NewTarget.Cleanup(); };

	// Content Browser > Editor Utilities > Editor Utility Widget, with a canvas root.
	UWidgetBlueprint* Blueprint = CastChecked<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
		UEditorUtilityWidget::StaticClass(), CreatePackage(*Target.PackageName), FName(*Target.AssetName), BPTYPE_Normal,
		UEditorUtilityWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass()));
	FAssetRegistryModule::AssetCreated(Blueprint);
	Blueprint->WidgetTree->RootWidget = Blueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Editor Utility Widget"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("the document keeps the asset's class"), Extracted->GetStringField(TEXT("Class")),
		UEditorUtilityWidgetBlueprint::StaticClass()->GetPathName());

	if (TestTrue(TEXT("an edit applies"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		Document->GetObjectField(TEXT("Body"))->SetStringField(TEXT("PaletteCategory"), TEXT("Tools"));
	}, TEXT("Edit")).IsSuccess()))
	{
		Blueprint = Target.Get<UWidgetBlueprint>();
		TestTrue(TEXT("still an Editor Utility Widget Blueprint"),
			Blueprint != nullptr && Blueprint->GetClass() == UEditorUtilityWidgetBlueprint::StaticClass());
		TestTrue(TEXT("the widget tree is kept"), Blueprint != nullptr && Blueprint->WidgetTree->FindWidget(TEXT("Root")) != nullptr);
		const FAssetDocumentResult Reread = ExtractResult(Target);
		TestEqual(TEXT("the palette category is set"),
			Reread.IsSuccess() ? StringAt(Reread.Payload, TEXT("/Body/PaletteCategory")) : FString(), FString(TEXT("Tools")));
	}

	TestCreateRefused(*this, Extracted, NewTarget, TEXT("Widget.SubclassCreateUnsupported"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantStatefulBlueprintSubclassTest,
	"UEAssetDoc.Tolerant.BlueprintSubclass.UnrepresentedProperties",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantStatefulBlueprintSubclassTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantBlueprintSubclassTestsLocal;
	const FTarget Target(TEXT("BP_Stateful"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	// A plugin's Blueprint asset class with saved values of its own, and one variable.
	UAssetDocumentTestStatefulBlueprint* Blueprint = CastChecked<UAssetDocumentTestStatefulBlueprint>(
		FKismetEditorUtilities::CreateBlueprint(UObject::StaticClass(), CreatePackage(*Target.PackageName),
			FName(*Target.AssetName), BPTYPE_Normal, UAssetDocumentTestStatefulBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass()));
	FAssetRegistryModule::AssetCreated(Blueprint);
	Blueprint->SavedCount = 7;
	Blueprint->SavedLabel = TEXT("kept");
	const FEdGraphPinType IntType(UEdGraphSchema_K2::PC_Int, NAME_None, nullptr, EPinContainerType::None, false, FEdGraphTerminalType());
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Count"), IntType);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	FExpected Expected;
	Expected.Warnings = {{TEXT("UBlueprint.Unrepresented"), TEXT("/Class")}};
	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, Expected, TEXT("Stateful Blueprint"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("the document keeps the asset's class"), Extracted->GetStringField(TEXT("Class")),
		UAssetDocumentTestStatefulBlueprint::StaticClass()->GetPathName());
	const FAssetDocumentResult Read = ExtractResult(Target);
	TestTrue(TEXT("the warning names SavedCount"), WarnsAbout(Read, TEXT("UBlueprint.Unrepresented"), TEXT("SavedCount")));
	TestTrue(TEXT("the warning names SavedLabel"), WarnsAbout(Read, TEXT("UBlueprint.Unrepresented"), TEXT("SavedLabel")));
	TestFalse(TEXT("a transient property is not named"), WarnsAbout(Read, TEXT("UBlueprint.Unrepresented"), TEXT("Scratch")));

	if (TestTrue(TEXT("a new variable applies"), ApplyEdited(*this, Target, &AddSpeedVariable, TEXT("Edit")).IsSuccess()))
	{
		const UAssetDocumentTestStatefulBlueprint* Edited = Target.Get<UAssetDocumentTestStatefulBlueprint>();
		TestTrue(TEXT("both variables"), HasMemberVariable(Edited, TEXT("Count")) && HasMemberVariable(Edited, TEXT("Speed")));
		TestTrue(TEXT("the saved values are kept"),
			Edited != nullptr && Edited->SavedCount == 7 && Edited->SavedLabel == TEXT("kept"));
		TestStillShows(*this, Target, TEXT("UBlueprint.Unrepresented"), TEXT("/Class"), TEXT("After edit"));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantOwnGraphsBlueprintSubclassTest,
	"UEAssetDoc.Tolerant.BlueprintSubclass.OwnGraphs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantOwnGraphsBlueprintSubclassTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantBlueprintSubclassTestsLocal;
	const FTarget Target(TEXT("BP_OwnGraphs"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	// A plugin's Blueprint asset class that keeps a graph of its own beside the K2 graphs.
	UAssetDocumentTestOwnGraphsBlueprint* Blueprint = CastChecked<UAssetDocumentTestOwnGraphsBlueprint>(
		FKismetEditorUtilities::CreateBlueprint(UObject::StaticClass(), CreatePackage(*Target.PackageName),
			FName(*Target.AssetName), BPTYPE_Normal, UAssetDocumentTestOwnGraphsBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass()));
	FAssetRegistryModule::AssetCreated(Blueprint);
	Blueprint->StateGraphs.Add(NewObject<UEdGraph>(Blueprint, TEXT("StateGraph"), RF_Transactional));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const FAssetDocumentResult Read = ExtractResult(Target);
	if (!TestTrue(TEXT("extract succeeds"), Read.IsSuccess() && Read.Payload.IsValid()))
	{
		LogResult(*this, TEXT("Extract"), Read);
		return false;
	}
	TestTrue(TEXT("the warning names the graphs"), WarnsAbout(Read, TEXT("UBlueprint.Unrepresented"), TEXT("StateGraphs")));
	TestRefused(*this, ApplyEdited(*this, Target, &AddSpeedVariable, TEXT("Edit")),
		TEXT("UBlueprint.SubclassGraphsUnsupported"), TEXT("/Class"), TEXT("Edit"));
	const UAssetDocumentTestOwnGraphsBlueprint* After = Target.Get<UAssetDocumentTestOwnGraphsBlueprint>();
	TestTrue(TEXT("the refused edit changes nothing"), After != nullptr && !HasMemberVariable(After, TEXT("Speed"))
		&& After->StateGraphs.Num() == 1 && After->StateGraphs[0] != nullptr);
	return !HasAnyErrors();
}

#endif
