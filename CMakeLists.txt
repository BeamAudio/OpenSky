cmake_minimum_required(VERSION 3.18)
project(OpenSky VERSION 0.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(OPENSKY_PLAYER_PACKAGE "Build player-facing release binary without developer editor link" ON)
option(OPENSKY_PORTABLE "Use generic x86-64 tuning instead of build-host CPU instructions" ON)

add_subdirectory(engine/thirdparty/jsbsim)

set(SIM_SOURCES
    sim_engine/src/gl_loader.cpp
    sim_engine/src/flight_profiler.cpp
    sim_engine/src/platform_io.cpp
    sim_engine/src/platform_window.cpp
    sim_engine/src/pack_reader.cpp
    sim_engine/src/async_streamer.cpp
    sim_engine/src/cdlod_terrain.cpp
    sim_engine/src/volumetric_clouds.cpp
    sim_engine/src/regional_atmosphere.cpp
    sim_engine/src/weather_library.cpp
    sim_engine/src/world_objects.cpp
    sim_engine/src/living_traffic.cpp
    sim_engine/src/airfield_vegetation.cpp
    sim_engine/src/audio_engine.cpp
    sim_engine/src/aircraft_package.cpp
    sim_engine/src/aircraft_hud.cpp
    sim_engine/src/game_frontend.cpp
    sim_engine/src/aircraft_session.cpp
    sim_engine/src/fleet_profile.cpp
    sim_engine/src/debug_overlays.cpp
    sim_engine/src/flight_replay.cpp
    sim_engine/src/runtime_checks.cpp
    sim_engine/src/main.cpp
    engine/src/c172_systems.cpp
    engine/src/flight_model_jsb.cpp
    engine/src/flight_yoke.cpp
    engine/src/opentrack_udp.cpp
    sim_engine/third_party/bc7enc.cpp
    sim_engine/third_party/miniz_tinfl.c
)

function(opensky_apply_compile_options target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W3 /O2 /fp:fast /wd4100 /wd4244 /wd4267)
    else()
        target_compile_options(${target_name} PRIVATE -O3 -fno-math-errno -Wall -Wextra -Wno-unused-parameter)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(${target_name} PRIVATE
                $<$<COMPILE_LANGUAGE:CXX>:-ffp-contract=fast -fopenmp-simd>
            )
            if(OPENSKY_PORTABLE)
                target_compile_options(${target_name} PRIVATE -mtune=generic)
            else()
                target_compile_options(${target_name} PRIVATE -mtune=native)
            endif()
        endif()
    endif()
    if(OPENSKY_PLAYER_PACKAGE)
        target_compile_definitions(${target_name} PRIVATE OPENSKY_PLAYER_PACKAGE=1)
    endif()
endfunction()

add_executable(OpenSky ${SIM_SOURCES})
target_include_directories(OpenSky PRIVATE
    sim_engine/third_party
    engine/src
    protocol
)
target_include_directories(OpenSky SYSTEM PRIVATE
    engine/thirdparty/jsbsim/src
)
opensky_apply_compile_options(OpenSky)
target_link_libraries(OpenSky PRIVATE libJSBSim)

if(WIN32)
    target_link_libraries(OpenSky PRIVATE
        opengl32
        gdi32
        user32
        winmm
        winhttp
        dinput8
        dxguid
        ws2_32
        psapi
        shell32
    )
    set_target_properties(OpenSky PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/sim_engine"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/sim_engine"
    )
    add_custom_command(TARGET OpenSky POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:libJSBSim>
            "$<TARGET_FILE_DIR:OpenSky>/$<TARGET_FILE_NAME:libJSBSim>"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/game/bin"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:libJSBSim>
            "${CMAKE_SOURCE_DIR}/game/bin/$<TARGET_FILE_NAME:libJSBSim>"
    )
    if(MINGW)
        get_filename_component(_mingw_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        foreach(_rt_dll libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll)
            if(EXISTS "${_mingw_bin_dir}/${_rt_dll}")
                add_custom_command(TARGET OpenSky POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_mingw_bin_dir}/${_rt_dll}"
                        "$<TARGET_FILE_DIR:OpenSky>/${_rt_dll}"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_mingw_bin_dir}/${_rt_dll}"
                        "${CMAKE_SOURCE_DIR}/game/bin/${_rt_dll}"
                )
            endif()
        endforeach()
    endif()
else()
    target_link_libraries(OpenSky PRIVATE X11 dl pthread m)
    set_target_properties(OpenSky PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/sim_engine"
        BUILD_RPATH "$ORIGIN/../game/bin"
    )
endif()

add_executable(test_flight_and_systems
    engine/tests/test_flight_and_systems.cpp
    engine/src/flight_model_jsb.cpp
    engine/src/c172_systems.cpp
)
target_include_directories(test_flight_and_systems PRIVATE engine/src protocol)
target_include_directories(test_flight_and_systems SYSTEM PRIVATE engine/thirdparty/jsbsim/src)
opensky_apply_compile_options(test_flight_and_systems)
if(MSVC)
    target_compile_options(test_flight_and_systems PRIVATE /UNDEBUG)
else()
    target_compile_options(test_flight_and_systems PRIVATE -UNDEBUG)
endif()
target_link_libraries(test_flight_and_systems PRIVATE libJSBSim)
if(WIN32)
    add_custom_command(TARGET test_flight_and_systems POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:libJSBSim>
            "$<TARGET_FILE_DIR:test_flight_and_systems>/$<TARGET_FILE_NAME:libJSBSim>"
    )
    if(MINGW)
        foreach(_rt_dll libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll)
            if(EXISTS "${_mingw_bin_dir}/${_rt_dll}")
                add_custom_command(TARGET test_flight_and_systems POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_mingw_bin_dir}/${_rt_dll}"
                        "$<TARGET_FILE_DIR:test_flight_and_systems>/${_rt_dll}"
                )
            endif()
        endforeach()
    endif()
endif()

add_executable(test_geo_and_pack
    engine/tests/test_geo_and_pack.cpp
)
target_include_directories(test_geo_and_pack PRIVATE engine/src protocol)
opensky_apply_compile_options(test_geo_and_pack)
if(MSVC)
    target_compile_options(test_geo_and_pack PRIVATE /UNDEBUG)
else()
    target_compile_options(test_geo_and_pack PRIVATE -UNDEBUG)
endif()
if(WIN32 AND MINGW)
    foreach(_rt_dll libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll)
        if(EXISTS "${_mingw_bin_dir}/${_rt_dll}")
            add_custom_command(TARGET test_geo_and_pack POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_mingw_bin_dir}/${_rt_dll}"
                    "$<TARGET_FILE_DIR:test_geo_and_pack>/${_rt_dll}"
            )
        endif()
    endforeach()
endif()
