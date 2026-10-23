// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// End-to-end MaterialInstanceConstant through the production Service and the Python Profile
// (profile.material_instance_constant.v1). The parent materials are built here as the Material
// Editor builds them (UMaterialEditingLibrary), since UMaterial itself has no document route yet.
//
// UEAssetDoc.MaterialInstance.Python: one ApplyFile creates an instance of M_Base
// (Parent = the New Material Instance factory's InitialParent) with scalar, vector, texture, static
// switch and component mask overrides and a Details property; Extract/Diff agree; the extraction
// lists the overrides only and no raw parameter arrays; re-applying it is a no-op (no save, package
// file untouched); Inspect lists every exposed parameter with group, default and override state;
// an edit changes, adds and clears overrides (static switch off) and sets the Refraction Depth Bias; unknown parameter, wrong kind,
// missing texture, wrong texture type, read-only layer stack and Body-owned property fail at the
// document path and change nothing; a failure after the edit (parent changed, then a bad
// parameter / bad property) rolls back; a parent change keeps the overrides the new parent has
// (same name and type) and drops the others; a parent cycle is refused; an instance of an instance
// sees the parent instance's values as defaults; the saved package reloads to the same document;
// failed creates leave nothing. Success messages name the profile; extract diff_only omits Details values
// at their class default and never lists ThumbnailInfo (editor state, refused when authored); a
// document without Body gets Meta.BodyRequired at /Body.
// UEAssetDoc.MaterialInstance.OverriddenDefaults: a Material apply (and its
// validate) that changes a parameter default an existing instance overrides adds an Info diagnostic naming that
// instance (it keeps its own value); an instance without the override is not named, and a default no instance
// overrides adds none.
// UEAssetDoc.MaterialInstance.SparseProperties: extract and Inspect list a Details struct
// (BasePropertyOverrides) with only its changed members (Inspect Properties held ~2.5 KB of defaults); the
// extract applied as a copy gives the same instance (Diff empty).
// UEAssetDoc.MaterialInstance.Undo: a human override edit + save, then an AI apply;
// Ctrl+Z afterwards must not corrupt the instance (AssetDocumentUndoHistory.h); a failed apply keeps
// the human's history.

#include "AssetDocumentDataTableTestSupport.h"

