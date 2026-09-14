# JITメタデータ調査(disassembler Phase 1)

> 対象: `feature/disassembler`。`documents/disasm-backend-decision.md` と対で読む。

このドキュメントは「JIT生成コードを逆アセンブルするために、いま何が手元にあり、
何が足りないのか」を実装コードから確認した結果である。指示書(引き継ぎ資料)が
想定していたファイル構成(`isiki-os/include/`, `src/`, `docs/`)は本リポジトリには
存在しないため、実際の構成(`src/c/`, `src/lisp/`, `test/c/`, `test/lisp/`,
`documents/`)へ読み替えている。

## 1. 関数オブジェクトの表現

JITコンパイル済み関数は `MAGIC_FUNCTION_NATIVE` の `TAG_INSTANCE`(4word)である
(`src/c/runtime.c` 冒頭のオブジェクト表レイアウト、`print.c:401`)。

| word | 内容 |
|---|---|
| word0 | `MAGIC_FUNCTION_NATIVE` (0x1) |
| word1 | `za_fn_meta_t` への**生ポインタ**(Immobilized Space上、GC非対象) |
| word2 | `nil`=組み込みprimitive / fixnum 1=za.cのJIT / fixnum 2=トランスパイラのlifted closure |
| word3 | 定義時env(JIT) または 捕捉env(lifted closure)。組み込みは `nil` |

`%%ZA-COMPILED-P`(`runtime.c:6143`)が `word0==MAGIC_FUNCTION_NATIVE && word2!=nil`
で「JIT済みか」を判定している。word2がfixnum 2(AOTトランスパイラのlifted closure)も
真になる点に注意(後述の「不足情報」参照)。

## 2. `za_fn_meta_t`(調査前の定義)

`src/c/runtime.h:854`

```c
typedef struct {
    UINT64 cons_entry;  /* offset 0: fn(evaluated_args, env) */
    UINT64 fixed_entry; /* offset 8: ABI-M5の固定引数レジスタ渡しエントリ(未使用なら0) */
    UINT64 arity;       /* offset 16: fixed_entryのarity */
} za_fn_meta_t;
```

オフセット0/8/16は**生成コードが直接dereferenceする**(`za_ensure_trampoline`が
`meta->cons_entry` を offset 0 で、`za_compile_call` が `meta->arity` を offset 16 で
読む)。したがって既存フィールドの並べ替えは不可、追加は末尾のみ可。

**コード長は保持していない。** これが disassembler にとって唯一の本質的な不足情報
だった(→「4. 追加したフィールド」)。

## 3. コードの格納先とライフサイクル

`za_try_compile_defun`(`src/c/za.c:5819`)の流れ:

1. 生成先は `static UINT8 g_jit_code[JIT_CODE_SIZE]`(512KB、16byte境界)。
   これは**ステージング用スクラッチ**で、関数間で共有される。
2. `UINT64 entry = g_jit_used;` を記録してからコード生成。
   `use_param_slots` の場合、`entry == fixed_entry_offset`(固定引数エントリが先頭)。
   続いて `cons_entry_offset`、共有本体、の順に並ぶ。
3. 成功すると `code_len = g_jit_used - entry` を
   `os_imm_pages_alloc_contiguous(page_count)` で取った
   **Immobilized Space の環境所有ページ**(4KB単位)へコピーする。
4. コピー後に2種類のrelocationを行う:
   - `g_jit_reloc_patch_offsets`: `g_jit_code` 内の別オフセットを指す自己参照movabs
     (埋め込みシンボル名文字列へのポインタ)。
   - `g_jit_trampoline_jmp_patch_offsets`: 共有トランポリンへの `jmp rel32`。
     トランポリンは `g_jit_code` 内に**残したまま**なので、コピー先から見た距離で再計算する。
5. `g_jit_used = entry` に巻き戻す(スクラッチ再利用)。
6. `os_make_jit_function(_dual)` で関数オブジェクトを作る。

**結論: 実行されるコードは Immobilized Space 上にあり、GC(Cheneyコピー)で動かない。**
`os_environment_reclaim_pages` で環境破棄時にのみ回収される。
したがって disassembler 側で GC の抑止(`za_gc_inhibit()` 相当)は**不要**であり、
そもそもそのようなAPIは存在しない。指示書 4-3 / FAQ の「GC を止める」は本リポジトリには
当てはまらない。

ただし**トランポリンだけは例外**で、`g_jit_code`(静的バッファ)内に居座る。
関数本体から `jmp rel32` で飛ぶ先が関数の外(コード範囲外)になるのはこのためである。

## 4. 追加したフィールド(本ブランチでの変更)

```c
typedef struct {
    UINT64 cons_entry;  /* offset 0  (既存、生成コードが直接読む) */
    UINT64 fixed_entry; /* offset 8  (既存) */
    UINT64 arity;       /* offset 16 (既存、生成コードが直接読む) */
    UINT64 code_base;   /* offset 24 (追加): 生成した機械語ブロック全体の先頭 */
    UINT64 code_len;    /* offset 32 (追加): 同ブロックのバイト長。0=不明(組み込み/AOT) */
} za_fn_meta_t;
```

- `code_base` は `za_try_compile_defun` のコピー先 `dest_bytes`、
  `code_len` は同関数の `code_len` をそのまま格納する。
- 末尾への追加なので、生成コードが焼き込んでいるoffset 0/8/16は不変。
- `os_fn_meta_alloc` は両方を0で初期化する。組み込みprimitive(`os_make_native_function`)と
  AOT lifted closure(`os_make_lifted_closure_with_meta`)は0のままで、
  「逆アセンブル対象外」を `code_len == 0` で表現する。
