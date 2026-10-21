#include "AssetDocumentRuntimeModule.h"
#include "AssetDocumentGenericRuntimeBootstrap.h"
#include "Containers/Ticker.h"
#include "Modules/ModuleManager.h"

class FAssetDocumentIntegrationModule final : public IAssetDocumentRuntimeModule
{
public:
	void StartupModule() override
	{
		Bootstrap = MakeUnique<FAssetDocumentGenericRuntimeBootstrap>();
		if (!Bootstrap->Activate() && Bootstrap->IsWaitingForPython())
		{
			ActivationHandle = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateRaw(
					this,
					&FAssetDocumentIntegrationModule::RetryActivation));
		}
	}
	void ShutdownModule() override
	{
		FTSTicker::GetCoreTicker().RemoveTicker(ActivationHandle);
		ActivationHandle.Reset();
		Bootstrap.Reset();
	}
	FName GetRuntimeModuleName() const override { return TEXT("AssetDocumentIntegration"); }
	bool HasActiveRuntimeRegistration() const override
	{
		return Bootstrap.IsValid() && Bootstrap->IsActive();
	}

private:
	bool RetryActivation(float DeltaTime)
	{
		(void)DeltaTime;
		const bool bActivated =
			Bootstrap.IsValid() && Bootstrap->Activate();
		if (bActivated)
		{
			ActivationHandle.Reset();
		}
		return !bActivated
			&& Bootstrap.IsValid()
			&& Bootstrap->IsWaitingForPython();
	}

	TUniquePtr<FAssetDocumentGenericRuntimeBootstrap> Bootstrap;
	FTSTicker::FDelegateHandle ActivationHandle;
};

IMPLEMENT_MODULE(FAssetDocumentIntegrationModule, AssetDocumentIntegration)
