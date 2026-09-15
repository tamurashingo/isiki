# 調査報告: let 内 defun

> 実施: 2026-09-15 / ブランチ: `feature/compiler-optimization`(分岐なし)
> **本調査ではコードを一切変更していない。** 測定は一時的な Lisp スクリプトのみで行い、
> 実行後に削除済み(作業ツリーは `documents/abi-redesign.md` が未追跡である以外クリーン)。
>
> 表記規約:
> - 「**確認**」= コードを読んだ、または実機(QEMU)で実測した。必ず根拠を示す
> - 「**推測**」= 裏を取れていない。そう明示する

---

## サマリ

1. **B の判定 = 「2. フレームへのポインタ保持」。** 値の焼き込みではない。自由変数は
   変数名の文字列を機械語に埋め込み、呼び出しのたびに `os_make_symbol` →
   `os_get_variable(sym, env)` で環境チェーンを辿る**完全な遅延束縛**。実測で
   `(setq x 2)` 後に `(f3)` が `2` を返すことを確認した。
2. `let` が作るものは `make-environment` が作るものと**完全に同一の型・同一の関数**
   (`os_make_environment`)。名前が `CALL-ENV` である点と `*environments*` に登録
   されない点以外に区別材料はない。
3. 登録先を決めているのは `eval_defun` の **1 行**(`eval.c:386`)。`defmacro`
   (`eval.c:530`)・`defgeneric`(`defun` へ展開)も同じ経路で同じバグを持つ。
4. 関数呼び出しは **Function Cell の間接参照**で、再定義に追従する(実測)。ただし
   **未定義のまま呼ぶと nil が恒久キャッシュされ、あとで定義しても直らない**別バグを発見した。
5. immobilized space に**重複排除は無い**。再定義 1 回あたり 4,160 byte を消費し、
   JIT コードページは解放しても**再利用されない**(確保側が free list を見ていない)。

---

## 調査項目 A: 環境と関数スロットの表現

### A-1/A-3. 環境オブジェクトのデータ構造

**確認。** 環境は構造体ではなく、**8 個の `(key . value)` ペアからなる cons リスト**。

`src/c/runtime.c:2925` `os_make_environment` が組み立てている(同 2975-3000 が実体):

```c
lisp_val_t name_slot          = os_make_cons(name_symbol, env_symbol);
lisp_val_t variables_slot     = os_make_cons(variables_symbol, nil);
lisp_val_t functions_slot     = os_make_cons(functions_symbol, nil);
lisp_val_t parent_slot        = os_make_cons(parent_symbol, parent_env);
lisp_val_t constants_slot     = os_make_cons(constants_symbol, nil);
lisp_val_t cells_slot         = os_make_cons(cells_symbol, nil);
lisp_val_t pages_slot         = os_make_cons(pages_symbol, nil);
lisp_val_t literal_slots_slot = os_make_cons(literal_slots_symbol, nil);
/* ... 8 個を list にする ... */
```

実機で確認した実際の形(スロット名の並び):

```
#envs 環境はconsリストか=T 長さ=8
#envs スロット名=(NAME VARIABLES FUNCTIONS PARENT CONSTANTS CELLS PAGES LITERAL-SLOTS)
```

| # | スロット | 内容 |
|---|---|---|
| 1 | `name` | 環境名のシンボル |
| 2 | `variables` | 変数の alist `(sym . value)` |
| 3 | `functions` | **関数の alist** `(sym . 関数オブジェクト)` |
| 4 | `parent` | 親環境(ルートは `nil`) |
| 5 | `constants` | `defconstant` 済みシンボルの記録 |
| 6 | `cells` | `(sym . Function Cell アドレス)` の alist |
| 7 | `pages` | この環境が所有する Immobilized Page のリスト |
| 8 | `literal-slots` | 同、za.c のリテラルスロット |

**変数スロットと関数スロットは別。** `os_set_variable` は 2 番目
(`runtime.c:3335` `cc_car(cc_cdr(env))`)、`os_set_function` は 3 番目
(`runtime.c:3471` `cc_car(cc_cdr(cc_cdr(env)))`)を見ている。

**親リンク**は 4 番目のスロット(`runtime.c:3547-3552` の `os_get_function_cell` が
`cdr` を 3 回辿って `parent` を取り出している)。

> 依頼文の前提「environment の実体は alist」は**半分正しい**。環境そのものは
> 「8 個の固定スロットを持つリスト」で、その中の `variables` / `functions` /
> `cells` スロットの**値**が alist。

### A-2. 関数スロットに入っている値の型

**確認。** 入るのは生アドレスではなく **Lisp の関数オブジェクト(`TAG_INSTANCE`)**。
word0 の MAGIC で 2 種類を区別する(`src/c/runtime.c:75-86` のレイアウト表)。

| | `MAGIC_FUNCTION_NATIVE` | `MAGIC_FUNCTION_INTERPRETED` |
|---|---|---|
| word1 | `za_fn_meta_t*`(**Immobilized Space 上の生ポインタ**) | `params`(S 式) |
| word2 | `nil`=組み込み / fixnum 1=JIT / fixnum 2=AOT lifted closure | `body`(S 式) |
| word3 | 定義時環境 | クロージャ環境 |

`za_fn_meta_t`(`src/c/runtime.h:854`)が実際のコードアドレスを持つ:

```c
typedef struct {
    UINT64 cons_entry;   /* offset 0  従来のconsリストABI */
    UINT64 fixed_entry;  /* offset 8  ABI-M5の固定引数エントリ */
    UINT64 arity;        /* offset 16 */
    UINT64 code_base;    /* offset 24 Phase1(disassembler)で追加 */
    UINT64 code_len;     /* offset 32 同上 */
} za_fn_meta_t;
```

**依頼文の前提「関数スロットの alist には、シンボルと immobilized space 上の
アドレスが登録される」は不正確。** 登録されるのは GC ヒープ上の関数オブジェクトで、
そのオブジェクトが Immobilized Space 上の `za_fn_meta_t` を指し、さらにそれが
コードアドレスを持つ、という 2 段の間接。

**「コンパイルされない場合は S 式と紐付く」は正しく、同じスロットに同じ型
(`TAG_INSTANCE`)で入る。区別は word0 の MAGIC(= タグ付き)。** 別フィールドではない。
`eval_defun`(`src/c/eval.c:382-385`)がその分岐そのもの:

```c
lisp_val_t fn = za_try_compile_defun(params, body, env);
if (fn == nil) {
    fn = make_interpreted_function(params, body, env);
}
os_set_function(name, fn, env);
```

Immobilized Space の生アドレスが直接入るのは **`cells` スロット(6 番目)**のほうで、
こちらは `TAG_RAW_POINTER` 付き(`runtime.h:786` の `os_get_function_cell` の説明)。

### A-4. `let` / `lambda` 適用時に作られるものの型

**確認。完全に同一の型で、同一の関数から作られる。関数スロットも持つ。**

`src/c/eval.c:152`(`apply_function` の `MAGIC_FUNCTION_INTERPRETED` 分岐):

```c
lisp_val_t call_env = os_make_environment(os_make_symbol("CALL-ENV"), closure_env);
GC_PROTECT(call_env);
bind_params(params, evaluated_args, call_env);
return eval_progn(body, call_env);
```

`make-environment` が呼ぶのも同じ `os_make_environment`
(`runtime.c:6338` の `primitive_make_environment`)。**違いは第 1 引数の名前だけ。**

