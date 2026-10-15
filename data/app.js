// HA Knob setup UI. Plain JavaScript, no build step.
// Security rules: DOM text is only ever set with textContent (never innerHTML), and every
// POST carries Content-Type: application/json plus X-HAKnob: 1 (the device rejects
// anything else). Secrets are write-only: the API reports only *_set flags.
'use strict';

const $ = (id) => document.getElementById(id);

let config = null;          // last GET /api/config
let statusTimer = null;     // polls /api/status while the Status or Home Assistant tab is open
let followTimer = null;

// ---- API ---------------------------------------------------------------------------

async function api(method, path, body) {
  const options = { method, cache: 'no-store', headers: {} };
  if (method === 'POST') {
    options.headers['Content-Type'] = 'application/json';
    options.headers['X-HAKnob'] = '1';
    options.body = JSON.stringify(body === undefined ? {} : body);
  }
  const response = await fetch(path, options);
  let data = null;
  try {
    data = await response.json();
  } catch (e) {
    data = null;
  }
  return { status: response.status, ok: response.ok, data };
}

const ERROR_TEXT = {
  password_required: 'A new network needs its password (or tick "Open network").',
  token_required: 'A new Home Assistant address needs a new token.',
  invalid_json: 'The request was not valid JSON.',
  nvs_write_failed: 'The device could not store the settings completely. Check them and try again.',
  out_of_range: 'Value out of range.',
  length: 'Wrong length.',
  invalid_chars: 'Contains characters that are not allowed.',
  invalid: 'Invalid value.',
  not_an_integer: 'Must be a whole number.',
  body_too_large: 'The request is too large.',
};

function describeError(result) {
  if (result.status === 401) return 'Not authorised: reload the page and enter the web password.';
  const data = result.data || {};
  const text = ERROR_TEXT[data.error] || data.error || ('HTTP ' + result.status);
  return data.field ? data.field + ': ' + text : text;
}

function showMessage(text, kind) {
  const el = $('message');
  el.textContent = text;
  el.className = 'message ' + (kind || 'info');
  el.hidden = !text;
}

function showLinkMessage(prefix, url) {
  const el = $('message');
  el.textContent = prefix;
  const link = document.createElement('a');
  link.href = url;
  link.textContent = url;
  el.appendChild(link);
  el.className = 'message success';
  el.hidden = false;
}

// ---- Tabs --------------------------------------------------------------------------

function selectTab(name) {
  document.querySelectorAll('#tabs button').forEach((b) => {
    b.classList.toggle('active', b.dataset.tab === name);
  });
  document.querySelectorAll('.tab').forEach((s) => {
    s.hidden = s.id !== 'tab-' + name;
  });
  if (statusTimer) {
    clearInterval(statusTimer);
    statusTimer = null;
  }
  if (name === 'status' || name === 'ha') {
    loadStatus();
    statusTimer = setInterval(loadStatus, name === 'ha' ? 3000 : 5000);
  }
}

// ---- Status ------------------------------------------------------------------------

