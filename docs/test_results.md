# Part A Test Results: 100 ms Timer ISR with I2C OLED Update

This file records the actual verification results for the Part A timer-based OLED firmware.

## Test Environment

| Item | Result |
|---|---|
| Board | ESP32 development board |
| Framework | ESP-IDF |
| RTOS | FreeRTOS |
| Display | SSD1306 OLED |
| Display interface | I2C |
| Timer source | ESP32 GPTimer |
| Timer period | 100 ms |
| I2C speed | 400 kHz |
| OLED I2C address | 0x3C |
| Logic analyzer | Saleae Logic 2.4.41 / Logic Pro 8 |

## Build and Flash Test

**Result: PASS**

The firmware built and flashed successfully from VS Code ESP-IDF.

Observed monitor output:

```text
main_task: Started on CPU0
main_task: Calling app_main()
PART_A_TIMER: ESP32 GPTimer + semaphore + I2C OLED demo starting
PART_A_TIMER: I2C config: SDA=21, SCL=22, FREQ=400000, OLED_ADDR=0x3C
```

Meaning: the application starts successfully and the firmware is configured for OLED I2C communication at address `0x3C`.

## OLED Initialization Test

**Result: PASS**

The OLED initialized and showed the timer-driven bar display pattern.

Meaning: OLED power, ground, SDA/SCL wiring, I2C setup, and SSD1306 initialization are working.

## Timer ISR and Display Task Test

**Result: PASS**

The monitor showed periodic timer-driven logs.

Observed monitor output:

```text
PART_A_TIMER: TIMER_EVT=10 ISR_EVENTS=10 HANDLED=10 MERGED_OR_PENDING=0
PART_A_TIMER: TIMER_EVT=20 ISR_EVENTS=20 HANDLED=20 MERGED_OR_PENDING=0
PART_A_TIMER: TIMER_EVT=30 ISR_EVENTS=30 HANDLED=30 MERGED_OR_PENDING=0
PART_A_TIMER: TIMER_EVT=40 ISR_EVENTS=40 HANDLED=40 MERGED_OR_PENDING=0
```

Meaning: the GPTimer ISR is producing events, `display_task` is handling them, and no merged/pending events were observed during this run.

Because the runtime log is printed once every 10 handled events, and the timer period is 100 ms, the log appears about once per second.

## ISR Safety Review

**Result: PASS**

The ISR does not call OLED functions, I2C functions, Serial/printf, delay, malloc, or blocking APIs.

The ISR only increments the ISR diagnostic counter and gives the binary semaphore using `xSemaphoreGiveFromISR()`.

Meaning: the ISR remains short and safe, and OLED/I2C work is deferred to task context.

## Logic Analyzer Capture

**Result: PASS**

Saleae Logic 2.4.41 was connected in parallel with the OLED I2C bus.

Connection used:

```text
Saleae D0 -> OLED SCL / ESP32 GPIO22
Saleae D1 -> OLED SDA / ESP32 GPIO21
Saleae GND -> ESP32 GND
```

The I2C analyzer decoded repeated OLED write transactions at address `0x3C`.

Observed decode pattern:

```text
W [0x3C]
```

Meaning: the ESP32 is repeatedly writing to the SSD1306 OLED at the expected I2C address.

The Saleae capture showed repeated I2C bursts while the ESP-IDF monitor showed periodic `TIMER_EVT` logs. This supports that OLED traffic is produced through the timer-driven display update path. The ISR itself only gives the semaphore; the display task performs the OLED update.

Evidence files to include:

```text
evidence/part_a_saleae_i2c_repeated_bursts.png
evidence/part_a_saleae_i2c_0x3c_ack_zoom.png
evidence/part_a_terminal_timer_logs.png
evidence/part_a_oled_periodic_update.jpg
```

Optional raw capture file:

```text
evidence/part_a_i2c_timer_capture.sal
```

## Post-Review Improvements Verified

**Result: PASS**

The following post-review improvements were implemented and verified:

- Added ESP-IDF log tag: `PART_A_TIMER`
- Changed I2C speed to 400 kHz
- Reduced OLED I2C timeout from 1000 ms to 100 ms
- Added ISR event counter
- Added handled-event counter
- Added `MERGED_OR_PENDING` diagnostic
- Reduced runtime logging to once every 10 handled timer events
- Captured Saleae I2C evidence showing repeated writes to `0x3C`

## Known Follow-Up Items

These are not blocking for the current Part A evidence package, but would improve production robustness:

- Run a longer 10-minute stress test.
- Save the raw Saleae capture file along with screenshots.
- Add repeated I2C error counter with retry/backoff and bus reset recovery.
- Add command stress testing if the Serial command protocol is included in this repo version.

## Final Result

**Overall Result: PASS**

The project demonstrates the required architecture:

```text
GPTimer ISR -> Binary Semaphore -> FreeRTOS Display Task -> I2C OLED Update
```

The timer fires at a documented 100 ms period, the ISR stays short, OLED/I2C writes happen outside ISR context, monitor logs show handled timer events, and Saleae capture shows repeated I2C writes to OLED address `0x3C`.
