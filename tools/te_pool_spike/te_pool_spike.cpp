// te_pool_spike — does a running TouchEngine engine survive Unload + loading a
// different tox, and how long does each step take?
//
// Usage: te_pool_spike <toxA> <toxB> [enginePath]
//
// Build (macOS):
//   clang++ -std=c++17 -O1 te_pool_spike.cpp -F<dir containing TouchEngine.framework> \
//     -framework TouchEngine -framework CoreFoundation -Wl,-rpath,<same dir> -o te_pool_spike
// Build and run (Windows): build.bat in this folder, then run.bat.

#include <TouchEngine/TouchEngine.h>
#ifdef _WIN32
#include <windows.h>
#define popen _popen
#define pclose _pclose
#else
#include <CoreFoundation/CoreFoundation.h>
#endif

#include <chrono>
#include <thread>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;

static Clock::time_point gStart = Clock::now();
static double Now() { return std::chrono::duration<double>(Clock::now() - gStart).count(); }

struct Engine {
	std::string name;
	TEInstance* instance = nullptr;
	std::mutex m;
	std::condition_variable cv;
	std::map<TEEvent, int> counts;
	std::map<TEEvent, TEResult> lastResult;
	int links = 0;
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
	if (event != TEEventFrameDidFinish) {
		std::printf("  [%7.2fs] %s event %s -> %s\n", Now(), e->name.c_str(), EventName(event), TEResultGetDescription(result));
	}
	e->cv.notify_all();
}

static void LinkCallback(TEInstance*, TELinkEvent event, const char*, void* info) {
	Engine* e = static_cast<Engine*>(info);
	std::lock_guard<std::mutex> lock(e->m);
	if (event == TELinkEventAdded) e->links++;
	if (event == TELinkEventRemoved) e->links--;
}

// Lets the main thread service its run loop / message queue, as a host app would,
// in case TouchEngine delivers anything through it.
static void PumpMainThread() {
#ifdef _WIN32
	MSG msg;
	while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	Sleep(10);
#else
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
#endif
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
			std::printf("  [%7.2fs] ... %s still waiting for %s (%.0fs)\n", Now(), e.name.c_str(), EventName(event), Now() - t0);
			nextBeat += 5;
		}
		PumpMainThread();
	}
}

