// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Environment Query primitives (ai.env_query.*, served by the AI provider "ai", owner AI): descriptors, the table
// provider, the EnvQuery derived-state rebuilder, the compile status describer and the list_types kinds.
//
// DTOs (all carry "schema_version": 1):
//   ai.env_query.read.v1     {offset?, defaults?[{kind: generator|test|comment, class}], dependents?} ->
//                            {total, next_offset, records[]}:
//     {kind: query, graph, open_in_editor, root[x, y], runtime_order[option names], params[{name, type}], messages[]}
//     {kind: option, name, generator_name, class, label, item_type, properties{} (sparse over a new node), unsupported[],
//      position[x, y], connected, runtime_index, comment, comment_visible, error, tests[names in sub node order]}
//     {kind: test, name, option, index, class, label, enabled, weight_pct, supported, properties{}, unsupported[],
//      comment, comment_visible}
//     {kind: comment, name, text, position, size, properties{CommentColor, FontSize, ...}}
//     {kind: default, node, request, class, label, properties{} (complete), unsupported[]} | {kind: comment_default, properties{}}
//     {kind: dependent, package, class, asset}
//   ai.env_query.edit.v1     {ops[]} (one editor transaction per call) -> {created[]} (the object name of each op's
//                            new object, "" otherwise):
//     {op: add_option, class, name?, position, connect?} | {op: add_test, option, class, name?, index?} |
//     {op: remove, name} | {op: move_test, name, option, index} | {op: set_enabled, name, enabled} |
//     {op: set_property, name, property, value} | {op: connect, name, connected} |
//     {op: layout, name ("" = Root), position?, comment?, comment_visible?} | {op: rename, name, key} |
//     {op: add_comment, name?, text, position, size?} | {op: comment, name, text?, size?, properties?{}} |
//     {op: remove_comment, name}
//   ai.env_query.finalize.v1 {} -> {messages[{severity, message, option?, test?}], params[]}

#include "EnvQuery/AssetDocumentEnvQueryPrimitives.h"

#include "AIGraph.h"
#include "AssetDocumentCompileStatus.h"
#include "AssetDocumentGraphProvider.h"
#include "AssetDocumentGraphValue.h"
#include "AssetDocumentTypeListers.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryManager.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "Runtime/AssetDocumentPackageSnapshot.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"
#include "Utils/AssetDocumentEngineCompat.h"

