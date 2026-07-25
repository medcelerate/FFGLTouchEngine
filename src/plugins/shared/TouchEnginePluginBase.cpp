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

// Log (but do not abort enumeration) when a single link cannot be read. Keeping
// the walk alive is the whole point of the enumeration-robustness work: a bad or
// unknown link must skip itself, never drop every parameter that follows it.
static void LogLinkSkip(const char* identifier, const char* reason) {
	std::string msg = std::string("FFGLTouchEngine: skipping parameter '") +
		(identifier ? identifier : "?") + "' — " + reason;
	FFGLLog::LogToHost(msg.c_str());
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
	isTouchFrameBusy(false),
	isBeingDestroyed(false)
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
	// Mark as destroying so event callbacks are ignored
	isBeingDestroyed = true;

	if (instance != nullptr) {
		if (isTouchEngineLoaded) {
			isTouchEngineLoaded = false;
			isTouchEngineReady = false;
			TEInstanceSuspend(instance);
			TEInstanceUnload(instance);
		}
		instance.reset();
	}
#ifdef __APPLE__
	MetalContext.reset();
	MetalCommandQueue = nil;
	MetalDevice = nil;
#endif
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
#ifdef __APPLE__
	if (MetalDevice == nil) {
		MetalDevice = MTLCreateSystemDefaultDevice();
		if (MetalDevice == nil) {
			return FailAndLog("Failed to create Metal device");
		}
		MetalCommandQueue = [MetalDevice newCommandQueue];
		if (MetalCommandQueue == nil) {
			return FailAndLog("Failed to create Metal command queue");
		}
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

void FFGLTouchEnginePluginBase::InitializeGlTexture(GLuint& texture, uint16_t width, uint16_t height, GLenum type) {
	if (texture != 0) {
		glDeleteTextures(1, &texture);
		texture = 0;
	}

	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, type, NULL);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glBindTexture(GL_TEXTURE_2D, 0);
}

bool FFGLTouchEnginePluginBase::LoadTEGraphicsContext(bool reload) {
	if (isGraphicsContextLoaded && !reload) {
		return true;
	}

	if (instance == nullptr) {
		return false;
	}


	// Load the TouchEngine graphics context

#ifdef _WIN32
	if (D3DDevice == nullptr) {
		FFGLLog::LogToHost("D3D11 Device Not Available, You Probably Failed Somewhere...In Your Life");
	}

	TEResult result = TED3D11ContextCreate(D3DDevice.Get(), D3DContext.take());
	if (result != TEResultSuccess) {
		return false;
	}

	result = TEInstanceAssociateGraphicsContext(instance, D3DContext);
	if (result != TEResultSuccess) {
		return false;
	}
	isGraphicsContextLoaded = true;
#endif
#ifdef __APPLE__
	if (MetalDevice == nil) {
		FFGLLog::LogToHost("Metal Device Not Available");
		return false;
	}

	TEResult result = TEMetalContextCreate(MetalDevice, MetalContext.take());
	if (result != TEResultSuccess) {
		FFGLLog::LogToHost("Failed to create TEMetalContext");
		return false;
	}

	result = TEInstanceAssociateGraphicsContext(instance, MetalContext);
	if (result != TEResultSuccess) {
		FFGLLog::LogToHost("Failed to associate Metal graphics context");
		return false;
	}
	isGraphicsContextLoaded = true;
#endif
	return isGraphicsContextLoaded;
}

bool FFGLTouchEnginePluginBase::LoadTEFile()
{
	std::lock_guard<std::recursive_mutex> lock(TEStateMutex);
	// Load the tox file into the TouchEngine
	// 1. Create a TouchEngine object


	if (instance == nullptr) {
		return false;
	}

	// No tox assigned yet (fresh instance): nothing to load, and attempting it
	// would log a spurious "TEInstanceLoad failed for ''".
	if (FilePath.empty()) {
		return false;
	}

	if (isLoadPending) {
		// Queue instead of dropping: replayed from the in-flight load's DidLoad.
		FFGLLog::LogToHost("FFGLTouchEngine: load already in progress — queueing reload");
		isReloadQueued = true;
		return false;
	}

	isTouchEngineReady = false;

	// 2. Load the tox file into the TouchEngine
	TEResult result = TEInstanceConfigure(instance, FilePath.c_str(), TETimeExternal);
	if (result != TEResultSuccess) {
		const char* desc = TEResultGetDescription(result);
		std::string msg = std::string("FFGLTouchEngine: TEInstanceConfigure failed for '") +
			FilePath + "' — " + (desc ? desc : "unknown error");
		FFGLLog::LogToHost(msg.c_str());
		return false;
	}

	result = TEInstanceSetFrameRate(instance, 60, 1);

	if (result != TEResultSuccess) {
		const char* desc = TEResultGetDescription(result);
		std::string msg = std::string("FFGLTouchEngine: TEInstanceSetFrameRate failed — ") +
			(desc ? desc : "unknown error");
		FFGLLog::LogToHost(msg.c_str());
		return false;
	}


	result = TEInstanceLoad(instance);
	if (result != TEResultSuccess) {
		const char* desc = TEResultGetDescription(result);
		std::string msg = std::string("FFGLTouchEngine: TEInstanceLoad failed for '") +
			FilePath + "' — " + (desc ? desc : "unknown error") +
			". The tox may have been authored in a newer TouchDesigner build than this engine.";
		FFGLLog::LogToHost(msg.c_str());
		return false;
	}

	isLoadPending = true;

	return true;
}

void FFGLTouchEnginePluginBase::LoadTouchEngine() {

	if (instance == nullptr) {

		FFGLLog::LogToHost("Loading TouchEngine");
		TEResult result = TEInstanceCreate(eventCallbackStatic, linkCallbackStatic, this, instance.take());
		if (result != TEResultSuccess) {
			FFGLLog::LogToHost("Failed to create TouchEngine instance");
			instance.reset();
			return;
		}

	}

}

FFResult FFGLTouchEnginePluginBase::SetFloatParameter(unsigned int dwIndex, float value) {
	std::lock_guard<std::recursive_mutex> lock(TEStateMutex);

	if (dwIndex == 1 && value == 1) {
		LoadTouchEngine();
		LoadTEFile();
		return FF_SUCCESS;
	}

	if (dwIndex == 2 && value == 1) {
		if (isTouchEngineLoaded) {
			TEInstanceSuspend(instance);
			TEInstanceUnload(instance);
		}
		ResetBaseParameters();
		return FF_SUCCESS;
	}

	if (dwIndex == 3 && value == 1) {
		ResetBaseParameters();
		ClearTouchInstance();
		return FF_SUCCESS;
	}

	if (!isTouchEngineLoaded || !isTouchEngineReady) {
		return FF_SUCCESS;
	}

	if (ActiveParams.find(dwIndex) == ActiveParams.end()) {
		return FF_SUCCESS;
	}

	FFUInt32 type = ParameterMapType[dwIndex];


	if (type == FF_TYPE_INTEGER) {
		ParameterMapInt[dwIndex] = static_cast<int32_t>(value);
		return FF_SUCCESS;
	}

	if (type == FF_TYPE_BOOLEAN || type == FF_TYPE_EVENT) {
		ParameterMapBool[dwIndex] = value;
		return FF_SUCCESS;
	}

	if (type == FF_TYPE_OPTION) {
		ParameterMapInt[dwIndex] = static_cast<int32_t>(value);
		return FF_SUCCESS;
	}


	ParameterMapFloat[dwIndex] = value;

	return FF_SUCCESS;
}

FFResult FFGLTouchEnginePluginBase::SetTextParameter(unsigned int dwIndex, const char* value) {
	std::lock_guard<std::recursive_mutex> lock(TEStateMutex);
	switch (dwIndex) {
	case 0:
		// Open file dialog
		FilePath = std::string(value);
		LoadTEFile();
		return FF_SUCCESS;
	}

	if (!isTouchEngineLoaded || !isTouchEngineReady) {
		return FF_SUCCESS;
	}

	if (ActiveParams.find(dwIndex) == ActiveParams.end()) {
		return FF_SUCCESS;
	}
	ParameterMapString[dwIndex] = value;
	return FF_SUCCESS;
}

float FFGLTouchEnginePluginBase::GetFloatParameter(unsigned int dwIndex) {
	std::lock_guard<std::recursive_mutex> lock(TEStateMutex);

	if (dwIndex == 1) {
		return 0;
	}
	if (!isTouchEngineLoaded || !isTouchEngineReady) {
		return 0;

	}

	if (ActiveParams.find(dwIndex) == ActiveParams.end()) {
		return 0;
	}

	FFUInt32 type = ParameterMapType[dwIndex];

	if (type == FF_TYPE_INTEGER) {
		return static_cast<float>(ParameterMapInt[dwIndex]);
	}

	if (type == FF_TYPE_BOOLEAN || type == FF_TYPE_EVENT) {
		return ParameterMapBool[dwIndex];
	}


	return static_cast<float>(ParameterMapFloat[dwIndex]);
}

char* FFGLTouchEnginePluginBase::GetTextParameter(unsigned int dwIndex) {
	std::lock_guard<std::recursive_mutex> lock(TEStateMutex);
	if (dwIndex == 0) {
		return (char*)FilePath.c_str();
	}

	if (!isTouchEngineLoaded || !isTouchEngineReady) {
		return nullptr;
	}

	if (ActiveParams.find(dwIndex) == ActiveParams.end()) {
		return nullptr;
	}

	return (char*)ParameterMapString[dwIndex].c_str();
}

void FFGLTouchEnginePluginBase::ConstructBaseParameters() {
	for (uint32_t i = OffsetParamsByType; i < MaxParamsByType + OffsetParamsByType; i++) {
		SetParamInfof(i, (std::string("Parameter") + std::to_string(i)).c_str(), FF_TYPE_STANDARD);
		SetParamVisibility(i, false, false);
	}


	for (uint32_t i = MaxParamsByType + OffsetParamsByType; i < (MaxParamsByType * 2) + OffsetParamsByType; i++) {
		SetParamInfof(i, (std::string("Parameter") + std::to_string(i)).c_str(), FF_TYPE_INTEGER);
		SetParamRange(i, -10000, 10000);
		SetParamVisibility(i, false, false);
	}

	for (uint32_t i = (MaxParamsByType * 2) + OffsetParamsByType; i < (MaxParamsByType * 3) + OffsetParamsByType; i++) {
		SetParamInfof(i, (std::string("Parameter") + std::to_string(i)).c_str(), FF_TYPE_BOOLEAN);
		SetParamVisibility(i, false, false);
	}


	for (uint32_t i = (MaxParamsByType * 3) + OffsetParamsByType; i < (MaxParamsByType * 4) + OffsetParamsByType; i++) {
		SetParamInfof(i, (std::string("Parameter") + std::to_string(i)).c_str(), FF_TYPE_TEXT);
		SetParamVisibility(i, false, false);
	}


	for (uint32_t i = (MaxParamsByType * 4) + OffsetParamsByType; i < (MaxParamsByType * 5) + OffsetParamsByType; i++) {
		SetParamInfof(i, (std::string("Pulse")).c_str(), FF_TYPE_EVENT);
		SetParamVisibility(i, false, false);
	}

	for (uint32_t i = (MaxParamsByType * 5) + OffsetParamsByType; i < (MaxParamsByType * 6) + OffsetParamsByType; i++) {
		SetOptionParamInfo(i, (std::string("Parameter") + std::to_string(i)).c_str(), 10, 0);
		SetParamVisibility(i, false, false);
	}

	// Pre-allocate color picker slots (groups of 4: R, G, B, A)
	// Each color uses 4 consecutive param slots
	uint32_t colorBase = (MaxParamsByType * 6) + OffsetParamsByType;
	for (uint32_t i = 0; i < MaxParamsByType; i += 4) {
		SetParamInfo(colorBase + i,     "Color", FF_TYPE_RED,        0.0f);
		SetParamInfo(colorBase + i + 1, "Color", FF_TYPE_GREEN,      0.0f);
		SetParamInfo(colorBase + i + 2, "Color", FF_TYPE_BLUE,       0.0f);
		SetParamInfo(colorBase + i + 3, "Color", FF_TYPE_ALPHA,      1.0f);
		SetParamVisibility(colorBase + i,     false, false);
		SetParamVisibility(colorBase + i + 1, false, false);
		SetParamVisibility(colorBase + i + 2, false, false);
		SetParamVisibility(colorBase + i + 3, false, false);
	}
}

void FFGLTouchEnginePluginBase::ResetBaseParameters() {
	for (auto& ParamID : ActiveParams) {
		SetParamVisibility(ParamID, false, true);
	}

	hasVideoOutput = false;
	ActiveParams.clear();
	ActiveVectorParams.clear();
	VectorParameters.clear();
	ParameterMapFloat.clear();
	ParameterMapInt.clear();
	ParameterMapString.clear();
	ParameterMapBool.clear();
	PulseParameters.clear();
	ColorParamCount = 0;
	Parameters.clear();
}

void FFGLTouchEnginePluginBase::GetAllParameters() {
	std::lock_guard<std::recursive_mutex> lock(TEStateMutex);
	ResetBaseParameters();

	TouchObject<TEStringArray> groupLinkInfo;

	if (instance == nullptr) {
		return;
	}

	TEResult result = TEInstanceGetLinkGroups(instance, TEScopeInput, groupLinkInfo.take());

	if (result != TEResultSuccess) {
		const char* desc = TEResultGetDescription(result);
		std::string msg = std::string("FFGLTouchEngine: failed to enumerate input link groups — ") +
			(desc ? desc : "unknown error");
		FFGLLog::LogToHost(msg.c_str());
		return;
	}

	for (int i = 0; i < groupLinkInfo->count; i++) {
		TouchObject<TEStringArray> links;
		result = TEInstanceLinkGetChildren(instance, groupLinkInfo->strings[i], links.take());

		if (result != TEResultSuccess) {
			// Skip only this group; keep walking the rest so one bad group can't
			// drop every parameter that follows it.
			LogLinkSkip(groupLinkInfo->strings[i], "failed to read group children");
			continue;
		}

		for (int j = 0; j < links->count; j++) {
			TouchObject<TELinkInfo> linkInfo;
			result = TEInstanceLinkGetInfo(instance, links->strings[j], linkInfo.take());

			if (result != TEResultSuccess) {
				LogLinkSkip(links->strings[j], "failed to read link info");
				continue;
			}


			if (linkInfo->domain == TELinkDomainParameter) {

				if (ActiveParams.size() >= MaxParamsByType * 7) {
					FFGLLog::LogToHost("Too many parameters, skipping");
					continue;
				}

				if (linkInfo->type == TELinkTypeGroup) {
					CreateParametersFromGroup(linkInfo);
				} else {
					CreateIndividualParameter(linkInfo);
				}
			} else if (linkInfo->domain == TELinkDomainOperator) {
				HandleOperatorLink(linkInfo);
			}

		}
	}

	// Get the texture output if it exists
	result = TEInstanceGetLinkGroups(instance, TEScopeOutput, groupLinkInfo.take());

	if (result != TEResultSuccess) {
		return;
	}

	for (int i = 0; i < groupLinkInfo->count; i++) {
		TouchObject<TEStringArray> links;
		result = TEInstanceLinkGetChildren(instance, groupLinkInfo->strings[i], links.take());

		if (result != TEResultSuccess) {
			LogLinkSkip(groupLinkInfo->strings[i], "failed to read output group children");
			continue;
		}

		for (int j = 0; j < links->count; j++) {
			TouchObject<TELinkInfo> linkInfo;
			result = TEInstanceLinkGetInfo(instance, links->strings[j], linkInfo.take());

			if (result != TEResultSuccess) {
				LogLinkSkip(links->strings[j], "failed to read output link info");
				continue;
			}

			if (linkInfo->domain == TELinkDomainOperator) {
				if (strcmp(linkInfo->name, "out1") == 0 && linkInfo->type == TELinkTypeTexture) {
					OutputOpName = linkInfo->identifier;
					hasVideoOutput = true;
					break;
				} else if (linkInfo->type == TELinkTypeTexture) {
					OutputOpName = linkInfo->identifier;
					hasVideoOutput = true;
					break;
				}
			}
		}

	}

	// Re-raise value events now that the walk is complete. Enumeration runs on
	// the TE callback thread while the host UI polls in parallel, so the host
	// can consume a slot's event and cache a stale display string before that
	// slot's registration finished; a final sweep forces a fresh query of every
	// active parameter.
	for (auto& ParamID : ActiveParams) {
		RaiseParamEvent(ParamID, FF_EVENT_FLAG_VALUE);
	}
}

void FFGLTouchEnginePluginBase::CreateIndividualParameter(const TouchObject<TELinkInfo>& linkInfo) {

	// Robustness contract for this whole function: every early-out must SKIP only
	// the current link (log + return), never leave a half-registered parameter
	// behind. To guarantee that, each branch reads all TE values FIRST and only
	// registers the parameter (Parameters / ActiveParams / ParameterMap*) once the
	// reads have succeeded. A read failure therefore leaves no dangling slot for
	// PushParametersToTouchEngine to trip over.
	switch (linkInfo->type) {
	case TELinkTypeTexture:
	case TELinkTypeGroup:
	case TELinkTypeSeparator:
	{
		// Not user-facing scalar parameters — nothing to enumerate here.
		return;
	}

	case TELinkTypeDouble:
	{
		TEResult result;
		if (linkInfo->intent == TELinkIntentColorRGBA || linkInfo->intent == TELinkIntentPositionXYZW || linkInfo->intent == TELinkIntentSizeWH) {
			std::string Suffix;
			switch (linkInfo->intent) {
			case TELinkIntentColorRGBA:
				Suffix = "RGBA";
				break;
			case TELinkIntentPositionXYZW:
				Suffix = "XYZW";
				break;
			case TELinkIntentSizeWH:
				Suffix = "WH";
				break;
			}

			double value[4] = { 0, 0, 0, 0 };
			result = TEInstanceLinkGetDoubleValue(instance, linkInfo->identifier, TELinkValueCurrent, value, linkInfo->count);
			if (result != TEResultSuccess) {
				LogLinkSkip(linkInfo->identifier, "failed to read vector value");
				return;
			}

			double max[4] = { 0, 0, 0, 0 };
			result = TEInstanceLinkGetDoubleValue(instance, linkInfo->identifier, TELinkValueUIMaximum, max, linkInfo->count);
			if (result != TEResultSuccess) {
				LogLinkSkip(linkInfo->identifier, "failed to read vector max");
				return;
			}
			double min[4] = { 0, 0, 0, 0 };
			result = TEInstanceLinkGetDoubleValue(instance, linkInfo->identifier, TELinkValueUIMinimum, min, linkInfo->count);
			if (result != TEResultSuccess) {
				LogLinkSkip(linkInfo->identifier, "failed to read vector min");
				return;
			}

			VectorParameterInfo info;
			info.count = linkInfo->count;
			info.identifier = linkInfo->identifier;



			static const FFUInt32 colorTypes[] = { FF_TYPE_RED, FF_TYPE_GREEN, FF_TYPE_BLUE, FF_TYPE_ALPHA };

			// Align to start of a group of 4 so R/G/B/A land on the correct pre-allocated types
			uint32_t colorGroupBase = (ColorParamCount / 4) * 4;
			if (ColorParamCount % 4 != 0) {
				colorGroupBase = ColorParamCount; // already mid-group shouldn't happen, but advance
			}

			for (uint32_t i = 0; i < linkInfo->count; i++) {
				uint32_t ParamID;

				if (linkInfo->intent == TELinkIntentColorRGBA && i < 4) {
					// Use pre-allocated color picker slots (aligned to groups of 4)
					ParamID = (MaxParamsByType * 6) + OffsetParamsByType + colorGroupBase + i;
					ParameterMapType[ParamID] = colorTypes[i];
				} else {
					ParamID = Parameters.size() + OffsetParamsByType;
					ParameterMapType[ParamID] = FF_TYPE_STANDARD;
				}

				Parameters.push_back(std::make_pair(std::string(linkInfo->identifier) + (char)0x03 + std::to_string(i), ParamID));
				ActiveParams.insert(ParamID);
				ActiveVectorParams.insert(ParamID);
				info.children[i] = ParamID;

				ParameterMapFloat[ParamID] = value[i];
				SetParamDisplayName(ParamID, linkInfo->label + std::string(".") + Suffix[i], true);
				SetParamRange(ParamID, min[i], max[i]);
				RaiseParamEvent(ParamID, FF_EVENT_FLAG_VALUE);
				SetParamVisibility(ParamID, true, true);

			}

			// Advance color counter by a full group of 4 to keep alignment
			if (linkInfo->intent == TELinkIntentColorRGBA) {
				ColorParamCount = colorGroupBase + 4;
			}

			VectorParameters.push_back(info);

			return;
		}

		// Scalar double — read value + range before registering.
		double value = 0;
		result = TEInstanceLinkGetDoubleValue(instance, linkInfo->identifier, TELinkValueCurrent, &value, 1);
		if (result != TEResultSuccess) {
			LogLinkSkip(linkInfo->identifier, "failed to read double value");
			return;
		}
		double max = 0;
		result = TEInstanceLinkGetDoubleValue(instance, linkInfo->identifier, TELinkValueUIMaximum, &max, 1);
		if (result != TEResultSuccess) {
			LogLinkSkip(linkInfo->identifier, "failed to read double max");
			return;
		}
		double min = 0;
		result = TEInstanceLinkGetDoubleValue(instance, linkInfo->identifier, TELinkValueUIMinimum, &min, 1);
		if (result != TEResultSuccess) {
			LogLinkSkip(linkInfo->identifier, "failed to read double min");
			return;
		}

		uint32_t ParamID = Parameters.size() + OffsetParamsByType;
		Parameters.push_back(std::make_pair(linkInfo->identifier, ParamID));
		ActiveParams.insert(ParamID);
		ParameterMapType[ParamID] = FF_TYPE_STANDARD;
		ParameterMapFloat[ParamID] = value;
		SetParamDisplayName(ParamID, linkInfo->label, true);
		SetParamRange(ParamID, min, max);
		RaiseParamEvent(ParamID, FF_EVENT_FLAG_VALUE);
		SetParamVisibility(ParamID, true, true);

		break;

	}
	case TELinkTypeInt:
	{
		TEResult result;
		if (TEInstanceLinkHasChoices(instance, linkInfo->identifier)) {
			TouchObject<TEStringArray> labels;
			result = TEInstanceLinkGetChoiceLabels(instance, linkInfo->identifier, labels.take());
			if (result != TEResultSuccess && !labels) {
				LogLinkSkip(linkInfo->identifier, "failed to read choice labels");
				return;
			}

			int32_t value = 0;
			result = TEInstanceLinkGetIntValue(instance, linkInfo->identifier, TELinkValueCurrent, &value, 1);
			if (result != TEResultSuccess) {
				LogLinkSkip(linkInfo->identifier, "failed to read menu value");
				return;
			}

			// NOTE: upstream ParamID arithmetic, kept as-is on this branch. It
			// derives the menu slot from ParameterMapInt.size() while the menu
			// branch never writes that map, so a second menu collapses onto the
			// first. Fixing it requires the per-family counters, which are a
			// breaking naming change — see the feat/slot-naming-ranges branch.
			uint32_t ParamID = (ParameterMapInt.size() + OffsetParamsByType) + (MaxParamsByType * 5);

			std::vector<std::string> labelsVector;
			std::vector<float> valuesVector;

			for (int k = 0; k < labels->count; k++) {
				labelsVector.push_back(labels->strings[k]);
				valuesVector.push_back(static_cast<float>(k));
			}

			SetParamElements(ParamID, labelsVector, valuesVector, true);

			Parameters.push_back(std::make_pair(linkInfo->identifier, ParamID));
			ActiveParams.insert(ParamID);
			SetParamDisplayName(ParamID, linkInfo->label, true);
			ParameterMapType[ParamID] = FF_TYPE_OPTION;

			RaiseParamEvent(ParamID, FF_EVENT_FLAG_VALUE);
			SetParamVisibility(ParamID, true, true);
			break;
		} else {
			int32_t value = 0;
			result = TEInstanceLinkGetIntValue(instance, linkInfo->identifier, TELinkValueCurrent, &value, 1);
			if (result != TEResultSuccess) {
				LogLinkSkip(linkInfo->identifier, "failed to read int value");
				return;
			}
			int32_t max = 0;
			result = TEInstanceLinkGetIntValue(instance, linkInfo->identifier, TELinkValueUIMaximum, &max, 1);
			if (result != TEResultSuccess) {
				LogLinkSkip(linkInfo->identifier, "failed to read int max");
				return;
			}
			int32_t min = 0;
			result = TEInstanceLinkGetIntValue(instance, linkInfo->identifier, TELinkValueUIMinimum, &min, 1);
			if (result != TEResultSuccess) {
				LogLinkSkip(linkInfo->identifier, "failed to read int min");
				return;
			}

			uint32_t ParamID = (ParameterMapInt.size() + OffsetParamsByType) + MaxParamsByType;
			Parameters.push_back(std::make_pair(linkInfo->identifier, ParamID));
			ActiveParams.insert(ParamID);
			ParameterMapType[ParamID] = FF_TYPE_INTEGER;
			SetParamDisplayName(ParamID, linkInfo->label, true);
			ParameterMapInt[ParamID] = value;
			SetParamRange(ParamID, static_cast<float>(min), static_cast<float>(max));
			RaiseParamEvent(ParamID, FF_EVENT_FLAG_VALUE);
			SetParamVisibility(ParamID, true, true);

			break;
		}
	}
	case TELinkTypeBoolean:
	{
		TEResult result;
		if (linkInfo->intent == TELinkIntentMomentary || linkInfo->intent == TELinkIntentPulse) {
			bool value = false;
			result = TEInstanceLinkGetBooleanValue(instance, linkInfo->identifier, TELinkValueCurrent, &value);
			if (result != TEResultSuccess) {
				LogLinkSkip(linkInfo->identifier, "failed to read event value");
				return;
			}

			uint32_t ParamID = (ParameterMapBool.size() + OffsetParamsByType) + (MaxParamsByType * 4);
			Parameters.push_back(std::make_pair(linkInfo->identifier, ParamID));
			ActiveParams.insert(ParamID);
			ParameterMapType[ParamID] = FF_TYPE_EVENT;

			if (linkInfo->intent == TELinkIntentPulse) {
				PulseParameters.insert(ParamID);
			}

			SetParamDisplayName(ParamID, linkInfo->label, true);
			ParameterMapBool[ParamID] = value;
			RaiseParamEvent(ParamID, FF_EVENT_FLAG_VALUE);
			SetParamVisibility(ParamID, true, true);
		} else {
			bool value = false;
			result = TEInstanceLinkGetBooleanValue(instance, linkInfo->identifier, TELinkValueCurrent, &value);
			if (result != TEResultSuccess) {
				LogLinkSkip(linkInfo->identifier, "failed to read boolean value");
				return;
			}

			uint32_t ParamID = (ParameterMapBool.size() + OffsetParamsByType) + MaxParamsByType * 2;
			Parameters.push_back(std::make_pair(linkInfo->identifier, ParamID));
			ActiveParams.insert(ParamID);
			ParameterMapType[ParamID] = FF_TYPE_BOOLEAN;
			SetParamDisplayName(ParamID, linkInfo->label, true);
			ParameterMapBool[ParamID] = value;
			RaiseParamEvent(ParamID, FF_EVENT_FLAG_VALUE);
			SetParamVisibility(ParamID, true, true);
		}

		break;
	}
	case TELinkTypeString:
	{
		TouchObject<TEString> value;
		TEResult result = TEInstanceLinkGetStringValue(instance, linkInfo->identifier, TELinkValueCurrent, value.take());
		if (result != TEResultSuccess) {
			LogLinkSkip(linkInfo->identifier, "failed to read string value");
			return;
		}

		uint32_t ParamID = (ParameterMapString.size() + OffsetParamsByType) + MaxParamsByType * 3;
		Parameters.push_back(std::make_pair(linkInfo->identifier, ParamID));
		ActiveParams.insert(ParamID);
		ParameterMapType[ParamID] = FF_TYPE_TEXT;
		SetParamDisplayName(ParamID, linkInfo->label, true);
		ParameterMapString[ParamID] = value->string;
		RaiseParamEvent(ParamID, FF_EVENT_FLAG_VALUE);
		SetParamVisibility(ParamID, true, true);

		break;
	}
	default:
	{
		// Unknown / future TELinkType (e.g. par types added in newer TouchDesigner
		// builds). Ignore it gracefully instead of silently aborting the walk, and
		// log so the gap is visible rather than surfacing as "params just missing".
		std::string msg = std::string("FFGLTouchEngine: unsupported link type ") +
			std::to_string(static_cast<int>(linkInfo->type)) + " for '" +
			(linkInfo->identifier ? linkInfo->identifier : "?") + "' — skipping";
		FFGLLog::LogToHost(msg.c_str());
		return;
	}
	}

}

void FFGLTouchEnginePluginBase::CreateParametersFromGroup(const TouchObject<TELinkInfo>& linkInfo) {

	TouchObject<TEStringArray> links;
	TEResult result = TEInstanceLinkGetChildren(instance, linkInfo->identifier, links.take());

	if (result != TEResultSuccess) {
		LogLinkSkip(linkInfo->identifier, "failed to read group children");
		return;
	}

	for (int j = 0; j < links->count; j++) {
		TouchObject<TELinkInfo> childLinkInfo;
		result = TEInstanceLinkGetInfo(instance, links->strings[j], childLinkInfo.take());

		if (result != TEResultSuccess) {
			// Skip this child but keep walking the rest of the group.
			LogLinkSkip(links->strings[j], "failed to read group child link info");
			continue;
		}

		CreateIndividualParameter(childLinkInfo);
	}



}

FFResult FFGLTouchEnginePluginBase::PushParametersToTouchEngine()
{
	std::lock_guard<std::recursive_mutex> lock(TEStateMutex);
	if (instance == nullptr) {
		return FF_SUCCESS;
	}

	for (auto& param : Parameters) {
		FFUInt32 type = ParameterMapType[param.second];

		if (ActiveVectorParams.find(param.second) != ActiveVectorParams.end()) {
			continue;
		}

		if (type == FF_TYPE_STANDARD) {
			TEResult result = TEInstanceLinkSetDoubleValue(instance, param.first.c_str(), &ParameterMapFloat[param.second], 1);
			if (result != TEResultSuccess) {
				isTouchFrameBusy = false;
				return FailAndLog("Failed to set double value");
			}
		}

		if (type == FF_TYPE_INTEGER || type == FF_TYPE_OPTION) {
			TEResult result = TEInstanceLinkSetIntValue(instance, param.first.c_str(), &ParameterMapInt[param.second], 1);
			if (result != TEResultSuccess) {
				isTouchFrameBusy = false;
				return FailAndLog("Failed to set int value");
			}
		}


		if (type == FF_TYPE_BOOLEAN || type == FF_TYPE_EVENT) {
			TEResult result = TEInstanceLinkSetBooleanValue(instance, param.first.c_str(), ParameterMapBool[param.second]);
			if (result != TEResultSuccess) {
				isTouchFrameBusy = false;
				return FailAndLog("Failed to set boolean value");
			}
			// Auto-reset pulse parameters to false after sending
			if (ParameterMapBool[param.second] && PulseParameters.find(param.second) != PulseParameters.end()) {
				ParameterMapBool[param.second] = false;
			}
		}

		if (type == FF_TYPE_TEXT) {
			TEResult result = TEInstanceLinkSetStringValue(instance, param.first.c_str(), ParameterMapString[param.second].c_str());
			if (result != TEResultSuccess) {
				isTouchFrameBusy = false;
				return FailAndLog("Failed to set string value");
			}
		}

	}

	for (auto& param : VectorParameters) {
		double values[4] = { 0,0,0,0 };

		for (uint8_t i = 0; i < param.count; i++) {
			values[i] = ParameterMapFloat[param.children[i]];
		}

		TEResult result = TEInstanceLinkSetDoubleValue(instance, param.identifier.c_str(), values, param.count);
		if (result != TEResultSuccess) {
			isTouchFrameBusy = false;
			return FailAndLog("Failed to set vector value");
		}

	}

	return FF_SUCCESS;
}



void FFGLTouchEnginePluginBase::eventCallback(TEEvent event, TEResult result, int64_t start_time_value, int32_t start_time_scale, int64_t end_time_value, int32_t end_time_scale) {

	// Ignore callbacks during destruction to prevent pure virtual calls
	if (isBeingDestroyed) {
		return;
	}

	// Atomic-only fast path: never let frame completion wait on the state
	// mutex while the render thread holds it through a frame section.
	if (event == TEEventFrameDidFinish) {
		isTouchFrameBusy = false;
		return;
	}

	std::lock_guard<std::recursive_mutex> lock(TEStateMutex);

	if (result == TEResultComponentErrors) {
		TouchObject<TEErrorArray> errors;
		TEResult result = TEInstanceGetErrors(instance, errors.take());
		if (result != TEResultSuccess) {
			return;
		}

		if (!errors) {
			return;
		}

		for (int i = 0; i < errors->count; i++) {
			std::string error =
				"TouchEngine Error: Severity: " +
				GetSeverityString(errors->errors[i].severity) +
				", Location: " + errors->errors[i].location +
				", Description: " + errors->errors[i].description;

			FFGLLog::LogToHost(error.c_str());
		}

		// The TouchEngine has encountered an error
		// You can get the error message with TEInstanceGetError
	}

	if (result == TEResultNoKey || result == TEResultKeyError || result == TEResultExpiredKey) {
		FFGLLog::LogToHost("TouchEngine License Error");
		return;
	}
	switch (event) {
	case TEEventInstanceDidLoad:
		isLoadPending = false;
		if (isReloadQueued) {
			// A newer load request (tox path change or reload pulse) arrived
			// while this load was in flight — supersede it immediately instead
			// of resuming a stale comp.
			isReloadQueued = false;
			LoadTEFile();
			break;
		}
		if (result != TEResultSuccess) {
			// Surface load failures (bad tox, or a tox authored in a newer TD build
			// than this engine) instead of silently showing nothing. Do NOT resume
			// or enumerate a failed load — that used to register parameters against
			// a dead instance and push into it every frame.
			const char* desc = TEResultGetDescription(result);
			std::string msg = std::string("FFGLTouchEngine: instance failed to load — ") +
				(desc ? desc : "unknown error") +
				". Check the tox path and that the TouchEngine build is new enough for this tox.";
			FFGLLog::LogToHost(msg.c_str());
			break;
		}
		if (LoadTEGraphicsContext(false)) {
			isTouchEngineLoaded = true;
			ResumeTouchEngine();
			// Render-ready only now: comp loaded, resumed, links enumerated.
			isTouchEngineReady = true;
		} else {
			FFGLLog::LogToHost("Failed to load TE graphics context");
		}
		break;
	case TEEventInstanceReady:
		// NOT render-readiness. Per TEInstanceConfigure docs this event means
		// "configure completed, ready to load" — and during a reload the
		// previously loaded comp has just been UNLOADED at this point. Treating
		// it as render-ready resumed per-frame texture pushes into links that
		// no longer exist and segfaulted the host inside TE (the Reload crash).
		break;
	case TEEventInstanceDidUnload:
		isTouchEngineLoaded = false;
		break;
	}
}

void FFGLTouchEnginePluginBase::linkCallback(TELinkEvent event, const char* identifier) {
	if (isBeingDestroyed) {
		return;
	}
	switch (event) {
	case TELinkEventAdded:
		// A link has been added
		break;
	case TELinkEventRemoved:
	{
		// Safety net: if TE tears down a link we hold an identifier for (an
		// engine-side rebuild we didn't initiate), stop the render thread from
		// pushing into it. Readiness returns with the next enumeration
		// (TEEventInstanceDidLoad). Holding the last frame beats a segfault
		// inside TE's link table.
		if (!isTouchEngineReady || identifier == nullptr) {
			break;
		}
		std::lock_guard<std::recursive_mutex> lock(TEStateMutex);
		bool held = (InputOpName == identifier) || (OutputOpName == identifier);
		if (!held) {
			for (auto& param : Parameters) {
				if (param.first == identifier) { held = true; break; }
			}
		}
		if (held) {
			isTouchEngineReady = false;
			std::string msg = std::string("FFGLTouchEngine: link '") + identifier +
				"' removed by engine — pausing output until parameters are re-enumerated";
			FFGLLog::LogToHost(msg.c_str());
		}
		break;
	}
	case TELinkEventValueChange:
		break;
	default:
		break;
	}
}

void FFGLTouchEnginePluginBase::eventCallbackStatic(TEInstance* instance, TEEvent event, TEResult result, int64_t start_time_value, int32_t start_time_scale, int64_t end_time_value, int32_t end_time_scale, void* info) {
	static_cast<FFGLTouchEnginePluginBase*>(info)->eventCallback(event, result, start_time_value, start_time_scale, end_time_value, end_time_scale);
}

void FFGLTouchEnginePluginBase::linkCallbackStatic(TEInstance* instance, TELinkEvent event, const char* identifier, void* info) {
	static_cast<FFGLTouchEnginePluginBase*>(info)->linkCallback(event, identifier);
}

#ifdef __APPLE__
GLuint FFGLTouchEnginePluginBase::CreateOpenGLTextureFromIOSurface(IOSurfaceRef surface, int width, int height)
{
	GLuint texture = 0;
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_RECTANGLE, texture);

	CGLContextObj cglContext = CGLGetCurrentContext();
	CGLError err = CGLTexImageIOSurface2D(
		cglContext,
		GL_TEXTURE_RECTANGLE,
		GL_RGBA,
		width,
		height,
		GL_BGRA,
		GL_UNSIGNED_INT_8_8_8_8_REV,
		surface,
		0
	);

	if (err != kCGLNoError) {
		FFGLLog::LogToHost("Failed to bind IOSurface to OpenGL texture");
		glDeleteTextures(1, &texture);
		glBindTexture(GL_TEXTURE_RECTANGLE, 0);
		return 0;
	}

	glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_RECTANGLE, 0);

	return texture;
}

