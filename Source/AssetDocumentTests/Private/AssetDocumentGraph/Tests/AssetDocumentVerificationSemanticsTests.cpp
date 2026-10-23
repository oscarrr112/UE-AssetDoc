// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// A Blueprint document the editor accepts and stores must verify, whatever order its items are
// written in and whatever default-valued fields it states or omits:
//   ComparisonPinDefault   a comparison node's pin default equal to the pin default ("0.0"), applied
//                              several times in one session and after a reload.
//   FunctionInputDefault   a function input Default "1.0" (stored as 1.000000).
//   CustomEventEmptyInputs a custom event with "Inputs": [].
//   ExtraReturnNode        PinOverrides on a further {"Implicit": "result"} return node.
//   ComponentsByName       components listed children first, defaults stated, AttachTo by the native
//                              component's subobject name (CollisionCylinder).
//   ParentCallOnOverride   a child overriding an event its Blueprint parent implements (the editor's
//                              automatically placed "Parent: Tick" call).
//   RemoveInputWithLink    removing a function input and its link in one apply.
//   StructArrayMaps        component Properties: an array of structs with maps and floats.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "AssetDocumentVerificationSemanticsTestTypes.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Camera/CameraComponent.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "HAL/FileManager.h"
#include "FileHelpers.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_PromotableOperator.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentVerificationSemanticsTests
{
struct FAsset
{
	FString Name;
	FString PackageName;
	FString ObjectPath;
	FString ClassPath;
	FString PackageFile;
	FString SidecarPath;

	FAsset(const FString& Folder, const TCHAR* InName)
		: Name(InName)
	{
		PackageName = FString(TEXT("/Game/")) / Folder / Name;
		ObjectPath = PackageName + TEXT(".") + Name;
		ClassPath = PackageName + TEXT(".") + Name + TEXT("_C");
		PackageFile = FPaths::ConvertRelativePathToFull(
			FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()));
		SidecarPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectContentDir(), Folder, Name + TEXT(".assetdoc.json")));
	}

	UBlueprint* Get() const { return Cast<UBlueprint>(FSoftObjectPath(ObjectPath).ResolveObject()); }
};

struct FFixture
{
	FAutomationTestBase& Test;
	const FString Folder;
	TArray<TUniquePtr<FAsset>> Assets;

	explicit FFixture(FAutomationTestBase& InTest)
		: Test(InTest)
		, Folder(FString(TEXT("AssetDocumentP65/")) + FGuid::NewGuid().ToString(EGuidFormats::Digits))
	{
	}

	const FAsset& Add(const TCHAR* Name)
	{
		return *Assets.Add_GetRef(MakeUnique<FAsset>(Folder, Name));
	}

