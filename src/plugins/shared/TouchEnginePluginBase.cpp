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
	return isGraphicsContextLoaded;
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
	Parameters.clear();
	return;
}

void FFGLTouchEnginePluginBase::CreateIndividualParameter(const TouchObject<TELinkInfo>& linkInfo) {

	switch (linkInfo->type) {
	case TELinkTypeTexture:
	case TELinkTypeGroup:
	case TELinkTypeSeparator:
	{
		return;
	}

	TEResult result;

	case TELinkTypeDouble:
	{
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
				return;
			}

			double max[4] = { 0, 0, 0, 0 };
			result = TEInstanceLinkGetDoubleValue(instance, linkInfo->identifier, TELinkValueUIMaximum, max, linkInfo->count);
			if (result != TEResultSuccess) {
				return;
			}
			double min[4] = { 0, 0, 0, 0 };
			result = TEInstanceLinkGetDoubleValue(instance, linkInfo->identifier, TELinkValueUIMinimum, min, linkInfo->count);
			if (result != TEResultSuccess) {
				return;
			}

			VectorParameterInfo info;
			info.count = linkInfo->count;
			info.identifier = linkInfo->identifier;



			for (uint32_t i = 0; i < linkInfo->count; i++) {
				uint32_t ParamID = Parameters.size() + OffsetParamsByType;
				Parameters.push_back(std::make_pair(std::string(linkInfo->identifier) + (char)0x03 + std::to_string(i), ParamID));
				ActiveParams.insert(ParamID);
				ActiveVectorParams.insert(ParamID);
				info.children[i] = ParamID;

				ParameterMapType[ParamID] = FF_TYPE_STANDARD;

				SetParamDisplayName(ParamID, linkInfo->label + std::string(".") + Suffix[i], true);

				SetParamRange(ParamID, min[i], max[i]);
				RaiseParamEvent(ParamID, FF_EVENT_FLAG_VALUE);
				SetParamVisibility(ParamID, true, true);

			}

			VectorParameters.push_back(info);

			return;
		}
		uint32_t ParamID = Parameters.size() + OffsetParamsByType;
		Parameters.push_back(std::make_pair(linkInfo->identifier, ParamID));
		ActiveParams.insert(ParamID);
		ParameterMapType[ParamID] = FF_TYPE_STANDARD;

		//SetParamInfof(Parameters[j].second, linkInfo->name, FF_TYPE_STANDARD);

		SetParamDisplayName(ParamID, linkInfo->label, true);

		double value = 0;
		result = TEInstanceLinkGetDoubleValue(instance, linkInfo->identifier, TELinkValueCurrent, &value, 1);
		if (result != TEResultSuccess) {
			return;
		}
		//SetParamInfo(Parameters[j].second, linkInfo->name, FF_TYPE_STANDARD, static_cast<float>(value));
		ParameterMapFloat[ParamID] = value;

		double max = 0;
		result = TEInstanceLinkGetDoubleValue(instance, linkInfo->identifier, TELinkValueUIMaximum, &max, 1);
		if (result != TEResultSuccess) {
			return;
		}
		double min = 0;
		result = TEInstanceLinkGetDoubleValue(instance, linkInfo->identifier, TELinkValueUIMinimum, &min, 1);
		if (result != TEResultSuccess) {
			return;
		}

		SetParamRange(ParamID, min, max);
		RaiseParamEvent(ParamID, FF_EVENT_FLAG_VALUE);
		SetParamVisibility(ParamID, true, true);

		break;

	}
	case TELinkTypeInt:
	{
		if (TEInstanceLinkHasChoices(instance, linkInfo->identifier)) {
			TouchObject<TEStringArray> labels;
			uint32_t ParamID = (ParameterMapInt.size() + OffsetParamsByType) + (MaxParamsByType * 5);
			result = TEInstanceLinkGetChoiceLabels(instance, linkInfo->identifier, labels.take());
			if (result != TEResultSuccess && !labels) {
				return;
			}

			std::vector<std::string> labelsVector;
			std::vector<float> valuesVector;

			for (int k = 0; k < labels->count; k++) {
				labelsVector.push_back(labels->strings[k]);
				valuesVector.push_back(static_cast<float>(k));
			}

			SetParamElements(ParamID, labelsVector, valuesVector, true);

			int32_t value = 0;
			result = TEInstanceLinkGetIntValue(instance, linkInfo->identifier, TELinkValueCurrent, &value, 1);
			if (result != TEResultSuccess) {
				return;
			}

			Parameters.push_back(std::make_pair(linkInfo->identifier, ParamID));
			ActiveParams.insert(ParamID);
			SetParamDisplayName(ParamID, linkInfo->label, true);
			ParameterMapType[ParamID] = FF_TYPE_OPTION;

			RaiseParamEvent(ParamID, FF_EVENT_FLAG_VALUE);
			SetParamVisibility(ParamID, true, true);
			break;
		} else {
			uint32_t ParamID = (ParameterMapInt.size() + OffsetParamsByType) + MaxParamsByType;
			Parameters.push_back(std::make_pair(linkInfo->identifier, ParamID));
			ActiveParams.insert(ParamID);
			ParameterMapType[ParamID] = FF_TYPE_INTEGER;

			int32_t value = 0;
			result = TEInstanceLinkGetIntValue(instance, linkInfo->identifier, TELinkValueCurrent, &value, 1);
			if (result != TEResultSuccess) {
				return;
			}
			SetParamDisplayName(ParamID, linkInfo->label, true);
			ParameterMapInt[ParamID] = value;


			int32_t max = 0;
			result = TEInstanceLinkGetIntValue(instance, linkInfo->identifier, TELinkValueUIMaximum, &max, 1);
			if (result != TEResultSuccess) {
				return;
			}

			int32_t min = 0;
			result = TEInstanceLinkGetIntValue(instance, linkInfo->identifier, TELinkValueUIMinimum, &min, 1);
			if (result != TEResultSuccess) {
				return;
			}

			SetParamRange(ParamID, static_cast<float>(min), static_cast<float>(max));
			RaiseParamEvent(ParamID, FF_EVENT_FLAG_VALUE);
			SetParamVisibility(ParamID, true, true);

			break;
		}
	}
	case TELinkTypeBoolean:
	{

		if (linkInfo->intent == TELinkIntentMomentary || linkInfo->intent == TELinkIntentPulse) {
			uint32_t ParamID = (ParameterMapBool.size() + OffsetParamsByType) + (MaxParamsByType * 4);
			Parameters.push_back(std::make_pair(linkInfo->identifier, ParamID));
			ActiveParams.insert(ParamID);
			ParameterMapType[ParamID] = FF_TYPE_EVENT;

			//SetParamInfof(Parameters[j].second, linkInfo->name, FF_TYPE_EVENT);
			bool value = false;
			result = TEInstanceLinkGetBooleanValue(instance, linkInfo->identifier, TELinkValueCurrent, &value);
			if (result != TEResultSuccess) {
				return;
			}

			SetParamDisplayName(ParamID, linkInfo->label, true);
			ParameterMapBool[ParamID] = value;
			RaiseParamEvent(ParamID, FF_EVENT_FLAG_VALUE);
			SetParamVisibility(ParamID, true, true);
		} else {
			//SetParamInfof(Parameters[j].second, linkInfo->name, FF_TYPE_BOOLEAN);
			uint32_t ParamID = (ParameterMapBool.size() + OffsetParamsByType) + MaxParamsByType * 2;
			Parameters.push_back(std::make_pair(linkInfo->identifier, ParamID));
			ActiveParams.insert(ParamID);
			ParameterMapType[ParamID] = FF_TYPE_BOOLEAN;

			bool value = false;
			result = TEInstanceLinkGetBooleanValue(instance, linkInfo->identifier, TELinkValueCurrent, &value);
			if (result != TEResultSuccess) {
				return;
			}

			//SetParamInfo(Parameters[j].second, linkInfo->name, FF_TYPE_BOOLEAN, value);
			SetParamDisplayName(ParamID, linkInfo->label, true);
			ParameterMapBool[ParamID] = value;
			RaiseParamEvent(ParamID, FF_EVENT_FLAG_VALUE);
			SetParamVisibility(ParamID, true, true);
		}

		break;
	}
	case TELinkTypeString:
	{
		uint32_t ParamID = (ParameterMapString.size() + OffsetParamsByType) + MaxParamsByType * 3; //(MaxParamsByType * 3) + OffsetParamsByType
		Parameters.push_back(std::make_pair(linkInfo->identifier, ParamID));
		ActiveParams.insert(ParamID);
		ParameterMapType[ParamID] = FF_TYPE_TEXT;

		TouchObject<TEString> value;
		result = TEInstanceLinkGetStringValue(instance, linkInfo->identifier, TELinkValueCurrent, value.take());
		if (result != TEResultSuccess) {
			return;
		}
		SetParamDisplayName(ParamID, linkInfo->label, true);
		ParameterMapString[ParamID] = value->string;
		RaiseParamEvent(ParamID, FF_EVENT_FLAG_VALUE);
		SetParamVisibility(ParamID, true, true);
		//SetParamInfo(Parameters[j].second, linkInfo->name, FF_TYPE_TEXT, value);



		break;
	}
	}

}

