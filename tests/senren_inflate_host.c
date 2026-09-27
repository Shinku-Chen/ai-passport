// tests/senren_inflate_host.c —— 宿主机上的 inflate 实现(给 tests/test_senren_model.c 用)。
//
// 固件走 ROM 里的 tinfl(main/senren_inflate.c),宿主机没有那套 ROM 符号,这里用 zlib
// 实现同一份接口,让数据层测试与真机走同样的调用契约。编译时要加 -lz。
#include "senren_inflate.h"

#include <string.h>
#include <zlib.h>

uint32_t senren_inflate(const uint8_t *src, uint32_t src_len, uint8_t *dst, uint32_t dst_cap)
{
    if (src == NULL || dst == NULL || dst_cap == 0) {
        return 0;
    }
    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    if (inflateInit(&stream) != Z_OK) {   // 包里是 zlib 格式,与固件同一约定
        return 0;
    }
    stream.next_in = (Bytef *)src;
    stream.avail_in = src_len;
    stream.next_out = dst;
    stream.avail_out = dst_cap;
    const int rc = inflate(&stream, Z_FINISH);
    const uint32_t produced = dst_cap - stream.avail_out;
    inflateEnd(&stream);
    return rc == Z_STREAM_END ? produced : 0;
}

bool senren_inflate_stream(const uint8_t *src, uint32_t src_len, senren_sink_t sink, void *context)
{
    if (src == NULL || sink == NULL || src_len == 0) {
        return false;
    }
    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    if (inflateInit(&stream) != Z_OK) {
        return false;
    }
    stream.next_in = (Bytef *)src;
    stream.avail_in = src_len;
    uint8_t chunk[1024];
    bool ok = true;
    for (int guard = 0; guard < 1000000; guard++) {
        stream.next_out = chunk;
        stream.avail_out = sizeof(chunk);
        const int rc = inflate(&stream, Z_NO_FLUSH);
        const uint32_t produced = (uint32_t)(sizeof(chunk) - stream.avail_out);
        if (produced > 0 && !sink(chunk, produced, context)) {
            ok = false;
            break;
        }
        if (rc == Z_STREAM_END) {
            break;
        }
        if (rc != Z_OK && rc != Z_BUF_ERROR) {
            ok = false;
            break;
        }
        if (produced == 0 && stream.avail_in == 0) {
            ok = false;
            break;
        }
    }
    inflateEnd(&stream);
    return ok;
}
