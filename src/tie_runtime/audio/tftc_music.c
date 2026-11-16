#include "tie_runtime/audio/tftc_music.h"

#include "aeron/audio.h"
#include "aeron/audio_decode.h"
#include "aeron/log.h"
#include "aeron/time.h"
#include "tie_runtime/audio/imuse_session.h"
#include "tie_runtime/audio/tftc_imc.h"
#include "tie_runtime/storage/storage.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* TIE's in-flight states (tie_updatemusic, fscript.c sdpArrays) and TFTC's
 * IMC state music for them in resources/tftc/music/ (its Reb and Fam files
 * are the same). The MIDI segment names abbreviate the track titles: tro
 * (Introduction), wait, reb (Rebels: StRival), pol (Polar Plotting:
 * StEmpire), intr (Intrusion: StPirate), chal, conf, panic, clim, fail,
 * succ. */
enum {
	TFTC_TRACK_INTRO = 1,
	TFTC_TRACK_WAITING = 2,
	TFTC_TRACK_FAILURE = 10,
	TFTC_TRACK_SUCCESS = 11,
	TFTC_TRACK_COUNT = 12,
};

static const char* const s_tracks[TFTC_TRACK_COUNT] = {
	NULL,
	"STINTROREB.IMC", "STWAITREB.IMC", "STRIVAL.IMC",	"STEMPIRE.IMC",	 "STPIRATE.IMC", "STCHALREB.IMC",
	"STCONFREB.IMC",  "STPANICREB.IMC", "STCLIMREB.IMC", "STFAILREB.IMC", "STSUCCREB.IMC",
};

/* The recordings in resources/tftc/signals/ of the bridges the MIDI script
 * plays between states (tro-in, tro-out, wait-in, wait-out, chal-in, fail-in,
 * fail-out, succ-in, succ-out) and of its sequences (fscript.c sequenceData).
 * Each is file number - 1. */
enum {
	TFTC_SIGNAL_NONE = -1,
	TFTC_SIGNAL_INTRO_START,
	TFTC_SIGNAL_INTO_INTRO_SOFT,
	TFTC_SIGNAL_INTO_INTRO_HARD,
	TFTC_SIGNAL_FROM_INTRO,
	TFTC_SIGNAL_INTO_WAIT,
	TFTC_SIGNAL_FROM_WAIT,
	TFTC_SIGNAL_INTO_FAIL,
	TFTC_SIGNAL_FROM_FAIL,
	TFTC_SIGNAL_INTO_SUCCESS,
	TFTC_SIGNAL_FROM_SUCCESS,
	TFTC_SIGNAL_INTO_CHALLENGE,
	TFTC_SIGNAL_WIN_1, /* s-win-1..4 */
	TFTC_SIGNAL_WIN_LARGE = TFTC_SIGNAL_WIN_1 + 4, /* s-win-lg */
	TFTC_SIGNAL_LOSS_SMALL,						   /* s-los-sm */
	TFTC_SIGNAL_LOSS_LARGE,						   /* s-los-lg */
	TFTC_SIGNAL_PRIMARY_FAILURE,				   /* s-ob1-fa */
	TFTC_SIGNAL_SECONDARY_FAILURE,				   /* s-ob2-fa */
	TFTC_SIGNAL_PRIMARY_SUCCESS,				   /* s-ob1-pa */
	TFTC_SIGNAL_SECONDARY_SUCCESS,				   /* s-ob2-pa */
	TFTC_SIGNAL_BONUS_SUCCESS,					   /* s-ob3-pa */
	TFTC_SIGNAL_NEUTRAL_SMALL,					   /* s-neu-sm */
	TFTC_SIGNAL_NEUTRAL_LARGE,					   /* s-neu-lg */
	TFTC_SIGNAL_IMPERIAL_SMALL,					   /* s-emp-sm */
	TFTC_SIGNAL_IMPERIAL_LARGE,					   /* s-emp-lg */
	TFTC_SIGNAL_REBELS_SMALL,					   /* s-reb-sm */
	TFTC_SIGNAL_REBELS_LARGE,					   /* s-reb-lg */
	TFTC_SIGNAL_EJECT,							   /* s-eject */
	TFTC_SIGNAL_HYPERSPACE,						   /* s-hyper */
	TFTC_SIGNAL_WIN_COUNT = 4,
};

