import csv, html as H, json
CH=json.load(open('/tmp/charts_svg.json'))
g={r['t0']:r for r in csv.DictReader(open('/tmp/gas.tsv'),delimiter='\t')}
f={r['t0']:r for r in csv.DictReader(open('/tmp/fp.tsv'),delimiter='\t')}
ex={}
for r in csv.reader(open('/tmp/examples.tsv'),delimiter='\t'):
    if r and r[0]!='t0': ex[r[0]]={'acct':r[1],'bn':r[2],'code':r[3],'hex':r[4],'kind':r[5] if len(r)>5 else 'busiest'}
POKE='1784583872'; DESTROY='1784800859'
def classify(t0):
    fr=f[t0]; gr=g[t0]
    tps=float(gr['tps']); tx=int(gr['tx']); tpa=float(fr['txperacc'])
    ours=int(fr['j_ours']); tt=int(fr['toptrace_n'] or 0); avg=float(gr['avggas']); gps=float(gr['sumgas_M'])/300
    if t0==POKE: return ('poke','our poke','big-acct')
    if t0==DESTROY: return ('destroy','our pool destroy','roll-up')
    if ours>=5000: return ('jetton','our jetton flood','inter')
    if tx and tt/tx>=0.20: return ('fanout','fan-out trace','inter')
    if gps>=1.0 or avg>=30000: return ('compute','compute','intra')
    if tps>=80 and tpa<1.7: return ('broad','broad organic flood','inter')
    return ('lowtps','low-TPS fragile','mixed')
TYPE_LABEL={'poke':'our poke (big-acct)','jetton':'our jetton flood','compute':'compute','broad':'broad organic flood','lowtps':'low-TPS fragile','destroy':'our pool destroy','fanout':'fan-out trace'}
EX_KIND={'our-jetton':'жетон-трансфер (наш пул)','destroy-trace':'трейс destroy (весь teardown)','poke':'поук value=1 → big-acct','busiest':'самый нагруженный контракт окна'}
rows=sorted(g.values(), key=lambda r:r['t0'])
sess={l.split('\t')[0]: l.rstrip('\n').split('\t') for l in open('/tmp/sessions.txt')}
import datetime as _dt
def _iso(ts): return _dt.datetime.utcfromtimestamp(int(ts)).strftime('%Y-%m-%dT%H:%M:%S.000Z')
def graf(t0):
    tN=int(sess[t0][3]); frm=_iso(int(t0)-2400); to=_iso(tN+1500)
    return f"https://status.toncenter.com/d/Dij6N5dVz/overview?orgId=1&from={frm}&to={to}&timezone=utc&refresh=5s"
def tv(t0):
    e=ex.get(t0);
    if not e or not e['hex']: return ''
    return f"https://tonviewer.com/transaction/{e['hex']}"
from collections import Counter
cnt=Counter(classify(r['t0'])[0] for r in rows)
tot=sum(int(f[r['t0']]['nsplits']) for r in rows)

CAUSE_CELL={
    'compute':'<span style="color:var(--compute)">gas</span>',
    'jetton':'<span style="color:var(--jetton)">queue</span>',
    'poke':'<span style="color:var(--poke)">long_collation</span>',
    'destroy':'<span style="color:var(--fanout)">queue?</span>',
    'fanout':'<span style="color:var(--fanout)">queue?</span>',
    'broad':'<span style="color:var(--broad)">bytes-load</span>',
    'lowtps':'<span style="color:var(--lowtps)">long_collation</span>',
}
def rowhtml(r):
    t0=r['t0']; fr=f[t0]; cls,lbl,flow=classify(t0)
    gps=float(r['sumgas_M'])/300; tps=float(r['tps'])
    tpsc='hi' if tps>=150 else ('mid' if tps>=60 else 'lo')
    gpsc='hi' if gps>=1.0 else ('mid' if gps>=0.4 else '')
    avgg=int(float(r['avggas'])); avgc='hi' if avgg>=30000 else ('mid' if avgg>=8000 else '')
    hl=' class="poke-row"' if t0==POKE else (' class="destroy-row"' if t0==DESTROY else '')
    e=ex.get(t0,{}); kind=EX_KIND.get(e.get('kind','busiest'),'пример tx')
    tvl=tv(t0)
    tvcell=f'<a class="tvl" href="{tvl}" target="_blank" rel="noopener" title="tonviewer: {kind}">tx ↗</a>' if tvl else '—'
    flowc={'inter':'fl-inter','intra':'fl-intra','mixed':'fl-mixed','big-acct':'fl-intra','roll-up':'fl-intra'}.get(flow,'')
    return (f'<tr{hl}>'
        f'<td class="mono">{fr["utc"][5:]}</td>'
        f'<td class="mono num">{fr["nsplits"]}</td>'
        f'<td class="mono num {tpsc}">{tps:g}</td>'
        f'<td class="mono num {gpsc}">{gps:.2f}</td>'
        f'<td class="mono num {avgc}">{avgg:,}</td>'
        f'<td class="mono num">{fr["txperacc"]}</td>'
        f'<td><span class="chip {cls}">{TYPE_LABEL[cls]}</span></td>'
        f'<td class="mono">{CAUSE_CELL.get(cls,"—")}</td>'
        f'<td class="mono"><span class="flow {flowc}">{flow}</span></td>'
        f'<td class="mono ctr">{tvcell}</td>'
        f'<td class="mono ctr"><a class="gl" href="{graf(t0)}" target="_blank" rel="noopener" title="Grafana: это окно">↗</a></td>'
        f'</tr>')
tablerows='\n'.join(rowhtml(r) for r in rows)

gc1=graf('1782320377'); gc2=graf('1782452241'); gp1=graf('1784583872'); gp2=graf('1784541118'); gd1=graf('1784800859')
tvc1=tv('1782320377'); tvc2=tv('1782452241'); tvp=tv('1784583872'); tvd=tv('1784800859')
tvj=tv('1783933916')
c_gas=CH['gas']; c_time=CH['time']

# ================= verified 84-split / 50-event dataset (source of truth) =================
VD=json.load(open('/tmp/final_verified.json'))
EPS=sorted(VD['episodes'], key=lambda e:e['utc'])
SESS={s['session_id']:s for s in VD['sessions']}
tot=len(EPS)      # 84 before_split splits
nrun=len(SESS)    # 50 events
# event-type counts for the taxonomy tiles
_lt2k={'broad':'broad','lowtps':'lowtps','compute':'compute','our-jetton':'jetton','our-poke':'poke','our-destroy':'destroy'}
_sc=Counter(_lt2k.get(s['load_type'],s['load_type']) for s in VD['sessions'])
cnt={k:_sc.get(k,0) for k in ('lowtps','broad','jetton','compute','poke','destroy')}
# split-type counts
def _ek(et): return 'lowtps' if et.startswith('lowtps') else et
_ec=Counter(_ek(e['ep_type']) for e in EPS)
epcnt={k:_ec.get(k,0) for k in ('lowtps','broad','our-jetton','compute','our-poke','our-destroy')}

EP_CHIP={'broad':('broad','organic flood'),'lowtps':('lowtps','low-TPS fragile'),
  'lowtps (organic tail)':('lowtps','organic tail'),'compute':('compute','compute'),
  'our-jetton':('jetton','our jetton'),'our-poke':('poke','our poke'),'our-destroy':('destroy','our destroy')}

CAUSES_V=json.load(open('/tmp/final_causes.json'))
_CV={(c['utc'],c['shard']):c for c in CAUSES_V}
CAUSE_STYLE={
  ('load_class','gas'):('load_class:gas','var(--compute)'),
  ('load_class','collated_data'):('load_class:cdata','var(--fanout)'),
  ('load_class','bytes'):('load_class:bytes','var(--broad)'),
  ('queue',None):('queue','var(--jetton)'),
  ('long_collation',None):('long_collation','var(--poke)'),
  ('dispatch',None):('dispatch','var(--fanout)'),
  ('unknown',None):('unknown','var(--lowtps)'),
}
def cause_cell(utc,shard):
    c=_CV.get((utc,shard))
    if not c: return '<span style="color:var(--lowtps)">unknown</span>'
    key=(c['primary'], c['load_dim'] if c['primary']=='load_class' else None)
    lbl,col=CAUSE_STYLE.get(key,(c['primary'],'var(--lowtps)'))
    inf = c['confidence']!='confirmed'
    mark='<span class="cq">?</span>' if (inf and c['primary']!='unknown') else ''
    sec=f'<span class="csec">+{c["secondary"]}</span>' if c.get('secondary') else ''
    op=' style="opacity:.72"' if inf else ''
    return (f'<span class="cw"{op} title="{H.escape(c["evidence"])}">'
            f'<span style="color:{col}">{lbl}</span>{mark}{sec}</span>')

def graf_ep(utc):
    t=int(_dt.datetime.strptime(utc,'%Y-%m-%d %H:%M:%S').replace(tzinfo=_dt.timezone.utc).timestamp())
    return f"https://status.toncenter.com/d/Dij6N5dVz/overview?orgId=1&from={_iso(t-2400)}&to={_iso(t+1500)}&timezone=utc&refresh=5s"

def _desc1(sid):
    d=SESS[sid]['description'].strip()
    # keep it concise but retain key evidence (token / TPS / mechanism): 1-2 sentences, ~260 chars
    if len(d)>260:
        cut=d.rfind('. ',0,260)
        if cut<90: cut=d.find('. ')
        if 90<=cut<len(d): d=d[:cut+1]
    return H.escape(d)

def build_rows():
    out=[]; i=0; N=len(EPS)
    while i<N:
        sid=EPS[i]['sid']; j=i
        while j<N and EPS[j]['sid']==sid: j+=1
        span=j-i
        for k in range(i,j):
            ep=EPS[k]; cls,lbl=EP_CHIP.get(ep['ep_type'],('lowtps',ep['ep_type']))
            if ep['ep_type']=='our-poke': hl=' class="poke-row"'
            elif ep['ep_type']=='our-destroy': hl=' class="destroy-row"'
            elif ep['ours']: hl=' class="ours-row"'
            else: hl=''
            gp=ep['gp']; mgc=' hi' if gp>=100 else ''
            t5=ep['t5']; t5c='hi' if t5>=150 else ('mid' if t5>=60 else 'lo')
            ag=int(ep['ag']); agc='hi' if ag>=30000 else ('mid' if ag>=8000 else '')
            row=(f'<tr{hl}>'
                f'<td class="mono">{H.escape(ep["utc"])[5:]}</td>'
                f'<td class="mono">{H.escape(ep["shard"])}</td>'
                f'<td><span class="chip {cls}">{lbl}</span></td>'
                f'<td class="mono num {t5c}">{t5:g}</td>'
                f'<td class="mono num t1">{ep["t1"]:g}</td>'
                f'<td class="mono num {agc}">{ag:,}</td>'
                f'<td class="mono num{mgc}">{ep["mg"]/1e6:.2f}M ({gp}%)</td>'
                f'<td class="mono num">{ep["tpb"]:g}</td>'
                f'<td class="mono cause-td">{cause_cell(ep["utc"],ep["shard"])}</td>')
            if k==i: row+=f'<td class="desc" rowspan="{span}">{_desc1(sid)}</td>'
            row+=(f'<td class="mono ctr"><a class="gl" href="{graf_ep(ep["utc"])}" target="_blank" rel="noopener" title="Grafana: это окно">↗</a></td></tr>')
            out.append(row)
        i=j
    return '\n'.join(out)
tablerows=build_rows()