	~FFixture()
	{
		TArray<UPackage*> Packages;
		for (int32 Index = Assets.Num() - 1; Index >= 0; --Index)
		{
			if (UPackage* Package = FindPackage(nullptr, *Assets[Index]->PackageName))
			{
				Package->SetDirtyFlag(false);
				Packages.Add(Package);
			}
		}
		if (!Packages.IsEmpty())
		{
			UPackageTools::UnloadPackages(Packages);
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		TArray<FString> Files;
		for (const TUniquePtr<FAsset>& Asset : Assets)
		{
			IFileManager::Get().Delete(*Asset->PackageFile, false, true);
			IFileManager::Get().Delete(*Asset->SidecarPath, false, true);
			Files.Add(Asset->PackageFile);
		}
		FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles(Files);
		IFileManager::Get().DeleteDirectory(*FPaths::Combine(FPaths::ProjectContentDir(), Folder), false, true);
	}

	FAssetDocumentResult Apply(const FAsset& Asset, const FString& Json, const FString& Stage) const
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Asset.SidecarPath), true);
		FFileHelper::SaveStringToFile(Json, *Asset.SidecarPath);
		FAssetDocumentApplyFileRequest Request;
		Request.FilePath = Asset.SidecarPath;
		Request.bSaveAsset = true;
		const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
		Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *Result.Message));
		for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
		{
			Test.AddInfo(FString::Printf(TEXT("%s: [%s] %s %s %s"), *Stage, *Diagnostic.Severity, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
		}
		return Result;
	}

	bool ApplyOk(const FAsset& Asset, const FString& Json, const FString& Stage) const
	{
		return Test.TestTrue(*FString::Printf(TEXT("%s: applies"), *Stage), Apply(Asset, Json, Stage).IsSuccess());
	}

	// Unloads the assets and loads them back from disk (an editor restart).
	bool Reload() const
	{
		TArray<UPackage*> Packages;
		for (int32 Index = Assets.Num() - 1; Index >= 0; --Index)
		{
			if (UBlueprint* Blueprint = Assets[Index]->Get())
			{
				if (!Test.TestFalse(*FString::Printf(TEXT("%s is saved before the reload"), *Assets[Index]->Name), Blueprint->GetOutermost()->IsDirty()))
				{
					return false;
				}
				Packages.Add(Blueprint->GetOutermost());
			}
		}
		UPackageTools::UnloadPackages(Packages);
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		for (const TUniquePtr<FAsset>& Asset : Assets)
		{
			if (!Test.TestNotNull(*FString::Printf(TEXT("%s loads back"), *Asset->Name), LoadObject<UBlueprint>(nullptr, *Asset->ObjectPath)))
			{
				return false;
			}
		}
		return true;
	}

	TSharedPtr<FJsonObject> Extract(const FAsset& Asset, const FString& Stage) const
	{
		FAssetDocumentExtractRequest Request;
		Request.AssetPath = Asset.ObjectPath;
		const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Extract(Request);
		if (!Test.TestTrue(*FString::Printf(TEXT("%s: extract"), *Stage), Result.IsSuccess() && Result.Payload.IsValid()))
		{
			Test.AddInfo(Result.Message);
			return nullptr;
		}
		return Result.Payload;
	}

	// The extracted document applied back (re-applying what the asset holds is a valid document).
	void ReapplyExtract(const FAsset& Asset, const FString& Stage) const
	{
		const TSharedPtr<FJsonObject> Extracted = Extract(Asset, Stage);
		if (!Extracted.IsValid())
		{
			return;
		}
		Extracted->SetStringField(TEXT("Action"), TEXT("Update"));
		FString Text;
		const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Extracted.ToSharedRef(), Writer);
		ApplyOk(Asset, Text, Stage);
	}
};

FString Document(const FAsset& Asset, const TCHAR* Action, const FString& Body)
{
	return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"%s\",\"Body\":{%s}}"),
		*Asset.PackageName, Action, *Body);
}

FString Serialize(const TSharedPtr<FJsonValue>& Value)
{
	FString Text;
	const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	FJsonSerializer::Serialize(Value, TEXT(""), Writer);
	return Text;
}

// The B pin of the event graph's comparison node whose function name contains Operator.
UEdGraphPin* FindComparisonPin(const UBlueprint* Blueprint, const TCHAR* Operator)
{
	for (const UEdGraph* Graph : Blueprint ? Blueprint->UbergraphPages : TArray<TObjectPtr<UEdGraph>>())
	{
		for (UEdGraphNode* Node : Graph ? Graph->Nodes : TArray<TObjectPtr<UEdGraphNode>>())
		{
			const UK2Node_PromotableOperator* Operation = Cast<UK2Node_PromotableOperator>(Node);
			if (Operation != nullptr && Operation->GetTargetFunction() != nullptr
				&& Operation->GetTargetFunction()->GetName().Contains(Operator))
			{
				return Node->FindPin(TEXT("B"), EGPD_Input);
			}
		}
	}
	return nullptr;
}

