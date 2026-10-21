// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Fidelity oracle over the Niagara corpus (design §6.1), one test per tier:
//   UEAssetDoc.Niagara.Corpus.{Systems, Stateless, Stress, Emitters, EmitterStress, Scripts, Collections}
// For every corpus asset present in this install: duplicate it to /Game (Content Browser Duplicate, saved),
// then Extract twice, after unload + reload from disk, and from a second Duplicate of the copy; all four
// documents must be identical (the second duplicate compared with its own package path mapped back). Inspect
// must succeed. Extract and Inspect never dirty a package (copy or plugin source). Durations are logged as
// "NiagaraTiming" lines (stress tier: recorded, not gated). Systems, Stateless and Stress tiers also round trip
// (N3): the copy's extract written as a Create document of a new system (emitters added from their Parent, or empty,
// every region authored) gives the same document, and re-applying it is a no-op; system scratch pads are authored
// (N4), emitter scratch pads come with emitter assets (round tripped on duplicates by Niagara.ScratchPad.Systems).
// The Emitters and EmitterStress tiers round trip as emitter assets (N5, RoundTripEmitter); the Emitters tier gates
// its refusals (known legacy content, or an emitter that does not compile on its own), EmitterStress records them.
//   UEAssetDoc.Niagara.Corpus.Smoke reads every NiagaraSystem and NiagaraEmitter asset of
// every mount straight through fx.niagara.read.v1 (no duplicate): no failure, every page within the runtime
// value budget, no package dirtied; total and slowest durations are logged.

