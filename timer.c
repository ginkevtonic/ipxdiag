/* timer.c -- see timer.h. PIT base clock = 1193182 Hz. */

#include <dos.h>
#include <i86.h>
#include <stddef.h>
#include <conio.h>
#include "timer.h"

#define PIT_HZ        1193182UL
#define TARGET_HZ     1000UL
#define CHAIN_EVERY   18        /* call old ISR every 18th tick (~18.2Hz) */

static volatile unsigned long msTicks = 0;
static volatile unsigned long chainCounter = 0;
static void (__interrupt __far *oldInt8)() = (void (__interrupt __far *)())NULL;
static int initialized = 0;

static void __interrupt __far newInt8(void)
{
    msTicks++;
    chainCounter++;

    if (chainCounter >= CHAIN_EVERY) {
        chainCounter = 0;
        _chain_intr(oldInt8);   /* lets BIOS tick/disk-motor timeout etc. run */
        return;                 /* chain_intr does the EOI + iret for us */
    }

    outp(0x20, 0x20);           /* non-terminal ISR: send EOI ourselves */
}

void Timer_Init(void)
{
    unsigned short divisor = (unsigned short)(PIT_HZ / TARGET_HZ);

    if (initialized) return;

    oldInt8 = _dos_getvect(0x08);

    _disable();
    outp(0x43, 0x36);                        /* ch0, lobyte/hibyte, mode3 */
    outp(0x40, (unsigned char)(divisor & 0xFF));
    outp(0x40, (unsigned char)(divisor >> 8));
    _dos_setvect(0x08, newInt8);
    _enable();

    msTicks = 0;
    chainCounter = 0;
    initialized = 1;
}

void Timer_Shutdown(void)
{
    if (!initialized) return;

    _disable();
    outp(0x43, 0x36);
    outp(0x40, 0x00);                        /* divisor 0 == 65536 -> ~18.2Hz, default */
    outp(0x40, 0x00);
    _dos_setvect(0x08, oldInt8);
    _enable();

    initialized = 0;
}

unsigned long Timer_Ms(void)
{
    return msTicks;
}
