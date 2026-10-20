// BambuBus.cpp - 完整移植版本，支持最新固件
// 包含：完整状态机、序列号响应、长包设置耗材、动态压力等
#include "BambuBus.h"
#include "crc_bus.h"
#include "settings_storage.h"  // 用于访问AMS_COUNT设置
#include "log_bus.h"
#include "env_sensor.h"

#if defined(ARDUINO_ARCH_RP2040)
#include <hardware/sync.h>
#endif

// ==================== 原有变量完全保留 ====================
// 单帧最大容量：接收组帧、双缓冲和发送缓冲区共用该上限
static constexpr uint16_t BUS_FRAME_CAPACITY = 1000u;
/**
 * BambuBus 接收组帧工作缓冲区
 * RX_IRQ 从帧头 0x3D 开始逐字节写入，用于拼接当前正在接收的报文；
 *       只有长度合法且 CRC8 校验通过的完整帧才会复制到完成帧双缓冲。
 */
static uint8_t BambuBus_data_buf[BUS_FRAME_CAPACITY];
// 兼容旧接口的待处理帧长度；大于 0 表示双缓冲中有完整帧未取走
static volatile int BambuBus_have_data = 0;
// 最近一次从 UART 收到字节的毫秒时间
static volatile uint32_t last_bus_rx_ms = 0;
// 是否至少收到过一个 UART 字节
static volatile bool bus_rx_seen = false;
// CRC 或长度错误累计次数
static volatile uint32_t rx_error_frames = 0;

#if defined(ARDUINO_ARCH_RP2040)
/**
 * 递增接收错误帧计数（RP2040）
 * 在禁用中断的临界区内增加 rx_error_frames，保证中断与主循环并发安全。
 */
static inline void rx_error_increment() {
    const uint32_t state = save_and_disable_interrupts();
    ++rx_error_frames;
    restore_interrupts(state);
}
/**
 * 读取接收错误帧计数（RP2040）
 * @return 错误帧累计数量
 * 在禁用中断的临界区内读取 rx_error_frames。
 */
static inline uint32_t rx_error_load() {
    const uint32_t state = save_and_disable_interrupts();
    const uint32_t value = rx_error_frames;
    restore_interrupts(state);
    return value;
}
#else
/**
 * 递增接收错误帧计数（原子操作）
 * ESP32 平台使用 __atomic 原子操作增加错误计数。
 */
static void rx_error_increment() { __atomic_add_fetch(&rx_error_frames, 1u, __ATOMIC_RELAXED); }
/**
 * 读取接收错误帧计数（原子操作）
 * @return 错误帧累计数量
 * ESP32 平台使用 __atomic 原子读取错误计数。
 */
static uint32_t rx_error_load() { return __atomic_load_n(&rx_error_frames, __ATOMIC_RELAXED); }
#endif

// 最近一次协议请求操作的通道号；255 表示未知
static unsigned char now_op_num = 255;
// 最近一次识别出的 package_type 枚举值
static int cmd = 0;

#define BambuBus_address_AMS08 ((uint16_t)0x700)
//#define BambuBus_address_AMSLite ((uint16_t)0x1200)
#define BambuBus_address_AMSLite ((uint16_t)0x1300)

#define SERSIAL_NUMBER long_packge_version_serial_number_AMS_lite
#define VERSION_NAME long_packge_version_version_and_name_AMS_lite

namespace {
    // ==================== 新协议内部状态机定义 ====================
    enum class filament_motion_internal : uint8_t {
        idle = 0, // 动作结束
        send_out = 1, // 请求进料
        before_on_use = 2, // 进料准备完成
        on_use = 3, // 开始使用耗材
        stop_on_use = 4, // 停止使用耗材
        before_pull_back = 5, // 请求退料
        pull_back = 6 // 退料中
    };
}

struct _filament_ext {
    // 当前内部运动状态机状态
    filament_motion_internal motion = filament_motion_internal::idle;
    // 虚拟累计耗材长度计数，用于平滑长度反馈
    uint32_t meters_virtual_count = 0;
    // 上一次长度计算使用的毫秒时间
    uint32_t last_time_ticks = 0;
    // 是否正在统计送出到使用的计时窗口
    bool is_sendout_onuse_timing = false;
    // 送出到使用计时窗口的起始时间
    uint32_t sendout_onuse_start_ticks = 0;
};

// 每个 AMS 的 4 个通道对应的内部运动扩展状态
static _filament_ext filament_ext[4][4];
// 当前协商使用的 AMS 总线地址
static uint16_t bambubus_ams_address = 0x0000;
// 各通道开始“送出到使用”计时的毫秒戳；0 表示未计时
static uint32_t time_sendout_onuse_ticks[4] = {};
// 各 AMS 是否已完成在线检测注册
static bool have_registered[4] = {false, false, false, false};

/**
 * 获取 32 位毫秒时间截断
 * @return 64 位时间戳的低 32 位
 * 用于状态机计时，允许自然回绕。
 */
static uint32_t get_ticks32() {
    return static_cast<uint32_t>(get_time64());
}

// ==================== 原有_filament结构体完全保留 ====================
struct _filament {
    // Bambu 耗材 8 字节 ID
    char ID[8] = "GFG00";
    // 耗材颜色 RGBA，各 8 位
    uint8_t color_R = 0xFF;
    uint8_t color_G = 0xFF;
    uint8_t color_B = 0xFF;
    uint8_t color_A = 0xFF;
    // 推荐打印温度范围（摄氏度）
    int16_t temperature_min = 220;
    int16_t temperature_max = 240;
    // 耗材名称，最多 19 个可见字符加结尾 NUL
    char name[20] = "PETG";
    // 耗材在线状态
    filament_status statu = online;
    // 累计进料长度（米）
    float meters = 0;
    // 对上层暴露的运动状态
    filament_motion_state_set motion_set = waiting;
};

namespace {
    struct alignas(4) flash_save_struct {
        // 4 个 AMS × 4 个通道的耗材记录
        _filament filament[4][4];
        // 当前装在打印头上的全局通道号；0-15，0xFF 表示无
        int BambuBus_now_filament_num = 0;
        // Flash 记录有效标志
        uint32_t check = 0x40614061;
        // 记录格式版本，用于升级兼容
        uint32_t format_version = 1;
        // 整条记录的 CRC32
        uint32_t crc32 = 0;
    } data_save;
}

