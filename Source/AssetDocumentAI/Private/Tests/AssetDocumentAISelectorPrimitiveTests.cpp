// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetDocumentAIPrimitiveProvider.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "IDetailTreeNode.h"
#include "IPropertyRowGenerator.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace AssetDocumentAISelectorPrimitiveTests
{
constexpr const TCHAR* PrimitiveId = TEXT("ai.behavior_tree.selector_inspect_evidence.v1");
constexpr const TCHAR* SelectorClassPath =
	TEXT("/Script/AssetDocumentTests.AssetDocumentSelectorTaskTestNode");

class FUnusedHandleResolver final : public IAssetDocumentHandleResolver
{
public:
	virtual FAssetDocumentIssuedHandleResult Issue(
		EAssetDocumentHandleKind,
		EAssetDocumentHandleAccess,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&&) override
	{
		return {};
	}

	virtual FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(
		const FAssetDocumentOpaqueHandle&,
		const FAssetDocumentHandleValidationContext&) override
	{
		return {};
	}

	virtual FAssetDocumentHandleInvalidationResult Invalidate(const FAssetDocumentHandleInvalidation&) override
	{
		return {};
	}
};

// The shared descriptor validator checks presence, not the installed Engine version.
// This exact evidence predicate is the Q574 manifest acceptance gate.
bool HasStock574Evidence(const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	return ValidateAssetDocumentPrimitiveDescriptor(Descriptor).bValid
		&& Descriptor.Admission.ExactEngineVersion == TEXT("5.7.4")
		&& Descriptor.Admission.AuthorityFingerprint ==
			TEXT("ue-5.7.4|ai|propertyeditor.selector-inspect-evidence.v1|public-row-generator-property-handle");
}

FAssetDocumentRuntimeValueDto StringValue(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto ObjectValue(
	TArray<FString> Keys,
	TArray<FAssetDocumentRuntimeValueDto> Elements)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Object;
	Result.ObjectKeys = MoveTemp(Keys);
	Result.Elements = MoveTemp(Elements);
	return Result;
}

FAssetDocumentPrimitiveInvocation ClassDefaultsInvocation(
	FString ExactClassPath = SelectorClassPath)
{
	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = PrimitiveId;
	Invocation.Version = 1;
	Invocation.RequestId = FGuid::NewGuid();
	Invocation.RuntimeRevision = 1;
	Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
	Invocation.ValueArguments = ObjectValue(
		{TEXT("mode"), TEXT("exact_asset_class_path")},
		{StringValue(TEXT("class_defaults")), StringValue(MoveTemp(ExactClassPath))});
	return Invocation;
}

FAssetDocumentPrimitiveInvocation LiveInvocation(
	const FString& ExactClassPath,
	const FString& ExactObjectPath)
{
	FAssetDocumentPrimitiveInvocation Invocation = ClassDefaultsInvocation();
	Invocation.ValueArguments = ObjectValue(
		{TEXT("mode"), TEXT("exact_asset_class_path"), TEXT("exact_asset_object_path")},
		{StringValue(TEXT("live_asset")), StringValue(ExactClassPath), StringValue(ExactObjectPath)});
	return Invocation;
}

const FAssetDocumentRuntimeValueDto* FindObjectField(
	const FAssetDocumentRuntimeValueDto& Object,
	const TCHAR* Key)
{
	for (int32 Index = 0; Index < Object.ObjectKeys.Num(); ++Index)
	{
		if (Object.ObjectKeys[Index] == Key)
		{
			return &Object.Elements[Index];
		}
	}
	return nullptr;
}

const FAssetDocumentRuntimeDiagnostic* FindDiagnostic(
	const FAssetDocumentPrimitiveOutcome& Outcome,
	const TCHAR* Code)
{
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Outcome.Diagnostics)
	{
		if (Diagnostic.Code == Code)
		{
			return &Diagnostic;
		}
	}
	return nullptr;
}

const FAssetDocumentRuntimeValueDto* FindSelector(
	const FAssetDocumentPrimitiveOutcome& Outcome,
	const TCHAR* PropertyPath)
{
	const FAssetDocumentRuntimeValueDto* Selectors = FindObjectField(Outcome.Value, TEXT("selectors"));
	if (!Selectors || Selectors->Kind != EAssetDocumentRuntimeValueKind::Array)
	{
		return nullptr;
	}
	for (const FAssetDocumentRuntimeValueDto& Candidate : Selectors->Elements)
	{
		const FAssetDocumentRuntimeValueDto* Path = FindObjectField(Candidate, TEXT("property_path"));
		if (Path && Path->Kind == EAssetDocumentRuntimeValueKind::String
			&& Path->StringValue == PropertyPath)
		{
			return &Candidate;
		}
	}
	return nullptr;
}