`let` はマクロ(`src/lisp/init.lisp:31`)で IIFE へ展開されるので、この
`apply_function` を通る:

```lisp
(defmacro let (bindings &rest body)
  `((lambda ,(%let-vars bindings) ,@body) ,@(%let-inits bindings)))
```

**`let` 適用時に実際に確保されるもの(コードの流れ順)**:

1. `eval_form`(`eval.c:172`)が `(let ...)` をマクロと判定 → `apply_macro`
2. `apply_macro`(`eval.c:562`)が **`MACRO-ENV` を 1 つ確保**し、マクロ本体を評価 →
   展開形 `((lambda (x) ...) 1)` の cons 木を確保
3. 展開形を評価。演算子位置の `(lambda ...)` を `eval_lambda` が評価 →
   `MAGIC_FUNCTION_INTERPRETED` オブジェクトを確保
4. 引数 `1` を評価して引数リストの cons を確保
5. `apply_function` が **`CALL-ENV` を 1 つ確保**(`eval.c:152`)
6. `bind_params` が `(x . 1)` を `CALL-ENV` の `variables` alist へ追加

**つまりインタプリタの `let` は 1 回の評価につき環境を 2 つ作る。**
実測(QEMU、GC 0 回、500 回平均):

| 測定 | byte/call |
|---|---|
| 関数呼び出しのみ(`let` なし) | 336 |
| 手書き IIFE(マクロ展開なし) | 672 |
| `let` | **1152** |
| 環境 1 つぶん(`%%make-environment` 1 回、引数リスト 32 byte 込み) | 288 |

環境 1 つは 16 cons × 16 byte = **256 byte**(`os_make_environment` が作る
spine 8 + pair 8)。

なお **JIT 経路では `let` は環境を作らない**(`za_compile_let`、`src/c/za.c:2881` が
IIFE をスタックスロットへインライン展開)。実測で JIT された `let` の確保量は
**0 byte/call**。AOT 経路も同様に C のブロックへ展開する
(`src/lisp/transpile.lisp:1120` `transpile-inline-immediate-lambda`)。

### A-5. `flet` / `labels`

**確認。両方とも実装されている。**

| | 生成する環境 | 場所 |
|---|---|---|
| `flet` | `FLET-ENV` | `src/c/eval.c:440` |
| `labels` | `LABELS-ENV` | `src/c/eval.c:479` |

束縛先は**新しく作ったその環境の関数スロット**。`eval_flet`(`eval.c:428-450`)が
`os_make_environment("FLET-ENV", env)` を作り、ループで各 binding を
`os_set_function(name, fn, new_env)` している。つまり **`let` 内 `defun` と同じ
構造**(新環境の関数スロットへ書き、抜けると到達不能)だが、`flet`/`labels` の場合は
それが**仕様どおり**である点が異なる。

### A. 環境名の一覧(確認)

| 名前 | 作られるとき | 場所 |
|---|---|---|
| `GLOBAL-ENV` | ブート時 1 回 | `runtime.c:2227` |
| `CALL-ENV` | **インタプリタの関数適用(`let` の IIFE を含む)** | `eval.c:152` |
| `FLET-ENV` / `LABELS-ENV` | flet / labels | `eval.c:440` / `479` |
| `MACRO-ENV` | マクロ展開 | `eval.c:562` |
| `LAMBDA-ENV` | JIT のクロージャ捕捉 | `za.c:2665`, `2766` |
| `__lisp_lambda_N` | AOT のリフト済みクロージャ捕捉環境 | `lisp_compiled.c`(生成物) |
| `F1`〜`F4` | プロセスの REPL 環境 | `os_make_process_environment` |
| ユーザー指定 | `make-environment` | `init.lisp:1009` |

### A. 推測・未確認

- `os_make_environment` のコメント(`runtime.c:2917-2921`)に「呼び出し元が渡す
  `env_symbol` はユニークであることが保証されていない」という TODO がある。
  同名環境が複数できたときに `switch-environment` がどれを引くかは
  `%environment-find-by-name-in`(`init.lisp:1021`)が**先頭一致**で返すが、
  この状況を実際に作って確かめてはいない(**未確認**)。

---

## 調査項目 B: let 内 defun のコンパイル結果 — 自由変数の扱い

### 判定: **2. フレームへのポインタ保持**

より正確には「**関数オブジェクトの word3 に定義時環境(= `let` の `CALL-ENV`)を
保持し、自由変数は実行時に `os_get_variable` で環境チェーンを辿る**」。
値の焼き込み(1)でも、スタック直接参照(3)でも、グローバル解決(4)でも、
コンパイルエラー(5)でもない。

### 根拠 1: コードを読んだ結果

`src/c/za.c:934-940`(`za_classify_operand`)。local/param/rest/裸の `T` の
いずれでもない裸シンボルの扱い:

```c
// 拡張19: local/param/rest/裸のTのいずれでもない裸シンボルは、eval.cのシンボル
// 評価(os_get_variableへの委譲)と同じくグローバル/未束縛変数の読み込みとして
// 扱う。コンパイル時に「本当にdefglobalか」を判定する必要は無く、実行時に
// os_get_variableがenvの親チェーンを辿って解決する(インタプリタと同じ)。
out->is_literal = 9;
out->literal = form;
return 1;
```

その emit(`src/c/za.c:1495-1507`):

```c
if (op->is_literal == 9) {
    UINT64 name_off = za_emit_symbol_name(op->literal);   // 変数名をコードへ埋め込む
    jit_movabs_self_ref(ZA_REG_RCX, name_off);
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_make_symbol);
    jit_call_r11();                                        // sym = os_make_symbol("X")
    jit_mov_reg_reg(ZA_REG_RCX, ZA_REG_RAX);
    za_load_slot(ZA_REG_RDX, ZA_OFF_ENV_VAL);              // env = 呼び出し時のENV
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_get_variable);
    jit_call_r11();                                        // os_get_variable(sym, env)
}
```

`ZA_OFF_ENV_VAL` は 16(`za.c:1205`)。この env は `apply_function`
(`eval.c:145`)が `word3`(定義時環境)を優先して渡す:

```c
lisp_val_t call_env = (obj[3] != nil) ? obj[3] : env;
```

そして `za_try_compile_defun` の末尾(`za.c:6141` 付近)が
`os_make_jit_function(cons_entry_addr, env)` で **コンパイル時の env を word3 に
入れている**。`let` の中で `defun` した場合、この env が `let` の `CALL-ENV`。

### 根拠 2: 実機での逆アセンブル(f2、オフセット 0x00ea〜0x012c)

```
00ea  e9 02 00 00 00                   jmp 0xf1
00ef  58 00                            .asciz "X"                        ← 変数名がコードに埋め込まれている
00f1  48 b9 ef 90 cd 0c 00 00 00 00    movabs rcx, 0xccd90ef  ; <immobilized>   ← その "X" のアドレス
00fb  49 bb 90 d3 7a 0c 00 00 00 00    movabs r11, 0xc7ad390  ; <kernel>        ← os_make_symbol
0105  48 83 ec 20                      sub rsp, 0x20
0109  41 ff d3                         call r11
010c  48 83 c4 20                      add rsp, 0x20
0110  48 89 c1                         mov rcx, rax                       ← 第1引数 = sym
0113  48 8b 94 24 10 00 00 00          mov rdx, [rsp+0x10]                ← 第2引数 = ENV_VAL(=16)
011b  49 bb a6 cf 7a 0c 00 00 00 00    movabs r11, 0xc7acfa6  ; <kernel>        ← os_get_variable
0125  48 83 ec 20                      sub rsp, 0x20
0129  41 ff d3                         call r11
012c  48 83 c4 20                      add rsp, 0x20
```

`; <kernel>` / `; <immobilized>` の注釈は Phase 1 の disassembler が付けたもの。
**即値としての `1` はどこにも現れない。**

### 根拠 3: 実機での挙動(遅延束縛の直接確認)

```
#B1 compiled=T (f1)=42 code-len=298     ; 自由変数なし(対照)
#B2 compiled=T (f2)=1   code-len=358     ; let変数を参照 → 正しく 1 が見える
#B3 setq前 (f3)=1
#B3 setq後 (f3)=2 compiled=T             ; ★ setq が反映される = 焼き込みではない
#B  letを抜けたあと (f2) => EVAL-ERROR
```

**`(setq x 2)` の後に `(f3)` が `2` を返す**ことが、値の焼き込み(判定 1)を明確に
否定する。コード長の差(298 → 358 = 60 byte)も、上の
`os_make_symbol` + `os_get_variable` 呼び出し列 1 組ぶんとして辻褄が合う。

### `let` を抜ける前に関数スロットを覗いて呼ぶ方法

**確認。特別な手段は不要で、`let` の本体の中で普通に呼べる。**
上記 `#B2` / `#B3` がそれ(`(f2)` / `(f3)` を `let` 本体内で評価している)。
`x` の値は**正しく見える**。ゴミも返らず、落ちもしない。

