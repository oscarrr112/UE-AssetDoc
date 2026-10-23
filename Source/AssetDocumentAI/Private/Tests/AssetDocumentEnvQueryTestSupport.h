// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Shared fixtures of the EnvQuery automation tests (AssetDocumentEnvQueryTests.cpp): /Game targets, the editor-side
// operations (what the Environment Query editor does on palette drops, Add Test, Details edits, toggles, wires,
// comments), the editor-built fixtures, the comparison form of a query (graph, runtime options, params) and a test
// world that runs a query (UEnvQueryManager::RunInstantQuery) for item / score comparisons.

#pragma once

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "../../../AssetDocument/Private/Tests/AssetDocumentDiffCountTestSupport.h"

#include "AIGraph.h"
#include "AIGraphNode.h"
#include "AIGraphSchema.h"
#include "AISystem.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphNode_Comment.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Engine.h"
#include "Engine/TargetPoint.h"
#include "Engine/World.h"
#include "EnvQuery/AssetDocumentEnvQueryPrimitives.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryManager.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "EnvironmentQuery/EnvQueryTypes.h"
#include "Factories/Factory.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEnvQueryTests
{
namespace EQ = UE::AssetDocument::Graph::EnvQuery;

const TCHAR* const FolderName = TEXT("AssetDocumentEnvQuery");
const TCHAR* const QueryClass = TEXT("/Script/AIModule.EnvQuery");

inline void ReleaseUndoHistory()
{
	if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting && !GEditor->IsTransactionActive()
		&& GEditor->Trans->GetQueueLength() > 0)
	{
		GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentEnvQueryTests", "Cleanup", "AssetDocument EnvQuery test cleanup"));
	}
}

inline double Milliseconds(double Start)
{
	return (FPlatformTime::Seconds() - Start) * 1000.0;
}

struct FTarget
{
	FString AssetName;
	FString PackageName;
	FString ObjectPath;
	FString PackageFile;
	FString SidecarPath;