int32 CountParentCalls(const UBlueprint* Blueprint)
{
	int32 Count = 0;
	for (const UEdGraph* Graph : Blueprint ? Blueprint->UbergraphPages : TArray<TObjectPtr<UEdGraph>>())
	{
		for (const UEdGraphNode* Node : Graph ? Graph->Nodes : TArray<TObjectPtr<UEdGraphNode>>())
		{
			Count += Node != nullptr && Node->IsA<UK2Node_CallParentFunction>() && !Node->IsAutomaticallyPlacedGhostNode() ? 1 : 0;
		}
	}
	return Count;
}
} // namespace AssetDocumentVerificationSemanticsTests


// K2:Greater / K2:LessEqual with B "0.0" / "0" (the pin defaults), re-applied with an unrelated change.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentVerificationComparisonPinDefaultTest,
	"UEAssetDoc.Blueprint.Verification.ComparisonPinDefault",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentVerificationComparisonPinDefaultTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentVerificationSemanticsTests;
	(void)Parameters;
	FFixture Fixture(*this);
	const FAsset& Asset = Fixture.Add(TEXT("BP_Compare"));
	auto Make = [&Asset](const TCHAR* Action, int32 Revision)
	{
		return Document(Asset, Action, FString::Printf(TEXT(R"json(
"ParentClass": "/Script/Engine.Actor",
"Variables": [{"Name": "Speed", "Type": "float", "DefaultValue": %d.5}, {"Name": "Count", "Type": "int", "DefaultValue": 2}],
"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
  {"Id": "Begin", "Action": "K2:BeginPlay"},
  {"Id": "GetSpeed", "Action": "K2:Get:Speed"},
  {"Id": "GetCount", "Action": "K2:Get:Count"},
  {"Id": "Lunging", "Action": "K2:Greater", "PinOverrides": [{"Pin": "B", "DefaultValue": "0.0"}]},
  {"Id": "Few", "Action": "K2:LessEqual", "PinOverrides": [{"Pin": "B", "DefaultValue": "0"}]},
  {"Id": "Both", "Action": "K2:CallFunction:/Script/Engine.KismetMathLibrary:BooleanAND"},
  {"Id": "Br", "Action": "K2:Branch"}],
 "Links": [
  {"From": "Begin:then", "To": "Br:execute"},
  {"From": "GetSpeed:Speed", "To": "Lunging:A"},
  {"From": "GetCount:Count", "To": "Few:A"},
  {"From": "Lunging:ReturnValue", "To": "Both:A"},
  {"From": "Few:ReturnValue", "To": "Both:B"},
  {"From": "Both:ReturnValue", "To": "Br:Condition"}]}])json"), Revision));
	};
	if (!Fixture.ApplyOk(Asset, Make(TEXT("Create"), 1), TEXT("create")))
	{
		return false;
	}
	Fixture.ApplyOk(Asset, Make(TEXT("Update"), 2), TEXT("second apply in the session"));
	Fixture.ApplyOk(Asset, Make(TEXT("Update"), 3), TEXT("third apply in the session"));
	Fixture.ApplyOk(Asset, Make(TEXT("Update"), 3), TEXT("same document again"));
	if (Fixture.Reload())
	{
		Fixture.ApplyOk(Asset, Make(TEXT("Update"), 4), TEXT("apply after a reload"));
		Fixture.ApplyOk(Asset, Make(TEXT("Update"), 5), TEXT("second apply after a reload"));
	}
	// A promotable operator's reconstruct can leave a pin holding its default value with no text (K2 reads an
	// empty real pin as 0.0): the document's "0.0" is that value, not a change, and verifies.
	UEdGraphPin* Pin = FindComparisonPin(Asset.Get(), TEXT("Greater"));
	if (TestNotNull(TEXT("Lunging's B pin"), Pin))
	{
		AddInfo(FString::Printf(TEXT("Lunging B: DefaultValue '%s', AutogeneratedDefaultValue '%s'"), *Pin->DefaultValue,
			*Pin->AutogeneratedDefaultValue));
		Pin->DefaultValue.Empty();
		TestTrue(TEXT("the pin without text matches its autogenerated default"), Pin->DoesDefaultValueMatchAutogenerated());
		TestTrue(TEXT("saved"), UEditorLoadingAndSavingUtils::SavePackages({Asset.Get()->GetOutermost()}, false));
		const FAssetDocumentResult Same = Fixture.Apply(Asset, Make(TEXT("Update"), 5), TEXT("same document over a pin without text"));
		TestTrue(TEXT("same document over a pin without text: applies"), Same.IsSuccess());
		TestFalse(TEXT("same document over a pin without text: nothing to save"), Same.bSavedAsset);
		Pin = FindComparisonPin(Asset.Get(), TEXT("Greater"));
		if (Pin != nullptr)
		{
			Pin->DefaultValue.Empty();
			TestTrue(TEXT("saved again"), UEditorLoadingAndSavingUtils::SavePackages({Asset.Get()->GetOutermost()}, false));
		}
		Fixture.ApplyOk(Asset, Make(TEXT("Update"), 6), TEXT("unrelated change over a pin without text"));
	}
	Fixture.ReapplyExtract(Asset, TEXT("extract re-applied"));
	return !HasAnyErrors();
}

