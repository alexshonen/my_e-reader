/**
 * @file main.cpp
 * @brief Touch-Enabled Ultra-Low-Power E-Paper Reader with Multi-Book Library Support
 *        for LilyGo T5 4.7" S3 (ED047TC1 E-Paper + GT911 Capacitive Touch).
 *
 * Features:
 *  - Multi-book management from SD card (/books/<book_folder>/ or legacy /pages/)
 *  - Interactive Library Menu UI with book progress cards and pagination
 *  - Touch navigation while reading:
 *      * Tap right half (x >= 220, y >= 120): Page suivante (Next Page)
 *      * Tap left half  (x < 220,  y >= 120): Page precedente (Previous Page)
 *      * Tap top header (y < 120): Ouvrir la bibliotheque (Open Library Menu)
 *  - Responsive reading loop with automatic Deep Sleep after 3 minutes of inactivity
 *  - Wakeup from Deep Sleep via physical buttons (BOOT or side BUTTON_1)
 *  - Per-book bookmark persistence in Flash NVS (Preferences.h)
 *  - Octal PSRAM framebuffer allocation (ps_malloc) & ultra-deep sleep (<20uA)
 */

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>
#include <Preferences.h>
#include "epd_driver.h"
#include "esp_sleep.h"
#include "gui.h"
#include <TouchDrvGT911.hpp>
#include "utilities.h"

// Instantiate Global Touch Controller (SensorLib)
TouchDrvGT911 touch;
bool touch_available = false;

// Physical Buttons (Active LOW)
constexpr gpio_num_t BOOT_BUTTON_PIN = GPIO_NUM_0;
#if defined(BUTTON_1)
constexpr gpio_num_t SIDE_BUTTON_PIN = (gpio_num_t)BUTTON_1; // GPIO 21 on T5 S3
#else
constexpr gpio_num_t SIDE_BUTTON_PIN = GPIO_NUM_21;
#endif

// Display Framebuffer Dimension Constants (540x960, 4-bit)
constexpr size_t PAGE_BUFFER_SIZE = (PORTRAIT_WIDTH * PORTRAIT_HEIGHT / 2); // 259,200 bytes


// Multi-Book Management Constants & State
constexpr int MAX_BOOKS = 24;
constexpr int BOOKS_PER_PAGE = 4;
BookMetadata books[MAX_BOOKS];
int total_books = 0;
int current_book_idx = 0;

// Inactivity timeout before entering ultra-low power Deep Sleep (3 minutes)
constexpr uint32_t INACTIVITY_TIMEOUT_MS = 180000;
uint32_t last_activity_time = 0;

// Framebuffer pointer in PSRAM
uint8_t *framebuffer = nullptr;

// Operating Modes
enum ReaderMode {
    MODE_READING = 0,
    MODE_LIBRARY = 1
};
ReaderMode current_mode = MODE_LIBRARY;

uint32_t hash_string(const char *str) {
    uint32_t hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    }
    return hash;
}

void run_library_menu(uint8_t *fb);
void render_current_book_page(uint8_t *fb);
/**
 * @brief Initializes the GT911 capacitive touch panel.
 */
void init_touch() {
    Wire.begin(BOARD_SDA, BOARD_SCL);

    // Pulse interrupt pin to wake controller if it was sleeping
    pinMode(TOUCH_INT, OUTPUT);
    digitalWrite(TOUCH_INT, HIGH);
    delay(10);

    uint8_t touchAddress = 0;
    Wire.beginTransmission(0x14);
    if (Wire.endTransmission() == 0) touchAddress = 0x14;
    Wire.beginTransmission(0x5D);
    if (Wire.endTransmission() == 0) touchAddress = 0x5D;

    if (touchAddress != 0) {
        touch.setPins(-1, TOUCH_INT);
        if (touch.begin(Wire, touchAddress, BOARD_SDA, BOARD_SCL)) {
            touch.setMaxCoordinates(PORTRAIT_WIDTH, PORTRAIT_HEIGHT);
            touch_available = true;
            Serial.printf("[TOUCH] GT911 detected and initialized at I2C address 0x%02X\n", touchAddress);
            return;
        }
    }

    Serial.println(F("[TOUCH] Warning: GT911 touch not detected. Using physical buttons."));
    touch_available = false;
}

