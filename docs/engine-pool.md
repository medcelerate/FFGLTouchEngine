# TouchEngine engine pool — design

Status: design, not implemented. Branch `spike/te-engine-reuse`.

## Why

Every plugin instance owns one `TEInstance`, and each instance is one
TouchEngine process. Starting that process is what makes a clip slow to
come up, and it is paid again every time a clip's engine is thrown away
(Clear Instance, the fork's Release On Idle, deleting the clip).

Measured with `tools/te_pool_spike` on Windows 11, TouchDesigner 2023.12000,
2026-10-28, using the repo's example toxes:

| Step | Time | Engine process |
|---|---|---|
| New instance, configure + load tox A | 15.7 s ready, 31 ms load, 0.47 s first frame | new, ~1.14 GB |
| Unload A | `InstanceDidUnload` immediately | same |
| Configure + load tox B on the same instance | 1 ms ready, 11 ms load, 51 ms first frame | same |
| Unload B, configure + load A again | 1 ms ready, 11 ms load, 11 ms first frame | same |
| New instance, configure with no tox | 11.3 s ready | new |
| …then configure + load tox B | 1 ms ready, 11 ms load, 0.49 s first frame | same |
| Release both instances | — | both exit within ~1.7 s |

The fork measured 25–40 s for the same spawn under Resolume with
TouchDesigner 2025.33070.

So the expensive part is the process, and a running process can take any
tox that runs on the same TouchDesigner version. A pool of running engines, handed to plugin instances as needed, turns
a 15–40 s stall into the cost of loading the tox itself.

What the pool does **not** do: save memory. An unloaded engine still holds
~1.18 GB. The pool trades memory for latency, so it must be capped.

## API facts the design relies on

Verified by the spike (Windows, TouchDesigner 2023.12000 unless noted):

- `TEInstanceConfigure(instance, NULL, mode)` starts the engine with no tox.
- `TEInstanceUnload` keeps the process alive. It ends in
  `TEEventInstanceDidUnload` (2–4 ms), **not** `TEEventInstanceReady` as
  `TEInstance.h` documents. `Ready` only follows the next `Configure`.
- Configure + Load of a different tox on a running instance reuses the
  process: ~1 ms to ready, 4–11 ms to load for the example toxes.
- **A realistic tox loads in ~65–75 ms on a warm engine**
  (`Samples/Palette/PointClouds/pointGenerator.tox`, 3.2 MB). Its first
  frame took 438 ms the first time and 38 ms when the same engine loaded it
  again. Steady frames were 1.5 ms median.
- `TERelease` on the instance ends the process.
- **One TE graphics context can serve several loaded engines at once**
  (2023.12000 and 2025.30280). Two engines, each associated once with the
  same context right after spawning, rendered interleaved at 1.5–1.8 ms per
  frame, and every readback through the shared device had real content.
  Handing an engine to another clip without touching its context worked
  both with the tox still loaded and after unload + reload.
- **Re-associating an engine with another device's TE context crashes the
  host process** (`STATUS_STACK_OVERFLOW` inside TouchEngine), whether the
  tox is loaded or already unloaded. The call never returns and the engine
  process is left orphaned. See [Graphics device](#graphics-device): an
  engine keeps the context it was given at spawn for life.
- **Keeping a tox loaded between clips saves nothing.** After a
  suspend/resume handoff the first frame took 95–117 ms; unload + reload of
  the same small tox took ~8 ms in all, and the realistic tox's reload plus
  first frame ~100 ms. So released engines are unloaded (see
  [Release](#release)).
- TouchDesigner 2025.30280 engines are ~130 MB larger than 2023.12000 ones
  (~1.26 GB against ~1.13 GB idle).
- **A different TouchDesigner version is always a new process.** Changing
  the preferred engine path on a running instance returns success, but the
  next Configure replaces the process (11–16 s). Loading a tox pinned by a
  `TouchEngine` link to another version does the same. Both take effect —
  `TEInstanceGetConfiguredEnginePath` reports the new engine afterwards —
  so nothing fails, it is just a cold start.

Not yet verified:

1. **Everything above on macOS** (Metal, IOSurface output path). Needs a
   licensed Mac; the spike's graphics section is Windows-only so far.
2. **Real show toxes.** The realistic tox loaded in ~70 ms, but a large
   show tox may take seconds, which the pool cannot hide. If it does,
   keeping toxes loaded between clips is worth revisiting.
3. **In Resolume**: one binary load shared by all instances, and the whole
   pool end to end (see [Plan](#plan)).

## How Resolume hosts plugin instances

The host loads a plugin binary once per process. Every clip or effect slot
using it sends `FF_INSTANTIATE_GL`, which runs the factory (`new
FFGLTouchEngine`) and hands the object pointer back as the instance ID
(`src/lib/FFGL/ffgl/FFGL.cpp`, `instantiateGL`). All instances therefore
share the binary's statics — the SDK itself relies on this for
`g_CurrPluginInfo` and `s_pPrototype` — so a pool is an ordinary static in
the shared code, and moving an engine between instances is moving a
`TEInstance*` within one address space.

Consequences:

- **The prototype instance.** On `FF_INITIALISE` the SDK constructs one
  extra instance of the plugin class, used only to read parameter info and
  defaults. Never acquire an engine from the constructor, or the prototype
  takes one. Acquire from `InitGL` or when the tox path is set.
- **Shutdown order.** A static pool's destructor runs when the binary
  unloads, possibly after `TouchEngine.dll`/framework teardown has begun.
  Release all engines explicitly from the library deinitialise hook
  (`FF_DEINITIALISE`) instead.
- **One pool per binary.** Source, FX and FX Presets are different
  binaries, and so is the same bundle installed in two folders; each has
  its own statics. Sharing across them is phase 2 below.
- **To confirm in Resolume** before relying on it: log a process-wide
  counter of live instances from the constructor and destructor, and check
  two clips of the same plugin report the same counter address.

## Shape

```
          one per process (phase 1: per plugin bundle)
┌──────────────────────── EnginePool ────────────────────────┐
│ slots: [EngineSlot] (Spawning | Empty | Leased | Dead)       │
│ owns:  Windows: a D3D11 device for FX input textures only    │
│        macOS:   one Metal device + one TE graphics context   │
│ policy: warm count, idle cap, eviction                       │
└──────────────┬───────────────────────────────────────────────┘
               │ Acquire(tox, enginePath) → EngineLease
               ▼
   FFGLTouchEnginePluginBase holds an EngineLease instead of
   TouchObject<TEInstance>; Release() hands the engine back.
```

### EngineSlot

Owns one `TEInstance` for its whole life. `TEInstanceCreate` binds the
callback target permanently, so the slot — never a plugin — is the
`callback_info`. States:

- **Spawning** — created, `Configure(NULL)` sent, waiting for `Ready`. On
  macOS the slot then associates the pool's TE context, once. Windows
  engines get no context (see [Graphics device](#graphics-device)).
- **Empty** — engine up, no tox.
- **Leased** — attached to a plugin instance.
- **Dead** — load failed in a way that leaves the engine unusable, or the
  process went away. Released and replaced, never reused.

### Callback routing

TE callbacks arrive on TE's threads. Today they call straight into the
plugin (`eventCallbackStatic` casts `info` to the plugin), which is why the
fork needed a recursive lifecycle mutex and an `isBeingDestroyed` flag.

With a pool, a callback must never reach a plugin that has already handed
its engine back or been destroyed. The slot does not call into the plugin.
It pushes each event onto the current lease's event queue under a short
slot-level lock, and the plugin drains its queue on its own thread at the
top of `ProcessOpenGL` and in the `Set*/Get*Parameter` entry points.
`TEEventFrameDidFinish` stays a fast path that only flips an atomic on the
lease.

That gives a simple rule: plugin code only touches TE state from the host's
thread, so there is no lock-order problem between the slot and the plugin,
and detaching a lease is just clearing the slot's lease pointer. Events for
a lease that was already returned land in a queue nobody drains, which is
harmless.

Cost: work that today happens on `DidLoad` (resume, `GetAllParameters`)
runs at the next drain instead. A clip that is not rendering would show its
parameters a frame late. Draining in `GetFloatParameter`/`GetTextParameter`
too keeps the host's parameter queries current.

### Acquire

`Acquire(tox, enginePath)` resolves the engine key (see
[Engine version](#engine-version)), then:

1. Takes an `Empty` slot with that key — Configure + Load, ~1 ms plus the
   tox's own load (5 ms for the examples, ~70 ms for the realistic tox).
2. Otherwise spawns a new slot (10–16 s). The lease is returned
   immediately in a Spawning state, and the plugin renders black / passes
   input through, as it does during any load today.

After each acquire the pool tops the Empty count back up to the warm target
in the background, so the next new clip finds an engine already running.

An engine's very first frame took 350–550 ms in the spike whether or not it
was pre-warmed, against 1–8 ms for first frames on a process that had
already rendered. So a pre-warmed engine still shows a short hitch on its
first clip.

An earlier draft kept released toxes loaded (a "Cached" state) to skip the
load on re-arm. The spike showed the suspend/resume handoff costs about as
much as an unload + reload, and a kept tox would carry its previous clip's
internal state into the next one, so it was dropped.

### Release

`EngineLease::Release()` suspends the engine, cancels any frame in flight
(`TEInstanceCancelFrame`, then wait for `FrameDidFinish`), detaches the
lease, and unloads the tox (2–4 ms, ending in `InstanceDidUnload`). The
slot is Empty again. It never touches the graphics context.

Every place that currently throws the engine away goes through Release
instead:

- `ClearTouchInstance()` in both plugins (`instance.reset()` today).
- The destructor and `DeInitGL`.
- The fork's Release On Idle, if adopted. With a pool it stops being a
  memory-versus-latency trade per clip: an idle clip gives its engine back
  to the pool, and only the pool's cap decides whether that process lives.

The Unload pulse (parameter 2) keeps its current meaning: unload the tox
but keep this clip's engine.

### Eviction and limits

Three settings, read once from an INI file next to the plugin bundle
(FFGL has no process-wide parameters, and a per-clip parameter would let two
clips disagree):

| Setting | Default | Meaning |
|---|---|---|
| `WarmEngines` | 1 | Empty engines kept ready for new clips |
| `MaxIdleEngines` | 2 | Empty engines kept at most, beyond the leased ones |
| `IdleEvictSeconds` | 300 | An Empty engine above `WarmEngines` unused this long is released |

Over the cap, the least recently used Empty slot is released. Leased slots
are never evicted. Peak memory is then
`(leased + MaxIdleEngines) × ~1.2 GB` instead of growing with every clip
fired during a set.

### Engine version

An engine runs exactly one TouchDesigner version for its life: loading a
tox that needs another version replaces the process, a full cold start
(verified for both a changed preferred path and a pinned tox). So slots are
keyed by engine path, and `Acquire` must work out the key **before** it
picks a slot, or it will burn a warm engine on a respawn:

1. A `TouchEngine` link in the tox's folder (verified with a Windows
   junction) wins.
2. Otherwise the plugin's preferred path — the newest installed
   TouchDesigner, as the fork's `FindNewestTouchDesignerInstall` picks it.

Pre-warmed Empty engines use key 2. A pinned tox whose version no slot runs
pays one cold start, as it does today.

After every Configure the slot still reads
`TEInstanceGetConfiguredEnginePath`. If it differs from the key — some
resolution rule this doesn't model, such as an environment variable or a
link in a parent folder — the process has already been replaced, so the
slot is re-keyed to the engine it now runs rather than discarded, and the
mismatch is logged so the rule can be added.

### Graphics device

Each plugin today creates its own D3D11 device (`InitializeDevice`) and its
own TE graphics context, and associates its engine with that context on
`DidLoad`. A pooled engine cannot follow that pattern: moving an engine to
another device's context crashed the host process in the spike, with the
tox loaded or unloaded, while one context shared by several engines worked
on both TouchDesigner versions tested.

**Decision (2026-10-28): on Windows, engines get no graphics context, and
the plugin imports TouchEngine's D3D shared texture straight into GL with a
GL keyed mutex** ([below](#windows-chosen-keyed-mutex-handoff-with-no-graphics-context)).
That avoids the re-association crash by never associating, works on both
TouchDesigner versions tested, and removes Spout and the per-plugin D3D11
devices. The Vulkan route also works and is recorded as the alternative.

What the pool owns, from phase 1:

- **Windows**: no TE context. One D3D11 device, used only to allocate the
  shared textures that carry FX input to TouchEngine (GL can import shared
  memory but not create it). Output needs no device at all.
- **macOS**: one Metal device and command queue, and one `TEMetalContext`
  made from it. Every engine is associated with it once, right after it
  spawns and before its first Resume, and never again. The plugins already
  share the system default `MTLDevice`; what changes is that they stop
  creating a `TEMetalContext` each. (A context-free route, taking
  `IOSurface` output straight into GL as the current code already does
  after its Metal copy, may also be possible; untested.)

Cross-bundle sharing (phase 2) has to share these too, not just the
engines.

#### Windows (alternative): Vulkan handoff

TouchDesigner renders in Vulkan whatever context the host associates; the
context only decides how output is handed over. A `TEVulkanContext` hands it
over as Vulkan memory that GL imports directly, which removes the D3D11
device, the Spout interop and the copies between them. Verified with
`te_pool_spike --only vulkan` on TouchDesigner 2023.12000 and 2025.30280
(RTX 2070 Super, NVIDIA 591.86, GL 4.6), two engines sharing one context:

- **The context is made from Resolume's GL context alone.**
  `TEVulkanContextCreate` takes the GPU's device UUID, driver UUID and LUID,
  which GL reports (`GL_DEVICE_UUID_EXT`, `GL_DRIVER_UUID_EXT`,
  `GL_DEVICE_LUID_EXT`). Both engines then reported `Vulkan, D3DShared` as
  supported and delivered Vulkan textures.
- **Output imports into GL** with `GL_EXT_memory_object_win32`
  (`glImportMemoryWin32HandleEXT` + `glTexStorageMem2DEXT`, optimal tiling,
  dedicated). Output was `VK_FORMAT_B8G8R8A8_UNORM`, so the GL texture needs
  a red/blue swizzle.
- **The import needs the allocation size**, which TouchEngine does not
  report. A small Vulkan device on the same GPU (matched by UUID, loaded from
  the driver's `vulkan-1.dll`) sizes an identical image: 8,847,360 bytes at
  1920×1080.
- **Acquire**: TouchEngine's transfer semaphore is a *timeline* semaphore
  (opaque Win32 handle). GL imports it only into a semaphore created with
  `glCreateSemaphoresNV` and typed timeline first; `glGenSemaphoresEXT`
  fails with `GL_INVALID_VALUE`. Then `glWaitSemaphoreEXT` on the wait value.
- **Release**: `TEInstanceAddVulkanTextureTransfer` rejects TouchEngine's
  own semaphore ("API client usage error"). It accepts a timeline semaphore
  the host created and exported — from the same small Vulkan device, wrapped
  with `TEVulkanSemaphoreCreate` and imported into GL so GL can signal it.
  One such semaphore per process, with a rising value, served every engine.

So on Windows the pool owns, instead of a D3D11 device: one
`TEVulkanContext`, one small Vulkan device (sizing and the release
semaphore), and the host semaphore. Engines are associated with the context
once, as above. The plugins need no device of their own.

Limits found:

- **Timeline semaphores in GL need `GL_NV_timeline_semaphore`**, which only
  NVIDIA drivers provide. AMD and Intel need another sync route — see the
  keyed-mutex route below.
- **FX input** is untested. GL can import memory but not create shareable
  memory, so the input texture must come from an allocator — the same
  small Vulkan device, or a D3D shared texture, which TouchEngine accepts
  as input without a context.
- Per-frame cost is unmeasured without the spike's full-image readback.
  A plugin should cache the GL import per TouchEngine texture handle, since
  TouchEngine reuses a small set of output textures.

#### Windows (chosen): keyed-mutex handoff, with no graphics context

The simpler alternative, verified with `te_pool_spike --only glshared` on
TouchDesigner 2023.12000 and 2025.30280 (same machine): the engine gets **no graphics
context**, delivers its usual D3D shared texture (a KMT handle,
`DXGI_FORMAT_B8G8R8A8_UNORM`), and GL imports it directly:

- `glImportMemoryWin32HandleEXT(mem, 0, GL_HANDLE_TYPE_D3D11_IMAGE_KMT_EXT,
  handle)` then `glTexStorageMem2DEXT(..., GL_RGBA8, ...)`. **The size must
  be 0** for D3D11 image handles: a nonzero size imports without error but
  makes the storage call fail with `GL_INVALID_VALUE`.
- Sync is `glAcquireKeyedMutexWin32EXT(mem, wait, timeout)` /
  `glReleaseKeyedMutexWin32EXT(mem, wait + 1)` with the keys from
  `TEInstanceGetTextureTransfer`, then `TEInstanceAddTextureTransfer` — the
  same keys the plugin's D3D11 path uses today, but with a timeout instead
  of `AcquireSync(INFINITE)`.
- Two engines, three rounds each, real pixels every round on both versions;
  import plus full readback took 4–7 ms against 16–43 ms on the Vulkan
  route. On 2025 an engine's very first frame was slower (640–675 ms
  against ~400 ms on 2023).

Compared with the Vulkan route it needs no Vulkan device, no allocation
sizing, no host semaphore and no NVIDIA-only extension — only
`GL_EXT_memory_object_win32` and `GL_EXT_win32_keyed_mutex`, which still
need checking on AMD and Intel. And with no context to associate, the pool's
"one context per engine for life" rule disappears entirely. Without a
context TouchEngine picks the default GPU, which matters only on
multi-GPU machines (`TEInstanceAssociateAdapter` covers that).

**Red and blue are swapped** in the import, as expected for BGRA bytes read
as `GL_RGBA8`: `te_pool_spike --only channels` read the same output frame
through D3D as (33, 89, 240) and through GL as (240, 89, 33), every round on
both versions, with an identity component map from TouchEngine. The plugin
swizzles red and blue back on the texture it draws from.

Either route removes the D3D11 device and Spout. TouchDesigner renders in
Vulkan with both; they differ only in how the frame crosses into GL.

Still to verify on this route:

- **FX input**: the pool's D3D11 device creates a shared texture (NT
  handle, keyed mutex); GL imports it with size 0, draws Resolume's input
  into it between keyed-mutex acquire/release, and TouchEngine takes it as
  a `TED3DSharedTexture` with no context, which the header allows. The
  channels section got as far as TouchEngine accepting a KMT-shared,
  keyed-mutex orange texture on an input link with no context, but the
  example tox's output didn't show whether the input was used.
- **AMD and Intel**: both extensions need confirming there.
- **Inside Resolume** — output verified: Arena 7.20.1 on the same machine
  loaded both plugins from `feat/keyed-mutex-output`; a TouchEngine source
  (`NoiseOutOnly5Param.tox`) and a TouchEngineFX on a clip
  (`InputOutput5Param.tox`) rendered right side up, without flicker, and
  kept running together, with no plugin errors logged. Not yet checked
  there: reloading or restarting a clip, which drops the cached imports,
  and red/blue against a tox with a known colour.

### Sharing across the three plugin bundles

Source, FX and the fork's FX Presets are separate binaries with separate
statics, so phase 1 gives each bundle its own pool, device and context. To
share one pool (phase 2):

- The pool is exposed as a versioned C function table
  (`struct TEPoolApi { uint32_t version, size; ... }`), never C++ types,
  because the bundles can be built from different versions.
- The first bundle to need a pool creates it and publishes the table
  pointer process-wide. On macOS, an exported symbol found with
  `dlsym(RTLD_DEFAULT, ...)`; on Windows, a named file mapping
  (`Local\TEPool-<pid>`) holding the pointer. Within one process this is
  the one place where shared memory helps, and only as a rendezvous for a
  pointer — `TEInstance` handles cannot cross processes.
- The creating bundle must stay loaded for the life of the process.
  Resolume does not unload plugins while running, but this needs
  confirming before relying on it.

## Prerequisites from the fork

The pool sits on the engine lifecycle, which upstream gets wrong in ways
the fork has already fixed. Take these first (`drmbt/modernize-te`):

- `3cbdec1` — readiness only after a successful `DidLoad` → Resume →
  enumeration, not on `InstanceReady`. Same mistake the spike hit.
- `8df5ccb` — Unload clears the loaded/ready flags and pending-load state.
- `f3149a6` (queued loads part) — a load requested while one is in flight
  is queued, not dropped.
- `cd943af` — the lifecycle mutex. With event queues the callbacks no
  longer need it, but the host-thread entry points still do until
  everything moves to the drain model.

## Plan

1. **Spike** — done on Windows: process reuse, pinned and changed engine
   paths, a realistic tox, one context shared by several engines, the
   crash on re-association, and both GL handoff routes. Remaining: macOS
   (needs a licensed Mac), FX input over the keyed-mutex route, and a real
   show tox's load time.
2. **Port the fork's lifecycle fixes** listed above — done on branch
   `fix/te-lifecycle` (not merged), plus a fix for load bookkeeping on
   error results and the fork's unique Spout mutex names.
3. **Replace Spout with the keyed-mutex GL handoff (Windows)**, per plugin
   and independent of the pool: output in both plugins first, then FX
   input. Drops the engine's D3D11 context association, the Spout interop
   and `AcquireSync(INFINITE)`. Output is on branch
   `feat/keyed-mutex-output` (`src/plugins/shared/TEOutputGL.{h,cpp}`):
   the Source plugin runs with no D3D11 device or context, and the FX
   plugin keeps both only for its Spout input.
4. **Phase 1 pool**: `src/plugins/shared/EnginePool.{h,cpp}` owning the
   engines and the per-platform GPU objects above; the event queue and
   drain; `EngineLease` replacing `instance` in the plugin base;
   `ClearTouchInstance`/destructor/`DeInitGL` calling Release; engine-key
   resolution; INI settings. Per bundle.
5. **Verify in Resolume**: N clips fired in sequence; engine count, memory
   and arm-to-first-frame latency against the current build; clip deleted
   while loading; Clear Instance while a frame is in flight; Source and FX
   clips running together.
6. **Phase 2**: shared pool and GPU objects across bundles.
