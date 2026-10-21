# Skeleton Profile

Classes: /Script/Engine.Skeleton

Exact profile for Skeletons: what the Skeleton editor authors. Update only: a Skeleton comes from a Skeletal Mesh import, so `Create` (and `CreateOrUpdate` of a missing Skeleton) fails with `Skeleton.CreateUnsupported`. Engine and plugin Skeletons are read-only: to add slots or sockets to one, copy it into `/Game` with `duplicate_asset`, list the original in the copy's `Properties.CompatibleSkeletons` (its animations stay usable) and edit the copy. Bones are read only; `inspect_asset_document_target` on the Skeleton lists its bones, slot groups and sockets (Skeleton facts) and `SkeletonDetails.BoneCount`. Omitted Body key = keep; a present key is the complete list of its kind (`[]` / `{}` clears what the editor can clear). Names identify entries; `RenamedFrom` renames an entry (as the editor's rename) instead of removing it and adding a new one. List order has no meaning.

Apply works with the Skeleton open in the Skeleton editor: the skeleton tree and the preview show the applied (or rolled-back) sockets.

```jsonc
"Body": {
  "SlotGroups": [{"Name": "DefaultGroup", "Slots": ["DefaultSlot", "UpperBody"]},
                 {"Name": "Face", "Slots": [{"Name": "Mouth", "RenamedFrom": "Jaw"}]}],
  "Sockets": [{"Name": "hand_r_weapon", "Bone": "hand_r", "Location": {"X": 0, "Y": 5, "Z": 0},
               "Rotation": {"Pitch": 0, "Yaw": 90, "Roll": 0}, "Scale": {"X": 1, "Y": 1, "Z": 1}, "ForceAlwaysAnimated": true}],
  "VirtualBones": [{"Name": "VB hand_ik", "Source": "pelvis", "Target": "hand_r"}],
  "BlendProfiles": [{"Name": "UpperBody", "Mode": "WeightFactor", "Bones": {"spine_01": 1.5}}],
  "BoneRetargeting": {"pelvis": "AnimationScaled"},
  "Curves": [{"Name": "CTRL_Blink", "Material": false, "MorphTarget": true}]
},
"Properties": {"CompatibleSkeletons": ["/Game/Characters/SK_Other_Skeleton.SK_Other_Skeleton"]}
```

- `SlotGroups` (Anim Slot Manager): `DefaultGroup` must stay (`Skeleton.DefaultGroupRequired`; a Skeleton that never had a slot extracts `[]`); slot and group names are unique across both (`Skeleton.DuplicateName`). Moving a slot to another group keeps it. A slot that an AnimMontage or an Animation Blueprint Slot node uses cannot be renamed or removed: `Skeleton.SlotInUse` names the users. AnimMontage `SlotAnimTracks` must name a slot listed here.
- `Sockets`: `Bone` must exist (`Skeleton.BoneNotFound`). `Location` / `Rotation` / `Scale` / `ForceAlwaysAnimated` omitted on a new socket = 0 / 0 / 1 / true, on an existing one = keep. A saved socket on no bone (`"Bone": "None"`, as some marketplace Skeletons have) is read with a `Skeleton.SocketWithoutBone` warning (the rest of the Skeleton is read as usual) and kept as it is while the document leaves it on `"None"`; a new socket, or one moved, on `"None"` fails with `Skeleton.InvalidSocket`.
- `VirtualBones`: names start with `"VB "` (`Skeleton.InvalidVirtualBone`); `Source` / `Target` are bones or virtual bones. Changing `Source` / `Target` replaces the virtual bone.
- `BlendProfiles`: `Mode` is `TimeFactor`, `WeightFactor` or `BlendMask`; `Bones` lists the non-default scales (default 1, BlendMask 0); listed profiles' other bones are reset.
- `BoneRetargeting`: translation retargeting mode per bone (`Animation`, `Skeleton`, `AnimationScaled`, `AnimationRelative`, `OrientAndScale`); bones not listed use `Animation`.
- `Curves`: curve metadata (`Material`, `MorphTarget` flags). Curve keys inside animations are not touched.
- Other Details settings (`CompatibleSkeletons`, `PreviewForwardAxis`, ...) are top-level `Properties`.
