# 調査報告(第2次): frame 分離案の実現可能性

> 実施: 2026-09-15 / ブランチ: `feature/compiler-optimization`(分岐なし)
> **コードは一切変更していない。** 測定は一時的な Lisp スクリプトのみで行い、実行後に削除済み。
> 前回報告: `documents/investigation-defun-in-let.md`
>
> - 「**確認**」= コードを読んだ、または実機(QEMU)で実測した。必ず根拠を示す
> - 「**推測**」= 裏を取れていない。そう明示する

---

## S の結果(仮説検証): **仮説は成立した**

「バグの対象は `let` ではなく `CALL-ENV` 全体である」は**正しい**。

```lisp
(defun outer () (defun inner () 42))
(outer)   ; → INNER(defun 自体は成功する)
(inner)   ; → EVAL-ERROR
```

実機実測:

```
#S outer compiled=NIL
#S (outer) の戻り値 = INNER
#S 外から (inner) = EVAL-ERROR
#S outer2 compiled=NIL 中から (inner2) = 43      ← outer の中でなら呼べる
#S 外から (inner2) = EVAL-ERROR
#S 対照: 普通の defun は JIT されるか = T
#S outer3(fletの中) 外から (inner3) = EVAL-ERROR
#S outer4(labelsの中) 外から (inner4) = EVAL-ERROR
#S 対照: トップレベル = 46
```

**`let` の場合とまったく同じ構造。** 「定義した環境の中でなら呼べる / 抜けると呼べない」
という挙動が一致している。`flet` / `labels` の中でも同じ。

### S-1. 理由は `let` と同一か

**確認。同一。** `eval_defun`(`eval.c:386`)が `os_set_function(name, fn, env)` に
呼び出し時の env を渡し、その env が `apply_function`(`eval.c:152`)の作った
`CALL-ENV` である、という一本道。`let` は IIFE なので同じ `apply_function` を通る
(前回 A-4 で確認済み)。

環境の**名前**そのものを Lisp から取得する手段は無い(前回の調査で確認済み。
`%%current-environment` は `proc->env` を返すのでレキシカル env ではない)。
したがって「`outer` の本体を評価している env の名前が `CALL-ENV` であること」は
**コードリーディングによる確認**であり、実機で名前を読んだわけではない。
`os_make_environment` を呼ぶ箇所は全部で 8 つしかなく
(前回報告の環境名一覧)、インタプリタの関数適用に対応するのは `eval.c:152` だけ
なので、この同定は確実である。

### S-2. JIT 経路との違い

**確認。`defun` を含む関数は JIT されない。**

`za_is_excluded_special_form`(`src/c/za.c:1705-1713`)に `defun` が入っている:

```c
return head == g_sym_quote ||
       head == g_sym_defun || head == g_sym_lambda || head == g_sym_defmacro ||
       head == g_sym_function ||
       head == g_sym_defvar || head == g_sym_defconstant ||
       head == g_sym_defglobal;
```

実測でも `outer` / `outer2` / `outer3` がすべて `compiled=NIL`(対照の
`(defun jit-ok (a b) (+ a b))` は `compiled=T`)。

**つまり「`defun` を含む関数本体」は常にインタプリタ実行**であり、JIT 経路で
`defun` の登録先が変わる、という分岐は存在しない。`let` の場合に JIT で環境が
作られない(前回確認)のとは事情が異なり、ここでは**経路は 1 つしかない**。

### S-3. `flet` / `labels` の中の `defun`

**確認。同じく外から呼べない。** `flet` は `FLET-ENV`(`eval.c:440`)、
`labels` は `LABELS-ENV`(`eval.c:479`)を作り、`defun` はそこへ登録される。

---

## サマリ

- **S の仮説は成立。** `let` 固有ではなく、インタプリタの関数適用が作る環境全般の問題。
- **案 A はそのままでは実装できない。** frame(4スロット)を渡すと、**スロットの
  `cdr` を破壊的に書き換える 4 つの関数が `nil` そのものを破壊する**。`nil` は
  自己参照 cons(`g_nil_cell`)なので、これが壊れると処理系が即死する。
- ただし**読み出しは安全**(`cc_cdr(nil) == nil` が成立するため、短いリストを
  辿り越しても `nil` が返るだけ)。危険なのは**書き込み 4 箇所に限定される**。
- したがって案 A は「frame へ書き込みうる経路を潰す」こととセットなら可能。
  必要な手当ては本文の表のとおりで、**`flet`/`labels` と `defconstant` と
  `za_try_compile_defun` のページ登録**が具体的な当たり所。
- **案 B(名前で判定)は推奨しない。** 判定に使う `name` スロットの値は
  ユーザーが `make-environment` で任意に指定でき、`CALL-ENV` 等と同名の環境を
  作れてしまう(`os_make_environment` の TODO が指摘するユニーク性の欠如)。

---

## 調査項目 A: 案 A の実現可能性 — 5〜8番目のスロットへのアクセス箇所

### 使用した検索コマンド(網羅性の根拠)

