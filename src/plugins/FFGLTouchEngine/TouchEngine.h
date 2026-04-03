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

private:
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
	std::string SpoutIDOutput;
	Spout OutputInterop;
	bool OutputInteropInitialized = false;
	GLuint SpoutTextureOutput = 0;
#endif

#ifdef __APPLE__
	GLuint OutputTextureGL = 0;
	id<MTLTexture> OutputMetalTexture = nil;
	IOSurfaceRef OutputIOSurface = nullptr;
	ffglex::FFGLShader rectShader;
#endif

	bool CreateInputTexture(int width, int height);

	void ResumeTouchEngine() override;
	void ClearTouchInstance() override;

	void HandleOperatorLink(const TouchObject<TELinkInfo>& linkInfo) override;
};