static int Count(Engine& e, TEEvent event) {
	std::lock_guard<std::mutex> lock(e.m);
	return e.counts[event];
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

static void Snapshot(const char* label) {
	auto procs = EngineProcesses();
	std::set<long> pids;
	long total = 0;
	std::ostringstream os;
	for (auto& pr : procs) {
		pids.insert(pr.pid);
		total += pr.rssMB;
		os << " " << pr.pid << (gPrevPids.count(pr.pid) ? "" : "(new)") << ":" << pr.rssMB << "MB";
	}
	std::ostringstream gone;
	for (long pid : gPrevPids) if (!pids.count(pid)) gone << " " << pid;
	std::printf("  [%7.2fs] engines after %-34s n=%zu total=%ldMB |%s%s%s\n", Now(), label, procs.size(), total,
		os.str().c_str(), gone.str().empty() ? "" : "  gone:", gone.str().c_str());
	gPrevPids = pids;
}

static void PrintErrors(Engine& e) {
	TEErrorArray* errors = nullptr;
	if (TEInstanceGetErrors(e.instance, &errors) == TEResultSuccess && errors) {
		for (int32_t i = 0; i < errors->count; i++) {
			std::printf("      error: %s\n", errors->errors[i].description ? errors->errors[i].description : "(none)");
		}
		TERelease(&errors);
	}
}

static bool Create(Engine& e, const char* enginePath) {
	TEResult r = TEInstanceCreate(EventCallback, LinkCallback, &e, &e.instance);
	if (r != TEResultSuccess) {
		std::printf("  %s: TEInstanceCreate failed: %s\n", e.name.c_str(), TEResultGetDescription(r));
		return false;
	}
	if (enginePath) TEInstanceSetPreferredEnginePath(e.instance, enginePath);
	TEInstanceSetFrameRate(e.instance, 60, 1);
	return true;
}

// Configure + wait for Ready. path may be null (engine only, no tox).
static double ConfigureAndWait(Engine& e, const char* path) {
	int before = Count(e, TEEventInstanceReady);
	double t0 = Now();
	TEResult r = TEInstanceConfigure(e.instance, path, TETimeExternal);
	if (r != TEResultSuccess) {
		std::printf("  %s: Configure failed: %s\n", e.name.c_str(), TEResultGetDescription(r));
		return -1;
	}
	TEResult ready = TEResultSuccess;
	if (WaitFor(e, TEEventInstanceReady, before, 180, &ready) < 0) {
		std::printf("  %s: timed out waiting for Ready\n", e.name.c_str());
		return -1;
	}
	if (ready != TEResultSuccess) PrintErrors(e);
	return Now() - t0;
}

static double LoadAndWait(Engine& e) {
	int before = Count(e, TEEventInstanceDidLoad);
	double t0 = Now();
	TEResult r = TEInstanceLoad(e.instance);
	if (r != TEResultSuccess) {
		std::printf("  %s: Load failed: %s\n", e.name.c_str(), TEResultGetDescription(r));
		return -1;
	}
	TEResult loaded = TEResultSuccess;
	if (WaitFor(e, TEEventInstanceDidLoad, before, 180, &loaded) < 0) {
		std::printf("  %s: timed out waiting for DidLoad\n", e.name.c_str());
		return -1;
	}
	if (loaded != TEResultSuccess) PrintErrors(e);
	return Now() - t0;
}

// Resume and render one frame, so "loaded" also means "producing frames".
static double FirstFrame(Engine& e) {
	TEInstanceResume(e.instance);
	int before = Count(e, TEEventFrameDidFinish);
	double t0 = Now();
	static int64_t t = 0;
	t += 1000;
	if (TEInstanceStartFrameAtTime(e.instance, t, 60000, false) != TEResultSuccess) return -1;
	if (WaitFor(e, TEEventFrameDidFinish, before, 60) < 0) return -1;
	return Now() - t0;
}

static double UnloadAndWait(Engine& e) {
	TEInstanceSuspend(e.instance);
	int before = Count(e, TEEventInstanceReady);
	double t0 = Now();
	TEInstanceUnload(e.instance);
	if (WaitFor(e, TEEventInstanceReady, before, 60) < 0) {
		std::printf("  %s: timed out waiting for Ready after Unload\n", e.name.c_str());
		return -1;
	}
	return Now() - t0;
}

struct Row { std::string step; double seconds; };
static std::vector<Row> gRows;

static void Record(const std::string& step, double seconds) {
	gRows.push_back({ step, seconds });
	std::printf("  => %-46s %s\n", step.c_str(), seconds < 0 ? "FAILED" : (std::to_string(seconds) + " s").c_str());
	Snapshot(step.c_str());
}

static void LoadTox(Engine& e, const char* path, const std::string& label) {
	double c = ConfigureAndWait(e, path);
	double l = c < 0 ? -1 : LoadAndWait(e);
	Record(label + ": configure->ready", c);
	Record(label + ": load->didload", l);
	if (l >= 0) Record(label + ": first frame", FirstFrame(e));
}

int main(int argc, char** argv) {
	if (argc < 3) {
		std::printf("usage: %s <toxA> <toxB> [enginePath]\n", argv[0]);
		return 2;
	}
	// Unbuffered, so the log is complete even if the run is interrupted.
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	const char* toxA = argv[1];
	const char* toxB = argv[2];
	const char* enginePath = argc > 3 ? argv[3] : nullptr;

	Snapshot("start");

	std::printf("\n== 1. Cold: new instance, load A\n");
	Engine e1; e1.name = "E1";
	if (!Create(e1, enginePath)) return 1;
	LoadTox(e1, toxA, "E1 cold A");

	std::printf("\n== 2. Unload A (engine should stay up)\n");
	Record("E1 unload A", UnloadAndWait(e1));

	std::printf("\n== 3. Swap: same instance, load B\n");
	LoadTox(e1, toxB, "E1 swap B");

	std::printf("\n== 4. Swap back: same instance, load A\n");
	Record("E1 unload B", UnloadAndWait(e1));
	LoadTox(e1, toxA, "E1 swap A");

	std::printf("\n== 5. Pre-warm: new instance, configure with no tox, then load B\n");
	Engine e2; e2.name = "E2";
	if (!Create(e2, enginePath)) return 1;
	Record("E2 prewarm (configure NULL)", ConfigureAndWait(e2, nullptr));
	LoadTox(e2, toxB, "E2 warm B");

	std::printf("\n== 6. Release both instances\n");
	TEInstanceSuspend(e1.instance);
	TERelease(&e1.instance);
	TEInstanceSuspend(e2.instance);
	TERelease(&e2.instance);
	for (int i = 0; i < 10; i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		if (EngineProcesses().empty()) break;
	}
	Snapshot("release both");

	std::printf("\n== Summary\n");
	for (auto& r : gRows) {
		if (r.step.find("snapshot") != std::string::npos) continue;
		std::printf("  %-46s %10s\n", r.step.c_str(), r.seconds < 0 ? "FAILED" : (std::to_string(r.seconds).substr(0, 6) + " s").c_str());
	}
	return 0;
}
