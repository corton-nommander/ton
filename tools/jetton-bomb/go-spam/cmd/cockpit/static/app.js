/* The display animates every second; data sources retain their own sampling cadence. */
const $ = (id) => document.getElementById(id);
let network = 'mainnet';
let selectedSpan = '1h';
let latest = null;
let history = null;
let reading = false;
let loadingHistory = false;
const TPS_GAUGE_MAX = 2000;
const TPS_RED_ZONE_START = 1600;
const GAUGE_START_ANGLE = -135;
const CHART_PAD = { left: 42, right: 10, top: 10, bottom: 26 };

function ageSeconds(time) {
    return time ? (Date.now() - Date.parse(time)) / 1000 : Infinity;
}

function since(ts) {
    const seconds = ageSeconds(ts);
    if (!Number.isFinite(seconds)) return 'no reading';
    if (seconds < 60) return `${Math.max(0, Math.round(seconds))}s ago`;
    if (seconds < 3600) return `${Math.round(seconds / 60)}m ago`;
    return `${Math.round(seconds / 3600)}h ago`;
}

function seconds(value) {
    if (!Number.isFinite(value)) return '—';
    if (Math.abs(value) >= 100) return `${Math.round(value)}s`;
    return `${Math.round(value * 10) / 10}s`;
}

function latencyText(value) {
    if (!Number.isFinite(value)) return '—';
    if (value >= 3600) return `${Math.round(value / 3600)}h`;
    if (value >= 60) return `${Math.round(value / 60)}m`;
    return seconds(value);
}

function short(value, digits = 1) {
    return Number.isFinite(value) ? Number(value).toFixed(digits) : '—';
}

function stateText(name) {
    return name === 'ok' ? 'LIVE' : name === 'bad' ? 'ALERT' : name === 'warn' ? 'CHECK' : 'NO DATA';
}

function addLamp(name, state, note) {
    const el = document.createElement('div');
    el.className = `lamp ${state}`;
    const light = document.createElement('span');
    light.className = 'lamp-light';
    const text = document.createElement('span');
    const label = document.createElement('b');
    label.textContent = name;
    const details = document.createElement('small');
    details.textContent = note;
    text.append(label, details);
    el.append(light, text);
    el.title = `${name}: ${stateText(state)} · ${note}`;
    return el;
}

function initializeGauges() {
    for (const id of ['gauge-300', 'gauge-60', 'gauge-900']) {
        const dial = $(id).querySelector('.dial');
        const marks = document.createDocumentFragment();
        for (let i = 0; i <= 100; i++) {
            const angle = GAUGE_START_ANGLE + i * 2.7;
            const radians = (angle * Math.PI) / 180;
            const point = (radius) => ({ x: 50 + radius * Math.sin(radians), y: 50 - radius * Math.cos(radians) });
            const major = i % 10 === 0;
            const medium = !major && i % 5 === 0;
            const red = i >= (TPS_RED_ZONE_START / TPS_GAUGE_MAX) * 100;
            const tick = document.createElement('span');
            const position = point(43.4);
            tick.className = `tick${major ? ' major' : medium ? ' medium' : ''}${red ? ' red' : ''}`;
            tick.style.left = `${position.x}%`;
            tick.style.top = `${position.y}%`;
            tick.style.setProperty('--angle', `${angle}deg`);
            marks.append(tick);
            if (major) {
                const number = document.createElement('span');
                const labelPosition = point(34.7);
                number.className = `number${red ? ' red' : ''}`;
                number.textContent = String((i / 100) * TPS_GAUGE_MAX);
                number.style.left = `${labelPosition.x}%`;
                number.style.top = `${labelPosition.y}%`;
                marks.append(number);
            }
        }
        dial.append(marks);
    }
}

