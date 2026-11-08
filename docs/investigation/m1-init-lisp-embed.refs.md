# 参照マップ: M1 init.lisp 同梱(実装担当・レビュー担当向け)

- 作成日: 2026-11-08
- 対応する論理要約: `docs/investigation/m1-init-lisp-embed.logic.md`
- 事実には行番号を添えた。実測していない推論は **★ 未検証** と付けた
- **読み取りのみ。QEMU も `make build` も実行していない**

---

## 0. 論理要約の項目 ↔ 実装箇所 対応表

| 論理要約の項 | 実装箇所 |
|---|---|
| 2. 中核層 | `src/c/runtime.c`、`src/c/eval.c`、`src/c/za.c` |
| 2. あらかじめ機械語化された層 | `src/c/lisp_compiled.c`(生成物)、`src/lisp/init_aot.lisp`、`src/lisp/utility.lisp`、`src/lisp/device.lisp`、`src/lisp/ide.lisp`、`src/lisp/partition.lisp`、`src/lisp/mount.lisp`、`src/lisp/file-node.lisp`、`src/lisp/fat16.lisp`、`src/lisp/fat32.lisp`、`src/lisp/file-cmd.lisp`、`src/lisp/bench_aot.lisp` |
| 2. 基盤ライブラリ | `src/lisp/init.lisp`(1337 行 / 72,220 byte) |
| 2. 補助ライブラリ | `src/lisp/disassemble.lisp`(143 行 / 7,250 byte) |
| 2. 読み込み作業 | `src/c/load.c:42-108`(`cc_load`) |
| 2. 入力元の種類 | `src/c/stream.h:149`(9p) / `src/c/stream_lisp.c:150`(mount 経由) / `src/c/stream.h:203`・`src/c/stream.c:325-331`(文字列) |
| 2. 対話セッション | `src/c/process.c`、`src/c/process.h:8`(`PROCESS_COUNT`)、`src/c/repl.c:37-80` |
| 2. 固定枠 | `src/c/runtime.c:1926`(`IMM_SPACE_SIZE` = 16MB)、`src/c/runtime.c:1928`(`g_imm_space`) |
| 2. 試験の台本 | `src/c/kernel.c:85`(`g_qemu_test_boot_script`)、`test/lisp/qemu_boot_*.lisp` |
| 3. 起動手順 | `src/c/kernel.c:98-209`(`kernel_main`) |
| 3. 自動読み込みの間接化 | `src/c/process.c:307-322`(`g_qemu_test_entry` / `os_set_qemu_test_mode` / `process_trampoline_c`)、`src/c/kernel.c:190` |
| 4. 失敗の表し方 | `src/c/load.c:48-56`・`:68-71`・`:73-79`(中止)、`src/c/load.c:90-103`(続行) |
| 5. ルール 1(作業領域) | `src/c/process.c:13-29`、`src/c/process.h:112`(`PROCESS_STACK_SIZE` = 256KB)、`src/c/kernel.c:169-171` |
| 5. ルール 2(整理への追随) | `src/c/load.c:58-63` |
| 5. ルール 3・4(やりかけの巻き戻し) | `src/c/load.c:81-103` |
| 5. ルール 5(補助ライブラリ) | `src/lisp/init.lisp:1334-1337` |
| 5. ルール 6(大域状態の初期宣言) | `src/lisp/init.lisp:1218-1230`、`src/lisp/init_aot.lisp:340-348`・`:563-564` |
| 5. ルール 7(固定枠は不滅) | `documents/constraint-jit-code-pages-are-immortal.md`、`src/c/runtime.c:2454-2468` |
| 5. ルール 8(条件の逃げ道) | `src/c/runtime.c:5248-5273` |
| 5. ルール 9・10(文字列入力元) | `src/c/stream.c:325-331`・`:426-432`、`src/c/runtime.c:3295-3311` |
| 5. ルール 11(刻みの起点) | `src/c/interrupt.c:504`・`:522-524`・`:634-636`、`src/c/kernel.c:208` → `src/c/process.c:395` |
| 6. 状態 (a)/(b) | `src/c/runtime.c:5191-5212`(経緯)、`test/lisp/qemu_boot_no_init_signal.lisp:1-21` |
| 7. 外部連携 | §E / §F の表を参照 |
| 8. 制約 | §A / §C / §F を参照 |
| 9. 問題点 | §B-7 / §E-3 / §G を参照 |

---

## A. 生成物の作法(issue #180 の材料)

### A-1. 生成しているルール(事実)

| 行 | 内容 |
|---|---|
| `Makefile:57-62` | 作法の説明コメント(「git 管理対象外」「入力 Lisp が変わった時だけ再生成」) |
| `Makefile:63` | `TRANSPILE_LISP_SRC` = 生成の**入力一覧**(13 ファイル) |
| `Makefile:64` | `LISP_COMPILED = $(SRCDIR)/lisp_compiled.c` |
| `Makefile:65` | `LISP_COMPILED_FIXTURE = $(TESTDIR)/lisp_compiled_fixture.c` |
| `Makefile:66` | `SRC` の末尾に `$(LISP_COMPILED)` を含める(= カーネルにリンクされる) |
| `Makefile:69` | `SRC_NO_SYMTAB = $(filter-out $(SRCDIR)/disasm_symtab.c,$(SRC))` |
| `Makefile:120` | `OBJ = $(patsubst $(SRCDIR)/%.c,$(BUILD_TMPDIR)/%.o,$(SRC))`(`compile` 用。自動で追随) |
| `Makefile:229-231` | **生成ルール本体**。`$(LISP_COMPILED): $(TRANSPILE_LISP_SRC)` |
| `Makefile:239-241` | 同じ内容の `$(LISP_COMPILED_FIXTURE)` のルール(重複を承知で独立ルールにしている理由が `:233-238` にある) |
| `Makefile:243` | `transpile: $(LISP_COMPILED) $(LISP_COMPILED_FIXTURE)` |
| `Makefile:313` | `$(TARGET): $(SRC_NO_SYMTAB) $(HDR) $(GC_DEBUG_STAMP)` ← `SRC` 経由で生成物が依存に入る |
| `Makefile:388-389` | `print-transpile-src`(入力一覧を Makefile から直接取る口) |
| `Makefile:678` | `clean` が `rm -f ... $(LISP_COMPILED) $(LISP_COMPILED_FIXTURE)` |

**依存宣言の要点**(`Makefile:224-228` のコメント):
以前は phony な `transpile` への無条件依存だったため、`lisp_compiled.c` の mtime が毎回更新され、
`$(TARGET)` と `BOOT_FAT32_IMG` が常に再ビルドされていた。**ファイル依存へ直したのが現在の形。**

### A-2. `.gitignore`(事実)

```
.gitignore:8   src/c/lisp_compiled.c
.gitignore:9   test/c/lisp_compiled_fixture.c
.gitignore:13  src/c/disasm_symtab.c   ← 別の生成物(別の作り方)
```

### A-3. 生成を実行するコマンドの形(事実)

`Makefile:230-231`(`:240-241` も同文):

```
docker run --rm --user "$(id -u):$(id -g)" --entrypoint bash -v "$(PWD)":/workspace isiki-builder \
    -c 'ros run --load src/lisp/transpile.lisp --eval "(main-or-die)" --quit'
```

- **docker**(`isiki-builder` イメージ、`Makefile:216-217` の `image` ターゲットで作る)
- 中身は **roswell / SBCL** でホスト側 Common Lisp 上の `src/lisp/transpile.lisp` を走らせる
- 1 回の実行で 2 ファイルを同時に生成する(`Makefile:233-238`)
- 実測 1 回 0.86 秒、ゲスト不要(`docs/plans/compiler-optimization.md:179-180`)

**対比**: ホストで直接走る生成/検査もある。例 `Makefile:528`付近の `@python3 tools/check_eval_error_budget.py`、
`Makefile:318-321` の `sh tools/gen_disasm_symtab.sh`。
**★ 未検証(判断)**: `init.lisp` → バイト配列は Lisp の意味解析が不要なので、
docker/roswell を使わず `tools/` のホスト側スクリプト(`python3` か `sh`)で生成するほうが
既存の `disasm_symtab.c` の作法に近く、生成が軽い。トランスパイラの入力一覧
(`TRANSPILE_LISP_SRC`)に `init.lisp` を足すのは**誤り**(トランスパイラの対象が変わってしまう)。

