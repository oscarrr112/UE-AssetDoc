// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

class UFunction;
class UObject;
class UWorld;
struct FKey;

// What the play tools refuse to run in the game: Python (execute_python is the one Python tool, whatever the
// plugin's settings), commands that act on the editor rather than the game, every way of handing a command or
// Python text to something that runs it (ServerExec, SendToConsole, ExecuteConsoleCommand, "defer", "ke"/"ce"
// calls, the Python script library, debug key bindings), and property commands ("set", "setnopec", "get") whose
// target is not an object of the play session.
//
// These limits keep honest clients and accidental commands to the game. They are not a sandbox: a client that can
// reach the editor can make it run code (a Blueprint it authors runs in play), which is why the endpoint itself is
// the security boundary (see SECURITY.md).
namespace UE::AssetDocumentLevel::Private::Play
{
// A refusal: Code is the diagnostic code (Play.CommandRefused, or Play.TargetOutsidePlay for a property command whose
// target is outside the play session), Message says why. Empty when the command may run.
struct FPlayCommandRefusal
{
	FString Code;
	FString Message;

	bool IsRefused() const { return !Message.IsEmpty(); }
};

// Why a console command line is refused, or nothing. The line is read as the player's console reads it: split into
// commands at '|' and line breaks outside quotes, with "//" comments dropped, and each command matched as its
// handlers match it (any whitespace before the name; a name ends at the first character that is not a letter or
// digit, so "py\t...", "py.x" and "\"py\"" are all "py"). Every command of the line is checked, and every word of a
// command is checked again as the start of a command, since a command can pass the rest of its line on as another
// command. A word that unescapes to other text (FParse::Token with escapes, as "set" reads its object name) is checked
// again, unescaped, as a whole line. Also refused: a line naming a protected settings class, and "set", "setnopec"
// or "get" whose target, resolved as the engine resolves it, is not an object of the play session (see
// IsPlaySessionObject). PlayWorld is the running play world, or null before a session starts (no object is in play
// then; the commands are checked again when they run).
FPlayCommandRefusal RefusedConsoleCommand(const FString& Command, const UWorld* PlayWorld);

// Whether Object belongs to the play session of PlayWorld: an object of a play-in-editor package (the play world, its
// levels, actors and components, and what they own) or the session's game instance, its local players and what they
// own (their subsystems). Class default objects, archetypes and classes never are.
bool IsPlaySessionObject(const UObject& Object, const UWorld* PlayWorld);

// Why play's "call" may not call Function, or empty: it runs a console command or Python (ServerExec,
// SendToConsole, ExecuteConsoleCommand, a Python script library function), or it is a console command (an exec
// function) that takes text, which it can pass on as another command.
FString RefusedFunction(const UFunction& Function);

// Why play's "input" may not press Key, or nothing: a debug key binding of the player input runs a command that
// RefusedConsoleCommand refuses when the key is pressed or released.
FPlayCommandRefusal RefusedKeyBinding(const FKey& Key, const UWorld* PlayWorld);
}
