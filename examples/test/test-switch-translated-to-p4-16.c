// SPDX-License-Identifier: Apache-2.0
// Copyright 2021 Eotvos Lorand University, Budapest, Hungary

#include "test.h"

fake_cmd_t t4p4s_testcase_test[][RTE_MAX_LCORE] = {
    {
        {FAKE_PKT, 0, 1, IPV4(ETH01, cIP4_0, ETH1A, cIP4_0), NO_CTL_REPLY, 11, IPV4(ETH01, cIP4_0, ETH1A, cIP4_0)},
        FEND,
    },

    {
        FEND,
    },
};

testcase_t t4p4s_test_suite[MAX_TESTCASES] = {
    { "test",           &t4p4s_testcase_test, "v1model" },
    TEST_SUITE_END,
};
