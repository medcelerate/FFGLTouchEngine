#pragma once

#ifdef _WIN32
#include <windows.h>
#include <string>
#include <vector>
#include "FFGL/FFGLSDK.h"
#include "TouchEngine/TouchObject.h"
#include "TouchEngine/TED3D.h"

// Receives TouchEngine's output textures in the host's GL context on Windows,
// without a D3D11 device, a TouchEngine graphics context or Spout.
//
// TouchEngine hands each output frame over as a D3D shared texture guarded by a
// keyed mutex. Its handle is imported into GL as a memory object
// (GL_EXT_memory_object_win32) and the keyed mutex is taken and given back from
// GL (GL_EXT_win32_keyed_mutex). While it is held, the frame is blitted into a
// texture this class owns, flipped to GL's bottom-left origin, so the plugin
// can keep drawing the last frame while TouchEngine cooks the next one.
//
// Every call needs the host's GL context current.
class TEOutputGL
{
public:
	TEOutputGL() = default;
	TEOutputGL(const TEOutputGL&) = delete;
	TEOutputGL& operator=(const TEOutputGL&) = delete;

	// Copies 'texture', TouchEngine's current output, into Texture() and hands
	// it back to TouchEngine. Returns false if the frame could not be taken, in
	// which case Texture() still holds the previous frame. Failures are logged
	// to the host once each, not every frame. Rebinds 'hostFBO' when done.
	bool Receive(TEInstance* instance, const TouchObject<TETexture>& texture, GLuint hostFBO);

	// Drops the cached imports, which keep TouchEngine's textures alive, but
	// keeps the last frame. Call whenever the instance is cleared.
	void DropImports();

	// Deletes every GL object, the last frame included. Call from DeInitGL.
	void Release();

	GLuint Texture() const { return output; }
	int Width() const { return outputWidth; }
	int Height() const { return outputHeight; }

private:
	// One imported TouchEngine texture. TouchEngine cycles through a few output
	// textures, so imports are kept and reused rather than made every frame.
	struct Import {
		HANDLE handle = nullptr;
		TouchObject<TETexture> texture; // keeps the handle valid while cached
		GLuint memory = 0;
		GLuint glTexture = 0;
		int width = 0;
		int height = 0;
		GLenum internalFormat = 0;
		bool swapRedBlue = false;
		uint64_t lastUsed = 0;
	};

	Import* FindOrImport(const TouchObject<TETexture>& texture);
	bool EnsureOutput(const Import& import);
	void Delete(Import& import);
	bool Fail(const std::string& message);

	std::vector<Import> imports;
	uint64_t frameCount = 0;
	bool checkedExtensions = false;
	bool hasExtensions = false;
	std::string lastError;

	GLuint output = 0;
	int outputWidth = 0;
	int outputHeight = 0;
	GLenum outputFormat = 0;
	bool outputSwapRedBlue = false;
	GLuint readFBO = 0;
	GLuint drawFBO = 0;
};
#endif
