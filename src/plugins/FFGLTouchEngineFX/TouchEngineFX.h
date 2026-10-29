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
#ifdef __APPLE__
	GLuint OutputTextureGL = 0;
	id<MTLTexture> OutputMetalTexture = nil;
	IOSurfaceRef OutputIOSurface = nullptr;
	IOSurfaceRef InputIOSurface = nullptr;
	id<MTLTexture> InputMetalTexture = nil;
	GLuint InputIOSurfaceGL = 0;
	GLuint InputRenderFBO = 0;
	ffglex::FFGLShader rectShader;
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
