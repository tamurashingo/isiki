# コンソールとプロセスの現状(スクロールバック実装の検討材料)

> 作成: 2026-09-14。逆アセンブラの出力が画面に収まらない件をきっかけに、
> スクロールバッファを入れる前提として現状の構造を調べたもの。
> **本ドキュメントは調査結果であり、変更は含まない。**

---

## 0. 結論(先に3行)

1. **F1〜F4 は「コンソール」ではなく「プロセス」である。** 仮想バッファはプロセスの
   標準出力として 1:1 で固定的に紐付いているだけで、コンソールという独立した概念は無い。
2. **出力経路は既にプロセス所有のバッファを通っている。** バッファ側だけを直せば
   スクロールバックは成立する。
3. ただし **「表示フォーカス」「実行中プロセス」「出力先バッファ」が1つの添字に
   潰れている**。プロセスを増やす前に分離しておかないと後戻りが大きい。

---

## 1. 全体像

```
   物理フレームバッファ (UEFI GOP, 1280x800)
   ┌──────────────────────────────────────────┐
   │  全仮想バッファが同じアドレスを指す      │   framebuffer.c:271
   │  is_active() な 1 つだけが実際に描画     │   framebuffer.c:17
   └──────────────────────────────────────────┘
                     ▲
                     │ active_index (framebuffer.c:9)
                     │
   ┌─────────┬─────────┬─────────┬─────────┐
   │ vbuf[0] │ vbuf[1] │ vbuf[2] │ vbuf[3] │  static frame_buffer frame_buffers[4]
   │ content │ content │ content │ content │  各 128行 x 256桁 の文字グリッド
   │ cursor  │ cursor  │ cursor  │ cursor  │
   └────▲────┴────▲────┴────▲────┴────▲────┘
        │         │         │         │      proc->stdout_buffer = &buffers[i]
        │         │         │         │      (process.c:397, 初期化時に固定)
   ┌────┴────┬────┴────┬────┴────┬────┴────┐
   │ proc[0] │ proc[1] │ proc[2] │ proc[3] │  g_processes[PROCESS_COUNT]
   │  "F1"   │  "F2"   │  "F3"   │  "F4"   │
   │ stdin   │ stdin   │ stdin   │ stdin   │  各 256 byte
   │ env     │ env     │ env     │ env     │  global_environment の子環境
   └─────────┴─────────┴─────────┴─────────┘
        │         │         │         │
   ┌────┴────┬────┴────┬────┴────┬────┴────┐
   │ stack0  │ stack1  │ stack2  │ stack3  │  各 256KB + 前後に 64KB ガード
   └─────────┴─────────┴─────────┴─────────┘  g_stack_area[] (process.c:53)
```

束ねているのは `process.h:7` のこの1行:

```c
/** プロセス数(F1〜F4に割り当てる。1プロセス=1仮想バッファ) */
#define PROCESS_COUNT VBUF_COUNT
```

---

## 2. 仮想バッファ (`frame_buffer`)

`src/c/framebuffer.h` / `src/c/framebuffer.c`

| 項目 | 値 | 場所 |
|---|---|---|
| 個数 | `VBUF_COUNT` = 4 | framebuffer.h:7 |
| 文字グリッド | `UINT8 content[VBUF_MAX_ROWS=128][VBUF_MAX_COLS=256]` | framebuffer.h:29 |
| 実体 | `static frame_buffer frame_buffers[VBUF_COUNT]`(`.bss`) | framebuffer.c:6 |
| アクティブ判定 | `self == &frame_buffers[active_index]` | framebuffer.c:17 |

### 非アクティブでも状態は保持される

`write_char` は **content グリッドを常に更新**し、アクティブな場合だけ物理画面へも
描画する(framebuffer.c:160)。切り替え時は `render_frame_buffer` が content を
そのまま描き直す(framebuffer.c:214)。つまり「裏で動いているプロセスの出力が
消える」ということはない。

### しかし content は「履歴」ではない

`newline()`(framebuffer.c:142)がスクロールを判定する条件:

```c
self->cursor_position.y += 1;
if (cursor_y(self) >= self->height) {   /* ← 物理画面の高さ。VBUF_MAX_ROWS ではない */
    content_scroll_up(self);            /* ← 先頭行を捨てて全体を1行上へシフト */
    if (is_active(self)) { scroll_up(self); }
    self->cursor_position.y -= 1;
}
```

