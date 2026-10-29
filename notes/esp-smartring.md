# Smart Ring Plus (VIEWE) — ESP32-S3 AI Chatbot — Research Notes

**Product:** [Smart Ring Plus — ESP32 AI Chatbot, 1.8" Smart Touch Display](https://viewedisplay.com/product/smart-ring-plus-esp32-ai-chatbot-1-8-inch-smart-touch-display/)
**Vendor:** SHENZHEN VIEWE TECHNOLOGY CO.,LTD (VIEWE / VIEWESMART)
**Category:** All-in-one ESP32-S3 circular touch terminal for interactive voice & touch AI apps.

> Note: The marketing page also lists a sibling product "Aurai Smart Ring 1P8" (1.8" AMOLED **466×466**, SH8601/CO5300). The **Smart Ring Plus** below is the **1.8" IPS 360×360 (ST77916)** variant with the dedicated `smartring_plus` BSP. Confirm which panel you actually have before choosing init tables (see "Gotchas").

---

## 1. Product overview

- **Price:** $35.99 (was $37.99)
- **Display:** 1.8-inch IPS Round, **360×360**, 400 nits (marketing; Alibaba lists 700 nits — verify) + capacitive touch
- **SoC:** ESP32-S3 (ESP32-S3-N16R8 module), 240 MHz dual-core, **8 MB Octal PSRAM + 16 MB Flash**
- **Wireless:** Wi-Fi 2.4 GHz + Bluetooth BLE 5 (and BLE Mesh on some SKUs)
- **AI interaction:** Dual MIC + speaker; pre-flashed firmware supports Meta Muse, DeepSeek, ChatGPT, Qwen, Xiaozhi (小智)
- **Hardware suite:** Built-in 6-axis IMU (QMI8658A), MicroSD slot, rechargeable LiPo battery (3.7V / 600 mAh), USB
- **Form factor:** Ø 57.6 × 12.2 mm round module
- **Firmware support:** Arduino, **ESP-IDF**, LVGL library (official BSP: `viewesmart/smartring_plus`)
- **Use cases:** AI chatbot, desktop companion (AIDA64 secondary display, MP3 playback, MJPG video, clock, weather), LVGL GUI dev board

---

## 2. Board Support Package (BSP) — the ESP-IDF path

**Repo:** https://github.com/VIEWESMART/smartring_plus
**Component registry:** https://components.espressif.com/components/viewesmart/smartring_plus
**Latest version:** **1.1.2** (v1.0.0 / v1.1.0 / v1.1.1 / v1.1.2 all in `dist/`)
**Author:** Ayang | **License:** Apache-2.0 | **ESP-IDF:** ≥ 5.5 | **Target:** `esp32s3`

The BSP wraps verified peripherals behind a stable `bsp_*` API so app code doesn't hard-code pins/init sequences/bus wiring.

```c
#include "bsp/smartring_plus.h"
```

### Subsystems exposed

| Subsystem | Hardware | Primary API |
|-----------|----------|-------------|
| Display | ST77916 QSPI, 360×360 round | `bsp_display_init()`, backlight / color-swap helpers |
| Touch | CST816 (I2C, addr 0x15) | Registered as LVGL pointer; `bsp_touch_get_handle()` |
| Shared bus | I2C SDA=8 / SCL=9 / 400 kHz | `bsp_i2c_get_bus()` |
| Audio | ES8311 + I2S + PA (NS4150B) | `bsp_audio_init()` / `read` / `write` / `set_mute` |
| Storage | SDMMC 4-bit FAT | `bsp_sd_init()`, mount `/sdcard` |
| Battery / PMIC | **V1** ADC GPIO1; **V2** AXP2101 (runtime probe) | `bsp_battery_*` / `bsp_battery_get_pmic_type()` |
| Power | GPIO47 soft power-off (same V1/V2) | `bsp_power_shutdown()` |
| IMU | QMI8658A | `bsp_imu_init()` / `get_accel` / `get_tilt` / calib helpers |
| UI stack | `esp_lvgl_adapter` + LVGL 9 | Ready after `bsp_display_init()` |

### Add as a dependency

In `idf_component.yml` (project root or `main/`):

```yaml
dependencies:
  viewesmart/smartring_plus: "^1.1.2"
```

Then:

```bash
idf.py set-target esp32s3
idf.py reconfigure
```

Alternative: drop the folder into `components/smartring_plus` as a local component.

### Dependencies pulled by the BSP

- `espressif/esp_lvgl_adapter` (^0.6.2)
- `lvgl/lvgl` **9.4.0**
- `espressif/esp_lcd_st77916` (^2.0.2)
- `espressif/esp_lcd_touch_cst816s` (1.1.1)
- `espressif/esp_codec_dev` (==1.3.4)
- `waveshare/qmi8658` (==2.0.0)

---

## 3. Recommended initialization order

`bsp_display_init()` creates the shared I2C bus, so **call it first**, then optional peripherals:

```c
#include "bsp/smartring_plus.h"
#include "esp_lv_adapter.h"
#include "esp_log.h"

void app_main(void)
{
    ESP_ERROR_CHECK(bsp_display_init());   /* I2C + touch + LCD + LVGL adapter + backlight */
    ESP_ERROR_CHECK(bsp_battery_init());
    ESP_ERROR_CHECK(bsp_power_init());

    /* Optional — fail softly if card / IMU absent */
    if (bsp_sd_init() != ESP_OK) {
        ESP_LOGW("app", "SD not mounted");
    }
    if (bsp_imu_init() != ESP_OK) {
        ESP_LOGW("app", "IMU init failed");
    }
    ESP_ERROR_CHECK(bsp_audio_init());

    ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));
    /* create LVGL screens here */
    esp_lv_adapter_unlock();
}
```

---

## 4. API reference

All public APIs in `include/bsp/smartring_plus.h`.

### 4.1 Display & touch

| Function | Description |
|----------|-------------|
| `esp_err_t bsp_display_init(void)` | Bring up shared I2C, CST816 touch, ST77916 QSPI panel, `esp_lvgl_adapter`, backlight at 100%. LVGL task running after success. |
| `void bsp_display_backlight_set(uint8_t percent)` | Backlight PWM 0–100 (GPIO46, active high). |
| `void bsp_display_set_swap_bytes(bool enable)` | Runtime RGB565 byte-swap (debug / panel variants); forces full redraw. |
| `bool bsp_display_get_swap_bytes(void)` | Current swap state. |
| `lv_display_t *bsp_display_get_handle(void)` | LVGL display handle. |
| `esp_lcd_touch_handle_t bsp_touch_get_handle(void)` | Touch handle for raw reads; `NULL` if not ready. |
| `i2c_master_bus_handle_t bsp_i2c_get_bus(void)` | Shared I2C bus for codec / IMU; `NULL` before display init. |

**LVGL note:** always take `esp_lv_adapter_lock(-1)` before creating/updating UI objects, then `esp_lv_adapter_unlock()`.

```c
ESP_ERROR_CHECK(bsp_display_init());
bsp_display_backlight_set(80);

ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));
lv_obj_t *scr = lv_obj_create(NULL);
lv_screen_load(scr);
esp_lv_adapter_unlock();
```

### 4.2 Battery / PMIC (V1 ADC and V2 AXP2101)

Cell (same on both HW revisions): single-cell LiPo **3.7 V / 600 mAh**, CV 4.2 V. Specs in `board_config.h` (`BOARD_BAT_*`).
`bsp_battery_init()` **probes** AXP2101 on the shared I2C bus (read reg `0x03`, expect CHIP ID `0x4A`).

| Path | When | Fuel / charge | Also |
|------|------|---------------|------|
| **V2** | Probe OK | REG `0xA4` %; `0x01[6:5]` direction; VBAT ADC `0x34/0x35` (1 mV/LSB) | ALDO3=3.3V; ICC 300 mA (0.5C); CV 4.2 V; precharge 75 mA; Iterm 25 mA |
| **V1** | No AXP / ID mismatch | GPIO1 ADC + same-cell OCV table | Charge from voltage hysteresis; IR-drop compensate while charging |

| Function | Description |
|----------|-------------|
| `esp_err_t bsp_battery_init(void)` | Probe and start 1 Hz sampler (requires `bsp_display_init` first). |
| `bsp_pmic_type_t bsp_battery_get_pmic_type(void)` | `BSP_PMIC_V1_ADC` / `BSP_PMIC_V2_AXP2101`. |
| `void bsp_battery_get_data(bsp_battery_data_t *out)` | Thread-safe snapshot. |
| `float bsp_battery_get_voltage(void)` | Latest voltage (V). |
| `int bsp_battery_get_percent(void)` | 0–100 %. |
| `bool bsp_battery_is_charging(void)` | Charging state. |

```c
ESP_ERROR_CHECK(bsp_battery_init());
ESP_LOGI("bat", "PMIC=%d", (int)bsp_battery_get_pmic_type());

bsp_battery_data_t bat;
bsp_battery_get_data(&bat);
ESP_LOGI("bat", "%.2f V  %d%%  %s",
         bat.voltage_v, bat.percent,
         bat.charging ? "charging" : "discharge");
```

### 4.3 Power

| Function | Description |
|----------|-------------|
| `esp_err_t bsp_power_init(void)` | Configure soft-off GPIO (default idle). |
| `void bsp_power_shutdown(void)` | Soft power-off: hold GPIO47 high for `BOARD_PW_OFF_HOLD_MS` (3.5 s). Normally does not return. |

V1 and V2 both use GPIO47; AXP2101 is not used for shutdown.

```c
ESP_ERROR_CHECK(bsp_power_init());
/* … user confirmed … */
bsp_power_shutdown();
```

### 4.4 Audio (ES8311 + I2S)

Fixed stream format: **48 kHz / 16-bit / stereo interleaved** (~192 KB/s). `bsp_audio_init()` only opens hardware; configure gain/volume via `esp_codec_dev` on the codec handle.

| Function | Description |
|----------|-------------|
| `esp_err_t bsp_audio_init(void)` | I2S + ES8311; requires prior `bsp_display_init()` for I2C. Idempotent (2nd call skips). PA driven by codec `pa_pin`. |
| `esp_codec_dev_handle_t bsp_audio_get_codec(void)` | Codec handle for gain / volume / mute / streaming. |
| `i2s_chan_handle_t bsp_audio_get_rx(void)` | I2S RX channel for raw PCM. |
| `esp_err_t bsp_audio_read(void *buf, size_t len, uint32_t timeout_ms)` | Blocking record until `len` bytes filled. |
| `esp_err_t bsp_audio_write(const void *buf, size_t len)` | Blocking playback of `len` bytes. |
| `esp_err_t bsp_audio_set_mute(bool mute)` | Output mute. |

**Playback sequence (important):** unmute → write (can be chunked) → mute again. Skipping the final mute causes a pop/click.

**Board-tuned settings (not hard requirements):**
- Mic gain ≈ **36 dB** via `esp_codec_dev_set_in_gain()` (driver snaps to 6 dB steps).
- Playback volume ≈ **75** via `esp_codec_dev_set_out_vol()` (0–100).

```c
ESP_ERROR_CHECK(bsp_audio_init());

esp_codec_dev_handle_t codec = bsp_audio_get_codec();
esp_codec_dev_set_out_vol(codec, 75);
esp_codec_dev_set_in_gain(codec, 36.0f);

/* playback */
ESP_ERROR_CHECK(bsp_audio_set_mute(false));
ESP_ERROR_CHECK(bsp_audio_write(pcm, pcm_bytes));
ESP_ERROR_CHECK(bsp_audio_set_mute(true));

/* record */
uint8_t buf[48 * 2 * 2 * 100]; /* ~100 ms stereo */
ESP_ERROR_CHECK(bsp_audio_read(buf, sizeof(buf), 1000));
```

> When calling `esp_codec_dev_write()` directly, **return value `0` means success** (opposite of `esp_err_t` convention).

### 4.5 IMU (QMI8658A)

| Function | Description |
|----------|-------------|
| `esp_err_t bsp_imu_init(void)` | ±2 g / 250 Hz accel + 250 Hz sample task + EMA filter. Needs shared I2C (`bsp_display_init()` first). |
| `void bsp_imu_get_accel(float *ax, float *ay, float *az)` | Filtered acceleration (m/s²), board axes, thread-safe. |
| `void bsp_imu_get_tilt(float *gx, float *gy)` | Screen-plane tilt vector (m/s²); subtracts calib zero if calibrated. `+gx` = right, `+gy` = down. |

**Level calibration (optional reference):**

| Function | Description |
|----------|-------------|
| `void bsp_imu_calib_start(void)` | Start / restart calibration window (30 s deadline). |
| `bsp_imu_calib_status_t bsp_imu_calib_poll(void)` | `IDLE` / `RUNNING` / `PASSED` / `FAILED`. Poll from UI every 100–200 ms. |
| `void bsp_imu_calib_get_info(float *std_g, float *tilt_deg, uint32_t *elapsed_ms)` | Debug metrics during `RUNNING`. |

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

### 4.6 SD card

Mount point: **`/sdcard`** (`BOARD_SD_MOUNT_POINT`). No card-detect pin — software cannot distinguish "no card" from "mount failure".

| Function | Description |
|----------|-------------|
| `esp_err_t bsp_sd_init(void)` | SDMMC 4-bit + FAT VFS. Does not assert/reboot on failure. 2nd call → `ESP_ERR_INVALID_STATE`. |
| `bool bsp_sd_is_mounted(void)` | Mount status. |
| `void bsp_sd_deinit(void)` | Unmount (safe if not mounted). |
| `const sdmmc_card_t *bsp_sd_get_card(void)` | Card info for `sdmmc_card_print_info()`. |
| `esp_err_t bsp_sd_list(const char *path, bsp_sd_entry_t **entries, size_t *count)` | Sorted directory listing (dirs first). Caller must `bsp_sd_free_list()`. |
| `void bsp_sd_free_list(bsp_sd_entry_t *entries, size_t count)` | Free list from `bsp_sd_list()`. |

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

## 5. Pin map (from `include/bsp/board_config.h`)

| Function | GPIOs / notes |
|----------|----------------|
| I2C | SDA **8**, SCL **9**, 400 kHz (pull-ups R8/R9 = 2.2 kΩ → 3V3) |
| Touch CST816 | Addr `0x15` (chip ID 0xB6), RST **40**, INT **41** |
| LCD ST77916 QSPI | SCL **10**, CS **11**, D0–D3 **12/13/15/14**, RST **39**, TE **38** (TE not used) |
| Backlight | **46** (LEDC PWM, AO3400A, active high) |
| SDMMC | CLK **4**, CMD **5**, D0–D3 **3/2/7/6** |
| Battery ADC (V1) | **1** (ADC1_CH0, ÷2 divider) |
| AXP2101 (V2) | I2C addr `0x34` (shared bus) |
| Soft power-off | **47** (hold high ~3.5 s; same V1/V2) |
| Audio I2S | MCLK **48**, BCLK **21**, WS **17**, DOUT **16**, DIN **18** |
| PA enable | **45** |
| Codec ES8311 | Addr `0x18` on shared I2C |
| IMU INT1 | **42** (reserved; driver currently polls) |

**Reserved / do not reuse as GPIO:** BOOT (0), USB (19/20), UART0 (43/44), Octal PSRAM (33–37, SiP-internal).

---

## 6. `sdkconfig` notes

Minimal settings the BSP expects:

```
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_CODEC_I2C_BACKWARD_COMPATIBLE=n
CONFIG_LV_USE_CLIB_MALLOC=y
```

Also enable Montserrat fonts / LVGL options as your UI requires. The BSP README points to the SmartRing-Plus **full-device** demo `sdkconfig.defaults` for a known-good complete set.

---

## 7. Build / flash workflow

```bash
# 1. Create project (or start from esp-idf template)
idf.py create-project smartring_demo && cd smartring_demo

# 2. Add the BSP dependency (see section 2), then:
idf.py set-target esp32s3
idf.py reconfigure

# 3. Build
idf.py build

# 4. Flash — device uses native USB (GPIO19/20), so the ESP32-S3 USB-Serial/JTAG port:
idf.py -p /dev/ttyACM0 flash monitor
```

> Hold **BOOT (GPIO0)** at power-up to enter download mode if the USB-Serial/JTAG console isn't auto-enumerated.

### Display init internals (for troubleshooting)

- QSPI bus on `SPI2_HOST`, 40 MHz (per `bsp` log), full-frame transfer buffer sized for `360 × 360 × 2` bytes.
- Uses the **vendor init table** from `lcd_init_seq.h` (`st77916_vendor_config_t`, `use_qspi_interface=1`). The component's default sequence is **wrong** for this panel (verified — produces vertical stripes), so the vendor table is mandatory.
- `esp_lv_adapter` config: `ESP_LV_ADAPTER_DISPLAY_SPI_WITH_PSRAM_DEFAULT_CONFIG(...)` with **`buffer_height = 40`**. A full-screen (360-line) single flush ≈ 259 KB and **exhausts internal RAM → `ESP_ERR_NO_MEM` / corrupted screen**. 40-line double buffer ≈ 56 KB is verified stable.
- `s_swap_bytes = true` by default; the adapter's "OTHER + RGB565" flush path auto-swaps bytes to match this panel. Use `bsp_display_set_swap_bytes()` only for debug / alternate panels.

---

## 8. Gotchas & pitfalls

1. **Panel revision:** `BOARD_LCD_VERSION_A` selects the init table variant. Confirm you have the "B" (new) panel; mismatch causes vertical stripes.
2. **Full-screen flush** causes `ESP_ERR_NO_MEM` with QSPI+PSRAM — keep `buffer_height` small (40).
3. **Audio pop/click:** always mute after a playback chunk; missing final mute causes a pop.
4. **`esp_codec_dev_write()` returns 0 on success** (inverted vs `esp_err_t`).
5. **SD "no card" vs "mount failure":** no card-detect pin; software can't tell them apart.
6. **IMU INT1 (GPIO42)** is reserved but currently not used — the driver polls at 250 Hz.
7. **Native USB (GPIO19/20)** is the console/flash path; UART0 (43/44) is a spare debug port. GPIO33–37 are PSRAM — never touch them.
8. **V1 vs V2 hardware:** battery path is auto-probed at runtime; soft power-off is GPIO47 in both cases (AXP2101 is not used for shutdown).

---

## 9. Related projects & ecosystems

| Project | What it gives you |
|---------|-------------------|
| [VIEWESMART/smartring_plus](https://github.com/VIEWESMART/smartring_plus) | The official ESP-IDF BSP (this doc's core). |
| [viewesmart/smartring_plus · ESP Component Registry](https://components.espressif.com/components/viewesmart/smartring_plus) | Pull it as a managed dependency (`^1.1.2`). |
| [VIEWESMART/VIEWE-SMARTRING](https://github.com/VIEWESMART/VIEWE-SMARTRING) | Older sibling board (square 1.75" AMOLED 466×466, CO5300/CST9217) — Arduino/PlatformIO + ESP-IDF examples, schematic, pin table, PDF manuals. |
| [VIEWESMART/ESP32-IDF](https://github.com/VIEWESMART/ESP32-IDF) | VIEWE's general ESP-IDF + LVGL demo collection (knob/button/touch screens, washing-machine UI). |
| [esp-arduino-libs/ESP32_Display_Panel](https://github.com/esp-arduino-libs/ESP32_Display_Panel) | Espressif display/touch driver + GUI porting library; VIEWE boards are natively integrated (Arduino + ESP-IDF + MicroPython). See `docs/board/board_viewe.md`. |
| [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) | Open-source "小智" voice AI assistant firmware. **No `viewe-smartring` board dir yet** (only `taiji-pi-s3`); the SmartRing Plus ships with VIEWE's pre-flashed Xiaozhi/Meta Muse firmware instead. You can add a board dir — see `docs/custom-board.md` (board needs `xxx_board.cc`, `config.h`, `config.json`). |
| [ESPVerse — Viewe SmartRing](https://espverse.com/hardware/viewe/viewe-smartring) | Board slug `viewe-smartring`; prebuilt mission firmware (Chronos Navio, World Cup 2026, Smartwatch LVGL, Car HUD). |
| [viewedisplay.com/ai](https://viewedisplay.com/ai/) | VIEWE AI product family overview (LLM ecosystems: OpenAI, Xiaozhi, Gemini). |
| [VIEWE 1.85" 360×360 TFT ESP32-S3 Round Smart Display (displaywiki)](https://www.displaywiki.com/products/esp32-s3-1.85inch-360x360-touch-uart-display-hmi/) | Technical wiki for the closely-related 1.85" ST77916 round board (same panel family). |

### Sibling product (don't confuse)
- **Aurai Smart Ring 1P8** — 1.8" **AMOLED 466×466**, 700 nits, SH8601/CO5300, Ø55. Use `BOARD_VIEWE_SMARTRING` in ESP32_Display_Panel, or the AMOLED init path. The `smartring_plus` BSP targets the **IPS 360×360 ST77916** version.

---

## 10. Useful links (originals)

- Product page: https://viewedisplay.com/product/smart-ring-plus-esp32-ai-chatbot-1-8-inch-smart-touch-display/
- BSP repo: https://github.com/VIEWESMART/smartring_plus
- BSP registry: https://components.espressif.com/components/viewesmart/smartring_plus
- User manual (manuals.plus mirror): https://manuals.plus/ae/1005008995336669
- VIEWE support center: https://viewedisplay.com/support-center/
- VIEWE GitHub org: https://github.com/VIEWESMART
- ESP32_Display_Panel: https://github.com/esp-arduino-libs/ESP32_Display_Panel

---

*Compiled from the product page, the `VIEWESMART/smartring_plus` v1.1.2 BSP source (README.md, README_CN.md, `include/bsp/board_config.h`, `include/bsp/smartring_plus.h`, `smartring_bsp.c`, `idf_component.yml`), the `VIEWESMART/VIEWE-SMARTRING` repo, and the 78/xiaozhi-esp32 boards tree.*

---

## 11. Working project in this repo (`smartring_hello`)

An ESP-IDF project is scaffolded at the repo root, target `esp32s3` (ESP-IDF **v6.1**), showing **"Hello World"** centered on the 1.8" display.

```
.                         # top-level CMakeLists.txt + sdkconfig.defaults
├── CMakeLists.txt        # project(smartring_hello)
├── sdkconfig.defaults    # PSRAM octal, LVGL CLIB malloc, Montserrat 14/16
├── main/
│   ├── CMakeLists.txt    # REQUIRES smartring_plus esp_lvgl_adapter lvgl
│   ├── idf_component.yml # idf >=5.5 + viewesmart/smartring_plus ^1.1.2
│   └── main.c            # bsp_display_init() + LVGL label
└── components/
    └── espressif__esp_codec_dev/   # LOCAL OVERRIDE (see below)
```

Build:

```bash
idf.py set-target esp32s3   # first time
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

`main.c` flow: `bsp_display_init()` → `bsp_battery_init()` / `bsp_power_init()` → (soft-fail) `bsp_sd_init()` / `bsp_imu_init()` → `bsp_audio_init()` → `bsp_display_backlight_set(100)` → under `esp_lv_adapter_lock(-1)` create a full-screen label "Hello World" → unlock.

### ESP-IDF v6 compatibility fix (`esp_codec_dev` 1.3.4)

The BSP pins `espressif/esp_codec_dev ==1.3.4`. That version targets ESP-IDF v5, where `driver/gpio.h`, `driver/i2c_master.h`, `driver/i2s_std.h` and `driver/spi_common.h` came from the monolithic `driver` component. In **ESP-IDF v6** these were split into `esp_driver_gpio` / `esp_driver_i2c` / `esp_driver_i2s` / `esp_driver_spi`, so `esp_codec_dev` 1.3.4 fails to build with:

```
fatal error: driver/gpio.h: No such file or directory
...
Compilation failed because audio_codec_gpio.c includes driver/gpio.h, provided by esp_driver_gpio...
```

**Fix (applied in this repo):** a local component override `components/espressif__esp_codec_dev/` (project components take precedence over `managed_components/` per the IDF build-system docs), with two edits:

1. `CMakeLists.txt` — add the split driver components to `PRIV_REQUIRES`:
   `PRIV_REQUIRES freertos esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_spi`
2. `platform/audio_codec_ctrl_spi.c` — add `#include "esp_idf_version.h"` (the `ESP_IDF_VERSION`/`ESP_IDF_VERSION_VAL` macros are no longer pulled in transitively; on v6.x the `SPI2_HOST` branch is taken, so the removed `HSPI_HOST` symbol is never compiled).

This override survives `idf.py fullclean` / dependency re-downloads (unlike patching `managed_components/`). Verify the override is actually used:

```bash
grep -oE "(components|managed_components)/espressif__esp_codec_dev" build/compile_commands.json | sort | uniq -c
# -> 171 components/espressif__esp_codec_dev   (override used)
```

> If you bump ESP-IDF or re-pull deps and the component manager recreates `managed_components/espressif__esp_codec_dev`, the `components/` override still wins; nothing to do.

### Second fix: FreeRTOS tick rate (`xTaskDelayUntil` assert)

At the **default 100 Hz FreeRTOS tick** the device booted, initialized display/touch/battery/SD, then **crashed** after IMU init:

```
I (2078) QMI8658: QMI8658 initialized successfully
I (2078) bsp_imu: QMI8658A found (WHO_AM_I=0x05, addr=0x6B)
assert failed: xTaskDelayUntil tasks.c:1499 (( xTimeIncrement > 0U ))
...
Rebooting...   # infinite boot loop
```

Cause: `bsp_imu.c:89` runs its 250 Hz sample task with `vTaskDelayUntil(&last, pdMS_TO_TICKS(IMU_SAMPLE_PERIOD_MS))` where `IMU_SAMPLE_PERIOD_MS = 4`. At 100 Hz, `pdMS_TO_TICKS(4) = 4*100/1000 = 0` ticks → the `xTimeIncrement > 0` assert.

**Fix:** `CONFIG_FREERTOS_HZ=1000` in `sdkconfig.defaults` (1 ms tick). This is what the 250 Hz IMU task and 48 kHz audio need.

```
# in sdkconfig.defaults
CONFIG_FREERTOS_HZ=1000
```

Apply by regenerating sdkconfig (`rm sdkconfig && idf.py reconfigure`) since `sdkconfig.defaults` is only read on creation. After the fix the boot log completes:

```
I (2140) bsp_imu: bsp_imu init done (accel 2G/250Hz + gyro on, 250Hz task)
I (2193) bsp_audio: 音频初始化完成：48000 Hz / 16bit / 立体声, MCLK=384x
I (2237) smartring: Hello World is on screen.
```

### Build result (reference)

```
smartring_hello.bin binary size 0xb1210 bytes. Smallest app partition is 0x100000 bytes. 0x4edf0 bytes (31%) free.
Project build complete.
```