namespace UE::AssetDocument::Graph::EnvQuery
{
namespace
{
constexpr uint32 EnvQueryProviderVersion = 1;

FAssetDocumentPrimitiveDescriptor MakeDescriptor(const TCHAR* PrimitiveId, bool bMutating, const TCHAR* AuthoritativeApi)
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = PrimitiveId;
	Descriptor.Version = EnvQueryProviderVersion;
	Descriptor.ProviderId = ProviderId;
	Descriptor.Owner = EAssetDocumentPrimitiveOwner::AI;
	Descriptor.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
	Descriptor.Access = bMutating ? EAssetDocumentPrimitiveAccess::Mutating : EAssetDocumentPrimitiveAccess::ReadOnly;
	if (bMutating)
	{
		Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair};
		Descriptor.MutationFootprintSchemaId = TEXT("graph.footprint.whole-package.v1");
	}
	else
	{
		Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Validate, EAssetDocumentRuntimeRequestPhase::Preflight,
			EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair,
			EAssetDocumentRuntimeRequestPhase::Verify};
	}
	Descriptor.bRequiresActiveTransaction = bMutating;
	Descriptor.InputSchemaId = Descriptor.PrimitiveId + TEXT(".input.v1");
	Descriptor.OutputSchemaId = Descriptor.PrimitiveId + TEXT(".output.v1");
	Descriptor.StableDiagnosticCodes = {
		TEXT("AD.RUNTIME.PRIMITIVE_FAILED"), TEXT("Graph.UnknownField"), TEXT("Graph.InvalidArgument"),
		TEXT("Graph.UnsupportedSchemaVersion"), TEXT("Graph.PieActive"), TEXT("Graph.ResultTooLarge"),
		TEXT("EnvQuery.UnsupportedAsset"), TEXT("EnvQuery.EditorUnavailable"), TEXT("EnvQuery.InvalidGraph"),
		TEXT("EnvQuery.InvalidOp"), TEXT("EnvQuery.EditFailed"), TEXT("EnvQuery.UnknownClass"), TEXT("EnvQuery.OptionNotFound"),
		TEXT("EnvQuery.TestNotFound"), TEXT("EnvQuery.NodeNotFound"), TEXT("EnvQuery.DuplicateKey"), TEXT("EnvQuery.InvalidKey"),
		TEXT("EnvQuery.NotDeletable"), TEXT("EnvQuery.ConnectDisallowed"), TEXT("EnvQuery.InvalidValue"),
		TEXT("Generic.UnknownProperty"), TEXT("Generic.NonWritableProperty"), TEXT("Property.EditorDerivedReference")};
	Descriptor.RequiredConsumerIds = {TEXT("route.generic.v1")};
	Descriptor.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
	Descriptor.HandleArgumentSchemas.Add({TEXT("asset"), EAssetDocumentHandleKind::Asset,
		bMutating ? EAssetDocumentHandleAccess::Mutable : EAssetDocumentHandleAccess::ReadOnly, true});

	FAssetDocumentPrimitiveAdmissionEvidence& Evidence = Descriptor.Admission;
	Evidence.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
	Evidence.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper;
	Evidence.ExactEngineVersion = TEXT("5.7.4");
	Evidence.PhysicalOwner = EAssetDocumentPrimitiveOwner::AI;
	Evidence.PublicModule = TEXT("AssetDocumentAI");
	Evidence.PublicHeader = TEXT("AssetDocumentAIPrimitiveProvider.h");
	Evidence.ExportedAuthoritativeApi = AuthoritativeApi;
	Evidence.AuthorityContextId = TEXT("assetdocument.editor.game-thread.editor-ready.v1");
	Evidence.PrimaryOperationId = Descriptor.PrimitiveId;
	Evidence.IdentityLifetimeContractId = TEXT("identity.request-transaction-revision-bound.v1");
	Evidence.TransactionContractId = TEXT("transaction.descriptor-governed.v1");
	Evidence.RollbackContractId = TEXT("rollback.core-package-snapshot.v1");
	Evidence.CompletionContractId = Descriptor.PrimitiveId + TEXT(".completion.v1");
	Evidence.SaveReloadEvidenceId = TEXT("UEAssetDoc.EnvQuery.Fixtures");
	Evidence.FailureInjectionTestIds = {TEXT("UEAssetDoc.EnvQuery.Rollback")};
	Evidence.VerificationTestIds = {TEXT("UEAssetDoc.EnvQuery.Fixtures")};
	Evidence.AuthorityFingerprint = FString::Printf(TEXT("ue-5.7.4|env_query|%s|%s"), PrimitiveId, AuthoritativeApi);
	return Descriptor;
}

TArray<FAssetDocumentPrimitiveDescriptor> BuildDescriptors()
{
	return {
		MakeDescriptor(TEXT("ai.env_query.read.v1"), false,
			TEXT("UEnvQuery::GetOptions;UEnvQuery::CollectQueryParams;FGraphNodeClassHelper::GatherClasses")),
		MakeDescriptor(TEXT("ai.env_query.edit.v1"), true,
			TEXT("FAISchemaAction_NewNode::PerformAction;FAISchemaAction_NewSubNode::PerformAction;FAISchemaAction_AddComment::PerformAction;"
				"UAIGraphNode::DestroyNode;UAIGraphNode::RemoveSubNode;UAIGraphNode::InsertSubNodeAt;UAIGraph::OnSubNodeDropped;"
				"UAIGraph::UpdateAsset;UEdGraphSchema::TryCreateConnection;UEdGraphSchema::BreakSinglePinLink;PropertyAccessUtil::SetPropertyValue_InContainer")),
		MakeDescriptor(TEXT("ai.env_query.finalize.v1"), true, TEXT("UAIGraph::Initialize;UAIGraph::UpdateAsset")),
	};
}

const FBinding Bindings[] = {
	{TEXT("ai.env_query.read.v1"), &HandleRead},
	{TEXT("ai.env_query.edit.v1"), &HandleEdit},
	{TEXT("ai.env_query.finalize.v1"), &HandleFinalize},
};

