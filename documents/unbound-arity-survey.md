# 調査: JIT での未束縛変数と arity 不一致(P6)

> 本書は実装ではなく**計測と方針案**である。実装は別途の指示を待つ。
>
> 対象は `documents/error-unwind-survey.md` §4-2 の #9(未束縛変数)と #10(arity error)、
> および §6-3 の C2。仕様の引用は同書と同じ `spec:NNNN`
> (ISLisp Working Draft 23.0 を `pdftotext -layout` で起こしたテキストの行番号)。

---

## 1. 結論

1. **未束縛変数は P5 で全経路が片付いている。** P6 の作業として残っていない。
   `os_get_variable_checked` を JIT も使うようにした時点で、インタプリタ・JIT の
   両方が `<unbound-variable>` を signal する。AOT はそもそも未束縛の裸シンボルを
   **トランスパイル時に拒否**する。
2. **残っているのは arity 不一致だけ。**
3. **arity の検査は、固定引数エントリでは 0 命令で入る。** cons リストエントリだけが
   費用を払う。**for / let / funcall / tailrec のベンチマークでは差を分離できなかった**
   (それらは固定引数エントリを通るため)。
4. 実測した費用は **cons エントリ 1 回あたり検査 2 回(約 6 命令)、AOT の
   生成コード全体で +3.65%** である。arity の数に依存しない形にできる(§5-3)。
5. **調査中に別件を 1 つ見つけた**(§6): `setq` が未束縛の変数に対して
   **新しい束縛を作ってしまう**。spec:2167-2169 は「setq can be used only for
   modifying bindings, and not for establishing a variable」と定めている。

---

## 2. 未束縛変数: 経路ごとの現状(P5 完了後)

| 経路 | 入口 | 現状 |
|---|---|---|
| インタプリタのシンボル評価 | `eval.c` `os_eval` → `os_get_variable_checked` | **signal する**(P5) |
| JIT の裸シンボル読み出し | `za.c` `is_literal == 9` → `os_get_variable_checked` | **signal する**(P5) |
| AOT の裸シンボル読み出し | `transpile.lisp` `transpile-expr` | **トランスパイル時にエラー**(`"未束縛の変数参照です"`)。実行時には到達しない |
| AOT のクロージャ自由変数の捕捉 | 生成コードの `os_get_variable`(683 箇所) | nil のまま。**捕捉環境に無ければ実装のバグ**であり利用者のエラーではない |
| スケジューラ内部 | `process.c` `interrupt.c` の `os_get_variable` | nil のまま。**意図的**(「まだ束縛されていない」を nil で判定している。かつ割り込みハンドラから Lisp を呼べない) |

JIT の実機確認は P5 の `test/lisp/type_unbound_test.lisp` に入っている
(`p5-unbound-jit` が `%%za-compiled-p` で JIT 化を確認した上で
`<unbound-variable>` を期待し、後から `defglobal` すると読めることも固定している)。

**`za.c:1852` のコメントが P5 の変更に追従していない**(「見つからない場合は nil を
返す」と書いたまま)。実装時に直すべき小さな残務。

---

## 3. arity 不一致: 現状と仕様

### 3-1. 仕様

| 参照 | 内容 |
|---|---|
| spec:896-898(§9.2 (2)) | 「an error shall be signaled if a function is activated with a number of arguments which is different than the number of parameters as required in the function definition (error-id. arity-error)」 |
| spec:1549-1550(`lambda`) | 「An error shall be signaled if the number of arguments received is incompatible with the specified lambda-list (error-id. arity-error)」 |
| spec:7191-7194(§29.4) | `arity-error` は **`<program-error>`** のコンディションで表す |

`<program-error>` はスロットを持たないので、**期待した個数と実際の個数は condition に
載せられない**(P4-2 の `index-out-of-range` と同じ制約)。

### 3-2. 現状: 3 つの経路すべてが黙っている

**インタプリタ** — `eval.c` `bind_params`:

```c
lisp_val_t val = (a != nil) ? cc_car(a) : nil;   /* 足りない分は nil で埋める */
...
while (p != nil) { ... }                          /* 余った分は捨てる */
```

**JIT** — `za.c` のプロローグ(`use_param_slots` の展開ループ)が
`cc_car` / `cc_cdr` で位置的に読むだけ。`cc_car(nil)` は nil なので足りない分は nil、
余りは `ZA_OFF_ARGS_VAL` に残したまま無視される。

**AOT** — `transpile.lisp` `emit-param-binding-stmt` が
`lisp_val_t x = cc_car(evaluated_args); evaluated_args = cc_cdr(evaluated_args);`
を並べるだけ。JIT と同じ。

