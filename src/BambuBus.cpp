// BambuBus.cpp - 完整移植版本，支持最新固件
// 包含：完整状态机、序列号响应、长包设置耗材、动态压力等
#include "BambuBus.h"
#include "CRC16.h"
#include "CRC8.h"
#include "settings_storage.h"  // 用于访问AMS_COUNT设置
#include "logbus.h"

#if defined(ARDUINO_ARCH_RP2040)
#include <hardware/sync.h>
#endif

// ==================== 原有变量完全保留 ====================
static CRC16 crc_16;
static CRC8 crc_8;
static constexpr uint16_t BUS_FRAME_CAPACITY = 1000u;
static uint8_t BambuBus_data_buf[BUS_FRAME_CAPACITY];
static volatile int BambuBus_have_data = 0;
static volatile uint32_t last_bus_rx_ms = 0;
static volatile bool bus_rx_seen = false;
static volatile uint32_t rx_error_frames = 0;

#if defined(ARDUINO_ARCH_RP2040)
static inline void rx_error_increment() {
    const uint32_t state = save_and_disable_interrupts();
    ++rx_error_frames;
    restore_interrupts(state);
}
static inline uint32_t rx_error_load() {
    const uint32_t state = save_and_disable_interrupts();
    const uint32_t value = rx_error_frames;
    restore_interrupts(state);
    return value;
}
#else
static void rx_error_increment() { __atomic_add_fetch(&rx_error_frames, 1u, __ATOMIC_RELAXED); }
static uint32_t rx_error_load() { return __atomic_load_n(&rx_error_frames, __ATOMIC_RELAXED); }
#endif

static unsigned char now_op_num = 255;
static int cmd = 0;

#define BambuBus_address_AMS08 ((uint16_t)0x700)
//#define BambuBus_address_AMSLite ((uint16_t)0x1200)
#define BambuBus_address_AMSLite ((uint16_t)0x1300)

#define SERSIAL_NUMBER long_packge_version_serial_number_AMS_lite
#define VERSION_NAME long_packge_version_version_and_name_AMS_lite

// ==================== 新协议内部状态机定义 ====================
enum class _filament_motion_internal : uint8_t {
    idle = 0,
    send_out = 1,
    before_on_use = 2,
    on_use = 3,
    stop_on_use = 4,
    before_pull_back = 5,
    pull_back = 6
};

struct _filament_ext {
    _filament_motion_internal motion = _filament_motion_internal::idle;
    uint32_t meters_virtual_count = 0;
    uint32_t last_time_ticks = 0;
    bool is_sendout_onuse_timing = false;
    uint32_t sendout_onuse_start_ticks = 0;
};

static _filament_ext filament_ext[4][4];
static uint8_t bambubus_ams_map[4] = {0, 1, 2, 3};
static uint16_t bambubus_ams_address = 0x0000;
static uint32_t time_sendout_onuse_ticks[4] = {};
static bool have_registered[4] = {false, false, false, false};

static uint32_t get_ticks32() {
    return static_cast<uint32_t>(get_time64());
}

// ==================== 原有_filament结构体完全保留 ====================
struct _filament {
    char ID[8] = "GFG00";
    uint8_t color_R = 0xFF;
    uint8_t color_G = 0xFF;
    uint8_t color_B = 0xFF;
    uint8_t color_A = 0xFF;
    int16_t temperature_min = 220;
    int16_t temperature_max = 240;
    char name[20] = "PETG";
    _filament_status statu = online;
    float meters = 0;
    _filament_motion_state_set motion_set = waiting;
};

namespace {
    struct alignas(4) flash_save_struct {
        _filament filament[4][4];
        int BambuBus_now_filament_num = 0;
        uint32_t check = 0x40614061;
        uint32_t format_version = 1;
        uint32_t crc32 = 0;
    } data_save;
}

static constexpr uint32_t FILAMENT_FORMAT_VERSION = 1u;
static bool Bambubus_need_to_save = false;

static uint32_t filament_record_crc(const flash_save_struct *record) {
    flash_save_struct copy = *record;
    copy.crc32 = 0;
    const auto bytes = reinterpret_cast<const uint8_t *>(&copy);
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < sizeof(copy); i++) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

// ==================== 存储操作API（后端见 platform.cpp）====================
bool Bambubus_read() {
    hal_storage_read(HAL_REGION_FILAMENT, &data_save, sizeof(data_save));
    if (data_save.check == 0x40614061 &&
        data_save.format_version == FILAMENT_FORMAT_VERSION &&
        filament_record_crc(&data_save) == data_save.crc32)
        return true;

    data_save = flash_save_struct(); // 空片或格式不匹配：恢复默认值
    Bambubus_need_to_save = true;
    return false;
}

void Bambubus_set_need_to_save() {
    Bambubus_need_to_save = true;
}

static bool Bambubus_save() {
    data_save.format_version = FILAMENT_FORMAT_VERSION;
    data_save.crc32 = filament_record_crc(&data_save);
    if (!hal_storage_write(HAL_REGION_FILAMENT, &data_save, sizeof(data_save))) return false;

    flash_save_struct saved;
    hal_storage_read(HAL_REGION_FILAMENT, &saved, sizeof(saved));
    return saved.check == data_save.check &&
           saved.format_version == FILAMENT_FORMAT_VERSION &&
           saved.crc32 == filament_record_crc(&saved) &&
           memcmp(&data_save, &saved, sizeof(data_save)) == 0;
}

// ==================== 数据访问API ====================
int get_now_filament_num() {
    return data_save.BambuBus_now_filament_num;
}

int get_current_AMS_index() {
    return data_save.BambuBus_now_filament_num < 16
               ? data_save.BambuBus_now_filament_num / 4
               : -1;
}


void reset_filament_meters(const int num) {
    if (num < 0 || num >= 16) return;
    data_save.filament[num / 4][num % 4].meters = 0;
    filament_ext[num / 4][num % 4].meters_virtual_count = 0;
}

void add_filament_meters(const int num, float meters) {
    if (num < 0 || num >= 16) return;
    data_save.filament[num / 4][num % 4].meters += meters;
}

float get_filament_meters(const int num) {
    if (num < 0 || num >= 16) return 0.0f;
    return data_save.filament[num / 4][num % 4].meters;
}

bool is_filament_online(const int num) {
    if (num < 0 || num >= 16) return false;
    return data_save.filament[num / 4][num % 4].statu == online;
}

// 设置耗材在线状态
void set_filament_online(const int num, const int count, const bool if_online) {
    if (num < 0 || num >= 4 || count < 0 || count >= 4) return;
    data_save.filament[num][count].statu = if_online ? online : offline;
}

String get_now_filament_name(const int num) {
    if (num < 0 || num >= 16) return {};
    return {data_save.filament[num / 4][num % 4].name};
}

static _filament_motion_state_set map_internal_to_api(_filament_motion_internal m) {
    switch (m) {
        case _filament_motion_internal::send_out:
            return need_send_out;
        case _filament_motion_internal::pull_back:
        case _filament_motion_internal::before_pull_back:
            return need_pull_back;
        default:
            return waiting;
    }
}

static void set_motion_internal(const uint8_t ams_num, const uint8_t ch, const _filament_motion_internal new_motion) {
    if (ams_num >= 4 || ch >= 4) return;
    if (new_motion == _filament_motion_internal::idle) BMCU_printer_idle(ams_num * 4 + ch);
    filament_ext[ams_num][ch].motion = new_motion;
    data_save.filament[ams_num][ch].motion_set = map_internal_to_api(new_motion);
}

