## 何をしたか

`<error>` 発生時の脱出処理を仕様に合わせる作業(P0〜P6)を `feature/compiler-optimization` へ取り込みます。
7 本の PR(#94, #96, #98, #99, #100, #101, #102, #103, #104, #105, #106)をまとめたものです。

**要点は「エラーを検出する側」を直したことです。** `with-handler` / `signal-condition` /
`continue-condition` そのものは調査した範囲で仕様に適合していました。不適合だったのは
**エラーの検出側が §9.1(a)「Evaluation of the current form shall stop」を守っていない**ことと、
**§9.2 の undefined-entity / arity を一切検出していない**ことでした。

## フェーズごとの結果

| | 内容 | `EVAL-ERROR` 予算 | `make test-qemu` |
|---|---|---|---|
| P0 | 検出器(予算ツール)と調査文書、`assert-error-class` | 50 | — |
| P1 | C から Lisp を呼び戻す箇所の制御転送の取りこぼし(`stream.c` / `mount.c`) | 50 | — |
| P2 | トップレベルの整備(内側 `%TOP-LEVEL` の握り潰し、environment の巻き戻し、`load` の表示) | 50 | — |
| P3 | `with-handler` の脱出先(ハンドラが正常 return したらその `with-handler` の値になる) | 50 | — |
| P4-1 | 算術(0 除算・定義域) | 44 | 4,674 |
| P4-2 | 添字・範囲(**`subseq` に範囲検査が無かった**) | 27 | 4,727 |
| P4-3 | 未定義関数・immutable-binding・非関数の呼び出し | 21 | 4,775 |
| P4-4 | 入出力(`load` / `open-*-file` / `file-length`) | 6 | 4,807 |
| P5 | 算術の型違い・未束縛変数 | 6 | 4,886 |
| P6 | arity 不一致(インタプリタ / JIT / AOT) | 6 | **4,937** |

`return g_sym_eval_error` は **50 → 6** になりました。残る 6 箇所はいずれも意図して残すもので
(条件システムが使えないときのフォールバック 3 + reader が読み取りエラーへ翻訳する
配列リテラルの状態 3)、**この予算はもう下がりません**。`tools/check_eval_error_budget.py` が
上下どちらにずれてもビルドを落とします。

## 途中で見つけて直した実害

- **`subseq` に範囲検査がそのものが無かった**(P4-2)。`(subseq (vector 1 2 3) 0 99)` が
  隣接するヒープ上のオブジェクトをそのまま Lisp 値として返し、その後 GC が辿ろうとして**ハング**した
- **`init.lisp` を読まない起動で signal すると無限再帰でスタックを食い潰した**(P4-2)。
  `make-instance` はあるのに条件クラスが 0 個という食い違いが原因。QEMU 上で
  `#PF: CR2 = RSP-8` を gdb スタブで押さえて特定した
- **`%%class-slots` が型検査なしで任意の値の word3 を生で読んでいた**(P4-2)。
  `(%%class-slots nil)` が `6931512` を返し、`make-array` がそれを使ってヒープを使い切っていた
- **`apply_function` がタグを見ずに word0 を読んでいた**(P4-3)。`((car '(1 2)) 3)` で**アドレス 0 を読む**
- **JIT の末尾呼び出しだけ未定義関数のクラスが違っていた**(P4-3)。共有トランポリンが
  Function Cell を捨てていたため。同じソースが末尾呼び出しかどうかでクラスが変わっていた
- **`(member 1)` のような呼び出しが黙って通っていた**(P6)。AOT 済み関数の cons エントリが無検査だった
- `cc_format` が FAT ストリームで GC 安全でない件は issue #97 として分離(未修正)

## 新しく常設になったもの

| | 内容 |
|---|---|
| `tools/check_eval_error_budget.py` | `make test` から走る。`EVAL-ERROR` 返しの箇所数が予算と違えば落ちる |
| `make test-qemu-error-escape-gc` | **GC_DEBUG ビルドで P2〜P6 のエラー脱出テストを全部流し、`g_gc_lifo_violations == 0` をアサーションで固定する。** `test-qemu-all` に入っている(357 件) |
| `test/lisp/qemu_boot_no_init_signal.lisp` | **`init.lisp` を読まない唯一の boot-entry。** 条件クラスが無い状態で signal しても戻ってくることを固定する |
| `assert-error-class`(`test_framework.lisp`) | signal されたクラスまで固定する。`assert-error` では 0 除算が `<simple-error>` でも通ってしまう |

## 文書

| | 内容 |
|---|---|
| `documents/error-unwind-survey.md` | 調査(P0)。**冒頭に「2026-09-23 時点であり現状ではない」と明記済み** |
| `documents/unbound-arity-survey.md` | 未束縛変数と arity の経路ごとの費用(P6) |
| `documents/islisp-implementation-defined.md` | **仕様が未定義/未記述の部分で採った挙動の一覧。** 後から仕様の挙動と誤解しないため |

`documents/type-system-survey.md:768` の「safety を見送った理由(例外機構が無い)」にも、
その前提が消えたことと §9-5 の閾値案が既定値と噛み合っていないことを追記しました。

## 性能

**速い経路は原則として増やしていません。** 検査はいずれも「すでに速い経路を外れたあと」に
置いてあります(算術は fixnum 高速経路の後、比較は偽になった分岐の内側、変数参照は
親環境まで辿った後、arity は cons リストエントリのみ)。objdump で確認した代表値:

| | 開始前 | 現在 |
|---|---|---|
| `os_get_variable` | 39 | **39(byte 単位で同一)** |
| `primitive_add2_fixnum` | 54 | **54** |
| `primitive_elt2` / `primitive_elt` / `primitive_set_elt` | 5 / 18 / 28 | **5 / 18 / 28** |
| `lisp_ll_member__step_fixed`(AOT 固定引数) | 284 | **284** |
| `bind_params` | 101 | **98(-3)** |
| **`primitive_less_than2` 他 2 引数比較 6 個** | 8 | 22(**成功経路 +5**) |

**唯一の悪化は 2 引数比較のガードです。** 構文別ベンチ(傾き法、前後 2 回ずつ)で
`tailrec +2.6%`(散らばり 0.7/0.9 なので確実)、`for +6.1%`(開始前の散らばりが 9.5 と大きい)。
`let +0.7%` / `funcall +1.3%` は散らばりの範囲です。算術の fixnum 高速経路は
逆アセンブル差分で不変を確認しています。

これは **`declaim` の `safety` で外せる分**です。受け皿(`OPTIMIZE_SAFETY` / `g_za_declaim`)は
既にあり未通電なので、引き継ぎメモを **issue #112** に置きました。

> ### ⚠ ベンチの基準値について(本 PR とは無関係の別件)
>
> `documents/performance-measurement.md` の記録(`loop` 105.08 / `arith` 142.00、基準 `7b5ce56`)と、
> **`feature/compiler-optimization` の head(`bf68e80`)での実測(`loop` 175.18 / `arith` 239.11)が
> 約 +68% 乖離しています。**
>
> 同じスクリプト・同じ条件で `7b5ce56` を測り直すと **`loop` 104.38 / `arith` 141.97** で記録と一致するため、
> **`7b5ce56`〜`bf68e80` の 165 コミットのどこかに、本作業とは別の悪化が入っています。**
> 上の「開始前」は `bf68e80` なので、本 PR の評価そのものには影響しません。
> 別 issue として切り出します。

## 確認

- `make test`: **8,517 OK / 0 NG**
- `make test-qemu-all`: **10 本すべて 0 failed**
  (4937 / 11 / 357(GC_DEBUG のエラー脱出回帰)/ 5 / 152 / 15 / 135 / 11 / 38 / 38)
- `ISIKIOS_GC_DEBUG` ビルドで `g_gc_lifo_violations == 0` と signal の深さ打ち切り 0。
  **期待値を 999 にすると落ちることを陽性対照で確認済み**

## 引き継ぎ(issue)

| # | 内容 |
|---|---|
| #97 | `cc_format` が FAT ストリームで GC 安全でない |
| #107 | AOT の `unwind-protect` が cleanup 内の脱出を捨てている |
| #108 | `*handlers*` が全プロセスで共有されている |
| #109 | `with-environment` に環境名を渡すとハング |
| #110 | `<storage-exhausted>` を signal していない |
| #111 | `setq` が未束縛の変数に束縛を作る(spec:2167-2169 違反) |
| #112 | **`declaim` の `safety` を実際に効かせる**(受け皿・閾値・費用の実測値) |

🤖 Generated with [Claude Code](https://claude.com/claude-code)

