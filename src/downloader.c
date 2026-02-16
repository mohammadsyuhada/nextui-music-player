#define _GNU_SOURCE
#include "downloader.h"
#include "keyboard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <ctype.h>

#include "defines.h"
#include "api.h"

// Paths
static char ytdlp_path[512] = "";
static char download_dir[512] = "";
static char queue_file[512] = "";
static char version_file[512] = "";
static char pak_path[512] = "";

// Module state
static bool youtube_initialized = false;
static DownloaderState youtube_state = DOWNLOADER_STATE_IDLE;
static char error_message[256] = "";

// Download queue
static DownloaderQueueItem download_queue[DOWNLOADER_MAX_QUEUE];
static int queue_count = 0;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;

// Download status
static DownloaderDownloadStatus download_status = {0};
static pthread_t download_thread;
static volatile bool download_running = false;
static volatile bool download_should_stop = false;

// Update status
static DownloaderUpdateStatus update_status = {0};
static pthread_t update_thread;
static volatile bool update_running = false;
static volatile bool update_should_stop = false;

// Search
static pthread_t search_thread;
static volatile bool search_running = false;
static volatile bool search_should_stop = false;
static DownloaderResult search_results[DOWNLOADER_MAX_RESULTS];
static int search_result_count = 0;
static DownloaderSearchStatus search_status = {0};
static char search_query_copy[256] = "";
static int search_max_results = DOWNLOADER_MAX_RESULTS;

// Current yt-dlp version
static char current_version[32] = "unknown";

// Forward declarations
static void* download_thread_func(void* arg);
static void* update_thread_func(void* arg);
static void* search_thread_func(void* arg);
static int run_command(const char* cmd, char* output, size_t output_size);
static void sanitize_filename(const char* input, char* output, size_t max_len);
static void clean_title(char* title);

// Clean title by removing text inside () and [] brackets
static void clean_title(char* title) {
    if (!title || !title[0]) return;

    char result[512];
    int j = 0;
    int paren_depth = 0;   // Track nested ()
    int bracket_depth = 0; // Track nested []

    for (int i = 0; title[i] && j < (int)sizeof(result) - 1; i++) {
        char c = title[i];

        if (c == '(') {
            paren_depth++;
        } else if (c == ')') {
            if (paren_depth > 0) paren_depth--;
        } else if (c == '[') {
            bracket_depth++;
        } else if (c == ']') {
            if (bracket_depth > 0) bracket_depth--;
        } else if (paren_depth == 0 && bracket_depth == 0) {
            result[j++] = c;
        }
    }
    result[j] = '\0';

    // Trim trailing spaces
    while (j > 0 && result[j-1] == ' ') {
        result[--j] = '\0';
    }

    // Trim leading spaces
    char* start = result;
    while (*start == ' ') start++;

    // Copy back to title (title buffer is at least 512 bytes from caller)
    strncpy(title, start, 511);
    title[511] = '\0';
}

int Downloader_init(void) {
    if (youtube_initialized) return 0;  // Already initialized

    // Pak directory is current working directory (launch.sh sets cwd to pak folder)
    strncpy(pak_path, ".", sizeof(pak_path) - 1);
    pak_path[sizeof(pak_path) - 1] = '\0';

    // Verify yt-dlp binary exists
    if (access("./bin/yt-dlp", F_OK) != 0) {
        LOG_error("yt-dlp binary not found\n");
        strncpy(error_message, "yt-dlp not found", sizeof(error_message) - 1);
        error_message[sizeof(error_message) - 1] = '\0';
        return -1;
    }

    // Set paths
    snprintf(ytdlp_path, sizeof(ytdlp_path), "%s/bin/yt-dlp", pak_path);
    snprintf(version_file, sizeof(version_file), "%s/state/yt-dlp_version.txt", pak_path);
    snprintf(queue_file, sizeof(queue_file), "%s/state/youtube_queue.txt", pak_path);
    snprintf(download_dir, sizeof(download_dir), "%s/Music/Downloaded", SDCARD_PATH);

    // Ensure binaries are executable
    chmod(ytdlp_path, 0755);

    // Initialize keyboard module
    Keyboard_init();

    // Create music directories if needed
    char music_dir[512];
    snprintf(music_dir, sizeof(music_dir), "%s/Music", SDCARD_PATH);
    mkdir(music_dir, 0755);
    mkdir(download_dir, 0755);

    // Load current version from version file first
    FILE* f = fopen(version_file, "r");
    if (f) {
        if (fgets(current_version, sizeof(current_version), f)) {
            // Remove newline
            char* nl = strchr(current_version, '\n');
            if (nl) *nl = '\0';
        }
        fclose(f);
    }

    // If version is still unknown, try to get it from yt-dlp --version
    if (strcmp(current_version, "unknown") == 0) {
        char cmd[600];
        snprintf(cmd, sizeof(cmd), "%s --version 2>/dev/null", ytdlp_path);
        FILE* pipe = popen(cmd, "r");
        if (pipe) {
            if (fgets(current_version, sizeof(current_version), pipe)) {
                char* nl = strchr(current_version, '\n');
                if (nl) *nl = '\0';
                // Save to version file for future
                FILE* vf = fopen(version_file, "w");
                if (vf) {
                    fprintf(vf, "%s\n", current_version);
                    fclose(vf);
                }
            }
            pclose(pipe);
        }
    }

    // Load queue from file
    Downloader_loadQueue();

    // Auto-resume pending downloads if queue has items and network is available
    if (queue_count > 0 && Downloader_checkNetwork()) {
        Downloader_downloadStart();
    }

    youtube_initialized = true;
    return 0;
}

