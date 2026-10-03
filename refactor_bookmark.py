import re

with open('src/main.cpp', 'r') as f:
    content = f.read()

# 1. Add hash_string function
hash_func = """uint32_t hash_string(const char *str) {
    uint32_t hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    }
    return hash;
}

"""
content = content.replace("void run_library_menu(uint8_t *fb);", hash_func + "void run_library_menu(uint8_t *fb);")


# 2. Update scan_sd_books
old_scan = """                // 3. Load saved page bookmark from NVS for this book
                Preferences book_prefs;
                char pref_ns[20];
                snprintf(pref_ns, sizeof(pref_ns), "b_%d", total_books);
                if (book_prefs.begin("reader", true)) {
                    char book_key[20];
                    snprintf(book_key, sizeof(book_key), "b_%d_p", total_books);
                    books[total_books].current_page = book_prefs.getInt(book_key, 0);
                    book_prefs.end();
                } else {
                    books[total_books].current_page = 0;
                }"""

new_scan = """                // 3. Load saved page bookmark from NVS for this book (using title hash)
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
                }"""
content = content.replace(old_scan, new_scan)

# 3. Update save_current_bookmark
old_save = """void save_current_bookmark() {
    Preferences prefs;
    if (prefs.begin("reader", false)) {
        prefs.putInt("book_idx", current_book_idx);

        char book_key[20];
        snprintf(book_key, sizeof(book_key), "b_%d_p", current_book_idx);
        prefs.putInt(book_key, books[current_book_idx].current_page);

        // Legacy compatibility key
        prefs.putInt("page", books[current_book_idx].current_page);
        prefs.end();

        Serial.printf("[NVS] Saved book [%d] bookmark: page %d\\n",
                      current_book_idx, books[current_book_idx].current_page);
    }
}"""

new_save = """void save_current_bookmark() {
    Preferences prefs;
    if (prefs.begin("reader", false)) {
        uint32_t title_hash = hash_string(books[current_book_idx].title);
        prefs.putUInt("last_hash", title_hash);

        char book_key[16];
        snprintf(book_key, sizeof(book_key), "%08X", title_hash);
        prefs.putInt(book_key, books[current_book_idx].current_page);
        prefs.end();

        Serial.printf("[NVS] Saved book '%s' bookmark: page %d\\n",
                      books[current_book_idx].title, books[current_book_idx].current_page);
    }
}"""
content = content.replace(old_save, new_save)

# 4. Update setup restore logic
old_restore = """    // 5. Restore active book index from NVS
    Preferences prefs;
    if (prefs.begin("reader", true)) {
        current_book_idx = prefs.getInt("book_idx", 0);
        if (current_book_idx >= total_books || current_book_idx < 0) {
            current_book_idx = 0;
        }
        prefs.end();
    }"""

new_restore = """    // 5. Restore active book index from NVS
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
    }"""
content = content.replace(old_restore, new_restore)

with open('src/main.cpp', 'w') as f:
    f.write(content)
