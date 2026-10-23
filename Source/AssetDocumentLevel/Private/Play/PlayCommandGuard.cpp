// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Play/PlayCommandGuard.h"

#include "AssetDocumentSettings.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/PlayerInput.h"
#include "InputCoreTypes.h"
#include "Misc/Parse.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocumentLevel::Private::Play
{
// Named per file (unity builds merge the anonymous namespaces of the play tools).
namespace PlayCommandGuardLocal
{
// Text handed on inside text handed on (an escaped word that unescapes to a command that quotes another) is read
// this deep; deeper text is refused unread.
constexpr int32 MaxDepth = 8;

// Every word of a line is read as the start of a command: a longer line is refused unread (console commands are short).
constexpr int32 MaxLength = 8192;

// Commands that act on the editor rather than the game.
const TCHAR* const EditorCommands[] = {TEXT("quit"), TEXT("exit"), TEXT("crash"), TEXT("debug"), TEXT("forcegc"), TEXT("obj")};

// The Python plugin's console command ("py <code>"), and any command under its names ("py.<name>", "python.<name>").
const TCHAR* const PythonNames[] = {TEXT("py"), TEXT("python")};

// Console commands that run text as other console commands: "exec <file>" runs a file of them, "defer <command>"
// runs one a frame later.
const TCHAR* const CommandRunners[] = {TEXT("exec"), TEXT("defer")};

// Functions that run text as a console command: the player controller's ServerExec (an exec function, so also a
// console command), its RPC, SendToConsole and ConsoleCommand, and the system library's ExecuteConsoleCommand.
const TCHAR* const FunctionRunners[] = {TEXT("ServerExec"), TEXT("ServerExecRPC"), TEXT("SendToConsole"), TEXT("ConsoleCommand"),
	TEXT("ExecuteConsoleCommand")};

template<int32 N>
bool IsListed(const TCHAR* const (&List)[N], const FString& Name)
{
	for (const TCHAR* Entry : List)
	{
		if (Name.Equals(Entry, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

bool IsPythonName(const FString& Name)
{
	for (const TCHAR* Entry : PythonNames)
	{
		if (Name.Equals(Entry, ESearchCase::IgnoreCase) || Name.StartsWith(FString(Entry) + TEXT("."), ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

// The commands the player's console runs for Text, as UPlayer::ConsoleCommand reads them: one FParse::Line each
// (split at '|' and line breaks outside quotes, "//" comments dropped; text after a NUL is never seen).
TArray<FString> SplitCommands(const FString& Text)
{
	TArray<FString> Commands;
	const int32 Size = FCString::Strlen(*Text) + 1;
	TArray<TCHAR> Buffer;
	Buffer.SetNumZeroed(Size);
	const TCHAR* Stream = *Text;
	while (FParse::Line(&Stream, Buffer.GetData(), Size))
	{
		Commands.Add(FString(Buffer.GetData()));
	}
	return Commands;
}

// The names a command's handlers can read from its start.
struct FCommandNames
{
	// FParse::Command (engine, editor and plugin commands, "py" among them): after spaces and tabs, a name matches
	// when the next character is not a letter or digit. Read here after any whitespace.
	FString Word;
	// The text after Word and the spaces and tabs FParse::Command skips: what "ke" and "ce" read their names from.
	FString Rest;
	// FParse::Token (an exec function's name for CallFunctionByNameWithArguments): a quoted name loses its quotes.
	FString Token;
	// The console manager's name for a console variable or command: up to the first whitespace, without a
	// trailing '?' or an "@<platform>" part.
	FString Name;
};

FCommandNames ReadNames(const FString& Command)
{
	FCommandNames Names;
	int32 Start = 0;
	while (Start < Command.Len() && FChar::IsWhitespace(Command[Start]))
	{
		++Start;
	}
	int32 End = Start;
	while (End < Command.Len() && FChar::IsAlnum(Command[End]))
	{
		++End;
	}
	Names.Word = Command.Mid(Start, End - Start);
	while (End < Command.Len() && (Command[End] == TEXT(' ') || Command[End] == TEXT('\t')))
	{
		++End;
	}
	Names.Rest = Command.Mid(End);
	const TCHAR* At = *Command;
	FParse::Token(At, Names.Token, true);
	End = Start;
	while (End < Command.Len() && !FChar::IsWhitespace(Command[End]))
	{
		++End;
	}
	Names.Name = Command.Mid(Start, End - Start);
	Names.Name.RemoveFromEnd(TEXT("?"));
	int32 PlatformAt = INDEX_NONE;
	if (Names.Name.FindChar(TEXT('@'), PlatformAt))
	{
		Names.Name.LeftInline(PlatformAt);
	}
	return Names;
}

// Why calling a function named Name by name ("ke", "ce") is refused, or empty: any loaded function of that name
// that play may not call (the call reaches every object that has one).
FString RefusedFunctionName(const FString& Name)
{
	if (IsListed(FunctionRunners, Name))
	{
		return TEXT("it runs a console command");
	}
	if (IsPythonName(Name))
	{
		return TEXT("it runs Python");
	}
	if (Name.IsEmpty() || Name.Len() >= NAME_SIZE)
	{
		return FString();
	}
	const FName Found(*Name, FNAME_Find);
	if (Found.IsNone())
	{
		return FString();
	}
	TArray<UObject*> Functions;
	StaticFindAllObjectsFastSafe(Functions, UFunction::StaticClass(), Found);
	for (const UObject* Object : Functions)
	{
		if (const UFunction* Function = Cast<UFunction>(Object))
		{
			const FString Why = RefusedFunction(*Function);
			if (!Why.IsEmpty())
			{
				return Why;
			}
		}
	}
	return FString();
}

FPlayCommandRefusal Refuse(const FString& Message)
{
	return FPlayCommandRefusal{TEXT("Play.CommandRefused"), Message};
}

FPlayCommandRefusal RefuseTarget(const FString& Message)
{
	return FPlayCommandRefusal{TEXT("Play.TargetOutsidePlay"), Message};
}

// The engine's buffer for the object, class and property names of "set" and "get" (StaticExec, PerformSetCommand):
// a longer name is cut there, so it is cut here the same way before it is looked up.
constexpr int32 PropertyCommandNameSize = 256;

// Why a property command at the start of Command is refused, or nothing: "set"/"setnopec" (UObject StaticExec,
// PerformSetCommand) or "get" whose target, read and looked up as the engine does, is not an object of the play
// session. A name that resolves to nothing does nothing in the engine either.
FPlayCommandRefusal RefusedPropertyTarget(const FString& Command, const UWorld* PlayWorld)
{
	const TCHAR* Args = *Command;
	const bool bSet = FParse::Command(&Args, TEXT("SET")) || FParse::Command(&Args, TEXT("SETNOPEC"));
	const bool bGet = !bSet && FParse::Command(&Args, TEXT("GET"));
	if (!bSet && !bGet)
	{
		return FPlayCommandRefusal();
	}
	TCHAR TargetName[PropertyCommandNameSize];
	if (!FParse::Token(Args, TargetName, UE_ARRAY_COUNT(TargetName), true))
	{
		return FPlayCommandRefusal();
	}
	// Both commands look the name up as a class first. The lookups pass the engine's options; any verbosity but NoLogging
	// takes the same search the engine's takes (NoLogging takes a shortcut), and Log keeps an ambiguous name out of the
	// warnings.
	const UClass* Class = FindFirstObject<UClass>(TargetName, EFindFirstObjectOptions::None, ELogVerbosity::Log);
	if (bGet)
	{
		return Class == nullptr ? FPlayCommandRefusal() : RefuseTarget(FString::Printf(TEXT("'get %s' reads the default object of the "
			"class %s, which is outside the play session. Read play-world values with query_play."), TargetName, *Class->GetPathName()));
	}
	if (Class != nullptr)
	{
		// A class-wide set (GlobalSetProperty): during play it changes only play-in-editor objects of the class, then saves
		// the class's config (SaveConfig on its default object), which writes a config file for a config class.
		if (Class->HasAnyClassFlags(CLASS_Config))
		{
			return RefuseTarget(FString::Printf(TEXT("'set %s' names the class %s, which is saved to a config file: a class-wide set "
				"writes that file outside the play session. Set one play-world object by its path (query_play lists them), or use "
				"play's \"set\" action."), TargetName, *Class->GetPathName()));
		}
		return FPlayCommandRefusal();
	}
	const UObject* Object = FindFirstObject<UObject>(TargetName, EFindFirstObjectOptions::NativeFirst, ELogVerbosity::Log);
	if (Object == nullptr || IsPlaySessionObject(*Object, PlayWorld))
	{
		return FPlayCommandRefusal();
	}
	if (const FString Why = UAssetDocumentSettings::WhyWriteProtected(Object); !Why.IsEmpty())
	{
		return RefuseTarget(FString::Printf(TEXT("'set %s' names %s. %s"), TargetName, *Object->GetPathName(), *Why));
	}
	return RefuseTarget(FString::Printf(TEXT("'set %s' names %s, which is outside the play session%s; set and setnopec change only "
		"objects of the play world, its game instance and local players. Name the play-world object by its path (query_play "
		"lists them), or use play's \"set\" action."), TargetName, *Object->GetPathName(),
		Object->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) ? TEXT(" (a class default object or archetype)") : TEXT("")));
}

// Why the start of Command is refused, or nothing: its name is Python, a command runner or (unless bCodeOnly) an
// editor command, it is "ke"/"ce" calling a function play may not call, or it is a property command whose target is
// outside the play session.
FPlayCommandRefusal RefusedHead(const FString& Command, bool bCodeOnly, const UWorld* PlayWorld)
{
	const FCommandNames Names = ReadNames(Command);
	for (const FString* Name : {&Names.Word, &Names.Token, &Names.Name})
	{
		if (Name->IsEmpty())
		{
			continue;
		}
		if (IsPythonName(*Name))
		{
			return Refuse(FString::Printf(TEXT("'%s' is refused here: play does not run Python (execute_python is the Python tool, when "
				"the user enables it)."), **Name));
		}
		if (IsListed(CommandRunners, *Name) || IsListed(FunctionRunners, *Name))
		{
			return Refuse(FString::Printf(TEXT("'%s' is refused here: it runs other console commands, which play checks only when they "
				"are typed directly."), **Name));
		}
		if (!bCodeOnly && IsListed(EditorCommands, *Name))
		{
			return Refuse(FString::Printf(TEXT("'%s' is refused here: it would affect the editor, not only the game."), **Name));
		}
	}
	// "ke <Object> <Function> <args>" and "ce <Function> <args>" (UEngine::HandleKismetEventCommand, HandleCeCommand).
	FString Called;
	const TCHAR* At = *Names.Rest;
	if (Names.Word.Equals(TEXT("ke"), ESearchCase::IgnoreCase) || Names.Word.Equals(TEXT("kismetevent"), ESearchCase::IgnoreCase))
	{
		FString Object;
		FParse::Token(At, Object, false);
		FParse::Token(At, Called, true);
	}
	else if (Names.Word.Equals(TEXT("ce"), ESearchCase::IgnoreCase))
	{
		FParse::Token(At, Called, true);
	}
	if (!Called.IsEmpty())
	{
		const FString Why = RefusedFunctionName(Called);
		if (!Why.IsEmpty())
		{
			return Refuse(FString::Printf(TEXT("'%s' cannot call %s here: %s."), *Names.Word, *Called, *Why));
		}
	}
	return RefusedPropertyTarget(Command, PlayWorld);
}

FPlayCommandRefusal RefusedLine(const FString& Text, int32 Depth, const UWorld* PlayWorld);

// Why one command (one FParse::Line) is refused, or nothing. Its start is checked as a command; so is every later
// word (after whitespace, a quote or '|'), since a command can hand the rest of its line on as another command
// (ServerExec, SetBind, "defer", a game's own exec functions); an escaped word is checked again unescaped, as a whole
// line with every check the typed line gets.
FPlayCommandRefusal RefusedCommand(const FString& Command, int32 Depth, const UWorld* PlayWorld)
{
	FPlayCommandRefusal Why = RefusedHead(Command, false, PlayWorld);
	for (int32 Index = 1; !Why.IsRefused() && Index < Command.Len(); ++Index)
	{
		const TCHAR Before = Command[Index - 1];
		if ((FChar::IsWhitespace(Before) || Before == TEXT('"') || Before == TEXT('|')) && !FChar::IsWhitespace(Command[Index]))
		{
			Why = RefusedHead(Command.Mid(Index), true, PlayWorld);
		}
	}
	const TCHAR* At = *Command;
	while (!Why.IsRefused())
	{
		while (FChar::IsWhitespace(*At))
		{
			++At;
		}
		if (*At == TEXT('\0'))
		{
			break;
		}
		const TCHAR* Raw = At;
		FString Word;
		FParse::Token(At, Word, true);
		if (At == Raw)
		{
			++At;
			continue;
		}
		const FString RawWord = FString::ConstructFromPtrSize(Raw, UE_PTRDIFF_TO_INT32(At - Raw));
		if (Word != RawWord && Word != RawWord.TrimQuotes())
		{
			Why = Depth >= MaxDepth ? Refuse(TEXT("The command hands on text too deeply nested to check.")) : RefusedLine(Word, Depth + 1, PlayWorld);
		}
	}
	return Why;
}

// Why Text (a line of commands, as typed or as one of its words unescapes) is refused, or nothing: a line naming a
// protected settings class, a line too long to check, or any command the console would run for it.
FPlayCommandRefusal RefusedLine(const FString& Text, int32 Depth, const UWorld* PlayWorld)
{
	if (const FString Why = UAssetDocumentSettings::WhyNamesProtectedSettings(Text); !Why.IsEmpty())
	{
		return Refuse(Why);
	}
	if (Text.Len() > MaxLength)
	{
		return Refuse(FString::Printf(TEXT("The command line is longer than %d characters; send shorter commands."), MaxLength));
	}
	for (const FString& Command : SplitCommands(Text))
	{
		const FPlayCommandRefusal Why = RefusedCommand(Command, Depth, PlayWorld);
		if (Why.IsRefused())
		{
			return Why;
		}
	}
	return FPlayCommandRefusal();
}
} // namespace PlayCommandGuardLocal

FPlayCommandRefusal RefusedConsoleCommand(const FString& Command, const UWorld* PlayWorld)
{
	return PlayCommandGuardLocal::RefusedLine(Command, 0, PlayWorld);
}

bool IsPlaySessionObject(const UObject& Object, const UWorld* PlayWorld)
{
	if (Object.HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) || Object.IsA<UField>())
	{
		return false;
	}
	// The play world and everything in it (levels, actors, components, world subsystems): the packages play duplicates
	// for the session, which GlobalSetProperty also limits a class-wide set to during play.
	const UPackage* Package = Object.GetPackage();
	if (Package != nullptr && Package->HasAnyPackageFlags(PKG_PlayInEditor))
	{
		return true;
	}
	const UGameInstance* GameInstance = PlayWorld != nullptr ? PlayWorld->GetGameInstance() : nullptr;
	if (GameInstance == nullptr)
	{
		return false;
	}
	if (&Object == GameInstance || Object.IsIn(GameInstance))
	{
		return true;
	}
	for (const ULocalPlayer* Player : GameInstance->GetLocalPlayers())
	{
		if (Player != nullptr && (&Object == Player || Object.IsIn(Player)))
		{
			return true;
		}
	}
	return false;
}

FString RefusedFunction(const UFunction& Function)
{
	using namespace PlayCommandGuardLocal;
	if (IsListed(FunctionRunners, Function.GetName()))
	{
		return TEXT("it runs a console command");
	}
	const UPackage* Package = Function.GetPackage();
	if (Package != nullptr && Package->GetName().StartsWith(TEXT("/Script/Python"), ESearchCase::IgnoreCase))
	{
		return TEXT("it runs Python (a Python script library function)");
	}
	if (Function.HasAnyFunctionFlags(FUNC_Exec))
	{
		for (TFieldIterator<FProperty> It(&Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			const bool bInput = !It->HasAnyPropertyFlags(CPF_ReturnParm) && (!It->HasAnyPropertyFlags(CPF_OutParm) || It->HasAnyPropertyFlags(CPF_ReferenceParm));
			if (bInput && (It->IsA<FStrProperty>() || It->IsA<FNameProperty>() || It->IsA<FTextProperty>()))
			{
				return FString::Printf(TEXT("it is a console command (an exec function) that takes text (%s), which it can run as another "
					"command; type the command with play's \"console\" instead"), *It->GetName());
			}
		}
	}
	return FString();
}

FPlayCommandRefusal RefusedKeyBinding(const FKey& Key, const UWorld* PlayWorld)
{
	using namespace PlayCommandGuardLocal;
	// The class default objects hold the bindings read from the config; each player input copies them.
	for (TObjectIterator<UPlayerInput> It(RF_NoFlags); It; ++It)
	{
		for (const FKeyBind& Bind : It->DebugExecBindings)
		{
			if (Bind.Key != Key)
			{
				continue;
			}
			// UPlayerInput::ExecInputCommands: each command of the binding, "OnRelease <command>" on release.
			for (const FString& Command : SplitCommands(Bind.Command))
			{
				const TCHAR* At = *Command;
				FParse::Command(&At, TEXT("OnRelease"));
				FPlayCommandRefusal Why = RefusedLine(FString(At), 0, PlayWorld);
				if (Why.IsRefused())
				{
					Why.Message = FString::Printf(TEXT("Pressing %s runs its debug key binding '%s'. %s"), *Key.GetFName().ToString(), *Bind.Command,
						*Why.Message);
					return Why;
				}
			}
		}
	}
	return FPlayCommandRefusal();
}
}
