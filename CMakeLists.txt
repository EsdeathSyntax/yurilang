cmake_minimum_required(VERSION 3.15)
project(YuriLang C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET default)
set(CMAKE_VISIBILITY_INLINES_HIDDEN OFF)

find_package(Terminfo QUIET)

find_package(LLVM REQUIRED CONFIG)
include_directories(${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})

file(GLOB_RECURSE PROJ_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
)

file(GLOB_RECURSE PROJ_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
    "${CMAKE_CURRENT_SOURCE_DIR}/std/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/std/*.cxx"
    "${CMAKE_CURRENT_SOURCE_DIR}/std/*.cc"
)

add_executable(yurilang ${PROJ_SOURCES} ${PROJ_HEADERS})

set_target_properties(yurilang PROPERTIES ENABLE_EXPORTS ON)

target_include_directories(yurilang PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")

llvm_map_components_to_libnames(llvm_libs core support jitlink native orcjit passes)
target_link_libraries(yurilang PRIVATE
    ${llvm_libs}
    ffi
)
