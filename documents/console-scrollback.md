# コンソールのスクロールバック

> 対象: `feature/console-scrollback`。
> 前提調査: `documents/console-process-current-state.md`

画面外へ流れた出力を `Shift+PageUp` / `Shift+PageDown` で遡って読めるようにした。
きっかけは `disassemble` の出力(実測 253 行)が 50 行の画面に収まらないことだが、
仮想バッファ側の機能なので、デバッグ出力やエラーメッセージ全般に効く。

---

## 1. 操作

| 操作 | 動作 |
|---|---|
| `Shift` + `PageUp` | 表示中のバッファを1画面(50行)さかのぼる |
| `Shift` + `PageDown` | 表示中のバッファを1画面すすむ |
| 任意の文字入力 | 最下部へ復帰。**その文字はそのまま入力として通る**(握り潰さない) |
| `F1`〜`F4` | プロセス切り替え。遡り位置は**バッファごとに保持される** |

さかのぼり中は最終行に現在位置が出る。

```
-- SCROLL 50/252 --
```

左が「いま何行さかのぼっているか」、右が「さかのぼれる上限」(= `count - rows`)。
これが無いと、画面が固まったのか遡っているのか区別できない。

`0x49`(PageUp) / `0x51`(PageDown) は従来 `SCANCODE_NORMAL[]` が 0 で無視されていた
ため、既存の入力動作には影響しない。拡張キーの `0xE0` プレフィックスは
`scancode & 0x80` の判定で破棄済みなので、テンキーとナビゲーションキーの
PgUp/PgDn は区別されない(この用途では問題にならない)。

---

## 2. 実測値と採用した構成

すべて QEMU(OVMF 既定の GOP モード)上の実測。

| 項目 | 値 |
|---|---|
| 画面解像度 | **1280 x 800**(`screendump` で確認) |
| 桁数 `cols` | **160**(`width / 8`) |
| 画面行数 `rows` | **50**(`height / 16`) |
| 履歴行数 `hist_rows` | **1000**(`VBUF_DEFAULT_HIST_ROWS`) |
| 確保バイト数 | **640,000 byte**(`160 x 1000 x 4バッファ`) |
| 確保元 | boot allocator(`os_boot_alloc_try`) |

`hist_rows = 1000` は、実測 253 行の逆アセンブル出力が 3〜4 回分遡れる量。

`cols` は UEFI(GOP)から得た横ピクセル数を 8 で割った**実行時の値**で、
コンパイル時には固定していない。80 / 160 / 240 のいずれでも同じ結果になることを
`test/c/framebuffer_test.c` が確認する。

### 従来との比較

| | 従来 | 現在 |
|---|---|---|
| グリッド | `content[128][256]` 静的配列 | `cols x hist_rows` のリング(boot allocator) |
| サイズ | 32,768 byte x 4 = 128 KB | 160,000 byte x 4 = 640 KB |
| 実際に使える行 | 50(画面ぶんだけ。残り78行は未使用) | 1000 |
| 桁 | 256 確保・160 使用 | 160 確保・160 使用 |
| 流れた行 | **捨てられる** | 1000 行まで保持 |

確保量は 512 KB 増えるが、従来は 128 KB のうち 24%(8,000 byte)しか
使っていなかったので、実効的な無駄は減っている。

---

## 3. 設計

### 3-1 履歴リング

論理行 `i`(`0 <= i < count`)の実体は `content[((base + i) % hist_rows) * cols]`。
画面に出るのは最後の `rows` 行を `view_offset` ぶん手前へずらした窓で、
`cursor_position.y` はその窓の中の行番号(`0〜rows-1`)である。

```
top = count - rows - view_offset          /* 窓の先頭の論理行 */
0 <= view_offset <= count - rows          /* クランプの範囲 */
```

`count` は初期値 `rows`(空行で埋まった1画面)から始まり、`hist_rows` まで伸びる。
伸び切ったあとは `base` が進んで最古の行が捨てられる。**どちらの場合も
「詰める」ためのコピーは発生しない。**

