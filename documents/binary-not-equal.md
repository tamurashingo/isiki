# `/=` の二項版を追加する

対象: PR #86(`feature/binary-not-equal`、base は `feature/compiler-optimization`)

関連: `documents/nan-comparison.md` §4-2(この問題を見つけた経緯)

---

## 1. 何をしたか

`primitive_num_not_equal2`(二項版)を追加し、`/=` を比較の専用経路に乗せた。

**比較演算子のうち、二項版が欠けていたのは `/=` だけだった。**

```
primitive_num_equal2     (=)   … 存在
primitive_less_than2     (<)   … 存在
primitive_greater_than2  (>)   … 存在
primitive_less_equal2    (<=)  … 存在
primitive_greater_equal2 (>=)  … 存在
primitive_num_not_equal2 (/=)  … 存在しなかった  ★
```

`za_compile_binary` は「`rcx=a, rdx=b` で直接 call できる二項版の関数ポインタ」を
要求するため、それが無い `/=` は `za_syms_t` に登録されず、n 項版
`primitive_num_not_equal` への**一般呼び出し**に落ちていた。

---

## 2. 効果

実測(96M、n=500。`%%za-compiled-p` が T であることを確認したうえで計測)。

| | 変更前 | 変更後 | `<`(対照) |
|---|---:|---:|---:|
| `code-len` | 1425 | **690** | **690** |
| `bytes/call` | 32 | **0** | **0** |

**`<` と完全に同じ数字になった。**

32 byte は引数リストを作るための cons 2 個ぶんである。`<` は
`primitive_less_than2(a, b)` をレジスタ渡しで直接呼ぶので確保が無い。
PR #80 で「比較は cons を作らず `number_compare` へ直接委譲する」と改善した経路に、
**`/=` だけ乗り損ねていた**のが揃った。

---

## 3. 実装

### 3-1 `num_ne` を使うこと(最大の注意点)

```c
lisp_val_t primitive_num_not_equal2(lisp_val_t a, lisp_val_t b) {
    return num_ne(a, b) ? g_sym_t : nil;
}
```

**`/=` は非順序(NaN)で真を返す唯一の比較である**(`documents/nan-comparison.md` §3-1)。

```c
/* やってはいけない。非順序がどちらにも該当せず偽になる */
return num_lt(a, b) || num_gt(a, b);
```

「等しくない」を「小さいか大きい」と読み替えると IEEE 754 に反する。
PR #85 で定義した `num_ne`(= `number_compare4(a,b) != NUM_CMP_EQUAL`)を
そのまま使えば、非順序は「等しくない」に含まれて真になる。

**型昇格(PR #80)も NaN 規則(PR #85)も `number_compare4` と `num_*` が
持っているので、二重実装しない。**

### 3-2 `za_syms_t` への登録

`ge` の直後に `ne` を足し、比較群としてまとめた。

**フィールド数は `_Static_assert` で固定されている**ので 25 → 26 に更新した。
更新を忘れるとビルドが止まる仕掛けで、実際に機能した。

```c
_Static_assert(sizeof(za_syms_t) == 26 * sizeof(lisp_val_t), ...);
```

この構造体は `za_try_compile_defun` の C スタック上のローカルで、
**コンパイル中の GC でシンボルが動くと `head == syms->ne` が静かに外れる**。
配列とみなして一括 link するため平坦性が機械的に保証されている(原則 8)。

### 3-3 n 項版はそのまま

引数が 3 つ以上なら従来どおり `primitive_num_not_equal` を通る。
`<` などと同じ構造である。

**意味論は隣接ペア判定のまま**で、CommonLisp の「全要素が相異なる」ではない
(本作業以前からの簡略化)。引数が 2 つのとき隣接ペアは 1 組しかないので、
二項版は n 項版の 2 引数と**同じ結果になる**。

```lisp
(/= 1 2)     ; 二項版を通る
(/= 1 2 1)   ; n 項版を通る。真(隣接ペア判定。変更なし)
```

---

## 4. 検証

| 検証 | 結果 |
|---|---|
| `make test` | 8498 OK / 0 NG |
| `make test-qemu` 256M | **4214 passed / 0 failed** |
| `QEMU_MEM=96M` | **4214 passed / 0 failed** |

`test/lisp/nan_comparison_test.lisp` に**JIT 経由の確認**を追加した。
二項版の追加で `/=` の通る経路が変わるため、**PR #85 で固定した NaN の結果が
経路が変わっても同じであること**を見る必要がある。

```lisp
(defun nan-ne-jit (a b) (/= a b))
(assert-equal t (%%za-compiled-p (function nan-ne-jit)))
(assert-equal t   (nan-ne-jit *nan-d* *nan-d*))   ; JIT 経由でも真
(assert-equal t   (/= *nan-d* *nan-d*))           ; インタプリタ経由も真
(assert-equal nil (nan-eq-jit *nan-d* *nan-d*))   ; = は偽のまま
(assert-equal t   (nan-ne-jit 0.1f0 0.1d0))       ; 型規則(PR #80)も維持
```

`za_syms_t` を触ったので、**他の比較が変わっていないこと**も確認している。

---

## 5. 次の作業への引き継ぎ

`feature/single-float-arith` で比較を `ucomiss` でインライン化する。
**`/=` が専用経路に乗ったので、対象は 6 つになる。**

| 演算子 | 実装 | 非順序のとき |
|---|---|---|
| `>` | `ja` | CF=1 で偽。自然に正しい |
| `>=` | `jae` | CF=1 で偽。自然に正しい |
| `<` | **オペランドを入れ替えて `ja`** | PF を見ずに正しくなる |
| `<=` | **オペランドを入れ替えて `jae`** | 同上 |
| `=` | `jp` で**偽へ**分岐してから `je` | ZF=1 に吸われるのを防ぐ |
| **`/=`** | **`jp` で真へ分岐**してから `jne` | **向きが他と逆** |

**`/=` だけ `jp` の飛び先が真である。** 他の 5 つは非順序を偽へ落とすが、
`/=` は非順序を真に含める。C 側(`num_ne`)と一致させること。
