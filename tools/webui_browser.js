#!/usr/bin/env node
// Step 6.7 device checks in a real headless browser (test points 13, 15 partly, 16, 17).
// Needs: puppeteer-core in a scratch dir (WEBUI_NODE_MODULES, see tools/webui_test.js),
// Edge or Chrome (BROWSER_EXE, default Edge), the device with the new data/ uploaded and a
// mock HA (tools/mockha.py --states-file F with 1500 entities; entity 0 must be
// light.mock_0 named '<img src=x onerror=alert(1)>') that the device is configured for.
//   node tools/webui_browser.js http://DEVICE_IP [web-password-check]
// With "web-password-check" it sets the web password 'testpw1', checks the 401 path and removes it.
'use strict';
const path = require('path');
const http = require('http');
const nm = process.env.WEBUI_NODE_MODULES;
const puppeteer = require(nm ? path.join(nm, 'puppeteer-core') : 'puppeteer-core');
const BASE = process.argv[2] || 'http://ha-knob.local';
const EXE = process.env.BROWSER_EXE || 'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe';
const XSS = '<img src=x onerror=alert(1)>';

const results = [];
function rec(point, ok, msg) { results.push({ point, ok, msg }); console.log(`${ok ? 'PASS' : 'FAIL'}  [${point}] ${msg}`); }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function postJson(p, body, auth) {
  return new Promise((resolve, reject) => {
    const data = JSON.stringify(body);
    const headers = { 'Content-Type': 'application/json', 'X-HAKnob': '1', 'Content-Length': Buffer.byteLength(data) };
    if (auth) headers.Authorization = 'Basic ' + Buffer.from(auth).toString('base64');
    const u = new URL(BASE + p);
    const req = http.request({ host: u.hostname, port: 80, path: p, method: 'POST', headers }, (res) => {
      let b = ''; res.on('data', (d) => (b += d)); res.on('end', () => resolve({ status: res.statusCode, body: b }));
    });
    req.on('error', reject); req.write(data); req.end();
  });
}

