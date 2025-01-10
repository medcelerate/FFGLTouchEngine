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

static CFFGLThumbnailInfo ThumbnailInfo(160, 120, thumbnail);

static const char vertexShaderCode[] = R"(#version 410 core
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
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

FFGLTouchEngine::FFGLTouchEngine()
	: FFGLTouchEnginePluginBase()
{
	// Input properties
	SetMinInputs(0);
	SetMaxInputs(0);

	ConstructBaseParameters();

#ifdef _WIN32
	EnableSpoutLogFile("FFGLTouchEngine.log");
#endif

}

FFGLTouchEngine::~FFGLTouchEngine()
{
#ifdef __APPLE__
	if (pDevice != nullptr) {
		pDevice->release();
		pDevice = nullptr;
	}
#endif
}

FFResult FFGLTouchEngine::InitGL(const FFGLViewportStruct* vp)
{
	FFResult result = InitializeDevice();
	if (result != FF_SUCCESS)
	{
		return result;
	}

#ifdef __APPLE__
    pDevice = MTL::CreateSystemDefaultDevice();
#endif

	//Load TouchEngine
	LoadTouchEngine();


#ifdef _WIN32
	SpoutID = GenerateRandomString(15);
	SpoutTexture = 0;
#endif

	result = InitializeShader(vertexShaderCode, fragmentShaderCode);
	if (result != FF_SUCCESS)
	{
		return result;
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
		return FailAndLog("Failed to load tox file");
	}

	return CFFGLPlugin::InitGL(vp);
}


FFResult FFGLTouchEngine::ProcessOpenGL(ProcessOpenGLStruct* pGL)
{

	if (!isTouchEngineLoaded || !isTouchEngineReady || isTouchFrameBusy)
	{
		return FF_SUCCESS;
	}

	if (hasVideoOutput) {
		TouchObject<TETexture> TETextureToSend;
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

				if (!isInteropInitialized) {
					OutputInterop.SetSenderName(SpoutID.c_str());

					if (!OutputInterop.OpenDirectX11(D3DDevice.Get())) {
						return FailAndLog("Failed to open DirectX11");
					}

					if (!OutputInterop.CreateInterop(OutputWidth, OutputHeight, RawTextureDesc.Format, false)) {
						return FailAndLog("Failed to create interop");
					}

					OutputInterop.frame.CreateAccessMutex("mutex");

					if (!OutputInterop.spoutdx.CreateDX11Texture(D3DDevice.Get(), OutputWidth, OutputHeight, RawTextureDesc.Format, &D3DTextureOutput)) {
						return FailAndLog("Failed to create DX11 texture");
					}

					InitializeGlTexture(SpoutTexture, OutputWidth, OutputHeight, GetGlType(RawTextureDesc.Format));

					isInteropInitialized = true;
				}

				if (
					RawTextureDesc.Width != OutputWidth
					|| RawTextureDesc.Height != OutputHeight
					|| GetGlType(RawTextureDesc.Format) != GLFormat
					) {
					OutputWidth = RawTextureDesc.Width;
					OutputHeight = RawTextureDesc.Height;

					if (!OutputInterop.CleanupInterop()) {
						return FailAndLog("Failed to cleanup interop");
					}

					if (!OutputInterop.CreateInterop(OutputWidth, OutputHeight, RawTextureDesc.Format, false)) {
						return FailAndLog("Failed to create interop");
					}

					OutputInterop.spoutdx.CreateDX11Texture(D3DDevice.Get(), OutputWidth, OutputHeight, RawTextureDesc.Format, &D3DTextureOutput);

					InitializeGlTexture(SpoutTexture, OutputWidth, OutputHeight, GetGlType(RawTextureDesc.Format));
				}

				IDXGIKeyedMutex* keyedMutex;
				RawTextureToSend->QueryInterface<IDXGIKeyedMutex>(&keyedMutex);

				if (keyedMutex == nullptr) {
					return FF_FAIL;
				}

				//Dynamically change texture size here when wxH changes

				TESemaphore* semaphore = nullptr;
				uint64_t waitValue = 0;
				/*
				if (TEInstanceHasTextureTransfer(instance, TETextureToSend) == false)
				{
					result = TEInstanceAddTextureTransfer(instance, TETextureToSend, semaphore, waitValue);
					if (result != TEResultSuccess)
					{
						return FF_FAIL;
					}
				}*/
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

		OutputInterop.ReadGLDXtexture(SpoutTexture, GL_TEXTURE_2D, OutputWidth, OutputHeight, true, pGL->HostFBO);

		//Receiver the texture from spout

		ffglex::ScopedShaderBinding shaderBinding(shader.GetGLID());
		ffglex::ScopedSamplerActivation activateSampler(0);
		ffglex::Scoped2DTextureBinding textureBinding(SpoutTexture);
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
		TEResult result = TEInstanceStartFrameAtTime(instance, FrameCount, 60, false);
		if (result != TEResultSuccess)
		{
			isTouchFrameBusy = false;
			return FF_FAIL;
		}
		FrameCount++;


	return FF_SUCCESS;
}


FFResult FFGLTouchEngine::DeInitGL()
{

	if (instance != nullptr)
	{
		if (isTouchEngineLoaded)
		{
			TEInstanceSuspend(instance);
			TEInstanceUnload(instance);
		}
	}

#ifdef _WIN32
	if (isInteropInitialized) {
		OutputInterop.CleanupInterop();
		OutputInterop.CloseDirectX();
	}
#endif
#ifdef __APPLE__
    if (pDevice != nullptr) {
        pDevice->release();
        pDevice = nullptr;
    }
#endif

	// Deinitialize the quad
	quad.Release();

	return FF_SUCCESS;
}

/*
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
*/

void FFGLTouchEngine::HandleOperatorLink(const TouchObject<TELinkInfo>& linkInfo)
{
	if (strcmp(linkInfo->name, "out1") == 0 && linkInfo->type == TELinkTypeTexture) {
		hasVideoOutput = true;
	}
}


void FFGLTouchEngine::ResumeTouchEngine() {
	TEResult result = TEInstanceResume(instance);
	if (result != TEResultSuccess)
	{
		FFGLLog::LogToHost("Failed to resume TouchEngine instance");
	}

#ifdef _WIN32
	if (TEVideoInputD3D == nullptr) {
		TEVideoInputD3D.take(TED3D11TextureCreate(D3DTextureInput.Get(), TETextureOriginTopLeft, kTETextureComponentMapIdentity, nullptr, nullptr));
	}
#endif

	GetAllParameters();
	return;

}

void FFGLTouchEngine::ClearTouchInstance() {
	if (instance != nullptr)
	{
		if (isTouchEngineLoaded)
		{
			TEInstanceSuspend(instance);
			TEInstanceUnload(instance);
		}
#ifdef _WIN32
		isInteropInitialized = !OutputInterop.CleanupInterop();
		D3DContext.reset();
#endif
		instance.reset();
		isGraphicsContextLoaded = false;
	}
	return;
}