逆アセンブルの取得も `let` 本体内で `(disassemble-to-stream stream (function f2))` と
書けば可能。`(function f2)` が `CALL-ENV` で解決されるため。
なお `(disassemble 'f2)` のように**シンボルで渡すと失敗する** — `%%disasm-code-base`
は渡された env でシンボルを解決するが、`disassemble` 経由だとその env は
`disassemble` の定義時環境(グローバル)になり `f2` が見えないため。

### 3 ケースの逆アセンブル全文

#### ケース 1: `(let ((x 1)) (defun f1 () 42))` — 自由変数なし(対照)

```
Function: #<FUNCTION-COMPILED>
Code:   298 bytes at 0xCCD8000
Entry:  cons=0x61  fixed=0x0
offset  bytes                            instruction
0000  53                               push rbx
0001  41 55                            push r13
0003  48 81 ec 38 1d 00 00             sub rsp, 0x1d38
000a  4c 89 b4 24 00 1d 00 00          mov [rsp+0x1d00], r14
0012  48 89 8c 24 10 00 00 00          mov [rsp+0x10], rcx
001a  49 bb ab 77 7b 0c 00 00 00 00    movabs r11, 0xc7b77ab              ; <kernel>
0024  48 83 ec 20                      sub rsp, 0x20
0028  41 ff d3                         call r11
002b  48 83 c4 20                      add rsp, 0x20
002f  48 89 84 24 00 00 00 00          mov [rsp], rax
0037  48 8d 8c 24 18 00 00 00          lea rcx, [rsp+0x18]
003f  48 8d 94 24 10 00 00 00          lea rdx, [rsp+0x10]
0047  49 bb cb 77 7b 0c 00 00 00 00    movabs r11, 0xc7b77cb              ; <kernel>
0051  48 83 ec 20                      sub rsp, 0x20
0055  41 ff d3                         call r11
0058  48 83 c4 20                      add rsp, 0x20
005c  e9 89 00 00 00                   jmp 0xea
0061  53                               push rbx
0062  41 55                            push r13
0064  48 81 ec 38 1d 00 00             sub rsp, 0x1d38
006b  4c 89 b4 24 00 1d 00 00          mov [rsp+0x1d00], r14
0073  48 89 8c 24 28 00 00 00          mov [rsp+0x28], rcx
007b  48 89 94 24 10 00 00 00          mov [rsp+0x10], rdx
0083  49 bb ab 77 7b 0c 00 00 00 00    movabs r11, 0xc7b77ab              ; <kernel>
008d  48 83 ec 20                      sub rsp, 0x20
0091  41 ff d3                         call r11
0094  48 83 c4 20                      add rsp, 0x20
0098  48 89 84 24 00 00 00 00          mov [rsp], rax
00a0  48 8d 8c 24 18 00 00 00          lea rcx, [rsp+0x18]
00a8  48 8d 94 24 10 00 00 00          lea rdx, [rsp+0x10]
00b0  49 bb cb 77 7b 0c 00 00 00 00    movabs r11, 0xc7b77cb              ; <kernel>
00ba  48 83 ec 20                      sub rsp, 0x20
00be  41 ff d3                         call r11
00c1  48 83 c4 20                      add rsp, 0x20
00c5  48 8d 8c 24 30 00 00 00          lea rcx, [rsp+0x30]
00cd  48 8d 94 24 28 00 00 00          lea rdx, [rsp+0x28]
00d5  49 bb cb 77 7b 0c 00 00 00 00    movabs r11, 0xc7b77cb              ; <kernel>
00df  48 83 ec 20                      sub rsp, 0x20
00e3  41 ff d3                         call r11
00e6  48 83 c4 20                      add rsp, 0x20
00ea  48 b8 50 01 00 00 00 00 00 00    movabs rax, 0x150
00f4  49 89 c5                         mov r13, rax
00f7  48 8b 8c 24 00 00 00 00          mov rcx, [rsp]
00ff  49 bb 0a 78 7b 0c 00 00 00 00    movabs r11, 0xc7b780a              ; <kernel>
0109  48 83 ec 20                      sub rsp, 0x20
010d  41 ff d3                         call r11
0110  48 83 c4 20                      add rsp, 0x20
0114  4c 89 e8                         mov rax, r13
0117  4c 8b b4 24 00 1d 00 00          mov r14, [rsp+0x1d00]
011f  48 81 c4 38 1d 00 00             add rsp, 0x1d38
0126  41 5d                            pop r13
0128  5b                               pop rbx
0129  c3                               ret
```

#### ケース 2: `(let ((x 1)) (defun f2 () x))` — let 変数を参照

