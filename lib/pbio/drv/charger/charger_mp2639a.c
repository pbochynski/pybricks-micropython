// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 The Pybricks Authors

// driver for MPS MP2639A USB battery charger chip

// NOTE: The datasheet uses CHG and CHGOK interchangeably. We are using CHG
// here since it is shorter. Also, the datasheet is ambiguous as to whether
// low means the CHG signal is low or the pin measured with an oscilloscope
// is logic low. Refer to the pages in the datasheet with oscilloscope captures
// to see what is really going on. When the (not inverted) CHG signal is "on"
// it means "charging", not "charging complete".

#include <pbdrv/config.h>

#if PBDRV_CONFIG_CHARGER_MP2639A

#include <stdbool.h>
#include <stdint.h>

#include <contiki.h>

#include <pbdrv/adc.h>
#include <pbdrv/charger.h>
#include <pbdrv/gpio.h>
#if PBDRV_CONFIG_CHARGER_MP2639A_MODE_PWM
#include <pbdrv/pwm.h>
#endif
#if PBDRV_CONFIG_CHARGER_MP2639A_CHG_RESISTOR_LADDER
#include <pbdrv/resistor_ladder.h>
#endif
#include <pbio/error.h>
#include <pbio/util.h>

#include "../core.h"
#include "charger_mp2639a.h"

#define platform pbdrv_charger_mp2639a_platform_data

PROCESS(pbdrv_charger_mp2639a_process, "MP2639A");
static pbdrv_charger_usb_type_t usb_type;
static process_event_t usb_event;

#if PBDRV_CONFIG_CHARGER_MP2639A_MODE_PWM
static pbdrv_pwm_dev_t *mode_pwm;
#endif

static pbdrv_charger_status_t pbdrv_charger_status;
static bool mode_pin_is_low;

void pbdrv_charger_init(void) {
    pbdrv_init_busy_up();
    usb_event = process_alloc_event();
    process_start(&pbdrv_charger_mp2639a_process);
}

pbdrv_charger_usb_type_t pbdrv_charger_get_usb_type(void) {
    return usb_type;
}

void pbdrv_charger_set_usb_type(pbdrv_charger_usb_type_t type) {
    usb_type = type;
    process_post(&pbdrv_charger_mp2639a_process, usb_event, NULL);
}

pbio_error_t pbdrv_charger_get_current_now(uint16_t *current) {
    pbio_error_t err = pbdrv_adc_get_ch(platform.ib_adc_ch, current);
    if (err != PBIO_SUCCESS) {
        return err;
    }

    // TODO: find and apply scaling

    return PBIO_SUCCESS;
}

pbdrv_charger_status_t pbdrv_charger_get_status(void) {
    return pbdrv_charger_status;
}

void pbdrv_charger_enable(bool enable) {
    #if PBDRV_CONFIG_CHARGER_MP2639A_MODE_PWM
    // REVISIT: only known use has max duty cycle of UINT16_MAX
    pbdrv_pwm_set_duty(mode_pwm, platform.mode_pwm_ch, enable ? 0 : UINT16_MAX);
    #else
    if (enable) {
        pbdrv_gpio_out_low(&platform.mode_gpio);
    } else {
        pbdrv_gpio_out_high(&platform.mode_gpio);
    }
    #endif

    // Need to keep track of MODE pin state for charging logic since /ACOK pin
    // is not wired up.
    mode_pin_is_low = enable;
}

/**
 * Gets the current CHG signal status (inverted compared to /CHG pin state).
 */
static bool read_chg(void) {
    #if PBDRV_CONFIG_CHARGER_MP2639A_CHG_RESISTOR_LADDER
    pbdrv_resistor_ladder_ch_flags_t flags;
    pbio_error_t err = pbdrv_resistor_ladder_get(platform.chg_resistor_ladder_id, &flags);
    if (err != PBIO_SUCCESS) {
        return false;
    }
    // /CHG pin is active low
    return !(flags & platform.chg_resistor_ladder_ch);
    #else
    // /CHG pin is active low.
    return !pbdrv_gpio_input(&platform.chg_gpio);
    #endif
}

PROCESS_THREAD(pbdrv_charger_mp2639a_process, ev, data) {
    PROCESS_BEGIN();

    #if PBDRV_CONFIG_CHARGER_MP2639A_MODE_PWM
    while (pbdrv_pwm_get_dev(platform.mode_pwm_id, &mode_pwm) != PBIO_SUCCESS) {
        PROCESS_PAUSE();
    }
    #endif

    pbdrv_charger_enable(false);

    #if !PBDRV_CONFIG_CHARGER_MP2639A_CHG_RESISTOR_LADDER
    // /CHG pin is pulled low or open drain.
    pbdrv_gpio_set_pull(&platform.chg_gpio, PBDRV_GPIO_PULL_UP);
    pbdrv_gpio_input(&platform.chg_gpio);
    #endif

    pbdrv_init_busy_down();

    // When there is a fault the /CHG pin will toggle on and off at 1Hz, so we
    // have to try to detect that to get 3 possible states out of a digital input.

    static bool chg_samples[5];
    static uint8_t chg_index = 0;
    static struct etimer timer;

    // sample at 5Hz
    etimer_set(&timer, 200);

    for (;;) {
        PROCESS_WAIT_EVENT_UNTIL(ev == PROCESS_EVENT_TIMER && etimer_expired(&timer));
        etimer_restart(&timer);

        chg_samples[chg_index++] = read_chg();
        if (chg_index >= PBIO_ARRAY_SIZE(chg_samples)) {
            chg_index = 0;
        }

        int sum = 0;
        for (int i = 0; i < PBIO_ARRAY_SIZE(chg_samples); i++) {
            sum += chg_samples[i];
        }

        if (mode_pin_is_low) {
            // Status is determined by CHG tri-state.
            if (sum < 2) {
                // CHG signal is off (/CHG pin is logic high).
                pbdrv_charger_status = PBDRV_CHARGER_STATUS_COMPLETE;
            } else if (sum > 3) {
                // CHG signal is on (/CHG pin is logic low).
                pbdrv_charger_status = PBDRV_CHARGER_STATUS_CHARGE;
            } else {
                // CHG blinking at 1 Hz indicates a fault.
                pbdrv_charger_status = PBDRV_CHARGER_STATUS_FAULT;
            }
        } else {
            // This means the battery is discharging. Note that the MP2639A is
            // NOT in discharge mode. That mode (used to charge external
            // devices) requires a momentary pulse on the /PB pin, which is
            // not wired up.
            pbdrv_charger_status = PBDRV_CHARGER_STATUS_DISCHARGE;
        }
    }

    PROCESS_END();
}

#endif // PBDRV_CONFIG_CHARGER_MP2639A
