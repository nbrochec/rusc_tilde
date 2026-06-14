include(${C74_MIN_API_DIR}/script/min-package.cmake)

include(${C74_MIN_API_DIR}/script/min-pretarget.cmake)

include_directories("${C74_INCLUDES}")

set(SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/rusc_tilde.cpp)

add_library(${PROJECT_NAME} MODULE ${SOURCE_FILES})

add_custom_command(TARGET ${PROJECT_NAME} PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E remove_directory
            "${C74_LIBRARY_OUTPUT_DIRECTORY}/rusc~.mxo"
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "${C74_LIBRARY_OUTPUT_DIRECTORY}/rusc~.mxo/Contents/MacOS"
    COMMENT "Removing old rusc~.mxo"
)

target_link_libraries(${PROJECT_NAME}
        PRIVATE
        clap)

include(${C74_MIN_API_DIR}/script/min-posttarget.cmake)

set_target_properties(${PROJECT_NAME}
        PROPERTIES
        BUILD_WITH_INSTALL_RPATH FALSE
        LINK_FLAGS "-Wl,-rpath,@loader_path/"
)

add_custom_command(
        TARGET ${PROJECT_NAME}
        POST_BUILD
        COMMAND cp "${ORT_LIB}"
                   "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/${${PROJECT_NAME}_EXTERN_OUTPUT_NAME}.mxo/Contents/MacOS/"
)

add_custom_command(
        TARGET ${PROJECT_NAME}
        POST_BUILD
        COMMAND codesign -s - --deep --force
                "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/${${PROJECT_NAME}_EXTERN_OUTPUT_NAME}.mxo"
)