// 当前耗材记录格式版本
static constexpr uint32_t FILAMENT_FORMAT_VERSION = 1u;
// 是否有耗材记录等待写入 Flash
static bool BambuBus_need_to_save = false;

/**
 * 计算耗材记录 CRC32
 * @param record 待校验的 Flash 记录
 * @return 计算得到的 32 位 CRC
 * 复制记录并把 crc32 字段清零后计算 CRC32，避免校验值自引用。
 */
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
/**
 * 读取并校验耗材记录
 * @return true 表示存储中存在有效记录
 * 从持久化存储读取记录，校验魔数、格式版本和 CRC32；无效时恢复默认值并标记待保存。
 */
bool BambuBus_read() {
    hal_storage_read(HAL_REGION_FILAMENT, &data_save, sizeof(data_save));
    if (data_save.check == 0x40614061 &&
        data_save.format_version == FILAMENT_FORMAT_VERSION &&
        filament_record_crc(&data_save) == data_save.crc32)
        return true;

    data_save = flash_save_struct(); // 空片或格式不匹配：恢复默认值
    BambuBus_need_to_save = true;
    return false;
}

/**
 * 标记耗材记录需要保存
 * 设置延迟保存标志，由主循环限频写入存储。
 */
void BambuBus_set_need_to_save() {
    BambuBus_need_to_save = true;
}

/**
 * 保存耗材记录并回读校验
 * @return true 表示写入内容与回读一致
 * 更新格式版本和 CRC32 后写入存储，再回读比较整块数据。
 */
