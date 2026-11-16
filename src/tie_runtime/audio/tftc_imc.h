#ifndef TIE_RUNTIME_AUDIO_TFTC_IMC_H
#define TIE_RUNTIME_AUDIO_TFTC_IMC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The rate TFTC's IMC music plays at here, that of the FLAC bridges it
 * hands over to and from. */
#define TIE_TFTC_IMC_RATE 44100

/* One of TFTC's digital iMUSE files (an MCMP container of VIMA blocks
 * holding an iMUS sound), decoded a block at a time as it is read, start to
 * end: the iMUS map's regions and jumps are not followed. */
typedef struct TieTftcImc TieTftcImc;

/* Opens resources/`path`; NULL when it is missing or not an IMC sound. */
TieTftcImc* TieTftcImc_Open(const char* path);
/* Up to `max_frames` interleaved stereo 16-bit frames at TIE_TFTC_IMC_RATE;
 * 0 at the end. */
size_t TieTftcImc_Read(TieTftcImc* imc, int16_t* pcm, size_t max_frames);
void TieTftcImc_Rewind(TieTftcImc* imc);
void TieTftcImc_Close(TieTftcImc* imc);

#ifdef __cplusplus
}
#endif

#endif
