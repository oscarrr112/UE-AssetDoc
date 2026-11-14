// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentGenericInspectContractTestTypes.h"

#include "Dom/JsonValue.h"
#include "Internationalization/StringTableRegistry.h"
#include "Misc/AutomationTest.h"
#include "Utils/PropertySetterUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
const TCHAR* TextMacroTestTableId = TEXT("UEAssetDocTest.TextMacroTable");

struct FTextMacroTestTable
{
	FTextMacroTestTable()
	{
		FStringTableRegistry::Get().Internal_NewLocTable(TextMacroTestTableId, TEXT("UEAssetDocTest"));
		FStringTableRegistry::Get().Internal_SetLocTableEntry(TextMacroTestTableId, TEXT("Greeting"), TEXT("Hello {Name}"));
	}

	~FTextMacroTestTable()
	{
		FStringTableRegistry::Get().UnregisterStringTable(TextMacroTestTableId);
	}
};

FTextProperty* FindDisplayTextProperty()
{
	return FindFProperty<FTextProperty>(UAssetDocumentGenericInspectContractTestDataAsset::StaticClass(), TEXT("DisplayText"));
}

bool SetDisplayText(UAssetDocumentGenericInspectContractTestDataAsset* Asset, const FString& Value)
{
	return FPropertySetterUtils::SetPropertyFromJson(Asset, FindDisplayTextProperty(), MakeShared<FJsonValueString>(Value));
}

FString ExtractDisplayText(UAssetDocumentGenericInspectContractTestDataAsset* Asset)
{
	FTextProperty* Property = FindDisplayTextProperty();
	TSharedPtr<FJsonValue> Json = FPropertySetterUtils::ExtractPropertyToJson(Property, Property->ContainerPtrToValuePtr<void>(Asset));
	return Json.IsValid() ? Json->AsString() : FString(TEXT("<invalid>"));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentTextMacroPropertyTest,
	"UEAssetDoc.Generic.Property.TextMacrosParseAndRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTextMacroPropertyTest::RunTest(const FString& Parameters)
{
	FTextMacroTestTable Table;
	UAssetDocumentGenericInspectContractTestDataAsset* Asset = NewObject<UAssetDocumentGenericInspectContractTestDataAsset>(GetTransientPackage());
	if (!TestNotNull(TEXT("DisplayText property"), FindDisplayTextProperty()))
	{
		return false;
	}

	// A string table macro becomes a reference to that entry.
	const FString TableMacro = FString::Printf(TEXT("LOCTABLE(\"%s\", \"Greeting\")"), TextMacroTestTableId);
	TestTrue(TEXT("LOCTABLE applies"), SetDisplayText(Asset, TableMacro));
	TestTrue(TEXT("LOCTABLE is a table reference"), Asset->DisplayText.IsFromStringTable());
	FName TableId;
	FString Key;
	FTextInspector::GetTableIdAndKey(Asset->DisplayText, TableId, Key);
	TestEqual(TEXT("LOCTABLE table"), TableId, FName(TextMacroTestTableId));
	TestEqual(TEXT("LOCTABLE key"), Key, FString(TEXT("Greeting")));
	TestEqual(TEXT("LOCTABLE display"), Asset->DisplayText.ToString(), FString(TEXT("Hello {Name}")));
	const FString ExtractedTable = ExtractDisplayText(Asset);
	TestTrue(TEXT("LOCTABLE extracts as LOCTABLE"), ExtractedTable.StartsWith(TEXT("LOCTABLE(")));
	TestTrue(TEXT("Extracted LOCTABLE re-applies"), SetDisplayText(Asset, ExtractedTable));
	TestTrue(TEXT("Re-applied LOCTABLE is a table reference"), Asset->DisplayText.IsFromStringTable());

	// NSLOCTEXT keeps namespace and key.
	TestTrue(TEXT("NSLOCTEXT applies"), SetDisplayText(Asset, TEXT("NSLOCTEXT(\"UEAssetDocTest\", \"Hi\", \"Hi there\")")));
	TestEqual(TEXT("NSLOCTEXT display"), Asset->DisplayText.ToString(), FString(TEXT("Hi there")));
	TestEqual(TEXT("NSLOCTEXT key"), FTextInspector::GetKey(Asset->DisplayText).Get(FString()), FString(TEXT("Hi")));
	TestEqual(TEXT("NSLOCTEXT extracts as its source string"), ExtractDisplayText(Asset), FString(TEXT("Hi there")));

	// INVTEXT is culture invariant.
	TestTrue(TEXT("INVTEXT applies"), SetDisplayText(Asset, TEXT("INVTEXT(\"42\")")));
	TestTrue(TEXT("INVTEXT is culture invariant"), Asset->DisplayText.IsCultureInvariant());
	TestEqual(TEXT("INVTEXT extracts as INVTEXT"), ExtractDisplayText(Asset), FString(TEXT("INVTEXT(\"42\")")));

	// A plain string stays plain source text.
	TestTrue(TEXT("Plain applies"), SetDisplayText(Asset, TEXT("Just words")));
	TestFalse(TEXT("Plain is not a table reference"), Asset->DisplayText.IsFromStringTable());
	TestEqual(TEXT("Plain extracts as itself"), ExtractDisplayText(Asset), FString(TEXT("Just words")));

	// A macro that does not parse is rejected rather than stored as its source string.
	TestFalse(TEXT("Broken macro is rejected"), SetDisplayText(Asset, TEXT("LOCTABLE(\"Missing")));
	TestFalse(TEXT("Macro with trailing text is rejected"), SetDisplayText(Asset, TEXT("INVTEXT(\"42\") and more")));
	TestEqual(TEXT("Rejected macros leave the value alone"), Asset->DisplayText.ToString(), FString(TEXT("Just words")));

	// A literal text whose string reads as a macro comes back as INVTEXT, so re-applying keeps the literal.
	Asset->DisplayText = FText::FromString(TableMacro);
	const FString ExtractedLiteral = ExtractDisplayText(Asset);
	TestTrue(TEXT("Macro-looking literal extracts as INVTEXT"), ExtractedLiteral.StartsWith(TEXT("INVTEXT(")));
	TestTrue(TEXT("Literal re-applies"), SetDisplayText(Asset, ExtractedLiteral));
	TestFalse(TEXT("Re-applied literal is not a table reference"), Asset->DisplayText.IsFromStringTable());
	TestEqual(TEXT("Re-applied literal keeps its string"), Asset->DisplayText.ToString(), TableMacro);
	return true;
}
#endif
