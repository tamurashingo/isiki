# 参照マップ: 初期アプリケーション `inspect`(実装 / レビュー向け)

対象: `main = f8ff0e2`。**読み取りのみ。実行していない**(★未検証は全編に効く)。
論理要約: `docs/investigation/app-inspect.logic.md`。
節番号・項目番号(A1 等)はそちらと対応する。

---

## 0. 件数の導出(規則 13)

**本文に数を書き写していない。** 下のコマンドを走らせて出すこと。

```sh
# [D1] Lisp から引ける登録名の総数(リテラル名で登録されているもの)
grep -rho 'os_set_function(os_make_symbol("[^"]*")' src/c/*.c \
  | sed 's/.*os_make_symbol("//;s/")//' | sort -u | wc -l

# [D2] うち診断窓口(%% で始まるもの)
grep -rho 'os_set_function(os_make_symbol("%%[^"]*")' src/c/*.c \
  | sed 's/.*os_make_symbol("//;s/")//' | sort -u | wc -l

# [D3] ファイルごとの登録件数(= どの初期化で登録されるかの分布)
grep -rn "os_set_function(" src/c/*.c | awk -F: '{print $1}' | sort | uniq -c

# [D4] ★ここが本件の核心★ os_bootstrap の中で登録される件数
#      = SMALL_HEAP_SIZE の狭い窓に効く集合の大きさ
awk '/^void os_bootstrap\(\) \{/{f=1} f&&/os_set_function\(/{c++} f&&/^\}/{print c; exit}' \
  src/c/runtime.c

# [D5] 型述語の一覧(目視で絞る必要あり。語尾 P / -P)
grep -rho 'os_set_function(os_make_symbol("[^"]*")' src/c/*.c \
  | sed 's/.*os_make_symbol("//;s/")//' | sort -u | grep -E 'P$'

# [D6] 同梱される Lisp ソース(カーネルに焼かれるもの)
python3 tools/gen_embedded_lisp.py --print-inputs

# [D7] AOT トランスパイル対象の Lisp ソース
make print-transpile-src      # もしくは Makefile:63 の TRANSPILE_LISP_SRC

# [D8] 同梱ソースの大きさと定義の数(起動費用の見積り。boot-embedded-init-cost.md §2)
wc -c $(python3 tools/gen_embedded_lisp.py --print-inputs)

# [D9] 試験ファイルの配線名簿(新規試験を足すとき)
python3 tools/check_test_count_ratchet.py --wiring-derive

# [D10] EVAL-ERROR 返しの残数予算(新しい 'eval-error を足すと落ちる)
python3 tools/check_eval_error_budget.py --list
```

---

## 1. 論理要約 §3-1「既にある内省の手段」の対応表

