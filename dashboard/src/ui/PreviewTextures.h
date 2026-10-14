#pragma once

#include <cstdint>
#include <vector>

#include "model/DashboardModel.h"

// one channel tile as the view sees it
struct preview_texture {
    uint64_t id = 0;  // OpenGL texture name, passed to ImGui as ImTextureID
    int width = 0;
    int height = 0;
};

/**
 * @brief OpenGL textures for the preview tiles. UI thread only, while the GL context is current
 *
 * update() uploads the pictures of a new preview set once (by version); textures are reused across sets
 */
class PreviewTextures {
public:
    PreviewTextures() = default;
    ~PreviewTextures();

    PreviewTextures(const PreviewTextures&) = delete;
    PreviewTextures& operator=(const PreviewTextures&) = delete;

    void update(const preview_state& preview);
    // must run before the GL context goes away
    void release();

    const std::vector<preview_texture>& get_textures() const;

    static constexpr size_t MAX_TEXTURES = 8;

private:
    std::vector<preview_texture> _textures;
    uint64_t _uploadedVersion = 0;
};
