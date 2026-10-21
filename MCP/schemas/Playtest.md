# Playtest Guide

Test a game in Play In Editor (PIE) as a human tester would. Three tools, no execute_python:

- `play`: the session (start, stop, status, pause, resume), time (wait), input, console commands,
  `call` / `set` (cheats).
- `query_play`: read the running game world (actors, values, AI, montages, effects, sounds) and the play log.
- `get_viewport_screenshot`: the game as the player sees it.

The editor world is never changed: query_level / edit_level / open_level refuse during play
(Level.PlaySessionActive), and nothing done in the game is kept after stop.

A typical loop:

```
play {"action": "start", "paused": true} -> session.player.pawn
query_play {"class": "BP_Enemy"} -> enemies, their vitals
play {"action": "input", "inputs": [{"input_action": "IA_Move", "value": {"X": 0, "Y": 1}, "duration": 1.5}]}
play {"action": "wait", "until": {"actor": "game_mode", "property": "bGameOver", "value": true}, "timeout": 30}
get_viewport_screenshot {}
play {"action": "stop"} -> session log summary
```

## play

Every result has `payload.session` (status, below) and `payload.log`: `errors`, `warnings`, up to 10
`lines` (errors and warnings logged during the call, each {seq, t, category, verbosity, message}),
`print` (Print String lines) and `last_seq`.

