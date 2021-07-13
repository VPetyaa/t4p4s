// SPDX-License-Identifier: Apache-2.0
// Copyright 2018 Eotvos Lorand University, Budapest, Hungary

#include "test.h"

fake_cmd_t t4p4s_testcase_test[][RTE_MAX_LCORE] = {
        {
                FSLEEP(INIT_WAIT_CONTROLPLANE_LONG_MILLIS),
                {
                    FAKE_PKT, 0, 0,
                    ETH("DDDDDDDD0000", ETH01, "0000000000000000000029f0", "12345678", "0a006363", "abcd"),
                    200, 0,
                    ETH("DDDDDDDD0000", ETH01, "0000000000000000000029f0", "12345678", "0a006363", "abcd")
                },
                FEND,
        },

        {
                FEND,
        },
};

testcase_t t4p4s_test_suite[MAX_TESTCASES] = {
        { "test",           &t4p4s_testcase_test },
        TEST_SUITE_END,
};