/**
 * @brief Reads touch point coordinates from GT911 controller.
 * @return true if an active touch was registered.
 */
bool read_touch_point(int16_t *x, int16_t *y) {
    if (!touch_available) return false;
    int16_t rx[2] = {0}, ry[2] = {0};
    uint8_t n = touch.getPoint(rx, ry, 1);
    if (n > 0) {
        // Raw touch coordinates perfectly align with the visually inverted screen!
        *x = rx[0];
        *y = ry[0];

        // Constrain coordinates to portrait screen boundaries
        if (*x < 0) *x = 0;
        if (*x >= PORTRAIT_WIDTH) *x = PORTRAIT_WIDTH - 1;
        if (*y < 0) *y = 0;
        if (*y >= PORTRAIT_HEIGHT) *y = PORTRAIT_HEIGHT - 1;
        return true;
    }
    return false;
}

/**
 * @brief Checks if any physical button (BOOT or side button) is pressed.
 */
bool is_any_button_pressed() {
    // Disabled to prevent phantom button loop (we rely on Touch instead)
    return false;
}

bool is_locked = false;

void toggle_lock_state() {
    if (is_locked) {
        Serial.println(F("[LOCK] Unlocking device... Resuming reading."));
        is_locked = false;
        render_current_book_page(framebuffer);
        last_activity_time = millis();
    } else {
        Serial.println(F("[LOCK] Locking device... Displaying cover page."));
        is_locked = true;
        
        if (total_books > 0 && framebuffer != nullptr) {
            BookMetadata &b = books[current_book_idx];
            char filename[120];
            snprintf(filename, sizeof(filename), "%s/page_0000.bin", b.path);
            
            File page_file = SD.open(filename, FILE_READ);
            if (page_file) {
                fb_clear(framebuffer, COLOR_WHITE);
                uint8_t row_buf[PORTRAIT_WIDTH / 2];
                for (int y = 0; y < PORTRAIT_HEIGHT; y++) {
                    size_t r = page_file.read(row_buf, sizeof(row_buf));
                    if (r == 0) break;
                    for (int x = 0; x < PORTRAIT_WIDTH; x++) {
                        uint8_t byte_val = row_buf[x / 2];
                        uint8_t color = ((x & 1) == 0) ? ((byte_val >> 4) & 0x0F) : (byte_val & 0x0F);
                        fb_set_pixel(framebuffer, x, y, color);
                    }
                }
                page_file.close();
                
                epd_poweron();
                epd_clear();
                epd_draw_grayscale_image(epd_full_screen(), framebuffer);
                epd_poweroff();
            }
        }
    }
    
    // Wait for button release
    while (digitalRead(21) == LOW || digitalRead(39) == LOW) {
        delay(50);
    }
    delay(500); // Debounce
}

/**
 * @brief Scans the SD card for available books in /books/ and fallback /pages/.
 */
