#include "AssetDocumentRuntimeModule.h"
#include "Modules/ModuleManager.h"

class FAssetDocumentPythonModule final : public IAssetDocumentRuntimeModule
{
public:
	FName GetRuntimeModuleName() const override { return TEXT("AssetDocumentPython"); }
};

IMPLEMENT_MODULE(FAssetDocumentPythonModule, AssetDocumentPython)