| # | 実装箇所(登録 / 定義) | 備考 |
|---|---|---|
| A1 型述語 | `src/c/runtime.c:3887`(`FUNCTIONP`)、`:3960`(`GENERIC-FUNCTION-P`)、`:3967`(`STREAMP`)他。一覧は [D5] | `CONSP`/`SYMBOLP`/`STRINGP`/`CHARACTERP`/`FIXNUMP`/`BIGNUMP`/`FLOATP`/`NUMBERP`/`LISTP`/`BASIC-VECTOR-P`/`GENERAL-VECTOR-P`/`GENERAL-ARRAY*-P`/`BASIC-ARRAY-P`/`INSTANCEP` |
| A2 `CLASS-OF` | 定義 `src/lisp/init_aot.lisp:495-519`、登録 `src/c/lisp_compiled.c:20366` | **分岐順序を動かすな**(`init_aot.lisp:492-494` のコメント)。`<FIXNUM>/<BIGNUM>/<SINGLE-FLOAT>/<DOUBLE-FLOAT>` を返す拡張あり |
| A3 `TYPEP`/`SUBCLASSP`/`INSTANCEP` | `init_aot.lisp:468-474`(`subclassp`)、`:524`(`typep`)、`:533`(`instancep`)、登録 `lisp_compiled.c:20363`/`20367` | |
| A4 `%%CLASS-NAME`/`%%CLASS-SUPERS`/`%%CLASS-SLOTS` | 登録 `runtime.c:4002`/`4003`/`4004`、実体 `runtime.c:11570`/`11582`/`11610` | **検査の有無が不揃い。下の §5 参照** |
| A5 `%%CLASSP`/`%%BUILTIN-CLASSP`/`%%STANDARD-CLASSP` | 登録 `runtime.c:4005`/`4006`/`4007` | `%%CLASSP` 実体 `runtime.c:11633` |
| A6 `%%INSTANCE-CLASS`/`%%INSTANCE-SLOTS`/`%%CLASS-INSTANCE-P` | 登録 `runtime.c:4009`/`4010`/`4011`、実体 `runtime.c:11694`/`11706`/`11718` | 前2つは**無検査** |
| A7 `SLOT-VALUE`/`SET-SLOT-VALUE`/`%SLOT-INDEX` | `init_aot.lisp:438-442`/`:166-170`、登録 `lisp_compiled.c:20359`/`20332` | **欄が無いと `'eval-error` を返す**(`init_aot.lisp:441`) |
| A8 分類の登録簿 `*classes*` | `init_aot.lisp:352`(`defdynamic`)、`:355`(`%find-class`)、`:360`(`%register-class`) | `(dynamic *classes*)` は `(name . class)` の alist。**全件歩ける** |
| A9 方法の登録簿 `*generic-methods*` | `init_aot.lisp:571`、`:574`(`%find-generic-methods`)、`:600`(`%register-method`) | `(name . ((specializers . fn) ...))` の alist |
| A10 `%APPLICABLE-METHODS` 他 | `init_aot.lisp:629`、登録 `lisp_compiled.c:20376` | |
| A11 `%%ZA-COMPILED-P` | 登録 `runtime.c:3888`、実体 `runtime.c:10419` | word0==NATIVE かつ word2!=nil |
| A12 `%%DISASM-*` + `disassemble` | 登録 `src/c/disasm_lisp.c:353`〜、Lisp 層 `src/lisp/disassemble.lisp`(全体) | **os_bootstrap の外**(`kernel.c` のブート列) |
| A13 `%%OPTIMIZE-OF`/`%%INLINE-OF`/`%%DECLARED-TYPES-OF` | 登録 `runtime.c:3951`/`3955`/`3956`、meta 定義 `runtime.h:1903-1932` | meta に `arity` の場所はあるが**窓口が無い**(`runtime.h:1906`) |
| A14 `(function name)` | `src/c/eval.c:535-548`、分岐 `eval.c:1252` | 未定義なら `<undefined-function>` を signal(= fboundp 代わりには使えない) |
| A15 `SYMBOL-NAME` | 登録 `runtime.c:3968` | 名前は**大文字化して保持**(`runtime.c:4306`) |
| A16 `PROPERTY`/`SET-PROPERTY`/`REMOVE-PROPERTY` | `src/lisp/init.lisp:901`(`*symbol-properties*`)、`:929`/`:939`/`:949` | 表の形は `((sym . propname) . value)`。**全件歩ける** |
| A17 `%%CURRENT-ENVIRONMENT`/`%%GLOBAL-ENVIRONMENT`/`%%MAKE-ENVIRONMENT`/`%%EVAL-IN-ENVIRONMENT` | 登録 `runtime.c:3890`/`3891`/`3889`/`3893` | |
| A18 環境の構造 | `runtime.c:4643-4668`(レイアウトのコメント)、`runtime.c:4636`(`os_make_environment_tagged`)、読み出しの例 `runtime.c:4035-4103`(`os_get_variable`) | スロット順 `NAME VARIABLES FUNCTIONS PARENT CONSTANTS CELLS PAGES LITERAL-SLOTS [CODE-CURSOR]`。**先頭4つは固定**(`runtime.c:4655`) |
| A19 `LIST-ENVIRONMENTS`/`%ENVIRONMENT-NAME`/`%ENVIRONMENT-PARENT` | `init.lisp:1332`/`:1272`/`:1336` | |
| A20 `(dynamic name)` | `eval.c:410-413`(未束縛は nil) | 名前を動的に作る場合は `%%EVAL-IN-ENVIRONMENT` に `(list 'dynamic sym)` を渡す |
| A21 流れ口の述語/位置 | `stream_lisp.c:730`(`OPEN-STREAM-P`)、`:745`(`FILE-POSITION`)、`:747`(`FILE-LENGTH`)、`INPUT-STREAM-P`/`OUTPUT-STREAM-P` も同ファイル | |
| A22 計器 | `runtime.c:3936`(`%%HEAP-USED-BYTES`)、`:3958`(`%%IMM-SPACE-USED-BYTES`)他。一覧は [D2] | |
| A23 `MACROEXPAND-1` | `eval.c:1177` | |
| A24 `HEX-DUMP-BYTES` | `src/lisp/utility.lisp`(`%hex-dump-*`)、登録 `lisp_compiled.c:20458` | |

