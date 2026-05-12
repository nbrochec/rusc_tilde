cmake_minimum_required(VERSION 3.19)
set(CMAKE_CXX_STANDARD 17)
project(clap_tilde VERSION 1.0.0)

set(CMAKE_OSX_ARCHITECTURES arm64)
include(FetchContent)

# ── r8brain ───────────────────────────────────────────────────────────────────
set(IPT_R8BRAIN "${CMAKE_CURRENT_SOURCE_DIR}/../ipt_tilde/libs/r8brain")
if(EXISTS "${IPT_R8BRAIN}/CMakeLists.txt")
    message(STATUS "Reusing r8brain from ipt_tilde")
    add_subdirectory("${IPT_R8BRAIN}" "${CMAKE_BINARY_DIR}/r8brain")
else()
    add_subdirectory(libs/r8brain)
endif()

# ── min-api ───────────────────────────────────────────────────────────────────
set(IPT_MIN_API "${CMAKE_CURRENT_SOURCE_DIR}/../ipt_tilde/min-api")
if(EXISTS "${IPT_MIN_API}/script/min-package.cmake")
    message(STATUS "Reusing min-api from ipt_tilde")
    set(C74_MIN_API_DIR "${IPT_MIN_API}")
else()
    set(C74_MIN_API_DIR "${CMAKE_CURRENT_SOURCE_DIR}/min-api")
endif()

include(${C74_MIN_API_DIR}/script/min-package.cmake)
set(C74_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/externals")

# ── onnxruntime ───────────────────────────────────────────────────────────────
set(ORT_VERSION "1.20.1")
set(ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-osx-arm64-${ORT_VERSION}.tgz")
set(LOCAL_ORT "${CMAKE_CURRENT_SOURCE_DIR}/libs/onnxruntime")

if(EXISTS "${LOCAL_ORT}/include/onnxruntime_cxx_api.h")
    message(STATUS "onnxruntime found locally: ${LOCAL_ORT}")
else()
    message(STATUS "Downloading onnxruntime ${ORT_VERSION}...")
    FetchContent_Declare(
        onnxruntime
        URL ${ORT_URL}
        SOURCE_DIR ${LOCAL_ORT}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(onnxruntime)
endif()

set(ORT_INCLUDE_DIR "${LOCAL_ORT}/include")
set(ORT_LIB        "${LOCAL_ORT}/lib/libonnxruntime.${ORT_VERSION}.dylib")

# ── Essentia (static) ─────────────────────────────────────────────────────────
set(ESSENTIA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/essentia")
if(NOT EXISTS "${ESSENTIA_DIR}/include/essentia/essentia.h")
    message(FATAL_ERROR
        "Essentia not found at ${ESSENTIA_DIR}.\n"
        "Build and install it:\n"
        "  git clone https://github.com/MTG/essentia.git /tmp/essentia\n"
        "  cd /tmp/essentia\n"
        "  brew install fftw eigen libyaml pkg-config\n"
        "  CXXFLAGS=-std=c++17 PKG_CONFIG_PATH=/opt/homebrew/lib/pkgconfig \\\n"
        "    python3 waf configure --build-static --prefix=${ESSENTIA_DIR}\n"
        "  CXXFLAGS=-std=c++17 python3 waf build install")
endif()

set(ESSENTIA_INCLUDE_DIR "${ESSENTIA_DIR}/include")
set(ESSENTIA_LIB        "${ESSENTIA_DIR}/lib/libessentia.a")

# FFTW3 (single-precision, used by Essentia internally)
find_library(FFTW3F_LIB fftw3f HINTS /opt/homebrew/lib REQUIRED)
find_library(FFTW3_LIB  fftw3  HINTS /opt/homebrew/lib REQUIRED)
message(STATUS "Essentia:  ${ESSENTIA_DIR}")
message(STATUS "FFTW3f:    ${FFTW3F_LIB}")

# ── targets ───────────────────────────────────────────────────────────────────
add_subdirectory(src)
add_subdirectory(clap_tilde)

# ── C++ standard / deployment target ─────────────────────────────────────────
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum OS X deployment version" FORCE)
message(STATUS "MacOS deployment target: ${CMAKE_OSX_DEPLOYMENT_TARGET}")
