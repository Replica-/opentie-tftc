#ifndef TIE_RUNTIME_AUDIO_IMUSE_DIGITAL_IMHOST_H
#define TIE_RUNTIME_AUDIO_IMUSE_DIGITAL_IMHOST_H

/* Host services for the digital iMUSE engine, ported from OpenXWA
 * (src/xwa/audio/imuse). Files resolve under resources/tftc/ through TIE
 * storage, so the engine's "music/StIntroReb.IMC" opens
 * resources/tftc/music/StIntroReb.IMC. */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void ImHostFile;

int ImHost_PrintStatus(const char* fmt, ...);
int ImHost_PrintMessage(const char* fmt, ...);
int ImHost_PrintWarning(const char* fmt, ...);
int ImHost_PrintError(const char* fmt, ...);
int ImHost_PrintDebug(const char* fmt, ...);
void ImHost_AssertFail(const char* expr, const char* file, int line);
int ImHost_RegisterAtExit(void (*fn)(void));
void* ImHost_AllocMem(unsigned int size);
void ImHost_FreeMem(void* block);
void* ImHost_ReallocMem(void* block, unsigned int size);
void* ImHost_LockHandle(void* handle);
int ImHost_GetTime(void);
ImHostFile* ImHost_OpenFile(const char* path, const char* mode);
int ImHost_CloseFile(ImHostFile* stream);
unsigned int ImHost_ReadFile(ImHostFile* stream, void* dst, unsigned int size);
char* ImHost_ReadLine(ImHostFile* stream, char* dst, int size);
unsigned int ImHost_WriteFile(ImHostFile* stream, void* src, unsigned int size);
int ImHost_TellFile(ImHostFile* stream);
int ImHost_SeekFile(ImHostFile* stream, int offset, int origin);
int ImHost_AtEof(ImHostFile* stream);
int ImHost_GetFileSize(const char* path);
int ImHost_FilePrintf(ImHostFile* stream, const char* fmt, ...);
void* ImHost_AllocHandle(unsigned int size);
void ImHost_FreeHandle(void* handle);
void* ImHost_ReallocHandle(void* handle, unsigned int size);
void ImHost_UnlockHandle(void* handle);

/* OpenXWA's runtime helpers the engine sources call. */
void* memcpy_0(void* dst, const void* src, size_t size);
#define Memory_AllocTagged(tag, size) ((void)(tag), calloc(1, (size)))
#define Memory_FreeTagged(tag, block) ((void)(tag), free(block))
#define Memory_ReallocTagged(tag, block, size) ((void)(tag), realloc((block), (size)))
void DebugPrintfChannel(int channel, const char* fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
