// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentEditorMarkerText.h"
#include "AssetDocumentEditorTrialScope.h"
#include "Internationalization/Regex.h"
#include "Misc/PackageName.h"
#include "UObject/Object.h"
#include "UObject/Package.h"

namespace UE::AssetDocument::EditorMarkerText
{
namespace
{
const TCHAR* const TransientPackageName = TEXT("/Engine/Transient");
const TCHAR* const StagedTrialRoot = TEXT("/Temp/AssetDocument/ValidateTrial/");

// Name without its trailing "_<digits>" groups ("DT_Items_3" -> "DT_Items"); Name itself when nothing else is left.
FString WithoutNumberSuffix(const FString& Name)
{
	int32 End = Name.Len();
	while (true)
	{
		int32 Index = End;
		while (Index > 0 && FChar::IsDigit(Name[Index - 1]))
		{
			--Index;
		}
		if (Index == End || Index == 0 || Name[Index - 1] != TEXT('_'))
		{
			break;
		}
		End = Index - 1;
	}
	return End > 0 ? Name.Left(End) : Name;
}

// Whether Rest is empty or only "_<digits>" groups.
bool IsNumberSuffix(const FString& Rest)
{
	int32 Index = Rest.Len();
	while (Index > 0)
	{
		const int32 End = Index;
		while (Index > 0 && FChar::IsDigit(Rest[Index - 1]))
		{
			--Index;
		}
		if (Index == End || Index == 0 || Rest[Index - 1] != TEXT('_'))
		{
			return false;
		}
		--Index;
	}
	return true;
}

// The name the object "<Package>.<Name>" has in the asset AssetName when it is an object of that asset: the asset's
// own package in any folder (the asset where it is, or before a move), or Validate's copy of it (the top-level object
// <AssetName> or <AssetName>_<N> of the same base in /Engine/Transient; <AssetName> in a staged trial package).
bool FindOwnObject(const FString& Package, const FString& Name, const FString& AssetName, FString& OutName)
{
	if (Package.StartsWith(TEXT("/Script/"), ESearchCase::CaseSensitive))
	{
		return false;
	}
	FString Short;
	if (!Package.Split(TEXT("/"), nullptr, &Short, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
	{
		Short = Package;
	}
	if (Short.Equals(AssetName, ESearchCase::CaseSensitive))
	{
		OutName = Name;
		return true;
	}
	if (Package.Equals(TransientPackageName, ESearchCase::CaseSensitive))
	{
		const FString Base = WithoutNumberSuffix(AssetName);
		if (Name.StartsWith(Base, ESearchCase::CaseSensitive) && IsNumberSuffix(Name.Mid(Base.Len())))
		{
			OutName = AssetName;
			return true;
		}
		return false;
	}
	if (Package.StartsWith(StagedTrialRoot, ESearchCase::CaseSensitive) && Name.Equals(AssetName, ESearchCase::CaseSensitive))
	{
		OutName = AssetName;
		return true;
	}
	return false;
}
}

FString Rebase(const FString& Text, const FString& AssetPath)
{
	FString Package;
	FString AssetName;
	if (!AssetPath.Split(TEXT("."), &Package, &AssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
	{
		return Text;
	}
	int32 Colon = INDEX_NONE;
	if (AssetName.FindChar(TEXT(':'), Colon))
	{
		AssetName.LeftInline(Colon);
	}
	if (AssetName.IsEmpty())
	{
		return Text;
	}
	// An object path "<package>.<object>" (a subobject continues with ":"): package and object name.
	static const FRegexPattern Pattern(TEXT("(/[^\\s'\"(),:]+)\\.([^\\s'\"(),:/.]+)"));
	FRegexMatcher Matcher(Pattern, Text);
	FString Out;
	int32 Copied = 0;
	while (Matcher.FindNext())
	{
		FString Own;
		if (FindOwnObject(Matcher.GetCaptureGroup(1), Matcher.GetCaptureGroup(2), AssetName, Own))
		{
			Out += Text.Mid(Copied, Matcher.GetMatchBeginning() - Copied);
			Out += Package + TEXT(".") + Own;
			Copied = Matcher.GetMatchEnding();
		}
	}
	return Out + Text.Mid(Copied);
}

FString GetAssetPath(const UObject* Within)
{
	const UObject* Asset = Within != nullptr ? Within->GetOutermostObject() : nullptr;
	if (Asset == nullptr)
	{
		return FString();
	}
	if (IsInGameThread())
	{
		if (const UObject* Source = FAssetDocumentEditorTrialScope::FindTrialSource(*Asset))
		{
			Asset = Source;
		}
	}
	const UPackage* Package = Asset->GetPackage();
	const FString PackageName = Package != nullptr ? Package->GetName() : FString();
	if (PackageName.IsEmpty() || PackageName.StartsWith(TEXT("/Script/"), ESearchCase::CaseSensitive))
	{
		return FString();
	}
	if (PackageName.Equals(TransientPackageName, ESearchCase::CaseSensitive)
		|| PackageName.StartsWith(StagedTrialRoot, ESearchCase::CaseSensitive))
	{
		// A transient object with no asset it copies: named by its own path.
		return Asset->GetPathName();
	}
	// The asset is the package's object of the package's name (also when Within is in a class default object).
	return PackageName + TEXT(".") + FPackageName::GetShortName(Package);
}

bool IsSameText(const FString& Authored, const FString& Current, const UObject* Within)
{
	if (Authored.Equals(Current, ESearchCase::CaseSensitive))
	{
		return true;
	}
	const FString AssetPath = GetAssetPath(Within);
	return !AssetPath.IsEmpty() && Rebase(Authored, AssetPath).Equals(Rebase(Current, AssetPath), ESearchCase::CaseSensitive);
}
}
