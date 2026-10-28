// te_pool_spike — checks the TouchEngine behaviours an engine pool depends on
// (docs/engine-pool.md) and times each step.
//
// Usage:
//   te_pool_spike <toxA> <toxB> [--engine DIR] [--alt-engine DIR] [--heavy TOX]
//                 [--only basic,graphics,path,pin,heavy,reassoc-unloaded,reassoc-loaded]
//                 [--log FILE]
//
//   basic     Unload keeps the process; a different tox loads on it.
//   graphics  Two engines on one shared D3D11 device and TE context, each
//             delivering readable output, and handoffs with no re-association
//             (Windows only). toxA must output a texture with visible content,
//             e.g. Example/NoiseOutOnly5Param.tox.
//   path      Changing the preferred engine path on a running instance
//             (needs --alt-engine).
//   pin       A tox pinned to another TouchDesigner by a TouchEngine link next to
//             it, loaded on a running engine (needs --alt-engine).
//   heavy     Load and frame times of a realistic tox on a warm engine (needs --heavy).
//   reassoc-unloaded, reassoc-loaded
//             Move an engine to another device's TE context, after Unload or
//             while loaded. Not run by default: the loaded variant crashed the
//             process on TouchDesigner 2023.12000.
//
// --engine and --alt-engine are TouchDesigner installs: the install folder on
// Windows, the .app on macOS. Sections whose inputs are missing are skipped.
//
// Build (macOS):
//   clang++ -std=c++17 -O1 te_pool_spike.cpp -F<dir containing TouchEngine.framework> \
//     -framework TouchEngine -framework CoreFoundation -Wl,-rpath,<same dir> -o te_pool_spike
// Build and run (Windows): build.bat in this folder, then run.bat.

#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#pragma comment(lib, "d3d11.lib")
#define popen _popen
#define pclose _pclose
#else
#include <CoreFoundation/CoreFoundation.h>
#endif

#include <TouchEngine/TouchEngine.h>
#ifdef _WIN32
#include <TouchEngine/TED3D11.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

static Clock::time_point gStart = Clock::now();
static double Now() { return std::chrono::duration<double>(Clock::now() - gStart).count(); }

static FILE* gLog = nullptr;

// Prints to the console and, with --log, to the log file, unbuffered so an
// interrupted run still leaves a complete record.
static void Out(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	std::vprintf(fmt, args);
	va_end(args);
	if (gLog) {
		va_start(args, fmt);
		std::vfprintf(gLog, fmt, args);
		va_end(args);
		std::fflush(gLog);
	}
}

struct Engine {
	std::string name;
	TEInstance* instance = nullptr;
	std::mutex m;
	std::map<TEEvent, int> counts;
	std::map<TEEvent, TEResult> lastResult;
	int64_t frameTime = 0;
};

static const char* EventName(TEEvent e) {
	switch (e) {
	case TEEventGeneral: return "General";
	case TEEventInstanceReady: return "InstanceReady";
	case TEEventInstanceDidLoad: return "InstanceDidLoad";
	case TEEventInstanceDidUnload: return "InstanceDidUnload";
	case TEEventFrameDidFinish: return "FrameDidFinish";
	default: return "?";
	}
}

static void EventCallback(TEInstance*, TEEvent event, TEResult result, int64_t, int32_t, int64_t, int32_t, void* info) {
	Engine* e = static_cast<Engine*>(info);
	{
		std::lock_guard<std::mutex> lock(e->m);
		e->counts[event]++;
		e->lastResult[event] = result;
	}
	if (event != TEEventFrameDidFinish || result != TEResultSuccess) {
		Out("  [%7.2fs] %s event %s -> %s\n", Now(), e->name.c_str(), EventName(event), TEResultGetDescription(result));
	}
}

static void LinkCallback(TEInstance*, TELinkEvent, const char*, void*) {}

// Lets the main thread service its run loop / message queue, as a host app would,
// in case TouchEngine delivers anything through it.
static void PumpMainThread() {
#ifdef _WIN32
	MSG msg;
	while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	Sleep(1);
#else
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, true);
#endif
}

