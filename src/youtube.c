#define _GNU_SOURCE
#include "youtube.h"
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
static char keyboard_path[512] = "";
static char wget_path[512] = "";
static char download_dir[512] = "";
static char queue_file[512] = "";
static char version_file[512] = "";
static char pak_path[512] = "";

// Module state
static YouTubeState youtube_state = YOUTUBE_STATE_IDLE;
static char error_message[256] = "";

// Download queue
static YouTubeQueueItem download_queue[YOUTUBE_MAX_QUEUE];
static int queue_count = 0;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;

// Download status
static YouTubeDownloadStatus download_status = {0};
static pthread_t download_thread;
static volatile bool download_running = false;
static volatile bool download_should_stop = false;

// Update status
static YouTubeUpdateStatus update_status = {0};
static pthread_t update_thread;
static volatile bool update_running = false;
static volatile bool update_should_stop = false;

// Search
static pthread_t search_thread;
static volatile bool search_running = false;
static volatile bool search_should_stop = false;
static YouTubeResult search_results[YOUTUBE_MAX_RESULTS];
static int search_result_count = 0;

// Current yt-dlp version
static char current_version[32] = "unknown";

// Forward declarations
static void* download_thread_func(void* arg);
static void* update_thread_func(void* arg);
static int run_command(const char* cmd, char* output, size_t output_size);
static void sanitize_filename(const char* input, char* output, size_t max_len);

int YouTube_init(void) {
    // Build paths based on pak location
    // Try multiple locations where the pak might be
    const char* search_paths[] = {
        "%s/.system/tg5040/paks/Emus/Music Player.pak",
        "%s/.system/tg5040/paks/Tools/Music Player.pak",
        "./Music Player.pak",
        ".",  // Current directory (when run from inside pak)
        ".."  // Parent directory
    };

    bool found = false;
    for (int i = 0; i < 5 && !found; i++) {
        if (i < 2) {
            snprintf(pak_path, sizeof(pak_path), search_paths[i], SDCARD_PATH);
        } else {
            strcpy(pak_path, search_paths[i]);
        }

        char test_path[600];
        snprintf(test_path, sizeof(test_path), "%s/bins/yt-dlp", pak_path);

        // Check if file exists (not just executable, as permissions might be different)
        if (access(test_path, F_OK) == 0) {
            found = true;
        }
    }

    if (!found) {
        LOG_error("yt-dlp binary not found in any search path\n");
        strcpy(error_message, "yt-dlp not found");
        return -1;
    }

    // Set paths
    snprintf(ytdlp_path, sizeof(ytdlp_path), "%s/bins/yt-dlp", pak_path);
    snprintf(keyboard_path, sizeof(keyboard_path), "%s/bins/keyboard", pak_path);
    snprintf(wget_path, sizeof(wget_path), "%s/bins/wget", pak_path);
    snprintf(version_file, sizeof(version_file), "%s/state/yt-dlp_version.txt", pak_path);
    snprintf(queue_file, sizeof(queue_file), "%s/state/youtube_queue.txt", pak_path);
    snprintf(download_dir, sizeof(download_dir), "%s/Music", SDCARD_PATH);

    // Ensure binaries are executable
    chmod(ytdlp_path, 0755);
    chmod(keyboard_path, 0755);
    chmod(wget_path, 0755);

    // Create music directory if needed
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
    YouTube_loadQueue();

    return 0;
}

void YouTube_cleanup(void) {
    // Stop any running operations
    YouTube_downloadStop();
    YouTube_cancelUpdate();
    YouTube_cancelSearch();

    // Save queue
    YouTube_saveQueue();

}

bool YouTube_isAvailable(void) {
    return access(ytdlp_path, X_OK) == 0;
}

const char* YouTube_getVersion(void) {
    return current_version;
}

