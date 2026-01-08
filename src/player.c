#include "player.h"
#include "radio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <samplerate.h>

#include "defines.h"
#include "api.h"

// Include dr_libs for audio decoding (header-only libraries)
#define DR_MP3_IMPLEMENTATION
#include "audio/dr_mp3.h"

#define DR_FLAC_IMPLEMENTATION
#include "audio/dr_flac.h"

#define DR_WAV_IMPLEMENTATION
#include "audio/dr_wav.h"

// For OGG we use stb_vorbis (implementation is in the .c file renamed to .h)
#include "audio/stb_vorbis.h"

#define SAMPLE_RATE 48000
#define AUDIO_CHANNELS 2
#define AUDIO_SAMPLES 4096  // Larger buffer to prevent crackling

// Global player context
static PlayerContext player = {0};
static int64_t audio_position_samples = 0;  // Track position in samples for precision
static WaveformData waveform = {0};  // Waveform overview for progress display
static int current_sample_rate = SAMPLE_RATE;  // Track current SDL audio device rate

// Preload system for background loading of next track
typedef struct {
    void* audio_data;
    size_t audio_size;
    TrackInfo track_info;
    char filepath[1024];
    AudioFormat format;
    volatile int ready;      // 0 = not ready, 1 = ready, -1 = failed
    volatile int loading;    // 1 = currently loading
    pthread_t thread;
    pthread_mutex_t mutex;
} PreloadBuffer;

static PreloadBuffer preload = {0};

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

    if (ctx->state != PLAYER_STATE_PLAYING || !ctx->audio_data) {
        memset(stream, 0, len);
        pthread_mutex_unlock(&ctx->mutex);
        return;
    }

    int16_t* src = (int16_t*)ctx->audio_data;
    int total_samples = ctx->audio_size / (sizeof(int16_t) * AUDIO_CHANNELS);

    int samples_to_copy = samples_needed;
    if (audio_position_samples + samples_to_copy > total_samples) {
        samples_to_copy = total_samples - audio_position_samples;
    }

    if (samples_to_copy > 0) {
        // Copy audio data
        memcpy(out, &src[audio_position_samples * AUDIO_CHANNELS],
               samples_to_copy * sizeof(int16_t) * AUDIO_CHANNELS);

        // Apply volume (only if not 1.0)
        if (ctx->volume < 0.99f || ctx->volume > 1.01f) {
            for (int i = 0; i < samples_to_copy * AUDIO_CHANNELS; i++) {
                out[i] = (int16_t)(out[i] * ctx->volume);
            }
        }

        // Copy to visualization buffer (non-blocking)
        if (pthread_mutex_trylock(&ctx->vis_mutex) == 0) {
            int vis_samples = samples_to_copy * AUDIO_CHANNELS;
            if (vis_samples > 2048) vis_samples = 2048;
            memcpy(ctx->vis_buffer, out, vis_samples * sizeof(int16_t));
            ctx->vis_buffer_pos = vis_samples;
            pthread_mutex_unlock(&ctx->vis_mutex);
        }

        // Update position
        audio_position_samples += samples_to_copy;
        ctx->position_ms = (audio_position_samples * 1000) / current_sample_rate;

        // Fill remaining with silence
        if (samples_to_copy < samples_needed) {
            memset(&out[samples_to_copy * AUDIO_CHANNELS], 0,
                   (samples_needed - samples_to_copy) * sizeof(int16_t) * AUDIO_CHANNELS);
        }
    } else {
        memset(stream, 0, len);
    }

    // Check if track ended
    if (audio_position_samples >= total_samples) {
        if (ctx->repeat) {
            audio_position_samples = 0;
            ctx->position_ms = 0;
        } else {
            ctx->state = PLAYER_STATE_STOPPED;
            audio_position_samples = 0;
            ctx->position_ms = 0;
        }
    }

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

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = AUDIO_CHANNELS;
    want.samples = AUDIO_SAMPLES;
    want.callback = audio_callback;
    want.userdata = &player;

    player.audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (player.audio_device == 0) {
        LOG_error("Failed to open audio device: %s\n", SDL_GetError());
        return -1;
    }

    player.audio_initialized = true;

    // Initialize preload mutex
    pthread_mutex_init(&preload.mutex, NULL);

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
        // Try to reopen at default rate
        want.freq = SAMPLE_RATE;
        player.audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (player.audio_device == 0) {
            return -1;
        }
    }

    current_sample_rate = have.freq;
    return 0;
}

