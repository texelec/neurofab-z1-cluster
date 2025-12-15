/**
 * SSD1306 OLED Driver (128x64, I2C)
 * 
 * Working driver for I2C OLED display
 * Address: 0x3C (7-bit)
 * SDA: GPIO 28
 * SCL: GPIO 29
 */

#ifndef SSD1306_H
#define SSD1306_H

#include <stdint.h>
#include <stdbool.h>

// Display dimensions
#define SSD1306_WIDTH  128
#define SSD1306_HEIGHT 64

// I2C configuration
#define SSD1306_I2C_ADDR    0x3C    // 7-bit address
#define SSD1306_SDA_PIN     28
#define SSD1306_SCL_PIN     29
#define SSD1306_I2C_FREQ    400000  // 400 kHz

// Initialize display
bool ssd1306_init(void);

// Clear display
void ssd1306_clear(void);

// Update display (send buffer to OLED)
void ssd1306_update(void);

// Write text (simple line-based interface)
void ssd1306_write_line(const char* text, uint8_t line);

// Low-level drawing (optional, for future use)
void ssd1306_set_pixel(uint8_t x, uint8_t y, bool on);
void ssd1306_draw_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, bool filled);

// I2C bus scan (debugging helper)
void ssd1306_i2c_scan(void);

#endif // SSD1306_H
