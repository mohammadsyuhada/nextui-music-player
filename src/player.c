#include "player.h"
#include "radio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <samplerate.h>
#include <SDL2/SDL_image.h>

#include "defines.h"
#include "api.h"
#include "msettings.h"

// Include dr_libs for audio decoding (header-only libraries)
#define DR_MP3_IMPLEMENTATION
#include "audio/dr_mp3.h"

#define DR_FLAC_IMPLEMENTATION
#include "audio/dr_flac.h"

#define DR_WAV_IMPLEMENTATION
#include "audio/dr_wav.h"

// For OGG we use stb_vorbis (implementation is in the .c file renamed to .h)
#include "audio/stb_vorbis.h"

// Sample rates for different audio outputs
#define SAMPLE_RATE_BLUETOOTH 44100  // 44.1kHz for Bluetooth A2DP compatibility
#define SAMPLE_RATE_SPEAKER   48000  // 48kHz for speaker output
#define SAMPLE_RATE_USB_DAC   48000  // 48kHz for USB DAC output
#define SAMPLE_RATE_DEFAULT   48000  // Default fallback

#define AUDIO_CHANNELS 2
#define AUDIO_SAMPLES 2048  // Smaller buffer for lower latency

// Global player context
static PlayerContext player = {0};
static int64_t audio_position_samples = 0;  // Track position in samples for precision
static WaveformData waveform = {0};  // Waveform overview for progress display
static int current_sample_rate = SAMPLE_RATE_DEFAULT;  // Track current SDL audio device rate
static bool bluetooth_audio_active = false;  // Track if Bluetooth audio is active

// Get target sample rate based on current audio sink
static int get_target_sample_rate(void) {
    if (bluetooth_audio_active) {
        return SAMPLE_RATE_BLUETOOTH;  // 44100 Hz for Bluetooth
    }
    // Check audio sink from msettings
    int sink = GetAudioSink();
    switch (sink) {
        case AUDIO_SINK_BLUETOOTH:
            return SAMPLE_RATE_BLUETOOTH;  // 44100 Hz
        case AUDIO_SINK_USBDAC:
            return SAMPLE_RATE_USB_DAC;    // 48000 Hz
        default:
            return SAMPLE_RATE_SPEAKER;    // 48000 Hz for speaker
    }
}

// Forward declaration for audio device change callback
static void audio_device_change_callback(int device_type, int event);

// ============ STREAMING PLAYBACK SYSTEM ============

// Decode chunk size (~0.5 seconds at 48kHz)
#define DECODE_CHUNK_FRAMES 24000

// Circular buffer functions
static int circular_buffer_init(CircularBuffer* cb, size_t capacity_frames) {
    cb->buffer = malloc(capacity_frames * sizeof(int16_t) * AUDIO_CHANNELS);
    if (!cb->buffer) {
        LOG_error("Failed to allocate circular buffer (%zu KB)\n",
                  capacity_frames * sizeof(int16_t) * AUDIO_CHANNELS / 1024);
        return -1;
    }
    cb->capacity = capacity_frames;
    cb->write_pos = 0;
    cb->read_pos = 0;
    cb->available = 0;
    pthread_mutex_init(&cb->mutex, NULL);
    return 0;
}

static void circular_buffer_free(CircularBuffer* cb) {
    if (cb->buffer) {
        free(cb->buffer);
        cb->buffer = NULL;
    }
    pthread_mutex_destroy(&cb->mutex);
    cb->capacity = 0;
    cb->write_pos = 0;
    cb->read_pos = 0;
    cb->available = 0;
}

static void circular_buffer_clear(CircularBuffer* cb) {
    pthread_mutex_lock(&cb->mutex);
    cb->write_pos = 0;
    cb->read_pos = 0;
    cb->available = 0;
    pthread_mutex_unlock(&cb->mutex);
}

static size_t circular_buffer_available(CircularBuffer* cb) {
    pthread_mutex_lock(&cb->mutex);
    size_t avail = cb->available;
    pthread_mutex_unlock(&cb->mutex);
    return avail;
}

// Write frames to circular buffer (called by decode thread)
static size_t circular_buffer_write(CircularBuffer* cb, int16_t* data, size_t frames) {
    pthread_mutex_lock(&cb->mutex);

    size_t space = cb->capacity - cb->available;
    size_t to_write = (frames < space) ? frames : space;

    if (to_write == 0) {
        pthread_mutex_unlock(&cb->mutex);
        return 0;
    }

    // Write in two parts if wrapping
    size_t first_part = cb->capacity - cb->write_pos;
    if (first_part > to_write) first_part = to_write;

    memcpy(&cb->buffer[cb->write_pos * AUDIO_CHANNELS], data,
           first_part * sizeof(int16_t) * AUDIO_CHANNELS);

    size_t second_part = to_write - first_part;
    if (second_part > 0) {
        memcpy(cb->buffer, &data[first_part * AUDIO_CHANNELS],
               second_part * sizeof(int16_t) * AUDIO_CHANNELS);
    }

    cb->write_pos = (cb->write_pos + to_write) % cb->capacity;
    cb->available += to_write;

    pthread_mutex_unlock(&cb->mutex);
    return to_write;
}

// Read frames from circular buffer (called by audio callback)
static size_t circular_buffer_read(CircularBuffer* cb, int16_t* data, size_t frames) {
    pthread_mutex_lock(&cb->mutex);

    size_t to_read = (frames < cb->available) ? frames : cb->available;

    if (to_read == 0) {
        pthread_mutex_unlock(&cb->mutex);
        return 0;
    }

    // Read in two parts if wrapping
    size_t first_part = cb->capacity - cb->read_pos;
    if (first_part > to_read) first_part = to_read;

    memcpy(data, &cb->buffer[cb->read_pos * AUDIO_CHANNELS],
           first_part * sizeof(int16_t) * AUDIO_CHANNELS);

    size_t second_part = to_read - first_part;
    if (second_part > 0) {
        memcpy(&data[first_part * AUDIO_CHANNELS], cb->buffer,
               second_part * sizeof(int16_t) * AUDIO_CHANNELS);
    }

    cb->read_pos = (cb->read_pos + to_read) % cb->capacity;
    cb->available -= to_read;

    pthread_mutex_unlock(&cb->mutex);
    return to_read;
}

// ============ STREAMING DECODER INTERFACE ============

