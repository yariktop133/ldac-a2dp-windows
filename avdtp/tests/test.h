#ifndef TEST_H
#define TEST_H

#include <stdio.h>

extern int g_failures;
extern int g_checks;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failures++;                                                        \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        long long a_ = (long long)(a), b_ = (long long)(b);                      \
        g_checks++;                                                              \
        if (a_ != b_) {                                                          \
            g_failures++;                                                        \
            fprintf(stderr, "%s:%d: CHECK_EQ failed: %s (%lld) != %s (%lld)\n",  \
                    __FILE__, __LINE__, #a, a_, #b, b_);                         \
        }                                                                        \
    } while (0)

void test_msg(void);
void test_codec(void);
void test_loopback(void);
void test_replay(void);

#endif
