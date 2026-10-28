#include "TEOutputGL.h"

#ifdef _WIN32
#include <algorithm>

namespace {

// How long to wait for TouchEngine to give an output texture back before
// dropping the frame. The D3D path waited INFINITE, which hung the host's
// render thread whenever TouchEngine never released the texture.
constexpr DWORD AcquireTimeoutMs = 100;

// TouchEngine cycles through a few output textures. Imports beyond this are
// dropped oldest first, which bounds how many of its textures are kept alive.
constexpr size_t MaxImports = 4;

// A BGRA texture imported as GL_RGBA8 reads back with red and blue exchanged:
// GL takes the bytes in its own RGBA order. The spike's channels section saw
// this on NVIDIA 591.86 with TouchDesigner 2023 and 2025 (D3D 33,89,240 read
// by GL as 240,89,33), so the output texture swizzles them back.
constexpr bool BGRAImportSwapsRedBlue = true;

struct GLFormat {
	GLenum internalFormat = 0;
	bool bgra = false;
};

GLFormat ToGLFormat(DXGI_FORMAT format)
{
	switch (format) {
	case DXGI_FORMAT_B8G8R8A8_UNORM: return { GL_RGBA8, true };
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return { GL_SRGB8_ALPHA8, true };
	case DXGI_FORMAT_R8G8B8A8_UNORM: return { GL_RGBA8 };
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return { GL_SRGB8_ALPHA8 };
	case DXGI_FORMAT_R10G10B10A2_UNORM: return { GL_RGB10_A2 };
	case DXGI_FORMAT_R16G16B16A16_UNORM: return { GL_RGBA16 };
	case DXGI_FORMAT_R16G16B16A16_FLOAT: return { GL_RGBA16F };
	case DXGI_FORMAT_R32G32B32A32_FLOAT: return { GL_RGBA32F };
	case DXGI_FORMAT_R8G8_UNORM: return { GL_RG8 };
	case DXGI_FORMAT_R16G16_UNORM: return { GL_RG16 };
	case DXGI_FORMAT_R16G16_FLOAT: return { GL_RG16F };
	case DXGI_FORMAT_R32G32_FLOAT: return { GL_RG32F };
	case DXGI_FORMAT_R8_UNORM: return { GL_R8 };
	case DXGI_FORMAT_R16_UNORM: return { GL_R16 };
	case DXGI_FORMAT_R16_FLOAT: return { GL_R16F };
	case DXGI_FORMAT_R32_FLOAT: return { GL_R32F };
	default: return {};
	}
}

}