### A-4. `src/c/init_lisp_src.c` を同じ作法で足すときに直す箇所(具体一覧)

| # | ファイル:行 | 直し方 |
|---|---|---|
| 1 | `Makefile:65` の直後 | `INIT_LISP_SRC = $(SRCDIR)/init_lisp_src.c` と、入力一覧 `INIT_LISP_EMBED_SRC = src/lisp/init.lisp`(補助ライブラリも同梱するなら `src/lisp/disassemble.lisp` も)を定義 |
| 2 | `Makefile:66`(`SRC`) | 末尾に `$(INIT_LISP_SRC)` を追加。これで `OBJ`(`:120`)・`SRC_NO_SYMTAB`(`:69`)・`$(TARGET)`(`:313`)の依存に自動で入る |
| 3 | `Makefile:243`(`transpile`)の近傍 | `$(INIT_LISP_SRC): $(INIT_LISP_EMBED_SRC)` の生成ルールを新設。**`:229-231` と同じ「入力ファイルへのファイル依存」にすること**(phony 依存にすると `:224-228` の事故が再発する) |
| 4 | `Makefile:212`(`.PHONY`) | 生成用の phony ターゲット(例 `embed-init`)を作るなら追記 |
| 5 | `Makefile:678`(`clean`) | `rm -f` の一覧に `$(INIT_LISP_SRC)` を追加 |
| 6 | `.gitignore:9` の次行 | `src/c/init_lisp_src.c` を追加 |
| 7 | `tools/check_eval_error_budget.py:151` | `GENERATED` 集合に `"init_lisp_src.c"` を追加。**この検査は `src/c/*.c` を走査する**(`:266`)。生成物は数えない方針(`:150`)に揃える。数えても `return g_sym_eval_error`(`:153` の正規表現)には当たらないので実害は無いが、作法として揃える |
| 8 | `Makefile:388-389` の近傍 | 入力一覧を出す `print-*` を足すなら追記(同一性比較の道具に渡す口。`tools/aot_identity.sh:63` が `print-transpile-src` を使う形と同じ) |
| 9 | `Makefile:388` 付近(任意) | 同一性比較(`tools/aot_identity.sh`)に載せるなら、`AOT_IDENTITY_FILES`(`tools/aot_identity.sh:61`)は環境変数で差し替えられるので、スクリプト本体は触らず Makefile 側に別ターゲットを足す形が取れる。**★ 未検証(判断)**: バイト配列はトランスパイラの出力ではないので、同一性比較の対象に含めるかは設計判断 |

**足す必要が無い箇所(重要)**:

- `Makefile:123` の `BUILD_FLAG_SIGNATURE` — **生成物は印に入れない。** 印はコンパイル行へ渡す
  **フラグ**だけを対象にしている(`:122-133`、`:91-121` のコメント)。
  生成物はファイル依存で追跡する設計(`:224-228`)なので、印に入れると二重管理になる
- `Makefile:303-311` の `KERNEL_LINK` — `$(SRC)` を使っているので `SRC` に足せば自動で入る
- `.github/workflows/ci.yml` — `make build` / `make test` / `make test-qemu-milestone` しか呼んでいないので変更不要
- `Dockerfile` — ホスト側スクリプトで生成するなら変更不要

**テスト用の結合リストに足す必要があるか**:
`Makefile:138-209` の `TEST_SRC_*` に `$(SRCDIR)/kernel.c` は**1 つも入っていない**(確認済み)。
したがって、同梱配列を参照するのが `kernel.c` だけなら **テスト用リストの変更は不要**。
逆に `process.c` から参照すると、`process.c` を含む 16 本の `TEST_SRC_*`
(`Makefile:138, 141, 144, 147, 150, 153, 156, 159, 162, 168, 171, 174, 177, 181, 185, 190, 196`)
**全部に `$(INIT_LISP_SRC)` を足すことになる**。§C-4 の設計上の結論に直結する。

### A-5. ビルドのスタンプ/署名との関係(事実)

- `Makefile:122-133`: `GC_DEBUG_STAMP = tmp/.gc-debug-flags`、
  `BUILD_FLAG_SIGNATURE = $(GC_DEBUG_FLAGS) $(ALIGN_AUDIT_FLAGS) $(JIT_DUMP_FLAGS) $(SMALL_HEAP_FLAGS) $(HEAP_EXHAUSTION_FLAGS) $(EXTRA_CFLAGS)`
- `Makefile:91-121`: 「新しいビルドフラグは必ず署名に入れる」を例外なしの規則にしている。
  実際に踏んだ事故(2026-09-17、`EXTRA_CFLAGS` が署名の外だったため 8byte 版のバイナリを測った)が記録されている
- `tools/aot_identity.sh:43-48`: 同一性比較でも印を突き合わせ、違えば終了コード 2 で止める
- **結論**: 生成物そのものは署名に絡まない。**ただし M1 で新しいビルドフラグを足す場合は
  `Makefile:123` に足す義務がある**(例「同梱を読まない」切り替えフラグを設けるなら必須)

---

## B. `cc_load` の輪の切り出し(issue #181 の材料)

対象: `src/c/load.c`(全 112 行)、`src/c/load.h`(全 20 行)

### B-1. 関数全体の構造(事実)

| 行 | 役割 | 切り出し後の所属 |
|---|---|---|
| `load.c:10-16` | 定数 3 つ(`LOAD_PATH_MAX` 256 / `LOAD_ERR_MSG_MAX` 128 / `LOAD_REPORT_MAX` 256) | `LOAD_PATH_MAX`・`LOAD_ERR_MSG_MAX` は `cc_load` 側、`LOAD_REPORT_MAX` は輪側 |
| `load.c:18-40` | `print_load_abort(path, condition, env)` — 打ち切りの理由表示 | **輪側**(`path` を名前として受ける) |
| `load.c:42-44` | 引数のパス取り出し | `cc_load` 側 |
| `load.c:46-56` | 入力元を開く + 開けなかったときの signal | `cc_load` 側(**ここが差し替え点**) |
| `load.c:58-63` | `GC_PROTECT(env)` と理由コメント | **輪側** |
| **`load.c:65-104`** | **これが「輪」** | **輪側** |
| `load.c:106-107` | 閉じる + `g_sym_t` を返す | **★ 未検証(判断)**: 閉じる責務を輪に持たせるか呼び出し側に残すかは設計判断。メモリ同梱では閉じる必要が実質無い(`stream.c:426` 付近の文字列入力は後始末が無い) |
| `load.c:110-112` | `os_register_load()` | 変更不要 |

**輪の中身の内訳**(`load.c:65-104`):

```
66      1 式読む
68-71   構文エラー → 閉じて signal(**早期 return**)
73-79   nil かつ error → 閉じて signal(**早期 return**) / nil かつ正常 → break
86-88   proc->env を退避(GC_PROTECT つき)
90-92   評価 + 結果の GC_PROTECT
93-103  aborted なら proc->env を戻し、理由を表示(**ここでは return しない**)
```

### B-2. 輪の中で `path` を使っている箇所(全数、事実)

| 行 | 使い方 |
|---|---|
| `load.c:55` | `os_signal_io_error("load", path, err_msg, env)` — **輪の外**(open 失敗) |
| `load.c:70` | `os_signal_io_error("load", path, "syntax error", env)` — **輪の中** |
| `load.c:76` | `os_signal_io_error("load", path, "I/O error", env)` — **輪の中** |
| `load.c:102` | `print_load_abort(path, result, env)` — **輪の中** |
| `load.c:27-31` | `print_load_abort` が `"load: " + path + ": "` を出力 | |

つまり**輪の中で `path` が要る理由は「エラー文面に出す名前」だけ**であり、
ファイルを開く用途では一切使っていない。