static int Count(Engine& e, TEEvent event) {
	std::lock_guard<std::mutex> lock(e.m);
	return e.counts[event];
}

// Waits for the count of `event` to rise above `before`. Returns seconds waited, or -1 on timeout.
static double WaitFor(Engine& e, TEEvent event, int before, double timeoutSec, TEResult* resultOut = nullptr) {
	double t0 = Now();
	double nextBeat = t0 + 5;
	for (;;) {
		{
			std::lock_guard<std::mutex> lock(e.m);
			if (e.counts[event] > before) {
				if (resultOut) *resultOut = e.lastResult[event];
				return Now() - t0;
			}
		}
		if (Now() - t0 > timeoutSec) return -1;
		if (Now() > nextBeat) {
			Out("  [%7.2fs] ... %s still waiting for %s (%.0fs)\n", Now(), e.name.c_str(), EventName(event), Now() - t0);
			nextBeat += 5;
		}
		PumpMainThread();
	}
}

struct Proc { long pid; long rssMB; };

// TouchEngine runs each engine as a separate OS process; list them so we can see spawns and reuse.
static std::vector<Proc> EngineProcesses() {
	std::vector<Proc> out;
#ifdef _WIN32
	FILE* p = popen("tasklist /FI \"IMAGENAME eq TouchEngine.exe\" /FO CSV /NH", "r");
	if (!p) return out;
	char line[512];
	while (std::fgets(line, sizeof line, p)) {
		// "TouchEngine.exe","1234","Console","1","1,234,567 K"
		std::string s(line);
		if (s.find("TouchEngine.exe") == std::string::npos) continue;
		std::vector<std::string> f;
		std::string cur; bool q = false;
		for (char c : s) {
			if (c == '"') { q = !q; continue; }
			if (c == ',' && !q) { f.push_back(cur); cur.clear(); continue; }
			cur += c;
		}
		f.push_back(cur);
		if (f.size() < 5) continue;
		std::string mem; for (char c : f[4]) if (c >= '0' && c <= '9') mem += c;
		out.push_back({ std::stol(f[1]), mem.empty() ? 0 : std::stol(mem) / 1024 });
	}
#else
	FILE* p = popen("ps -axo pid=,rss=,comm=", "r");
	if (!p) return out;
	char line[2048];
	while (std::fgets(line, sizeof line, p)) {
		long pid = 0, rss = 0; char comm[1800] = {};
		if (std::sscanf(line, "%ld %ld %1799[^\n]", &pid, &rss, comm) != 3) continue;
		std::string c(comm);
		std::string base = c.substr(c.find_last_of('/') + 1);
		if (base == "TouchEngine") out.push_back({ pid, rss / 1024 });
	}
#endif
	pclose(p);
	return out;
}

static std::set<long> gPrevPids;

// Prints the engine processes, marking spawns "(new)" and listing exits. Returns the number spawned.
static int Snapshot(const char* label) {
	auto procs = EngineProcesses();
	std::set<long> pids;
	long total = 0;
	int spawned = 0;
	std::ostringstream os;
	for (auto& pr : procs) {
		pids.insert(pr.pid);
		total += pr.rssMB;
		bool isNew = !gPrevPids.count(pr.pid);
		spawned += isNew;
		os << " " << pr.pid << (isNew ? "(new)" : "") << ":" << pr.rssMB << "MB";
	}
	std::ostringstream gone;
	for (long pid : gPrevPids) if (!pids.count(pid)) gone << " " << pid;
	Out("  [%7.2fs] engines after %-40s n=%zu total=%ldMB |%s%s%s\n", Now(), label, procs.size(), total,
		os.str().c_str(), gone.str().empty() ? "" : "  gone:", gone.str().c_str());
	gPrevPids = pids;
	return spawned;
}

struct Row { std::string step; std::string value; };
static std::vector<Row> gRows;

static std::string Secs(double s) {
	if (s < 0) return "FAILED";
	char buf[32];
	std::snprintf(buf, sizeof buf, s < 1 ? "%.1f ms" : "%.2f s", s < 1 ? s * 1000 : s);
	return buf;
}

