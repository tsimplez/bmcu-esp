// Run with Node.js and Playwright installed; uses mocked device APIs, no hardware.
const { chromium } = require('playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

(async () => {
  const browser = await chromium.launch({ channel: process.env.BROWSER_CHANNEL || 'msedge', headless: true });
  try {
    const page = await browser.newPage({ viewport: { width: 1000, height: 1100 } });
    const errors = [];
    page.on('pageerror', e => errors.push(e.message));
    const pins = Array.from({ length: 48 }, (_, id) => ({ id, label: `IO${id}` }));
    const gpioPins = pins.slice();
    const pcfIds = [];
    for (const [device, banks] of [[50, 60], [70, 80], [90, 100], [110, 120]].entries()) {
      for (const [bank, base] of banks.entries()) {
        for (let bit = 0; bit < 8; ++bit) {
          const id = base + bit;
          pcfIds.push(id);
          pins.push({ id, label: `${id}（io扩展${device + 1} P${bank * 10 + bit}）` });
        }
      }
    }
    const i2cStatus = { configured: false, pcf: false, pcf_devices: [
      { address: 0x23, online: true }, { address: 0x20, online: false },
      { address: 0x22, online: false }, { address: 0x21, online: true },
    ] };
    const settings = {
      ams: 1, channels: 4, maxams: 4, motor_limit: 1, manual_ms: 300, auto_ms: 1000,
      cm: Array(16).fill(2), to: Array(16).fill(7), serial: '', ams_version: '90.0.0.0',
      otaurl: '', otanotify: 0,
      io: {
        motor: Array.from({ length: 16 }, (_, i) => i < 4 ? [i * 2, i * 2 + 1] : [255, 255]),
        microswitch: Array(16).fill(255), pins,
      },
      pcf: { configured: true, sda: 46, scl: 47, pins: gpioPins },
    };
    const posts = [];
    let failSave = false;
    let failOta = false, otaChecks = 0;
    await page.route('http://bmcu.test/**', async route => {
      const req = route.request();
      const url = new URL(req.url());
      if (url.pathname === '/') {
        return route.fulfill({ contentType: 'text/html', body: fs.readFileSync(path.join(__dirname, '../../web/index.html'), 'utf8') });
      }
      let data = {};
      if (url.pathname === '/api/settings' && req.method() === 'POST') {
        const body = new URLSearchParams(req.postData());
        posts.push(body);
        if (failSave) return route.fulfill({ status: 500, json: { ok: false, msg: '保存失败，请重试' } });
        if (body.has('ams')) {
          settings.ams = Number(body.get('ams'));
          settings.channels = settings.ams * 4;
          for (let i = 0; i < settings.channels; i++) {
            settings.io.motor[i] = [Number(body.get('io_mf' + i)), Number(body.get('io_mb' + i))];
            settings.io.microswitch[i] = Number(body.get('io_sw' + i));
          }
        }
        if (body.has('serial')) settings.serial = body.get('serial');
        if (body.has('ams_version')) settings.ams_version = body.get('ams_version');
        if (body.has('otaurl')) settings.otaurl = body.get('otaurl');
        data = { ok: true, msg: '参数已保存' };
      } else if (url.pathname === '/api/settings') data = settings;
      else if (url.pathname === '/api/ota/check') {
        otaChecks++;
        data = failOta ? { ok: false, msg: '自动渠道检查失败' } : {
          ok: true, current: '26.10.25', latest: '26.10.28', has_update: true, source: 'ghfast.top', size: 1600000,
        };
      }
      else if (url.pathname === '/api/status') data = {
        ...settings, mode: 'STA', ip: '192.0.2.1', chip: 'ESP32-C3', rs485: 1,
        maintenance: true, printer: 0, motors: [], fl: [], uptime: 10, heap: 102400, heap_total: 204800,
        i2c: i2cStatus,
      };
      await route.fulfill({ json: data });
    });
    await page.goto('http://bmcu.test/');
    await page.waitForFunction(() => document.querySelectorAll('#iobody tr').length === 4);
    await page.click('[data-tab="settings"]');
    await page.waitForFunction(() => document.querySelector('#p-ams').value === '1');
    // Wait for the tab's settings read before editing.
    await page.evaluate(() => loadPara());
    await page.fill('#p-serial', '123456789ABC');
    assert.equal(await page.evaluate(() => document.querySelector('#p-serial').checkValidity()), true);
    await page.fill('#p-serial', '123456789AB');
    assert.equal(await page.evaluate(() => document.querySelector('#p-serial').checkValidity()), false);
    await page.fill('#p-serial', '123456789ABG');
    assert.equal(await page.evaluate(() => document.querySelector('#p-serial').checkValidity()), false);
    await page.fill('#p-serial', '123456789ABC');
    await page.evaluate(() => saveSerial());
    await page.waitForFunction(() => !document.querySelector('#p-serial').disabled);
    assert.equal(settings.serial, '123456789ABC');
    assert.equal(posts.at(-1).get('serial'), '123456789ABC');
    await page.fill('#p-serial', '');
    const versionPosts = posts.length;
    await page.fill('#p-ams-version', '90.1.256.0');
    await page.click('#save-ams-version');
    await page.waitForTimeout(50);
    assert.equal(posts.length, versionPosts, 'invalid AMS version must not be submitted');
    await page.fill('#p-ams-version', '90.1.2.3');
    await page.click('#save-ams-version');
    await page.waitForFunction(() => !document.querySelector('#p-ams-version').disabled);
    assert.equal(settings.ams_version, '90.1.2.3');
    assert.equal(posts.at(-1).get('ams_version'), '90.1.2.3');
    assert.equal(await page.locator('#t-settings #iobody').count(), 1);
    assert.equal(await page.locator('#t-status #iobody').count(), 0);
    assert.equal(await page.locator('#t-settings #card-pcf').count(), 1);
    for (const count of [2, 3, 4, 1, 2]) {
      await page.selectOption('#p-ams', String(count));
      assert.equal(await page.locator('#iobody tr').count(), count * 4);
    }
    assert.equal(await page.locator('#chbody tr').count(), 4, 'preview must not enable manual controls');
    for (let i = 4; i < 8; i++) {
      await page.selectOption('#io-mf' + i, String(i * 2));
      await page.selectOption('#io-mb' + i, String(i * 2 + 1));
    }
    const ioPosts = posts.length;
    await page.selectOption('#io-mf4', '0');
    await page.click('#save-io');
    assert.equal(posts.length, ioPosts, 'duplicate pins must not be submitted');
    await page.selectOption('#io-mf4', '8');
    await page.selectOption('#p-ams', '1');
    await page.selectOption('#p-ams', '2');
    assert.equal(await page.inputValue('#io-mf4'), '8', 'draft must survive count changes');
    await page.click('[data-tab="status"]');
    await page.click('[data-tab="settings"]');
    assert.equal(await page.inputValue('#p-ams'), '2', 'draft must survive tab changes');
    failSave = true;
    await page.click('#save-io');
    await page.waitForFunction(() => !document.querySelector('#save-io').disabled);
    assert.equal(settings.ams, 1);
    assert.equal(await page.inputValue('#io-mf4'), '8', 'save failure must preserve edits');
    failSave = false;
    await page.click('#save-io');
    await page.waitForFunction(() => document.querySelectorAll('#chbody tr').length === 8);
    assert.equal(settings.ams, 2);
    assert.equal(posts.at(-1).get('io_mb7'), '15');
    assert.equal(posts.at(-1).get('ams'), '2');
    assert.equal(await page.locator('#motor-limit option').count(), 8);
    await page.selectOption('#p-ams', '1');
    await page.click('#save-io');
    await page.waitForFunction(() => document.querySelectorAll('#chbody tr').length === 4);
    assert.equal(posts.at(-1).has('io_mf4'), false, 'disabled channels must not be submitted');
    await page.selectOption('#p-ams', '2');
    assert.equal(await page.inputValue('#io-mf4'), '8', 'disabled channel mappings must survive save/reload');
    // OTA 渠道复用地址设置；保存后重新加载应保持选择，失败保留服务器原设置。
    await page.click('[data-tab="update"]');
    await page.evaluate(() => loadPara());
    assert.equal(await page.inputValue('#u-channel'), 'custom');
    const sources = {
      auto: 'https://tsimplez.github.io/bmcu-esp',
      github: 'https://github.com/tsimplez/bmcu-esp/releases/latest/download',
      ghfast: 'https://ghfast.top/https://github.com/tsimplez/bmcu-esp/releases/latest/download',
      ghproxy: 'https://gh-proxy.com/https://github.com/tsimplez/bmcu-esp/releases/latest/download',
      llkk: 'https://gh.llkk.cc/https://github.com/tsimplez/bmcu-esp/releases/latest/download',
    };
    for (const [channel, url] of Object.entries(sources)) {
      await page.selectOption('#u-channel', channel);
      assert.equal(await page.inputValue('#u-otaurl'), url);
      assert.equal(await page.evaluate(() => document.querySelector('#u-otaurl').readOnly), true);
      assert.equal(await page.evaluate(() => saveOtaSrc()), true);
      assert.equal(posts.at(-1).get('otaurl'), url);
      await page.evaluate(() => loadPara());
      assert.equal(await page.inputValue('#u-channel'), channel);
    }
    await page.selectOption('#u-channel', 'custom');
    await page.fill('#u-otaurl', 'https://example.test/my-ota');
    failSave = true;
    assert.equal(await page.evaluate(() => saveOtaSrc()), false);
    assert.equal(settings.otaurl, sources.llkk);
    failSave = false;
    assert.equal(await page.evaluate(() => saveOtaSrc()), true);
    await page.evaluate(() => loadPara());
    assert.equal(await page.inputValue('#u-channel'), 'custom');
    assert.equal(await page.inputValue('#u-otaurl'), 'https://example.test/my-ota');
    await page.evaluate(() => { checkUpdate(1); checkUpdate(1); });
    await page.waitForFunction(() => !otaChecking);
    assert.equal(otaChecks, 1, 'duplicate checks must be suppressed');
    assert.match(await page.locator('#u-source').innerText(), /ghfast.top/);
    assert.equal(await page.locator('#btn-otapull').isEnabled(), true);
    failOta = true;
    await page.evaluate(() => checkUpdate(1));
    await page.waitForFunction(() => !otaChecking);
    assert.match(await page.locator('#u-stat').innerText(), /检查失败/);
    assert.equal(await page.locator('#btn-otapull').isDisabled(), true, 'failure must invalidate old update');

    // 四片按地址显示状态，数组顺序和首片缺失不应影响其他芯片。
    await page.evaluate(() => refreshStatus());
    for (const id of ['s-pcf', 's-pcf-1', 's-pcf-2', 's-pcf-3'])
      assert.equal(await page.locator('#' + id).textContent(), '未配置');
    i2cStatus.configured = true;
    await page.evaluate(() => refreshStatus());
    for (const [id, expected] of [['s-pcf', '未检测到'], ['s-pcf-1', '已检测到'], ['s-pcf-2', '未检测到'], ['s-pcf-3', '已检测到']])
      assert.equal(await page.locator('#' + id).textContent(), expected);

    await page.reload();
    await page.waitForFunction(() => document.querySelectorAll('#iobody tr').length === 4);
    await page.click('[data-tab="settings"]');
    await page.evaluate(() => loadPara());
    assert.deepEqual(await page.locator('#io-mf0 option').evaluateAll(options =>
      options.map(option => Number(option.value)).filter(pin => pin >= 50 && pin !== 255)), pcfIds);
    assert.equal(pcfIds.length, 64);
    assert.match(await page.locator('#io-mf0 option[value="60"]').textContent(), /60（io扩展1 P10）/);
    assert.match(await page.locator('#io-mf0 option[value="127"]').textContent(), /127（io扩展4 P17）/);
    for (let ch = 0; ch < 4; ++ch) {
      await page.selectOption('#io-mf' + ch, String(50 + ch * 20));
      await page.selectOption('#io-mb' + ch, String(51 + ch * 20));
      await page.selectOption('#io-sw' + ch, String(67 + ch * 20));
    }
    const beforePcfSave = posts.length;
    await page.selectOption('#io-mf1', '50');
    await page.evaluate(() => saveIoConfig());
    assert.equal(posts.length, beforePcfSave, '同一扩展脚重复使用仍须拒绝');
    await page.selectOption('#io-mf1', '70');
    await page.evaluate(() => saveIoConfig());
    assert.equal(posts.at(-1).get('io_sw3'), '127');
    await page.evaluate(() => loadPara());
    for (let ch = 0; ch < 4; ++ch) {
      assert.equal(await page.inputValue('#io-mf' + ch), String(50 + ch * 20));
      assert.equal(await page.inputValue('#io-sw' + ch), String(67 + ch * 20));
    }
    await page.setViewportSize({ width: 390, height: 844 });
    assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true, 'mobile layout must fit');
    await page.click('[data-tab="status"]');
    assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true, '四片状态必须适配手机宽度');
    if (process.env.WEB_TEST_SCREENSHOT) await page.screenshot({ path: process.env.WEB_TEST_SCREENSHOT, fullPage: true });
    assert.deepEqual(errors, []);
    console.log('PASS: settings, channel persistence, four PCF devices/status/pins/save, OTA source display/failure, duplicate checks, mobile layout');
  } finally {
    await browser.close();
  }
})().catch(e => { console.error(e); process.exitCode = 1; });