**渡し方(推奨)**: `const char *name` として受ける。
- 9p 経路 → 従来どおり `path`(文面が 1 文字も変わらない。R7 の陰性対照が成立する)
- 同梱経路 → 固定の表示名(`"src/lisp/init.lisp"` にしておけば、既存の打ち切り表示を
  読んでいる人・道具から見て文面が変わらない。**★ 未検証(判断)**: ただし「同梱である」ことが
  表示から分からなくなるので、`"<embedded>src/lisp/init.lisp"` 等の設計判断が要る)
- `os_signal_io_error` の第 1 引数 `"load"`(`runtime.h:2157`)は `what` なので、
  同梱でも `"load"` に揃えるか別名にするかも設計判断

### B-3. `GC_PROTECT` と `proc->env` の退避・復元の根拠(コメント要約、事実)

**`GC_PROTECT(env)`(`load.c:58-63`)**:
`env` は輪の複数回の反復を跨いで再利用される。読み込んだ式の評価(`defun` 等でヒープ確保を伴う)の
途中で GC が走ると `env` の指す先(例: `global_environment`)が**再配置される**。
`GC_PROTECT` で毎回の GC に自動追随させないと、以後の全式が再配置前の古い番地
(転送マーカーで上書き済み)を参照する。

**`proc->env` の退避・復元(`load.c:81-85`、`:93-103`)**:
- REPL と同じく、エラーで打ち切られた式の中での `switch-environment`
  (`proc->env` の恒久的な書き換え)は無かったことにする
- `cc_load` 自身は `env` 引数のもとで評価するので `proc->env` を読まないが、
  `proc->env` は load 後の REPL に残るため、やりかけの切り替えを残さない
- **正常終了した式の `switch-environment` は従来どおり有効なままにする**
- `saved_proc_env` が 0 のとき(= まだ遅延生成されていなかった)は**戻さない**。
  0 へ書き戻すと、式の中で生成された process environment が捨てられ、
  次の `%%current-environment` が同名の環境をもう 1 つ `*environments*` へ足す
- `proc` は GC で動かないが、評価の中で GC が走っていれば `proc->env` は書き換わっているので、
  **`get_current_process()` で取り直してから戻す**(`load.c:98-100`)

対応する REPL 側の同じ扱い: `src/c/repl.c:49-55`・`:61-64`。

### B-4. 中断(abort)の扱い(事実)

- `os_eval_top_level_ex(form, env, &aborted)`(宣言 `src/c/eval.h:47`、説明 `:30-47`)
- `aborted` が立つのは `%abort-top-level` による打ち切り。
  `%abort-top-level` 自体は `src/lisp/init_aot.lisp:717-718`(AOT 側)にある
- `load.c:93-103`: 打ち切り時に `proc->env` を戻し `print_load_abort` で 1 行出す。
  **`return` しない → 次の式へ進む → 最終的に `g_sym_t` を返す**
- `print_load_abort` は `os_condition_report_cstr`(`src/c/eval.c:1363-1385`)を試し、
  得られなければ `os_print` に落とす(`load.c:34-38`)

> **【最重要】`cc_load` は打ち切り件数を返さない。**
> 計画 D1(`docs/plans/local-load-and-core-image.md` の「M1-T4」)の
> 「読み込み中の syntax error や signal で boot error にする」は、
> **syntax error(`load.c:68-71`)は捕まえられるが、式ごとの signal は捕まえられない**。
> signal は `os_eval_top_level_ex` の中で `%abort-top-level` に化け、
> `aborted` として「表示して続行」に吸収される。
> **切り出す関数に `int *out_aborted_count`(または合計戻り値)を足さないと D1 は実装できない。**

### B-5. `load.h` / 宣言の場所 / `os_register_load` との関係(事実)

- `src/c/load.h:15` `lisp_val_t cc_load(lisp_val_t args, lisp_val_t env);`
- `src/c/load.h:18` `void os_register_load(void);`
- `src/c/load.c:110-112` `os_register_load` は `os_set_function(os_make_symbol("LOAD"), ..., global_environment)` のみ
- 呼び出し元: `src/c/kernel.c:147`(`os_register_load()`)、`src/c/kernel.c:89`(`cc_load` を直呼び)
- 新しい関数の宣言は `load.h` に足す。`kernel.c:13` が既に `#include "load.h"` している
- **`load.h:6-14` の doxygen が古い**: 「エラー内容を表示し、`g_sym_eval_error` を返す」と
  書かれているが、現在は `os_signal_io_error` で signal を上げる(`load.c:49-55` のコメントが経緯)。
  切り出しのついでに直すべき(原則 12)

### B-6. 切り出しで壊しやすい点(名指し)

| # | 壊しやすい点 | 根拠 |
|---|---|---|
| 1 | **`GC_PROTECT(env)` を輪の外に置き忘れる** | `load.c:58-63`。症状は「2 つめ以降の全式が古い番地を見る」= 無言の破壊 |
| 2 | **`saved_proc_env == 0` の判定を落とす** | `load.c:94-101`。落とすと `*environments*` に同名環境が重複する |
| 3 | **`get_current_process()` の取り直しを `proc` 変数の再利用に変える** | `load.c:98-100`。`proc` 自体は動かないが `proc->env` は GC で書き換わる |
| 4 | **`GC_PROTECT(result)` を落とす** | `load.c:92`。`result` は `print_load_abort` に渡るまで生存する |
| 5 | **早期 return の 2 か所で `os_stream_close` を忘れる** | `load.c:69`・`:75`。輪を関数化すると閉じる責務の所在が曖昧になる |
| 6 | **`g_sym_read_error` と `nil` の判別順を入れ替える** | `load.c:68`・`:73`。`nil` は `TAG_CONS` を持つ(既知の罠)ので判定の形を変えない |
| 7 | **`stream.error` の確認を落とす** | `load.c:74`。`reader.h:20-21` に「I/O エラーはクリーン EOF と区別されない。呼び出し側が nil 受け取り後に `stream->error` を確認すること」と明記されている。**メモリ同梱では `error` は立たないが、関数は 9p と共有するので残す** |
| 8 | **エラー文面を変える** | `load.c:55`・`:70`・`:76`、`load.c:27-31`。`test/lisp/io_signal_test.lisp`(`qemu_boot_test.lisp:20` から読まれる)が文面・クラスを見ている可能性がある。**★ 未検証**: 中身は読んでいない |
| 9 | **`cc_load` 経由の既存ユニット試験を壊す** | `test/c/load_test.c:193-197` は `os_eval` で `LOAD` を呼ぶ形(= `cc_load` を通る)。7 本のテスト(`:199, 217, 227, 237, 275, 297, 312`)が 9p の fake(`:38-59`)に乗っている。**切り出しても `cc_load` が従来どおり振る舞えば全部通る**。新関数用のテストは同じファイルに足せる |
| 10 | **`print_load_abort` を `static` のまま輪の外に残す** | `load.c:27`。輪が別関数になると `static` の見える範囲に注意 |

---

## C. 読む場所(issue #182 の材料)

### C-1. `.qemu-test-trigger` の自動 load の経路(事実、行番号つき)

```
src/c/kernel.c:172-175   トリガーを 9p で開こうとする(os_stream_open_9p_file)
src/c/kernel.c:176-187   1 行目を g_qemu_test_boot_script[256] へ読む(空なら既定値のまま)
src/c/kernel.c:188       qemu_test_mode = 1
src/c/kernel.c:189       g_qemu_test_power_off = power_off
src/c/kernel.c:190       os_set_qemu_test_mode(run_qemu_boot_test)   ← 関数ポインタの登録だけ
src/c/kernel.c:196       os_set_panic_hook(power_off)
src/c/kernel.c:206       os_process_install_stack_guards()
src/c/kernel.c:208       process_scheduler_start()
   └ src/c/process.c:391-393  spawn(i) を PROCESS_COUNT 回
   └ src/c/process.c:395      enable_timer_irq()        ← ここで初めて tick が進み始める
   └ src/c/process.c:398-400  for(;;) hlt
タイマー割り込み → 偽の IRETQ フレーム(src/c/process.c:362-367)から
src/c/process.c:314-322  process_trampoline_c(proc_index)
   316: if (proc_index == 0 && g_qemu_test_entry != 0)
   317:     g_qemu_test_entry();                        ← ここで実行される
   319-321: for(;;) os_repl_step(proc);
```

