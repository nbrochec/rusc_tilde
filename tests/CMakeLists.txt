# Unit tests for the model-independent DSP code (no Max, no ONNX model needed).
add_executable(rusc_tests test_dsp.cpp)
target_link_libraries(rusc_tests PRIVATE rusc_dsp)
set_target_properties(rusc_tests PROPERTIES
    CXX_STANDARD 17
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

if(MSVC)
    target_compile_options(rusc_tests PRIVATE /W3 /EHsc)
else()
    target_compile_options(rusc_tests PRIVATE -Wall -Wextra)
endif()

add_test(NAME rusc_dsp COMMAND rusc_tests)