	explicit FTarget(const TCHAR* Prefix)
		: AssetName(FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits)))
	{
		PackageName = FString(TEXT("/Game/")) + FolderName + TEXT("/") + AssetName;
		ObjectPath = PackageName + TEXT(".") + AssetName;
		PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		SidecarPath = FPaths::Combine(FPaths::ProjectContentDir(), FolderName, AssetName + TEXT(".assetdoc.json"));
	}

	UObject* Get() const { return FSoftObjectPath(ObjectPath).ResolveObject(); }
	UEnvQuery* Query() const { return Cast<UEnvQuery>(Get()); }

	// Saved assets are unloaded and deleted from disk as the Content Browser does.
	void Cleanup() const
	{
		ReleaseUndoHistory();
		if (UObject* Asset = Get())
		{
			if (GEditor != nullptr)
			{
				if (UAssetEditorSubsystem* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
				{
					Editors->CloseAllEditorsForAsset(Asset);
				}
			}
			Asset->GetOutermost()->ClearDirtyFlag();
			if (IFileManager::Get().FileExists(*PackageFile))
			{
				UPackageTools::UnloadPackages({Asset->GetOutermost()});
			}
			else
			{
				ObjectTools::DeleteObjects({Asset}, false, ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
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

// Test assets, removed in reverse order.
struct FFixtures
{
	TArray<TUniquePtr<FTarget>> Targets;
	const FTarget& Add(const TCHAR* Prefix) { return *Targets.Add_GetRef(MakeUnique<FTarget>(Prefix)); }
	~FFixtures()
	{
		for (int32 Index = Targets.Num() - 1; Index >= 0; --Index)
		{
			Targets[Index]->Cleanup();
		}
	}
};

// ---- JSON / service ------------------------------------------------------------------------------------------------

inline FString Serialize(const TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	if (Object.IsValid())
	{
		const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}
	return Text;
}

inline TSharedPtr<FJsonObject> Parse(const FString& Json)
{
	TSharedPtr<FJsonObject> Object;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object);
	return Object;
}

inline TSharedPtr<FJsonObject> Child(const TSharedPtr<FJsonObject>& Parent, const FString& Name)
{
	const TSharedPtr<FJsonObject>* Found = nullptr;
	if (Parent.IsValid() && Parent->TryGetObjectField(Name, Found))
	{
		return *Found;
	}
	return MakeShared<FJsonObject>();
}

inline TArray<TSharedPtr<FJsonValue>> ArrayOf(const TSharedPtr<FJsonObject>& Parent, const FString& Name)
{
	const TArray<TSharedPtr<FJsonValue>>* Found = nullptr;
	if (Parent.IsValid() && Parent->TryGetArrayField(Name, Found))
	{
		return *Found;
	}
	return {};
}

inline TSharedPtr<FJsonObject> BodyOf(const TSharedPtr<FJsonObject>& Document) { return Child(Document, TEXT("Body")); }
inline TArray<TSharedPtr<FJsonValue>> OptionsOf(const TSharedPtr<FJsonObject>& Document) { return ArrayOf(BodyOf(Document), TEXT("Options")); }

// The document with Target and Action replaced (a deep copy).
inline TSharedPtr<FJsonObject> ForTarget(const TSharedPtr<FJsonObject>& Document, const FTarget& Target, const TCHAR* Action)
{
	const TSharedPtr<FJsonObject> Copy = Parse(Serialize(Document));
	if (Copy.IsValid())
	{
		Copy->SetStringField(TEXT("Target"), Target.PackageName);
		Copy->SetStringField(TEXT("Action"), Action);
	}
	return Copy;
}

inline TSharedPtr<FJsonObject> Document(const FTarget& Target, const TCHAR* BodyJson, const TCHAR* Action = TEXT("CreateOrUpdate"))
{
	const TSharedPtr<FJsonObject> Doc = MakeShared<FJsonObject>();
	Doc->SetNumberField(TEXT("SchemaVersion"), 1);
	Doc->SetStringField(TEXT("Target"), Target.PackageName);
	Doc->SetStringField(TEXT("Class"), QueryClass);
	Doc->SetStringField(TEXT("Action"), Action);
	Doc->SetObjectField(TEXT("Definitions"), MakeShared<FJsonObject>());
	Doc->SetObjectField(TEXT("Properties"), MakeShared<FJsonObject>());
	Doc->SetObjectField(TEXT("Body"), Parse(BodyJson));
	return Doc;
}

inline FString WithoutTarget(const TSharedPtr<FJsonObject>& Document)
{
	if (!Document.IsValid())
	{
		return FString();
	}
	const TSharedPtr<FJsonObject> Copy = Parse(Serialize(Document));
	Copy->SetStringField(TEXT("Target"), FString());
	return Serialize(Copy);
}

inline FString FirstDifference(const FString& A, const FString& B)
{
	int32 Index = 0;
	while (Index < A.Len() && Index < B.Len() && A[Index] == B[Index])
	{
		++Index;
	}
	const int32 Start = FMath::Max(0, Index - 100);
	return FString::Printf(TEXT("at %d: '%s' vs '%s'"), Index, *A.Mid(Start, 260), *B.Mid(Start, 260));
}

inline FString DiagnosticsText(const FAssetDocumentResult& Result)
{
	TArray<FString> Lines;
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Lines.Add(FString::Printf(TEXT("[%s] %s %s%s"), *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message,
			Diagnostic.Details.IsValid() ? *(TEXT(" ") + Serialize(Diagnostic.Details)) : TEXT("")));
	}
	return Lines.IsEmpty() ? Result.Message : FString::Join(Lines, TEXT(" / "));
}

inline FAssetDocumentResult ExtractResult(const FString& ObjectPath)
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = ObjectPath;
	return FAssetDocumentModule::Get().GetService().Extract(Request);
}

inline TSharedPtr<FJsonObject> Extract(FAutomationTestBase& Test, const FString& ObjectPath, const FString& Stage)
{
	const FAssetDocumentResult Result = ExtractResult(ObjectPath);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: extract succeeds"), *Stage), Result.IsSuccess() && Result.Payload.IsValid()))
	{
		Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *DiagnosticsText(Result)));
		return nullptr;
	}
	return Result.Payload;
}

inline FAssetDocumentResult Inspect(const FString& ObjectPath)
{
	FAssetDocumentInspectRequest Request;
	Request.ClassOrAsset = ObjectPath;
	return FAssetDocumentModule::Get().GetService().Inspect(Request);
}

// Number of changes Diff reports between the document and the live asset (changed, added and removed entries; -1 when Diff fails).
inline int32 DiffCount(const TSharedPtr<FJsonObject>& Document, FString& OutText)
{
	FAssetDocumentDiffRequest Request;
	Request.Document = Document;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Diff(Request);
	const int32 Count = AssetDocumentDiffCountTestSupport::CountDiffEntries(Result);
	if (Count < 0)
	{
		OutText = DiagnosticsText(Result);
		return -1;
	}
	OutText = Serialize(Result.Payload);
	return Count;
}

inline FAssetDocumentResult Apply(const FTarget& Target, const TSharedPtr<FJsonObject>& Document)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target.SidecarPath), true);
	FFileHelper::SaveStringToFile(Serialize(Document), *Target.SidecarPath);
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Target.SidecarPath;
	Request.bSaveAsset = true;
	return FAssetDocumentModule::Get().GetService().ApplyFile(Request);
}

