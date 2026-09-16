# 調査: 制御転送チェックの削減(改善B)

> 調査日: 2026-09-16 / ブランチ: `feature/control-transfer-survey`(分岐元 `feature/compiler-optimization` `c2e669d`)
> 発端: `documents/type-system-survey.md` §7-2 / §9-1、`documents/inline-builtin.md`
> 関連: `documents/jit.md`(非局所脱出の設計)、`documents/fixnum-signed-fastpath.md`(改善A)
>
> **これは調査であり実装ではない。** コードは一切変更していない。

---

## 0. 結論(4-2)

### **一部省ける。ただし、省く前にやるべきことが別にある。**

制御転送チェックは **1 箇所あたり 49 byte / 16 命令**で、そのうち**実際の判定は 2 命令**しかない。
残り 14 命令は**呼び出し規約と `call`/`ret` のオーバーヘッド**である。

したがって独立した 2 つの改善がある。

| | 内容 | 適用範囲 | 意味論の変更 | 1箇所あたり |
|---|---|---|---|---|
| **(α) チェックをインライン展開する** | `call os_is_control_transfer` をやめ、`and`/`cmp` を直接出す | **全 39 箇所** | **無し** | 16 命令 → 約 5 命令 |
| **(β) leaf に対するチェックを省く** | 「値が制御転送になりえない」式の後ろのチェックを出さない | 26〜29 / 39 箇所 | **有り(要根拠)** | 16 命令 → 0 |

**(α) を先にやるべきである。** 意味論を一切変えず、全箇所に効き、
(β) の 2/3 以上の効果が**根拠の議論なしに**得られる。
(β) は (α) の後で残差を見てから判断すればよい。

### 省ける根拠(β について)

**パラメータ参照と非 boxed な let-local 参照は、制御転送値を保持しえない。**
根拠は「そこへ書き込む経路が全数把握でき、すべて書き込み前にチェックしている」こと(§3-3)。

- **パラメータスロットへの書き込みは 2 箇所だけ**(`za.c:6285/6288/6291` の fixed entry、
  `za.c:6328` の cons entry)。どちらも呼び出し元から渡された値で、
  呼び出し元は**インタプリタ側(`eval.c:41-46` の `eval_args`)も JIT 側
  (`za.c:4045` の引数ループ)も、引数 1 個ごとにチェックしてから渡す**
- **固定引数/`&rest` への `setq` は JIT が非対応**(`za.c:4896-4898` で `return 0`)。
  つまりパラメータスロットは**関数入口で 1 回書かれたきり**である
- **let-local への書き込みは 2 箇所**(`za.c:3319` の init、`za.c:4941` の setq)。
  どちらも直前に制御転送チェックがある

### 実装しない理由

**この不変条件は「45 箇所がそれぞれ独立にチェックしている」ことで成り立っており、
1 箇所で保証されていない**(za.c に 25 箇所、eval.c に約 20 箇所)。
pitfalls の言う omission list そのもので、**将来どこかが漏れたときに
(β) を入れてあると静かに壊れる**。

§7 の検出手段(不変条件そのものを観測する仕掛け)を用意してから入れるべきである。
既存の非局所脱出テスト(`za_test_ext5.lisp` の 69 件ほか)は**この壊れ方を検出しない**。
理由は §7 に書く。

---

## 1. `os_is_control_transfer` は何を検出しているか(3-1)

### 1-1 実装(`src/c/eval.h:96`)

```c
static inline int os_is_control_transfer(lisp_val_t v) {
    if ((v & TAG_MASK) != TAG_INSTANCE) {
        return 0;
    }
    UINT64 *obj = (UINT64 *)(v & ~TAG_MASK);
    return obj[0] == MAGIC_BLOCK_EXIT || obj[0] == MAGIC_CATCH_EXIT || obj[0] == MAGIC_GO_EXIT;
}
```

**検出対象は 3 つ**である。`MAGIC_BLOCK_EXIT` だけではない。

