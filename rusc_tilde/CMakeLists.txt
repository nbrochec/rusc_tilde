include(${C74_MIN_API_DIR}/script/min-package.cmake)
include(${C74_MIN_API_DIR}/script/min-pretarget.cmake)

include_directories("${C74_INCLUDES}")

set(SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/rusc_tilde.cpp)

add_library(${PROJECT_NAME} MODULE ${SOURCE_FILES})

target_link_libraries(${PROJECT_NAME} PRIVATE clap)

include(${C74_MIN_API_DIR}/script/min-posttarget.cmake)

set(EXTERN_NAME "${${PROJECT_NAME}_EXTERN_OUTPUT_NAME}")   # rusc~

if(APPLE)
    # Fresh bundle each build, onnxruntime dylib inside it, loaded via @loader_path
    add_custom_command(TARGET ${PROJECT_NAME} PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E remove_directory
                "${C74_LIBRARY_OUTPUT_DIRECTORY}/${EXTERN_NAME}.mxo"
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "${C74_LIBRARY_OUTPUT_DIRECTORY}/${EXTERN_NAME}.mxo/Contents/MacOS"
        COMMENT "Removing old ${EXTERN_NAME}.mxo"
    )

    set_target_properties(${PROJECT_NAME} PROPERTIES
        BUILD_WITH_INSTALL_RPATH FALSE
        LINK_FLAGS "-Wl,-rpath,@loader_path/"
    )

    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ORT_RUNTIME}"
                "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/${EXTERN_NAME}.mxo/Contents/MacOS/"
        COMMAND codesign -s - --deep --force
                "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/${EXTERN_NAME}.mxo"
        COMMENT "Bundling onnxruntime and signing ${EXTERN_NAME}.mxo"
    )
elseif(WIN32)
    # onnxruntime.dll must be reachable by Max at load time. It is copied next to
    # the .mxe64; in an installed package put it in the package's support/ folder.
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ORT_RUNTIME}"
                "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/"
        COMMENT "Copying onnxruntime.dll next to ${EXTERN_NAME}.mxe64"
    )
endif()