void Downloader_cleanup(void) {
    // Stop any running operations
    Downloader_downloadStop();
    Downloader_cancelUpdate();
    Downloader_cancelSearch();

    // Wait briefly for download thread to finish
    for (int i = 0; i < 30 && download_running; i++) {
        usleep(100000);  // 100ms
    }

    // Re-enable auto sleep
    PWR_enableAutosleep();

    // Save queue (DOWNLOADING items will become PENDING on next load)
    Downloader_saveQueue();
}

bool Downloader_isAvailable(void) {
    return access(ytdlp_path, X_OK) == 0;
}

bool Downloader_checkNetwork(void) {
    // Quick connectivity check - try primary DNS first, then fallback
    int conn = system("ping -c 1 -W 2 8.8.8.8 >/dev/null 2>&1");
    if (conn != 0) {
        conn = system("ping -c 1 -W 2 1.1.1.1 >/dev/null 2>&1");
    }
    return (conn == 0);
}

const char* Downloader_getVersion(void) {
    return current_version;
}

void Downloader_cancelSearch(void) {
    search_should_stop = true;
    if (search_running) {
        // Kill any running yt-dlp search process to allow immediate re-search
        system("pkill -f 'yt-dlp.*music.youtube.com/search' 2>/dev/null");
        search_running = false;
    }
}

// Background search thread function
static void* search_thread_func(void* arg) {
    (void)arg;

    search_status.searching = true;
    search_status.completed = false;
    search_status.result_count = 0;
    search_status.error_message[0] = '\0';

    // Check connectivity first to fail fast
    int conn = system("ping -c 1 -W 2 8.8.8.8 >/dev/null 2>&1");
    if (conn != 0) {
        conn = system("ping -c 1 -W 2 1.1.1.1 >/dev/null 2>&1");
    }

    if (conn != 0) {
        strncpy(search_status.error_message, "No internet connection", sizeof(search_status.error_message) - 1);
        search_status.error_message[sizeof(search_status.error_message) - 1] = '\0';
        search_status.result_count = -1;
        search_status.searching = false;
        search_status.completed = true;
        search_running = false;
        youtube_state = DOWNLOADER_STATE_IDLE;
        return NULL;
    }

    if (search_should_stop) {
        search_status.searching = false;
        search_status.completed = true;
        search_running = false;
        youtube_state = DOWNLOADER_STATE_IDLE;
        return NULL;
    }

    // Sanitize query - escape special characters
    char safe_query[256];
    int j = 0;
    for (int i = 0; search_query_copy[i] && j < (int)sizeof(safe_query) - 2; i++) {
        char c = search_query_copy[i];
        // Skip potentially dangerous characters for shell
        if (c == '"' || c == '\'' || c == '`' || c == '$' || c == '\\' || c == ';' || c == '&' || c == '|') {
            continue;
        }
        safe_query[j++] = c;
    }
    safe_query[j] = '\0';

    int num_results = search_max_results > DOWNLOADER_MAX_RESULTS ? DOWNLOADER_MAX_RESULTS : search_max_results;

    // Use a temp file to capture results (more reliable than pipe)
    const char* temp_file = "/tmp/yt_search_results.txt";
    const char* temp_err = "/tmp/yt_search_error.txt";

    // Build yt-dlp search command
    // Note: --socket-timeout handles network-level timeouts
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "%s 'https://music.youtube.com/search?q=%s#songs' "
        "--flat-playlist "
        "-I :%d "
        "--no-warnings "
        "--socket-timeout 15 "
        "--print '%%(id)s\t%%(title)s' "
        "> %s 2> %s",
        ytdlp_path,
        safe_query,
        num_results,
        temp_file,
        temp_err);

    int ret = system(cmd);

    // Check if cancelled during search
    if (search_should_stop) {
        unlink(temp_file);
        unlink(temp_err);
        search_status.searching = false;
        search_status.completed = true;
        search_running = false;
        youtube_state = DOWNLOADER_STATE_IDLE;
        return NULL;
    }

    if (ret != 0) {
        // Try to read error message
        FILE* err = fopen(temp_err, "r");
        if (err) {
            char err_line[256];
            if (fgets(err_line, sizeof(err_line), err)) {
                // Remove newline
                char* nl = strchr(err_line, '\n');
                if (nl) *nl = '\0';
                // Check for common errors
                if (strstr(err_line, "name resolution") || strstr(err_line, "resolve")) {
                    strncpy(search_status.error_message, "Network error - check WiFi", sizeof(search_status.error_message) - 1);
                } else if (strstr(err_line, "timed out") || strstr(err_line, "timeout")) {
                    strncpy(search_status.error_message, "Connection timed out", sizeof(search_status.error_message) - 1);
                } else {
                    strncpy(search_status.error_message, "Search failed", sizeof(search_status.error_message) - 1);
                }
                search_status.error_message[sizeof(search_status.error_message) - 1] = '\0';
                LOG_error("yt-dlp error: %s\n", err_line);
            }
            fclose(err);
        }
    }

    // Read results from temp file
    FILE* f = fopen(temp_file, "r");
    if (!f) {
        if (search_status.error_message[0] == '\0') {
            strncpy(search_status.error_message, "Failed to read search results", sizeof(search_status.error_message) - 1);
            search_status.error_message[sizeof(search_status.error_message) - 1] = '\0';
        }
        search_status.result_count = -1;
        search_status.searching = false;
        search_status.completed = true;
        search_running = false;
        youtube_state = DOWNLOADER_STATE_IDLE;
        return NULL;
    }

    char line[512];
    int count = 0;

    while (fgets(line, sizeof(line), f) && count < search_max_results) {
        // Check for cancellation
        if (search_should_stop) {
            break;
        }

        // Remove newline
        char* nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        // Skip empty lines
        if (line[0] == '\0') continue;

        // Make a copy for strtok since it modifies the string
        char line_copy[512];
        strncpy(line_copy, line, sizeof(line_copy) - 1);
        line_copy[sizeof(line_copy) - 1] = '\0';

        // Parse: id<TAB>title (tab-separated)
        char* id = strtok(line_copy, "\t");
        char* title = strtok(NULL, "\t");

        if (id && title && strlen(id) > 0) {
            strncpy(search_results[count].title, title, DOWNLOADER_MAX_TITLE - 1);
            search_results[count].title[DOWNLOADER_MAX_TITLE - 1] = '\0';

            strncpy(search_results[count].video_id, id, DOWNLOADER_VIDEO_ID_LEN - 1);
            search_results[count].video_id[DOWNLOADER_VIDEO_ID_LEN - 1] = '\0';

            search_results[count].artist[0] = '\0';
            search_results[count].duration_sec = 0;

            count++;
        }
    }

    fclose(f);

    // Cleanup temp files
    unlink(temp_file);
    unlink(temp_err);

    search_status.result_count = count;
    search_status.searching = false;
    search_status.completed = true;
    search_running = false;
    youtube_state = DOWNLOADER_STATE_IDLE;

    return NULL;
}

