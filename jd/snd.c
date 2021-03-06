#include "jdsimple.h"

#define CMD_PLAY_TONE 0x80

#ifndef SND_OFF
#define SND_OFF 1
#endif

struct srv_state {
    SRV_COMMON;
    uint8_t volume;
    uint8_t pwm_pin;
    uint8_t is_on;
    uint8_t pin;
    uint32_t end_tone_time;
    uint16_t period;
};

REG_DEFINITION(               //
    snd_regs,                 //
    REG_SRV_BASE,             //
    REG_U8(JD_REG_INTENSITY), //
)

static void set_pwr(srv_t *state, int on) {
    if (state->is_on == on)
        return;
    if (on) {
        pwr_enter_tim();
    } else {
        pin_set(state->pin, SND_OFF);
        pwm_enable(state->pwm_pin, 0);
        pwr_leave_tim();
    }
    state->is_on = on;
}

static void play_tone(srv_t *state, uint32_t period, uint32_t duty) {
    duty = (duty * state->volume) >> 8;
#if SND_OFF == 1
    duty = period - duty;
#endif
    set_pwr(state, 1);
    state->pwm_pin = pwm_init(state->pin, period, duty, cpu_mhz);
}

void snd_process(srv_t *state) {
    if (state->period && in_past(state->end_tone_time))
        state->period = 0;

    if (state->period == 0) {
        set_pwr(state, 0);
        return;
    }
}

void snd_handle_packet(srv_t *state, jd_packet_t *pkt) {
    srv_handle_reg(state, pkt, snd_regs);
    switch (pkt->service_command) {
    case CMD_PLAY_TONE:
        if (pkt->service_size >= 6) {
            state->end_tone_time = now + ((uint16_t *)pkt->data)[2] * 1000;
            state->period = ((uint16_t *)pkt->data)[0];
            play_tone(state, state->period, ((uint16_t *)pkt->data)[1]);
        }
        snd_process(state);
        break;
    }
}

SRV_DEF(snd, JD_SERVICE_CLASS_MUSIC);
void snd_init(uint8_t pin) {
    SRV_ALLOC(snd);
    state->pin = pin;
    state->volume = 255;
    pin_set(state->pin, SND_OFF);
    pin_setup_output(state->pin);
}
