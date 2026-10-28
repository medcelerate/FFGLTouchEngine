#include "TEInputGL.h"

#ifdef _WIN32

namespace {

// How long to wait for TouchEngine to give an input texture back before
// dropping the frame. With two textures alternating it has had a whole frame.
constexpr DWORD AcquireTimeoutMs = 100;

struct SharedFormat {
	DXGI_FORMAT dxgi;
	GLenum gl;
};

// The shared texture's format for a host texture's GL internal format. RGBA
// byte order on both sides, so unlike the output no swizzle is needed: the
// spike's fxinput section read GL-written orange back through D3D unchanged.
SharedFormat ToSharedFormat(GLint hostFormat)
{
	switch (hostFormat) {
	case GL_RGBA16F: return { DXGI_FORMAT_R16G16B16A16_FLOAT, GL_RGBA16F };
	case GL_RGBA32F: return { DXGI_FORMAT_R32G32B32A32_FLOAT, GL_RGBA32F };
	case GL_RGBA16: return { DXGI_FORMAT_R16G16B16A16_UNORM, GL_RGBA16 };
	default: return { DXGI_FORMAT_R8G8B8A8_UNORM, GL_RGBA8 };
	}
}

}

bool TEInputGL::Send(TEInstance* instance, const std::string& link, ID3D11Device* device,
	GLuint texture, int width, int height, GLuint hostFBO)
{
	if (!checkedExtensions) {
		checkedExtensions = true;
		hasExtensions = GLEW_EXT_memory_object && GLEW_EXT_memory_object_win32 && GLEW_EXT_win32_keyed_mutex;
		if (!hasExtensions) {
			FFGLLog::LogToHost("FFGLTouchEngine: the GPU driver lacks GL_EXT_memory_object_win32 or GL_EXT_win32_keyed_mutex, so input cannot be sent to TouchEngine");
		}
	}
	if (!hasExtensions) {
		return false;
	}
	if (device == nullptr) {
		return Fail("No D3D11 device to allocate input textures");
	}
	AdoptCurrentContext();

	GLint previousTexture = 0;
	GLint hostFormat = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &hostFormat);
	glBindTexture(GL_TEXTURE_2D, previousTexture);

	if (buffers[0].glTexture == 0 || width != bufferWidth || height != bufferHeight || hostFormat != bufferHostFormat) {
		if (!Allocate(device, width, height, hostFormat)) {
			return false;
		}
	}

	Buffer& buffer = buffers[next];

	// TouchEngine sets no transfer on input it has read (it gives the keyed
	// mutex back at the key it took), so the key last handed over is the one
	// to wait for. Prefer TouchEngine's if it ever does set one.
	uint64_t key = buffer.key;
	if (TEInstanceHasTextureTransfer(instance, buffer.shared)) {
		TouchObject<TESemaphore> semaphore;
		uint64_t waitValue = 0;
		if (TEInstanceGetTextureTransfer(instance, buffer.shared, semaphore.take(), &waitValue) == TEResultSuccess
			&& semaphore == nullptr) {
			key = waitValue;
		}
	}

	if (!glAcquireKeyedMutexWin32EXT(buffer.memory, key, AcquireTimeoutMs)) {
		return Fail("Timed out waiting for TouchEngine to release an input texture");
	}

	// A raw copy: the host's scissor would clip it and sRGB conversion would
	// change it.
	GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
	GLboolean srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_FRAMEBUFFER_SRGB);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, readFBO);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFBO);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, buffer.glTexture, 0);
	// GL's bottom row goes to the shared texture's top row, which is where
	// its declared top-left origin puts the first row.
	glBlitFramebuffer(0, 0, width, height, 0, height, width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer(GL_FRAMEBUFFER, hostFBO);

	if (scissor) glEnable(GL_SCISSOR_TEST);
	if (srgb) glEnable(GL_FRAMEBUFFER_SRGB);

	glReleaseKeyedMutexWin32EXT(buffer.memory, key + 1);
	// The release is queued behind the blit; flush so TouchEngine can take
	// the texture without waiting for the host's next flush.
	glFlush();
	buffer.key = key + 1;

	// A null semaphore tells TouchEngine to take the keyed mutex at this key.
	// The transfer has to be given every time the texture is set again.
	TEResult result = TEInstanceAddTextureTransfer(instance, buffer.shared, nullptr, buffer.key);
	if (result != TEResultSuccess) {
		return Fail(std::string("Failed to hand the input texture to TouchEngine: ") + TEResultGetDescription(result));
	}
	result = TEInstanceLinkSetTextureValue(instance, link.c_str(), buffer.shared, nullptr);
	if (result != TEResultSuccess) {
		return Fail(std::string("Failed to set the input texture: ") + TEResultGetDescription(result));
	}

	next = 1 - next;
	lastError.clear();
	return true;
}

