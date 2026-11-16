/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "fm1_device.h"
#include "fm1_audio.h"
#define REG32(address) (*(volatile uint32_t *)(address))
static void (*input_scan)(void);
/* Diagnostics (transfer command 23): the longest run of this interrupt in the
 * audio timer's ticks, wall time (a higher interrupt inside it counts too).
 * Two timer reads, a compare and a store; audio_output.c owns the word. */
extern volatile uint32_t fm1_panel_irq_peak;
static FM1_AUDIO_CODE __attribute__((interrupt(""))) void fm1_panel_irq(void)
{
    uint32_t start = FM1_AUDIO_TIMER_COUNT;
    REG32(0x10900U) |= 0x4000U;
    if (input_scan) input_scan();
    uint32_t end = FM1_AUDIO_TIMER_COUNT;
    uint32_t took = end >= start ? end - start : end + FM1_AUDIO_TIMER_PERIOD + 1U - start;
    if (took > fm1_panel_irq_peak) fm1_panel_irq_peak = took;
}
void fm1_device_start(void (*scan)(void))
{
    /* TIMER4: free-running 24 MHz timebase; TIMER5: 10 kHz input scan. */
    REG32(0x10800U) = 0;
    REG32(0x10808U) = 0xffffffffU;
    REG32(0x10804U) = 0;
    REG32(0x10800U) = 0x4009U;
    fm1_device_adc_register(3U); /* PB01 battery */
    fm1_device_adc_register(4U); /* PB06 master */
    input_scan = scan;
    REG32(0x10900U) = 0x4000U;
    REG32(0x10904U) = 0;
    REG32(0x10908U) = 600U;
    fm1_device_irq_attach(63U, fm1_panel_irq, 5U);
    REG32(0x10900U) = 0x4019U;
}
