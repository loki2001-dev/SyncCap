# Third-party software

SyncCap's own source code is under the MIT license (`LICENSE`). It builds against the libraries below.
The source trees under `3rdparty/` are unmodified upstream releases and keep their own license files.

| Library | Version | License | In this repository | Used by |
| --- | --- | --- | --- | --- |
| [libuv](https://github.com/libuv/libuv) | 1.51.0 | MIT | `3rdparty/libuv_src` (source) | collector, server |
| [Eclipse Paho MQTT C](https://github.com/eclipse-paho/paho.mqtt.c) | 1.3.16 | EPL-2.0 / EDL-1.0 | `3rdparty/paho_mqtt_c` (source) | collector, server |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | 0.61.0 | MIT | `3rdparty/cpp_httplib` (header) | collector, server, dashboard |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | MIT | `3rdparty/nlohmann` (header) | all |
| [spdlog](https://github.com/gabime/spdlog) (bundled {fmt}) | 1.16.0 | MIT | `3rdparty/spdlog` (header) | all |
| [Catch2](https://github.com/catchorg/Catch2) | 3.11.0 | BSL-1.0 | `3rdparty/Catch2/amalgamated` | tests |
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.92.5 WIP (master 2025-10-16) | MIT | `3rdparty/imgui` (source) | dashboard |
| [GLFW](https://github.com/glfw/glfw) | 3.4 | zlib | `3rdparty/glfw` (source) | dashboard |
| [stb_image](https://github.com/nothings/stb) | 2.30 | MIT / public domain | `3rdparty/stb` (header) | dashboard |
| [FFmpeg](https://ffmpeg.org) with [libx264](https://www.videolan.org/developers/x264.html) | n8.1 GPL build (Windows), distribution packages (Linux) | **GPL-2.0-or-later** | **not included**; `scripts/fetch_ffmpeg.ps1` downloads it on Windows | collector |
| [OpenCV](https://opencv.org) (optional) | system | Apache-2.0 | not included | collector webcam source |
| Swagger UI | 5.17.14, loaded from unpkg by `/docs` | Apache-2.0 | not included | server |

## GPL note

The collector links FFmpeg's libavcodec built with libx264, both GPL. This repository publishes source code
only and contains no FFmpeg or libx264 binaries. If you distribute a built collector together with a GPL
FFmpeg build, the combined work must be distributed under the terms of the GPL.