// Start async search
int Downloader_startSearch(const char* query) {
    if (!query || search_running) {
        return -1;
    }

    // Reset status
    memset(&search_status, 0, sizeof(search_status));
    search_result_count = 0;

    // Copy query for thread
    strncpy(search_query_copy, query, sizeof(search_query_copy) - 1);
    search_query_copy[sizeof(search_query_copy) - 1] = '\0';

    search_running = true;
    search_should_stop = false;
    youtube_state = DOWNLOADER_STATE_SEARCHING;

    if (pthread_create(&search_thread, NULL, search_thread_func, NULL) != 0) {
        search_running = false;
        youtube_state = DOWNLOADER_STATE_ERROR;
        strncpy(search_status.error_message, "Failed to start search", sizeof(search_status.error_message) - 1);
        search_status.error_message[sizeof(search_status.error_message) - 1] = '\0';
        search_status.result_count = -1;
        search_status.completed = true;
        return -1;
    }

    pthread_detach(search_thread);
    return 0;
}

// Get search status
const DownloaderSearchStatus* Downloader_getSearchStatus(void) {
    return &search_status;
}

// Get search results
DownloaderResult* Downloader_getSearchResults(void) {
    return search_results;
}

int Downloader_queueAdd(const char* video_id, const char* title) {
    if (!video_id || !title) return -1;

    pthread_mutex_lock(&queue_mutex);

    // Check if already in queue
    for (int i = 0; i < queue_count; i++) {
        if (strcmp(download_queue[i].video_id, video_id) == 0) {
            pthread_mutex_unlock(&queue_mutex);
            return 0;  // Already in queue
        }
    }

    // Check queue size
    if (queue_count >= DOWNLOADER_MAX_QUEUE) {
        pthread_mutex_unlock(&queue_mutex);
        return -1;  // Queue full
    }

    // Add to queue
    strncpy(download_queue[queue_count].video_id, video_id, DOWNLOADER_VIDEO_ID_LEN - 1);
    strncpy(download_queue[queue_count].title, title, DOWNLOADER_MAX_TITLE - 1);
    download_queue[queue_count].status = DOWNLOADER_STATUS_PENDING;
    download_queue[queue_count].progress_percent = 0;
    queue_count++;

    pthread_mutex_unlock(&queue_mutex);

    // Save queue to file
    Downloader_saveQueue();

    // Auto-start download thread if not already running
    Downloader_downloadStart();

    return 1;  // Successfully added
}

int Downloader_queueRemove(int index) {
    pthread_mutex_lock(&queue_mutex);

    if (index < 0 || index >= queue_count) {
        pthread_mutex_unlock(&queue_mutex);
        return -1;
    }

    // Shift items
    for (int i = index; i < queue_count - 1; i++) {
        download_queue[i] = download_queue[i + 1];
    }
    queue_count--;

    pthread_mutex_unlock(&queue_mutex);

    Downloader_saveQueue();
    return 0;
}

int Downloader_queueRemoveById(const char* video_id) {
    if (!video_id) return -1;

    pthread_mutex_lock(&queue_mutex);

    int found_index = -1;
    for (int i = 0; i < queue_count; i++) {
        if (strcmp(download_queue[i].video_id, video_id) == 0) {
            found_index = i;
            break;
        }
    }

    if (found_index < 0) {
        pthread_mutex_unlock(&queue_mutex);
        return -1;  // Not found
    }

    // Shift items
    for (int i = found_index; i < queue_count - 1; i++) {
        download_queue[i] = download_queue[i + 1];
    }
    queue_count--;

    pthread_mutex_unlock(&queue_mutex);

    Downloader_saveQueue();
    return 0;
}

int Downloader_queueClear(void) {
    pthread_mutex_lock(&queue_mutex);
    queue_count = 0;
    pthread_mutex_unlock(&queue_mutex);

    Downloader_saveQueue();
    return 0;
}

int Downloader_queueCount(void) {
    return queue_count;
}

DownloaderQueueItem* Downloader_queueGet(int* count) {
    if (count) *count = queue_count;
    return download_queue;
}

bool Downloader_isInQueue(const char* video_id) {
    if (!video_id) return false;

    pthread_mutex_lock(&queue_mutex);
    for (int i = 0; i < queue_count; i++) {
        if (strcmp(download_queue[i].video_id, video_id) == 0) {
            pthread_mutex_unlock(&queue_mutex);
            return true;
        }
    }
    pthread_mutex_unlock(&queue_mutex);
    return false;
}

bool Downloader_isDownloaded(const char* video_id) {
    if (!video_id) return false;

    // Check if file exists in download directory
    // This is a simple check - could be improved with a database
    char pattern[600];
    snprintf(pattern, sizeof(pattern), "%s/*%s*", download_dir, video_id);

    // For now, just return false - would need glob() for proper implementation
    return false;
}

