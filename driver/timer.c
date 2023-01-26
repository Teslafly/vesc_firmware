/*
	Copyright 2019 Benjamin Vedder	benjamin@vedder.se

	This file is part of the VESC firmware.

	The VESC firmware is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    The VESC firmware is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
    */

#include "timer.h"
#include "ch.h"
#include "hal.h"
#include "stm32f4xx_conf.h"

// Settings
#define TIMER_HZ					1.4e7
#define TIMER_GENERAL_PURPOSE       TIM5

void timer_init(void) {
	RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIMER_GENERAL_PURPOSE, ENABLE);
	uint16_t PrescalerValue = (uint16_t) ((SYSTEM_CORE_CLOCK / 2) / TIMER_HZ) - 1;

	TIM_TimeBaseInitTypeDef  TIM_TimeBaseStructure;
	TIM_TimeBaseStructure.TIM_Period = 0xFFFFFFFF;
	TIM_TimeBaseStructure.TIM_Prescaler = PrescalerValue;
	TIM_TimeBaseStructure.TIM_ClockDivision = 0;
	TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;
	TIM_TimeBaseInit(TIMER_GENERAL_PURPOSE, &TIM_TimeBaseStructure);

	TIMER_GENERAL_PURPOSE->CNT = 0;
	TIM_Cmd(TIMER_GENERAL_PURPOSE, ENABLE);
}

uint32_t timer_time_now(void) {
	return TIMER_GENERAL_PURPOSE->CNT;
}

float timer_seconds_elapsed_since(uint32_t time) {
	uint32_t diff = TIMER_GENERAL_PURPOSE->CNT - time;
	return (float)diff / (float)TIMER_HZ;
}

/**
 * Blocking sleep based on timer.
 *
 * @param seconds
 * Seconds to sleep.
 */
void timer_sleep(float seconds) {
	uint32_t start_t = TIMER_GENERAL_PURPOSE->CNT;

	for (;;) {
		if (timer_seconds_elapsed_since(start_t) >= seconds) {
			return;
		}
	}
}
