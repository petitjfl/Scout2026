// app.js - UI friendly messages, pending badge, compact logs, robust WS reconnect + keepalive
(() => {
  'use strict';

  const WS_PORT = 81;

  const accessoriesTable = document.getElementById('accessories');
  const accessoriesTbody = accessoriesTable ? accessoriesTable.querySelector('tbody') : null;
  const scanBtn = document.getElementById('btn-scan');
  const btnForceOffAll = document.getElementById('btn-force-off-all');
  const btnForceIdleAll = document.getElementById('btn-force-idle-all');
  const btnForceGlitchAll = document.getElementById('btn-force-glitch-all');
  const btnTimeSync = document.getElementById('btn-time-sync');
  const logBox = document.getElementById('log');

  function ensureElement(id, tag = 'div', parent = document.body) {
    let el = document.getElementById(id);
    if (!el) {
      el = document.createElement(tag);
      el.id = id;
      parent.appendChild(el);
    }
    return el;
  }
  const _logBox = logBox || ensureElement('log', 'div', document.body);
  const _accessoriesTbody = accessoriesTbody || (function() {
    let tbl = document.getElementById('accessories');
    if (!tbl) {
      tbl = document.createElement('table');
      tbl.id = 'accessories';
      const thead = document.createElement('thead');
      thead.innerHTML = `<tr>
        <th>ID</th><th>Nom</th><th>MAC</th><th>Prés.</th><th>Mode</th><th>Batt.</th><th>Actions</th>
      </tr>`;
      tbl.appendChild(thead);
      const wrap = document.createElement('div');
      wrap.className = 'table-wrap';
      wrap.appendChild(tbl);
      document.body.appendChild(wrap);
    }
    let tb = tbl.querySelector('tbody');
    if (!tb) {
      tb = document.createElement('tbody');
      tbl.appendChild(tb);
    }
    return tb;
  })();

  let ws = null;
  let wsKeepalive = null;
  let reconnectDelay = 1000;
  let reconnectTimer = null;
  let accessories = [];
  let reconnectAttempts = 0;

  const BATTERY_DELTA_MV = 50;
  const LOG_MIN_INTERVAL_MS = 30 * 1000;

  const CMD_LABELS = {
    'FORCE_OFF': 'Arrêt',
    'FORCE_IDLE': 'Mode Idle',
    'FORCE_GLITCH': 'Mode Glitch',
    'GLITCH_LOCK': 'Verrouillage Glitch',
    'TIME': 'Synchronisation horaire',
    'FINAL': 'Final',
    // Lanternes
    'FORCE_CANDLE': 'Bougie',
    'FORCE_ALERT': 'Alerte',
    'FORCE_WHITE': 'Blanc chaud',
    'SETMODE': 'Changer mode'
  };
  function cmdLabel(cmd) { return CMD_LABELS[cmd] || cmd; }

  // --- Accessory type detection (by ID range) ---
  // Balises 1..9, lanternes 10..19, médaillons 20..29. Keep this aligned with
  // the DEVICE_ID set in each firmware.
  function kindForId(id) {
    id = Number(id);
    if (id >= 20) return 'medaillon';
    if (id >= 10) return 'lanterne';
    return 'balise';
  }
  function isLantern(id) { return kindForId(id) === 'lanterne'; } // kept for compat
  function idsForKind(kind) { return accessories.filter(a => kindForId(a.id) === kind).map(a => a.id); }

  const KIND_LABEL = { balise: 'balise', lanterne: 'lanterne', medaillon: 'médaillon' };
  const KIND_LABEL_PLURAL = { balise: 'Balises', lanterne: 'Lanternes', medaillon: 'Médaillons' };

  // Numeric mode -> human label, per accessory type.
  const BALISE_MODES   = { 0: 'Off', 1: 'Idle', 2: 'Glitch' };
  const LANTERN_MODES  = { 0: 'Éteint', 1: 'Bougie', 2: 'Alerte', 3: 'Blanc' };
  const MEDAILLON_MODES = { 0: 'Repos', 1: 'Effet' };
  function modeLabel(id, mode) {
    if (mode === '' || mode === null || typeof mode === 'undefined') return '';
    const k = kindForId(id);
    const map = (k === 'lanterne') ? LANTERN_MODES : (k === 'medaillon') ? MEDAILLON_MODES : BALISE_MODES;
    const m = Number(mode);
    return (map[m] !== undefined) ? map[m] : String(mode);
  }

  function markPending(ids, cmd) {
    ids.forEach(id => {
      const idx = accessories.findIndex(a => a.id === id);
      if (idx !== -1) accessories[idx].pending = { cmd: cmd, nextTryAt: Date.now(), attempts: 0 };
    });
    renderAccessories();
  }

  // Send a command to every device of a given kind (used by prod view, the
  // discreet panel and the dev lantern bar).
  function sendToKind(kind, cmd) {
    const ids = idsForKind(kind);
    if (!ids.length) {
      const m = `Aucun ${KIND_LABEL[kind]} détecté`;
      log(m); prodStatus(m);
      return false;
    }
    const ok = sendAction({ action: 'send', targets: ids, cmd: cmd, arg: '' });
    if (ok) {
      const m = `${KIND_LABEL_PLURAL[kind]} → ${cmdLabel(cmd)} (${ids.length})`;
      log(m); prodStatus(m);
      markPending(ids, cmd);
    }
    return ok;
  }

  // FINALE: glitch all balises + alert the lantern(s) (FORCE_GLITCH is aliased
  // to ALERT in the lantern firmware) + fire the médaillon effect — all at once.
  function triggerFinale() {
    const lit = accessories
      .filter(a => { const k = kindForId(a.id); return k === 'balise' || k === 'lanterne'; })
      .map(a => a.id);
    const med = idsForKind('medaillon');
    let any = false;
    if (lit.length) { sendAction({ action: 'send', targets: lit, cmd: 'FORCE_GLITCH', arg: '' }); markPending(lit, 'FORCE_GLITCH'); any = true; }
    if (med.length) { sendAction({ action: 'send', targets: med, cmd: 'TRIGGER', arg: '' }); markPending(med, 'TRIGGER'); any = true; }
    const m = any
      ? `FINALE déclenchée — ${lit.length} lumière(s) + ${med.length} médaillon(s)`
      : 'Aucun appareil pour la finale';
    log(m); prodStatus(m);
  }

  // Transient status line (shown in prod view and the discreet panel).
  function prodStatus(msg) {
    const t = new Date().toLocaleTimeString();
    ['prod-status', 'stealth-status'].forEach(id => {
      const el = document.getElementById(id);
      if (el) el.textContent = `[${t}] ${msg}`;
    });
  }

  function updateCounts() {
    ['balise', 'lanterne', 'medaillon'].forEach(k => {
      const el = document.getElementById('count-' + k);
      if (!el) return;
      const list = accessories.filter(a => kindForId(a.id) === k);
      const online = list.filter(a => a.present).length;
      el.textContent = list.length ? `${online}/${list.length} en ligne` : 'aucun';
      el.classList.toggle('count-none', list.length === 0);
    });
  }

  function log(msg) {
    try {
      if (typeof msg !== 'string') msg = String(msg);
      const lines = msg.replace(/\r/g, '').split('\n').map(l => l.trim()).filter(l => l.length > 0);
      if (!lines.length) return;
      const t = new Date().toLocaleTimeString();
      lines.forEach(lineText => {
        const line = document.createElement('div');
        line.className = 'log-line';
        line.textContent = `[${t}] ${lineText}`;
        _logBox.prepend(line);
      });
    } catch (e) {
      console.log('LOG error:', e, msg);
    }
  }

  function safeParse(s) {
    try { return JSON.parse(s); } catch (e) { return null; }
  }

  function connectWs() {
    try {
      if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) {
        try { ws.close(); } catch (e) {}
        ws = null;
      }

      const proto = (location.protocol === 'https:') ? 'wss' : 'ws';
      const url = `${proto}://${location.hostname}:${WS_PORT}`;
      ws = new WebSocket(url);

      ws.addEventListener('open', () => {
        log('WebSocket connecté');
        reconnectDelay = 1000;
        reconnectAttempts = 0;
        if (reconnectTimer) { clearTimeout(reconnectTimer); reconnectTimer = null; }
        sendAction({ action: 'scan' });
        if (wsKeepalive) { clearInterval(wsKeepalive); wsKeepalive = null; }
        wsKeepalive = setInterval(() => {
          try {
            if (ws && ws.readyState === WebSocket.OPEN) {
              ws.send(JSON.stringify({ type: 'ping' }));
            }
          } catch (e) {}
        }, 20000);
      });

      ws.addEventListener('message', (ev) => {
        const data = safeParse(ev.data);
        if (!data) {
          log('Message WS non JSON: ' + ev.data);
          return;
        }
        if (data.type === 'pong') return;
        handleMessage(data);
      });

      ws.addEventListener('close', (ev) => {
        log(`WebSocket déconnecté (code=${ev.code} reason=${ev.reason || 'n/a'})`);
        if (wsKeepalive) { clearInterval(wsKeepalive); wsKeepalive = null; }
        ws = null;
        scheduleReconnect();
      });

      ws.addEventListener('error', (err) => {
        console.error('WS error', err);
      });
    } catch (e) {
      log('Erreur création WebSocket: ' + e.message);
      scheduleReconnect();
    }
  }

  function scheduleReconnect() {
    if (reconnectTimer) return;
    reconnectAttempts++;
    reconnectTimer = setTimeout(() => {
      reconnectTimer = null;
      reconnectDelay = Math.min(Math.floor(reconnectDelay * 1.5), 30000);
      log(`Tentative reconnexion WS (essai ${reconnectAttempts})...`);
      connectWs();
    }, reconnectDelay);
  }

  function sendAction(obj) {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      log('WS non connecté — action ignorée');
      return false;
    }
    try {
      ws.send(JSON.stringify(obj));
      return true;
    } catch (e) {
      log('Erreur envoi WS: ' + e.message);
      return false;
    }
  }

  function normalizeMacDisplay(mac) {
    if (!mac) return '—';
    return String(mac).replace(/\s*/g, '').toUpperCase();
  }

  function formatNextTry(nextTryMs) {
    if (!nextTryMs || typeof nextTryMs !== 'number') return '';
    const delta = Math.round((nextTryMs - Date.now()) / 1000);
    if (delta <= 0) return 'maintenant';
    if (delta < 60) return `dans ${delta}s`;
    const m = Math.floor(delta / 60);
    return `dans ${m}m`;
  }

  function nameForId(id) {
    const a = accessories.find(x => x.id === id);
    return a ? (a.name || `ID_${id}`) : `ID_${id}`;
  }

  // --- Handlers that were missing (fix onRowCmd undefined) ---
  function onRowCmd(e) {
    const btn = e.currentTarget;
    const id = parseInt(btn.dataset.id, 10);
    const cmd = btn.dataset.cmd;
    const payload = { action: 'send', targets: [id], cmd: cmd, arg: '' };
    const ok = sendAction(payload);
    if (ok) {
      log(`Commande envoyée vers ${nameForId(id)} : ${cmdLabel(cmd)} (mise en file)`);
      const idx = accessories.findIndex(a => a.id === id);
      if (idx !== -1) {
        accessories[idx].pending = { cmd: cmd, nextTryAt: Date.now(), attempts: 0 };
        renderAccessories();
      }
    } else {
      log(`Échec envoi vers ${nameForId(id)} : ${cmdLabel(cmd)}`);
    }
  }

  function onRowGlitchLock(e) {
    const btn = e.currentTarget;
    const id = parseInt(btn.dataset.id, 10);
    const locked = btn.dataset.locked === 'true';
    const newState = !locked;
    btn.dataset.locked = newState ? 'true' : 'false';
    btn.textContent = newState ? 'GlitchLock On' : 'GlitchLock Off';
    const idx = accessories.findIndex(a => a.id === id);
    if (idx !== -1) accessories[idx].glitch_locked = newState;
    const arg = newState ? 'ON' : 'OFF';
    const payload = { action: 'send', targets: [id], cmd: 'GLITCH_LOCK', arg: arg };
    const ok = sendAction(payload);
    if (ok) {
      log(`Commande envoyée vers ${nameForId(id)} : ${cmdLabel('GLITCH_LOCK')} (${newState ? 'ON' : 'OFF'})`);
      if (idx !== -1) {
        accessories[idx].pending = { cmd: 'GLITCH_LOCK', nextTryAt: Date.now(), attempts: 0 };
        renderAccessories();
      }
    } else {
      log(`Échec envoi GLITCH_LOCK vers ${nameForId(id)}`);
      btn.dataset.locked = locked ? 'true' : 'false';
      btn.textContent = locked ? 'GlitchLock On' : 'GlitchLock Off';
      if (idx !== -1) accessories[idx].glitch_locked = locked;
    }
  }

  function onRowInfo(e) {
    const id = parseInt(e.currentTarget.dataset.id, 10);
    const a = accessories.find(x => x.id === id);
    if (!a) { log(`Aucun accessoire id=${id}`); return; }
    log(`Info ${a.name} — MAC=${normalizeMacDisplay(a.mac)} — présent=${a.present} — mode=${a.mode} — batt=${a.batteryMv}`);
  }
  // --- end missing handlers ---

  function renderAccessories() {
    try {
      _accessoriesTbody.innerHTML = '';
      accessories.forEach(item => {
        const tr = document.createElement('tr');
        if (!item.present) tr.classList.add('muted');
        const mac = normalizeMacDisplay(item.mac);
        const locked = !!item.glitch_locked;
        const lockLabel = locked ? 'GlitchLock On' : 'GlitchLock Off';

        let pendingHtml = '';
        if (item.pending && item.pending.cmd) {
          const attempts = item.pending.attempts || 0;
          const nextTryText = item.pending.nextTryAt ? formatNextTry(item.pending.nextTryAt) : '';
          const smallClass = attempts > 0 ? ' small' : '';
          pendingHtml = `<span class="pending-badge${smallClass}">${escapeHtml(cmdLabel(item.pending.cmd))}${attempts>0 ? ` • ${attempts}` : ''}${nextTryText ? ` • ${escapeHtml(nextTryText)}` : ''}</span>`;
        }

        let presenceBadge = '';
        if (!item.present) presenceBadge = `<span class="badge badge-offline">Hors ligne</span>`;
        else presenceBadge = `<span class="badge badge-online">En ligne</span>`;

        // Action buttons depend on the accessory type.
        const kind = kindForId(item.id);
        let actionsHtml;
        if (kind === 'lanterne') {
          actionsHtml = `
            <button class="row-cmd lantern candle" data-id="${item.id}" data-cmd="FORCE_CANDLE">Bougie</button>
            <button class="row-cmd lantern alert" data-id="${item.id}" data-cmd="FORCE_ALERT">Alerte</button>
            <button class="row-cmd lantern white" data-id="${item.id}" data-cmd="FORCE_WHITE">Blanc</button>
            <button class="row-cmd lantern off" data-id="${item.id}" data-cmd="FORCE_OFF">Éteint</button>
            <button class="row-info" data-id="${item.id}">Info</button>`;
        } else if (kind === 'medaillon') {
          actionsHtml = `
            <button class="row-cmd candle" data-id="${item.id}" data-cmd="TRIGGER">Déclencher</button>
            <button class="row-cmd off" data-id="${item.id}" data-cmd="STOP">Arrêter</button>
            <button class="row-info" data-id="${item.id}">Info</button>`;
        } else {
          actionsHtml = `
            <button class="row-cmd" data-id="${item.id}" data-cmd="FORCE_OFF">Off</button>
            <button class="row-cmd" data-id="${item.id}" data-cmd="FORCE_IDLE">Idle</button>
            <button class="row-cmd" data-id="${item.id}" data-cmd="FORCE_GLITCH">Glitch</button>
            <button class="row-glitch-lock" data-id="${item.id}" data-locked="${locked}">${lockLabel}</button>
            <button class="row-info" data-id="${item.id}">Info</button>`;
        }

        tr.innerHTML = `
          <td>${item.id}</td>
          <td>${escapeHtml(item.name || `ID_${item.id}`)} ${pendingHtml}</td>
          <td class="mono">${escapeHtml(mac)}</td>
          <td>${presenceBadge}</td>
          <td>${escapeHtml(String(modeLabel(item.id, item.mode)))}</td>
          <td>${item.batteryMv ?? ''}</td>
          <td class="row-actions">${actionsHtml}</td>
        `;
        _accessoriesTbody.appendChild(tr);
      });

      Array.from(_accessoriesTbody.querySelectorAll('.row-cmd')).forEach(b => {
        b.removeEventListener('click', onRowCmd);
        b.addEventListener('click', onRowCmd);
      });
      Array.from(_accessoriesTbody.querySelectorAll('.row-glitch-lock')).forEach(b => {
        b.removeEventListener('click', onRowGlitchLock);
        b.addEventListener('click', onRowGlitchLock);
      });
      Array.from(_accessoriesTbody.querySelectorAll('.row-info')).forEach(b => {
        b.removeEventListener('click', onRowInfo);
        b.addEventListener('click', onRowInfo);
      });

      updateCounts();
    } catch (e) {
      console.error('renderAccessories error', e);
    }
  }

  function upsertAccessory(obj) {
    const id = obj.id;
    if (typeof id === 'undefined' || id === null) return;
    let idx = accessories.findIndex(a => a.id === id);
    if (idx === -1) {
      accessories.push({
        id: id,
        name: obj.name || `ID_${id}`,
        mac: obj.mac || '',
        present: !!obj.present,
        mode: obj.mode || '',
        batteryMv: obj.batteryMv || obj.batt || '',
        lastHbMs: obj.lastHbMs || 0,
        glitch_locked: !!obj.glitch_locked,
        pending: undefined,
        lastLoggedMode: obj.mode || '',
        lastLoggedBatt: obj.batteryMv || obj.batt || 0,
        lastLoggedPresent: !!obj.present,
        lastLoggedMs: 0
      });
    } else {
      const a = accessories[idx];
      if (obj.name) a.name = obj.name;
      if (obj.mac) a.mac = obj.mac;
      if (typeof obj.present !== 'undefined') a.present = !!obj.present;
      if (typeof obj.mode !== 'undefined') a.mode = obj.mode;
      if (typeof obj.batteryMv !== 'undefined') a.batteryMv = obj.batteryMv;
      if (typeof obj.batt !== 'undefined') a.batteryMv = obj.batt;
      if (typeof obj.lastHbMs !== 'undefined') a.lastHbMs = obj.lastHbMs;
      if (typeof obj.glitch_locked !== 'undefined') a.glitch_locked = !!obj.glitch_locked;
      if (typeof obj.pending !== 'undefined') a.pending = obj.pending;
    }
    renderAccessories();
  }

  function processHeartbeatItem(it) {
    const id = it.id;
    const name = it.name || nameForId(id);
    let idx = accessories.findIndex(a => a.id === id);

    if (idx === -1) {
      accessories.push({
        id: id,
        name: it.name || `ID_${id}`,
        mac: it.mac || '',
        present: typeof it.present !== 'undefined' ? !!it.present : true,
        mode: typeof it.mode !== 'undefined' ? it.mode : '',
        batteryMv: typeof it.batt !== 'undefined' ? it.batt : '',
        lastHbMs: it.lastHbMs || Date.now(),
        glitch_locked: typeof it.glitch_locked !== 'undefined' ? !!it.glitch_locked : false,
        pending: undefined,
        lastLoggedMode: typeof it.mode !== 'undefined' ? it.mode : '',
        lastLoggedBatt: typeof it.batt !== 'undefined' ? it.batt : 0,
        lastLoggedPresent: typeof it.present !== 'undefined' ? !!it.present : true,
        lastLoggedMs: 0
      });
      renderAccessories();
      log(`${name} — présence confirmée — mode ${it.mode} — batt ${it.batt}`);
      return;
    }

    const a = accessories[idx];
    const prevLoggedMode = a.lastLoggedMode;
    const prevLoggedBatt = a.lastLoggedBatt || 0;
    const prevLoggedPresent = a.lastLoggedPresent;
    const prevLoggedMs = a.lastLoggedMs || 0;

    if (it.mac) a.mac = it.mac;
    a.present = typeof it.present !== 'undefined' ? !!it.present : a.present;
    a.mode = typeof it.mode !== 'undefined' ? it.mode : a.mode;
    a.batteryMv = typeof it.batt !== 'undefined' ? it.batt : a.batteryMv;
    a.lastHbMs = it.lastHbMs || Date.now();
    if (typeof it.glitch_locked !== 'undefined') a.glitch_locked = !!it.glitch_locked;

    const now = Date.now();
    let shouldLog = false;

    if (a.present !== prevLoggedPresent) shouldLog = true;
    else if (a.mode !== prevLoggedMode) shouldLog = true;
    else if (typeof a.batteryMv === 'number' && Math.abs(a.batteryMv - prevLoggedBatt) >= BATTERY_DELTA_MV) shouldLog = true;
    else if (now - prevLoggedMs >= LOG_MIN_INTERVAL_MS) shouldLog = true;

    if (shouldLog) {
      log(`${name} — présence ${a.present ? 'confirmée' : 'absente'} — mode ${a.mode} — batt ${a.batteryMv}`);
      a.lastLoggedMode = a.mode;
      a.lastLoggedBatt = a.batteryMv;
      a.lastLoggedPresent = a.present;
      a.lastLoggedMs = now;
    }
  }

  function handleMessage(msg) {
    try {
      switch (msg.type) {
        case 'scan_result':
          accessories = [];
          (msg.items || []).forEach(it => {
            accessories.push({
              id: it.id,
              name: it.name || `ID_${it.id}`,
              mac: it.mac || '',
              present: it.present || false,
              mode: it.mode || '',
              batteryMv: it.batteryMv || it.batt || '',
              lastHbMs: it.lastHbMs || 0,
              glitch_locked: !!it.glitch_locked,
              pending: undefined,
              lastLoggedMode: it.mode || '',
              lastLoggedBatt: it.batteryMv || it.batt || 0,
              lastLoggedPresent: it.present || false,
              lastLoggedMs: 0
            });
          });
          renderAccessories();
          log(`Scan reçu (${accessories.length} accessoires)`);
          break;

        case 'hb':
          processHeartbeatItem({
            id: msg.id,
            mode: msg.mode,
            batt: msg.batt,
            present: typeof msg.present !== 'undefined' ? msg.present : true,
            mac: msg.mac,
            lastHbMs: msg.lastHbMs
          });
          renderAccessories();
          break;

        case 'hb_batch':
          (msg.items || []).forEach(it => {
            processHeartbeatItem({
              id: it.id,
              mode: it.mode,
              batt: it.batt,
              present: typeof it.present !== 'undefined' ? it.present : true,
              mac: it.mac,
              lastHbMs: it.lastHbMs
            });
          });
          renderAccessories();
          break;

        case 'status':
          upsertAccessory({
            id: msg.id,
            present: typeof msg.present !== 'undefined' ? msg.present : undefined,
            mode: typeof msg.mode !== 'undefined' ? msg.mode : undefined,
            glitch_locked: typeof msg.glitch_locked !== 'undefined' ? msg.glitch_locked : undefined
          });
          if (typeof msg.present !== 'undefined') log(`${nameForId(msg.id)} — présent=${msg.present}`);
          break;

        case 'pending_update':
          {
            const id = msg.id;
            const name = nameForId(id);
            const attempts = msg.attempts ?? 0;
            const next = (typeof msg.nextTryAt === 'number') ? Math.max(0, Math.floor((msg.nextTryAt - Date.now())/1000)) : null;
            const label = cmdLabel(msg.cmd);
            if (next !== null) {
              log(`Commande en attente pour ${name} : ${label} — prochaine tentative ${formatNextTry(msg.nextTryAt)}`);
            } else {
              log(`Commande en attente pour ${name} : ${label} (tentative ${attempts})`);
            }
            const idx = accessories.findIndex(a => a.id === id);
            if (idx !== -1) {
              accessories[idx].pending = {
                cmd: msg.cmd,
                nextTryAt: msg.nextTryAt || Date.now(),
                attempts: attempts
              };
              renderAccessories();
            }
          }
          break;

        case 'send_result':
          {
            const name = nameForId(msg.id);
            const label = cmdLabel(msg.cmd);
            if (msg.ok) {
              if (msg.queued) log(`Commande mise en file pour ${name} : ${label}`);
              else log(`Envoi réussi vers ${name} : ${label}`);
              const idx = accessories.findIndex(a => a.id === msg.id);
              if (idx !== -1) { accessories[idx].pending = undefined; renderAccessories(); }
            } else {
              log(`Échec envoi vers ${name} : ${label}` + (msg.reason ? ` — raison: ${msg.reason}` : ''));
              const idx = accessories.findIndex(a => a.id === msg.id);
              if (idx !== -1) accessories[idx].pending = accessories[idx].pending || { cmd: msg.cmd, attempts: 0 };
              renderAccessories();
            }
          }
          break;

        case 'ack':
          {
            const name = nameForId(msg.id);
            const label = cmdLabel(msg.cmd);
            log(`${name} a confirmé : ${label}`);
            const idx = accessories.findIndex(a => a.id === msg.id);
            if (idx !== -1) { accessories[idx].pending = undefined; renderAccessories(); }
          }
          break;

        case 'log':
          log(msg.msg || JSON.stringify(msg));
          break;

        default:
          console.debug('WS message ignored type=', msg.type, msg);
      }
    } catch (e) {
      console.error('handleMessage error', e);
    }
  }

  if (scanBtn) {
    scanBtn.addEventListener('click', () => {
      sendAction({ action: 'scan' });
      log('Scan demandé');
    });
  }

  if (btnForceOffAll) {
    btnForceOffAll.addEventListener('click', () => {
      sendAction({ action: 'send_all', cmd: 'FORCE_OFF', arg: '' });
      log('Envoi FORCE_OFF à tous');
    });
  }
  if (btnForceIdleAll) {
    btnForceIdleAll.addEventListener('click', () => {
      sendAction({ action: 'send_all', cmd: 'FORCE_IDLE', arg: '' });
      log('Envoi FORCE_IDLE à tous');
    });
  }
  if (btnForceGlitchAll) {
    btnForceGlitchAll.addEventListener('click', () => {
      sendAction({ action: 'send_all', cmd: 'FORCE_GLITCH', arg: '' });
      log('Envoi FORCE_GLITCH à tous');
    });
  }

  if (btnTimeSync) {
    btnTimeSync.addEventListener('click', () => {
      const epoch = Math.floor(Date.now() / 1000);
      sendAction({ action: 'send_all', cmd: 'TIME', arg: String(epoch) });
      log(`Envoi TIME (epoch=${epoch}) à tous`);
    });
  }

  // --- Generic kind-action buttons (prod view, dev lantern bar, stealth panel) ---
  // Every button carrying data-kind + data-cmd sends that command to all devices
  // of that kind, with a brief tactile confirmation.
  Array.from(document.querySelectorAll('button[data-kind][data-cmd]')).forEach(b => {
    b.addEventListener('click', () => {
      const ok = sendToKind(b.dataset.kind, b.dataset.cmd);
      b.classList.add('hit');
      setTimeout(() => b.classList.remove('hit'), 220);
    });
  });

  // --- FINALE buttons (prod view + stealth) ---
  Array.from(document.querySelectorAll('#btn-finale, [data-finale]')).forEach(b => {
    b.addEventListener('click', () => {
      triggerFinale();
      b.classList.add('hit');
      setTimeout(() => b.classList.remove('hit'), 220);
    });
  });

  // --- View toggle: Prod (default) vs Détaillé (dev) ---
  const prodView = document.getElementById('prod-view');
  const devView = document.getElementById('dev-view');
  const btnViewProd = document.getElementById('btn-view-prod');
  const btnViewDev = document.getElementById('btn-view-dev');
  function setView(v) {
    if (prodView) prodView.classList.toggle('hidden', v !== 'prod');
    if (devView) devView.classList.toggle('hidden', v !== 'dev');
    if (btnViewProd) { btnViewProd.classList.toggle('primary', v === 'prod'); btnViewProd.classList.toggle('ghost', v !== 'prod'); }
    if (btnViewDev) { btnViewDev.classList.toggle('primary', v === 'dev'); btnViewDev.classList.toggle('ghost', v !== 'dev'); }
  }
  if (btnViewProd) btnViewProd.addEventListener('click', () => setView('prod'));
  if (btnViewDev) btnViewDev.addEventListener('click', () => setView('dev'));
  setView('prod'); // default landing

  // --- Discreet (stealth) full-screen mode ---
  const stealthPanel = document.getElementById('stealth-panel');
  const btnDiscreet = document.getElementById('btn-discreet');
  const btnDiscreetExit = document.getElementById('btn-discreet-exit');
  function setStealth(on) {
    if (!stealthPanel) return;
    stealthPanel.classList.toggle('hidden', !on);
    stealthPanel.setAttribute('aria-hidden', on ? 'false' : 'true');
    document.body.classList.toggle('stealth', on);
  }
  if (btnDiscreet) btnDiscreet.addEventListener('click', () => setStealth(true));
  if (btnDiscreetExit) btnDiscreetExit.addEventListener('click', () => setStealth(false));

  function escapeHtml(s) {
    if (typeof s !== 'string') return s;
    return s.replace(/[&<>"']/g, (m) => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[m]));
  }

  connectWs();

  window.app = window.app || {};
  window.app.sendAction = sendAction;
  window.app.renderAccessories = renderAccessories;
  window.app.accessories = accessories;
  window.app.log = log;

})();
