cmake_minimum_required(VERSION 3.24)

project(CharleneSKSEPlayground
    VERSION 0.1.0
    LANGUAGES C CXX
)
set(PROJECT_AUTHOR "Charlene Hoo")
set(PROJECT_AUTHOR_EMAIL "CharleneHoo@hotmail.com")
set(PLUGIN_HEADER "${CMAKE_CURRENT_BINARY_DIR}/src/Plugin.h")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Plugin.h.in"
    "${PLUGIN_HEADER}"
    @ONLY
)

set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/build/_deps" CACHE PATH "Shared FetchContent cache")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    add_compile_options(-Wno-unused-command-line-argument)
endif()
add_subdirectory(extern/CommonLibSSE-NG SYSTEM EXCLUDE_FROM_ALL)
if(TARGET CommonLibSSE)
    target_compile_options(CommonLibSSE PRIVATE
        $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-w>
        $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-w>
        $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/w>
    )
endif()

add_library("${PROJECT_NAME}" SHARED
    src/main.cpp
)

target_include_directories(
    "${PROJECT_NAME}" PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/src"
)

target_link_libraries(${PROJECT_NAME} PRIVATE CommonLibSSE::CommonLibSSE)

target_precompile_headers("${PROJECT_NAME}" PRIVATE src/pch.h)

if(CMAKE_EXPORT_COMPILE_COMMANDS)
    add_custom_target(copy_compile_commands ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_BINARY_DIR}/compile_commands.json"
        "${CMAKE_SOURCE_DIR}/compile_commands.json"
        COMMENT "Syncing compile_commands.json to source root"
        VERBATIM
    )
endif()