static bool BambuBus_save() {
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
/**
 * 读取当前耗材通道编号
 * @return 0-15 的通道编号；无当前通道时为 255
 * 返回打印机当前选中的全局通道号。
 */
int get_now_filament_num() {
    return data_save.BambuBus_now_filament_num;
}

/**
 * 读取当前 AMS 编号
 * @return 0-3 的 AMS 编号；无当前通道时为 -1
 * 由当前通道号除以 4 得到。
 */
int get_current_AMS_index() {
    return data_save.BambuBus_now_filament_num < 16
               ? data_save.BambuBus_now_filament_num / 4
               : -1;
}


/**
 * 清零指定通道的耗材长度
 * @param num 全局通道编号 0-15
 * 同时清零记录中的长度和内部虚拟计数。
 */
void reset_filament_meters(const int num) {
    if (num < 0 || num >= 16) return;
    data_save.filament[num / 4][num % 4].meters = 0;
    filament_ext[num / 4][num % 4].meters_virtual_count = 0;
}

/**
 * 累加指定通道的耗材长度
 * @param num 全局通道编号 0-15
 * @param meters 本次变化的长度
 * 把进料或退料距离累加到耗材记录。
 */
void add_filament_meters(const int num, const float meters) {
    if (num < 0 || num >= 16) return;
    data_save.filament[num / 4][num % 4].meters += meters;
}

/**
 * 读取指定通道的耗材长度
 * @param num 全局通道编号 0-15
 * @return 累计长度；编号越界返回 0
 * 返回记录中保存的耗材长度。
 */
float get_filament_meters(const int num) {
    if (num < 0 || num >= 16) return 0.0f;
    return data_save.filament[num / 4][num % 4].meters;
}

/**
 * 查询通道是否有料
 * @param num 全局通道编号 0-15
 * @return true 表示在线/有料
 * 读取通道状态是否为 online。
 */
bool is_filament_online(const int num) {
    if (num < 0 || num >= 16) return false;
    return data_save.filament[num / 4][num % 4].statu == online;
}

/**
 * 设置通道有料状态
 * @param num AMS 编号 0-3
 * @param count AMS 内通道编号 0-3
 * @param if_online true 表示有料
 * 更新耗材记录中的 statu 字段，供协议响应和网页显示。
 */
void set_filament_online(const int num, const int count, const bool if_online) {
    if (num < 0 || num >= 4 || count < 0 || count >= 4) return;
    data_save.filament[num][count].statu = if_online ? online : offline;
}

/**
 * 读取指定通道的耗材名称
 * @param num 全局通道编号 0-15
 * @return 耗材名称；编号越界返回空串
 * 把记录中的 20 字节名称转换为 String。
 */
String get_now_filament_name(const int num) {
    if (num < 0 || num >= 16) return {};
    return {data_save.filament[num / 4][num % 4].name};
}

/**
 * 把内部运动状态转换为对外状态
 * @param m 内部状态机枚举
 * @return waiting、need_send_out 或 need_pull_back
 * 隐藏 before_on_use、on_use 等中间状态。
 */
static filament_motion_state_set map_internal_to_api(filament_motion_internal m) {
    switch (m) {
        case filament_motion_internal::send_out:
            return need_send_out;
        case filament_motion_internal::pull_back:
        case filament_motion_internal::before_pull_back:
            return need_pull_back;
        default:
            return waiting;
    }
}

/**
 * 获取内部运动状态名称
 * @param m 内部运动状态机枚举
 * @return 供主板日志显示的中文状态
 * 只用于日志，不改变协议状态。
 */
static const char *motion_internal_name(const filament_motion_internal m) {
    switch (m) {
        case filament_motion_internal::send_out: return "请求进料";
        case filament_motion_internal::before_on_use: return "进料准备完成";
        case filament_motion_internal::on_use: return "开始使用耗材";
        case filament_motion_internal::stop_on_use: return "停止使用耗材";
        case filament_motion_internal::before_pull_back: return "请求退料";
        case filament_motion_internal::pull_back: return "退料中";
        default: return "动作结束";
    }
}

/**
 * 更新内部运动状态
 * @param ams_num AMS 编号
 * @param ch 通道编号
 * @param new_motion 新的内部状态
 * 同步内部状态与对外 motion_set；进入 idle 时通知 BMCU 解除打印机阻断。
 */
static void set_motion_internal(const uint8_t ams_num, const uint8_t ch, const filament_motion_internal new_motion) {
    if (ams_num >= 4 || ch >= 4) return;
    const auto previous = filament_ext[ams_num][ch].motion;
    if (previous != new_motion)
        LOG_MAIN("[printer] 通道 %d %s（%s -> %s）", ams_num * 4 + ch + 1,
                 motion_internal_name(new_motion), motion_internal_name(previous),
                 motion_internal_name(new_motion));
    if (new_motion == filament_motion_internal::idle) BMCU_printer_idle(ams_num * 4 + ch);
    filament_ext[ams_num][ch].motion = new_motion;
    data_save.filament[ams_num][ch].motion_set = map_internal_to_api(new_motion);
}

/**
 * 读取通道对外运动状态
 * @param num 全局通道编号 0-15
 * @return 当前运动状态；越界返回 waiting
 * 供 BMCU 运动层查询打印机请求。
 */
filament_motion_state_set get_filament_motion(const int num) {
    if (num < 0 || num >= 16) return waiting;
    return data_save.filament[num / 4][num % 4].motion_set;
}

/**
 * 设置通道对外运动状态
 * @param num 全局通道编号 0-15
 * @param motion 新的对外状态
 * 同步更新内部状态机，保证 BMCU 与协议层状态一致。
 */
void set_filament_motion(const int num, const filament_motion_state_set motion) {
    if (num < 0 || num >= 16) return;
    data_save.filament[num / 4][num % 4].motion_set = motion;
    const uint8_t ams = num / 4;
    const uint8_t ch = num % 4;
    switch (motion) {
        case need_send_out:
            filament_ext[ams][ch].motion = filament_motion_internal::send_out;
            break;
        case need_pull_back:
            filament_ext[ams][ch].motion = filament_motion_internal::pull_back;
            break;
        default:
            filament_ext[ams][ch].motion = filament_motion_internal::idle;
            break;
    }
}

/**
 * 读取耗材透明度通道
 * @param num 全局通道编号 0-15
 * @return A 通道值；越界返回 0
 * 返回 RGBA 颜色中的 A 分量。
 */
uint8_t get_filament_color_A(const int num) {
    if (num < 0 || num >= 16) return 0;
    return data_save.filament[num / 4][num % 4].color_A;
}

/**
 * 读取耗材红色通道
 * @param num 全局通道编号 0-15
 * @return R 通道值；越界返回 0
 * 返回 RGBA 颜色中的 R 分量。
 */
uint8_t get_filament_color_R(const int num) {
    if (num < 0 || num >= 16) return 0;
    return data_save.filament[num / 4][num % 4].color_R;
}

/**
 * 读取耗材绿色通道
 * @param num 全局通道编号 0-15
 * @return G 通道值；越界返回 0
 * 返回 RGBA 颜色中的 G 分量。
 */
uint8_t get_filament_color_G(const int num) {
    if (num < 0 || num >= 16) return 0;
    return data_save.filament[num / 4][num % 4].color_G;
}

/**
 * 读取耗材蓝色通道
 * @param num 全局通道编号 0-15
 * @return B 通道值；越界返回 0
 * 返回 RGBA 颜色中的 B 分量。
 */
uint8_t get_filament_color_B(const int num) {
    if (num < 0 || num >= 16) return 0;
    return data_save.filament[num / 4][num % 4].color_B;
}

// ==================== 新协议状态机核心 ====================
/**
 * 读取 AMS 当前已加载通道
 * @param ams_num AMS 编号
 * @return 0-3；该 AMS 没有加载通道时返回 0xFF
 * 判断打印机当前耗材是否位于指定 AMS 内。
 */
static uint8_t get_loaded_filament(const uint8_t ams_num) {
    const int now = data_save.BambuBus_now_filament_num;
    if (now >= ams_num * 4 && now < (ams_num + 1) * 4)
        return now % 4;
    return 0xFF;
}

/**
 * 处理 BambuBus 运动状态机
 * @param ams_num AMS 编号
 * @param filament_channel AMS 内通道编号
 * @param statu_flags 状态标志
 * @param motion_flag 运动标志
 * @param time_used_ms 距上次处理的时间
 * @return true 表示状态发生变化
 * 根据短包状态组合切换送出、使用、停止、回卷等内部状态，并维护当前通道和耗材长度。
 */
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
                    set_motion_internal(prev_ams, prev_ch, filament_motion_internal::idle);
                    time_sendout_onuse_ticks[prev_ch] = 0;
                }
                data_save.BambuBus_now_filament_num = ams_num * 4 + filament_channel;
            }
        }

        if (is_send_out) {
            time_sendout_onuse_ticks[filament_channel] = 0;

            if (ext->motion != filament_motion_internal::send_out && loaded != 0xFF) {
                // 卸载其他通道
                for (int i = 0; i < 4; i++) {
                    if (i != filament_channel) {
                        set_motion_internal(ams_num, i, filament_motion_internal::idle);
                    }
                }
            }

            set_motion_internal(ams_num, filament_channel, filament_motion_internal::send_out);
            state_changed = true;
        } else if (is_before_on_use && allow_any) {
            time_sendout_onuse_ticks[filament_channel] = 0;
            set_motion_internal(ams_num, filament_channel, filament_motion_internal::before_on_use);
            state_changed = true;
        } else if (is_on_use && allow_any) {
            if (ext->motion == filament_motion_internal::send_out) {
                const uint32_t now = get_ticks32();
                if (time_sendout_onuse_ticks[filament_channel] == 0)
                    time_sendout_onuse_ticks[filament_channel] = now;

                const uint32_t dt = now - time_sendout_onuse_ticks[filament_channel];
                if (dt < 5000)
                    return true;
                time_sendout_onuse_ticks[filament_channel] = 0;
            }

            set_motion_internal(ams_num, filament_channel, filament_motion_internal::on_use);
            if (ext->meters_virtual_count < 10000) {
                fil->meters += static_cast<float>(time_used_ms) / 300000.0f;
                ext->meters_virtual_count += time_used_ms;
            }
            state_changed = true;
        } else if (is_stop_on_use && allow_stop) {
            time_sendout_onuse_ticks[filament_channel] = 0;
            if (ext->motion == filament_motion_internal::on_use ||
                ext->motion == filament_motion_internal::before_on_use) {
                set_motion_internal(ams_num, filament_channel, filament_motion_internal::stop_on_use);
                state_changed = true;
            }
        } else if (is_before_pullb && allow_stop) {
            time_sendout_onuse_ticks[filament_channel] = 0;
            if (ext->motion == filament_motion_internal::on_use ||
                ext->motion == filament_motion_internal::before_on_use ||
                ext->motion == filament_motion_internal::stop_on_use) {
                set_motion_internal(ams_num, filament_channel, filament_motion_internal::before_pull_back);
                state_changed = true;
            }
        } else if (statu_flags == 0x09) {
            time_sendout_onuse_ticks[filament_channel] = 0;
        }
    }

    return state_changed;
}

