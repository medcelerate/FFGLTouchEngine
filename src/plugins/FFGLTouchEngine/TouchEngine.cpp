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

#ifdef __APPLE__
// Rectangle textures use pixel coordinates, not normalized 0-1 UVs
static const char rectVertexShaderCode[] = R"(#version 410 core
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

uniform vec2 TextureSize;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vec2(vUV.x, 1.0 - vUV.y) * TextureSize;
}
)";

static const char rectFragmentShaderCode[] = R"(#version 410 core
uniform sampler2DRect InputTexture;

in vec2 uv;
out vec4 fragColor;

void main()
{
	vec4 color = texture( InputTexture, uv );
	// IOSurface comes as BGRA, swizzle to RGBA
	fragColor = color.bgra;
}
)";
#endif

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
}

FFResult FFGLTouchEngine::InitGL(const FFGLViewportStruct* vp)
{
	FFResult result = InitializeDevice();
	if (result != FF_SUCCESS)
	{
		return result;
	}

	//Load TouchEngine
	LoadTouchEngine();


#ifdef _WIN32
	SpoutIDOutput = GenerateRandomString(15);
	SpoutTextureOutput = 0;
#endif

	result = InitializeShader(vertexShaderCode, fragmentShaderCode);
	if (result != FF_SUCCESS)
	{
		return result;
	}

#ifdef __APPLE__
	if (!rectShader.Compile(rectVertexShaderCode, rectFragmentShaderCode)) {
		DeInitGL();
		return FailAndLog("Failed to compile rectangle shader");
	}
#endif

	// Set the viewport size
	OutputWidth = vp->width;
	OutputHeight = vp->height;


	if (FilePath.empty())
	{
		return CFFGLPlugin::InitGL(vp);
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

	if (instance == nullptr || !isTouchEngineLoaded || !isTouchEngineReady || isTouchFrameBusy)
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

				if (!OutputInteropInitialized) {
					OutputInterop.SetSenderName(SpoutIDOutput.c_str());

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

					InitializeGlTexture(SpoutTextureOutput, OutputWidth, OutputHeight, GetGlType(RawTextureDesc.Format));

					OutputInteropInitialized = true;
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

					InitializeGlTexture(SpoutTextureOutput, OutputWidth, OutputHeight, GetGlType(RawTextureDesc.Format));
				}

				IDXGIKeyedMutex* keyedMutex;
				RawTextureToSend->QueryInterface<IDXGIKeyedMutex>(&keyedMutex);

				if (keyedMutex == nullptr) {
					return FF_FAIL;
				}

				TESemaphore* semaphore = nullptr;
				uint64_t waitValue = 0;
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

#ifdef __APPLE__
		// If we got a new texture from TE, blit it into our IOSurface-backed texture
		if (result == TEResultSuccess && TETextureToSend != nullptr) {
			TETextureType texType = TETextureGetType(TETextureToSend);
			id<MTLTexture> srcTexture = nil;

			if (texType == TETextureTypeMetal) {
				srcTexture = TEMetalTextureGetTexture(static_cast<TEMetalTexture*>(TETextureToSend.get()));
			} else if (texType == TETextureTypeIOSurface) {
				IOSurfaceRef surface = TEIOSurfaceTextureGetSurface(static_cast<TEIOSurfaceTexture*>(TETextureToSend.get()));
				if (surface != nullptr) {
					int w = (int)IOSurfaceGetWidth(surface);
					int h = (int)IOSurfaceGetHeight(surface);
					MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
					desc.storageMode = MTLStorageModeShared;
					srcTexture = [MetalDevice newTextureWithDescriptor:desc iosurface:surface plane:0];
				}
			}

			if (srcTexture != nil) {
				int texWidth = (int)srcTexture.width;
				int texHeight = (int)srcTexture.height;

				// Recreate our IOSurface-backed texture if size changed
				if (OutputMetalTexture == nil || texWidth != OutputWidth || texHeight != OutputHeight) {
					OutputWidth = texWidth;
					OutputHeight = texHeight;

					if (OutputIOSurface != nullptr) { CFRelease(OutputIOSurface); OutputIOSurface = nullptr; }
					OutputMetalTexture = nil;
					if (OutputTextureGL != 0) { glDeleteTextures(1, &OutputTextureGL); OutputTextureGL = 0; }

					OutputMetalTexture = CreateIOSurfaceBackedMetalTexture(OutputWidth, OutputHeight, &OutputIOSurface);
					if (OutputMetalTexture != nil && OutputIOSurface != nullptr) {
						OutputTextureGL = CreateOpenGLTextureFromIOSurface(OutputIOSurface, OutputWidth, OutputHeight);
					}
				}

				if (OutputMetalTexture != nil) {
					CopyMetalTexture(srcTexture, OutputMetalTexture);
				}

				result = TEInstanceAddTextureTransfer(instance, TETextureToSend, nullptr, 0);
			}
		}

		// Always draw the last valid frame
		if (OutputTextureGL != 0) {
			ffglex::ScopedShaderBinding shaderBinding(rectShader.GetGLID());
			ffglex::ScopedSamplerActivation activateSampler(0);
			glBindTexture(GL_TEXTURE_RECTANGLE, OutputTextureGL);
			rectShader.Set("InputTexture", 0);
			rectShader.Set("TextureSize", (float)OutputWidth, (float)OutputHeight);
			quad.Draw();
			glBindTexture(GL_TEXTURE_RECTANGLE, 0);
		}
#endif

	}


	// Unbind the input texture
	glBindTexture(GL_TEXTURE_2D, 0);

	isTouchFrameBusy = true;

	PushParametersToTouchEngine();

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
			isTouchEngineLoaded = false;
			isTouchEngineReady = false;
			TEInstanceSuspend(instance);
			TEInstanceUnload(instance);
		}
	}

#ifdef _WIN32
	if (OutputInteropInitialized) {
		OutputInterop.CleanupInterop();
		OutputInterop.CloseDirectX();
	}
#endif
#ifdef __APPLE__
	if (OutputTextureGL != 0) {
		glDeleteTextures(1, &OutputTextureGL);
		OutputTextureGL = 0;
	}
	OutputMetalTexture = nil;
	if (OutputIOSurface != nullptr) {
		CFRelease(OutputIOSurface);
		OutputIOSurface = nullptr;
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
		OutputInteropInitialized = !OutputInterop.CleanupInterop();
		D3DContext.reset();
#endif
#ifdef __APPLE__
		MetalContext.reset();
		if (OutputTextureGL != 0) {
			glDeleteTextures(1, &OutputTextureGL);
			OutputTextureGL = 0;
		}
		OutputMetalTexture = nil;
		if (OutputIOSurface != nullptr) {
			CFRelease(OutputIOSurface);
			OutputIOSurface = nullptr;
		}
#endif
		instance.reset();
		isGraphicsContextLoaded = false;
	}
	return;
}