static const char* const s_signals[] = {
	"01. 'Introduction' Start.flac",
	"02. Into 'Introduction' Soft.flac",
	"03. Into 'Introduction' Hard.flac",
	"04. From 'Introduction'.flac",
	"05. Into 'Wait'.flac",
	"06. From 'Wait'.flac",
	"07. Into 'Fail'.flac",
	"08. From 'Fail'.flac",
	"09. Into 'Success'.flac",
	"10. From 'Success'.flac",
	"11. Into 'Challenge'.flac",
	"12. Win 1.flac",
	"13. Win 2.flac",
	"14. Win 3.flac",
	"15. Win 4.flac",
	"16. Win Large.flac",
	"17. Loss Small.flac",
	"18. Loss Large.flac",
	"19. Primary Mission Objective - Failure.flac",
	"20. Secondary Mission Objective - Failure.flac",
	"21. Primary Mission Objective - Success.flac",
	"22. Secondary Mission Objective - Success.flac",
	"23. Bonus Mission Objective - Success.flac",
	"24. Neutral Small.flac",
	"25. Neutral Large.flac",
	"26. Imperial Small.flac",
	"27. Imperial Large.flac",
	"28. Rebels Small.flac",
	"29. Rebels Large.flac",
	"30. Eject.flac",
	"31. Hyperspace.flac",
};

#define TFTC_MUSIC_DIR "tftc/music/"
#define TFTC_SIGNALS_DIR "tftc/signals/"
#define TFTC_FRONTEND_DIR "tftc/"
#define TFTC_FADE_US 2000000u
/* A bridge takes over from the track it leaves, as the MIDI script's does at
 * a segment end; the old track fades out quickly under it. */
#define TFTC_BRIDGE_FADE_US 500000u
/* TIE re-evaluates its state every few seconds; the MIDI script only moves on
 * at segment ends. A track plays at least this long before another state
 * replaces it, except failure and success. */
#define TFTC_HOLD_US 10000000u
/* The state music's gain under a small-win sting, and the time to get there. */
#define TFTC_DUCK_GAIN 0.4f
#define TFTC_DUCK_US 250000u
/* The spacing of the markers a waiting sting starts on: a bar of the MIDI
 * score at about 120 beats a minute. */
#define TFTC_MARKER_US 2000000u
#define TFTC_BUFFER_SECONDS 2
#define TFTC_DECODE_FRAMES 4096
/* The recordings start on up to 1 s of silence and end on long fades: the
 * bridges are a short hit dying away over seconds. The start of a file below
 * -30 dBFS is skipped. Frames below the tail level are held back until
 * something louder follows; at the end of a file the next one starts there,
 * with the held tail mixed under its start. The tail level is -12 dBFS, or
 * 3 dB under the file's peak so far for the quiet bridges that never reach
 * -12 dBFS. */
#define TFTC_SILENCE 1036		  /* -30 dBFS */
#define TFTC_TAIL_LEVEL 8231	  /* -12 dBFS */
#define TFTC_TAIL_UNDER_PEAK 0.708f /* -3 dB */
/* Longer than any recording's fade below the tail level (about 7 s). */
#define TFTC_HELD_SECONDS 12
/* Front end music stops this quickly for the next piece. */
#define TFTC_ROOM_STOP_US 300000u
/* A file nothing follows, like a sting, fades its tail out over this. */
#define TFTC_END_FADE_MS 500

/* A file being played: an IMC state track, or a FLAC recording. */
typedef struct TieTftcSource {
	TieTftcImc* imc;
	AeronAudioDecoder* decoder;
} TieTftcSource;

/* A queued PCM stream fed from a music file: a state's track, possibly
 * behind the bridge into it, or a sting. */
typedef struct TieTftcVoice {
	int track; /* 0 outside flight state music */
	bool bridge; /* the decoder is on the bridge into `track` */
	bool loop;
	const char* file;			/* the file the decoder is on */
	const char* next;			/* the file to go on into at its end */
	const char* next_dir;
	uint64_t file_frames;		/* frames decoded from it */
	int file_peak;				/* its loudest sample so far */
	TieTftcSource* decoder; /* NULL once the stream only drains */
	AeronAudioStream stream;
	int rate;
	bool at_start; /* skipping the quiet start of the file */
	int16_t* held; /* quiet frames not yet known to be the file's end */
	size_t held_frames;
	size_t held_capacity;
	int16_t* mix; /* the previous file's tail, mixed under this one's start */
	size_t mix_frames;
	size_t mix_pos;
	float fade_from;
	float fade_to;
	uint64_t fade_start_us;
	uint32_t fade_us;
} TieTftcVoice;

static TieTftcVoice s_voice;  /* the current state's music */
static TieTftcVoice s_fading; /* the state music it replaced, fading out */
static TieTftcVoice s_sting;  /* a sting over the state music */
static TieTftcVoice s_sting_fading; /* a sting stopped by a screen change, fading out */
static int s_sting_waiting = TFTC_SIGNAL_NONE; /* the sting waiting to start */
static int s_sting_waiting_priority;
static uint64_t s_sting_due_us; /* its marker; 0 for the playing sting's end */
static int s_pending;		  /* the track TIE's state asks for */
static uint64_t s_track_us;	  /* when s_voice started */
static uint64_t s_update_us;
static float s_duck = 1.0f;
static float s_volume = 1.0f;
static float s_speech_duck = 1.0f; /* iMUSE's music ducking under speech */
static TieTftcVoice s_room;		   /* the front end music */
static TieTftcVoice s_room_fading; /* the room music it replaced, fading out */
static const char* s_room_file;
static bool s_room_requested; /* the new screen asked for its music */

