# 調査報告: flet / labels の Function Cell リーク

> 調査日: 2026-09-15 / ブランチ: `feature/compiler-optimization`(`6da82cb`、main 取り込み後)
> **調査のみ。コードは変更していない。**

## サマリ

**案A(遅延生成)を推す。** `os_get_function_cell` の呼び出し元は**JIT 生成コードだけ**で
(確認)、インタプリタは cells を一切見ない。したがって遅延生成にすれば
インタプリタ経路の 16 byte/回 は**ゼロになる**。1:1 前提に依存するコードも無い(確認)。
案B(スイープ)は**危険**。JIT が cell のアドレスをキャッシュしており、フリーリスト
再利用で別 cell に化ける。案C は案A より効果が小さく、コールサイトを太らせる。

**ただし急がなくてよい。** 枯渇は `defun` 側が先に、しかも **260 倍速く**起こる(実測)。

---

## 調査項目 A: cell はいつ、なぜ作られるのか

### 確認したこと

**A-1. 新規確保と書き換えの条件**(`src/c/runtime.c:3558` `os_set_function`)

cells スロットの alist を `cc_assoc_eq(sym, cells_alist)` で引き、

- **見つかれば**(`runtime.c:3600-3603`)セルのアドレスはそのままに中身だけ書き換える
- **見つからなければ**(`runtime.c:3605-3625`)`os_imm_slot_alloc` で 16 byte 確保し、
  next リストへ繋いで cells alist へ push する

判定キーは **`sym` と、その `env` の cells スロット**。`flet` は呼び出しのたびに
新しい `FLET-ENV` を作る(`src/c/eval.c:460` `os_make_frame(os_make_symbol("FLET-ENV"), env)`)
ので、その cells は毎回空。**毎回「見つからない」側に落ちる。**

**A-2. 誰のために作っているのか → JIT 生成コードだけ**

`os_get_function_cell` の呼び出し元を全数列挙した:

| 呼び出し元 | 場所 |
|---|---|
| JIT 生成コード(キャッシュ有) | `src/c/za.c:3643` |
| JIT 生成コード(キャッシュ無) | `src/c/za.c:3678` |
| JIT 生成コード(一般呼び出し) | `src/c/za.c:3817` |
| C のユニットテスト | `test/c/runtime_test.c:380, 391, 2013` |

**それ以外に呼び出し元は無い。** 具体的に:

- **インタプリタは呼ばない。** `eval.c` に出現しない
- **Lisp の組み込み関数として公開されていない。** `%%...FUNCTION-CELL` 相当の
  primitive は存在しない(`grep -rn 'FUNCTION-CELL' src/c/ src/lisp/` が空)
- **AOT も呼ばない。** `src/lisp/transpile.lisp` / `src/c/lisp_compiled.c` に出現しない

**A-3. 1:1 前提に依存するコードは無い**

cells スロットを**読む**のは `os_get_function_cell`(`runtime.c:3650`)の1箇所だけ。
他は生成時(`runtime.c:3038-3046`)のみ。PR #59 で `gc_fixup_environment_cells` が
廃止され、GC は cells ではなく next リストを辿るようになったため、**cells を走査する
コードは GC からも消えた。**

しかも `os_get_function_cell` は cell が無くても壊れない:

```c
lisp_val_t cell_pair = cc_assoc_eq(sym, cells_alist);
return cc_cdr(cell_pair);            // runtime.c:3650-3651
```

`cc_assoc_eq` が `nil` を返した場合、`nil` は自己参照 cons(`runtime.c:2250-2257` で
car も cdr も自分自身)なので `cc_cdr(nil)` は `nil`。**つまり「functions にはあるが
cells に無い」状態でも `nil` を返して正常に終わる。** 案A の前提が成立する。

**A-4. `g_function_cell_cursor` は cell 専用**(全6箇所)

| 場所 | 用途 |
|---|---|
| `runtime.c:1139` | 定義 |
| `runtime.c:1392-1393` | 使用量の計算(`os_imm_space_used_bytes`) |
| `runtime.c:1422` | panic 時の内訳表示 |
| `runtime.c:2831-2832` | テスト用リセット |
| `runtime.c:3609` | **唯一の確保箇所** |

### 推測・未確認

- `os_set_function` が常に cell を作る設計にした理由は、`os_get_function_cell` の
  doc コメント「functions で見つかったレベルの cells には対応する cell が必ず存在する」
  (`runtime.c:3634-3635`)という不変条件を置きたかったためと**推測**する。
  コミット履歴は追っていない

---

## 調査項目 B: 呼び出し元と frame cell の使用実態

### 確認したこと

**B-1/B-2.** 上記 A-2 のとおり JIT 生成コードのみ。frame に束縛された名前に対して
呼ばれうるのは、**`word3`(定義時 env)が frame である JIT 関数のコールサイト**。

