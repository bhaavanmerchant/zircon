// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <threads.h>

#include <zircon/syscalls.h>
#include <zircon/threads.h>
#include <unittest/unittest.h>

static volatile int threads_done[7];

static int thread_entry(void* arg) {
    int thread_number = (int)(intptr_t)arg;
    errno = thread_number;
    unittest_printf("thread %d sleeping for .1 seconds\n", thread_number);
    zx_nanosleep(zx_deadline_after(ZX_MSEC(100)));
    EXPECT_EQ(errno, thread_number, "errno changed by someone!");
    threads_done[thread_number] = 1;
    return thread_number;
}

bool c11_thread_test(void) {
    BEGIN_TEST;

    thrd_t thread;
    int return_value = 99;

    unittest_printf("Welcome to thread test!\n");

    memset((void*)threads_done, 0, sizeof(threads_done));
    for (int i = 0; i != 4; ++i) {
        int return_value = 99;
        int ret = thrd_create_with_name(&thread, thread_entry, (void*)(intptr_t)i, "c11 thread test");
        ASSERT_EQ(ret, thrd_success, "Error while creating thread");

        ret = thrd_join(thread, &return_value);
        ASSERT_EQ(ret, thrd_success, "Error while thread join");
        ASSERT_EQ(return_value, i, "Incorrect return from thread");
    }

    unittest_printf("Attempting to create thread with a null name. This should succeed\n");
    int ret = thrd_create_with_name(&thread, thread_entry, (void*)(intptr_t)4, NULL);
    ASSERT_EQ(ret, thrd_success, "Error returned from thread creation");
    zx_handle_t handle = thrd_get_zx_handle(thread);
    ASSERT_NE(handle, ZX_HANDLE_INVALID, "got invalid thread handle");
    // Prove this is a valid handle by duplicating it.
    zx_handle_t dup_handle;
    zx_status_t status = zx_handle_duplicate(handle, ZX_RIGHT_SAME_RIGHTS, &dup_handle);
    ASSERT_EQ(status, 0, "failed to duplicate thread handle");

    ret = thrd_join(thread, &return_value);
    ASSERT_EQ(ret, thrd_success, "Error while thread join");
    ASSERT_EQ(zx_handle_close(dup_handle), ZX_OK, "failed to close duplicate handle");
    ASSERT_EQ(return_value, 4, "Incorrect return from thread");

    ret = thrd_create_with_name(&thread, thread_entry, (void*)(intptr_t)5, NULL);
    ASSERT_EQ(ret, thrd_success, "Error returned from thread creation");
    ret = thrd_detach(thread);
    ASSERT_EQ(ret, thrd_success, "Error while thread detach");

    while (!threads_done[5])
        zx_nanosleep(zx_deadline_after(ZX_MSEC(100)));

    thread_entry((void*)(intptr_t)6);
    ASSERT_TRUE(threads_done[6], "All threads should have completed");

    END_TEST;
}

bool long_name_succeeds(void) {
    BEGIN_TEST;

    // Creating a thread with a super long name should succeed.
    static const char long_name[] =
        "0123456789012345678901234567890123456789"
        "0123456789012345678901234567890123456789";
    ASSERT_GT(strlen(long_name), (size_t)ZX_MAX_NAME_LEN-1,
              "too short to truncate");

    thrd_t thread;
    int ret = thrd_create_with_name(
        &thread, thread_entry, (void*)(intptr_t)0, long_name);
    ASSERT_EQ(ret, thrd_success, "long name should have succeeded");

    // Clean up.
    int return_value;
    EXPECT_EQ(thrd_join(thread, &return_value), thrd_success, "");
    END_TEST;
}

static int detach_thrd(void* arg) {
    BEGIN_HELPER;
    thrd_t* thrd = (thrd_t*) arg;
    EXPECT_EQ(thrd_detach(*thrd), 0, "");
    free(thrd);
    END_HELPER;
}

bool detach_self_test(void) {
    BEGIN_TEST;

    for (size_t i = 0; i < 1000; i++) {
        thrd_t* thrd = calloc(sizeof(thrd_t), 1);
        ASSERT_NONNULL(thrd, "");
        ASSERT_EQ(thrd_create(thrd, detach_thrd, thrd), 0, "");
    }

    END_TEST;
}

BEGIN_TEST_CASE(c11_thread_tests)
RUN_TEST(c11_thread_test)
RUN_TEST(long_name_succeeds)
RUN_TEST(detach_self_test)
END_TEST_CASE(c11_thread_tests)
