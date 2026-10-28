#pragma once

#ifdef _WIN32
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include "FFGL/FFGLSDK.h"
#include "TouchEngine/TouchObject.h"
#include "TouchEngine/TED3D.h"

// Sends the host's GL input texture to TouchEngine on Windows, without a
// TouchEngine graphics context or Spout.
//
// Each frame is blitted from GL into a D3D11 texture shared by handle with a
// keyed mutex (imported into GL as a memory object), taken and given back
// from GL with GL_EXT_win32_keyed_mutex, and set on the input link as a
// TED3DSharedTexture with the key TouchEngine should wait for. The D3D11
// device only allocates those textures; nothing is drawn with it. Two
// textures alternate so the host never writes the one TouchEngine is reading.
//
// Every call needs the host's GL context current.
class TEInputGL
{
public:
	TEInputGL() = default;
	TEInputGL(const TEInputGL&) = delete;
	TEInputGL& operator=(const TEInputGL&) = delete;

	// Copies 'texture' (a GL_TEXTURE_2D whose content is width x height) into
	// the next shared texture and sets it on 'link'. 'device' allocates the
	// shared textures. Returns false if the frame could not be sent; failures
	// are logged to the host once each. Rebinds 'hostFBO' when done.
	bool Send(TEInstance* instance, const std::string& link, ID3D11Device* device,
		GLuint texture, int width, int height, GLuint hostFBO);

	// Deletes the shared textures and GL objects. Call from DeInitGL and
	// whenever the instance is cleared.
	void Release();

private:
	struct Buffer {
		Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
		TouchObject<TED3DSharedTexture> shared;
		GLuint memory = 0;
		GLuint glTexture = 0;
		uint64_t key = 0; // the key the keyed mutex is next free at
	};

	bool Allocate(ID3D11Device* device, int width, int height, GLint hostFormat);
	void Delete(Buffer& buffer);
	bool Fail(const std::string& message);

	Buffer buffers[2];
	int next = 0;
	int bufferWidth = 0;
	int bufferHeight = 0;
	GLint bufferHostFormat = 0;
	bool checkedExtensions = false;
	bool hasExtensions = false;
	std::string lastError;
	GLuint readFBO = 0;
	GLuint drawFBO = 0;
};
#endif
