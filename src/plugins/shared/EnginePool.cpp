#include "EnginePool.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#endif
#ifdef __APPLE__
#include <dlfcn.h>
#import <Metal/Metal.h>
#include <TouchEngine/TEMetal.h>
#endif

namespace {

double Now()
{
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void Log(const std::string& message)
{
	FFGLLog::LogToHost(("FFGLTouchEngine: pool: " + message).c_str());
}

// How long an engine may take to report its tox unloaded before the pool
// stops trusting it and releases it.
constexpr double UnloadTimeoutSeconds = 5;

// How often Tick() looks at the slots; it runs from every plugin's frame.
constexpr double TickSeconds = 1;

// The folder this plugin binary sits in (the folder holding the .bundle on
// macOS), where FFGLTouchEngine.ini is read from.
std::filesystem::path PluginFolder()
{
#ifdef _WIN32
	HMODULE module = nullptr;
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&PluginFolder), &module)) {
		wchar_t path[MAX_PATH];
		DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
		if (length > 0 && length < MAX_PATH) {
			return std::filesystem::path(path).parent_path();
		}
	}
#endif
#ifdef __APPLE__
	Dl_info info;
	if (dladdr(reinterpret_cast<const void*>(&PluginFolder), &info) && info.dli_fname) {
		// <folder>/<name>.bundle/Contents/MacOS/<name>
		return std::filesystem::path(info.dli_fname).parent_path().parent_path().parent_path().parent_path();
	}
#endif
	return {};
}

std::string Trim(const std::string& s)
{
	size_t start = s.find_first_not_of(" \t\r\n");
	size_t end = s.find_last_not_of(" \t\r\n");
	return start == std::string::npos ? "" : s.substr(start, end - start + 1);
}

#ifdef __APPLE__
// One Metal device and TouchEngine context for every engine in the pool,
// associated once when each engine starts. Associating a different context
// with an engine later crashes the host (seen with D3D11 on Windows), so a
// plugin must never associate its own with a pooled engine.
id<MTLDevice> PoolMetalDevice = nil;
TouchObject<TEMetalContext> PoolMetalContext;
#endif

}

enum class SlotState {
	Spawning,  // started, waiting for its first InstanceReady
	Empty,     // up, no tox, no lease
	Leased,    // a plugin holds it
	Unloading, // handed back, waiting for InstanceDidUnload
	Dead,      // unusable; released by the next Tick
};

// The queue between a slot and the plugin holding it. Shared, so neither
// side's lifetime matters to the other.
struct LeaseChannel {
	std::mutex mutex;
	std::vector<EngineEvent> events;
};

class EngineSlot {
public:
	TEInstance* instance = nullptr; // owned; null once released
	std::string key;
	bool warm = false;

	std::mutex mutex; // guards everything below
	SlotState state = SlotState::Spawning;
	std::shared_ptr<LeaseChannel> channel;
	double since = 0; // when the slot entered its state

	void SetState(SlotState s)
	{
		state = s;
		since = Now();
	}

	static void EventCallback(TEInstance* instance, TEEvent event, TEResult result,
		int64_t, int32_t, int64_t, int32_t, void* info)
	{
		static_cast<EngineSlot*>(info)->OnEvent(event, result);
	}

	static void LinkCallback(TEInstance* instance, TELinkEvent event, const char* identifier, void* info)
	{
		static_cast<EngineSlot*>(info)->OnLinkEvent(event, identifier);
	}

private:
	// On a TouchEngine thread.
	void OnEvent(TEEvent event, TEResult result)
	{
		std::lock_guard<std::mutex> lock(mutex);
		switch (state) {
		case SlotState::Spawning:
			// The engine's own start (Configure with no tox), not the plugin's.
			if (event == TEEventInstanceReady) {
				if (TEResultGetSeverity(result) == TESeverityError) {
					SetState(SlotState::Dead);
					// A clip waiting on this engine hears why, as it would
					// have from its own Configure.
					if (channel) {
						EngineEvent e;
						e.event = event;
						e.result = result;
						std::lock_guard<std::mutex> channelLock(channel->mutex);
						channel->events.push_back(std::move(e));
					}
				}
				else {
					SetState(channel ? SlotState::Leased : SlotState::Empty);
				}
			}
			return;
		case SlotState::Unloading:
			if (event == TEEventInstanceDidUnload) {
				SetState(SlotState::Empty);
			}
			return;
		case SlotState::Leased:
			if (channel) {
				EngineEvent e;
				e.event = event;
				e.result = result;
				std::lock_guard<std::mutex> channelLock(channel->mutex);
				channel->events.push_back(std::move(e));
			}
			return;
		default:
			return;
		}
	}

