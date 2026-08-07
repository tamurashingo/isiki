
# isikiOS は ISLisp ベースの LispOS の実装

## できること(TODO)

- シングルユーザー
- マルチプロセス
- すべてがオープンで変更可能
  - ハードウェアを直接参照できる
  - ハードウェアを直接叩ける
  - すべてのメモリを参照できる
  - すべてのメモリを更新できる
  - 他のプロセスの状態を変更できる
    - PC
    - stackframe
    - registers


## 実行モデル

### ISLisp の module とインクリメンタルな開発の相性

ISLisp には CommonLisp のような package は存在しないが、module という単位でコードをまとめてカプセル化する仕組みは存在する。
module は外部にシンボルを晒さずコードを隠蔽するためのものであり、この点はそのまま isikiOS でも利用できる。

しかし module はあくまで完成したコードをまとめて隠蔽するための仕組みであり、CommonLisp の package のように
現在の環境に対して定義を継続的に追加・再定義していく、実行しながら開発を進める(インクリメンタルな)開発を支援するものではない。
ISLisp は、開発しきった完成済みのコードを module としてロードして実行する、というスタイルを前提としている。

isikiOS は REPL を通じてコードを書きながら動かし、動かしながら書き直していくインクリメンタルな開発スタイルを取る。
module によるカプセル化の恩恵はそのまま受けられるが、module 自体はこのインクリメンタルな開発、
とりわけ既存の定義を自由に上書きしていくシャドウイングには関与しない。

そこで isikiOS では、ISLisp の仕様の小ささを保ったまま CommonLisp 的な開発のしやすさを実現するため、
module によるカプセル化とは別に「環境(environment)」という仕組みを用意し、シャドウイングによるインクリメンタルな開発を実現する。

### インクリメンタルな開発のための環境(environment)

環境は、変数・関数の定義を保持する入れ物である。各環境は以下の3つのスロットを持つ。

- `variables`: その環境に登録された変数の一覧
- `functions`: その環境に登録された関数の一覧
- `parent`: 親環境への参照。シンボルの探索は自分の環境 → parent → parent の parent … の順に行われる

イメージ

```lisp
'((variables . ())
  (functions . ())
  (parent . ()))
```

すべての環境は、最終的に parent をルート環境である global environment まで遡ることができる。
isikiOS 起動時には、global environment に加えて、開発者が最初に使う user 用の環境が用意される。

```lisp
(defparameter *user-environment*
  '((variables . ())
    (functions . ())
    (parent . *global-environment*)))
```

### 環境を汚しながら開発するスタイル

package を持たず、module も完成済みコードの隠蔽にしか使えない ISLisp では、開発中に試した定義もそのままグローバルな環境に登録されてしまい、試行錯誤の跡を切り離しておく場所がない。
isikiOS ではこれを避けるため、作業の単位ごとに新しい環境を作成し、その環境の中でシンボルを自由に汚しながら(登録・再定義しながら)開発を進める。

```lisp
;; 親の環境を指定できる。未指定時は *global-environment* が親となる
(make-environment dev-environment ())

;; 作成した環境に入る
(in-environment dev-environment)
```

### シンボルの共有とシャドウイングによる上書き

symbol 自体はどの環境からも共有された、global にひとつだけ存在する値である(intern)。
そのため global-environment の `foo` と dev-environment の `foo` は同じ symbol を指している。

親環境で既に使われているシンボルと同じシンボルを使って現在の環境に変数・関数を登録すると、親環境側の定義は隠され(shadow)、
現在の環境の中ではその新しい定義が優先して使われるようになる。

```lisp
;; dev-environment の functions スロットに foo を登録する
;; global-environment にあった foo は shadow され、dev-environment の中では新しい foo が使われる
(defun foo ()
  "hello world")
```

親環境側の定義そのものは書き換わらないため、dev-environment を抜けて別の環境に移れば元の `foo` がそのまま使える。
このシャドウイングの仕組みにより、環境を汚しながら値や処理を自由に上書きして試行錯誤できる。

### 束縛の書き込み先: defparameter・defun は current environment にのみ作用する

ISLisp は Lisp-2 であり、1つの symbol に対して変数と関数が別々のセル(名前空間)を持つ。
これに対応して isikiOS の環境も `variables` スロットと `functions` スロットを分けて持つ。

`defparameter` や `defun` は、実行時の current environment の `variables`/`functions` スロットにのみ束縛を書き込む。
親環境やさらに上の環境の束縛を書き換えることはない。

例えば parent が global-environment である bar-env の中で `(defparameter x 100)` を実行すると:

