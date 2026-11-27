# Pergamo

> Pergamo is built from [**OPDS Client**](https://github.com/j2robin/Pocketbook-OPDSClient)
> by j2robin, an OPDS catalog client for PocketBook
> e-readers, and extends it with eBiblio Madrid support, a settings screen,
> Spanish/English translations, encrypted credentials and automatic updates.

An OPDS catalog client built in C for PocketBook e-readers. This application allows users to connect to OPDS servers (such as COPS, Calibre-Web, and Project Gutenberg), browse catalogs, search for titles, and download books directly to the device.

## Features

* **E-Ink Optimized UI:** Built with `libinkview`. Includes visual touch feedback (screen inversion) for button presses and interactions.
* **Native Dark Mode Support:** Seamlessly integrates with PocketBook's system-wide Dark Mode (Firmware 6.8+). Book covers are intelligently processed so they retain their original colors while the rest of the interface inverts perfectly.
* **Customizable Layout:** Adjust the catalog view to display between 4 and 10 rows per page via the Server Settings menu. Fonts, thumbnails, and touch targets scale dynamically to ensure comfortable reading and tapping at any size.
* **Robust Configuration:** Settings are safely stored in a text-based file directly in the app's folder (`/mnt/ext1/applications/Pergamo/opds_client.cfg`) to prevent system write-protection issues. The app automatically detects and migrates legacy binary save files.
* **Embedded Assets:** Icons are compiled directly into the binary, requiring no external image files for the base UI. Line-art icons smoothly adapt to Dark Mode.
* **Cache Management:** Maintains a 20MB cache limit for downloaded cover images. The app automatically deletes the oldest thumbnails when this limit is reached.
* **Sort by publication date:** the **ORDENAR** button in the catalog sorts any list of books by publication date (newest or oldest first). OPDS servers such as Calibre-Web only send lists in a fixed order, so Pergamo first loads every page of the list (up to 1,500 books) and sorts it on the reader; books without a known date go last. The choice is remembered for every list.
* **Search Support:** Compatible with standard OPDS search endpoints and OpenSearch. Includes URL encoding to handle multi-word searches on Python-based servers like Calibre-Web.
* **eBiblio Madrid (experimental):** Supports its OPDS 2 catalog and search, account activity, and the reservation/loan actions exposed by eBiblio's feeds. Each operation that changes a loan or reservation asks for confirmation first.
* **Continuous Pagination:** Tracks page numbers across server batches to provide a seamless browsing experience.
* **Network Handling:** Uses connection reuse, automatically resolves relative URLs, and implements 30-second timeouts to handle unresponsive servers without freezing the device.
* **Catalog Diagnostics:** Shows the connection or HTTP failure reason instead of reporting an empty catalog; valid feeds with no entries still show the empty-results message.
* **Downloads:** Books are downloaded directly to the device's storage. After saving a book, the app asks PocketBook's resident scanner to rescan the library; indexing can still take a little time, and some firmware or models may not provide the scanner service.
* **LCP licenses:** eBiblio LCP license files are saved with the `.lcpl` extension and a PocketBook library scan is requested. Open the scanned item from the library so its associated reader can process the license and retrieve the protected EPUB. This depends on the device firmware having an LCP-capable reader/file association.

### eBiblio Madrid

Add a server with URL `https://madrid.ebiblio.es/v1/home.opds2` and enter your
library card number and password in the server's **Usuario** and **Contraseña**
fields. Passwords are never stored in plain text: they are encrypted with
XChaCha20-Poly1305 using a key derived (BLAKE2b) from this reader's serial
number and model, so a copy of the configuration file is useless on another
device. For stronger protection, turn on **Configuración → PIN para las
contraseñas**: the key then also depends on a PIN (Argon2id, 4 MiB) that you type
when opening a server that has a password. Without the PIN, someone holding the
reader itself could still recover the passwords. If you forget the PIN, the app
offers to delete the protected passwords so you can enter them again. Plain-text
passwords from older versions are encrypted automatically on first launch. The
cryptography comes from the bundled [Monocypher](https://monocypher.org) 4.0.3
(`monocypher.c`, CC0/BSD-2), so the app does not depend on the reader's OpenSSL. The OAuth access token is kept in memory only.
Credentials and bearer tokens are sent only to `madrid.ebiblio.es` over HTTPS.

The **Mis préstamos y reservas** screen follows the account links supplied by
eBiblio. Borrowing/reserving, accepting, cancelling, and returning are shown
only when the corresponding action link is present in the feed; open a title's
details to load its publication links and see its available actions. Cancellable
holds use the OPDS `cancellable` property and `DELETE` request. Reservation
details show eBiblio's `availability.until` estimate and hold position when the
feed provides them; these estimates are not guaranteed. Generic download links
without a recognized format are omitted, and duplicate LCP formats are shown
once. If a loan self link contains its loan ID, the app offers return using
eBiblio's `/loans/{id}/return` endpoint with `PUT`. When the detail screen has
more actions and download formats than fit at once, use **Anterior** and
**Siguiente** to page through them. The public catalog reports an item's
availability, but does not expose a member's remaining reservation allowance;
eBiblio validates that limit when the action is submitted.
Download links are also taken from the authenticated feed. Some loans use Adobe
ACSM or LCP DRM; compatibility depends on PocketBook firmware and cannot be
confirmed without testing with an actual account and loan. LCP license files are
checked for a valid license structure, then indexed for opening from the library;
the reader must still support eBiblio's LCP license and encryption profile to
retrieve and open the EPUB.

---

## Installation

1. Download the compiled `Pergamo.app` file.
2. Connect your PocketBook to your computer via USB.
3. Copy `Pergamo.app` into the `applications` folder on your device's internal storage. *(Note: This folder may be hidden by your computer's operating system).*
4. OPTIONAL:  If you want to have a couple of starting servers already configured you can copy the `opds_client.cfg` file into the `applications\Pergamo\` folder. If you haven't run the application yet the `Pergamo` folder won't exist. You will have to create it manually or run the app once and come back and copy this file.
5. Disconnect the device. The app will be available in your PocketBook's "Applications" menu.

## Sync read books with Calibre-Web

For Calibre-Web servers (an OPDS URL ending in `/opds`, with user and password)
the server screen shows **Sincronizar leídos**. Pergamo reads the PocketBook
library database (`system/explorer-3/explorer-3.db`, read-only; books whose
`completed` flag is set) and marks those books as read in Calibre-Web. It also
runs automatically when the app opens (at most once a day, only if Wi-Fi is
already connected, up to 15 books each time); turn this off with
**Configuración → Sincronizar leídos al abrir**.

* **Matching:** books downloaded with Pergamo from Calibre-Web are recorded with
  their Calibre-Web book number (`calibre_links.cfg`). Other books are looked up
  with the OPDS search: the title must match exactly and the author must
  share a name; if several Calibre-Web books match, nothing is changed.
* **Marking:** Pergamo logs in to the web interface once per sync (Calibre-Web
  limits logins), checks the book's *read* checkbox and only then calls
  `/ajax/toggleread`, which toggles the state. It never marks a book as unread.
* Each book's result is stored in `read_sync.cfg` so it is not repeated. The
  manual button also retries books that could not be identified before.
* SQLite is loaded at runtime, so the app still starts on firmwares without it.

## Automatic updates

Once Pergamo 1.2 or later is installed, you no longer need to copy the `.app`
file by hand. When the app opens (at most once a day, and only if Wi-Fi is
already connected) it checks `version.txt` in this repository's `main` branch.
If a newer version is published it asks before installing it. You can also
check at any time from **Configuración → Buscar actualizaciones**, and turn the
automatic check off with **Buscar al abrir**.

The update downloads `Pergamo.app` over HTTPS from GitHub, verifies it against
the BLAKE2b-256 checksum in `Pergamo.app.b2` and checks that it is an ARM
executable before replacing the installed app. The previous version is kept as
`Pergamo.app.old` until the new one starts successfully. If anything fails,
the installed app is left untouched.

### Publishing a new version

1. Bump `PERGAMO_VERSION` in `opds_app.h`.
2. Build the app (`make`).
3. Run `make release` (or `python3 tools/make_release_files.py`) to write
   `version.txt` and `Pergamo.app.b2`.
4. Commit `Pergamo.app`, `version.txt` and `Pergamo.app.b2` together and push
   them to `main`. Readers pick up the update on their next check.

---

## PocketBook Launcher Icon

The launcher icon images are embedded in `Pergamo.app`. On the first launch,
the app extracts them to `/mnt/ext1/applications/icons/` and registers itself in
`/mnt/ext1/system/config/desktop/view.json` (PocketBook firmware 6.8+). It makes
a one-time backup at `view.json.Pergamo.bak` before changing the launcher
configuration. If the icon or launcher entry changes, the app asks whether to
restart the reader; the launcher may keep its old menu and icon cached until
restarted. A pending question is retained if startup is interrupted, and
additional launches are ignored while the app is already starting.

Only the `.app` file needs to be copied to the device. If the firmware does not
provide a compatible `view.json`, the app continues to work with the default
launcher icon. The launcher icons are drawn as line art by
`tools/make_launcher_icons.py` (Python 3 + Pillow) to match the firmware's own
app icons on the Verse Pro: 98 × 98 px with a ~72 px drawing and ~3 px
anti-aliased strokes (calibrated with photos of the launcher: the label is
placed lower the taller the image is; the MobileRead wiki lists 122 × 122 for
other models, which pushed the label lower), saved
as 24-bit BMP (`Pergamo.bmp`, and `Pergamo_f.bmp` for the selected state). The
build embeds both files directly, so regenerating them is picked up by the next
`make`. When an updated app ships a different icon, it replaces the installed
copy and offers to restart the reader so the launcher reloads it. `opds.bmp` is
the original artwork, kept for reference.

## Uninstalling

Choose **Desinstalar** from the main menu and confirm. The app removes its
launcher entry, `Pergamo.app`, its saved server configuration, settings and log, its
numeric `cache_*.jpg` thumbnails, and its two launcher icon files. If the
launcher configuration cannot be safely updated, no files are removed.

Downloaded books in `/mnt/ext1/Downloads/`, other files in the application
folders, and the `view.json.Pergamo.bak` recovery copy are preserved.
`LOGTRIGGER.TXT` is also preserved. If any owned file cannot be removed, the
app reports that the cleanup was incomplete. After removing the launcher
entry, it asks whether to restart so the launcher reloads its menu; choosing
“Después” leaves the reader running, and the menu may remain stale until a
later restart. Choosing **Reiniciar** requests a firmware reboot through
InkView.

---

## Navigation & Controls

The application supports both touchscreen input and hardware button navigation.

### Touchscreen Controls
* **Visual Feedback:** Tapping a book or folder will invert the row color to indicate the touch was registered while the network request processes.
* **Header Navigation:** When browsing a catalog, tap the catalog title at the top of the screen to return directly to the Server Options menu.

### Hardware Button Mappings

| Button | Context: Catalog Browsing | Context: Book Details |
| :--- | :--- | :--- |
| **Next Page** | Load the next page of the catalog. | Scroll down the book summary text. |
| **Prev Page** | Load the previous page. | Scroll up the book summary text. |
| **Menu / Home**| Opens a number pad allowing you to jump to a specific page. Supports "Time Machine" jumping back to previously loaded batches. | *No action* |

---

## Debug Logging

The app logs network requests and errors if a trigger file is present.

**To enable and access logs:**
1. Create an empty file named `LOGTRIGGER.TXT` in the app's installation folder (`/mnt/ext1/applications/Pergamo/`).
2. Run the application and perform the actions you wish to log.
3. Open the `opds_client.log` file on your computer. This file contains `libcurl` network traces, HTTP headers, and redirect information.

I have tested on a Pocketbook ERA and Inkpad Color 3. I made the UI scalable but have not tested on any older or lower resolution devices.

---

## Compiling from Source

This application targets the PocketBook SDK using the `arm-linux-gnueabihf` toolchain. 

It is recommended to build this project using the **[5keeve PocketBook SDK 6.3.0 Docker image](https://github.com/ezdiy/docker-pocketbook-sdk)**. 

Dependencies required to compile:
* `libinkview` (PocketBook UI)
* `libcurl` (Networking)
* `libxml2` (OPDS Parsing)
* `freetype2` (Font Rendering)
* `libjson-c` (Launcher configuration; linked statically)
* `stb_image.h` (Included header for decoding images)

To compile, run:
```bash
make
```

## Customizing Icons (Advanced/Optional)

Note: You do not need to do this to use or compile the app. The default icons are already converted and safely stored inside the `icons.h` file.

If you want to replace the default folder or book icons with your own custom images, a Python helper script is included in the source code.

1. Ensure you have Python 3 installed on your computer.
2. Replace `folder.png` and `book.png` in the project directory with your own standard, 24-bit RGB PNG files. (The app will automatically calculate the aspect ratio for the folder icon, so any standard dimensions will work).
3. Run the conversion script from your terminal:
   ```bash
   python3 image_to_c.py
   ```
    
This will instantly overwrite the `icons.h` file, converting your new images into raw C byte arrays.

Recompile the app using `make`. Your new icons are now permanently embedded in the final `.app` file!

## License

This project is licensed under the MIT License. See the LICENSE file for details.

## Acknowledgments

Portions of this software are copyright © 2026 The FreeType Project (www.freetype.org). All rights reserved.