// ==================== UART底层（完全保留）====================
// 串口发送是否空闲；send_uart() 发送期间置为 false
static bool TX_IDLE = true;
// send_uart() 是同步发送的；处理一条 RX 报文时，响应 TX 继承当前 RX 的类型。
// 没有对应 RX 来源的主动发送保持 -1，网页归入“未知”。
// 当前 TX 报文关联的 RX 类型，供日志筛选；-1 表示主动发送/未知
static int bus_tx_context_type = -1;

/**
 * 通过 BambuBus 串口发送数据
 * @param data 待发送数据
 * @param length 数据长度
 * 按 RS485 模式控制 DE 方向，发送完成后恢复接收，并可选写入原始报文日志。
 */
void send_uart(const unsigned char *data, const uint16_t length) {
    // RS485_MODE_RXTX（只用 RXD/TXD）时模块自行控制收发方向，不驱动 DE
    const bool drive_de = (RS485_MODE == RS485_MODE_DE);

    TX_IDLE = false;
    if (drive_de) digitalWrite(BUS_DE, 1);
    hal_bus_write(data, length);
    if (drive_de) digitalWrite(BUS_DE, 0);
    TX_IDLE = true;

    if (BUS_LOG_ENABLE) log_bus_hex_typed(LOG_CH_BUS, 'T', bus_tx_context_type, data, length);
}

/**
 * 完成帧双缓冲 A
 * 保存一帧已经通过长度和 CRC8 校验的 BambuBus 报文，供主循环解析；
 *       与 buf_Y 组成双缓冲，避免接收中断和主循环同时读写同一帧。
 */
static uint8_t buf_X[BUS_FRAME_CAPACITY];
/**
 * 完成帧双缓冲 B
 * 与 buf_X 交替存放完成帧；一个缓冲区被主循环处理时，另一个可继续接收新帧。
 */
static uint8_t buf_Y[BUS_FRAME_CAPACITY];
/**
 * 完成帧缓冲区索引表
 * 把槽位号映射到实际缓冲区，completed_buf[0] 指向 buf_X，
 *       completed_buf[1] 指向 buf_Y；本身不保存报文数据。
 */
static uint8_t *const completed_buf[2] = {buf_X, buf_Y};
/**
 * 待处理完成帧的槽位号
 * -1 表示当前没有待处理帧；0 或 1 表示 completed_buf/completed_len
 *       中对应槽位已经发布，BambuBus_run() 取出后交给协议解析。
 */
static volatile int8_t completed_slot = -1;
/**
 * 各完成帧槽位的报文长度
 * 记录 buf_X、buf_Y 中有效帧的字节数，主循环按槽位读取对应长度。
 */
static volatile uint16_t completed_len[2] = {};
/**
 * 下一个用于发布完成帧的槽位
 * RX_IRQ 成功发布一帧后切换 0/1，实现 buf_X 与 buf_Y 交替写入。
 */
static uint8_t rx_publish_slot = 0;
// 因双缓冲槽位已占用而丢弃的完整帧数量
static volatile uint32_t rx_dropped_frames = 0;
#if defined(ARDUINO_ARCH_RP2040)
/**
 * 递增接收丢帧计数（RP2040）
 * 在临界区内增加因双缓冲已占用而丢弃的帧数。
 */
static inline void rx_dropped_increment() {
    const uint32_t state = save_and_disable_interrupts();
    ++rx_dropped_frames;
    restore_interrupts(state);
}
/**
 * 读取接收丢帧计数（RP2040）
 * @return 丢帧累计数量
 * 在临界区内读取 rx_dropped_frames。
 */
static inline uint32_t rx_dropped_load() {
    const uint32_t state = save_and_disable_interrupts();
    const uint32_t value = rx_dropped_frames;
    restore_interrupts(state);
    return value;
}
/**
 * 读取已完成帧槽位（RP2040）
 * @return 槽位编号；没有完成帧时为 -1
 * 用于判断双缓冲中是否已有待处理帧。
 */
static inline int8_t completed_slot_load() {
    const uint32_t state = save_and_disable_interrupts();
    const int8_t value = completed_slot;
    restore_interrupts(state);
    return value;
}
/**
 * 发布已完成帧（RP2040）
 * @param slot 缓冲区槽位
 * @param length 帧长度
 * @return true 表示发布成功
 * 在临界区内占用空槽位并设置待处理帧长度。
 */
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
/**
 * 取出已完成帧（RP2040）
 * @return 槽位编号；没有完成帧时为 -1
 * 在临界区内取走槽位并清空待处理标志。
 */
static inline int8_t completed_slot_take() {
    const uint32_t state = save_and_disable_interrupts();
    const int8_t slot = completed_slot;
    completed_slot = -1;
    BambuBus_have_data = 0;
    restore_interrupts(state);
    return slot;
}
#else
/**
 * 递增接收丢帧计数（原子操作）
 * ESP32 平台原子增加丢帧计数。
 */
static void rx_dropped_increment() { __atomic_add_fetch(&rx_dropped_frames, 1u, __ATOMIC_RELAXED); }
/**
 * 读取接收丢帧计数（原子操作）
 * @return 丢帧累计数量
 * ESP32 平台原子读取丢帧计数。
 */
static uint32_t rx_dropped_load() { return __atomic_load_n(&rx_dropped_frames, __ATOMIC_RELAXED); }
/**
 * 读取已完成帧槽位（原子操作）
 * @return 槽位编号；没有完成帧时为 -1
 * ESP32 平台原子读取双缓冲槽位。
 */