// Open decoder and read metadata (doesn't decode audio yet)
static int stream_decoder_open(StreamDecoder* sd, const char* filepath) {
    memset(sd, 0, sizeof(StreamDecoder));

    sd->format = Player_detectFormat(filepath);
    if (sd->format == AUDIO_FORMAT_UNKNOWN) {
        LOG_error("Stream: Unknown audio format: %s\n", filepath);
        return -1;
    }

    switch (sd->format) {
        case AUDIO_FORMAT_MP3: {
            drmp3* mp3 = malloc(sizeof(drmp3));
            if (!mp3 || !drmp3_init_file(mp3, filepath, NULL)) {
                free(mp3);
                LOG_error("Stream: Failed to open MP3: %s\n", filepath);
                return -1;
            }
            sd->decoder = mp3;
            sd->source_sample_rate = mp3->sampleRate;
            sd->source_channels = mp3->channels;
            sd->total_frames = drmp3_get_pcm_frame_count(mp3);
            break;
        }
        case AUDIO_FORMAT_WAV: {
            drwav* wav = malloc(sizeof(drwav));
            if (!wav || !drwav_init_file(wav, filepath, NULL)) {
                free(wav);
                LOG_error("Stream: Failed to open WAV: %s\n", filepath);
                return -1;
            }
            sd->decoder = wav;
            sd->source_sample_rate = wav->sampleRate;
            sd->source_channels = wav->channels;
            sd->total_frames = wav->totalPCMFrameCount;
            break;
        }
        case AUDIO_FORMAT_FLAC: {
            drflac* flac = drflac_open_file(filepath, NULL);
            if (!flac) {
                LOG_error("Stream: Failed to open FLAC: %s\n", filepath);
                return -1;
            }
            sd->decoder = flac;
            sd->source_sample_rate = flac->sampleRate;
            sd->source_channels = flac->channels;
            sd->total_frames = flac->totalPCMFrameCount;
            break;
        }
        case AUDIO_FORMAT_OGG: {
            int error;
            stb_vorbis* vorbis = stb_vorbis_open_filename(filepath, &error, NULL);
            if (!vorbis) {
                LOG_error("Stream: Failed to open OGG: %s (error %d)\n", filepath, error);
                return -1;
            }
            sd->decoder = vorbis;
            stb_vorbis_info info = stb_vorbis_get_info(vorbis);
            sd->source_sample_rate = info.sample_rate;
            sd->source_channels = info.channels;
            sd->total_frames = stb_vorbis_stream_length_in_samples(vorbis);
            break;
        }
        default:
            LOG_error("Stream: Unsupported format for streaming: %d\n", sd->format);
            return -1;
    }

    sd->current_frame = 0;
    return 0;
}

// Read chunk of audio from decoder (returns frames read, outputs stereo)
static size_t stream_decoder_read(StreamDecoder* sd, int16_t* buffer, size_t frames) {
    if (!sd->decoder) return 0;

    size_t frames_read = 0;

    switch (sd->format) {
        case AUDIO_FORMAT_MP3: {
            drmp3* mp3 = (drmp3*)sd->decoder;
            if (sd->source_channels == 1) {
                // Read mono, convert to stereo
                int16_t* mono = malloc(frames * sizeof(int16_t));
                if (mono) {
                    frames_read = drmp3_read_pcm_frames_s16(mp3, frames, mono);
                    for (size_t i = 0; i < frames_read; i++) {
                        buffer[i * 2] = mono[i];
                        buffer[i * 2 + 1] = mono[i];
                    }
                    free(mono);
                }
            } else {
                frames_read = drmp3_read_pcm_frames_s16(mp3, frames, buffer);
            }
            break;
        }
        case AUDIO_FORMAT_WAV: {
            drwav* wav = (drwav*)sd->decoder;
            if (sd->source_channels == 1) {
                int16_t* mono = malloc(frames * sizeof(int16_t));
                if (mono) {
                    frames_read = drwav_read_pcm_frames_s16(wav, frames, mono);
                    for (size_t i = 0; i < frames_read; i++) {
                        buffer[i * 2] = mono[i];
                        buffer[i * 2 + 1] = mono[i];
                    }
                    free(mono);
                }
            } else {
                frames_read = drwav_read_pcm_frames_s16(wav, frames, buffer);
            }
            break;
        }
        case AUDIO_FORMAT_FLAC: {
            drflac* flac = (drflac*)sd->decoder;
            if (sd->source_channels == 1) {
                int16_t* mono = malloc(frames * sizeof(int16_t));
                if (mono) {
                    frames_read = drflac_read_pcm_frames_s16(flac, frames, mono);
                    for (size_t i = 0; i < frames_read; i++) {
                        buffer[i * 2] = mono[i];
                        buffer[i * 2 + 1] = mono[i];
                    }
                    free(mono);
                }
            } else {
                frames_read = drflac_read_pcm_frames_s16(flac, frames, buffer);
            }
            break;
        }
        case AUDIO_FORMAT_OGG: {
            stb_vorbis* vorbis = (stb_vorbis*)sd->decoder;
            // stb_vorbis always outputs interleaved, can handle stereo conversion
            frames_read = stb_vorbis_get_samples_short_interleaved(
                vorbis, AUDIO_CHANNELS, buffer, frames * AUDIO_CHANNELS);
            break;
        }
        default:
            break;
    }

    sd->current_frame += frames_read;
    return frames_read;
}

// Seek to frame position
static int stream_decoder_seek(StreamDecoder* sd, int64_t frame) {
    if (!sd->decoder) return -1;

    if (frame < 0) frame = 0;
    if (frame > sd->total_frames) frame = sd->total_frames;

    bool success = false;
    switch (sd->format) {
        case AUDIO_FORMAT_MP3:
            success = drmp3_seek_to_pcm_frame((drmp3*)sd->decoder, frame);
            break;
        case AUDIO_FORMAT_WAV:
            success = drwav_seek_to_pcm_frame((drwav*)sd->decoder, frame);
            break;
        case AUDIO_FORMAT_FLAC:
            success = drflac_seek_to_pcm_frame((drflac*)sd->decoder, frame);
            break;
        case AUDIO_FORMAT_OGG:
            success = (stb_vorbis_seek((stb_vorbis*)sd->decoder, (unsigned int)frame) != 0);
            break;
        default:
            break;
    }

    if (success) {
        sd->current_frame = frame;
        return 0;
    }
    return -1;
}

