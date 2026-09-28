# 📖 Ultra-Low-Power E-Paper Book Reader (LilyGo T5 4.7" S3)

Complete ultra-low-power book reader system based on the **LilyGo T5 4.7" S3** board (ESP32-S3 + ED047TC1 e-paper display).

The system architecture relies on pre-rendering PDF or E-PUB pages on a host computer into an optimized 4-bit binary format stored on a MicroSD card. The ESP32-S3 wakes up solely to stream the page directly into PSRAM, refresh the e-paper display, commit the bookmark to non-volatile flash memory (NVS), and immediately plunges back into deep sleep (**Deep Sleep < 20 µA**).

---

## 🛠️ Hardware Specifications & Constraints

| Component / Parameter | Specification | Technical Constraint |
| :--- | :--- | :--- |
| **Microcontroller** | ESP32-S3-WROOM-1 | Octal PSRAM enabled (`qio_opi`) in PlatformIO |
| **Flash / PSRAM Memory** | 16 MB Flash / 8 MB Octal PSRAM | Framebuffer allocated exclusively in PSRAM via `ps_malloc()` |
| **E-Paper Display & Touch** | 4.7" ED047TC1 + Capacitive Touch (LilyGo T5 S3) | Resolution $540 \times 960$ (Portrait), 16 levels of gray (4-bit nibble). Integrated capacitive touch panel. |
| **Display & Touch Library** | `LilyGo-EPD47` | Official waveforms, `epd_draw_grayscale_image()`, and touch driver (`touch.h`) |
| **Touch Controller Pins** | `SDA=17`, `SCL=18`, `INT=21` | Dedicated I2C bus and interrupt line for L58 / GT911 capacitive touch controller |
| **MicroSD Bus** | Dedicated SPI Bus (FSPI) | `SCK=39`, `MOSI=40`, `MISO=41`, `CS=42` (Do not use default SPI) |
| **Page Navigation & Wakeup** | Capacitive Touch + `BOOT` Button | Tap right: page++, Tap left: page--. Wakeup via Touch INT (GPIO 21) or BOOT button (GPIO 0, `EXT0`) |
| **Power Switch** | Physical Slide Switch | Complete mechanical power cut (on LiPo line or between `EN` and `GND`) |
| **Bookmark** | Flash NVS (`Preferences.h`) | Guaranteed persistence even across total hardware power cuts |
| **Standby Power Profile** | Deep Sleep ($< 20\,\mu\text{A}$) | Image persists indefinitely with zero power draw on bistable e-paper |

---

## 📐 Binary Data Format Specification (`.bin`)

To bypass the microcontroller's computational and vector rendering limitations:
* **Dimensions:** Exactly $540$ pixels wide by $960$ pixels high (portrait).
* **Color Depth:** 4 bits per pixel (16 grayscale levels: `0x0` = pure black, `0xF` = pure white).
* **Nibble Packing:** 2 consecutive pixels packed into 1 byte:
  * High nibble (`byte >> 4`): Even pixel ($2k$)
  * Low nibble (`byte & 0x0F`): Odd pixel ($2k+1$)
* **Exact File Size:**
  $$\frac{540 \times 960}{2} = 259\,200\text{ bytes}$$
* **MicroSD Directory Structure:** `/pages/page_%04d.bin` (e.g., `page_0000.bin`, `page_0001.bin`, ...).

---

## 📁 Repository Structure

```
.
├── context.md              # Initial requirements and technical specifications
├── platformio.ini          # PlatformIO configuration (ESP32-S3, PSRAM qio_opi, lib EPD47)
├── requirements.txt        # Python dependencies (pymupdf, pillow)
├── .gitignore              # Ignore rules (PlatformIO, venv, pages/ and binaries)
├── README.md               # Setup and usage guide
├── src/
│   ├── font8x8.h           # Lightweight 8x8 font table for on-device UI
│   ├── gui.h               # Direct 4-bit PSRAM drawing engine & Library Menu renderer
│   ├── touch_gt911.h       # Standalone I2C driver for GT911 capacitive touch
│   └── main.cpp            # Firmware orchestrator (touch navigation, multi-book library, deep sleep)
└── tools/
    ├── convert.py          # Python pipeline: EPUB & PDF -> 4-bit .bin page conversion
    └── preview.py          # Verification tool: .bin -> inspectable PNG conversion
```

