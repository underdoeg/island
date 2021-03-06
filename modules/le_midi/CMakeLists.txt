set( RTMIDI_BUILD_TESTING  OFF CACHE INTERNAL "" )

if (${PLUGINS_DYNAMIC})
    set( BUILD_SHARED_LIBS ON)
endif()

add_subdirectory( 3rdparty/rtmidi )

set (TARGET le_midi)

# list modules this module depends on
# depends_on_island_module(le_path)

set (SOURCES "le_midi.cpp")
set (SOURCES ${SOURCES} "le_midi.h")

if (${PLUGINS_DYNAMIC})

    add_library(${TARGET} SHARED ${SOURCES})

    # IMPORTANT: --no-gnu-unique
    #            This compiler flag is neccesary as otherwise the library may get compiled
    #            with some symbols exported as UNIQUE, which implicitly makes this library
    #            un-closeable. This means, any calls to `dlclose` on this library, once
    #            loaded, will have no effect, and autoreload for this library will not work
    #            as the first version of the library will remain resident.
    target_compile_options (${TARGET} PUBLIC --no-gnu-unique)
    
    target_compile_definitions(${TARGET}  PUBLIC "PLUGINS_DYNAMIC")

else()

    add_library(${TARGET} STATIC ${SOURCES})

    set (STATIC_LIBS ${STATIC_LIBS} ${TARGET} PARENT_SCOPE)

endif()

# set (LINKER_FLAGS ${LINKER_FLAGS} stdc++fs)

if (WIN32)
    set (LINKER_FLAGS ${LINKER_FLAGS} rtmidi)
else()
    set (LINKER_FLAGS -Wl,--whole-archive rtmidi -Wl,--no-whole-archive)
endif()

target_link_libraries(${TARGET} PUBLIC ${LINKER_FLAGS})

source_group(${TARGET} FILES ${SOURCES})