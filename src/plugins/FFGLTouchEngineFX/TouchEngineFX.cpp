#include "TouchEngineFX.h"

static CFFGLPluginInfo PluginInfo(
	PluginFactory< FFGLTouchEngineFX >,// Create method
	"TEFX",                        // Plugin unique ID
	"TouchEngineFX",            // Plugin name
	2,                             // API major version number
	1,                             // API minor version number
	1,                             // Plugin major version number
	000,                           // Plugin minor version number
	FF_EFFECT,                     // Plugin type
	"Loads tox files from TouchDesigner",// Plugin description
	"TouchEngine Loader made by Evan Clark"        // About
);

static CFFGLThumbnailInfo ThumbnailInfo(160, 120, thumbnail);

static const char vertexShaderCode[] = R"(#version 410 core
uniform vec2 MaxUV;

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV * MaxUV;
}
)";

static const char fragmentShaderCode[] = R"(#version 410 core
uniform sampler2D InputTexture;

in vec2 uv;
out vec4 fragColor;

void main()
{
	vec4 color = texture( InputTexture, uv );
	fragColor = color;
}
)";

#ifdef _WIN32
void textureCallback(TED3D11Texture* texture, TEObjectEvent event, void* info)
{
	return;
}
#endif

FFGLTouchEngineFX::FFGLTouchEngineFX()
	: FFGLTouchEnginePluginBase()
{
	srand(static_cast<long int>(time(0)));


	// Input properties
	SetMinInputs(0);
	SetMaxInputs(1);

	// Parameters
 	SetParamInfof(0, "Tox File", FF_TYPE_FILE);
	SetParamInfof(1, "Reload", FF_TYPE_EVENT);
	SetParamInfof(2, "Unload", FF_TYPE_EVENT);
	SetParamInfof(3, "Clear Instance", FF_TYPE_EVENT);

	//This is the starting point for the parameters and should be the count of manually added parameters.
	OffsetParamsByType = 4;


	MaxParamsByType = 40;


	ConstructBaseParameters();

#ifdef _WIN32
	EnableSpoutLogFile("FFGLTouchEngineFX.log");
#endif

}

FFGLTouchEngineFX::~FFGLTouchEngineFX()
{

	if (instance != nullptr)
	{
		TEInstanceSuspend(instance);
		TEInstanceUnload(instance);
	}


}

FFResult FFGLTouchEngineFX::InitGL(const FFGLViewportStruct* vp)
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
	if (FAILED(hr))
	{
		FFGLLog::LogToHost("Failed to create D3D11 device");
		return FF_FAIL;
	}
#endif

	//Load TouchEngine
	LoadTouchEngine();

#ifdef _WIN32
	SpoutIDInput = GenerateRandomString(15);
	SpoutIDOutput = GenerateRandomString(15);

	SpoutTextureOutput = 0;
	SpoutTextureInput = 0;
#endif

	if (!shader.Compile(vertexShaderCode, fragmentShaderCode))
	{
		DeInitGL();
		return FailAndLog("Failed to compile shader");

	}

	// Initialize the quad
	if (!quad.Initialise())
	{
		DeInitGL();
		return FailAndLog("Failed to initialize quad");
	}


	// Set the viewport size
	OutputWidth = vp->width;
	OutputHeight = vp->height;


	if (FilePath.empty())
	{
		return CFFGLPlugin::InitGL(vp);;
	}

	bool Status = LoadTEFile();

	if (!Status)
	{
		FFGLLog::LogToHost("Failed to load TE file");
		return FF_FAIL;
	}

	return CFFGLPlugin::InitGL(vp);
}