```bash
# 5番目以降を位置で辿っている箇所(cc_cdr 4連鎖以上)
grep -rn "cc_cdr(cc_cdr(cc_cdr(cc_cdr(" src/c/*.c | grep -v lisp_compiled

# スロット名シンボルによる検索
grep -rn '"constants"\|"cells"\|"pages"\|"literal-slots"\|CONSTANTS\|LITERAL-SLOTS' \
     src/c/*.c src/lisp/*.lisp | grep -v lisp_compiled

# Lisp 側の環境アクセサ
grep -n "%environment-" src/lisp/init.lisp src/lisp/init_aot.lisp

# 環境の長さを仮定している箇所
grep -rn "length.*env\|nth.*env" src/lisp/*.lisp
```

スロット名シンボルによる検索の結果は `os_make_environment` の**構築箇所のみ**
(`runtime.c:2965-2971`)。**シンボル名で環境スロットを引いているコードは存在せず、
すべて位置(cc_cdr の連鎖)でアクセスしている。** したがって上記 1 本目の grep が
5 番目以降のアクセスを網羅する。

### 確認したこと: 5〜8番目にアクセスする箇所は C の 9 箇所のみ

| 場所 | 関数 | 何番目 | frame が渡りうるか | 渡ったときの挙動 | 対処 |
|---|---|---|---|---|---|
| `runtime.c:2025` | `gc_fixup_environment_cells` | 6 (cells) | **渡らない** | — | 不要 |
| `runtime.c:3036` | `os_is_constant` | 5 (constants) | 渡る | **nil が返るだけ(安全)** | 不要 |
| `runtime.c:3060` | `os_mark_constant` | 5 (constants) | 渡る | **★ nil を破壊** | **必要** |
| `runtime.c:3498` | `os_set_function` | 6 (cells) | 渡る | **★ nil を破壊** | **必要** |
| `runtime.c:3541` | `os_get_function_cell` | 6 (cells) | 渡る | **nil が返る(安全だが誤動作)** | 要検討 |
| `runtime.c:3581` | `os_environment_register_pages` | 7 (pages) | 渡る | **★ nil を破壊** | **必要** |
| `runtime.c:3598` | `os_environment_reclaim_pages` | 7 (pages) | 渡らない | — | 不要 |
| `runtime.c:3615` | `os_environment_register_literal_slot` | 8 (literal-slots) | 渡る | **★ nil を破壊** | **必要** |
| `runtime.c:3629` | `os_environment_reclaim_literal_slots` | 8 | 渡らない | — | 不要 |

**Lisp 側に 5 番目以降を触る箇所は無い。** `init.lisp` の環境アクセサは
`%environment-name`(1番目、`init.lisp:1017`)と `%environment-parent`
(4番目、`init.lisp` 末尾)のみで、どちらも先頭 4 スロットに収まる。
環境の長さを仮定した `length` / `nth` によるアクセスも見つからなかった。

### 根拠 1: 読み出しは安全 — `nil` は自己参照 cons

**確認。** `cc_car` / `cc_cdr` は**型チェックをしない**(`src/c/lisp.c:13-34`):

```c
lisp_val_t cc_car(lisp_val_t obj) {
    // TODO: TAG_CONS であることのチェックを入れる
    lisp_val_t v = ((lisp_val_t *)(obj & ~TAG_MASK))[0];
    return v;
}
```

一見すると短いリストを辿り越すと未定義領域を読むように見えるが、`nil` の作られ方が
これを救っている(`src/c/runtime.c:2214-2222`):

```c
// NIL の作成。From/To空間どちらにも属さない専用の固定領域(g_nil_cell)を使う
lisp_addr_t addr = (lisp_addr_t)g_nil_cell;
lisp_val_t tagged = (lisp_val_t)(addr | TAG_CONS);
lisp_val_t *cell = (lisp_val_t *)addr;
cell[0] = tagged;
cell[1] = tagged;    /* ← car も cdr も自分自身 */
nil = tagged;
```

**`nil` は car も cdr も自分自身を指す cons。** したがって
`cc_cdr(nil) == nil`、`cc_car(nil) == nil` が成立し、4 スロットの frame に対して
`cc_cdr` を 5 回でも 7 回でも辿れば `nil` が返る。**読み出しだけならクラッシュしない。**

### 根拠 2: 書き込みが `nil` を破壊する

**確認。** 5 番目以降のスロットを更新する 4 関数は、いずれも次の形をしている。
`os_set_function`(`runtime.c:3497-3518`)を例に:

```c
lisp_val_t cells_slot = cc_car(cc_cdr(cc_cdr(cc_cdr(cc_cdr(cc_cdr(env))))));  /* frame なら nil */
...
} else {
    ...
    lisp_addr_t cells_slot_addr = cells_slot & ~TAG_MASK;      /* = g_nil_cell のアドレス */
    ((lisp_val_t *)cells_slot_addr)[1] = new_cells_alist;      /* ★ nil の cdr を上書き */
}
```

同じ形が `os_mark_constant`(`runtime.c:3075`)、
`os_environment_register_pages`(`runtime.c:3592`)、
`os_environment_register_literal_slot`(`runtime.c:3626`)にある。

`nil` の cdr が別のリストを指すようになると、**あらゆるリスト終端判定が壊れる**
(`while (list != nil)` が終わらない、`cc_cdr` の連鎖が別の場所へ迷い込む)。
`g_nil_cell` は From/To 空間の外の固定領域なので GC でも復元されない。

### 根拠 3: 「frame が渡りうるか」の判定

呼び出し元を辿った結果:

- `os_mark_constant` ← `eval_defconstant`(`eval.c:303`)が `env` を渡す。
  `(let () (defconstant +k+ 1))` で frame が渡る → **踏む**
