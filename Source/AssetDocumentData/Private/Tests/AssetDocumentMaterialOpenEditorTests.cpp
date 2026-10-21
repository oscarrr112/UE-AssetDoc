// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Open-editor apply of materials (Runtime/AssetDocumentOpenEditor.h, AssetDocumentDataMaterialOpenEditor.cpp).
//
// UEAssetDoc.Material.OpenEditorApply.Material / .Function: the Material Editor edits a preview copy
//   of the material (of the function) and writes it back on its Apply, Save and Undo. With the asset open, an apply
//   (a new node linked into the graph) closes the editor and reopens it (Material.EditorReopened); a no-op apply and
//   documents that validation or preflight refuse leave the same editor open; the reopened
//   editor's copy and graph hold the new node. A human edit in the editor is then pending (its Apply is enabled):
//   an apply refuses it before any change (Material.OpenInEditorUnsavedEdits) and the editor keeps it; the editor's
//   own Apply writes it and keeps the applied node. A failed apply (staging save vetoed) rolls back and reopens the
//   editor on the unchanged asset. The editor's undo / redo of a human edit after the apply write the copy back
//   with the applied node. asset_editor's ways out of a pending edit (AssetDocumentOpenEditor.h pending edits): its
//   apply_editor presses the editor's Apply; close with discard_unapplied closes it without the edit. The editor closes
//   without a prompt.
// UEAssetDoc.MaterialInstance.OpenEditorApply: the Material Instance Editor edits the instance in place
//   through its proxy (UMaterialEditorInstanceConstant), which writes its parameter list back on every Details edit
//   and on every undo / redo. With the instance open, an apply (an override changed, one added) is made in place:
//   the editor stays open and its proxy lists the applied overrides; a human Details edit afterwards and the
//   editor's undo / redo keep them. A failed apply rolls back and the proxy shows the rolled-back instance. The
//   editor closes without a prompt.

#include "AssetDocumentMaterialTestSupport.h"

#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/InputBindingManager.h"
#include "Framework/Commands/UICommandInfo.h"
#include "Framework/Commands/UICommandList.h"
#include "IMaterialEditor.h"
#include "MaterialEditor/DEditorParameterValue.h"
#include "MaterialEditor/DEditorVectorParameterValue.h"
#include "MaterialEditor/MaterialEditorInstanceConstant.h"
#include "MaterialGraph/MaterialGraph.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/ScopeExit.h"
#include "Runtime/AssetDocumentOpenEditor.h"
#include "Runtime/AssetDocumentUndoHistory.h"
#include "ScopedTransaction.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "TickableEditorObject.h"
#include "Toolkits/AssetEditorToolkit.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentMaterialOpenEditorTests
{
using namespace AssetDocumentMaterialTests;

const FName MaterialEditorName(TEXT("MaterialEditor"));
const FName MaterialInstanceEditorName(TEXT("MaterialInstanceEditor"));

UAssetEditorSubsystem* Editors()
{
	return GEditor != nullptr ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
}

// Ticks the editor UI (Slate and the editor tickables) and finishes the shader compiles the editor started.
void TickEditorUi()
{
	for (int32 Tick = 0; Tick < 3; ++Tick)
	{
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().Tick();
		}
		FTickableEditorObject::TickObjects(0.f);
	}
	FinishCompilation();
}

FAssetEditorToolkit* ToolkitOf(UObject* Asset, FName EditorName)
{
	UAssetEditorSubsystem* Subsystem = Editors();
	if (Subsystem == nullptr || Asset == nullptr)
	{
		return nullptr;
	}
	for (IAssetEditorInstance* Instance : Subsystem->FindEditorsForAsset(Asset))
	{
		if (Instance != nullptr && Instance->GetEditorName() == EditorName)
		{
			return static_cast<FAssetEditorToolkit*>(Instance);
		}
	}
	return nullptr;
}

IMaterialEditor* MaterialEditorOf(UObject* Asset)
{
	return static_cast<IMaterialEditor*>(ToolkitOf(Asset, MaterialEditorName));
}

// The Material Editor's preview copy (for a function: the preview material holding the copy's expressions).
UMaterial* EditorCopyOf(UObject* Asset)
{
	IMaterialEditor* Editor = MaterialEditorOf(Asset);
	return Editor != nullptr ? Cast<UMaterial>(Editor->GetMaterialInterface()) : nullptr;
}

TSharedPtr<FUICommandInfo> FindCommand(FName Context, FName Name)
{
	return FInputBindingManager::Get().FindCommandInContext(Context, Name);
}

// The editor's toolbar command (mapped in its toolkit commands) can run now.
bool CanExecute(UObject* Asset, FName EditorName, FName Context, FName Name)
{
	FAssetEditorToolkit* Toolkit = ToolkitOf(Asset, EditorName);
	const TSharedPtr<FUICommandInfo> Command = FindCommand(Context, Name);
	return Toolkit != nullptr && Command.IsValid() && Toolkit->GetToolkitCommands()->IsActionMapped(Command)
		&& Toolkit->GetToolkitCommands()->CanExecuteAction(Command.ToSharedRef());
}