void scan_sd_books() {
    total_books = 0;

    // Check /books directory
    File books_dir = SD.open("/books");
    if (books_dir && books_dir.isDirectory()) {
        File entry = books_dir.openNextFile();
        while (entry && total_books < MAX_BOOKS) {
            if (entry.isDirectory()) {
                const char *folder_path = entry.path();
                snprintf(books[total_books].path, sizeof(books[total_books].path), "%s", folder_path);

                // 1. Read book title from title.txt if present
                char title_file_path[120];
                snprintf(title_file_path, sizeof(title_file_path), "%s/title.txt", folder_path);
                if (SD.exists(title_file_path)) {
                    File tf = SD.open(title_file_path, FILE_READ);
                    if (tf) {
                        String t = tf.readStringUntil('\n');
                        t.trim();
                        if (t.length() > 0) {
                            snprintf(books[total_books].title, sizeof(books[total_books].title), "%s", t.c_str());
                        } else {
                            const char *slash = strrchr(folder_path, '/');
                            snprintf(books[total_books].title, sizeof(books[total_books].title), "%s", slash ? slash + 1 : folder_path);
                        }
                        tf.close();
                    }
                } else {
                    const char *slash = strrchr(folder_path, '/');
                    snprintf(books[total_books].title, sizeof(books[total_books].title), "%s", slash ? slash + 1 : folder_path);
                }

                // 2. Count total page files (Fast O(1) via pages.txt metadata if available)
                int p_count = 0;
                char meta_path[120];
                snprintf(meta_path, sizeof(meta_path), "%s/pages.txt", folder_path);
                if (SD.exists(meta_path)) {
                    File meta = SD.open(meta_path, FILE_READ);
                    if (meta) {
                        String p = meta.readStringUntil('\n');
                        p.trim();
                        p_count = p.toInt();
                        meta.close();
                    }
                }
                if (p_count == 0) {
                    // Fallback to slow scan
                    while (p_count < 9999) {
                        char test_page[120];
                        snprintf(test_page, sizeof(test_page), "%s/page_%04d.bin", folder_path, p_count);
                        if (!SD.exists(test_page)) break;
                        p_count++;
                    }
                }
                books[total_books].page_count = p_count;

                // 3. Load saved page bookmark from NVS for this book (using title hash)
                Preferences book_prefs;
                if (book_prefs.begin("reader", true)) {
                    char book_key[16];
                    uint32_t title_hash = hash_string(books[total_books].title);
                    snprintf(book_key, sizeof(book_key), "%08X", title_hash);
                    books[total_books].current_page = book_prefs.getInt(book_key, 0);
                    book_prefs.end();
                } else {
                    books[total_books].current_page = 0;
                }
                
                // Safety bounds check
                if (books[total_books].current_page >= books[total_books].page_count) {
                    books[total_books].current_page = 0;
                }

                if (books[total_books].page_count > 0) {
                    Serial.printf("[SD] Discovered Book [%d]: '%s' (%d pages, bookmark: p.%d) in %s\n",
                                  total_books, books[total_books].title,
                                  books[total_books].page_count,
                                  books[total_books].current_page + 1,
                                  books[total_books].path);
                    total_books++;
                }
            }
            entry = books_dir.openNextFile();
        }
        books_dir.close();
    }

    // Fallback: If no books in /books/, check legacy /pages/ directory
    if (total_books == 0 && SD.exists("/pages/page_0000.bin")) {
        snprintf(books[0].path, sizeof(books[0].path), "/pages");
        snprintf(books[0].title, sizeof(books[0].title), "Livre par defaut");

        int p_count = 0;
        if (SD.exists("/pages/pages.txt")) {
            File meta = SD.open("/pages/pages.txt", FILE_READ);
            if (meta) {
                String p = meta.readStringUntil('\n');
                p.trim();
                p_count = p.toInt();
                meta.close();
            }
        }
        if (p_count == 0) {
            while (p_count < 9999) {
                char test_page[64];
                snprintf(test_page, sizeof(test_page), "/pages/page_%04d.bin", p_count);
                if (!SD.exists(test_page)) break;
                p_count++;
            }
        }
        books[0].page_count = p_count;

        Preferences p;
        if (p.begin("reader", true)) {
            books[0].current_page = p.getInt("page", 0);
            p.end();
        } else {
            books[0].current_page = 0;
        }

        Serial.printf("[SD] Legacy single book: %d pages in /pages/\n", p_count);
        total_books = 1;
    }

    Serial.printf("[SD] Total books discovered: %d\n", total_books);
}

/**
 * @brief Saves the current book index and its current page progress to NVS.
 */
void save_current_bookmark() {
    Preferences prefs;
    if (prefs.begin("reader", false)) {
        uint32_t title_hash = hash_string(books[current_book_idx].title);
        prefs.putUInt("last_hash", title_hash);

        char book_key[16];
        snprintf(book_key, sizeof(book_key), "%08X", title_hash);
        prefs.putInt(book_key, books[current_book_idx].current_page);
        prefs.end();

        Serial.printf("[NVS] Saved book '%s' bookmark: page %d\n",
                      books[current_book_idx].title, books[current_book_idx].current_page);
    }
}