// Close decoder
static void stream_decoder_close(StreamDecoder* sd) {
    if (!sd->decoder) return;

    switch (sd->format) {
        case AUDIO_FORMAT_MP3:
            drmp3_uninit((drmp3*)sd->decoder);
            free(sd->decoder);
            break;
        case AUDIO_FORMAT_WAV:
            drwav_uninit((drwav*)sd->decoder);
            free(sd->decoder);
            break;
        case AUDIO_FORMAT_FLAC:
            drflac_close((drflac*)sd->decoder);
            break;
        case AUDIO_FORMAT_OGG:
            stb_vorbis_close((stb_vorbis*)sd->decoder);
            break;
        default:
            break;
    }

    sd->decoder = NULL;
    sd->format = AUDIO_FORMAT_UNKNOWN;
}

// ============ STREAMING RESAMPLER ============

// Resample a chunk of audio (for streaming)
// Returns number of output frames
static size_t resample_chunk(int16_t* input, size_t input_frames,
                             int src_rate, int dst_rate,
                             int16_t* output, size_t max_output_frames,
                             SRC_STATE* src_state, bool is_last) {
    if (src_rate == dst_rate) {
        // No resampling needed, just copy
        size_t to_copy = (input_frames < max_output_frames) ? input_frames : max_output_frames;
        memcpy(output, input, to_copy * sizeof(int16_t) * AUDIO_CHANNELS);
        return to_copy;
    }

    double ratio = (double)dst_rate / (double)src_rate;

    // Convert input to float
    float* float_in = malloc(input_frames * AUDIO_CHANNELS * sizeof(float));
    float* float_out = malloc(max_output_frames * AUDIO_CHANNELS * sizeof(float));
    if (!float_in || !float_out) {
        free(float_in);
        free(float_out);
        return 0;
    }

    for (size_t i = 0; i < input_frames * AUDIO_CHANNELS; i++) {
        float_in[i] = input[i] / 32768.0f;
    }

    // Setup conversion
    SRC_DATA src_data;
    src_data.data_in = float_in;
    src_data.data_out = float_out;
    src_data.input_frames = input_frames;
    src_data.output_frames = max_output_frames;
    src_data.src_ratio = ratio;
    src_data.end_of_input = is_last ? 1 : 0;

    int error = src_process(src_state, &src_data);
    if (error) {
        LOG_error("Resample chunk failed: %s\n", src_strerror(error));
        free(float_in);
        free(float_out);
        return 0;
    }

    // Convert output back to int16
    size_t output_frames = src_data.output_frames_gen;
    for (size_t i = 0; i < output_frames * AUDIO_CHANNELS; i++) {
        float sample = float_out[i] * 32767.0f;
        if (sample > 32767.0f) sample = 32767.0f;
        if (sample < -32768.0f) sample = -32768.0f;
        output[i] = (int16_t)sample;
    }

    free(float_in);
    free(float_out);
    return output_frames;
}

// ============ STREAMING DECODE THREAD ============

static void* stream_thread_func(void* arg) {
    (void)arg;

    // Allocate decode buffer
    int16_t* decode_buffer = malloc(DECODE_CHUNK_FRAMES * sizeof(int16_t) * AUDIO_CHANNELS);
    // Resample output buffer (allow for 2x expansion)
    size_t resample_buffer_size = DECODE_CHUNK_FRAMES * 3;
    int16_t* resample_buffer = malloc(resample_buffer_size * sizeof(int16_t) * AUDIO_CHANNELS);

    if (!decode_buffer || !resample_buffer) {
        LOG_error("Stream thread: Failed to allocate buffers\n");
        free(decode_buffer);
        free(resample_buffer);
        return NULL;
    }

    while (player.stream_running) {
        // Check if seeking requested
        if (player.stream_seeking) {
            stream_decoder_seek(&player.stream_decoder, player.seek_target_frame);
            circular_buffer_clear(&player.stream_buffer);
            if (player.resampler) {
                src_reset((SRC_STATE*)player.resampler);
            }
            player.stream_seeking = false;
        }

        // Check if buffer needs more data (< 50% full)
        size_t available = circular_buffer_available(&player.stream_buffer);
        if (available < STREAM_BUFFER_FRAMES / 2) {
            // Decode a chunk
            size_t decoded = stream_decoder_read(&player.stream_decoder,
                                                  decode_buffer, DECODE_CHUNK_FRAMES);
            if (decoded > 0) {
                // Resample chunk to target rate if needed
                int src_rate = player.stream_decoder.source_sample_rate;
                int dst_rate = get_target_sample_rate();
                bool is_last = (player.stream_decoder.current_frame >= player.stream_decoder.total_frames);

                size_t output_frames;
                if (src_rate == dst_rate) {
                    // No resampling needed
                    output_frames = decoded;
                    circular_buffer_write(&player.stream_buffer, decode_buffer, output_frames);
                } else {
                    // Resample
                    output_frames = resample_chunk(decode_buffer, decoded,
                                                   src_rate, dst_rate,
                                                   resample_buffer, resample_buffer_size,
                                                   (SRC_STATE*)player.resampler, is_last);
                    circular_buffer_write(&player.stream_buffer, resample_buffer, output_frames);
                }
            }
        } else {
            // Buffer full enough, sleep briefly
            usleep(5000);  // 5ms
        }
    }

    free(decode_buffer);
    free(resample_buffer);
    return NULL;
}

// ============ END STREAMING PLAYBACK SYSTEM ============

