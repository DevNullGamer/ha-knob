// HA Knob setup UI. Plain JavaScript, no build step.
// Security rules: DOM text is only ever set with textContent (never innerHTML), and every
// POST carries Content-Type: application/json plus X-HAKnob: 1 (the device rejects
// anything else). Secrets are write-only: the API reports only *_set flags.
'use strict';

const $ = (id) => document.getElementById(id);

let config = null;          // last GET /api/config
let statusTimer = null;     // polls /api/status while the Status or Home Assistant tab is open
let followTimer = null;
let haFormFilled = false;   // the certificate fields were filled from the stored config

// ---- API ---------------------------------------------------------------------------

// signal: optional AbortSignal (fetch then rejects when it is aborted). onResponse:
// optional, called when the response headers have arrived (before the body is read).
async function api(method, path, body, signal, onResponse) {
  const options = { method, cache: 'no-store', headers: {}, signal };
  if (method === 'POST') {
    options.headers['Content-Type'] = 'application/json';
    options.headers['X-HAKnob'] = '1';
    options.body = JSON.stringify(body === undefined ? {} : body);
  }
  const response = await fetch(path, options);
  if (onResponse) onResponse(response);
  let data = null;
  try {
    data = await response.json();
  } catch (e) {
    data = null;
  }
  return { status: response.status, ok: response.ok, data, headers: response.headers };
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const PEM_ONLY = 'Paste only the -----BEGIN CERTIFICATE----- ... -----END CERTIFICATE----- ' +
  'block(s) of the CA, nothing else (no "subject=" or "issuer=" lines, never a private key).';

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
  invalid_for_domain: 'Not offered for this kind of entity.',
  unsupported_domain: 'This kind of entity is not supported.',
  too_many: 'Too many entries.',
  too_large: 'The settings do not fit into the device storage. Shorten labels or remove ' +
    'controls.',
};

// Texts for one field's error codes (checked before ERROR_TEXT).
const FIELD_ERROR_TEXT = {
  'ha.ca_pem/length': 'The CA certificate text is too long (at most 4096 characters). ' + PEM_ONLY,
  'ha.ca_pem/not_a_certificate': 'This is not a CA certificate. ' + PEM_ONLY,
  'ha.ca_pem/too_many': 'At most 3 certificates. ' + PEM_ONLY,
  'ha.ca_pem/invalid_base64': 'The certificate text is damaged or incomplete. ' + PEM_ONLY,
  'ha.ca_pem/invalid_der': 'The certificate text is damaged or incomplete. ' + PEM_ONLY,
  'ha.ca_pem/requires_https': 'A CA certificate is only used with an https:// address.',
  'ha.tls_insecure/requires_https': '"Do not verify" is only used with an https:// address.',
  'ha.tls_insecure/conflicts_with_ca_pem': 'Use either a CA certificate or "Do not verify", not both.',
};

// errorText(data) may return a better text for a 4xx/5xx answer (or null).
function describeError(result, errorText) {
  if (result.status === 401) return 'Not authorised: reload the page and enter the web password.';
  const data = result.data || {};
  const text = (errorText && errorText(data)) || FIELD_ERROR_TEXT[data.field + '/' + data.error] ||
    ERROR_TEXT[data.error] || data.error || ('HTTP ' + result.status);
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
  if (name === 'controls') openControls();
  else stopEntities();  // no entity polling while the Controls tab is not shown
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
  invalid_url: 'not connecting: the stored address cannot be used',
};

const TLS_TEXT = {
  none: 'not used (http)',
  bundle: 'public certificate authorities (built-in list)',
  custom_ca: 'own CA certificate',
  insecure: 'OFF: the certificate is not verified (insecure)',
};

// Explanations shown next to last_error.
const HA_ERROR_TEXT = {
  tls_cert_rejected: 'the server certificate was not accepted. With a public certificate leave ' +
    'the CA field empty; with your own CA paste that CA. With an IP address in the URL the ' +
    'certificate is only accepted if its name (CN) is exactly that IP address and it has no other ' +
    'names (SAN): use the host name the certificate was issued for, or "Do not verify".',
  tls_ca_invalid: 'the stored CA certificate cannot be used by the device. ' + PEM_ONLY,
  tls_handshake_failed: 'the TLS connection failed. The port may not speak https, or the server ' +
    'only allows TLS 1.3 (the device supports TLS 1.2).',
  tls_timeout: 'the server did not complete the TLS handshake in time.',
  connect_failed: 'the address could not be reached (wrong host or port, or the server is down).',
};

