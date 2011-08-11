// Sega Master System/Mark III, Sega Genesis/Mega Drive, BBC Micro VGM music file emulator

// Game_Music_Emu 0.5.5
#ifndef VGM_EMU_H
#define VGM_EMU_H

#include "blargg_common.h"
#include "blargg_source.h"
#include "resampler.h"
#include "multi_buffer.h"
#include "ym2413_emu.h"
#include "ym2612_emu.h"
#include "sms_apu.h"

typedef short sample_t;
typedef int vgm_time_t;
typedef int fm_time_t;

enum { fm_time_bits = 12 };
enum { blip_time_bits = 12 };
enum { buf_size = 2048 };

// VGM header format
enum { header_size = 0x40 };
struct header_t
{
	char tag [4];
	byte data_size [4];
	byte version [4];
	byte psg_rate [4];
	byte ym2413_rate [4];
	byte gd3_offset [4];
	byte track_duration [4];
	byte loop_offset [4];
	byte loop_duration [4];
	byte frame_rate [4];
	byte noise_feedback [2];
	byte noise_width;
	byte unused1;
	byte ym2612_rate [4];
	byte ym2151_rate [4];
	byte data_offset [4];
	byte unused2 [8];
};

enum { gme_max_field = 63 };
struct track_info_t
{
	/* times in milliseconds; -1 if unknown */
	long length;
	long intro_length;
	long loop_length;
	
	/* empty string if not available */
	char game      [64];
	char song      [96];
	char author    [64];
};

// Emulates VGM music using SN76489/SN76496 PSG, YM2612, and YM2413 FM sound chips.
// Supports custom sound buffer and frequency equalization when VGM uses just the PSG.
// FM sound chips can be run at their proper rates, or slightly higher to reduce
// aliasing on high notes. Currently YM2413 support requires that you supply a
// YM2413 sound chip emulator. I can provide one I've modified to work with the library.
struct Vgm_Emu {
	int fm_rate;
	long psg_rate;
	long vgm_rate;
	bool disable_oversampling;
	
	long fm_time_offset;
	int fm_time_factor;

	int blip_time_factor;
	
	byte const* file_begin;
	byte const* file_end;
	
	vgm_time_t vgm_time;
	byte const* loop_begin;
	byte const* pos;
	
	byte const* pcm_data;
	byte const* pcm_pos;
	int dac_amp;
	int dac_disabled; // -1 if disabled

	struct Blip_Buffer* blip_buf;

	// general
	long clock_rate_;
	unsigned buf_changed_count;
	int max_initial_silence;
	int voice_count;
	int mute_mask_;
	int tempo;
	int gain;
	
	long sample_rate;
	
	// track-specific
	blargg_long out_time;  // number of samples played since start of track
	blargg_long emu_time;  // number of samples emulator has generated since start of track
	bool emu_track_ended_; // emulator has reached end of track
	volatile bool track_ended;
	
	// fading
	blargg_long fade_start;
	int fade_step;
	
	// silence detection
	int silence_lookahead; // speed to run emulator when looking ahead for silence
	bool ignore_silence;
	long silence_time;     // number of samples where most recent silence began
	long silence_count;    // number of samples of silence to play before using buf
	long buf_remain;       // number of samples left in silence buffer
	
	// larger items at the end
	struct track_info_t info;
	sample_t buf_ [buf_size];

	struct Ym2612_Emu ym2612;
	struct Ym2413_Emu ym2413;
	
	struct Sms_Apu psg;
	struct Blip_Synth pcm;
	struct Stereo_Buffer stereo_buf;
	
	struct Resampler resampler;
	
	struct Stereo_Buffer buf;
};

void Vgm_init( struct Vgm_Emu* this );

// Disable running FM chips at higher than normal rate. Will result in slightly
// more aliasing of high notes.
static inline void Vgm_disable_oversampling( struct Vgm_Emu* this, bool disable ) { this->disable_oversampling = disable; }

// Header for currently loaded file
static inline struct header_t *header( struct Vgm_Emu* this ) { return (struct header_t*) this->file_begin; }
	
// Basic functionality (see Gme_File.h for file loading/track info functions)
blargg_err_t Vgm_load_mem( struct Vgm_Emu* this, byte const* new_data, long new_size, bool parse_info );

// True if any FM chips are used by file. Always false until init_fm()
// is called.
static inline bool uses_fm( struct Vgm_Emu* this ) { return Ym2612_enabled( &this->ym2612 ) || Ym2413_enabled( &this->ym2413 ); }

// Set output sample rate. Must be called only once before loading file.
blargg_err_t Vgm_set_sample_rate( struct Vgm_Emu* this, long sample_rate );
	
// Start a track, where 0 is the first track. Also clears warning string.
blargg_err_t Vgm_start_track( struct Vgm_Emu* this );
	
// Generate 'count' samples info 'buf'. Output is in stereo. Any emulation
// errors set warning string, and major errors also end track.
blargg_err_t Vgm_play( struct Vgm_Emu* this, long count, sample_t* buf ) ICODE_ATTR;
		
// Track status/control

// Number of milliseconds (1000 msec = 1 second) played since beginning of track
long Track_tell( struct Vgm_Emu* this );
	
// Seek to new time in track. Seeking backwards or far forward can take a while.
blargg_err_t Track_seek( struct Vgm_Emu* this, long msec );
	
// Skip n samples
blargg_err_t Track_skip( struct Vgm_Emu* this, long n );
		
// Set start time and length of track fade out. Once fade ends track_ended() returns
// true. Fade time can be changed while track is playing.
void Track_set_fade( struct Vgm_Emu* this, long start_msec, long length_msec );

// Get track length in milliseconds
static inline long Track_get_length( struct Vgm_Emu* this )
{
	long length = this->info.length;
	if ( length <= 0 )
	{
		length = this->info.intro_length + 2 * this->info.loop_length; // intro + 2 loops
		if ( length <= 0 )
			length = 150 * 1000; // 2.5 minutes
	}
	
	return length;
}
	
// Sound customization
	
// Adjust song tempo, where 1.0 = normal, 0.5 = half speed, 2.0 = double speed.
// Track length as returned by track_info() assumes a tempo of 1.0.
void Sound_set_tempo( struct Vgm_Emu* this, int t );
	
// Mute/unmute voice i, where voice 0 is first voice
void Sound_mute_voice( struct Vgm_Emu* this, int index, bool mute );
	
// Set muting state of all voices at once using a bit mask, where -1 mutes them all,
// 0 unmutes them all, 0x01 mutes just the first voice, etc.
void Sound_mute_voices( struct Vgm_Emu* this, int mask );
	
// Change overall output amplitude, where 1.0 results in minimal clamping.
// Must be called before set_sample_rate().
static inline void Sound_set_gain( struct Vgm_Emu* this, int g )
{
	assert( !this->sample_rate ); // you must set gain before setting sample rate
	this->gain = g;
}


#endif
