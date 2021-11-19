#include <stdio.h>
#include "unit_testing.h"
#include "kernel_sched.h"
#include "tinyos.h"

int mock_task(int x, void* data)
{
  for(int i=0; i<x; i++){
    ;  
  }
	return 0;
}

BOOT_TEST(test_sys_CreateThread, "Test that creating a thread with sys_CreateThread works properly"){
  const char *str = "test";
  PTCB* new_thread = (PTCB*) CreateThread(mock_task, sizeof(str), str);
  ASSERT(new_thread->argl == sizeof(str));
  ASSERT(new_thread->args == str);
  ASSERT(new_thread->detached == 0);
  ASSERT(new_thread->exited == 0);
  ASSERT(new_thread->ptcb_list_node.ptcb == new_thread);
  ASSERT(new_thread->refcount == 1);
  ASSERT(new_thread->tcb != NULL);
}

BARE_TEST(my_test, "This is a silly test")
{
  ASSERT(1+1==2);
  ASSERT(2*2*2 < 10);
}

BARE_TEST(impossible_to_fail_test, "This test never fails"){
  ASSERT(1==1);
}

TEST_SUITE(all_my_tests, "These are mine")
{
  &my_test,
  &impossible_to_fail_test,
  &test_sys_CreateThread,
  NULL
};

int main(int argc, char** argv)
{
  return register_test(&all_my_tests) ||
    run_program(argc, argv, &all_my_tests);
}

