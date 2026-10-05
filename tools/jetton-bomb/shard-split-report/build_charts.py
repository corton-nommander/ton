import json, datetime

VD = json.load(open('/tmp/final_verified.json'))
EPS = sorted(VD['episodes'], key=lambda e: e['utc'])

# 6-colour legend by episode type
COL = {'broad': '#37b3a5', 'lowtps': '#8698a0', 'our-jetton': '#5b9bd5',
       'compute': '#e56b42', 'our-poke': '#d472d4', 'our-destroy': '#9d80e6'}
def eck(et):
    if et.startswith('lowtps'): return 'lowtps'
    return et if et in COL else 'lowtps'

def ep_ts(e):
    return int(datetime.datetime.strptime(e['utc'], '%Y-%m-%d %H:%M:%S')
              .replace(tzinfo=datetime.timezone.utc).timestamp())

TS = [ep_ts(e) for e in EPS]
TMIN, TMAX = min(TS), max(TS)


# ---------- Chart GAS: max gas/block vs config-23 soft (10M) ----------
def chart_gas():
    W, Hh = 760, 430; ml, mr, mt, mb = 64, 150, 34, 54
    pw, ph = W - ml - mr, Hh - mt - mb
    ymax = 22_000_000
    def X(t): return ml + (t - TMIN) / (TMAX - TMIN) * pw
    def Y(v): return mt + ph - min(v, ymax) / ymax * ph
    s = [f'<svg viewBox="0 0 {W} {Hh}" class="chart" role="img" aria-label="max gas per block vs soft">']
    s.append(f'<rect x="{ml}" y="{mt}" width="{pw}" height="{ph}" fill="none" stroke="#333"/>')
    for gy in [0, 5_000_000, 10_000_000, 15_000_000, 20_000_000]:
        y = Y(gy); s.append(f'<line x1="{ml}" y1="{y:.1f}" x2="{ml+pw}" y2="{y:.1f}" stroke="#1e1e1e"/>')
        s.append(f'<text x="{ml-8}" y="{y+3:.1f}" class="cxs" text-anchor="end">{gy//1_000_000}M</text>')
    ys = Y(10_000_000); s.append(f'<line x1="{ml}" y1="{ys:.1f}" x2="{ml+pw}" y2="{ys:.1f}" stroke="#e3a83e" stroke-width="1.5" stroke-dasharray="6 3"/>')
    s.append(f'<text x="{ml+6}" y="{ys-5:.0f}" class="cann" fill="#e3a83e">gas soft = 10M (порог load-класса)</text>')
    yh = Y(20_000_000); s.append(f'<line x1="{ml}" y1="{yh:.1f}" x2="{ml+pw}" y2="{yh:.1f}" stroke="#e56b42" stroke-dasharray="3 4"/>')
    s.append(f'<text x="{ml+6}" y="{yh+13:.0f}" class="cxs" fill="#e56b42">hard = 20M</text>')
    for mo, lab in [(5, 'май'), (6, 'июн'), (7, 'июл')]:
        t = int(datetime.datetime(2026, mo, 1, tzinfo=datetime.timezone.utc).timestamp())
        if TMIN <= t <= TMAX:
            x = X(t); s.append(f'<line x1="{x:.1f}" y1="{mt}" x2="{x:.1f}" y2="{mt+ph}" stroke="#3a3a3a"/>')
            s.append(f'<text x="{x+4:.0f}" y="{mt+ph+15}" class="cxs">1 {lab}</text>')
    for e in EPS:
        v = int(e['mg']); k = eck(e['ep_type'])
        x = X(ep_ts(e)); y = Y(v); rad = 4.6 if k == 'compute' else 4
        s.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{rad}" fill="{COL[k]}" fill-opacity=".85">'
                 f'<title>{e["utc"]}  max {v/1e6:.1f}M газа/блок ({e["gp"]}% soft) [{e["ep_type"]}]</title></circle>')
    s.append(f'<text transform="translate(15,{mt+ph/2:.0f}) rotate(-90)" class="cax" text-anchor="middle">max газ на блок (за 5 мин до сплита) →</text>')
    lx = ml + pw + 16; ly = mt + 8
    for k, name in [('broad', 'May broad'), ('lowtps', 'low-TPS'), ('our-jetton', 'our jetton'),
                    ('compute', 'compute'), ('our-poke', 'our poke'), ('our-destroy', 'our destroy')]:
        s.append(f'<circle cx="{lx+5}" cy="{ly}" r="5" fill="{COL[k]}"/><text x="{lx+15}" y="{ly+4}" class="cxs">{name}</text>')
        ly += 21
    s.append(f'<text x="{lx}" y="{ly+8}" class="cann" fill="#8a8a8a">soft пересекают</text>')
    s.append(f'<text x="{lx}" y="{ly+20}" class="cann" fill="#e56b42">только compute (~15.3M)</text>')
    s.append('</svg>')
    return '\n'.join(s)