# cause per load type, verified from validator session-stats archive (color-coded ok/maybe)
CAUSES = [
  ("ok", "итог по механизму: storage-словари, а не большие аккаунты",
   "<b>Тезис.</b> По имеющимся данным low-TPS сплиты объясняются стоимостью <b>перестроения storage-словарей "
   "аккаунтов</b> — обходом деревьев ячеек, ожидающая часть которого приходится на чтения CellDB. "
   "Не компетом, не очередью, не размером блока — и <b>не большими аккаунтами</b>. "
   "<b>Что за этим стоит.</b> У LC-блоков фазы storage занимают <b>97%</b> времени коллации против <b>17%</b> "
   "у контрольных блоков тех же окон; объём обхода <b>16 660</b> ячеек против <b>670</b>, в пересчёте на транзакцию "
   "<b>1 826</b> против <b>111</b>; отношение <span class=\"mono\">real/cpu</span> на фазе <b>1.77</b> против "
   "<b>1.04</b> — то есть процессор ждёт загрузку ячеек, а у контрольных блоков ожидания нет. Доля LC растёт "
   "монотонно с объёмом обхода: от 0% в нижних четырёх децилях до <b>52%</b> в верхнем. "
   "<b>Почему не большие аккаунты.</b> Медиана на блок — <b>19 рядовых аккаунтов</b> (26–3999 ячеек) и "
   "<b>ноль</b> крупных; в <b>62%</b> LC-блоков крупного аккаунта нет вообще; средний обойдённый — <b>719</b> ячеек; "
   "на рядовые приходится <b>69%</b> объёма обхода. Крупные аккаунты дают оставшиеся 30% и все блоки дольше секунды, "
   "но это редкий режим, а не причина класса. "
   "<b>Чего тезис НЕ утверждает.</b> Что обход был единственным потребителем времени во всех без исключения блоках; "
   "что доля чтений с диска одинакова везде (медиана отношения 1.77 — это медиана, а не константа); и главное — "
   "что наблюдаемая плотность LC-блоков достаточна для <span class=\"mono\">want_split</span> (см. оговорку ниже). "
   "Работу порождает поток отвергаемых экстерналов — по причине это вход того же механизма, а не отдельная версия."),
  ("ok", "на что и почему распространён вывод",
   "Из 60 вердиктов <span class=\"mono\">long_collation</span>: <b>19</b> — прямое наблюдение устойчивой серии, "
   "<b>31</b> — условие видно, но выборка редкая, <b>10</b> — покрытия архива нет вовсе и вывод распространён "
   "с известных случаев. Основание для распространения — три независимых пункта. "
   "<b>(1) Базовая частота.</b> Среди органических сплитов после 27.05, где покрытие ЕСТЬ, "
   "<span class=\"mono\">long_collation</span> даёт <b>44 из 46 = 96%</b>; оставшиеся 2 — это compute-аномалии "
   "24.06/26.06, которые опознаются без архива, по одному лишь газу из ClickHouse. "
   "<b>(2) Пропуски не связаны с нагрузкой.</b> Покрыт сплит или нет — зависит от того, какой валидатор "
   "коллатил блок, а не от того, что происходило: у покрытых медиана <b>33.0 TPS</b>, у непокрытых <b>28.9</b>; "
   "доля покрытия по типам 38/45 low-TPS, 6/7 broad, 2/2 compute. Систематического перекоса нет. "
   "<b>(3) Остальные триггеры исключаются независимо от архива.</b> <span class=\"mono\">load_class</span> — "
   "точно, по history_weight, посчитанному на ВСЕХ блоках из ClickHouse; <span class=\"mono\">dispatch</span> — "
   "не сработал ни разу в 1 730 блоках; <span class=\"mono\">queue ≥ 4096</span> не встречается ни в одном "
   "органическом сплите за 3 месяца (только в каскаде нашего флуда 13.07). "
   "Поэтому у непокрытых органических окон после 27.05 просто не остаётся другого кандидата. "
   "<b>Чего распространение НЕ покрывает:</b> 9 сплитов остаются <span class=\"mono\">unknown</span>, и они "
   "непокрыты по другим причинам — 2 из них до 27.05, когда триггер long_collation на майннете ещё не действовал, "
   "а 7 — наши собственные прогоны (каскадные дочерние шарды 13.07, где у соседей подтверждена очередь, "
   "плюс 20.07 09:51 и 23.07). Их база не переносится."),
  ("ok", "почему появились сплиты при 16–49 TPS",
   "Триггер <span class=\"mono\">long_collation</span> написан <b>06.04.2026</b> (коммит "
   "<span class=\"mono\">0d34f2d62</span>, PR #2290, там он назывался <span class=\"mono\">too_long_collation</span> — условие "
   "уже дословно то же), а коммит <span class=\"mono\">f5c669138</span> от 22.05 лишь переименовал его и вывел тайминги "
   "в session-stats. Но <b>действовать на майннете</b> он начал только с обновления ноды между <b>26 и 27 мая</b>: "
   "в архиве поля <span class=\"mono\">check_load_*</span> отсутствуют у 0 из 337 блоков до 26.05 и есть у 100% блоков с 27.05. "
   "<b>Доказательство — прямое.</b> Для мая условие пересчитано из <span class=\"mono\">total_time</span>/"
   "<span class=\"mono\">work_time</span> (<span class=\"mono\">wait_externals ≈ total − work</span>); формула проверена "
   "на 2 550 блоках, где доступны оба способа: совпадение <b>99%</b>, полнота 98%, ложных срабатываний ~0%. "
   "Одной и той же формулой по обе стороны границы: <b>до 27.05</b> условие выполнено у 10/10/41/16/10% блоков по дням, "
   "<b>после</b> — у 4–28%; метка <span class=\"mono\">INBOUND_EXT_MESSAGES: timeout</span> — 9–27% и 0–22% соответственно. "
   "<b>Частоты одинаковые, но в мае нет ни одного low-TPS сплита</b> (минимум 67.9 TPS), а после — 31 событие при 16–48 TPS. "
   "Будь триггер активен в мае, май раскалывался бы так же. Отсюда вывод: <b>нагрузка не менялась — изменилось то, "
   "на что реагирует нода</b>. Дорогие экстерналы были фоном всё время, триггер превратил этот фон в сплиты. "
   "Оговорка: тот же апдейт принёс и другие изменения, так что это точное совпадение по времени плюс механизм, "
   "а не изолированный эксперимент."),
  ("ok", "long_collation — 60 из 84 сплитов",
   "<b>60 из 84 сплитов</b> (остальные: unknown 9, queue 6, load_class:bytes 5, load_class:gas 2, "
   "load_class:cdata 1, dispatch 1). Условие из коллатора проверено дословно по архиву session-stats: "
   "<span class=\"mono\">check_load_total_time &gt; 0.1с</span> И <span class=\"mono\">wait_externals &lt; 0.2·total</span> "
   "И <span class=\"mono\">do_collate &gt; 0.6·total</span> (collator.cpp:5424–5437). Всего разобрано <b>1 730</b> собственноручно "
   "сколлаченных блоков валидаторов на нужных шардах. <b>Контроль</b> (27.05, спокойные часы того же дня): 157 из 158 блоков "
   "условие НЕ выполняют — <span class=\"mono\">total_time</span> те же ~0.40с, но <span class=\"mono\">wait_externals ≈ 0.38с</span> "
   "(коллатор простаивает в ожидании externals). В окнах сплита <span class=\"mono\">wait_externals ≤ 0.002с</span>, а "
   "<span class=\"mono\">do_collate ≈ total</span> — коллатор выжигает весь бюджет. Сигнал различающий, не «всегда истинный». "
   "Максимум коллации по набору — <b>6.44с</b> (10.06 09:35). <b>Переосмысление термина:</b> у не-LC блоков "
   "<span class=\"mono\">check_load_total_time</span> практически та же, что у LC (<b>0.394с</b> против <b>0.378с</b>), "
   "а <span class=\"mono\">work_time</span> — <b>0.035с</b> против <b>0.378с</b>. Эти ~0.4с и есть фиксированный бюджет ожидания "
   "экстерналов, поэтому <span class=\"mono\">long_collation</span> означает не «блок собирался аномально долго», а «коллатор "
   "потратил бюджет на работу вместо ожидания»; 0.37-секундный блок сам по себе перегрузкой не является."),
  ("ok", "load_class достижим ровно в 2 сплитах",
   "Порог <span class=\"mono\">want_split</span> — не один блок, а <b>взвешенная история</b>: "
   "<span class=\"mono\">3·popcount(16) + 2·popcount(16) + 1·popcount(16) − 64 ≥ 0</span> (collator.cpp:5400/5477), то есть перегруз "
   "должен держаться устойчиво в минимум <b>24 перегруженных блока</b> — если это 16 самых свежих (вес 3) плюс 8 следующих (вес 2); при равномерном распределении по всему регистру из 48 блоков нужна доля <b>≥ 2/3</b>, то есть 32 из 48. Этот вес посчитан <b>точно, по КАЖДОМУ блоку</b> из ClickHouse для трёх измеримых "
   "осей. Результат: газ даёт <span class=\"mono\">hw = +4</span> (24.06) и <span class=\"mono\">+15</span> (26.06) — и "
   "<span class=\"mono\">−64</span> во всех остальных 82 сплитах; <b>lt и байты исходящих — −64 везде</b>. Значит триггер load-class "
   "физически не мог развалить ни один другой шард за 3 месяца."),
  ("ok", "queue 4096 — только глубокие дочерние шарды 13.07",
   "Реальная <span class=\"mono\">out_msg_queue</span> видна только в архиве. Порог <span class=\"mono\">FORCE_SPLIT_QUEUE_SIZE = 4096</span> "
   "перейдён в <b>5 сплитах</b>, все — дочерние шарды нашего жетон-флуда 13.07: <span class=\"mono\">2000</span> пик <b>6 196</b>, "
   "<span class=\"mono\">3000</span> 5 108, <span class=\"mono\">5000</span> 4 860, <span class=\"mono\">7000</span> 4 822, "
   "<span class=\"mono\">1000</span> 4 731. У КОРНЕВОГО шарда <span class=\"mono\">8000</span> в те же прогоны очередь ≤ 684 — "
   "корневые сплиты очередью не объясняются. Триггер <span class=\"mono\">dispatch</span> не сработал <b>ни разу</b> "
   "(0 из 1 730 блоков с маркером <span class=\"mono\">total limit reached</span>)."),
  ("maybe", "наши четыре прогона",
   "<b>13.07 (жетон):</b> корень <span class=\"mono\">8000</span> — long_collation (13/14 и 17/25 непустых блоков), очередь ≤684; "
   "каскад вглубь — queue (подтверждено на 5 шардах); один шард <span class=\"mono\">A000</span> — load_class по "
   "<span class=\"mono\">collated_data</span> (1.72–1.80 МБ = 164–172% от soft). "
   "<b>20.07 утро:</b> покрытия архива в момент сплита нет (документированная дыра) → <span class=\"mono\">unknown</span>; "
   "load-class при этом исключён точно. <b>20.07 поук:</b> long_collation, но по косвенным (в окне покрытия нет; 3 из 4 блоков "
   "сразу после сплита удовлетворяют условию). <b>23.07 destroy:</b> сбор session-stats за 23.07 обрывается в 01:27 UTC — "
   "данных нет вообще → <span class=\"mono\">unknown</span>."),
  ("unknown", "что не подтвердилось при перепроверке",
   "Три прежних утверждения сняты. (1) Очередь <b>5 277</b>, которой объясняли сплит 13.07 09:11:56, относится к "
   "<b>09:19:26 на шарде 2000</b> — на два уровня каскада глубже; до 09:18:06 очередь ≥4096 не встречается вообще. "
   "(2) Коллация <b>4.75с</b>, которой объясняли поук 20.07 21:44, в окне поука отсутствует; ближайшее совпадение по всему набору — "
   "<b>4.738с 01.07 13:59:40</b>, другой день и другое событие. (3) Прокси очереди из ClickHouse "
   "(<span class=\"mono\">created − consumed</span>) <b>недостоверен</b>: там, где архив показывает реальную очередь 333 и 438, "
   "прокси давал 6 439 и 12 771. Все выводы «queue» на его основе отозваны."),
  ("unknown", "чего наблюдения не покрывают",
   "<b>Главная оговорка.</b> <span class=\"mono\">want_split</span> требует минимум <b>24 перегруженных блока</b> — если это 16 самых свежих (вес 3) плюс 8 следующих (вес 2); "
   "при равномерном распределении по регистру из 48 блоков нужна доля <b>≥ 2/3</b>, то есть 32 из 48. Наблюдаемая плотность LC-блоков этого НЕ достигает: по 31 low-TPS событию LC составляют <b>18%</b> покрытых "
   "блоков; ни один из <b>37</b> сплитов с покрытием ≥8 блоков не даёт ≥2/3 по окну; в лучшем 25-секундном отрезке ≥2/3 достигают "
   "<b>5 сплитов из 37</b>, медиана лучшего отрезка <b>0%</b>. Два объяснения, различить которые имеющимися данными нельзя: либо "
   "архив недобирает короткий всплеск перегруза (видно 5–8 блоков из каждых ~50, всплеск длится 20–40 с), либо в большинстве "
   "сплитов <span class=\"mono\">want_split</span> выставил не <span class=\"mono\">long_collation</span>. Поэтому вывод "
   "формулируется так: <b><span class=\"mono\">long_collation</span> — единственный уцелевший кандидат по исключению, его сигнатура "
   "присутствует, но требуемая плотность не подтверждена.</b> Отдельно: у <b>17 из 60</b> "
   "<span class=\"mono\">long_collation</span>-сплитов в окне нет ни одного ЗАКОММИЧЕННОГО LC-блока — там вердикт является "
   "экстраполяцией с покрытых окон."),
  ("ok", "органика: тот же механизм при 16–48 TPS",
   "Хрупкие low-TPS сплиты (05.06, 06.06, 07.06, 08.07, 09.07, 10.07) — тот же <span class=\"mono\">long_collation</span> на "
   "обычном майннет-миксе USDT + DEX + EVAA. Газ там 10–40% от soft, очередь единицы–десятки против 4096. То есть <b>шард роняет не "
   "объём нагрузки, а время коллации</b> — и порог по TPS тут ни при чём."),
]

_causes_html = "".join(
  f'<div class="cause {c}"><div class="lt">{lt}</div><div class="dc">{d}</div></div>'
  for c,lt,d in CAUSES)

# the four split triggers, straight from the node collator (queue one highlighted)
TRIGGERS = [
  ("", "load-class ≥ soft", "блок заполнил любую ось лимита (газ / байты / lt / collated_data) до soft-таргета ConfigParam 23.", "collator.cpp:5439–5480"),
  ("", "out_msg_queue ≥ 4096", "бэклог исходящих меж-шардовых сообщений достиг FORCE_SPLIT_QUEUE_SIZE. Сработал в 6 сплитах из 84 — все дочерние шарды 13.07.", "collator.cpp:51"),
  ("q", "long_collation", "коллация блока заняла слишком долго: total &gt; 0.1с И wait_externals &lt; 0.2·total И do_collate &gt; 0.6·total. <b>Самый частый вердикт: 60 из 84 сплитов.</b>", "collator.cpp:5424–5437"),
  ("", "dispatch_queue лимит", "исчерпан общий лимит dispatch-очереди.", "dispatch_queue_total_limit_reached"),
]
_triggers_html = "".join(
  f'<div class="trig {c}"><div class="n">триггер {i}</div><h4>{t}</h4><p>{d}</p><div class="ref mono">{r}</div></div>'
  for i,(c,t,d,r) in enumerate(TRIGGERS,1))

