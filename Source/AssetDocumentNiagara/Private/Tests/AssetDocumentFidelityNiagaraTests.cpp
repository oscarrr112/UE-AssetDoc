// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// UEAssetDoc.Fidelity.Niagara.ParameterMeta: a user parameter given a 0 to 10 slider, a display unit and advanced
// display in its Details, and a declared parameter given advanced display and an alias, read as their Meta. The
// extract re-applies as a no-op, a new system created from it holds the same metadata, an edited Meta is complete
// (listed values written, unlisted ones reset) and an omitted Meta keeps the metadata.

#include "AssetDocumentNiagaraPrimitives.h"
#include "AssetDocumentTolerantTestSupport.h"

#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemEditorData.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentFidelityNiagaraTestsLocal
{
using namespace AssetDocumentTolerantTestSupport;

const TCHAR* const MetaSourceSystem = TEXT("/Niagara/DefaultAssets/Templates/Systems/DirectionalBurst.DirectionalBurst");

TSharedPtr<FJsonObject> ParameterNamed(const TSharedPtr<FJsonObject>& Document, const TCHAR* List, const TCHAR* Name)
{
	const TSharedPtr<FJsonObject>* Body = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
	if (!Document.IsValid() || !Document->TryGetObjectField(TEXT("Body"), Body) || !(*Body)->TryGetArrayField(List, Entries))
	{
		return nullptr;
	}
	for (const TSharedPtr<FJsonValue>& Entry : *Entries)
	{
		const TSharedPtr<FJsonObject> Object = Entry.IsValid() ? Entry->AsObject() : nullptr;
		if (Object.IsValid() && Object->GetStringField(TEXT("Name")) == Name)
		{
			return Object;
		}
	}
	return nullptr;
}

FString MetaText(const TSharedPtr<FJsonObject>& Parameter)
{
	const TSharedPtr<FJsonObject>* Meta = nullptr;
	return Parameter.IsValid() && Parameter->TryGetObjectField(TEXT("Meta"), Meta) ? Serialize(*Meta) : FString();
}

const UNiagaraScriptVariable* UserVariable(const FTarget& Target, const TCHAR* Name)
{
	UNiagaraSystem* System = Target.Get<UNiagaraSystem>();
	UNiagaraSystemEditorData* EditorData = System != nullptr ? Cast<UNiagaraSystemEditorData>(System->GetEditorData()) : nullptr;
	return EditorData != nullptr
		? EditorData->FindOrAddUserScriptVariable(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), Name), *System) : nullptr;
}

UNiagaraScriptVariable* DeclaredVariable(const FTarget& Target, const TCHAR* Name)
{
	UNiagaraSystem* System = Target.Get<UNiagaraSystem>();
	if (System == nullptr)
	{
		return nullptr;
	}
	for (const UNiagaraScriptVariable* Variable : UE::AssetDocument::Niagara::ReadDeclaredParameters(*System))
	{
		if (Variable->Variable.GetName() == Name)
		{
			return const_cast<UNiagaraScriptVariable*>(Variable);
		}
	}
	return nullptr;
}

