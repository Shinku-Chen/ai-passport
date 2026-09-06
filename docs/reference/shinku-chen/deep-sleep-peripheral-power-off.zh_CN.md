<p align="right">
  <strong>简体中文</strong> · <a href="deep-sleep-peripheral-power-off.md">English</a>
</p>

# 深睡前关闭板载外设

在「音效钥匙扣」v1.4.0 固件、voice-keychain 版本（提交 `ce9b13d`）发布后沉淀。
这些是通用、上游受益的经验，适用于任何 AI Passport 应用，而非 fork 专属定制。

> **验证状态。** 以下均已在本项目「音效钥匙扣」固件上真机验证：空闲 5 分钟入睡、
> 任意按键可唤醒、codec/面板/电量计在深睡时电流下降。待机电流数值未用万用表实测，
> 故“量级”描述请视为相对大小，不是确切读数。

## 为什么要关外设

`esp_deep_sleep_start()` 只把 MCU 核心断电，但挂在常通 3.3 V 轨上的**外设并不会**
被切断——它们仍在耗电。若目标是极低待机，每个可关的外设都必须在芯片入睡前被
点名进入自己的低功耗态。MCU 侧有框架代劳，外设侧没有。

## 四条关机调用，按顺序

在**配好 GPIO 唤醒源之后**、`esp_deep_sleep_start()` 之前调用这些：

| 外设 | 函数 | 做了什么 |
|---|---|---|
| LCD 面板 | `bsp_display_sleep()` | `esp_lcd_panel_disp_on_off(panel, false)`（0x28 DISPOFF）再 `esp_lcd_panel_disp_sleep(panel, true)`（0x10 面板睡眠）。**黑屏并不等于下电**——就算背光灭、DISP 关，面板控制器仍在耗电。 |
| 背光 | `bsp_display_backlight(0)` | 把 LEDC 占空比归零。深睡本就会把该未被 hold 的引脚浮空，LED 反正灭，但先归零更明确。 |
| 音频 codec | `bsp_audio_sleep()` | 关闭 codec，内部走 `es8311_suspend()`——写 16 个寄存器停掉 ADC/DAC、门控时钟、禁用 PA。 |
| 电量计 | `bsp_battery_sleep()` | 让 CW2017 进睡眠：写 CONFIG 0x30（重启）再 0xF0（睡眠）。 |

## 坑：`esp_codec_dev_close()` 只在 codec 层「被打开过」时才真正 suspend

最花调试时间的一个坑。真正执行 `es8311_suspend()`、把芯片下电的是
`es8311_enable(false)`，而 `esp_codec_dev_close()` **只在** codec-dev 层的
`output_opened`/`input_opened` 为 true 时才走到它；否则直接进 cleanup，什么都不做。

你自己维护的 `s_opened` 正好镜像这一点：它只在 `bsp_audio_set_format()` 成功后
才为 true。所以如果设备**开机后从未播放任何音频就超时深睡**，`s_opened == false`，
close 被跳过，芯片停在 `es8311_open` 的配置态——配好寄存器了，但**没有下电**。
修复是保证任何时序都走完 suspend 序列：

```c
esp_err_t bsp_audio_sleep(void) {
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    if (!s_opened) {
        bsp_audio_set_format(16000, 16, 1);   // 无声打开——不发声，只是把 codec-dev 层走成 open
    }
    esp_codec_dev_close(s_dev);                 // 这次才真的跑 es8311_suspend()
    s_opened = false;
    return ESP_OK;
}
```

多出来的 `set_format` 是无声的（从不写 PCM），只是补完 codec-dev 的 `open` 路径，
让随后的 `close` 真正触发 suspend。「`s_dev` 存在但从未 open 过」正是那个静默泄漏的场景。

## MCU 侧已经代劳，无需再处理

在此平台上 `esp_deep_sleep_start()` 会自动调用 `esp_sleep_isolate_digital_gpio()`
（ESP32-C3 **未定义** `SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP`，
所以 `#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP` 分支一定会走）。它把每个
未被 hold 的数字 GPIO——包括 I2S、I2C、SPI 引脚——全部浮空，并禁用其内部上/下拉。
所以没有任何 MCU 侧漏电路径需要追；残余耗电纯粹来自轨上的外设（以及芯片之外、
代码够不着的功放和 3.3 V 稳压器）。

## 软件修不了的部分

本板功放用 `BSP_I2S_PA_CTRL = -1`，即 PA 使能脚**没有接到 MCU**——硬件常通。
要断它只能改板（接使能脚，或把功放放到可切换的电源轨上）。同理，3.3 V 轨稳压器
自身的静态电流和电池自放电也不是软件能插手的。四条调用全上待机仍偏高时，剩下的
大头几乎可以肯定是这些硬件常量。

## 与本工作重叠的一个真实唤醒坑

顺带一提，相关的唤醒坑已单独记录在
[`display-refresh-and-deep-sleep.zh_CN.md`](display-refresh-and-deep-sleep.zh_CN.md)：
唤醒源参数是**引脚位掩码**，不是引脚号。两者可以一起复核——跑完这些关机调用后，
用 GPIO0 低电平唤醒，确认应用会重新初始化它刚关掉的外设。