### 欄の定義の形

`src/lisp/init.lisp:401-404`(`%slot-spec-form`): `(slot-name initarg-key thunk)`。
`(car descriptor)` が欄の名前。継承分は `init_aot.lisp:427`(`%merge-superclass-slots`)が
**単純連結。同名の上書きをしない**(`init_aot.lisp:423-425` のコメント)。

---

## 2. §3-2「既にある一覧表示系」

| もの | 実装 | 注意 |
|---|---|---|
| `DESCRIBE`(**装置専用**) | `src/lisp/device.lisp:221-231`、登録 `src/c/lisp_compiled.c:20439` | **一般名が占有されている。** `device.lisp:1-12` の「AOT対象外」コメントは M15 で古くなっている(いまは `Makefile:63` の `TRANSPILE_LISP_SRC` に入っている) |
| `DISASSEMBLE` 一式 | `src/lisp/disassemble.lisp:42-140` | **本件の雛形。**表示先を引数で受ける(`:73`)、画面向け(`:89`)、文字列向け+あふれ検出(`:115-125`)、構造で返す(`:127`) |
| `ROOM` | `src/lisp/utility.lisp`、登録 `lisp_compiled.c:20416` | 桁区切り `%room-group-digits`、十進文字列 `%room-digit-string` が流用できる |

---

## 3. §4「処理の流れ」

| 項目 | 実装箇所 |
|---|---|
| 対話1周 | `src/c/repl.c:37-80`(`os_repl_step`) |
| **結果を必ず印字する** | `repl.c:67`/`:70`(`os_print(result, proc->stdout_buffer)`) |
| 打ち切り時の表示 | `repl.c:22-28`(`print_condition_report`)、`repl.c:62-71` |
| 1式を読む(プロセス入力) | `src/c/reader.c:917-935`(`os_read`) |
| 1行待つ | `src/c/reader.c:62-72`(`ensure_data`)→ `src/c/interrupt.c:282-286`(`os_wait_for_more_input`、`hlt` ループ) |
| 促し | `reader.c:35`(`prompt_shown` は static)、`reader.c:48-57`(`print_prompt`。文面は固定) |
| 鍵盤→受け皿 | `src/c/interrupt.c:326-355`(`c_keyboard_handler`)→ `src/c/process.c:473-480`(`process_stdin_push`。`'\n'` で `ready=1`) |
| 受け皿の構造と上限 | `src/c/process.h:10`(`PROCESS_STDIN_BUF_SIZE 256`)、`:27-37` |
| 1文字読む(Lisp 側の鍵盤口から) | `src/c/reader.c:987-995`(`os_process_stdin_read_char`。**中で `ensure_data` を呼ぶ = 待つ**) |
| 印字本体 | `src/c/print.c:426-503`(`print_value`)、`:343-361`(`print_list`)、`:388-418`(`print_vector`) |
| 実体の印字(**欄を出さない**) | `print.c:490-494` |
| 関数の印字(**名前も引数も出さない**) | `print.c:495-500` |