- global-environment の `variables` スロットにある `x` の値は変化しない
- bar-env の `variables` スロットに新しく `(x . 100)` という束縛が追加される(すでに bar-env 自身に `x` の束縛があれば、その値だけが上書きされる)

一方、symbol の値を参照する際は、current environment から始めて parent → parent の parent … と順に辿り、最初に見つかった束縛の値を返す。
そのため bar-env の中で `x` を参照すると、bar-env 自身に登録された `100` が見える(shadow)。bar-env を抜けて global-environment に戻れば、元の `x` の値がそのまま見える。

まとめると:

- 書き込み(`defparameter` / `defun` / `setq` など): 常に current environment の `variables`/`functions` スロットのみに対して行われる
- 読み込み(symbol の参照・関数呼び出し): current environment → parent → … と辿り、最初に見つかった束縛を使う

この「書き込みは current のみ、読み込みは parent まで辿る」という非対称な挙動が、前述のシャドウイングの実体である。

### 他の環境への直接アクセス

symbol は全環境で共有されているため、現在の環境ではなく親環境や他の環境を明示的に指定して、その環境の変数・関数を直接参照・変更することもできる。

```lisp
;; global-environment の foo を直接参照する(shadow を経由しない)
(env-ref-function global-environment 'foo)

;; global-environment の foo を直接書き換える(shadow ではなく破壊的な上書き)
(env-set-function global-environment 'foo (lambda () "rewritten"))
```

これにより、dev-environment で固まった変更を親環境側へ反映させたり、逆に他の環境の状態を直接調べたりすることができる。

### 組み込み関数の再定義について

ISLisp では組み込み関数の再定義やシャドウイングは制限されているが、isikiOS では上記の仕組みにより明示的にこれを許可する。


## subprimitiveについて

システムプログラムやOSの実装のために用意された低レベルな関数である。
プレフィックスでどのレベルの情報にアクセスできるかが変わる。

- %%: ハードウェア層
  - CPU命令やI/Oポート、レジスタの直接操作
  - LispObjectのタグや型を一切無視した64bitの値
  - `(%%in-8 #x60)` `(%%sti)`

- %: OS・カーネル層
  - GC、メモリ管理、デバイスドライバの内部ロジック
  - タグは外されているが、ポインタや整数として意味が整えられた値
  - `(%untag-cons c)` `(%parse-scancode raw)`

- (プレフィックスなし): ユーザ・システム層
  - アプリケーション、REPL、OS標準API
  - すべてがLispのタグ付きオブジェクトとして完全に隠蔽された世界
  - `(display x)` `(car x)`


この記法を利用した、キーボードから入力を一文字受け取るという処理のサンプル

```lisp
;; 【ユーザー・システム層】普通のアプリケーションが呼ぶ安全な関数
(defun read-char-from-keyboard ()
  (let ((key-event (sys-pop-keyboard-buffer))) ; 安全なLispオブジェクト（構造体など）が返る
    (if (eq (keyboard-event-type key-event) :press)
        (keyboard-event-char key-event)
        nil)))

;; 【OS・カーネル層】タイマー割り込みやドライバが裏で回す低レイヤー関数
(defun %handle-keyboard-interrupt ()
  (let ((status (%%in-8 #x64))) ; ★%%でハードウェアの生ステータスを見る
    (when (logbitp 0 status)
      (let ((raw-code (%%in-8 #x60))) ; ★%%で生のスキャンコードを引っこ抜く
        (let ((ascii-code (%parse-scancode raw-code))) ; ★%でLispが読める整数/シンボルに整える
          (%push-to-keyboard-buffer ascii-code))))))
```

### c における命名規則

C 側の命名規則(`sys_`/`os_`/`cc_`/`c_`)については README の「命名規則」を参照。

このうち `cc_`(C レイヤーで実装した Lisp の関数)は、実装している処理のレベルに応じて `os_set_function` により
`%%` または `%` プレフィックス付きのシンボルとして Lisp から呼び出せるように登録する。
`os_` は OS の内部実装であり、Lisp からは直接呼び出せない。
`c_` は手書きアセンブリの割り込みハンドラから呼ばれるための命名であり、Lisp への登録とは関係しない。

例
```c
lisp_val_t cc_in_8(lisp_val_t port) {
    //....
}

os_set_function(os_make_symbol("%%IN-8"), os_make_native_function((lisp_addr_t)cc_in_8), global_environment);
```


## 初期のマイルストン

- [x] GOPを仕様して画面に文字を表示する
- [x] キー入力を画面に表示する
  - キー入力は割り込みで実現する