#include "AssetCompilingManager.h"
#include "FileHelpers.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionStaticComponentMaskParameter.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Engine/Texture.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/ScopeExit.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Runtime/AssetDocumentUndoHistory.h"
#include "ScopedTransaction.h"
#include "Utils/AssetDocumentJsonKeys.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentMaterialInstanceTests
{
using namespace AssetDocumentDataTableTests;

const TCHAR* InstanceClass = TEXT("/Script/Engine.MaterialInstanceConstant");
const TCHAR* DefaultTexture = TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture");
const TCHAR* WhiteTexture = TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture");
const TCHAR* CubeTexture = TEXT("/Engine/EngineResources/DefaultTextureCube.DefaultTextureCube");
const TCHAR* MetalPhysMaterial = TEXT("/Engine/EngineMaterials/PhysMat_Metal.PhysMat_Metal");

// Shader compiles started by the test (materials, static permutations) finish before the fixtures go.
void FinishCompilation()
{
	FAssetCompilingManager::Get().FinishAllCompilation();
}

template <typename T>
T* AddExpression(UMaterial& Material, const TCHAR* Name, int32 X)
{
	T* Expression = Cast<T>(UMaterialEditingLibrary::CreateMaterialExpression(&Material, T::StaticClass(), X, 0));
	Expression->ParameterName = Name;
	return Expression;
}

void Connect(FExpressionInput& Input, UMaterialExpression* From, int32 OutputIndex = 0)
{
	Input.Expression = From;
	Input.OutputIndex = OutputIndex;
}

// M_Base: Roughness (scalar, group Surface), Metallic (scalar), BaseColor (vector), Albedo (texture),
// UseDetail (static switch: Albedo or BaseColor), Channels (static component mask of BaseColor).
// M_Other: Roughness (scalar), Albedo (a scalar here), Tint (vector).
UMaterial* CreateParentMaterial(FAutomationTestBase& Test, const FTarget& Target, bool bOther)
{
	UPackage* Package = CreatePackage(*Target.PackageName);
	UMaterial* Material = NewObject<UMaterial>(Package, FName(*Target.AssetName), RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Material);
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
	UMaterialExpressionScalarParameter* Roughness = AddExpression<UMaterialExpressionScalarParameter>(*Material, TEXT("Roughness"), -400);
	Roughness->DefaultValue = 0.5f;
	Roughness->Group = TEXT("Surface");
	Connect(Inputs->Roughness, Roughness);
	if (bOther)
	{
		UMaterialExpressionScalarParameter* Albedo = AddExpression<UMaterialExpressionScalarParameter>(*Material, TEXT("Albedo"), -400);
		Albedo->DefaultValue = 0.25f;
		Connect(Inputs->Metallic, Albedo);
		UMaterialExpressionVectorParameter* Tint = AddExpression<UMaterialExpressionVectorParameter>(*Material, TEXT("Tint"), -400);
		Tint->DefaultValue = FLinearColor(1, 1, 1, 1);
		Connect(Inputs->BaseColor, Tint);
	}
	else
	{
		UMaterialExpressionScalarParameter* Metallic = AddExpression<UMaterialExpressionScalarParameter>(*Material, TEXT("Metallic"), -400);
		Metallic->DefaultValue = 0.f;
		Connect(Inputs->Metallic, Metallic);
		UMaterialExpressionVectorParameter* Color = AddExpression<UMaterialExpressionVectorParameter>(*Material, TEXT("BaseColor"), -800);
		Color->DefaultValue = FLinearColor(0.5f, 0.5f, 0.5f, 1.f);
		UMaterialExpressionTextureSampleParameter2D* Albedo = AddExpression<UMaterialExpressionTextureSampleParameter2D>(*Material, TEXT("Albedo"), -800);
		Albedo->Texture = LoadObject<UTexture>(nullptr, DefaultTexture);
		UMaterialExpressionStaticSwitchParameter* UseDetail = AddExpression<UMaterialExpressionStaticSwitchParameter>(*Material, TEXT("UseDetail"), -400);
		UseDetail->DefaultValue = false;
		Connect(UseDetail->A, Albedo);
		Connect(UseDetail->B, Color);
		Connect(Inputs->BaseColor, UseDetail);
		UMaterialExpressionStaticComponentMaskParameter* Channels = AddExpression<UMaterialExpressionStaticComponentMaskParameter>(*Material, TEXT("Channels"), -400);
		Channels->DefaultR = true;
		Connect(Channels->Input, Color);
		Connect(Inputs->Specular, Channels);
	}
	UMaterialEditingLibrary::RecompileMaterial(Material);
	Test.TestTrue(*FString::Printf(TEXT("%s saved"), *Target.AssetName), UEditorLoadingAndSavingUtils::SavePackages({Package}, false));
	ReleaseUndoHistory();
	return Material;
}

FString InstanceDocument(const FTarget& Target, const TCHAR* Action, const FString& Body, const FString& Properties = TEXT("{}"))
{
	return Document(Target, InstanceClass, Action, Body, Properties);
}

float Scalar(const UMaterialInstanceConstant* Instance, const TCHAR* Name)
{
	float Value = -1.f;
	return Instance != nullptr && Instance->GetScalarParameterValue(FHashedMaterialParameterInfo(Name), Value) ? Value : -1.f;
}

bool Switch(const UMaterialInstanceConstant* Instance, const TCHAR* Name, bool& bOutOverridden)
{
	bool bValue = false;
	FGuid Guid;
	bOutOverridden = false;
	if (Instance != nullptr)
	{
		Instance->GetStaticSwitchParameterValue(FHashedMaterialParameterInfo(Name), bValue, Guid);
		// GetStaticParameters() returns a copy: keep it in a named local. Iterating the member of the
		// temporary (inherited from FStaticParameterSetRuntimeData) relies on lifetime extension through
		// a base-class member, which MSVC does not apply: the loop read a destroyed array on Windows.
		const FStaticParameterSet StaticParameters = Instance->GetStaticParameters();
		for (const FStaticSwitchParameter& Parameter : StaticParameters.StaticSwitchParameters)
		{
			bOutOverridden |= Parameter.ParameterInfo.Name == Name && Parameter.bOverride;
		}
	}
	return bValue;
}

TSharedPtr<FJsonObject> BodyObject(const TSharedPtr<FJsonObject>& Document, const TCHAR* Field)
{
	const TSharedPtr<FJsonObject>* Body = nullptr;
	const TSharedPtr<FJsonObject>* Value = nullptr;
	return Document.IsValid() && Document->TryGetObjectField(TEXT("Body"), Body) && (*Body)->TryGetObjectField(Field, Value)
		? *Value : nullptr;
}

TArray<FString> Keys(const TSharedPtr<FJsonObject>& Object)
{
	TArray<FString> Out;
	if (Object.IsValid())
	{
		AssetDocumentServerJson::GetKeys(*Object, Out);
		Out.Sort();
	}
	return Out;
}

// The Inspect ParameterSchema entry of a parameter ({"Name": ...}).
FString SchemaEntry(const FString& Inspected, const TCHAR* Name)
{
	const int32 Start = Inspected.Find(FString::Printf(TEXT("{\"Name\":\"%s\""), Name));
	const int32 End = Start == INDEX_NONE ? INDEX_NONE : Inspected.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
	return End == INDEX_NONE ? FString() : Inspected.Mid(Start, End - Start);
}

// A number field of a ParameterSchema entry (the entry text without its closing brace).
double SchemaNumber(const FString& Entry, const TCHAR* Field)
{
	const TSharedPtr<FJsonObject> Object = Entry.IsEmpty() ? nullptr : Parse(Entry + TEXT("}"));
	double Number = -1.0;
	return Object.IsValid() && Object->TryGetNumberField(Field, Number) ? Number : -1.0;
}
} // namespace AssetDocumentMaterialInstanceTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMaterialInstancePythonTest, "UEAssetDoc.MaterialInstance.Python",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMaterialInstancePythonTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMaterialInstanceTests;
	const FTarget Base(TEXT("M_Base"));
	const FTarget Other(TEXT("M_Other"));
	const FTarget Target(TEXT("MI_Rock"));
	const FTarget Child(TEXT("MI_Child"));
	const FTarget NoParent(TEXT("MI_NoParent"));
	const FTarget MissingParent(TEXT("MI_MissingParent"));
	const FTarget BadCreate(TEXT("MI_BadCreate"));
	ON_SCOPE_EXIT
	{
		FinishCompilation();
		for (const FTarget* Fixture : {&Child, &Target, &NoParent, &MissingParent, &BadCreate, &Other, &Base})
		{
			Fixture->Cleanup();
		}
	};
	UMaterial* BaseMaterial = CreateParentMaterial(*this, Base, false);
	CreateParentMaterial(*this, Other, true);

	// 1. One apply creates the instance with its parent and overrides.
	const FString CreateBody = FString::Printf(TEXT(R"json("Parent":"%s","Parameters":{"Roughness":0.4,"BaseColor":[1,0,0,1],
		"Albedo":"%s","UseDetail":true,"Channels":{"R":false,"G":true,"B":false,"A":false}})json"), *Base.ObjectPath, WhiteTexture);
	const FString Created = InstanceDocument(Target, TEXT("Create"), CreateBody, FString::Printf(TEXT("{\"PhysMaterial\":\"%s\"}"), MetalPhysMaterial));
	const double CreateStart = FPlatformTime::Seconds();
	const FAssetDocumentResult CreateResult = Apply(*this, Target, Created, TEXT("Create"));
	if (!TestTrue(TEXT("create applies"), CreateResult.IsSuccess()))
	{
		return false;
	}
	TestEqual(TEXT("the success message names the profile"), CreateResult.Message, FString(TEXT("MaterialInstanceConstant document applied")));
	AddInfo(FString::Printf(TEXT("create with a static switch override took %.2f s (shaders compile asynchronously)"), FPlatformTime::Seconds() - CreateStart));
	UMaterialInstanceConstant* Instance = Target.Get<UMaterialInstanceConstant>();
	if (!TestNotNull(TEXT("instance exists"), Instance))
	{
		return false;
	}
	TestTrue(TEXT("parent is the factory's InitialParent"), Instance->Parent == BaseMaterial);
	TestEqual(TEXT("scalar override"), Scalar(Instance, TEXT("Roughness")), 0.4f);
	TestEqual(TEXT("unset scalar keeps the parent value"), Scalar(Instance, TEXT("Metallic")), 0.f);
	FLinearColor Color;
	TestTrue(TEXT("vector override"), Instance->GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("BaseColor")), Color) && Color == FLinearColor(1, 0, 0, 1));
	UTexture* Texture = nullptr;
	TestTrue(TEXT("texture override"), Instance->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Albedo")), Texture)
		&& Texture != nullptr && Texture->GetPathName() == WhiteTexture);
	bool bOverridden = false;
	TestTrue(TEXT("static switch override"), Switch(Instance, TEXT("UseDetail"), bOverridden) && bOverridden);
	TestTrue(TEXT("static parameters build a permutation"), Instance->bHasStaticPermutationResource != 0);
	TestTrue(TEXT("Details property"), Instance->PhysMaterial != nullptr && Instance->PhysMaterial->GetPathName() == MetalPhysMaterial);
	TestNoDiff(*this, Created, TEXT("after create"));

	// 2. The extraction lists the overrides only, without raw parameter arrays; re-applying it is a no-op.
	FString Baseline = TestNoOpReapply(*this, Target, TEXT("Reapply"));
	{
		const TSharedPtr<FJsonObject> Extracted = Parse(Baseline);
		TestEqual(TEXT("overridden parameters only"), FString::Join(Keys(BodyObject(Extracted, TEXT("Parameters"))), TEXT(",")),
			FString(TEXT("Albedo,BaseColor,Channels,Roughness,UseDetail")));
		TestFalse(TEXT("no raw parameter arrays"), Baseline.Contains(TEXT("ScalarParameterValues")) || Baseline.Contains(TEXT("TextureParameterValues")));
		TestTrue(TEXT("scalar extracts as written"), Baseline.Contains(TEXT("\"Roughness\":0.4")));
		// diff_only (the default) lists only the Details values that differ from the class default;
		// the thumbnail (editor state) is never extracted.
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		TestTrue(TEXT("diff_only extract has Properties"), Extracted.IsValid() && Extracted->TryGetObjectField(TEXT("Properties"), Properties));
		const TArray<FString> DiffOnlyNames = Properties ? Keys(*Properties) : TArray<FString>();
		TestTrue(*FString::Printf(TEXT("diff_only Properties hold the authored PhysMaterial and few others (%s)"), *FString::Join(DiffOnlyNames, TEXT(","))),
			DiffOnlyNames.Contains(TEXT("PhysMaterial")) && DiffOnlyNames.Num() <= 5);
		FAssetDocumentExtractRequest AllRequest;
		AllRequest.AssetPath = Target.ObjectPath;
		AllRequest.bDiffOnly = false;
		const FAssetDocumentResult All = FAssetDocumentModule::Get().GetService().Extract(AllRequest);
		const TSharedPtr<FJsonObject>* AllProperties = nullptr;
		if (TestTrue(TEXT("diff_only=false extract succeeds"), All.IsSuccess() && All.Payload.IsValid()
			&& All.Payload->TryGetObjectField(TEXT("Properties"), AllProperties)))
		{
			const TArray<FString> AllNames = Keys(*AllProperties);
			TestTrue(*FString::Printf(TEXT("diff_only=false lists every writable Details value (%d)"), AllNames.Num()), AllNames.Num() >= DiffOnlyNames.Num() + 10);
			TestTrue(*FString::Printf(TEXT("diff_only=false lists every diff_only value too (%s)"), *FString::Join(AllNames, TEXT(","))),
				DiffOnlyNames.FilterByPredicate([&AllNames](const FString& Name) { return !AllNames.Contains(Name); }).IsEmpty());
			TestFalse(TEXT("ThumbnailInfo is not extracted"), AllNames.Contains(TEXT("ThumbnailInfo")));
		}
	}
	TestFailsWith(*this, Apply(*this, Target, InstanceDocument(Target, TEXT("Update"), TEXT("\"Parameters\":{}"), TEXT("{\"ThumbnailInfo\":null}")),
		TEXT("Thumbnail")), TEXT("MaterialInstance.NotAuthoredProperty"), TEXT("/Properties/ThumbnailInfo"), TEXT("thumbnail is editor state"));
	// An exact-profile document without Body gets a coded diagnostic at /Body (Validate and Apply).
	{
		const FString NoBody = FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"%s\",\"Action\":\"Update\",\"Properties\":{}}"),
			*Target.PackageName, InstanceClass);
		TestFailsWith(*this, ValidateDocument(*this, NoBody, TEXT("NoBody validate")), TEXT("Meta.BodyRequired"), TEXT("/Body"), TEXT("Body-less validate"));
		TestFailsWith(*this, Apply(*this, Target, NoBody, TEXT("NoBody")), TEXT("Meta.BodyRequired"), TEXT("/Body"), TEXT("Body-less apply"));
	}

	// 3. Inspect lists every parameter the parent exposes.
	const FString Inspected = Inspect(*this, Target, TEXT("Inspect"));
	{
		const FString Metallic = SchemaEntry(Inspected, TEXT("Metallic"));
		const FString Roughness = SchemaEntry(Inspected, TEXT("Roughness"));
		const FString UseDetail = SchemaEntry(Inspected, TEXT("UseDetail"));
		TestTrue(*FString::Printf(TEXT("Metallic is listed, not overridden (%s)"), *Metallic), Metallic.Contains(TEXT("\"Kind\":\"scalar\""))
			&& Metallic.Contains(TEXT("\"Overridden\":false")) && Metallic.Contains(TEXT("\"Default\":0")));
		TestTrue(*FString::Printf(TEXT("Roughness has group, default and value (%s)"), *Roughness), Roughness.Contains(TEXT("\"Group\":\"Surface\""))
			&& Roughness.Contains(TEXT("\"Default\":0.5")) && Roughness.Contains(TEXT("\"Value\":0.4")) && Roughness.Contains(TEXT("\"Path\":\"/Body/Parameters/Roughness\"")));
		TestTrue(*FString::Printf(TEXT("UseDetail is a static switch (%s)"), *UseDetail), UseDetail.Contains(TEXT("\"Kind\":\"static_switch\"")));
		TestTrue(TEXT("Inspect names the base material"), Inspected.Contains(FString::Printf(TEXT("\"BaseMaterial\":\"%s\""), *Base.ObjectPath)));
	}

	// 4. An edit changes, adds and clears overrides; the static switch goes off.
	const FString Edited = InstanceDocument(Target, TEXT("Update"), TEXT(
		R"json("Parameters":{"Roughness":0.6,"Metallic":1,"BaseColor":null,"UseDetail":false},"RefractionDepthBias":2.5)json"));
	const double EditStart = FPlatformTime::Seconds();
	if (TestTrue(TEXT("edit applies"), Apply(*this, Target, Edited, TEXT("Edit")).IsSuccess()))
	{
		AddInfo(FString::Printf(TEXT("static switch edit took %.2f s"), FPlatformTime::Seconds() - EditStart));
		TestEqual(TEXT("scalar changed"), Scalar(Instance, TEXT("Roughness")), 0.6f);
		TestEqual(TEXT("scalar added"), Scalar(Instance, TEXT("Metallic")), 1.f);
		TestTrue(TEXT("vector cleared: parent value"), Instance->GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("BaseColor")), Color)
			&& Color == FLinearColor(0.5f, 0.5f, 0.5f, 1.f));
		TestFalse(TEXT("static switch off"), Switch(Instance, TEXT("UseDetail"), bOverridden));
		TestTrue(TEXT("static switch still overridden"), bOverridden);
		float Bias = 0.f;
		TestTrue(TEXT("Refraction Depth Bias set"), Instance->GetRefractionSettings(Bias) && Bias == 2.5f);
		TestTrue(TEXT("untouched texture override kept"), Instance->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Albedo")), Texture)
			&& Texture != nullptr && Texture->GetPathName() == WhiteTexture);
		TestNoDiff(*this, Edited, TEXT("after edit"));
	}
	Baseline = TestNoOpReapply(*this, Target, TEXT("Reapply edited"));

	// 5. Errors name the document location and change nothing.
	const FAssetDocumentResult Unknown = Apply(*this, Target, InstanceDocument(Target, TEXT("Update"), TEXT(R"json("Parameters":{"Rougness":0.1})json")), TEXT("Unknown"));
	TestFailsWith(*this, Unknown, TEXT("MaterialInstance.UnknownParameter"), TEXT("/Body/Parameters/Rougness"), TEXT("unknown parameter"));
	TestTrue(TEXT("unknown parameter suggests the close name"), Unknown.Message.Contains(TEXT("Did you mean: Roughness?")));
	TestTrue(*FString::Printf(TEXT("unknown parameter lists the parent's parameters (%s)"), *Unknown.Message),
		Unknown.Message.Contains(TEXT("UseDetail (static_switch)")) && Unknown.Message.Contains(TEXT("BaseColor (vector)")));
	TestFailsWith(*this, Apply(*this, Target, InstanceDocument(Target, TEXT("Update"), TEXT(R"json("Parameters":{"Roughness":"rough"})json")), TEXT("WrongKind")),
		TEXT("MaterialInstance.InvalidValue"), TEXT("/Body/Parameters/Roughness"), TEXT("wrong kind"));
	TestFailsWith(*this, Apply(*this, Target, InstanceDocument(Target, TEXT("Update"), TEXT(R"json("Parameters":{"UseDetail":1})json")), TEXT("WrongSwitch")),
		TEXT("MaterialInstance.InvalidValue"), TEXT("/Body/Parameters/UseDetail"), TEXT("switch takes a bool"));
	TestFailsWith(*this, Apply(*this, Target, InstanceDocument(Target, TEXT("Update"),
		TEXT(R"json("Parameters":{"Albedo":"/Game/AssetDocumentDataTablePython/T_Missing.T_Missing"})json")), TEXT("MissingTexture")),
		TEXT("MaterialInstance.AssetNotFound"), TEXT("/Body/Parameters/Albedo"), TEXT("missing texture"));
	TestFailsWith(*this, Apply(*this, Target, InstanceDocument(Target, TEXT("Update"),
		FString::Printf(TEXT(R"json("Parameters":{"Albedo":"%s"})json"), CubeTexture)), TEXT("CubeTexture")),
		TEXT("MaterialInstance.InvalidValue"), TEXT("/Body/Parameters/Albedo"), TEXT("texture of another sampler type"));
	TestFailsWith(*this, Apply(*this, Target, InstanceDocument(Target, TEXT("Update"), TEXT("\"Parameters\":{}"),
		TEXT("{\"ScalarParameterValues\":[]}")), TEXT("RawArray")),
		TEXT("MaterialInstance.BodyOwnedProperty"), TEXT("/Properties/ScalarParameterValues"), TEXT("raw parameter array"));
	TestFailsWith(*this, Apply(*this, Target, InstanceDocument(Target, TEXT("Update"), TEXT("\"MaterialLayers\":{\"Layers\":[]}")), TEXT("Layers")),
		TEXT("MaterialInstance.MaterialLayersReadOnly"), TEXT("/Body/MaterialLayers"), TEXT("layer stack read-only"));
	ExtractEquals(*this, Target, Baseline, TEXT("after refused edits"));

	// 6. Failures after the edit started roll back whole (parent changed, then a bad parameter;
	//    parameters changed, then a bad Details property).
	const FAssetDocumentResult MidParent = Apply(*this, Target, InstanceDocument(Target, TEXT("Update"), FString::Printf(
		TEXT(R"json("Parent":"%s","Parameters":{"Tint":[0,1,0,1],"Nope":1})json"), *Other.ObjectPath)), TEXT("MidParent"));
	TestFailsWith(*this, MidParent, TEXT("MaterialInstance.UnknownParameter"), TEXT("/Body/Parameters/Nope"), TEXT("bad parameter after parent change"));
	Instance = Target.Get<UMaterialInstanceConstant>();
	TestTrue(TEXT("rolled back: parent restored"), Instance != nullptr && Instance->Parent == BaseMaterial);
	TestTrue(TEXT("rolled back: package clean"), Instance != nullptr && !Instance->GetOutermost()->IsDirty());
	TestTrue(TEXT("rolled back: static switch still overridden"), Instance != nullptr && !Switch(Instance, TEXT("UseDetail"), bOverridden) && bOverridden);
	ExtractEquals(*this, Target, Baseline, TEXT("after parent rollback"));
	const FAssetDocumentResult MidProperty = Apply(*this, Target, InstanceDocument(Target, TEXT("Update"),
		TEXT(R"json("Parameters":{"Roughness":0.9,"UseDetail":true})json"), TEXT("{\"PhysMaterial\":\"/Game/AssetDocumentDataTablePython/Nope.Nope\"}")), TEXT("MidProperty"));
	TestFalse(TEXT("bad property after parameter edit fails"), MidProperty.IsSuccess());
	Instance = Target.Get<UMaterialInstanceConstant>();
	TestEqual(TEXT("rolled back: scalar restored"), Scalar(Instance, TEXT("Roughness")), 0.6f);
	ExtractEquals(*this, Target, Baseline, TEXT("after property rollback"));

	// 7. Parent change: overrides the new parent has with the same type are kept (Roughness), the
	//    others dropped (Albedo is a scalar there; Metallic, UseDetail, Channels do not exist).
	const FString Reparented = InstanceDocument(Target, TEXT("Update"), FString::Printf(
		TEXT(R"json("Parent":"%s","Parameters":{"Tint":[0,1,0,1]})json"), *Other.ObjectPath));
	if (TestTrue(TEXT("parent change applies"), Apply(*this, Target, Reparented, TEXT("Reparent")).IsSuccess()))
	{
		const TSharedPtr<FJsonObject> Extracted = Extract(*this, Target, TEXT("after reparent"));
		TestEqual(TEXT("kept and new overrides"), FString::Join(Keys(BodyObject(Extracted, TEXT("Parameters"))), TEXT(",")),
			FString(TEXT("Roughness,Tint")));
		Instance = Target.Get<UMaterialInstanceConstant>();
		TestEqual(TEXT("kept override value"), Scalar(Instance, TEXT("Roughness")), 0.6f);
		float Bias = 0.f;
		TestTrue(TEXT("Refraction Depth Bias survives the parent change"), Instance != nullptr && Instance->GetRefractionSettings(Bias) && Bias == 2.5f);
		TestTrue(TEXT("no static parameters left"), Instance != nullptr && Instance->GetStaticParameters().StaticSwitchParameters.IsEmpty());
		TestNoDiff(*this, Reparented, TEXT("after reparent"));
	}
	Baseline = TestNoOpReapply(*this, Target, TEXT("Reapply reparented"));

	// 8. The saved package, unloaded and loaded back from disk, extracts to the same document.
	//    (Before any child instance exists: the fresh-verification copy of a saved child stays loaded
	//    and references its parent.)
	FinishCompilation();
	TestReload(*this, Target, Baseline);

	// 9. An instance of this instance sees its values as defaults; making it this instance's parent loops.
	if (TestTrue(TEXT("child instance applies"), Apply(*this, Child, InstanceDocument(Child, TEXT("Create"),
		FString::Printf(TEXT("\"Parent\":\"%s\",\"Parameters\":{\"Tint\":[0,0,1,1]}"), *Target.ObjectPath)), TEXT("Child")).IsSuccess()))
	{
		const FString ChildInspected = Inspect(*this, Child, TEXT("Inspect child"));
		const FString Roughness = SchemaEntry(ChildInspected, TEXT("Roughness"));
		TestEqual(*FString::Printf(TEXT("child default is the parent instance's override (%s)"), *Roughness), SchemaNumber(Roughness, TEXT("Default")), 0.6, 1e-6);
		TestTrue(TEXT("child does not override Roughness"), Roughness.Contains(TEXT("\"Overridden\":false")));
		TestFailsWith(*this, Apply(*this, Target, InstanceDocument(Target, TEXT("Update"), FString::Printf(TEXT("\"Parent\":\"%s\""), *Child.ObjectPath)),
			TEXT("Cycle")), TEXT("MaterialInstance.ParentCycle"), TEXT("/Body/Parent"), TEXT("parent cycle"));
		ExtractEquals(*this, Target, Baseline, TEXT("after parent cycle"));
	}

	// 10. A parentless create (the New menu without a parent) works; failed creates leave nothing.
	if (TestTrue(TEXT("parentless create applies"), Apply(*this, NoParent, InstanceDocument(NoParent, TEXT("Create"), TEXT("\"Parameters\":{}")),
		TEXT("NoParent")).IsSuccess()))
	{
		TestTrue(TEXT("no parent"), NoParent.Get<UMaterialInstanceConstant>() != nullptr && NoParent.Get<UMaterialInstanceConstant>()->Parent == nullptr);
	}
	// The generic asset resolver logs the engine's load warnings for the missing package.
	AddExpectedError(TEXT("/Game/AssetDocumentDataTablePython/M_Nope"), EAutomationExpectedErrorFlags::Contains, 0);
	const FAssetDocumentResult MissingParentResult = Apply(*this, MissingParent, InstanceDocument(MissingParent, TEXT("Create"),
		TEXT("\"Parent\":\"/Game/AssetDocumentDataTablePython/M_Nope.M_Nope\"")), TEXT("MissingParent"));
	TestFailsWith(*this, MissingParentResult, TEXT("MaterialInstance.ParentNotFound"), TEXT("/Body/Parent"), TEXT("missing parent"));
	TestNothingCreated(*this, MissingParent, TEXT("missing parent"));
	// The failed create keeps the author's sidecar and says so.
	bool bSidecarKept = false;
	TestTrue(TEXT("failed create reports the kept sidecar"), MissingParentResult.Payload.IsValid()
		&& MissingParentResult.Payload->TryGetBoolField(TEXT("sidecar_kept"), bSidecarKept) && bSidecarKept);
	TestTrue(TEXT("the sidecar file is still there"), IFileManager::Get().FileExists(*MissingParent.SidecarPath));
	TestFailsWith(*this, Apply(*this, BadCreate, InstanceDocument(BadCreate, TEXT("Create"), FString::Printf(
		TEXT(R"json("Parent":"%s","Parameters":{"Roughness":0.2,"Nope":true})json"), *Base.ObjectPath)), TEXT("BadCreate")),
		TEXT("MaterialInstance.UnknownParameter"), TEXT("/Body/Parameters/Nope"), TEXT("create with an unknown parameter"));
	TestNothingCreated(*this, BadCreate, TEXT("create with an unknown parameter"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMaterialOverriddenDefaultsTest, "UEAssetDoc.MaterialInstance.OverriddenDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMaterialOverriddenDefaultsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMaterialInstanceTests;
	const FTarget Base(TEXT("M_Defaults"));
	const FTarget Rock(TEXT("MI_RockOverride"));
	const FTarget Plain(TEXT("MI_PlainOverride"));
	ON_SCOPE_EXIT
	{
		FinishCompilation();
		for (const FTarget* Fixture : {&Plain, &Rock, &Base})
		{
			Fixture->Cleanup();
		}
	};
	CreateParentMaterial(*this, Base, false);
	if (!TestTrue(TEXT("MI_Rock overrides Roughness"), Apply(*this, Rock, InstanceDocument(Rock, TEXT("Create"),
			FString::Printf(TEXT(R"json("Parent":"%s","Parameters":{"Roughness":0.9})json"), *Base.ObjectPath)), TEXT("MI_Rock")).IsSuccess())
		|| !TestTrue(TEXT("MI_Plain overrides BaseColor"), Apply(*this, Plain, InstanceDocument(Plain, TEXT("Create"),
			FString::Printf(TEXT(R"json("Parent":"%s","Parameters":{"BaseColor":[0,1,0,1]})json"), *Base.ObjectPath)), TEXT("MI_Plain")).IsSuccess()))
	{
		return false;
	}
	// The material document with new defaults of the named scalar parameters.
	const auto WithDefaults = [&](const TMap<FString, double>& Defaults)
		{
			const TSharedPtr<FJsonObject> Document = Extract(*this, Base, TEXT("M_Defaults"));
			const TSharedPtr<FJsonObject>* Body = nullptr;
			const TSharedPtr<FJsonObject>* Expressions = nullptr;
			if (!Document.IsValid() || !Document->TryGetObjectField(TEXT("Body"), Body) || !(*Body)->TryGetObjectField(TEXT("Expressions"), Expressions))
			{
				return FString();
			}
			Document->SetStringField(TEXT("Action"), TEXT("Update"));
			for (const auto& Entry : (*Expressions)->Values)
			{
				const TSharedPtr<FJsonObject>* Properties = nullptr;
				FString Name;
				if (Entry.Value->AsObject()->TryGetObjectField(TEXT("Properties"), Properties) && (*Properties)->TryGetStringField(TEXT("ParameterName"), Name)
					&& Defaults.Contains(Name))
				{
					(*Properties)->SetNumberField(TEXT("DefaultValue"), Defaults[Name]);
				}
			}
			return Serialize(Document);
		};
	const auto FindInfo = [](const FAssetDocumentResult& Result) -> const FAssetDocumentDiagnostic*
		{
			return Result.Diagnostics.FindByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
				{
					return Diagnostic.Code == TEXT("Material.DefaultOverriddenByInstances");
				});
		};

	// 1. Roughness (overridden by MI_Rock) and Metallic (overridden by none) change: validate and apply name MI_Rock only.
	const FString Changed = WithDefaults({{TEXT("Roughness"), 0.3}, {TEXT("Metallic"), 0.6}});
	for (const bool bValidate : {true, false})
	{
		const TCHAR* Stage = bValidate ? TEXT("validate") : TEXT("apply");
		const FAssetDocumentResult Result = bValidate ? ValidateDocument(*this, Changed, Stage) : Apply(*this, Base, Changed, Stage);
		TestTrue(*FString::Printf(TEXT("%s succeeds"), Stage), Result.IsSuccess());
		const FAssetDocumentDiagnostic* Info = FindInfo(Result);
		if (TestNotNull(*FString::Printf(TEXT("%s: the overriding instance is reported"), Stage), Info))
		{
			TestTrue(*FString::Printf(TEXT("%s: Info severity"), Stage), Info->Severity == TEXT("info"));
			TestTrue(*FString::Printf(TEXT("%s: names MI_Rock and Roughness"), Stage),
				Info->Message.Contains(Rock.AssetName) && Info->Message.Contains(TEXT("(Roughness)")) && Info->Message.StartsWith(TEXT("1 material instance")));
			TestFalse(*FString::Printf(TEXT("%s: MI_Plain does not override them"), Stage), Info->Message.Contains(Plain.AssetName));
		}
	}
	UMaterial* Material = Base.Get<UMaterial>();
	float Metallic = -1.f;
	TestTrue(TEXT("the new default is applied"), Material != nullptr && Material->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("Metallic")), Metallic)
		&& FMath::IsNearlyEqual(Metallic, 0.6f));

	// 2. Only Metallic changes: no instance overrides it, no Info.
	const FAssetDocumentResult Unreported = Apply(*this, Base, WithDefaults({{TEXT("Metallic"), 0.1}}), TEXT("Metallic only"));
	TestTrue(TEXT("Metallic only: applies"), Unreported.IsSuccess());
	TestNull(TEXT("Metallic only: nothing overrides it"), FindInfo(Unreported));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMaterialInstanceSparseTest, "UEAssetDoc.MaterialInstance.SparseProperties",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMaterialInstanceSparseTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMaterialInstanceTests;
	const FTarget Base(TEXT("M_SparseBase"));
	const FTarget Target(TEXT("MI_Sparse"));
	const FTarget Copy(TEXT("MI_SparseCopy"));
	ON_SCOPE_EXIT
	{
		FinishCompilation();
		for (const FTarget* Fixture : {&Copy, &Target, &Base})
		{
			Fixture->Cleanup();
		}
	};
	CreateParentMaterial(*this, Base, false);
	const FString Body = FString::Printf(TEXT(R"json("Parent":"%s","Parameters":{"Roughness":0.4,"BaseColor":[0,1,0,1]})json"), *Base.ObjectPath);
	const FString Created = InstanceDocument(Target, TEXT("Create"), Body,
		TEXT(R"json({"BasePropertyOverrides":{"bOverride_TwoSided":true,"TwoSided":true}})json"));
	if (!TestTrue(TEXT("create applies"), Apply(*this, Target, Created, TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	auto PropertiesOf = [](const TSharedPtr<FJsonObject>& Document) -> TSharedPtr<FJsonObject>
	{
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		return Document.IsValid() && Document->TryGetObjectField(TEXT("Properties"), Properties) ? *Properties : nullptr;
	};
	// Inspect used to read every Details value (the diff_only false extract's Properties).
	FAssetDocumentExtractRequest AllRequest;
	AllRequest.AssetPath = Target.ObjectPath;
	AllRequest.bDiffOnly = false;
	const FAssetDocumentResult All = FAssetDocumentModule::Get().GetService().Extract(AllRequest);
	const FString AllProperties = Serialize(PropertiesOf(All.Payload));
	const TSharedPtr<FJsonObject> Inspected = Parse(Inspect(*this, Target, TEXT("Inspect")));
	const FString InspectedProperties = Serialize(PropertiesOf(Inspected));
	const TSharedPtr<FJsonObject> Extracted = Extract(*this, Target, TEXT("Extract"));
	const FString ExtractedProperties = Serialize(PropertiesOf(Extracted));
	AddInfo(FString::Printf(TEXT("MI Properties: every value %d chars, Inspect %d chars, extract %d chars: %s"), AllProperties.Len(),
		InspectedProperties.Len(), ExtractedProperties.Len(), *ExtractedProperties));
	const TSharedPtr<FJsonObject>* OverridesField = nullptr;
	const TSharedPtr<FJsonObject> Overrides = PropertiesOf(Extracted).IsValid()
		&& PropertiesOf(Extracted)->TryGetObjectField(TEXT("BasePropertyOverrides"), OverridesField) ? *OverridesField : nullptr;
	// The changed members: the two authored ones, and OpacityMaskClipValue, which the instance copies from its parent
	// (0.3333, the struct's default being 0.333333); the unchanged override flags are left out. Engines whose
	// overrides hold usage flags copy the parent's into UsageFlags as well.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	const FString ChangedMembers = TEXT("bOverride_TwoSided,OpacityMaskClipValue,TwoSided,UsageFlags");
#else
	const FString ChangedMembers = TEXT("bOverride_TwoSided,OpacityMaskClipValue,TwoSided");
#endif
	TestEqual(TEXT("extract: BasePropertyOverrides lists its changed members only"), FString::Join(Keys(Overrides), TEXT(",")),
		ChangedMembers);
	// Inspect writes numbers at full precision (0.33329999999999999), extract shortest (0.3333): compare the members.
	const TSharedPtr<FJsonObject>* InspectedOverrides = nullptr;
	TestEqual(TEXT("Inspect lists the Properties extract lists"), FString::Join(Keys(PropertiesOf(Inspected)), TEXT(",")),
		FString::Join(Keys(PropertiesOf(Extracted)), TEXT(",")));
	TestEqual(TEXT("Inspect lists the BasePropertyOverrides members extract lists"),
		PropertiesOf(Inspected).IsValid() && PropertiesOf(Inspected)->TryGetObjectField(TEXT("BasePropertyOverrides"), InspectedOverrides)
			? FString::Join(Keys(*InspectedOverrides), TEXT(",")) : FString(),
		FString::Join(Keys(Overrides), TEXT(",")));
	TestTrue(TEXT("Inspect Properties are a small part of every value"), InspectedProperties.Len() * 4 <= AllProperties.Len());

	// Applied as a copy (a new target), the extract gives the same instance.
	if (!Extracted.IsValid())
	{
		return false;
	}
	Extracted->SetStringField(TEXT("Target"), Copy.PackageName);
	Extracted->SetStringField(TEXT("Action"), TEXT("Create"));
	const FString CopyDocument = Serialize(Extracted);
	if (!TestTrue(TEXT("the extract applies as a copy"), Apply(*this, Copy, CopyDocument, TEXT("Copy")).IsSuccess()))
	{
		return false;
	}
	TestNoDiff(*this, CopyDocument, TEXT("copy"));
	const UMaterialInstanceConstant* CopyInstance = Copy.Get<UMaterialInstanceConstant>();
	TestTrue(TEXT("the copy is two-sided, overridden"), CopyInstance != nullptr && CopyInstance->BasePropertyOverrides.bOverride_TwoSided
		&& CopyInstance->BasePropertyOverrides.TwoSided);
	const TSharedPtr<FJsonObject> CopyExtracted = Extract(*this, Copy, TEXT("copy extract"));
	TestEqual(TEXT("the copy extracts the same Properties"), Serialize(PropertiesOf(CopyExtracted)), ExtractedProperties);
	TestNoOpReapply(*this, Target, TEXT("Reapply"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMaterialInstanceUndoTest, "UEAssetDoc.MaterialInstance.Undo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMaterialInstanceUndoTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMaterialInstanceTests;
	const FTarget Base(TEXT("M_UndoBase"));
	const FTarget Target(TEXT("MI_Undo"));
	ON_SCOPE_EXIT
	{
		FinishCompilation();
		Target.Cleanup();
		Base.Cleanup();
	};
	if (GEditor == nullptr || GEditor->Trans == nullptr)
	{
		return false;
	}
	UMaterial* BaseMaterial = CreateParentMaterial(*this, Base, false);
	if (!TestTrue(TEXT("create applies"), Apply(*this, Target, InstanceDocument(Target, TEXT("Create"), FString::Printf(
		TEXT(R"json("Parent":"%s","Parameters":{"Roughness":0.4})json"), *Base.ObjectPath)), TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	ReleaseUndoHistory();
	auto Live = [&Target]() { return Target.Get<UMaterialInstanceConstant>(); };
	// The Material Instance Editor's override edit: PreEditChange (Modify), set, PostEditChange.
	auto HumanEdit = [&Live](const TCHAR* Name, float Value)
	{
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentMaterialInstanceTests", "UserEdit", "Edit Parameter"));
		UMaterialInstanceConstant* Instance = Live();
		Instance->PreEditChange(nullptr);
		Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(Name), Value);
		Instance->PostEditChange();
	};
	auto UndoRedoAll = [this, &Live, BaseMaterial](const TCHAR* Stage)
	{
		int32 Undone = 0;
		while (Undone < 16 && GEditor->Trans->CanUndo() && GEditor->UndoTransaction())
		{
			++Undone;
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			TestTrue(*FString::Printf(TEXT("%s: instance whole after undo %d"), Stage, Undone), Live() != nullptr && Live()->Parent == BaseMaterial);
		}
		int32 Redone = 0;
		while (Redone < Undone && GEditor->Trans->CanRedo() && GEditor->RedoTransaction())
		{
			++Redone;
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		TestEqual(*FString::Printf(TEXT("%s: every undone step redoes"), Stage), Redone, Undone);
	};

	// 1. The human overrides Metallic and saves; the AI then edits: history referencing the instance is cleared.
	HumanEdit(TEXT("Metallic"), 0.7f);
	TestTrue(TEXT("human save"), UEditorLoadingAndSavingUtils::SavePackages({Live()->GetOutermost()}, false));
	TestTrue(TEXT("the human edit is in the undo history"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	if (!TestTrue(TEXT("AI edit applies"), Apply(*this, Target, InstanceDocument(Target, TEXT("Update"),
		TEXT(R"json("Parameters":{"Roughness":0.8,"UseDetail":true})json")), TEXT("Edit")).IsSuccess()))
	{
		return false;
	}
	TestFalse(TEXT("after AI edit: no undo history references the instance"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	TestEqual(TEXT("human override kept (not in the document)"), Scalar(Live(), TEXT("Metallic")), 0.7f);
	const FString AfterEdit = Serialize(Extract(*this, Target, TEXT("after AI edit")));
	UndoRedoAll(TEXT("after AI edit"));
	ExtractEquals(*this, Target, AfterEdit, TEXT("after AI edit + undo/redo"));

	// 2. The human edits again; an AI apply fails and rolls back: the human step stays undoable.
	ReleaseUndoHistory();
	HumanEdit(TEXT("Roughness"), 0.2f);
	const int32 HumanSteps = GEditor->Trans->GetQueueLength();
	const FAssetDocumentResult Failed = Apply(*this, Target, InstanceDocument(Target, TEXT("Update"),
		TEXT(R"json("Parameters":{"Metallic":0.1,"Nope":1})json")), TEXT("Failed"));
	TestFalse(TEXT("failing apply fails"), Failed.IsSuccess());
	TestEqual(TEXT("after rollback: the human history is kept"), GEditor->Trans->GetQueueLength(), HumanSteps);
	TestEqual(TEXT("after rollback: Roughness keeps the human edit"), Scalar(Live(), TEXT("Roughness")), 0.2f);
	TestTrue(TEXT("Ctrl+Z undoes the human edit"), GEditor->UndoTransaction());
	TestEqual(TEXT("after undo: Roughness is back to the AI value"), Scalar(Live(), TEXT("Roughness")), 0.8f);
	ExtractEquals(*this, Target, AfterEdit, TEXT("after rollback + undo"));
	return !HasAnyErrors();
}
#endif
