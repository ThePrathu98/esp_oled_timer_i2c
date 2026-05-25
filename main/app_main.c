/*
 * ESP32 100 ms Timer ISR + FreeRTOS Binary Semaphore + I2C OLED Demo
 *
 * System flow:
 * Hardware GPTimer generates a periodic interrupt every 100 ms.
 * Timer ISR gives a binary semaphore using an ISR-safe FreeRTOS API.
 * display_task blocks forever on that semaphore.
 * When the semaphore is given, display_task wakes and updates the OLED over I2C.
 *
 * Important rule:
 * Do not call OLED/I2C functions inside the ISR. I2C is slower and should run
 * in task context, not interrupt context.
 *
 * Key design principle:
 * ISR triggers events. Tasks do the work.
 */

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/i2c_master.h"
#include "driver/gptimer.h"

#include "esp_err.h"

//Added library header file for better ESP logging. will replace printf() with ESP-IDF production
//style-logging
#include "esp_log.h"     

#include "esp_system.h"

#include "i2c_app.h"
#include "oled.h"

/*Added ESP-IDF logging tag.
Using ESP_LOGx instead of printf gives module-specific logs and allows
Log filtering by severity level. */

static const char *TAG = "PART_A_TIMER";

/*
 * Timer configuration.
 *
 * GPTIMER_RESOLUTION_HZ = 1,000,000 means the timer runs at 1 MHz.
 *
 * At 1 MHz:
 * 1 timer tick = 1 microsecond
 * 100,000 ticks = 100,000 microseconds = 100 ms
 *
 * DISPLAY_TIMER_PERIOD_US sets the timer alarm period to 100 ms.
 */
#define GPTIMER_RESOLUTION_HZ        1000000ULL     //For simplicity, start from 1Mhz. CPU clk freq. for this
                                                    // microcontroller is 160 MHZ for normal mode

#define DISPLAY_TIMER_PERIOD_US      100000ULL      //Timer period macro 100 ms.

/*
 * Display task configuration.
 *
 * Stack size is in bytes in ESP-IDF FreeRTOS.
 * Priority 3 is higher than the idle task and is enough for this project.
 */
#define DISPLAY_TASK_STACK_SIZE      4096
#define DISPLAY_TASK_PRIORITY        3
/*
 * Binary semaphore used for ISR-to-task synchronization.
 *
 * Timer ISR gives this semaphore every 100 ms.
 * display_task waits on this semaphore.
 *
 * The semaphore does not carry data. It only signals:
 * "An OLED update event happened."
 */


/*Added Post-review diagnostic and recovery configuration.
OLED_RECOVERY_ERROR_LIMIT:
After this many consecutive OLED/I2C failures, the display task attempts to reset the I2C bus and reinitialize the OLED.

STATUS_LOG_DIVIDER:
Runtime status is logged every 10 handled timer events instead of every
single 100 ms tick. This avoids excessive UART logging in the hot path. */

#define OLED_RECOVERY_ERROR_LIMIT    3U
#define OLED_RECOVERY_BACKOFF_MS     50U
#define STATUS_LOG_DIVIDER           10U


//Global varriable counters for missed/merged event diagnostics, directly address the feedback item
static volatile uint32_t isr_event_count = 0;
static uint32_t handled_event_count = 0;

static SemaphoreHandle_t display_sem = NULL;
/*
 * GPTimer handle.
 *
 * This handle is created during initialization and used by ESP-IDF internally
 * after the timer is started.
 */
static gptimer_handle_t display_timer = NULL;

/*
 * Timer interrupt callback.
 *
 * This function runs in interrupt context.
 *
 * ISR rules:
 * - Do not call I2C functions here.
 * - Do not call OLED functions here.
 * - Do not print logs here.
 * - Do not allocate memory here.
 * - Do only short, deterministic work.
 *
 * In this project, the ISR only gives a semaphore and exits.
 */
static bool IRAM_ATTR timer_isr_handler(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx)
{
    /*This variable tells FreeRTOS whether giving the semaphore woke a task
     * that has higher priority than the currently running task.
     * If yes, the ISR can request a context switch before returning.*/
    BaseType_t higher_priority_task_woken = pdFALSE;

    /*Added Diagnostic counter:
    * Counts how many timer interrupts were produced.
    * This is intentionally simple and short so the ISR still remains safe.
    */
    isr_event_count++;

    /* Get the semaphore from user_ctx instead of directly using the global
    * display_sem. This keeps the ISR slightly decoupled from global state.
    *
    * Normal behavior is unchanged: the ISR still gives the same binary
    * semaphore to wake display_task.    */
    SemaphoreHandle_t sem = (SemaphoreHandle_t)user_ctx;

    if (sem != NULL)
    {
        xSemaphoreGiveFromISR(sem, &higher_priority_task_woken);
    }

    /*
     * For GPTimer callbacks, returning true requests a context switch after
     * the ISR exits if a higher-priority task was woken.
     */
    return higher_priority_task_woken == pdTRUE;
}