inline bool HasCode(const FAssetDocumentResult& Result, const TCHAR* Code)
{
	return Result.Diagnostics.ContainsByPredicate([Code](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == Code; });
}

inline bool ApplyOk(FAutomationTestBase& Test, const FTarget& Target, const TSharedPtr<FJsonObject>& Document, const FString& Stage)
{
	const FAssetDocumentResult Result = Apply(Target, Document);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s applies"), *Stage), Result.IsSuccess()))
	{
		Test.AddError(FString::Printf(TEXT("%s: %s"), *Stage, *DiagnosticsText(Result)));
		return false;
	}
	return true;
}

// A refused apply: its code and path, and an unchanged asset (package clean, file untouched, same extraction).
inline void ExpectRefused(FAutomationTestBase& Test, const FTarget& Target, const TSharedPtr<FJsonObject>& Document, const TCHAR* Code,
	const FString& Path, const FString& Stage)
{
	const FString Before = Serialize(Extract(Test, Target.ObjectPath, Stage + TEXT(" before")));
	const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Target.PackageFile);
	const FAssetDocumentResult Result = Apply(Target, Document);
	Test.TestFalse(*FString::Printf(TEXT("%s: refused"), *Stage), Result.IsSuccess());
	// Code null: any code (reported), at Path.
	const FAssetDocumentDiagnostic* Found = Code == nullptr ? (Result.Diagnostics.IsEmpty() ? nullptr : &Result.Diagnostics[0])
		: Result.Diagnostics.FindByPredicate([Code](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == Code; });
	if (Code == nullptr && Found != nullptr)
	{
		Test.AddInfo(FString::Printf(TEXT("%s: refused with %s"), *Stage, *DiagnosticsText(Result)));
	}
	if (!Test.TestNotNull(*FString::Printf(TEXT("%s: %s"), *Stage, Code != nullptr ? Code : TEXT("a diagnostic")), Found))
	{
		Test.AddError(FString::Printf(TEXT("%s: %s"), *Stage, *DiagnosticsText(Result)));
	}
	else
	{
		Test.TestEqual(*FString::Printf(TEXT("%s: path"), *Stage), Found->Path, Path);
	}
	const UObject* Asset = Target.Get();
	Test.TestFalse(*FString::Printf(TEXT("%s: package clean"), *Stage), Asset != nullptr && Asset->GetOutermost()->IsDirty());
	Test.TestEqual(*FString::Printf(TEXT("%s: package file untouched"), *Stage), IFileManager::Get().GetTimeStamp(*Target.PackageFile), Stamp);
	Test.TestEqual(*FString::Printf(TEXT("%s: extraction unchanged"), *Stage), Serialize(Extract(Test, Target.ObjectPath, Stage + TEXT(" after"))), Before);
}

inline bool Save(FAutomationTestBase& Test, UObject* Asset, const FString& Stage)
{
	return Test.TestTrue(*FString::Printf(TEXT("%s saved"), *Stage), Asset != nullptr && UEditorLoadingAndSavingUtils::SavePackages({Asset->GetOutermost()}, false));
}

// ---- the editor side: the Environment Query editor's own calls ------------------------------------------------------

// The Content Browser's Add > Artificial Intelligence > Environment Query (UEnvironmentQueryFactory), saved.
inline UEnvQuery* CreateQuery(FAutomationTestBase& Test, const FTarget& Target)
{
	EQ::EnsureEditorModule();
	UClass* FactoryClass = FindObject<UClass>(nullptr, TEXT("/Script/EnvironmentQueryEditor.EnvironmentQueryFactory"));
	UFactory* Factory = FactoryClass != nullptr ? NewObject<UFactory>(GetTransientPackage(), FactoryClass) : nullptr;
	if (!Test.TestNotNull(TEXT("Environment Query factory"), Factory))
	{
		return nullptr;
	}
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UEnvQuery* Query = Cast<UEnvQuery>(AssetTools.CreateAsset(Target.AssetName, FPackageName::GetLongPackagePath(Target.PackageName), UEnvQuery::StaticClass(), Factory));
	if (Test.TestNotNull(*FString::Printf(TEXT("%s created by the factory"), *Target.AssetName), Query))
	{
		Save(Test, Query, Target.AssetName);
	}
	ReleaseUndoHistory();
	return Query;
}

struct FEditorQuery
{
	UEnvQuery& Query;

	explicit FEditorQuery(UEnvQuery& InQuery) : Query(InQuery) {}

	static bool Expect(const TCHAR* What, bool bValue)
	{
		FAutomationTestBase* Current = FAutomationTestFramework::Get().GetCurrentTest();
		return Current != nullptr ? Current->TestTrue(What, bValue) : bValue;
	}

