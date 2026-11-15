#include "tie_runtime/audio/imuse_digital/imhost.h"

#include "aeron/log.h"
#include "aeron/time.h"
#include "tie_runtime/storage/storage.h"

#include <stdarg.h>
#include <stdio.h>

#define IMHOST_ROOT_PREFIX "tftc/"

static char g_imHostLogScratch[256];

static void ImHost_Log(int error, const char* kind, const char* fmt, va_list args) {
	vsnprintf(g_imHostLogScratch, sizeof g_imHostLogScratch, fmt, args);
	if (error)
		Aeron_LogError("tie.music", "iMUSE %s: %s", kind, g_imHostLogScratch);
	else
		Aeron_LogVerbose("tie.music", "iMUSE %s: %s", kind, g_imHostLogScratch);
}

void DebugPrintfChannel(int channel, const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	ImHost_Log(channel == 4, "debug", fmt, args);
	va_end(args);
}

int ImHost_PrintStatus(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	ImHost_Log(0, "status", fmt, args);
	va_end(args);
	return 0;
}

int ImHost_PrintMessage(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	ImHost_Log(0, "message", fmt, args);
	va_end(args);
	return 0;
}

int ImHost_PrintWarning(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	ImHost_Log(0, "warning", fmt, args);
	va_end(args);
	return 0;
}

int ImHost_PrintError(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	ImHost_Log(1, "error", fmt, args);
	va_end(args);
	return 0;
}

int ImHost_PrintDebug(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	ImHost_Log(0, "debug", fmt, args);
	va_end(args);
	return 0;
}

/* The original exits; a failed music assertion should not end the game. */
void ImHost_AssertFail(const char* expr, const char* file, int line) {
	Aeron_LogError("tie.music", "iMUSE assert: (%s:%d) %s", file, line, expr);
}

int ImHost_RegisterAtExit(void (*fn)(void)) { return atexit(fn); }
void* ImHost_AllocMem(unsigned int size) { return calloc(1, size); }
void ImHost_FreeMem(void* block) { free(block); }
void* ImHost_ReallocMem(void* block, unsigned int size) { return realloc(block, size); }
void* ImHost_AllocHandle(unsigned int size) { return calloc(1, size); }
void ImHost_FreeHandle(void* handle) { free(handle); }
void* ImHost_ReallocHandle(void* handle, unsigned int size) { return realloc(handle, size); }
void* ImHost_LockHandle(void* handle) { return handle; }
void ImHost_UnlockHandle(void* handle) { (void)handle; }

/* XWA ticks game time in milliseconds. */
int ImHost_GetTime(void) { return (int)(Aeron_NowUs() / 1000u); }

ImHostFile* ImHost_OpenFile(const char* path, const char* mode) {
	char full[256];
	if (!path || (mode && (mode[0] == 'w' || mode[0] == 'a')))
		return NULL;
	snprintf(full, sizeof full, IMHOST_ROOT_PREFIX "%s", path);
	TieFile* file = TieStorage_Open(TIE_FILE_ROOT_RESOURCE, full, "rb");
	if (!file)
		Aeron_LogWarn("tie.music", "cannot open resources/%s", full);
	else
		Aeron_LogInfo("tie.music", "opened resources/%s", full);
	return file;
}

int ImHost_CloseFile(ImHostFile* stream) { return stream ? TieStorage_Close(stream) : -1; }

unsigned int ImHost_ReadFile(ImHostFile* stream, void* dst, unsigned int size) {
	return stream ? (unsigned int)TieStorage_Read(dst, 1, size, stream) : 0;
}

char* ImHost_ReadLine(ImHostFile* stream, char* dst, int size) {
	if (!stream || size <= 0)
		return NULL;
	int length = 0;
	while (length < size - 1) {
		const int c = TieStorage_Getc(stream);
		if (c == TIE_EOF)
			break;
		dst[length++] = (char)c;
		if (c == '\n')
			break;
	}
	dst[length] = '\0';
	return length ? dst : NULL;
}

unsigned int ImHost_WriteFile(ImHostFile* stream, void* src, unsigned int size) {
	(void)stream;
	(void)src;
	(void)size;
	return 0;
}

int ImHost_TellFile(ImHostFile* stream) { return stream ? (int)TieStorage_Tell(stream) : -1; }

int ImHost_SeekFile(ImHostFile* stream, int offset, int origin) {
	return stream ? TieStorage_Seek(stream, offset, origin) : -1;
}

/* The music path never asks; matches OpenXWA. */
int ImHost_AtEof(ImHostFile* stream) {
	(void)stream;
	return 0;
}

int ImHost_GetFileSize(const char* path) {
	ImHostFile* file = ImHost_OpenFile(path, "rb");
	if (!file)
		return -1;
	const int size = (int)TieStorage_FileLength(file);
	TieStorage_Close(file);
	return size;
}

int ImHost_FilePrintf(ImHostFile* stream, const char* fmt, ...) {
	(void)stream;
	(void)fmt;
	return 0;
}