IOSurfaceRef FFGLTouchEnginePluginBase::CreateIOSurface(int width, int height)
{
	NSDictionary *properties = @{
		(NSString *)kIOSurfaceWidth: @(width),
		(NSString *)kIOSurfaceHeight: @(height),
		(NSString *)kIOSurfaceBytesPerElement: @(4),
		(NSString *)kIOSurfacePixelFormat: @((uint32_t)'BGRA'),
	};
	return IOSurfaceCreate((__bridge CFDictionaryRef)properties);
}

id<MTLTexture> FFGLTouchEnginePluginBase::CreateIOSurfaceBackedMetalTexture(int width, int height, IOSurfaceRef* outSurface)
{
	// Create the IOSurface
	IOSurfaceRef surface = CreateIOSurface(width, height);
	if (surface == nullptr) {
		FFGLLog::LogToHost("Failed to create IOSurface for Metal texture");
		return nil;
	}

	// Create a Metal texture descriptor matching the IOSurface
	MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
		width:width
		height:height
		mipmapped:NO];
	desc.storageMode = MTLStorageModeShared;
	desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;

	// Create Metal texture backed by the IOSurface
	id<MTLTexture> texture = [MetalDevice newTextureWithDescriptor:desc iosurface:surface plane:0];
	if (texture == nil) {
		FFGLLog::LogToHost("Failed to create IOSurface-backed Metal texture");
		CFRelease(surface);
		return nil;
	}

	*outSurface = surface;
	return texture;
}

void FFGLTouchEnginePluginBase::CopyMetalTexture(id<MTLTexture> src, id<MTLTexture> dst)
{
	id<MTLCommandBuffer> cmdBuf = [MetalCommandQueue commandBuffer];
	id<MTLBlitCommandEncoder> blit = [cmdBuf blitCommandEncoder];
	[blit copyFromTexture:src
		sourceSlice:0
		sourceLevel:0
		sourceOrigin:MTLOriginMake(0, 0, 0)
		sourceSize:MTLSizeMake(src.width, src.height, 1)
		toTexture:dst
		destinationSlice:0
		destinationLevel:0
		destinationOrigin:MTLOriginMake(0, 0, 0)];
	[blit endEncoding];
	[cmdBuf commit];
	[cmdBuf waitUntilCompleted];
}
#endif
