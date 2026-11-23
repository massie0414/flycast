target_sources(${PROJECT_NAME} PRIVATE
        audiostream.h)

if(NOT LIBRETRO)
    if(WIN32 AND NOT WINDOWS_STORE)
        target_sources(${PROJECT_NAME} PRIVATE level_meter.cpp level_meter.h)
    endif()
    target_sources(${PROJECT_NAME} PRIVATE
            audiobackend_alsa.cpp
            audiobackend_coreaudio.cpp
            audiobackend_directsound.cpp
            audiobackend_libao.cpp
            audiobackend_null.cpp
            audiobackend_oboe.cpp
            audiobackend_omx.cpp
            audiobackend_oss.cpp
            audiobackend_pulseaudio.cpp
            audiobackend_sdl2.cpp
            audiostream.cpp)
endif()
