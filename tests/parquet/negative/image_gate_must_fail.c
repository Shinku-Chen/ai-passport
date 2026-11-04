// 门禁负例（tracked）：这个 include 故意不存在。
// tools/parquet_gate.sh 必须把这次编译失败传播为非零退出；如果它把失败吞掉
// （只 echo FAIL 后 exit 0），gate 自己判失败。
#include "parquet_gfx_missing_header_for_negative_gate.h"

int main(void)
{
    return 0;
}
