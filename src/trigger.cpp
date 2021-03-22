/**
 * @file trigger.cpp
 * @brief Implementaion of a periodic trigger utility
 * @date 2021-01-15
 * @author DIY Embedded Systems (diy.embeddedsytems@gmail.com)
 */

#include "trigger.h"

/**
 * @brief Called frequently, this function returns 1 once per period
 * @param[in,out] pTriggerNextMs points to a variable holding the next trigger date (in milliseconds)
 * @param triggerPeriodMs the period (in milliseconds) of the trigger
 * @return the number of triggers since last call
 * @note the variable pointed to by pTriggerNextMs shall not be modified outside this function
 * 
 * Usage example: start printing "tick" every second, start 12s after platform reset.
 * 
 * void loop() {
 *   static uint32_t nextMs = 12000;
 *   if (periodicTrigger(&nextMs, 1000)) {
 *     Serial.println("Tick!");
 *   }
 * }
 * 
 */
uint8_t periodicTrigger(uint32_t *pTriggerNextMs, const uint32_t triggerPeriodMs) {
  uint8_t res = 0;
  uint32_t nowMs = millis();

  // note: integer division rounds towards zero
  res = (nowMs - *pTriggerNextMs) / triggerPeriodMs;  

  if (res > 0) {
    *pTriggerNextMs += res * triggerPeriodMs;
  } else {
    res = 0;
  }

  return res;
}