int YouTube_search(const char* query, YouTubeResult* results, int max_results) {
    if (!query || !results || max_results <= 0) {
        return -1;
    }

    if (search_running) {
        return -1;  // Already searching
    }

    youtube_state = YOUTUBE_STATE_SEARCHING;

    // Sanitize query - escape special characters
    char safe_query[256];
    int j = 0;
    for (int i = 0; query[i] && j < (int)sizeof(safe_query) - 2; i++) {
        char c = query[i];
        // Skip potentially dangerous characters for shell
        if (c == '"' || c == '\'' || c == '`' || c == '$' || c == '\\' || c == ';' || c == '&' || c == '|') {
            continue;
        }
        safe_query[j++] = c;
    }
    safe_query[j] = '\0';

    int num_results = max_results > YOUTUBE_MAX_RESULTS ? YOUTUBE_MAX_RESULTS : max_results;

    // Use a temp file to capture results (more reliable than pipe)
    const char* temp_file = "/tmp/yt_search_results.txt";
    const char* temp_err = "/tmp/yt_search_error.txt";

    // Build yt-dlp search command - use tab as delimiter to avoid issues with | in titles
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "%s 'ytsearch%d:%s music' "
        "--flat-playlist "
        "--no-warnings "
        "--print '%%(id)s\t%%(title)s\t%%(duration_string)s' "
        "> %s 2> %s",
        ytdlp_path,
        num_results,
        safe_query,
        temp_file,
        temp_err);


    int ret = system(cmd);
    if (ret != 0) {
        // Try to read error message
        FILE* err = fopen(temp_err, "r");
        if (err) {
            char err_line[256];
            if (fgets(err_line, sizeof(err_line), err)) {
                LOG_error("yt-dlp error: %s\n", err_line);
            }
            fclose(err);
        }
    }

    // Read results from temp file
    FILE* f = fopen(temp_file, "r");
    if (!f) {
        strcpy(error_message, "Failed to read search results");
        youtube_state = YOUTUBE_STATE_ERROR;
        return -1;
    }

    char line[512];
    int count = 0;

    while (fgets(line, sizeof(line), f) && count < max_results) {
        // Remove newline
        char* nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        // Skip empty lines
        if (line[0] == '\0') continue;

        // Make a copy for strtok since it modifies the string
        char line_copy[512];
        strncpy(line_copy, line, sizeof(line_copy) - 1);
        line_copy[sizeof(line_copy) - 1] = '\0';

        // Parse: id<TAB>title<TAB>duration (tab-separated)
        char* id = strtok(line_copy, "\t");
        char* title = strtok(NULL, "\t");
        char* duration = strtok(NULL, "\t");

        if (id && title && strlen(id) > 0) {
            strncpy(results[count].title, title, YOUTUBE_MAX_TITLE - 1);
            results[count].title[YOUTUBE_MAX_TITLE - 1] = '\0';

            strncpy(results[count].video_id, id, YOUTUBE_VIDEO_ID_LEN - 1);
            results[count].video_id[YOUTUBE_VIDEO_ID_LEN - 1] = '\0';

            results[count].artist[0] = '\0';

            // Parse duration string (e.g., "3:45" or "1:23:45")
            results[count].duration_sec = 0;
            if (duration && strlen(duration) > 0) {
                int h = 0, m = 0, s = 0;
                int parts = sscanf(duration, "%d:%d:%d", &h, &m, &s);
                if (parts == 2) {
                    results[count].duration_sec = h * 60 + m;
                } else if (parts == 3) {
                    results[count].duration_sec = h * 3600 + m * 60 + s;
                }
            }

            count++;
        }
    }

    fclose(f);

    // Cleanup temp files
    unlink(temp_file);
    unlink(temp_err);

    youtube_state = YOUTUBE_STATE_IDLE;

    return count;
}

void YouTube_cancelSearch(void) {
    search_should_stop = true;
}

int YouTube_queueAdd(const char* video_id, const char* title) {
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
    if (queue_count >= YOUTUBE_MAX_QUEUE) {
        pthread_mutex_unlock(&queue_mutex);
        return -1;  // Queue full
    }

    // Add to queue
    strncpy(download_queue[queue_count].video_id, video_id, YOUTUBE_VIDEO_ID_LEN - 1);
    strncpy(download_queue[queue_count].title, title, YOUTUBE_MAX_TITLE - 1);
    download_queue[queue_count].status = YOUTUBE_STATUS_PENDING;
    download_queue[queue_count].progress_percent = 0;
    queue_count++;

    pthread_mutex_unlock(&queue_mutex);

    // Save queue to file
    YouTube_saveQueue();

    return 1;  // Successfully added
}

int YouTube_queueRemove(int index) {
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

    YouTube_saveQueue();
    return 0;
}

int YouTube_queueRemoveById(const char* video_id) {
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

    YouTube_saveQueue();
    return 0;
}

int YouTube_queueClear(void) {
    pthread_mutex_lock(&queue_mutex);
    queue_count = 0;
    pthread_mutex_unlock(&queue_mutex);

    YouTube_saveQueue();
    return 0;
}

