// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// An editor-made Widget Blueprint that breaks rules written for new input (a design size of 0, a property bound
// twice, a property binding with no source path): it reads with warnings, its own extract re-applies as a no-op,
// other edits work, and the rules still refuse new input.

#include "AssetDocumentTolerantTestSupport.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/TextBlock.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ScopeExit.h"
#include "WidgetBlueprint.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantWidgetBlueprintTest, "UEAssetDoc.Tolerant.WidgetBlueprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantWidgetBlueprintTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	const FTarget Target(TEXT("WBP_Hud"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	UWidgetBlueprint* Blueprint = CastChecked<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(UUserWidget::StaticClass(),
		CreatePackage(*Target.PackageName), FName(*Target.AssetName), BPTYPE_Normal, UWidgetBlueprint::StaticClass(),
		UWidgetBlueprintGeneratedClass::StaticClass()));
	FAssetRegistryModule::AssetCreated(Blueprint);
	UCanvasPanel* Root = Blueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
	Blueprint->WidgetTree->RootWidget = Root;
	UTextBlock* Label = Blueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Label"));
	Root->AddChild(Label);
	const FEdGraphPinType TextType(UEdGraphSchema_K2::PC_Text, NAME_None, nullptr, EPinContainerType::None, false, FEdGraphTerminalType());
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Title"), TextType);
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Subtitle"), TextType);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	// A hand-merged asset: Label.Text bound twice, and a property binding of ToolTipText that lost its source path.
	auto Bind = [Blueprint](const TCHAR* Property, const TCHAR* Source)
	{
		FDelegateEditorBinding Binding;
		Binding.ObjectName = TEXT("Label");
		Binding.PropertyName = Property;
		Binding.Kind = EBindingKind::Property;
		if (Source != nullptr)
		{
			Binding.SourcePath = FEditorPropertyPath(TArray<FFieldVariant>{
				FFieldVariant(FindFProperty<FProperty>(Blueprint->SkeletonGeneratedClass, Source))});
		}
		Blueprint->Bindings.Add(Binding);
	};
	Bind(TEXT("Text"), TEXT("Title"));
	Bind(TEXT("Text"), TEXT("Subtitle"));
	Bind(TEXT("ToolTipText"), nullptr);
	// The Designer's custom size box accepts 0.
	UUserWidget* Defaults = Blueprint->GeneratedClass->GetDefaultObject<UUserWidget>();
	Defaults->DesignSizeMode = EDesignPreviewSizeMode::Custom;
	Defaults->DesignTimeSize = FVector2D(0, 720);
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	FExpected Expected;
	Expected.Warnings = {
		{TEXT("Widget.InvalidValue"), TEXT("/Body/DesignSize/Width")},
		{TEXT("Widget.DuplicateBinding"), TEXT("/Body/Bindings/1")},
		{TEXT("Widget.InvalidBinding"), TEXT("/Body/Bindings/2/SourcePath")}};
	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, Expected, TEXT("Hand-merged widget"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("both bindings of Label.Text read"), Extracted->GetObjectField(TEXT("Body"))->GetArrayField(TEXT("Bindings")).Num(), 3);

	if (TestTrue(TEXT("another edit applies"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		Document->GetObjectField(TEXT("Body"))->SetStringField(TEXT("PaletteCategory"), TEXT("Hud"));
	}, TEXT("Edit")).IsSuccess()))
	{
		Blueprint = Target.Get<UWidgetBlueprint>();
		TestTrue(TEXT("bindings kept"), Blueprint != nullptr && Blueprint->Bindings.Num() == 3);
		TestStillShows(*this, Target, TEXT("Widget.DuplicateBinding"), TEXT("/Body/Bindings/1"), TEXT("After edit"));
		TestStillShows(*this, Target, TEXT("Widget.InvalidValue"), TEXT("/Body/DesignSize/Width"), TEXT("After edit"));
	}

	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		Document->GetObjectField(TEXT("Body"))->GetObjectField(TEXT("DesignSize"))->SetNumberField(TEXT("Height"), 0);
	}, TEXT("New zero height")), TEXT("Widget.InvalidValue"), TEXT("/Body/DesignSize/Height"), TEXT("New zero height"));
	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		TArray<TSharedPtr<FJsonValue>> Rows = Document->GetObjectField(TEXT("Body"))->GetArrayField(TEXT("Bindings"));
		const TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Widget"), TEXT("Label"));
		Row->SetStringField(TEXT("Property"), TEXT("ToolTipText"));
		Row->SetStringField(TEXT("Kind"), TEXT("Property"));
		Row->SetArrayField(TEXT("SourcePath"), {MakeShared<FJsonValueString>(TEXT("Title"))});
		Rows.Add(MakeShared<FJsonValueObject>(Row));
		Document->GetObjectField(TEXT("Body"))->SetArrayField(TEXT("Bindings"), Rows);
	}, TEXT("New duplicate binding")), TEXT("Widget.DuplicateBinding"), TEXT("/Body/Bindings/3"), TEXT("New duplicate binding"));
	return !HasAnyErrors();
}

#endif
