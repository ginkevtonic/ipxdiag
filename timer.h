/* timer.h -- 1ms-resolution timer, needed because the default BIOS tick
 * (~54.9ms) is far too coarse to measure LAN/wifi round-trip times.
 *
 * Reprograms PIT channel 0 to ~1000Hz and hooks INT 8, chaining to the
 * original handler often enough (every 18th tick) to keep the BIOS clock
 * and any DOS/DOSBox timing-dependent behaviour working normally.
 * MUST call Timer_Shutdown() before exiting, including on Ctrl-Break --
 * install a break handler in the caller if you want to be thorough.
 */

#ifndef TIMER_H
#define TIMER_H

void Timer_Init(void);
void Timer_Shutdown(void);
unsigned long Timer_Ms(void);   /* milliseconds since Timer_Init() */

#endif
