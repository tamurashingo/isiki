# declaim の設計(Phase 2: optimize のみ)

> 作成日: 2026-09-15 / ブランチ: `feature/declaim`(分岐元 `feature/compiler-optimization` `4bd9296`)
>
> **CommonLisp の `declaim` とは意味論が異なる。** 本実装は **environment 単位**で作用し、
> 親からは引き継がない。

## 調査結果

### 4-1. environment の構造

**environment は C の構造体ではなく、スロットの cons リストである。**
`os_make_environment_tagged`(`src/c/runtime.c:3166` 付近)が組み立てる。

| # | スロット | 内容 |
|---|---|---|
| 1 | `name` / `FRAME` | car が `NAME` なら environment、`FRAME` なら frame。cdr が名前 |
| 2 | `variables` | 変数束縛の alist |
| 3 | `functions` | 関数束縛の alist |
| 4 | `parent` | 親環境 |
| 5 | `constants` | defconstant の名前リスト |
| 6 | `cells` | Function Cell の alist |
| 7 | `pages` | 所有する Immobilized Page |
| 8 | `literal-slots` | 所有するリテラルスロット |
| 9 | `code-cursor` | **environment のみ。** JIT コードのパッキング用カーソル(PR #61) |
| **10** | **`declaim`** | **本 Phase で追加。environment のみ** |

**スロット番号のハードコードは「位置による cdr チェーン」の形で存在する**
(1/4/6/7/8/9 番目)。いずれも先頭から数えるため、**末尾に 10 番目を足しても影響しない。**

**PR #59 の Function Cell 単方向リスト走査は、environment のスロット配置に依存しない
(確認済み)。** `gc_fixup_all_function_cells` は `g_function_cell_list_head` から
cell の生ポインタを辿るだけで、環境を一切参照しない:

```c
lisp_val_t *cell = g_function_cell_list_head;
while (cell != 0) {
    cell[0] = gc_copy_value(cell[0]);
    cell = (lisp_val_t *)(lisp_addr_t)cell[1];
}
```

environment と frame の判別は `os_definition_env`(`runtime.c:3754` 付近)が
1 番目のスロットの car と `g_sym_frame` を比較して行う。

### 4-2. フィールド追加の可否

**指示書 5-1 の「構造体に素の整数フィールドを追加する」は、そのままの形では採れない。**
environment は cons リストであって C 構造体ではないため、`typedef struct { ... int opt_speed; }`
に相当する場所が無い。

**採った形: 10 番目のスロットに fixnum 1 個。** `speed | (safety << 2) | (space << 4)`
(各 2 bit、値域 0〜3)を 1 つの fixnum へ詰める。指示書の意図(GC ルートを増やさない、
走査コストを乗せない)はこれで満たされる:

- **fixnum は即値**(`os_make_fixnum` は `magnitude << 3` を返すだけで確保しない)。
  Lisp オブジェクトではないので GC ルートが増えない
- **Cheney コピーで自動的に保たれる。** スロットの cons は通常の cons として複製され、
  中身の fixnum は `gc_copy_value` が素通しする(タグ 0 = FIXNUM、
  `os_tag_is_heap_ref` が 0 を返す)。**特別な対応は要らない**
- 値の更新は**スロットの cdr への書き込みだけ**で、確保が起きない。
  したがって `declaim` の評価中に GC が走らない

**frame には持たせない。** `os_make_environment_tagged` が `slot_tag != g_sym_frame`
のときだけ 10 番目を作る。`let` / `flet` / 関数呼び出しのたびに cons を 2 つ
(スロットとリストセル)増やさずに済む。9 番目の `code-cursor` と同じ扱い。

### 4-3. `za_try_compile_defun` のシグネチャ

変更前:

```c
lisp_val_t za_try_compile_defun(lisp_val_t params, lisp_val_t body,
                                lisp_val_t capture_env, lisp_val_t owner_env);
```

**`UINT64 optimize` を第 5 引数として足す。** 詰めた fixnum のマグニチュード
(`speed | safety<<2 | space<<4`)をそのまま渡す。定義は 2 箇所(x86_64 版と
非 x86_64 のスタブ)にあるので両方直す。

Phase 3 以降は `za_try_compile_defun` の中でこの値を見て生成コードを変えることになる。
本 Phase では**受け取って `za_fn_meta_t` へ記録するだけ**で、コード生成には使わない。

### `%%optimize-of` の置き場所

**`za_fn_meta_t` に 6 番目のフィールドとして足す。追加コストはゼロ。**
現在 5 フィールド = 40 byte だが `os_imm_slot_alloc` が 16 byte 境界へ丸めるため
実際には 48 byte 確保されており、**6 番目を足しても 48 byte のまま**である。

```c
typedef struct {
    UINT64 cons_entry;   /* offset 0  */
    UINT64 fixed_entry;  /* offset 8  */
    UINT64 arity;        /* offset 16 */
    UINT64 code_base;    /* offset 24 */
    UINT64 code_len;     /* offset 32 */
    UINT64 optimize;     /* offset 40 ← 本 Phase で追加 */
} za_fn_meta_t;
```

生成コードが直接 deref するのは offset 0 と 16 だけ(`za_ensure_trampoline`)なので、
末尾への追加は機械語に影響しない。

**制限: インタプリタ実行の関数では `%%optimize-of` は nil を返す。**
`MAGIC_FUNCTION_INTERPRETED` は word1/2/3 を params/body/env で使い切っており
meta を持たない(`make_interpreted_function`、`src/c/eval.c:80`)。
そもそもコンパイルされていないので optimize を適用する対象が無い。

## 仕様

| 項目 | 内容 |
|---|---|
| 保持するのは | **environment のみ。** frame は持たない |
| 参照先 | `defun` の登録先と同一、つまり `os_definition_env(env)` が返す environment |
| 親からの継承 | **しない。** `make-environment` はデフォルト値で初期化する |
| デフォルト値 | `speed` / `safety` / `space` すべて **1** |
| 効くタイミング | **`defun` の時点。** 定義済みの関数には影響しない |
| 指定しなかった項目 | **変更しない** |
| 値域 | 0〜3。範囲外はエラー |
| `optimize` 以外の指定子 | **エラーにせず無視する** |
| 戻り値 | `nil` |

`let` / `flet` / `labels` / 関数本体の中で `declaim` しても、frame は declaim を
持たないため読み飛ばされ、その frame を囲む environment に記録される。
`defun` 側も同じ `os_definition_env` を通るので、**「関数はそれが属する
environment の方針でコンパイルされる」という規則が両側で一致する。**

## プロセス間の挙動(6-6)

**F1〜F4 はそれぞれ独立した declaim を持つ。**

`repl.c:20` が `proc->env = os_make_process_environment(proc->name)` を呼び、
`os_make_process_environment`(`runtime.c:3301`)は `os_make_environment` を使う。
`os_make_environment` は 1 番目のスロットの car に `NAME` を入れる
(`g_sym_frame` ではない)ので、**プロセス環境は frame ではなく environment である。**

したがって:

- F1 で `(declaim (optimize (speed 3)))` しても **F2 には影響しない**
- どのプロセスの `declaim` も **`global_environment` には波及しない**
- 各プロセスは起動時にデフォルト値(1 1 1)から始まる

`global_environment` 自身の declaim を変えたい場合は、そこへ明示的に移動する必要がある
(プロセス環境は global の子だが、**親から引き継がない**仕様のため)。

**自動テストでは F1〜F4 のキーボード切り替えを再現できない**ので、
同じ性質を「global の子として作った 2 つの sibling environment が互いに影響しない」
という形で検証している(`test/lisp/declaim_test.lisp`)。プロセス環境も
同じ `os_make_environment` で作られるので、機構は同一である。

## 本 Phase のスコープ外

`declare` / `inline` / `notinline` / `type` / `ftype` / AOT での declaim /
再コンパイル手段。**optimize の値を記録して引き渡すところまで**で、
その値で生成コードを変えるのは Phase 3 以降。
**本 Phase 完了時点で `disassemble` の出力は変化しない。**

## 実装後に判明したこと

### `declaim` は JIT コンパイル対象外にする必要があった

`declaim` は environment の 10 番目のスロットを書き換える特殊形式で、za.c が
機械語を出せる形を持たない。`za_is_excluded_special_form`(`src/c/za.c:1705` 付近)へ
入れておかないと、**一般呼び出しとしてコンパイルされて実行時に `DECLAIM` という
関数が見つからず EVAL-ERROR になる。**

```lisp
(defun dcl-setter () (declaim (optimize (space 3))))
(dcl-setter)          ; 入れ忘れると EVAL-ERROR。declaim は効かない
```

`defun` / `defvar` / `defglobal` 等と同じ扱いで、**本体に `declaim` を含む関数は
インタプリタへ落ちる**(`%%za-compiled-p` が nil)。

### 組み込み関数の追加で `SMALL_HEAP_SIZE` の再調整が要った

`%%CURRENT-OPTIMIZE` と `%%OPTIMIZE-OF` の 2 つを `os_bootstrap` へ足し、
さらに全 environment へ 10 番目のスロットを追加したため、
`test/c/runtime_test.c` の `SMALL_HEAP_SIZE`(GC 発火テスト用の小さいヒープ)が
足りなくなり、**`make test` が gcd/isqrt テストで無限ループ**した。

実測した窓は **79〜80KB**。78KB 以下はハング、81KB 以上は isqrt テストで GC が
発火せず NG(`runtime_test.c:1855`)。中央を取って **80KB** にした(従来 77KB)。

### 副産物: GC ヒープの領域境界が奇数回の GC 後に壊れていた(既存バグ)

declaim のテストが GC を強制的に起こしたことで、後続の `disassemble_test` が
`%%disasm-region-bounds` の既存バグを踏んだ。**declaim とは無関係の既存バグ。**

`os_addr_region_bounds` の `OS_ADDR_GC_HEAP` が
`start = g_from_start` / `end = g_to_end` としていたが、**From/To は GC のたびに
入れ替わる**(`os_gc_collect_body` 末尾)。入れ替わった後は両者が中間点で一致し、
`start == end` となって「境界が未確定」と誤報していた。

```
#B GC前     gc回数=0 bounds=(25282240 . 196526080)
#B GC 1回後 gc回数=1 bounds=NIL          ← 奇数回で壊れる
#B GC 2回後 gc回数=2 bounds=(25282240 . 196526080)
#B GC 3回後 gc回数=3 bounds=NIL
```

両者の min/max を取る形に直し、`disassemble_test.lisp` へ
「GC を跨いでも境界が同じ区間を指す」回帰テストを追加した。