static void TieTftcSource_Close(TieTftcSource* source) {
	if (!source)
		return;
	if (source->imc)
		TieTftcImc_Close(source->imc);
	if (source->decoder)
		Aeron_AudioDecoderClose(source->decoder);
	free(source);
}

static size_t TieTftcSource_Read(TieTftcSource* source, int16_t* pcm, size_t max_frames) {
	return source->imc ? TieTftcImc_Read(source->imc, pcm, max_frames)
					   : Aeron_AudioDecoderRead(source->decoder, pcm, max_frames);
}

static bool TieTftcSource_Rewind(TieTftcSource* source) {
	if (source->imc) {
		TieTftcImc_Rewind(source->imc);
		return true;
	}
	return Aeron_AudioDecoderSeekUs(source->decoder, 0);
}

/* An .IMC file decodes with TFTC's own codec, anything else with Aeron's. */
static TieTftcSource* TieTftcMusic_Open(const char* dir, const char* file, int* rate) {
	char path[96];
	snprintf(path, sizeof path, "%s%s", dir, file);
	TieTftcSource* source = calloc(1, sizeof *source);
	if (!source)
		return NULL;
	const size_t length = strlen(file);
	if (length > 4 && strcmp(file + length - 4, ".IMC") == 0) {
		source->imc = TieTftcImc_Open(path);
		*rate = TIE_TFTC_IMC_RATE;
	} else {
		source->decoder =
			Aeron_AudioDecoderOpenFile((AeronFile*)TieStorage_Open(TIE_FILE_ROOT_RESOURCE, path, "rb"), path);
		AeronAudioDecoderInfo info;
		Aeron_AudioDecoderGetInfo(source->decoder, &info);
		*rate = source->decoder ? info.sample_rate : 0;
	}
	if ((!source->imc && !source->decoder) || *rate <= 0) {
		Aeron_LogWarn("tie.music", "TFTC music: cannot play resources/%s", path);
		TieTftcSource_Close(source);
		return NULL;
	}
	return source;
}

static void TieTftcVoice_Close(TieTftcVoice* v) {
	if (v->decoder)
		TieTftcSource_Close(v->decoder);
	if (v->stream) {
		Aeron_AudioStreamPause(v->stream);
		Aeron_AudioStreamClose(v->stream);
	}
	free(v->held);
	free(v->mix);
	memset(v, 0, sizeof *v);
}

/* Starts `v` on `decoder` with a stream of its own, with room for the held
 * frames on top of TFTC_BUFFER_SECONDS. */
static bool TieTftcVoice_Start(TieTftcVoice* v, TieTftcSource* decoder, int rate, const char* file) {
	const size_t held_capacity = (size_t)rate * TFTC_HELD_SECONDS;
	int16_t* held = malloc(held_capacity * 2 * sizeof *held);
	int16_t* mix = malloc(held_capacity * 2 * sizeof *mix);
	AeronAudioStream stream =
		held && mix ? Aeron_AudioStreamOpen(rate, 2, AERON_PCM_S16,
											(size_t)rate * TFTC_BUFFER_SECONDS + held_capacity + TFTC_DECODE_FRAMES,
											0.0f)
					: 0;
	if (!stream) {
		Aeron_LogWarn("tie.music", "TFTC music: could not open a PCM stream");
		free(held);
		free(mix);
		TieTftcSource_Close(decoder);
		return false;
	}
	v->mix = mix;
	v->mix_frames = 0;
	v->mix_pos = 0;
	v->decoder = decoder;
	v->stream = stream;
	v->rate = rate;
	v->file = file;
	v->file_frames = 0;
	v->file_peak = 0;
	v->at_start = true;
	v->held = held;
	v->held_frames = 0;
	v->held_capacity = held_capacity;
	return true;
}

static void TieTftcVoice_ApplyGain(TieTftcVoice* v, uint64_t now, float duck) {
	const uint64_t elapsed = now - v->fade_start_us;
	const float fade = elapsed >= v->fade_us
						   ? v->fade_to
						   : v->fade_from + (v->fade_to - v->fade_from) * (float)elapsed / (float)v->fade_us;
	Aeron_AudioStreamSetGain(v->stream, s_volume * s_speech_duck * fade * duck);
}

/* At the end of a file the voice goes on into the file queued next (a
 * bridge's track, a queued sting) or loops; false when it has nothing more
 * to play. */