# ---------- Chart TIME: split TPS (1-min peak) over time ----------
def chart_time():
    W, Hh = 760, 320; ml, mr, mt, mb = 54, 20, 26, 42
    pw, ph = W - ml - mr, Hh - mt - mb
    YMAX = 500
    def X(t): return ml + (t - TMIN) / (TMAX - TMIN) * pw
    def Y(v): return mt + ph - min(v, YMAX) / YMAX * ph
    s = [f'<svg viewBox="0 0 {W} {Hh}" class="chart" role="img" aria-label="split TPS (1-min peak) over time">']
    s.append(f'<rect x="{ml}" y="{mt}" width="{pw}" height="{ph}" fill="none" stroke="#333"/>')
    for gy in [0, 100, 200, 300, 400, 500]:
        y = Y(gy); s.append(f'<line x1="{ml}" y1="{y:.1f}" x2="{ml+pw}" y2="{y:.1f}" stroke="#1e1e1e"/>')
        s.append(f'<text x="{ml-8}" y="{y+3:.1f}" class="cxs" text-anchor="end">{gy}</text>')
    for mo, lab in [(5, 'май'), (6, 'июн'), (7, 'июл')]:
        t = int(datetime.datetime(2026, mo, 1, tzinfo=datetime.timezone.utc).timestamp())
        if TMIN <= t <= TMAX:
            x = X(t); s.append(f'<line x1="{x:.1f}" y1="{mt}" x2="{x:.1f}" y2="{mt+ph}" stroke="#3a3a3a"/>')
            s.append(f'<text x="{x+4:.0f}" y="{mt+13}" class="cxs">1 {lab}</text>')
    ts = 1780636000
    if TMIN <= ts <= TMAX:
        x = X(ts); s.append(f'<line x1="{x:.1f}" y1="{mt}" x2="{x:.1f}" y2="{mt+ph}" stroke="#e3a83e" stroke-dasharray="4 3"/>')
        s.append(f'<text x="{x+4:.0f}" y="{mt+ph-8:.0f}" class="cann" fill="#e3a83e">← 06-05 всплеск key-блоков</text>')
    # faint 5-min mean series
    for e in EPS:
        k = eck(e['ep_type'])
        s.append(f'<circle cx="{X(ep_ts(e)):.1f}" cy="{Y(float(e["t5"])):.1f}" r="2.4" fill="{COL[k]}" fill-opacity=".28"/>')
    # main 1-min peak series
    for e in EPS:
        k = eck(e['ep_type'])
        s.append(f'<circle cx="{X(ep_ts(e)):.1f}" cy="{Y(float(e["t1"])):.1f}" r="4" fill="{COL[k]}" fill-opacity=".9">'
                 f'<title>{e["utc"]}  {e["t1"]:g} TPS 1m-peak / {e["t5"]:g} 5m [{e["ep_type"]}]</title></circle>')
    s.append(f'<text transform="translate(15,{mt+ph/2:.0f}) rotate(-90)" class="cax" text-anchor="middle">TPS 1-мин пик при сплите →</text>')
    s.append('</svg>')
    return '\n'.join(s)


out = {'gas': chart_gas(), 'time': chart_time()}
json.dump(out, open('/tmp/charts_svg.json', 'w'))
print("charts built (84 episodes):", {k: len(v) for k, v in out.items()})