_filament_motion_state_set get_filament_motion(const int num) {
    if (num < 0 || num >= 16) return waiting;
    return data_save.filament[num / 4][num % 4].motion_set;
}

void set_filament_motion(const int num, const _filament_motion_state_set motion) {
    if (num < 0 || num >= 16) return;
    data_save.filament[num / 4][num % 4].motion_set = motion;
    const uint8_t ams = num / 4;
    const uint8_t ch = num % 4;
    switch (motion) {
        case need_send_out:
            filament_ext[ams][ch].motion = _filament_motion_internal::send_out;
            break;
        case need_pull_back:
            filament_ext[ams][ch].motion = _filament_motion_internal::pull_back;
            break;
        default:
            filament_ext[ams][ch].motion = _filament_motion_internal::idle;
            break;
    }
}

uint8_t get_filament_color_A(const int num) {
    if (num < 0 || num >= 16) return 0;
    return data_save.filament[num / 4][num % 4].color_A;
}

uint8_t get_filament_color_R(const int num) {
    if (num < 0 || num >= 16) return 0;
    return data_save.filament[num / 4][num % 4].color_R;
}

uint8_t get_filament_color_G(const int num) {
    if (num < 0 || num >= 16) return 0;
    return data_save.filament[num / 4][num % 4].color_G;
}

uint8_t get_filament_color_B(const int num) {
    if (num < 0 || num >= 16) return 0;
    return data_save.filament[num / 4][num % 4].color_B;
}

// ==================== 新协议状态机核心 ====================
static uint8_t get_loaded_filament(const uint8_t ams_num) {
    const int now = data_save.BambuBus_now_filament_num;
    if (now >= ams_num * 4 && now < (ams_num + 1) * 4)
        return now % 4;
    return 0xFF;
}

static bool process_motion_state_machine(
    const uint8_t ams_num,
    const uint8_t filament_channel,
    const uint8_t statu_flags,
    const uint8_t motion_flag,
    const uint32_t time_used_ms) {
    if (ams_num >= AMS_COUNT || ams_num >= 4 || filament_channel >= 4) return false;

    _filament_ext *ext = &filament_ext[ams_num][filament_channel];
    _filament *fil = &data_save.filament[ams_num][filament_channel];

    ext->last_time_ticks = get_ticks32();

    const uint8_t loaded = get_loaded_filament(ams_num);
    const bool allow_any = loaded == 0xFF || loaded == filament_channel;
    const bool allow_stop = loaded == filament_channel;

    bool state_changed = false;

    // 始终使用AMS08地址的逻辑
    {
        const bool is_send_out = statu_flags == 0x03 && motion_flag == 0x00;
        const bool is_before_on_use = statu_flags == 0x09 && motion_flag == 0xA5;
        const bool is_stop_on_use = statu_flags == 0x07 && motion_flag == 0x00;
        const bool is_on_use = statu_flags == 0x07 && motion_flag == 0x7F;
        const bool is_before_pullb = statu_flags == 0x09 && motion_flag == 0x3F;

        const bool accept =
                is_send_out ||
                (is_before_on_use && allow_any) ||
                (is_on_use && allow_any) ||
                (is_stop_on_use && allow_stop) ||
                (is_before_pullb && allow_stop);

        if (accept) {
            if (data_save.BambuBus_now_filament_num != ams_num * 4 + filament_channel) {
                if (data_save.BambuBus_now_filament_num < 16) {
                    const uint8_t prev_ams = data_save.BambuBus_now_filament_num / 4;
                    const uint8_t prev_ch = data_save.BambuBus_now_filament_num % 4;
                    set_motion_internal(prev_ams, prev_ch, _filament_motion_internal::idle);
                    time_sendout_onuse_ticks[prev_ch] = 0;
                }
                data_save.BambuBus_now_filament_num = ams_num * 4 + filament_channel;
            }
        }

        if (is_send_out) {
            time_sendout_onuse_ticks[filament_channel] = 0;

            if (ext->motion != _filament_motion_internal::send_out && loaded != 0xFF) {
                // 卸载其他通道
                for (int i = 0; i < 4; i++) {
                    if (i != filament_channel) {
                        set_motion_internal(ams_num, i, _filament_motion_internal::idle);
                    }
                }
            }

            set_motion_internal(ams_num, filament_channel, _filament_motion_internal::send_out);
            state_changed = true;
        } else if (is_before_on_use && allow_any) {
            time_sendout_onuse_ticks[filament_channel] = 0;
            set_motion_internal(ams_num, filament_channel, _filament_motion_internal::before_on_use);
            state_changed = true;
        } else if (is_on_use && allow_any) {
            if (ext->motion == _filament_motion_internal::send_out) {
                const uint32_t now = get_ticks32();
                if (time_sendout_onuse_ticks[filament_channel] == 0)
                    time_sendout_onuse_ticks[filament_channel] = now;

                const uint32_t dt = now - time_sendout_onuse_ticks[filament_channel];
                if (dt < 5000)
                    return true;
                time_sendout_onuse_ticks[filament_channel] = 0;
            }

            set_motion_internal(ams_num, filament_channel, _filament_motion_internal::on_use);
            if (ext->meters_virtual_count < 10000) {
                fil->meters += static_cast<float>(time_used_ms) / 300000.0f;
                ext->meters_virtual_count += time_used_ms;
            }
            state_changed = true;
        } else if (is_stop_on_use && allow_stop) {
            time_sendout_onuse_ticks[filament_channel] = 0;
            if (ext->motion == _filament_motion_internal::on_use ||
                ext->motion == _filament_motion_internal::before_on_use) {
                set_motion_internal(ams_num, filament_channel, _filament_motion_internal::stop_on_use);
                state_changed = true;
            }
        } else if (is_before_pullb && allow_stop) {
            time_sendout_onuse_ticks[filament_channel] = 0;
            if (ext->motion == _filament_motion_internal::on_use ||
                ext->motion == _filament_motion_internal::before_on_use ||
                ext->motion == _filament_motion_internal::stop_on_use) {
                set_motion_internal(ams_num, filament_channel, _filament_motion_internal::before_pull_back);
                state_changed = true;
            }
        } else if (statu_flags == 0x09) {
            time_sendout_onuse_ticks[filament_channel] = 0;
        }
    }

    return state_changed;
}

// ==================== UART底层（完全保留）====================
static bool TX_IDLE = true;
// send_uart() 是同步发送的；处理一条 RX 报文时，响应 TX 继承当前 RX 的类型。
// 没有对应 RX 来源的主动发送保持 -1，网页归入“未知”。
static int bus_tx_context_type = -1;

void send_uart(const unsigned char *data, uint16_t length) {
    // RS485_MODE_RXTX（只用 RXD/TXD）时模块自行控制收发方向，不驱动 DE
    const bool drive_de = (RS485_MODE == RS485_MODE_DE);

    TX_IDLE = false;
    if (drive_de) digitalWrite(BUS_DE, 1);
    hal_bus_write(data, length);
    if (drive_de) digitalWrite(BUS_DE, 0);
    TX_IDLE = true;

    if (BUS_LOG_ENABLE) logbus_hex_typed(LOG_CH_BUS, 'T', bus_tx_context_type, data, length);
}

static uint8_t buf_X[BUS_FRAME_CAPACITY];
static uint8_t buf_Y[BUS_FRAME_CAPACITY];
static uint8_t *const completed_buf[2] = {buf_X, buf_Y};
static volatile int8_t completed_slot = -1;
static volatile uint16_t completed_len[2] = {};
static uint8_t rx_publish_slot = 0;
static volatile uint32_t rx_dropped_frames = 0;
CRC8 _RX_IRQ_crcx(0x39, 0x66, 0x00, false, false);

