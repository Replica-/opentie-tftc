#include "tie_runtime/audio/tftc_imc.h"

#include "aeron/log.h"
#include "tie_runtime/audio/imuse_digital/imuse.h"
#include "tie_runtime/storage/storage.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* The iMUS header is in the first block; this much of the decoded sound
 * always covers it. */
#define TIE_TFTC_IMC_HEADER_BYTES 4096u

struct TieTftcImc {
	uint8_t* file;
	uint32_t blocks;
	/* Per block (immap/imuse.c's ImMcmpStream): where its decoded bytes start
	 * in the sound (one more entry for the end), where its packed bytes are in
	 * the file, how many, and whether they are VIMA rather than stored. */
	uint32_t* start;
	uint32_t* packed_at;
	uint32_t* packed_size;
	uint8_t* vima;
	int16_t* block; /* the decoded block `cached` */
	int64_t cached;
	size_t data_at; /* the sound's PCM, in the decoded stream */
	size_t in_frames;
	int rate;
	int channels;
	size_t out_frames;
	size_t out_pos;
};

static uint32_t TieTftcImc_Be16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }

static uint32_t TieTftcImc_Be32(const uint8_t* p) {
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint8_t* TieTftcImc_ReadFile(const char* path, size_t* size) {
	TieFile* file = TieStorage_Open(TIE_FILE_ROOT_RESOURCE, path, "rb");
	if (!file)
		return NULL;
	const int32_t length = TieStorage_FileLength(file);
	uint8_t* data = length > 0 ? malloc((size_t)length) : NULL;
	if (data && TieStorage_Read(data, 1, (size_t)length, file) != (size_t)length) {
		free(data);
		data = NULL;
	}
	TieStorage_Close(file);
	*size = data ? (size_t)length : 0;
	return data;
}

/* Byte `offset` of the decoded sound, decoding its block if it is not the
 * one held. Reads go forward, so the search starts at the held block. */
static uint8_t TieTftcImc_Byte(TieTftcImc* imc, size_t offset) {
	if (offset >= imc->start[imc->blocks])
		return 0;
	uint32_t b = imc->cached >= 0 && offset >= imc->start[imc->cached] ? (uint32_t)imc->cached : 0;
	while (offset >= imc->start[b + 1])
		++b;
	if ((int64_t)b != imc->cached) {
		const uint32_t size = imc->start[b + 1] - imc->start[b];
		if (imc->vima[b]) {
			ImVimaState state;
			ImVimaResetState(&state);
			ImVimaDecodeBlock(&state, imc->block, (char*)imc->file + imc->packed_at[b], size);
		} else {
			memcpy(imc->block, imc->file + imc->packed_at[b], imc->packed_size[b] < size ? imc->packed_size[b] : size);
		}
		imc->cached = b;
	}
	return ((const uint8_t*)imc->block)[offset - imc->start[b]];
}

/* Channel `channel` of input frame `frame`: VIMA decodes to native 16-bit. */
static int TieTftcImc_Sample(TieTftcImc* imc, size_t frame, int channel) {
	const size_t offset = imc->data_at + 2 * ((size_t)imc->channels * frame + (size_t)channel);
	const uint8_t lo = TieTftcImc_Byte(imc, offset), hi = TieTftcImc_Byte(imc, offset + 1);
	return (int16_t)(uint16_t)(lo | hi << 8);
}

/* The MCMP block table: codec index, decoded and packed size per block, then
 * the codec names, then the packed blocks. */
static bool TieTftcImc_ReadTable(TieTftcImc* imc, size_t file_size) {
	const uint8_t* file = imc->file;
	if (file_size < 8 || memcmp(file, "MCMP", 4) != 0)
		return false;
	imc->blocks = TieTftcImc_Be16(file + 4);
	const size_t names_at = 6 + 9 * (size_t)imc->blocks;
	if (!imc->blocks || names_at + 2 > file_size)
		return false;
	const size_t names_size = TieTftcImc_Be16(file + names_at);
	const char* names = (const char*)file + names_at + 2;
	imc->start = malloc((imc->blocks + 1) * sizeof *imc->start);
	imc->packed_at = malloc(imc->blocks * sizeof *imc->packed_at);
	imc->packed_size = malloc(imc->blocks * sizeof *imc->packed_size);
	imc->vima = malloc(imc->blocks);
	if (!imc->start || !imc->packed_at || !imc->packed_size || !imc->vima)
		return false;
	size_t at = names_at + 2 + names_size, decoded = 0, largest = 0;
	for (uint32_t i = 0; i < imc->blocks; i++) {
		const uint8_t* entry = file + 6 + 9 * i;
		const uint32_t size = TieTftcImc_Be32(entry + 1), packed = TieTftcImc_Be32(entry + 5);
		if (at + packed > file_size)
			return false;
		const char* codec = names;
		for (uint32_t n = entry[0]; n && codec < names + names_size; n--)
			codec += strlen(codec) + 1;
		imc->vima[i] = codec + 4 <= names + names_size && memcmp(codec, "VIMA", 4) == 0;
		imc->start[i] = (uint32_t)decoded;
		imc->packed_at[i] = (uint32_t)at;
		imc->packed_size[i] = packed;
		decoded += size;
		largest = size > largest ? size : largest;
		at += packed;
	}
	imc->start[imc->blocks] = (uint32_t)decoded;
	/* Room for the WVSM path's whole 4096-byte chunks. */
	imc->block = malloc(largest + 4096);
	imc->cached = -1;
	return imc->block != NULL;
}

TieTftcImc* TieTftcImc_Open(const char* path) {
	size_t file_size;
	TieTftcImc* imc = calloc(1, sizeof *imc);
	if (!imc)
		return NULL;
	imc->file = TieTftcImc_ReadFile(path, &file_size);
	char header[TIE_TFTC_IMC_HEADER_BYTES];
	const char* data = NULL;
	int bits = 0, data_size = 0;
	if (imc->file && TieTftcImc_ReadTable(imc, file_size)) {
		const size_t total = imc->start[imc->blocks];
		const size_t n = total < sizeof header ? total : sizeof header;
		for (size_t i = 0; i < n; i++)
			header[i] = (char)TieTftcImc_Byte(imc, i);
		if (n >= 4 && memcmp(header, "iMUS", 4) == 0)
			data = ImParseSoundHeader(header, NULL, &imc->rate, &bits, &imc->channels, NULL, &data_size, NULL, NULL);
		if (data && (data < header || data > header + n))
			data = NULL;
	}
	if (!data || bits != 16 || imc->channels < 1 || imc->channels > 2 || imc->rate <= 0) {
		Aeron_LogWarn("tie.music", "TFTC music: cannot decode resources/%s", path);
		TieTftcImc_Close(imc);
		return NULL;
	}
	imc->data_at = (size_t)(data - header);
	const size_t available = imc->start[imc->blocks] - imc->data_at;
	imc->in_frames =
		(data_size > 0 && (size_t)data_size < available ? (size_t)data_size : available) / (2 * (size_t)imc->channels);
	imc->out_frames = (size_t)((uint64_t)imc->in_frames * TIE_TFTC_IMC_RATE / (uint64_t)imc->rate);
	return imc;
}

/* Linear resampling to TIE_TFTC_IMC_RATE; mono plays on both sides. */
size_t TieTftcImc_Read(TieTftcImc* imc, int16_t* pcm, size_t max_frames) {
	size_t n = 0;
	for (; n < max_frames && imc->out_pos < imc->out_frames; ++n, ++imc->out_pos) {
		const uint64_t pos = (uint64_t)imc->out_pos * (uint64_t)imc->rate * 65536u / TIE_TFTC_IMC_RATE;
		const size_t a = (size_t)(pos >> 16), b = a + 1 < imc->in_frames ? a + 1 : a;
		const int frac = (int)(pos & 0xFFFFu);
		for (int c = 0; c < 2; c++) {
			const int channel = c < imc->channels ? c : 0;
			const int sa = TieTftcImc_Sample(imc, a, channel), sb = TieTftcImc_Sample(imc, b, channel);
			pcm[2 * n + (size_t)c] = (int16_t)(sa + ((sb - sa) * frac >> 16));
		}
	}
	return n;
}

void TieTftcImc_Rewind(TieTftcImc* imc) { imc->out_pos = 0; }

void TieTftcImc_Close(TieTftcImc* imc) {
	if (!imc)
		return;
	free(imc->file);
	free(imc->start);
	free(imc->packed_at);
	free(imc->packed_size);
	free(imc->vima);
	free(imc->block);
	free(imc);
}