// Parse yt-dlp speed string like "1.23MiB/s" or "500KiB/s" to bytes/sec
static int parse_ytdlp_speed(const char* speed_str) {
    if (!speed_str) return 0;
    float val = 0;
    if (sscanf(speed_str, "%f", &val) != 1) return 0;
    if (strstr(speed_str, "GiB/s")) return (int)(val * 1024 * 1024 * 1024);
    if (strstr(speed_str, "MiB/s")) return (int)(val * 1024 * 1024);
    if (strstr(speed_str, "KiB/s")) return (int)(val * 1024);
    if (strstr(speed_str, "B/s"))   return (int)val;
    return 0;
}

// Parse yt-dlp ETA string like "00:03" or "01:23:45" to seconds
static int parse_ytdlp_eta(const char* eta_str) {
    if (!eta_str) return 0;
    int h = 0, m = 0, s = 0;
    // Try HH:MM:SS first
    if (sscanf(eta_str, "%d:%d:%d", &h, &m, &s) == 3) {
        return h * 3600 + m * 60 + s;
    }
    // Try MM:SS
    if (sscanf(eta_str, "%d:%d", &m, &s) == 2) {
        return m * 60 + s;
    }
    return 0;
}

static void* download_thread_func(void* arg) {
    (void)arg;

    // Disable auto sleep while downloading
    PWR_disableAutosleep();

    while (!download_should_stop) {
        pthread_mutex_lock(&queue_mutex);

        // Find next pending item
        int download_index = -1;
        for (int i = 0; i < queue_count; i++) {
            if (download_queue[i].status == DOWNLOADER_STATUS_PENDING) {
                download_index = i;
                break;
            }
        }

        if (download_index < 0) {
            pthread_mutex_unlock(&queue_mutex);
            break;  // No more items
        }

        // Mark as downloading
        download_queue[download_index].status = DOWNLOADER_STATUS_DOWNLOADING;
        char video_id[DOWNLOADER_VIDEO_ID_LEN];
        char title[DOWNLOADER_MAX_TITLE];
        strncpy(video_id, download_queue[download_index].video_id, sizeof(video_id));
        strncpy(title, download_queue[download_index].title, sizeof(title));

        pthread_mutex_unlock(&queue_mutex);

        // Update status
        download_status.current_index = download_index;
        strncpy(download_status.current_title, title, sizeof(download_status.current_title));

        // Sanitize filename
        char safe_filename[128];
        sanitize_filename(title, safe_filename, sizeof(safe_filename));

        char output_file[600];
        char temp_file[600];
        snprintf(output_file, sizeof(output_file), "%s/%s.m4a", download_dir, safe_filename);
        snprintf(temp_file, sizeof(temp_file), "%s/.downloading_%s.m4a", download_dir, video_id);

        // Check if already exists
        bool success = false;
        if (access(output_file, F_OK) == 0) {
            success = true;
        } else {
            // Build download command - download M4A directly with metadata
            // Metadata is embedded by yt-dlp (uses mutagen, no ffmpeg needed)
            // Album art will be fetched by player during playback
            // Force M4A only - no fallback to other formats
            // socket-timeout prevents network hangs
            char cmd[2048];
            snprintf(cmd, sizeof(cmd),
                "%s "
                "-f \"bestaudio[ext=m4a]\" "
                "--embed-metadata "
                "--socket-timeout 30 "
                "--parse-metadata \"title:%%(artist)s - %%(title)s\" "
                "--newline --progress "
                "-o \"%s\" "
                "--no-playlist "
                "\"https://music.youtube.com/watch?v=%s\" "
                "2>&1",
                ytdlp_path, temp_file, video_id);


            // Use popen to read progress in real-time
            FILE* pipe = popen(cmd, "r");
            int result = -1;

            if (pipe) {
                char line[512];
                while (fgets(line, sizeof(line), pipe)) {
                    // Log errors from yt-dlp
                    if (strstr(line, "ERROR") || strstr(line, "error:")) {
                        LOG_error("yt-dlp: %s", line);
                    }

                    // Parse progress from yt-dlp output
                    // Format: [download]  55.3% of ~  5.21MiB at  1.23MiB/s ETA 00:03
                    if (strstr(line, "[download]")) {
                        char* pct = strstr(line, "%");
                        if (pct) {
                            // Find the start of the percentage number
                            char* start = pct - 1;
                            while (start > line && (isdigit(*start) || *start == '.')) {
                                start--;
                            }
                            start++;

                            float percent = 0;
                            if (sscanf(start, "%f", &percent) == 1) {
                                int speed = 0;
                                int eta = 0;

                                // Parse speed: find "at" keyword then speed value
                                char* at_ptr = strstr(line, " at ");
                                if (at_ptr) {
                                    speed = parse_ytdlp_speed(at_ptr + 4);
                                }

                                // Parse ETA: find "ETA" keyword
                                char* eta_ptr = strstr(line, "ETA ");
                                if (eta_ptr) {
                                    eta = parse_ytdlp_eta(eta_ptr + 4);
                                }

                                pthread_mutex_lock(&queue_mutex);
                                if (download_index < queue_count) {
                                    // Download is ~80% of total, post-processing is ~20%
                                    download_queue[download_index].progress_percent = (int)(percent * 0.8f);
                                    download_queue[download_index].speed_bps = speed;
                                    download_queue[download_index].eta_sec = eta;
                                    download_status.speed_bps = speed;
                                    download_status.eta_sec = eta;
                                }
                                pthread_mutex_unlock(&queue_mutex);
                            }
                        }
                    }
                    // Check for post-processing progress (metadata/thumbnail embedding)
                    if (strstr(line, "[EmbedThumbnail]") || strstr(line, "Post-process")) {
                        pthread_mutex_lock(&queue_mutex);
                        if (download_index < queue_count) {
                            download_queue[download_index].progress_percent = 85;
                            download_queue[download_index].speed_bps = 0;
                            download_queue[download_index].eta_sec = 0;
                            download_status.speed_bps = 0;
                            download_status.eta_sec = 0;
                        }
                        pthread_mutex_unlock(&queue_mutex);
                    }
                    if (strstr(line, "[Metadata]") || strstr(line, "Adding metadata")) {
                        pthread_mutex_lock(&queue_mutex);
                        if (download_index < queue_count) {
                            download_queue[download_index].progress_percent = 95;
                            download_queue[download_index].speed_bps = 0;
                            download_queue[download_index].eta_sec = 0;
                        }
                        pthread_mutex_unlock(&queue_mutex);
                    }
                }
                result = pclose(pipe);
            }

            if (result == 0 && access(temp_file, F_OK) == 0) {
                // Validate M4A file before moving
                bool valid_m4a = false;
                struct stat st;
                if (stat(temp_file, &st) == 0 && st.st_size >= 10240) {
                    // Minimum 10KB for a valid M4A
                    int fd = open(temp_file, O_RDONLY);
                    if (fd >= 0) {
                        unsigned char header[12];
                        if (read(fd, header, 12) == 12) {
                            // Check for ftyp atom (MP4/M4A container)
                            // Bytes 4-7 should be "ftyp"
                            if (header[4] == 'f' && header[5] == 't' &&
                                header[6] == 'y' && header[7] == 'p') {
                                valid_m4a = true;
                            }
                        }
                        close(fd);
                    }
                }

                if (valid_m4a) {
                    // Sync file to disk before rename
                    int fd = open(temp_file, O_RDONLY);
                    if (fd >= 0) {
                        fsync(fd);
                        close(fd);
                    }
                    // Move temp to final
                    if (rename(temp_file, output_file) == 0) {
                        success = true;
                    }
                } else {
                    LOG_error("Invalid M4A file: %s\n", temp_file);
                    unlink(temp_file);
                }
            } else {
                // Cleanup temp file
                unlink(temp_file);
                LOG_error("Download failed: %s\n", video_id);
            }
        }

        // Update queue item status
        pthread_mutex_lock(&queue_mutex);
        if (download_index < queue_count) {
            if (success) {
                download_status.completed_count++;
                // Remove successful download from queue
                for (int i = download_index; i < queue_count - 1; i++) {
                    download_queue[i] = download_queue[i + 1];
                }
                queue_count--;
            } else {
                download_queue[download_index].status = DOWNLOADER_STATUS_FAILED;
                download_queue[download_index].progress_percent = 0;
                download_status.failed_count++;
            }
        }
        pthread_mutex_unlock(&queue_mutex);

        // Reset speed/ETA for next item
        download_status.speed_bps = 0;
        download_status.eta_sec = 0;
    }

    // Re-enable auto sleep when downloads complete
    PWR_enableAutosleep();

    download_status.speed_bps = 0;
    download_status.eta_sec = 0;
    download_running = false;
    youtube_state = DOWNLOADER_STATE_IDLE;

    // Save queue state
    Downloader_saveQueue();

    return NULL;
}