#if defined(ARDUINO_ARCH_RP2040)
static inline void rx_dropped_increment() {
    const uint32_t state = save_and_disable_interrupts();
    ++rx_dropped_frames;
    restore_interrupts(state);
}
static inline uint32_t rx_dropped_load() {
    const uint32_t state = save_and_disable_interrupts();
    const uint32_t value = rx_dropped_frames;
    restore_interrupts(state);
    return value;
}
static inline int8_t completed_slot_load() {
    const uint32_t state = save_and_disable_interrupts();
    const int8_t value = completed_slot;
    restore_interrupts(state);
    return value;
}
static inline bool completed_slot_publish(int8_t slot, uint16_t length) {
    const uint32_t state = save_and_disable_interrupts();
    if (completed_slot != -1) {
        restore_interrupts(state);
        return false;
    }
    completed_slot = slot;
    BambuBus_have_data = length;
    restore_interrupts(state);
    return true;
}
static inline int8_t completed_slot_take() {
    const uint32_t state = save_and_disable_interrupts();
    const int8_t slot = completed_slot;
    completed_slot = -1;
    BambuBus_have_data = 0;
    restore_interrupts(state);
    return slot;
}
#else
static void rx_dropped_increment() { __atomic_add_fetch(&rx_dropped_frames, 1u, __ATOMIC_RELAXED); }
static uint32_t rx_dropped_load() { return __atomic_load_n(&rx_dropped_frames, __ATOMIC_RELAXED); }
static int8_t completed_slot_load() { return __atomic_load_n(&completed_slot, __ATOMIC_ACQUIRE); }
static bool completed_slot_publish(int8_t slot, uint16_t length) {
    int8_t expected = -1;
    if (!__atomic_compare_exchange_n(&completed_slot, &expected, slot, false,
                                     __ATOMIC_RELEASE, __ATOMIC_RELAXED)) return false;
    __atomic_store_n(&BambuBus_have_data, (int) length, __ATOMIC_RELEASE);
    return true;
}
static int8_t completed_slot_take() {
    const int8_t slot = __atomic_exchange_n(&completed_slot, -1, __ATOMIC_ACQUIRE);
    __atomic_store_n(&BambuBus_have_data, 0, __ATOMIC_RELAXED);
    return slot;
}
#endif

static void RX_IRQ(unsigned char _RX_IRQ_data) {
    static int _index = 0;
    static uint16_t length = 0;
    static uint8_t data_length_index = 0;
    static uint8_t data_CRC8_index = 0;
    static uint32_t last_byte_ms = 0;
    unsigned char data = _RX_IRQ_data;

    const uint32_t now_ms = millis();
    if (_index != 0 && now_ms - last_byte_ms > 100u)
        _index = 0;
    last_byte_ms = now_ms;

    if (_index == 0) {
        if (data == 0x3D) {
            BambuBus_data_buf[0] = 0x3D;
            _RX_IRQ_crcx.restart();
            _RX_IRQ_crcx.add(0x3D);
            data_length_index = 4;
            length = 0;
            _index = 1;
        }
    } else {
        if (_index >= BUS_FRAME_CAPACITY) {
            _index = 0;
            return;
        }

        BambuBus_data_buf[_index] = data;
        if (_index == 1) {
            if (data & 0x80) {
                data_length_index = 2;
                data_CRC8_index = 3;
            } else {
                // Long packets store the total frame length as uint16 at [4..5].
                data_length_index = 5;
                data_CRC8_index = 6;
            }
        }
        if (_index == data_length_index) {
            const bool is_long = (BambuBus_data_buf[1] & 0x80u) == 0u;
            length = is_long ? static_cast<uint16_t>(BambuBus_data_buf[4]) | (static_cast<uint16_t>(data) << 8) : data;
        }

        if (_index == data_length_index) {
            const bool is_long = (BambuBus_data_buf[1] & 0x80u) == 0u;
            const uint16_t min_length = is_long ? 15u : 7u;
            if (length < min_length || length > BUS_FRAME_CAPACITY) {
                if (BUS_LOG_ENABLE)
                    logbus_hex_typed(LOG_CH_BUS, 'R', -1, BambuBus_data_buf,
                                     static_cast<uint16_t>(_index) + 1u);
                rx_error_increment();
                _index = 0;
                length = 0;
                return;
            }
        }

        if (_index < data_CRC8_index) {
            _RX_IRQ_crcx.add(data);
        } else if (_index == data_CRC8_index) {
            if (data != _RX_IRQ_crcx.calc()) {
                if (BUS_LOG_ENABLE)
                    logbus_hex_typed(LOG_CH_BUS, 'R', -1, BambuBus_data_buf,
                                     static_cast<uint16_t>(_index) + 1u);
                rx_error_increment();
                _index = 0;
                return;
            }
        }
        if (length != 0 && static_cast<uint16_t>(_index) + 1u >= length) {
            if (completed_slot_load() == -1) {
                memcpy(completed_buf[rx_publish_slot], BambuBus_data_buf, length);
                completed_len[rx_publish_slot] = length;
                if (completed_slot_publish((int8_t) rx_publish_slot, length)) {
                    rx_publish_slot ^= 1u;
                } else {
                    rx_dropped_increment();
                }
            } else {
                rx_dropped_increment();
            }
            _index = 0;
            length = 0;
            return;
        }

        ++_index;
        if (_index >= BUS_FRAME_CAPACITY) {
            _index = 0;
            length = 0;
        }
    }
}

static void on_uart_rx() {
    bool received = false;

    while (hal_bus_available()) {
        received = true;
        const auto c = static_cast<unsigned char>(hal_bus_read());
        RX_IRQ(c);
    }
    if (received) {
        last_bus_rx_ms = millis();
        bus_rx_seen = true;
    }
}

void BambuBUS_UART_Init(uint32_t baudrate) {
    hal_bus_begin(baudrate);
    BUS_IRQ_SETUP();
}

