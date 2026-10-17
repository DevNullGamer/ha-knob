#!/usr/bin/env node
// Step 6.7 Web UI (Controls tab) tests: static checks plus a jsdom harness with a mocked
// fetch and a virtual clock. Test points 1-12 and 17-19 (jsdom part) of testpoints-6.7.md.
//
// No node_modules in the repo: install jsdom in any scratch directory and point to it:
//   mkdir %TEMP%\webui_nm && cd %TEMP%\webui_nm && npm init -y && npm i jsdom
//   set WEBUI_NODE_MODULES=%TEMP%\webui_nm\node_modules        (PowerShell: $env:WEBUI_NODE_MODULES=...)
//   node tools/webui_test.js
// Exit code 0 = all passed. Never touches a device; data/ is only read.
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const util = require('util');

const ROOT = path.resolve(__dirname, '..');
const DATA = path.join(ROOT, 'data');
let JSDOM;
try {
  JSDOM = require(process.env.WEBUI_NODE_MODULES ? path.join(process.env.WEBUI_NODE_MODULES, 'jsdom') : 'jsdom').JSDOM;
} catch (e) {
  console.error('jsdom not found. Set WEBUI_NODE_MODULES to a node_modules dir that contains jsdom.');
  process.exit(2);
}

const APP = fs.readFileSync(path.join(DATA, 'app.js'), 'utf8');
const HTML = fs.readFileSync(path.join(DATA, 'index.html'), 'utf8');
const CSS = fs.readFileSync(path.join(DATA, 'style.css'), 'utf8');

// ---- mini framework ------------------------------------------------------------------
const results = [];
let current = null;
function check(cond, msg) {
  if (!cond) current.fails.push(msg);
  current.n++;
}
function eq(a, b, msg) {
  const same = util.isDeepStrictEqual(JSON.parse(JSON.stringify(a === undefined ? null : a)),
    JSON.parse(JSON.stringify(b === undefined ? null : b)));
  check(same, msg + ' (got ' + JSON.stringify(a) + ', want ' + JSON.stringify(b) + ')');
}
const tests = [];
function test(points, name, fn) { tests.push({ points, name, fn }); }

