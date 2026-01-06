#define _GNU_SOURCE  // For strcasestr
#include "radio.h"
#include "player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <dirent.h>

#include "defines.h"
#include "api.h"
#include "include/parson/parson.h"

// mbedTLS for HTTPS support
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"

// MP3 streaming decoder (implementation is in player.c)
#include "audio/dr_mp3.h"

// AAC decoder (Helix)
#include "aacdec.h"

// Audio format types for radio streams
typedef enum {
    RADIO_FORMAT_UNKNOWN = 0,
    RADIO_FORMAT_MP3,
    RADIO_FORMAT_AAC
} RadioAudioFormat;

// Stream types
typedef enum {
    STREAM_TYPE_DIRECT = 0,  // Direct MP3/AAC stream (Shoutcast/Icecast)
    STREAM_TYPE_HLS          // HLS (m3u8 playlist)
} StreamType;

// HLS segment info
#define HLS_MAX_SEGMENTS 64
#define HLS_MAX_URL_LEN 1024

typedef struct {
    char url[HLS_MAX_URL_LEN];
    float duration;
    char title[256];
    char artist[256];
} HLSSegment;

typedef struct {
    char base_url[HLS_MAX_URL_LEN];      // Base URL for relative paths
    HLSSegment segments[HLS_MAX_SEGMENTS];
    int segment_count;
    int current_segment;
    float target_duration;
    int media_sequence;
    int last_played_sequence;            // Track last played sequence to avoid duplicates
    bool is_live;                         // Live stream (no ENDLIST)
    uint32_t last_playlist_fetch;         // For live stream refresh
} HLSContext;

#define SAMPLE_RATE 48000
#define AUDIO_CHANNELS 2

// Ring buffer for decoded audio
#define AUDIO_RING_SIZE (SAMPLE_RATE * 2 * 10)  // 10 seconds of stereo audio (reduced from 15s to save ~960KB)

// HLS segment buffer size (reduced from 512KB - typical HLS segment is ~60KB)
#define HLS_SEGMENT_BUF_SIZE (256 * 1024)

// Default radio stations
static RadioStation default_stations[] = {};

// Dynamic curated stations loaded from JSON files
#define MAX_CURATED_COUNTRIES 32
#define MAX_CURATED_STATIONS 256

static CuratedCountry curated_countries[MAX_CURATED_COUNTRIES];
static int curated_country_count = 0;

static CuratedStation curated_stations[MAX_CURATED_STATIONS];
static int curated_station_count = 0;

// Stations directory path (in pak folder)
static char stations_path[512] = "";

// Load stations from a JSON file for a specific country
static int load_country_stations(const char* filepath) {
    JSON_Value* root = json_parse_file(filepath);
    if (!root) {
        LOG_error("Failed to parse JSON: %s\n", filepath);
        return -1;
    }

    JSON_Object* obj = json_value_get_object(root);
    if (!obj) {
        json_value_free(root);
        return -1;
    }

    const char* country_name = json_object_get_string(obj, "country");
    const char* country_code = json_object_get_string(obj, "code");

    if (!country_name || !country_code) {
        json_value_free(root);
        return -1;
    }

    // Add country to list if not already present
    bool country_exists = false;
    for (int i = 0; i < curated_country_count; i++) {
        if (strcmp(curated_countries[i].code, country_code) == 0) {
            country_exists = true;
            break;
        }
    }

    if (!country_exists && curated_country_count < MAX_CURATED_COUNTRIES) {
        strncpy(curated_countries[curated_country_count].name, country_name, 63);
        strncpy(curated_countries[curated_country_count].code, country_code, 7);
        curated_country_count++;
    }

    // Load stations
    JSON_Array* stations_arr = json_object_get_array(obj, "stations");
    if (stations_arr) {
        int count = json_array_get_count(stations_arr);
        for (int i = 0; i < count && curated_station_count < MAX_CURATED_STATIONS; i++) {
            JSON_Object* station = json_array_get_object(stations_arr, i);
            if (!station) continue;

            const char* name = json_object_get_string(station, "name");
            const char* url = json_object_get_string(station, "url");
            const char* genre = json_object_get_string(station, "genre");
            const char* slogan = json_object_get_string(station, "slogan");

            if (name && url) {
                strncpy(curated_stations[curated_station_count].name, name, RADIO_MAX_NAME - 1);
                strncpy(curated_stations[curated_station_count].url, url, RADIO_MAX_URL - 1);
                strncpy(curated_stations[curated_station_count].genre, genre ? genre : "", 63);
                strncpy(curated_stations[curated_station_count].slogan, slogan ? slogan : "", 127);
                strncpy(curated_stations[curated_station_count].country_code, country_code, 7);
                curated_station_count++;
            }
        }
    }

    json_value_free(root);
    return 0;
}

// Scan stations directory and load all JSON files
static void load_curated_stations(void) {
    curated_country_count = 0;
    curated_station_count = 0;

    // Build stations path - look in pak folder first, then current directory
    const char* search_paths[] = {
        "%s/.system/tg5040/paks/Emus/Music Player.pak/stations",
        "./stations"
    };

    bool found = false;
    for (int i = 0; i < 2 && !found; i++) {
        if (i == 0) {
            snprintf(stations_path, sizeof(stations_path), search_paths[0], SDCARD_PATH);
        } else {
            strcpy(stations_path, search_paths[1]);
        }

        DIR* dir = opendir(stations_path);
        if (dir) {
            closedir(dir);
            found = true;
        }
    }

    if (!found) {
        return;
    }


    DIR* dir = opendir(stations_path);
    if (!dir) return;

    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        // Skip non-JSON files
        const char* ext = strrchr(ent->d_name, '.');
        if (!ext || strcasecmp(ext, ".json") != 0) continue;

        char filepath[768];
        snprintf(filepath, sizeof(filepath), "%s/%s", stations_path, ent->d_name);
        load_country_stations(filepath);
    }

    closedir(dir);
}

// Radio context
typedef struct {
    // State
    RadioState state;
    char error_msg[256];

    // Connection
    int socket_fd;
    char current_url[RADIO_MAX_URL];
    char redirect_url[RADIO_MAX_URL];  // For handling HTTP redirects

    // SSL/TLS support
    bool use_ssl;
    mbedtls_net_context ssl_net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config ssl_conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    bool ssl_initialized;

    // ICY metadata
    int icy_metaint;          // Bytes between metadata
    int bytes_until_meta;     // Countdown to next metadata
    RadioMetadata metadata;

    // Stream buffer (raw network data)
    uint8_t* stream_buffer;
    int stream_buffer_size;
    int stream_buffer_pos;

    // Audio ring buffer (decoded PCM)
    int16_t* audio_ring;
    int audio_ring_write;
    int audio_ring_read;
    int audio_ring_count;
    pthread_mutex_t audio_mutex;

    // Audio format detection
    RadioAudioFormat audio_format;

    // MP3 decoder (low-level for streaming)
    drmp3dec mp3_decoder;
    bool mp3_initialized;
    int mp3_sample_rate;
    int mp3_channels;

    // AAC decoder
    HAACDecoder aac_decoder;
    bool aac_initialized;
    uint8_t aac_inbuf[AAC_MAINBUF_SIZE * 2];
    int aac_inbuf_size;
    int aac_sample_rate;
    int aac_channels;

    // HLS support
    StreamType stream_type;
    HLSContext hls;
    uint8_t* hls_segment_buffer;     // Buffer for downloading segments
    int hls_segment_buffer_size;
    int hls_segment_buffer_pos;

    // Pre-allocated HLS buffers (to reduce memory fragmentation)
    uint8_t* hls_segment_buf;        // Segment download buffer
    uint8_t* hls_aac_buf;            // AAC decode buffer

    // TS demuxer state
    int ts_aac_pid;                  // PID of AAC audio stream
    bool ts_pid_detected;

    // Threading
    pthread_t stream_thread;
    bool thread_running;
    bool should_stop;

    // Stations
    RadioStation stations[RADIO_MAX_STATIONS];
    int station_count;
} RadioContext;

static RadioContext radio = {0};

// Parse URL into host, port, path, and detect HTTPS
// host_size and path_size are the buffer sizes
static int parse_url_safe(const char* url, char* host, int host_size, int* port, char* path, int path_size, bool* is_https) {
    if (!url || !host || !path || host_size < 1 || path_size < 1) {
        return -1;
    }

    *is_https = false;
    *port = 80;
    host[0] = '\0';
    path[0] = '\0';

    // Skip protocol
    const char* start = url;
    if (strncmp(url, "https://", 8) == 0) {
        start = url + 8;
        *is_https = true;
        *port = 443;
    } else if (strncmp(url, "http://", 7) == 0) {
        start = url + 7;
    }

    // Find path
    const char* path_start = strchr(start, '/');
    if (path_start) {
        snprintf(path, path_size, "%s", path_start);
    } else {
        strcpy(path, "/");
        path_start = start + strlen(start);
    }

    // Find port
    const char* port_start = strchr(start, ':');
    if (port_start && port_start < path_start) {
        *port = atoi(port_start + 1);
        int host_len = port_start - start;
        if (host_len >= host_size) host_len = host_size - 1;
        memcpy(host, start, host_len);
        host[host_len] = '\0';
    } else {
        int host_len = path_start - start;
        if (host_len >= host_size) host_len = host_size - 1;
        memcpy(host, start, host_len);
        host[host_len] = '\0';
    }

    return 0;
}

// Legacy wrapper for compatibility
static int parse_url(const char* url, char* host, int* port, char* path, bool* is_https) {
    return parse_url_safe(url, host, 256, port, path, 512, is_https);
}

