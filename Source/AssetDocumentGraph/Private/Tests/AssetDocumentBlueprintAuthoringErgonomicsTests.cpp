// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Blueprint graph authoring ergonomics, on the production route
// (FAssetDocumentService -> Generic Runtime -> Python UBlueprint profile -> graph primitives):
//   UserTypes          user struct Break pins by member name; user enum values by display name (variable
//                                default, pin default, extract); K2:EqualEnum joins two enum pins.
//   OperatorAliases    the short operator keys place their nodes.
//   CastLocalsRows     cast output as Result / As<Class> with or without spaces; K2:Get / K2:Set of a
//                                local variable; Get Data Table Row's RowFound / OutRow.
//   InheritedMembers   K2:Get / K2:CallFunction:Self / K2:AddDelegate of a parent Blueprint's members.
//   OverrideName       a function named like the parent's event says it is treated as an override.
//   AttachSocket       components attach to a socket of their parent.
//   ValidateGraphs     Validate checks node keys, pins and links of a new Blueprint and reports them all;
//                                Apply reports the other problems of the same document too.
//   NodeLister         list_types kind=blueprint_node lists a key's pins without creating anything.
//   PrunedNodeWarning  a node with exec pins whose exec input is not linked, read by a node that runs, is the
//                                compiler's PrunedExecInUse warning: Graph.PrunedNodeValueRead at that node (apply passes,
//                                as the editor compiles it with a warning).
//   KeepsAuthoredIds   extract writes the document's node Ids and action spellings back (IfActive, K2:Min),
//                                also after save and reload, and the next edit by those Ids changes the same nodes.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "HAL/FileManager.h"
#include "K2Node_EnumEquality.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
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
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentBlueprintAuthoringTests
{
struct FAuthoredAsset
{
	FString Name;
	FString PackageName;
	FString ObjectPath;
	FString ClassPath;
	FString PackageFile;
	FString SidecarPath;

	FAuthoredAsset(const FString& Folder, const TCHAR* InName)
		: Name(InName)
	{
		PackageName = FString(TEXT("/Game/")) / Folder / Name;
		ObjectPath = PackageName + TEXT(".") + Name;
		ClassPath = ObjectPath + TEXT("_C");
		PackageFile = FPaths::ConvertRelativePathToFull(
			FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()));
		SidecarPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectContentDir(), Folder, Name + TEXT(".assetdoc.json")));
	}

	UObject* Find() const { return FSoftObjectPath(ObjectPath).ResolveObject(); }
	UBlueprint* Blueprint() const { return Cast<UBlueprint>(Find()); }
};

struct FAuthoringFixture
{
	FAutomationTestBase& Test;
	const FString Folder;
	TArray<TUniquePtr<FAuthoredAsset>> Assets;
	// Tokens replaced in every document: {Name} -> the object path of that fixture asset, {Name_C} -> its class.
	TMap<FString, FString> Tokens;

	explicit FAuthoringFixture(FAutomationTestBase& InTest)
		: Test(InTest)
		, Folder(FString(TEXT("AssetDocumentP68/")) + FGuid::NewGuid().ToString(EGuidFormats::Digits))
	{
	}

	const FAuthoredAsset& Add(const TCHAR* Name)
	{
		const FAuthoredAsset& Asset = *Assets.Add_GetRef(MakeUnique<FAuthoredAsset>(Folder, Name));
		Tokens.Add(FString::Printf(TEXT("{%s}"), Name), Asset.ObjectPath);
		Tokens.Add(FString::Printf(TEXT("{%s_C}"), Name), Asset.ClassPath);
		Tokens.Add(FString::Printf(TEXT("{%s.Package}"), Name), Asset.PackageName);
		return Asset;
	}

	~FAuthoringFixture()
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
		for (const TUniquePtr<FAuthoredAsset>& Asset : Assets)
		{
			IFileManager::Get().Delete(*Asset->PackageFile, false, true);
			IFileManager::Get().Delete(*Asset->SidecarPath, false, true);
			Files.Add(Asset->PackageFile);
		}
		FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles(Files);
		IFileManager::Get().DeleteDirectory(*FPaths::Combine(FPaths::ProjectContentDir(), Folder), false, true);
	}

	FString Expand(const FString& Text) const
	{
		FString Out = Text;
		for (const TPair<FString, FString>& Token : Tokens)
		{
			Out.ReplaceInline(*Token.Key, *Token.Value, ESearchCase::CaseSensitive);
		}
		return Out;
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

	// A document: {"SchemaVersion":1,"Target":<package>,"Class":<class>,"Action":<action>,"Body":{<body>}}.
	FString Document(const FAuthoredAsset& Asset, const TCHAR* Class, const TCHAR* Action, const FString& Body) const
	{
		return Expand(FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"%s\",\"Action\":\"%s\",\"Body\":{%s}}"),
			*Asset.PackageName, Class, Action, *Body));
	}

	FString Blueprint(const FAuthoredAsset& Asset, const TCHAR* Action, const FString& Body) const
	{
		return Document(Asset, TEXT("/Script/Engine.Blueprint"), Action, Body);
	}

	FAssetDocumentResult Apply(const FAuthoredAsset& Asset, const FString& Json, const FString& Stage) const
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Asset.SidecarPath), true);
		FFileHelper::SaveStringToFile(Json, *Asset.SidecarPath);
		FAssetDocumentApplyFileRequest Request;
		Request.FilePath = Asset.SidecarPath;
		Request.bSaveAsset = true;
		const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
		Report(Stage, Result);
		return Result;
	}

	bool ApplyOk(const FAuthoredAsset& Asset, const FString& Json, const FString& Stage) const
	{
		return Test.TestTrue(*FString::Printf(TEXT("%s: applies"), *Stage), Apply(Asset, Json, Stage).IsSuccess());
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

	// Validate, then Apply: a document the editor accepts is valid, and applying it again changes nothing.
	bool ValidateApplyReapply(const FAuthoredAsset& Asset, const FString& Json, const FString& Stage) const
	{
		const bool bValid = Test.TestTrue(*FString::Printf(TEXT("%s: validates"), *Stage), Validate(Json, Stage + TEXT(" validate")).IsSuccess());
		if (!ApplyOk(Asset, Json, Stage))
		{
			return false;
		}
		FString UpdateJson = Json.Replace(TEXT("\"Action\":\"Create\""), TEXT("\"Action\":\"Update\""));
		const FAssetDocumentResult Again = Apply(Asset, UpdateJson, Stage + TEXT(" again"));
		Test.TestTrue(*FString::Printf(TEXT("%s: re-apply succeeds"), *Stage), Again.IsSuccess());
		Test.TestFalse(*FString::Printf(TEXT("%s: re-apply changes nothing"), *Stage), Again.bSavedAsset);
		return bValid && Again.IsSuccess();
	}

	TSharedPtr<FJsonObject> Extract(const FAuthoredAsset& Asset, const FString& Stage) const
	{
		FAssetDocumentExtractRequest Request;
		Request.AssetPath = Asset.ObjectPath;
		const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Extract(Request);
		if (!Test.TestTrue(*FString::Printf(TEXT("%s: extract"), *Stage), Result.IsSuccess() && Result.Payload.IsValid()))
		{
			Report(Stage, Result);
			return nullptr;
		}
		return Result.Payload;
	}

	bool ReapplyExtract(const FAuthoredAsset& Asset, const FString& Stage) const
	{
		const TSharedPtr<FJsonObject> Extracted = Extract(Asset, Stage);
		if (!Extracted.IsValid())
		{
			return false;
		}
		Extracted->SetStringField(TEXT("Action"), TEXT("Update"));
		FString Text;
		const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Extracted.ToSharedRef(), Writer);
		const FAssetDocumentResult Result = Apply(Asset, Text, Stage);
		return Test.TestTrue(*FString::Printf(TEXT("%s: the extract applies back unchanged"), *Stage), Result.IsSuccess() && !Result.bSavedAsset);
	}
};

