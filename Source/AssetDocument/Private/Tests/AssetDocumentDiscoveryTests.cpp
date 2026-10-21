// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "Discovery/AssetDocumentDiscovery.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"

#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Character.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Materials/Material.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/AutomationTest.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
TArray<FString> PageStrings(const FAssetDocumentResult& Result, const TCHAR* ArrayKey, const TCHAR* FieldKey)
{
	TArray<FString> Values;
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	if (Result.Payload.IsValid() && Result.Payload->TryGetArrayField(ArrayKey, Rows))
	{
		for (const TSharedPtr<FJsonValue>& Row : *Rows)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			FString Value;
			if (Row->TryGetObject(Object) && (*Object)->TryGetStringField(FieldKey, Value))
			{
				Values.Add(Value);
			}
		}
	}
	return Values;
}

const TSharedPtr<FJsonObject>* FindRow(const FAssetDocumentResult& Result, const TCHAR* ArrayKey, const TCHAR* FieldKey, const FString& Value)
{
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	if (Result.Payload.IsValid() && Result.Payload->TryGetArrayField(ArrayKey, Rows))
	{
		for (const TSharedPtr<FJsonValue>& Row : *Rows)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (Row->TryGetObject(Object) && (*Object)->GetStringField(FieldKey) == Value)
			{
				return Object;
			}
		}
	}
	return nullptr;
}

