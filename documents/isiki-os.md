
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


## ISLisp を実装するために必要な機能について

つぎのオペレータを c レイヤーにて実装する

### 優先度高: 数値演算と基本型の判定

計算やループカウンター処理を書くため

- `+`, `-`, `*`, `/`
  - 整数演算
  - CPUの加減乗除命令を呼ぶため
- `<`, `>`, `=`
  - 数値比較
  - 制御構文の終了判定に必須
- `numberp`, `symbolp`, `consp`
  - 型判定プリミティブ
  - タグ付きポインタ/構造体の型チェック
- `fixnump`
  - 整数型判定
  - ISLisp規格の型チェック用


### 優先度高: 制御構造・大域脱出(ISLisp構文の土台)

ISLisp の for, while, with-open-input-stream などをLisp側のマクロで実装するために、
c レイヤーで以下の「スタック操作を行う特殊形式」が必要

- `unwind-protect`
  - 後処理の強制実行
  - with-open マクロがファイルクローズを保証するために不可欠
- `block` / `return-from`

### 優先度中: マクロシステムとシンボル操作の基盤

- `gensym`
  - ユニークなシンボル生成
  - マクロ展開時の変数名の衝突を防ぐ
- `macroexpand-`
  - マクロ展開
  - c レイヤーでマクロフックを扱うための関数
- `symbol-name` / `string-to-symbol`
  - シンボルと文字列の相互変換
  - reader や動的シンボル生成用

### 優先度中: ストリームI/O

- `open-input-stream` / `open-output-stream`
  - ストリーム生成
  - 9Pや画面/シリアルポートをLispのstreamオブジェクトとしてラップする
- `close`
  - ストリームの破棄
- `read-char` / `write-char`
  - 1文字入出力
  - 評価器の read や format の最下層
- `read`
  - S式のパース
  - c で実装した reader を lisp 関数として公開


## Lisp側にて実装すべきもの

- `let` / `let*`
- `for` / `while`
- `cond`
- `and` / `or`
- `with-open-input-stream`


## ISLisp の Lisp2スコープ対応