void FFGLTouchEnginePluginBase::CreateParametersFromGroup(const TouchObject<TELinkInfo>& linkInfo) {

	TouchObject<TEStringArray> links;
	TEResult result = TEInstanceLinkGetChildren(instance, linkInfo->identifier, links.take());

	if (result != TEResultSuccess) {
		return;
	}

	for (int j = 0; j < links->count; j++) {
		TouchObject<TELinkInfo> linkInfo;
		result = TEInstanceLinkGetInfo(instance, links->strings[j], linkInfo.take());

		if (result != TEResultSuccess) {
			continue;
			FFGLLog::LogToHost("Failed to get link info");

		}

		CreateIndividualParameter(linkInfo);
	}



}


void FFGLTouchEnginePluginBase::eventCallback(TEEvent event, TEResult result, int64_t start_time_value, int32_t start_time_scale, int64_t end_time_value, int32_t end_time_scale) {


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
		if (LoadTEGraphicsContext(false)) {
			isTouchEngineLoaded = true;
			ResumeTouchEngine();
		}
		// The tox file has been loaded into the TouchEngine
		break;
	case TEEventFrameDidFinish:
		isTouchFrameBusy = false;
		// A frame has finished rendering
		break;
	case TEEventInstanceReady:
		FFGLLog::LogToHost("TouchEngine Ready");
		isTouchEngineReady = true;
		// The TouchEngine is ready to start rendering frames
		break;
	case TEEventInstanceDidUnload:
		FFGLLog::LogToHost("TouchEngine Unloaded");
		isTouchEngineLoaded = false;
		break;
	}
}

void FFGLTouchEnginePluginBase::linkCallback(TELinkEvent event, const char* identifier) {
	switch (event) {
	case TELinkEventAdded:
		// A link has been added
		break;
	case TELinkEventValueChange:
		break;
	}
}

void FFGLTouchEnginePluginBase::eventCallbackStatic(TEInstance* instance, TEEvent event, TEResult result, int64_t start_time_value, int32_t start_time_scale, int64_t end_time_value, int32_t end_time_scale, void* info) {
	static_cast<FFGLTouchEnginePluginBase*>(info)->eventCallback(event, result, start_time_value, start_time_scale, end_time_value, end_time_scale);
}

void FFGLTouchEnginePluginBase::linkCallbackStatic(TEInstance* instance, TELinkEvent event, const char* identifier, void* info) {
	static_cast<FFGLTouchEnginePluginBase*>(info)->linkCallback(event, identifier);
}
