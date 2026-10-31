// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Widget tree restructuring through the production Service and the Python Profile: an existing widget
// that one apply takes out of the tree (its parent goes away, changes class, or its single-content /
// root place is taken by a new container) and puts back elsewhere.
//
// UEAssetDoc.WidgetBlueprint.TreeMove.Shapes: from Root(SizeBox) -> Wrap(Overlay) -> [ChildA, ChildB],
// each of these applies, extracts to the requested tree, re-applies with no change, and applies back:
// Wrap replaced by a new VerticalBox holding the children in either order, Root holding ChildA alone,
// Wrap wrapped in a new container under Root, Wrap changing class with its children kept, and the root
// itself wrapped in a new container.
//
// UEAssetDoc.WidgetBlueprint.TreeMove.NamedSlots: the same for a widget in a named slot of a
// User Widget host and in a named slot inherited from the parent Widget Blueprint, wrapped in a new
// container in that slot.
//
// UEAssetDoc.WidgetBlueprint.TreeMove.Identity: moved widgets stay the same objects with the same
// variable GUIDs, keep their property, binding, bound event and slot values, and the Blueprint compiles.
//
// UEAssetDoc.WidgetBlueprint.TreeMove.Rollback: an apply that restructures the tree and then fails
// later in the same document leaves the asset as it was.
//
// UEAssetDoc.WidgetBlueprint.TreeMove.Detached: straight through the umg.tree.* handlers, a detached
// widget (with its children) stays in the Blueprint with its variable GUIDs until it is placed again,
// is listed as detached by umg.tree.query.v1, and removing it leaves the Blueprint as a plain remove does
// (variable GUIDs, bindings, Designer tree, the widget's bound event).

#include "AssetDocumentGraphValue.h"
#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "AssetDocumentWidgetPrimitives.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/PanelWidget.h"
#include "Components/ProgressBar.h"
#include "Components/VerticalBoxSlot.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "HAL/FileManager.h"
#include "K2Node_ComponentBoundEvent.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GarbageCollection.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"
#include "WidgetBlueprint.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentWidgetTreeMoveTests
{
const TCHAR* Folder = TEXT("/Game/AssetDocumentWidgetTreeMove");

struct FTarget
{
	FString AssetName;
	FString PackageName;
	FString ObjectPath;
	FString PackageFile;
	FString SidecarPath;

	explicit FTarget(const FString& Prefix)
		: AssetName(FString::Printf(TEXT("%s_%s"), *Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits)))
	{
		PackageName = FString(Folder) / AssetName;
		ObjectPath = PackageName + TEXT(".") + AssetName;
		PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		SidecarPath = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("AssetDocumentWidgetTreeMove"), AssetName + TEXT(".assetdoc.json"));
	}

	UWidgetBlueprint* Get() const { return Cast<UWidgetBlueprint>(FSoftObjectPath(ObjectPath).ResolveObject()); }

	void Cleanup() const
	{
		if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting && !GEditor->IsTransactionActive()
			&& GEditor->Trans->GetQueueLength() > 0)
		{
			GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentWidgetTreeMoveTests", "Cleanup", "AssetDocument widget tree move test cleanup"));
		}
		if (UWidgetBlueprint* Blueprint = Get())
		{
			if (IFileManager::Get().FileExists(*PackageFile))
			{
				UPackageTools::UnloadPackages({Blueprint->GetOutermost()});
			}
			else
			{
				ObjectTools::DeleteObjects({Blueprint}, false, ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
			}
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		if (IFileManager::Get().FileExists(*PackageFile))
		{
			IFileManager::Get().Delete(*PackageFile, false, true);
			FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({PackageFile});
		}
		IFileManager::Get().Delete(*SidecarPath, false, true);
	}
};

FString Serialize(const TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	if (Object.IsValid())
	{
		const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}
	return Text;
}

TSharedPtr<FJsonObject> Parse(const FString& Json)
{
	TSharedPtr<FJsonObject> Object;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object);
	return Object;
}

FString Document(const FTarget& Target, const FString& Body, const FString& ParentClass = TEXT("/Script/UMG.UserWidget"))
{
	return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/UMGEditor.WidgetBlueprint\",")
		TEXT("\"Action\":\"CreateOrUpdate\",\"Body\":{\"ParentClass\":\"%s\",%s}}"), *Target.PackageName, *ParentClass, *Body);
}

FString TreeBody(const TCHAR* RootWidget)
{
	return FString::Printf(TEXT("\"WidgetTree\":{\"RootWidget\":%s}"), RootWidget);
}

FAssetDocumentResult Apply(FAutomationTestBase& Test, const FTarget& Target, const FString& Json, const FString& Stage)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target.SidecarPath), true);
	FFileHelper::SaveStringToFile(Json, *Target.SidecarPath);
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Target.SidecarPath;
	Request.bSaveAsset = true;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
	if (!Result.IsSuccess())
	{
		Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *Result.Message));
		for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
		{
			Test.AddInfo(FString::Printf(TEXT("%s: [%s] %s %s"), *Stage, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
		}
	}
	return Result;
}

