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
 *  - Touch wakeup: touching the screen wakes the ESP32-S3 from deep sleep
 *  - BOOT button fallback: short press = next page, long press (>1.2s) = open library
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
#include "touch_gt911.h"

// Instantiate Global Touch Controller
TouchGT911 Touch;

// MicroSD Dedicated SPI Bus Pins (LilyGo T5 4.7" S3)
constexpr int SD_SCK  = 39;
constexpr int SD_MOSI = 40;
constexpr int SD_MISO = 41;
constexpr int SD_CS   = 42;

// Page Turn Button (BOOT button, Active LOW)
constexpr gpio_num_t BUTTON_PIN = GPIO_NUM_0;

// Display Framebuffer Dimension Constants (540x960, 4-bit)
constexpr size_t PAGE_BUFFER_SIZE = (EPD_WIDTH * EPD_HEIGHT / 2); // 259,200 bytes

// MicroSD SPI Clock Frequency (20 MHz)
constexpr uint32_t SD_SPI_FREQ = 20000000;

// Dedicated custom SPI bus instance
SPIClass sd_spi(FSPI);

// Multi-Book Management Constants & State
constexpr int MAX_BOOKS = 24;
constexpr int BOOKS_PER_PAGE = 4;
BookMetadata books[MAX_BOOKS];
int total_books = 0;
int current_book_idx = 0;

// Operating Modes
enum ReaderMode {
    MODE_READING = 0,
    MODE_LIBRARY = 1
};
ReaderMode current_mode = MODE_READING;

/**
 * @brief Enters deep sleep with both Touch INT and BOOT button configured as wakeup triggers.
 */