- `os_set_function` ← `eval_defun`(`eval.c:386`)、`eval_defmacro`(`eval.c:530`)、
  **`eval_flet`(`eval.c:454`)**、**`eval_labels`(`eval.c:493`)**。
  前 2 つは `os_define_function` 導入で回避できるが、**後ろ 2 つは
  `FLET-ENV`/`LABELS-ENV` へ直接書くのが仕様**なので、これらを frame にすると必ず踏む
- `os_environment_register_pages` / `os_environment_register_literal_slot`
  ← `za_try_compile_defun`(`za.c:6124`, `za.c:6130`)が**コンパイル時の env**を渡す。
  `(let () (defun f () 1))` で frame が渡る → **踏む**。
  `os_define_function` を入れても、**ページ登録は別経路なので回避されない**
- `gc_fixup_environment_cells` ← `global_environment` と `get_process(i)->env` のみ
  (`runtime.c:2168`, `2170`) → **frame は渡らない**
- `os_environment_reclaim_*` ← `destroy-environment` 経由のみ。
  破棄対象はユーザーが `make-environment` で作った環境 → **frame は渡らない**

### 重点項目 1: `os_get_function_cell` に frame が渡ったとき

**確認。クラッシュはしないが、`nil` を返して前回 D の nil キャッシュバグを踏む。**

`runtime.c:3532-3556`:

```c
while (current_env != nil) {
    lisp_val_t func_slot = cc_car(cc_cdr(cc_cdr(current_env)));   /* 3番目。frame にもある */
    lisp_val_t alist = cc_cdr(func_slot);
    lisp_val_t pair = cc_assoc_eq(sym, alist);
    if (pair != nil) {
        lisp_val_t cells_slot = cc_car(cc_cdr(...5回...(current_env)));  /* frame なら nil */
        lisp_val_t cells_alist = cc_cdr(cells_slot);                     /* nil */
        lisp_val_t cell_pair = cc_assoc_eq(sym, cells_alist);            /* nil */
        return cc_cdr(cell_pair);                                        /* nil を返す */
    }
    /* 親へ */
}
```

**「frame では素通りして祖先 environment に cell を作る」という想定は成り立たない。**
`functions` スロットは frame にも存在するため、名前が frame で見つかると
**そこで打ち切って `nil` を返す**。素通りさせたいなら、この関数にも frame 判定が要る。

そして返った `nil` は `za_emit_fn_resolve_cached`(`za.c:3585`)が無条件に
キャッシュへ書き戻すため、前回 D で確認した**恒久的な破損**に直結する。
(nil キャッシュバグの修正は本調査の対象外だが、**案 A はこのバグの踏みやすさを上げる**。)

### 重点項目 2: `os_environment_reclaim_pages`

**確認。frame は渡らないので問題にならない。** `destroy-environment` からしか
呼ばれず、その対象はユーザーが作った環境。仮に渡っても `pages_slot` が `nil` で
`cc_cdr(nil) == nil` なのでループは 0 回、最後の
`((lisp_val_t *)pages_slot_addr)[1] = nil;`(`runtime.c:3609`)は
**`nil` の cdr に `nil` を書く**ので実害がない(たまたま無害)。

### 重点項目 3: `destroy-environment` の実装全体

**確認。**

```
destroy-environment (init.lisp:末尾)
  ├─ %environment-ancestor-p で現在の環境/祖先かを検査(そうならエラー)
  ├─ %%destroy-environment-reclaim (za.c:6170 付近 primitive_destroy_environment_reclaim)
  │    ├─ os_environment_reclaim_pages(target_env)          runtime.c:3597
  │    └─ os_environment_reclaim_literal_slots(target_env, za_free_literal_slot)  runtime.c:3629
  └─ *environments* から取り除く
```

**`cells` スロット(Function Cell)は解放していない。** → 項目 D 参照。

### 重点項目 4: frame に `defconstant` が来たときの現在の挙動

**確認。現状(8スロット)では正常に動くが、意味的には「消える定数」になる。**

`(let () (defconstant +k+ 1))` は `os_set_variable(name, val, CALL-ENV)` と
`os_mark_constant(name, CALL-ENV)` を実行する(`eval.c:302-303`)。
どちらも `CALL-ENV` に書かれ、`let` を抜けると消える。
**`defun` とまったく同じバグ**である(前回 C-3 の表のとおり)。

案 A で frame 化すると、`os_mark_constant` が `nil` を破壊する。

### A. 推測・未確認

- 「frame が渡りうるか」の判定は**呼び出し元の静的な追跡**によるもので、
  実際に frame を作って渡す実験はしていない(コードを変更しないため)。**推測**を
  含むのは `os_get_function_cell` の「frame では打ち切る」挙動で、これは
  コードの構造から明らかだが実行では確かめていない。
- `nil` 破壊が実際に処理系を即死させることは**実証していない**(**推測**)。
  `g_nil_cell` が固定領域であること、GC ルートに含まれず復元されないことは確認した。

---

## 調査項目 B: `os_set_function` / `os_set_variable` の呼び出し元の全列挙

### 使用した検索コマンド