function gauge(element, value, window, stale) {
    const reading = Number.isFinite(value) ? Math.max(0, value) : null;
    const angle = GAUGE_START_ANGLE + (Math.min(TPS_GAUGE_MAX, reading || 0) / TPS_GAUGE_MAX) * 270;
    element.querySelector('.needle-arm').style.transform = `rotate(${angle}deg)`;
    element.querySelector('.odometer output').textContent =
        reading == null ? '—' : String(Math.round(reading)).padStart(4, '0');
    element.classList.toggle('is-stale', stale);
    element.setAttribute(
        'aria-label',
        `${window} TPS: ${reading == null ? 'no reading' : Math.round(reading)} of ${TPS_GAUGE_MAX}${stale ? ', unverified or stale' : ''}`,
    );
    element.title = `${window} TPS: ${reading == null ? 'no reading' : Math.round(reading)}. Red zone: 1600–2000.${stale ? ' Meter is stale or unverified.' : ''}`;
}

function updateTape(name, value, max, display) {
    const level = Number.isFinite(value) && max > 0 ? `${Math.min(100, (Math.max(0, value) / max) * 100)}%` : '0%';
    $(`${name}-fill`).style.height = level;
    $(`${name}-pointer`).style.bottom = level;
    $(name === 'fuel' ? 'fuel-value' : name === 'shards' ? 'shards-number' : 'latency-value').textContent = display;
    const tape = $(`tape-${name === 'fuel' ? 'gas' : name}`);
    const label = name === 'fuel' ? 'GAS - GRAM' : name === 'latency' ? 'TPSMETER LATENCY' : 'SHARDS';
    tape.setAttribute('aria-label', `${label}: ${display}${max > 0 ? ` of ${max}` : ''}`);
    tape.title = `${label}: ${display}. Scale: 0–${max > 0 ? max : 'unknown'}.`;
}

function blockAge(block) {
    return block?.gen_utime ? Date.now() / 1000 - block.gen_utime : Infinity;
}

function renderMeter(s) {
    const v = s.meter.data || {};
    const live =
        ageSeconds(s.meter.updated_at) <= 7 &&
        v.coverage_end &&
        ageSeconds(v.coverage_end) <= 12 &&
        v.forward_synced &&
        v.source_verified;
    const recentMismatch = v.recent_mismatch;
    const valid = live && !recentMismatch;
    const state = !s.meter.updated_at ? 'unknown' : !live ? 'bad' : valid ? 'ok' : 'warn';
    $('meter-state').textContent =
        state === 'bad' ? 'METER STALE' : state === 'warn' ? 'RECENT BLOCK MISMATCH' : stateText(state);
    $('meter-state').className = `state ${state}`;
    const values = new Map((v.windows || []).map((w) => [w.window_size, w.tps]));
    gauge($('gauge-60'), values.get(60), '1 MIN', !valid);
    gauge($('gauge-300'), values.get(300), '5 MIN', !valid);
    gauge($('gauge-900'), values.get(900), '15 MIN', !valid);
    const latency = ageSeconds(v.coverage_end);
    updateTape('latency', latency, 15, latencyText(latency));
    $('tape-latency').title = v.coverage_end
        ? `Time since the last block processed by tpsmeter: ${latencyText(latency)}. Scale: 0–15s.`
        : 'Waiting for the first processed block from tpsmeter.';
    $('meter-detail').textContent = v.coverage_end
        ? `${recentMismatch ? 'Meter saw a different chain tip or block than its checkpoint in the last 60s · ' : ''}Latest processed block ${since(v.coverage_end)} · ${v.forward_synced ? 'at head' : 'catching up'}`
        : s.meter.error || 'No tpsmeter checkpoint';
    $('meter-detail').title =
        v.last_error || s.meter.error || 'Last block processed by tpsmeter; TPS comes only from this meter.';
    return { state, live, valid };
}

