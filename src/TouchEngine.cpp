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

FFGLTouchEngine::FFGLTouchEngine()
	: CFFGLPlugin()
{
	// Input properties
	SetMinInputs(1);
	SetMaxInputs(100);

	// Parameters
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

	return FF_SUCCESS;
}

FFResult FFGLTouchEngine::ProcessOpenGL(ProcessOpenGLStruct* pGL)
{
	if (pGL->numInputTextures < 1)
		return FF_FAIL;

	FFGLTextureStruct &Texture = *(pGL->inputTextures[0]);

	// Bind the shader
	shader.Bind();

	// Bind the input texture
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, Texture.Handle);
	shader.SetTexture(0);

	// Set the uniforms
	glUniform4f(rgbLeftLocation, rgba1.red, rgba1.green, rgba1.blue, rgba1.alpha);
	glUniform4f(rgbRightLocation, hsba2.hue, hsba2.sat, hsba2.bri, hsba2.alpha);

	// Render the quad
	quad.Render();

	// Unbind the input texture
	glBindTexture(GL_TEXTURE_2D, 0);

	// Unbind the shader
	shader.Unbind();

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
	quad.FreeGLResources();

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

	GetAllParameters();
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