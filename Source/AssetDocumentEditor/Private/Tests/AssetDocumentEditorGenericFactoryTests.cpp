// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Behavior tests for the class-neutral generic factory lifecycle
// (factory_generic.v1): factory selection, the transient factory instance,
// the two scratch primitives and create_by_factory.v2 + rollback_created.v1.

#include "AssetDocumentEditorPrimitiveProvider.h"
#include "AssetDocumentEditorTargetLifecycleCatalog.h"

#include "AssetDocumentEditorProviderTestSupport.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Curves/CurveBase.h"
#include "Curves/CurveFloat.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Engine/StaticMesh.h"
#include "Factories/CurveFactory.h"
#include "Factories/DataAssetFactory.h"
#include "Factories/Factory.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Provider/AssetDocumentEditorNativeIdentity.h"
#include "Provider/AssetDocumentEditorValueDto.h"
#include "Provider/Lifecycle/AssetDocumentEditorGenericFactory.h"
#include "Provider/Lifecycle/AssetDocumentEditorLifecyclePayloads.h"
#include "Provider/AssetDocumentEditorTypedValueCodec.h"
#include "Provider/Property/AssetDocumentEditorPropertyAdmission.h"
#include "Provider/Property/AssetDocumentEditorPropertyBatchTokenPayload.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorGenericFactoryTests
{
using namespace UE::AssetDocument::EditorPrimitive;

const TCHAR* const TestDataAssetClassPath = TEXT("/Script/AssetDocumentTests.TestDataAsset");
const TCHAR* const BehaviorTreeClassPath = TEXT("/Script/AIModule.BehaviorTree");
const TCHAR* const PackageRoot = TEXT("/Game/AssetDocumentTests/GenericFactory");

class FClassPayload final : public INativeIdentityPayload
{
public:
	explicit FClassPayload(UClass& InClass)
		: Class(&InClass), Path(InClass.GetPathName()), Revision(static_cast<uint64>(InClass.GetUniqueID()))
	{
	}
	virtual FName GetPayloadTypeId() const override { return ClassPayloadTypeId; }
	virtual bool IsAlive() const override
	{
		return Class.IsValid() && Class->GetPathName() == Path
			&& static_cast<uint64>(Class->GetUniqueID()) == Revision;
	}
	virtual bool TryGetClassIdentity(FClassIdentityView& OutView) const override
	{
		if (!IsAlive())
		{
			return false;
		}
		OutView.Class = Class.Get();
		OutView.CanonicalClassPath = Path;
		OutView.TypeRevision = Revision;
		return true;
	}

private:
	TWeakObjectPtr<UClass> Class;
	FString Path;
	uint64 Revision = 0;
};

const FAssetDocumentPrimitiveDescriptor* FindDescriptor(
	const IAssetDocumentPrimitiveProvider& Provider, const TCHAR* PrimitiveId)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Provider.DescribePrimitives())
	{
		if (Descriptor.PrimitiveId == PrimitiveId)
		{
			return &Descriptor;
		}
	}
	return nullptr;
}

const FAssetDocumentOpaqueHandle* FindSingleHandle(
	const FAssetDocumentPrimitiveOutcome& Outcome, const TCHAR* Name)
{
	for (const FAssetDocumentPrimitiveHandleResult& Result : Outcome.HandleResults)
	{
		if (Result.Name == Name && Result.Handles.Num() == 1)
		{
			return &Result.Handles[0];
		}
	}
	return nullptr;
}

bool HasDiagnostic(const FAssetDocumentPrimitiveOutcome& Outcome, const TCHAR* Code)
{
	return Outcome.Diagnostics.ContainsByPredicate(
		[Code](const FAssetDocumentRuntimeDiagnostic& Diagnostic) { return Diagnostic.Code == Code; });
}