static bool TieTftcVoice_Continue(TieTftcVoice* v) {
	v->file_frames = 0;
	v->file_peak = 0;
	if (!v->next)
		return v->loop && TieTftcSource_Rewind(v->decoder);
	int rate;
	TieTftcSource* decoder = TieTftcMusic_Open(v->next_dir, v->next, &rate);
	if (decoder && rate != v->rate) {
		Aeron_LogWarn("tie.music", "TFTC music: %s is not at %d Hz", v->next, v->rate);
		TieTftcSource_Close(decoder);
		decoder = NULL;
	}
	v->bridge = false;
	if (!decoder) {
		v->next = NULL;
		return false;
	}
	TieTftcSource_Close(v->decoder);
	v->decoder = decoder;
	v->file = v->next;
	v->next = NULL;
	return true;
}

/* Queues `frames`, with whatever is left of the previous file's tail mixed
 * in. */
static void TieTftcVoice_Write(TieTftcVoice* v, const int16_t* pcm, size_t frames) {
	int16_t mixed[TFTC_DECODE_FRAMES * 2];
	while (frames) {
		const size_t n = frames < TFTC_DECODE_FRAMES ? frames : TFTC_DECODE_FRAMES;
		const int16_t* out = pcm;
		if (v->mix_pos < v->mix_frames) {
			const size_t m = n < v->mix_frames - v->mix_pos ? n : v->mix_frames - v->mix_pos;
			const int16_t* tail = v->mix + 2 * v->mix_pos;
			memcpy(mixed, pcm, n * 2 * sizeof *pcm);
			for (size_t i = 0; i < 2 * m; i++) {
				const int sum = mixed[i] + tail[i];
				mixed[i] = (int16_t)(sum > INT16_MAX ? INT16_MAX : sum < INT16_MIN ? INT16_MIN : sum);
			}
			v->mix_pos += m;
			out = mixed;
		}
		Aeron_AudioStreamWrite(v->stream, out, n);
		pcm += 2 * n;
		frames -= n;
	}
}

static void TieTftcVoice_Hold(TieTftcVoice* v, const int16_t* pcm, size_t frames) {
	/* A quiet stretch longer than the hold is a pause, not the end. */
	if (v->held_frames + frames > v->held_capacity) {
		TieTftcVoice_Write(v, v->held, v->held_frames);
		v->held_frames = 0;
	}
	memcpy(v->held + v->held_frames * 2, pcm, frames * 2 * sizeof *pcm);
	v->held_frames += frames;
}

/* Keeps TFTC_BUFFER_SECONDS queued, trimming the silence at either end of
 * each file; false once the voice has played out. The stream always has
 * room to take the held frames with a decoded block. */
static bool TieTftcVoice_Fill(TieTftcVoice* v) {
	int16_t pcm[TFTC_DECODE_FRAMES * 2];
	const size_t target = (size_t)v->rate * TFTC_BUFFER_SECONDS;
	while (v->decoder && Aeron_AudioStreamQueuedFrames(v->stream) < target &&
		   Aeron_AudioStreamWritableFrames(v->stream) >= v->held_frames + TFTC_DECODE_FRAMES) {
		const size_t decoded = TieTftcSource_Read(v->decoder, pcm, TFTC_DECODE_FRAMES);
		if (!decoded) {
			const bool silent_file = v->at_start;
			const char* ended = v->file;
			const double end_s = (double)(v->file_frames - v->held_frames) / v->rate;
			const double tail_s = (double)v->held_frames / v->rate;
			v->at_start = true;
			if (!silent_file && TieTftcVoice_Continue(v)) {
				/* The held tail plays under the next file's start. */
				int16_t* tail = v->held;
				v->held = v->mix;
				v->mix = tail;
				v->mix_frames = v->held_frames;
				v->mix_pos = 0;
				Aeron_LogInfo("tie.music", "TFTC music: %s hands over at %.2f s to %s%s, %.2f s of tail under it",
							  ended, end_s, v->file, v->file == ended ? " (loop)" : "", tail_s);
			} else {
				/* Nothing follows: the tail fades out. */
				const size_t fade = (size_t)v->rate * TFTC_END_FADE_MS / 1000;
				const size_t frames = v->held_frames < fade ? v->held_frames : fade;
				for (size_t i = 0; i < frames; i++) {
					const float gain = 1.0f - (float)i / (float)fade;
					v->held[2 * i] = (int16_t)(v->held[2 * i] * gain);
					v->held[2 * i + 1] = (int16_t)(v->held[2 * i + 1] * gain);
				}
				TieTftcVoice_Write(v, v->held, frames);
				TieTftcSource_Close(v->decoder);
				v->decoder = NULL;
				Aeron_LogInfo("tie.music", "TFTC music: %s ends at %.2f s, %.2f s of its %.2f s tail fades out",
							  ended, end_s, (double)frames / v->rate, tail_s);
			}
			v->held_frames = 0;
			continue;
		}
		v->file_frames += decoded;
		size_t first = decoded; /* the first frame above -30 dBFS */
		size_t last = decoded;	/* the last frame above the tail level */
		for (size_t i = 0; i < decoded; i++) {
			const int level = abs(pcm[2 * i]) > abs(pcm[2 * i + 1]) ? abs(pcm[2 * i]) : abs(pcm[2 * i + 1]);
			if (first == decoded && level > TFTC_SILENCE)
				first = i;
			if (level > v->file_peak)
				v->file_peak = level;
			const float under_peak = (float)v->file_peak * TFTC_TAIL_UNDER_PEAK;
			if (level > TFTC_TAIL_LEVEL || (float)level > under_peak)
				last = i;
		}
		if (v->at_start) {
			if (first == decoded)
				continue;
			v->at_start = false;
			Aeron_LogInfo("tie.music", "TFTC music: %s starts at %.2f s", v->file,
						  (double)(v->file_frames - decoded + first) / v->rate);
		} else {
			first = 0;
		}
		if (last == decoded) {
			TieTftcVoice_Hold(v, pcm + 2 * first, decoded - first);
			continue;
		}
		TieTftcVoice_Write(v, v->held, v->held_frames);
		v->held_frames = 0;
		TieTftcVoice_Write(v, pcm + 2 * first, last + 1 - first);
		TieTftcVoice_Hold(v, pcm + 2 * (last + 1), decoded - last - 1);
	}
	return v->decoder || Aeron_AudioStreamQueuedFrames(v->stream) > 0;
}

