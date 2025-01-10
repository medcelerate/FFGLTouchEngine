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

	bool CreateInputTexture(int width, int height);

	void ResumeTouchEngine() override;
	void GetAllParameters();
	void ClearTouchInstance() override;
};