// Initialize SSL/TLS
static int ssl_init(const char* host) {
    int ret;
    const char* pers = "radio_client";

    mbedtls_net_init(&radio.ssl_net);
    mbedtls_ssl_init(&radio.ssl);
    mbedtls_ssl_config_init(&radio.ssl_conf);
    mbedtls_entropy_init(&radio.entropy);
    mbedtls_ctr_drbg_init(&radio.ctr_drbg);

    // Seed random number generator
    ret = mbedtls_ctr_drbg_seed(&radio.ctr_drbg, mbedtls_entropy_func,
                                 &radio.entropy, (const unsigned char*)pers, strlen(pers));
    if (ret != 0) {
        LOG_error("mbedtls_ctr_drbg_seed failed: %d\n", ret);
        return -1;
    }

    // Set up SSL config
    ret = mbedtls_ssl_config_defaults(&radio.ssl_conf,
                                       MBEDTLS_SSL_IS_CLIENT,
                                       MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        LOG_error("mbedtls_ssl_config_defaults failed: %d\n", ret);
        return -1;
    }

    // Skip certificate verification (radio streams use various CAs)
    mbedtls_ssl_conf_authmode(&radio.ssl_conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&radio.ssl_conf, mbedtls_ctr_drbg_random, &radio.ctr_drbg);

    // Set up SSL context
    ret = mbedtls_ssl_setup(&radio.ssl, &radio.ssl_conf);
    if (ret != 0) {
        LOG_error("mbedtls_ssl_setup failed: %d\n", ret);
        return -1;
    }

    // Set hostname for SNI
    ret = mbedtls_ssl_set_hostname(&radio.ssl, host);
    if (ret != 0) {
        LOG_error("mbedtls_ssl_set_hostname failed: %d\n", ret);
        return -1;
    }

    radio.ssl_initialized = true;
    return 0;
}

// Cleanup SSL
static void ssl_cleanup(void) {
    if (radio.ssl_initialized) {
        mbedtls_ssl_close_notify(&radio.ssl);
        mbedtls_net_free(&radio.ssl_net);
        mbedtls_ssl_free(&radio.ssl);
        mbedtls_ssl_config_free(&radio.ssl_conf);
        mbedtls_ctr_drbg_free(&radio.ctr_drbg);
        mbedtls_entropy_free(&radio.entropy);
        radio.ssl_initialized = false;
    }
}

// Send wrapper (works with both HTTP and HTTPS)
static int radio_send(const void* buf, size_t len) {
    if (radio.use_ssl) {
        return mbedtls_ssl_write(&radio.ssl, buf, len);
    } else {
        return send(radio.socket_fd, buf, len, 0);
    }
}

// Receive wrapper (works with both HTTP and HTTPS)
static int radio_recv(void* buf, size_t len) {
    if (radio.use_ssl) {
        return mbedtls_ssl_read(&radio.ssl, buf, len);
    } else {
        return recv(radio.socket_fd, buf, len, 0);
    }
}

