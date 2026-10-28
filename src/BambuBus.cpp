// 包含：完整状态机、序列号响应、长包设置耗材、动态压力等
#include "BambuBus.h"
#include "crc_bus.h"
#include "settings_storage.h"  // 用于访问AMS_COUNT设置
#include "log_bus.h"
#include "env_sensor.h"
#include "bambu_motion_reference.h"

// ==================== 接收状态与共享计数 ====================
// 单帧最大容量：接收组帧、接收队列和发送缓冲区共用该上限
static constexpr uint16_t BUS_FRAME_CAPACITY = 1000u;
/**
 * BambuBus 接收组帧工作缓冲区
 * RX_IRQ 从帧头 0x3D 开始逐字节写入，用于拼接当前正在接收的报文；
 *       只有长度合法且 CRC8 校验通过的完整帧才会复制到完成帧接收队列
 */
static uint8_t BambuBus_data_buf[BUS_FRAME_CAPACITY];
// 接收事件与主循环共享的活动快照，统一通过平台原子接口访问
static volatile uint32_t last_bus_rx_ms = 0;
static volatile uint32_t bus_rx_seen = 0;
// 回调开始/结束各递增一次，奇数表示忙；序号变化可识别快照期间完整发生过的回调
static volatile uint32_t rx_callback_seq = 0;
static volatile uint32_t rx_partial = 0;
// CRC 或长度错误累计次数
static volatile uint32_t rx_error_frames = 0;
// 接收残帧在静默超过此时间后失效，与 RX_IRQ 的组帧超时一致
static constexpr uint32_t BUS_RX_PARTIAL_TIMEOUT_MS = 100u;

/**
 * 递增接收错误计数
 * 使用平台原子操作，允许接收回调与主循环同时报告错误
 */
static void rx_error_increment() { hal_atomic_increment_u32(&rx_error_frames); }

/**
 * 读取接收错误计数
 * @return 错误帧累计数量
 * 使用平台原子操作读取接收回调与主循环共享的计数
 */
static uint32_t rx_error_load() { return hal_atomic_load_u32(&rx_error_frames); }

// 最近一次协议请求操作的通道号；255 表示未知
static unsigned char now_op_num = 255;
// 最近一次识别出的 package_type 枚举值
static int cmd = 0;

#define BambuBus_address_AMS08 ((uint16_t)0x700)
//#define BambuBus_address_AMSLite ((uint16_t)0x1200)
#define BambuBus_address_AMSLite ((uint16_t)0x1300)

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
    // 独立有效标记使零毫秒也能作为报文间隔的起点
    bool last_time_valid = false;
    // 是否正在统计送出到使用的计时窗口
    bool is_sendout_onuse_timing = false;
    // 送出到使用计时窗口的起始时间
    uint32_t sendout_onuse_start_ticks = 0;
    // 反馈独立于电机动作：送出等待使用时仍需报告 04/2B00
    uint16_t response_pressure = 0xFF74;
    uint8_t response_use_flag = 0;
    // 使用前准备历史与响应计数按 AMS/通道保存，不借用其他料槽的数据
    uint8_t last_prepare_flag = 0;
    uint8_t on_use_count = 0;
    uint8_t prepare_reply_index = 0;
};

// 每个 AMS 的 4 个通道对应的内部运动扩展状态
static _filament_ext filament_ext[4][4];
// 当前协商使用的 AMS 总线地址
static uint16_t bambubus_ams_address = 0x0000;
// 各 AMS 是否已完成在线检测注册
static bool have_registered[4] = {false, false, false, false};
// 发现阶段：0 为首次报名，1 为后续报名；注册确认固定使用 0x0A 前缀
static uint8_t online_detect_phase[4] = {};
// 注册和序列号查询共用每个 AMS 的身份，固定为协议的 66 字节载荷
static uint8_t serial_identity[4][66] = {};

static void refresh_serial_identities();

static bool registration_identity_matches(const uint8_t *buf, int length);

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
 * 复制记录并把 crc32 字段清零后计算 CRC32，避免校验值自引用
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
 * 从持久化存储读取记录，校验魔数、格式版本和 CRC32；无效时恢复默认值并标记待保存
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
 * 设置延迟保存标志，由主循环限频写入存储
 */
void BambuBus_set_need_to_save() {
    BambuBus_need_to_save = true;
}

/**
 * 保存耗材记录并回读校验
 * @return true 表示写入内容与回读一致
 * 更新格式版本和 CRC32 后写入存储，再回读比较整块数据
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
 * 返回打印机当前选中的全局通道号
 */
int get_now_filament_num() {
    return data_save.BambuBus_now_filament_num;
}

/**
 * 读取当前 AMS 编号
 * @return 0-3 的 AMS 编号；无当前通道时为 -1
 * 由当前通道号除以 4 得到
 */
int get_current_AMS_index() {
    return data_save.BambuBus_now_filament_num < 16
               ? data_save.BambuBus_now_filament_num / 4
               : -1;
}


/**
 * 清零指定通道的耗材长度
 * @param num 全局通道编号 0-15
 * 同时清零记录中的长度和内部虚拟计数
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
 * 把进料或退料距离累加到耗材记录
 */
void add_filament_meters(const int num, const float meters) {
    if (num < 0 || num >= 16) return;
    data_save.filament[num / 4][num % 4].meters += meters;
}

/**
 * 读取指定通道的耗材长度
 * @param num 全局通道编号 0-15
 * @return 累计长度；编号越界返回 0
 * 返回记录中保存的耗材长度
 */
float get_filament_meters(const int num) {
    if (num < 0 || num >= 16) return 0.0f;
    return data_save.filament[num / 4][num % 4].meters;
}

/**
 * 查询通道是否有料
 * @param num 全局通道编号 0-15
 * @return true 表示在线/有料
 * 读取通道状态是否为 online
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
 * 更新耗材记录中的 statu 字段，供协议响应和网页显示
 */
void set_filament_online(const int num, const int count, const bool if_online) {
    if (num < 0 || num >= 4 || count < 0 || count >= 4) return;
    data_save.filament[num][count].statu = if_online ? online : offline;
}

/**
 * 读取指定通道的耗材名称
 * @param num 全局通道编号 0-15
 * @return 耗材名称；编号越界返回空串
 * 把记录中的 20 字节名称转换为 String
 */
String get_now_filament_name(const int num) {
    if (num < 0 || num >= 16) return {};
    return {data_save.filament[num / 4][num % 4].name};
}

/**
 * 把内部运动状态转换为对外状态
 * @param m 内部状态机枚举
 * @return waiting、need_send_out 或 need_pull_back
 * 隐藏 before_on_use、on_use 等中间状态
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
 * 只用于日志，不改变协议状态
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
 * @param printer_request 是否由打印机请求更新，默认为 true
 * 同步状态与转换历史；离开进料时清除等待计时，进入 idle 时清除反馈和准备历史
 * 只有打印机请求的 idle 才通知 BMCU 解除阻断，内部停车必须保留阻断
 */
static void set_motion_internal(const uint8_t ams_num, const uint8_t ch,
                                const filament_motion_internal new_motion, const bool printer_request = true) {
    if (ams_num >= 4 || ch >= 4) return;
    auto &ext = filament_ext[ams_num][ch];
    const auto previous = ext.motion;
    if (previous != new_motion) {
        LOG_MAIN("[printer] 通道 %d %s（%s -> %s）", ams_num * 4 + ch + 1,
                 motion_internal_name(new_motion), motion_internal_name(previous),
                 motion_internal_name(new_motion));
    }
    if (new_motion != filament_motion_internal::send_out) ext.is_sendout_onuse_timing = false;
    if (new_motion != filament_motion_internal::before_on_use || previous != new_motion)
        ext.prepare_reply_index = 0;
    if (new_motion == filament_motion_internal::idle) {
        ext.last_time_valid = false;
        ext.response_pressure = 0xFF74;
        ext.response_use_flag = 0;
        ext.last_prepare_flag = 0;
        ext.on_use_count = 0;
        if (printer_request) BMCU_printer_idle(ams_num * 4 + ch);
    }
    ext.motion = new_motion;
    data_save.filament[ams_num][ch].motion_set = map_internal_to_api(new_motion);
}

