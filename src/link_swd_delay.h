#ifndef LINK_SWD_DELAY_H
#define LINK_SWD_DELAY_H

/* Values 1..20 retain the legacy microsecond delays. The two fast
 * profiles specify minimum instruction padding at 60 MHz. GPIO accesses,
 * branches and calls add time; pin frequency requires board measurement. */
#define LINK_SWD_PAD_2MHZ 0xfeu
#define LINK_SWD_PAD_5MHZ 0xffu
static inline __attribute__((always_inline)) void link_swd_half_delay(uint16_t timing)
{
#if LINK_S9_FAST_CLOCK
#if FREQ_SYS != 60000000
#error Fast SWD profiles require the 60 MHz CH592 build
#endif
    if (timing == LINK_SWD_PAD_5MHZ) {
        __asm__ volatile ("nop\n nop" ::: "memory");
        return;
    }
    if (timing == LINK_SWD_PAD_2MHZ) {
        __asm__ volatile (".rept 8\n nop\n .endr" ::: "memory");
        return;
    }
#endif
    DelayUs(timing);
}
#endif
