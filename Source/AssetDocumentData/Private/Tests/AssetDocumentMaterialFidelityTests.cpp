// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Fidelity of Material / Material Function documents on real content, and free editing compared with the editor.
//
// UEAssetDoc.Material.Fidelity: engine materials and functions of many kinds (lit / unlit /
// translucent / masked / additive, post process, decal, UI, volume, material attributes, function calls, static
// switches, custom HLSL, named reroutes, vertex interpolators, material layers / blends, Substrate) are duplicated
// into /Game; each duplicate is extracted and the document applied as a NEW asset. The two must be equivalent by an
// oracle that does not use the profile: every expression with every reflected property (identity guids aside), the
// material inputs, the Details settings, the parameter set (names, types, defaults, groups), the compile errors and
// the HLSL the translator generates. Re-applying the new asset's extraction is a no-op. The test reports the pass rate
// and every difference.
// UEAssetDoc.Material.FreeEdit: edits through the document on a duplicate of real content compared
// with the same edits made through the editor's API (UMaterialEditingLibrary, Details writes) on a second duplicate:
// insert a node into a connection, delete a node, reconnect, change properties, rename a parameter, move nodes.

#include "AssetDocumentMaterialTestSupport.h"
#include "Utils/AssetDocumentJsonKeys.h"