---

## 4. §5「業務ルール・不変条件」の根拠行

| § | 根拠 |
|---|---|
| 5-1 nil は TAG_CONS を持つ | `src/c/runtime.c:3723-3736`(`g_nil_cell` を自己循環 cons にする)、`print.c:427-431`、`eval.c:418-420`、`init_aot.lisp:500`(`class-of` で `null` を先に判定) |
| 5-2 欄は単純連結・上書きなし | `init_aot.lisp:423-427` |
| 5-3 欄が無いと `'eval-error` | `init_aot.lisp:438-442`。予算は `tools/check_eval_error_budget.py`([D10]) |
| 5-4 再定義は前に積む | `init_aot.lisp:596-603`、`:360-363` |
| 5-5 **検査の無い窓口** | §5 の表(下) |
| 5-6 印字は循環を見張らない | `print.c:343-361`(`while (current != nil ...)` のみ。深さ/長さの上限なし)、`print.c:426`(再帰) |
| 5-7 長さが循環で止まらない | `runtime.c:11231-11245`(cons 分岐は `:11239-11245`)(`primitive_length` の cons 分岐) |
| 5-8 添字は循環でも止まる | `runtime.c:11281-11323`(`primitive_elt_impl`。cons/string/vector の3種に効く) |
| 5-9 名前は大文字化 | `runtime.c:4306`(`os_make_string_for(name, 1 /* uppercase */)`) |
| 5-10 環境の先頭4つは固定 | `runtime.c:4655-4667` |
| 5-11 文字列の器は黙ってあふれる | `src/c/stream.h:123`(`STREAM_STRING_OUTPUT_CAP 1024`)、作法の前例 `disassemble.lisp:107-125` |
| 5-12 戻り値は必ず印字される | `repl.c:67`/`:70` |
| 5-13 利用者向けの道具が `%%` を直接呼ぶ前例 | `disassemble.lisp:44`/`:52`/`:65-66` |

---

## 5. 検査の有無が不揃いな窓口(**実装者必読**)

| 窓口 | 実体 | 型検査 | 任意の値を渡したとき |
|---|---|---|---|
| `%%CLASS-NAME` | `runtime.c:11570-11574` | **無し** | word1 を生で読む。ヒープ外を読みうる |
| `%%INSTANCE-CLASS` | `runtime.c:11694-11698` | **無し** | word1 を生で読む |
| `%%INSTANCE-SLOTS` | `runtime.c:11706-11710` | **無し** | word2 を生で読む |
| `%%CLASS-SUPERS` | `runtime.c:11582-11602` | あり(TAG + MAGIC) | `nil` を返す。コメントに事故の記録あり |
| `%%CLASS-SLOTS` | `runtime.c:11610-11625` | あり | `<STANDARD-CLASS>` の domain-error を signal。コメントに事故の記録あり |

→ `inspect` は**必ず `%%CLASS-INSTANCE-P` / `%%CLASSP` で先に判定してから**上の3つを呼ぶ。

---

## 6. §6「状態」

| 項目 | 実装 |
|---|---|
| 受け皿の状態遷移 | `process.c:473-480`、`reader.c:25-29`(`reset_line`)、`interrupt.c:339-342`(`ready` 中の打鍵は捨てる) |
| **読み取り口の1文字先読みと戻し** | `src/c/stream_lisp.c:264-295`(`cc_read`。`:287-293` で口へ戻す) |
| **既定の鍵盤口は毎回新しく作られる** | `stream_lisp.c:60-68`(`resolve_input_args`)、`stream_lisp.c:31-34`(`make_keyboard_input_stream`)、`src/lisp/init.lisp:1024-1026`(`standard-input`) |