class FEnvQueryRebuilder final : public IAssetDocumentDerivedStateRebuilder
{
public:
	virtual FName GetId() const override { return TEXT("AssetDocumentAI.EnvQuery"); }
	virtual bool Supports(const UObject& PrimaryAsset) const override { return PrimaryAsset.IsA<UEnvQuery>(); }

	// The names of the query's options and tests (the document's keys). The snapshot's PostEditUndo runs
	// UAIGraphNode::ResetNodeOwner, which renames each restored node instance to a new unique name (as the editor's
	// Undo does); the restore gives them their captured names back so a refused apply leaves the document as it was.
	struct FNames
	{
		TArray<TPair<TWeakObjectPtr<UObject>, FName>> Objects;
	};

	virtual TSharedPtr<void> CaptureDerivedState(UObject& PrimaryAsset) const override
	{
		TSharedPtr<FNames> Names = MakeShared<FNames>();
		TArray<UObject*> Inner;
		GetObjectsWithOuter(&PrimaryAsset, Inner, AssetDocumentObjects::DirectOnly);
		for (UObject* Object : Inner)
		{
			Names->Objects.Emplace(Object, Object->GetFName());
		}
		return Names;
	}

	virtual FAssetDocumentContractValidationResult RebuildFromState(UObject& PrimaryAsset, const TSharedPtr<void>& State) override
	{
		const FNames* Names = static_cast<const FNames*>(State.Get());
		for (int32 Pass = 0; Names != nullptr && Pass < 2; ++Pass)
		{
			for (const TPair<TWeakObjectPtr<UObject>, FName>& Entry : Names->Objects)
			{
				UObject* Object = Entry.Key.Get();
				if (Object != nullptr && Object->GetOuter() == &PrimaryAsset && Object->GetFName() != Entry.Value
					&& StaticFindObjectFast(nullptr, &PrimaryAsset, Entry.Value) == nullptr)
				{
					Object->Rename(*Entry.Value.ToString(), &PrimaryAsset, REN_DontCreateRedirectors | REN_DoNotDirty | REN_NonTransactional);
				}
			}
		}
		return Rebuild(PrimaryAsset);
	}

	virtual FAssetDocumentContractValidationResult Rebuild(UObject& PrimaryAsset) override
	{
		// The restored Options are consistent with the restored graph; the editor's Undo refreshes the EQS manager's
		// cached query instances (UpdateAsset -> NotifyAssetUpdate) and the graph panel.
		UEnvQuery& Query = *CastChecked<UEnvQuery>(&PrimaryAsset);
#if USE_EQS_DEBUGGER
		UEnvQueryManager::NotifyAssetUpdate(&Query);
#endif
		if (UAIGraph* Graph = GetGraph(Query))
		{
			Graph->NotifyGraphChanged();
		}
		FAssetDocumentContractValidationResult Result;
		Result.bValid = true;
		return Result;
	}
};

TSharedPtr<IAssetDocumentDerivedStateRebuilder> GRebuilder;
const FName DescriberId(TEXT("AssetDocumentAI.EnvQuery"));

FString QueryValue(const TMap<FString, FString>& Query, const TCHAR* Key)
{
	const FString* Value = Query.Find(Key);
	return Value != nullptr ? Value->TrimStartAndEnd() : FString();
}