#include "AssetDocumentNiagaraPrimitives.h"
#include "AssetDocumentNiagaraTestEditing.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "NiagaraEmitter.h"
#include "NiagaraParameterCollection.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentNiagaraTests
{
namespace
{
const TCHAR* const SystemTier[] = {
	TEXT("/Niagara/DefaultAssets/DefaultSystem.DefaultSystem"),
	TEXT("/Niagara/DefaultAssets/Templates/Systems/DirectionalBurst.DirectionalBurst"),
	TEXT("/Niagara/DefaultAssets/Templates/Systems/RadialBurst.RadialBurst"),
	TEXT("/Niagara/DefaultAssets/Templates/Systems/SimpleExplosion.SimpleExplosion"),
	TEXT("/Niagara/DefaultAssets/Templates/Systems/AttributeReaderTrails.AttributeReaderTrails"),
	TEXT("/Niagara/DefaultAssets/Templates/BehaviorExamples/RenderTargetTexturePainter.RenderTargetTexturePainter"),
	TEXT("/Water/Effects/Niagara/Shoreline/NiagaraShore_System.NiagaraShore_System"),
	TEXT("/ExampleCustomDataInterface/FollowCursorSystem.FollowCursorSystem"),
	TEXT("/Niagara/VectorFields/VectorFieldVisualizationSystem.VectorFieldVisualizationSystem"),
	TEXT("/WaterAdvanced/Niagara/Systems/NDCCollision_FountainDemo.NDCCollision_FountainDemo"),
	TEXT("/ClonerEffector/Systems/NS_ClonerBase.NS_ClonerBase"),
	TEXT("/VirtualCamera/Modifiers/TeleportModifier/Dependencies/NS_TeleportUX.NS_TeleportUX"),
};

const TCHAR* const StatelessTier[] = {
	TEXT("/Niagara/DefaultAssets/Templates/Systems/MinimalLightweight.MinimalLightweight"),
	TEXT("/Niagara/DefaultAssets/Templates/Systems/DirectionalBurstLightweight.DirectionalBurstLightweight"),
	TEXT("/Niagara/DefaultAssets/Templates/Systems/FountainLightweight.FountainLightweight"),
};

const TCHAR* const StressTier[] = {
	TEXT("/NiagaraFluids/Templates/Gas/2D/Systems/Grid2D_Gas_Explosion.Grid2D_Gas_Explosion"),
	TEXT("/WaterAdvanced/Niagara/Systems/Grid2D_SW_River.Grid2D_SW_River"),
	TEXT("/NiagaraFluids/Templates/Gas/3D/Systems/Grid3D_Gas_Fire.Grid3D_Gas_Fire"),
};

const TCHAR* const EmitterTier[] = {
	TEXT("/Niagara/DefaultAssets/Templates/BehaviorExamples/SubUVAnimation.SubUVAnimation"),
	TEXT("/Niagara/DefaultAssets/Templates/BehaviorExamples/RibbonID.RibbonID"),
	TEXT("/Niagara/DefaultAssets/Templates/BehaviorExamples/KillParticles.KillParticles"),
	TEXT("/Niagara/DefaultAssets/Templates/BehaviorExamples/WorkingWithAttributes.WorkingWithAttributes"),
	TEXT("/Niagara/DefaultAssets/Templates/BehaviorExamples/SpawnGroupsFromMultipleBones.SpawnGroupsFromMultipleBones"),
	TEXT("/Niagara/DefaultAssets/Templates/BehaviorExamples/RendererBindingOverrides.RendererBindingOverrides"),
	TEXT("/Niagara/DefaultAssets/Templates/CascadeConversion/CompletelyEmpty.CompletelyEmpty"),
	TEXT("/HairStrands/Emitters/GroomRodsEmitter.GroomRodsEmitter"),
};
const TCHAR* const TemplateEmitterFolder = TEXT("/Niagara/DefaultAssets/Templates/Emitters");

// NiagaraFluids emitter assets (simulation stages, grid data interfaces, scratch pads, deep parent chains).
const TCHAR* const EmitterStressTier[] = {
	TEXT("/NiagaraFluids/Templates/Gas/3D/Emitters/Grid3D_Gas_Emitter.Grid3D_Gas_Emitter"),
	TEXT("/NiagaraFluids/Templates/Gas/2D/Emitters/SourceParticles.SourceParticles"),
	TEXT("/NiagaraFluids/Templates/Sand/Emitters/SandEmitter.SandEmitter"),
	TEXT("/NiagaraFluids/Templates/Utility/Emitters/ParticleSurfacing_Emitter.ParticleSurfacing_Emitter"),
};

const TCHAR* const ScriptTier[] = {
	TEXT("/Niagara/DefaultAssets/DefaultModule.DefaultModule"),
	TEXT("/Niagara/DefaultAssets/DefaultFunction.DefaultFunction"),
	TEXT("/Niagara/DefaultAssets/DefaultDynamicInputs.DefaultDynamicInputs"),
	TEXT("/Niagara/Modules/Update/Camera/CameraOffset.CameraOffset"),
	TEXT("/Niagara/DynamicInputs/Math/Abs_Float.Abs_Float"),
	TEXT("/Niagara/Functions/Rotation/MatrixToQuaternion.MatrixToQuaternion"),
	TEXT("/Niagara/DynamicInputs/Helpers/SimulationPosition.SimulationPosition"),
	TEXT("/Niagara/DynamicInputs/Rotation/MakeQuaternion.MakeQuaternion"),
	TEXT("/Niagara/Modules/Spawn/Location/V2/ShapeLocation.ShapeLocation"),
	TEXT("/Niagara/Functions/MakeScaleMatrix.MakeScaleMatrix"),
	TEXT("/Niagara/Modules/Update/Forces/AerodynamicDrag.AerodynamicDrag"),
	TEXT("/Niagara/Modules/Spawn/Location/V2/SkeletalMeshLocation.SkeletalMeshLocation"),
	TEXT("/Niagara/Modules/Collision/Collision.Collision"),
};

const TCHAR* const CollectionTier[] = {
	TEXT("/DMXFixtures/Fireworks/FWGlobals.FWGlobals"),
};

UObject* LoadCorpusAsset(const FString& ObjectPath)
{
	const FString Package = FPackageName::ObjectPathToPackageName(ObjectPath);
	if (!FPackageName::DoesPackageExist(Package))
	{
		return nullptr;
	}
	return LoadObject<UObject>(nullptr, *ObjectPath);
}

// The oracle for one asset already copied to First (saved). Returns false on the first broken invariant.
bool CheckFidelity(FAutomationTestBase& Test, const FString& Label, const FTarget& First, UPackage* SourcePackage)
{
	const bool bSourceDirty = SourcePackage != nullptr && SourcePackage->IsDirty();
	double ExtractMs = 0.0;
	double RepeatMs = 0.0;
	double ReloadMs = 0.0;
	double DuplicateMs = 0.0;
	double InspectMs = 0.0;
	const FString A = Serialize(Extract(Test, First, Label + TEXT(" extract"), &ExtractMs));
	if (A.IsEmpty())
	{
		return false;
	}
	UObject* Copy = First.Get();
	Test.TestFalse(*FString::Printf(TEXT("%s: extract leaves the copy clean"), *Label), Copy == nullptr || Copy->GetOutermost()->IsDirty());
	const FString B = Serialize(Extract(Test, First, Label + TEXT(" repeat"), &RepeatMs));
	TestTextEqual(Test, Label + TEXT(": extract twice"), B, A);

	const TSharedPtr<FJsonObject> Inspected = Inspect(Test, First.ObjectPath, Label + TEXT(" inspect"), &InspectMs);
	Test.TestTrue(*FString::Printf(TEXT("%s: inspect reports NiagaraDetails"), *Label),
		Inspected.IsValid() && Inspected->HasTypedField<EJson::Object>(TEXT("NiagaraDetails")));
	Copy = First.Get();
	Test.TestFalse(*FString::Printf(TEXT("%s: inspect leaves the copy clean"), *Label), Copy == nullptr || Copy->GetOutermost()->IsDirty());

	// Unload and load back from disk.
	if (Copy != nullptr)
	{
		ReleaseUndoHistory();
		AssetDocumentNiagaraTests::UnloadPackages({Copy->GetOutermost()});
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}
	const FString C = Serialize(Extract(Test, First, Label + TEXT(" reload"), &ReloadMs));
	TestTextEqual(Test, Label + TEXT(": extract after save and reload"), C, A);

	// Duplicate of the copy: same document under its own path.
	const FTarget Second(First.AssetName.Left(40) + TEXT("_Dup"));
	UObject* Reloaded = First.Get();
	UObject* SecondCopy = Reloaded != nullptr ? DuplicateTo(*Reloaded, Second) : nullptr;
	if (Test.TestNotNull(*FString::Printf(TEXT("%s: duplicate of the copy"), *Label), SecondCopy))
	{
		FString D = Serialize(Extract(Test, Second, Label + TEXT(" duplicate"), &DuplicateMs));
		D.ReplaceInline(*Second.ObjectPath, *First.ObjectPath, ESearchCase::CaseSensitive);
		D.ReplaceInline(*Second.PackageName, *First.PackageName, ESearchCase::CaseSensitive);
		// A duplicated parameter collection gets a namespace of its own name (the editor makes it unique).
		D.ReplaceInline(*Second.AssetName, *First.AssetName, ESearchCase::CaseSensitive);
		TestTextEqual(Test, Label + TEXT(": extract of a duplicate"), D, A);
	}
	Second.Cleanup();
	Test.TestEqual(*FString::Printf(TEXT("%s: plugin source package dirty state unchanged"), *Label),
		SourcePackage != nullptr && SourcePackage->IsDirty(), bSourceDirty);
	Test.AddInfo(FString::Printf(TEXT("NiagaraTiming %s extract=%.1fms repeat=%.1fms inspect=%.1fms reload=%.1fms duplicate=%.1fms size=%d"),
		*Label, ExtractMs, RepeatMs, InspectMs, ReloadMs, DuplicateMs, A.Len()));
	return true;
}

// An emitter asset's extract written as a Create document of a new emitter asset (N5): the New Niagara Emitter
// factory's empty emitter (or, with a Parent, a child of it), every region authored through the emitter editor's
// working copy (stacks, inputs, handlers, stages, renderers, properties, scratch pads). The new asset's document
// equals the source's and re-applying it is a no-op. Content the editor can no longer make is refused (OutNote).
// The emitter's own compile status (the emitter editor's Compile on a working copy): "Error" when it has compile
// errors on its own.
FString StandaloneCompileStatus(const FTarget& Target)
{
	using namespace UE::AssetDocument::Niagara;
	UNiagaraEmitter* Emitter = LoadObject<UNiagaraEmitter>(nullptr, *Target.ObjectPath);
	if (Emitter == nullptr)
	{
		return FString();
	}
	TStrongObjectPtr<UNiagaraSystem> Placeholder(MakeEmitterPlaceholder(*Emitter));
	Placeholder->RequestCompile(true);
	SettleCompilation(*Placeholder);
	TArray<FCompileMessage> Messages;
	const FString Status = ReadCompileStatus(*Placeholder, Messages);
	return Messages.ContainsByPredicate([](const FCompileMessage& Message) { return Message.Severity == TEXT("Error"); }) ? FString(TEXT("Error")) : Status;
}

// Version guids are the asset's own: a copy's versions get new ones.
TSharedPtr<FJsonObject> WithoutVersionGuids(const TSharedPtr<FJsonObject>& Document)
{
	const TSharedPtr<FJsonObject> Out = Clone(Document);
	for (const TSharedPtr<FJsonValue>& Entry : List(Child(Out, TEXT("Body")), TEXT("Versions")))
	{
		if (Entry.IsValid() && Entry->Type == EJson::Object)
		{
			Entry->AsObject()->RemoveField(TEXT("Guid"));
		}
	}
	return Out;
}

// How a tier round trips: not at all, gated (a refused create must be one of the known legacy refusals), or recording
// refusals (stress tiers: the refusal's code and message are logged, not gated).
enum class ERoundTrip : uint8
{
	None,
	Gated,
	RecordRefusals,
};

FString FirstError(const FAssetDocumentResult& Result)
{
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		if (!Diagnostic.Code.IsEmpty())
		{
			return Diagnostic.Code + TEXT(": ") + Diagnostic.Message.Left(200);
		}
	}
	return TEXT("refused");
}

bool RoundTripEmitter(FAutomationTestBase& Test, const FString& Label, const FTarget& Source, FRoundTripTiming& OutTiming, FString& OutNote,
	ERoundTrip Mode)
{
	const TSharedPtr<FJsonObject> Extracted = Extract(Test, Source, Label + TEXT(" round trip source"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	const FTarget Fresh(FString(TEXT("NE_RT_")) + Label.Left(30));
	ON_SCOPE_EXIT
	{
		Fresh.Cleanup();
	};
	FString Text = Serialize(Extracted);
	Text.ReplaceInline(*Source.ObjectPath, *Fresh.ObjectPath, ESearchCase::CaseSensitive);
	Text.ReplaceInline(*Source.PackageName, *Fresh.PackageName, ESearchCase::CaseSensitive);
	const TSharedPtr<FJsonObject> Document = Retargeted(Parse(Text), Fresh, TEXT("Create"));
	double Start = FPlatformTime::Seconds();
	const FAssetDocumentResult Created = ApplyDocument(Test, Fresh, Document, Label + TEXT(" round trip create"));
	OutTiming.CreateMs = Milliseconds(Start);
	const TArray<FString> Legacy = LegacyModuleNames(Document);
	struct FRefusal
	{
		const TCHAR* Code;
		const TCHAR* Note;
	};
	const FRefusal Refusals[] = {
		{TEXT("Niagara.InvalidModuleName"), TEXT("module names the editor never gives a new module")},
		{TEXT("Niagara.DeprecatedFunction"), TEXT("a scratch pad calls a deprecated data interface function the menu no longer offers")},
		{TEXT("Graph.ConnectNeedsConversion"), TEXT("a scratch pad links Vector and Position pins directly, which the editor now joins only through a conversion node")},
		{TEXT("Niagara.DynamicInputType"), TEXT("a dynamic input whose output type the input no longer takes (its dynamic input menu does not offer it)")},
	};
	for (const FRefusal& Refusal : Refusals)
	{
		if (!Created.IsSuccess() && HasDiagnostic(Created, Refusal.Code))
		{
			Test.TestFalse(*FString::Printf(TEXT("%s: the refused create leaves no asset"), *Label), Fresh.Exists());
			OutNote = FString::Printf(TEXT("%s%s (%s, not round tripped)"), Refusal.Note,
				Legacy.IsEmpty() ? TEXT("") : *(TEXT(": ") + FString::Join(Legacy, TEXT(", "))), Refusal.Code);
			return false;
		}
	}
	// An older engine's Map Get default pin name (<output>_Input); the editor now names the pin after its output.
	const bool bLegacyDefaultPin = !Created.IsSuccess() && Created.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == TEXT("Graph.PinNotFound") && Diagnostic.Message.Contains(TEXT("_Input'"));
	});
	if (bLegacyDefaultPin)
	{
		Test.TestFalse(*FString::Printf(TEXT("%s: the refused create leaves no asset"), *Label), Fresh.Exists());
		OutNote = TEXT("a scratch pad's Map Get default pin keeps an older engine's name (<output>_Input; the editor now names it after its output) (Graph.PinNotFound, not round tripped)");
		return false;
	}
	if (!Created.IsSuccess() && Mode == ERoundTrip::RecordRefusals)
	{
		Test.TestFalse(*FString::Printf(TEXT("%s: the refused create leaves no asset"), *Label), Fresh.Exists());
		OutNote = FirstError(Created) + TEXT(" (not round tripped)");
		return false;
	}
	// A new asset has no compile errors before the apply, so the errors of an emitter that does not compile on its own
	// (one made for a particular system) are new ones: the create is refused.
	if (!Created.IsSuccess() && HasDiagnostic(Created, TEXT("Niagara.CompileFailed")))
	{
		const FString Status = StandaloneCompileStatus(Source);
		Test.TestEqual(*FString::Printf(TEXT("%s: a compile refusal comes from the emitter's own compile errors"), *Label), Status, FString(TEXT("Error")));
		Test.TestFalse(*FString::Printf(TEXT("%s: the refused create leaves no asset"), *Label), Fresh.Exists());
		OutNote = TEXT("the emitter does not compile on its own (Niagara.CompileFailed, not round tripped)");
		return false;
	}
	if (!Legacy.IsEmpty())
	{
		Test.TestTrue(*FString::Printf(TEXT("%s: a module name the editor cannot give is refused"), *Label), !Created.IsSuccess());
	}
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: the round trip document creates the emitter asset"), *Label), Created.IsSuccess()))
	{
		LogResidue(Test, Fresh, Label);
		return false;
	}
	FString Result = Serialize(Extract(Test, Fresh, Label + TEXT(" round trip result"), &OutTiming.ExtractMs));
	TestTextEqual(Test, FString::Printf(TEXT("%s: round trip == source"), *Label), MaskEditorNames(Content(WithoutVersionGuids(Parse(Result)))),
		MaskEditorNames(Content(WithoutVersionGuids(Document))));
	Start = FPlatformTime::Seconds();
	TestNoOpReapply(Test, Fresh, Label + TEXT(" round trip"));
	OutTiming.ReapplyMs = Milliseconds(Start);
	return true;
}

void RunTier(FAutomationTestBase& Test, TConstArrayView<FString> Paths, int32 MinimumPresent, ERoundTrip Mode = ERoundTrip::None)
{
	int32 Present = 0;
	int32 RoundTripped = 0;
	TArray<FString> Skipped;
	for (const FString& Path : Paths)
	{
		UObject* Source = LoadCorpusAsset(Path);
		if (Source == nullptr)
		{
			Test.AddInfo(FString::Printf(TEXT("NiagaraCorpus absent in this install: %s"), *Path));
			continue;
		}
		++Present;
		const FString Label = FPackageName::ObjectPathToObjectName(Path);
		const FTarget First(Label.Left(40));
		const double Start = FPlatformTime::Seconds();
		UObject* Copy = DuplicateTo(*Source, First);
		const double CopyMs = Milliseconds(Start);
		// Read before the fidelity checks, which reload the copy (the pointer does not survive them).
		const bool bEmitter = Cast<UNiagaraEmitter>(Copy) != nullptr;
		if (Test.TestNotNull(*FString::Printf(TEXT("%s: duplicated to /Game"), *Label), Copy))
		{
			Test.AddInfo(FString::Printf(TEXT("NiagaraTiming %s copy=%.1fms"), *Label, CopyMs));
			if (CheckFidelity(Test, Label, First, Source->GetOutermost()) && Mode != ERoundTrip::None)
			{
				FRoundTripTiming Timing;
				FString Note;
				if (bEmitter ? RoundTripEmitter(Test, Label, First, Timing, Note, Mode) : RoundTrip(Test, Label, First, Timing, Note))
				{
					++RoundTripped;
					Test.AddInfo(FString::Printf(TEXT("NiagaraTiming %s roundtrip create=%.1fms extract=%.1fms reapply=%.1fms"), *Label,
						Timing.CreateMs, Timing.ExtractMs, Timing.ReapplyMs));
				}
				else if (!Note.IsEmpty())
				{
					Skipped.Add(Label + TEXT(" (") + Note + TEXT(")"));
				}
			}
		}
		First.Cleanup();
	}
	Test.TestTrue(TEXT("enough corpus assets are present"), Present >= MinimumPresent);
	if (Mode != ERoundTrip::None)
	{
		Test.AddInfo(FString::Printf(TEXT("NiagaraRoundTrip %d of %d present round tripped%s%s"), RoundTripped, Present,
			Skipped.IsEmpty() ? TEXT("") : TEXT("; skipped: "), *FString::Join(Skipped, TEXT("; "))));
	}
}

TArray<FString> ToStrings(TConstArrayView<const TCHAR*> Paths)
{
	TArray<FString> Out;
	for (const TCHAR* Path : Paths)
	{
		Out.Add(Path);
	}
	return Out;
}

// Every page of fx.niagara.read.v1 on Asset, straight through the handler (as the provider runs it).
bool ReadAllPages(FAutomationTestBase& Test, UObject& Asset, const TCHAR* Detail, int32& OutRows, int32& OutPages)
{
	using namespace UE::AssetDocument::Niagara;
	int64 Offset = 0;
	OutRows = 0;
	OutPages = 0;
	while (Offset >= 0)
	{
		FAssetDocumentPrimitiveInvocation Invocation;
		Invocation.PrimitiveId = TEXT("fx.niagara.read.v1");
		Invocation.Version = 1;
		Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
		FObjectBuilder Args;
		Args.AddInt(TEXT("schema_version"), 1);
		Args.AddInt(TEXT("offset"), Offset);
		Args.AddString(TEXT("detail"), Detail);
		Invocation.ValueArguments = Args.Build();
		FAssetDocumentGraphError Error;
		FCall Call(Invocation, Asset, Error);
		FObjectBuilder Result;
		if (!HandleRead(Call, Result))
		{
			Test.AddError(FString::Printf(TEXT("%s (%s): [%s] %s"), *Asset.GetPathName(), Detail, *Error.Code, *Error.Message));
			return false;
		}
		const FValue Page = Result.Build();
		// Outcomes are bounded to 4096 runtime value nodes (RuntimeValueMaximumNodes).
		const int32 Nodes = UE::AssetDocument::Graph::CountValueNodes(Page);
		if (Nodes > 4096)
		{
			Test.AddError(FString::Printf(TEXT("%s (%s): page at %lld has %d value nodes"), *Asset.GetPathName(), Detail, Offset, Nodes));
			return false;
		}
		const FValue* Rows = UE::AssetDocument::Graph::FindField(Page, TEXT("rows"));
		const FValue* Next = UE::AssetDocument::Graph::FindField(Page, TEXT("next_offset"));
		OutRows += Rows != nullptr ? Rows->Elements.Num() : 0;
		++OutPages;
		Offset = Next != nullptr ? Next->IntegerValue : -1;
	}
	return true;
}
} // namespace
} // namespace AssetDocumentNiagaraTests