| magic | 値 | 生成元 |
|---|---:|---|
| `MAGIC_BLOCK_EXIT` | 0x5 | `return-from`。word2=block 名、word3=値 |
| `MAGIC_CATCH_EXIT` | 0x9 | `throw`。word1=tag、word2=値 |
| `MAGIC_GO_EXIT` | 0xA | `go`。word1=tag |

いずれも `TAG_INSTANCE` の**ヒープオブジェクト**である。

### 1-2 PR #65 の理解は正しい

**制御転送はスタックを巻き戻さない。戻り値に埋め込んだ特殊な値をバケツリレーする方式である。**
`documents/jit.md:16-21` が明記している:

> isiki-os には setjmp/longjmp が無いため、いずれも eval.c の eval_block/… と同じ
> 「制御転送値(MAGIC_BLOCK_EXIT/MAGIC_CATCH_EXIT/MAGIC_GO_EXIT の TAG_INSTANCE)を
> 戻り値として上位の呼び出し元へバケツリレーする」方式を機械語化する。

**ただし `go` だけは例外**で、飛び先ラベルが同一 JIT 関数内に静的に存在する場合は
コンパイル時に解決して直接 `jmp` する(実行時に制御転送値を作らない)。
`go` が `catch` / `unwind-protect` のスパンを飛び越える場合はコンパイルを断念する。

### 1-3 真だったとき呼び出し側は何をするか

`za_emit_ct_check_and_jmp_if_transfer`(`za.c:621`)が出すコードは:

```asm
mov  rcx, rax                    ; 引数
mov  r13, rax                    ; 元の値を退避(callee-saved)
mov  r11d, <os_is_control_transfer>
sub  rsp, 0x20 / call r11 / add rsp, 0x20
mov  r11d, 0x0
cmp  rax, r11
je   →通常経路                   ; 0 なら制御転送でない
mov  rax, r13                    ; 制御転送 → 値を復元して
jmp  →伝播先                     ;    残りの評価を全部スキップ
通常経路:
mov  rax, r13                    ; 値を復元してフォールスルー
```

**制御転送値はそのまま関数の戻り値として上へ伝播する。**
途中で確保した GC ルートがあれば、伝播先(cleanup ブロック)で unlink してから合流する。

**`os_is_control_transfer` はヒープ確保を一切しない**ので、
呼び出しを挟んで `r13` に退避するだけで GC 安全である(`za.c:618` のコメント)。

---

## 2. どこで挿入されているか(3-2)

### 2-1 挿入箇所は **25 箇所**、すべて `za_emit_ct_check_and_jmp_if_transfer()` 経由

| za.c 行 | 関数 | 何の評価結果に対して |
|---:|---|---|
| 2597 | `za_compile_fold` | 算術 fold の第 1 オペランド |
| 2613 | `za_compile_fold` | 第 2 オペランド以降(ループ) |
| 2719 | `za_compile_unary` | 単項述語のオペランド(`null`/`consp`/`fixnump` 等) |
| 2829 | `za_compile_unary_env` | `car`/`cdr` のオペランド |
| 2888 | `za_compile_binary` | 2 項の第 1 オペランド |
| 2901 | `za_compile_binary` | 2 項の第 2 オペランド |
| 3319 | `za_compile_let` | `let` の各 init 式 |
| 3403 | `za_compile_let` | `let` body の**非最終**フォーム |
| 3455 | `za_compile_progn` | `progn` の**非最終**フォーム |
| 3740 | `za_compile_expr_inner` | **`if` の test** |
| 4045 | `za_compile_call` | 一般呼び出しの**各引数** |
| 4505 / 4525 | `za_compile_quasiquote` | unquote / unquote-splicing の各要素 |
| 4601 | `za_compile_body_forms` | body の非最終フォーム(block/catch 等の共通ヘルパー) |
| 4667 | `za_compile_block` | block body の結果 |
| 4729 | `za_compile_return_from` | `return-from` の値式 |
| 4826 | `za_compile_defdynamic` | `defdynamic` の初期値式 |
| 4906 | `za_compile_setq` | **グローバル変数**への setq の値式 |
| 4941 | `za_compile_setq` | **local/boxed** への setq の値式 |
| 4981 | `za_compile_catch` | `catch` の tag 式 |
| 5060 / 5074 | `za_compile_throw` | `throw` の tag 式 / 結果式 |
| 5887 / 5901 | `za_compile_unwind_protect` | cleanup の結果 / protected の結果 |
| 6015 | `za_compile_tagbody` | tagbody の各フォーム |