`g_qemu_test_entry` の実体 = `src/c/kernel.c:88-96` `run_qemu_boot_test`:

```
kernel.c:89  cc_load(os_make_cons(os_make_string(g_qemu_test_boot_script), nil), global_environment);
kernel.c:90-94  ALIGN_AUDIT=1 ビルドのみ os_align_audit_report()
kernel.c:95  g_qemu_test_power_off();
```

**`cc_load` を呼んでいる箇所は `src/c/kernel.c:89` の 1 か所だけ**(`src/c/` 内で他に無い)。

### C-2. その呼び出しが乗っている前提(事実)

| 前提 | 根拠 |
|---|---|
| **プロセス 0 専用スタック(256KB)の上で走る** | `process.c:332`(`stack_top`)、`process.h:112`、`kernel.c:169-171` |
| ブート時スタックで呼ぶとクラッシュする | `kernel.c:169-171`、`process.c:13-26`(初回フルロードで C 再帰が実測 35〜36KB) |
| **割り込みは有効** | `process.c:365` `iretq_frame[2] = 0x202`(IF=1) |
| **tick は 0 付近から数え始める** | `interrupt.c:504`・`:522-524`、`interrupt.c:634-636`(`enable_timer_irq`)は `process.c:395` で呼ばれる |
| スタックの下にガードページがある | `kernel.c:203-206`、`process.c:32-55`(ガード 64KB × 各スロット + 末尾ガード) |
| スタックのカナリアが設営済み | `process.c:331`(`stack_canary_init`)、検査は `process.c:252-281` |
| GC ルートは全部そろっている | `runtime.c:3520` 付近(`global_environment` / `g_dynamic_bindings` / `g_symbol_table` / 登録ルート / 全プロセスの shadow stack)。各プロセスの `env` と `live_blocks` は `process.c:422`・`:424` で登録済み |
| `proc->env` は**まだ 0**(未生成) | `process.c:421` で 0 に初期化。生成は `repl.c:38-39` か `runtime.c:10441-10442` の遅延生成 |
| フレームバッファは使える | `kernel.c:115-130`(`os_boot_alloc` より前に繰り上げ済み。`kernel.c:106-114` のコメント) |
| 他のプロセス(1〜3)も同時に走り出す | `process.c:391-393` が `PROCESS_COUNT` 個 spawn する。`PROCESS_COUNT = VBUF_COUNT`(`process.h:8`) |

### C-3. 埋め込みの読み込みを入れるべき位置(結論と理由)

**位置: `src/c/process.c:316-318` の `g_qemu_test_entry()` 呼び出しの直前。**

理由:

1. **`.qemu-test-trigger` の台本より前でなければならない。**
   台本(`test/lisp/qemu_boot_*.lisp`)は基盤ライブラリの定義(`let` 等のマクロ、条件クラス)を
   前提に書かれている。D2 で台本側の `(load "src/lisp/init.lisp")` を外す(§E)ので、
   **同梱の読み込みが台本より後だと台本が動かない**
2. **`os_repl_step` の輪より前でなければならない。** `process.c:319-321` に入ったら戻らない
3. **プロセス 0 のスタックの上でなければならない。** `kernel_main` のブート時スタックでは
   クラッシュする(F15/F16。`kernel.c:169-171`、`process.c:13-26`)
4. **`proc_index == 0` のときだけでなければならない。** `process.c:316` と同じ条件。
   全プロセスで走らせると同じものを 4 回読み込み、固定枠を 4 倍消費する(不滅)
5. `g_qemu_test_entry` の**中**(= `run_qemu_boot_test`)に入れてはいけない。
   あれは試験モードのときだけ登録される(`kernel.c:190`)ので、
   **通常の対話起動では呼ばれない** = G2 が達成できない

**実装の形(推奨)**: `process.c` に直接 `cc_load`/`os_load_from_stream` を書かず、
`os_set_qemu_test_mode`(`process.c:310-312`)と**同じ関数ポインタ登録の形**をもう 1 つ足し、
`kernel.c` 側(`kernel.c:197` 付近、トリガー判定の後・`kernel.c:199` の手前)から
無条件に登録する。

| 理由 | 根拠 |
|---|---|
| `process.c` は 16 本の `TEST_SRC_*` にリンクされている(§A-4) | `Makefile:138〜196` |
| そこへ `load.c` / 同梱配列 / `reader.c` 依存を持ち込むと 16 本のリンクリストを全部直すことになる | 同上 |
| 既存の `g_qemu_test_entry` がまさにこの境界を守るための仕掛けである | `process.c:307-312` と `kernel.c:190` |
| 登録が無いユニット試験では何も起きない(挙動が変わらない) | `process.c:316` と同じ `!= 0` 判定 |

### C-4. そこで使える env(事実)

- **`global_environment` を使う。** 既存の `kernel.c:89` が `cc_load(..., global_environment)` を
  渡しているのと同じ
- **`proc->env` は使えない**: その時点でまだ 0(`process.c:421`)。遅延生成は
  `repl.c:38-39` / `runtime.c:10441-10442`
- `global_environment` へ読み込む意味: 各プロセスの環境は `global_environment` の
  子として作られる(`runtime.c:4786`)ので、全プロセスから見える
- 利用手引き `README.md:23-29` が手で行っている 3 手
  (`(%%set-current-environment (%%global-environment))` → `load` → `(switch-environment 'f1)`)が
  **不要になる**。README の書き換えが要る
- `*environments*` の初期宣言(`src/lisp/init.lisp:1230` `(defdynamic *environments* (dynamic *environments*))`)は、
  未束縛でも安全: `os_get_dynamic`(`runtime.c:4857-4860`)は未登録なら `nil` を返す。
  現在の `kernel.c:89` も `os_repl_step` より前に走るので、**この並びは既に実地で通っている**

### C-5. `os_panic` を呼んで良い文脈か(事実)

**呼べる。制約は 3 つある。**

- `os_panic`(`runtime.c:1465-1484`、宣言 `runtime.h:2009`)は
  `get_active_frame_buffer()` → 文面 → `panic_write_imm_breakdown`(`runtime.c:2470-2483`)→
  フックがあれば呼ぶ → `for(;;) hlt`
- フレームバッファはこの時点で使える(`kernel.c:115-130`)
- 試験モードでは `os_set_panic_hook(power_off)`(`kernel.c:196`)が登録済みなので、
  **試験は固まらずに落ちる**(D1 の前提どおり)

| 制約 | 根拠 |
|---|---|
| **(1) 他のプロセスは止まらない。** 割り込みは有効(`process.c:365` IF=1)なので、`hlt` はタイマーで起きる。プロセス 1〜3 の REPL は動き続ける。通常起動では「画面に PANIC が出て、プロセス 0 だけ死んだ機械」になる | `runtime.c:1479-1483`、`process.c:365` |
| **(2) 表示先は「アクティブな」フレームバッファ。** 利用者がプロセス 1 を表示していれば PANIC は見えない | `runtime.c:1466`(`get_active_frame_buffer`) |
| **(3) ユニット試験ビルドでは `hlt` が消えて空の `for(;;)` になる = ハング。** ただし `process_trampoline_c` はユニット試験では実行されないので届かない | `runtime.c:1480-1482`、`Makefile:551` の `-DISIKIOS_UNIT_TEST` |

---

## D. 「init.lisp 未ロード時」の分岐(issue #183 の材料)

### D-1. 指定された 2 か所(事実)