(async () => {
  const browser = await puppeteer.launch({ executablePath: EXE, headless: true, args: ['--no-sandbox'] });
  const page = await browser.newPage();
  await page.setViewport({ width: 420, height: 900 });
  const consoleLog = [];
  const dialogs = [];
  page.on('console', (m) => consoleLog.push(`${m.type()}: ${m.text()}`));
  page.on('pageerror', (e) => consoleLog.push('pageerror: ' + e.message));
  page.on('dialog', async (d) => { dialogs.push(`${d.type()}: ${d.message()}`); await d.accept(); });
  const failedReq = [];
  const badResp = [];
  page.on('response', (r) => { if (r.status() >= 400) badResp.push(r.status() + ' ' + r.request().method() + ' ' + new URL(r.url()).pathname); });
  page.on('requestfailed', (r) => failedReq.push(r.url() + ' ' + (r.failure() && r.failure().errorText)));
  const $text = (sel) => page.$eval(sel, (e) => e.textContent);
  const disp = (sel) => page.$eval(sel, (e) => getComputedStyle(e).display);

  const clear = await postJson('/api/config', { controls: [] });
  rec('15', clear.status === 200, 'start: controls cleared (' + clear.status + ')');
  await page.goto(BASE + '/', { waitUntil: 'networkidle0' });

  // ---- 13: HA tab behaviour with the real CSS
  await page.click('#tabs button[data-tab="ha"]');
  await sleep(300);
  rec('13', (await disp('#ha-tls')) === 'none', 'HA tab: TLS fieldset hidden for http address (display ' + (await disp('#ha-tls')) + ')');
  await page.$eval('#ha-url', (e) => { e.value = 'https://example.org'; e.dispatchEvent(new Event('input', { bubbles: true })); });
  rec('13', (await disp('#ha-tls')) !== 'none', 'HA tab: TLS fieldset shown for https');
  rec('13', (await disp('#ha-insecure-warning')) === 'none', 'HA tab: insecure warning hidden at first');
  await page.click('#ha-tls-insecure');
  rec('13', (await disp('#ha-insecure-warning')) !== 'none' && dialogs.some((d) => /certificate verification/.test(d)), 'HA tab: insecure warning shown after confirm (dialogs: ' + dialogs.length + ')');
  await page.click('#ha-tls-insecure');
  rec('13', (await disp('#ha-insecure-warning')) === 'none' && await page.$eval('#ha-ca-pem', (e) => !e.disabled), 'HA tab: warning hidden and CA field enabled again after unticking');
  rec('13', (await disp('#message')) === 'none', '#message hidden when empty');
  await page.$eval('#ha-url', (e) => { e.value = ''; e.dispatchEvent(new Event('input', { bubbles: true })); });
  await page.click('#ha-form button[type="submit"]');
  await sleep(200);
  rec('13', (await disp('#message')) !== 'none' && /Enter the Home Assistant address/.test(await $text('#message')), 'message box shown on error: ' + (await $text('#message')));
  rec('13', await page.evaluate(() => { const l = document.createElement('label'); l.hidden = true; document.body.append(l); const d = getComputedStyle(l).display; l.remove(); return d === 'none'; }), 'a hidden <label> is display:none with the real style.css ([hidden] !important rule works)');

  // ---- Controls tab, 1500 entities, XSS name
  await page.click('#tabs button[data-tab="controls"]');
  const t0 = Date.now();
  await page.waitForFunction(() => document.querySelectorAll('#ent-list > li').length > 0, { timeout: 90000 });
  const listMs = Date.now() - t0;
  const state = await $text('#ent-state');
  rec('17', /1286 entities/.test(state), `entity list arrived in ${listMs} ms after opening the tab (cold device fetch); state: "${state}"`);
  rec('17', !/slow to answer/.test(state), 'no "slow to answer" text');
  rec('1', (await page.$$('#ent-list img')).length === 0 && dialogs.every((d) => !/alert|^alert/.test(d) || /certificate/.test(d)), 'no <img> element and no alert dialog from the XSS name');
  rec('10', (await page.$$eval('#ent-list > li', (l) => l.length)) === 100 && /Showing 100 of 1286/.test(await $text('#ent-note')), 'picker shows 100 rows: ' + (await $text('#ent-note')));
  await page.type('#ent-search', 'img');
  await sleep(150);
  const xssRow = await page.$$eval('#ent-list > li', (l) => l.map((x) => x.querySelector('.entity-name').textContent));
  rec('1', xssRow.length === 1 && xssRow[0] === XSS, 'search "img" finds the XSS entity; shown as text: ' + JSON.stringify(xssRow));
  // add light.mock_7 (brightness), a switch, light.mock_7 again, then the XSS light (onoff only)
  const addId = async (id) => {
    await page.$eval('#ent-search', (e, v) => { e.value = v; e.dispatchEvent(new Event('input', { bubbles: true })); }, id);
    await page.evaluate((want) => { const li = [...document.querySelectorAll('#ent-list > li')].find((x) => x.querySelector('.entity-id').textContent.startsWith(want + ' - ')); li.querySelector('button').click(); }, id);
  };
  await addId('light.mock_7'); await addId('switch.mock_1'); await addId('light.mock_7'); await addId('light.mock_0');
  const titles = await page.$$eval('#ctl-list .control-title', (l) => l.map((x) => x.textContent));
  rec('1', titles.length === 4 && titles[3] === '4. ' + XSS, 'control title is text: ' + JSON.stringify(titles));
  rec('1', (await page.$$('#ctl-list img')).length === 0, 'no <img> in the control list');
  // 13: Step field invisible for none / switch (real CSS)
  const stepDisp = () => page.$$eval('#ctl-list > li', (rows) => rows.map((r) => getComputedStyle(r.querySelector('[data-field="step"]').closest('label')).display));
  let d = await stepDisp();
  rec('13', d[0] !== 'none' && d[1] === 'none' && d[2] !== 'none' && d[3] === 'none', 'Step field: brightness light shown, switch and onoff light hidden: ' + d.join(','));
  await page.select('#ctl-list > li:nth-child(3) [data-field="action"]', 'none');
  d = await stepDisp();
  rec('13', d[2] === 'none', 'Step field hidden after setting a brightness light to none: ' + d.join(','));
  await page.select('#ctl-list > li:nth-child(3) [data-field="action"]', 'brightness');
  d = await stepDisp();
  rec('13', d[2] !== 'none', 'Step field shown again for brightness: ' + d.join(','));
  // reorder: move row 3 up
  await page.click('#ctl-list > li:nth-child(3) [data-focus="up"]');
  const order = () => page.$$eval('#ctl-list .control-id', (l) => l.map((x) => x.textContent.split(' ')[0]));
  const o1 = await order();
  rec('15', JSON.stringify(o1) === JSON.stringify(['light.mock_7', 'light.mock_7', 'switch.mock_1', 'light.mock_0']), 'reorder: ' + JSON.stringify(o1));
  // invalid step blocked
  await page.$eval('#ctl-list > li:nth-child(1) [data-field="step"]', (e) => { e.value = '999'; e.dispatchEvent(new Event('input', { bubbles: true })); });
  await page.click('#ctl-save'); await sleep(300);
  rec('15', /Not saved: correct/.test(await $text('#ctl-status')), 'invalid step blocked: ' + (await $text('#ctl-status')));
  await page.$eval('#ctl-list > li:nth-child(1) [data-field="step"]', (e) => { e.value = '7'; e.dispatchEvent(new Event('input', { bubbles: true })); });
  // forced server row error: control character in a label (checked by the device only)
  await page.evaluate(() => { controls[2].label = 'a\u0001b'; });
  await page.click('#ctl-save'); await sleep(1200);
  const rowErr = await page.$$eval('#ctl-list > li', (rows) => rows.map((r) => r.querySelector('.control-error').textContent));
  const focusInfo = await page.evaluate(() => { const e = document.activeElement; return e && e.dataset ? e.dataset.focus + '@' + (e.closest('li') || {}).dataset?.index : String(e && e.id); });
  rec('15', rowErr[2] !== '' && rowErr[0] === '' && /rejected/.test(await $text('#ctl-status')), 'server rejected row 3: ' + JSON.stringify(rowErr) + ' / ' + (await $text('#ctl-status')));
  rec('19', /^label@2$/.test(focusInfo), 'focus on the rejected input after a server row error: ' + focusInfo);
  await page.evaluate(() => { controls[2].label = ''; });
  await page.$eval('#ctl-list > li:nth-child(3) [data-field="label"]', (e) => { e.value = 'Switch 1'; e.dispatchEvent(new Event('input', { bubbles: true })); });
  await page.click('#ctl-save'); await sleep(1500);
  const okFocus = await page.evaluate(() => document.activeElement && document.activeElement.id);
  rec('15', /Controls saved/.test(await $text('#ctl-status')), 'Save: ' + (await $text('#ctl-status')));
  rec('19', okFocus === 'ctl-save', 'focus on Save after a successful save: ' + okFocus);
  await page.reload({ waitUntil: 'networkidle0' });
  await page.click('#tabs button[data-tab="controls"]');
  await page.waitForFunction(() => document.querySelectorAll('#ctl-list > li').length > 0, { timeout: 20000 });
  const reload = await page.$$eval('#ctl-list > li', (rows) => rows.map((r) => [r.querySelector('.control-id').textContent.split(' ')[0], r.querySelector('[data-field="label"]').value]));
  rec('15', JSON.stringify(reload.map((x) => x[0])) === JSON.stringify(o1) && reload[2][1] === 'Switch 1', 'reload shows the saved list: ' + JSON.stringify(reload));
  // 25th refused: fill to 24 via the model (the device accepts at most 24), then try the 25th
  await page.waitForFunction(() => document.querySelectorAll('#ent-list > li').length > 0, { timeout: 90000 });
  await page.evaluate(() => { while (controls.length < 24) addControl(catalogById.get('light.mock_7')); });
  const full = await page.evaluate(() => [document.querySelectorAll('#ctl-list > li').length, [...document.querySelectorAll('#ent-list button')].every((b) => b.disabled)]);
  await page.evaluate(() => addControl(catalogById.get('light.mock_7')));
  rec('15', full[0] === 24 && full[1] && (await page.$$eval('#ctl-list > li', (l) => l.length)) === 24 && /at most 24/.test(await $text('#ctl-status')), '25th refused: ' + (await $text('#ctl-status')));

  // ---- CSP / console
  const csp = consoleLog.filter((l) => /Content Security Policy|Refused to|violat/i.test(l));
  rec('13', csp.length === 0, 'no CSP violations in the console (' + consoleLog.length + ' console lines; CSP lines: ' + JSON.stringify(csp) + ')');
  const errs = consoleLog.filter((l) => /^pageerror/.test(l));
  rec('13', errs.length === 0, 'no page (script) errors: ' + JSON.stringify(errs));
  // only the forced 400 (POST /api/config) and the browser's own /favicon.ico probe may be >= 400
  const unexpected = badResp.filter((x) => x !== '400 POST /api/config' && x !== '404 GET /favicon.ico');
  rec('13', unexpected.length === 0, 'unexpected HTTP errors: ' + JSON.stringify(unexpected) + ' (all >= 400: ' + JSON.stringify(badResp) + ')');

  // ---- 17: throttled client (CDP network emulation)
  const cdp = await page.createCDPSession();
  await cdp.send('Network.enable');
  await cdp.send('Network.setCacheDisabled', { cacheDisabled: true });
  await page.reload({ waitUntil: 'networkidle0' });
  await cdp.send('Network.emulateNetworkConditions', { offline: false, latency: 100, downloadThroughput: 4 * 1024, uploadThroughput: 4 * 1024 });  // 4 KB/s: 135 KB takes ~34 s
  const t1 = Date.now();
  await page.click('#tabs button[data-tab="controls"]');
  const seen = new Set();
  let done = false;
  while (Date.now() - t1 < 125000 && !done) {
    seen.add(await $text('#ent-state'));
    done = await page.evaluate(() => document.querySelectorAll('#ent-list > li').length > 0);
    await sleep(500);
  }
  rec('17', done, `throttled (4 KB/s) list arrived after ${Date.now() - t1} ms; states seen: ${JSON.stringify([...seen])}`);
  await cdp.send('Network.emulateNetworkConditions', { offline: false, latency: 0, downloadThroughput: -1, uploadThroughput: -1 });

  // ---- 18: leave the tab during a slow download
  await page.evaluate(() => { catalog = null; catalogSorted = []; catalogById = new Map(); });
  await cdp.send('Network.emulateNetworkConditions', { offline: false, latency: 100, downloadThroughput: 8 * 1024, uploadThroughput: 8 * 1024 });
  const reqLog = [];
  page.on('request', (r) => { if (r.url().includes('/api/ha/')) reqLog.push(r.method() + ' ' + new URL(r.url()).pathname + ' t=' + Date.now()); });
  page.on('requestfailed', (r) => { if (r.url().includes('/api/ha/')) reqLog.push('FAILED ' + r.url() + ' ' + (r.failure() && r.failure().errorText)); });
  await page.click('#tabs button[data-tab="status"]');
  await page.click('#tabs button[data-tab="controls"]');
  await sleep(4000);
  await page.click('#tabs button[data-tab="status"]');
  await sleep(500);
  const n1 = reqLog.length;
  await sleep(6000);
  rec('18', reqLog.length === n1 && reqLog.some((l) => /FAILED.*ERR_ABORTED/.test(l)), 'leaving mid-download: request aborted, nothing sent while away: ' + JSON.stringify(reqLog.map((l) => l.replace(/ t=\d+/, ''))));
  await cdp.send('Network.emulateNetworkConditions', { offline: false, latency: 0, downloadThroughput: -1, uploadThroughput: -1 });
  await page.click('#tabs button[data-tab="controls"]');
  await page.waitForFunction(() => document.querySelectorAll('#ent-list > li').length > 0, { timeout: 60000 });
  rec('18', !reqLog.slice(n1).some((l) => /^POST/.test(l)), 'list loads on return with GET only');

  // ---- 16: web password and the 401 path
  if (process.argv[3] === 'web-password-check') {
    let r = await postJson('/api/config', { web: { password: 'testpw1' } });
    rec('16', r.status === 200, 'web password set: ' + r.status);
    await page.authenticate({ username: 'admin', password: 'testpw1' });
    await page.goto(BASE + '/', { waitUntil: 'networkidle0' });
    await page.click('#tabs button[data-tab="controls"]');
    await page.waitForFunction(() => document.querySelectorAll('#ent-list > li').length > 0, { timeout: 60000 });
    rec('16', true, 'with the right password the Controls tab loads');
    // the browser caches the credentials; change the password behind its back so the cached ones go stale
    r = await postJson('/api/config', { web: { password: 'testpw2' } }, 'admin:testpw1');
    rec('16', r.status === 200, 'password changed behind the page: ' + r.status);
    await page.authenticate({ username: 'admin', password: 'testpw1' });  // stale
    await page.evaluate(() => { catalog = null; });
    await page.click('#tabs button[data-tab="status"]');
    await page.click('#tabs button[data-tab="controls"]');
    await sleep(3000);
    const st = await $text('#ent-state');
    rec('16', /Not authorised/.test(st), 'stale credentials: entity state text "' + st + '"');
    await page.evaluate(() => { controlsDirty = false; });
    await page.click('#ctl-save'); await sleep(2000);
    const cs = await $text('#ctl-status');
    rec('16', /Not authorised|Not saved/.test(cs), 'Save with wrong credentials: "' + cs + '"');
    r = await postJson('/api/config', { web: { password: null } }, 'admin:testpw2');
    rec('16', r.status === 200, 'web password removed: ' + r.status);
  }

  await browser.close();
  const failed = results.filter((x) => !x.ok).length;
  console.log(`\n${results.length - failed} of ${results.length} browser checks passed, ${failed} failed`);
  console.log('dialogs:', JSON.stringify(dialogs));
  process.exit(failed ? 1 : 0);
})().catch((e) => { console.error('ERROR', e); process.exit(2); });
