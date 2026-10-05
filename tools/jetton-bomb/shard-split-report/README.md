# Сборка отчёта о сплитах базчейна

Генераторы двух HTML-страниц в корне репозитория. Скрипты писались в `/tmp` и однажды
уже пострадали от системной чистки — здесь их постоянная копия вместе со всеми входными файлами.

## Запуск

Скрипты читают входные файлы по абсолютным путям `/tmp/…`, поэтому сначала разложи их туда:

```bash
cp shard-split-report/*.{py,json,tsv,txt} /tmp/
cp shard-split-report/summary_rows.html /tmp/
cd /tmp

# полный отчёт -> SHARD_SPLIT_REVIEW.html
python3 build_html.py

# сводка -> SHARD_SPLIT_SUMMARY.html (аргумент = ссылка на опубликованный полный отчёт)
python3 mk_summary.py 'https://gist.githack.com/1IxI1/<gist-id>/raw/<sha>/shard-split-review.html'
```

Оба пишут результат в этот же каталог (`shard-split-report/`).

## Файлы

| файл | что это |
|---|---|
| `build_html.py` | генератор полного отчёта `SHARD_SPLIT_REVIEW.html` |
| `mk_summary.py` | генератор сводки `SHARD_SPLIT_SUMMARY.html`; страница собирается f-строкой, поэтому **литеральные фигурные скобки в CSS должны быть удвоены** |
| `build_charts.py` | пересборка графиков из `final_verified.json` → `charts_svg.json` |
| `final_verified.json` | **основные данные**: 84 сплита + 50 событий с метриками и описаниями нагрузки |
| `final_causes.json` | **вердикты по триггерам**: на каждый сплит primary / load_dim / secondary / confidence / evidence |
| `charts_svg.json` | готовые SVG графиков (TPS во времени, газ на блок) |
| `summary_rows.html` | 84 готовые строки таблицы для сводки |
| `SHARD_SPLIT_REVIEW.html` | **собранный полный отчёт** (выход `build_html.py`) |
| `SHARD_SPLIT_SUMMARY.html` | **собранная сводка** (выход `mk_summary.py`) |
| `SHARD_SPLIT_TRIGGERS_VERIFIED.md` | построчная таблица триггеров по 84 сплитам |
| `SHARD_SPLIT_EPISODES_VERIFIED.md` | разбор эпизодов с описанием нагрузки |
| `gas.tsv`, `fp.tsv`, `examples.tsv`, `sessions.txt` | входы легаси-части `build_html.py`: 4 ссылки на tonviewer и 5 на Grafana. **Это заглушки**, восстановленные из ранее собранного HTML после чистки `/tmp`. Значения ссылок верные, но если понадобится менять легаси-блок (типы нагрузки, примеры транзакций) — нужны настоящие файлы |

## Публикация

Страницы выложены secret-gist'ом `5ede00e3cbc047c8e3683160ff07d153` и отдаются через
`gist.githack.com` (сам gist рендерит HTML как исходник). Ссылки — **с номером ревизии**:
безревизионный URL githack кеширует около суток и после обновления отдаёт старую версию.

```bash
gh gist edit 5ede00e3cbc047c8e3683160ff07d153 -a shard-split-review.html
gh api gists/5ede00e3cbc047c8e3683160ff07d153 --jq '.history[0].version'   # свежий SHA
```

Ссылка внутри сводки ведёт на закреплённую ревизию отчёта, поэтому **при обновлении отчёта
сводку надо пересобрать с новым SHA**, иначе кнопка внизу тихо отстаёт на версию.

## Проверки после сборки

```bash
python3 - <<'EOF'
import re
D='/Users/victor/Projects/jetton-bomb/shard-split-report/'
for f in ('SHARD_SPLIT_SUMMARY.html','SHARD_SPLIT_REVIEW.html'):
    s=open(D+f,encoding='utf-8').read()
    css=s[s.index('<style>'):s.index('</style>')]
    rows=len(re.findall(r'<tr class="r-',s)) or len(re.findall(r'<td class="mono cause-td">',s))
    print(f, len(s), 'строк', rows, 'CSS', css.count('{'), css.count('}'),
          'p', len(re.findall(r'<p[ >]',s)), len(re.findall(r'</p>',s)))
EOF
```

Строк таблицы должно быть ровно 84, скобки CSS обязаны сходиться — незакрытая скобка
превращает остаток стилей в CSS-вложенность, и половина оформления молча пропадает.