	// Opening the editor's graph tab (SpawnTab_UpdateGraph: a new graph when the query has none, then Initialize).
	UAIGraph& Graph() const { return *EQ::EnsureGraph(Query); }
	UAIGraphSchema& Schema() const { return *const_cast<UAIGraphSchema*>(CastChecked<UAIGraphSchema>(Graph().GetSchema())); }

	UAIGraphNode* Option(const FString& Name) const { return EQ::FindOptionNode(Graph(), Name); }
	UAIGraphNode* Test(const FString& Name) const { return EQ::FindTestNode(Graph(), Name); }

	// A palette drop of a generator (dragged from Root's output when bWired). Returns the option's name.
	FString AddOption(const TCHAR* Label, int32 X, int32 Y, bool bWired = true) const
	{
		TArray<FString> Candidates;
		UClass* Class = EQ::ResolveClass(Label, EQ::ENodeKind::Generator, Candidates);
		UEdGraphPin* From = bWired ? EQ::RootOutput(Graph()) : nullptr;
		// The palette's action for the class (UEdGraphSchema_EnvironmentQuery::GetGraphContextActions builds it from
		// the editor module's class cache, which exists only once an editor opened; EQ keeps one built the same way).
		FGraphNodeClassData Data;
		if (Class != nullptr && EQ::FindClassData(*Class, EQ::ENodeKind::Generator, Data))
		{
			UAIGraphNode* Template = NewObject<UAIGraphNode>(&Graph(), EQ::OptionNodeClass());
			Template->ClassData = Data;
			FAISchemaAction_NewNode NewNode;
			NewNode.NodeTemplate = Template;
			UAIGraphNode* Node = Cast<UAIGraphNode>(NewNode.PerformAction(&Graph(), From, FVector2f(static_cast<float>(X), static_cast<float>(Y)), true));
			UEnvQueryOption* Made = Node != nullptr ? EQ::GetOption(*Node) : nullptr;
			return Made != nullptr ? Made->GetName() : FString();
		}
		Expect(*FString::Printf(TEXT("palette offers %s"), Label), false);
		return FString();
	}

	// The option node's Add Test menu. Returns the test's name.
	FString AddTest(const FString& OptionName, const TCHAR* Label) const
	{
		UAIGraphNode* Parent = Option(OptionName);
		TArray<FString> Candidates;
		UClass* Class = EQ::ResolveClass(Label, EQ::ENodeKind::Test, Candidates);
		// UEdGraphSchema_EnvironmentQuery::GetSubNodeClasses (see AddOption).
		FGraphNodeClassData ClassData;
		UClass* GraphNodeClass = EQ::TestNodeClass();
		const FGraphNodeClassData* Data = Class != nullptr && EQ::FindClassData(*Class, EQ::ENodeKind::Test, ClassData) ? &ClassData : nullptr;
		if (!Expect(*FString::Printf(TEXT("Add Test offers %s"), Label), Parent != nullptr && Data != nullptr && GraphNodeClass != nullptr))
		{
			return FString();
		}
		UAIGraphNode* Template = NewObject<UAIGraphNode>(&Graph(), GraphNodeClass);
		Template->ClassData = *Data;
		FAISchemaAction_NewSubNode Action;
		Action.ParentNode = Parent;
		Action.NodeTemplate = Template;
		Action.PerformAction(&Graph(), nullptr, FVector2f::ZeroVector, true);
		return Template->NodeInstance != nullptr ? Template->NodeInstance->GetName() : FString();
	}

	// The Details panel on the selected generator / test: one property from its text form, then the editor's
	// OnFinishedChangingProperties (the option's weights) and the graph update.
	bool SetProperty(const FString& Name, const TCHAR* Property, const TCHAR* Text) const
	{
		UAIGraphNode* Node = Option(Name);
		UObject* Instance = nullptr;
		if (Node != nullptr)
		{
			Instance = EQ::GetOption(*Node) != nullptr ? EQ::GetOption(*Node)->Generator.Get() : nullptr;
		}
		else if ((Node = Test(Name)) != nullptr)
		{
			Instance = Node->NodeInstance;
		}
		FProperty* Field = Instance != nullptr ? Instance->GetClass()->FindPropertyByName(Property) : nullptr;
		if (!Expect(*FString::Printf(TEXT("%s.%s exists"), *Name, Property), Field != nullptr))
		{
			return false;
		}
		const FScopedTransaction Transaction(NSLOCTEXT("PropertyEditor", "EditPropertyTransaction", "Edit Property"));
		Instance->Modify();
		Instance->PreEditChange(Field);
		const TCHAR* Result = Field->ImportText_InContainer(Text, Instance, Instance, PPF_None);
		FPropertyChangedEvent Event(Field, EPropertyChangeType::ValueSet);
		Instance->PostEditChangeProperty(Event);
		Graph().UpdateAsset();
		return Expect(*FString::Printf(TEXT("%s.%s = %s"), *Name, Property, Text), Result != nullptr);
	}

