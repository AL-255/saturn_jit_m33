/* Minimal Cortex-M33 startup for QEMU mps2-an505.
 * Vector table at 0x10000000 (code memory). The only entries we care
 * about are the initial MSP value and the reset handler; everything
 * else stays in a default-handler loop so an unexpected fault is
 * visible under gdb. */

#include <stdint.h>

extern uint32_t _estack;          /* top of stack, from linker */
extern uint32_t _sdata, _edata;   /* .data in RAM */
extern uint32_t _sidata;          /* .data load address in flash */
extern uint32_t _sbss, _ebss;

void Reset_Handler(void);
void Default_Handler(void);
int  main(void);

__attribute__((used, section(".isr_vector")))
const void * const g_pfnVectors[] = {
    (void *)&_estack,
    (void *)Reset_Handler,
    /* NMI..SysTick — all default for smoke */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler,
};

void Reset_Handler(void) {
    /* copy .data from flash to ram */
    uint32_t *src = &_sidata;
    for (uint32_t *dst = &_sdata; dst < &_edata; ) *dst++ = *src++;
    /* zero .bss */
    for (uint32_t *dst = &_sbss;  dst < &_ebss;  ) *dst++ = 0;
    (void)main();
    /* if main returns, halt */
    for (;;) __asm__ volatile ("wfi");
}

void Default_Handler(void) { for (;;) __asm__ volatile ("bkpt #0"); }
