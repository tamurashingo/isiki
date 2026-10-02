# JIT がその構文をどう扱うか(一覧)

JIT(`za_try_compile_defun`)が**ある構文を受け取ったときに何が起きるか**を
1 枚にまとめる。`safety` を入れるときも `*` `/` の展開を入れるときも、
**「この構文は JIT でどうなるか」を毎回調べ直すことになる。** ここに書いておけば
その都度の調査が引ける。

## この表の前提 — 症状は 4 つある

**「JIT に乗ったか」だけでは足りない。** PR #115 で
`%%za-compiled-p` が T の `for` が AOT より 2.6 倍遅い、という形で分かった。

| 症状 | 見分け方 | 何が起きているか |
|---|---|---|
| **① 乗らない** | `%%za-compiled-p` が `NIL` | `defun` 全体がインタプリタ。1 反復あたり 1 桁遅い |
| **② 落ちる** | `%%za-compiled-p` は T だが **`os_eval` の命令数が N に比例して増える** | 外側は JIT だが本体の一部がインタプリタを呼ぶ。その数字は「JIT の性能」ではない |
| **③ 展開形が AOT と食い違う** | 同じ S 式なのに AOT と JIT で命令数が桁違い。**確保量の傾きで裏が取れる** | `init.lisp` の `defmacro` と `transpile.lisp` の移植版が別物になっている。**2 つの数字は比較できない** |
| **④ 乗るが実行時に壊れる / 止まる** | 深さや大きさを変えると返ってこない | C スタック枯渇など |

① は `%%za-compiled-p`、② は `os_eval` 区間の傾き
(`tools/bench/run_jit_bench.sh` のゲート2)、③ は**確保量の傾き**
(`%%HEAP-USED-BYTES`。規則 9 のとおり、これはぶれない計器である)で見る。

## 一覧

確認したカーネルのリビジョンは **`8a92737`**(`feature/compiler-optimization`。
PR #115 も本作業もカーネルに入るソースを変えていない)、確認日は
いずれも **2026-10-02**。

| 構文 | 症状 | 詳細 | 確認方法 |
|---|---|---|---|
| **パラメータへの `setq`**<br>`(defun f (n) ... (setq n ...))` | **①乗らない** | `%%za-compiled-p` = `NIL`。インタプリタでは正しい答えを返す | `test/lisp/bench_jit_guard_test.lisp`(常時テスト)。`documents/control-transfer-survey.md:485` |
| **並列束縛 `let` で 5 つの init が同じローカル変数を参照**<br>`(let ((a i) (b i) (c i) (d i) (e i)) e)` | **①乗らない** | `%%bench-jit-let5par` が `NIL`。**原因は未調査。** 束縛数を減らした形・`let*` 版(`let1` / `let` / `let10` / `let5const`)はいずれも T | `test/lisp/bench_jit_test.lisp`(常時テスト。期待値を `nil` で固定してある。**T になったら退行ではなく改善**) |
| **`for`** | **③展開形が食い違う** | `%%za-compiled-p` は T、`os_eval` の傾きも 0。**だが `init.lisp` の `for` は毎反復 `(list step1 step2)` を作る旧展開形**で、`transpile.lisp` の `expand-for` は一時変数+素の `setq` の新展開形。実測で **JIT 側だけ 32 byte/反復(cons 2 個)**確保する。命令数は JIT 635.29 対 AOT 244.57(2.6 倍) | `documents/performance-measurement.md`「`for` の 2.6 倍の切り分け」節 |
| **非末尾再帰(深さ 34 以上)** | **④止まる** | 深さ 33 までは返る。34 で C スタック(`STACK_SIZE` = 256KB)を使い切りガードページを踏む。**報告は `-serial stdio` にしか出ない**ので、外からは QEMU の無反応と区別がつかない。AOT 版は同じ S 式で深さ 100 が通る(1 段 330 byte に対し JIT は約 7.5KB) | `documents/performance-measurement.md`、`documents/known-issue-deep-if-nesting-jit.md` の「残っているタスク」 |

## 乗ると確認したもの(否定の結果。規則 6)

**「乗らない」と書かれていたのに、いまは乗るもの**がある。
消さずに残しておかないと、次の人が同じ調査を繰り返す。

| 構文 | 以前の記録 | 2026-10-02 の実測 |
|---|---|---|
| **`labels`** | 作業指示書が「以前インタプリタに落ちると確認済み」としていた | **`%%za-compiled-p` = T。乗る** |
| **`tagbody` / `go`** | `documents/bench-pinned-nil.md:149` が「JIT が断念する(`%%za-compiled-p` が nil)」 | **T。乗る** |
| 素の `let` + `while` | — | T |

候補 4 つを 1 ブートで測った実測:

```
tagbody=T   labels=T   パラメータへの setq=NIL   素の let+while=T
```

したがって**陽性対照に使えるのはパラメータへの `setq` だけ**である
(`labels` を使うと対照が反応しない)。

## 症状②(落ちる)の陽性対照

**①と②は別物なので、対照も別に要る。**
`test/lisp/bench_jit_eval_control.lisp` に「**ゲート1 を通ってゲート2 で落ちる**」
形を置いてある。

```lisp
(defun %%bench-eval-callee (x) (progn (setq x (+ x 1)) x))   ; NIL(①乗らない)
(defun %%bench-jit-evalfall (n) ... (setq acc (%%bench-eval-callee acc)) ...)  ; T
```

外側は JIT なのに、ループ本体が毎反復インタプリタの関数を呼ぶ。

```
BENCH_PATHS=jit BENCH_CASES=evalfall BENCH_REPEAT=1 BENCH_NO_CHECK=1 \
  make test-qemu-jit-bench
```

## 足すときの作法

- **症状を 4 つのどれかに分類する。** 「動かない」だけでは次の作業が始まらない
- **確認方法を同じ行に書く**(規則 1)。常時テストにできたならそのファイル名を書く
- **確認したリビジョンと日付を書く。** 上の `labels` のように、
  **JIT の対応範囲は動く。** 日付の無い記録は「現状」とは読めない
- **乗るようになったものも消さずに残す**(規則 6)