### 2-2 **一律である。条件付きではない。**

**「評価結果が rax に載った直後」であれば、その式が何であれ無条件に出す。**
パラメータ参照だろうと fixnum リテラルだろうと同じく出る。

これは設計上の判断というより**網羅のため**である。
根拠は `za.c:2622-2626` のコメントで、**プロジェクト自身が既にこの事実を認識している**:

> skip_protect 時のオペランドは za_operand_is_safe_leaf 判定により呼び出しを含まないため、
> **制御転送は理論上起こり得ないが、安全のため** ct_patch0 と同じ最終合流点へ向けておく

つまり `za_compile_fold` は、**GC ルートの link/unlink は既に省いているのに、
制御転送チェックだけは「安全のため」残している。**

### 2-3 既に存在する類似の最適化: `za_operand_is_safe_leaf`

`za.c:2406`。「JIT GC 保護コスト削減(Phase1)」で導入済み。

```c
static int za_operand_is_safe_leaf(lisp_val_t form, lisp_val_t params, UINT64 fixed_count,
                                    const za_local_scope_t *locals) {
    if ((form & TAG_MASK) == TAG_FIXNUM || (form & TAG_MASK) == TAG_CHAR) return 1;
    if ((form & TAG_MASK) == TAG_SYMBOL) {
        /* 非 boxed な let-local 参照 */
        if (za_local_lookup(...)) return local_kind != ZA_VAR_BOXED;
        /* param-slot 方式が有効な関数の固定引数参照 */
        if (za_param_index(...)) return g_za_use_param_slots ? 1 : 0;
    }
    return 0;
}
```

**この述語が答えているのは「評価が CALL を一切含まないか(= GC が起きえないか)」**であって、
「値が制御転送になりえないか」**ではない**。§4 で見るとおり、後者はこれより**広い**。

---

## 3. パラメータ参照が制御転送を返しうるか(3-3)

### **返しえない。** 根拠は「書き込み経路の全数把握」である。

パラメータスロット(`za_param_val_off(i)`)へ書き込むコードは `za.c` に **2 箇所しかない**。

```
za.c:6285/6288/6291  fixed entry のプロローグ。rdx/r8/r9 をそのまま store
za.c:6328            cons entry のプロローグ。引数リストを cc_car で展開して store
```

`grep -n 'za_param_val_off' src/c/za.c` の全結果を確認した。
読み出し(`za.c:1673`)以外に書き込みは無い。

### 3-1 呼び出し元は必ずチェックしてから渡す

**インタプリタ経由**(`eval.c:37-47` `eval_args`):

```c
lisp_val_t head = os_eval(cc_car(args), env);
if (is_control_transfer(head)) { return head; }      /* ← リストに入れない */
lisp_val_t tail = eval_args(cc_cdr(args), env);
if (is_control_transfer(tail)) { return tail; }
return os_make_cons(head, tail);
```

`eval_call`(`eval.c:191`)も `eval_args` の戻り値をチェックしてから `apply_function` を呼ぶ。
**制御転送値を含む引数リストは構築されえない。**

**JIT 経由**(`za.c:4040-4047`):

```c
for (UINT64 i = 0; i < argc; i++) {
    za_compile_expr(arg_forms[i], ...);
    ct_patches[ct_patch_count++] = za_emit_ct_check_and_jmp_if_transfer();   /* ← ここ */
    za_store_slot(ZA_REG_RAX, za_arg_val_off(call_depth, i));
    za_emit_gc_link_slot(...);
}
```

**チェックが store より前にある。** 制御転送なら `abort_cleanup` へ飛び、呼び出し自体を行わない。

### 3-2 パラメータへの `setq` は JIT が受け付けない