/**
 * 读取通道对外运动状态
 * @param num 全局通道编号 0-15
 * @return 当前运动状态；越界返回 waiting
 * 供 BMCU 运动层查询打印机请求
 */
filament_motion_state_set get_filament_motion(const int num) {
    if (num < 0 || num >= 16) return waiting;
    return data_save.filament[num / 4][num % 4].motion_set;
}

/**
 * 查询打印机是否明确要求停止当前供料
 * @param num 全局通道编号
 * @return true 表示已收到该通道的 07/00 停止请求
 * 仅提供状态，主循环据此跳过普通进料结束延迟，不在协议处理器中驱动电机
 */
bool BambuBus_filament_stop_requested(const int num) {
    return num >= 0 && num < active_channel_count() && num < 16 &&
           filament_ext[num / 4][num % 4].motion == filament_motion_internal::stop_on_use;
}

/**
 * 设置通道对外运动状态
 * @param num 全局通道编号 0-15
 * @param motion 新的对外状态
 * 同步状态、历史和计时，但不解除打印机阻断；保留上层传入的对外命令
 */
void set_filament_motion(const int num, const filament_motion_state_set motion) {
    if (num < 0 || num >= 16) return;
    const uint8_t ams = num / 4;
    const uint8_t ch = num % 4;
    switch (motion) {
        case need_send_out:
            set_motion_internal(ams, ch, filament_motion_internal::send_out, false);
            filament_ext[ams][ch].response_use_flag = 0x02;
            filament_ext[ams][ch].response_pressure = 0x4700;
            break;
        case need_pull_back:
            set_motion_internal(ams, ch, filament_motion_internal::pull_back, false);
            filament_ext[ams][ch].response_use_flag = 0x02;
            filament_ext[ams][ch].response_pressure = 0x4700;
            break;
        default:
            set_motion_internal(ams, ch, filament_motion_internal::idle, false);
            break;
    }
    data_save.filament[ams][ch].motion_set = motion;
}

/**
 * 读取耗材透明度通道
 * @param num 全局通道编号 0-15
 * @return A 通道值；越界返回 0
 * 返回 RGBA 颜色中的 A 分量
 */
uint8_t get_filament_color_A(const int num) {
    if (num < 0 || num >= 16) return 0;
    return data_save.filament[num / 4][num % 4].color_A;
}

/**
 * 读取耗材红色通道
 * @param num 全局通道编号 0-15
 * @return R 通道值；越界返回 0
 * 返回 RGBA 颜色中的 R 分量
 */
uint8_t get_filament_color_R(const int num) {
    if (num < 0 || num >= 16) return 0;
    return data_save.filament[num / 4][num % 4].color_R;
}

/**
 * 读取耗材绿色通道
 * @param num 全局通道编号 0-15
 * @return G 通道值；越界返回 0
 * 返回 RGBA 颜色中的 G 分量
 */
uint8_t get_filament_color_G(const int num) {
    if (num < 0 || num >= 16) return 0;
    return data_save.filament[num / 4][num % 4].color_G;
}

/**
 * 读取耗材蓝色通道
 * @param num 全局通道编号 0-15
 * @return B 通道值；越界返回 0
 * 返回 RGBA 颜色中的 B 分量
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
 * 判断打印机当前耗材是否位于指定 AMS 内
 */
static uint8_t get_loaded_filament(const uint8_t ams_num) {
    const int now = data_save.BambuBus_now_filament_num;
    if (now >= ams_num * 4 && now < (ams_num + 1) * 4)
        return now % 4;
    return 0xFF;
}

/**
 * 处理 Cxx/Dxx 共用的运动状态机
 * @param ams_num AMS 编号
 * @param filament_channel AMS 内通道编号，0xFF 表示无指定通道
 * @param statu_flags 状态标志
 * @param motion_flag 运动标志
 * @return true 表示请求范围有效，可继续生成回复
 * 计时和历史按 AMS、通道隔离；无通道请求只操作本 AMS，未知组合保持原状态
 * 对齐 BMCU-U 的 09/7F、09/A5 和 15 秒送出等待；动作与反馈分开，零毫秒与回绕均有效
 */