int Downloader_downloadStart(void) {
    if (download_running) {
        return 0;  // Already running, thread will pick up new items
    }

    // Count pending items
    pthread_mutex_lock(&queue_mutex);
    int pending = 0;
    for (int i = 0; i < queue_count; i++) {
        if (download_queue[i].status == DOWNLOADER_STATUS_PENDING) {
            pending++;
        }
    }
    pthread_mutex_unlock(&queue_mutex);

    if (pending == 0) {
        return -1;  // Nothing to download
    }

    // Reset download status
    memset(&download_status, 0, sizeof(download_status));
    download_status.state = DOWNLOADER_STATE_DOWNLOADING;
    download_status.total_items = pending;

    download_running = true;
    download_should_stop = false;
    youtube_state = DOWNLOADER_STATE_DOWNLOADING;

    if (pthread_create(&download_thread, NULL, download_thread_func, NULL) != 0) {
        download_running = false;
        youtube_state = DOWNLOADER_STATE_ERROR;
        return -1;
    }

    pthread_detach(download_thread);
    return 0;
}

void Downloader_downloadStop(void) {
    if (download_running) {
        download_should_stop = true;
        // Thread is detached, just signal and return - no need to wait
    }
}

bool Downloader_isDownloading(void) {
    return download_running;
}

const DownloaderDownloadStatus* Downloader_getDownloadStatus(void) {
    download_status.state = youtube_state;
    return &download_status;
}