#include "AssetToolsModule.h"
#include "FileHelpers.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentMaterialFidelityTests
{
using namespace AssetDocumentMaterialTests;

const TCHAR* const Candidates[] = {
	// Materials: domains, blend modes, shading models, features.
	TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"),
	TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"),
	TEXT("/Engine/EngineMaterials/DefaultDeferredDecalMaterial.DefaultDeferredDecalMaterial"),
	TEXT("/Engine/BufferVisualization/WorldNormal.WorldNormal"),
	TEXT("/Engine/EngineDebugMaterials/M_VolumeRenderSphereTracePP.M_VolumeRenderSphereTracePP"),
	TEXT("/Engine/EngineMaterials/ColorGradingWheel.ColorGradingWheel"),
	TEXT("/Engine/EngineMaterials/EmissiveMeshMaterial.EmissiveMeshMaterial"),
	TEXT("/Engine/EditorMaterials/TransformGizmoMaterial_UnlitMasked.TransformGizmoMaterial_UnlitMasked"),
	TEXT("/Engine/EngineMaterials/SparseVolumeMaterial.SparseVolumeMaterial"),
	TEXT("/Engine/EngineMaterials/BaseFlattenMaterial.BaseFlattenMaterial"),
	TEXT("/Engine/OpenWorldTemplate/LandscapeMaterial/M_ProcGrid.M_ProcGrid"),
	TEXT("/Engine/EditorLandscapeResources/CircleBrushMaterial.CircleBrushMaterial"),
	TEXT("/Engine/EditorMaterials/GeometryCollection/SelectedGeometryMaterial.SelectedGeometryMaterial"),
	TEXT("/Engine/EngineDebugMaterials/VertexColorViewMode_RedOnly.VertexColorViewMode_RedOnly"),
	TEXT("/Engine/EditorMaterials/WorldGridPreviewMaterial.WorldGridPreviewMaterial"),
	TEXT("/Engine/InteractiveToolsFramework/Materials/GizmoComponentMaterial.GizmoComponentMaterial"),
	TEXT("/Engine/EngineSky/VolumetricClouds/m_SimpleVolumetricCloud.m_SimpleVolumetricCloud"),
	TEXT("/Engine/EngineMaterials/DefaultPostProcessMaterial.DefaultPostProcessMaterial"),
	TEXT("/Engine/EngineMaterials/DefaultLightFunctionMaterial.DefaultLightFunctionMaterial"),
	TEXT("/Engine/EngineMaterials/DefaultDecalMaterial.DefaultDecalMaterial"),
	TEXT("/Engine/EngineMaterials/Widget3DPassThrough.Widget3DPassThrough"),
	// Material functions, layers and blends.
	TEXT("/Engine/EditorMaterials/CompositeARGBLayer.CompositeARGBLayer"),
	TEXT("/Engine/Functions/Engine_MaterialFunctions01/Texturing/WorldAlignedNormal.WorldAlignedNormal"),
	TEXT("/Engine/Functions/Engine_MaterialFunctions02/Math/MulM4V4.MulM4V4"),
	TEXT("/Engine/Functions/Engine_MaterialFunctions01/Texturing/ParallaxOcclusionMapping_BoundedUVs.ParallaxOcclusionMapping_BoundedUVs"),
	TEXT("/Engine/Functions/MaterialLayerFunctions/MatLayerBlend_Standard.MatLayerBlend_Standard"),
	TEXT("/Engine/Functions/MaterialLayerFunctions/ML_ExampleMaterialLayer.ML_ExampleMaterialLayer"),
	TEXT("/Engine/EngineMaterials/Blends/MLB_Vertical.MLB_Vertical"),
	TEXT("/Engine/Functions/Substrate/MF_Substrate_OpenPBR_Opaque.MF_Substrate_OpenPBR_Opaque"),
	TEXT("/Engine/Functions/Substrate/SMF_UE4Legacy.SMF_UE4Legacy"),
	TEXT("/Engine/Functions/Substrate/Substrate-StandardSurface-Translucent.Substrate-StandardSurface-Translucent"),
	TEXT("/Engine/OpenWorldTemplate/LandscapeMaterial/MF_ProcGrid.MF_ProcGrid"),
	TEXT("/Engine/Functions/Engine_MaterialFunctions02/PivotPainter2/PivotPainter2FoliageShader.PivotPainter2FoliageShader"),
};

// Test assets, removed in reverse order (references to them stay valid).
struct FFixtures
{
	TArray<TUniquePtr<FTarget>> Targets;
	const FTarget& Add(const TCHAR* Prefix) { return *Targets.Add_GetRef(MakeUnique<FTarget>(Prefix)); }
	~FFixtures()
	{
		FinishCompilation();
		for (int32 Index = Targets.Num() - 1; Index >= 0; --Index)
		{
			Targets[Index]->Cleanup();
		}
	}
};

// Duplicates Source into Target's path (the Content Browser's Duplicate) and saves it.
UObject* Duplicate(FAutomationTestBase& Test, UObject* Source, const FTarget& Target)
{
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UObject* Copy = AssetTools.DuplicateAsset(Target.AssetName, FPackageName::GetLongPackagePath(Target.PackageName), Source);
	if (Copy == nullptr)
	{
		return nullptr;
	}
	if (UMaterial* Material = Cast<UMaterial>(Copy))
	{
		UMaterialEditingLibrary::RecompileMaterial(Material);
	}
	Test.TestTrue(*FString::Printf(TEXT("%s saved"), *Target.AssetName), UEditorLoadingAndSavingUtils::SavePackages({Copy->GetOutermost()}, false));
	ReleaseUndoHistory();
	return Copy;
}

// Count of expression classes in a graph (for the report).
FString ClassSummary(UObject* Host)
{
	TMap<FString, int32> Counts;
	for (UMaterialExpression* Expression : GraphExpressions(Host))
	{
		Counts.FindOrAdd(Expression->GetClass()->GetName().Replace(TEXT("MaterialExpression"), TEXT("")))++;
	}
	Counts.KeySort(TLess<FString>());
	TArray<FString> Parts;
	for (const TPair<FString, int32>& Pair : Counts)
	{
		Parts.Add(FString::Printf(TEXT("%s x%d"), *Pair.Key, Pair.Value));
	}
	return FString::Join(Parts, TEXT(", "));
}

// The first differing line of two texts, with its number.
FString FirstDifference(const FString& A, const FString& B)
{
	TArray<FString> Left;
	TArray<FString> Right;
	A.ParseIntoArrayLines(Left, false);
	B.ParseIntoArrayLines(Right, false);
	for (int32 Line = 0; Line < FMath::Max(Left.Num(), Right.Num()); ++Line)
	{
		const FString L = Left.IsValidIndex(Line) ? Left[Line].TrimStartAndEnd() : TEXT("<end>");
		const FString R = Right.IsValidIndex(Line) ? Right[Line].TrimStartAndEnd() : TEXT("<end>");
		if (L != R)
		{
			return FString::Printf(TEXT("line %d: '%s' vs '%s'"), Line + 1, *L.Left(300), *R.Left(300));
		}
	}
	return TEXT("(same lines)");
}

// The first JSON path where two documents differ, with both values.
FString FirstJsonDifference(const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B, const FString& Path)
{
	auto Text = [](const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return FString(TEXT("<missing>"));
		}
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Value, TEXT(""), Writer);
		return Out.Left(300);
	};
	if (!A.IsValid() || !B.IsValid() || A->Type != B->Type)
	{
		return FString::Printf(TEXT("%s: %s vs %s"), *Path, *Text(A), *Text(B));
	}
	if (A->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject> Left = A->AsObject();
		const TSharedPtr<FJsonObject> Right = B->AsObject();
		TSet<FString> Keys;
		for (const auto& Pair : Left->Values) { Keys.Add(FString(*Pair.Key)); }
		for (const auto& Pair : Right->Values) { Keys.Add(FString(*Pair.Key)); }
		for (const FString& Key : Keys)
		{
			const FString Found = FirstJsonDifference(Left->TryGetField(Key), Right->TryGetField(Key), Path + TEXT("/") + Key);
			if (!Found.IsEmpty())
			{
				return Found;
			}
		}
		return FString();
	}
	if (A->Type == EJson::Array)
	{
		const TArray<TSharedPtr<FJsonValue>>& Left = A->AsArray();
		const TArray<TSharedPtr<FJsonValue>>& Right = B->AsArray();
		for (int32 Index = 0; Index < FMath::Max(Left.Num(), Right.Num()); ++Index)
		{
			const FString Found = FirstJsonDifference(Left.IsValidIndex(Index) ? Left[Index] : nullptr,
				Right.IsValidIndex(Index) ? Right[Index] : nullptr, FString::Printf(TEXT("%s/%d"), *Path, Index));
			if (!Found.IsEmpty())
			{
				return Found;
			}
		}
		return FString();
	}
	return Text(A) == Text(B) ? FString() : FString::Printf(TEXT("%s: %s vs %s"), *Path, *Text(A), *Text(B));
}