void PostEditMetadata(UNiagaraScriptVariable& Variable)
{
	FPropertyChangedEvent Event(UNiagaraScriptVariable::StaticClass()->FindPropertyByName(TEXT("Metadata")), EPropertyChangeType::ValueSet);
	Variable.PostEditChangeProperty(Event);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelityNiagaraParameterMetaTest, "UEAssetDoc.Fidelity.Niagara.ParameterMeta",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentFidelityNiagaraParameterMetaTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentFidelityNiagaraTestsLocal;
	const FTarget Target(TEXT("NS_FidelityMeta"));
	const FTarget Created(TEXT("NS_FidelityMetaCreated"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Created.Cleanup(); Target.Cleanup(); };

	UObject* Source = LoadObject<UObject>(nullptr, MetaSourceSystem);
	if (!TestNotNull(TEXT("template system loads"), Source))
	{
		return false;
	}
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UNiagaraSystem* System = Cast<UNiagaraSystem>(AssetTools.DuplicateAsset(Target.AssetName,
		FPackageName::GetLongPackagePath(Target.PackageName), Source));
	UNiagaraSystemEditorData* EditorData = System != nullptr ? Cast<UNiagaraSystemEditorData>(System->GetEditorData()) : nullptr;
	if (!TestNotNull(TEXT("duplicate"), EditorData))
	{
		return false;
	}
	System->WaitForCompilationComplete(false, false);

	// The User Parameters panel: "+" float User.Intensity, then its Details: a 0 to 10 slider in meters, advanced.
	FNiagaraVariable Intensity(FNiagaraTypeDefinition::GetFloatDef(), TEXT("User.Intensity"));
	FNiagaraEditorUtilities::ResetVariableToDefaultValue(Intensity);
	System->Modify();
	System->GetExposedParameters().AddParameter(Intensity);
	EditorData->Modify();
	UNiagaraScriptVariable* IntensityVariable = EditorData->FindOrAddUserScriptVariable(Intensity, *System);
	if (!TestNotNull(TEXT("user script variable"), IntensityVariable))
	{
		return false;
	}
	IntensityVariable->Modify();
	FNiagaraInputParameterCustomization& Widget = IntensityVariable->Metadata.WidgetCustomization;
	Widget.WidgetType = ENiagaraInputWidgetType::Slider;
	Widget.bHasMinValue = true;
	Widget.MinValue = 0.0f;
	Widget.bHasMaxValue = true;
	Widget.MaxValue = 10.0f;
	IntensityVariable->Metadata.DisplayUnit = EUnit::Meters;
	IntensityVariable->Metadata.bAdvancedDisplay = true;
	PostEditMetadata(*IntensityVariable);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	// The system Parameters panel declares System.Strength; its Details: advanced display and an alias.
	const FAssetDocumentResult Declared = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Body = Document->GetObjectField(TEXT("Body"));
		TArray<TSharedPtr<FJsonValue>> Entries;
		const TArray<TSharedPtr<FJsonValue>>* Existing = nullptr;
		if (Body->TryGetArrayField(TEXT("Parameters"), Existing))
		{
			Entries = *Existing;
		}
		const TSharedRef<FJsonObject> Strength = MakeShared<FJsonObject>();
		Strength->SetStringField(TEXT("Name"), TEXT("System.Strength"));
		Strength->SetStringField(TEXT("Type"), TEXT("float"));
		Entries.Add(MakeShared<FJsonValueObject>(Strength));
		Body->SetArrayField(TEXT("Parameters"), Entries);
	}, TEXT("Declare"));
	UNiagaraScriptVariable* StrengthVariable = DeclaredVariable(Target, TEXT("System.Strength"));
	if (!TestTrue(TEXT("declare applies"), Declared.IsSuccess()) || !TestNotNull(TEXT("declared variable"), StrengthVariable))
	{
		return false;
	}
	StrengthVariable->Modify();
	StrengthVariable->Metadata.bAdvancedDisplay = true;
	StrengthVariable->Metadata.AlternateAliases = {FName(TEXT("System.Power"))};
	PostEditMetadata(*StrengthVariable);
	if (!TestTrue(TEXT("saved with declared metadata"), Target.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("ParameterMeta"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	const FString UserMeta = MetaText(ParameterNamed(Extracted, TEXT("UserParameters"), TEXT("User.Intensity")));
	const FString DeclaredMeta = MetaText(ParameterNamed(Extracted, TEXT("Parameters"), TEXT("System.Strength")));
	AddInfo(FString::Printf(TEXT("user Meta %s; declared Meta %s"), *UserMeta, *DeclaredMeta));
	TestTrue(TEXT("user Meta: slider, unit, advanced"), UserMeta.Contains(TEXT("Slider")) && UserMeta.Contains(TEXT("Meters"))
		&& UserMeta.Contains(TEXT("bAdvancedDisplay")) && UserMeta.Contains(TEXT("MaxValue")) && !UserMeta.Contains(TEXT("Description")));
	TestTrue(TEXT("declared Meta: advanced, alias"), DeclaredMeta.Contains(TEXT("bAdvancedDisplay")) && DeclaredMeta.Contains(TEXT("System.Power")));

	// A new system from the extract holds the same metadata.
	{
		const TSharedPtr<FJsonObject> Copy = Parse(Serialize(Extracted));
		Copy->SetStringField(TEXT("Action"), TEXT("Create"));
		Copy->SetStringField(TEXT("Target"), Created.PackageName);
		const FAssetDocumentResult Made = ApplyText(*this, Created, Serialize(Copy), TEXT("Create"));
		const FAssetDocumentResult Read = ExtractResult(Created);
		TestTrue(TEXT("create applies"), Made.IsSuccess() && Read.IsSuccess());
		TestEqual(TEXT("created user Meta"), MetaText(ParameterNamed(Read.Payload, TEXT("UserParameters"), TEXT("User.Intensity"))), UserMeta);
		TestEqual(TEXT("created declared Meta"), MetaText(ParameterNamed(Read.Payload, TEXT("Parameters"), TEXT("System.Strength"))), DeclaredMeta);
	}

	// Edited: the user Meta lists only the unit (slider and advanced display reset); the declared Meta is omitted (kept).
	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> User = ParameterNamed(Document, TEXT("UserParameters"), TEXT("User.Intensity"));
		const TSharedPtr<FJsonObject> Strength = ParameterNamed(Document, TEXT("Parameters"), TEXT("System.Strength"));
		const TSharedPtr<FJsonObject>* Meta = nullptr;
		if (User.IsValid() && User->TryGetObjectField(TEXT("Meta"), Meta))
		{
			const TSharedRef<FJsonObject> Kept = MakeShared<FJsonObject>();
			Kept->SetField(TEXT("DisplayUnit"), (*Meta)->TryGetField(TEXT("DisplayUnit")));
			User->SetObjectField(TEXT("Meta"), Kept);
		}
		if (Strength.IsValid())
		{
			Strength->RemoveField(TEXT("Meta"));
		}
	}, TEXT("Edit"));
	if (TestTrue(TEXT("edit applies"), Edited.IsSuccess()))
	{
		const UNiagaraScriptVariable* User = UserVariable(Target, TEXT("User.Intensity"));
		const UNiagaraScriptVariable* Strength = DeclaredVariable(Target, TEXT("System.Strength"));
		TestTrue(TEXT("user: unit kept, slider and advanced display reset"), User != nullptr
			&& User->Metadata.DisplayUnit == EUnit::Meters && !User->Metadata.bAdvancedDisplay
			&& User->Metadata.WidgetCustomization.WidgetType == ENiagaraInputWidgetType::Default);
		TestTrue(TEXT("declared: omitted Meta keeps it"), Strength != nullptr && Strength->Metadata.bAdvancedDisplay
			&& Strength->Metadata.AlternateAliases.Contains(FName(TEXT("System.Power"))));
	}

	// An empty declared Meta resets its metadata.
	const FAssetDocumentResult Cleared = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		if (const TSharedPtr<FJsonObject> Strength = ParameterNamed(Document, TEXT("Parameters"), TEXT("System.Strength")))
		{
			Strength->SetObjectField(TEXT("Meta"), MakeShared<FJsonObject>());
		}
	}, TEXT("Clear"));
	if (TestTrue(TEXT("clear applies"), Cleared.IsSuccess()))
	{
		const UNiagaraScriptVariable* Strength = DeclaredVariable(Target, TEXT("System.Strength"));
		TestTrue(TEXT("declared metadata reset"), Strength != nullptr && !Strength->Metadata.bAdvancedDisplay
			&& Strength->Metadata.AlternateAliases.IsEmpty());
		TestTrue(TEXT("re-extract has no declared Meta"), MetaText(ParameterNamed(ExtractResult(Target).Payload,
			TEXT("Parameters"), TEXT("System.Strength"))).IsEmpty());
	}
	return !HasAnyErrors();
}

#endif