| 場所 | 種別 | 何を分岐しているか |
|---|---|---|
| `src/c/reader.h:35` | **文書のみ**(doxygen) | `os_parse_number` が解析失敗時に返す値の説明。「通常はハンドラ経由でトップレベルへ abort するため到達しない。init.lisp 未ロード時は `g_sym_eval_error`」。**実際の分岐はここに無い**。実体は `runtime.c:5269-5273` |
| `src/c/eval.h:56` | **文書**(実体は `eval.c:1372-1374`) | `os_condition_report_cstr` が 0 を返す 3 条件のうち 1 つ。実体 `eval.c:1371-1374` は `os_get_function(%REPORT-CONDITION-STRING, env) == nil` で 0 を返す |

### D-2. 同種の分岐の全数(grep「未ロード / 読んでいない / 読まれていない」で網羅)

| 場所 | 種別 | 内容 | 同梱後 |
|---|---|---|---|
| `src/c/runtime.c:5269-5273` | **コードの分岐(本体)** | `make_instance_fn == nil` または `signal_condition_fn == nil` または(symbol なのに)`resolved_class == nil` → `g_sym_eval_error` | **到達可能**(D-3) |
| `src/c/runtime.c:5229-5232` | コードの分岐 | 通知の入れ子が `SIGNAL_MAX_DEPTH`(16)を超えたら同じ逃げ道へ | **到達可能**(番犬。目的が別) |
| `src/c/runtime.c:5288-5292` | コードの分岐 | `os_resolve_class`: `%find-class` が未定義なら `g_sym_eval_error` | **到達可能** |
| `src/c/eval.c:1371-1374` | コードの分岐 | `%REPORT-CONDITION-STRING` が未定義なら 0 | **到達可能** |
| `src/c/eval.c:1379-1381` | コードの分岐 | 言葉にする途中で脱出した / 戻り値が文字列でない → 0 | **到達可能**(未ロードと無関係) |
| `src/c/runtime.c:5030-5033` | コードの分岐 | **`init_aot.lisp` が**まだ読まれていない場合(`*CLASSES*` が intern されていない) | **無関係**。AOT 側の話なので残る |
| `src/c/runtime.h:2087` | 文書のみ | `os_signal_condition` の戻り値の説明 | 文面の直しが要る |
| `src/c/runtime.h:2211` | 文書のみ | `os_resolve_class` の戻り値の説明 | 文面の直しが要る |
| `src/c/runtime.c:5302`, `:5349`, `:5425`, `:5462` | 文書のみ | 各 signal ヘルパの戻り値の説明「init.lisp 未ロードの場合は `g_sym_eval_error`」 | 文面の直しが要る |
| `src/c/runtime.c:5191-5212`, `:5248-5257` | 文書(長い経緯) | 「手続きはあるのに条件クラスが 1 つも無い」食い違いの説明と無限再帰の実例 | 文面の直しが要る |
| `src/c/runtime.h:2743-2770` | 文書 + 定数 | `DEFAULT_FLOAT_FORMAT_IS_SINGLE` が「init.lisp を評価するまで」と「それ以降」の両方を兼ねる単一の真実源 | **変更不要**(同梱でも兼ねる関係は不変) |
| `src/lisp/init.lisp:1218-1230` | **Lisp 側の分岐** | `*environments*` の初期宣言が「既存値を書き戻す」形になっている(REPL が先に登録している場合に備える) | **残す必要がある**(§C-4 と §G-3) |

### D-3. 到達不能になるか(結論)

**どれも到達不能にはならない。** 根拠 3 つ:

1. **起動前半は常に「未ロード」である。** 同梱の読み込みは `process_trampoline_c`
   (`process.c:316-318` の位置)で走るので、`os_bootstrap`(`kernel.c:142`)〜
   `os_run_aot_toplevel_forms`(`kernel.c:155`)〜`initialize_processes`(`kernel.c:157`)の間に
   通知が起きれば、条件クラスは 1 つも登録されていない
2. **ネイティブのユニット試験は基盤ライブラリを読まない。**
   `test/c/load_test.c:250`(「init.lisp をロードしないテストなので、error や report-condition は使えない」)、
   `test/c/runtime_test.c:2276-2282`(`setup_small_heap` は `os_bootstrap` だけ)。
   これらは `process_trampoline_c` を走らせない
3. **「諦めの値を返す箇所」の残数を上限と下限の両方で突き合わせる検査がある。**
   `tools/check_eval_error_budget.py:138-145`(`BUDGETS = {"c_return": 8, "lisp_quote": 2}`)。
   `Makefile:548` の `test` が `check-eval-error-budget` を呼ぶ。
   **分岐を 1 つ消すと 8 → 7 になって落ちる**(`:299` 以降の ratchet)

**やるべきこと**: 削除ではなく**注記の書き換え**(原則 12)。
「init.lisp 未ロード時」→「条件クラスが登録される前(ブート前半)、およびネイティブのユニット試験」。
対象は上の表の「文書のみ」行すべて。

**壊れるもの**: `test/lisp/qemu_boot_no_init_signal.lisp`(§E-3)。
この試験だけは**状態 (a) のまま走ること自体が要件**なので、同梱を無条件にすると成立しない。

---

## E. プローブの load(issue #184 の材料)

### E-1. `(load "src/lisp/init.lisp")` を**実行として**書いている全ファイル(事実)

grep パターン `load "src/lisp/init.lisp"` の全件からコメント行を除いた結果。

#### test/lisp/(45 ファイル、各 1 回)

| ファイル | 行 |
|---|---|
| `qemu_boot_test.lisp` | 1 |
| `qemu_boot_m1_base.lisp` | 1 |
| `qemu_boot_m2_za.lisp` | 1 |
| `qemu_boot_m2_za_stress.lisp` | 1 |
| `qemu_boot_m3_za_ext5.lisp` | 1 |
| `qemu_boot_m4_environment.lisp` | 1 |
| `qemu_boot_m5_ide.lisp` | 1 |
| `qemu_boot_m6_fat16.lisp` | 1 |
| `qemu_boot_fat16_m0.lisp` | 1 |
| `qemu_boot_fat32.lisp` | 1 |
| `qemu_boot_fat32_primary_boot.lisp` | 1 |
| `qemu_boot_fat_callback_transfer.lisp` | 4 |
| `qemu_boot_partition.lisp` | 1 |
| `qemu_boot_perf_fat16.lisp` | 1 |
| `qemu_boot_perf_fat32.lisp` | 1 |
| `qemu_boot_perf_read_fat16.lisp` | 1 |
| `qemu_boot_perf_read_fat32.lisp` | 1 |
| `qemu_boot_read_file_native.lisp` | 1 |
| `qemu_boot_for_semantics.lisp` | 1 |
| `qemu_boot_for_expansion.lisp` | 1 |
| `qemu_boot_for_gc_stress.lisp` | 1 |
| `qemu_boot_error_escape_gc.lisp` | 20 |
| `qemu_boot_stack_guard.lisp` | 1 |
| `qemu_boot_stack_guard_gc.lisp` | 1 |
| `qemu_boot_heap_exhaustion.lisp` | 7 |
| `qemu_boot_imm_leak.lisp` | 1 |
| `qemu_boot_jit_limits.lisp` | 1 |
| `qemu_boot_jit_frame.lisp` | 1 |
| `qemu_boot_inline_phase0.lisp` | 11 |
| `qemu_boot_inline_phase0b.lisp` | 10 |
| `qemu_boot_inline_phase3.lisp` | 12 |
| `qemu_boot_inline_muldiv.lisp` | 2 |
| `qemu_boot_instcount_gate_check.lisp` | 14 |
| `qemu_boot_compare_single.lisp` | 2 |
| `qemu_boot_compare_fixnum.lisp` | 2 |
| `qemu_boot_safety3.lisp` | 2 |
| `qemu_boot_declare_audit.lisp` | 2 |
| `qemu_boot_declare_bench.lisp` | 1 |
| `qemu_boot_alloc_audit.lisp` | 2 |
| `qemu_boot_alloc_breakdown.lisp` | 3 |
| `qemu_boot_abi_bench.lisp` | 1 |
| `qemu_boot_bench_jit.lisp` | 1 |
| `qemu_boot_bench_interp.lisp` | 1 |
| `qemu_boot_bench_construct.lisp` | 1 |
| `qemu_boot_float_contagion_bench.lisp` | 1 |

