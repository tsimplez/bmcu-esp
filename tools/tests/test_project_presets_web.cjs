// 先用 test_project_presets.py --json 导出真实 GET 响应，再把 JSON 路径作为本脚本参数。
const { chromium } = require('playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

(async () => {
  assert(process.argv[2], '请提供 test_project_presets.py --json 导出的项目列表');
  const data = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
  const [direct, pcb] = data.projects;
  assert(direct.available && pcb.available);
  assert.equal(pcb.ams, 2);
  const browser = await chromium.launch({ channel: process.env.BROWSER_CHANNEL || 'msedge', headless: true });
  try {
    const page = await browser.newPage({ viewport: { width: 1100, height: 1000 } });
    const errors = [], posts = [];
    page.on('pageerror', error => errors.push(error.message));
    let current = data.current, maintenance = true, failSave = false;
    const pins = Array.from({ length: 66 }, (_, id) => ({ id, label: `IO${id}` }));
    const settings = { maxams: 4, motor_limit: 1, manual_ms: 300, auto_ms: 1000,
      cm: Array(16).fill(2), to: Array(16).fill(7), otaurl: '', otanotify: 0 };
    /** 将后端预览数据用作模拟设备重启后的设置，不在测试里重写项目接线表。 */
    const rebootWith = preset => {
      current = preset.id;
      Object.assign(settings, { ams: preset.ams, channels: preset.ams * 4, rs485: preset.rs485,
        bus: { rx: preset.rx, tx: preset.tx, de: preset.de, pins },
        pcf: { sda: preset.sda, scl: preset.scl, configured: preset.sda !== 255, pins },
        io: { pins, motor: Array.from({ length: 16 }, (_, i) => preset.pins[i]?.slice(0, 2) || [255, 255]),
          microswitch: Array.from({ length: 16 }, (_, i) => preset.pins[i]?.[2] ?? 255) } });
    };
    rebootWith(direct);
    await page.route('http://bmcu.test/**', async route => {
      const request = route.request(), url = new URL(request.url());
      if (url.pathname === '/') return route.fulfill({ contentType: 'text/html',
        body: fs.readFileSync(path.join(__dirname, '../../web/index.html'), 'utf8') });
      if (url.pathname === '/api/projects') {
        if (request.method() === 'POST') {
          const body = new URLSearchParams(request.postData()); posts.push(body);
          assert.deepEqual([...body.keys()], ['id'], '整套项目必须只提交 ID');
          if (failSave) return route.fulfill({ status: 500, json: { ok: false, msg: '保存或回读校验失败' } });
          rebootWith(data.projects.find(p => p.id === body.get('id')));
          return route.fulfill({ json: { ok: true, msg: '项目已保存，设备正在重启' } });
        }
        return route.fulfill({ json: { ...data, current } });
      }
      if (url.pathname === '/api/settings') return route.fulfill({ json: settings });
      if (url.pathname === '/api/status') return route.fulfill({ json: {
        ...settings, project: current, maintenance, chip: 'ESP32-C3', mode: 'STA', ip: '192.0.2.1',
        printer: 0, uptime: 10, heap: 100000, heap_total: 320000, motors: [], fl: [],
        rssi: -55, mac: '02:00:00:00:00:01', motor_running: 0, version: '测试固件', target: 'esp32c3',
        i2c: { configured: settings.pcf.configured, pcf: settings.pcf.configured } } });
      return route.fulfill({ json: {} });
    });
    await page.goto('http://bmcu.test/');
    await page.waitForFunction(() => PROJECT_LOADED && statusAt > 0 && IO_CHANNELS === 4);
    assert.equal(await page.inputValue('#project-select'), direct.id);
    assert.equal(await page.locator('#project-preview').isVisible(), false, '加载页面不自动弹窗');
    assert.equal(await page.locator('header #apply-project').count(), 0);
    assert.equal(await page.locator('#project-preview #apply-project').count(), 1);
    assert.equal(await page.locator('#apply-project').isDisabled(), true);
    await page.selectOption('#project-select', pcb.id);
    assert.equal(await page.locator('#project-preview').isVisible(), true, '选择项目弹出接线预览');
    await page.click('#cancel-project');
    assert.equal(await page.locator('#project-preview').isVisible(), false);
    assert.equal(await page.inputValue('#project-select'), direct.id, '取消恢复实际项目');
    assert.equal(await page.evaluate(() => document.activeElement.id), 'project-select');
    await page.selectOption('#project-select', pcb.id);
    await page.keyboard.press('Escape');
    assert.equal(await page.locator('#project-preview').isVisible(), false);
    assert.equal(await page.inputValue('#project-select'), direct.id, 'Esc 取消选择');
    assert.equal(await page.evaluate(() => document.body.classList.contains('project-open')), false);
    await page.selectOption('#project-select', pcb.id);
    assert.equal(posts.length, 0, '选择项目只预览，不写入');
    assert.equal(await page.locator('#project-pins tr').count(), 8);
    assert.deepEqual(await page.locator('#project-pins tr').first().locator('td').allTextContents(),
      ['1', 'PCF8575 0x20 P0', 'PCF8575 0x20 P4', 'PCF8575 0x20 P12']);
    for (let row = 4; row < 8; ++row)
      assert.deepEqual(await page.locator('#project-pins tr').nth(row).locator('td').allTextContents(),
        [String(row + 1), '无', '无', '无']);
    assert.match(await page.locator('#project-summary').innerText(), /SDA IO4 \/ SCL IO5/);
    await page.evaluate(() => refreshStatus());
    assert.equal(await page.inputValue('#project-select'), pcb.id, '轮询保留待应用选择');
    maintenance = false;
    await page.waitForFunction(() => S.maintenance === false);
    assert.equal(await page.locator('#apply-project').isDisabled(), true);
    maintenance = true;
    await page.waitForFunction(() => S.maintenance === true);
    failSave = true;
    await page.click('#apply-project');
    await page.waitForFunction(() => !PROJECT_SAVING);
    assert.equal(current, direct.id);
    assert.equal(await page.inputValue('#project-select'), pcb.id, '保存失败保留选择');
    assert.equal(await page.locator('#project-preview').isVisible(), true, '失败留在弹窗内重试');
    assert.match(await page.locator('#project-state').innerText(), /失败/);
    failSave = false;
    // 同步触发两次点击，只能发送一个成功请求。
    await page.evaluate(() => { applyProject(); applyProject(); });
    await page.waitForFunction(() => PROJECT_SAVING);
    assert.equal(posts.length, 2, '一次失败加一次成功，不重复提交');
    await page.keyboard.press('Escape');
    assert.equal(await page.locator('#project-preview').isVisible(), true, '保存及重启期间不能关闭弹窗');
    assert.equal(await page.locator('#cancel-project').isDisabled(), true);
    await page.waitForFunction(id => PROJECT_CURRENT === id && !PROJECT_SAVING && IO_CHANNELS === 8, pcb.id);
    assert.equal(await page.inputValue('#project-select'), pcb.id);
    assert.equal(settings.pcf.configured, true);
    assert.equal(await page.locator('#project-preview').isVisible(), false, '重启加载后不自动弹窗');
    await page.selectOption('#project-select', pcb.id);
    if (process.env.WEB_TEST_SCREENSHOT_DIR) {
      await page.screenshot({ path: path.join(process.env.WEB_TEST_SCREENSHOT_DIR, 'bmcu-project-desktop.png'), fullPage: true });
    }
    await page.setViewportSize({ width: 390, height: 844 });
    assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), '手机页面不得横向溢出');
    const modal = await page.locator('#project-preview').boundingBox();
    assert(modal.x >= 0 && modal.y >= 0 && modal.x + modal.width <= 390 && modal.y + modal.height <= 844);
    const action = await page.locator('#apply-project').boundingBox();
    assert(action.y >= 0 && action.y + action.height <= 844, '手机上始终可见弹窗操作按钮');
    if (process.env.WEB_TEST_SCREENSHOT_DIR) {
      await page.screenshot({ path: path.join(process.env.WEB_TEST_SCREENSHOT_DIR, 'bmcu-project-mobile.png'), fullPage: true });
    }
    await page.click('#cancel-project');
    await page.selectOption('#project-select', direct.id);
    await page.click('#apply-project');
    await page.waitForFunction(id => PROJECT_CURRENT === id && !PROJECT_SAVING && IO_CHANNELS === 4, direct.id);
    assert.equal(settings.pcf.configured, false);
    assert.deepEqual(settings.io.motor.slice(4), Array.from({ length: 12 }, () => [255, 255]));
    current = '';
    await page.waitForFunction(() => PROJECT_CURRENT === '');
    assert.equal(await page.inputValue('#project-select'), '', '手动配置不匹配时显示自定义');
    assert.deepEqual(errors, []);
    console.log('PASS: modal preview, cancel/Esc, in-dialog apply, reload recognition, failure, busy, custom, mobile layout');
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
