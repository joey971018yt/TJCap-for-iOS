cmake_minimum_required(VERSION 3.20)
project(ios_usb_vcam VERSION 1.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/W4 /wd4324 /utf-8 /EHsc /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
    if(WIN32)
        add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
    endif()
endif()

# Core cross-platform protocol, image scaling, shared-memory IPC, and GitHub Releases updater library
add_library(ios_vcam_core STATIC
    src/protocol/coremedia_codec.cpp
    src/protocol/qt_session.cpp
    src/common/image_convert.cpp
    src/common/shared_ipc.cpp
    src/common/app_updater.cpp
)
target_include_directories(ios_vcam_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if(UNIX AND NOT APPLE)
    target_link_libraries(ios_vcam_core PUBLIC pthread rt)
endif()

# Automated unit tests (buildable on both Linux and Windows)
add_executable(ios_vcam_tests
    tests/test_protocol_and_pipeline.cpp
)
target_link_libraries(ios_vcam_tests PRIVATE ios_vcam_core)
if(WIN32)
    target_link_libraries(ios_vcam_tests PRIVATE winhttp bcrypt shell32)
endif()
if(MINGW)
    target_link_options(ios_vcam_tests PRIVATE -static -static-libgcc -static-libstdc++)
endif()
add_test(NAME protocol_and_pipeline_tests COMMAND ios_vcam_tests)

# Windows-native targets: DirectShow Virtual Capture Card DLL, Host Daemon, Driver Setup, and GUI App
if(WIN32)
    # 1. DirectShow Video + Audio Virtual Capture Card DLL
    add_library(ios_vcam_dshow SHARED
        src/dshow/dll_main.cpp
        src/dshow/video_capture_filter.cpp
        src/dshow/audio_capture_filter.cpp
        src/dshow/ios_vcam_dshow.def
        src/common/binary_version.rc
    )
    target_include_directories(ios_vcam_dshow PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/dshow
    )
    target_link_libraries(ios_vcam_dshow PRIVATE
        ios_vcam_core
        strmiids
        ole32
        oleaut32
        uuid
        advapi32
        user32
        gdi32
        winmm
    )
    set_target_properties(ios_vcam_dshow PROPERTIES
        PREFIX ""
        OUTPUT_NAME "ios_vcam_dshow"
    )
    if(MINGW)
        target_link_options(ios_vcam_dshow PRIVATE
            -static
            -static-libgcc
            -static-libstdc++
            -Wl,--kill-at
        )
    endif()

    # 2. iOS USB Capture, Hardware H.264 Decoder & WASAPI Audio Router Host Executable
    add_executable(ios_vcam_host
        src/host/host_main.cpp
        src/usb/usb_ios_transport.cpp
        src/decoder/mf_h264_decoder.cpp
        src/decoder/wasapi_renderer.cpp
        src/common/binary_version.rc
    )
    target_include_directories(ios_vcam_host PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(ios_vcam_host PRIVATE
        ios_vcam_core
        ole32
        oleaut32
        uuid
        mfplat
        mfuuid
        d3d11
        dxgi
        avrt
        winmm
        advapi32
        user32
    )
    if(MINGW)
        target_link_options(ios_vcam_host PRIVATE -static -static-libgcc -static-libstdc++)
    endif()

    # 3. Safe UpperFilter Driver & DirectShow Registration Tool
    add_executable(ios_vcam_driver_setup
        src/driver_tool/driver_setup_main.cpp
        src/common/binary_version.rc
    )
    target_include_directories(ios_vcam_driver_setup PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(ios_vcam_driver_setup PRIVATE
        setupapi
        newdev
        cfgmgr32
        advapi32
        ole32
        shell32
        user32
    )
    if(MINGW)
        target_link_options(ios_vcam_driver_setup PRIVATE -static -static-libgcc -static-libstdc++)
    endif()

    # 4. One-Click Graphical App (ios_vcam_gui.exe) with custom App Icon, USB Speed Probe & PerMonitorV2 DPI manifest
    set(IOS_VCAM_GUI_EXTRA_SRCS src/gui/ios_vcam_gui.rc)
    if(MSVC)
        list(APPEND IOS_VCAM_GUI_EXTRA_SRCS src/gui/ios_vcam_gui.manifest)
    endif()
    add_executable(ios_vcam_gui WIN32
        src/gui/gui_main.cpp
        src/usb/usb_ios_transport.cpp
        src/decoder/wasapi_renderer.cpp
        ${IOS_VCAM_GUI_EXTRA_SRCS}
    )
    target_include_directories(ios_vcam_gui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(ios_vcam_gui PRIVATE
        ios_vcam_core
        setupapi
        comctl32
        gdi32
        user32
        shell32
        advapi32
        ole32
        oleaut32
        uuid
        winmm
        winhttp
        bcrypt
    )
    if(MINGW)
        target_compile_definitions(ios_vcam_gui PRIVATE IOS_VCAM_EMBED_RC_MANIFEST=1)
        target_link_options(ios_vcam_gui PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
endif()