static void Note(const std::string& step, const std::string& value) {
	gRows.push_back({ step, value });
	Out("  => %-52s %s\n", step.c_str(), value.c_str());
}

// Records a timed step and the engine processes after it.
static void Record(const std::string& step, double seconds) {
	Note(step, Secs(seconds));
	Snapshot(step.c_str());
}

static void PrintErrors(Engine& e) {
	TEErrorArray* errors = nullptr;
	if (TEInstanceGetErrors(e.instance, &errors) == TEResultSuccess && errors) {
		for (int32_t i = 0; i < errors->count; i++) {
			Out("      error: %s\n", errors->errors[i].description ? errors->errors[i].description : "(none)");
		}
		TERelease(&errors);
	}
}

static std::string ConfiguredEnginePath(Engine& e) {
	TEString* s = nullptr;
	std::string path = "(unknown)";
	if (TEInstanceGetConfiguredEnginePath(e.instance, &s) == TEResultSuccess && s) {
		path = s->string && *s->string ? s->string : "(none)";
		TERelease(&s);
	}
	return path;
}

static bool Create(Engine& e, const std::string& name, const char* enginePath) {
	e.name = name;
	TEResult r = TEInstanceCreate(EventCallback, LinkCallback, &e, &e.instance);
	if (r != TEResultSuccess) {
		Out("  %s: TEInstanceCreate failed: %s\n", e.name.c_str(), TEResultGetDescription(r));
		return false;
	}
	if (enginePath) {
		r = TEInstanceSetPreferredEnginePath(e.instance, enginePath);
		if (r != TEResultSuccess) Out("  %s: SetPreferredEnginePath failed: %s\n", e.name.c_str(), TEResultGetDescription(r));
	}
	TEInstanceSetFrameRate(e.instance, 60, 1);
	return true;
}

// Suspends and releases the instance, then waits for its process to exit.
static void Release(Engine& e) {
	if (!e.instance) return;
	TEInstanceSuspend(e.instance);
	TERelease(&e.instance);
	size_t before = gPrevPids.size();
	for (int i = 0; i < 20 && EngineProcesses().size() >= before && before > 0; i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
	}
	Snapshot((e.name + " released").c_str());
}

// Configure + wait for Ready. path may be null (engine only, no tox).
static double ConfigureAndWait(Engine& e, const char* path, TEResult* readyOut = nullptr) {
	int before = Count(e, TEEventInstanceReady);
	double t0 = Now();
	TEResult r = TEInstanceConfigure(e.instance, path, TETimeExternal);
	if (r != TEResultSuccess) {
		Out("  %s: Configure failed: %s\n", e.name.c_str(), TEResultGetDescription(r));
		return -1;
	}
	TEResult ready = TEResultSuccess;
	if (WaitFor(e, TEEventInstanceReady, before, 180, &ready) < 0) {
		Out("  %s: timed out waiting for Ready\n", e.name.c_str());
		return -1;
	}
	if (readyOut) *readyOut = ready;
	if (ready != TEResultSuccess) PrintErrors(e);
	return Now() - t0;
}

static double LoadAndWait(Engine& e, TEResult* loadedOut = nullptr) {
	int before = Count(e, TEEventInstanceDidLoad);
	double t0 = Now();
	TEResult r = TEInstanceLoad(e.instance);
	if (r != TEResultSuccess) {
		Out("  %s: Load failed: %s\n", e.name.c_str(), TEResultGetDescription(r));
		return -1;
	}
	TEResult loaded = TEResultSuccess;
	if (WaitFor(e, TEEventInstanceDidLoad, before, 180, &loaded) < 0) {
		Out("  %s: timed out waiting for DidLoad\n", e.name.c_str());
		return -1;
	}
	if (loadedOut) *loadedOut = loaded;
	if (loaded != TEResultSuccess) PrintErrors(e);
	return Now() - t0;
}

