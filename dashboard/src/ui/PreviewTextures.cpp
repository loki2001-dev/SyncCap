#include "ui/PreviewTextures.h"

#include <GLFW/glfw3.h>

namespace {

// OpenGL 1.2 constant; the Windows SDK gl.h stops at 1.1
#ifndef GL_CLAMP_TO_EDGE
constexpr GLint CLAMP_TO_EDGE = 0x812F;
#else
constexpr GLint CLAMP_TO_EDGE = GL_CLAMP_TO_EDGE;
#endif

}  // namespace

PreviewTextures::~PreviewTextures() {
    release();
}

void PreviewTextures::update(const preview_state& preview) {
    if (preview.version == _uploadedVersion) {
        return;
    }
    _uploadedVersion = preview.version;
    for (const preview_frame& frame : preview.frames) {
        if (!frame.rgba || frame.width <= 0 || frame.height <= 0) {
            continue;
        }
        const auto slot = static_cast<size_t>(frame.index);
        // P3: the index came over the network; the model keeps at most a handful of channels
        if (frame.index < 0 || slot >= MAX_TEXTURES) {
            continue;
        }
        if (_textures.size() <= slot) {
            _textures.resize(slot + 1);
        }
        preview_texture& texture = _textures[slot];
        if (texture.id == 0) {
            GLuint name = 0;
            glGenTextures(1, &name);
            texture.id = name;
            glBindTexture(GL_TEXTURE_2D, name);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, CLAMP_TO_EDGE);
        }
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture.id));
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (texture.width == frame.width && texture.height == frame.height) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width, frame.height, GL_RGBA, GL_UNSIGNED_BYTE,
                            frame.rgba->data());
        } else {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, frame.width, frame.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         frame.rgba->data());
            texture.width = frame.width;
            texture.height = frame.height;
        }
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

void PreviewTextures::release() {
    for (preview_texture& texture : _textures) {
        if (texture.id != 0) {
            const auto name = static_cast<GLuint>(texture.id);
            glDeleteTextures(1, &name);
            texture.id = 0;
        }
    }
    _textures.clear();
    _uploadedVersion = 0;
}

const std::vector<preview_texture>& PreviewTextures::get_textures() const {
    return _textures;
}