// A function input Default "1.0" (and "3" on an int) is stored in the editor's form.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentVerificationFunctionInputDefaultTest,
	"UEAssetDoc.Blueprint.Verification.FunctionInputDefault",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentVerificationFunctionInputDefaultTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentVerificationSemanticsTests;
	(void)Parameters;
	FFixture Fixture(*this);
	const FAsset& Asset = Fixture.Add(TEXT("BP_FunctionDefault"));
	auto Make = [&Asset](const TCHAR* Action, int32 Revision)
	{
		return Document(Asset, Action, FString::Printf(TEXT(R"json(
"ParentClass": "/Script/Engine.Actor",
"Variables": [{"Name": "Revision", "Type": "int", "DefaultValue": %d}],
"FunctionGraphs": [{"Name": "ExecuteMove",
  "Signature": {"Inputs": [{"Name": "DamageBonus", "Type": "float", "Default": "1.0"}, {"Name": "Hits", "Type": "int", "Default": "3"},
                           {"Name": "Scale", "Type": "float", "Default": "0.8"}, {"Name": "Bold", "Type": "bool", "Default": "true"}]},
  "Nodes": [{"Id": "Print", "Action": "K2:PrintString"}],
  "Links": [{"From": "Entry:then", "To": "Print:execute"}]}])json"), Revision));
	};
	if (!Fixture.ApplyOk(Asset, Make(TEXT("Create"), 1), TEXT("create")))
	{
		return false;
	}
	Fixture.ApplyOk(Asset, Make(TEXT("Update"), 2), TEXT("update"));
	if (Fixture.Reload())
	{
		Fixture.ApplyOk(Asset, Make(TEXT("Update"), 3), TEXT("update after a reload"));
	}
	Fixture.ReapplyExtract(Asset, TEXT("extract re-applied"));
	return !HasAnyErrors();
}

// "Inputs": [] on a custom event is the same as no Inputs.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentVerificationCustomEventEmptyInputsTest,
	"UEAssetDoc.Blueprint.Verification.CustomEventEmptyInputs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentVerificationCustomEventEmptyInputsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentVerificationSemanticsTests;
	(void)Parameters;
	FFixture Fixture(*this);
	const FAsset& Asset = Fixture.Add(TEXT("BP_CustomEvent"));
	auto Make = [&Asset](const TCHAR* Action, int32 Revision)
	{
		return Document(Asset, Action, FString::Printf(TEXT(R"json(
"ParentClass": "/Script/Engine.Actor",
"Variables": [{"Name": "Revision", "Type": "int", "DefaultValue": %d}],
"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
  {"Id": "EvStart", "Action": "K2:CustomEvent", "Name": "EvStart", "Inputs": []},
  {"Id": "EvHit", "Action": "K2:CustomEvent", "Name": "EvHit", "Inputs": [{"Name": "Amount", "Type": "float"}]},
  {"Id": "Print", "Action": "K2:PrintString"}],
 "Links": [{"From": "EvStart:then", "To": "Print:execute"}]}])json"), Revision));
	};
	if (!Fixture.ApplyOk(Asset, Make(TEXT("Create"), 1), TEXT("create")))
	{
		return false;
	}
	Fixture.ApplyOk(Asset, Make(TEXT("Update"), 2), TEXT("update"));
	Fixture.ReapplyExtract(Asset, TEXT("extract re-applied"));
	return !HasAnyErrors();
}

