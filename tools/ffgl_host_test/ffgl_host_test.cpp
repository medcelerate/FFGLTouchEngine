// A headless FFGL host for testing the engine pool without Resolume.
//
//   ffgl_host_test.exe <source.dll> <tox> [--fx <fx.dll> <passthrough.tox>]
//                      [--te-dir DIR] [--log FILE]
//
// --te-dir is where TouchEngine.dll is, if not next to the plugin.
//
// Loads the plugins the way a host does (FF_INITIALISE_V2, then clips with
// FF_INSTANTIATE_GL / FF_DEINSTANTIATE_GL on one GL context), renders every
// live clip each frame into an offscreen framebuffer, and runs:
//
//   1. Pool: four Source clips in turn. The first needs a cold engine; the
//      others should take a warm or handed-back one. Reports each clip's
//      time to first output and the TouchEngine process count.
//   2. Edge cases on the Source plugin: a clip closed while its engine is
//      still cold-starting (the pool should give that engine up after its
//      unload timeout), and Clear Instance sent with a frame in flight,
//      then Reload.
//   3. With --fx: an FX clip fed a solid orange input through a pass-through
//      tox (an In TOP 'in1' wired to an Out TOP 'out1'), running alongside a
//      Source clip. Its output should be the same orange.
//   4. FF_DEINITIALISE on each plugin: every engine should be released, and
//      the process should get through unloading the plugins.
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
#include <cstring>
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

struct Plugin {
	std::string name;
	HMODULE module = nullptr;
	PlugMainFn plugMain = nullptr;

	FFMixed Call(FFUInt32 code, void* input, FFInstanceID id)
	{
		FFMixed value;
		value.PointerValue = input;
		return plugMain(code, value, id);
	}
};

struct Host {
	HWND window = nullptr;
	HDC dc = nullptr;
	HGLRC rc = nullptr;
	GLuint fbo = 0, target = 0;
	static const int W = 640, H = 360;
};

