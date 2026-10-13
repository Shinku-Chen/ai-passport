<p align="right">
  <a href="galgame-content-authoring.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# GalGame Content Authoring (Brief)

For title authors and AI agents: **edit one file** to fill in your title and the
framework gives you 19 logical screens, three-key controls, story locks, saving and
progress out of the box.

> How to use: hand this brief, the framework branch (`feature/galgame-framework`, content
> layer at `main/gal/gal_content.c`) and your raw materials to an AI agent, and it can
> generate the complete content layer and firmware. Framework internals are documented in
> the full guide [galgame-framework.md](galgame-framework.md).

## 1. The one file you edit

```
main/gal/gal_content.c     ← fill in content
main/gal/gal_content.h     ← title macros; add story flags if needed
```

Everything else (UI, state machine, saves, story locks, audio) stays untouched.

The skeleton draws its background art in code and synthesises square-wave music, so a
**text-only title can go on the device as is**. For real art and audio, see section 6.

## 2. The tables

| Array / macro | Purpose | Example |
| --- | --- | --- |
| `GAL_CONTENT_TITLE` and 3 siblings | Title, subtitle, about-page logo subtitle, version | `"Sample Title"` |
| `gal_content_lines[]` | Dialogue: speaker (empty = narration), text, required flag | `{ "Hero", "…mm.", GAL_UNLOCK_NONE }` |
| `gal_content_events[]` | **Event stream** played in order: kind, index, flag written on arrival | `{ GAL_EV_LINE, 0, GAL_UNLOCK_NONE }` |
| `gal_content_choices[]` | Choice groups: prompt line plus option array | see the example below |
| `gal_content_transitions[]` | Chapter transitions: chapter, eyebrow, title, subtitle | `{ "Chapter 2", "CHAPTER 02", "One More Train", "Chapter 2 · The Platform" }` |
| `gal_content_endings[]` | Endings: eyebrow, title, body | `{ "NORMAL END", "One More Train", "…" }` |
| `gal_content_cgs[]` | CGs: label, caption, unlock event | `{ "CG 01", "Empty Platform", 1, false }` |
| `gal_content_chapters[]` | Chapters: name, summary, progress text, unlock event, required flag | see the example |
| `gal_content_recalls[]` | CG recall: scene title, chapter, required CG, replay start event | see the example |
| `gal_content_tracks[]` | Music: title, duration text, required CG, note sequence | see the example |
| `gal_content_*_menu[]` | Title-page menu (6 rows) and in-game menu (6 rows) — text is yours, keep the counts | `"Start new game"…` |
| `gal_content_about_rows[]`, `GAL_CONTENT_ABOUT` | About page rows and body | `{ "Version", "v0.1.0" }` |

Every array needs its `const int gal_content_xxx_count = (int)(sizeof(...) / sizeof(...[0]));`
neighbour — the example below already has them, copy as is.

## 3. Minimal working example (replace the whole file)