// Audio callback - SDL pulls audio data from here
static void audio_callback(void* userdata, Uint8* stream, int len) {
    PlayerContext* ctx = (PlayerContext*)userdata;
    int samples_needed = len / (sizeof(int16_t) * AUDIO_CHANNELS);
    int16_t* out = (int16_t*)stream;

    // Check if radio is active - handle radio audio separately
    if (Radio_isActive()) {
        // Only get audio when radio is actually playing (not buffering)
        if (Radio_getState() == RADIO_STATE_PLAYING) {
            // Get audio from radio module
            int samples_got = Radio_getAudioSamples(out, samples_needed * AUDIO_CHANNELS);

            // If we got less than needed, fill rest with silence
            if (samples_got < samples_needed * AUDIO_CHANNELS) {
                memset(&out[samples_got], 0, (samples_needed * AUDIO_CHANNELS - samples_got) * sizeof(int16_t));
            }

            // Apply volume if needed
            if (ctx->volume < 0.99f || ctx->volume > 1.01f) {
                for (int i = 0; i < samples_needed * AUDIO_CHANNELS; i++) {
                    out[i] = (int16_t)(out[i] * ctx->volume);
                }
            }
        } else {
            // Still buffering - output silence
            memset(stream, 0, len);
        }

        return;
    }

    // Try to lock, if can't, output silence (non-blocking to prevent crackling)
    if (pthread_mutex_trylock(&ctx->mutex) != 0) {
        memset(stream, 0, len);
        return;
    }

    if (ctx->state != PLAYER_STATE_PLAYING) {
        memset(stream, 0, len);
        pthread_mutex_unlock(&ctx->mutex);
        return;
    }

    // ============ STREAMING MODE ============
    if (ctx->use_streaming) {
        // Read from circular buffer
        size_t samples_read = circular_buffer_read(&ctx->stream_buffer, out, samples_needed);

        // If not enough data, fill rest with silence
        if (samples_read < (size_t)samples_needed) {
            memset(&out[samples_read * AUDIO_CHANNELS], 0,
                   (samples_needed - samples_read) * sizeof(int16_t) * AUDIO_CHANNELS);
        }

        // Apply volume (only if not 1.0)
        if (ctx->volume < 0.99f || ctx->volume > 1.01f) {
            for (size_t i = 0; i < samples_read * AUDIO_CHANNELS; i++) {
                out[i] = (int16_t)(out[i] * ctx->volume);
            }
        }

        // Copy to visualization buffer (non-blocking)
        if (samples_read > 0 && pthread_mutex_trylock(&ctx->vis_mutex) == 0) {
            int vis_samples = samples_read * AUDIO_CHANNELS;
            if (vis_samples > 2048) vis_samples = 2048;
            memcpy(ctx->vis_buffer, out, vis_samples * sizeof(int16_t));
            ctx->vis_buffer_pos = vis_samples;
            pthread_mutex_unlock(&ctx->vis_mutex);
        }

        // Update position
        audio_position_samples += samples_read;
        ctx->position_ms = (audio_position_samples * 1000) / current_sample_rate;

        // Check if track ended
        if (ctx->stream_decoder.current_frame >= ctx->stream_decoder.total_frames &&
            circular_buffer_available(&ctx->stream_buffer) == 0) {
            if (ctx->repeat) {
                // Seek back to beginning
                ctx->seek_target_frame = 0;
                ctx->stream_seeking = true;
                audio_position_samples = 0;
                ctx->position_ms = 0;
            } else {
                ctx->state = PLAYER_STATE_STOPPED;
                audio_position_samples = 0;
                ctx->position_ms = 0;
            }
        }

        pthread_mutex_unlock(&ctx->mutex);
        return;
    }

    // No audio loaded - output silence
    memset(stream, 0, len);
    pthread_mutex_unlock(&ctx->mutex);
}

int Player_init(void) {
    memset(&player, 0, sizeof(PlayerContext));

    pthread_mutex_init(&player.mutex, NULL);
    pthread_mutex_init(&player.vis_mutex, NULL);

    player.volume = 1.0f;
    player.state = PLAYER_STATE_STOPPED;

    // Initialize SDL audio
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        LOG_error("Failed to init SDL audio: %s\n", SDL_GetError());
        return -1;
    }

    // Check current audio sink setting
    int audio_sink = GetAudioSink();

    // Also check if .asoundrc exists with bluealsa config (more reliable than msettings)
    const char* home = getenv("HOME");

    if (home) {
        char asoundrc_path[512];
        snprintf(asoundrc_path, sizeof(asoundrc_path), "%s/.asoundrc", home);
        FILE* f = fopen(asoundrc_path, "r");
        if (f) {
            char buf[256];
            while (fgets(buf, sizeof(buf), f)) {
                if (strstr(buf, "bluealsa")) {
                    audio_sink = AUDIO_SINK_BLUETOOTH;
                    bluetooth_audio_active = true;
                    break;
                }
            }
            fclose(f);
        }
    }

    // If Bluetooth audio is detected, set BlueALSA mixer to 100% for software volume control
    if (audio_sink == AUDIO_SINK_BLUETOOTH) {
        // Set all mixer controls that contain "A2DP" in their name to 100%
        // This handles devices like "Galaxy Buds Live (4B23 A2DP" etc.
        system("amixer scontrols 2>/dev/null | grep -i 'A2DP' | "
               "sed \"s/.*'\\([^']*\\)'.*/\\1/\" | "
               "while read ctrl; do amixer sset \"$ctrl\" 127 2>/dev/null; done");
    }

    // Determine target sample rate based on audio output
    int target_rate = get_target_sample_rate();

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = target_rate;
    want.format = AUDIO_S16SYS;
    want.channels = AUDIO_CHANNELS;
    want.samples = AUDIO_SAMPLES;
    want.callback = audio_callback;
    want.userdata = &player;

    // Open default audio device - respects .asoundrc for Bluetooth/USB DAC routing
    player.audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);

    if (player.audio_device == 0) {
        LOG_error("Failed to open audio device: %s\n", SDL_GetError());

        // If Bluetooth was detected but device isn't available, fall back to speaker
        if (bluetooth_audio_active) {
            bluetooth_audio_active = false;

            // Retry with speaker sample rate and explicit device name to bypass .asoundrc
            want.freq = SAMPLE_RATE_SPEAKER;

            // Try to open the first available audio device explicitly
            int num_devices = SDL_GetNumAudioDevices(0);
            for (int i = 0; i < num_devices; i++) {
                const char* device_name = SDL_GetAudioDeviceName(i, 0);
                player.audio_device = SDL_OpenAudioDevice(device_name, 0, &want, &have, 0);
                if (player.audio_device != 0) {
                    break;
                }
            }

            if (player.audio_device == 0) {
                LOG_error("All fallback audio devices failed\n");
                return -1;
            }
        } else {
            return -1;
        }
    }

    player.audio_initialized = true;

    // Register for audio device changes (Bluetooth, USB DAC, etc.)
    PLAT_audioDeviceWatchRegister(audio_device_change_callback);

    return 0;
}