int YouTube_queueCount(void) {
    return queue_count;
}

YouTubeQueueItem* YouTube_queueGet(int* count) {
    if (count) *count = queue_count;
    return download_queue;
}

bool YouTube_isInQueue(const char* video_id) {
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

bool YouTube_isDownloaded(const char* video_id) {
    if (!video_id) return false;

    // Check if file exists in download directory
    // This is a simple check - could be improved with a database
    char pattern[600];
    snprintf(pattern, sizeof(pattern), "%s/*%s*", download_dir, video_id);

    // For now, just return false - would need glob() for proper implementation
    return false;
}

static void* download_thread_func(void* arg) {
    (void)arg;


    while (!download_should_stop) {
        pthread_mutex_lock(&queue_mutex);

        // Find next pending item
        int download_index = -1;
        for (int i = 0; i < queue_count; i++) {
            if (download_queue[i].status == YOUTUBE_STATUS_PENDING) {
                download_index = i;
                break;
            }
        }

        if (download_index < 0) {
            pthread_mutex_unlock(&queue_mutex);
            break;  // No more items
        }

        // Mark as downloading
        download_queue[download_index].status = YOUTUBE_STATUS_DOWNLOADING;
        char video_id[YOUTUBE_VIDEO_ID_LEN];
        char title[YOUTUBE_MAX_TITLE];
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
        snprintf(output_file, sizeof(output_file), "%s/%s.mp3", download_dir, safe_filename);
        snprintf(temp_file, sizeof(temp_file), "%s/.downloading_%s.mp3", download_dir, video_id);

        // Check if already exists
        bool success = false;
        if (access(output_file, F_OK) == 0) {
            success = true;
        } else {
            // Build download command with ffmpeg in PATH for conversion and metadata
            // Use --newline for progress parsing and --progress for percentage output
            char cmd[2048];
            snprintf(cmd, sizeof(cmd),
                "PATH=\"%s/bins:$PATH\" %s "
                "-f \"bestaudio\" "
                "-x --audio-format mp3 --audio-quality 0 "
                "--embed-metadata "
                "--newline --progress "
                "-o \"%s\" "
                "--no-playlist "
                "\"https://music.youtube.com/watch?v=%s\" "
                "2>&1",
                pak_path, ytdlp_path, temp_file, video_id);


            // Use popen to read progress in real-time
            FILE* pipe = popen(cmd, "r");
            int result = -1;

            if (pipe) {
                char line[512];
                while (fgets(line, sizeof(line), pipe)) {
                    // Parse progress from yt-dlp output
                    // Format: [download]  XX.X% of ...
                    char* pct = strstr(line, "%");
                    if (pct && strstr(line, "[download]")) {
                        // Find the start of the percentage number
                        char* start = pct - 1;
                        while (start > line && (isdigit(*start) || *start == '.')) {
                            start--;
                        }
                        start++;

                        float percent = 0;
                        if (sscanf(start, "%f", &percent) == 1) {
                            pthread_mutex_lock(&queue_mutex);
                            if (download_index < queue_count) {
                                // Download is ~70% of total, conversion is ~30%
                                download_queue[download_index].progress_percent = (int)(percent * 0.7f);
                            }
                            pthread_mutex_unlock(&queue_mutex);
                        }
                    }
                    // Check for ffmpeg conversion progress (post-processing)
                    if (strstr(line, "[ExtractAudio]") || strstr(line, "Post-process")) {
                        pthread_mutex_lock(&queue_mutex);
                        if (download_index < queue_count) {
                            download_queue[download_index].progress_percent = 75;
                        }
                        pthread_mutex_unlock(&queue_mutex);
                    }
                    if (strstr(line, "[Metadata]") || strstr(line, "Adding metadata")) {
                        pthread_mutex_lock(&queue_mutex);
                        if (download_index < queue_count) {
                            download_queue[download_index].progress_percent = 90;
                        }
                        pthread_mutex_unlock(&queue_mutex);
                    }
                }
                result = pclose(pipe);
            }

            if (result == 0 && access(temp_file, F_OK) == 0) {
                // Validate MP3 file before moving
                bool valid_mp3 = false;
                struct stat st;
                if (stat(temp_file, &st) == 0 && st.st_size >= 10240) {
                    // Minimum 10KB for a valid MP3
                    int fd = open(temp_file, O_RDONLY);
                    if (fd >= 0) {
                        unsigned char header[10];
                        if (read(fd, header, 10) == 10) {
                            // Check for ID3v2 tag or MP3 sync bytes
                            if ((header[0] == 'I' && header[1] == 'D' && header[2] == '3') ||
                                (header[0] == 0xFF && (header[1] & 0xE0) == 0xE0)) {
                                valid_mp3 = true;
                            }
                        }
                        close(fd);
                    }
                }

                if (valid_mp3) {
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
                    LOG_error("Invalid MP3 file: %s\n", temp_file);
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
                download_queue[download_index].status = YOUTUBE_STATUS_FAILED;
                download_queue[download_index].progress_percent = 0;
                download_status.failed_count++;
            }
        }
        pthread_mutex_unlock(&queue_mutex);
    }

    download_running = false;
    youtube_state = YOUTUBE_STATE_IDLE;

    // Save queue state
    YouTube_saveQueue();

    return NULL;
}

int YouTube_downloadStart(void) {
    if (download_running) {
        return 0;  // Already running
    }

    if (queue_count == 0) {
        return -1;  // Nothing to download
    }

    // Count pending items
    int pending = 0;
    for (int i = 0; i < queue_count; i++) {
        if (download_queue[i].status == YOUTUBE_STATUS_PENDING) {
            pending++;
        }
    }

    if (pending == 0) {
        return -1;  // Nothing pending
    }

    // Reset status
    memset(&download_status, 0, sizeof(download_status));
    download_status.state = YOUTUBE_STATE_DOWNLOADING;
    download_status.total_items = pending;

    download_running = true;
    download_should_stop = false;
    youtube_state = YOUTUBE_STATE_DOWNLOADING;

    if (pthread_create(&download_thread, NULL, download_thread_func, NULL) != 0) {
        download_running = false;
        youtube_state = YOUTUBE_STATE_ERROR;
        strcpy(error_message, "Failed to create download thread");
        return -1;
    }

    pthread_detach(download_thread);
    return 0;
}

void YouTube_downloadStop(void) {
    if (download_running) {
        download_should_stop = true;
        // Wait briefly for thread to stop
        usleep(100000);  // 100ms
    }
}

const YouTubeDownloadStatus* YouTube_getDownloadStatus(void) {
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
        strcpy(update_status.error_message, "No internet connection");
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
    if (access(wget_path, X_OK) == 0) {
        snprintf(cmd, sizeof(cmd),
            "%s -q -O \"%s\" \"https://api.github.com/repos/yt-dlp/yt-dlp/releases/latest\" 2>/dev/null",
            wget_path, latest_file);
    } else {
        // Fall back to system wget if available
        snprintf(cmd, sizeof(cmd),
            "wget -q -O \"%s\" \"https://api.github.com/repos/yt-dlp/yt-dlp/releases/latest\" 2>/dev/null",
            latest_file);
    }

    if (system(cmd) != 0 || access(latest_file, F_OK) != 0) {
        strcpy(update_status.error_message, "Failed to check GitHub");
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
        strcpy(update_status.error_message, "Could not parse version");
        update_status.updating = false;
        update_running = false;
        return NULL;
    }

    strncpy(update_status.latest_version, latest_version, sizeof(update_status.latest_version));
    strncpy(update_status.current_version, current_version, sizeof(update_status.current_version));

    // Compare versions
    if (strcmp(latest_version, current_version) == 0) {
        update_status.update_available = false;
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
        strcpy(update_status.error_message, "No ARM64 binary found");
        update_status.updating = false;
        update_running = false;
        return NULL;
    }

    update_status.progress_percent = 50;

    // Download new binary
    char new_binary[600];
    snprintf(new_binary, sizeof(new_binary), "%s/bins/yt-dlp", temp_dir);

    if (access(wget_path, X_OK) == 0) {
        snprintf(cmd, sizeof(cmd), "%s -q -O \"%s\" \"%s\" 2>/dev/null",
            wget_path, new_binary, download_url);
    } else {
        snprintf(cmd, sizeof(cmd), "wget -q -O \"%s\" \"%s\" 2>/dev/null",
            new_binary, download_url);
    }


    if (system(cmd) != 0 || access(new_binary, F_OK) != 0) {
        strcpy(update_status.error_message, "Download failed");
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
        strcpy(update_status.error_message, "Failed to install update");
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

int YouTube_checkForUpdate(void) {
    if (update_running) return 0;

    // Just check version without downloading
    memset(&update_status, 0, sizeof(update_status));
    strncpy(update_status.current_version, current_version, sizeof(update_status.current_version));

    return 0;
}

int YouTube_startUpdate(void) {
    if (update_running) return 0;

    memset(&update_status, 0, sizeof(update_status));
    strncpy(update_status.current_version, current_version, sizeof(update_status.current_version));

    update_running = true;
    update_should_stop = false;
    youtube_state = YOUTUBE_STATE_UPDATING;

    if (pthread_create(&update_thread, NULL, update_thread_func, NULL) != 0) {
        update_running = false;
        youtube_state = YOUTUBE_STATE_ERROR;
        strcpy(error_message, "Failed to create update thread");
        return -1;
    }

    pthread_detach(update_thread);
    return 0;
}

void YouTube_cancelUpdate(void) {
    if (update_running) {
        update_should_stop = true;
        usleep(100000);
    }
}

const YouTubeUpdateStatus* YouTube_getUpdateStatus(void) {
    return &update_status;
}

YouTubeState YouTube_getState(void) {
    return youtube_state;
}

const char* YouTube_getError(void) {
    return error_message;
}

void YouTube_update(void) {
    // Check if threads finished
    if (!download_running && youtube_state == YOUTUBE_STATE_DOWNLOADING) {
        youtube_state = YOUTUBE_STATE_IDLE;
    }
    if (!update_running && youtube_state == YOUTUBE_STATE_UPDATING) {
        youtube_state = YOUTUBE_STATE_IDLE;
    }
}

void YouTube_saveQueue(void) {
    pthread_mutex_lock(&queue_mutex);

    FILE* f = fopen(queue_file, "w");
    if (f) {
        for (int i = 0; i < queue_count; i++) {
            // Only save pending items
            if (download_queue[i].status == YOUTUBE_STATUS_PENDING) {
                fprintf(f, "%s|%s\n",
                    download_queue[i].video_id,
                    download_queue[i].title);
            }
        }
        fclose(f);
    }

    pthread_mutex_unlock(&queue_mutex);
}

void YouTube_loadQueue(void) {
    pthread_mutex_lock(&queue_mutex);

    queue_count = 0;

    FILE* f = fopen(queue_file, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f) && queue_count < YOUTUBE_MAX_QUEUE) {
            char* nl = strchr(line, '\n');
            if (nl) *nl = '\0';

            char* video_id = strtok(line, "|");
            char* title = strtok(NULL, "|");

            if (video_id && title) {
                strncpy(download_queue[queue_count].video_id, video_id, YOUTUBE_VIDEO_ID_LEN - 1);
                strncpy(download_queue[queue_count].title, title, YOUTUBE_MAX_TITLE - 1);
                download_queue[queue_count].status = YOUTUBE_STATUS_PENDING;
                download_queue[queue_count].progress_percent = 0;
                queue_count++;
            }
        }
        fclose(f);
    }

    pthread_mutex_unlock(&queue_mutex);

}

const char* YouTube_getDownloadPath(void) {
    return download_dir;
}

char* YouTube_openKeyboard(const char* prompt) {
    (void)prompt;  // Not used with external keyboard

    if (access(keyboard_path, X_OK) != 0) {
        LOG_error("Keyboard binary not found: %s\n", keyboard_path);
        return NULL;
    }

    // Get font path (minui.ttf should be in pak/fonts folder)
    char font_path[600];
    snprintf(font_path, sizeof(font_path), "%s/fonts/minui.ttf", pak_path);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "%s \"%s\" 2>/dev/null", keyboard_path, font_path);

    FILE* pipe = popen(cmd, "r");
    if (!pipe) {
        return NULL;
    }

    char* result = malloc(512);
    if (result) {
        result[0] = '\0';
        if (fgets(result, 512, pipe)) {
            char* nl = strchr(result, '\n');
            if (nl) *nl = '\0';
        }

        // If empty, user cancelled
        if (result[0] == '\0') {
            free(result);
            result = NULL;
        }
    }

    pclose(pipe);
    return result;
}

static void sanitize_filename(const char* input, char* output, size_t max_len) {
    size_t j = 0;
    for (size_t i = 0; input[i] && j < max_len - 1; i++) {
        char c = input[i];
        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == ' ' || c == '.' || c == '_' || c == '-') {
            output[j++] = c;
        }
    }
    output[j] = '\0';

    // Trim to 60 chars
    if (j > 60) {
        output[60] = '\0';
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
        strcpy(output, "download");
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
