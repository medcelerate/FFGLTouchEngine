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

	FFResult SetFloatParameter(unsigned int dwIndex, float value) override;
	FFResult SetTextParameter(unsigned int dwIndex, const char* value) override;

	float GetFloatParameter(unsigned int index) override;
	char* GetTextParameter(unsigned int index) override;

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


	void InitializeGlTexture(GLuint &texture, uint16_t width, uint16_t height, GLenum type);
	void ConstructBaseParameters();
	void ResetBaseParameters();


#ifdef _WIN32
	bool CreateInputTexture(int width, int height, DXGI_FORMAT dxformat);
	bool CreateOutputTexture(int width, int height, DXGI_FORMAT dxformat);
#endif

	void ResumeTouchEngine();
	void GetAllParameters();
	void CreateIndividualParameter(const TouchObject<TELinkInfo>& linkInfo);
	void CreateParametersFromGroup(const TouchObject<TELinkInfo>& linkInfo);
	void ClearTouchInstance();
	void eventCallback(TEEvent event, TEResult result, int64_t start_time_value, int32_t start_time_scale, int64_t end_time_value, int32_t end_time_scale) override;
};
