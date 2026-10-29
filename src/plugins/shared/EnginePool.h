#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "FFGL/FFGLSDK.h"
#include "TouchEngine/TouchObject.h"

// A pool of running TouchEngine instances, so a new clip can take an engine
// that is already up (about 1 ms plus the tox's own load) instead of
// starting one (10-16 s and about 1.2 GB each). Design: docs/engine-pool.md
// on the spike/te-engine-reuse branch.
//
// One pool per plugin binary: Resolume loads each binary once per process,
// and every clip using it is an object in that binary, so they all share
// these statics. The Source and FX binaries each have their own pool.
//
// The pool, not a plugin, is every engine's callback target, because
// TEInstanceCreate binds that for the engine's life. Events are queued for
// whichever lease holds the engine and handed to the plugin on the host's
// thread, so TouchEngine's threads never call into a plugin or take its
// locks, and an engine handed back can never call into the clip that had it.
//
// Everything except the TouchEngine callbacks runs on the host's thread.

// One TouchEngine callback, queued for the plugin.
struct EngineEvent {
	bool isLinkEvent = false;
	TEEvent event = TEEventGeneral;
	TEResult result = TEResultSuccess;
	TELinkEvent linkEvent = TELinkEventAdded;
	std::string identifier;
};

class EngineSlot;
struct LeaseChannel;

// A plugin instance's claim on one pooled engine. Handing it back unloads the
// tox and returns the engine to the pool; it never destroys the engine.
class EngineLease {
public:
	EngineLease() = default;
	~EngineLease();
	EngineLease(EngineLease&& other) noexcept = default;
	EngineLease& operator=(EngineLease&& other) noexcept;
	EngineLease(const EngineLease&) = delete;
	EngineLease& operator=(const EngineLease&) = delete;

	explicit operator bool() const { return slot != nullptr; }

	TEInstance* Instance() const;

	// False while the engine is still starting; it cannot take a Configure
	// until then.
	bool IsReady() const;

	// True once the engine failed to start or was released; a new lease is
	// needed.
	bool IsDead() const;

	// Which TouchDesigner the engine runs: "" for TouchEngine's default.
	const std::string& Key() const;

	// Moves the events reported since the last call into 'events'.
	void TakeEvents(std::vector<EngineEvent>& events);

	void Release();

private:
	friend class EnginePool;
	std::shared_ptr<EngineSlot> slot;
	std::shared_ptr<LeaseChannel> channel;
};

class EnginePool {
public:
	// Created on first use and never destroyed: its destructor would run when
	// the binary unloads, possibly after TouchEngine itself has shut down.
	// Shutdown() releases the engines instead.
	static EnginePool& Shared();

	// The engine key for a tox: a TouchEngine link in its folder pins the
	// TouchDesigner it runs on, and loading it on any other engine would
	// replace that engine's process anyway.
	static std::string KeyForTox(const std::string& toxPath);

	// An engine with 'key': an idle one if there is one, else one still
	// starting, else a new one. Empty after Shutdown().
	EngineLease Acquire(const std::string& key);

	// Releases idle engines over the limits and starts warm ones. Cheap;
	// called from every plugin's frame.
	void Tick();

	// Releases every engine. Called once, from the library deinitialise hook.
	void Shutdown();

private:
	friend class EngineLease;
	EnginePool();
	void Release(EngineLease& lease);
	std::shared_ptr<EngineSlot> Spawn(const std::string& key, bool warm);
	void Retire(const std::shared_ptr<EngineSlot>& slot, const char* why);
	void TopUpWarmEngines();

	std::mutex mutex;
	std::vector<std::shared_ptr<EngineSlot>> slots;
	// Retired slots are kept, engine released, so a late TouchEngine
	// callback still finds a live object.
	std::vector<std::shared_ptr<EngineSlot>> retired;
	bool shutDown = false;
	bool used = false;
	double lastTick = 0;

	struct Settings {
		int warmEngines = 1;
		int maxIdleEngines = 2;
		double idleEvictSeconds = 300;
	} settings;
};

// FFGL library deinitialise hook (CFFGLPluginInfo's last argument).
void __stdcall EnginePoolDeinitialise();