function renderChain(s, lsState) {
    const chain = s.chain.data;
    const freshRead = ageSeconds(s.chain.updated_at) < 10;
    const mcAge = blockAge(chain?.mc);
    const mcFresh = freshRead && mcAge < 15 && mcAge > -5;
    const mcState = !freshRead
        ? 'unknown'
        : mcFresh
          ? 'ok'
          : lsState === 'ok'
            ? 'warn'
            : lsState === 'bad'
              ? 'bad'
              : 'unknown';
    const shards = chain?.shards || [];
    updateTape('shards', chain?.shards ? shards.length : null, 4, chain?.shards ? String(shards.length) : '—');
    $('tape-shards').title = `Active basechain shards: ${chain?.shards ? shards.length : 'no reading'}. Scale: 0–4.`;
    $('blocks-source').textContent = chain?.source
        ? `${chain.source}${s.chain.error ? ' · last read failed' : ''}`
        : s.chain.error || '';
    $('blocks-source').title = s.chain.error || 'Live liteserver block headers; independent of tpsmeter TPS catch-up.';
    const rows = $('block-rows');
    rows.replaceChildren();
    const active = [
        { name: 'MASTERCHAIN', block: chain?.mc },
        ...shards.map((b) => ({ name: `SHARD ${b.shard?.slice(0, 8) || '—'}`, block: b })),
    ];
    let stalled = 0;
    for (const item of active) {
        const b = item.block;
        const lag = blockAge(b);
        const tipState =
            !b?.gen_utime || !Number.isFinite(lag) || lag < -5
                ? 'unknown'
                : lag > 15
                  ? 'bad'
                  : lag > 8 || !freshRead
                    ? 'warn'
                    : 'ok';
        if (tipState === 'bad' && freshRead && mcFresh && b?.workchain === 0) stalled++;
        const el = document.createElement('div');
        el.className = `block ${tipState}`;
        const light = document.createElement('span');
        light.className = `dot ${tipState}`;
        const title = document.createElement('span');
        const name = document.createElement('b');
        name.textContent = item.name;
        const detail = document.createElement('small');
        detail.textContent = b
            ? `#${b.seqno.toLocaleString()} · ${new Date(b.gen_utime * 1000).toLocaleTimeString('en-GB', { timeZone: 'UTC' })} UTC`
            : 'No block available';
        title.append(name, detail);
        const age = document.createElement('span');
        age.className = 'block-age';
        age.textContent = seconds(lag);
        el.append(light, title, age);
        el.title = `${item.name}: ${b ? `LS block #${b.seqno}, ${seconds(lag)} old` : 'no LS block'}${tipState === 'ok' ? '' : ` · ${stateText(tipState)} (LS data)`}`;
        rows.append(el);
    }
    return { mcState, stalled, freshRead, mcAge };
}

function renderFuel(s) {
    const fuel = s.fuel.data || {};
    const fresh = ageSeconds(s.fuel.updated_at) < 180;
    const pct = fresh ? fuel.remaining_pct : null;
    $('fuel-tick-max').textContent = fuel.initial > 0 ? short(fuel.initial, 2) : '—';
    $('fuel-tick-mid').textContent = fuel.initial > 0 ? short(fuel.initial / 2, 2) : '—';
    updateTape('fuel', fresh ? fuel.average : null, fuel.initial, fresh ? short(fuel.average, 3) : '—');
    $('tape-gas').classList.toggle('is-low', pct != null && pct < 25);
    const eta = fuel.scope?.includes('unknown')
        ? 'active-slice ETA unavailable'
        : fuel.minutes_left == null
          ? 'ETA pending'
          : `~${Math.round(fuel.minutes_left)} min left`;
    $('tape-gas').title = fresh
        ? `${fuel.sampled}/20 sampled · ${fuel.scope} · initial ${short(fuel.initial, 2)} TON · ${fuel.below_threshold} low · ${eta} · ${since(s.fuel.updated_at)}`
        : s.fuel.error || 'Waiting for balances';
    return {
        state: !fresh ? 'unknown' : fuel.missing ? 'warn' : pct < 20 ? 'bad' : 'ok',
        note: !fresh ? 'No recent sample' : `${fuel.sampled}/20 · ${short(fuel.average, 3)} TON avg`,
    };
}

