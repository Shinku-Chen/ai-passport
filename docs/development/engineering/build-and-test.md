<p align="right">
  <a href="build-and-test.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Build and Test

Use ESP-IDF 5.5.3. On a clean machine or when the toolchain is missing, follow
the [environment bootstrap](environment-setup.md) first.

> **No original-firmware backup is required before downloading (flashing) new
> firmware to the device.** Reading out the installed firmware is not a
> prerequisite. Flashing replaces the installed firmware and does not provide
> automatic restoration of it. This does not mean user data is preserved: if
> you need existing settings or records, export or otherwise save them first.
> See [flashing and stored data](firmware-layout.md#flashing-and-stored-data).

> Prefer `./tools/validate.sh --firmware` for firmware builds. Flash its
> verified `build/FoloToy-AI-Passport-full.bin` at offset `0x0` for a blank
> device or an intentional complete refresh. The merged image may reset NVS;
> use segmented `idf.py flash` when existing NVS state must be preserved. Treat
> `idf.py build` and `idf.py flash` as incremental development commands, not the
> default delivery path.

```bash
source <path-to-esp-idf-v5.5.3>/export.sh
idf.py --version             # must report ESP-IDF v5.5.3
./tools/validate.sh --firmware # preferred: build and verify merged 0x0 image
idf.py set-target esp32c3     # fresh checkout or changed target
idf.py build                  # optional incremental application build
idf.py flash monitor          # optional incremental application flash
idf.py fullclean              # remove stale generated build state only
```

`idf.py fullclean` does not fully synchronize an existing `sdkconfig` with
changed defaults. Preserve intentional local settings, then run
`idf.py set-target esp32c3` when the target or tracked defaults must be
regenerated.

### Speeding up repeated builds

ccache can reuse previous compiler results when sources need to be compiled
again. ESP-IDF 5.5.3 disables it by default. After activating ESP-IDF, check that
ccache is available and enable it for an individual build (the same command
works in Linux/macOS shells and native Windows ESP-IDF terminals):

```text
ccache --version
idf.py --ccache build
```

Alternatively, enable it for the current Linux/macOS shell and its child
processes, including the validation script:

```bash
export IDF_CCACHE_ENABLE=1
idf.py build
```

This is an `idf.py` option, not a `sdkconfig` or `menuconfig` setting. For
repeatable project or CI builds, pass `--ccache` explicitly or set
`IDF_CCACHE_ENABLE=1` in that build environment; do not silently edit shell
startup files. See the [ESP-IDF 5.5.3 option definition](https://github.com/espressif/esp-idf/blob/v5.5.3/tools/idf_py_actions/core_ext.py).

Inspect the active cache configuration instead of assuming a fixed path:

```text
ccache --show-config
ccache --show-stats
```

The effective `cache_dir` depends on the ccache version, platform,
configuration, and `CCACHE_DIR`; it is not always `~/.ccache`. Keep it outside
`build/` and temporary validation directories so their removal preserves it.
Cache clearing is not a routine build step. Only when intentionally clearing
the active cache, use `ccache --clear`, which preserves the configuration file,
instead of deleting the directory. This also discards cached compiler results
for other projects sharing that cache. See the [ccache manual](https://ccache.dev/manual/latest.html).

On Windows, real-time antivirus or endpoint scanning can contribute to slow
builds, but diagnose the bottleneck first. For Microsoft Defender, use its
[performance analyzer](https://learn.microsoft.com/en-us/defender-endpoint/performance-analyzer-reference);
its results are not automatic exclusion recommendations. Exclusions reduce
protection and are optional: obtain user or administrator approval under the
applicable security policy, then limit any exception to the smallest confirmed
scope. Do not routinely exclude the entire ESP-IDF installation, tools tree,
or project, and do not disable real-time protection.

The tracked `dependencies.lock` pins Managed Component resolution. After changing an `idf_component.yml`, regenerate the lock with ESP-IDF 5.5.3, review version changes, and commit it with the manifest. An ordinary build must not leave an unexplained lock-file diff.

Firmware validation uses a fresh temporary build directory and an isolated `sdkconfig` generated from the tracked defaults. It does not consume or overwrite a developer's root `sdkconfig`, and it copies only the verified merged image to `build/FoloToy-AI-Passport-full.bin`. The gate also validates the [configured firmware layout](firmware-layout.md): image offsets from `flash_args`, partition-table MD5, bounds and non-overlap, and an application that starts in and fits its configured app partition. User-defined partition layouts are allowed.

The baseline also has a hardware-independent logic test:

```bash
cc -std=c11 -Wall -Wextra -Werror -Imain \
  tests/test_ui_pixel_math.c main/ui_pixel_math.c \
  -o /tmp/test_ui_pixel_math
/tmp/test_ui_pixel_math
```

Use the unified validation entry point:

```bash
./tools/validate.sh --static    # repository checks, workflows, links, secrets, host tests
./tools/validate.sh --firmware  # build, merge-bin, offsets, and configured layout
./tools/validate.sh             # complete gate; requires an activated ESP-IDF environment
```

CI calls the same script. Fix the shared script or environment if local and CI behavior differs; do not duplicate command sequences in workflows.

Hardware-affecting changes must also run the applicable on-device checklist in the hardware guide. Report compilation separately from physical-device validation.

Never upload the app-only `build/FoloToy-AI-Passport.bin` to the community. Only
the validated `build/FoloToy-AI-Passport-full.bin` contains the complete checked
firmware layout.
