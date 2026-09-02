# DSP + model front-end, header-only (needs r8brain and pocketfft only).
add_library(rusc_dsp INTERFACE)
target_sources(rusc_dsp INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/utility.h
        ${CMAKE_CURRENT_SOURCE_DIR}/circular_buffer.h
        ${CMAKE_CURRENT_SOURCE_DIR}/energy_threshold.h
        ${CMAKE_CURRENT_SOURCE_DIR}/leaky_integrator.h
        ${CMAKE_CURRENT_SOURCE_DIR}/mel_frontend.h
        ${CMAKE_CURRENT_SOURCE_DIR}/bpe_tokenizer.h
        ${CMAKE_CURRENT_SOURCE_DIR}/spsc_ring.h
)
target_include_directories(rusc_dsp INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${POCKETFFT_INCLUDE_DIR})
target_link_libraries(rusc_dsp INTERFACE r8brain)
if(NOT WIN32)
    # pocketfft can use pthreads for large multi-dimensional transforms; not needed here
    target_compile_definitions(rusc_dsp INTERFACE POCKETFFT_NO_MULTITHREADING)
endif()

# CLAP classifier on top of ONNX Runtime.
add_library(clap INTERFACE)
target_sources(clap INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/clap_model.h
        ${CMAKE_CURRENT_SOURCE_DIR}/clap_model_onnx.h
        ${CMAKE_CURRENT_SOURCE_DIR}/clap_classifier.h
)
target_include_directories(clap INTERFACE ${ORT_INCLUDE_DIR})
target_link_libraries(clap INTERFACE rusc_dsp "${ORT_LIB}")