```bash
grep -rn "os_set_function(" src/c/*.c | grep -v lisp_compiled
grep -rn "os_set_variable(" src/c/*.c | grep -v lisp_compiled
grep -c  "os_set_function(" src/c/lisp_compiled.c      # → 378
grep -c  "os_set_variable(" src/c/lisp_compiled.c      # → 0
grep -n  "os_set_function\|os_set_variable" src/c/za.c # JIT 生成コードからの呼び出し
# スロットを直接更新している箇所
grep -rn "cc_cdr(cc_cdr(env))\|cc_car(cc_cdr(env))" src/c/*.c | grep -v lisp_compiled
```

### `os_set_function`

| 呼び出し元 | 用途 | 渡している env | 分類 |
|---|---|---|---|
| `eval.c:386` `eval_defun` | `defun` | **呼び出し時の env** | **定義** |
| `eval.c:530` `eval_defmacro` | `defmacro` | **呼び出し時の env** | **定義** |
| `eval.c:454` `eval_flet` | flet 束縛 | `FLET-ENV`(新規作成) | 内部束縛 |
| `eval.c:493` `eval_labels` | labels 束縛 | `LABELS-ENV`(新規作成) | 内部束縛 |
| `eval.c:933,936` ほか | 組み込み登録 | `global_environment` | 定義(ブート時) |
| `runtime.c:2199` ほか多数 | 組み込み登録 | `global_environment` | 定義(ブート時) |
| `clock.c:35-44`, `format.c:243-249`, `bench_subprimitive.c:426-470`, `za.c:6179-6194`, `disasm_lisp.c` | 組み込み登録 | `global_environment` | 定義(ブート時) |
| **`za.c:5404`(JIT 生成コード)** | **flet/labels の gensym 登録** | **`global_environment`** | 内部束縛 |
| **`za.c:5439`(JIT 生成コード)** | **同、動的 extent 脱出時の復元** | **`global_environment`** | 内部束縛 |
| **`lisp_compiled.c` 378 箇所(AOT 生成物)** | AOT 関数の登録 | **すべて `global_environment`** | 定義(ブート時) |

**JIT 生成コードからの `os_set_function` は 2 箇所あり、いずれも
`global_environment` を渡している**(`za.c:5402-5403`, `5435-5436` で
`&global_environment` を movabs してから deref している)。設計意図は
`za.c:677-680` のコメントに明記:

> 生成コードはここから都度現在値をロードして
> `os_get_function`/`os_set_function`(…, `global_environment`)の第1引数に使う

**したがって JIT 生成コードは frame 判定の影響を受けない。**
AOT 生成物も 378 箇所すべて `global_environment` で、`global_environment` 以外を
渡している箇所は grep で 0 件だった。

### `os_set_variable`

| 呼び出し元 | 用途 | 渡している env | 分類 |
|---|---|---|---|
| `eval.c:110`, `eval.c:114` `bind_params` | **仮引数の束縛** | `call_env`(= frame) | **内部束縛** |
| `eval.c:281` `eval_defvar` | `defvar` | 呼び出し時の env | **定義** |
| `eval.c:302` `eval_defconstant` | `defconstant` | 呼び出し時の env | **定義** |
| `eval.c:345` `eval_defglobal` | `defglobal` | 呼び出し時の env | **定義** |
| `runtime.c:3395` `os_setq_variable` | `setq` のフォールバック | 呼び出し時の env | 内部束縛 |
| `interrupt.c:568` | `*current-process*` 更新 | `global_environment` | 内部束縛 |
| `process.c:356` | `*RUN-QUEUE*` 構築 | `global_environment` | 内部束縛 |
| `runtime.c:2233-2252` | ブート時の定数登録 | `global_environment` | 定義(ブート時) |
| **`za.c:2701`, `za.c:2743`(JIT 生成コード)** | **クロージャ捕捉環境への値のコピー** | **`LAMBDA-ENV`(新規作成)** | 内部束縛 |

`os_set_variable` は**2 番目のスロット**しか触らないので(`runtime.c:3335`)、
frame(4スロット)でも安全。AOT 生成物からの呼び出しは 0 件。

### スロットを直接更新している箇所

`eval_defvar`(`eval.c:274`)だけが `os_set_variable` を経由せず
`(variables . alist)` を自分で取り出して更新している。2 番目のスロットなので
frame でも安全。

### B. 結論

**「定義」に分類されるのは 6 つ** — `defun`(`eval.c:386`)/ `defmacro`(`530`)/
`defvar`(`281`)/ `defconstant`(`302`)/ `defglobal`(`345`)、およびブート時の
組み込み登録(これは `global_environment` 固定なので実質対象外)。

**frame へ書き込みうる「内部束縛」のうち、5 番目以降を触るのは
`eval_flet` / `eval_labels` の `os_set_function` だけ。** ここが案 A の最大の障害。

---

## 調査項目 C: 重複排除のフック位置と S 式の同一性

### C-1. フック位置

**確認。`za_try_compile_defun` の中にはフックできない。関数名を受け取っていないため。**

```c
lisp_val_t za_try_compile_defun(lisp_val_t params, lisp_val_t body, lisp_val_t env)  /* za.c:5819 */
```

引数は `params` / `body` / `env` の 3 つで、**`name` が無い**。
呼び出し元の `eval_defun`(`eval.c:382`)は `name` を持っている:

```c
lisp_val_t fn = za_try_compile_defun(params, body, env);   /* eval.c:382 */
if (fn == nil) {
    fn = make_interpreted_function(params, body, env);
}
os_set_function(name, fn, env);                            /* eval.c:386 */
```

