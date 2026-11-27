#include "launcher_icon.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LAUNCHER_PATH_LEN 512
#define LAUNCHER_VIEW_FILE "/mnt/ext1/system/config/desktop/view.json"
#define LAUNCHER_VIEW_BACKUP "/mnt/ext1/system/config/desktop/view.json.Pergamo.bak"
#define LAUNCHER_ENTRY_ID "U_Pergamo"
#define LEGACY_LAUNCHER_ENTRY_ID "U_OPDSClient"
#define LAUNCHER_APP_PATH OPDS_APP_BINARY_PATH
#define LAUNCHER_ICON_PATH OPDS_LAUNCHER_ICON_DIR "Pergamo.bmp"
#define LAUNCHER_FOCUSED_ICON_PATH OPDS_LAUNCHER_ICON_DIR "Pergamo_f.bmp"

extern void LogDebug(const char *msg);
static int RemoveEntryFromGroup(struct json_object *group, const char *entry_id);

static int WriteAtomically(const char *path, const void *data, size_t size, mode_t mode) {
    char temp_path[LAUNCHER_PATH_LEN];
    if (snprintf(temp_path, sizeof(temp_path), "%s.opdsclient.tmp", path) >= (int)sizeof(temp_path)) {
        return -1;
    }

    int fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return -1;

    FILE *file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
        unlink(temp_path);
        return -1;
    }

    int ok = fwrite(data, 1, size, file) == size && fflush(file) == 0;
    if (ok && fsync(fd) != 0) {
        int sync_error = errno;
        /* Some PocketBook storage filesystems do not support fsync. */
        if (sync_error != EINVAL && sync_error != ENOSYS) ok = 0;
    }
    if (fclose(file) != 0) ok = 0;
    if (!ok || rename(temp_path, path) != 0) {
        unlink(temp_path);
        return -1;
    }
    return 0;
}

/* Instala el icono si falta o si es distinto del incluido en la app (p. ej.
   tras actualizar Pergamo). Devuelve 1 si lo escribió, 0 si ya estaba igual. */
static int WriteAssetIfChanged(const char *path, const unsigned char *data, size_t size) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (!S_ISREG(st.st_mode)) return -1;
        if ((size_t)st.st_size == size) {
            FILE *file = fopen(path, "rb");
            if (file) {
                int same = 1;
                unsigned char buffer[4096];
                size_t offset = 0, n;
                while (same && (n = fread(buffer, 1, sizeof(buffer), file)) > 0) {
                    if (offset + n > size || memcmp(buffer, data + offset, n) != 0) same = 0;
                    offset += n;
                }
                fclose(file);
                if (same && offset == size) return 0;
            }
        }
    } else if (errno != ENOENT) {
        return -1;
    }
    return WriteAtomically(path, data, size, 0644) == 0 ? 1 : -1;
}

static int BackupViewFile(const unsigned char *data, size_t size, mode_t mode) {
    struct stat st;
    if (stat(LAUNCHER_VIEW_BACKUP, &st) == 0) {
        return S_ISREG(st.st_mode) ? 0 : -1;
    }
    if (errno != ENOENT) return -1;
    return WriteAtomically(LAUNCHER_VIEW_BACKUP, data, size, mode & 0777);
}

static int SaveViewFile(struct json_object *root, const unsigned char *original,
                        size_t original_size, mode_t mode) {
    const char *serialized = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY);
    if (!serialized || BackupViewFile(original, original_size, mode) != 0) {
        LogDebug("Launcher configuration update failed: could not create the view.json backup.");
        return -1;
    }

    size_t serialized_size = strlen(serialized);
    char *output = (char *)malloc(serialized_size + 1);
    if (!output) {
        LogDebug("Launcher configuration update failed: out of memory.");
        return -1;
    }
    memcpy(output, serialized, serialized_size);
    output[serialized_size] = '\n';

    int result = WriteAtomically(LAUNCHER_VIEW_FILE, output, serialized_size + 1,
                                 mode & 0777);
    free(output);
    if (result != 0) {
        LogDebug("Launcher configuration update failed: could not write view.json.");
        return -1;
    }
    return 0;
}