FString SerializeJson(const TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	return Text;
}

bool HasDiagnostic(const FAssetDocumentResult& Result, const TCHAR* Code, const TCHAR* PathPart, const TCHAR* MessagePart = nullptr)
{
	return Result.Diagnostics.ContainsByPredicate([&](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == Code && Diagnostic.Path.Contains(PathPart)
			&& (MessagePart == nullptr || Diagnostic.Message.Contains(MessagePart));
	});
}

UEdGraph* FindGraph(UBlueprint& Blueprint, const TCHAR* Name)
{
	TArray<UEdGraph*> Graphs;
	Blueprint.GetAllGraphs(Graphs);
	UEdGraph* const* Found = Graphs.FindByPredicate([Name](const UEdGraph* Graph) { return Graph != nullptr && Graph->GetName() == Name; });
	return Found != nullptr ? *Found : nullptr;
}

int64 ReadInteger(const UObject& Object, const TCHAR* PropertyName)
{
	const FProperty* Property = Object.GetClass()->FindPropertyByName(PropertyName);
	const void* Value = Property != nullptr ? Property->ContainerPtrToValuePtr<void>(&Object) : nullptr;
	if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
	{
		return EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(Value);
	}
	const FNumericProperty* Numeric = CastField<FNumericProperty>(Property);
	return Numeric != nullptr ? Numeric->GetSignedIntPropertyValue(Value) : -1;
}

