# CMake toolchain for iOS devices (arm64), used for the dependencies
# (build-deps.sh) and for the app. The same as the owner's DOOM 3 and Jedi
# Knight ports use.
set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_SYSTEM_PROCESSOR arm64)
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
set(CMAKE_OSX_SYSROOT "iphoneos" CACHE STRING "" FORCE)

# The minimum iOS version; CI and build-deps.sh pass the same one. It must
# match ANGLE's: dyld won't load a framework built for a newer iOS than the phone's.
if(DEFINED ENV{IOS_DEPLOYMENT_TARGET})
	set(CMAKE_OSX_DEPLOYMENT_TARGET $ENV{IOS_DEPLOYMENT_TARGET} CACHE STRING "" FORCE)
else()
	set(CMAKE_OSX_DEPLOYMENT_TARGET "18.0" CACHE STRING "" FORCE)
endif()

# Unsigned: CI signs the app ad hoc; a sideloading app signs it for real.
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO" CACHE STRING "" FORCE)
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO" CACHE STRING "" FORCE)

# Libraries and headers come from the dependency prefix only, never the Mac host.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
