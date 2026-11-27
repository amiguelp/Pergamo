#include "opds_app.h"
#include "launcher_icon.h"
#include "uninstall.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define APP_LOG_FILE APP_ROOT_DIR "opds_client.log"
#define ICON_FILE OPDS_LAUNCHER_ICON_DIR "Pergamo.bmp"
#define FOCUSED_ICON_FILE OPDS_LAUNCHER_ICON_DIR "Pergamo_f.bmp"
#define ICON_TEMP_FILE ICON_FILE ".pergamo.tmp"
#define FOCUSED_ICON_TEMP_FILE FOCUSED_ICON_FILE ".pergamo.tmp"
#define VIEW_TEMP_FILE "/mnt/ext1/system/config/desktop/view.json.pergamo.tmp"
#define VIEW_BACKUP_TEMP_FILE "/mnt/ext1/system/config/desktop/view.json.Pergamo.bak.pergamo.tmp"

static int RemoveOwnedFile(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISREG(st.st_mode)) return -1;
    return unlink(path);
}

static int IsCacheFileName(const char *name) {
    size_t length = strlen(name);
    if (length < 11 || strncmp(name, "cache_", 6) != 0 ||
        strcmp(name + length - 4, ".jpg") != 0) {
        return 0;
    }
    for (size_t i = 6; i < length - 4; i++) {
        if (!isdigit((unsigned char)name[i])) return 0;
    }
    return 1;
}

static int RemoveOwnedCacheFiles(void) {
    DIR *directory = opendir(IMAGES_DIR);
    if (!directory) return errno == ENOENT ? 0 : -1;

    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (!IsCacheFileName(entry->d_name)) continue;

        char path[512];
        if (snprintf(path, sizeof(path), "%s%s", IMAGES_DIR, entry->d_name) >=
            (int)sizeof(path)) {
            result = -1;
            continue;
        }
        if (RemoveOwnedFile(path) != 0) result = -1;
    }
    if (closedir(directory) != 0) result = -1;
    return result;
}

int ClearPergamoCoverCache(void) {
    return RemoveOwnedCacheFiles();
}

static int RemoveEmptyDirectory(const char *path) {
    if (rmdir(path) == 0 || errno == ENOENT || errno == ENOTEMPTY || errno == EEXIST) {
        return 0;
    }
    return -1;
}

int UninstallPergamo(void) {
    if (RemoveLauncherRegistration() != 0) return -1;

    int cleanup_failed = 0;
    const char *files[] = {
        NEW_CFG_FILE,
        APP_LOG_FILE,
        RESTART_PENDING_FILE,
        ICON_FILE,
        FOCUSED_ICON_FILE,
        ICON_TEMP_FILE,
        FOCUSED_ICON_TEMP_FILE,
        VIEW_TEMP_FILE,
        VIEW_BACKUP_TEMP_FILE
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        if (RemoveOwnedFile(files[i]) != 0) cleanup_failed = 1;
    }
    if (RemoveOwnedCacheFiles() != 0) cleanup_failed = 1;

    if (RemoveEmptyDirectory(IMAGES_DIR) != 0) cleanup_failed = 1;
    if (RemoveEmptyDirectory(APP_ROOT_DIR) != 0) cleanup_failed = 1;
    if (RemoveEmptyDirectory(OPDS_LAUNCHER_ICON_DIR) != 0) cleanup_failed = 1;

    if (RemoveOwnedFile(OPDS_APP_BINARY_PATH) != 0) cleanup_failed = 1;

    return cleanup_failed ? 1 : 0;
}
