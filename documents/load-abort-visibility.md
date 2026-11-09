# 読み込みの打ち切りを母艦から見えるようにする(#197)

`load` は個々の最上位の式が打ち切られても**成功を返し、理由を画面にしか出さない**。
画面を見ない試験では「静かに半分欠けた」になる。ここはその穴を塞いだ記録である。

- 出典の調査: `docs/investigation/detector-t4-passline.logic.md` §4-3 / §9-4 / §9-5、
  `docs/investigation/detector-t4-passline.refs.md` §C
- 隣の検出器: `tools/check_test_count_ratchet.py`(#204。段ごとの件数の名簿)、
  `tools/check_pass_line.py`(#205。合否の一行そのもの)。
  **この 3 つは別のものを見ている**(下の §5)

---

## 1. 何を変え、何を変えないか

**`load` の意味は変えない。** 契約(`src/c/load.h` の「打ち切りは失敗にならない」)は
そのままである。1 つの式が失敗しても次へ進むのが、ユーザの使う `load` としては妥当である。

**変えるのは「母艦から見えるか」だけ。** C で数えて直列線へ出し、母艦側に判定を置く。
前例は `src/c/kernel.c` の 9P の無い起動の要約(C が 1 件ずつ数えて直列線へ出す)。

Lisp へ組み込み関数で渡す案は採らない。**`native` 関数を 1 つ足すと小さいヒープの
試験が固まる**罠がある(`SMALL_HEAP_SIZE` の再調整が必要。`documents/pitfalls.md`)。

---

## 2. 測る前に宣言した予測(規則 11)

**この節は実装・実測より前に書いてコミットした。** あとから書けば同じ文章が言い訳になる
(規則 11)。実測は §3 に別の節として足す。**★ は読みで埋めない。**

### 2-1. 構文ごとに「打ち切りとして数えられるか」の予測

いまの実装では、打ち切り(`*out_aborted = 1`)になるのは
**宛先が `%TOP-LEVEL` で、かつ `MAGIC_BLOCK_EXIT`** のときだけである
(`src/c/eval.c` の `os_eval_top_level_ex`)。したがって:

| # | 最上位に置く形 | 直す前の予測 | 根拠(読み) |
|---|---|---|---|
| A | `(return-from %top-level 9)` | **数えられる** | `MAGIC_BLOCK_EXIT` + 宛先が `%TOP-LEVEL` |
| B | `(block b (return-from b 1))` | 打ち切りでない(正常値) | `block` が自分で捕まえる |
| C | `(return-from no-such-block 1)` | **数えられる** | 宛先が生きていないので `<control-error>` を signal し、最上位への中断になる |
| D | `(catch 'tg (throw 'tg 1))` | 打ち切りでない(正常値) | `catch` が自分で捕まえる |
| E | `(throw 'tg 1)`(`catch` 無し) | **数えられない** | `MAGIC_CATCH_EXIT` で宛先は `tg`。`%TOP-LEVEL` ではないので素通りする |
| F | `(tagbody (go done) (setq x 1) done)` | 打ち切りでない(`nil`) | `tagbody` が自分のタグとして解決する |
| G | `(go nowhere)`(`tagbody` 無し) | **数えられない** | `MAGIC_GO_EXIT` で宛先は `nowhere` |
| H | `(tagbody (go nowhere))` | **数えられない** | `tagbody` は自分のタグでない脱出をそのまま伝播する |
| I | `(car)`(引数不足などの素のエラー) | **数えられる** | signal が最上位への中断になる |

**直したあとの予測: A〜I のうち B・D・F は打ち切りでなく、残る A・C・E・G・H・I が
すべて数えられる。** 対照群(数えてはいけない 3 つ)を多めに並べてある(規則 11 の形)。

**どう転んだら何を意味するか:**

- E・G・H が直す前から数えられていたら、調査 §C-2 の「数えられていない脱出」は
  存在しない = 直す必要が無い。**その場合は実装を減らす**
- B・D・F が直したあとに数えられてしまったら、**正常な脱出を打ち切りと誤認している** =
  誤検出である。1 周で誤って鳴る(陰性対照が落ちる)
- H だけ外れる(直したあとも数えられない)なら、`tagbody` が脱出を**飲んで**いる
  ことになり、伝播の読み(`eval.c` の「自分のタグではない脱出シグナルはそのまま伝播」)が
  間違っていたことになる

### 2-2. 件数の行の予測

- **0 件でも 1 行出す。** 出ないことを成功と読ませてはいけない(#204 / #208 で学んだ形)
- 通常の 1 周(`test/lisp/qemu_boot_test.lisp`)の総数は **0** と予測する。
  0 でなければ、**いま現に静かに欠けている**ということである
- 母艦側は「行が無い」「総数が非 0」「行が 2 本ある」のいずれでも落ちる

---

## 3. 実測(§2 の予測を実機とホストで確かめた結果)

> この節は実測のあとに書いた。**★ は 1 つも残っていない**(構文 9 通りすべてを
> 実際に走らせた)。使った QEMU は **1 段 2 回**だけである。

### 3-1. 構文ごとの実測(単体試験。`make test` で毎回走る)

`test/c/load_test.c` の `test_load_counts_every_kind_of_escape` が
走るたびにこの表を印字する。**「直す前」の列は別の木を建てずに取っている:**
`os_eval_top_level_ex` は今回**変えていない**ので、同じ式をその関数だけに
食わせれば直す前の分類がそのまま出る(規則 8 の陽性対照)。

```
  [#197] 構文ごとの実測(1 = 数えられる / 0 = 数えられない):
    A  (return-from %top-level 9)       直す前=1 直した後=1 直列線=[load] abort <memory>: <reason unavailable>
    E  (throw 'tg 1) / catch 無し       直す前=0 直した後=1 直列線=[load] abort <memory>: uncaught catch-exit (throw with no matching catch)
    G  (go nowhere) / tagbody 無し      直す前=0 直した後=1 直列線=[load] abort <memory>: uncaught go-exit (go with no matching tagbody tag)
    H  (tagbody (go nowhere))           直す前=0 直した後=1 直列線=[load] abort <memory>: uncaught go-exit (go with no matching tagbody tag)
    B  (block b (return-from b 1))      直す前=0 直した後=0 直列線=(無し)
    D  (catch 'tg (throw 'tg 1))        直す前=0 直した後=0 直列線=(無し)
    F  (tagbody (go done) ... done)     直す前=0 直した後=0 直列線=(無し)
```

**条件クラスを 1 つも登録していない試験なので、signal の経路(C と I)はここでは測れない**
(`os_signal_condition` が `g_sym_eval_error` で素通りする)。それは実機で測った。

### 3-2. 構文ごとの実測(実機。`make test-qemu-load-abort-positive` を 1 回)

台本は `test/lisp/qemu_boot_load_abort_positive.lisp`(A〜I を 1 つずつ最上位に置いた)。
直列線に出た 6 行がそのまま答えである(`load` は上から順に評価するので、
**出た順で構文に対応が付く**)。

```
[load] abort test/lisp/qemu_boot_load_abort_positive.lisp: <reason unavailable>
[load] abort test/lisp/qemu_boot_load_abort_positive.lisp: <CONTROL-ERROR>
[load] abort test/lisp/qemu_boot_load_abort_positive.lisp: uncaught catch-exit (throw with no matching catch)
[load] abort test/lisp/qemu_boot_load_abort_positive.lisp: uncaught go-exit (go with no matching tagbody tag)
[load] abort test/lisp/qemu_boot_load_abort_positive.lisp: uncaught go-exit (go with no matching tagbody tag)
[load] abort test/lisp/qemu_boot_load_abort_positive.lisp: NIL is not of expected class #<CLASS <CONS>>
[load] file test/lisp/qemu_boot_load_abort_positive.lisp: aborted 6
[load] aborted top-level forms: 6
```

### 3-3. 予測との照合(**★ は無い。9 通りすべて実測した**)

| # | 最上位に置く形 | 予測 | 直す前(実測) | 直した後(実測) | 当たったか | どこで測ったか |
|---|---|---|---|---|---|---|
| A | `(return-from %top-level 9)` | 数えられる | **1** | **1** | ○ | 単体 + 実機 |
| B | `(block b (return-from b 1))` | 打ち切りでない | 0 | **0** | ○ | 単体 + 実機 |
| C | `(return-from no-such-block 1)` | 数えられる | — | **1**(`<CONTROL-ERROR>`) | ○ | **実機のみ**(条件クラスが要る) |
| D | `(catch 'tg (throw 'tg 1))` | 打ち切りでない | 0 | **0** | ○ | 単体 + 実機 |
| E | `(throw 'tg 1)` / `catch` 無し | **数えられない** | **0** | **1** | ○ | 単体 + 実機 |
| F | `(tagbody (go done) ... done)` | 打ち切りでない | 0 | **0** | ○ | 単体 + 実機 |
| G | `(go nowhere)` / `tagbody` 無し | **数えられない** | **0** | **1** | ○ | 単体 + 実機 |
| H | `(tagbody (go nowhere))` | **数えられない** | **0** | **1** | ○ | 単体 + 実機 |
| I | `(car)`(引数不足) | 数えられる | — | **1**(`NIL is not of expected class #<CLASS <CONS>>`) | ○ | **実機のみ** |

- **調査が ★ を立てていた `go` / `tagbody`(G / H)は、予測どおり「数えられていなかった」。**
  直したあとは数えられ、理由も `uncaught go-exit` として直列線へ出る
- 対照群 **B / D / F は直したあとも 0 件**で、誤検出していない
- **A の理由だけが `<reason unavailable>` になる。** 最上位へ逃げた値が条件のインスタンス
  ではない(この台本では fixnum の `9`、`isiki_test.lisp` では印のシンボル)ため、
  `%REPORT-CONDITION-STRING` が文字列を返さず、理由を引けない。
  **件数と「どのファイルか」は出るので判定には足りる**が、
  理由の文面としては弱い。改善は別件とする

### 3-4. **予測を外した 1 つ: 通常の 1 周の件数は 0 ではなく 7 だった**

§2-2 で「通常の 1 周の総数は **0**」と宣言した。**外れた。**
`make test-qemu`(`test/lisp/qemu_boot_test.lisp`。**5178 passed, 0 failed** の周)の実測:

```
[load] abort test/lisp/toplevel_abort_test.lisp: tl inside with-environment
[load] abort test/lisp/toplevel_abort_test.lisp: tl plain
[load] abort test/lisp/toplevel_abort_test.lisp: tl after switch
[load] file test/lisp/toplevel_abort_test.lisp: aborted 3
[load] abort test/lisp/handler_exit_test.lisp: he7e
[load] file test/lisp/handler_exit_test.lisp: aborted 1
[load] abort test/lisp/arith_signal_test.lisp: arithmetic error: DIV (1 0)
[load] file test/lisp/arith_signal_test.lisp: aborted 1
[load] abort test/lisp/isiki_test.lisp: <reason unavailable>
[load] abort test/lisp/isiki_test.lisp: <reason unavailable>
[load] file test/lisp/isiki_test.lisp: aborted 2
[load] aborted top-level forms: 7
```

§2-2 には「0 でなければ**いま現に静かに欠けている**ということである」と書いた。
**そこも外れた。** 中身を 1 件ずつ追うと、7 件すべてが
**打ち切りの仕組みそのものの回帰試験**で、わざと最上位で打ち切っている:

| ファイル | 件数 | なぜわざと打ち切るのか |
|---|---|---|
| `test/lisp/toplevel_abort_test.lisp` | 3 | 最上位への中断そのものの試験。`with-environment` の中 / 素 / `switch-environment` のあとの 3 通り(`:36` `:42` `:66`) |
| `test/lisp/handler_exit_test.lisp` | 1 | ハンドラからの脱出。捕まえずに最上位まで上げる 1 件 |
| `test/lisp/arith_signal_test.lisp` | 1 | 算術のエラー(`DIV (1 0)`)を最上位まで上げる |
| `test/lisp/isiki_test.lisp` | 2 | `%abort-top-level` が AOT 化されても伝播することの回帰試験(#27 / M12 Phase7。`:1447-1448`) |

**だから判定を「総数が 0」から「名簿と突き合わせる」へ変えた。**
最初の形(0 でなければ落ちる)は陰性対照で鳴ったので採れない。
既存の作法(`check_eval_error_budget.py` の予算、`check_test_count_ratchet.py` の名簿)に
そろえて、**名簿は 1 箇所・両方向・次に書く数字を出す**形にした。

> **この外し方には意味がある。** 「打ち切りは起きていないはずだ」という思い込みの
> ままで「非 0 なら落ちる」を入れていたら、**1 周が毎回赤になる検出器**ができ、
> すぐに外されていた。陰性対照を先に取ったから形が変わった(規則 8)。

### 3-5. 陽性対照が実際に落ちた出力(3 つ + α)

**(1) 打ち切りが起きる台本で母艦側が落ちる**(実機 1 回。合否の一行は**通っている**)

```
==== isiki tests: 1 passed, 0 failed ====
OK: 合否の一行は行 3(成果物の最後の非空行)。通ったのは 1 件、落ちたのは 0 件
NG: 読み込みの打ち切りが名簿と合っていない(1 件)
  - 名簿に無いファイルで打ち切りがある: test/lisp/qemu_boot_load_abort_positive.lisp が 6 件。
    **このファイルは半分だけ静かに欠けている。**
NG: test/lisp/qemu_boot_load_abort_positive.lisp は合否の一行は通ったが、**読み込みの打ち切り**で落ちました。
    結果は出ているので flake ではありません。**再試行しません**
```

**ここが #197 の核心である**: 合否の一行(`1 passed, 0 failed`)は通っている。
**合否の一行だけを見ていたら、6 つの式が消えたことに気づけない。**

**(2) `catch` の無い `throw` でも落ちる**(§C-2 の穴)

上の実機の出力の 3 行目がそれである:

```
[load] abort test/lisp/qemu_boot_load_abort_positive.lisp: uncaught catch-exit (throw with no matching catch)
```

直す前はこの 1 行が**どこにも出なかった**(件数も増えず、画面にも出ない)。
単体試験の `直す前=0 / 直した後=1`(§3-1 の E 行)がその対照である。

**(3) 行が出ないときに落ちる**

```
OK(陽性対照5・行が出ない(ログはあるが行が無い)): rc=1(期待 1)
      | NG: 打ち切り件数の行が 1 行も無い(.../serial.txt)。**出ないことを成功と読んではいけない**
OK(陽性対照6・ログそのものが無い): rc=1(期待 1)
      | NG: 直列線のログ .../serial.txt が無い。**打ち切りの行を誰も見ていない状態である**
```

段の配線(`tools/run_qemu_milestone.sh`)の側にも同じ対照がある:

```
OK(陽性対照6・打ち切りの行が出ていない -> 落ちる): rc=1 flake=0 件 (期待 rc=1 flake=0)
```

**(4) 陰性対照: 通常の 1 周で誤って鳴らない**

実機で取った 1 周のログ(§3-4)を、名簿を入れたあとの判定にかけ直した:

```
OK: 読み込みの打ち切りは 7 件(行 19)。**どれも名簿どおり**(名簿の合計は 7 件 / 4 ファイル)
    test/lisp/arith_signal_test.lisp: 1 件(わざと: 算術のエラー(DIV (1 0))を最上位まで上げる 1 件)
    ...
```

**(5) 名簿が両方向で落ちる**(規則 16)

```
OK(陽性対照2・名簿のファイルの件数が**増えた**(新しく欠けた)): rc=1(期待 1)
OK(陽性対照3・名簿のファイルの件数が**減った**(わざとの打ち切りが効かなくなった。規則 16)): rc=1(期待 1)
OK(陽性対照8・どのファイルかの行だけが消えた(総数と合わない)): rc=1(期待 1)
OK(陽性対照9・総数だけを 0 に書き換えた): rc=1(期待 1)
```

### 3-6. 走らせていないもの(正直に)

- **`make test-qemu-all` は 1 度も回していない**(1 周 12 分 / 直列。依頼で禁じられている)。
  したがって `test-qemu` と `test-qemu-load-abort-positive` 以外の段で
  打ち切りが出るかは**測っていない**。出れば名簿に無いファイルとして落ちるので、
  **1 周を回したときに名簿を足す作業が残る**(落ちたら `--list` が次に書く行を出す)
- **`make test-qemu-no-9p` も回していない。** 配線(`--verify` の 1 行)は入れたが、
  あの段の打ち切りが 0 件であることは**読みである**(`(load "/app/hello.lisp")` の
  直後に `(isiki-hello)` が 42 を返す判定が通っているので、読めて評価されている)
- `make test-qemu` 側の `--verify` の配線は**実際に走らせて落ちるところまで見た**
  (§3-4 の 1 周は、名簿を入れる前の判定で落ちた)

---

## 4. 出る行の形

直列線へ出るのは 3 種類で、**どれも合否の一行(`N passed, M failed`)には似せていない**
(`tools/check_pass_line.py` の在庫に引っかからないようにするため)。

| 行 | 出どころ | 何を答えるか |
|---|---|---|
| `[load] abort <名前>: <理由>` | 打ち切り 1 件ごと(`os_load_from_stream`) | **なぜ**欠けたか |
| `[load] file <パス>: aborted <件数>` | ファイル 1 本ごと(`cc_load`。件数が 1 以上のときだけ) | **どのファイル**が欠けたか |
| `[load] aborted top-level forms: <総数>` | 1 周の終わりに 1 回(`kernel.c`)。**0 件でも出す** | 母艦の判定に使う |

母艦側の判定は `tools/check_load_abort_line.py --verify <直列線のログ>` 1 箇所。
判定は**名簿との突き合わせ**である(§3-4 でそう変えた)。名簿は
`tools/check_load_abort_line.py` の `EXPECTED` 1 箇所で、`--roster` が中身を、
`--list <ログ>` が次に書く行を出す。

`[load] abort` の理由は、最上位へ逃げた値が条件のインスタンスでない場合に
`<reason unavailable>` になる(§3-3 の A)。**件数と「どのファイルか」は出る。**

---

## 5. 隣の検出器との境界(3 つは別のものを見ている)

| 検出器 | 見るもの | これが捕まえる事故 |
|---|---|---|
| `check_test_count_ratchet.py`(#204) | 段ごとの**走ったアサーションの数** | 段がまるごと消えた / 静かに縮んだ |
| `check_pass_line.py`(#205) | **合否の一行そのもの**(偽造) | 一行を直接書く / 集計を戻す |
| `check_load_abort_line.py`(#197。本件) | **読み込まれなかった最上位の式の数** | ファイルが半分だけ静かに欠ける |

**両方(三方)必要である。** ファイルが丸ごと落ちれば #204 が捕まえ、
半分欠ければ本件が捕まえる。
