;;;; ファイルシステム非依存のファイルノード抽象クラス([ファイルI/O]#43/#45)。
;;;; FAT16/FAT32等、具体的なファイルシステムのディレクトリエントリはこのクラスの
;;;; サブクラス(<fat16-file-node>/<fat32-file-node>、fat16.lisp/fat32.lisp参照)
;;;; として表現する。name/attr/size/start-clusterは、これまで別々に重複定義
;;;; されていたdir-entry(fat16.lisp)/dir-entry32(fat32.lisp)が共通して持っていた
;;;; スロットをそのまま踏襲する。deviceは所属するブロックデバイスハンドル
;;;; (現時点では未使用、read-into!/write-from!がdeviceを引数に取らずnodeだけから
;;;; 解決できるようにするための置き場所)。last-cluster-index/last-cluster-numberは
;;;; 後続マイルストーンのread-into!/write-from!(オフセット→クラスタ探索)が使う
;;;; シーケンシャルアクセス用のクラスタ位置キャッシュで、未使用の間はnilのまま
;;;; (name/attr/size/start-clusterと同じくタグ付き値を保持するだけの通常スロット
;;;; なので%接頭辞は付けない、README.mdの命名規約参照)。
(defclass <file-node> ()
  ((name :initarg :name :initform nil)
   (attr :initarg :attr :initform nil)
   (size :initarg :size :initform nil)
   (start-cluster :initarg :start-cluster :initform nil)
   (device :initarg :device :initform nil)
   (last-cluster-index :initarg :last-cluster-index :initform nil)
   (last-cluster-number :initarg :last-cluster-number :initform nil)))

;;; --- [ファイルI/O]#47(M4): read-into!総称関数の宣言 ---
;;; 実際のディスパッチ先(<fat16-file-node>/<fat32-file-node>用のdefmethod)は
;;; それぞれfat16.lisp/fat32.lispで定義する(defgeneric/defmethodはM1で
;;; トランスパイラに追加した機能、#44参照)。ここでは総称関数名だけを
;;; ファイルシステム非依存のこのファイルで宣言しておく。
;;; (node file-offsetバイト目からcountバイト分をbufferのbuffer-offset位置へ
;;; 書き込み、実際に読めたバイト数を返す。EOFに達した場合は要求より少ない)
(defgeneric read-into! (node buffer buffer-offset file-offset count))
