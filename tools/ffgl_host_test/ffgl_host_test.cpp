// A headless FFGL host for testing the engine pool without Resolume.
//
//   ffgl_host_test.exe <plugin.dll> <tox> [--te-dir DIR] [--log FILE]
//
// --te-dir is where TouchEngine.dll is, if not next to the plugin.
//
// Loads the plugin the way a host does (FF_INITIALISE_V2, then clips with
// FF_INSTANTIATE_GL / FF_DEINSTANTIATE_GL on one GL context), renders every
// live clip each frame into an offscreen framebuffer, and reports:
//
//   - how long each new clip takes to show output: the first needs a cold
//     engine; later ones should take a warm or handed-back engine;
//   - how many TouchEngine processes are running after each step;
//   - whether FF_DEINITIALISE shuts every engine down, and whether the
//     process gets through unloading the plugin without crashing.
//
// The plugin's own log (FFGLLog) is printed with timestamps.
// Windows only.

#include <windows.h>
#include <tlhelp32.h>
#include <GL/gl.h>

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

// FFGL ABI, as in src/lib/FFGL/ffgl/FFGL.h.
typedef uint32_t FFUInt32;
typedef union FFMixed { FFUInt32 UIntValue; void* PointerValue; } FFMixed;
typedef void* FFInstanceID;
typedef FFMixed(__stdcall* PlugMainFn)(FFUInt32, FFMixed, FFInstanceID);
typedef void(__stdcall* LogFn)(char*);
typedef void(__stdcall* SetLogCallbackFn)(LogFn);

static const FFUInt32 FF_INITIALISE_V2 = 34;
static const FFUInt32 FF_DEINITIALISE = 2;
static const FFUInt32 FF_SET_PARAMETER = 8;
static const FFUInt32 FF_PROCESS_OPENGL = 17;
static const FFUInt32 FF_INSTANTIATE_GL = 18;
static const FFUInt32 FF_DEINSTANTIATE_GL = 19;
static const FFUInt32 FF_FAIL = 1;

struct SetParameterStruct { FFUInt32 ParameterNumber; FFMixed NewParameterValue; };
struct FFGLViewportStruct { GLuint x, y, width, height; };
struct FFGLTextureStruct { FFUInt32 Width, Height, HardwareWidth, HardwareHeight; GLuint Handle; };
struct ProcessOpenGLStruct { FFUInt32 numInputTextures; FFGLTextureStruct** inputTextures; GLuint HostFBO; };

// GL 3.0 framebuffer entry points, which opengl32 does not export.
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_RGBA8 0x8058
typedef void(APIENTRY* GenFramebuffersFn)(GLsizei, GLuint*);
typedef void(APIENTRY* BindFramebufferFn)(GLenum, GLuint);
typedef void(APIENTRY* FramebufferTexture2DFn)(GLenum, GLenum, GLenum, GLuint, GLint);
static GenFramebuffersFn glGenFramebuffers_;
static BindFramebufferFn glBindFramebuffer_;
static FramebufferTexture2DFn glFramebufferTexture2D_;

static FILE* gLog = nullptr;
static const auto gStart = std::chrono::steady_clock::now();

static double Seconds()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - gStart).count();
}

static void Out(const char* format, ...)
{
	char line[2048];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof line, format, args);
	va_end(args);
	std::printf("[%8.2fs] %s\n", Seconds(), line);
	std::fflush(stdout);
	if (gLog) {
		std::fprintf(gLog, "[%8.2fs] %s\n", Seconds(), line);
		std::fflush(gLog);
	}
}

static void __stdcall PluginLog(char* message)
{
	Out("  plugin: %s", message ? message : "");
}

static int CountEngines()
{
	int count = 0;
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) return -1;
	PROCESSENTRY32W entry = { sizeof entry };
	for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
		if (_wcsicmp(entry.szExeFile, L"TouchEngine.exe") == 0) count++;
	}
	CloseHandle(snapshot);
	return count;
}

struct Host {
	PlugMainFn plugMain = nullptr;
	HWND window = nullptr;
	HDC dc = nullptr;
	HGLRC rc = nullptr;
	GLuint fbo = 0, target = 0;
	static const int W = 640, H = 360;

	FFMixed Call(FFUInt32 code, void* input, FFInstanceID id)
	{
		FFMixed value;
		value.PointerValue = input;
		return plugMain(code, value, id);
	}
};

struct Clip {
	std::string name;
	FFInstanceID id = nullptr;
	double tStart = 0;
	double tFirstOutput = -1;
};