function haSummary(ha) {
  if (!ha) return '-';
  if (ha.state === 'no_config' && HA_CONFIG_ERROR_TEXT[ha.last_error]) {
    return HA_CONFIG_ERROR_TEXT[ha.last_error];
  }
  let text = HA_STATE_TEXT[ha.state] || ha.state;
  if (ha.state === 'ready' && ha.ha_version) text += ' (' + ha.ha_version + ')';
  if (ha.state === 'ready' && ha.tls === 'insecure') text += ', certificate NOT verified';
  return text;
}

function renderHa(ha) {
  $('ha-state').textContent = haSummary(ha);
  $('ha-version').textContent = (ha && ha.ha_version) || '-';
  const error = (ha && ha.last_error) || '';
  $('ha-error').textContent = error ? (HA_ERROR_TEXT[error] ? error + ': ' + HA_ERROR_TEXT[error] : error) : '-';
  $('ha-tls-state').textContent = (ha && (TLS_TEXT[ha.tls] || ha.tls)) || '-';
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

// Returns true when the config was read.
async function loadConfig() {
  const result = await api('GET', '/api/config');
  if (!result.ok || !result.data) {
    showMessage(describeError(result), 'error');
    return false;
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
  if (!haFormFilled) {
    // Only once: a failed save must not overwrite what the user is editing.
    haFormFilled = true;
    $('ha-ca-pem').value = ha.ca_pem || '';
    $('ha-tls-insecure').checked = !!ha.tls_insecure;
  }
  updateHaTlsForm();
  return true;
}

// Sends a patch. Sections are replaced as a whole, so callers always send complete ones.
// errorText: optional, see describeError().
async function savePatch(patch, successText, errorText) {
  try {
    const result = await api('POST', '/api/config', patch);
    await loadConfig().catch(() => {});  // show the *_set flags as stored now
    if (!result.ok) {
      showMessage(describeError(result, errorText), 'error');
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

function isHttps(url) {
  return /^https:/i.test(url);
}

// The certificate fields apply to https addresses only; "Do not verify" replaces the CA.
function updateHaTlsForm() {
  const https = isHttps($('ha-url').value.trim());
  const insecure = $('ha-tls-insecure').checked;
  $('ha-tls').hidden = !https;
  $('ha-ca-pem').disabled = insecure;
  $('ha-insecure-warning').hidden = !insecure;
}

function confirmInsecure() {
  if ($('ha-tls-insecure').checked &&
      !confirm('Turn off certificate verification? Anyone who can intercept the connection could ' +
        'pretend to be Home Assistant and capture the token. Use this only on a network you trust.')) {
    $('ha-tls-insecure').checked = false;
  }
  updateHaTlsForm();
}

// The PEM as the device stores it (CRLF -> LF, trailing blanks per line removed, trimmed,
// one final LF), so an unchanged CA is recognised.
function normalizePem(text) {
  const pem = text.replace(/\r\n?/g, '\n').split('\n').map((l) => l.replace(/[ \t]+$/, '')).join('\n').trim();
  return pem ? pem + '\n' : '';
}

// "https:", host and port of an address, or null.
function endpointOf(url) {
  try {
    const u = new URL(url);
    return { https: u.protocol === 'https:', host: u.hostname.toLowerCase(),
      port: u.port || (u.protocol === 'https:' ? '443' : '80') };
  } catch (e) {
    return null;
  }
}

// Mirrors the device's rule: on the same https host:port, turning verification off or a
// different CA certificate needs the token again (else the address changed).
function trustWeakened(oldHa, newHa) {
  const a = endpointOf(oldHa.url || '');
  const b = endpointOf(newHa.url);
  if (!a || !b || !a.https || !b.https || a.host !== b.host || a.port !== b.port) return false;
  return (newHa.tls_insecure && !oldHa.tls_insecure) ||
    (newHa.ca_pem !== '' && newHa.ca_pem !== (oldHa.ca_pem || ''));
}

async function saveHa(event) {
  event.preventDefault();
  if (!config) return;
  const url = $('ha-url').value.trim();
  const token = $('ha-token').value.trim();
  if (!url) {
    showMessage('Enter the Home Assistant address, or use "Remove Home Assistant".', 'error');
    return;
  }
  if (!token && !(config.ha && config.ha.token_set)) {
    showMessage('Enter the long-lived access token.', 'error');
    return;
  }
  const https = isHttps(url);
  const insecure = https && $('ha-tls-insecure').checked;
  // Every ha section carries url, ca_pem and tls_insecure (sections are replaced as a whole);
  // an empty token keeps the stored one.
  const ha = {
    url: url,
    ca_pem: https && !insecure ? normalizePem($('ha-ca-pem').value) : '',
    tls_insecure: insecure,
    token: token,
  };
  const oldHa = config.ha || {};
  const result = await savePatch({ ha: ha }, 'Home Assistant saved. Connecting...', (data) => {
    if (data.error !== 'token_required') return null;
    return trustWeakened(oldHa, ha)
      ? 'A weaker certificate check (verification turned off, or a different CA certificate) ' +
        'needs the token again: enter the long-lived access token and save.'
      : null;
  });
  if (!result) return;
  $('ha-token').value = '';
  loadStatus();
}

async function forgetHa() {
  if (!confirm('Remove the Home Assistant address and token from the device?')) return;
  const result = await savePatch({ ha: { url: '', ca_pem: '', tls_insecure: false, token: null } },
    'Home Assistant removed.');
  if (result) {
    $('ha-url').value = '';
    $('ha-token').value = '';
    $('ha-ca-pem').value = '';
    $('ha-tls-insecure').checked = false;
    updateHaTlsForm();
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

// ---- Controls ----------------------------------------------------------------------
// The action table (actions, presses, step ranges, limits) comes only from
// /api/meta/actions; nothing of it is repeated here. The entity list is the device's
// catalog from /api/ha/entities.

let meta = null;             // GET /api/meta/actions
let domainSpecs = new Map(); // domain -> { actions: Map(name -> action), names: [..], presses: [..] }
let catalog = null;          // last 200 body of GET /api/ha/entities
let catalogById = new Map();
let catalogSorted = [];      // [{entity, id, name}] sorted by name; id / name lower-cased for search
let catalogGen = 0;          // a newer load (or leaving the tab) cancels an older poll loop
let catalogLoading = false;  // a load / poll loop is running
let catalogAbort = null;     // AbortController of the running loop
let catalogResume = false;   // a loop was stopped by leaving the tab: continue on return
let controls = [];           // the list being edited: {entity_id, label, action, step, press, error, errorField}
let controlsLoaded = false;  // controls came from the config (never save an unloaded list)
let controlsDirty = false;   // edited since the last load / save
let controlsSaving = false;

const STEP_TOLERANCE = 1e-9;   // as the device (DomainActions stepInRange)
const ENTITY_POLL_MS = 1000;
const ENTITY_POLL_LIMIT_MS = 120000;
const ENTITY_FETCH_TIMEOUT_MS = 10000;  // until the response headers arrive
const ENTITY_BODY_TIMEOUT_MS = 60000;   // then for the body
const ENTITY_SHOW_MAX = 100;   // rows shown in the picker at once (refine the search for more)
const collator = new Intl.Collator(undefined, { sensitivity: 'base' });

const CATALOG_ERROR_TEXT = {
  states_too_large: 'the list is larger than the device can receive (4 MB)',
  states_no_memory: 'the device ran out of memory while receiving the list',
  states_timeout: 'Home Assistant did not send the list in time',
  states_unexpected: 'Home Assistant sent an unexpected answer',
  connection_lost: 'the connection to Home Assistant ended before the list arrived',
};

// Prefixes for a row error, by the field the device named.
const CONTROL_FIELD_TEXT = {
  label: 'Label: ',
  step: 'Step: ',
  action: 'Turning the knob: ',
  press: 'Pressing the knob: ',
  entity_id: 'Entity: ',
};

const domainOf = (entityId) => entityId.split('.')[0];
const labelBytes = (text) => new TextEncoder().encode(text).length;

function actionSpec(entityId, action) {
  const domain = domainSpecs.get(domainOf(entityId));
  return (domain && domain.actions.get(action)) || null;
}

// The action's step range, or null for actions without a step (e.g. "none").
function stepRange(entityId, action) {
  const spec = actionSpec(entityId, action);
  return spec && spec.step ? spec.step : null;
}

function setMeta(data) {
  meta = data;
  domainSpecs = new Map();
  const select = $('ent-domain');
  select.replaceChildren();
  const all = document.createElement('option');
  all.value = '';
  all.textContent = 'All';
  select.append(all);
  for (const d of data.domains) {
    domainSpecs.set(d.domain, {
      actions: new Map(d.actions.map((a) => [a.name, a])),
      names: d.actions.map((a) => a.name),
      presses: d.presses.slice(),
    });
    const option = document.createElement('option');
    option.value = d.domain;
    option.textContent = d.domain;
    select.append(option);
  }
}

// The editor list from the page's cached config (loadConfig() keeps it current).
function setControlsFromConfig() {
  controls = (config.controls || []).map((c) => ({
    entity_id: c.entity_id, label: c.label || '', action: c.action, step: c.step, press: c.press,
    error: '', errorField: '',
  }));
  controlsLoaded = true;
  controlsDirty = false;
}

function updateControlsBusy() {
  // The fieldset disables every input, select and button of the editor and the picker.
  $('ctl-editor').disabled = controlsSaving;
  $('ctl-save').disabled = !controlsLoaded || controlsSaving;
  $('ent-refresh').disabled = catalogLoading;
}

async function openControls() {
  if (!meta) {
    try {
      const result = await api('GET', '/api/meta/actions');
      if (!result.ok || !result.data || !Array.isArray(result.data.domains)) {
        showControlsStatus('The action table could not be loaded: ' + describeError(result), 'error');
        return;
      }
      setMeta(result.data);
    } catch (e) {
      showControlsStatus('The device is not reachable.', 'error');
      return;
    }
  }
  if (!config) {
    await loadConfig().catch(() => false);
    if (!config) {
      showControlsStatus('The settings could not be loaded. Open the tab again to retry.', 'error');
      return;
    }
  }
  showControlsStatus('', 'info');  // an earlier load error is stale now
  if (!controlsDirty && !controlsSaving) setControlsFromConfig();
  renderControls();
  renderEntities();
  updateControlsBusy();
  // Q6-2: fetched automatically only while no list is loaded; afterwards only by Refresh.
  // A loop stopped by leaving the tab continues (GET only: the device keeps fetching).
  if (!catalogLoading && (!catalog || catalogResume)) loadEntities(false);
}

function showControlsStatus(text, kind) {
  const el = $('ctl-status');
  el.textContent = text;
  el.className = 'message ' + (kind || 'info');
  el.hidden = !text;
}

function markDirty() {
  controlsDirty = true;
  $('ctl-dirty').hidden = false;
}

// ---- Controls: entity list

function setEntityState(text) {
  $('ent-state').textContent = text;
}

function setCatalog(data) {
  catalog = data;
  catalogById = new Map(data.entities.map((e) => [e.id, e]));
  catalogSorted = data.entities
    .map((e) => ({ entity: e, id: e.id.toLowerCase(), name: (e.name || '').toLowerCase() }))
    .sort((a, b) => collator.compare(a.entity.name || a.entity.id, b.entity.name || b.entity.id) ||
      (a.entity.id < b.entity.id ? -1 : a.entity.id > b.entity.id ? 1 : 0));
  let text = data.entities.length + ' entities';
  if (data.truncated) text += ' (list cut: only the first ' + data.entities.length + ' are shown)';
  if (data.skipped) text += ', ' + data.skipped + ' could not be read';
  setEntityState(text + '.');
  renderControls();  // "not in the list" hints and the entity's own action list; keeps focus
  renderEntities();
}

function entityErrorText(result) {
  const data = result.data || {};
  if (result.status === 503 && data.error === 'ha_not_connected') {
    return 'Home Assistant is not connected, so the entity list cannot be loaded. Check the ' +
      'Home Assistant tab, then press Refresh list.';
  }
  if (result.status === 502 && data.error === 'fetch_failed') {
    const detail = String(data.detail || '');
    const known = CATALOG_ERROR_TEXT[detail];
    const text = known || (detail.startsWith('states_failed') ? 'Home Assistant refused the ' +
      'request (' + detail + ')' : detail || 'unknown error');
    return 'Loading the entity list failed: ' + text + '. Press Refresh list to try again.';
  }
  return 'Loading the entity list failed: ' + describeError(result);
}

// Stops a running load (the Controls tab was left); openControls() continues it.
function stopEntities() {
  if (!catalogLoading) return;
  catalogGen++;
  if (catalogAbort) catalogAbort.abort();
  catalogAbort = null;
  catalogLoading = false;
  catalogResume = true;
  setEntityState('');
  updateControlsBusy();
}

// One request of the loop: aborted by stopEntities() or by a timeout (then result is null
// and timedOut true).
async function entityRequest(method, path, controller, deadline) {
  let timedOut = false;
  const request = new AbortController();
  const forward = () => request.abort();
  controller.signal.addEventListener('abort', forward);
  const expire = () => {
    timedOut = true;
    request.abort();
  };
  // ENTITY_FETCH_TIMEOUT_MS until the headers arrive; then the body (a list of a few hundred
  // KB on a weak link) gets ENTITY_BODY_TIMEOUT_MS.
  const left = () => Math.max(0, deadline - Date.now());  // the loop's overall limit
  let timer = setTimeout(expire, Math.min(ENTITY_FETCH_TIMEOUT_MS, left()));
  const onResponse = () => {
    clearTimeout(timer);
    timer = setTimeout(expire, Math.min(ENTITY_BODY_TIMEOUT_MS, left()));
  };
  try {
    const result = await api(method, path, method === 'POST' ? {} : undefined, request.signal,
      onResponse);
    // api() reads an aborted body as null: that was the body timeout, not an answer.
    return timedOut ? { result: null, timedOut: true } : { result };
  } catch (e) {
    if (timedOut) return { result: null, timedOut: true };
    throw e;
  } finally {
    clearTimeout(timer);
    controller.signal.removeEventListener('abort', forward);
  }
}

// refresh: ask the device to fetch a new list first (POST), else take the current one.
async function loadEntities(refresh) {
  if (catalogAbort) catalogAbort.abort();
  const gen = ++catalogGen;
  const current = () => gen === catalogGen;
  const controller = new AbortController();
  catalogAbort = controller;
  catalogLoading = true;
  catalogResume = false;
  updateControlsBusy();
  setEntityState(refresh ? 'Asking Home Assistant for a new list...' : 'Loading the entity list...');
  try {
    await pollEntities(refresh, current, controller);
  } finally {
    if (current()) {
      catalogLoading = false;
      catalogAbort = null;
      updateControlsBusy();
    }
  }
}

// On 202 it polls every second for at most 2 minutes; "busy" waits for Retry-After; a
// request that times out is retried within the 2 minutes.
async function pollEntities(refresh, current, controller) {
  const started = Date.now();
  const deadline = started + ENTITY_POLL_LIMIT_MS;
  try {
    if (refresh) {
      const { result, timedOut } = await entityRequest('POST', '/api/ha/entities/refresh', controller,
        deadline);
      if (!current()) return;
      if (timedOut) {
        setEntityState('The device did not answer in time. Press Refresh list to try again.');
        return;
      }
      if (result.status !== 202) {
        setEntityState(entityErrorText(result));
        return;
      }
    }
    while (Date.now() < deadline) {
      const { result, timedOut } = await entityRequest('GET', '/api/ha/entities', controller, deadline);
      if (!current()) return;
      const data = (result && result.data) || {};
      let delay = ENTITY_POLL_MS;
      if (timedOut) {
        setEntityState('The device is slow to answer, retrying...');
        delay = 0;
      } else if (result.status === 200 && Array.isArray(data.entities)) {
        setCatalog(data);
        return;
      } else if (result.status === 202) {
        setEntityState('Home Assistant is sending the entity list... (' +
          Math.round((Date.now() - started) / 1000) + ' s)');
      } else if (result.status === 503 && data.error === 'busy') {
        const after = Number(result.headers.get('Retry-After'));
        delay = Number.isFinite(after) && after > 0 ? Math.min(after, 10) * 1000 : ENTITY_POLL_MS;
      } else {
        setEntityState(entityErrorText(result));
        return;
      }
      await sleep(delay);
      if (!current()) return;
    }
    setEntityState('The entity list did not arrive within 2 minutes. Press Refresh list to try again.');
  } catch (e) {
    if (current()) setEntityState('The device is not reachable. Press Refresh list to try again.');
  }
}

function renderEntities() {
  const list = $('ent-list');
  list.replaceChildren();
  if (!meta) return;
  const query = $('ent-search').value.trim().toLowerCase();
  const domain = $('ent-domain').value;
  const full = controls.length >= meta.max_controls;
  const matches = catalogSorted.filter((e) =>
    (!domain || domainOf(e.entity.id) === domain) &&
    (!query || e.id.includes(query) || e.name.includes(query)));
  for (const { entity } of matches.slice(0, ENTITY_SHOW_MAX)) {
    const item = document.createElement('li');
    const text = document.createElement('div');
    text.className = 'entity-text';
    const name = document.createElement('span');
    name.className = 'entity-name';
    name.textContent = entity.name || entity.id;
    const id = document.createElement('span');
    id.className = 'entity-id hint';
    id.textContent = entity.id + ' - ' + (entity.state || '');
    text.append(name, id);
    const add = document.createElement('button');
    add.type = 'button';
    add.textContent = 'Add';
    add.setAttribute('aria-label', 'Add ' + (entity.name || entity.id));
    add.disabled = full;
    add.addEventListener('click', () => addControl(entity));
    item.append(text, add);
    list.append(item);
  }
  let note = '';
  if (full) {
    note = 'The knob holds at most ' + meta.max_controls + ' controls: remove one to add another. ';
  }
  if (catalog) {
    if (matches.length === 0) note += 'No matching entities.';
    else if (matches.length > ENTITY_SHOW_MAX) {
      note += 'Showing ' + ENTITY_SHOW_MAX + ' of ' + matches.length + ' matching entities; refine the search.';
    }
  }
  $('ent-note').textContent = note;
}

// ---- Controls: selected list

// The step a control gets for an action: the entity's own default for its default action
// (catalog step_default, e.g. a fan's percentage_step), else the table default; 0 without a step.
function defaultStep(entityId, action) {
  const range = stepRange(entityId, action);
  if (!range) return 0;
  const entity = catalogById.get(entityId);
  if (entity && entity.actions[0] === action && Number.isFinite(entity.step_default) &&
      stepInRange(range, entity.step_default)) {
    return entity.step_default;
  }
  return range.default;
}

function stepInRange(range, step) {
  return Number.isFinite(step) && step >= range.min - STEP_TOLERANCE && step <= range.max + STEP_TOLERANCE;
}

function addControl(entity) {
  if (controlsSaving || !controlsLoaded) return;
  if (!meta || controls.length >= meta.max_controls) {
    showControlsStatus('The knob holds at most ' + (meta ? meta.max_controls : '-') +
      ' controls: remove one to add another.', 'error');
    renderEntities();
    return;
  }
  const domain = domainSpecs.get(domainOf(entity.id));
  if (!domain) return;  // the catalog lists supported domains only
  const action = domain.actions.has(entity.actions[0]) ? entity.actions[0] : domain.names[0];
  controls.push({
    entity_id: entity.id, label: '', action: action, step: defaultStep(entity.id, action),
    press: domain.presses[0], error: '', errorField: '',
  });
  markDirty();
  showControlsStatus('', 'info');
  renderControls();
  renderEntities();
}

// Options for a select: the offered values plus the current one (a saved value is always offered).
function fillSelect(select, values, current) {
  const list = values.slice();
  if (current && !list.includes(current)) list.push(current);
  for (const value of list) {
    const option = document.createElement('option');
    option.value = value;
    option.textContent = value;
    select.append(option);
  }
  select.value = current;
}

// Encoder actions offered for a control: the entity's own (catalog) if it is listed, else
// all actions of its domain.
function actionChoices(control) {
  const domain = domainSpecs.get(domainOf(control.entity_id));
  const names = domain ? domain.names : [];
  const entity = catalogById.get(control.entity_id);
  return entity ? entity.actions.filter((a) => names.includes(a)) : names;
}

function controlTitle(control) {
  const entity = catalogById.get(control.entity_id);
  return control.label || (entity && entity.name) || control.entity_id;
}

// Shows a row's error; field ("label", "step", "action", "press" or "" for the whole row)
// marks that input aria-invalid. Every input of the row is described by the error text.
function setRowError(item, control, text, field) {
  control.error = text;
  control.errorField = text ? (field || '') : '';
  item.classList.toggle('invalid', !!text);
  const el = item.querySelector('.control-error');
  el.textContent = text;
  el.hidden = !text;
  item.querySelectorAll('[data-field]').forEach((input) => {
    const invalid = !!text && (!control.errorField || control.errorField === input.dataset.field);
    if (invalid) input.setAttribute('aria-invalid', 'true');
    else input.removeAttribute('aria-invalid');
    if (text) input.setAttribute('aria-describedby', el.id);
    else input.removeAttribute('aria-describedby');
  });
}

function labeled(text, input) {
  const label = document.createElement('label');
  label.append(document.createTextNode(text), input);
  return label;
}

function moveControl(index, delta) {
  const to = index + delta;
  if (controlsSaving || to < 0 || to >= controls.length) return;
  const [control] = controls.splice(index, 1);
  controls.splice(to, 0, control);
  markDirty();
  // Keep the focus on the same button of the moved row (the other one at the list's end).
  const atEnd = delta < 0 ? to === 0 : to === controls.length - 1;
  renderControls({ index: to, field: atEnd ? (delta < 0 ? 'down' : 'up') : (delta < 0 ? 'up' : 'down') });
}

function removeControl(index) {
  if (controlsSaving) return;
  controls.splice(index, 1);
  markDirty();
  // Focus the Remove button of the row that took its place (or the new last row).
  renderControls(controls.length ? { index: Math.min(index, controls.length - 1), field: 'remove' }
    : { element: $('ctl-save') });
  renderEntities();
}

function rowButton(text, ariaLabel, field, disabled, onClick) {
  const button = document.createElement('button');
  button.type = 'button';
  button.textContent = text;
  button.setAttribute('aria-label', ariaLabel);
  button.dataset.focus = field;
  button.disabled = disabled;
  button.addEventListener('click', onClick);
  return button;
}

function missingText() {
  return catalog && (catalog.truncated || catalog.skipped)
    ? 'Not in the loaded entity list (the list is incomplete).'
    : 'Not in Home Assistant (not in the entity list).';
}

function controlRow(control, index) {
  const item = document.createElement('li');
  item.dataset.index = String(index);
  const entity = catalogById.get(control.entity_id);
  const domain = domainSpecs.get(domainOf(control.entity_id));
  const n = index + 1;

  const title = document.createElement('div');
  title.className = 'control-title';
  title.textContent = n + '. ' + controlTitle(control);
  const id = document.createElement('div');
  id.className = 'control-id hint';
  id.textContent = control.entity_id + (entity ? ' - ' + (entity.state || '') : '');
  item.append(title, id);
  if (catalog && !entity) {
    const missing = document.createElement('div');
    missing.className = 'hint warning';
    missing.textContent = missingText();
    item.append(missing);
  }

  const label = document.createElement('input');
  label.type = 'text';
  label.dataset.field = 'label';
  label.dataset.focus = 'label';
  label.value = control.label;
  label.placeholder = (entity && entity.name) || 'name from Home Assistant';
  label.addEventListener('input', () => {
    control.label = label.value;
    markDirty();
    title.textContent = n + '. ' + controlTitle(control);
    const bytes = labelBytes(label.value);
    setRowError(item, control, bytes > meta.max_label_bytes ? labelError(bytes) : '', 'label');
  });
  item.append(labeled('Label (empty = name from Home Assistant)', label));

  const fields = document.createElement('div');
  fields.className = 'control-fields';
  const action = document.createElement('select');
  action.dataset.field = 'action';
  action.dataset.focus = 'action';
  fillSelect(action, actionChoices(control), control.action);
  const step = document.createElement('input');
  step.type = 'number';
  step.dataset.field = 'step';
  step.dataset.focus = 'step';
  const stepLabel = labeled('Step', step);
  const press = document.createElement('select');
  press.dataset.field = 'press';
  press.dataset.focus = 'press';
  fillSelect(press, domain ? domain.presses : [], control.press);

  const showStep = () => {
    const range = stepRange(control.entity_id, control.action);
    stepLabel.hidden = !range;
    if (!range) return;
    step.min = String(range.min);
    step.max = String(range.max);
    // a fan's percentage step is HA's exact 100/speeds (e.g. 33.333..), not a multiple of 1
    step.step = control.action === 'percentage' ? 'any' : String(range.min);
    stepLabel.firstChild.textContent = 'Step (' + range.min + ' to ' + range.max + ')';
    step.value = Number.isFinite(control.step) ? String(control.step) : '';
  };
  action.addEventListener('change', () => {
    control.action = action.value;
    control.step = defaultStep(control.entity_id, control.action);
    markDirty();
    setRowError(item, control, '');
    showStep();
  });
  step.addEventListener('input', () => {
    control.step = step.value === '' ? NaN : Number(step.value);
    markDirty();
    setRowError(item, control, '');
  });
  press.addEventListener('change', () => {
    control.press = press.value;
    markDirty();
    setRowError(item, control, '');
  });
  showStep();
  fields.append(labeled('Turning the knob', action), stepLabel, labeled('Pressing the knob', press));
  item.append(fields);

  const buttons = document.createElement('div');
  buttons.className = 'row';
  const remove = rowButton('Remove', 'Remove control ' + n, 'remove', false, () => removeControl(index));
  remove.className = 'danger';
  buttons.append(
    rowButton('Up', 'Move control ' + n + ' up', 'up', index === 0, () => moveControl(index, -1)),
    rowButton('Down', 'Move control ' + n + ' down', 'down', index === controls.length - 1,
      () => moveControl(index, 1)),
    remove);
  item.append(buttons);

  const error = document.createElement('p');
  error.className = 'control-error hint warning';
  error.id = 'ctl-error-' + index;
  item.append(error);
  setRowError(item, control, control.error, control.errorField);
  return item;
}

// The focused element of the list as {index, field, start, end}, or null.
function listFocus() {
  const active = document.activeElement;
  const item = active && active.closest ? active.closest('#ctl-list > li') : null;
  if (!item || !active.dataset.focus) return null;
  const focus = { index: Number(item.dataset.index), field: active.dataset.focus };
  if (active.type === 'text') {
    focus.start = active.selectionStart;
    focus.end = active.selectionEnd;
  }
  return focus;
}

// Rebuilds the list. focus: where to put the focus afterwards ({index, field} or
// {element}); default: where it was (same row, field and caret).
function renderControls(focus) {
  if (!meta) return;
  const target = focus || listFocus();
  const list = $('ctl-list');
  list.replaceChildren();
  controls.forEach((control, index) => list.append(controlRow(control, index)));
  $('ctl-count').textContent = String(controls.length);
  $('ctl-max').textContent = String(meta.max_controls);
  $('ctl-empty').hidden = controls.length > 0;
  $('ctl-dirty').hidden = !controlsDirty;
  if (!target) return;
  if (target.element) {
    target.element.focus();
    return;
  }
  const item = list.children[target.index];
  const el = item && item.querySelector('[data-focus="' + target.field + '"]');
  if (!el || el.disabled) return;
  el.focus();
  if (target.start !== undefined && target.start !== null) {
    el.setSelectionRange(target.start, target.end);
  }
}

function labelError(bytes) {
  return 'The label is ' + bytes + ' bytes long; at most ' + meta.max_label_bytes +
    ' bytes are allowed (letters such as \u00e6, \u00f8 and \u00e5 count as 2 bytes).';
}

// The device's checks that the page can make itself; the device checks everything again.
function checkControls() {
  let ok = true;
  for (const control of controls) {
    let error = '';
    let field = '';
    const bytes = labelBytes(control.label);
    const range = stepRange(control.entity_id, control.action);
    if (bytes > meta.max_label_bytes) {
      error = labelError(bytes);
      field = 'label';
    } else if (range && !stepInRange(range, control.step)) {
      error = 'The step must be a number from ' + range.min + ' to ' + range.max + '.';
      field = 'step';
    }
    control.error = error;
    control.errorField = field;
    if (error) ok = false;
  }
  return ok;
}

async function saveControls() {
  // Never save a list that was not loaded from the config: it would delete the stored one.
  if (!meta || !controlsLoaded || controlsSaving) return;
  if (controls.length > meta.max_controls) {
    showControlsStatus('At most ' + meta.max_controls + ' controls: remove ' +
      (controls.length - meta.max_controls) + '.', 'error');
    return;
  }
  const valid = checkControls();
  renderControls();
  if (!valid) {
    showControlsStatus('Not saved: correct the marked controls.', 'error');
    return;
  }
  // The whole section: the device replaces the stored list with exactly this one.
  const sent = controls.slice();
  const patch = {
    controls: sent.map((c) => ({
      entity_id: c.entity_id,
      label: c.label,
      action: c.action,
      step: stepRange(c.entity_id, c.action) ? c.step : 0,
      press: c.press,
    })),
  };
  // The editor stays disabled until the answer is in, so rows cannot move meanwhile.
  controlsSaving = true;
  updateControlsBusy();
  let focusInvalid = false;  // rejected on a row: focus its invalid input afterwards
  try {
    const result = await api('POST', '/api/config', patch);
    const reloaded = await loadConfig().catch(() => false);
    if (result.ok) {
      if (!reloaded) config.controls = patch.controls;  // stored as sent
      setControlsFromConfig();  // the list as the device stored it
      renderControls();
      renderEntities();
      showControlsStatus('Controls saved.', 'success');
      return;
    }
    // A field such as "controls[3].step" belongs to row 3 of the list that was sent.
    const data = result.data || {};
    const match = /^controls\[(\d+)\]\.(\w+)$/.exec(data.field || '');
    const row = match ? sent[Number(match[1])] : undefined;
    if (result.status === 400 && row) {
      const field = match[2];
      row.error = (CONTROL_FIELD_TEXT[field] || '') + (ERROR_TEXT[data.error] || data.error || 'invalid');
      row.errorField = CONTROL_FIELD_TEXT[field] && field !== 'entity_id' ? field : '';
      renderControls();
      focusInvalid = true;
      showControlsStatus('Not saved: control ' + (Number(match[1]) + 1) + ' was rejected by the ' +
        'device.', 'error');
    } else {
      showControlsStatus('Not saved: ' + describeError(result), 'error');
    }
  } catch (e) {
    showControlsStatus('The device is not reachable.', 'error');
  } finally {
    controlsSaving = false;
    updateControlsBusy();
    // The disabled fieldset dropped the focus: put it back on Save or the rejected input.
    const invalid = focusInvalid ? [...$('ctl-list').querySelectorAll('[aria-invalid="true"]')]
      .find((el) => !el.closest('[hidden]')) : null;
    (invalid || $('ctl-save')).focus();
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
  $('ha-url').addEventListener('input', updateHaTlsForm);
  $('ha-tls-insecure').addEventListener('change', confirmInsecure);
  $('ha-forget').addEventListener('click', forgetHa);
  $('ha-retry').addEventListener('click', retryHa);
  $('ctl-save').addEventListener('click', saveControls);
  $('ent-refresh').addEventListener('click', () => loadEntities(true));
  $('ent-search').addEventListener('input', renderEntities);
  $('ent-domain').addEventListener('change', renderEntities);
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