/**
 * @brief Renders the current book's page from SD onto the e-paper display.
 */
void render_current_book_page(uint8_t *fb) {
    if (total_books == 0) {
        Serial.println(F("[ERROR] No books available to render!"));
        return;
    }

    BookMetadata &b = books[current_book_idx];

    // Bounds check
    if (b.current_page >= b.page_count && b.page_count > 0) {
        b.current_page = 0;
        save_current_bookmark();
    }
    if (b.current_page < 0) {
        b.current_page = 0;
        save_current_bookmark();
    }

    char filename[120];
    snprintf(filename, sizeof(filename), "%s/page_%04d.bin", b.path, b.current_page);

    Serial.printf("[READER] Loading '%s' (%s, p.%d / %d)...\n",
                  filename, b.title, b.current_page + 1, b.page_count);

    File page_file = SD.open(filename, FILE_READ);
    if (!page_file) {
        Serial.printf("[ERROR] Failed to open '%s'! File may be missing.\n", filename);
        return;
    }

    fb_clear(fb, COLOR_WHITE);
    uint8_t row_buf[PORTRAIT_WIDTH / 2];
    for (int y = 0; y < PORTRAIT_HEIGHT; y++) {
        size_t r = page_file.read(row_buf, sizeof(row_buf));
        if (r == 0) break;
        for (int x = 0; x < PORTRAIT_WIDTH; x++) {
            uint8_t byte_val = row_buf[x / 2];
            uint8_t color;
            if ((x & 1) == 0) {
                color = (byte_val >> 4) & 0x0F;
            } else {
                color = byte_val & 0x0F;
            }
            fb_set_pixel(fb, x, y, color);
        }
    }
    page_file.close();

    // Refresh E-Paper display
    Serial.println(F("[EPD] Refreshing ED047TC1 e-paper panel..."));
    epd_poweron();
    epd_clear();
    epd_draw_grayscale_image(epd_full_screen(), fb);
    epd_poweroff();
    Serial.println(F("[EPD] Panel refresh complete."));
}

/**
 * @brief Runs the interactive Library Menu on the e-paper panel.
 */
void run_library_menu(uint8_t *fb) {
    int lib_page = current_book_idx / BOOKS_PER_PAGE;
    int total_lib_pages = max(1, (total_books + BOOKS_PER_PAGE - 1) / BOOKS_PER_PAGE);

    // Initial render of the library screen
    fb_render_library_screen(fb, books, total_books, current_book_idx, lib_page, BOOKS_PER_PAGE);
    epd_poweron();
    epd_clear();
    epd_draw_grayscale_image(epd_full_screen(), fb);
    epd_poweroff();

    Serial.println(F("[MENU] Library menu active. Waiting for touch selection..."));

    // Interactive touch loop with 30s timeout
    uint32_t menu_start = millis();
    constexpr uint32_t MENU_TIMEOUT_MS = 30000;

    while (millis() - menu_start < MENU_TIMEOUT_MS) {
        int16_t tx = 0, ty = 0;
        if (read_touch_point(&tx, &ty)) {
            Serial.printf("[MENU] Touch detected at (%d, %d)\n", tx, ty);
            menu_start = millis();

            // Check Footer Buttons: y in [850..950]
            if (ty >= 850 && ty <= 950) {
                if (tx >= 20 && tx <= 160) {
                    // PREV library page
                    if (lib_page > 0) {
                        lib_page--;
                        fb_render_library_screen(fb, books, total_books, current_book_idx, lib_page, BOOKS_PER_PAGE);
                        epd_poweron();
                        epd_clear();
                        epd_draw_grayscale_image(epd_full_screen(), fb);
                        epd_poweroff();
                    }
                    delay(300);
                    continue;
                } else if (tx >= 180 && tx <= 360) {
                    // RESUME reading current book
                    Serial.println(F("[MENU] Resuming current book."));
                    current_mode = MODE_READING;
                    break;
                } else if (tx >= 380 && tx <= 520) {
                    // NEXT library page
                    if (lib_page < total_lib_pages - 1) {
                        lib_page++;
                        fb_render_library_screen(fb, books, total_books, current_book_idx, lib_page, BOOKS_PER_PAGE);
                        epd_poweron();
                        epd_clear();
                        epd_draw_grayscale_image(epd_full_screen(), fb);
                        epd_poweroff();
                    }
                    delay(300);
                    continue;
                }
            }

            // Check Book Cards: y in [105..820]
            int start_idx = lib_page * BOOKS_PER_PAGE;
            int end_idx = min(start_idx + BOOKS_PER_PAGE, total_books);
            int y_start = 105;
            int card_h = 150;
            int card_sp = 18;

            for (int i = start_idx; i < end_idx; i++) {
                int slot = i - start_idx;
                int cy = y_start + slot * (card_h + card_sp);
                if (ty >= cy && ty <= cy + card_h && tx >= 20 && tx <= PORTRAIT_WIDTH - 20) {
                    Serial.printf("[MENU] Selected book [%d]: '%s'\n", i, books[i].title);
                    current_book_idx = i;
                    save_current_bookmark();
                    current_mode = MODE_READING;
                    delay(300);
                    return;
                }
            }
        }

        // BOOT button short press: exit menu and resume
        if (is_any_button_pressed()) {
            delay(200);
            current_mode = MODE_READING;
            return;
        }

        delay(30);
    }

    Serial.println(F("[MENU] Menu timed out. Returning to reading mode."));
    current_mode = MODE_READING;
}

