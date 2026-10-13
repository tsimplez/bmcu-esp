#pragma once
#include <Arduino.h>
#include <Wire.h>

// ============================================================
// 平台适配层：所有平台差异集中在此，其他文件保持平台无关
// 支持：RP2040 (Pico) / ESP32-S3 / ESP32-C3
// ============================================================

// ---------- 引脚映射 ----------

// 该平台未实现的通道使用的占位引脚
#define PIN_NONE 255

// 每对引脚：前一个=正转(进料)，后一个=反转(退料)
// #define MOTOR_PINS_1_4 {0, 1}, {12, 13}, {10, 6}, {7, 8}

#if defined(CONFIG_IDF_TARGET_ESP32C3)

  // GPIO00  排针02 可用 XTAL_32K_P        ADC1_CH0(ADC_0)   UART1_TX
  // GPIO01  排针03 可用 XTAL_32K_N        ADC1_CH1(ADC_1)   UART1_RX
  // GPIO02  排针19 可用 FSPIQ             ADC1_CH2(ADC_2)   strapping脚，上电电平要注意
  // GPIO03  排针20 可用                   ADC1_CH3(ADC_3)   纯IO，可自由用
  // GPIO04  排针28 可用 MTMS / FSPIHD     ADC1_CH4(ADC_4)   板级默认 I2C_SDA
  // GPIO05  排针27 可用 MTDI / FSPIWP     ADC2_CH0(ADC_5)   板级默认 I2C_SCL
  // GPIO06  排针22 可用 MTCK / FSPICLK    -                 纯IO，可自由用
  // GPIO07  排针23 可用 MTDO / FSPID      -                 纯IO，可自由用
  // GPIO08  排针29 可用                   -                 strapping脚，下载时被拉低，勿外挂下拉
  // GPIO09  排针30 推荐不用                -                 接BOOT按键，strapping，上电不能下拉
  // GPIO10  排针21 可用 / FSPICS0        -                 纯IO，可自由用
  // GPIO11  排针24 不用 VDD_SPI(PB_11)   -                 默认给Flash供电，需烧eFuse才能当IO
  // GPIO12  排针04 可用 SPIHD            -                 板载LED D4（高电平点亮）；DIO模式下不接Flash
  // GPIO13  排针10 可用 SPIWP            -                 板载LED D5（高电平点亮）；DIO模式下不接Flash
  // GPIO14  未引出 不用 SPICS0  ← 外置4MB Flash 片选
  // GPIO15  未引出 不用 SPICLK  ← 外置4MB Flash 时钟
  // GPIO16  未引出 不用 SPID    ← 外置4MB Flash D0
  // GPIO17  未引出 不用 SPIQ    ← 外置4MB Flash D1
  // GPIO18  排针05 不用 USB_D-（FSPICLK）         -                 经典款可用；新款被原生USB占用
  // GPIO19  排针06 不用 USB_D+（FSPICS0）         -                 经典款可用；新款被原生USB占用
  // GPIO20  排针08 可用 U0RXD            -                UART0，接板载USB转TTL（日志口，921600）
  // GPIO21  排针09 可用 U0TXD            -                UART0，接板载USB转TTL（日志口，921600）

  // ---------- 引脚方案：默认 4 通道直连 GPIO ----------
  // 是否再挂 PCF8575 由 Web 开关决定（见 PCF_ENABLE），固件不预置扩展脚映射。
  // 18 个可引出 IO 里，14-17 给外置 Flash、11 不烧 eFuse 用不了，本方案实际用 15 个：
  //   电机1  IO0  / IO1  微动1  IO2   (strapping：复位瞬间不能为低)
  //   电机2  IO12 / IO13 微动2  IO3
  //   电机3  IO10 / IO6  微动3  IO4
  //   电机4  IO7  / IO5  微动4  IO8   (strapping：复位瞬间不能为低)
  //   BambuBus RX/TX  IO18 / IO19
  //   RS485 DE  IO9
  //   控制台日志 IO20 / IO21（UART0，接板载 USB 转 TTL）
  // 注意：
  //   * IO2/IO8 是 strapping 脚：复位时该通道微动开关若闭合（有料）会把脚拉低，
  //     芯片可能进下载模式或启动失败。复位前先退料，或把这两路换到别的脚。
  //   * IO12/IO13 与板载 LED 共用，电机换向时 LED 会微亮；不想看到就焊掉 LED。
  //   * IO9 复位时必须为高（板载 BOOT 键按下即拉低），所以只做输出且平时为低，
  //     正常运行时按 BOOT 顶多跟 DE 抢一下（DE 空闲为低，不会冲突）。
  //   * 不用 IO11：默认给 Flash 供电，要当 IO 得先一次性烧 eFuse(VDD_SPI_AS_GPIO)。

  // 板型：1 = 经典款（板载 CH343 转串口，UART0 接 Type-C）→ BambuBus 用 IO18/IO19
  //       0 = 新款/简约版（Type-C 直连芯片 USB）→ BambuBus 改用 IO20/IO21，
  //           同时要额外加 -DARDUINO_USB_CDC_ON_BOOT=1 才能从板载 USB 看日志
  // [合宙 airm2m_core_esp32c3](https://wiki-zh.luatos.org/chips/esp32c3/board.html)
  #define C3_BOARD_USB_TTL 1

  #define MSW_PINS_5_8     PIN_NONE, PIN_NONE, PIN_NONE, PIN_NONE
  #define MSW_PINS_9_12    PIN_NONE, PIN_NONE, PIN_NONE, PIN_NONE
  #define MSW_PINS_13_16   PIN_NONE, PIN_NONE, PIN_NONE, PIN_NONE
  #define PIN_SWITCH       PIN_NONE  // C3 不接单路微动开关，接口保留
  #define PIN_BUS_DE       9

  // 4 通道直连 GPIO；扩展芯片的通道接线在 Web 里按 AMS 数量配置。
  #define PIN_I2C_SDA       4  // 启用 PCF8575 时的默认 I2C 接线，Web 里可改
  #define PIN_I2C_SCL       5
  #define MOTOR_PINS_1_4    {0, 1}, {12, 13}, {10, 6}, {7, 8}
  #define MSW_PINS_1_4      2, 3, 4, 5

  #if C3_BOARD_USB_TTL
    #define PIN_BUS_RX  18
    #define PIN_BUS_TX  19
  #else
    #define PIN_BUS_RX  20
    #define PIN_BUS_TX  21
  #endif