/* The bridge the MIDI script plays from state `from` (0 when the music
 * starts) into `to`: the transition records of fscript.c's SDP arrays. */
static int TieTftcMusic_Bridge(int from, int to) {
	switch (to) {
		case TFTC_TRACK_INTRO:
			if (!from)
				return TFTC_SIGNAL_INTRO_START;
			return rand() & 1 ? TFTC_SIGNAL_INTO_INTRO_SOFT : TFTC_SIGNAL_INTO_INTRO_HARD;
		case TFTC_TRACK_WAITING:
			return from > TFTC_TRACK_INTRO ? TFTC_SIGNAL_INTO_WAIT : TFTC_SIGNAL_NONE;
		case TFTC_TRACK_FAILURE:
			return from ? TFTC_SIGNAL_INTO_FAIL : TFTC_SIGNAL_NONE;
		case TFTC_TRACK_SUCCESS:
			return from ? TFTC_SIGNAL_INTO_SUCCESS : TFTC_SIGNAL_NONE;
		default: /* the combat states */
			switch (from) {
				case 0:
					return TFTC_SIGNAL_INTO_CHALLENGE;
				case TFTC_TRACK_INTRO:
					return TFTC_SIGNAL_FROM_INTRO;
				case TFTC_TRACK_WAITING:
					return TFTC_SIGNAL_FROM_WAIT;
				case TFTC_TRACK_FAILURE:
					return TFTC_SIGNAL_FROM_FAIL;
				case TFTC_TRACK_SUCCESS:
					return TFTC_SIGNAL_FROM_SUCCESS;
				default:
					return TFTC_SIGNAL_NONE;
			}
	}
}

static void TieTftcMusic_Switch(int track) {
	const uint64_t now = Aeron_NowUs();
	const int signal = TieTftcMusic_Bridge(s_voice.track, track);
	TieTftcVoice_Close(&s_fading);
	if (s_voice.stream) {
		s_fading = s_voice;
		s_fading.fade_from = 1.0f;
		s_fading.fade_to = 0.0f;
		s_fading.fade_start_us = now;
		s_fading.fade_us = signal != TFTC_SIGNAL_NONE ? TFTC_BRIDGE_FADE_US : TFTC_FADE_US;
		memset(&s_voice, 0, sizeof s_voice);
	}
	s_track_us = now;
	/* Kept without a stream when the files cannot play, so the state is not
	 * retried every frame. */
	s_voice.track = track;
	int rate;
	TieTftcSource* decoder =
		signal != TFTC_SIGNAL_NONE ? TieTftcMusic_Open(TFTC_SIGNALS_DIR, s_signals[signal], &rate) : NULL;
	const bool bridge = decoder != NULL;
	if (!decoder)
		decoder = TieTftcMusic_Open(TFTC_MUSIC_DIR, s_tracks[track], &rate);
	if (!decoder || !TieTftcVoice_Start(&s_voice, decoder, rate, bridge ? s_signals[signal] : s_tracks[track]))
		return;
	s_voice.bridge = bridge;
	s_voice.next = bridge ? s_tracks[track] : NULL;
	s_voice.next_dir = TFTC_MUSIC_DIR;
	s_voice.loop = true;
	/* Without a bridge the new track fades in under the old one. */
	s_voice.fade_from = s_fading.stream && !bridge ? 0.0f : 1.0f;
	s_voice.fade_to = 1.0f;
	s_voice.fade_start_us = now;
	s_voice.fade_us = TFTC_FADE_US;
	TieTftcVoice_Fill(&s_voice);
	TieTftcVoice_ApplyGain(&s_voice, now, s_duck);
	Aeron_AudioStreamPlay(s_voice.stream);
	if (bridge)
		Aeron_LogInfo("tie.music", "TFTC music: %s, then %s", s_signals[signal], s_tracks[track]);
	else
		Aeron_LogInfo("tie.music", "TFTC music: %s", s_tracks[track]);
}