	void OnLinkEvent(TELinkEvent event, const char* identifier)
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (state != SlotState::Leased || !channel) {
			return;
		}
		EngineEvent e;
		e.isLinkEvent = true;
		e.linkEvent = event;
		e.identifier = identifier ? identifier : "";
		std::lock_guard<std::mutex> channelLock(channel->mutex);
		channel->events.push_back(std::move(e));
	}
};

// EngineLease

EngineLease::~EngineLease()
{
	Release();
}

EngineLease& EngineLease::operator=(EngineLease&& other) noexcept
{
	if (this != &other) {
		Release();
		slot = std::move(other.slot);
		channel = std::move(other.channel);
	}
	return *this;
}

TEInstance* EngineLease::Instance() const
{
	return slot ? slot->instance : nullptr;
}

bool EngineLease::IsDead() const
{
	if (!slot) {
		return false;
	}
	std::lock_guard<std::mutex> lock(slot->mutex);
	return slot->state == SlotState::Dead || slot->instance == nullptr;
}

bool EngineLease::IsReady() const
{
	if (!slot) {
		return false;
	}
	std::lock_guard<std::mutex> lock(slot->mutex);
	return slot->state == SlotState::Leased && slot->instance != nullptr;
}

const std::string& EngineLease::Key() const
{
	static const std::string none;
	return slot ? slot->key : none;
}

void EngineLease::TakeEvents(std::vector<EngineEvent>& events)
{
	events.clear();
	if (channel) {
		std::lock_guard<std::mutex> lock(channel->mutex);
		events.swap(channel->events);
	}
}

void EngineLease::Release()
{
	if (slot) {
		EnginePool::Shared().Release(*this);
	}
	slot.reset();
	channel.reset();
}

// EnginePool

EnginePool& EnginePool::Shared()
{
	static EnginePool* pool = new EnginePool();
	return *pool;
}

EnginePool::EnginePool()
{
	std::filesystem::path ini = PluginFolder() / "FFGLTouchEngine.ini";
	std::ifstream file(ini);
	std::string line;
	while (std::getline(file, line)) {
		line = Trim(line);
		size_t equals = line.find('=');
		if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[' || equals == std::string::npos) {
			continue;
		}
		std::string name = Trim(line.substr(0, equals));
		std::string value = Trim(line.substr(equals + 1));
		try {
			if (name == "WarmEngines") settings.warmEngines = std::max(0, std::stoi(value));
			else if (name == "MaxIdleEngines") settings.maxIdleEngines = std::max(0, std::stoi(value));
			else if (name == "IdleEvictSeconds") settings.idleEvictSeconds = std::max(0.0, std::stod(value));
		}
		catch (...) {
			Log("ignoring unreadable setting '" + line + "' in " + ini.string());
		}
	}
	Log("WarmEngines " + std::to_string(settings.warmEngines) +
		", MaxIdleEngines " + std::to_string(settings.maxIdleEngines) +
		", IdleEvictSeconds " + std::to_string((int)settings.idleEvictSeconds) +
		(file.is_open() ? " (from " + ini.string() + ")" : " (defaults; no " + ini.string() + ")"));
}

std::string EnginePool::KeyForTox(const std::string& toxPath)
{
	if (toxPath.empty()) {
		return "";
	}
	std::error_code error;
	std::filesystem::path link = std::filesystem::u8path(toxPath).parent_path() / "TouchEngine";
	if (!std::filesystem::exists(link, error)) {
		return "";
	}
	std::filesystem::path target = std::filesystem::weakly_canonical(link, error);
	return (error ? link : target).u8string();
}