// 3: PinOverrides on a further return node are written and read back.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentVerificationExtraReturnNodeTest,
	"UEAssetDoc.Blueprint.Verification.ExtraReturnNode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentVerificationExtraReturnNodeTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentVerificationSemanticsTests;
	(void)Parameters;
	FFixture Fixture(*this);
	const FAsset& Asset = Fixture.Add(TEXT("BP_ExtraReturn"));
	auto Make = [&Asset](const TCHAR* Action, int32 Revision)
	{
		return Document(Asset, Action, FString::Printf(TEXT(R"json(
"ParentClass": "/Script/Engine.Actor",
"Variables": [{"Name": "Revision", "Type": "int", "DefaultValue": %d}],
"FunctionGraphs": [{"Name": "ApplyDamage",
  "Signature": {"Inputs": [{"Name": "Amount", "Type": "float"}], "Outputs": [{"Name": "PoiseBroken", "Type": "bool"}, {"Name": "Left", "Type": "float"}]},
  "Nodes": [
    {"Id": "Big", "Action": "K2:Greater", "PinOverrides": [{"Pin": "B", "DefaultValue": "50.0"}]},
    {"Id": "Br", "Action": "K2:Branch"},
    {"Id": "ResultBroken", "Implicit": "result", "PinOverrides": [{"Pin": "PoiseBroken", "DefaultValue": "true"}, {"Pin": "Left", "DefaultValue": "0.25"}]}],
  "Links": [
    {"From": "Entry:then", "To": "Br:execute"},
    {"From": "Entry:Amount", "To": "Big:A"},
    {"From": "Big:ReturnValue", "To": "Br:Condition"},
    {"From": "Br:then", "To": "ResultBroken:execute"},
    {"From": "Br:else", "To": "Result:execute"}]}])json"), Revision));
	};
	if (!Fixture.ApplyOk(Asset, Make(TEXT("Create"), 1), TEXT("create")))
	{
		return false;
	}
	Fixture.ApplyOk(Asset, Make(TEXT("Update"), 2), TEXT("update"));
	Fixture.ReapplyExtract(Asset, TEXT("extract re-applied"));
	return !HasAnyErrors();
}

// Components matched by Name (listed children first, a new one inserted at the front), defaults stated,
// AttachTo naming the Character's capsule by its subobject name.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentVerificationComponentsByNameTest,
	"UEAssetDoc.Blueprint.Verification.ComponentsByName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentVerificationComponentsByNameTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentVerificationSemanticsTests;
	(void)Parameters;
	FFixture Fixture(*this);
	const FAsset& Asset = Fixture.Add(TEXT("BP_Components"));
	auto Make = [&Asset](const TCHAR* Action, bool bExtra, const TCHAR* Capsule)
	{
		const FString Extra = bExtra ? FString(TEXT(R"json({"Name": "Momentum", "Class": "/Script/Engine.SceneComponent", "AttachTo": "CameraBoom"},)json")) : FString();
		return Document(Asset, Action, FString::Printf(TEXT(R"json(
"ParentClass": "/Script/Engine.Character",
"Components": [%s
  {"Name": "FollowCamera", "Class": "/Script/Engine.CameraComponent", "AttachTo": "CameraBoom", "Properties": {"FieldOfView": 90.0, "bUsePawnControlRotation": false}},
  {"Name": "CameraBoom", "Class": "/Script/Engine.SpringArmComponent", "AttachTo": "%s", "Properties": {"TargetArmLength": 350.8, "bUsePawnControlRotation": true}},
  {"Name": "PP_JR", "Class": "/Script/Engine.PostProcessComponent", "AttachTo": "%s", "Properties": {"bUnbound": true, "BlendWeight": 0.8}},
  {"Name": "PP_Bullet", "Class": "/Script/Engine.PostProcessComponent", "Properties": {"bUnbound": true, "Priority": 1.0}}]
)json"), *Extra, Capsule, Capsule));
	};
	if (!Fixture.ApplyOk(Asset, Make(TEXT("Create"), false, TEXT("CollisionCylinder")), TEXT("create (AttachTo the capsule's subobject name)")))
	{
		return false;
	}
	Fixture.ApplyOk(Asset, Make(TEXT("Update"), true, TEXT("CollisionCylinder")), TEXT("a new component listed first"));
	Fixture.ApplyOk(Asset, Make(TEXT("Update"), true, TEXT("CapsuleComponent")), TEXT("AttachTo the capsule's variable name"));
	if (const UBlueprint* Blueprint = Asset.Get())
	{
		const USCS_Node* Boom = Blueprint->SimpleConstructionScript ? Blueprint->SimpleConstructionScript->FindSCSNode(TEXT("CameraBoom")) : nullptr;
		// The engine keeps a native parent by its subobject name (SCS_Node::SetParent).
		TestTrue(TEXT("CameraBoom attaches to the capsule"), Boom != nullptr && Boom->bIsParentComponentNative
			&& Boom->ParentComponentOrVariableName == TEXT("CollisionCylinder"));
	}
	Fixture.ReapplyExtract(Asset, TEXT("extract re-applied"));
	return !HasAnyErrors();
}

// A child Blueprint overrides Tick, which its Blueprint parent implements.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentVerificationParentCallOnOverrideTest,
	"UEAssetDoc.Blueprint.Verification.ParentCallOnOverride",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentVerificationParentCallOnOverrideTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentVerificationSemanticsTests;
	(void)Parameters;
	FFixture Fixture(*this);
	const FAsset& Parent = Fixture.Add(TEXT("BP_Base"));
	const FAsset& Child = Fixture.Add(TEXT("BP_Boss"));
	const FAsset& Other = Fixture.Add(TEXT("BP_Dummy"));
	const FString ParentBody = TEXT(R"json(
"ParentClass": "/Script/Engine.Actor",
"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
  {"Id": "Tick", "Action": "K2:Event:/Script/Engine.Actor:ReceiveTick"},
  {"Id": "Print", "Action": "K2:PrintString", "PinOverrides": [{"Pin": "InString", "DefaultValue": "base"}]}],
 "Links": [{"From": "Tick:then", "To": "Print:execute"}]}])json");
	if (!Fixture.ApplyOk(Parent, Document(Parent, TEXT("Create"), ParentBody), TEXT("create the parent")))
	{
		return false;
	}
	const FString ChildEmpty = FString::Printf(TEXT("\"ParentClass\": \"%s\""), *Parent.ClassPath);
	if (!Fixture.ApplyOk(Child, Document(Child, TEXT("Create"), ChildEmpty), TEXT("create the child"))
		|| !Fixture.ApplyOk(Other, Document(Other, TEXT("Create"), ChildEmpty), TEXT("create the second child")))
	{
		return false;
	}
	// The document lists the override only: the graph holds what it lists.
	const FString Override = FString::Printf(TEXT(R"json("ParentClass": "%s",
"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
  {"Id": "Tick", "Action": "K2:Event:/Script/Engine.Actor:ReceiveTick"},
  {"Id": "Print", "Action": "K2:PrintString", "PinOverrides": [{"Pin": "InString", "DefaultValue": "boss"}]}],
 "Links": [{"From": "Tick:then", "To": "Print:execute"}]}])json"), *Parent.ClassPath);
	if (Fixture.ApplyOk(Child, Document(Child, TEXT("Update"), Override), TEXT("override Tick without the parent call")))
	{
		TestEqual(TEXT("no parent call the document does not list"), CountParentCalls(Child.Get()), 0);
		Fixture.ApplyOk(Child, Document(Child, TEXT("Update"), Override), TEXT("same document again"));
	}
	// The document lists the parent call: it is kept and wired as written.
	const FString WithParent = FString::Printf(TEXT(R"json("ParentClass": "%s",
"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
  {"Id": "Tick", "Action": "K2:Event:/Script/Engine.Actor:ReceiveTick"},
  {"Id": "Super", "Action": "K2:CallParentFunction:%s:ReceiveTick"},
  {"Id": "Print", "Action": "K2:PrintString", "PinOverrides": [{"Pin": "InString", "DefaultValue": "dummy"}]}],
 "Links": [{"From": "Tick:then", "To": "Super:execute"}, {"From": "Tick:DeltaSeconds", "To": "Super:DeltaSeconds"},
           {"From": "Super:then", "To": "Print:execute"}]}])json"), *Parent.ClassPath, *Parent.ClassPath);
	if (Fixture.ApplyOk(Other, Document(Other, TEXT("Update"), WithParent), TEXT("override Tick with the parent call")))
	{
		TestEqual(TEXT("the listed parent call"), CountParentCalls(Other.Get()), 1);
	}
	if (Fixture.Reload())
	{
		Fixture.ReapplyExtract(Child, TEXT("child extract re-applied"));
		Fixture.ReapplyExtract(Other, TEXT("second child extract re-applied"));
	}
	return !HasAnyErrors();
}