// ==================== 初始化 ====================
void BambuBus_init() {
    const bool _init_ready = Bambubus_read();
    crc_8.reset(0x39, 0x66, 0, false, false);
    crc_16.reset(0x1021, 0x913D, 0, false, false);
    if (RS485_MODE == RS485_MODE_DE) {
        digitalWrite(BUS_DE, LOW);
        pinMode(BUS_DE, OUTPUT);
    }
    LOG_MAIN("RS485 模式 = %d (%s)", RS485_MODE,
             RS485_MODE == RS485_MODE_RXTX ? "仅 RXD/TXD，不驱动 DE" : "RO/DI/RE/DE，DE 控制方向");

    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            filament_ext[i][j] = _filament_ext();
        }
        time_sendout_onuse_ticks[i] = 0;
        have_registered[i] = false;
    }
    bambubus_ams_address = 0x0000;

    if (!_init_ready) {
        data_save.filament[0][0].color_R = 0xFF;
        data_save.filament[0][0].color_G = 0x00;
        data_save.filament[0][0].color_B = 0x00;
        data_save.filament[0][1].color_R = 0x00;
        data_save.filament[0][1].color_G = 0xFF;
        data_save.filament[0][1].color_B = 0x00;
        data_save.filament[0][2].color_R = 0x00;
        data_save.filament[0][2].color_G = 0x00;
        data_save.filament[0][2].color_B = 0xFF;
        data_save.filament[0][3].color_R = 0x88;
        data_save.filament[0][3].color_G = 0x88;
        data_save.filament[0][3].color_B = 0x88;

        data_save.filament[1][0].color_R = 0xC0;
        data_save.filament[1][0].color_G = 0x20;
        data_save.filament[1][0].color_B = 0x20;
        data_save.filament[1][1].color_R = 0x20;
        data_save.filament[1][1].color_G = 0xC0;
        data_save.filament[1][1].color_B = 0x20;
        data_save.filament[1][2].color_R = 0x20;
        data_save.filament[1][2].color_G = 0x20;
        data_save.filament[1][2].color_B = 0xC0;
        data_save.filament[1][3].color_R = 0x60;
        data_save.filament[1][3].color_G = 0x60;
        data_save.filament[1][3].color_B = 0x60;

        data_save.filament[2][0].color_R = 0x80;
        data_save.filament[2][0].color_G = 0x40;
        data_save.filament[2][0].color_B = 0x40;
        data_save.filament[2][1].color_R = 0x40;
        data_save.filament[2][1].color_G = 0x80;
        data_save.filament[2][1].color_B = 0x40;
        data_save.filament[2][2].color_R = 0x40;
        data_save.filament[2][2].color_G = 0x40;
        data_save.filament[2][2].color_B = 0x80;
        data_save.filament[2][3].color_R = 0x40;
        data_save.filament[2][3].color_G = 0x40;
        data_save.filament[2][3].color_B = 0x40;

        data_save.filament[3][0].color_R = 0x40;
        data_save.filament[3][0].color_G = 0x20;
        data_save.filament[3][0].color_B = 0x20;
        data_save.filament[3][1].color_R = 0x20;
        data_save.filament[3][1].color_G = 0x40;
        data_save.filament[3][1].color_B = 0x20;
        data_save.filament[3][2].color_R = 0x20;
        data_save.filament[3][2].color_G = 0x20;
        data_save.filament[3][2].color_B = 0x40;
        data_save.filament[3][3].color_R = 0x20;
        data_save.filament[3][3].color_G = 0x20;
        data_save.filament[3][3].color_B = 0x20;
    }

    for (auto &i: data_save.filament) {
        for (auto &j: i) {
#ifdef _Bambubus_DEBUG_mode_
            j.statu = online;
#else
            j.statu = offline;
#endif
            j.motion_set = waiting;
        }
    }

    BambuBUS_UART_Init(1228800);
}

// ==================== 包处理辅助函数 ====================
static bool package_check_crc16(const uint8_t *data, int data_length) {
    if (!data || data_length < 7 || data_length > BUS_FRAME_CAPACITY)
        return false;

    crc_16.restart();
    data_length -= 2;
    for (auto i = 0; i < data_length; i++) {
        crc_16.add(data[i]);
    }
    const uint16_t num = crc_16.calc();
    if (data[data_length] == (num & 0xFF) && (data[(data_length + 1)] == ((num >> 8) & 0xFF)))
        return true;
    return false;
}

static void package_send_with_crc(uint8_t *data, int data_length) {
    crc_8.restart();
    if (data[1] & 0x80) {
        for (auto i = 0; i < 3; i++) {
            crc_8.add(data[i]);
        }
        data[3] = crc_8.calc();
    } else {
        for (auto i = 0; i < 6; i++) {
            crc_8.add(data[i]);
        }
        data[6] = crc_8.calc();
    }
    crc_16.restart();
    data_length -= 2;
    for (auto i = 0; i < data_length; i++) {
        crc_16.add(data[i]);
    }
    const uint16_t num = crc_16.calc();
    data[data_length] = num & 0xFF;
    data[(data_length + 1)] = num >> 8;
    data_length += 2;
    send_uart(data, data_length);
}

static uint8_t packge_send_buf[1000];

struct long_packge_data {
    uint16_t package_number;
    uint16_t package_length;
    uint8_t crc8;
    uint16_t target_address;
    uint16_t source_address;
    uint16_t type;
    uint8_t *datas;
    uint16_t data_length;
} __attribute__((packed));

static void Bambubus_long_package_send(long_packge_data *data) {
    packge_send_buf[0] = 0x3D;
    packge_send_buf[1] = 0x00;
    data->package_length = data->data_length + 15;
    memcpy(packge_send_buf + 2, data, 11);
    memcpy(packge_send_buf + 13, data->datas, data->data_length);
    package_send_with_crc(packge_send_buf, data->data_length + 15);
}

static void Bambubus_long_package_analysis(uint8_t *buf, int data_length, long_packge_data *data) {
    if (!buf || !data || data_length < 15 || data_length > BUS_FRAME_CAPACITY) {
        if (data) memset(data, 0, sizeof(*data));
        return;
    }

    memcpy(data, buf + 2, 11);
    data->datas = buf + 13;
    data->data_length = data_length - 15;
}

static long_packge_data printer_data_long;

// ==================== 动态压力值计算 ====================
static uint16_t get_pressure_value(uint8_t ams_num, uint8_t ch) {
    if (ams_num >= AMS_COUNT || ch >= 4) return 0xFFFF;

    const _filament_motion_internal m = filament_ext[ams_num][ch].motion;
    const _filament_motion_internal prev_motion = m;

    switch (m) {
        case _filament_motion_internal::send_out:
            return 0x4700;
        case _filament_motion_internal::before_on_use:
            return (prev_motion == _filament_motion_internal::send_out) ? 0x4700 : 0x2B00;
        case _filament_motion_internal::on_use:
            return 0x2B00;
        case _filament_motion_internal::stop_on_use:
            return (prev_motion == _filament_motion_internal::send_out) ? 0x4700 : 0x2B00;
        case _filament_motion_internal::before_pull_back:
            return 0x2B00;
        case _filament_motion_internal::pull_back:
            return 0x4700;
        default:
            return 0xFFFF;
    }
}

static uint8_t get_filament_use_flag(uint8_t ams_num) {
    if (ams_num >= AMS_COUNT) return 0x00;

    for (int i = 0; i < 4; i++) {
        _filament_motion_internal m = filament_ext[ams_num][i].motion;
        if (m != _filament_motion_internal::idle) {
            if (m == _filament_motion_internal::send_out ||
                m == _filament_motion_internal::pull_back)
                return 0x02;
            return 0x04;
        }
    }
    return 0x00;
}

// ==================== 包类型解析 ====================

// [765986] RX 3D C5 21 6B 20 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 >
// [765986] RX 00 00 00 00 00 00 00 BD 2C
// [765986] << 收到包 heartbeat 心跳 (type=9) len=33

// [0]	3D	帧头（RX_IRQ 靠它同步，src/BambuBus.cpp:469）
// [1]	C5	0x80 置位 → 短包；0x45 是源/目标路由位
// [2]	21	短包总长度 = 33，和日志 len=33 一致
// [3]	6B	CRC8（poly 0x39/init 0x66，覆盖 [0..2]）
// [4]	20	包类型 = heartbeat，对应枚举里第 9 项，所以日志写 type=9
//  [5..30]	26 × 00	载荷，本帧全 0
//  [31..32]	BD 2C	CRC16（poly 0x1021/init 0x913D，小端 = 0x2CBD，覆盖 [0..30]）

// ---------------------------

// 心跳 heartbeat
// RX 3D C5 21 6B 20 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
// RX 00 00 00 00 00 00 00 BD 2C

// 在线 online_detect
// RX 3D C5 1F 58 05 01 81 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
// RX 00 00 00 00 00 70 F8

// [765998] RX 3D C5 1F 58 05 01 81 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 >
// [765998] RX 00 00 00 00 00 70 F8
// [765999] << 收到包 online_detect 在线检测 (type=2) len=31


