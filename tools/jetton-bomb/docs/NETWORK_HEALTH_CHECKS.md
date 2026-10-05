# Проверка состояния сети под нагрузкой

Чеклист для разбора прогона нагрузки (testnet / mainnet / devnet). Не финальный —
дополнять по мере находок. Составлен по разбору тестнет-прогона 2026-08-02.

Главная ценность файла — раздел [«Штатное, не считать находкой»](#штатное-не-считать-находкой).
Без него легко потратить час на «открытие» того, что все давно знают.

Как готовить и проводить сам прогон (чеклисты до/после, `scripts/netcheck.py`,
как заносить строку в таблицу Stress tests) — [LOADTEST_RUNBOOK.md](LOADTEST_RUNBOOK.md).

---

## Источники данных

| что | как достать |
|---|---|
| **ClickHouse** (индекс блоков/транзакций/джеттонов) | `http://tc-ch-02.toncenter.local:8123/`, БД `mainnet_d` / `testnet_d`. Пользователь `vicread`, ключ — в памяти `toncenter-mainnet-infra-access` (намеренно не вписан сюда: файл лежит в git). Заголовки `X-ClickHouse-User` / `X-ClickHouse-Key`. |
| **Prometheus** | MCP-сервер `ton-prometheus`. Инструменты отложенные, сначала `ToolSearch` с `select:mcp__ton-prometheus__query,mcp__ton-prometheus__range_query,mcp__ton-prometheus__series,mcp__ton-prometheus__label_values,mcp__ton-prometheus__list_targets`. Живёт на `devnet-log`, retention ~15.5 дней. Подробности — память `ton-monitoring-stack`. |
| **Логи валидаторов** | MCP `ton-logs`. Сети `mainnet` / `testnet`, ~70 хостов. Раньше падал с `-32000`, потому что `/home/yungwine` был `0750`; 2026-08-02 на каталог выдан `o+x` (только проход, листинг закрыт), теперь работает от любого пользователя без sudo. Если MCP всё же не поднялся — драйвер напрямую, см. ниже. |
| **Session-stats** | архив на `devnet-log`. Рецепт разбора — память `split-cause-from-session-stats`. |
| **Эксплорер** | `https://test-explorer.toncoin.org/block?workchain=0&shard=<HEX16>&seqno=<N>&roothash=<HEX64>&filehash=<HEX64>` — hex в верхнем регистре. В ClickHouse: `upper(hex(base64Decode(root_hash)))`. |
| **Обсуждения ядра** | `ton_kernel_rnd_dump.jsonl` в корне репо — дамп чата ton kernel R&D (~22 тыс. сообщений, поле `message`). Прежде чем объявлять находку, поискать там. |

### Драйвер к логам в обход MCP (запасной путь)

```bash
# /tmp/tlq.py на devnet-log (пересоздать, если пропал).
# ВАЖНО: нужен guard `if __name__ == "__main__"` — без него функции с process pool
# (error_summary, validation_activity) форк-бомбят себя при переимпорте в потомках.
import sys, json
sys.path.insert(0, "/home/yungwine/mcp-logs-server")
from tonlogs import tools_generic, tools_smart
if __name__ == "__main__":
    fn = sys.argv[1]
    kw = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
    mod = tools_smart if hasattr(tools_smart, fn) else tools_generic
    print(json.dumps(getattr(mod, fn)(**kw), indent=1, default=str)[:20000])
```

```bash
# sudo больше не нужен, cwd не важен — index.db резолвится относительно модуля
ssh victor@devnet-log.toncenter.com \
  '/home/yungwine/mcp-logs-server/venv/bin/python /tmp/tlq.py sync_status "{\"network\":\"testnet\",\"last\":\"60m\"}"'
```

Функции: `list_sources`, `host_liveness`, `sync_status`, `error_summary`,
`validation_activity`, `system_events`, `search_logs`, `count_matches`, `tail_logs`,
`get_context`. Времена в UTC, окна вида `last="90m"` либо явные ISO `start`/`end`.

---

## Порядок проверки

### 0. Доверие к данным — всегда первым

```sql
-- отставание индексатора: у всех трёх таблиц должно быть единицы секунд
select 'blocks' as tbl, max(gen_utime) as newest from testnet_d.blocks
union all select 'transactions', max(now) from testnet_d.transactions
union all select 'jetton_transfers', max(transaction_now) from testnet_d.jetton_transfers;

-- дубликаты строк (были случаи подозрений — проверять, а не верить)
select count() as rows_, uniqExact((workchain, shard, seqno)) as distinct_blocks
from testnet_d.blocks where gen_utime >= <от>;
```

Если хвостовой бакет выглядит просевшим — **перечитать его через минуту-две и сравнить**.
Не изменился → просадка настоящая. Изменился → это догрузка, хвост отбросить.
Немонотонность соседних бакетов (например `21:50 < 21:55`) — почти всегда признак неполноты.

### 1. Пропускная способность

```sql
select toStartOfInterval(fromUnixTimestamp(transaction_now), interval 5 minute) as ts,
       round(count()/300, 1) as jtps, uniqExact(source) as senders
from testnet_d.jetton_transfers where transaction_now >= <от> group by ts order by ts;
```

Отдельно — общий tx/s из `transactions`. **Отношение tx на транзфер должно быть
постоянным** (для нашего пула 4.03). Если поехало — сломалась середина цепочки
(не долетают internal, растёт доля bounce), и это уже настоящая проблема.

### 2. Форма кривой, а не «на глаз»

Всплеск в первые 5–10 минут после подъёма нагрузки — это слив буфера отправителя,
а не устойчивый уровень. Мерить наклон линейной регрессией на плато, а не сравнивать
первый бакет с последним. Проверить, монотонна ли последовательность вообще.

### 3. Блоки

```sql
select toStartOfInterval(fromUnixTimestamp(gen_utime), interval 10 minute) as ts,
       count() as blocks,
       countIf(transaction_count = 0) as empty_blocks,
       round(100.0*countIf(transaction_count=0)/count(), 1) as empty_pct,
       round(avgIf(transaction_count, transaction_count>0), 1) as tx_per_useful,
       countIf(want_split) as want_split, countIf(after_split) as after_split
from testnet_d.blocks where workchain = 0 and gen_utime >= <от> group by ts order by ts;
```

Ключевая производная — **полезных блоков/с/шард** (`непустые / сек / число шардов`).
Именно она, а не общий блокрейт, определяет потолок.

Число шардов честно считается только по мастерчейн-блоку — внутри одного
`mc_block_seqno` набор шардов всегда корректное разбиение:

```sql
select mc_block_seqno, uniqExact(shard) as shards
from testnet_d.blocks where workchain = 0 group by mc_block_seqno;
```

### 4. Упирается ли во что-нибудь

```sql
select toStartOfInterval(fromUnixTimestamp(now), interval 10 minute) as ts,
       round(avg(compute_ph__vm__gas_used), 0) as avg_gas,
       round(avg(compute_ph__vm__vm_steps), 1) as avg_steps,
       countIf(aborted) as aborted,
       countIf(action__no_funds) as no_funds
from testnet_d.transactions where now >= <от> group by ts order by ts;
```

Плюс газ на блок против софт-лимита (базчейн ~10 M, `ConfigParam 23`). Если выбрано
10–20 % — упор **не в ресурсы**, искать в каденсе/таймаутах.
`avg_gas` и `avg_steps` строго постоянные → дрейфа профиля нагрузки нет,
гипотезы «состояние растёт, транзакции дорожают» можно закрывать сразу.

### 5. Здоровье пула отправителей

```sql
-- размер пула и не выпадают ли кошельки
select uniqExact(source) from testnet_d.jetton_transfers where transaction_now between <a> and <b>;
-- дренаж
select quantile(0.5)(account_state_balance), min(account_state_balance)
from testnet_d.transactions where account in (...) and now >= <от>;
```

Дубли/ретраи ловятся распределением интервалов между отправками одного кошелька:
короткие (< 5 с) интервалы = дублирующая отправка, длинные = штатный обход пула.

### 6. Prometheus

- `up` по job'у сети — кто вообще лежал весь прогон.
- `scrape_duration_seconds` — если упёрлось в таймаут (1 с) при том, что
  `ton_exporter_last_collection_duration_seconds` ~10 мс, значит **насыщен actor-планировщик ноды**.
  Это самый честный индикатор перегрузки процесса из тех, что есть.
- `resets()` по любому счётчику — были ли рестарты.
- `rate(ton_perf_op_ticks_total[10m]) / rate(ton_perf_ops_total[10m])` по op
  (`cell_load`, `celldb_store_cell`, `apply_block_to_state_fast`) — растёт ли стоимость операции.
  `cell_store` держать как контроль: если выросло всё, включая его, — это просто нехватка CPU.
- Транспорт: QUIC RTT и потери, ADNL roundtrip и дропы `reason="limited"`, RLDP2.

### 7. Логи

- `sync_status(network, last=...)` — отставание мастерчейна по хостам.
- `error_summary` **на двух окнах**: час под пиком и час на ровном плато. Смотреть на
  дельту, а не на абсолют. Тег `known_noise` — это метка, а не фильтр; но и всплеск
  надо проверять по `ratio_vs_median_7d`, а то «рост» окажется половиной обычного.
- `count_matches(..., bucket="5m")` по коллаторным строкам. Что искать:
  - `medium timeout reached, stop processing inbound external messages` — приём externals обрывается по времени;
  - `Check block overload timers` → `wait_externals` (простой в ожидании) и `do_collate`;
  - `BLOCK FULL`;
  - `block load statistics` → `gas`, `size_est`, `collated_size_est`;
  - `want_split set because of overload history`;
  - `Error 651: too many pending external message checks`, `Error -701`;
  - `final out_msg_queue size`.
- `validation_activity` — равномерны ли ошибки по шардам.

---

## Штатное, не считать находкой

- **LS принимает externals, которые коллатор не включает.** На потолке это ожидаемо:
  `sendBoc` не знает судьбу сообщения. Клиентский счётчик `sent` выше реального
  on-chain темпа в разы (наблюдали 933/с против 295 jtps на toncenter и 985/с против
  ~288 на gton). Следствие: **`sent` бесполезен как метрика нагрузки выше потолка**,
  мерить `TPS(600)` по цепочке (`pkg/tpsmeter`). Ниже потолка совпадение точное.
- **`Error 651` и поток `Error -701`** — следствие того же насыщения, а не причина.
  `-701` — подпись повторной отправки уже применённого external после сдвига seqno.
- **Тестнет не делится дальше 4 шардов** — `max_split = 2` в `ConfigParam 12`.
  При этом `want_split set because of overload history` в логах коллатора может
  идти непрерывно, а в заголовках блоков `want_split` останется нулём: эвристика
  срабатывает, а сплит упирается в потолок глубины. Ограничивает и то, что вообще
  можно измерить тестом порога сплита на тестнете.
- **Первый блок в leader window пустой.** Известная проблема, `ton_kernel_rnd_dump.jsonl`
  #29239 / #29795 / #29808, ранее #23542. Окно = `slots_per_leader_window` из
  `ConfigParam 30` (в тестнете 4), поэтому пустые идут с шагом 4. Нода поздно понимает,
  что окно настало, и стартует с нулевым таймаутом. Без пайплайна коллации-валидации
  по транзакциям не лечится. Цена — бенчмарк `DanShaders/ton feature/bench`: 600–850 tps
  с оптимизацией раннего старта против 415 без неё.
  Частота срабатывания **зависит от нагрузки**: ~10 % окон на 200 jtps против ~98 % на пике.
- **Мастерчейн-блоки не пустые** (~3 транзакции каждый) — если увидел пустые, проверь фильтр `workchain`.
- **В дереве логов `testnet` лежат протухшие копии майннетных хостов** (`ton-nval-*`,
  `ton-sval-*`, `tc-hero-*`, `neodix-val-*`, `tc-pub-ls-03/04`). Все флаги
  `no_status_reports_in_window` оттуда — артефакт.

## Дыры в мониторинге (на 2026-08-02)

- **На тестнетных валидаторах нет node_exporter.** Job `node` — 27 таргетов, все devnet.
  CPU/диск/память тестнета посмотреть нечем.
- `ton-tval-02` и `ton-tval-04` не отвечали весь прогон; `ton-tval-01` мёртв ~88 дней.
- `neodix-ls-02/03` монотонно уезжают в отставание под нагрузкой и не догоняют
  (до 29 и 36 минут за три часа), скорость накопления удваивается на пике.
  На этих хостах другой билд — STATUS печатает секунды, а не миллисекунды.
- Debug-verbosity только на `ton-tval-10` и `ton-tval-11` — все коллаторные внутренности только оттуда.

## Грабли в запросах

- `window` — зарезервированное слово в ClickHouse, алиасом не брать.
- Алиас агрегата нельзя использовать в `WHERE` вложенного запроса (`ILLEGAL_AGGREGATION`).
- SQL писать в файл и слать через `--data-binary @file`: двойные кавычки внутри
  одинарных ClickHouse трактует как идентификаторы.
- Шард в hex: `hex(reinterpretAsUInt64(shard))`; обратно — `reinterpretAsInt64(reverse(unhex('2000000000000000')))`.
- `seqno % N` **не** выделяет позицию в leader window — фаза плывёт, потому что пустой
  блок не всегда получает свой block id. Использовать распределение расстояний между пустыми.
- `rate()` на окне длиннее времени жизни серии занижает результат в разы.
  Проверять, что история покрывает окно, и перепроверять неправдоподобные значения.
- `count_matches` / `search_logs` — всегда передавать явный список `hosts`,
  иначе `scan budget exhausted` (файлы по 28–57 ГБ в день).
- `system_events` игнорирует фильтр хостов и почти всегда упирается в бюджет сканирования.
