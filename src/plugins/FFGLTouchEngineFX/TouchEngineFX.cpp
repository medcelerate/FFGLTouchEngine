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

#ifdef _WIN32
void textureCallback(TED3D11Texture* texture, TEObjectEvent event, void* info)
{
	if (event == TEObjectEventRelease) {
		FFGLLog::LogToHost("Releasing texture");
	}
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

	ConstructBaseParameters();

#ifdef _WIN32
	EnableSpoutLogFile("FFGLTouchEngineFX.log");
#endif

}

FFGLTouchEngineFX::~FFGLTouchEngineFX()
{
}

FFResult FFGLTouchEngineFX::InitGL(const FFGLViewportStruct* vp)
{
	FFResult result = InitializeDevice();
	if (result != FF_SUCCESS)
	{
		return result;
	}

	//Load TouchEngine
	LoadTouchEngine();

#ifdef _WIN32
	SpoutIDInput = GenerateRandomString(15);
	SpoutIDOutput = GenerateRandomString(15);

	SpoutTextureOutput = 0;
	SpoutTextureInput = 0;
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


FFResult FFGLTouchEngineFX::ProcessOpenGL(ProcessOpenGLStruct* pGL)
{

	if (instance == nullptr || !isTouchEngineLoaded || !isTouchEngineReady || isTouchFrameBusy)
	{
#ifdef _WIN32
		ffglex::ScopedShaderBinding shaderBinding(shader.GetGLID());
		ffglex::ScopedSamplerActivation activateSampler(0);
		ffglex::Scoped2DTextureBinding textureBinding(SpoutTextureOutput);
		shader.Set("InputTexture", 0);
		shader.Set("MaxUV", 1.0f, 1.0f);
		quad.Draw();
#endif
		return FF_FAIL;
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

	shader.Set("InputTexture", 0);
	FFGLTexCoords maxCoords = GetMaxGLTexCoords(*pGL->inputTextures[0]);
	shader.Set("MaxUV", maxCoords.s, maxCoords.t);
	quad.Draw();

	if (hasVideoOutput) {
		TouchObject<TETexture> TETextureToSend;

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

				if (!OutputInteropInitialized || RawTextureDesc.Width != OutputWidth
					|| RawTextureDesc.Height != OutputHeight
					|| RawTextureDesc.Format != DXFormat) {

					OutputWidth = RawTextureDesc.Width;
					OutputHeight = RawTextureDesc.Height;

					OutputInterop.SetSenderName(SpoutIDOutput.c_str());

					if (!OutputInterop.OpenDirectX11(D3DDevice.Get())) {
						return FailAndLog("Failed to open DirectX11");
					}

					if (!OutputInterop.CreateInterop(OutputWidth, OutputHeight, RawTextureDesc.Format, false)) {
						return FailAndLog("Failed to create interop");
					}

					OutputInterop.frame.CreateAccessMutex("mutex2");

					if (!OutputInterop.spoutdx.CreateDX11Texture(D3DDevice.Get(), OutputWidth, OutputHeight, RawTextureDesc.Format, &D3DTextureOutput)) {
						return FailAndLog("Failed to create DX11 texture");
					}

					InitializeGlTexture(SpoutTextureOutput, OutputWidth, OutputHeight, GetGlType(RawTextureDesc.Format));
					DXFormat = RawTextureDesc.Format;

					OutputInteropInitialized = true;
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

	if (hasVideoInput) {

		ffglex::ScopedShaderBinding shaderBinding(shader.GetGLID());
		ffglex::ScopedSamplerActivation activateSampler(0);
		ffglex::Scoped2DTextureBinding textureBinding(pGL->inputTextures[0]->Handle);

		GLint InputFormat = 0;
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &InputFormat);

		if (!InputInteropInitialized || InputWidth != pGL->inputTextures[0]->Width
			|| InputHeight != pGL->inputTextures[0]->Height
			|| InputFormat != GLFormat) {

			InputWidth = pGL->inputTextures[0]->Width;
			InputHeight = pGL->inputTextures[0]->Height;
#ifdef _WIN32
			InputInterop.SetSenderName(SpoutIDInput.c_str());

			if (!InputInterop.OpenDirectX11(D3DDevice.Get())) {
				return FailAndLog("Failed to open DirectX11");
			}

			if (!InputInterop.CreateInterop(InputWidth, InputHeight, GlToDXFromat(InputFormat), false)) {
				return FailAndLog("Failed to create interop");
			}

			InputInterop.frame.CreateAccessMutex("mutex1");

			if (!InputInterop.spoutdx.CreateDX11Texture(D3DDevice.Get(), InputWidth, InputHeight, GlToDXFromat(InputFormat), &D3DTextureInput)) {
				return FailAndLog("Failed to create DX11 texture");
			}

			InitializeGlTexture(SpoutTextureInput, InputWidth, InputHeight, GetGlType(InputFormat));

			InputInteropInitialized = true;
#endif

#ifdef __APPLE__
			// Clean up old resources
			if (InputIOSurface != nullptr) { CFRelease(InputIOSurface); InputIOSurface = nullptr; }
			InputMetalTexture = nil;
			if (InputIOSurfaceGL != 0) { glDeleteTextures(1, &InputIOSurfaceGL); InputIOSurfaceGL = 0; }
			if (InputRenderFBO != 0) { glDeleteFramebuffers(1, &InputRenderFBO); InputRenderFBO = 0; }

			InputMetalTexture = CreateIOSurfaceBackedMetalTexture(InputWidth, InputHeight, &InputIOSurface);
			if (InputMetalTexture == nil || InputIOSurface == nullptr) {
				return FailAndLog("Failed to create IOSurface-backed Metal texture for input");
			}

			InputIOSurfaceGL = CreateOpenGLTextureFromIOSurface(InputIOSurface, InputWidth, InputHeight);
			if (InputIOSurfaceGL == 0) {
				return FailAndLog("Failed to create GL texture from input IOSurface");
			}

			glGenFramebuffers(1, &InputRenderFBO);
			InputInteropInitialized = true;
#endif
			GLFormat = InputFormat;
		}


#ifdef _WIN32
		InputInterop.WriteGLDXtexture(pGL->inputTextures[0]->Handle, GL_TEXTURE_2D, InputWidth, InputHeight, true, pGL->HostFBO);

		InputInterop.ReadTexture(D3DTextureInput.GetAddressOf());

		TETextureToReceive.take(TED3D11TextureCreate(D3DTextureInput.Get(), TETextureOriginTopLeft, kTETextureComponentMapIdentity, (TED3D11TextureCallback)textureCallback, nullptr));

		TEResult result = TEInstanceLinkSetTextureValue(instance, InputOpName.c_str(), TETextureToReceive, D3DContext);

		if (result != TEResultSuccess)
		{
			isTouchFrameBusy = false;
			return FF_FAIL;
		}
#endif

#ifdef __APPLE__
		if (InputIOSurfaceGL != 0 && InputMetalTexture != nil && InputRenderFBO != 0) {
			// Save current GL state
			GLint prevViewport[4];
			glGetIntegerv(GL_VIEWPORT, prevViewport);

			// Clear stale GL errors
			while (glGetError() != GL_NO_ERROR) {}

			// Render the host input texture into the IOSurface-backed rect texture
			glBindFramebuffer(GL_FRAMEBUFFER, InputRenderFBO);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, InputIOSurfaceGL, 0);
			glViewport(0, 0, InputWidth, InputHeight);

			{
				ffglex::ScopedShaderBinding shaderBinding(shader.GetGLID());
				ffglex::ScopedSamplerActivation activateSampler(0);
				ffglex::Scoped2DTextureBinding textureBinding(pGL->inputTextures[0]->Handle);
				shader.Set("InputTexture", 0);
				FFGLTexCoords maxCoords = GetMaxGLTexCoords(*pGL->inputTextures[0]);
				shader.Set("MaxUV", maxCoords.s, maxCoords.t);
				quad.Draw();
			}

			// Restore host FBO and viewport
			glBindFramebuffer(GL_FRAMEBUFFER, pGL->HostFBO);
			glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);

			// Ensure GL is fully done before TE reads the IOSurface
			glFinish();

			// Send to TouchEngine as IOSurface texture
			TouchObject<TEIOSurfaceTexture> inputTETex;
			inputTETex.take(TEIOSurfaceTextureCreate(
				InputIOSurface,
				TETextureFormatBGRA8Unorm,
				0,
				TETextureOriginBottomLeft,
				kTETextureComponentMapIdentity,
				nullptr,
				nullptr
			));

			TEResult result = TEInstanceLinkSetTextureValue(instance, InputOpName.c_str(), inputTETex, nullptr);
			if (result != TEResultSuccess) {
				isTouchFrameBusy = false;
				return FF_FAIL;
			}
		}
#endif

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
	InputMetalTexture = nil;
	if (InputIOSurface != nullptr) {
		CFRelease(InputIOSurface);
		InputIOSurface = nullptr;
	}
	if (InputIOSurfaceGL != 0) {
		glDeleteTextures(1, &InputIOSurfaceGL);
		InputIOSurfaceGL = 0;
	}
	if (InputRenderFBO != 0) {
		glDeleteFramebuffers(1, &InputRenderFBO);
		InputRenderFBO = 0;
	}
#endif

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

	// Deinitialize the quad
	quad.Release();

	return FF_SUCCESS;
}

/*
bool FFGLTouchEngineFX::CreateInputTexture(int width, int height, DXGI_FORMAT dxformat) {
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
	return true;
}
*/

void FFGLTouchEngineFX::ResetBaseParameters()
{
	FFGLTouchEnginePluginBase::ResetBaseParameters();
	hasVideoInput = false;
}

void FFGLTouchEngineFX::HandleOperatorLink(const TouchObject<TELinkInfo>& linkInfo)
{
	if (strcmp(linkInfo->name, "in1") == 0 && linkInfo->type == TELinkTypeTexture) {
		InputOpName = linkInfo->identifier;
		isVideoFX = true;
		hasVideoInput = true;
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
#ifdef __APPLE__
		MetalContext.reset();
		InputInteropInitialized = false;
		OutputInteropInitialized = false;
		if (OutputTextureGL != 0) {
			glDeleteTextures(1, &OutputTextureGL);
			OutputTextureGL = 0;
		}
		OutputMetalTexture = nil;
		if (OutputIOSurface != nullptr) {
			CFRelease(OutputIOSurface);
			OutputIOSurface = nullptr;
		}
		InputMetalTexture = nil;
		if (InputIOSurface != nullptr) {
			CFRelease(InputIOSurface);
			InputIOSurface = nullptr;
		}
		if (InputIOSurfaceGL != 0) {
			glDeleteTextures(1, &InputIOSurfaceGL);
			InputIOSurfaceGL = 0;
		}
		if (InputRenderFBO != 0) {
			glDeleteFramebuffers(1, &InputRenderFBO);
			InputRenderFBO = 0;
		}
#endif
        instance.reset();
        isGraphicsContextLoaded = false;
	}
	return;
}
