# VFX (JRPGVFX)

**Status: built; the two automation tests pass. The first effects in use are the three Spirit auras
([The Spirit aura](#the-spirit-aura)); the battle hook for them is written, waiting for the next build.**
Sections marked *planned* are decisions, not code.

The remake's effects are **new, modern effects**: particles flying, a fire tornado on the enemy, wind,
lightning. Nothing is ported from the PS1 look. The original is only read for **when and where** each
effect fires (see [What comes from the original](#what-comes-from-the-original)).

| Piece | Where | What it does |
|---|---|---|
| `UJRPGVFXRecipe` | `Public/VFX/JRPGVFXRecipe.h` | One effect as data: a timeline of layers. A Data Asset in the project |
| `FJRPGVFXLayer` and friends | `Public/VFX/JRPGVFXTypes.h` | The layer, its animated tracks, the firing context, the `DT_VFX` row |
| `UJRPGVFXSubsystem` | `Public/VFX/JRPGVFXSubsystem.h` | Plays recipes: spawns, anchors, animates and pools the layers |
| `UJRPGAnimNotify_VFX` | `Public/VFX/JRPGAnimNotify_VFX.h` | Fires a recipe from a frame of a clip |
| `JRPG.VFX.Tracks`, `JRPG.VFX.RecipeLength` | `Private/Tests/JRPGVFXTests.cpp` | Tests of the pure-data part (eases, tracks, keys, recipe length) |

The plugin ships **no** effect assets. Recipes, materials, meshes and Niagara systems live in the project.

---

## The idea: an effect is a stack of layers

A modern effect is never one thing. A mini fire tornado on a monster is:

| Layer | What it is | Who makes it look good |
|---|---|---|
| The funnel | 2 or 3 mesh shells with fire scrolling, eroding and twisting | A **material** with procedural HLSL |
| Embers, smoke, sparks | Particles spiralling up | A **Niagara** system |
| The glow | An orange point light pulsing | The recipe (a `Light` layer) |
| The scorch | The body of the target flashing | The recipe (a `BodyParam` layer) |
| The punch | A camera shake | The recipe (a `CameraShake` layer) |

So the split of work is:

- **Materials own the appearance**: noisy edges, erosion, plasma arcs, glow, fading with age. Written as
  HLSL in `Shaders/` and called from a Custom node, the same way `JRPGMist.ush` is.
- **Niagara owns particle motion**: spawn, velocity, lifetime, size, colour over life. A few generic
  systems driven by user parameters, not one hand-built system per spell.
- **C++ owns orchestration**: what enters when, anchored to whom, with which parameters, and cleaning up.
- **The recipe is the glue**, and it is data: tuning an effect does not need a rebuild.

---

## The recipe

Create one in the Content Browser: **Miscellaneous › Data Asset › JRPGVFXRecipe**.

| Field | Meaning |
|---|---|
| `Duration` | Length of the effect in seconds. `0` = the end of the last layer that has its own duration |
| `bLooping` | The effect stays until `StopEffect` (an aura, a status). See [Looping](#looping-and-stopping) |
| `FadeOutTime` | On a soft stop, how long the effect takes to disappear |
| `MinPlayTime` | The least the effect plays: a soft stop asked earlier waits until then before fading. An immediate stop does not wait. `0` = none |
| `CueTime` | The moment that marks the recipe (the Absorb flash). A caller can set `Context.TimeScale = CueTime / seconds` so that moment falls on a game event. `0` = none |
| `FadeParameter` | The scalar the `Mesh` layers' materials receive during the fade (`JRPGVFXFade`, 1 → 0) |
| `Layers` | The layers, in any order |

### A layer

Every layer has a `Label` (only for reading the recipe), a `Type`, a `StartTime` and a `Duration`.
`Duration 0` means "until the recipe ends".

| Type | Puts in the scene | Its own fields | Tracks write to |
|---|---|---|---|
| `Niagara` | A Niagara system | `System`, `SkeletalMeshParameter` | User parameters |
| `Mesh` | A static mesh | `Mesh`, `Material` (all slots; empty = the mesh's own) | Material parameters |
| `Light` | A point light, no shadows | `LightColor`, `LightIntensity` (unitless), `LightRadius` | `Intensity`, `Radius`, `Color` |
| `BodyParam` | Nothing new | — | Material parameters on **every mesh of the anchor actor** |
| `CameraShake` | A shake, fired once at `StartTime` | `ShakePreset`, `ShakeScale` | — |
| `PostProcess` | A full-screen post-process material | `Material` (Material Domain = Post Process) | Material parameters |

A `PostProcess` layer is for effects drawn around the silhouette of the anchor actor (the anime aura): while it
lives, the subsystem turns **Render CustomDepth Pass** on for every mesh of the anchor actor and off again when
the last layer asking for it ends (a mesh that already had the pass on is left alone). The material is a
blendable on an unbound post-process component, so it adds to the post-process volumes of the level instead of
replacing them. `Placement`, `Socket` and `Offset` do not apply. On a soft stop it receives `FadeParameter`,
like a `Mesh` layer.

New layer types always go at the **end** of `EJRPGVFXLayerType` (it is serialized in the recipes).

### Where a layer sits

| Field | Meaning |
|---|---|
| `Anchor` | `Target`, `Instigator` or `Location` (a world point). A missing actor falls back to the context's `Location` |
| `Placement` | On the anchor actor: `Pivot`, `BoundsCenter` or `BoundsBottom` (the floor under it) |
| `Socket` | A socket or bone of the anchor actor. With it, `Placement` is ignored. A socket that does not exist falls back to `Placement` |
| `bAttach` | The layer follows the anchor. Off = it stays where it was born |
| `Offset` | Position, rotation and scale relative to the anchor point. The yaw follows the anchor actor's yaw |
| `bFollowBody` | Each frame the layer's XY follows the centre of the anchor's skeletal-mesh bones (Z stays where it was born). Niagara layers also get the float user parameter `BodyHeightScale` (body centre height now / at the start; < 1 when crouching) |
| `bScaleByAnchorSize` + `ReferenceSize` | Scales the layer by the anchor's XY bounds radius divided by `ReferenceSize`, so the same tornado grows on a big monster |
| `bRandomYaw` | A random yaw per play, from the subsystem's cosmetic stream |

The offset's position scales together with the effect: a funnel twice as big sits twice as far from the floor.

### Tracks

A track animates one parameter from `From` to `To` over the layer's life.

- **Scalar track**: `Name`, `From`, `To`, and an `Ease` (`Linear`, `EaseIn`, `EaseOut`, `EaseInOut`,
  `Pulse`) or a `Curve` asset (time 0..1 → blend 0..1) that replaces the ease.
- **Color track**: `Name`, `From`, `To`, `Ease`, and `bTintWithContextColor` to multiply by the colour of
  the play (the element, the hero's colour).

`Pulse` goes to `To` in the middle of the layer and back to `From` at the end.

**Keys.** A scalar track can instead carry `Keys`: a list of `Time` (seconds since the layer started) and
`Value`, in time order. With keys, `From`, `To` and `Curve` are ignored; `Ease` applies between each pair of
keys; the value holds before the first key and after the last one; and the track **does not repeat** in a
looping layer. This is how a parameter rises, holds and falls on one layer (`Absorb` 0 → 1 → 1 → 0), and how
an aura lights up once and stays (`Flames` ending at 1 on a looping layer). It is the Lab's `animate.keys`.

Reserved scalar names, which never reach a material or a user parameter:

| Name | Applies to | Effect |
|---|---|---|
| `Scale` | `Niagara`, `Mesh`, `Light` | Multiplies the layer's scale |
| `Intensity` | `Light` | Multiplies `LightIntensity` |
| `Radius` | `Light` | Multiplies `LightRadius` |

On a `Light` layer the color track named `Color` sets the light's colour.

### What every Niagara layer receives

Written by the subsystem if the system has a user parameter with that name and type:

| User parameter | Type | When | Value |
|---|---|---|---|
| `Color` | LinearColor | At spawn | The context's colour. **White (the default) writes nothing**: the system keeps its own `Color` |
| `EffectScale` | float | At spawn | Context scale × the anchor-size factor |
| `TargetPosition` | Position | Every frame | The centre of the target |
| `InstigatorPosition` | Position | Every frame | The centre of whoever caused it |

The two positions are refreshed every frame on purpose: a swarm pulled toward the hero keeps following the
hero. `SkeletalMeshParameter` names a Skeletal Mesh data interface user parameter that receives the anchor
actor's mesh, for particles born on the body's surface (dust on death, the Seru absorb).

The system is spawned **deactivated**, receives its parameters, and is activated after: the first
simulation tick already sees the right values.

---

## Playing an effect

```cpp
FJRPGVFXContext Context;
Context.Instigator = Attacker;
Context.Target = Victim;
Context.Color = ElementColor;

UJRPGVFXSubsystem* VFX = UJRPGVFXSubsystem::Get(this);
VFX->PlayEffect(Recipe, Context);              // by asset
VFX->PlayEffectById(TEXT("TornadoFlame"), Context);   // by DT_VFX row
```

| Call | Use |
|---|---|
| `PlayEffect(Recipe, Context)` | Returns a handle. Layers with `StartTime 0` enter on the same frame |
| `PlayEffectById(Id, Context)` | Looks the recipe up in `DT_VFX` (`FJRPGVFXRow`: RowName = id, `Recipe` = soft reference) |
| `SetEffectFloat(Handle, Name, Value, Delay, BlendTime)` | Drives a float on every Niagara, Mesh and PostProcess layer of a playing effect; beats a track of the same name. Real clock |
| `PulseEffectFloat(Handle, Name, Peak, Rise, Fall, Rest, Delay)` | Same targets: up to `Peak` in `Rise`, back to `Rest` in `Fall`, and stays there (a blink, a flame burst) |
| `StopEffect(Handle, bImmediate)` | Soft (fades over `FadeOutTime`) or at once |
| `StopAllEffects(bImmediate)` | End of a fight, a scene change |
| `IsEffectPlaying(Handle)` | |

`DT_VFX` follows the other tables: `/Game/Data/DT_VFX`, falling back to the plugin's `Data/` folder.

**`Context.TimeScale`** (default 1) is the speed of the recipe's timeline: `StartTime`, `Duration`, `Keys` and
`MinPlayTime` are all read on it (2 = everything happens in half the time). Niagara layers that end simulate at
the same speed (`SetCustomTimeDilation`); looping layers stay at 1, so an aura does not flicker faster.
`FadeOutTime` and `SetEffectFloat` stay on the real clock. Tracks without `Keys` on a looping layer repeat
every `GetLength()` of recipe time, so they also repeat faster.

**From a clip.** Add the **Battle VFX** notify (`UJRPGAnimNotify_VFX`) on the frame. It takes a `Recipe`,
or an `EffectId` when there is no recipe set. Whoever plays the clip is the instigator. With
`bOnActionTarget` the target is the target of the action in progress (`AJRPGBattlePawn::GetActionTarget`);
otherwise it is the owner itself. A notify keeps no handle, so it **refuses looping recipes**.

**From the console (PIE).**

```
JRPG.VFX.Play <DT_VFX id | /Game/path/to/Recipe> [scale]
JRPG.VFX.StopAll [soft]
```

`JRPG.VFX.Play` plays on the player's pawn, as both instigator and target.

### Looping and stopping

In a looping recipe, layers with `Duration 0` never end and their tracks repeat every `GetLength()`.
Layers with their own duration play once, at the start (an intro burst before the aura settles).

On a soft `StopEffect`:

- Niagara layers stop emitting at once; live particles finish on their own.
- `Mesh` layers receive `FadeParameter` from 1 to 0. The material has to read it, or the mesh pops out.
- `Light` layers dim to 0.
- `BodyParam` layers ease back to each track's `From`.

When a non-looping `BodyParam` layer ends, the body keeps the **final** value of its tracks. A hit flash
should therefore use `Pulse`, or end at the rest value.

### The Spirit aura

The first effects wired into the fight. When a hero's Spirit step starts, `AJRPGBattlePawn` plays the
`DT_VFX` row `Spirit_<Key>` (`Spirit_Vahn`, `Spirit_Noa`, `Spirit_Gala`) on itself, and stops it softly when
the action ends (or at once if the pawn leaves the world). A hero with no row only logs the warning.

The three rows play the VFX Lab port ([From the VFX Lab](#from-the-vfx-lab-the-spirit-charge)), one
recipe per hero with the palette in colour tracks:

| Row | Recipe | Lab palette |
|---|---|---|
| `Spirit_Vahn` | `VFX_SpiritCharge_Vahn` | "Furia vermelha" |
| `Spirit_Noa` | `VFX_SpiritCharge_Noa` | "Vento verde" |
| `Spirit_Gala` | `VFX_SpiritCharge_Gala` | "Chakra azul" |

All in `/Game/Legaia/VFX/SpiritCharge/`, looping, `FadeOutTime` 1 s. The aura lasts as long as the hero's
`Spirit` clip, which is short for Gala (1.8 s at rate 2 ≈ 0.9 s, against ≈ 1.6 s for Vahn and ≈ 1.9 s for
Noa), so `VFX_SpiritCharge_Gala` has `MinPlayTime` 1.8: the action still ends on the original's frame, only
the aura stays. The context colour is left white, so each recipe's colour tracks decide the palette.

The first version of these auras (`VFX_Spirit_<Key>`, duplicates of systems from two free packs in
`/Game/_ThidParty/`) was deleted when the Lab port replaced it. What was learned from those packs, for the
next effects:

- **Saturated HDR colours with the opposite channels at zero** (fire `(100, 15, 0)`). This keeps the
  colour alive over a bright background; a low colour mixed toward white goes pastel.
- **Translucent, not additive**, for the mesh layers: colour × a noise mask, opacity apart.
- **Two noise textures**: one panning along a spiral mesh, the other distorting it; a 4-channel Dynamic
  Parameter driven by Niagara (erosion over life, a mask sweeping along the mesh, a random offset, contrast).
- **Flipbooks** for volume: smoke 8×8, flames 2×2, lightning 3×3.
- Pack emitters set to `Once` with `Life Cycle Mode: Self` play one pass and go dark when another emitter
  keeps the system alive; they need `Infinite`.
- The engine MCP cannot copy one emitter out of another system; a pack system can only be used whole.

### From the VFX Lab: the Spirit charge

Effects are written first in the **VFX Lab** (`D:\JOGOS\VFXTest`, outside this repo): one `.ush` per
effect, previewed in the browser and checked with `fxc`, and a scene JSON (`scenes/Spirit.json`) that stacks
them on meshes and particle layers around a dummy, entrance included (Reiniciar replays it). The Lab is the
source of truth; the Unreal assets are a port. `python -m vfxlab export` writes each effect's Custom node
code; `vfxlab.scenes.export_scene` writes the cone OBJs and a `LEIAME.md` with every number below
(meshes, World Position Offset formula, each emitter's motion, the entrance tracks).

The port of the `Spirit` scene lives in `/Game/Legaia/VFX/SpiritCharge/`:

| Lab layer | Unreal | Notes |
|---|---|---|
| `Chao` (`SpiritGround`, plane) | `M_SpiritGround` on `/Engine/BasicShapes/Plane`, scale 5, 2 cm up | Flat |
| `ChaoSaia` (`SpiritAura`, low wide cone) | `MI_SpiritAura_Saia` on `SM_SpiritSaia` (55 → 230 cm radius, 110 cm) | WPO waves |
| `Aura` (`SpiritAura`, cone) | `MI_SpiritAura_Main` on `SM_SpiritAura` (60 → 162 cm, 384 cm) | WPO waves + sway |
| `AuraVolume` (`SpiritAura`, outer cone) | `MI_SpiritAura_Volume` on `SM_SpiritAuraVolume` (78 → 195 cm, 330 cm) | Weaker, other phase |
| 11 particle layers | 11 emitters of `NS_SpiritCharge`, same names as the Lab layers | All **GPU Compute** |

Particle materials: `M_SpiritSpark` (sparks, streaks), `M_SpiritStar`, `M_SpiritOrb` (the orbs),
`M_SpiritWisp` (energy tongues escaping the cone and the ground), `M_SpiritSmoke` (dust, Translucent).

**Recipes.** `VFX_SpiritCharge` (default palette) and `VFX_SpiritCharge_Vahn` / `_Noa` / `_Gala`, which add
colour tracks `ColorCore` / `ColorMain` / `ColorEdge` on every layer; the `Spirit_<Key>` rows of `DT_VFX`
play the hero copies. Looping, `FadeOutTime` 1. Each mesh is two layers: `<Name>Entrada` with its own
`Duration` and a track `Reveal` 0 → 1 `EaseOut` (the same curve as the Lab's `out`), then `<Name>`
starting when it ends, looping, `Reveal` held at 1 (a pooled mesh can come back with the value an
interrupted entrance left). The entrance layer comes first in the list, so on the swap tick it is released
and the loop layer starts on the same frame; the materials read global time, so the image does not jump.
Ground 0.8 s, skirt 0.6 s, cone 1.0 s, outer cone 0.1 s late and 1.1 s long. Try a recipe in PIE with
`JRPG.VFX.Play /Game/Legaia/VFX/SpiritCharge/VFX_SpiritCharge`.

**Materials.** Unlit; Additive except the dust. The Custom node holds the exported code inline (no include,
no rebuild), `Output Type` Float 4: RGB → Emissive, A → Opacity (UE's Additive adds `Emissive × Opacity`,
the Lab's `SRC_ALPHA, ONE`). `Reveal` and `Seed` live inside the `.ush`; mesh materials multiply the
opacity by `JRPGVFXFade` and `Opacity` (0.9 on the cones); sprite materials multiply by `Particle Color`
(the Lab's `vColor`). The effect inputs `ParticleAge` and `ParticleRandom` are per particle: Dynamic
Parameter `Param1` (Niagara writes `Particles.NormalizedAge`) and Particle Random
(`Particles.MaterialRandom`). `T_SpiritNoise`: sRGB off, `TC_Masks`, sampler `SAMPLERTYPE_Masks`.

**Cone World Position Offset.** `M_SpiritAura` has a second Custom node (code in the Lab scene's
`LEIAME.md`) fed by UV, Time and Local Position, transformed local → world. Parameters `Wobble` (lumps),
`Wave` (ripples travelling up the wall), `Sway` (the whole cone leaning, metres at the top), `WobbleSpeed`,
`Phase`; all 0 in the parent, set per layer in the three `MI_SpiritAura_*` instances. The cone meshes have
bounds extensions so the swaying top is not culled.

**Particles.** Every emitter is GPU Compute, local space, fixed bounds (±450 × ±450, −50 → 700 cm). No
forces: `SetParameters` modules with HLSL expressions reproduce the Lab's `VS_PART`. Spawn hashes
`Particles.MaterialRandom` into two random `float4`s and picks angle, radius, height, speed, distance and
size; update recomputes position and velocity per motion:

| Lab `motion` | Position |
|---|---|
| `rise` | ring radius × (1 + spread × age), height + speed × time |
| `cone` | on the `Aura` cone wall: radius `(60 + 102 × h^1.5) × k` at the current height |
| `ground` | out over the floor: `r0 + D × a(2 − a)` (an ease out, like the ground's `Reveal`), plus a rise |
| `coneOut` | born on the cone wall, escaping outwards and upwards at constant speed |

Stretched sprites are Velocity Aligned (`SpriteSize.y` = length). Alpha is `smoothstep(0, f0) ×
(1 − smoothstep(f1, 1))` per layer. Lab `burst` layers are `Once` emitters (`Life Cycle Mode: Self`,
`SpawnBurst_Instantaneous`) fired when the pooled component activates; a Lab `delay` on a continuous layer
is `SpawnRate = rate × step(delay, Emitter.Age)`. The sprite renderers bind `User.ColorCore` /
`User.ColorMain` / `User.ColorEdge` to the material parameters of the same name, so the recipe's colour
tracks recolour meshes and particles together.

Traps found while porting:

- The Niagara CPU VM has **no `smoothstep`** in HLSL expressions; the expressions write `t*t*(3-2t)` with
  `saturate` so they stay valid on either target.
- The editor viewport barely ticks Niagara while the editor window is unfocused: judge particles in
  Simulate or PIE, not in the idle viewport.
- Over the MCP, a Custom node's `Inputs` (and a recipe's `Layers`) cannot grow and change in the same
  `set_properties`; empty or grow first, then set. A Dynamic Parameter output only connects after its output
  names were queried once.

### From the VFX Lab: the hit impacts

Lab scenes `HitPunch` and `HitSlash` → `/Game/Legaia/VFX/Hit/`. `DT_VFX` rows `HitPunch` and `HitSlash`.

| Lab layer | Unreal | Notes |
|---|---|---|
| `Linhas` (`HitLine`) | emitter `Linhas`, `M_HitLine` | 10 sprites (9 in the slash), 240–650 cm, spread evenly by Sprite Rotation |
| `Faiscas`, `RiscosLongos` (`SpiritSpark`) | emitters of the same name, `M_SpiritSpark` | Velocity Aligned, stretched 7× and 14× |
| `Bolinhas`, `Brasas` (`SpiritOrb`) | emitters of the same name, `M_SpiritOrb` | `Brasas` twinkles |
| `Corte` (`HitCut`, slash only) | emitter `Corte`, `M_HitCut` | 1 sprite, 200 cm |

`NS_HitPunch` and `NS_HitSlash` are one-shot bursts, all **GPU Compute**, in **world space**: the particles
stay where the hit happened. The system's origin is the hit point; the recipes (`VFX_HitPunch`,
`VFX_HitSlash`) anchor it on `Location`, not attached, and whoever plays them gives the point.

**Where the hit lands.** The original does not compute it from the body: the impact burst is an effect record
(code `0` / `9`) of the **defender's reaction clip**, with an offset authored per monster and per reaction
(`BATTLE_RE_SWEEP.md` §8.2). The generators read it: `ImpactOffset` on the reaction rows of `DT_EnemyActions`
(tags 2, 3, 4) and `FlinchHighImpact` / `FlinchLowImpact` / `KnockdownImpact` on `DT_HeroReactions`. Three
numbers in PS1 units, actor space: sideways, up from the floor, forward. Gimard `(0, 330, 30)` is 143 cm up;
Vahn takes a high blow at `(7, 359, 128)` and a low one at `(0, 245, 117)`. The pawn uses the point of the
reaction the blow causes (the event's `LowHit` flag), × `UnitsToCm` × the mesh scale, measured from the **root of
the target's model** (the original's actor position is its floor point). The burst is spawned by the walker's
table arm, which rotates the offset by the facing **+ 0x800**: the record's positive "forward" ends up
**behind** the target, away from the attacker (the knockdown, which flies backwards, has the larger value;
its floor ring, a direct-arm record with no half turn, is at `z = -450`). `HitVFXForward` (default 0) moves
the point toward the attacker in fractions of the capsule radius. A target with no point in the tables falls
back to `HitVFXHeight` (0.65 of its capsule). The Lab's `origin` is the same idea, in front of the chest. `M_HitLine` and `M_HitCut` have **Disable Depth Test** on (the Lab's `depthTest: false`).
The sparks and orbs reuse the Spirit materials; each layer's own `Intensity` / `CoreSize` is a scalar in the
renderer's Material Parameters, and the fire palette is the system's `User.ColorCore` / `ColorMain` /
`ColorEdge` defaults (a recipe colour track recolours it). `User.Opacity` (0.7) multiplies the alpha of every
particle; a recipe scalar track `Opacity` changes it.

The Lab's `explode` motion: `Position = Origin + Dir × (r0 + D × (1 − (1 − a)^k))`. `Dir` mixes a random
direction on the sphere with one on the plane facing the camera (`planar`). The camera comes from the stock
dynamic input `ReturnCameraPosition`, written to `Emitter.CamPos` in Emitter Update; the plane is built from the
direction hit point → camera, so it does not work in split screen (the dynamic input says so). `rotate: even`
is `(UniqueID mod N + 0.5 ± 0.3) / N × 360` on `Particles.SpriteRotation`.

**In the fight.** `AJRPGBattlePawn` plays the impact on the target of every hit of its action that connects
(`Damage`). A `Block` or a `Miss` plays none, the same rule as the impact sound. Which one:

- The default is the pawn's `HitVFXId`, `HitPunch`.
- A hit whose impact sound has its own `DT_VFX` row plays that row instead. The row is named
  `Hit_<sound asset name>`: `Hit_SW_Battle_Cue04` → `VFX_HitSlash`. The sound is what tells a blade from a fist,
  so the cut is chosen per sound, and a new cutting sound only needs one more row.

The logic often resolves the hit a frame or two before the clip's sound notify arrives, so the effect waits
`HitVFXWaitFrames` (2 logic frames) for the sound before falling back to the default.

Rows so far, told by ear among the common battle sounds: the cuts are `SW_Battle_Cue03`, `04`, `05`, `0B`, `0C`
and `4B`. `Cue06` to `Cue10`, `Cue0A` and `Cue4C` are normal hits and have no row. The hero swing clips imported
so far play `Cue07` / `08` / `09`, whatever the weapon, so a hero's knife gets the normal hit.

`JRPG.VFX.Play HitSlash` plays one on the player's pawn.

### From the VFX Lab: the Absorb

Lab scene `Absorb` → `/Game/Legaia/VFX/Absorb/`. `DT_VFX` row `Absorb`. It replaced the first Art aura
(`/Game/Legaia/VFX/ArtAura/`, deleted).

| Lab layer | Unreal | Notes |
|---|---|---|
| `Chao` (`AbsorbGround`, plane) | `M_AbsorbGround` on `/Engine/BasicShapes/Plane`, scale 6, 2 cm up | Keys `Absorb`, `Pulse` |
| `ChaoVolume` (`AbsorbDome`, low cone) | `M_AbsorbDome` on `SM_AbsorbDome` (290 → 22 cm radius, 32 cm) | WPO wobble 0.03; keys `Absorb` |
| `Faixas`, `FaixasLongas`, `FaixasChao` (`AbsorbStreak`, ribbons) | 3 emitters of `NS_AbsorbPull`, `M_AbsorbStreak` | **CPU** (see below) |
| `Particulas`, `FaiscasSugadas` | 2 emitters of `NS_AbsorbPull` | GPU, motion `inward` |
| `Aura` (`AnimeAura`, post) | `M_AnimeAura`, a `PostProcess` layer | Keys `Charge`, `Flash`, `Flames` |
| `Estouro`, `BolinhasEstouro`, `FaiscasAura`, `LinguasSoltas` | `NS_AbsorbAura` | GPU, motion `rise`; starts at the flash |

**Recipe.** `VFX_Absorb`, looping, `FadeOutTime` 0.6: energy is pulled in for 3.8 s, the body flashes, and the
flame aura stays until `StopEffect`. Ground 0 → 4.8 s, dome and `NS_AbsorbPull` 0 → 3.8 s, `NS_AbsorbAura`
from 3.8 s, the aura layer for the whole effect; the Lab's `animate.keys` are the tracks' `Keys`, with the
same times. A `Light` layer on the chest stands in for the rim light the Lab puts on its dummy.

**In the fight: the Art aura.** One aura per action, and it builds up. `AJRPGBattlePawn::StartArtAuraVFX`
runs when a starter step begins; four `DT_VFX` rows per hero (`<Key>` = Vahn, Noa, Gala), all in
`/Game/Legaia/VFX/Absorb/`:

| Row | Recipe | What it plays |
|---|---|---|
| `ArtAura_<Key>` | `VFX_ArtAura_<Key>` | The aura, looping: the `M_AnimeAura` layer and `NS_ArtAuraLoop` (sparks and loose tongues, `SparkRate` 0/1). Its size, flash and flames come from the pawn (`SetEffectFloat` / `PulseEffectFloat`) |
| `ArtAbsorb_<Key>` | `VFX_ArtAbsorb_<Key>` | One `SpecialStarter`: ground, dome, `NS_AbsorbPull` and the chest light. `CueTime` 3.8 |
| `ArtFlash_<Key>` | `VFX_ArtFlash_<Key>` | The flash: `NS_ArtAuraBurst` and a light pulse |
| `ArtDissipate_<Key>` | `VFX_ArtDissipate_<Key>` | The end: `NS_ArtAuraDissipate`, particles thrown off the body |

- **Levels.** `ArtAuraSizes` / `ArtAuraHeights` on the pawn: level 0 = `ArtStarter` (0.015 / 3), level 1 = the first
  flash (0.043 / 3), level 2 = the second flash on (0.055 / 3.4). An `ArtStarter` opens the aura at level 0, or
  leaves it alone. A `SpecialStarter` plays the absorb with whatever aura is lit (with none, it opens one with only
  the rim line charging, `Charge` 0 → 1), and at the flash the aura goes one level up (`Size` / `Height` blend in
  0.15 s, `Flash` pulse, `Flames` 1.3 → 1, `SparkRate` 1).
- **The flash frame** comes from the **Art Aura Flash** notify (`UJRPGAnimNotify_AuraFlash`) on the hero's
  `AS_<Hero>_Battle_SpecialStarter` clip: Vahn frame 18, Noa 12, Gala 12. It is read like the hit notifies: clip
  seconds, plus the `Loop Window` turns when the flash is after it, over the step's play speed. Without the notify the
  flash comes at the end of the step. The absorb is stretched to it: `TimeScale = CueTime / seconds to the flash`
  (clamped to 0.25 – 8).
- **The end** (`FinishAction`): a `Flash` pulse, `Dissolve` 0 → 1 (delay 0.1 s, 0.4 s), `ArtDissipate_<Key>`, and a
  soft stop with the recipe's `FadeOutTime` 0.7.
- **Sounds** (on the pawn, soft references, empty = none): `ArtAbsorbSound` plays when a `SpecialStarter` starts the
  absorb, and `ArtFlashSound` plays at its flash. The end blink is silent. The defaults are `SW_Battle_StarterAbsorb` /
  `SW_Battle_StarterFlash` in `/Game/Legaia/Battle/Sounds/Common/`.
- **Following the body.** Every spatial layer has `bFollowBody`: the battle pawn only moves at the end of each clip,
  the body moves inside it. `NS_AbsorbPull` scales its target height by `BodyHeightScale`, so the energy goes down
  with the hero's crouch.
- **`M_AnimeAura` `Cover`** (0.6): the background is darkened under the aura before its colour is added
  (`Scene × (1 − Cover × A) + RGB × A`). Pure additive (0, the Lab's look) washes out on a bright floor.
- Colours: constant `ColorCore` / `ColorMain` / `ColorEdge` tracks with the hero's Spirit palette. The Lab scene for
  the whole cycle is `scenes/ArtAura.json`.
- Built by `.claude/unreal-mcp/vfxport/art.py`.

**Ribbons.** The Niagara Ribbon renderer does not run on GPU emitters, so the three ribbon emitters are CPU.
A ribbon is 16 particles in a row (`RibbonID = UniqueID / 16`, an `int32`: the renderer's `RibbonIdBinding`
has to be retyped from the default `NiagaraID`; `RibbonLinkOrder = UniqueID mod 16`). Each particle stores how
long until its ribbon is complete, so the 16 share one clock, and sits at `ribbonAt(age − k × trail)`, the
same formula as the Lab's `VS_RIBBON`: the ribbon is born collapsed at the start point, curves with the
spiral, and the tail is pulled in after the head arrives. The material reads `UV` swapped (V across, U along)
and the per-ribbon random from Dynamic Parameter `Param2`.

**`inward` and `until`.** `e = a^accel`; radius `lerp(r0, target, e)`, angle `+ swirl × e` (one in four turns
the other way × 0.6), height toward the target plus an arc `× sin(π e)`. The Lab's `until` (only spawn what
arrives before the flash) is a spawn window plus a lifetime clamp `min(life, until − Emitter.Age)`: the last
particles are faster instead of missing. These emitters use `Loop Duration` 1000 so `Emitter.Age` never wraps.

**The aura.** `M_AnimeAura` is a Post Process material at **Scene Color Before Bloom**: Emissive =
`PostProcessInput0` + the Custom node's RGB × A × `JRPGVFXFade`. `UV` = Screen Position (ViewportUV),
`TexelSize` = the `InvSize` of a SceneTexture **CustomDepth** node; a SceneTexture **SceneDepth** node is wired
to an unused extra input (`SceneDepthRef`) only so the compiler declares that texture for the
`SceneTextureLookup` calls in the code. `Charge`, `Flames` and `Flash` default to 0: the material draws nothing
until a recipe drives it. The aura is in screen space, so `Size` is a fraction of the screen height, not a
world size.

More traps found here:

- A Simulate session is already ~2 s old when `StartPIE` returns: an effect shorter than that has ended before
  the first capture. Delay the bursts for a test, or capture a sequence.
- `SetEmitterData` with `{"Name": ...}` renames an emitter; `CreateNiagaraSystem` with an existing system as
  template copies it whole. Copying `NS_SpiritCharge` and rewriting its emitters is faster than building from
  the stock templates.

---

## Authoring effects with Claude

Niagara is a node graph, which an LLM cannot edit directly. The pipeline below avoids the graph. It comes
from a workflow other people use for the same problem ("Automating Unreal Engine Niagara VFX with Claude,
Python, and C++", r/ClaudeCode), adapted to what this project already has.

```
asset script / MCP calls  ->  saved assets (materials, Niagara systems, recipes)  ->  UJRPGVFXSubsystem plays them
```

1. **Start from a working template, never from an empty system.** The engine's stock emitters live in
   `/Niagara/DefaultAssets/Templates/Emitters/`. Copy one into the system per layer and name each one for
   what it does (`Flash`, `Sparks`, `Smoke`, `Embers`).
2. **Describe before changing.** Dump the emitter's real modules and inputs first; names cannot be guessed.
3. **Set with typed setters, then read back.** A change that matched nothing did not apply, and it fails
   silently. Every write is followed by a read of the same value.
4. **The look goes in the material.** Procedural HLSL for edges, erosion, glow and age-based fading. Niagara
   only feeds it motion, lifetime, size and colour.
5. **Compile explicitly and wait.** Niagara compilation and shader compilation are asynchronous; check the
   compile state and the stack issues before judging the result or closing the editor.
6. **Judge it in the FX Lab**, not in the asset editor.

In the original workflow steps 1 to 3 go through a custom C++ editor library exposed to Python. Here the
engine's MCP server already offers that surface (`NiagaraToolsets.NiagaraToolset_System`: create a system
from a template, add an emitter from a template, add a renderer, read and write emitter and module inputs,
`GetStackIssues`, `GetSystemCompileState`). The plugin has no Editor module and should not grow one for
this unless the MCP turns out to be missing something specific. **How far the MCP gets is not verified yet**:
the first effect is the test.

Two things worth trying while building the first systems, both unverified:

- **Lightweight (stateless) emitters.** They are configured through a fixed set of modules with plain
  properties, no graph and no scratch pad, which is the shape a tool can edit. There is a forum report of
  systems made only of stateless emitters failing to save in 5.8.2.
- **Epic's Niagara Examples pack** (5.7) as a source of working systems to parametrise.

### The FX Lab *(planned)*

A map in the host project used only to look at effects. It is the preview: effects are judged in the real
renderer, with the game's own post-process, fog and lighting.

- A target dummy and an instigator dummy, a neutral floor, a fixed camera, a dark and a bright backdrop.
- At least one small and one large target, to check `bScaleByAnchorSize`.
- `JRPG.VFX.Play` plays any recipe there; a recipe is a Data Asset, so editing it and playing again needs no
  restart.

LegaiaStudio does not preview effects. A browser cannot run Niagara or Unreal materials, so anything drawn
there would be a different effect. If the Studio gets a VFX tab later, it edits recipe data and asks the
editor to play it in the Lab.

---

## What comes from the original

Only the trigger points. Almost every battle effect of the original is authored per clip, as up to eight
8-byte effect records in the same action entry that carries the clip's sounds
([`Reference/BATTLE_RE_SWEEP.md`](../Reference/BATTLE_RE_SWEEP.md) §8.2). The plan:

1. Export those records with the sound tracks (`frame`, `code`, `offset`).
2. The import script places a **Battle VFX** notify with an `EffectId` on each clip, beside the sound notifies.
3. `DT_VFX` maps each id to a recipe made from scratch.

No texture, model or look is taken from the disc for effects.

---

## Rules

- **The fight's logic does not know VFX exists.** Effects are fired by the presentation and by clips.
- **Randomness is cosmetic.** It comes from the subsystem's own stream, never from the fight's `Rng`.
- **`UJRPGVFXSubsystem` is a `UTickableWorldSubsystem`**, not a `UGameInstanceSubsystem`: it owns
  components that live and die with the world, like `UJRPGBattleSceneSubsystem`. This is an exception to
  the first ground rule of [`ARCHITECTURE.md`](../ARCHITECTURE.md).
- **Meshes and lights are pooled** on one transient host actor per world; Niagara uses its own pool
  (`ManualRelease`). Effect meshes cast no shadow and stay out of the distance field, so the mist does not
  flow around them.
- **A recipe edited while it plays ends that play.** The layer count no longer matches the running state.

## Checked, and still to check

- Checked: the two automation tests pass (`JRPG.VFX`); a recipe with Niagara, mesh and light layers plays
  through the subsystem in Simulate.
- Checked in Simulate (2026-11-16): the hit impacts, the Absorb ground, dome, ribbons and particles, and
  `M_AnimeAura` on a post-process volume.
- Checked in PIE (2026-11-16): `VFX_Absorb` with its `Keys` tracks and the `PostProcess` aura layer (the rim line
  grows with `Charge`, the flash, the flames stay; Custom Depth is turned on and off by the layer), and the
  `JRPG.VFX.Tracks` cases pass.
- Known limit: `Placement` reads `GetActorBounds` of the whole actor. On `BP_FieldCharacter` the bounds are huge
  (about 3200 × 2000 × 3400 cm), so `BoundsBottom` lands ~13 m from the pawn and the mesh, light and Niagara layers
  of any recipe (Spirit included) play off screen when `JRPG.VFX.Play` is used in the field. Battle pawns are not
  measured yet.
- To check, after the build that carries it: the hit impacts in a real fight (position on the target, the cut
  rows).
- To check, after the build that carries `bFollowBody`, `PulseEffectFloat` and the Art Aura Flash notify: the Art
  aura in a real fight (small aura, absorb with it lit, the flash on the notify frame, the second Hyper growing it,
  the end blink and dissolve, the absorb following the crouch).
- To check: the Spirit aura in a real fight (after the build that carries the pawn hook).
- To check: a `Battle VFX` notify plays in the animation editor's preview. The subsystem is enabled for
  preview worlds for this; it is the least certain part of the code.
- To check: a pooled Niagara component is returned and reused without keeping the previous play's
  parameters (a white context colour now writes nothing, so a reused component keeps its last `Color`).
- A soft stop on a looping recipe leaves nothing behind (no light, no hidden mesh still attached).
