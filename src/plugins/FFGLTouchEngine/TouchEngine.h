#pragma once

#include <TouchEnginePluginBase.h>

#include "Thumbnail.h"

class FFGLTouchEngine : public FFGLTouchEnginePluginBase
{
public:
	FFGLTouchEngine();
	~FFGLTouchEngine() override;

	//CFFGLPlugin
	FFResult InitGL(const FFGLViewportStruct* vp) override;
	FFResult ProcessOpenGL(ProcessOpenGLStruct* pGL) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter(unsigned int dwIndex, float value) override;
	FFResult SetTextParameter(unsigned int dwIndex, const char* value) override;

	float GetFloatParameter(unsigned int index) override;
	char* GetTextParameter(unsigned int index) override;

private:
#ifdef __APPLE__
    MTL::Device *pDevice;
#endif

	//TouchEngine IO objects
#ifdef _WIN32
	TouchObject<TED3D11Texture> TEVideoInputD3D;
	TouchObject<TED3D11Texture> TEVideoOutputD3D;
#endif
	TouchObject<TETexture> TEVideoInputTexture;
	TouchObject<TETexture> TEVideoOutputTexture;
	TouchObject<TEFloatBuffer> TEAudioInFloatBuffer1;
	TouchObject<TEFloatBuffer> TEAudioInFloatBuffer2;


#ifdef _WIN32
	//Spout Configs
	std::string SpoutID;
	Spout OutputInterop;
	bool isInteropInitialized = false;
#endif


#ifdef _WIN32
	GLuint SpoutTexture = 0;
#endif
	void InitializeGlTexture(GLuint& texture, uint16_t width, uint16_t height, GLenum type);
	void ConstructBaseParameters();
	void ResetBaseParameters();


	bool LoadTEGraphicsContext(bool Reload);
	bool CreateInputTexture(int width, int height);
	bool LoadTEFile();

	void LoadTouchEngine();
	void ResumeTouchEngine();
	void GetAllParameters();
	void CreateIndividualParameter(const TouchObject<TELinkInfo>& linkInfo);
	void CreateParametersFromGroup(const TouchObject<TELinkInfo>& linkInfo);
	void ClearTouchInstance();
	void eventCallback(TEEvent event, TEResult result, int64_t start_time_value, int32_t start_time_scale, int64_t end_time_value, int32_t end_time_scale);
	void linkCallback(TELinkEvent event, const char* identifier);

	static void eventCallbackStatic(TEInstance* instance, TEEvent event, TEResult result, int64_t start_time_value, int32_t start_time_scale, int64_t end_time_value, int32_t end_time_scale, void* info);
	static void linkCallbackStatic(TEInstance* instance, TELinkEvent event, const char* identifier, void* info);
};