#### tools/(7 箇所 / 6 ファイル)

| ファイル | 行 | 形 |
|---|---|---|
| `tools/bench/run_jit_bench.sh` | 127 | `echo '(load ...)'` で boot-entry を生成 |
| `tools/bench/run_interp_bench.sh` | 118 | 同 |
| `tools/bench/run_construct_bench.sh` | 88 | 同 |
| `tools/bench/run_paint_audit.sh` | **72, 105** | **2 箇所**。72 = 本番の boot 生成(`gen_boot`)、105 = ウォームアップ用の別スクリプト |
| `tools/gen_macro_parity_drivers.py` | 42 | 生成する boot-entry に埋め込む |
| `tools/gen_jit_nest_probe.py` | 41 | 同 |
| `tools/mandel_run.lisp` | 24 | 直接 |

#### src/ ・ドキュメント(コメント・手引きのみ。実行ではない)

| ファイル | 行 | 内容 |
|---|---|---|
| `src/lisp/init.lisp` | 1 | 「`(load "src/lisp/init.lisp")` が動くことを確かめる関数」というコメント |
| `src/c/process.c` | 15 | スタックサイズの根拠のコメント |
| `src/c/runtime.c` | 5196, 5251 | 「boot-entry が `(load ...)` を持たない場合」の説明 |
| `Makefile` | 999 | `qemu_boot_no_init_signal.lisp` が唯一の例外である旨のコメント |
| **`README.md`** | **26** | **利用手引きの手順(実行指示)。`:24` と `:29` の 2 手とセット** |
| `docs/plans/local-load-and-core-image.md` | 4 箇所 | 計画書 |
| `test/lisp/environment_utilities_test.lisp` | 103, 117, 123 | **すべてコメント**。「複数プロセスがそれぞれ load する想定の代用」の説明 |

### E-2. ファイルごとの出現回数まとめ

- **実行として 1 回ずつ**: `test/lisp/qemu_boot_*.lisp` 45 ファイル
- **2 回**: `tools/bench/run_paint_audit.sh`(別々の boot-entry を生成するので**両方必要**)
- **1 回**: `tools/bench/run_jit_bench.sh`、`run_interp_bench.sh`、`run_construct_bench.sh`、
  `tools/gen_macro_parity_drivers.py`、`tools/gen_jit_nest_probe.py`、`tools/mandel_run.lisp`、`README.md`
- **0 回(コメントのみ)**: `test/lisp/environment_utilities_test.lisp`、`src/c/*`、`Makefile`

### E-3. 外すと壊れそうなもの(名指し)

| # | 対象 | 何が壊れるか |
|---|---|---|
| **1** | **`test/lisp/qemu_boot_no_init_signal.lisp`** | **そもそも `load` を書いていない。これが要件である。** `:3-5` に「**このファイルは `(load "src/lisp/init.lisp")` を意図的に行わない。それがこの試験の要件である。他の boot-entry スクリプトを真似して init.lisp を足さないこと**」と明記。`:38-40` で `(null (%find-class '<domain-error>))` を前提として確認している。**同梱を無条件にすると、この試験は前提から崩れる。** 実行は `Makefile:1007`(`test-qemu-all` 内)のみ。`Makefile:997-999` に「他の milestone はすべて先頭で load するため、この状態はここでしか通らない」とある。**CI(`.github/workflows/ci.yml`)には入っていない** → 崩れても常時検査では気づかない |
| **2** | **`src/lisp/init.lisp:1337`** | **`(load "src/lisp/disassemble.lisp")`。基盤ライブラリ自身が末尾でホスト共有から別ファイルを読む。** `:1334-1336` に理由(while/format 依存)。**同梱しても、9p を外した起動ではこの 1 行が失敗する。** しかも `cc_load` の輪は「打ち切られた式は表示して続行」なので、**全体は成功(`g_sym_t`)を返したまま `disassemble` 系 21 定義が欠ける**(§B-4)。G2 の完了条件(`-fsdev` を外して起動)に直接当たる |
| **3** | `tools/bench/run_paint_audit.sh:72` と `:105` | **別々の boot-entry を作っている。** 片方だけ外すとウォームアップと本番で読み込み状態が食い違い、`:100-103` の「初回は所要時間が膨らむので捨てる」前提が壊れる。両方同時に扱うこと |
| **4** | `test/lisp/qemu_boot_fat_callback_transfer.lisp:4`、`qemu_boot_error_escape_gc.lisp:20`、`qemu_boot_heap_exhaustion.lisp:7`、`qemu_boot_inline_phase0.lisp:11`、`qemu_boot_inline_phase0b.lisp:10`、`qemu_boot_inline_phase3.lisp:12`、`qemu_boot_instcount_gate_check.lisp:14` | **行 1 ではない = 前に別の設定行がある。** 機械的に 1 行目を削る置換は使えない。**★ 未検証**: 前の行が load に依存しているかは個別に読む必要がある |
| **5** | `README.md:24-29` | 3 手セット(`%%set-current-environment` → `load` → `switch-environment 'f1`)。`load` だけ外すと前後 2 手が意味を失う。**手順まるごと書き換えが要る** |
| **6** | `test/lisp/environments_predefined_test.lisp:3-14` | 「REPL が init.lisp 読み込みより先に `*environments*` へ登録済みのことがある」ことの試験。`:12-14` に「qemu 実機起動(cc_load 経由で init.lisp を最初に load する)ではこの事前登録の…」とあり、**ネイティブ試験専用で `main()` から事前登録する形**。**★ 未検証**: 同梱で読み込み順が変わらない(どちらも `os_repl_step` より前)ので影響は無いと見るが、注記の文面は古くなる |
| **7** | `tools/gen_macro_parity_drivers.py:42`、`tools/gen_jit_nest_probe.py:41` | **生成器**なので、生成される boot-entry 側ではなくここを直す |
| **8** | `test/lisp/environment_utilities_test.lisp:103-123` | コメントのみ。ただし `:117` と `:123` が「従来の `(load "src/lisp/init.lisp")` とは異なり、重複無しに共有できている」と**旧挙動を基準に説明している**。注記の更新対象 |

---

## F. 計測の手立て(issue #185 の材料)

### F-1. Immobilized Space の使用量を読む手段(事実)

| 層 | 口 | 場所 |
|---|---|---|
| **Lisp** | `(%%imm-space-used-bytes)` | 実装 `src/c/runtime.c:1991-1995`、登録 `src/c/runtime.c:3958` |
| **Lisp** | `(%%imm-space-total-bytes)` | 実装 `src/c/runtime.c:1979`(= `IMM_SPACE_SIZE`)、登録 `src/c/runtime.c:3957` |
| **Lisp** | `(room)` が両方を整形して出す | `src/lisp/utility.lisp:68-74`。**`utility.lisp` は AOT 対象**(`Makefile:63`)なので**基盤ライブラリ無しでも使える** |
| **C** | `os_imm_space_used_bytes()` | 宣言 `src/c/runtime.h:2002`、実装 `src/c/runtime.c:2439-2452`。**ページ粒度の bump から各スロットカーソルの未使用末尾を差し引くのでバイト粒度** |
| **panic の表示** | `used= / total= byte`、`cursor fn_meta: offset=`、`function_cell: offset=`、`last request=` | `src/c/runtime.c:2470-2483`(`panic_write_imm_breakdown`)、`os_panic` から `runtime.c:1470` で常に呼ばれる |
| 定数 | `IMM_SPACE_SIZE = 16 * 1024 * 1024` | `src/c/runtime.c:1926` |
| 既存の試験 | `test/lisp/room_test.lisp:43`、`test/lisp/imm_leak_test.lisp`(`test/lisp/qemu_boot_imm_leak.lisp:1-6` から読む) | |
| 登録は bootstrap 時 | `runtime.c:3957-3958`(`os_bootstrap` 内) | **基盤ライブラリ未読込でも呼べる** |