したがって **(name, env, body) をキーにするなら `eval_defun` 側にフックするのが自然**。
`za_try_compile_defun` 側でやるなら `name` を引数に足す必要がある。

コード生成の前に判定できる位置はある。`za_try_compile_defun` は冒頭で
`za_validate_params` → `body` の検査を行い、実際のコード生成は `entry = g_jit_used`
(`za.c:5921`)以降なので、その手前で「既に持っているか」を引ければコード生成全体を
飛ばせる。

### C-2. 生成コードの中の環境依存・名前依存の埋め込み

**確認。前回挙げた 3 つ以外に、環境・名前に依存する埋め込みは見つからなかった。**

`jit_movabs_self_ref` / `za_emit_symbol_name` を使う箇所と、スロットアドレスを
`movabs` する箇所を洗い出した結果:

| 埋め込み | 内容 | 依存先 |
|---|---|---|
| (a) 自由変数名の文字列 | `za_emit_operand` の `is_literal==2/9`(`za.c:1455`, `1500`) | **body のみ** |
| (b) 呼び出し先名の文字列 | `za_emit_fn_resolve_cached`(`za.c:3574`, `3589`) | **body のみ** |
| (c) 呼び出し先キャッシュのスロットアドレス | `za.c:3568` | **コンパイルごとに新規** |
| (d) quote/number/lambda リテラルのスロットアドレス | `za_alloc_quote_slot` 等(`za.c:1470` 付近) | **コンパイルごとに新規** |
| (e) flet/labels の gensym スロットアドレス | `za.c:5399`, `5433` | **コンパイルごとに新規** |
| (f) C 関数のアドレス | `os_make_symbol` / `os_get_variable` 等 | 不変 |
| (g) `global_environment` のアドレス | `za.c:5402` | 不変(変数のアドレス) |

**(d) と (e) は前回挙がっていなかった。** いずれも「コンパイルごとに確保される
スロットのアドレス」で、(c) と同じ性質を持つ。つまり**コードを共有すると
リテラルスロットと gensym スロットも共有される**。

リテラルスロット(d)は `body` の quote 値を保持するだけなので、同じ body なら
共有して問題ない(**推測**。スロットは `os_gc_register_root` 済みで GC 追随する)。
gensym スロット(e)は flet/labels のあるコードでのみ現れ、gensym は
コンパイルごとに新しく作られるので、共有すると意味が変わる可能性がある(**推測**)。

環境そのもの(`env` の値)は**どこにも埋め込まれていない**。実行時に `ENV_VAL`
スロット(`ZA_OFF_ENV_VAL` = 16、`za.c:1205`)から読むだけで、生成コードは
環境のレイアウトにも中身にも依存しない。

### C-3. `body` の S 式の同一性判定

**確認。`eq`(ポインタ比較)は GC のコピー後も保たれる。**

Cheney GC は転送ポインタ方式で、`gc_copy_value`(`runtime.c:1561`)が
「word0 の下位 3bit が `TAG_FORWARD` か」でコピー済みを判定する
(`runtime.c:925` のコメント「gc_copy_value は word0 の下位3bitが TAG_FORWARD かで
転送済み判定」)。**1 オブジェクトにつきコピーは 1 回だけ**なので、
コピー前に `eq` だったものはコピー後も `eq`。

ただし**アドレスそのものは変わる**ので、キャッシュ表を「生のアドレス」で持つと
GC で壊れる。`lisp_val_t` として GC がスキャンする構造(alist 等)に置く必要がある。

ループ内の `defun` が毎回同じ cons 木を評価することは、インタプリタが同じ AST を
再評価する構造から従う(**推測**。実機で `eq` を確かめてはいない)。

### C-4. キャッシュ表から `za_fn_meta_t` を指す方法

**確認。`TAG_RAW_POINTER` がそのまま使える。**

`os_tag_is_heap_ref`(`runtime.h:37-42`)が `TAG_RAW_POINTER` に対して 0 を返すため、
GC は追わないし動かさない。既に `cells` スロット(Function Cell)と
`pages` スロットが同じ手口を使っている。Immobilized Space は動かないので整合する。

### C. 所見にかかわる重要な制約

**キーに登録先環境を含める必要がある、という依頼文の指摘は正しい。** ただし
理由は依頼文の想定より限定的になる:

生成コードは環境を埋め込まないので、**異なる環境で同じコードを実行すること自体は
安全**。危険なのは (c)(d)(e) のスロット共有だけで、特に (c) の呼び出し先キャッシュは
「最初に呼ばれたときの `ENV_VAL` で解決した cell」を固定してしまう。

**逆に言えば、修正後に `defun` の登録先が「最も近い祖先 environment」へ統一されると、
ループの各反復は同じ環境へ登録されることになるので、(c) の共有は安全になる**
(**推測**。呼び出し先が同じ環境で解決されるため)。

---

## 調査項目 D: 破棄済み環境の Function Cell とキャッシュのダングリング

> **再現手順は本節に先に記録した。実機での再現は試みていない**(危険性の判定が
> 目的であり、コードリーディングで結論が出たため)。

### D-1. `destroy-environment` は Function Cell を解放するか

**確認。解放しない。**

`primitive_destroy_environment_reclaim`(`za.c:6170` 付近)が呼ぶのは 2 つだけ:

- `os_environment_reclaim_pages`(`runtime.c:3597`)→ **`pages` スロット**のページを
  `os_imm_page_free` へ
