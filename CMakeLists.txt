cmake_minimum_required(VERSION 3.16)
project(sdl3_template C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(SDL3 REQUIRED CONFIG)

find_program(GLSLC_EXECUTABLE glslc REQUIRED)

set(SHADER_SOURCES
    shaders/triangle.vert
    shaders/triangle.frag
)

set(SHADER_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT_DIR})

set(SHADER_OUTPUTS)
foreach(SHADER IN LISTS SHADER_SOURCES)
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    set(SPV_OUTPUT ${SHADER_OUT_DIR}/${SHADER_NAME}.spv)
    add_custom_command(
        OUTPUT ${SPV_OUTPUT}
        COMMAND ${GLSLC_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/${SHADER} -o ${SPV_OUTPUT}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${SHADER}
        COMMENT "glslc ${SHADER}"
        VERBATIM
    )
    list(APPEND SHADER_OUTPUTS ${SPV_OUTPUT})
endforeach()

add_custom_target(shaders DEPENDS ${SHADER_OUTPUTS})

add_library(qrcodegen STATIC vendor/qrcodegen/qrcodegen.c)
target_include_directories(qrcodegen PUBLIC vendor/qrcodegen)

add_executable(${PROJECT_NAME} src/main.c)
add_dependencies(${PROJECT_NAME} shaders)

target_include_directories(${PROJECT_NAME} PRIVATE include)
target_link_libraries(${PROJECT_NAME} PRIVATE SDL3::SDL3 qrcodegen)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()
