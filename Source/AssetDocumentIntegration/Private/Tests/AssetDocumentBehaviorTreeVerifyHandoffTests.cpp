// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#if WITH_DEV_AUTOMATION_TESTS
#include "AssetDocumentGenericReadOnlyDispatcher.h"
#include "AssetDocumentRequiredPrimitiveProviderView.h"
#include "AssetDocumentAIPrimitiveProvider.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "AssetDocumentPythonRuntime.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Engine.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "AssetDocumentPluginPaths.h"

namespace AssetDocumentBehaviorTreeVerifyHandoffTests
{
constexpr uint64 Revision = 51;
constexpr TCHAR ReadbackId[] = TEXT("ai.behavior_tree.authored_graph.readback.v1");

// Observation only: all operations forward to the real runtime/context. No
// provider, descriptor, resolver, handle or transaction is substituted.
struct FObservation
{
	FAssetDocumentRequestContextMetadata Metadata;
	FGuid TransactionId;
	bool bActiveTransaction = false;
	TArray<FString> Calls;
	TArray<FAssetDocumentPrimitiveOutcome> Outcomes;
	TOptional<FAssetDocumentOpaqueHandle> Source;
	TOptional<FAssetDocumentOpaqueHandle> Alias;
};
class FObservedContext final : public IAssetDocumentNativeRequestContext
{
public:
	FObservedContext(TUniquePtr<IAssetDocumentNativeRequestContext> InInner, FObservation& InObserved)
		: Inner(MoveTemp(InInner)), Observed(InObserved) {}
	const FAssetDocumentRequestContextMetadata& GetMetadata() const override { return Inner->GetMetadata(); }
	FAssetDocumentPrimitiveRegistrySnapshot InspectCatalog() const override { return Inner->InspectCatalog(); }
	FAssetDocumentPrimitiveOutcome Invoke(const FAssetDocumentPrimitiveInvocation& Invocation) override
	{
		Observed.Calls.Add(Invocation.PrimitiveId);
		auto Result = Inner->Invoke(Invocation);
		Observed.Outcomes.Add(Result);
		return Result;
	}
	FAssetDocumentIssuedHandleResult Handoff(const FAssetDocumentOpaqueHandle& Source,
		FStringView Target, EAssetDocumentHandleAccess Access) override
	{
		Observed.Calls.Add(TEXT("handoff"));
		Observed.Source = Source;
		auto Result = Inner->Handoff(Source, Target, Access);
		Observed.Alias = Result.Handle;
		return Result;
	}
private:
	TUniquePtr<IAssetDocumentNativeRequestContext> Inner;
	FObservation& Observed;
};
class FObservedRuntime final : public IAssetDocumentPythonRuntime
{
public:
	explicit FObservedRuntime(const FAssetDocumentPythonRuntimeConfiguration& Config)
		: Inner(CreateAssetDocumentPythonRuntime(Config)) {}
	~FObservedRuntime() { Inner->ShutdownPythonBinding(); }
	FAssetDocumentContractValidationResult InitializePythonBinding() override { return Inner->InitializePythonBinding(); }
	FAssetDocumentPythonRevisionActivationResult TryActivateRevision(const FAssetDocumentPythonRevisionCandidate& Candidate) override { return Inner->TryActivateRevision(Candidate); }
	uint64 GetActiveRuntimeRevision() const override { return Inner->GetActiveRuntimeRevision(); }
	bool HasActiveRequests() const override { return Inner->HasActiveRequests(); }
	IAssetDocumentPythonRuntimeHost& GetHost() override { return Inner->GetHost(); }
	const IAssetDocumentPrimitiveRegistry* GetActivePrimitiveRegistry() const override { return Inner->GetActivePrimitiveRegistry(); }
	FAssetDocumentPythonRequestContextCreateResult CreateRequestContext(const FAssetDocumentRequestContextMetadata& Metadata,
		const FGuid& TransactionId, IAssetDocumentAtomicTransaction* Transaction) override
	{
		Observed = {};
		Observed.Metadata = Metadata;
		Observed.TransactionId = TransactionId;
		Observed.bActiveTransaction = Transaction != nullptr;
		auto Result = Inner->CreateRequestContext(Metadata, TransactionId, Transaction);
		if (Result.Context) Result.Context = MakeUnique<FObservedContext>(MoveTemp(Result.Context), Observed);
		return Result;
	}
	FAssetDocumentHandleInvalidationResult InvalidateHandles(const FAssetDocumentHandleInvalidation& Event) override { return Inner->InvalidateHandles(Event); }
	void ShutdownPythonBinding() override { Inner->ShutdownPythonBinding(); }
	FObservation Observed;
private:
	TUniquePtr<IAssetDocumentPythonRuntime> Inner;
};

// The module lives in a test-owned temporary copy of the unchanged Python
// package. Its globals deliberately retain proxies to probe dispatch-close.
const TCHAR* Callback = TEXT(R"PY(
from asset_document.dto import PrimitiveRequirement, RuntimePhase, _diagnostic_native_projection
from asset_document.regions.primitive_catalog import (_NativePrimitiveContext,
    NamedOpaqueHandleArgument, PrimitiveOutcomeStatus, _runtime_to_native)
from asset_document.diagnostics import AssetDocumentContractError
saved = None

def result(request, value=None, diagnostics=()):
    return dict(abi_version=1, request_id=request['request_id'],
        terminal_status=1 if diagnostics else 0, final_phase=7 if diagnostics else 6,
        runtime_revision=request['expected_runtime_revision'], diagnostics=diagnostics,
        canonical_value=_runtime_to_native(value), canonical_bytes=b'',
        semantic_hash='', codec_version=0, changed_set=())

def dispatch(request, native):
    global saved
    assert native.request_id == request['request_id']
    assert native.runtime_revision == request['expected_runtime_revision'] == 51
    assert native.phase == 5
    context = _NativePrimitiveContext(native, RuntimePhase.VERIFY)
    mode = request['canonical_document']['mode']
    if mode == 'closed':
        old, alias = saved
        checks = []
        for label, operation, code in (
            ('context', lambda: old.invoke(requirement=PrimitiveRequirement('ai.behavior_tree.authored_graph.readback.v1', 1, 'ai_profile'), value_arguments={'behavior_tree_path':request['target_asset_path']}, handle_arguments=(NamedOpaqueHandleArgument('asset', alias),)), 'AD.RUNTIME.PRIMITIVE_INVOCATION_REJECTED'),
            ('handoff', lambda: old.handoff(source_handle=alias, target_provider_id='editor', access='read_only'), 'AD.RUNTIME.HANDOFF_INVALID_CONTEXT'),
            ('cross_binding', lambda: context.handoff(source_handle=alias, target_provider_id='editor', access='read_only'), 'AD.RUNTIME.STALE_HANDLE')):
            try:
                operation()
            except (RuntimeError, AssetDocumentContractError) as error:
                actual = error.diagnostic.code if isinstance(error, AssetDocumentContractError) else str(error).partition(': ')[0]
                assert actual == code, (label, actual, code)
                checks.append(label)
            else:
                raise AssertionError(label + ' escaped closed binding')
        saved = None
        return result(request, checks)
    resolved = context.invoke(requirement=PrimitiveRequirement('editor.asset.resolve_existing.v1', 1, 'route.generic.v1'),
        value_arguments={'path':request['target_asset_path'], 'expected_class':'/Script/AIModule.BlackboardData' if mode == 'wrong_class' else '/Script/AIModule.BehaviorTree'}, handle_arguments=())
    if resolved.status != PrimitiveOutcomeStatus.SUCCEEDED:
        return result(request, diagnostics=tuple(_diagnostic_native_projection(d) for d in resolved.diagnostics))
    if not resolved.value['exists']:
        assert {group.name for group in resolved.handle_results} == {'asset_read', 'asset_mutable', 'package'}
        assert all(not group.handles for group in resolved.handle_results)
        return result(request, diagnostics=({'path':'/A5S/Resolve', 'code':'A5S.TargetMissing', 'message':'Exact fixture is absent; no readback attempted.', 'severity':2, 'context':()},))
    assert resolved.value['canonical_path'] == request['target_asset_path']
    assert resolved.value['canonical_class'] == '/Script/AIModule.BehaviorTree'
    source, = next(group.handles for group in resolved.handle_results if group.name == 'asset_read')
    alias = context.handoff(source_handle=source, target_provider_id='ai', access='read_only')
    saved = context, alias
    value = {'behavior_tree_path':request['target_asset_path']}
    if mode == 'bad_input': value['unexpected'] = True
    if mode == 'wrong_path': value['behavior_tree_path'] += '_Other'
    outcome = context.invoke(requirement=PrimitiveRequirement('ai.behavior_tree.authored_graph.readback.v1', 1, 'ai_profile'),
        value_arguments=value, handle_arguments=(NamedOpaqueHandleArgument('asset', alias),))
    assert not outcome.handle_results and not outcome.changed_set
    if outcome.status != PrimitiveOutcomeStatus.SUCCEEDED:
        return result(request, diagnostics=tuple(_diagnostic_native_projection(d) for d in outcome.diagnostics))
    expected = {'comments':[], 'graph_guid':request['canonical_document']['graph_guid'],
        'graph_state':'present' if mode == 'present' else 'absent', 'root':None}
    assert outcome.value == expected, (outcome.value, expected)
    assert not outcome.diagnostics
    return result(request, outcome.value)
)PY");

FAssetDocumentRuntimeValueDto String(FString Text)
{
	FAssetDocumentRuntimeValueDto Value;
	Value.Kind = EAssetDocumentRuntimeValueKind::String;
	Value.StringValue = MoveTemp(Text);
	return Value;
}
UEdGraph* GraphOf(UObject* Tree)
{
	FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(Tree->GetClass(), TEXT("BTGraph"));
	return Property ? Cast<UEdGraph>(Property->GetObjectPropertyValue_InContainer(Tree)) : nullptr;
}

struct FFixture
{
	FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AssetDocumentA5S"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FString AssetRoot = TEXT("/Game/AssetDocumentTests/A5S_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	FString AbsentPath = AssetRoot + TEXT("/BT_Absent.BT_Absent");
	FString PresentPath = AssetRoot + TEXT("/BT_Present.BT_Present");
	bool RunPython(const FString& Source, const TCHAR* Name)
	{
		const FString Path = FPaths::Combine(Root, Name);
		return FFileHelper::SaveStringToFile(Source, *Path)
			&& GEngine->Exec(nullptr, *FString::Printf(TEXT("py \"%s\""), *Path));
	}
	bool Setup()
	{
		IFileManager::Get().MakeDirectory(*Root, true);
		const FString Package = FPaths::Combine(Root, TEXT("Python"));
		if (!FPlatformFileManager::Get().GetPlatformFile().CopyDirectoryTree(*Package,
			*FPaths::Combine(GetAssetDocumentPluginBaseDir(), TEXT("Content/Python")), false)) return false;
		if (!FFileHelper::SaveStringToFile(Callback, *FPaths::Combine(Package, TEXT("a5s_callback.py")))) return false;
		// Stock factory and stock asset editor create/register the fixtures. All
		// setup finishes before the read-only snapshots and Verify dispatches.
		return RunPython(FString::Printf(TEXT(
			"import unreal\n"
			"tools = unreal.AssetToolsHelpers.get_asset_tools()\n"
			"a5s_absent = tools.create_asset('BT_Absent', '%s', unreal.BehaviorTree, unreal.BehaviorTreeFactory())\n"
			"a5s_present = tools.create_asset('BT_Present', '%s', unreal.BehaviorTree, unreal.BehaviorTreeFactory())\n"
			"assert a5s_absent and a5s_present\n"
			"editors = unreal.get_editor_subsystem(unreal.AssetEditorSubsystem)\n"
			"assert editors.open_editor_for_assets([a5s_present])\n"
			"editors.close_all_editors_for_asset(a5s_present)\n"), *AssetRoot, *AssetRoot), TEXT("setup.py"));
	}
	~FFixture()
	{
		RunPython(FString::Printf(TEXT(
			"import unreal\n"
			"for path in ('%s', '%s'):\n"
			"    asset = unreal.find_asset(path)\n"
			"    if asset:\n"
			"        unreal.get_editor_subsystem(unreal.AssetEditorSubsystem).close_all_editors_for_asset(asset)\n"
			"        unreal.EditorAssetLibrary.delete_loaded_asset(asset)\n"), *AbsentPath, *PresentPath), TEXT("cleanup.py"));
		IFileManager::Get().DeleteDirectory(*Root, false, true);
	}
};

FAssetDocumentRuntimeRequestDto Request(const FString& Path, const FString& Mode, UEdGraph* Graph)
{
	FAssetDocumentRuntimeRequestDto Result;
	Result.RequestId = FGuid::NewGuid();
	Result.Operation = EAssetDocumentRuntimeOperation::Extract;
	Result.TargetAssetPath = Path;
	Result.RequestedAssetClassPath = TEXT("/Script/AIModule.BehaviorTree");
	Result.ExpectedRuntimeRevision = Revision;
	Result.CanonicalDocument.Kind = EAssetDocumentRuntimeValueKind::Object;
	Result.CanonicalDocument.ObjectKeys = {TEXT("mode"), TEXT("graph_guid")};
	Result.CanonicalDocument.Elements = {String(Mode), Graph ? String(Graph->GraphGuid.ToString(EGuidFormats::Digits)) : FAssetDocumentRuntimeValueDto{}};
	return Result;
}

void CheckIdentity(FAutomationTestBase& Test, const FObservation& Seen, const FAssetDocumentRuntimeRequestDto& Input)
{
	Test.TestFalse(TEXT("Verify has no active Atomic transaction"), Seen.bActiveTransaction);
	Test.TestEqual(TEXT("context request pinned"), Seen.Metadata.RequestId, Input.RequestId);
	Test.TestEqual(TEXT("context revision pinned"), Seen.Metadata.PinnedRuntimeRevision, Revision);
	Test.TestEqual(TEXT("context phase is Verify"), Seen.Metadata.Phase, EAssetDocumentRuntimeRequestPhase::Verify);
	if (!Test.TestTrue(TEXT("real handoff issued alias"), Seen.Source.IsSet() && Seen.Alias.IsSet())) return;
	const auto& Source = Seen.Source.GetValue();
	const auto& Alias = Seen.Alias.GetValue();
	Test.TestEqual(TEXT("source is Editor readonly Asset"), Source.ProviderId, FString(TEXT("editor")));
	Test.TestEqual(TEXT("alias is AI"), Alias.ProviderId, FString(TEXT("ai")));
	for (const auto* Handle : {&Source, &Alias})
	{
		Test.TestEqual(TEXT("handle request matches context"), Handle->RequestId, Input.RequestId);
		Test.TestEqual(TEXT("handle transaction identity matches context"), Handle->TransactionId, Seen.TransactionId);
		Test.TestEqual(TEXT("handle revision matches context"), Handle->RuntimeRevision, Revision);
		Test.TestEqual(TEXT("handle remains readonly"), Handle->Access, EAssetDocumentHandleAccess::ReadOnly);
		Test.TestEqual(TEXT("handle is Asset"), Handle->Kind, EAssetDocumentHandleKind::Asset);
	}
	Test.TestNotEqual(TEXT("alias receives a fresh slot"), Source.Slot, Alias.Slot);
	Test.TestEqual(TEXT("exact Verify chain"), Seen.Calls,
		TArray<FString>{TEXT("editor.asset.resolve_existing.v1"), TEXT("handoff"), ReadbackId});
	if (Test.TestEqual(TEXT("two real provider outcomes"), Seen.Outcomes.Num(), 2))
	{
		Test.TestTrue(TEXT("resolve retains its existing multiple leases"), Seen.Outcomes[0].HandleResults.Num() > 1);
		Test.TestTrue(TEXT("AI result has no handles"), Seen.Outcomes[1].HandleResults.IsEmpty());
		Test.TestTrue(TEXT("AI result has no ChangedSet"), Seen.Outcomes[1].ChangedSet.IsEmpty());
	}
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBehaviorTreeVerifyHandoffTest,
	"UEAssetDoc.Integration.BehaviorTree.VerifyHandoff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBehaviorTreeVerifyHandoffTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentBehaviorTreeVerifyHandoffTests;
	FFixture Fixture;
	if (!TestTrue(TEXT("stock fixture setup"), Fixture.Setup())) return false;
	UObject* Absent = FindObject<UObject>(nullptr, *Fixture.AbsentPath);
	UObject* Present = FindObject<UObject>(nullptr, *Fixture.PresentPath);
	if (!TestNotNull(TEXT("absent-graph BT registered"), Absent) || !TestNotNull(TEXT("present-graph BT registered"), Present)) return false;
	UEdGraph* Graph = GraphOf(Present);
	if (!TestNull(TEXT("factory BT has no authored graph"), GraphOf(Absent)) || !TestNotNull(TEXT("stock editor created authored graph"), Graph)) return false;
	Absent->GetOutermost()->SetDirtyFlag(false);
	Present->GetOutermost()->SetDirtyFlag(false);
	const FGuid GraphGuid = Graph->GraphGuid;
	const auto NodesBefore = Graph->Nodes;
	const bool DirtyAbsent = Absent->GetOutermost()->IsDirty();
	const bool DirtyPresent = Present->GetOutermost()->IsDirty();
	FAssetDocumentPythonRuntimeConfiguration Config;
	Config.RepositoryRoot = Fixture.Root;
	Config.PackageRootRelativePath = TEXT("Python");
	FObservedRuntime Runtime(Config);
	if (!TestTrue(TEXT("real Python binding initializes"), Runtime.InitializePythonBinding().bValid)) return false;
	FAssetDocumentPythonRevisionCandidate Candidate;
	Candidate.RuntimeRevision = Revision;
	Candidate.EntryModuleRelativePath = TEXT("a5s_callback.py");
	Candidate.Providers = {CreateAssetDocumentEditorPrimitiveProvider(), CreateAssetDocumentAIPrimitiveProvider()};
	Candidate.RequiredPrimitives = {{TEXT("editor.asset.resolve_existing.v1"), 1, TEXT("route.generic.v1")}, {ReadbackId, 1, TEXT("ai_profile")}};
	const auto Projected = BuildAssetDocumentRequiredPrimitiveProjection(
		Candidate.Providers, Candidate.RequiredPrimitives, GetG1FrozenAuthorityExceptionRecords());
	for (const auto& D : Projected.Diagnostics) AddInfo(D.Code + TEXT(": ") + D.Message);
	if (!TestTrue(TEXT("existing callable projection selects real descriptors"), Projected.Projection.IsValid())) return false;
	Candidate.Providers = Projected.Projection->Providers;
	Candidate.RequiredPrimitives = Projected.Projection->Requirements;
	const auto Activated = Runtime.TryActivateRevision(Candidate);
	for (const auto& D : Activated.Diagnostics) AddInfo(D.Code + TEXT(": ") + D.Message);
	if (!TestTrue(TEXT("real Editor/AI descriptors freeze"), Activated.bActivated)) return false;
	for (const FString Mode : {TEXT("absent"), TEXT("present"), TEXT("bad_input"), TEXT("wrong_path")})
	{
		const auto Input = Request(Mode == TEXT("absent") ? Fixture.AbsentPath : Fixture.PresentPath, Mode, Mode == TEXT("absent") ? nullptr : Graph);
		const auto Result = FAssetDocumentGenericReadOnlyDispatcher::DispatchPhase(Runtime, Input, EAssetDocumentRuntimeRequestPhase::Verify);
		for (const auto& D : Result.Diagnostics) AddInfo(Mode + TEXT(": ") + D.Code + TEXT(": ") + D.Message);
		const bool Success = Mode == TEXT("absent") || Mode == TEXT("present");
		TestEqual(Mode + TEXT(" terminal"), Result.TerminalStatus, Success ? EAssetDocumentRuntimeTerminalStatus::Succeeded : EAssetDocumentRuntimeTerminalStatus::Failed);
		TestEqual(TEXT("Host preserves request"), Result.RequestId, Input.RequestId);
		TestEqual(TEXT("Host preserves revision"), Result.RuntimeRevision, Revision);
		TestTrue(TEXT("Host exports no ChangedSet"), Result.ChangedSet.IsEmpty());
		CheckIdentity(*this, Runtime.Observed, Input);
		if (!Success)
			TestEqual(Mode + TEXT(" provider error preserved"), Result.Diagnostics.IsEmpty() ? FString() : Result.Diagnostics[0].Code,
				FString(Mode == TEXT("bad_input") ? TEXT("AIProvider.InvalidValueShape") : TEXT("AIProvider.InvalidHandle")));
		const auto Closed = FAssetDocumentGenericReadOnlyDispatcher::DispatchPhase(Runtime,
			Request(Input.TargetAssetPath, TEXT("closed"), nullptr), EAssetDocumentRuntimeRequestPhase::Verify);
		for (const auto& D : Closed.Diagnostics) AddInfo(TEXT("closed: ") + D.Code + TEXT(": ") + D.Message);
		TestEqual(Mode + TEXT(" closed context/alias and cross-binding probes"), Closed.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Succeeded);
		TestTrue(TEXT("closed probes never reach real providers"), Runtime.Observed.Calls.IsEmpty());
	}
	for (const FString Mode : {TEXT("missing"), TEXT("wrong_class")})
	{
		const auto Input = Request(Mode == TEXT("missing") ? Fixture.AssetRoot + TEXT("/Missing.Missing") : Fixture.PresentPath, Mode, nullptr);
		const auto Result = FAssetDocumentGenericReadOnlyDispatcher::DispatchPhase(Runtime, Input, EAssetDocumentRuntimeRequestPhase::Verify);
		TestEqual(Mode + TEXT(" cannot become successful empty readback"), Result.TerminalStatus, EAssetDocumentRuntimeTerminalStatus::Failed);
		TestEqual(Mode + TEXT(" exact failure"), Result.Diagnostics.IsEmpty() ? FString() : Result.Diagnostics[0].Code,
			FString(Mode == TEXT("missing") ? TEXT("A5S.TargetMissing") : TEXT("ResolveExisting.ExpectedClassMismatch")));
		TestEqual(TEXT("failed resolve never hands off or reads AI"), Runtime.Observed.Calls, TArray<FString>{TEXT("editor.asset.resolve_existing.v1")});
	}
	TestEqual(TEXT("graph pointer unchanged"), GraphOf(Present), Graph);
	TestEqual(TEXT("graph GUID unchanged"), Graph->GraphGuid, GraphGuid);
	TestTrue(TEXT("authored nodes unchanged"), Graph->Nodes == NodesBefore);
	TestNull(TEXT("absent graph remains absent"), GraphOf(Absent));
	TestEqual(TEXT("absent dirty flag unchanged"), Absent->GetOutermost()->IsDirty(), DirtyAbsent);
	TestEqual(TEXT("present dirty flag unchanged"), Present->GetOutermost()->IsDirty(), DirtyPresent);
	return !HasAnyErrors();
}
#endif
