// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentSidecarAutoReimport.h"

#include "AssetDocumentModule.h"
#include "AssetDocumentSettings.h"
#include "Misc/Paths.h"
#include "Settings/EditorLoadingSavingSettings.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::SidecarAutoReimport
{
namespace SidecarAutoReimportLocal
{
const TCHAR* const Wildcards[] = {TEXT("*.assetdoc.json"), TEXT("*.assetdoc.asset.json")};
FDelegateHandle GSettingHandle;

FString FullDirectory(const FString& Directory)
{
	FString Full = FPaths::ConvertRelativePathToFull(Directory);
	FPaths::NormalizeDirectoryName(Full);
	return Full + TEXT("/");
}
} // namespace SidecarAutoReimportLocal

TConstArrayView<const TCHAR*> GetWildcards()
{
	return SidecarAutoReimportLocal::Wildcards;
}

bool CoversContent(const FAutoReimportDirectoryConfig& Directory)
{
	using namespace SidecarAutoReimportLocal;
	// The directory the Auto Reimport manager watches for this entry (it skips an entry it cannot parse).
	FString Source = Directory.SourceDirectory;
	FString MountPoint = Directory.MountPoint;
	if (!FAutoReimportDirectoryConfig::ParseSourceDirectoryAndMountPoint(Source, MountPoint, FAutoReimportDirectoryConfig::FParseContext(false))
		|| Source.IsEmpty())
	{
		return false;
	}
	const FString Watched = FullDirectory(Source);
	const FString Content = FullDirectory(FPaths::ProjectContentDir());
	return Watched.StartsWith(Content, ESearchCase::IgnoreCase) || Content.StartsWith(Watched, ESearchCase::IgnoreCase);
}

bool UpdateDirectories(TArray<FAutoReimportDirectoryConfig>& Directories, bool bExclude)
{
	bool bChanged = false;
	for (FAutoReimportDirectoryConfig& Directory : Directories)
	{
		if (!CoversContent(Directory))
		{
			continue;
		}
		for (const TCHAR* Wildcard : GetWildcards())
		{
			const auto IsOurs = [Wildcard](const FAutoReimportWildcard& Entry)
			{
				return !Entry.bInclude && Entry.Wildcard.Equals(Wildcard, ESearchCase::IgnoreCase);
			};
			if (!bExclude)
			{
				bChanged |= Directory.Wildcards.RemoveAll(IsOurs) > 0;
			}
			else if (!Directory.Wildcards.ContainsByPredicate(IsOurs))
			{
				FAutoReimportWildcard Entry;
				Entry.Wildcard = Wildcard;
				Entry.bInclude = false;
				Directory.Wildcards.Add(MoveTemp(Entry));
				bChanged = true;
			}
		}
	}
	return bChanged;
}

bool Sync()
{
	if (IsRunningCommandlet())
	{
		return false;
	}
	UEditorLoadingSavingSettings* Settings = GetMutableDefault<UEditorLoadingSavingSettings>();
	const bool bExclude = GetDefault<UAssetDocumentSettings>()->bExcludeSidecarsFromAutoReimport;
	TArray<FAutoReimportDirectoryConfig> Directories = Settings->AutoReimportDirectorySettings;
	if (!UpdateDirectories(Directories, bExclude))
	{
		return false;
	}
	Settings->AutoReimportDirectorySettings = MoveTemp(Directories);
	// As an edit of the setting in Editor Preferences: the settings are saved and the Auto Reimport manager restarts its
	// directory watchers with the new wildcards.
	FProperty* Property = FindFProperty<FProperty>(UEditorLoadingSavingSettings::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UEditorLoadingSavingSettings, AutoReimportDirectorySettings));
	FPropertyChangedEvent Event(Property);
	static_cast<UObject*>(Settings)->PostEditChangeProperty(Event);
	UE_LOG(LogAssetDocument, Log, TEXT("AssetDocument: %s the sidecar exclude wildcards in the Auto Reimport directory settings"),
		bExclude ? TEXT("added") : TEXT("removed"));
	return true;
}

void Register()
{
	using namespace SidecarAutoReimportLocal;
	Sync();
	if (!GSettingHandle.IsValid())
	{
		GSettingHandle = UAssetDocumentSettings::OnAutoReimportSettingChanged().AddLambda([]() { Sync(); });
	}
}

void Unregister()
{
	using namespace SidecarAutoReimportLocal;
	if (GSettingHandle.IsValid())
	{
		UAssetDocumentSettings::OnAutoReimportSettingChanged().Remove(GSettingHandle);
		GSettingHandle.Reset();
	}
}
} // namespace UE::AssetDocument::SidecarAutoReimport
