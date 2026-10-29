#include <TFE_RenderBackend/shaderBuffer.h>
#include "gl.h"
#include <memory.h>
#include "openGL_Caps.h"

GLenum getFormat(const ShaderBufferDef& bufferDef);

ShaderBuffer::~ShaderBuffer()
{
	destroy();
}

#ifdef TFE_GLES
// OpenGL ES 3.0 (iOS, ANGLE on Metal) has no texture buffers. Each buffer is a
// 2D texture SHADER_BUFFER_TEX_WIDTH texels wide instead, which the shaders
// read with texelFetch() at (i % width, i / width) (see shader.cpp). Updates
// go to the next of a few textures in turn, like orphaning a buffer: Metal
// would otherwise wait for the GPU to finish drawing with the texture before
// writing to it.
static void getUploadFormat(GLenum internalFormat, GLenum* format, GLenum* type)
{
	switch (internalFormat)
	{
		case GL_R8:      *format = GL_RED;          *type = GL_UNSIGNED_BYTE; break;
		case GL_RG8:     *format = GL_RG;           *type = GL_UNSIGNED_BYTE; break;
		case GL_RGBA8:   *format = GL_RGBA;         *type = GL_UNSIGNED_BYTE; break;
		case GL_R32UI:   *format = GL_RED_INTEGER;  *type = GL_UNSIGNED_INT;  break;
		case GL_RG32UI:  *format = GL_RG_INTEGER;   *type = GL_UNSIGNED_INT;  break;
		case GL_RGBA32UI:*format = GL_RGBA_INTEGER; *type = GL_UNSIGNED_INT;  break;
		case GL_R32I:    *format = GL_RED_INTEGER;  *type = GL_INT;           break;
		case GL_RG32I:   *format = GL_RG_INTEGER;   *type = GL_INT;           break;
		case GL_RGBA32I: *format = GL_RGBA_INTEGER; *type = GL_INT;           break;
		case GL_R32F:    *format = GL_RED;          *type = GL_FLOAT;         break;
		case GL_RG32F:   *format = GL_RG;           *type = GL_FLOAT;         break;
		default:         *format = GL_RGBA;         *type = GL_FLOAT;         break;	// GL_RGBA32F
	}
}

// Writes the first 'count' elements of 'data' into the texture currently bound
// to GL_TEXTURE_2D: whole rows, then the start of the next one.
static void uploadElements(const u8* data, u32 count, u32 stride, GLenum format, GLenum type)
{
	if (!data || !count) { return; }
	GLint unpackAlignment = 4;
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackAlignment);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	const u32 rows = count >> SHADER_BUFFER_TEX_WIDTH_SHIFT;
	const u32 rest = count & (SHADER_BUFFER_TEX_WIDTH - 1);
	if (rows)
	{
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SHADER_BUFFER_TEX_WIDTH, rows, format, type, data);
	}
	if (rest)
	{
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, rows, rest, 1, format, type, data + size_t(rows) * SHADER_BUFFER_TEX_WIDTH * stride);
	}
	glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlignment);
}

bool ShaderBuffer::create(u32 count, const ShaderBufferDef& bufferDef, bool dynamic, void* initData)
{
	if (!count) { return false; }
	GLenum internalFormat = getFormat(bufferDef);
	if (internalFormat == GL_INVALID_ENUM)
	{
		return false;
	}
	m_initialized = true;

	m_bufferDef = bufferDef;
	m_stride  = m_bufferDef.channelCount * m_bufferDef.channelSize;
	m_count   = count;
	m_size    = m_stride * m_count;
	m_dynamic = dynamic;
	m_texHeight = (count + SHADER_BUFFER_TEX_WIDTH - 1) >> SHADER_BUFFER_TEX_WIDTH_SHIFT;
	GLenum format, type;
	getUploadFormat(internalFormat, &format, &type);
	m_glFormat = format;
	m_glType = type;

	const u32 textureCount = dynamic ? RING_SIZE : 1;
	for (u32 i = 0; i < RING_SIZE; i++) { m_ring[i] = 0; }
	glGenTextures(textureCount, m_ring);
	for (u32 i = 0; i < textureCount; i++)
	{
		glBindTexture(GL_TEXTURE_2D, m_ring[i]);
		// Integer and 32-bit float textures can only be read unfiltered.
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		glTexStorage2D(GL_TEXTURE_2D, 1, internalFormat, SHADER_BUFFER_TEX_WIDTH, m_texHeight);
		uploadElements((const u8*)initData, count, m_stride, format, type);
	}
	glBindTexture(GL_TEXTURE_2D, 0);
	m_ringIndex = 0;
	m_gpuHandle[0] = m_ring[0];
	m_gpuHandle[1] = m_ring[0];
	return true;
}