#else

  // ---------- RP2040 与 ESP32-S3 一致的引脚 ----------
  #define PIN_BUS_DE       6
  #define PIN_I2C_SDA      4
  #define PIN_I2C_SCL      5
  #define MOTOR_PINS_1_4   {11, 12}, {9, 10}, {7, 8}, {13, 14}
  #define MSW_PINS_5_8     57, 56, 55, 54  // PCF8575 虚拟引脚

  #if defined(ARDUINO_ARCH_ESP32)
    // ESP32-S3：GPIO0 是 strapping 脚且 UART0 要留给日志；26-29 被 Flash/PSRAM 占用
    #define PIN_BUS_TX  17
    #define PIN_BUS_RX  18
    #define PIN_SWITCH  42
    #define MSW_PINS_1_4 38, 39, 40, 41
  #else
    // RP2040
    #define PIN_BUS_TX  0
    #define PIN_BUS_RX  1
    #define PIN_SWITCH  0
    #define MSW_PINS_1_4 26, 27, 28, 29
  #endif

#endif

// 未提供板级默认接线的通道保持未配置，启用前需在 Web 中设置。
#ifndef MOTOR_PINS_5_8
#define MOTOR_PINS_5_8 {PIN_NONE, PIN_NONE}, {PIN_NONE, PIN_NONE}, {PIN_NONE, PIN_NONE}, {PIN_NONE, PIN_NONE}
#endif
#ifndef MSW_PINS_5_8
#define MSW_PINS_5_8 PIN_NONE, PIN_NONE, PIN_NONE, PIN_NONE
#endif
#ifndef MOTOR_PINS_9_12
#define MOTOR_PINS_9_12 {PIN_NONE, PIN_NONE}, {PIN_NONE, PIN_NONE}, {PIN_NONE, PIN_NONE}, {PIN_NONE, PIN_NONE}
#endif
#ifndef MSW_PINS_9_12
#define MSW_PINS_9_12 PIN_NONE, PIN_NONE, PIN_NONE, PIN_NONE
#endif
#ifndef MOTOR_PINS_13_16
#define MOTOR_PINS_13_16 {PIN_NONE, PIN_NONE}, {PIN_NONE, PIN_NONE}, {PIN_NONE, PIN_NONE}, {PIN_NONE, PIN_NONE}
#endif
#ifndef MSW_PINS_13_16
#define MSW_PINS_13_16 PIN_NONE, PIN_NONE, PIN_NONE, PIN_NONE
#endif

