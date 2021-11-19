#include "tinyos.h"
#include "kernel_sched.h"
#include "kernel_proc.h"
#include "kernel_cc.h"

/**
 * @brief 
 * 
 */

// new function, similar to start_main_thread, as described.
void start_thread(){
  
  int exitval;

  TCB* current_t = cur_thread();

  Task call = current_t -> ptcb -> task;

  int argl = current_t -> ptcb -> argl;
  void* args = current_t -> ptcb -> args;
  exitval = call(argl,args);
  ThreadExit(exitval);

}

/** 
  @brief Create a new thread in the current process.
  */
Tid_t sys_CreateThread(Task task, int argl, void* args)
{
  //ptcb allocation using util function xmalloc
  PTCB* ptcb = xmalloc(sizeof(PTCB));
  TCB* currentTCB;

  //owners pcb thread count increases
  CURPROC -> thread_count++;

  // spawns a thread using our new function
  currentTCB = spawn_thread(CURPROC,start_thread);
  
  //initialization
  currentTCB -> ptcb = ptcb;
  ptcb -> tcb = currentTCB; 
  ptcb -> argl = argl;
  ptcb -> args = args;
  ptcb -> task = task;
  ptcb -> detached = 0; 
  ptcb -> exited = 0;
  // we define refcount to start from 1
  ptcb -> refcount = 1;
  ptcb -> exit_cv = COND_INIT;

  rlnode_init(&ptcb -> ptcb_list_node, ptcb);
  rlist_push_back(&CURPROC->ptcb_list, &ptcb->ptcb_list_node);

  // wakes up the current thread
  wakeup(currentTCB);
  // returns the newly created ptcb 
	return (Tid_t) ptcb;
  
}



/**
  @brief Return the Tid of the current thread.
 */
Tid_t sys_ThreadSelf()
{
	return (Tid_t) cur_thread()->ptcb;
}



/**
  @brief Join the given thread.

  This function will wait for the thread with the given
  tid to exit, and return its exit status in `*exitval`.
  The tid must refer to a legal thread owned by the same
  process that owns the caller. Also, the thread must 
  be undetached, or an error is returned.

  After a call to join succeeds, subsequent calls will fail
  (unless tid was re-cycled to a new thread). 

  It is possible that multiple threads try to join the
  same thread. If these threads block, then all must return the
  exit status correctly.

  @param tid the thread to join
  @param exitval a location where to store the exit value of the joined 
              thread. If NULL, the exit status is not returned.
  @returns 0 on success and -1 on error. Possible errors are:
    - there is no thread with the given tid in this process.
    - the tid corresponds to the current thread.
    - the tid corresponds to a detached thread.

  Documentation taken from tinyos.h to avoid swapping to check it.
  */
int sys_ThreadJoin(Tid_t tid, int* exitval)
{
  
  // check if the given tid is 0 or the same as the current one
  if(tid==0 || tid==ThreadSelf()){
    return -1;
  }

  // find the thread with the given tid, return NULL if unsuccessful
  PCB* curproc = CURPROC;  
  TCB* curthread  = cur_thread();
  rlnode* tmp = rlist_find(&curproc->ptcb_list, (PTCB*)tid, NULL);   // could also be a check like tid->owner_pcb!=CURPROC and probably should be, since we do not know for sure that tid is the PTCB's "key" that rlist_find uses to search
  PTCB* thread_to_join = tmp->ptcb;
  
  // if the search was unsuccessful, exit
  if(thread_to_join == NULL){   
    return -1;
  }

  // check if joining the given thread is allowed
  if(thread_to_join->exited==1 || thread_to_join->detached==1){
    return -1;
  }

  // redundant, paranoid check
  if(thread_to_join->tcb->state != RUNNING){
    return -1;
  }

  // after the checks, we are sure the join is legal

  // remove current thread's PTCB from the sched queue (TODO: change this to take into
  // consideration the multiple queues)
  rlist_remove(&cur_thread()->sched_node);   // the TCB is what is in the sched queue  

  // increase thread_to_join's refcount
  thread_to_join->refcount++;

  // wait on the CondVar
  while(thread_to_join->exited==0 && thread_to_join->detached==0){
    kernel_wait(&curthread->ptcb->exit_cv, SCHED_USER);
  }

  // save the exit value
  if(exitval!=NULL){
    *exitval = thread_to_join->exitval;
  }

  // decrement the refcount
  thread_to_join->refcount--;

  // destroy the ptcb once refcount reaches 0
  if(thread_to_join->refcount==0){
    rlist_remove(&thread_to_join->ptcb_list_node);    // remove the ptcb from the owner process's thread list
    // do not decrease thread count as it only counts "active" threads (and if the thead_to_join's thread has already exited, we may end up with negative thread_count and undefined behavior)
    free(thread_to_join);
  }

	return 0;

}