`za.c:4892-4898`:

```c
if (!za_local_lookup(locals, sym, &val_off, &kind)) {
    if (za_param_index(params, sym, fixed_count, &dummy_idx) ||
        za_rest_param_symbol(params, fixed_count, &dummy_sym)) {
        return 0;  // 既存の制限: 固定引数/&restへのsetqは非対応のまま
    }
    ...
```

**パラメータスロットは関数入口で 1 回書かれたきり、関数の生存中に二度と書き換わらない。**

### 3-3 let-local も同じ論法が立つ

書き込みは `za_compile_let` の init(`za.c:3319` の直後)と
`za_compile_setq` の local 分岐(`za.c:4941` の直後)の 2 箇所。
**どちらも直前に制御転送チェックがある。**

boxed local(`ZA_VAR_BOXED`)はスロットに束縛 cons のポインタが入っており、
値の更新は `os_setcdr` で行う。`za_operand_is_safe_leaf` は boxed を除外しているので
(β)の対象外だが、**除外しておけば安全側**である。

### 3-4 ただし、これは「1 箇所で保証された不変条件」ではない

上の議論は **45 箇所(za.c 25 + eval.c 約 20)がそれぞれ正しくチェックしている**
ことに依存している。**どこか 1 箇所が漏れれば前提が崩れる。**
これは `documents/pitfalls.md` の言う omission list そのものである。

現在この不変条件を**検査する仕掛けは存在しない**。

---

## 4. 省ける条件の候補(3-4)

判定すべきは「**この式の値が制御転送オブジェクトになりうるか**」であって、
「評価が CALL を含むか」ではない。両者は一致しない。

| 式 | 制御転送を返しうるか | 根拠 |
|---|---|---|
| fixnum / char リテラル | **無い** | `movabs rax, imm` で TAG_FIXNUM/TAG_CHAR。制御転送は TAG_INSTANCE |
| パラメータ参照 | **無い** | §3。スロットの書き込み元が 2 箇所、両方チェック済み。setq 不可 |
| 非 boxed な let-local 参照 | **無い** | §3-3。書き込み元 2 箇所、両方チェック済み |
| `nil` / `t` | **無い** | `nil` は `g_nil_cell`(TAG_CONS)、`t` は `g_sym_t`(TAG_SYMBOL) |
| `(quote X)` | **無い** | ソース AST 上の既存の値。リーダが制御転送を作ることはない |
| `(function sym)` | **無い** | `os_get_function` は関数オブジェクトか nil を返す |
| boxed local 参照 | **無い(が除外を勧める)** | 更新は setq 経由でチェック済みだが、クロージャ共有があり追跡が増える |
| グローバル変数参照 | **たぶん無い(要精査)** | 書き込みは setq(`za.c:4906` / `eval.c`)と defglobal/defvar/defconstant(`eval.c:254/284/309`)。いずれもチェックしているが、**全数を追い切っていない** |
| 関数呼び出しの結果 | **ありうる** | 呼び先が `return-from` / `throw` する |
| `if` / `progn` / `let` / `block` / `catch` / `tagbody` / `unwind-protect` の結果 | **ありうる** | 内部に呼び出しや NLX を含みうる(部分式がすべて leaf なら再帰的に「無い」と言えるが、本調査では扱わない) |

### 段階分け

- **段階 A(既存述語のまま)**: fixnum/char リテラル、非 boxed local、param。
  `za_operand_is_safe_leaf` をそのまま「制御転送になりえない」判定に流用する
- **段階 B(値の型まで見る)**: A + `nil` / `t` / `quote` / `(function sym)`。
  **評価が CALL を含んでも(= GC が起きても)、値が制御転送になりえないことは別に言える**
- **段階 C**: グローバル変数参照。書き込み経路の全数把握が必要で、本調査では詰め切っていない

**段階 A と B は根拠が揃っている。C は未了。**

---

## 5. 削減量の実測(3-5)

### 5-1 チェック 1 箇所のコスト

`disassemble` の実測で、**1 箇所あたり 49 byte**。命令は次のとおり。