// ---------- 非易失存储 ----------
#define HAL_REGION_FILAMENT 0  // BambuBus 耗材数据
#define HAL_REGION_SETTINGS 1  // 系统设置

// 返回芯片提供的稳定物理 ID；协议层只负责格式化，不直接依赖平台 API。
uint64_t hal_get_device_id();

void hal_storage_read(int region, void* buf, uint32_t len);
bool hal_storage_write(int region, const void* buf, uint32_t len);

// 在任何外设接线初始化之前释放 USB 焊盘，避免后续覆盖 I2C/电机的复用。
#if defined(CONFIG_IDF_TARGET_ESP32C3)
#include <soc/soc.h>
#include <soc/usb_serial_jtag_reg.h>
#include <soc/io_mux_reg.h>
#endif
static inline void hal_prepare_gpio() {
    // ESP32-C3  USB-TTL 转串口芯片
#if defined(CONFIG_IDF_TARGET_ESP32C3) && C3_BOARD_USB_TTL && !ARDUINO_USB_CDC_ON_BOOT
    // 禁用内部 USB Serial/JTAG 外设对 USB 物理引脚的占用
    CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
    //  把 GPIO18 / GPIO19 切换为普通 GPIO
    PIN_FUNC_SELECT(IO_MUX_GPIO18_REG, FUNC_GPIO18_GPIO18);
    PIN_FUNC_SELECT(IO_MUX_GPIO19_REG, FUNC_GPIO19_GPIO19);
#endif
}

// ---------- I2C ----------
// 从设置加载后使用；未配置时取板级默认值。
extern int PCF_SDA;
extern int PCF_SCL;
// Web 里的 PCF8575 开关：关闭时不初始化 I2C，扩展虚拟引脚也不可选。
extern int PCF_ENABLE;
static void hal_i2c_begin() {
    if (!PCF_ENABLE) return;  // 未启用 PCF8575 时不占用 I2C 引脚
#if defined(ARDUINO_ARCH_ESP32)
    Wire.begin(PCF_SDA, PCF_SCL);
    Wire.setClock(400000);
#else
#endif
}

// ---------- 控制台串口 ----------
// Arduino-ESP32 只在「USB CDC 上电启动」（ARDUINO_USB_CDC_ON_BOOT）时才自己调用
// Serial.begin()，本工程各 env 都没开这个宏，于是 Serial 从头到尾没有 begin：
// core 内部 _uart 为 NULL，Serial.write() 会静默丢数据（不报错、不打日志）
// 表现就是串口上只剩 ROM 启动信息和 core 自己的 [E] 日志，一条应用日志都看不到
// 因此这里显式打开控制台串口（C3: UART0 GPIO20/21，S3: GPIO43/44）
#if defined(ARDUINO_ARCH_ESP32)
static void hal_console_begin() { Serial.begin(115200); }
#else
// RP2040: 日志走 stdout，无需 begin
static inline void hal_console_begin() {}
#endif