```
Function: #<FUNCTION-COMPILED>
Code:   358 bytes at 0xCCD9000
Entry:  cons=0x61  fixed=0x0
offset  bytes                            instruction
0000  53                               push rbx
0001  41 55                            push r13
0003  48 81 ec 38 1d 00 00             sub rsp, 0x1d38
000a  4c 89 b4 24 00 1d 00 00          mov [rsp+0x1d00], r14
0012  48 89 8c 24 10 00 00 00          mov [rsp+0x10], rcx
001a  49 bb ab 77 7b 0c 00 00 00 00    movabs r11, 0xc7b77ab              ; <kernel>
0024  48 83 ec 20                      sub rsp, 0x20
0028  41 ff d3                         call r11
002b  48 83 c4 20                      add rsp, 0x20
002f  48 89 84 24 00 00 00 00          mov [rsp], rax
0037  48 8d 8c 24 18 00 00 00          lea rcx, [rsp+0x18]
003f  48 8d 94 24 10 00 00 00          lea rdx, [rsp+0x10]
0047  49 bb cb 77 7b 0c 00 00 00 00    movabs r11, 0xc7b77cb              ; <kernel>
0051  48 83 ec 20                      sub rsp, 0x20
0055  41 ff d3                         call r11
0058  48 83 c4 20                      add rsp, 0x20
005c  e9 89 00 00 00                   jmp 0xea
0061  53                               push rbx
0062  41 55                            push r13
0064  48 81 ec 38 1d 00 00             sub rsp, 0x1d38
006b  4c 89 b4 24 00 1d 00 00          mov [rsp+0x1d00], r14
0073  48 89 8c 24 28 00 00 00          mov [rsp+0x28], rcx
007b  48 89 94 24 10 00 00 00          mov [rsp+0x10], rdx
0083  49 bb ab 77 7b 0c 00 00 00 00    movabs r11, 0xc7b77ab              ; <kernel>
008d  48 83 ec 20                      sub rsp, 0x20
0091  41 ff d3                         call r11
0094  48 83 c4 20                      add rsp, 0x20
0098  48 89 84 24 00 00 00 00          mov [rsp], rax
00a0  48 8d 8c 24 18 00 00 00          lea rcx, [rsp+0x18]
00a8  48 8d 94 24 10 00 00 00          lea rdx, [rsp+0x10]
00b0  49 bb cb 77 7b 0c 00 00 00 00    movabs r11, 0xc7b77cb              ; <kernel>
00ba  48 83 ec 20                      sub rsp, 0x20
00be  41 ff d3                         call r11
00c1  48 83 c4 20                      add rsp, 0x20
00c5  48 8d 8c 24 30 00 00 00          lea rcx, [rsp+0x30]
00cd  48 8d 94 24 28 00 00 00          lea rdx, [rsp+0x28]
00d5  49 bb cb 77 7b 0c 00 00 00 00    movabs r11, 0xc7b77cb              ; <kernel>
00df  48 83 ec 20                      sub rsp, 0x20
00e3  41 ff d3                         call r11
00e6  48 83 c4 20                      add rsp, 0x20
00ea  e9 02 00 00 00                   jmp 0xf1
00ef  58 00                            .asciz "X"
00f1  48 b9 ef 90 cd 0c 00 00 00 00    movabs rcx, 0xccd90ef              ; <immobilized>
00fb  49 bb 90 d3 7a 0c 00 00 00 00    movabs r11, 0xc7ad390              ; <kernel>
0105  48 83 ec 20                      sub rsp, 0x20
0109  41 ff d3                         call r11
010c  48 83 c4 20                      add rsp, 0x20
0110  48 89 c1                         mov rcx, rax
0113  48 8b 94 24 10 00 00 00          mov rdx, [rsp+0x10]
011b  49 bb a6 cf 7a 0c 00 00 00 00    movabs r11, 0xc7acfa6              ; <kernel>
0125  48 83 ec 20                      sub rsp, 0x20
0129  41 ff d3                         call r11
012c  48 83 c4 20                      add rsp, 0x20
0130  49 89 c5                         mov r13, rax
0133  48 8b 8c 24 00 00 00 00          mov rcx, [rsp]
013b  49 bb 0a 78 7b 0c 00 00 00 00    movabs r11, 0xc7b780a              ; <kernel>
0145  48 83 ec 20                      sub rsp, 0x20
0149  41 ff d3                         call r11
014c  48 83 c4 20                      add rsp, 0x20
0150  4c 89 e8                         mov rax, r13
0153  4c 8b b4 24 00 1d 00 00          mov r14, [rsp+0x1d00]
015b  48 81 c4 38 1d 00 00             add rsp, 0x1d38
0162  41 5d                            pop r13
0164  5b                               pop rbx
0165  c3                               ret
```

#### ケース 3: `(let ((x 1)) (defun f3 () x) (setq x 2))` — 参照 + 書き換え

```
Function: #<FUNCTION-COMPILED>
Code:   358 bytes at 0xCCDA000
Entry:  cons=0x61  fixed=0x0
offset  bytes                            instruction
0000  53                               push rbx
0001  41 55                            push r13
0003  48 81 ec 38 1d 00 00             sub rsp, 0x1d38
000a  4c 89 b4 24 00 1d 00 00          mov [rsp+0x1d00], r14
0012  48 89 8c 24 10 00 00 00          mov [rsp+0x10], rcx
001a  49 bb ab 77 7b 0c 00 00 00 00    movabs r11, 0xc7b77ab              ; <kernel>
0024  48 83 ec 20                      sub rsp, 0x20
0028  41 ff d3                         call r11
002b  48 83 c4 20                      add rsp, 0x20
002f  48 89 84 24 00 00 00 00          mov [rsp], rax
0037  48 8d 8c 24 18 00 00 00          lea rcx, [rsp+0x18]
003f  48 8d 94 24 10 00 00 00          lea rdx, [rsp+0x10]
0047  49 bb cb 77 7b 0c 00 00 00 00    movabs r11, 0xc7b77cb              ; <kernel>
0051  48 83 ec 20                      sub rsp, 0x20
0055  41 ff d3                         call r11
0058  48 83 c4 20                      add rsp, 0x20
005c  e9 89 00 00 00                   jmp 0xea
0061  53                               push rbx
0062  41 55                            push r13
0064  48 81 ec 38 1d 00 00             sub rsp, 0x1d38
006b  4c 89 b4 24 00 1d 00 00          mov [rsp+0x1d00], r14
0073  48 89 8c 24 28 00 00 00          mov [rsp+0x28], rcx
007b  48 89 94 24 10 00 00 00          mov [rsp+0x10], rdx
0083  49 bb ab 77 7b 0c 00 00 00 00    movabs r11, 0xc7b77ab              ; <kernel>
008d  48 83 ec 20                      sub rsp, 0x20
0091  41 ff d3                         call r11
0094  48 83 c4 20                      add rsp, 0x20
0098  48 89 84 24 00 00 00 00          mov [rsp], rax
00a0  48 8d 8c 24 18 00 00 00          lea rcx, [rsp+0x18]
00a8  48 8d 94 24 10 00 00 00          lea rdx, [rsp+0x10]
00b0  49 bb cb 77 7b 0c 00 00 00 00    movabs r11, 0xc7b77cb              ; <kernel>
00ba  48 83 ec 20                      sub rsp, 0x20
00be  41 ff d3                         call r11
00c1  48 83 c4 20                      add rsp, 0x20
00c5  48 8d 8c 24 30 00 00 00          lea rcx, [rsp+0x30]
00cd  48 8d 94 24 28 00 00 00          lea rdx, [rsp+0x28]
00d5  49 bb cb 77 7b 0c 00 00 00 00    movabs r11, 0xc7b77cb              ; <kernel>
00df  48 83 ec 20                      sub rsp, 0x20
00e3  41 ff d3                         call r11
00e6  48 83 c4 20                      add rsp, 0x20
00ea  e9 02 00 00 00                   jmp 0xf1
00ef  58 00                            .asciz "X"
00f1  48 b9 ef a0 cd 0c 00 00 00 00    movabs rcx, 0xccda0ef              ; <immobilized>
00fb  49 bb 90 d3 7a 0c 00 00 00 00    movabs r11, 0xc7ad390              ; <kernel>
0105  48 83 ec 20                      sub rsp, 0x20
0109  41 ff d3                         call r11
010c  48 83 c4 20                      add rsp, 0x20
0110  48 89 c1                         mov rcx, rax
0113  48 8b 94 24 10 00 00 00          mov rdx, [rsp+0x10]
011b  49 bb a6 cf 7a 0c 00 00 00 00    movabs r11, 0xc7acfa6              ; <kernel>
0125  48 83 ec 20                      sub rsp, 0x20
0129  41 ff d3                         call r11
012c  48 83 c4 20                      add rsp, 0x20
0130  49 89 c5                         mov r13, rax
0133  48 8b 8c 24 00 00 00 00          mov rcx, [rsp]
013b  49 bb 0a 78 7b 0c 00 00 00 00    movabs r11, 0xc7b780a              ; <kernel>
0145  48 83 ec 20                      sub rsp, 0x20
0149  41 ff d3                         call r11
014c  48 83 c4 20                      add rsp, 0x20
0150  4c 89 e8                         mov rax, r13
0153  4c 8b b4 24 00 1d 00 00          mov r14, [rsp+0x1d00]
015b  48 81 c4 38 1d 00 00             add rsp, 0x1d38
0162  41 5d                            pop r13
0164  5b                               pop rbx
0165  c3                               ret
```