// Reconfigure audio device with a new sample rate
static int reconfigure_audio_device(int new_sample_rate) {
    if (new_sample_rate == current_sample_rate && player.audio_device > 0) {
        return 0;  // No change needed
    }

    // Pause and close existing device
    if (player.audio_device > 0) {
        SDL_PauseAudioDevice(player.audio_device, 1);
        SDL_CloseAudioDevice(player.audio_device);
        player.audio_device = 0;
    }

    // Open with new sample rate
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = new_sample_rate;
    want.format = AUDIO_S16SYS;
    want.channels = AUDIO_CHANNELS;
    want.samples = AUDIO_SAMPLES;
    want.callback = audio_callback;
    want.userdata = &player;

    player.audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (player.audio_device == 0) {
        LOG_error("Failed to open audio device at %d Hz: %s\n", new_sample_rate, SDL_GetError());
        // Try to reopen at target rate for current audio sink
        int fallback_rate = get_target_sample_rate();
        want.freq = fallback_rate;
        player.audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (player.audio_device == 0) {
            return -1;
        }
    }

    current_sample_rate = have.freq;
    return 0;
}

// Reopen audio device (called when audio sink changes, e.g., Bluetooth connect/disconnect)
static void reopen_audio_device(void) {
    // Remember current playback state
    PlayerState prev_state = player.state;

    // Pause and close existing device
    if (player.audio_device > 0) {
        SDL_PauseAudioDevice(player.audio_device, 1);
        SDL_CloseAudioDevice(player.audio_device);
        player.audio_device = 0;
    }

    // Get target sample rate for the new audio sink
    int target_rate = get_target_sample_rate();

    // Reopen with target sample rate
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = target_rate;
    want.format = AUDIO_S16SYS;
    want.channels = AUDIO_CHANNELS;
    want.samples = AUDIO_SAMPLES;
    want.callback = audio_callback;
    want.userdata = &player;

    player.audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (player.audio_device == 0) {
        LOG_error("Failed to reopen audio device: %s\n", SDL_GetError());
        return;
    }

    current_sample_rate = have.freq;

    // Resume playback if it was playing
    if (prev_state == PLAYER_STATE_PLAYING) {
        SDL_PauseAudioDevice(player.audio_device, 0);
    }
}

// Callback for audio device changes (Bluetooth connect/disconnect, USB DAC, etc.)
static void audio_device_change_callback(int device_type, int event) {
    (void)device_type;
    (void)event;

    // Re-check if Bluetooth is now active/inactive
    bool was_bluetooth = bluetooth_audio_active;
    bluetooth_audio_active = false;

    const char* home = getenv("HOME");
    if (home) {
        char asoundrc_path[512];
        snprintf(asoundrc_path, sizeof(asoundrc_path), "%s/.asoundrc", home);
        FILE* f = fopen(asoundrc_path, "r");
        if (f) {
            char buf[256];
            while (fgets(buf, sizeof(buf), f)) {
                if (strstr(buf, "bluealsa")) {
                    bluetooth_audio_active = true;
                    break;
                }
            }
            fclose(f);
        }
    }

    if (was_bluetooth != bluetooth_audio_active) {
        // If Bluetooth just activated, set mixer to 100%
        if (bluetooth_audio_active) {
            system("amixer scontrols 2>/dev/null | grep -i 'A2DP' | "
                   "sed \"s/.*'\\([^']*\\)'.*/\\1/\" | "
                   "while read ctrl; do amixer sset \"$ctrl\" 127 2>/dev/null; done");
        }
    }

    reopen_audio_device();
}

void Player_quit(void) {
    // Unregister audio device watcher
    PLAT_audioDeviceWatchUnregister();

    Player_stop();

    if (player.audio_device > 0) {
        SDL_CloseAudioDevice(player.audio_device);
        player.audio_device = 0;
    }

    SDL_QuitSubSystem(SDL_INIT_AUDIO);

    pthread_mutex_destroy(&player.mutex);
    pthread_mutex_destroy(&player.vis_mutex);

    player.audio_initialized = false;
}

// ============ METADATA PARSING ============

// Helper: read syncsafe integer (ID3v2)
static uint32_t read_syncsafe_int(const uint8_t* data) {
    return ((uint32_t)(data[0] & 0x7F) << 21) |
           ((uint32_t)(data[1] & 0x7F) << 14) |
           ((uint32_t)(data[2] & 0x7F) << 7) |
           ((uint32_t)(data[3] & 0x7F));
}

