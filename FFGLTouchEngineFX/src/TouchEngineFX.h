#pragma once
#include <windows.h>
#include "FFGL/FFGLSDK.h"
#include <d3d11_4.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "user32.lib")   
#pragma comment(lib, "gdi32.lib")
#define WIN32_LEAN_AND_MEAN
#include <wrl.h>
#include <shobjidl.h>
#include "TouchEngine/TouchObject.h"
#include "TouchEngine/TEGraphicsContext.h"
#include "TouchEngine/TED3D11.h"
#include "SpoutGL/SpoutSender.h"
#include "Thumbnail.h"


class FFGLTouchEngineFX : public CFFGLPlugin
{
public:
	FFGLTouchEngineFX();
	~FFGLTouchEngineFX();

	//CFFGLPlugin
	FFResult InitGL(const FFGLViewportStruct* vp) override;
	FFResult ProcessOpenGL(ProcessOpenGLStruct* pGL) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter(unsigned int dwIndex, float value) override;
	FFResult SetTextParameter(unsigned int dwIndex, const char* value) override;

	float GetFloatParameter(unsigned int index) override;
	char* GetTextParameter(unsigned int index) override;

private:
	TouchObject<TEInstance> instance;
	Microsoft::WRL::ComPtr<ID3D11Device> D3DDevice;
	TouchObject<TED3D11Context> D3DContext;

	HANDLE dxInteropHandle = 0;

	Microsoft::WRL::ComPtr <ID3D11Texture2D> D3DTextureInput = nullptr;
	HANDLE InputSharedHandle = nullptr;
	HANDLE dxInteropInputObject = 0;

	Microsoft::WRL::ComPtr <ID3D11Texture2D> D3DTextureOutput = nullptr;
	HANDLE OutputSharedHandle = nullptr;
	HANDLE dxInteropOutputObject = 0;

	std::unordered_map<ID3D11Texture2D*, IDXGIKeyedMutex*> TextureMutexMap;
	std::unordered_map<int, IDXGIKeyedMutex*> MutexMap;

	std::atomic_bool isTouchEngineLoaded = false;
	std::atomic_bool isTouchEngineReady = false;
	std::atomic_bool isGraphicsContextLoaded = false;
	std::atomic_bool isTouchFrameBusy = false;
	uint64_t FrameCount = 0;

	//Touch file capabilities
	bool hasVideoInput = false;
	bool hasVideoOutput = false;
	bool isVideoFX = false;

	
	//TouchEngine parameters
	uint32_t MaxParamsByType = 0;
	uint32_t OffsetParamsByType = 0;
	std::set<FFUInt32> ActiveParams;
	std::vector<std::pair<std::string, FFUInt32>> Parameters;
	std::unordered_map<FFUInt32, FFUInt32> ParameterMapType;
	std::unordered_map<FFUInt32, int32_t> ParameterMapInt;
	std::unordered_map<FFUInt32, double> ParameterMapFloat;
	std::unordered_map<FFUInt32, std::string> ParameterMapString;
	std::unordered_map<FFUInt32, bool> ParameterMapBool;

	//Spout Configs
	std::string SpoutIDInput;
	std::string SpoutIDOutput;

	Spout InputInterop;
	Spout OutputInterop;

	uint32_t SpoutSenderID = 0;

	bool InputInteropInitialized = false;
	bool OutputInteropInitialized = false;


	int InputWidth = 0;
	int InputHeight = 0;
	int OutputWidth = 0;
	int OutputHeight = 0;


	std::string FilePath;


	ffglex::FFGLShader shader;  //!< Utility to help us compile and link some shaders into a program.
	ffglex::FFGLScreenQuad quad;//!< Utility to help us render a full screen quad.
	GLuint SpoutTextureOutput = 0;
	GLuint SpoutTextureInput = 0;
	void InitializeGlTexture(GLuint &texture, uint16_t width, uint16_t height);
	void ConstructBaseParameters();
	void ResetBaseParameters();


	bool LoadTEGraphicsContext(bool Reload);
	bool CreateInputTexture(int width, int height, DXGI_FORMAT dxformat);
	bool CreateOutputTexture(int width, int height, DXGI_FORMAT dxformat);
	bool LoadTEFile();

	void LoadTouchEngine();
	void ResumeTouchEngine();
	void GetAllParameters();
	void ClearTouchInstance();
	void eventCallback(TEEvent event, TEResult result, int64_t start_time_value, int32_t start_time_scale, int64_t end_time_value, int32_t end_time_scale);
	void linkCallback(TELinkEvent event, const char* identifier);

	static void eventCallbackStatic(TEInstance* instance, TEEvent event, TEResult result, int64_t start_time_value, int32_t start_time_scale, int64_t end_time_value, int32_t end_time_scale, void* info);
	static void linkCallbackStatic(TEInstance* instance, TELinkEvent event, const char* identifier, void* info);
};