static int8_t completed_slot_load() { return __atomic_load_n(&completed_slot, __ATOMIC_ACQUIRE); }
/**
 * 发布已完成帧（原子操作）
 * @param slot 缓冲区槽位
 * @param length 帧长度
 * @return true 表示发布成功
 * 使用 compare-exchange 占用空槽位并发布帧长度。
 */
static bool completed_slot_publish(int8_t slot, uint16_t length) {
    int8_t expected = -1;
    if (!__atomic_compare_exchange_n(&completed_slot, &expected, slot, false,
                                     __ATOMIC_RELEASE, __ATOMIC_RELAXED)) return false;
    __atomic_store_n(&BambuBus_have_data, (int) length, __ATOMIC_RELEASE);
    return true;
}
/**
 * 取出已完成帧（原子操作）
 * @return 槽位编号；没有完成帧时为 -1
 * 原子交换清空槽位，并复位待处理长度。
 */
static int8_t completed_slot_take() {
    const int8_t slot = __atomic_exchange_n(&completed_slot, -1, __ATOMIC_ACQUIRE);
    __atomic_store_n(&BambuBus_have_data, 0, __ATOMIC_RELAXED);
    return slot;
}
#endif

/**
 * 逐字节解析 BambuBus 接收帧
 * @param _RX_IRQ_data 串口收到的单个字节
 * 从帧头 0x3D 开始组帧，解析短包/长包长度；收到 CRC8 字节时对已缓存的头部查表校验，
 * 完整帧写入双缓冲供主循环处理。
 */