function formatUptime(seconds) {
  const d = Math.floor(seconds / 86400);
  const h = Math.floor((seconds % 86400) / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  const s = seconds % 60;
  return (d ? d + 'd ' : '') + h + 'h ' + m + 'm ' + s + 's';
}

const MODE_TEXT = {
  off: 'off',
  ap: 'setup access point',
  sta: 'connected to the network',
  ap_sta: 'setup access point + network',
};

const HA_STATE_TEXT = {
  no_config: 'not configured',
  connecting: 'connecting...',
  authenticating: 'logging in...',
  subscribing: 'subscribing...',
  ready: 'connected',
  auth_failed: 'token rejected: enter a new token and save, or press Retry',
  backoff: 'not connected, retrying automatically',
};

const HA_CONFIG_ERROR_TEXT = {
  tls_not_supported: 'not connecting: https is not supported, use the http:// address',
  invalid_url: 'not connecting: the stored address cannot be used',
};

function haSummary(ha) {
  if (!ha) return '-';
  if (ha.state === 'no_config' && HA_CONFIG_ERROR_TEXT[ha.last_error]) {
    return HA_CONFIG_ERROR_TEXT[ha.last_error];
  }
  const text = HA_STATE_TEXT[ha.state] || ha.state;
  return ha.state === 'ready' && ha.ha_version ? text + ' (' + ha.ha_version + ')' : text;
}

function renderHa(ha) {
  $('ha-state').textContent = haSummary(ha);
  $('ha-version').textContent = (ha && ha.ha_version) || '-';
  $('ha-error').textContent = (ha && ha.last_error) || '-';
  $('ha-retry').disabled = !ha || (ha.state !== 'auth_failed' && ha.state !== 'backoff');
}

function renderStatus(s) {
  const rows = [
    ['Firmware', s.fw],
    ['Uptime', formatUptime(s.uptime_s)],
    ['Wi-Fi mode', MODE_TEXT[s.wifi.mode] || s.wifi.mode],
    ['Network', s.wifi.ssid || 'not configured'],
    ['Connected', s.wifi.connected ? 'yes' : 'no'],
    ['IP address', s.wifi.ip || '-'],
    ['Signal', s.wifi.connected ? s.wifi.rssi + ' dBm' : '-'],
    ['Hostname', s.wifi.hostname + '.local'],
    ['Setup AP', s.wifi.ap_active ? s.wifi.ap_ssid + ' (' + s.wifi.ap_ip + ')' : 'off'],
    ['Home Assistant', haSummary(s.ha)],
    ['Touch', s.touch_variant],
    ['Free heap', Math.round(s.heap_free / 1024) + ' KB'],
    ['Free PSRAM', Math.round(s.psram_free / 1024) + ' KB'],
  ];
  const list = $('status-list');
  list.replaceChildren();
  for (const [key, value] of rows) {
    const dt = document.createElement('dt');
    dt.textContent = key;
    const dd = document.createElement('dd');
    dd.textContent = String(value);
    list.append(dt, dd);
  }
  renderHa(s.ha);
}

async function loadStatus() {
  try {
    const result = await api('GET', '/api/status');
    if (result.ok && result.data) {
      renderStatus(result.data);
    } else {
      showMessage(describeError(result), 'error');
    }
    return result.ok ? result.data : null;
  } catch (e) {
    return null;  // device restarting or out of reach; the next poll retries
  }
}

// ---- Config ------------------------------------------------------------------------

async function loadConfig() {
  const result = await api('GET', '/api/config');
  if (!result.ok || !result.data) {
    showMessage(describeError(result), 'error');
    return;
  }
  config = result.data;
  const wifi = config.wifi || {};
  $('wifi-current').textContent = wifi.ssid || 'none';
  $('wifi-password-set').textContent = wifi.ssid
    ? (wifi.password_set ? 'password stored' : 'open network, no password stored')
    : 'not configured';
  if (!$('wifi-ssid').value) $('wifi-ssid').value = wifi.ssid || '';

  const d = config.device || {};
  $('dev-hostname').value = d.hostname;
  $('device-host-hint').textContent = d.hostname;
  $('dev-brightness').value = d.brightness;
  $('dev-dim-after').value = d.dim_after_s;
  $('dev-dim-level').value = d.dim_level;
  $('dev-off-after').value = d.off_after_s;
  $('dev-send-interval').value = d.send_interval_ms;
  $('dev-buzzer').checked = !!d.buzzer;
  $('dev-reverse').checked = !!d.encoder_reverse;
  $('dev-accel').checked = !!d.accel;

  $('web-password-set').textContent = (config.web && config.web.password_set) ? 'set' : 'not set';

  const ha = config.ha || {};
  $('ha-current').textContent = ha.url || 'none';
  $('ha-token-set').textContent = ha.token_set ? 'stored' : 'not stored';
  if (!$('ha-url').value) $('ha-url').value = ha.url || '';
}

// Sends a patch. Sections are replaced as a whole, so callers always send complete ones.
async function savePatch(patch, successText) {
  try {
    const result = await api('POST', '/api/config', patch);
    await loadConfig().catch(() => {});  // show the *_set flags as stored now
    if (!result.ok) {
      showMessage(describeError(result), 'error');
      return null;
    }
    showMessage(successText, 'success');
    return result.data;
  } catch (e) {
    showMessage('The device is not reachable.', 'error');
    return null;
  }
}

// ---- Wi-Fi -------------------------------------------------------------------------

function renderNetworks(networks) {
  const list = $('wifi-networks');
  list.replaceChildren();
  for (const net of networks) {
    const item = document.createElement('li');
    const button = document.createElement('button');
    button.type = 'button';
    button.textContent = net.ssid + (net.secure ? '' : ' (open)');
    const signal = document.createElement('span');
    signal.className = 'hint';
    signal.textContent = ' ' + net.rssi + ' dBm';
    button.addEventListener('click', () => {
      $('wifi-ssid').value = net.ssid;
      $('wifi-open').checked = !net.secure;
      $('wifi-password').disabled = !net.secure;
      $('wifi-password').value = '';
      $('wifi-password').focus();
    });
    item.append(button, signal);
    list.append(item);
  }
}

async function scanWifi() {
  $('wifi-scan').disabled = true;
  $('wifi-scan-state').textContent = 'Scanning...';
  try {
    for (let i = 0; i < 20; i++) {
      const result = await api('GET', '/api/wifi/scan');
      if (!result.ok || !result.data) {
        $('wifi-scan-state').textContent = describeError(result);
        return;
      }
      const scan = result.data;
      if (!scan.scanning) {
        renderNetworks(scan.networks);
        $('wifi-scan-state').textContent = scan.failed
          ? 'Scan failed (the device may be busy connecting). Try again.'
          : scan.networks.length + ' networks found';
        return;
      }
      await new Promise((r) => setTimeout(r, 1500));
    }
    $('wifi-scan-state').textContent = 'Scan timed out. Try again.';
  } catch (e) {
    $('wifi-scan-state').textContent = 'The device is not reachable.';
  } finally {
    $('wifi-scan').disabled = false;
  }
}

// After a Wi-Fi change: follow the device until it joins the network (or give up).
function followConnection(ssid) {
  if (followTimer) clearInterval(followTimer);
  const started = Date.now();
  followTimer = setInterval(async () => {
    const s = await loadStatus();
    if (s && s.wifi.connected && s.wifi.ssid === ssid && /^\d+\.\d+\.\d+\.\d+$/.test(s.wifi.ip)) {
      clearInterval(followTimer);
      followTimer = null;
      showLinkMessage('Connected to ' + ssid + '. The setup page is now at ', 'http://' + s.wifi.ip + '/');
      return;
    }
    if (Date.now() - started > 90000) {
      clearInterval(followTimer);
      followTimer = null;
      showMessage('The device has not joined ' + ssid + ' yet. Check the password; the device keeps ' +
        'retrying and its setup access point stays available.', 'error');
    }
  }, 3000);
}

async function saveWifi(event) {
  event.preventDefault();
  if (!config) return;
  const ssid = $('wifi-ssid').value;  // byte for byte: not trimmed
  const password = $('wifi-password').value;
  const open = $('wifi-open').checked;
  const changed = ssid !== (config.wifi.ssid || '');
  if (!ssid) {
    showMessage('Enter a network name, or use "Forget Wi-Fi".', 'error');
    return;
  }
  let pw;
  if (open) {
    pw = null;  // open network: clear any stored password
  } else if (password) {
    if (password.length < 8 || password.length > 63) {
      showMessage('The Wi-Fi password must be 8 to 63 characters.', 'error');
      return;
    }
    pw = password;
  } else if (changed) {
    showMessage(ERROR_TEXT.password_required, 'error');
    return;
  } else {
    pw = '';  // same network: keep the stored password
  }
  const result = await savePatch({ wifi: { ssid: ssid, password: pw } }, 'Wi-Fi saved.');
  if (!result) return;
  $('wifi-password').value = '';
  if (result.restart_wifi) {
    showMessage('Wi-Fi saved. The device now connects to ' + ssid + '...', 'success');
    followConnection(ssid);
  } else {
    showMessage('Wi-Fi saved (no change).', 'success');
  }
}

async function forgetWifi() {
  if (!confirm('Forget the stored Wi-Fi network? The device returns to setup mode.')) return;
  await savePatch({ wifi: { ssid: '', password: null } }, 'Wi-Fi forgotten. The device starts its setup access point.');
  $('wifi-ssid').value = '';
}

// ---- Home Assistant ----------------------------------------------------------------

async function saveHa(event) {
  event.preventDefault();
  if (!config) return;
  const url = $('ha-url').value.trim();
  const token = $('ha-token').value.trim();
  if (!url) {
    showMessage('Enter the Home Assistant address, or use "Remove Home Assistant".', 'error');
    return;
  }
  if (/^https:/i.test(url)) {
    showMessage('https is not supported: the device would not connect. Use the http:// address ' +
      '(Home Assistant on the local network).', 'error');
    return;
  }
  if (!token && !(config.ha && config.ha.token_set)) {
    showMessage('Enter the long-lived access token.', 'error');
    return;
  }
  // The URL goes with every ha section; an empty token keeps the stored one.
  const result = await savePatch({ ha: { url: url, token: token } }, 'Home Assistant saved. Connecting...');
  if (!result) return;
  $('ha-token').value = '';
  loadStatus();
}

async function forgetHa() {
  if (!confirm('Remove the Home Assistant address and token from the device?')) return;
  const result = await savePatch({ ha: { url: '', token: null } }, 'Home Assistant removed.');
  if (result) {
    $('ha-url').value = '';
    $('ha-token').value = '';
    loadStatus();
  }
}

async function retryHa() {
  try {
    const result = await api('POST', '/api/ha/retry', {});
    showMessage(result.ok ? 'Reconnecting to Home Assistant...' : describeError(result),
      result.ok ? 'success' : 'error');
    loadStatus();
  } catch (e) {
    showMessage('The device is not reachable.', 'error');
  }
}

// ---- Device ------------------------------------------------------------------------

function intValue(id) {
  const n = Number($(id).value);
  return Number.isFinite(n) ? Math.round(n) : 0;
}

async function saveDevice(event) {
  event.preventDefault();
  const device = {
    hostname: $('dev-hostname').value.trim(),
    brightness: intValue('dev-brightness'),
    dim_after_s: intValue('dev-dim-after'),
    dim_level: intValue('dev-dim-level'),
    off_after_s: intValue('dev-off-after'),
    send_interval_ms: intValue('dev-send-interval'),
    buzzer: $('dev-buzzer').checked,
    encoder_reverse: $('dev-reverse').checked,
    accel: $('dev-accel').checked,
  };
  await savePatch({ device: device }, 'Device settings saved.');
}

async function setWebPassword(event) {
  event.preventDefault();
  const pw = $('web-password').value;
  if (!pw) {
    showMessage('Enter the new web password.', 'error');
    return;
  }
  const result = await savePatch({ web: { password: pw } },
    'Web password set. Your browser will ask for it (user name admin).');
  if (result) $('web-password').value = '';
}

async function removeWebPassword() {
  if (!confirm('Remove the web password? Anyone on your network can then change the settings.')) return;
  await savePatch({ web: { password: null } }, 'Web password removed.');
}

async function systemAction(path, question, text) {
  if (!confirm(question)) return;
  try {
    const result = await api('POST', path, {});
    showMessage(result.ok ? text : describeError(result), result.ok ? 'success' : 'error');
  } catch (e) {
    showMessage('The device is not reachable.', 'error');
  }
}

// ---- Start -------------------------------------------------------------------------

document.addEventListener('DOMContentLoaded', () => {
  document.querySelectorAll('#tabs button').forEach((b) => {
    b.addEventListener('click', () => selectTab(b.dataset.tab));
  });
  $('status-refresh').addEventListener('click', loadStatus);
  $('wifi-form').addEventListener('submit', saveWifi);
  $('wifi-scan').addEventListener('click', scanWifi);
  $('wifi-forget').addEventListener('click', forgetWifi);
  $('wifi-open').addEventListener('change', () => {
    $('wifi-password').disabled = $('wifi-open').checked;
  });
  $('ha-form').addEventListener('submit', saveHa);
  $('ha-forget').addEventListener('click', forgetHa);
  $('ha-retry').addEventListener('click', retryHa);
  $('device-form').addEventListener('submit', saveDevice);
  $('web-form').addEventListener('submit', setWebPassword);
  $('web-remove').addEventListener('click', removeWebPassword);
  $('reboot').addEventListener('click', () => systemAction('/api/reboot',
    'Reboot the device?', 'Rebooting. Reload this page in about 10 seconds.'));
  $('factory-reset').addEventListener('click', () => systemAction('/api/factory-reset',
    'Erase all settings and restart in setup mode?',
    'Factory reset. The device restarts with its setup access point (name and password on its screen).'));

  loadConfig().catch(() => showMessage('The device is not reachable.', 'error'));
  selectTab('status');
});