| action | fields | result payload |
|---|---|---|
| start | `mode`: window (default, a 1280x720 window), viewport (the active Level Editor viewport), simulate (no player pawn); `map` (+ `save_first`) opens a map first; `paused`; `console` (a command or array, run before the first frame); `allow_blueprint_errors` | session, `console` results |
| stop | | `played_real_seconds`, `played_unpaused_real_seconds`, `final_world_time`, the session's `log` summary |
| status | | `session` |
| pause / resume | | `session` (state paused / playing) |
| wait | `seconds` (game time) and/or `until` (condition or array: any); `timeout` (real s, default 10, max 60) | `satisfied`, `reason` (condition, time, timeout, session_ended), `condition`, `observed`, `waited_game_seconds`; not met: `last_observed` + warning Play.WaitTimedOut |
| input | `inputs` (below), `settle` (game s to keep playing after the last input) | `inputs[]`: what each input reached |
| console | `command` (`slomo 0.5`, `god`, `ke BP_Boss_C_0 Enrage`; several separated by `\|`) | `output` (the command's), `logged`, `resolved` |
| call | `actor`, `function`, `args` (below) | `return`, `outputs`, `defaulted` |
| set | `actor`, `values`, `location`, `rotation` (below) | `changes` [{path, before, after}], `actor` |
| reset_input_settings | (no session needed) | player key remaps reset; saved file `deleted` |

`session`: `state` (stopped, starting, playing, paused, simulating, stopping), `mode`, `world`, `map`,
`world_time` (game s), `real_time` (real s since start, pauses included), `unpaused_real_time`
(real s the game ran), `time_dilation` (when not 1),
`game_mode`, `player` {`controller`, `pawn` (the query_play actor summary)}, `held_inputs`, `log`
{`session_errors`, `session_warnings`, `session_start_seq`, `last_seq`}.

start compiles modified Blueprints like the Play button; compile errors refuse it with
**Play.BlueprintCompileErrors** (details.blueprints) instead of the
editor's prompt: fix them, or pass `allow_blueprint_errors: true`. No Player Start: the player starts
at the editor camera (warning Play.NoPlayerStart). No pawn: warning Play.NoPlayerPawn (the
GameMode's Default Pawn Class). Play.StartFailed lists the errors the editor logged.

A paused game runs only during `wait` and `input`. Start
with `paused: true`: it pauses after BeginPlay, before the first frame (`world_time` 0);
otherwise it runs between your calls. A
game that reloads its map (Open Level, restart) stays the same session: `world_time`
starts again from 0.

### call and set

Cheats; both run at once, also while paused; `actor` is a reference (query_play). `call` runs a
Blueprint function, custom event or UFUNCTION of the actor or a component
(`"CharacterMovement.StopMovementImmediately"`); `args` by name
(`{"Amount": 50, "Target": "player"}`) or an array in order, in value forms (structs `{"X","Y","Z"}`,
actors by reference); inputs left out take the pin default (`defaulted`). Errors:
Play.FunctionNotFound, Play.FunctionNotCallable (latent, delegates), Play.ComponentNotFound,
Play.InvalidArgument. `set` writes like a Set node: `values` {path: value} (paths as `properties`:
`"BPC_Combat.bAutoAttack"`); `location` / `rotation` teleport (world cm / degrees; members left out kept). All values are checked before any is
written (Play.PropertyNotFound / Play.InvalidValue). Warning Play.NotMoved: not Movable.

### Conditions (wait until)

- `{"actor": ref, "property": path, "op": "<=", "value": 0}`: a value (op ==, !=, <, <=, >, >=;
  default ==). Numbers compare numerically, everything else by its JSON value.
- `{"actor": ref, "exists": false}`: the actor is gone; `true`: it exists.
- `{"count": {"class": "BP_Enemy", "tag"?, "name"?, "near"?}, "op": "==", "value": 0}`: how many
  actors match (the query_play filters).
- `{"log": "text", "category"?: "LogBlueprintUserMessages"}`: a log line logged after the wait began.
- `{"session": "ended"}`: the session stopped (the game quit itself; pending waits return reason
  `session_ended`).

### Input

Each entry of `inputs` names one of:

- `input_action`: an Enhanced Input action, by path (`/Game/Input/IA_Jump`) or asset name
  (`IA_Jump`; several with that name: the one the player's contexts map). `value`:
  true/1 (default), a number for Axis1D, `{"X", "Y"}` for Axis2D (Move: Y forward, X right; Look: X
  yaw, Y pitch), `{"X","Y","Z"}` for Axis3D. It reaches the action as from a mapped key after
  the mapping's modifiers: give the final value. The action's own triggers apply;
  a mapping's triggers (such as Pressed on LMB) do not. It bypasses context priority and
  consumption: test context switches with `key`.
- `key`: a key or axis sent as a physical key would be (through the player's mapping contexts,
  their triggers and modifiers): `SpaceBar`, `W`, `LeftShift`,
  `LeftMouseButton`, `MouseX` / `MouseY` (value: per-frame delta), `Gamepad_LeftX` (-1..1),
  `Gamepad_Left2D` / `Gamepad_Right2D` / `Mouse2D` (`{"X", "Y"}`), `Gamepad_FaceButton_Bottom`. An axis reaches its pair's mapping (MouseX -> Mouse2D: `via`); remapped keys count.

`duration`: game seconds held (default 0: one frame, a tap; a Pressed trigger fires once). `at`:
game seconds after the request it starts (entries run in parallel: a combo is entries with
different `at`). `state: "down"` presses and keeps it held after the request
(`session.held_inputs`; e.g. charge or block) until an entry with
`state: "up"` (no value or duration) or stop, across pauses and calls (Hold triggers count game time). Errors: Play.InputHeld, Play.InputNotHeld.

Each `inputs[]` result: `input_action` and `mapped_keys`, or `key`; `start` / `end`, `held_frames`;
`actions[]` reached, each with `triggered`, `triggered_frames` and trigger `events` (Started,
Triggered, Ongoing, Completed, Canceled); `listeners` bound to them ("IA_Jump: Triggered on
BP_Hero_C_0"). `triggered: false` with listeners: the triggers did not fire (hold longer, or
tap); no listeners: nothing reacts. Warnings:
**Play.ActionNotMapped** (no context of the player maps it: a player could not press it), **Play.KeyNotMapped**, **Play.PlayerInputBlocked** (the game disabled the player's input or
ignores move / look input, as in a cinematic: wait, retry). Errors: Play.InputActionNotFound,
Play.InputActionAmbiguous, Play.UnknownKey, Play.NoPlayer, Play.InputValueTypeMismatch.

## query_play

Actors (default `what`): filter by `class` (subclasses unless `exact_class`), `name` (object name or
label, `*` `?` wildcards), `tag`, `near` {`location` | `actor`, `radius`} (sorted by distance, adds
`distance`); or list `actors` by reference. `limit` (default 50, max 500) and `offset` page; `total`
counts all matches.

Actor references: `"player"` (the player's pawn), `"controller"`, `"game_mode"`, `"game_state"`,
`"player_state"`, `"hud"`, `"camera"`; an object name (`BP_Enemy_C_2`), a label, or a path: the
editor path query_level returned maps to the actor's game copy.
Errors: Play.ActorNotFound (close names), Play.ActorRefAmbiguous.

Each actor: `name`, `label`, `class`, `path` (unique game object path), `location`, `rotation`,
`velocity` / `speed`, `hidden`, `tags`; pawns: `controller` {name, class, player}; controllers: `pawn`; `vitals`: up to 12
health-like values (Health, HP, Shield, ...) of the actor and its Blueprint
components (`"BPC_Combat.Health"`); `montage` {asset, position, section} while one plays; `ai`
{controller, behavior_tree, running, active_node, active_tasks, blackboard {key: value},
move_status, focus}. `properties: ["BPC_Combat.bAttacking", "CharacterMovement.MaxWalkSpeed", "Controller.PlayerState"]` adds `values` (and `value_errors` for paths that do not exist): a
component by name, then properties, struct members and object references. `detail`: "components"
adds `components`; "properties" (= "full") also `properties` (values that differ from the class
default; `diff_only: false` for all), as in query_level. Values use the AssetDocument value forms.

Log (`what: "log"`): `since` "session" (default), "all" or a seq (an earlier `last_seq`: only what is
new); `category` (name or array: LogBlueprintUserMessages is Print String; "Accessed None" and
infinite loops are PIE errors; LogOutputDevice has ensures), `verbosity` (least severe kept: error,
warning, display, log (default), verbose), `text`, `limit` (newest kept; `skipped`). Readable after stop.

Effects (`what: "effects"`): active Niagara and Cascade components (`include_inactive` for all):
`component`, `type`, `asset`, `owner`, `attached_to`, `location`, `active`; `limit` / `offset`.

Audio (`what: "audio"`): `components` playing now {component, sound, owner, location}; `sounds`
started since `since` (as for the log; Play Sound at Location / 2D too), oldest first: {seq, t,
sound, owner, component, location, finished, finished_t, `virtual` (started inaudible: out of range
or concurrency)}; `limit` (newest kept). Both filter by `name` (component, owner, asset or sound;
wildcards) and `near`. Without audio (-nosound): none, warning Play.AudioDisabled.

Input (`what: "input"`, no fields): `contexts` [{context, priority, mappings [{action, key,
asset_key}]}], `user_settings` {saved, customized}; saved remaps persist across sessions
(warning Play.InputUserSettingsOverride; reset: play reset_input_settings). Fields are flat
and belong to their `what`; another what's field is refused.

## get_viewport_screenshot

`view`: "auto" (default: the game while a session runs, else the editor viewport), "game", "editor"
(Simulate has no game view). `hud` (default true) adds UMG widgets (rendered off screen; without them: warning Play.HudNotCaptured). `format` "jpeg" (default; `quality` 1-100,
default 70) or "png" (lossless); `max_width` (64-3840, default 1024) scales down.

## Pitfalls

- MoveTo needs a navigation mesh. With no `NavMeshBoundsVolume` the enemy stands still (`ai.move_status` `Idle`, `location` unchanged). Stop, then `edit_level` spawn `/Script/NavigationSystem.NavMeshBoundsVolume` scaled over the floor (Behavior Tree guide).
- `seconds` and `duration` are game time; `timeout` is real time.
- Spawned actors get names like BP_Enemy_C_3: across sessions use `class`/`tag` filters or labels
  of placed actors, not remembered names.
- `console` refuses editor commands (quit, exit, py, obj, ...): Play.CommandRefused. `ke <actor>
  <event>` targets the game copy (name, label or path). Warning Play.CommandNotRecognized: the game
  did not know it.

## Diagnostics

Also: Play.NoSession, Play.AlreadyRunning, Play.EditorBusy (a transaction or modal dialog is open),
Play.NoViewport, Play.Timeout, Play.SessionEnded, Play.NoGameViewport, Play.CaptureFailed,
Play.UnknownOperation, Play.InvalidRequest.
