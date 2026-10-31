// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Control Rig details the editor authors outside the graph and hierarchy shapes:
// - ControlRigVariableDetails: a variable's Expose on Spawn, Private, Slider Range and Value Range (set as the variable
//   Details write them) are extracted, re-apply as a no-op, change through the document, and a document that leaves
//   them out keeps them.
// - ControlRigPinLayout: a function's pin layout made in the Details panel (pin categories, the pins in each, a pin label;
//   node layouts on) is extracted, re-applies as a no-op, changes through the document, is kept by a document that leaves
//   it out, a label is written while node layouts are off too (the console variable left off), and a label equal to the
//   pin's name reads as none.
// - ControlRigFunctionVariant: a function's Variant tags (the Details panel's Variant row) are extracted, re-apply as a
//   no-op, change and reorder through the document, are kept by a document that leaves them out, and a tag the
//   project does not define is refused.
// - ControlRigControlLimits: a float control whose limit is off keeps a Maximum (its slider range) and hides a space
//   from the space picker (Customization.RemovedSpaces): both are extracted, re-apply as a no-op, change through the
//   document, and are kept by a document that leaves them out.
// - ControlRigHierarchyComponents: hierarchy components (added as the hierarchy's Add Component does) are not in the
//   document: Extract warns ControlRig.ComponentsNotAuthored at each element that has some, the extract re-applies as
//   a no-op keeping them, a rename keeps them, and removing an element that has some warns ControlRig.ComponentsRemoved.

#include "AssetDocumentIKRigTestSupport.h"
#include "AssetDocumentTolerantTestSupport.h"

#include "ControlRigBlueprintLegacy.h"
#include "HAL/IConsoleManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/ScopeExit.h"
#include "RigVMModel/Nodes/RigVMLibraryNode.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMFunctionLibrary.h"
#include "RigVMModel/RigVMPin.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyComponents.h"
#include "Rigs/RigHierarchyController.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentControlRigTests
{
UControlRigBlueprint* MakeControlRig(FAutomationTestBase& Test, const AssetDocumentIKRigTests::FTarget& Target, const TCHAR* MeshPath, bool bModular);
} // namespace AssetDocumentControlRigTests

namespace AssetDocumentTolerantControlRigDetailsTestsLocal
{
// The tolerant-test view of a Control Rig fixture (same asset, package and sidecar).
AssetDocumentTolerantTestSupport::FTarget DetailsView(const AssetDocumentIKRigTests::FTarget& Target)
{
	AssetDocumentTolerantTestSupport::FTarget Out(TEXT("Unused"));
	Out.AssetName = Target.AssetName;
	Out.PackageName = Target.PackageName;
	Out.ObjectPath = Target.ObjectPath;
	Out.PackageFile = Target.PackageFile;
	Out.SidecarPath = Target.SidecarPath;
	return Out;
}

// The Body/Variables entry named Name (null when absent).
TSharedPtr<FJsonObject> DetailsVariable(const TSharedPtr<FJsonObject>& Document, const TCHAR* Name)
{
	if (!Document.IsValid())
	{
		return nullptr;
	}
	for (const TSharedPtr<FJsonValue>& Value : Document->GetObjectField(TEXT("Body"))->GetArrayField(TEXT("Variables")))
	{
		if (Value->AsObject()->GetStringField(TEXT("Name")) == Name)
		{
			return Value->AsObject();
		}
	}
	return nullptr;
}

FString DetailsMetadata(UBlueprint& Blueprint, const TCHAR* Variable, const TCHAR* Key)
{
	FString Value;
	FBlueprintEditorUtils::GetBlueprintVariableMetaData(&Blueprint, Variable, nullptr, Key, Value);
	return Value;
}

TSharedPtr<FJsonObject> DetailsFunction(const TSharedPtr<FJsonObject>& Document, const TCHAR* Name)
{
	const TSharedPtr<FJsonObject>* Functions = nullptr;
	const TSharedPtr<FJsonObject>* Function = nullptr;
	return Document.IsValid() && Document->GetObjectField(TEXT("Body"))->TryGetObjectField(TEXT("Functions"), Functions)
		&& (*Functions)->TryGetObjectField(Name, Function) ? *Function : nullptr;
}

// The index of pin Name in a function entry's Inputs (INDEX_NONE when absent).
int32 DetailsInputIndex(const TSharedPtr<FJsonObject>& Function, const TCHAR* Name)
{
	return Function.IsValid() ? Function->GetArrayField(TEXT("Inputs")).IndexOfByPredicate([Name](const TSharedPtr<FJsonValue>& Pin)
	{
		return Pin->AsObject()->GetStringField(TEXT("Name")) == Name;
	}) : INDEX_NONE;
}

// A function node's pin categories as "Name[pin,pin]" joined by "; " (the layout the Details panel shows).
FString DetailsLayout(const URigVMNode& Node)
{
	TArray<FString> Parts;
	const FRigVMNodeLayout Layout = Node.GetNodeLayout(true);
	for (const FRigVMPinCategory& Category : Layout.Categories)
	{
		Parts.Add(FString::Printf(TEXT("%s[%s]"), *Category.Path, *FString::Join(Category.Elements, TEXT(","))));
	}
	return FString::Join(Parts, TEXT("; "));
}

// The Body/Hierarchy entry of element Key ("Type(Name)"; null when absent).
TSharedPtr<FJsonObject> DetailsElement(const TSharedPtr<FJsonObject>& Document, const FString& Type, const FString& Name)
{
	if (!Document.IsValid())
	{
		return nullptr;
	}
	for (const TSharedPtr<FJsonValue>& Value : Document->GetObjectField(TEXT("Body"))->GetArrayField(TEXT("Hierarchy")))
	{
		if (Value->AsObject()->GetStringField(TEXT("Type")) == Type && Value->AsObject()->GetStringField(TEXT("Name")) == Name)
		{
			return Value->AsObject();
		}
	}
	return nullptr;
}

// RigVM.EnableNodeLayouts (the Details panel's pin layout section) on or off for a scope; value and SetBy restored after.
struct FDetailsNodeLayouts
{
	IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("RigVM.EnableNodeLayouts"));
	bool bWasOn = Variable != nullptr && Variable->GetBool();
	EConsoleVariableFlags WasFlags = Variable != nullptr ? Variable->GetFlags() : ECVF_Default;

