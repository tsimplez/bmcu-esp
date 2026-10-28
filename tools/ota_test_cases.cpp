/** 重置网络、Flash 和运行状态，场景之间不共享结果 */
static void reset_ota_test() {
    responses.clear();requests.clear();checked_md5.clear();busy=false;fake_ms=0;
    starts=aborts=writes=feeds=0;OTA_URL=DEFAULT_OTA_URL;
}
/** 构造小型有效清单，仅模拟网络响应，解析使用生产实现 */
static String manifest(const String &file="firmware-esp32c3-26.10.28.bin") {
    return "{\"esp32c3\":{\"version\":\"26.10.28\",\"url\":\""+file+
        "\",\"size\":4,\"md5\":\"098F6BCD4621D373CADE4E832627B4F6\"}}";
}
/** 设置某个渠道的有效清单并执行真实检查流程 */
static void select_source(int source) {
    responses[ota_source_base(source)+"/manifest.json"]={200,manifest()};
    String err;assert(ota_fetch_manifest(err));assert(g_ota_source==source);
    if (OTA_URL==DEFAULT_OTA_URL) {
        assert(requests.size()==static_cast<size_t>(source+1));
        for(int i=0;i<=source;++i)assert(requests[i]==ota_source_base(i)+"/manifest.json");
    }
    requests.clear();
}
/** 验证清单回退、渠道固定、完整性约束和下载失败边界 */
int main() {
    String err;
    reset_ota_test();select_source(2);
    assert(g_ota_md5=="098f6bcd4621d373cade4e832627b4f6");
    assert(g_ota_url=="https://ghfast.top/https://github.com/tsimplez/bmcu-esp/releases/download/v26.10.28/firmware-esp32c3-26.10.28.bin");
    assert(feeds>=6);

    reset_ota_test();
    responses[ota_source_base(0)+"/manifest.json"]={200,"<html>proxy error</html>"};
    responses[ota_source_base(1)+"/manifest.json"]={200,manifest(),9000};
    select_source(3);assert(g_ota_source==3);

    reset_ota_test();assert(!ota_fetch_manifest(err));assert(requests.size()==5);
    assert(g_ota_url.empty() && g_ota_md5.empty() && g_ota_source==-1);
    reset_ota_test();OTA_URL=ota_source_base(2);assert(!ota_fetch_manifest(err));assert(requests.size()==1);
    reset_ota_test();OTA_URL=ota_source_base(0)+"/";select_source(4);
    assert(g_ota_source==4);

    reset_ota_test();OTA_URL="https://private.test/ota/";
    responses["https://private.test/ota/manifest.json"]={200,manifest("sub/firmware.bin")};
    assert(ota_fetch_manifest(err));assert(g_ota_url=="https://private.test/ota/sub/firmware.bin");
    responses[g_ota_url]={200,"test"};requests.clear();
    assert(ota_flash_from_url(err));assert(requests.size()==1 && starts==1);

    reset_ota_test();select_source(2);
    const String pages=ota_source_base(0,g_ota_ver)+"/"+g_ota_file;
    responses[pages]={200,"test"};
    assert(ota_flash_from_url(err));assert(requests.size()==2 && requests[1]==pages);
    assert(g_ota_source==0 && checked_md5.size()==1);

    // 分别模拟接收中断、MD5 不匹配以及 Content-Length 不匹配。
    for (int mode : {1,3,5}) {
        reset_ota_test();select_source(0);
        responses[g_ota_url]={200,"test",mode==5 ? 5 : 4,mode};
        const String github=ota_source_base(1,g_ota_ver)+"/"+g_ota_file;
        responses[github]={200,"test"};
        assert(ota_flash_from_url(err));assert(requests.size()==2 && requests[1]==github);
        assert(starts==(mode==5 ? 1 : 2));assert(aborts==(mode==5 ? 0 : 1));
        for(const auto &md5:checked_md5)assert(md5=="098f6bcd4621d373cade4e832627b4f6");
    }

    reset_ota_test();select_source(0);responses[g_ota_url]={200,"test",4,2};
    assert(!ota_flash_from_url(err));assert(requests.size()==1 && aborts==1);
    reset_ota_test();select_source(0);responses[g_ota_url]={200,"test",4,4};
    assert(!ota_flash_from_url(err));assert(requests.size()==1 && starts==0);
    reset_ota_test();OTA_URL=ota_source_base(2);select_source(2);
    assert(!ota_flash_from_url(err));assert(requests.size()==1);

    reset_ota_test();busy=true;assert(!ota_fetch_manifest(err));assert(requests.empty());
    reset_ota_test();OTA_URL=ota_source_base(1);
    responses[OTA_URL+"/manifest.json"]={302,"",0,0,"https://asset.test/manifest.json"};
    responses["https://asset.test/manifest.json"]={200,manifest()};
    assert(ota_fetch_manifest(err));assert(requests.size()==2);
    responses["https://asset.test/manifest.json"]={302,"",0,0,"https://asset.test/manifest.json"};
    requests.clear();assert(!ota_fetch_manifest(err));assert(requests.size()==5);
    assert(g_ota_url.empty());
    std::puts("PASS: OTA channel fallback, fixed/custom sources, redirects, pinned version/MD5, interrupted downloads, flash/state failures");
}