FString ReadStringField(const FAssetDocumentRuntimeValueDto& Value, const TCHAR* Key)
{
	const int32 Index = Value.ObjectKeys.IndexOfByKey(FString(Key));
	return Index != INDEX_NONE && Value.Elements.IsValidIndex(Index)
			&& Value.Elements[Index].Kind == EAssetDocumentRuntimeValueKind::String
		? Value.Elements[Index].StringValue
		: FString();
}

FAssetDocumentRuntimeValueDto MakeString(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto MakeLocation(const FString& PackagePath, const FString& AssetName)
{
	const FAssetDocumentEditorValueField Fields[] = {
		{TEXT("package_path"), MakeString(PackagePath)},
		{TEXT("asset_name"), MakeString(AssetName)},
	};
	FAssetDocumentRuntimeValueDto Result;
	FString DiagnosticCode;
	TryBuildAssetDocumentEditorValueObject(Fields, Result, DiagnosticCode);
	return Result;
}

UObject* ResolveScratch(
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	const FAssetDocumentEditorProviderTestContext& Context,
	const FAssetDocumentOpaqueHandle& Handle)
{
	FAssetDocumentHandleValidationContext Lease;
	Lease.RequestId = Context.RequestId;
	Lease.TransactionId = Context.TransactionId;
	Lease.RuntimeRevision = Context.RuntimeRevision;
	Lease.ProviderId = Context.ProviderId;
	Lease.Phase = EAssetDocumentRuntimeRequestPhase::Preflight;
	Lease.RequiredKind = EAssetDocumentHandleKind::Scratch;
	Lease.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
	FAssetDocumentVerifiedLeaseResult Result = Handles.AcquireVerifiedLease(Handle, Lease);
	if (!Result.Lease.IsValid())
	{
		return nullptr;
	}
	INativeIdentityPayload* Payload =
		TryGetNativeIdentityPayload(Result.Lease->GetPayloadChecked(), ScratchPayloadTypeId);
	FObjectIdentityView Identity;
	return Payload != nullptr && Payload->TryGetObjectIdentity(Identity) ? Identity.Object : nullptr;
}

FAssetDocumentPrimitiveOutcome InvokeWithClass(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	UClass& AssetClass,
	const FAssetDocumentRuntimeValueDto* Values,
	FAssetDocumentEditorProviderTestContext& Context,
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	FAssetDocumentEditorProviderTestTransaction* Transaction)
{
	FAssetDocumentIssuedHandleResult ClassHandle = Handles.Issue(
		EAssetDocumentHandleKind::Class, EAssetDocumentHandleAccess::ReadOnly,
		MakeUnique<FClassPayload>(AssetClass));
	FAssetDocumentPrimitiveInvocation Invocation =
		MakeAssetDocumentEditorProviderTestInvocation(Descriptor, Context);
	if (Values != nullptr)
	{
		Invocation.ValueArguments = *Values;
	}
	Invocation.HandleArguments.Add({TEXT("asset_class"), ClassHandle.Handle.GetValue()});
	return CreateAssetDocumentEditorPrimitiveProvider()->Invoke(Invocation, Handles, Transaction);
}

struct FTargetPaths
{
	FString PackagePath = PackageRoot;
	FString AssetName;
	FString PackageName;
	FString ObjectPath;
	FString Filename;

	explicit FTargetPaths(const TCHAR* Prefix)
		: AssetName(FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits)))
	{
		PackageName = PackagePath / AssetName;
		ObjectPath = PackageName + TEXT(".") + AssetName;
		Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	}
};

