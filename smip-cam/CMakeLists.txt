cmake_minimum_required(VERSION 3.20)
project(SimpCam LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "SimpCam yalnızca Windows üzerinde çalışır.")
endif()

add_executable(SimpCam WIN32 src/main.cpp)
target_compile_features(SimpCam PRIVATE cxx_std_17)
target_compile_definitions(SimpCam PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(SimpCam PRIVATE mf mfplat mfreadwrite mfuuid ole32)

if(MSVC)
    target_compile_options(SimpCam PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()