// TEInstance.h says Unload ends in InstanceReady, but the engine sends
// InstanceDidUnload; Ready only follows the next Configure.
static double UnloadAndWait(Engine& e) {
	TEInstanceSuspend(e.instance);
	int before = Count(e, TEEventInstanceDidUnload);
	double t0 = Now();
	TEInstanceUnload(e.instance);
	if (WaitFor(e, TEEventInstanceDidUnload, before, 60) < 0) {
		Out("  %s: timed out waiting for DidUnload\n", e.name.c_str());
		return -1;
	}
	return Now() - t0;
}

// Renders n frames on a resumed instance. Returns each frame's time, -1 for a failed frame.
static std::vector<double> RenderFrames(Engine& e, int n) {
	std::vector<double> times;
	for (int i = 0; i < n; i++) {
		int before = Count(e, TEEventFrameDidFinish);
		double t0 = Now();
		e.frameTime += 1000;
		if (TEInstanceStartFrameAtTime(e.instance, e.frameTime, 60000, false) != TEResultSuccess ||
			WaitFor(e, TEEventFrameDidFinish, before, 60) < 0) {
			times.push_back(-1);
			continue;
		}
		times.push_back(Now() - t0);
	}
	return times;
}

static std::string FrameStats(const std::vector<double>& t) {
	if (t.empty()) return "no frames";
	int failed = (int)std::count(t.begin(), t.end(), -1.0);
	std::vector<double> rest(t.begin() + 1, t.end());
	rest.erase(std::remove(rest.begin(), rest.end(), -1.0), rest.end());
	std::sort(rest.begin(), rest.end());
	std::ostringstream os;
	os << "first " << Secs(t[0]);
	if (!rest.empty()) os << ", then median " << Secs(rest[rest.size() / 2]) << " max " << Secs(rest.back());
	os << " (" << t.size() << " frames";
	if (failed) os << ", " << failed << " failed";
	os << ")";
	return os.str();
}

// Configure + load + render, recording each step. Returns false if the tox did not load.
static bool LoadTox(Engine& e, const char* path, const std::string& label, int frames = 5) {
	TEResult loaded = TEResultSuccess;
	double c = ConfigureAndWait(e, path);
	Record(label + ": configure->ready", c);
	if (c < 0) return false;
	double l = LoadAndWait(e, &loaded);
	Record(label + ": load->didload", l);
	if (l < 0 || loaded != TEResultSuccess) return false;
	TEInstanceResume(e.instance);
	Note(label + ": frames", FrameStats(RenderFrames(e, frames)));
	return true;
}

// ---- basic -----------------------------------------------------------------

static void SectionBasic(const char* toxA, const char* toxB, const char* engine) {
	Out("\n== basic 1. Cold: new instance, load A\n");
	Engine e1;
	if (!Create(e1, "E1", engine)) return;
	LoadTox(e1, toxA, "E1 cold A");
	Note("E1 configured engine", ConfiguredEnginePath(e1));

	Out("\n== basic 2. Unload A (engine should stay up)\n");
	Record("E1 unload A", UnloadAndWait(e1));

	Out("\n== basic 3. Swap: same instance, load B\n");
	LoadTox(e1, toxB, "E1 swap B");

	Out("\n== basic 4. Swap back: same instance, load A\n");
	Record("E1 unload B", UnloadAndWait(e1));
	LoadTox(e1, toxA, "E1 swap A");

	Out("\n== basic 5. Pre-warm: new instance, configure with no tox, then load B\n");
	Engine e2;
	if (!Create(e2, "E2", engine)) return;
	Record("E2 prewarm (configure NULL)", ConfigureAndWait(e2, nullptr));
	LoadTox(e2, toxB, "E2 warm B");

	Out("\n== basic 6. Release both instances\n");
	Release(e1);
	Release(e2);
}

// ---- graphics --------------------------------------------------------------

#ifdef _WIN32
using Microsoft::WRL::ComPtr;

// One D3D11 device and TE context, as each plugin instance creates today.
struct Gpu {
	std::string name;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	TED3D11Context* te = nullptr;
};