**ケース 2 と 3 の機械語は、1 箇所を除いてバイト単位で完全に同一**(どちらも 358 byte)。
機械的に diff を取った結果、差は次の 1 行だけだった:

```
f2:  00f1  48 b9 ef 90 cd 0c 00 00 00 00    movabs rcx, 0xccd90ef
f3:  00f1  48 b9 ef a0 cd 0c 00 00 00 00    movabs rcx, 0xccda0ef
```

これは**埋め込んだ文字列 "X" 自身のアドレス**で、2 つの関数が別の Immobilized Page
(0xCCD9000 と 0xCCDA000)に配置されたことによる差にすぎない。

**`setq x 2` はコンパイル結果に 1 ビットも影響していない。** `x` は実行時に引かれるため。

### B. 推測・未確認

- 逆アセンブル中の `0xc7ad390` を `os_make_symbol`、`0xc7acfa6` を
  `os_get_variable` と同定したのは、**命令列が `za_emit_operand` の
  `is_literal == 9` の emit 順序と完全に一致することから**であり、
  シンボルテーブルによる逆引きで確認したわけではない(**推測**。
  ただし命令列の一致は厳密で、他の解釈は考えにくい)。

---

## 調査項目 C: defun の登録先決定ロジック

### C-1. 決めている箇所

**確認。`src/c/eval.c:386`、`eval_defun` の中の 1 行。**

```c
static lisp_val_t eval_defun(lisp_val_t args, lisp_val_t env) {   /* eval.c:368 */
    ...
    lisp_val_t fn = za_try_compile_defun(params, body, env);      /* eval.c:382 */
    if (fn == nil) {
        fn = make_interpreted_function(params, body, env);        /* eval.c:384 */
    }
    os_set_function(name, fn, env);                               /* eval.c:386 ★ */
```

`os_set_function`(`runtime.c:3462`)は**渡された env 自身の関数スロットにだけ書き、
親を辿らない**:

```c
lisp_val_t next_cell = cc_cdr(cc_cdr(env));   /* runtime.c:3471 */
lisp_val_t func_slot = cc_car(next_cell);     /* (functions . alist) */
```

### C-2. 「現在の環境」の出どころ

**確認。グローバル変数でも VM レジスタでもなく、評価器の引数として渡ってくる。**

呼び出しチェーン:

```
os_eval(form, env)                    eval.c:960 付近
  └─ op == g_sym_defun                eval.c:978
       └─ eval_defun(args, env)       eval.c:979   ← env をそのまま渡す
            └─ os_set_function(name, fn, env)   eval.c:386
                 └─ env の3番目のスロット(functions)の alist を破壊的に更新
                      runtime.c:3471-3500
```

この `env` は、`let` の本体を評価している場合は `apply_function` が作った
`CALL-ENV`(`eval.c:152`)。**`proc->env`(`%%current-environment` が返すもの)とは
別物**で、`let` は `proc->env` を一切書き換えない。実測:

```
#cur 外の環境名          = F1
#cur letの中で同じenvか   = T (名前 F1)
#cur 関数の中で同じenvか  = T (名前 F1)
#cur orの中で同じenvか    = T (名前 F1)
```

### C-3. 同じ登録経路を通る他の定義形式

**確認。実装されているものと経路は以下のとおり。**

| 形式 | 実装 | 登録先 | `let` 内で同じ問題が起きるか |
|---|---|---|---|
| `defun` | `eval_defun` `eval.c:368` | `os_set_function(name, fn, env)` `eval.c:386` | **起きる** |
| `defmacro` | `eval_defmacro` `eval.c:522` | `os_set_function(name, macro, env)` `eval.c:530` | **起きる(同一経路)** |
| `defgeneric` | `init.lisp:443` マクロ | `(defun name ...)` へ展開 | **起きる(defun 経由)** |
| `defglobal` | `eval_defglobal` `eval.c:336` | `os_set_variable(name, val, env)` `eval.c:345` | **起きる(変数版)** |
| `defconstant` | `eval_defconstant` `eval.c:293` | `os_set_variable(name, val, env)` `eval.c:302` | **起きる(変数版)** |
| `defvar` | `eval_defvar` `eval.c:268` | env の `variables` スロットを直接更新 `eval.c:274` | **起きる(変数版)** |
| `defdynamic` | `eval_defdynamic` `eval.c:314` | `g_dynamic_bindings`(単一のグローバル) | **起きない** |
| `defclass` | `init.lisp:359` マクロ | `%register-class` → `*classes*` 動的変数 `init_aot.lisp:333-336` | **起きない** |
| `defmethod` | `init.lisp:481` マクロ | `%register-method`(グローバルなテーブル) | **起きない** |

**同じ経路(`os_set_function` + 呼び出し時の env)を通るのは `defun` / `defmacro` /
`defgeneric` の 3 つ。** `defglobal`/`defconstant`/`defvar` は関数スロットではなく
変数スロットだが、「呼び出し時の env に書く」という構造はまったく同じ。

`defdynamic` / `defclass` / `defmethod` が無事なのは、環境ではなく**グローバルな
単一のストア**へ書いているため。

### C-4. 環境を明示引数に取る低レベル API

**確認。C には存在する。Lisp には公開されていない。**

- C: `os_set_function(sym, val, env)`(`runtime.h:773`)、
  `os_set_variable(sym, val, env)`(`runtime.h` 同節)
- Lisp: `%%SET-FUNCTION` / `%%SET-VARIABLE` 相当の primitive は**登録されていない**
  (`src/c/*.c` を grep して該当なし)。環境を扱う primitive は
  `%%MAKE-ENVIRONMENT` / `%%CURRENT-ENVIRONMENT` / `%%GLOBAL-ENVIRONMENT` /
  `%%SET-CURRENT-ENVIRONMENT` / `%%EVAL-IN-ENVIRONMENT` の 5 つのみ
  (`runtime.c:2359-2363`)

### C. 修正を入れるとしたら

**確認できる事実としては「登録先を決めている箇所は `eval.c:386` の 1 行」。**
ただし `defmacro`(`eval.c:530`)と変数系 3 つ(`eval.c:274` / `302` / `345`)にも
同じ構造があるので、**同じ考え方を適用するなら 6 箇所**になる。

---

## 調査項目 D: 関数呼び出しの解決方法

### 判定: **3. 初回のみ解決してキャッシュ。ただしキャッシュするのは「関数」ではなく「Function Cell のアドレス」**

そのため**再定義には追従する**。

### 根拠 1: 実機の挙動

```
#D 再定義前 (caller)=1 caller-compiled=T
#D 再定義後 (caller)=2       ← 2 が返る = 焼き込みではない
```

依頼文の「`1` が返る場合は焼き込み方式が確定」の**逆**の結果。