# ---- Hypotheses table (section 5) ----
def _res(txt,cls): return f'<span class="res {cls}">{txt}</span>'
HYP = [
  ("H1","«Сплит газо-ограничен» (унаследовано из ошибочного разбора ConfigParam 22 = мастерчейн gas soft 1M).",
   "Пересняли лимиты правильного ConfigParam <b>23</b> (базчейн, gas soft 10M) и наложили max газ/блок по всем 84 сплитам (50 событиям) на этот soft.",
   _res("ОПРОВЕРГНУТА","refuted")+" — при soft 10M порог пересекают только 2 compute-аномалии; у остальных 82 сплитов (48 событий) газ ниже soft, значит их триггер не газ.",
   '<span class="src">(декод ConfigParam 23; ChartGAS: maxgas.tsv vs 10M)</span>'),
  ("H2","Дрейф TPS-порога сплита — органический (свойство самой нагрузки).",
   "Сопоставили порог по событиям во времени (chart TIME) с изменениями в сети/конфиге; проверили ConfigParam 23 на всплеске key-блоков 06-05.",
   _res("ПЕРЕОСМЫСЛЕНА","reframed")+" — дрейф реален и он сетевой/конфиг-side, а не органический. Но <b>механизм переопределён</b>: корневые сплиты обоих прогонов шли по <span class=\"mono\">long_collation</span>, а не по очереди, так что «сеть стала быстрее дренировать очередь» больше не является объяснением дрейфа; чем именно ускорилась коллация — открытый вопрос.",
   '<span class="src">(ClickHouse blocks TPS×время; наши прогоны 13.07/20.07)</span>'),
  ("H3","Большие аккаунты — «ахиллесова пята» шарда через VM-газ.",
   "Корреляция maxstor (макс. стейт аккаунта в окне) с газом и TPS; затем поук value=1 в 100 больших аккаунтов и чтение session-stats корневого блока.",
   _res("ОПРОВЕРГНУТА","refuted")+" для газ-канала (maxstor r≈0.05 с газом/TPS); "+_res("ПЕРЕОСМЫСЛЕНА","reframed")+" через long_collation. Для поука прежнее «ПОДТВЕРЖДЕНА (ctt 4.75с на 1 tx)» <b>снято</b>: в окне поука архив не покрыл ни одного блока шарда, а блок с 4.738с нашёлся в другой день (01.07 13:59). Остаётся вывод по исключению — load-class исключён точно, а 3 из 4 блоков сразу после сплита дают long_collation.",
   '<span class="src">(ClickHouse maxstor corr; session-stats 20.07, покрытия в окне нет)</span>'),
  ("H4","Майский сплит 26.05 (broad, 223 TPS) — газо-ограничен.",
   "Прочли session-stats корневых блоков окна 26.05: block_full по осям, cat_gas/cat_bytes, tx/блок.",
   _res("ОПРОВЕРГНУТА","refuted")+" для газа и "+_res("ПЕРЕОСМЫСЛЕНА","reframed")+" по механизму: отдельные блоки действительно упирались в байтовый soft (block_full bytes 1 048 855, cat_bytes=2, 373 tx/блок), но взвешенной истории это не даёт — по оси байтов <span class=\"mono\">hw = −64</span>. Устойчиво срабатывал <span class=\"mono\">long_collation</span>; байты — сопутствующий фактор.",
   '<span class="src">(session-stats 26.05, collatedBlock, wc=0)</span>'),
  ("H5","Наш жетон-флуд всегда сплитит около ~250 TPS (по 5-мин среднему).",
   "Пересчитали мгновенный порог по 30-с окнам (наш rate = переводы/с ×4 + органика) для прогонов 13.07 и 20.07.",
   _res("ОПРОВЕРГНУТА","refuted")+" — 5-мин среднее (13.07 ~203 vs 20.07 268) размывает ramp; реальный порог скакнул <b>200 TPS (13.07) → 330 TPS (20.07)</b>, ×1.6. Альт-подгипотеза (фоновый big-account → long_collation на 13.07) при перепроверке <b>сменила знак</b>: полный проход по архиву (sudo, все 11 хостов) показал, что корневые сплиты 13.07 как раз и шли по <span class=\"mono\">long_collation</span> (13/14 и 17/25 непустых блоков), а очередь ≥4096 появилась только двумя уровнями каскада ниже.",
   '<span class="src">(ClickHouse 30-с TPS + лог прогонов; session-stats 13.07, wc=0; см. §4.8)</span>'),
  ("H6","Сплит 05.06 (low-TPS, 16.8 TPS) — слабый/пограничный, возможно шум.",
   "Прочли session-stats по всем 6 сплитам дня (05:12, 07:01, 07:06, 08:49, 17:47, 17:55): block_full, out_msg_queue, check_load_total_time, limits_log.",
   _res("ПОДТВЕРЖДЕНА","confirmed")+" как <b>long_collation</b>: ctt 0.87→3.13с на почти пустых блоках при block_full=0 и очереди 20–44 — то есть вектор реальный, а не шум. Метка INBOUND_EXT: timeout в этих блоках есть, но механизм по ней не определяется (§4.7b); что именно съело бюджет — перестроение storage-словарей на потоке экстерналов.",
   '<span class="src">(session-stats 05.06 ×6 сплитов, wc=0)</span>'),
  ("H7","Органические low-TPS сплиты вызваны медленной CellDB: в них должны быть задеты большие (или неудачно разложенные) аккаунты.",
   "Разбор фаз коллации (<span class=\"mono\">work_time_real_stats</span>) и счётчиков <span class=\"mono\">storage_stat_cache</span> для <b>195</b> LC-блоков против <b>854</b> контрольных блоков ТЕХ ЖЕ окон (31 low-TPS событие, 1 049 покрытых блоков, включая пустые); объём обхода считается как <span class=\"mono\">miss_cells + small_cells</span>; затем поаккаунтная атрибуция шести сплитов по ClickHouse + размеры стейта.",
   _res("ПЕРЕОСМЫСЛЕНА","reframed")+" — как МАССОВАЯ причина большие аккаунты "+_res("ОПРОВЕРГНУТЫ","refuted")+", но и противопоставления «CellDB против экстерналов» нет: это одна цепочка. LC-блоки против контроля (медианы): предложено экстерналов <b>55</b> против 17, отклонено <b>54</b> против 14, инстанцировано аккаунтов <b>19</b> против 5, обход <b>16 660</b> против 670 ячеек (1 826 против 111 на транзакцию), фаза <span class=\"mono\">prelim</span> <b>97%</b> против 17% коллации, real/cpu <b>1.77</b> против 1.04. По коду это вход и выход одного механизма: поток экстерналов → инстанцирование получателя на каждый → перестроение storage-словаря (кэш не держит аккаунты &lt;4000 ячеек). Корреляции по 1 049 блокам: отклонённые экстерналы ↔ обход <b>r=+0.60</b>, аккаунты ↔ обход +0.51, обход ↔ время <span class=\"mono\">prelim</span> +0.57. Доля LC по децилям обхода растёт монотонно, до <b>52%</b> в верхнем дециле. Поаккаунтно: медиана на блок — <b>19 мелких</b> аккаунтов (26–3999 ячеек) и <b>0 крупных</b> (≥4000), средний обойдённый мелкий — <b>719</b> ячеек; по объёму обхода мелкие <b>69%</b>, крупные <b>30%</b>. Крупный аккаунт — редкий крайний случай: <b>в 62% LC-блоков его нет вообще</b>, но именно он даёт все блоки дольше секунды, а тёплый кэш ускоряет ту же фазу в <b>56 раз</b>. Чинить, соответственно, надо порог кэширования и поток отвергаемых экстерналов, а не «большие аккаунты». <b>Оговорка:</b> требуемой для <span class=\"mono\">want_split</span> плотности (≥2/3 блоков) наблюдаемые <b>18%</b> LC не достигают.",
   '<span class="src">(архив session-stats: 195 LC vs 854 контрольных; §4.7b)</span>'),
  ("H8","Low-TPS сплиты вызваны походами в CellDB за большими аккаунтами.",
   "Сравнили LC-блоки с контрольными блоками ТЕХ ЖЕ окон по разбивке фаз коллации (<span class=\"mono\">work_time_real_stats</span>), по объёму обхода (<span class=\"mono\">miss_cells + small_cells</span>), по отношению real/cpu и по составу затронутых аккаунтов; 195 LC против 854 контрольных.",
   _res("ПОДТВЕРЖДЕНА","confirmed")+" в части CellDB и "+_res("ОПРОВЕРГНУТА","refuted")+" в части «больших аккаунтов». Время уходит в перестроение storage-словарей (97% против 17% у контроля, обход 16 660 против 670 ячеек, real/cpu 1.77 против 1.04). Но обходятся <b>рядовые</b> аккаунты: медиана 19 мелких и 0 крупных на блок, в 62% блоков крупного нет вовсе, средний обойдённый 719 ячеек, 69% объёма — на мелких. Порог кэша <span class=\"mono\">MIN_ACCOUNT_CELLS = 4000</span> гарантирует, что этот класс не кэшируется никогда.",
   '<span class="src">(архив session-stats: 195 LC vs 854 контрольных; §4.7b)</span>'),
]
_hyp_rows = "".join(
  f'<tr><td class="hid">{hid}</td><td>{hyp}</td><td>{meth}</td><td>{res}</td><td>{data}</td></tr>'
  for hid,hyp,meth,res,data in HYP)

# ---- freq -> TPS -> split tables (section 4.8) ----
FREQ_1307 = """<div class="tscroll"><table class="mini">
  <thead><tr><th>req/s</th><th class="num">наш TPS (×4)</th><th>сплит</th></tr></thead>
  <tbody>
    <tr><td class="mono">100</td><td class="mono num">~420–486</td><td>СПЛИТ (08:24)</td></tr>
    <tr><td class="mono">63</td><td class="mono num">~250–280</td><td>СПЛИТ (08:36)</td></tr>
    <tr class="thr-row"><td class="mono">44</td><td class="mono num">~200–220</td><td>СПЛИТ (08:46) <span class="hit">← порог</span></td></tr>
    <tr><td class="mono">35</td><td class="mono num">~165–180</td><td>нет (держали ~20 мин)</td></tr>
  </tbody>
</table></div>"""
FREQ_2007 = """<div class="tscroll"><table class="mini">
  <thead><tr><th>req/s</th><th class="num">наш TPS (×4)</th><th>сплит</th></tr></thead>
  <tbody>
    <tr><td class="mono">44</td><td class="mono num">~210–225</td><td>нет</td></tr>
    <tr><td class="mono">54</td><td class="mono num">~252–275</td><td>нет</td></tr>
    <tr><td class="mono">64</td><td class="mono num">~285–307</td><td>нет</td></tr>
    <tr class="thr-row"><td class="mono">75</td><td class="mono num">~330–348</td><td>СПЛИТ (09:51) <span class="hit">← порог</span></td></tr>
  </tbody>
</table></div>"""

# ---- ConfigParam 22 vs 23 decode (appendix) ----
CONFIG_DECODE = """<div class="tscroll"><table class="mini">
  <thead><tr><th>ось</th><th class="num">underload</th><th class="num">soft</th><th class="num">hard</th></tr></thead>
  <tbody>
    <tr><td class="mono">bytes</td><td class="mono num">256 K</td><td class="mono num">1 M</td><td class="mono num">2 M</td></tr>
    <tr><td class="mono">gas</td><td class="mono num">2 M</td><td class="mono num">10 M</td><td class="mono num">20 M</td></tr>
    <tr><td class="mono">lt_delta</td><td class="mono num">1 000</td><td class="mono num">5 000</td><td class="mono num">10 000</td></tr>
  </tbody>
</table></div>"""

# ---- session-stats field schema (appendix) ----
FIELDS = [
  ("workchain","0 = базчейн, −1 = мастерчейн. ОБЯЗАТЕЛЬНЫЙ фильтр == 0 (0x8000 общий для обоих)."),
  ("block_full &lt;axis&gt;","абсолютное заполнение блока по оси (gas / bytes) — с чем сравнивать soft ConfigParam 23."),
  ("cat_gas / cat_bytes","load-класс по оси (0 underload … 3 &gt; hard); class ≥ soft взводит бит перегруза."),
  ("out_msg_queue","длина очереди исходящих меж-шардовых сообщений; ≥ 4096 (FORCE_SPLIT_QUEUE_SIZE) = queue-триггер."),
  ("new_out_msg_queue_size","строка → tonumber; очередь после коллации блока (дренаж/рост)."),
  ("check_load_total_time","время коллации (сек); ~0.4с — фиксированный бюджет, поэтому одна ctt не различает перегруз; сравнивать с work_time (0.035с у не-LC против 0.378с у LC)."),
  ("limits_log","свободный текст: INBOUND[_EXT]: timeout, long_collation, dispatch_queue… — per-block тэг того, ГДЕ кончился бюджет; механизм по нему не определяется (§4.7b)."),
]
_fields_rows = "".join(f'<tr><td class="mono">{k}</td><td>{d}</td></tr>' for k,d in FIELDS)