struct Clip {
	std::string name;
	Plugin* plugin = nullptr;
	FFInstanceID id = nullptr;
	GLuint input = 0; // FX clips: the texture fed to the plugin
	double tStart = 0;
	double tFirstOutput = -1;
	unsigned char firstOutput[4] = {};
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

static std::vector<unsigned char> Solid(unsigned char r, unsigned char g, unsigned char b)
{
	std::vector<unsigned char> pixels((size_t)Host::W * Host::H * 4);
	for (size_t i = 0; i < pixels.size(); i += 4) {
		pixels[i] = r;
		pixels[i + 1] = g;
		pixels[i + 2] = b;
		pixels[i + 3] = 255;
	}
	return pixels;
}

// A solid-colour RGBA8 texture standing in for an FX clip's input.
static GLuint MakeInput(unsigned char r, unsigned char g, unsigned char b)
{
	std::vector<unsigned char> pixels = Solid(r, g, b);
	GLuint texture = 0;
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, Host::W, Host::H, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glBindTexture(GL_TEXTURE_2D, 0);
	return texture;
}

static void Recolour(GLuint texture, unsigned char r, unsigned char g, unsigned char b)
{
	std::vector<unsigned char> pixels = Solid(r, g, b);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, Host::W, Host::H, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
	glBindTexture(GL_TEXTURE_2D, 0);
}

// Renders one clip into the offscreen framebuffer and says whether anything
// non-black came out. 'centre' gets the middle pixel.
static bool RenderClip(Host& h, Clip& c, unsigned char centre[4])
{
	glBindFramebuffer_(GL_FRAMEBUFFER, h.fbo);
	glViewport(0, 0, Host::W, Host::H);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	FFGLTextureStruct input = { (FFUInt32)Host::W, (FFUInt32)Host::H, (FFUInt32)Host::W, (FFUInt32)Host::H, c.input };
	FFGLTextureStruct* inputs[1] = { &input };
	ProcessOpenGLStruct process = { c.input ? 1u : 0u, c.input ? inputs : nullptr, h.fbo };
	c.plugin->Call(FF_PROCESS_OPENGL, &process, c.id);
	unsigned char pixels[4 * 9] = {};
	for (int i = 0; i < 9; i++) {
		glReadPixels(Host::W * (1 + i % 3) / 4, Host::H * (1 + i / 3) / 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels + 4 * i);
	}
	glBindFramebuffer_(GL_FRAMEBUFFER, 0);
	for (int i = 0; i < 4; i++) centre[i] = pixels[16 + i];
	for (unsigned char p : pixels) {
		if (p != 0) return true;
	}
	return false;
}

static void RenderAll(Host& h, std::vector<Clip*>& clips)
{
	MSG msg;
	while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
	for (Clip* c : clips) {
		unsigned char centre[4];
		bool output = RenderClip(h, *c, centre);
		if (output && c->tFirstOutput < 0) {
			c->tFirstOutput = Seconds();
			for (int i = 0; i < 4; i++) c->firstOutput[i] = centre[i];
			Out("%s: first output %.2f s after its tox was set, centre rgba %d %d %d %d (%d TouchEngine processes)",
				c->name.c_str(), c->tFirstOutput - c->tStart, centre[0], centre[1], centre[2], centre[3], CountEngines());
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
		RenderAll(h, clips);
	}
}

static void SetFloat(Clip& c, FFUInt32 index, float value)
{
	SetParameterStruct set = { index };
	std::memcpy(&set.NewParameterValue.UIntValue, &value, sizeof value);
	c.plugin->Call(FF_SET_PARAMETER, &set, c.id);
}

// Event parameters fire on 1; set back to 0 as a host does.
static void Pulse(Clip& c, FFUInt32 index)
{
	SetFloat(c, index, 1);
	SetFloat(c, index, 0);
}

static void SetTox(Clip& c, const std::string& tox)
{
	SetParameterStruct set = { 0 };
	set.NewParameterValue.PointerValue = (void*)tox.c_str();
	c.tStart = Seconds();
	c.tFirstOutput = -1;
	c.plugin->Call(FF_SET_PARAMETER, &set, c.id);
}

static bool Open(Plugin& p, std::vector<Clip*>& live, Clip& c, const std::string& name, const std::string& tox, GLuint input = 0)
{
	c.name = name;
	c.plugin = &p;
	c.input = input;
	FFGLViewportStruct viewport = { 0, 0, Host::W, Host::H };
	FFMixed result = p.Call(FF_INSTANTIATE_GL, &viewport, nullptr);
	if (result.PointerValue == (void*)(uintptr_t)FF_FAIL || result.PointerValue == nullptr) {
		Out("%s: FF_INSTANTIATE_GL failed", name.c_str());
		return false;
	}
	c.id = result.PointerValue;
	SetTox(c, tox);
	live.push_back(&c);
	Out("%s: opened on %s, tox set (%d TouchEngine processes)", name.c_str(), p.name.c_str(), CountEngines());
	return true;
}

static void Close(std::vector<Clip*>& live, Clip& c)
{
	c.plugin->Call(FF_DEINSTANTIATE_GL, nullptr, c.id);
	for (size_t i = 0; i < live.size(); i++) {
		if (live[i] == &c) live.erase(live.begin() + i);
	}
	Out("%s: closed (%d TouchEngine processes)", c.name.c_str(), CountEngines());
}

static bool Load(Plugin& p, const std::string& name, const std::string& dll)
{
	p.name = name;
	// By full path with the altered search order, as hosts load plugins, so
	// the plugin's own folder is searched for TouchEngine.dll. Plain
	// LoadLibrary only searches this program's folder and the system ones.
	char full[MAX_PATH];
	DWORD length = GetFullPathNameA(dll.c_str(), MAX_PATH, full, nullptr);
	if (length == 0 || length >= MAX_PATH) {
		Out("could not resolve %s", dll.c_str());
		return false;
	}
	p.module = LoadLibraryExA(full, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!p.module) {
		Out("LoadLibrary %s failed: %lu", full, GetLastError());
		return false;
	}
	p.plugMain = (PlugMainFn)GetProcAddress(p.module, "plugMain");
	if (auto setLog = (SetLogCallbackFn)GetProcAddress(p.module, "SetLogCallback")) setLog(PluginLog);
	if (!p.plugMain) {
		Out("%s has no plugMain export", full);
		return false;
	}
	if (p.Call(FF_INITIALISE_V2, nullptr, nullptr).UIntValue == FF_FAIL) {
		Out("FF_INITIALISE_V2 failed for %s", full);
		return false;
	}
	Out("%s initialised (%d TouchEngine processes)", name.c_str(), CountEngines());
	return true;
}

static void Report(const Clip& c)
{
	if (c.name.empty()) return;
	if (c.tFirstOutput < 0) {
		Out("%-3s %s", c.name.c_str(), "no output");
	}
	else {
		Out("%-3s %6.2f s to first output, centre rgba %d %d %d %d", c.name.c_str(), c.tFirstOutput - c.tStart,
			c.firstOutput[0], c.firstOutput[1], c.firstOutput[2], c.firstOutput[3]);
	}
}

int main(int argc, char** argv)
{
	if (argc < 3) {
		std::printf("usage: %s <source.dll> <tox> [--fx <fx.dll> <passthrough.tox>] [--te-dir DIR] [--log FILE]\n", argv[0]);
		return 2;
	}
	std::string dll = argv[1], tox = argv[2], teDir, fxDll, fxTox;
	for (int i = 3; i < argc; i++) {
		std::string arg = argv[i];
		if (arg == "--log" && i + 1 < argc) gLog = std::fopen(argv[++i], "w");
		else if (arg == "--te-dir" && i + 1 < argc) teDir = argv[++i];
		else if (arg == "--fx" && i + 2 < argc) {
			fxDll = argv[++i];
			fxTox = argv[++i];
		}
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

	Plugin source;
	if (!Load(source, "Source", dll)) return 1;

	std::vector<Clip*> live;
	Clip a, b, c, d, e1, e2, x, s;

	Out("== 1. pool");
	Out("-- A: first clip, needs a cold engine");
	if (!Open(source, live, a, "A", tox)) return 1;
	Run(h, live, 60, &a);
	Out("-- let the warm engine start");
	Run(h, live, 25);
	Out("TouchEngine processes with one clip and a warm engine: %d", CountEngines());

	Out("-- B: should take the warm engine");
	Open(source, live, b, "B", tox);
	Run(h, live, 60, &b);
	Run(h, live, 3);

	Out("-- close A, then C: should take A's engine back");
	Close(live, a);
	Run(h, live, 3);
	Open(source, live, c, "C", tox);
	Run(h, live, 60, &c);
	Run(h, live, 3);

	Out("-- D: takes whatever is idle or warm");
	Open(source, live, d, "D", tox);
	Run(h, live, 60, &d);
	Run(h, live, 3);

	Out("-- close B, C and D");
	Close(live, b);
	Close(live, c);
	Close(live, d);
	Run(h, live, 2);
	Out("TouchEngine processes with no clips: %d", CountEngines());

	Out("== 2. edge cases");
	Out("-- E1 and E2 back to back: E2 should find no idle engine and start one cold; close E2 at once");
	Open(source, live, e1, "E1", tox);
	Open(source, live, e2, "E2", tox);
	Run(h, live, 0.1);
	Close(live, e2);
	Run(h, live, 60, &e1);
	Out("-- keep E1 running past the pool's 5 s unload timeout, so it gives up E2's engine if that never unloaded");
	Run(h, live, 10);
	Out("TouchEngine processes with E1 running: %d", CountEngines());

	Out("-- Clear Instance on E1 straight after a frame starts, then Reload");
	RenderAll(h, live);
	Pulse(e1, 3);
	Out("E1: cleared (%d TouchEngine processes)", CountEngines());
	Run(h, live, 2);
	e1.tStart = Seconds();
	e1.tFirstOutput = -1;
	Pulse(e1, 1);
	Out("E1: reload sent");
	Run(h, live, 60, &e1);
	Run(h, live, 3);
	Close(live, e1);

	Plugin fx;
	if (!fxDll.empty()) {
		Out("== 3. FX with an orange input, alongside a Source clip");
		if (Load(fx, "FX", fxDll)) {
			GLuint orange = MakeInput(255, 128, 0);
			Open(fx, live, x, "X", fxTox, orange);
			Open(source, live, s, "S", tox);
			Run(h, live, 60, &x);
			Run(h, live, 60, &s);
			Run(h, live, 5);

			// The FX plugin draws its input underneath TouchEngine's output,
			// so orange alone proves nothing. Turn the input blue: that base
			// layer turns blue at once, while TouchEngine's output, drawn
			// over it, lags at least a frame behind. Orange for a frame or
			// more, then blue, means the input went through TouchEngine and
			// back; blue straight away means only the base layer is showing.
			Recolour(orange, 0, 64, 255);
			std::string sequence;
			int firstBlue = -1;
			for (int f = 0; f < 30; f++) {
				unsigned char centre[4];
				RenderClip(h, x, centre);
				bool blue = centre[2] > 200 && centre[0] < 60;
				bool isOrange = centre[0] > 200 && centre[2] < 60;
				sequence += blue ? 'B' : isOrange ? 'O' : '?';
				if (blue && firstBlue < 0) firstBlue = f;
				SwapBuffers(h.dc);
				std::this_thread::sleep_for(std::chrono::milliseconds(16));
			}
			Out("X: frames after the input turned blue (O orange, B blue, ? other): %s", sequence.c_str());
			Out("X: %s", firstBlue < 0 ? "never turned blue: TouchEngine's output is stuck" :
				firstBlue == 0 ? "blue at once: only the plugin's own base layer is showing, not TouchEngine's output" :
				"orange first, then blue: the input reached TouchEngine and came back through the pass-through");
			Close(live, x);
			Close(live, s);
			glDeleteTextures(1, &orange);
		}
	}
	else {
		Out("== 3. skipped: no --fx");
	}

	Out("== 4. FF_DEINITIALISE and unload");
	for (Plugin* p : { &source, &fx }) {
		if (!p->module) continue;
		p->Call(FF_DEINITIALISE, nullptr, nullptr);
		for (int i = 0; i < 40 && CountEngines() > 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(250));
		Out("%s: deinitialised (%d TouchEngine processes)", p->name.c_str(), CountEngines());
	}
	for (Plugin* p : { &source, &fx }) {
		if (p->module) FreeLibrary(p->module);
	}
	Out("plugins unloaded (%d TouchEngine processes)", CountEngines());

	Out("== Summary");
	for (Clip* clip : { &a, &b, &c, &d, &e1, &x, &s }) Report(*clip);
	if (gLog) std::fclose(gLog);
	return 0;
}