void ShaderBuffer::destroy()
{
	if (m_initialized)
	{
		for (u32 i = 0; i < RING_SIZE; i++)
		{
			if (m_ring[i]) { glDeleteTextures(1, &m_ring[i]); }
			m_ring[i] = 0;
		}
	}
	m_gpuHandle[0] = 0;
	m_gpuHandle[1] = 0;
	m_initialized = false;
}

void ShaderBuffer::update(const void* buffer, size_t size)
{
	if (!m_initialized) { return; }
	u32 count = u32(size / m_stride);
	if (count > m_count) { count = m_count; }	// a texture can't grow like a buffer.

	if (m_dynamic)
	{
		m_ringIndex = (m_ringIndex + 1) % RING_SIZE;
		m_gpuHandle[0] = m_ring[m_ringIndex];
		m_gpuHandle[1] = m_ring[m_ringIndex];
	}
	glBindTexture(GL_TEXTURE_2D, m_gpuHandle[1]);
	uploadElements((const u8*)buffer, count, m_stride, m_glFormat, m_glType);
	glBindTexture(GL_TEXTURE_2D, 0);
}

void ShaderBuffer::bind(s32 bindPoint) const
{
	if (bindPoint < 0) { return; }
	glActiveTexture(GL_TEXTURE0 + bindPoint);
	glBindTexture(GL_TEXTURE_2D, m_gpuHandle[1]);
}

void ShaderBuffer::unbind(s32 bindPoint) const
{
	if (bindPoint < 0) { return; }
	glActiveTexture(GL_TEXTURE0 + bindPoint);
	glBindTexture(GL_TEXTURE_2D, 0);
}

s32 ShaderBuffer::getMaxSize()
{
	return OpenGL_Caps::getMaxTextureBufferSize();
}

#else
bool ShaderBuffer::create(u32 count, const ShaderBufferDef& bufferDef, bool dynamic, void* initData)
{
	if (!count) { return false; }
	GLenum internalFormat = getFormat(bufferDef);
	if (internalFormat == GL_INVALID_ENUM)
	{
		return false;
	}
	m_initialized = true;

	// Track shader buffer attributes.
	m_bufferDef = bufferDef;
	m_stride  = m_bufferDef.channelCount * m_bufferDef.channelSize;
	m_count   = count;
	m_size    = m_stride * m_count;
	m_dynamic = dynamic;
	
	// Build the GPU buffer and copy the initial data.
	glGenBuffers(1, &m_gpuHandle[0]);
	glBindBuffer(GL_TEXTURE_BUFFER, m_gpuHandle[0]);
	glBufferData(GL_TEXTURE_BUFFER, m_size, initData, dynamic ? GL_STREAM_DRAW : GL_STATIC_DRAW);
	glBindBuffer(GL_TEXTURE_BUFFER, 0);

	glGenTextures(1, &m_gpuHandle[1]);
	glBindTexture(GL_TEXTURE_BUFFER, m_gpuHandle[1]);
	glTexBuffer(GL_TEXTURE_BUFFER, internalFormat, m_gpuHandle[0]);
	glBindTexture(GL_TEXTURE_BUFFER, 0);
	
	return true;
}