- Immobilized Space の消費は 24→40 byte/関数。`documents/pitfalls.md` 原則3の
  「O(プログラムサイズ)であること」は保たれる(defun 1個につき1個、実行回数に依存しない)。

## 5. シンボルテーブル(関数名 → 関数オブジェクト)

- `os_get_function(sym, env)`(`runtime.h:765`)が環境チェーンを辿って関数を返す。
- `os_get_function_cell(sym, env)` は Immobilized Space 上の Function Cell を返す
  (JIT生成コードの間接呼び出しが使う)。
- **逆方向(コードアドレス → 関数名)のテーブルは存在しない。**
  Phase 1 では `disassemble` の引数として渡された名前をそのまま見出しに使い、
  `call` 先のアドレスからC関数名を引く機能は持たない(→「7. 不足情報」)。

## 6. 既存のデバッグ facility

`za.c` 末尾に GC 監査用の診断primitiveがすでにある(`os_register_za_primitives`)。

| primitive | 内容 |
|---|---|
| `%%DIAG-ZA-CODE-ADDR` / `-CODE-LEN` | **直近に**コンパイルした関数のコード先頭と長さ |
| `%%DIAG-ZA-CODE-BYTE i` | 同上の i バイト目(ホスト側で逆アセンブルするための生dump用) |
| `%%DIAG-ZA-SCAN` / `-IMM-OFF` / `-IMM-VAL` / `-IMM-REGION` | 焼き込まれたmovabs即値の走査 |
| `%%ZA-HEAP-IMM-COUNT` | GCで動く領域を指す即値の検出数(原則8の常時監視) |

`%%DIAG-ZA-CODE-*` は**グローバルな「直近の1件」**しか持たないため、
任意の関数を後から指定して逆アセンブルすることはできない。これが本ブランチで
`za_fn_meta_t` にコード範囲を持たせた理由である(既存primitiveは変更しない)。

なお `%%DIAG-ZA-SCAN` の movabs 走査は、本ブランチで追加した本物のデコーダとは独立に
残す。役割が違う(あちらは「即値の値」、こちらは「命令列の表示」)。

## 7. disassembler に必要な情報 / 不足情報

| 情報 | 状態 |
|---|---|
| コード開始アドレス | ✓ `meta->code_base`(本ブランチで追加) |
| コード長 | ✓ `meta->code_len`(本ブランチで追加) |
| 関数名 | ✓ 呼び出し側が渡す(逆引きテーブルは無い) |
| 各エントリポイントの位置 | ✓ `cons_entry` / `fixed_entry`(コード内の位置を見出しに出せる) |
| 行番号情報 | ✗ 存在しない。Phase 1 では出さない |
| 変数のスロット割り付け情報 | △ `ZA_OFF_*` は**コンパイル時のCローカル**で、実行時には残らない。<br>`[rsp+0x...]` を意味のある名前に解決するのは Phase 2 以降 |
| call先のシンボル名 | ✗ カーネルのシンボルテーブルは持っていない。`call r11` の直前の `movabs r11, 0x...` の即値を見れば飛び先アドレスは分かるが、名前は引けない |
| 埋め込み文字列の識別 | △ `za_emit_symbol_name` が `jmp rel32` で飛び越した直後に NUL終端文字列を置く。<br>デコーダ側でこの形を認識して `.asciz` として表示する(ヒューリスティック、後述) |

## 8. 指示書のAPIから変えた点

指示書 3-1 が想定していたCインタフェースは、本リポジトリの制約に合わないため
以下のように読み替えた。いずれも「無い機能を使わない」ための変更である。

| 指示書 | 本実装 | 理由 |
|---|---|---|
| `za_disassemble_fn()` が `za_disasm_result_t *` を malloc して返し、`za_disasm_free()` で解放 | `os_disasm_item()` が呼び出し側の `os_disasm_insn_t` へ1項目ずつ書く | `-nostdlib` で `malloc`/`free` が存在しない。確保しない設計にすればリークもしない |
| `za_disasm_error_t` / `za_disasm_last_error()` | primitive は nil を返し、Lisp側(`disassemble`)が `error` を signal する | グローバルなエラー状態変数は「読み忘れると無言で通る」経路になる(原則6)。この処理系には既に condition system がある |
| `za_gc_inhibit()` / `za_gc_force()` で GC を止めてから逆アセンブルする | 何もしない | 生成コードは Immobilized Space 上にあり GC で動かない(上記3節)。そもそもそのAPIは存在しない |
| `ISL_SYMBOL *fn_name` を結果に持つ | 持たない | コードアドレス→関数名の逆引きテーブルが無い。名前は呼び出し側が渡す |
| 出力を `za_disasm_print()` が標準出力へ直接書く | 整形済みの1行を文字列として返し、出力先はLisp側が決める | ストリームに出す/文字列で受け取る/リストで受け取る、の3つを同じ経路で賄える |

`disassemble-to-string` だけは、文字列出力ストリームの容量
(`STREAM_STRING_OUTPUT_CAP` = 1024、`src/c/stream.h`)を超えると結果が途中で切れる。
ストリーム側が溢れを黙って捨てる実装なので、`disassemble-to-string` は末尾に番兵を
書いてそれが残っているかで切り詰めを検出し、起きていれば戻り値の末尾にその旨を
1行付ける(原則6)。全体が要る場合は `disassemble` でストリームへ直接出す。

## 9. Phase 1 で省略すると決めたこと

- AOT(`lisp_compiled.c`)および組み込みprimitiveの逆アセンブル。
  `code_len == 0` で明示的にエラーにする(指示書の「対象: JIT 生成コードに限定」)。
- 共有トランポリン本体の逆アセンブル(`g_jit_code` 内に残るため、関数のコード範囲外)。
- call/jmp先の関数名解決、行番号、変数名。