	// The test node's toggle (SGraphNode_EnvironmentQuery::OnTestToggleChanged).
	void Toggle(const FString& Name, bool bEnabled) const
	{
		UAIGraphNode* Node = Test(Name);
		FBoolProperty* Flag = Node != nullptr ? CastField<FBoolProperty>(Node->GetClass()->FindPropertyByName(TEXT("bTestEnabled"))) : nullptr;
		if (Expect(TEXT("test toggle"), Flag != nullptr))
		{
			Flag->SetPropertyValue_InContainer(Node, bEnabled);
			Graph().UpdateAsset();
		}
	}

	// Alt-click on Root's wire to the option.
	void Unwire(const FString& Name) const
	{
		UAIGraphNode* Node = Option(Name);
		UEdGraphPin* Input = Node != nullptr ? Node->GetInputPin() : nullptr;
		if (Expect(TEXT("option input"), Input != nullptr))
		{
			const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentEnvQueryTests", "UnlinkPins", "Unlink Pins"));
			Schema().BreakPinLinks(*Input, true);
		}
	}

	void Move(UEdGraphNode& Node, int32 X, int32 Y) const
	{
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentEnvQueryTests", "MoveNode", "Move Node"));
		Node.Modify();
		Node.NodePosX = X;
		Node.NodePosY = Y;
		Graph().UpdateAsset();
	}

	void NodeComment(UEdGraphNode& Node, const TCHAR* Text, bool bVisible) const
	{
		const FScopedTransaction Transaction(NSLOCTEXT("K2Node", "CommentCommitted", "Comment Changed"));
		Node.Modify();
		Node.NodeComment = Text;
		Node.bCommentBubbleVisible = bVisible;
		Node.bCommentBubblePinned = bVisible;
	}

	FString AddComment(const TCHAR* Text, int32 X, int32 Y, int32 Width, int32 Height) const
	{
		FAISchemaAction_AddComment Action;
		UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Action.PerformAction(&Graph(), nullptr, FVector2f(static_cast<float>(X), static_cast<float>(Y)), true));
		if (Comment == nullptr)
		{
			return FString();
		}
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentEnvQueryTests", "EditComment", "Edit Comment"));
		Comment->Modify();
		Comment->NodeComment = Text;
		Comment->NodePosX = X;
		Comment->NodePosY = Y;
		Comment->NodeWidth = Width;
		Comment->NodeHeight = Height;
		return Comment->GetName();
	}

	// The editor's Delete on a node (FAIGraphEditor::DeleteSelectedNodes).
	void Delete(UEdGraphNode& Node) const
	{
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentEnvQueryTests", "DeleteNode", "Delete Node"));
		Graph().Modify();
		Node.Modify();
		Node.DestroyNode();
		Graph().UpdateAsset();
	}

	// Save (the editor's Save runs UpdateAsset first).
	bool SaveQuery(FAutomationTestBase& TestBase, const FString& Stage) const
	{
		Graph().UpdateAsset();
		const bool bSaved = Save(TestBase, &Query, Stage);
		ReleaseUndoHistory();
		return bSaved;
	}
};

// ---- the comparison form ----------------------------------------------------------------------------------------

inline FString DescribeObject(const UObject* Object, int32 Depth);

inline FString DescribeValue(const FProperty& Property, const void* Value, int32 Depth)
{
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		TArray<FString> Parts;
		// FEnvTraceData::OnPostLoad rewrites VersionNum and SerializedChannel on every load (TraceChannel stays).
		const bool bTraceData = Struct->Struct == FEnvTraceData::StaticStruct();
		for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
		{
			if (!It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)
				&& !(bTraceData && (It->GetFName() == TEXT("VersionNum") || It->GetFName() == TEXT("SerializedChannel"))))
			{
				Parts.Add(It->GetName() + TEXT("=") + DescribeValue(**It, It->ContainerPtrToValuePtr<void>(Value), Depth + 1));
			}
		}
		return TEXT("(") + FString::Join(Parts, TEXT(",")) + TEXT(")");
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		FScriptArrayHelper Helper(Array, Value);
		TArray<FString> Parts;
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			Parts.Add(DescribeValue(*Array->Inner, Helper.GetRawPtr(Index), Depth + 1));
		}
		return TEXT("[") + FString::Join(Parts, TEXT(",")) + TEXT("]");
	}
	if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(&Property))
	{
		const UObject* Object = ObjectProperty->GetObjectPropertyValue(Value);
		if (Object != nullptr && Property.HasAnyPropertyFlags(CPF_InstancedReference | CPF_PersistentInstance) && Depth < 8)
		{
			return DescribeObject(Object, Depth + 1);
		}
		return Object != nullptr ? Object->GetPathName() : TEXT("None");
	}
	FString Text;
	Property.ExportTextItem_Direct(Text, Value, nullptr, nullptr, PPF_None);
	return Text;
}