TSharedPtr<FJsonObject> Extract(FAutomationTestBase& Test, const FTarget& Target, const FString& Stage)
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = Target.ObjectPath;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Extract(Request);
	Test.TestTrue(*FString::Printf(TEXT("%s: extract succeeds"), *Stage), Result.IsSuccess() && Result.Payload.IsValid());
	return Result.Payload;
}

void TestNoDiff(FAutomationTestBase& Test, const FString& Json, const FString& Stage)
{
	FAssetDocumentDiffRequest Request;
	Request.Document = Parse(Json);
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Diff(Request);
	const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
	const bool bOk = Result.IsSuccess() && Result.Payload.IsValid() && Result.Payload->TryGetArrayField(TEXT("changed"), Changed);
	if (Test.TestTrue(*FString::Printf(TEXT("%s: diff succeeds"), *Stage), bOk)
		&& !Test.TestEqual(*FString::Printf(TEXT("%s: live asset equals the document"), *Stage), Changed->Num(), 0))
	{
		Test.AddInfo(Serialize(Result.Payload));
	}
}

// "Name:Class[children]" of an extracted (or authored) widget node, the class by its short name.
FString Shape(const TSharedPtr<FJsonObject>& Node)
{
	if (!Node.IsValid())
	{
		return TEXT("null");
	}
	FString Class = Node->GetStringField(TEXT("Class"));
	Class.Split(TEXT("."), nullptr, &Class, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	FString Out = Node->GetStringField(TEXT("Name")) + TEXT(":") + Class;
	const TSharedPtr<FJsonObject>* NamedSlots = nullptr;
	if (Node->TryGetObjectField(TEXT("NamedSlots"), NamedSlots) && !(*NamedSlots)->Values.IsEmpty())
	{
		TArray<FString> Keys;
		for (const auto& Entry : (*NamedSlots)->Values)
		{
			Keys.Add(FString(*Entry.Key));
		}
		Keys.Sort();
		TArray<FString> Parts;
		for (const FString& Key : Keys)
		{
			Parts.Add(Key + TEXT("=") + Shape((*NamedSlots)->GetObjectField(Key)));
		}
		Out += TEXT("{") + FString::Join(Parts, TEXT(",")) + TEXT("}");
	}
	const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
	if (Node->TryGetArrayField(TEXT("Children"), Children) && !Children->IsEmpty())
	{
		TArray<FString> Parts;
		for (const TSharedPtr<FJsonValue>& Child : *Children)
		{
			Parts.Add(Shape(Child->AsObject()));
		}
		Out += TEXT("[") + FString::Join(Parts, TEXT(",")) + TEXT("]");
	}
	return Out;
}

// The root and the inherited named slots of a WidgetTree object.
FString TreeShape(const TSharedPtr<FJsonObject>& Tree)
{
	if (!Tree.IsValid())
	{
		return TEXT("no tree");
	}
	const TSharedPtr<FJsonObject>* Root = nullptr;
	FString Out = TEXT("Root=") + (Tree->TryGetObjectField(TEXT("RootWidget"), Root) ? Shape(*Root) : FString(TEXT("null")));
	const TSharedPtr<FJsonObject>* Slots = nullptr;
	if (Tree->TryGetObjectField(TEXT("NamedSlotBindings"), Slots))
	{
		TArray<FString> Keys;
		for (const auto& Entry : (*Slots)->Values)
		{
			Keys.Add(FString(*Entry.Key));
		}
		Keys.Sort();
		for (const FString& Key : Keys)
		{
			Out += FString::Printf(TEXT(";%s="), *Key) + Shape((*Slots)->GetObjectField(Key));
		}
	}
	return Out;
}

FString ExtractedShape(FAutomationTestBase& Test, const FTarget& Target, const FString& Stage)
{
	const TSharedPtr<FJsonObject> Extracted = Extract(Test, Target, Stage);
	const TSharedPtr<FJsonObject>* Body = nullptr;
	const TSharedPtr<FJsonObject>* Tree = nullptr;
	if (Extracted.IsValid() && Extracted->TryGetObjectField(TEXT("Body"), Body) && (*Body)->TryGetObjectField(TEXT("WidgetTree"), Tree))
	{
		return TreeShape(*Tree);
	}
	return TEXT("no tree");
}

FString AuthoredShape(const FString& Json)
{
	return TreeShape(Parse(Json)->GetObjectField(TEXT("Body"))->GetObjectField(TEXT("WidgetTree")));
}

// Apply Json: it succeeds, the tree extracts to the authored one, diff is empty and a second apply of
// the same document changes nothing (no save, no compile).
bool ApplyAndCheck(FAutomationTestBase& Test, const FTarget& Target, const FString& Json, const FString& Stage)
{
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: applies"), *Stage), Apply(Test, Target, Json, Stage).IsSuccess()))
	{
		return false;
	}
	Test.TestEqual(*FString::Printf(TEXT("%s: extracts to the requested tree"), *Stage), ExtractedShape(Test, Target, Stage), AuthoredShape(Json));
	TestNoDiff(Test, Json, Stage);
	UWidgetBlueprint* Blueprint = Target.Get();
	int32 Compiles = 0;
	const FDelegateHandle Handle = Blueprint != nullptr
		? Blueprint->OnCompiled().AddLambda([&Compiles](UBlueprint*) { ++Compiles; }) : FDelegateHandle();
	const FAssetDocumentResult Again = Apply(Test, Target, Json, Stage + TEXT(" again"));
	if (Blueprint != nullptr)
	{
		Blueprint->OnCompiled().Remove(Handle);
	}
	Test.TestTrue(*FString::Printf(TEXT("%s: re-apply succeeds"), *Stage), Again.IsSuccess());
	Test.TestFalse(*FString::Printf(TEXT("%s: re-apply saves nothing"), *Stage), Again.bSavedAsset);
	Test.TestEqual(*FString::Printf(TEXT("%s: re-apply does not compile"), *Stage), Compiles, 0);
	return true;
}

const TCHAR* BaseRoot = TEXT(R"json({"Name":"Root","Class":"/Script/UMG.SizeBox","IsVariable":false,"Children":[
 {"Name":"Wrap","Class":"/Script/UMG.Overlay","IsVariable":false,"Children":[
  {"Name":"ChildA","Class":"/Script/UMG.SizeBox","IsVariable":false},
  {"Name":"ChildB","Class":"/Script/UMG.SizeBox","IsVariable":false}]}]})json");

