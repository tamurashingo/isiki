# `/` を直接 call 経路に乗せる

対象: PR(`feature/divide-direct-call`、base は `feature/compiler-optimization`)

関連: `documents/nan-comparison.md`、PR #86(`/=` の二項版)

---

## 1. 何をしたか

`primitive_divide2`(二項版)を新設し、`za_syms_t` へ `/` を登録して
**二項の直接 call 経路**に乗せた。

**ISLisp に `/` は無い。** `quotient` が正式で、`/` はその実装
(`init.lisp` の `%quotient2`)が内部で使う経路である。

PR #86 の `/=` と同じ形である。比較演算子で二項版が欠けていたのが `/=` だけ
だったように、算術では `/` だけが `za_syms_t` に居なかった。

---

## 2. 効果

実測(96M、n=500。`%%za-compiled-p` が T であることを確認したうえで計測)。

| | 変更前 | 変更後 | 対照 `<` |
|---|---:|---:|---:|
| **`/` code-len** | 1424 | **690** | **690** |
| `/` 整数 bytes/call | 32 | 32 | 0 |
| **`/` single bytes/call** | 32 | **0** | 0 |
| **`/` double bytes/call** | 64 | **32** | — |
| `quotient` 整数 bytes/call | 272 | **240** | — |
| `quotient` single bytes/call | 112 | **80** | — |

**コードサイズは `<` と完全に同じになった**(1424 → 690)。PR #86 の `/=`
(1425 → 690)と同じ形である。

### bytes/call が型で違う理由

**呼び出し側の cons 2 個(32 byte)は全型で消えた。** 残っているのは C 側が
作り直す分である。

| 型 | 残る確保 | 結果 |
|---|---|---|
| single | 無し(`os_make_float_of_kind` が即値を返す) | **0** |
| double | instance の確保 32 byte | 32 |
| 整数 | `primitive_divide2` が n 項版へ委譲して cons 2 個 | 32(差し引き 0) |

**完了条件の「bytes/call が 0」は single でのみ達成した。**

整数を 0 にするには `primitive_divide2` の bignum 経路を二項版へ展開する必要が
あるが、それは**本作業が除外した「`primitive_divide2` の中身の変更」**に当たる。
double は結果が instance である以上、確保は避けられない。

### `quotient` への波及

**全型で一律 32 byte 減った**(272→240、112→80)。内側の `/` の cons が消えた
ぶんそのままである。**外側の Lisp 関数呼び出しのコストが支配的**で、
`/` ほどは改善しない。

---

## 3. 調査

### 3-1 `za_compile_binary` は確保する呼び先を扱えるか

**これが登録の可否を決める点だった。答えは「扱える」。**

`za_compile_binary` がこれまで扱ってきたのは比較演算だけで、比較は一切
ヒープを確保しない。一方 `primitive_divide2` は bignum や double を返すため
**確保しうる**。「呼び先は確保しない」前提で GC 保護を省いていれば、
`/` を乗せるのは GC バグになる。

**省いていなかった。** 確保しうる `primitive_add2` を呼ぶ `za_compile_fold` と
**保護の形が完全に同一**である。

| | `za_compile_fold`(確保する呼び先) | `za_compile_binary` |
|---|---|---|
| op0 の退避 | `za_store_slot(RAX, val_off)` | 同じ |
| GC ルート登録 | `if (!skip_protect) za_emit_gc_link_slot(...)` | 同じ |
| call 直前 | `za_load_slot(RCX, val_off)` | 同じ |
| 解除 | `za_emit_gc_unlink_slot(node_off)` | 同じ |

**call 中の GC は C 側が担保する。** `primitive_*2` は確保の前に引数を
`GC_PROTECT` する。これは `+` でも `/` でも変わらない。

実測でも確認した(§5)。

### 3-2 なぜ登録されていなかったのか