- スクロール判定は **物理画面の行数**(`height / 16`)で行われる
- 1280x800 なので **50行**。`VBUF_MAX_ROWS` は 128 なので **78行(61%)は一度も使われない**
- 桁も同様で、`width / 8` = **160桁**に対し `VBUF_MAX_COLS` は 256 なので **96桁(37%)が未使用**
- `content_scroll_up` は先頭行を**捨てる**ので、流れた行はどこにも残らない

実際に使われているのは 50行 x 160桁 = 8,000 byte / 32,768 byte = **24%** である。

### スクロール処理のコスト

`content_scroll_up` は毎回 `VBUF_MAX_ROWS x VBUF_MAX_COLS` = 32,768 byte 分のループを
回す(実際に意味があるのは 8,000 byte 分だけ)。履歴を持たせて行数を増やすと、
この方式ではそのまま比例して重くなる。

---

## 3. プロセス (`process_t`)

`src/c/process.h` / `src/c/process.c`

| フィールド | 用途 |
|---|---|
| `stdout_buffer` | **`frame_buffer *`**(ポインタ)。初期化時に `&buffers[i]` 固定 |
| `stdin_buf[256]` / `stdin_len` / `read_pos` | キー入力の蓄積 |
| `ready` | Enter で1行確定したフラグ(`volatile`、割り込みから書かれる) |
| `env` | `global_environment` の子環境。GC ルート登録済み |
| `live_blocks` | 動的extentにある block 名のリスト。GC ルート登録済み |
| `gc_roots` | このプロセスの shadow stack 先頭 |

### spawn は「プロセスを作る」ものではない

`src/c/process.c:310`

```c
static lisp_val_t spawn(UINT32 proc_index) {
```

- **`static`。Lisp から呼べる `spawn` は存在しない**(`src/lisp/`・`subprimitive.c` に無し)
- 引数は新規作成ではなく、**既に確保済みの `g_processes[4]` / `g_stack_area` への添字**
- 実際にやるのは、その添字のスタック上に偽の IRETQ フレーム(15レジスタ + iretqフレーム +
  FXSAVE領域)を組み、PCB(`MAGIC_PROCESS`)を `*RUN-QUEUE*` の循環リストへ繋ぐこと
- 呼び出しは `process_scheduler_start` の `for (i = 0; i < PROCESS_COUNT; i++) spawn(i);` のみ

**したがって現状、実行時にプロセスを増やす手段は無い。** 「spawn したらバッファが要るか」
という問いは、spawn を動的化した場合に初めて生じる。

---

## 4. 出力経路

Lisp の通常出力は**すべてプロセス所有のバッファ**へ行く。非フォーカスのプロセスが
書いても正しく自分のバッファに入る。

| 経路 | 出力先 | 場所 |
|---|---|---|
| REPL の結果表示 | `proc->stdout_buffer` | repl.c:32 |
| プロンプト `F1>` / キーエコー | `proc->stdout_buffer` | reader.c:53,55 / interrupt.c:333 |
| `load` の進捗メッセージ | `proc->stdout_buffer` | load.c:17 |
| `(standard-output)` の既定値 | `get_current_process()->stdout_buffer` | stream_lisp.c:184 |

`get_active_frame_buffer()`(表示フォーカス)を直接使うのは**診断経路だけ**である。

| 経路 | 場所 |
|---|---|
| `os_panic` / スタック溢れ診断 / GCルート枯渇 | runtime.c:404, 770, 1031, 1449, 1479, 1533, 2657 |
| CPU 例外ダンプ | interrupt.c:262, 719 |

これらは「今画面に出ているところに出す」のが正しいので、意図どおり。

### `(standard-output)` の解決

`src/lisp/init.lisp:872`

```lisp
(defun standard-output ()
  (let ((s (dynamic *standard-output*)))
    ...))   ; nil なら (open-output-stream) で画面ストリームを新規作成
```

- `*standard-output*` が nil の間は、**呼ぶたびに新しい画面ストリームを作る**
  (`cc_open_output_stream(nil, env)` → `get_current_process()->stdout_buffer`)
- `os_stream_open_screen_output`(stream.c:261)は **`frame_buffer *` を
  `stream->out_fb` にキャッシュする**

  → **一度開いたストリームは、後から `proc->stdout_buffer` を差し替えても追随しない。**
     コンソールを動的に付け替える設計にする場合はここが効く。