> **これが意味すること**: 同梱の読み込みの**直前と直後**で `%%imm-space-used-bytes` を取れば差分が出る。
> ただし「直前」を Lisp から取るには、読み込みより前に Lisp を評価する口が要る。
> C 側の `os_imm_space_used_bytes()` を同梱読み込みの前後で呼んで直列線路へ出すのが素直。
> **★ 未検証(判断)**。

### F-2. ヒープ使用量を読む手段(事実)

| 層 | 口 | 場所 |
|---|---|---|
| Lisp | `(%%heap-used-bytes)` | 実装 `src/c/runtime.c:1745-1753`、登録 `src/c/runtime.c:3936` |
| Lisp | `(%%heap-total-bytes)`(From 空間 = ヒープ全体の半分) | `src/c/runtime.c:1733`、登録 `:3935` |
| Lisp | `(room)` | `src/lisp/utility.lisp:68-72`(AOT 済み) |
| C | `os_heap_used_ratio()` | `src/c/repl.c:77` が GC 発火の判定に使っている |
| 桶別の内訳 | `ALLOC_AUDIT`(`ALIGN_AUDIT` とは別) | `src/c/runtime.c:574` に「合計は `%%HEAP-USED-BYTES` と必ず一致する」と明記。`Makefile` の `test-qemu-alloc-audit` / `test-qemu-alloc-breakdown`(`.PHONY` は `Makefile:212`) |
| 既存の試験 | `test/lisp/room_test.lisp`、`test/lisp/qemu_boot_alloc_breakdown.lisp` | |

### F-3. 「ブートから REPL が入力を受けるまで」を測れる既存の仕掛け(事実 + 判断)

**無い。** 根拠:

1. **tick は割り込み許可より後しか数えない。**
   `src/c/interrupt.c:504`(`g_tick_counter = 0`)、`:522-524`(`c_timer_switch` で `++`)、
   `:634-636`(`enable_timer_irq` で IRQ0 のマスクを外す)。
   `enable_timer_irq` は `src/c/process.c:395`(`process_scheduler_start` の中)で呼ばれ、
   これは `src/c/kernel.c:208` = **起動手順の最後**。
   したがって `kernel.c:104`〜`:206`(boot alloc / `os_bootstrap` / 各 `os_register_*` /
   `os_run_aot_toplevel_forms` / `initialize_processes` / IDT 等)は**刻みに 1 も入らない**
2. `GET-INTERNAL-REAL-TIME` はその tick をそのまま返す。
   `src/c/clock.c:19`・`:38`、`src/c/clock.h:17`。刻みは **100Hz**(`src/c/clock.c:7` `TICKS_PER_SECOND 100`)= 分解能 10ms
3. **処理器の周期計数(rdtsc)を使っている既存コードは `src/c/` に 1 つも無い**(grep 済み)
4. 経過を出す既存の仕掛けは Lisp 側の `isiki-test-mark` / `isiki-test-load`
   (`test/lisp/test_framework.lisp:38, 46-51, 69-70`)。ラベルと「今の tick」「前のマークからの差」を出す。
   **これは台本が走り始めてからの話**
5. `src/c/interrupt.c:510-541` に tick ごとのサンプラ(`g_tick_sample_interval`)があるが、
   これは「無音のハングでどこを回っているか」を出す調査用

**何を足せば測れるか(★ 未検証・判断)**:

| 案 | 内容 | 覆う範囲 |
|---|---|---|
| (i) | 同梱読み込みの**直前と直後**で `get_tick_counter()` を読み、直列線路へ出す | **同梱読み込みのぶんだけ**。M1-T6 の主目的(「init.lisp の読み込みに何 tick か」)はこれで足りる |
| (ii) | `process_trampoline_c` で `os_repl_step` の輪へ入る直前に 1 回出す | 「割り込み許可 → 対話受付」まで |
| (iii) | 起動前半を含めたいなら、PIT のカウンタを直読みするか周期計数を足す | 全体。**新しい機構が要る** |
| (iv) | 外から測る(QEMU の起動から最初の出力までの壁時計) | 全体。既に `tools/bench/*.sh` が `elapsed_sec` を測る形を持っている(`tools/bench/run_paint_audit.sh:64` の `summary.tsv` の列) |

### F-4. `test/c/runtime_test.c` の `SMALL_HEAP_SIZE`(事実)

- **現在値: `85 * 1024` = 87,040 byte**。`test/c/runtime_test.c:2272-2274`(`#ifndef SMALL_HEAP_SIZE`)
- 使う場所: `test/c/runtime_test.c:2276-2282`(`setup_small_heap`。`malloc` → `os_heap_init` →
  `os_reset_runtime_state_for_test` → `os_bootstrap`)
- 記録されている実測(`:2270`): 「2026-10-30、PR #94 取り込みで組み込みが 4 つ増えた構成: **85KB は通過、80KB は 60 秒で打ち切り**」
- 罠(`:2265-2271`): **通る窓の余裕は 5KB 未満。** 組み込み関数を 1 つ足すだけで
  `os_bootstrap` の恒久消費が増え、GC 後も確保できなくなる。そのとき `os_panic` は
  ユニット試験では無限ループ(`runtime.c:1480-1482`)なので、**NG ではなくハングとして現れる**
- **別の口と混ぜない**: `HEAP_EXHAUSTION_HEAP_SIZE`(既定 `4 * 1024 * 1024`、
  `test/c/runtime_test.c:2725-2726`)は要件が逆。`Makefile:28-45` に両者の違いが書かれている

**走らせ方(事実)**:

```
make test                                   # 全部。runtime_test も含む
make test SMALL_HEAP_SIZE=80000             # 掃引(単位 byte)
make test HEAP_EXHAUSTION_SIZE=2000000      # こちらは別の口
```

- `Makefile:23-33`(`SMALL_HEAP_SIZE ?=`)、`Makefile:117`(`SMALL_HEAP_FLAGS`)
- ビルド行: `Makefile:549-553`(`-DISIKIOS_UNIT_TEST $(ALIGN_AUDIT_FLAGS) $(SMALL_HEAP_FLAGS) $(HEAP_EXHAUSTION_FLAGS)`)。
  **`SMALL_HEAP_FLAGS` を渡しているのは `TEST_BIN_RUNTIME` の行だけ**(`Makefile:26-27` のコメントどおり)
- 実行行: `Makefile:654`(`docker run ... --entrypoint /workspace/$(TEST_BIN_RUNTIME)`)
- 署名: `Makefile:123` の `BUILD_FLAG_SIGNATURE` に `$(SMALL_HEAP_FLAGS)` が入っている(`:114-121` が理由)
- 掃引時の罠(`test/c/runtime_test.c:2261-2263`): **`timeout` はコンテナの中で効かせる。**
  `timeout N docker run ...` は docker クライアントしか殺さない

**M1 がこの試験に波及するか(リスク R10)**:
**しない。** 根拠: 同梱の読み込みは `process_trampoline_c`(`process.c:314-322`)の上で走るが、
`runtime_test` は `process.c` をリンクするだけで `process_trampoline_c` を呼ばない
(`test/c/runtime_test.c:90` のコメント「`process.c` が参照する…ダミー実装」)。
`setup_small_heap`(`:2276-2282`)が呼ぶのは `os_bootstrap` まで。
**ただし、同梱に伴って `os_bootstrap` に組み込み関数を 1 つでも足したら掃引が要る**
(`:2265-2271`。例えば「同梱を読む」用の Lisp プリミティブを足す設計を選んだ場合)。

---

## G. 二重定義(issue #186 の材料)

### G-1. 突き合わせの方法(機械的、再現可能)

```bash
export LC_ALL=C
for f in src/lisp/init.lisp src/lisp/init_aot.lisp; do
  grep -oE '\((defun|defmacro|defglobal|defdynamic)[[:space:]]+[^ ()]+' "$f" \
    | awk '{print tolower($2)}' | sort -u > "/tmp/$(basename $f).all"
done
comm -12 /tmp/init.lisp.all /tmp/init_aot.lisp.all
```

- 行頭固定版(`^\(`)と、インデントも拾う版の**両方**で実行した
- クラス階層も別に突き合わせた(`defclass` 対 `%register-builtin-class` / `%register-class`)

