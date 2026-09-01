cmake_minimum_required(VERSION 3.19)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    # Must be set before project(): Apple Silicon only
    set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "" FORCE)
endif()

project(rusc_tilde VERSION 1.0.0)

include(FetchContent)
option(RUSC_BUILD_TESTS "Build the rusc~ unit tests" ON)

if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ── r8brain (resampler) ───────────────────────────────────────────────────────
# A local checkout in libs/r8brain (with its own CMakeLists.txt) takes precedence;
# otherwise the sources are fetched from GitHub.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/r8brain/CMakeLists.txt")
    message(STATUS "r8brain: using local libs/r8brain")
    add_subdirectory(libs/r8brain)
else()
    message(STATUS "r8brain: fetching r8brain-free-src")
    FetchContent_Declare(
        r8brain_src
        GIT_REPOSITORY https://github.com/avaneev/r8brain-free-src.git
        GIT_TAG        version-6.5
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(r8brain_src)
    file(GLOB R8BRAIN_SOURCES "${r8brain_src_SOURCE_DIR}/*.cpp" "${r8brain_src_SOURCE_DIR}/*.h")
    list(REMOVE_ITEM R8BRAIN_SOURCES "${r8brain_src_SOURCE_DIR}/example.cpp")
    add_library(r8brain STATIC ${R8BRAIN_SOURCES})
    target_include_directories(r8brain SYSTEM PUBLIC "${r8brain_src_SOURCE_DIR}")
    set_target_properties(r8brain PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# ── pocketfft (header-only FFT) ───────────────────────────────────────────────
FetchContent_Declare(
    pocketfft
    GIT_REPOSITORY https://github.com/mreineck/pocketfft.git
    GIT_TAG        c90e55b3d529f8efa40ed01a20de22405f45fc65   # branch "cpp"
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(pocketfft)   # no CMakeLists in the repo: populate only
set(POCKETFFT_INCLUDE_DIR "${pocketfft_SOURCE_DIR}")

# ── min-api ───────────────────────────────────────────────────────────────────
set(C74_MIN_API_DIR "${CMAKE_CURRENT_SOURCE_DIR}/min-api")
include(${C74_MIN_API_DIR}/script/min-package.cmake)
set(C74_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/externals")

# ── onnxruntime (prebuilt release) ────────────────────────────────────────────
set(ORT_VERSION "1.28.0")
set(LOCAL_ORT "${CMAKE_CURRENT_SOURCE_DIR}/libs/onnxruntime")

if(APPLE)
    set(ORT_ARCHIVE "onnxruntime-osx-arm64-${ORT_VERSION}.tgz")
elseif(WIN32)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
        set(ORT_ARCHIVE "onnxruntime-win-arm64-${ORT_VERSION}.zip")
    else()
        set(ORT_ARCHIVE "onnxruntime-win-x64-${ORT_VERSION}.zip")
    endif()
else()
    set(ORT_ARCHIVE "onnxruntime-linux-x64-${ORT_VERSION}.tgz")
endif()
set(ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/${ORT_ARCHIVE}")

if(EXISTS "${LOCAL_ORT}/include/onnxruntime_cxx_api.h")
    message(STATUS "onnxruntime found locally: ${LOCAL_ORT}")
else()
    message(STATUS "Downloading ${ORT_ARCHIVE} ...")
    FetchContent_Declare(
        onnxruntime
        URL ${ORT_URL}
        SOURCE_DIR ${LOCAL_ORT}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(onnxruntime)
endif()

set(ORT_INCLUDE_DIR "${LOCAL_ORT}/include")
if(APPLE)
    set(ORT_LIB     "${LOCAL_ORT}/lib/libonnxruntime.${ORT_VERSION}.dylib")
    set(ORT_RUNTIME "${ORT_LIB}")
elseif(WIN32)
    set(ORT_LIB     "${LOCAL_ORT}/lib/onnxruntime.lib")
    set(ORT_RUNTIME "${LOCAL_ORT}/lib/onnxruntime.dll")
else()
    set(ORT_LIB     "${LOCAL_ORT}/lib/libonnxruntime.so.${ORT_VERSION}")
    set(ORT_RUNTIME "${ORT_LIB}")
endif()
message(STATUS "onnxruntime lib: ${ORT_LIB}")

# ── targets ───────────────────────────────────────────────────────────────────
add_subdirectory(src)
add_subdirectory(rusc_tilde)

if(RUSC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ── deployment target ─────────────────────────────────────────────────────────
if(APPLE)
    # onnxruntime 1.28 is built for macOS 14+
    set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS deployment version" FORCE)
    message(STATUS "macOS deployment target: ${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()
