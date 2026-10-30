# smartring_plus

**SmartRing-Plus** 板级支持包（BSP）—— VIEWE 基于 **ESP32-S3-N16R8** 设计的SmartRing-Plus开发板。

| | |
|---|---|
| **作者** | Ayang |
| **公司** | SHENZHEN VIEWE TECHNOLOGY CO.,LTD（深圳优奕视界有限公司） |
| **目标芯片** | `esp32s3`（推荐 16MB Flash + 8MB Octal PSRAM） |
| **ESP-IDF** | ≥ 5.5 |
| **许可证** | Apache-2.0 |

本组件将板上已验证的外设封装为稳定的 `bsp_*` API，应用层无需硬编码引脚、初始化时序或总线接线。

```c
#include "bsp/smartring_plus.h"
```

---

## 功能一览

| 子系统 | 硬件 | 主要 API |
|--------|------|----------|
| 显示 | ST77916 QSPI，360×360 圆屏 | `bsp_display_init()`、背光 / 字节序辅助 |
| 触摸 | CST816（I2C） | 注册为 LVGL 指针设备；`bsp_touch_get_handle()` |
| 共享总线 | I2C SDA=8 / SCL=9 / 400 kHz | `bsp_i2c_get_bus()` |
| 音频 | ES8311 + I2S + PA | `bsp_audio_init()` / `read` / `write` / `set_mute` |
| 存储 | SDMMC 4-bit FAT | `bsp_sd_init()`，挂载点 `/sdcard` |
| 电池 / PMIC | **V1** ADC GPIO1；**V2** AXP2101（运行时探测） | `bsp_battery_*` / `bsp_battery_get_pmic_type()` |
| 电源 | GPIO47 软关机（V1/V2 相同） | `bsp_power_shutdown()` |
| IMU | QMI8658A | `bsp_imu_init()` / `get_accel` / `get_tilt` / 校准辅助 |
| UI 栈 | `esp_lvgl_adapter` + LVGL 9 | `bsp_display_init()` 成功后即可建 UI |

引脚、I2C 地址、挂载点等定义集中在：  
[`include/bsp/board_config.h`](include/bsp/board_config.h)。

---

## 作为组件依赖

在工程的 `idf_component.yml`（或 `main/idf_component.yml`）中添加：

```yaml
dependencies:
  viewesmart/smartring_plus: "^1.1.2"
```

拉取依赖：

```bash
idf.py set-target esp32s3
idf.py reconfigure
```

也可将本目录直接放到工程的 `components/smartring_plus` 本地引用。

---

## 推荐初始化顺序

多个模块复用 `bsp_display_init()` 内创建的 I2C 总线，**请先初始化显示**，再按需初始化其它外设：

```c
#include "bsp/smartring_plus.h"
#include "esp_lv_adapter.h"
#include "esp_log.h"

void app_main(void)
{
    ESP_ERROR_CHECK(bsp_display_init());   /* I2C + 触摸 + LCD + LVGL 适配器 + 背光 */
    ESP_ERROR_CHECK(bsp_battery_init());
    ESP_ERROR_CHECK(bsp_power_init());

    /* 可选：无卡 / IMU 失败时仅告警，勿阻塞整机 */
    if (bsp_sd_init() != ESP_OK) {
        ESP_LOGW("app", "SD 未挂载");
    }
    if (bsp_imu_init() != ESP_OK) {
        ESP_LOGW("app", "IMU 初始化失败");
    }
    ESP_ERROR_CHECK(bsp_audio_init());

    ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));
    /* 在此创建 LVGL 界面 */
    esp_lv_adapter_unlock();
}
```

---

## API 说明

公开接口均声明于 [`include/bsp/smartring_plus.h`](include/bsp/smartring_plus.h)。

### 显示与触摸