EngineLease EnginePool::Acquire(const std::string& key)
{
	std::lock_guard<std::mutex> lock(mutex);
	EngineLease lease;
	if (shutDown) {
		Log("starting again after the library was deinitialised");
		shutDown = false;
	}
	// A clip asking for an engine means the host is still using the pool.
	shutdownRequested = false;
	used = true;

	std::shared_ptr<EngineSlot> chosen;
	// Prefer an engine that is up, then one already starting.
	for (SlotState wanted : { SlotState::Empty, SlotState::Spawning }) {
		for (auto& slot : slots) {
			std::lock_guard<std::mutex> slotLock(slot->mutex);
			if (slot->key == key && slot->state == wanted && !slot->channel) {
				chosen = slot;
				break;
			}
		}
		if (chosen) break;
	}

	if (!chosen) {
		chosen = Spawn(key, false);
		if (!chosen) {
			return lease;
		}
	}

	lease.slot = chosen;
	lease.channel = std::make_shared<LeaseChannel>();
	{
		std::lock_guard<std::mutex> slotLock(chosen->mutex);
		chosen->channel = lease.channel;
		if (chosen->state == SlotState::Empty) {
			chosen->SetState(SlotState::Leased);
		}
	}
	Log(std::string(chosen->warm ? "handed out a warm engine" : "handed out an engine") +
		(key.empty() ? "" : " for " + key) + " (" + std::to_string(slots.size()) + " running)");

	TopUpWarmEngines();
	return lease;
}

std::shared_ptr<EngineSlot> EnginePool::Spawn(const std::string& key, bool warm)
{
	auto slot = std::make_shared<EngineSlot>();
	slot->key = key;
	slot->warm = warm;
	TEResult result = TEInstanceCreate(EngineSlot::EventCallback, EngineSlot::LinkCallback, slot.get(), &slot->instance);
	if (result != TEResultSuccess || slot->instance == nullptr) {
		Log(std::string("could not create an engine: ") + TEResultGetDescription(result));
		return nullptr;
	}
	TEInstanceSetFrameRate(slot->instance, 60, 1);

#ifdef __APPLE__
	if (PoolMetalContext == nullptr) {
		PoolMetalDevice = MTLCreateSystemDefaultDevice();
		if (PoolMetalDevice == nil || TEMetalContextCreate(PoolMetalDevice, PoolMetalContext.take()) != TEResultSuccess) {
			Log("could not create the Metal context");
		}
	}
	if (PoolMetalContext != nullptr) {
		TEInstanceAssociateGraphicsContext(slot->instance, PoolMetalContext);
	}
#endif

	if (warm) {
		// Start the engine with no tox, so a clip can configure it later in
		// about a millisecond. It turns Empty on InstanceReady.
		slot->SetState(SlotState::Spawning);
		result = TEInstanceConfigure(slot->instance, nullptr, TETimeExternal);
		if (result != TEResultSuccess) {
			Log(std::string("could not start a warm engine: ") + TEResultGetDescription(result));
			slot->SetState(SlotState::Dead);
		}
	}
	else {
		// Leased straight away: the plugin's Configure with its tox starts it.
		slot->SetState(SlotState::Leased);
	}
	slots.push_back(slot);
	return slot;
}

void EnginePool::Release(EngineLease& lease)
{
	std::shared_ptr<EngineSlot> slot = lease.slot;
	if (!slot) {
		return;
	}
	bool unload = false;
	{
		std::lock_guard<std::mutex> slotLock(slot->mutex);
		// Detach first: nothing the engine reports from here reaches the plugin.
		if (slot->channel == lease.channel) {
			slot->channel.reset();
		}
		if (slot->state == SlotState::Leased && slot->instance != nullptr) {
			slot->SetState(SlotState::Unloading);
			unload = true;
		}
		// A slot still starting simply turns Empty on InstanceReady.
	}

	if (unload) {
		// Outside the slot lock: TouchEngine may report synchronously.
		TEInstanceSuspend(slot->instance);
		TEInstanceCancelFrame(slot->instance);
		TEResult result = TEInstanceUnload(slot->instance);
		if (result != TEResultSuccess) {
			// Nothing was loaded, so no InstanceDidUnload is coming.
			std::lock_guard<std::mutex> slotLock(slot->mutex);
			if (slot->state == SlotState::Unloading) {
				slot->SetState(SlotState::Empty);
			}
		}
	}

	std::lock_guard<std::mutex> lock(mutex);
	if (shutdownRequested && LeasedCount() == 0) {
		Log("the last clip handed its engine back after the library was deinitialised");
		ReleaseAll();
	}
}