| | 命令数 |
|---|---:|
| JIT 側(`mov` ×2、`mov r11d,imm`、`sub rsp`、`call`、`add rsp`、`mov r11d,0`、`cmp`、`je`、`mov rax,r13`) | 10 |
| `os_is_control_transfer`(非 INSTANCE で早期 return する通常経路) | 6 |
| **合計** | **16** |

`os_is_control_transfer` の実体(`objdump -d tmp/za.o`):

```asm
os_is_control_transfer:
  mov  %rcx,%rdx
  and  $0x7,%edx
  mov  $0x0,%eax
  cmp  $0x5,%rdx
  jne  ret            ; ← fixnum 等はここで抜ける(6命令)
  and  $-8,%rcx
  mov  (%rcx),%rdx
  lea  -0x9(%rdx),%rax
  cmp  $0x1,%rax
  setbe %al
  cmp  $0x5,%rdx
  sete %dl
  or   %edx,%eax
  movzbl %al,%eax
  ret                 ; ← TAG_INSTANCE なら 16命令
```

**実際の判定は `and $7` + `cmp $5` の 2 命令。**
残り 14 命令は呼び出し規約(`sub rsp,0x20`/`call`/`add rsp,0x20`/`ret`)と
値の退避・復元である。

### 5-2 代表的な関数での実測

同一ブートで 14 個の関数を JIT し、`disassemble` して数えた。
CT チェックは `mov rcx, rax` の直後に `mov r13, rax` が来る並びで一意に識別できる
(`jit_mov_r13_rax()` は `za.c:623` の 1 箇所でしか呼ばれない)。

| 関数 | code len | CT 数 | CT byte | 占有率 | **段階A で省ける** | **段階B で省ける** |
|---|---:|---:|---:|---:|---:|---:|
| `(defun p (x) x)` | 412 | 0 | 0 | 0.0% | — | — |
| `(car x)` | 489 | 1 | 49 | **10.0%** | 1 | 1 |
| `(+ a b)` | 739 | 2 | 98 | **13.3%** | 2 | 2 |
| `(< a b)` | 694 | 2 | 98 | **14.1%** | 2 | 2 |
| `(+ a 1)` | 596 | 2 | 98 | **16.4%** | 2 | 2 |
| `(+ a (* b 2))` | 978 | 4 | 196 | **20.0%** | 3 | 3 |
| `(if (and a b c) 1 0)` | 935 | 3 | 147 | **15.7%** | 2 | 2 |
| `(length x)` | 1146 | 1 | 49 | 4.3% | 1 | 1 |
| `(let ((i 0)) (while (< i n) (setq i (+ i 1))) i)` | 1603 | 11 | 539 | **33.6%** | 5 | 5 |
| `(block b (return-from b x))` | 809 | 2 | 98 | 12.1% | 1 | 1 |
| `(let ((y x)) (+ y y))` | 794 | 3 | 147 | **18.5%** | 3 | 3 |
| `(catch 'tg (throw 'tg x))` | 942 | 3 | 147 | 15.6% | 1 | **3** |
| `(unwind-protect x nil)` | 719 | 2 | 98 | 13.6% | 1 | **2** |
| `(let ((y 0)) (setq y x) y)` | 718 | 3 | 147 | **20.5%** | 2 | 2 |
| **合計(識別関数を除く 13 本)** | **11,162** | **39** | **1,911** | **17.1%** | **26** | **29** |

- **制御転送チェックは全コードの 17.1% を占める**
- **段階 A で 26/39(66.7%)= 1,274 byte が省ける** → 全コードの **11.4%**
- **段階 B で 29/39(74.4%)= 1,421 byte** → 全コードの **12.7%**

### 5-3 内訳の導出(どのチェックが何に対応するか)

実測の総数と、ソース構造からの導出が**全 13 本で一致した**ので、分類は信頼できる。