→ **口を毎回作ると戻した1文字が捨てられる。** `(let ((in (standard-input))) ...)` で持ち回るか
`with-standard-input`(`init.lisp:1035`)で束縛する。★未検証(機構は上の行から読める)。

---

## 7. §7「出入り口・置き場所」

| 選択肢 | 実装箇所 | 代償 |
|---|---|---|
| カーネル同梱 | 入力一覧 `tools/gen_embedded_lisp.py:46-47`、生成物 `src/c/init_lisp_src.c`、読む所 `src/c/kernel.c:351-368`(`run_boot_entry`)、1本読む所 `kernel.c:168` | 起動費用(`documents/boot-embedded-init-cost.md`)、不動領域の恒久消費(`runtime.c:1926` `IMM_SPACE_SIZE 16MB`、単価は `documents/performance-measurement.md`) |
| アプリ専用の円盤 | 見本 `apps/hello.lisp`、焼く所 `Makefile:1157-1190`、9P 無し起動での読み `src/c/kernel.c:301-321` | 読む前に `mount` が要る(`kernel.c:307`)。常駐しない |
| AOT(機械語化)層 | `Makefile:63`(`TRANSPILE_LISP_SRC`)、登録 `src/c/lisp_compiled.c:20314`(`os_register_aot_init_functions`)、呼ぶ所 `src/c/kernel.c:435` | 変換器の制約(`documents/jit-unsupported-syntax.md`、`transpile.lisp`)。**os_bootstrap の外なので SMALL_HEAP_SIZE には触らない** |
| ホスト共有 | `documents/load-path-contract.md`、事故の記録 `src/lisp/init.lisp:1364-1374` | 9P 無しで**黙って定義が欠ける** |

---

## 8. §8「制約」の根拠(**この調査でいちばん重要な訂正**)

### 8-1. `SMALL_HEAP_SIZE` が効く範囲

- つまみ: `test/c/runtime_test.c:2272-2274`(既定 `85*1024`)、掃引の記録 `:2240-2271`
- **効く経路はここだけ**: `test/c/runtime_test.c:2276-2282`(`setup_small_heap`)。
  呼ぶのは `os_heap_init` → `os_reset_runtime_state_for_test` → **`os_bootstrap` のみ**
- 使用箇所は `setup_small_heap()` を呼ぶ試験だけ:
  `grep -c "setup_small_heap()" test/c/runtime_test.c`
- `os_bootstrap` の本体: `src/c/runtime.c:3722-4016`。登録件数は [D4]
- **他の初期化は小ヒープ試験の経路に入らない**(各試験の `os_bootstrap` 呼び出しは
  `grep -n "os_bootstrap\|os_register" test/c/*.c` で確認できる。
  `clock_test.c:155` / `ide_test.c:467` / `script_test.c:299-303` は別のヒープ)
- 枯渇試験は**別のつまみ**: `test/c/runtime_test.c:2714-2730`(共有してはいけない理由つき)
- 陽性対照: `make check-small-heap-knob`(`Makefile:751-758`)
- 罠の記述: `src/c/runtime.h:914-916`、`src/c/za.c:4327-4328`、`:4387`、`src/c/load.h:93`

### 8-2. その他の見張り(新規ファイルを足すとき落ちるもの)

| 見張り | 実装 | 何で落ちるか |
|---|---|---|
| `check-eval-error-budget` | `tools/check_eval_error_budget.py`、`Makefile:773` | `src/lisp/*.lisp` に `'eval-error` を増やすと落ちる |
| `check-test-file-wiring` | `tools/check_test_count_ratchet.py --wiring`、`Makefile:436-448` | 新しい試験ファイルを名簿に入れずに置くと落ちる([D9]) |
| `check-macro-order` | `tools/check_macro_forward_refs.py` | `init.lisp` のマクロ定義順 |
| `check-defdynamic-safe` | `tools/check_defdynamic_safe.py` | 動的名の初期宣言の安全形(`init.lisp:898-901` の作法) |
| `check-embedded-lisp-src` | `tools/gen_embedded_lisp.py --check`、`Makefile:294-295` | 同梱の生成物が古い |
| `check-pass-line` | `tools/check_pass_line.py` | 合否の一行を書く箇所の在庫 |

