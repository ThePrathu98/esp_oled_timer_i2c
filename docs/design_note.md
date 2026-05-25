# Part A Design Note: 100 ms Timer ISR with Deferred I2C OLED Update

This design note explains the **why** behind the Part A timer-based OLED firmware.

The project uses an ESP32 GPTimer to generate a periodic interrupt every 100 ms. The timer ISR does not update the OLED directly. Instead, it gives a FreeRTOS binary semaphore, and `display_task` performs the OLED update over I2C in task context.

```text
GPTimer ISR -> Binary Semaphore -> display_task -> I2C OLED Update
```

The main design rule is:

```text
ISR triggers events. Tasks do the work.
```

The 100 ms period gives a 10 Hz update rate. This is fast enough to visibly prove periodic OLED updates and slow enough to keep I2C traffic, UART logs, and task scheduling easy to observe.

The ISR stays short and safe. It does not call OLED functions, I2C functions, printf/logging, delay, malloc, or blocking APIs. It only increments a diagnostic counter and gives the semaphore using `xSemaphoreGiveFromISR()`.

A binary semaphore is used because the timer event is only a wakeup signal. It does not carry data. This is acceptable because the OLED refresh is idempotent: if multiple timer events merge before the task runs, the display only needs the latest refresh state.

The display task blocks on `xSemaphoreTake(display_sem, portMAX_DELAY)`. While blocked, it consumes no CPU. When the ISR gives the semaphore, the task wakes and performs all OLED/I2C work outside interrupt context.

The initialization order is deliberate: I2C init, OLED init, startup OLED pattern, semaphore creation, display task creation, then GPTimer start. This prevents the timer ISR from firing before the semaphore and task exist.

The firmware is separated into layers. `i2c_app.c/.h` owns I2C bus/device setup, `oled.c/.h` owns SSD1306 OLED operations, and `app_main.c` owns timer, semaphore, and task scheduling.

Post-review changes added lightweight observability: `isr_event_count`, `handled_event_count`, and `MERGED_OR_PENDING` logs. These counters measure whether timer events are being handled or merged.

Post-review tuning changed I2C speed to 400 kHz and reduced the OLED I2C timeout from 1000 ms to 100 ms. This shortens OLED transfer time and surfaces bus faults faster.

This design also prepares for Project B because Wi-Fi and I2C share MCU time. Keeping ISR work short and deferring OLED/I2C work to task context reduces the risk of watchdog resets, TCP drops, and OLED glitches when Wi-Fi is added.

The Part A objective is met because the timer drives the cadence, the ISR stays short, and all OLED/I2C writes happen outside interrupt context.