static bool process_motion_state_machine(
    const uint8_t ams_num,
    const uint8_t filament_channel,
    const uint8_t statu_flags,
    const uint8_t motion_flag) {
    if (ams_num >= AMS_COUNT || ams_num >= 4 ||
        (filament_channel >= 4 && filament_channel != 0xFF))
        return false;

    const uint8_t loaded = get_loaded_filament(ams_num);
    if (filament_channel == 0xFF) {
        if (statu_flags == 0x03 && motion_flag == 0x00) {
            if (loaded < 4) {
                auto &ext = filament_ext[ams_num][loaded];
                if (ext.motion == filament_motion_internal::before_pull_back ||
                    ext.motion == filament_motion_internal::on_use ||
                    ext.motion == filament_motion_internal::before_on_use ||
                    ext.motion == filament_motion_internal::stop_on_use) {
                    set_motion_internal(ams_num, loaded, filament_motion_internal::pull_back);
                    ext.response_use_flag = 0x02;
                    ext.response_pressure = 0x4700;
                }
            }
        } else if (statu_flags == 0x01) {
            // FF/01 是无指定通道轮询，不得结束打印或正在执行的回抽；本地动作完成后才释放选择
            if (loaded < 4 && filament_ext[ams_num][loaded].motion == filament_motion_internal::idle) {
                set_motion_internal(ams_num, loaded, filament_motion_internal::idle);
                data_save.BambuBus_now_filament_num = 0xFF;
            }
        } else {
            if (loaded < 4) {
                const auto motion = filament_ext[ams_num][loaded].motion;
                if (motion == filament_motion_internal::on_use ||
                    motion == filament_motion_internal::before_on_use ||
                    motion == filament_motion_internal::stop_on_use)
                    return true;
            }
            for (uint8_t ch = 0; ch < 4; ++ch)
                set_motion_internal(ams_num, ch, filament_motion_internal::idle);
            // 打印机全局选中的通道只能由其所属 AMS 清除
            if (loaded < 4) data_save.BambuBus_now_filament_num = 0xFF;
        }
        return true;
    }

    auto &ext = filament_ext[ams_num][filament_channel];
    auto &fil = data_save.filament[ams_num][filament_channel];
    const uint32_t now = millis();
    const uint32_t dt = ext.last_time_valid ? now - ext.last_time_ticks : 0u;
    const uint32_t time_used_ms = dt > 1000u ? 0u : dt;
    ext.last_time_ticks = now;
    ext.last_time_valid = true;

    const bool allow_any = loaded == 0xFF || loaded == filament_channel;
    const bool allow_stop = loaded == filament_channel;
    const bool is_send_out = statu_flags == 0x03 && motion_flag == 0x00;
    const bool is_before_on_use = statu_flags == 0x09 && (motion_flag == 0xA5 || motion_flag == 0x7F);
    const bool is_stop_on_use = statu_flags == 0x07 && motion_flag == 0x00;
    const bool is_on_use = statu_flags == 0x07 && motion_flag == 0x7F;
    const bool is_before_pullb = statu_flags == 0x09 && motion_flag == 0x3F;

    const bool accept = is_send_out ||
                        (is_before_on_use && allow_any) ||
                        (is_on_use && allow_any) ||
                        (is_stop_on_use && allow_stop) ||
                        (is_before_pullb && allow_stop);
    if (accept && data_save.BambuBus_now_filament_num != ams_num * 4 + filament_channel) {
        const int previous = data_save.BambuBus_now_filament_num;
        if (previous >= 0 && previous < active_channel_count())
            set_motion_internal(previous / 4, previous % 4, filament_motion_internal::idle);
        // 打印头只使用一个全局通道，真正切换时才释放上一个通道
        data_save.BambuBus_now_filament_num = ams_num * 4 + filament_channel;
    }

    if (is_send_out) {
        ext.is_sendout_onuse_timing = false;
        ext.last_prepare_flag = 0;
        ext.on_use_count = 0;
        if (ext.motion != filament_motion_internal::send_out && loaded != 0xFF) {
            for (uint8_t ch = 0; ch < 4; ++ch)
                if (ch != filament_channel) set_motion_internal(ams_num, ch, filament_motion_internal::idle);
        }
        set_motion_internal(ams_num, filament_channel, filament_motion_internal::send_out);
        ext.response_use_flag = 0x02;
        ext.response_pressure = 0x4700;
    } else if (is_before_on_use && allow_any) {
        const auto previous = ext.motion;
        if (ext.last_prepare_flag != motion_flag) ext.prepare_reply_index = 0;
        set_motion_internal(ams_num, filament_channel, filament_motion_internal::before_on_use);
        ext.last_prepare_flag = motion_flag;
        ext.on_use_count = 0;
        ext.response_use_flag = 0x04;
        ext.response_pressure = motion_flag == 0x7F ? 0x1E34 :
            previous == filament_motion_internal::send_out ? 0x4700 : 0x2B00;
    } else if (is_on_use && allow_any) {
        if (ext.motion == filament_motion_internal::send_out) {
            if (!ext.is_sendout_onuse_timing) {
                ext.sendout_onuse_start_ticks = now;
                ext.is_sendout_onuse_timing = true;
            }
            if (uint32_t(now - ext.sendout_onuse_start_ticks) < 15000u) {
                ext.response_use_flag = 0x04;
                ext.response_pressure = 0x2B00;
                return true;
            }
        }
        set_motion_internal(ams_num, filament_channel, filament_motion_internal::on_use);
        ext.response_use_flag = 0x04;
        ext.response_pressure = 0x2B00;
        if (ext.last_prepare_flag == 0x7F && ext.on_use_count < 5) {
            ++ext.on_use_count;
            ext.response_pressure = 0x1E34;
        }
        if (ext.meters_virtual_count < 10000u) {
            const uint32_t remaining = 10000u - ext.meters_virtual_count;
            const uint32_t elapsed = time_used_ms < remaining ? time_used_ms : remaining;
            fil.meters += static_cast<float>(elapsed) / 300000.0f;
            ext.meters_virtual_count += elapsed;
        }
    } else if (is_stop_on_use && allow_stop) {
        ext.is_sendout_onuse_timing = false;
        const auto previous = ext.motion;
        if (previous == filament_motion_internal::on_use || previous == filament_motion_internal::before_on_use ||
            previous == filament_motion_internal::send_out)
            set_motion_internal(ams_num, filament_channel, filament_motion_internal::stop_on_use);
        ext.response_use_flag = 0x04;
        ext.response_pressure = previous == filament_motion_internal::before_on_use &&
            ext.last_prepare_flag == 0x7F ? 0x1E34 : 0x2B00;
    } else if (is_before_pullb && allow_stop) {
        ext.is_sendout_onuse_timing = false;
        if (ext.motion == filament_motion_internal::on_use ||
            ext.motion == filament_motion_internal::before_on_use ||
            ext.motion == filament_motion_internal::stop_on_use)
            set_motion_internal(ams_num, filament_channel, filament_motion_internal::before_pull_back);
        ext.response_use_flag = 0x04;
        ext.response_pressure = 0x2B00;
    } else if (statu_flags == 0x09) {
        ext.is_sendout_onuse_timing = false;
    }
    return true;
}

// ==================== UART 收发与完成帧队列 ====================
// 唯一发送槽，短包复制到此处，长包直接在此组帧；仅由主循环使用
static uint8_t packge_send_buf[BUS_FRAME_CAPACITY];
// 发送槽是否空闲；同步发送和 TX 日志读取期间均保持 false
static bool TX_IDLE = true;
// 当前请求预占发送槽，直到全部回复及日志完成；防止重入时消费下一帧
static bool bus_request_active = false;
// 发送完成时间参与后台保存的静默判断
static uint32_t last_bus_tx_ms = 0;
static bool bus_tx_seen = false;
// send_uart() 是同步发送的；处理一条 RX 报文时，响应 TX 继承当前 RX 的类型
// 没有对应 RX 来源的主动发送保持 -1，网页归入“未知”
// 当前 TX 报文关联的 RX 类型，供日志筛选；-1 表示主动发送/未知
static int bus_tx_context_type = -1;

/**
 * 通过 BambuBus 串口发送数据
 * @param data 待发送数据
 * @param length 数据长度
 * 先占用固定发送槽并复制报文、保存日志类型，再同步发送；TX 日志完成后才释放缓冲
 * 主循环预占请求后调用，忙状态或超出缓冲容量时拒绝写入，避免覆盖在途帧
 */
void send_uart(const unsigned char *data, const uint16_t length) {
    if (!TX_IDLE || !data || !length || length > sizeof(packge_send_buf)) return;
    TX_IDLE = false;
    if (data != packge_send_buf) memmove(packge_send_buf, data, length);
    const int context_type = bus_tx_context_type;
    // RS485_MODE_RXTX（只用 RXD/TXD）时模块自行控制收发方向，不驱动 DE
    const bool drive_de = (RS485_MODE == RS485_MODE_DE);

    if (drive_de) digitalWrite(BUS_DE, 1);
    hal_bus_write(packge_send_buf, length);
    if (drive_de) digitalWrite(BUS_DE, 0);
    last_bus_tx_ms = millis();
    bus_tx_seen = true;
    if (BUS_LOG_ENABLE) log_bus_hex_typed(LOG_CH_BUS, 'T', context_type, packge_send_buf, length);
    TX_IDLE = true;
}

// 单生产者（UART 回调）、单消费者（主循环）的固定队列
// 沿用两块完成帧缓冲；包含正在处理的帧在内最多容纳两帧，不分配堆内存
static constexpr uint32_t BUS_RX_QUEUE_CAPACITY = 2u;
static uint8_t completed_buf[BUS_RX_QUEUE_CAPACITY][BUS_FRAME_CAPACITY];
static uint16_t completed_len[BUS_RX_QUEUE_CAPACITY] = {};
static volatile uint32_t rx_write_seq = 0;
static volatile uint32_t rx_read_seq = 0;
static volatile uint32_t rx_dropped_frames = 0;

/**
 * 读取接收丢帧计数
 * @return 队列满时丢弃的新帧累计数量
 * 使用平台原子读取；已排队及正在处理的帧不会被覆盖
 */
static uint32_t rx_dropped_load() { return hal_atomic_load_u32(&rx_dropped_frames); }

/**
 * 将完整帧发布到接收队列
 * @param frame 已通过头部校验的完整帧
 * @param length 帧长度
 * 仅由 UART 接收回调调用；先复制内容再发布写序号，队列满则丢弃新帧并计数
 */
static void completed_frame_publish(const uint8_t *frame, uint16_t length) {
    const uint32_t write = hal_atomic_load_u32(&rx_write_seq);
    const uint32_t read = hal_atomic_load_u32(&rx_read_seq);
    if (write - read >= BUS_RX_QUEUE_CAPACITY) {
        hal_atomic_increment_u32(&rx_dropped_frames);
        return;
    }
    const uint32_t slot = write % BUS_RX_QUEUE_CAPACITY;
    memcpy(completed_buf[slot], frame, length);
    completed_len[slot] = length;
    hal_atomic_store_u32(&rx_write_seq, write + 1u);
}