/* The recorded sting for TIE music sequence `tie_sequence` (fscript.c
 * sequenceData, fscript_MsSetSequence ids), or TFTC_SIGNAL_NONE for TIE's
 * empty sequence 0. The small win (2) is one of the Win stings, not the one
 * before. */
static int TieTftcMusic_StingFor(int tie_sequence) {
	static const int stings[18] = {
		TFTC_SIGNAL_NONE,
		TFTC_SIGNAL_WIN_LARGE,
		TFTC_SIGNAL_WIN_1,
		TFTC_SIGNAL_LOSS_LARGE,
		TFTC_SIGNAL_LOSS_SMALL,
		TFTC_SIGNAL_PRIMARY_SUCCESS,
		TFTC_SIGNAL_PRIMARY_FAILURE,
		TFTC_SIGNAL_SECONDARY_SUCCESS,
		TFTC_SIGNAL_SECONDARY_FAILURE,
		TFTC_SIGNAL_BONUS_SUCCESS,
		TFTC_SIGNAL_IMPERIAL_LARGE,
		TFTC_SIGNAL_IMPERIAL_SMALL,
		TFTC_SIGNAL_REBELS_LARGE,
		TFTC_SIGNAL_REBELS_SMALL,
		TFTC_SIGNAL_NEUTRAL_LARGE,
		TFTC_SIGNAL_NEUTRAL_SMALL,
		TFTC_SIGNAL_EJECT,
		TFTC_SIGNAL_HYPERSPACE,
	};
	/* As fscript_ChooseDest: each Win sting plays once before any repeats. */
	static unsigned s_wins_left;
	if (tie_sequence < 0 || tie_sequence >= 18)
		return TFTC_SIGNAL_NONE;
	if (stings[tie_sequence] != TFTC_SIGNAL_WIN_1)
		return stings[tie_sequence];
	if (!s_wins_left)
		s_wins_left = (1u << TFTC_SIGNAL_WIN_COUNT) - 1u;
	int pick = rand() % __builtin_popcount(s_wins_left);
	int win = 0;
	for (; win < TFTC_SIGNAL_WIN_COUNT; ++win)
		if ((s_wins_left & (1u << win)) && pick-- == 0)
			break;
	s_wins_left &= ~(1u << win);
	return TFTC_SIGNAL_WIN_1 + win;
}

/* Starts `signal` over the state music. */
static void TieTftcMusic_StartSting(int signal) {
	TieTftcVoice_Close(&s_sting);
	int rate;
	TieTftcSource* decoder = TieTftcMusic_Open(TFTC_SIGNALS_DIR, s_signals[signal], &rate);
	if (!decoder || !TieTftcVoice_Start(&s_sting, decoder, rate, s_signals[signal]))
		return;
	s_sting.fade_from = 1.0f;
	s_sting.fade_to = 1.0f;
	s_sting.fade_us = 1;
	TieTftcVoice_Fill(&s_sting);
	TieTftcVoice_ApplyGain(&s_sting, Aeron_NowUs(), 1.0f);
	Aeron_AudioStreamPlay(s_sting.stream);
	Aeron_LogInfo("tie.music", "TFTC music: %s", s_sting.file);
}

/* fscript_PlaySequence and fcallbk.c: one sting waits for the music's next
 * marker, where it starts; a request while one waits replaces it only with a
 * strictly higher priority (fscript.c sequencePriorities), and is otherwise
 * dropped. A sting that has started has no priority, so the next request
 * waits for its end, its last marker. The recordings have no markers; a
 * TFTC_MARKER_US grid from the state track's start stands in for them. */
static void TieTftcMusic_PlaySting(int signal, int priority) {
	if (!s_voice.stream)
		return;
	const uint64_t now = Aeron_NowUs();
	if (s_sting_waiting != TFTC_SIGNAL_NONE) {
		if (s_sting_waiting_priority >= priority)
			return;
		Aeron_LogInfo("tie.music", "TFTC music: %s replaces %s waiting", s_signals[signal],
					  s_signals[s_sting_waiting]);
	} else if (s_sting.stream) {
		s_sting_due_us = 0; /* at the playing sting's end */
		Aeron_LogInfo("tie.music", "TFTC music: %s waits for %s to end", s_signals[signal], s_sting.file);
	} else {
		s_sting_due_us = now + TFTC_MARKER_US - (now - s_track_us) % TFTC_MARKER_US;
		Aeron_LogInfo("tie.music", "TFTC music: %s waits %.1f s for a marker", s_signals[signal],
					  (double)(s_sting_due_us - now) / 1e6);
	}
	s_sting_waiting = signal;
	s_sting_waiting_priority = priority;
}

