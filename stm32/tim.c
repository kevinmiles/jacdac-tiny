#include "jdstm.h"

#define TIMx TIM17
#define TIMx_IRQn TIM17_IRQn
#define TIMx_IRQHandler TIM17_IRQHandler
#define TIMx_CLK_EN() __HAL_RCC_TIM17_CLK_ENABLE()

static volatile uint64_t timeoff;
static volatile cb_t timer_cb;

// takes around 1us
uint64_t tim_get_micros() {
    while (1) {
        uint32_t v0 = TIMx->CNT;
        uint64_t off = timeoff;
        uint32_t v1 = TIMx->CNT;
        if (v0 <= v1)
            return off + v1;
    }
}

cb_t tim_steal_callback(uint32_t *usec) {
    cb_t f;
    f = timer_cb;
    if (f) {
        uint16_t delta = LL_TIM_OC_GetCompareCH1(TIMx) - TIMx->CNT;
        *usec = delta;
        if (500 <= delta && delta <= 10000) {
            timer_cb = NULL;
        } else {
            f = NULL;
        }
    }
    return f;
}

void tim_forward(int us) {
    timeoff += us;
}

void tim_set_timer(int delta, cb_t cb) {
    if (delta < 10)
        delta = 10;

    rtc_cancel_cb();
    target_disable_irq();
    timer_cb = cb;
    uint16_t nextTrig = TIMx->CNT + (unsigned)delta;
    LL_TIM_OC_SetCompareCH1(TIMx, nextTrig);
    LL_TIM_ClearFlag_CC1(TIMx);
    target_enable_irq();
}

void tim_init() {
    LL_TIM_InitTypeDef TIM_InitStruct = {0};

    /* Peripheral clock enable */
    TIMx_CLK_EN();

    NVIC_SetPriority(TIMx_IRQn, 2);
    NVIC_EnableIRQ(TIMx_IRQn);

    TIM_InitStruct.Prescaler = cpu_mhz - 1;
    TIM_InitStruct.CounterMode = LL_TIM_COUNTERMODE_UP;
    TIM_InitStruct.Autoreload = 0xffff;
    TIM_InitStruct.ClockDivision = LL_TIM_CLOCKDIVISION_DIV1;
    TIM_InitStruct.RepetitionCounter = 0;
    LL_TIM_Init(TIMx, &TIM_InitStruct);
    LL_TIM_DisableARRPreload(TIMx);
    LL_TIM_SetClockSource(TIMx, LL_TIM_CLOCKSOURCE_INTERNAL);
    LL_TIM_SetTriggerOutput(TIMx, LL_TIM_TRGO_RESET);
#ifdef STM32G0
    LL_TIM_SetTriggerOutput2(TIMx, LL_TIM_TRGO2_RESET);
#endif
    LL_TIM_DisableMasterSlaveMode(TIMx);

    LL_TIM_ClearFlag_UPDATE(TIMx);
    LL_TIM_EnableIT_UPDATE(TIMx);

#if 0
    LL_TIM_OC_InitTypeDef TIM_OC_InitStruct = {0};
    TIM_OC_InitStruct.OCMode = LL_TIM_OCMODE_FROZEN;
    TIM_OC_InitStruct.OCState = LL_TIM_OCSTATE_DISABLE;
    TIM_OC_InitStruct.OCNState = LL_TIM_OCSTATE_DISABLE;
    TIM_OC_InitStruct.CompareValue = 1000;
    TIM_OC_InitStruct.OCPolarity = LL_TIM_OCPOLARITY_HIGH;
    LL_TIM_OC_Init(TIM3, LL_TIM_CHANNEL_CH1, &TIM_OC_InitStruct);
    LL_TIM_OC_DisableFast(TIM3, LL_TIM_CHANNEL_CH1);
#endif

    LL_TIM_EnableIT_CC1(TIMx);

    LL_TIM_EnableCounter(TIMx);

    tim_set_timer(5000, NULL);
}

void TIMx_IRQHandler() {
    /* Check whether update interrupt is pending */
    if (LL_TIM_IsActiveFlag_UPDATE(TIMx) == 1) {
        /* Clear the update interrupt flag */
        LL_TIM_ClearFlag_UPDATE(TIMx);
        timeoff += 0x10000;
    }

    cb_t f = NULL;

    // need to disable IRQ while checking flags, otherwise we can get IRQ after
    // check, which can in turn set the timer_cb
    target_disable_irq();
    if (LL_TIM_IsActiveFlag_CC1(TIMx) == 1) {
        f = timer_cb;
        timer_cb = NULL;
        LL_TIM_ClearFlag_CC1(TIMx);
    }
    target_enable_irq();

    if (f)
        f();
}

void tim_update_prescaler() {
    LL_TIM_DisableCounter(TIMx);
    LL_TIM_SetPrescaler(TIMx, cpu_mhz - 1);
    LL_TIM_EnableCounter(TIMx);
}