```c
static void content_scroll_up(frame_buffer *self) {
    if (self->count < self->hist_rows) {
        self->count++;
    } else {
        self->base = (self->base + 1) % self->hist_rows;
    }
    clear_logical_row(self, self->count - 1);
}
```

`newline()` のスクロール判定条件は変えていない(物理画面の高さで判定し続ける)。
変わったのは「先頭行を捨てて全体をシフト」が「ベース/件数を進める」になった点だけ。

窓の位置の計算は `os_vbuf_visible_row` の1箇所にまとめてあり、描画とテストの
両方がそこを通る。2箇所に書くと、片方だけ直したときに静かにずれる。

### 3-2 確保のタイミング

`content` のサイズは `cols` が決まらないと分からず、`cols` は GOP の解像度から決まる。
解像度は `kernel_main` の引数として渡ってくるので、**ブートの最初から分かっている**。

一方 boot allocator は `os_boot_alloc_finalize` を呼ぶと残りが Lisp の GC ヒープに
なってしまうため、確保はその**前**に行う必要がある。そこで
`initialize_virtual_buffers` の呼び出しを `os_boot_alloc_finalize` より前へ繰り上げた。

```
os_boot_alloc_init
  → os_boot_alloc_try(content)         ← 追加
  → initialize_virtual_buffers          ← ここまで繰り上げ
  → clear_screen / draw_cursor          ← あわせて繰り上げ(理由は下記)
  → os_block_device_probe_all
  → os_boot_alloc_finalize
  → os_heap_init ...
```

`clear_screen` も一緒に繰り上げた。これは**物理ピクセルだけを黒く塗り content には
触れない**ので、間に出力があると「content には残っているが画面には出ていない」
食い違いが生まれ、バッファ切り替えやスクロールで唐突に現れる。content が空の時点で
呼んでおけばこの窓が消える。

副作用として、これ以降の `os_panic` や boot allocator 枯渇の診断表示が実際に画面へ
出るようになった(従来はブート後半までフレームバッファの関数ポインタが 0 のままで、
その間の診断は表示されないどころか NULL 参照になる状態だった)。

### 3-3 確保に失敗した場合

`os_boot_alloc`(枯渇時に停止する)ではなく `os_boot_alloc_try`(0 を返す)を
新設して使う。1000 行ぶんが取れなければ画面行数(50)まで落として続行する。
**スクロールバックは効かなくなるが、コンソールは従来どおり動く。**
起動しないよりよい。

なお `os_boot_alloc` の枯渇パスは `get_active_frame_buffer()->write_string` を
呼ぶが、仮想バッファの初期化より前ではその関数ポインタがまだ 0 で、そのまま呼ぶと
ページフォルトになる。NULL 検査を足してある。

### 3-4 確保サイズと実使用サイズの一致

`hist_rows` が画面行数を下回ると窓の計算(`count - rows`)が成立しないため切り上げる。
この切り上げは `os_vbuf_effective_hist_rows` に集約し、**確保側
(`os_vbuf_content_bytes`)と初期化側(`initialize_virtual_buffers`)の両方が通る**。

当初は初期化側だけで切り上げていた。呼び出し側が小さい値で確保すると、初期化が
その外側へ書き込む。`test/c/framebuffer_test.c` の
`test_hist_rows_below_screen_is_raised` がこれを踏み、AddressSanitizer が
`heap-buffer-overflow` として報告して発覚した。

### 3-5 出力が来たときの挙動

さかのぼり中に出力が来たら**最下部へスナップする**。触るのはそのバッファの
`view_offset` だけで、他のバッファには影響しない。

スケジューラは 4 プロセスすべてを回しているため、フォーカスしていないプロセスも
自分のバッファへ書き得る。ただし REPL は通常入力待ちで止まるので、実際に踏む頻度は低い。

### 3-6 インジケータ

`content` へは**書かない**。履歴が汚れると、戻ったときにインジケータが本文として
残ってしまう。物理フレームバッファの最終行だけを直接塗るので、
`render_frame_buffer` が content から描き直せば自然に消える。
表示するのはアクティブなバッファの状態のみ。

### 3-7 行末折り返しの off-by-one(同時修正)

