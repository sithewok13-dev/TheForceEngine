file(GLOB_RECURSE SOURCES "*.cpp" "*.c")

set(ARCHFLAGS "${CMAKE_SYSTEM_PROCESSOR}")

if(IOS)
	# iOS (arm64): AngelScript's native calling convention code for Apple's
	# arm64, in the Xcode assembler's syntax; nothing from the branches below.
	list(APPEND SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Angelscript/angelscript/source/as_callfunc_arm64_xcode.S")
	set(ARCHFLAGS "ios")
endif()

if (APPLE AND NOT IOS)
    # TODO: Would this break building on armv7 Linux?
    if (${CMAKE_SYSTEM_PROCESSOR} MATCHES "arm")
        set(ARCHFLAGS "aarch64")
    endif ()
endif ()

if(${ARCHFLAGS} MATCHES "^arm")
   if(CMAKE_ASM_COMPILER_WORKS)
       list(APPEND SOURCES
           TheForceEngine/TFE_ForceScript/Angelscript/angelscript/source/as_callfunc_arm_gcc.S)
       set_property(SOURCE
           TheForceEngine/TFE_ForceScript/Angelscript/angelscript/source/as_callfunc_arm_gcc.S
           APPEND PROPERTY COMPILE_FLAGS " -Wa,-mimplicit-it=always")
   else()
       message(FATAL ERROR "ARM target requires a working assembler")
   endif()
endif()

if(${ARCHFLAGS} MATCHES "^aarch64")
   if(CMAKE_ASM_COMPILER_WORKS)
       if(NOT APPLE)
           list(APPEND SOURCES
               TheForceEngine/TFE_ForceScript/Angelscript/angelscript/source/as_callfunc_arm64_gcc.S)
       else()
           list(APPEND SOURCES
               TheForceEngine/TFE_ForceScript/Angelscript/angelscript/source/as_callfunc_arm64_xcode.S)
       endif()
   else()
       message(FATAL ERROR "ARM target requires a working assembler")
   endif()
endif()

target_sources(tfe PRIVATE ${SOURCES})
