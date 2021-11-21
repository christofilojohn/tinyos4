#include <stdio.h>
#include "unit_testing.h"
#include "kernel_sched.h"
#include "tinyos.h"

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
  NULL
};

int main(int argc, char** argv)
{
  return register_test(&all_my_tests) ||
    run_program(argc, argv, &all_my_tests);
}

