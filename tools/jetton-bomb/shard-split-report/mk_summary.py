import sys
rows=open('/tmp/summary_rows.html').read()
REPORT_URL=sys.argv[1] if len(sys.argv)>1 else '#'
page=f'''<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Сплиты базчейна mainnet — итог и полная таблица</title>
<style>
:root{{
  --bg:#000; --panel:#0b0b0b; --line:#262626; --line2:#1a1a1a;
  --fg:#e8e8e8; --mut:#9aa0a3; --dim:#6f767a; --acc:#e3a83e;
  --lc:#d472d4; --q:#5b9bd5; --g:#e56b42; --b:#37b3a5; --c:#9d80e6; --u:#94a4ab;
  --ok:#4fb06a; --ours:#5b9bd5;
  --mono:ui-monospace,SFMono-Regular,"SF Mono",Menlo,Consolas,monospace;
  --sans:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,"Helvetica Neue",sans-serif;
}}
*{{box-sizing:border-box}}
body{{margin:0;background:var(--bg);color:var(--fg);font:15px/1.62 var(--sans);
  -webkit-font-smoothing:antialiased;padding:0 0 90px;max-width:100%;overflow-x:clip}}
.wrap{{max-width:820px;margin:0 auto;padding:0 20px}}
img,svg{{max-width:100%}}
a{{color:var(--acc);text-decoration:none;border-bottom:1px solid #4a3a17}}
a:hover{{border-bottom-color:var(--acc)}}
a:focus-visible{{outline:2px solid var(--acc);outline-offset:2px}}
.eyebrow{{font:11px/1 var(--mono);letter-spacing:.16em;text-transform:uppercase;color:var(--dim);margin:0 0 12px}}
h1{{font-size:31px;line-height:1.18;margin:0 0 10px;font-weight:640;letter-spacing:-.01em;text-wrap:balance}}
h2{{font-size:20px;margin:52px 0 6px;font-weight:620;letter-spacing:-.005em;text-wrap:balance}}
h2 .num{{font:12px/1 var(--mono);color:var(--dim);margin-right:9px;letter-spacing:.08em}}
h3{{font-size:15px;margin:26px 0 6px;font-weight:640}}
p{{margin:9px 0}}
.sub{{color:var(--mut);font-size:15px;margin:0 0 4px}}
.src{{color:var(--dim);font-size:12.5px}}
.m{{font-family:var(--mono);font-variant-numeric:tabular-nums}}
b,strong{{font-weight:640;color:#fff}}
header{{border-bottom:1px solid var(--line);padding:44px 0 26px;margin-bottom:8px}}
.stats-scroll{{overflow-x:auto;max-width:100%;margin:26px 0 4px;scrollbar-width:thin;scrollbar-color:#3a3a3a transparent}}
.stats-scroll::-webkit-scrollbar{{height:9px}}
.stats-scroll::-webkit-scrollbar-track{{background:#0a0a0a}}
.stats-scroll::-webkit-scrollbar-thumb{{background:#3a3a3a}}
.stats{{display:grid;grid-template-columns:repeat(auto-fit,minmax(148px,1fr));gap:1px;background:var(--line);border:1px solid var(--line)}}
.stat{{background:var(--bg);padding:14px 15px}}
.stat .v{{font:26px/1.1 var(--mono);font-variant-numeric:tabular-nums;font-weight:600}}
.stat .k{{font-size:12px;color:var(--mut);margin-top:5px;line-height:1.35}}
.panel{{background:var(--panel);border:1px solid var(--line);padding:16px 18px;margin:18px 0}}
.panel p:first-child{{margin-top:0}} .panel p:last-child{{margin-bottom:0}}
.split{{display:grid;grid-template-columns:repeat(auto-fit,minmax(290px,1fr));gap:1px;background:var(--line);
  border:1px solid var(--line);margin:18px 0}}
.split > div{{background:var(--bg);padding:15px 17px}}
.split h4{{margin:0 0 7px;font-size:13px;font-family:var(--mono);font-weight:640;letter-spacing:.02em}}
.split p{{margin:0;color:var(--mut);font-size:13.5px;line-height:1.55}}
.split p + p{{margin-top:7px}}
.tbl-scroll{{overflow-x:auto;max-width:100%;border:1px solid var(--line);margin:16px 0 6px;background:var(--panel);
  scrollbar-width:thin;scrollbar-color:#3a3a3a transparent}}
.tbl-scroll::-webkit-scrollbar{{height:9px}}
.tbl-scroll::-webkit-scrollbar-track{{background:#0a0a0a}}
.tbl-scroll::-webkit-scrollbar-thumb{{background:#3a3a3a}}
table{{border-collapse:collapse;width:max-content;min-width:100%;font-size:12.5px}}
thead th{{position:sticky;top:0;z-index:2;background:#101010;text-align:left;font:11px/1.3 var(--mono);
  letter-spacing:.05em;color:var(--mut);font-weight:600;padding:9px 10px;border-bottom:1px solid var(--line);white-space:nowrap}}
td{{padding:7px 10px;border-bottom:1px solid var(--line2);vertical-align:top}}
tbody tr:hover td{{background:#0e0e0e}}
td.m{{font-family:var(--mono);font-variant-numeric:tabular-nums;white-space:nowrap}}
td.n{{text-align:right}} td.c{{text-align:center;color:var(--mut)}}
td.b{{font-weight:640}} td.hot{{color:var(--g);font-weight:640}}
td.desc{{color:var(--mut);font-size:12px;line-height:1.5;border-left:1px solid var(--line);max-width:430px}}
.tag{{font:10.5px/1 var(--mono);padding:3px 6px;border:1px solid;white-space:nowrap;letter-spacing:.02em}}
.t-lowtps{{color:var(--u);border-color:#3a4145}} .t-broad{{color:var(--b);border-color:#1f4a45}}
.t-compute{{color:var(--g);border-color:#5a3122}} .t-ours{{color:var(--ours);border-color:#26415c}}
td.trig{{font-size:11.5px}} td.trig.dim{{opacity:.66}}
.g-lc{{color:var(--lc)}} .g-q{{color:var(--q)}} .g-g{{color:var(--g)}} .g-b{{color:var(--b)}}
.g-c{{color:var(--c)}} .g-d{{color:var(--c)}} .g-u{{color:var(--u)}}
td.trig .q{{color:var(--dim);margin-left:1px}}
td.trig i{{color:var(--dim);font-style:normal;font-size:10px;margin-left:5px}}
tr.r-ours td{{background:#07101a}} tr.r-ours:hover td{{background:#0a1725}}
tr.r-compute td{{background:#120b07}}
.legend{{display:grid;grid-template-columns:150px 1fr;gap:7px 16px;margin:14px 0 0;font-size:13.5px;align-items:baseline}}
.legend dt{{font-family:var(--mono);font-size:12px;font-weight:640}}
.legend dd{{margin:0;color:var(--mut)}}
.note{{border-left:2px solid var(--acc);padding:2px 0 2px 15px;margin:18px 0;color:var(--mut);font-size:14px}}
.note b{{color:var(--fg)}}
.cta{{display:block;background:var(--panel);border:1px solid var(--line);padding:18px 20px;margin:26px 0 0;
  border-bottom:1px solid var(--line)}}
.cta:hover{{border-color:#3a3a3a}}
.cta .t{{font-size:16px;font-weight:640;color:var(--acc)}}
.cta .d{{color:var(--mut);font-size:13.5px;margin-top:5px}}
footer{{margin-top:56px;padding-top:20px;border-top:1px solid var(--line);color:var(--dim);font-size:12.5px}}
@media (max-width:640px){{
  .wrap{{padding:0 15px}} h1{{font-size:25px}} .legend{{grid-template-columns:1fr;gap:3px 0}}
  .stat .v{{font-size:22px}}
}}
@media (prefers-reduced-motion:reduce){{*{{transition:none!important;animation:none!important}}}}
</style>

<header><div class="wrap">
  <h1>Сплиты mainnet</h1>
  <div class="stats-scroll"><div class="stats">
    <div class="stat"><div class="v">84</div><div class="k">сплита <span class="m">before_split</span></div></div>
    <div class="stat"><div class="v">50</div><div class="k">событий (групп сплитов подряд)</div></div>
    <div class="stat"><div class="v" style="color:var(--ours)">4</div><div class="k">наших прогона — 8% событий</div></div>
    <div class="stat"><div class="v" style="color:var(--lc)">31</div><div class="k">событие с нагрузкой 16–48 TPS</div></div>
    <div class="stat"><div class="v" style="color:var(--ok)">31</div><div class="k">сплит, где триггер виден в архиве напрямую</div></div>
  </div></div>
</div></header>

<div class="wrap">

<h2><span class="num">01</span>Что получилось</h2>
<p>Из 50 событий наших — <b>4</b>. Из оставшихся 46: <b>2</b> compute-аномалии (один тяжёлый по газу контракт — единственные случаи за 3 месяца, где газ вообще перешёл soft-лимит), <b>13</b> широкого органического флуда (пик 43–252 TPS) и <b>31</b> при 16–48 TPS.</p>
<p>Последние 31 не объясняются блок-данными: газ низкий, блоки мелкие, транзакций мало, очередей нет. По индексатору у них причины не видно. В архиве session-stats у этих окон выполняется условие триггера <span class="m">long_collation</span>: <span class="m">total &gt; 0.1с</span> И <span class="m">wait_externals &lt; 0.2·total</span> И <span class="m">do_collate &gt; 0.6·total</span>. Остальные три триггера коллатора для них исключены счётом: <span class="m">load_class</span> достижим по взвешенной истории ровно в 2 сплитах из 84, <span class="m">queue ≥ 4096</span> встречается только на дочерних шардах нашего флуда 13.07, <span class="m">dispatch</span> не сработал ни разу на 1 730 разобранных блоках.</p>

<div class="note"><b>Что означает метка <span class="m">long_collation</span>.</b> У блоков без неё <span class="m">check_load_total_time</span> практически такая же, как у помеченных — <b>0.394 с</b> против <b>0.378 с</b>, — но <span class="m">work_time</span> <b>0.035 с</b> против <b>0.378 с</b>. Эти ~0.4 с — фиксированный бюджет ожидания экстерналов. То есть метка говорит не «блок собирался аномально долго», а «коллатор потратил бюджет на работу вместо ожидания»; сам по себе 0.37-секундный блок перегрузкой называть не следует.</div>

<p>Все 31 событие произошли после обновления ноды <b>26 → 27 мая</b>. Триггер написан раньше, 06.04 (PR&nbsp;#2290), а действовать на майннете начал с этого обновления. Одной и той же формулой условие выполнялось в мае у 10–41% блоков, после — у 4–28%, то есть с сопоставимой частотой; при этом в мае нет ни одного сплита ниже 68 TPS, а после — 31 событие при 16–48. Наблюдение согласуется с тем, что изменилась не нагрузка, а набор условий, на которые нода реагирует; изолированного эксперимента здесь нет — в том же обновлении приехали и другие изменения.</p>

<h3>На что уходит время коллации</h3>
<p>Метка <span class="m">INBOUND_*_MESSAGES: timeout</span> в <span class="m">limits_log</span> признаком механизма не является. Дедлайн у коллации один на весь блок (<span class="m">max(wait_externals_until, soft_timeout)</span> = начало слота), а проверка стоит в начале цикла, поэтому метка фиксирует, <b>где</b> кончился бюджет, а не <b>на что</b> он ушёл; внутри LC-блоков она почти тавтологична.</p>
<p>Поэтому сравниваем по измеренному времени фаз (<span class="m">work_time_real_stats</span>) и по фактическому объёму обхода, где обход = <span class="m">miss_cells + small_cells</span>. Второе слагаемое обязательно: кэш storage-статистики не хранит аккаунты меньше <span class="m">MIN_ACCOUNT_CELLS = 4000</span> (<span class="m">validator/storage-stat-cache.hpp:65</span>), но обходит их так же полно, а аккаунты меньше 26 ячеек счётчикам не видны вовсе.</p>
<p>Сравнение — с контрольными блоками тех же окон: 31 low-TPS событие, 1 049 покрытых архивом блоков, включая пустые (<span class="m">check_block_overload</span> выполняется на каждом сколлаченном блоке; среди 195 LC-блоков пустых 88). Все значения — медианы по блоку.</p>
<div class="tbl-scroll" style="margin-top:14px">
<table>
<thead><tr><th></th><th style="text-align:right">предложено ext</th><th style="text-align:right">принято ext</th>
<th style="text-align:right">отклонено ext</th><th style="text-align:right">аккаунтов инстанцировано</th>
<th style="text-align:right">обход, ячеек</th><th style="text-align:right">prelim/total</th></tr></thead>
<tbody>
<tr><td><span class="g-lc">LC-блоки (195)</span></td><td class="m n b">55</td><td class="m n">0</td><td class="m n b">54</td><td class="m n b">19</td><td class="m n b">16 660</td><td class="m n b">97%</td></tr>
<tr><td>контроль — не-LC блоки тех же окон (854)</td><td class="m n">17</td><td class="m n">3</td><td class="m n">14</td><td class="m n">5</td><td class="m n">670</td><td class="m n">17%</td></tr>
</tbody>
</table>
</div>
<p>В пересчёте на транзакцию обход <b>1 826</b> ячеек против <b>111</b>. Отношение real/cpu на фазе <span class="m">prelim</span> — <b>1.77</b> против <b>1.04</b>: у LC-блоков процессор ждёт загрузку ячеек, у контрольных ожидания нет.</p>
<p>Показатели складываются в одну цепочку, а не в противопоставление «CellDB против экстерналов». По коду она такая: <span class="m">process_external_message → create_ordinary_transaction → make_account → init_account_storage_dict</span>, и внутри последнего тикает <span class="m">prelim_storage_stat</span>. Поток предлагаемых коллатору экстерналов заставляет инстанцировать аккаунт получателя на каждый из них, storage-словарь при этом строится заново — кэш не держит аккаунты меньше 4000 ячеек, — и бюджет коллации исчерпывается. По времени это перестроение storage-словарей (97% фазы), по причине — поток экстерналов. Отклонение экстернала выясняется только после попытки создать транзакцию, поэтому отвергнутые стоят столько же, сколько принятые: коллатору предлагают 11–24 экстернала на блок, отклоняется 95–99%, и уровень отказов одинаков в мае и в июне-июле.</p>
<p>Корреляции по всем 1 049 покрытым блокам: отклонённые экстерналы ↔ обход ячеек <b>r = +0.60</b>; число инстанцированных аккаунтов ↔ обход <b>+0.51</b>; обход ↔ время фазы <span class="m">prelim</span> <b>+0.57</b>; принятые экстерналы ↔ обход <b>+0.32</b>. Это согласуется с описанной цепочкой, но сами по себе корреляции её не устанавливают — порядок вызовов взят из кода, а не измерен.</p>
<p>Зависимость монотонная. Доля LC-блоков по децилям объёма обхода:</p>
<div class="tbl-scroll">
<table>
<thead><tr><th>обход, ячеек</th><th style="text-align:right">0</th><th style="text-align:right">27–82</th><th style="text-align:right">85–202</th><th style="text-align:right">203–595</th><th style="text-align:right">602–4.2k</th><th style="text-align:right">4.2–8.7k</th><th style="text-align:right">8.7–14.5k</th><th style="text-align:right">14.5–20k</th><th style="text-align:right">20–24k</th><th style="text-align:right">24–81k</th></tr></thead>
<tbody><tr><td>доля LC</td><td class="m n">0%</td><td class="m n">0%</td><td class="m n">0%</td><td class="m n">0%</td><td class="m n">4%</td><td class="m n">11%</td><td class="m n">15%</td><td class="m n">19%</td><td class="m n">18%</td><td class="m n b">52%</td></tr></tbody>
</table>
</div>
<div class="split">
  <div>
    <h4 style="color:var(--lc)">Массовый механизм: обход рядовых аккаунтов</h4>
    <p>Медианный LC-блок инстанцирует <b>19</b> аккаунтов и обходит <b>~16.7 тыс.</b> ячеек, при этом на фазу <span class="m">prelim</span> приходится 97% времени коллации. Это <b>не</b> «большие аккаунты»: медиана на блок — <b>19 мелких</b> (26–3999 ячеек, которые кэш не хранит) и <b>0 крупных</b> (≥4000 ячеек); средний обойдённый мелкий аккаунт — <b>719</b> ячеек (медиана). По суммарному объёму обхода мелкие дают <b>69%</b>, крупные — <b>30%</b>.</p>
    <p>То есть массовый механизм — обход примерно двух десятков рядовых аккаунтов на блок, каждый из которых кэш отказывается хранить из-за порога <span class="m">MIN_ACCOUNT_CELLS = 4000</span>, поэтому их словари строятся заново на каждом блоке. Направление, в котором тут что-то можно менять, — порог кэширования и поток отвергаемых экстерналов, порождающий инстанцирование этих аккаунтов.</p>
    <p>Чего отсюда не следует: доля аккаунтов, инстанцированных именно из-за отклонённых экстерналов, поблочно не измерена — связь установлена корреляцией и порядком вызовов в коде.</p>
  </div>
  <div>
    <h4 style="color:var(--u)">Редкий крайний случай: один крупный аккаунт</h4>
    <p>Крупные аккаунты — редкий режим: они дают <b>30%</b> суммарного объёма обхода и все блоки дольше секунды (вплоть до 6.4 с), но <b>в 62% LC-блоков крупного аккаунта нет вообще</b> (<span class="m">miss_cnt = 0</span>). Опознаны: Highload Wallet V2 <span class="m">0:78724B16…A166</span> с насыщенным словарём <span class="m">old_queries</span> (50 163 ячейки; обход рос 38 185 → 48 971 → 49 796 за три дня) и накопитель поинтов <span class="m">0:6396D51D…F8A</span> (20 681 ячейка). Размеры — из сегодняшнего стейта (исторические состояния аккаунтов недоступны), и для таких аккаунтов это допустимо: большой стейт меняется медленно, поэтому сегодняшний размер совпадает с числом обойдённых тогда ячеек с точностью <b>1%</b>.</p>
    <p>Здесь решает промах кэша: блок <span class="m">77185054</span> — 1.011 с при 49 723 обойдённых ячейках, соседний <span class="m">77185055</span> — <b>0.018 с</b> при 49 724 попаданиях, то есть та же фаза в 56 раз быстрее. Кэш свой у каждой ноды, поэтому один и тот же аккаунт у одного валидатора рвёт блок, а у другого проходит незаметно. К массовой картине этот режим не сводится.</p>
  </div>
</div>
<div class="note"><b>Оговорка о выборке.</b> В покрытых архивом окнах LC-блоки составляют <b>18%</b>, а <span class="m">want_split</span> требует доли <b>≥ 2/3</b> блоков регистра — этого порога наблюдения не достигают; это ограничение выборки архива (пишут ~11 из ~350 валидаторов), а не свойство нагрузки. Поэтому <span class="m">long_collation</span> — единственный уцелевший кандидат по исключению: его сигнатура в окнах присутствует, требуемая плотность прямым наблюдением не подтверждена.</div>

<h2><span class="num">02</span>Полная таблица · 84 сплита</h2>
<p class="src">Одна строка — один сплит <span class="m">before_split</span>. Метрики посчитаны по окну 5 минут до сплита. Наведите курсор на триггер — в подсказке цифры-основания. Описание нагрузки объединено по событию.</p>
<div class="tbl-scroll">
<table>
<thead><tr>
  <th>UTC</th><th>шард</th><th>тип нагрузки</th><th style="text-align:right">TPS 5м</th>
  <th style="text-align:right">TPS 1м пик</th><th style="text-align:right">газ/tx</th>
  <th style="text-align:right">пик газа</th><th>триггер</th><th>что это была за нагрузка</th>
</tr></thead>
<tbody>
{rows}
</tbody>
</table>
</div>

<dl class="legend">
  <dt style="color:var(--lc)">long_collation</dt><dd>коллация заняла слишком долго — время, а не объём</dd>
  <dt style="color:var(--q)">queue</dt><dd>очередь исходящих меж-шардовых сообщений достигла 4096</dd>
  <dt style="color:var(--g)">load_class:gas</dt><dd>блок упёрся в soft-лимит газа (10M)</dd>
  <dt style="color:var(--b)">load_class:bytes</dt><dd>блок упёрся в soft-лимит байтов (1 МБ)</dd>
  <dt style="color:var(--c)">load_class:cdata</dt><dd>упёрлись collated data — данные для валидации блока</dd>
  <dt style="color:var(--c)">dispatch</dt><dd>исчерпан лимит dispatch-очереди</dd>
  <dt style="color:var(--u)">unknown</dt><dd>архив не покрыл этот шард в окне — триггер не определён</dd>
  <dt>метка <span style="color:var(--dim)">?</span></dt><dd>вывод по исключению, без прямого наблюдения</dd>
  <dt>пик газа</dt><dd>максимум газа на блок в долях от soft-лимита 10M</dd>
</dl>

<h2><span class="num">03</span>Как это измерено</h2>
<p>Из индексатора определимы только газ и lt. Очередь, тайминги коллации, dispatch и collated data в блок-данных отсутствуют в принципе, а поле <span class="m">want_split</span> там мусорное — оно проставлено лишь на дочерних блоках после сплита. Поэтому триггеры разбирались по архиву session-stats: <b>1 730</b> блоков, сколлаченных валидаторами на нужных шардах, каждое из четырёх условий проверено дословно.</p>
<p>Отдельно посчитан порог истории: <span class="m">want_split</span> требует устойчивого перегруза — минимум <b>24 перегруженных блока</b> — если это 16 самых свежих (вес 3) плюс 8 следующих (вес 2); при равномерном распределении по всему регистру из 48 блоков нужна доля <b>≥ 2/3</b>, то есть 32 из 48, поэтому один медленный блок ничего не раскалывает. Этот вес вычислен по каждому блоку — и по нему <b>load_class достижим ровно в 2 сплитах</b> из 84.</p>

<div class="panel">
<p><b>Что осталось открытым.</b></p>
<p>— Плотность LC-блоков в покрытых окнах — 18% против требуемых ≥2/3: ограничение выборки архива.</p>
<p>— У <b>17 из 60</b> <span class="m">long_collation</span>-сплитов в окне нет ни одного закоммиченного LC-блока — там метка является экстраполяцией с покрытых окон, а не наблюдением.</p>
<p>— У 9 сплитов архив не покрыл шард в нужном окне — триггер помечен <span class="m">unknown</span>.</p>
<p>— Скачок порога сплита 200 → 330 TPS между нашими прогонами 13.07 и 20.07 объяснения пока не имеет.</p>
</div>

<a class="cta" href="{REPORT_URL}">
  <div class="t">Полный отчёт →</div>
  <div class="d">Механизм из кода ноды, разбор всех 4 триггеров, проверка гипотезы о больших аккаунтах (как массовая причина опровергнута), графики, таблица сплитов с описаниями нагрузки и приложения.</div>
</a>

<footer><div>Источники: ClickHouse <span class="m">mainnet_d</span>, архив session-stats валидаторов, исходники ноды <span class="m">ton/</span>. Окно: 10.05–23.07.2026.</div></footer>
</div>
'''
open('/Users/victor/Projects/jetton-bomb/shard-split-report/SHARD_SPLIT_SUMMARY.html','w').write(page)
print('written',len(page),'bytes')
