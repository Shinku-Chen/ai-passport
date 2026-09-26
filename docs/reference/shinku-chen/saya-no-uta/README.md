<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Saya no Uta (Community)

A landscape visual-novel reader for the AI Passport. It ports the Mi Band 10 fan
port of *Saya no Uta* to the device: 44 chapters, 3,828 dialogue lines and three
endings, fully offline, read with three keys.

## Publish information

- **Title**: Saya no Uta (Community)
- **Description**: submitted as:

  > Turn the AI Passport into a pocket copy of *Saya no Uta*: 44 chapters, 3,828 lines of dialogue and three endings, fully offline.
  >
  > Story: medical student Fuminori Sakisaka survives a car crash that kills his parents, but the brain surgery that saves him leaves the world twisted - he sees organs and rotting flesh everywhere, people as moving lumps of meat, speech as animal noise, and even ordinary food as revolting. In his despair the only normal thing left is a mysterious girl, Saya, who came to the hospital looking for her father. Fuminori falls for her, decides she is the reason he can bear living in this diseased world, and invites her to live with him. That is only the door to the madness still to come.
  >
  > Controls - three keys read the whole story:
  > - OK: open the menu (save / load / skip chapter / back to title / close)
  > - UP: tap for the next segment (one press finishes the typing first), hold for one second to fast-forward and release to stop
  > - DOWN: step back through the pages of a long segment; UP and DOWN also move the cursor in menus and choices
  > - Choices stop and wait for you, and the menu's skip-chapter also stops at a choice the chapter has not reached yet
  >
  > Also:
  > - Saves: five manual slots plus one automatic slot written on every scene change; "Continue" on the title screen resumes from it.
  > - Settings: text speed (slow / medium / fast / instant) and font size (16 px or 20 px); the about page scrolls.
  > - Reading screen: art area with background and sprite on top, the speaker name in its lower-left corner, and the dialogue box below.
  > - Fully offline: the script, 193 backgrounds and 75 sprites are packed into the firmware - no network and no decompression at runtime; the Chinese font subset is built in.
  > - The first screen is a content warning: scroll to the end before OK continues.
  > - Idle 60 s dims the backlight, 180 s turns it off and 420 s enters deep sleep; any key wakes it and resumes from the automatic save.
  >
  > This firmware is a personal study port built from a public Mi Band port and is meant for personal devices only. The story contains heavy gore; please read with care and support the official release if you can.

- **Category**: games
- **Cover**: `saya-no-uta-cover.png` (PNG, 1152 × 1536, 3:4) — publish metadata only; the
  cover image is not committed here.
- **Source**: <https://github.com/Shinku-Chen/ai-passport>

## Cover

`saya-no-uta-cover.png` (PNG, 1152 × 1536) is the portrait 3:4 cover used for the
community listing: the original key visual with Saya and the title lettering. The
archive is text-only, so only the file name and format are recorded.

## Features

- **Three keys, 44 chapters**: OK opens the menu, UP advances (hold to fast-forward,
  release to stop) and DOWN steps back a page; list cursors stop at the ends instead
  of wrapping around.
- **Paginated body text**: 19 full-width characters per line and four lines per
  screen at 16 px (15 characters and three lines at 20 px), with a typewriter effect
  that can also print a whole page instantly.
- **Choices and branches**: one choice in chapter 10 and one in chapter 20, three
  endings in total (End / BadEnd / MadEnd); a choice always stops and waits.
- **Skip chapter**: the menu entry skips the current chapter, but stops at a choice
  the chapter has not reached yet instead of deciding for the player.
- **Saves**: five manual slots plus one automatic slot written on every scene change;
  "Continue" restores the automatic one. Holding OK on a slot in save mode deletes it.
- **Settings**: text speed, font size, and a scrollable about page.
- **Content warning screen**: the first screen after boot; it must be scrolled to the
  end before OK continues.
- **Offline resource pack**: script, backgrounds and sprites are read straight out of
  Flash with no runtime decompression; decoding happens only when the background or
  sprite actually changes, and sprites are composited through a 1bpp mask.
- **Chinese font subset**: a Noto Sans SC subset (4 bpp, uncompressed) built into the
  firmware, covering both the UI strings and every character in the script.
- **Battery and power**: battery percentage in the top-right corner; idle 60 s dims,
  180 s turns the screen off and 420 s enters deep sleep, with any key waking it.