| 函数 | 说明 |
|------|------|
| `esp_err_t bsp_display_init(void)` | 初始化共享 I2C、CST816、ST77916 QSPI、`esp_lvgl_adapter`，背光默认 100%。成功后 LVGL 任务已运行。 |
| `void bsp_display_backlight_set(uint8_t percent)` | 背光 PWM 0–100（GPIO46，高电平点亮）。 |
| `void bsp_display_set_swap_bytes(bool enable)` | 运行时切换 RGB565 字节序（调试/换屏）；会触发全屏重绘。 |
| `bool bsp_display_get_swap_bytes(void)` | 当前字节序状态。 |
| `lv_display_t *bsp_display_get_handle(void)` | LVGL 显示句柄。 |
| `esp_lcd_touch_handle_t bsp_touch_get_handle(void)` | 触摸句柄（直读坐标）；未就绪返回 `NULL`。 |
| `i2c_master_bus_handle_t bsp_i2c_get_bus(void)` | 共享 I2C（codec / IMU 复用）；显示未初始化时为 `NULL`。 |

**LVGL 注意：** 创建或更新 UI 前必须 `esp_lv_adapter_lock(-1)`，结束后 `esp_lv_adapter_unlock()`。

```c
ESP_ERROR_CHECK(bsp_display_init());
bsp_display_backlight_set(80);

ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));
lv_obj_t *scr = lv_obj_create(NULL);
lv_screen_load(scr);
esp_lv_adapter_unlock();
```

---

### 电池 / PMIC（V1 ADC 与 V2 AXP2101）

**电芯（两版相同）**：单节锂电 **3.7V / 600mAh**，满充 4.2V。规格宏见 `board_config.h`（`BOARD_BAT_*`）。

`bsp_battery_init()` **运行时探测**共享 I2C 上的 AXP2101（读 `0x03`，期望 CHIP ID `0x4A`）：

| 路径 | 条件 | 电量 / 充电 | 其它 |
|------|------|-------------|------|
| **V2** | 探测成功 | REG `0xA4` 百分比；`0x01[6:5]` 充放电方向；VBAT ADC `0x34/0x35`（1 mV/LSB） | ALDO3=3.3V；恒流 300mA（0.5C）；CV 4.2V；预充 75mA；终止 25mA |
| **V1** | 无 AXP / ID 不匹配 | GPIO1 ADC + 同规格 OCV 查表 | 充电由电压滞回推断；插电时扣轨压抬升再查表 |

| 函数 | 说明 |
|------|------|
| `esp_err_t bsp_battery_init(void)` | 探测并启动 1 Hz 采样（须先 `bsp_display_init`）。 |
| `bsp_pmic_type_t bsp_battery_get_pmic_type(void)` | `BSP_PMIC_V1_ADC` / `BSP_PMIC_V2_AXP2101`。 |
| `void bsp_battery_get_data(bsp_battery_data_t *out)` | 线程安全快照：`voltage_v`、`percent`、`charging`。 |
| `float bsp_battery_get_voltage(void)` | 最新电压（V）。 |
| `int bsp_battery_get_percent(void)` | 电量 0–100%。 |
| `bool bsp_battery_is_charging(void)` | 是否充电中。 |

```c
ESP_ERROR_CHECK(bsp_battery_init());
ESP_LOGI("bat", "PMIC=%d", (int)bsp_battery_get_pmic_type());

bsp_battery_data_t bat;
bsp_battery_get_data(&bat);
ESP_LOGI("bat", "%.2f V  %d%%  %s",
         bat.voltage_v, bat.percent,
         bat.charging ? "充电中" : "放电");
```

---

### 电源

| 函数 | 说明 |
|------|------|
| `esp_err_t bsp_power_init(void)` | 配置软关机 GPIO（默认不触发）。 |
| `void bsp_power_shutdown(void)` | 软关机：GPIO47 保持高电平 `BOARD_PW_OFF_HOLD_MS`（3.5 s）。正常情况不返回。调用前请先做 UI 提示。 |

**V1/V2 均使用 GPIO47**；AXP2101 不参与关机。

```c
ESP_ERROR_CHECK(bsp_power_init());
/* … 用户确认后 … */
bsp_power_shutdown();
```

---

### 音频（ES8311 + I2S）

固定数据格式：**48 kHz / 16bit / 立体声交错**（约 192 KB/s）。  
`bsp_audio_init()` 只做硬件初始化，**不设置增益/音量**——请通过 codec 句柄自行配置。

