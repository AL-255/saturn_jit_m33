/* Cortex-M33 startup, shared by all firmware targets in this tree. */

#include <stdint.h>

extern uint32_t _estack;
extern uint32_t _sdata, _edata, _sidata;
extern uint32_t _sbss, _ebss;

void Reset_Handler(void);
void Default_Handler(void);
int  main(void);

__attribute__((used, section(".isr_vector")))
const void * const g_pfnVectors[] = {
    (void *)&_estack,
    (void *)Reset_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler,
};

void Reset_Handler(void) {
    uint32_t *src = &_sidata;
    for (uint32_t *dst = &_sdata; dst < &_edata; ) *dst++ = *src++;
    for (uint32_t *dst = &_sbss;  dst < &_ebss;  ) *dst++ = 0;
    (void)main();
    for (;;) __asm__ volatile ("wfi");
}
void Default_Handler(void) { for (;;) __asm__ volatile ("bkpt #0"); }