// The class and Details values of an instanced object (generator, test, data binding), recursively.
inline FString DescribeObject(const UObject* Object, int32 Depth)
{
	if (Object == nullptr)
	{
		return TEXT("None");
	}
	TArray<FString> Parts;
	for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_Edit) && !It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
		{
			Parts.Add(It->GetName() + TEXT("=") + DescribeValue(**It, It->ContainerPtrToValuePtr<void>(Object), Depth));
		}
	}
	return Object->GetClass()->GetPathName() + TEXT("{") + FString::Join(Parts, TEXT(";")) + TEXT("}");
}

inline bool TestEnabled(const UAIGraphNode& Node)
{
	return EQ::IsTestEnabled(Node);
}

struct FFormOptions
{
	bool bNames = true;     // object names of options and tests
	bool bPositions = true; // node positions
	bool bComments = true;  // comment boxes and node comments
};

// The query as the editor and the runtime see it, one fact per line: the graph (Root, options left to right with
// their wire, generator, tests, toggles, comments; comment boxes), the runtime Options the editor built from it and
// the named params.
inline FString QueryForm(const UEnvQuery& Query, const FFormOptions& Options = FFormOptions())
{
	TArray<FString> Lines;
	const UAIGraph* Graph = EQ::GetGraph(Query);
	auto Name = [&Options](const UObject* Object) { return Options.bNames && Object != nullptr ? Object->GetName() : FString(TEXT("-")); };
	if (Graph != nullptr)
	{
		const UEdGraphNode* Root = EQ::FindRoot(*Graph);
		if (Root != nullptr && Options.bPositions)
		{
			Lines.Add(FString::Printf(TEXT("root (%d,%d)"), Root->NodePosX, Root->NodePosY));
		}
		TArray<UAIGraphNode*> Nodes = EQ::GetOptionNodes(*Graph);
		Nodes.Sort([](const UAIGraphNode& A, const UAIGraphNode& B) { return A.NodePosX != B.NodePosX ? A.NodePosX < B.NodePosX : A.NodePosY < B.NodePosY; });
		for (const UAIGraphNode* Node : Nodes)
		{
			const UEnvQueryOption* Option = EQ::GetOption(*Node);
			Lines.Add(FString::Printf(TEXT("option %s wired=%d %s %s"), *Name(Option), EQ::IsConnected(*Node) ? 1 : 0,
				Options.bPositions ? *FString::Printf(TEXT("(%d,%d)"), Node->NodePosX, Node->NodePosY) : TEXT(""),
				Options.bComments ? *FString::Printf(TEXT("comment='%s' bubble=%d"), *Node->NodeComment, Node->bCommentBubbleVisible ? 1 : 0) : TEXT("")));
			Lines.Add(TEXT("  generator ") + DescribeObject(Option != nullptr ? Option->Generator.Get() : nullptr, 0));
			for (const UAIGraphNode* Sub : Node->SubNodes)
			{
				Lines.Add(FString::Printf(TEXT("  test %s enabled=%d %s %s"), *Name(Sub->NodeInstance), TestEnabled(*Sub) ? 1 : 0,
					Options.bComments ? *FString::Printf(TEXT("comment='%s' bubble=%d"), *Sub->NodeComment, Sub->bCommentBubbleVisible ? 1 : 0) : TEXT(""),
					*DescribeObject(Sub->NodeInstance, 0)));
			}
		}
		if (Options.bComments)
		{
			TArray<FString> Comments;
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (const UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node))
				{
					Comments.Add(FString::Printf(TEXT("comment %s '%s' (%d,%d) %dx%d color=%s font=%d bubble=%d colorbubble=%d mode=%d"), *Name(Comment), *Comment->NodeComment,
						Comment->NodePosX, Comment->NodePosY, Comment->NodeWidth, Comment->NodeHeight, *Comment->CommentColor.ToString(), Comment->FontSize,
						Comment->bCommentBubbleVisible_InDetailsPanel ? 1 : 0, Comment->bColorCommentBubble ? 1 : 0, static_cast<int32>(Comment->MoveMode)));
				}
			}
			Comments.Sort();
			Lines.Append(Comments);
		}
	}
	for (const UEnvQueryOption* Option : Query.GetOptions())
	{
		Lines.Add(FString::Printf(TEXT("runtime option %s"), *Name(Option)));
		Lines.Add(TEXT("  generator ") + DescribeObject(Option != nullptr ? Option->Generator.Get() : nullptr, 0));
		if (Option != nullptr)
		{
			for (const UEnvQueryTest* Test : Option->Tests)
			{
				Lines.Add(FString::Printf(TEXT("  test %s order=%d %s"), *Name(Test), Test != nullptr ? Test->TestOrder : -1, *DescribeObject(Test, 0)));
			}
		}
	}
	TArray<FAIDynamicParam> Params;
	Query.CollectQueryParams(*GetTransientPackage(), Params);
	for (const FAIDynamicParam& Param : Params)
	{
		Lines.Add(FString::Printf(TEXT("param %s type=%d value=%g"), *Param.ParamName.ToString(), static_cast<int32>(Param.ParamType), Param.Value));
	}
	return FString::Join(Lines, TEXT("\n"));
}