function renderLoad(s) {
    const panel = $('load-status');
    const details = $('load-details');
    const fresh = s.bot_fresh === true && ageSeconds(s.bot?.updated_at) < 20;
    const report = s.bot?.data || {};
    const run = fresh ? report.pool : null;
    const staticRuns = fresh ? [report.static, report.shards].filter(Boolean) : [];
    const staticTPS = staticRuns.reduce((total, item) => total + item.tps_per_shard * item.shards, 0);
    const active = !!run || staticRuns.length > 0;
    panel.classList.toggle('is-stale', !fresh);
    panel.classList.toggle('is-idle', fresh && !active);
    $('load-state').textContent = !fresh ? 'NO REPORT' : active ? 'ACTIVE' : 'IDLE';
    details.replaceChildren();

    function row(label, value, title = '') {
        const item = document.createElement('div');
        item.className = 'load-row';
        const name = document.createElement('span');
        name.textContent = label;
        const reading = document.createElement('strong');
        reading.textContent = value;
        item.append(name, reading);
        item.title = title || `${label}: ${value}`;
        details.append(item);
    }

    if (!fresh) {
        row('Last seen', s.bot?.updated_at && ageSeconds(s.bot.updated_at) < Infinity ? since(s.bot.updated_at) : '—');
        return;
    }
    if (!active) {
        row('Current load', 'None');
        return;
    }

    const modeNames = {
        hamster: 'Jetton · full pool',
        jetton: 'Jetton transfer',
        fat_jetton: 'Fat jetton',
        gram: 'GRAM transfer',
        fat_gram: 'Fat GRAM',
        gassless_gram: 'Gasless GRAM',
        gassless_jetton: 'Gasless jetton',
        external: 'External only',
    };
    row('Type', run ? modeNames[run.mode] || run.mode.replaceAll('_', ' ') : 'Static TPS');
    row('Req/s aim', run ? Number(run.target_rps).toLocaleString('en-US') : '—');

    // Only modes with a stable transaction multiplier get a TPS estimate.
    // Gasless batching and GRAM bounces vary in their transactions per request.
    const factor = { hamster: 4, jetton: 4, fat_jetton: 4, external: 1 }[run?.mode];
    const targetTPS = run && factor ? run.target_rps * factor + staticTPS : run ? null : staticTPS;
    row(
        'TPS aim',
        targetTPS == null ? '—' : `${run ? '≈' : ''}${targetTPS.toLocaleString('en-US')}`,
        run && factor
            ? `Estimate: ${factor} transactions per successful ${run.mode} request${staticTPS ? `, plus ${staticTPS} static TPS` : ''}. Actual network TPS is shown on the gauges.`
            : run
              ? 'Transaction count per request varies in this mode; see the network TPS gauges.'
              : 'Configured static transactions per second.',
    );

    if (run) {
        const sent = Number(run.sent || 0);
        const failed = Number(run.failed || 0);
        const attempts = sent + failed;
        row('Sent', sent.toLocaleString('en-US'));
        row(
            'Send OK',
            attempts ? `${((sent / attempts) * 100).toFixed(1)}%` : '—',
            'Accepted logical submissions: sent / (sent + failed). On-chain delivery is sampled separately below.',
        );
        const elapsed = ageSeconds(run.started_at);
        row('Avg sent/s', elapsed > 0 && Number.isFinite(elapsed) ? (sent / elapsed).toFixed(0) : '—');
        if (failed) row('Failed', failed.toLocaleString('en-US'));
        if (run.skipped) row('Skipped', Number(run.skipped).toLocaleString('en-US'));
        if (run.mode === 'hamster') row('Pool range', Number(report.pool_total).toLocaleString('en-US'));
        else if (report.pool_size) row('Pool range', Number(report.pool_size).toLocaleString('en-US'));

        const delivery = run.delivery;
        if (!delivery) {
            row('Delivery', 'Not monitored');
        } else if (!delivery.enabled) {
            row('Delivery', 'Off');
        } else {
            row('Delivery', delivery.ready ? 'Sampling' : 'Connecting');
            row(
                'Conf./s',
                `${short(delivery.confirmed_per_second, 2)}/s`,
                `${Number(delivery.confirmed).toLocaleString('en-US')} total confirmed samples; at most one transfer sampled per second.`,
            );
            row(
                'Final./s',
                `${short(delivery.finalized_per_second, 2)}/s`,
                `${Number(delivery.finalized).toLocaleString('en-US')} total finalized samples; at most one transfer sampled per second.`,
            );
            row('Conf. p95', delivery.p95_seconds ? `${short(delivery.p95_seconds, 2)}s` : '—');
            row('Final. p95', delivery.finalized_p95_seconds ? `${short(delivery.finalized_p95_seconds, 2)}s` : '—');
            if (delivery.pending) row('Pending', Number(delivery.pending).toLocaleString('en-US'));
            if (delivery.not_found || delivery.finalized_not_found)
                row('Not found', Number(delivery.not_found || 0) + Number(delivery.finalized_not_found || 0));
        }
    }
    if (staticTPS)
        row('Static TPS', staticTPS.toLocaleString('en-US'), 'On-chain static load tracked by this process.');
}

