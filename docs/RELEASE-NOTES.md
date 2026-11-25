<p align="right">
  <a href="RELEASE-NOTES.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# v1.0.0-nine-haruiro Release Notes

## What's new

The complete offline *9-nine- Haruiro* story is now available on AI Passport:
15 chapters, 13 choices, two ending milestones, 18 CG groups with 100 variants,
15 chapter previews, two music tracks, and an OP gallery.

- The OP gallery plays the complete opening animation and its opening track.
  Natural end, short OK skip, and long OK back all return to the same OP row
  without changing the story.
- The reading screen uses the full 320×180 artwork, a compact fitted speaker
  badge, and a lower body panel with three 16 px lines or two 20 px lines.
  Reading page numbers are no longer shown; real pagination and save-page
  positions still work.
- Fast forward advances one whole sentence per input tick and stops on release.
  A transient busy lock no longer drops the key action.
- Gallery browsing is silent by design; the opening and explicitly selected
  music continue normally. Returning to the title restores its music.

## How to use

Flash the merged full image at address `0x0`, then start the reader. The three
physical buttons provide short and long press actions:

- Short OK / Up / Down advances the reading; a short press during typing
  completes the page and advances in the same press.
- Long OK opens the in-game menu on reading and choice pages. The menu has
  return, save, load, history, settings and return-to-title.
- Long Down toggles automatic reading. Hold Up to fast forward; release stops
  without adding a short press.
- The title menu opens the gallery. Choose CG, chapter previews, music or OP.
  On secondary pages, Long OK goes back.
- Save and load use 30 manual slots plus automatic progress; a damaged slot is
  reported instead of replacing the current position.

## Verified

The tested device passed: the complete gallery OP picture and sound with return;
silent gallery browsing, switching and title-music restore; fast-forward release;
and save/load text and images.

## Not verified

This release does not claim every branch, long-run stability, power-loss during
writes, or every hardware peripheral. The source snapshot excludes the R18 scenes
and voice tracks; this port does not reconstruct them.

## Source and attribution

- AI Passport framework: [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport)
- This release source: [Shinku-Chen/ai-passport, release/nine-haruiro-20261125](https://github.com/Shinku-Chen/ai-passport/tree/release/nine-haruiro-20261125)
- Story/art source snapshot: [xuanyide01/9-nine-rw5](https://github.com/xuanyide01/9-nine-rw5) at `6a8907a5ae576f4377abe4b74f15bd9af13efe07`
- *9-nine-* and its characters belong to their original rights holders. This is
  an unofficial, non-commercial offline fan port; no voice or R18 content is
  included.
