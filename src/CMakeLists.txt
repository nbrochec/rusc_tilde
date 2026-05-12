
add_library(clap INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/utility.h
        ${CMAKE_CURRENT_SOURCE_DIR}/circular_buffer.h
        ${CMAKE_CURRENT_SOURCE_DIR}/energy_threshold.h
        ${CMAKE_CURRENT_SOURCE_DIR}/leaky_integrator.h
        ${CMAKE_CURRENT_SOURCE_DIR}/bpe_tokenizer.h
        ${CMAKE_CURRENT_SOURCE_DIR}/clap_model.h
        ${CMAKE_CURRENT_SOURCE_DIR}/clap_model_onnx.h
        ${CMAKE_CURRENT_SOURCE_DIR}/clap_classifier.h
)

target_include_directories(clap INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${ORT_INCLUDE_DIR}
        ${ESSENTIA_INCLUDE_DIR}
        /opt/homebrew/include
        /opt/homebrew/include/eigen3)

find_library(YAML_LIB yaml HINTS /opt/homebrew/lib REQUIRED)

target_link_libraries(clap
        INTERFACE
        r8brain
        "${ORT_LIB}"
        "${ESSENTIA_LIB}"
        "${FFTW3F_LIB}"
        "${FFTW3_LIB}"
        "${YAML_LIB}")