---

## 🚀 Getting Started Guide

### 1. Python Prerequisites & Host Setup

Install the dependencies required for the conversion pipeline:

```bash
# Create a virtual environment (recommended)
python3 -m venv venv
source venv/bin/activate  # On Windows: venv\Scripts\activate

# Install dependencies
pip install -r requirements.txt
```

### 2. E-Book Conversion (`tools/convert.py`: EPUB & PDF)

The `convert.py` script automatically detects whether the input file is an **EPUB** (or reflowable e-book) or a **PDF**:

* **EPUB & Reflowable Formats:** Automatically reflows and repaginates the text to fit the exact $540 \times 960$ screen geometry. Just like on an **Amazon Kindle**, you can freely adjust font size using `--font-size` (points: `12`, `14`, `16`, `18`, `22`, or presets: `small`, `medium`, `large`, `xlarge`), dynamically adapting the text layout.
* **PDF Formats:** Applies $2\times$ supersampling, Lanczos downsampling, and proportional fitting with crisp antialiased typography.
* **Automatic Metadata & Multi-Book Output:** Automatically extracts the book title from EPUB/PDF metadata, creates a clean subfolder in `books/<book_name>/`, and writes a `title.txt` file used by the on-screen library menu.
* **Image Processing:** Supports contrast adjustment (`--contrast`), linear 8-bit $\to$ 4-bit quantization, and strict file size validation ($259\,200$ bytes).

#### 📖 Single Book Conversion Examples

```bash
# 1. Automatic conversion (creates books/my_book/ with title.txt and page_XXXX.bin)
python3 tools/convert.py --input my_book.epub

# 2. Custom title and font size (large typography)
python3 tools/convert.py --input les_miserables.epub --output books/Les_Miserables --title "Les Miserables - Victor Hugo" --font-size large

# 3. PDF with enhanced contrast
python3 tools/convert.py --input "I_Have_No_Mouth_and_I_Must_Scream_-_Harlan_Ellison.pdf" --output books/Harlan_Ellison --title "I Have No Mouth - Harlan Ellison" --contrast 1.25

# 4. Legacy single-book mode: output directly into 'pages/'
python3 tools/convert.py --input my_book.epub --output pages
```

#### 📦 Batch Converting Multiple Books in One Command

You can convert an entire directory of e-books at once using a simple shell loop:

```bash
# Convert all EPUB files in your collection into books/
for book in ~/MyEbooks/*.epub; do
    python3 tools/convert.py --input "$book"
done
```

---

### 3. Visual Verification Before SD Transfer (`tools/preview.py`)

You can inspect the readability, contrast, and layout of any `.bin` file on your PC without having to flash or copy it to the device:

```bash
# Converts page_0000.bin to PNG and opens it in your default image viewer
python3 tools/preview.py books/Les_Miserables/page_0000.bin --show
```

---

### 4. MicroSD Card Preparation & Multi-Book Hierarchy

Follow these simple steps to load your library onto your e-reader:

1. **Format the MicroSD Card:** Format your MicroSD card to **FAT32** (standard 32 KB allocation unit size).
2. **Copy the Books:** Create a folder named `books` at the root of the card and copy your generated book folders inside:

```
MicroSD Card (FAT32)
└── books/
    ├── Candide/
    │     ├── title.txt          <- Display title in library menu (e.g. "Candide - Voltaire")
    │     ├── page_0000.bin      <- Page 1 (259,200 bytes)
    │     ├── page_0001.bin      <- Page 2 (259,200 bytes)
    │     └── ...
    ├── Les_Miserables/
    │     ├── title.txt          <- "Les Miserables - Victor Hugo"
    │     ├── page_0000.bin
    │     └── ...
    └── Harlan_Ellison/
          ├── title.txt          <- "I Have No Mouth - Harlan Ellison"
          ├── page_0000.bin
          └── ...
```

```bash
# Example copy command from Linux / macOS terminal:
cp -r books/ /media/$USER/YOUR_SD_CARD/
sync
```