**B-3. `s5-f3` のケースは実在する(実測)**

```
#S5 s5-f3 compiled=T 値=FLET-LOCAL cell増=2
```

`(flet ((s5-shadow () 'flet-local)) (defun s5-f3 () (s5-shadow)))` の `s5-f3` は
JIT コンパイルされ、`FLET-LOCAL` を返した。**JIT が frame の cells を引いている。**

成立条件は:

- `flet` の本体に `defun` が含まれると、**`flet` 自体は JIT されない**
  (`za_is_excluded_special_form`、`src/c/za.c:1705-1713` が `defun` を除外する)。
  インタプリタが `eval_flet` で `FLET-ENV` frame を作る
- その中の `(defun s5-f3 ...)` をインタプリタが評価し、`za_try_compile_defun` が
  `s5-f3` の**本体だけ**をコンパイルする。capture_env は `FLET-ENV`
- 逆に、`flet` が JIT された場合は束縛関数を gensym で
  **`global_environment` へ登録する**(`src/c/za.c:5418-5419`
  「関数namespaceはgensym+global_environment経由なので、このenvには一切登録しない」)。
  frame の cells は使われない

**B-4. 解決は「コンパイル済みコードブロックのコールサイトごとに1回」**

JIT は解決結果を `g_za_fn_cell_cache_slots`(`za.c:1078`、2048 枠)のスロットへ入れ、
生成コードには**スロットのアドレス**を `movabs r14, <slot>` で焼き込む
(逆アセンブルで確認: `movabs r14, 0xdc77ac0` → `mov rax, [r14]`)。
初回実行時だけ `os_make_symbol` + `os_get_function_cell` を通り、以後はスロットを読むだけ。

**`defun` を実行し直すとコードが再生成され、新しいスロットが割り当てられる**ので、
その場合は実質「`defun` 実行ごとに1回」になる。

### 実測: 経路別の Immobilized Space 消費

インタプリタ経路の強制には「固定引数への `setq`」を使った(JIT が断念する条件)。
`compiled=` の値で経路を確認済み。

| 経路 | compiled | 2,000 回の消費 | 1回あたり | cell 増 |
|---|---|---|---|---|
| インタプリタ `flet` 束縛1個 | NIL | 32,000 byte | **16 byte** | 2,000 |
| インタプリタ `flet` 束縛2個 | NIL | 64,000 byte | **32 byte** | 4,000 |
| インタプリタ `labels` 束縛1個 | NIL | 32,000 byte | **16 byte** | 2,000 |
| **JIT 内 `flet` 束縛1個** | **T** | **16 byte(合計)** | **≒0** | **1** |
| `defun`(グローバル) | — | 5回で 20,800 byte | **4,160 byte** | 0 |

依頼書の数値(16 / 32 / 0 byte)を再現。`defun` は依頼書の 4,128 に対し実測 **4,160 byte**。

### 推測・未確認