struct FShapeCase
{
	const TCHAR* Name;
	const TCHAR* RootWidget;
};

const FShapeCase ShapeCases[] = {
	{TEXT("WrapReplacedSwapped"), TEXT(R"json({"Name":"Root","Class":"/Script/UMG.SizeBox","IsVariable":false,"Children":[
	 {"Name":"Stack","Class":"/Script/UMG.VerticalBox","IsVariable":false,"Children":[
	  {"Name":"ChildB","Class":"/Script/UMG.SizeBox","IsVariable":false},
	  {"Name":"ChildA","Class":"/Script/UMG.SizeBox","IsVariable":false}]}]})json")},
	{TEXT("WrapReplacedInOrder"), TEXT(R"json({"Name":"Root","Class":"/Script/UMG.SizeBox","IsVariable":false,"Children":[
	 {"Name":"Stack","Class":"/Script/UMG.VerticalBox","IsVariable":false,"Children":[
	  {"Name":"ChildA","Class":"/Script/UMG.SizeBox","IsVariable":false},
	  {"Name":"ChildB","Class":"/Script/UMG.SizeBox","IsVariable":false}]}]})json")},
	{TEXT("RootHoldsChildA"), TEXT(R"json({"Name":"Root","Class":"/Script/UMG.SizeBox","IsVariable":false,"Children":[
	 {"Name":"ChildA","Class":"/Script/UMG.SizeBox","IsVariable":false}]})json")},
	{TEXT("WrapWrapped"), TEXT(R"json({"Name":"Root","Class":"/Script/UMG.SizeBox","IsVariable":false,"Children":[
	 {"Name":"Stack","Class":"/Script/UMG.VerticalBox","IsVariable":false,"Children":[
	  {"Name":"Wrap","Class":"/Script/UMG.Overlay","IsVariable":false,"Children":[
	   {"Name":"ChildA","Class":"/Script/UMG.SizeBox","IsVariable":false},
	   {"Name":"ChildB","Class":"/Script/UMG.SizeBox","IsVariable":false}]}]}]})json")},
	{TEXT("WrapChangesClass"), TEXT(R"json({"Name":"Root","Class":"/Script/UMG.SizeBox","IsVariable":false,"Children":[
	 {"Name":"Wrap","Class":"/Script/UMG.VerticalBox","IsVariable":false,"Children":[
	  {"Name":"ChildA","Class":"/Script/UMG.SizeBox","IsVariable":false},
	  {"Name":"ChildB","Class":"/Script/UMG.SizeBox","IsVariable":false}]}]})json")},
	{TEXT("RootWrapped"), TEXT(R"json({"Name":"Outer","Class":"/Script/UMG.VerticalBox","IsVariable":false,"Children":[
	 {"Name":"Root","Class":"/Script/UMG.SizeBox","IsVariable":false,"Children":[
	  {"Name":"Wrap","Class":"/Script/UMG.Overlay","IsVariable":false,"Children":[
	   {"Name":"ChildA","Class":"/Script/UMG.SizeBox","IsVariable":false},
	   {"Name":"ChildB","Class":"/Script/UMG.SizeBox","IsVariable":false}]}]}]})json")},
};

// Variables, a property binding and a bound event the moved widgets carry.
const TCHAR* IdentityMembers = TEXT(R"json(
"Variables":[{"Name":"Health","Type":"float","DefaultValue":0.5}],
"Bindings":[{"Widget":"ChildA","Property":"Percent","Kind":"Property","SourcePath":["Health"]}],
"UbergraphPages":[{"Name":"EventGraph","Nodes":[
  {"Id":"Clicked","Action":"K2:ComponentBoundEvent:/Script/UMG.Button:OnClicked@ChildB"},
  {"Id":"Tell","Action":"K2:PrintString","PinOverrides":[{"Pin":"InString","DefaultValue":"go"}]}],
  "Links":[{"From":"Clicked:then","To":"Tell:execute"}]}])json");

