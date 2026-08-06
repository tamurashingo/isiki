#ifndef _TEST_ASSERT_H_
#define _TEST_ASSERT_H_

#include <stdio.h>

// 失敗しても最後まで実行を続けるための assert。
// <assert.h> の assert とは違い、成功/失敗どちらも1行出力し、abortしない。
// 全体の成否は g_test_failed を見て main 側で判定する。
static int g_test_failed = 0;

#define assert(cond, msg) do { \
    int _result = !!(cond); \
    printf("test %s:%d: %s, %s ... %s\n", __FILE__, __LINE__, (msg), #cond, _result ? "OK" : "NG"); \
    if (!_result) { g_test_failed = 1; } \
} while (0)

#endif /* _TEST_ASSERT_H_ */
