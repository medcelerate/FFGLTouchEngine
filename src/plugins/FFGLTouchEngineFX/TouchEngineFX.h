#pragma once

#include <TouchEnginePluginBase.h>

#include "Thumbnail.h"

class FFGLTouchEngineFX : public FFGLTouchEnginePluginBase
{
public:
	FFGLTouchEngineFX();
	~FFGLTouchEngineFX() override;
    
    FFGLTouchEngineFX(const FFGLTouchEngineFX& other) = delete;
    FFGLTouchEngineFX& operator=(const FFGLTouchEngineFX& other) = delete;

	//CFFGLPlugin
	FFResult InitGL(const FFGLViewportStruct* vp) override;
	FFResult ProcessOpenGL(ProcessOpenGLStruct* pGL) override;
	FFResult DeInitGL() override;

private:
#ifdef _WIN32
	HANDLE dxInteropHandle = 0;

	HANDLE InputSharedHandle = nullptr;
	HANDLE dxInteropInputObject = 0;

	HANDLE OutputSharedHandle = nullptr;
	HANDLE dxInteropOutputObject = 0;

	std::unordered_map<int, IDXGIKeyedMutex*> MutexMap;

	//Spout Configs
	std::string SpoutIDInput;
	std::string SpoutIDOutput;

	Spout InputInterop;
	Spout OutputInterop;

	uint32_t SpoutSenderID = 0;
	GLuint SpoutTextureOutput = 0;
	GLuint SpoutTextureInput = 0;
#endif

	bool InputInteropInitialized = false;
	bool OutputInteropInitialized = false;

	int InputWidth = 0;
	int InputHeight = 0;

	void ResetBaseParameters() override;

#ifdef _WIN32
	bool CreateInputTexture(int width, int height, DXGI_FORMAT dxformat);
	bool CreateOutputTexture(int width, int height, DXGI_FORMAT dxformat);
#endif

	void ResumeTouchEngine() override;
	void ClearTouchInstance() override;

	void HandleOperatorLink(const TouchObject<TELinkInfo>& linkInfo) override;
};