- `os_environment_reclaim_literal_slots`(`runtime.c:3629`)→ `literal-slots` を
  za.c のフリーリストへ

**`cells` スロットには触れていない。** Function Cell は
`os_imm_slot_alloc(&g_function_cell_cursor, ...)`(`runtime.c:3509`)で確保され、
**解放経路が存在しない**(`g_function_cell_cursor` を巻き戻す処理も無い)。

→ **Function Cell 自体のダングリングは発生しない。** cell のアドレスをキャッシュ
している他の環境のコードは、破棄後も有効なメモリを読む。

### D-2/D-3. ただし別のダングリングがある

**確認(コードリーディング)。cell は生き残るが、cell が指す関数の
「コードページ」は解放される。**

```
destroy-environment E
  → E の pages(JITコードページ)を os_imm_page_free でフリーリストへ
  → E の cells はそのまま(中身 = 関数オブジェクト、word1 = za_fn_meta_t、
     meta->cons_entry = 解放済みページ上のアドレス)
```

さらに、解放されたページは `os_imm_page_alloc`(`runtime.c:1143-1148`)が
フリーリストから取り出して **Function Cell や `za_fn_meta_t` として再利用する**
(JIT コード用の `os_imm_pages_alloc_contiguous` はフリーリストを見ない —
前回 E-2b)。つまり**解放済みコードページはいずれメタデータで上書きされる**。

**危険。** ただし成立には「E で定義された関数オブジェクトが E の外へ脱出している」
ことが必要。

### 再現手順(記録のみ。実行していない)

```lisp
;; 1. 環境 E を作り、そこで関数を定義する
(defglobal *e* (make-environment 'dangle-env))
(defglobal *escaped* (%%eval-in-environment
                       '(progn (defun victim () 42) (function victim))
                       *e*))
;; 2. 脱出した関数オブジェクトを外から呼べることを確認する
(funcall *escaped*)          ; → 42 のはず
;; 3. E を破棄する(コードページがフリーリストへ戻る)
(destroy-environment *e*)
;; 4. フリーリストを使い切らせて、解放済みページをメタデータで上書きさせる
(defun burn (n) (let ((i 0)) (while (< i n) (progn (defun tmp-g () 1) (setq i (+ i 1))))))
;; ※ defun をループ内で書けないので、実際には toplevel の defun を大量に並べる
;; 5. 再度呼ぶ
(funcall *escaped*)          ; → 何が起きるか
```

**この手順は本修正とは独立した既存の穴**である。修正によって「ループ内 `let`+`defun`」
が書けるようになると `destroy-environment` を使う機会も増えるため、
踏む確率は上がると思われる(**推測**)。

### D-4. あわせて見つけた別の穴: frame の cells は GC で fixup されない

**確認。** `gc_fixup_environment_cells`(`runtime.c:2017`)は
`global_environment` と `get_process(i)->env` に対してしか呼ばれない
(`runtime.c:2168`, `2170`)。

一方 `os_set_function` は**どの環境に対しても** Function Cell を作る
(`runtime.c:3509`)。したがって `CALL-ENV` に `defun` した場合
(= まさに今回のバグ)、その cell の中身(関数オブジェクトへのタグ付きポインタ)は
**GC で更新されない**。

現状これが顕在化しないのは、`CALL-ENV` が即座に捨てられて誰も読まないため。
しかし次の形なら読まれる:

```lisp
(let ((x 1))
  (defun f () x)
  (defun g () (f))    ; g の JIT コードが f の cell アドレスをキャッシュする
  (g)                 ; ここで cell を解決してキャッシュ
  ;; … ここで GC が走ると、cell の中身は古い from-space アドレスのまま
  (g))                ; → stale な関数オブジェクトを読む
```

**危険。ただし実機での再現は試みていない(未確認)。**
修正で `defun` の登録先が祖先 environment(= `global_environment` や
`proc->env`)へ移れば、`gc_fixup_environment_cells` の対象に入るので**この穴は
修正によって塞がる**(**推測**)。

---

## 調査項目 E: JIT / AOT 経路への波及

### E-1. `za.c` の生成コードは環境のレイアウトを仮定しているか

**確認。していない。**

生成コードが環境に対して行うのは「`ENV_VAL` スロット(フレーム上のオフセット 16、
`za.c:1205`)から読んで、C 関数の引数として渡す」だけ。
`za_load_slot(ZA_REG_RDX, ZA_OFF_ENV_VAL)` は 7 箇所あり、渡し先は
`os_get_variable` / `os_get_function_cell` / `os_make_environment` /
`os_set_variable` / `os_set_function` / `os_apply_function`。

**環境の cons セルを直接 deref している生成コードは存在しない**
(`jit_mov_reg_from_mem_disp8` で構造体を辿るのは関数オブジェクト `obj[0]`〜`obj[3]` と
`za_fn_meta_t` のみで、環境ではない)。

→ **案 A のレイアウト変更は JIT 生成コードに影響しない。**

### E-2. AOT の `__lisp_lambda_N` 捕捉環境

**確認。`os_make_environment` で作られている。**

`grep -o "os_make_environment(..." src/c/lisp_compiled.c` でヒットし、環境名は
`os_make_symbol_cached(&__closure_env_name_idx, "__lisp_lambda_N")` の形。
生成の意図は `transpile.lisp:1680-1698` のコメントにある:

> 自由変数がある場合は、`os_make_environment`(親を持たない、この捕捉専用の環境)を
> 作り GC_PROTECT したのち、各自由変数について … `os_env_add_binding_pair` で
> (sym . 値)ペアとして連結する

**親を持たない(parent = nil)捕捉専用の環境**で、変数スロットしか使わない。
案 A で `os_make_environment` 自体を 4 スロット化しない限り影響を受けないし、
仮に 4 スロットにしても**安全**。`os_env_add_binding_pair`(`runtime.c:3423-3435`)が
触るのは 2 番目のスロットだけであることを確認した:

```c
lisp_val_t var_slot = cc_car(cc_cdr(env));          /* runtime.c:3426 (variables . alist) */
...
lisp_addr_t var_slot_addr = var_slot & ~TAG_MASK;
((lisp_val_t *)var_slot_addr)[1] = new_alist;       /* runtime.c:3434 */
```

自由変数が 1 つも無い場合は環境を作らず `global_environment` を渡す
(同コメント)。

### E-3. `LAMBDA-ENV` の用途と frame 化の可否

**確認。JIT が作るクロージャの捕捉環境。frame 化して安全と思われる。**

`za.c:2655` `za_emit_build_capture_env` のコメント:

> 2. 新規env = `os_make_environment`("LAMBDA-ENV", 現在のenv)を構築し、linkする。
> 3. TMPスロットを一度だけlinkし、外側の固定引数を1つずつ `os_set_variable` で …

捕捉のために使うのは **2 番目のスロット(`os_set_variable`)だけ**
(`za.c:2701`, `2743`)。`os_set_function` は呼ばない。

ただし `LAMBDA-ENV` は**クロージャの word3 になる**ので、そのクロージャの本体が
インタプリタ実行で `defun` を含むと `eval_defun` → `os_set_function(LAMBDA-ENV)` に
なりうる(**推測**。そのような形が実際に構成できるかは未確認)。

---

## 調査項目 F: 前回の積み残し

### F-1. AOT 関数を再定義したとき、AOT 側の呼び出し元は追従するか

**確認。追従しない。** AOT 生成コードは**リンク時に確定する C 関数を直接呼ぶ**。

`transpile.lisp:1885-1889`:

> ABI-M6: name が `*known-function-names*` に載っており(defunされた関数)、かつ …
> `name__fixed(env, arg0, ...)` を直接 call する(consチェーン構築を経由しない) …
> 間接呼び出しではなく元々リンク時に確定する C 関数名の直接呼び出しのため

生成物にも `lisp_ll_slot_value__fixed(` が 200 箇所、`lisp_ll_list(` が 66 箇所など、
直接呼び出しの形で現れる。

実機実測:

```
#F1 再定義前 (assoc 'b '((a . 1) (b . 2))) = (B . 2)
#F1 再定義後 直接 (assoc ...) = REDEFINED              ← Lisp から呼ぶと新定義
#F1 AOT呼び出し元 (%find-class '<object>) = #<CLASS <OBJECT>>   ← 旧定義のまま正常動作
```

`%find-class`(`init_aot.lisp:329`)は内部で `(assoc name (dynamic *classes*))` を
呼ぶが、`assoc` を `'REDEFINED` を返すよう再定義しても**正常にクラスを返した**。
AOT の `%find-class` は `lisp_ll_assoc` を直接呼び続けている。

**つまり「焼き込み方式」は JIT ではなく AOT に存在する。** JIT は Function Cell 経由で
追従する(前回 D)。

### F-2. `with-environment` の巻き戻しと、その中の `defun` の登録先

**確認。巻き戻る。`defun` は対象環境へ正しく登録される。**

`with-environment`(`init.lisp:1064-1072`)は `unwind-protect` で
`%%set-current-environment` を復元し、body は
`(%%eval-in-environment '(progn ,@body) target)` で対象環境上で評価する。

実機実測:

```
#F2 with-environment の戻り値 = WE-FN
#F2 [誤った測り方] 切替えてから呼ぶ = EVAL-ERROR
#F2 [正しい測り方] %%eval-in-environment = 7        ← we-env で評価すれば呼べる
#F2 グローバルから直接        = EVAL-ERROR           ← グローバルには居ない
```

**`(with-environment env (defun f () 7))` は env の関数スロットへ正しく登録される。**
これは「環境ごとに関数を定義できる」という維持したい仕様そのものであり、
今回のバグとは別物。

> **測定上の注意(自分の誤りの記録)**: 最初 `(progn (switch-environment 'we-env) (we-fn))`
> で確かめようとして `EVAL-ERROR` になり、「登録されていない」と誤判定しかけた。
> `switch-environment` は `proc->env` を変えるだけで、**実行中の式のレキシカル env は
> 変わらない**ため、`(we-fn)` の解決は元の環境で行われる。
> 正しくは `%%eval-in-environment` で対象環境を明示する。

---

## 所見(ここから先は推測を含む)

> **以下は実装方針への示唆であり、コードで裏を取った事実ではない。**

### 1. 案 A は「書き込み 4 箇所」を塞げば成立する

読み出しが `nil` で安全に着地する(自己参照 nil)のは案 A にとって大きな追い風。
塞ぐべきは次の 4 つに限定される。

