#include "config.h"

void SysTick_Handler(void)      { HAL_IncTick(); }
void EXTI9_5_IRQHandler(void)   { HAL_GPIO_EXTI_IRQHandler(OVM_PIN); }   /* OVM on PA9 */

void NMI_Handler(void)          { }
void HardFault_Handler(void)    { for (;;) { } }
void MemManage_Handler(void)    { for (;;) { } }
void BusFault_Handler(void)     { for (;;) { } }
void UsageFault_Handler(void)   { for (;;) { } }
void SVC_Handler(void)          { }
void DebugMon_Handler(void)     { }
void PendSV_Handler(void)       { }
