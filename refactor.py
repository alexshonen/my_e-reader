import re

with open('src/main.cpp', 'r') as f:
    content = f.read()

# 1. Add is_locked and toggle_lock_state
lock_impl = """bool is_locked = false;

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
"""

content = re.sub(r'/\*\*\n \* @brief Locks the device.*?\n}\n\n/\*\*\n \* @brief Enters ultra-low-power deep sleep.*?\n}\n', lock_impl, content, flags=re.DOTALL)
content = content.replace('void enter_deep_sleep();\n', '')

# 2. Replace enter_deep_sleep calls in setup with while(true)
content = content.replace('        enter_deep_sleep();', '        while(true) delay(100);')

# 3. Remove deep sleep wakeup checks in setup
wakeup_check = """    // Check if woken from Deep Sleep via Touch Interrupt
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_GPIO || cause == ESP_SLEEP_WAKEUP_EXT1) {
        Serial.println(F("[PWR] Woke up from Deep Sleep via Touch! Resuming reading mode..."));
        current_mode = MODE_READING;
    } else {
        // Normal boot: stay in default mode (MODE_LIBRARY)
    }"""
content = content.replace(wakeup_check, '')

# 4. Update loop
loop_old = """void loop() {
    // 1. Inactivity timeout check -> Enter Deep Sleep (<20uA)
    if (millis() - last_activity_time > INACTIVITY_TIMEOUT_MS) {
        while(true) delay(100);
    }

    // 2. Check Touch Input
    int16_t tx = 0, ty = 0;
    if (read_touch_point(&tx, &ty)) {
        Serial.printf("[TOUCH] Registered at (%d, %d)\\n", tx, ty);
        last_activity_time = millis();

        if (ty < 120 && tx < 120) {
            // Top-Left Corner: Manual Lock & Sleep
            Serial.println(F("[TOUCH] Top-Left tap -> Locking device!"));
            lock_device_and_sleep();
        } else if (ty < 120) {"""

loop_new = """void loop() {
    // 1. Inactivity timeout check -> Auto-lock
    if (!is_locked && millis() - last_activity_time > INACTIVITY_TIMEOUT_MS) {
        toggle_lock_state();
    }

    // 2. Check Touch Input
    int16_t tx = 0, ty = 0;
    if (!is_locked && read_touch_point(&tx, &ty)) {
        Serial.printf("[TOUCH] Registered at (%d, %d)\\n", tx, ty);
        last_activity_time = millis();

        if (ty < 120 && tx < 120) {
            // Top-Left Corner: Manual Lock
            Serial.println(F("[TOUCH] Top-Left tap -> Locking device!"));
            toggle_lock_state();
        } else if (ty < 120) {"""
content = content.replace(loop_old, loop_new)

# Update physical buttons at bottom of loop
content = content.replace('lock_device_and_sleep();', 'toggle_lock_state();')

with open('src/main.cpp', 'w') as f:
    f.write(content)