CSS = """
:root{
  --accent:#e3a83e;
  --poke:#d472d4; --jetton:#5b9bd5; --compute:#e56b42; --broad:#37b3a5; --lowtps:#94a4ab; --fanout:#9d80e6;
  --hi:#e07a4b; --midc:#e3a83e; --loc:#8a9aa0; --grid:#1e1e1e;
  --confirmed:#4fb06a; --refuted:#d9655a; --reframed:#e3a83e;
}
*{box-sizing:border-box}
html{scroll-behavior:smooth}
body{margin:0;background:#000;color:#e0e0e0;font-family:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;font-size:15.5px;line-height:1.62;-webkit-font-smoothing:antialiased;max-width:100%;overflow-x:clip;}
img,svg{max-width:100%;}
.mono{font-family:ui-monospace,"SF Mono","Cascadia Code",Menlo,Consolas,monospace;font-variant-numeric:tabular-nums;}
.wrap{max-width:820px;margin:0 auto;padding:40px 20px 100px;}
a{color:#cfd8dc;text-decoration:underline;text-underline-offset:2px;}
a:hover{color:#fff;}
h1{font-size:29px;line-height:1.22;margin:0 0 16px;font-weight:700;color:#fff;}
h2{font-size:21px;margin:52px 0 12px;padding-top:22px;border-top:1px solid #2a2a2a;font-weight:700;color:#fff;}
h3{font-size:16.5px;margin:28px 0 8px;font-weight:700;color:#f0f0f0;}
h4{font-size:14.5px;margin:20px 0 6px;font-weight:700;color:#e8e8e8;}
p{margin:11px 0;}
strong,b{color:#fff;font-weight:650;}
.src{color:#7d7d7d;font-size:12px;font-family:ui-monospace,monospace;font-variant-numeric:tabular-nums;}
.lead{color:#c4c4c4;}
ul,ol{padding-left:22px;} li{margin:7px 0;}
hr{border:0;border-top:1px solid #2a2a2a;margin:26px 0;}
.abstract{color:#c8c8c8;font-size:16px;}
.tscroll{overflow-x:auto;max-width:100%;min-width:0;}
table{border-collapse:collapse;width:100%;font-size:13px;}
.tscroll table{width:max-content;min-width:100%;}
thead th{text-align:left;padding:9px 11px;font-family:ui-monospace,monospace;font-size:10.5px;letter-spacing:.06em;text-transform:uppercase;color:#9a9a9a;border-bottom:1px solid #3a3a3a;white-space:nowrap;}
tbody td{padding:7px 11px;border-bottom:1px solid #1a1a1a;white-space:nowrap;vertical-align:top;}
tbody tr:hover{background:#0d0d0d;}
td.num{text-align:right;} td.ctr{text-align:center;}
.flow{font-size:12px;}
td.hi{color:var(--hi);font-weight:600;} td.mid{color:var(--midc);} td.lo{color:var(--loc);}
tr.poke-row{background:color-mix(in srgb,var(--poke) 14%,#000);}
tr.destroy-row{background:color-mix(in srgb,var(--fanout) 12%,#000);}
tr.ours-row{background:color-mix(in srgb,var(--jetton) 9%,#000);}
td.t1{color:#f2f2f2;font-weight:650;}
td.desc{white-space:normal;text-align:left;font-size:11px;line-height:1.42;color:#9a9a9a;max-width:300px;min-width:220px;vertical-align:top;border-left:1px solid #222;}
.chip{display:inline-block;font-family:ui-monospace,monospace;font-size:11px;padding:2px 7px;border:1px solid;white-space:nowrap;font-weight:500;}
.chip.poke{color:var(--poke);border-color:color-mix(in srgb,var(--poke) 55%,#000);}
.chip.jetton{color:var(--jetton);border-color:color-mix(in srgb,var(--jetton) 55%,#000);}
.chip.compute{color:var(--compute);border-color:color-mix(in srgb,var(--compute) 55%,#000);}
.chip.broad{color:var(--broad);border-color:color-mix(in srgb,var(--broad) 55%,#000);}
.chip.lowtps{color:var(--lowtps);border-color:color-mix(in srgb,var(--lowtps) 50%,#000);}
.chip.destroy{color:var(--fanout);border-color:color-mix(in srgb,var(--fanout) 55%,#000);}
.chip.fanout{color:var(--fanout);border-color:color-mix(in srgb,var(--fanout) 55%,#000);}
.tvl{color:var(--jetton);text-decoration:underline;font-size:11.5px;}
.gl{color:var(--accent);text-decoration:underline;font-size:12px;}
.legend{display:flex;gap:16px;flex-wrap:wrap;margin:14px 2px;font-size:12px;color:#a8a8a8;font-family:ui-monospace,monospace;}
.legend span{display:inline-flex;align-items:center;gap:6px;}
.dot{width:9px;height:9px;display:inline-block;}
.tax{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:1px;margin:16px 0;background:#2a2a2a;border:1px solid #2a2a2a;}
.tax .t{background:#000;padding:12px 14px;}
.tax .n{font-family:ui-monospace,monospace;font-size:24px;font-weight:600;}
.tax .l{font-size:12px;color:#a0a0a0;margin-top:3px;line-height:1.35;}
.note{margin:16px 0;background:#0b0b0b;border:1px solid #262626;padding:14px 16px;font-size:13.5px;color:#b0b0b0;}
.note b{color:#eee;}
.deflist{display:grid;grid-template-columns:auto 1fr;gap:6px 14px;margin:10px 0 0;font-size:13.5px;align-items:baseline;}
.deflist dt{font-family:ui-monospace,monospace;font-weight:600;}
.deflist dd{margin:0;color:#a8a8a8;}
.cw{font-family:ui-monospace,monospace;white-space:nowrap;}
.cq{color:#7a7a7a;margin-left:1px;}
.csec{color:#6a6a6a;font-size:10.5px;margin-left:5px;}
.cause-td{font-size:11.5px;}
.timeline{margin-top:16px;border:1px solid #262626;}
.band{display:grid;grid-template-columns:150px 1fr;background:#000;}
.band+.band{border-top:1px solid #262626;}
.band .when{padding:14px 16px;font-family:ui-monospace,monospace;font-size:13px;color:#e0e0e0;background:#0b0b0b;border-right:1px solid #262626;display:flex;flex-direction:column;justify-content:center;}
.band .when span{color:#8a8a8a;font-size:11px;}
.band .what{padding:14px 18px;}
.band .what .thr{font-family:ui-monospace,monospace;font-weight:600;font-size:14px;}
.band .what p{margin:3px 0 0;font-size:13.5px;color:#b0b0b0;}
.thr.lo{color:var(--broad);} .thr.mid{color:var(--midc);} .thr.hi{color:var(--compute);}
.chart{width:100%;height:auto;display:block;margin:8px 0 4px;font-family:ui-monospace,monospace;background:#000;border:1px solid #262626;padding:8px;}
.cxs{font-size:10px;fill:#8a8a8a;} .cax{font-size:11px;fill:#d0d0d0;} .cann{font-size:10.5px;font-family:system-ui;}
figure{margin:18px 0 26px;} figcaption{font-size:13px;color:#a8a8a8;margin-top:8px;} figcaption b{color:#ddd;}
.limits{display:grid;grid-template-columns:repeat(3,1fr);gap:1px;margin:16px 0;background:#2a2a2a;border:1px solid #2a2a2a;}
.limit{background:#000;padding:13px 15px;}
.limit .d{font-family:ui-monospace,monospace;font-size:10.5px;text-transform:uppercase;letter-spacing:.08em;color:#9a9a9a;}
.limit .v{font-family:ui-monospace,monospace;font-size:14px;margin-top:5px;color:#e8e8e8;}
.limit.gas .d{color:var(--compute);}
.limit.gas .v{color:var(--compute);font-weight:600;}
.triggers{display:grid;grid-template-columns:repeat(2,1fr);gap:12px;margin:16px 0;}
.trig{background:#0b0b0b;border:1px solid #262626;padding:14px 15px;display:flex;flex-direction:column;}
.trig .n{font-family:ui-monospace,monospace;font-size:10px;text-transform:uppercase;letter-spacing:.08em;color:#8a8a8a;}
.trig h4{margin:6px 0 6px;font-size:13px;font-family:ui-monospace,monospace;color:#e8e8e8;}
.trig p{margin:0;font-size:12.5px;color:#a8a8a8;}
.trig .ref{font-size:10.5px;color:#7a7a7a;margin-top:9px;font-family:ui-monospace,monospace;}
.trig.q{border-color:var(--jetton);}
.trig.q h4{color:var(--jetton);}
.causes{display:grid;gap:9px;margin:16px 0 0;}
.cause{display:grid;grid-template-columns:180px 1fr;gap:16px;align-items:baseline;background:#0b0b0b;border:1px solid #262626;padding:12px 15px;font-size:13.5px;}
.cause .lt{font-family:ui-monospace,monospace;font-weight:600;color:var(--cc,#e8e8e8);font-size:12.5px;}
.cause .dc{color:#b0b0b0;} .cause .dc b{color:var(--cc,#fff);font-weight:650;}
.cause.ok{--cc:var(--confirmed);} .cause.maybe{--cc:var(--accent);} .cause.unknown{--cc:var(--lowtps);}
.dives{display:grid;grid-template-columns:1fr;gap:14px;margin-top:18px;}
.dive{background:#0b0b0b;border:1px solid #262626;padding:18px 20px;}
.dive.d-compute{--dv:var(--compute);} .dive.d-poke{--dv:var(--poke);} .dive.d-destroy{--dv:var(--fanout);}
.dive .k{font-family:ui-monospace,monospace;font-size:11px;letter-spacing:.08em;text-transform:uppercase;color:var(--dv,var(--accent));}
.dive h3{margin:6px 0 8px;font-size:16px;}
.dive p{font-size:13.5px;color:#b0b0b0;margin:0;}
.dive .fig{font-family:ui-monospace,monospace;font-size:12.5px;color:#d0d0d0;margin:12px 0 0;padding-top:12px;border-top:1px solid #262626;display:grid;gap:4px;}
.dive .fig b{color:var(--dv,var(--accent));font-weight:600;}
.dive .links{margin-top:12px;font-family:ui-monospace,monospace;font-size:12px;display:flex;gap:14px;flex-wrap:wrap;}
.dive .links a{color:var(--dv,var(--accent));}
table.hyptable{width:100%;min-width:0;}
.hyptable td{white-space:normal;vertical-align:top;font-size:13px;}
.hyptable td.hid{font-family:ui-monospace,monospace;color:#cfd8dc;font-weight:600;white-space:nowrap;}
.res{font-family:ui-monospace,monospace;font-size:12px;font-weight:600;}
.res.confirmed{color:var(--confirmed);} .res.refuted{color:var(--refuted);} .res.reframed{color:var(--reframed);}
.placeholder{background:#0b0b0b;border:1px solid #262626;padding:16px 18px;color:#b0b0b0;font-size:14px;}
.thresh-grid{display:grid;grid-template-columns:1fr 1fr;gap:16px;margin:14px 0 0;}
.thresh-card{background:#0b0b0b;border:1px solid #262626;padding:14px 16px 8px;min-width:0;overflow-x:auto;}
.thresh-card h4{margin:0 0 6px;font-size:13.5px;font-family:ui-monospace,monospace;color:var(--jetton);}
table.mini{border-collapse:collapse;width:max-content;min-width:100%;font-size:12.5px;margin:2px 0 0;}
table.mini td,table.mini th{padding:5px 10px 5px 0;border-bottom:1px solid #1a1a1a;text-align:left;white-space:nowrap;}
table.mini thead th{background:none;text-transform:none;letter-spacing:0;font-size:10.5px;color:#9a9a9a;border-bottom:1px solid #3a3a3a;padding:5px 10px 5px 0;}
table.mini td.num{text-align:right;padding-right:14px;}
table.mini tr.thr-row td{color:var(--jetton);font-weight:600;}
table.mini.fields{width:100%;min-width:0;}
table.mini.fields td:last-child{white-space:normal;}
table.mini .hit{color:var(--accent);font-weight:600;}
.foot{margin-top:44px;padding-top:18px;border-top:1px solid #2a2a2a;font-family:ui-monospace,monospace;font-size:11.5px;color:#7a7a7a;}
@media (max-width:640px){.triggers{grid-template-columns:1fr;}.cause{grid-template-columns:1fr;gap:3px;}.limits{grid-template-columns:1fr;}.thresh-grid{grid-template-columns:1fr;}.band{grid-template-columns:1fr;}.band .when{border-right:none;border-bottom:1px solid #262626;}.deflist{grid-template-columns:1fr;}}
"""

