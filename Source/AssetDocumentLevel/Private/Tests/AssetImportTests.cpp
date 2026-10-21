// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// import_assets (POST /assetdoc/import), the Content Browser's Import / Reimport without UI:
// UEAssetDoc.Import.StaticMesh.ObjAndFbx: OBJ and FBX static meshes with the import dialog's options
//   (collision, combine meshes, uniform scale); facts list bounds and collision; "options" lists the pipelines.
// UEAssetDoc.Import.SkeletalMesh.GltfAndAnimation: a skinned, animated glTF makes a skeletal mesh,
//   skeleton and animation; its animation alone (glTF, and FBX exported from it) imports onto the existing skeleton.
// UEAssetDoc.Import.Texture.Formats: PNG, JPG, TGA and EXR textures; normal map, sRGB and
//   compression options.
// UEAssetDoc.Import.Sound.Wav: a WAV imports as a sound wave through the editor's sound factory.
// UEAssetDoc.Import.Reimport: reimport from the changed source, from a new source, refusals.
// UEAssetDoc.Import.Refusals: non-local, missing and unsupported sources, bad destinations, clashes,
//   unknown options and pipeline settings.

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetImportTestSupport.h"
#include "Misc/EngineVersionComparison.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "PhysicsEngine/BodySetup.h"
#include "Sound/SoundWave.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentImportStaticMeshTest, "UEAssetDoc.Import.StaticMesh.ObjAndFbx",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentImportStaticMeshTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentImportTests;
	FScopedImportTest Scope;
	const FString Cube = Scope.Source(TEXT("Crate.obj"));
	const FString Boxes = Scope.Source(TEXT("Pair.obj"));
	if (!TestTrue(TEXT("OBJ sources written"), WriteObjCube(Cube, 100.0) && WriteObjTwoBoxes(Boxes)))
	{
		return false;
	}

	// "options" describes the import dialog of the file: the stack's pipelines and their settings.
	const FLevelToolResult Options = RunImport(*this, FString::Printf(TEXT(R"({"action": "options", "files": [%s],
		"options": {"uniform_scale": 2.0}})"), *Quote(Cube)), TEXT("options"));
	TestTrue(TEXT("options succeeded"), Options.bSuccess);
	FString Importer;
	TestTrue(TEXT("Interchange imports OBJ"), Options.Payload.IsValid() && Options.Payload->TryGetStringField(TEXT("importer"), Importer)
		&& Importer.StartsWith(TEXT("Interchange")));
	// The mesh pipeline's combine setting: a combine behaviour on newer engines, a flag before.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	const TCHAR* const CombineSetting = TEXT("CombineStaticMeshesBehavior");
	const TCHAR* const CombineAll = TEXT("\"All\"");
#else
	const TCHAR* const CombineSetting = TEXT("bCombineStaticMeshes");
	const TCHAR* const CombineAll = TEXT("true");
#endif
	const TSharedPtr<FJsonObject>* Pipelines = nullptr;
	bool bHasCombine = false;
	double Scale = 0.0;
	if (TestTrue(TEXT("pipelines listed"), Options.Payload.IsValid() && Options.Payload->TryGetObjectField(TEXT("pipelines"), Pipelines)))
	{
		for (const auto& Pipeline : (*Pipelines)->Values)
		{
			const TSharedPtr<FJsonObject>* Mesh = nullptr;
			bHasCombine |= Pipeline.Value->AsObject()->TryGetObjectField(TEXT("MeshPipeline"), Mesh)
				&& (*Mesh)->HasField(CombineSetting);
			Pipeline.Value->AsObject()->TryGetNumberField(TEXT("ImportOffsetUniformScale"), Scale);
		}
	}
	TestTrue(FString::Printf(TEXT("the mesh pipeline's %s is listed"), CombineSetting), bHasCombine);
	TestEqual(TEXT("the listed settings include the options"), Scale, 2.0);

	// No collision, then a box collision.
	const FLevelToolResult Plain = RunImport(*this, FString::Printf(TEXT(R"({"files": [%s], "destination": "%s",
		"options": {"collision": false, "import_materials": false}})"), *Quote(Cube), *Scope.Folder(TEXT("Plain"))), TEXT("plain"));
	const TSharedPtr<FJsonObject> PlainMesh = ResultAsset(Plain, TEXT("StaticMesh"));
	if (!TestTrue(TEXT("plain import"), Plain.bSuccess) || !TestTrue(TEXT("a static mesh"), PlainMesh.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("created"), PlainMesh->GetStringField(TEXT("status")), FString(TEXT("created")));
	TestEqual(TEXT("no collision"), FactNumber(PlainMesh, TEXT("CollisionShapes")), 0.0);
	TestTrue(TEXT("the source is recorded"), FactString(PlainMesh, TEXT("Source")).EndsWith(TEXT("Crate.obj")));
	TestTrue(TEXT("saved"), PayloadStrings(Plain, TEXT("saved_packages")).Num() > 0);
	TestTrue(TEXT("saved to disk"), FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(AssetPath(PlainMesh))));
	const double PlainSize = FactSize(PlainMesh, 0);
	TestTrue(TEXT("a non-empty box"), PlainSize > 0.0);

	const FLevelToolResult Boxed = RunImport(*this, FString::Printf(TEXT(R"({"files": [%s], "destination": "%s", "name": "CrateBoxed",
		"options": {"collision_type": "Box", "import_materials": false}})"), *Quote(Cube), *Scope.Folder(TEXT("Boxed"))), TEXT("boxed"));
	const TSharedPtr<FJsonObject> BoxedMesh = ResultAsset(Boxed, TEXT("StaticMesh"));
	TestTrue(TEXT("boxed import"), Boxed.bSuccess && BoxedMesh.IsValid());
	TestTrue(TEXT("named CrateBoxed"), AssetPath(BoxedMesh).EndsWith(TEXT(".CrateBoxed")));
	TestEqual(TEXT("one box collision"), FactNumber(BoxedMesh, TEXT("CollisionShapes")), 1.0);
	if (UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *AssetPath(BoxedMesh)))
	{
		TestEqual(TEXT("the collision is a box"), Mesh->GetBodySetup() != nullptr ? Mesh->GetBodySetup()->AggGeom.BoxElems.Num() : 0, 1);
	}

	// Two objects: separate meshes, or one combined mesh.
	const FLevelToolResult Separate = RunImport(*this, FString::Printf(TEXT(R"({"files": [%s], "destination": "%s",
		"options": {"combine_meshes": false, "import_materials": false}})"), *Quote(Boxes), *Scope.Folder(TEXT("Separate"))), TEXT("separate"));
	const FLevelToolResult Combined = RunImport(*this, FString::Printf(TEXT(R"({"files": [%s], "destination": "%s",
		"pipeline": {"MeshPipeline": {"%s": %s}}, "options": {"import_materials": false}})"), *Quote(Boxes),
		*Scope.Folder(TEXT("Combined")), CombineSetting, CombineAll), TEXT("combined"));
	const FLevelToolResult CombinedByOption = RunImport(*this, FString::Printf(TEXT(R"({"files": [%s], "destination": "%s",
		"options": {"combine_meshes": true, "import_materials": false}})"), *Quote(Boxes), *Scope.Folder(TEXT("CombinedByOption"))),
		TEXT("combined by option"));
	TestEqual(TEXT("two separate meshes"), ResultAssets(Separate, TEXT("StaticMesh")).Num(), 2);
	TestEqual(TEXT("one combined mesh"), ResultAssets(Combined, TEXT("StaticMesh")).Num(), 1);
	TestEqual(TEXT("combine_meshes makes one combined mesh"), ResultAssets(CombinedByOption, TEXT("StaticMesh")).Num(), 1);
	TestTrue(TEXT("the combined mesh spans both boxes"), FactSize(ResultAsset(Combined, TEXT("StaticMesh")), 0) > PlainSize * 0.5);

	// FBX: the editor's FBX exporter writes the imported crate; it imports back as a static mesh.
	const FString Fbx = Scope.Source(TEXT("CrateExport.fbx"));
	if (TestTrue(TEXT("FBX exported"), ExportAsset(LoadObject<UStaticMesh>(nullptr, *AssetPath(PlainMesh)), Fbx)))
	{
		const FLevelToolResult FromFbx = RunImport(*this, FString::Printf(TEXT(R"({"files": [%s], "destination": "%s",
			"options": {"mesh_type": "static", "import_materials": false}})"), *Quote(Fbx), *Scope.Folder(TEXT("Fbx"))), TEXT("fbx"));
		const TSharedPtr<FJsonObject> FbxMesh = ResultAsset(FromFbx, TEXT("StaticMesh"));
		TestTrue(TEXT("FBX import"), FromFbx.bSuccess && FbxMesh.IsValid());
		TestTrue(TEXT("the FBX mesh has the crate's size"), FMath::IsNearlyEqual(FactSize(FbxMesh, 0), PlainSize, PlainSize * 0.02));
		TestEqual(TEXT("12 triangles"), FactNumber(FbxMesh, TEXT("Triangles")), 12.0);
		// The import's uniform scale (Interchange's OBJ translator ignores it; FBX and glTF apply it).
		const FLevelToolResult FbxScaled = RunImport(*this, FString::Printf(TEXT(R"({"files": [%s], "destination": "%s", "name": "CrateBig",
			"options": {"uniform_scale": 2.0, "import_materials": false}})"), *Quote(Fbx), *Scope.Folder(TEXT("Fbx"))), TEXT("fbx scaled"));
		const TSharedPtr<FJsonObject> ScaledFbx = ResultAsset(FbxScaled, TEXT("StaticMesh"));
		TestTrue(TEXT("scaled FBX import"), FbxScaled.bSuccess && AssetPath(ScaledFbx).EndsWith(TEXT(".CrateBig")));
		TestTrue(*FString::Printf(TEXT("twice the size (%f vs %f)"), FactSize(ScaledFbx, 0), PlainSize),
			FMath::IsNearlyEqual(FactSize(ScaledFbx, 0), PlainSize * 2.0, PlainSize * 0.02));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentImportSkeletalTest, "UEAssetDoc.Import.SkeletalMesh.GltfAndAnimation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentImportSkeletalTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentImportTests;
	FScopedImportTest Scope;
	const FString Gltf = Scope.Source(TEXT("Flag.gltf"));
	if (!TestTrue(TEXT("glTF written"), WriteGltfSkinnedFlag(Gltf)))
	{
		return false;
	}
	const FLevelToolResult Imported = RunImport(*this, FString::Printf(TEXT(R"({"files": [%s], "destination": "%s",
		"options": {"import_materials": false, "create_physics_asset": false}})"), *Quote(Gltf), *Scope.Folder(TEXT("Hero"))), TEXT("glTF"));
	const TSharedPtr<FJsonObject> Mesh = ResultAsset(Imported, TEXT("SkeletalMesh"));
	const TSharedPtr<FJsonObject> Skeleton = ResultAsset(Imported, TEXT("Skeleton"));
	const TSharedPtr<FJsonObject> Anim = ResultAsset(Imported, TEXT("AnimSequence"));
	if (!TestTrue(TEXT("glTF import"), Imported.bSuccess) || !TestTrue(TEXT("a skeletal mesh"), Mesh.IsValid())
		|| !TestTrue(TEXT("a skeleton"), Skeleton.IsValid()) || !TestTrue(TEXT("an animation"), Anim.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("two bones"), FactNumber(Mesh, TEXT("Bones")), 2.0);
	TestEqual(TEXT("the mesh uses the skeleton"), FactString(Mesh, TEXT("Skeleton")), AssetPath(Skeleton));
	TestTrue(TEXT("the flag is a metre tall (100 units)"), FMath::IsNearlyEqual(FMath::Max3(FactSize(Mesh, 0), FactSize(Mesh,
		1), FactSize(Mesh, 2)), 100.0, 1.0));
	TestTrue(*FString::Printf(TEXT("the animation is one second (%f)"), FactNumber(Anim, TEXT("PlayLength"))),
		FMath::IsNearlyEqual(FactNumber(Anim, TEXT("PlayLength")), 1.0, 0.05));
	TestEqual(TEXT("the animation uses the skeleton"), FactString(Anim, TEXT("Skeleton")), AssetPath(Skeleton));
	TestNull(TEXT("no physics asset"), ResultAsset(Imported, TEXT("PhysicsAsset")).Get());

	// The animation alone onto the existing skeleton (the import dialog's Skeleton + Import Only Animations).
	const FLevelToolResult AnimOnly = RunImport(*this, FString::Printf(TEXT(R"({"files": [%s], "destination": "%s",
		"options": {"skeleton": "%s", "animations_only": true, "import_materials": false}})"), *Quote(Gltf), *Scope.Folder(TEXT("Anims")),
		*AssetPath(Skeleton)), TEXT("anim only"));
	const TSharedPtr<FJsonObject> OnlyAnim = ResultAsset(AnimOnly, TEXT("AnimSequence"));
	TestTrue(TEXT("animation-only import"), AnimOnly.bSuccess && OnlyAnim.IsValid());
	TestEqual(TEXT("onto the target skeleton"), FactString(OnlyAnim, TEXT("Skeleton")), AssetPath(Skeleton));
	TestNull(TEXT("no new skeletal mesh"), ResultAsset(AnimOnly, TEXT("SkeletalMesh")).Get());
	TestNull(TEXT("no new skeleton"), ResultAsset(AnimOnly, TEXT("Skeleton")).Get());

	// FBX: the animation exported by the editor's FBX exporter imports onto the skeleton.
	const FString Fbx = Scope.Source(TEXT("Wave.fbx"));
	if (TestTrue(TEXT("animation exported to FBX"), ExportAsset(LoadObject<UAnimSequence>(nullptr, *AssetPath(Anim)), Fbx)))
	{
		const FLevelToolResult FromFbx = RunImport(*this, FString::Printf(TEXT(R"({"files": [%s], "destination": "%s",
			"options": {"skeleton": "%s", "animations_only": true}})"), *Quote(Fbx), *Scope.Folder(TEXT("FbxAnims")), *AssetPath(Skeleton)),
			TEXT("fbx anim"));
		const TSharedPtr<FJsonObject> FbxAnim = ResultAsset(FromFbx, TEXT("AnimSequence"));
		TestTrue(TEXT("FBX animation import"), FromFbx.bSuccess && FbxAnim.IsValid());
		TestEqual(TEXT("FBX animation onto the target skeleton"), FactString(FbxAnim, TEXT("Skeleton")), AssetPath(Skeleton));
		TestTrue(TEXT("FBX animation about one second"), FMath::IsNearlyEqual(FactNumber(FbxAnim, TEXT("PlayLength")), 1.0, 0.1));

		// The same FBX has bones and no mesh. Without its animations it is a skeleton-only FBX, which
		// Interchange imports as nothing ("There was no data to import"); the diagnostic says why and what to pass.
		const FLevelToolResult BonesOnly = RunImport(*this, FString::Printf(TEXT(R"({"files": [%s], "destination": "%s",
			"options": {"import_animations": false}})"), *Quote(Fbx), *Scope.Folder(TEXT("BonesOnly"))), TEXT("bones-only fbx"));
		TestFalse(TEXT("a skeleton-only FBX imports nothing"), BonesOnly.bSuccess);
		const FAssetDocumentDiagnostic* NothingImported = BonesOnly.Diagnostics.FindByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == TEXT("Import.NothingImported") && Diagnostic.Path == TEXT("/files/0");
		});
		if (TestNotNull(TEXT("Import.NothingImported"), NothingImported))
		{
			AddInfo(NothingImported->Message);
			TestTrue(TEXT("the message says the file has no mesh"), NothingImported->Message.Contains(TEXT("no mesh")));
			TestTrue(TEXT("the message names options.skeleton"), NothingImported->Message.Contains(TEXT("options.skeleton")));
			TestTrue(TEXT("the message names options.animations_only"), NothingImported->Message.Contains(TEXT("options.animations_only")));
		}
	}

	// A skeleton option naming nothing is refused before anything imports.
	const FLevelToolResult BadSkeleton = RunImport(*this, FString::Printf(TEXT(R"({"files": [%s], "destination": "%s",
		"options": {"skeleton": "/Game/Nope/SK_Missing"}})"), *Quote(Gltf), *Scope.Folder(TEXT("Bad"))), TEXT("bad skeleton"));
	TestTrue(TEXT("Import.SkeletonNotFound"), HasDiagnostic(BadSkeleton, TEXT("Import.SkeletonNotFound"), TEXT("/options/skeleton")));
	TestFalse(TEXT("nothing imported"), FPackageName::DoesPackageExist(Scope.Folder(TEXT("Bad")) / TEXT("Flag")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentImportTextureTest, "UEAssetDoc.Import.Texture.Formats",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentImportTextureTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentImportTests;
	FScopedImportTest Scope;
	const FString Png = Scope.Source(TEXT("T_Bricks_N.png"));
	const FString Jpg = Scope.Source(TEXT("T_Mask.jpg"));
	const FString Tga = Scope.Source(TEXT("T_Rough.tga"));
	const FString Exr = Scope.Source(TEXT("T_Sky.exr"));
	if (!TestTrue(TEXT("images written"), WriteImage(Png, 64, 32) && WriteImage(Jpg, 32, 32) && WriteImage(Tga, 16, 16) && WriteImage(Exr, 8, 8)))
	{
		return false;
	}
	// A normal map.
	const FLevelToolResult Normal = RunImport(*this, FString::Printf(TEXT(R"({"files": [%s], "destination": "%s",
		"options": {"normal_map": true}})"), *Quote(Png), *Scope.Folder(TEXT("Tex"))), TEXT("png normal"));
	const TSharedPtr<FJsonObject> NormalTex = ResultAsset(Normal, TEXT("Texture2D"));
	if (!TestTrue(TEXT("PNG import"), Normal.bSuccess && NormalTex.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("64 wide"), FactSize(NormalTex, 0), 64.0);
	TestEqual(TEXT("32 high"), FactSize(NormalTex, 1), 32.0);
	TestEqual(TEXT("normal map compression"), FactString(NormalTex, TEXT("Compression")), FString(TEXT("TC_Normalmap")));
	TestEqual(TEXT("normal map group"), FactString(NormalTex, TEXT("LODGroup")), FString(TEXT("TEXTUREGROUP_WorldNormalMap")));
	bool bSrgb = true;
	Facts(NormalTex)->TryGetBoolField(TEXT("SRGB"), bSrgb);
	TestFalse(TEXT("normal maps are linear"), bSrgb);
	if (UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *AssetPath(NormalTex)))
	{
		TestEqual(TEXT("the texture's compression"), Texture->CompressionSettings.GetValue(), TC_Normalmap);
		TestFalse(TEXT("saved with the options"), Texture->GetOutermost()->IsDirty());
	}

	// JPG and TGA together: linear, grayscale.
	const FLevelToolResult Masks = RunImport(*this, FString::Printf(TEXT(R"({"files": [%s, %s], "destination": "%s",
		"options": {"srgb": false, "compression": "Grayscale"}})"), *Quote(Jpg), *Quote(Tga), *Scope.Folder(TEXT("Tex"))), TEXT("jpg+tga"));
	TestTrue(TEXT("JPG and TGA import"), Masks.bSuccess);
	TArray<TSharedPtr<FJsonObject>> MaskTextures = ResultAssets(Masks, TEXT("Texture2D"));
	TestEqual(TEXT("two textures"), MaskTextures.Num(), 2);
	for (const TSharedPtr<FJsonObject>& Texture : MaskTextures)
	{
		bool bTextureSrgb = true;
		Facts(Texture)->TryGetBoolField(TEXT("SRGB"), bTextureSrgb);
		TestFalse(*FString::Printf(TEXT("%s is linear"), *AssetPath(Texture)), bTextureSrgb);
		TestEqual(*FString::Printf(TEXT("%s is grayscale"), *AssetPath(Texture)), FactString(Texture, TEXT("Compression")),
			FString(TEXT("TC_Grayscale")));
		TestEqual(*FString::Printf(TEXT("%s is square"), *AssetPath(Texture)), FactSize(Texture, 0), FactSize(Texture, 1));
	}

	// EXR: an HDR texture.
	const FLevelToolResult Hdr = RunImport(*this, FString::Printf(TEXT(R"({"files": [%s], "destination": "%s"})"), *Quote(Exr),
		*Scope.Folder(TEXT("Tex"))), TEXT("exr"));
	const TSharedPtr<FJsonObject> HdrTex = ResultAsset(Hdr, TEXT("Texture2D"));
	TestTrue(TEXT("EXR import"), Hdr.bSuccess && HdrTex.IsValid());
	bool bHdr = false;
	Facts(HdrTex)->TryGetBoolField(TEXT("HDR"), bHdr);
	TestTrue(TEXT("an HDR source"), bHdr);
	TestEqual(TEXT("8 wide"), FactSize(HdrTex, 0), 8.0);

	// Unknown compression: refused per texture, the import itself stands.
	const FLevelToolResult BadCompression = RunImport(*this, FString::Printf(TEXT(R"({"files": [%s], "destination": "%s",
		"options": {"compression": "Shiny"}})"), *Quote(Tga), *Scope.Folder(TEXT("Bad"))), TEXT("bad compression"));
	TestTrue(TEXT("Import.InvalidOption for the compression"), HasDiagnostic(BadCompression, TEXT("Import.InvalidOption"),
		TEXT("/options/compression")));
	TestEqual(TEXT("imported, partly as asked: 207"), BadCompression.HttpStatus, 207);
	TestTrue(TEXT("the texture is still listed"), ResultAsset(BadCompression, TEXT("Texture2D")).IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentImportSoundTest, "UEAssetDoc.Import.Sound.Wav",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentImportSoundTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentImportTests;
	FScopedImportTest Scope;
	const FString Wav = Scope.Source(TEXT("Beep.wav"));
	if (!TestTrue(TEXT("WAV written"), WriteWav(Wav, 22050, 0.5)))
	{
		return false;
	}
	const FLevelToolResult Imported = RunImport(*this, FString::Printf(TEXT(R"({"files": [%s], "destination": "%s"})"), *Quote(Wav),
		*Scope.Folder(TEXT("Audio"))), TEXT("wav"));
	const TSharedPtr<FJsonObject> Sound = ResultAsset(Imported, TEXT("SoundWave"));
	if (!TestTrue(TEXT("WAV import"), Imported.bSuccess && Sound.IsValid()))
	{
		return false;
	}
	TestTrue(*FString::Printf(TEXT("half a second (%f)"), FactNumber(Sound, TEXT("Duration"))),
		FMath::IsNearlyEqual(FactNumber(Sound, TEXT("Duration")), 0.5, 0.01));
	TestEqual(TEXT("mono"), FactNumber(Sound, TEXT("Channels")), 1.0);
	TestEqual(TEXT("22050 Hz"), FactNumber(Sound, TEXT("ImportedSampleRate")), 22050.0);
	TestTrue(TEXT("the source is recorded"), FactString(Sound, TEXT("Source")).EndsWith(TEXT("Beep.wav")));
	const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
	FString Importer;
	if (Imported.Payload->TryGetArrayField(TEXT("files"), Files) && Files->Num() == 1)
	{
		Importer = (*Files)[0]->AsObject()->GetStringField(TEXT("importer"));
	}
	AddInfo(FString::Printf(TEXT("WAV importer: %s"), *Importer));
	TestFalse(TEXT("the importer is named"), Importer.IsEmpty());

	// Mesh options do not apply to a sound: a warning, the sound imports.
	const FLevelToolResult WithMeshOption = RunImport(*this, FString::Printf(TEXT(R"({"files": [%s], "destination": "%s", "name": "Beep2",
		"options": {"combine_meshes": true}})"), *Quote(Wav), *Scope.Folder(TEXT("Audio"))), TEXT("mesh option"));
	TestTrue(TEXT("imported"), ResultAsset(WithMeshOption, TEXT("SoundWave")).IsValid());
	TestTrue(TEXT("Import.OptionNotApplicable"), HasDiagnostic(WithMeshOption, TEXT("Import.OptionNotApplicable"), TEXT("/options/combine_meshes")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentImportReimportTest, "UEAssetDoc.Import.Reimport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentImportReimportTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentImportTests;
	FScopedImportTest Scope;
	const FString Cube = Scope.Source(TEXT("Block.obj"));
	const FString Png = Scope.Source(TEXT("T_Decal.png"));
	if (!TestTrue(TEXT("sources written"), WriteObjCube(Cube, 100.0) && WriteImage(Png, 32, 32)))
	{
		return false;
	}
	const FLevelToolResult Imported = RunImport(*this, FString::Printf(TEXT(R"({"files": [%s, %s], "destination": "%s",
		"options": {"import_materials": false}})"), *Quote(Cube), *Quote(Png), *Scope.Folder(TEXT("Props"))), TEXT("import"));
	const FString MeshPath = AssetPath(ResultAsset(Imported, TEXT("StaticMesh")));
	const FString TexturePath = AssetPath(ResultAsset(Imported, TEXT("Texture2D")));
	if (!TestTrue(TEXT("imported"), Imported.bSuccess) || !TestFalse(TEXT("a mesh"), MeshPath.IsEmpty())
		|| !TestFalse(TEXT("a texture"), TexturePath.IsEmpty()))
	{
		return false;
	}
	const double Size = FactSize(ResultAsset(Imported, TEXT("StaticMesh")), 0);

	// The source changed on disk: reimport picks it up, the asset is updated in place.
	WriteObjCube(Cube, 200.0);
	WriteImage(Png, 64, 16);
	const FLevelToolResult Reimported = RunImport(*this, FString::Printf(TEXT(R"({"action": "reimport", "assets": ["%s", "%s"],
		"options": {"srgb": false}})"), *MeshPath, *TexturePath), TEXT("reimport"));
	TestTrue(TEXT("reimported"), Reimported.bSuccess);
	const TSharedPtr<FJsonObject> Mesh = ResultAsset(Reimported, TEXT("StaticMesh"));
	const TSharedPtr<FJsonObject> Texture = ResultAsset(Reimported, TEXT("Texture2D"));
	TestEqual(TEXT("the mesh is updated"), Mesh.IsValid() ? Mesh->GetStringField(TEXT("status")) : FString(), FString(TEXT("updated")));
	TestEqual(TEXT("same mesh asset"), AssetPath(Mesh), MeshPath);
	TestTrue(*FString::Printf(TEXT("twice the size (%f vs %f)"), FactSize(Mesh, 0), Size),
		FMath::IsNearlyEqual(FactSize(Mesh, 0), Size * 2.0, 0.01));
	TestEqual(TEXT("the texture is 64 wide"), FactSize(Texture, 0), 64.0);
	bool bSrgb = true;
	Facts(Texture)->TryGetBoolField(TEXT("SRGB"), bSrgb);
	TestFalse(TEXT("texture options apply on reimport"), bSrgb);

	// From a new source file.
	const FString Bigger = Scope.Source(TEXT("BlockV2.obj"));
	WriteObjCube(Bigger, 300.0);
	const FLevelToolResult NewSource = RunImport(*this, FString::Printf(TEXT(R"({"action": "reimport", "assets": ["%s"], "source": %s})"),
		*MeshPath, *Quote(Bigger)), TEXT("new source"));
	const TSharedPtr<FJsonObject> Moved = ResultAsset(NewSource, TEXT("StaticMesh"));
	TestTrue(TEXT("reimported from the new file"), NewSource.bSuccess && Moved.IsValid());
	TestTrue(TEXT("three times the size"), FMath::IsNearlyEqual(FactSize(Moved, 0), Size * 3.0, 0.01));
	TestTrue(TEXT("the new source is recorded"), FactString(Moved, TEXT("Source")).EndsWith(TEXT("BlockV2.obj")));
	AddInfo(FString::Printf(TEXT("recorded source: %s"), *FactString(Moved, TEXT("Source"))));
	// The next plain reimport uses the new source.
	WriteObjCube(Bigger, 400.0);
	const FLevelToolResult Again = RunImport(*this, FString::Printf(TEXT(R"({"action": "reimport", "assets": ["%s"]})"), *MeshPath),
		TEXT("again"));
	TestTrue(TEXT("reimported from the recorded new source"), Again.bSuccess);
	TestTrue(TEXT("four times the size"), FMath::IsNearlyEqual(FactSize(ResultAsset(Again, TEXT("StaticMesh")), 0), Size * 4.0, 0.01));

	// The recorded source is gone: refused, naming 'source'.
	IFileManager::Get().Delete(*Bigger);
	TestTrue(TEXT("Import.SourceNotFound"), HasDiagnostic(RunImport(*this, FString::Printf(TEXT(R"({"action": "reimport", "assets": ["%s"]})"),
		*MeshPath), TEXT("source gone")), TEXT("Import.SourceNotFound"), TEXT("/assets/0")));
	// No such asset; an asset not imported from a file; options of the import dialog.
	TestTrue(TEXT("Import.AssetNotFound"), HasDiagnostic(RunImport(*this, TEXT(R"({"action": "reimport", "assets": ["/Game/Nope/SM_Missing"]})"),
		TEXT("missing")), TEXT("Import.AssetNotFound"), TEXT("/assets/0")));
	UPackage* Package = CreatePackage(*Scope.Folder(TEXT("M_Plain")));
	UMaterial* Material = NewObject<UMaterial>(Package, TEXT("M_Plain"), RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Material);
	TestTrue(TEXT("Import.NoSourceFile"), HasDiagnostic(RunImport(*this, FString::Printf(TEXT(R"({"action": "reimport", "assets": ["%s"]})"),
		*Material->GetPathName()), TEXT("not imported")), TEXT("Import.NoSourceFile"), TEXT("/assets/0")));
	TestTrue(TEXT("pipeline settings are refused for reimport"), HasDiagnostic(RunImport(*this, FString::Printf(TEXT(R"({"action": "reimport",
		"assets": ["%s"], "pipeline": {"MeshPipeline": {"bCombineStaticMeshes": true}}})"), *MeshPath), TEXT("pipeline")),
		TEXT("Import.InvalidRequest"), TEXT("/pipeline")));
	TestTrue(TEXT("mesh options are refused for reimport"), HasDiagnostic(RunImport(*this, FString::Printf(TEXT(R"({"action": "reimport",
		"assets": ["%s"], "options": {"uniform_scale": 2}})"), *MeshPath), TEXT("mesh option")), TEXT("Import.InvalidRequest"),
		TEXT("/options/uniform_scale")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentImportRefusalsTest, "UEAssetDoc.Import.Refusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentImportRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentImportTests;
	FScopedImportTest Scope;
	const FString Png = Scope.Source(TEXT("T_Once.png"));
	const FString Cube = Scope.Source(TEXT("Crate.obj"));
	const FString Unknown = Scope.Source(TEXT("notes.xyz"));
	if (!TestTrue(TEXT("sources written"), WriteImage(Png, 8, 8) && WriteObjCube(Cube, 50.0)
		&& FFileHelper::SaveStringToFile(TEXT("not an asset"), *Unknown)))
	{
		return false;
	}
	const FString Folder = Scope.Folder(TEXT("Refusals"));
	auto Refused = [this, &Folder](const FString& Files, const FString& Extra, const TCHAR* Code, const TCHAR* Path)
	{
		const FLevelToolResult Result = RunImport(*this, FString::Printf(TEXT(R"({"files": [%s], "destination": "%s"%s})"), *Files, *Folder,
			*Extra), Code);
		TestFalse(*FString::Printf(TEXT("%s: refused"), Code), Result.bSuccess);
		TestEqual(*FString::Printf(TEXT("%s: 400"), Code), Result.HttpStatus, 400);
		TestTrue(*FString::Printf(TEXT("%s at %s"), Code, Path), HasDiagnostic(Result, Code, Path));
		return Result;
	};
	Refused(TEXT("\"https://example.com/models/crate.fbx\""), FString(), TEXT("Import.SourceNotLocal"), TEXT("/files/0"));
	Refused(TEXT("\"Models/crate.fbx\""), FString(), TEXT("Import.InvalidRequest"), TEXT("/files/0"));
	Refused(Quote(Scope.Source(TEXT("Missing.fbx"))), FString(), TEXT("Import.SourceNotFound"), TEXT("/files/0"));
	Refused(Quote(Scope.SourceDir), FString(), TEXT("Import.SourceNotFile"), TEXT("/files/0"));
	const FLevelToolResult Unsupported = Refused(Quote(Unknown), FString(), TEXT("Import.FormatNotSupported"), TEXT("/files/0"));
	const TArray<TSharedPtr<FJsonValue>>* Supported = nullptr;
	const FAssetDocumentDiagnostic* Diagnostic = Unsupported.Diagnostics.FindByPredicate([](const FAssetDocumentDiagnostic& Entry)
	{
		return Entry.Code == TEXT("Import.FormatNotSupported");
	});
	TestTrue(TEXT("the supported extensions are listed"), Diagnostic != nullptr && Diagnostic->Details.IsValid()
		&& Diagnostic->Details->TryGetArrayField(TEXT("supported"), Supported) && Supported->ContainsByPredicate(
			[](const TSharedPtr<FJsonValue>& Value) { return Value->AsString() == TEXT("fbx"); }));
	Refused(Quote(Cube), TEXT(R"(, "options": {"scale": 2})"), TEXT("Import.UnknownOption"), TEXT("/options/scale"));
	Refused(Quote(Cube), TEXT(R"(, "options": {"uniform_scale": "big"})"), TEXT("Import.InvalidOption"), TEXT("/options/uniform_scale"));
	Refused(Quote(Cube), TEXT(R"(, "options": {"mesh_type": "rigid"})"), TEXT("Import.InvalidOption"), TEXT("/options/mesh_type"));
	Refused(Quote(Cube), TEXT(R"(, "pipeline": {"MeshPipeline": {"bCombineEverything": true}})"), TEXT("Import.UnknownPipelineProperty"),
		TEXT("/pipeline/MeshPipeline/bCombineEverything"));
	Refused(Quote(Cube), TEXT(R"(, "pipeline": {"bNoSuchSetting": true})"), TEXT("Import.UnknownPipelineProperty"), TEXT("/pipeline/bNoSuchSetting"));
	Refused(Quote(Cube), TEXT(R"(, "colour": true)"), TEXT("Import.InvalidRequest"), TEXT("/colour"));
	TestTrue(TEXT("nothing was imported by the refusals"), !FPackageName::DoesPackageExist(Folder / TEXT("Crate")));

	// Destinations outside /Game.
	for (const TCHAR* Destination : {TEXT("/Engine/Imported"), TEXT("Game/Imported"), TEXT("/Game/Imported/")})
	{
		const FLevelToolResult Result = RunImport(*this, FString::Printf(TEXT(R"({"files": [%s], "destination": "%s"})"), *Quote(Png),
			Destination), Destination);
		TestTrue(*FString::Printf(TEXT("%s: Import.InvalidDestination"), Destination), HasDiagnostic(Result, TEXT("Import.InvalidDestination"),
			TEXT("/destination")));
	}

	// An existing asset: refused unless replace_existing (then updated in place).
	const FLevelToolResult First = RunImport(*this, FString::Printf(TEXT(R"({"files": [%s], "destination": "%s"})"), *Quote(Png), *Folder),
		TEXT("first"));
	TestTrue(TEXT("first import"), First.bSuccess);
	Refused(Quote(Png), FString(), TEXT("Import.AssetExists"), TEXT("/files/0"));
	WriteImage(Png, 16, 8);
	const FLevelToolResult Replaced = RunImport(*this, FString::Printf(TEXT(R"({"files": [%s], "destination": "%s", "replace_existing": true})"),
		*Quote(Png), *Folder), TEXT("replace"));
	const TSharedPtr<FJsonObject> Texture = ResultAsset(Replaced, TEXT("Texture2D"));
	TestTrue(TEXT("replaced"), Replaced.bSuccess && Texture.IsValid());
	TestEqual(TEXT("the same asset"), AssetPath(Texture), AssetPath(ResultAsset(First, TEXT("Texture2D"))));
	TestEqual(TEXT("updated"), Texture.IsValid() ? Texture->GetStringField(TEXT("status")) : FString(), FString(TEXT("updated")));
	TestEqual(TEXT("with the new size"), FactSize(Texture, 0), 16.0);

	// Request shape.
	const FLevelToolResult NoFiles = RunImport(*this, FString::Printf(TEXT(R"({"destination": "%s"})"), *Folder), TEXT("no files"));
	TestTrue(TEXT("no files: Import.InvalidRequest"), HasDiagnostic(NoFiles, TEXT("Import.InvalidRequest"), TEXT("/files")));
	const FLevelToolResult NoDestination = RunImport(*this, FString::Printf(TEXT(R"({"files": [%s]})"), *Quote(Png)), TEXT("no destination"));
	TestTrue(TEXT("no destination: Import.InvalidRequest"), HasDiagnostic(NoDestination, TEXT("Import.InvalidRequest"), TEXT("/destination")));
	const FLevelToolResult BadAction = RunImport(*this, TEXT(R"({"action": "export"})"), TEXT("bad action"));
	TestTrue(TEXT("bad action: Import.InvalidRequest"), HasDiagnostic(BadAction, TEXT("Import.InvalidRequest"), TEXT("/action")));
	return true;
}

#endif