	explicit FDetailsNodeLayouts(bool bOn) { Set(bOn); }
	~FDetailsNodeLayouts()
	{
		Set(bWasOn);
		if (Variable != nullptr)
		{
			Variable->SetFlags(WasFlags);
		}
	}

	EConsoleVariableFlags SetBy() const
	{
		return Variable != nullptr ? static_cast<EConsoleVariableFlags>(Variable->GetFlags() & ECVF_SetByMask) : ECVF_Default;
	}
	FDetailsNodeLayouts(const FDetailsNodeLayouts&) = delete;
	FDetailsNodeLayouts& operator=(const FDetailsNodeLayouts&) = delete;

	void Set(bool bOn) const
	{
		if (Variable != nullptr)
		{
			Variable->Set(bOn, ECVF_SetByCode);
		}
	}
};
} // namespace AssetDocumentTolerantControlRigDetailsTestsLocal

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantControlRigVariableDetailsTest, "UEAssetDoc.Tolerant.ControlRigVariableDetails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantControlRigVariableDetailsTest::RunTest(const FString& Parameters)
{
	namespace Tolerant = AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentTolerantControlRigDetailsTestsLocal;
	AssetDocumentIKRigTests::FFixtures Fixtures;
	const AssetDocumentIKRigTests::FTarget& Rig = Fixtures.Add(TEXT("CR_VariableDetails"));
	UControlRigBlueprint* Blueprint = AssetDocumentControlRigTests::MakeControlRig(*this, Rig, AssetDocumentIKRigTests::TutorialMesh, false);
	if (Blueprint == nullptr)
	{
		return false;
	}
	// My Blueprint's "+" and the variable Details: Expose on Spawn, Private, Slider Range 0..10, Value Range -5..20.5;
	// a second variable with only a Value Range maximum.
	IRigVMAssetInterface& RigVM = *Blueprint;
	RigVM.AddMemberVariable(TEXT("Speed"), TEXT("float"), true, false, TEXT("1.0"));
	RigVM.AddMemberVariable(TEXT("Count"), TEXT("int32"), false, false, FString());
	RigVM.SetVariableExposeOnSpawn(TEXT("Speed"), true);
	RigVM.SetVariablePrivate(TEXT("Speed"), true);
	FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, TEXT("Speed"), nullptr, TEXT("UIMin"), TEXT("0"));
	FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, TEXT("Speed"), nullptr, TEXT("UIMax"), TEXT("10"));
	FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, TEXT("Speed"), nullptr, TEXT("ClampMin"), TEXT("-5"));
	FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, TEXT("Speed"), nullptr, TEXT("ClampMax"), TEXT("20.5"));
	FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, TEXT("Count"), nullptr, TEXT("ClampMax"), TEXT("100"));
	if (!TestTrue(TEXT("saved"), AssetDocumentIKRigTests::Save(*Blueprint)))
	{
		return false;
	}
	const Tolerant::FTarget Target = DetailsView(Rig);

	const TSharedPtr<FJsonObject> Extracted = Tolerant::TestTolerantRoundTrip(*this, Target, Tolerant::FExpected(), TEXT("Variable details"));
	const TSharedPtr<FJsonObject> Speed = DetailsVariable(Extracted, TEXT("Speed"));
	const TSharedPtr<FJsonObject> Count = DetailsVariable(Extracted, TEXT("Count"));
	if (!TestTrue(TEXT("variables extracted"), Speed.IsValid() && Count.IsValid()))
	{
		return false;
	}
	bool bFlag = false;
	TestTrue(TEXT("ExposeOnSpawn"), Speed->TryGetBoolField(TEXT("ExposeOnSpawn"), bFlag) && bFlag);
	TestTrue(TEXT("Private"), Speed->TryGetBoolField(TEXT("Private"), bFlag) && bFlag);
	// Documents list object keys sorted.
	TestEqual(TEXT("SliderRange"), Tolerant::Serialize(Speed->GetObjectField(TEXT("SliderRange"))), FString(TEXT("{\"Max\":10,\"Min\":0}")));
	TestEqual(TEXT("ValueRange"), Tolerant::Serialize(Speed->GetObjectField(TEXT("ValueRange"))), FString(TEXT("{\"Max\":20.5,\"Min\":-5}")));
	TestEqual(TEXT("Count ValueRange"), Tolerant::Serialize(Count->GetObjectField(TEXT("ValueRange"))), FString(TEXT("{\"Max\":100}")));
	TestFalse(TEXT("Count has no SliderRange"), Count->HasField(TEXT("SliderRange")) || Count->HasField(TEXT("ExposeOnSpawn")) || Count->HasField(TEXT("Private")));

	// The document changes them: Expose on Spawn off, the slider range 1..5, the value range minimum cleared, a Count
	// slider maximum.
	const FAssetDocumentResult Edited = Tolerant::ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Entry = DetailsVariable(Document, TEXT("Speed"));
		Entry->SetBoolField(TEXT("ExposeOnSpawn"), false);
		const TSharedPtr<FJsonObject> Slider = MakeShared<FJsonObject>();
		Slider->SetNumberField(TEXT("Min"), 1);
		Slider->SetNumberField(TEXT("Max"), 5);
		Entry->SetObjectField(TEXT("SliderRange"), Slider);
		Entry->GetObjectField(TEXT("ValueRange"))->RemoveField(TEXT("Min"));
		const TSharedPtr<FJsonObject> CountSlider = MakeShared<FJsonObject>();
		CountSlider->SetNumberField(TEXT("Max"), 50);
		DetailsVariable(Document, TEXT("Count"))->SetObjectField(TEXT("SliderRange"), CountSlider);
	}, TEXT("Edit"));
	if (TestTrue(TEXT("edit applied"), Edited.IsSuccess()))
	{
		UBlueprint* Applied = Target.Get<UBlueprint>();
		if (TestNotNull(TEXT("rig"), Applied))
		{
			TestEqual(TEXT("Expose on Spawn cleared"), DetailsMetadata(*Applied, TEXT("Speed"), TEXT("ExposeOnSpawn")), FString());
			TestEqual(TEXT("Private kept"), DetailsMetadata(*Applied, TEXT("Speed"), TEXT("BlueprintPrivate")), FString(TEXT("true")));
			TestEqual(TEXT("UIMin"), DetailsMetadata(*Applied, TEXT("Speed"), TEXT("UIMin")), FString(TEXT("1")));
			TestEqual(TEXT("UIMax"), DetailsMetadata(*Applied, TEXT("Speed"), TEXT("UIMax")), FString(TEXT("5")));
			TestEqual(TEXT("ClampMin cleared"), DetailsMetadata(*Applied, TEXT("Speed"), TEXT("ClampMin")), FString());
			TestEqual(TEXT("ClampMax kept"), DetailsMetadata(*Applied, TEXT("Speed"), TEXT("ClampMax")), FString(TEXT("20.5")));
			TestEqual(TEXT("Count UIMax"), DetailsMetadata(*Applied, TEXT("Count"), TEXT("UIMax")), FString(TEXT("50")));
		}
		const TSharedPtr<FJsonObject> After = Tolerant::ExtractResult(Target).Payload;
		Tolerant::TestAppliesUnchanged(*this, Target, After, TEXT("Edited extract"));
	}

	// A document written before these were read (the keys left out) keeps them.
	const TSharedPtr<FJsonObject> Older = Tolerant::ExtractResult(Target).Payload;
	if (TestTrue(TEXT("extract"), Older.IsValid()))
	{
		for (const TCHAR* Key : {TEXT("ExposeOnSpawn"), TEXT("Private"), TEXT("SliderRange"), TEXT("ValueRange")})
		{
			DetailsVariable(Older, TEXT("Speed"))->RemoveField(Key);
			DetailsVariable(Older, TEXT("Count"))->RemoveField(Key);
		}
		Tolerant::TestAppliesUnchanged(*this, Target, Older, TEXT("Keys left out"));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantControlRigPinLayoutTest, "UEAssetDoc.Tolerant.ControlRigPinLayout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantControlRigPinLayoutTest::RunTest(const FString& Parameters)
{
	namespace Tolerant = AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentTolerantControlRigDetailsTestsLocal;
	FDetailsNodeLayouts Layouts(true);
	if (!TestNotNull(TEXT("RigVM.EnableNodeLayouts"), Layouts.Variable))
	{
		return false;
	}
	AssetDocumentIKRigTests::FFixtures Fixtures;
	const AssetDocumentIKRigTests::FTarget& Rig = Fixtures.Add(TEXT("CR_PinLayout"));
	UControlRigBlueprint* Blueprint = AssetDocumentControlRigTests::MakeControlRig(*this, Rig, AssetDocumentIKRigTests::TutorialMesh, false);
	if (Blueprint == nullptr)
	{
		return false;
	}
	// My Blueprint's New Function with three float inputs and an output, then the Details panel's pin layout (the
	// library's controller on the function node): categories Settings and Unused, Gamma then Alpha in Settings, a label.
	URigVMBlueprint& VM = *Blueprint;
	URigVMController* Library = VM.GetLocalFunctionLibrary() != nullptr ? VM.GetOrCreateController(VM.GetLocalFunctionLibrary()) : nullptr;
	URigVMLibraryNode* Function = Library != nullptr ? Library->AddFunctionToLibrary(TEXT("Blend"), true, FVector2D::ZeroVector, false, false) : nullptr;
	URigVMController* Body = Function != nullptr ? VM.GetOrCreateController(Function->GetContainedGraph()) : nullptr;
	if (!TestNotNull(TEXT("function"), Body))
	{
		return false;
	}
	for (const TCHAR* Name : {TEXT("Alpha"), TEXT("Beta"), TEXT("Gamma")})
	{
		Body->AddExposedPin(Name, ERigVMPinDirection::Input, TEXT("float"), NAME_None, FString(), false, false);
	}
	Body->AddExposedPin(TEXT("Result"), ERigVMPinDirection::Output, TEXT("float"), NAME_None, FString(), false, false);
	TestTrue(TEXT("category Settings"), Library->AddEmptyPinCategory(Function->GetFName(), TEXT("Settings"), false, false));
	TestTrue(TEXT("category Unused"), Library->AddEmptyPinCategory(Function->GetFName(), TEXT("Unused"), false, false));
	TestTrue(TEXT("Gamma in Settings"), Library->SetPinCategory(Function->FindPin(TEXT("Gamma"))->GetPinPath(), TEXT("Settings"), false, false));
	TestTrue(TEXT("Alpha in Settings"), Library->SetPinCategory(Function->FindPin(TEXT("Alpha"))->GetPinPath(), TEXT("Settings"), false, false));
	TestTrue(TEXT("Alpha label"), Library->SetPinDisplayName(Function->FindPin(TEXT("Alpha"))->GetPinPath(), TEXT("Blend Alpha"), false, false));
	const FString Made = DetailsLayout(*Function);
	if (!TestTrue(TEXT("saved"), AssetDocumentIKRigTests::Save(*Blueprint)))
	{
		return false;
	}
	const Tolerant::FTarget Target = DetailsView(Rig);

	const TSharedPtr<FJsonObject> Extracted = Tolerant::TestTolerantRoundTrip(*this, Target, Tolerant::FExpected(), TEXT("Pin layout"));
	const TSharedPtr<FJsonObject> Entry = DetailsFunction(Extracted, TEXT("Blend"));
	if (!TestTrue(TEXT("function extracted with its pin categories"), Entry.IsValid() && Entry->HasField(TEXT("PinCategories"))))
	{
		return false;
	}
	TArray<FString> Read;
	for (const TSharedPtr<FJsonValue>& Category : Entry->GetArrayField(TEXT("PinCategories")))
	{
		TArray<FString> Pins;
		Category->AsObject()->TryGetStringArrayField(TEXT("Pins"), Pins);
		Read.Add(FString::Printf(TEXT("%s[%s]"), *Category->AsObject()->GetStringField(TEXT("Name")), *FString::Join(Pins, TEXT(","))));
	}
	TestEqual(TEXT("PinCategories as the Details panel shows them"), FString::Join(Read, TEXT("; ")), Made);
	TestTrue(TEXT("Settings holds Gamma and Alpha"), Made.Contains(TEXT("Gamma")) && Made.Contains(TEXT("Alpha")) && Made.Contains(TEXT("Unused[]")));
	const int32 Alpha = DetailsInputIndex(Entry, TEXT("Alpha"));
	TestEqual(TEXT("Alpha label"), Tolerant::StringAt(Extracted, FString::Printf(TEXT("/Body/Functions/Blend/Inputs/%d/DisplayName"), Alpha)),
		FString(TEXT("Blend Alpha")));

	// The document's layout: Unused removed, Main added first (expanded) with Beta then Alpha, Gamma alone in Settings;
	// Alpha's label cleared, Beta labelled.
	const FAssetDocumentResult Edited = Tolerant::ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Blend = DetailsFunction(Document, TEXT("Blend"));
		const TSharedPtr<FJsonObject> Categories = Tolerant::Parse(TEXT("{\"List\":[{\"Name\":\"Main\",\"Pins\":[\"Beta\",\"Alpha\"],"
			"\"Expanded\":true},{\"Name\":\"Settings\",\"Pins\":[\"Gamma\"]}]}"));
		Blend->SetArrayField(TEXT("PinCategories"), Categories->GetArrayField(TEXT("List")));
		const TArray<TSharedPtr<FJsonValue>>& Inputs = Blend->GetArrayField(TEXT("Inputs"));
		Inputs[DetailsInputIndex(Blend, TEXT("Alpha"))]->AsObject()->SetStringField(TEXT("DisplayName"), FString());
		Inputs[DetailsInputIndex(Blend, TEXT("Beta"))]->AsObject()->SetStringField(TEXT("DisplayName"), TEXT("Blend Beta"));
	}, TEXT("Edit"));
	if (TestTrue(TEXT("layout edit applied"), Edited.IsSuccess()))
	{
		UControlRigBlueprint* Applied = Target.Get<UControlRigBlueprint>();
		const URigVMLibraryNode* After = Applied != nullptr && static_cast<URigVMBlueprint&>(*Applied).GetLocalFunctionLibrary() != nullptr
			? static_cast<URigVMBlueprint&>(*Applied).GetLocalFunctionLibrary()->FindFunction(TEXT("Blend")) : nullptr;
		if (TestNotNull(TEXT("function after the edit"), After))
		{
			TestEqual(TEXT("layout after the edit"), DetailsLayout(*After), FString(TEXT("Main[Beta,Alpha]; Settings[Gamma]")));
			TestTrue(TEXT("Main expanded"), After->IsPinCategoryExpanded(TEXT("Main")));
			const FRigVMNodeLayout Layout = After->GetNodeLayout(true);
			TestFalse(TEXT("Alpha label cleared"), Layout.DisplayNames.Contains(TEXT("Alpha")));
			TestEqual(TEXT("Beta label"), Layout.DisplayNames.FindRef(TEXT("Beta")), FString(TEXT("Blend Beta")));
		}
		Tolerant::TestAppliesUnchanged(*this, Target, Tolerant::ExtractResult(Target).Payload, TEXT("Edited extract"));
	}

	// A document written before the layout was read (no PinCategories, no DisplayName) keeps it.
	const TSharedPtr<FJsonObject> Older = Tolerant::ExtractResult(Target).Payload;
	const TSharedPtr<FJsonObject> OlderBlend = DetailsFunction(Older, TEXT("Blend"));
	if (TestTrue(TEXT("extract"), OlderBlend.IsValid()))
	{
		OlderBlend->RemoveField(TEXT("PinCategories"));
		for (const TSharedPtr<FJsonValue>& Pin : OlderBlend->GetArrayField(TEXT("Inputs")))
		{
			Pin->AsObject()->RemoveField(TEXT("DisplayName"));
		}
		Tolerant::TestAppliesUnchanged(*this, Target, Older, TEXT("Layout left out"));
	}

	// Node layouts off (the default, set by the engine's constructor): the rig still holds its layout, and a document's
	// label is written all the same; the console variable is left off with its SetBy unchanged, so an ini or device
	// profile can still set it.
	Layouts.Set(false);
	Layouts.Variable->SetFlags(static_cast<EConsoleVariableFlags>((Layouts.Variable->GetFlags() & ~ECVF_SetByMask) | ECVF_SetByConstructor));
	const FAssetDocumentResult LayoutsOff = Tolerant::ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Blend = DetailsFunction(Document, TEXT("Blend"));
		Blend->GetArrayField(TEXT("Inputs"))[DetailsInputIndex(Blend, TEXT("Beta"))]->AsObject()->SetStringField(TEXT("DisplayName"), TEXT("Other"));
	}, TEXT("Layouts off"));
	if (TestTrue(TEXT("label written while node layouts are off"), LayoutsOff.IsSuccess()))
	{
		UControlRigBlueprint* Applied = Target.Get<UControlRigBlueprint>();
		const URigVMLibraryNode* After = Applied != nullptr && static_cast<URigVMBlueprint&>(*Applied).GetLocalFunctionLibrary() != nullptr
			? static_cast<URigVMBlueprint&>(*Applied).GetLocalFunctionLibrary()->FindFunction(TEXT("Blend")) : nullptr;
		const FRigVMNodeLayout Layout = After != nullptr ? After->GetNodeLayout(true) : FRigVMNodeLayout();
		TestEqual(TEXT("Beta label"), Layout.DisplayNames.FindRef(TEXT("Beta")), FString(TEXT("Other")));
	}
	TestFalse(TEXT("node layouts stay off"), Layouts.Variable->GetBool());
	TestEqual(TEXT("node layouts keep their SetBy"), static_cast<int32>(Layouts.SetBy()), static_cast<int32>(ECVF_SetByConstructor));
	// A label equal to the pin's name is no label: it reads back as none and re-applies unchanged.
	const FAssetDocumentResult Named = Tolerant::ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Blend = DetailsFunction(Document, TEXT("Blend"));
		Blend->GetArrayField(TEXT("Inputs"))[DetailsInputIndex(Blend, TEXT("Gamma"))]->AsObject()->SetStringField(TEXT("DisplayName"), TEXT("Gamma"));
	}, TEXT("Label equal to the name"));
	TestTrue(TEXT("a label equal to the pin's name applies"), Named.IsSuccess());
	const TSharedPtr<FJsonObject> Reread = DetailsFunction(Tolerant::ExtractResult(Target).Payload, TEXT("Blend"));
	TestFalse(TEXT("and reads as no label"), Reread.IsValid() && Reread->GetArrayField(TEXT("Inputs"))[DetailsInputIndex(Reread, TEXT("Gamma"))]->AsObject()->HasField(TEXT("DisplayName")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantControlRigFunctionVariantTest, "UEAssetDoc.Tolerant.ControlRigFunctionVariant",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantControlRigFunctionVariantTest::RunTest(const FString& Parameters)
{
	namespace Tolerant = AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentTolerantControlRigDetailsTestsLocal;
	AssetDocumentIKRigTests::FFixtures Fixtures;
	const AssetDocumentIKRigTests::FTarget& Rig = Fixtures.Add(TEXT("CR_FunctionVariant"));
	UControlRigBlueprint* Blueprint = AssetDocumentControlRigTests::MakeControlRig(*this, Rig, AssetDocumentIKRigTests::TutorialMesh, false);
	if (Blueprint == nullptr)
	{
		return false;
	}
	// My Blueprint's New Function, then the Details panel's Variant row: the project tag Stable.
	URigVMBlueprint& VM = *Blueprint;
	URigVMController* Library = VM.GetLocalFunctionLibrary() != nullptr ? VM.GetOrCreateController(VM.GetLocalFunctionLibrary()) : nullptr;
	URigVMLibraryNode* Function = Library != nullptr ? Library->AddFunctionToLibrary(TEXT("Tagged"), true, FVector2D::ZeroVector, false, false) : nullptr;
	if (!TestNotNull(TEXT("function"), Function)
		|| !TestTrue(TEXT("tag Stable"), Library->AddDefaultTagToFunctionVariant(Function->GetFName(), TEXT("Stable"), false, false))
		|| !TestTrue(TEXT("saved"), AssetDocumentIKRigTests::Save(*Blueprint)))
	{
		return false;
	}
	const Tolerant::FTarget Target = DetailsView(Rig);
	auto Tags = [&Target]()
	{
		UControlRigBlueprint* Applied = Target.Get<UControlRigBlueprint>();
		const URigVMFunctionLibrary* Functions = Applied != nullptr ? static_cast<URigVMBlueprint&>(*Applied).GetLocalFunctionLibrary() : nullptr;
		const URigVMLibraryNode* Node = Functions != nullptr ? Functions->FindFunction(TEXT("Tagged")) : nullptr;
		TArray<FString> Names;
		if (Node != nullptr)
		{
			for (const FRigVMTag& Tag : Node->GetFunctionVariant().Tags)
			{
				Names.Add(Tag.Name.ToString());
			}
		}
		return FString::Join(Names, TEXT(","));
	};

	const TSharedPtr<FJsonObject> Extracted = Tolerant::TestTolerantRoundTrip(*this, Target, Tolerant::FExpected(), TEXT("Variant"));
	const TSharedPtr<FJsonObject> Entry = DetailsFunction(Extracted, TEXT("Tagged"));
	TArray<FString> Read;
	const TSharedPtr<FJsonObject>* Variant = nullptr;
	TestTrue(TEXT("Variant extracted"), Entry.IsValid() && Entry->TryGetObjectField(TEXT("Variant"), Variant)
		&& (*Variant)->TryGetStringArrayField(TEXT("Tags"), Read) && FString::Join(Read, TEXT(",")) == TEXT("Stable"));

	// Outdated added before Stable (an order the appends cannot reach).
	const FAssetDocumentResult Edited = Tolerant::ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		DetailsFunction(Document, TEXT("Tagged"))->SetObjectField(TEXT("Variant"), Tolerant::Parse(TEXT("{\"Tags\":[\"Outdated\",\"Stable\"]}")));
	}, TEXT("Edit"));
	if (TestTrue(TEXT("tags applied"), Edited.IsSuccess()))
	{
		TestEqual(TEXT("tags after the edit"), Tags(), FString(TEXT("Outdated,Stable")));
		Tolerant::TestAppliesUnchanged(*this, Target, Tolerant::ExtractResult(Target).Payload, TEXT("Edited extract"));
	}

	// Left out, the tags are kept.
	const TSharedPtr<FJsonObject> Older = Tolerant::ExtractResult(Target).Payload;
	if (TestTrue(TEXT("extract"), DetailsFunction(Older, TEXT("Tagged")).IsValid()))
	{
		DetailsFunction(Older, TEXT("Tagged"))->RemoveField(TEXT("Variant"));
		Tolerant::TestAppliesUnchanged(*this, Target, Older, TEXT("Variant left out"));
	}

	// A tag the project does not define cannot be added (the Variant row offers only the project's tags).
	const FAssetDocumentResult Refused = Tolerant::ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		DetailsFunction(Document, TEXT("Tagged"))->SetObjectField(TEXT("Variant"), Tolerant::Parse(TEXT("{\"Tags\":[\"NoSuchTag\"]}")));
	}, TEXT("Unknown tag"));
	Tolerant::TestRefused(*this, Refused, TEXT("ControlRig.InvalidValue"), TEXT("/Body/Functions/Tagged/Variant/Tags"), TEXT("Unknown tag"));
	TestEqual(TEXT("tags unchanged by the refusal"), Tags(), FString(TEXT("Outdated,Stable")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantControlRigControlLimitsTest, "UEAssetDoc.Tolerant.ControlRigControlLimits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantControlRigControlLimitsTest::RunTest(const FString& Parameters)
{
	namespace Tolerant = AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentTolerantControlRigDetailsTestsLocal;
	AssetDocumentIKRigTests::FFixtures Fixtures;
	const AssetDocumentIKRigTests::FTarget& Rig = Fixtures.Add(TEXT("CR_ControlLimits"));
	UControlRigBlueprint* Blueprint = AssetDocumentControlRigTests::MakeControlRig(*this, Rig, AssetDocumentIKRigTests::TutorialMesh, false);
	if (Blueprint == nullptr)
	{
		return false;
	}
	// A float control as the Details panel's type switch makes it (limit off, range 0..100), a space, and the pelvis
	// hidden from its space picker (the control settings' Customization).
	URigHierarchy* Hierarchy = Blueprint->GetHierarchy();
	URigHierarchyController* Elements = Hierarchy->GetController(true);
	const FRigElementKey Root(TEXT("root"), ERigElementType::Bone);
	const FRigElementKey Pelvis(TEXT("pelvis"), ERigElementType::Bone);
	const FRigElementKey Space = Elements->AddNull(TEXT("limits_space"), Root, FTransform::Identity, true, false);
	FRigControlSettings Settings;
	Settings.ControlType = ERigControlType::Float;
	Settings.SetupLimitArrayForType(false);
	Settings.MinimumValue = FRigControlValue::Make<float>(0.f);
	Settings.MaximumValue = FRigControlValue::Make<float>(100.f);
	const FRigElementKey Slider = Elements->AddControl(TEXT("slider_ctrl"), Root, Settings, FRigControlValue::Make<float>(0.f), FTransform::Identity,
		FTransform::Identity, false);
	if (!TestTrue(TEXT("control"), Slider.IsValid()) || !TestTrue(TEXT("space"), Elements->AddAvailableSpace(Slider, Space, NAME_None, false)))
	{
		return false;
	}
	FRigControlSettings Hidden = Hierarchy->Find<FRigControlElement>(Slider)->Settings;
	Hidden.Customization.RemovedSpaces.Add(Pelvis);
	Hierarchy->SetControlSettings(Slider, Hidden, false, true, false);
	if (!TestTrue(TEXT("saved"), AssetDocumentIKRigTests::Save(*Blueprint)))
	{
		return false;
	}
	const Tolerant::FTarget Target = DetailsView(Rig);

	const TSharedPtr<FJsonObject> Extracted = Tolerant::TestTolerantRoundTrip(*this, Target, Tolerant::FExpected(), TEXT("Control limits"));
	const TSharedPtr<FJsonObject> Entry = DetailsElement(Extracted, TEXT("Control"), TEXT("slider_ctrl"));
	if (!TestTrue(TEXT("control extracted"), Entry.IsValid()))
	{
		return false;
	}
	double Maximum = 0.0;
	TestTrue(TEXT("Maximum shown with the limit off"), Entry->TryGetNumberField(TEXT("Maximum"), Maximum) && FMath::IsNearlyEqual(Maximum, 100.0));
	TestFalse(TEXT("a zero Minimum is left out"), Entry->HasField(TEXT("Minimum")));
	TArray<FString> Removed;
	TestTrue(TEXT("RemovedSpaces"), Entry->TryGetStringArrayField(TEXT("RemovedSpaces"), Removed) && FString::Join(Removed, TEXT(",")) == TEXT("Bone(pelvis)"));

	const FAssetDocumentResult Edited = Tolerant::ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Control = DetailsElement(Document, TEXT("Control"), TEXT("slider_ctrl"));
		Control->SetNumberField(TEXT("Maximum"), 50.0);
		Control->SetArrayField(TEXT("RemovedSpaces"), TArray<TSharedPtr<FJsonValue>>());
	}, TEXT("Edit"));
	if (TestTrue(TEXT("edit applied"), Edited.IsSuccess()))
	{
		UControlRigBlueprint* Applied = Target.Get<UControlRigBlueprint>();
		const FRigControlElement* Control = Applied != nullptr ? Applied->GetHierarchy()->Find<FRigControlElement>(Slider) : nullptr;
		if (TestNotNull(TEXT("control after the edit"), Control))
		{
			TestEqual(TEXT("Maximum"), Applied->GetHierarchy()->GetControlValue(Slider, ERigControlValueType::Maximum).Get<float>(), 50.f);
			TestEqual(TEXT("RemovedSpaces cleared"), Control->Settings.Customization.RemovedSpaces.Num(), 0);
			TestEqual(TEXT("spaces kept"), Control->Settings.Customization.AvailableSpaces.Num(), 1);
		}
		Tolerant::TestAppliesUnchanged(*this, Target, Tolerant::ExtractResult(Target).Payload, TEXT("Edited extract"));
	}

	// Put back, then a document that leaves both out keeps them.
	const FAssetDocumentResult Restored = Tolerant::ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		TArray<TSharedPtr<FJsonValue>> Keys;
		Keys.Add(MakeShared<FJsonValueString>(TEXT("Bone(pelvis)")));
		DetailsElement(Document, TEXT("Control"), TEXT("slider_ctrl"))->SetArrayField(TEXT("RemovedSpaces"), Keys);
	}, TEXT("Restore"));
	TestTrue(TEXT("RemovedSpaces set by the document"), Restored.IsSuccess());
	const TSharedPtr<FJsonObject> Older = Tolerant::ExtractResult(Target).Payload;
	const TSharedPtr<FJsonObject> OlderControl = DetailsElement(Older, TEXT("Control"), TEXT("slider_ctrl"));
	if (TestTrue(TEXT("extract shows them"), OlderControl.IsValid() && OlderControl->HasField(TEXT("RemovedSpaces")) && OlderControl->HasField(TEXT("Maximum"))))
	{
		OlderControl->RemoveField(TEXT("RemovedSpaces"));
		OlderControl->RemoveField(TEXT("Maximum"));
		Tolerant::TestAppliesUnchanged(*this, Target, Older, TEXT("Left out"));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantControlRigHierarchyComponentsTest, "UEAssetDoc.Tolerant.ControlRigHierarchyComponents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantControlRigHierarchyComponentsTest::RunTest(const FString& Parameters)
{
	namespace Tolerant = AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentTolerantControlRigDetailsTestsLocal;
	AssetDocumentIKRigTests::FFixtures Fixtures;
	const AssetDocumentIKRigTests::FTarget& Rig = Fixtures.Add(TEXT("CR_HierarchyComponents"));
	UControlRigBlueprint* Blueprint = AssetDocumentControlRigTests::MakeControlRig(*this, Rig, AssetDocumentIKRigTests::TutorialMesh, false);
	UScriptStruct* ComponentType = FindObject<UScriptStruct>(nullptr, TEXT("/Script/ControlRig.RigBaseComponent"));
	if (Blueprint == nullptr || !TestNotNull(TEXT("component struct"), ComponentType))
	{
		return false;
	}
	// Two nulls with a component each, as the hierarchy's Add Component makes them.
	URigHierarchy* Hierarchy = Blueprint->GetHierarchy();
	URigHierarchyController* Elements = Hierarchy->GetController(true);
	const FRigElementKey Root(TEXT("root"), ERigElementType::Bone);
	const FRigElementKey Dropped = Elements->AddNull(TEXT("components_dropped"), Root, FTransform::Identity, true, false);
	const FRigElementKey Kept = Elements->AddNull(TEXT("components_kept"), Root, FTransform::Identity, true, false);
	const FRigComponentKey DroppedComponent = Elements->AddComponent(ComponentType, TEXT("DroppedPart"), Dropped);
	const FRigComponentKey KeptComponent = Elements->AddComponent(ComponentType, TEXT("KeptPart"), Kept);
	if (!TestTrue(TEXT("components added"), DroppedComponent.IsValid() && KeptComponent.IsValid())
		|| !TestTrue(TEXT("saved"), AssetDocumentIKRigTests::Save(*Blueprint)))
	{
		return false;
	}
	const Tolerant::FTarget Target = DetailsView(Rig);

	// Extract warns at each of the two elements.
	const TSharedPtr<FJsonObject> First = Tolerant::ExtractResult(Target).Payload;
	Tolerant::FExpected Expected;
	if (First.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>& Elements0 = First->GetObjectField(TEXT("Body"))->GetArrayField(TEXT("Hierarchy"));
		for (int32 Index = 0; Index < Elements0.Num(); ++Index)
		{
			const FString Name = Elements0[Index]->AsObject()->GetStringField(TEXT("Name"));
			if (Name == TEXT("components_dropped") || Name == TEXT("components_kept"))
			{
				Expected.Warnings.Add({TEXT("ControlRig.ComponentsNotAuthored"), FString::Printf(TEXT("/Body/Hierarchy/%d"), Index)});
			}
		}
	}
	TestEqual(TEXT("both elements listed"), Expected.Warnings.Num(), 2);
	Tolerant::TestTolerantRoundTrip(*this, Target, Expected, TEXT("Components"));
	UControlRigBlueprint* Loaded = Target.Get<UControlRigBlueprint>();
	TestEqual(TEXT("the round trip keeps the components"), Loaded != nullptr ? Loaded->GetHierarchy()->NumComponents() : -1, 2);

	// Rename one element, remove the other.
	const FAssetDocumentResult Edited = Tolerant::ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Renamed = DetailsElement(Document, TEXT("Null"), TEXT("components_kept"));
		Renamed->SetStringField(TEXT("RenamedFrom"), TEXT("components_kept"));
		Renamed->SetStringField(TEXT("Name"), TEXT("components_renamed"));
		TArray<TSharedPtr<FJsonValue>> Hierarchy0 = Document->GetObjectField(TEXT("Body"))->GetArrayField(TEXT("Hierarchy"));
		Hierarchy0.RemoveAll([](const TSharedPtr<FJsonValue>& Value) { return Value->AsObject()->GetStringField(TEXT("Name")) == TEXT("components_dropped"); });
		Document->GetObjectField(TEXT("Body"))->SetArrayField(TEXT("Hierarchy"), Hierarchy0);
	}, TEXT("Rename and remove"));
	if (TestTrue(TEXT("edit applied"), Edited.IsSuccess()))
	{
		TestTrue(TEXT("removing an element with components warns"), Tolerant::HasDiagnostic(Edited, TEXT("ControlRig.ComponentsRemoved"), TEXT("/Body/Hierarchy"), TEXT("warning")));
		UControlRigBlueprint* Applied = Target.Get<UControlRigBlueprint>();
		URigHierarchy* After = Applied != nullptr ? Applied->GetHierarchy() : nullptr;
		if (TestNotNull(TEXT("rig after the edit"), After))
		{
			TestEqual(TEXT("the renamed element keeps its component"), After->NumComponents(FRigElementKey(TEXT("components_renamed"), ERigElementType::Null)), 1);
			TestEqual(TEXT("only the removed element's component is gone"), After->NumComponents(), 1);
		}
	}
	return !HasAnyErrors();
}

#endif