void ShaderBuffer::destroy()
{
	if (m_initialized)
	{
		if (m_gpuHandle[1]) { glDeleteTextures(1, &m_gpuHandle[1]); }
		if (m_gpuHandle[0]) { glDeleteBuffers(1, &m_gpuHandle[0]); }
	}
	m_gpuHandle[0] = 0;
	m_gpuHandle[1] = 0;
	m_initialized = false;
}

void ShaderBuffer::update(const void* buffer, size_t size)
{
	glBindBuffer(GL_TEXTURE_BUFFER, m_gpuHandle[0]);
	glBufferData(GL_TEXTURE_BUFFER, size, buffer, m_dynamic ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
	glBindBuffer(GL_TEXTURE_BUFFER, 0);
}

void ShaderBuffer::bind(s32 bindPoint) const
{
	if (bindPoint < 0) { return; }
	glActiveTexture(GL_TEXTURE0 + bindPoint);
	glBindTexture(GL_TEXTURE_BUFFER, m_gpuHandle[1]);
}

void ShaderBuffer::unbind(s32 bindPoint) const
{
	if (bindPoint < 0) { return; }
	glActiveTexture(GL_TEXTURE0 + bindPoint);
	glBindTexture(GL_TEXTURE_BUFFER, 0);
}

s32 ShaderBuffer::getMaxSize()
{
	return OpenGL_Caps::getMaxTextureBufferSize();
}

#endif

GLenum getFormat(const ShaderBufferDef& bufferDef)
{
	if (bufferDef.channelCount == 1)
	{
		if (bufferDef.channelSize == 1)
		{
			return GL_R8;
		}
		else if (bufferDef.channelSize == 2)
		{
		#ifdef TFE_GLES
			return GL_INVALID_ENUM;	// no 16-bit normalized formats in ES 3.0
		#else
			return GL_R16;
		#endif
		}
		else if (bufferDef.channelSize == 4)
		{
			if (bufferDef.channelType == BUF_CHANNEL_UINT)
			{
				return GL_R32UI;
			}
			else if (bufferDef.channelType == BUF_CHANNEL_INT)
			{
				return GL_R32I;
			}
			else if (bufferDef.channelType == BUF_CHANNEL_FLOAT)
			{
				return GL_R32F;
			}
		}
	}
	else if (bufferDef.channelCount == 2)
	{
		if (bufferDef.channelSize == 1)
		{
			return GL_RG8;
		}
		else if (bufferDef.channelSize == 2)
		{
		#ifdef TFE_GLES
			return GL_INVALID_ENUM;	// no 16-bit normalized formats in ES 3.0
		#else
			return GL_RG16;
		#endif
		}
		else if (bufferDef.channelSize == 4)
		{
			if (bufferDef.channelType == BUF_CHANNEL_UINT)
			{
				return GL_RG32UI;
			}
			else if (bufferDef.channelType == BUF_CHANNEL_INT)
			{
				return GL_RG32I;
			}
			else if (bufferDef.channelType == BUF_CHANNEL_FLOAT)
			{
				return GL_RG32F;
			}
		}
	}
	else if (bufferDef.channelCount == 4)
	{
		if (bufferDef.channelSize == 1)
		{
			return GL_RGBA8;
		}
		else if (bufferDef.channelSize == 2)
		{
		#ifdef TFE_GLES
			return GL_INVALID_ENUM;	// no 16-bit normalized formats in ES 3.0
		#else
			return GL_RGBA16;
		#endif
		}
		else if (bufferDef.channelSize == 4)
		{
			if (bufferDef.channelType == BUF_CHANNEL_UINT)
			{
				return GL_RGBA32UI;
			}
			else if (bufferDef.channelType == BUF_CHANNEL_INT)
			{
				return GL_RGBA32I;
			}
			else if (bufferDef.channelType == BUF_CHANNEL_FLOAT)
			{
				return GL_RGBA32F;
			}
		}
	}
	return GL_INVALID_ENUM;
}