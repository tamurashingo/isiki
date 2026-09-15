# 修正済み: frame の Function Cell が GC で更新されない

> 確認日: 2026-09-15 / 確認ブランチ: `feature/frame-separation` および `main`
> **修正済み(2026-09-15、`feature/frame-separation` / PR #59)。**
> 以下は症状・機序・再現手順を残すための記録で、「症状」「機序」の各節は
> **修正前**の状態を記述している。修正の内容は末尾の「修正」節を参照。

## 症状(修正前)

JIT コンパイルされた関数が、`flet` / `labels` が束縛した名前を呼ぶ場合、
**GC が 1 回走った時点で呼び出しが壊れる**(`EVAL-ERROR` を返すようになる)。

```lisp
(flet ((h () 'alive))
  (defun f () (h))
  (f)          ; → ALIVE
  ;; ここで GC が 1 回走ると
  (f))         ; → EVAL-ERROR
```

## 機序(修正前)

> この節の行番号は**修正前**のもので、現在のツリーとは対応しない。

1. `os_set_function` は `functions` スロットへの登録と同時に、Immobilized Space 上へ
   **Function Cell** を確保して `cells` スロットへ登録する(`runtime.c:3509` 付近)
2. JIT 生成コードは呼び出し先の **cell のアドレス**を 1 度だけ解決してコードに
   キャッシュし、以後は毎回 cell を deref して現在の関数オブジェクトを得る
   (`za_emit_fn_resolve_cached`)
3. cell の中身は GC ヒープ上の関数オブジェクトへのタグ付きポインタだが、
   cell 自体は `TAG_RAW_POINTER` で `cells` スロットに入っているため、
   **通常の GC スキャンは cell の中身を追わない**
   (`os_tag_is_heap_ref` が `TAG_RAW_POINTER` に 0 を返す、`runtime.h:37-42`)
4. そのための後始末が `gc_fixup_environment_cells`(`runtime.c:1859`)だが、
   **`global_environment`(`runtime.c:2011`)と各 `proc->env`(`runtime.c:2013`)に
   対してしか呼ばれない**
5. `FLET-ENV` / `LABELS-ENV` はどちらにも該当しないので、その cell は更新されない。
   GC 後、cell は旧 From 空間のアドレスを指したままになる

GC 後の旧オブジェクトの word0 は転送ポインタ(`TAG_FORWARD`)で上書きされているため、
呼び出し側の magic 判定(`MAGIC_FUNCTION_NATIVE` との比較)が外れてフォールバック経路
へ落ち、`os_apply_function` が「関数ではない」と判断して `g_sym_eval_error` を返す。
クラッシュではなく決定的な `EVAL-ERROR` になるのはこのため。

## 影響範囲(修正前。実測で切り分け済み)

| ケース | GC前 | GC後 | |
|---|---|---|---|
| A: `flet` 束縛を **JIT** から呼ぶ | ALIVE | **EVAL-ERROR** | **壊れる** |
| A2: 同じ関数を `flet` の外から呼ぶ | — | **EVAL-ERROR** | **壊れる** |
| B: `flet` 束縛を **インタプリタ** から呼ぶ | ALIVE | ALIVE | 無事 |
| C: `let` frame 内で defun、呼び出し先はグローバル | ALIVE | ALIVE | 無事 |
| D: 完全にグローバル | ALIVE | ALIVE | 無事 |
| E: `labels` 束縛を JIT から呼ぶ | ALIVE | **EVAL-ERROR** | **壊れる** |

- **JIT の cell 経路に限る。** インタプリタは `os_get_function` で `functions` の
  alist を引くだけで、こちらは通常の GC スキャンの対象なので無事(B)
- **frame に関数束縛がある場合に限る。** `let`/`CALL-ENV` は関数束縛を持たないので、
  呼び出し先の解決は親の environment へ行き、そちらの cells は fixup される(C)
- `CALL-ENV` に `os_set_function` が来る経路は frame 分離後は存在しない
- AOT の捕捉環境(`__lisp_lambda_N`)は変数束縛しか持たないので該当しない

つまり危ないのは **`FLET-ENV` / `LABELS-ENV` だけ**。

## いつからあるか

**`main`(frame 分離前)でも同一の結果**になることを確認済み。**既存の穴であり、
frame 分離が作ったものではない。**

ただし frame 分離によって**到達可能性が大きく広がった**:

- 修正前: `(flet ((h () 1)) (defun f () (h)))` の `f` は `FLET-ENV` にしか
  登録されず、`flet` の外から呼べなかった。壊れた状態に到達するには
  `flet` の本体内で GC を挟む必要があった
- 修正後: `f` はグローバルへ登録され、**プログラムの残り全期間にわたって
  呼べる**。一方 word3 は `FLET-ENV` frame のままなので、cell は stale になる

## 再現手順(修正前の挙動を再現する場合)

```bash
git checkout feature/frame-separation   # main でも同じ
# 下記2ファイルを置く
make test-qemu-milestone MILESTONE=test/lisp/tmp_boot_gc_cell.lisp QEMU_MEM=96M
cat test-results.txt
```

`QEMU_MEM=96M` にするのは Lisp ヒープを小さくして GC を早く起こすため
(96M で heap≈6.3MB。既定の 256M だと cons の回数が桁違いに増える)。

### `test/lisp/tmp_gc_cell.lisp`

```lisp
;; GC を1回起こす: gc-collect-count が増えるまで cons し続ける
(defun force-gc ()
  (let ((c (%%gc-collect-count)))
    (while (= (%%gc-collect-count) c) (cons 1 2))
    (%%gc-collect-count)))
(defun say (label v) (progn (format *isiki-test-stream* "~A = ~S~%" label v)
                            (finish-output *isiki-test-stream*)))

(flet ((h () 'alive))
  (defun f () (h))
  (say "#A before" (f))
  (force-gc)
  (say "#A after " (f)))
(say "#A2 fletの外から" (f))
```

### `test/lisp/tmp_boot_gc_cell.lisp`

```lisp
(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")
(load "test/lisp/tmp_gc_cell.lisp")
(isiki-test-report)
(close *isiki-test-stream*)
```

### 実際の出力

```
#G heap=6295200 gc0=0
#G before      = ALIVE (compiled=T)
#G gc1 count=1
#G after gc1   = EVAL-ERROR
#G gc2 count=2
#G after gc2   = EVAL-ERROR
#G after gc3   = EVAL-ERROR
```

## 検討した修正案

1. **Function Cell を GC ルートとして登録する。** cell は Immobilized Space 上の
   固定アドレスの `lisp_val_t` なので `os_gc_register_root` の形にそのまま合う。
   **不採用**: `GC_MAX_EXTRA_ROOTS` は 2048 で、`os_gc_register_root` は冪等性のため
   線形探索している(`runtime.c:1301` 付近)。cell は (sym, env) ごとに作られ
   O(プログラムサイズ)で増えるので、この表では足りない
2. **cell を1本のグローバルなリストにも繋ぎ、GC がそれを辿って fixup する。**
   **採用。** 下記「修正」を参照
3. **frame では cell を作らない**(実装計画の Step 5)。
   **採れない。** JIT が frame の cells を実際に読む経路が存在することが
   実証されている(`documents/investigation-frame-separation.md` の 5-0、
   および上表の A/E)。cell を作らないと `os_get_function_cell` が `nil` を返し、
   呼び出しそのものが壊れる

## 修正

案 2 を、**cell スロットの未使用部分を next ポインタに使う**形で実装した。

Function Cell は `os_imm_slot_alloc(&g_function_cell_cursor, 2 * sizeof(lisp_val_t))`
で **16 byte** 確保されているが、実際に使っているのは先頭 8 byte だけだった
(16 byte アラインメントを保つための確保)。この**後半 8 byte を next ポインタに使う**
ので、追加のメモリは一切消費しない。

| | 場所 |
|---|---|
| リスト先頭 `g_function_cell_list_head` | `runtime.c:1138` |
| cell 総数 `g_function_cell_count` | `runtime.c:1144` |
| 走査 `gc_fixup_all_function_cells` | `runtime.c:1887` |
| GC からの呼び出し | `runtime.c:2045`(`gc_scan_queue` の後、再度 `gc_scan_queue`) |
| リストへの連結 | `runtime.c:3432`(`os_set_function` 内) |
| テスト用リセット | `runtime.c:2673` |
| 診断用 `%%DIAG-FN-CELL-COUNT` | `runtime.c:1151` |

`gc_fixup_environment_cells`(環境の `cells` スロットを辿る版)は廃止した。
全 cell を1本のリストで辿るので、cell がどの環境に属するか
(`global_environment` か `proc->env` か `FLET-ENV` か)に関わらず漏れなく再配置される。

### 実装上の注意(コード中のコメントにも記載)

- **`g_function_cell_list_head` を `os_gc_register_root` へ渡してはならない。**
  これは GC ヒープ上の値ではなく Immobilized Space 上の生ポインタで、
  `gc_copy_value` の対象ではない
- **`next` は必ず明示的に書く。** Immobilized Space のページはフリーリストから
  再利用されることがあり、ゼロ初期化は保証されない
- **値を書いた直後、他の確保を一切挟まずにリストへ繋ぐ。** 間に確保を挟むと
  そこで GC が走り、リストに載っていない cell を取りこぼす
- **強参照である**(`gc_copy_value` を無条件に呼ぶ)。cell は一度作られたら
  解放されないので、cell が参照する関数オブジェクトも解放されない。
  弱参照にするには cell 自体の回収機構が要るが、現状は持っていない

### GC 時間の増分(実測)

`QEMU_MEM=96M`(heap≈6.3MB)で 2,000,000 回 cons して GC を 5 回起こし、
その所要 tick(10ms 単位)を測った。同じ手順を修正前(`git stash`)と修正後で交互に流した。

| | n | 平均 tick | ばらつき |
|---|---|---|---|
| 修正前 | 4 | 405.0 | 392〜428(sd 17.1) |
| 修正後 | 5 | 425.6 | 408〜460(sd 21.3) |

差 +20.6 tick(+206ms、+5.1%)。ただし **95% 信頼区間は -10 〜 +52 tick で 0 を含み、
統計的に有意ではない**(t = 1.57)。同一ビルドの中でも 408〜460 と 50 tick 以上ばらつく。

**機序の上でも増分はほぼゼロのはず。** 同時に測った cell 総数は **712** で、
これは修正前に走査していた `global_environment` の cells の件数(**712**)と同じだった。
つまり走査件数は実質変わっていない(frame 由来の cell はこのベンチではほぼ発生しない)。
仮に 712 件 × 5 GC = 3,560 回のポインタ参照が 206ms かかるとすれば 1 件 58µs で、
これはあり得ない。**観測された差はノイズと判断する。**

なお、frame を多用するプログラムでは cell 総数が global の件数を上回りうるが、
走査コストは cell 総数に比例するだけで、プログラムサイズに対して線形である。

### 回帰テスト

`test/lisp/frame_cell_gc_test.lisp`。上表の A / A2 / B / C / D / E の全ケースに加え、
GC を跨いだ再定義と cell 多数のケースを含む。GC は `fcg-force-gc`
(`%%gc-collect-count` が増えるまで cons する)で強制的に起こす。
`qemu_boot_test.lisp` と `qemu_boot_m2_za.lisp` に登録済み。
