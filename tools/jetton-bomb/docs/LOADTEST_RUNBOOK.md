# Раннбук нагрузочного теста (testnet / mainnet / devnet)

Что делать ДО прогона, чем гонять, что снимать ПОСЛЕ и как заносить строку в таблицу
«Stress tests». Цель файла — чтобы не изучать инфраструктуру заново каждый раз.

Смежные файлы: [NETWORK_HEALTH_CHECKS.md](NETWORK_HEALTH_CHECKS.md) — разбор
состояния сети под нагрузкой (там же раздел «штатное, не считать находкой»),
[DEVNET_INTEGRATION.md](DEVNET_INTEGRATION.md) — девнет.

---

## 0. Источники и доступы

| что | где | как |
|---|---|---|
| **Таблица прогонов** | [Stress tests](https://docs.google.com/spreadsheets/d/1GP1C1KkRd1zpmCUJg8zXd2gqxgHmfFOXED_wl4XGdkU/edit) | id `1GP1C1KkRd1zpmCUJg8zXd2gqxgHmfFOXED_wl4XGdkU`, вкладки `Сводка` (основная, 38 колонок A..AL) и `Журналы`. MCP `sheets`: `sheets_get_values` / `sheets_update_values` |
| **ClickHouse** (блоки/транзакции/жетоны) | `http://tc-ch-02.toncenter.local:8123/`, БД `testnet_d` / `mainnet_d` | резолвится **напрямую с ноутбука** (не с devnet-log — там `.local` не резолвится, curl падает с exit 6). Юзер `vicread`, пароль в `configs-private/loadtest.env` (gitignored) |
| **toncenter v2** | `https://testnet.toncenter.com/api/v2`, `https://toncenter.com/api/v2` | конфиг-параметры, шарды, мастерчейн-инфо. Без ключа быстро ловится **429** — `netcheck.py` ретраит, ключ можно дать через `TONCENTER_API_KEY` |
| **Prometheus** | MCP `ton-prometheus` | `ToolSearch select:mcp__ton-prometheus__query,mcp__ton-prometheus__label_values`. Полезно: `up{instance=~".*coll.*"}` — живые коллаторы (`ton-tcoll-01/02` testnet, `ton-coll-01..04` mainnet). Метрик celldb/размера стейта там **нет** |
| **Логи валидаторов** | MCP `ton-logs` (сети mainnet/testnet, ~70 хостов) | сервер периодически отваливается; запасной путь — `/tmp/tlq.py` на devnet-log, рецепт в NETWORK_HEALTH_CHECKS.md |
| **Session stats** | `devnet-log`: тестнет `/var/sftp/sftp-swisscops/session_stats`, майннет `/nvmepool/mainnet_session_stats` (+ MCP `consensus-explorer` по слотам/лидерам) | поминутные `session_stats.<host>_DD-MM-YYYY_HH-MM-SS.log.gz`, по JSON-объекту на склеенный блок; хосты `ton-tval-03..17`, `ton-tcoll-01/02`. **Фильтровать `block_id.workchain==0`.** Ключевое: `block_limits.limits_log` (что закрыло блок: `block_full collated_data` против `INBOUND_*: timeout`), `cat_*`, `overload_reason`/`want_split` (после PR #2488), `bytes`, `collated_data_bytes`, `block_stats.transactions`, `work_time`. Тестнетовская ретенция короткая (в августе — с 17.08), старые прогоны разбирать уже нечем |
| **Grafana toncenter** | `https://status.toncenter.com/d/ee18qqhytttkwa/overview` | это ссылка в колонку `chart`; `?orgId=1&from=<ISO>&to=<ISO>&timezone=browser` |
| **Дашборды devnet-log** | `:8000` devnet, `:18000` mainnet, `:18001` testnet | память `mainnet-testnet-dashboards-tpsmeter`, `devnet-dashboard-tpsmeter-wiring` |
| **Чат ядра** | `ton_kernel_rnd_dump.jsonl` в корне репо | дамп ton kernel R&D. Отсюда: постановка задачи, версии («коллаторы тестнета на acf8639, валидаторы на 3d478cb»), кто занял сеть, что меняли в конфиге. Дамп **статический** — если нужны свежие сообщения, просить перевыгрузку |
| **Эксплореры** | `https://test-explorer.toncoin.org`, `https://explorer.toncoin.org` | `/config?workchain=-1&shard=8000000000000000&seqno=&roothash=&filehash=` — hex в ВЕРХНЕМ регистре, это ссылка в колонку `config` |

Креды: `configs-private/loadtest.env` (в `.gitignore`), формат `CH_USER=` / `CH_PASS=` / `CH_URL=`.
`scripts/netcheck.py` читает его сам, либо берёт из окружения.

---

## 1. Инструмент: `scripts/netcheck.py`

Разбирает BOC-и конфига и ходит в ClickHouse — чтобы не вспоминать TL-B и SQL.

```bash
python3 scripts/netcheck.py config  [testnet|mainnet]   # параметры 8/12/22/23/29/30/34 + шарды + конфиг-ссылка
python3 scripts/netcheck.py diff                        # testnet vs mainnet по ключевым параметрам
python3 scripts/netcheck.py status [testnet|mainnet]    # шарды / tps за 15 мин / блокрейт сейчас
python3 scripts/netcheck.py run "2026-08-24 16:00" "2026-08-24 17:00" testnet
```

`run` печатает ровно то, что идёт в таблицу: tps по минутам, **max 15-min avg tps**
(топ-3 окна), блокрейт и bps на шард по 5 мин, долю пустых блоков, tx на непустой
блок, `want_split`/`want_merge`, jtps + tx на трансфер и готовую ссылку на графану.
Сверено на прогоне #18 (16.08): скрипт даёт `1176.7` при записанных в таблице `1177 TPS`,
блокрейт 2.48 на плато, 4 шарда — совпадает.

Что означают параметры конфига:

- **param 8** — `version` / `capabilities`. Мейннет и тестнет сейчас `15` / `1006`.
  Версия ниже 13 = нет сабсекундных блоков (наступали на это в девнете, прогон #16).
- **param 12** — `min_split` / `max_split` на воркчейн. `max_split=2` → до 4 шардов,
  `max_split=0` → **сплит запрещён, сеть будет сидеть на 1 шарде**. Ловушка №1, см. §5.
- **param 22 / 23** — лимиты блока мастерчейна / шардов: `bytes`, `gas`, `lt_delta`,
  каждый как `[underload, soft, hard]`. Упираемся обычно в `bytes soft` (1 MiB).
- **param 29** — `max_block_bytes` / `max_collated_bytes` / catchain-параметры.
- **param 30** — simplex-конфиг отдельно для `mc` и `shard`: `protocol_version` (2 бита!),
  `use_quic`, `slots_per_leader_window`, некритичные параметры (`target_rate_ms` 400,
  `first_block_timeout_ms` 700, `min_block_interval_ms` 300).
  **`protocol_version >= 2` = plumtree (новые бродкасты)**, `== 1` = block sync, `0` = базовый.
  Отсюда заполняются колонки `proto version` / `old broadcast` / `new broadcast (plumtrees)`.
- **param 34** — текущий сет: `total` / `main` + окно раунда. Тестнет ~14-15 (раунд 1 час),
  мейннет 385/100. Это колонка `validators`.

---

## 2. Чеклист ПЕРЕД прогоном

1. **Понять цель и опорную точку.** Постановка обычно в чате (`ton_kernel_rnd_dump.jsonl`,
   ищем сообщения Толи с планом на день). Найти в таблице предыдущую строку с той же
   методикой — её `measured value` идёт в новую строку как `predicted value`, а номер — в мотивацию
   («держит ли те же тпс, что 16.08 (#18)»).
2. **Сеть свободна?** В чате пишут «беру тестнет/девнет» — убедиться, что никто не грузит
   и никто не катает апдейты (`swisscops` часто в этот момент обновляет машины).
3. **Версии** валидаторов и коллаторов — из чата; в строку идут коммиты, не «последняя ветка».
4. **Конфиг:** `python3 scripts/netcheck.py config testnet` и `diff`. Смотреть:
   - `max_split` — для теста на 4 шардах нужен **2**; для теста «порог сплита» нужен
     сначала мерж до 1 шарда;
   - лимиты 22/23 — не остались ли включёнными эксперименты (23.08 поднимали x1.5 и потом откатывали);
   - param 30 — какие бродкасты, совпадает ли с мейннетом;
   - param 34 — сколько валидаторов в текущем раунде (го-валидаторы выводятся стейком 0).
5. **Baseline:** `python3 scripts/netcheck.py status testnet` — органический tps за 15 мин
   (колонка `organic tps`), блокрейт (норма 2.5 bps), сколько шардов сейчас.
6. **Свежесть индекса ClickHouse** — печатается в `status`; если отставание больше пары
   секунд, цифры после прогона будут неполными.
7. **Завести строку в таблице заранее** (мотивация, дата, план, метод, предсказание,
   версии, конфиг-ссылка) — после прогона останется дописать факт.

---

## 3. Чем гонять

**TG-бот** (`go-spam/cmd/bot`, живёт на девнет-хосте, пишет `start`/`stop` в чат с
`sent/failed/rate/секунды` — эти цифры идут в `notes`):

- `/hamster_kombat <jtps>` — жетон-трансферы со всего пула 2.5M кошельков (по умолчанию для тестнета);
- `/jetton <jtps>` — то же, но по окну `/pool_size`;
- `/fat_gram`, `/fat_jetton`, `/fat_size`, `/fat_count`, `/fat_mutate` — нагрузка на большие аккаунты;
- `/pool_size <n>` — окно пула (на весь пул = поставить размер пула);
- `/stop` — останавливает всё; `/timeout`, `/errors`, `/retry`, `/initmode`, `/api`, `/config`.

**Headless** (`go-spam/cmd/spam`): `--mode jetton|external|targets|jetton-fats|highload-*`,
`--check-balance`, `--no-retry`. Полное описание — в `CLAUDE.md`.

**Метр TPS** (`go-spam/cmd/tpsmeter`) — живой шард-каунт и TPS 1/5/15 мин, когда
ClickHouse недоступен (девнет) или нужен живой контроль во время прогона.

Типовые методики (переносить в колонку `method` дословно из прошлой строки):

- «1000 jtps -> до 4000 tps; считаем on-chain tps» — потолок, метрика `max 15 min avg tps`;
- «200 jtps -> 800 tps» — стабильность, метрика `15 min avg tps`;
- «бинарный поиск между 200 и 400 TPS, точность до 10 TPS…» — порог сплита;
- «держим N jtps весь прогон, фазы: …» — отказоустойчивость (гасим коллаторы и т.п.).

---

## 4. Чеклист ПОСЛЕ прогона

1. `python3 scripts/netcheck.py run "<start UTC>" "<end UTC>" testnet` — оттуда:
   - `max 15-min avg tps` → колонка `measured value`;
   - `bps_per_shard` на плато → `actual blockrate`, окно и число шардов → `blockrate notes`;
   - доля пустых блоков, tx на блок, `want_split` → в `notes`, если объясняют результат;
   - `tx_per_transfer` ≈ 4.03 — если поехало, значит рвалась цепочка (это уже находка);
   - готовая ссылка на графану → колонка `chart`.
2. `sent / failed / rate / длительность` — из stop-сообщения бота в чате.
3. Если упёрлись — понять во что: session-stats (`cat_*`, `limits_log`: `block_full bytes` /
   `collated_data` / `INBOUND_*` / long_collation), память `split-cause-from-session-stats`,
   `block-bytes-is-the-binding-limit`, `collators-160gb-collated-data-ceiling`.
4. **Перед тем как объявлять находку** — прочитать раздел «штатное, не считать находкой»
   в NETWORK_HEALTH_CHECKS.md и поискать по чат-дампу.
5. Дозаполнить строку в таблице (см. §5) и, если нужно, дать в чат ссылки на конкретные
   адреса/транзакции, чтобы можно было проверить самому.

---

## 5. Как заносить строку в таблицу

Вкладка `Сводка`, шапка в строке 1, данные с строки 2. Новая строка = следующий `#N`
(например `#19` лёг в `Сводка!A20`).

```
mcp__sheets__sheets_update_values(
  spreadsheetId="1GP1C1KkRd1zpmCUJg8zXd2gqxgHmfFOXED_wl4XGdkU",
  range="Сводка!A20",            # A<номер строки>, диапазон растянется сам
  valueInputOption="USER_ENTERED",
  values=[[ ...36 значений A..AJ... ]])
```

**Стиль:** кратко, с маленькой буквы, по делу, без пафоса; за основу берётся
предыдущая строка с той же методикой и в ней меняются данные. Незаполненное = `-`.

| кол. | заголовок | что писать |
|---|---|---|
| A | — | `#N` |
| B | motivation | зачем прогон, одной фразой + на какую прошлую строку опираемся |
| C | date | `24.08.2026 16:00 UTC` (в UTC; в таблице попадаются и MSK — держаться UTC) |
| D | planned duration | `1h`, `30m + 30m check` |
| E | actual duration | по факту |
| F | method | дословно из прошлой строки, если методика та же |
| G | network | `testnet` / `mainnet` / `devnet` |
| H | target value | **метрика**, а не число: `max 15 min avg tps`, `15 min avg tps`, `1->2 shard split load` |
| I | predicted value | предсказание до прогона, обычно `≈X TPS (как в #M)` |
| J | measured value | факт из `netcheck run` |
| K | notes | что увидели: во что упёрлись, sent/failed, аномалии |
| L | validator version | коммиты: `3d478cb (v2026.08, master); коллаторы acf8639 (ветка collators)` |
| M | load type | `jetton transfers, 4tx each, no excesses, получатели - свои же случайные кошельки пула` |
| N | jettons | `tolk, prepaid, optimized (jetton-prepaid)` |
| O | sender | чем гоняли: `tg bot, go, 344881, /jetton` \| `/hamster_kombat` \| `ton jetton-spam (go, cmd/spam)` |
| P | **sources** | **сколько кошельков отправляют**: `100k wallet-spam` (тестнет по умолчанию, окно `/pool_size`), `2.5m wallet-spam` (весь пул, `/hamster_kombat`), `5k wallet-spam` (майннет и девнет) |
| Q | **targets** | **кому шлём**: при пул-в-пул то же, что в `sources`; при fat-режимах — `<N> fats of <S>KB` (напр. `50k fats of 200KB`) |
| R | send backend | `tgls, 2 go ls` или `https://testnet.toncenter.com/api/v2` |
| S | init mode (bot) | `0 (classic jetton state init)` — на 1 не переходим, имитируем реальную нагрузку |
| T | retries | `on` / `off` |
| U | check balance | `on` / `off` |
| V | in memory | `on in collators` / `off` |
| W | collators | `on, 2 (ton-tcoll-01/02)`; в мейннете 4 |
| X | validators | из `param 34`: `14 в текущем раунде, ton-tval-*, trusted` + примечания (го выведены стейком 0) |
| Y | config | ссылка на `/config?...` эксплорера — печатает `netcheck config` |
| Z | proto version | из param 30: `1 в шардах / 0 в мастерчейне (как в майннете)` |
| AA | min_split | из param 12 |
| AB | max_split | из param 12 — **проверять всегда** |
| AC | organic tps (15 мин) | из `netcheck status` до прогона |
| AD | old broadcast | `on`, если `protocol_version < 2` |
| AE | new broadcast (plumtrees) | `on`, если `protocol_version >= 2` |
| AF | target blockrate | `2.5 bps` (из `target_rate_ms` 400) |
| AG | actual blockrate | `bps_per_shard` на плато |
| AH | blockrate notes | `clickhouse, 16:00-17:00 utc; 4 шарда` |
| AI | full parameters (neodix) | строка запуска валидаторов, если менялась, иначе `-` |
| AJ | full parameters (swisscops) | пер-хостовые флаги, иначе `-` |
| AK | parameters note | отличия конфига от мейннета и всё, что важно знать про параметры |
| AL | chart | ссылка на графану — печатает `netcheck run` |

`sources`/`targets` появились 24.08 (строки до этого проставлены задним числом). Они же держат
**A/B-пары**: один и тот же прогон с `100k wallet-spam` и сразу следом с `2.5m wallet-spam`
(`/hamster_kombat`) должен давать одинаковый tps — если нет, дело в размере пула отправителей,
а не в сети. В `motivation` такой пары прямо писать «A/B к #N».

---

## 6. Ловушки (за них уже платили временем)

- **`max_split = 0`.** 20.08 сплит в тестнете выключили ради экспериментов на 1 шарде.
  Тест «на 4 шардах» в таком конфиге просто не состоится, а по метрикам это выглядит как
  «сеть не держит». Проверять `param 12` перед КАЖДЫМ прогоном.
- **`protocol_version` — 2-битное поле** внутри `simplex_config_v2#22`
  (`flags:(## 5) protocol_version:(## 2) use_quic:Bool`). Plumtree включается только при `>= 2`.
  По полю в таблице `proto version` из старых строк ориентироваться нельзя — сравнивать с мейннетом.
- **Хвост `workchain_v2#a7`** (split_merge_timings, persistent_state_split_depth) между версиями
  уезжает на пару бит — в `netcheck.py` он читается **с конца** ячейки, не с начала.
- **toncenter без ключа = 429** на серии запросов.
- **ClickHouse `.local` резолвится с ноутбука, но не с devnet-log.**
- **`data_boc` в ClickHouse — NULL**, размер стейта аккаунта оттуда не посчитать
  (память `big-state-accounts-are-the-grinders`: считать через `getShardAccountCell`).
- **Отставание индекса** и догрузка хвоста: последний бакет перечитать через минуту.
- **MCP `ton-logs` периодически отваливается** — запасной путь через ssh на devnet-log.
- **Не экономим на цене транзакции** (init mode 0, реальные жетон-трансферы) — имитируем
  реальную пользовательскую нагрузку, а не самую дешёвую.