/**
  @brief Detach the given thread.
  */
int sys_ThreadDetach(Tid_t tid)
{
  PTCB* ptcb = (PTCB*) tid;
	if (ptcb->tcb->state == EXITED || ptcb->tcb == NOTHREAD || ptcb->tcb->owner_pcb!=CURPROC)
  {
    return -1;
  }

  ptcb->detached = 1;
  kernel_broadcast(&ptcb->exit_cv);
  return 0;

}

/**
  @brief Terminate the current thread.
  */
void sys_ThreadExit(int exitval){

  
  PCB* curproc = CURPROC;                 // get current PCB
  PTCB* curptcb = cur_thread()->ptcb;     // get current PTCB
  curproc->thread_count--;

  curptcb->exitval = exitval;           // save the exitval
  curptcb->exited = 1;                  // set the exited flag on the PTCB
  curptcb->refcount--;                  // decrement the refcount
  kernel_broadcast(&curptcb->exit_cv);    // wake up all the threads waiting on this one

  if(curproc->thread_count == 0){   // if we are the last thread, do everything sys_Exit used to do in the original project
    if(get_pid(curproc)!=1){
      PCB* initpcb = get_pcb(1);
      while(!is_rlist_empty(& curproc->children_list)) {
        rlnode* child = rlist_pop_front(& curproc->children_list);
        child->pcb->parent = initpcb;
        rlist_push_front(& initpcb->children_list, child);
      }

    /* Add exited children to the initial task's exited list 
      and signal the initial task */
      if(!is_rlist_empty(& curproc->exited_list)) {
        rlist_append(& initpcb->exited_list, &curproc->exited_list);
        kernel_broadcast(& initpcb->child_exit);
      }

      /* Put me into my parent's exited list */
      rlist_push_front(&curproc->parent->exited_list, &curproc->exited_node);
      kernel_broadcast(& curproc->parent->child_exit);
    }

    assert(is_rlist_empty(& curproc->children_list));
    assert(is_rlist_empty(& curproc->exited_list));


    /* 
      Do all the other cleanup we want here, close files etc. 
    */

    /* Release the args data */
    if(curproc->args) {
      free(curproc->args);
      curproc->args = NULL;
    }

    /* Clean up FIDT */
    for(int i=0;i<MAX_FILEID;i++) {
      if(curproc->FIDT[i] != NULL) {
        FCB_decref(curproc->FIDT[i]);
        curproc->FIDT[i] = NULL;
      }
    }
      
    if(curptcb->refcount == 0){
      rlist_remove(&curptcb->ptcb_list_node);  // remove the PTCB from the PCB's list
      free(curptcb);                          // free the PTCB
    }

    /* Disconnect my main_thread */
    curproc->main_thread = NULL;

    /* Now, mark the process as exited. */
    curproc->pstate = ZOMBIE;

    
  } // from vsam's sys_Exit

  /* Bye-bye cruel world */
  kernel_sleep(EXITED, SCHED_USER);     // exit, and set tcb's state to EXITED



}