/**
 * 查看队首完整帧
 * @return 缓冲槽编号；队列为空返回 -1
 * 仅由主循环调用；读取不会释放槽位，内容一直保持到 completed_frame_release()
 */
static int completed_frame_peek() {
    const uint32_t read = hal_atomic_load_u32(&rx_read_seq);
    if (read == hal_atomic_load_u32(&rx_write_seq)) return -1;
    return static_cast<int>(read % BUS_RX_QUEUE_CAPACITY);
}

/**
 * 释放已处理的队首帧
 * 仅由主循环在解析、回复和日志读取全部结束后调用，允许生产者重用该槽位
 */
static void completed_frame_release() {
    const uint32_t read = hal_atomic_load_u32(&rx_read_seq);
    hal_atomic_store_u32(&rx_read_seq, read + 1u);
}

/**
 * 逐字节解析 BambuBus 接收帧
 * @param _RX_IRQ_data 串口收到的单个字节
 * @return true 表示仍在组帧，false 表示完整帧已发布或残帧已丢弃
 * 从帧头 0x3D 开始组帧，解析短包/长包长度；收到 CRC8 字节时对已缓存的头部查表校验，
 * 完整帧写入接收队列供主循环处理
 */
static bool RX_IRQ(unsigned char _RX_IRQ_data) {
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
    const unsigned char data = _RX_IRQ_data;

    const uint32_t now_ms = millis();
    if (_index != 0 && now_ms - last_byte_ms > BUS_RX_PARTIAL_TIMEOUT_MS)
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
            return false;
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
                return false;
            }
        }

        if (_index == data_CRC8_index) {
            if (data != bus_crc8(BambuBus_data_buf, data_CRC8_index)) {
                if (BUS_LOG_ENABLE)
                    log_bus_hex_typed(LOG_CH_BUS, 'R', -1, BambuBus_data_buf,
                                      static_cast<uint16_t>(_index) + 1u);
                rx_error_increment();
                _index = 0;
                return false;
            }
        }
        if (length != 0 && static_cast<uint16_t>(_index) + 1u >= length) {
            completed_frame_publish(BambuBus_data_buf, length);
            _index = 0;
            length = 0;
            return false;
        }

        ++_index;
        if (_index >= BUS_FRAME_CAPACITY) {
            _index = 0;
            length = 0;
        }
    }
    return _index != 0;
}

/**
 * 处理串口接收事件
 * 读空 UART 接收缓冲并逐字节送入 RX_IRQ；原子发布回调序号、残帧和最后收字节时间
 */
static void on_uart_rx() {
    hal_atomic_increment_u32(&rx_callback_seq);
    while (hal_bus_available()) {
        const int value = hal_bus_read();
        if (value < 0) break;
        hal_atomic_store_u32(&last_bus_rx_ms, millis());
        hal_atomic_store_u32(&bus_rx_seen, 1u);
        const bool partial = RX_IRQ(static_cast<unsigned char>(value));
        hal_atomic_store_u32(&rx_partial, partial ? 1u : 0u);
    }
    hal_atomic_increment_u32(&rx_callback_seq);
}

/**
 * 初始化 BambuBus 串口与接收回调
 * @param baud 串口波特率
 * 调用平台层初始化串口，并注册接收中断或事件回调
 */
static void BambuBUS_UART_Init(const uint32_t baud) {
    hal_bus_begin(baud);
    BUS_IRQ_SETUP();
}

// ==================== 初始化 ====================

/**
 * 初始化 BambuBus 协议层
 * 加载耗材记录、复位运动与注册状态、生成各 AMS 共用身份，再以 1.25Mbps 启动接收回调；仅在启动时调用
 */
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
        have_registered[i] = false;
        online_detect_phase[i] = 0;
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

    refresh_serial_identities();
    BambuBUS_UART_Init(1250000);
}

// ==================== 包处理辅助函数 ====================
/**
 * 校验报文末尾的 CRC16
 * @param data 完整报文
 * @param data_length 报文长度
 * @return true 表示 CRC 正确
 * 对除末两字节外的数据查表计算 CRC16，并与报文中的小端校验值比较
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
 * 先检查发送槽、容量和声明长度，再填充 CRC8/CRC16，忙时不改写任何在途数据
 */