- `*standard-output*` は動的変数だが、`g_dynamic_bindings` は**単一のグローバル**
  (runtime.c:222)。つまり **動的変数はプロセス間で共有されている**。
  片方のプロセスの `with-standard-output` が全プロセスに波及する。

---

## 5. 入力経路と F1〜F4

`src/c/interrupt.c:292` `c_keyboard_handler`

```
scancode 0x2A/0x36        → Shift 押下
scancode 0xAA/0xB6        → Shift 解放
scancode & 0x80           → その他の Break コードは破棄     ← 0xE0 もここで消える
scancode 0x3B〜0x3E       → switch_active_process(sc - 0x3B)  ← F1〜F4
それ以外                  → SCANCODE_NORMAL/SHIFT[128] で ASCII 化
                            → current->stdout_buffer へエコー
                            → process_stdin_push(current, c)
```

`switch_active_process`(process.c:432)がやること:

```c
g_processes[g_current_process_index].state = PROCESS_STATE_READY;
g_current_process_index = index;
g_processes[g_current_process_index].state = PROCESS_STATE_RUNNING;
switch_active_frame_buffer(index);          /* 表示バッファも同じ添字へ */
```

**プロセス切り替えが主で、表示バッファはそれに追従する。** 逆ではない。

### 拡張キーは現状すべて捨てられている

`0xE0` は `scancode & 0x80` が真なので Break コードとして破棄される。続く make コードは
プレフィックス無しで単独到着する。PageUp = `0x49`、PageDown = `0x51` はどちらも
`SCANCODE_NORMAL[]` が 0 なので無視される。

- **スキャンコードテーブルは `[128]`** なので、`0xE0` が範囲外参照になる心配は無い
  (`& 0x80` で先に弾かれるため)
- `0x49` / `0x51` は**空き番としてそのまま使える**
- 副作用として、テンキーの PgUp/PgDn とナビゲーションキーの PgUp/PgDn が
  区別されず同じ動作になる(スクロールバックの用途では問題にならない)

---

## 6. メモリの内訳(実測)

`sizeof` を実測した値。すべて `.bss`(静的確保)。

| 項目 | 1個あたり | x4 |
|---|---|---|
| スタックスロット(ガード 64KB + スタック 256KB) | **320 KB** | 1,344 KB(末尾ガード込み) |
| `frame_buffer`(うち `content` が 32,768 B) | 32,856 B | 128 KB |
| `process_t` | 320 B | 1.2 KB |
| **プロセス1つの追加コスト** | **約 353 KB** | **約 1.44 MB** |

`.bss` 全体は 20.4 MB(`objdump -h` 実測)で、内訳の大半は以下:

| 項目 | サイズ |
|---|---|
| `g_imm_space`(Immobilized Space) | 16 MB |
| `g_stack_area`(プロセススタック) | 1.31 MB |
| `g_jit_code`(JIT ステージングバッファ) | 512 KB |
| `frame_buffers` | 128 KB |

**バッファはプロセス1つあたりのコストの 9% にすぎない。** プロセスを増やしたときに
最初に効くのは 256KB のスタックである。

---

## 7. スクロールバックを入れるときに効く制約

### 7-1. 必要な変更(4点)

1. **content を物理画面から切り離す**
   現在は `newline()` が画面の高さでスクロールし、先頭行を捨てている。
   履歴 N 行のリングバッファにし、書き込みカーソルを履歴座標で持つ必要がある。
   現在の `content_scroll_up` は毎行 32KB のループなので、履歴を持つとそのままでは
   さらに重くなる。**ベース添字を進めるリングにすれば O(1)** になる。

2. **`view_offset`(何行遡っているか)をバッファごとに持つ**
   `render_frame_buffer` は今グリッド全体を描いているので、`[top, top+rows)` の窓を
   描く形に変える。

3. **`write_char` の直接描画との整合**
   現在は content 更新と同時に物理画面へ `draw_char` している。遡って表示している
   最中に新しい出力が来たとき、**(a) 描画を抑止して履歴だけ進める** か
   **(b) 最下部へスナップする** かを決める必要がある。
   → **ここは設計判断。** (a) は Unix 端末的で落ち着くが「動いているのに画面が
     止まって見える」、(b) は分かりやすいが読んでいる途中で飛ばされる。

4. **キー入力**
   `0x49`/`0x51` を `c_keyboard_handler` の F1〜F4 と同じ位置(ASCII 変換の前)で
   拾う。現状は無視されているので既存動作への影響は無い。

### 7-2. サイズの見積もり