// Connect to stream server (supports HTTP and HTTPS)
static int connect_stream(const char* url) {
    char host[256], path[512];
    int port;
    bool is_https;
    int ret;

    if (parse_url(url, host, &port, path, &is_https) != 0) {
        snprintf(radio.error_msg, sizeof(radio.error_msg), "Invalid URL");
        return -1;
    }

    radio.use_ssl = is_https;

    if (is_https) {
        // HTTPS connection using mbedTLS
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", port);

        // Initialize SSL
        if (ssl_init(host) != 0) {
            snprintf(radio.error_msg, sizeof(radio.error_msg), "SSL init failed");
            return -1;
        }

        // Connect using mbedTLS
        ret = mbedtls_net_connect(&radio.ssl_net, host, port_str, MBEDTLS_NET_PROTO_TCP);
        if (ret != 0) {
            LOG_error("mbedtls_net_connect failed: %d\n", ret);
            ssl_cleanup();
            snprintf(radio.error_msg, sizeof(radio.error_msg), "Connection failed");
            return -1;
        }

        // Set socket for SSL
        mbedtls_ssl_set_bio(&radio.ssl, &radio.ssl_net,
                            mbedtls_net_send, mbedtls_net_recv, NULL);

        // SSL handshake
        while ((ret = mbedtls_ssl_handshake(&radio.ssl)) != 0) {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                LOG_error("mbedtls_ssl_handshake failed: %d\n", ret);
                ssl_cleanup();
                snprintf(radio.error_msg, sizeof(radio.error_msg), "SSL handshake failed");
                return -1;
            }
        }


        // Store socket fd for select() compatibility
        radio.socket_fd = radio.ssl_net.fd;
    } else {
        // Plain HTTP connection - use getaddrinfo (thread-safe)
        struct addrinfo hints, *result = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", port);

        int gai_ret = getaddrinfo(host, port_str, &hints, &result);
        if (gai_ret != 0 || !result) {
            if (result) freeaddrinfo(result);
            snprintf(radio.error_msg, sizeof(radio.error_msg), "DNS lookup failed");
            return -1;
        }

        radio.socket_fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
        if (radio.socket_fd < 0) {
            freeaddrinfo(result);
            snprintf(radio.error_msg, sizeof(radio.error_msg), "Socket creation failed");
            return -1;
        }

        struct timeval tv;
        tv.tv_sec = 10;
        tv.tv_usec = 0;
        setsockopt(radio.socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(radio.socket_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(radio.socket_fd, result->ai_addr, result->ai_addrlen) < 0) {
            close(radio.socket_fd);
            radio.socket_fd = -1;
            freeaddrinfo(result);
            snprintf(radio.error_msg, sizeof(radio.error_msg), "Connection failed");
            return -1;
        }
        freeaddrinfo(result);
    }

    // Send HTTP request with ICY headers
    char request[1024];
    snprintf(request, sizeof(request),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: MusicPlayer/1.0\r\n"
        "Accept: */*\r\n"
        "Icy-MetaData: 1\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host);

    if (radio_send(request, strlen(request)) < 0) {
        if (radio.use_ssl) {
            ssl_cleanup();
        } else {
            close(radio.socket_fd);
        }
        radio.socket_fd = -1;
        snprintf(radio.error_msg, sizeof(radio.error_msg), "Send failed");
        return -1;
    }

    return 0;
}

// Parse HTTP response headers
// Returns: 0 = success, 1 = redirect (check redirect_url), -1 = error
static int parse_headers(void) {
    char header_buf[4096];
    int header_pos = 0;
    char c;

    radio.redirect_url[0] = '\0';

    // Read headers until \r\n\r\n
    while (header_pos < sizeof(header_buf) - 1) {
        if (radio_recv(&c, 1) != 1) {
            snprintf(radio.error_msg, sizeof(radio.error_msg), "Header read failed");
            return -1;
        }
        header_buf[header_pos++] = c;

        // Check for end of headers
        if (header_pos >= 4 &&
            header_buf[header_pos-4] == '\r' && header_buf[header_pos-3] == '\n' &&
            header_buf[header_pos-2] == '\r' && header_buf[header_pos-1] == '\n') {
            break;
        }
    }
    header_buf[header_pos] = '\0';


    // Check for HTTP or ICY response
    if (strncmp(header_buf, "HTTP/1.", 7) != 0 && strncmp(header_buf, "ICY", 3) != 0) {
        snprintf(radio.error_msg, sizeof(radio.error_msg), "Invalid response");
        return -1;
    }

    // Check HTTP status code for redirects
    int http_status = 0;
    if (strncmp(header_buf, "HTTP/1.", 7) == 0) {
        // Parse status code (e.g., "HTTP/1.1 302 Found")
        char* status_start = header_buf + 9;  // Skip "HTTP/1.X "
        http_status = atoi(status_start);

        // Check for redirect (301, 302, 303, 307, 308)
        if (http_status >= 300 && http_status < 400) {
            // Find Location header
            char* loc = strcasestr(header_buf, "\nLocation:");
            if (!loc) loc = strcasestr(header_buf, "\rlocation:");
            if (loc) {
                loc += 10;  // Skip "Location:"
                while (*loc == ' ' || *loc == '\t') loc++;  // Skip whitespace

                // Copy until end of line
                char* end = loc;
                while (*end && *end != '\r' && *end != '\n') end++;

                int len = end - loc;
                if (len > 0 && len < RADIO_MAX_URL) {
                    strncpy(radio.redirect_url, loc, len);
                    radio.redirect_url[len] = '\0';
                    return 1;  // Indicate redirect
                }
            }
            snprintf(radio.error_msg, sizeof(radio.error_msg), "Redirect without Location");
            return -1;
        }

        // Check for error status
        if (http_status >= 400) {
            snprintf(radio.error_msg, sizeof(radio.error_msg), "HTTP error %d", http_status);
            return -1;
        }
    }

    // Parse ICY headers
    radio.icy_metaint = 0;
    radio.metadata.bitrate = 0;
    radio.metadata.station_name[0] = '\0';
    radio.metadata.content_type[0] = '\0';

    char* line = strtok(header_buf, "\r\n");
    while (line) {
        if (strncasecmp(line, "icy-metaint:", 12) == 0) {
            radio.icy_metaint = atoi(line + 12);
        }
        else if (strncasecmp(line, "icy-br:", 7) == 0) {
            radio.metadata.bitrate = atoi(line + 7);
        }
        else if (strncasecmp(line, "icy-name:", 9) == 0) {
            strncpy(radio.metadata.station_name, line + 9, sizeof(radio.metadata.station_name) - 1);
            // Trim leading space
            char* name = radio.metadata.station_name;
            while (*name == ' ') name++;
            memmove(radio.metadata.station_name, name, strlen(name) + 1);
        }
        else if (strncasecmp(line, "content-type:", 13) == 0) {
            strncpy(radio.metadata.content_type, line + 13, sizeof(radio.metadata.content_type) - 1);
        }
        line = strtok(NULL, "\r\n");
    }

    radio.bytes_until_meta = radio.icy_metaint;

    // Detect audio format from content type
    radio.audio_format = RADIO_FORMAT_MP3;  // Default to MP3
    const char* ct = radio.metadata.content_type;
    if (ct[0]) {
        // Skip leading whitespace
        while (*ct == ' ') ct++;

        if (strcasestr(ct, "aac") != NULL ||
            strcasestr(ct, "mp4") != NULL ||
            strcasestr(ct, "m4a") != NULL) {
            radio.audio_format = RADIO_FORMAT_AAC;
        } else if (strcasestr(ct, "mpeg") != NULL ||
                   strcasestr(ct, "mp3") != NULL) {
            radio.audio_format = RADIO_FORMAT_MP3;
        }
    }

    return 0;
}

// Parse ICY metadata block
static void parse_icy_metadata(const uint8_t* data, int len) {
    // Format: StreamTitle='Artist - Title';StreamUrl='...';
    char meta[4096];
    if (len >= sizeof(meta)) len = sizeof(meta) - 1;
    memcpy(meta, data, len);
    meta[len] = '\0';


    // Find StreamTitle
    char* title_start = strstr(meta, "StreamTitle='");
    if (title_start) {
        title_start += 13;
        char* title_end = strchr(title_start, '\'');
        if (title_end) {
            *title_end = '\0';
            strncpy(radio.metadata.title, title_start, sizeof(radio.metadata.title) - 1);

            // Try to parse "Artist - Title" format
            char* separator = strstr(radio.metadata.title, " - ");
            if (separator) {
                *separator = '\0';
                strncpy(radio.metadata.artist, radio.metadata.title, sizeof(radio.metadata.artist) - 1);
                memmove(radio.metadata.title, separator + 3, strlen(separator + 3) + 1);
            } else {
                radio.metadata.artist[0] = '\0';
            }
        }
    }
}

// Parse ID3 tags from HLS segment (returns bytes to skip, or 0 if no ID3)
static int parse_hls_id3_metadata(const uint8_t* data, int len) {
    // ID3 tag header: "ID3" + version (2 bytes) + flags (1 byte) + size (4 bytes syncsafe)
    if (len < 10) return 0;
    if (data[0] != 'I' || data[1] != 'D' || data[2] != '3') return 0;

    uint8_t version_major = data[3];  // 3 = ID3v2.3, 4 = ID3v2.4
    uint8_t version_minor = data[4];
    uint8_t flags = data[5];

    // Parse syncsafe size (4 bytes, 7 bits each)
    uint32_t tag_size = ((data[6] & 0x7F) << 21) |
                        ((data[7] & 0x7F) << 14) |
                        ((data[8] & 0x7F) << 7) |
                        (data[9] & 0x7F);

    int total_size = 10 + tag_size;  // Header + tag data
    if (total_size > len) return 0;  // Incomplete tag


    // Parse ID3 frames
    int pos = 10;
    while (pos + 10 < total_size) {
        // Frame header: ID (4 bytes) + size (4 bytes) + flags (2 bytes)
        char frame_id[5] = {data[pos], data[pos+1], data[pos+2], data[pos+3], 0};

        // Frame size: syncsafe in ID3v2.4, regular in ID3v2.3
        uint32_t frame_size;
        if (version_major >= 4) {
            frame_size = ((data[pos+4] & 0x7F) << 21) | ((data[pos+5] & 0x7F) << 14) |
                         ((data[pos+6] & 0x7F) << 7) | (data[pos+7] & 0x7F);
        } else {
            frame_size = (data[pos+4] << 24) | (data[pos+5] << 16) |
                         (data[pos+6] << 8) | data[pos+7];
        }

        if (frame_size == 0 || pos + 10 + frame_size > total_size) break;

        const uint8_t* frame_data = &data[pos + 10];

        // Debug: log frame info

        // TIT2 = Title
        if (strcmp(frame_id, "TIT2") == 0 && frame_size > 1) {
            int encoding = frame_data[0];
            const char* text = (const char*)&frame_data[1];
            int text_len = frame_size - 1;
            if (text_len > sizeof(radio.metadata.title) - 1)
                text_len = sizeof(radio.metadata.title) - 1;

            if (encoding == 0 || encoding == 3) {  // ISO-8859-1 or UTF-8
                memcpy(radio.metadata.title, text, text_len);
                radio.metadata.title[text_len] = '\0';
            }
        }
        // TPE1 = Artist
        else if (strcmp(frame_id, "TPE1") == 0 && frame_size > 1) {
            int encoding = frame_data[0];
            const char* text = (const char*)&frame_data[1];
            int text_len = frame_size - 1;
            if (text_len > sizeof(radio.metadata.artist) - 1)
                text_len = sizeof(radio.metadata.artist) - 1;

            if (encoding == 0 || encoding == 3) {  // ISO-8859-1 or UTF-8
                memcpy(radio.metadata.artist, text, text_len);
                radio.metadata.artist[text_len] = '\0';
            }
        }
        // TXXX = User-defined text (may contain StreamTitle)
        else if (strcmp(frame_id, "TXXX") == 0 && frame_size > 1) {
            int encoding = frame_data[0];
            if (encoding == 0 || encoding == 3) {
                const char* desc = (const char*)&frame_data[1];
                // Look for StreamTitle in TXXX
                if (strstr(desc, "StreamTitle") || strstr(desc, "TITLE")) {
                    // Find the null terminator after description
                    const char* value = desc;
                    while (*value && (value - (const char*)frame_data) < (int)frame_size) value++;
                    value++;  // Skip null

                    if ((value - (const char*)frame_data) < (int)frame_size) {
                        int val_len = frame_size - (value - (const char*)frame_data);
                        if (val_len > sizeof(radio.metadata.title) - 1)
                            val_len = sizeof(radio.metadata.title) - 1;

                        memcpy(radio.metadata.title, value, val_len);
                        radio.metadata.title[val_len] = '\0';

                        // Try to parse "Artist - Title" format
                        char* separator = strstr(radio.metadata.title, " - ");
                        if (separator) {
                            *separator = '\0';
                            strncpy(radio.metadata.artist, radio.metadata.title,
                                    sizeof(radio.metadata.artist) - 1);
                            memmove(radio.metadata.title, separator + 3,
                                    strlen(separator + 3) + 1);
                        }
                    }
                }
            }
        }
        // PRIV = Private frame (some streams put metadata here)
        else if (strcmp(frame_id, "PRIV") == 0 && frame_size > 0) {
            // PRIV format: owner-identifier (null-terminated) + binary data
            const char* owner = (const char*)frame_data;

            // Look for StreamTitle in private data
            char priv_data[512];
            int copy_len = frame_size < sizeof(priv_data) - 1 ? frame_size : sizeof(priv_data) - 1;
            memcpy(priv_data, frame_data, copy_len);
            priv_data[copy_len] = '\0';

            char* title_start = strstr(priv_data, "StreamTitle='");
            if (title_start) {
                title_start += 13;
                char* title_end = strchr(title_start, '\'');
                if (title_end) {
                    *title_end = '\0';
                    strncpy(radio.metadata.title, title_start, sizeof(radio.metadata.title) - 1);

                    char* separator = strstr(radio.metadata.title, " - ");
                    if (separator) {
                        *separator = '\0';
                        strncpy(radio.metadata.artist, radio.metadata.title,
                                sizeof(radio.metadata.artist) - 1);
                        memmove(radio.metadata.title, separator + 3,
                                strlen(separator + 3) + 1);
                    } else {
                        radio.metadata.artist[0] = '\0';
                    }
                }
            }
        }
        // Log first few bytes of unknown frames for debugging
        else {
            char hex[64] = {0};
            int hex_len = frame_size < 20 ? frame_size : 20;
            for (int i = 0; i < hex_len; i++) {
                sprintf(hex + i*3, "%02X ", frame_data[i]);
            }
        }

        pos += 10 + frame_size;
    }

    return total_size;  // Return bytes to skip
}

// ============== HLS SUPPORT ==============

// Check if URL is an HLS stream
static bool is_hls_url(const char* url) {
    const char* ext = strrchr(url, '.');
    if (ext && strcasecmp(ext, ".m3u8") == 0) return true;
    // Also check for m3u8 in query string
    if (strstr(url, ".m3u8") != NULL) return true;
    return false;
}

// Extract base URL from full URL (for resolving relative paths)
static void get_base_url(const char* url, char* base, int base_size) {
    strncpy(base, url, base_size - 1);
    base[base_size - 1] = '\0';

    // Find last slash after the host
    char* last_slash = strrchr(base, '/');
    if (last_slash && last_slash > base + 8) {  // After "https://"
        *(last_slash + 1) = '\0';
    }
}

// Resolve a potentially relative URL
static void resolve_url(const char* base, const char* relative, char* result, int result_size) {
    if (strncmp(relative, "http://", 7) == 0 || strncmp(relative, "https://", 8) == 0) {
        // Absolute URL
        strncpy(result, relative, result_size - 1);
        result[result_size - 1] = '\0';
    } else if (relative[0] == '/') {
        // Root-relative URL - extract host from base
        const char* host_start = strstr(base, "://");
        if (host_start) {
            host_start += 3;
            const char* host_end = strchr(host_start, '/');
            if (host_end) {
                int host_len = host_end - base;
                strncpy(result, base, host_len);
                result[host_len] = '\0';
                strncat(result, relative, result_size - strlen(result) - 1);
            } else {
                snprintf(result, result_size, "%s%s", base, relative);
            }
        }
    } else {
        // Relative URL
        snprintf(result, result_size, "%s%s", base, relative);
    }
}

// SSL context for fetch_url_content (heap allocated to save stack space)
typedef struct {
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    bool initialized;
} FetchSSLContext;

// Fetch content from URL into buffer (for HLS playlists and segments)
// Returns bytes read, or -1 on error
static int fetch_url_content(const char* url, uint8_t* buffer, int buffer_size, char* content_type, int ct_size) {

    if (!url || !buffer || buffer_size <= 0) {
        LOG_error("[HLS] Invalid parameters to fetch_url_content\n");
        return -1;
    }

    // Use heap for URL components to reduce stack usage
    char* host = (char*)malloc(256);
    char* path = (char*)malloc(512);
    if (!host || !path) {
        LOG_error("[HLS] Failed to allocate host/path buffers\n");
        free(host);
        free(path);
        return -1;
    }

    int port;
    bool is_https;

    if (parse_url(url, host, &port, path, &is_https) != 0) {
        LOG_error("[HLS] Failed to parse URL: %s\n", url);
        free(host);
        free(path);
        return -1;
    }


    int sock_fd = -1;
    FetchSSLContext* ssl_ctx = NULL;
    char* header_buf = NULL;  // Will be allocated later, initialized here for cleanup

    if (is_https) {
        // Allocate SSL context on heap to avoid stack overflow
        ssl_ctx = (FetchSSLContext*)calloc(1, sizeof(FetchSSLContext));
        if (!ssl_ctx) {
            LOG_error("[HLS] Failed to allocate SSL context\n");
            free(host);
            free(path);
            return -1;
        }

        const char* pers = "hls_fetch";
        mbedtls_net_init(&ssl_ctx->net);
        mbedtls_ssl_init(&ssl_ctx->ssl);
        mbedtls_ssl_config_init(&ssl_ctx->conf);
        mbedtls_entropy_init(&ssl_ctx->entropy);
        mbedtls_ctr_drbg_init(&ssl_ctx->ctr_drbg);

        if (mbedtls_ctr_drbg_seed(&ssl_ctx->ctr_drbg, mbedtls_entropy_func, &ssl_ctx->entropy,
                                   (const unsigned char*)pers, strlen(pers)) != 0) {
            goto cleanup;
        }

        if (mbedtls_ssl_config_defaults(&ssl_ctx->conf, MBEDTLS_SSL_IS_CLIENT,
                                         MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
            goto cleanup;
        }

        mbedtls_ssl_conf_authmode(&ssl_ctx->conf, MBEDTLS_SSL_VERIFY_NONE);
        mbedtls_ssl_conf_rng(&ssl_ctx->conf, mbedtls_ctr_drbg_random, &ssl_ctx->ctr_drbg);

        if (mbedtls_ssl_setup(&ssl_ctx->ssl, &ssl_ctx->conf) != 0) {
            goto cleanup;
        }

        mbedtls_ssl_set_hostname(&ssl_ctx->ssl, host);

        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", port);

        if (mbedtls_net_connect(&ssl_ctx->net, host, port_str, MBEDTLS_NET_PROTO_TCP) != 0) {
            goto cleanup;
        }

        mbedtls_ssl_set_bio(&ssl_ctx->ssl, &ssl_ctx->net, mbedtls_net_send, mbedtls_net_recv, NULL);

        int ret;
        while ((ret = mbedtls_ssl_handshake(&ssl_ctx->ssl)) != 0) {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                goto cleanup;
            }
        }

        ssl_ctx->initialized = true;
        sock_fd = ssl_ctx->net.fd;
    } else {

        // Use getaddrinfo instead of gethostbyname (thread-safe)
        struct addrinfo hints, *result = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", port);

        int gai_ret = getaddrinfo(host, port_str, &hints, &result);
        if (gai_ret != 0 || !result) {
            LOG_error("[HLS] getaddrinfo failed for host: %s (error: %d)\n", host, gai_ret);
            if (result) freeaddrinfo(result);
            free(host);
            free(path);
            return -1;
        }

        sock_fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
        if (sock_fd < 0) {
            LOG_error("[HLS] socket() failed\n");
            freeaddrinfo(result);
            free(host);
            free(path);
            return -1;
        }

        struct timeval tv = {10, 0};
        setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(sock_fd, result->ai_addr, result->ai_addrlen) < 0) {
            LOG_error("[HLS] connect() failed\n");
            close(sock_fd);
            freeaddrinfo(result);
            free(host);
            free(path);
            return -1;
        }
        freeaddrinfo(result);
    }

    // Send HTTP request (use smaller buffer)
    char request[512];
    snprintf(request, sizeof(request),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: MusicPlayer/1.0\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host);

    int sent;
    if (is_https) {
        sent = mbedtls_ssl_write(&ssl_ctx->ssl, (unsigned char*)request, strlen(request));
    } else {
        sent = send(sock_fd, request, strlen(request), 0);
    }

    if (sent < 0) {
        LOG_error("[HLS] send() failed\n");
        goto cleanup;
    }

    // Read response - allocate header buffer on heap to reduce stack pressure
    #define HEADER_BUF_SIZE 2048
    header_buf = (char*)malloc(HEADER_BUF_SIZE);
    if (!header_buf) {
        LOG_error("[HLS] Failed to allocate header buffer\n");
        goto cleanup;
    }
    int header_pos = 0;
    bool headers_done = false;

    // Read headers
    while (header_pos < HEADER_BUF_SIZE - 1) {
        char c;
        int r;
        if (is_https) {
            r = mbedtls_ssl_read(&ssl_ctx->ssl, (unsigned char*)&c, 1);
        } else {
            r = recv(sock_fd, &c, 1, 0);
        }
        if (r != 1) break;

        header_buf[header_pos++] = c;
        if (header_pos >= 4 &&
            header_buf[header_pos-4] == '\r' && header_buf[header_pos-3] == '\n' &&
            header_buf[header_pos-2] == '\r' && header_buf[header_pos-1] == '\n') {
            headers_done = true;
            break;
        }
    }
    header_buf[header_pos] = '\0';

    if (!headers_done) goto cleanup;

    // Check for redirect
    if (strstr(header_buf, "301") || strstr(header_buf, "302") ||
        strstr(header_buf, "303") || strstr(header_buf, "307")) {
        char* loc = strcasestr(header_buf, "\nLocation:");
        if (loc) {
            loc += 10;
            while (*loc == ' ') loc++;
            char* end = loc;
            while (*end && *end != '\r' && *end != '\n') end++;

            // Copy redirect URL before cleanup
            char redirect_url[1024];
            int rlen = end - loc;
            if (rlen >= sizeof(redirect_url)) rlen = sizeof(redirect_url) - 1;
            strncpy(redirect_url, loc, rlen);
            redirect_url[rlen] = '\0';

            // Cleanup current connection and buffers before recursive call
            if (ssl_ctx) {
                mbedtls_ssl_close_notify(&ssl_ctx->ssl);
                mbedtls_net_free(&ssl_ctx->net);
                mbedtls_ssl_free(&ssl_ctx->ssl);
                mbedtls_ssl_config_free(&ssl_ctx->conf);
                mbedtls_ctr_drbg_free(&ssl_ctx->ctr_drbg);
                mbedtls_entropy_free(&ssl_ctx->entropy);
                free(ssl_ctx);
            } else {
                close(sock_fd);
            }
            free(header_buf);
            free(host);
            free(path);

            // Follow redirect
            return fetch_url_content(redirect_url, buffer, buffer_size, content_type, ct_size);
        }
        goto cleanup;
    }

    // Extract content type if requested
    if (content_type && ct_size > 0) {
        content_type[0] = '\0';
        char* ct = strcasestr(header_buf, "\nContent-Type:");
        if (ct) {
            ct += 14;
            while (*ct == ' ') ct++;
            char* end = ct;
            while (*end && *end != '\r' && *end != '\n' && *end != ';') end++;
            int len = end - ct;
            if (len < ct_size) {
                strncpy(content_type, ct, len);
                content_type[len] = '\0';
            }
        }
    }

    // Read body
    int total_read = 0;
    while (total_read < buffer_size - 1) {
        int r;
        if (is_https) {
            r = mbedtls_ssl_read(&ssl_ctx->ssl, buffer + total_read, buffer_size - total_read - 1);
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        } else {
            r = recv(sock_fd, buffer + total_read, buffer_size - total_read - 1, 0);
        }
        if (r <= 0) break;
        total_read += r;
    }

    // Cleanup
    if (ssl_ctx) {
        mbedtls_ssl_close_notify(&ssl_ctx->ssl);
        mbedtls_net_free(&ssl_ctx->net);
        mbedtls_ssl_free(&ssl_ctx->ssl);
        mbedtls_ssl_config_free(&ssl_ctx->conf);
        mbedtls_ctr_drbg_free(&ssl_ctx->ctr_drbg);
        mbedtls_entropy_free(&ssl_ctx->entropy);
        free(ssl_ctx);
    } else {
        close(sock_fd);
    }

    free(header_buf);
    free(host);
    free(path);
    return total_read;

cleanup:
    if (ssl_ctx) {
        if (ssl_ctx->initialized) {
            mbedtls_ssl_close_notify(&ssl_ctx->ssl);
        }
        mbedtls_net_free(&ssl_ctx->net);
        mbedtls_ssl_free(&ssl_ctx->ssl);
        mbedtls_ssl_config_free(&ssl_ctx->conf);
        mbedtls_ctr_drbg_free(&ssl_ctx->ctr_drbg);
        mbedtls_entropy_free(&ssl_ctx->entropy);
        free(ssl_ctx);
    } else if (sock_fd >= 0) {
        close(sock_fd);
    }
    free(header_buf);
    free(host);
    free(path);
    return -1;
}

