#include "AssetDocumentRuntimeModule.h"
#include "Modules/ModuleManager.h"

class FAssetDocumentUMGModule final : public IAssetDocumentRuntimeModule
{
public:
	void StartupModule() override { check(!HasActiveRuntimeRegistration()); }
	FName GetRuntimeModuleName() const override { return TEXT("AssetDocumentUMG"); }
};

IMPLEMENT_MODULE(FAssetDocumentUMGModule, AssetDocumentUMG)