```c
// main/gal/gal_content.c —— sample title content layer
#include "gal_content.h"

#define GAL_CONTENT_TITLE    "Sample Title"
#define GAL_CONTENT_SUBTITLE "A short sample journey"
#define GAL_CONTENT_LOGO_SUB "SAMPLE TITLE"
#define GAL_CONTENT_VERSION  "v0.1.0"

// Story flags: add or remove as needed
enum {
    GAL_FLAG_KIND = 0,   // chose "Be gentle"
    GAL_FLAG_CLEARED,    // reached an ending once
};

// ---------------------------------------------------------------- dialogue
const gal_line_t gal_content_lines[] = {
    { "", "After the rain, only the two of us were left on the platform.", GAL_UNLOCK_NONE },
    { "Hero", "Are you really leaving?", GAL_UNLOCK_NONE },
    { "Friend", "…Mm.", GAL_UNLOCK_NONE },
    { "Friend", "But today, we can take it slow.", GAL_FLAG_KIND },   // only after "Be gentle"
    { "Hero", "Then let's wait for one more train.", GAL_UNLOCK_NONE },
};
const int gal_content_line_count =
    (int)(sizeof(gal_content_lines) / sizeof(gal_content_lines[0]));

// ---------------------------------------------------------------- choices
static const gal_option_t choice0_options[] = {
    { "Be gentle", GAL_FLAG_KIND, GAL_UNLOCK_NONE },
    { "Say nothing", GAL_UNLOCK_NONE, GAL_UNLOCK_NONE },
};
const gal_choice_t gal_content_choices[] = {
    { "A train rumbles somewhere far away.", 2, choice0_options },
};
const int gal_content_choice_count =
    (int)(sizeof(gal_content_choices) / sizeof(gal_content_choices[0]));

// ---------------------------------------------------------------- event stream
const gal_event_t gal_content_events[] = {
    { GAL_EV_LINE, 0, GAL_UNLOCK_NONE },
    { GAL_EV_LINE, 1, GAL_UNLOCK_NONE },
    { GAL_EV_LINE, 2, GAL_UNLOCK_NONE },
    { GAL_EV_CHOICE, 0, GAL_UNLOCK_NONE },
    { GAL_EV_LINE, 3, GAL_UNLOCK_NONE },
    { GAL_EV_LINE, 4, GAL_UNLOCK_NONE },
    { GAL_EV_TRANSITION, 0, GAL_UNLOCK_NONE },
    { GAL_EV_ENDING, 0, GAL_FLAG_CLEARED },
};
const int gal_content_event_count =
    (int)(sizeof(gal_content_events) / sizeof(gal_content_events[0]));

// ---------------------------------------------------------------- transitions, endings
const gal_transition_t gal_content_transitions[] = {
    { "Chapter 2", "CHAPTER 02", "One More Train", "Chapter 2 · The Platform" },
};
const int gal_content_transition_count =
    (int)(sizeof(gal_content_transitions) / sizeof(gal_content_transitions[0]));

const gal_ending_t gal_content_endings[] = {
    { "NORMAL END", "One More Train", "The train came.\nNeither of them boarded." },
};
const int gal_content_ending_count =
    (int)(sizeof(gal_content_endings) / sizeof(gal_content_endings[0]));

// ---------------------------------------------------------------- CGs
const gal_cg_t gal_content_cgs[] = {
    { "CG 01", "Empty platform", 1, false },
    { "CG 02", "One more train", 6, false },
};
const int gal_content_cg_count = (int)(sizeof(gal_content_cgs) / sizeof(gal_content_cgs[0]));

// ---------------------------------------------------------------- chapters / routes
const gal_chapter_t gal_content_chapters[] = {
    { "Prologue · Empty Platform", "The platform after the rain.", "Progress  1 / 2",
      GAL_UNLOCK_NONE, GAL_UNLOCK_NONE, false },
    { "Hidden · Night Train", "Unlocks after reaching an ending.", "Progress  0 / 2",
      GAL_UNLOCK_NONE, GAL_FLAG_CLEARED, false },
};
const int gal_content_chapter_count =
    (int)(sizeof(gal_content_chapters) / sizeof(gal_content_chapters[0]));

// ---------------------------------------------------------------- CG recall
const gal_recall_t gal_content_recalls[] = {
    { "Empty platform", "Prologue", 0, 0 },   // needs CG 01, replays from event 0
};
const int gal_content_recall_count =
    (int)(sizeof(gal_content_recalls) / sizeof(gal_content_recalls[0]));

// ---------------------------------------------------------------- music
// notes index the tone table (C-major scale starting at C4, see gal_music.c); 0xFF = rest.
static const uint8_t track0_notes[] = { 0, 4, 7, 4, GAL_NOTE_REST };
const gal_track_t gal_content_tracks[] = {
    { "Empty platform", "01:30", 0, track0_notes, (uint8_t)sizeof(track0_notes) },
};
const int gal_content_track_count =
    (int)(sizeof(gal_content_tracks) / sizeof(gal_content_tracks[0]));

// ---------------------------------------------------------------- about
const gal_about_row_t gal_content_about_rows[] = {
    { "Version", GAL_CONTENT_VERSION },
    { "Author", "Your name" },
    { "Art / Music", "Credits" },
    { "License", "Per title" },
};
const int gal_content_about_row_count =
    (int)(sizeof(gal_content_about_rows) / sizeof(gal_content_about_rows[0]));

// ---------------------------------------------------------------- menus
const char *const gal_content_title_menu[] = {
    "Start new game", "Continue", "Load", "Settings", "Gallery", "About",
};
const int gal_content_title_menu_count =
    (int)(sizeof(gal_content_title_menu) / sizeof(gal_content_title_menu[0]));

const char *const gal_content_quick_menu[] = {
    "Back to story", "Save", "Load", "History", "Settings", "Back to title",
};
const int gal_content_quick_menu_count =
    (int)(sizeof(gal_content_quick_menu) / sizeof(gal_content_quick_menu[0]));

// ---------------------------------------------------------------- gallery categories
const char *const gal_content_gallery_menu[] = {
    "CG browser", "CG recall", "Story", "Music",
};
const int gal_content_gallery_menu_count =
    (int)(sizeof(gal_content_gallery_menu) / sizeof(gal_content_gallery_menu[0]));
```

