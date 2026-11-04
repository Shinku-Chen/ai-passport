#!/usr/bin/env bash
# 《anemoi》本作门禁：宿主 C 测试（真实包/真实 JPEG/真实存储）+ 独立源审计 + 负例。
#
# 用法：./tools/anemoi_gate.sh [OUT_DIR]
#   OUT_DIR 默认 build/anemoi-gate（每次新建目录，不覆盖旧日志）。
#   编译器：$CC，或 cc/gcc/clang，再不行用 python -m ziglang cc。
# 需要先有 main/anemoi_data/anemoi_pack.bin（tools/anemoi_pack.py 生成）。
set -uo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
out_dir="${1:-${repo_root}/build/anemoi-gate}"
mkdir -p "${out_dir}"
exec > >(tee -i "${out_dir}/gate.log") 2>&1

pack="${repo_root}/main/anemoi_data/anemoi_pack.bin"
if [[ ! -f "${pack}" ]]; then
    echo "FAIL | 缺少资源包 ${pack}（先运行 tools/anemoi_pack.py）"
    exit 2
fi

if [[ -n "${CC:-}" ]]; then
    cc_cmd=("${CC}")
elif command -v cc >/dev/null 2>&1; then
    cc_cmd=(cc)
elif command -v gcc >/dev/null 2>&1; then
    cc_cmd=(gcc)
elif command -v clang >/dev/null 2>&1; then
    cc_cmd=(clang)
else
    cc_cmd=(python -m ziglang cc)
fi

common=(-std=c11 -Wall -Wextra -Werror -DANEM_HOST_TEST
        "-I${repo_root}/main/anemoi" "-I${repo_root}/main/anemoi/tjpgd"
        "-I${repo_root}/tests" "-I${repo_root}/tests/anemoi_stubs" -lm)
src_common=("${repo_root}/main/anemoi/anemoi_pack.c" "${repo_root}/main/anemoi/anemoi_script.c"
            "${repo_root}/main/anemoi/anemoi_store.c")

status=0
run_build() {
    local name="$1"
    shift
    local log="${out_dir}/${name}.log"
    if "${cc_cmd[@]}" "${common[@]}" "$@" -o "${out_dir}/${name}" >"${log}" 2>&1; then
        echo "PASS | ${name} | 编译 | ${log}"
    else
        echo "FAIL | ${name} | 编译 | ${log}"
        tail -5 "${log}" | sed 's/^/    /'
        return 1
    fi
}

run_test() {
    local name="$1"
    shift
    local log="${out_dir}/${name}.log"
    if "${out_dir}/${name}" "$@" >>"${log}" 2>&1; then
        echo "PASS | ${name} | 运行 | ${log}"
    else
        echo "FAIL | ${name} | 运行 | ${log}"
        tail -5 "${log}" | sed 's/^/    /'
        return 1
    fi
}

echo "== anemoi gate $(date '+%Y-%m-%d %H:%M:%S') ==" | tee "${out_dir}/gate.log"

run_build test_pack "${repo_root}/tests/test_anemoi_pack.c" \
    "${repo_root}/main/anemoi/anemoi_pack.c" && run_test test_pack "${pack}" || status=1

run_build test_script "${repo_root}/tests/test_anemoi_script.c" "${src_common[@]}" \
    && run_test test_script "${pack}" || status=1

run_build test_codec_crc_neg "${repo_root}/tests/test_anemoi_codec_crc_neg.c" "${src_common[@]}" \
    && run_test test_codec_crc_neg "${pack}" || status=1
run_build test_codec_neg "${repo_root}/tests/test_anemoi_codec_neg.c" "${src_common[@]}" \
    && run_test test_codec_neg || status=1
run_build test_store "${repo_root}/tests/test_anemoi_store.c" "${src_common[@]}" \
    && run_test test_store "${pack}" || status=1

