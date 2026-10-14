cmake_minimum_required(VERSION 3.16)

# MSVC prints its /showIncludes lines in the console language, and Ninja recognizes only the prefix detected at
# configure time. a compile from a console in another language records no header dependencies, so a changed
# header leaves stale objects behind (LNK2019). English is forced for the detection and for every compile
if(CMAKE_HOST_WIN32)
  set(ENV{VSLANG} 1033)
endif()
project(synccap VERSION 1.0.0 LANGUAGES C CXX)
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
  foreach(lang C CXX)
    if(NOT CMAKE_${lang}_COMPILER_LAUNCHER)
      set(CMAKE_${lang}_COMPILER_LAUNCHER ${CMAKE_COMMAND} -E env VSLANG=1033)
    endif()
  endforeach()
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "build type" FORCE)
endif()

option(SYNCCAP_BUILD_COLLECTOR "build the acquisition system (needs FFmpeg, libuv)" ON)
option(SYNCCAP_BUILD_SERVER "build the OpenAPI server" ON)
option(SYNCCAP_BUILD_TESTS "build Catch2 tests" ON)
option(SYNCCAP_BUILD_DASHBOARD "build the Dear ImGui dashboard (needs OpenGL; X11 dev packages on Linux)" ON)
set(SYNCCAP_WITH_OPENCV "AUTO" CACHE STRING "webcam source through OpenCV: AUTO, ON or OFF")
set(PROJECT_SANITIZER "" CACHE STRING "empty, address or thread")

set(PROJECT_WARNINGS
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /utf-8 /permissive->
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Werror>
)

