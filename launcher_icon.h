#ifndef LAUNCHER_ICON_H
#define LAUNCHER_ICON_H

#define OPDS_LAUNCHER_ICON_DIR "/mnt/ext1/applications/icons/"
#define OPDS_APP_BINARY_PATH "/mnt/ext1/applications/Pergamo.app"

extern const unsigned char opds_launcher_icon_bmp[];
extern const unsigned int opds_launcher_icon_bmp_len;
extern const unsigned char opds_launcher_icon_focused_bmp[];
extern const unsigned int opds_launcher_icon_focused_bmp_len;

int InstallLauncherIcon(void);
int RemoveLauncherRegistration(void);

#endif
