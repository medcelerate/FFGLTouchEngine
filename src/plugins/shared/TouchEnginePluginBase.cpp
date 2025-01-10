#include "TouchEnginePluginBase.h"

FFResult FailAndLog(std::string message)
{
	FFGLLog::LogToHost(message.c_str());
	return FF_FAIL;
}

std::string GetSeverityString(TESeverity severity) {
	switch (severity) {
	case TESeverityWarning:
		return "Warning";
	case TESeverityError:
		return "Error";
	default:
		return "Unknown";
	}

}

std::string GenerateRandomString(size_t length) {
	auto randchar = []() -> char {
		const char charset[] =
			"0123456789"
			"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
			"abcdefghijklmnopqrstuvwxyz";
		const size_t max_index = (sizeof(charset) - 1);
		return charset[rand() % max_index];
		};
	std::string str(length, 0);
	std::generate_n(str.begin(), length, randchar);
	return str;
}

#ifdef _WIN32
DXGI_FORMAT GlToDXFromat(GLint format) {

	switch (format) {
	case GL_RGBA8:
		return DXGI_FORMAT_B8G8R8A8_UNORM;

	case GL_RGB8:
		return DXGI_FORMAT_R8G8B8A8_UNORM;

	case GL_RGBA16:
		return DXGI_FORMAT_R16G16B16A16_UNORM;



	default:
		auto s = "Unsupported Format:: " + std::to_string(format);
		FFGLLog::LogToHost(s.c_str());
	}
}
#endif

GLenum GetGlType(GLint format) {
	switch (format) {
	case GL_UNSIGNED_BYTE:
		return GL_RGBA;
	case GL_RGBA16:
		return GL_UNSIGNED_SHORT;
	}
}

#ifdef _WIN32
GLenum GetGlType(DXGI_FORMAT format) {
	switch (format) {
	case DXGI_FORMAT_B8G8R8A8_UNORM:
		return GL_UNSIGNED_BYTE;
	case DXGI_FORMAT_R16G16B16A16_UNORM:
		return GL_UNSIGNED_SHORT;
	case DXGI_FORMAT_R32G32B32A32_FLOAT:
		return GL_FLOAT;
	default:
		return GL_UNSIGNED_BYTE;
	}
}
#endif

FFGLTouchEnginePluginBase::FFGLTouchEnginePluginBase()
	: CFFGLPlugin(),
	isTouchEngineLoaded(false),
	isTouchEngineReady(false),
	isGraphicsContextLoaded(false),
	isTouchFrameBusy(false)
{
	// Parameters
	SetParamInfof(0, "Tox File", FF_TYPE_FILE);
	SetParamInfof(1, "Reload", FF_TYPE_EVENT);
	SetParamInfof(2, "Unload", FF_TYPE_EVENT);
	SetParamInfof(3, "Clear Instance", FF_TYPE_EVENT);

	//This is the starting point for the parameters and is equal to the number of parameters above.
	OffsetParamsByType = 4;

	MaxParamsByType = 40;
}

FFGLTouchEnginePluginBase::~FFGLTouchEnginePluginBase()
{
	if (instance != nullptr) {
		TEInstanceSuspend(instance);
		TEInstanceUnload(instance);
	}
}

FFResult FFGLTouchEnginePluginBase::InitializeDevice()
{
#ifdef _WIN32
	// Create D3D11 device
	HRESULT hr = D3D11CreateDevice(
		nullptr,
		D3D_DRIVER_TYPE_HARDWARE,
		nullptr,
		0,
		nullptr,
		0,
		D3D11_SDK_VERSION,
		D3DDevice.GetAddressOf(),
		nullptr,
		nullptr
	);
	if (FAILED(hr)) {
		return FailAndLog("Failed to create D3D11 device");
	}
#endif

	return FF_SUCCESS;
}

FFResult FFGLTouchEnginePluginBase::InitializeShader(const std::string& vertexShaderCode, const std::string& fragmentShaderCode)
{
	if (!shader.Compile(vertexShaderCode, fragmentShaderCode)) {
		DeInitGL();
		return FailAndLog("Failed to compile shader");

	}

	// Initialize the quad
	if (!quad.Initialise()) {
		DeInitGL();
		return FailAndLog("Failed to initialize quad");
	}

	return FF_SUCCESS;
}

FFResult FFGLTouchEnginePluginBase::DeInitGL()
{
	return FF_SUCCESS;
}

bool FFGLTouchEnginePluginBase::LoadTEFile()
{
	// Load the tox file into the TouchEngine
	// 1. Create a TouchEngine object


	if (instance == nullptr) {
		return false;
	}

	isTouchEngineReady = false;

	// 2. Load the tox file into the TouchEngine
	TEResult result = TEInstanceConfigure(instance, FilePath.c_str(), TETimeExternal);
	if (result != TEResultSuccess) {
		return false;
	}

	result = TEInstanceSetFrameRate(instance, 60, 1);

	if (result != TEResultSuccess) {
		return false;
	}


	result = TEInstanceLoad(instance);
	if (result != TEResultSuccess) {
		return false;
	}


	return true;
}
