/*
 * Copyright (c) 2014-2015 ARM. All rights reserved.
 */
/**
 *
 * \file event.c
 * \brief Core event handler.
 *
 *  Event dispatching functions.
 *
 */
#include <string.h>
#include "ns_types.h"
#include "ns_list.h"
#include "eventOS_event.h"
#include "eventOS_scheduler.h"
#include "timer_sys.h"
#include "nsdynmemLIB.h"
#include "ns_timer.h"

#include "platform/arm_hal_interrupt.h"

#include "minar/minar.h"
#include "mbed/FunctionPointer.h"

using minar::Scheduler;
using minar::pre_loop_hook_t;
using mbed::FunctionPointer1;

typedef struct arm_core_tasklet_list_s {
    int8_t id; /**< Event handler Tasklet ID */
    void (*func_ptr)(arm_event_s *);
    ns_list_link_t link;
} arm_core_tasklet_list_s;

typedef struct arm_core_event_s {
    arm_event_s data;
    ns_list_link_t link;
} arm_core_event_s;

static NS_LIST_DEFINE(arm_core_tasklet_list, arm_core_tasklet_list_s, link);
static NS_LIST_DEFINE(event_queue_active, arm_core_event_s, link);
static NS_LIST_DEFINE(free_event_entry, arm_core_event_s, link);

/** Curr_tasklet tell to core and platform which task_let is active, Core Update this automatic when switch Tasklet. */
int8_t curr_tasklet = 0;


static arm_core_tasklet_list_s *tasklet_dynamically_allocate(void);
static arm_core_event_s *event_dynamically_allocate(void);
static arm_core_event_s *event_core_get(void);
static void event_core_write(arm_core_event_s *event);
static pre_loop_hook_t prev_hook = NULL;

static arm_core_tasklet_list_s *event_tasklet_handler_get(uint8_t tasklet_id)
{
    ns_list_foreach(arm_core_tasklet_list_s, cur, &arm_core_tasklet_list) {
        if (cur->id == tasklet_id) {
            return cur;
        }
    }
    return NULL;
}

// XXX this can return 0, but 0 seems to mean "none" elsewhere? Or at least
// curr_tasklet is reset to 0 in various places.
static int8_t tasklet_get_free_id(void)
{
    /*(Note use of uint8_t to avoid overflow if we reach 0x7F)*/
    for (uint8_t i = 0; i <= INT8_MAX; i++) {
        if (!event_tasklet_handler_get(i)) {
            return i;
        }
    }
    return -1;
}


int8_t eventOS_event_handler_create(void (*handler_func_ptr)(arm_event_s *), uint8_t init_event_type)
{
    arm_core_event_s *event_tmp;

    // XXX Do we really want to prevent multiple tasklets with same function?
    ns_list_foreach(arm_core_tasklet_list_s, cur, &arm_core_tasklet_list) {
        if (cur->func_ptr == handler_func_ptr) {
            return -1;
        }
    }

    //Allocate new
    arm_core_tasklet_list_s *new_task = tasklet_dynamically_allocate();
    if (!new_task) {
        return -2;
    }

    event_tmp = event_core_get();
    if (!event_tmp) {
        ns_dyn_mem_free(new_task);
        return -2;
    }

    //Fill in tasklet; add to list
    new_task->id = tasklet_get_free_id();
    new_task->func_ptr = handler_func_ptr;
    ns_list_add_to_end(&arm_core_tasklet_list, new_task);

    //Queue "init" event for the new task
    event_tmp->data.receiver = new_task->id;
    event_tmp->data.sender = 0;
    event_tmp->data.event_type = init_event_type;
    event_tmp->data.event_data = 0;
    event_core_write(event_tmp);

    return new_task->id;
}

/**
* \brief Send event to  event scheduler.
*
* \param event pointer to pushed event.
*
* \return 0 Event push OK
* \return -1 Memory allocation Fail
*
*/
int8_t eventOS_event_send(arm_event_s *event)
{
    int8_t retval = -1;
    if (event_tasklet_handler_get(event->receiver)) {
        arm_core_event_s *event_tmp = event_core_get();
        if (event_tmp) {
            event_tmp->data = *event;
            event_core_write(event_tmp);
            retval = 0;
        }
    }
    return retval;
}


static arm_core_event_s *event_dynamically_allocate(void)
{
    return (arm_core_event_s*)ns_dyn_mem_alloc(sizeof(arm_core_event_s));
}

static arm_core_tasklet_list_s *tasklet_dynamically_allocate(void)
{
    return (arm_core_tasklet_list_s*)ns_dyn_mem_alloc(sizeof(arm_core_tasklet_list_s));
}


arm_core_event_s *event_core_get(void)
{
    arm_core_event_s *event;
    platform_enter_critical();
    event = ns_list_get_first(&free_event_entry);
    if (event) {
        ns_list_remove(&free_event_entry, event);
    } else {
        event = event_dynamically_allocate();
    }
    if (event) {
        event->data.data_ptr = NULL;
        event->data.priority = ARM_LIB_LOW_PRIORITY_EVENT;
    }
    platform_exit_critical();
    return event;
}

