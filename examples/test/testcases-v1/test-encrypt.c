// SPDX-License-Identifier: Apache-2.0
// Copyright 2018 Eotvos Lorand University, Budapest, Hungary

#include "test.h"

fake_cmd_t t4p4s_testcase_test[][RTE_MAX_LCORE] = {
    {
        {FAKE_PKT, 0, 1,    FDATA("00","68656c6c6f776f726c64617364313200"),NO_CTL_REPLY, 0,
                            FDATA("00","a5c06e42980a6dbbea2fd2c45a94bc77")},
        {FAKE_PKT, 0, 1,    FDATA("01","1168656c6c6f776f726c64617364313200"),NO_CTL_REPLY, 0,
                            FDATA("01","11a5c06e42980a6dbbea2fd2c45a94bc77")},
        {FAKE_PKT, 0, 1,    FDATA("02","112268656c6c6f776f726c64617364313200"),NO_CTL_REPLY, 0,
                            FDATA("02","1122a5c06e42980a6dbbea2fd2c45a94bc77")},
        {FAKE_PKT, 0, 1,    FDATA("03","11223368656c6c6f776f726c64617364313200"),NO_CTL_REPLY, 0,
                            FDATA("03","112233a5c06e42980a6dbbea2fd2c45a94bc77")},
        {FAKE_PKT, 0, 1,    FDATA("04","1122334468656c6c6f776f726c64617364313200"),NO_CTL_REPLY, 0,
                            FDATA("04","11223344a5c06e42980a6dbbea2fd2c45a94bc77")},
        {FAKE_PKT, 0, 1,    FDATA("0b","112233445566778899aabb68656c6c6f776f726c64617364313200"),NO_CTL_REPLY, 0,
                            FDATA("0b","112233445566778899aabba5c06e42980a6dbbea2fd2c45a94bc77")},


        {FAKE_PKT, 0, 1,    FDATA("00","68656c6c6f0000000000000000000000"), NO_CTL_REPLY, 0,
                            FDATA("00","10fb1c3fed5a1d4aa8d60b955b09ff02")},
        {FAKE_PKT, 0, 1,    FDATA("01","68656c6c6f776f726c6461736431323300"), NO_CTL_REPLY, 0,
                            FDATA("01","68cad90cc0231405a0c0d880630b34facf")},
        {FAKE_PKT, 0, 1,    FDATA("01","68656c6c6f776f726c6461736400000000"), NO_CTL_REPLY, 0,
                            FDATA("01","685d8f891abb54cbfe8ef3a330f0853bd5")},
        {FAKE_PKT, 0, 1,    FDATA("02","2268656c6c6f776f726c6461736400000000"), NO_CTL_REPLY, 0,
                            FDATA("02","22685d8f891abb54cbfe8ef3a330f0853bd5")},
        {FAKE_PKT, 0, 1,    FDATA("03","332268656c6c6f776f726c6461736400000000"), NO_CTL_REPLY, 0,
                            FDATA("03","3322685d8f891abb54cbfe8ef3a330f0853bd5")},
        {FAKE_PKT, 0, 1,    FDATA("05","5544332268656c6c6f776f726c6461736400000000"), NO_CTL_REPLY, 0,
                            FDATA("05","55443322685d8f891abb54cbfe8ef3a330f0853bd5")},


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