bool Execute(UObject* Asset, FName EditorName, FName Context, FName Name)
{
	FAssetEditorToolkit* Toolkit = ToolkitOf(Asset, EditorName);
	const TSharedPtr<FUICommandInfo> Command = FindCommand(Context, Name);
	return Toolkit != nullptr && Command.IsValid() && Toolkit->GetToolkitCommands()->ExecuteAction(Command.ToSharedRef());
}

bool HasCode(const FAssetDocumentResult& Result, const TCHAR* Code)
{
	return Result.Diagnostics.ContainsByPredicate([Code](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == Code; });
}

UMaterialExpression* FindExpression(UObject* Host, const TCHAR* Key)
{
	for (UMaterialExpression* Expression : GraphExpressions(Host))
	{
		if (Expression->GetName() == Key)
		{
			return Expression;
		}
	}
	return nullptr;
}

// A failure after every apply step: the apply's save to its staging file is vetoed (once), and the apply rolls back.
class FStagingSaveVeto
{
public:
	explicit FStagingSaveVeto(FString InPackageName)
		: Previous(FCoreUObjectDelegates::IsPackageOKToSaveDelegate), PackageName(MoveTemp(InPackageName))
	{
		FCoreUObjectDelegates::IsPackageOKToSaveDelegate.BindLambda([this](UPackage* Saved, const FString& Filename, FOutputDevice* Error)
		{
			if (Previous.IsBound() && !Previous.Execute(Saved, Filename, Error))
			{
				return false;
			}
			if (!bTriggered && Saved != nullptr && Saved->GetName() == PackageName
				&& FPaths::ConvertRelativePathToFull(Filename).Contains(TEXT("/Saved/AssetDocument/Runtime/Staging/")))
			{
				bTriggered = true;
				return false;
			}
			return true;
		});
	}
	~FStagingSaveVeto() { FCoreUObjectDelegates::IsPackageOKToSaveDelegate = Previous; }
	FStagingSaveVeto(const FStagingSaveVeto&) = delete;
	FStagingSaveVeto& operator=(const FStagingSaveVeto&) = delete;

	bool bTriggered = false;

private:
	FCoreUObjectDelegates::FIsPackageOKToSaveDelegate Previous;
	FString PackageName;
};

