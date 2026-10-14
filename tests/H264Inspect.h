#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// test helper: walks an H.264 Annex-B byte stream
struct h264_summary {
    int pictures = 0;  // slices with first_mb_in_slice == 0
    int idrPictures = 0;
    int sps = 0;
    int firstNalType = -1;
    bool firstPictureIsIdr = false;
};

inline h264_summary inspectH264(const std::vector<uint8_t>& data) {
    h264_summary summary;
    size_t index = 0;
    while (index + 3 < data.size()) {
        const bool startCode3 = data[index] == 0 && data[index + 1] == 0 && data[index + 2] == 1;
        if (!startCode3) {
            index += 1;
            continue;
        }
        const size_t header = index + 3;
        if (header + 1 >= data.size()) {
            break;
        }
        const int type = data[header] & 0x1F;
        if (summary.firstNalType < 0) {
            summary.firstNalType = type;
        }
        if (type == 7) {
            summary.sps += 1;
        }
        // first_mb_in_slice is ue(v); value 0 is coded as a single '1' bit
        if ((type == 1 || type == 5) && (data[header + 1] & 0x80) != 0) {
            if (summary.pictures == 0) {
                summary.firstPictureIsIdr = type == 5;
            }
            summary.pictures += 1;
            if (type == 5) {
                summary.idrPictures += 1;
            }
        }
        index = header + 1;
    }
    return summary;
}

inline std::vector<uint8_t> readBinary(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}