コメントにも経緯にも「意図的に外した」という記録は無かった。
`+` `-` `*` は `za_compile_fold` / `za_compile_minus` に乗っているが、
**除算は畳み込みの形に合わない**(非可換で、`(/ a b c)` は `((a/b)/c)`)ため、
fold へ足すには専用の関数が要る。`za_compile_binary` で二項だけ拾えば済むと
気づかれていなかった、という形に見える。

### 3-3 切り捨て方向(§3-4)

**実装を読めば分かる。** `documents/fixnum-signed-fastpath.md` が
「確認しないと着手できない」として保留していたが、仕様を調べるまでもなかった。

```lisp
(/ 7 2)  => 3      (/ -7 2)  => -3     ; **ゼロ方向**(床関数なら -4)
(quotient 7 2) => 3.5   (quotient -7 2) => -3.5
```

`quotient` は割り切れないと float を返すので、切り捨て自体が起きない。

**本作業では変えていない。**

---

## 4. 実装

`primitive_add2` と同じ形にした。

```c
lisp_val_t primitive_divide2(lisp_val_t a, lisp_val_t b) {
    int kind = float_kind_max(float_kind_of(a), float_kind_of(b));
    if (kind != FLOAT_KIND_NONE) {
        return os_make_float_of_kind(kind, to_double(a) / to_double(b));  /* cons なし */
    }
    GC_PROTECT(a);
    GC_PROTECT(b);
    lisp_val_t args = os_make_cons(a, os_make_cons(b, nil));
    return primitive_divide(args, global_environment);                    /* 整数は委譲 */
}
```

`za_syms_t` へ `slash` を追加し、フィールド数を固定する `_Static_assert` を
26 → 27 へ更新した(更新を忘れるとビルドが止まる)。

**二項のみ。** `(/ a b c)` は一般呼び出しのままで、n 項の型昇格規則は C 側が持つ。
`<` や `/=` と同じ構造である。

**型特化はしていない。** `primitive_divide2` の中身は変更しない。

### ゼロ除算は二系統のまま

| | ゼロ除算 |
|---|---|
| `/` | IEEE どおり `inf` / `nan` |
| `quotient` | **`<division-by-zero>` をシグナル** |

`%quotient2` が `/` を呼ぶ**前に**除数 0 を弾く。本作業で変えていない。

---

## 5. 検証

| 検証 | 結果 |
|---|---|
| `make test` | 8498 OK / 0 NG |
| `make test-qemu` 256M | **4248 passed / 0 failed** |
| `QEMU_MEM=96M` | 同上 |

`test/lisp/divide_direct_call_test.lisp` で固定した範囲。

**GC 安全性(§3-1 の実証)**

- bignum を返す除算(`(/ (expt 2 100) 3)`)を **GC を跨いで 200 回**
- double を返す除算を同じく 200 回
- **`%%za-heap-imm-count` が 0** — 生成コードに GC で動く領域を指す即値が
  焼かれていないこと(JIT の即値焼き込み監査)

**意味論**

- ゼロ方向の切り捨て、負数、n 項、型昇格(PR #80)
- ゼロ除算(`/` は INF、`quotient` は `<division-by-zero>`)
- `quotient` / `reciprocal` の結果が変わっていないこと

---

## 6. 測定の作法: 切り替え前に作業ツリーが clean か確認する

**変更前後で同じバイナリを測る失敗を 2 回した。**

| | 手段 | 失敗の形 |
|---|---|---|
| PR #87 | `git stash push -q -- src/c` | オプション位置の誤りで stash が空振り |
| 本作業 | `git checkout <base>` | **未コミットの変更が持ち越された** |

`git checkout` はコンフリクトしない限り未コミットの変更を持ち越すので、
**コミットか stash を済ませていないと切り替えたことにならない。**

**2 回とも、気づけた手がかりは「完全一致」だけだった**
(`documents/declare-types.md` §6-2)。本作業では `/` の code-len が対照の `<` と
同じ 690 に並んだ時点で気づいた。

**測定手順に「切り替え後に `git status --porcelain` が空であることを出力する」を
入れること。** 本作業の最終測定ではそうした。
