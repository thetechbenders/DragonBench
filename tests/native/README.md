# Native common-state test

This test directly exercises the platform-neutral C workload registry,
validation, state transitions, run identity retention, and abort behavior.

From the repository root on a host with a C11 compiler:

```text
cc -std=c11 -Wall -Wextra -Werror -Ifirmware/common/include \
  firmware/common/db_run.c tests/native/test_db_run.c -o test_db_run
./test_db_run
```

The generated executable is ignored by Git.
