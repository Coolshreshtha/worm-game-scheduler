#define _XOPEN_SOURCE
#define _XOPEN_SOURCE_EXTENDED

#include "scheduler.h"

#include <assert.h>
#include <curses.h>
#include <ucontext.h>

#include "util.h"

// This is an upper limit on the number of tasks we can create.
#define MAX_TASKS 128

// This is the size of each task's stack memory
#define STACK_SIZE 65536

// define states of the program
#define NEW_TASK 0
#define WAITING 1
#define SLEEPING 2
#define WAIT_INPUT 3
#define DONE 4
// This struct will hold the all the necessary information for each task
typedef struct task_info {
  // This field stores all the state required to switch back to this task
  ucontext_t context;

  // This field stores another context. This one is only used when the task
  // is exiting.
  ucontext_t exit_context;

  // Keep track of this task's state.
  int state;
  // If the task is sleeping, when should it wake up?
  size_t wake_time;
  // If the task is waiting for another task, which task is it waiting for?
  task_t waiting_for;
  // if waiting for user input, store that here
  int readchar;

} task_info_t;

int current_task = 0;          //< The handle of the currently-executing task
int num_tasks = 1;             //< The number of tasks created so far
task_info_t tasks[MAX_TASKS];  //< Information for every task

/**
 * Initialize the scheduler. Programs should call this before calling any other
 * functiosn in this file.
 */
void scheduler_init() {
  current_task = 0;
  num_tasks = 1;
}

/**
 * This function will execute when a task's function returns. This allows you
 * to update scheduler states and start another task. This function is run
 * because of how the contexts are set up in the task_create function.
 */
void task_exit() {
  // Mark the state of finished task
  tasks[current_task].state = DONE;

  // find next thing to do
  task_t do_next = find_next();
  task_t old_task = current_task;
  current_task = do_next;
  int swap = -1;
  // jump to chosen task
  swap = swapcontext(&tasks[old_task].context, &tasks[do_next].context);
  printf("swapped out to exit\n");
  if (swap == -1) {
    current_task = old_task;
  }
  return;
}

/**
 * Create a new task and add it to the scheduler.
 *
 * \param handle  The handle for this task will be written to this location.
 * \param fn      The new task will run this function.
 */
void task_create(task_t* handle, task_fn_t fn) {
  // Claim an index for the new task
  int index = num_tasks;
  num_tasks++;

  // Set the task handle to this index, since task_t is just an int
  *handle = index;

  // We're going to make two contexts: one to run the task, and one that runs at the end of the task
  // so we can clean up. Start with the second

  // First, duplicate the current context as a starting point
  getcontext(&tasks[index].exit_context);

  // Set up a stack for the exit context
  tasks[index].exit_context.uc_stack.ss_sp = malloc(STACK_SIZE);
  tasks[index].exit_context.uc_stack.ss_size = STACK_SIZE;

  // Set up a context to run when the task function returns. This should call task_exit.
  makecontext(&tasks[index].exit_context, task_exit, 0);

  // Now we start with the task's actual running context
  getcontext(&tasks[index].context);

  // Allocate a stack for the new task and add it to the context
  tasks[index].context.uc_stack.ss_sp = malloc(STACK_SIZE);
  tasks[index].context.uc_stack.ss_size = STACK_SIZE;

  // Now set the uc_link field, which sets things up so our task will go to the exit context when
  // the task function finishes
  tasks[index].context.uc_link = &tasks[index].exit_context;

  // And finally, set up the context to execute the task function
  makecontext(&tasks[index].context, fn, 0);

  // State of new task
  tasks[index].state = NEW_TASK;
  // read character default
  tasks[index].readchar = ERR;
}

/**
 * Wait for a task to finish. If the task has not yet finished, the scheduler should
 * suspend this task and wake it up later when the task specified by handle has exited.
 *
 * \param handle  This is the handle produced by task_create
 */
void task_wait(task_t handle) {
  // block current task
  tasks[current_task].state = WAITING;
  tasks[current_task].waiting_for = handle;

  // find next thing to do
  task_t do_next = find_next();
  task_t old_task = current_task;
  current_task = do_next;
  int swap = -1;
  // jump to new task
  swap = swapcontext(&tasks[old_task].context, &tasks[do_next].context);
  printf("swapped connntext in wait");
  if (swap == -1) {
    current_task = old_task;
  }
  return;
}

/**
 * The currently-executing task should sleep for a specified time. If that time is larger
 * than zero, the scheduler should suspend this task and run a different task until at least
 * ms milliseconds have elapsed.
 *
 * \param ms  The number of milliseconds the task should sleep.
 */
void task_sleep(size_t ms) {
  // block current task
  tasks[current_task].state = SLEEPING;
  tasks[current_task].wake_time = (ms + time_ms());

  // find next thing to do
  task_t do_next = find_next();
  int old = current_task;
  current_task = do_next;
  int swap = -1;
  swap = swapcontext(&tasks[old].context, &tasks[do_next].context);
  if (swap == -1) {
    current_task = old;
  }
  return;
}

/**
 * Read a character from user input. If no input is available, the task should
 * block until input becomes available. The scheduler should run a different
 * task while this task is state
 * .
 *
 * \returns The read character code
 */
int task_readchar() {
  // read input
  int read = getch();

  // return if found input, otherwise choose another task to do
  if (read != ERR) {
    printf("it read a good letter");
    tasks[current_task].readchar = read;
    return read;
  } else {
    // block current task
    tasks[current_task].state = WAIT_INPUT;

    // find next task to do
    task_t do_next = find_next();
    int old = current_task;
    current_task = do_next;
    int swap = -1;
    swap = swapcontext(&tasks[old].context, &tasks[do_next].context);
    if (swap == -1) {
      current_task = old;
    }
    // return the input char read in the scheduler
    return tasks[old].readchar;
  }
}

/** Loop through the tasks array to find the next blocked one
 *
 * \returns The next task to do
 */

task_t find_next() {
  int next_task = current_task + 1;
  int next_index = -1;

  // loop until a task found, keep looping to bide time until something is found
  for (;; next_task++) {
    // task index in array
    next_index = (next_task % num_tasks);

    // behaviour based on current state of task
    switch (tasks[next_index].state) {
      // new tasks are always open to work
      case NEW_TASK:
        return next_index;
      // check the task we are waiting for is finished
      case WAITING: {
        // printf("waiting for task\n");
        if (tasks[tasks[next_index].waiting_for].state == DONE) {
          return next_index;
        } else
          continue;
      }
      // check if sleeping time has elapsed
      case SLEEPING: {
        // printf("sleeping check\n");
        if (time_ms() >= tasks[next_index].wake_time) {
          return next_index;
        } else
          continue;
      }
      // if task is waiting for an input and we get an input, finish it
      case WAIT_INPUT: {
        int read = getch();
        if (read != ERR) {
          tasks[next_index].readchar = read;
          return next_index;
        } else
          continue;
      };
      case DONE:
        continue;
    }
  }
}
