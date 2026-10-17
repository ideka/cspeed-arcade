cmake_minimum_required(VERSION 3.20)
project(CaliforniaSpeedArcade C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 11)
add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

file(GLOB GENERATED_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/generated/*.c)
add_library(GuestCode STATIC ${GENERATED_SOURCES})
target_include_directories(GuestCode PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
# The R5000 rounds after each multiply in madd.s; keep the compiler from fusing.
target_compile_options(GuestCode PRIVATE /clang:-ffp-contract=off -Wno-unused-label -Wno-unused-value -Wno-parentheses-equality -Wno-tautological-compare)

# libchdr reads the hard disk image (CHD).
set(CHDR_WANT_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/lib/libchdr EXCLUDE_FROM_ALL)

file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/runtime/*.cpp)
add_executable(CaliforniaSpeedArcade ${RUNTIME_SOURCES})
target_include_directories(CaliforniaSpeedArcade PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
target_link_libraries(CaliforniaSpeedArcade PRIVATE GuestCode chdr-static dbghelp)

