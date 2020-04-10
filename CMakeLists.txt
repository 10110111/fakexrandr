cmake_minimum_required(VERSION 3.10.0)
project(FakeXRandR VERSION 0.1)

find_package(PkgConfig REQUIRED)
pkg_check_modules(X11 REQUIRED x11)
pkg_check_modules(XRANDR REQUIRED xrandr)
pkg_check_modules(XCB REQUIRED xcb)
pkg_check_modules(XCBRANDR REQUIRED xcb-randr)

add_executable(xcbtest xcbtest.c)
target_include_directories(xcbtest PUBLIC ${XCB_INCLUDE_DIRS} ${XCBRANDR_INCLUDE_DIRS} ${X11_INCLUDE_DIRS} ${XRANDR_INCLUDE_DIRS})
target_link_libraries(xcbtest ${XCB_LINK_LIBRARIES} ${XCBRANDR_LINK_LIBRARIES} ${X11_LINK_LIBRARIES} ${XRANDR_LINK_LIBRARIES})

include_directories(${CMAKE_BINARY_DIR})

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -nostdlib") # don't link to libstdc++ and libgcc_s

macro(make_library genericName sourceFile origSoname headerForSkeleton funcPrefix prefixedTypes cflags includeDirs)
    add_custom_command(OUTPUT ${genericName}-orig-lib.h DEPENDS xcbtest
        COMMAND sh -c "ldd xcbtest | sed -n 's@^\\s*${origSoname} *=> *\\([^ ]\\+\\).*@#define REAL_LIB \"\\1\"@p' > ${genericName}-orig-lib.h"
        VERBATIM)
    add_custom_command(OUTPUT preprocessed_${genericName}.h
        COMMAND sh -c "printf '#include <${headerForSkeleton}>' | ${CMAKE_C_COMPILER} ${cflags} ${CMAKE_C_FLAGS} -E -xc - -o preprocessed_${genericName}.h"
        VERBATIM)
    add_custom_command(OUTPUT skeleton-${genericName}.h DEPENDS ${CMAKE_SOURCE_DIR}/make_skeleton.py preprocessed_${genericName}.h ${sourceFile}
        COMMAND sh -c "\
${CMAKE_SOURCE_DIR}/make_skeleton.py ${CMAKE_BINARY_DIR}/preprocessed_${genericName}.h ${funcPrefix} ${CMAKE_SOURCE_DIR}/${sourceFile} ${prefixedTypes} > \
skeleton-${genericName}.h || { rm -f skeleton-${genericName}.h; exit 1; }"
        VERBATIM)
    add_library(fake-${genericName} SHARED ${sourceFile} ${genericName}-orig-lib.h skeleton-${genericName}.h)
    target_include_directories(fake-${genericName} PUBLIC ${includeDirs})
    target_link_libraries(fake-${genericName} dl c)
endmacro()

make_library(Xrandr libXrandr.c libXrandr.so.2 "X11/extensions/Xrandr.h" "XRR" "RRCrtc,RROutput" "${XRANDR_CFLAGS}" "${XRANDR_INCLUDE_DIRS}")
make_library(xcb-randr libxcb-randr.cpp libxcb-randr.so.0 "xcb/randr.h" "xcb_randr_" "xcb_randr_output_t,xcb_randr_crtc_t" "${XCB_CFLAGS}" "${XCB_INCLUDE_DIRS}" "${XCB_LINK_LIBRARIES}")

if(NOT LIB_INSTALL_PREFIX)
    install(CODE "
cmake_policy(SET CMP0007 NEW)
# Find the directory where the original library resides
file(READ ${CMAKE_BINARY_DIR}/xcb-randr-orig-lib.h origLibDefine)
string(REGEX REPLACE \"#define REAL_LIB \\\"(.+)/[^/]+\\\\.so\\\\.0\\\".*\" \"\\\\1\" origLibPath \${origLibDefine})
# Find all the library search paths that precede that of the original library
execute_process(COMMAND sh -c \"ldconfig -v 2>/dev/null | sed -n 's@^\\\\(/[^:]*\\\\):.*@\\\\1@p' | grep -B999 '^\${origLibPath}\\\$' | head -n -1\" OUTPUT_VARIABLE searchPaths)
# Transform them to a list
string(REPLACE \"\\n\" \";\" searchPaths \${searchPaths})
list(REMOVE_ITEM searchPaths \"\")
list(LENGTH searchPaths searchPathCount)
if(searchPathCount LESS 1)
    message(FATAL_ERROR \"Failed to find a suitable installation directory for libraries. Such a
directory must precede \${origLibPath} in dynamic loader's search list. You may
want to add one to /etc/ld.so.conf file or create an entry in
/etc/ld.so.conf.d/.
To force a particular directory, set LIB_INSTALL_PREFIX to its path.\")
endif()
# Try some good candidates
list(FIND searchPaths /usr/local/lib usrLocalLibIndex)
list(FIND searchPaths /usr/lib usrLibIndex)
list(GET searchPaths 0 LIB_INSTALL_PREFIX) # default to first element
if(usrLocalLibIndex GREATER -1)
    set(LIB_INSTALL_PREFIX /usr/local/lib)
elseif(usrLibIndex GREATER -1)
    set(LIB_INSTALL_PREFIX /usr/lib)
endif()
    ")
else()
    install(CODE "set(LIB_INSTALL_PREFIX ${LIB_INSTALL_PREFIX})")
endif()
install(FILES "${CMAKE_BINARY_DIR}/libfake-xcb-randr.so" DESTINATION "/\${LIB_INSTALL_PREFIX}")
install(FILES "${CMAKE_BINARY_DIR}/libfake-Xrandr.so" DESTINATION "/\${LIB_INSTALL_PREFIX}")
install(FILES "${CMAKE_SOURCE_DIR}/fakexrandr-manage.py" DESTINATION "bin")