TArray<FString> FindSelectorPathsWithPrefix(
	const FAssetDocumentPrimitiveOutcome& Outcome,
	const TCHAR* Prefix)
{
	TArray<FString> Paths;
	const FAssetDocumentRuntimeValueDto* Selectors = FindObjectField(Outcome.Value, TEXT("selectors"));
	if (!Selectors || Selectors->Kind != EAssetDocumentRuntimeValueKind::Array)
	{
		return Paths;
	}
	for (const FAssetDocumentRuntimeValueDto& Candidate : Selectors->Elements)
	{
		const FAssetDocumentRuntimeValueDto* Path = FindObjectField(Candidate, TEXT("property_path"));
		if (Path && Path->Kind == EAssetDocumentRuntimeValueKind::String
			&& Path->StringValue.StartsWith(Prefix))
		{
			Paths.Add(Path->StringValue);
		}
	}
	Paths.Sort();
	return Paths;
}

TSharedPtr<IPropertyHandle> FindPropertyHandle(
	const TArray<TSharedRef<IDetailTreeNode>>& Nodes,
	const FName PropertyName)
{
	for (const TSharedRef<IDetailTreeNode>& Node : Nodes)
	{
		const TSharedPtr<IPropertyHandle> Handle = Node->CreatePropertyHandle();
		if (Handle.IsValid() && Handle->GetProperty()
			&& Handle->GetProperty()->GetFName() == PropertyName)
		{
			return Handle;
		}
		TArray<TSharedRef<IDetailTreeNode>> Children;
		Node->GetChildren(Children, true);
		if (const TSharedPtr<IPropertyHandle> Child = FindPropertyHandle(Children, PropertyName))
		{
			return Child;
		}
	}
	return nullptr;
}

bool PopulateNamedLiveMapEntry(UObject& Subject, FString& OutFailure)
{
	FPropertyRowGeneratorArgs Args;
	FPropertyEditorModule& PropertyEditor =
		FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	const TSharedRef<IPropertyRowGenerator> Generator = PropertyEditor.CreatePropertyRowGenerator(Args);
	Generator->OnRowsRefreshed().AddLambda([]() {});
	Generator->SetObjects({&Subject});
	const TSharedPtr<IPropertyHandle> MapHandle =
		FindPropertyHandle(Generator->GetRootTreeNodes(), TEXT("MapValues"));
	const TSharedPtr<IPropertyHandleMap> Map = MapHandle.IsValid() ? MapHandle->AsMap() : nullptr;
	if (!Map.IsValid())
	{
		OutFailure = TEXT("map property handle unavailable");
		return false;
	}
	const FPropertyHandleItemAddResult MapEntry = Map->AddItem();
	if (MapEntry.GetAccessResult() != FPropertyAccess::Success)
	{
		OutFailure = TEXT("map AddItem failed");
		return false;
	}
	const TSharedRef<IPropertyHandle> MapElement = Map->GetElement(MapEntry.GetIndex());
	const TSharedPtr<IPropertyHandle> MapKey = MapElement->GetKeyHandle();
	if (!MapKey.IsValid())
	{
		OutFailure = TEXT("map key handle unavailable");
		return false;
	}
	if (MapKey->SetValue(FName(TEXT("NamedMapKey"))) != FPropertyAccess::Success)
	{
		OutFailure = TEXT("map key SetValue failed");
		return false;
	}
	return true;
}