void Player_quit(void) {
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

// Generate waveform overview from decoded audio
static void generate_waveform(void) {
    memset(&waveform, 0, sizeof(waveform));

    if (!player.audio_data || player.audio_size == 0) {
        return;
    }

    int16_t* samples = (int16_t*)player.audio_data;
    int total_samples = player.audio_size / sizeof(int16_t);  // Total int16 samples (L+R)
    int total_frames = total_samples / AUDIO_CHANNELS;  // Stereo frames

    // Calculate samples per bar
    int frames_per_bar = total_frames / WAVEFORM_BARS;
    if (frames_per_bar < 1) frames_per_bar = 1;

    waveform.bar_count = WAVEFORM_BARS;
    if (total_frames < WAVEFORM_BARS) {
        waveform.bar_count = total_frames;
    }

    // Calculate peak amplitude for each bar
    for (int bar = 0; bar < waveform.bar_count; bar++) {
        int start_frame = bar * frames_per_bar;
        int end_frame = start_frame + frames_per_bar;
        if (end_frame > total_frames) end_frame = total_frames;

        int32_t peak = 0;
        for (int frame = start_frame; frame < end_frame; frame++) {
            // Average of left and right channel
            int16_t left = samples[frame * 2];
            int16_t right = samples[frame * 2 + 1];
            int32_t avg = (abs(left) + abs(right)) / 2;
            if (avg > peak) peak = avg;
        }

        // Normalize to 0.0-1.0
        waveform.bars[bar] = (float)peak / 32768.0f;
    }

    waveform.valid = true;
}

// ============ RESAMPLING ============

// High-quality resampler using libsamplerate
// Returns newly allocated buffer on success, NULL on failure
// Caller must free the returned buffer (different from input)
// Input must be stereo interleaved 16-bit samples
static int16_t* resample_linear(int16_t* input, size_t input_frames,
                                 int src_rate, int dst_rate, size_t* output_frames) {
    if (src_rate == dst_rate) {
        *output_frames = input_frames;
        return NULL;  // No resampling needed, caller should use original
    }

    double ratio = (double)dst_rate / (double)src_rate;
    *output_frames = (size_t)(input_frames * ratio) + 1;  // +1 for safety margin

    if (*output_frames == 0) {
        LOG_error("Resample: output_frames is 0, input_frames=%zu, ratio=%.4f\n",
                  input_frames, ratio);
        return NULL;
    }

    // libsamplerate requires float data
    size_t input_samples = input_frames * AUDIO_CHANNELS;
    size_t output_samples = *output_frames * AUDIO_CHANNELS;

    float* float_input = malloc(input_samples * sizeof(float));
    float* float_output = malloc(output_samples * sizeof(float));
    if (!float_input || !float_output) {
        free(float_input);
        free(float_output);
        LOG_error("Resample: malloc failed for float buffers\n");
        return NULL;
    }

    // Convert int16 to float (normalize to -1.0 to 1.0)
    for (size_t i = 0; i < input_samples; i++) {
        float_input[i] = input[i] / 32768.0f;
    }

    // Setup libsamplerate conversion
    SRC_DATA src_data;
    src_data.data_in = float_input;
    src_data.data_out = float_output;
    src_data.input_frames = (long)input_frames;
    src_data.output_frames = (long)*output_frames;
    src_data.src_ratio = ratio;
    src_data.end_of_input = 1;

    // Use SRC_SINC_MEDIUM_QUALITY for good balance of quality and speed
    // SRC_SINC_BEST_QUALITY is higher quality but slower
    // SRC_SINC_FASTEST is fastest but lower quality
    int error = src_simple(&src_data, SRC_SINC_MEDIUM_QUALITY, AUDIO_CHANNELS);
    free(float_input);

    if (error) {
        LOG_error("Resample: libsamplerate error: %s\n", src_strerror(error));
        free(float_output);
        return NULL;
    }

    // Update actual output frames
    *output_frames = src_data.output_frames_gen;

    // Allocate final int16 output buffer
    int16_t* output = malloc(*output_frames * sizeof(int16_t) * AUDIO_CHANNELS);
    if (!output) {
        free(float_output);
        LOG_error("Resample: malloc failed for output buffer\n");
        return NULL;
    }

    // Convert float back to int16 with clipping
    size_t final_samples = *output_frames * AUDIO_CHANNELS;
    for (size_t i = 0; i < final_samples; i++) {
        float sample = float_output[i] * 32768.0f;
        if (sample > 32767.0f) sample = 32767.0f;
        if (sample < -32768.0f) sample = -32768.0f;
        output[i] = (int16_t)sample;
    }

    free(float_output);
    return output;
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

        // Only process text frames (TIT2, TPE1, TALB, etc.)
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

static int load_mp3(const char* filepath) {
    drmp3 mp3;
    if (!drmp3_init_file(&mp3, filepath, NULL)) {
        LOG_error("Failed to load MP3: %s\n", filepath);
        return -1;
    }

    // Get info
    player.track_info.sample_rate = mp3.sampleRate;
    player.track_info.channels = mp3.channels;

    // Read all frames
    drmp3_uint64 total_frames = drmp3_get_pcm_frame_count(&mp3);
    player.track_info.duration_ms = (int)((total_frames * 1000) / mp3.sampleRate);

    // Allocate buffer for decoded PCM (convert to stereo if mono)
    size_t buffer_size = total_frames * sizeof(int16_t) * AUDIO_CHANNELS;
    int16_t* buffer = malloc(buffer_size);
    if (!buffer) {
        drmp3_uninit(&mp3);
        return -1;
    }

    // Save format info before uninit (values may be invalid after uninit)
    int source_channels = mp3.channels;
    int source_sample_rate = mp3.sampleRate;

    // Decode to 16-bit PCM
    drmp3_uint64 frames_read = drmp3_read_pcm_frames_s16(&mp3, total_frames, buffer);
    drmp3_uninit(&mp3);

    // Handle incomplete frame reads (truncated/corrupt MP3)
    if (frames_read < total_frames) {
        LOG_error("MP3: Only read %llu of %llu frames (file may be truncated)\n",
                  (unsigned long long)frames_read, (unsigned long long)total_frames);
        // Resize buffer to actual frames read to avoid playing garbage data
        // Use source_channels since mono-to-stereo conversion happens later
        buffer_size = frames_read * sizeof(int16_t) * source_channels;
        // Update duration based on actual frames
        player.track_info.duration_ms = (int)((frames_read * 1000) / source_sample_rate);
    }

    // If mono, convert to stereo
    if (source_channels == 1) {
        int16_t* stereo_buffer = malloc(frames_read * sizeof(int16_t) * 2);
        for (drmp3_uint64 i = 0; i < frames_read; i++) {
            stereo_buffer[i * 2] = buffer[i];
            stereo_buffer[i * 2 + 1] = buffer[i];
        }
        free(buffer);
        buffer = stereo_buffer;
        buffer_size = frames_read * sizeof(int16_t) * 2;
    }

    // Resample to target rate if needed
    int effective_sample_rate = source_sample_rate;
    if (source_sample_rate != SAMPLE_RATE) {
        size_t resampled_frames;
        int16_t* resampled = resample_linear(buffer, frames_read, source_sample_rate, SAMPLE_RATE, &resampled_frames);
        if (resampled) {
            free(buffer);
            buffer = resampled;
            buffer_size = resampled_frames * sizeof(int16_t) * AUDIO_CHANNELS;
            effective_sample_rate = SAMPLE_RATE;
        } else {
            // Resampling failed - will need to use native sample rate
            LOG_error("MP3: Resampling failed, using native rate %d Hz\n", source_sample_rate);
        }
    }

    player.audio_data = buffer;
    player.audio_size = buffer_size;
    player.format = AUDIO_FORMAT_MP3;
    // Update sample_rate to reflect the actual rate of the buffer (for device config)
    player.track_info.sample_rate = effective_sample_rate;

    // Parse MP3 metadata (ID3 tags)
    parse_mp3_metadata(filepath);

    return 0;
}

static int load_wav(const char* filepath) {
    drwav wav;
    if (!drwav_init_file(&wav, filepath, NULL)) {
        LOG_error("Failed to load WAV: %s\n", filepath);
        return -1;
    }

    player.track_info.sample_rate = wav.sampleRate;
    player.track_info.channels = wav.channels;
    player.track_info.duration_ms = (int)((wav.totalPCMFrameCount * 1000) / wav.sampleRate);

    // Allocate buffer
    size_t buffer_size = wav.totalPCMFrameCount * sizeof(int16_t) * AUDIO_CHANNELS;
    int16_t* buffer = malloc(buffer_size);
    if (!buffer) {
        drwav_uninit(&wav);
        return -1;
    }

    // Save format info before uninit (values may be invalid after uninit)
    int source_channels = wav.channels;
    int source_sample_rate = wav.sampleRate;

    // Decode to 16-bit PCM
    drwav_uint64 frames_read = drwav_read_pcm_frames_s16(&wav, wav.totalPCMFrameCount, buffer);
    drwav_uninit(&wav);

    // If mono, convert to stereo
    if (source_channels == 1) {
        int16_t* stereo_buffer = malloc(frames_read * sizeof(int16_t) * 2);
        for (drwav_uint64 i = 0; i < frames_read; i++) {
            stereo_buffer[i * 2] = buffer[i];
            stereo_buffer[i * 2 + 1] = buffer[i];
        }
        free(buffer);
        buffer = stereo_buffer;
        buffer_size = frames_read * sizeof(int16_t) * 2;
    }

    // Resample to target rate if needed
    int effective_sample_rate = source_sample_rate;
    if (source_sample_rate != SAMPLE_RATE) {
        size_t resampled_frames;
        int16_t* resampled = resample_linear(buffer, frames_read, source_sample_rate, SAMPLE_RATE, &resampled_frames);
        if (resampled) {
            free(buffer);
            buffer = resampled;
            buffer_size = resampled_frames * sizeof(int16_t) * AUDIO_CHANNELS;
            effective_sample_rate = SAMPLE_RATE;
        } else {
            LOG_error("WAV: Resampling failed, using native rate %d Hz\n", source_sample_rate);
        }
    }

    player.audio_data = buffer;
    player.audio_size = buffer_size;
    player.format = AUDIO_FORMAT_WAV;
    player.track_info.sample_rate = effective_sample_rate;

    return 0;
}

static int load_flac(const char* filepath) {
    // Open with metadata callback to read Vorbis comments
    drflac* flac = drflac_open_file_with_metadata(filepath, flac_metadata_callback, NULL, NULL);
    if (!flac) {
        LOG_error("Failed to load FLAC: %s\n", filepath);
        return -1;
    }

    player.track_info.sample_rate = flac->sampleRate;
    player.track_info.channels = flac->channels;
    player.track_info.duration_ms = (int)((flac->totalPCMFrameCount * 1000) / flac->sampleRate);

    // Allocate buffer
    size_t buffer_size = flac->totalPCMFrameCount * sizeof(int16_t) * AUDIO_CHANNELS;
    int16_t* buffer = malloc(buffer_size);
    if (!buffer) {
        drflac_close(flac);
        return -1;
    }

    // Decode to 16-bit PCM
    drflac_uint64 frames_read = drflac_read_pcm_frames_s16(flac, flac->totalPCMFrameCount, buffer);
    int channels = flac->channels;
    int sample_rate = flac->sampleRate;
    drflac_close(flac);

    // If mono, convert to stereo
    if (channels == 1) {
        int16_t* stereo_buffer = malloc(frames_read * sizeof(int16_t) * 2);
        for (drflac_uint64 i = 0; i < frames_read; i++) {
            stereo_buffer[i * 2] = buffer[i];
            stereo_buffer[i * 2 + 1] = buffer[i];
        }
        free(buffer);
        buffer = stereo_buffer;
        buffer_size = frames_read * sizeof(int16_t) * 2;
    }

    // Resample to target rate if needed
    int effective_sample_rate = sample_rate;
    if (sample_rate != SAMPLE_RATE) {
        size_t resampled_frames;
        int16_t* resampled = resample_linear(buffer, frames_read, sample_rate, SAMPLE_RATE, &resampled_frames);
        if (resampled) {
            free(buffer);
            buffer = resampled;
            buffer_size = resampled_frames * sizeof(int16_t) * AUDIO_CHANNELS;
            effective_sample_rate = SAMPLE_RATE;
        } else {
            LOG_error("FLAC: Resampling failed, using native rate %d Hz\n", sample_rate);
        }
    }

    player.audio_data = buffer;
    player.audio_size = buffer_size;
    player.format = AUDIO_FORMAT_FLAC;
    player.track_info.sample_rate = effective_sample_rate;

    return 0;
}

static int load_ogg(const char* filepath) {
    int error;
    stb_vorbis* vorbis = stb_vorbis_open_filename(filepath, &error, NULL);
    if (!vorbis) {
        LOG_error("Failed to load OGG: %s (error %d)\n", filepath, error);
        return -1;
    }

    stb_vorbis_info info = stb_vorbis_get_info(vorbis);
    int total_samples = stb_vorbis_stream_length_in_samples(vorbis);

    player.track_info.sample_rate = info.sample_rate;
    player.track_info.channels = info.channels;
    player.track_info.duration_ms = (int)((total_samples * 1000) / info.sample_rate);

    // Read Vorbis comments for metadata
    stb_vorbis_comment comments = stb_vorbis_get_comment(vorbis);
    for (int i = 0; i < comments.comment_list_length; i++) {
        parse_vorbis_comment(comments.comment_list[i]);
    }


    // Allocate buffer
    size_t buffer_size = total_samples * sizeof(int16_t) * AUDIO_CHANNELS;
    int16_t* buffer = malloc(buffer_size);
    if (!buffer) {
        stb_vorbis_close(vorbis);
        return -1;
    }

    // Save source sample rate before closing
    int source_sample_rate = info.sample_rate;

    // Decode to 16-bit interleaved PCM
    int samples_read = stb_vorbis_get_samples_short_interleaved(vorbis, AUDIO_CHANNELS, buffer, total_samples * AUDIO_CHANNELS);
    stb_vorbis_close(vorbis);

    // Resample to target rate if needed
    int effective_sample_rate = source_sample_rate;
    if (source_sample_rate != SAMPLE_RATE) {
        size_t resampled_frames;
        int16_t* resampled = resample_linear(buffer, samples_read, source_sample_rate, SAMPLE_RATE, &resampled_frames);
        if (resampled) {
            free(buffer);
            buffer = resampled;
            samples_read = resampled_frames;
            effective_sample_rate = SAMPLE_RATE;
        } else {
            LOG_error("OGG: Resampling failed, using native rate %d Hz\n", source_sample_rate);
        }
    }

    player.audio_data = buffer;
    player.audio_size = samples_read * sizeof(int16_t) * AUDIO_CHANNELS;
    player.format = AUDIO_FORMAT_OGG;
    player.track_info.sample_rate = effective_sample_rate;

    return 0;
}

// ============ PRELOAD SYSTEM ============

// Internal function to decode audio file into a buffer (for preloading)
static int decode_audio_file(const char* filepath, void** out_data, size_t* out_size,
                             TrackInfo* out_info, AudioFormat* out_format) {
    AudioFormat format = Player_detectFormat(filepath);
    if (format == AUDIO_FORMAT_UNKNOWN) {
        return -1;
    }

    void* audio_data = NULL;
    size_t audio_size = 0;
    TrackInfo info = {0};

    // Extract title from filename
    const char* filename = strrchr(filepath, '/');
    if (filename) filename++; else filename = filepath;
    strncpy(info.title, filename, sizeof(info.title) - 1);
    char* ext = strrchr(info.title, '.');
    if (ext) *ext = '\0';

    switch (format) {
        case AUDIO_FORMAT_MP3: {
            drmp3 mp3;
            if (!drmp3_init_file(&mp3, filepath, NULL)) return -1;

            info.sample_rate = mp3.sampleRate;
            info.channels = mp3.channels;
            drmp3_uint64 total_frames = drmp3_get_pcm_frame_count(&mp3);
            info.duration_ms = (int)((total_frames * 1000) / mp3.sampleRate);

            int source_channels = mp3.channels;
            int source_sample_rate = mp3.sampleRate;
            size_t buffer_size = total_frames * sizeof(int16_t) * AUDIO_CHANNELS;
            int16_t* buffer = malloc(buffer_size);
            if (!buffer) { drmp3_uninit(&mp3); return -1; }

            drmp3_uint64 frames_read = drmp3_read_pcm_frames_s16(&mp3, total_frames, buffer);
            drmp3_uninit(&mp3);

            if (source_channels == 1) {
                int16_t* stereo = malloc(frames_read * sizeof(int16_t) * 2);
                for (drmp3_uint64 i = 0; i < frames_read; i++) {
                    stereo[i * 2] = buffer[i];
                    stereo[i * 2 + 1] = buffer[i];
                }
                free(buffer);
                buffer = stereo;
                buffer_size = frames_read * sizeof(int16_t) * 2;
            }

            // Resample to target rate if needed
            int effective_rate = source_sample_rate;
            if (source_sample_rate != SAMPLE_RATE) {
                size_t resampled_frames;
                int16_t* resampled = resample_linear(buffer, frames_read, source_sample_rate, SAMPLE_RATE, &resampled_frames);
                if (resampled) {
                    free(buffer);
                    buffer = resampled;
                    buffer_size = resampled_frames * sizeof(int16_t) * AUDIO_CHANNELS;
                    effective_rate = SAMPLE_RATE;
                }
            }
            info.sample_rate = effective_rate;

            audio_data = buffer;
            audio_size = buffer_size;
            break;
        }
        case AUDIO_FORMAT_WAV: {
            drwav wav;
            if (!drwav_init_file(&wav, filepath, NULL)) return -1;

            info.sample_rate = wav.sampleRate;
            info.channels = wav.channels;
            info.duration_ms = (int)((wav.totalPCMFrameCount * 1000) / wav.sampleRate);

            int source_channels = wav.channels;
            int source_sample_rate = wav.sampleRate;
            size_t buffer_size = wav.totalPCMFrameCount * sizeof(int16_t) * AUDIO_CHANNELS;
            int16_t* buffer = malloc(buffer_size);
            if (!buffer) { drwav_uninit(&wav); return -1; }

            drwav_uint64 frames_read = drwav_read_pcm_frames_s16(&wav, wav.totalPCMFrameCount, buffer);
            drwav_uninit(&wav);

            if (source_channels == 1) {
                int16_t* stereo = malloc(frames_read * sizeof(int16_t) * 2);
                for (drwav_uint64 i = 0; i < frames_read; i++) {
                    stereo[i * 2] = buffer[i];
                    stereo[i * 2 + 1] = buffer[i];
                }
                free(buffer);
                buffer = stereo;
                buffer_size = frames_read * sizeof(int16_t) * 2;
            }

            // Resample to target rate if needed
            int effective_rate = source_sample_rate;
            if (source_sample_rate != SAMPLE_RATE) {
                size_t resampled_frames;
                int16_t* resampled = resample_linear(buffer, frames_read, source_sample_rate, SAMPLE_RATE, &resampled_frames);
                if (resampled) {
                    free(buffer);
                    buffer = resampled;
                    buffer_size = resampled_frames * sizeof(int16_t) * AUDIO_CHANNELS;
                    effective_rate = SAMPLE_RATE;
                }
            }
            info.sample_rate = effective_rate;

            audio_data = buffer;
            audio_size = buffer_size;
            break;
        }
        case AUDIO_FORMAT_FLAC: {
            drflac* flac = drflac_open_file(filepath, NULL);
            if (!flac) return -1;

            info.sample_rate = flac->sampleRate;
            info.channels = flac->channels;
            info.duration_ms = (int)((flac->totalPCMFrameCount * 1000) / flac->sampleRate);

            int source_channels = flac->channels;
            int source_sample_rate = flac->sampleRate;
            size_t buffer_size = flac->totalPCMFrameCount * sizeof(int16_t) * AUDIO_CHANNELS;
            int16_t* buffer = malloc(buffer_size);
            if (!buffer) { drflac_close(flac); return -1; }

            drflac_uint64 frames_read = drflac_read_pcm_frames_s16(flac, flac->totalPCMFrameCount, buffer);
            drflac_close(flac);

            if (source_channels == 1) {
                int16_t* stereo = malloc(frames_read * sizeof(int16_t) * 2);
                for (drflac_uint64 i = 0; i < frames_read; i++) {
                    stereo[i * 2] = buffer[i];
                    stereo[i * 2 + 1] = buffer[i];
                }
                free(buffer);
                buffer = stereo;
                buffer_size = frames_read * sizeof(int16_t) * 2;
            }

            // Resample to target rate if needed
            int effective_rate = source_sample_rate;
            if (source_sample_rate != SAMPLE_RATE) {
                size_t resampled_frames;
                int16_t* resampled = resample_linear(buffer, frames_read, source_sample_rate, SAMPLE_RATE, &resampled_frames);
                if (resampled) {
                    free(buffer);
                    buffer = resampled;
                    buffer_size = resampled_frames * sizeof(int16_t) * AUDIO_CHANNELS;
                    effective_rate = SAMPLE_RATE;
                }
            }
            info.sample_rate = effective_rate;

            audio_data = buffer;
            audio_size = buffer_size;
            break;
        }
        case AUDIO_FORMAT_OGG: {
            int error;
            stb_vorbis* vorbis = stb_vorbis_open_filename(filepath, &error, NULL);
            if (!vorbis) return -1;

            stb_vorbis_info vinfo = stb_vorbis_get_info(vorbis);
            int total_samples = stb_vorbis_stream_length_in_samples(vorbis);

            info.sample_rate = vinfo.sample_rate;
            info.channels = vinfo.channels;
            info.duration_ms = (int)((total_samples * 1000) / vinfo.sample_rate);

            int source_sample_rate = vinfo.sample_rate;
            size_t buffer_size = total_samples * sizeof(int16_t) * AUDIO_CHANNELS;
            int16_t* buffer = malloc(buffer_size);
            if (!buffer) { stb_vorbis_close(vorbis); return -1; }

            int samples_read = stb_vorbis_get_samples_short_interleaved(vorbis, AUDIO_CHANNELS, buffer, total_samples * AUDIO_CHANNELS);
            stb_vorbis_close(vorbis);

            // Resample to target rate if needed
            int effective_rate = source_sample_rate;
            if (source_sample_rate != SAMPLE_RATE) {
                size_t resampled_frames;
                int16_t* resampled = resample_linear(buffer, samples_read, source_sample_rate, SAMPLE_RATE, &resampled_frames);
                if (resampled) {
                    free(buffer);
                    buffer = resampled;
                    samples_read = resampled_frames;
                    effective_rate = SAMPLE_RATE;
                }
            }
            info.sample_rate = effective_rate;

            audio_data = buffer;
            audio_size = samples_read * sizeof(int16_t) * AUDIO_CHANNELS;
            break;
        }
        default:
            return -1;
    }

    *out_data = audio_data;
    *out_size = audio_size;
    *out_info = info;
    *out_format = format;
    return 0;
}

// Background thread function for preloading
static void* preload_thread_func(void* arg) {
    (void)arg;

    pthread_mutex_lock(&preload.mutex);
    char filepath[1024];
    strncpy(filepath, preload.filepath, sizeof(filepath) - 1);
    pthread_mutex_unlock(&preload.mutex);

    void* data = NULL;
    size_t size = 0;
    TrackInfo info = {0};
    AudioFormat format;

    int result = decode_audio_file(filepath, &data, &size, &info, &format);

    pthread_mutex_lock(&preload.mutex);
    if (result == 0) {
        preload.audio_data = data;
        preload.audio_size = size;
        preload.track_info = info;
        preload.format = format;
        preload.ready = 1;
    } else {
        preload.ready = -1;
    }
    preload.loading = 0;
    pthread_mutex_unlock(&preload.mutex);

    return NULL;
}

// Cancel any pending preload
static void cancel_preload(void) {
    pthread_mutex_lock(&preload.mutex);
    if (preload.loading) {
        pthread_mutex_unlock(&preload.mutex);
        pthread_join(preload.thread, NULL);
        pthread_mutex_lock(&preload.mutex);
    }
    if (preload.audio_data) {
        free(preload.audio_data);
        preload.audio_data = NULL;
    }
    preload.audio_size = 0;
    preload.ready = 0;
    preload.loading = 0;
    preload.filepath[0] = '\0';
    pthread_mutex_unlock(&preload.mutex);
}

// Start preloading a track in the background
void Player_preload(const char* filepath) {
    if (!filepath || !player.audio_initialized) return;

    // Cancel any existing preload
    cancel_preload();

    pthread_mutex_lock(&preload.mutex);
    strncpy(preload.filepath, filepath, sizeof(preload.filepath) - 1);
    preload.loading = 1;
    preload.ready = 0;
    pthread_mutex_unlock(&preload.mutex);

    // Start background thread
    pthread_create(&preload.thread, NULL, preload_thread_func, NULL);
}

// Reset audio device to default sample rate (for radio use)
void Player_resetSampleRate(void) {
    reconfigure_audio_device(SAMPLE_RATE);
}

// Set audio device to specific sample rate
void Player_setSampleRate(int sample_rate) {
    if (sample_rate > 0) {
        reconfigure_audio_device(sample_rate);
    }
}

// Check if preload is ready for a specific file
static int is_preload_ready(const char* filepath) {
    pthread_mutex_lock(&preload.mutex);
    int ready = (preload.ready == 1 && strcmp(preload.filepath, filepath) == 0);
    pthread_mutex_unlock(&preload.mutex);
    return ready;
}

int Player_load(const char* filepath) {
    if (!filepath || !player.audio_initialized) return -1;

    // Stop any current playback
    Player_stop();

    int result = -1;

    // Check if we have preloaded data for this file
    if (is_preload_ready(filepath)) {
        pthread_mutex_lock(&preload.mutex);

        // Use preloaded data
        player.audio_data = preload.audio_data;
        player.audio_size = preload.audio_size;
        player.track_info = preload.track_info;
        player.format = preload.format;

        // Clear preload (data now owned by player)
        preload.audio_data = NULL;
        preload.audio_size = 0;
        preload.ready = 0;
        preload.filepath[0] = '\0';

        pthread_mutex_unlock(&preload.mutex);

        // Store filename
        strncpy(player.current_file, filepath, sizeof(player.current_file) - 1);

        // Parse metadata for MP3
        if (player.format == AUDIO_FORMAT_MP3) {
            parse_mp3_metadata(filepath);
        }

        result = 0;
    } else {
        // Cancel any pending preload since we're loading a different file
        cancel_preload();

        pthread_mutex_lock(&player.mutex);

        // Detect format
        AudioFormat format = Player_detectFormat(filepath);
        if (format == AUDIO_FORMAT_UNKNOWN) {
            LOG_error("Unknown audio format: %s\n", filepath);
            pthread_mutex_unlock(&player.mutex);
            return -1;
        }

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

        // Load based on format
        switch (format) {
            case AUDIO_FORMAT_MP3:
                result = load_mp3(filepath);
                break;
            case AUDIO_FORMAT_WAV:
                result = load_wav(filepath);
                break;
            case AUDIO_FORMAT_FLAC:
                result = load_flac(filepath);
                break;
            case AUDIO_FORMAT_OGG:
                result = load_ogg(filepath);
                break;
            default:
                LOG_error("Unsupported format: %d\n", format);
                return -1;
        }
    }

    if (result == 0) {
        // Configure audio device to match the buffer's sample rate
        // (should be 48kHz if resampling succeeded, or native rate if it failed)
        reconfigure_audio_device(player.track_info.sample_rate);

        pthread_mutex_lock(&player.mutex);
        player.position_ms = 0;
        player.state = PLAYER_STATE_STOPPED;
        pthread_mutex_unlock(&player.mutex);

        // Generate waveform overview for progress display
        generate_waveform();
    }

    return result;
}

int Player_play(void) {
    if (!player.audio_data) return -1;

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
    pthread_mutex_lock(&player.mutex);

    SDL_PauseAudioDevice(player.audio_device, 1);

    player.state = PLAYER_STATE_STOPPED;
    player.position_ms = 0;
    audio_position_samples = 0;

    if (player.audio_data) {
        free(player.audio_data);
        player.audio_data = NULL;
    }
    player.audio_size = 0;

    memset(&player.track_info, 0, sizeof(TrackInfo));
    player.current_file[0] = '\0';

    // Clear waveform
    memset(&waveform, 0, sizeof(waveform));

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

void Player_update(void) {
    // Check if track finished
    pthread_mutex_lock(&player.mutex);
    if (player.state == PLAYER_STATE_PLAYING && player.audio_data) {
        int total_samples = player.audio_size / (sizeof(int16_t) * AUDIO_CHANNELS);
        int pos_samples = player.position_ms * current_sample_rate / 1000;
        if (pos_samples >= total_samples) {
            if (player.repeat) {
                player.position_ms = 0;
            } else {
                player.state = PLAYER_STATE_STOPPED;
                player.position_ms = 0;
            }
        }
    }
    pthread_mutex_unlock(&player.mutex);
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