- **`(+ a (* b 2))` = 4 箇所**: `a`(leaf)/`(* b 2)`(非leaf)/`b`(leaf)/`2`(leaf) → **3 省ける**
- **`(if (and a b c) 1 0)` = 3 箇所**: マクロ展開は `(if (if a (if b c nil) nil) 1 0)`。
  test は `a`(leaf)/`b`(leaf)/**内側 `if` の結果(非leaf)** → **2 省ける**。
  外側の test は `and` 全体なので leaf ではない
- **`while` ループ = 11 箇所**: let init `0`(leaf)/let body 非最終 `(while…)`/block 結果/
  tagbody フォーム/`if` test `(< i n)`/**`<` の 2 オペランド(leaf)**/progn 非最終/
  setq 値式 `(+ i 1)`/**`+` の 2 オペランド(leaf)** → **5 省ける**
- **`(catch 'tg (throw 'tg x))` = 3 箇所**: catch の tag `'tg` / throw の tag `'tg` / throw の値 `x`。
  **quote シンボルは評価に `os_make_symbol` の CALL を伴うので段階 A では leaf でないが、
  値が制御転送になりえないのは自明**。段階 B なら 3 箇所とも省ける

### 5-4 (α) インライン展開の削減量

**意味論を変えずに全 39 箇所に効く。**

```asm
; 現在(16命令 / 49 byte)
mov rcx,rax / mov r13,rax / mov r11d,imm / sub rsp,0x20 / call r11 / add rsp,0x20
mov r11d,0 / cmp rax,r11 / je / mov rax,r13          + 呼び先 6命令

; インライン化した場合(概算 5命令 / 約 20 byte)
mov  rdx, rax
and  edx, 7
cmp  rdx, 5
jne  →通常経路                  ; TAG_INSTANCE でなければ確定で「制御転送でない」
(TAG_INSTANCE のときだけ obj[0] を読んで 3 つの magic と比較)
```

**通常経路(fixnum / cons / symbol など)は 4 命令で抜けられる。**
39 箇所 × (16 − 5) = **約 429 命令分**が、意味論の議論なしに消える。

なお `mov r11d, 0; cmp rax, r11`(2 命令 9 byte)は `test eax, eax`(1 命令 2 byte)で
等価である。これも全 39 箇所に効く(§8-2)。

### 5-5 速度への効果は本調査では測っていない

**バイト数と命令数しか出していない。** TCG のコストは命令数に比例するので
(α) の 429 命令削減はそのまま効くはずだが、**実測していない以上「はず」である。**
`documents/bench-pinned-nil.md` の教訓どおり、実装してから同一ブート内で測ること。

---

## 6. 危険な経路(3-6)

(β)を入れた場合に壊れうるケース。**(α) にはこの節は適用されない**(意味論を変えないため)。

| 経路 | 危険か | 理由 |
|---|---|---|
| `block` / `return-from` が同一関数内 | **安全** | `return-from` の値は leaf でも、`block` body の結果チェック(`za.c:4667`)は残る |
| `return-from` が引数位置 | **安全** | 引数チェック(`za.c:4045`)の対象は引数式そのもの。`(f (return-from b 1))` の引数は leaf でない |
| `return-from` が `if` の test 位置 | **安全** | test が `(return-from …)` なら leaf でない |
| `catch` / `throw` が関数境界を越える | **安全** | 呼び出し結果は leaf でないのでチェックが残る |
| `unwind-protect` の cleanup 経由の伝播 | **安全** | protected/cleanup の結果は leaf でない限りチェックが残る |
| `tagbody` / `go`(同一関数内) | **安全** | 静的 `jmp` に解決され、制御転送値を作らない |
| `go` が `catch`/`unwind-protect` を飛び越える | **安全** | コンパイルを断念してインタプリタへ落ちる |
| 入れ子(`block` の中の `catch` の中の `tagbody` 等) | **安全** | 各層のチェックは残る。省くのは leaf の後ろだけ |
| **JIT 関数 → JIT 関数の `return-from`** | **安全** | 呼び出し結果は leaf でない。`za.c:4045`/`4601` のチェックが受ける |
| **`&rest` パラメータ参照** | **要注意** | `is_literal==3` で `za_operand_is_safe_leaf` は 0 を返す。**除外されているので現状は安全**。述語を広げる際に巻き込まないこと |
| **boxed local** | **要注意** | 同上。クロージャ間で共有される cons の cdr なので、書き込み経路が増える |
| **グローバル変数参照** | **未確認** | §4 段階 C。書き込み経路を全数把握していない |
| **インタプリタが JIT 関数を呼ぶ場合** | **安全** | `eval_args` がチェック済みの引数リストを渡す(§3-1) |
| **AOT(`lisp_compiled.c`)が JIT 関数を呼ぶ場合** | **要確認** | トランスパイラの生成コードにも `os_is_control_transfer` が 7,210 箇所ある(`eval.h:92` のコメント)。同じ規約に従っているはずだが、**本調査では確認していない** |

### 6-1 実際に全パターンが起きうる

`documents/type-system-survey.md` §6-1 のとおり、
`block` / `return-from` / `catch` / `throw` / `unwind-protect` / `tagbody` / `go` は
**すべて JIT に乗る**。したがって上表は机上の分類ではなく、
生成コード上で実際に起きるパターンである。

---

## 7. 検出手段(3-7)

### 7-1 既存テストは**この壊れ方を検出しない**

非局所脱出のテストは厚い。

| ファイル | 内容 | assert 数 |
|---|---|---:|
| `test/lisp/za_test_ext5.lisp` | 拡張5 専用。block/return-from、catch/throw、unwind-protect、tagbody/go、フォールバック確認 | 69 |
| `test/lisp/za_test_ext14.lisp` | 入れ子 tagbody | — |
| `test/c/eval_test.c` / `lisp_compiled_test.c` | インタプリタ / AOT 側 | — |

`za_test_ext5.lisp` は「動的スコープを跨ぐ throw」「JIT の throw をインタプリタの catch で
受ける」「go が catch/unwind-protect のスパンを飛び越えるとフォールバックする」まで
カバーしている。**カバー範囲としては十分である。**

**しかし、(β) の壊れ方はこれらに引っかからない。**

(β) が壊れるのは「**leaf と判定した場所に、実際には制御転送値が来たとき**」である。
上のテストはすべて「制御転送が**正しく伝播すること**」を確認しており、
チェックを省いた場所に制御転送が来る状況を作らない
(そもそも現在の設計では来ないので、テストを書きようがない)。

**つまり、テストが通ることは (β) の安全性の証拠にならない。**

### 7-2 必要なのは不変条件そのものの観測

GC 監査(`documents/gc-audit-handoff.md`、塗り潰しとトラップ)と同じ発想が要る。
案を 3 つ挙げる。

1. **デバッグビルドでの逆検査(推奨)**
   `ISIKIOS_CT_AUDIT` のようなビルドフラグを足し、
   **チェックを省いた場所に、省かない検査を残す**。
   制御転送値を見つけたらシリアルへ出して `os_panic` する。
   通常ビルドは省いたコードを出し、監査ビルドは省いた場所を全部見張る。
   **「省けるはず」を実行時に検証できる唯一の形**である

2. **書き込み側の検査**
   パラメータスロット/local スロットへ store する 4 箇所(`za.c:6285/6288/6291/6328`、
   `3319`、`4941`)に、デバッグビルドでのみ「制御転送値を書き込もうとしていないか」の
   検査を入れる。読み出し側を全部見張るより箇所が少ない

3. **静的な網羅検査**
   `za_emit_ct_check_and_jmp_if_transfer` を呼ばずに `rax` を消費する経路が
   新しく増えたときに気づけるようにする。現状 25 箇所を人手で維持しているのを
   機械的に数えるだけでも、omission list の劣化は検出できる

### 7-3 足すべきテスト

上記に加えて、(β) の実装時には次を足すべきである。

- **leaf 位置に制御転送が「来ない」ことを固定する回帰テスト。**
  例: パラメータへの `setq` が JIT 非対応であること(`%%za-compiled-p` が NIL)を
  テストで固定する。これが将来「対応」されたとき、(β) の前提が崩れることに気づける
- **AOT 経路(`lisp_compiled.c`)から JIT 関数を呼ぶ非局所脱出のテスト**(§6 の未確認項目)

---

## 8. 気づいた問題(4-3。**修正はしていない**)

### 8-1 `static inline` 化が JIT に届いていない

`src/c/eval.h:92-95` のコメント:

> [性能測定] Phase4: 生成コード中に 7,210 箇所ある最頻の呼び出し。中身はタグ判定と
> magic 比較だけで、**実測 19.5 命令のうち大半が呼び出しオーバーヘッドだった**。
> static inline へ移す。

この `static inline` 化が効くのは **C の呼び出し元(AOT の `lisp_compiled.c` など)だけ**である。
`za.c:624` は

```c
jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_is_control_transfer);
jit_call_r11();
```

と**関数のアドレスを取っている**ので、gcc は out-of-line の実体を生成し、
**JIT の生成コードは今も本物の `call` を出している**。
コメントが指摘した「呼び出しオーバーヘッドが大半」という問題が、
JIT 側には**そのまま残っている**(§5-1 の実測で 16 命令中 14 命令)。

**§5-4 の (α) は、この取りこぼしを JIT 側で回収する話である。**

### 8-2 `mov r11d, 0; cmp rax, r11` は `test eax, eax` で足りる

`za.c:625-627`(`jit_movabs_r11(0); jit_cmp_rax_r11();`)。
2 命令 9 byte が 1 命令 2 byte になる。**全 39 箇所に効く。**
同じ形は `documents/type-system-survey.md` 執筆時にも目に付いていた。

### 8-3 `documents/jit.md` の上限表が古い

`documents/jit.md:345-355` の表と実際の `#define` が食い違っている。

| 項目 | jit.md | 実際(`za.c`) |
|---|---:|---:|
| `ZA_MAX_NLX_DEPTH` | 4 | **8** |
| `ZA_MAX_LAMBDA_SLOTS` | 32 | **256** |
| `ZA_MAX_QUOTE_SLOTS` | 32 | **1024** |
| `ZA_MAX_NUMBER_SLOTS` | 32 | **512** |

後ろ 3 つは 2026-09-14 に拡張された(`za.c` のコメントに経緯がある)が、
`jit.md` が追随していない。

### 8-4 `za.c:1408` のコメントも古い

```c
#define ZA_MAX_CALL_DEPTH 4  /* ZA_MAX_NLX_DEPTHと同じ値。392mod16=8のため偶数を保つ */
```

`ZA_MAX_NLX_DEPTH` は 8 なので「同じ値」ではない。

### 8-5 制御転送の不変条件に単一の真実源が無い

`os_is_control_transfer` の呼び出しは za.c に 25 箇所、eval.c に約 20 箇所あり、
**それぞれが独立に「ここでチェックする」ことで不変条件を維持している**。
`os_tag_is_heap_ref`(`runtime.h:37`)のように「単一の真実源」としてまとめられた例が
プロジェクト内にあるのと対照的である。

(β) を入れるかどうかに関わらず、**「制御転送値が変数束縛に入らない」ことを
どこかで言語化しておく価値がある**(現状はコードに散らばっているだけ)。

---

## 9. 次にやること(本調査では決めない)

1. **(α) チェックのインライン展開。** 意味論を変えず全 39 箇所に効き、
   1 箇所 16 命令 → 約 5 命令。§8-2 の `test eax,eax` も同時に入れる。
   **実装指示書を書く材料は揃っている**
2. (α) を入れて**同一ブート内で速度を測る**。残差を見てから (β) の要否を判断する
3. (β) を入れるなら、**§7-2 の監査ビルドを先に作る**。
   既存テストは (β) の壊れ方を検出しないため、テストが通ることは安全性の証拠にならない
4. §4 段階 C(グローバル変数参照)と §6 の AOT 経路は未確認。
   (β) をグローバル変数まで広げるなら先に詰める

---

## 10. 回帰

コードを変更していないので変化しないことの確認のみ。

- `make test` … **8,343 件 / 失敗 0**
- `make test-qemu` … **3,570 件 / 失敗 0**