void DeleteTarget(const FTargetPaths& Paths)
{
	if (UObject* Asset = FSoftObjectPath(Paths.ObjectPath).ResolveObject())
	{
		ObjectTools::DeleteObjects({Asset}, false, ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
	}
	if (UPackage* Package = FindPackage(nullptr, *Paths.PackageName))
	{
		Package->SetDirtyFlag(false);
		UPackageTools::UnloadPackages({Package});
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}
	IFileManager::Get().Delete(*Paths.Filename, false, true);
}

void TestTargetAbsent(FAutomationTestBase& Test, const FTargetPaths& Paths, const TCHAR* Label)
{
	Test.TestNull(*FString::Printf(TEXT("%s: object absent"), Label), FSoftObjectPath(Paths.ObjectPath).ResolveObject());
	Test.TestNull(*FString::Printf(TEXT("%s: package absent"), Label), FindPackage(nullptr, *Paths.PackageName));
	Test.TestFalse(*FString::Printf(TEXT("%s: registry absent"), Label),
		FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(Paths.ObjectPath)).IsValid());
	Test.TestFalse(*FString::Printf(TEXT("%s: file absent"), Label), IFileManager::Get().FileExists(*Paths.Filename));
}

UClass* LoadBehaviorTreeClass()
{
	FModuleManager::LoadModulePtr<IModuleInterface>(TEXT("BehaviorTreeEditor"));
	return FSoftClassPath(BehaviorTreeClassPath).ResolveClass();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericFactorySelectionTest,
	"UEAssetDoc.GenericProvider.GenericFactory.Selection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericFactorySelectionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericFactoryTests;
	const FAssetDocumentEditorGenericFactorySelection Curve =
		SelectAssetDocumentEditorGenericFactory(UCurveFloat::StaticClass());
	TestTrue(TEXT("CurveFloat has a factory"), Curve.IsSelected());
	TestTrue(TEXT("CurveFloat uses UCurveFloatFactory"), Curve.FactoryClass == UCurveFloatFactory::StaticClass());
	TestTrue(TEXT("CurveFloat factory supports it exactly"), Curve.bExactSupportedClass);

	UClass* const DataAssetClass = FSoftClassPath(TestDataAssetClassPath).ResolveClass();
	if (TestNotNull(TEXT("TestDataAsset class loads"), DataAssetClass))
	{
		const auto DataAsset = SelectAssetDocumentEditorGenericFactory(DataAssetClass);
		TestTrue(TEXT("DataAsset subclass uses the New-menu DataAsset factory"),
			DataAsset.FactoryClass == UDataAssetFactory::StaticClass());
		TestFalse(TEXT("DataAsset subclass is an ancestor match"), DataAsset.bExactSupportedClass);
	}
	if (UClass* const BehaviorTree = LoadBehaviorTreeClass(); TestNotNull(TEXT("BehaviorTree class loads"), BehaviorTree))
	{
		const auto Tree = SelectAssetDocumentEditorGenericFactory(BehaviorTree);
		TestTrue(TEXT("BehaviorTree uses its exact factory"), Tree.IsSelected() && Tree.bExactSupportedClass
			&& Tree.FactoryClass->GetName() == TEXT("BehaviorTreeFactory"));
	}
	auto Rejected = [this](const TCHAR* Label, const UClass* Class, const TCHAR* Code)
	{
		const auto Selection = SelectAssetDocumentEditorGenericFactory(Class);
		TestFalse(Label, Selection.IsSelected());
		TestEqual(*FString::Printf(TEXT("%s diagnostic"), Label), FString(Selection.GetDiagnosticCode()), FString(Code));
		TestFalse(*FString::Printf(TEXT("%s message"), Label), Selection.GetDiagnosticMessage(Class).IsEmpty());
		TestFalse(*FString::Printf(TEXT("%s not admitted"), Label), IsAssetDocumentEditorGenericFactoryClassAdmitted(Class));
	};
	Rejected(TEXT("Blueprint asset class"), UBlueprint::StaticClass(), TEXT("FactoryCreate.BlueprintClassUnsupported"));
	Rejected(TEXT("abstract class"), UCurveBase::StaticClass(), TEXT("FactoryCreate.AbstractClass"));
	Rejected(TEXT("class without a New factory"), UStaticMesh::StaticClass(), TEXT("FactoryCreate.NoFactory"));
	Rejected(TEXT("missing class"), nullptr, TEXT("FactoryCreate.ClassMismatch"));

	const auto* Generic = FindAssetDocumentEditorTargetLifecycle(TEXT("/Script/Engine.CurveFloat"));
	if (TestNotNull(TEXT("CurveFloat resolves the class-neutral lifecycle"), Generic))
	{
		TestTrue(TEXT("generic row is class-neutral"), Generic->IsClassNeutral());
		TestEqual(TEXT("generic lifecycle id"), Generic->LifecycleId, FString(AssetDocumentEditorGenericFactoryLifecycleId));
		TestTrue(TEXT("generic row creates CurveFloat"),
			CanAssetDocumentEditorTargetLifecycleCreate(*Generic, TEXT("/Script/Engine.CurveFloat")));
		TestFalse(TEXT("generic row cannot create Blueprints"),
			CanAssetDocumentEditorTargetLifecycleCreate(*Generic, TEXT("/Script/Engine.Blueprint")));
	}
	const auto* Montage = FindAssetDocumentEditorTargetLifecycle(TEXT("/Script/Engine.AnimMontage"));
	TestTrue(TEXT("Montage shares the class-neutral row"), Montage != nullptr && Montage->IsClassNeutral());
	TestTrue(TEXT("Montage is created by its editor factory"), Montage != nullptr &&
		CanAssetDocumentEditorTargetLifecycleCreate(*Montage, TEXT("/Script/Engine.AnimMontage")));
	TestNull(TEXT("lifecycle id must match the selected row"),
		FindAssetDocumentEditorTargetLifecycle(TEXT("/Script/Engine.AnimMontage"), TEXT("animation_montage_exact.v1")));
	return true;
}