static void RX_IRQ(unsigned char _RX_IRQ_data) {
    // 当前帧已写入 BambuBus_data_buf 的字节数；0 表示等待下一个帧头
    static int _index = 0;
    // 当前帧声明的总长度；短包取长度字节，长包取 [4..5] 的小端 uint16
    static uint16_t length = 0;
    // 当前帧长度字段所在的字节下标，由短包/长包类型决定
    static uint8_t data_length_index = 0;
    // 当前帧 CRC8 字段所在的字节下标，由短包/长包类型决定
    static uint8_t data_CRC8_index = 0;
    // 上一次收到字节的毫秒时间，用于检测超过 100ms 的帧间空闲并丢弃残帧
    static uint32_t last_byte_ms = 0;
    unsigned char data = _RX_IRQ_data;

    const uint32_t now_ms = millis();
    if (_index != 0 && now_ms - last_byte_ms > 100u)
        _index = 0;
    last_byte_ms = now_ms;

    if (_index == 0) {
        if (data == 0x3D) {
            BambuBus_data_buf[0] = 0x3D;
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
                    log_bus_hex_typed(LOG_CH_BUS, 'R', -1, BambuBus_data_buf,
                                     static_cast<uint16_t>(_index) + 1u);
                rx_error_increment();
                _index = 0;
                length = 0;
                return;
            }
        }

        if (_index == data_CRC8_index) {
            if (data != bus_crc8(BambuBus_data_buf, data_CRC8_index)) {
                if (BUS_LOG_ENABLE)
                    log_bus_hex_typed(LOG_CH_BUS, 'R', -1, BambuBus_data_buf,
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

/**
 * 处理串口接收事件
 * 读空 UART 接收缓冲并逐字节送入 RX_IRQ，同时记录最近一次接收时间。
 */
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

/**
 * 初始化 BambuBus 串口与接收回调
 * @param baud 串口波特率
 * 调用平台层初始化串口，并注册接收中断或事件回调。
 */
static void BambuBUS_UART_Init(const uint32_t baud) {
    hal_bus_begin(baud);
    BUS_IRQ_SETUP();
}

// ==================== 初始化 ====================

// 初始化 BambuBus 协议层 加载耗材记录、初始化 CRC 查表接口和 RS485 DE、复位状态机，并以 1.2288Mbps 启动串口。
void BambuBus_init() {
    const bool _init_ready = BambuBus_read();
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
#ifdef _BambuBus_DEBUG_mode_
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
/**
 * 校验报文末尾的 CRC16
 * @param data 完整报文
 * @param data_length 报文长度
 * @return true 表示 CRC 正确
 * 对除末两字节外的数据查表计算 CRC16，并与报文中的小端校验值比较。
 */
static bool package_check_crc16(const uint8_t *data, int data_length) {
    if (!data || data_length < 7 || data_length > BUS_FRAME_CAPACITY)
        return false;

    data_length -= 2;
    const uint16_t num = bus_crc16(data, data_length);
    if (data[data_length] == (num & 0xFF) && (data[(data_length + 1)] == ((num >> 8) & 0xFF)))
        return true;
    return false;
}

/**
 * 填充 CRC 并发送报文
 * @param data 待发送报文
 * @param data_length 报文总长度
 * 按短包或长包位置查表填充 CRC8 和小端 CRC16，然后调用 send_uart() 发送。
 */
static void package_send_with_crc(uint8_t *data, int data_length) {
    if (data[1] & 0x80) {
        data[3] = bus_crc8(data, 3);
    } else {
        data[6] = bus_crc8(data, 6);
    }
    data_length -= 2;
    const uint16_t num = bus_crc16(data, data_length);
    data[data_length] = num & 0xFF;
    data[(data_length + 1)] = num >> 8;
    data_length += 2;
    send_uart(data, data_length);
}

// 长包发送组帧缓冲区：头部、载荷和 CRC 在此拼接后发送
static uint8_t packge_send_buf[1000];

struct long_packge_data {
    // 长包序号，回复时沿用请求中的序号
    uint16_t package_number;
    // 报文总长度，由载荷长度加 15 字节头尾计算
    uint16_t package_length;
    // 长包头部 CRC8
    uint8_t crc8;
    // 目标设备地址
    uint16_t target_address;
    // 源设备地址
    uint16_t source_address;
    // 长包类型，如 0x211/0x21A/0x218
    uint16_t type;
    // 指向载荷起始位置
    uint8_t *datas;
    // 载荷字节数
    uint16_t data_length;
} __attribute__((packed));

/**
 * 组装并发送长包
 * @param data 长包字段与载荷描述
 * 写入帧头、长度和 11 字节头字段，拼接载荷后统一计算 CRC 并发送。
 */
static void BambuBus_long_package_send(long_packge_data *data) {
    packge_send_buf[0] = 0x3D;
    packge_send_buf[1] = 0x00;
    data->package_length = data->data_length + 15;
    memcpy(packge_send_buf + 2, data, 11);
    memcpy(packge_send_buf + 13, data->datas, data->data_length);
    package_send_with_crc(packge_send_buf, data->data_length + 15);
}

/**
 * 解析长包头
 * @param buf 完整长包
 * @param data_length 报文长度
 * @param data 输出的长包描述
 * 复制 11 字节头字段，并把 datas 指向载荷起始位置。
 */
static void BambuBus_long_package_analysis(uint8_t *buf, int data_length, long_packge_data *data) {
    if (!buf || !data || data_length < 15 || data_length > BUS_FRAME_CAPACITY) {
        if (data) memset(data, 0, sizeof(*data));
        return;
    }

    memcpy(data, buf + 2, 11);
    data->datas = buf + 13;
    data->data_length = data_length - 15;
}

// 最近一次解析出的打印机长包描述，供各长包响应函数复用
static long_packge_data printer_data_long;

// ==================== 动态压力值计算 ====================
/**
 * 计算通道压力反馈值
 * @param ams_num AMS 编号
 * @param ch AMS 内通道编号
 * @return 压力值；无效通道返回 0xFFFF
 * 根据内部运动状态映射为打印机期望的压力反馈。
 */
static uint16_t get_pressure_value(const uint8_t ams_num, uint8_t ch) {
    if (ams_num >= AMS_COUNT || ch >= 4) return 0xFFFF;

    const filament_motion_internal m = filament_ext[ams_num][ch].motion;
    const filament_motion_internal prev_motion = m;

    switch (m) {
        case filament_motion_internal::send_out:
            return 0x4700;
        case filament_motion_internal::before_on_use:
            return (prev_motion == filament_motion_internal::send_out) ? 0x4700 : 0x2B00;
        case filament_motion_internal::on_use:
            return 0x2B00;
        case filament_motion_internal::stop_on_use:
            return (prev_motion == filament_motion_internal::send_out) ? 0x4700 : 0x2B00;
        case filament_motion_internal::before_pull_back:
            return 0x2B00;
        case filament_motion_internal::pull_back:
            return 0x4700;
        default:
            return 0xFFFF;
    }
}

/**
 * 计算 AMS 耗材使用标志
 * @param ams_num AMS 编号
 * @return 0x00、0x02 或 0x04
 * 有通道运动时返回送出/回卷标志，否则返回空闲。
 */
static uint8_t get_filament_use_flag(uint8_t ams_num) {
    if (ams_num >= AMS_COUNT) return 0x00;

    for (int i = 0; i < 4; i++) {
        filament_motion_internal m = filament_ext[ams_num][i].motion;
        if (m != filament_motion_internal::idle) {
            if (m == filament_motion_internal::send_out ||
                m == filament_motion_internal::pull_back)
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
/**
 * 识别 BambuBus 报文类型
 * @param buf 完整报文
 * @param length 报文长度
 * @return package_type 枚举值；校验失败返回 ERROR
 * 校验 CRC16、短包长度或长包长度，再按命令字节和长包 type 分类。
 */
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
        BambuBus_long_package_analysis(buf, length, &printer_data_long);

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

// 短包响应序号：占用响应头 bit3..5，每回复一帧在 0..7 间循环
static uint8_t package_num = 0;

/**
 * 生成耗材在线状态位图
 * @param AMS_num AMS 编号
 * @return 每通道 2 bit 的状态位图；无效 AMS 返回 0
 * bit0 表示在线，bit1 表示通道正在运动，供打印机显示耗材状态。
 */
uint8_t get_filament_left_char(const uint8_t AMS_num) {
    if (AMS_num >= 4) return 0;
    uint8_t data = 0;
    for (int i = 0; i < 4; i++) {
        if (data_save.filament[AMS_num][i].statu == online) {
            data |= (1 << i) << i;
            if (filament_ext[AMS_num][i].motion != filament_motion_internal::idle &&
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

/**
 * 短包运动请求（Cxx）的固定响应模板
 * send_for_Cxx() 先复制请求中的通道和状态，再按 5 字节头 + C_test 载荷组装回应。
 */
unsigned char Cxx_res[] = {
    0x3D, 0xE0, 0x2C, 0x1A, 0x03,
    C_test 0x00, 0x00, 0x00, 0x00,
    0x90, 0xE4
};

// ==================== 辅助函数：字节序转换 ====================
/**
 * 填充短包运动响应载荷
 * @param set_buf 响应载荷缓冲区
 * @param buf 收到的请求报文
 * @param length 请求长度
 * @return true 表示参数有效并已填充
 * 写入使用标志、通道号、剩余长度、压力值和在线位图。
 */
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

/**
 * 回复短包运动请求
 * @param buf 收到的请求报文
 * @param length 请求长度
 * 设置包序号和响应载荷后计算 CRC 并发送，包序号在 0-7 间循环。
 */
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


/**
 * 长包运动请求（Dxx）的固定响应模板
 * send_for_Dxx() 每帧改写 AMS、通道、在线位图、长度、压力和温度/湿度等字段。
 */
static unsigned char Dxx_res[] = {
    0x3D, 0xE0, 0x3C, 0x1A, 0x04,
    0x00, 0x75, 0x01, 0x11,  // [6..7]=温度 0.1℃ 小端（默认 37.3℃），[8]=湿度 %（默认 17%）
    0x04, 0x04, 0x04, 0xFF,
    0x00, 0x00, 0x00, 0x00,
    C_test 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF,
    0x90, 0xE4
};

// 0x06 握手锁定的 AMS 编号；0xFF 表示尚未锁定，之后重复握手沿用首个编号
static uint8_t res_for_06_num = 0xFF;
// 兼容旧在线检测流程的剩余响应次数；当前代码保留判断但未写入非零值
static int last_detect = 0;
// 旧在线检测流程使用的耗材检测位图；当前由 Dxx 在线位图字段直接计算
static uint8_t filament_flag_detected = 0;

/**
 * 回复长包运动请求
 * @param buf 收到的请求报文
 * @param length 请求长度
 * 解析 AMS/通道/状态标志，驱动状态机并生成包含在线、NFC、长度和压力的 Dxx 响应。
 */
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

    // 每个 AMS 上一次处理 Dxx 的毫秒时间，用于估算运动状态机的时间增量
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
        if (filament_ext[AMS_num][now_ch].motion == filament_motion_internal::before_pull_back) {
            set_motion_internal(AMS_num, now_ch, filament_motion_internal::pull_back);
        }
    } else if (read_num == 0xFF && statu_flags == 0x01) {
        const uint8_t now_ch = data_save.BambuBus_now_filament_num % 4;
        if (now_ch < 4) {
            // 卸载当前通道，设置为idle状态
            set_motion_internal(AMS_num, now_ch, filament_motion_internal::idle);
            time_sendout_onuse_ticks[now_ch] = 0;
        }
    } else if (read_num == 0xFF) {
        if (data_save.BambuBus_now_filament_num < 16) {
            const uint8_t now_ch = data_save.BambuBus_now_filament_num % 4;
            filament_motion_internal m = filament_ext[AMS_num][now_ch].motion;
            if (m == filament_motion_internal::on_use ||
                m == filament_motion_internal::before_on_use ||
                m == filament_motion_internal::stop_on_use) {
                return;
            }
        }

        // 重置所有通道状态
        for (uint8_t i = 0; i < 4; i++) {
            set_motion_internal(AMS_num, i, filament_motion_internal::idle);
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

    // AHT20 有效时把温湿度写入 AMS 长包回复；无效或未检测时恢复固定默认值，
    // 避免把上一次的旧读数继续发给打印机。
    Dxx_res[6] = 0x75;
    Dxx_res[7] = 0x01;
    Dxx_res[8] = 0x11;
    float aht_temperature = 0.0f;
    float aht_humidity = 0.0f;
    if (env_sensor_get_aht20(&aht_temperature, &aht_humidity)) {
        if (aht_temperature < -40.0f) aht_temperature = -40.0f;
        if (aht_temperature > 125.0f) aht_temperature = 125.0f;
        if (aht_humidity < 0.0f) aht_humidity = 0.0f;
        if (aht_humidity > 100.0f) aht_humidity = 100.0f;

        const int32_t temperature_tenths = static_cast<int32_t>(
            aht_temperature >= 0.0f ? aht_temperature * 10.0f + 0.5f
                                    : aht_temperature * 10.0f - 0.5f);
        const uint16_t temperature_raw = static_cast<uint16_t>(static_cast<int16_t>(temperature_tenths));
        const int32_t humidity_percent = static_cast<int32_t>(aht_humidity + 0.5f);
        Dxx_res[6] = static_cast<unsigned char>(temperature_raw & 0xFF);
        Dxx_res[7] = static_cast<unsigned char>(temperature_raw >> 8);
        Dxx_res[8] = static_cast<unsigned char>(humidity_percent);
    }

    package_send_with_crc(Dxx_res, sizeof(Dxx_res));
    if (package_num < 7)
        package_num++;
    else
        package_num = 0;
}

// ==================== 在线检测（新协议完整注册机制）====================
/**
 * 在线检测/注册回复模板（每个 AMS 一份）
 * send_for_Fxx() 按 AMS 编号改写包序号和 AMS 地址后发送 29 字节 F00 响应。
 */
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

/**
 * 等待 5ms 总线空闲间隔
 * 忙等约 5ms，确保连续响应帧之间留有间隔。
 */
static void bus_wait_idle_5ms() {
    uint32_t t0 = get_ticks32();
    while (get_ticks32() - t0 < 5) {
        __asm__ volatile ("nop");
    }
}

/**
 * 回复在线检测与注册请求
 * @param buf 收到的请求报文
 * @param length 请求长度
 * 处理广播注册、指定 AMS 查询和 0x06 握手，维护已注册状态并回复 F00/F06 报文。
 */
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
// MC 在线长包载荷模板：首字节写入 AMS 编号，其余字节作为状态占位
unsigned char long_packge_MC_online[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

/**
 * 回复 MC 在线长包
 * @param buf 收到的长包
 * @param length 报文长度
 * 校验 AMS 编号后复制包序号和地址，回复 MC 在线状态。
 */
void send_for_long_packge_MC_online(unsigned char *buf, int length) {
    long_packge_data data{};
    BambuBus_long_package_analysis(buf, length, &printer_data_long);

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
    BambuBus_long_package_send(&data);
}

// ==================== 读取耗材信息 ====================
// 耗材信息长包载荷模板；发送前按请求填入通道、ID、名称、颜色和温度范围
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

/**
 * 回复耗材信息长包
 * @param buf 收到的长包
 * @param length 报文长度
 * 读取指定通道的 ID、名称、颜色和温度范围，组装长包回复打印机。
 */
void send_for_long_packge_filament(unsigned char *buf, int length) {
    long_packge_data data{};
    BambuBus_long_package_analysis(buf, length, &printer_data_long);

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
    BambuBus_long_package_send(&data);
}

// ==================== 修复点1：版本和序列号数组定义 ====================

// AMS lite 版本响应中的序列号模板；最后字节在回复时替换为 AMS 编号
static unsigned char long_packge_version_serial_number_AMS_lite[] = {
    0x0F, // 序列号长度15
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, // 15字节SN
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 17字节填充
    0x0E, 0x7D, // 2字节标识
    0x32, 0x31, 0x31, 0x38, 0x15, 0x00, 0x36, 0x39, 0x37, 0x33, 0xFF, 0xFF, 0xFF, 0xFF, // 14字节额外数据
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xBB, 0x44, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00 // 最后字节，将被替换为AMS序号
};

// AMS 08 版本响应中的序列号模板；最后字节在回复时替换为 AMS 编号
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
// AMS lite 版本响应中的版本号和型号名模板（"AMS_F102"）
static unsigned char long_packge_version_version_and_name_AMS_lite[] = {
    0x00, 0x00, 0x00, 0x3C, // 版本号60
    0x41, 0x4D, 0x53, 0x5F, 0x46, 0x31, 0x30, 0x32, 0x00, 0x00, 0x00, 0x00, 0x00, // "AMS_F102"
    0x00, 0x00, 0x00, 0x00 // 填充 + AMS序号位置
};

// AMS 08 版本响应中的版本号和型号名模板（"AMS08"）
static unsigned char long_packge_version_version_and_name_AMS08[] = {
    0x00, 0x00, 0x00, 0x5A, // 版本号90
    0x41, 0x4D, 0x53, 0x30, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // "AMS08"
    0x00, 0x00, 0x00, 0x00 // 填充 + AMS序号位置
};

/**
 * 生成稳定的设备序列号
 * 把芯片唯一 ID 格式化为 15 字节序列号，并填充两套 AMS 序列号缓冲区。
 */
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

/**
 * 回复序列号或版本查询
 * @param buf 收到的长包
 * @param length 报文长度
 * type=0x402 回复序列号，type=0x103 回复版本和名称；支持网页自定义序列号。
 */
void send_for_long_packge_version(unsigned char *buf, int length) {
    long_packge_data data{};
    BambuBus_long_package_analysis(buf, length, &printer_data_long);

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
    BambuBus_long_package_send(&data);
}

// ==================== 设置耗材信息（短包 0xC5 0x08）====================
// 短包设置耗材（0x08）的固定 ACK 响应，发送时再补 CRC8
unsigned char Set_filament_res[] = {0x3D, 0xC0, 0x08, 0xB2, 0x08, 0x60, 0xB4, 0x04};

/**
 * 处理短包设置耗材请求
 * @param buf 收到的请求报文
 * @param length 请求长度
 * 解析通道、ID、颜色、温度范围和名称，回复 ACK 并标记记录待保存。
 */
static void send_for_Set_filament(const unsigned char *buf, const int length) {
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
    BambuBus_set_need_to_save();
}

// ==================== 新增：设置耗材信息类型2（长包 0x05 0x218）====================
// 长包设置耗材类型 2 的三字节确认载荷：[AMS 编号, 通道号, 0x00]
unsigned char set_filament_res_type2[] = {0x00, 0x00, 0x00};

/**
 * 处理长包设置耗材请求
 * @param buf 收到的长包
 * @param length 报文长度
 * 按长包偏移解析耗材信息，回复三字节确认并标记记录待保存。
 */
void send_for_Set_filament_type2(unsigned char *buf, int length) {
    long_packge_data data{};
    BambuBus_long_package_analysis(buf, length, &printer_data_long);

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
    BambuBus_long_package_send(&data);

    BambuBus_set_need_to_save();
}

/**
 * 读取最近一次报文类型
 * @return 最近处理的 package_type
 * 供网页或调试代码查询协议状态。
 */
int get_cmd_type() {
    return cmd;
}

/**
 * 读取最近操作的通道号
 * @return 最近操作的通道号；未知时为 255
 * 记录 Cxx/Dxx/设置耗材等请求中的通道。
 */
unsigned char get_now_op_num() {
    return now_op_num;
}

// ==================== 主循环 ====================
// 最近一次收到合法 BambuBus 帧的毫秒时间
static uint32_t last_bus_packet_ms = 0;
// 是否已经收到过合法总线帧；false 时不计算通信超时
static bool bus_packet_seen = false;
// 最近一次收到心跳报文的毫秒时间
static uint32_t last_heartbeat_ms = 0;
// 是否已经收到过心跳；false 时打印机连接状态返回 0
static bool heartbeat_seen = false;

/**
 * 获取打印机连接状态
 * @return 0=尚未收到心跳，1=最近 5 秒有心跳，2=心跳超时
 * 仅表示通信连接状态，不推断打印或故障状态。
 */
int BambuBus_printer_status() {
    // 容忍偶发丢帧；仅表示连接状态，不推断打印/暂停/故障。
    return !heartbeat_seen ? 0 : millis() - last_heartbeat_ms < 5000 ? 1 : 2;
}

/**
 * 读取接收错误帧总数
 * @return 错误帧累计数量
 * 返回 CRC 或格式错误导致的丢帧计数。
 */
uint32_t BambuBus_rx_error_count() {
    return rx_error_load();
}

/**
 * 读取接收丢帧总数
 * @return 丢帧累计数量
 * 返回完整帧因双缓冲已满而丢弃的计数。
 */
uint32_t BambuBus_rx_dropped_count() {
    return rx_dropped_load();
}

/**
 * 读取距最近合法报文的时间
 * @return 毫秒数；从未收到合法报文时返回 UINT32_MAX
 * 供网页显示总线新鲜度。
 */
uint32_t BambuBus_last_valid_packet_age_ms() {
    if (!bus_packet_seen) return UINT32_MAX;
    return millis() - last_bus_packet_ms;
}

/**
 * 判断总线最近是否有活动
 * @param quiet_ms 静默时间窗口（毫秒）
 * @return true 表示有待处理帧或窗口内收到过数据
 * 同时检查双缓冲、原始接收时间和合法报文时间。
 */
bool BambuBus_recent_activity(const uint32_t quiet_ms) {
    uint32_t now = millis();
    return completed_slot_load() >= 0 ||
           BambuBus_have_data > 0 ||
           (bus_rx_seen && now - last_bus_rx_ms < quiet_ms) ||
           (bus_packet_seen && now - last_bus_packet_ms < quiet_ms);
}

/**
 * 获取报文类型名称
 * @param type package_type 枚举值
 * @return 用于日志显示的名称
 * 把报文类型转换为中文/英文可读文本。
 */
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

/**
 * BambuBus 主处理循环
 * @return 0=本轮无帧，1=处理了合法帧，2=收到错误帧，-1=合法通信超时
 * 取出双缓冲中的完整帧、识别类型并分发响应，同时更新心跳/超时状态并限频保存耗材记录。
 */
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
            log_bus_hex_typed(LOG_CH_BUS, 'R', cmd, frame, static_cast<uint16_t>(data_length));
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

    // 下一次允许尝试保存耗材记录的毫秒时间，失败后至少间隔 1 秒再重试
    static uint32_t next_filament_save_ms = 0;
    const uint32_t save_now = millis();
    if (BambuBus_need_to_save && static_cast<int32_t>(save_now - next_filament_save_ms) >= 0) {
        if (BambuBus_save()) {
            BambuBus_need_to_save = false;
        } else {
            LOG_MAIN("[storage] 耗材记录写入或校验失败，将稍后重试");
        }
        next_filament_save_ms = save_now + 1000u;
    }

    return stu;
}