static void* update_thread_func(void* arg) {
    (void)arg;


    update_status.updating = true;
    update_status.progress_percent = 0;

    // Check connectivity
    int conn = system("ping -c 1 -W 2 8.8.8.8 >/dev/null 2>&1");
    if (conn != 0) {
        conn = system("ping -c 1 -W 2 1.1.1.1 >/dev/null 2>&1");
    }

    if (conn != 0) {
        strncpy(update_status.error_message, "No internet connection", sizeof(update_status.error_message) - 1);
        update_status.error_message[sizeof(update_status.error_message) - 1] = '\0';
        update_status.updating = false;
        update_running = false;
        return NULL;
    }

    // Check for cancellation
    if (update_should_stop) {
        update_status.updating = false;
        update_running = false;
        return NULL;
    }

    update_status.progress_percent = 10;

    // Fetch latest version from GitHub API
    char temp_dir[512];
    snprintf(temp_dir, sizeof(temp_dir), "/tmp/ytdlp_update_%d", getpid());
    mkdir(temp_dir, 0755);

    char latest_file[600];
    snprintf(latest_file, sizeof(latest_file), "%s/latest.json", temp_dir);

    char cmd[1024];
    char error_file[600];
    char wget_bin[600];
    snprintf(error_file, sizeof(error_file), "%s/wget_error.txt", temp_dir);
    snprintf(wget_bin, sizeof(wget_bin), "%s/bin/wget", pak_path);

    update_status.progress_percent = 15;

    // Use timeout to prevent indefinite blocking on slow/unstable WiFi
    snprintf(cmd, sizeof(cmd),
        "%s -q -T 30 -t 2 -U \"NextUI-Music-Player\" -O \"%s\" \"https://api.github.com/repos/yt-dlp/yt-dlp/releases/latest\" 2>\"%s\"",
        wget_bin, latest_file, error_file);

    int wget_result = system(cmd);
    if (wget_result != 0 || access(latest_file, F_OK) != 0) {
        // Copy error file to pak for debugging
        char debug_cmd[1024];
        snprintf(debug_cmd, sizeof(debug_cmd), "cp \"%s\" \"%s/state/wget_error.txt\" 2>/dev/null", error_file, pak_path);
        system(debug_cmd);

        // Try to read the actual error
        FILE* ef = fopen(error_file, "r");
        if (ef) {
            char err_line[128];
            if (fgets(err_line, sizeof(err_line), ef)) {
                char* nl = strchr(err_line, '\n');
                if (nl) *nl = '\0';
                // Shorter prefix to avoid truncation
                strncpy(update_status.error_message, err_line, sizeof(update_status.error_message) - 1);
            } else {
                snprintf(update_status.error_message, sizeof(update_status.error_message),
                    "wget error %d", WEXITSTATUS(wget_result));
            }
            fclose(ef);
        } else {
            strncpy(update_status.error_message, "Failed to check GitHub", sizeof(update_status.error_message) - 1);
            update_status.error_message[sizeof(update_status.error_message) - 1] = '\0';
        }
        update_status.updating = false;
        update_running = false;
        return NULL;
    }

    // Check for cancellation after wget
    if (update_should_stop) {
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
        system(cmd);
        update_status.updating = false;
        update_running = false;
        return NULL;
    }

    update_status.progress_percent = 30;

    // Parse version from JSON (simple grep approach)
    char version_cmd[1024];
    snprintf(version_cmd, sizeof(version_cmd),
        "grep -o '\"tag_name\": *\"[^\"]*' \"%s\" | cut -d'\"' -f4",
        latest_file);

    char latest_version[32] = "";
    FILE* pipe = popen(version_cmd, "r");
    if (pipe) {
        if (fgets(latest_version, sizeof(latest_version), pipe)) {
            char* nl = strchr(latest_version, '\n');
            if (nl) *nl = '\0';
        }
        pclose(pipe);
    }

    if (strlen(latest_version) == 0) {
        strncpy(update_status.error_message, "Could not parse version", sizeof(update_status.error_message) - 1);
        update_status.error_message[sizeof(update_status.error_message) - 1] = '\0';
        update_status.updating = false;
        update_running = false;
        return NULL;
    }

    strncpy(update_status.latest_version, latest_version, sizeof(update_status.latest_version));
    strncpy(update_status.current_version, current_version, sizeof(update_status.current_version));

    // Compare versions
    if (strcmp(latest_version, current_version) == 0) {
        update_status.update_available = false;
        update_status.progress_percent = 100;  // Signal completion for UI
        update_status.updating = false;
        update_running = false;
        return NULL;
    }

    // Check for cancellation before download
    if (update_should_stop) {
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
        system(cmd);
        update_status.updating = false;
        update_running = false;
        return NULL;
    }

    update_status.update_available = true;
    update_status.progress_percent = 40;

    // Get download URL for aarch64
    char url_cmd[1024];
    snprintf(url_cmd, sizeof(url_cmd),
        "grep -o '\"browser_download_url\": *\"[^\"]*yt-dlp_linux_aarch64\"' \"%s\" | cut -d'\"' -f4",
        latest_file);

    char download_url[512] = "";
    pipe = popen(url_cmd, "r");
    if (pipe) {
        if (fgets(download_url, sizeof(download_url), pipe)) {
            char* nl = strchr(download_url, '\n');
            if (nl) *nl = '\0';
        }
        pclose(pipe);
    }

    if (strlen(download_url) == 0) {
        strncpy(update_status.error_message, "No ARM64 binary found", sizeof(update_status.error_message) - 1);
        update_status.error_message[sizeof(update_status.error_message) - 1] = '\0';
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
        system(cmd);
        update_status.updating = false;
        update_running = false;
        return NULL;
    }

    // Check for cancellation before large download
    if (update_should_stop) {
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
        system(cmd);
        update_status.updating = false;
        update_running = false;
        return NULL;
    }

    update_status.progress_percent = 50;

    // Download new binary with real-time progress via file size monitoring
    char new_binary[600];
    snprintf(new_binary, sizeof(new_binary), "%s/yt-dlp.new", temp_dir);

    update_status.progress_percent = 50;
    update_status.download_bytes = 0;
    update_status.download_total = 0;
    strncpy(update_status.status_detail, "Getting file info...", sizeof(update_status.status_detail) - 1);

    // First, get the actual file size from server using wget --spider
    // GitHub releases redirect to CDN, so we need to follow redirects and get the final Content-Length
    char size_file[600];
    snprintf(size_file, sizeof(size_file), "%s/size.txt", temp_dir);

    // Use --max-redirect to follow GitHub's redirects, get last Content-Length (from final destination)
    snprintf(cmd, sizeof(cmd),
        "%s --spider -S --max-redirect=10 -T 30 -U \"NextUI-Music-Player\" \"%s\" 2>&1 | grep -i 'Content-Length' | tail -1 | awk '{print $2}' | tr -d '\\r' > \"%s\"",
        wget_bin, download_url, size_file);
    system(cmd);

    // Read the file size
    FILE* size_f = fopen(size_file, "r");
    if (size_f) {
        long file_size = 0;
        if (fscanf(size_f, "%ld", &file_size) == 1 && file_size > 1000000) {
            update_status.download_total = file_size;
        }
        fclose(size_f);
    }

    // Fallback to approximate size if we couldn't get it (yt-dlp is ~34MB as of 2024+)
    if (update_status.download_total == 0) {
        update_status.download_total = 35000000;  // ~35MB fallback
    }

    // Check for cancellation
    if (update_should_stop) {
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
        system(cmd);
        update_status.updating = false;
        update_running = false;
        return NULL;
    }

    strncpy(update_status.status_detail, "Starting download...", sizeof(update_status.status_detail) - 1);

    // Create a marker file to track wget completion
    char done_marker[600];
    snprintf(done_marker, sizeof(done_marker), "%s/wget.done", temp_dir);

    // Start wget in background, create marker file when done
    // Using shell to chain commands: wget && touch done_marker
    snprintf(cmd, sizeof(cmd),
        "(%s -T 120 -t 3 -q -U \"NextUI-Music-Player\" -O \"%s\" \"%s\"; echo $? > \"%s\") &",
        wget_bin, new_binary, download_url, done_marker);
    system(cmd);

    // Monitor download progress
    int timeout_seconds = 0;
    const int max_timeout = 180;  // 3 minutes
    long last_size = 0;
    int stable_count = 0;

    while (timeout_seconds < max_timeout) {
        // Check for cancellation
        if (update_should_stop) {
            system("pkill -f 'wget.*yt-dlp' 2>/dev/null");
            snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
            system(cmd);
            update_status.updating = false;
            update_running = false;
            return NULL;
        }

        // Check if wget finished (marker file exists)
        if (access(done_marker, F_OK) == 0) {
            break;
        }

        // Check current file size
        struct stat st;
        if (stat(new_binary, &st) == 0 && st.st_size > 0) {
            update_status.download_bytes = st.st_size;

            // Calculate progress (map to 50-78% range, save 78-80 for verification)
            int dl_percent = (int)((st.st_size * 100) / update_status.download_total);
            if (dl_percent > 100) dl_percent = 100;
            update_status.progress_percent = 50 + (dl_percent * 28 / 100);

            // Format MB display
            float mb_done = st.st_size / (1024.0f * 1024.0f);
            float mb_total = update_status.download_total / (1024.0f * 1024.0f);
            snprintf(update_status.status_detail, sizeof(update_status.status_detail),
                "%.1fMB / %.1fMB", mb_done, mb_total);

            // Track if file stopped growing (might be done or stalled)
            if (st.st_size == last_size) {
                stable_count++;
            } else {
                stable_count = 0;
            }
            last_size = st.st_size;
        } else {
            // File not created yet
            snprintf(update_status.status_detail, sizeof(update_status.status_detail),
                "Connecting...");
        }

        usleep(500000);  // Check every 0.5 seconds for smoother updates
        timeout_seconds++;
        if (timeout_seconds % 2 == 0) {
            // Only increment actual timeout counter every second
        }
    }

    // Wait a moment for wget to fully finish writing
    usleep(500000);

    // Kill any remaining wget just in case
    system("pkill -f 'wget.*yt-dlp' 2>/dev/null");

    // Check wget exit status from marker file
    int wget_exit = -1;
    FILE* marker_f = fopen(done_marker, "r");
    if (marker_f) {
        fscanf(marker_f, "%d", &wget_exit);
        fclose(marker_f);
    }

    update_status.progress_percent = 78;

    // Verify download completed
    struct stat final_st;
    if (stat(new_binary, &final_st) != 0 || final_st.st_size < 1000000) {
        // File doesn't exist or too small (< 1MB)
        if (wget_exit != 0 && wget_exit != -1) {
            snprintf(update_status.error_message, sizeof(update_status.error_message),
                "Download failed (error %d)", wget_exit);
        } else if (timeout_seconds >= max_timeout) {
            strncpy(update_status.error_message, "Download timed out", sizeof(update_status.error_message) - 1);
        } else {
            snprintf(update_status.error_message, sizeof(update_status.error_message),
                "Incomplete (%ld bytes)", (long)(stat(new_binary, &final_st) == 0 ? final_st.st_size : 0));
        }
        update_status.error_message[sizeof(update_status.error_message) - 1] = '\0';
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
        system(cmd);
        update_status.updating = false;
        update_running = false;
        return NULL;
    }

    // Update final size for display
    update_status.download_bytes = final_st.st_size;
    update_status.download_total = final_st.st_size;
    float final_mb = final_st.st_size / (1024.0f * 1024.0f);
    snprintf(update_status.status_detail, sizeof(update_status.status_detail),
        "%.1f MB downloaded", final_mb);

    // Check for cancellation after download
    if (update_should_stop) {
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
        system(cmd);
        update_status.updating = false;
        update_running = false;
        return NULL;
    }

    update_status.progress_percent = 80;

    // Make executable
    chmod(new_binary, 0755);

    // Backup old binary
    char backup_path[600];
    snprintf(backup_path, sizeof(backup_path), "%s.old", ytdlp_path);
    rename(ytdlp_path, backup_path);

    // Move new binary
    snprintf(cmd, sizeof(cmd), "mv \"%s\" \"%s\"", new_binary, ytdlp_path);
    if (system(cmd) != 0) {
        // Restore backup
        rename(backup_path, ytdlp_path);
        strncpy(update_status.error_message, "Failed to install update", sizeof(update_status.error_message) - 1);
        update_status.error_message[sizeof(update_status.error_message) - 1] = '\0';
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
        system(cmd);
        update_status.updating = false;
        update_running = false;
        return NULL;
    }

    // Update version file
    FILE* vf = fopen(version_file, "w");
    if (vf) {
        fprintf(vf, "%s\n", latest_version);
        fclose(vf);
    }

    strncpy(current_version, latest_version, sizeof(current_version));

    // Cleanup
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", temp_dir);
    system(cmd);

    update_status.progress_percent = 100;
    update_status.updating = false;
    update_running = false;

    return NULL;
}