// Pins the CurveFloat key gap: the curve data is serialized but not
// Details-editable, so the generic property admission excludes it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericFactoryCurveFloatSurfaceTest,
	"UEAssetDoc.GenericProvider.GenericFactory.CurveFloatSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericFactoryCurveFloatSurfaceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TArray<FString> Admitted;
	for (const FAssetDocumentEditorPropertyBatchTokenFieldView& Field :
		GetAssetDocumentEditorAdmittedPropertyFields(*UCurveFloat::StaticClass()))
	{
		Admitted.Add(Field.Property->GetAuthoredName());
	}
	AddInfo(FString::Printf(TEXT("CurveFloat admitted properties: [%s]"), *FString::Join(Admitted, TEXT(", "))));
	for (const TCHAR* Name : {TEXT("FloatCurve"), TEXT("bIsEventCurve")})
	{
		FProperty* Property = FindFProperty<FProperty>(UCurveFloat::StaticClass(), Name);
		if (!TestNotNull(*FString::Printf(TEXT("%s is reflected"), Name), Property))
		{
			continue;
		}
		TestFalse(*FString::Printf(TEXT("%s is not Details-editable (no CPF_Edit)"), Name), Property->HasAnyPropertyFlags(CPF_Edit));
		TestFalse(*FString::Printf(TEXT("%s is not admitted"), Name), Admitted.Contains(Name));
		const FAssetDocumentEditorTypedValueCodecResult Codec = InspectAssetDocumentEditorTypedValueProperty(*Property);
		AddInfo(FString::Printf(TEXT("%s codec: ok=%d id=%s diagnostic=%s"), Name, Codec.bSucceeded ? 1 : 0, *Codec.CodecId, *Codec.DiagnosticCode));
	}
	for (TFieldIterator<FProperty> It(FRichCurve::StaticStruct()); It; ++It)
	{
		AddInfo(FString::Printf(TEXT("FRichCurve member %s transient=%d edit=%d"), *It->GetName(),
			It->HasAnyPropertyFlags(CPF_Transient) ? 1 : 0, It->HasAnyPropertyFlags(CPF_Edit) ? 1 : 0));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericFactoryTransientInstanceTest,
	"UEAssetDoc.GenericProvider.GenericFactory.TransientInstance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericFactoryTransientInstanceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UCurveFactory* const Default = GetMutableDefault<UCurveFloatFactory>();
	const UClass* const DefaultCurveClass = Default->CurveClass;
	const TStrongObjectPtr<UFactory> Factory =
		NewAssetDocumentEditorGenericFactoryInstance(*UCurveFloatFactory::StaticClass());
	if (!TestTrue(TEXT("factory instance created"), Factory.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("instance is not the CDO"), Factory.Get() != Default);
	TestTrue(TEXT("instance lives in the transient package"), Factory->GetOuter() == GetTransientPackage());
	TestTrue(TEXT("instance is transient"), Factory->HasAnyFlags(RF_Transient));
	CastChecked<UCurveFactory>(Factory.Get())->CurveClass = nullptr;
	TestTrue(TEXT("configuring the instance leaves the CDO untouched"), Default->CurveClass == DefaultCurveClass);
	TestFalse(TEXT("abstract factory classes are refused"),
		NewAssetDocumentEditorGenericFactoryInstance(*UFactory::StaticClass()).IsValid());
	TestFalse(TEXT("non-factory classes are refused"),
		NewAssetDocumentEditorGenericFactoryInstance(*UCurveFloat::StaticClass()).IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericFactoryScratchTest,
	"UEAssetDoc.GenericProvider.GenericFactory.Scratch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericFactoryScratchTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericFactoryTests;
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider = CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Create = FindDescriptor(*Provider, TEXT("editor.scratch.create_by_factory.v1"));
	const FAssetDocumentPrimitiveDescriptor* Clone = FindDescriptor(*Provider, TEXT("editor.scratch.clone_asset.v2"));
	if (!TestNotNull(TEXT("create scratch descriptor"), Create) || !TestNotNull(TEXT("clone scratch descriptor"), Clone))
	{
		return false;
	}
	TestEqual(TEXT("clone scratch is version 2"), Clone->Version, 2u);
	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);

	TArray<UClass*> Classes = {UCurveFloat::StaticClass()};
	if (UClass* const DataAsset = FSoftClassPath(TestDataAssetClassPath).ResolveClass()) { Classes.Add(DataAsset); }
	if (UClass* const BehaviorTree = LoadBehaviorTreeClass()) { Classes.Add(BehaviorTree); }
	for (UClass* const Class : Classes)
	{
		const FAssetDocumentPrimitiveOutcome Outcome =
			InvokeWithClass(*Create, *Class, nullptr, Context, Handles, nullptr);
		for (const auto& Diagnostic : Outcome.Diagnostics) { AddInfo(Diagnostic.Code + TEXT(": ") + Diagnostic.Message); }
		if (!TestEqual(*FString::Printf(TEXT("%s scratch succeeds"), *Class->GetName()),
			Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
		{
			continue;
		}
		TestEqual(TEXT("scratch evidence names the lifecycle"), ReadStringField(Outcome.Value, TEXT("lifecycle_id")),
			FString(AssetDocumentEditorGenericFactoryLifecycleId));
		TestEqual(TEXT("scratch evidence names the exact class"), ReadStringField(Outcome.Value, TEXT("canonical_class")),
			Class->GetPathName());
		const FAssetDocumentOpaqueHandle* Handle = FindSingleHandle(Outcome, TEXT("scratch"));
		UObject* const Scratch = Handle != nullptr ? ResolveScratch(Handles, Context, *Handle) : nullptr;
		TestTrue(TEXT("scratch is an exact transient instance"), Scratch != nullptr && Scratch->GetClass() == Class
			&& Scratch->GetOutermost() == GetTransientPackage() && Scratch->HasAnyFlags(RF_Transient)
			&& !Scratch->HasAnyFlags(RF_Public | RF_Standalone));
	}
	const auto Blueprint = InvokeWithClass(*Create, *UBlueprint::StaticClass(), nullptr, Context, Handles, nullptr);
	TestTrue(TEXT("Blueprint scratch is refused"), Blueprint.Status == EAssetDocumentPrimitiveOutcomeStatus::Failed
		&& HasDiagnostic(Blueprint, TEXT("FactoryCreate.BlueprintClassUnsupported")));
	const auto NoFactory = InvokeWithClass(*Create, *UStaticMesh::StaticClass(), nullptr, Context, Handles, nullptr);
	TestTrue(TEXT("class without a factory is refused"), HasDiagnostic(NoFactory, TEXT("FactoryCreate.NoFactory")));
	FAssetDocumentRuntimeValueDto Unexpected = MakeLocation(TEXT("/Game/X"), TEXT("X"));
	const auto Extra = InvokeWithClass(*Create, *UCurveFloat::StaticClass(), &Unexpected, Context, Handles, nullptr);
	TestEqual(TEXT("scratch takes no value arguments"), Extra.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);

	// Clone v2 previews any existing non-Blueprint asset as a transient duplicate.
	TStrongObjectPtr<UCurveFloat> Source(NewObject<UCurveFloat>(GetTransientPackage(), NAME_None, RF_Transient));
	Source->FloatCurve.AddKey(0.f, 1.f);
	Source->FloatCurve.AddKey(2.f, 3.f);
	FAssetDocumentIssuedHandleResult SourceHandle = Handles.Issue(EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::ReadOnly, MakeAssetDocumentEditorResolvedAssetPayload(*Source));
	FAssetDocumentPrimitiveInvocation CloneInvocation = MakeAssetDocumentEditorProviderTestInvocation(*Clone, Context);
	CloneInvocation.HandleArguments.Add({TEXT("source_asset"), SourceHandle.Handle.GetValue()});
	const FAssetDocumentPrimitiveOutcome Cloned = Provider->Invoke(CloneInvocation, Handles, nullptr);
	for (const auto& Diagnostic : Cloned.Diagnostics) { AddInfo(Diagnostic.Code + TEXT(": ") + Diagnostic.Message); }
	if (TestEqual(TEXT("clone scratch succeeds"), Cloned.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
	{
		const FAssetDocumentOpaqueHandle* Handle = FindSingleHandle(Cloned, TEXT("scratch"));
		const UCurveFloat* const Scratch = Handle != nullptr ? Cast<UCurveFloat>(ResolveScratch(Handles, Context, *Handle)) : nullptr;
		TestTrue(TEXT("clone is a distinct transient copy with the source keys"), Scratch != nullptr
			&& Scratch != Source.Get() && Scratch->HasAnyFlags(RF_Transient)
			&& Scratch->FloatCurve.GetNumKeys() == 2 && Scratch->FloatCurve.Eval(2.f) == 3.f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericFactoryCreateAssetTest,
	"UEAssetDoc.GenericProvider.GenericFactory.CreateAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericFactoryCreateAssetTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericFactoryTests;
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider = CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Create = FindDescriptor(*Provider, TEXT("editor.asset.create_by_factory.v2"));
	const FAssetDocumentPrimitiveDescriptor* Rollback = FindDescriptor(*Provider, TEXT("editor.asset.rollback_created.v1"));
	if (!TestNotNull(TEXT("create descriptor"), Create) || !TestNotNull(TEXT("rollback descriptor"), Rollback))
	{
		return false;
	}
	TestEqual(TEXT("create is version 2"), Create->Version, 2u);
	TArray<TPair<UClass*, const TCHAR*>> Classes = {{UCurveFloat::StaticClass(), TEXT("Curve")}};
	if (UClass* const DataAsset = FSoftClassPath(TestDataAssetClassPath).ResolveClass()) { Classes.Add({DataAsset, TEXT("DA")}); }
	if (UClass* const BehaviorTree = LoadBehaviorTreeClass()) { Classes.Add({BehaviorTree, TEXT("BT")}); }
	TestEqual(TEXT("all three probe classes load"), Classes.Num(), 3);
	for (const TPair<UClass*, const TCHAR*>& Entry : Classes)
	{
		const FTargetPaths Paths(Entry.Value);
		ON_SCOPE_EXIT { DeleteTarget(Paths); };
		FAssetDocumentEditorProviderTestContext Context;
		FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
		FAssetDocumentEditorProviderTestTransaction Transaction(Context);
		const FAssetDocumentRuntimeValueDto Location = MakeLocation(Paths.PackagePath, Paths.AssetName);
		const FAssetDocumentPrimitiveOutcome Created =
			InvokeWithClass(*Create, *Entry.Key, &Location, Context, Handles, &Transaction);
		for (const auto& Diagnostic : Created.Diagnostics) { AddInfo(Diagnostic.Code + TEXT(": ") + Diagnostic.Message); }
		if (!TestEqual(*FString::Printf(TEXT("%s create succeeds"), *Entry.Key->GetName()),
			Created.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
		{
			continue;
		}
		UObject* const Asset = FSoftObjectPath(Paths.ObjectPath).ResolveObject();
		TestTrue(TEXT("created asset has the exact class"), Asset != nullptr && Asset->GetClass() == Entry.Key);
		TestTrue(TEXT("created asset is registered"),
			FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(Paths.ObjectPath)).IsValid());
		TestFalse(TEXT("creation enlists a footprint"), Transaction.GetEnlistedFootprint().IsEmpty());
		const FAssetDocumentOpaqueHandle* Mutable = FindSingleHandle(Created, TEXT("asset_mutable"));
		const FAssetDocumentOpaqueHandle* Package = FindSingleHandle(Created, TEXT("package"));
		if (!TestNotNull(TEXT("mutable handle"), Mutable) || !TestNotNull(TEXT("package handle"), Package))
		{
			continue;
		}
		// The generic creation record is accepted by the class-neutral created-asset rollback.
		FAssetDocumentPrimitiveInvocation Invocation = MakeAssetDocumentEditorProviderTestInvocation(*Rollback, Context);
		Invocation.HandleArguments.Add({TEXT("asset"), *Mutable});
		Invocation.HandleArguments.Add({TEXT("package"), *Package});
		const FAssetDocumentPrimitiveOutcome RolledBack = Provider->Invoke(Invocation, Handles, &Transaction);
		for (const auto& Diagnostic : RolledBack.Diagnostics) { AddInfo(Diagnostic.Code + TEXT(": ") + Diagnostic.Message); }
		TestEqual(TEXT("rollback_created deletes the generic creation"), RolledBack.Status,
			EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
		TestTargetAbsent(*this, Paths, TEXT("after rollback"));
	}

	auto Refused = [&](UClass& Class, const TCHAR* Code)
	{
		const FTargetPaths Paths(TEXT("Refused"));
		ON_SCOPE_EXIT { DeleteTarget(Paths); };
		FAssetDocumentEditorProviderTestContext Context;
		FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
		FAssetDocumentEditorProviderTestTransaction Transaction(Context);
		const FAssetDocumentRuntimeValueDto Location = MakeLocation(Paths.PackagePath, Paths.AssetName);
		const auto Outcome = InvokeWithClass(*Create, Class, &Location, Context, Handles, &Transaction);
		TestTrue(*FString::Printf(TEXT("%s refused with %s"), *Class.GetName(), Code),
			Outcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Failed && HasDiagnostic(Outcome, Code));
		TestTrue(TEXT("refusal enlists nothing"), Transaction.GetEnlistedFootprint().IsEmpty());
		TestTargetAbsent(*this, Paths, Code);
	};
	Refused(*UBlueprint::StaticClass(), TEXT("FactoryCreate.BlueprintClassUnsupported"));
	Refused(*UCurveBase::StaticClass(), TEXT("FactoryCreate.AbstractClass"));
	Refused(*UStaticMesh::StaticClass(), TEXT("FactoryCreate.NoFactory"));
	return true;
}

// The Blueprint catalog row declares editor.asset.rollback_created.v1 as its
// rollback primitive, so a Blueprint creation record (blueprint_factory.v1) must
// be accepted by it just like the generic factory records.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintCreationRollbackCreatedTest,
	"UEAssetDoc.GenericProvider.GenericFactory.BlueprintRollbackCreated",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBlueprintCreationRollbackCreatedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericFactoryTests;
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider = CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Create = FindDescriptor(*Provider, TEXT("editor.blueprint.asset.create.v1"));
	const FAssetDocumentPrimitiveDescriptor* Rollback = FindDescriptor(*Provider, TEXT("editor.asset.rollback_created.v1"));
	if (!TestNotNull(TEXT("blueprint create descriptor"), Create) || !TestNotNull(TEXT("rollback descriptor"), Rollback))
	{
		return false;
	}
	const FTargetPaths Paths(TEXT("BP_RollbackCreated"));
	ON_SCOPE_EXIT { DeleteTarget(Paths); };
	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	FAssetDocumentEditorProviderTestTransaction Transaction(Context);
	const FAssetDocumentRuntimeValueDto Location = MakeLocation(Paths.PackagePath, Paths.AssetName);
	FAssetDocumentPrimitiveInvocation CreateInvocation = MakeAssetDocumentEditorProviderTestInvocation(*Create, Context);
	CreateInvocation.ValueArguments = Location;
	FAssetDocumentIssuedHandleResult AssetClass = Handles.Issue(
		EAssetDocumentHandleKind::Class, EAssetDocumentHandleAccess::ReadOnly, MakeUnique<FClassPayload>(*UBlueprint::StaticClass()));
	FAssetDocumentIssuedHandleResult ParentClass = Handles.Issue(
		EAssetDocumentHandleKind::Class, EAssetDocumentHandleAccess::ReadOnly, MakeUnique<FClassPayload>(*AActor::StaticClass()));
	CreateInvocation.HandleArguments.Add({TEXT("asset_class"), AssetClass.Handle.GetValue()});
	CreateInvocation.HandleArguments.Add({TEXT("parent_class"), ParentClass.Handle.GetValue()});
	const FAssetDocumentPrimitiveOutcome Created = Provider->Invoke(CreateInvocation, Handles, &Transaction);
	for (const auto& Diagnostic : Created.Diagnostics) { AddInfo(Diagnostic.Code + TEXT(": ") + Diagnostic.Message); }
	if (!TestEqual(TEXT("blueprint create succeeds"), Created.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
	{
		return false;
	}
	const FAssetDocumentOpaqueHandle* Mutable = FindSingleHandle(Created, TEXT("asset_mutable"));
	const FAssetDocumentOpaqueHandle* Package = FindSingleHandle(Created, TEXT("package"));
	if (!TestNotNull(TEXT("mutable handle"), Mutable) || !TestNotNull(TEXT("package handle"), Package))
	{
		return false;
	}
	FAssetDocumentPrimitiveInvocation Invocation = MakeAssetDocumentEditorProviderTestInvocation(*Rollback, Context);
	Invocation.HandleArguments.Add({TEXT("asset"), *Mutable});
	Invocation.HandleArguments.Add({TEXT("package"), *Package});
	const FAssetDocumentPrimitiveOutcome RolledBack = Provider->Invoke(Invocation, Handles, &Transaction);
	for (const auto& Diagnostic : RolledBack.Diagnostics) { AddInfo(Diagnostic.Code + TEXT(": ") + Diagnostic.Message); }
	TestEqual(TEXT("rollback_created deletes the Blueprint creation"), RolledBack.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	TestTargetAbsent(*this, Paths, TEXT("after Blueprint rollback"));
	return true;
}
#endif
