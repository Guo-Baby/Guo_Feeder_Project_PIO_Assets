// =====================================================
// P1.1 Contract Test —— 负向探针（证明断言真的是"活的"）
//
// 本文件**故意**包含一个必然失败的断言，用于证明：
//   log_events.h 内的 static_assert 确实在本工具链下被求值，
//   而不是被跳过 / 被预处理器吞掉。
//
// 期望：编译**必须失败**，且报错信息为本文件的 assert 文本。
//
// 运行方式见同目录 run_contract_test.sh
//   —— 若本文件"编译通过"，则说明 Contract Test 形同虚设，判定为 FAIL。
// =====================================================

#include "log_events.h"

// 故意错误：契约冻结为 31 条/段（3984 B），这里断言 32 试图触发失败
static_assert(LOG_RECORDS_PER_SEGMENT == 32u,
              "NEGATIVE PROBE: this MUST fail to compile");
