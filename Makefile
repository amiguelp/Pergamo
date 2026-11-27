# PocketBook SDK 6.3.0 Makefile
SDK_PATH = /SDK/usr
SYSROOT = $(SDK_PATH)/arm-obreey-linux-gnueabi/sysroot

CC = $(SDK_PATH)/bin/arm-obreey-linux-gnueabi-gcc

APP_NAME = Pergamo.app
SRC = main.c network.c parser.c ebiblio.c ui.c launcher_icon.c uninstall.c i18n.c secrets.c monocypher.c settings_store.c updater.c calibre_sync.c
OBJ = $(SRC:.c=.o) launcher_icons.o

# LDFLAGS: Changed -lft2 to -lfreetype
# Added explicit path to the sysroot library folder where libfreetype.so lives
LDFLAGS = -L$(SYSROOT)/usr/lib \
          -linkview -lcurl -lxml2 -lfreetype \
          -Wl,-Bstatic -ljson-c -Wl,-Bdynamic -lm -ldl

CFLAGS = -Wall -O2 \
         -I$(SYSROOT)/usr/include \
         -I$(SYSROOT)/usr/include/libxml2 \
         -I$(SYSROOT)/usr/include/freetype2

all: $(APP_NAME)

$(APP_NAME): $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

main.o: launcher_icon.h opds_app.h i18n.h secrets.h updater.h
ui.o: icons.h uninstall.h opds_app.h i18n.h secrets.h settings_store.h updater.h calibre_sync.h
launcher_icon.o: launcher_icon.h
uninstall.o: uninstall.h launcher_icon.h opds_app.h
ebiblio.o: opds_app.h i18n.h
i18n.o: i18n.h opds_app.h
secrets.o: secrets.h monocypher.h
settings_store.o: settings_store.h opds_app.h
updater.o: updater.h opds_app.h launcher_icon.h monocypher.h
calibre_sync.o: calibre_sync.h opds_app.h
i18n.o: settings_store.h
network.o: opds_app.h i18n.h

%.o: %.c
	$(CC) $(CFLAGS) -c $*.c -o $@

launcher_icons.o: launcher_icons.S Pergamo.bmp Pergamo_f.bmp
	$(CC) -c $< -o $@

clean:
	rm -f $(OBJ) $(APP_NAME)

# Genera version.txt y Pergamo.app.b2 para la actualización automática.
release: $(APP_NAME)
	python3 tools/make_release_files.py

.PHONY: all clean release
