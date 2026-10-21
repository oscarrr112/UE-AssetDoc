# AnimMontage Profile

Classes: /Script/Engine.AnimMontage

Class `/Script/Engine.AnimMontage`. `Body` accepts these canonical keys, all writable; unknown keys fail with `AnimMontage.UnknownBodyKey` and `_Skipped` with `AnimMontage.ExtractOnlyBodyKey`:

Apply works with the Montage open in its Animation editor: the timeline, notify and Montage Sections panels and the preview show the applied (or rolled-back) Montage.

- Assets and structure: `Skeleton`, `PreviewMesh`, `Curves`, `SlotAnimTracks`, `CompositeSections`, `Notifies`, `NotifyStates`, `NotifyTracks` (rows below).
- Settings regions (objects; exact fields as extract emits them): `Preview` (`PreviewMesh`, `PreviewBasePose`), `Blend` (`BlendInTime`, `BlendOutTime`, `BlendModeIn/Out`, `BlendOutTriggerTime`, `bEnableAutoBlendOut`, `BlendInOption/BlendOutOption`, `BlendInCustomCurve/BlendOutCustomCurve`, `BlendProfileIn/Out`), `Sync` (`SyncGroup`, `SyncSlotIndex`), `RootMotion` (`bEnableRootMotionTranslation/Rotation`, `RootMotionRootLock`), `TimeStretch` (`TimeStretchCurveName`, `SamplingRate`, `CurveValueMinPrecision`), `Metadata` (`[{Class, Properties}]`), `SectionMetadata` (`{SectionName: [{Class, Properties}]}`). `References` and `Preview` also accept `Skeleton` / `PreviewMesh` as aliases of the top-level keys.
- Plain reflected Montage fields (`RateScale`, `bLoop`, ...) go in top-level `Properties`. `Definitions` must be empty.
- Region semantics: an omitted key preserves the current value; a present array replaces that whole region; `[]` explicitly clears it.
- A new Montage needs `Skeleton` (it is the factory context); everything else can be in the same create apply.

Rows are exact: every listed field is required and unknown fields are rejected (`AnimMontage.InvalidCanonicalShape`).