const TCHAR* IdentityBaseRoot = TEXT(R"json({"Name":"Root","Class":"/Script/UMG.SizeBox","IsVariable":false,"Children":[
 {"Name":"Wrap","Class":"/Script/UMG.Overlay","IsVariable":false,"Children":[
  {"Name":"ChildA","Class":"/Script/UMG.ProgressBar","IsVariable":true,"Properties":{"Percent":0.25}},
  {"Name":"ChildB","Class":"/Script/UMG.Button","IsVariable":true,"Children":[
   {"Name":"Label","Class":"/Script/UMG.TextBlock","IsVariable":false,"Properties":{"Text":"Go"}}]}]}]})json");

// Wrap goes away; the children swap order in a new VerticalBox, ChildA centred by its new slot.
const TCHAR* IdentityMovedRoot = TEXT(R"json({"Name":"Root","Class":"/Script/UMG.SizeBox","IsVariable":false,"Children":[
 {"Name":"Stack","Class":"/Script/UMG.VerticalBox","IsVariable":false,"Children":[
  {"Name":"ChildB","Class":"/Script/UMG.Button","IsVariable":true,"Children":[
   {"Name":"Label","Class":"/Script/UMG.TextBlock","IsVariable":false,"Properties":{"Text":"Go"}}]},
  {"Name":"ChildA","Class":"/Script/UMG.ProgressBar","IsVariable":true,"Properties":{"Percent":0.25},
   "Slot":{"HorizontalAlignment":"HAlign_Center"}}]}]})json");

FString IdentityBody(const TCHAR* RootWidget)
{
	return FString(IdentityMembers) + TEXT(",") + TreeBody(RootWidget);
}

UWidget* FindWidget(const UWidgetBlueprint* Blueprint, const TCHAR* Name)
{
	return Blueprint != nullptr && Blueprint->WidgetTree != nullptr ? Blueprint->WidgetTree->FindWidget(FName(Name)) : nullptr;
}

bool HasBoundEvent(const UWidgetBlueprint* Blueprint, FName Widget)
{
	for (const UEdGraph* Graph : Blueprint != nullptr ? Blueprint->UbergraphPages : TArray<TObjectPtr<UEdGraph>>())
	{
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			const UK2Node_ComponentBoundEvent* Bound = Cast<UK2Node_ComponentBoundEvent>(Node);
			if (Bound != nullptr && Bound->ComponentPropertyName == Widget && Bound->DelegatePropertyName == TEXT("OnClicked"))
			{
				return true;
			}
		}
	}
	return false;
}