bool TEInputGL::Allocate(ID3D11Device* device, int width, int height, GLint hostFormat)
{
	Release();
	SharedFormat format = ToSharedFormat(hostFormat);

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = format.dxgi;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

	for (Buffer& buffer : buffers) {
		HANDLE handle = nullptr;
		Microsoft::WRL::ComPtr<IDXGIResource> resource;
		HRESULT hr = device->CreateTexture2D(&desc, nullptr, &buffer.texture);
		if (SUCCEEDED(hr)) hr = buffer.texture.As(&resource);
		if (SUCCEEDED(hr)) hr = resource->GetSharedHandle(&handle);
		if (FAILED(hr)) {
			Release();
			char code[16];
			snprintf(code, sizeof code, "0x%08lx", (unsigned long)hr);
			return Fail(std::string("Failed to create a shared input texture (") + code + ")");
		}

		buffer.shared.take(TED3DSharedTextureCreate(handle, TED3DHandleTypeD3D11Global, format.dxgi, width, height,
			TETextureOriginTopLeft, kTETextureComponentMapIdentity, nullptr, nullptr));

		while (glGetError() != GL_NO_ERROR) {}
		glCreateMemoryObjectsEXT(1, &buffer.memory);
		GLint dedicated = GL_TRUE;
		glMemoryObjectParameterivEXT(buffer.memory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
		// Size 0, as for the output: the driver takes it from the resource.
		glImportMemoryWin32HandleEXT(buffer.memory, 0, GL_HANDLE_TYPE_D3D11_IMAGE_KMT_EXT, handle);

		GLint previousTexture = 0;
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
		glGenTextures(1, &buffer.glTexture);
		glBindTexture(GL_TEXTURE_2D, buffer.glTexture);
		glTexStorageMem2DEXT(GL_TEXTURE_2D, 1, format.gl, width, height, buffer.memory, 0);
		glBindTexture(GL_TEXTURE_2D, previousTexture);

		GLenum error = glGetError();
		if (buffer.shared == nullptr || error != GL_NO_ERROR) {
			Release();
			char code[16];
			snprintf(code, sizeof code, "0x%04x", error);
			return Fail(std::string("Failed to import a shared input texture into GL (GL error ") + code + ")");
		}
	}

	// Make the textures visible to TouchEngine's device before it opens them.
	Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
	device->GetImmediateContext(&context);
	context->Flush();

	if (readFBO == 0) glGenFramebuffers(1, &readFBO);
	if (drawFBO == 0) glGenFramebuffers(1, &drawFBO);

	bufferWidth = width;
	bufferHeight = height;
	bufferHostFormat = hostFormat;
	next = 0;
	return true;
}

void TEInputGL::Release()
{
	bool owned = OwnsCurrentContext();
	bool hadObjects = buffers[0].glTexture != 0 || buffers[1].glTexture != 0 || readFBO != 0 || drawFBO != 0;
	for (Buffer& buffer : buffers) {
		Delete(buffer);
	}
	if (owned && readFBO != 0) glDeleteFramebuffers(1, &readFBO);
	if (owned && drawFBO != 0) glDeleteFramebuffers(1, &drawFBO);
	readFBO = 0;
	drawFBO = 0;
	bufferWidth = 0;
	bufferHeight = 0;
	bufferHostFormat = 0;
	next = 0;
	if (!owned && hadObjects) {
		// Left for the context that owns them to free when it is destroyed.
		FFGLLog::LogToHost("FFGLTouchEngine: GL cleanup ran without the plugin's GL context current; its GL objects were left alone");
	}
	glContext = nullptr;
}

bool TEInputGL::OwnsCurrentContext() const
{
	return glContext != nullptr && wglGetCurrentContext() == glContext;
}

void TEInputGL::AdoptCurrentContext()
{
	HGLRC current = wglGetCurrentContext();
	if (glContext != nullptr && current != glContext) {
		// The host renders on another context now: start again there.
		Release();
	}
	glContext = current;
}

void TEInputGL::Delete(Buffer& buffer)
{
	bool owned = OwnsCurrentContext();
	if (owned && buffer.glTexture != 0) glDeleteTextures(1, &buffer.glTexture);
	if (owned && buffer.memory != 0) glDeleteMemoryObjectsEXT(1, &buffer.memory);
	buffer.glTexture = 0;
	buffer.memory = 0;
	buffer.shared.reset();
	buffer.texture.Reset();
	buffer.key = 0;
}

bool TEInputGL::Fail(const std::string& message)
{
	if (message != lastError) {
		lastError = message;
		FFGLLog::LogToHost(("FFGLTouchEngine: " + message).c_str());
	}
	return false;
}
#endif