// One apply removes a function input and the link from it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentVerificationRemoveInputWithLinkTest,
	"UEAssetDoc.Blueprint.Verification.RemoveInputWithLink",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentVerificationRemoveInputWithLinkTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentVerificationSemanticsTests;
	(void)Parameters;
	FFixture Fixture(*this);
	const FAsset& Asset = Fixture.Add(TEXT("BP_HitFeel"));
	auto Make = [&Asset](const TCHAR* Action, bool bWithRealNow)
	{
		const FString Inputs = bWithRealNow ? FString(TEXT(R"json([{"Name": "RealNow", "Type": "float"}, {"Name": "Alpha", "Type": "float"}])json"))
			: FString(TEXT(R"json([{"Name": "Alpha", "Type": "float"}])json"));
		const FString Links = bWithRealNow ? FString(TEXT(R"json(,{"From": "Entry:RealNow", "To": "SetLast:LastTime"})json")) : FString();
		return Document(Asset, Action, FString::Printf(TEXT(R"json(
"ParentClass": "/Script/Engine.Actor",
"Variables": [{"Name": "LastTime", "Type": "float"}, {"Name": "Fov", "Type": "float"}],
"FunctionGraphs": [{"Name": "UpdateChargeFOV", "Signature": {"Inputs": %s},
  "Nodes": [{"Id": "SetLast", "Action": "K2:Set:LastTime"}, {"Id": "SetFov", "Action": "K2:Set:Fov"}],
  "Links": [{"From": "Entry:then", "To": "SetLast:execute"}, {"From": "SetLast:then", "To": "SetFov:execute"},
            {"From": "Entry:Alpha", "To": "SetFov:Fov"}%s]}])json"), *Inputs, *Links));
	};
	if (!Fixture.ApplyOk(Asset, Make(TEXT("Create"), true), TEXT("create")))
	{
		return false;
	}
	Fixture.ApplyOk(Asset, Make(TEXT("Update"), false), TEXT("remove the input and its link"));
	Fixture.ReapplyExtract(Asset, TEXT("extract re-applied"));
	return !HasAnyErrors();
}

// An array of structs with maps (object keys, name keys) and floats in a component's Properties.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentVerificationStructArrayMapsTest,
	"UEAssetDoc.Blueprint.Verification.StructArrayMaps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentVerificationStructArrayMapsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentVerificationSemanticsTests;
	(void)Parameters;
	FFixture Fixture(*this);
	const FAsset& Asset = Fixture.Add(TEXT("BP_Palette"));
	const FString Component = UAssetDocumentVerificationPaletteComponent::StaticClass()->GetPathName();
	auto Make = [&Asset, &Component](const TCHAR* Action, const FString& Palettes)
	{
		return Document(Asset, Action, FString::Printf(TEXT(R"json(
"ParentClass": "/Script/Engine.Actor",
"Components": [
  {"Name": "Root", "Class": "/Script/Engine.SceneComponent"},
  {"Name": "Combat", "Class": "%s", "Properties": {"ChargeFullScale": 1.3, "Palettes": %s}}])json"), *Component, *Palettes));
	};
	// The map in the text form the editor's copy writes.
	const FString TextForm = TEXT(R"json([
  {"Stance": "Low", "BufferTolerance": 0.8, "Swap": "((/Engine/BasicShapes/Cube.Cube, /Engine/BasicShapes/Sphere.Sphere))", "Weights": "((Light, 0.25),(Heavy, 1.5))"},
  {"Stance": "High", "BufferTolerance": 0.3}])json");
	if (!Fixture.ApplyOk(Asset, Make(TEXT("Create"), TextForm), TEXT("create (maps as text)")))
	{
		return false;
	}
	Fixture.ApplyOk(Asset, Make(TEXT("Update"), TextForm), TEXT("same document again"));
	// The map as a JSON object (validate accepts it).
	const FString ObjectForm = TEXT(R"json([
  {"Stance": "Low", "BufferTolerance": 0.8, "Swap": {"/Engine/BasicShapes/Cube.Cube": "/Engine/BasicShapes/Cone.Cone"}, "Weights": {"Light": 0.5}},
  {"Stance": "High", "BufferTolerance": 0.3}])json");
	if (Fixture.ApplyOk(Asset, Make(TEXT("Update"), ObjectForm), TEXT("maps as JSON objects")))
	{
		const UBlueprint* Blueprint = Asset.Get();
		const USCS_Node* Node = Blueprint && Blueprint->SimpleConstructionScript ? Blueprint->SimpleConstructionScript->FindSCSNode(TEXT("Combat")) : nullptr;
		const UAssetDocumentVerificationPaletteComponent* Template = Node ? Cast<UAssetDocumentVerificationPaletteComponent>(Node->ComponentTemplate) : nullptr;
		if (TestNotNull(TEXT("Combat template"), Template) && TestEqual(TEXT("two palettes"), Template->Palettes.Num(), 2))
		{
			const FAssetDocumentVerificationPalette& Low = Template->Palettes[0];
			TestEqual(TEXT("Swap entries"), Low.Swap.Num(), 1);
			const TObjectPtr<UStaticMesh>* Swapped = Low.Swap.Num() == 1 ? &Low.Swap.CreateConstIterator()->Value : nullptr;
			TestTrue(TEXT("Swap value"), Swapped != nullptr && *Swapped != nullptr && (*Swapped)->GetName() == TEXT("Cone"));
			TestTrue(TEXT("Weights"), Low.Weights.Num() == 1 && Low.Weights.Contains(TEXT("Light")) && FMath::IsNearlyEqual(Low.Weights[TEXT("Light")], 0.5f));
		}
	}
	Fixture.ReapplyExtract(Asset, TEXT("extract re-applied"));
	if (const TSharedPtr<FJsonObject> Extracted = Fixture.Extract(Asset, TEXT("extract")))
	{
		AddInfo(FString::Printf(TEXT("extracted Body: %s"), *Serialize(Extracted->TryGetField(TEXT("Body"))).Left(3000)));
	}
	return !HasAnyErrors();
}
#endif