### 根拠 2: 逆アセンブル(`caller`、オフセット 0x0134〜0x01f7)

```
0134  49 be 10 89 c7 0d 00 00 00 00    movabs r14, 0xdc78910            ← キャッシュスロットのアドレス
013e  49 8b 46 00                      mov rax, [r14]                   ← キャッシュを読む
0142  49 89 c2                         mov r10, rax
0145  4d 85 d2                         test r10, r10
0148  0f 85 52 00 00 00                jne 0x1a0                        ← 入っていれば解決を飛ばす
014e  e9 07 00 00 00                   jmp 0x15a
0153  43 41 4c 4c 45 45 00             .asciz "CALLEE"                  ← 呼び出し先の名前
015a  48 b9 53 c1 cd 0c 00 00 00 00    movabs rcx, 0xccdc153  ; <immobilized>
0164  49 bb 90 d3 7a 0c 00 00 00 00    movabs r11, 0xc7ad390  ; <kernel>   ← os_make_symbol
0172  41 ff d3                         call r11
0179  49 89 c5                         mov r13, rax
017c  4c 89 e9                         mov rcx, r13
017f  48 8b 94 24 10 00 00 00          mov rdx, [rsp+0x10]              ← ENV_VAL
0187  49 bb bd 1a 7b 0c 00 00 00 00    movabs r11, 0xc7b1abd  ; <kernel>   ← os_get_function_cell
0195  41 ff d3                         call r11
019c  49 89 46 00                      mov [r14], rax                   ← ★ cell をキャッシュへ書き戻す
01a0  48 89 84 24 40 00 00 00          mov [rsp+0x40], rax
   ...
01cd  4c 8b 94 24 40 00 00 00          mov r10, [rsp+0x40]              ← cell
01eb  49 83 e2 f8                      and r10, -0x8                    ← タグを外す
01ef  4d 8b 52 00                      mov r10, [r10]                   ← ★ cell を毎回 deref = 最新の fn
```

**キャッシュされるのは Function Cell のアドレス。中身(現在の関数オブジェクト)は
呼び出しのたびに `mov r10, [r10]` で読み直される。** これが再定義に追従する理由。

実装は `za_emit_fn_resolve_cached`(`src/c/za.c:3564-3586`)。設計意図は
`runtime.h:786` に明記:

> cell のアドレス自体は (sym, env) の束縛が存在する間不変だが、中身は
> `os_set_function` が再 defun のたびに書き換える。呼び出し側は cell のアドレスだけ
> 握っておけば、中身を読むたびに常に最新の定義を得られる

### AOT 側

**確認。AOT も名前解決を実行時に行う。** `src/lisp/transpile.lisp:214-217` が
`%%funcall-by-name` を `primitive_funcall_by_name` へ写像しており、これは
`os_get_function` + `os_apply_function` の組。実機で確認:

```
#D AOT reverse 元=(3 2 1)
```

AOT 化された `reverse`(`init_aot.lisp:61`)が正常に動く。
ただし **AOT 関数を Lisp から再定義したときに AOT 側の呼び出し元が追従するかは
確かめていない(未確認)。** `%%funcall-by-name` 経由なら追従するはずだが、
トランスパイラが直接 C 関数呼び出しへ落としている経路もあるため、
一律には言えない(**推測**)。

### ★ D で発見した別のバグ: 未定義のまま呼ぶと恒久的に壊れる

**確認。再現手順あり。**

```lisp
(defun early-caller () (not-yet-defined))   ; 呼び出し先はまだ未定義
(early-caller)                              ; → EVAL-ERROR(ここまでは想定内)
(defun not-yet-defined () 99)               ; あとから定義する
(early-caller)                              ; → EVAL-ERROR のまま ★
```

実測:

```
#D2 early-caller compiled=T
#D2 定義前の呼び出し = ERROR
#D2 定義後の呼び出し = ERROR        ← 定義しても直らない
#D3 対照(定義済みで呼ぶ) = 77       ← 順序を逆にすれば正常
```

**機序(コードを読んで確認):** `os_get_function_cell`(`runtime.c:3532-3556`)は
未定義のとき `nil` を返す。`za_emit_fn_resolve_cached`(`za.c:3585`)は戻り値を
無条件にキャッシュへ書き戻す(`jit_mov_mem_disp8_from_reg(ZA_REG_R14, 0, ZA_REG_RAX)`)。
キャッシュの有無判定は `test r10, r10; jne`(`za.c:3571-3572`)だが、
**`nil` は 0 ではない**(逆アセンブルに `movabs rax, 0xdc52031` として現れる
タグ付きヒープ値)ため、`nil` がキャッシュされると以後ずっと「解決済み」と
みなされる。

これは `documents/jit.md` の「呼び出し先は毎回名前で再解決するため、前方参照・
再定義がそのまま安全に動きます」という記述と食い違う。**前方参照は「定義前に
一度も呼ばなければ」安全、が正確なところ。**

本バグ(let 内 defun)を修正するとループ内での再定義が普通に起きるようになるため、
この経路を踏む確率は上がる。

---

## 調査項目 E: immobilized space への配置と重複排除

### E-1/E-2. 再定義したときの使用量と重複排除

**確認。重複排除は存在しない。再定義のたびに新しいページを消費する。**

実測(同名 `(defun g-redef () 1)` を 100 回):

```
#E 同名 defun x100: 565408 -> 979840 (増分 414432 byte, 1回あたり 4144)
#E imm 総量=16777216 使用=979840
```

**1 回の `defun` あたり約 4,144〜4,160 byte** = 4KB ページ 1 枚 + `za_fn_meta_t` 等。
16MB を 4,144 で割ると **約 4,000 回で枯渇**する。

重複排除の仕組みは `src/c/za.c` / `src/c/runtime.c` に見当たらなかった。
`za_try_compile_defun`(`za.c:6030` 付近)は毎回
`os_imm_pages_alloc_contiguous(page_count)` で新しいページを取る。

### E-2b. 古い領域の回収

**確認。`destroy-environment` では回収されるが、JIT からは再利用されない。**

実測:

```
#E2 destroy=T  before=561248 mid=565408 after=565408
#E2 defunで+4160、destroyで0
```

`%%imm-space-used-bytes` が減らないのは、この指標が bump ポインタ基準だから
(`runtime.c:1354-1367`)。**回収自体は行われている** —
`os_environment_reclaim_pages`(`runtime.c:3597-3610`)が各ページを
`os_imm_page_free` へ渡し、`os_imm_page_free`(`runtime.c:1369-1372`)が
フリーリストへ繋ぐ。

**しかし重要な非対称がある(確認):**

| 確保関数 | フリーリストを見るか | 用途 |
|---|---|---|
| `os_imm_page_alloc` `runtime.c:1143-1155` | **見る**(1144 行目で先頭を取り出す) | `za_fn_meta_t`、Function Cell(`os_imm_slot_alloc` 経由) |
| `os_imm_pages_alloc_contiguous` `runtime.c:1374-1382` | **見ない**(bump 一直線) | **JIT コードページ** |

```c
void *os_imm_pages_alloc_contiguous(UINT64 count) {
    UINT64 needed = count * IMM_PAGE_SIZE;
    if (g_imm_bump + needed > g_imm_space + IMM_SPACE_SIZE) {
        return 0;
    }
    void *pages = g_imm_bump;
    g_imm_bump += needed;
    return pages;
}
```

**つまり `destroy-environment` で解放した JIT コードページは、フリーリストには
戻るが JIT コードとしては二度と再利用されない。** 単ページ確保(メタデータ側)だけが
再利用する。