static bool MakeContext(Host& h)
{
	WNDCLASSA wc = {};
	wc.style = CS_OWNDC;
	wc.lpfnWndProc = DefWindowProcA;
	wc.hInstance = GetModuleHandleA(nullptr);
	wc.lpszClassName = "ffgl_host_test";
	RegisterClassA(&wc);
	h.window = CreateWindowA(wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);
	h.dc = GetDC(h.window);
	PIXELFORMATDESCRIPTOR pfd = { sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32 };
	if (!SetPixelFormat(h.dc, ChoosePixelFormat(h.dc, &pfd), &pfd)) return false;
	h.rc = wglCreateContext(h.dc);
	if (!h.rc || !wglMakeCurrent(h.dc, h.rc)) return false;
	glGenFramebuffers_ = (GenFramebuffersFn)wglGetProcAddress("glGenFramebuffers");
	glBindFramebuffer_ = (BindFramebufferFn)wglGetProcAddress("glBindFramebuffer");
	glFramebufferTexture2D_ = (FramebufferTexture2DFn)wglGetProcAddress("glFramebufferTexture2D");
	if (!glGenFramebuffers_ || !glBindFramebuffer_ || !glFramebufferTexture2D_) return false;
	Out("GL %s, %s", (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION));

	glGenTextures(1, &h.target);
	glBindTexture(GL_TEXTURE_2D, h.target);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, Host::W, Host::H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glBindTexture(GL_TEXTURE_2D, 0);
	glGenFramebuffers_(1, &h.fbo);
	glBindFramebuffer_(GL_FRAMEBUFFER, h.fbo);
	glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, h.target, 0);
	glBindFramebuffer_(GL_FRAMEBUFFER, 0);
	return true;
}

// Renders one clip into the offscreen framebuffer and says whether anything
// non-black came out.
static bool RenderClip(Host& h, Clip& c)
{
	glBindFramebuffer_(GL_FRAMEBUFFER, h.fbo);
	glViewport(0, 0, Host::W, Host::H);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	ProcessOpenGLStruct process = { 0, nullptr, h.fbo };
	h.Call(FF_PROCESS_OPENGL, &process, c.id);
	unsigned char pixels[4 * 9] = {};
	for (int i = 0; i < 9; i++) {
		glReadPixels(Host::W * (1 + i % 3) / 4, Host::H * (1 + i / 3) / 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels + 4 * i);
	}
	glBindFramebuffer_(GL_FRAMEBUFFER, 0);
	for (unsigned char p : pixels) {
		if (p != 0) return true;
	}
	return false;
}

static void RenderAll(Host& h, std::vector<Clip*>& clips)
{
	for (Clip* c : clips) {
		bool output = RenderClip(h, *c);
		if (output && c->tFirstOutput < 0) {
			c->tFirstOutput = Seconds();
			Out("%s: first output %.2f s after its tox was set (%d TouchEngine processes)",
				c->name.c_str(), c->tFirstOutput - c->tStart, CountEngines());
		}
	}
	SwapBuffers(h.dc);
	std::this_thread::sleep_for(std::chrono::milliseconds(16));
}

// Renders every live clip for 'seconds', or until 'until' has output.
static void Run(Host& h, std::vector<Clip*>& clips, double seconds, Clip* until = nullptr)
{
	double end = Seconds() + seconds;
	while (Seconds() < end && !(until && until->tFirstOutput >= 0)) {
		MSG msg;
		while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
		RenderAll(h, clips);
	}
}

static bool Open(Host& h, Clip& c, const std::string& name, const std::string& tox)
{
	c.name = name;
	FFGLViewportStruct viewport = { 0, 0, Host::W, Host::H };
	FFMixed result = h.Call(FF_INSTANTIATE_GL, &viewport, nullptr);
	if (result.PointerValue == (void*)(uintptr_t)FF_FAIL || result.PointerValue == nullptr) {
		Out("%s: FF_INSTANTIATE_GL failed", name.c_str());
		return false;
	}
	c.id = result.PointerValue;
	SetParameterStruct set = { 0 };
	set.NewParameterValue.PointerValue = (void*)tox.c_str();
	c.tStart = Seconds();
	h.Call(FF_SET_PARAMETER, &set, c.id);
	Out("%s: opened, tox set (%d TouchEngine processes)", name.c_str(), CountEngines());
	return true;
}

static void Close(Host& h, std::vector<Clip*>& clips, Clip& c)
{
	h.Call(FF_DEINSTANTIATE_GL, nullptr, c.id);
	for (size_t i = 0; i < clips.size(); i++) {
		if (clips[i] == &c) clips.erase(clips.begin() + i);
	}
	Out("%s: closed (%d TouchEngine processes)", c.name.c_str(), CountEngines());
}