inline FString FirstLineDifference(const FString& A, const FString& B)
{
	TArray<FString> Left;
	TArray<FString> Right;
	A.ParseIntoArrayLines(Left, false);
	B.ParseIntoArrayLines(Right, false);
	for (int32 Index = 0; Index < FMath::Max(Left.Num(), Right.Num()); ++Index)
	{
		const FString L = Left.IsValidIndex(Index) ? Left[Index] : TEXT("<none>");
		const FString R = Right.IsValidIndex(Index) ? Right[Index] : TEXT("<none>");
		if (L != R)
		{
			int32 At = 0;
			while (At < L.Len() && At < R.Len() && L[At] == R[At])
			{
				++At;
			}
			const int32 From = FMath::Max(0, At - 200);
			return FString::Printf(TEXT("line %d at %d: '%s' vs '%s'"), Index, At, *L.Mid(From, 600), *R.Mid(From, 600));
		}
	}
	return TEXT("(equal)");
}

// ---- round trip --------------------------------------------------------------------------------------------------

struct FRoundTrip
{
	FAssetDocumentResult Applied;
	TSharedPtr<FJsonObject> Recreated;
	double CreateMs = 0.0;
	double NewExtractMs = 0.0;
	double NoOpMs = 0.0;
	double ReloadMs = 0.0;
	TArray<FString> Problems;

	FString Timings() const
	{
		return FString::Printf(TEXT("create %.0f ms, extract new %.0f ms, no-op re-apply %.0f ms, reload extract %.0f ms"), CreateMs, NewExtractMs, NoOpMs, ReloadMs);
	}
};