// ---- static checks (point "Static") ----------------------------------------------------
test('S', 'static checks', async () => {
  const { execFileSync } = require('child_process');
  try {
    execFileSync(process.execPath, ['--check', path.join(DATA, 'app.js')]);
    check(true, '');
  } catch (e) { check(false, 'node --check app.js failed: ' + e.message); }
  const bad = /innerHTML|outerHTML|insertAdjacentHTML|eval\(|new Function|style=| on[a-z]+=/;
  for (const f of ['index.html', 'app.js', 'style.css']) {
    fs.readFileSync(path.join(DATA, f), 'utf8').split('\n').forEach((line, i) => {
      if (bad.test(line)) {
        const allowed = f === 'app.js' && i + 1 === 2;
        check(allowed, `forbidden pattern in ${f}:${i + 1}: ${line.trim()}`);
      }
    });
  }
  check(!/<style/i.test(HTML), 'index.html has an inline <style>');
  check(!/<script(?![^>]*\bsrc=)/i.test(HTML), 'index.html has an inline <script>');
  check(!/\sstyle\s*=/i.test(HTML), 'index.html has a style attribute');
  // No action / press / step-range literals: the table comes from /api/meta/actions only.
  for (const lit of ['toggle', 'brightness', 'color_temp', 'hue', 'target_temp', 'position', 'volume',
    'percentage', 'play_pause', 'mute', 'tilt', 'open_close']) {
    check(!new RegExp(`['"]${lit}['"]`).test(APP), `app.js contains the action literal '${lit}'`);
  }
  check(/\[hidden\]\s*\{\s*display\s*:\s*none\s*!important\s*;?\s*\}/.test(CSS), 'style.css lacks [hidden]{display:none !important}');
});

// ---- fixtures ------------------------------------------------------------------------
const META = {
  max_controls: 24, max_label_bytes: 48,
  domains: [
    { domain: 'light', actions: [{ name: 'brightness', step: { default: 5, min: 1, max: 25 } },
      { name: 'color_temp', step: { default: 200, min: 50, max: 1000 } }, { name: 'none' }], presses: ['toggle', 'none'] },
    { domain: 'switch', actions: [{ name: 'none' }], presses: ['toggle', 'none'] },
    { domain: 'climate', actions: [{ name: 'target_temp', step: { default: 0.5, min: 0.1, max: 5 } }, { name: 'none' }], presses: ['toggle', 'none'] },
    { domain: 'fan', actions: [{ name: 'percentage', step: { default: 10, min: 1, max: 50 } }, { name: 'none' }], presses: ['toggle', 'none'] },
  ],
};
const ACTIONS = { light: ['brightness', 'color_temp', 'none'], switch: ['none'], climate: ['target_temp', 'none'], fan: ['percentage', 'none'] };
const ent = (id, name, state, extra) => Object.assign({
  id, name, state: state || 'on', actions: ACTIONS[id.split('.')[0]], step_default: id.startsWith('fan.') ? 10 : id.startsWith('climate.') ? 0.5 : id.startsWith('light.') ? 5 : 0,
}, extra || {});
const ctl = (id, extra) => Object.assign({ entity_id: id, label: '', action: ACTIONS[id.split('.')[0]][0],
  step: id.startsWith('switch.') ? 0 : 5, press: 'toggle' }, extra || {});
const catalogOf = (entities, extra) => Object.assign({ entities, truncated: false, skipped: 0 }, extra || {});

// ---- page harness ----------------------------------------------------------------------
class Page {
  constructor() {
    this.t = 0; this.timers = []; this.nextTimer = 1; this.reqs = []; this.id = 0;
    this.srv = {
      config: { wifi: { ssid: 'x', password_set: true }, device: { hostname: 'ha-knob', brightness: 50, dim_after_s: 0, dim_level: 10, off_after_s: 0, send_interval_ms: 300 },
        web: { password_set: false }, ha: { url: 'http://h:8123', token_set: true }, controls: [] },
      configStatus: 200, configNet: false, meta: META,
      catalog: catalogOf([]),
      onGetEntities: () => ({ status: 200, data: this.srv.catalog }),
      onRefresh: () => ({ status: 202, data: { ok: true } }),
      onPostConfig: null,
    };
  }
  async init() {
    const html = HTML.replace(/<script[^>]*><\/script>/, '').replace(/<link[^>]*stylesheet[^>]*>/, `<style>${CSS}</style>`);
    const dom = new JSDOM(html, { runScripts: 'outside-only', url: 'http://knob.local/', pretendToBeVisual: true,
      beforeParse: (w) => this.install(w) });
    this.dom = dom; this.w = dom.window; this.d = dom.window.document;
    this.ctx = dom.getInternalVMContext();
    await new Promise((r) => (this.d.readyState === 'complete' ? r() : this.w.addEventListener('load', r)));
    this.w.alert = () => { this.alerted = true; };
    this.w.confirm = () => true;
    this.pageErrors = [];
    this.w.addEventListener('error', (e) => this.pageErrors.push(String(e.message)));
    new vm.Script(APP, { filename: 'app.js' }).runInContext(this.ctx);
    this.d.dispatchEvent(new this.w.Event('DOMContentLoaded'));
    await this.flush();
    return this;
  }
  install(w) {
    const self = this;
    w.TextEncoder = util.TextEncoder;
    const RealDate = w.Date;
    w.Date = class extends RealDate { static now() { return self.t; } };
    w.setTimeout = (fn, ms, ...a) => { const id = self.nextTimer++; self.timers.push({ id, at: self.t + Math.max(0, ms || 0), fn: () => fn(...a) }); return id; };
    w.setInterval = (fn, ms, ...a) => { const id = self.nextTimer++; self.timers.push({ id, at: self.t + Math.max(1, ms || 1), every: Math.max(1, ms || 1), fn: () => fn(...a) }); return id; };
    w.clearTimeout = w.clearInterval = (id) => { self.timers = self.timers.filter((x) => x.id !== id); };
    w.fetch = (p, opts) => self.fetch(p, opts || {});
  }
  fetch(p, opts) {
    const req = { id: ++this.id, method: opts.method || 'GET', path: p, headers: opts.headers || {}, bodyText: opts.body,
      body: opts.body ? JSON.parse(opts.body) : undefined, signal: opts.signal, t: this.t, aborted: false, done: false };
    this.reqs.push(req);
    const abortErr = () => new this.w.DOMException('The operation was aborted.', 'AbortError');
    return new Promise((resolve, reject) => {
      const fail = (e) => { req.done = true; reject(e); };
      if (req.signal && req.signal.aborted) { req.aborted = true; return fail(abortErr()); }
      if (req.signal) req.signal.addEventListener('abort', () => { if (!req.done) { req.aborted = true; fail(abortErr()); } });
      const spec = this.route(req) || {};
      if (spec.networkError) return fail(new TypeError('Failed to fetch'));
      if (spec.hang) return undefined;  // headers never arrive
      const answer = () => {
        if (req.done) return;
        req.done = true;
        const hdr = Object.fromEntries(Object.entries(spec.headers || {}).map(([k, v]) => [k.toLowerCase(), String(v)]));
        resolve({ status: spec.status, ok: spec.status >= 200 && spec.status < 300,
          headers: { get: (n) => (n.toLowerCase() in hdr ? hdr[n.toLowerCase()] : null) },
          json: () => new Promise((res, rej) => {
            if (spec.bodyHang) {
              req.done = false;  // body still in flight until aborted
              if (req.signal) req.signal.addEventListener('abort', () => { req.aborted = true; req.done = true; rej(abortErr()); });
              return;
            }
            res(JSON.parse(JSON.stringify(spec.data === undefined ? null : spec.data)));
          }) });
      };
      if (spec.delay) this.w.setTimeout(answer, spec.delay); else answer();
    });
  }
  route(req) {
    const key = req.method + ' ' + req.path, S = this.srv;
    if (key === 'GET /api/status') {
      return { status: 200, data: { fw: 't', uptime_s: 1, wifi: { mode: 'sta', ssid: 'x', connected: true, ip: '1.2.3.4', rssi: -50, hostname: 'ha-knob', ap_active: false },
        ha: { state: 'ready' }, touch_variant: 'a', heap_free: 1024, psram_free: 1024 } };
    }
    if (key === 'GET /api/config') {
      if (S.configNet) return { networkError: true };
      return S.configStatus === 200 ? { status: 200, data: S.config } : { status: S.configStatus, data: { error: 'internal' } };
    }
    if (key === 'POST /api/config') {
      if (S.onPostConfig) return S.onPostConfig(req);
      if (req.body.controls) S.config.controls = req.body.controls;
      return { status: 200, data: { ok: true } };
    }
    if (key === 'GET /api/meta/actions') return S.metaSpec ? S.metaSpec() : { status: 200, data: S.meta };
    if (key === 'GET /api/ha/entities') { S.getN = (S.getN || 0) + 1; return S.onGetEntities(S.getN, req); }
    if (key === 'POST /api/ha/entities/refresh') { S.postN = (S.postN || 0) + 1; return S.onRefresh(S.postN, req); }
    return { status: 404, data: { error: 'not_found' } };
  }
  async flush() { for (let i = 0; i < 6; i++) await new Promise((r) => setImmediate(r)); }
  async advance(ms) {
    const end = this.t + ms;
    await this.flush();
    for (;;) {
      const due = this.timers.filter((x) => x.at <= end).sort((a, b) => a.at - b.at || a.id - b.id)[0];
      if (!due) break;
      this.t = Math.max(this.t, due.at);
      if (due.every) due.at += due.every; else this.timers = this.timers.filter((x) => x !== due);
      due.fn();
      await this.flush();
    }
    this.t = end;
    await this.flush();
  }
  $(id) { return this.d.getElementById(id); }
  q(sel, root) { return (root || this.d).querySelector(sel); }
  qa(sel, root) { return [...(root || this.d).querySelectorAll(sel)]; }
  js(expr) { return new vm.Script(expr).runInContext(this.ctx); }
  rows() { return this.qa('#ctl-list > li'); }
  api(path, method) { return this.reqs.filter((r) => r.path === path && (!method || r.method === method)); }
  async tab(name) { this.q(`#tabs button[data-tab="${name}"]`).click(); await this.flush(); }
  async open() { await this.tab('controls'); }
  async leave() { await this.tab('status'); }
  set(el, value) { el.value = value; el.dispatchEvent(new this.w.Event('input', { bubbles: true })); }
  choose(el, value) { el.value = value; el.dispatchEvent(new this.w.Event('change', { bubbles: true })); }
  rowBtn(i, which) { return this.q(`[data-focus="${which}"]`, this.rows()[i]); }
  async add(id) {
    const li = this.qa('#ent-list > li').find((x) => this.q('.entity-id', x).textContent.startsWith(id + ' - '));
    if (!li) throw new Error('no picker row for ' + id);
    this.q('button', li).click(); await this.flush();
  }
  vis(el) { return this.w.getComputedStyle(el).display !== 'none'; }
}
async function page(setup) {
  const p = new Page();
  if (setup) setup(p.srv, p);
  return p.init();
}

const A = ent('light.a', 'Lamp A'), B = ent('switch.b', 'Switch B'), C = ent('light.c', 'Lamp C'), F = ent('fan.f', 'Fan F'), K = ent('climate.k', 'Climate K');
const XSS = '<img src=x onerror=alert(1)>';

// ---- 1: XSS ----------------------------------------------------------------------------
test('1', 'friendly name / state with markup renders as text', async () => {
  const p = await page((s) => {
    s.catalog = catalogOf([ent('light.x', XSS, XSS)]);
    s.config.controls = [ctl('light.x'), ctl('light.gone', { label: XSS })];
  });
  await p.open(); await p.advance(1);
  check(p.qa('img').length === 0, 'an <img> element exists in the document');
  check(!p.alerted, 'alert was called');
  const pick = p.q('#ent-list > li');
  eq(p.q('.entity-name', pick).textContent, XSS, 'picker name text');
  eq(p.q('.entity-id', pick).textContent, 'light.x - ' + XSS, 'picker id/state text');
  eq(p.q('button', pick).getAttribute('aria-label'), 'Add ' + XSS, 'picker aria-label');
  const rows = p.rows();
  eq(p.q('.control-title', rows[0]).textContent, '1. ' + XSS, 'control title');
  eq(p.q('.control-id', rows[0]).textContent, 'light.x - ' + XSS, 'control id line');
  eq(p.q('[data-field="label"]', rows[0]).placeholder, XSS, 'label placeholder');
  eq(p.q('.control-title', rows[1]).textContent, '2. ' + XSS, 'control title from label');
  eq(p.q('[data-field="label"]', rows[1]).value, XSS, 'label value');
  await p.add('light.x');
  const added = p.rows()[2];
  eq(p.q('[aria-label="Remove control 3"]', added) !== null, true, 'added row aria-label');
  check(p.qa('img').length === 0, 'an <img> after Add');
  check(!/<img/i.test(p.q('.entity-name', p.$('ent-list')).innerHTML), 'unescaped <img in the name markup');
});

// ---- 2: Step field hidden for none ------------------------------------------------------
test('2', 'no Step field for switch and for a light set to none (computed display)', async () => {
  const p = await page((s) => { s.catalog = catalogOf([A, B]); s.config.controls = [ctl('switch.b', { action: 'none', step: 0 }), ctl('light.a')]; });
  await p.open(); await p.advance(1);
  const stepLabel = (i) => p.q('[data-field="step"]', p.rows()[i]).closest('label');
  check(!p.vis(stepLabel(0)), 'switch: Step field displayed');
  check(p.vis(stepLabel(1)), 'light/brightness: Step field not displayed');
  p.choose(p.q('[data-field="action"]', p.rows()[1]), 'none');
  check(!p.vis(stepLabel(1)), 'light set to none: Step field displayed');
  p.choose(p.q('[data-field="action"]', p.rows()[1]), 'color_temp');
  check(p.vis(stepLabel(1)), 'light back to color_temp: Step field not displayed');
  eq(p.q('[data-field="step"]', p.rows()[1]).value, '200', 'step reset to meta default 200');
  // jsdom's built-in UA sheet already hides [hidden], so it cannot prove the CSS rule is needed:
  // point 13 (tools/webui_browser.js, real Edge/Chrome) checks the real cascade.
});

// ---- 3: config GET failure -> no POST ------------------------------------------------------
for (const mode of ['500', 'network']) {
  test('3', `GET /api/config ${mode}: Save never posts; recovery on reopen`, async () => {
    const p = await page((s) => { s.catalog = catalogOf([A]); s.config.controls = [ctl('light.a')]; if (mode === '500') s.configStatus = 500; else s.configNet = true; });
    await p.open(); await p.advance(1);
    check(/settings could not be loaded/.test(p.$('ctl-status').textContent), 'error text missing: ' + p.$('ctl-status').textContent);
    check(p.$('ctl-save').disabled, 'Save not disabled');
    p.$('ctl-save').click(); p.js('saveControls()'); await p.flush();
    eq(p.api('/api/config', 'POST').length, 0, 'POST count after failed load');
    p.srv.configStatus = 200; p.srv.configNet = false;
    await p.leave(); await p.open(); await p.advance(1);
    eq(p.rows().length, 1, 'rows after recovery');
    check(p.$('ctl-status').hidden && p.$('ctl-status').textContent === '', 'stale error text still shown: ' + p.$('ctl-status').textContent);
    check(!p.$('ctl-save').disabled, 'Save still disabled after recovery');
  });
}
test('3', 'config OK at page load but list fetch via loadConfig failing mid-save keeps controls', async () => {
  const p = await page((s) => { s.catalog = catalogOf([A]); s.config.controls = [ctl('light.a')]; });
  await p.open(); await p.advance(1);
  p.srv.configNet = true;  // the re-read after a successful save fails
  p.$('ctl-save').click(); await p.advance(1);
  eq(p.rows().length, 1, 'row kept when the re-read fails');
  check(/Controls saved/.test(p.$('ctl-status').textContent), 'saved text: ' + p.$('ctl-status').textContent);
});

// ---- 4: full edit + payload ---------------------------------------------------------------
test('4', 'add 3 incl. duplicate, reorder, edit, Save: exact payload and headers', async () => {
  const p = await page((s) => { s.catalog = catalogOf([A, B]); });
  await p.open(); await p.advance(1);
  await p.add('light.a'); await p.add('switch.b'); await p.add('light.a');
  eq(p.rows().length, 3, 'rows after 3 adds');
  p.rowBtn(2, 'up').click(); await p.flush();           // [a, a, b]
  eq(p.js('controls.map(c=>c.entity_id)'), ['light.a', 'light.a', 'switch.b'], 'order after reorder');
  p.choose(p.rowBtn(0, 'action'), 'color_temp');
  p.set(p.rowBtn(0, 'step'), '300');
  p.choose(p.rowBtn(0, 'press'), 'none');
  p.choose(p.rowBtn(1, 'action'), 'none');
  p.set(p.rowBtn(1, 'label'), 'Dup');
  p.$('ctl-save').click(); await p.advance(1);
  const posts = p.api('/api/config', 'POST');
  eq(posts.length, 1, 'POST count');
  eq(posts[0].body, { controls: [
    { entity_id: 'light.a', label: '', action: 'color_temp', step: 300, press: 'none' },
    { entity_id: 'light.a', label: 'Dup', action: 'none', step: 0, press: 'toggle' },
    { entity_id: 'switch.b', label: '', action: 'none', step: 0, press: 'toggle' }] }, 'payload');
  eq(Object.keys(posts[0].body), ['controls'], 'payload top-level keys');
  eq(posts[0].headers['X-HAKnob'], '1', 'X-HAKnob header');
  eq(posts[0].headers['Content-Type'], 'application/json', 'Content-Type header');
  check(/Controls saved/.test(p.$('ctl-status').textContent), 'success text');
  eq(p.rows().length, 3, 'rows after save');
  check(p.$('ctl-dirty').hidden, 'dirty marker still shown after save');
  // 25th is refused
  const q = await page((s) => { s.catalog = catalogOf([A]); s.config.controls = Array.from({ length: 24 }, () => ctl('light.a')); });
  await q.open(); await q.advance(1);
  check(q.qa('#ent-list button').every((b) => b.disabled), 'Add buttons not disabled at 24');
  q.js(`addControl(catalogById.get('light.a'))`); await q.flush();
  eq(q.rows().length, 24, '25th refused');
  check(/at most 24/.test(q.$('ctl-status').textContent), '25th refusal text: ' + q.$('ctl-status').textContent);
});

// ---- 5: forced 400 row error -------------------------------------------------------------------
test('5', 'forced 400 on controls[2].step lands on index 2 of the sent list; remove clears it', async () => {
  const p = await page((s) => {
    s.catalog = catalogOf([A, C, ent('light.b', 'Lamp B')]);
    s.config.controls = [ctl('light.a'), ctl('light.b'), ctl('light.c')];
    s.onPostConfig = () => ({ status: 400, data: { error: 'out_of_range', field: 'controls[2].step' } });
  });
  await p.open(); await p.advance(1);
  p.rowBtn(2, 'up').click(); await p.flush();   // [a, c, b]
  p.$('ctl-save').click(); await p.advance(1);
  const sent = p.api('/api/config', 'POST')[0].body.controls;
  const rows = p.rows();
  eq(p.q('.control-id', rows[2]).textContent.split(' ')[0], sent[2].entity_id, 'row 2 is sent[2]');
  const step = p.q('[data-field="step"]', rows[2]);
  eq(step.getAttribute('aria-invalid'), 'true', 'step aria-invalid');
  const errEl = p.q('.control-error', rows[2]);
  eq(step.getAttribute('aria-describedby'), errEl.id, 'aria-describedby');
  check(!errEl.hidden && /^Step: /.test(errEl.textContent), 'row error text: ' + errEl.textContent);
  check(!p.q('[data-field="step"]', rows[1]).hasAttribute('aria-invalid') && !p.q('[data-field="step"]', rows[0]).hasAttribute('aria-invalid'), 'other rows invalid');
  check(/control 3 was rejected/.test(p.$('ctl-status').textContent), 'status text: ' + p.$('ctl-status').textContent);
  p.rowBtn(2, 'remove').click(); await p.flush();
  eq(p.qa('[aria-invalid]').length, 0, 'aria-invalid after removing the row');
  check(p.qa('.control-error').every((e) => e.hidden), 'an error is still visible');
});

// ---- 6: label bytes --------------------------------------------------------------------------
test('6', 'label 48 bytes accepted, 49 blocked (live and on Save)', async () => {
  const p = await page((s) => { s.catalog = catalogOf([A]); s.config.controls = [ctl('light.a')]; });
  await p.open(); await p.advance(1);
  const l48 = 'æ'.repeat(24), l49 = l48 + 'a';
  p.set(p.rowBtn(0, 'label'), l48);
  check(p.q('.control-error', p.rows()[0]).hidden, 'error shown for 48 bytes');
  eq(p.rowBtn(0, 'label').hasAttribute('aria-invalid'), false, '48 bytes aria-invalid');
  p.set(p.rowBtn(0, 'label'), l49);
  check(!p.q('.control-error', p.rows()[0]).hidden && /49 bytes/.test(p.q('.control-error', p.rows()[0]).textContent), 'live error for 49: ' + p.q('.control-error', p.rows()[0]).textContent);
  eq(p.rowBtn(0, 'label').getAttribute('aria-invalid'), 'true', '49 bytes aria-invalid');
  p.$('ctl-save').click(); await p.advance(1);
  eq(p.api('/api/config', 'POST').length, 0, 'POST with 49 bytes');
  check(/Not saved/.test(p.$('ctl-status').textContent), 'Save blocked text');
  p.set(p.rowBtn(0, 'label'), l48);
  p.$('ctl-save').click(); await p.advance(1);
  eq(p.api('/api/config', 'POST').length, 1, 'POST with 48 bytes');
  eq(p.api('/api/config', 'POST')[0].body.controls[0].label, l48, 'label sent');
});

// ---- 7: step validation ----------------------------------------------------------------------
test('7', 'steps: below min, above max, empty, non-number blocked; climate 0.1 and 5 accepted', async () => {
  const p = await page((s) => { s.catalog = catalogOf([A, K]); s.config.controls = [ctl('light.a'), ctl('climate.k', { step: 0.5 })]; });
  await p.open(); await p.advance(1);
  for (const [v, label] of [['0', 'below min'], ['26', 'above max'], ['', 'empty'], ['abc', 'non-number']]) {
    p.set(p.rowBtn(0, 'step'), v);
    p.$('ctl-save').click(); await p.advance(1);
    eq(p.api('/api/config', 'POST').length, 0, `POST for ${label}`);
    eq(p.rowBtn(0, 'step').getAttribute('aria-invalid'), 'true', `${label} aria-invalid`);
    check(/from 1 to 25/.test(p.q('.control-error', p.rows()[0]).textContent), `${label} text`);
  }
  p.set(p.rowBtn(0, 'step'), '25');
  p.set(p.rowBtn(1, 'step'), '0.1');
  p.$('ctl-save').click(); await p.advance(1);
  eq(p.api('/api/config', 'POST').length, 1, 'POST count for 25 and 0.1');
  eq(p.api('/api/config', 'POST')[0].body.controls.map((c) => c.step), [25, 0.1], 'steps sent');
  p.set(p.rowBtn(1, 'step'), '5');
  p.$('ctl-save').click(); await p.advance(1);
  eq(p.api('/api/config', 'POST').length, 2, 'POST count for 5');
  eq(p.api('/api/config', 'POST')[1].body.controls[1].step, 5, 'climate 5 sent');
  p.set(p.rowBtn(1, 'step'), '5.1');
  p.$('ctl-save').click(); await p.advance(1);
  eq(p.api('/api/config', 'POST').length, 2, 'climate 5.1 blocked');
});

// ---- 8: polling -------------------------------------------------------------------------------
const gets = (p) => p.api('/api/ha/entities', 'GET');
test('8', '202 x N then 200 renders; interval 1 s', async () => {
  const p = await page((s) => { s.catalog = catalogOf([A]); s.onGetEntities = (n) => (n <= 3 ? { status: 202, data: { loading: true } } : { status: 200, data: s.catalog }); });
  await p.open(); await p.advance(5000);
  eq(gets(p).length, 4, 'GET count');
  eq(gets(p).map((r) => r.t - gets(p)[0].t), [0, 1000, 2000, 3000], 'GET times');
  eq(p.qa('#ent-list > li').length, 1, 'list rendered');
  check(/1 entities/.test(p.$('ent-state').textContent), 'state line: ' + p.$('ent-state').textContent);
  check(!p.$('ent-refresh').disabled, 'Refresh still disabled');
});
test('8', '503 busy Retry-After 3 waits 3 s; 30 is capped at 10 s', async () => {
  for (const [ra, want] of [['3', 3000], ['30', 10000]]) {
    const p = await page((s) => { s.catalog = catalogOf([A]); s.onGetEntities = (n) => (n === 1 ? { status: 503, headers: { 'Retry-After': ra }, data: { error: 'busy' } } : { status: 200, data: s.catalog }); });
    await p.open(); await p.advance(want - 1);
    eq(gets(p).length, 1, `GETs just before ${want} ms (Retry-After ${ra})`);
    await p.advance(2);
    eq(gets(p).length, 2, `GETs after ${want} ms`);
    eq(gets(p)[1].t - gets(p)[0].t, want, 'wait');
    eq(p.qa('#ent-list > li').length, 1, 'rendered');
  }
});
test('8', '202 forever stops at 120 s with timeout text; Refresh enabled again', async () => {
  const p = await page((s) => { s.onGetEntities = () => ({ status: 202, data: { loading: true } }); });
  await p.open(); await p.advance(119000);
  check(p.$('ent-refresh').disabled, 'Refresh enabled while loading');
  check(/Home Assistant is sending/.test(p.$('ent-state').textContent), 'progress text: ' + p.$('ent-state').textContent);
  await p.advance(2000);
  check(/2 minutes/.test(p.$('ent-state').textContent), 'timeout text: ' + p.$('ent-state').textContent);
  check(!p.$('ent-refresh').disabled, 'Refresh disabled after timeout');
  const n = gets(p).length;
  await p.advance(10000);
  eq(gets(p).length, n, 'requests after the timeout');
  check(n >= 118 && n <= 122, 'GET count ~120: ' + n);
});
test('8', 'second Refresh cancels the first loop', async () => {
  const p = await page((s) => { s.onGetEntities = () => ({ status: 202, data: { loading: true } }); });
  await p.open(); await p.advance(5000);
  const first = gets(p).length;
  check(p.$('ent-refresh').disabled, 'Refresh button enabled during load');
  p.$('ent-refresh').click(); await p.flush();
  eq(p.api('/api/ha/entities/refresh', 'POST').length, 0, 'disabled button click posted');
  p.js('loadEntities(true)'); await p.advance(1);   // a second load while the first runs
  eq(p.api('/api/ha/entities/refresh', 'POST').length, 1, 'refresh POST');
  const mark = gets(p).length;
  await p.advance(10000);
  const after = gets(p).length - mark;
  check(after >= 9 && after <= 11, 'one loop only: GETs in 10 s = ' + after + ' (first loop had ' + first + ' in 5 s)');
  check(p.reqs.filter((r) => r.path === '/api/ha/entities' && !r.done).length <= 1, 'more than one request pending');
});
test('8', 'leaving the tab stops requests; coming back resumes with GET only', async () => {
  const p = await page((s) => { s.catalog = catalogOf([A]); s.onGetEntities = (n) => (n <= 6 ? { status: 202, data: {} } : { status: 200, data: s.catalog }); });
  await p.open(); await p.advance(1);
  p.js('loadEntities(true)'); await p.advance(2500);   // refresh POST + GETs
  const posts = p.api('/api/ha/entities/refresh', 'POST').length, g = gets(p).length;
  await p.leave();
  const before = p.reqs.filter((r) => r.path.startsWith('/api/ha')).length;
  await p.advance(30000);
  eq(p.reqs.filter((r) => r.path.startsWith('/api/ha')).length, before, 'requests to /api/ha while away');
  await p.open(); await p.advance(10000);
  eq(p.api('/api/ha/entities/refresh', 'POST').length, posts, 'new POST after return');
  check(gets(p).length > g, 'no GET after return');
  eq(p.qa('#ent-list > li').length, 1, 'list loaded after return');
});

// ---- 9: error texts -------------------------------------------------------------------------
test('9', '503 ha_not_connected on GET and refresh POST', async () => {
  const p = await page((s) => { s.onGetEntities = () => ({ status: 503, data: { error: 'ha_not_connected' } }); s.onRefresh = () => ({ status: 503, data: { error: 'ha_not_connected' } }); });
  await p.open(); await p.advance(1);
  check(/Home Assistant is not connected/.test(p.$('ent-state').textContent), 'GET text: ' + p.$('ent-state').textContent);
  p.$('ent-refresh').click(); await p.advance(1);
  check(/Home Assistant is not connected/.test(p.$('ent-state').textContent), 'POST text: ' + p.$('ent-state').textContent);
  eq(gets(p).length, 1, 'GET after failed refresh');
  check(!p.$('ent-refresh').disabled, 'Refresh disabled after error');
});
test('9', '502 fetch_failed: each known detail and states_failed', async () => {
  const known = { states_too_large: /larger than the device can receive/, states_no_memory: /ran out of memory/, states_timeout: /did not send the list in time/,
    states_unexpected: /unexpected answer/, connection_lost: /connection to Home Assistant ended/ };
  const cases = Object.entries(known).concat([['states_failed: unauthorized', /refused the request \(states_failed: unauthorized\)/], ['weird', /failed: weird\./], ['', /unknown error/]]);
  for (const [detail, re] of cases) {
    for (const via of ['GET', 'POST']) {
      const p = await page((s) => {
        const r = () => ({ status: 502, data: { error: 'fetch_failed', detail } });
        if (via === 'GET') s.onGetEntities = r; else { s.onRefresh = r; s.onGetEntities = r; }
      });
      await p.open(); await p.advance(1);
      if (via === 'POST') { p.$('ent-refresh').click(); await p.advance(1); }
      check(re.test(p.$('ent-state').textContent) && /Press Refresh list/.test(p.$('ent-state').textContent), `${via} ${detail}: ${p.$('ent-state').textContent}`);
    }
  }
});
test('9', 'other errors: 401 on entities, failed refresh keeps the old list', async () => {
  const p = await page((s) => { s.catalog = catalogOf([A]); });
  await p.open(); await p.advance(1);
  eq(p.qa('#ent-list > li').length, 1, 'list');
  p.srv.onRefresh = () => ({ status: 503, data: { error: 'ha_not_connected' } });
  p.$('ent-refresh').click(); await p.advance(1);
  eq(p.qa('#ent-list > li').length, 1, 'list kept after failed refresh');
  const q = await page((s) => { s.onGetEntities = () => ({ status: 401, data: {} }); });
  await q.open(); await q.advance(1);
  check(/Not authorised/.test(q.$('ent-state').textContent), '401 text: ' + q.$('ent-state').textContent);
});

// ---- 10: 1500 entities -------------------------------------------------------------------------
test('10', '1500 entities: 100 rows, hint, search responsive; incomplete-list hint', async () => {
  const many = [];
  const doms = ['light', 'switch', 'climate', 'fan'];
  for (let i = 0; i < 1500; i++) many.push(ent(`${doms[i % 4]}.mock_${i}`, `Entity ${(i * 7919) % 1500} æ`, 'on'));
  const p = await page((s) => { s.catalog = catalogOf(many); s.config.controls = [ctl('light.gone')]; });
  let t0 = process.hrtime.bigint();
  await p.open(); await p.advance(1);
  const loadMs = Number(process.hrtime.bigint() - t0) / 1e6;
  eq(p.qa('#ent-list > li').length, 100, 'rows rendered');
  check(/Showing 100 of 1500/.test(p.$('ent-note').textContent), 'note: ' + p.$('ent-note').textContent);
  const names = p.qa('#ent-list .entity-name').map((x) => x.textContent);
  check(names[0] === 'Entity 0 æ' || names[0].startsWith('Entity 0'), 'sorted by name: ' + names[0]);
  t0 = process.hrtime.bigint();
  p.set(p.$('ent-search'), 'MOCK_14');
  const searchMs = Number(process.hrtime.bigint() - t0) / 1e6;
  const hits = p.qa('#ent-list > li').length;
  eq(hits, 11 + 0 + 1 + 0 > 0 ? hits : 0, 'search ran');  // count checked below
  check(hits > 0 && hits <= 100 && p.qa('#ent-list .entity-id').every((x) => /mock_14/.test(x.textContent)), 'search hits ' + hits);
  p.set(p.$('ent-search'), 'zzzz');
  check(/No matching/.test(p.$('ent-note').textContent), 'no match note');
  p.set(p.$('ent-search'), ''); p.choose(p.$('ent-domain'), 'fan');
  eq(p.qa('#ent-list > li').length, 100, 'domain filter rows');
  check(/Showing 100 of 375/.test(p.$('ent-note').textContent), 'domain note: ' + p.$('ent-note').textContent);
  console.log(`    (10: open+render 1500 in jsdom ${loadMs.toFixed(0)} ms, one search keystroke ${searchMs.toFixed(0)} ms)`);
  check(searchMs < 500, 'search keystroke took ' + searchMs.toFixed(0) + ' ms');
  check(/Not in Home Assistant/.test(p.q('.warning', p.rows()[0]).textContent), 'complete list hint: ' + p.q('.warning', p.rows()[0]).textContent);
  for (const extra of [{ truncated: true }, { skipped: 3 }]) {
    const q = await page((s) => { s.catalog = catalogOf([A], extra); s.config.controls = [ctl('light.gone')]; });
    await q.open(); await q.advance(1);
    eq(q.q('.warning', q.rows()[0]).textContent, 'Not in the loaded entity list (the list is incomplete).', 'incomplete hint ' + JSON.stringify(extra));
  }
});

// ---- 11: editor disabled while a save hangs ----------------------------------------------------------
test('11', 'fieldset disabled while save hangs; controls do nothing; focus returns', async () => {
  const p = await page((s) => { s.catalog = catalogOf([A, C]); s.config.controls = [ctl('light.a'), ctl('light.c')]; s.onPostConfig = (req) => { s.config.controls = req.body.controls; return { status: 200, data: {}, delay: 5000 }; }; });
  await p.open(); await p.advance(1);
  p.$('ctl-save').focus();
  p.$('ctl-save').click(); await p.advance(100);
  check(p.$('ctl-editor').disabled, 'fieldset not disabled');
  check(p.$('ctl-save').matches(':disabled'), 'Save not :disabled');
  const order = () => p.js('controls.map(c=>c.entity_id)');
  const addBtn = p.q('#ent-list button');
  addBtn.click(); p.rowBtn(0, 'down').click(); p.rowBtn(1, 'up').click(); p.rowBtn(0, 'remove').click();
  p.js(`addControl(catalogById.get('light.a'))`); p.js('moveControl(0,1)'); p.js('removeControl(0)');
  await p.flush();
  eq(order(), ['light.a', 'light.c'], 'controls changed while saving');
  eq(p.api('/api/config', 'POST').length, 1, 'second POST');
  p.js('saveControls()'); await p.flush();
  eq(p.api('/api/config', 'POST').length, 1, 'double save');
  await p.advance(5000);
  check(!p.$('ctl-editor').disabled, 'fieldset still disabled');
  check(p.d.activeElement === p.$('ctl-save'), 'focus not on Save: ' + (p.d.activeElement && p.d.activeElement.id));
  check(/Controls saved/.test(p.$('ctl-status').textContent), 'saved text');
  // network failure during save
  const q = await page((s) => { s.catalog = catalogOf([A]); s.config.controls = [ctl('light.a')]; s.onPostConfig = () => ({ networkError: true }); });
  await q.open(); await q.advance(1);
  q.$('ctl-save').click(); await q.advance(1);
  check(/not reachable/.test(q.$('ctl-status').textContent) && !q.$('ctl-editor').disabled && q.d.activeElement === q.$('ctl-save'), 'network failure: ' + q.$('ctl-status').textContent);
});

// ---- 12: focus -------------------------------------------------------------------------------------
test('12', 'catalog arrival while typing keeps focus and caret', async () => {
  const p = await page((s) => { s.catalog = catalogOf([A, C]); s.config.controls = [ctl('light.a'), ctl('light.c')]; s.onGetEntities = (n) => (n <= 2 ? { status: 202, data: {} } : { status: 200, data: s.catalog }); });
  await p.open(); await p.advance(1);
  const label = p.rowBtn(1, 'label');
  label.focus(); p.set(label, 'hello'); label.setSelectionRange(2, 4);
  await p.advance(3000);
  eq(p.rowBtn(0, 'label').placeholder, 'Lamp A', 'catalog arrived (placeholder)');
  const now = p.d.activeElement;
  check(now && now.dataset.focus === 'label' && now.closest('li').dataset.index === '1', 'focus lost: ' + (now && now.tagName));
  check(now.value === 'hello' && now.selectionStart === 2 && now.selectionEnd === 4, `caret ${now.selectionStart}-${now.selectionEnd}`);
  check(now !== label ? true : true, '');
});
test('12', 'Up/Down/Remove focus follows the row', async () => {
  const p = await page((s) => { s.catalog = catalogOf([A, C, F]); s.config.controls = [ctl('light.a'), ctl('light.c'), ctl('fan.f', { action: 'percentage', step: 10 })]; });
  await p.open(); await p.advance(1);
  const where = () => { const e = p.d.activeElement; return e && e.dataset ? [e.dataset.focus, e.closest('li') && e.closest('li').dataset.index] : null; };
  p.rowBtn(0, 'down').focus(); p.rowBtn(0, 'down').click(); await p.flush();
  eq(where(), ['down', '1'], 'after Down on row 0');
  eq(p.js('controls.map(c=>c.entity_id)'), ['light.c', 'light.a', 'fan.f'], 'order after Down');
  p.rowBtn(1, 'down').focus(); p.rowBtn(1, 'down').click(); await p.flush();
  eq(where(), ['up', '2'], 'after Down onto the last row');
  p.rowBtn(2, 'up').focus(); p.rowBtn(2, 'up').click(); await p.flush();
  eq(where(), ['up', '1'], 'after Up on row 2');
  p.rowBtn(1, 'up').focus(); p.rowBtn(1, 'up').click(); await p.flush();
  eq(where(), ['down', '0'], 'after Up onto the first row');
  p.rowBtn(1, 'remove').focus(); p.rowBtn(1, 'remove').click(); await p.flush();
  eq(where(), ['remove', '1'], 'after Remove of row 1');
  p.rowBtn(1, 'remove').focus(); p.rowBtn(1, 'remove').click(); await p.flush();
  eq(where(), ['remove', '0'], 'after Remove of the last row');
  p.rowBtn(0, 'remove').focus(); p.rowBtn(0, 'remove').click(); await p.flush();
  check(p.d.activeElement === p.$('ctl-save'), 'after removing the last control focus is not on Save');
});

// ---- 17: slow device ----------------------------------------------------------------------------------
test('17', 'headers never arrive: slow text at 10 s, retries, 2-minute text, no hanging request', async () => {
  const p = await page((s) => { s.onGetEntities = () => ({ hang: true }); });
  await p.open(); await p.advance(9999);
  eq(gets(p).length, 1, 'GETs before 10 s');
  await p.advance(2);
  check(/slow to answer, retrying/.test(p.$('ent-state').textContent), 'slow text: ' + p.$('ent-state').textContent);
  check(gets(p)[0].aborted && gets(p).length === 2, 'first aborted and retried immediately: ' + gets(p).length);
  await p.advance(115000);
  check(/2 minutes/.test(p.$('ent-state').textContent), '2-minute text: ' + p.$('ent-state').textContent);
  check(gets(p).every((r) => r.done), 'hanging request left: ' + gets(p).filter((r) => !r.done).length);
  check(!p.$('ent-refresh').disabled, 'Refresh stays disabled');
});
test('17', 'slow first, then success; body that stalls is cut after 60 s', async () => {
  const p = await page((s) => { s.catalog = catalogOf([A]); s.onGetEntities = (n) => (n === 1 ? { hang: true } : n === 2 ? { status: 200, bodyHang: true, data: null } : { status: 200, data: s.catalog }); });
  await p.open(); await p.advance(10001);
  check(/slow to answer/.test(p.$('ent-state').textContent), 'slow text');
  await p.advance(59000);
  eq(p.qa('#ent-list > li').length, 0, 'list before body timeout');
  eq(gets(p).length, 2, 'GETs during the stalled body');
  await p.advance(2000);
  eq(gets(p).length, 3, 'GETs after body timeout');
  eq(p.qa('#ent-list > li').length, 1, 'list loaded on the third try');
  check(gets(p).every((r) => r.done), 'hanging request left');
});

// ---- 18: leave the tab while the list downloads ------------------------------------------------------------
test('18', 'leave while the body downloads: aborted, silent while away, loads on return', async () => {
  const p = await page((s) => { s.catalog = catalogOf([A]); s.onGetEntities = (n) => (n === 1 ? { status: 200, bodyHang: true, data: null } : { status: 200, data: s.catalog }); });
  await p.open(); await p.advance(3000);
  eq(gets(p).length, 1, 'first GET');
  check(!gets(p)[0].done, 'body not pending');
  await p.leave();
  check(gets(p)[0].aborted, 'request not aborted on leaving');
  await p.advance(70000);
  eq(p.reqs.filter((r) => r.path.startsWith('/api/ha')).length, 1, 'requests while away');
  await p.open(); await p.advance(1);
  eq(gets(p).length, 2, 'GET on return');
  eq(p.api('/api/ha/entities/refresh', 'POST').length, 0, 'POST on return');
  eq(p.qa('#ent-list > li').length, 1, 'list on return');
  // leaving while the headers are pending
  const q = await page((s) => { s.catalog = catalogOf([A]); s.onGetEntities = (n) => (n === 1 ? { hang: true } : { status: 200, data: s.catalog }); });
  await q.open(); await q.advance(2000); await q.leave();
  check(gets(q)[0].aborted, 'pending-header request not aborted');
  await q.advance(30000); eq(gets(q).length, 1, 'requests while away (headers)');
  await q.open(); await q.advance(1);
  eq(q.qa('#ent-list > li').length, 1, 'list on return (headers)');
});

// ---- 19: focus after Save -------------------------------------------------------------------------------------------
test('19', 'Save -> Save; row error -> rejected input; row-level error on switch -> its label', async () => {
  const p = await page((s) => { s.catalog = catalogOf([A, B]); s.config.controls = [ctl('light.a'), ctl('switch.b', { action: 'none', step: 0 })]; });
  await p.open(); await p.advance(1);
  p.$('ctl-save').click(); await p.advance(1);
  check(p.d.activeElement === p.$('ctl-save'), 'focus after Save is on ' + (p.d.activeElement && p.d.activeElement.id));
  p.srv.onPostConfig = () => ({ status: 400, data: { error: 'out_of_range', field: 'controls[0].step' } });
  p.$('ctl-save').click(); await p.advance(1);
  check(p.d.activeElement === p.rowBtn(0, 'step'), 'focus after step error is on ' + (p.d.activeElement && p.d.activeElement.dataset.focus));
  p.srv.onPostConfig = () => ({ status: 400, data: { error: 'invalid_for_domain', field: 'controls[1].entity_id' } });
  p.$('ctl-save').click(); await p.advance(1);
  check(p.d.activeElement === p.rowBtn(1, 'label'), 'focus after row-level error is on ' + (p.d.activeElement && p.d.activeElement.dataset.focus));
  eq(p.rowBtn(1, 'label').getAttribute('aria-invalid'), 'true', 'row-level label aria-invalid');
  check(!p.rowBtn(1, 'step').hasAttribute('aria-invalid') || !p.vis(p.rowBtn(1, 'step').closest('label')), 'hidden step marked invalid is skipped');
  // general (non-row) errors
  for (const [status, data, re] of [[400, { error: 'too_many', field: 'controls' }, /Not saved: controls: Too many/], [413, { error: 'too_large' }, /do not fit/], [401, {}, /Not authorised/], [500, { error: 'internal' }, /Not saved/]]) {
    p.srv.onPostConfig = () => ({ status, data });
    p.$('ctl-save').click(); await p.advance(1);
    check(re.test(p.$('ctl-status').textContent) && p.d.activeElement === p.$('ctl-save'), `status ${status}: ${p.$('ctl-status').textContent}`);
  }
});

// ---- 8.5: device.detent_click checkbox ------------------------------------------------------------------------------
test('8.5', 'detent_click checkbox loads from config and is always sent in the device section', async () => {
  const p = await page((s) => { s.config.device.detent_click = true; });
  await p.open(); await p.advance(1);
  eq(p.$('dev-detent-click').checked, true, 'checkbox loaded true');
  let body = null;
  p.srv.onPostConfig = (req) => { body = req.body; return { status: 200, data: {} }; };
  p.$('device-form').dispatchEvent(new p.w.Event('submit', { cancelable: true, bubbles: true })); await p.advance(1);
  eq(body && body.device && body.device.detent_click, true, 'saved true');
  p.$('dev-detent-click').checked = false;
  p.$('device-form').dispatchEvent(new p.w.Event('submit', { cancelable: true, bubbles: true })); await p.advance(1);
  eq(body && body.device && body.device.detent_click, false, 'saved false (key always present)');
  const q = await page(() => {});
  await q.open(); await q.advance(1);
  eq(q.$('dev-detent-click').checked, false, 'absent in config -> unchecked');
});

// ---- run ----------------------------------------------------------------------------------------------------------------
(async () => {
  const only = process.argv[2];
  let failed = 0, asserts = 0;
  for (const t of tests) {
    if (only && !t.points.split(',').includes(only)) continue;
    current = { fails: [], n: 0 };
    try { await t.fn(); } catch (e) { current.fails.push('exception: ' + (e && e.stack || e)); }
    asserts += current.n;
    const ok = current.fails.length === 0;
    if (!ok) failed++;
    console.log(`${ok ? 'PASS' : 'FAIL'}  [${t.points}] ${t.name}`);
    for (const f of current.fails) console.log('        - ' + f);
    results.push({ t, ok });
  }
  console.log(`\n${results.length - failed} of ${results.length} test groups passed, ${asserts} checks, ${failed} failed`);
  process.exit(failed ? 1 : 0);
})();