// 获取包类型
static package_type get_package_type(unsigned char *buf, const int length) {
    if (!buf || length < 7 || length > BUS_FRAME_CAPACITY)
        return BambuBus_package_ERROR;

    if (package_check_crc16(buf, length) == false) {
        return BambuBus_package_ERROR;
    }

    if (buf[1] & 0x80u) {
        if (buf[2] != length)
            return BambuBus_package_ERROR;

        switch (buf[4]) {
            case 0x03:
                return BambuBus_package_filament_motion_short;
            case 0x04:
                return BambuBus_package_filament_motion_long;
            case 0x05:
                return BambuBus_package_online_detect;
            case 0x06:
                return BambuBus_package_REQx6;
            case 0x07:
                return BambuBus_package_NFC_detect;
            case 0x08:
                return BambuBus_package_set_filament;
            case 0x20:
                return BambuBus_package_heartbeat;
            default:
                return BambuBus_package_ETC;
        }
    }
    if (length < 15 || (static_cast<uint16_t>(buf[4]) | (static_cast<uint16_t>(buf[5]) << 8)) != static_cast<uint16_t>(length))
        return BambuBus_package_ERROR;

    if (buf[1] == 0x05) {
        Bambubus_long_package_analysis(buf, length, &printer_data_long);

        // 始终使用AMS08地址与打印机通信
        bambubus_ams_address = BambuBus_address_AMS08;

        switch (printer_data_long.type) {
            case 0x21A:
                return BambuBus_long_package_MC_online;
            case 0x211:
                return BambuBus_longe_package_filament;
            case 0x218:
                return BambuBus_package_set_filament_type2; // 新增
            case 0x103:
                return BambuBus_long_package_version;
            case 0x402:
                return BambuBus_long_package_version;
            default:
                return BambuBus_package_ETC;
        }
    }
    return BambuBus_package_ERROR;
}

uint8_t package_num = 0;

uint8_t get_filament_left_char(const uint8_t AMS_num) {
    if (AMS_num >= 4) return 0;
    uint8_t data = 0;
    for (int i = 0; i < 4; i++) {
        if (data_save.filament[AMS_num][i].statu == online) {
            data |= (1 << i) << i;
            if (filament_ext[AMS_num][i].motion != _filament_motion_internal::idle &&
                bambubus_ams_address == BambuBus_address_AMS08) {
                data |= (2 << i) << i;
            }
        }
    }
    return data;
}

// ==================== 响应函数 ====================
#define C_test 0x00, 0x00, 0x00, 0x00, \
               0x00, 0x00, 0x80, 0xBF, \
               0x00, 0x00, 0x00, 0xC0, \
               0x00, 0xC0, 0x5D, 0xFF, \
               0xFC, 0xFF, 0xFC, 0xFF, \
               0x00, 0x00, 0x44, 0x00, \
               0x55,                   \
               0xC1, 0xC3, 0xEC, 0xBC, \
               0x01, 0x01, 0x01, 0x01,

unsigned char Cxx_res[] = {
    0x3D, 0xE0, 0x2C, 0x1A, 0x03,
    C_test 0x00, 0x00, 0x00, 0x00,
    0x90, 0xE4
};

// ==================== 辅助函数：字节序转换 ====================
static bool set_cxx_buf(unsigned char *set_buf, unsigned char *buf, int length) {
    const unsigned char AMS_num = buf[5];
    const unsigned char read_num = buf[7];
    now_op_num = read_num;
    float meters = -1;

    if (AMS_num >= AMS_COUNT || AMS_num >= 4 || read_num >= 4) return false;

    meters = data_save.filament[AMS_num][read_num].meters;

    // 根据状态设置正确的flagx
    uint8_t flagx = get_filament_use_flag(AMS_num);
    if (flagx == 0x00) flagx = 0x02; // 默认送出状态

    // 设置filament_use_flag (Cxx_res[7])
    set_buf[2] = flagx;
    // 设置filament_channel (Cxx_res[8])
    set_buf[3] = read_num;

    // 设置meters (Cxx_res[9-12])
    memcpy(set_buf + 4, &meters, sizeof(meters));

    // ========== 关键修复：设置pressure (Cxx_res[13-14]) ==========
    const uint16_t pressure = get_pressure_value(AMS_num, read_num);
    memcpy(set_buf + 8, &pressure, sizeof(pressure));

    // 设置filament_left_char (Cxx_res[29])
    set_buf[24] = get_filament_left_char(AMS_num);
    return true;
}

static void send_for_Cxx(unsigned char *buf, const int length) {
    if (!buf || length < 10) return;

    Cxx_res[1] = 0xC0 | (package_num << 3);

    if (!set_cxx_buf(Cxx_res + 5, buf, length)) return;

    package_send_with_crc(Cxx_res, sizeof(Cxx_res));
    if (package_num < 7)
        package_num++;
    else
        package_num = 0;
}


static unsigned char Dxx_res[] = {
    0x3D, 0xE0, 0x3C, 0x1A, 0x04,
    0x00, 0x75, 0x01, 0x11,
    0x04, 0x04, 0x04, 0xFF,
    0x00, 0x00, 0x00, 0x00,
    C_test 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF,
    0x90, 0xE4
};

static uint8_t res_for_06_num = 0xFF;
static int last_detect = 0;
static uint8_t filament_flag_detected = 0;

static void send_for_Dxx(const unsigned char *buf, const int length) {
    if (!buf || length < 10) return;

    unsigned char filament_flag_on = 0x00;
    unsigned char filament_flag_NFC = 0x00;
    const unsigned char AMS_num = buf[5];
    const unsigned char statu_flags = buf[6];
    const unsigned char filament_motion_flag = buf[7];
    const unsigned char read_num = buf[9];
    now_op_num = read_num;
    float meters = -1;

    if (AMS_num >= AMS_COUNT) return;

    for (auto i = 0; i < 4; i++) {
        if (data_save.filament[AMS_num][i].statu == online) {
            filament_flag_on |= 1 << i;
        } else if (data_save.filament[AMS_num][i].statu == NFC_waiting) {
            filament_flag_on |= 1 << i;
            filament_flag_NFC |= 1 << i;
        }
    }

    static uint32_t last_ticks[4] = {};
    const uint32_t now_ticks = get_ticks32();
    const uint32_t dt = last_ticks[AMS_num] == 0 ? 0 : now_ticks - last_ticks[AMS_num];
    last_ticks[AMS_num] = now_ticks;
    const uint32_t time_used = dt > 1000 ? 0 : dt;

    if (read_num != 0xFF && read_num < 4) {
        process_motion_state_machine(AMS_num, read_num, statu_flags, filament_motion_flag, time_used);
        meters = data_save.filament[AMS_num][read_num].meters;
    } else if (read_num == 0xFF && statu_flags == 0x03 && filament_motion_flag == 0x00) {
        const uint8_t now_ch = data_save.BambuBus_now_filament_num % 4;
        if (filament_ext[AMS_num][now_ch].motion == _filament_motion_internal::before_pull_back) {
            set_motion_internal(AMS_num, now_ch, _filament_motion_internal::pull_back);
        }
    } else if (read_num == 0xFF && statu_flags == 0x01) {
        const uint8_t now_ch = data_save.BambuBus_now_filament_num % 4;
        if (now_ch < 4) {
            // 卸载当前通道，设置为idle状态
            set_motion_internal(AMS_num, now_ch, _filament_motion_internal::idle);
            time_sendout_onuse_ticks[now_ch] = 0;
        }
    } else if (read_num == 0xFF) {
        if (data_save.BambuBus_now_filament_num < 16) {
            const uint8_t now_ch = data_save.BambuBus_now_filament_num % 4;
            _filament_motion_internal m = filament_ext[AMS_num][now_ch].motion;
            if (m == _filament_motion_internal::on_use ||
                m == _filament_motion_internal::before_on_use ||
                m == _filament_motion_internal::stop_on_use) {
                return;
            }
        }

        // 重置所有通道状态
        for (uint8_t i = 0; i < 4; i++) {
            set_motion_internal(AMS_num, i, _filament_motion_internal::idle);
            time_sendout_onuse_ticks[i] = 0;
        }

        data_save.BambuBus_now_filament_num = 0xFF;
    }

    uint16_t pressure = 0xFFFF;
    uint8_t use_flag = 0x00;
    if (read_num < 4) {
        pressure = get_pressure_value(AMS_num, read_num);
        use_flag = get_filament_use_flag(AMS_num);
    }

    Dxx_res[1] = 0xC0 | (package_num << 3);
    Dxx_res[5] = AMS_num;
    Dxx_res[9] = filament_flag_on;
    Dxx_res[10] = filament_flag_on - filament_flag_NFC;
    Dxx_res[11] = filament_flag_on - filament_flag_NFC;
    Dxx_res[12] = 0xFF;
    Dxx_res[17] = AMS_num;
    Dxx_res[19] = use_flag;
    Dxx_res[20] = read_num == 0xFF ? 0xFF : read_num;
    Dxx_res[13] = filament_flag_NFC;
    Dxx_res[41] = get_filament_left_char(AMS_num);

    memcpy(Dxx_res + 21, &meters, sizeof(meters));
    memcpy(Dxx_res + 25, &pressure, sizeof(pressure));
    // 处理压力值的特殊情况
    if (pressure == 0xF06F) {
        // 特殊压力值处理
    }

    if (last_detect != 0) {
        if (last_detect > 10) {
            Dxx_res[19] = 0x01;
        } else {
            Dxx_res[12] = filament_flag_detected;
            Dxx_res[19] = 0x01;
            Dxx_res[20] = filament_flag_detected;
        }
        last_detect--;
    }

    package_send_with_crc(Dxx_res, sizeof(Dxx_res));
    if (package_num < 7)
        package_num++;
    else
        package_num = 0;
}