// Helper: read big-endian 32-bit integer
static uint32_t read_be32(const uint8_t* data) {
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

// Helper: copy string, trimming trailing spaces
static void copy_metadata_string(char* dest, const char* src, size_t max_len) {
    if (!src || !dest || max_len == 0) return;

    size_t len = strlen(src);
    if (len >= max_len) len = max_len - 1;

    memcpy(dest, src, len);
    dest[len] = '\0';

    // Trim trailing spaces and nulls
    while (len > 0 && (dest[len-1] == ' ' || dest[len-1] == '\0')) {
        dest[--len] = '\0';
    }
}

// Helper: convert UTF-16LE to ASCII (for ID3v2 text frames)
static void utf16le_to_ascii(char* dest, const uint8_t* src, size_t src_len, size_t max_len) {
    if (!dest || !src || max_len == 0) return;

    size_t j = 0;
    for (size_t i = 0; i + 1 < src_len && j < max_len - 1; i += 2) {
        // Get UTF-16LE character (low byte first)
        uint16_t ch = src[i] | (src[i + 1] << 8);

        // Only copy ASCII range characters
        if (ch > 0 && ch < 128) {
            dest[j++] = (char)ch;
        } else if (ch >= 128 && ch < 256) {
            // Latin-1 supplement - just use low byte
            dest[j++] = (char)ch;
        }
        // Skip non-ASCII/non-Latin1 characters
    }
    dest[j] = '\0';
}

// Helper: convert UTF-16BE to ASCII (for ID3v2 text frames)
static void utf16be_to_ascii(char* dest, const uint8_t* src, size_t src_len, size_t max_len) {
    if (!dest || !src || max_len == 0) return;

    size_t j = 0;
    for (size_t i = 0; i + 1 < src_len && j < max_len - 1; i += 2) {
        // Get UTF-16BE character (high byte first)
        uint16_t ch = (src[i] << 8) | src[i + 1];

        // Only copy ASCII range characters
        if (ch > 0 && ch < 128) {
            dest[j++] = (char)ch;
        } else if (ch >= 128 && ch < 256) {
            // Latin-1 supplement
            dest[j++] = (char)ch;
        }
    }
    dest[j] = '\0';
}

// Parse ID3v1 tag (at end of file, 128 bytes)
static void parse_id3v1(const char* filepath) {
    FILE* f = fopen(filepath, "rb");
    if (!f) return;

    // Seek to last 128 bytes
    if (fseek(f, -128, SEEK_END) != 0) {
        fclose(f);
        return;
    }

    uint8_t tag[128];
    if (fread(tag, 1, 128, f) != 128) {
        fclose(f);
        return;
    }
    fclose(f);

    // Check for "TAG" header
    if (tag[0] != 'T' || tag[1] != 'A' || tag[2] != 'G') {
        return;
    }

    // ID3v1 layout: TAG(3) + Title(30) + Artist(30) + Album(30) + Year(4) + Comment(30) + Genre(1)
    char buf[31];

    // Title (bytes 3-32)
    if (player.track_info.title[0] == '\0' || strstr(player.track_info.title, ".") != NULL) {
        memcpy(buf, &tag[3], 30);
        buf[30] = '\0';
        copy_metadata_string(player.track_info.title, buf, sizeof(player.track_info.title));
    }

    // Artist (bytes 33-62)
    if (player.track_info.artist[0] == '\0') {
        memcpy(buf, &tag[33], 30);
        buf[30] = '\0';
        copy_metadata_string(player.track_info.artist, buf, sizeof(player.track_info.artist));
    }

    // Album (bytes 63-92)
    if (player.track_info.album[0] == '\0') {
        memcpy(buf, &tag[63], 30);
        buf[30] = '\0';
        copy_metadata_string(player.track_info.album, buf, sizeof(player.track_info.album));
    }

}

// Parse ID3v2 tag (at beginning of file)
static void parse_id3v2(const char* filepath) {
    FILE* f = fopen(filepath, "rb");
    if (!f) return;

    uint8_t header[10];
    if (fread(header, 1, 10, f) != 10) {
        fclose(f);
        return;
    }

    // Check for "ID3" header
    if (header[0] != 'I' || header[1] != 'D' || header[2] != '3') {
        fclose(f);
        return;
    }

    uint8_t version_major = header[3];  // 3 = ID3v2.3, 4 = ID3v2.4
    // uint8_t version_minor = header[4];
    // uint8_t flags = header[5];
    uint32_t tag_size = read_syncsafe_int(&header[6]);


    // Read entire tag
    uint8_t* tag_data = malloc(tag_size);
    if (!tag_data) {
        fclose(f);
        return;
    }

    if (fread(tag_data, 1, tag_size, f) != tag_size) {
        free(tag_data);
        fclose(f);
        return;
    }
    fclose(f);

    // Parse frames
    uint32_t pos = 0;
    while (pos + 10 < tag_size) {
        // Frame header: ID(4) + Size(4) + Flags(2)
        char frame_id[5];
        memcpy(frame_id, &tag_data[pos], 4);
        frame_id[4] = '\0';

        // Check for padding (all zeros)
        if (frame_id[0] == '\0') break;

        uint32_t frame_size;
        if (version_major == 4) {
            frame_size = read_syncsafe_int(&tag_data[pos + 4]);
        } else {
            frame_size = read_be32(&tag_data[pos + 4]);
        }

        // Skip flags
        pos += 10;

        if (frame_size == 0 || pos + frame_size > tag_size) break;

        // Process text frames (TIT2, TPE1, TALB, etc.)
        if (frame_id[0] == 'T' && frame_size > 1) {
            const uint8_t* frame_data = &tag_data[pos];
            uint8_t encoding = frame_data[0];
            const uint8_t* text_data = &frame_data[1];
            size_t text_len = frame_size - 1;

            char temp[256];
            temp[0] = '\0';

            // Handle different encodings
            // 0 = ISO-8859-1, 1 = UTF-16 with BOM, 2 = UTF-16BE, 3 = UTF-8
            if (encoding == 0 || encoding == 3) {
                // ISO-8859-1 or UTF-8: copy directly
                size_t copy_len = text_len < 255 ? text_len : 255;
                memcpy(temp, text_data, copy_len);
                temp[copy_len] = '\0';
            } else if (encoding == 1) {
                // UTF-16 with BOM
                if (text_len >= 2) {
                    bool is_le = (text_data[0] == 0xFF && text_data[1] == 0xFE);
                    bool is_be = (text_data[0] == 0xFE && text_data[1] == 0xFF);
                    if (is_le || is_be) {
                        text_data += 2;
                        text_len -= 2;
                    }
                    if (is_be) {
                        utf16be_to_ascii(temp, text_data, text_len, sizeof(temp));
                    } else {
                        // Default to LE
                        utf16le_to_ascii(temp, text_data, text_len, sizeof(temp));
                    }
                }
            } else if (encoding == 2) {
                // UTF-16BE without BOM
                utf16be_to_ascii(temp, text_data, text_len, sizeof(temp));
            }

            // Assign to appropriate field
            if (strcmp(frame_id, "TIT2") == 0 && temp[0]) {  // Title
                copy_metadata_string(player.track_info.title, temp, sizeof(player.track_info.title));
            } else if (strcmp(frame_id, "TPE1") == 0 && temp[0]) {  // Artist
                copy_metadata_string(player.track_info.artist, temp, sizeof(player.track_info.artist));
            } else if (strcmp(frame_id, "TALB") == 0 && temp[0]) {  // Album
                copy_metadata_string(player.track_info.album, temp, sizeof(player.track_info.album));
            }
        }
        // Process APIC frame (album art) - only if we don't already have art
        else if (strcmp(frame_id, "APIC") == 0 && frame_size > 10 && player.album_art == NULL) {
            const uint8_t* frame_data = &tag_data[pos];
            uint8_t encoding = frame_data[0];
            size_t offset = 1;

            // Skip MIME type (null-terminated string)
            while (offset < frame_size && frame_data[offset] != '\0') offset++;
            offset++;  // Skip null terminator

            if (offset < frame_size) {
                uint8_t pic_type = frame_data[offset];
                offset++;

                // Skip description (null-terminated, encoding-dependent)
                if (encoding == 1 || encoding == 2) {
                    // UTF-16: look for double null
                    while (offset + 1 < frame_size) {
                        if (frame_data[offset] == 0 && frame_data[offset + 1] == 0) {
                            offset += 2;
                            break;
                        }
                        offset++;
                    }
                } else {
                    // ISO-8859-1 or UTF-8: single null
                    while (offset < frame_size && frame_data[offset] != '\0') offset++;
                    offset++;
                }

                // Now frame_data + offset points to the image data
                if (offset < frame_size) {
                    size_t image_size = frame_size - offset;
                    const uint8_t* image_data = &frame_data[offset];

                    // Prefer front cover (type 3), but accept any if we have none
                    if (pic_type == 3 || player.album_art == NULL) {
                        SDL_RWops* rw = SDL_RWFromConstMem(image_data, image_size);
                        if (rw) {
                            SDL_Surface* art = IMG_Load_RW(rw, 1);  // 1 = auto-close RWops
                            if (art) {
                                // Free previous art if we're replacing with front cover
                                if (player.album_art) {
                                    SDL_FreeSurface(player.album_art);
                                }
                                player.album_art = art;
                            }
                        }
                    }
                }
            }
        }

        pos += frame_size;
    }

    free(tag_data);

}

// Parse MP3 metadata (ID3v2 first, then ID3v1 as fallback)
static void parse_mp3_metadata(const char* filepath) {
    // Try ID3v2 first (more modern, more info)
    parse_id3v2(filepath);

    // Fall back to ID3v1 for any missing fields
    if (player.track_info.artist[0] == '\0' || player.track_info.album[0] == '\0') {
        parse_id3v1(filepath);
    }
}

// Parse Vorbis comments (for OGG and FLAC)
static void parse_vorbis_comment(const char* comment) {
    if (!comment) return;

    // Vorbis comments are in format "KEY=VALUE"
    const char* eq = strchr(comment, '=');
    if (!eq) return;

    size_t key_len = eq - comment;
    const char* value = eq + 1;

    if (strncasecmp(comment, "TITLE", key_len) == 0 && key_len == 5) {
        copy_metadata_string(player.track_info.title, value, sizeof(player.track_info.title));
    } else if (strncasecmp(comment, "ARTIST", key_len) == 0 && key_len == 6) {
        copy_metadata_string(player.track_info.artist, value, sizeof(player.track_info.artist));
    } else if (strncasecmp(comment, "ALBUM", key_len) == 0 && key_len == 5) {
        copy_metadata_string(player.track_info.album, value, sizeof(player.track_info.album));
    }
}

// FLAC metadata callback
static void flac_metadata_callback(void* pUserData, drflac_metadata* pMetadata) {
    (void)pUserData;

    if (pMetadata->type == DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) {
        // Parse Vorbis comments
        const drflac_vorbis_comment_iterator* comments = NULL;
        uint32_t commentCount = pMetadata->data.vorbis_comment.commentCount;
        const char* pComments = pMetadata->data.vorbis_comment.pComments;

        // Iterate through comments
        for (uint32_t i = 0; i < commentCount; i++) {
            uint32_t commentLength;
            if (pComments) {
                // Read comment length (little-endian 32-bit)
                commentLength = *(const uint32_t*)pComments;
                pComments += 4;

                // Create null-terminated copy
                char* comment = malloc(commentLength + 1);
                if (comment) {
                    memcpy(comment, pComments, commentLength);
                    comment[commentLength] = '\0';
                    parse_vorbis_comment(comment);
                    free(comment);
                }

                pComments += commentLength;
            }
        }
    }
}

AudioFormat Player_detectFormat(const char* filepath) {
    if (!filepath) return AUDIO_FORMAT_UNKNOWN;

    const char* ext = strrchr(filepath, '.');
    if (!ext) return AUDIO_FORMAT_UNKNOWN;
    ext++; // Skip the dot

    if (strcasecmp(ext, "mp3") == 0) return AUDIO_FORMAT_MP3;
    if (strcasecmp(ext, "wav") == 0) return AUDIO_FORMAT_WAV;
    if (strcasecmp(ext, "ogg") == 0) return AUDIO_FORMAT_OGG;
    if (strcasecmp(ext, "flac") == 0) return AUDIO_FORMAT_FLAC;
    if (strcasecmp(ext, "mod") == 0 || strcasecmp(ext, "xm") == 0 ||
        strcasecmp(ext, "s3m") == 0 || strcasecmp(ext, "it") == 0) {
        return AUDIO_FORMAT_MOD;
    }

    return AUDIO_FORMAT_UNKNOWN;
}

// Reset audio device to default sample rate (for radio use)
void Player_resetSampleRate(void) {
    reconfigure_audio_device(get_target_sample_rate());
}

// Set audio device to specific sample rate
void Player_setSampleRate(int sample_rate) {
    if (sample_rate > 0) {
        reconfigure_audio_device(sample_rate);
    }
}

// Load file using streaming playback (decode on-the-fly)
static int load_streaming(const char* filepath) {
    // Open decoder
    if (stream_decoder_open(&player.stream_decoder, filepath) != 0) {
        return -1;
    }

    // Initialize circular buffer
    if (circular_buffer_init(&player.stream_buffer, STREAM_BUFFER_FRAMES) != 0) {
        stream_decoder_close(&player.stream_decoder);
        return -1;
    }

    // Initialize resampler for streaming
    int src_rate = player.stream_decoder.source_sample_rate;
    int dst_rate = get_target_sample_rate();

    if (src_rate != dst_rate) {
        int error;
        player.resampler = src_new(SRC_SINC_FASTEST, AUDIO_CHANNELS, &error);
        if (!player.resampler) {
            LOG_error("Stream: Failed to create resampler: %s\n", src_strerror(error));
            circular_buffer_free(&player.stream_buffer);
            stream_decoder_close(&player.stream_decoder);
            return -1;
        }
    }

    // Set track info
    player.track_info.sample_rate = dst_rate;  // Output rate
    player.track_info.channels = AUDIO_CHANNELS;
    player.track_info.duration_ms = (int)((player.stream_decoder.total_frames * 1000) /
                                          player.stream_decoder.source_sample_rate);

    // Configure audio device at target rate (no reconfiguration needed later!)
    reconfigure_audio_device(dst_rate);

    // Start decode thread
    player.stream_running = true;
    player.stream_seeking = false;
    pthread_create(&player.stream_thread, NULL, stream_thread_func, NULL);

    // Pre-buffer some audio before returning (~0.5 seconds)
    int prebuffer_timeout = 100;  // 100 * 10ms = 1 second max
    while (circular_buffer_available(&player.stream_buffer) < STREAM_BUFFER_FRAMES / 6 &&
           prebuffer_timeout > 0) {
        usleep(10000);  // 10ms
        prebuffer_timeout--;
    }

    player.use_streaming = true;
    player.format = player.stream_decoder.format;

    return 0;
}

int Player_load(const char* filepath) {
    if (!filepath || !player.audio_initialized) return -1;

    // Stop any current playback
    Player_stop();

    int result = -1;

    pthread_mutex_lock(&player.mutex);

    // Store filename
    strncpy(player.current_file, filepath, sizeof(player.current_file) - 1);

    // Extract title from filename
    const char* filename = strrchr(filepath, '/');
    if (filename) filename++; else filename = filepath;
    strncpy(player.track_info.title, filename, sizeof(player.track_info.title) - 1);

    // Remove extension from title
    char* ext = strrchr(player.track_info.title, '.');
    if (ext) *ext = '\0';

    // Clear artist/album
    player.track_info.artist[0] = '\0';
    player.track_info.album[0] = '\0';

    pthread_mutex_unlock(&player.mutex);

    // Use streaming playback for supported formats
    AudioFormat format = Player_detectFormat(filepath);
    if (format == AUDIO_FORMAT_MP3 || format == AUDIO_FORMAT_WAV ||
        format == AUDIO_FORMAT_FLAC || format == AUDIO_FORMAT_OGG) {
        result = load_streaming(filepath);

        // Parse metadata for MP3
        if (result == 0 && format == AUDIO_FORMAT_MP3) {
            parse_mp3_metadata(filepath);
        }
    } else {
        LOG_error("Unsupported format for streaming: %s\n", filepath);
        return -1;
    }

    if (result == 0) {
        pthread_mutex_lock(&player.mutex);
        player.position_ms = 0;
        audio_position_samples = 0;
        player.state = PLAYER_STATE_STOPPED;
        pthread_mutex_unlock(&player.mutex);

        // Note: Waveform generation disabled for streaming mode
        // (would require reading entire file which defeats the purpose)
    }

    return result;
}

int Player_play(void) {
    // Check if we have audio loaded
    if (!player.use_streaming || !player.stream_decoder.decoder) return -1;

    pthread_mutex_lock(&player.mutex);
    player.state = PLAYER_STATE_PLAYING;
    pthread_mutex_unlock(&player.mutex);

    SDL_PauseAudioDevice(player.audio_device, 0);
    return 0;
}

void Player_pause(void) {
    pthread_mutex_lock(&player.mutex);
    if (player.state == PLAYER_STATE_PLAYING) {
        player.state = PLAYER_STATE_PAUSED;
        SDL_PauseAudioDevice(player.audio_device, 1);
    }
    pthread_mutex_unlock(&player.mutex);
}

void Player_stop(void) {
    // Stop streaming thread first (before locking mutex to avoid deadlock)
    if (player.use_streaming && player.stream_running) {
        player.stream_running = false;
        pthread_join(player.stream_thread, NULL);
    }

    pthread_mutex_lock(&player.mutex);

    SDL_PauseAudioDevice(player.audio_device, 1);

    player.state = PLAYER_STATE_STOPPED;
    player.position_ms = 0;
    audio_position_samples = 0;

    // Clean up streaming resources
    if (player.use_streaming) {
        stream_decoder_close(&player.stream_decoder);
        circular_buffer_free(&player.stream_buffer);
        if (player.resampler) {
            src_delete((SRC_STATE*)player.resampler);
            player.resampler = NULL;
        }
        player.use_streaming = false;
    }

    memset(&player.track_info, 0, sizeof(TrackInfo));
    player.current_file[0] = '\0';

    // Clear waveform
    memset(&waveform, 0, sizeof(waveform));

    // Free album art
    if (player.album_art) {
        SDL_FreeSurface(player.album_art);
        player.album_art = NULL;
    }

    pthread_mutex_unlock(&player.mutex);
}

void Player_togglePause(void) {
    pthread_mutex_lock(&player.mutex);
    if (player.state == PLAYER_STATE_PLAYING) {
        player.state = PLAYER_STATE_PAUSED;
        SDL_PauseAudioDevice(player.audio_device, 1);
    } else if (player.state == PLAYER_STATE_PAUSED) {
        player.state = PLAYER_STATE_PLAYING;
        SDL_PauseAudioDevice(player.audio_device, 0);
    }
    pthread_mutex_unlock(&player.mutex);
}

void Player_seek(int position_ms) {
    pthread_mutex_lock(&player.mutex);
    if (position_ms < 0) position_ms = 0;
    if (position_ms > player.track_info.duration_ms) {
        position_ms = player.track_info.duration_ms;
    }

    if (player.use_streaming) {
        // Streaming mode: signal decode thread to seek
        // Calculate target frame in source sample rate
        int64_t target_frame = (int64_t)position_ms * player.stream_decoder.source_sample_rate / 1000;
        player.seek_target_frame = target_frame;
        player.stream_seeking = true;
    }

    player.position_ms = position_ms;
    audio_position_samples = (int64_t)position_ms * current_sample_rate / 1000;
    pthread_mutex_unlock(&player.mutex);
}

void Player_setVolume(float volume) {
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    pthread_mutex_lock(&player.mutex);
    player.volume = volume;
    pthread_mutex_unlock(&player.mutex);
}

float Player_getVolume(void) {
    return player.volume;
}

PlayerState Player_getState(void) {
    return player.state;
}

int Player_getPosition(void) {
    return player.position_ms;
}

int Player_getDuration(void) {
    return player.track_info.duration_ms;
}

const TrackInfo* Player_getTrackInfo(void) {
    return &player.track_info;
}

const char* Player_getCurrentFile(void) {
    return player.current_file;
}

int Player_getVisBuffer(int16_t* buffer, int max_samples) {
    if (!buffer || max_samples <= 0) return 0;

    pthread_mutex_lock(&player.vis_mutex);
    int samples_to_copy = player.vis_buffer_pos;
    if (samples_to_copy > max_samples) samples_to_copy = max_samples;
    if (samples_to_copy > 0) {
        memcpy(buffer, player.vis_buffer, samples_to_copy * sizeof(int16_t));
    }
    pthread_mutex_unlock(&player.vis_mutex);

    return samples_to_copy;
}

const WaveformData* Player_getWaveform(void) {
    return &waveform;
}

SDL_Surface* Player_getAlbumArt(void) {
    return player.album_art;
}

void Player_update(void) {
    // End-of-track detection is handled in the audio callback for streaming mode
    // This function is kept for any future polling needs
}

void Player_resumeAudio(void) {
    if (player.audio_device > 0) {
        SDL_PauseAudioDevice(player.audio_device, 0);
    }
}

void Player_pauseAudio(void) {
    if (player.audio_device > 0) {
        SDL_PauseAudioDevice(player.audio_device, 1);
    }
}

bool Player_isBluetoothActive(void) {
    return bluetooth_audio_active;
}