static int ReadViewFile(unsigned char **data, size_t *size, mode_t *mode) {
    struct stat st;
    if (stat(LAUNCHER_VIEW_FILE, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size <= 0 || st.st_size > 1024 * 1024) {
        return -1;
    }

    FILE *file = fopen(LAUNCHER_VIEW_FILE, "rb");
    if (!file) return -1;

    size_t length = (size_t)st.st_size;
    unsigned char *buffer = (unsigned char *)malloc(length + 2);
    if (!buffer) {
        fclose(file);
        return -1;
    }

    if (fread(buffer, 1, length, file) != length || ferror(file) ||
        memchr(buffer, '\0', length) != NULL) {
        free(buffer);
        fclose(file);
        return -1;
    }
    fclose(file);

    buffer[length] = ' ';
    buffer[length + 1] = '\0';
    *data = buffer;
    *size = length;
    *mode = st.st_mode;
    return 0;
}

static int ParseViewContents(const unsigned char *contents, size_t contents_size,
                             struct json_object **root_out) {
    struct json_tokener *tokener = json_tokener_new();
    if (!tokener) {
        LogDebug("Launcher configuration error: cannot allocate JSON parser.");
        return -1;
    }

    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT);
    struct json_object *root = json_tokener_parse_ex(tokener, (const char *)contents,
                                                      (int)(contents_size + 1));
    enum json_tokener_error parse_error = json_tokener_get_error(tokener);
    int consumed = tokener->char_offset;
    int parse_ok = parse_error == json_tokener_success && root &&
                   json_object_get_type(root) == json_type_object;
    for (size_t i = consumed < 0 ? 0 : (size_t)consumed; parse_ok && i < contents_size + 1; i++) {
        if (!isspace(contents[i])) parse_ok = 0;
    }
    json_tokener_free(tokener);

    if (!parse_ok) {
        if (root) json_object_put(root);
        char message[192];
        if (parse_error != json_tokener_success) {
            snprintf(message, sizeof(message), "Launcher configuration error: view.json parse error: %s.",
                     json_tokener_error_desc(parse_error));
        } else {
            snprintf(message, sizeof(message),
                     "Launcher configuration error: view.json is not a valid JSON object (offset %d).",
                     consumed);
        }
        LogDebug(message);
        return -1;
    }

    *root_out = root;
    return 0;
}

static int SetStringProperty(struct json_object *object, const char *key, const char *value) {
    struct json_object *current = NULL;
    if (json_object_object_get_ex(object, key, &current) &&
        json_object_get_type(current) == json_type_string &&
        strcmp(json_object_get_string(current), value) == 0) {
        return 0;
    }

    struct json_object *replacement = json_object_new_string(value);
    if (!replacement) return -1;
    json_object_object_add(object, key, replacement);
    return 1;
}

