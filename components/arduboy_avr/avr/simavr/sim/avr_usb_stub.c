#include "avr_usb.h"

#if defined(SIMAVR_STUB_USB)
/*
 * Minimal PLLCSR behaviour for games that wait for the USB PLL to lock.
 * On real hardware PLOCK (bit0) turns 1 once PLLE (bit1) is set and the
 * PLL has locked.  We emulate the locked state immediately: any write that
 * sets PLLE also sets PLOCK, so games polling PLLCSR.0 don't hang forever.
 */
static void
usb_pll_write(struct avr_t * avr, avr_io_addr_t addr, uint8_t v, void * param)
{
    (void)param;
    v |= (v >> 1) & 1;      /* PLLE -> PLOCK */
    avr_core_watch_write(avr, addr, v);
}

void avr_usb_init(avr_t *avr, avr_usb_t *port)
{
    if (port->r_pllcsr)
        avr_register_io_write(avr, port->r_pllcsr, usb_pll_write, port);
}
#endif