// Parse M3U8 playlist content
// Note: Does NOT reset current_segment - caller must handle that
static int parse_m3u8_playlist(const char* content, const char* base_url) {
    radio.hls.segment_count = 0;
    // Don't reset current_segment here - let caller handle it for proper refresh logic
    radio.hls.is_live = true;  // Assume live until we see ENDLIST
    radio.hls.target_duration = 10.0f;
    radio.hls.media_sequence = 0;

    strncpy(radio.hls.base_url, base_url, HLS_MAX_URL_LEN - 1);

    const char* line = content;
    float segment_duration = 0;
    char segment_title[256] = "";
    char segment_artist[256] = "";
    char variant_url[HLS_MAX_URL_LEN] = "";
    bool is_master_playlist = false;

    while (*line && radio.hls.segment_count < HLS_MAX_SEGMENTS) {
        // Skip whitespace
        while (*line == ' ' || *line == '\t') line++;

        // Find end of line
        const char* eol = line;
        while (*eol && *eol != '\n' && *eol != '\r') eol++;
        int line_len = eol - line;

        if (line_len > 0) {
            char line_buf[HLS_MAX_URL_LEN];
            if (line_len >= HLS_MAX_URL_LEN) line_len = HLS_MAX_URL_LEN - 1;
            strncpy(line_buf, line, line_len);
            line_buf[line_len] = '\0';

            if (strncmp(line_buf, "#EXTM3U", 7) == 0) {
                // Valid M3U8 header
            } else if (strncmp(line_buf, "#EXT-X-STREAM-INF:", 18) == 0) {
                // Master playlist - we need to fetch the variant
                is_master_playlist = true;
            } else if (strncmp(line_buf, "#EXT-X-TARGETDURATION:", 22) == 0) {
                radio.hls.target_duration = atof(line_buf + 22);
            } else if (strncmp(line_buf, "#EXT-X-MEDIA-SEQUENCE:", 22) == 0) {
                radio.hls.media_sequence = atoi(line_buf + 22);
            } else if (strncmp(line_buf, "#EXTINF:", 8) == 0) {
                // Segment duration and optional metadata
                // Format: #EXTINF:10,title="...",artist="..."
                segment_duration = atof(line_buf + 8);
                segment_title[0] = '\0';
                segment_artist[0] = '\0';

                // Parse title="..."
                char* title_start = strstr(line_buf, "title=\"");
                if (title_start) {
                    title_start += 7;  // Skip 'title="'
                    char* title_end = strchr(title_start, '"');
                    if (title_end) {
                        int len = title_end - title_start;
                        if (len > 255) len = 255;
                        strncpy(segment_title, title_start, len);
                        segment_title[len] = '\0';
                    }
                }

                // Parse artist="..."
                char* artist_start = strstr(line_buf, "artist=\"");
                if (artist_start) {
                    artist_start += 8;  // Skip 'artist="'
                    char* artist_end = strchr(artist_start, '"');
                    if (artist_end) {
                        int len = artist_end - artist_start;
                        if (len > 255) len = 255;
                        strncpy(segment_artist, artist_start, len);
                        segment_artist[len] = '\0';
                    }
                }
            } else if (strncmp(line_buf, "#EXT-X-ENDLIST", 14) == 0) {
                radio.hls.is_live = false;
            } else if (line_buf[0] != '#' && line_buf[0] != '\0') {
                // This is a URL
                if (is_master_playlist && variant_url[0] == '\0') {
                    // Save first variant URL
                    resolve_url(radio.hls.base_url, line_buf, variant_url, HLS_MAX_URL_LEN);
                } else if (!is_master_playlist) {
                    // Media segment
                    resolve_url(radio.hls.base_url, line_buf,
                               radio.hls.segments[radio.hls.segment_count].url, HLS_MAX_URL_LEN);
                    radio.hls.segments[radio.hls.segment_count].duration = segment_duration;
                    strncpy(radio.hls.segments[radio.hls.segment_count].title, segment_title, 255);
                    strncpy(radio.hls.segments[radio.hls.segment_count].artist, segment_artist, 255);
                    radio.hls.segment_count++;
                    segment_duration = 0;
                    segment_title[0] = '\0';
                    segment_artist[0] = '\0';
                }
            }
        }

        // Move to next line
        line = eol;
        while (*line == '\n' || *line == '\r') line++;
    }

    // If master playlist, fetch the variant playlist
    if (is_master_playlist && variant_url[0]) {
        uint8_t* playlist_buf = malloc(64 * 1024);
        if (playlist_buf) {
            int len = fetch_url_content(variant_url, playlist_buf, 64 * 1024, NULL, 0);
            if (len > 0) {
                playlist_buf[len] = '\0';
                // Update base URL for variant
                get_base_url(variant_url, radio.hls.base_url, HLS_MAX_URL_LEN);
                parse_m3u8_playlist((char*)playlist_buf, radio.hls.base_url);
            }
            free(playlist_buf);
        }
    }

    return radio.hls.segment_count;
}