void EnginePool::Retire(const std::shared_ptr<EngineSlot>& slot, const char* why)
{
	TEInstance* instance = nullptr;
	{
		std::lock_guard<std::mutex> slotLock(slot->mutex);
		slot->SetState(SlotState::Dead);
		slot->channel.reset();
		instance = slot->instance;
		slot->instance = nullptr;
	}
	if (instance != nullptr) {
		TEInstanceSuspend(instance);
		TERelease(&instance);
	}
	retired.push_back(slot);
	slots.erase(std::remove(slots.begin(), slots.end(), slot), slots.end());
	Log(std::string("released an engine: ") + why + " (" + std::to_string(slots.size()) + " running)");
}

void EnginePool::TopUpWarmEngines()
{
	if (shutDown || !used) {
		return;
	}
	int warm = 0;
	for (auto& slot : slots) {
		std::lock_guard<std::mutex> slotLock(slot->mutex);
		if (slot->key.empty() && !slot->channel &&
			(slot->state == SlotState::Empty || slot->state == SlotState::Spawning)) {
			warm++;
		}
	}
	for (; warm < settings.warmEngines; warm++) {
		if (!Spawn("", true)) {
			break;
		}
		Log("starting a warm engine (" + std::to_string(slots.size()) + " running)");
	}
}

void EnginePool::Tick()
{
	std::lock_guard<std::mutex> lock(mutex);
	double now = Now();
	if (shutDown || now - lastTick < TickSeconds) {
		return;
	}
	lastTick = now;

	struct Idle {
		std::shared_ptr<EngineSlot> slot;
		double since;
	};
	std::vector<std::shared_ptr<EngineSlot>> dead, stuck;
	std::vector<Idle> idle;
	for (auto& slot : slots) {
		std::lock_guard<std::mutex> slotLock(slot->mutex);
		// Never one a clip still holds: it would be left with a freed instance.
		// The clip swaps it on its next load (EngineLease::IsDead).
		if (slot->state == SlotState::Dead && !slot->channel) {
			dead.push_back(slot);
		}
		else if (slot->state == SlotState::Unloading && now - slot->since > UnloadTimeoutSeconds) {
			// Typically an engine handed back while it was still starting.
			stuck.push_back(slot);
		}
		else if (slot->state == SlotState::Empty) {
			idle.push_back({ slot, slot->since });
		}
	}
	for (auto& slot : dead) {
		Retire(slot, "it failed to start");
	}
	for (auto& slot : stuck) {
		Retire(slot, "it was handed back but never reported its tox unloaded");
	}

	// Least recently used first.
	std::sort(idle.begin(), idle.end(), [](const Idle& a, const Idle& b) { return a.since < b.since; });
	size_t keep = idle.size();
	while (keep > (size_t)settings.maxIdleEngines) {
		Retire(idle[idle.size() - keep].slot, "over MaxIdleEngines");
		keep--;
	}
	for (size_t i = idle.size() - keep; i < idle.size() && keep > (size_t)settings.warmEngines; i++) {
		if (now - idle[i].since > settings.idleEvictSeconds) {
			Retire(idle[i].slot, "idle longer than IdleEvictSeconds");
			keep--;
		}
	}

	TopUpWarmEngines();
}

int EnginePool::LeasedCount()
{
	int leased = 0;
	for (auto& slot : slots) {
		std::lock_guard<std::mutex> slotLock(slot->mutex);
		if (slot->channel) leased++;
	}
	return leased;
}

void EnginePool::ReleaseAll()
{
	shutDown = true;
	shutdownRequested = false;
	while (!slots.empty()) {
		Retire(slots.front(), "shutting down");
	}
#ifdef __APPLE__
	PoolMetalContext.reset();
	PoolMetalDevice = nil;
#endif
}

void EnginePool::Shutdown()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (shutDown) {
		return;
	}
	int leased = LeasedCount();
	if (leased > 0) {
		// Releasing these would leave live clips holding freed instances.
		shutdownRequested = true;
		Log("library deinitialised with " + std::to_string(leased) +
			" engine(s) still held by clips; releasing once they are handed back");
		return;
	}
	ReleaseAll();
}

void __stdcall EnginePoolDeinitialise()
{
	EnginePool::Shared().Shutdown();
}