Also replace `GAL_CONTENT_ABOUT` in `gal_content.h` with your synopsis, and add or remove
`GAL_FLAG_*` entries as needed.

## 4. Story locks (optional)

Three steps:

1. **Declare a flag** in `GAL_FLAG_*` inside `gal_content.h`.
2. **Write the flag**: an option's `flag_set`, or an event's `flag_set` (transitions and
   endings are the usual places).
3. **Require the flag**: a line's `require_flag` (unmet means the line is **skipped**, which
   is how branches are built), an option's `require_flag` (greys the row out), or a
   chapter's `require_flag` (shows as locked).

CGs and chapters can also use `unlock_event`: reaching that event index unlocks them
(for example `6` = the seventh event in the stream). Unlock state and flags persist across
saves, and both "Continue" and the gallery read from them.

## 5. After filling in

```bash
# 1) Content changed: regenerate the CJK font subsets (chars come from main/ literals)
python3 tools/gal_font.py --font <source>.otf --out-dir assets/fonts

# 2) Gate: repository checks + host tests + font coverage (missing glyphs fail)
CC=<cc wrapper> ./tools/validate.sh --static      # macOS needs a CC wrapper; see the full guide
./tools/validate.sh --firmware                    # requires ESP-IDF 5.5.3 active

# 3) Flash
idf.py -p <port> flash monitor
```

## 6. Real assets (advanced)

| Goal | Where to change |
| --- | --- |
| Real backgrounds / CG images | `gal_ui.c`: replace the gradients in `bg`, the gallery tiles and the CG viewer with `lv_image`; put assets under `assets/` (LVGL images or raw RGB565) |
| Real BGM / voice | `gal_music.c`: replace the square-wave loop with decoded PCM, still written through `bsp_audio_write()` with `bsp_audio_set_format()` |
| Bulk-import a PC script | Write a converter that turns the script into the three content tables (lines / events / choices); map flags and chapters as in section 4 |
| Landscape art | Use the BSP landscape API and ship 320×240 crops (layout table in section 5 of the full guide) |

## 7. Checklist before delivery

- [ ] Four core tables present: lines, events, choices, endings — each with its `count`
- [ ] Every event index exists (`GAL_EV_LINE 3` requires `gal_content_lines[3]`)
- [ ] At least one chapter transition (otherwise the story/chapter screens have nothing)
- [ ] CG / chapter / recall / track unlock events point at real event indices
- [ ] About page matches the title; menu labels localised
- [ ] `./tools/validate.sh --static` passes after regenerating fonts (0 missing glyphs)
- [ ] Walk it on the device: start → dialogue → choice → transition → ending → title → continue