// ============== MPEG-TS DEMUXER ==============

#define TS_PACKET_SIZE 188
#define TS_SYNC_BYTE 0x47
#define TS_PAT_PID 0x0000

// Extract AAC audio from MPEG-TS data
// Returns number of AAC bytes extracted
static int demux_ts_to_aac(const uint8_t* ts_data, int ts_len, uint8_t* aac_out, int aac_out_size) {
    int aac_pos = 0;
    int pmt_pid = -1;
    int audio_pid = -1;

    // Use cached audio PID if available
    if (radio.ts_pid_detected) {
        audio_pid = radio.ts_aac_pid;
    }

    int pos = 0;
    while (pos + TS_PACKET_SIZE <= ts_len && aac_pos < aac_out_size - 1024) {
        // Find sync byte
        while (pos < ts_len && ts_data[pos] != TS_SYNC_BYTE) {
            pos++;
        }

        if (pos + TS_PACKET_SIZE > ts_len) break;

        const uint8_t* pkt = &ts_data[pos];

        // Parse TS header
        int pid = ((pkt[1] & 0x1F) << 8) | pkt[2];
        int payload_start = (pkt[1] & 0x40) != 0;
        int adaptation_field = (pkt[3] >> 4) & 0x03;

        int header_len = 4;
        if (adaptation_field == 2 || adaptation_field == 3) {
            header_len += 1 + pkt[4];  // Skip adaptation field
        }

        if (adaptation_field == 1 || adaptation_field == 3) {
            // Has payload
            const uint8_t* payload = pkt + header_len;
            int payload_len = TS_PACKET_SIZE - header_len;

            if (pid == TS_PAT_PID && payload_start && !radio.ts_pid_detected) {
                // Parse PAT to find PMT PID
                int section_start = payload[0] + 1;  // Skip pointer field
                if (section_start + 8 < payload_len) {
                    const uint8_t* pat = payload + section_start;
                    // Skip PAT header (8 bytes), look at first program entry
                    if (pat[0] == 0x00) {  // table_id for PAT
                        int section_len = ((pat[1] & 0x0F) << 8) | pat[2];
                        if (section_len >= 9) {
                            // First program: bytes 8-11 (skip header)
                            pmt_pid = ((pat[10] & 0x1F) << 8) | pat[11];
                        }
                    }
                }
            } else if (pmt_pid > 0 && pid == pmt_pid && payload_start && !radio.ts_pid_detected) {
                // Parse PMT to find audio stream PID
                int section_start = payload[0] + 1;
                if (section_start + 12 < payload_len) {
                    const uint8_t* pmt = payload + section_start;
                    if (pmt[0] == 0x02) {  // table_id for PMT
                        int section_len = ((pmt[1] & 0x0F) << 8) | pmt[2];
                        int pcr_pid = ((pmt[8] & 0x1F) << 8) | pmt[9];
                        (void)pcr_pid;
                        int prog_info_len = ((pmt[10] & 0x0F) << 8) | pmt[11];

                        int es_pos = 12 + prog_info_len;
                        while (es_pos + 5 <= section_len + 3 - 4) {  // -4 for CRC
                            int stream_type = pmt[es_pos];
                            int es_pid = ((pmt[es_pos + 1] & 0x1F) << 8) | pmt[es_pos + 2];
                            int es_info_len = ((pmt[es_pos + 3] & 0x0F) << 8) | pmt[es_pos + 4];

                            // AAC stream types: 0x0F (ADTS), 0x11 (LATM)
                            if (stream_type == 0x0F || stream_type == 0x11) {
                                audio_pid = es_pid;
                                radio.ts_aac_pid = audio_pid;
                                radio.ts_pid_detected = true;
                                break;
                            }
                            // Also check for MP3 (0x03, 0x04)
                            if (stream_type == 0x03 || stream_type == 0x04) {
                                audio_pid = es_pid;
                                radio.ts_aac_pid = audio_pid;
                                radio.ts_pid_detected = true;
                                break;
                            }

                            es_pos += 5 + es_info_len;
                        }
                    }
                }
            } else if (audio_pid > 0 && pid == audio_pid) {
                // Extract audio data from PES packet
                const uint8_t* pes = payload;
                int pes_len = payload_len;

                if (payload_start) {
                    // Check PES start code
                    if (pes[0] == 0x00 && pes[1] == 0x00 && pes[2] == 0x01) {
                        // Parse PES header
                        int pes_header_len = 9 + pes[8];  // 9 bytes basic + optional header
                        if (pes_header_len < pes_len) {
                            int audio_len = pes_len - pes_header_len;
                            if (aac_pos + audio_len < aac_out_size) {
                                memcpy(aac_out + aac_pos, pes + pes_header_len, audio_len);
                                aac_pos += audio_len;
                            }
                        }
                    }
                } else {
                    // Continuation of PES packet - raw audio data
                    if (aac_pos + pes_len < aac_out_size) {
                        memcpy(aac_out + aac_pos, pes, pes_len);
                        aac_pos += pes_len;
                    }
                }
            }
        }

        pos += TS_PACKET_SIZE;
    }

    return aac_pos;
}

