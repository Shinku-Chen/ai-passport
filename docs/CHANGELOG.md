<p align="right">
  <a href="CHANGELOG.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Changelog

## Unreleased

- Added a new demo application "What to Eat Today" (`main/demo_eat_what.c`): a button-driven roulette that cycles two GIF-source food animations while a key is held. Backed by LVGL I8 indexed binary assets (`main/eat_what_g1.bin` / `eat_what_g2.bin`) embedded via `EMBED_FILES`, with a pure state-machine module `main/ui_eatwhat_math.c` and an asset generator `tools/generate_eat_what_assets.py`. Hold UP to loop the lunch-prompt animation (10 fps), hold DOWN to loop the food-picker animation (20 fps); release to stop on the current frame.
- Added a global auto-poweroff: after 2 minutes with no button activity on any page, the device turns off the backlight and enters deep sleep, woken by a GPIO0 low-level edge (the board's external 10 kOhm pull-up keeps it high at idle; pressing any key pulls it low). Implemented in `main/autopower.c` with a pure idle-expiry check in `main/ui_autopower_math.c`. Uses GPIO wakeup because ESP32-C3 has no EXT0/EXT1; RTC GPIO0 plus the external pull-up make low-level wakeup reliable.
- Reduced the firmware to a single application: the boot sequence now enters "What to Eat Today" directly with no main menu. Removed the other demo pages (Display/Button/Audio/Battery/Radio/Wi-Fi/BLE/Low Power) from `main/CMakeLists.txt` along with their build dependencies (`bt`, `esp_netif`, `esp_wifi`), and slimmed `main/main.c`/`main/demo.h` to the one app. Wi-Fi/Bluetooth stacks and audio/battery peripherals are no longer initialized on boot.
- Added a candidate fast-refresh renderer (`main/ui_eatwhat_render.c`) to compare against LVGL's partial redraw: it drives `esp_lcd_panel_draw_bitmap` directly to refresh only the image rectangle, with an optional interlaced two-pass mode (even rows then odd rows, half the pixels per pass). Toggle it in-app with a single OK press. Intended for hardware A/B testing of refresh speed vs. tearing; no firmware change is final until measured on-device.
- Added a post-release follow-up workflow: an `issue-suggestions` skill for filing user feedback as issues against the upstream project, an `experience-pr` skill for submitting reusable development experience as a documentation PR, a `docs/experiences/` directory for per-entry experience files, and supporting `after-release`, `file-issues`, and experience-index documents.
- Simplified the tracked repository root: moved GitHub-recognized community documents into `.github/`, moved the changelog into `docs/`, updated every reference, and added a root-document allowlist to repository checks.
- Repository-wide language policy: every maintained Markdown default `.md` file is English, Simplified Chinese uses a paired `.zh_CN.md`, and both provide language switches. Static checks reject missing peers, missing switches, and Chinese prose in English defaults.
- Phase one of the AI development workflow: streamlined task-based context routing, unified local/CI validation, added PR checks and a template, and committed the dependency lock for reproducible builds.
- PR review fixes: pinned GitHub Actions to full commit SHAs, split build/release jobs by least privilege, disabled persisted sync checkout credentials, added Feature Request and Usage Question forms, clarified private security-report fallback, and corrected stale README, CI-trigger, and branch descriptions.
- Changed commit titles, PR titles, and PR bodies from Chinese-default to English; updated the Chinese punctuation rule so it no longer applies to PR descriptions.
- Reworked `build-firmware.yml` to pass `SDKCONFIG_DEFAULTS=sdkconfig.defaults`, enable `partitions.csv`, preserve the 8 MB image header, merge a flashable `FoloToy-AI-Passport-full.bin`, publish only that artifact, and use Actions cache v5.
- Integrated upstream PR #6 to resolve PR #4 conflicts: Wi-Fi, Bluetooth LE, radio lifecycle, and low-power demos; a 3 MB factory partition; build/menu/configuration updates; hardware-guide coverage; and bilingual capability tables.
- Defined English imperative Conventional Commit formatting for both commits and PR titles.
- Removed stale sync-workflow template comments and generalized an irrelevant Redis TTL rule to cache components.
- Added Chinese punctuation, credential safety, and recoverable file-deletion conventions.
- Expanded source-comment requirements for functions, state, ownership, concurrency, timing, registers, and magic values.
- Removed AI execution instructions from product READMEs so they remain human-facing product and repository overviews.
- Added `docs/development/agent-guide.md` as the focused AI workflow guide.
- Updated `AGENTS.md`, `docs/INDEX.md`, and the development index for the agent guide.
- Documented why the root README path is reserved for fork owners and how GitHub README precedence supports it.
- Created `main-update` from the upstream-aligned baseline and combined the repository-structure, firmware-CI, and upstream-sync work.
- Corrected the merged documentation index, workflow path, project tree, and CI references.
- Moved CI documentation from software design to `docs/development/`.
- Moved fork-only documentation assets from `assets/docs/` to `docs/assets/`.
- Moved the upstream English/Chinese project READMEs under `docs/` and renamed the documentation catalog to `docs/INDEX.md`.
- Initialized `AGENTS.md`, `CLAUDE.md`, and `CHANGELOG.md`.
- Standardized the initial project README language filenames.
- Added the `docs/`, `assets/`, and `skills/` directory structure.
- Moved the upstream hardware guide into `docs/hardware-design/`.
- Standardized subdirectory README capitalization and introduced fork conventions.
- Allowed fork-owned root README and supplemental documentation content on fork `main`.
- Added and documented the fork-only supplemental-document directory.
- Moved the build CI document to its dedicated CI branch before consolidation.
- Documented clean-`main` reasons, the direct-development exception, and Actions enablement for forks.
- Split the original agent rules into contribution, development, and fork documents with a compact root index.
- Updated software-design and project README references for the new documentation structure.
- Added the documentation catalog and task-triggered routing based on the earlier repository model.
- Added bilingual contribution, code-of-conduct, security, and support documents tailored to this ESP-IDF and fork workflow.