### E-3. `za_fn_meta_t` の一意化の粒度

**確認。一意化されていない。`defun` 1 回につき 1 個確保される。**

`os_fn_meta_alloc`(`runtime.c:1265-1271`)は毎回
`os_imm_slot_alloc(&g_fn_meta_cursor, sizeof(za_fn_meta_t))` で新しいスロットを切る。

> 依頼文の「Phase 1 で fnptr 単位に一意化したという経緯」は、**別のもの**を
> 指していると思われる。Phase 1(disassembler)では `za_fn_meta_t` に
> `code_base`/`code_len` の 2 フィールドを**追加**しただけで、一意化はしていない
> (`runtime.h:854`)。一意化が行われたのは **AOT の lifted closure**のほうで、
> `os_make_lifted_closure_with_meta`(`runtime.c:3009` 付近)が
> 「同じ lambda から作られる全クロージャが 1 つの meta を共有してよい」として
> 生成コード側の `static za_fn_meta_t meta;` を使い回す形に変わっている
> (コメントに「旧実装はここで `os_fn_meta_alloc` を呼んでおり、let を 1 回評価する
> ごとに Immobilized Space を 32byte ずつ消費していた…約 24,300 反復で使い切る」)。
> **これは推測混じりの同定なので、経緯の確認が必要なら別途。**

### E-4. 枯渇したときの挙動

**確認。確保経路によって違う。**

| 経路 | 枯渇時の挙動 | 場所 |
|---|---|---|
| `os_imm_pages_alloc_contiguous`(JIT コード) | **0 を返す** → `za_try_compile_defun` が `nil` を返しインタプリタへフォールバック | `runtime.c:1376-1378`、`za.c:6035` 付近 |
| `os_imm_page_alloc`(メタデータ/Function Cell) | **`os_panic("immobilized space exhausted")` で停止** | `runtime.c:1149-1151` |

panic 時は `panic_write_imm_breakdown`(`runtime.c:1385-1398`)が使用量・各カーソル・
最後の要求サイズを画面へ出す。

つまり **JIT コードの枯渇は「静かに遅くなる」、メタデータの枯渇は「止まる」。**

---

## 調査項目 F: environment の GC 上の扱い

### F-1. コピー GC の対象か

**確認。対象。GC ヒープ(From/To 空間)に置かれる。**

環境は `os_make_cons` だけで組み立てられており(`runtime.c:2975-3000`)、
`os_make_cons` は `os_alloc_bytes`(From 空間)を使う(`runtime.c:2466`)。
非コピー領域(Immobilized Space)に置かれるのは `cells` スロットが**指す先**の
Function Cell と `pages` スロットが指すページだけ。

### F-2. ルートセット

**確認。**

| ルート | 登録箇所 |
|---|---|
| `global_environment` | `os_gc_collect_body` が直接 `gc_copy_value` `runtime.c:2091` |
| 各プロセスの `proc->env` | `os_gc_register_root(&proc->env)` `process.c:402` |
| `*environments*`(登録済み環境の一覧) | 動的変数なので `g_dynamic_bindings` 経由 `runtime.c:2092` |
| `CALL-ENV` 等の一時環境 | C スタック上のローカルを `GC_PROTECT`(`eval.c:155` 等) |

### F-3. 関数スロットの alist に入るアドレスの GC からの見え方

**確認。2 種類あり、扱いが分かれている。**

- `functions` スロットの値 = **関数オブジェクト(`TAG_INSTANCE`)**。GC ヒープ上に
  あり、コピー対象。`gc_scan_instance` が word1 を素通しする特別扱いがある
  (`runtime.c:1719-1722`、`MAGIC_FUNCTION_NATIVE` の word1 は `za_fn_meta_t` への
  生ポインタなので追ってはいけない)
- `cells` スロットの値 = **`TAG_RAW_POINTER` 付きの Immobilized Space アドレス**。
  `os_tag_is_heap_ref`(`runtime.h:37-42`)が `TAG_RAW_POINTER` に対して 0 を返すので
  **GC は追わないし動かさない**

```c
static inline int os_tag_is_heap_ref(UINT64 tag) {
    /* FIXNUM/CHARは即値、RAW_POINTERはGC管理外の生ポインタ。... */
    return tag != TAG_FIXNUM && tag != TAG_CHAR && tag != TAG_RAW_POINTER;
}
```

Immobilized Space は動かないので、この扱いで整合している。

### F-4. 関数スロットへの格納前後の `GC_PROTECT`

**確認。張られている。**

`os_set_function`(`runtime.c:3462-3479`)の冒頭:

```c
GC_PROTECT(fn_obj);
GC_PROTECT(sym);
GC_PROTECT(env);
lisp_val_t next_cell = cc_cdr(cc_cdr(env));
lisp_val_t func_slot = cc_car(next_cell);
GC_PROTECT(func_slot);
lisp_val_t alist = cc_cdr(func_slot);
GC_PROTECT(alist);
```

コメントに「新規追加パスでは `os_make_cons` 後に `fn_obj`/`sym`/`env` を return や
後続の cells スロット同期処理で読み直すため保護する」とあり、意識的に張られている。

`eval_defun` 側(`eval.c:368-386`)も `name` / `params` / `body` / `env` を
`za_try_compile_defun` の前に保護している。

### F-5. `switch-environment` 中の環境の到達可能性

**確認。`proc->env` 経由。**

`switch-environment`(`init.lisp:1036-1042`)は `%%set-current-environment` を呼び、
これが `proc->env` を書き換える。`proc->env` は `os_gc_register_root` 済み
(`process.c:402`)なので、切り替え中の環境はそこからルートとして到達可能。
加えて `make-environment` が `*environments*` へ登録している
(`init.lisp:1012`)ので、動的変数経由でも到達できる。

### F. 推測・未確認

- 「格納の途中で GC が走ると from-space の古いポインタが残る」という事故が
  **実際に起きないことは確認していない**(**未確認**)。`documents/pitfalls.md` の
  原則 4/原則 8 と、`environment_literal_slots_test.lisp` /
  `environment_pages_test.lisp` という既存の塗り潰し監査テストが存在することから、
  この領域は一度監査されていると思われる(**推測**)。
- 環境オブジェクトのアドレスを実機で領域分類しようとしたが、存在しない primitive
  (`%%disasm-addr-of`)を呼んでしまい**測定は失敗した**(戻り値 0 は無意味)。
  上記 F-1 の結論はコードリーディングのみに基づく。

---

## 調査項目 G: switch-environment の巻き戻し

### 判定: **巻き戻らない。`unwind-protect` を明示的に書いた場合だけ戻る。**

**確認。実測:**

```
#G 開始時の環境      = F1
#G letの中で切替     = G-TMP
#G letを抜けた直後   = G-TMP      ← ★ 戻らない
#G 明示的に戻した後  = F1
#G 非局所脱出の中    = G-TMP
#G 脱出したあと      = G-TMP      ← ★ エラー脱出でも戻らない
#G 再び戻した後      = F1
#G unwind-protect で戻す = G-TMP
#G unwind-protect の後   = F1     ← ★ unwind-protect なら戻る
```

### G-1. ブロックを抜けた場合

**戻らない。** `switch-environment` は `%%set-current-environment` で `proc->env` を
書き換えるだけで、動的 extent の概念を持たない。
`let` は `proc->env` に触れないので、`let` を抜けても書き換えられたままになる。