| 函数 | 说明 |
|------|------|
| `esp_err_t bsp_audio_init(void)` | I2S + ES8311；须先 `bsp_display_init()`（I2C）。重复调用会跳过。PA 由 codec 的 `pa_pin` 自动控制。 |
| `esp_codec_dev_handle_t bsp_audio_get_codec(void)` | codec 句柄（增益/音量/静音/流式播放）。 |
| `i2s_chan_handle_t bsp_audio_get_rx(void)` | I2S RX 通道（原始 PCM）。 |
| `esp_err_t bsp_audio_read(void *buf, size_t len, uint32_t timeout_ms)` | 阻塞录音，写满 `len` 字节才返回。 |
| `esp_err_t bsp_audio_write(const void *buf, size_t len)` | 阻塞回放 `len` 字节。 |
| `esp_err_t bsp_audio_set_mute(bool mute)` | 输出静音开关。 |

**回放时序（重要）：** 取消静音 → `write`（可分块）→ 再静音。结尾漏掉静音容易爆音。

**本板实测推荐值（应用参考，非硬性）：**

- 录音增益约 **36 dB**：`esp_codec_dev_set_in_gain()`（驱动按 6 dB 整档）
- 回放音量约 **75**：`esp_codec_dev_set_out_vol()`（0–100）

```c
ESP_ERROR_CHECK(bsp_audio_init());

esp_codec_dev_handle_t codec = bsp_audio_get_codec();
esp_codec_dev_set_out_vol(codec, 75);
esp_codec_dev_set_in_gain(codec, 36.0f);

/* 回放 */
ESP_ERROR_CHECK(bsp_audio_set_mute(false));
ESP_ERROR_CHECK(bsp_audio_write(pcm, pcm_bytes));
ESP_ERROR_CHECK(bsp_audio_set_mute(true));

/* 录音 */
uint8_t buf[48 * 2 * 2 * 100]; /* 约 100 ms 立体声 */
ESP_ERROR_CHECK(bsp_audio_read(buf, sizeof(buf), 1000));
```

> 若直接调用 `esp_codec_dev_write()`，**返回值 `0` 才表示成功**（与 `esp_err_t` 约定相反）。

---

### IMU（QMI8658A）

| 函数 | 说明 |
|------|------|
| `esp_err_t bsp_imu_init(void)` | ±2 g / 250 Hz 加速度 + 250 Hz 采样任务 + EMA 滤波。复用共享 I2C，须先 `bsp_display_init()`。 |
| `void bsp_imu_get_accel(float *ax, float *ay, float *az)` | 滤波后加速度（m/s²，板载轴），线程安全。 |
| `void bsp_imu_get_tilt(float *gx, float *gy)` | 屏幕平面倾斜向量（m/s²）；校准通过后会减零点。`+gx` 向右，`+gy` 向下。 |

**水平校准（可选参考实现，不用可不调）：**

| 函数 | 说明 |
|------|------|
| `void bsp_imu_calib_start(void)` | 开始/重新开始校准（含 30 s 截止）。 |
| `bsp_imu_calib_status_t bsp_imu_calib_poll(void)` | `IDLE` / `RUNNING` / `PASSED` / `FAILED`。建议 UI 每 100–200 ms 轮询。 |
| `void bsp_imu_calib_get_info(float *std_g, float *tilt_deg, uint32_t *elapsed_ms)` | 校准过程中的调试观测值。 |

```c
ESP_ERROR_CHECK(bsp_imu_init());

float ax, ay, az, gx, gy;
bsp_imu_get_accel(&ax, &ay, &az);
bsp_imu_get_tilt(&gx, &gy);

bsp_imu_calib_start();
while (bsp_imu_calib_poll() == BSP_IMU_CALIB_RUNNING) {
    vTaskDelay(pdMS_TO_TICKS(100));
}
```

也可完全忽略校准，只使用 `bsp_imu_get_accel()`。

---

### SD 卡

挂载点：**`/sdcard`**（`BOARD_SD_MOUNT_POINT`）。无卡检测脚，软件无法区分「未插卡」与「挂载失败」。

