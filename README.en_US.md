<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

# Shengzi Cards

A **landscape** Chinese character flashcard app for the AI Passport: browse, self-test and spell modes, with known characters persisted in NVS.

## Controls

**Long press UP / DOWN** cycles `BROWSE` → `TEST` → `SPELL`. BROWSE shows the character with its pinyin, **UP** goes back one card and **DOWN** jumps to a random one; TEST hides the pinyin first, **OK** reveals it, then **UP** marks the card known and **DOWN** unknown (both advance randomly); SPELL reveals with **OK** and advances on the next **OK**.

## Features

- **Three modes** — browse, self-test and spell, with the footer showing mode, card index and known count.
- **Known marks persist** — known characters are stored in NVS and survive a reboot.
- **Random advance** — prevents reciting in a fixed order.

## Dependencies

**Standalone and offline**: the character list and pinyin ship with the firmware; no phone, network or account.

## Build and flash

```bash
idf.py set-target esp32c3 && idf.py build
idf.py merge-bin
esptool.py --chip esp32c3 -p <PORT> -b 460800 write_flash 0x0 build/merged-binary.bin
```

## Release

Tag `v1.0.0`.

## Known limitations

- The character list is fixed in the firmware and cannot be extended.
- Only a known/unknown flag is stored; there is no spaced repetition or mistake log.

## License

MIT, see [`LICENSE`](LICENSE).
