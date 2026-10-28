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
tox. A pool of running engines, handed to plugin instances as needed, turns
a 15–40 s stall into the cost of loading the tox itself.

What the pool does **not** do: save memory. An unloaded engine still holds
~1.18 GB. The pool trades memory for latency, so it must be capped.

## API facts the design relies on

Verified by the spike:

- `TEInstanceConfigure(instance, NULL, mode)` starts the engine with no tox.
- `TEInstanceUnload` keeps the process alive. It ends in
  `TEEventInstanceDidUnload`, **not** `TEEventInstanceReady` as
  `TEInstance.h` documents. `Ready` only follows the next `Configure`.
- Configure + Load of a different tox on a running instance reuses the
  process and is near-instant for small toxes.
- `TERelease` on the instance ends the process.

Not yet verified — each needs a spike step before the code depends on it:

1. **Re-associating a graphics context.** Each plugin today creates its own
   D3D11 device / Metal device and associates a TE context made from it. A
   pooled engine moves between plugins, so it would be re-associated with a
   different context. The header allows calling
   `TEInstanceAssociateGraphicsContext` again "if you require support for a
   different context", but nothing has exercised it.
2. **A tox that pins a different TouchDesigner version** (a TouchEngine
   link next to the tox). Unknown whether Configure on a running engine
   silently restarts it, fails, or ignores the pin.
3. **Load time of a realistic show tox.** The example toxes load in ~10 ms.
   A heavy tox still pays its own load and cook, which the pool cannot hide.
4. **Changing the preferred engine path** on a running instance.

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
  its own statics. Sharing across them is phase 3 below.
- **To confirm in Resolume** before relying on it: log a process-wide
  counter of live instances from the constructor and destructor, and check
  two clips of the same plugin report the same counter address.

## Shape

```
          one per process (phase 1: per plugin bundle)
┌──────────────────────── EnginePool ────────────────────────┐
│ slots: [EngineSlot] (Spawning | Empty | Cached(tox) | Leased)│
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

- **Spawning** — created, `Configure(NULL)` sent, waiting for `Ready`.
- **Empty** — engine up, no tox.
- **Cached(tox)** — tox loaded and suspended, nobody using it. Keeping the
  tox loaded makes re-arming the same clip skip even the tox load.
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

`Acquire(tox, enginePath)` picks, in order:

1. A `Cached(tox)` slot with the same engine path — no load at all; the new
   owner pushes **all** its parameter values, not only dirty ones, since the
   tox still holds the previous owner's.
2. An `Empty` slot — Configure + Load, ~10 ms plus the tox's own cost.
3. The least recently used `Cached(other)` slot — Unload, then as in 2.
4. Otherwise spawn a new slot. The lease is returned immediately in a
   Spawning state, and the plugin renders black / passes input through, as
   it does during any load today.

After each acquire the pool tops the Empty count back up to the warm target
in the background, so the next new clip finds an engine already running.

An engine's very first frame took ~0.5 s in the spike whether or not it was
pre-warmed, against 10–50 ms for later frames on the same process. That is
per process, not per tox, so a pre-warmed engine still shows a short
hitch on its first clip.

### Release

`EngineLease::Release()` suspends the engine, cancels any frame in flight
(`TEInstanceCancelFrame`, then wait for `FrameDidFinish`), detaches the
lease, and marks the slot `Cached(tox)`. It does not unload: unloading
happens lazily in step 3 of Acquire, or on eviction.

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
| `MaxIdleEngines` | 2 | Empty + Cached engines kept beyond the leased ones |
| `IdleEvictSeconds` | 300 | A Cached engine unused this long is released |

Over the cap, the least recently used Cached slot is released first, then
Empty slots. Leased slots are never evicted. Peak memory is then
`(leased + MaxIdleEngines) × ~1.2 GB` instead of growing with every clip
fired during a set.

### Engine version

Slots are keyed by the preferred engine path set before their first
Configure. After every Configure the slot reads
`TEInstanceGetConfiguredEnginePath`; if the tox resolved to a different
engine than the slot was started with, the slot is marked Dead and the
lease retries on a slot for that path. Unverified item 2 decides whether
this check is enough or whether pinned toxes must bypass the pool.

### Graphics device

Phase 1 keeps each plugin's own device and re-associates the leased
engine with that plugin's TE context on every acquire (unverified item 1).

Phase 2 moves the D3D11 / Metal device and the TE graphics context into the
pool, so every plugin and every engine share one. This removes the
re-association question and one device per clip. On Windows the Spout
interop and the plugin's textures are created from `D3DDevice`, so they
would take the pool's device instead — no other change, since all clips
already run on the same GPU.

### Sharing across the three plugin bundles

Source, FX and the fork's FX Presets are separate binaries with separate
statics, so phase 1 gives each bundle its own pool. To share one pool:

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

1. **Spike the unverified items** in `tools/te_pool_spike`: context
   re-association with a real D3D11/Metal context, a pinned-version tox, a
   heavy show tox, preferred-path change. Run on Windows (licensed) and
   macOS once licensed.
2. **Port the fork's lifecycle fixes** listed above.
3. **Phase 1 pool**: `src/plugins/shared/EnginePool.{h,cpp}`, the event
   queue and drain, `EngineLease` replacing `instance` in the plugin base,
   `ClearTouchInstance`/destructor/`DeInitGL` calling Release, INI settings.
   Per bundle.
4. **Verify in Resolume**: N clips fired in sequence; engine count, memory
   and arm-to-first-frame latency against the current build; clip deleted
   while loading; Clear Instance while a frame is in flight.
5. **Phase 2**: shared device and graphics context.
6. **Phase 3**: shared pool across bundles.