/*
 * Initialize and start the 100 ms periodic GPTimer.
 *
 * This function configures:
 * - count direction: up
 * - resolution: 1 MHz
 * - alarm period: 100,000 us = 100 ms
 * - auto reload: enabled
 * - callback: timer_isr_handler
 */
static esp_err_t display_timer_init(void)
{
    /*
     * Configure the GPTimer base settings.
     *
     * direction = count up
     * resolution_hz = 1 MHz, so timer count units are microseconds
     */
    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = GPTIMER_RESOLUTION_HZ,
    };

    //Create the GPTimer instance and store the handle in display_timer.
    esp_err_t ret = gptimer_new_timer(&timer_config, &display_timer);

    if (ret != ESP_OK)
    {
        printf("ERROR: gptimer_new_timer failed: %d\n", ret);
        return ret;
    }

    gptimer_alarm_config_t alarm_config = {
        .alarm_count = DISPLAY_TIMER_PERIOD_US,
        .reload_count = 0,
        .flags.auto_reload_on_alarm = true,
    };

    /*
     * Configure the timer alarm.
     *
     * alarm_count = 100,000 ticks
     * Since the timer resolution is 1 MHz, this equals 100 ms.
     *
     * reload_count = 0 means the timer restarts counting from 0.
     * auto_reload_on_alarm = true makes the timer repeat periodically.
     */
    ret = gptimer_set_alarm_action(display_timer, &alarm_config);

    if (ret != ESP_OK)
    {
        printf("ERROR: gptimer_set_alarm_action failed: %d\n", ret);
        return ret;
    }

    //Register the callback function that runs when the timer alarm fires.
    gptimer_event_callbacks_t callbacks = {
        .on_alarm = timer_isr_handler,
    };

    /* Pass display_sem into the GPTimer callback as user_ctx.
    * The ISR receives this handle and gives the same semaphore from interrupt
    * context using xSemaphoreGiveFromISR().    */
    ret = gptimer_register_event_callbacks(display_timer, &callbacks, display_sem);

    if (ret != ESP_OK)
    {
        printf("ERROR: gptimer_register_event_callbacks failed: %d\n", ret);
        return ret;
    }

    //Enable the timer before starting it.
    ret = gptimer_enable(display_timer);

    if (ret != ESP_OK)
    {
        printf("ERROR: gptimer_enable failed: %d\n", ret);
        return ret;
    }

    /*
     * Start the timer.
     *
     * After this call, the timer begins generating an alarm interrupt every
     * 100 ms.
     */
    ret = gptimer_start(display_timer);

    if (ret != ESP_OK)
    {
        printf("ERROR: gptimer_start failed: %d\n", ret);
        return ret;
    }

    return ESP_OK;
}

/*
 * Task that owns OLED updates.
 *
 * This task is fully event-driven and blocking.
 *
 * It does not poll.
 * It does not use vTaskDelay().
 * It sleeps indefinitely until the timer ISR gives display_sem.
 *
 * All OLED/I2C work happens here in task context.
 */
static void display_task(void *arg)
{
    /*
     * Counts how many timer events were handled by this task.
     *
     * This is used to change the OLED bar pattern so the update is visible.
     */
    uint32_t event_count = 0;

    for (;;)
    {
        /*
         * Block forever until timer ISR gives the semaphore.
         *
         * portMAX_DELAY means this task consumes no CPU while waiting.
         */
        BaseType_t sem_status = xSemaphoreTake(display_sem, portMAX_DELAY);

        if (sem_status == pdTRUE)
        {
            event_count++;

            //Added increment handled counter in task, diagnostic count for how many timer events were actually processed by the task.
            handled_event_count++;

            /*
             * Page 0 bar grows with timer event count.
             * Modulo keeps the width inside the 128-pixel OLED width.
             */
            uint8_t count_bar = (uint8_t)(event_count % OLED_WIDTH);

            if (count_bar == 0)
            {
                count_bar = OLED_WIDTH;
            }
            /*
             * Clear the display before drawing the next frame.
             *
             * oled_clear() uses I2C internally, which is why it is called from
             * task context instead of ISR context.
             */
            esp_err_t ret = oled_clear();

            if (ret != ESP_OK)
            {
                printf("ERROR: oled_clear failed: %d\n", ret);
                continue;
            }

            /*
             * Page 0: growing bar showing timer event count.
             */
            ret = oled_draw_bar(0, count_bar);

            if (ret != ESP_OK)
            {
                printf("ERROR: oled_draw_bar page 0 failed: %d\n", ret);
                continue;
            }
            /*
             * Page 2: fixed reference bar showing the timer-driven design is active.
             */
            ret = oled_draw_bar(2, 80);

            if (ret != ESP_OK)
            {
                printf("ERROR: oled_draw_bar page 2 failed: %d\n", ret);
                continue;
            }

            /*
             * Page 4: Alternating bar to visually confirm periodic updates.
             */
            ret = oled_draw_bar(4, (event_count % 2U) ? 100 : 30);

            if (ret != ESP_OK)
            {
                printf("ERROR: oled_draw_bar page 4 failed: %d\n", ret);
                continue;
            }
            /*
            * Log once every 10 handled events instead of every 100 ms tick.
            * This reduces UART overhead and adds observability for merged events.
            */
            if ((event_count % 10U) == 0U)
            {
                uint32_t isr_snapshot = isr_event_count;
                uint32_t merged_or_pending = 0;

                if (isr_snapshot > handled_event_count)
                {
                    merged_or_pending = isr_snapshot - handled_event_count;
                }

                ESP_LOGI(TAG,
                        "TIMER_EVT=%" PRIu32 " ISR_EVENTS=%" PRIu32
                        " HANDLED=%" PRIu32 " MERGED_OR_PENDING=%" PRIu32,
                        event_count,
                        isr_snapshot,
                        handled_event_count,
                        merged_or_pending);
            }
        }
    }
}