const TCHAR* EnumBody = TEXT(R"JSON("Enumerators":[{"Name":"Normal"},{"Name":"Hit"},{"Name":"Dead"}])JSON");
const TCHAR* StructBody = TEXT(R"JSON("Members":[{"Name":"LaunchVelocity","Type":"struct:/Script/CoreUObject.Vector"},
{"Name":"Damage","Type":"float"},{"Name":"Kind","Type":"byte:{E_State}"}])JSON");
} // namespace AssetDocumentBlueprintAuthoringTests


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintAuthoringUserTypesTest,
	"UEAssetDoc.Blueprint.Authoring.UserTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBlueprintAuthoringUserTypesTest::RunTest(const FString&)
{
	using namespace AssetDocumentBlueprintAuthoringTests;
	FAuthoringFixture Fixture(*this);
	const FAuthoredAsset& Enum = Fixture.Add(TEXT("E_State"));
	const FAuthoredAsset& Struct = Fixture.Add(TEXT("S_Hit"));
	const FAuthoredAsset& Blueprint = Fixture.Add(TEXT("BP_UserTypes"));
	if (!Fixture.ApplyOk(Enum, Fixture.Document(Enum, TEXT("/Script/Engine.UserDefinedEnum"), TEXT("Create"), EnumBody), TEXT("enum"))
		|| !Fixture.ApplyOk(Struct, Fixture.Document(Struct, TEXT("/Script/CoreUObject.UserDefinedStruct"), TEXT("Create"), StructBody), TEXT("struct")))
	{
		return false;
	}
	// Break pins by member name (Break:Damage is the pin Damage_N_<GUID>), enum values by display name, two enum
	// pins on one K2:EqualEnum (the editor's Equal (Enum); a promotable == turns its B into a byte).
	const FString Body = TEXT(R"JSON("ParentClass":"/Script/Engine.Actor",
"Variables":[{"Name":"State","Type":"byte:{E_State}","DefaultValue":"Dead"}],
"FunctionGraphs":[{"Name":"Check",
 "Signature":{"Inputs":[{"Name":"Hit","Type":"struct:{S_Hit}"},{"Name":"Other","Type":"byte:{E_State}"}],
              "Outputs":[{"Name":"Same","Type":"bool"},{"Name":"Damage","Type":"float"}]},
 "Nodes":[{"Id":"Break","Action":"K2:BreakStruct:{S_Hit}"},
          {"Id":"Eq","Action":"K2:EqualEnum"},
          {"Id":"Set","Action":"K2:Set:State","PinOverrides":[{"Pin":"State","DefaultValue":"Hit"}]}],
 "Links":[{"From":"Entry:then","To":"Set:execute"},{"From":"Set:then","To":"Result:execute"},
          {"From":"Entry:Hit","To":"Break:S_Hit"},{"From":"Break:Kind","To":"Eq:A"},{"From":"Entry:Other","To":"Eq:B"},
          {"From":"Eq:ReturnValue","To":"Result:Same"},{"From":"Break:Damage","To":"Result:Damage"}]}])JSON");
	if (!Fixture.ValidateApplyReapply(Blueprint, Fixture.Blueprint(Blueprint, TEXT("Create"), Body), TEXT("user types")))
	{
		return false;
	}
	UBlueprint* Asset = Blueprint.Blueprint();
	if (!TestNotNull(TEXT("Blueprint"), Asset) || !TestNotNull(TEXT("class"), Asset->GeneratedClass.Get()))
	{
		return false;
	}
	TestEqual(TEXT("variable default by display name: Dead"), ReadInteger(*Asset->GeneratedClass->GetDefaultObject(), TEXT("State")), int64(2));
	UEdGraph* Check = FindGraph(*Asset, TEXT("Check"));
	if (!TestNotNull(TEXT("Check graph"), Check))
	{
		return false;
	}
	const TObjectPtr<UEdGraphNode>* Set = Check->Nodes.FindByPredicate([](const UEdGraphNode* Node) { return Node->IsA<UK2Node_VariableSet>(); });
	const UEdGraphPin* StatePin = Set != nullptr ? (*Set)->FindPin(TEXT("State"), EGPD_Input) : nullptr;
	TestTrue(TEXT("pin default by display name stores the enumerator (NewEnumerator1)"), StatePin != nullptr && StatePin->DefaultValue == TEXT("NewEnumerator1"));
	const TObjectPtr<UEdGraphNode>* Eq = Check->Nodes.FindByPredicate([](const UEdGraphNode* Node) { return Node->IsA<UK2Node_EnumEquality>(); });
	TestTrue(TEXT("K2:EqualEnum is an Equal (Enum) node with both enum pins linked"), Eq != nullptr
		&& (*Eq)->FindPin(TEXT("A")) != nullptr && (*Eq)->FindPin(TEXT("A"))->LinkedTo.Num() == 1
		&& (*Eq)->FindPin(TEXT("B")) != nullptr && (*Eq)->FindPin(TEXT("B"))->LinkedTo.Num() == 1);
	// Extract writes display names; the extract applies back unchanged.
	const TSharedPtr<FJsonObject> Extracted = Fixture.Extract(Blueprint, TEXT("user types extract"));
	const FString Text = Extracted.IsValid() ? SerializeJson(Extracted) : FString();
	TestTrue(TEXT("extract writes the variable default by display name"), Text.Contains(TEXT("\"DefaultValue\":\"Dead\"")));
	TestTrue(TEXT("extract writes the pin default by display name"), Text.Contains(TEXT("\"DefaultValue\":\"Hit\"")));
	TestFalse(TEXT("extract writes no internal enumerator names"), Text.Contains(TEXT("NewEnumerator")));
	Fixture.ReapplyExtract(Blueprint, TEXT("user types reapply extract"));
	// The internal names stay accepted (documents written before) and compare equal to the display names.
	FString Internal = Body.Replace(TEXT("\"DefaultValue\":\"Dead\""), TEXT("\"DefaultValue\":\"NewEnumerator2\""))
		.Replace(TEXT("\"DefaultValue\":\"Hit\""), TEXT("\"DefaultValue\":\"NewEnumerator1\""));
	const FAssetDocumentResult Same = Fixture.Apply(Blueprint, Fixture.Blueprint(Blueprint, TEXT("Update"), Internal), TEXT("internal names"));
	TestTrue(TEXT("internal enumerator names apply and are the same values"), Same.IsSuccess() && !Same.bSavedAsset);
	// A value that names no enumerator lists the display names.
	FString Bad = Body.Replace(TEXT("\"DefaultValue\":\"Dead\""), TEXT("\"DefaultValue\":\"Purple\""));
	const FAssetDocumentResult Refused = Fixture.Apply(Blueprint, Fixture.Blueprint(Blueprint, TEXT("Update"), Bad), TEXT("unknown enumerator"));
	TestFalse(TEXT("an unknown enumerator is refused"), Refused.IsSuccess());
	TestTrue(TEXT("the refusal lists the display names"), Refused.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Message.Contains(TEXT("Normal")) && Diagnostic.Message.Contains(TEXT("Dead"));
	}));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintAuthoringOperatorAliasesTest,
	"UEAssetDoc.Blueprint.Authoring.OperatorAliases",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBlueprintAuthoringOperatorAliasesTest::RunTest(const FString&)
{
	using namespace AssetDocumentBlueprintAuthoringTests;
	FAuthoringFixture Fixture(*this);
	const FAuthoredAsset& Blueprint = Fixture.Add(TEXT("BP_Operators"));
	const TCHAR* Aliases[] = {
		TEXT("K2:EqualEqual"), TEXT("K2:NotEqual"), TEXT("K2:Less"), TEXT("K2:Greater"), TEXT("K2:LessEqual"), TEXT("K2:GreaterEqual"),
		TEXT("K2:Add"), TEXT("K2:Subtract"), TEXT("K2:Multiply"), TEXT("K2:Divide"),
		TEXT("K2:EqualEnum"), TEXT("K2:NotEqualEnum"),
		TEXT("K2:SelectFloat"), TEXT("K2:SelectInt"), TEXT("K2:SelectString"), TEXT("K2:SelectName"), TEXT("K2:SelectText"),
		TEXT("K2:SelectVector"), TEXT("K2:SelectRotator"), TEXT("K2:SelectTransform"), TEXT("K2:SelectColor"),
		TEXT("K2:SelectObject"), TEXT("K2:SelectClass"),
		TEXT("K2:And"), TEXT("K2:Or"), TEXT("K2:Xor"), TEXT("K2:Nand"), TEXT("K2:Nor"), TEXT("K2:Not"),
		TEXT("K2:Abs"), TEXT("K2:AbsInt"), TEXT("K2:Min"), TEXT("K2:Max"), TEXT("K2:MinInt"), TEXT("K2:MaxInt"),
		TEXT("K2:Clamp"), TEXT("K2:ClampInt"), TEXT("K2:Lerp"), TEXT("K2:Percent"), TEXT("K2:PercentInt"),
		TEXT("K2:NearlyEqual"), TEXT("K2:InRange")};
	TArray<FString> Nodes;
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Aliases); ++Index)
	{
		Nodes.Add(FString::Printf(TEXT("{\"Id\":\"Op%d\",\"Action\":\"%s\"}"), Index, Aliases[Index]));
	}
	const FString Body = FString::Printf(TEXT("\"ParentClass\":\"/Script/Engine.Actor\",\"FunctionGraphs\":[{\"Name\":\"Ops\",\"Nodes\":[%s],"
		"\"Links\":[{\"From\":\"Entry:then\",\"To\":\"Result:execute\"}]}]"), *FString::Join(Nodes, TEXT(",")));
	if (!Fixture.ValidateApplyReapply(Blueprint, Fixture.Blueprint(Blueprint, TEXT("Create"), Body), TEXT("operator aliases")))
	{
		return false;
	}
	UEdGraph* Ops = Blueprint.Blueprint() != nullptr ? FindGraph(*Blueprint.Blueprint(), TEXT("Ops")) : nullptr;
	TestTrue(TEXT("every alias placed one node (plus entry and result)"), Ops != nullptr && Ops->Nodes.Num() == UE_ARRAY_COUNT(Aliases) + 2);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintAuthoringCastLocalsRowsTest,
	"UEAssetDoc.Blueprint.Authoring.CastLocalsRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBlueprintAuthoringCastLocalsRowsTest::RunTest(const FString&)
{
	using namespace AssetDocumentBlueprintAuthoringTests;
	FAuthoringFixture Fixture(*this);
	const FAuthoredAsset& Enum = Fixture.Add(TEXT("E_State"));
	const FAuthoredAsset& Struct = Fixture.Add(TEXT("S_Hit"));
	const FAuthoredAsset& Table = Fixture.Add(TEXT("DT_Hits"));
	const FAuthoredAsset& Blueprint = Fixture.Add(TEXT("BP_CastLocals"));
	if (!Fixture.ApplyOk(Enum, Fixture.Document(Enum, TEXT("/Script/Engine.UserDefinedEnum"), TEXT("Create"), EnumBody), TEXT("enum"))
		|| !Fixture.ApplyOk(Struct, Fixture.Document(Struct, TEXT("/Script/CoreUObject.UserDefinedStruct"), TEXT("Create"), StructBody), TEXT("struct"))
		|| !Fixture.ApplyOk(Table, Fixture.Document(Table, TEXT("/Script/Engine.DataTable"), TEXT("Create"),
			TEXT(R"JSON("RowStruct":"{S_Hit}","Rows":[{"Name":"Light","Values":{"Damage":5,"Kind":"Hit"}}])JSON")), TEXT("table")))
	{
		return false;
	}
	for (const TCHAR* CastPin : {TEXT("Result"), TEXT("AsCharacter"), TEXT("As Character")})
	{
		const FString Body = FString::Printf(TEXT(R"JSON("ParentClass":"/Script/Engine.Actor",
"FunctionGraphs":[{"Name":"Probe",
 "Signature":{"Inputs":[{"Name":"Target","Type":"object:/Script/Engine.Actor"}],"Outputs":[{"Name":"Damage","Type":"float"}]},
 "LocalVariables":[{"Name":"Tmp","Type":"int"}],
 "Nodes":[{"Id":"Cast","Action":"K2:DynamicCast:/Script/Engine.Character"},
          {"Id":"Jump","Action":"K2:CallFunction:/Script/Engine.Character:Jump"},
          {"Id":"SetTmp","Action":"K2:Set:Tmp","PinOverrides":[{"Pin":"Tmp","DefaultValue":"3"}]},
          {"Id":"GetTmp","Action":"K2:Get:Tmp"},
          {"Id":"Row","Action":"K2:GetDataTableRow","PinOverrides":[{"Pin":"DataTable","DefaultObject":"{DT_Hits}"},{"Pin":"RowName","DefaultValue":"Light"}]},
          {"Id":"Break","Action":"K2:BreakStruct:{S_Hit}"},
          {"Id":"Print","Action":"K2:PrintString"}],
 "Links":[{"From":"Entry:then","To":"Cast:execute"},{"From":"Entry:Target","To":"Cast:Object"},
          {"From":"Cast:then","To":"Jump:execute"},{"From":"Cast:%s","To":"Jump:self"},
          {"From":"Jump:then","To":"SetTmp:execute"},{"From":"SetTmp:then","To":"Row:execute"},
          {"From":"Row:RowFound","To":"Print:execute"},{"From":"Row:OutRow","To":"Break:S_Hit"},
          {"From":"Print:then","To":"Result:execute"},{"From":"Break:Damage","To":"Result:Damage"}]}])JSON"), CastPin);
		const FString Stage = FString::Printf(TEXT("cast pin '%s'"), CastPin);
		if (!Fixture.ValidateApplyReapply(Blueprint, Fixture.Blueprint(Blueprint, TEXT("CreateOrUpdate"), Body), Stage))
		{
			return false;
		}
	}
	UBlueprint* Asset = Blueprint.Blueprint();
	UEdGraph* Probe = Asset != nullptr ? FindGraph(*Asset, TEXT("Probe")) : nullptr;
	if (!TestNotNull(TEXT("Probe graph"), Probe))
	{
		return false;
	}
	int32 LocalNodes = 0;
	for (const UEdGraphNode* Node : Probe->Nodes)
	{
		if (const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node))
		{
			LocalNodes += Variable->VariableReference.IsLocalScope() ? 1 : 0;
		}
	}
	TestEqual(TEXT("K2:Get:Tmp / K2:Set:Tmp in the function are its local variable's nodes"), LocalNodes, 2);
	Fixture.ReapplyExtract(Blueprint, TEXT("cast locals reapply extract"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintAuthoringInheritedMembersTest,
	"UEAssetDoc.Blueprint.Authoring.InheritedMembers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBlueprintAuthoringInheritedMembersTest::RunTest(const FString&)
{
	using namespace AssetDocumentBlueprintAuthoringTests;
	FAuthoringFixture Fixture(*this);
	const FAuthoredAsset& Base = Fixture.Add(TEXT("BP_Base"));
	const FAuthoredAsset& Child = Fixture.Add(TEXT("BP_Child"));
	if (!Fixture.ApplyOk(Base, Fixture.Blueprint(Base, TEXT("Create"), TEXT(R"JSON("ParentClass":"/Script/Engine.Actor",
"Variables":[{"Name":"Health","Type":"float","DefaultValue":100.0}],
"Components":[{"Name":"Combat","Class":"/Script/Engine.SceneComponent"}],
"EventDispatchers":[{"Name":"OnDied"}],
"FunctionGraphs":[{"Name":"Heal","Links":[{"From":"Entry:then","To":"Result:execute"}]}])JSON")), TEXT("base")))
	{
		return false;
	}
	// Short forms of the parent's members, as for the Blueprint's own: the editor's palette offers them in the child.
	const FString Body = TEXT(R"JSON("ParentClass":"{BP_Base_C}",
"UbergraphPages":[{"Name":"EventGraph",
 "Nodes":[{"Id":"Begin","Action":"K2:BeginPlay"},
          {"Id":"Heal","Action":"K2:CallFunction:Self:Heal"},
          {"Id":"Bind","Action":"K2:AddDelegate:OnDied"},
          {"Id":"Died","Action":"K2:CustomEvent","Name":"HandleDied"},
          {"Id":"Health","Action":"K2:Get:Health"},
          {"Id":"Combat","Action":"K2:Get:Combat"},
          {"Id":"Print","Action":"K2:PrintString"},
          {"Id":"ToText","Action":"K2:CallFunction:/Script/Engine.KismetStringLibrary:Conv_DoubleToString"},
          {"Id":"Visible","Action":"K2:CallFunction:/Script/Engine.SceneComponent:SetVisibility"}],
 "Links":[{"From":"Begin:then","To":"Heal:execute"},{"From":"Heal:then","To":"Bind:execute"},
          {"From":"Died:OutputDelegate","To":"Bind:Delegate"},{"From":"Bind:then","To":"Print:execute"},
          {"From":"Health:Health","To":"ToText:InDouble"},{"From":"ToText:ReturnValue","To":"Print:InString"},
          {"From":"Print:then","To":"Visible:execute"},{"From":"Combat:Combat","To":"Visible:self"}]}])JSON");
	if (!Fixture.ValidateApplyReapply(Child, Fixture.Blueprint(Child, TEXT("Create"), Body), TEXT("inherited short forms")))
	{
		return false;
	}
	Fixture.ReapplyExtract(Child, TEXT("inherited reapply extract"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintAuthoringOverrideNameTest,
	"UEAssetDoc.Blueprint.Authoring.OverrideName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBlueprintAuthoringOverrideNameTest::RunTest(const FString&)
{
	using namespace AssetDocumentBlueprintAuthoringTests;
	FAuthoringFixture Fixture(*this);
	const FAuthoredAsset& Blueprint = Fixture.Add(TEXT("BP_Override"));
	const FAssetDocumentResult Result = Fixture.Apply(Blueprint, Fixture.Blueprint(Blueprint, TEXT("Create"), TEXT(R"JSON("ParentClass":"/Script/Engine.Actor",
"FunctionGraphs":[{"Name":"ReceiveHit","Signature":{"Inputs":[{"Name":"Amount","Type":"float"}]},
 "Links":[{"From":"Entry:then","To":"Result:execute"}]}])JSON")), TEXT("function named like a parent event"));
	TestFalse(TEXT("the editor treats it as an override: custom inputs are refused"), Result.IsSuccess());
	TestTrue(TEXT("the message names the parent event and the override"), Result.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == TEXT("Graph.UserPinsUnsupported") && Diagnostic.Message.Contains(TEXT("AActor::ReceiveHit"))
			&& Diagnostic.Message.Contains(TEXT("override")) && Diagnostic.Message.Contains(TEXT("K2:Event:/Script/Engine.Actor:ReceiveHit"));
	}));
	TestNull(TEXT("the failed create leaves no asset"), Blueprint.Find());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintAuthoringAttachSocketTest,
	"UEAssetDoc.Blueprint.Authoring.AttachSocket",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBlueprintAuthoringAttachSocketTest::RunTest(const FString&)
{
	using namespace AssetDocumentBlueprintAuthoringTests;
	FAuthoringFixture Fixture(*this);
	const FAuthoredAsset& Mesh = Fixture.Add(TEXT("SM_Sockets"));
	const FAuthoredAsset& Blueprint = Fixture.Add(TEXT("BP_Attach"));
	// A static mesh with sockets "hand_r" and "hand_l" (the mesh itself needs no geometry for sockets).
	{
		UPackage* Package = CreatePackage(*Mesh.PackageName);
		UStaticMesh* Asset = NewObject<UStaticMesh>(Package, *Mesh.Name, RF_Public | RF_Standalone | RF_Transactional);
		for (const TCHAR* SocketName : {TEXT("hand_r"), TEXT("hand_l")})
		{
			UStaticMeshSocket* Socket = NewObject<UStaticMeshSocket>(Asset);
			Socket->SocketName = SocketName;
			Asset->AddSocket(Socket);
		}
		FAssetRegistryModule::AssetCreated(Asset);
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		if (!TestTrue(TEXT("save the socket mesh"), UPackage::SavePackage(Package, Asset, *Mesh.PackageFile, Args)))
		{
			return false;
		}
	}
	// The mesh is set in the same document: the socket is written after the component values.
	const FString Body = TEXT(R"JSON("ParentClass":"/Script/Engine.Actor",
"Components":[{"Name":"Body","Class":"/Script/Engine.StaticMeshComponent","Properties":{"StaticMesh":"{SM_Sockets}"}},
              {"Name":"Spear","Class":"/Script/Engine.StaticMeshComponent","AttachTo":"Body","AttachSocket":"hand_r"}])JSON");
	if (!Fixture.ValidateApplyReapply(Blueprint, Fixture.Blueprint(Blueprint, TEXT("Create"), Body), TEXT("attach socket")))
	{
		return false;
	}
	auto SocketOf = [&Blueprint]() -> FName
	{
		UBlueprint* Asset = Blueprint.Blueprint();
		USCS_Node* Node = Asset != nullptr && Asset->SimpleConstructionScript != nullptr ? Asset->SimpleConstructionScript->FindSCSNode(TEXT("Spear")) : nullptr;
		return Node != nullptr ? Node->AttachToName : FName(TEXT("<missing>"));
	};
	TestEqual(TEXT("SCS AttachToName"), SocketOf(), FName(TEXT("hand_r")));
	const TSharedPtr<FJsonObject> Extracted = Fixture.Extract(Blueprint, TEXT("attach extract"));
	TestTrue(TEXT("extract writes AttachSocket"), Extracted.IsValid() && SerializeJson(Extracted).Contains(TEXT("\"AttachSocket\":\"hand_r\"")));
	Fixture.ReapplyExtract(Blueprint, TEXT("attach reapply extract"));
	// Another socket, then none.
	TestTrue(TEXT("change the socket"), Fixture.Apply(Blueprint, Fixture.Blueprint(Blueprint, TEXT("Update"),
		Body.Replace(TEXT("hand_r"), TEXT("hand_l"))), TEXT("socket hand_l")).IsSuccess());
	TestEqual(TEXT("socket changed"), SocketOf(), FName(TEXT("hand_l")));
	TestTrue(TEXT("clear the socket"), Fixture.Apply(Blueprint, Fixture.Blueprint(Blueprint, TEXT("Update"),
		Body.Replace(TEXT(",\"AttachSocket\":\"hand_r\""), TEXT(""))), TEXT("no socket")).IsSuccess());
	TestEqual(TEXT("socket cleared"), SocketOf(), FName(NAME_None));
	// A socket the parent's mesh does not have lists the sockets it has.
	const FAssetDocumentResult Missing = Fixture.Apply(Blueprint, Fixture.Blueprint(Blueprint, TEXT("Update"),
		Body.Replace(TEXT("hand_r"), TEXT("foot_r"))), TEXT("unknown socket"));
	TestFalse(TEXT("an unknown socket is refused"), Missing.IsSuccess());
	TestTrue(TEXT("the refusal lists the sockets"), HasDiagnostic(Missing, TEXT("BlueprintMember.InvalidAttach"), TEXT("/Body/Components/1/AttachSocket"), TEXT("hand_l")));
	// AttachSocket without AttachTo is refused before any change.
	const FAssetDocumentResult NoParent = Fixture.Validate(Fixture.Blueprint(Blueprint, TEXT("Update"), TEXT(R"JSON("ParentClass":"/Script/Engine.Actor",
"Components":[{"Name":"Spear","Class":"/Script/Engine.StaticMeshComponent","AttachSocket":"hand_r"}])JSON")), TEXT("socket without parent"));
	TestTrue(TEXT("AttachSocket needs AttachTo"), !NoParent.IsSuccess() && HasDiagnostic(NoParent, TEXT("UBlueprint.InvalidAttach"), TEXT("AttachSocket")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintAuthoringValidateGraphsTest,
	"UEAssetDoc.Blueprint.Authoring.ValidateGraphs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBlueprintAuthoringValidateGraphsTest::RunTest(const FString&)
{
	using namespace AssetDocumentBlueprintAuthoringTests;
	FAuthoringFixture Fixture(*this);
	const FAuthoredAsset& Probe = Fixture.Add(TEXT("BP_Probe"));
	// A new Blueprint with three independent mistakes: a pin the Branch does not have, a key that names no action,
	// and a link the editor makes only through a conversion node.
	const FString Body = TEXT(R"JSON("ParentClass":"/Script/Engine.Actor",
"UbergraphPages":[{"Name":"EventGraph",
 "Nodes":[{"Id":"Begin","Action":"K2:BeginPlay"},{"Id":"B","Action":"K2:Branch"},
          {"Id":"Print","Action":"K2:CallFunction:/Script/Engine.KismetSystemLibrary:PrintStrin"},
          {"Id":"Rand","Action":"K2:CallFunction:/Script/Engine.KismetMathLibrary:RandomInteger"},
          {"Id":"Delay","Action":"K2:Delay"}],
 "Links":[{"From":"Begin:then","To":"B:Bogus"},{"From":"Rand:ReturnValue","To":"B:Condition"},
          {"From":"B:then","To":"Delay:execute"}]}])JSON");
	const FString Json = Fixture.Blueprint(Probe, TEXT("Create"), Body);
	const FAssetDocumentResult Validated = Fixture.Validate(Json, TEXT("bad graph validate"));
	TestFalse(TEXT("a new Blueprint with bad links is not valid"), Validated.IsSuccess());
	TestTrue(TEXT("Validate reports the missing pin"), HasDiagnostic(Validated, TEXT("Graph.PinNotFound"), TEXT("/Links/0"), TEXT("Bogus")));
	TestTrue(TEXT("Validate reports the unknown key with suggestions"), HasDiagnostic(Validated, TEXT("Graph.ActionNotFound"), TEXT("/Nodes/2/Action"), TEXT("PrintString")));
	TestTrue(TEXT("Validate reports the conversion link"), HasDiagnostic(Validated, TEXT("Graph.ConnectNeedsConversion"), TEXT("/Links/1")));
	TestNull(TEXT("Validate creates nothing"), Probe.Find());
	TestFalse(TEXT("Validate leaves no package file"), IFileManager::Get().FileExists(*Probe.PackageFile));
	// Apply stops at its first failure and reports the other problems of the document with it.
	const FAssetDocumentResult Applied = Fixture.Apply(Probe, Json, TEXT("bad graph apply"));
	TestFalse(TEXT("Apply refuses"), Applied.IsSuccess());
	int32 GraphProblems = 0;
	for (const TCHAR* Code : {TEXT("Graph.PinNotFound"), TEXT("Graph.ActionNotFound"), TEXT("Graph.ConnectNeedsConversion")})
	{
		GraphProblems += Applied.Diagnostics.ContainsByPredicate([Code](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == Code; }) ? 1 : 0;
	}
	TestEqual(TEXT("Apply reports all three problems in one pass"), GraphProblems, 3);
	TestNull(TEXT("the failed create leaves no asset"), Probe.Find());
	// The same document fixed validates on the new target and applies.
	const FString Fixed = Body.Replace(TEXT("PrintStrin\""), TEXT("PrintString\"")).Replace(TEXT("B:Bogus"), TEXT("B:execute"))
		.Replace(TEXT("{\"From\":\"Rand:ReturnValue\",\"To\":\"B:Condition\"},"), TEXT(""));
	Fixture.ValidateApplyReapply(Probe, Fixture.Blueprint(Probe, TEXT("Create"), Fixed), TEXT("fixed graph"));
	// On the existing target, Validate reports a bad pin too.
	const FAssetDocumentResult Existing = Fixture.Validate(Fixture.Blueprint(Probe, TEXT("Update"), Fixed.Replace(TEXT("Delay:execute"), TEXT("Delay:Go"))),
		TEXT("existing target validate"));
	TestTrue(TEXT("Validate of an existing target reports the missing pin"), !Existing.IsSuccess()
		&& HasDiagnostic(Existing, TEXT("Graph.PinNotFound"), TEXT("/Links/"), TEXT("Go")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintAuthoringNodeListerTest,
	"UEAssetDoc.Blueprint.Authoring.NodeLister",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBlueprintAuthoringNodeListerTest::RunTest(const FString&)
{
	using namespace AssetDocumentBlueprintAuthoringTests;
	FAuthoringFixture Fixture(*this);
	const FAuthoredAsset& Enum = Fixture.Add(TEXT("E_State"));
	const FAuthoredAsset& Struct = Fixture.Add(TEXT("S_Hit"));
	if (!Fixture.ApplyOk(Enum, Fixture.Document(Enum, TEXT("/Script/Engine.UserDefinedEnum"), TEXT("Create"), EnumBody), TEXT("enum"))
		|| !Fixture.ApplyOk(Struct, Fixture.Document(Struct, TEXT("/Script/CoreUObject.UserDefinedStruct"), TEXT("Create"), StructBody), TEXT("struct")))
	{
		return false;
	}
	auto List = [this](const TMap<FString, FString>& Query) -> TArray<TSharedPtr<FJsonValue>>
	{
		const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ListTypes(Query);
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (!TestTrue(TEXT("list_types succeeds"), Result.IsSuccess() && Result.Payload.IsValid() && Result.Payload->TryGetArrayField(TEXT("Types"), Rows)))
		{
			AddInfo(Result.Message);
			return {};
		}
		return *Rows;
	};
	auto PinsOf = [](const TSharedPtr<FJsonValue>& Row) -> FString
	{
		return Row.IsValid() && Row->AsObject().IsValid() ? SerializeJson(Row->AsObject()) : FString();
	};
	// The user struct's Break: pin names with their member display names.
	TArray<TSharedPtr<FJsonValue>> Rows = List({{TEXT("kind"), TEXT("blueprint_node")}, {TEXT("name"), Fixture.Expand(TEXT("K2:BreakStruct:{S_Hit}"))}});
	const FString Break = Rows.Num() == 1 ? PinsOf(Rows[0]) : FString();
	TestTrue(TEXT("Break lists the member pins with their display names"), Break.Contains(TEXT("\"DisplayName\":\"Damage\"")) && Break.Contains(TEXT("Damage_")));
	// An alias, a cast and Get Data Table Row: direction, type and display names.
	Rows = List({{TEXT("kind"), TEXT("blueprint_node")}, {TEXT("name"), TEXT("K2:GetDataTableRow")}});
	const FString Row = Rows.Num() == 1 ? PinsOf(Rows[0]) : FString();
	TestTrue(TEXT("Get Data Table Row lists then as Row Found"), Row.Contains(TEXT("\"Name\":\"then\"")) && Row.Contains(TEXT("Row Found")));
	Rows = List({{TEXT("kind"), TEXT("blueprint_node")}, {TEXT("name"), TEXT("K2:DynamicCast:/Script/Engine.Character")}});
	const FString Cast = Rows.Num() == 1 ? PinsOf(Rows[0]) : FString();
	TestTrue(TEXT("the cast lists AsCharacter as an output"), Cast.Contains(TEXT("AsCharacter")) && Cast.Contains(TEXT("\"Direction\":\"output\"")));
	Rows = List({{TEXT("kind"), TEXT("blueprint_node")}, {TEXT("name"), TEXT("K2:SelectFloat")}});
	TestEqual(TEXT("an alias resolves"), Rows.Num(), 1);
	// Words search the keys.
	Rows = List({{TEXT("kind"), TEXT("blueprint_node")}, {TEXT("name"), TEXT("print string")}});
	TestTrue(TEXT("words find PrintString"), Rows.ContainsByPredicate([&PinsOf](const TSharedPtr<FJsonValue>& Value) { return PinsOf(Value).Contains(TEXT("PrintString")); }));
	// An unknown key fails with suggestions.
	const FAssetDocumentResult Unknown = FAssetDocumentModule::Get().GetService().ListTypes({{TEXT("kind"), TEXT("blueprint_node")}, {TEXT("name"), TEXT("K2:PrintStrin")}});
	TestTrue(TEXT("an unknown key fails with suggestions"), !Unknown.IsSuccess() && Unknown.Message.Contains(TEXT("PrintString")));
	return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintAuthoringPrunedNodeWarningTest,
	"UEAssetDoc.Blueprint.Authoring.PrunedNodeWarning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBlueprintAuthoringPrunedNodeWarningTest::RunTest(const FString&)
{
	using namespace AssetDocumentBlueprintAuthoringTests;
	FAuthoringFixture Fixture(*this);
	const FAuthoredAsset& Blueprint = Fixture.Add(TEXT("BP_Pruned"));
	// Does Data Table Row Exist (an impure node) left without its exec link while Print String reads its output.
	const FAssetDocumentResult Result = Fixture.Apply(Blueprint, Fixture.Blueprint(Blueprint, TEXT("Create"), TEXT(R"JSON("ParentClass":"/Script/Engine.Actor",
"UbergraphPages":[{"Name":"EventGraph","Nodes":[{"Id":"BeginPlay","Action":"K2:BeginPlay"},
 {"Id":"Print","Action":"K2:PrintString"},
 {"Id":"Level","Action":"K2:CallFunction:/Script/Engine.GameplayStatics:GetCurrentLevelName"}],
 "Links":[{"From":"BeginPlay:then","To":"Print:execute"},{"From":"Level:ReturnValue","To":"Print:InString"}]}])JSON")),
		TEXT("value read from a node that never runs"));
	TestTrue(TEXT("the editor compiles it (with a warning): apply passes"), Result.IsSuccess());
	TestTrue(TEXT("the pruned node's warning names it, in document terms"), Result.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == TEXT("Graph.PrunedNodeValueRead") && Diagnostic.Severity.Equals(TEXT("Warning"), ESearchCase::IgnoreCase)
			&& Diagnostic.Path == TEXT("/Body/UbergraphPages/0/Nodes/2") && Diagnostic.Message.Contains(TEXT("'Level'"));
	}));
	return !HasAnyErrors();
}

namespace AssetDocumentBlueprintAuthoringTests
{
// Id -> Action of the extracted EventGraph's nodes, and its links as "From>To".
void ReadEventGraph(const TSharedPtr<FJsonObject>& Document, TMap<FString, FString>& Nodes, TSet<FString>& Links)
{
	Nodes.Reset();
	Links.Reset();
	const TSharedPtr<FJsonObject>* Body = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Pages = nullptr;
	if (!Document.IsValid() || !Document->TryGetObjectField(TEXT("Body"), Body) || !(*Body)->TryGetArrayField(TEXT("UbergraphPages"), Pages))
	{
		return;
	}
	for (const TSharedPtr<FJsonValue>& Page : *Pages)
	{
		const TSharedPtr<FJsonObject> Graph = Page->AsObject();
		if (!Graph.IsValid() || Graph->GetStringField(TEXT("Name")) != TEXT("EventGraph"))
		{
			continue;
		}
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Graph->TryGetArrayField(TEXT("Nodes"), Values))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				FString Action;
				Value->AsObject()->TryGetStringField(TEXT("Action"), Action);
				Nodes.Add(Value->AsObject()->GetStringField(TEXT("Id")), Action);
			}
		}
		if (Graph->TryGetArrayField(TEXT("Links"), Values))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				FString From;
				FString To;
				Value->AsObject()->TryGetStringField(TEXT("From"), From);
				Value->AsObject()->TryGetStringField(TEXT("To"), To);
				Links.Add(From + TEXT(">") + To);
			}
		}
	}
}

// The authored Ids and action keys, as the document wrote them.
void TestAuthoredIds(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Document, const TCHAR* Stage)
{
	TMap<FString, FString> Nodes;
	TSet<FString> Links;
	ReadEventGraph(Document, Nodes, Links);
	const TCHAR* Expected[][2] = {{TEXT("Begin"), TEXT("K2:BeginPlay")}, {TEXT("IfActive"), TEXT("K2:Branch")},
		{TEXT("Active"), TEXT("K2:Get:bActive")}, {TEXT("Clamp"), TEXT("K2:Min")}, {TEXT("Heal"), TEXT("K2:Set:Health")}};
	for (const auto& Pair : Expected)
	{
		Test.TestEqual(FString::Printf(TEXT("%s: node %s keeps its Id and Action"), Stage, Pair[0]), Nodes.FindRef(Pair[0]), FString(Pair[1]));
	}
	Test.TestEqual(FString::Printf(TEXT("%s: five nodes"), Stage), Nodes.Num(), 5);
	Test.TestTrue(FString::Printf(TEXT("%s: links name the authored Ids"), Stage), Links.Contains(TEXT("Begin:then>IfActive:execute"))
		&& Links.Contains(TEXT("IfActive:then>Heal:execute")) && Links.Contains(TEXT("Clamp:ReturnValue>Heal:Health")));
}
} // namespace AssetDocumentBlueprintAuthoringTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintAuthoringKeepsAuthoredIdsTest,
	"UEAssetDoc.Blueprint.Authoring.KeepsAuthoredIds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBlueprintAuthoringKeepsAuthoredIdsTest::RunTest(const FString&)
{
	using namespace AssetDocumentBlueprintAuthoringTests;
	FAuthoringFixture Fixture(*this);
	const FAuthoredAsset& Blueprint = Fixture.Add(TEXT("BP_Pickup"));
	// Extract used to return Branch for IfActive and an FMin call for K2:Min, so the next edit had to
	// use other Ids.
	const FString Body = TEXT(R"JSON("ParentClass":"/Script/Engine.Actor",
"Variables":[{"Name":"Health","Type":"float","DefaultValue":100.0},{"Name":"bActive","Type":"bool","DefaultValue":true}],
"UbergraphPages":[{"Name":"EventGraph","Nodes":[{"Id":"Begin","Action":"K2:BeginPlay"},
 {"Id":"IfActive","Action":"K2:Branch"},{"Id":"Active","Action":"K2:Get:bActive"},
 {"Id":"Clamp","Action":"K2:Min","PinOverrides":[{"Pin":"A","DefaultValue":"150"},{"Pin":"B","DefaultValue":"%s"}]},
 {"Id":"Heal","Action":"K2:Set:Health"}],
 "Links":[{"From":"Begin:then","To":"IfActive:execute"},{"From":"Active:bActive","To":"IfActive:Condition"},
  {"From":"IfActive:then","To":"Heal:execute"},{"From":"Clamp:ReturnValue","To":"Heal:Health"}]}])JSON");
	if (!Fixture.ApplyOk(Blueprint, Fixture.Blueprint(Blueprint, TEXT("Create"), Body.Replace(TEXT("%s"), TEXT("100"))), TEXT("authored Ids")))
	{
		return false;
	}
	// Without the saved document beside it (an MCP apply keeps none): the Ids come from the nodes.
	IFileManager::Get().Delete(*Blueprint.SidecarPath, false, true);
	TestAuthoredIds(*this, Fixture.Extract(Blueprint, TEXT("extract")), TEXT("extract"));

	// Save (the apply saved the package) and reload: the Ids are stored with the nodes.
	UPackage* Package = FindPackage(nullptr, *Blueprint.PackageName);
	if (!TestNotNull(TEXT("package loaded"), Package))
	{
		return false;
	}
	TestFalse(TEXT("the apply saved the package"), Package->IsDirty());
	{
		ResetLoaders(Package);
		TArray<UPackage*> ToUnload{Package};
		FText UnloadError;
		TestTrue(TEXT("package unloaded"), UPackageTools::UnloadPackages(ToUnload, UnloadError, /*bUnloadDirtyPackages=*/true));
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}
	TestNull(TEXT("Blueprint is gone from memory"), Blueprint.Find());
	TestNotNull(TEXT("package reloaded"), LoadPackage(nullptr, *Blueprint.PackageName, LOAD_None));
	TestAuthoredIds(*this, Fixture.Extract(Blueprint, TEXT("extract after reload")), TEXT("after reload"));
	UEdGraph* EventGraph = Blueprint.Blueprint() != nullptr ? FindGraph(*Blueprint.Blueprint(), TEXT("EventGraph")) : nullptr;
	if (!TestNotNull(TEXT("EventGraph"), EventGraph))
	{
		return false;
	}
	TSet<FGuid> Before;
	for (const UEdGraphNode* Node : EventGraph->Nodes)
	{
		Before.Add(Node->NodeGuid);
	}

	// The next edit names the same Ids: it changes that pin, and no node is replaced.
	IFileManager::Get().Delete(*Blueprint.SidecarPath, false, true);
	const FAssetDocumentResult Edit = Fixture.Apply(Blueprint, Fixture.Blueprint(Blueprint, TEXT("Update"), Body.Replace(TEXT("%s"), TEXT("120"))), TEXT("edit by Id"));
	TestTrue(TEXT("edit by the authored Ids applies and changes the asset"), Edit.IsSuccess() && Edit.bSavedAsset);
	TSet<FGuid> After;
	FString ClampB;
	for (const UEdGraphNode* Node : EventGraph->Nodes)
	{
		After.Add(Node->NodeGuid);
		if (const UEdGraphPin* Pin = Node->FindPin(TEXT("B"), EGPD_Input); Pin != nullptr && Node->FindPin(TEXT("A"), EGPD_Input) != nullptr)
		{
			ClampB = Pin->DefaultValue;
		}
	}
	TestTrue(TEXT("the same nodes (no node replaced)"), Before.Num() == After.Num() && Before.Includes(After));
	TestEqual(TEXT("Clamp's B changed"), FCString::Atod(*ClampB), 120.0);
	IFileManager::Get().Delete(*Blueprint.SidecarPath, false, true);
	TestAuthoredIds(*this, Fixture.Extract(Blueprint, TEXT("extract after edit")), TEXT("after edit"));
	return !HasAnyErrors();
}

#endif