```c
/* 修正前 */ if (cursor_x(self) > self->width)    /* ピクセルで比較。cols+1 文字目が入る */
/* 修正後 */ if (self->cursor_position.x >= self->cols)
```

`width = 1280` のとき `x = 160` で折り返さず、161 文字目が `content` に書かれていた。
一方 `render_frame_buffer` は `cols = 160` で切るため、**バッファを切り替えて戻ると
161 文字目が消える**という食い違いが起きていた。折り返し周りを触るのでここで直した。

---

## 4. 性能

`content_scroll_up` は O(行数 x 桁数) から O(桁数) になった。
修正前後の実装を同じ条件で 100,000 回まわした実測:

| | 1回あたり | 1行あたりのバイト操作 |
|---|---|---|
| 修正前(128x256 グリッド全体シフト) | 8.149 us | 32,768 |
| 修正後(履歴1000行のリング) | 0.040 us | 160 |
| | **201x 速い** | |

**履歴を 128 行から 1000 行へ増やしたうえで、むしろ軽くなっている。**

なお出力全体で見ると支配的なのは物理画面のピクセルスクロール
(`scroll_up`、1280x800x4 = 約 4MB のコピー)で、こちらは変えていない。
content 側の 32KB はその 1% 未満だったので、体感の差は出ない。
「遅くなっていないこと」が要件だったのでこれで足りる。

---

## 5. 検証

### 5-1 実機(QEMU モニタ経由)

`-monitor unix:...` で QEMU のモニタに繋ぎ、`sendkey` でキーを注入して
`screendump` で画面を取る形で、**キーボード割り込みから描画までの実際の経路**を確認した。
300 行を出力してから:

| 操作 | 結果 |
|---|---|
| 初期状態 | `LINE 252` 〜 `LINE 299` + `== END OF OUTPUT ==` |
| `Shift+PageUp` x1 | `LINE 202` 〜 `LINE 250`、`-- SCROLL 50/252 --` |
| `Shift+PageUp` x13 | `LINE 0` 〜 `LINE 48`、`-- SCROLL 252/252 --`(先頭でクランプ) |
| さらに `Shift+PageUp` | 変化なし(クランプが効いている) |
| `Shift+PageDown` x1 | `-- SCROLL 202/252 --`(ちょうど1画面ぶん戻る) |
| 文字キー `a` | 最下部へ復帰し、`a` がカーソル位置にエコーされる(握り潰していない) |

流れて消えていた `LINE 0`(出力の先頭)まで遡れることを確認した。

### 5-2 ユニットテスト

`test/c/framebuffer_test.c`(68 アサーション)。`framebuffer.c` は runtime に
依存しないので単体でリンクできる(content の確保を呼び出し側の責任にしてあるため)。

- リングの前進(一周前・一周後の `base` / `count`)
- 一周した後の窓の位置(`top` が負にならない、内容がずれない)
- 両端でのクランプ(空バッファで遡る / 最下部でさらに進む)
- `view_offset` がバッファごとに保持されること、他バッファへの出力で動かないこと
- `cols` が 80 / 160 / 240 のいずれでも同じ結果になること
- 確保失敗時(`hist_rows == rows`)に全操作が異常終了しないこと
- 行末がちょうど `cols` で折り返し、切り替えて戻っても内容が変わらないこと

AddressSanitizer 下でもエラー 0。

### 5-3 回帰

`make test` 8,343 passed / 0 failed、`make test-qemu` 3,228 passed / 0 failed。

---

## 6. 本作業に含めなかったもの

- **コンソールとプロセスの分離**。プロセス1つのコストは約 353KB でうちバッファは 9%、
  本当の制約は 256KB の静的スタックであり、分離しても動的 spawn には近づかない
  (`documents/console-process-current-state.md` §9)
- **`get_current_process()` が表示フォーカスを返す問題**(同 §8-1)。
  GC ルート混線という独立した不具合
- **`stream->out_fb` のキャッシュ問題**(同 §4)。動的な付け替えをしない限り顕在化しない
- **ページャ(`more` 相当)**。履歴リングができたので、必要になれば
  「一定行数で止めてキー待ち」だけで足せる
