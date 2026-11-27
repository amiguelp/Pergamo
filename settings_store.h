#ifndef PERGAMO_SETTINGS_STORE_H
#define PERGAMO_SETTINGS_STORE_H

const char *SettingGet(const char *key, const char *default_value);
long SettingGetLong(const char *key, long default_value);
void SettingSet(const char *key, const char *value);
void SettingSetLong(const char *key, long value);

#endif
