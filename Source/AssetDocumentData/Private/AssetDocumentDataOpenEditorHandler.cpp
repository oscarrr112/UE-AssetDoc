// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentDataOpenEditorHandler.h"

#include "DataTableEditorUtils.h"
#include "Engine/DataTable.h"
#include "Runtime/AssetDocumentOpenEditor.h"

namespace UE::AssetDocument::Graph::Data
{
namespace DataTableOpenEditorHandling
{
class FDataTableOpenEditorHandler final : public IAssetDocumentOpenEditorHandler
{
public:
	virtual FName GetId() const override
	{
		return TEXT("AssetDocumentData.DataTable");
	}

	virtual bool Supports(const UObject& Asset) const override
	{
		return Asset.IsA<UDataTable>();
	}

	virtual FString GetProfileName(const UObject& Asset) const override
	{
		return Asset.GetClass()->GetName();
	}

	virtual EAssetDocumentOpenEditorStrategy GetStrategy(const UObject& Asset) const override
	{
		return EAssetDocumentOpenEditorStrategy::RefreshInPlace;
	}

	virtual void RefreshOpenEditor(UObject& Asset, bool bSucceeded) const override
	{
		FDataTableEditorUtils::FDataTableEditorManager::Get().PostChange(CastChecked<UDataTable>(&Asset),
			FDataTableEditorUtils::EDataTableChangeInfo::RowList);
	}
};
} // namespace DataTableOpenEditorHandling

TSharedRef<IAssetDocumentOpenEditorHandler> MakeDataTableOpenEditorHandler()
{
	return MakeShared<DataTableOpenEditorHandling::FDataTableOpenEditorHandler>();
}
} // namespace UE::AssetDocument::Graph::Data
