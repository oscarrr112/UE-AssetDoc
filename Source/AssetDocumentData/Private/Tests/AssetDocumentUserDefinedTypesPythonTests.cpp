// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// End-to-end User Defined Struct / Enum Profiles (profile.user_defined_struct.v1,
// profile.user_defined_enum.v1) through the production Service:
//
// UEAssetDoc.UserDefinedTypes.Struct: one ApplyFile creates a struct (StructureFactory)
// and populates members of every type kind with defaults and settings; Extract/Diff agree; the
// extraction re-applies as a no-op (no save, package file untouched); a second apply renames
// (RenamedFrom), adds, retypes, reorders and removes members; an invalid type, default or setting
// fails at its document path and changes nothing; a failure in the middle of the member edits
// rolls back; Inspect explains the type form; the saved package reloads to the same document.
// UEAssetDoc.UserDefinedTypes.Enum: the same for enumerators (EnumFactory).
// UEAssetDoc.UserDefinedTypes.Dependents: a DataTable using the struct and a
// Blueprint with variables of the struct / enum: member rename / add keep the rows, retype is
// recorded, every updated package is reported and stays saved; an apply is refused while a
// dependent has unsaved changes; a failed apply leaves the dependents as they were.
// UEAssetDoc.UserDefinedTypes.Undo: human edit + save, AI apply, Ctrl+Z / Ctrl+Y
// stay safe; a failed AI apply keeps the human's undo step.
// UEAssetDoc.UserDefinedTypes.EnumUndo: the same for an enum. UE 5.7 cannot undo an
// Enum editor step at all (UEnum::Serialize asserts NumValues == 0, Enum.cpp:119, also without
// AssetDocument), so after an AI apply, successful or rolled back, no undo history may refer to
// the enum.

#include "AssetDocumentDataTableTestSupport.h"

#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/DataTable.h"
#include "Engine/UserDefinedEnum.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Misc/ScopeExit.h"
#include "Runtime/AssetDocumentUndoHistory.h"
#include "ScopedTransaction.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentUserDefinedTypesTests
{
using namespace AssetDocumentDataTableTests;

const TCHAR* StructClass = TEXT("/Script/CoreUObject.UserDefinedStruct");
const TCHAR* EnumClass = TEXT("/Script/Engine.UserDefinedEnum");
const TCHAR* DataTableClass = TEXT("/Script/Engine.DataTable");
const TCHAR* DefaultTexture = TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture");

const TCHAR* InitialMembers = TEXT(R"json("Tooltip":"One inventory item.","Members":[
  {"Name":"Label","Type":"text","DefaultValue":"Sword","Tooltip":"Shown in the UI","MultiLine":true},
  {"Name":"Damage","Type":"float","DefaultValue":12.5,"ClampMin":"0","UIMax":"100"},
  {"Name":"Count","Type":"int","DefaultValue":3,"SaveGame":true,"InstanceEditable":false},
  {"Name":"Offset","Type":"struct:/Script/CoreUObject.Vector","DefaultValue":{"X":1,"Y":2,"Z":3},"Widget3D":true},
  {"Name":"Tags","Type":"array:name","DefaultValue":["A","B"]},
  {"Name":"Lookup","Type":{"Category":"name","Container":"map","ValueType":"int"},"DefaultValue":{"K":1}},
  {"Name":"Icon","Type":"softobject:/Script/Engine.Texture2D","DefaultValue":"/Engine/EngineResources/DefaultTexture.DefaultTexture"}])json");

// Rename Count -> Amount (and retype it to float), add Weight, remove Tags, reorder.
const TCHAR* EditedMembers = TEXT(R"json("Members":[
  {"Name":"Amount","RenamedFrom":"Count","Type":"float","DefaultValue":2.5,"SaveGame":true,"InstanceEditable":false},
  {"Name":"Label","Type":"text","DefaultValue":"Sword","Tooltip":"Shown in the UI","MultiLine":true},
  {"Name":"Damage","Type":"float","DefaultValue":12.5,"ClampMin":"0","UIMax":"100"},
  {"Name":"Offset","Type":"struct:/Script/CoreUObject.Vector","DefaultValue":{"X":1,"Y":2,"Z":3},"Widget3D":true},
  {"Name":"Lookup","Type":{"Category":"name","Container":"map","ValueType":"int"},"DefaultValue":{"K":1}},
  {"Name":"Icon","Type":"softobject:/Script/Engine.Texture2D","DefaultValue":"/Engine/EngineResources/DefaultTexture.DefaultTexture"},
  {"Name":"Weight","Type":"double","DefaultValue":0.5}])json");

FString FriendlyNames(const UUserDefinedStruct* Struct)
{
	TArray<FString> Names;
	if (Struct != nullptr)
	{
		for (const FStructVariableDescription& Var : FStructureEditorUtils::GetVarDesc(Struct))
		{
			Names.Add(Var.FriendlyName);
		}
	}
	return FString::Join(Names, TEXT(","));
}

const FStructVariableDescription* FindVar(const UUserDefinedStruct* Struct, const TCHAR* Name)
{
	return Struct == nullptr ? nullptr : FStructureEditorUtils::GetVarDesc(Struct).FindByPredicate(
		[Name](const FStructVariableDescription& Var) { return Var.FriendlyName == Name; });
}

FString DisplayNames(const UEnum* Enum)
{
	TArray<FString> Names;
	for (int32 Index = 0; Enum != nullptr && Index < Enum->NumEnums() - 1; ++Index)
	{
		Names.Add(Enum->GetDisplayNameTextByIndex(Index).ToString());
	}
	return FString::Join(Names, TEXT(","));
}

