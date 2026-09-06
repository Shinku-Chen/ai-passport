<p align="right">
  <a href="deep-sleep-peripheral-power-off.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Shutting Down On-Board Peripherals Before Deep-Sleep

Captured after the **"Sound Effects Keychain"** v1.4.0 firmware release on the
voice-keychain edition (commit `ce9b13d`). These are general, upstream-benefiting
learnings that apply to any AI Passport app, not fork-specific customization.

> **Verification status.** All of the following was verified on device with the
> sound-keychain firmware: the device sleeps after 5 min idle and wakes on any
> button, and codec/panel/gauge draw drops in deep sleep. Standby-current numbers
> were not measured with a meter, so treat the magnitude claims as relative.

## Why shut peripherals down at all

`esp_deep_sleep_start()` powers the MCU core down, but the peripherals that sit on
an always-on 3.3 V rail are **not** cut off — they keep drawing current even while
the chip sleeps. If the intent is minimum standby draw, each shuttable peripheral
must be told to enter its own low-power state before the chip sleeps. The MCU side
is handled for you; the peripheral side is not.

## The four shutdown calls, in order

Call these **after** arming the GPIO wake source and **before**
`esp_deep_sleep_start()`:

| Peripheral | Function | What it does |
|---|---|---|
| LCD panel | `bsp_display_sleep()` | `esp_lcd_panel_disp_on_off(panel, false)` (0x28 DISPOFF) then `esp_lcd_panel_disp_sleep(panel, true)` (0x10 panel sleep). A black screen is **not** powered down — the panel's controller still draws even with the backlight off and DISP off. |
| Backlight | `bsp_display_backlight(0)` | Set the LEDC duty to 0. Deep sleep also floats this un-held pin, so the LED is dark either way, but zeroing first is explicit. |
| Audio codec | `bsp_audio_sleep()` | Close the codec, which runs `es8311_suspend()` — writes 16 registers to stop the ADC/DAC, gate the clock, and disable the PA. |
| Fuel gauge | `bsp_battery_sleep()` | Put the CW2017 to sleep: write CONFIG 0x30 (restart) then 0xF0 (sleep). |

## Gotcha: `esp_codec_dev_close()` only suspends if the codec layer was opened

The trap that cost the most debugging. `esp_codec_dev_close()` calls
`es8311_enable(false)` — which is what actually runs `es8311_suspend()` and powers
the chip down — **only** when the codec-dev layer's `output_opened`/`input_opened`
is true. Otherwise it falls straight through to cleanup and does nothing.

Your own `s_opened` flag mirrors that: it is only true after a
`bsp_audio_set_format()` succeeds. So if the device **never plays any audio before
timing out into deep sleep**, `s_opened == false`, the close is skipped, and the
chip sits in the `es8311_open` configuration state — configured but **not**
powered down. The fix is to guarantee the suspend sequence runs on every path:

```c
esp_err_t bsp_audio_sleep(void) {
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    if (!s_opened) {
        bsp_audio_set_format(16000, 16, 1);   // silent open — no sound, just takes the codec-dev layer open
    }
    esp_codec_dev_close(s_dev);                 // now really runs es8311_suspend()
    s_opened = false;
    return ESP_OK;
}
```

The added `set_format` is silent (it never writes PCM), only completes the
codec-dev `open` path so the subsequent `close` triggers the suspend. A `s_dev`
that exists but was never opened is exactly the case that silently leaked.

## The MCU side is already handled — don't redo it

`esp_deep_sleep_start()` automatically calls `esp_sleep_isolate_digital_gpio()`
on this platform (ESP32-C3 does **not** define `SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP`,
so the `#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP` branch always runs). That
floats every un-held digital GPIO — including the I2S, I2C, and SPI pins — and
disables their internal pull-ups/pull-downs. So there is no MCU-side leakage path
to chase; the residual draw is purely the peripherals on the rail (and, outside
the chip, the external PA and the 3.3 V regulator, which are out of software reach).

## What software cannot fix

The board's power amplifier uses `BSP_I2S_PA_CTRL = -1`, meaning the PA enable pin
is **not** wired to the MCU — it is hardware always-on. Powering it off requires a
board change (wire the enable pin, or put the PA on a switchable rail). Similarly,
the 3.3 V rail's own regulator quiescent current and cell self-discharge are not
software-addressable. If standby is still high after all four calls, the remaining
draw is almost certainly these hardware constants.

## A real wake-from-sleep pitfall that overlaps this work

While here, note the related wakeup gotcha recorded separately in
[`display-refresh-and-deep-sleep.md`](display-refresh-and-deep-sleep.md): the
wake-source argument is a **pin bitmask**, not a pin number. It is easy to re-verify
both together — after these shutdown calls, wake on GPIO0 low and confirm the app
re-initializes the peripherals it just shut down.