// ==================== 在线检测（新协议完整注册机制）====================
static unsigned char F00_res[4][29] = {
    {
        0x3D, 0xC0, 0x1D, 0xB4, 0x05, 0x01, 0x00,
        0x16,
        0x0E, 0x7D, 0x32, 0x31, 0x31, 0x38, 0x15, 0x00, 0x36, 0x39, 0x37, 0x33, 0xFF, 0xFF, 0xFF, 0xFF,
        0x00, 0x00, 0x00, 0x33, 0xF0
    },
    {
        0x3D, 0xC0, 0x1D, 0xB4, 0x05, 0x01, 0x01,
        0x16,
        0x0E, 0x7D, 0x32, 0x31, 0x31, 0x38, 0x15, 0x00, 0x36, 0x39, 0x37, 0x33, 0xFF, 0xFF, 0xFF, 0xFF,
        0x00, 0x00, 0x00, 0x33, 0xF0
    },
    {
        0x3D, 0xC0, 0x1D, 0xB4, 0x05, 0x01, 0x02,
        0x16,
        0x0E, 0x7D, 0x32, 0x31, 0x31, 0x38, 0x15, 0x00, 0x36, 0x39, 0x37, 0x33, 0xFF, 0xFF, 0xFF, 0xFF,
        0x00, 0x00, 0x00, 0x33, 0xF0
    },
    {
        0x3D, 0xC0, 0x1D, 0xB4, 0x05, 0x01, 0x03,
        0x16,
        0x0E, 0x7D, 0x32, 0x31, 0x31, 0x38, 0x15, 0x00, 0x36, 0x39, 0x37, 0x33, 0xFF, 0xFF, 0xFF, 0xFF,
        0x00, 0x00, 0x00, 0x33, 0xF0
    }
};

static void bus_wait_idle_5ms() {
    uint32_t t0 = get_ticks32();
    while (get_ticks32() - t0 < 5) {
        __asm__ volatile ("nop");
    }
}

void send_for_Fxx(unsigned char *buf, int length) {
    if (!buf || length < 8) return;
    if (buf[5] == 0x01u && length < 26) return;

    // 广播注册阶段
    if (buf[5] == 0x00) {
        for (int i = 0; i < AMS_COUNT; i++) {
            if (have_registered[i]) continue;
            // AMS 是否存在由 AMS_COUNT 决定，空槽不能让整台 AMS 消失。

            // 使用对应AMS的响应数组
            F00_res[i][0] = 0x3D;
            F00_res[i][1] = 0xC0;
            F00_res[i][2] = 29;
            F00_res[i][3] = 0xB4;
            F00_res[i][4] = 0x05;
            F00_res[i][5] = 0x00;
            F00_res[i][6] = i;

            package_send_with_crc(F00_res[i], 29);
            bus_wait_idle_5ms();
        }
    }
    // 具体查询阶段
    else if (buf[5] == 0x01 && buf[6] < AMS_COUNT) {
        const uint8_t ams_num = buf[6];

        // 使用对应AMS的响应数组
        F00_res[ams_num][0] = 0x3D;
        F00_res[ams_num][1] = 0xC0;
        F00_res[ams_num][2] = 29;
        F00_res[ams_num][3] = 0xB4;
        F00_res[ams_num][4] = 0x05;
        F00_res[ams_num][5] = 0x01;
        F00_res[ams_num][6] = ams_num;

        package_send_with_crc(F00_res[ams_num], 29);

        // 验证注册状态
        if (memcmp(F00_res[ams_num] + 7, buf + 7, 20) == 0)
            have_registered[ams_num] = true;
        else
            have_registered[ams_num] = false;
    } else if (buf[4] == 0x06) // 握手
    {
        uint8_t ams_num = buf[5];
        if (res_for_06_num == 0xFF) {
            res_for_06_num = ams_num;
        } else {
            ams_num = res_for_06_num;
        }

        unsigned char F06_res[] = {
            0x3D, 0xC0, 0x0B, 0xA0, 0x06, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };

        F06_res[1] = 0xC0 | (package_num << 3);
        F06_res[5] = ams_num;

        package_send_with_crc(F06_res, sizeof(F06_res));

        if (package_num < 7)
            package_num++;
        else
            package_num = 0;
    }
}