bool HasDiagnostic(const FAssetDocumentResult& Result, const TCHAR* Code, const FString& MessagePart = FString())
{
	return Result.Diagnostics.ContainsByPredicate([Code, &MessagePart](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == Code && (MessagePart.IsEmpty() || Diagnostic.Message.Contains(MessagePart));
	});
}

bool IsDirty(const FTarget& Target)
{
	const UObject* Asset = Target.Get();
	return Asset != nullptr && Asset->GetOutermost()->IsDirty();
}

bool Save(const FTarget& Target)
{
	UObject* Asset = Target.Get();
	return Asset != nullptr && UEditorLoadingAndSavingUtils::SavePackages({Asset->GetOutermost()}, false);
}

// A Blueprint actor with an Item variable of the struct and a Kind variable of the enum, as the
// My Blueprint panel adds them; compiled and saved.
UBlueprint* MakeBlueprint(const FTarget& Target, UUserDefinedStruct* Struct, UUserDefinedEnum* Enum)
{
	UPackage* Package = CreatePackage(*Target.PackageName);
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package, FName(*Target.AssetName), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (Blueprint == nullptr)
	{
		return nullptr;
	}
	FAssetRegistryModule::AssetCreated(Blueprint);
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Item"),
		FEdGraphPinType(UEdGraphSchema_K2::PC_Struct, NAME_None, Struct, EPinContainerType::None, false, FEdGraphTerminalType()));
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Kind"),
		FEdGraphPinType(UEdGraphSchema_K2::PC_Byte, NAME_None, Enum, EPinContainerType::None, false, FEdGraphTerminalType()));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	UEditorLoadingAndSavingUtils::SavePackages({Package}, false);
	return Blueprint;
}