bool ListTypes(ENodeKind Kind, const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError)
{
	if (!EnsureEditorModule())
	{
		OutError = TEXT("The Environment Query editor (plugin EnvironmentQueryEditor) is not loaded.");
		return false;
	}
	const FString Pattern = QueryValue(Query, TEXT("name"));
	const FString Prefix = QueryValue(Query, TEXT("path"));
	const bool bWildcard = Pattern.Contains(TEXT("*")) || Pattern.Contains(TEXT("?"));
	for (const FPaletteClass& Entry : GetPalette(Kind))
	{
		auto Test = [&Pattern, bWildcard](const FString& Text)
		{
			return bWildcard ? Text.MatchesWildcard(Pattern, ESearchCase::IgnoreCase) : Text.Contains(Pattern, ESearchCase::IgnoreCase);
		};
		const FString Path = Entry.Class->GetPathName();
		if (!Pattern.IsEmpty() && !Test(Entry.Label) && !Test(Entry.DisplayName) && !Test(Path))
		{
			continue;
		}
		if (!Prefix.IsEmpty() && !Path.StartsWith(Prefix, ESearchCase::IgnoreCase))
		{
			continue;
		}
		TStrongObjectPtr<UObject> Defaults(NewDefaultsObject(*Entry.Class));
		TArray<FString> Unsupported;
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Path"), Path);
		Row->SetStringField(TEXT("Class"), Entry.Label);
		Row->SetStringField(TEXT("DisplayName"), Entry.DisplayName);
		if (!Entry.Category.IsEmpty())
		{
			Row->SetStringField(TEXT("Category"), Entry.Category);
		}
		if (!Entry.Tooltip.IsEmpty())
		{
			Row->SetStringField(TEXT("Description"), Entry.Tooltip);
		}
		if (!Entry.Deprecated.IsEmpty())
		{
			Row->SetStringField(TEXT("Deprecated"), Entry.Deprecated);
		}
		if (Entry.bBlueprint)
		{
			Row->SetBoolField(TEXT("Blueprint"), true);
		}
		if (const UEnvQueryGenerator* Generator = Cast<UEnvQueryGenerator>(Defaults.Get()))
		{
			Row->SetStringField(TEXT("ItemType"), Generator->ItemType != nullptr ? Generator->ItemType->GetPathName() : FString());
		}
		Row->SetObjectField(TEXT("Properties"), ValueToJson(EncodeProperties(*Defaults, nullptr, Unsupported))->AsObject());
		OutRows.Add(Row);
	}
	return true;
}

bool ListGenerators(const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError)
{
	return ListTypes(ENodeKind::Generator, Query, OutRows, OutError);
}

bool ListTests(const TMap<FString, FString>& Query, TArray<TSharedPtr<FJsonObject>>& OutRows, FString& OutError)
{
	return ListTypes(ENodeKind::Test, Query, OutRows, OutError);
}

TSharedPtr<FJsonObject> Describe(const UObject& Asset)
{
	return DescribeCompileStatus(Asset);
}
} // namespace

TConstArrayView<FAssetDocumentPrimitiveDescriptor> GetDescriptors()
{
	static const TArray<FAssetDocumentPrimitiveDescriptor> Descriptors = BuildDescriptors();
	return Descriptors;
}

TConstArrayView<FBinding> GetBindings()
{
	return Bindings;
}
} // namespace UE::AssetDocument::Graph::EnvQuery

TSharedRef<IAssetDocumentPrimitiveProvider> UE::AssetDocument::Graph::EnvQuery::CreateProvider()
{
	return CreateAssetDocumentAssetTablePrimitiveProvider(ProviderId, &GetDescriptors, &GetBindings);
}

void RegisterAssetDocumentEnvQueryHooks()
{
	using namespace UE::AssetDocument::Graph::EnvQuery;
	if (!GRebuilder.IsValid())
	{
		GRebuilder = MakeShared<FEnvQueryRebuilder>();
		RegisterAssetDocumentDerivedStateRebuilder(GRebuilder.ToSharedRef());
	}
	RegisterAssetDocumentCompileStatusDescriber(DescriberId, &Describe);
	RegisterAssetDocumentTypeLister(TEXT("env_query_generator"), &ListGenerators);
	RegisterAssetDocumentTypeLister(TEXT("env_query_test"), &ListTests);
}

void UnregisterAssetDocumentEnvQueryHooks()
{
	using namespace UE::AssetDocument::Graph::EnvQuery;
	UnregisterAssetDocumentTypeLister(TEXT("env_query_test"));
	UnregisterAssetDocumentTypeLister(TEXT("env_query_generator"));
	UnregisterAssetDocumentCompileStatusDescriber(DescriberId);
	if (GRebuilder.IsValid())
	{
		UnregisterAssetDocumentDerivedStateRebuilder(GRebuilder->GetId());
		GRebuilder.Reset();
	}
	ReleaseClassCache();
}