// HLS streaming thread
static void* hls_stream_thread_func(void* arg) {
    (void)arg;

    // Use pre-allocated buffers from RadioContext to reduce memory fragmentation
    uint8_t* segment_buf = radio.hls_segment_buf;
    uint8_t* aac_buf = radio.hls_aac_buf;

    if (!segment_buf || !aac_buf) {
        radio.state = RADIO_STATE_ERROR;
        snprintf(radio.error_msg, sizeof(radio.error_msg), "HLS buffers not allocated");
        return NULL;
    }

    // Initialize AAC decoder
    radio.aac_decoder = AACInitDecoder();
    if (!radio.aac_decoder) {
        radio.state = RADIO_STATE_ERROR;
        snprintf(radio.error_msg, sizeof(radio.error_msg), "AAC decoder init failed");
        return NULL;
    }
    radio.aac_initialized = true;
    radio.aac_inbuf_size = 0;
    radio.aac_sample_rate = 0;  // Will be set on first frame

    radio.state = RADIO_STATE_BUFFERING;

    int loop_iteration = 0;
    while (!radio.should_stop) {
        loop_iteration++;

        // Check if we need to refresh the playlist (for live streams)
        if (radio.hls.is_live && radio.hls.current_segment >= radio.hls.segment_count) {
            // Fetch updated playlist
            uint8_t* playlist_buf = malloc(64 * 1024);
            if (playlist_buf) {
                int len = fetch_url_content(radio.current_url, playlist_buf, 64 * 1024, NULL, 0);
                if (len > 0) {
                    playlist_buf[len] = '\0';
                    char base_url[HLS_MAX_URL_LEN];
                    get_base_url(radio.current_url, base_url, HLS_MAX_URL_LEN);

                    parse_m3u8_playlist((char*)playlist_buf, base_url);

                    // Skip segments we've already played based on last_played_sequence
                    // media_sequence is the sequence number of the first segment in the new playlist
                    // We want to start at last_played_sequence + 1
                    if (radio.hls.last_played_sequence >= 0) {
                        int next_seq = radio.hls.last_played_sequence + 1;
                        int start_idx = next_seq - radio.hls.media_sequence;
                        if (start_idx < 0) start_idx = 0;
                        if (start_idx > radio.hls.segment_count) start_idx = radio.hls.segment_count;
                        radio.hls.current_segment = start_idx;
                    } else {
                        radio.hls.current_segment = 0;
                    }
                }
                free(playlist_buf);
            }

            // If still no segments, wait a bit
            if (radio.hls.current_segment >= radio.hls.segment_count) {
                usleep(100000);  // 100ms
                continue;
            }
        }

        // Get next segment
        if (radio.hls.current_segment >= radio.hls.segment_count) {
            if (!radio.hls.is_live) {
                // End of stream
                break;
            }
            usleep(100000);
            continue;
        }

        // Wait if buffer is getting full to prevent memory pressure
        if (radio.audio_ring_count > AUDIO_RING_SIZE * 3 / 4) {
            int wait_count = 0;
            while (radio.audio_ring_count > AUDIO_RING_SIZE / 2 && !radio.should_stop) {
                usleep(100000);  // 100ms - wait until buffer drops to 50%
                wait_count++;
                if (wait_count % 50 == 0) {  // Log every 5 seconds
                }
            }
        }
        if (radio.should_stop) break;

        // Validate segment index
        if (radio.hls.current_segment < 0 || radio.hls.current_segment >= HLS_MAX_SEGMENTS) {
            LOG_error("[HLS] Invalid segment index: %d\n", radio.hls.current_segment);
            break;
        }

        const char* seg_url = radio.hls.segments[radio.hls.current_segment].url;
        const char* seg_title = radio.hls.segments[radio.hls.current_segment].title;
        const char* seg_artist = radio.hls.segments[radio.hls.current_segment].artist;

        // Update metadata from EXTINF if available (non-empty)
        if (seg_title && seg_title[0] != '\0') {
            strncpy(radio.metadata.title, seg_title, sizeof(radio.metadata.title) - 1);
        }
        if (seg_artist && seg_artist[0] != '\0' && strcmp(seg_artist, " ") != 0) {
            strncpy(radio.metadata.artist, seg_artist, sizeof(radio.metadata.artist) - 1);
        }

        // Validate URL
        if (!seg_url || seg_url[0] == '\0') {
            LOG_error("[HLS] Empty segment URL at index %d\n", radio.hls.current_segment);
            radio.hls.current_segment++;
            continue;
        }

        // Fetch segment
        int seg_len = fetch_url_content(seg_url, segment_buf, HLS_SEGMENT_BUF_SIZE, NULL, 0);
        if (seg_len <= 0) {
            LOG_error("[HLS] Failed to fetch segment: %s\n", seg_url);
            radio.hls.current_segment++;
            continue;
        }

        // Calculate and update bitrate from segment size and duration
        float seg_duration = radio.hls.segments[radio.hls.current_segment].duration;
        if (seg_duration > 0) {
            int bitrate = (int)((seg_len * 8.0f) / (seg_duration * 1000.0f));
            if (bitrate > 0 && bitrate < 1000) {  // Sanity check (0-1000 kbps)
                radio.metadata.bitrate = bitrate;
            }
        }

        // Check for ID3 metadata at start of segment (common in HLS radio streams)
        int id3_skip = parse_hls_id3_metadata(segment_buf, seg_len);
        if (id3_skip > 0) {
            // Adjust buffer to skip ID3 tag
            seg_len -= id3_skip;
            memmove(segment_buf, segment_buf + id3_skip, seg_len);
        }

        // Check if segment is MPEG-TS (starts with 0x47) or raw AAC (starts with 0xFF for ADTS)
        int aac_len = 0;
        if (seg_len > 0 && segment_buf[0] == TS_SYNC_BYTE) {
            // MPEG-TS container - demux to get AAC
            aac_len = demux_ts_to_aac(segment_buf, seg_len, aac_buf, HLS_SEGMENT_BUF_SIZE);
        } else {
            // Raw AAC/ADTS - use directly
            aac_len = seg_len;
            memcpy(aac_buf, segment_buf, seg_len);
        }

        // Decode AAC - process entire segment
        if (aac_len > 0) {
            int frames_decoded = 0;
            int aac_pos = 0;  // Current position in aac_buf

            // Process all AAC data from segment
            while (aac_pos < aac_len && !radio.should_stop) {
                // Find sync word in remaining data
                int sync_offset = AACFindSyncWord(aac_buf + aac_pos, aac_len - aac_pos);
                if (sync_offset < 0) {
                    break;  // No more sync words
                }
                aac_pos += sync_offset;

                // Decode frame - buffer on stack (4KB is fine with 1MB thread stack)
                int16_t decode_buf[AAC_MAX_NSAMPS * AAC_MAX_NCHANS * 2];  // Extra room for HE-AAC
                unsigned char* inptr = aac_buf + aac_pos;
                int bytes_left = aac_len - aac_pos;

                int err = AACDecode(radio.aac_decoder, &inptr, &bytes_left, decode_buf);

                if (err == ERR_AAC_NONE) {
                    AACFrameInfo frame_info;
                    AACGetLastFrameInfo(radio.aac_decoder, &frame_info);

                    // Update sample rate/channels on first successful decode
                    if (radio.aac_sample_rate == 0 && frame_info.sampRateOut > 0) {
                        radio.aac_sample_rate = frame_info.sampRateOut;
                        radio.aac_channels = frame_info.nChans;
                        // Reconfigure audio device to match stream's sample rate
                        Player_setSampleRate(frame_info.sampRateOut);
                        Player_resumeAudio();  // Resume after reconfiguration
                    }

                    // Update position based on consumed bytes
                    int consumed = (aac_len - aac_pos) - bytes_left;
                    aac_pos += consumed;

                    if (frame_info.outputSamps > 0) {
                        frames_decoded++;

                        pthread_mutex_lock(&radio.audio_mutex);

                        int samples = frame_info.outputSamps;
                        for (int s = 0; s < samples; s++) {
                            if (radio.audio_ring_count < AUDIO_RING_SIZE) {
                                radio.audio_ring[radio.audio_ring_write] = decode_buf[s];
                                radio.audio_ring_write = (radio.audio_ring_write + 1) % AUDIO_RING_SIZE;
                                radio.audio_ring_count++;
                            }
                        }

                        pthread_mutex_unlock(&radio.audio_mutex);
                    }
                } else if (err == ERR_AAC_INDATA_UNDERFLOW) {
                    break;
                } else {
                    // Skip one byte and try again
                    aac_pos++;
                }
            }
        }

        // Update state based on buffer level (use lower threshold for HLS - 0.5 second of audio)
        if (radio.state == RADIO_STATE_BUFFERING &&
            radio.audio_ring_count > SAMPLE_RATE) {  // 0.5 second of stereo audio (reduced for faster start)
            radio.state = RADIO_STATE_PLAYING;
        }

        // Track the sequence number of the segment we just played (before incrementing)
        radio.hls.last_played_sequence = radio.hls.media_sequence + radio.hls.current_segment;

        radio.hls.current_segment++;
    }


    // Note: segment_buf and aac_buf are pre-allocated in RadioContext, not freed here

    return NULL;
}

// Find MP3 sync word in buffer
// Returns offset to sync word, or -1 if not found
static int find_mp3_sync(const uint8_t* buf, int size) {
    for (int i = 0; i < size - 1; i++) {
        // MP3 sync: 0xFF followed by 0xE0-0xFF (11 bits set)
        if (buf[i] == 0xFF && (buf[i + 1] & 0xE0) == 0xE0) {
            return i;
        }
    }
    return -1;
}