/* Starts the waiting sting when its marker comes. */
static void TieTftcMusic_UpdateWaitingSting(uint64_t now) {
	if (s_sting_waiting == TFTC_SIGNAL_NONE || s_sting.stream || (s_sting_due_us && now < s_sting_due_us))
		return;
	const int signal = s_sting_waiting;
	s_sting_waiting = TFTC_SIGNAL_NONE;
	s_sting_waiting_priority = 0;
	TieTftcMusic_StartSting(signal);
}

bool TieTftcMusic_Start(void) { return true; }

void TieTftcMusic_Shutdown(void) {
	TieTftcVoice_Close(&s_voice);
	TieTftcVoice_Close(&s_fading);
	TieTftcVoice_Close(&s_sting);
	TieTftcVoice_Close(&s_sting_fading);
	TieTftcVoice_Close(&s_room);
	TieTftcVoice_Close(&s_room_fading);
	s_room_file = NULL;
	s_pending = 0;
	s_sting_waiting = TFTC_SIGNAL_NONE;
	s_sting_waiting_priority = 0;
}

/* tie_updatemusic's states: 1 no hostile near, 2 no hostile after combat,
 * 3/4/5 hostiles at mid range (Rebels, other, side 4 present), 6 even fight,
 * 7 winning, 8 missile lock or outnumbered, 9 one flight group from a goal,
 * 10 primary goals failed, 11 goals complete. Training uses 6..9 for gate
 * progress. Each state has its own track. */
void TieTftcMusic_SetTieState(int tie_state) {
	if (tie_state >= TFTC_TRACK_INTRO && tie_state < TFTC_TRACK_COUNT)
		s_pending = tie_state;
}

void TieTftcMusic_SetFailed(void) { TieTftcMusic_SetTieState(TFTC_TRACK_FAILURE); }

const char* TieTftcMusic_CurrentTune(void) { return s_voice.stream ? s_tracks[s_voice.track] : NULL; }

void TieTftcMusic_PlayRoom(const char* file, bool loop) {
	s_room_requested = true;
	/* A piece still playing carries on; one that has ended plays again. */
	if (file && s_room.stream && s_room_file && strcmp(file, s_room_file) == 0)
		return;
	const uint64_t now = Aeron_NowUs();
	TieTftcVoice_Close(&s_room_fading);
	if (s_room.stream) {
		/* The old piece stops quickly rather than crossfading. */
		s_room_fading = s_room;
		s_room_fading.fade_from = 1.0f;
		s_room_fading.fade_to = 0.0f;
		s_room_fading.fade_start_us = now;
		s_room_fading.fade_us = TFTC_ROOM_STOP_US;
		memset(&s_room, 0, sizeof s_room);
		Aeron_LogInfo("tie.music", "TFTC room music: %s stops", s_room_file);
	}
	s_room_file = file;
	if (!file)
		return;
	int rate;
	TieTftcSource* decoder = TieTftcMusic_Open(TFTC_FRONTEND_DIR, file, &rate);
	if (!decoder || !TieTftcVoice_Start(&s_room, decoder, rate, file))
		return;
	s_room.loop = loop;
	s_room.fade_from = 1.0f;
	s_room.fade_to = 1.0f;
	s_room.fade_start_us = now;
	s_room.fade_us = TFTC_FADE_US;
	TieTftcVoice_Fill(&s_room);
	TieTftcVoice_ApplyGain(&s_room, now, 1.0f);
	Aeron_AudioStreamPlay(s_room.stream);
	Aeron_LogInfo("tie.music", "TFTC room music: %s", file);
}

/* Stings belong to the screen they played on: a screen change stops the
 * playing one as quickly as front end music and drops one still waiting. */
static void TieTftcMusic_StopStings(void) {
	s_sting_waiting = TFTC_SIGNAL_NONE;
	s_sting_waiting_priority = 0;
	s_sting_due_us = 0;
	if (!s_sting.stream)
		return;
	TieTftcVoice_Close(&s_sting_fading);
	s_sting_fading = s_sting;
	s_sting_fading.fade_from = 1.0f;
	s_sting_fading.fade_to = 0.0f;
	s_sting_fading.fade_start_us = Aeron_NowUs();
	s_sting_fading.fade_us = TFTC_ROOM_STOP_US;
	memset(&s_sting, 0, sizeof s_sting);
	Aeron_LogInfo("tie.music", "TFTC music: %s stops for the screen change", s_sting_fading.file);
}

void TieTftcMusic_BeginRoomChange(void) {
	s_room_requested = false;
	TieTftcMusic_StopStings();
}

void TieTftcMusic_EndRoomChange(void) {
	if (!s_room_requested)
		TieTftcMusic_PlayRoom(NULL, false);
}