function renderFleet(s) {
    const fleet = s.ls.data || {};
    const fresh = ageSeconds(s.ls.updated_at) < (s.network === 'testnet' ? 180 : 60);
    const nodes = fleet.nodes || [];
    $('ls-count').textContent = fresh ? `${fleet.healthy}/${fleet.total}` : 'NO DATA';
    const rows = $('ls-rows');
    rows.replaceChildren();
    for (const node of nodes) {
        const state = !fresh
            ? 'unknown'
            : node.state === 'ok'
              ? 'ok'
              : node.state === 'lagging'
                ? 'warn'
                : node.state === 'down'
                  ? 'bad'
                  : 'unknown';
        const row = document.createElement('div');
        row.className = 'row';
        const dot = document.createElement('span');
        dot.className = `dot ${state}`;
        const label = document.createElement('span');
        label.textContent = node.name;
        const age = document.createElement('strong');
        age.textContent =
            node.age_seconds == null
                ? node.state.toUpperCase()
                : `${seconds(node.age_seconds)}${node.shard_gap_blocks == null ? '' : ` · SC ${Math.round(node.shard_gap_blocks)}`}`;
        row.title = node.reported_at
            ? `${node.name}: ${stateText(state)} · last report ${seconds(node.age_seconds)} ago`
            : node.shard_gap_blocks == null
              ? `${node.name}: no recent report`
              : `${node.name}: ${Math.round(node.shard_gap_blocks)} masterchain blocks behind`;
        if (node.shard_gap_blocks != null)
            row.title += ` · SC = shard client lag: ${Math.round(node.shard_gap_blocks)} blocks (0 = caught up)`;
        row.append(dot, label, age);
        label.title = node.name;
        rows.append(row);
    }
    if (!nodes.length) rows.textContent = s.ls.error || 'Waiting for lite server measurements';
    const degraded = nodes.filter((n) => n.state === 'lagging' || n.state === 'down').length;
    return {
        state: !fresh ? 'unknown' : degraded ? 'bad' : nodes.some((n) => n.state === 'unknown') ? 'warn' : 'ok',
        note: fresh ? `${fleet.healthy}/${fleet.total} healthy` : 'Measurements unavailable',
    };
}

function healthState(status) {
    if (status === 'ok' || status === 'operational') return 'ok';
    if (status === 'degraded_performance' || status === 'warn' || status === 'maintenance') return 'warn';
    if (status === 'down' || status === 'major_outage' || status === 'partial_outage') return 'bad';
    return 'unknown';
}

function componentState(component, fresh) {
    if (!fresh) return 'unknown';
    const state = healthState(component.status);
    if (state === 'bad' || !component.name.includes('(freshness)') || !Number.isFinite(component.lag_seconds))
        return state;
    if (component.lag_seconds > 15) return 'bad';
    if (component.lag_seconds > 8) return 'warn';
    return state;
}

