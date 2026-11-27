/* Ajustes generales de Pergamo en applications/Pergamo/pergamo_settings.cfg
   (líneas clave=valor). Se leen una vez y se reescriben enteros al cambiar
   un valor, para que cada ajuste no borre los demás. */
#include "settings_store.h"
#include "opds_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SETTINGS_FILE APP_ROOT_DIR "pergamo_settings.cfg"
#define MAX_SETTINGS 32

static char setting_keys[MAX_SETTINGS][64];
static char setting_values[MAX_SETTINGS][256];
static int setting_count = -1;

static void LoadSettingsFile(void) {
    if (setting_count >= 0) return;
    setting_count = 0;
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f) return;
    char line[400];
    while (setting_count < MAX_SETTINGS && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *eq = strchr(line, '=');
        if (!eq || eq == line) continue;
        *eq = '\0';
        snprintf(setting_keys[setting_count], sizeof(setting_keys[0]), "%s", line);
        snprintf(setting_values[setting_count], sizeof(setting_values[0]), "%s", eq + 1);
        setting_count++;
    }
    fclose(f);
}

const char *SettingGet(const char *key, const char *default_value) {
    LoadSettingsFile();
    for (int i = 0; i < setting_count; i++) {
        if (strcmp(setting_keys[i], key) == 0) return setting_values[i];
    }
    return default_value;
}

long SettingGetLong(const char *key, long default_value) {
    const char *value = SettingGet(key, NULL);
    if (!value || !value[0]) return default_value;
    char *end = NULL;
    long number = strtol(value, &end, 10);
    return (end && *end == '\0') ? number : default_value;
}

void SettingSet(const char *key, const char *value) {
    LoadSettingsFile();
    int index = -1;
    for (int i = 0; i < setting_count; i++) {
        if (strcmp(setting_keys[i], key) == 0) { index = i; break; }
    }
    if (index < 0) {
        if (setting_count >= MAX_SETTINGS) return;
        index = setting_count++;
        snprintf(setting_keys[index], sizeof(setting_keys[0]), "%s", key);
    }
    snprintf(setting_values[index], sizeof(setting_values[0]), "%s", value ? value : "");

    mkdir(APP_ROOT_DIR, 0777);
    char tmp[sizeof(SETTINGS_FILE) + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", SETTINGS_FILE);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < setting_count; i++) {
        fprintf(f, "%s=%s\n", setting_keys[i], setting_values[i]);
    }
    if (fclose(f) == 0) rename(tmp, SETTINGS_FILE);
}

void SettingSetLong(const char *key, long value) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%ld", value);
    SettingSet(key, buffer);
}
