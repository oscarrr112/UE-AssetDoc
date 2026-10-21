#include "AssetDocumentRuntimeModule.h"
#include "BehaviorTree/AssetDocumentAIBlackboardOpenEditorHandler.h"
#include "EnvQuery/AssetDocumentEnvQueryPrimitives.h"
#include "StateTree/AssetDocumentStateTreePrimitives.h"
#include "AssetDocumentAIOpenEditor.h"
#include "Modules/ModuleManager.h"

class FAssetDocumentAIModule final : public IAssetDocumentRuntimeModule
{
public:
	void StartupModule() override
	{
		check(!HasActiveRuntimeRegistration());
		// Blackboard: an open Behavior Tree editor's Blackboard panels show every apply.
		RegisterAssetDocumentBlackboardOpenEditorHandler();
		// StateTree: snapshot rollback recompiles the restored tree; Apply / Extract report its compile status.
		RegisterAssetDocumentStateTreeEditorHooks();
		// Environment Query: rollback rebuilder, compile status (validation) and list_types kinds env_query_generator /
		// env_query_test (the editor module EnvironmentQueryEditor is loaded on first use).
		RegisterAssetDocumentEnvQueryHooks();
		// Open-editor apply: refresh an open StateTree / Environment Query editor, reopen a Behavior Tree editor.
		RegisterAssetDocumentAIOpenEditorHandlers();
	}
	void ShutdownModule() override
	{
		UnregisterAssetDocumentAIOpenEditorHandlers();
		UnregisterAssetDocumentEnvQueryHooks();
		UnregisterAssetDocumentStateTreeEditorHooks();
		UnregisterAssetDocumentBlackboardOpenEditorHandler();
	}
	FName GetRuntimeModuleName() const override { return TEXT("AssetDocumentAI"); }
};

IMPLEMENT_MODULE(FAssetDocumentAIModule, AssetDocumentAI)
