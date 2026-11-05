/**
 * PCF8575引脚映射头文件
 * 
 * 地址 0x20～0x23 每片有 16 个有效虚拟引脚，起始编号依次为 50、70、90、110
 * 每片使用起始编号加 0～7、10～17，尾数 8、9 留空；四片共 64 个引脚
 * 位序使用 0～15；面板名称为 P0～P7、P10～P17，名称不能直接用于位移
 * 
 * 使用方法：
 * 1. 在配置中使用虚拟引脚号，首片为 50～57、60～67，末片为 110～117、120～127
 * 2. 调用PCF8575_init()初始化PCF8575
 * 3. 调用PCF8575_update()更新引脚状态
 */

#pragma once
#include <Arduino.h>

// PCF8575配置
#define PCF8575_VIRTUAL_PIN_BASE 50
#define PCF8575_ADDR 0x20           // 首片地址，后续芯片依次加 1
#define PCF8575_DEVICE_COUNT 4
#define PCF8575_PIN_COUNT 16        // 每片的寄存器位数
#define PCF8575_VIRTUAL_PIN_STRIDE 20 // 相邻芯片起始编号的间隔，含空缺编号
#define PCF8575_VIRTUAL_PIN_COUNT (PCF8575_DEVICE_COUNT * PCF8575_PIN_COUNT)
#define PCF8575_VIRTUAL_PIN_LIMIT (PCF8575_VIRTUAL_PIN_BASE + PCF8575_DEVICE_COUNT * PCF8575_VIRTUAL_PIN_STRIDE)

// 虚拟引脚范围检查；每十个编号只使用前八个
#define IS_PCF8575_VIRTUAL_PIN(pin) ((pin) >= PCF8575_VIRTUAL_PIN_BASE && (pin) < PCF8575_VIRTUAL_PIN_LIMIT && ((pin) - PCF8575_VIRTUAL_PIN_BASE) % 10 < 8)

// 调用前必须检查有效性；面板名称 0～7、10～17，寄存器位序 0～15，缓存索引 0～63
#define VIRTUAL_TO_PCF8575_DEVICE(pin) (((pin) - PCF8575_VIRTUAL_PIN_BASE) / PCF8575_VIRTUAL_PIN_STRIDE)
#define VIRTUAL_TO_PCF8575_PIN(pin) (((pin) - PCF8575_VIRTUAL_PIN_BASE) % PCF8575_VIRTUAL_PIN_STRIDE)
#define VIRTUAL_TO_PCF8575_BIT(pin) (VIRTUAL_TO_PCF8575_PIN(pin) / 10 * 8 + VIRTUAL_TO_PCF8575_PIN(pin) % 10)
#define VIRTUAL_TO_PCF8575_INDEX(pin) (VIRTUAL_TO_PCF8575_DEVICE(pin) * PCF8575_PIN_COUNT + VIRTUAL_TO_PCF8575_BIT(pin))

/**
 * 初始化PCF8575 的四个地址槽
 * @return true 表示至少一片初始化成功
 * 共用已初始化的 I2C 总线，缺失芯片不影响其他芯片，重复调用不重置在线芯片
 */
bool PCF8575_init();

/**
 * 查询指定 PCF8575 是否在线
 * @param device 芯片索引 0～3，默认 0 对应原有的 0x20 芯片
 * @return true 表示芯片已完成初始化且可以读写
 * 未配置 I2C 或芯片未应答时返回 false，调用方应停用扩展虚拟引脚
 */
bool PCF8575_is_online(uint8_t device = 0);

/**
 * 查询虚拟引脚所属芯片是否在线
 * @param pin 虚拟引脚号（50～127，尾数 8、9 无效）
 * @return true 表示引脚有效且所属芯片已经初始化
 * 电机及微动按各自接线判断，其他芯片在线不能代替当前芯片
 */
bool PCF8575_pin_is_online(uint8_t pin);

/**
 * 定期重试离线 PCF8575
 * 各离线芯片独立每 5 秒重试，恢复时清空输出电平并保留已配置模式
 */
void PCF8575_poll();

/**
 * 更新所有PCF8575引脚状态
 * 需要在loop()中定期调用
 */
void PCF8575_update();

/**
 * 设置PCF8575虚拟引脚模式（模拟pinMode）
 * @param pin 虚拟引脚号（50～127，尾数 8、9 无效）
 * @param mode 模式（OUTPUT/INPUT）
 */
void PCF8575_pinMode(uint8_t pin, uint8_t mode);

/**
 * 设置PCF8575虚拟引脚电平（模拟digitalWrite）
 * @param pin 虚拟引脚号（50～127，尾数 8、9 无效）
 * @param value 电平（HIGH/LOW）
 */
void PCF8575_digitalWrite(uint8_t pin, uint8_t value);

/**
 * 读取PCF8575虚拟引脚电平（模拟digitalRead）
 * @param pin 虚拟引脚号（50～127，尾数 8、9 无效）
 * @return 引脚电平（HIGH/LOW）
 */
int PCF8575_digitalRead(uint8_t pin);

/**
 * 设置PCF8575虚拟引脚PWM（模拟analogWrite）
 * @param pin 虚拟引脚号（50～127，尾数 8、9 无效）
 * @param value PWM值（0-255）
 */
void PCF8575_analogWrite(uint8_t pin, uint8_t value);

// Arduino标准函数重写，用于透明替换
void pinMode_PCF8575(uint8_t pin, uint8_t mode);
void digitalWrite_PCF8575(uint8_t pin, uint8_t value);
int digitalRead_PCF8575(uint8_t pin);
void analogWrite_PCF8575(uint8_t pin, uint8_t value);
