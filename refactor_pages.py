import re

with open('src/main.cpp', 'r') as f:
    content = f.read()

# Replace book scanning loop
old_scan_loop = """                // 2. Count total page files (page_0000.bin, page_0001.bin, ...)
                int p_count = 0;
                while (p_count < 9999) {
                    char test_page[120];
                    snprintf(test_page, sizeof(test_page), "%s/page_%04d.bin", folder_path, p_count);
                    if (!SD.exists(test_page)) break;
                    p_count++;
                }
                books[total_books].page_count = p_count;"""

new_scan_loop = """                // 2. Count total page files (Fast O(1) via pages.txt metadata if available)
                int p_count = 0;
                char meta_path[120];
                snprintf(meta_path, sizeof(meta_path), "%s/pages.txt", folder_path);
                if (SD.exists(meta_path)) {
                    File meta = SD.open(meta_path, FILE_READ);
                    if (meta) {
                        String p = meta.readStringUntil('\\n');
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
                books[total_books].page_count = p_count;"""
content = content.replace(old_scan_loop, new_scan_loop)

# Replace legacy scan loop
old_legacy_loop = """        int p_count = 0;
        while (p_count < 9999) {
            char test_page[64];
            snprintf(test_page, sizeof(test_page), "/pages/page_%04d.bin", p_count);
            if (!SD.exists(test_page)) break;
            p_count++;
        }
        books[0].page_count = p_count;"""

new_legacy_loop = """        int p_count = 0;
        if (SD.exists("/pages/pages.txt")) {
            File meta = SD.open("/pages/pages.txt", FILE_READ);
            if (meta) {
                String p = meta.readStringUntil('\\n');
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
        books[0].page_count = p_count;"""
content = content.replace(old_legacy_loop, new_legacy_loop)

with open('src/main.cpp', 'w') as f:
    f.write(content)
