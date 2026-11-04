#!/usr/bin/env bash
set -euo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    echo "Usage: $0 [--all|--static|--firmware]" >&2
}

run_static_checks() {
    local actionlint_bin
    local cc_bin
    local case_dir
    local case_log
    local passed=0
    local failed=0
    local skipped=0
    local skip_notes=()

    # 每个用例的输出、二进制都留在 build 内,不删:交付要能回看单条失败。
    case_dir="${repo_root}/build/logs/host-tests/$(date +%Y%m%d-%H%M%S)"
    mkdir -p "${case_dir}"
    echo "host-test artifacts: ${case_dir}"

    # run_case <name> <command...>:结果一行 PASS/FAIL,完整输出进 case 日志。
    run_case() {
        local name="$1"
        shift
        case_log="${case_dir}/${name}.log"
        if "$@" >"${case_log}" 2>&1; then
            passed=$((passed + 1))
            echo "PASS | ${name} | ok | ${case_log}"
        else
            failed=$((failed + 1))
            echo "FAIL | ${name} | $(tail -n 1 "${case_log}") | ${case_log}"
        fi
    }

    # run_py <name> <python 脚本...>
    run_py() {
        local name="$1"
        shift
        case_log="${case_dir}/${name}.log"
        if PYTHONDONTWRITEBYTECODE=1 python3 "$@" >"${case_log}" 2>&1; then
            passed=$((passed + 1))
            echo "PASS | ${name} | ok | ${case_log}"
        else
            failed=$((failed + 1))
            echo "FAIL | ${name} | $(tail -n 1 "${case_log}") | ${case_log}"
        fi
    }

    skip_case() {
        local name="$1"
        local reason="$2"
        skipped=$((skipped + 1))
        skip_notes+=("${name}: ${reason}")
        case_log="${case_dir}/${name}.log"
        printf 'SKIP %s\n' "${reason}" >"${case_log}"
        echo "SKIP | ${name} | ${reason} | ${case_log}"
    }

    # 宿主机 C 编译器:优先 CC,其次 cc/gcc/clang,最后 tools/cc-zig.sh(ziglang 自带 clang)。
    # 全都没有 = 环境不完整,直接失败;不允许"跳过整组 C 测试还报 PASS"。
    cc_bin="${CC:-}"
    if [[ -z "${cc_bin}" ]]; then
        for candidate in cc gcc clang; do
            if command -v "${candidate}" >/dev/null 2>&1; then
                cc_bin="${candidate}"
                break
            fi
        done
    fi
    if [[ -z "${cc_bin}" ]] && python3 -m ziglang version >/dev/null 2>&1; then
        cc_bin="${repo_root}/tools/cc-zig.sh"
    fi
    if [[ -z "${cc_bin}" ]] || ! command -v "${cc_bin}" >/dev/null 2>&1; then
        echo "ERROR: no host C compiler. Install gcc/clang, or 'pip install ziglang', or set CC." >&2
        return 1
    fi
    echo "host C compiler: ${cc_bin}"

    run_py check_repo tools/check_repo.py

    actionlint_bin="${ACTIONLINT_BIN:-}"
    if [[ -z "${actionlint_bin}" ]]; then
        actionlint_bin="$(command -v actionlint || true)"
    fi
    if [[ -z "${actionlint_bin}" || ! -x "${actionlint_bin}" ]]; then
        actionlint_bin="$(./tools/install-actionlint.sh)"
    fi
    run_case actionlint "${actionlint_bin}" -color .github/workflows/*.yml

    # 基线 BSP / 框架的纯逻辑测试
    run_case ui_pixel_math "${cc_bin}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_ui_pixel_math.c main/ui_pixel_math.c -o "${case_dir}/test_ui_pixel_math"
    run_case ui_pixel_math_run "${case_dir}/test_ui_pixel_math"
    run_case demo_navigation "${cc_bin}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_demo_navigation.c main/demo_navigation.c -o "${case_dir}/test_demo_navigation"
    run_case demo_navigation_run "${case_dir}/test_demo_navigation"
    run_case key_guard "${cc_bin}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_key_guard.c -o "${case_dir}/test_key_guard"
    run_case key_guard_run "${case_dir}/test_key_guard"
    run_case bsp_display_rounding "${cc_bin}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_display_rounding.c components/bsp/src/bsp_display_rounding.c \
        -o "${case_dir}/test_bsp_display_rounding"
    run_case bsp_display_rounding_run "${case_dir}/test_bsp_display_rounding"
    run_case bsp_es8311_sleep "${cc_bin}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_es8311_sleep_check.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${case_dir}/test_bsp_es8311_sleep_check"
    run_case bsp_es8311_sleep_run "${case_dir}/test_bsp_es8311_sleep_check"
    run_case bsp_button "${cc_bin}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include tests/test_bsp_button.c \
        -o "${case_dir}/test_bsp_button"
    run_case bsp_button_run "${case_dir}/test_bsp_button"
    run_case bsp_lvgl_init "${cc_bin}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_lvgl_init.c components/bsp/src/bsp_display_rounding.c \
        -o "${case_dir}/test_bsp_lvgl_init"
    run_case bsp_lvgl_init_run "${case_dir}/test_bsp_lvgl_init"
    run_case bsp_audio_recovery "${cc_bin}" -std=c11 -Wall -Wextra -Werror \
        -Itests/audio_stubs -Icomponents/bsp/include -Icomponents/bsp/src \
        tests/test_bsp_audio_recovery.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${case_dir}/test_bsp_audio_recovery"
    run_case bsp_audio_recovery_run "${case_dir}/test_bsp_audio_recovery"
    run_case gal_text "${cc_bin}" -std=c11 -Wall -Wextra -Werror -Imain -Imain/gal \
        tests/test_gal_text.c main/gal/gal_text.c -o "${case_dir}/test_gal_text"
    run_case gal_text_run "${case_dir}/test_gal_text"
    run_case gal_tone "${cc_bin}" -std=c11 -Wall -Wextra -Werror -Imain -Imain/gal \
        tests/test_gal_tone.c main/gal/gal_tone.c -o "${case_dir}/test_gal_tone"
    run_case gal_tone_run "${case_dir}/test_gal_tone"

    # eden:资源包 / 剧本引擎 / 存档 / 贴图几何(纯逻辑)
    run_case eden_pack "${cc_bin}" -std=c11 -Wall -Wextra -Werror -Imain -Imain/gal \
        tests/test_eden_pack.c main/eden_pack.c main/eden_script.c \
        -o "${case_dir}/test_eden_pack"
    run_case eden_pack_run "${case_dir}/test_eden_pack" main/eden_data/eden_pack.bin
    run_case eden_script "${cc_bin}" -std=c11 -Wall -Wextra -Werror -Imain -Imain/gal \
        tests/test_eden_script.c main/eden_pack.c main/eden_script.c \
        -o "${case_dir}/test_eden_script"
    run_case eden_script_run "${case_dir}/test_eden_script"
    run_case eden_store "${cc_bin}" -std=c11 -Wall -Wextra -Werror -Imain -Imain/gal \
        tests/test_eden_store.c main/eden_store.c -o "${case_dir}/test_eden_store"
    run_case eden_store_run "${case_dir}/test_eden_store"
    run_case eden_image_geom "${cc_bin}" -std=c11 -Wall -Wextra -Werror -Imain -Imain/gal \
        tests/test_eden_image_geom.c -o "${case_dir}/test_eden_image_geom"
    run_case eden_image_geom_run "${case_dir}/test_eden_image_geom"

    # eden:真实解码失败回退(生产 eden_image.c + JPEG/LVGL 替身)
    run_case eden_image "${cc_bin}" -std=c11 -Wall -Wextra -Werror \
        -Itests/app_stubs -Imain -Imain/gal \
        tests/test_eden_image.c tests/app_stubs/stubs.c tests/app_stubs/lvgl_stub.c \
        main/eden_image.c main/eden_pack.c -o "${case_dir}/test_eden_image"
    run_case eden_image_run "${case_dir}/test_eden_image"

    # eden:真实应用层(生产 eden_app.c/eden_ui.c/eden_store_nvs.c + 全替身)
    run_case eden_app "${cc_bin}" -std=c11 -Wall -Wextra -Werror -DEDEN_APP_HOST_TEST \
        -Itests/app_stubs -Imain -Imain/gal \
        tests/test_eden_app.c tests/app_stubs/stubs.c tests/app_stubs/lvgl_stub.c \
        main/eden_app.c main/eden_ui.c main/eden_image.c main/eden_pack.c \
        main/eden_script.c main/eden_store.c main/eden_store_nvs.c \
        main/gal/gal_settings.c main/gal/gal_text.c -o "${case_dir}/test_eden_app"
    run_case eden_app_run "${case_dir}/test_eden_app"

    # 基线硬件自检 demo:靠 --gc-sections 丢掉引用 LVGL/ui_pixel 的 demo_enter。
    # GNU ld 会安静丢弃,LLD(Windows 的 zig 兜底)在链接期报未定义符号。
    if "${cc_bin}" -std=c11 -ffunction-sections -fdata-sections -Itests/demo_stubs -Imain \
            tests/test_demo_audio_runtime.c -Wl,--gc-sections \
            -o "${case_dir}/probe_demo" >"${case_dir}/probe_demo.log" 2>&1; then
        for demo in audio low_power ble wifi; do
            run_case "demo_${demo}_runtime" "${cc_bin}" -std=c11 -Wall -Wextra -Werror \
                -ffunction-sections -fdata-sections -Itests/demo_stubs -Imain \
                "tests/test_demo_${demo}_runtime.c" -Wl,--gc-sections \
                -o "${case_dir}/test_demo_${demo}_runtime"
            run_case "demo_${demo}_runtime_run" "${case_dir}/test_demo_${demo}_runtime"
        done
    else
        skip_case demo_runtime "linker keeps GC'd LVGL demo symbols: $(head -n 1 "${case_dir}/probe_demo.log")"
    fi

    # 字体覆盖:字符集取自资源包与 main/ 的字面量。
    run_py gal_font_check tools/gal_font.py --check \
        --pack main/eden_data/eden_pack.bin --out-dir assets/fonts
    run_py deep_sleep_contract tests/test_deep_sleep_contract.py
    run_py key_wake_contract tests/test_key_wake_contract.py
    run_py check_repo_tests tests/test_check_repo.py
    run_py verify_firmware_tests tests/test_verify_firmware.py
    run_py archive_firmware_tests tests/test_archive_firmware.py

    # 该测试会真实创建目录符号链接:Windows 未开开发者模式/无管理员权限时报 1314。
    if python3 -c "import os,tempfile;d=tempfile.mkdtemp();os.symlink(d,os.path.join(d,'l'),target_is_directory=True)" 2>/dev/null; then
        run_py install_passport_skills tests/test_install_passport_skills.py
    else
        skip_case install_passport_skills "environment cannot create directory symlinks (Windows needs Developer Mode or admin rights)"
    fi

    # eden 资源包 / 解码对照 / 字库覆盖 / 分页容量
    run_py eden_artifacts tests/test_eden_artifacts.py
    # 源 ⇄ 包逐条审计:必须对着源仓库跑。
    if [[ -n "${EDEN_SOURCE:-}" ]]; then
        case_log="${case_dir}/eden_source_audit.log"
        if EDEN_SOURCE="${EDEN_SOURCE}" PYTHONDONTWRITEBYTECODE=1 \
                python3 tests/test_eden_source_audit.py >"${case_log}" 2>&1; then
            passed=$((passed + 1))
            echo "PASS | eden_source_audit | ok | ${case_log}"
        else
            failed=$((failed + 1))
            echo "FAIL | eden_source_audit | $(tail -n 1 "${case_log}") | ${case_log}"
        fi
    else
        skip_case eden_source_audit "EDEN_SOURCE 未指向 eden-rw5 源仓库"
    fi

    echo "------------------------------------------------------------"
    echo "host cases: passed=${passed} failed=${failed} skipped=${skipped}"
    for note in "${skip_notes[@]:-}"; do
        [[ -n "${note}" ]] && echo "  skipped: ${note}"
    done
    if [[ "${failed}" -gt 0 ]]; then
        echo "Host tests: FAIL"
        return 1
    fi
    if [[ "${skipped}" -gt 0 ]]; then
        echo "Host tests: PARTIAL (${passed} passed, ${skipped} skipped)"
        return 0
    fi
    echo "Host tests: PASS (${passed} passed)"
    return 0
}

run_firmware_checks() (
    local validation_build_dir

    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: idf.py is not available; activate ESP-IDF 5.5.3 first." >&2
        return 1
    fi

    # 干净构建放在 build 内、按时间戳分目录并保留:交付要能回看同一次构建的 ELF/MAP,
    # 不做任何永久删除(框架原来的 /tmp + trap rm -rf 不再沿用)。
    validation_build_dir="${repo_root}/build/firmware-check-$(date +%Y%m%d-%H%M%S)"
    mkdir -p "${validation_build_dir}"

    SDKCONFIG_DEFAULTS="${repo_root}/sdkconfig.defaults" \
        idf.py -B "${validation_build_dir}" \
        -D "SDKCONFIG=${validation_build_dir}/sdkconfig" build
    idf.py -B "${validation_build_dir}" merge-bin \
        -o "${validation_build_dir}/FoloToy-AI-Passport-full.bin"
    python3 tools/verify_firmware.py "${validation_build_dir}"
    PYTHONDONTWRITEBYTECODE=1 python3 tools/archive_firmware.py create \
        "${validation_build_dir}" --archive-root "${repo_root}/build/firmware"
    mkdir -p "${repo_root}/build"
    install -m 0644 \
        "${validation_build_dir}/FoloToy-AI-Passport-full.bin" \
        "${repo_root}/build/FoloToy-AI-Passport-full.bin"
    echo "Firmware build: PASS"
)

cd "${repo_root}"
case "${mode}" in
    --all)
        run_static_checks
        run_firmware_checks
        ;;
    --static)
        run_static_checks
        ;;
    --firmware)
        run_firmware_checks
        ;;
    *)
        usage
        exit 2
        ;;
esac
