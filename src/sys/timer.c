// timer.c
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef TIMER_TRACE
#define TRACE
#endif

#ifdef TIMER_DEBUG
#define DEBUG
#endif

#include "timer.h"
#include "thread.h"
#include "riscv.h"
#include "assert.h"
#include "intr.h"
#include "conf.h"
#include "see.h" // for set_stcmp

// EXPORTED GLOBAL VARIABLE DEFINITIONS
// 

char timer_initialized = 0;

// INTERNVAL GLOBAL VARIABLE DEFINITIONS
//

static struct alarm * sleep_list;

// INTERNAL FUNCTION DECLARATIONS
//

// EXPORTED FUNCTION DEFINITIONS
//

void timer_init(void) {
    set_stcmp(UINT64_MAX);
    timer_initialized = 1;
}

// void alarm_init(struct alarm * al, const char * name)
// Inputs: 
//   struct alarm * al - Pointer to an alarm structure to be initialized.
//   const char * name - Name identifier for the alarm, or NULL for default name.
// Outputs: 
//   None
// Description: 
//   Initializes an alarm structure, setting its name, clearing its wait list, 
//   and setting its wake-up time to the current system time.
// Side Effects: 
//   Modifies the given alarm structure.

void alarm_init(struct alarm * al, const char * name) {
    // FIXME your code goes here
    if(name == NULL){
        al->cond.name = "alarm";
    }
    else{
        al->cond.name = name;
    }

    al->cond.wait_list.head = NULL;
    al->cond.wait_list.tail = NULL;

    al->next = NULL;

    al->twake = rdtime(); //only thing that makes sense

}

// void alarm_sleep(struct alarm * al, unsigned long long tcnt)
// Inputs: 
//   struct alarm * al - Pointer to an alarm structure representing the sleep request.
//   unsigned long long tcnt - The number of timer counts to sleep before waking up.
// Outputs: 
//   None
// Description: 
//   Puts the calling thread to sleep until the specified number of timer counts elapses.
//   The function inserts the alarm into the sleep queue and enables timer interrupts if needed.
// Side Effects: 
//   Modifies the global sleep queue and puts the calling thread to sleep.

void alarm_sleep(struct alarm * al, unsigned long long tcnt) {
    unsigned long long now;
    unsigned long sie_val;

    int pie;

    now = rdtime();

    // If the tcnt is so large it wraps around, set it to UINT64_MAX

    if (UINT64_MAX - al->twake < tcnt)
        al->twake = UINT64_MAX;
    else
        al->twake += tcnt;
    
    // If the wake-up time has already passed, return

    if (al->twake < now)
        return;


    // FIXME your code goes here

    //insertion into linked list logic

    pie = disable_interrupts();

    if (sleep_list == NULL || al->twake < sleep_list->twake) { // if alarm is going to be the new head
        al->next = sleep_list; // make it the new head
        sleep_list = al; // adjust sleep list pointer to the new head
    } else {
        struct alarm *prev = sleep_list;
        struct alarm *curr = sleep_list->next;
        while (curr != NULL && curr->twake <= al->twake) { // find the correct place to insert this alarm based on twake
            prev = curr;
            curr = curr->next;
        }
        al->next = curr;
        prev->next = al;
    }

    if(sleep_list == al){
        set_stcmp(al->twake); // if alarm is the new head mtcmp gets set to this alarm's twake
    }

    //put thread to sleep?


    //enable timer interrupts

    sie_val = RISCV_SIE_STIE;
    csrs_sie(sie_val);


    condition_wait(&al->cond); // wait until this alarm gets awoken

    restore_interrupts(pie);


}

// Resets the alarm so that the next sleep increment is relative to the time
// alarm_reset is called.

void alarm_reset(struct alarm * al) {
    al->twake = rdtime();
}

void alarm_sleep_sec(struct alarm * al, unsigned int sec) {
    alarm_sleep(al, sec * TIMER_FREQ);
}

void alarm_sleep_ms(struct alarm * al, unsigned long ms) {
    alarm_sleep(al, ms * (TIMER_FREQ / 1000));
}

void alarm_sleep_us(struct alarm * al, unsigned long us) {
    alarm_sleep(al, us * (TIMER_FREQ / 1000 / 1000));
}

void sleep_sec(unsigned int sec) {
    sleep_ms(1000UL * sec);
}

void sleep_ms(unsigned long ms) {
    sleep_us(1000UL * ms);
}

void sleep_us(unsigned long us) {
    struct alarm al;

    alarm_init(&al, "sleep");
    alarm_sleep_us(&al, us);
}

// handle_timer_interrupt() is dispatched from intr_handler in intr.c

// void handle_timer_interrupt(void)
// Inputs: 
//   None
// Outputs: 
//   None
// Description: 
//   Handles a timer interrupt by checking the sleep queue, waking up expired alarms, and updating the timer comparator register.
//   If no alarms remain in the queue, timer interrupts are disabled.
// Side Effects: 
//   Modifies the sleep queue, potentially wakes up threads, and updates the timer comparator register.

void handle_timer_interrupt(void) {
    struct alarm * head = sleep_list;
    struct alarm * next;
    uint64_t now;
    unsigned long sie_val;

    now = rdtime();

    trace("[%lu] %s()", now, __func__);
    debug("[%lu] mtcmp = %lu", now, rdtime());

    if(head == NULL){
        //disable timer interrupts somehow?
        return;
    }

    while(head != NULL && head->twake <= now){
        next = head->next;
        condition_broadcast(&head->cond); // as we wake up the alarms we need to broadcast
        head = next;
    }

    sleep_list = head; // once we have awoken the necessary alarms, we need to set sleep_list to the first alarm that is not awake

    if(sleep_list != NULL){
        set_stcmp(sleep_list->twake); // if there are still alarms in the sleep list mtcmp gets updated with the smallest twake
    }
    else{
        //disable timer interrupts here 
        sie_val = RISCV_SIE_STIE;
        csrc_sie(sie_val);
    }

    return;
    
    // FIXME your code goes here
}