### G-2. 結果(事実)

**`defun` / `defmacro` / `defglobal` / `defdynamic` の重複は 1 件も無い。**

| 指標 | `src/lisp/init.lisp` | `src/lisp/init_aot.lisp` | 共通 |
|---|---|---|---|
| 4 種の定義名(行頭固定) | 112 | 99 | **0** |
| 4 種の定義名(インデント込み) | 119 | 99 | **0** |
| クラス名(`defclass` 対 `%register-*-class '…`) | 18 | 28 | **0** |
| `defgeneric` / `defmethod` 名 | 10 / 8 | 0 / 0 | **0** |

種別の内訳(行頭固定):

| 種別 | `init.lisp` | `init_aot.lisp` |
|---|---|---|
| `defun` | 79 | 96 |
| `defmacro` | **31** | 0 |
| `defdynamic` | 7 | 3 |
| `defclass` | 18 | 0 |
| `defgeneric` | 2 | 0 |
| `defmethod` | 8 | 0 |
| `defglobal` | 0 | 0 |

`defmacro` が 31 個というのは計画書 `docs/plans/local-load-and-core-image.md:0 節` の F4 と一致する。

### G-3. 重複は無いが、**役割分担が残っている 2 系統**(M1-T7 で記録すべき事実)

**(1) クラスの登録先が 2 か所に分かれている**

| 場所 | 対象 | 手段 |
|---|---|---|
| `src/lisp/init_aot.lisp:375-405` | `<object>` 以下の組み込みクラス 28 件 | `%register-builtin-class` / `%register-class` の**トップレベル呼び出し**(`os_run_aot_toplevel_forms` で起動前半に実行) |
| `src/lisp/init.lisp`(18 件の `defclass`) | 条件クラス階層(`<condition>` 〜 `<storage-exhausted>`)と `<parse-error>` 等 | `defclass` マクロ |

- `init_aot.lisp:705-707` のコメント「条件クラス階層と `*handlers*` の `defdynamic`、
  `with-handler` マクロ自体は `defclass`/`defdynamic`/`defmacro` が常にインタプリタ実行される
  (main が defun 以外を無視するため)ため**移動不要**」
- `src/c/runtime.c:5248-5257` が「手続きは AOT 側にあるのに条件クラスは 1 つも無い」食い違いの
  根拠として明記している
- **名前の重複は無い**ので「どちらを正とするか」の衝突は発生していない

**(2) `defdynamic` が 2 か所に分かれており、1 つだけ特別な形をしている**

| 場所 | 名前 |
|---|---|
| `src/lisp/init_aot.lisp:348` | `*classes*` |
| `src/lisp/init_aot.lisp:563` | `*generic-methods*` |
| `src/lisp/init_aot.lisp:564` | `*next-methods*` |
| `src/lisp/init.lisp:616` | `*handlers*` |
| `src/lisp/init.lisp:893` | `*symbol-properties*` |
| `src/lisp/init.lisp:1004-1006` | `*standard-input*` / `*standard-output*` / `*error-output*` |
| `src/lisp/init.lisp:1059` | `*read-default-float-format*` |
| **`src/lisp/init.lisp:1230`** | **`*environments*` — `(defdynamic *environments* (dynamic *environments*))`** |

- `init_aot.lisp` 側の 3 つは、**まさにこの事故を避けるために移された**ものである。
  `init_aot.lisp:340-347`(`*classes*`)、`:552-557`(`*generic-methods*`)。
  根拠: `defdynamic` は `eval_defdynamic` が**無条件に** `os_set_dynamic` で上書きする仕様なので、
  起動前半に AOT のトップレベルフォームが書き込んだ登録を**無言で消す**
- `init.lisp:1230` だけが「既存値を書き戻す」安全な形。理由は `:1222-1229` に詳述
- **M1 での意味**: 同梱によって読み込みが**常に**走るようになるので、この事故の露出が
  「試験・手動のときだけ」から「毎回」に変わる。**今後 AOT 側で新しい共有動的状態を
  書き込むときは、その `defdynamic` が `init.lisp` 側にあるかを先に確認する**
  (この運用則は既にプロジェクト記憶として記録済み)

---

## H. 作業時に一緒に直すべき古い注記(原則 12)

| 場所 | 現在の記述 | 直す理由 |
|---|---|---|
| `src/c/load.h:6-14` | 「エラー内容を表示し、`g_sym_eval_error` を返す」 | 実際は `os_signal_io_error` で signal を上げる(`load.c:49-55`) |
| `src/c/reader.h:35` | 「init.lisp 未ロード時は `g_sym_eval_error`」 | §D-3。「条件クラス登録前(ブート前半)/ネイティブ試験」へ |
| `src/c/eval.h:56` | 「init.lisp 未ロード等で」 | 同 |
| `src/c/runtime.h:2087`, `:2211` | 「init.lisp 未ロードで」 | 同 |
| `src/c/runtime.c:5193-5196`, `:5248-5253`, `:5302`, `:5349`, `:5425`, `:5462` | 「boot-entry スクリプトが `(load "src/lisp/init.lisp")` を持たない場合」 | 同梱後は boot-entry の有無では決まらない |
| `src/c/process.c:15` | 「`(load "src/lisp/init.lisp")` のような初回フルロード」 | 同梱読み込みに読み替え |
| `src/c/kernel.c:165-171` | `.qemu-test-trigger` 専用の説明 | 同梱読み込みが同じ場所に相乗りする旨を追記 |
| `Makefile:997-999` | 「他の milestone はすべて先頭で load する」 | §E-3 の 1 |
| `README.md:23-30` | 手で読み込む 3 手 | §C-4 |
| `test/lisp/environment_utilities_test.lisp:103-123` | 「従来の `(load ...)` とは異なり」 | §E-3 の 8 |
| `test/lisp/environments_predefined_test.lisp:3-14` | 「qemu 実機起動(cc_load 経由で init.lisp を最初に load する)」 | §E-3 の 6 |

---

## I. 論点 L1〜L7(論理要約の追記)に対応する実装上の選択肢

| 論点 | 実装上の選択肢と根拠 |
|---|---|
| L1 打ち切り件数 | `os_load_from_stream(os_stream_t *, const char *name, lisp_val_t env, int *out_aborted_count)` にする。`load.c:90-103` で `(*out_aborted_count)++` を足すだけ。`cc_load` は NULL を渡せば従来と同一。**これなしに D1 は成立しない**(§B-4) |
| L2 補助ライブラリ | (a) `disassemble.lisp` も同梱して `init.lisp:1337` を書き換える / (b) `init.lisp:1337` を削り、別途同梱して続けて読む / (c) 失敗を許す。**(c) は上の L1 と組み合わせると D1 と矛盾する**(§E-3 の 2) |
| L3 包むか包まないか | `cc_load` と同じ**C のローカル `os_stream_t`**(`load.c:47`)にすれば `runtime.c:3295-3311` の複製は起きない。言語側の値(`os_make_stream`)として包むと**GC ごとに 72KB の複製**(`runtime.c:3304-3311`、`str_cap` 分を丸ごと `gc_to_alloc`) |
| L4 起動点の置き方 | `os_set_qemu_test_mode`(`process.c:310-312`)と同じ形の登録を足し、`kernel.c:197` 付近から登録。`process.c` に新しいリンク依存を持ち込まない(§A-4 の 16 本の `TEST_SRC_*`) |
| L5 (a) を残す口 | 同梱を読まない切り替えが必要なら、**新しいビルドフラグは `Makefile:123` の `BUILD_FLAG_SIGNATURE` に入れる義務**がある(§A-5)。または `.qemu-test-trigger` の内容に印を持たせる(`kernel.c:176-187` の読み取りを拡張) |
| L6 下準備の外す範囲 | §E-1 の 45 + 7 箇所。生成器 2 本(`tools/gen_*.py`)とシェル 4 本は**生成側**を直す。`run_paint_audit.sh` は 2 箇所 |
| L7 立ち上げ時間 | §F-3 の (i)〜(iv)。(i) が最小で M1-T6 の目的を満たす |