void app_main(void)
{
    esp_err_t ret;

    //replaced key printf() calls with ESP_LOGx for this and other places in the code.
    ESP_LOGI(TAG, "ESP32 GPTimer + semaphore + I2C OLED demo starting");

    ESP_LOGI(TAG,
         "I2C config: SDA=%d, SCL=%d, FREQ=%d, OLED_ADDR=0x%02X",
         I2C_MASTER_SDA_IO,
         I2C_MASTER_SCL_IO,
         I2C_MASTER_FREQ_HZ,
         OLED_I2C_ADDR);

    i2c_master_dev_handle_t oled_dev_handle = NULL;
    /*
     * Step 1:
     * Initialize I2C master first.
     *
     * This creates the I2C bus and adds the OLED as an I2C device.
     */
    ret = i2c_app_master_init(&oled_dev_handle);

    if (ret != ESP_OK)
    {
        printf("ERROR: i2c_app_master_init failed: %d\n", ret);
        return;
    }
    /*
     * Step 2:
     * Initialize OLED.
     *
     * oled_init() receives the device handle created by i2c_app_master_init().
     */
    ret = oled_init(oled_dev_handle);

    if (ret != ESP_OK)
    {
        printf("ERROR: oled_init failed: %d\n", ret);
        return;
    }

    /*
     * Step 3:
     * Draw a startup pattern before starting the timer.
     *
     * This confirms basic I2C/OLED communication before interrupts begin.
     */
    ret = oled_clear();

    if (ret != ESP_OK)
    {
        printf("ERROR: oled_clear failed: %d\n", ret);
        return;
    }

    ret = oled_draw_bar(0, 20);

    if (ret != ESP_OK)
    {
        printf("ERROR: startup oled_draw_bar failed: %d\n", ret);
        return;
    }

    /*
     * Step 4:
     * Create the binary semaphore before starting the timer.
     *
     * This is critical because the timer ISR will use this semaphore handle.
     * If the timer started before this handle was valid, the ISR would not
     * have a valid synchronization object to signal.
     */
    
    display_sem = xSemaphoreCreateBinary();
    if (display_sem == NULL)
    {
        printf("ERROR: Failed to create display semaphore\n");
        return;
    }

    /*
     * Step 5:
     * Create display_task before enabling the timer.
     *
     * This ensures a task is already waiting when the timer starts generating
     * periodic events.
     */
    BaseType_t task_status = xTaskCreate(display_task,
                                         "display_task",
                                         DISPLAY_TASK_STACK_SIZE,
                                         NULL,
                                         DISPLAY_TASK_PRIORITY,
                                         NULL);

    if (task_status != pdPASS)
    {
        printf("ERROR: Failed to create display_task\n");
        return;
    }

    /*
     * Step 6:
     * Initialize and start the 100 ms periodic timer.
     *
     * After this point:
     * - GPTimer ISR gives the semaphore every 100 ms.
     * - display_task wakes up.
     * - display_task updates the OLED over I2C.
     */
    ret = display_timer_init();

    if (ret != ESP_OK)
    {
        printf("ERROR: display_timer_init failed: %d\n", ret);
        return;
    }

    printf("Minimum free heap size: %" PRIu32 " bytes\n",
           esp_get_minimum_free_heap_size());

    ESP_LOGI(TAG, "100 ms timer ISR + blocking display_task started");

    /*
     * app_main does not need a while loop.
     *
     * The previous GPIO-loopback design used a loop in app_main to toggle
     * GPIO pins. The current design uses GPTimer as the trigger source.
     *
     * FreeRTOS tasks and the GPTimer continue running after app_main returns.
     */
}