static void event_core_free_push(arm_core_event_s *free)
{
    platform_enter_critical();
    ns_list_add_to_start(&free_event_entry, free);
    platform_exit_critical();
}


static arm_core_event_s *event_core_read(void)
{
    arm_core_event_s *event;
    platform_enter_critical();
    event = ns_list_get_first(&event_queue_active);
    if (event) {
        ns_list_remove(&event_queue_active, event);
    }
    platform_exit_critical();
    return event;
}

static void event_callback(arm_core_event_s *cur_event) {
    arm_core_tasklet_list_s *tasklet;
    arm_event_s event;

    platform_enter_critical();
    ns_list_remove(&event_queue_active, cur_event);
    platform_exit_critical();

    curr_tasklet = 0;
    event = cur_event->data;
    event_core_free_push(cur_event);
    tasklet = event_tasklet_handler_get(event.receiver);
    if (tasklet) {
        curr_tasklet = event.receiver;
        /* Tasklet Scheduler Call */
        tasklet->func_ptr(&event);
        /* Set Current Tasklet to Idle state */
        curr_tasklet = 0;
    }
}


void event_core_write(arm_core_event_s *event)
{
    platform_enter_critical();
    bool added = false;
    ns_list_foreach(arm_core_event_s, event_tmp, &event_queue_active) {
        // note enum ordering means we're checking if event_tmp is LOWER priority than event
        if (event_tmp->data.priority > event->data.priority) {
            ns_list_add_before(&event_queue_active, event_tmp, event);
            added = true;
            break;
        }
    }
    if (!added) {
        ns_list_add_to_end(&event_queue_active, event);
    }

    /* Wake From Idle */
    eventOS_scheduler_signal();
    platform_exit_critical();
    Scheduler::postCallback(FunctionPointer1<void, arm_core_event_s*>(event_callback).bind(event)).delay(event->data.priority);
}

/**
 *
 * \brief Initialize Nanostack Core.
 *
 * Function Initialize Nanostack Core, Socket Interface,Buffer memory and Send Init event to all Tasklett which are Defined.
 *
 */
void eventOS_scheduler_init(void)
{
    /* Reset Event List variables */
    ns_list_init(&free_event_entry);
    ns_list_init(&event_queue_active);
    ns_list_init(&arm_core_tasklet_list);

    //Allocate 10 entry
    for (uint8_t i = 0; i < 10; i++) {
        arm_core_event_s *event = event_dynamically_allocate();
        if (event) {
            ns_list_add_to_start(&free_event_entry, event);
        }
    }

    /* Init Generic timer module */
    ns_timer_init();
    timer_sys_init();               //initialize timer
    /* Set Tasklett switcher to Idle */
    curr_tasklet = 0;

}


int8_t eventOS_scheduler_get_active_tasklet(void)
{
    return  curr_tasklet;
}

void eventOS_scheduler_set_active_tasklet(int8_t tasklet)
{
    curr_tasklet = tasklet;
}

int eventOS_scheduler_timer_stop(void)
{
    timer_sys_disable();
    if (ns_timer_sleep() != 0) {
        return 1;
    }
    return 0;
}

int eventOS_scheduler_timer_synch_after_sleep(uint32_t sleep_ticks)
{
    //Update MS to 10ms ticks
    sleep_ticks /= 10;
    sleep_ticks++;
    system_timer_tick_update(sleep_ticks);
    if (timer_sys_wakeup() == 0) {
        return 0;
    }
    return -1;
}

#if 0 // these functions aren't used anymore
/**
 *
 * \brief Infinite Event Read Loop.
 *
 * Function Read and handle Cores Event and switch/enable tasklet which are event receiver. WhenEvent queue is empty it goes to sleep
 *
 */
void event_dispatch_cycle(void)
{
    arm_core_tasklet_list_s *tasklet;
    arm_core_event_s *cur_event;
    arm_event_s event;

    curr_tasklet = 0;

    cur_event =  event_core_read();
    if (cur_event) {
        event = cur_event->data;
        event_core_free_push(cur_event);
        tasklet = event_tasklet_handler_get(event.receiver);
        if (tasklet) {
            curr_tasklet = event.receiver;
            /* Tasklet Scheduler Call */
            tasklet->func_ptr(&event);
            /* Set Current Tasklet to Idle state */
            curr_tasklet = 0;
        }
    } else {
        eventOS_scheduler_idle();
    }
}

/**
 *
 * \brief Infinite Event Read Loop.
 *
 * Function Read and handle Cores Event and switch/enable tasklet which are event receiver. WhenEvent queue is empty it goes to sleep
 *
 */
noreturn void eventOS_scheduler_run(void)
{
    while (1) {
        event_dispatch_cycle();
    }
}
#endif
