// OTA 主机测试边界：仅模拟 Arduino 字符串、HTTP、Flash 和设备状态。
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <map>
#include <vector>
#include <algorithm>
#include <type_traits>
#include <cstdio>

// 为真实 OTA 源码提供 Arduino String 使用到的操作。
class String : public std::string {
public:
    using std::string::string;
    using std::string::operator=;
    /** 接收标准字符串运算结果 */
    String(const std::string &s) : std::string(s) {}
    /** 模拟 Arduino 数字格式化 */
    template<class T, typename std::enable_if<std::is_arithmetic<T>::value, int>::type = 0>
    String(T n) : std::string(std::to_string(n)) {}
    /** 查找子串并用 -1 表示未找到 */
    int indexOf(const String &s) const { auto p = find(s); return p == npos ? -1 : int(p); }
    /** 查找单个字符 */
    int indexOf(char c) const { auto p = find(c); return p == npos ? -1 : int(p); }
    /** 判断前缀 */
    bool startsWith(const String &s) const { return rfind(s, 0) == 0; }
    /** 判断后缀 */
    bool endsWith(const String &s) const { return size() >= s.size() && compare(size()-s.size(), s.size(), s) == 0; }
    /** 删除末尾指定位置后的内容 */
    void remove(size_t p) { erase(p); }
    /** 截取起止位置之间的内容 */
    String substring(size_t a, size_t b) const { return substr(a, b-a); }
    /** 去掉首尾空白 */
    void trim() { auto a=find_first_not_of(" \t\r\n"), b=find_last_not_of(" \t\r\n"); *this=a==npos ? "" : substr(a,b-a+1); }
    /** 统一摘要字符大小写 */
    void toLowerCase() { for (char &c : *this) c=char(std::tolower(static_cast<unsigned char>(c))); }
};

struct Response { int status=-1; String body; int length=-2; int mode=0; String location; };
static std::map<std::string, Response> responses;
static std::vector<String> requests, checked_md5;
static Response current_response;
static bool busy=false;
static uint32_t fake_ms=0;
static int starts=0, aborts=0, writes=0, feeds=0;
static constexpr int HTTP_CODE_OK=200, HTTPC_DISABLE_FOLLOW_REDIRECTS=0, UPDATE_ERROR_MD5=7;
static const char *FW_TARGET="esp32c3";
#define DEFAULT_OTA_URL "https://tsimplez.github.io/bmcu-esp"
static String OTA_URL=DEFAULT_OTA_URL;
#define LOG_MAIN(...) do {} while (0)
/** 模拟设备可维护状态 */
static bool BMCU_can_maintain() { return !busy; }
/** 记录喂狗，验证回退路径仍喂狗 */
static void hal_watchdog_feed() { ++feeds; }
/** 为中断下载提供可控时钟 */
static uint32_t millis() { return fake_ms; }
/** 模拟接收等待时让出 CPU */
static void delay(unsigned ms) { fake_ms+=ms; }

class WiFiClient {
public:
    size_t offset=0;
    /** 允许通过基类释放 TLS 客户端 */
    virtual ~WiFiClient() = default;
    /** 读取模拟固件；mode=1 表示接收部分字节后超时 */
    int readBytes(uint8_t *out, size_t count) {
        if (current_response.mode==1 && offset) { fake_ms+=16001; return 0; }
        size_t n=std::min(count, current_response.body.size()-offset);
        if (current_response.mode==1) n=std::min(n, size_t(1));
        memcpy(out,current_response.body.data()+offset,n); offset+=n; return int(n);
    }
};
class WiFiClientSecure : public WiFiClient {
public:
    /** 测试不建立真实 TLS */
    void setInsecure() {}
    /** 检查生产代码限制握手等待 */
    void setHandshakeTimeout(unsigned seconds) { assert(seconds<=3); }
    /** 默认模拟普通连接失败，没有额外 TLS 错误 */
    int lastError(char *, size_t) { return 0; }
};
class HTTPClient {
    String url;
    WiFiClient *client=nullptr;
public:
    /** 接收连接超时配置 */
    void setConnectTimeout(int ms) { assert(ms<=2000); }
    /** 接收读取超时配置 */
    void setTimeout(int ms) { assert(ms<=2000); }
    /** 禁止底层自动跳转，由生产代码逐次处理 */
    void setFollowRedirects(int mode) { assert(mode==0); }
    /** 绑定模拟连接和完整 URL */
    bool begin(WiFiClient &c, const String &u) { client=&c;url=u;return true; }
    /** 按 URL 返回预设响应并记录请求顺序 */
    int GET() { requests.push_back(url);current_response=responses[url];client->offset=0;if(current_response.mode==4)busy=true;return current_response.status; }
    /** 提供已知长度或模拟缺失长度 */
    int getSize() { return current_response.length==-2 ? int(current_response.body.size()) : current_response.length; }
    /** 提供清单内容 */
    String getString() { return current_response.body; }
    /** 提供固件数据流 */
    WiFiClient *getStreamPtr() { return client; }
    /** 模拟中途断开连接 */
    bool connected() { return current_response.mode!=1; }
    /** 提供服务器跳转地址 */
    String getLocation() { return current_response.location; }
    /** 更新模拟完整跳转 URL */
    bool setURL(const String &u) { url=u;return true; }
    /** 模拟关闭 HTTP 请求 */
    void end() {}
    /** 提供传输错误描述 */
    static String errorToString(int) { return "connection refused"; }
};
class FakeUpdate {
public:
    /** 记录写入开始；不访问真实 Flash */
    bool begin(size_t) { ++starts;return true; }
    /** 记录每次写入所校验的摘要 */
    bool setMD5(const char *md5) { checked_md5.push_back(md5);return true; }
    /** mode=2 模拟不可重试的 Flash 写入错误 */
    size_t write(const uint8_t *, size_t n) { ++writes;return current_response.mode==2 ? 0 : n; }
    /** mode=3 模拟摘要不匹配 */
    bool end() { return current_response.mode!=3; }
    /** 暴露模拟摘要错误 */
    int getError() { return UPDATE_ERROR_MD5; }
    /** 提供模拟错误说明 */
    const char *errorString() { return "injected error"; }
    /** 记录中止，验证回退前结束失败写入 */
    void abort() { ++aborts; }
} Update;