FAssetDocumentResult FailingApply(FAutomationTestBase& Test, const FTarget& Target, const FString& Json, const TCHAR* Stage)
{
	Test.AddExpectedMessage(TEXT("is not allowed to save"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	FStagingSaveVeto Veto(Target.PackageName);
	const FAssetDocumentResult Result = Apply(Test, Target, Json, Stage);
	Test.TestTrue(*FString::Printf(TEXT("%s: the staging save was vetoed"), Stage), Veto.bTriggered);
	return Result;
}

bool SaveAsset(FAutomationTestBase& Test, UObject* Asset, const TCHAR* Stage)
{
	return Test.TestTrue(*FString::Printf(TEXT("%s: saved"), Stage),
		Asset != nullptr && UEditorLoadingAndSavingUtils::SavePackages({Asset->GetOutermost()}, false));
}

// Editors closed and the package left clean before the fixture goes.
void CloseAndClean(const FTarget& Target)
{
	if (UObject* Asset = Target.Get())
	{
		if (UAssetEditorSubsystem* Subsystem = Editors())
		{
			Subsystem->CloseAllEditorsForAsset(Asset);
		}
		Asset->GetOutermost()->ClearDirtyFlag();
	}
}

// ---- Material / Material Function ------------------------------------------------------------------------------

// One variant of the Material Editor test: the asset, the node the apply adds and the value a human edits.
struct FMaterialVariant
{
	const TCHAR* Prefix;
	const TCHAR* Class;
	FString Body;
	// The expression a human edits in the editor and its float property.
	const TCHAR* EditedKey;
	const TCHAR* EditedProperty;
	// Adds the node "Added" to an extracted document, linked into the graph.
	TFunction<void(const TSharedPtr<FJsonObject>&)> AddNode;
};

float FloatOf(UObject* Host, const FMaterialVariant& Variant)
{
	UMaterialExpression* Expression = FindExpression(Host, Variant.EditedKey);
	const FFloatProperty* Property = Expression != nullptr ? CastField<FFloatProperty>(Expression->GetClass()->FindPropertyByName(Variant.EditedProperty)) : nullptr;
	return Property != nullptr ? Property->GetPropertyValue_InContainer(Expression) : -1.f;
}

// A Details edit of the copy's expression in the Material Editor (its Details panel marks the copy dirty).
bool EditInEditor(UObject* Asset, const FMaterialVariant& Variant, float Value, bool bTransaction)
{
	UMaterialExpression* Expression = FindExpression(EditorCopyOf(Asset), Variant.EditedKey);
	FFloatProperty* Property = Expression != nullptr ? CastField<FFloatProperty>(Expression->GetClass()->FindPropertyByName(Variant.EditedProperty)) : nullptr;
	if (Property == nullptr)
	{
		return false;
	}
	TOptional<FScopedTransaction> Transaction;
	if (bTransaction)
	{
		Transaction.Emplace(NSLOCTEXT("AssetDocumentMaterialOpenEditorTests", "Edit", "Edit Node"));
	}
	Expression->Modify();
	Expression->PreEditChange(Property);
	Property->SetPropertyValue_InContainer(Expression, Value);
	FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
	Expression->PostEditChangeProperty(Event);
	MaterialEditorOf(Asset)->MarkMaterialDirty();
	return true;
}

bool RunMaterialEditorTest(FAutomationTestBase& Test, const FMaterialVariant& Variant)
{
	UAssetEditorSubsystem* Subsystem = Editors();
	if (!Test.TestNotNull(TEXT("asset editor subsystem"), Subsystem) || GEditor->Trans == nullptr)
	{
		return false;
	}
	const FTarget Target(Variant.Prefix);
	ON_SCOPE_EXIT
	{
		FinishCompilation();
		CloseAndClean(Target);
		Target.Cleanup();
	};
	if (!Test.TestTrue(TEXT("create applies"), Apply(Test, Target, Document(Target, Variant.Class, TEXT("Create"), Variant.Body), TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	FinishCompilation();
	ReleaseUndoHistory();
	const FString Created = Serialize(Extract(Test, Target, TEXT("created")));
	const FString Original = Serialize(WithAction(Parse(Created), TEXT("Update")));
	if (!Subsystem->OpenEditorForAsset(Target.Get()))
	{
		Test.AddWarning(TEXT("The Material Editor did not open in this session; the open-editor apply was not exercised."));
		return true;
	}
	TickEditorUi();
	Test.TestFalse(TEXT("opening the editor leaves the package clean"), Target.Get()->GetOutermost()->IsDirty());
	if (!Test.TestNotNull(TEXT("the Material Editor's copy"), FindExpression(EditorCopyOf(Target.Get()), Variant.EditedKey)))
	{
		return false;
	}
	Test.TestFalse(TEXT("a fresh editor has nothing to apply"), CanExecute(Target.Get(), MaterialEditorName, TEXT("MaterialEditor"), TEXT("Apply")));
	const UMaterial* CopyBefore = EditorCopyOf(Target.Get());

	// 0. The editor is closed only once the apply is about to change the asset. A document equal to the asset
	//    (a no-op) and documents that validation or preflight refuse leave the same editor open, untouched.
	{
		const IMaterialEditor* EditorBefore = MaterialEditorOf(Target.Get());
		const FAssetDocumentResult NoOp = Apply(Test, Target, Original, TEXT("no-op while open"));
		Test.TestTrue(TEXT("a no-op apply while open succeeds"), NoOp.IsSuccess());
		Test.TestFalse(TEXT("a no-op apply saves nothing"), NoOp.bSavedAsset);
		Test.TestFalse(TEXT("a no-op apply does not reopen the editor"), HasCode(NoOp, TEXT("Material.EditorReopened")));
		const TSharedPtr<FJsonObject> UnknownKey = Parse(Original);
		Field(UnknownKey, TEXT("Body"))->SetNumberField(TEXT("NoSuchKey"), 1);
		const TSharedPtr<FJsonObject> UnknownProperty = Parse(Original);
		EnsureField(ExpressionOf(UnknownProperty, Variant.EditedKey), TEXT("Properties"))->SetNumberField(TEXT("NoSuchProperty"), 1);
		for (const TSharedPtr<FJsonObject>& Invalid : {UnknownKey, UnknownProperty})
		{
			const FAssetDocumentResult Refused = Apply(Test, Target, Serialize(Invalid), TEXT("invalid document while open"));
			Test.TestFalse(TEXT("an invalid document fails"), Refused.IsSuccess());
			Test.TestFalse(TEXT("an invalid document does not reopen the editor"), HasCode(Refused, TEXT("Material.EditorReopened")));
		}
		TickEditorUi();
		Test.TestTrue(TEXT("the same Material Editor is still open"), EditorBefore != nullptr && MaterialEditorOf(Target.Get()) == EditorBefore);
		Test.TestTrue(TEXT("with the same preview copy"), EditorCopyOf(Target.Get()) == CopyBefore);
		Test.TestFalse(TEXT("the package stays clean"), Target.Get()->GetOutermost()->IsDirty());
	}

	// 1. An apply adds a node linked into the graph: the editor is closed for it and reopened on the applied asset.
	const TSharedPtr<FJsonObject> Added = WithAction(Parse(Created), TEXT("Update"));
	Variant.AddNode(Added);
	const FAssetDocumentResult Applied = Apply(Test, Target, Serialize(Added), TEXT("apply while open"));
	if (!Test.TestTrue(TEXT("apply while open"), Applied.IsSuccess()))
	{
		return false;
	}
	FinishCompilation();
	Test.TestTrue(TEXT("the result reports the reopened editor"), HasCode(Applied, TEXT("Material.EditorReopened")));
	Test.TestNotNull(TEXT("the asset has the new node"), FindExpression(Target.Get(), TEXT("Added")));
	Test.TestFalse(TEXT("the apply saved the package"), Target.Get()->GetOutermost()->IsDirty());
	Test.TestFalse(TEXT("the apply is not an undo step"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	TickEditorUi();
	UMaterial* Copy = EditorCopyOf(Target.Get());
	if (!Test.TestNotNull(TEXT("the Material Editor is open again"), Copy))
	{
		return false;
	}
	Test.AddInfo(FString::Printf(TEXT("the reopened editor %s the preview copy"), Copy != CopyBefore ? TEXT("made a new") : TEXT("kept")));
	UMaterialExpression* Shown = FindExpression(Copy, TEXT("Added"));
	Test.TestNotNull(TEXT("the reopened editor's copy holds the new node"), Shown);
	Test.TestTrue(TEXT("the editor's graph shows the new node"), Shown != nullptr && Shown->GraphNode != nullptr && Copy->MaterialGraph != nullptr
		&& Copy->MaterialGraph->Nodes.Contains(Shown->GraphNode));
	Test.TestFalse(TEXT("the reopened editor has nothing to apply"), CanExecute(Target.Get(), MaterialEditorName, TEXT("MaterialEditor"), TEXT("Apply")));
	const FString AppliedText = Serialize(Extract(Test, Target, TEXT("after apply while open")));

	// 2. A human edit in the editor is pending (its Apply is enabled): an apply refuses it before any change and the
	//    editor keeps it; the editor's own Apply writes it, and the applied node stays.
	if (!Test.TestTrue(TEXT("a human edit in the editor"), EditInEditor(Target.Get(), Variant, 0.75f, false)))
	{
		return false;
	}
	Test.TestTrue(TEXT("the human edit is pending in the editor"), CanExecute(Target.Get(), MaterialEditorName, TEXT("MaterialEditor"), TEXT("Apply")));
	const IMaterialEditor* EditorBefore = MaterialEditorOf(Target.Get());
	const FAssetDocumentResult Pending = Apply(Test, Target, Original, TEXT("apply over the pending edit"));
	TestFailsWith(Test, Pending, TEXT("Material.OpenInEditorUnsavedEdits"), TEXT("/"), TEXT("apply over the pending edit"));
	Test.TestFalse(TEXT("the refused apply did not reopen the editor"), HasCode(Pending, TEXT("Material.EditorReopened")));
	Test.TestTrue(TEXT("the editor stays open"), MaterialEditorOf(Target.Get()) == EditorBefore);
	Test.TestTrue(TEXT("the editor keeps the pending edit"), CanExecute(Target.Get(), MaterialEditorName, TEXT("MaterialEditor"), TEXT("Apply"))
		&& FMath::IsNearlyEqual(FloatOf(EditorCopyOf(Target.Get()), Variant), 0.75f));
	ExtractEquals(Test, Target, AppliedText, TEXT("after the refusal"));
	// The refusal names asset_editor's ways out, and asset_editor apply_editor presses the editor's Apply.
	Test.TestTrue(TEXT("the refusal names apply_editor and discard_unapplied"), Pending.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& D)
		{
			return D.Code == TEXT("Material.OpenInEditorUnsavedEdits") && D.Message.Contains(TEXT("apply_editor")) && D.Message.Contains(TEXT("discard_unapplied"));
		}));
	Test.TestTrue(TEXT("the refusal names the asset, not the editor's preview copy"), Pending.Diagnostics.ContainsByPredicate(
		[Name = Target.Get()->GetName()](const FAssetDocumentDiagnostic& D)
		{
			return D.Code == TEXT("Material.OpenInEditorUnsavedEdits") && D.Message.StartsWith(Name) && !D.Message.Contains(TEXT("PreviewMaterial"));
		}));
	Test.TestEqual(TEXT("the pending edits' button"), UE::AssetDocument::OpenEditor::GetPendingEditsButton(*Target.Get()), FString(TEXT("Apply")));
	{
		FString Reason;
		Test.TestTrue(TEXT("the editor's Apply (asset_editor apply_editor)"), UE::AssetDocument::OpenEditor::ApplyPendingEdits(*Target.Get(), Reason));
	}
	FinishCompilation();
	Test.TestTrue(TEXT("the editor's Apply writes the human edit"), FMath::IsNearlyEqual(FloatOf(Target.Get(), Variant), 0.75f));
	Test.TestNotNull(TEXT("the editor's Apply keeps the applied node"), FindExpression(Target.Get(), TEXT("Added")));
	Test.TestFalse(TEXT("nothing is pending after the editor's Apply"), CanExecute(Target.Get(), MaterialEditorName, TEXT("MaterialEditor"), TEXT("Apply")));
	SaveAsset(Test, Target.Get(), TEXT("the human save"));
	ReleaseUndoHistory();
	const FString HumanText = Serialize(Extract(Test, Target, TEXT("after the human save")));

	// 3. A failure after every step (the staging save is vetoed) rolls back and reopens the editor on the unchanged asset.
	const FAssetDocumentResult Failed = FailingApply(Test, Target, Original, TEXT("failing apply while open"));
	FinishCompilation();
	Test.TestFalse(TEXT("failing apply while open fails"), Failed.IsSuccess());
	Test.TestTrue(TEXT("the failed result reports the reopened editor"), HasCode(Failed, TEXT("Material.EditorReopened")));
	ExtractEquals(Test, Target, HumanText, TEXT("after the rollback"));
	Test.TestFalse(TEXT("after the rollback the package is clean"), Target.Get()->GetOutermost()->IsDirty());
	TickEditorUi();
	Test.TestNotNull(TEXT("after the rollback the reopened editor's copy holds the node"), FindExpression(EditorCopyOf(Target.Get()), TEXT("Added")));
	Test.TestTrue(TEXT("after the rollback the reopened editor's copy holds the human edit"),
		FMath::IsNearlyEqual(FloatOf(EditorCopyOf(Target.Get()), Variant), 0.75f));

	// 4. The editor's own undo / redo after the apply (its PostUndo writes the copy back to the asset).
	ReleaseUndoHistory();
	if (Test.TestTrue(TEXT("a human edit as a transaction"), EditInEditor(Target.Get(), Variant, 0.25f, true)))
	{
		Test.TestTrue(TEXT("the editor's undo"), GEditor->Trans->CanUndo() && GEditor->UndoTransaction());
		FinishCompilation();
		Test.TestTrue(TEXT("after the editor's undo the asset has the value before the edit"), FMath::IsNearlyEqual(FloatOf(Target.Get(), Variant), 0.75f));
		Test.TestNotNull(TEXT("after the editor's undo the asset keeps the applied node"), FindExpression(Target.Get(), TEXT("Added")));
		Test.TestTrue(TEXT("the editor's redo"), GEditor->Trans->CanRedo() && GEditor->RedoTransaction());
		FinishCompilation();
		Test.TestTrue(TEXT("after the editor's redo the copy has the edit again"), FMath::IsNearlyEqual(FloatOf(EditorCopyOf(Target.Get()), Variant), 0.25f));
		Test.TestNotNull(TEXT("after the editor's redo the asset keeps the applied node"), FindExpression(Target.Get(), TEXT("Added")));
		TickEditorUi();
	}
	// 5. asset_editor close with discard_unapplied: the editor closes without its pending edit (its close prompt's "No")
	//    and the asset keeps what it had.
	ReleaseUndoHistory();
	if (MaterialEditorOf(Target.Get()) != nullptr)
	{
		FString Reason;
		if (!UE::AssetDocument::OpenEditor::GetPendingEditsButton(*Target.Get()).IsEmpty())
		{
			UE::AssetDocument::OpenEditor::ApplyPendingEdits(*Target.Get(), Reason);
			FinishCompilation();
		}
		SaveAsset(Test, Target.Get(), TEXT("before the discarded edit"));
		ReleaseUndoHistory();
		const FString Kept = Serialize(Extract(Test, Target, TEXT("before the discarded edit")));
		if (Test.TestTrue(TEXT("a human edit to discard"), EditInEditor(Target.Get(), Variant, 0.6f, false)))
		{
			Test.TestEqual(TEXT("the edit is pending"), UE::AssetDocument::OpenEditor::GetPendingEditsButton(*Target.Get()), FString(TEXT("Apply")));
			Test.TestTrue(TEXT("the editor closes without the pending edit"), UE::AssetDocument::OpenEditor::CloseDiscardingPendingEdits(*Target.Get(), Reason));
			Test.TestNull(TEXT("no Material Editor is left open"), MaterialEditorOf(Target.Get()));
			ExtractEquals(Test, Target, Kept, TEXT("after closing without the pending edit"));
			Test.TestFalse(TEXT("the package stays saved"), Target.Get()->GetOutermost()->IsDirty());
		}
	}
	ReleaseUndoHistory();
	if (UObject* Asset = Target.Get())
	{
		if (CanExecute(Asset, MaterialEditorName, TEXT("MaterialEditor"), TEXT("Apply")))
		{
			Execute(Asset, MaterialEditorName, TEXT("MaterialEditor"), TEXT("Apply"));
			FinishCompilation();
		}
		Asset->GetOutermost()->ClearDirtyFlag();
		Subsystem->CloseAllEditorsForAsset(Asset);
		Test.TestNull(TEXT("the editor closed"), Subsystem->FindEditorForAsset(Asset, false));
	}
	return !Test.HasAnyErrors();
}
} // namespace AssetDocumentMaterialOpenEditorTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMaterialOpenEditorApplyMaterialTest, "UEAssetDoc.Material.OpenEditorApply.Material",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMaterialOpenEditorApplyMaterialTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMaterialOpenEditorTests;
	FMaterialVariant Variant;
	Variant.Prefix = TEXT("M_OpenEd");
	Variant.Class = MaterialClass;
	Variant.Body = TEXT(R"json(
"Expressions": {
  "Rough": {"Class": "ScalarParameter", "Position": [-400, 0], "Properties": {"ParameterName": "Roughness", "DefaultValue": 0.5}},
  "Color": {"Class": "VectorParameter", "Position": [-400, 200], "Properties": {"ParameterName": "Color"}}
},
"Outputs": {"Roughness": "Rough", "BaseColor": "Color.RGB"})json");
	Variant.EditedKey = TEXT("Rough");
	Variant.EditedProperty = TEXT("DefaultValue");
	Variant.AddNode = [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Half = MakeShared<FJsonObject>();
		Half->SetStringField(TEXT("Class"), TEXT("Multiply"));
		EnsureField(Half, TEXT("Properties"))->SetNumberField(TEXT("ConstB"), 0.5);
		EnsureField(Half, TEXT("Inputs"))->SetStringField(TEXT("A"), TEXT("Rough"));
		Field(Field(Document, TEXT("Body")), TEXT("Expressions"))->SetObjectField(TEXT("Added"), Half);
		Field(Field(Document, TEXT("Body")), TEXT("Outputs"))->SetStringField(TEXT("Specular"), TEXT("Added"));
	};
	return RunMaterialEditorTest(*this, Variant);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMaterialOpenEditorApplyFunctionTest, "UEAssetDoc.Material.OpenEditorApply.Function",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMaterialOpenEditorApplyFunctionTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMaterialOpenEditorTests;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	// The Material editor's Apply rebuilds the function in place from its preview copy; the engine then logs that
	// the rebuilt function's editor-only data was not loaded, although the data is there.
	AddExpectedError(TEXT("is missing editor-only data"), EAutomationExpectedErrorFlags::Contains, 0);
#endif
	FMaterialVariant Variant;
	Variant.Prefix = TEXT("MF_OpenEd");
	Variant.Class = FunctionClass;
	Variant.Body = TEXT(R"json(
"Expressions": {
  "Value": {"Class": "FunctionInput", "Position": [-500, 0], "Properties": {"InputName": "Value", "InputType": "FunctionInput_Vector3"}},
  "Mul": {"Class": "Multiply", "Position": [-250, 0], "Properties": {"ConstB": 2}, "Inputs": {"A": "Value"}},
  "Result": {"Class": "FunctionOutput", "Position": [0, 0], "Properties": {"OutputName": "Result"}, "Inputs": {"A": "Mul"}}
})json");
	Variant.EditedKey = TEXT("Mul");
	Variant.EditedProperty = TEXT("ConstB");
	Variant.AddNode = [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Bias = MakeShared<FJsonObject>();
		Bias->SetStringField(TEXT("Class"), TEXT("Add"));
		EnsureField(Bias, TEXT("Properties"))->SetNumberField(TEXT("ConstB"), 1.0);
		EnsureField(Bias, TEXT("Inputs"))->SetStringField(TEXT("A"), TEXT("Mul"));
		Field(Field(Document, TEXT("Body")), TEXT("Expressions"))->SetObjectField(TEXT("Added"), Bias);
		EnsureField(ExpressionOf(Document, TEXT("Result")), TEXT("Inputs"))->SetStringField(TEXT("A"), TEXT("Added"));
	};
	return RunMaterialEditorTest(*this, Variant);
}

// ---- Material Instance -------------------------------------------------------------------------------------------

namespace AssetDocumentMaterialOpenEditorTests
{
// The Material Instance Editor's proxy of the instance (its Details panel's object).
UMaterialEditorInstanceConstant* ProxyOf(UObject* Asset)
{
	FAssetEditorToolkit* Toolkit = ToolkitOf(Asset, MaterialInstanceEditorName);
	const TArray<UObject*>* EditedObjects = Toolkit != nullptr ? Toolkit->GetObjectsCurrentlyBeingEdited() : nullptr;
	if (EditedObjects == nullptr)
	{
		return nullptr;
	}
	for (UObject* Edited : *EditedObjects)
	{
		if (UMaterialEditorInstanceConstant* Proxy = Cast<UMaterialEditorInstanceConstant>(Edited))
		{
			return Proxy;
		}
	}
	return nullptr;
}

UDEditorParameterValue* ProxyParameter(UMaterialEditorInstanceConstant* Proxy, const TCHAR* Name)
{
	if (Proxy != nullptr)
	{
		for (FEditorParameterGroup& Group : Proxy->ParameterGroups)
		{
			for (UDEditorParameterValue* Parameter : Group.Parameters)
			{
				if (Parameter != nullptr && Parameter->ParameterInfo.Name == Name)
				{
					return Parameter;
				}
			}
		}
	}
	return nullptr;
}

// "<value>" when the proxy overrides the scalar, "-" when it lists it without override, "" when it does not list it.
FString ProxyScalar(UMaterialEditorInstanceConstant* Proxy, const TCHAR* Name)
{
	UDEditorParameterValue* Parameter = ProxyParameter(Proxy, Name);
	FMaterialParameterMetadata Value;
	if (Parameter == nullptr || !Parameter->GetValue(Value))
	{
		return FString();
	}
	return Parameter->bOverride ? FString::SanitizeFloat(Value.Value.AsScalar()) : FString(TEXT("-"));
}

float InstanceScalar(UObject* Asset, const TCHAR* Name)
{
	float Value = -1.f;
	if (UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Asset))
	{
		Instance->GetScalarParameterValue(FHashedMaterialParameterInfo(Name), Value, true);
	}
	return Value;
}

bool InstanceOverridesVector(UObject* Asset, const TCHAR* Name)
{
	const UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Asset);
	return Instance != nullptr && Instance->VectorParameterValues.ContainsByPredicate([Name](const FVectorParameterValue& Value)
	{
		return Value.ParameterInfo.Name == Name;
	});
}

// A Details edit in the Material Instance Editor: the Tint override set (the proxy copies itself to the instance).
bool OverrideTintInEditor(UObject* Asset, const FLinearColor& Color, bool bTransaction)
{
	UMaterialEditorInstanceConstant* Proxy = ProxyOf(Asset);
	UDEditorVectorParameterValue* Tint = Cast<UDEditorVectorParameterValue>(ProxyParameter(Proxy, TEXT("Tint")));
	if (Tint == nullptr)
	{
		return false;
	}
	TOptional<FScopedTransaction> Transaction;
	if (bTransaction)
	{
		Transaction.Emplace(NSLOCTEXT("AssetDocumentMaterialOpenEditorTests", "Override", "Edit Parameter"));
	}
	Proxy->Modify();
	Tint->Modify();
	Tint->bOverride = true;
	Tint->ParameterValue = Color;
	FPropertyChangedEvent Event(UMaterialEditorInstanceConstant::StaticClass()->FindPropertyByName(TEXT("ParameterGroups")), EPropertyChangeType::ValueSet);
	Proxy->PostEditChangeProperty(Event);
	return true;
}
} // namespace AssetDocumentMaterialOpenEditorTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMaterialInstanceOpenEditorApplyTest, "UEAssetDoc.MaterialInstance.OpenEditorApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMaterialInstanceOpenEditorApplyTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMaterialOpenEditorTests;
	UAssetEditorSubsystem* Subsystem = Editors();
	if (!TestNotNull(TEXT("asset editor subsystem"), Subsystem) || GEditor->Trans == nullptr)
	{
		return false;
	}
	const FTarget Parent(TEXT("M_OpenEdParent"));
	const FTarget Target(TEXT("MI_OpenEd"));
	ON_SCOPE_EXIT
	{
		FinishCompilation();
		CloseAndClean(Target);
		Target.Cleanup();
		Parent.Cleanup();
	};
	const TCHAR* InstanceClass = TEXT("/Script/Engine.MaterialInstanceConstant");
	if (!TestTrue(TEXT("parent applies"), Apply(*this, Parent, Document(Parent, MaterialClass, TEXT("Create"), TEXT(R"json(
"Expressions": {
  "Rough": {"Class": "ScalarParameter", "Position": [-400, 0], "Properties": {"ParameterName": "Roughness", "DefaultValue": 0.5}},
  "Metal": {"Class": "ScalarParameter", "Position": [-400, 150], "Properties": {"ParameterName": "Metallic", "DefaultValue": 0}},
  "Tint": {"Class": "VectorParameter", "Position": [-400, 300], "Properties": {"ParameterName": "Tint"}}
},
"Outputs": {"Roughness": "Rough", "Metallic": "Metal", "BaseColor": "Tint.RGB"})json")), TEXT("parent")).IsSuccess()))
	{
		return false;
	}
	FinishCompilation();
	auto InstanceDocument = [&Target, &Parent, InstanceClass](const TCHAR* Action, const TCHAR* ParametersJson)
	{
		return Document(Target, InstanceClass, Action, FString::Printf(TEXT(R"json("Parent":"%s","Parameters":%s)json"), *Parent.ObjectPath, ParametersJson));
	};
	if (!TestTrue(TEXT("instance applies"), Apply(*this, Target, InstanceDocument(TEXT("Create"), TEXT(R"json({"Roughness":0.25})json")), TEXT("instance")).IsSuccess()))
	{
		return false;
	}
	FinishCompilation();
	ReleaseUndoHistory();
	if (!Subsystem->OpenEditorForAsset(Target.Get()))
	{
		AddWarning(TEXT("The Material Instance Editor did not open in this session; the open-editor apply was not exercised."));
		return true;
	}
	TickEditorUi();
	TestFalse(TEXT("opening the editor leaves the package clean"), Target.Get()->GetOutermost()->IsDirty());
	UMaterialEditorInstanceConstant* Proxy = ProxyOf(Target.Get());
	if (!TestNotNull(TEXT("the editor's proxy"), Proxy))
	{
		return false;
	}
	TestEqual(TEXT("the proxy lists the Roughness override"), ProxyScalar(Proxy, TEXT("Roughness")), FString(TEXT("0.25")));
	TestEqual(TEXT("the proxy lists Metallic without override"), ProxyScalar(Proxy, TEXT("Metallic")), FString(TEXT("-")));

	// 1. An apply changes an override and adds one, in place: the editor stays open and its proxy lists them.
	const FAssetDocumentResult Applied = Apply(*this, Target, InstanceDocument(TEXT("Update"), TEXT(R"json({"Roughness":0.75,"Metallic":1})json")),
		TEXT("apply while open"));
	if (!TestTrue(TEXT("apply while open"), Applied.IsSuccess()))
	{
		return false;
	}
	FinishCompilation();
	TestFalse(TEXT("the editor is not reopened"), HasCode(Applied, TEXT("MaterialInstance.EditorReopened")));
	TestEqual(TEXT("the instance's Roughness"), InstanceScalar(Target.Get(), TEXT("Roughness")), 0.75f);
	TestEqual(TEXT("the instance's Metallic"), InstanceScalar(Target.Get(), TEXT("Metallic")), 1.f);
	TestFalse(TEXT("the apply saved the package"), Target.Get()->GetOutermost()->IsDirty());
	TestFalse(TEXT("the apply is not an undo step"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	TestTrue(TEXT("the same editor is still open"), ProxyOf(Target.Get()) == Proxy);
	TestTrue(TEXT("the proxy edits the instance"), Proxy->SourceInstance == Target.Get());
	TestEqual(TEXT("the editor's proxy shows the new Roughness"), ProxyScalar(Proxy, TEXT("Roughness")), FString(TEXT("0.75")));
	TestEqual(TEXT("the editor's proxy shows the new Metallic override"), ProxyScalar(Proxy, TEXT("Metallic")), FString(TEXT("1.0")));
	TickEditorUi();
	const FString AppliedText = Serialize(Extract(*this, Target, TEXT("after apply while open")));

	// 2. The editor writes its proxy back on every Details edit: a human edit afterwards keeps the apply.
	TestTrue(TEXT("a human Details edit"), OverrideTintInEditor(Target.Get(), FLinearColor(0.f, 1.f, 0.f, 1.f), false));
	FinishCompilation();
	TestTrue(TEXT("the human edit landed"), InstanceOverridesVector(Target.Get(), TEXT("Tint")));
	TestEqual(TEXT("the human edit keeps the applied Roughness"), InstanceScalar(Target.Get(), TEXT("Roughness")), 0.75f);
	TestEqual(TEXT("the human edit keeps the applied Metallic"), InstanceScalar(Target.Get(), TEXT("Metallic")), 1.f);
	TestTrue(TEXT("the human edit dirtied the package"), Target.Get()->GetOutermost()->IsDirty());
	SaveAsset(*this, Target.Get(), TEXT("the human save"));
	ReleaseUndoHistory();
	const FString HumanText = Serialize(Extract(*this, Target, TEXT("after the human save")));

	// 3. A failed apply rolls back while the editor is open; the proxy shows the rolled-back instance.
	const FAssetDocumentResult Failed = FailingApply(*this, Target, InstanceDocument(TEXT("Update"), TEXT(R"json({"Roughness":0.1})json")),
		TEXT("failing apply while open"));
	FinishCompilation();
	TestFalse(TEXT("failing apply while open fails"), Failed.IsSuccess());
	ExtractEquals(*this, Target, HumanText, TEXT("after the rollback"));
	TestFalse(TEXT("after the rollback the package is clean"), Target.Get()->GetOutermost()->IsDirty());
	Proxy = ProxyOf(Target.Get());
	TestTrue(TEXT("after the rollback the proxy edits the instance"), Proxy != nullptr && Proxy->SourceInstance == Target.Get());
	TestEqual(TEXT("after the rollback the proxy shows the Roughness"), ProxyScalar(Proxy, TEXT("Roughness")), FString(TEXT("0.75")));
	TestTrue(TEXT("after the rollback the proxy shows the Tint override"), ProxyParameter(Proxy, TEXT("Tint")) != nullptr && ProxyParameter(Proxy, TEXT("Tint"))->bOverride);
	TickEditorUi();

	// 4. The editor's undo / redo (its PostUndo copies the proxy to the instance) keep the apply.
	ReleaseUndoHistory();
	if (TestTrue(TEXT("a human edit as a transaction"), OverrideTintInEditor(Target.Get(), FLinearColor(1.f, 0.f, 0.f, 1.f), true)))
	{
		TestTrue(TEXT("the editor's undo"), GEditor->Trans->CanUndo() && GEditor->UndoTransaction());
		FinishCompilation();
		TestEqual(TEXT("after the editor's undo the instance keeps the applied Roughness"), InstanceScalar(Target.Get(), TEXT("Roughness")), 0.75f);
		TestEqual(TEXT("after the editor's undo the instance keeps the applied Metallic"), InstanceScalar(Target.Get(), TEXT("Metallic")), 1.f);
		TestTrue(TEXT("the editor's redo"), GEditor->Trans->CanRedo() && GEditor->RedoTransaction());
		FinishCompilation();
		TestEqual(TEXT("after the editor's redo the instance keeps the applied Roughness"), InstanceScalar(Target.Get(), TEXT("Roughness")), 0.75f);
		TickEditorUi();
	}
	ReleaseUndoHistory();
	if (UObject* Asset = Target.Get())
	{
		Asset->GetOutermost()->ClearDirtyFlag();
		Subsystem->CloseAllEditorsForAsset(Asset);
		TestNull(TEXT("the editor closed"), Subsystem->FindEditorForAsset(Asset, false));
	}
	return !HasAnyErrors();
}
#endif // WITH_DEV_AUTOMATION_TESTS