bool CompilesClean(const UBlueprint* Blueprint)
{
	return Blueprint != nullptr && (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
}

// One umg.* primitive straight through its handler, as the provider runs it inside an apply. Returns the result
// as JSON (null on failure, which is reported).
TSharedPtr<FJsonObject> Invoke(FAutomationTestBase& Test, UWidgetBlueprint& Blueprint, const TCHAR* Primitive,
	UE::AssetDocument::Graph::FObjectBuilder Args, bool bMutating = true)
{
	using namespace UE::AssetDocument::Graph;
	FHandler Handler = nullptr;
	for (const FBinding& Binding : Widget::GetBindings())
	{
		if (FCString::Strcmp(Binding.PrimitiveId, Primitive) == 0)
		{
			Handler = Binding.Handler;
		}
	}
	if (!Test.TestTrue(*FString::Printf(TEXT("%s is bound"), Primitive), Handler != nullptr))
	{
		return nullptr;
	}
	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = Primitive;
	Invocation.Version = 1;
	Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	Args.AddInt(TEXT("schema_version"), 1);
	Invocation.ValueArguments = Args.Build();
	FAssetDocumentGraphError Error;
	FCall Call(Invocation, Blueprint, Error);
	Call.bMutating = bMutating;
	FObjectBuilder Result;
	if (!Handler(Call, Result))
	{
		Test.AddError(FString::Printf(TEXT("%s on %s: [%s] %s"), Primitive, *Blueprint.GetName(), *Error.Code, *Error.Message));
		return nullptr;
	}
	const TSharedPtr<FJsonValue> Json = ValueToJson(Result.Build());
	return Json.IsValid() ? Json->AsObject() : nullptr;
}

TArray<FString> Detached(FAutomationTestBase& Test, UWidgetBlueprint& Blueprint)
{
	TArray<FString> Out;
	const TSharedPtr<FJsonObject> Query = Invoke(Test, Blueprint, TEXT("umg.tree.query.v1"), UE::AssetDocument::Graph::FObjectBuilder(), false);
	const TArray<TSharedPtr<FJsonValue>>* Names = nullptr;
	if (Query.IsValid() && Query->TryGetArrayField(TEXT("detached"), Names))
	{
		for (const TSharedPtr<FJsonValue>& Name : *Names)
		{
			Out.Add(Name->AsString());
		}
	}
	return Out;
}

// The extracted Designer state: widget tree, bindings and desired focus (graphs carry per-asset GUIDs).
FString ExtractedDesigner(FAutomationTestBase& Test, const FTarget& Target, const FString& Stage)
{
	const TSharedPtr<FJsonObject> Extracted = Extract(Test, Target, Stage);
	const TSharedPtr<FJsonObject>* Body = nullptr;
	if (!Extracted.IsValid() || !Extracted->TryGetObjectField(TEXT("Body"), Body))
	{
		return FString();
	}
	const TSharedPtr<FJsonObject> Designer = MakeShared<FJsonObject>();
	for (const TCHAR* Key : {TEXT("WidgetTree"), TEXT("Bindings"), TEXT("DesiredFocus")})
	{
		if (const TSharedPtr<FJsonValue> Value = (*Body)->TryGetField(Key))
		{
			Designer->SetField(FString(Key), Value);
		}
	}
	return Serialize(Designer);
}

TArray<FName> GuidNames(const UWidgetBlueprint* Blueprint)
{
	TArray<FName> Names;
	if (Blueprint != nullptr)
	{
		Blueprint->WidgetVariableNameToGuidMap.GetKeys(Names);
	}
	Names.Sort(FNameLexicalLess());
	return Names;
}
} // namespace AssetDocumentWidgetTreeMoveTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentWidgetTreeMoveShapesTest, "UEAssetDoc.WidgetBlueprint.TreeMove.Shapes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentWidgetTreeMoveShapesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentWidgetTreeMoveTests;
	for (const FShapeCase& Case : ShapeCases)
	{
		const FTarget Target(FString(TEXT("WBP_Move")) + Case.Name);
		ON_SCOPE_EXIT { Target.Cleanup(); };
		const FString Base = Document(Target, TreeBody(BaseRoot));
		if (!TestTrue(*FString::Printf(TEXT("%s: base creates"), Case.Name), Apply(*this, Target, Base, Case.Name).IsSuccess()))
		{
			continue;
		}
		// Forward: the case's tree. Back: the base tree again (the reverse restructuring).
		const FString Moved = Document(Target, TreeBody(Case.RootWidget));
		if (ApplyAndCheck(*this, Target, Moved, Case.Name))
		{
			TestTrue(*FString::Printf(TEXT("%s: compiles"), Case.Name), CompilesClean(Target.Get()));
			ApplyAndCheck(*this, Target, Base, FString(Case.Name) + TEXT(" back"));
			TestTrue(*FString::Printf(TEXT("%s back: compiles"), Case.Name), CompilesClean(Target.Get()));
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentWidgetTreeMoveIdentityTest, "UEAssetDoc.WidgetBlueprint.TreeMove.Identity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentWidgetTreeMoveIdentityTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentWidgetTreeMoveTests;
	const FTarget Target(TEXT("WBP_MoveIdentity"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	const FString Base = Document(Target, IdentityBody(IdentityBaseRoot));
	if (!TestTrue(TEXT("base creates"), Apply(*this, Target, Base, TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	UWidgetBlueprint* Blueprint = Target.Get();
	if (!TestNotNull(TEXT("Widget Blueprint exists"), Blueprint))
	{
		return false;
	}
	const TStrongObjectPtr<UWidget> ChildA(FindWidget(Blueprint, TEXT("ChildA")));
	const TStrongObjectPtr<UWidget> ChildB(FindWidget(Blueprint, TEXT("ChildB")));
	const TStrongObjectPtr<UWidget> Label(FindWidget(Blueprint, TEXT("Label")));
	const FGuid GuidA = Blueprint->WidgetVariableNameToGuidMap.FindRef(TEXT("ChildA"));
	const FGuid GuidB = Blueprint->WidgetVariableNameToGuidMap.FindRef(TEXT("ChildB"));
	const FGuid GuidLabel = Blueprint->WidgetVariableNameToGuidMap.FindRef(TEXT("Label"));
	TestTrue(TEXT("the widgets and their variable GUIDs exist"), ChildA.IsValid() && ChildB.IsValid() && Label.IsValid()
		&& GuidA.IsValid() && GuidB.IsValid() && GuidLabel.IsValid());

	auto CheckKept = [&](const TCHAR* Stage)
	{
		Blueprint = Target.Get();
		TestTrue(*FString::Printf(TEXT("%s: ChildA is the same object"), Stage), ChildA.IsValid() && FindWidget(Blueprint, TEXT("ChildA")) == ChildA.Get());
		TestTrue(*FString::Printf(TEXT("%s: ChildB is the same object"), Stage), ChildB.IsValid() && FindWidget(Blueprint, TEXT("ChildB")) == ChildB.Get());
		TestTrue(*FString::Printf(TEXT("%s: Label is the same object"), Stage), Label.IsValid() && FindWidget(Blueprint, TEXT("Label")) == Label.Get());
		TestEqual(*FString::Printf(TEXT("%s: ChildA keeps its variable GUID"), Stage), Blueprint->WidgetVariableNameToGuidMap.FindRef(TEXT("ChildA")), GuidA);
		TestEqual(*FString::Printf(TEXT("%s: ChildB keeps its variable GUID"), Stage), Blueprint->WidgetVariableNameToGuidMap.FindRef(TEXT("ChildB")), GuidB);
		TestEqual(*FString::Printf(TEXT("%s: Label keeps its variable GUID"), Stage), Blueprint->WidgetVariableNameToGuidMap.FindRef(TEXT("Label")), GuidLabel);
		const UProgressBar* Bar = Cast<UProgressBar>(FindWidget(Blueprint, TEXT("ChildA")));
		TestTrue(*FString::Printf(TEXT("%s: ChildA keeps its Percent"), Stage), Bar != nullptr && FMath::IsNearlyEqual(Bar->GetPercent(), 0.25f));
		TestTrue(*FString::Printf(TEXT("%s: ChildA keeps its binding"), Stage), Blueprint->Bindings.ContainsByPredicate(
			[](const FDelegateEditorBinding& Binding) { return Binding.ObjectName == TEXT("ChildA") && Binding.PropertyName == TEXT("Percent"); }));
		TestTrue(*FString::Printf(TEXT("%s: ChildB keeps its OnClicked event"), Stage), HasBoundEvent(Blueprint, TEXT("ChildB")));
		TestTrue(*FString::Printf(TEXT("%s: Label stays inside ChildB"), Stage), Label.IsValid() && Label->GetParent() == ChildB.Get());
		TestTrue(*FString::Printf(TEXT("%s: compiles"), Stage), CompilesClean(Blueprint));
	};

	const FString Moved = Document(Target, IdentityBody(IdentityMovedRoot));
	if (ApplyAndCheck(*this, Target, Moved, TEXT("Moved")))
	{
		CheckKept(TEXT("Moved"));
		const UVerticalBoxSlot* Slot = ChildA.IsValid() ? Cast<UVerticalBoxSlot>(ChildA->Slot) : nullptr;
		TestTrue(TEXT("Moved: ChildA's new slot is centred"), Slot != nullptr && Slot->GetHorizontalAlignment() == HAlign_Center);
		const UPanelWidget* Stack = Cast<UPanelWidget>(FindWidget(Blueprint, TEXT("Stack")));
		TestTrue(TEXT("Moved: Stack holds ChildB then ChildA"), Stack != nullptr && Stack->GetChildrenCount() == 2
			&& Stack->GetChildAt(0) == ChildB.Get() && Stack->GetChildAt(1) == ChildA.Get());
		TestNull(TEXT("Moved: Wrap is gone"), FindWidget(Blueprint, TEXT("Wrap")));
		TestFalse(TEXT("Moved: Wrap has no variable GUID"), Blueprint->WidgetVariableNameToGuidMap.Contains(TEXT("Wrap")));
		if (ApplyAndCheck(*this, Target, Base, TEXT("Back")))
		{
			CheckKept(TEXT("Back"));
			TestNull(TEXT("Back: Stack is gone"), FindWidget(Blueprint, TEXT("Stack")));
			TestFalse(TEXT("Back: Stack has no variable GUID"), Blueprint->WidgetVariableNameToGuidMap.Contains(TEXT("Stack")));
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentWidgetTreeMoveRollbackTest, "UEAssetDoc.WidgetBlueprint.TreeMove.Rollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentWidgetTreeMoveRollbackTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentWidgetTreeMoveTests;
	const FTarget Target(TEXT("WBP_MoveRollback"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	const FString Base = Document(Target, IdentityBody(IdentityBaseRoot));
	if (!TestTrue(TEXT("base creates"), Apply(*this, Target, Base, TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	const FString Before = Serialize(Extract(*this, Target, TEXT("before")));
	// The tree restructuring succeeds; the binding to a missing function then fails the apply.
	const FString Failing = Document(Target, FString(TEXT(R"json(
"Variables":[{"Name":"Health","Type":"float","DefaultValue":0.5}],
"Bindings":[{"Widget":"ChildA","Property":"Percent","Kind":"Function","Function":"Missing"}],)json")) + TreeBody(IdentityMovedRoot));
	const FAssetDocumentResult Failed = Apply(*this, Target, Failing, TEXT("Failing"));
	TestFalse(TEXT("the apply fails"), Failed.IsSuccess());
	TestTrue(TEXT("it fails at the binding"), Failed.Diagnostics.ContainsByPredicate(
		[](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == TEXT("Widget.BindingFunctionNotFound"); }));
	UWidgetBlueprint* Blueprint = Target.Get();
	TestTrue(TEXT("rolled back: compiles"), CompilesClean(Blueprint));
	TestTrue(TEXT("rolled back: package is clean"), Blueprint != nullptr && !Blueprint->GetOutermost()->IsDirty());
	TestEqual(TEXT("rolled back: extraction is unchanged"), Serialize(Extract(*this, Target, TEXT("after"))), Before);
	TestNull(TEXT("rolled back: no Stack"), FindWidget(Blueprint, TEXT("Stack")));
	// The asset still restructures after the rollback.
	ApplyAndCheck(*this, Target, Document(Target, IdentityBody(IdentityMovedRoot)), TEXT("After rollback"));
	return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentWidgetTreeMoveNamedSlotsTest, "UEAssetDoc.WidgetBlueprint.TreeMove.NamedSlots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentWidgetTreeMoveNamedSlotsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentWidgetTreeMoveTests;
	const FTarget Host(TEXT("WBP_MoveHost"));
	const FTarget User(TEXT("WBP_MoveUser"));
	const FTarget Child(TEXT("WBP_MoveChild"));
	ON_SCOPE_EXIT
	{
		Child.Cleanup();
		User.Cleanup();
		Host.Cleanup();
	};
	// The host exposes the named slot "Body".
	const FString HostDocument = Document(Host, TreeBody(TEXT(R"json({"Name":"Frame","Class":"/Script/UMG.Overlay","IsVariable":false,
		"Children":[{"Name":"Body","Class":"/Script/UMG.NamedSlot","IsVariable":false}]})json")));
	if (!TestTrue(TEXT("host creates"), Apply(*this, Host, HostDocument, TEXT("Host")).IsSuccess()))
	{
		return false;
	}
	const FString HostClass = Host.PackageName + TEXT(".") + Host.AssetName + TEXT("_C");

	// A host widget whose named slot content is wrapped in a new container.
	auto Card = [&HostClass](const TCHAR* Node)
	{
		return FString(TEXT(R"json({"Name":"Root","Class":"/Script/UMG.SizeBox","IsVariable":false,"Children":[
		{"Name":"Card","Class":"HOST","IsVariable":false,"NamedSlots":{"Body":NODE}}]})json")).Replace(TEXT("HOST"), *HostClass).Replace(TEXT("NODE"), Node);
	};
	const TCHAR* Content = TEXT(R"json({"Name":"ChildA","Class":"/Script/UMG.SizeBox","IsVariable":false})json");
	const TCHAR* Wrapped = TEXT(R"json({"Name":"Stack","Class":"/Script/UMG.VerticalBox","IsVariable":false,"Children":[
		{"Name":"ChildA","Class":"/Script/UMG.SizeBox","IsVariable":false}]})json");
	const FString UserBase = Document(User, TreeBody(*Card(Content)));
	if (TestTrue(TEXT("user creates"), Apply(*this, User, UserBase, TEXT("User")).IsSuccess())
		&& ApplyAndCheck(*this, User, Document(User, TreeBody(*Card(Wrapped))), TEXT("Host slot wrapped")))
	{
		ApplyAndCheck(*this, User, UserBase, TEXT("Host slot back"));
	}

	// A named slot inherited from the parent Widget Blueprint, wrapped the same way.
	auto Inherited = [](const TCHAR* Node) { return FString::Printf(TEXT("\"WidgetTree\":{\"RootWidget\":null,\"NamedSlotBindings\":{\"Body\":%s}}"), Node); };
	const FString ChildBase = Document(Child, Inherited(Content), HostClass);
	if (TestTrue(TEXT("child creates"), Apply(*this, Child, ChildBase, TEXT("Child")).IsSuccess())
		&& ApplyAndCheck(*this, Child, Document(Child, Inherited(Wrapped), HostClass), TEXT("Inherited slot wrapped")))
	{
		ApplyAndCheck(*this, Child, ChildBase, TEXT("Inherited slot back"));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentWidgetTreeMoveDetachedTest, "UEAssetDoc.WidgetBlueprint.TreeMove.Detached",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentWidgetTreeMoveDetachedTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentWidgetTreeMoveTests;
	using UE::AssetDocument::Graph::FObjectBuilder;
	const FTarget Left(TEXT("WBP_MoveLeft"));
	const FTarget Plain(TEXT("WBP_MovePlain"));
	ON_SCOPE_EXIT
	{
		Left.Cleanup();
		Plain.Cleanup();
	};
	if (!TestTrue(TEXT("left creates"), Apply(*this, Left, Document(Left, IdentityBody(IdentityBaseRoot)), TEXT("Left")).IsSuccess())
		|| !TestTrue(TEXT("plain creates"), Apply(*this, Plain, Document(Plain, IdentityBody(IdentityBaseRoot)), TEXT("Plain")).IsSuccess()))
	{
		return false;
	}
	UWidgetBlueprint* LeftBlueprint = Left.Get();
	UWidgetBlueprint* PlainBlueprint = Plain.Get();
	if (!TestTrue(TEXT("both exist"), LeftBlueprint != nullptr && PlainBlueprint != nullptr))
	{
		return false;
	}
	const TStrongObjectPtr<UWidget> ChildB(FindWidget(LeftBlueprint, TEXT("ChildB")));
	const TStrongObjectPtr<UWidget> Label(FindWidget(LeftBlueprint, TEXT("Label")));
	const FGuid GuidB = LeftBlueprint->WidgetVariableNameToGuidMap.FindRef(TEXT("ChildB"));
	const FGuid GuidLabel = LeftBlueprint->WidgetVariableNameToGuidMap.FindRef(TEXT("Label"));
	auto InBlueprint = [&](const TCHAR* Stage)
	{
		TestTrue(*FString::Printf(TEXT("%s: ChildB stays in the Blueprint"), Stage), ChildB.IsValid()
			&& ChildB->GetOuter() == LeftBlueprint->WidgetTree && ChildB->GetFName() == TEXT("ChildB"));
		TestTrue(*FString::Printf(TEXT("%s: Label stays inside ChildB"), Stage), Label.IsValid()
			&& Label->GetOuter() == LeftBlueprint->WidgetTree && Label->GetFName() == TEXT("Label") && Label->GetParent() == ChildB.Get());
		TestEqual(*FString::Printf(TEXT("%s: ChildB keeps its variable GUID"), Stage), LeftBlueprint->WidgetVariableNameToGuidMap.FindRef(TEXT("ChildB")), GuidB);
		TestEqual(*FString::Printf(TEXT("%s: Label keeps its variable GUID"), Stage), LeftBlueprint->WidgetVariableNameToGuidMap.FindRef(TEXT("Label")), GuidLabel);
	};

	// Detached, then another tree edit (which would compile), then placed again.
	Invoke(*this, *LeftBlueprint, TEXT("umg.tree.move.v1"), FObjectBuilder().AddString(TEXT("name"), TEXT("ChildB")).AddBool(TEXT("detach"), true));
	InBlueprint(TEXT("detached"));
	TestEqual(TEXT("detached: listed by the tree query"), Detached(*this, *LeftBlueprint), TArray<FString>{TEXT("ChildB")});
	for (UWidgetBlueprint* Blueprint : {LeftBlueprint, PlainBlueprint})
	{
		Invoke(*this, *Blueprint, TEXT("umg.tree.add.v1"), FObjectBuilder().AddString(TEXT("name"), TEXT("Extra"))
			.AddString(TEXT("class"), TEXT("/Script/UMG.TextBlock")).AddString(TEXT("parent"), TEXT("Wrap")));
	}
	InBlueprint(TEXT("after an add"));
	for (UWidgetBlueprint* Blueprint : {LeftBlueprint, PlainBlueprint})
	{
		Invoke(*this, *Blueprint, TEXT("umg.tree.move.v1"), FObjectBuilder().AddString(TEXT("name"), TEXT("ChildB"))
			.AddString(TEXT("parent"), TEXT("Wrap")).AddInt(TEXT("index"), 0));
	}
	InBlueprint(TEXT("placed again"));
	TestTrue(TEXT("placed again: first child of Wrap"), ChildB.IsValid() && ChildB->GetParent() == FindWidget(LeftBlueprint, TEXT("Wrap"))
		&& ChildB->GetParent()->GetChildIndex(ChildB.Get()) == 0);
	TestEqual(TEXT("placed again: nothing is detached"), Detached(*this, *LeftBlueprint), TArray<FString>());

	// Detached and not placed again, then removed: the same Blueprint as a plain remove.
	Invoke(*this, *LeftBlueprint, TEXT("umg.tree.move.v1"), FObjectBuilder().AddString(TEXT("name"), TEXT("ChildB")).AddBool(TEXT("detach"), true));
	Invoke(*this, *LeftBlueprint, TEXT("umg.tree.remove.v1"), FObjectBuilder().AddString(TEXT("name"), TEXT("ChildB")));
	Invoke(*this, *PlainBlueprint, TEXT("umg.tree.remove.v1"), FObjectBuilder().AddString(TEXT("name"), TEXT("ChildB")));
	TestEqual(TEXT("removed: nothing is detached"), Detached(*this, *LeftBlueprint), TArray<FString>());
	for (UWidgetBlueprint* Blueprint : {LeftBlueprint, PlainBlueprint})
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		TestTrue(*FString::Printf(TEXT("%s: compiles"), *Blueprint->GetName()), CompilesClean(Blueprint));
		TestNull(*FString::Printf(TEXT("%s: ChildB is gone"), *Blueprint->GetName()), FindObjectFast<UWidget>(Blueprint->WidgetTree, TEXT("ChildB")));
		TestNull(*FString::Printf(TEXT("%s: Label is gone"), *Blueprint->GetName()), FindObjectFast<UWidget>(Blueprint->WidgetTree, TEXT("Label")));
	}
	// The editor's delete leaves a bound event of the widget in the graph (the compiler warns about it).
	TestEqual(TEXT("removed: ChildB's event is handled the same"), HasBoundEvent(LeftBlueprint, TEXT("ChildB")), HasBoundEvent(PlainBlueprint, TEXT("ChildB")));
	TestEqual(TEXT("removed: the same variable GUID names"), GuidNames(LeftBlueprint), GuidNames(PlainBlueprint));
	TestEqual(TEXT("removed: the same bindings"), LeftBlueprint->Bindings.Num(), PlainBlueprint->Bindings.Num());
	TestEqual(TEXT("removed: the same Designer extraction"), ExtractedDesigner(*this, Left, TEXT("left")), ExtractedDesigner(*this, Plain, TEXT("plain")));
	return !HasAnyErrors();
}
#endif