- function (#' syntax sugar)
  - あわせて #\a や #\Space なども対応するよう reader を修正する

- flet / labels
  - ローカル関数の作成

- defvar / defconstant / defdynamic / dynamic
  - defvar: レキシカルなグローバル変数
  - defconstant: 定数
    - 現在は定数がないため、 environment に定数用のフラグ等を付けて setq 等で更新できないようにする
  - defdynamic / dynamic: ISLisp 固有の動的変数

## データ構造の拡張

### ベクトル・配列
- make-array
- aref
- array-dimensions

### 文字列操作
- string-elt
- length
- create-string

### 高階関数・シーケンス関数
- mapcar, mapc, mapcan
- member, assoc
- append, reverse

### エラー処理とコンディショナルシステム
- signal-condition
- with-handler
- error
  - エラーを発生させて中断

## ILOS (ISLisp Object System)
- defclass
- defgeneric
- defmethod
- make-instance
- initialize-object
- call-next-method
- next-method-p
- class-of

## ISLisp仕様との差分(未実装機能)

`documents/islisp-v23.pdf` (ISLISP Working Draft 23.0) を仕様の一次ソースとして、
現在のisiki-osのLisp実装(C側のeval.c/runtime.c/reader.c + src/lisp/init.lisp)と
突き合わせたギャップ分析。チェックボックスは「未実装」を表す(このセクションの目的が
差分の可視化のため、原則すべて`[ ]`)。命名がISLisp仕様と異なるまま実装済みのもの
(例: `create`(仕様)に対して`make-instance`、`create-array`に対して`make-array`、
汎用の`elt`に対して`string-elt`)は各項目の note に記載する。

### classes (§10, Metaclass / predefined class)

現状の[ILOS](#ilos-islisp-object-system)実装は「クラスオブジェクトはユーザーが`defclass`で
作るもの」という前提のみで組み立てられており、仕様§10が規定するメタクラスと
predefined classの体系そのものが存在しなかった。クラスオブジェクト(旧`MAGIC_CLASS`,
`src/c/runtime.c`の`primitive_make_class_raw`)は`[name, supers, slots]`の3フィールドのみで、
「このクラス自身がどのクラスのインスタンスか」を保持する場所が無かった。

- [x] メタクラス`<standard-class>`/`<built-in-class>`の導入 — 新フィールドを追加する代わりに、
      `MAGIC_CLASS`を`MAGIC_BUILTIN_CLASS`/`MAGIC_STANDARD_CLASS`という2つのmagicタグに
      置き換えることで区別した(`src/c/runtime.h`/`runtime.c`)。`class-of`をクラスオブジェクトに
      適用すると`%%standard-classp`/`%%builtin-classp`で判定して`<standard-class>`/
      `<built-in-class>`を返す。`defclass`(`%%MAKE-CLASS-RAW`)は常に`<standard-class>`、
      predefinedクラスのbootstrap(`%%MAKE-BUILTIN-CLASS-RAW`)は常に`<built-in-class>`になる
- [ ] `defclass`の`:metaclass`クラスオプション対応 — `src/lisp/init.lisp`の`defclass`マクロは
      現在も`&rest options`を完全に無視している。`:metaclass`が指定された場合にそれを使う対応と、
      `<built-in-class>`が指定された場合はエラーにする仕様要件(spec 3081-3082行)は未実装
      (`defclass`は常に`<standard-class>`を使うため、この違反自体は構造的に発生しない)
- [ ] `create`(本実装では`make-instance`)で`<built-in-class>`のインスタンスを生成しようとした
      場合にエラーを発生させる(spec 1088-1089行、built-inクラスはサブクラス化・直接の
      インスタンス化ともに禁止)。既存の`create`/`make-instance`への影響範囲が広いため未対応
- [x] `<object>`クラスの実装 — predefined class階層(Figure 1)全体の頂点。`src/lisp/init.lisp`の
      predefinedクラスbootstrap(`%register-builtin-class`呼び出し群)で登録し、コンディション
      階層(`<error>`系)も含め全クラスがこの下に位置付けられるようになった
- [x] `<standard-object>`クラスの実装 — `defclass`でsupersを指定しなかった場合に暗黙的に付与
      されるスーパークラス(spec 3017行)。`%defclass-supers`が空supersを
      `(list (%find-class '<standard-object>))`にフォールバックするよう修正した
- [x] `<basic-array>` `<basic-array*>` `<basic-vector>` `<general-array*>` `<general-vector>`
      `<string>`のクラスオブジェクト化。`class-of`は`stringp`/`general-vector-p`/
      `general-array*-p`(既存述語)で判定して対応するクラスを返す。note: `<basic-array>`/
      `<basic-vector>`自体はクラス階層としては登録したが、`class-of`が実際にこれらを返す
      値は無い(常により特定的な`<string>`/`<general-vector>`/`<general-array*>`にマッチする)
- [x] `<character>`のクラスオブジェクト化
- [x] `<cons>` `<null>` `<list>` `<symbol>`のクラスオブジェクト化。`<null>`は仕様通り`<list>`と
      `<symbol>`の両方を直接のスーパークラスに持つ
- [x] `<number>` `<integer>` `<float>`のクラスオブジェクト化
- [x] `<function>` `<generic-function>` `<standard-generic-function>`のクラスオブジェクト化。
      note: クラス階層としては登録したが、`generic-function-p`が専用タグの不在により常にnilを
      返す既知の制約は変わらず、`class-of`が実際に`<generic-function>`/
      `<standard-generic-function>`を返す値は無い(総称関数も`<function>`止まり)
- [x] `<stream>`のクラスオブジェクト化
- [x] 既存のコンディション階層(`<error>`等)を正しく`<object>`の下に接続する。`<condition>`
      (仕様に無い独自クラス、後方互換のため残置)がsupers無しの`defclass`であることを利用し、
      `<standard-object>`フォールバック経由で自動的に`<object>`へ接続されるようにした
- [x] 公開`typep`関数がILOSインスタンス以外に対して常に`nil`を返す不整合の修正 —
      `class-of`が組み込み型も含めて汎用化されたことで、`typep`を
      `(subclassp (class-of instance) ...)`に一般化した。`(typep 5 '<integer>)`は`t`を返す。
      `%assure-typep`も個別case分岐を削除して`typep`に一本化した

### special operators (§12.3, 未実装の残り)

現在block/return-from/unwind-protect/if/progn/setq/lambda/flet/labels/function/
quote/quasiquote系/and/or/cond/for/while/with-handlerは実装済み。残りは以下。

- [x] setq: lambdaクロージャ内からの外側の変数書き換え — `os_set_variable`(current
      environment自身にしか書き込まない「frame定義」用)とは別に、`os_setq_variable`
      (envから親を順に辿り、既存の変数束縛を見つけて破壊的に上書きする「environment
      代入」用。どの親にも見つからない場合のみ`os_set_variable`にフォールバックしてローカル
      新規定義する)を新設し、`eval_setq`をこちらに切り替えた。これにより
      `(let ((x 0)) (let ((f (lambda () (setq x 99)))) (funcall f) x))`は`99`を返す。
      note: `os_is_constant`(defconstant定数へのsetqを禁止する判定)も同じ親チェーン探索に
      変更した。旧実装は現在のenv自身のconstantsスロットしか見ておらず、修正後の
      `os_setq_variable`が親envまで書き込みに到達できるようになった以上、ネストした
      クロージャ内からのsetqが外側スコープのdefconstant定数を素通りして書き換えて
      しまわないようにするために必要な変更(setq修正の副作用として発生する新たな
      正しさリスクを防ぐための追加修正であり、事前に明示合意した範囲ではないが
      必然的に必要な修正として実施)
- [x] assure — 式の値が指定クラスのインスタンスであることを検査する(違反時はdomain-errorを発生させる`the`のチェック版)。note: 組み込み型のクラスオブジェクトが揃ったため、個別の述語分岐を廃し`typep`に一本化した
- [x] case / case-using — キーをeql(またcase-usingでは任意の述語)で照合してクラウズを選ぶ多分岐。note: fixnum/symbol/characterがimmediate値表現のため、caseの照合はeq(existing memberの比較)でeql相当としている
- [x] catch / throw — 動的スコープのタグによる非局所脱出(block/return-fromは静的スコープなので別物)
- [x] class — クラス名からクラスオブジェクトを取得する特殊形式。note: predefinedクラスのbootstrapにより`<integer>`等の組み込み型のクラスオブジェクトも`%find-class`経由で取得できるようになった
- [x] convert — 数値/文字/シンボル/文字列/リスト/ベクタ間の型強制変換。note: 既存primitiveで組み立てられるsymbol<->string/string<->listのみ対応。仕様の変換表でlist->stringは「エラーを発生させる」と定められているため未対応であり、character<->integer等(char-code/code-char等が未実装)も対象外としてerrorになる
- [x] dynamic-let — bodyの間だけダイナミック変数を再束縛する
- [x] go / tagbody — タグ付きシーケンスとgoによるジャンプ(Common Lispのprog相当)
- [x] ignore-errors — bodyでエラーが発生したらnilを返して握り潰す
- [x] set-dynamic — ダイナミック変数への代入を行う特殊形式(現状は`%%SET-DYNAMIC`プリミティブのみで、専用構文になっていない)
- [x] the — 式の値のクラスを宣言する(assureと違い違反時のチェックはしない)。note: 型チェックはせずformの値を返すだけのno-op
- [x] with-error-output / with-standard-input / with-standard-output — 標準入出力/エラー出力ストリームをbodyの間だけ再束縛する。note: スコープ限定—動的変数(`*standard-input*`等)とアクセサ・再束縛マクロのみ実装。既存の`read`/`read-char`/`write-char`等のI/O primitiveは全て明示的にstream引数を取る実装のままで、省略時にこれらの動的変数を見るようには改修していないため、仕様例のような引数無し`(read)`は動作しない
- [x] with-open-input-file — ファイルをopenしbody終了時に自動closeする。note: 既存の`with-open-input-stream`と`open-input-stream`を組み合わせるだけの実装。オプションの`element-class`は評価はされるが無視する(既存`open-input-stream`が文字ストリーム固定のため)
- [x] with-open-output-file / with-open-io-file — ファイルをopenしbody終了時に自動closeする。note: このnoteは実装済みになった後も更新されずに残っていた旧記述。9PのTwrite/Tcreateによる書き込みtransport(`os_stream_open_9p_file_write`/`os_stream_open_9p_file_io`、streams/I/O/filesのセクション参照)が実装済みで、`open-output-file`/`open-io-file`は既に画面出力ではなく実ファイルの書き込み用9Pストリームを返す。`with-open-output-file`/`with-open-io-file`は既存の`with-open-output-stream`/`with-open-io-stream`と組み合わせるだけでそのまま動作する

### defining forms (§12.4)

- [x] defglobal — 可変なグローバル変数を定義する正式なISLisp形式。note: defconstantと同じ「value-formを評価してos_set_variableで登録する」形だが、os_mark_constantを呼ばないため定義後もsetqで書き換えられる。defvarと異なり既存束縛の有無は確認せず、再定義するたびに常にvalue-formを再評価する(defvarはCommon Lisp由来の非標準拡張として残置)

### 述語 (§13)

- [x] eql / equal — eqより厳密な値の一致(eql: 数値・文字の値比較)/構造的な同値性(equal)。note: 本実装ではfixnum/characterがimmediate値表現・symbolがinternされているため、eqlはeqと同じ判定になる。equalはcons/string/vectorを再帰的に内容比較するC側primitiveとして実装(任意階数のarrayに対する既存のLisp-levelなapply手段が無いため)
- [x] not — 論理否定。note: nullと仕様上完全に同一のため実体(C関数ポインタ)を共用する
- [x] listp / characterp / stringp / functionp / generic-function-p — クラス判定述語。note: generic-function-pは、defgeneric(下記オブジェクトシステムのセクション参照)がdispatch用の通常のinterpreted functionを生成するだけでgeneric functionを示す専用タグを持たないため、区別する手段が無く常にnilを返す
- [x] basic-array-p / basic-array*-p / basic-vector-p / general-array*-p / general-vector-p — 配列/ベクタ系のクラス判定述語。note: TAG_VECTORのrank(1次元かどうか)とTAG_STRINGの特別扱いだけで判定する外延ベースの実装。本実装には配列のサブタイプが無いためbasic-array*-pとgeneral-array*-pは実体を共用する
- [x] streamp / instancep — ストリーム判定述語 / 任意クラスへの所属を調べる汎用述語。note: instancepは既存のtypep(designatorもクラスオブジェクトも受け付ける)にそのまま委譲するLisp-levelなdefun(known limitation: 仕様通りのdomain-errorチェックは行わない)

### オブジェクトシステム (§15, ILOSセクションで既出のものを除く)

- [x] next-method-p — メソッド内でcall-next-methodが呼べる次のメソッドが存在するか調べる。note: 仕様上は`labels`と同様にメソッド本体にレキシカルに束縛されるが、本実装は`*next-methods*`という動的変数によるフレームスタック(with-handlerの`*handlers*`と同じpush/unwind-protectパターン)で代替する
- [x] initialize-object — createがインスタンス生成時に呼ぶ、initargからスロットを埋める総称関数。note: `make-instance`(仕様上の`create`)は自前でスロットを埋めず、この総称関数を呼ぶように改修済み。ユーザーはクラスを指定した`defmethod initialize-object`でオーバーライド/`call-next-method`による拡張ができる
- [x] class-of — オブジェクトが直接属するクラスを返す。note: ILOSのクラスインスタンスだけでなく、クラスオブジェクト自身(メタクラス判定)や組み込み型の値(整数・文字列・シンボル等)も対象になるよう汎用化した
- [x] instancep — 任意クラスに対するインスタンス判定(上の述語セクションと重複掲載、実装済み)
- note: 仕様上のインスタンス生成関数名は`create`だが、現状は`make-instance`という名前で実装されている
- note: `defgeneric`/`defmethod`/`call-next-method`は本タスクで新規実装した(既知の簡略化: 単一(第1引数)dispatchのみで多重ディスパッチは未対応(第2引数以降にspecializerを書くとdefmethod展開時にerror)。`:before`/`:after`/`:around`等のmethod-qualifierは未対応でprimary methodのみ。メソッドの特定性順序はspecializerクラス間の`subclassp`比較による簡易な挿入ソートで、真のクラス優先度リスト(MRO)計算は行わない(defclassのスロット継承が単純な連結であることと同水準の簡略化)。dispatch/apply用に内部primitive`%%apply`(`src/c/eval.c`、既存`funcall`と同型)を追加した)

### 宣言と型強制 (§17)

- [x] the / assure / convert — 上の special operators と同じ(§12.3/§17両方に現れる)

### symbol class (§18)

- [x] property / set-property / remove-property — シンボルに紐づくプロパティリストの読み書き・削除。note: symbolのC側構造体にproperty list用フィールドが無いため、`*classes*`/`*generic-methods*`と同じ「外部のdefdynamicグローバルにentryを持つ・再定義は既存entryを取り除いた上で前に積む」パターンで実装した(entryは`((symbol . property-name) . value)`、symbol/property-nameの比較はinternされていることを前提にeqで行う)。symbol/property-nameがsymbolでない場合は`<domain-error>`が未実装のため、`assure`/`convert`と同じく通常のerrorで代替する。`(setf (property symbol property-name) obj)`も`setf`マクロにplaceパターンを追加して対応した

### number class (§19)

現状+, -, *, /, <, >, =のみ実装。加えてreaderが浮動小数点数(float)のリテラル自体を
サポートしていない — これは個々の関数が足りないというより、数値タワーの土台
(float class)がまだ存在しないというアーキテクチャ上のギャップ。

整数(integer)は符号付きFIXNUM(60bitマグニチュード+1bit符号、tag下位3bitを除く
bit3〜62がマグニチュード・bit63が符号)と、60bitを超える値を表すbignum
(既存TAG_INSTANCEにMAGIC_BIGNUMを追加する形。符号+可変長limb配列(基数2^32)を
別途ヒープ確保して保持)の2階層で実装した。bignumの加減乗除・比較は素朴なアルゴリズム
(O(n*m)乗算、1bitシフト&サブトラクトの長除算)を採用しており、速度は最適化していない
(正しさ・実装の単純さを優先)。+/-/*//および</>/=は全オペランドがFIXNUMかつ演算結果が
60bit以内に収まる場合のみ既存の高速な即値演算パスを使い、それ以外(bignumが混在、
負数が絡む、桁あふれの恐れがある)は符号+limb配列に分解する一般パスに切り替える
二段構成にした。eql/equalもbignum同値比較(sign+limb内容の比較)に対応済み。

整数リテラルのreader構文は仕様書(§19.3、Integers are written in one of the following
formats)通り、10進の明示的な`+`符号(`+42`)と`#b`/`#B`(2進)・`#o`/`#O`(8進)・
`#x`/`#X`(16進)のradix表記(符号付き含む)にも対応した。radix表記でもマグニチュードが
60bitを超えればbignumに昇格する。

浮動小数点数(float)対応を実施済み。当初の調査では、実機カーネルのビルドが
`-mgeneral-regs-only`(SSE命令禁止)付きでリンクするため、C側で`double`の四則演算・
比較演算子を素朴に使うと`__adddf3`/`__ltdf2`/`__gtdf2`等のlibgcc soft-float関数への
未定義参照でリンクエラーになることを確認していた(`make compile`はコンパイルのみで
リンクしないためこの問題を検出できない)。最終的にカーネル起動時にFPU/SSEを
有効化する方式(hardware float)を採用した: `init_fpu`でCR0/CR4のFPU/SSE関連
ビットを設定し`-mgeneral-regs-only`をMakefileから削除、タイマー割り込みハンドラ
(`asm_timer_handler`)にGPR15個のpush/popの外側でFXSAVE/FXRSTORを追加し、
プロセス切り替え時にx87/SSEレジスタ(xmm0-15/MXCSR含む)も保存・復元するようにした。
float本体はTAG_INSTANCE+MAGIC_FLOATのボックス化表現(word1にdoubleのbit patternを
格納)。floatp/float/+/-/*//および比較演算子/eqlをfloat対応に拡張し、readerに
float literal構文(`1.5`, `-2.0E10`, `3E5`等、§19.2準拠)、printerにfloatの
10進文字列変換(有効17桁・固定小数点/E表記の自動選択)を実装した。
FXSAVE/FXRSTORの正しさはQEMU上でプロセスをまたいだ長時間(数万tick)の
プリエンプティブ切り替えを経ても壊れないことを確認済み。

- [x] 浮動小数点数(float)のリーダ構文とfloatクラスそのもの
- [x] /= / >= / <= — 残りの数値比較。既存の</>/=と同様に可変長引数の
      隣接ペア連鎖判定として実装しており、`/=`は仕様上の「全引数が相異なる」ではなく
      「隣接ペアがすべて等しくない」という簡略化になっている点に注意
      (例: `(/= 1 2 1)`は仕様上nilだが、隣接ペア判定でもnilになるため実害は薄いが、
      3引数以上での意味論は厳密には仕様と異なる)
- [x] quotient / reciprocal — 除算・逆数。`quotient`は両オペランドが整数かつ割り切れる場合のみ
      整数を返し、それ以外はfloatに変換して`/`を使う(`init.lisp`のLisp合成関数)。`reciprocal`は
      `(quotient 1 x)`
- [x] max / min / abs — 最大・最小・絶対値
- [x] exp / log / expt / sqrt — 指数・対数・べき乗・平方根。`sqrt`(SSE2 `sqrtsd`、完全平方の
      整数はfloatを経由せず整数のまま返す)と`log`(自然対数、x87 `fyl2x`)はCプリミティブとして
      新規実装し、`domain-error`発火(後述)の対象。`exp`は`f2xm1`+`fscale`による引数域分割で
      Cプリミティブ実装(domain-errorなし、numberでない引数への型チェックも未対応で既存の
      `g_sym_eval_error`の簡略化のまま)。`expt`は整数底・非負整数指数を`*`の繰り返し二乗法
      (`init.lisp`)、それ以外は`(exp (* x2 (log x1)))`への合成(`x1=0`絡みの特殊ケースのみ
      `error`)として実装
- [x] sin / cos / tan / atan / atan2 / sinh / cosh / tanh / atanh — 三角関数・双曲線関数。
      `sin`/`cos`(x87 `fsin`/`fcos`)/`atan2`(x87 `fpatan`)はCプリミティブ、それ以外
      (`tan`/`atan`/`sinh`/`cosh`/`tanh`/`atanh`)は`sin`/`cos`/`atan2`/`exp`/`log`からの
      Lisp合成(`init.lisp`)。`atanh`は専用のdomain-errorコードを書いておらず、
      `|x| >= 1`のとき合成先の`log`へ0以下の値が渡ることで`log`のdomain-errorが
      そのまま伝播し、結果的に仕様(4658-4669行)が要求する`atanh`のdomain-errorを
      副産物として満たしている
- [x] *most-positive-float* / *most-negative-float* — IEEE754 binary64のDBL_MAX/-DBL_MAX相当を
      `init.lisp`でfloat literalとして`defconstant`(当初`defdynamic`で実装していたが、
      `defdynamic`の値は`(dynamic name)`経由でしかアクセスできず、bareなシンボル参照は
      未定義変数アクセスになってしまうため`defconstant`に修正した。仕様の
      「named constant」という位置づけにも`defconstant`の方が合致する)
- [x] *pi* — 数値関連の名前付き定数。上記と同じ理由で`defconstant`として実装
- [x] float — integer/bignum/floatから floatへの変換
- [x] floor / ceiling / truncate / round — floatとintegerの相互変換・丸め。x87の制御ワードの
      丸めモードビット(RC)を関数ごとに切り替えて`frndint`を使うCプリミティブとして実装
      (`round`はties-to-even、仕様の「halfwayはeven」要件と一致)。結果のdoubleは新規の
      `double_to_integer`ヘルパー(IEEE754のsign/exponent/mantissaを分解し`os_make_integer`に
      渡す)でfixnum/bignumに変換する。引数がもとから整数の場合はFPUを経由しない高速パスがある
- [x] integerp — 整数クラス判定。`(or (fixnump obj) (bignump obj))`として実装
- [x] div / mod / gcd / lcm / isqrt — 整数演算。`div`/`mod`は仕様(§19.4)通りfloor除算
      (`/`の切り捨て除算とは異なり、商は`-∞`方向に丸め、余りは除数と同じ符号になる)
      として実装した。0除算・負数の`isqrt`は他の整数演算の0除算(`runtime.c`の`/`など)
      と同様、条件システムではなく`g_sym_eval_error`をそのまま返す方式に合わせている
- [x] parse-number — 文字列を数値としてパースする。`reader.c`に既存の`process_source_*`と
      同型の`string_source_*`を新設し、既存の`read_atom`ロジックを文字列バッファに対して
      流用する。読み取り結果が数値かつ文字列を最後まで消費していれば返し、そうでなければ
      `<parse-error>`(error-id `cannot-parse-number`)をsignalする

#### domain-errorの対応範囲

C primitiveから評価器(Lisp側の`signal-condition`)を呼び戻す土台
(`os_apply_function`/`os_is_control_transfer`をeval.cから公開、`os_signal_condition`を
runtime.cに新設)を今回追加したが、domain-errorの発火は明示的に指示された以下の3ケースのみに
絞っている(「型は合っているが値が違う」場合):

- `sqrt` に負の数を渡す
- `log` に 0以下の数値を渡す
- `asin` / `acos` に定義域`-1.0`〜`1.0`の範囲外の値を渡す(`atanh`は上記の通り`log`への
  委譲で副次的にdomain-errorになる)

`exp`/`expt`/`sin`/`cos`/`tan`/`atan`/`atan2`/`sinh`/`cosh`/`tanh`/`floor`/`ceiling`/
`truncate`/`round`/`quotient`/`reciprocal`の「引数がnumberでない」場合のdomain-error化は
今回のスコープ外とし、既存の`g_sym_eval_error`/`error`の簡略化のままにしている。

#### 既知の未検証事項

- `sin`/`cos`のx87 `fsin`/`fcos`は引数が大きいほど精度が落ちる既知の制限があり、
  大きな引数での精度は未検証
- `atan2`の符号付きゼロに関する仕様上の特殊ケース(仕様図3相当)の完全一致は未検証
- QEMU実機上でのREPL対話確認は、このカーネルがフレームバッファ+キーボード入力のみで
  シリアルコンソール出力を持たないため、ヘッドレスな自動テストからは実施できていない
  (`make test`のユニットテストでのみ検証済み)

### character class (§20)

readerの`#\c`リテラルは実装済み。

- [x] characterp — 文字クラス判定(述語セクションと重複掲載、実装済み)
- [x] char= / char/= / char< / char> / char<= / char>= — 文字の比較。CHARは即値
      (上位ビットに文字コードを格納)のため、`(UINT8)(val >> 3)`で取り出した
      ASCIIコード同士を比較するだけで実装できる。数値比較の`/=`/`>=`/`<=`と同様、
      仕様上は2引数だが既存の実装方針に合わせて可変長引数の隣接ペア連鎖判定に
      一般化しており、`char/=`も「隣接ペアがすべて等しくない」という簡略化になる。
      大文字・小文字は区別する(`(char= #\a #\A)`はnil)

### list operations (§21.3)

- [x] create-list — 指定した長さのリストを(初期値を詰めて)確保する。`initial-element`は
      仕様上省略可(省略時の値はimplementation defined)で、本実装では省略時はnilを詰める
- [x] nreverse — reverseの破壊的版。新しいconsは確保せず、既存のconsのcdrを
      `set-cdr`で書き換えて反転する
- [x] maplist / mapl / mapcon — mapcar/mapc/mapcanの、要素ではなく後続のsublistに
      関数を適用する版。`mapcon`は仕様上は`nconc`による破壊的な結果連結だが、
      `nconc`自体が未実装のため既存の`mapcan`(こちらも`append`で代用済み)と同様の
      簡略化として`append`で連結している
- [x] reader: ドット対記法`(a . b)`のサポート — `read_list`(`src/c/reader.c`)が単独
      トークンの`.`(`g_sym_dot`として読まれるシンボル)を特別扱いしないまま通常の
      symbolとして読んでいたため、`'(one . 11)`が「`.`という名前のsymbolを含む3要素の
      普通のlist」になっていた問題を修正。`read_list`の2番目以降の要素読み取りを
      `read_list_rest`に分離し、要素が`g_sym_dot`と一致したら直後の1式をcdrとして
      採用し閉じ括弧のみを許可するようにした。リスト先頭の単独`.`(`(. a)`)は構文エラー。
      ベクタリテラル`#(...)`は元々dotted pairを想定しない実装(`os_make_vector_from_list`
      が`nil`終端を前提にcdrを辿る)のため、`is_proper_list`で正規のリストであることを
      確認してから渡すガードを追加した(`#(1 . 2)`は構文エラーになる)

### arrays / vectors (§22, §23)

- [x] garef / set-garef — general-arrayに限定したaref/set-aref。本実装には配列の
      サブタイプが無く(既存のbasic-array*-p/general-array*-pと同様に外延が一致する)、
      domain-errorを要求する条件系も未実装のため、aref/set-arefと同一のC関数を
      別シンボル名で登録して共用している
- [x] vector / create-vector — ベクタを構築する専用関数。`create-vector`の
      initial-element省略時の初期値は仕様上「implementation defined」のため、
      既存のmake-arrayのnil初期化に倣ってnilとした
- [x] `#(...)` — ベクタリテラルのreader構文。`print_vector`の出力形式と対称
- note: 仕様上の配列生成関数名は`create-array`だが、現状は`make-array`という名前で実装されている
- note: VECTORの内部表現を専用タグ`TAG_VECTOR`(0x6)から、他のオブジェクト種別
      (function/stream/class/bignum等)と同じ`TAG_INSTANCE`(0x5)+新規`MAGIC_VECTOR`
      の組み合わせへ移行した。目的はタグ空間(下位3bit、8種類)のうち2つ
      (0x6・0x7)を将来のために予約として空けておくこと

### string class (§24)

- [x] string= / string/= / string< / string> / string<= / string>= — 文字列の比較。
      char=等と同様、仕様上は2引数だが本実装では隣接ペア連鎖のN項関数として実装した
- [x] char-index / string-index — 文字列内の文字・部分文字列の位置検索。
      start-positionは省略可(省略時0)、空文字列の部分文字列探索は探索開始位置に即マッチする
- [x] string-append — 文字列の連結
- note: 仕様上のアクセサ名は(sequence共通の)`elt`だが、現状は`string-elt`という文字列専用の名前で実装されている

### sequence functions (§25)

- [x] elt / set-elt — list/string/vectorに共通の汎用アクセサ(現状string-elt/arefのように型別にしか無い)
- [x] subseq — 部分列の取得
- [x] map-into — 複数列に関数を適用した結果を指定した列に破壊的に詰める

### streams / I/O / files (§26-28)

9PのTwrite/Tcreateに対応し、ファイル書き込み・文字列ストリーム・formatを実装済み。

- [x] streamp / open-stream-p / input-stream-p / output-stream-p — ストリームの状態・種別判定
- [x] standard-input / standard-output / error-output — note: アクセサ関数と`with-standard-*`マクロは実装済みだが、デフォルト値は変更せず`nil`のままにしている。`defdynamic`が単一グローバルalist(プロセス単位ではない)であるため、実ストリームをデフォルト値にすると評価した1プロセスの入出力に固定され、他プロセスの入出力に誤って繋がってしまうことを避けるための意図的な選択
- [x] open-input-file / open-output-file / open-io-file — 9PのTwrite/Tcreateを実装し、ファイルストリームの読み書き・新規作成に対応
- [x] finish-output — バッファされた出力をフラッシュする
- [x] create-string-input-stream / create-string-output-stream / get-output-stream-string — 文字列を裏付けとするストリーム
- [x] preview-char / read-line / stream-ready-p — note: stream-ready-pは非同期I/Oが無く常に同期的にブロックするため、常にtrueを返すスタブ
- [x] format とその補助関数(format-char / format-float / format-fresh-line / format-integer / format-object / format-tab) — `~`指令によるフォーマット出力。note: floatが未実装のため、format-float/`~G`は整数値を「整数+".0"」として近似出力する
- [x] read-byte / write-byte — バイナリストリームの入出力
- [x] probe-file / file-position / set-file-position / file-length — note: 9PにTstat/Tgetattrが無いため、probe-fileは「開けるかどうか」、file-lengthは「先頭から読み切ったバイト数」で近似している

### condition system (§29, with-handler/signal-condition/error以外の残り)

仕様のコンディションクラス階層(§29, spec図)通りに`<serious-condition>`以下を実装し、
cerror/report-condition/condition-continuable/continue-conditionと各クラスのアクセサ関数群を
すべて追加した。

note: C側の既存エラー発生箇所(ゼロ除算・未束縛変数・未定義関数・ストリームエラー等)を
これらのconditionクラスへ実際に繋ぎ直すことはスコープ外(C primitiveから評価器を呼び戻す
仕組みが現状無いため)。既存の`g_sym_eval_error`によるふるまいは変更していない。
`<floating-point-overflow>`/`<floating-point-underflow>`は浮動小数点数自体が未実装のため、
発生源を持たない仕様追従のためのクラス定義のみ。

- [x] `<serious-condition>` — error / storage-exhaustedの共通の親クラス
- [x] `<arithmetic-error>` とそのサブクラス`<division-by-zero>` / `<floating-point-overflow>` / `<floating-point-underflow>` — 数値演算エラー
- [x] `<control-error>` — 不正な非局所脱出(catchタグ不在など)
- [x] `<parse-error>` — テキストが期待クラスとしてパースできないエラー
- [x] `<program-error>` とそのサブクラス`<domain-error>` / `<undefined-entity>`(さらにその下の`<unbound-variable>` / `<undefined-function>`) — 引数の型不一致・未定義参照系のエラー
- [x] `<stream-error>` とそのサブクラス`<end-of-stream>` — ストリーム関連エラー
- [x] `<storage-exhausted>` — メモリ確保失敗
- [x] cerror — continuableなエラーを発生させる(現状errorはcontinuable指定ができない)
- [x] ignore-errors — (special operatorsの節と重複掲載)
- [x] report-condition — コンディションを人間向けメッセージとしてストリームに出力する総称関数
- [x] condition-continuable / continue-condition — continuable判定とcontinue時の再開
- [x] with-error-output — (special operatorsの節と重複掲載)
- [x] 各コンディションクラスのアクセサ関数群 — arithmetic-error-operation/-operands、domain-error-object/-expected-class、parse-error-string/-expected-class、simple-error-format-string/-format-arguments、stream-error-stream、undefined-entity-name/-namespace

### miscellaneous (§30)

- [x] identity — 引数をそのまま返す
- [x] get-universal-time / get-internal-run-time / get-internal-real-time / internal-time-units-per-second — 時刻・経過時間の取得。`get-universal-time`はUEFI `GetTime()`を起動時に1回呼んで取得したUTCに、PIT tickカウンタ(約100Hz)による起動後経過秒数を加算する方式。`GetTime`が失敗した場合は起動時刻を0(=1900-01-01)として扱う(エラーをLisp条件系へsignalする仕組みは無いため、C側呼び戻し機構が無い制約はスコープ外)。`EFI_TIME`の`TimeZone`は無視しUTCとして扱う。`get-internal-run-time`はプロセス単位のCPU時間計測が無いため`get-internal-real-time`と同じtickカウンタを流用する簡略実装。


## isikiOS 独自の機能

### 環境

- [ ] make-environment
- [ ] in-environment


## GCのCコード実行中への拡張(shadow stack)

### 現状の制約とモチベーション

現在のGC(Cheney方式のコピーGC、`os_gc_collect` @ `src/c/runtime.c`)は、
**トップレベルform間のセーフポイント**(`os_repl_step` @ `src/c/repl.c`、
`os_eval_top_level`の呼び出しが完全に終わった後、次の`os_read`より前)からしか
呼ばれない。この時点では`os_eval_top_level`/`os_read`/`os_print`のCフレームは
すべて戻り切っており、lisp_val_tを保持したCスタックフレームは存在しないため、
ルート集合はグローバル状態(`global_environment`・`g_dynamic_bindings`・
`g_symbol_table`・キャッシュ済み`g_sym_*`・`os_gc_register_root`で登録した
`proc->env`)だけで足りていた。

一方`os_alloc_bytes`(`src/c/runtime.c`)の「out of memory」分岐には
`// TODO: GCを呼ぶ`が残っている。これを実現するには、**割り当てが失敗した瞬間、
つまり`os_eval`の再帰的な呼び出しの奥深く(`eval_args` → `os_eval` → `eval_form` →
`apply_function` → …)からでも安全にGCを起動できる**ようにする必要がある。

`eval.c`はCの再帰下降インタプリタで、ほぼ全ての関数が評価済みのlisp_val_tを
1〜3個ローカル変数として保持し、その後の再帰呼び出し(`os_eval`/`eval_progn`等)
の**後**でその変数を使う、という形をしている。

```c
// eval.c: eval_args (抜粋)
static lisp_val_t eval_args(lisp_val_t args, lisp_val_t env) {
    lisp_val_t head = os_eval(cc_car(args), env); // head を評価
    lisp_val_t tail = eval_args(cc_cdr(args), env); // 再帰呼び出しの間、head は生きている
    return os_make_cons(head, tail); // 呼び出しから戻った後で head を使う
}
```

このような`head`のようなローカル変数は、GCがCコールスタックを覗く手段を
持たない限りルート集合から漏れる。これを解決するのが以降で説明する
shadow stack方式である。

### shadow stackの構造

Cの関数に入ったときにローカル変数をリスト(shadow stack)の先頭に繋ぎ、
関数を抜けるときに外す。ノード自体はCスタック上(呼び出し元の関数の
ローカル変数として)に確保するため、専用のヒープ領域は不要。

```c
typedef struct _gc_rootnode {
    lisp_val_t *var_ptr;       // 保護したいローカル変数へのポインタ
    struct _gc_rootnode *next; // 1つ外側のフレームで繋いだノード
} gc_rootnode;
```

### shadow stackのトップポインタはプロセス単位で持つ

isikiOSはfreestandingなベアメタルカーネルであり、Cコンパイラ標準の
`__thread`(TLS)を支える機構自体が存在しない。isikiOSのプロセスは
`PROCESS_COUNT`個の固定グリーンスレッドで、タイマー割り込み駆動の
プリエンプティブなコンテキスト切替(`c_timer_switch` @ `src/c/interrupt.c`)
により実行が切り替わる。既存の`proc->env`(`process_t`のフィールド、
`os_gc_register_root`で登録)と同じ考え方で、shadow stackのトップも
`process_t`のフィールドとして持つ。

```c
// process.h の process_t に追加するフィールド(イメージ)
typedef struct {
    // ...既存フィールド...
    gc_rootnode *gc_roots; // このプロセスのshadow stackの先頭
} process_t;
```

`process_t`のフィールドはレジスタではなくメモリ上のデータなので、
`saved_rsp`のようにコンテキスト切替時に明示的な保存・復元コードを
`c_timer_switch`側に追加する必要はない。各プロセスの`gc_roots`は、
そのプロセスが実行を再開したときにも自然に正しい値を指している
(ノード自体もそのプロセス専用のCスタック上に存在し続けるため)。

### GC_PROTECT/GC_UNPROTECTマクロ

ユーザー案のイメージそのままに手動で`GC_UNPROTECT()`を呼ぶ実装にすると、
`eval.c`の各関数は非局所脱出(block/return-from・catch/throw・tagbody/go)の
シグナル値(`MAGIC_BLOCK_EXIT`等)を伝播させるために複数のreturn文を持つため
(isikiOSは`setjmp`/`longjmp`が使えないfreestanding環境であり、脱出は
シグナル値をCの戻り値として各段で伝播させる方式を取っている)、
どこかのreturnで`GC_UNPROTECT()`を呼び忘れるとshadow stackの対応が
ずれてしまう。

```c
// eval.c: eval_args は途中で3箇所からreturnする(抜粋)
static lisp_val_t eval_args(lisp_val_t args, lisp_val_t env) {
    if (args == nil) {
        return nil;                          // 1
    }
    lisp_val_t head = os_eval(cc_car(args), env);
    if (is_control_transfer(head)) {
        return head;                         // 2
    }
    lisp_val_t tail = eval_args(cc_cdr(args), env);
    if (is_control_transfer(tail)) {
        return tail;                         // 3
    }
    return os_make_cons(head, tail);         // 4
}
```

このため、対応するマクロはGCC/Clangの`__attribute__((cleanup(...)))`拡張を
使い、スコープを抜けるタイミング(どのreturn文経由でも)で自動的に
unprotectされるようにする。isikiOSのビルドは`gcc`/`x86_64-w64-mingw32-gcc`
(`-std=c11`, `Makefile`)を使うため、この拡張はfreestandingでも
(ライブラリ依存なしに)そのまま使える。

```c
static void gc_unprotect_node(gc_rootnode *node) {
    get_current_process()->gc_roots = node->next;
}

#define GC_PROTECT(var) \
    gc_rootnode _gcnode_##var __attribute__((cleanup(gc_unprotect_node))) = \
        { (lisp_val_t *)&(var), get_current_process()->gc_roots }; \
    get_current_process()->gc_roots = &_gcnode_##var
```

`GC_UNPROTECT()`は明示的には呼ばない(スコープを抜けるときに自動で
呼ばれる)。使用イメージ:

```c
lisp_val_t eval_add(lisp_val_t args, lisp_val_t env) {
    lisp_val_t a = nil;
    lisp_val_t b = nil;

    GC_PROTECT(a);
    GC_PROTECT(b);

    a = eval(car(args), env);
    b = eval(car(cdr(args)), env);

    // どこでreturnしても、スコープを抜ける際に a, b は自動的に
    // shadow stackから外れる(LIFO順、宣言と逆順にcleanupが走る)

    return make_fixnum(a + b);
}
```

`GC_PROTECT`する変数は、登録前に必ず`nil`など有効なlisp_val_tで
初期化しておく(未初期化のままだとGCが不正な値をスキャンしてしまう)。
`node->var_ptr`は変数への**ポインタ**であり、値そのものではないため、
`a = eval(...)`のように後から再代入しても再登録は不要(スキャン時に
`*var_ptr`を読むため常に最新の値を見る)。これは既存の`g_sym_quote`等の
キャッシュ済みシンボル変数が`os_gc_collect`から直接書き換えられる仕組みと
同じ考え方である。

### ルート集合スキャンへの統合

`os_gc_collect`(`src/c/runtime.c`)には、既存のグローバルルート
(`global_environment`・`g_symbol_table`・`g_sym_*`・`g_gc_extra_roots`)に
加えて、**全プロセス**のshadow stackを辿ってスキャンするループを追加する。

```c
// os_gc_collect 内、既存のグローバルルートのコピーに続けて
for (UINT32 i = 0; i < PROCESS_COUNT; i++) {
    for (gc_rootnode *node = g_processes[i].gc_roots; node != NULL; node = node->next) {
        *node->var_ptr = gc_copy_value(*node->var_ptr);
    }
}
```

**自分のプロセスだけでなく全プロセスを辿る必要がある**: isikiOSの
プロセス切替はタイマー割り込みによるプリエンプションであり、GCが
起動した瞬間、実行中でないプロセスもCスタックの途中(evalの再帰の中)で
停止しているだけで、そのCフレームが保持するlisp_val_tは依然として
生きている。自プロセスのshadow stackしか見ないと、他プロセスが
保持中の値を取り逃して破壊してしまう。

既存の`os_gc_register_root`/`g_gc_extra_roots`(`proc->env`用)はこの
仕組みとは別に併存させる。`env`はCコールスタックのスコープに縛られない
プロセスの永続フィールドであり、shadow stackのような一時的なルートとは
性質が異なるため、既存のまま変更しない。

### 安全上の不変条件

- GC_PROTECTされた変数だけがGCの移動に追随する。**タグ付きの生ポインタ
  (`addr & ~TAG_MASK`で得た`UINT64*`)を、アロケーションが起きうる呼び出しを
  またいで保持してはならない**。コピーGCはオブジェクトを移動させるため、
  GC_PROTECTされていない生ポインタはGC後に無効なアドレスを指してしまう。
  現在の`eval.c`はすでに、オブジェクトのフィールドを読み終えてから
  アロケーションする関数を呼ぶ、という順序を守っている(`apply_function`
  で`params`/`body`/`closure_env`を先に読み出す等)。GCがCコード実行中にも
  起動できるようになると、この順序は「たまたま安全」ではなく**必須の
  不変条件**になる。
- GC_PROTECTする対象は、他のCの呼び出し(GCを誘発しうる`os_eval`/
  `eval_progn`/`apply_function`/`os_make_*`等のアロケーション関数)を
  またいで生存するlisp_val_tローカル変数すべてである。`eval.c`のほぼ
  全ての関数(`eval_args`/`eval_form`/`apply_function`/`eval_catch`等)が
  該当する。

### スタック使用量への影響

各プロセスは固定16KBのスタック(ガードページなし、`process.c`の
`STACK_SIZE`)を持つ。`eval.c`の再帰の深さに比例してshadow stackの
ノード(GC_PROTECTされた変数と並んでCスタック上に確保される)も
積み重なるため、フレームあたりのスタック使用量がわずかに増える。
もともとevalの再帰には深さ制限もガードページも無いため、深いLisp再帰は
既にスタック溢れのリスクを抱えている。shadow stack導入はこのリスクを
悪化させる方向に働くため、実装時にはスタック使用量の実測(または
`STACK_SIZE`の見直し)を合わせて検討する。

### 今後の課題

- [x] `eval.c`の各関数への`GC_PROTECT`の機械的な適用
- [x] `process_t`への`gc_roots`フィールド追加と初期化
- [x] `os_gc_collect`への全プロセススキャンループの追加
- [x] `os_alloc_bytes`の「out of memory」時に実際にGCを起動する変更
      (`// TODO: GCを呼ぶ`の解消)
- [x] `runtime.c`のビルトイン関数群(`primitive_*`/`cc_*`)への`GC_PROTECT`の適用
- [x] bignum/vectorの生バッファに対する「確保直後に包む」規律の適用

### 生バッファ(タグなし)を扱う関数を書くときの規律

`os_alloc_bytes`のOOM時に`os_gc_collect`を実際に呼ぶようにしたことで、
**ヒープを確保するあらゆる呼び出しが、その場でGCを起動しうる**ようになった。
`GC_PROTECT`はタグ付きの`lisp_val_t`ローカル変数しか追跡できないため、
`os_alloc_bytes`が返す生の`UINT64*`/`lisp_addr_t`(bignumのlimb配列、
vectorのdata部など、まだどのタグ付きオブジェクトからも到達不能なメモリ)は
別の規律が必要になる。

Cheney方式のコピーGCは、collectが起きても**現在アクティブな半分(from空間)
のバイト列自体は書き換えない**(live objectをto空間へコピーするだけ)。
そのため、生バッファへのポインタを1回のアロケーションだけ挟んで
読み書きする分には(たまたま)安全である。しかし、**2回目以降の
collectが起きると、その2回目のcollectのbump allocationが、まさに
1回目のcollectで用済みになった側の半分(今はidleなto空間)へ書き込む
ため、そこに置いたままの生バッファは上書きされて破壊される**。
`mag_gcd`/`mag_isqrt`のような無制限ループや、四則演算のループ
アキュムレータのように、イテレーションごとに新しい生バッファを
確保し続ける関数は、この2回目以降のcollectで実際に事故が起きる
(1引数や2引数の単純な呼び出しでは再現しにくいが、`(+ ...)`に
bignumを3個以上渡す、`gcd`/`isqrt`で反復回数の多い入力を渡す、
といった呼び出しで顕在化する)。

このため、新しく`os_alloc_bytes`で確保した生バッファ/生アドレスは、
**次にアロケーションを伴う呼び出しをする前に、必ずタグ付きの
到達可能な値(`os_make_integer`/`os_make_instance`等)に包んで
`GC_PROTECT`すること。生ポインタを後から信用せず、包んだ後の値
から(`decompose`等で)再取得すること**を徹底する。具体的な型ごとの
パターン:

- bignumのlimbバッファ: `os_make_integer(sign, limbs, count)`で
  MAGIC_BIGNUMのINSTANCEに包み、その戻り値を`GC_PROTECT`する。
  以後limbsを使う箇所は、包んだ値に対して`decompose()`を呼び直して
  新しいポインタを取得する(`decompose`が返す`.limbs`は`word3`への
  生ポインタであり、それ自体はアロケーションを挟むと追従しない)。
- vectorのdata部(`alloc_vector_block`の戻り値): アロケーションを
  挟まずに要素を書き込み切れる場合は、そのまま`os_make_instance`で
  包めば安全(`os_make_vector_from_list`/`primitive_create_vector`/
  `primitive_make_array`/`primitive_subseq`のVECTOR枝はいずれもこの
  パターン)。書き込みの途中でさらにアロケーションが必要になる場合は、
  bignumと同じく先に包んでから書き込む。

一方、`os_make_cons`/`os_make_instance`のような**リーフの確保関数
自身**は、渡された引数(car/cdr、w1/w2/w3)を、自分の内部で行う
唯一のアロケーション(`os_alloc_bytes`呼び出し1回)の直後に、それ以上
アロケーションを挟まずすぐ使い切っている。これは前段落の「1回だけの
アロケーションはたまたま安全」に該当するため意図的に変更していない
(呼び出し元がすでに新鮮な値を渡している前提に依存する、際どいが
現状は健全な設計)。将来これらの関数の内部に新たなアロケーションを
追加する場合は、この前提が崩れるため注意すること。
