# The Dark Forces app around The Force Engine: bundle id, Info.plist, the
# first-launch screen, TFE's support files, ANGLE and the icon. Included from
# the top-level CMakeLists.txt on iOS, with IOS_APP_DIR pointing here.

set(IOS_BUNDLE_ID "com.sithewok.darkforces" CACHE STRING "The app's bundle id")
set(IOS_VERSION "1.0" CACHE STRING "CFBundleShortVersionString")
set(IOS_BUILD_NUMBER "1" CACHE STRING "CFBundleVersion (CI passes its run number)")
set(IOS_EXECUTABLE "DarkForces")

if(NOT ANGLE_ROOT)
	message(FATAL_ERROR "iOS needs ANGLE: pass -DANGLE_ROOT=<dir with include/ and out/ios/lib*.framework>")
endif()

# The app's own code: the first-launch screen and paths (Objective-C, ARC),
# and the ANGLE context in place of SDL's OpenGL (C++)
set(IOS_APP_OBJC_SOURCES
	"${IOS_APP_DIR}/IOSGameData.m"
)
target_sources(tfe PRIVATE ${IOS_APP_OBJC_SOURCES} "${IOS_APP_DIR}/iosAngle.cpp")
set_source_files_properties(${IOS_APP_OBJC_SOURCES} PROPERTIES COMPILE_FLAGS "-fobjc-arc")
target_include_directories(tfe PRIVATE "${IOS_APP_DIR}" "${ANGLE_ROOT}/include")

configure_file("${IOS_APP_DIR}/Info.plist.in" "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" @ONLY)
set_target_properties(tfe PROPERTIES
	MACOSX_BUNDLE TRUE
	OUTPUT_NAME "${IOS_EXECUTABLE}"
	MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
)

# What static SDL2 builds on, in case its package doesn't pass them all on.
# (No OpenGLES: ANGLE draws instead.)
target_link_libraries(tfe PRIVATE
	"-framework AVFoundation" "-framework AudioToolbox" "-framework CoreAudio"
	"-framework CoreBluetooth" "-framework CoreGraphics" "-framework CoreHaptics"
	"-framework CoreMotion" "-framework Foundation" "-framework GameController"
	"-framework Metal" "-framework QuartzCore"
	"-framework UIKit" "-framework UniformTypeIdentifiers"
)

# ANGLE (OpenGL ES on Metal): its two frameworks, linked by @rpath and copied
# into the app's Frameworks folder
set(ANGLE_FRAMEWORK_DIR "${ANGLE_ROOT}/out/ios")
target_link_libraries(tfe PRIVATE
	"-F${ANGLE_FRAMEWORK_DIR}" "-framework libEGL" "-framework libGLESv2"
	"-Wl,-rpath,@executable_path/Frameworks"
)

set(IOS_BUNDLE_DIR "$<TARGET_BUNDLE_DIR:tfe>")
add_custom_command(TARGET tfe POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E make_directory "${IOS_BUNDLE_DIR}/Frameworks"
	COMMAND ${CMAKE_COMMAND} -E copy_directory "${ANGLE_FRAMEWORK_DIR}/libEGL.framework" "${IOS_BUNDLE_DIR}/Frameworks/libEGL.framework"
	COMMAND ${CMAKE_COMMAND} -E copy_directory "${ANGLE_FRAMEWORK_DIR}/libGLESv2.framework" "${IOS_BUNDLE_DIR}/Frameworks/libGLESv2.framework"
	COMMENT "Bundling ANGLE"
	VERBATIM)

# TFE's own support files (shaders, fonts, UI images and text, captions, the
# manual, sound fonts, external data, the built-in AdjustableHud mod), at the
# top of the bundle, where TFE looks next to its executable. None of this is
# game data.
foreach(dir Captions Documentation ExternalData Fonts Mods Shaders SoundFonts UI_Images UI_Text)
	add_custom_command(TARGET tfe POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/TheForceEngine/${dir}" "${IOS_BUNDLE_DIR}/${dir}"
		VERBATIM)
endforeach()
# (the mod's CMakeLists.txt is only for desktop installs)
add_custom_command(TARGET tfe POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E rm -f "${IOS_BUNDLE_DIR}/Mods/TFE/AdjustableHud/CMakeLists.txt"
	VERBATIM)

# The icon and the launch screen colour: compile the asset catalog into the
# bundle, as Xcode would, and add the icon keys actool reports to Info.plist.
add_custom_command(TARGET tfe POST_BUILD
	COMMAND xcrun actool "${IOS_APP_DIR}/Assets.xcassets"
		--compile "${IOS_BUNDLE_DIR}"
		--platform iphoneos
		--minimum-deployment-target ${CMAKE_OSX_DEPLOYMENT_TARGET}
		--target-device iphone --target-device ipad
		--app-icon AppIcon
		--output-partial-info-plist "${CMAKE_CURRENT_BINARY_DIR}/assets-Info.plist"
		--output-format human-readable-text --notices --warnings
	COMMAND /usr/libexec/PlistBuddy -c "Merge ${CMAKE_CURRENT_BINARY_DIR}/assets-Info.plist" "${IOS_BUNDLE_DIR}/Info.plist"
	COMMENT "Compiling the asset catalog (icon) into the app"
	VERBATIM)
