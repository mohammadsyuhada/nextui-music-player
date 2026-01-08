#ifndef __PLAYER_H__
#define __PLAYER_H__

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

// Audio format types
typedef enum {
    AUDIO_FORMAT_UNKNOWN = 0,
    AUDIO_FORMAT_WAV,
    AUDIO_FORMAT_MP3,
    AUDIO_FORMAT_OGG,
    AUDIO_FORMAT_FLAC,
    AUDIO_FORMAT_MOD
} AudioFormat;

// Player states
typedef enum {
    PLAYER_STATE_STOPPED = 0,
    PLAYER_STATE_PLAYING,
    PLAYER_STATE_PAUSED
} PlayerState;

// Track metadata
typedef struct {
    char title[256];
    char artist[256];
    char album[256];
    int duration_ms;        // Total duration in milliseconds
    int sample_rate;
    int channels;
    int bitrate;
} TrackInfo;

// Waveform overview data
#define WAVEFORM_BARS 128  // Number of bars in waveform display
typedef struct {
    float bars[WAVEFORM_BARS];  // Amplitude values 0.0-1.0 for each bar
    int bar_count;
    bool valid;
} WaveformData;

// Player context
typedef struct {
    // State
    PlayerState state;
    AudioFormat format;

    // Current track
    char current_file[512];
    TrackInfo track_info;

    // Playback
    int position_ms;        // Current position in milliseconds
    float volume;           // 0.0 to 1.0
    bool repeat;            // Loop current track

    // Audio buffer for visualization
    int16_t vis_buffer[2048];  // Stereo samples for FFT
    int vis_buffer_pos;
    pthread_mutex_t vis_mutex;

    // Internal
    void* audio_data;       // Loaded audio data
    size_t audio_size;
    void* mixer_sound;      // audio_mixer_sound_t*
    void* mixer_voice;      // audio_mixer_voice_t*

    // SDL Audio
    int audio_device;
    bool audio_initialized;

    // Threading
    pthread_t decode_thread;
    bool thread_running;
    pthread_mutex_t mutex;
} PlayerContext;

// Initialize the player
int Player_init(void);

// Cleanup the player
void Player_quit(void);

// Load a file (does not start playing)
int Player_load(const char* filepath);

// Preload a file in the background (for faster track transitions)
void Player_preload(const char* filepath);

// Start/resume playback
int Player_play(void);

// Pause playback
void Player_pause(void);

// Stop playback and unload
void Player_stop(void);

// Toggle play/pause
void Player_togglePause(void);

// Seek to position (in milliseconds)
void Player_seek(int position_ms);

// Set volume (0.0 to 1.0)
void Player_setVolume(float volume);

// Get current volume
float Player_getVolume(void);

// Get current state
PlayerState Player_getState(void);

// Get current position in milliseconds
int Player_getPosition(void);

// Get track duration in milliseconds
int Player_getDuration(void);

// Get track info
const TrackInfo* Player_getTrackInfo(void);

// Get current file path
const char* Player_getCurrentFile(void);

// Get visualization buffer (for spectrum analyzer)
// Returns number of samples copied
int Player_getVisBuffer(int16_t* buffer, int max_samples);

// Get waveform overview data (for static waveform progress display)
const WaveformData* Player_getWaveform(void);

// Check if a file format is supported
AudioFormat Player_detectFormat(const char* filepath);

// Update player (call this in main loop)
void Player_update(void);

// Resume/pause audio device (used by radio module)
void Player_resumeAudio(void);
void Player_pauseAudio(void);

// Reset audio device to default 48000 Hz sample rate (for radio use)
void Player_resetSampleRate(void);

// Set audio device to specific sample rate
void Player_setSampleRate(int sample_rate);

// Check if Bluetooth audio is currently active
bool Player_isBluetoothActive(void);

#endif
