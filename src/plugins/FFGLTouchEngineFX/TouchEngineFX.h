#pragma once

#ifdef _WIN32
#include <windows.h>
#include <d3d11_4.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "user32.lib")   
#pragma comment(lib, "gdi32.lib")
#define WIN32_LEAN_AND_MEAN
#include <wrl.h>
#include <shobjidl.h>
#endif

#ifdef __APPLE__
#define NS_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#endif

#include "FFGL/FFGLSDK.h"
#include "TouchEngine/TouchObject.h"

#ifdef _WIN32
#include "TouchEngine/TED3D11.h"
#include "SpoutGL/SpoutSender.h"
#endif

#include "Thumbnail.h"

typedef struct {
	std::string identifier;
	uint8_t count;
	FFUInt32 children[4];
} VectorParameterInfo;

class FFGLTouchEngineFX : public CFFGLPlugin
{
public:
	FFGLTouchEngineFX();
	~FFGLTouchEngineFX();
    
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
	TouchObject<TEInstance> instance;
#ifdef _WIN32
	Microsoft::WRL::ComPtr<ID3D11Device> D3DDevice;
	TouchObject<TED3D11Context> D3DContext;

	HANDLE dxInteropHandle = 0;

	DXGI_FORMAT DXFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
#endif
	GLint GLFormat = 0;
	
#ifdef _WIN32
	Microsoft::WRL::ComPtr <ID3D11Texture2D> D3DTextureInput = nullptr;
	HANDLE InputSharedHandle = nullptr;
	HANDLE dxInteropInputObject = 0;

	Microsoft::WRL::ComPtr <ID3D11Texture2D> D3DTextureOutput = nullptr;
	HANDLE OutputSharedHandle = nullptr;
	HANDLE dxInteropOutputObject = 0;

	std::unordered_map<ID3D11Texture2D*, IDXGIKeyedMutex*> TextureMutexMap;
	std::unordered_map<int, IDXGIKeyedMutex*> MutexMap;
#endif

	std::atomic_bool isTouchEngineLoaded;
	std::atomic_bool isTouchEngineReady;
	std::atomic_bool isGraphicsContextLoaded;
	std::atomic_bool isTouchFrameBusy;
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
	
	//First check if it is a vector parameter

	std::set<FFUInt32> ActiveVectorParams;
	std::vector<VectorParameterInfo> VectorParameters;



	//Texture Name
	std::string OutputOpName;

#ifdef _WIN32
	//Spout Configs
	std::string SpoutIDInput;
	std::string SpoutIDOutput;

	Spout InputInterop;
	Spout OutputInterop;

	uint32_t SpoutSenderID = 0;
#endif

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
	void InitializeGlTexture(GLuint &texture, uint16_t width, uint16_t height, GLenum type);
	void ConstructBaseParameters();
	void ResetBaseParameters();


	bool LoadTEGraphicsContext(bool Reload);
#ifdef _WIN32
	bool CreateInputTexture(int width, int height, DXGI_FORMAT dxformat);
	bool CreateOutputTexture(int width, int height, DXGI_FORMAT dxformat);
#endif
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