// The Values object of row Index of an extracted data table.
TSharedPtr<FJsonObject> RowValues(const TSharedPtr<FJsonObject>& Extracted, int32 Index)
{
	const TArray<TSharedPtr<FJsonValue>> Rows = BodyArray(Extracted, TEXT("Rows"));
	const TSharedPtr<FJsonObject>* Values = nullptr;
	return Rows.IsValidIndex(Index) && Rows[Index]->AsObject()->TryGetObjectField(TEXT("Values"), Values) ? *Values : nullptr;
}
} // namespace AssetDocumentUserDefinedTypesTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUserDefinedStructTest, "UEAssetDoc.UserDefinedTypes.Struct",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUserDefinedStructTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentUserDefinedTypesTests;
	const FTarget Target(TEXT("S_Item"));
	const FTarget BadCreate(TEXT("S_BadCreate"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
		BadCreate.Cleanup();
	};

	// 1. Create + populate in one apply (the Content Browser's Structure factory, then the editor's edits).
	const FString Created = Document(Target, StructClass, TEXT("Create"), InitialMembers);
	if (!TestTrue(TEXT("create applies"), Apply(*this, Target, Created, TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	UUserDefinedStruct* Struct = Target.Get<UUserDefinedStruct>();
	TestEqual(TEXT("members in order (the factory's MemberVar_0 replaced)"), FriendlyNames(Struct),
		FString(TEXT("Label,Damage,Count,Offset,Tags,Lookup,Icon")));
	TestTrue(TEXT("struct compiled"), Struct != nullptr && Struct->Status == EUserDefinedStructureStatus::UDSS_UpToDate);
	TestEqual(TEXT("struct tooltip"), FStructureEditorUtils::GetTooltip(Struct), FString(TEXT("One inventory item.")));
	if (const FStructVariableDescription* Count = FindVar(Struct, TEXT("Count")))
	{
		TestTrue(TEXT("Count: not editable on instances, SaveGame"), Count->bDontEditOnInstance && Count->bEnableSaveGame);
		const FProperty* Property = FStructureEditorUtils::GetPropertyByGuid(Struct, Count->VarGuid);
		TestTrue(TEXT("Count: default 3"), Property != nullptr && *Property->ContainerPtrToValuePtr<int32>(Struct->GetDefaultInstance()) == 3);
	}
	else
	{
		AddError(TEXT("Count member missing"));
	}
	const FStructVariableDescription* Damage = FindVar(Struct, TEXT("Damage"));
	TestTrue(TEXT("Damage: value range metadata"), Damage != nullptr && Damage->MetaData.FindRef(TEXT("ClampMin")) == TEXT("0")
		&& Damage->MetaData.FindRef(TEXT("UIMax")) == TEXT("100"));
	TestTrue(TEXT("Label: multi-line and tooltip"), FindVar(Struct, TEXT("Label")) != nullptr && FindVar(Struct, TEXT("Label"))->bEnableMultiLineText
		&& FindVar(Struct, TEXT("Label"))->ToolTip == TEXT("Shown in the UI"));
	TestTrue(TEXT("Offset: 3D widget"), FindVar(Struct, TEXT("Offset")) != nullptr && FindVar(Struct, TEXT("Offset"))->bEnable3dWidget);
	TestNoDiff(*this, Created, TEXT("after create"));

	// 2. Extract round-trip and the no-op re-apply.
	FString Baseline = TestNoOpReapply(*this, Target, TEXT("Reapply created"));
	TestFalse(TEXT("internal member names stay out of the document"), Baseline.Contains(TEXT("MemberVar")) || Baseline.Contains(TEXT("_0_")));

	// 3. Rename (keeps the GUID), retype, add, remove, reorder.
	const FGuid CountGuid = FindVar(Struct, TEXT("Count")) != nullptr ? FindVar(Struct, TEXT("Count"))->VarGuid : FGuid();
	const FString Edited = Document(Target, StructClass, TEXT("Update"), EditedMembers);
	if (TestTrue(TEXT("edit applies"), Apply(*this, Target, Edited, TEXT("Edit")).IsSuccess()))
	{
		Struct = Target.Get<UUserDefinedStruct>();
		TestEqual(TEXT("edited members"), FriendlyNames(Struct), FString(TEXT("Amount,Label,Damage,Offset,Lookup,Icon,Weight")));
		const FStructVariableDescription* Amount = FindVar(Struct, TEXT("Amount"));
		TestTrue(TEXT("rename keeps the member identity"), Amount != nullptr && Amount->VarGuid == CountGuid);
		TestTrue(TEXT("retyped to float (double)"), Amount != nullptr && Amount->ToPinType().PinCategory == UEdGraphSchema_K2::PC_Real);
		TestNoDiff(*this, Edited, TEXT("after edit"));
	}
	Baseline = TestNoOpReapply(*this, Target, TEXT("Reapply edited"));

	// 4. Invalid types, defaults and settings name the document location and change nothing.
	struct FInvalidCase
	{
		const TCHAR* Stage;
		const TCHAR* Member;
		const TCHAR* Code;
		const TCHAR* Path;
	};
	const FInvalidCase Cases[] = {
		{TEXT("MissingStructType"), TEXT(R"json({"Name":"Weight","Type":"struct:/Script/CoreUObject.NoSuchStruct"})json"),
			TEXT("UserDefinedStruct.InvalidMemberType"), TEXT("/Body/Members/6/Type")},
		{TEXT("NotAStruct"), TEXT(R"json({"Name":"Weight","Type":"struct:/Script/Engine.Actor"})json"),
			TEXT("UserDefinedStruct.InvalidMemberType"), TEXT("/Body/Members/6/Type")},
		{TEXT("InvalidDefault"), TEXT(R"json({"Name":"Weight","Type":"double","DefaultValue":"heavy"})json"),
			TEXT("UserDefinedStruct.InvalidDefault"), TEXT("/Body/Members/6/DefaultValue")},
		{TEXT("MultiLineOnNumber"), TEXT(R"json({"Name":"Weight","Type":"double","MultiLine":true})json"),
			TEXT("UserDefinedStruct.MultiLineNotAllowed"), TEXT("/Body/Members/6/MultiLine")},
		{TEXT("RangeOnText"), TEXT(R"json({"Name":"Weight","Type":"text","ClampMin":"1"})json"),
			TEXT("UserDefinedStruct.ValueRangeNotAllowed"), TEXT("/Body/Members/6/ClampMin")},
	};
	const FString Kept = TEXT(R"json({"Name":"Amount","Type":"float"},{"Name":"Label","Type":"text"},{"Name":"Damage","Type":"float"},
		{"Name":"Offset","Type":"struct:/Script/CoreUObject.Vector"},{"Name":"Lookup","Type":{"Category":"name","Container":"map","ValueType":"int"}},
		{"Name":"Icon","Type":"softobject:/Script/Engine.Texture2D"},)json");
	// The unresolved struct path is looked up (and logged) by the type reader before it fails.
	AddExpectedMessagePlain(TEXT("Failed to find object 'Object /Script/CoreUObject.NoSuchStruct'"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 0);
	for (const FInvalidCase& Case : Cases)
	{
		const FString Json = Document(Target, StructClass, TEXT("Update"), FString::Printf(TEXT("\"Members\":[%s%s]"), *Kept, Case.Member));
		TestFailsWith(*this, Apply(*this, Target, Json, Case.Stage), Case.Code, Case.Path, Case.Stage);
		TestFalse(*FString::Printf(TEXT("%s: package stays clean"), Case.Stage), IsDirty(Target));
		ExtractEquals(*this, Target, Baseline, Case.Stage);
	}
	TestFailsWith(*this, Apply(*this, Target, Document(Target, StructClass, TEXT("Update"), TEXT("\"Members\":[{\"Name\":\"A\",\"Type\":\"exec\"}]")),
		TEXT("ExecType")), TEXT("UserDefinedStruct.InvalidMemberType"), TEXT("/Body/Members/0/Type"), TEXT("exec type"));

	// 5. A failure in the middle of the member edits (a member renamed and one added, then an
	//    invalid default): the struct rolls back whole and stays saved.
	const FAssetDocumentResult MidFailed = Apply(*this, Target, Document(Target, StructClass, TEXT("Update"), TEXT(R"json("Members":[
		{"Name":"Quantity","RenamedFrom":"Amount","Type":"float"},{"Name":"Label","Type":"text"},{"Name":"Damage","Type":"float"},
		{"Name":"Offset","Type":"struct:/Script/CoreUObject.Vector"},{"Name":"Lookup","Type":{"Category":"name","Container":"map","ValueType":"int"}},
		{"Name":"Icon","Type":"softobject:/Script/Engine.Texture2D"},{"Name":"Weight","Type":"double"},
		{"Name":"Extra","Type":"int","DefaultValue":"many"}])json")), TEXT("MidApplyFailure"));
	TestFailsWith(*this, MidFailed, TEXT("UserDefinedStruct.InvalidDefault"), TEXT("/Body/Members/7/DefaultValue"), TEXT("mid-apply failure"));
	Struct = Target.Get<UUserDefinedStruct>();
	TestEqual(TEXT("rolled-back members"), FriendlyNames(Struct), FString(TEXT("Amount,Label,Damage,Offset,Lookup,Icon,Weight")));
	TestTrue(TEXT("rolled-back struct compiled"), Struct != nullptr && Struct->Status == EUserDefinedStructureStatus::UDSS_UpToDate);
	TestTrue(TEXT("rolled-back struct properties match the members"), Struct != nullptr && FindFProperty<FProperty>(Struct,
		FindVar(Struct, TEXT("Amount")) != nullptr ? FindVar(Struct, TEXT("Amount"))->VarName : NAME_None) != nullptr);
	TestFalse(TEXT("rolled-back package is clean"), IsDirty(Target));
	ExtractEquals(*this, Target, Baseline, TEXT("after mid-apply rollback"));
	TestNoOpReapply(*this, Target, TEXT("Reapply after rollback"));

	// 6. Inspect explains the type form and the per-member capabilities.
	const FString Inspected = Inspect(*this, Target, TEXT("Inspect"));
	TestTrue(TEXT("inspect lists the type form"), Inspected.Contains(TEXT("\"TypeForm\"")) && Inspected.Contains(TEXT("struct:/Script/CoreUObject.Vector")));
	TestTrue(TEXT("inspect lists member details"), Inspected.Contains(TEXT("\"MemberDetails\"")) && Inspected.Contains(TEXT("\"Allows\"")));

	// 7. The saved package reloads to the same document.
	TestReload(*this, Target, Baseline);

	// 8. A failed create leaves nothing behind.
	TestFailsWith(*this, Apply(*this, BadCreate, Document(BadCreate, StructClass, TEXT("Create"),
		TEXT("\"Members\":[{\"Name\":\"A\",\"Type\":\"int\",\"DefaultValue\":\"x\"}]")), TEXT("BadCreate")),
		TEXT("UserDefinedStruct.InvalidDefault"), TEXT("/Body/Members/0/DefaultValue"), TEXT("bad create"));
	TestNothingCreated(*this, BadCreate, TEXT("bad create"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUserDefinedEnumTest, "UEAssetDoc.UserDefinedTypes.Enum",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUserDefinedEnumTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentUserDefinedTypesTests;
	const FTarget Target(TEXT("E_Kind"));
	ON_SCOPE_EXIT { Target.Cleanup(); };

	const FString Created = Document(Target, EnumClass, TEXT("Create"), TEXT(R"json("Tooltip":"How an item is used.","Bitflags":false,
		"Enumerators":[{"Name":"Melee","Tooltip":"Close range"},{"Name":"Bow"},{"Name":"Magic"}])json"));
	if (!TestTrue(TEXT("create applies"), Apply(*this, Target, Created, TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	UUserDefinedEnum* Enum = Target.Get<UUserDefinedEnum>();
	TestEqual(TEXT("enumerators"), DisplayNames(Enum), FString(TEXT("Melee,Bow,Magic")));
	TestEqual(TEXT("enumerator description"), Enum != nullptr ? Enum->GetMetaData(TEXT("ToolTip"), 0) : FString(), FString(TEXT("Close range")));
	TestEqual(TEXT("enum description"), Enum != nullptr ? Enum->EnumDescription.ToString() : FString(), FString(TEXT("How an item is used.")));
	TestNoDiff(*this, Created, TEXT("after create"));
	FString Baseline = TestNoOpReapply(*this, Target, TEXT("Reapply created"));
	const FString BowInternal = Enum != nullptr ? Enum->GetNameStringByIndex(1) : FString();

	// Rename Bow -> Ranged (keeps its internal name), remove Magic, add Thrown, reorder, bitflags, tooltips.
	const FString Edited = Document(Target, EnumClass, TEXT("Update"), TEXT(R"json("Tooltip":"Usage.","Bitflags":true,
		"Enumerators":[{"Name":"Thrown","Tooltip":"Far"},{"Name":"Ranged","RenamedFrom":"Bow"},{"Name":"melee"}])json"));
	if (TestTrue(TEXT("edit applies"), Apply(*this, Target, Edited, TEXT("Edit")).IsSuccess()))
	{
		Enum = Target.Get<UUserDefinedEnum>();
		TestEqual(TEXT("edited enumerators"), DisplayNames(Enum), FString(TEXT("Thrown,Ranged,melee")));
		TestEqual(TEXT("rename keeps the internal name"), Enum != nullptr ? Enum->GetNameStringByIndex(1) : FString(), BowInternal);
		TestTrue(TEXT("bitflags"), FEnumEditorUtils::IsEnumeratorBitflagsType(Enum));
		TestEqual(TEXT("moved enumerator keeps its description"), Enum != nullptr ? Enum->GetMetaData(TEXT("ToolTip"), 2) : FString(), FString());
		TestNoDiff(*this, Edited, TEXT("after edit"));
	}
	Baseline = TestNoOpReapply(*this, Target, TEXT("Reapply edited"));

	// Errors name the document location and change nothing; a mid-apply failure (an enumerator
	// added, then a name the Enum editor refuses) rolls back.
	TestFailsWith(*this, Apply(*this, Target, Document(Target, EnumClass, TEXT("Update"),
		TEXT("\"Enumerators\":[{\"Name\":\"A\"},{\"Name\":\"a\"}]")), TEXT("Duplicate")),
		TEXT("UserDefinedEnum.DuplicateEnumerator"), TEXT("/Body/Enumerators/1/Name"), TEXT("duplicate"));
	const FAssetDocumentResult MidFailed = Apply(*this, Target, Document(Target, EnumClass, TEXT("Update"), TEXT(R"json("Tooltip":"Changed",
		"Enumerators":[{"Name":"Thrown"},{"Name":"Ranged"},{"Name":"melee","Tooltip":"New"},{"Name":"Extra"},{"Name":"(INVALID)"}])json")),
		TEXT("MidApplyFailure"));
	TestFailsWith(*this, MidFailed, TEXT("UserDefinedEnum.InvalidEnumeratorName"), TEXT("/Body/Enumerators/4/Name"), TEXT("mid-apply failure"));
	Enum = Target.Get<UUserDefinedEnum>();
	TestEqual(TEXT("rolled-back enumerators"), DisplayNames(Enum), FString(TEXT("Thrown,Ranged,melee")));
	TestFalse(TEXT("rolled-back package is clean"), IsDirty(Target));
	ExtractEquals(*this, Target, Baseline, TEXT("after mid-apply rollback"));

	const FString Inspected = Inspect(*this, Target, TEXT("Inspect"));
	TestTrue(TEXT("inspect lists internal names and values"), Inspected.Contains(TEXT("\"InternalName\"")) && Inspected.Contains(BowInternal));
	TestReload(*this, Target, Baseline);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUserDefinedTypesDependentsTest, "UEAssetDoc.UserDefinedTypes.Dependents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUserDefinedTypesDependentsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentUserDefinedTypesTests;
	const FTarget EnumTarget(TEXT("E_Dep"));
	const FTarget StructTarget(TEXT("S_Dep"));
	const FTarget TableTarget(TEXT("DT_Dep"));
	const FTarget BlueprintTarget(TEXT("BP_Dep"));
	ON_SCOPE_EXIT
	{
		BlueprintTarget.Cleanup();
		TableTarget.Cleanup();
		StructTarget.Cleanup();
		EnumTarget.Cleanup();
	};

	// The enum, a struct with a member of it, a table of the struct and a Blueprint with both; all saved.
	if (!TestTrue(TEXT("enum created"), Apply(*this, EnumTarget, Document(EnumTarget, EnumClass, TEXT("Create"),
			TEXT("\"Enumerators\":[{\"Name\":\"Melee\"},{\"Name\":\"Bow\"}]")), TEXT("Enum")).IsSuccess())
		|| !TestTrue(TEXT("struct created"), Apply(*this, StructTarget, Document(StructTarget, StructClass, TEXT("Create"), FString::Printf(
			TEXT("\"Members\":[{\"Name\":\"Score\",\"Type\":\"int\"},{\"Name\":\"Kind\",\"Type\":\"byte:%s\"}]"), *EnumTarget.ObjectPath)),
			TEXT("Struct")).IsSuccess())
		|| !TestTrue(TEXT("table created"), Apply(*this, TableTarget, Document(TableTarget, DataTableClass, TEXT("Create"), FString::Printf(
			TEXT("\"RowStruct\":\"%s\",\"Rows\":[{\"Name\":\"First\",\"Values\":{\"Score\":7,\"Kind\":\"Bow\"}},{\"Name\":\"Second\",\"Values\":{\"Score\":-2}}]"),
			*StructTarget.ObjectPath)), TEXT("Table")).IsSuccess()))
	{
		return false;
	}
	UBlueprint* Blueprint = MakeBlueprint(BlueprintTarget, StructTarget.Get<UUserDefinedStruct>(), EnumTarget.Get<UUserDefinedEnum>());
	if (!TestNotNull(TEXT("blueprint"), Blueprint))
	{
		return false;
	}
	ReleaseUndoHistory();
	auto AllClean = [&]()
	{
		return !IsDirty(EnumTarget) && !IsDirty(StructTarget) && !IsDirty(TableTarget) && !IsDirty(BlueprintTarget);
	};
	TestTrue(TEXT("everything saved"), AllClean());
	const FString TableBaseline = Serialize(Extract(*this, TableTarget, TEXT("table baseline")));

	// 1. Member rename + add: the editor re-reads the rows by member GUID; the apply reports the
	//    table and the Blueprint and leaves them saved.
	const FString Renamed = Document(StructTarget, StructClass, TEXT("Update"), FString::Printf(TEXT(
		"\"Members\":[{\"Name\":\"Points\",\"RenamedFrom\":\"Score\",\"Type\":\"int\"},{\"Name\":\"Kind\",\"Type\":\"byte:%s\"},{\"Name\":\"Note\",\"Type\":\"string\"}]"),
		*EnumTarget.ObjectPath));
	const FAssetDocumentResult RenameResult = Apply(*this, StructTarget, Renamed, TEXT("Rename"));
	if (TestTrue(TEXT("rename applies"), RenameResult.IsSuccess()))
	{
		TestTrue(TEXT("rename reports the table"), HasDiagnostic(RenameResult, TEXT("UserDefinedStruct.DependentsUpdated"), TableTarget.PackageName));
		TestTrue(TEXT("rename reports the blueprint"), HasDiagnostic(RenameResult, TEXT("UserDefinedStruct.DependentsUpdated"), BlueprintTarget.PackageName));
		TestTrue(TEXT("dependents stay saved"), AllClean());
		const TSharedPtr<FJsonObject> Table = Extract(*this, TableTarget, TEXT("table after rename"));
		const TSharedPtr<FJsonObject> First = RowValues(Table, 0);
		TestTrue(TEXT("rows keep the renamed member's value"), First.IsValid() && First->GetIntegerField(TEXT("Points")) == 7
			&& First->GetStringField(TEXT("Kind")) == TEXT("Bow") && !First->HasField(TEXT("Score")));
		TestTrue(TEXT("rows gain the added member"), First.IsValid() && First->HasField(TEXT("Note")));
		TestTrue(TEXT("the blueprint variable still has the struct type"), Blueprint->NewVariables.ContainsByPredicate([&](const FBPVariableDescription& Var)
		{
			return Var.VarName == TEXT("Item") && Var.VarType.PinSubCategoryObject.Get() == StructTarget.Get();
		}));
		// The table on disk loads to the same rows (it was left saved).
		TestReload(*this, TableTarget, Serialize(Table));
	}

	// 2. An apply is refused while a dependent has unsaved changes (its unsaved state could not be
	//    restored if the apply failed); nothing changes.
	const FString StructBaseline = Serialize(Extract(*this, StructTarget, TEXT("struct baseline")));
	UDataTable* LoadedTable = TableTarget.Get<UDataTable>();
	if (LoadedTable == nullptr)
	{
		LoadedTable = LoadObject<UDataTable>(nullptr, *TableTarget.ObjectPath);
	}
	if (TestNotNull(TEXT("table loaded"), LoadedTable))
	{
		LoadedTable->MarkPackageDirty();
		const FAssetDocumentResult Refused = Apply(*this, StructTarget, Document(StructTarget, StructClass, TEXT("Update"), FString::Printf(TEXT(
			"\"Members\":[{\"Name\":\"Points\",\"Type\":\"int\"},{\"Name\":\"Kind\",\"Type\":\"byte:%s\"},{\"Name\":\"Note\",\"Type\":\"string\"},{\"Name\":\"More\",\"Type\":\"bool\"}]"),
			*EnumTarget.ObjectPath)), TEXT("DirtyDependent"));
		TestFailsWith(*this, Refused, TEXT("UserDefinedStruct.DependentHasUnsavedChanges"), TEXT("/Body/Members"), TEXT("dirty dependent"));
		ExtractEquals(*this, StructTarget, StructBaseline, TEXT("after refused apply"));
		TestTrue(TEXT("the dirty table stays dirty"), IsDirty(TableTarget));
		TestTrue(TEXT("table saved again"), Save(TableTarget));
	}

	// 3. A failed apply (a member renamed, then an invalid default) leaves the dependents as they were,
	//    restored in memory: the same objects (a reload from disk would replace them under the editor
	//    systems and the request that still point to them).
	const FString TableBefore = Serialize(Extract(*this, TableTarget, TEXT("table before failure")));
	const TWeakObjectPtr<UDataTable> TableObject = TableTarget.Get<UDataTable>();
	const TWeakObjectPtr<UBlueprint> BlueprintObject = BlueprintTarget.Get<UBlueprint>();
	const FAssetDocumentResult Failed = Apply(*this, StructTarget, Document(StructTarget, StructClass, TEXT("Update"), FString::Printf(TEXT(
		"\"Members\":[{\"Name\":\"Total\",\"RenamedFrom\":\"Points\",\"Type\":\"int\"},{\"Name\":\"Kind\",\"Type\":\"byte:%s\"},{\"Name\":\"Note\",\"Type\":\"string\",\"DefaultValue\":7}]"),
		*EnumTarget.ObjectPath)), TEXT("FailedWithDependents"));
	TestFailsWith(*this, Failed, TEXT("UserDefinedStruct.InvalidDefault"), TEXT("/Body/Members/2/DefaultValue"), TEXT("failed apply"));
	ExtractEquals(*this, StructTarget, StructBaseline, TEXT("struct after failed apply"));
	ExtractEquals(*this, TableTarget, TableBefore, TEXT("table after failed apply"));
	TestTrue(TEXT("dependents stay saved after the failed apply"), AllClean());
	TestTrue(TEXT("the table is the same object after the failed apply"), TableObject.IsValid() && TableTarget.Get<UDataTable>() == TableObject.Get());
	TestTrue(TEXT("the blueprint is the same object after the failed apply"), BlueprintObject.IsValid() && BlueprintTarget.Get<UBlueprint>() == BlueprintObject.Get());
	TestFalse(TEXT("every dependent is restored"), HasDiagnostic(Failed, TEXT("UserDefinedType.DependentNotRestored"), TEXT("")));
	Blueprint = BlueprintTarget.Get<UBlueprint>();
	TestTrue(TEXT("the blueprint compiles after the failed apply"), Blueprint != nullptr && Blueprint->Status != BS_Error);

	// 4. Retype (documented): the editor converts stored values where the old value fits the new type.
	const FAssetDocumentResult Retyped = Apply(*this, StructTarget, Document(StructTarget, StructClass, TEXT("Update"), FString::Printf(TEXT(
		"\"Members\":[{\"Name\":\"Points\",\"Type\":\"float\"},{\"Name\":\"Kind\",\"Type\":\"byte:%s\"},{\"Name\":\"Note\",\"Type\":\"string\"}]"),
		*EnumTarget.ObjectPath)), TEXT("Retype"));
	if (TestTrue(TEXT("retype applies"), Retyped.IsSuccess()))
	{
		TestTrue(TEXT("retype warns about stored values"), HasDiagnostic(Retyped, TEXT("UserDefinedStruct.MemberValuesConverted"), TEXT("Points")));
		const TSharedPtr<FJsonObject> First = RowValues(Extract(*this, TableTarget, TEXT("table after retype")), 0);
		double Points = -1.0;
		TestTrue(TEXT("retyped member has a number value"), First.IsValid() && First->TryGetNumberField(TEXT("Points"), Points));
		TestEqual(TEXT("int 7 is converted to 7.0 (the editor's tagged-property conversion)"), Points, 7.0);
		TestTrue(TEXT("dependents stay saved after retype"), AllClean());
	}

	// 5. Enum: renaming an enumerator (display name) keeps the stored values, which use the
	//    unchanged internal name; the editor does not recompile users for it, so nothing is
	//    reported. A failed reshaping enum apply leaves the users as they were.
	const FAssetDocumentResult EnumRenamed = Apply(*this, EnumTarget, Document(EnumTarget, EnumClass, TEXT("Update"),
		TEXT("\"Enumerators\":[{\"Name\":\"Melee\"},{\"Name\":\"Ranged\",\"RenamedFrom\":\"Bow\"}]")), TEXT("EnumRename"));
	if (TestTrue(TEXT("enum rename applies"), EnumRenamed.IsSuccess()))
	{
		TestFalse(TEXT("enum rename does not update the struct"), HasDiagnostic(EnumRenamed, TEXT("UserDefinedEnum.DependentsUpdated"), StructTarget.PackageName));
		const TSharedPtr<FJsonObject> First = RowValues(Extract(*this, TableTarget, TEXT("table after enum rename")), 0);
		TestTrue(TEXT("stored enum value follows the rename"), First.IsValid() && First->GetStringField(TEXT("Kind")) == TEXT("Ranged"));
		TestTrue(TEXT("dependents stay saved after enum rename"), AllClean());
	}
	const FString TableAfterEnum = Serialize(Extract(*this, TableTarget, TEXT("table before enum failure")));
	const FString StructAfterEnum = Serialize(Extract(*this, StructTarget, TEXT("struct before enum failure")));
	const FAssetDocumentResult EnumFailed = Apply(*this, EnumTarget, Document(EnumTarget, EnumClass, TEXT("Update"),
		TEXT("\"Enumerators\":[{\"Name\":\"Ranged\"},{\"Name\":\"Melee\"},{\"Name\":\"Extra\"},{\"Name\":\"(INVALID)\"}]")), TEXT("EnumFailure"));
	TestFailsWith(*this, EnumFailed, TEXT("UserDefinedEnum.InvalidEnumeratorName"), TEXT("/Body/Enumerators/3/Name"), TEXT("enum failure"));
	ExtractEquals(*this, TableTarget, TableAfterEnum, TEXT("table after enum failure"));
	ExtractEquals(*this, StructTarget, StructAfterEnum, TEXT("struct after enum failure"));
	TestTrue(TEXT("dependents stay saved after the failed enum apply"), AllClean());

	// 6. Removing an enumerator that stored values use warns; the editor recompiles the struct
	//    (reported, kept saved).
	const FAssetDocumentResult EnumRemoved = Apply(*this, EnumTarget, Document(EnumTarget, EnumClass, TEXT("Update"),
		TEXT("\"Enumerators\":[{\"Name\":\"Melee\"}]")), TEXT("EnumRemove"));
	if (TestTrue(TEXT("enum remove applies"), EnumRemoved.IsSuccess()))
	{
		TestTrue(TEXT("enum remove warns"), HasDiagnostic(EnumRemoved, TEXT("UserDefinedEnum.EnumeratorValuesInvalidated"), TEXT("Ranged")));
		TestTrue(TEXT("enum remove reports the struct"), HasDiagnostic(EnumRemoved, TEXT("UserDefinedEnum.DependentsUpdated"), StructTarget.PackageName));
		TestTrue(TEXT("dependents stay saved after enum remove"), AllClean());
	}
	ReleaseUndoHistory();
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUserDefinedTypesUndoTest, "UEAssetDoc.UserDefinedTypes.Undo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUserDefinedTypesUndoTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentUserDefinedTypesTests;
	const FTarget Target(TEXT("S_Undo"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	if (GEditor == nullptr || GEditor->Trans == nullptr
		|| !TestTrue(TEXT("create applies"), Apply(*this, Target, Document(Target, StructClass, TEXT("Create"), InitialMembers), TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	ReleaseUndoHistory();
	auto Live = [&Target]() { return Target.Get<UUserDefinedStruct>(); };
	auto UndoRedoAll = [this, &Live](const TCHAR* Stage)
	{
		int32 Undone = 0;
		while (Undone < 16 && GEditor->Trans->CanUndo() && GEditor->UndoTransaction())
		{
			++Undone;
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			TestTrue(*FString::Printf(TEXT("%s: struct whole after undo %d"), Stage, Undone),
				Live() != nullptr && Live()->Status == EUserDefinedStructureStatus::UDSS_UpToDate);
		}
		int32 Redone = 0;
		while (Redone < Undone && GEditor->Trans->CanRedo() && GEditor->RedoTransaction())
		{
			++Redone;
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		TestEqual(*FString::Printf(TEXT("%s: every undone step redoes"), Stage), Redone, Undone);
		return Undone;
	};

	// 1. The human adds a member in the Struct editor and saves.
	TestTrue(TEXT("human adds a member"), FStructureEditorUtils::AddVariable(Live(), FEdGraphPinType(UEdGraphSchema_K2::PC_Int, NAME_None, nullptr,
		EPinContainerType::None, false, FEdGraphTerminalType())));
	TestTrue(TEXT("human save"), Save(Target));
	TestTrue(TEXT("the human edit is in the undo history"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));

	// 2. The AI rewrites the members (the human's member is not in the document).
	if (!TestTrue(TEXT("AI edit applies"), Apply(*this, Target, Document(Target, StructClass, TEXT("Update"), EditedMembers), TEXT("Edit")).IsSuccess()))
	{
		return false;
	}
	TestEqual(TEXT("AI members"), FriendlyNames(Live()), FString(TEXT("Amount,Label,Damage,Offset,Lookup,Icon,Weight")));
	TestFalse(TEXT("after AI edit: no undo history references the struct"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	const FString AfterEdit = Serialize(Extract(*this, Target, TEXT("after AI edit")));
	UndoRedoAll(TEXT("after AI edit"));
	ExtractEquals(*this, Target, AfterEdit, TEXT("after AI edit + undo/redo"));

	// 3. The human changes a member tooltip (Struct editor) and saves. A rejected AI apply (invalid
	//    document, nothing changed) keeps the human's step undoable.
	ReleaseUndoHistory();
	const FStructVariableDescription* Label = FindVar(Live(), TEXT("Label"));
	if (!TestNotNull(TEXT("Label"), Label))
	{
		return false;
	}
	TestTrue(TEXT("human edits a tooltip"), FStructureEditorUtils::ChangeVariableTooltip(Live(), Label->VarGuid, TEXT("Human tooltip")));
	TestTrue(TEXT("human save 2"), Save(Target));
	const int32 HumanSteps = GEditor->Trans->GetQueueLength();
	const FString AfterHumanEdit = Serialize(Extract(*this, Target, TEXT("after human edit")));
	const FAssetDocumentResult Rejected = Apply(*this, Target, Document(Target, StructClass, TEXT("Update"), TEXT(R"json("Members":[
		{"Name":"Amount","Type":"float"},{"Name":"amount","Type":"int"}])json")), TEXT("Rejected"));
	TestFalse(TEXT("invalid document is rejected"), Rejected.IsSuccess());
	TestEqual(TEXT("after rejection: the human history is kept"), GEditor->Trans->GetQueueLength(), HumanSteps);
	TestTrue(TEXT("Ctrl+Z undoes the human edit"), GEditor->UndoTransaction());
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestTrue(TEXT("after undo: struct whole"), Live() != nullptr && Live()->Status == EUserDefinedStructureStatus::UDSS_UpToDate);
	ExtractEquals(*this, Target, AfterEdit, TEXT("after rejection + undo"));
	TestTrue(TEXT("Ctrl+Y redoes the human edit"), GEditor->RedoTransaction());
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	ExtractEquals(*this, Target, AfterHumanEdit, TEXT("after rejection + redo"));

	// 4. An AI apply fails mid-way and rolls back. The Struct editor operations it ran recorded their
	//    own transactions (stale after the rollback), so the undo history is cleared (direction doc
	//    §3.1); the struct is back at the human's saved state and nothing is left to undo into a mix.
	TestTrue(TEXT("human save 3"), Save(Target));
	const FAssetDocumentResult MidFailed = Apply(*this, Target, Document(Target, StructClass, TEXT("Update"), TEXT(R"json("Members":[
		{"Name":"Quantity","RenamedFrom":"Amount","Type":"float"},{"Name":"Extra","Type":"int","DefaultValue":"many"}])json")), TEXT("MidApplyFailure"));
	TestFalse(TEXT("mid-apply failure fails"), MidFailed.IsSuccess());
	TestFalse(TEXT("after rollback: no undo history references the struct"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	TestTrue(TEXT("after rollback: struct whole"), Live() != nullptr && Live()->Status == EUserDefinedStructureStatus::UDSS_UpToDate);
	ExtractEquals(*this, Target, AfterHumanEdit, TEXT("after rollback"));
	TestFalse(TEXT("after rollback: package clean"), IsDirty(Target));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUserDefinedTypesEnumUndoTest, "UEAssetDoc.UserDefinedTypes.EnumUndo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUserDefinedTypesEnumUndoTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentUserDefinedTypesTests;
	const FTarget Target(TEXT("E_Undo"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	if (GEditor == nullptr || GEditor->Trans == nullptr
		|| !TestTrue(TEXT("create applies"), Apply(*this, Target, Document(Target, EnumClass, TEXT("Create"),
			TEXT("\"Enumerators\":[{\"Name\":\"Melee\"},{\"Name\":\"Bow\"}]")), TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	ReleaseUndoHistory();
	auto Live = [&Target]() { return Target.Get<UUserDefinedEnum>(); };
	auto UndoRedoAll = [this](const TCHAR* Stage)
	{
		int32 Undone = 0;
		while (Undone < 16 && GEditor->Trans->CanUndo() && GEditor->UndoTransaction())
		{
			++Undone;
		}
		int32 Redone = 0;
		while (Redone < Undone && GEditor->Trans->CanRedo() && GEditor->RedoTransaction())
		{
			++Redone;
		}
		TestEqual(*FString::Printf(TEXT("%s: every undone step redoes"), Stage), Redone, Undone);
	};

	// 1. The human adds an enumerator in the Enum editor and saves.
	FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Live());
	TestTrue(TEXT("human save"), Save(Target));
	TestTrue(TEXT("the human edit is in the undo history"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));

	// 2. The AI rewrites the enumerators. Nothing left in the undo history refers to the enum, so
	//    Ctrl+Z / Ctrl+Y cannot reach an enum record (restoring one asserts in UEnum::Serialize,
	//    which needs an empty name table; the stock editor's undo does not provide one).
	const FAssetDocumentResult Edited = Apply(*this, Target, Document(Target, EnumClass, TEXT("Update"),
		TEXT("\"Enumerators\":[{\"Name\":\"Thrown\"},{\"Name\":\"Ranged\",\"RenamedFrom\":\"Bow\"},{\"Name\":\"Melee\"}]")), TEXT("Edit"));
	if (!TestTrue(TEXT("AI edit applies"), Edited.IsSuccess()))
	{
		return false;
	}
	TestFalse(TEXT("after AI edit: no undo history references the enum"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	const FString AfterEdit = Serialize(Extract(*this, Target, TEXT("after AI edit")));
	UndoRedoAll(TEXT("after AI edit"));
	TestEqual(TEXT("after AI edit + undo/redo"), DisplayNames(Live()), FString(TEXT("Thrown,Ranged,Melee")));
	ExtractEquals(*this, Target, AfterEdit, TEXT("after AI edit + undo/redo"));

	// 3. The human edits again and saves; an AI apply fails mid-way (an enumerator added, then a
	//    name the Enum editor refuses) and rolls back. The Enum editor recorded its own steps, so
	//    the history is cleared and nothing can undo into the restored enum.
	FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Live());
	TestTrue(TEXT("human save 2"), Save(Target));
	const FString AfterHumanEdit = Serialize(Extract(*this, Target, TEXT("after human edit")));
	const FAssetDocumentResult MidFailed = Apply(*this, Target, Document(Target, EnumClass, TEXT("Update"),
		TEXT("\"Enumerators\":[{\"Name\":\"Thrown\"},{\"Name\":\"Extra\"},{\"Name\":\"(INVALID)\"}]")), TEXT("MidApplyFailure"));
	TestFailsWith(*this, MidFailed, TEXT("UserDefinedEnum.InvalidEnumeratorName"), TEXT(""), TEXT("mid-apply failure"));
	TestFalse(TEXT("after rollback: no undo history references the enum"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	UndoRedoAll(TEXT("after rollback"));
	ExtractEquals(*this, Target, AfterHumanEdit, TEXT("after rollback + undo/redo"));
	TestFalse(TEXT("after rollback: package clean"), IsDirty(Target));
	return !HasAnyErrors();
}

#endif