// Streaming thread
static void* stream_thread_func(void* arg) {
    (void)arg;  // Unused
    uint8_t recv_buf[8192];

    while (!radio.should_stop && radio.socket_fd >= 0) {
        bool has_data = false;

        // For SSL, check if there's pending data in the SSL buffer first
        if (radio.use_ssl && mbedtls_ssl_get_bytes_avail(&radio.ssl) > 0) {
            has_data = true;
        }

        // If no pending SSL data, use select to wait for socket data
        if (!has_data) {
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(radio.socket_fd, &read_fds);

            struct timeval tv = {0, 100000};  // 100ms timeout
            int ret = select(radio.socket_fd + 1, &read_fds, NULL, NULL, &tv);

            if (ret < 0) {
                radio.state = RADIO_STATE_ERROR;
                snprintf(radio.error_msg, sizeof(radio.error_msg), "Select error");
                break;
            }

            if (ret == 0) continue;  // Timeout, check should_stop
            has_data = true;
        }

        // Receive data
        int bytes_read = radio_recv(recv_buf, sizeof(recv_buf));
        if (bytes_read <= 0) {
            // For SSL, check if it's a non-fatal error
            if (radio.use_ssl && (bytes_read == MBEDTLS_ERR_SSL_WANT_READ ||
                                   bytes_read == MBEDTLS_ERR_SSL_WANT_WRITE)) {
                continue;  // Retry
            }
            radio.state = RADIO_STATE_ERROR;
            snprintf(radio.error_msg, sizeof(radio.error_msg), "Stream ended");
            break;
        }

        // Process received data
        int i = 0;
        while (i < bytes_read && !radio.should_stop) {
            if (radio.icy_metaint > 0 && radio.bytes_until_meta == 0) {
                // Read metadata length byte
                int meta_len = recv_buf[i++] * 16;
                if (meta_len > 0 && i + meta_len <= bytes_read) {
                    parse_icy_metadata(&recv_buf[i], meta_len);
                    i += meta_len;
                }
                radio.bytes_until_meta = radio.icy_metaint;
            } else {
                // Calculate how many bytes to copy
                int bytes_to_copy = bytes_read - i;
                if (radio.icy_metaint > 0 && bytes_to_copy > radio.bytes_until_meta) {
                    bytes_to_copy = radio.bytes_until_meta;
                }

                // Add to stream buffer if space available
                if (radio.stream_buffer_pos + bytes_to_copy <= radio.stream_buffer_size) {
                    memcpy(radio.stream_buffer + radio.stream_buffer_pos, &recv_buf[i], bytes_to_copy);
                    radio.stream_buffer_pos += bytes_to_copy;
                }

                i += bytes_to_copy;
                if (radio.icy_metaint > 0) {
                    radio.bytes_until_meta -= bytes_to_copy;
                }
            }
        }

        // Initialize decoder once we have enough data
        if (radio.stream_buffer_pos >= 16384) {
            if (radio.audio_format == RADIO_FORMAT_AAC && !radio.aac_initialized) {
                // Initialize AAC decoder
                radio.aac_decoder = AACInitDecoder();
                if (radio.aac_decoder) {
                    radio.aac_initialized = true;
                    radio.aac_inbuf_size = 0;
                    radio.aac_sample_rate = 0;  // Will be set on first frame
                    radio.state = RADIO_STATE_BUFFERING;
                } else {
                    LOG_error("AAC decoder init failed\n");
                }
            } else if (radio.audio_format == RADIO_FORMAT_MP3 && !radio.mp3_initialized) {
                // Initialize low-level MP3 decoder for streaming

                // Find MP3 sync word first
                int sync_offset = find_mp3_sync(radio.stream_buffer, radio.stream_buffer_pos);
                if (sync_offset >= 0) {
                    // Skip to sync word
                    if (sync_offset > 0) {
                        memmove(radio.stream_buffer, radio.stream_buffer + sync_offset,
                                radio.stream_buffer_pos - sync_offset);
                        radio.stream_buffer_pos -= sync_offset;
                    }

                    // Initialize low-level decoder
                    drmp3dec_init(&radio.mp3_decoder);
                    radio.mp3_initialized = true;
                    radio.mp3_sample_rate = 0;  // Will be set on first frame
                    radio.mp3_channels = 0;
                    radio.state = RADIO_STATE_BUFFERING;
                } else {
                    LOG_error("No MP3 sync found in buffer\n");
                }
            }
        }

        // Decode audio based on format
        if (radio.audio_format == RADIO_FORMAT_AAC && radio.aac_initialized && radio.stream_buffer_pos >= 4096) {
            // AAC decoding
            // Copy data to AAC input buffer
            int copy_size = radio.stream_buffer_pos;
            if (radio.aac_inbuf_size + copy_size > sizeof(radio.aac_inbuf)) {
                copy_size = sizeof(radio.aac_inbuf) - radio.aac_inbuf_size;
            }
            if (copy_size > 0) {
                memcpy(radio.aac_inbuf + radio.aac_inbuf_size, radio.stream_buffer, copy_size);
                radio.aac_inbuf_size += copy_size;
                // Shift stream buffer
                memmove(radio.stream_buffer, radio.stream_buffer + copy_size, radio.stream_buffer_pos - copy_size);
                radio.stream_buffer_pos -= copy_size;
            }

            // Find sync and decode AAC frames
            while (radio.aac_inbuf_size >= AAC_MAINBUF_SIZE) {
                int sync_offset = AACFindSyncWord(radio.aac_inbuf, radio.aac_inbuf_size);
                if (sync_offset < 0) {
                    // No sync found, discard data
                    radio.aac_inbuf_size = 0;
                    break;
                }

                // Skip to sync word
                if (sync_offset > 0) {
                    memmove(radio.aac_inbuf, radio.aac_inbuf + sync_offset, radio.aac_inbuf_size - sync_offset);
                    radio.aac_inbuf_size -= sync_offset;
                }

                // Decode frame - extra room for HE-AAC (can output 4096 samples)
                int16_t decode_buf[AAC_MAX_NSAMPS * AAC_MAX_NCHANS * 2];
                unsigned char* inptr = radio.aac_inbuf;
                int bytes_left = radio.aac_inbuf_size;

                int err = AACDecode(radio.aac_decoder, &inptr, &bytes_left, decode_buf);

                if (err == ERR_AAC_NONE) {
                    AACFrameInfo frame_info;
                    AACGetLastFrameInfo(radio.aac_decoder, &frame_info);

                    // Update sample rate/channels on first successful decode
                    if (radio.aac_sample_rate == 0 && frame_info.sampRateOut > 0) {
                        radio.aac_sample_rate = frame_info.sampRateOut;
                        radio.aac_channels = frame_info.nChans;
                        // Reconfigure audio device to match stream's sample rate
                        Player_setSampleRate(frame_info.sampRateOut);
                        Player_resumeAudio();  // Resume after reconfiguration
                    }

                    // Consume the decoded data
                    int consumed = radio.aac_inbuf_size - bytes_left;
                    memmove(radio.aac_inbuf, radio.aac_inbuf + consumed, bytes_left);
                    radio.aac_inbuf_size = bytes_left;

                    if (frame_info.outputSamps > 0) {
                        pthread_mutex_lock(&radio.audio_mutex);

                        // Add to ring buffer (handle mono/stereo)
                        int samples = frame_info.outputSamps;
                        for (int s = 0; s < samples; s++) {
                            if (radio.audio_ring_count < AUDIO_RING_SIZE) {
                                radio.audio_ring[radio.audio_ring_write] = decode_buf[s];
                                radio.audio_ring_write = (radio.audio_ring_write + 1) % AUDIO_RING_SIZE;
                                radio.audio_ring_count++;
                            }
                        }

                        pthread_mutex_unlock(&radio.audio_mutex);
                    }
                } else if (err == ERR_AAC_INDATA_UNDERFLOW) {
                    // Need more data
                    break;
                } else {
                    // Error, skip a byte and try again
                    memmove(radio.aac_inbuf, radio.aac_inbuf + 1, radio.aac_inbuf_size - 1);
                    radio.aac_inbuf_size--;
                }
            }

            // Update state based on buffer level
            if (radio.state == RADIO_STATE_BUFFERING &&
                radio.audio_ring_count > AUDIO_RING_SIZE * 2 / 3) {
                radio.state = RADIO_STATE_PLAYING;
            }
        } else if (radio.audio_format == RADIO_FORMAT_MP3 && radio.mp3_initialized && radio.stream_buffer_pos >= 1024) {
            // MP3 decoding using low-level frame decoder
            // DRMP3_MAX_SAMPLES_PER_FRAME = 1152*2 = 2304
            int16_t decode_buf[2304 * 2];  // Stereo samples
            drmp3dec_frame_info frame_info;

            // Decode frames while we have data
            while (radio.stream_buffer_pos >= 512) {
                // Find sync word
                int sync_offset = find_mp3_sync(radio.stream_buffer, radio.stream_buffer_pos);
                if (sync_offset < 0) {
                    // No sync found, keep last few bytes in case sync spans buffer boundary
                    if (radio.stream_buffer_pos > 4) {
                        memmove(radio.stream_buffer, radio.stream_buffer + radio.stream_buffer_pos - 4, 4);
                        radio.stream_buffer_pos = 4;
                    }
                    break;
                }

                // Skip to sync
                if (sync_offset > 0) {
                    memmove(radio.stream_buffer, radio.stream_buffer + sync_offset,
                            radio.stream_buffer_pos - sync_offset);
                    radio.stream_buffer_pos -= sync_offset;
                }

                // Decode frame
                int samples = drmp3dec_decode_frame(&radio.mp3_decoder,
                                                     radio.stream_buffer,
                                                     radio.stream_buffer_pos,
                                                     decode_buf,
                                                     &frame_info);

                if (samples > 0 && frame_info.frame_bytes > 0) {
                    // Update sample rate/channels on first successful decode
                    if (radio.mp3_sample_rate == 0) {
                        radio.mp3_sample_rate = frame_info.sample_rate;
                        radio.mp3_channels = frame_info.channels;
                        // Reconfigure audio device to match stream's sample rate
                        Player_setSampleRate(frame_info.sample_rate);
                        Player_resumeAudio();  // Resume after reconfiguration
                    }

                    // Consume the frame
                    memmove(radio.stream_buffer, radio.stream_buffer + frame_info.frame_bytes,
                            radio.stream_buffer_pos - frame_info.frame_bytes);
                    radio.stream_buffer_pos -= frame_info.frame_bytes;

                    // Add decoded samples to ring buffer
                    pthread_mutex_lock(&radio.audio_mutex);

                    int total_samples = samples * frame_info.channels;
                    for (int s = 0; s < total_samples; s++) {
                        if (radio.audio_ring_count < AUDIO_RING_SIZE) {
                            radio.audio_ring[radio.audio_ring_write] = decode_buf[s];
                            radio.audio_ring_write = (radio.audio_ring_write + 1) % AUDIO_RING_SIZE;
                            radio.audio_ring_count++;
                        }
                    }

                    pthread_mutex_unlock(&radio.audio_mutex);
                } else if (frame_info.frame_bytes > 0) {
                    // Invalid frame, skip it
                    memmove(radio.stream_buffer, radio.stream_buffer + frame_info.frame_bytes,
                            radio.stream_buffer_pos - frame_info.frame_bytes);
                    radio.stream_buffer_pos -= frame_info.frame_bytes;
                } else {
                    // Need more data
                    break;
                }
            }

            // Update state based on buffer level
            if (radio.state == RADIO_STATE_BUFFERING &&
                radio.audio_ring_count > AUDIO_RING_SIZE * 2 / 3) {
                radio.state = RADIO_STATE_PLAYING;
            }
        }

        // If buffering and have enough data
        if (radio.state == RADIO_STATE_CONNECTING && radio.stream_buffer_pos > 0) {
            radio.state = RADIO_STATE_BUFFERING;
        }
    }

    return NULL;
}

int Radio_init(void) {
    memset(&radio, 0, sizeof(RadioContext));

    radio.socket_fd = -1;
    radio.state = RADIO_STATE_STOPPED;

    pthread_mutex_init(&radio.audio_mutex, NULL);

    // Allocate buffers
    radio.stream_buffer_size = RADIO_BUFFER_SIZE;
    radio.stream_buffer = malloc(radio.stream_buffer_size);
    radio.audio_ring = malloc(AUDIO_RING_SIZE * sizeof(int16_t));

    // Pre-allocate HLS buffers to reduce memory fragmentation
    radio.hls_segment_buf = malloc(HLS_SEGMENT_BUF_SIZE);
    radio.hls_aac_buf = malloc(HLS_SEGMENT_BUF_SIZE);

    if (!radio.stream_buffer || !radio.audio_ring ||
        !radio.hls_segment_buf || !radio.hls_aac_buf) {
        LOG_error("Radio_init: Failed to allocate buffers\n");
        Radio_quit();
        return -1;
    }

    // Load default stations
    radio.station_count = sizeof(default_stations) / sizeof(default_stations[0]);
    memcpy(radio.stations, default_stations, sizeof(default_stations));

    // Try to load custom stations
    Radio_loadStations();

    // Load curated stations from JSON files
    load_curated_stations();

    return 0;
}

void Radio_quit(void) {
    Radio_stop();

    pthread_mutex_destroy(&radio.audio_mutex);

    if (radio.stream_buffer) {
        free(radio.stream_buffer);
        radio.stream_buffer = NULL;
    }
    if (radio.audio_ring) {
        free(radio.audio_ring);
        radio.audio_ring = NULL;
    }
    if (radio.hls_segment_buf) {
        free(radio.hls_segment_buf);
        radio.hls_segment_buf = NULL;
    }
    if (radio.hls_aac_buf) {
        free(radio.hls_aac_buf);
        radio.hls_aac_buf = NULL;
    }
}

int Radio_getStations(RadioStation** stations) {
    *stations = radio.stations;
    return radio.station_count;
}

int Radio_addStation(const char* name, const char* url, const char* genre, const char* slogan) {
    if (radio.station_count >= RADIO_MAX_STATIONS) return -1;

    RadioStation* s = &radio.stations[radio.station_count];
    strncpy(s->name, name, RADIO_MAX_NAME - 1);
    strncpy(s->url, url, RADIO_MAX_URL - 1);
    strncpy(s->genre, genre ? genre : "", 63);
    strncpy(s->slogan, slogan ? slogan : "", 127);
    radio.station_count++;

    return radio.station_count - 1;
}

void Radio_removeStation(int index) {
    if (index < 0 || index >= radio.station_count) return;

    memmove(&radio.stations[index], &radio.stations[index + 1],
            (radio.station_count - index - 1) * sizeof(RadioStation));
    radio.station_count--;
}

void Radio_saveStations(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/radio_stations.txt", SHARED_USERDATA_PATH);

    FILE* f = fopen(path, "w");
    if (!f) return;

    for (int i = 0; i < radio.station_count; i++) {
        fprintf(f, "%s|%s|%s|%s\n",
                radio.stations[i].name,
                radio.stations[i].url,
                radio.stations[i].genre,
                radio.stations[i].slogan);
    }

    fclose(f);
}

