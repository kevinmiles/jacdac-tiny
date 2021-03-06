#include "jdsimple.h"

#define CHECK_PERIOD 1000 // how often to probe the ADC, in us
#define OVERLOAD_MS 1000  // how long to shut down the power for after overload, in ms

// calibrate readings
#define MA_SCALE 1890
#define MV_SCALE 419

#define PIN_PRE_SENSE PA_4
#define PIN_GND_SENSE PA_5
#define PIN_OVERLOAD PA_6
#define PIN_PULSE PA_3

// both in ms
#define PWR_REG_KEEP_ON_PULSE_DURATION 0x80
#define PWR_REG_KEEP_ON_PULSE_PERIOD 0x81

#define PWR_REG_BAT_VOLTAGE 0x180
#define PWR_REG_OVERLOAD 0x181

#define READING_WINDOW 3

struct srv_state {
    SENSOR_COMMON;
    uint8_t intensity;
    uint8_t overload;
    uint16_t max_power;
    uint16_t curr_power;
    uint16_t battery_voltage;
    uint16_t pulse_duration;
    uint32_t pulse_period;
    uint32_t last_pulse;
    uint32_t nextSample;
    uint32_t overloadExpire;
    uint16_t nsum;
    uint32_t sum_gnd;
    uint32_t sum_pre;
    uint16_t readings[READING_WINDOW - 1];
    uint8_t pwr_on;
};

REG_DEFINITION(                              //
    power_regs,                              //
    REG_SENSOR_BASE,                         //
    REG_U8(JD_REG_INTENSITY),                //
    REG_U8(PWR_REG_OVERLOAD),                //
    REG_U16(JD_REG_MAX_POWER),               //
    REG_U16(JD_REG_READING),                 //
    REG_U16(PWR_REG_BAT_VOLTAGE),            //
    REG_U16(PWR_REG_KEEP_ON_PULSE_DURATION), //
    REG_U16(PWR_REG_KEEP_ON_PULSE_PERIOD),   //
)

static void sort_ints(uint16_t arr[], int n) {
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && arr[j - 1] > arr[j]; j--) {
            int tmp = arr[j - 1];
            arr[j - 1] = arr[j];
            arr[j] = tmp;
        }
}

static void overload(srv_t *state) {
    pwr_pin_enable(0);
    state->pwr_on = 0;
    state->overloadExpire = now + OVERLOAD_MS * 1000;
    state->overload = 1;
}

static int overload_detect(srv_t *state, uint16_t readings[READING_WINDOW]) {
    sort_ints(readings, READING_WINDOW);
    int med_gnd = readings[READING_WINDOW / 2];
    int ma = (med_gnd * MA_SCALE) >> 7;
    if (ma > state->max_power) {
        DMESG("current %dmA", ma);
        overload(state);
        return 1;
    }
    return 0;
}

static void turn_on_power(srv_t *state) {
    DMESG("power on");
    uint16_t readings[READING_WINDOW] = {0};
    unsigned rp = 0;
    adc_prep_read_pin(PIN_GND_SENSE);
    uint32_t t0 = tim_get_micros();
    pwr_pin_enable(1);
    for (int i = 0; i < 1000; ++i) {
        int gnd = adc_convert();
        readings[rp++] = gnd;
        if (rp == READING_WINDOW)
            rp = 0;
        if (overload_detect(state, readings)) {
            DMESG("overload after %d readings %d us", i + 1, (uint32_t)tim_get_micros() - t0);
            (void)t0;
            adc_disable();
            return;
        }
    }
    adc_disable();
    state->pwr_on = 1;
}

void power_process(srv_t *state) {
    sensor_process_simple(state, &state->curr_power, sizeof(state->curr_power));

    if (!should_sample(&state->nextSample, CHECK_PERIOD * 9 / 10))
        return;

    if (state->pulse_period && state->pulse_duration) {
        uint32_t pulse_delta = now - state->last_pulse;
        if (pulse_delta > state->pulse_period * 1000) {
            pulse_delta = 0;
            state->last_pulse = now;
        }
        pin_set(PIN_PULSE, pulse_delta < state->pulse_duration * 1000);
    }

    if (state->overload && in_past(state->overloadExpire))
        state->overload = 0;
    pin_set(PIN_OVERLOAD, state->overload);

    int should_be_on = state->intensity && !state->overload;
    if (should_be_on != state->pwr_on) {
        if (should_be_on) {
            turn_on_power(state);
        } else {
            DMESG("power off");
            state->pwr_on = 0;
            pwr_pin_enable(0);
        }
    }

    int gnd = adc_read_pin(PIN_GND_SENSE);
    int pre = adc_read_pin(PIN_PRE_SENSE);

    uint16_t sortedReadings[READING_WINDOW];
    memcpy(sortedReadings + 1, state->readings, sizeof(state->readings));
    sortedReadings[0] = gnd;
    memcpy(state->readings, sortedReadings, sizeof(state->readings));

    if (overload_detect(state, sortedReadings)) {
        DMESG("overload detected");
    }

    state->nsum++;
    state->sum_gnd += gnd;
    state->sum_pre += pre;

    if (state->nsum >= 1000) {
        state->curr_power = (state->sum_gnd * MA_SCALE) / (128 * state->nsum);
        state->battery_voltage = (state->sum_pre * MV_SCALE) / (256 * state->nsum);

        DMESG("%dmV %dmA %s", state->battery_voltage, state->curr_power,
              state->pwr_on ? "ON" : "OFF");

        state->nsum = 0;
        state->sum_gnd = 0;
        state->sum_pre = 0;
    }
}

void power_handle_packet(srv_t *state, jd_packet_t *pkt) {
    if (sensor_handle_packet_simple(state, pkt, &state->curr_power, sizeof(state->curr_power)))
        return;

    switch (srv_handle_reg(state, pkt, power_regs)) {
    case PWR_REG_KEEP_ON_PULSE_PERIOD:
    case PWR_REG_KEEP_ON_PULSE_DURATION:
        if (state->pulse_period && state->pulse_duration) {
            // assuming 22R in 0805, we get around 1W or power dissipation, but can withstand only
            // 1/8W continous so we limit duty cycle to 10%, and make sure it doesn't stay on for
            // more than 1s
            if (state->pulse_duration > 1000)
                state->pulse_duration = 1000;
            if (state->pulse_period < state->pulse_duration * 10)
                state->pulse_period = state->pulse_duration * 10;
        }
        break;
    }
}

SRV_DEF(power, JD_SERVICE_CLASS_POWER);
void power_init(void) {
    SRV_ALLOC(power);
    state->intensity = 1;
    state->max_power = 500;
    // These should be reasonable defaults.
    // Only 1 out of 14 batteries tested wouldn't work with these settings.
    // 0.6/20s is 7mA (at 22R), so ~6 weeks on 10000mAh battery (mAh are quoted for 3.7V not 5V).
    // These can be tuned by the user for better battery life.
    // Note that the power_process() above at 1kHz takes about 1mA on its own.
    state->pulse_duration = 600;
    state->pulse_period = 20000;
    state->last_pulse = now - state->pulse_duration * 1000;
    tim_max_sleep = CHECK_PERIOD;
    pin_setup_output(PIN_OVERLOAD);
    pin_setup_output(PIN_PULSE);
}
