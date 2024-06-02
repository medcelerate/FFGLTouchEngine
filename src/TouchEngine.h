#pragma once
#include <windows.h>
#include "FFGL/FFGLSDK.h"
#include <d3d11.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "user32.lib")   
#pragma comment(lib, "gdi32.lib")
#define WIN32_LEAN_AND_MEAN
#include <wrl.h>
#include <windows.h>
#include <shobjidl.h>
#include "TouchEngine/TouchObject.h"
#include "TouchEngine/TEGraphicsContext.h"
#include "TouchEngine/TED3D11.h"
#include "SpoutDX/SpoutDX.h"
#include "SpoutGL/SpoutReceiver.h"


class FFGLTouchEngine : public CFFGLPlugin
{
public:
	FFGLTouchEngine();

	//CFFGLPlugin
	FFResult InitGL(const FFGLViewportStruct* vp) override;
	FFResult ProcessOpenGL(ProcessOpenGLStruct* pGL) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter(unsigned int dwIndex, float value) override;

	//float GetFloatParameter(unsigned int index) override;

private:
	TouchObject<TEInstance> instance;
	Microsoft::WRL::ComPtr<ID3D11Device> D3DDevice;
	TouchObject<TED3D11Context> D3DContext;
	Microsoft::WRL::ComPtr <ID3D11Texture2D> D3DTextureInput = nullptr;
	Microsoft::WRL::ComPtr <ID3D11Texture2D> D3DTextureOutput = nullptr;

	bool isTouchEngineLoaded = false;
	bool isTouchEngineReady = false;
	std::atomic_bool isTouchFrameBusy = false;
	uint64_t FrameCount = 0;

	//Touch file capabilities
	bool hasVideoInput = false;
	bool hasAudioInput = false;
	bool hasVideoOutput = false;
	bool isVideoFX = false;

	//TouchEngine IO objects
	TouchObject<TED3D11Texture> TEVideoInputD3D;
	TouchObject<TED3D11Texture> TEVideoOutputD3D;
	TouchObject<TETexture> TEVideoInputTexture;
	TouchObject<TETexture> TEVideoOutputTexture;
	TouchObject<TEFloatBuffer> TEAudioInFloatBuffer1;
	TouchObject<TEFloatBuffer> TEAudioInFloatBuffer2;
	
	//TouchEngine parameters
	std::vector<std::pair<std::string, FFUInt32>> Parameters;

	std::string SpoutID;
	SpoutReceiver SPReceiver;
	spoutDX SPSender;



	int Width = 0;
	int Height = 0;


	std::string FilePath;


	ffglex::FFGLShader shader;  //!< Utility to help us compile and link some shaders into a program.
	ffglex::FFGLScreenQuad quad;//!< Utility to help us render a full screen quad.

	bool LoadTEGraphicsContext(bool Reload);
	bool CreateInputTexture(int width, int height);
	bool LoadTEFile();
	bool OpenFileDialog();
	void LoadTouchEngine();
	void ResumeTouchEngine();
	void GetAllParameters();
	void eventCallback(TEEvent event, TEResult result, int64_t start_time_value, int32_t start_time_scale, int64_t end_time_value, int32_t end_time_scale);
	void linkCallback(TELinkEvent event, const char* identifier);

	static void eventCallbackStatic(TEInstance* instance, TEEvent event, TEResult result, int64_t start_time_value, int32_t start_time_scale, int64_t end_time_value, int32_t end_time_scale, void* info);
	static void linkCallbackStatic(TEInstance* instance, TELinkEvent event, const char* identifier, void* info);
};