set(PROJECT_SANITIZER_FLAGS)
if(PROJECT_SANITIZER STREQUAL "address")
  set(PROJECT_SANITIZER_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
elseif(PROJECT_SANITIZER STREQUAL "thread")
  set(PROJECT_SANITIZER_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
elseif(NOT PROJECT_SANITIZER STREQUAL "")
  message(FATAL_ERROR "unknown PROJECT_SANITIZER=${PROJECT_SANITIZER}")
endif()

# build info
execute_process(
  COMMAND git describe --always --dirty --abbrev=12
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE PROJECT_COMMIT
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if(NOT PROJECT_COMMIT)
  set(PROJECT_COMMIT unknown)
endif()
configure_file(src/app/BuildInfo.h.in ${CMAKE_BINARY_DIR}/generated/app/BuildInfo.h @ONLY)

set(THIRDPARTY ${CMAKE_SOURCE_DIR}/3rdparty)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# 3rdparty: header only
# ---------------------------------------------------------------------------
add_library(synccap_thirdparty_headers INTERFACE)
target_include_directories(synccap_thirdparty_headers SYSTEM INTERFACE
  ${THIRDPARTY}/spdlog/include
  ${THIRDPARTY}/nlohmann/include
  ${THIRDPARTY}/cpp_httplib
)
if(WIN32)
  # httplib needs Windows 10 APIs, winsock must not be pulled in by windows.h first
  target_compile_definitions(synccap_thirdparty_headers INTERFACE _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(synccap_thirdparty_headers INTERFACE ws2_32 crypt32)
endif()

# ---------------------------------------------------------------------------
# 3rdparty: Eclipse Paho MQTT C (source build, static, no TLS)
# ---------------------------------------------------------------------------
set(PAHO_BUILD_STATIC TRUE CACHE BOOL "" FORCE)
set(PAHO_BUILD_SHARED FALSE CACHE BOOL "" FORCE)
set(PAHO_WITH_SSL FALSE CACHE BOOL "" FORCE)
set(PAHO_ENABLE_TESTING FALSE CACHE BOOL "" FORCE)
set(PAHO_ENABLE_CPACK FALSE CACHE BOOL "" FORCE)
set(PAHO_BUILD_SAMPLES FALSE CACHE BOOL "" FORCE)
set(PAHO_BUILD_DOCUMENTATION FALSE CACHE BOOL "" FORCE)
set(PAHO_HIGH_PERFORMANCE TRUE CACHE BOOL "" FORCE)
add_subdirectory(${THIRDPARTY}/paho_mqtt_c/paho.mqtt.c-1.3.16 ${CMAKE_BINARY_DIR}/3rdparty/paho EXCLUDE_FROM_ALL)
if(MSVC)
  # paho headers carry non-ASCII comments; read them as UTF-8 instead of the system code page
  target_compile_options(paho-mqtt3a-static PRIVATE /utf-8)
endif()
add_library(synccap_paho INTERFACE)
target_link_libraries(synccap_paho INTERFACE paho-mqtt3a-static)
target_include_directories(synccap_paho SYSTEM INTERFACE
  ${THIRDPARTY}/paho_mqtt_c/paho.mqtt.c-1.3.16/src
  ${CMAKE_BINARY_DIR}/3rdparty/paho/src
)

# ---------------------------------------------------------------------------
# 3rdparty: libuv 1.51.0 (source build, static) on every platform
# ---------------------------------------------------------------------------
set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)
add_subdirectory(${THIRDPARTY}/libuv_src/libuv-1.51.0 ${CMAKE_BINARY_DIR}/3rdparty/libuv EXCLUDE_FROM_ALL)
add_library(synccap_uv INTERFACE)
target_link_libraries(synccap_uv INTERFACE uv_a)

# ---------------------------------------------------------------------------
# synccap_common: shared by the collector and the server
# ---------------------------------------------------------------------------
add_library(synccap_common STATIC
  src/util/log/Logger.cpp
  src/util/TimeUtil.cpp
  src/core/UvLoop.cpp
  src/core/UvTimer.cpp
  src/core/UvSignal.cpp
  src/codec/StatusJsonCodec.cpp
  src/codec/ControlCommandCodec.cpp
  src/network/HttpLibClient.cpp
  src/network/PahoMqttClient.cpp
  src/network/HttpServerHost.cpp
)
target_include_directories(synccap_common PUBLIC src ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(synccap_common PUBLIC synccap_thirdparty_headers synccap_paho synccap_uv Threads::Threads)
target_compile_options(synccap_common PRIVATE ${PROJECT_WARNINGS})

set(SYNCCAP_TARGETS synccap_common)

# ---------------------------------------------------------------------------
# collector
# ---------------------------------------------------------------------------
if(SYNCCAP_BUILD_COLLECTOR)
  # FFmpeg with libx264
  if(WIN32)
    # not in the repository: scripts/fetch_ffmpeg.ps1 downloads a pinned GPL build with libx264
    set(FFMPEG_ROOT ${THIRDPARTY}/ffmpeg_x264/win64)
    if(NOT EXISTS ${FFMPEG_ROOT}/lib/avcodec.lib)
      message(FATAL_ERROR "FFmpeg not found at ${FFMPEG_ROOT}. Run: "
                          "powershell -ExecutionPolicy Bypass -File scripts/fetch_ffmpeg.ps1")
    endif()
    add_library(synccap_ffmpeg INTERFACE)
    target_include_directories(synccap_ffmpeg SYSTEM INTERFACE ${FFMPEG_ROOT}/include)
    target_link_libraries(synccap_ffmpeg INTERFACE
      ${FFMPEG_ROOT}/lib/avformat.lib ${FFMPEG_ROOT}/lib/avcodec.lib
      ${FFMPEG_ROOT}/lib/swscale.lib ${FFMPEG_ROOT}/lib/avutil.lib)
    file(GLOB SYNCCAP_RUNTIME_DLLS ${FFMPEG_ROOT}/bin/*.dll)

  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libswscale libavutil)
    add_library(synccap_ffmpeg INTERFACE)
    target_link_libraries(synccap_ffmpeg INTERFACE PkgConfig::FFMPEG)
    set(SYNCCAP_RUNTIME_DLLS)
  endif()


  set(SYNCCAP_OPENCV_FOUND OFF)
  if(NOT SYNCCAP_WITH_OPENCV STREQUAL "OFF")
    if(SYNCCAP_WITH_OPENCV STREQUAL "ON")
      find_package(OpenCV REQUIRED COMPONENTS core imgproc videoio)
    else()
      find_package(OpenCV QUIET COMPONENTS core imgproc videoio)
    endif()
    if(OpenCV_FOUND)
      set(SYNCCAP_OPENCV_FOUND ON)
    endif()
  endif()
  message(STATUS "synccap: webcam source through OpenCV=${SYNCCAP_OPENCV_FOUND}")

  set(COLLECTOR_SOURCES
    src/core/MessageBus.cpp
    src/core/StateStore.cpp
    src/config/ConfigLoader.cpp
    src/model/FrameBuffer.cpp
    src/source/FfmpegVideoSource.cpp
    src/source/FileVideoSource.cpp
    src/source/NetworkStreamVideoSource.cpp
    src/source/SyntheticVideoSource.cpp
    src/source/VideoSourceFactory.cpp
    src/source/CsvSensorSource.cpp
    src/source/SimulatedSensorSource.cpp
    src/source/SensorSourceFactory.cpp
    src/codec/X264Encoder.cpp
    src/codec/FfmpegLog.cpp
    src/codec/JpegEncoder.cpp
    src/storage/SegmentClock.cpp
    src/storage/SegmentedRecorder.cpp
    src/pipeline/FramePool.cpp
    src/pipeline/LatestFrameSlot.cpp
    src/pipeline/FrameClock.cpp
    src/pipeline/SyncMonitor.cpp
    src/pipeline/FrameSynchronizer.cpp
    src/pipeline/CaptureWorker.cpp
    src/pipeline/EncodeWorker.cpp
    src/pipeline/PreviewWorker.cpp
    src/pipeline/VideoPipeline.cpp
    src/component/AcquisitionController.cpp
    src/component/VideoCaptureComponent.cpp
    src/component/SensorComponent.cpp
    src/component/StatusReportComponent.cpp
    src/component/MqttComponent.cpp
    src/component/StatusSnapshot.cpp
    src/component/PreviewComponent.cpp
    src/app/Application.cpp
  )
  if(SYNCCAP_OPENCV_FOUND)
    list(APPEND COLLECTOR_SOURCES src/source/WebcamVideoSource.cpp)
  endif()

  add_library(synccap_collector_lib STATIC ${COLLECTOR_SOURCES})
  target_link_libraries(synccap_collector_lib PUBLIC synccap_common synccap_ffmpeg)
  if(SYNCCAP_OPENCV_FOUND)
    target_compile_definitions(synccap_collector_lib PUBLIC SYNCCAP_WITH_OPENCV=1)
    target_include_directories(synccap_collector_lib SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(synccap_collector_lib PUBLIC ${OpenCV_LIBS})
  endif()
  if(WIN32)
    target_link_libraries(synccap_collector_lib PUBLIC winmm)
  endif()
  target_compile_options(synccap_collector_lib PRIVATE ${PROJECT_WARNINGS})

  add_executable(synccap_collector src/app/main.cpp)
  target_link_libraries(synccap_collector PRIVATE synccap_collector_lib)
  target_compile_options(synccap_collector PRIVATE ${PROJECT_WARNINGS})
  list(APPEND SYNCCAP_TARGETS synccap_collector_lib synccap_collector)

  if(SYNCCAP_RUNTIME_DLLS)
    add_custom_command(TARGET synccap_collector POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SYNCCAP_RUNTIME_DLLS} $<TARGET_FILE_DIR:synccap_collector>)
  endif()
endif()

# ---------------------------------------------------------------------------
# server
# ---------------------------------------------------------------------------
if(SYNCCAP_BUILD_SERVER)
  add_library(synccap_server_lib STATIC
    server/src/StatusRepository.cpp
    server/src/ControlRelay.cpp
    server/src/ApiServer.cpp
    server/src/ServerConfig.cpp
  )
  target_include_directories(synccap_server_lib PUBLIC server/src)
  target_link_libraries(synccap_server_lib PUBLIC synccap_common)
  target_compile_options(synccap_server_lib PRIVATE ${PROJECT_WARNINGS})

  add_executable(synccap_server server/src/main.cpp)
  target_link_libraries(synccap_server PRIVATE synccap_server_lib)
  target_compile_options(synccap_server PRIVATE ${PROJECT_WARNINGS})
  list(APPEND SYNCCAP_TARGETS synccap_server_lib synccap_server)
endif()

# ---------------------------------------------------------------------------
# dashboard (Dear ImGui + GLFW + OpenGL 3), a client of the OpenAPI server
# ---------------------------------------------------------------------------
if(SYNCCAP_BUILD_DASHBOARD)
  find_package(OpenGL REQUIRED)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  # X11 only on Linux: Wayland would add wayland-scanner and protocol packages to the build
  set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
  add_subdirectory(${THIRDPARTY}/glfw/glfw-3.4-src ${CMAKE_BINARY_DIR}/3rdparty/glfw EXCLUDE_FROM_ALL)
  if(MSVC)
    target_compile_options(glfw PRIVATE /utf-8)
  endif()

  set(IMGUI_ROOT ${THIRDPARTY}/imgui/imgui-1.92.5-wip)
  add_library(synccap_imgui STATIC
    ${IMGUI_ROOT}/imgui.cpp
    ${IMGUI_ROOT}/imgui_draw.cpp
    ${IMGUI_ROOT}/imgui_tables.cpp
    ${IMGUI_ROOT}/imgui_widgets.cpp
    ${IMGUI_ROOT}/backends/imgui_impl_glfw.cpp
    ${IMGUI_ROOT}/backends/imgui_impl_opengl3.cpp
  )
  target_include_directories(synccap_imgui SYSTEM PUBLIC ${IMGUI_ROOT} ${IMGUI_ROOT}/backends)
  target_link_libraries(synccap_imgui PUBLIC glfw OpenGL::GL)
  if(MSVC)
    target_compile_options(synccap_imgui PRIVATE /utf-8)
  endif()

  add_library(synccap_dashboard_lib STATIC
    dashboard/src/model/DashboardModel.cpp
    dashboard/src/net/ServerClient.cpp
    dashboard/src/net/PreviewClient.cpp
    dashboard/src/net/StbImage.cpp
    dashboard/src/ui/DashboardView.cpp
    dashboard/src/ui/PreviewTextures.cpp
  )
  target_include_directories(synccap_dashboard_lib SYSTEM PRIVATE ${THIRDPARTY}/stb)
  target_include_directories(synccap_dashboard_lib PUBLIC dashboard/src)
  target_link_libraries(synccap_dashboard_lib PUBLIC synccap_common synccap_imgui)
  target_compile_options(synccap_dashboard_lib PRIVATE ${PROJECT_WARNINGS})

  add_executable(synccap_dashboard dashboard/src/ui/main.cpp dashboard/src/ui/DashboardApp.cpp)
  target_link_libraries(synccap_dashboard PRIVATE synccap_dashboard_lib)
  if(WIN32)
    # timeBeginPeriod for a steady 30 fps preview
    target_link_libraries(synccap_dashboard PRIVATE winmm)
  endif()
  target_compile_options(synccap_dashboard PRIVATE ${PROJECT_WARNINGS})
  list(APPEND SYNCCAP_TARGETS synccap_dashboard_lib synccap_dashboard)
endif()

# ---------------------------------------------------------------------------
# tests
# ---------------------------------------------------------------------------
if(SYNCCAP_BUILD_TESTS)
  enable_testing()
  add_library(synccap_catch2 STATIC ${THIRDPARTY}/Catch2/amalgamated/catch_amalgamated.cpp)
  target_include_directories(synccap_catch2 SYSTEM PUBLIC ${THIRDPARTY}/Catch2/amalgamated)
  target_compile_features(synccap_catch2 PUBLIC cxx_std_17)
  # tests/TestMain.cpp installs the logger before Catch2 runs
  target_compile_definitions(synccap_catch2 PRIVATE CATCH_AMALGAMATED_CUSTOM_MAIN)
  if(MSVC)
    target_compile_options(synccap_catch2 PRIVATE /utf-8)
  endif()

  set(COMMON_TEST_SOURCES
    tests/TestMain.cpp
    tests/BoundedQueueTest.cpp
    tests/BlockingQueueTest.cpp
    tests/StatusJsonCodecTest.cpp
    tests/ControlCommandCodecTest.cpp
    tests/HttpLibClientTest.cpp
  )
  set(TEST_LIBS synccap_common)
  if(SYNCCAP_BUILD_COLLECTOR)
    list(APPEND COMMON_TEST_SOURCES
      tests/FramePoolTest.cpp
      tests/FrameClockTest.cpp
      tests/SegmentClockTest.cpp
      tests/SyncMonitorTest.cpp
      tests/FrameSynchronizerTest.cpp
      tests/ConfigLoaderTest.cpp
      tests/SensorSourceTest.cpp
      tests/X264EncoderTest.cpp
      tests/MessageBusTest.cpp
      tests/AcquisitionControllerTest.cpp
      tests/MqttComponentTest.cpp
      tests/PreviewComponentTest.cpp
      tests/VideoPipelineIntegrationTest.cpp
    )
    list(APPEND TEST_LIBS synccap_collector_lib)
  endif()
  if(SYNCCAP_BUILD_SERVER)
    list(APPEND COMMON_TEST_SOURCES tests/ServerApiTest.cpp)
    list(APPEND TEST_LIBS synccap_server_lib)
  endif()
  if(SYNCCAP_BUILD_DASHBOARD)
    list(APPEND COMMON_TEST_SOURCES tests/DashboardModelTest.cpp tests/ServerClientTest.cpp tests/PreviewClientTest.cpp)
    list(APPEND TEST_LIBS synccap_dashboard_lib)
  endif()

  add_executable(synccap_tests ${COMMON_TEST_SOURCES})
  target_link_libraries(synccap_tests PRIVATE ${TEST_LIBS} synccap_catch2)
  if(SYNCCAP_BUILD_COLLECTOR)
    target_compile_definitions(synccap_tests PRIVATE SYNCCAP_TESTS_WITH_COLLECTOR=1)
  endif()
  target_compile_options(synccap_tests PRIVATE ${PROJECT_WARNINGS})
  list(APPEND SYNCCAP_TARGETS synccap_tests)
  if(SYNCCAP_BUILD_COLLECTOR AND SYNCCAP_RUNTIME_DLLS)
    add_custom_command(TARGET synccap_tests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SYNCCAP_RUNTIME_DLLS} $<TARGET_FILE_DIR:synccap_tests>)
  endif()
  add_test(NAME synccap_tests COMMAND synccap_tests WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(synccap_tests PROPERTIES TIMEOUT 300)
endif()

foreach(target ${SYNCCAP_TARGETS})
  if(PROJECT_SANITIZER_FLAGS)
    target_compile_options(${target} PRIVATE ${PROJECT_SANITIZER_FLAGS})
    get_target_property(target_type ${target} TYPE)
    if(target_type STREQUAL "EXECUTABLE")
      target_link_options(${target} PRIVATE ${PROJECT_SANITIZER_FLAGS})
    endif()
  endif()
endforeach()
