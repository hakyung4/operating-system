// timer.h
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifndef _TIMER_H_
#define _TIMER_H_

#include <stdint.h>
#include "thread.h" // for struct condition
#include "trap.h" // for struct trap_frame
#include "conf.h" // for TIMER_FREQ


struct alarm {
    struct condition cond;
    struct alarm * next;
    unsigned long long twake;
};

// EXPORTED FUNCTION DECLARATIONS
//

extern char timer_initialized;
extern void timer_init(void);

// Initializes an alarm. The _name_ argument is optional.

extern void alarm_init(struct alarm * al, const char * name);

// Puts the current thread to sleep for some number of ticks. The _tcnt_
// argument specifies the number of timer ticks relative to the most recent
// alarm event, either init, wake-up, or reset.

extern void alarm_sleep(struct alarm * al, unsigned long long tcnt);

// Resets the alarm so that the next sleep increment is relative to the time
// of this function call.

extern void alarm_reset(struct alarm * al);

extern void alarm_sleep_sec(struct alarm * al, unsigned int sec);
extern void alarm_sleep_ms(struct alarm * al, unsigned long ms);
extern void alarm_sleep_us(struct alarm * al, unsigned long us);

extern void sleep_sec(unsigned int sec);
extern void sleep_ms(unsigned long ms);
extern void sleep_us(unsigned long us);

extern void handle_timer_interrupt(void); // called from trap.s

#endif // _TIMER_H_