set (TARGET le_font)

depends_on_island_module(le_path)

set (SOURCES "le_font.cpp")
set (SOURCES ${SOURCES} "le_font.h")
set (SOURCES ${SOURCES} "3rdparty/stb_truetype.h")
set (SOURCES ${SOURCES} "3rdparty/stb_truetype.cpp")
set (SOURCES ${SOURCES} "3rdparty/stb_rect_pack.h")

if (${PLUGINS_DYNAMIC})

    add_library(${TARGET} SHARED ${SOURCES})

    
    add_dynamic_linker_flags()
    
    target_compile_definitions(${TARGET} PUBLIC "PLUGINS_DYNAMIC")

else()

    add_library(${TARGET} STATIC ${SOURCES})

    add_static_lib( ${TARGET} )

endif()

# set (LINKER_FLAGS ${LINKER_FLAGS} stdc++fs)

target_link_libraries(${TARGET} PUBLIC ${LINKER_FLAGS})

source_group(${TARGET} FILES ${SOURCES})
