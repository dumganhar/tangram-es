#include "texture.h"

#include "platform.h"
#include "util/geom.h"
#include "gl/renderState.h"
#include "gl/hardware.h"
#include "tangram.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <cstring> // for memset

namespace Tangram {

Texture::Texture(unsigned int _width, unsigned int _height, TextureOptions _options, bool _generateMipmaps)
    : m_options(_options), m_generateMipmaps(_generateMipmaps) {

    m_glHandle = 0;
    m_shouldResize = false;
    m_target = GL_TEXTURE_2D;
    m_generation = -1;

    resize(_width, _height);
}

Texture::Texture(const std::string& _file, TextureOptions _options, bool _generateMipmaps, bool _flipOnLoad)
    : Texture(0u, 0u, _options, _generateMipmaps) {

    unsigned int size;
    unsigned char* data;

    data = bytesFromFile(_file.c_str(), PathType::resource, &size);

    if (data) {
        loadImageFromMemory(data, size, _flipOnLoad);
    } else {
        LOGE("Failed to read Texture file: %s", _file.c_str());
    }

    free(data);
}

Texture::Texture(const unsigned char* data, size_t dataSize, TextureOptions options, bool generateMipmaps, bool _flipOnLoad)
    : Texture(0u, 0u, options, generateMipmaps) {

    loadImageFromMemory(data, dataSize, _flipOnLoad);
}

void Texture::loadImageFromMemory(const unsigned char* blob, unsigned int size, bool flipOnLoad) {
    unsigned char* pixels = nullptr;
    int width, height, comp;

    // stbi_load_from_memory loads the image as a serie of scanline starting from
    // the top-left corner of the image. When shouldFlip is set to true, the image
    // would be flipped vertically.
    stbi_set_flip_vertically_on_load((int)flipOnLoad);

    if (blob != nullptr && size != 0) {
        pixels = stbi_load_from_memory(blob, size, &width, &height, &comp, STBI_rgb_alpha);
    }

    if (pixels) {
        resize(width, height);

        setData(reinterpret_cast<GLuint*>(pixels), width * height);

        stbi_image_free(pixels);

        m_validData = true;
    } else {
        // Default inconsistent texture data is set to a 1*1 pixel texture
        // This reduces inconsistent behavior when texture failed loading
        // texture data but a Tangram style shader requires a shader sampler
        GLuint blackPixel = 0x0000ff;

        setData(&blackPixel, 1);

        LOGE("Decoding image from memory failed");

        m_validData = false;
    }
}

Texture::Texture(Texture&& _other) {
    m_glHandle = _other.m_glHandle;
    _other.m_glHandle = 0;

    m_options = _other.m_options;
    m_data = std::move(_other.m_data);
    m_dirtyRanges = std::move(_other.m_dirtyRanges);
    m_shouldResize = _other.m_shouldResize;
    m_width = _other.m_width;
    m_height = _other.m_height;
    m_target = _other.m_target;
    m_generation = _other.m_generation;
    m_generateMipmaps = _other.m_generateMipmaps;
}

Texture& Texture::operator=(Texture&& _other) {
    m_glHandle = _other.m_glHandle;
    _other.m_glHandle = 0;

    m_options = _other.m_options;
    m_data = std::move(_other.m_data);
    m_dirtyRanges = std::move(_other.m_dirtyRanges);
    m_shouldResize = _other.m_shouldResize;
    m_width = _other.m_width;
    m_height = _other.m_height;
    m_target = _other.m_target;
    m_generation = _other.m_generation;
    m_generateMipmaps = _other.m_generateMipmaps;

    return *this;
}

Texture::~Texture() {
    if (m_glHandle) {
        Tangram::runOnMainLoop([id = m_glHandle]() { GL_CHECK(glDeleteTextures(1, &id)); });

        // if the texture is bound, and deleted, the binding defaults to 0
        // according to the OpenGL spec, in this case we need to force the
        // currently bound texture to 0 in the render states
        if (RenderState::texture.compare(m_target, m_glHandle)) {
            RenderState::texture.init(m_target, 0, false);
        }
    }
}

void Texture::setData(const GLuint* _data, unsigned int _dataSize) {

    if (m_data.size() > 0) { m_data.clear(); }

    m_data.insert(m_data.begin(), _data, _data + _dataSize);

    setDirty(0, m_height);
}

void Texture::setSubData(const GLuint* _subData, uint16_t _xoff, uint16_t _yoff,
                         uint16_t _width, uint16_t _height, uint16_t _stride) {

    size_t bpp = bytesPerPixel();
    size_t divisor = sizeof(GLuint) / bpp;

    // Init m_data if update() was not called after resize()
    if (m_data.size() != (m_width * m_height) / divisor) {
        m_data.resize((m_width * m_height) / divisor);
    }

    // update m_data with subdata
    for (size_t row = 0; row < _height; row++) {

        size_t pos = ((_yoff + row) * m_width + _xoff) / divisor;
        size_t posIn = (row * _stride) / divisor;
        std::memcpy(&m_data[pos], &_subData[posIn], _width * bpp);
    }

    setDirty(_yoff, _height);
}

void Texture::setDirty(size_t _yoff, size_t _height) {
    // FIXME: check that dirty range is valid for texture size!
    size_t max = _yoff + _height;
    size_t min = _yoff;

    if (m_dirtyRanges.empty()) {
        m_dirtyRanges.push_back({min, max});
        return;
    }

    auto n = m_dirtyRanges.begin();

    // Find first overlap
    while (n != m_dirtyRanges.end()) {
        if (min > n->max) {
            // this range is after current
            ++n;
            continue;
        }
        if (max < n->min) {
            // this range is before current
            m_dirtyRanges.insert(n, {min, max});
            return;
        }
        // Combine with overlapping range
        n->min = std::min(n->min, min);
        n->max = std::max(n->max, max);
        break;
    }
    if (n == m_dirtyRanges.end()) {
        m_dirtyRanges.push_back({min, max});
        return;
    }

    // Merge up to last overlap
    auto it = n+1;
    while (it != m_dirtyRanges.end() && max >= it->min) {
        n->max = std::max(it->max, max);
        it = m_dirtyRanges.erase(it);
    }
}

void Texture::bind(GLuint _unit) {
    RenderState::textureUnit(_unit);
    RenderState::texture(m_target, m_glHandle);
}

void Texture::generate(GLuint _textureUnit) {
   GL_CHECK(glGenTextures(1, &m_glHandle));

    bind(_textureUnit);

    GL_CHECK(glTexParameteri(m_target, GL_TEXTURE_MIN_FILTER, m_options.filtering.min));
    GL_CHECK(glTexParameteri(m_target, GL_TEXTURE_MAG_FILTER, m_options.filtering.mag));

    GL_CHECK(glTexParameteri(m_target, GL_TEXTURE_WRAP_S, m_options.wrapping.wraps));
    GL_CHECK(glTexParameteri(m_target, GL_TEXTURE_WRAP_T, m_options.wrapping.wrapt));

    m_generation = RenderState::generation();
}

void Texture::checkValidity() {

    if (!RenderState::isValidGeneration(m_generation)) {
        m_shouldResize = true;
        m_glHandle = 0;
    }
}

bool Texture::isValid() const {
    return (RenderState::isValidGeneration(m_generation)
        && m_glHandle != 0
        && hasValidData());
}

bool Texture::hasValidData() const {
    return m_validData;
}

void Texture::update(GLuint _textureUnit) {

    checkValidity();

    if (!m_shouldResize && m_dirtyRanges.empty()) {
        return;
    }

    if (m_glHandle == 0) {
        if (m_data.size() == 0) {
            size_t divisor = sizeof(GLuint) / bytesPerPixel();
            m_data.resize((m_width * m_height) / divisor, 0);
        }
    }

    GLuint* data = m_data.size() > 0 ? m_data.data() : nullptr;

    update(_textureUnit, data);
}

void Texture::update(GLuint _textureUnit, const GLuint* data) {

    checkValidity();

    if (!m_shouldResize && m_dirtyRanges.empty()) {
        return;
    }

    if (m_glHandle == 0) {
        // texture hasn't been initialized yet, generate it
        generate(_textureUnit);
    } else {
        bind(_textureUnit);
    }

    // resize or push data
    if (m_shouldResize) {
        if (Hardware::maxTextureSize < m_width || Hardware::maxTextureSize < m_height) {
            LOGW("The hardware maximum texture size is currently reached");
        }

        GL_CHECK(glTexImage2D(m_target, 0, m_options.internalFormat,
                     m_width, m_height, 0, m_options.format,
                     GL_UNSIGNED_BYTE, data));

        if (data && m_generateMipmaps) {
            // generate the mipmaps for this texture
            GL_CHECK(glGenerateMipmap(m_target));
        }
        m_shouldResize = false;
        m_dirtyRanges.clear();
        return;
    }
    size_t bpp = bytesPerPixel();
    size_t divisor = sizeof(GLuint) / bpp;

    for (auto& range : m_dirtyRanges) {
        size_t offset =  (range.min * m_width) / divisor;
        GL_CHECK(glTexSubImage2D(m_target, 0, 0, range.min, m_width, range.max - range.min,
                        m_options.format, GL_UNSIGNED_BYTE,
                        data + offset));
    }
    m_dirtyRanges.clear();
}

void Texture::resize(const unsigned int _width, const unsigned int _height) {
    m_width = _width;
    m_height = _height;

    if (!(Hardware::supportsTextureNPOT) &&
        !(isPowerOfTwo(m_width) && isPowerOfTwo(m_height)) &&
        (m_generateMipmaps || isRepeatWrapping(m_options.wrapping))) {
        LOGW("OpenGL ES doesn't support texture repeat wrapping for NPOT textures nor mipmap textures");
        LOGW("Falling back to LINEAR Filtering");
        m_options.filtering = {GL_LINEAR, GL_LINEAR};
        m_generateMipmaps = false;
    }

    m_shouldResize = true;
    m_dirtyRanges.clear();
}

bool Texture::isRepeatWrapping(TextureWrapping _wrapping) {
    return _wrapping.wraps == GL_REPEAT || _wrapping.wrapt == GL_REPEAT;
}

size_t Texture::bytesPerPixel() {
    switch (m_options.internalFormat) {
        case GL_ALPHA:
        case GL_LUMINANCE:
            return 1;
        case GL_LUMINANCE_ALPHA:
            return 2;
        case GL_RGB:
            return 3;
        default:
            return 4;
    }
}

}