FString DiagnosticsText(const FAssetDocumentResult& Result)
{
	TArray<FString> Lines;
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Lines.Add(FString::Printf(TEXT("[%s] %s %s"), *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message.Left(400)));
	}
	return Lines.IsEmpty() ? Result.Message : FString::Join(Lines, TEXT(" / "));
}
} // namespace AssetDocumentMaterialFidelityTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMaterialFidelityTest, "UEAssetDoc.Material.Fidelity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMaterialFidelityTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMaterialFidelityTests;
	int32 Tried = 0;
	int32 Passed = 0;
	TArray<FString> Report;
	for (const TCHAR* Path : Candidates)
	{
		UObject* Source = LoadObject<UObject>(nullptr, Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (Source == nullptr)
		{
			AddInfo(FString::Printf(TEXT("SKIP %s: not in this engine's content"), Path));
			continue;
		}
		++Tried;
		FFixtures Fixtures;
		const FTarget& Original = Fixtures.Add(TEXT("Fid_Src"));
		const FTarget& Rebuilt = Fixtures.Add(TEXT("Fid_New"));
		TArray<FString> Problems;
		UObject* Copy = Duplicate(*this, Source, Original);
		const FString Name = FPaths::GetBaseFilename(Path);
		if (Copy == nullptr)
		{
			Problems.Add(TEXT("duplicate failed"));
		}
		const TSharedPtr<FJsonObject> Extracted = Copy != nullptr ? Extract(*this, Original, *Name) : nullptr;
		if (Copy != nullptr && !Extracted.IsValid())
		{
			Problems.Add(TEXT("extract failed"));
		}
		const double Start = FPlatformTime::Seconds();
		const FAssetDocumentResult Applied = Extracted.IsValid() ? Apply(*this, Rebuilt, Retarget(Extracted, Rebuilt), *Name) : FAssetDocumentResult();
		const double Seconds = FPlatformTime::Seconds() - Start;
		UObject* New = Rebuilt.Get();
		if (Extracted.IsValid() && (!Applied.IsSuccess() || New == nullptr))
		{
			Problems.Add(FString::Printf(TEXT("apply as a new asset failed: %s"), *DiagnosticsText(Applied)));
		}
		if (Copy != nullptr && New != nullptr)
		{
			Problems.Append(CompareGraphs(Copy, New));
			if (UMaterial* CopyMaterial = Cast<UMaterial>(Copy))
			{
				UMaterial* NewMaterial = Cast<UMaterial>(New);
				const FString ErrorsBefore = FString::Join(CompileErrors(CopyMaterial), TEXT(" | "));
				const FString ErrorsAfter = FString::Join(CompileErrors(NewMaterial), TEXT(" | "));
				if (ErrorsBefore != ErrorsAfter)
				{
					Problems.Add(FString::Printf(TEXT("compile errors differ: '%s' vs '%s'"), *ErrorsBefore.Left(300), *ErrorsAfter.Left(300)));
				}
				const FString SourceBefore = TranslatedSource(CopyMaterial);
				const FString SourceAfter = TranslatedSource(NewMaterial);
				if (SourceBefore != SourceAfter)
				{
					// Custom vertex interpolators compile in the order of the expression list, which a document does
					// not carry: the same lines in another order are the same shader.
					TArray<FString> Left;
					TArray<FString> Right;
					SourceBefore.ParseIntoArrayLines(Left, false);
					SourceAfter.ParseIntoArrayLines(Right, false);
					Left.Sort();
					Right.Sort();
					if (Left != Right)
					{
						// The lines only one side has (first few).
						TArray<FString> OnlyLeft;
						TArray<FString> OnlyRight;
						int32 L = 0;
						int32 R = 0;
						while ((L < Left.Num() || R < Right.Num()) && OnlyLeft.Num() + OnlyRight.Num() < 6)
						{
							if (R >= Right.Num() || (L < Left.Num() && Left[L] < Right[R])) { OnlyLeft.Add(Left[L++].TrimStartAndEnd().Left(200)); }
							else if (L >= Left.Num() || Right[R] < Left[L]) { OnlyRight.Add(Right[R++].TrimStartAndEnd().Left(200)); }
							else { ++L; ++R; }
						}
						Problems.Add(FString::Printf(TEXT("translated HLSL differs: %s; only in the original: [%s]; only in the new: [%s]"),
							*FirstDifference(SourceBefore, SourceAfter), *FString::Join(OnlyLeft, TEXT(" | ")), *FString::Join(OnlyRight, TEXT(" | "))));
					}
					else
					{
						AddInfo(FString::Printf(TEXT("%s: same HLSL lines in another order (%s)"), *Name, *FirstDifference(SourceBefore, SourceAfter)));
					}
				}
			}
			if (Cast<UMaterialInterface>(Copy) != nullptr && ParameterSet(Cast<UMaterialInterface>(Copy)) != ParameterSet(Cast<UMaterialInterface>(New)))
			{
				Problems.Add(FString::Printf(TEXT("parameter set differs: %s vs %s"), *FString::Join(ParameterSet(Cast<UMaterialInterface>(Copy)), TEXT(";")).Left(400),
					*FString::Join(ParameterSet(Cast<UMaterialInterface>(New)), TEXT(";")).Left(400)));
			}
			// Document level: the new asset extracts to the same document; re-applying it is a no-op.
			const TSharedPtr<FJsonObject> NewExtracted = Extract(*this, Rebuilt, *Name);
			if (NewExtracted.IsValid() && Retarget(NewExtracted, Rebuilt) != Retarget(Extracted, Rebuilt))
			{
				// Another spelling of the same graph (e.g. "<key>" and "<key>.RGB" for output 0 of a node an older
				// engine saved) is fine when the original document applies to the new asset as a no-op.
				const FString Difference = FirstJsonDifference(MakeShared<FJsonValueObject>(Parse(Retarget(Extracted, Rebuilt))),
					MakeShared<FJsonValueObject>(Parse(Retarget(NewExtracted, Rebuilt))), TEXT(""));
				const FAssetDocumentResult Same = Apply(*this, Rebuilt, Serialize(WithAction(Parse(Retarget(Extracted, Rebuilt)), TEXT("Update"))), *Name);
				if (!Same.IsSuccess() || Same.bSavedAsset)
				{
					Problems.Add(FString::Printf(TEXT("the new asset extracts to another document: %s"), *Difference));
				}
				else
				{
					AddInfo(FString::Printf(TEXT("%s: extracts in another spelling (original document applies as a no-op): %s"), *Name, *Difference));
				}
			}
			if (NewExtracted.IsValid())
			{
				const FAssetDocumentResult Reapplied = Apply(*this, Rebuilt, Serialize(WithAction(NewExtracted, TEXT("Update"))), *Name);
				if (!Reapplied.IsSuccess() || Reapplied.bSavedAsset || New->GetOutermost()->IsDirty())
				{
					Problems.Add(FString::Printf(TEXT("re-apply is not a no-op (%s, saved %d)"), *DiagnosticsText(Reapplied), Reapplied.bSavedAsset ? 1 : 0));
				}
			}
		}
		const FString Summary = Copy != nullptr ? ClassSummary(Copy) : FString();
		if (Problems.IsEmpty())
		{
			++Passed;
			Report.Add(FString::Printf(TEXT("PASS %s (%d nodes, %.1f s): %s"), *Name, Copy != nullptr ? GraphExpressions(Copy).Num() : 0, Seconds, *Summary));
		}
		else
		{
			Report.Add(FString::Printf(TEXT("FAIL %s (%s): %s"), *Name, *Summary, *FString::Join(Problems, TEXT(" ;; ")).Left(3000)));
			for (const FString& Problem : Problems)
			{
				AddError(FString::Printf(TEXT("%s: %s"), *Name, *Problem.Left(1000)));
			}
		}
	}
	for (const FString& Line : Report)
	{
		AddInfo(Line);
	}
	AddInfo(FString::Printf(TEXT("FIDELITY %d/%d passed"), Passed, Tried));
	TestTrue(TEXT("at least 15 candidates exist"), Tried >= 15);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMaterialFreeEditTest, "UEAssetDoc.Material.FreeEdit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMaterialFreeEditTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMaterialFidelityTests;
	UMaterial* Source = LoadObject<UMaterial>(nullptr, TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (Source == nullptr)
	{
		Source = LoadObject<UMaterial>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	}
	if (!TestNotNull(TEXT("source material"), Source))
	{
		return false;
	}
	FFixtures Fixtures;
	const FTarget& ByDocument = Fixtures.Add(TEXT("Edit_Doc"));
	const FTarget& ByEditor = Fixtures.Add(TEXT("Edit_Ed"));
	UMaterial* DocumentMaterial = Cast<UMaterial>(Duplicate(*this, Source, ByDocument));
	UMaterial* EditorMaterial = Cast<UMaterial>(Duplicate(*this, Source, ByEditor));
	if (!TestNotNull(TEXT("duplicates"), DocumentMaterial) || !TestNotNull(TEXT("duplicates"), EditorMaterial))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Extracted = Extract(*this, ByDocument, TEXT("Extract"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	TArray<FString> Problems = CompareGraphs(DocumentMaterial, EditorMaterial);
	TestEqual(TEXT("duplicates start equal"), FString::Join(Problems, TEXT("\n")), FString());

	// The editor's edits on one duplicate.
	auto Find = [](UMaterial* Material, const FString& Key) -> UMaterialExpression*
	{
		for (UMaterialExpression* Expression : GraphExpressions(Material))
		{
			if (Expression->GetName() == Key)
			{
				return Expression;
			}
		}
		return nullptr;
	};
	// Pick: the expression feeding the first connected material input, a scalar parameter, a deletable node.
	UMaterialEditorOnlyData* EditorInputs = EditorMaterial->GetEditorOnlyData();
	FExpressionInput* FedInput = nullptr;
	EMaterialProperty FedProperty = MP_MAX;
	FString FedName;
	for (int32 Index = 0; Index < MP_MAX && FedInput == nullptr; ++Index)
	{
		FExpressionInput* Input = EditorMaterial->GetExpressionInputForProperty(static_cast<EMaterialProperty>(Index));
		// (A float connection: a One Minus can sit in it.)
		if (Input != nullptr && Input->Expression != nullptr && !FMaterialAttributeDefinitionMap::GetAttributeName(static_cast<EMaterialProperty>(Index)).IsEmpty()
			&& (Input->Expression->GetOutputValueType(Input->OutputIndex) & MCT_Float) != 0)
		{
			FedInput = Input;
			FedProperty = static_cast<EMaterialProperty>(Index);
			FedName = FMaterialAttributeDefinitionMap::GetAttributeName(FedProperty);
		}
	}
	if (!TestNotNull(TEXT("a connected material input"), FedInput))
	{
		return false;
	}
	UMaterialExpression* Fed = FedInput->Expression;
	const int32 FedOutput = FedInput->OutputIndex;
	UMaterialExpressionScalarParameter* Scalar = nullptr;
	UMaterialExpression* Victim = nullptr;
	for (UMaterialExpression* Expression : GraphExpressions(EditorMaterial))
	{
		Scalar = Scalar != nullptr ? Scalar : Cast<UMaterialExpressionScalarParameter>(Expression);
		if (Victim == nullptr && Expression != Fed && !Expression->IsA<UMaterialExpressionComment>() && !Expression->IsA<UMaterialExpressionScalarParameter>())
		{
			Victim = Expression;
		}
	}
	AddInfo(FString::Printf(TEXT("insert before %s, delete %s, scalar %s"), *FedName, Victim != nullptr ? *Victim->GetName() : TEXT("-"),
		Scalar != nullptr ? *Scalar->GetName() : TEXT("-")));
	{
		// Insert a node into the material input's connection.
		UMaterialExpression* Inserted = UMaterialEditingLibrary::CreateMaterialExpression(EditorMaterial, UMaterialExpressionOneMinus::StaticClass(), -200, 0);
		Inserted->Rename(TEXT("Inserted"), nullptr, REN_DontCreateRedirectors);
		Inserted->GetInput(0)->Connect(FedOutput, Fed);
		FedInput->Connect(0, Inserted);
		// Delete a node (its links go).
		const FString VictimName = Victim != nullptr ? Victim->GetName() : FString();
		if (Victim != nullptr)
		{
			UMaterialEditingLibrary::DeleteMaterialExpression(EditorMaterial, Victim);
		}
		// Change a parameter's properties and rename it (the Details panel), move it.
		if (Scalar != nullptr)
		{
			// Details panel: Modify, PreEditChange, write, PostEditChangeProperty, then the Material Editor's
			// NotifyPostChange (parameter name -> UpdateExpressionParameterName, else PropagateExpressionParameterChanges).
			auto DetailsEdit = [EditorMaterial, Scalar](const TCHAR* PropertyName, TFunctionRef<void()> Write)
			{
				FProperty* Property = UMaterialExpressionScalarParameter::StaticClass()->FindPropertyByName(PropertyName);
				Scalar->Modify();
				Scalar->PreEditChange(Property);
				Write();
				FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
				Scalar->PostEditChangeProperty(Event);
				if (FCString::Strcmp(PropertyName, TEXT("ParameterName")) == 0)
				{
					EditorMaterial->UpdateExpressionParameterName(Scalar);
				}
				else
				{
					EditorMaterial->PropagateExpressionParameterChanges(Scalar);
				}
			};
			DetailsEdit(TEXT("ParameterName"), [Scalar]() { Scalar->ParameterName = TEXT("RenamedByEdit"); });
			DetailsEdit(TEXT("DefaultValue"), [Scalar]() { Scalar->DefaultValue = 0.125f; });
			Scalar->MaterialExpressionEditorX += 64;
			Scalar->MaterialExpressionEditorY -= 32;
		}
		UMaterialEditingLibrary::RecompileMaterial(EditorMaterial);
		ReleaseUndoHistory();

		// The same edits through the document.
		const TSharedPtr<FJsonObject> Document = WithAction(Extracted, TEXT("Update"));
		const TSharedPtr<FJsonObject> Body = Field(Document, TEXT("Body"));
		const TSharedPtr<FJsonObject> Expressions = Field(Body, TEXT("Expressions"));
		const TSharedPtr<FJsonObject> Outputs = Field(Body, TEXT("Outputs"));
		const TSharedPtr<FJsonObject> Insert = MakeShared<FJsonObject>();
		Insert->SetStringField(TEXT("Class"), TEXT("OneMinus"));
		Insert->SetArrayField(TEXT("Position"), TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueNumber>(-200), MakeShared<FJsonValueNumber>(0)});
		EnsureField(Insert, TEXT("Inputs"))->SetStringField(TEXT("Input"), Outputs->GetStringField(FedName));
		Expressions->SetObjectField(TEXT("Inserted"), Insert);
		Outputs->SetStringField(FedName, TEXT("Inserted"));
		if (!VictimName.IsEmpty())
		{
			Expressions->RemoveField(VictimName);
			// Links to the deleted node go (the editor breaks them).
			for (const auto& Pair : Expressions->Values)
			{
				const TSharedPtr<FJsonObject> Inputs = Field(Pair.Value->AsObject(), TEXT("Inputs"));
				TArray<FString> Gone;
				for (const auto& Link : Inputs.IsValid() ? Inputs->Values : TMap<AssetDocumentServerJson::FKey, TSharedPtr<FJsonValue>>())
				{
					FString Upstream;
					Link.Value->AsString().Split(TEXT("."), &Upstream, nullptr);
					if (Link.Value->AsString() == VictimName || Upstream == VictimName)
					{
						Gone.Add(FString(*Link.Key));
					}
				}
				for (const FString& Key : Gone)
				{
					Inputs->RemoveField(Key);
				}
			}
			TArray<FString> GoneOutputs;
			for (const auto& Link : Outputs->Values)
			{
				FString Upstream;
				Link.Value->AsString().Split(TEXT("."), &Upstream, nullptr);
				if (Link.Value->AsString() == VictimName || Upstream == VictimName)
				{
					GoneOutputs.Add(FString(*Link.Key));
				}
			}
			for (const FString& Key : GoneOutputs)
			{
				Outputs->RemoveField(Key);
			}
		}
		if (Scalar != nullptr)
		{
			const TSharedPtr<FJsonObject> Entry = Field(Expressions, *Scalar->GetName());
			EnsureField(Entry, TEXT("Properties"))->SetNumberField(TEXT("DefaultValue"), 0.125);
			Field(Entry, TEXT("Properties"))->SetStringField(TEXT("ParameterName"), TEXT("RenamedByEdit"));
			Entry->SetArrayField(TEXT("Position"), TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueNumber>(Scalar->MaterialExpressionEditorX),
				MakeShared<FJsonValueNumber>(Scalar->MaterialExpressionEditorY)});
		}
		const FString Edit = Serialize(Document);
		if (TestTrue(TEXT("document edit applies"), Apply(*this, ByDocument, Edit, TEXT("Edit")).IsSuccess()))
		{
			DocumentMaterial = ByDocument.Get<UMaterial>();
			Problems = CompareGraphs(EditorMaterial, DocumentMaterial);
			TestEqual(TEXT("document edit equals the editor's edit"), FString::Join(Problems, TEXT("\n")), FString());
			TestEqual(TEXT("same HLSL"), TranslatedSource(DocumentMaterial) == TranslatedSource(EditorMaterial), true);
			TestEqual(TEXT("same parameters"), FString::Join(ParameterSet(DocumentMaterial), TEXT(";")), FString::Join(ParameterSet(EditorMaterial), TEXT(";")));
			TestEqual(TEXT("same compile errors"), FString::Join(CompileErrors(DocumentMaterial), TEXT(";")), FString::Join(CompileErrors(EditorMaterial), TEXT(";")));
			TestNoDiff(*this, Edit, TEXT("after document edit"));
			TestNotNull(TEXT("editor's insert exists"), Find(EditorMaterial, TEXT("Inserted")));
		}
	}
	return !HasAnyErrors();
}
#endif
