// SPDX-License-Identifier: Apache-2.0

#include <zxconfig/fuzzy.hpp>

#include <gtest/gtest.h>

TEST(Fuzzy, empty_pattern_matches_everything) {
    int score = -1;
    EXPECT_TRUE(zxconfig::fuzzy_match("anything", "", score));
    EXPECT_EQ(score, 0);
}

TEST(Fuzzy, is_case_insensitive) {
    int score = 0;
    EXPECT_TRUE(zxconfig::fuzzy_match("NR_CPUS", "nr", score));
    EXPECT_TRUE(zxconfig::fuzzy_match("nr_cpus", "CP", score));
}

TEST(Fuzzy, rejects_a_non_subsequence) {
    int score = 0;
    EXPECT_FALSE(zxconfig::fuzzy_match("SMP", "xyz", score));
    EXPECT_FALSE(zxconfig::fuzzy_match("SMP", "SPMX", score));
}

TEST(Fuzzy, ranks_prefixes_above_scattered) {
    int prefix = 0;
    int scattered = 0;
    ASSERT_TRUE(zxconfig::fuzzy_match("PREEMPT", "pre", prefix));
    ASSERT_TRUE(zxconfig::fuzzy_match("PARENT", "pre", scattered));
    EXPECT_GT(prefix, scattered);
}

TEST(Fuzzy, ranks_consecutive_above_gappy) {
    int consecutive = 0;
    int gappy = 0;
    ASSERT_TRUE(zxconfig::fuzzy_match("VIRTIO_NET", "net", consecutive));
    ASSERT_TRUE(zxconfig::fuzzy_match("N_E_T", "net", gappy));
    EXPECT_GT(consecutive, gappy);
}

TEST(Fuzzy, rewards_word_boundaries) {
    int boundary = 0;
    int middle = 0;
    ASSERT_TRUE(zxconfig::fuzzy_match("CONFIG_NET", "net", boundary));
    ASSERT_TRUE(zxconfig::fuzzy_match("CONFIGNET", "net", middle));
    EXPECT_GT(boundary, middle);
}
