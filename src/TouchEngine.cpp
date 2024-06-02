#include "TouchEngine.h"

static CFFGLPluginInfo PluginInfo(
	PluginFactory< FFGLTouchEngine >,// Create method
	"TE01",                        // Plugin unique ID
	"TouchEngine",            // Plugin name
	2,                             // API major version number
	1,                             // API minor version number
	1,                             // Plugin major version number
	000,                           // Plugin minor version number
	FF_SOURCE,                     // Plugin type
	"Loads tox files from TouchDesigner",// Plugin description
	"TouchEngine Loader made by Evan Clark"        // About
);


std::string GetSeverityString(TESeverity severity) {
	switch (severity)
	{
	case TESeverityWarning:
		return "Warning";
	case TESeverityError:
		return "Error";
	default:
		return "Unknown";
	}

}

std::string GenerateRandomString(size_t length)
{
	auto randchar = []() -> char
		{
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

FFGLTouchEngine::FFGLTouchEngine()
	: CFFGLPlugin()
{
	SpoutID = GenerateRandomString(10);
	// Input properties
	SetMinInputs(0);
	SetMaxInputs(0);

	// Parameters
	SetParamInfof(0, "Tox File", FF_TYPE_EVENT);

	/*
	SetParamInfo(0, "RGBA1 Red", FF_TYPE_STANDARD, 1.0f);
	SetParamInfo(1, "RGBA1 Green", FF_TYPE_STANDARD, 1.0f);
	SetParamInfo(2, "RGBA1 Blue", FF_TYPE_STANDARD, 0.0f);
	SetParamInfo(3, "RGBA1 Alpha", FF_TYPE_STANDARD, 1.0f);

	SetParamInfo(4, "HSBA2 Hue", FF_TYPE_STANDARD, 0.0f);
	SetParamInfo(5, "HSBA2 Saturation", FF_TYPE_STANDARD, 1.0f);
	SetParamInfo(6, "HSBA2 Brightness", FF_TYPE_STANDARD, 1.0f);
	SetParamInfo(7, "HSBA2 Alpha", FF_TYPE_STANDARD, 1.0f);
	*/
}

FFResult FFGLTouchEngine::InitGL(const FFGLViewportStruct* vp)
{

	Width = vp->width;
	Height = vp->height;
	

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

	if (FAILED(hr))
	{
		FFGLLog::LogToHost("Failed to create D3D11 device");
		return FF_FAIL;
	}


	SPReceiver.SetActiveSender(SpoutID.c_str());
	SPSender.SetActiveSender(SpoutID.c_str());

	SPSender.SetAdapterAuto(true);



	// Load the TouchEngine graphics context
	if (!LoadTEGraphicsContext(false)) {
		return FF_FAIL;
	}

	// Create the input texture

	// Set the viewport size

	Width = vp->width;
	Height = vp->height;

	if (!CreateInputTexture(Width, Height)) {
		return FF_FAIL;
	}

	if (FilePath.empty())
	{
		return FF_SUCCESS;
	}

	bool Status = LoadTEFile();

	if (!Status)
	{
		FFGLLog::LogToHost("Failed to load TE file");
		return FF_FAIL;
	}

	return FF_SUCCESS;
}

FFResult FFGLTouchEngine::ProcessOpenGL(ProcessOpenGLStruct* pGL)
{
	if (pGL->numInputTextures < 1)
		return FF_FAIL;

	if (!isTouchEngineLoaded || !isTouchEngineReady || isTouchFrameBusy)
	{
		return FF_FAIL;
	}

	isTouchFrameBusy = true;

	FFGLTextureStruct &Texture = *(pGL->inputTextures[0]);

	for (auto& param : ParameterMapFloat)
	{
		TEResult result = TEInstanceLinkSetDoubleValue(instance, Parameters[param.first - 1].first.c_str(), &param.second, 1);
		if (result != TEResultSuccess)
		{
			isTouchFrameBusy = false;
			return FF_FAIL;
		}
	}
	//Need to handle bools
	for (auto& param : ParameterMapInt)
	{
		TEResult result = TEInstanceLinkSetIntValue(instance, Parameters[param.first - 1].first.c_str(), &param.second, 1);
		if (result != TEResultSuccess)
		{
			isTouchFrameBusy = false;
			return FF_FAIL;
		}
	}

	for (auto& param : ParameterMapBool)
	{
		TEResult result = TEInstanceLinkSetBooleanValue(instance, Parameters[param.first - 1].first.c_str(), param.second);
		if (result != TEResultSuccess)
		{
			isTouchFrameBusy = false;
			return FF_FAIL;
		}
	}


	TEResult result = TEInstanceStartFrameAtTime(instance, FrameCount, 60, false);
	if (result != TEResultSuccess)
	{
		isTouchFrameBusy = false;
		return FF_FAIL;
	}
	FrameCount++;

	while (isTouchFrameBusy)
	{

	}

	if (hasVideoOutput) {
		TouchObject<TETexture> TETextureToSend;
		//Will need to replace the below value with something more standard
		result = TEInstanceLinkGetTextureValue(instance, "op/vdjtextureout", TELinkValueCurrent, TETextureToSend.take());
		if (result == TEResultSuccess && TETextureToSend != nullptr) {
			if (TETextureGetType(TETextureToSend) == TETextureTypeD3DShared && result == TEResultSuccess) {
				TouchObject<TED3D11Texture> D3DTextureToSend;
				result = TED3D11ContextGetTexture(D3DContext, static_cast<TED3DSharedTexture*>(TETextureToSend.get()), D3DTextureToSend.take());
				if (result != TEResultSuccess)
				{
					return FF_FALSE;
				}
				Microsoft::WRL::ComPtr<ID3D11Texture2D> RawTextureToSend = TED3D11TextureGetTexture(D3DTextureToSend);

				if (RawTextureToSend == nullptr) {
					return FF_FALSE;
				}
				Microsoft::WRL::ComPtr <IDXGIKeyedMutex> keyedMutex;
				RawTextureToSend->QueryInterface<IDXGIKeyedMutex>(&keyedMutex);

				TouchObject<TESemaphore> semaphore;
				uint64_t waitValue = 0;
				result = TEInstanceGetTextureTransfer(instance, TETextureToSend, semaphore.take(), &waitValue);
				if (result != TEResultSuccess)
				{
					return FF_FALSE;
				}
				keyedMutex->AcquireSync(waitValue, INFINITE);
			
				SPSender.SendTexture(RawTextureToSend.Get());

				keyedMutex->ReleaseSync(waitValue + 1);

			}
		
		}
		//Bind and receive the texture
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, Texture.Handle);
		SPReceiver.ReceiveTexture(Texture.Handle, Width, Height);
	
	}
	
	// Render the quad
	quad.Draw();

	// Unbind the input texture
	glBindTexture(GL_TEXTURE_2D, 0);


	return FF_SUCCESS;
}


FFResult FFGLTouchEngine::DeInitGL()
{

	if (instance != nullptr)
	{
		TEInstanceSuspend(instance);
		TEInstanceUnload(instance);
		D3DContext.reset();
		D3DDevice->Release();
	}

	// Deinitialize the quad
	quad.Release();

	return FF_SUCCESS;
}


FFResult FFGLTouchEngine::SetFloatParameter(unsigned int dwIndex, float value) {
	switch (dwIndex) {
		case 0:
			// Open file dialog
			OpenFileDialog();
			LoadTEFile();
			return FF_SUCCESS;
	}

	ParameterMapFloat[Parameters[dwIndex - 1].second] = value;

	return FF_SUCCESS;
}

bool FFGLTouchEngine::LoadTEGraphicsContext(bool reload)
{
	// Load the TouchEngine graphics context

	if (instance == nullptr || reload) {
		TEResult result = TED3D11ContextCreate(D3DDevice.Get(), D3DContext.take());
		if (result != TEResultSuccess) {
			return false;
		}

		result = TEInstanceAssociateGraphicsContext(instance, D3DContext);
		if (result != TEResultSuccess)
		{
			return false;
		}
	}

	return true;
}

bool FFGLTouchEngine::CreateInputTexture(int width, int height) {
	// Create the input texture

	if (D3DTextureInput != nullptr)
	{
		D3DTextureInput->Release();
		D3DTextureInput = nullptr;
	}

	if (D3DTextureOutput != nullptr)
	{
		D3DTextureOutput->Release();
		D3DTextureOutput = nullptr;
	}

	D3D11_TEXTURE2D_DESC description = { 0 };
	description.Width = width;
	description.Height = height;
	description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	description.Usage = D3D11_USAGE_DEFAULT;
	description.CPUAccessFlags = 0;
	description.MiscFlags = 0;
	description.MipLevels = 0;
	description.ArraySize = 1;
	description.SampleDesc.Count = 1;
	description.SampleDesc.Quality = 0;
	description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

	HRESULT hr = D3DDevice->CreateTexture2D(&description, nullptr, &D3DTextureInput);
	if (FAILED(hr))
	{
		switch (hr) {
		case DXGI_ERROR_INVALID_CALL:
			//str += "DXGI_ERROR_INVALID_CALL";
			break;
		case E_INVALIDARG:
			//	str += "E_INVALIDARG";
			break;
		case E_OUTOFMEMORY:
			//	str += "E_OUTOFMEMORY";
			break;
		default:
			//	str += "Unlisted error";
			break;
		}
		FFGLLog::LogToHost("Failed to create texture input");
		return false;
	}

	hr = D3DDevice->CreateTexture2D(&description, nullptr, &D3DTextureOutput);

	if (FAILED(hr))
	{
		switch (hr) {
		case DXGI_ERROR_INVALID_CALL:
			//str += "DXGI_ERROR_INVALID_CALL";
			break;
		case E_INVALIDARG:
			//	str += "E_INVALIDARG";
			break;
		case E_OUTOFMEMORY:
			//	str += "E_OUTOFMEMORY";
			break;
		default:
			//	str += "Unlisted error";
			break;
		}
		FFGLLog::LogToHost("Failed to create texture output");
		return false;
	}


	return true;


}

void FFGLTouchEngine::GetAllParameters()
{
	TouchObject<TEStringArray> groupLinkInfo;

	if (instance == nullptr) {
		return;
	}

	TEResult result = TEInstanceGetLinkGroups(instance, TEScopeInput, groupLinkInfo.take());

	if (result != TEResultSuccess)
	{
		return;
	}

	for (int i = 0; i < groupLinkInfo->count; i++)
	{
		TouchObject<TEStringArray> links;
		result = TEInstanceLinkGetChildren(instance, groupLinkInfo->strings[i], links.take());

		if (result != TEResultSuccess)
		{
			return;
		}

		for (int j = 0; j < links->count; j++)
		{
			TouchObject<TELinkInfo> linkInfo;
			result = TEInstanceLinkGetInfo(instance, links->strings[j], linkInfo.take());

			if (result != TEResultSuccess)
			{
				continue;
			}


			if (linkInfo->domain == TELinkDomainParameter) {
				Parameters.push_back(std::make_pair(linkInfo->identifier, j + 1));


				switch (linkInfo->type)
				{
					case TELinkTypeTexture:
					{
						continue;
					}

					case TELinkTypeDouble:
					{
						SetParamInfof(Parameters[j].second, linkInfo->name, FF_TYPE_STANDARD);

						double value = 0;
						result = TEInstanceLinkGetDoubleValue(instance, linkInfo->identifier, TELinkValueCurrent, &value, 1);
						if (result != TEResultSuccess)
						{
							continue;
						}

						ParameterMapFloat[Parameters[j].second] = value;

						double max = 0;
						result = TEInstanceLinkGetDoubleValue(instance, linkInfo->identifier, TELinkValueMaximum, &max, 1);
						if (result != TEResultSuccess)
						{
							continue;
						}
						double min = 0;
						result = TEInstanceLinkGetDoubleValue(instance, linkInfo->identifier, TELinkValueMinimum, &min, 1);
						if (result != TEResultSuccess)
						{
							continue;
						}

						SetParamRange(Parameters[j].second, min, max);

						break;

					}
					case TELinkTypeInt:
					{

						SetParamInfof(Parameters[j].second, linkInfo->name, FF_TYPE_INTEGER);

						int32_t value = 0;
						result = TEInstanceLinkGetIntValue(instance, linkInfo->identifier, TELinkValueCurrent, &value, 1);
						if (result != TEResultSuccess)
						{
							continue;
						}

						ParameterMapInt[Parameters[j].second] = value;


						int32_t max = 0;
						result = TEInstanceLinkGetIntValue(instance, linkInfo->identifier, TELinkValueMaximum, &max, 1);
						if (result != TEResultSuccess)
						{
							continue;
						}

						int32_t min = 0;
						result = TEInstanceLinkGetIntValue(instance, linkInfo->identifier, TELinkValueMinimum, &min, 1);
						if (result != TEResultSuccess)
						{
							continue;
						}

						SetParamRange(Parameters[j].second, min, max);

						break;
					}
					case TELinkTypeBoolean:
					{

						if (linkInfo->intent == TELinkIntentMomentary) {
							SetParamInfof(Parameters[j].second, linkInfo->name, FF_TYPE_EVENT);
						}
						else
						{
							SetParamInfof(Parameters[j].second, linkInfo->name, FF_TYPE_BOOLEAN);

							bool value = false;
							result = TEInstanceLinkGetBooleanValue(instance, linkInfo->identifier, TELinkValueCurrent, &value);
							if (result != TEResultSuccess)
							{
								continue;
							}

							ParameterMapBool[Parameters[j].second] = value;
						}

						break;
					}
				}
			}
			else if (linkInfo->domain == TELinkDomainOperator) {
				if (strcmp(linkInfo->name, "vdjtexturein") == 0 && linkInfo->type == TELinkTypeTexture)
				{
					isVideoFX = true;
					hasVideoInput = true;
				}

				else if (strcmp(linkInfo->name, "vdjaudioin") == 0 && linkInfo->type == TELinkTypeFloatBuffer)
				{
					hasAudioInput = true;
				}

			}

		}
	}
}

bool FFGLTouchEngine::LoadTEFile()
{
	// Load the tox file into the TouchEngine
	// 1. Create a TouchEngine object


	if (instance == nullptr)
	{
		LoadTouchEngine();
		if (instance == nullptr)
		{
			FFGLLog::LogToHost("Failed to create TouchEngine instance");
			return false;
		}
	}

	// 2. Load the tox file into the TouchEngine
	TEResult result = TEInstanceConfigure(instance, FilePath.c_str(), TETimeExternal);
	if (result != TEResultSuccess)
	{
		return false;
	}

	result = TEInstanceSetFrameRate(instance, 60, 1);

	if (result != TEResultSuccess)
	{
		return false;
	}


	result = TEInstanceLoad(instance);
	if (result != TEResultSuccess)
	{
		return false;
	}


	return true;
}

bool FFGLTouchEngine::OpenFileDialog()
{
	IFileOpenDialog* pFileOpen;

	// Create the FileOpenDialog object.
	HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL,
		IID_IFileOpenDialog, reinterpret_cast<void**>(&pFileOpen));

	if (SUCCEEDED(hr)) {
		hr = pFileOpen->Show(nullptr);

		if (SUCCEEDED(hr)) {
			IShellItem* pItem;
			hr = pFileOpen->GetResult(&pItem);
			if (SUCCEEDED(hr)) {
				LPWSTR filePathW;
				hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &filePathW);
				if (SUCCEEDED(hr)) {
					std::wstring filePathWStr(filePathW);
					FilePath = std::string(filePathWStr.begin(), filePathWStr.end());
					std::string error = "TouchEngine Error: File Path: " + FilePath;
					FFGLLog::LogToHost(error.c_str());
					CoTaskMemFree(filePathW);
				}
				pItem->Release();
			}
		}
	}
	return true;
}

