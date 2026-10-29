// 使用真实页面和模拟设备检查日志收集、虚拟滚动及长时间覆盖；不连接硬件。
const { chromium } = require('playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

/**
 * 运行日志网页回归与浏览器性能检查
 * 可用 WEB_LOG_BASELINE 指定旧网页，以真实旧渲染函数做可选对比
 */
(async () => {
  const browser = await chromium.launch({ channel: process.env.BROWSER_CHANNEL || 'msedge', headless: true });
  try {
    const page = await browser.newPage({ viewport: { width: 1000, height: 1000 }, acceptDownloads: true });
    const errors = [], requests = [];
    page.on('pageerror', error => errors.push(error.message));
    let boot = 'boot-a', uptime = 10000, chunk = 8192, lag = 0, malformed = false, fail = false;
    let buffers = [Buffer.alloc(0), Buffer.alloc(0)], floors = [0, 0], inFlight = 0, maxInFlight = 0;
    const settings = { ams: 1, channels: 4, maxams: 4, maintenance: true, printer: 0, mode: 'STA',
      cm: Array(16).fill(2), to: Array(16).fill(7), motor_limit: 1, manual_ms: 300, auto_ms: 1000,
      serial: '', ams_version: '90.0.0.0', otaurl: '', otanotify: 0, motors: [], fl: [], uptime: 10,
      heap: 100000, heap_total: 200000, pcf: { sda: 255, scl: 255, pins: [] },
      io: { motor: Array.from({ length: 16 }, () => [255, 255]), microswitch: Array(16).fill(255), pins: [] } };
    await page.route('http://bmcu.test/**', async route => {
      const url = new URL(route.request().url());
      if (url.pathname === '/') return route.fulfill({ contentType: 'text/html', body: fs.readFileSync(path.join(__dirname, '../../web/index.html'), 'utf8') });
      if (url.pathname !== '/api/log') return route.fulfill({ json: url.pathname === '/api/projects' ? { projects: [] } : settings });
      requests.push(url);inFlight++;maxInFlight = Math.max(maxInFlight, inFlight);
      assert.equal(url.searchParams.get('both'), '1', '全部日志读取必须通过合并接口');
      const reset = url.searchParams.has('boot') && url.searchParams.get('boot') !== boot;
      const mask = Number(url.searchParams.get('clear_mask') || 0);
      const parts = [0, 1].map(ch => {
        if (mask & (1 << ch)) floors[ch] = buffers[ch].length;
        const seq = mask & (1 << ch) ? floors[ch] : reset ? 0 : Number(url.searchParams.get(ch ? 'bus_seq' : 'main_seq'));
        const start = Math.max(floors[ch], Math.min(seq, buffers[ch].length));
        const data = buffers[ch].subarray(start, start + chunk);
        return { data, seq: start + data.length, total: buffers[ch].length, dropped: Math.max(0, floors[ch] - seq) };
      });
      const headers = { 'X-Log-Boot': boot, 'X-Uptime-Ms': String(uptime), 'X-Log-Boundary': String(buffers[1].length) };
      for (let ch = 0; ch < 2; ch++) {
        const prefix = ch ? 'X-Log-Bus-' : 'X-Log-Main-';
        for (const key of ['seq', 'total', 'dropped']) headers[prefix + key] = String(parts[ch][key]);
        headers[prefix + 'Length'] = String(parts[ch].data.length + (malformed && ch === 0 ? 1 : 0));
      }
      const body = Buffer.concat(parts.map(part => part.data)), status = fail ? 503 : 200;
      if (lag) await new Promise(resolve => setTimeout(resolve, lag));
      try { await route.fulfill({ status, contentType: 'application/octet-stream', headers, body }); }
      finally { inFlight--; }
    });
    await page.goto('http://bmcu.test/');
    await page.waitForFunction(() => typeof LOGS !== 'undefined');
    const client = await page.context().newCDPSession(page);

    /**
     * 等待当前请求结束后主动执行一次统一轮询，防止定时器影响断言
     */
    const poll = async () => {
      await page.waitForFunction(() => !logPollBusy);
      await page.evaluate(async () => { clearTimeout(logPollTimer);logPollTimer = null;await pollLogs();clearTimeout(logPollTimer);logPollTimer = null; });
    };
    /**
     * 取得指定通道的全部已组装文本
     * @param {number} ch 通道编号
     * @return {Promise<string>} 浏览器缓存文本
     */
    const cached = ch => page.evaluate(ch => { const L = LOGS[ch], lines = [];for (let id = L.head; id < L.next; id++) lines.push(L.get(id).text);return lines.join('\n'); }, ch);

    buffers = [Buffer.from('[10000] 中文主板日志\n'), Buffer.from('[10000] type=0 RX 01 02\n')];
    chunk = 11;
    await page.click('[data-tab="logs"]');
    await page.waitForFunction(() => LOGS[0].seq > 0);
    for (let i = 0; i < 4; i++) await poll();
    assert.match(await cached(0), /中文主板日志/);
    assert.ok(!(await cached(0)).includes('\uFFFD'), '跨响应的中文必须完整');
    assert.match(await cached(1), /RX 01 02/);
    assert.equal(maxInFlight, 1);
    chunk = 8192;

    // 不完整响应必须保持两路游标，下次从同一位置补读。
    const beforeFailure = await page.evaluate(() => LOGS.map(L => L.seq));
    buffers[0] = Buffer.concat([buffers[0], Buffer.from('[10001] 重试后只出现一次\n')]);
    malformed = true;await poll();
    assert.deepEqual(await page.evaluate(() => LOGS.map(L => L.seq)), beforeFailure);
    malformed = false;fail = true;await poll();
    assert.deepEqual(await page.evaluate(() => LOGS.map(L => L.seq)), beforeFailure);
    fail = false;await poll();
    assert.equal((await cached(0)).match(/重试后只出现一次/g).length, 1);

    // 暂停和切到其它功能页仍收集，不刷新当前日志节点。
    await page.evaluate(() => { showLog(1);togglePause(); });
    const frozen = await page.locator('#log1').textContent();
    buffers[1] = Buffer.concat([buffers[1], Buffer.from('[10002] type=1 TX 暂停期间\n')]);
    await poll();assert.equal(await page.locator('#log1').textContent(), frozen);
    await page.evaluate(() => { togglePause();showTab('status'); });
    buffers[0] = Buffer.concat([buffers[0], Buffer.from('[10003] 离开日志页仍收集\n')]);
    await poll();assert.match(await cached(0), /离开日志页仍收集/);
    await page.evaluate(() => showTab('logs'));

    // 清空和在途响应交错时，仅清空目标通道，旧响应不能把它填回。
    lag = 250;
    buffers[1] = Buffer.concat([buffers[1], Buffer.from('[10004] type=0 RX 旧响应\n')]);
    const pendingPoll = poll();
    await page.waitForFunction(() => logPollBusy);
    await page.evaluate(() => { showLog(1);clearLog(); });
    await pendingPoll;lag = 0;await poll();
    assert.equal(await cached(1), '');
    assert.match(await cached(0), /中文主板日志/);
    buffers[1] = Buffer.concat([buffers[1], Buffer.from('[10005] type=1 RX 清空后\n')]);
    await poll();assert.match(await cached(1), /清空后/);assert.ok(!(await cached(1)).includes('旧响应'));

    // 设备重启保留历史，尾行不能与新启动周期拼接，旧时间保持不变。
    const oldText = await cached(0);
    buffers[0] = Buffer.concat([buffers[0], Buffer.from('[10006] 半行')]);await poll();
    boot = 'boot-b';uptime = 100;buffers = [Buffer.from('[100] 新启动\n'), Buffer.from('[100] type=0 RX 重启\n')];floors = [0, 0];
    await poll();assert.ok((await cached(0)).startsWith(oldText));assert.match(await cached(0), /设备已重启/);
    assert.match(await cached(0), /新启动/);assert.ok(!(await cached(0)).includes('半行'));

    // 抓取共享轮询游标，排除开始前和截止后的记录，固定开始时的类型筛选。
    await page.evaluate(() => { showLog(1);setAllBusLogFilters(false);busLogFilters[1] = true;startBusCapture();busLogFilters[1] = false; });
    await poll();
    buffers[1] = Buffer.concat([buffers[1], Buffer.from('[101] type=1 TX 抓取目标\n[102] type=0 RX 排除类型\n[30101] type=1 RX 截止之后\n')]);
    await poll();
    await page.evaluate(() => { busCapture.until = Date.now() - 1; });
    const captureDownload = page.waitForEvent('download');await poll();
    const captureText = fs.readFileSync(await (await captureDownload).path(), 'utf8');
    assert.match(captureText, /抓取目标/);assert.ok(!captureText.includes('排除类型'));assert.ok(!captureText.includes('截止之后'));assert.ok(!captureText.includes('重启'));

    // 长尾行、异常超长记录和 HTML 文本不能破坏缓存或成为可执行 DOM。
    const partial = await page.evaluate(() => {
      const L = new LogRing(0);appendLogBytes(L, logEncoder.encode('x'.repeat(LOG_PENDING_MAX + 1)), LOG_PENDING_MAX + 1);
      appendLogBytes(L, logEncoder.encode('尾行\n[100] <img src=x onerror=alert(1)>\n'), LOG_PENDING_MAX + 60);
      return { pending: L.pending.length, count: L.next, text: L.get(L.next - 1).text, bytes: L.bytes };
    });
    assert.equal(partial.pending, 0);assert.equal(partial.count, 2);assert.match(partial.text, /<img/);
    const wrapTime = await page.evaluate(() => {
      const oldOffset = logClockOffset, oldUptime = logClockUptime;
      logClockOffset = new Date(2026, 0, 1, 12, 0, 0, 5).getTime() - 5;logClockUptime = 5;
      const value = fmtLogLine('[4294967290] 回绕前');logClockOffset = oldOffset;logClockUptime = oldUptime;return value;
    });
    assert.equal(wrapTime, '[11:59:59.994] 回绕前');

    // 写入远超两路容量、经过物理槽位绕回，检查字节上限和索引。
    await page.evaluate(() => { clearTimeout(logPollTimer);logStarted = false;setAllBusLogFilters(true);LOGS.forEach(L => L.clear()); });
    /**
     * 向真实缓存连续写入模拟日志，并周期性更新可视区
     * @param {number} rounds 每批约 20 KiB 的批次数
     * @return {Promise<Object>} 容量与渲染耗时
     */
    const stress = rounds => page.evaluate(rounds => {
      const samples = [];
      for (let batch = 0; batch < rounds; batch++) {
        for (let ch = 0; ch < 2; ch++) {
          const L = LOGS[ch], text = Array.from({ length: 128 }, (_, i) => '[1000] type=' + (i % 2) + ' RX ' + (ch ? '01 23 45 67 89 AB CD EF '.repeat(5) : '中文主板测试 '.repeat(8)) + batch + ':' + i + '\n').join('');
          const bytes = logEncoder.encode(text);L.seq = (L.seq + bytes.length) >>> 0;appendLogBytes(L, bytes, L.seq);
        }
        const start = performance.now();renderLogViewport(LOGS[1]);samples.push(performance.now() - start);
      }
      samples.sort((a, b) => a - b);
      return { channels: LOGS.map(L => ({ bytes: L.bytes, capacity: L.capacity, head: L.head, next: L.next, nodes: L.nodes.size })),
        p50: samples[Math.floor(samples.length / 2)], p95: samples[Math.floor(samples.length * 0.95)] };
    }, rounds);
    const firstStress = await stress(550);
    for (const channel of firstStress.channels) {
      assert.ok(channel.bytes <= channel.capacity);assert.ok(channel.bytes > channel.capacity * 0.99);
      assert.ok(channel.head > 0);assert.ok(channel.next > 65536);assert.ok(channel.nodes <= 256);
    }
    await client.send('HeapProfiler.collectGarbage');
    const firstHeap = await client.send('Runtime.getHeapUsage');
    const secondStress = await stress(150);
    await client.send('HeapProfiler.collectGarbage');
    const secondHeap = await client.send('Runtime.getHeapUsage');
    assert.ok(secondHeap.usedSize - firstHeap.usedSize < 8 * 1048576, '填满后的堆内存不得随累计日志持续增长');

    // 顶部、中段、底部的像素索引与节点位置必须正确，筛选后的旧记录仍能翻阅。
    const layout = await page.evaluate(() => {
      const L = LOGS[1], errors = [];
      for (const id of [L.head, L.head + 10, Math.floor((L.head + L.next) / 2), L.next - 1]) {
        const rec = L.get(id);if (L.atPixel(L.offset(id) + rec.height / 2).id !== id) errors.push(id);
      }
      L.follow = false;L.anchor = L.head;L.anchorDelta = 0;renderLogViewport(L);
      return { errors, firstRendered: Number(L.space.firstElementChild.dataset.logId), oldest: L.head, count: L.nodes.size };
    });
    assert.deepEqual(layout.errors, []);assert.equal(layout.firstRendered, layout.oldest);assert.ok(layout.count < 256);
    await page.evaluate(() => { busLogFilters[0] = false;renderBusLog();renderLogViewport(LOGS[1]); });
    assert.equal(await page.evaluate(() => [...LOGS[1].nodes.keys()].every(id => LOGS[1].get(id).type === 1)), true);
    await page.evaluate(() => { setAllBusLogFilters(false);renderLogViewport(LOGS[1]); });
    assert.equal(await page.locator('#log1 .logline').count(), 0);
    await page.evaluate(() => { busLogFilters[1] = true;renderBusLog();renderLogViewport(LOGS[1]); });
    const allDownload = page.waitForEvent('download');await page.evaluate(() => downloadLog());
    const allText = fs.readFileSync(await (await allDownload).path(), 'utf8');
    assert.ok(Buffer.byteLength(allText) > 1048576);assert.match(allText, /type=0/);assert.match(allText, /type=1/);

    // 实际滚动及手机宽度下的换行不能产生节点重叠或超宽。
    await page.setViewportSize({ width: 390, height: 844 });
    await page.evaluate(() => { LOGS[1].follow = true;requestLogRender(); });
    await page.waitForTimeout(120);
    assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
    const overlap = await page.evaluate(() => {
      const nodes = [...LOGS[1].nodes.values()].sort((a, b) => Number(a.dataset.logId) - Number(b.dataset.logId));
      return nodes.some((node, i) => i && nodes[i - 1].getBoundingClientRect().bottom > node.getBoundingClientRect().top + 1);
    });
    assert.equal(overlap, false, '换行后的虚拟记录不得重叠');
    if (process.env.WEB_LOG_SCREENSHOT) await page.screenshot({ path: process.env.WEB_LOG_SCREENSHOT, fullPage: true });

    let baseline = null;
    if (process.env.WEB_LOG_BASELINE) {
      await page.setViewportSize({ width: 1000, height: 1000 });
      const old = fs.readFileSync(process.env.WEB_LOG_BASELINE, 'utf8');
      const start = old.indexOf('const renderBusLog=()=>{'), end = old.indexOf('/**', start);
      assert.ok(start >= 0 && end > start);
      baseline = await page.evaluate(source => {
        const el = document.createElement('pre');el.className = 'logbox';el.style.width = LOGS[1].el.getBoundingClientRect().width + 'px';document.body.appendChild(el);
        const text = '[1000] type=0 RX ' + '01 23 45 67 89 AB CD EF '.repeat(3) + '\n';
        const records = Array.from({ length: Math.floor(262144 / text.length) }, () => ({ type: 0, text }));
        const variants = { original: source, noColor: source.replace(/line.style.color=.*\n/, ''), noFilter: source.replace(/if\(busLogFilters\[rec.type\]===false\) continue/, '') };
        const result = {}, L = LOGS[1];setAllBusLogFilters(true);L.clear();L.follow = true;
        const bytes = logEncoder.encode(text.repeat(records.length));appendLogBytes(L, bytes, bytes.length);renderLogViewport(L);
        const current = [];
        for (let i = 0; i < 12; i++) { const start = performance.now();renderLogViewport(L);current.push(performance.now() - start); }
        current.sort((a, b) => a - b);result.optimized = { p50: current[6], p95: current[11], nodes: L.nodes.size };
        for (const [name, code] of Object.entries(variants)) {
          const render = new Function('LOGS', '$', 'busLogFilters', 'busLogColors', 'fmtLogLine', 'activeLog', code + ';return renderBusLog')(
            [{}, { records }], () => el, {}, busLogColors, fmtLogLine, 1);
          const times = [];
          for (let i = 0; i < 12; i++) { const start = performance.now();render();times.push(performance.now() - start); }
          times.sort((a, b) => a - b);result[name] = { p50: times[6], p95: times[11], nodes: el.childElementCount };
        }
        el.remove();return result;
      }, old.slice(start, end));
    }
    assert.deepEqual(errors, []);assert.equal(maxInFlight, 1);
    console.log('PASS: 合并请求、UTF-8 尾行、失败重试、暂停/切页、清空竞态、重启、抓取、双环覆盖、虚拟滚动、筛选、下载、手机布局');
    console.log(JSON.stringify({ firstStress, secondStress, heap: { before: firstHeap.usedSize, after: secondHeap.usedSize }, baseline }, null, 2));
  } finally { await browser.close(); }
})().catch(error => { console.error(error);process.exitCode = 1; });