- `Skeleton`, `PreviewMesh`: asset path string or `null` (`"/Game/Char/SK_Hero"` is normalized to `"/Game/Char/SK_Hero.SK_Hero"`). These are plain strings, not `AssetRef` objects.
- `SlotAnimTracks[]`: `{"SlotName": string, "Segments": [Segment]}`; slot names unique and each must exist in the Skeleton's slot groups: `AnimMontage.UnknownSlot` at `/Body/SlotAnimTracks/<i>/SlotName` names the Skeleton and lists its slots. A new Montage without `SlotAnimTracks` keeps the engine's one `DefaultSlot` track, so its Skeleton needs `DefaultSlot` too (`AnimMontage.UnknownSlot` at `/Body/SlotAnimTracks`). Add a missing slot with a Skeleton document (`Body.SlotGroups`, section Skeleton) first (an engine or plugin Skeleton is read-only: copy it into `/Game` with `duplicate_asset` and use the copy); `inspect_asset_document_target` on the Skeleton lists its slot groups. Sections and notifies sit on a slot: when a track is replaced (e.g. a new Montage whose first track is not `DefaultSlot`, since its `Default` section is on `DefaultSlot`), author `CompositeSections` (and notifies) on the new slots in the same document.
- Segment: `{"AnimReference": "/Game/Anim/AS_Attack.AS_Attack", "StartPos": float >= 0, "AnimStartTime": float >= 0, "AnimEndTime": float > AnimStartTime, "AnimPlayRate": non-zero float, "LoopingCount": int >= 1}`. A segment lasts (AnimEndTime - AnimStartTime) * LoopingCount / |AnimPlayRate * the sequence's own `RateScale`| on the montage timeline (a 1.3 RateScale sequence, 0-1.0 s at AnimPlayRate 1.15, lasts 0.669 s); `inspect_asset_document_target` on the Montage lists each segment's `Duration` and `EndPos` (`Animation.Segments`).
- Segments are contiguous, as the editor keeps them: `Segments[0].StartPos` is 0 and each next `StartPos` is the previous segment's end (`StartPos + LoopingCount * (AnimEndTime - AnimStartTime) / |AnimPlayRate * the animation's RateScale|`, float32). A `StartPos` within 1e-5 s of that end is taken as the end (the no-op check accepts it too); otherwise `AnimationProvider.Montage.SegmentGap` at `.../Segments/<j>/StartPos` says whether it leaves a gap or overlaps and gives the `StartPos` to use. Other bad segment fields fail with `AnimationProvider.Montage.InvalidSegment` at that field (an `AnimEndTime` past the animation's end gives its length and the overflow).
- Segment correspondence is positional, like the editor's: segment `j` of a slot in the document is segment `j` of that slot in the Montage, even when several segments use the same animation. Sections and notifies you do not list keep their link (Relative and Proportional times move with their segment's new `StartPos`). If such a kept element's segment is removed, or its time would land on another segment, Apply fails with `AnimationProvider.Montage.SegmentCorrespondence`: list `CompositeSections` / `Notifies` / `NotifyStates` in the same document.
- `CompositeSections[]`: `{"Name": string, "NextSectionName": string | null, "LinkableTime": float >= 0, "SlotName": string | null, "LinkMethod": 0 | 1 | 2}` (`LinkMethod`: 0 Absolute, 1 Relative, 2 Proportional). Names unique (`AnimMontage.DuplicateSection`); `NextSectionName` must name a section in the same array or be `null` (`AnimMontage.InvalidSectionLink`).
- `Notifies[]`: `{"Time": float >= 0, "TrackIndex": int >= 0, "Object": {"Class": "/Script/Engine.AnimNotify" or "/Game/.../BP_Notify.BP_Notify_C", "Properties": {...}}}`. `Class` must be a full object path (a bare `/Game/BP_Notify` is rejected).
- `NotifyStates[]`: same as `Notifies` plus `"Duration": float > 0`.
- `Notifies` and `NotifyStates` are compared as sets: list them in any order (the editor keeps them sorted by time); a notify's `Properties` lists only what it sets (an omitted property keeps the class default). A mismatch names the document row with the read-back notify of the same class and track nearest in time. Two notifies (or states) cannot start at the same `Time` on one notify track: give one another `TrackIndex` or `Time`.
- `NotifyTracks[]`: `{"Name": string}`, the Montage's notify track rows in order (`TrackIndex` i is `NotifyTracks[i]`). Names are labels, as in the editor (any text, duplicates allowed); a new track gets the editor's default colour, an existing one keeps its colour. Omitted, the tracks are kept (a new Montage has one track, `"1"`). A present array needs at least one track (`AnimMontage.NotifyTrackRequired`) and `Notifies` or `NotifyStates` in the same document (`AnimMontage.NotifyTracksNeedNotifies`), since removing a track moves notifies; Extract gives all three. Every `TrackIndex` must name a track (`AnimMontage.NotifyTrackMissing`, which lists the tracks). A notify must end within the length the `SlotAnimTracks` give (`AnimMontage.NotifyBeyondLength`). A Create can list notifies with its slots and sections. On an existing Montage, Validate runs the same timeline check as Apply (segments, sections, correspondence, notify tracks and lengths), so it reports what Apply would.
- `Curves[]`: exact float-curve payload `{Name, Flags, Color, Comment, DefaultValue, PreExtrapolation, PostExtrapolation, Keys}` where colors, times, values, and tangents are float32 bit patterns (`{"Bits": uint32}`) and `DefaultValue` is `{"HasValue": bool, "Float32Bits": uint32 | null}`. Key times must be strictly increasing. Copy curves from `extract_asset_document` rather than writing them by hand.

Times, rates and settings are float32, as everywhere (core guide): write `0.1`, Extract returns `0.1`, and the no-op check compares at float32 precision. Only `Curves` carry raw bit patterns.

Example (update of an existing Montage):

```json
{
  "SchemaVersion": 1,
  "Target": "/Game/Anim/AM_Attack",
  "Class": "/Script/Engine.AnimMontage",
  "Action": "Update",
  "Definitions": {},
  "Properties": {},
  "Body": {
    "Skeleton": "/Game/Char/SK_Hero.SK_Hero",
    "PreviewMesh": null,
    "SlotAnimTracks": [
      {
        "SlotName": "DefaultSlot",
        "Segments": [
          { "AnimReference": "/Game/Anim/AS_Attack.AS_Attack", "StartPos": 0.0, "AnimStartTime": 0.0, "AnimEndTime": 1.2, "AnimPlayRate": 1.0, "LoopingCount": 1 }
        ]
      }
    ],
    "CompositeSections": [
      { "Name": "Default", "NextSectionName": null, "LinkableTime": 0.0, "SlotName": "DefaultSlot", "LinkMethod": 0 }
    ],
    "Notifies": [
      { "Time": 0.4, "TrackIndex": 0, "Object": { "Class": "/Game/Anim/Notifies/BP_HitNotify.BP_HitNotify_C", "Properties": {} } }
    ],
    "NotifyStates": [
      { "Time": 0.3, "Duration": 0.5, "TrackIndex": 0, "Object": { "Class": "/Script/Engine.AnimNotifyState_Trail", "Properties": {} } }
    ],
    "NotifyTracks": [{ "Name": "1" }]
  }
}
```