- [x] LispObjectを定義する
- [x] 仮想バッファを持ち、複数のバッファを切り替えられるようにする
  - F1〜F4にそれぞれバッファを割り当てる
  - まずは各バッファに文字を表示し、F1〜F4で切り替えるのを確認する
- [x] プロセスを定義する
  - [x] プロセスに標準入力、標準出力を持たせる
  - [x] 標準入力はキー入力、標準出力はそのプロセスに紐付いているバッファとする

- [x] REPLの作成
  - [x] READ: `os_read` 関数
    - キーボード入力をリングバッファに積む
    - Enterキーで行を確定するまでは backspace で編集可能なラインエディットとする
    - 1行バッファには複数のS式を記入することができる
    - `os_read` は行バッファ内の読取カーソル位置を保持し、1回の呼び出しで1つの完全なS式だけを読み取ってカーソルを進める。
    - カーソルが行末に逹していなければ次回呼び出し時に新しい行の入力を待つ


プロセスごとに定義する内容のサンプル

```c
#define LINE_BUF_SIZE 256
static char line_buffer[LINE_BUF_SIZE];
static int64_t buffer_position = 0;

void c_keyboard_interrupt(int c) {
    if (buffer_position < LINE_BUF_SIZE) {
        line_buffer[buffer_position++] = c;
    }
}
```

`os_read` のサンプル
`read_string_literal` や `read_number` や `read_symbol` など BNF をそのままコードにしたようなコードとする

```c

static int64_t read_pos = 0;
lisp_val_t os_read() {
    if (read_pos >= buffer_position) {
        clear_line_buffer();
        return nil;
    }

    char c = peek_code();
    if (c == '(') {
        read_pos++;
        return read_list();
    } else if (c == ')') {
        return g_sym_read_error;
    } else if (c == '"') {
        read_pos++;
        return read_string();
    } else if (c == '\'') {

    }
    
    else {
        return read_symbol();
    }
}
```
  - [x] EVAL: `os_eval` 関数
    - STRING, FIXNUM は値をそのまま返す
    - SYMBOL は env から値を lookup して返す
    - CONS は car に応じた処理を呼ぶ
    - INSTANCE の表示内容は別途決める
    - cons は (eval-form (car exp) (cdr exp) env)を呼ぶ
    - eval-form
      - 初回は + と - だけ登録してある
  - [x] PRINT: `os_print` 関数
    - TAG に応じて表示する



## 最小のLispのためのマイルストン

以下のオペレータをcで実装する。

### special forms

- [x] quote
- [x] if
- [x] progn
- [x] setq
- [x] defun
- [x] lambda

### buit-in primitives

- [x] cons
- [x] car
- [x] cder

- [x] eq
- [x] null

### REPL primitives

- [x] read: ユーザの入力を文字ストリームから読み込み、S式を構築して返す
- [x] print: Lispオブジェクト(S式) を文字列に変換して画面へ出力する

### os level primitives

- [x] %%in-8
- [x] %%out-8
- [x] %%peek
- [x] %%poke


## LispでLispを実装するためのマイルストン

Lispファイルを外部から読み込むため9Pプロトコルを使う。
まずは QEMU の VirtIO-9p を使う。
将来的にNICドライバから9pサーバへアクセスできるようにする。

### QEMUのVirtIO-9pを使い、1ファイルをloadできるようにする

- [x] PCIバススキャンを実施し、VirtIO-9pデバイスを検出する
- [x] VirtQueue(DMA送受信リング)を作成する
- [x] Tversion -> Tattach -> Twalk -> Topen -> Tread を発行する
- [x] ホスト上の ./src/lisp/init.lisp をメモリ上に読み込めること
      REPL ではなく、C文字列バッファに値が入っていれば良いのでテスト用にバッファを確保する


### Streamの抽象化とload関数の実装

- [x] Stream構造体を作成する
- [x] `STREAM_9P_FILE` のような種別を作る
- [x] 9PのTreadをバッファリングして切り出す `stream_read_char` を実装
- [x] Lisp側に (load "init.lisp") を追加し、 9P stream から S 式を read -> eval する処理を実装


### Transport Layer のインタフェース分離

- [x] 9Pクライアントコードから VirtIO の API を直接呼ぶのをやめ、 `9p_transport_send()` / `9p_transport_recv()` という関数ポインタ抽象インタフェースを導入
- [x] VirtIO のコードを `transport_virtio_9p` としてカプセル化


### QEMU上でのNIC(e1000)ドライバ + Python 9Pサーバ

- [ ] ホスト側でPython製の9Pサーバを動かす
- [ ] Intel e1000 PCIドライバおよび最小限のTCPスタックを実装する
- [ ] transport interfaceをVirtIOからe1000経由に差し替える