bool TEOutputGL::Receive(TEInstance* instance, const TouchObject<TETexture>& texture, GLuint hostFBO)
{
	if (!checkedExtensions) {
		checkedExtensions = true;
		hasExtensions = GLEW_EXT_memory_object && GLEW_EXT_memory_object_win32 && GLEW_EXT_win32_keyed_mutex;
		if (!hasExtensions) {
			FFGLLog::LogToHost("FFGLTouchEngine: the GPU driver lacks GL_EXT_memory_object_win32 or GL_EXT_win32_keyed_mutex, so TouchEngine output cannot be shown");
		}
	}
	if (!hasExtensions) {
		return false;
	}
	AdoptCurrentContext();
	if (TETextureGetType(texture) != TETextureTypeD3DShared) {
		return Fail("TouchEngine output is not a D3D shared texture");
	}

	Import* import = FindOrImport(texture);
	if (import == nullptr || !EnsureOutput(*import)) {
		return false;
	}

	// Without a pending transfer there is no key to wait on. The FX plugin
	// used to guess key 0, which waits out the timeout every frame if the
	// mutex is on any other key; keep the last frame instead.
	if (!TEInstanceHasTextureTransfer(instance, texture)) {
		return false;
	}
	TouchObject<TESemaphore> semaphore;
	uint64_t waitValue = 0;
	TEResult result = TEInstanceGetTextureTransfer(instance, texture, semaphore.take(), &waitValue);
	if (result != TEResultSuccess) {
		return Fail(std::string("Failed to get the output texture transfer: ") + TEResultGetDescription(result));
	}
	if (semaphore != nullptr) {
		return Fail("TouchEngine synchronises its output with a semaphore, not a keyed mutex");
	}

	if (!glAcquireKeyedMutexWin32EXT(import->memory, waitValue, AcquireTimeoutMs)) {
		return Fail("Timed out waiting for TouchEngine to release its output texture");
	}

	// A raw copy: the host's scissor would clip it and sRGB conversion would
	// change it.
	GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
	GLboolean srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_FRAMEBUFFER_SRGB);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, readFBO);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, import->glTexture, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFBO);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output, 0);
	// D3D textures start at the top row and GL textures at the bottom one.
	bool flip = TETextureGetOrigin(texture) == TETextureOriginTopLeft;
	int w = import->width, h = import->height;
	glBlitFramebuffer(0, 0, w, h, 0, flip ? h : 0, w, flip ? 0 : h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer(GL_FRAMEBUFFER, hostFBO);

	if (scissor) glEnable(GL_SCISSOR_TEST);
	if (srgb) glEnable(GL_FRAMEBUFFER_SRGB);

	glReleaseKeyedMutexWin32EXT(import->memory, waitValue + 1);
	// The release is queued behind the blit; flush so TouchEngine can take
	// the texture back without waiting for the host's next flush.
	glFlush();

	// A null semaphore tells TouchEngine to take the keyed mutex at this key.
	result = TEInstanceAddTextureTransfer(instance, texture, nullptr, waitValue + 1);
	if (result != TEResultSuccess) {
		return Fail(std::string("Failed to hand the output texture back to TouchEngine: ") + TEResultGetDescription(result));
	}

	lastError.clear();
	return true;
}