### G-2. 非局所脱出

**戻らない。** `block` + `with-handler` + `return-from` で脱出しても `G-TMP` のまま。

### G-3. `unwind-protect`

**実装されている。** `eval.c:996`(`g_sym_unwind_protect` のディスパッチ)、
`eval_unwind_protect`。実測でも cleanup 節が走って `F1` に戻った。

なお `with-environment`(`init.lisp:1066` 付近)というスコープ付き切り替えマクロが
別に存在する。これは `%%eval-in-environment` で body を**別環境で評価する**方式で、
`proc->env` の書き換えとは別経路(`init.lisp:1044-1056` のコメントに経緯がある)。
**`with-environment` の巻き戻し挙動は今回確認していない(未調査)。**

---

## 所見(ここから先は推測を含む)

> **以下は修正方針への示唆であり、コードで裏を取った事実ではない。**

### 1. 「frame と environment を分ける」方針は成立しそうだが、`let` だけでは足りない

`let` が作る環境は `apply_function`(`eval.c:152`)が作っており、これは
**通常の関数呼び出しとまったく同じ 1 行**である。`let` は展開後は文字通り関数呼び出し
なので、「`let` が作ったものだけ frame にする」ことは現在の構造では**できない**
(呼び出し元が `let` だったという情報が `apply_function` に届いていない)。

分けるには `let` を IIFE でなくす必要がある。具体的には
**`let` を `eval.c` の特殊形式にする**のが素直で、そうすると:

- `or` / `case` / `case-using` / `for` / `while` / `with-open-*` / `dynamic-let` など
  **init.lisp で `let` へ展開している 13 箇所**が自動的に追従する
  (`grep -c '`(let \|`(let\*' src/lisp/init.lisp` = 13)
- マクロ展開ぶんのコスト(実測 480 byte/call)も消える
- ただし `za.c`(`za_compile_let` は macroexpand 後の IIFE を見ている)と
  `transpile.lisp`(`expand-let`)の両方に変更が必要

### 2. 「最も近い祖先 environment へ委譲」は `flet`/`labels` と衝突しうる

`flet`/`labels` は「新環境の関数スロットへ束縛して、抜けたら消える」ことが**仕様**
(`eval.c:440` / `479`)。frame への `set-function` を無条件に祖先へ委譲すると、
`flet` の束縛まで漏れる可能性がある。frame と `FLET-ENV`/`LABELS-ENV` を
別扱いにする必要があると思われる。

### 3. 修正すると E の消費が効いてくる

ループ内の `let` + `defun` が書けるようになると、**反復ごとに 4KB のページを消費し、
JIT 側は解放済みページを再利用しない**(E-2b)。16MB ÷ 4,144 ≒ 4,000 回で
JIT コードの確保が 0 を返し、以後**黙ってインタプリタへフォールバックする**
(止まらないぶん気づきにくい)。修正とセットで、少なくとも以下のどれかが要りそう:

- 同じ (name, env, body) の再コンパイルを避ける重複排除
- `os_imm_pages_alloc_contiguous` をフリーリスト対応にする
- 再定義時に古いページを解放する

### 4. D で見つけた nil キャッシュは、修正より前に潰しておくほうが安全

`(defun a () (b))` を `b` の定義前に一度でも呼ぶと恒久的に壊れる。
`za.c:3585` のキャッシュ書き戻しを「`nil` ならキャッシュしない」に変えるだけで
済むように見える(**推測**。副作用の確認はしていない)。
本バグを修正すると再定義・再コンパイルが日常的になるため、踏む確率が上がる。

### 5. `declaim` の設計との関係(前回の調査より)

`let` は `proc->env`(`%%current-environment` が返すもの)を書き換えない。
環境スコープの値を持たせたいなら `proc->env` 側のスロットに置けば `let` は無関係で、
本バグの修正を待つ必要はない。

---

## 未調査・積み残し

| 項目 | 内容 |
|---|---|
| D(AOT) | AOT 関数を Lisp から再定義したとき、AOT 側の呼び出し元が追従するか。`%%funcall-by-name` 経由なら追従するはずだが、直接 C 呼び出しに落ちている経路の有無を確認していない |
| E-3 | 「Phase 1 で fnptr 単位に一意化した」経緯の同定。AOT lifted closure の meta 共有と混同している可能性がある |
| F | 環境オブジェクトが GC ヒープにあることの**実機での**確認(コードリーディングのみ)。測定に使おうとした primitive が存在せず失敗した |
| F | 「格納途中の GC で from-space ポインタが残る」事故の実在確認 |
| G | `with-environment`(`%%eval-in-environment` 方式)の巻き戻し挙動 |
| A | 同名環境が複数ある場合の `switch-environment` の解決順 |

---

## 付録: 測定に使った一時スクリプト

**いずれもコミットしておらず、実行後に削除済み。** 再現するには
`test/lisp/` へ置いて `make test-qemu-milestone MILESTONE=...` で実行する。

### B / D / E の測定

```lisp
(let ((x 1))
  (defun f1 () 42)
  (format *isiki-test-stream* "#B1 compiled=~S (f1)=~S code-len=~S~%"
          (%%za-compiled-p (function f1)) (f1) (%%disasm-code-len (function f1)))
  (disassemble-to-stream *isiki-test-stream* (function f1)))
(let ((x 1))
  (defun f2 () x)
  (disassemble-to-stream *isiki-test-stream* (function f2)))
(let ((x 1))
  (defun f3 () x)
  (format *isiki-test-stream* "#B3 setq前 (f3)=~S~%" (f3))
  (setq x 2)
  (format *isiki-test-stream* "#B3 setq後 (f3)=~S~%" (f3)))

(defun callee () 1)
(defun caller () (callee))
(format *isiki-test-stream* "#D 再定義前 (caller)=~S~%" (caller))
(defun callee () 2)
(format *isiki-test-stream* "#D 再定義後 (caller)=~S~%" (caller))

(defun early-caller () (not-yet-defined))
(format *isiki-test-stream* "#D2 定義前 = ~S~%"
        (block b (with-handler (lambda (c) (return-from b 'ERROR)) (early-caller))))
(defun not-yet-defined () 99)
(format *isiki-test-stream* "#D2 定義後 = ~S~%"
        (block b (with-handler (lambda (c) (return-from b 'ERROR)) (early-caller))))

(defglobal *e-a* (%%imm-space-used-bytes))
;; (defun g-redef () 1) を 100 行
(defglobal *e-b* (%%imm-space-used-bytes))
```

### G の測定

```lisp
(defun env-name () (cdr (car (%%current-environment))))
(defglobal *g-tmp* (make-environment 'g-tmp))
(format *isiki-test-stream* "#G letの中で切替   = ~S~%" (let () (switch-environment 'g-tmp) (env-name)))
(format *isiki-test-stream* "#G letを抜けた直後 = ~S~%" (env-name))
(switch-environment 'f1)
(format *isiki-test-stream* "#G 非局所脱出の中  = ~S~%"
        (block b (with-handler (lambda (c) (return-from b (env-name)))
                   (progn (switch-environment 'g-tmp) (error "boom")))))
(format *isiki-test-stream* "#G 脱出したあと    = ~S~%" (env-name))
```

**新規に計測用の C コードを足した箇所は無い。** 既存の primitive
(`%%za-compiled-p` / `%%disasm-code-len` / `%%disasm-code-base` /
`%%imm-space-used-bytes` / `%%heap-used-bytes` / `%%gc-collect-count`)だけで足りた。