static bool MakeGpu(Gpu& g, const char* name) {
	g.name = name;
	HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
		g.device.GetAddressOf(), nullptr, g.context.GetAddressOf());
	if (FAILED(hr)) {
		Out("  %s: D3D11CreateDevice failed 0x%08lx\n", name, (unsigned long)hr);
		return false;
	}
	TEResult r = TED3D11ContextCreate(g.device.Get(), &g.te);
	if (r != TEResultSuccess) {
		Out("  %s: TED3D11ContextCreate failed: %s\n", name, TEResultGetDescription(r));
		return false;
	}
	return true;
}

// The first output texture link, found the way the plugin finds it.
static std::string FindOutputTexture(Engine& e) {
	TEStringArray* groups = nullptr;
	std::string found;
	if (TEInstanceGetLinkGroups(e.instance, TEScopeOutput, &groups) != TEResultSuccess || !groups) return found;
	for (int32_t i = 0; i < groups->count && found.empty(); i++) {
		TEStringArray* links = nullptr;
		if (TEInstanceLinkGetChildren(e.instance, groups->strings[i], &links) != TEResultSuccess || !links) continue;
		for (int32_t j = 0; j < links->count && found.empty(); j++) {
			TELinkInfo* info = nullptr;
			if (TEInstanceLinkGetInfo(e.instance, links->strings[j], &info) != TEResultSuccess || !info) continue;
			if (info->domain == TELinkDomainOperator && info->type == TELinkTypeTexture) found = info->identifier;
			TERelease(&info);
		}
		TERelease(&links);
	}
	TERelease(&groups);
	return found;
}

static int BytesPerPixel(DXGI_FORMAT f) {
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		return 4;
	case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_TYPELESS:
		return 8;
	case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_TYPELESS:
		return 16;
	default:
		return 0;
	}
}