bool HasFailure(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveOutcome& Outcome,
	const TCHAR* Code)
{
	Test.TestEqual(TEXT("invalid selector invocation fails"), Outcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Test.TestNotNull(*FString::Printf(TEXT("failure has %s"), Code), FindDiagnostic(Outcome, Code));
	Test.TestTrue(TEXT("failed selector invocation has no handle results"),
		Outcome.HandleResults.IsEmpty());
	Test.TestTrue(TEXT("failed selector invocation has no changed set"),
		Outcome.ChangedSet.IsEmpty());
	return true;
}

} // namespace AssetDocumentAISelectorPrimitiveTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAISelectorPrimitiveTest,
	"UEAssetDoc.AI.SelectorEvidence.Primitive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAISelectorPrimitiveTest::RunTest(const FString&)
{
	using namespace AssetDocumentAISelectorPrimitiveTests;

	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentAIPrimitiveProvider();
	TestEqual(TEXT("AI provider id"), Provider->GetProviderId(), FString(TEXT("ai")));
	const FAssetDocumentPrimitiveDescriptor* Descriptor = nullptr;
	for (const FAssetDocumentPrimitiveDescriptor& Candidate : Provider->DescribePrimitives())
	{
		if (Candidate.PrimitiveId == PrimitiveId && Candidate.Version == 1)
		{
			Descriptor = &Candidate;
			break;
		}
	}
	TestNotNull(TEXT("selector descriptor is resolved by exact id and version"), Descriptor);
	if (Descriptor == nullptr)
	{
		return false;
	}
	TestEqual(TEXT("selector primitive id"), Descriptor->PrimitiveId, FString(PrimitiveId));
	TestEqual(TEXT("selector primitive version"), Descriptor->Version, 1u);
	TestEqual(TEXT("selector primitive is read only"), Descriptor->Access,
		EAssetDocumentPrimitiveAccess::ReadOnly);
	TestTrue(TEXT("selector primitive has no handle arguments"),
		Descriptor->HandleArgumentSchemas.IsEmpty());
	TestTrue(TEXT("selector primitive has no handle results"),
		Descriptor->HandleResultSchemas.IsEmpty());
	TestEqual(TEXT("selector primitive only runs in Verify"), Descriptor->AllowedPhases,
		TArray<EAssetDocumentRuntimeRequestPhase>({EAssetDocumentRuntimeRequestPhase::Verify}));
	TestEqual(TEXT("selector primitive requires generic route"), Descriptor->RequiredConsumerIds,
		TArray<FString>({TEXT("route.generic.v1")}));

	TestTrue(TEXT("selector admission binds exact stock 5.7.4 evidence"), HasStock574Evidence(*Descriptor));
	FAssetDocumentPrimitiveDescriptor StaleEvidence = *Descriptor;
	StaleEvidence.Admission.ExactEngineVersion = TEXT("5.7.3");
	TestFalse(TEXT("Q574 manifest rejects stale Engine evidence"), HasStock574Evidence(StaleEvidence));
	StaleEvidence = *Descriptor;
	StaleEvidence.Admission.AuthorityFingerprint =
		TEXT("ue-5.7.3|ai|propertyeditor.selector-inspect-evidence.v1|public-row-generator-property-handle");
	TestFalse(TEXT("Q574 manifest rejects stale fingerprint"), HasStock574Evidence(StaleEvidence));

	FUnusedHandleResolver Handles;
	const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
		ClassDefaultsInvocation(), Handles, nullptr);
	TestEqual(TEXT("class-default selector evidence succeeds"), Outcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	TestEqual(TEXT("class-default selector evidence has no handles"),
		Outcome.HandleResults.Num(), 0);
	TestEqual(TEXT("class-default selector evidence has no changed set"),
		Outcome.ChangedSet.Num(), 0);
	TestEqual(TEXT("class-default selector evidence has no side-effect diagnostics"),
		Outcome.Diagnostics.Num(), 0);
	const FAssetDocumentRuntimeValueDto* Subject = FindObjectField(Outcome.Value, TEXT("subject"));
	TestNotNull(TEXT("class-default selector evidence has a subject"), Subject);
	if (Subject)
	{
		TestNotNull(TEXT("class-default subject has kind"), FindObjectField(*Subject, TEXT("kind")));
		TestNotNull(TEXT("class-default subject has exact class"),
			FindObjectField(*Subject, TEXT("exact_class_path")));
		TestNull(TEXT("class-default subject omits asset path"),
			FindObjectField(*Subject, TEXT("asset_path")));
	}
	TestNotNull(TEXT("native direct selector evidence is present"),
		FindSelector(Outcome, TEXT("DirectSelector")));
	TestNotNull(TEXT("native nested selector schema is present"),
		FindSelector(Outcome, TEXT("NestedValue/Selector")));
	TestNotNull(TEXT("native array wildcard selector schema is present"),
		FindSelector(Outcome, TEXT("ArrayValues/*/Selector")));
	TestNotNull(TEXT("native map wildcard selector schema is present"),
		FindSelector(Outcome, TEXT("MapValues/*/Selector")));
	TestNotNull(TEXT("native set wildcard selector schema is present"),
		FindSelector(Outcome, TEXT("SetValues/*/Selector")));
	TestNotNull(TEXT("native instanced-struct selector schema is present"),
		FindSelector(Outcome, TEXT("DynamicValue/Properties/Selector")));
	const FString BlueprintPackageName = FString::Printf(
		TEXT("/Temp/AssetDocumentAI/SelectorPrimitive_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UPackage* BlueprintPackage = CreatePackage(*BlueprintPackageName);
	UClass* NativeClass = LoadClass<UObject>(nullptr, SelectorClassPath);
	UBlueprint* Blueprint = NativeClass && BlueprintPackage
		? FKismetEditorUtilities::CreateBlueprint(
			NativeClass,
			BlueprintPackage,
			FName(*FPackageName::GetLongPackageAssetName(BlueprintPackageName)),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass())
		: nullptr;
	TestNotNull(TEXT("test-owned Blueprint selector fixture is created"), Blueprint);
	if (Blueprint)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}
	UClass* BlueprintClass = Blueprint ? Blueprint->GeneratedClass : nullptr;
	TestNotNull(TEXT("test-owned Blueprint selector fixture compiles"), BlueprintClass);
	if (BlueprintClass)
	{
		const FAssetDocumentPrimitiveOutcome BlueprintOutcome = Provider->Invoke(
			ClassDefaultsInvocation(BlueprintClass->GetPathName()), Handles, nullptr);
		TestEqual(TEXT("Blueprint selector evidence succeeds"), BlueprintOutcome.Status,
			EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
		TestNotNull(TEXT("Blueprint selector evidence retains recursive wildcard"),
			FindSelector(BlueprintOutcome, TEXT("ArrayValues/*/Selector")));
	}
	const FAssetDocumentRuntimeValueDto* Optional = FindSelector(Outcome, TEXT("OptionalSelector"));
	TestNotNull(TEXT("optional selector evidence is present"), Optional);
	if (Optional)
	{
		const FAssetDocumentRuntimeValueDto* NoneAllowed = FindObjectField(*Optional, TEXT("none_allowed"));
		TestTrue(TEXT("optional selector admits none"), NoneAllowed
			&& NoneAllowed->Kind == EAssetDocumentRuntimeValueKind::Boolean
			&& NoneAllowed->BooleanValue);
	}
	const FAssetDocumentRuntimeValueDto* Direct = FindSelector(Outcome, TEXT("DirectSelector"));
	if (Direct)
	{
		const FAssetDocumentRuntimeValueDto* AllowedTypes = FindObjectField(*Direct, TEXT("allowed_types"));
		TestTrue(TEXT("direct selector has object filter evidence"), AllowedTypes
			&& AllowedTypes->Kind == EAssetDocumentRuntimeValueKind::Array
			&& AllowedTypes->Elements.Num() == 1);
		if (AllowedTypes && AllowedTypes->Elements.Num() == 1)
		{
			const FAssetDocumentRuntimeValueDto& AllowedType = AllowedTypes->Elements[0];
			const FAssetDocumentRuntimeValueDto* Filter = FindObjectField(AllowedType, TEXT("filter_type_path"));
			const FAssetDocumentRuntimeValueDto* Base = FindObjectField(AllowedType, TEXT("base_class_path"));
			TestTrue(TEXT("direct selector filter type is canonical"), Filter
				&& Filter->StringValue == TEXT("/Script/AIModule.BlackboardKeyType_Object"));
			TestTrue(TEXT("direct selector base class is canonical"), Base
				&& Base->StringValue == TEXT("/Script/Engine.Actor"));
		}
	}

	const FAssetDocumentPrimitiveOutcome Ordinary = Provider->Invoke(
		[&]()
		{
			FAssetDocumentPrimitiveInvocation Invocation = ClassDefaultsInvocation();
			Invocation.ValueArguments.Elements[1] = StringValue(TEXT("/Script/CoreUObject.Object"));
			return Invocation;
		}(), Handles, nullptr);
	TestEqual(TEXT("ordinary class selector evidence succeeds"), Ordinary.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	const FAssetDocumentRuntimeValueDto* OrdinarySelectors = FindObjectField(Ordinary.Value, TEXT("selectors"));
	TestTrue(TEXT("ordinary class has no selector policies"), OrdinarySelectors
		&& OrdinarySelectors->Kind == EAssetDocumentRuntimeValueKind::Array
		&& OrdinarySelectors->Elements.IsEmpty());

	UPackage* LivePackage = CreatePackage(TEXT("/Temp/AssetDocumentAI/SelectorPrimitive"));
	UObject* LiveObject = NativeClass && LivePackage
		? NewObject<UObject>(LivePackage, NativeClass, TEXT("LiveSelector"))
		: nullptr;
	TestNotNull(TEXT("test-owned live selector subject exists"), LiveObject);
	if (LiveObject)
	{
		FString MapPopulationFailure;
		const bool bPopulatedMap = PopulateNamedLiveMapEntry(*LiveObject, MapPopulationFailure);
		TestTrue(
			*FString::Printf(
				TEXT("public PropertyEditor populates a named live map entry (%s)"),
				*MapPopulationFailure),
			bPopulatedMap);
		const bool bPackageDirtyBeforeInvoke = LivePackage->IsDirty();
		const FAssetDocumentPrimitiveOutcome Live = Provider->Invoke(
			LiveInvocation(SelectorClassPath, LiveObject->GetPathName()), Handles, nullptr);
		TestEqual(TEXT("live selector evidence succeeds"), Live.Status,
			EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
		const FAssetDocumentRuntimeValueDto* LiveSubject = FindObjectField(Live.Value, TEXT("subject"));
		const FAssetDocumentRuntimeValueDto* LivePath = LiveSubject
			? FindObjectField(*LiveSubject, TEXT("asset_path")) : nullptr;
		TestTrue(TEXT("live selector subject keeps canonical asset path"), LivePath
			&& LivePath->StringValue == LiveObject->GetPathName());
		TestEqual(TEXT("live selector evidence does not dirty its package"),
			LivePackage->IsDirty(), bPackageDirtyBeforeInvoke);
		TestEqual(TEXT("live populated map exposes only its schema wildcard"),
			FindSelectorPathsWithPrefix(Live, TEXT("MapValues/")),
			TArray<FString>({TEXT("MapValues/*/Selector")}));

		HasFailure(*this, Provider->Invoke(
			LiveInvocation(TEXT("/Script/CoreUObject.Object"), LiveObject->GetPathName()), Handles, nullptr),
			TEXT("AISelector.ClassMismatch"));
	}

	FAssetDocumentPrimitiveInvocation InvalidKeys = ClassDefaultsInvocation();
	InvalidKeys.ValueArguments.ObjectKeys.Add(TEXT("unexpected"));
	InvalidKeys.ValueArguments.Elements.Add(StringValue(TEXT("value")));
	HasFailure(*this, Provider->Invoke(InvalidKeys, Handles, nullptr),
		TEXT("PrimitiveInvocation.InvalidValueShape"));
	FAssetDocumentPrimitiveInvocation InvalidMode = ClassDefaultsInvocation();
	InvalidMode.ValueArguments.Elements[0] = StringValue(TEXT("unknown_mode"));
	HasFailure(*this, Provider->Invoke(InvalidMode, Handles, nullptr),
		TEXT("PrimitiveInvocation.InvalidValueShape"));
	FAssetDocumentPrimitiveInvocation NonCanonicalClass = ClassDefaultsInvocation();
	NonCanonicalClass.ValueArguments.Elements[1] = StringValue(TEXT("AssetDocumentSelectorTaskTestNode"));
	HasFailure(*this, Provider->Invoke(NonCanonicalClass, Handles, nullptr),
		TEXT("AISelector.NonCanonicalSubject"));
	HasFailure(*this, Provider->Invoke(
		LiveInvocation(SelectorClassPath, TEXT("not-a-canonical-object-path")), Handles, nullptr),
		TEXT("AISelector.NonCanonicalSubject"));
	FAssetDocumentPrimitiveInvocation Unavailable = ClassDefaultsInvocation();
	Unavailable.ValueArguments.Elements[1] = StringValue(TEXT("/Script/AssetDocument.MissingSelectorSubject"));
	AddExpectedError(TEXT("MissingSelectorSubject"), EAutomationExpectedErrorFlags::Contains, 1);
	HasFailure(*this, Provider->Invoke(Unavailable, Handles, nullptr),
		TEXT("AISelector.SubjectUnavailable"));
	FAssetDocumentPrimitiveInvocation WithHandle = ClassDefaultsInvocation();
	WithHandle.HandleArguments.Add({TEXT("unexpected"), {}});
	HasFailure(*this, Provider->Invoke(WithHandle, Handles, nullptr),
		TEXT("PrimitiveInvocation.InvalidHandleArguments"));
	if (BlueprintPackage)
	{
		BlueprintPackage->MarkAsGarbage();
	}
	return true;
}

#endif
