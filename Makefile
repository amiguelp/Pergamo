# PocketBook SDK 6.3.0 Makefile
SDK_PATH = /SDK/usr
SYSROOT = $(SDK_PATH)/arm-obreey-linux-gnueabi/sysroot

CC = $(SDK_PATH)/bin/arm-obreey-linux-gnueabi-gcc

APP_NAME = Pergamo.app
SRC = main.c network.c parser.c ebiblio.c ui.c launcher_icon.c uninstall.c
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

main.o: launcher_icon.h opds_app.h
ui.o: icons.h uninstall.h opds_app.h
launcher_icon.o: launcher_icon.h
uninstall.o: uninstall.h launcher_icon.h opds_app.h
ebiblio.o: opds_app.h

%.o: %.c
	$(CC) $(CFLAGS) -c $*.c -o $@

launcher_icons.o: launcher_icons.S Pergamo.bmp Pergamo_f.bmp
	$(CC) -c $< -o $@

clean:
	rm -f $(OBJ) $(APP_NAME)
