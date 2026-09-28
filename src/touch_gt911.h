/**
 * @file touch_gt911.h
 * @brief Standalone I2C Driver for GT911 Capacitive Touch Panel on LilyGo T5 4.7" S3.
 *
 * Implements I2C scanning (0x5D / 0x14), coordinate reading, gesture/sleep mode,
 * and debounced polling without requiring heavy external dependencies.
 */

#pragma once

#include <Arduino.h>
#include <Wire.h>

// Dedicated Touch I2C Bus & Interrupt Pins (LilyGo T5 4.7" S3)
#define TOUCH_SDA_PIN     17
#define TOUCH_SCL_PIN     18
#define TOUCH_INT_PIN     16
#define TOUCH_INT_ALT_PIN 21

class TouchGT911 {
public:
    uint8_t address = 0x5D;
    bool initialized = false;

    /**
     * @brief Initializes I2C bus and probes for GT911 at 0x5D and 0x14.
     */
    bool begin() {
        Wire.begin(TOUCH_SDA_PIN, TOUCH_SCL_PIN, 400000);

        // Probe primary address 0x5D
        Wire.beginTransmission(0x5D);
        if (Wire.endTransmission() == 0) {
            address = 0x5D;
            initialized = true;
            Serial.println(F("[TOUCH] GT911 detected at I2C address 0x5D"));
            return true;
        }

        // Probe secondary address 0x14
        Wire.beginTransmission(0x14);
        if (Wire.endTransmission() == 0) {
            address = 0x14;
            initialized = true;
            Serial.println(F("[TOUCH] GT911 detected at I2C address 0x14"));
            return true;
        }

        Serial.println(F("[TOUCH] Warning: No GT911 controller detected. Falling back to BOOT button."));
        initialized = false;
        return false;
    }

    /**
     * @brief Reads a single touch point from the controller.
     * @param x Output X coordinate (0..539)
     * @param y Output Y coordinate (0..959)
     * @return true if a valid touch was captured, false otherwise.
     */
    bool read(uint16_t *x, uint16_t *y) {
        if (!initialized) return false;

        // 1. Read Buffer Status from register 0x814E
        Wire.beginTransmission(address);
        Wire.write(0x81);
        Wire.write(0x4E);
        if (Wire.endTransmission() != 0) return false;

        if (Wire.requestFrom((uint8_t)address, (uint8_t)1) != 1) return false;
        uint8_t status = Wire.read();

        bool has_touch = false;
        if ((status & 0x80) && (status & 0x0F) > 0) {
            // 2. Read first touch point coordinates (registers 0x8150..0x8153)
            Wire.beginTransmission(address);
            Wire.write(0x81);
            Wire.write(0x50);
            if (Wire.endTransmission() == 0 && Wire.requestFrom((uint8_t)address, (uint8_t)4) == 4) {
                uint8_t x_lo = Wire.read();
                uint8_t x_hi = Wire.read();
                uint8_t y_lo = Wire.read();
                uint8_t y_hi = Wire.read();

                *x = (uint16_t)(x_lo | (x_hi << 8));
                *y = (uint16_t)(y_lo | (y_hi << 8));

                // Constrain to physical display dimensions (540x960 portrait)
                if (*x >= 540) *x = 539;
                if (*y >= 960) *y = 959;

                has_touch = true;
            }
        }

        // 3. Clear buffer status register (write 0x00 to 0x814E)
        if (status & 0x80) {
            Wire.beginTransmission(address);
            Wire.write(0x81);
            Wire.write(0x4E);
            Wire.write(0x00);
            Wire.endTransmission();
        }

        return has_touch;
    }

    /**
     * @brief Polls for an active touch within a given timeout window (useful on wakeup).
     */
    bool poll(uint16_t *x, uint16_t *y, uint32_t timeout_ms = 80) {
        if (!initialized) return false;
        uint32_t start = millis();
        while (millis() - start < timeout_ms) {
            if (read(x, y)) {
                return true;
            }
            delay(5);
        }
        return false;
    }

    /**
     * @brief Puts GT911 into low-power / sleep mode so it monitors touches
     *        and pulses the INT pin to wake up the ESP32.
     */
    void prepare_for_sleep() {
        if (!initialized) return;

        // Command check & command register to enter low-power sleep / gesture mode
        Wire.beginTransmission(address);
        Wire.write(0x80);
        Wire.write(0x46);
        Wire.write(0x08);
        Wire.endTransmission();

        Wire.beginTransmission(address);
        Wire.write(0x80);
        Wire.write(0x40);
        Wire.write(0x08);
        Wire.endTransmission();
    }
};

extern TouchGT911 Touch;