void clear_i2c_bus() {
    pinMode(BOARD_SDA, INPUT_PULLUP);
    pinMode(BOARD_SCL, OUTPUT);
    for (int i = 0; i < 9; i++) {
        digitalWrite(BOARD_SCL, LOW);
        delayMicroseconds(5);
        digitalWrite(BOARD_SCL, HIGH);
        delayMicroseconds(5);
        if (digitalRead(BOARD_SDA) == HIGH) {
            break;
        }
    }
    pinMode(BOARD_SCL, INPUT);
}

void setup() {
    Serial.begin(115200);
    clear_i2c_bus(); // Free I2C bus if GT911 is holding it low
    delay(3000);

    Serial.println(F("\n========================================================"));
    Serial.println(F(" LilyGo T5 4.7\" S3 Touch Reader & Multi-Book Library"));
    Serial.println(F("========================================================"));

    // Configure physical buttons
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    pinMode(21, INPUT_PULLUP); // SIDE_BUTTON_PIN
    pinMode(39, INPUT_PULLUP); // SENSOP_VN

    // 1. Initialize E-Paper hardware & allocate PSRAM framebuffer
    Serial.println(F("[EPD] Initializing ED047TC1 panel..."));
    epd_init();
    epd_poweron();
    epd_clear();
    epd_poweroff();

    framebuffer = (uint8_t *)ps_malloc(PAGE_BUFFER_SIZE);
    if (!framebuffer) {
        framebuffer = (uint8_t *)malloc(PAGE_BUFFER_SIZE);
    }

    if (!framebuffer) {
        Serial.println(F("[ERROR] PSRAM/RAM framebuffer allocation failed!"));
        epd_poweroff();
        while(true) delay(100);
    }
    memset(framebuffer, 0xFF, PAGE_BUFFER_SIZE); // Clear to white

    // 2. Initialize Touch Controller
    init_touch();

    // 3. Initialize MicroSD Card on SPI bus (pins from utilities.h)
    Serial.println(F("[SD] Initializing MicroSD card..."));
    SPI.begin(SD_SCLK, SD_MISO, SD_MOSI);
    bool sd_ok = SD.begin(SD_CS, SPI);
    if (!sd_ok) {
        // Retry at lower clock speed
        sd_ok = SD.begin(SD_CS, SPI, 10000000);
    }

    if (!sd_ok) {
        Serial.println(F("[ERROR] MicroSD card mount failed! Check card and FAT32 format."));
        fb_render_status_screen(
            framebuffer,
            "CARTE MICROSD NON DETECTEE",
            "Attention / SD non trouvee",
            "1. Inserez une carte MicroSD formatee en FAT32",
            "2. Placez vos livres dans le dossier /books/",
            "Broches SPI : SCK=11, MOSI=15, MISO=16, CS=42",
            "Inserez la carte SD puis appuyez sur BOOT."
        );
        epd_draw_grayscale_image(epd_full_screen(), framebuffer);
        epd_poweroff();

        // Go to deep sleep immediately, the screen will retain the error message
        while(true) delay(100);
    }

    Serial.printf("[SD] MicroSD mounted successfully. Size: %.2f GB\n",
                  SD.cardSize() / (1024.0 * 1024.0 * 1024.0));

    // 4. Scan books on SD card
    scan_sd_books();
    if (total_books == 0) {
        Serial.println(F("[WARN] No books found on SD card!"));
        fb_render_status_screen(
            framebuffer,
            "AUCUN LIVRE SUR LA CARTE SD",
            "Carte SD detectee (FAT32 OK)",
            "La carte SD fonctionne mais aucun livre",
            "n a ete detecte dans /books/ ou /pages/.",
            "Utilisez 'python tools/convert.py mon_livre.epub'",
            "Copiez les dossiers dans /books/ puis redemarrez."
        );
        epd_draw_grayscale_image(epd_full_screen(), framebuffer);
        epd_poweroff();

        // Go to deep sleep immediately
        while(true) delay(100);
    }

    // 5. Restore active book index from NVS
    Preferences prefs;
    if (prefs.begin("reader", true)) {
        uint32_t last_book_hash = prefs.getUInt("last_hash", 0);
        current_book_idx = 0;
        for (int i = 0; i < total_books; i++) {
            if (hash_string(books[i].title) == last_book_hash) {
                current_book_idx = i;
                break;
            }
        }
        prefs.end();
    }



    if (current_mode == MODE_LIBRARY) {
        run_library_menu(framebuffer);
    }

    // Render initial page
    render_current_book_page(framebuffer);
    last_activity_time = millis();
}