int Downloader_checkForUpdate(void) {
    if (update_running) return 0;

    // Just check version without downloading
    memset(&update_status, 0, sizeof(update_status));
    strncpy(update_status.current_version, current_version, sizeof(update_status.current_version));

    return 0;
}

int Downloader_startUpdate(void) {
    if (update_running) return 0;

    memset(&update_status, 0, sizeof(update_status));
    strncpy(update_status.current_version, current_version, sizeof(update_status.current_version));

    update_running = true;
    update_should_stop = false;
    youtube_state = DOWNLOADER_STATE_UPDATING;

    if (pthread_create(&update_thread, NULL, update_thread_func, NULL) != 0) {
        update_running = false;
        youtube_state = DOWNLOADER_STATE_ERROR;
        strncpy(error_message, "Failed to create update thread", sizeof(error_message) - 1);
        error_message[sizeof(error_message) - 1] = '\0';
        return -1;
    }

    pthread_detach(update_thread);
    return 0;
}

void Downloader_cancelUpdate(void) {
    if (update_running) {
        update_should_stop = true;
        // Thread is detached, just signal and return - no need to wait
    }
}

const DownloaderUpdateStatus* Downloader_getUpdateStatus(void) {
    return &update_status;
}

DownloaderState Downloader_getState(void) {
    return youtube_state;
}