*(Note: Single-book legacy mode is also fully supported. If no `/books/` folder is found, the firmware automatically falls back to `/pages/page_XXXX.bin`).*

---

## ⚡ Firmware Compilation & Flashing

The project is configured for **PlatformIO** (VS Code or CLI).

### Hardware Configuration in `platformio.ini`
- **Board:** `esp32-s3-devkitc-1`
- **Memory:** Octal PSRAM (`board_build.arduino.memory_type = qio_opi`)
- **Flags:** `-DBOARD_HAS_PSRAM` and `-mfix-esp32-psram-cache-issue`
- **Library:** `LilyGo-EPD47` fetched automatically from GitHub

### PlatformIO CLI Commands

```bash
# Compile firmware
pio run

# Flash to the ESP32-S3 board (connected via USB)
pio run -t upload

# Monitor serial diagnostics (115200 baud)
pio device monitor
```

---

## 🔄 System Operation & Firmware Logic

```
[Touch Screen (Tap Right)] --(Touch INT Wakeup)--> Next Page (page++) -> Save NVS -> Render -> Deep Sleep
[Touch Screen (Tap Left)]  --(Touch INT Wakeup)--> Previous Page (page--) -> Save NVS -> Render -> Deep Sleep
[Touch Screen (Tap Header)]--(Touch INT Wakeup)--> Open Interactive Library Menu (Choose book)
[BOOT Button (Short Press)]--(EXT0 Wakeup)-------> Next Page (page++) -> Save NVS -> Render -> Deep Sleep
[BOOT Button (Long Press)] --(EXT0 Wakeup)-------> Open Interactive Library Menu (Choose book)
[Power Switch]             --(Cold Boot)---------> Keep current book & page -> Render -> Deep Sleep
```

### 1. Reading Mode (Instant Deep Sleep)
* **Touch Page Turning:**
  * Tap **Right Side** ($x \ge 220, y \ge 120$): Advances to the next page (`current_page++`).
  * Tap **Left Side** ($x < 220, y \ge 120$): Returns to the previous page (`current_page--`).
* **Open Library Menu:**
  * Tap **Top Header** ($y < 120$) or hold the physical **BOOT button** for $>1.2\text{s}$.
* **Touch Wakeup:** Touching the screen immediately wakes up the ESP32-S3 via the GT911 touch interrupt line (`TOUCH_INT` on GPIO 16/21).
* **Bookmark Persistence:** Every page turn commits the per-book progress into Flash NVS (`Preferences.h`).
* **Instant Power-Down:** As soon as the page is refreshed onto the ED047TC1 panel, the framebuffer is freed and the ESP32-S3 re-enters deep sleep ($<20\,\mu\text{A}$).

### 2. Interactive Library Menu (Bookshelf Mode)
* **Book Discovery:** Automatically scans the MicroSD card for all book folders inside `/books/` and loads book titles from `title.txt` and bookmarks from NVS.
* **On-Screen Bookshelf:** Renders a clean list of book cards displaying:
  * Book title,
  * Reading progress (e.g. `Page 45 / 180 (25%)`),
  * Visual progress bar,
  * `[EN COURS]` badge on the currently selected book.
* **Touch Selection:**
  * Tap any **Book Card** to immediately switch to that book, load its saved bookmark, render its current page, and sleep.
  * Tap **`< PREV`** / **`NEXT >`** to paginate through your collection (4 books per screen).
  * Tap **`LIRE / RESUME`** (or let it timeout after 25s) to resume your current book.

---

## 🔍 Troubleshooting & Diagnostics

In case of unexpected behavior, connect the board via USB and observe the serial console at **115200 baud**:

* **`[ERROR] MicroSD card mount failed!`:** Check MicroSD card insertion, verify FAT32 formatting, and inspect the slot.
* **`[ERROR] PSRAM allocation failed!`:** Ensure Octal PSRAM mode (`qio_opi`) and the `-DBOARD_HAS_PSRAM` flag are enabled in `platformio.ini`.
* **`Page file ... not found. Wrapping back to page 0.`:** The end of the book was reached (or the requested file does not exist); the reader automatically loops back to `page_0000.bin`.