---

## 9. §9「問題点」の対応行

1. 名前の衝突 → `src/lisp/device.lisp:221`(`describe`)
2. 循環で印字が止まらない → `src/c/print.c:343-361`。REPL が必ず印字 → `src/c/repl.c:67`/`:70`
3. 長さが止まらない → `src/c/runtime.c:11239-11245`
4. 無検査の窓口 → 本書 §5
5. 先読みの喪失 → 本書 §6
6. 促しの文面が固定 → `src/c/reader.c:48-57`、`prompt_shown` の寿命 `reader.c:35`/`:67-70`/`:918`
7. 文字列の器のあふれ → `src/c/stream.h:123`、作法 `src/lisp/disassemble.lisp:107-125`
8. 欄名の重複 → `src/lisp/init_aot.lisp:423-427`
9. ホスト共有の黙った欠落 → `src/lisp/init.lisp:1364-1374`
10. 解釈実行関数の中が取れない → レイアウト `src/c/runtime.h:179-180`
    (`word1`=仮引数、`word2`=本体、`word3`=定義時環境)。
    **これを読む窓口は存在しない**:
    `grep -rn "MAGIC_FUNCTION_INTERPRETED" src/c/runtime.c src/c/za.c src/c/eval.c`
    で読んでいるのは評価器と GC だけ。
    値→番地の窓口も無い(`grep -rn "TAG_RAW_POINTER" src/c/runtime.c` は環境のページ/セル用のみ)
11. 起動費用 → `documents/boot-embedded-init-cost.md`(合格線を置かない方針も同文書)

---

## 10. §11「骨格」の実装メモ

### 使う窓口(すべて既存)

- 種類の決定: `CLASS-OF` → `%%CLASS-NAME`(`%%CLASSP` で守って呼ぶ)
- 分解:
  - cons: `CAR`/`CDR`
  - string / vector / array / 欄の値の並び: `ELT`(`runtime.c:11281`。3種に共通で効く)
  - 配列の形: `ARRAY-DIMENSIONS`(`runtime.c:3975`)
  - 実体: `%%CLASS-INSTANCE-P` → `%%INSTANCE-CLASS` → `%%CLASS-SLOTS`(名前は `(car d)`)
          → `%%INSTANCE-SLOTS` を `ELT` で引く
  - 分類: `%%CLASS-NAME` / `%%CLASS-SUPERS` / `%%CLASS-SLOTS`
  - 記号: `SYMBOL-NAME`、`(dynamic sym)`(動的な名前は `%%EVAL-IN-ENVIRONMENT` 経由)、
          属性は `(dynamic *symbol-properties*)` を絞る
  - 環境: 素の `CAR`/`CDR`/`ASSOC`(`ASSOC` は eq 比較。`init_aot.lisp:34-39`)
  - 関数: `%%ZA-COMPILED-P` / `%%DISASM-CODE-BASE` / `%%DISASM-CODE-LEN` /
          `%%OPTIMIZE-OF` / `%%DECLARED-TYPES-OF`
- たどる / 戻る: 道筋は cons の列。`MEMBER`(**eq 比較**。`init_aot.lisp:25-30`)が
  同一性による既訪判定に使える
- 対話: `(read in)`(`stream_lisp.c:264`)または `READ-LINE`(`:740`)+ `PARSE-NUMBER`(`runtime.c:3870`)。
  **`in` は1つ作って持ち回る**(本書 §6)
- 表示: `FORMAT`(`format.c:291`)。指示子は `format.c:204-276` にある
  `A S B O D X R C G T % & ~` のみ。
  **`~nA` の桁詰めは無い**(`num` は A/S では無視される。`format.c:204-215`)。
  桁揃えは `~nT`(`format.c:258-266`、`stream->column` を見る)か `STRING-APPEND` で作る
