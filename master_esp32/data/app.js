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
    'FINAL': 'Final'
  };
  function cmdLabel(cmd) { return CMD_LABELS[cmd] || cmd; }

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

        tr.innerHTML = `
          <td>${item.id}</td>
          <td>${escapeHtml(item.name || `ID_${item.id}`)} ${pendingHtml}</td>
          <td class="mono">${escapeHtml(mac)}</td>
          <td>${presenceBadge}</td>
          <td>${item.mode ?? ''}</td>
          <td>${item.batteryMv ?? ''}</td>
          <td class="row-actions">
            <button class="row-cmd" data-id="${item.id}" data-cmd="FORCE_OFF">Off</button>
            <button class="row-cmd" data-id="${item.id}" data-cmd="FORCE_IDLE">Idle</button>
            <button class="row-cmd" data-id="${item.id}" data-cmd="FORCE_GLITCH">Glitch</button>
            <button class="row-glitch-lock" data-id="${item.id}" data-locked="${locked}">${lockLabel}</button>
            <button class="row-info" data-id="${item.id}">Info</button>
          </td>
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