void FFGLTouchEngine::LoadTouchEngine() {

	if (instance == nullptr) {

		FFGLLog::LogToHost("Loading TouchEngine");
		TEResult result = TEInstanceCreate(eventCallbackStatic, linkCallbackStatic, this, instance.take());
		if (result != TEResultSuccess)
		{
			FFGLLog::LogToHost("Failed to create TouchEngine instance");
			instance.reset();
			return;
		}

	}

}

void FFGLTouchEngine::ResumeTouchEngine() {
	TEResult result = TEInstanceResume(instance);
	if (result != TEResultSuccess)
	{
		FFGLLog::LogToHost("Failed to resume TouchEngine instance");
	}

	if (TEVideoInputD3D == nullptr) {
		TEVideoInputD3D.take(TED3D11TextureCreate(D3DTextureInput.Get(), TETextureOriginTopLeft, kTETextureComponentMapIdentity, nullptr, nullptr));
	}

	//GetAllParameters();
	return;

}


void FFGLTouchEngine::eventCallback(TEEvent event, TEResult result, int64_t start_time_value, int32_t start_time_scale, int64_t end_time_value, int32_t end_time_scale)
{


	if (result == TEResultComponentErrors)
	{
		TouchObject<TEErrorArray> errors;
		TEResult result = TEInstanceGetErrors(instance, errors.take());
		if (result != TEResultSuccess)
		{
			return;
		}

		for (int i = 0; i < errors->count; i++)
		{
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
	switch (event) {
	case TEEventInstanceDidLoad:
		if (LoadTEGraphicsContext(false)) {
			isTouchEngineLoaded = true;
			ResumeTouchEngine();
		}
		// The tox file has been loaded into the TouchEngine
		break;
	case TEEventFrameDidFinish: {
		isTouchFrameBusy = false;
		// A frame has finished rendering
		break;
	}
	case TEEventInstanceReady:
		isTouchEngineReady = true;
		// The TouchEngine is ready to start rendering frames
		break;

	case TEEventInstanceDidUnload:
		isTouchEngineLoaded = false;
		break;
	}
}

void FFGLTouchEngine::linkCallback(TELinkEvent event, const char* identifier)
{
	switch (event) {
	case TELinkEventAdded:
		// A link has been added
		break;
	case TELinkEventValueChange:
		break;
	}
}

void FFGLTouchEngine::eventCallbackStatic(TEInstance* instance, TEEvent event, TEResult result, int64_t start_time_value, int32_t start_time_scale, int64_t end_time_value, int32_t end_time_scale, void* info)
{
	static_cast<FFGLTouchEngine*>(info)->eventCallback(event, result, start_time_value, start_time_scale, end_time_value, end_time_scale);
}

void FFGLTouchEngine::linkCallbackStatic(TEInstance* instance, TELinkEvent event, const char* identifier, void* info)
{
	static_cast<FFGLTouchEngine*>(info)->linkCallback(event, identifier);
}