- 出力先: `(standard-output)`(`init.lisp:1027-1029`)。
  **`(format t ...)` は使えない**(第1引数は MAGIC_STREAM を要求する。
  `format.c:164-168`→`stream_lisp.c:139`→`:18-22` が生ポインタを読む)

### 書いてはいけないこと

- 合成値を `~A`/`~S` に渡す(`format.c:204-215` が `os_print_to_sink` を呼ぶ = §5-6 の経路)
- cons に `LENGTH` を使う(§5-7)
- `'eval-error` を返す(§8-2)
- 無検査の窓口を素で呼ぶ(§5)

### 置き場所の推奨と、足す場合の置き場所

- 第1版: `apps/inspect.lisp`(`apps/hello.lisp` と同じ扱い。`Makefile:1183` の wildcard に自動で乗る)
- 常駐させるなら `tools/gen_embedded_lisp.py:46-47` に追加 + `src/c/kernel.c:351-368` の順序を決める
- **もし窓口を1つ足すなら** `src/c/subprimitive.c:173-184` の
  `os_register_subprimitives`(`kernel.c:418` から呼ばれる)か
  `src/c/disasm_lisp.c:353`〜 の形にする。**`src/c/runtime.c` の `os_bootstrap` には置かない**
  (§8-1。足したら `make test` と `make check-small-heap-knob` で確かめる)

---

## 11. 近い試験と設計記録

| もの | 場所 |
|---|---|
| 逆アセンブル表示の試験(**雛形**) | `test/lisp/disassemble_test.lisp`(`%%za-compiled-p` の前提確認が `:14-15`) |
| 環境操作の試験 | `test/lisp/environment_utilities_test.lisp`、`environments_predefined_test.lisp`、`environment_pages_test.lisp`、`environment_literal_slots_test.lisp` |
| 装置表示の試験 | `test/lisp/device_test.lisp` |
| 印字の試験 | `test/c/print_test.c`。**循環の試験は nil の1件だけ**(`:325`)。合成値の循環は試験が無い(`grep -n '循環' test/c/print_test.c` は1件) |
| 読み取り/REPL の試験 | `test/c/reader_test.c`、`test/c/repl_test.c` |
| 流れ口の試験 | `test/c/stream_lisp_test.c`、`test/lisp/isiki_test.lisp:1190-1260`(read / 文字列ストリーム) |
| 円盤からアプリを読む試験 | `test/lisp/load_disk_test.lisp:46-60`、9P 無し起動は `src/c/kernel.c:290-330` |
| 試験の枠組み | `test/lisp/test_framework.lisp`、段の例 `test/lisp/qemu_boot_test.lisp` |
| 型階層の調査記録 | `documents/type-system-survey.md` |
| 仕様の実装定義事項 | `documents/islisp-implementation-defined.md` |
| 環境の設計(本文が参照している `documents/environment.md` は**存在しない**) | `ls documents/ | grep -i environ` → 0 件。`init.lisp:1241` の参照は切れている |
| コンソール/巻き戻し | `documents/console-process-current-state.md`、`documents/console-scrollback.md` |
| 同梱の起動費用 | `documents/boot-embedded-init-cost.md` |
| 不動領域が解放されないこと | `documents/constraint-jit-code-pages-are-immortal.md` |
| 記録と計測の規則 | `documents/records-and-measurement-rules.md`(規則 9 / 13 / 16 はこの調査で参照した) |

### 「inspect に近いもの」は存在しない

```sh
grep -rni "inspect" --include='*.lisp' --include='*.c' --include='*.h' --include='*.md' . \
  | grep -v '^./tmp/' | grep -v disasm_symtab
```
→ ヒットは `src/lisp/init.lisp:1317` のコメント1件だけ(語としての用法)。