| 構成 | 1コンソール | x4 |
|---|---|---|
| 現状(128行 x 256桁、うち有効 50 x 160) | 32 KB | 128 KB |
| 履歴 1000行 x 256桁 | 256 KB | 1 MB |
| **履歴 1000行 x 160桁**(実桁数に切詰め) | **160 KB** | **640 KB** |
| 履歴 2000行 x 160桁 | 320 KB | 1.25 MB |

`VBUF_MAX_COLS` を実際の 160 まで下げるだけで 37% 削減できる。
参考として、今回の `(disassemble 'dis-caller)` は見出し込み **約 253 行**。
1関数を遡るなら 256 行、実用なら 1000 行程度。

---

## 8. 既知の不具合・設計上の歪み

### 8-1. `get_current_process()` は「実行中」ではなく「表示フォーカス」

`process.h:98`

```c
static inline process_t* get_current_process(void) {
    return &g_processes[g_current_process_index];
}
```

`g_current_process_index` を更新するのは `switch_active_process`(F1〜F4)**だけ**で、
スケジューラ(`c_timer_switch`)は `*CURRENT-PROCESS*` / `*RUN-QUEUE*` / PCB 側で
切り替えており、この添字には触らない。

`runtime.c:445-448` に明記されている:

> `get_current_process` が返すのは `g_current_process_index`(=表示フォーカス)であって、
> スケジューラが実際に走らせているプロセスではない。プリエンプティブな切り替えが
> 起きると、複数のプロセスのノードが1本のリストへ混ざる

- `GC_PROTECT` は `get_current_process()->gc_roots` へ繋ぐので、この混線が起きる
  (`g_gc_lifo_violations` が回数を数えている)
- `os_process_stack_check`(process.c:247)は添字が当てにならないため、
  **rsp がどの範囲に入るか**で判定するという回避をすでに入れている

**現在は「プロセス添字 == バッファ添字 == フォーカス添字」で偶然1つに見えているだけ。**

### 8-2. 行末の折り返しが1文字ぶんずれている

`framebuffer.c:176`

```c
self->cursor_position.x += 1;
if (cursor_x(self) > self->width) {   /* >= であるべき */
```

`width = 1280` のとき `x = 160`(= 1280px)で折り返さず、**161文字目が
`content[row][160]` に書かれる**。一方 `render_frame_buffer` は `cols = width/8 = 160`
で切るため、**バッファを切り替えて戻ると 161 文字目が消える**。
ライブ描画と再描画で内容が食い違う。行の折り返し周りを触るなら同時に直すのがよい。

---

## 9. 検討の分岐点

### 選択肢 A: 現状維持(1プロセス = 1バッファ)のままスクロールバックを入れる

- 変更は `framebuffer.c` 内で閉じる。最短
- プロセスを増やすたびに履歴ぶん(160KB〜)が増える
- プロセス数は当面 4 固定なので、当座は問題にならない

### 選択肢 B: コンソールをプロセスから分離してからスクロールバックを入れる

- `stdout_buffer` は**既にポインタ**なので、束ねているのは `#define PROCESS_COUNT
  VBUF_COUNT` と `initialize_processes` の代入だけ。分離自体は小さい
- コンソール数は「画面に出せる数 / F キーで選べる数」= 4 で固定し、
  プロセスは独立に増やせる(Unix の tty と多対1の関係)
- 履歴コストがプロセス数に依存しなくなる(4 コンソール固定で 640KB 〜 1MB)
- 分離すると、今1つに潰れている 3 つが明示的に分かれる:
  - `active_console` — 画面に出ているコンソール
  - そのコンソールの foreground process — キー入力を受け取るプロセス
  - 実行中プロセス — スケジューラの管轄(表示とは無関係)
- ただし複数プロセスが1コンソールを共有すると、出力が混ざりカーソルも共有になる
  (job control が無い tty と同じ)。誰がキー入力を受けるかを明示的に持つ必要がある
- `stream->out_fb` のキャッシュ(§4)により、**既に開いているストリームは付け替えに
  追随しない**。動的な付け替えをやるならここも直す必要がある

### 判断材料

- **動的 spawn を将来やるか。** やらないなら A で十分。やるなら B を先にしたほうが
  後戻りが小さい(履歴リングの所有者を後から移す作業が乗らない)
- **本当の制約はバッファではなくスタック(256KB/プロセス)。** プロセスを増やす話は、
  バッファよりも先にスタックの確保方式(静的配列 → 動的確保)の検討が要る