int main(int argc, char** argv)
{
	if (argc < 3) {
		std::printf("usage: %s <plugin.dll> <tox> [--te-dir DIR] [--log FILE]\n", argv[0]);
		return 2;
	}
	std::string dll = argv[1], tox = argv[2], teDir;
	for (int i = 3; i + 1 < argc; i++) {
		std::string arg = argv[i];
		if (arg == "--log") gLog = std::fopen(argv[++i], "w");
		else if (arg == "--te-dir") teDir = argv[++i];
	}
	if (!teDir.empty()) {
		// SetDllDirectory wants a full path; a relative one was not searched.
		char full[MAX_PATH];
		DWORD length = GetFullPathNameA(teDir.c_str(), MAX_PATH, full, nullptr);
		if (length == 0 || length >= MAX_PATH || !SetDllDirectoryA(full)) {
			Out("could not add %s to the DLL search path", teDir.c_str());
		}
		else {
			Out("TouchEngine.dll search path: %s", full);
		}
	}

	Host h;
	if (!MakeContext(h)) {
		Out("could not create a GL context");
		return 1;
	}
	Out("TouchEngine processes before loading the plugin: %d", CountEngines());

	// By full path with the altered search order, as hosts load plugins, so
	// the plugin's own folder is searched for TouchEngine.dll. Plain
	// LoadLibrary only searches this program's folder and the system ones.
	char fullDll[MAX_PATH];
	DWORD fullLength = GetFullPathNameA(dll.c_str(), MAX_PATH, fullDll, nullptr);
	if (fullLength == 0 || fullLength >= MAX_PATH) {
		Out("could not resolve %s", dll.c_str());
		return 1;
	}
	HMODULE module = LoadLibraryExA(fullDll, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!module) {
		Out("LoadLibrary failed: %lu", GetLastError());
		return 1;
	}
	h.plugMain = (PlugMainFn)GetProcAddress(module, "plugMain");
	if (auto setLog = (SetLogCallbackFn)GetProcAddress(module, "SetLogCallback")) setLog(PluginLog);
	if (!h.plugMain) {
		Out("no plugMain export");
		return 1;
	}
	if (h.Call(FF_INITIALISE_V2, nullptr, nullptr).UIntValue == FF_FAIL) {
		Out("FF_INITIALISE_V2 failed");
		return 1;
	}
	Out("plugin initialised (%d TouchEngine processes)", CountEngines());

	std::vector<Clip*> live;
	Clip a, b, c, d;

	Out("== 1. first clip: needs a cold engine");
	if (!Open(h, a, "A", tox)) return 1;
	live.push_back(&a);
	Run(h, live, 60, &a);
	Out("== 2. let the warm engine start");
	Run(h, live, 25);
	Out("TouchEngine processes with one clip and a warm engine: %d", CountEngines());

	Out("== 3. second clip: should take the warm engine");
	Open(h, b, "B", tox);
	live.push_back(&b);
	Run(h, live, 60, &b);
	Run(h, live, 3);

	Out("== 4. close A, then a third clip: should take A's engine back");
	Close(h, live, a);
	Run(h, live, 3);
	Open(h, c, "C", tox);
	live.push_back(&c);
	Run(h, live, 60, &c);
	Run(h, live, 3);

	Out("== 5. a fourth clip at once: takes whatever is idle or warm");
	Open(h, d, "D", tox);
	live.push_back(&d);
	Run(h, live, 60, &d);
	Run(h, live, 3);

	Out("== 6. close everything");
	Close(h, live, b);
	Close(h, live, c);
	Close(h, live, d);
	Out("TouchEngine processes with no clips: %d", CountEngines());

	Out("== 7. FF_DEINITIALISE: the pool releases every engine");
	h.Call(FF_DEINITIALISE, nullptr, nullptr);
	for (int i = 0; i < 40 && CountEngines() > 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(250));
	Out("TouchEngine processes after FF_DEINITIALISE: %d", CountEngines());

	FreeLibrary(module);
	Out("plugin unloaded");

	Out("== Summary");
	for (Clip* x : { &a, &b, &c, &d }) {
		Out("%s: %s", x->name.c_str(), x->tFirstOutput < 0 ? "no output" :
			(std::to_string(x->tFirstOutput - x->tStart).substr(0, 5) + " s to first output").c_str());
	}
	if (gLog) std::fclose(gLog);
	return 0;
}
