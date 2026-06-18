# DragonSlayer — handoff брифинг для новой сессии

## TL;DR — что делать сейчас

Открытый PR #9 (`slayer3d/dmg-indicator-and-sgs`). В нём УЖЕ:
- Damage indicator под прицелом (taken + dealt + fallback на DeathMsg + звук > threshold)
- SGS auto-strafer для мобильных (`+slayer_sgs` / `slayer_sgs` + 6 cvar-ов)
- Skip Steam default-silhouette хеша в аватарках
- `Documentation/slayer-cvars.md`

Пользователь хочет **по очереди** ещё три задачи, в этом же PR:

1. **Scoreboard polish**: гладкие quarter-circle углы + мёртвые игроки в конец списка команды + team banners (цветные плашки-разделители команд)
2. **`Slayer_Scoreboard_OnConnected` хук**: prefetch аватарок на `ca_active` из `cl_main.c::CL_CheckClientState`
3. **R8 proguard keep rules** в `android/app/proguard-rules.pro` для `XashActivity.downloadAvatar`, `XashActivity.startSteamLogin`, `SteamAPIHelper.fetchBatchAvatars`

Делать **строго по одной** задаче за раз, ждать подтверждения пользователя между ними. После каждой — один commit, один push в ту же ветку, БЕЗ создания нового PR (PR #9 уже есть).

---

## Состояние репо (важно, learnings врут)

- Origin: `1997-berserk/DragonSlayer` (private, Xash3D Android fork «Slayer3D»)
- Локальный путь: `/projects/sandbox/DragonSlayer`
- `master` сейчас **только PR #4 + #5**. Никаких PR #7 / #8 в master нет — это устаревшие learnings. Всё что они описывают (smooth corners, dead-last, team banners, OnConnected hook, R8 keep rules) — **никогда не было смерджено** и сейчас ОТСУТСТВУЕТ в коде.
- Текущая активная ветка: `slayer3d/dmg-indicator-and-sgs` = PR #9 (открыт)
- Перед началом: `git checkout slayer3d/dmg-indicator-and-sgs` и сверь HEAD с `gh pr view 9 --json headRefOid`. Если ремот ушёл вперёд — `git fetch && git reset --hard FETCH_HEAD` (см. ниже recovery recipe).

---

## Архитектура для текущих задач

### Scoreboard (задача 1)

`engine/client/cl_scoreboard_slayer.c` — главный файл (~1182 строк):
- `slayer_border_corner_segs[10]` (строка ~108) — таблица staircase-углов 5×4-px. Заменить на quarter-circle inset table (10×2-px шаги дают visually smooth закругление радиуса 20px).
- `Slayer_Scoreboard_Draw()` (строка ~653) — рендер. Background strips и border template **синхронны**. Если меняешь одно — обязательно меняй другое (см. learning «staircase corner rendering»).
- Перед сменой strip-таблицы выведи на бумаге какой контур получится: (a) BG strips, (b) border template, (c) horizontal cap width = `board_w − 2×first_inset`, (d) body wall y-range.
- `Slayer_SortCompare()` (строка ~640) — добавить второй ключ: `flags & 1 (dead)` → мёртвые ниже живых внутри той же команды.
- Team banner: цветной row-фон высотой `row_h`, шириной `board_w - 8`, с alpha ~80, перед заголовком команды (там где сейчас `drawn_ct_header` / `drawn_t_header` ставится). Сразу за фоном — текст "Counter-Terrorists  count/total" / "Terrorists  count/total".

### Avatar prefetch (задача 2)

- `Slayer_Scoreboard_OnConnected()` — новая функция в `cl_scoreboard_slayer.c` + декларация в `.h`.
- Тело: вызвать `Cbuf_AddText("status\n")` чтобы получить SteamID для всех игроков, выставить `slayer_status_pending = true; slayer_status_deadline = host.realtime + 30.0;` (а не 5s — см. learning «pre-warm hook» — на mobile cellular ответ может прийти позже 5s) и `slayer_status_next_time = host.realtime + 30.0;`. **Не** ставить deadline короче throttle — это lock-out trap.
- Hook в `engine/client/cl_main.c::CL_CheckClientState()` сразу после `cls.state = ca_active;`. Один раз за подключение.
- Не использовать `Slayer_AvatarDownload_Request` напрямую тут — статус ответ сам триггерит загрузку через `Slayer_ParseStatusLine`.

### R8 proguard (задача 3)

- `android/app/proguard-rules.pro` — добавить keep rules для:
  - `su.xash.engine.XashActivity { static int downloadAvatar(java.lang.String, java.lang.String); }` (вызывается из `cl_avatar_download.c` через `GetStaticMethodID`)
  - `su.xash.engine.XashActivity { static void startSteamLogin(); }` (из `cl_steam_login.c`)
  - `su.xash.engine.SteamAPIHelper { static int fetchBatchAvatars(java.lang.String, java.lang.String, java.lang.String); }` (из `cl_steam_api.c::FindClass`)
- Если `proguard-rules.pro` уже содержит общие `-keep`, добавлять только новые методы. Не дублировать.
- Без этого release-сборка с `isMinifyEnabled=true` молча отрубает аватарки/Steam-логин (debug работает потому что minification выключен).

---

## Правила работы (no exceptions)

### Git
- **НИКОГДА не используй `git push` напрямую.** Только `mcp_sandbox_github_push_to_remote` с **абсолютным** путём `/projects/sandbox/DragonSlayer`.
- Перед push: `mcp_sandbox_github_list_pull_requests` чтобы убедиться что PR #9 ещё открыт.
- **Один push на задачу.** push и create_pull_request НЕ в параллель — это даёт два разных headSha (см. learning).
- PR #9 УЖЕ ОТКРЫТ. Не создавать новый — просто пушить commit-ы в ту же ветку.
- Никаких force-push на shared PR-ветке. Если remote разошёлся: `git fetch → save HEAD → git reset --hard FETCH_HEAD → cherry-pick saved`. См. learning «sandbox quirk».

### Сборка
- Полный waf build НЕ работает в sandbox (SDL2-devel не доступен в Amazon Linux 2023 repos, --dedicated не компилит client/*).
- Smoke-test для client/*.c файлов:
  ```
  gcc -fsyntax-only -DENGINE_DLL=1 -DXASH_BUILD_COMMIT=\"test\" \
    -I . -I engine -I engine/common -I engine/client -I engine/client/vgui \
    -I engine/server -I engine/common/imagelib -I engine/common/soundlib \
    -I engine/platform -I public -I common -I filesystem -I pm_shared \
    -I 3rdparty/library_suffix/include -I 3rdparty/MultiEmulator/include \
    engine/client/<файл>.c
  ```
  Без `-DENGINE_DLL=1` cvar.h не определит `convar_t`.
- Перед первым syntax-check может потребоваться `git submodule update --init --recursive`.
- Финальная проверка — это CI на push (`.github/workflows/c-cpp.yml` собирает Android APK через `pull_request` trigger). НЕ модифицируй workflow — `push` там filtered только на `master`, что правильно (см. learning).

### Стиль ответа
- Русский если пользователь пишет по-русски.
- Кратко: 1) что сделано одной строкой, 2) bullet-список изменений (file path + 1 предложение), 3) verification только если build реально запускался, 4) closing line.
- Никаких header-ов, мотивашек, повторов запроса пользователя.
- Длинная проза только когда пользователь сказал «объясни» или есть non-obvious design decision.

### Learnings
- После каждой задачи сохраняй **один** durable insight через `create_feedback_learning`. Что капчурить: bug pattern + root cause + правило предотвращения. Что НЕ капчурить: код, one-off детали, текущий feature scope.
- Перед созданием — `get_learnings` чтобы избежать дубликатов.

---

## Известные ловушки

1. **Master не содержит PR #7 / #8.** Не доверяй learning-ам что упоминают эти PR. Проверка: `mcp_sandbox_github_get_merged_pull_requests`.
2. **`bld.path.ant_glob('client/**/*.c')`** в `engine/wscript` сам подхватит новые .c файлы. Ничего регистрировать в build не надо.
3. **5s parse window + 30s throttle = lock-out** на медленной мобильной сети. Окно ≥ throttle.
4. **R8 не видит JNI lookup.** Любой Java символ что вызывается ТОЛЬКО из C через `FindClass`/`GetStaticMethodID` обязательно нужен в `-keep`.
5. **Steam default avatar SHA1**: `fef49e7fa7e1997310d705b2a6158ff8dc1cdfeb`. Skip УЖЕ реализован в текущем PR.
6. **`voice_scale` cvar бесполезен для усиления голоса** (ограничен 255, см. learning). Для будущих voice-задач масштабировать сэмплы.

---

## Текущие работающие cvars и команды (для контекста — не трогать)

См. `Documentation/slayer-cvars.md` (в этом же PR). Ключевые группы:
- `slayer_thirdperson*`, `slayer_cam_*` — третье лицо + free-look камера
- `slayer_killsound*` — звук на убийство
- `slayer_chat_color*` — цвет чата
- `slayer_ducktap`, `slayer_autostrafe`, `slayer_autojump` — movement tweaks
- `slayer_smooth_zoom*` — плавный FOV
- `slayer_scoreboard*` — табло (то что сейчас полируем)
- `slayer_steam_apikey` — Steam Web API ключ
- `slayer_avatar_download` — мастер-вкл аватарок
- `slayer_damage_indicator*` — damage indicator (новое в PR #9)
- `slayer_sgs*` + `+slayer_sgs/-slayer_sgs` — SGS (новое в PR #9)

Ничего из этого списка пользователь сейчас менять не просит.