function renderHealth(s) {
    const fresh = ageSeconds(s.health.updated_at) < 120 && ageSeconds(s.health.data?.last_sync) < 300;
    const rows = $('service-rows');
    rows.replaceChildren();
    const parts = s.health.data?.components || [];
    for (const component of parts) {
        const state = componentState(component, fresh);
        const row = document.createElement('div');
        row.className = 'row';
        const dot = document.createElement('span');
        dot.className = `dot ${state}`;
        const name = document.createElement('span');
        name.textContent = component.name;
        const status = document.createElement('strong');
        status.textContent =
            component.lag_seconds == null
                ? component.status.toUpperCase().replaceAll('_', ' ')
                : seconds(component.lag_seconds);
        row.append(dot, name, status);
        name.title = component.name;
        row.title = `${component.name}: ${status.textContent}${component.name.includes('(freshness)') ? ' · green ≤8s, amber ≤15s, red >15s' : component.name === 'API v3' ? ' · availability only; freshness is listed below' : ''}`;
        rows.append(row);
    }
    if (!parts.length) rows.textContent = s.health.error || 'Waiting for service checks';
    const streaming = parts.filter(
        (p) => p.name.includes('Streaming') || p.name.startsWith('SSE ') || p.name.startsWith('WS '),
    );
    const streamState =
        !fresh || !streaming.length
            ? 'unknown'
            : streaming.some((p) => componentState(p, fresh) === 'bad')
              ? 'bad'
              : streaming.some((p) => componentState(p, fresh) === 'warn')
                ? 'warn'
                : 'ok';
    const indexer = parts.filter((p) => p.name.includes('(freshness)'));
    const indexerState =
        !fresh || !indexer.length
            ? 'unknown'
            : indexer.some((p) => componentState(p, fresh) === 'bad')
              ? 'bad'
              : indexer.some((p) => componentState(p, fresh) === 'warn')
                ? 'warn'
                : 'ok';
    return { streamState, indexerState, fresh };
}

function renderPressure(s) {
    const data = s.pressure.data || {};
    const sampled = !!data.observed_at;
    const stale = sampled && ageSeconds(data.observed_at) >= 180;
    const items = [
        ['Message queue / shard', data.queue_size, 0],
        ['Rejected externals / block', data.rejected, 1],
        ['Timeout overload fraction', data.timeout_load, 2],
        ['Preliminary storage work', data.collation_time, 3],
    ];
    $('pressure-rows').replaceChildren(
        ...items.map(([name, value, digits]) => {
            const el = document.createElement('div');
            el.className = 'pressure';
            const title = document.createElement('small');
            title.textContent = `${name}:`;
            const number = document.createElement('strong');
            number.textContent = sampled
                ? `${short(value, digits)}${stale ? ` · ${since(data.observed_at)}` : ''}`
                : '—';
            el.title = `${name}: ${sampled ? short(value, digits) : 'no sample'}${sampled ? ` · sampled ${since(data.observed_at)}` : ''}`;
            el.append(title, number);
            return el;
        }),
    );
}

