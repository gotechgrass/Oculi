cmake_minimum_required(VERSION 3.15)
project(Oculi VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Audio Engine Library
add_library(oculi_audio
    audio_analysis.h
    audio_analysis.cpp
    audio_capture.h
    audio_capture.cpp
)

target_include_directories(oculi_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    target_link_libraries(oculi_audio PUBLIC ole32)
endif()

# Standalone Terminal Eye
add_executable(eye eye.cpp)

# Desktop Eye Widget (Windows only)
if(WIN32)
    add_executable(eye_widget WIN32 eye_widget.cpp)
    target_link_libraries(eye_widget PRIVATE oculi_audio gdi32 user32)
endif()

# Audio Engine Tests
add_executable(test_audio test_audio.cpp)
target_link_libraries(test_audio PRIVATE oculi_audio)

# Live WASAPI Loopback Interactive Test
add_executable(test_live_wasapi test_live_wasapi.cpp)
target_link_libraries(test_live_wasapi PRIVATE oculi_audio)