const char* Downloader_getError(void) {
    return error_message;
}

void Downloader_update(void) {
    // Check if threads finished
    if (!download_running && youtube_state == DOWNLOADER_STATE_DOWNLOADING) {
        youtube_state = DOWNLOADER_STATE_IDLE;
    }
    if (!update_running && youtube_state == DOWNLOADER_STATE_UPDATING) {
        youtube_state = DOWNLOADER_STATE_IDLE;
    }
}

void Downloader_saveQueue(void) {
    pthread_mutex_lock(&queue_mutex);

    FILE* f = fopen(queue_file, "w");
    if (f) {
        for (int i = 0; i < queue_count; i++) {
            // Only save pending items
            if (download_queue[i].status == DOWNLOADER_STATUS_PENDING) {
                fprintf(f, "%s|%s\n",
                    download_queue[i].video_id,
                    download_queue[i].title);
            }
        }
        fclose(f);
    }

    pthread_mutex_unlock(&queue_mutex);
}

void Downloader_loadQueue(void) {
    pthread_mutex_lock(&queue_mutex);

    queue_count = 0;

    FILE* f = fopen(queue_file, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f) && queue_count < DOWNLOADER_MAX_QUEUE) {
            char* nl = strchr(line, '\n');
            if (nl) *nl = '\0';

            char* video_id = strtok(line, "|");
            char* title = strtok(NULL, "|");

            if (video_id && title) {
                strncpy(download_queue[queue_count].video_id, video_id, DOWNLOADER_VIDEO_ID_LEN - 1);
                strncpy(download_queue[queue_count].title, title, DOWNLOADER_MAX_TITLE - 1);
                download_queue[queue_count].status = DOWNLOADER_STATUS_PENDING;
                download_queue[queue_count].progress_percent = 0;
                queue_count++;
            }
        }
        fclose(f);
    }

    pthread_mutex_unlock(&queue_mutex);

}

const char* Downloader_getDownloadPath(void) {
    return download_dir;
}

char* Downloader_openKeyboard(const char* prompt) {
    return Keyboard_open(prompt);
}

static void sanitize_filename(const char* input, char* output, size_t max_len) {
    size_t j = 0;
    for (size_t i = 0; input[i] && j < max_len - 1; i++) {
        unsigned char c = (unsigned char)input[i];

        // Allow UTF-8 multi-byte sequences (bytes >= 0x80)
        // This preserves Korean, Japanese, Chinese, and other Unicode characters
        if (c >= 0x80) {
            output[j++] = (char)c;
            continue;
        }

        // Allow ASCII alphanumeric and safe symbols
        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == ' ' || c == '.' || c == '_' || c == '-' ||
            c == '(' || c == ')' || c == '[' || c == ']' ||
            c == '!' || c == ',' || c == '\'') {
            output[j++] = (char)c;
        }
        // Skip filesystem-unsafe characters: / \ : * ? " < > |
    }
    output[j] = '\0';

    // Trim to 120 bytes (allow longer names for CJK which use 3 bytes per char)
    if (j > 120) {
        // Find a safe truncation point (don't cut in middle of UTF-8 sequence)
        j = 120;
        while (j > 0 && (output[j] & 0xC0) == 0x80) {
            j--;  // Back up to start of UTF-8 sequence
        }
        output[j] = '\0';
    }

    // Trim trailing/leading spaces
    while (j > 0 && output[j-1] == ' ') {
        output[--j] = '\0';
    }

    char* start = output;
    while (*start == ' ') start++;
    if (start != output) {
        memmove(output, start, strlen(start) + 1);
    }

    // Default if empty
    if (output[0] == '\0') {
        strncpy(output, "download", max_len - 1);
        output[max_len - 1] = '\0';
    }
}

static int run_command(const char* cmd, char* output, size_t output_size) {
    FILE* pipe = popen(cmd, "r");
    if (!pipe) return -1;

    if (output && output_size > 0) {
        output[0] = '\0';
        size_t total = 0;
        char buf[256];
        while (fgets(buf, sizeof(buf), pipe) && total < output_size - 1) {
            size_t len = strlen(buf);
            if (total + len < output_size) {
                strcat(output, buf);
                total += len;
            }
        }
    }

    return pclose(pipe);
}
