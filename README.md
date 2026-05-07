# esp_oled_timer_i2c

ESP-IDF demo project for ESP32 that uses a 100 ms hardware timer interrupt to wake a FreeRTOS display task using a binary semaphore and update an SSD1306 OLED over I2C from task context.

## Overview

This project demonstrates a timer-based interrupt design for updating an OLED display.

- ESP32 GPTimer generates a periodic interrupt every 100 ms
- The timer ISR gives a FreeRTOS binary semaphore
- A display task waits on the semaphore
- When the semaphore is received, the display task updates a 128x64 SSD1306 OLED over I2C

The design keeps interrupt work short and avoids calling I2C or OLED routines inside the ISR.

## Main design principle:

```text
ISR triggers events. Tasks do the work.
```

## Project Flow
```text
ESP32 Hardware GPTimer
        |
        | 100 ms periodic interrupt
        v
Timer ISR Callback
        |
        | xSemaphoreGiveFromISR()
        v
FreeRTOS Binary Semaphore
        |
        | wakes display task
        v
Display Task
        |
        | I2C OLED update
        v
SSD1306 OLED Display
```

In simple terms:

- Timer creates periodic event
- ISR signals the task
- Task performs I2C OLED update

## Hardware

- ESP32 development board
- SSD1306 128x64 I2C OLED display
- USB cable for flashing and serial monitor

## Pin Map

| Signal | GPIO | Purpose |
| --- | --- | --- |
| OLED SDA | GPIO21 | I2C data |
| OLED SCL | GPIO22 | I2C clock |
| OLED VCC | 3.3V | OLED power |
| OLED GND | GND | OLED ground |

## Wiring

```text
GPIO21 -> OLED SDA
GPIO22 -> OLED SCL
3.3V   -> OLED VCC
GND    -> OLED GND
```


## Timer and FreeRTOS Design:

The project uses the ESP-IDF GPTimer driver.

Typical timer settings:
```c
#define TIMER_RESOLUTION_HZ 1000000
#define TIMER_PERIOD_US     100000
```

TIMER_RESOLUTION_HZ = 1000000 means the timer runs at 1 MHz, so 1 timer tick equals 1 microsecond. TIMER_PERIOD_US = 100000 means the timer interrupt runs every 100 ms, which creates an update event at approximately 10 Hz.

The timer ISR gives the binary semaphore using the ISR-safe FreeRTOS API:

```c
xSemaphoreGiveFromISR(display_sem, &higher_priority_task_woken);
```

The display task waits on the semaphore using:

```c
xSemaphoreTake(display_sem, portMAX_DELAY);
```
The semaphore does not carry data. It only signals that an OLED update is needed.

I2C is not called inside the ISR because I2C communication is slower and may wait for bus status, acknowledgments, or driver completion. The ISR only gives the semaphore and returns. The OLED update happens later in the FreeRTOS display task.

## Build And Flash

Prerequisites:

- ESP-IDF installed and configured
- ESP32 target selected
- Board connected over USB

## Example CLI flow:

```bash
idf.py set-target esp32
idf.py build
idf.py -p <PORT> flash monitor
```

## Example on Windows:
```bash
idf.py -p COM4 flash monitor
```
If you use VS Code with the ESP-IDF extension, open the folder and use the build, flash, and monitor commands from the extension UI.

## Expected Behavior

After flashing:

- ESP32 initializes the I2C OLED
- ESP32 configures and starts the GPTimer
- The GPTimer generates an interrupt every 100 ms
- The timer ISR gives the binary semaphore
- The display task wakes up and updates the OLED over I2C

The OLED should update periodically. The serial monitor may also show debug logs confirming that the timer and display task are running.

## Project Layout

- `main/app_main.c`: Timer setup, timer ISR callback, FreeRTOS binary semaphore, and display task
- `main/i2c_app.c` and `main/i2c_app.h`: I2C master bus setup for the OLED
- `main/oled.c` and `main/oled.h`: SSD1306 command and drawing helpers
- `main/oled_cmd_table.c` and `main/oled_cmd_table.h`: OLED initialization command table
- `main/Kconfig.projbuild`: configurable project options
- `main/CMakeLists.txt`: Main component build configuration

## Repository Notes

- `build/` is ignored because it contains generated ESP-IDF artifacts
- `sdkconfig` is ignored so local machine configuration stays out of Git
- `.vscode/` is ignored because it currently contains machine-specific settings such as the COM port and local tool paths

## License

This project is licensed under the MIT License. See [`LICENSE`](LICENSE) for details.
