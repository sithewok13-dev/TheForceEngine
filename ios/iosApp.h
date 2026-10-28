#pragma once
// The app around The Force Engine on iOS (ios/IOSGameData.m), for the engine's
// C++ code.
#ifdef TFE_IOS
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Called first thing in main(): sound on silent, then, until the player's
// Dark Forces files are in the app's Documents folder, a screen saying what
// to copy where. Returns once they are there.
void IOS_AppInit(void);

// The folder holding DARK.GOB, found in Documents (directly, or in a game
// folder copied there, up to two levels down), with a trailing slash.
// Returns false when there is none.
bool IOS_FindGameFolder(char* outPath, size_t size);

// The app's Documents folder (shown in Files), with a trailing slash.
const char* IOS_DocumentsPath(void);

#ifdef __cplusplus
}
#endif
#endif // TFE_IOS
