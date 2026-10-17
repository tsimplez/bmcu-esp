# OTA 更新服务器配置

设备端已经从「更新源目录」拉取 `manifest.json`，再按自己的板子名找固件下载、校验、写 Flash。
你只需要一个能把静态文件发出去的服务器 —— 家里的 NAS、树莓派、云主机、对象存储都行。

## 一、目录结构

发布目录里就两样东西：清单 + 各平台的 `.bin`。

```
ota/
├── manifest.json
├── firmware-esp32s3-26.09.25.bin
├── firmware-esp32c3-26.09.25.bin
└── firmware-airm2m_core_esp32c3-26.09.25.bin
```

设备网页里「更新源目录」填的是**这个目录的 URL**，比如：

- `http://192.168.1.10/ota`（局域网，最简单）
- `https://example.com/ota`（公网 + HTTPS）

不用带 `/manifest.json`，也不用带结尾斜杠，填了也会被自动去掉。

## 二、manifest.json 格式

```json
{
  "esp32s3": {"version":"26.09.25","notes":"修复 RS485 偶发丢包","url":"firmware-esp32s3-26.09.25.bin","size":1029824,"md5":"229dfea069befa82c61299e0efa0fc37"},
  "esp32c3": {"version":"26.09.25","notes":"修复 RS485 偶发丢包","url":"firmware-esp32c3-26.09.25.bin","size":1101920,"md5":"da8ec9af5d6c686d9a5b5563687e95ee"},
  "airm2m_core_esp32c3": {"version":"26.09.25","notes":"修复 RS485 偶发丢包","url":"firmware-airm2m_core_esp32c3-26.09.25.bin","size":1092016,"md5":"f54b287e9ba739e41db3b29720eb3612"}
}
```

字段说明：

- 每个板型条目独立带 `version` 和 `notes`，只更新一个板型时不会让其他板型误报新版。版本格式为 `YY.MM.DD` 或 `YY.MM.DD.修订号`，例如 `26.12.22.123`；数字逐段比较。
- 板子键名：必须和固件里的 `FW_TARGET` 完全一致（`src/version.h`）。设备只读自己那一条，其他板子的条目留着无所谓。
- 每条的 `url` 写文件名（会自动拼到更新源目录后面）或完整 URL 都行。
- `md5` 必填：设备写 Flash 时会校验完整内容。MD5 只能校验传输一致性，不能证明来源可信。
- `size` 用于页面显示，并与服务器返回的 `Content-Length` 核对。

## 三、服务器要求

1. **能直接 GET 到这两个文件**就行，不需要任何动态逻辑。
2. **必须返回 `Content-Length`**（正常静态服务器都有）。设备拒绝分块传输和长度不明的响应。
3. **HTTPS 可选**。设备用 `setInsecure()`，不校验证书，所以自签名证书也能用 —— 代价是没法防中间人，安全性等同 HTTP。要是只在内网用，直接 HTTP 更省事。
4. **不需要配 CORS**。拉清单的是设备上的原生 HTTP 客户端，不走浏览器同源策略；只有你想在浏览器里直接打开 manifest 调试时才需要。
5. **别对这个目录开强制 gzip / 压缩**。固件是二进制，压了反而要设备解压；`manifest.json` 才几百字节，也没必要。
6. **允许 302 跳转**（可选）。设备开了跟随重定向，对象存储的临时链接、CDN 跳转都能用。

### nginx 例子

```nginx
server {
    listen 80;
    root /var/www/html;          # 把 ota/ 放到 /var/www/html/ota/

    location /ota/ {
        autoindex on;            # 方便浏览器里点着看
        types { application/octet-stream bin; application/json json; }
        add_header Cache-Control "no-cache";   # 发新版本后别被缓存住
    }
}
```

公网 HTTPS 就是正常的 443 站点再加一段 `location /ota/`，证书申请和普通网站一样。

## 四、发布一个新固件

```powershell
# 1. 改版本号：src/version.h 里的 FW_VERSION，例如 26.09.25 -> 26.09.25.1
# 2. 重新编译（脚本会去 .pio/build/<env>/firmware.bin 拿产物）
$env:PLATFORMIO_CORE_DIR='D:\Data\.platformio'
pio run -e esp32s3 -e esp32c3 -e airm2m_core_esp32c3

# 3. 生成发布目录
python tools\make_ota.py esp32s3 esp32c3 airm2m_core_esp32c3 --notes "修复 XX 问题"

# 4. 把 dist 目录整个传到服务器对应的 ota 目录
```

`make_ota.py` 的清单是**累加**的：这次只发布了 `esp32s3`，之前发过的 `esp32c3` 条目会原样保留，
所以可以一块板子一块板子地发，不影响其它板子检查更新。
脚本会检查版本号、构建产物时间和二进制中的版本字符串；请在修改 `version.h` 后重新编译。

参数：

| 参数 | 说明 |
|------|------|
| `targets` | 一个或多个 PlatformIO 环境名，同时也是清单里的板子键名 |
| `--version` | 版本号，默认从 `src/version.h` 读；指定时必须与头文件一致 |
| `--notes` | 更新说明，写入本次发布的板型条目 |
| `--out` | 输出目录，默认 `dist/`（已在 `.gitignore` 里） |

## 五、设备端怎么用

1. 网页「参数 → 固件更新 → 更新源目录」填上 URL，点「保存地址」。
2. 开「进页面提示」的话，以后每次打开网页都会自动查一次；有新版顶部会挂一条**常驻横幅**，
   点「去更新」直达更新卡片。关掉提示后不自动查，但「检查更新」按钮随时能手动查。
3. 停止打印，断开打印机总线，等待至少 30 秒且确认所有电机停止。固件会拒绝有近期通信或运动的设备更新。
4. 「在线更新」= 设备自己下载 + 写 Flash + 重启，全程别断电，大概几十秒。
5. 服务器不方便动、或者固件已经在手上：直接用「本地升级」选 `.bin` 传上去，仍需满足停止条件。

## 六、常见问题

- **检查更新报「地址无法解析」**：URL 少了 `http://` 前缀，或者设备所在网络到不了这个地址。
- **报「清单里没有 esp32c3 对应的固件」**：`manifest.json` 里没有这个键，或者键名和 `FW_TARGET` 拼写不一致。
- **报「服务器返回 HTTP 404」**：更新源目录填错了，或者文件没传上去。
- **HTTPS 站点连不上**：先换 `http://` 试；有些主机的 TLS 版本或加密套件 ESP32 的 mbedTLS 不支持。
- **升级完又回到老版本**：确认当前板型条目的 `version` 比设备上的高，并且上传的是新编译的 `.bin`。
- **新固件启动但业务异常**：双 OTA 分区本身不会自动判定业务健康或回退；必要时用 USB 恢复。