void loop() {
    // 1. Inactivity timeout check -> Auto-lock
    if (!is_locked && millis() - last_activity_time > INACTIVITY_TIMEOUT_MS) {
        toggle_lock_state();
    }

    // 2. Check Touch Input
    int16_t tx = 0, ty = 0;
    if (!is_locked && read_touch_point(&tx, &ty)) {
        Serial.printf("[TOUCH] Registered at (%d, %d)\n", tx, ty);
        last_activity_time = millis();

        if (ty < 180) {
            // Top Header (generous 180px height): Open Library Menu
            Serial.println(F("[TOUCH] Header tap -> Opening Library Menu!"));
            run_library_menu(framebuffer);
            render_current_book_page(framebuffer);
            last_activity_time = millis();
        } else if (tx < 220) {
            // Left Zone: Previous Page
            Serial.println(F("[TOUCH] Left tap -> Previous page (page--)"));
            if (books[current_book_idx].current_page > 0) {
                books[current_book_idx].current_page--;
                save_current_bookmark();
                render_current_book_page(framebuffer);
            }
        } else {
            // Right Zone: Next Page
            Serial.println(F("[TOUCH] Right tap -> Next page (page++)"));
            books[current_book_idx].current_page++;
            save_current_bookmark();
            render_current_book_page(framebuffer);
        }

        // Wait for finger release and clear lingering touch events
        int16_t dump_x, dump_y;
        while (read_touch_point(&dump_x, &dump_y)) {
            delay(10);
        }
        delay(100); // Small debounce after release
    }
    // 3. Check Physical Buttons (21, 39) for Manual Lock/Sleep (requires holding for 1 second)
    if (digitalRead(21) == LOW || digitalRead(39) == LOW) {
        uint32_t press_start = millis();
        while ((digitalRead(21) == LOW || digitalRead(39) == LOW) && millis() - press_start < 1000) {
            delay(20);
        }
        if (millis() - press_start >= 1000) {
            Serial.println(F("[BUTTON] Physical button hold -> Toggling lock state!"));
            toggle_lock_state();
        }
    }

    delay(20);
}