static void package_send_with_crc(uint8_t *data, int data_length) {
    if (!TX_IDLE || !data || data_length < 7 || data_length > BUS_FRAME_CAPACITY) return;
    const bool is_short = (data[1] & 0x80u) != 0u;
    if (!is_short && data_length < 15) return;
    const uint16_t declared = is_short
                                  ? data[2]
                                  : static_cast<uint16_t>(data[4]) | (static_cast<uint16_t>(data[5]) << 8);
    if (declared != data_length) return;
    if (is_short) {
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
 * 先检查发送槽、载荷容量与指针，再在共用发送缓冲拼接头部和载荷；统一计算 CRC
 */
static void BambuBus_long_package_send(long_packge_data *data) {
    if (!TX_IDLE || !data || data->data_length > BUS_FRAME_CAPACITY - 15u ||
        (data->data_length && !data->datas))
        return;
    packge_send_buf[0] = 0x3D;
    packge_send_buf[1] = 0x00;
    data->package_length = data->data_length + 15;
    memcpy(packge_send_buf + 2, data, 11);
    if (data->data_length) memcpy(packge_send_buf + 13, data->datas, data->data_length);
    package_send_with_crc(packge_send_buf, data->data_length + 15);
}

/**
 * 解析长包头
 * @param buf 完整长包
 * @param data_length 报文长度
 * @param data 输出的长包描述
 * 复制 11 字节头字段，并把 datas 指向载荷起始位置
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
 * @return 状态机保存的反馈压力；无当前通道返回 0xFF74
 * 保留准备、等待使用和早期使用阶段的历史反馈，不根据电机动作重新推导
 */
static uint16_t get_pressure_value(const uint8_t ams_num, const uint8_t ch) {
    if (ams_num >= AMS_COUNT || ams_num >= 4 || ch >= 4) return 0xFF74;
    return filament_ext[ams_num][ch].response_pressure;
}

/**
 * 计算 AMS 当前选中通道的耗材使用标志
 * @param ams_num AMS 编号
 * @return 0x00 表示空闲，0x02 表示送出/回卷反馈，0x04 表示准备/等待使用/使用反馈
 * 返回本 AMS 当前通道保存的反馈；等待使用时动作仍可为 send_out，反馈已经为 04
 */
static uint8_t get_filament_use_flag(const uint8_t ams_num) {
    if (ams_num >= AMS_COUNT || ams_num >= 4) return 0x00;
    const uint8_t ch = get_loaded_filament(ams_num);
    if (ch >= 4) return 0x00;
    const auto motion = filament_ext[ams_num][ch].motion;
    if (motion == filament_motion_internal::idle) return 0x00;
    return filament_ext[ams_num][ch].response_use_flag;
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


/**
 * 获取序列号查询使用的 AMS 编号
 * @param payload 长包载荷
 * @param payload_length 载荷长度
 * @return 载荷 [33] 的 AMS 编号；无选择字段时回退 AMS 0
 * 0x402 的 AMS 选择字段位于 [33]，兼容载荷不超过 33 字节的旧式请求
 */
static uint8_t serial_query_ams(const uint8_t *payload, const uint16_t payload_length) {
    if (!payload || payload_length <= 33u) return 0u;
    return payload[33];
}

/**
 * 验证请求中的 AMS 与通道范围
 * @param type 合法请求对应的报文类型
 * @param ams AMS 编号
 * @param channel AMS 内通道编号，无通道字段时传 0
 * @param allow_empty 是否允许 0xFF 表示无当前通道
 * @return 合法类型、范围错误 ERROR 或未启用 AMS 的 IGNORED
 * 协议容量为四个 AMS；配置外的有效 AMS 请求不会操作本机或恢复本机通信状态
 */
static package_type addressed_package(package_type type, uint8_t ams, uint8_t channel = 0,
                                      bool allow_empty = false) {
    if (ams >= 4u || (channel >= 4u && !(allow_empty && channel == 0xFFu)))
        return BambuBus_package_ERROR;
    if (ams * 4u >= static_cast<unsigned>(active_channel_count()))
        return BambuBus_package_IGNORED;
    return type;
}

/**
 * 识别并集中校验 BambuBus 请求
 * @param buf 完整报文
 * @param length 包含头部与两字节 CRC16 的总长度
 * @return 合法类型、校验/字段错误 ERROR 或不属于本机的 IGNORED
 * 在更新通信状态前校验帧头、CRC8/16、载荷、目标地址、AMS/通道和注册身份；不读取 CRC 尾部作为载荷
 */
static package_type get_package_type(unsigned char *buf, const int length) {
    if (!buf || length < 7 || length > BUS_FRAME_CAPACITY || buf[0] != 0x3Du)
        return BambuBus_package_ERROR;
    const bool is_short = (buf[1] & 0x80u) != 0u;
    if (!is_short && length < 15) return BambuBus_package_ERROR;
    const uint16_t declared = is_short ? buf[2] : static_cast<uint16_t>(buf[4]) | (static_cast<uint16_t>(buf[5]) << 8);
    const uint8_t crc8_index = is_short ? 3u : 6u;
    if (declared != length || buf[crc8_index] != bus_crc8(buf, crc8_index) ||
        !package_check_crc16(buf, length))
        return BambuBus_package_ERROR;

    if (is_short) {
        // 0xC5 为打印机请求；其它路由位包括本机发送回显，只消费而不应答
        if (buf[1] != 0xC5u) return BambuBus_package_IGNORED;
        switch (buf[4]) {
            case 0x03:
                if (length < 12) return BambuBus_package_ERROR;
                return addressed_package(BambuBus_package_filament_motion_short, buf[5], buf[7], true);
            case 0x04:
                if (length < 13) return BambuBus_package_ERROR;
                return addressed_package(BambuBus_package_filament_motion_long, buf[5], buf[9], true);
            case 0x05:
                if (length < 8) return BambuBus_package_ERROR;
                if (buf[5] == 0x00u) return BambuBus_package_online_detect;
                if (buf[5] == 0x01u) {
                    if (length < 26) return BambuBus_package_ERROR;
                    // 打印机认证/能力探测使用 0x80..0x83 等槽位编号；
                    // 这些帧不属于本机 AMS 注册请求，静默消费且不得伪造认证结果。
                    if (buf[6] >= 4u) return BambuBus_package_IGNORED;
                    const auto type = addressed_package(BambuBus_package_online_detect, buf[6]);
                    if (type != BambuBus_package_online_detect) return type;
                    return registration_identity_matches(buf, length) ? type : BambuBus_package_IGNORED;
                }
                return BambuBus_package_ETC; // 其它注册子命令留给后续兼容适配
            case 0x06:
                return BambuBus_package_REQx6;
            case 0x07:
                return BambuBus_package_NFC_detect;
            case 0x08:
                if (length < 45) return BambuBus_package_ERROR;
                return addressed_package(BambuBus_package_set_filament, buf[5] >> 4, buf[5] & 0x0Fu);
            case 0x20:
                return BambuBus_package_heartbeat;
            default:
                return BambuBus_package_ETC;
        }
    }

    if (buf[1] != 0x05u && buf[1] != 0x04u) return BambuBus_package_IGNORED;
    BambuBus_long_package_analysis(buf, length, &printer_data_long);
    if (printer_data_long.target_address != BambuBus_address_AMS08)
        return BambuBus_package_IGNORED;
    const uint16_t payload_length = printer_data_long.data_length;
    const uint8_t *payload = printer_data_long.datas;
    package_type type = BambuBus_package_ETC;
    switch (printer_data_long.type) {
        case 0x21A:
            if (payload_length < 1u) return BambuBus_package_ERROR;
            type = addressed_package(BambuBus_long_package_MC_online, payload[0]);
            break;
        case 0x211:
            if (payload_length < 2u) return BambuBus_package_ERROR;
            type = addressed_package(BambuBus_longe_package_filament, payload[0], payload[1]);
            break;
        case 0x218:
            if (payload_length < 34u) return BambuBus_package_ERROR;
            type = addressed_package(BambuBus_package_set_filament_type2, payload[0], payload[1]);
            break;
        case 0x103:
            if (payload_length < 1u) return BambuBus_package_ERROR;
            type = addressed_package(BambuBus_long_package_version, payload[0]);
            break;
        case 0x402:
            if (payload_length < 1u) return BambuBus_package_ERROR;
            type = addressed_package(BambuBus_long_package_version,
                                     serial_query_ams(payload, payload_length));
            break;
        default:
            break;
    }
    if (type != BambuBus_package_ERROR && type != BambuBus_package_IGNORED)
        bambubus_ams_address = BambuBus_address_AMS08;
    return type;
}

// 短包响应序号：占用响应头 bit3..5，每回复一帧在 0..7 间循环
static uint8_t package_num = 0;

/**
 * 生成耗材在线状态位图
 * @param AMS_num AMS 编号
 * @return 每通道 2 bit 的状态位图；无效 AMS 返回 0
 * bit0 表示在线，bit1 表示通道正在运动，供打印机显示耗材状态
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
/**
 * Cxx 的 44 字节基础响应，未知区与 BMCU-U 保持一致
 * 每次回复重新复制，防止上一帧的特殊准备数据污染普通响应或其他 AMS
 */
static const unsigned char Cxx_template[] = {
    0x3D, 0xC0, 0x2C, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF,
    0x36, 0x00, 0x00, 0x00, 0xF8, 0xFF, 0xF7, 0xFF, 0x00, 0x00, 0x27, 0x00,
    0x00, 0xFB, 0xF4, 0xF2, 0xF6, 0xB5, 0xB7, 0xB4, 0xB1,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static_assert(sizeof(Cxx_template) == 44, "Cxx frame length");
static unsigned char Cxx_res[sizeof(Cxx_template)];

/**
 * 填充 Cxx/Dxx 共用的运动反馈载荷
 * @param set_buf 至少 25 字节的运动载荷缓冲区，从响应中的 AMS 字段开始
 * @param ams_num 已校验的 AMS 编号
 * @return 反馈中的当前通道，空闲时为 0xFF
 * 从状态机读取当前通道、使用标志、长度、压力和位图，避免两个入口反馈不一致
 */
static uint8_t set_motion_response(unsigned char *set_buf, const uint8_t ams_num) {
    const uint8_t use_flag = get_filament_use_flag(ams_num);
    const uint8_t ch = use_flag ? get_loaded_filament(ams_num) : 0xFF;
    const float meters = ch < 4 ? data_save.filament[ams_num][ch].meters : 0.0f;
    const uint16_t pressure = get_pressure_value(ams_num, ch);
    set_buf[0] = ams_num;
    set_buf[2] = use_flag;
    set_buf[3] = ch;
    memcpy(set_buf + 4, &meters, sizeof(meters));
    memcpy(set_buf + 8, &pressure, sizeof(pressure));
    set_buf[24] = get_filament_left_char(ams_num);
    return ch;
}

/**
 * 填充 Cxx 的 09/7F 使用前准备响应
 * @param out 完整的 44 字节响应
 * @param ams_num AMS 编号
 * @param ch 当前通道，0xFF 表示空闲
 * 每个通道独立推进 45 行数据；保留 BMCU-U 实际写入的 [29..36] 字节位置
 */
static void set_prepare_response(unsigned char *out, const uint8_t ams_num, const uint8_t ch) {
    if (ch >= 4) {
        out[15] = 0x08;
        out[16] = 0x00;
        return;
    }
    auto &ext = filament_ext[ams_num][ch];
    if (ext.motion != filament_motion_internal::before_on_use || ext.last_prepare_flag != 0x7F)
        return;
    const size_t count = sizeof(BAMBU_PREPARE_7F_ROWS) / sizeof(BAMBU_PREPARE_7F_ROWS[0]);
    if (ext.prepare_reply_index < count) {
        const auto &row = BAMBU_PREPARE_7F_ROWS[ext.prepare_reply_index++];
        out[13] = static_cast<uint8_t>(row.pressure);
        out[14] = static_cast<uint8_t>(row.pressure >> 8);
        out[15] = static_cast<uint8_t>(row.unknow2);
        out[16] = static_cast<uint8_t>(row.unknow2 >> 8);
        memcpy(out + 17, row.unknow3, sizeof(row.unknow3));
        // 保留参考实现实际写入的偏移，不能按普通位图/last1/last2 成员边界右移一字节
        for (uint8_t i = 0; i < 4; ++i) {
            out[29 + i] = static_cast<uint8_t>(row.last1 >> (8 * i));
            out[33 + i] = static_cast<uint8_t>(row.last2 >> (8 * i));
        }
    } else {
        out[13] = 0x34; out[14] = 0x1E;
        out[15] = 0xFD; out[16] = 0x0A;
    }
}

/**
 * 回复短包运动请求
 * @param buf 收到的请求报文
 * @param length 请求长度
 * 要求完整的 12 字节请求；重建基础响应，按状态填充准备序列，最后计算 CRC
 */
static void send_for_Cxx(const unsigned char *buf, const int length) {
    if (!buf || length < 12) return;

    // Cxx 的通道位于 [7]、运动标志位于 [8]；与 Dxx 的布局不同
    if (!process_motion_state_machine(buf[5], buf[7], buf[6], buf[8])) return;
    now_op_num = buf[7];
    memcpy(Cxx_res, Cxx_template, sizeof(Cxx_res));
    Cxx_res[1] = 0xC0 | (package_num << 3);
    Cxx_res[38] = set_motion_response(Cxx_res + 5, buf[5]);
    set_prepare_response(Cxx_res, buf[5], Cxx_res[38]);

    package_send_with_crc(Cxx_res, sizeof(Cxx_res));
    if (package_num < 7)
        package_num++;
    else
        package_num = 0;
}


/**
 * Dxx 的 60 字节基础响应，使用短帧头及 BMCU-U 的未知状态区
 * 每帧重新复制后更新 AMS、请求通道、运动反馈和本机温湿度
 */
static const unsigned char Dxx_template[] = {
    0x3D, 0xC0, 0x3C, 0x00, 0x04,
    0x00, 0x75, 0x01, 0x11, // [6..7]=温度 0.1℃ 小端（默认 37.3℃），[8]=湿度 %（默认 17%）
    0x04, 0x04, 0x04, 0xFF,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF,
    0x36, 0x00, 0x00, 0x00, 0xF9, 0xFF, 0xF8, 0xFF, 0x00, 0x00, 0x27, 0x00,
    0x00, 0xFB, 0xF4, 0xF2, 0xF6, 0xB5, 0xB7, 0xB4, 0xB1,
    0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x00
};
static_assert(sizeof(Dxx_template) == 60, "Dxx frame length");
static unsigned char Dxx_res[sizeof(Dxx_template)];

// 0x06 握手锁定的 AMS 编号；0xFF 表示尚未锁定，之后重复握手沿用首个编号
static uint8_t res_for_06_num = 0xFF;

/**
 * 回复长包运动请求
 * @param buf 收到的请求报文
 * @param length 请求长度
 * 解析 AMS/通道/状态标志，驱动状态机并生成包含在线、NFC、长度和压力的 Dxx 响应
 * 与 Cxx 共用状态和反馈；回显请求通道，保留本机温湿度与 NFC 数据，不消耗 Cxx 准备序列
 */
static void send_for_Dxx(const unsigned char *buf, const int length) {
    if (!buf || length < 13) return;

    unsigned char filament_flag_on = 0x00;
    unsigned char filament_flag_NFC = 0x00;
    const unsigned char AMS_num = buf[5];
    // Dxx 的运动标志位于 [7]、通道位于 [9]
    if (!process_motion_state_machine(AMS_num, buf[9], buf[6], buf[7])) return;
    now_op_num = buf[9];
    memcpy(Dxx_res, Dxx_template, sizeof(Dxx_res));

    for (auto i = 0; i < 4; i++) {
        if (data_save.filament[AMS_num][i].statu == online) {
            filament_flag_on |= 1 << i;
        } else if (data_save.filament[AMS_num][i].statu == NFC_waiting) {
            filament_flag_on |= 1 << i;
            filament_flag_NFC |= 1 << i;
        }
    }

    Dxx_res[1] = 0xC0 | (package_num << 3);
    Dxx_res[5] = AMS_num;
    Dxx_res[9] = filament_flag_on;
    Dxx_res[10] = filament_flag_on - filament_flag_NFC;
    Dxx_res[11] = filament_flag_on - filament_flag_NFC;
    Dxx_res[12] = buf[9];
    Dxx_res[13] = filament_flag_NFC;
    set_motion_response(Dxx_res + 17, AMS_num);

    // AHT20 有效时把温湿度写入 AMS 长包回复；无效或未检测时恢复固定默认值，
    // 避免把上一次的旧读数继续发给打印机
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
            aht_temperature >= 0.0f
                ? aht_temperature * 10.0f + 0.5f
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
 * send_for_Fxx() 按 AMS 编号改写包序号和 AMS 地址后发送 29 字节 F00 响应
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
 * 使用 millis() 的 32 位无符号时间差忙等约 5ms，确保连续响应帧之间留有间隔并允许自然回绕
 */
static void bus_wait_idle_5ms() {
    const uint32_t t0 = millis();
    while (static_cast<uint32_t>(millis() - t0) < 5u) {
        __asm__ volatile ("nop");
    }
}

/**
 * 回复在线检测与注册请求
 * @param buf 收到的请求报文
 * @param length 请求长度
 * 广播首次使用 0C 前缀，随后使用 0A；确认请求检查 0A 和 16 字节身份，再回复并注册
 * 身份不匹配时不发送确认也不取消既有注册；CRC16 尾部不参与身份比较
 */
static void send_for_Fxx(const unsigned char *buf, const int length) {
    if (!TX_IDLE || !buf || length < 8) return;
    if (buf[5] == 0x01u && length < 26) return;
    refresh_serial_identities();

    // 广播注册阶段
    if (buf[5] == 0x00) {
        for (int i = 0; i < AMS_COUNT && i < 4; i++) {
            if (have_registered[i]) continue;
            // AMS 是否存在由 AMS_COUNT 决定，空槽不能让整台 AMS 消失

            // 使用对应AMS的响应数组
            F00_res[i][0] = 0x3D;
            F00_res[i][1] = 0xC0;
            F00_res[i][2] = 29;
            F00_res[i][3] = 0xB4;
            F00_res[i][4] = 0x05;
            F00_res[i][5] = 0x00;
            F00_res[i][6] = i;
            F00_res[i][7] = online_detect_phase[i] ? 0x0A : 0x0C;

            package_send_with_crc(F00_res[i], 29);
            online_detect_phase[i] = 1;
            bus_wait_idle_5ms();
        }
    }
    // 具体查询阶段
    else if (buf[5] == 0x01 && buf[6] < AMS_COUNT && buf[6] < 4) {
        if (!registration_identity_matches(buf, length)) return;
        const uint8_t ams_num = buf[6];

        // 使用对应AMS的响应数组
        F00_res[ams_num][0] = 0x3D;
        F00_res[ams_num][1] = 0xC0;
        F00_res[ams_num][2] = 29;
        F00_res[ams_num][3] = 0xB4;
        F00_res[ams_num][4] = 0x05;
        F00_res[ams_num][5] = 0x01;
        F00_res[ams_num][6] = ams_num;
        F00_res[ams_num][7] = 0x0A;

        package_send_with_crc(F00_res[ams_num], 29);

        // 同步发送结束后才确认当前 AMS 注册成功，其他 AMS 的状态不受影响
        have_registered[ams_num] = true;
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
 * 校验 AMS 编号后复制包序号和地址，回复 MC 在线状态
 */
void send_for_long_packge_MC_online(unsigned char *buf, int length) {
    long_packge_data data{};
    BambuBus_long_package_analysis(buf, length, &printer_data_long);

    if (printer_data_long.data_length < 1u) return;

    // 目标地址和请求载荷已由 get_package_type() 集中校验

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
 * 读取指定通道的 ID、名称、颜色和温度范围，组装长包回复打印机
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

// 注册和序列号查询共用的 66 字节载荷模板；[33..48] 为注册身份块
static const unsigned char serial_identity_template[] = {
    0x0F,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0E, 0xA0,
    0x32, 0x31, 0x31, 0x38, 0x15, 0x00, 0x36, 0x39, 0x37, 0x33, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xBB, 0x44, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00
};
static_assert(sizeof(serial_identity_template) == sizeof(serial_identity[0]), "serial identity payload size");

// 版本号数组（17字节）
// AMS lite 版本响应中的版本号和型号名模板（"AMS_F102"）
static unsigned char long_packge_version_version_and_name_AMS_lite[] = {
    0x00, 0x00, 0x00, 0x3C, // 版本号60
    0x41, 0x4D, 0x53, 0x5F, 0x46, 0x31, 0x30, 0x32, 0x00, 0x00, 0x00, 0x00, 0x00, // "AMS_F102"
    0x00, 0x00, 0x00, 0x00 // 填充 + AMS序号位置
};

// AMS 08 版本响应中的版本号和型号名模板（"AMS08"）
static unsigned char long_packge_version_version_and_name_AMS08[] = {
    0x57, 0x06, 0x00, 0x01, // AMS 版本 1.0.6.87（发送时由设置覆盖）
    0x41, 0x4D, 0x53, 0x30, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // "AMS08"
    0x00, 0x00, 0x00, 0x00 // 填充 + AMS序号位置
};

/**
 * 将序列号中的十六进制字符转换为半字节
 * @param c 单个十六进制字符
 * @return 0..15 的数值，非法字符按 0 处理
 * 设置校验和自动序列号生成保证输入为合法十六进制字符
 */
static uint8_t serial_hex_nibble(const char c) {
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
    if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
    return 0u;
}

/**
 * 更新各 AMS 的统一注册身份与序列号
 * 固定生成 0EA + AMS 编号 + 12 位后缀，注册包直接复制序列号载荷的 [33..48]
 * 自定义值只替换末 12 位；留空时后缀取设备 ID 低 48 位，首字节保持协议固定 0x0F
 * 可见序列号会同步编码到注册身份块 [33..40]；身份变更后各 AMS 从首次 0C 报名重新开始
 */
static void refresh_serial_identities() {
    static bool ready = false;
    static char previous_custom[CUSTOM_SERIAL_LENGTH + 1] = {};
    char current_custom[CUSTOM_SERIAL_LENGTH + 1] = {};
    for (int i = 0; i < CUSTOM_SERIAL_LENGTH && CUSTOM_SERIAL[i]; ++i)
        current_custom[i] = CUSTOM_SERIAL[i];
    if (ready && memcmp(previous_custom, current_custom, sizeof(current_custom)) == 0) return;

    static constexpr char hex[] = "0123456789ABCDEF";
    const uint64_t id = hal_get_device_id();
    for (uint8_t ams = 0; ams < 4; ++ams) {
        auto *serial = serial_identity[ams];
        memcpy(serial, serial_identity_template, sizeof(serial_identity_template));
        serial[1] = '0';
        serial[2] = 'E';
        serial[3] = 'A';
        serial[4] = '0' + ams;
        if (current_custom[0]) {
            memcpy(serial + 5, current_custom, CUSTOM_SERIAL_LENGTH);
        } else {
            for (int i = 0; i < CUSTOM_SERIAL_LENGTH; ++i)
                serial[5 + i] = hex[(id >> ((11 - i) * 4)) & 0x0Fu];
        }
        serial[34] = 0xA0 + ams;
        for (int i = 0; i < 8; ++i)
            serial[35 + i] = static_cast<unsigned char>(id >> (56 - i * 8));
        // 注册身份 [33..40] 同步为可见序列号的前 8 字节，否则打印机仍显示旧设备哈希
        for (int i = 0; i < 8; ++i) {
            serial[33 + i] = static_cast<uint8_t>(
                (serial_hex_nibble(static_cast<char>(serial[1 + i * 2])) << 4) |
                serial_hex_nibble(static_cast<char>(serial[2 + i * 2])));
        }
        serial[65] = ams;
        F00_res[ams][6] = ams;
        memcpy(F00_res[ams] + 8, serial + 33, 16);
        have_registered[ams] = false;
        online_detect_phase[ams] = 0;
        F00_res[ams][7] = 0x0C;
    }
    memcpy(previous_custom, current_custom, sizeof(current_custom));
    ready = true;
}

/**
 * 检查注册确认请求中的完整身份
 * @param buf 注册确认请求
 * @param length 包含 CRC16 的帧长
 * @return true 表示 AMS、身份前缀和 16 字节身份块均匹配
 * 不匹配不发确认，也不清除其他合法查询建立的注册状态；CRC 由集中解析入口校验
 */
static bool registration_identity_matches(const uint8_t *buf, const int length) {
    if (!buf || length < 26 || buf[4] != 0x05 || buf[5] != 0x01 ||
        buf[6] >= AMS_COUNT || buf[6] >= 4)
        return false;
    refresh_serial_identities();
    return buf[7] == 0x0A && memcmp(F00_res[buf[6]] + 8, buf + 8, 16) == 0;
}

/**
 * 回复序列号或版本查询
 * @param buf 收到的长包
 * @param length 报文长度
 * type=0x402 回复该 AMS 的统一身份载荷，0x103 回复版本；序列号和注册包来自同一份数据
 */
static void send_for_long_packge_version(unsigned char *buf, int length) {
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
        // 序列号查询通常把 AMS 编号放在载荷 [33]；旧式短请求回退 AMS 0
        AMS_num = serial_query_ams(printer_data_long.datas, printer_data_long.data_length);
        if (AMS_num >= AMS_COUNT || AMS_num >= 4) return;
        refresh_serial_identities();
        payload = serial_identity[AMS_num];
        payload_len = sizeof(serial_identity[AMS_num]);
    } else if (printer_data_long.type == 0x103) // 版本查询
    {
        payload = long_packge_version_version_and_name_AMS08;
        payload_len = sizeof(long_packge_version_version_and_name_AMS08);
        // 网页显示 A.B.C.D，线上按 D.C.B.A 写入；末字节保留 AMS 编号
        for (int i = 0; i < 4; ++i)
            payload[i] = static_cast<unsigned char>(AMS_VERSION[3 - i]);
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
 * 拒绝未启用的 AMS 或越界通道，解析完整载荷后回复 ACK 并标记记录待保存
 */
static void send_for_Set_filament(const unsigned char *buf, const int length) {
    if (!buf || length < 45) return;

    uint8_t read_num = buf[5];
    const uint8_t AMS_num = (read_num >> 4) & 0x0F;
    read_num = read_num & 0x0F;
    now_op_num = read_num;

    if (AMS_num >= AMS_COUNT || AMS_num >= 4 || read_num >= 4) return;

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
 * 校验载荷和启用范围后解析耗材信息，回复三字节确认；实际写入由空闲保存入口执行
 */
void send_for_Set_filament_type2(unsigned char *buf, int length) {
    long_packge_data data{};
    BambuBus_long_package_analysis(buf, length, &printer_data_long);

    if (printer_data_long.data_length < 34u) return;

    const uint8_t AMS_num = printer_data_long.datas[0];
    const uint8_t read_num = printer_data_long.datas[1];

    if (AMS_num >= AMS_COUNT || AMS_num >= 4 || read_num >= 4) return;

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
 * 供网页或调试代码查询协议状态
 */
int get_cmd_type() {
    return cmd;
}

/**
 * 读取最近操作的通道号
 * @return 最近操作的通道号；未知时为 255
 * 记录 Cxx/Dxx/设置耗材等请求中的通道
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
// 超时由主循环锁存，仅新的合法帧/心跳可以清除，避免长期断线跨回绕后误恢复
static bool bus_packet_timed_out = false;
static bool heartbeat_timed_out = false;

/**
 * 更新通信和心跳超时锁存
 * @param now 当前 32 位毫秒时间
 * 主循环每轮调用；分别保持 1 秒合法通信超时和 10 秒心跳超时，直到新的合法报文清除
 */
static void update_bus_timeouts(const uint32_t now) {
    if (bus_packet_seen && now - last_bus_packet_ms > 1000u)
        bus_packet_timed_out = true;
    if (heartbeat_seen && now - last_heartbeat_ms >= 10000u)
        heartbeat_timed_out = true;
}

/**
 * 获取打印机连接状态
 * @return 0=尚未收到心跳，1=最近 5 秒有心跳，2=心跳超时
 * 仅表示通信连接状态；超时锁存到下次合法心跳，不会因 millis() 回绕而自动恢复
 */
int BambuBus_printer_status() {
    update_bus_timeouts(millis());
    return !heartbeat_seen ? 0 : heartbeat_timed_out ? 2 : 1;
}

/**
 * 读取接收错误帧总数
 * @return 错误帧累计数量
 * 返回 CRC 或格式错误导致的丢帧计数
 */
uint32_t BambuBus_rx_error_count() {
    return rx_error_load();
}

/**
 * 读取接收丢帧总数
 * @return 丢帧累计数量
 * 返回完整帧因接收队列已满而丢弃的计数
 */
uint32_t BambuBus_rx_dropped_count() {
    return rx_dropped_load();
}

/**
 * 读取距最近合法报文的时间
 * @return 毫秒数；从未收到合法报文时返回 UINT32_MAX
 * 供网页显示总线新鲜度
 */
uint32_t BambuBus_last_valid_packet_age_ms() {
    if (!bus_packet_seen) return UINT32_MAX;
    return millis() - last_bus_packet_ms;
}

/**
 * 判断总线最近是否有活动
 * @param quiet_ms 静默时间窗口（毫秒）
 * @return true 表示当前不满足总线静默条件
 * 检查请求预占、发送槽、队列、UART、接收回调及最近收发；残帧静默超过 100ms 后不再阻止保存
 */
bool BambuBus_recent_activity(const uint32_t quiet_ms) {
    // 回调尚未退出时，即使 UART 已读空或帧还未发布也不能开始保存
    const uint32_t callback_seq = hal_atomic_load_u32(&rx_callback_seq);
    if ((callback_seq & 1u) || !TX_IDLE || bus_request_active) return true;
    const uint32_t last_rx = hal_atomic_load_u32(&last_bus_rx_ms);
    const uint32_t partial = hal_atomic_load_u32(&rx_partial);
    const bool received = hal_atomic_load_u32(&bus_rx_seen) != 0;
    const uint32_t now = millis();
    return completed_frame_peek() >= 0 || hal_bus_available() ||
           hal_atomic_load_u32(&rx_callback_seq) != callback_seq ||
           (received && (now - last_rx < quiet_ms ||
                         (partial && now - last_rx <= BUS_RX_PARTIAL_TIMEOUT_MS))) ||
           (bus_tx_seen && now - last_bus_tx_ms < quiet_ms);
}

/**
 * 获取报文类型名称
 * @param type package_type 枚举值
 * @return 用于日志显示的名称
 * 把报文类型转换为中文/英文可读文本
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
        case BambuBus_package_IGNORED: return "忽略(IGNORED)";
        case BambuBus_package_set_filament_type2: return "长包设置耗材(set_filament_type2)";
        default: return "未知(unknown)";
    }
}

/**
 * BambuBus 主处理循环
 * @return 0=无帧、发送忙或忽略其它设备帧，1=处理了本机合法帧，2=错误帧，-1=合法通信超时
 * 发送槽空闲时才预占并解析队首，每轮最多处理一帧；全部回复及日志完成后释放请求和缓冲
 * 发送忙时不消费请求、不改变业务状态，仍更新通信超时；保存由独立后台入口执行
 */
int BambuBus_run() {
    int stu = 0;
    const uint32_t now = millis();
    if (!TX_IDLE || bus_request_active) {
        update_bus_timeouts(now);
        return bus_packet_timed_out ? -1 : 0;
    }
    const int slot = completed_frame_peek();

    if (slot >= 0) {
        bus_request_active = true;
        const int data_length = completed_len[slot];
        uint8_t *const frame = completed_buf[slot];
        cmd = get_package_type(frame, data_length);
        if (cmd != BambuBus_package_ERROR && cmd != BambuBus_package_IGNORED) {
            stu = 1;
            last_bus_packet_ms = now;
            bus_packet_seen = true;
            bus_packet_timed_out = false;
        } else if (cmd == BambuBus_package_ERROR) {
            stu = 2;
            rx_error_increment();
        }
        if (BUS_LOG_ENABLE) {
            // 只在完整报文解析后记录 RX，这样十六进制行可以和类型精确绑定
            log_bus_hex_typed(LOG_CH_BUS, 'R', cmd, frame, static_cast<uint16_t>(data_length));
            LOG_BUS("<< 收到包 %s (type=%d) len=%d", package_type_name(cmd), cmd, data_length);
        }
        bus_tx_context_type = cmd;
        switch (cmd) {
            case BambuBus_package_heartbeat:
                last_heartbeat_ms = now;
                heartbeat_seen = true;
                heartbeat_timed_out = false;
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
        completed_frame_release();
        bus_request_active = false;
    }

    update_bus_timeouts(millis());
    if (bus_packet_timed_out)
        stu = -1;

    return stu;
}

/**
 * 在总线空闲时保存耗材记录
 * 每轮先更新一秒冷却标记，再检查收发静默；写入并回读成功才清除待保存标记，失败下轮限频重试
 * 冷却到期会被锁存为就绪，即使长期没有新保存请求也不会被旧截止时间阻塞
 */
void BambuBus_save_run() {
    static bool cooling_down = false;
    static uint32_t save_finished_ms = 0;
    const uint32_t now = millis();
    if (cooling_down && now - save_finished_ms >= 1000u)
        cooling_down = false;
    if (!BambuBus_need_to_save || cooling_down || BambuBus_recent_activity(5u))
        return;

    if (BambuBus_save()) {
        BambuBus_need_to_save = false;
    } else {
        LOG_MAIN("[storage] 耗材记录写入或校验失败，将在总线空闲时重试");
    }
    save_finished_ms = millis();
    cooling_down = true;
}