BODY = f"""
<div class="wrap">

<h1>Обзор сплитов базчейн-шарда TON под нагрузкой</h1>
<p class="abstract">Рабочие заметки о том, как корневой шард базчейна TON (0x8000…) дробится (сплитится) под нагрузкой и почему TPS-порог сплита прыгал от недели к неделе — то ~200, то ~330, то вовсе 16 TPS. Материал охватывает каждый сплит корневого шарда за период <b>05-10 → 07-23 2026</b>: измеренные TPS/газ/тип нагрузки из ClickHouse плюс атрибуцию причины по архиву session-stats валидаторов. Заметки идут по порядку — от источников и методов к ходу исследования (включая ошибочные повороты), гипотезам и открытым вопросам. Выводы пока не формулируются.</p>
<p class="src">окно 05-10 → 07-23 2026 · сплитов {tot} в {nrun} событиях · корневой шард 0x8000 = −9223372036854775808 · пул-минтер 0:A0295D8A…</p>

<h2>1. Что изучаем</h2>
<p>При достаточной нагрузке корневой шард базчейна дробится на полушарды (и дальше), а при спаде — мёржится обратно. За 3 месяца таких сплитов набралось много, и порог по TPS, при котором это происходило, оказался нестабильным. Вопрос заметок: какой именно ресурс блока упирается в лимит на каждом сплите (газ, байты, очередь меж-шардовых сообщений или время коллации) и почему порог дрейфовал. Отдельный подвопрос — правда ли, что касание «больших аккаунтов» (тяжёлый стейт) способно сплитить шард.</p>

<h2>2. Источники данных</h2>

<h3>a. ClickHouse mainnet_d</h3>
<p>Таблицы <span class="mono">blocks</span> / <span class="mono">transactions</span> / <span class="mono">messages</span> / <span class="mono">jetton_transfers</span>, реальное время. Даёт сам факт сплита (<span class="mono">before_split</span>), TPS, газ и байты на блок, состав транзакций и трейсы. <span class="src">(доступ: SQL к mainnet_d)</span></p>

<h3>b. Архив session-stats на devnet-log</h3>
<p>Per-validator gzip JSON-lines по пути <span class="mono">/nvmepool/…/DD.MM.YYYY/sftp-swisscops/*.gz</span> — примерно <b>11 из ~350</b> валидаторов, глубина ~3 мес. Читается <span class="mono">sudo -n zcat &lt;file&gt; | jq</span>. Даёт то, чего нет в ClickHouse: <span class="mono">out_msg_queue</span>, <span class="mono">check_load_total_time</span>, <span class="mono">block_full</span> по осям, <span class="mono">limits_log</span> — именно по ним и делается атрибуция причины сплита. <span class="src">(devnet-log, проверено 2026-07-24)</span></p>

<h3>c. Исходники ноды (ton/)</h3>
<p>C++ коллатор: логика решения о сплите (<span class="mono">Collator::check_block_overload</span>) и константы триггеров. Отсюда — точные условия перегруза и то, какой ConfigParam берётся для лимитов. <span class="src">(ton/ collator.cpp, mc-config.cpp)</span></p>

<h3>d. Локальные адреса</h3>
<p><span class="mono">big_accounts_full.txt.sizes</span> — топ-20 000 стейтов размером ≥36 КБ (цели поук-теста, размер-сортировано); адреса нашего пула из <span class="mono">go-spam cmd/pooladdrs</span> (сверка отправителей/получателей в трейсах). <span class="src">(локальные файлы репо)</span></p>

<h3>e. tonviewer / Grafana</h3>
<p>Примеры конкретных транзакций (tonviewer) и обзорные графики окна сплита (Grafana <span class="mono">status.toncenter.com</span>) — ссылки в таблице приложения 9.1. <span class="src">(внешние UI)</span></p>

<h2>3. Методы</h2>

<h3>Детект сплита</h3>
<p><span class="mono">blocks WHERE workchain=0 AND before_split</span>; корневой шард = 0x8000 = <span class="mono">−9223372036854775808</span>. <span class="src">(ClickHouse mainnet_d.blocks)</span></p>

<h3>TPS</h3>
<p><span class="mono">sum(transaction_count)/window</span> по блокам wc=0. <span class="src">(ClickHouse mainnet_d.blocks, wc=0)</span></p>

<h3>Отпечаток события</h3>
<p>Окно = <b>5 мин до первого before_split</b> события; по нему газ (sum/avg/p99), uacc, tx/acc, доля наших жетон-трансферов. <span class="src">(ClickHouse, окно 300 с)</span></p>

<h3>Газ на блок</h3>
<p>max / avg / p99 <span class="mono">gas_used</span> на блок в окне; <span class="mono">max_gas_block</span> против soft-лимита. <span class="src">(ClickHouse mainnet_d.transactions/blocks)</span></p>

<h3>Декод ConfigParam</h3>
<p><span class="mono">getConfigParam 22/23</span> → распаковка block-limits (bytes / gas / lt: underload / soft / hard). Полный декод — приложение 9.2. <span class="src">(getConfigParam 23)</span></p>

<h3>Атрибуция причины по session-stats — рецепт и поправки метода</h3>
<p>Читаем session-stats корневых блоков в окне сплита и определяем, какая ось упёрлась в лимит. Метод потребовал нескольких поправок:</p>
<ul>
  <li><b>Фильтр <span class="mono">workchain == 0</span> обязателен.</b> Шард <span class="mono">0x8000</span> общий для мастерчейна (wc −1) и базчейна (wc 0) — без фильтра мешаются мастер-блоки. <span class="src">(session-stats)</span></li>
  <li><b><span class="mono">before_split mc_seqno</span> отстаёт от реального перегруза на ~1–4 мин.</b> Плюс покрытие архива редкое (<b>~11 из ~350</b> валидаторов), поэтому берём ШИРОКОЕ окно и читаем последние корневые блоки перед split-gap + первые полушарды после него. <span class="src">(devnet-log, ~11/350)</span></li>
  <li><b><span class="mono">new_out_msg_queue_size</span> — строка,</b> нужен <span class="mono">tonumber</span>. <span class="src">(session-stats)</span></li>
  <li><b><span class="mono">check_load_total_time</span> сама по себе признаком не является:</b> ~0.4с — это фиксированный бюджет, и он расходуется и без перегруза (у не-LC блоков ctt 0.394с против 0.378с у LC). Различает <span class="mono">work_time</span>: 0.035с против 0.378с — то есть отдыхал коллатор или работал. <span class="src">(session-stats + collator.cpp)</span></li>
  <li><b><span class="mono">limits_log</span> — свободный текст</b> (<span class="mono">INBOUND: timeout</span>, <span class="mono">long_collation</span>, …). Тэг говорит, ГДЕ кончился бюджет коллации, а не на что он ушёл: дедлайн один на весь блок, проверка стоит в начале цикла, поэтому <span class="mono">INBOUND_*: timeout</span> механизм не идентифицирует (§4.7b). <span class="src">(session-stats)</span></li>
</ul>

<h4>Оговорки метода (ограничения измерения)</h4>
<ul>
  <li><b>Блок-данные мерят только 3 из 4 осей.</b> ClickHouse даёт газ / байты / lt на блок, но <b>не</b> <span class="mono">out_msg_queue</span>, <span class="mono">collated_data</span> и время коллации — а именно они триггерят большинство наших сплитов. Поэтому причины разобраны по архиву session-stats, а не по блок-данным. <span class="src">(ClickHouse vs session-stats)</span></li>
  <li><b>«TPS» события — 5-мин среднее, занижает порог наших жетон-флудов.</b> При бинарном поиске freq рос внутри окна (для 07-20 09:51 окно ещё захватило спад после остановки) — среднее размывает ramp. Истинные пороги — по 30-с разрешению (§4.8). <span class="src">(ClickHouse, окно 300 с)</span></li>
  <li><b>Газ ≠ CellDB-I/O.</b> <span class="mono">gas_used</span> мерит VM-вычисления, а не загрузку ячеек из CellDB при коллации; стоимость большого стейта идёт через <span class="mono">long_collation</span> (время). <span class="src">(collator.cpp)</span></li>
  <li><b>Историческое ConfigParam 23 за май/июнь не восстановить</b> — читаем текущее значение; каким soft был на старых сплитах, из доступных данных не следует. <span class="src">(getConfigParam — только текущее)</span></li>
  <li><b>Ось байтов измеряется частично.</b> <span class="mono">estimate_block_size</span> считает полный блок; из ClickHouse видны лишь байты исходящих сообщений — байтовая ось недо-измерена. <span class="src">(ClickHouse messages)</span></li>
</ul>

<h2>4. Ход исследования</h2>

<h3>4.1 Инвентаризация сплитов</h3>
<p>С 2026-05-01 на корневом шарде базчейна набралось <b>{tot} сплита before_split</b>, сгруппированных по близости во времени в <b>{nrun} событий</b> (2026-05-01…07-24; несколько подряд идущих сплитов = одно событие). Таблица по сплитам ниже (приложение 9.1) даёт по строке на КАЖДЫЙ сплит, а описание события объединено rowspan по всем его сплитам. <span class="src">(ClickHouse mainnet_d.blocks, wc=0 AND before_split; группировка по gap)</span></p>

<h3>4.2 Отпечаток нагрузки и таксономия типов</h3>
<p class="lead">По 5-мин окну каждого события снят отпечаток (TPS, газ, tx/acc, доля наших tx) и назначен тип нагрузки. Распределение по {nrun} событиям:</p>
<div class="tax">
  <div class="t"><div class="n" style="color:var(--lowtps)">{cnt['lowtps']}</div><div class="l">low-TPS fragile (16–49 TPS)</div></div>
  <div class="t"><div class="n" style="color:var(--broad)">{cnt['broad']}</div><div class="l">broad organic flood (май, 68–255 TPS)</div></div>
  <div class="t"><div class="n" style="color:var(--jetton)">{cnt['jetton']}</div><div class="l">our jetton tests (порог 200→330 TPS)</div></div>
  <div class="t"><div class="n" style="color:var(--compute)">{cnt['compute']}</div><div class="l">compute (1 контракт, ~1M газа/tx)</div></div>
  <div class="t"><div class="n" style="color:var(--poke)">1</div><div class="l">our big-acct poke (07-20 21:44)</div></div>
  <div class="t"><div class="n" style="color:var(--fanout)">1</div><div class="l">our pool destroy (1 трейс, 63% окна)</div></div>
</div>
<p class="src">(классификация по fp.tsv/gas.tsv: наш жетон = j_ours≥5000; compute = gas/s≥1M или avg≥30k; broad = TPS≥80 &amp; tx/acc&lt;1.7; иначе low-TPS)</p>
<p class="lead">Тот же расклад по <b>{tot} отдельным сплитам</b> (каждый before_split — своя строка в приложении 9.1): low-TPS <b>{epcnt['lowtps']}</b>, наш жетон <b>{epcnt['our-jetton']}</b>, broad <b>{epcnt['broad']}</b>, compute <b>{epcnt['compute']}</b>, наш поук <b>{epcnt['our-poke']}</b>, наш destroy <b>{epcnt['our-destroy']}</b>.</p>
<p>Полная таблица сплитов (тип-чип, TPS 5-мин + 1-мин пик, газ, вероятная причина, описание события, Grafana) — <b>приложение 9.1</b>. Ниже — легенда цветов, общая для таблицы.</p>

<div class="legend">
  <span><i class="dot" style="background:var(--lowtps)"></i>low-TPS</span>
  <span><i class="dot" style="background:var(--broad)"></i>organic flood</span>
  <span><i class="dot" style="background:var(--jetton)"></i>our jetton</span>
  <span><i class="dot" style="background:var(--compute)"></i>compute</span>
  <span><i class="dot" style="background:var(--poke)"></i>our poke</span>
  <span><i class="dot" style="background:var(--fanout)"></i>our destroy</span>
</div>

<h3>4.2b Что это была за нагрузка (органика по существу)</h3>
<p class="lead">Отдельно от механизма сплита — чем ФАКТИЧЕСКИ были органические всплески. Разобрано по составу транзакций (opcode, отправители/получатели, топ-жетоны) в ClickHouse:</p>
<div class="causes">
  <div class="cause ok"><div class="lt" style="--cc:var(--broad)">dust / airdrop fan-out</div><div class="dc">Крупные органические высоко-TPS сплиты (<b>05-19</b> 165–243 TPS, <b>05-26</b> 223, <b>05-27</b> 252 / 292-пик) — это <b>пыле-/эйрдроп-вееры</b>: ~8–12 highload-контрактов-раздатчиков, каждый выстреливает тысячи переводов по <b>1 наноТОН с пустым телом</b> (opcode <span class="mono">0x00</span>, near-zero bounce) на десятки тысяч свежих получателей. Паттерн «scam/airdrop spam» — тяжёлый по числу/байтам, тривиальный по газу (14–20% soft).</div></div>
  <div class="cause ok"><div class="lt" style="--cc:var(--compute)">single arb/DeFi bot</div><div class="dc">Compute-сплит <b>06-26</b> = <b>один торговый/арбитражный бот</b> (<span class="mono">0:3FC4…A56</span>): ~921 жетон-своп-экстерналов STON.fi-v2 по ~1M газа каждый → единственный газ-driven органический сплит (вместе с 06-24, тяжёлый DeFi-контракт). Оба пересекают 10M soft (<b>153 / 154%</b>).</div></div>
  <div class="cause ok"><div class="lt" style="--cc:var(--lowtps)">USDT + DEX + EVAA фон</div><div class="dc">Под почти каждым органическим сплитом — <b>USDT (Tether)</b> переводы + <b>DEX</b> (STON.fi / DeDust) + <b>EVAA</b> lending. Хрупкие low-TPS сплиты (<b>06-05 / 06-07 / 07-08 / 07-09</b>) — это тот же органический микс на <b>20–48 TPS</b> с тривиальным газом → семья <span class="mono">long_collation</span>.</div></div>
  <div class="cause maybe"><div class="lt" style="--cc:var(--jetton)">наши четыре прогона</div><div class="dc"><b>07-13</b> (жетон-флуд, 18 сплитов, каскад 2→4→8 шардов), <b>07-20 утро</b> (жетон), <b>07-20 21:44</b> (поук), <b>07-23</b> (destroy).</div></div>
</div>

<h3>4.3 Дрейф порога по периодам</h3>
<p class="lead">Наблюдаемый факт: TPS сплита сильно различался по неделям. Три периода как чистое наблюдение (период → диапазон TPS → доминирующая нагрузка); что именно менялось между ними, из блок-данных не следует. Все сплиты держались <b>110–296 с</b> (медиана 142 с) и мёржились обратно. <span class="src">(ClickHouse blocks: before_split→merge)</span></p>
<div class="timeline">
  <div class="band"><div class="when">10–29 мая<span>10 событий</span></div><div class="what"><div class="thr mid">наблюдалось ~68–255 TPS</div><p>Широкий органический флуд: десятки тысяч разных аккаунтов по ~1 tx (88.7% — opcode-0 простые переводы, массовые клеймы/минты). Дёшево по газу (avg ~1.5–3.4k). <span class="src">(ClickHouse messages: opcode, uacc)</span></p></div></div>
  <div class="band"><div class="when">05 июн – 10 июл<span>36 событий</span></div><div class="what"><div class="thr lo">наблюдалось ~16–49 TPS</div><p>Низк-TPS сплиты. Газ дороже (avg ~5–11k/tx), заметная доля жетон-трансферов. Плюс 2 аномалии (06-24, 06-26) — чистый compute-сплит по газу. <b>Наиболее вероятное объяснение периода:</b> триггер <span class="mono">long_collation</span> (PR #2290, написан 06.04) начал ДЕЙСТВОВАТЬ на майннете с обновления ноды 26→27.05 — до этой даты минимальный TPS сплита 67.9, после неё 52 из 54 органических сплитов ниже 50 TPS, при том что само условие выполнялось в мае с той же частотой (§4.7). <span class="src">(ClickHouse gas.tsv; ton 0d34f2d62; архив session-stats)</span></p></div></div>
  <div class="band"><div class="when">13–20 июл<span>наши тесты</span></div><div class="what"><div class="thr hi">порог сплита ~200 → ~330 TPS</div><p>Наши жетон-флуды (бинарный поиск порога). Порог реально скакнул <b>~200 → ~330 TPS</b> (×1.6): 13.07 сплит уже при 44 req/s (~200 TPS; 35 держали ~20 мин без сплита), 20.07 тот же 44 (и 54, и 64) НЕ сплитил — понадобилось 75 (~330 TPS). Пересчёт — §4.8. <span class="src">(наши прогоны + ClickHouse 30-с)</span></p></div></div>
</div>
<figure>
  {c_time}
  <figcaption><b>TPS при сплите во времени.</b> Каждая точка — событие, цвет — тип нагрузки. Амбер-пунктир — всплеск из <b>9 key-блоков 06-05 04:32–04:40</b> (8 мин): факт; при этом <b>ConfigParam 23 НЕ менялся</b> (подтверждено оператором), т.е. это не смена лимитов. <span class="src">(ClickHouse blocks: key_block; оператор)</span></figcaption>
</figure>

<h3>4.4 Поправка: ConfigParam 22 vs 23</h3>
<p>Первый разбор ошибочно взял <b>ConfigParam 22</b> (мастерчейн, gas soft ~1M) и заключил, что сплит «газо-ограничен» на 1M. Это неверно: базчейн (который и сплитится) берёт лимиты из <b>ConfigParam 23</b> — <span class="mono">mc-config.cpp:729</span>: <span class="mono">param = is_masterchain ? 22 : 23</span>. Правильный декод ConfigParam 23: bytes <b>256K / 1M / 2M</b>, gas <b>2M / 10M / 20M</b>, lt <b>1000 / 5000 / 10000</b> (underload / soft / hard). Ключевое: gas soft не 1M, а <b>10M</b> — это опровергает старый gas-bound вывод (полный декод — приложение 9.2). <span class="src">(декод getConfigParam 23; ton/ mc-config.cpp:729)</span></p>

<h3>4.5 Заполнение блока по осям vs config-23 soft</h3>
<p class="lead">Наложили max газ/блок по всем событиям на правильный soft (10M). Живые soft-значения ConfigParam 23:</p>
<div class="limits">
  <div class="limit"><div class="d">bytes · soft</div><div class="v">1 MB</div></div>
  <div class="limit gas"><div class="d">gas · soft</div><div class="v">10 000 000</div></div>
  <div class="limit"><div class="d">lt_delta · soft</div><div class="v">5 000</div></div>
</div>
<figure>
  {c_gas}
  <figcaption><b>Газ на блок против soft (10M, ConfigParam 23).</b> Soft пересекают ТОЛЬКО compute-аномалии (06-24 / 06-26, до <b>15.3M = 153%</b>); у всех остальных событий пик <b>≤82% soft</b> → их триггер другой (очередь / long_collation / байты). График показывает лишь газ; остальные оси установлены по session-stats (§4.7). <span class="src">(maxgas.tsv vs 10M)</span></figcaption>
</figure>

<h3>4.6 Механизм из кода ноды</h3>
<p class="lead">Коллатор шарда решает сплитить в <span class="mono">Collator::check_block_overload()</span> (<span class="mono">collator.cpp:5439–5480</span>): ведёт взвешенную историю перегруза за <b>48 блоков</b> и, когда она переходит ноль, ставит <span class="mono">want_split</span>. Бит перегруза блока ставит <b>любой из четырёх</b> триггеров: <span class="src">(ton/ collator.cpp)</span></p>
<div class="triggers">
  {_triggers_html}
</div>
<div class="note">Per-tx лимит газа (<b>ConfigParam 21</b>, ~1M) ограничивает ОДНУ транзакцию — сплит он <b>не</b> триггерит. <span class="src">(ton/ collator.cpp; ConfigParam 21)</span></div>

<h3>4.7 Атрибуция причин по session-stats</h3>
<p class="lead">Причины перепроверены с нуля по архиву session-stats валидаторов (devnet-log, 2026-07-25): <b>1 730</b> блоков, сколлаченных валидаторами на нужных шардах в окнах <span class="mono">[T−300, T]</span>, каждое из 4 условий коллатора проверено дословно. Покрытие есть у <b>64 из 84</b> сплитов; для остальных триггер честно помечен <span class="mono">unknown</span> или выведен по исключению. Уровень уверенности: <span style="color:var(--confirmed)">подтверждено</span> · <span style="color:var(--accent)">вероятно</span> · <span style="color:var(--lowtps)">не подтвердилось</span>.</p>
<div class="causes">
  {_causes_html}
</div>
<h4>4.7b На что уходит время коллации: экстерналы и storage-словари</h4>
<p class="lead">Исходная гипотеза, унаследованная от нашего поук-теста: коллация тормозит, потому что коллатор обходит дерево ячеек аккаунтов с огромным стейтом. Проверка — на органических low-TPS сплитах, с <b>внутриоконным контролем</b>: LC-блоки против обычных блоков ТОГО ЖЕ окна (иначе «в блоке был большой аккаунт» ничего не значит — USDT и роутеры DEX есть почти везде). Разбор фаз коллации из <span class="mono">work_time_real_stats</span> и счётчиков <span class="mono">storage_stat_cache</span>. Выборка: <b>31</b> low-TPS событие, <b>1 049</b> покрытых архивом блоков — <b>195</b> LC против <b>854</b> контрольных, включая пустые (<span class="mono">check_block_overload</span> выполняется на КАЖДОМ сколлаченном блоке, а пустых среди LC-блоков <b>88 из 195</b>, поэтому исключать их нельзя). <span class="src">(архив session-stats: 195 LC vs 854 контрольных)</span></p>
<div class="note">
  <b>Поправки счёта: две поправки метода, отменяющие прежний разбор.</b>
  <p style="margin:8px 0 0"><b>(1) Метка <span class="mono">INBOUND_*_MESSAGES: timeout</span> не является признаком механизма.</b>
  Дедлайн у коллации один на весь блок (<span class="mono">max(wait_externals_until, soft_timeout)</span> = начало слота),
  а проверка стоит в начале цикла — поэтому метка фиксирует, ГДЕ кончился бюджет, а не НА ЧТО он ушёл; внутри LC-блоков
  она почти тавтологична. Прежнее деление LC-блоков на «CellDB» и «экстерналы» по этой метке снято.</p>
  <p style="margin:8px 0 0"><b>(2) Объём обхода = <span class="mono">miss_cells + small_cells</span>.</b> Кэш storage-статистики
  не хранит аккаунты меньше <span class="mono">MIN_ACCOUNT_CELLS = 4000</span>
  (<span class="mono">validator/storage-stat-cache.hpp:65</span>), но обходит их так же полно; аккаунты меньше 26 ячеек
  (<span class="mono">acc_state_cells_for_storage_dict</span>) счётчикам не видны вовсе. Прежний счёт только по
  <span class="mono">miss_cells</span> давал ложный вывод: у <b>53 блоков из 91</b> <span class="mono">miss_cells = 0</span>
  при медиане <span class="mono">small_cells</span> <b>16 627</b> — «нулевой обход» был артефактом порога кэша. Из-за него
  первая версия разбора получала картину «два независимых механизма»: обхода у большинства LC-блоков как бы нет,
  а время съедают экстерналы отдельно от storage. Третья поправка — фильтр «только закоммиченные блоки», он выбрасывает
  почти все блоки с большим обходом (разбор ниже, врезка «фильтр «только закоммиченные» смещает выборку»).</p>
  <p style="margin:12px 0 6px"><b>LC-блоки против контрольных блоков тех же окон</b> (медианы по блоку):</p>
  <div class="tscroll"><table class="mini">
    <thead><tr><th></th><th class="num">предложено ext</th><th class="num">принято ext</th><th class="num">отклонено ext</th><th class="num">аккаунтов инстанцировано</th><th class="num">обход, ячеек</th><th class="num">prelim/total</th></tr></thead>
    <tbody>
      <tr class="thr-row"><td>LC-блоки (195)</td><td class="mono num">55</td><td class="mono num">0</td><td class="mono num">54</td><td class="mono num">19</td><td class="mono num">16 660</td><td class="mono num">97%</td></tr>
      <tr><td>контроль (854)</td><td class="mono num">17</td><td class="mono num">3</td><td class="mono num">14</td><td class="mono num">5</td><td class="mono num">670</td><td class="mono num">17%</td></tr>
    </tbody>
  </table></div>
  <p style="margin:10px 0 0">Дополнительно: обход <b>на транзакцию</b> — 1 826 против 111; real/cpu на фазе
  <span class="mono">prelim</span> — <b>1.77</b> против <b>1.04</b>, то есть у LC-блоков процессор ждёт загрузку ячеек, а у
  контрольных ожидания нет. TVM при этом занимает медианные 0.5–1.8%: исполнение контрактов ни при чём.</p>
</div>
<p>Показатели складываются в <b>одну цепочку</b>, а не в противопоставление «CellDB против экстерналов». Порядок вызовов взят из кода: <span class="mono">process_external_message → create_ordinary_transaction → make_account → init_account_storage_dict</span>, и именно внутри последнего тикает <span class="mono">prelim_storage_stat</span>. Поток предлагаемых коллатору экстерналов заставляет инстанцировать аккаунт получателя на каждый из них; storage-словарь при этом строится заново, потому что кэш не держит аккаунты меньше 4000 ячеек; бюджет коллации на этом исчерпывается. По времени это <b>перестроение storage-словарей</b> (97% фазы), по причине — <b>поток экстерналов</b>. Отклонение экстернала выясняется только ПОСЛЕ попытки создать транзакцию, поэтому отвергнутые стоят столько же, сколько принятые: коллатору предлагают <b>11–24</b> экстернала на блок, отклоняется <b>95–99%</b>, и уровень отказов одинаков и в мае, и в июне-июле.</p>
<div class="note">
  <b>Корреляции по всем 1 049 покрытым блокам.</b> Отклонённые экстерналы ↔ обход ячеек <b>r = +0.60</b>;
  число инстанцированных аккаунтов ↔ обход <b>+0.51</b>; обход ↔ время фазы <span class="mono">prelim</span> <b>+0.57</b>;
  принятые экстерналы ↔ обход <b>+0.32</b>. Это согласуется с цепочкой выше, но сама причинность из корреляций не следует —
  порядок вызовов взят из кода, поблочной атрибуции «сколько аккаунтов пришло именно от отклонённых экстерналов» нет.
  <p style="margin:12px 0 6px"><b>Доля LC-блоков по децилям объёма обхода — монотонный рост:</b></p>
  <div class="tscroll"><table class="mini">
    <thead><tr><th>обход, ячеек</th><th class="num">0</th><th class="num">27–82</th><th class="num">85–202</th><th class="num">203–595</th><th class="num">602–4.2k</th><th class="num">4.2–8.7k</th><th class="num">8.7–14.5k</th><th class="num">14.5–20k</th><th class="num">20–24k</th><th class="num">24–81k</th></tr></thead>
    <tbody><tr><td>доля LC</td><td class="mono num">0%</td><td class="mono num">0%</td><td class="mono num">0%</td><td class="mono num">0%</td><td class="mono num">4%</td><td class="mono num">11%</td><td class="mono num">15%</td><td class="mono num">19%</td><td class="mono num">18%</td><td class="mono num hi">52%</td></tr></tbody>
  </table></div>
</div>
<p><b>Обходятся рядовые аккаунты, а не «большие».</b> Поаккаунтный разрез по <b>195</b> LC-блокам <b>31</b> low-TPS события: медиана на блок — <b>19 мелких</b> аккаунтов (26–3999 ячеек, которые кэш не хранит) и <b>0 крупных</b> (≥4000 ячеек); <b>в 62% LC-блоков крупного аккаунта нет вообще</b> (<span class="mono">miss_cnt = 0</span>); средний обойдённый мелкий аккаунт — <b>719</b> ячеек (медиана). По суммарному объёму обхода мелкие дают <b>69%</b>, крупные — <b>30%</b>. То есть массовый механизм — это обход примерно двух десятков рядовых аккаунтов на блок, каждый из которых кэш отказывается хранить из-за порога <span class="mono">MIN_ACCOUNT_CELLS = 4000</span>, а вовсе не один раздутый аккаунт. Соответственно и направление возможного исправления — <b>порог кэширования</b> и <b>поток отвергаемых экстерналов</b>, порождающий инстанцирование этих аккаунтов, а не «ограничить большие аккаунты».</p>
<div class="causes">
  <div class="cause ok"><div class="lt" style="--cc:var(--confirmed)">редкий крайний случай: один крупный аккаунт</div><div class="dc">Отдельный режим — обход <span class="mono">miss_cells ≥ 15 тыс.</span> на ОДИН аккаунт. Он редок — <b>в 62% LC-блоков крупного аккаунта нет вообще</b> (<span class="mono">miss_cnt = 0</span>), а на все крупные приходится <b>30%</b> суммарного объёма обхода против <b>69%</b> у мелких, — но даёт все блоки дольше секунды, вплоть до 6.4 с. Естественный эксперимент на соседних блоках: <span class="mono">77185054</span> обошёл <b>49 723</b> ячейки (<span class="mono">miss_cnt=1</span> — ровно ОДИН аккаунт), 97% коллации в <span class="mono">prelim_storage_stat</span>, <b>1.011 с</b>; следующий блок <span class="mono">77185055</span> отчитывается о <b>49 724 ячейках ПОПАДАНИЯ</b> в кэш, и та же фаза падает до <b>0.018 с — в 56 раз</b>. Виновники опознаны: <span class="mono">0:78724B16…A166</span>, Highload Wallet V2 с насыщенным словарём <span class="mono">old_queries</span> (50 163 ячейки; обход рос <b>38 185 → 48 971 → 49 796</b> за три дня), и <span class="mono">0:6396D51D…F8A</span>, накопитель поинтов на 20 681 ячейку. К массовой картине (медианный LC-блок: 19 инстанцированных аккаунтов, 16.7 тыс. ячеек) этот режим не сводится. Размеры взяты из сегодняшнего стейта (исторические состояния недоступны: <span class="mono">account_states</span> пуста, <span class="mono">data_boc</span> — NULL у всех 194 млн строк), и для таких аккаунтов это допустимо: большой стейт меняется медленно, поэтому сегодняшний размер совпадает с числом обойдённых тогда ячеек с точностью <b>1%</b>.</div></div>
  <div class="cause ok"><div class="lt" style="--cc:var(--accent)">решает промах кэша, а не размер</div><div class="dc">Присутствие большого аккаунта <b>само по себе не тормозит</b>. Из 12 блоков архива, тронувших эти три раздутых аккаунта, у 11 был промах кэша (медиана коллации <b>1.04 с</b>, prelim 96%), а единственный с тёплым кэшем — <b>0.415 с</b> и prelim 0%. Кэш storage-статистики свой у каждой ноды и прогревается первым же обходом — поэтому один и тот же аккаунт у одного валидатора рвёт блок, а у другого проходит незаметно.</div></div>
  <div class="cause maybe"><div class="lt" style="--cc:var(--accent)">фильтр «только закоммиченные» смещает выборку</div><div class="dc">Сверка каждого блока архива с закоммиченным в ClickHouse: <b>проигравшими кандидатами</b> (коллация опоздала, выиграл блок другого валидатора) оказались <b>17 из 107</b> LC-блоков (<b>16%</b>) против <b>0 из 763</b> не-LC, и у ВСЕХ 17 обход <b>16 757–50 568</b> ячеек. Бит проигравшего кандидата в цепочку не попадает: <span class="mono">overload_history_</span> живёт в СОСТОЯНИИ шарда — берётся из предыдущего блока (<span class="mono">collator.cpp:1286</span>) и проверяется как <span class="mono">ns_ == ps_ &lt;&lt; 1</span> (<span class="mono">validate-query.cpp:6772</span>). Но для описания механизма это значит другое, чем считалось: фильтр по закоммиченным выбрасывает почти все блоки с большим обходом, поэтому прежняя оценка «обход даёт 10% бит» была артефактом фильтра — по всем LC-блокам доля таких блоков <b>23%</b>.</div></div>
  <div class="cause unknown"><div class="lt" style="--cc:var(--lowtps)">главная оговорка: плотность не подтверждена</div><div class="dc"><span class="mono">want_split</span> требует минимум <b>24 перегруженных блока</b> — если это 16 самых свежих (вес 3) плюс 8 следующих (вес 2); при равномерном распределении по регистру из 48 блоков нужна доля <b>≥ 2/3</b>, то есть 32 из 48. Наблюдаемая плотность LC-блоков этого НЕ достигает: по 31 low-TPS событию LC составляют <b>18%</b> покрытых блоков; ни один из <b>37</b> сплитов с покрытием ≥8 блоков не даёт ≥2/3 по окну; в лучшем 25-секундном отрезке ≥2/3 достигают <b>5 сплитов из 37</b>, медиана лучшего отрезка <b>0%</b>. Два объяснения, различить которые имеющимися данными нельзя: либо архив недобирает короткий всплеск перегруза (видно 5–8 блоков из каждых ~50, всплеск длится 20–40 с), либо в большинстве сплитов <span class="mono">want_split</span> выставил не <span class="mono">long_collation</span>. Отсюда формулировка: <b><span class="mono">long_collation</span> — единственный уцелевший кандидат по исключению, его сигнатура присутствует, но требуемая плотность не подтверждена.</b> Плюс: у <b>17 из 60</b> <span class="mono">long_collation</span>-сплитов в окне нет ни одного закоммиченного LC-блока — там вердикт экстраполирован.</div></div>
</div>
<h4>Три показательных механизма (детально)</h4>
<div class="dives">
  <div class="dive d-compute">
    <div class="k">gas (блок &gt;10M soft)</div>
    <h3>Один тяжёлый контракт рвёт шард при 20–46 TPS</h3>
    <p>06-24 и 06-26: единственный контракт под потолком per-tx газа (~1M/tx, ConfigParam 21) даёт 90–97% газа окна. Пик блока <b>15.3M &gt; 10M soft</b> (ConfigParam 23) → это ГАЗ, подтверждено. <span class="src">(session-stats: block_full gas 15 077 650, cat_gas=3)</span></p>
    <div class="fig">
      <div>06-24 16:59 · <b>651M / 724M</b> газа (90%) · один аккаунт</div>
      <div>06-26 05:37 · <b>921M / 954M</b> газа (97%) · avg 997k/tx</div>
      <div>пик блока <b>15.3M / 10M soft (153%)</b> — единственный тип, что пересёк soft</div>
    </div>
    <div class="links"><a href="{tvc1}" target="_blank" rel="noopener">tx 06-24 ↗</a><a href="{tvc2}" target="_blank" rel="noopener">tx 06-26 ↗</a><a href="{gc1}" target="_blank" rel="noopener">Grafana 06-24 ↗</a><a href="{gc2}" target="_blank" rel="noopener">Grafana 06-26 ↗</a></div>
  </div>
  <div class="dive d-poke">
    <div class="k">long_collation — по исключению (покрытия архива нет)</div>
    <h3>Сплит при 70 TPS против ~330 в тот же день</h3>
    <p>Пул отправил ровно 100 поуков (value=1, пустое тело) в 100 аккаунтов по 37–63 КБ. Газ блока лишь 27% soft → не газ; load-class исключён точно (<span class="mono">hw = −64</span> по всем трём измеримым осям). Причина — <b>long_collation</b>, но <b>по исключению</b>: в окне <span class="mono">[T−300, T]</span> на шарде <span class="mono">8000</span> архив не покрыл ни одного блока (2 из 11 хостов писали лог, и оба коллатили мастерчейн). Косвенное: 3 из 4 первых блоков сразу после сплита (T+13…T+15) удовлетворяют условию long_collation. <b>Прежнее «ctt 4.75с на 1 tx» снято</b> — такого блока в окне поука нет; ближайшее совпадение по всему трёхмесячному набору — <b>4.738с 01.07 13:59:40</b>, другой день и другое событие. Механизм (CellDB медленно грузит большие стейты) остаётся правдоподобным, но прямого наблюдения нет. <span class="src">(session-stats 20.07, sudo zcat, wc=0)</span></p>
    <div class="fig">
      <div>07-20 09:51 жетон-флуд → сплит при <b>339 TPS</b> (1-мин пик; 268 5-мин ср. — прямое измерение)</div>
      <div>07-20 21:44 поук (~0.3/с) → сплит при <b>70 TPS</b> (5-мин; 91 1-мин пик)</div>
      <div>покрытия архива в окне <b>нет</b> · после сплита 3/4 блока long_collation · газ 27% soft</div>
    </div>
    <div class="links"><a href="{tvp}" target="_blank" rel="noopener">tx поука ↗</a><a href="{gp1}" target="_blank" rel="noopener">Grafana поук ↗</a><a href="{gp2}" target="_blank" rel="noopener">Grafana жетон ↗</a></div>
  </div>
  <div class="dive d-destroy">
    <div class="k">unknown — архива за 23.07 нет</div>
    <h3>Teardown 5001 кошелька — один трейс рвёт шард при 79 TPS</h3>
    <p>07-23 10:00: корень трейса = наш collector (<span class="mono">UQC4lxeD…</span>, совпало побайтово). Все 5001 jetton-кошельков свипнули GRAM на него — 5002 входящих, одна линейная цепочка 15 004 tx. Триггер <b>определить нельзя</b>: сбор session-stats за 23.07 обрывается в <b>01:27 UTC</b>, за окно сплита архива нет вообще. Из ClickHouse точно исключается только load-class (<span class="mono">hw = −64</span> по газу, lt и байтам); очередь и время коллации в блок-данных не видны. Прежняя версия «вероятно ОЧЕРЕДЬ» опиралась на прокси, который перепроверка признала недостоверным. <span class="src">(ClickHouse трейс 07-23; архив session-stats пуст)</span></p>
    <div class="fig">
      <div>трейс: <b>15 004 tx</b> из 23 739 (63%)</div>
      <div>триггер <b>неизвестен</b>: архива нет · load-class исключён · газ 62% soft</div>
      <div>симметрично deploy → тоже способен сплитить</div>
    </div>
    <div class="links"><a href="{tvd}" target="_blank" rel="noopener">трейс destroy ↗</a><a href="{gd1}" target="_blank" rel="noopener">Grafana destroy ↗</a></div>
  </div>
</div>

<h3>4.8 Точный пересчёт порога наших жетон-флудов: 200 → 330 TPS</h3>
<p class="lead">«TPS 5m» в таблице 9.1 — 5-мин среднее по всем tx базчейна: оно размывает бинарный поиск (freq рос внутри окна) и занижает мгновенный порог — поэтому в таблицу теперь добавлена колонка <b>TPS 1m peak</b> (1-мин пик), чтобы ramp не разбавлялся. Ключевой факт: сплит <b>20.07 09:51</b> теперь <b>измерен прямо в момент сплита</b> — <b>268 TPS (5-мин ср.) / 339 TPS (1-мин пик)</b>, топ-жетон окна = наш пул <span class="mono">0:A029…DEC1</span> с <b>17 523 переводами 5000→4850</b>. Против 13.07 (~200 TPS) это скачок <b>≈ ×1.6, подтверждённый на самом моменте сплита</b> (раньше это был вывод через пробел покрытия session-stats). Точные пороги по 30-с разрешению (наш rate = переводы/с минтера, ×4 = наши tx/с; total = наши + органика): <span class="src">(ClickHouse 30-с TPS + лог прогонов)</span></p>
<div class="thresh-grid">
  <div class="thresh-card">
    <h4>13.07.2026 — порог ≈ 200 TPS</h4>
    {FREQ_1307}
  </div>
  <div class="thresh-card">
    <h4>20.07.2026 — порог ≈ 330 TPS</h4>
    {FREQ_2007}
  </div>
</div>
<div class="note"><b>Порог поднялся ~1.6× — это не шум.</b> Тот же 44 req/s (~210 TPS), что сплитил 13.07, на 20.07 не сплитил (как и 54/64) — понадобилось 75. <b>Прежнее объяснение «оба теста queue-driven, сеть стала быстрее дренировать очередь» перепроверку не прошло:</b> полный проход по архиву показал, что корневой шард <span class="mono">8000</span> 13.07 сплитился по <span class="mono">long_collation</span> (13/14 и 17/25 непустых блоков), а его очередь не превышала <b>684</b> при пороге 4096. Очередь ≥4096 появляется только с 09:18:06, двумя уровнями каскада ниже. Значит дрейф порога — по-прежнему сетевой/валидаторный эффект, но по каналу <b>времени коллации</b>, и чем именно она ускорилась между датами, из имеющихся данных не следует. <span class="src">(session-stats 13.07, sudo, все 11 хостов; collator.cpp:51)</span></div>
<div class="note"><b>Проверка по session-stats.</b> Вопрос порога 200→330 разобран прямо в архиве session-stats:
<ul>
  <li>Альтернатива оператора — что случайное фоновое касание большого аккаунта (→ long_collation) помогло сплиту 13.07 при ~200 TPS — проверена и <b>ОПРОВЕРГНУТА</b>: в обоих окнах 13.07 <b>ноль long_collation</b>. <span class="src">(session-stats devnet-log, 13.07, wc=0)</span></li>
  <li><b>Поправка адресации:</b> очередь <span class="mono">5277 &gt; 4096</span>, которой раньше объясняли сплит <b>09:11:56</b>, на самом деле относится к <b>09:19:26 на шарде <span class="mono">2000</span></b> — на два уровня каскада глубже. В окне 09:11:56 очередь не превышала <b>14</b>. Всего пересечений 4096 в датасете <b>70 блоков</b>, все — 13.07 после 09:18:06, на дочерних шардах. <span class="src">(session-stats 13.07, 1 489 блоков)</span></li>
  <li><b>13.07 08:46 (тот самый ~200-TPS порог) — маргинальный/флаппи флуд-сплит:</b> bytes-full от блоков ~363 tx (<span class="mono">block_full bytes 1 066 356</span>, cat_bytes=2) + очередь лезет к ~330 и дренируется — не long_collation и не чистый 4096. <span class="src">(session-stats 13.07, wc=0)</span></li>
  <li><b>Наблюдение о дренаже остаётся, объяснением быть перестало:</b> 20.07 действительно несла ~413 tx/блок при <span class="mono">out_msg_queue ≤106</span> против ~363 tx/блок при 313–329 на 13.07. Но раз корневые сплиты обоих прогонов шли не по очереди, эта разница описывает состояние сети, а не причину сплита. <span class="src">(session-stats 20.07 &amp; 13.07, wc=0)</span></li>
  <li><b>Что теперь прямо, а что нет:</b> <b>TPS сплита 20.07 09:51 измерен прямо</b> — <b>268 / 339 TPS</b> (5-мин / 1-мин пик, ClickHouse). А вот триггер этого сплита теперь честно <span class="mono">unknown</span>: момент попал в пробел покрытия архива (11 из ~350 валидаторов; на шарде <span class="mono">8000</span> нет ни одного покрытого блока в <span class="mono">[T−300, T]</span>). Точно исключается только load-class (<span class="mono">hw = −64</span> по газу, lt и байтам). Прежний вывод «очередь дошла до 4096» снят: он строился на до-пробельном дренаже и на прокси очереди из ClickHouse, который оказался недостоверным. <span class="src">(ClickHouse 30-с; session-stats coverage gap)</span></li>
</ul></div>

<h2>5. Гипотезы и проверки</h2>
<p class="lead">Каждая строка — одна гипотеза: как проверяли, результат, где данные. Таблица расширяемая — новые гипотезы добавляются строками.</p>
<div class="tscroll">
<table class="hyptable">
  <thead><tr><th>#</th><th>гипотеза</th><th>как проверяли</th><th>результат</th><th>где данные</th></tr></thead>
  <tbody>
{_hyp_rows}
  </tbody>
</table>
</div>

<h2>6. Инструментарий, добавленный по ходу</h2>
<p><b>PR #2488</b> (<span class="mono">ton-blockchain/ton</span>, ветка testnet) — добавляет в session-stats поля <span class="mono">overload_reason</span> / <span class="mono">want_split</span> / <span class="mono">peak_block_limit_class</span>, которые пишутся на КАЖДОМ сколлаченном блоке; <span class="mono">overload_reason ≠ 0</span> только там, где взведён бит перегруза. Собран начисто. Это снимет главную неопределённость: атрибуцию причины больше не придётся реконструировать по <span class="mono">block_full</span>/<span class="mono">limits_log</span> — она будет прямо в поле. <span class="src">(ton-blockchain/ton PR #2488)</span></p>

<h2>7. Выводы</h2>
<p class="lead">Сформулированы после полной перепроверки: {tot} сплитов, {nrun} событий, 1 730 сколлаченных блоков из архива session-stats, каждое из четырёх условий коллатора проверено дословно.</p>

<h3>7.1 Что раскалывало базчейн</h3>
<ul>
  <li><b>Наша доля — 4 события из {nrun} (8%).</b> Остальные 46: <b>2</b> compute-аномалии, <b>13</b> широкого органического флуда (пик 43–252 TPS) и <b>31</b> при 16–48 TPS.</li>
  <li><b>Три из четырёх триггеров исключены счётом.</b> <span class="mono">load_class</span> достижим по взвешенной истории ровно в <b>2 сплитах из {tot}</b> (24.06 и 26.06, оба газовые); <span class="mono">queue ≥ 4096</span> встречается только на дочерних шардах нашего жетон-флуда 13.07 и <b>ни разу</b> в органике за 3 месяца; <span class="mono">dispatch</span> не сработал ни разу на 1 730 блоках. Остаётся <span class="mono">long_collation</span>.</li>
  <li><b>Метка <span class="mono">long_collation</span> означает не «блок собирался аномально долго».</b> У блоков без неё <span class="mono">check_load_total_time</span> практически такая же (0.394 с против 0.378 с), но <span class="mono">work_time</span> — 0.035 с против 0.378 с. Эти ~0.4 с суть фиксированный бюджет ожидания экстерналов, и метка говорит: коллатор потратил бюджет на работу вместо ожидания.</li>
</ul>

<h3>7.2 Куда уходило время у 31 low-TPS события</h3>
<div class="note">
  <b>Основной вывод.</b> По имеющимся данным low-TPS сплиты объясняются стоимостью <b>перестроения storage-словарей аккаунтов</b> — обходом деревьев ячеек, ожидающая часть которого приходится на чтения CellDB. <b>Не большими аккаунтами.</b>
  <p style="margin:10px 0 0">Внутриоконный контроль (LC-блоки против не-LC блоков ТЕХ ЖЕ окон): storage-фазы <b>97%</b> времени коллации против <b>17%</b>; объём обхода <b>16 660</b> ячеек против <b>670</b>, на транзакцию <b>1 826</b> против <b>111</b>; <span class="mono">real/cpu</span> на фазе <b>1.77</b> против <b>1.04</b> — процессор ждёт загрузку ячеек, у контрольных блоков ожидания нет. Доля LC растёт монотонно с объёмом обхода: 0% в нижних четырёх децилях → <b>52%</b> в верхнем.</p>
  <p style="margin:10px 0 0">Обходятся <b>рядовые</b> аккаунты: медиана <b>19</b> мелких (26–3999 ячеек) и <b>0</b> крупных на блок, в <b>62%</b> LC-блоков крупного нет вообще, средний обойдённый — <b>719</b> ячеек, на мелкие приходится <b>69%</b> объёма. Порог кэша <span class="mono">MIN_ACCOUNT_CELLS = 4000</span> гарантирует, что этот класс не кэшируется никогда. Крупные аккаунты дают оставшиеся 30% и все блоки дольше секунды — редкий режим, а не причина класса.</p>
</div>
<p><b>Работу порождает поток экстерналов.</b> Коллатору предлагают 11–24 экстернала на блок, он отклоняет <b>95–99%</b>, но отклонение выясняется только после попытки создать транзакцию — то есть на каждый мусорный экстернал грузится аккаунт получателя и перестраивается его словарь. У LC-блоков предложено <b>55</b> экстерналов при <b>нуле</b> принятых против 17/3 у контрольных; корреляция отклонённых экстерналов с объёмом обхода <b>r = +0.60</b>. Цепочка подтверждается кодом: <span class="mono">process_external_message → create_ordinary_transaction → make_account → init_account_storage_dict</span>. Это <b>не</b> «CellDB против экстерналов» — это вход и выход одного механизма.</p>

<h3>7.3 Почему класс появился в конце мая</h3>
<p>Все 31 событие произошли после обновления ноды <b>26 → 27 мая</b>. Триггер <span class="mono">long_collation</span> написан раньше, 06.04 (PR #2290), а действовать на майннете начал с этого обновления. Одной и той же формулой условие выполнялось в мае у 10–41% блоков, после — у 4–28%, то есть с сопоставимой частотой; при этом в мае нет ни одного сплита ниже 68 TPS, а после — 31 событие при 16–48. <b>Изменилась не нагрузка, а набор условий, на которые реагирует нода.</b> Дорогие экстерналы были фоном всё время.</p>

<h3>7.4 Что из этого следует для исправления</h3>
<ul>
  <li><b>Со стороны входа</b> — лимиты и дроп заведомо необрабатываемых экстерналов плюс параллельная проверка перед добавлением в мемпул (PR #2470, релиз v2026.06 от 15.07). В наше окно наблюдений он ещё не действовал, поэтому проверить эффект по этим данным нельзя.</li>
  <li><b>Со стороны выхода</b> — порог <span class="mono">MIN_ACCOUNT_CELLS = 4000</span>, из-за которого аккаунты в 26–3999 ячеек не кэшируются никогда и перестраиваются при каждом касании. Именно этот класс даёт 69% объёма обхода.</li>
  <li>Ограничения на размер стейта проблему <b>не</b> снимут: в 62% LC-блоков крупного аккаунта нет вовсе.</li>
</ul>

<h3>7.5 Граница доказанного</h3>
<p>Вывод формулируется как «<span class="mono">long_collation</span> — единственный уцелевший кандидат по исключению, его сигнатура присутствует», а не как замкнутая цепочка. Незакрытым остаётся одно звено: <b>наблюдаемая плотность LC-блоков не достигает порога, который требует <span class="mono">want_split</span></b> (18% покрытых блоков против нужных ≥2/3; подробно — §4.7b и §8). Либо архив не видит короткий всплеск перегруза, либо в части сплитов бит выставил не он; различить эти объяснения имеющимся покрытием нельзя.</p>

<h2>8. Открытые вопросы / что доисследовать</h2>
<ul>
  <li>Точная перепроверка по сплитам всех <b>{nrun} событий / {tot} сплитов</b> по session-stats с новым полем <span class="mono">overload_reason</span> (PR #2488) — вместо ручной реконструкции по осям.</li>
  <li><b>Плотность перегруза не подтверждена (главная оговорка §4.7b).</b> <span class="mono">want_split</span> требует перегруза минимум <b>24 перегруженных блока</b> — если это 16 самых свежих (вес 3) плюс 8 следующих (вес 2); при равномерном распределении по всему регистру из 48 блоков нужна доля <b>≥ 2/3</b>, то есть 32 из 48, а наблюдаемая доля LC-блоков — 18% покрытых; ни один из 37 сплитов с покрытием ≥8 блоков не даёт ≥2/3 по окну. Либо архив недобирает короткий всплеск (видно 5–8 блоков из ~50 при длительности всплеска 20–40 с), либо в большинстве сплитов <span class="mono">want_split</span> выставил не <span class="mono">long_collation</span>; различить это можно только более плотным покрытием или полем <span class="mono">overload_reason</span> из PR #2488. Сюда же — <b>17 из 60</b> long_collation-сплитов без единого закоммиченного LC-блока в окне.</li>
  <li>Воспроизводимость поук-сплита: было 2 прогона — 1 сплит, 1 нет. Что определяет успех (какие именно аккаунты, их горячесть в CellDB, тайминг)?</li>
  <li><b>Почему порог скакнул 200→330 — ОТКРЫТО и переформулировано</b> (§4.8): «очередь дренировалась быстрее» больше не объяснение, потому что корневые сплиты 13.07 шли по <span class="mono">long_collation</span>, а не по очереди. Новый вопрос: <b>что между 13.07 и 20.07 ускорило коллацию блока</b> (версия ноды, диск/CellDB, состав валидаторов). Для 20.07 09:51 нужны session-stats валидатора, сколлатившего <span class="mono">mc 80871701–80875501</span> (в текущем архиве их нет), либо воспроизведение с полем <span class="mono">overload_reason</span> из PR #2488.</li>
  <li>Проверить причины для остальных дней архива (сейчас разобраны экземпляры 26.05, 05.06, 06-24/26, 07-13/20/23; прочие low-TPS/broad ряды — по экстраполяции).</li>
  <li>Восстановить историческое ConfigParam 23 за май/июнь, если возможно (сейчас доступно только текущее значение).</li>
</ul>

<h2>9. Приложения</h2>

<h3>9.1 Полная таблица сплитов ({tot} сплита / {nrun} событий)</h3>
<p class="lead">Одна строка = один сплит <span class="mono">before_split</span> (окно = 5 мин до сплита). Колонка <b>TPS 5m</b> — 5-мин среднее (для наших жетон-прогонов занижает порог, см. §4.8); <b>TPS 1m peak</b> — 1-мин пик (более резкая метрика, не разбавляет ramp). <b>max gas/blk (%soft)</b> — абсолютный пик газа/блок и его доля от 10M soft (красным при ≥100% = compute). <b>trigger</b> — какой из <b>4</b> триггеров коллатора сработал, проверено дословно по архиву session-stats (<span class="mono">load_class</span> с осью <span class="mono">:gas</span>/<span class="mono">:cdata</span>/<span class="mono">:bytes</span>, <span class="mono">queue</span>, <span class="mono">long_collation</span>, <span class="mono">dispatch</span>; §4.7). Знак <span class="mono">?</span> и приглушённый цвет = вывод по исключению, без прямого наблюдения (нет покрытия архива на этом шарде); <span class="mono">+что-то</span> — второй сработавший триггер. Наведи курсор — в подсказке цифры-основания. <b>description</b> — что именно была за нагрузка в этом событии (объединено rowspan по всем сплитам события). <span class="mono">↗</span> — Grafana с окном сплита. Строки поука и destroy подсвечены, наши жетон-сплиты — слегка. <span class="src">(ClickHouse mainnet_d + session-stats)</span></p>
<div class="tscroll">
<table>
  <thead><tr>
    <th>UTC mon-day hh:mm</th><th>shard</th><th>type</th><th>TPS 5m</th><th>TPS 1m peak</th><th>avg gas/tx</th><th>max gas/blk (%soft)</th><th>tx/blk</th><th>trigger</th><th>description</th><th>graf</th>
  </tr></thead>
  <tbody>
{tablerows}
  </tbody>
</table>
</div>
<div class="note"><b>{tot} сплита before_split → {nrun} событий.</b> Таблица теперь по сплитам: по строке на каждый <span class="mono">before_split</span> (со своими TPS/газом), а описание события объединено <span class="mono">rowspan</span> по всем его сплитам (близкие сплиты = одно событие). Сумма сплитов по всем событиям = {tot}. <span class="src">(ClickHouse blocks, группировка по gap)</span></div>

<h3>9.2 Декод ConfigParam 22/23</h3>
<p><b>ConfigParam 22</b> — мастерчейн (is_masterchain), НЕ сплитится; первый разбор ошибочно взял его gas soft ~1M. <b>ConfigParam 23</b> — базчейн (актуальные live-значения):</p>
{CONFIG_DECODE}
<p class="src">(getConfigParam 23; ton/ mc-config.cpp:729 param = is_masterchain ? 22 : 23)</p>

<h3>9.3 Схема полей session-stats</h3>
<div class="tscroll">
<table class="mini fields">
  <thead><tr><th>поле</th><th>что это / как использовать</th></tr></thead>
  <tbody>
{_fields_rows}
  </tbody>
</table>
</div>
<p class="src">(session-stats devnet-log; поправки метода — §3)</p>

<div class="foot">рабочие заметки · ClickHouse mainnet_d + session-stats (devnet-log) + ton/ collator.cpp · окна 5 мин до сплита · пул-минтер 0:A0295D8A… · {nrun} событий / {tot} сплитов · 05-10 → 07-23 2026</div>
</div>
"""

DOC = f"""<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Обзор сплитов базчейна mainnet · 3 месяца</title>
<style>{CSS}</style>
</head>
<body>
{BODY}
</body>
</html>"""
open('/Users/victor/Projects/jetton-bomb/shard-split-report/SHARD_SPLIT_REVIEW.html','w').write(DOC)
print("wrote SHARD_SPLIT_REVIEW.html", len(DOC), "bytes; types:", dict(cnt))
missing=[t0 for t0 in (r['t0'] for r in rows) if not tv(t0)]
print("sessions missing tonviewer example:", len(missing), missing[:5])