// ==================== MC在线 ====================
unsigned char long_packge_MC_online[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

void send_for_long_packge_MC_online(unsigned char *buf, int length) {
    long_packge_data data{};
    Bambubus_long_package_analysis(buf, length, &printer_data_long);

    if (printer_data_long.data_length < 1u) return;

    // 始终使用AMS08地址，忽略目标地址检查

    const uint8_t AMS_num = printer_data_long.datas[0];
    if (AMS_num >= AMS_COUNT)
        return;

    data.datas = long_packge_MC_online;
    data.datas[0] = AMS_num;
    data.data_length = sizeof(long_packge_MC_online);

    data.package_number = printer_data_long.package_number;
    data.type = printer_data_long.type;
    data.source_address = BambuBus_address_AMS08;
    data.target_address = printer_data_long.source_address;
    Bambubus_long_package_send(&data);
}

// ==================== 读取耗材信息 ====================
unsigned char long_packge_filament[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x47, 0x46, 0x42, 0x30, 0x30, 0x00, 0x00, 0x00,
    0x41, 0x42, 0x53, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xDD, 0xB1, 0xD4, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00,
    0x18, 0x01, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

void send_for_long_packge_filament(unsigned char *buf, int length) {
    long_packge_data data{};
    Bambubus_long_package_analysis(buf, length, &printer_data_long);

    if (printer_data_long.data_length < 2u) return;

    const uint8_t AMS_num = printer_data_long.datas[0];
    const uint8_t filament_num = printer_data_long.datas[1];

    if (AMS_num >= AMS_COUNT || filament_num >= 4 ||
        data_save.filament[AMS_num][filament_num].statu != online)
        return;

    long_packge_filament[0] = AMS_num;
    long_packge_filament[1] = filament_num;
    memcpy(long_packge_filament + 19, data_save.filament[AMS_num][filament_num].ID, 8);
    memcpy(long_packge_filament + 27, data_save.filament[AMS_num][filament_num].name, 20);
    long_packge_filament[59] = data_save.filament[AMS_num][filament_num].color_R;
    long_packge_filament[60] = data_save.filament[AMS_num][filament_num].color_G;
    long_packge_filament[61] = data_save.filament[AMS_num][filament_num].color_B;
    long_packge_filament[62] = data_save.filament[AMS_num][filament_num].color_A;
    memcpy(long_packge_filament + 79, &data_save.filament[AMS_num][filament_num].temperature_max, 2);
    memcpy(long_packge_filament + 81, &data_save.filament[AMS_num][filament_num].temperature_min, 2);

    data.datas = long_packge_filament;
    data.data_length = sizeof(long_packge_filament);

    data.package_number = printer_data_long.package_number;
    data.type = printer_data_long.type;
    data.source_address = BambuBus_address_AMS08;
    data.target_address = printer_data_long.source_address;
    Bambubus_long_package_send(&data);
}

// ==================== 修复点1：版本和序列号数组定义 ====================

static unsigned char long_packge_version_serial_number_AMS_lite[] = {
    0x0F, // 序列号长度15
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, // 15字节SN
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 17字节填充
    0x0E, 0x7D, // 2字节标识
    0x32, 0x31, 0x31, 0x38, 0x15, 0x00, 0x36, 0x39, 0x37, 0x33, 0xFF, 0xFF, 0xFF, 0xFF, // 14字节额外数据
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xBB, 0x44, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00 // 最后字节，将被替换为AMS序号
};

static unsigned char long_packge_version_serial_number_AMS08[] = {
    0x0F,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0E, 0x7D,
    0x32, 0x31, 0x31, 0x38, 0x15, 0x00, 0x36, 0x39, 0x37, 0x33, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xBB, 0x44, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00
};

// 版本号数组（17字节）
static unsigned char long_packge_version_version_and_name_AMS_lite[] = {
    0x00, 0x00, 0x00, 0x3C, // 版本号60
    0x41, 0x4D, 0x53, 0x5F, 0x46, 0x31, 0x30, 0x32, 0x00, 0x00, 0x00, 0x00, 0x00, // "AMS_F102"
    0x00, 0x00, 0x00, 0x00 // 填充 + AMS序号位置
};

static unsigned char long_packge_version_version_and_name_AMS08[] = {
    0x00, 0x00, 0x00, 0x5A, // 版本号90
    0x41, 0x4D, 0x53, 0x30, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // "AMS08"
    0x00, 0x00, 0x00, 0x00 // 填充 + AMS序号位置
};

static void build_stable_serial_identity() {
    static constexpr char hex[] = "0123456789ABCDEF";
    const uint64_t id = hal_get_device_id();
    unsigned char *serials[] = {
        long_packge_version_serial_number_AMS_lite,
        long_packge_version_serial_number_AMS08
    };
    for (unsigned char *serial: serials) {
        serial[0] = 0x0F;
        serial[1] = '0';
        serial[2] = 'E';
        serial[3] = 'A';
        serial[4] = '0';
        // 11 个可见字符保留原有 15 字节 SN 长度，同时区分物理设备。
        for (int i = 0; i < 11; i++) {
            const int shift = (10 - i) * 4;
            serial[5 + i] = hex[(id >> shift) & 0x0Fu];
        }
        serial[33] = 0x0E;
        serial[34] = 0xA0;
        for (int i = 0; i < 8; i++)
            serial[35 + i] = static_cast<unsigned char>(id >> (56 - i * 8));
        serial[65] = 0;
    }
}

void send_for_long_packge_version(unsigned char *buf, int length) {
    long_packge_data data{};
    Bambubus_long_package_analysis(buf, length, &printer_data_long);

    if (printer_data_long.data_length < 1u) return;

    uint8_t AMS_num = printer_data_long.datas[0];
    unsigned char *payload = nullptr;
    uint16_t payload_len = 0;

    // 始终使用AMS08地址
    if (bambubus_ams_address != BambuBus_address_AMS08) {
        return;
    }

    if (printer_data_long.type == 0x402) // 序列号查询
    {
        if (printer_data_long.data_length < 34u) return;

        build_stable_serial_identity();

        // 关键修复：使用datas[33]作为AMS序号（原始协议要求）
        AMS_num = printer_data_long.datas[33];
        if (AMS_num >= AMS_COUNT) AMS_num = 0;

        payload = long_packge_version_serial_number_AMS08;
        payload_len = sizeof(long_packge_version_serial_number_AMS08);

        // 网页设置了自定义序列号时完整使用该值；否则使用芯片 ID 生成值，
        // 并保留原有的 AMS 编号位，避免多个 AMS 响应完全重复。
        if (CUSTOM_SERIAL[0]) {
            memcpy(payload + 1, CUSTOM_SERIAL, CUSTOM_SERIAL_LENGTH);
        } else {
            payload[4] = 0x30 + AMS_num; // SN第4字节
        }
        payload[34] = 0xA0 + AMS_num; // 标识字节
        payload[65] = AMS_num; // 最后确认
    } else if (printer_data_long.type == 0x103) // 版本查询
    {
        payload = long_packge_version_version_and_name_AMS08;
        payload_len = sizeof(long_packge_version_version_and_name_AMS08);
        // 关键修复：使用payload_len - 1避免越界
        payload[payload_len - 1] = AMS_num;
    }

    if (payload == nullptr) return;

    data.datas = payload;
    data.data_length = payload_len;
    data.package_number = printer_data_long.package_number;
    data.type = printer_data_long.type;
    data.source_address = BambuBus_address_AMS08;
    data.target_address = printer_data_long.source_address;
    Bambubus_long_package_send(&data);
}

// ==================== 设置耗材信息（短包 0xC5 0x08）====================
unsigned char Set_filament_res[] = {0x3D, 0xC0, 0x08, 0xB2, 0x08, 0x60, 0xB4, 0x04};

void send_for_Set_filament(unsigned char *buf, int length) {
    if (!buf || length < 45) return;

    uint8_t read_num = buf[5];
    const uint8_t AMS_num = (read_num >> 4) & 0x0F;
    read_num = read_num & 0x0F;
    now_op_num = read_num;

    if (AMS_num >= 4 || read_num >= 4) return;

    memcpy(data_save.filament[AMS_num][read_num].ID, buf + 7, 8);
    data_save.filament[AMS_num][read_num].color_R = buf[15];
    data_save.filament[AMS_num][read_num].color_G = buf[16];
    data_save.filament[AMS_num][read_num].color_B = buf[17];
    data_save.filament[AMS_num][read_num].color_A = buf[18];
    memcpy(&data_save.filament[AMS_num][read_num].temperature_min, buf + 19, 2);
    memcpy(&data_save.filament[AMS_num][read_num].temperature_max, buf + 21, 2);
    memcpy(data_save.filament[AMS_num][read_num].name, buf + 23, 20);
    data_save.filament[AMS_num][read_num].name[19] = '\0';

    package_send_with_crc(Set_filament_res, sizeof(Set_filament_res));
    Bambubus_set_need_to_save();
}

// ==================== 新增：设置耗材信息类型2（长包 0x05 0x218）====================
unsigned char set_filament_res_type2[] = {0x00, 0x00, 0x00};

void send_for_Set_filament_type2(unsigned char *buf, int length) {
    long_packge_data data{};
    Bambubus_long_package_analysis(buf, length, &printer_data_long);

    if (printer_data_long.data_length < 34u) return;

    const uint8_t AMS_num = printer_data_long.datas[0];
    const uint8_t read_num = printer_data_long.datas[1];

    if (AMS_num >= 4 || read_num >= 4) return;

    now_op_num = read_num;

    // 解析长包数据（偏移2字节开始）
    memcpy(data_save.filament[AMS_num][read_num].ID,
           printer_data_long.datas + 2, 8);

    data_save.filament[AMS_num][read_num].color_R = printer_data_long.datas[10];
    data_save.filament[AMS_num][read_num].color_G = printer_data_long.datas[11];
    data_save.filament[AMS_num][read_num].color_B = printer_data_long.datas[12];
    data_save.filament[AMS_num][read_num].color_A = printer_data_long.datas[13];

    memcpy(&data_save.filament[AMS_num][read_num].temperature_min,
           printer_data_long.datas + 14, 2);
    memcpy(&data_save.filament[AMS_num][read_num].temperature_max,
           printer_data_long.datas + 16, 2);

    memset(data_save.filament[AMS_num][read_num].name, 0, 20);
    memcpy(data_save.filament[AMS_num][read_num].name,
           printer_data_long.datas + 18, 16);
    data_save.filament[AMS_num][read_num].name[19] = '\0';

    // 构建响应
    set_filament_res_type2[0] = AMS_num;
    set_filament_res_type2[1] = read_num;
    set_filament_res_type2[2] = 0x00;

    data.datas = set_filament_res_type2;
    data.data_length = sizeof(set_filament_res_type2);
    data.package_number = printer_data_long.package_number;
    data.type = printer_data_long.type;
    data.source_address = BambuBus_address_AMS08;
    data.target_address = printer_data_long.source_address;
    Bambubus_long_package_send(&data);

    Bambubus_set_need_to_save();
}

int get_cmd_type() {
    return cmd;
}

unsigned char get_now_op_num() {
    return now_op_num;
}

// ==================== 主循环 ====================
static uint32_t last_bus_packet_ms = 0;
static bool bus_packet_seen = false;
static uint32_t last_heartbeat_ms = 0;
static bool heartbeat_seen = false;

int BambuBus_printer_status() {
    // 容忍偶发丢帧；仅表示连接状态，不推断打印/暂停/故障。
    return !heartbeat_seen ? 0 : millis() - last_heartbeat_ms < 5000 ? 1 : 2;
}

uint32_t BambuBus_rx_error_count() {
    return rx_error_load();
}

uint32_t BambuBus_rx_dropped_count() {
    return rx_dropped_load();
}

uint32_t BambuBus_last_valid_packet_age_ms() {
    if (!bus_packet_seen) return UINT32_MAX;
    return millis() - last_bus_packet_ms;
}

bool BambuBus_recent_activity(uint32_t quiet_ms) {
    uint32_t now = millis();
    return completed_slot_load() >= 0 ||
           BambuBus_have_data > 0 ||
           (bus_rx_seen && now - last_bus_rx_ms < quiet_ms) ||
           (bus_packet_seen && now - last_bus_packet_ms < quiet_ms);
}

static const char *package_type_name(const int type) {
    switch (type) {
        case BambuBus_package_filament_motion_short: return "filament_motion_short";
        case BambuBus_package_filament_motion_long: return "filament_motion_long";
        case BambuBus_package_online_detect: return "在线检测(online_detect)";
        case BambuBus_package_REQx6: return "REQx6";
        case BambuBus_package_NFC_detect: return "NFC检测(NFC_detect)";
        case BambuBus_package_set_filament: return "设置耗材(set_filament)";
        case BambuBus_long_package_MC_online: return "MC_online";
        case BambuBus_longe_package_filament: return "耗材信息(filament_info)";
        case BambuBus_long_package_version: return "版本(version)";
        case BambuBus_package_heartbeat: return "心跳(heartbeat)";
        case BambuBus_package_ETC: return "其他(ETC)";
        case BambuBus_package_ERROR: return "错误(ERROR)";
        case BambuBus_package_set_filament_type2: return "长包设置耗材(set_filament_type2)";
        default: return "未知(unknown)";
    }
}

int BambuBus_run() {
    // 返回值：0=本轮没有帧，1=处理了合法帧，2=收到但校验/格式错误，-1=合法通信超时。
    int stu = 0;
    const uint32_t now = millis();
    const int8_t slot = completed_slot_take();

    if (slot >= 0 && slot < 2) {
        const int data_length = completed_len[slot];
        uint8_t *const frame = completed_buf[slot];
        cmd = get_package_type(frame, data_length);
        if (cmd != BambuBus_package_ERROR) {
            stu = 1;
            last_bus_packet_ms = now;
            bus_packet_seen = true;
        } else {
            stu = 2;
            rx_error_increment();
        }
        if (BUS_LOG_ENABLE) {
            // 只在完整报文解析后记录 RX，这样十六进制行可以和类型精确绑定。
            logbus_hex_typed(LOG_CH_BUS, 'R', cmd, frame, static_cast<uint16_t>(data_length));
            LOG_BUS("<< 收到包 %s (type=%d) len=%d", package_type_name(cmd), cmd, data_length);
        }
        bus_tx_context_type = cmd;
        switch (cmd) {
            case BambuBus_package_heartbeat:
                last_heartbeat_ms = now;
                heartbeat_seen = true;
                break;
            case BambuBus_package_filament_motion_short:
                send_for_Cxx(frame, data_length);
                break;
            case BambuBus_package_filament_motion_long:
                send_for_Dxx(frame, data_length);
                break;
            case BambuBus_package_online_detect:
                send_for_Fxx(frame, data_length);
                break;
            case BambuBus_package_REQx6:
                break;
            case BambuBus_long_package_MC_online:
                send_for_long_packge_MC_online(frame, data_length);
                break;
            case BambuBus_longe_package_filament:
                send_for_long_packge_filament(frame, data_length);
                break;
            case BambuBus_long_package_version:
                send_for_long_packge_version(frame, data_length);
                break;
            case BambuBus_package_NFC_detect:
                break;
            case BambuBus_package_set_filament:
                send_for_Set_filament(frame, data_length);
                break;
            case BambuBus_package_set_filament_type2: // 新增
                send_for_Set_filament_type2(frame, data_length);
                break;
            default:
                break;
        }
        bus_tx_context_type = -1;
    }

    if (bus_packet_seen && now - last_bus_packet_ms > 1000u)
        stu = -1;

    static uint32_t next_filament_save_ms = 0;
    const uint32_t save_now = millis();
    if (Bambubus_need_to_save && static_cast<int32_t>(save_now - next_filament_save_ms) >= 0) {
        if (Bambubus_save()) {
            Bambubus_need_to_save = false;
        } else {
            LOG_MAIN("[storage] 耗材记录写入或校验失败，将稍后重试");
        }
        next_filament_save_ms = save_now + 1000u;
    }

    return stu;
}