FFResult FFGLTouchEngineFX::ProcessOpenGL(ProcessOpenGLStruct* pGL)
{

	if (!isTouchEngineLoaded || !isTouchEngineReady || isTouchFrameBusy)
	{
		return FF_SUCCESS;
	}

	if (pGL->numInputTextures < 1) {
		isVideoFX = false;
		hasVideoInput = false;
		return FF_FAIL;
	}

	if (pGL->inputTextures[0] == nullptr) {
		isVideoFX = false;
		hasVideoInput = false;
		return FF_FAIL;
	}


	TouchObject<TETexture> TETextureToSend;

	if (hasVideoOutput) {

		//Will need to replace the below value with something more standard
		TEResult result = TEInstanceLinkGetTextureValue(instance, OutputOpName.c_str(), TELinkValueCurrent, TETextureToSend.take());
#ifdef _WIN32
		if (result == TEResultSuccess && TETextureToSend != nullptr) {
			if (TETextureGetType(TETextureToSend) == TETextureTypeD3DShared && result == TEResultSuccess) {
				TouchObject<TED3D11Texture> D3DTextureToSend;
				result = TED3D11ContextGetTexture(D3DContext, static_cast<TED3DSharedTexture*>(TETextureToSend.get()), D3DTextureToSend.take());
				if (result != TEResultSuccess)
				{
					return FF_FALSE;
				}
				ID3D11Texture2D* RawTextureToSend = TED3D11TextureGetTexture(D3DTextureToSend);

				if (RawTextureToSend == nullptr) {
					return FF_FALSE;
				}

				D3D11_TEXTURE2D_DESC RawTextureDesc;
				ZeroMemory(&RawTextureDesc, sizeof(RawTextureDesc));

				RawTextureToSend->GetDesc(&RawTextureDesc);

				if (!OutputInteropInitialized) {

					OutputInterop.SetSenderName(SpoutIDOutput.c_str());

					OutputInterop.OpenDirectX11(D3DDevice.Get());

					if (!OutputInterop.CreateInterop(OutputWidth, OutputHeight, RawTextureDesc.Format, false)) {
						FFGLLog::LogToHost("Failed to create interop");
						return FF_FAIL;
					}

					OutputInterop.frame.CreateAccessMutex("mutex2");

					if (!OutputInterop.spoutdx.CreateDX11Texture(D3DDevice.Get(), OutputWidth, OutputHeight, RawTextureDesc.Format, &D3DTextureOutput)) {
						FFGLLog::LogToHost("Failed to create DX11 texture");
						return FF_FAIL;
					}

					InitializeGlTexture(SpoutTextureOutput, OutputWidth, OutputHeight, GetGlType(RawTextureDesc.Format));
					DXFormat = RawTextureDesc.Format;

					OutputInteropInitialized = true;
				}

				if (
					RawTextureDesc.Width != OutputWidth
					|| RawTextureDesc.Height != OutputHeight
					|| RawTextureDesc.Format != DXFormat
					) {
					OutputWidth = RawTextureDesc.Width;
					OutputHeight = RawTextureDesc.Height;

					if (!OutputInterop.CleanupInterop()) {
						FFGLLog::LogToHost("Failed to cleanup interop");
						return FF_FAIL;
					}

					if (!OutputInterop.CreateInterop(OutputWidth, OutputHeight, RawTextureDesc.Format, false)) {
						FFGLLog::LogToHost("Failed to create interop");
						return FF_FAIL;
					}

					OutputInterop.spoutdx.CreateDX11Texture(D3DDevice.Get(), OutputWidth, OutputHeight, RawTextureDesc.Format, &D3DTextureOutput);

					InitializeGlTexture(SpoutTextureOutput, OutputWidth, OutputHeight, GetGlType(RawTextureDesc.Format));

					DXFormat = RawTextureDesc.Format;

				}


				IDXGIKeyedMutex* keyedMutex;
				RawTextureToSend->QueryInterface<IDXGIKeyedMutex>(&keyedMutex);
				if (keyedMutex == nullptr) {
					return FF_FAIL;
				}

				TESemaphore* semaphore = nullptr;
				uint64_t waitValue = 0;
				if (TEInstanceHasTextureTransfer(instance, TETextureToSend) == false)
				{
					result = TEInstanceAddTextureTransfer(instance, TETextureToSend, semaphore, waitValue);
					if (result != TEResultSuccess)
					{
						return FF_FAIL;
					}
				}
				result = TEInstanceGetTextureTransfer(instance, TETextureToSend, &semaphore, &waitValue);
				if (result != TEResultSuccess)
				{
					return FF_FALSE;
				}
				keyedMutex->AcquireSync(waitValue, INFINITE);

				Microsoft::WRL::ComPtr<ID3D11DeviceContext> devContext;
				D3DDevice->GetImmediateContext(&devContext);
				devContext->CopyResource(D3DTextureOutput.Get(), RawTextureToSend);
				OutputInterop.WriteTexture(D3DTextureOutput.GetAddressOf());
				keyedMutex->ReleaseSync(waitValue + 1);

				result = TEInstanceAddTextureTransfer(instance, TETextureToSend, semaphore, waitValue + 1);
				if (result != TEResultSuccess)
				{
					return FF_FAIL;
				}
				devContext->Flush();
				devContext->Release();
				keyedMutex->Release();
			}

		}

		OutputInterop.ReadGLDXtexture(SpoutTextureOutput, GL_TEXTURE_2D, OutputWidth, OutputHeight, true, pGL->HostFBO);

		ffglex::ScopedShaderBinding shaderBinding(shader.GetGLID());
		ffglex::ScopedSamplerActivation activateSampler(0);
		ffglex::Scoped2DTextureBinding textureBinding(SpoutTextureOutput);
		shader.Set("InputTexture", 0);
		shader.Set("MaxUV", 1.0f, 1.0f);
		quad.Draw();
#endif
	}


	// Unbind the input texture
	glBindTexture(GL_TEXTURE_2D, 0);


	isTouchFrameBusy = true;



	for (auto& param : Parameters) {
		FFUInt32 type = ParameterMapType[param.second];
		
		if (ActiveVectorParams.find(param.second) != ActiveVectorParams.end()) {
			continue;
		}

		if (type == FF_TYPE_STANDARD) {
			TEResult result = TEInstanceLinkSetDoubleValue(instance, param.first.c_str(), &ParameterMapFloat[param.second], 1);
			if (result != TEResultSuccess)
			{
				isTouchFrameBusy = false;
				return FailAndLog("Failed to set double value");
			}
		}

		if (type == FF_TYPE_INTEGER || type == FF_TYPE_OPTION) {
			TEResult result = TEInstanceLinkSetIntValue(instance, param.first.c_str(), &ParameterMapInt[param.second], 1);
			if (result != TEResultSuccess)
			{
				isTouchFrameBusy = false;
				return FailAndLog("Failed to set int value");
			}
		}


		if (type == FF_TYPE_BOOLEAN || type == FF_TYPE_EVENT) {
			TEResult result = TEInstanceLinkSetBooleanValue(instance, param.first.c_str(), ParameterMapBool[param.second]);
			if (result != TEResultSuccess)
			{
				isTouchFrameBusy = false;
				return FailAndLog("Failed to set boolean value");
			}
		}

		if (type == FF_TYPE_TEXT) {
			TEResult result = TEInstanceLinkSetStringValue(instance, param.first.c_str(), ParameterMapString[param.second].c_str());
			if (result != TEResultSuccess)
			{
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
		if (result != TEResultSuccess)
		{
			isTouchFrameBusy = false;
			return FailAndLog("Failed to set int value");
		}
		
	}

	TouchObject<TETexture> TETextureToReceive;
	if (hasVideoInput) {
		
		//Basic functions to get data out of resolume and into a texture.
		ffglex::ScopedShaderBinding shaderBinding(shader.GetGLID());
		ffglex::ScopedSamplerActivation activateSampler(0);
		ffglex::Scoped2DTextureBinding textureBinding(pGL->inputTextures[0]->Handle);

		shader.Set("InputTexture", 0);
		FFGLTexCoords maxCoords = GetMaxGLTexCoords(*pGL->inputTextures[0]);
		shader.Set("MaxUV", maxCoords.s, maxCoords.t);
		quad.Draw();

		GLint InputFormat = 0;
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &InputFormat);

		
		if (!InputInteropInitialized) {

			InputWidth = pGL->inputTextures[0]->Width;
			InputHeight = pGL->inputTextures[0]->Height;
#ifdef _WIN32
			/*
			DXGI_FORMAT texformat = DXOutputFormat;

			if (!InputInterop.CreateInterop(InputWidth, InputHeight, texformat, false)) {
				FFGLLog::LogToHost("Failed to create interop");
				return FF_FAIL;
			}

			if(!InputInterop.frame.CreateAccessMutex(SpoutID.c_str())) {
				FFGLLog::LogToHost("Failed to create access mutex");
				return FF_FAIL;
			}
			*/

			InputInterop.SetSenderName(SpoutIDInput.c_str());

			if (!InputInterop.OpenDirectX11(D3DDevice.Get())) {
				FFGLLog::LogToHost("Failed to open DirectX11");
				return FF_FAIL;
			}

			if (!InputInterop.CreateInterop(InputWidth, InputHeight, GlToDXFromat(InputFormat), false)) {
				FFGLLog::LogToHost("Failed to create interop");
				return FF_FAIL;
			}

			InputInterop.frame.CreateAccessMutex("mutex1");

			if (!InputInterop.spoutdx.CreateDX11Texture(D3DDevice.Get(), InputWidth, InputHeight, GlToDXFromat(InputFormat), &D3DTextureInput)) {
				FFGLLog::LogToHost("Failed to create DX11 texture");
				return FF_FAIL;
			}

			InitializeGlTexture(SpoutTextureInput, InputWidth, InputHeight, GetGlType(InputFormat));

			InputInteropInitialized = true;
#endif
		}

		if (
			InputWidth != pGL->inputTextures[0]->Width
			|| InputHeight != pGL->inputTextures[0]->Height
			|| InputFormat != GLFormat
		) {

			InputWidth = pGL->inputTextures[0]->Width;
			InputHeight = pGL->inputTextures[0]->Height;

#ifdef _WIN32
			if (!InputInterop.CleanupInterop()) {
				FFGLLog::LogToHost("Failed to cleanup interop");
				return FF_FAIL;
			}

			if (!InputInterop.CreateInterop(InputWidth, InputHeight, GlToDXFromat(InputFormat), false)) {
				FFGLLog::LogToHost("Failed to create interop");
				return FF_FAIL;
			}

			InputInterop.spoutdx.CreateDX11Texture(D3DDevice.Get(), InputWidth, InputHeight, GlToDXFromat(InputFormat), &D3DTextureInput);

			InitializeGlTexture(SpoutTextureInput, InputWidth, InputHeight, GetGlType(InputFormat));
#endif
			GLFormat = InputFormat;
		}

#ifdef _WIN32
		InputInterop.WriteGLDXtexture(pGL->inputTextures[0]->Handle, GL_TEXTURE_2D, InputWidth, InputHeight, true, pGL->HostFBO);
		
		InputInterop.ReadTexture(D3DTextureInput.GetAddressOf());

		//Creates a TETexture from the D3D11 texture
		TETextureToReceive.take(TED3D11TextureCreate(D3DTextureInput.Get(), TETextureOriginTopLeft, kTETextureComponentMapIdentity, (TED3D11TextureCallback)textureCallback, nullptr));
		//TEResult result = TEInstanceAddTextureTransfer(instance, TETextureToReceive, nullptr, 0);

		//Sets the texture to the input of the TouchEngine instance
		TEResult result = TEInstanceLinkSetTextureValue(instance, "op/in1", TETextureToReceive, D3DContext);
		
		if (result != TEResultSuccess)
		{
			isTouchFrameBusy = false;
			return FF_FAIL;
		}
#endif

		//keyedMutex->Release();
	}

	TEResult result = TEInstanceStartFrameAtTime(instance, FrameCount, 60, false);
	if (result != TEResultSuccess)
	{
		isTouchFrameBusy = false;
		return FF_FAIL;
	}
	FrameCount++;


	return FF_SUCCESS;
}


FFResult FFGLTouchEngineFX::DeInitGL()
{

#ifdef _WIN32
	for (auto it : TextureMutexMap) 
		it.second->Release();

	TextureMutexMap.clear();
#endif

	if (instance != nullptr)
	{
		if (isTouchEngineLoaded)
		{
			TEInstanceSuspend(instance);
			TEInstanceUnload(instance);
		}
	}

	// Deinitialize the quad
	quad.Release();

	return FF_SUCCESS;
}


FFResult FFGLTouchEngineFX::SetFloatParameter(unsigned int dwIndex, float value) {

	if (dwIndex == 1 && value == 1) {
		LoadTouchEngine();
		LoadTEFile();
		return FF_SUCCESS;
	}

	if (dwIndex == 2 && value == 1) {
		if (isTouchEngineLoaded)
		{
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

FFResult FFGLTouchEngineFX::SetTextParameter(unsigned int dwIndex, const char* value) {
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

float FFGLTouchEngineFX::GetFloatParameter(unsigned int dwIndex) {

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

char* FFGLTouchEngineFX::GetTextParameter(unsigned int dwIndex) {
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

bool FFGLTouchEngineFX::LoadTEGraphicsContext(bool reload)
{
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
	if (result != TEResultSuccess)
	{
		return false;
	}
	isGraphicsContextLoaded = true;
#endif
	return isGraphicsContextLoaded;
}
/*
bool FFGLTouchEngineFX::CreateInputTexture(int width, int height, DXGI_FORMAT dxformat) {
	// Create the input texture

	if (D3DTextureInput != nullptr)
	{
		D3DTextureInput->Release();
		D3DTextureInput = nullptr;
	}


	D3D11_TEXTURE2D_DESC description = { 0 };
	description.Width = width;
	description.Height = height;
	description.Format = dxformat;
	description.Usage = D3D11_USAGE_DEFAULT;
	description.CPUAccessFlags = 0;
	description.MiscFlags = 0;
	description.MipLevels = 0;
	description.ArraySize = 1;
	description.SampleDesc.Count = 1;
	description.SampleDesc.Quality = 0;
	description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	description.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

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

	Microsoft::WRL::ComPtr <IDXGIResource> InputSharedResource;

	hr = D3DTextureInput->QueryInterface(__uuidof(IDXGIResource), (void**)&InputSharedResource);

	if (FAILED(hr))
	{
		FFGLLog::LogToHost("Failed to get input share handle");
		return false;
	}

	hr = InputSharedResource->GetSharedHandle(&InputSharedHandle);

	if (FAILED(hr))
	{
		FFGLLog::LogToHost("Failed to get input share handle");
		return false;
	}

	InputSharedResource->Release();

	InitializeGlTexture(SpoutTextureInput, width, height);

	ffglex::Scoped2DTextureBinding inputBinding(SpoutTextureInput);
//	glBindTexture(GL_TEXTURE_2D, SpoutTextureInput);

	if (dxInteropHandle == 0 ) {
		dxInteropHandle = wglDXOpenDeviceNV(D3DDevice.Get());
	}

	if (dxInteropHandle == 0)
	{
		FFGLLog::LogToHost("Failed to open device");
		return false;
	}

	dxInteropInputObject = wglDXRegisterObjectNV(dxInteropHandle, D3DTextureInput.Get(), SpoutTextureInput, GL_TEXTURE_2D, WGL_ACCESS_READ_WRITE_NV);

	if (dxInteropInputObject == 0)
	{
		FFGLLog::LogToHost("Failed to register object");
		return false;
	}
 
	return true;


}

bool FFGLTouchEngineFX::CreateOutputTexture(int width, int height, DXGI_FORMAT dxformat) {
	if (D3DTextureOutput != nullptr)
	{
		D3DTextureOutput->Release();
		D3DTextureOutput = nullptr;
	}

	D3D11_TEXTURE2D_DESC description = { 0 };
	description.Width = width;
	description.Height = height;
	description.Format = dxformat;
	description.Usage = D3D11_USAGE_DEFAULT;
	description.CPUAccessFlags = 0;
	description.MiscFlags = 0;
	description.MipLevels = 0;
	description.ArraySize = 1;
	description.SampleDesc.Count = 1;
	description.SampleDesc.Quality = 0;
	description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	description.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

	HRESULT hr = D3DDevice->CreateTexture2D(&description, nullptr, &D3DTextureOutput);

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

	Microsoft::WRL::ComPtr <IDXGIResource> OutputSharedResource;

	hr = D3DTextureOutput->QueryInterface(__uuidof(IDXGIResource), (void**)&OutputSharedResource);

	if (FAILED(hr))
	{
		FFGLLog::LogToHost("Failed to get output share handle");
		return false;
	}

	hr = OutputSharedResource->GetSharedHandle(&OutputSharedHandle);

	if (FAILED(hr))
	{
		FFGLLog::LogToHost("Failed to get output share handle");
		return false;
	}

	OutputSharedResource->Release();

	InitializeGlTexture(SpoutTextureOutput, width, height);

	ffglex::Scoped2DTextureBinding inputBinding(SpoutTextureOutput);
	//glBindTexture(GL_TEXTURE_2D, SpoutTextureOutput);
	if (dxInteropHandle == 0) {
		dxInteropHandle = wglDXOpenDeviceNV(D3DDevice.Get());
	}

	if (dxInteropHandle == 0)
	{
		FFGLLog::LogToHost("Failed to open device");
		return false;
	}

	dxInteropOutputObject = wglDXRegisterObjectNV(dxInteropHandle, D3DTextureOutput.Get(), SpoutTextureOutput, GL_TEXTURE_2D, WGL_ACCESS_READ_WRITE_NV);

	if (dxInteropOutputObject == 0)
	{
		FFGLLog::LogToHost("Failed to register object");
		return false;
	}

	return true;

}
*/
void FFGLTouchEngineFX::ConstructBaseParameters() {
	for (uint32_t i = OffsetParamsByType; i < MaxParamsByType + OffsetParamsByType; i++)
	{
		SetParamInfof(i, (std::string("Parameter") + std::to_string(i)).c_str(), FF_TYPE_STANDARD);
		SetParamVisibility(i, false, false);
	}


	for (uint32_t i = MaxParamsByType + OffsetParamsByType; i < (MaxParamsByType * 2) + OffsetParamsByType; i++)
	{
		SetParamInfof(i, (std::string("Parameter") + std::to_string(i)).c_str(), FF_TYPE_INTEGER);
		SetParamRange(i, -10000, 10000);
		SetParamVisibility(i, false, false);
	}

	for (uint32_t i = (MaxParamsByType * 2) + OffsetParamsByType; i < (MaxParamsByType * 3) + OffsetParamsByType; i++)
	{
		SetParamInfof(i, (std::string("Parameter") + std::to_string(i)).c_str(), FF_TYPE_BOOLEAN);
		SetParamVisibility(i, false, false);
	}


	for (uint32_t i = (MaxParamsByType * 3) + OffsetParamsByType; i < (MaxParamsByType * 4) + OffsetParamsByType; i++)
	{
		SetParamInfof(i, (std::string("Parameter") + std::to_string(i)).c_str(), FF_TYPE_TEXT);
		SetParamVisibility(i, false, false);
	}


	for (uint32_t i = (MaxParamsByType * 4) + OffsetParamsByType; i < (MaxParamsByType * 5) + OffsetParamsByType; i++)
	{
		SetParamInfof(i, (std::string("Pulse")).c_str(), FF_TYPE_EVENT);
		SetParamVisibility(i, false, false);
	}
	
	for (uint32_t i = (MaxParamsByType * 5) + OffsetParamsByType; i < (MaxParamsByType * 6) + OffsetParamsByType; i++)
	{
		SetOptionParamInfo(i, (std::string("Parameter") + std::to_string(i)).c_str(), 10, 0);
		SetParamVisibility(i, false, false);
	}
	

	return;

}

void FFGLTouchEngineFX::ResetBaseParameters() {

	for (auto& ParamID : ActiveParams) {
		SetParamVisibility(ParamID, false, true);
	}
	hasVideoInput = false;
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

void FFGLTouchEngineFX::GetAllParameters()
{
	ResetBaseParameters();

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

				if (ActiveParams.size() > MaxParamsByType * 6) {
					FFGLLog::LogToHost("Too many parameters, skipping");
					continue;
				}

				if (linkInfo->type == TELinkTypeGroup) {
					CreateParametersFromGroup(linkInfo);
				}
				else {
					CreateIndividualParameter(linkInfo);
				}
			}
			else if (linkInfo->domain == TELinkDomainOperator) {
				if (strcmp(linkInfo->name, "in1") == 0 && linkInfo->type == TELinkTypeTexture)
				{
					isVideoFX = true;
					hasVideoInput = true;
				}

			}

		}
	}

	// Get the texture output if it exists
	result = TEInstanceGetLinkGroups(instance, TEScopeOutput, groupLinkInfo.take());

	if (result != TEResultSuccess)
	{
		return;
	}

	for (int i = 0; i < groupLinkInfo->count; i++) {
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

			if (linkInfo->domain == TELinkDomainOperator) {
				if (strcmp(linkInfo->name, "out1") == 0 && linkInfo->type == TELinkTypeTexture)
				{
					OutputOpName = linkInfo->identifier;
					hasVideoOutput = true;
					break;
				}
				else if (linkInfo->type == TELinkTypeTexture)
				{
					OutputOpName = linkInfo->identifier;
					hasVideoOutput = true;
					break;
				}
			}
		}
	
	}

}

void FFGLTouchEngineFX::CreateIndividualParameter(const TouchObject<TELinkInfo>& linkInfo) {

	switch (linkInfo->type)
	{
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
			if (result != TEResultSuccess)
			{
				return;
			}

			double max[4] = { 0, 0, 0, 0 };
			result = TEInstanceLinkGetDoubleValue(instance, linkInfo->identifier, TELinkValueUIMaximum, max, linkInfo->count);
			if (result != TEResultSuccess)
			{
				return;
			}
			double min[4] = { 0, 0, 0, 0 };
			result = TEInstanceLinkGetDoubleValue(instance, linkInfo->identifier, TELinkValueUIMinimum, min, linkInfo->count);
			if (result != TEResultSuccess)
			{
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
		if (result != TEResultSuccess)
		{
			return;
		}
		//SetParamInfo(Parameters[j].second, linkInfo->name, FF_TYPE_STANDARD, static_cast<float>(value));
		ParameterMapFloat[ParamID] = value;

		double max = 0;
		result = TEInstanceLinkGetDoubleValue(instance, linkInfo->identifier, TELinkValueUIMaximum, &max, 1);
		if (result != TEResultSuccess)
		{
			return;
		}
		double min = 0;
		result = TEInstanceLinkGetDoubleValue(instance, linkInfo->identifier, TELinkValueUIMinimum, &min, 1);
		if (result != TEResultSuccess)
		{
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
			if (result != TEResultSuccess && !labels)
			{
				return;
			}

			std::vector<std::string> labelsVector;
			std::vector<float> valuesVector;

			for (int k = 0; k < labels->count; k++)
			{
				labelsVector.push_back(labels->strings[k]);
				valuesVector.push_back(static_cast<float>(k));
			}

			SetParamElements(ParamID, labelsVector, valuesVector, true);

			int32_t value = 0;
			result = TEInstanceLinkGetIntValue(instance, linkInfo->identifier, TELinkValueCurrent, &value, 1);
			if (result != TEResultSuccess)
			{
				return;
			}

			Parameters.push_back(std::make_pair(linkInfo->identifier, ParamID));
			ActiveParams.insert(ParamID);
			SetParamDisplayName(ParamID, linkInfo->label, true);
			ParameterMapType[ParamID] = FF_TYPE_OPTION;

			RaiseParamEvent(ParamID, FF_EVENT_FLAG_VALUE);
			SetParamVisibility(ParamID, true, true);
			break;
		}
		else {
			uint32_t ParamID = (ParameterMapInt.size() + OffsetParamsByType) + MaxParamsByType;
			Parameters.push_back(std::make_pair(linkInfo->identifier, ParamID));
			ActiveParams.insert(ParamID);
			ParameterMapType[ParamID] = FF_TYPE_INTEGER;

			int32_t value = 0;
			result = TEInstanceLinkGetIntValue(instance, linkInfo->identifier, TELinkValueCurrent, &value, 1);
			if (result != TEResultSuccess)
			{
				return;
			}
			SetParamDisplayName(ParamID, linkInfo->label, true);
			ParameterMapInt[ParamID] = value;


			int32_t max = 0;
			result = TEInstanceLinkGetIntValue(instance, linkInfo->identifier, TELinkValueUIMaximum, &max, 1);
			if (result != TEResultSuccess)
			{
				return;
			}

			int32_t min = 0;
			result = TEInstanceLinkGetIntValue(instance, linkInfo->identifier, TELinkValueUIMinimum, &min, 1);
			if (result != TEResultSuccess)
			{
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
			if (result != TEResultSuccess)
			{
				return;
			}

			SetParamDisplayName(ParamID, linkInfo->label, true);
			ParameterMapBool[ParamID] = value;
			RaiseParamEvent(ParamID, FF_EVENT_FLAG_VALUE);
			SetParamVisibility(ParamID, true, true);
		}
		else
		{
			//SetParamInfof(Parameters[j].second, linkInfo->name, FF_TYPE_BOOLEAN);
			uint32_t ParamID = (ParameterMapBool.size() + OffsetParamsByType) + MaxParamsByType * 2;
			Parameters.push_back(std::make_pair(linkInfo->identifier, ParamID));
			ActiveParams.insert(ParamID);
			ParameterMapType[ParamID] = FF_TYPE_BOOLEAN;

			bool value = false;
			result = TEInstanceLinkGetBooleanValue(instance, linkInfo->identifier, TELinkValueCurrent, &value);
			if (result != TEResultSuccess)
			{
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
		if (result != TEResultSuccess)
		{
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

void FFGLTouchEngineFX::CreateParametersFromGroup(const TouchObject<TELinkInfo>& linkInfo) {

	TouchObject<TEStringArray> links;
	TEResult result = TEInstanceLinkGetChildren(instance, linkInfo->identifier, links.take());

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
			FFGLLog::LogToHost("Failed to get link info");

		}

		CreateIndividualParameter(linkInfo);
	}



}

bool FFGLTouchEngineFX::LoadTEFile()
{
	// Load the tox file into the TouchEngine
	// 1. Create a TouchEngine object


	if (instance == nullptr)
	{
		return false;
	}

	isTouchEngineReady = false;

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


void FFGLTouchEngineFX::LoadTouchEngine() {

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

void FFGLTouchEngineFX::ResumeTouchEngine() {
	TEResult result = TEInstanceResume(instance);
	if (result != TEResultSuccess)
	{
		FFGLLog::LogToHost("Failed to resume TouchEngine instance");
	}


	GetAllParameters();
	return;

}

void FFGLTouchEngineFX::ClearTouchInstance() {
	if (instance != nullptr)
	{
		if (isTouchEngineLoaded)
		{
			TEInstanceSuspend(instance);
			TEInstanceUnload(instance);
		}
#ifdef _WIN32
		InputInteropInitialized = !InputInterop.CleanupInterop();
		OutputInteropInitialized = !OutputInterop.CleanupInterop();
		D3DContext.reset();
#endif
        instance.reset();
        isGraphicsContextLoaded = false;
	}
	return;
}

void FFGLTouchEngineFX::InitializeGlTexture(GLuint& texture, uint16_t width, uint16_t height, GLenum type)
{
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


void FFGLTouchEngineFX::eventCallback(TEEvent event, TEResult result, int64_t start_time_value, int32_t start_time_scale, int64_t end_time_value, int32_t end_time_scale)
{


	if (result == TEResultComponentErrors)
	{
		TouchObject<TEErrorArray> errors;
		TEResult result = TEInstanceGetErrors(instance, errors.take());
		if (result != TEResultSuccess)
		{
			return;
		}

		if (!errors) {
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
	case TEEventFrameDidFinish: {
		isTouchFrameBusy = false;
		// A frame has finished rendering
		break;
	}
	case TEEventInstanceReady: {
		FFGLLog::LogToHost("TouchEngine Ready");
		isTouchEngineReady = true;
		// The TouchEngine is ready to start rendering frames
		break;
	}
	case TEEventInstanceDidUnload: {
		FFGLLog::LogToHost("TouchEngine Unloaded");
		isTouchEngineLoaded = false;
		break;
	}
	}
}

void FFGLTouchEngineFX::linkCallback(TELinkEvent event, const char* identifier)
{
	switch (event) {
	case TELinkEventAdded:
		// A link has been added
		break;
	case TELinkEventValueChange:
		break;
	}
}

void FFGLTouchEngineFX::eventCallbackStatic(TEInstance* instance, TEEvent event, TEResult result, int64_t start_time_value, int32_t start_time_scale, int64_t end_time_value, int32_t end_time_scale, void* info)
{
	static_cast<FFGLTouchEngineFX*>(info)->eventCallback(event, result, start_time_value, start_time_scale, end_time_value, end_time_scale);
}

void FFGLTouchEngineFX::linkCallbackStatic(TEInstance* instance, TELinkEvent event, const char* identifier, void* info)
{
	static_cast<FFGLTouchEngineFX*>(info)->linkCallback(event, identifier);
}