| 関数 | frame が来る経路 | 考えられる手当て |
|---|---|---|
| `os_set_function`(cells) | `eval_flet` / `eval_labels` | **FLET-ENV / LABELS-ENV は 8 スロットのままにする**のが最も安全 |
| `os_mark_constant`(constants) | `eval_defconstant` | `defconstant` も `os_define_function` と同じ「祖先を探す」経路に載せる |
| `os_environment_register_pages`(pages) | `za_try_compile_defun` | **登録先を「祖先 environment」へ変える必要がある。`os_define_function` だけでは足りない** |
| `os_environment_register_literal_slot` | 同上 | 同上 |

**最小の frame 化は「`CALL-ENV` と `MACRO-ENV` だけ 4 スロット」**で、
`FLET-ENV` / `LABELS-ENV` / `LAMBDA-ENV` は 8 スロットのまま据え置くのが、
触る面積が最も小さいと思われる。frame 判定(「5番目以降があるか」)は
`cc_cdr` を 4 回辿って `nil` かどうかで足りる。

ただしその場合、frame 判定の意味が「`CALL-ENV`/`MACRO-ENV` か」になり、
`FLET-ENV` などは environment 扱いになるので、
**`(flet ((h () 1)) (defun f () 1))` は `FLET-ENV` へ登録されたまま**になる。
S-3 で確認したとおりこれも現状バグなので、そこは直らない。

### 2. `za_try_compile_defun` のページ登録が見落としやすい

`os_define_function` を入れて `eval_defun` の登録先だけ変えても、
**`za.c:6124` / `6130` のページ・リテラルスロット登録は元の env のまま**である。
案 A で frame 化するとここで `nil` を破壊する。
案 A を採らない(8 スロットのまま)場合でも、ページが frame に登録されると
**`destroy-environment` の対象にならないため回収されない**(前回 E の
リークがそのまま残る)。**登録先の決定は 1 箇所ではなく 3 箇所**である。

### 3. 案 B(名前判定)を推奨しない理由

`name` スロットの値はユーザーが `make-environment` の第 1 引数で自由に決められる
(`init.lisp:1009`)。`(make-environment 'call-env)` と書けば frame と誤判定される。
`os_make_environment` の TODO(`runtime.c:2917-2921`)が指摘するユニーク性の欠如が
そのまま安全性の欠如になる。案 A の構造判定(スロット数)にはこの問題が無い。

### 4. 重複排除より先に、登録先の統一が効く

前回 E で測った 4,144 byte/defun の消費は、**登録先が統一されれば
`destroy-environment` で回収可能になる**(現状は frame に登録されるので回収経路に
乗らない)。ただし JIT コードページはフリーリストから再利用されない
(前回 E-2b)ため、回収しても bump は戻らない。
重複排除を入れるなら C-2 の (c)(d)(e) のスロット共有が論点になる。

### 5. nil キャッシュバグ(対象外)との関係

本調査の対象外だが、案 A は `os_get_function_cell` が frame で `nil` を返す経路を
新設することになるため(重点項目 1)、**nil キャッシュバグを先に潰しておくほうが
安全**だと思われる。

---

## 未調査・積み残し

| 項目 | 内容 |
|---|---|
| A | `nil` 破壊が実際に処理系を即死させることの実証(コードを変更しないため未実施) |
| A | `os_get_function_cell` が frame で打ち切る挙動の実行時確認 |
| C | ループ内 `defun` の body cons 木が毎回 `eq` であることの実機確認 |
| C | リテラルスロット(d)・gensym スロット(e)を共有したときの意味論 |
| D | ダングリングの実機再現(手順は本文に記録。危険と判断し未実行) |
| D | frame の cells が GC で fixup されない件の実機再現 |
| E | `LAMBDA-ENV` を word3 に持つクロージャの本体で `defun` する形が構成可能か |

---

## 付録: 測定に使った一時スクリプト

**いずれもコミットしておらず、実行後に削除済み。新規に C コードを足した箇所は無い。**

### S(仮説検証)

```lisp
(defun outer () (defun inner () 42))
(format *isiki-test-stream* "#S outer compiled=~S~%" (%%za-compiled-p (function outer)))
(defglobal *s1* (outer))
(format *isiki-test-stream* "#S 外から (inner) = ~S~%"
        (block b (with-handler (lambda (c) (return-from b 'ERROR)) (inner))))
(defun outer2 () (progn (defun inner2 () 43) (inner2)))
(format *isiki-test-stream* "#S 中から (inner2) = ~S~%" (outer2))
(defun outer3 () (flet ((h () 1)) (defun inner3 () 44)))
(defglobal *s3* (outer3))
(defun outer4 () (labels ((h () 1)) (defun inner4 () 45)))
(defglobal *s4* (outer4))
```

### F(AOT 再定義 / with-environment)

```lisp
(format *isiki-test-stream* "#F1 再定義前 = ~S~%" (assoc 'b '((a . 1) (b . 2))))
(defun assoc (key alist) 'REDEFINED)
(format *isiki-test-stream* "#F1 再定義後 直接 = ~S~%" (assoc 'b '((a . 1) (b . 2))))
(format *isiki-test-stream* "#F1 AOT呼び出し元 = ~S~%" (%find-class '<object>))

(defglobal *we-env* (make-environment 'we-env))
(defglobal *we-r* (with-environment *we-env* (defun we-fn () 7)))
(format *isiki-test-stream* "#F2 %%eval-in-environment = ~S~%"
        (%%eval-in-environment '(we-fn) *we-env*))
```
