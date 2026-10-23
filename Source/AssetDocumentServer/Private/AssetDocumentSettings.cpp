// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentSettings.h"

UAssetDocumentSettings::UAssetDocumentSettings()
{
}

UAssetDocumentSettings* UAssetDocumentSettings::Get()
{
	return GetMutableDefault<UAssetDocumentSettings>();
}

namespace AssetDocumentSettingsLocal
{
// Settings no tool of the plugin writes, with the reason. Besides this plugin's own settings, each entry is an engine
// settings class that makes the editor run code, or opens a channel that runs it, once it is changed. A class that is
// not loaded (its plugin is off, or the engine version has none) has no object to write and is skipped.
struct FProtectedSettingsClass
{
	const TCHAR* ClassPath;
	const TCHAR* Reason;
};

const FProtectedSettingsClass ExecutionSettings[] = {
	{TEXT("/Script/PythonScriptPlugin.PythonScriptPluginSettings"),
		TEXT("the Python plugin's settings run Python: startup scripts and commands, the script search paths, pip installs "
			"at startup and the remote execution server")},
	{TEXT("/Script/RemoteControlCommon.RemoteControlSettings"),
		TEXT("the Remote Control web server calls functions, sets properties and can run console commands and Python for its clients")},
	{TEXT("/Script/ModelContextProtocolEngine.ModelContextProtocolSettings"),
		TEXT("the engine's own MCP server runs editor tools for its clients")},
	{TEXT("/Script/MovieRenderPipelineCore.MoviePipelineCommandLineEncoderSettings"),
		TEXT("it names an executable and the command line a render runs")},
	{TEXT("/Script/MovieRenderPipelineCore.MoviePipelineInProcessExecutorSettings"),
		TEXT("it holds the command line of the editor process a render starts, which can run Python or console commands")},
	{TEXT("/Script/GameFeaturesEditor.GameFeaturesEditorSettings"),
		TEXT("plugin templates name a Python script run when a game feature plugin is created")},
	{TEXT("/Script/Terminal.TerminalSettings"),
		TEXT("it names the shell the editor's terminal starts and the commands it runs at startup")},
};

const UClass* SettingsClassOf(const UObject& Object)
{
	const UClass* Class = Cast<UClass>(&Object);
	return Class != nullptr ? Class : Object.GetClass();
}

const FProtectedSettingsClass* FindExecutionSettings(const UClass& Class)
{
	for (const UClass* Each = &Class; Each != nullptr; Each = Each->GetSuperClass())
	{
		for (const FProtectedSettingsClass& Entry : ExecutionSettings)
		{
			if (Each->GetPathName().Equals(Entry.ClassPath, ESearchCase::IgnoreCase))
			{
				return &Entry;
			}
		}
	}
	return nullptr;
}

// The short class name of a class path ("/Script/Module.Name" -> "Name").
FString ShortClassName(const TCHAR* ClassPath)
{
	FString Path(ClassPath);
	int32 Dot = INDEX_NONE;
	return Path.FindLastChar(TEXT('.'), Dot) ? Path.Mid(Dot + 1) : Path;
}
} // namespace AssetDocumentSettingsLocal

bool UAssetDocumentSettings::IsProtectedSettingsObject(const UObject* Object)
{
	if (Object == nullptr)
	{
		return false;
	}
	if (const UClass* Class = Cast<UClass>(Object))
	{
		return Class->IsChildOf(StaticClass());
	}
	return Object->IsA(StaticClass());
}

FString UAssetDocumentSettings::WhyWriteProtected(const UObject* Object)
{
	using namespace AssetDocumentSettingsLocal;
	if (Object == nullptr)
	{
		return FString();
	}
	if (IsProtectedSettingsObject(Object))
	{
		return DescribeProtectedSettings();
	}
	const UClass* Class = SettingsClassOf(*Object);
	const FProtectedSettingsClass* Entry = Class != nullptr ? FindExecutionSettings(*Class) : nullptr;
	if (Entry == nullptr)
	{
		return FString();
	}
	return FString::Printf(TEXT("%s is not written by an AI client: %s. Reading it is allowed; the user changes it in the "
		"editor."), Entry->ClassPath, Entry->Reason);
}

FString UAssetDocumentSettings::WhyNamesProtectedSettings(const FString& Text)
{
	using namespace AssetDocumentSettingsLocal;
	// Every name the engine resolves to a class or its default object contains the class name ("AssetDocumentSettings",
	// "Default__AssetDocumentSettings", "/Script/AssetDocumentServer.AssetDocumentSettings"); object names compare
	// case-insensitively.
	if (Text.Contains(StaticClass()->GetName(), ESearchCase::IgnoreCase))
	{
		return DescribeProtectedSettings();
	}
	for (const FProtectedSettingsClass& Entry : ExecutionSettings)
	{
		if (Text.Contains(ShortClassName(Entry.ClassPath), ESearchCase::IgnoreCase))
		{
			return FString::Printf(TEXT("%s is not written by an AI client: %s; the user changes it in the editor."), Entry.ClassPath,
				Entry.Reason);
		}
	}
	return FString();
}

bool UAssetDocumentSettings::NamesProtectedSettings(const FString& Text)
{
	return !WhyNamesProtectedSettings(Text).IsEmpty();
}

FString UAssetDocumentSettings::DescribeProtectedSettings()
{
	return TEXT("The UE-AssetDoc plugin's own server and security settings cannot be changed by an AI client; the user changes "
		"them in Project Settings > Plugins > UE-AssetDoc.");
}