void enter_deep_sleep() {
    Serial.println(F("[PWR] Arming Touch and Button wakeups. Entering deep sleep..."));
    Serial.flush();

    // Prepare touch controller for low-power gesture/tap detection
    Touch.prepare_for_sleep();

    // Enable EXT1 wakeup: triggers on LOW logic level on BOOT button (GPIO 0)
    // or Touch Interrupt (GPIO 16 or GPIO 21)
    uint64_t wakeup_mask = (1ULL << BUTTON_PIN) | 
                           (1ULL << TOUCH_INT_PIN) | 
                           (1ULL << TOUCH_INT_ALT_PIN);
    esp_sleep_enable_ext1_wakeup(wakeup_mask, ESP_EXT1_WAKEUP_ANY_LOW);

    // Also configure EXT0 on BOOT button for absolute redundancy
    esp_sleep_enable_ext0_wakeup(BUTTON_PIN, 0);

    // Enter deep sleep: display remains visible indefinitely with zero power draw
    esp_deep_sleep_start();
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
                char title_file_path[100];
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

                // 2. Count pages (look for page_0000.bin, page_0001.bin, ...)
                char test_p0[100];
                snprintf(test_p0, sizeof(test_p0), "%s/page_0000.bin", folder_path);
                if (SD.exists(test_p0)) {
                    int p_count = 0;
                    while (p_count < 9999) {
                        char test_page[100];
                        snprintf(test_page, sizeof(test_page), "%s/page_%04d.bin", folder_path, p_count);
                        if (!SD.exists(test_page)) break;
                        p_count++;
                    }
                    books[total_books].page_count = p_count;

                    // 3. Load per-book bookmark from Flash NVS
                    Preferences p;
                    if (p.begin("reader", true)) {
                        char book_key[20];
                        snprintf(book_key, sizeof(book_key), "b_%d_p", total_books);
                        books[total_books].current_page = p.getInt(book_key, 0);
                        p.end();
                    } else {
                        books[total_books].current_page = 0;
                    }

                    Serial.printf("[SD] Book [%d]: '%s' (%d pages, bookmark: p.%d) in %s\n",
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
        while (p_count < 9999) {
            char test_page[48];
            snprintf(test_page, sizeof(test_page), "/pages/page_%04d.bin", p_count);
            if (!SD.exists(test_page)) break;
            p_count++;
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
        prefs.putInt("book_idx", current_book_idx);

        char book_key[20];
        snprintf(book_key, sizeof(book_key), "b_%d_p", current_book_idx);
        prefs.putInt(book_key, books[current_book_idx].current_page);

        // Also save legacy "page" key for backward compatibility
        prefs.putInt("page", books[current_book_idx].current_page);
        prefs.end();

        Serial.printf("[NVS] Saved book [%d] bookmark: page %d\n",
                      current_book_idx, books[current_book_idx].current_page);
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

    // Wrap-around check
    if (b.current_page >= b.page_count && b.page_count > 0) {
        Serial.printf("[READER] Reached end of book. Wrapping to page 0.\n");
        b.current_page = 0;
        save_current_bookmark();
    }
    if (b.current_page < 0) {
        b.current_page = 0;
        save_current_bookmark();
    }

    char filename[100];
    snprintf(filename, sizeof(filename), "%s/page_%04d.bin", b.path, b.current_page);

    Serial.printf("[READER] Loading '%s' (%s, p.%d)...\n",
                  filename, b.title, b.current_page + 1);

    File page_file = SD.open(filename, FILE_READ);
    if (!page_file) {
        Serial.printf("[ERROR] Failed to open '%s'! File may be missing.\n", filename);
        return;
    }

    size_t bytes_read = page_file.read(fb, PAGE_BUFFER_SIZE);
    page_file.close();

    if (bytes_read != PAGE_BUFFER_SIZE) {
        Serial.printf("[WARN] Incomplete page read: %u of %u bytes.\n",
                      (unsigned int)bytes_read, (unsigned int)PAGE_BUFFER_SIZE);
    }

    // Refresh E-Paper display
    Serial.println(F("[EPD] Refreshing ED047TC1 e-paper panel..."));
    epd_init();
    epd_poweron();
    epd_clear();
    epd_draw_grayscale_image(epd_full_screen(), fb);
    epd_poweroff();
    Serial.println(F("[EPD] Panel refresh complete."));
}

/**
 * @brief Runs the interactive Library Menu on the e-paper panel.
 *        Allows the user to browse books, tap a book to read it, or resume reading.
 */
void run_library_menu(uint8_t *fb) {
    int lib_page = current_book_idx / BOOKS_PER_PAGE;
    int total_lib_pages = max(1, (total_books + BOOKS_PER_PAGE - 1) / BOOKS_PER_PAGE);

    // Initial render of the library screen
    fb_render_library_screen(fb, books, total_books, current_book_idx, lib_page, BOOKS_PER_PAGE);
    epd_init();
    epd_poweron();
    epd_clear();
    epd_draw_grayscale_image(epd_full_screen(), fb);
    epd_poweroff();

    Serial.println(F("[MENU] Library menu active. Waiting for touch selection..."));

    // Interactive touch loop (25 seconds timeout to save battery)
    uint32_t menu_start = millis();
    constexpr uint32_t MENU_TIMEOUT_MS = 25000;

    while (millis() - menu_start < MENU_TIMEOUT_MS) {
        uint16_t tx = 0, ty = 0;
        if (Touch.read(&tx, &ty)) {
            Serial.printf("[MENU] Touch detected at (%d, %d)\n", tx, ty);
            menu_start = millis(); // Reset timeout on user interaction

            // Check Footer Buttons: y in [860..940]
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
                    delay(250);
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
                    delay(250);
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
                if (ty >= cy && ty <= cy + card_h && tx >= 20 && tx <= EPD_WIDTH - 20) {
                    Serial.printf("[MENU] Selected book [%d]: '%s'\n", i, books[i].title);
                    current_book_idx = i;
                    save_current_bookmark();
                    current_mode = MODE_READING;
                    delay(250);
                    return;
                }
            }
        }
        delay(30);
    }

    Serial.println(F("[MENU] Menu timed out. Returning to reading mode."));
    current_mode = MODE_READING;
}

void setup() {
    Serial.begin(115200);
    Serial.println(F("\n========================================================"));
    Serial.println(F(" LilyGo T5 4.7\" S3 Touch Reader & Multi-Book Library"));
    Serial.println(F("========================================================"));

    // 1. Determine Wakeup Reason
    esp_sleep_wakeup_cause_t wakeup_cause = esp_sleep_get_wakeup_cause();
    Serial.printf("[WAKE] Wakeup cause: %d\n", wakeup_cause);

    // 2. Initialize Touch Controller (I2C)
    Touch.begin();

    // 3. Initialize Custom SPI Bus and MicroSD Card
    sd_spi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS, sd_spi, SD_SPI_FREQ)) {
        Serial.println(F("[ERROR] MicroSD card mount failed! Check card and FAT32 format."));
        enter_deep_sleep();
    }

    // 4. Scan Books from SD
    scan_sd_books();
    if (total_books == 0) {
        Serial.println(F("[ERROR] No books found on SD card! Place pages in /books/<name>/ or /pages/."));
        SD.end();
        enter_deep_sleep();
    }

    // 5. Restore Active Book Index from NVS
    Preferences prefs;
    if (prefs.begin("reader", true)) {
        current_book_idx = prefs.getInt("book_idx", 0);
        if (current_book_idx >= total_books || current_book_idx < 0) {
            current_book_idx = 0;
        }
        prefs.end();
    }

    // 6. Handle User Action (Touch / BOOT Button / Reset)
    bool button_long_press = false;
    if (digitalRead(BUTTON_PIN) == LOW) {
        uint32_t press_start = millis();
        while (digitalRead(BUTTON_PIN) == LOW && millis() - press_start < 1300) {
            delay(20);
        }
        if (millis() - press_start >= 1200) {
            button_long_press = true;
            Serial.println(F("[INPUT] BOOT button long press detected -> opening Library Menu!"));
        }
    }

    uint16_t touch_x = 0, touch_y = 0;
    bool has_touch = Touch.poll(&touch_x, &touch_y, 70);

    if (button_long_press) {
        current_mode = MODE_LIBRARY;
    } else if (has_touch) {
        Serial.printf("[TOUCH] Wakeup touch registered at (%d, %d)\n", touch_x, touch_y);
        if (touch_y < 120) {
            // Tapping top header zone opens Library Menu
            Serial.println(F("[TOUCH] Header tap -> opening Library Menu!"));
            current_mode = MODE_LIBRARY;
        } else if (touch_x < 220) {
            // Tapping left zone goes to previous page
            Serial.println(F("[TOUCH] Left tap -> Previous page (page--)"));
            if (books[current_book_idx].current_page > 0) {
                books[current_book_idx].current_page--;
                save_current_bookmark();
            }
            current_mode = MODE_READING;
        } else {
            // Tapping right zone advances to next page
            Serial.println(F("[TOUCH] Right tap -> Next page (page++)"));
            books[current_book_idx].current_page++;
            save_current_bookmark();
            current_mode = MODE_READING;
        }
    } else if (wakeup_cause == ESP_SLEEP_WAKEUP_EXT0 || 
              (wakeup_cause == ESP_SLEEP_WAKEUP_EXT1 && digitalRead(BUTTON_PIN) == LOW)) {
        // BOOT Button short press
        Serial.println(F("[BUTTON] BOOT button pressed -> Next page (page++)"));
        books[current_book_idx].current_page++;
        save_current_bookmark();
        current_mode = MODE_READING;
    } else {
        // Cold boot (slide switch toggled ON) or unknown reset: maintain current page
        Serial.printf("[BOOT] Power-on / reset -> maintaining book [%d] at page %d\n",
                      current_book_idx, books[current_book_idx].current_page + 1);
        current_mode = MODE_READING;
    }

    // 7. Allocate PSRAM Framebuffer
    uint8_t *fb = (uint8_t *)ps_malloc(PAGE_BUFFER_SIZE);
    if (!fb) {
        Serial.println(F("[ERROR] PSRAM framebuffer allocation failed!"));
        SD.end();
        enter_deep_sleep();
    }

    // 8. Execute Operating Mode
    if (current_mode == MODE_LIBRARY) {
        run_library_menu(fb);
    }

    // Render current book page
    render_current_book_page(fb);

    // 9. Cleanup & Enter Deep Sleep
    free(fb);
    fb = nullptr;
    SD.end();

    enter_deep_sleep();
}

void loop() {
    // Execution will never reach loop() as esp_deep_sleep_start() terminates setup()
}