function render(s) {
    latest = s;
    network = s.network;
    $('mainnet-link').classList.toggle('active', network === 'mainnet');
    $('testnet-link').classList.toggle('active', network === 'testnet');
    $('meter-source-note').classList.toggle('hidden', network !== 'testnet');
    const meter = renderMeter(s);
    const fuel = renderFuel(s);
    renderLoad(s);
    const fleet = renderFleet(s);
    const chain = renderChain(s, fleet.state);
    const services = renderHealth(s);
    renderPressure(s);
    const lamps = $('lamps');
    lamps.replaceChildren();
    const entries = [
        [
            'TPS METER',
            meter.state,
            s.meter.data?.coverage_end ? `Block ${since(s.meter.data.coverage_end)}` : 'No checkpoint',
        ],
        [
            'MASTERCHAIN',
            chain.mcState,
            chain.mcState === 'ok'
                ? `Latest ${seconds(chain.mcAge)} ago`
                : chain.mcState === 'warn'
                  ? 'LS head is older than 15s'
                  : 'Check LS block age',
        ],
        [
            'SHARD CHAINS',
            !chain.freshRead ? 'unknown' : chain.stalled ? 'bad' : 'ok',
            chain.stalled ? `${chain.stalled} stalled` : `${s.chain.data?.shards?.length || 0} active`,
        ],
        ['LITE SERVERS', fleet.state, fleet.note],
        ['STREAMING', services.streamState, services.fresh ? 'SSE & WebSocket checks' : 'Checks unavailable'],
        ['API INDEXER', services.indexerState, services.fresh ? 'V2 / V3 freshness' : 'Checks unavailable'],
        ['POOL FUEL', fuel.state, fuel.note],
        [
            'SESSION STATS',
            !s.pressure.data?.observed_at ? 'unknown' : ageSeconds(s.pressure.data.observed_at) < 180 ? 'ok' : 'warn',
            `Block sample ${since(s.pressure.data?.observed_at)}`,
        ],
    ];
    for (const [name, state, note] of entries) lamps.append(addLamp(name, state, note));
    const message = !meter.live
        ? `TPS meter is behind. Displayed TPS is from ${s.meter.data?.coverage_end || 'an unknown time'}. ${s.meter.data?.last_error || ''}`
        : chain.stalled
          ? `${chain.stalled} shard(s) have stopped advancing while the masterchain is live.`
          : chain.mcState === 'warn'
            ? 'The liteserver block head is old; check liteserver sync.'
            : '';
    $('warning').textContent = message;
    $('warning').title = message;
    $('warning').classList.toggle('hidden', !message);
}

function drawChart(canvas, points, field, color, integer) {
    const width = canvas.clientWidth || 400,
        height = canvas.clientHeight || 160;
    const dpr = window.devicePixelRatio || 1;
    canvas.width = Math.round(width * dpr);
    canvas.height = Math.round(height * dpr);
    const g = canvas.getContext('2d');
    g.scale(dpr, dpr);
    g.clearRect(0, 0, width, height);
    const pad = CHART_PAD;
    const w = width - pad.left - pad.right,
        h = height - pad.top - pad.bottom;
    if (!points.length) {
        g.fillStyle = '#929e91';
        g.font = '12px Inter, Arial, sans-serif';
        g.fillText('No history available', 14, 30);
        return;
    }
    const values = points.map((p) => p[field]).filter((v) => typeof v === 'number' && Number.isFinite(v));
    if (!values.length) return;
    const max = integer
        ? Math.max(4, Math.ceil(Math.max(...values) / 4) * 4)
        : Math.max(10, Math.ceil(Math.max(...values) * 1.12));
    const xy = (p, i) => [
        pad.left + (i / Math.max(points.length - 1, 1)) * w,
        pad.top + h - (Math.max(0, p[field]) / max) * h,
    ];
    g.strokeStyle = '#dde4d7';
    g.fillStyle = '#879487';
    g.lineWidth = 1;
    g.font = '10px Inter, Arial, sans-serif';
    for (let i = 0; i <= 4; i++) {
        const y = pad.top + (h * i) / 4;
        g.beginPath();
        g.moveTo(pad.left, y);
        g.lineTo(width - pad.right, y);
        g.stroke();
        g.fillText(String(Math.round(max * (1 - i / 4))), 2, y + 3);
    }
    for (let i = 0; i <= 4; i++) {
        const index = Math.round(((points.length - 1) * i) / 4);
        const x = pad.left + (w * i) / 4;
        g.textAlign = i === 0 ? 'left' : i === 4 ? 'right' : 'center';
        g.fillText(
            new Date(points[index].timestamp).toLocaleTimeString('en-GB', {
                timeZone: 'UTC',
                hour: '2-digit',
                minute: '2-digit',
            }),
            x,
            height - 5,
        );
    }
    g.textAlign = 'left';
    g.beginPath();
    let drawing = false;
    points.forEach((point, i) => {
        if (point[field] == null) {
            drawing = false;
            return;
        }
        const [x, y] = xy(point, i);
        if (!drawing) {
            g.moveTo(x, y);
            drawing = true;
        } else g.lineTo(x, y);
    });
    g.strokeStyle = color;
    g.lineWidth = 2;
    g.stroke();
}