| 函数 | 说明 |
|------|------|
| `esp_err_t bsp_sd_init(void)` | SDMMC 4-bit + FAT VFS。失败只返回错误、不重启。重复调用 → `ESP_ERR_INVALID_STATE`。 |
| `bool bsp_sd_is_mounted(void)` | 是否已挂载。 |
| `void bsp_sd_deinit(void)` | 卸载（未挂载时安全无操作）。 |
| `const sdmmc_card_t *bsp_sd_get_card(void)` | 卡句柄，可供 `sdmmc_card_print_info()`。 |
| `esp_err_t bsp_sd_list(const char *path, bsp_sd_entry_t **entries, size_t *count)` | 列目录（文件夹在前，strcasecmp 排序）。用完须 `bsp_sd_free_list()`。 |
| `void bsp_sd_free_list(bsp_sd_entry_t *entries, size_t count)` | 释放 `bsp_sd_list()` 结果。 |

```c
if (bsp_sd_init() == ESP_OK) {
    bsp_sd_entry_t *ents = NULL;
    size_t n = 0;
    if (bsp_sd_list("/sdcard", &ents, &n) == ESP_OK) {
        for (size_t i = 0; i < n; i++) {
            ESP_LOGI("sd", "%s %s",
                     ents[i].type == BSP_SD_ENTRY_DIR ? "DIR " : "FILE",
                     ents[i].name);
        }
        bsp_sd_free_list(ents, n);
    }
}
```

---

## 引脚一览

| 功能 | GPIO / 说明 |
|------|-------------|
| I2C | SDA **8**，SCL **9**，400 kHz |
| 触摸 CST816 | 地址 `0x15`，RST **40**，INT **41** |
| LCD ST77916 QSPI | SCL **10**，CS **11**，D0–D3 **12/13/15/14**，RST **39**，TE **38** |
| 背光 | **46**（LEDC PWM） |
| SDMMC | CLK **4**，CMD **5**，D0–D3 **3/2/7/6** |
| 电池 ADC（V1） | **1**（ADC1_CH0，÷2） |
| AXP2101（V2） | I2C 地址 `0x34`（共享总线） |
| 软关机 | **47**（保持高电平约 3.5 s；V1/V2 相同） |
| 音频 I2S | MCLK **48**，BCLK **21**，WS **17**，DOUT **16**，DIN **18** |
| PA 使能 | **45** |
| Codec ES8311 | 地址 `0x18`（共享 I2C） |
| IMU INT1 | **42**（预留；当前驱动为轮询） |

请勿占用：BOOT（0）、USB（19/20）、UART0（43/44）、Octal PSRAM（33–37）。

完整宏定义见 `include/bsp/board_config.h`。

---

## sdkconfig 注意点

本 BSP 至少建议开启：

```
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_CODEC_I2C_BACKWARD_COMPATIBLE=n
CONFIG_LV_USE_CLIB_MALLOC=y
```

字体与其它 LVGL 选项按 UI 需求配置。完整可参考 SmartRing-Plus **full-device** 示例工程的 `sdkconfig.defaults`。

---

## 目录结构

```
smartring_plus/
├── include/bsp/
│   ├── smartring_plus.h   # 公开 API
│   └── board_config.h     # 引脚与板级常量
├── smartring_bsp.c        # 显示 / 触摸 / I2C / LVGL 适配
├── bsp_audio.c            # 音频
├── bsp_battery.c
├── bsp_axp2101.c          # V2 AXP2101 底层
├── bsp_power.c
├── bsp_imu.c
├── bsp_sd.c
├── lcd_init_seq.h         # ST77916 初始化表（官方 B 版）
├── CMakeLists.txt
├── idf_component.yml
└── LICENSE
```

---

## 依赖组件（由 `idf_component.yml` 拉取）

- `espressif/esp_lvgl_adapter`
- `lvgl/lvgl` 9.x
- `espressif/esp_lcd_st77916`
- `espressif/esp_lcd_touch_cst816s`
- `espressif/esp_codec_dev`
- `waveshare/qmi8658`

---

## 许可证

Apache-2.0  
Copyright 2024–2026 Ayang / SHENZHEN VIEWE TECHNOLOGY CO.,LTD

---

## 致谢

特别感谢 **[@nianhua-entropy](https://gitee.com/nianhua-entropy)** 的慷慨帮助，使本 BSP 得以顺利完成。