FString FirstCode(const FAssetDocumentResult& Result)
{
	return Result.Diagnostics.IsEmpty() ? FString() : Result.Diagnostics[0].Code;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentDiscoveryFindAssetsTest,
	"UEAssetDoc.Discovery.FindAssets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentDiscoveryFindAssetsTest::RunTest(const FString&)
{
	const FAssetDocumentService Service;
	const FAssetDocumentResult Skeletons = Service.FindAssets({
		{TEXT("path"), TEXT("/Engine/EngineMeshes")}, {TEXT("class"), TEXT("/Script/Engine.Skeleton")}});
	TestTrue(TEXT("find by path and class succeeds"), Skeletons.IsSuccess());
	TestTrue(TEXT("engine skeleton is found"),
		PageStrings(Skeletons, TEXT("Assets"), TEXT("ObjectPath")).Contains(TEXT("/Engine/EngineMeshes/SkeletalCube_Skeleton.SkeletalCube_Skeleton")));
	TestFalse(TEXT("class filter excludes the skeletal mesh"),
		PageStrings(Skeletons, TEXT("Assets"), TEXT("ObjectPath")).Contains(TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube")));

	// Subclasses are included by default: MaterialInterface finds Materials.
	const FAssetDocumentResult Materials = Service.FindAssets({
		{TEXT("path"), TEXT("/Engine/EngineMaterials")}, {TEXT("class"), TEXT("MaterialInterface")}, {TEXT("name"), TEXT("BaseFlatten*Material")}});
	const TArray<FString> MaterialPaths = PageStrings(Materials, TEXT("Assets"), TEXT("ObjectPath"));
	TestTrue(TEXT("wildcard and base class find BaseFlattenMaterial"),
		MaterialPaths.Contains(TEXT("/Engine/EngineMaterials/BaseFlattenMaterial.BaseFlattenMaterial")));
	TestFalse(TEXT("wildcard is anchored"), MaterialPaths.Contains(TEXT("/Engine/EngineMaterials/BaseFlattenMaterial_VT.BaseFlattenMaterial_VT")));
	const FAssetDocumentResult Exact = Service.FindAssets({
		{TEXT("path"), TEXT("/Engine/EngineMaterials")}, {TEXT("class"), TEXT("MaterialInterface")}, {TEXT("exact_class"), TEXT("true")}});
	TestEqual(TEXT("exact_class excludes subclasses"), PageStrings(Exact, TEXT("Assets"), TEXT("ObjectPath")).Num(), 0);

	const FAssetDocumentResult Page = Service.FindAssets({{TEXT("path"), TEXT("/Engine/EngineMaterials")}, {TEXT("limit"), TEXT("2")}});
	TestEqual(TEXT("limit bounds the page"), PageStrings(Page, TEXT("Assets"), TEXT("ObjectPath")).Num(), 2);
	TestTrue(TEXT("a partial page reports NextOffset"), Page.Payload.IsValid() && Page.Payload->HasField(TEXT("NextOffset")));
	const FAssetDocumentResult Next = Service.FindAssets({{TEXT("path"), TEXT("/Engine/EngineMaterials")}, {TEXT("limit"), TEXT("2")}, {TEXT("offset"), TEXT("2")}});
	const TArray<FString> NextPaths = PageStrings(Next, TEXT("Assets"), TEXT("ObjectPath"));
	TestTrue(TEXT("pages do not overlap"), NextPaths.Num() > 0 && !PageStrings(Page, TEXT("Assets"), TEXT("ObjectPath")).Contains(NextPaths[0]));

	const FAssetDocumentResult BadClass = Service.FindAssets({{TEXT("class"), TEXT("/Script/Engine.NoSuchAssetDocumentClass")}});
	TestFalse(TEXT("unknown class fails"), BadClass.IsSuccess());
	TestEqual(TEXT("unknown class is an invalid query"), FirstCode(BadClass), FString(TEXT("Discovery.InvalidQuery")));
	const FAssetDocumentResult BadLimit = Service.FindAssets({{TEXT("limit"), TEXT("-1")}});
	TestEqual(TEXT("negative limit is an invalid query"), FirstCode(BadLimit), FString(TEXT("Discovery.InvalidQuery")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentDiscoveryListTypesTest,
	"UEAssetDoc.Discovery.ListTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentDiscoveryListTypesTest::RunTest(const FString&)
{
	const FAssetDocumentService Service;
	const FAssetDocumentResult RowStructs = Service.ListTypes({
		{TEXT("kind"), TEXT("struct")}, {TEXT("base"), TEXT("/Script/Engine.TableRowBase")}, {TEXT("limit"), TEXT("500")}});
	TestTrue(TEXT("row structs listed"), RowStructs.IsSuccess());
	const TArray<FString> StructPaths = PageStrings(RowStructs, TEXT("Types"), TEXT("Path"));
	TestTrue(TEXT("TableRowBase subclasses include GameplayTagTableRow"), StructPaths.Contains(TEXT("/Script/GameplayTags.GameplayTagTableRow")));
	TestFalse(TEXT("unrelated structs are excluded"), StructPaths.Contains(TEXT("/Script/CoreUObject.Vector")));

	const FAssetDocumentResult Concrete = Service.ListTypes({
		{TEXT("kind"), TEXT("class")}, {TEXT("base"), TEXT("/Script/Engine.ActorComponent")}, {TEXT("name"), TEXT("PrimitiveComponent")}});
	TestFalse(TEXT("abstract classes are excluded by default"),
		PageStrings(Concrete, TEXT("Types"), TEXT("Path")).Contains(TEXT("/Script/Engine.PrimitiveComponent")));
	const FAssetDocumentResult Scene = Service.ListTypes({
		{TEXT("kind"), TEXT("class")}, {TEXT("base"), TEXT("/Script/Engine.SceneComponent")}, {TEXT("name"), TEXT("StaticMeshComponent")}});
	TestTrue(TEXT("base and name filters list concrete subclasses"),
		PageStrings(Scene, TEXT("Types"), TEXT("Path")).Contains(TEXT("/Script/Engine.StaticMeshComponent")));
	const FAssetDocumentResult Abstract = Service.ListTypes({
		{TEXT("kind"), TEXT("class")}, {TEXT("base"), TEXT("/Script/Engine.ActorComponent")}, {TEXT("name"), TEXT("PrimitiveComponent")},
		{TEXT("include_abstract"), TEXT("true")}});
	const TSharedPtr<FJsonObject>* Primitive = FindRow(Abstract, TEXT("Types"), TEXT("Path"), TEXT("/Script/Engine.PrimitiveComponent"));
	TestTrue(TEXT("include_abstract lists abstract classes, marked"), Primitive != nullptr && (*Primitive)->GetBoolField(TEXT("Abstract")));

	const FAssetDocumentResult Enums = Service.ListTypes({{TEXT("kind"), TEXT("enum")}, {TEXT("name"), TEXT("ECollisionChannel")}});
	TestTrue(TEXT("enums listed by name"), PageStrings(Enums, TEXT("Types"), TEXT("Path")).Contains(TEXT("/Script/Engine.ECollisionChannel")));

	// Names typed as the editor shows them, with spaces. A class name has none, so a spaced
	// pattern that matches nothing is retried without them, and the page says so.
	const FAssetDocumentResult Spaced = Service.ListTypes({
		{TEXT("kind"), TEXT("class")}, {TEXT("base"), TEXT("/Script/Engine.SceneComponent")}, {TEXT("name"), TEXT("*Static Mesh Comp*")}});
	TestTrue(TEXT("a spaced wildcard finds the class named without spaces"),
		PageStrings(Spaced, TEXT("Types"), TEXT("Path")).Contains(TEXT("/Script/Engine.StaticMeshComponent")));
	TestTrue(TEXT("the page notes the unspaced match"), Spaced.Payload.IsValid() && Spaced.Payload->GetStringField(TEXT("Note")).Contains(TEXT("*StaticMeshComp*")));
	TestFalse(TEXT("a pattern that matches as written has no note"), Scene.Payload.IsValid() && Scene.Payload->HasField(TEXT("Note")));
	const FAssetDocumentResult Tracks = Service.ListTypes({{TEXT("kind"), TEXT("sequencer_track")}, {TEXT("name"), TEXT("Camera Cut")}});
	if (Tracks.IsSuccess())
	{
		TestTrue(TEXT("a provider kind retries a spaced name too (Camera Cut -> CameraCut)"),
			PageStrings(Tracks, TEXT("Types"), TEXT("Name")).ContainsByPredicate([](const FString& Name) { return Name.Contains(TEXT("CameraCut")); })
			|| PageStrings(Tracks, TEXT("Types"), TEXT("Path")).ContainsByPredicate([](const FString& Path) { return Path.Contains(TEXT("CameraCut")); }));
	}

	TestEqual(TEXT("unknown kind is an invalid query"), FirstCode(Service.ListTypes({{TEXT("kind"), TEXT("widget")}})), FString(TEXT("Discovery.InvalidQuery")));
	TestEqual(TEXT("unknown base is an invalid query"),
		FirstCode(Service.ListTypes({{TEXT("kind"), TEXT("struct")}, {TEXT("base"), TEXT("/Script/Engine.NoSuchAssetDocumentStruct")}})),
		FString(TEXT("Discovery.InvalidQuery")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentDiscoveryInspectTypesTest,
	"UEAssetDoc.Discovery.InspectTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentDiscoveryInspectTypesTest::RunTest(const FString&)
{
	const FAssetDocumentService Service;
	FAssetDocumentInspectRequest Request;
	Request.ClassOrAsset = TEXT("/Script/GameplayTags.GameplayTagTableRow");
	const FAssetDocumentResult Struct = Service.Inspect(Request);
	TestTrue(TEXT("a struct path inspects"), Struct.IsSuccess());
	TestEqual(TEXT("struct kind"), Struct.Payload.IsValid() ? Struct.Payload->GetStringField(TEXT("Kind")) : FString(), FString(TEXT("Struct")));
	TestTrue(TEXT("TableRowBase child is a row struct"), Struct.Payload.IsValid() && Struct.Payload->GetBoolField(TEXT("RowStruct")));
	const TSharedPtr<FJsonObject>* Tag = FindRow(Struct, TEXT("Fields"), TEXT("Name"), TEXT("Tag"));
	TestTrue(TEXT("struct fields carry name and type"), Tag != nullptr && (*Tag)->GetStringField(TEXT("Type")) == TEXT("FName"));

	Request.ClassOrAsset = TEXT("/Script/Engine.ECollisionChannel");
	const FAssetDocumentResult Enum = Service.Inspect(Request);
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	TestTrue(TEXT("an enum path inspects its values"), Enum.IsSuccess() && Enum.Payload->TryGetArrayField(TEXT("Values"), Values)
		&& Values->ContainsByPredicate([](const TSharedPtr<FJsonValue>& Value) { return Value->AsString() == TEXT("ECC_WorldStatic"); }));

	Request.ClassOrAsset = TEXT("/Script/Engine.NoSuchAssetDocumentType");
	const FAssetDocumentResult Missing = Service.Inspect(Request);
	TestFalse(TEXT("unknown type fails"), Missing.IsSuccess());
	TestEqual(TEXT("unknown type reports a diagnostic"), FirstCode(Missing), FString(TEXT("ClassResolve.Unresolved")));
	TestEqual(TEXT("diagnostic points at the argument"), Missing.Diagnostics.IsEmpty() ? FString() : Missing.Diagnostics[0].Path, FString(TEXT("/class_or_asset")));

	Request.ClassOrAsset = TEXT("/Game/AssetDocumentDiscovery/NoSuchAsset");
	TestEqual(TEXT("missing /Game asset reports a diagnostic"), FirstCode(Service.Inspect(Request)), FString(TEXT("Inspect.AssetNotFound")));
	return true;
}

// Inspecting /Script/Engine.CameraShakeBase (the parent of a camera shake Blueprint)
// was refused with FactoryCreate.AbstractClass. An abstract class is described read-only; a
// document still cannot author it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentDiscoveryInspectAbstractClassTest,
	"UEAssetDoc.Discovery.InspectAbstractClass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentDiscoveryInspectAbstractClassTest::RunTest(const FString&)
{
	const FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	FAssetDocumentInspectRequest Request;
	Request.ClassOrAsset = TEXT("/Script/Engine.CameraShakeBase");
	const FAssetDocumentResult Result = Service.Inspect(Request);
	AddInfo(FString::Printf(TEXT("%s (%s)"), *Result.Message, *FirstCode(Result)));
	if (!TestTrue(TEXT("an abstract class inspects"), Result.IsSuccess() && Result.Payload.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("the result is read-only"), Result.bReadOnly && Result.Payload->GetBoolField(TEXT("ReadOnly")));
	TestTrue(TEXT("the payload says abstract"), Result.Payload->GetBoolField(TEXT("Abstract")));
	TestTrue(TEXT("the message says how to author it"), Result.Message.Contains(TEXT("abstract")) && Result.Message.Contains(TEXT("ParentClass")));
	TArray<FString> Names = PageStrings(Result, TEXT("properties"), TEXT("name"));
	Names.Append(PageStrings(Result, TEXT("skipped"), TEXT("name")));
	TestTrue(TEXT("its properties are listed (bSingleInstance)"), Names.Contains(TEXT("bSingleInstance")));
	TestTrue(TEXT("its properties are listed (RootShakePattern)"), Names.Contains(TEXT("RootShakePattern")));

	// Authoring stays refused.
	FAssetDocumentProfileRequest Profile;
	Profile.ClassOrAsset = TEXT("/Script/Engine.CameraShakeBase");
	TestEqual(TEXT("inspect_asset_document_profile still refuses it"), FirstCode(Service.InspectProfile(Profile)), FString(TEXT("FactoryCreate.AbstractClass")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentDiscoveryAssetFactsTest,
	"UEAssetDoc.Discovery.AssetFacts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentDiscoveryAssetFactsTest::RunTest(const FString&)
{
	// Engine content is read-only; without a runtime route (a bare Service) Inspect still reports its facts
	// (with the route it adds the profile details: EngineAssets.ReadOnlyInspect).
	const FAssetDocumentService Service;
	FAssetDocumentInspectRequest Request;
	Request.ClassOrAsset = TEXT("/Engine/EngineMaterials/BaseFlattenMaterial");
	const FAssetDocumentResult Material = Service.Inspect(Request);
	TestTrue(TEXT("engine material inspects"), Material.IsSuccess());
	const TArray<TSharedPtr<FJsonValue>>* Parameters = nullptr;
	TestTrue(TEXT("material facts list parameters"), Material.Payload.IsValid()
		&& Material.Payload->TryGetArrayField(TEXT("MaterialParameters"), Parameters) && !Parameters->IsEmpty());
	TestTrue(TEXT("engine asset is read-only"), Material.Payload.IsValid() && Material.Payload->GetBoolField(TEXT("ReadOnly")));

	Request.ClassOrAsset = TEXT("/Engine/EngineMeshes/SkeletalCube_Skeleton");
	const FAssetDocumentResult EngineSkeleton = Service.Inspect(Request);
	const TSharedPtr<FJsonObject>* SkeletonFacts = nullptr;
	TestTrue(TEXT("engine skeleton facts"), EngineSkeleton.IsSuccess() && EngineSkeleton.Payload->TryGetObjectField(TEXT("Skeleton"), SkeletonFacts)
		&& (*SkeletonFacts)->HasTypedField<EJson::Array>(TEXT("Bones")));

	USkeleton* Skeleton = NewObject<USkeleton>(GetTransientPackage(), NAME_None, RF_Transient);
	ON_SCOPE_EXIT { Skeleton->MarkAsGarbage(); };
	Skeleton->AddSlotGroupName(TEXT("Discovery"));
	Skeleton->SetSlotGroupName(TEXT("DiscoveryUpper"), TEXT("Discovery"));
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	UE::AssetDocument::Discovery::AddAssetFacts(*Skeleton, *Payload);
	const TSharedPtr<FJsonObject>* Facts = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Groups = nullptr;
	bool bFoundSlot = false;
	if (Payload->TryGetObjectField(TEXT("Skeleton"), Facts) && (*Facts)->TryGetArrayField(TEXT("SlotGroups"), Groups))
	{
		for (const TSharedPtr<FJsonValue>& Group : *Groups)
		{
			const TSharedPtr<FJsonObject> Object = Group->AsObject();
			const TArray<TSharedPtr<FJsonValue>>* Slots = nullptr;
			bFoundSlot |= Object->GetStringField(TEXT("Group")) == TEXT("Discovery") && Object->TryGetArrayField(TEXT("Slots"), Slots)
				&& Slots->ContainsByPredicate([](const TSharedPtr<FJsonValue>& Slot) { return Slot->AsString() == TEXT("DiscoveryUpper"); });
		}
	}
	TestTrue(TEXT("skeleton facts list slot groups with their slots"), bFoundSlot);

	// Existing payload keys win (a profile's own field is never replaced).
	TSharedRef<FJsonObject> Existing = MakeShared<FJsonObject>();
	Existing->SetStringField(TEXT("Skeleton"), TEXT("profile"));
	UE::AssetDocument::Discovery::AddAssetFacts(*Skeleton, *Existing);
	TestEqual(TEXT("existing payload keys are kept"), Existing->GetStringField(TEXT("Skeleton")), FString(TEXT("profile")));
	return true;
}

// What placing and timing need, without execute_python: mesh bounds around the pivot, animation length, root
// motion and notifies, sound duration.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentDiscoveryPlacementFactsTest,
	"UEAssetDoc.Discovery.PlacementFacts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentDiscoveryPlacementFactsTest::RunTest(const FString&)
{
	const FAssetDocumentService Service;
	auto Facts = [this, &Service](const TCHAR* Asset, const TCHAR* Key) -> TSharedPtr<FJsonObject>
	{
		FAssetDocumentInspectRequest Request;
		Request.ClassOrAsset = Asset;
		const FAssetDocumentResult Result = Service.Inspect(Request);
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!TestTrue(*FString::Printf(TEXT("%s has %s facts"), Asset, Key), Result.IsSuccess() && Result.Payload.IsValid()
			&& Result.Payload->TryGetObjectField(Key, Object)))
		{
			return nullptr;
		}
		return *Object;
	};
	auto Vector = [](const TSharedPtr<FJsonObject>& Object, const TCHAR* Key)
	{
		FString Text;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Object.IsValid() && Object->TryGetArrayField(Key, Values))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				Text += FString::Printf(TEXT("%s%g"), Text.IsEmpty() ? TEXT("") : TEXT(","), Value->AsNumber());
			}
		}
		return Text;
	};
	if (const TSharedPtr<FJsonObject> Cube = Facts(TEXT("/Engine/BasicShapes/Cube"), TEXT("StaticMesh")))
	{
		const TSharedPtr<FJsonObject> Bounds = Cube->GetObjectField(TEXT("Bounds"));
		TestEqual(TEXT("the cube's bounds are centred on its pivot (Min)"), Vector(Bounds, TEXT("Min")), FString(TEXT("-50,-50,-50")));
		TestEqual(TEXT("the cube's size"), Vector(Bounds, TEXT("Size")), FString(TEXT("100,100,100")));
		TestTrue(TEXT("material slots"), Cube->HasTypedField<EJson::Array>(TEXT("MaterialSlots")));
	}
	if (const TSharedPtr<FJsonObject> Skeletal = Facts(TEXT("/Engine/EngineMeshes/SkeletalCube"), TEXT("SkeletalMesh")))
	{
		TestFalse(TEXT("skeletal mesh bounds"), Vector(Skeletal->GetObjectField(TEXT("Bounds")), TEXT("Size")).IsEmpty());
	}
	if (const TSharedPtr<FJsonObject> Animation = Facts(TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle"), TEXT("Animation")))
	{
		TestTrue(TEXT("animation length"), Animation->GetNumberField(TEXT("PlayLength")) > 0.0);
		TestTrue(TEXT("root motion"), Animation->HasTypedField<EJson::Boolean>(TEXT("RootMotion")));
		TestTrue(TEXT("notifies summary"), Animation->HasTypedField<EJson::Number>(TEXT("NotifyCount"))
			&& Animation->HasTypedField<EJson::Array>(TEXT("Notifies")));
	}
	if (const TSharedPtr<FJsonObject> Sound = Facts(TEXT("/Engine/VREditor/Sounds/UI/Click_on_Button"), TEXT("Sound")))
	{
		TestTrue(TEXT("sound duration"), Sound->GetNumberField(TEXT("Duration")) > 0.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentDiscoveryCompileStatusTest,
	"UEAssetDoc.Discovery.CompileStatus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentDiscoveryCompileStatusTest::RunTest(const FString&)
{
	// No generated class: the status is read from the Blueprint and its node annotations only.
	UBlueprint* Blueprint = NewObject<UBlueprint>(GetTransientPackage(), NAME_None, RF_Transient);
	UEdGraph* Graph = NewObject<UEdGraph>(Blueprint, TEXT("EventGraph"), RF_Transient);
	Blueprint->UbergraphPages.Add(Graph);
	ON_SCOPE_EXIT { Graph->MarkAsGarbage(); Blueprint->MarkAsGarbage(); };
	UEdGraphNode* Clean = NewObject<UEdGraphNode>(Graph, NAME_None, RF_Transient);
	Graph->AddNode(Clean, false, false);
	UEdGraphNode* Warned = NewObject<UEdGraphNode>(Graph, NAME_None, RF_Transient);
	Warned->CreateNewGuid();
	Warned->bHasCompilerMessage = true;
	Warned->ErrorType = EMessageSeverity::Warning;
	Warned->ErrorMsg = TEXT("Pin default is orphaned");
	Graph->AddNode(Warned, false, false);
	Blueprint->Status = BS_UpToDateWithWarnings;

	const TSharedPtr<FJsonObject> Compile = UE::AssetDocument::Discovery::DescribeCompileStatus(*Blueprint);
	TestTrue(TEXT("Blueprint has compile status"), Compile.IsValid());
	if (!Compile.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("status is the editor's"), Compile->GetStringField(TEXT("Status")), FString(TEXT("UpToDateWithWarnings")));
	const TArray<TSharedPtr<FJsonValue>>& Messages = Compile->GetArrayField(TEXT("Messages"));
	TestEqual(TEXT("only annotated nodes report messages"), Messages.Num(), 1);
	if (Messages.Num() == 1)
	{
		const TSharedPtr<FJsonObject> Message = Messages[0]->AsObject();
		TestEqual(TEXT("warning severity"), Message->GetStringField(TEXT("Severity")), FString(TEXT("Warning")));
		TestEqual(TEXT("message text"), Message->GetStringField(TEXT("Message")), FString(TEXT("Pin default is orphaned")));
		TestEqual(TEXT("graph name"), Message->GetStringField(TEXT("Graph")), FString(TEXT("EventGraph")));
		TestEqual(TEXT("node guid"), Message->GetStringField(TEXT("NodeGuid")), Warned->NodeGuid.ToString(EGuidFormats::Digits));
	}
	Warned->ErrorType = EMessageSeverity::Error;
	Blueprint->Status = BS_Error;
	const TSharedPtr<FJsonObject> Failed = UE::AssetDocument::Discovery::DescribeCompileStatus(*Blueprint);
	TestEqual(TEXT("error status"), Failed->GetStringField(TEXT("Status")), FString(TEXT("Error")));
	TestEqual(TEXT("error severity"), Failed->GetArrayField(TEXT("Messages"))[0]->AsObject()->GetStringField(TEXT("Severity")), FString(TEXT("Error")));

	TestFalse(TEXT("non-Blueprint assets have no compile status"), UE::AssetDocument::Discovery::DescribeCompileStatus(*Graph).IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentDiscoveryDiagnosticDetailsTest,
	"UEAssetDoc.Discovery.DiagnosticDetails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentDiscoveryDiagnosticDetailsTest::RunTest(const FString&)
{
	FAssetDocumentRuntimeDiagnostic Source;
	Source.Path = TEXT("/Body/RowStruct");
	Source.Code = TEXT("ClassReference.NonCanonicalAsset");
	Source.Message = TEXT("The asset path did not resolve.");
	Source.Severity = EAssetDocumentDiagnosticSeverity::Warning;
	FAssetDocumentRuntimeDiagnosticContext& Context = Source.Context.AddDefaulted_GetRef();
	Context.Key = TEXT("primitive");
	Context.Value.Kind = EAssetDocumentRuntimeValueKind::String;
	Context.Value.StringValue = TEXT("editor.reference.resolve_asset.v1");

	FAssetDocumentResult Result = FAssetDocumentResult::Failure(TEXT("failed"));
	Result.Diagnostics.Add(ProjectAssetDocumentRuntimeDiagnostic(Source));
	Result.Diagnostics.Add({TEXT("/x"), TEXT("Plain"), TEXT("no severity")});
	const TSharedPtr<FJsonObject> Json = Result.ToJson();
	const TArray<TSharedPtr<FJsonValue>>& Diagnostics = Json->GetArrayField(TEXT("diagnostics"));
	TestEqual(TEXT("two diagnostics"), Diagnostics.Num(), 2);
	if (Diagnostics.Num() != 2)
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Projected = Diagnostics[0]->AsObject();
	TestEqual(TEXT("message unchanged"), Projected->GetStringField(TEXT("message")), FString(TEXT("The asset path did not resolve.")));
	TestEqual(TEXT("severity serialized"), Projected->GetStringField(TEXT("severity")), FString(TEXT("warning")));
	const TSharedPtr<FJsonObject>* Details = nullptr;
	TestTrue(TEXT("context goes to details"), Projected->TryGetObjectField(TEXT("details"), Details)
		&& (*Details)->GetStringField(TEXT("primitive")) == TEXT("editor.reference.resolve_asset.v1"));
	const TSharedPtr<FJsonObject> Plain = Diagnostics[1]->AsObject();
	TestFalse(TEXT("no severity field when unset"), Plain->HasField(TEXT("severity")));
	TestFalse(TEXT("no details field when unset"), Plain->HasField(TEXT("details")));
	TestFalse(TEXT("no compile field for non-Blueprint results"), Json->HasField(TEXT("compile")));
	return true;
}
namespace AssetDocumentDiscoveryTestsLocal
{
// Counts the engine's "Short type name ... provided for TryFindType" warnings (KI-5).
class FShortTypeNameWarningCapture final : public FOutputDevice
{
public:
	FShortTypeNameWarningCapture()
	{
		GLog->FlushThreadedLogs();
		GLog->AddOutputDevice(this);
	}
	virtual ~FShortTypeNameWarningCapture() override { GLog->RemoveOutputDevice(this); }
	FShortTypeNameWarningCapture(const FShortTypeNameWarningCapture&) = delete;
	FShortTypeNameWarningCapture& operator=(const FShortTypeNameWarningCapture&) = delete;

	virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override
	{
		if (Message != nullptr && FCString::Strstr(Message, TEXT("provided for TryFindType")) != nullptr)
		{
			FScopeLock Lock(&Guard);
			Lines.Add(FString(Message).Left(200));
		}
	}
	virtual bool CanBeUsedOnAnyThread() const override { return true; }
	virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

	TArray<FString> Take()
	{
		GLog->FlushThreadedLogs();
		FScopeLock Lock(&Guard);
		return MoveTemp(Lines);
	}

private:
	FCriticalSection Guard;
	TArray<FString> Lines;
};
}

// KI-5: discovery resolved short type names ("Actor", "TableRowBase") with UClass::TryFindTypeSlow,
// which logs a warning with a callstack for every short name. They resolve silently, natives first.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentDiscoveryShortTypeNamesSilentTest,
	"UEAssetDoc.Discovery.ShortTypeNamesSilent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentDiscoveryShortTypeNamesSilentTest::RunTest(const FString&)
{
	using namespace AssetDocumentDiscoveryTestsLocal;
	const FAssetDocumentService Service;
	FShortTypeNameWarningCapture Capture;

	const FAssetDocumentResult Materials = Service.FindAssets({
		{TEXT("path"), TEXT("/Engine/EngineMaterials")}, {TEXT("class"), TEXT("MaterialInterface")}, {TEXT("name"), TEXT("BaseFlattenMaterial")}});
	TestTrue(TEXT("a short class name filters find_assets"),
		PageStrings(Materials, TEXT("Assets"), TEXT("ObjectPath")).Contains(TEXT("/Engine/EngineMaterials/BaseFlattenMaterial.BaseFlattenMaterial")));
	const FAssetDocumentResult Scene = Service.ListTypes({
		{TEXT("kind"), TEXT("class")}, {TEXT("base"), TEXT("SceneComponent")}, {TEXT("name"), TEXT("StaticMeshComponent")}});
	TestTrue(TEXT("a short base class lists its subclasses"),
		PageStrings(Scene, TEXT("Types"), TEXT("Path")).Contains(TEXT("/Script/Engine.StaticMeshComponent")));
	const FAssetDocumentResult Rows = Service.ListTypes({
		{TEXT("kind"), TEXT("struct")}, {TEXT("base"), TEXT("TableRowBase")}, {TEXT("limit"), TEXT("500")}});
	TestTrue(TEXT("a short base struct lists its subclasses"),
		PageStrings(Rows, TEXT("Types"), TEXT("Path")).Contains(TEXT("/Script/GameplayTags.GameplayTagTableRow")));
	// Inspect looks a bare name up as a native class first (else as a User Defined Enum / Struct).
	FAssetDocumentInspectRequest Request;
	Request.ClassOrAsset = TEXT("Actor");
	AddInfo(FString::Printf(TEXT("inspect Actor: %s"), *Service.Inspect(Request).Message));
	TestEqual(TEXT("an unknown short base is still an invalid query"),
		FirstCode(Service.ListTypes({{TEXT("kind"), TEXT("class")}, {TEXT("base"), TEXT("NoSuchAssetDocumentShortClass")}})),
		FString(TEXT("Discovery.InvalidQuery")));

	const TArray<FString> Warnings = Capture.Take();
	TestEqual(TEXT("no short type name warning is logged"), Warnings.Num(), 0);
	for (const FString& Warning : Warnings)
	{
		AddInfo(FString::Printf(TEXT("logged: %s"), *Warning));
	}
	return true;
}

namespace DiscoveryMeshFactsTests
{
const TSharedPtr<FJsonObject>* FindNamed(const TArray<TSharedPtr<FJsonValue>>& Rows, const TCHAR* Key, const FString& Name)
{
	for (const TSharedPtr<FJsonValue>& Row : Rows)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (Row->TryGetObject(Object) && (*Object)->GetStringField(Key) == Name)
		{
			return Object;
		}
	}
	return nullptr;
}

// The response as an MCP client receives it: compact JSON.
int32 CompactSize(const FAssetDocumentResult& Result)
{
	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	FJsonSerializer::Serialize(Result.ToJson().ToSharedRef(), Writer);
	return Text.Len();
}

UBlueprint* CreateTransientBlueprint(UClass* Parent, const TCHAR* Prefix)
{
	const FName Name(*FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(Parent, GetTransientPackage(), Name, BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (Blueprint != nullptr)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}
	return Blueprint;
}
}

// A mesh's material slots are facts to build on (a component's OverrideMaterials is indexed by them), and the
// skipped SkeletalMesh.Materials says that the mesh saves it itself (it is not runtime-only).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentDiscoveryMeshMaterialSlotsTest,
	"UEAssetDoc.Discovery.MeshMaterialSlots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentDiscoveryMeshMaterialSlotsTest::RunTest(const FString&)
{
	using namespace DiscoveryMeshFactsTests;
	const FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	const USkeletalMesh* Skeletal = LoadObject<USkeletalMesh>(nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
	const UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("engine skeletal mesh"), Skeletal) || !TestNotNull(TEXT("engine static mesh"), Cube)
		|| !TestTrue(TEXT("the meshes have material slots"), !Skeletal->GetMaterials().IsEmpty() && !Cube->GetStaticMaterials().IsEmpty()))
	{
		return false;
	}
	FAssetDocumentInspectRequest Request;
	Request.ClassOrAsset = TEXT("/Engine/EngineMeshes/SkeletalCube");
	const FAssetDocumentResult SkeletalResult = Service.Inspect(Request);
	const TSharedPtr<FJsonObject>* Facts = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Slots = nullptr;
	if (TestTrue(TEXT("skeletal mesh facts list MaterialSlots"), SkeletalResult.IsSuccess()
		&& SkeletalResult.Payload->TryGetObjectField(TEXT("SkeletalMesh"), Facts)
		&& (*Facts)->TryGetArrayField(TEXT("MaterialSlots"), Slots)))
	{
		TestEqual(TEXT("one row per slot"), Slots->Num(), Skeletal->GetMaterials().Num());
		const TSharedPtr<FJsonObject> First = (*Slots)[0]->AsObject();
		TestTrue(TEXT("a slot row is {Index, SlotName, Material}"), First.IsValid() && First->GetNumberField(TEXT("Index")) == 0
			&& First->GetStringField(TEXT("SlotName")) == Skeletal->GetMaterials()[0].MaterialSlotName.ToString()
			&& First->HasField(TEXT("Material")));
		const UMaterialInterface* Material = Skeletal->GetMaterials()[0].MaterialInterface;
		FString MaterialPath;
		First->TryGetStringField(TEXT("Material"), MaterialPath);
		TestEqual(TEXT("the slot's material"), MaterialPath, Material != nullptr ? Material->GetPathName() : FString());
	}
	const TSharedPtr<FJsonObject>* MaterialsRow = FindRow(SkeletalResult, TEXT("skipped"), TEXT("name"), TEXT("Materials"));
	TestTrue(TEXT("SkeletalMesh.Materials is skipped as saved by the asset, not as transient"),
		MaterialsRow != nullptr && (*MaterialsRow)->GetStringField(TEXT("reason")) == TEXT("saved-by-asset"));

	Request.ClassOrAsset = TEXT("/Engine/BasicShapes/Cube");
	const FAssetDocumentResult StaticResult = Service.Inspect(Request);
	if (TestTrue(TEXT("static mesh facts list MaterialSlots"), StaticResult.IsSuccess()
		&& StaticResult.Payload->TryGetObjectField(TEXT("StaticMesh"), Facts)
		&& (*Facts)->TryGetArrayField(TEXT("MaterialSlots"), Slots) && Slots->Num() == Cube->GetStaticMaterials().Num()))
	{
		const TSharedPtr<FJsonObject> First = (*Slots)[0]->AsObject();
		TestTrue(TEXT("a static mesh slot row is {Index, SlotName, Material}"), First.IsValid()
			&& First->GetNumberField(TEXT("Index")) == 0
			&& First->GetStringField(TEXT("SlotName")) == Cube->GetStaticMaterials()[0].MaterialSlotName.ToString()
			&& First->HasField(TEXT("Material")));
	}
	return true;
}

// An Actor Blueprint's inspect lists its mesh components (native, inherited with this Blueprint's override, own)
// with the mesh each uses and that mesh's material slots, under the names its document's Components use.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentDiscoveryBlueprintMeshComponentsTest,
	"UEAssetDoc.Discovery.BlueprintMeshComponents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentDiscoveryBlueprintMeshComponentsTest::RunTest(const FString&)
{
	using namespace DiscoveryMeshFactsTests;
	USkeletalMesh* Skeletal = LoadObject<USkeletalMesh>(nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UMaterialInterface* Override = UMaterial::GetDefaultMaterial(MD_Surface);
	UBlueprint* Parent = CreateTransientBlueprint(ACharacter::StaticClass(), TEXT("DiscoveryMeshParent"));
	if (!TestNotNull(TEXT("meshes"), Skeletal) || !TestNotNull(TEXT("cube"), Cube)
		|| !TestTrue(TEXT("parent Blueprint compiled"), Parent != nullptr && Parent->GeneratedClass != nullptr))
	{
		return false;
	}
	ON_SCOPE_EXIT { Parent->MarkAsGarbage(); };
	USCS_Node* Prop = Parent->SimpleConstructionScript->CreateNode(UStaticMeshComponent::StaticClass(), TEXT("Prop"));
	Parent->SimpleConstructionScript->AddNode(Prop);
	Cast<UStaticMeshComponent>(Prop->ComponentTemplate)->SetStaticMesh(Cube);
	FKismetEditorUtilities::CompileBlueprint(Parent);
	ACharacter* ParentDefaults = Cast<ACharacter>(Parent->GeneratedClass->GetDefaultObject());
	ParentDefaults->GetMesh()->SetSkeletalMeshAsset(Skeletal);

	TArray<TSharedPtr<FJsonValue>> Rows = UE::AssetDocument::Discovery::DescribeMeshComponents(*Parent);
	const TSharedPtr<FJsonObject>* Mesh = FindNamed(Rows, TEXT("Name"), TEXT("Mesh"));
	const TArray<TSharedPtr<FJsonValue>>* Slots = nullptr;
	TestTrue(TEXT("the native Mesh component is listed by its member name"), Mesh != nullptr
		&& (*Mesh)->GetStringField(TEXT("Scope")) == TEXT("Native")
		&& (*Mesh)->GetStringField(TEXT("Class")) == USkeletalMeshComponent::StaticClass()->GetPathName()
		&& (*Mesh)->GetStringField(TEXT("Mesh")) == Skeletal->GetPathName());
	TestTrue(TEXT("with its mesh's material slots"), Mesh != nullptr && (*Mesh)->TryGetArrayField(TEXT("MaterialSlots"), Slots)
		&& Slots->Num() == Skeletal->GetMaterials().Num());
	TestFalse(TEXT("no OverrideMaterials while none is set"), Mesh != nullptr && (*Mesh)->HasField(TEXT("OverrideMaterials")));
	const TSharedPtr<FJsonObject>* Owned = FindNamed(Rows, TEXT("Name"), TEXT("Prop"));
	TestTrue(TEXT("the Blueprint's own static mesh component"), Owned != nullptr
		&& (*Owned)->GetStringField(TEXT("Scope")) == TEXT("Owned") && (*Owned)->GetStringField(TEXT("Mesh")) == Cube->GetPathName()
		&& (*Owned)->TryGetArrayField(TEXT("MaterialSlots"), Slots) && Slots->Num() == Cube->GetStaticMaterials().Num());

	// A child Blueprint: the parent's component is Inherited, with the child's override.
	UBlueprint* Child = CreateTransientBlueprint(Parent->GeneratedClass, TEXT("DiscoveryMeshChild"));
	if (!TestTrue(TEXT("child Blueprint compiled"), Child != nullptr && Child->GeneratedClass != nullptr))
	{
		return false;
	}
	ON_SCOPE_EXIT { Child->MarkAsGarbage(); };
	UActorComponent* ChildTemplate = Child->GetInheritableComponentHandler(true)->CreateOverridenComponentTemplate(FComponentKey(Prop));
	if (!TestNotNull(TEXT("child override template"), Cast<UStaticMeshComponent>(ChildTemplate)))
	{
		return false;
	}
	Cast<UStaticMeshComponent>(ChildTemplate)->OverrideMaterials = {Override};
	Rows = UE::AssetDocument::Discovery::DescribeMeshComponents(*Child);
	const TSharedPtr<FJsonObject>* Inherited = FindNamed(Rows, TEXT("Name"), TEXT("Prop"));
	const TArray<TSharedPtr<FJsonValue>>* Overrides = nullptr;
	TestTrue(TEXT("the parent's component is Inherited in the child"), Inherited != nullptr
		&& (*Inherited)->GetStringField(TEXT("Scope")) == TEXT("Inherited") && (*Inherited)->GetStringField(TEXT("Mesh")) == Cube->GetPathName());
	TestTrue(TEXT("with the child's OverrideMaterials"), Inherited != nullptr
		&& (*Inherited)->TryGetArrayField(TEXT("OverrideMaterials"), Overrides) && Overrides->Num() == 1
		&& (*Overrides)[0]->AsString() == Override->GetPathName());
	TestNotNull(TEXT("the native Mesh is listed in the child too"), FindNamed(Rows, TEXT("Name"), TEXT("Mesh")));

	// AddAssetFacts puts them on the Blueprint's inspect.
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	UE::AssetDocument::Discovery::AddAssetFacts(*Child, *Payload);
	TestTrue(TEXT("inspect facts carry MeshComponents"), Payload->HasTypedField<EJson::Array>(TEXT("MeshComponents")));
	return true;
}

// inspect over HTTP is summarized unless detail is true: a class's nested struct and instanced defaults are given by
// type and size, so the reflected properties of a large class stay a few KB.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentDiscoveryInspectSummarySizeTest,
	"UEAssetDoc.Discovery.InspectSummarySize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentDiscoveryInspectSummarySizeTest::RunTest(const FString&)
{
	using namespace DiscoveryMeshFactsTests;
	const FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	struct FCase
	{
		const TCHAR* Class;
		int32 Budget;
		const TCHAR* LargeProperty;
	};
	for (const FCase& Case : {FCase{TEXT("/Script/Engine.Character"), 10000, TEXT("Mesh")},
		FCase{TEXT("/Script/Engine.SkeletalMeshComponent"), 20000, TEXT("BodyInstance")}})
	{
		FAssetDocumentInspectRequest Request;
		Request.ClassOrAsset = Case.Class;
		const FAssetDocumentResult Detail = Service.Inspect(Request);
		Request.bSummary = true;
		const FAssetDocumentResult Summary = Service.Inspect(Request);
		if (!TestTrue(FString::Printf(TEXT("%s inspects"), Case.Class), Detail.IsSuccess() && Summary.IsSuccess()))
		{
			continue;
		}
		const int32 SummarySize = CompactSize(Summary);
		const int32 DetailSize = CompactSize(Detail);
		AddInfo(FString::Printf(TEXT("%s: summary %d bytes, detail %d bytes"), Case.Class, SummarySize, DetailSize));
		TestTrue(FString::Printf(TEXT("%s summary within %d bytes"), Case.Class, Case.Budget), SummarySize <= Case.Budget);
		TestTrue(FString::Printf(TEXT("%s summary says so"), Case.Class), Summary.Payload->HasTypedField<EJson::String>(TEXT("Summary")));
		TestTrue(FString::Printf(TEXT("%s summary groups skipped by reason"), Case.Class), Summary.Payload->HasTypedField<EJson::Object>(TEXT("skipped")));
		const TSharedPtr<FJsonObject>* Large = FindRow(Summary, TEXT("properties"), TEXT("name"), Case.LargeProperty);
		TestTrue(FString::Printf(TEXT("%s.%s: type and size, not the value"), Case.Class, Case.LargeProperty), Large != nullptr
			&& (*Large)->HasField(TEXT("type_token")) && (*Large)->HasField(TEXT("size")) && !(*Large)->HasField(TEXT("default_value")));
		const TSharedPtr<FJsonObject>* Whole = FindRow(Detail, TEXT("properties"), TEXT("name"), Case.LargeProperty);
		TestTrue(FString::Printf(TEXT("%s.%s whole with detail"), Case.Class, Case.LargeProperty), Whole != nullptr
			&& (*Whole)->HasTypedField<EJson::Object>(TEXT("default_value")) && !Detail.Payload->HasField(TEXT("Summary")));
	}
	return true;
}
#endif