void TieTftcMusic_PauseRoom(bool paused) {
	TieTftcVoice* voices[] = { &s_room, &s_room_fading };
	for (size_t i = 0; i < sizeof voices / sizeof voices[0]; i++) {
		if (!voices[i]->stream)
			continue;
		if (paused)
			Aeron_AudioStreamPause(voices[i]->stream);
		else
			Aeron_AudioStreamPlay(voices[i]->stream);
	}
}

const char* TieTftcMusic_CurrentRoom(void) { return s_room.stream ? s_room_file : NULL; }

const char* TieTftcMusic_CurrentStings(void) {
	static char text[160];
	text[0] = '\0';
	if (s_voice.stream && s_voice.bridge)
		snprintf(text, sizeof text, "%s", s_voice.file);
	if (s_sting.stream)
		snprintf(text + strlen(text), sizeof text - strlen(text), "%s%s", text[0] ? " + " : "", s_sting.file);
	return text[0] ? text : NULL;
}

void TieTftcMusic_PlayTieSequence(int tie_sequence) {
	/* fscript.c sequencePriorities */
	static const int priorities[18] = { 0, 10, 2, 9, 1, 15, 14, 13, 12, 11, 8, 5, 7, 4, 6, 3, 20, 20 };
	const int signal = TieTftcMusic_StingFor(tie_sequence);
	if (signal != TFTC_SIGNAL_NONE)
		TieTftcMusic_PlaySting(signal, priorities[tie_sequence]);
}

void TieTftcMusic_Stop(void) {
	if (s_voice.stream)
		Aeron_LogInfo("tie.music", "TFTC music stopped");
	TieTftcVoice_Close(&s_voice);
	TieTftcVoice_Close(&s_fading);
	/* A sting playing as the mission ends, like Hyperspace, plays on until the
	 * screen changes; one still waiting for its marker starts now, with no
	 * music left to wait on. */
	if (s_sting_waiting != TFTC_SIGNAL_NONE && !s_sting.stream) {
		s_sting_due_us = 1;
		TieTftcMusic_UpdateWaitingSting(Aeron_NowUs());
	}
	s_pending = 0;
	s_duck = 1.0f;
}

void TieTftcMusic_SetVolume(int volume) {
	if (volume < 0)
		volume = 0;
	if (volume > 16)
		volume = 16;
	s_volume = (float)volume / 16.0f;
}

static void TieTftcVoice_UpdateFading(TieTftcVoice* v, uint64_t now, float duck) {
	if (!v->stream)
		return;
	if (now - v->fade_start_us >= v->fade_us || !TieTftcVoice_Fill(v))
		TieTftcVoice_Close(v);
	else
		TieTftcVoice_ApplyGain(v, now, duck);
}

/* A voice whose files fail keeps its track, so the state is not retried
 * every frame. */
static void TieTftcVoice_Update(TieTftcVoice* v, uint64_t now, float duck) {
	if (!v->stream)
		return;
	if (TieTftcVoice_Fill(v)) {
		TieTftcVoice_ApplyGain(v, now, duck);
	} else {
		const int track = v->track;
		TieTftcVoice_Close(v);
		v->track = track;
	}
}

void TieTftcMusic_Update(void) {
	const uint64_t now = Aeron_NowUs();
	const float step = s_update_us ? (float)(now - s_update_us) / (float)TFTC_DUCK_US : 0.0f;
	s_update_us = now;
	/* The Music Volume During Speech setting, as iMUSE applies it to MIDI. */
	s_speech_duck = TieImuseSession_MusicDuckGain();
	if (s_pending && s_pending != s_voice.track &&
		(!s_voice.stream || s_pending >= TFTC_TRACK_FAILURE || now - s_track_us >= TFTC_HOLD_US))
		TieTftcMusic_Switch(s_pending);

	/* The sting follows the volume and speech ducking as it plays. */
	if (s_sting.stream && !TieTftcVoice_Fill(&s_sting))
		TieTftcVoice_Close(&s_sting);
	else if (s_sting.stream)
		TieTftcVoice_ApplyGain(&s_sting, now, 1.0f);
	TieTftcMusic_UpdateWaitingSting(now);
	const float duck_target = s_sting.stream ? TFTC_DUCK_GAIN : 1.0f;
	if (s_duck > duck_target)
		s_duck = fmaxf(duck_target, s_duck - step * (1.0f - TFTC_DUCK_GAIN));
	else
		s_duck = fminf(duck_target, s_duck + step * (1.0f - TFTC_DUCK_GAIN));

	TieTftcVoice_UpdateFading(&s_fading, now, s_duck);
	TieTftcVoice_Update(&s_voice, now, s_duck);
	TieTftcVoice_UpdateFading(&s_sting_fading, now, 1.0f);
	TieTftcVoice_UpdateFading(&s_room_fading, now, 1.0f);
	TieTftcVoice_Update(&s_room, now, 1.0f);
}