function hideChartHover(field) {
    $(`${field}-tooltip`).classList.add('hidden');
    $(`${field}-crosshair`).classList.add('hidden');
}

function showChartHover(canvas, key, field, label, event) {
    const points = history?.points || [];
    if (!points.length) return;
    const width = canvas.clientWidth;
    const plotWidth = Math.max(1, width - CHART_PAD.left - CHART_PAD.right);
    const cursor = event.clientX - canvas.getBoundingClientRect().left;
    const index = Math.round(
        (Math.max(0, Math.min(plotWidth, cursor - CHART_PAD.left)) / plotWidth) * (points.length - 1),
    );
    const point = points[index];
    const x = CHART_PAD.left + (index / Math.max(1, points.length - 1)) * plotWidth;
    const value = Number.isFinite(point[field])
        ? field === 'shards'
            ? String(point[field])
            : short(point[field], 1)
        : 'no sample';
    const time = new Date(point.timestamp).toLocaleString('en-GB', { timeZone: 'UTC', hour12: false });
    const tooltip = $(`${key}-tooltip`);
    tooltip.textContent = `${label}: ${value} · ${time} UTC`;
    tooltip.classList.remove('hidden');
    const half = tooltip.offsetWidth / 2;
    tooltip.style.left = `${Math.max(half, Math.min(width - half, x))}px`;
    $(`${key}-crosshair`).style.left = `${x}px`;
    $(`${key}-crosshair`).classList.remove('hidden');
    canvas.removeAttribute('title');
}

function renderHistory() {
    hideChartHover('tps');
    hideChartHover('shards');
    const points = history?.points || [];
    drawChart($('tps-chart'), points, 'total', '#b55d3f', false);
    drawChart($('shards-chart'), points, 'shards', '#527b59', true);
    $('chart-detail').textContent = history?.end
        ? `Recorded through ${new Date(history.end).toLocaleString('en-GB', { timeZone: 'UTC' })} UTC · ${selectedSpan}${latest?.meter?.data?.recent_mismatch ? ' · block mismatch in last minute' : ''}`
        : 'Waiting for tpsmeter history';
}

async function refreshSnapshot() {
    if (reading) return;
    reading = true;
    try {
        const response = await fetch('/api/snapshot', { cache: 'no-store' });
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        render(await response.json());
    } catch (error) {
        $('warning').textContent = `Cockpit API unavailable: ${error.message}`;
        $('warning').classList.remove('hidden');
    } finally {
        reading = false;
    }
}

async function refreshHistory() {
    if (loadingHistory) return;
    loadingHistory = true;
    try {
        const response = await fetch(`/api/history?span=${selectedSpan}`, { cache: 'no-store' });
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        history = await response.json();
        renderHistory();
    } catch (error) {
        $('chart-detail').textContent = `History unavailable: ${error.message}`;
    } finally {
        loadingHistory = false;
    }
}

initializeGauges();
$('mainnet-link').href = `${location.protocol}//${location.hostname}:1902/`;
$('testnet-link').href = `${location.protocol}//${location.hostname}:1901/`;
for (const [id, key, field, label] of [
    ['tps-chart', 'tps', 'total', 'TPS'],
    ['shards-chart', 'shards', 'shards', 'Shards'],
]) {
    const canvas = $(id);
    canvas.addEventListener('pointermove', (event) => showChartHover(canvas, key, field, label, event));
    canvas.addEventListener('pointerleave', () => hideChartHover(key));
}
for (const button of document.querySelectorAll('[data-span]'))
    button.addEventListener('click', () => {
        selectedSpan = button.dataset.span;
        document.querySelectorAll('[data-span]').forEach((b) => b.classList.toggle('active', b === button));
        history = null;
        refreshHistory();
    });
window.addEventListener('resize', () => history && renderHistory());
setInterval(() => {
    $('clock').textContent = new Date().toLocaleTimeString('en-GB', { timeZone: 'UTC' });
    if (latest) render(latest);
}, 1000);
setInterval(refreshSnapshot, 1000);
setInterval(refreshHistory, 15000);
refreshSnapshot();
refreshHistory();