### 3-3. **固定引数エントリでは arity は既に保証されている**

これが費用の話の要点である。JIT も AOT も 2 つの入口を持つ。

| 入口 | 引数の受け取り方 | arity |
|---|---|---|
| 固定引数(`__step_fixed` / `meta->fixed_entry`) | C の仮引数 / レジスタ渡し | **呼び出し側が `meta->arity == argc`(JIT)/ `fixed_argc`(AOT)を実行時に照合してから入る。一致しなければこの入口を使わない** |
| cons リスト(`__step` / `meta->cons_entry`) | 評価済み引数リストを位置で辿る | **無検査** |

つまり **arity が合わない呼び出しは必ず cons エントリへ落ちる**。
したがって検査を cons エントリだけに置けば、
**固定引数エントリ(=速い経路)には 1 命令も増えない。**

---

## 4. 必要な変更

| 箇所 | 変更 |
|---|---|
| `runtime.c` / `runtime.h` | `os_signal_arity_error(env)` を新設。`<program-error>` を signal する(spec:7191-7194) |
| `eval.c` `bind_params` | 引数が尽きた分岐を signal へ。ループ後に「余り」を 1 回検査。**既存の三項演算子が `a` を見ているので正常系に命令は増えない** |
| `eval.c` マクロ展開の `bind_params` 呼び出し | マクロの arity は展開時の誤りなので**別に扱う**(評価中の signal とは行き先が違う)。本調査では扱っていない |
| `za.c` プロローグ(cons 経路) | 展開ループに検査を入れる。**未着手**(§5-4) |
| `transpile.lisp` `emit-param-binding-stmt` / `param-scope-and-preamble` | 同上。試作済み |
| `za.c:1852` のコメント | P5 の変更に追従させる |

---

## 5. 費用の実測

試作は 2 版作った。どちらも `make test` **8,517 OK / 0 NG**、
`make test-qemu` **4,886 passed / 0 failed** で、
**既存のコードに arity のずれに依存したものは 1 つも無かった。**

- **試作A**: 固定パラメータ 1 つごとに「引数が尽きていないか」を検査する素朴な形
- **試作B**: 検査を **「最後の固定パラメータの直前」と「全パラメータの後」の 2 回だけ**にした形。
  `cc_car(nil)` / `cc_cdr(nil)` がどちらも nil なので、途中で引数が尽きていれば
  最後のパラメータの時点でも必ず nil になっている。よって**1 回で「足りない」を
  全部捕まえられる**。「多すぎ」はループ後の 1 回。**arity の数に依存しない**。
  あわせて cold 側(`tco_result_t` の組み立て)を `noinline` のヘルパへ追い出した

### 5-1. 生成コードの命令数(`lisp_compiled.o`、`-O1`)

| 関数 | 固定引数の数 | base | 試作A | 試作B |
|---|---|---|---|---|
| `lisp_ll_member__step` | 2 | 307 | 344(+37) | **327(+20)** |
| `lisp_ll_assoc__step` | 2 | 343 | 380(+37) | **363(+20)** |
| `lisp_ll_make_instance__step` | 1+&rest | 493 | 508(+15) | **498(+5)** |
| `lisp_ll_reverse__step` | 1 | 111 | 126(+15) | 136(+25) |
| **`lisp_ll_member__step_fixed`** | 2 | 284 | **284(+0)** | **284(+0)** |

**固定引数エントリは両版とも増分 0。** §3-3 のとおり。

`reverse`(1 パラメータ)だけ試作B が試作A より大きいのは、パラメータ 1 個では
B の検査回数(2 回)が A(1 回)より多いことと、追加した制御フローの周りで
gcc のレジスタ割付が崩れたためである。**静的な命令数は実行命令数の代理にならない**
(下記)。

### 5-2. 実行される命令(成功経路)

追加されるのは検査 1 回あたり次の 3 命令だけで、残りは cold 側にある。

```
mov  <nil のアドレス>,%rax
cmp  %rdx,(%rax)
je   <cold>
```

`nil` がグローバル変数なのでアドレスのロードが 1 つ余計に入っている
(JIT 側には `ISIKIOS_PINNED_NIL` で nil を r12 に常駐させる実験があり、
そちらなら 1 命令減る)。

| 経路 | 実行される増分 |
|---|---|
| 固定引数エントリ | **0** |
| cons エントリ(試作A) | 約 3 × 固定パラメータ数 |
| cons エントリ(試作B) | **約 6(arity に依存しない)** |
| インタプリタ `bind_params` | **0**(既存の分岐を差し替えるだけ) |