// 从持久化设置加载，PIN_BUS_* 仅作为默认值。
extern int BUS_RX, BUS_TX, BUS_DE;

// ---------- 看门狗 / BambuBus 串口 ----------
#if defined(ARDUINO_ARCH_ESP32)

#include <esp_idf_version.h>
#include <esp_task_wdt.h>
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#include <soc/soc_caps.h>
#endif
#if defined(CONFIG_IDF_TARGET_ESP32C3)
#include <soc/soc.h>
#include <soc/usb_serial_jtag_reg.h>
#include <soc/io_mux_reg.h>
#endif

#define BUS_SERIAL Serial1
#define BUS_IRQ_SETUP() BUS_SERIAL.onReceive(on_uart_rx)

static void hal_watchdog_init() {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    // IDF5 起看门狗参数从 (超时秒数, 是否 panic) 换成了结构体，而且系统启动时已经
    // 初始化过一次 TWDT：先 reconfigure，避免 init 打出 "TWDT already initialized"。
    esp_task_wdt_config_t cfg = {};
    cfg.timeout_ms = 10000;
    cfg.idle_core_mask = (1u << SOC_CPU_CORES_NUM) - 1u; // 空闲任务照旧一起盯
    cfg.trigger_panic = true;
    if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) esp_task_wdt_init(&cfg);
#else
    esp_task_wdt_init(10, true);
#endif
    esp_task_wdt_add(NULL); // 把 loop 任务挂进看门狗
}
static void hal_watchdog_feed() { esp_task_wdt_reset(); }

static void hal_bus_begin(const uint32_t baud) {
    // 1228800 bps 下默认 256 字节 RX 缓冲太小，事件任务来不及取就会丢包
    BUS_SERIAL.setRxBufferSize(2048);
    BUS_SERIAL.begin(baud, SERIAL_8E1, BUS_RX, BUS_TX, false);
}
static bool hal_bus_available() { return BUS_SERIAL.available(); }
static int hal_bus_read() { return BUS_SERIAL.read(); }
static void hal_bus_write(const uint8_t *data, const uint16_t len) {
    BUS_SERIAL.write(data, len);
    BUS_SERIAL.flush();
}

#else

#include <hardware/flash.h>
#include <hardware/irq.h>
#include <hardware/uart.h>
#include <hardware/watchdog.h>

#define BUS_SERIAL uart0
#define BUS_IRQ_SETUP()                                  \
    do {                                                 \
        irq_set_exclusive_handler(UART0_IRQ, on_uart_rx); \
        irq_set_enabled(UART0_IRQ, true);                \
        uart_set_irq_enables(BUS_SERIAL, true, false);   \
    } while (0)

static inline void hal_watchdog_init() { watchdog_enable(10000, true); }
static inline void hal_watchdog_feed() { watchdog_update(); }

static inline void hal_bus_begin(uint32_t baud) {
    uart_init(BUS_SERIAL, baud);
    gpio_set_function(BUS_TX, GPIO_FUNC_UART);
    gpio_set_function(BUS_RX, GPIO_FUNC_UART);
    uart_set_hw_flow(BUS_SERIAL, false, false);
    uart_set_format(BUS_SERIAL, 8, 1, UART_PARITY_EVEN);
    uart_set_fifo_enabled(BUS_SERIAL, true);
}
static inline bool hal_bus_available() { return uart_is_readable(BUS_SERIAL); }
static inline int hal_bus_read() { return uart_getc(BUS_SERIAL); }
static inline void hal_bus_write(const uint8_t *data, uint16_t len) {
    uart_write_blocking(BUS_SERIAL, data, len);
    uart_tx_wait_blocking(BUS_SERIAL);
}

#endif