// Create from the extract of Original at Created: the same document, graph, runtime options and params; Diff empty,
// the unchanged re-apply a no-op, the reload stable.
inline FRoundTrip RoundTrip(FAutomationTestBase& Test, const UEnvQuery& Original, const TSharedPtr<FJsonObject>& Extracted, const FTarget& Created, const FString& Name)
{
	FRoundTrip Trip;
	const FString Expected = QueryForm(Original);
	double Start = FPlatformTime::Seconds();
	Trip.Applied = Apply(Created, ForTarget(Extracted, Created, TEXT("Create")));
	Trip.CreateMs = Milliseconds(Start);
	if (!Trip.Applied.IsSuccess())
	{
		Trip.Problems.Add(FString::Printf(TEXT("Create failed: %s"), *DiagnosticsText(Trip.Applied)));
		return Trip;
	}
	Start = FPlatformTime::Seconds();
	const FAssetDocumentResult NewResult = ExtractResult(Created.ObjectPath);
	Trip.NewExtractMs = Milliseconds(Start);
	Trip.Recreated = NewResult.IsSuccess() ? NewResult.Payload : nullptr;
	if (!Trip.Recreated.IsValid())
	{
		Trip.Problems.Add(FString::Printf(TEXT("extract of the created query failed: %s"), *DiagnosticsText(NewResult)));
		return Trip;
	}
	if (WithoutTarget(Extracted) != WithoutTarget(Trip.Recreated))
	{
		Trip.Problems.Add(FString::Printf(TEXT("the created query extracts differently %s"), *FirstDifference(WithoutTarget(Extracted), WithoutTarget(Trip.Recreated))));
	}
	if (const UEnvQuery* NewQuery = Created.Query())
	{
		const FString Actual = QueryForm(*NewQuery);
		if (Expected != Actual)
		{
			Trip.Problems.Add(FString::Printf(TEXT("graph / runtime options / params differ %s"), *FirstLineDifference(Expected, Actual)));
		}
	}
	FString DiffText;
	const TSharedPtr<FJsonObject> Update = ForTarget(Extracted, Created, TEXT("Update"));
	const int32 Changes = DiffCount(Update, DiffText);
	if (Changes != 0)
	{
		Trip.Problems.Add(FString::Printf(TEXT("Diff after Create reports %d changes: %s"), Changes, *DiffText.Left(800)));
	}
	UObject* NewAsset = Created.Get();
	const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Created.PackageFile);
	Start = FPlatformTime::Seconds();
	const FAssetDocumentResult Again = Apply(Created, Update);
	Trip.NoOpMs = Milliseconds(Start);
	if (!Again.IsSuccess() || Again.bSavedAsset || (NewAsset != nullptr && NewAsset->GetOutermost()->IsDirty())
		|| IFileManager::Get().GetTimeStamp(*Created.PackageFile) != Stamp)
	{
		Trip.Problems.Add(FString::Printf(TEXT("the unchanged re-apply is not a no-op: %s saved=%d"), *DiagnosticsText(Again), Again.bSavedAsset ? 1 : 0));
	}
	if (NewAsset != nullptr)
	{
		ReleaseUndoHistory();
		UPackageTools::UnloadPackages({NewAsset->GetOutermost()});
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		if (LoadObject<UEnvQuery>(nullptr, *Created.ObjectPath) == nullptr)
		{
			Trip.Problems.Add(TEXT("reload failed"));
		}
		else
		{
			Start = FPlatformTime::Seconds();
			const FString AfterReload = Serialize(Extract(Test, Created.ObjectPath, Name + TEXT(" reloaded")));
			Trip.ReloadMs = Milliseconds(Start);
			if (AfterReload != Serialize(Trip.Recreated))
			{
				Trip.Problems.Add(FString::Printf(TEXT("extraction after reload differs %s"), *FirstDifference(Serialize(Trip.Recreated), AfterReload)));
			}
			if (const UEnvQuery* Reloaded = Created.Query())
			{
				const FString ReloadedForm = QueryForm(*Reloaded);
				if (ReloadedForm != Expected)
				{
					Trip.Problems.Add(FString::Printf(TEXT("the reloaded query differs %s"), *FirstLineDifference(Expected, ReloadedForm)));
				}
			}
		}
	}
	return Trip;
}

// ---- runtime: a game world with an AI system that runs queries ---------------------------------------------------

struct FQueryWorld
{
	UWorld* World = nullptr;
	AActor* Querier = nullptr;

	FQueryWorld()
	{
		const UWorld::InitializationValues Values = UWorld::InitializationValues().CreatePhysicsScene(true).ShouldSimulatePhysics(false)
			.EnableTraceCollision(true).CreateNavigation(false).CreateAISystem(true).AllowAudioPlayback(false);
		World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AssetDocumentEnvQueryWorld"), nullptr, true, ERHIFeatureLevel::Num, &Values);
		if (World == nullptr)
		{
			return;
		}
		FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
		Context.SetCurrentWorld(World);
		if (World->GetAISystem() == nullptr)
		{
			World->CreateAISystem();
		}
		Querier = Spawn(FVector::ZeroVector);
	}

	~FQueryWorld()
	{
		if (World != nullptr)
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			World->RemoveFromRoot();
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
	}

	AActor* Spawn(const FVector& Location) const
	{
		FActorSpawnParameters Parameters;
		Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return World != nullptr ? World->SpawnActor<ATargetPoint>(ATargetPoint::StaticClass(), FTransform(Location), Parameters) : nullptr;
	}

	// Every item the query returns (AllMatching), location and score, in result order.
	TArray<FString> Run(UEnvQuery& Query) const
	{
		TArray<FString> Items;
		UEnvQueryManager* Manager = World != nullptr ? UEnvQueryManager::GetCurrent(World) : nullptr;
		if (Manager == nullptr || Querier == nullptr)
		{
			Items.Add(TEXT("<no EQS manager>"));
			return Items;
		}
		FEnvQueryRequest Request(&Query, Querier);
		const TSharedPtr<FEnvQueryResult> Result = Manager->RunInstantQuery(Request, EEnvQueryRunMode::AllMatching);
		if (!Result.IsValid())
		{
			Items.Add(TEXT("<no result>"));
			return Items;
		}
		Items.Add(FString::Printf(TEXT("status %d option %d"), static_cast<int32>(Result->GetRawStatus()), Result->OptionIndex));
		for (int32 Index = 0; Index < Result->Items.Num(); ++Index)
		{
			Items.Add(FString::Printf(TEXT("%s score %.4f"), *Result->GetItemAsLocation(Index).ToCompactString(), Result->GetItemScore(Index)));
		}
		return Items;
	}
};
} // namespace AssetDocumentEnvQueryTests
#endif // WITH_DEV_AUTOMATION_TESTS