void Radio_loadStations(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/radio_stations.txt", SHARED_USERDATA_PATH);

    FILE* f = fopen(path, "r");
    if (!f) return;

    radio.station_count = 0;

    char line[1024];
    while (fgets(line, sizeof(line), f) && radio.station_count < RADIO_MAX_STATIONS) {
        // Remove newline
        char* nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        // Parse: name|url|genre|slogan (slogan is optional)
        char* name = strtok(line, "|");
        char* url = strtok(NULL, "|");
        char* genre = strtok(NULL, "|");
        char* slogan = strtok(NULL, "|");

        if (name && url) {
            Radio_addStation(name, url, genre, slogan);
        }
    }

    fclose(f);
}

int Radio_play(const char* url) {
    Radio_stop();

    // Reset audio device to 48000 Hz for radio playback
    Player_resetSampleRate();

    strncpy(radio.current_url, url, RADIO_MAX_URL - 1);
    radio.state = RADIO_STATE_CONNECTING;
    radio.error_msg[0] = '\0';

    // Reset buffers
    radio.stream_buffer_pos = 0;
    radio.audio_ring_write = 0;
    radio.audio_ring_read = 0;
    radio.audio_ring_count = 0;

    memset(&radio.metadata, 0, sizeof(RadioMetadata));

    // Reset HLS state
    radio.ts_pid_detected = false;
    radio.ts_aac_pid = -1;
    memset(&radio.hls, 0, sizeof(HLSContext));

    // Check if this is an HLS stream
    if (is_hls_url(url)) {
        radio.stream_type = STREAM_TYPE_HLS;

        // Fetch and parse the M3U8 playlist
        uint8_t* playlist_buf = malloc(64 * 1024);
        if (!playlist_buf) {
            radio.state = RADIO_STATE_ERROR;
            snprintf(radio.error_msg, sizeof(radio.error_msg), "Memory allocation failed");
            return -1;
        }

        int len = fetch_url_content(url, playlist_buf, 64 * 1024, NULL, 0);
        if (len <= 0) {
            free(playlist_buf);
            radio.state = RADIO_STATE_ERROR;
            snprintf(radio.error_msg, sizeof(radio.error_msg), "Failed to fetch playlist");
            return -1;
        }

        playlist_buf[len] = '\0';

        // Get base URL for resolving relative paths
        char base_url[HLS_MAX_URL_LEN];
        get_base_url(url, base_url, HLS_MAX_URL_LEN);

        // Initialize segment tracking for new stream
        radio.hls.current_segment = 0;
        radio.hls.last_played_sequence = -1;

        int seg_count = parse_m3u8_playlist((char*)playlist_buf, base_url);
        free(playlist_buf);

        if (seg_count > 0) {
        }

        if (seg_count <= 0) {
            radio.state = RADIO_STATE_ERROR;
            snprintf(radio.error_msg, sizeof(radio.error_msg), "No segments in playlist");
            return -1;
        }

        // Start HLS streaming thread with larger stack (mbedtls + getaddrinfo need more stack space)
        radio.should_stop = false;
        radio.thread_running = true;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        size_t stacksize = 1024 * 1024;  // 1MB stack - getaddrinfo uses a lot of stack
        int stack_result = pthread_attr_setstacksize(&attr, stacksize);
        if (pthread_create(&radio.stream_thread, &attr, hls_stream_thread_func, NULL) != 0) {
            pthread_attr_destroy(&attr);
            radio.state = RADIO_STATE_ERROR;
            snprintf(radio.error_msg, sizeof(radio.error_msg), "Thread creation failed");
            return -1;
        }
        pthread_attr_destroy(&attr);

        // Unpause audio device for radio playback
        Player_resumeAudio();

        return 0;
    }

    // Direct stream (Shoutcast/Icecast)
    radio.stream_type = STREAM_TYPE_DIRECT;

    // Connect with redirect handling (max 5 redirects)
    char current_url[RADIO_MAX_URL];
    strncpy(current_url, url, RADIO_MAX_URL - 1);
    current_url[RADIO_MAX_URL - 1] = '\0';

    int max_redirects = 5;
    int header_result;

    for (int redirect_count = 0; redirect_count <= max_redirects; redirect_count++) {
        // Connect to stream
        if (connect_stream(current_url) != 0) {
            radio.state = RADIO_STATE_ERROR;
            return -1;
        }

        // Parse headers
        header_result = parse_headers();

        if (header_result == 0) {
            // Success - headers parsed, ready to stream
            break;
        } else if (header_result == 1) {
            // Redirect - cleanup current connection and try new URL
            if (radio.use_ssl) {
                ssl_cleanup();
                radio.use_ssl = false;
            } else {
                close(radio.socket_fd);
            }
            radio.socket_fd = -1;

            if (radio.redirect_url[0] == '\0') {
                snprintf(radio.error_msg, sizeof(radio.error_msg), "Empty redirect URL");
                radio.state = RADIO_STATE_ERROR;
                return -1;
            }

            strncpy(current_url, radio.redirect_url, RADIO_MAX_URL - 1);
            current_url[RADIO_MAX_URL - 1] = '\0';

            if (redirect_count == max_redirects) {
                snprintf(radio.error_msg, sizeof(radio.error_msg), "Too many redirects");
                radio.state = RADIO_STATE_ERROR;
                return -1;
            }
        } else {
            // Error - cleanup connection
            if (radio.use_ssl) {
                ssl_cleanup();
                radio.use_ssl = false;
            } else {
                close(radio.socket_fd);
            }
            radio.socket_fd = -1;
            radio.state = RADIO_STATE_ERROR;
            return -1;
        }
    }

    // Start streaming thread
    radio.should_stop = false;
    radio.thread_running = true;
    if (pthread_create(&radio.stream_thread, NULL, stream_thread_func, NULL) != 0) {
        if (radio.use_ssl) {
            ssl_cleanup();
            radio.use_ssl = false;
        } else {
            close(radio.socket_fd);
        }
        radio.socket_fd = -1;
        radio.state = RADIO_STATE_ERROR;
        snprintf(radio.error_msg, sizeof(radio.error_msg), "Thread creation failed");
        return -1;
    }

    // Unpause audio device for radio playback
    Player_resumeAudio();

    return 0;
}

void Radio_stop(void) {
    radio.should_stop = true;

    if (radio.thread_running) {
        pthread_join(radio.stream_thread, NULL);
        radio.thread_running = false;
    }

    // Cleanup SSL if active
    if (radio.use_ssl) {
        ssl_cleanup();
        radio.use_ssl = false;
    } else if (radio.socket_fd >= 0) {
        close(radio.socket_fd);
    }
    radio.socket_fd = -1;

    if (radio.mp3_initialized) {
        // Low-level drmp3dec doesn't need uninit
        radio.mp3_initialized = false;
        radio.mp3_sample_rate = 0;
        radio.mp3_channels = 0;
    }

    if (radio.aac_initialized) {
        AACFreeDecoder(radio.aac_decoder);
        radio.aac_decoder = NULL;
        radio.aac_initialized = false;
        radio.aac_inbuf_size = 0;
        radio.aac_sample_rate = 0;
        radio.aac_channels = 0;
    }

    // Reset HLS state
    radio.stream_type = STREAM_TYPE_DIRECT;
    radio.ts_pid_detected = false;
    radio.ts_aac_pid = -1;

    radio.state = RADIO_STATE_STOPPED;

    // Pause audio device when radio stops
    Player_pauseAudio();
}

RadioState Radio_getState(void) {
    return radio.state;
}

const RadioMetadata* Radio_getMetadata(void) {
    return &radio.metadata;
}

float Radio_getBufferLevel(void) {
    return (float)radio.audio_ring_count / AUDIO_RING_SIZE;
}

const char* Radio_getError(void) {
    return radio.error_msg;
}

void Radio_update(void) {
    // Check for buffer underrun
    if (radio.state == RADIO_STATE_PLAYING && radio.audio_ring_count < SAMPLE_RATE * 2) {
        radio.state = RADIO_STATE_BUFFERING;
    }
}

int Radio_getAudioSamples(int16_t* buffer, int max_samples) {
    pthread_mutex_lock(&radio.audio_mutex);

    int samples_to_read = max_samples;
    if (samples_to_read > radio.audio_ring_count) {
        samples_to_read = radio.audio_ring_count;
    }

    for (int i = 0; i < samples_to_read; i++) {
        buffer[i] = radio.audio_ring[radio.audio_ring_read];
        radio.audio_ring_read = (radio.audio_ring_read + 1) % AUDIO_RING_SIZE;
    }
    radio.audio_ring_count -= samples_to_read;

    // Fill rest with silence
    for (int i = samples_to_read; i < max_samples; i++) {
        buffer[i] = 0;
    }

    pthread_mutex_unlock(&radio.audio_mutex);

    return samples_to_read;
}

bool Radio_isActive(void) {
    return radio.state != RADIO_STATE_STOPPED && radio.state != RADIO_STATE_ERROR;
}

// Curated stations API
int Radio_getCuratedCountryCount(void) {
    return curated_country_count;
}

const CuratedCountry* Radio_getCuratedCountries(void) {
    return curated_countries;
}

int Radio_getCuratedStationCount(const char* country_code) {
    int count = 0;
    for (int i = 0; i < curated_station_count; i++) {
        if (strcmp(curated_stations[i].country_code, country_code) == 0) {
            count++;
        }
    }
    return count;
}

const CuratedStation* Radio_getCuratedStations(const char* country_code, int* count) {
    // Find the first station for this country and count total
    const CuratedStation* first = NULL;
    *count = 0;

    for (int i = 0; i < curated_station_count; i++) {
        if (strcmp(curated_stations[i].country_code, country_code) == 0) {
            if (!first) {
                first = &curated_stations[i];
            }
            (*count)++;
        }
    }

    return first;
}

bool Radio_stationExists(const char* url) {
    for (int i = 0; i < radio.station_count; i++) {
        if (strcmp(radio.stations[i].url, url) == 0) {
            return true;
        }
    }
    return false;
}

bool Radio_removeStationByUrl(const char* url) {
    for (int i = 0; i < radio.station_count; i++) {
        if (strcmp(radio.stations[i].url, url) == 0) {
            Radio_removeStation(i);
            return true;
        }
    }
    return false;
}