- 「どれくらい起きやすいか」は**推測**になるが、`flet` の中で `defun` を書く必要が
  あるため、通常の Lisp コードではまず現れない。ただし frame 分離(PR #59)で
  `s5-f3` が**グローバルに登録され、プログラムの残り全期間呼べる**ようになったため、
  一度作られた frame cell は最後まで参照されうる

---

## 調査項目 C: 3つの案の実現可能性

### 案A: cell を遅延生成する — **実現できる**

**C-1. `os_imm_slot_alloc` / `os_imm_page_alloc` は GC を誘発しない(確認)**

`runtime.c:1428-1439` / `runtime.c:1172-1184`。バンプとフリーリストの操作だけで、
`os_gc_collect` を呼ぶ経路は無い。枯渇時は `os_panic`。

**ただし cells alist への追加には `os_make_cons` が 2 回要り、そちらは GC を誘発する。**
現在の `os_get_function_cell`(`runtime.c:3639`)には `GC_PROTECT` が1つも無いので、
`sym` / `env` の保護を足す必要がある。シンボルは GC ヒープ上にある
(`os_make_symbol` が `os_alloc_bytes(32)`、`runtime.c:2698`)ので移動する。

呼び出し元(JIT 生成コード)側は、この時点で引数を GC リンク済み
(逆アセンブルで確認: 引数の `za_gc_link` は `os_get_function_cell` 呼び出しより前)。

**C-2. 1:1 前提への依存は無い**(A-3 で確認済み)

**C-3. 未定義名では従来どおり `nil` が返る**

遅延生成でも「functions に無い」名前は cell を作れないので `nil` を返す。
JIT 側は `nil` をキャッシュしない(PR #59 Step 1、`za.c:3646-3665`)ので整合する。
実測でも未定義呼び出しは `EVAL-ERROR` を返し、クラッシュしない:

```
#S5 未定義呼び出し compiled=T 値=EVAL-ERROR
```

**C-4. GC の走査軽減は測れる。** `%%DIAG-FN-CELL-COUNT` がリスト長(=走査件数)を
返すので、同じワークロードの前後で比較できる。ただし PR #59 の測定では
712 件 × 5 GC の走査は GC 時間に対して**測定限界以下**だったので、
**減らしても有意差は出ない見込み**(推測)。

### 案B: GC で死んだ cell を回収する — **危険。推さない**

**B-4(依頼書の最大の懸念)が現実の危険であることを確認した。**

JIT は解決した **Function Cell のアドレスをキャッシュに保持する**。`za.c:1064-1070`:

> 格納する値(Function Cellのアドレス)は `os_imm_slot_alloc` … で確保された
> 非移動領域を指すため、GCによる再配置を追跡する必要がなく、
> `os_gc_register_root` は呼ばない。

つまり **cell のアドレスが「永久に有効」であることを前提にした設計**になっている。
cell を解放してページをフリーリストへ返し、別の cell として再利用すると、
キャッシュ済みのアドレスが**別の関数の cell を指す**。呼び出し先が黙って入れ替わる。

**緩和の目処はある**(推測): 焼き込まれているのは**スロットのアドレス**であって
cell のアドレスではなく、スロットは `g_za_fn_cell_cache_slots`(2048 個の配列)に
集約されている。回収時にこの配列を走査して該当アドレスを 0 に戻せば再解決が走る。
ただし 2048 件の線形走査が回収のたびに要る。

**C-1(死亡判定)にも根本的な障害がある。** cell には所有者環境への逆参照が無い
(16 byte = 値 8 + next 8 で埋まっている)。そして**生きている環境を列挙する手段が無い**:
`*environments*` は `os_make_environment` で作られたものだけを載せる
(`runtime.c:3072-3083`)ので、`flet` の frame は載らない。
これは PR #59 で next リストを導入した理由そのものである。

**C-3(cell を 24 byte に広げて逆参照を持つ)** は、16 byte → 32 byte アラインで
**リーク速度が倍**になる。回収が効くまでの間は悪化する。二相マークスイープの
実装コストと合わせて、案A に対して割に合わないと**考える(所見)**。

### 案C: frame では cell を作らず JIT に nil フォールバック — **実現できるが案A に劣る**

**C-2. 既存の遅いパスは再利用できるが、シンボルを渡す必要がある**

JIT には既に cell が `nil` のときのフォールバック分岐がある(`za.c:3845-3850`)。
そこから `os_apply_via_cell` へ合流する。しかし `os_apply_via_cell`
(`runtime.c:3676-3682`)は `cell == nil` のとき

```c
return os_apply_function(nil, evaluated_args, env);
```

としており、**名前からの再探索をしない**(EVAL-ERROR になる)。
案C を成立させるには `os_apply_via_cell` に `sym` を渡して
`os_get_function` へ落とす経路を足す必要がある。

**C-1. 増分の見積もり(推測)**

- `os_apply_via_cell` は現在 rcx/rdx/r8 の 3 引数(逆アセンブルで確認、
  d1 の 0x453-0x480)。`sym` を r9 で渡すなら **+3〜11 byte**
- ただしキャッシュ高速パスではシンボルを再解決していないので、
  遅いパス側で埋め込み済みの名前文字列から `os_make_symbol` を呼び直す必要がある。
  `movabs rcx, <name>` + `call os_make_symbol` 相当で **+約 25 byte**
- 合計 **+30〜40 byte / コールサイト**(現在 762 byte の 4〜5%)と**見積もる**

コールサイトを太らせると `defun` 1回あたりの 4,160 byte が増え、
**先に効いてくる枯渇(後述 D)を悪化させる。**

---

## 調査項目 D: どれくらい急ぐ問題か

### 確認したこと

**D-1. カーネルの Lisp ソースは `flet` / `labels` を1箇所も使っていない**

```
grep -rn '(flet \|(labels ' src/lisp/*.lisp   → 0 件
```

テストでは 72 箇所使っている。イベントループ・スケジューラは C 側(`process.c` /
`repl.c`)にあり、`flet` を通らない。**リークはユーザプログラムからしか到達しない。**

**D-2. 枯渇時の表示(実測)**

`defun` を 5,000 回並べたファイルを読み込ませ、`-serial file:` で捕捉した:

```
PANIC: immobilized space exhausted
  immobilized space: used=16773488 / total=16777216 byte
  cursor fn_meta: offset=4080, function_cell: offset=384
  last request=48 byte
```

- **シリアルにも出る。** `panic_write_string`(`runtime.c:342-347`)が
  framebuffer と `os_diag_serial_write` の両方へ書く
- **`last request=48 byte` は Function Cell(16 byte)ではなく `za_fn_meta_t`。**
  つまり枯渇を最初に踏んだのは JIT の meta 確保だった
- パニック後は `g_panic_hook` により QEMU が電源断した(テスト実行時の登録による)
- 内訳表示(`panic_write_imm_breakdown`、`runtime.c:1413-1427`)が
  fn_meta と function_cell のカーソル位置を出すので、**どちらが犯人か判別できる**

**D-3. `defun` 側が圧倒的に先に効く**

| 経路 | 1回あたり | 16MB を使い切るまで |
|---|---|---|
| `defun`(グローバル) | 4,160 byte | **約 3,900 回** |
| インタプリタ `flet` 束縛1個 | 16 byte | 約 1,048,576 回 |

実測でも `i=3500` まで進んだところで枯渇した(開始時 544,672 byte 使用済み。
`(16,777,216 - 544,672) / 4,160 = 3,901`)。**約 260 倍の差。**

なお `os_imm_pages_alloc_contiguous`(JIT のコード領域、`runtime.c:1403-1411`)は
枯渇時に**0 を返すだけでパニックしない**。パニックするのは
`os_imm_page_alloc`(`runtime.c:1172-1184`)を経由する slot 確保側。

---

## 案の比較

| | 案A 遅延生成 | 案B GC で回収 | 案C frame で作らず nil |
|---|---|---|---|
| 実現可否 | **できる** | **危険** | できる |
| 変更範囲 | `os_set_function` / `os_get_function_cell`(runtime.c のみ) | GC 二相化 + cell 構造変更 + JIT キャッシュ無効化 | `os_get_function_cell` + `os_apply_via_cell` + za.c の発行 |
| インタプリタ `flet` のリーク | **ゼロ** | 回収されるまで発生 | **ゼロ** |
| JIT から frame cell を引く経路 | 従来どおり動く | 従来どおり動く | **遅いパスへ落ちる**(動くが遅い) |
| コールサイトのサイズ | 変化なし | 変化なし | **+30〜40 byte(4〜5%)** |
| 主な障害 | `os_get_function_cell` に GC_PROTECT が要る | **JIT がキャッシュした cell アドレスがフリーリスト再利用で別 cell を指す**。生きた環境を列挙する手段が無い | 遅いパスに `sym` を渡す必要がある |
| 残るリーク | **無し**(JIT が引いた分だけ作られる) | 回収周期分 | **無し** |

---

## 所見

**(所見)案A を推す。** 理由は3つ。

1. **cells の読み手が JIT だけだと確定した。** インタプリタも AOT も Lisp 組み込みも
   読まない。作る側だけが常に作っているという、素直に不要な仕事になっている
2. **1:1 を壊しても既存コードが壊れない。** cells を読むのは1箇所で、
   そこは `nil` を安全に返す。PR #59 で GC が cells 走査をやめたことが効いている
3. **コールサイトを太らせない。** 案C の +30〜40 byte は、
   より速い枯渇要因(`defun` 4,160 byte)を直接悪化させる

**(所見)PR #59 の next リストは、案B を可能にはしなかった。** 依頼書は
「スイープが現実的になったか」を問うているが、リストが解いたのは
「全 cell を**列挙**できない」問題であって、「どの cell が**生きているか**分からない」
問題は解いていない。後者には生きた環境の列挙が要り、frame は `*environments*` に
載らないので依然として手段が無い。

**(所見)急ぐ問題ではない。** カーネルは `flet` を使わず、`defun` 側が 260 倍速く
枯渇する。**先に手を入れるべきは `defun` 1回 4,160 byte のほう**だと考える。
約 3,900 回で停止するのは、対話的に使っていれば現実的に踏みうる数字である。

**(所見)パニックの診断表示は良くできている。** `last request` と
2本のカーソル位置が出るので、どの確保が犯人か即座に分かった。
`documents/pitfalls.md` 原則6 が効いている例だと思う。

---

## 未調査・積み残し

- **`defun` 1回 4,160 byte の内訳**は取っていない。`za_fn_meta_t`(48 byte)+
  コードページ(4,096 byte)= 4,144 で概ね説明できそうだが、残り 16 byte は未確認
- **案B の緩和案**(`g_za_fn_cell_cache_slots` 2048 件の走査で無効化)は
  机上のみ。実際に安全かは検証していない
- **案C の増分見積もり +30〜40 byte は推測。** 実装して測ってはいない
- **`os_get_function_cell` に `GC_PROTECT` を足したときの影響範囲**は未確認。
  JIT 生成コードから呼ばれる関数でシャドースタックを触ることになるため、
  呼び出し規約(MS x64 ABI)との相性を別途見る必要がある
- 環境が `destroy-environment` される際に cells がどう扱われるかは追っていない
  (`destroy_environment` 相当のシンボルが `runtime.c` に見つからなかった)