// Pulls the current output texture through `gpu` exactly as the plugin's
// Windows path does (shared texture -> context texture -> keyed mutex ->
// copy), reads it back, and reports whether it has visible content.
static std::string CheckOutput(Engine& e, Gpu& gpu, const std::string& link) {
	TETexture* tex = nullptr;
	TEResult r = TEInstanceLinkGetTextureValue(e.instance, link.c_str(), TELinkValueCurrent, &tex);
	if (r != TEResultSuccess || !tex) return std::string("no texture: ") + TEResultGetDescription(r);
	std::ostringstream os;
	TETextureType type = TETextureGetType(tex);
	if (type != TETextureTypeD3DShared) {
		os << "texture type " << (int)type << ", not D3DShared";
		TERelease(&tex);
		return os.str();
	}
	TED3D11Texture* t11 = nullptr;
	r = TED3D11ContextGetTexture(gpu.te, (TED3DSharedTexture*)tex, &t11);
	if (r != TEResultSuccess || !t11) {
		TERelease(&tex);
		return std::string("ContextGetTexture failed: ") + TEResultGetDescription(r);
	}
	ID3D11Texture2D* raw = TED3D11TextureGetTexture(t11);
	ComPtr<ID3D11Device> owner;
	raw->GetDevice(owner.GetAddressOf());
	D3D11_TEXTURE2D_DESC desc = {};
	raw->GetDesc(&desc);
	os << desc.Width << "x" << desc.Height << " fmt " << (int)desc.Format
		<< (owner.Get() == gpu.device.Get() ? ", on " : ", NOT on ") << gpu.name;

	ComPtr<IDXGIKeyedMutex> mutex;
	raw->QueryInterface(IID_PPV_ARGS(mutex.GetAddressOf()));
	TESemaphore* sem = nullptr;
	uint64_t wait = 0;
	r = TEInstanceGetTextureTransfer(e.instance, tex, &sem, &wait);
	if (r != TEResultSuccess || !mutex) {
		os << ", transfer unavailable: " << TEResultGetDescription(r);
	}
	else {
		HRESULT hr = mutex->AcquireSync(wait, 2000);
		if (hr != S_OK) {
			os << ", keyed mutex acquire " << (hr == WAIT_TIMEOUT ? "timed out" : "failed");
		}
		else {
			D3D11_TEXTURE2D_DESC sd = desc;
			sd.Usage = D3D11_USAGE_STAGING;
			sd.BindFlags = 0;
			sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			sd.MiscFlags = 0;
			sd.MipLevels = 1;
			sd.ArraySize = 1;
			ComPtr<ID3D11Texture2D> staging;
			bool copied = SUCCEEDED(gpu.device->CreateTexture2D(&sd, nullptr, staging.GetAddressOf()));
			if (copied) gpu.context->CopyResource(staging.Get(), raw);
			mutex->ReleaseSync(wait + 1);
			TEInstanceAddTextureTransfer(e.instance, tex, sem, wait + 1);

			int bpp = BytesPerPixel(desc.Format);
			D3D11_MAPPED_SUBRESOURCE map = {};
			if (copied && bpp && SUCCEEDED(gpu.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
				std::set<std::string> distinct;
				int nonzero = 0, samples = 0;
				for (int y = 0; y < 16; y++) {
					for (int x = 0; x < 16; x++) {
						const uint8_t* px = static_cast<const uint8_t*>(map.pData) +
							(size_t)(y * desc.Height / 16) * map.RowPitch + (size_t)(x * desc.Width / 16) * bpp;
						std::string v(reinterpret_cast<const char*>(px), bpp);
						distinct.insert(v);
						bool any = false;
						for (int b = 0; b < bpp; b++) any |= px[b] != 0;
						nonzero += any;
						samples++;
					}
				}
				gpu.context->Unmap(staging.Get(), 0);
				os << ", " << nonzero << "/" << samples << " samples nonzero, " << distinct.size() << " distinct";
			}
			else {
				os << ", readback skipped";
			}
		}
	}
	if (sem) TERelease(&sem);
	TERelease(&t11);
	TERelease(&tex);
	return os.str();
}

static void Associate(Engine& e, Gpu& g, const std::string& label) {
	TEResult r = TEInstanceAssociateGraphicsContext(e.instance, (TEGraphicsContext*)g.te);
	Note(label + ": associate " + g.name, r == TEResultSuccess ? "ok" : TEResultGetDescription(r));
}

static void RenderAndCheck(Engine& e, Gpu& g, const std::string& label) {
	TEInstanceResume(e.instance);
	Note(label + ": frames", FrameStats(RenderFrames(e, 5)));
	std::string link = FindOutputTexture(e);
	Note(label + ": output", link.empty() ? "no output texture link" : CheckOutput(e, g, link));
}

// The pool's setup: one device and TE context shared by every engine, each
// associated once after it spawns and never again.
static void SectionGraphics(const char* toxA, const char* engine) {
	Out("\n== graphics: two engines on one shared device and TE context\n");
	Gpu shared;
	if (!MakeGpu(shared, "shared device")) return;
	Engine g1, g2;
	if (!Create(g1, "G1", engine) || !Create(g2, "G2", engine)) return;

	// Associate right after the engine is up, before any tox, as a pool slot would.
	Record("G1 spawn (configure NULL)", ConfigureAndWait(g1, nullptr));
	Associate(g1, shared, "G1 after spawn");
	Record("G2 spawn (configure NULL)", ConfigureAndWait(g2, nullptr));
	Associate(g2, shared, "G2 after spawn");

	Record("G1 configure A", ConfigureAndWait(g1, toxA));
	Record("G1 load A", LoadAndWait(g1));
	Record("G2 configure A", ConfigureAndWait(g2, toxA));
	Record("G2 load A", LoadAndWait(g2));
	RenderAndCheck(g1, shared, "G1 shared");
	RenderAndCheck(g2, shared, "G2 shared");
	// Interleaved, as two clips on screen at once would render.
	RenderAndCheck(g1, shared, "G1 shared again");
	RenderAndCheck(g2, shared, "G2 shared again");

	Out("\n== graphics: hand G1 to another plugin with its tox still loaded (Cached reuse)\n");
	TEInstanceSuspend(g1.instance);
	RenderAndCheck(g1, shared, "G1 cached handoff");

	Out("\n== graphics: hand G1 over after unload and reload (Empty reuse)\n");
	Record("G1 unload", UnloadAndWait(g1));
	Record("G1 configure A again", ConfigureAndWait(g1, toxA));
	Record("G1 load A again", LoadAndWait(g1));
	RenderAndCheck(g1, shared, "G1 reloaded handoff");

	Release(g1);
	Release(g2);
	TERelease(&shared.te);
}

// Moving an engine to another device's TE context. With the tox loaded this
// crashed the process (STATUS_STACK_OVERFLOW) on 2023.12000, so both variants
// run only when named in --only.
static void SectionReassoc(const char* toxA, const char* engine, bool unloadFirst) {
	Out("\n== %s: move an engine from device A's context to device B's\n", unloadFirst ? "reassoc-unloaded" : "reassoc-loaded");
	Gpu a, b;
	if (!MakeGpu(a, "device A") || !MakeGpu(b, "device B")) return;
	Engine g;
	if (!Create(g, "G", engine)) return;

	Record("G configure A", ConfigureAndWait(g, toxA));
	Record("G load A", LoadAndWait(g));
	Associate(g, a, "G on A");
	RenderAndCheck(g, a, "G on A");

	TEInstanceSuspend(g.instance);
	if (unloadFirst) {
		Record("G unload", UnloadAndWait(g));
		Associate(g, b, "G unloaded");
		Record("G configure A again", ConfigureAndWait(g, toxA));
		Record("G load A again", LoadAndWait(g));
	}
	else {
		Out("  associating while loaded; this crashed on TouchDesigner 2023.12000\n");
		Associate(g, b, "G loaded");
	}
	RenderAndCheck(g, b, "G on B");

	Release(g);
	TERelease(&a.te);
	TERelease(&b.te);
}
#endif

// ---- path ------------------------------------------------------------------

static void SectionPath(const char* toxA, const char* engine, const char* alt) {
	Out("\n== path: change the preferred engine on a running instance\n");
	Engine p;
	if (!Create(p, "P", engine)) return;
	LoadTox(p, toxA, "P first engine");
	Note("P configured engine (before)", ConfiguredEnginePath(p));
	Record("P unload", UnloadAndWait(p));

	TEResult r = TEInstanceSetPreferredEnginePath(p.instance, alt);
	Note("P set preferred engine to alt", r == TEResultSuccess ? "ok" : TEResultGetDescription(r));
	LoadTox(p, toxA, "P after change");
	Note("P configured engine (after)", ConfiguredEnginePath(p));
	Release(p);
}

// ---- pin -------------------------------------------------------------------

// Copies toxA into a temp folder next to a TouchEngine link pointing at alt.
static std::string MakePinnedTox(const char* toxA, const char* alt) {
	std::error_code ec;
	fs::path dir = fs::temp_directory_path(ec) / "te_pool_spike_pin";
	fs::path link = dir / "TouchEngine";
	fs::remove(link, ec);
	fs::create_directories(dir, ec);
	fs::path tox = dir / "pinned.tox";
	fs::copy_file(toxA, tox, fs::copy_options::overwrite_existing, ec);
	if (ec) {
		Out("  copy to %s failed: %s\n", tox.string().c_str(), ec.message().c_str());
		return "";
	}
#ifdef _WIN32
	// A junction needs no admin rights, unlike a directory symlink.
	std::string cmd = "mklink /J \"" + link.string() + "\" \"" + alt + "\" >nul";
	if (std::system(cmd.c_str()) != 0) {
		Out("  mklink /J failed\n");
		return "";
	}
#else
	fs::create_directory_symlink(alt, link, ec);
	if (ec) {
		Out("  symlink failed: %s\n", ec.message().c_str());
		return "";
	}
#endif
	Note("pinned tox", tox.string() + " (TouchEngine -> " + alt + ")");
	return tox.string();
}

static void SectionPin(const char* toxA, const char* engine, const char* alt) {
	Out("\n== pin: tox pinned to another TouchDesigner by a TouchEngine link\n");
	std::string pinned = MakePinnedTox(toxA, alt);
	if (pinned.empty()) return;

	Out("\n== pin control: fresh instance loads the pinned tox (does the pin work at all?)\n");
	Engine q;
	if (!Create(q, "Q", engine)) return;
	LoadTox(q, pinned.c_str(), "Q fresh pinned");
	Note("Q configured engine", ConfiguredEnginePath(q));
	Release(q);

	Out("\n== pin reuse: running engine on the default TouchDesigner loads the pinned tox\n");
	Engine r;
	if (!Create(r, "R", engine)) return;
	LoadTox(r, toxA, "R unpinned A");
	Note("R configured engine (before)", ConfiguredEnginePath(r));
	Record("R unload", UnloadAndWait(r));
	LoadTox(r, pinned.c_str(), "R pinned on running engine");
	Note("R configured engine (after)", ConfiguredEnginePath(r));
	Release(r);
}

// ---- heavy -----------------------------------------------------------------

static void SectionHeavy(const char* toxA, const char* heavy, const char* engine) {
	Out("\n== heavy: realistic tox on a pre-warmed engine\n");
	Engine h;
	if (!Create(h, "H", engine)) return;
	Record("H prewarm (configure NULL)", ConfigureAndWait(h, nullptr));
	LoadTox(h, heavy, "H heavy first load", 30);
	Record("H unload heavy", UnloadAndWait(h));
	LoadTox(h, toxA, "H small tox between");
	Record("H unload small", UnloadAndWait(h));
	LoadTox(h, heavy, "H heavy second load", 30);
	Release(h);
}

// ---- main ------------------------------------------------------------------

int main(int argc, char** argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	std::vector<std::string> positional;
	std::string engine, alt, heavy, only, logPath;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto next = [&](std::string& dst) { if (i + 1 < argc) dst = argv[++i]; };
		if (a == "--engine") next(engine);
		else if (a == "--alt-engine") next(alt);
		else if (a == "--heavy") next(heavy);
		else if (a == "--only") next(only);
		else if (a == "--log") next(logPath);
		else positional.push_back(a);
	}
	if (positional.size() < 2) {
		std::printf("usage: %s <toxA> <toxB> [--engine DIR] [--alt-engine DIR] [--heavy TOX] "
			"[--only basic,graphics,path,pin,heavy,reassoc-unloaded,reassoc-loaded] [--log FILE]\n", argv[0]);
		return 2;
	}
	if (!logPath.empty()) gLog = std::fopen(logPath.c_str(), "w");

	const char* toxA = positional[0].c_str();
	const char* toxB = positional[1].c_str();
	const char* enginePtr = engine.empty() ? nullptr : engine.c_str();
	auto named = [&](const char* s) { return ("," + only + ",").find(std::string(",") + s + ",") != std::string::npos; };
	auto wanted = [&](const char* s) { return only.empty() || named(s); };

	Out("toxA %s\ntoxB %s\nengine %s\nalt-engine %s\nheavy %s\n", toxA, toxB,
		engine.empty() ? "(TouchEngine's choice)" : engine.c_str(), alt.empty() ? "(none)" : alt.c_str(),
		heavy.empty() ? "(none)" : heavy.c_str());
	Snapshot("start");

	if (wanted("basic")) SectionBasic(toxA, toxB, enginePtr);
#ifdef _WIN32
	if (wanted("graphics")) SectionGraphics(toxA, enginePtr);
	if (named("reassoc-unloaded")) SectionReassoc(toxA, enginePtr, true);
	if (named("reassoc-loaded")) SectionReassoc(toxA, enginePtr, false);
#else
	if (wanted("graphics")) Note("graphics", "skipped: Windows only for now");
#endif
	if (wanted("path")) {
		if (alt.empty()) Note("path", "skipped: needs --alt-engine");
		else SectionPath(toxA, enginePtr, alt.c_str());
	}
	if (wanted("pin")) {
		if (alt.empty()) Note("pin", "skipped: needs --alt-engine");
		else SectionPin(toxA, enginePtr, alt.c_str());
	}
	if (wanted("heavy")) {
		if (heavy.empty()) Note("heavy", "skipped: needs --heavy");
		else SectionHeavy(toxA, heavy.c_str(), enginePtr);
	}

	Out("\n== Summary\n");
	for (auto& r : gRows) Out("  %-52s %s\n", r.step.c_str(), r.value.c_str());
	if (gLog) std::fclose(gLog);
	return 0;
}