run_build test_image "${repo_root}/tests/test_anemoi_image.c" \
    "${repo_root}/main/anemoi/anemoi_canvas.c" "${repo_root}/main/anemoi/anemoi_image.c" \
    "${repo_root}/main/anemoi/anemoi_pack.c" "${repo_root}/main/anemoi/anemoi_script.c" \
    "${repo_root}/main/anemoi/tjpgd/tjpgd.c" "${repo_root}/tests/anemoi_stubs/tinfl_shim.c" \
    && run_test test_image "${repo_root}/tests/anemoi/fixtures/images_pack.bin" || status=1

run_build test_startup "${repo_root}/tests/test_anemoi_startup.c" "${repo_root}/main/anemoi/anemoi_startup.c" && run_test test_startup || status=1
run_build test_semantics "${repo_root}/tests/test_anemoi_semantics.c" "${src_common[@]}"     && run_test test_semantics "${pack}" || status=1

run_build test_app "${repo_root}/tests/test_anemoi_app.c" "${src_common[@]}" \
    "${repo_root}/main/anemoi/anemoi_ui.c" "${repo_root}/main/anemoi/anemoi_app.c" \
    "${repo_root}/main/anemoi/anemoi_canvas.c" "${repo_root}/main/anemoi/anemoi_image.c" \
    "${repo_root}/main/anemoi/tjpgd/tjpgd.c" "${repo_root}/tests/anemoi_stubs/tinfl_shim.c" \
    && run_test test_app "${pack}" || status=1

# 负例：坏代码必须编译失败并以非零退出（不能只 echo FAIL 后 exit 0）。
negative_src="${out_dir}/negative_compile.c"
printf '#include "anemoi_image.h"\nint main(void){ return anemoi_this_symbol_does_not_exist(); }\n' \
    >"${negative_src}"
if "${cc_cmd[@]}" "${common[@]}" "${negative_src}" -o "${out_dir}/negative" \
    >"${out_dir}/negative-compile.log" 2>&1; then
    echo "FAIL | negative-compile | 坏代码竟然编译通过 | ${out_dir}/negative-compile.log"
    status=1
else
    echo "PASS | negative-compile | 编译失败按预期非零退出 | ${out_dir}/negative-compile.log"
fi

if [[ -n "${ANEMOI_SOURCE:-}" ]]; then
    if python "${repo_root}/tools/anemoi_audit.py" --source "${ANEMOI_SOURCE}" --pack "${pack}" \
        --json "${out_dir}/audit.json" >"${out_dir}/audit.log" 2>&1; then
        echo "PASS | source-audit | 独立源逐 op 审计 | ${out_dir}/audit.log"
    else
        echo "FAIL | source-audit | ${out_dir}/audit.log"
        tail -5 "${out_dir}/audit.log" | sed 's/^/    /'
        status=1
    fi
else
    echo "SKIP | source-audit | 未设置 ANEMOI_SOURCE（缺源；汇总 PARTIAL，但已有 FAIL 优先）"
    # FAIL 优先：缺源只能是 PARTIAL，不得覆盖先前的 FAIL(1)。
    if [[ "${status}" -eq 0 ]]; then
        status=2
    fi
fi

if python "${repo_root}/tests/test_anemoi_artifacts.py" >"${out_dir}/artifacts.log" 2>&1; then
    echo "PASS | artifacts | 清单/哈希/负例 | ${out_dir}/artifacts.log"
else
    echo "FAIL | artifacts | ${out_dir}/artifacts.log"
    tail -5 "${out_dir}/artifacts.log" | sed 's/^/    /'
    status=1
fi

echo "----"
echo "SUMMARY | status=${status} | PASS=$(grep -c '^PASS' "${out_dir}/gate.log" 2>/dev/null || echo 0) FAIL=$(grep -c '^FAIL' "${out_dir}/gate.log" 2>/dev/null || echo 0) SKIP=$(grep -c '^SKIP' "${out_dir}/gate.log" 2>/dev/null || echo 0)"
exit "${status}"