TEOutputGL::Import* TEOutputGL::FindOrImport(const TouchObject<TETexture>& texture)
{
	frameCount++;
	const TED3DSharedTexture* shared = static_cast<const TED3DSharedTexture*>(texture.get());
	HANDLE handle = TED3DSharedTextureGetHandle(shared);
	for (Import& import : imports) {
		if (import.handle == handle) {
			import.texture = texture;
			import.lastUsed = frameCount;
			return &import;
		}
	}

	TED3DHandleType handleType = TED3DSharedTextureGetHandleType(shared);
	GLenum memoryType = handleType == TED3DHandleTypeD3D11Global ? GL_HANDLE_TYPE_D3D11_IMAGE_KMT_EXT :
		handleType == TED3DHandleTypeD3D11NT ? GL_HANDLE_TYPE_D3D11_IMAGE_EXT : 0;
	if (memoryType == 0) {
		Fail("TouchEngine output uses a D3D12 handle, which the GL import does not take");
		return nullptr;
	}

	int width = (int)TED3DSharedTextureGetWidth(shared);
	int height = (int)TED3DSharedTextureGetHeight(shared);
	DXGI_FORMAT format = TED3DSharedTextureGetFormat(shared);
	if (width == 0 || height == 0 || format == DXGI_FORMAT_UNKNOWN) {
		Fail("TouchEngine reported no size or format for its output; this TouchDesigner build is too old");
		return nullptr;
	}
	GLFormat glFormat = ToGLFormat(format);
	if (glFormat.internalFormat == 0) {
		Fail("TouchEngine output format DXGI " + std::to_string((int)format) + " has no GL equivalent");
		return nullptr;
	}

	if (imports.size() >= MaxImports) {
		auto oldest = std::min_element(imports.begin(), imports.end(),
			[](const Import& a, const Import& b) { return a.lastUsed < b.lastUsed; });
		Delete(*oldest);
		imports.erase(oldest);
	}

	Import import;
	import.handle = handle;
	import.texture = texture;
	import.width = width;
	import.height = height;
	import.internalFormat = glFormat.internalFormat;
	import.swapRedBlue = glFormat.bgra && BGRAImportSwapsRedBlue;
	import.lastUsed = frameCount;

	while (glGetError() != GL_NO_ERROR) {}
	glCreateMemoryObjectsEXT(1, &import.memory);
	GLint dedicated = GL_TRUE;
	glMemoryObjectParameterivEXT(import.memory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
	// Size 0: for D3D11 image handles the driver takes the size from the
	// resource. A nonzero size imports without error but then makes
	// glTexStorageMem2DEXT fail with GL_INVALID_VALUE (NVIDIA 591.86).
	glImportMemoryWin32HandleEXT(import.memory, 0, memoryType, handle);

	GLint previousTexture = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
	glGenTextures(1, &import.glTexture);
	glBindTexture(GL_TEXTURE_2D, import.glTexture);
	glTexStorageMem2DEXT(GL_TEXTURE_2D, 1, import.internalFormat, width, height, import.memory, 0);
	glBindTexture(GL_TEXTURE_2D, previousTexture);

	GLenum error = glGetError();
	if (error != GL_NO_ERROR) {
		Delete(import);
		char code[16];
		snprintf(code, sizeof code, "0x%04x", error);
		Fail(std::string("Failed to import TouchEngine's output texture into GL (GL error ") + code + ")");
		return nullptr;
	}

	imports.push_back(std::move(import));
	return &imports.back();
}

bool TEOutputGL::EnsureOutput(const Import& import)
{
	if (readFBO == 0) glGenFramebuffers(1, &readFBO);
	if (drawFBO == 0) glGenFramebuffers(1, &drawFBO);

	if (output != 0 && outputWidth == import.width && outputHeight == import.height
		&& outputFormat == import.internalFormat && outputSwapRedBlue == import.swapRedBlue) {
		return true;
	}

	if (output != 0) {
		glDeleteTextures(1, &output);
		output = 0;
	}

	GLint previousTexture = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
	glGenTextures(1, &output);
	glBindTexture(GL_TEXTURE_2D, output);
	glTexStorage2D(GL_TEXTURE_2D, 1, import.internalFormat, import.width, import.height);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	if (import.swapRedBlue) {
		// The blit copies bytes as they are; swap them back when sampled.
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
	}
	glBindTexture(GL_TEXTURE_2D, previousTexture);

	outputWidth = import.width;
	outputHeight = import.height;
	outputFormat = import.internalFormat;
	outputSwapRedBlue = import.swapRedBlue;
	return true;
}

void TEOutputGL::DropImports()
{
	for (Import& import : imports) {
		Delete(import);
	}
	imports.clear();
	lastError.clear();
}

void TEOutputGL::Release()
{
	bool owned = OwnsCurrentContext();
	bool hadObjects = !imports.empty() || output != 0 || readFBO != 0 || drawFBO != 0;
	DropImports();
	if (owned && output != 0) glDeleteTextures(1, &output);
	if (owned && readFBO != 0) glDeleteFramebuffers(1, &readFBO);
	if (owned && drawFBO != 0) glDeleteFramebuffers(1, &drawFBO);
	output = 0;
	readFBO = 0;
	drawFBO = 0;
	outputWidth = 0;
	outputHeight = 0;
	outputFormat = 0;
	outputSwapRedBlue = false;
	if (!owned && hadObjects) {
		// Left for the context that owns them to free when it is destroyed.
		FFGLLog::LogToHost("FFGLTouchEngine: GL cleanup ran without the plugin's GL context current; its GL objects were left alone");
	}
	glContext = nullptr;
}

bool TEOutputGL::OwnsCurrentContext() const
{
	return glContext != nullptr && wglGetCurrentContext() == glContext;
}

void TEOutputGL::AdoptCurrentContext()
{
	HGLRC current = wglGetCurrentContext();
	if (glContext != nullptr && current != glContext) {
		// The host renders on another context now: start again there.
		Release();
	}
	glContext = current;
}

void TEOutputGL::Delete(Import& import)
{
	bool owned = OwnsCurrentContext();
	if (owned && import.glTexture != 0) glDeleteTextures(1, &import.glTexture);
	if (owned && import.memory != 0) glDeleteMemoryObjectsEXT(1, &import.memory);
	import.glTexture = 0;
	import.memory = 0;
	import.texture.reset();
}

bool TEOutputGL::Fail(const std::string& message)
{
	if (message != lastError) {
		lastError = message;
		FFGLLog::LogToHost(("FFGLTouchEngine: " + message).c_str());
	}
	return false;
}
#endif
