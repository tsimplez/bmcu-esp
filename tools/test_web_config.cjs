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
    const settings = {
      ams: 1, channels: 4, maxams: 4, motor_limit: 1, manual_ms: 300, auto_ms: 1000,
      cm: Array(16).fill(2), to: Array(16).fill(7), serial: '', otaurl: '', otanotify: 0,
      io: {
        motor: Array.from({ length: 16 }, (_, i) => i < 4 ? [i * 2, i * 2 + 1] : [255, 255]),
        microswitch: Array(16).fill(255), pins,
      },
      pcf: { supported: true, sda: 46, scl: 47, pins },
    };
    const posts = [];
    let failSave = false;
    await page.route('http://bmcu.test/**', async route => {
      const req = route.request();
      const url = new URL(req.url());
      if (url.pathname === '/') {
        return route.fulfill({ contentType: 'text/html', body: fs.readFileSync(path.join(__dirname, '../web/index.html'), 'utf8') });
      }
      let data = {};
      if (url.pathname === '/api/settings' && req.method() === 'POST') {
        const body = new URLSearchParams(req.postData());
        posts.push(body);
        if (failSave) return route.fulfill({ status: 500, json: { ok: false, msg: '保存失败，请重试' } });
        settings.ams = Number(body.get('ams'));
        settings.channels = settings.ams * 4;
        for (let i = 0; i < settings.channels; i++) {
          settings.io.motor[i] = [Number(body.get('io_mf' + i)), Number(body.get('io_mb' + i))];
          settings.io.microswitch[i] = Number(body.get('io_sw' + i));
        }
        data = { ok: true, msg: '参数已保存' };
      } else if (url.pathname === '/api/settings') data = settings;
      else if (url.pathname === '/api/status') data = {
        ...settings, mode: 'STA', ip: '192.0.2.1', chip: 'ESP32-C3', rs485: 1,
        maintenance: true, printer: 0, motors: [], fl: [], uptime: 10, heap: 102400, heap_total: 204800,
      };
      await route.fulfill({ json: data });
    });
    await page.goto('http://bmcu.test/');
    await page.waitForFunction(() => document.querySelectorAll('#iobody tr').length === 4);
    await page.click('[data-tab="settings"]');
    await page.waitForFunction(() => document.querySelector('#p-ams').value === '1');
    // Wait for the tab's settings read before editing.
    await page.evaluate(() => loadPara());
    assert.equal(await page.locator('#t-settings #iobody').count(), 1);
    assert.equal(await page.locator('#t-status #iobody').count(), 0);
    assert.equal(await page.locator('#t-settings #card-pcf').count(), 1);
    for (const count of [2, 3, 4, 1, 2]) {
      await page.selectOption('#p-ams', String(count));
      assert.equal(await page.locator('#iobody tr').count(), count * 4);
    }
    assert.equal(await page.locator('#chbody tr').count(), 4, 'preview must not enable manual controls');
    await page.click('#save-io');
    assert.equal(posts.length, 0, 'unconfigured pins must not be submitted');
    for (let i = 4; i < 8; i++) {
      await page.selectOption('#io-mf' + i, String(i * 2));
      await page.selectOption('#io-mb' + i, String(i * 2 + 1));
    }
    await page.selectOption('#io-mf4', '0');
    await page.click('#save-io');
    assert.equal(posts.length, 0, 'duplicate pins must not be submitted');
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
    await page.setViewportSize({ width: 390, height: 844 });
    assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true, 'mobile layout must fit');
    if (process.env.WEB_TEST_SCREENSHOT) await page.screenshot({ path: process.env.WEB_TEST_SCREENSHOT, fullPage: true });
    assert.deepEqual(errors, []);
    console.log('PASS: placement, 4/8/12/16 preview, validation, drafts, save failure, grow/shrink, mobile layout');
  } finally {
    await browser.close();
  }
})().catch(e => { console.error(e); process.exitCode = 1; });