static int GroupContainsEntry(struct json_object *groups, const char *entry_id) {
    int count = json_object_array_length(groups);
    for (int i = 0; i < count; i++) {
        struct json_object *group = json_object_array_get_idx(groups, i);
        struct json_object *apps = NULL;
        if (!group || json_object_get_type(group) != json_type_object) return -1;
        if (!json_object_object_get_ex(group, "apps", &apps)) continue;
        if (!apps || json_object_get_type(apps) != json_type_array) return -1;

        int app_count = json_object_array_length(apps);
        for (int j = 0; j < app_count; j++) {
            struct json_object *app = json_object_array_get_idx(apps, j);
            if (app && json_object_get_type(app) == json_type_string &&
                strcmp(json_object_get_string(app), entry_id) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

static int AddEntryToFirstGroup(struct json_object *groups, const char *entry_id) {
    if (json_object_array_length(groups) <= 0) return -1;

    struct json_object *group = json_object_array_get_idx(groups, 0);
    if (!group || json_object_get_type(group) != json_type_object) return -1;

    struct json_object *apps = NULL;
    if (!json_object_object_get_ex(group, "apps", &apps)) {
        apps = json_object_new_array();
        if (!apps) return -1;
        json_object_object_add(group, "apps", apps);
    } else if (!apps || json_object_get_type(apps) != json_type_array) {
        return -1;
    }

    struct json_object *entry = json_object_new_string(entry_id);
    if (!entry) return -1;
    if (json_object_array_add(apps, entry) != 0) {
        json_object_put(entry);
        return -1;
    }
    return 0;
}

int InstallLauncherIcon(void) {
    struct stat view_stat;
    if (stat(LAUNCHER_VIEW_FILE, &view_stat) != 0) {
        if (errno == ENOENT) {
            LogDebug("Launcher icon setup skipped: this firmware has no view.json.");
            return 0;
        }
        LogDebug("Launcher icon setup failed: cannot inspect view.json.");
        return -1;
    }

    if (mkdir(OPDS_LAUNCHER_ICON_DIR, 0777) != 0 && errno != EEXIST) {
        LogDebug("Launcher icon setup failed: cannot create applications/icons.");
        return -1;
    }
    struct stat icon_dir_stat;
    if (stat(OPDS_LAUNCHER_ICON_DIR, &icon_dir_stat) != 0 ||
        !S_ISDIR(icon_dir_stat.st_mode)) {
        LogDebug("Launcher icon setup failed: applications/icons is not a directory.");
        return -1;
    }

    int icons_changed = 0;
    int result = WriteAssetIfChanged(LAUNCHER_ICON_PATH, opds_launcher_icon_bmp,
                                    opds_launcher_icon_bmp_len);
    if (result < 0) {
        LogDebug("Launcher icon setup failed: cannot install icon image files.");
        return -1;
    }
    icons_changed |= result;

    result = WriteAssetIfChanged(LAUNCHER_FOCUSED_ICON_PATH,
                                 opds_launcher_icon_focused_bmp,
                                 opds_launcher_icon_focused_bmp_len);
    if (result < 0) {
        LogDebug("Launcher icon setup failed: cannot install icon image files.");
        return -1;
    }
    icons_changed |= result;

    unsigned char *contents = NULL;
    size_t contents_size = 0;
    mode_t view_mode = 0;
    if (ReadViewFile(&contents, &contents_size, &view_mode) != 0) {
        LogDebug("Launcher icon setup failed: cannot read view.json.");
        return -1;
    }

    struct json_object *root = NULL;
    if (ParseViewContents(contents, contents_size, &root) != 0) {
        free(contents);
        return -1;
    }

    struct json_object *applications = NULL;
    struct json_object *groups = NULL;
    struct json_object *view = NULL;
    int changed = 0;

    if (!json_object_object_get_ex(root, "applications", &applications)) {
        applications = json_object_new_object();
        if (!applications) changed = -1;
        else {
            json_object_object_add(root, "applications", applications);
            changed = 1;
        }
    } else if (!applications || json_object_get_type(applications) != json_type_object) {
        changed = -1;
    }

    struct json_object *application = NULL;
    if (changed >= 0 &&
        json_object_object_get_ex(applications, LEGACY_LAUNCHER_ENTRY_ID, &application)) {
        json_object_object_del(applications, LEGACY_LAUNCHER_ENTRY_ID);
        changed = 1;
    }

    application = NULL;
    if (changed >= 0 &&
        json_object_object_get_ex(applications, LAUNCHER_ENTRY_ID, &application) &&
        (!application || json_object_get_type(application) != json_type_object)) {
        changed = -1;
    }
    if (changed >= 0 && !application) {
        application = json_object_new_object();
        if (!application) changed = -1;
        else {
            json_object_object_add(applications, LAUNCHER_ENTRY_ID, application);
            changed = 1;
        }
    }

    const char *keys[] = {"path", "title", "icon", "focused_icon"};
    const char *values[] = {LAUNCHER_APP_PATH, "Pergamo", LAUNCHER_ICON_PATH,
                            LAUNCHER_FOCUSED_ICON_PATH};
    for (int i = 0; changed >= 0 && i < 4; i++) {
        int result = SetStringProperty(application, keys[i], values[i]);
        if (result < 0) changed = -1;
        else if (result > 0) changed = 1;
    }

    if (changed >= 0 &&
        (!json_object_object_get_ex(root, "view", &view) ||
         !view || json_object_get_type(view) != json_type_object ||
         !json_object_object_get_ex(view, "groups", &groups) ||
         !groups || json_object_get_type(groups) != json_type_array)) {
        changed = -1;
    }

    if (changed >= 0) {
        int legacy_group_count = json_object_array_length(groups);
        for (int i = 0; i < legacy_group_count; i++) {
            struct json_object *legacy_group = json_object_array_get_idx(groups, i);
            if (legacy_group && json_object_get_type(legacy_group) == json_type_object) {
                int result = RemoveEntryFromGroup(legacy_group, LEGACY_LAUNCHER_ENTRY_ID);
                if (result < 0) changed = -1;
                else if (result > 0) changed = 1;
            }
        }

        int in_group = GroupContainsEntry(groups, LAUNCHER_ENTRY_ID);
        if (in_group < 0) changed = -1;
        else if (!in_group && AddEntryToFirstGroup(groups, LAUNCHER_ENTRY_ID) != 0) changed = -1;
        else if (!in_group) changed = 1;
    }

    if (changed > 0 && SaveViewFile(root, contents, contents_size, view_mode) != 0) changed = -1;

    json_object_put(root);
    free(contents);

    if (changed < 0) return -1;
    if (changed > 0) LogDebug("Launcher icon registered successfully.");
    return (icons_changed || changed > 0) ? 1 : 0;
}

static int RemoveEntryFromGroup(struct json_object *group, const char *entry_id) {
    struct json_object *apps = NULL;
    if (!json_object_object_get_ex(group, "apps", &apps)) return 0;
    if (!apps || json_object_get_type(apps) != json_type_array) return -1;

    int count = json_object_array_length(apps);
    int found = 0;
    for (int i = 0; i < count; i++) {
        struct json_object *app = json_object_array_get_idx(apps, i);
        if (app && json_object_get_type(app) == json_type_string &&
            strcmp(json_object_get_string(app), entry_id) == 0) {
            found = 1;
            break;
        }
    }
    if (!found) return 0;

    struct json_object *replacement = json_object_new_array();
    if (!replacement) return -1;
    for (int i = 0; i < count; i++) {
        struct json_object *app = json_object_array_get_idx(apps, i);
        if (app && json_object_get_type(app) == json_type_string &&
            strcmp(json_object_get_string(app), entry_id) == 0) {
            continue;
        }
        struct json_object *copy = app ? json_object_get(app) : NULL;
        if (json_object_array_add(replacement, copy) != 0) {
            if (copy) json_object_put(copy);
            json_object_put(replacement);
            return -1;
        }
    }

    json_object_object_add(group, "apps", replacement);
    return 1;
}

int RemoveLauncherRegistration(void) {
    struct stat view_stat;
    if (stat(LAUNCHER_VIEW_FILE, &view_stat) != 0) {
        if (errno == ENOENT) return 0;
        LogDebug("Uninstall failed: cannot inspect view.json.");
        return -1;
    }

    unsigned char *contents = NULL;
    size_t contents_size = 0;
    mode_t view_mode = 0;
    if (ReadViewFile(&contents, &contents_size, &view_mode) != 0) {
        LogDebug("Uninstall failed: cannot read view.json.");
        return -1;
    }

    struct json_object *root = NULL;
    if (ParseViewContents(contents, contents_size, &root) != 0) {
        free(contents);
        return -1;
    }

    int changed = 0;
    struct json_object *applications = NULL;
    struct json_object *application = NULL;
    if (json_object_object_get_ex(root, "applications", &applications)) {
        if (!applications || json_object_get_type(applications) != json_type_object) {
            changed = -1;
        } else {
            if (json_object_object_get_ex(applications, LAUNCHER_ENTRY_ID, &application)) {
                json_object_object_del(applications, LAUNCHER_ENTRY_ID);
                changed = 1;
            }
            if (json_object_object_get_ex(applications, LEGACY_LAUNCHER_ENTRY_ID, &application)) {
                json_object_object_del(applications, LEGACY_LAUNCHER_ENTRY_ID);
                changed = 1;
            }
        }
    }

    struct json_object *view = NULL;
    struct json_object *groups = NULL;
    if (changed >= 0 && json_object_object_get_ex(root, "view", &view)) {
        if (!view || json_object_get_type(view) != json_type_object) {
            changed = -1;
        } else if (json_object_object_get_ex(view, "groups", &groups)) {
            if (!groups || json_object_get_type(groups) != json_type_array) {
                changed = -1;
            } else {
                int group_count = json_object_array_length(groups);
                for (int i = 0; changed >= 0 && i < group_count; i++) {
                    struct json_object *group = json_object_array_get_idx(groups, i);
                    if (!group || json_object_get_type(group) != json_type_object) {
                        changed = -1;
                        break;
                    }
                    int result = RemoveEntryFromGroup(group, LAUNCHER_ENTRY_ID);
                    if (result < 0) changed = -1;
                    else if (result > 0) changed = 1;
                    result = RemoveEntryFromGroup(group, LEGACY_LAUNCHER_ENTRY_ID);
                    if (result < 0) changed = -1;
                    else if (result > 0) changed = 1;
                }
            }
        }
    }

    if (changed > 0 && SaveViewFile(root, contents, contents_size, view_mode) != 0) {
        changed = -1;
    }
    json_object_put(root);
    free(contents);

    if (changed < 0) {
        LogDebug("Uninstall failed: could not safely remove the launcher registration.");
        return -1;
    }
    return 0;
}