### 5-3. オブジェクトサイズ(AOT 生成コード全体)

| 版 | bytes | 増分 |
|---|---|---|
| base | 2,768,628 | — |
| 試作A | 2,905,158 | +4.93% |
| **試作B** | 2,869,711 | **+3.65%** |

カーネルイメージに乗るので、ここは無視できない。試作B の `noinline` ヘルパ化で
1.3 ポイント削れている。

### 5-4. 端点のベンチマーク(構文別、傾き法、AOT 版 命令/単位)

`make test-qemu-construct-bench BENCH_CASES="for let funcall tailrec"`

| カテゴリ | base | 試作A | 試作B |
|---|---|---|---|
| for | 256.44 | 255.80 | 257.48 |
| let | 606.38 | 605.40 | 605.27 |
| funcall | 295.94 | 297.15 | 298.07 |
| tailrec | 365.65 | 368.81 | 366.76 |

**差は分離できない。** 増減が ±3 の範囲で符号も揃わない(for と let は base より
小さい値が出ている)。P5 で確認した同一ビルド内の散らばり(loop で 11.5、
arith で 3.6)と同じ大きさである。

**これは計器の限界ではなく、構造どおりの結果である。**
`funcall` / `tailrec` の呼び出しは AOT の**固定引数エントリ**
(`.is_tail_call = 2, .fixed_fn = ..._step_fixed`)を通るので、
試作が触った cons エントリを一度も実行しない。

**cons エントリを通る経路のベンチマークは存在しない。** 測るには
`&rest` 付き・引数 4 個以上・`funcall` / `apply` 経由のいずれかを使う
ベンチマークを新設する必要がある(本調査では作っていない。**未計測**)。

---

## 6. 調査中に見つかった別件(修正していない)

**`setq` が未束縛の変数に新しい束縛を作る。** `runtime.c` `os_setq_variable` は
親チェーンを辿って見つからなかった場合、末尾で `os_set_variable` を呼んで
**その環境に束縛を作る**。

spec:2167-2169 は `setq` について
「This result is used to modify the variable binding denoted by the identifier var
(if it is mutable). **setq can be used only for modifying bindings, and not for
establishing a variable.** The setq special form must be contained in the scope of
var, established by defglobal, let, let*, for, or a lambda expression」
と定めている。したがって `(setq 未束縛 1)` は `<unbound-variable>` 相当を
signal すべきで、黙って束縛を作るのは仕様に反する。

`os_setq_variable` には create-on-miss を前提にしている内部の呼び出し元が
あるかもしれないので、`os_get_variable` / `os_get_variable_checked` と同じ
**入口の二分**(P5 と同じ手口)が要る。**本調査では確認していない。**

---

## 7. 方針案

**推奨: 試作B の形で入れる。** 根拠:

1. **速い経路(固定引数エントリ)の費用が 0** である。§3-3 のとおり構造から保証される
2. cons エントリの費用が **arity に依存しない 2 回の検査**に収まる
3. インタプリタは既存の分岐を差し替えるだけで **0 命令**
4. 既存テスト(`make test` 8,517 / `make test-qemu` 4,886)が**両版とも無改変で通る**。
   arity のずれに依存したコードが 1 つも無いことが確認できている

**段階を分けるなら**、インタプリタ(`bind_params`)だけ先に入れる手がある。
費用 0 で、仕様違反のうち**最も踏みやすい経路**(インタプリタ実行の `defun`)が閉まる。
JIT / AOT の cons エントリは固定引数エントリに乗らない呼び出しに限られるので後回しにできる。

**`safety` との関係**: P5 の PR #104 で整理したとおり、`OPTIMIZE_SAFETY` の受け皿は
既にある(`runtime.h:857-861`、`za.c` の `g_za_declaim`)。cons エントリの検査も
`safety 0` で外す対象として素直に乗る。**既定は検査する側**(`OPTIMIZE_DEFAULT` は
safety=1)。ただし本件は固定引数エントリが 0 費用なので、safety を待つ必要は薄い。

**決めていないこと**:

- `<program-error>` にスロットが無いため「期待 N 個・実際 M 個」を運べない。
  仕様に無いクラスを増やすかどうかは P4-2 と同じ判断(**増やさない**方針で通してきた)
- マクロ展開時の arity をどう扱うか(展開時の誤りなので評価中の signal とは行き先が違う)
- ネイティブプリミティブの arity(`(car)` が nil を返す等)。数百箇所あり、
  本調査の範囲外。`bind_params` を直しても閉まらない