#define NIAGARA_CORPUS_TEST(Name, Paths, Minimum, Mode) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraCorpus##Name##Test, "UEAssetDoc.Niagara.Corpus." #Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter) \
	bool FAssetDocumentNiagaraCorpus##Name##Test::RunTest(const FString& Parameters) \
	{ \
		(void)Parameters; \
		using namespace AssetDocumentNiagaraTests; \
		RunTier(*this, ToStrings(Paths), Minimum, ERoundTrip::Mode); \
		return true; \
	}

NIAGARA_CORPUS_TEST(Systems, AssetDocumentNiagaraTests::SystemTier, 6, Gated)
NIAGARA_CORPUS_TEST(Stateless, AssetDocumentNiagaraTests::StatelessTier, 1, Gated)
NIAGARA_CORPUS_TEST(Stress, AssetDocumentNiagaraTests::StressTier, 0, Gated)
NIAGARA_CORPUS_TEST(Scripts, AssetDocumentNiagaraTests::ScriptTier, 10, None)
NIAGARA_CORPUS_TEST(EmitterStress, AssetDocumentNiagaraTests::EmitterStressTier, 0, RecordRefusals)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraCorpusEmittersTest, "UEAssetDoc.Niagara.Corpus.Emitters",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraCorpusEmittersTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraTests;
	TArray<FString> Paths = ToStrings(EmitterTier);
	TArray<FAssetData> Templates;
	IAssetRegistry::GetChecked().GetAssetsByPath(TemplateEmitterFolder, Templates, true);
	for (const FAssetData& Template : Templates)
	{
		if (Template.AssetClassPath == UNiagaraEmitter::StaticClass()->GetClassPathName())
		{
			Paths.Add(Template.GetObjectPathString());
		}
	}
	AddInfo(FString::Printf(TEXT("NiagaraCorpus emitter templates: %d"), Templates.Num()));
	RunTier(*this, Paths, 10, ERoundTrip::Gated);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraCorpusCollectionsTest, "UEAssetDoc.Niagara.Corpus.Collections",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraCorpusCollectionsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraTests;
	RunTier(*this, ToStrings(CollectionTier), 0);

	// A collection built as the editor's New Niagara Parameter Collection + Add Parameter would (the corpus
	// collection only ships with the Templates content).
	const FTarget Built(TEXT("NPC_Built"));
	UPackage* Package = CreatePackage(*Built.PackageName);
	UNiagaraParameterCollection* Collection = NewObject<UNiagaraParameterCollection>(Package, *Built.AssetName,
		RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Collection);
	const FName Speed = Collection->ParameterNameFromFriendlyString(TEXT("Speed"));
	const FName Tint = Collection->ParameterNameFromFriendlyString(TEXT("Tint"));
	Collection->AddParameter(Speed, FNiagaraTypeDefinition::GetFloatDef());
	Collection->AddParameter(Tint, FNiagaraTypeDefinition::GetColorDef());
	if (UNiagaraParameterCollectionInstance* Instance = Collection->GetDefaultInstance())
	{
		Instance->GetParameterStore().SetParameterValue(2.5f, FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), Speed));
		Instance->GetParameterStore().SetParameterValue(FLinearColor(1.0f, 0.5f, 0.25f, 1.0f),
			FNiagaraVariable(FNiagaraTypeDefinition::GetColorDef(), Tint));
	}
	if (TestTrue(TEXT("built collection saves"), SaveAsset(*Collection, Built.PackageFile)))
	{
		const TSharedPtr<FJsonObject> Document = Extract(*this, Built, TEXT("built collection"));
		const FString Text = Serialize(Document);
		AddInfo(FString::Printf(TEXT("NiagaraExample collection %s"), *Text));
		TestTrue(TEXT("collection parameters by friendly name with values"),
			Text.Contains(TEXT("{\"Name\":\"Speed\",\"Type\":\"float\",\"Value\":2.5}")));
		CheckFidelity(*this, TEXT("NPC_Built"), Built, nullptr);
	}
	Built.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraCorpusSmokeTest, "UEAssetDoc.Niagara.Corpus.Smoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraCorpusSmokeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraTests;
	TArray<FAssetData> Assets;
	IAssetRegistry::GetChecked().GetAssetsByClass(UNiagaraSystem::StaticClass()->GetClassPathName(), Assets);
	const int32 SystemCount = Assets.Num();
	IAssetRegistry::GetChecked().GetAssetsByClass(UNiagaraEmitter::StaticClass()->GetClassPathName(), Assets);
	Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });
	struct FTiming
	{
		FString Path;
		double Ms = 0.0;
	};
	TArray<FTiming> Timings;
	double Total = 0.0;
	int32 Failed = 0;
	int32 Rows = 0;
	for (const FAssetData& Data : Assets)
	{
		UObject* Asset = Data.GetAsset();
		if (Asset == nullptr)
		{
			AddInfo(FString::Printf(TEXT("NiagaraSmoke cannot load %s"), *Data.GetObjectPathString()));
			continue;
		}
		const bool bDirty = Asset->GetOutermost()->IsDirty();
		int32 AssetRows = 0;
		int32 Pages = 0;
		const double Start = FPlatformTime::Seconds();
		if (!ReadAllPages(*this, *Asset, TEXT("document"), AssetRows, Pages))
		{
			++Failed;
		}
		const double Ms = Milliseconds(Start);
		Total += Ms;
		Rows += AssetRows;
		Timings.Add({Data.GetObjectPathString(), Ms});
		TestEqual(*FString::Printf(TEXT("%s: read leaves the package dirty state unchanged"), *Data.GetObjectPathString()),
			Asset->GetOutermost()->IsDirty(), bDirty);
		AddInfo(FString::Printf(TEXT("NiagaraSmoke %s rows=%d pages=%d %.1fms"), *Data.GetObjectPathString(), AssetRows, Pages, Ms));
		if (Timings.Num() % 25 == 0)
		{
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
	}
	Timings.Sort([](const FTiming& A, const FTiming& B) { return A.Ms > B.Ms; });
	AddInfo(FString::Printf(TEXT("NiagaraSmoke total: %d systems + %d emitters, %d rows, %d failed, %.0fms"),
		SystemCount, Assets.Num() - SystemCount, Rows, Failed, Total));
	for (int32 Index = 0; Index < FMath::Min(5, Timings.Num()); ++Index)
	{
		AddInfo(FString::Printf(TEXT("NiagaraSmoke slowest %s %.1fms"), *Timings[Index].Path, Timings[Index].Ms));
	}
	TestEqual(TEXT("every Niagara system and emitter reads"), Failed, 0);
	TestTrue(TEXT("the install has Niagara content"), Assets.Num() > 50);
	return true;
}
#endif
