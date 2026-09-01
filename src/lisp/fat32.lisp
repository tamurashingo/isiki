;;;; FAT32ファイルシステムの読み込みAPI。
;;;; ide.lispと同じ理由でAOTトランスパイル対象外の、通常のload形式(インタプリタ
;;;; 実行専用)ファイル。各関数はdeviceハンドル(*devices*/(%device-handle 'blk0)
;;;; 等で得られるblock_device_t*)を引数に取り、read-sector(src/lisp/ide.lisp)経由
;;;; で読み込む。REPLから(load "src/lisp/device.lisp")→(load "src/lisp/ide.lisp")→
;;;; (load "src/lisp/fat32.lisp")の順に明示的に読み込む。src/lisp/fat16.lispとは
;;;; コードを共有しない独立ファイル(ide.lisp/device.lispと同じ「個別ドライバは
;;;; 自己完結ファイル」という方針)。
;;;;
;;;; FAT32-M1: BPB(拡張含む)パースのみを実装する。

;; (%fat32-u16 bytes offset) : bytes(fixnum(0-255)のリスト、read-sectorの戻り値)の
;; offsetから始まるLE 16bit値を1個のfixnumに合成する。
(defun %fat32-u16 (bytes offset)
  (logior (elt bytes offset) (ash (elt bytes (+ offset 1)) 8)))

;; (%fat32-u32 bytes offset) : bytesのoffsetから始まるLE 32bit値を1個のfixnumに
;; 合成する。logior/ashが2引数限定のため%fat32-u16を2回に分けて合成する。
(defun %fat32-u32 (bytes offset)
  (logior (%fat32-u16 bytes offset) (ash (%fat32-u16 bytes (+ offset 2)) 16)))

;; BPB(BIOS Parameter Block)のうち、FAT32のレイアウト計算に必要な最小限の
;; フィールドのみを保持する。FAT16のbpbクラスとの違い: root-entry-count/
;; sectors-per-fatは常に0になるため持たず、代わりに拡張BPBのfat-size-32/
;; root-clusterを持つ。
(defclass bpb32 ()
  ((bytes-per-sector :initarg :bytes-per-sector :initform nil)
   (sectors-per-cluster :initarg :sectors-per-cluster :initform nil)
   (reserved-sectors :initarg :reserved-sectors :initform nil)
   (num-fats :initarg :num-fats :initform nil)
   (total-sectors :initarg :total-sectors :initform nil)
   (fat-size-32 :initarg :fat-size-32 :initform nil)
   (root-cluster :initarg :root-cluster :initform nil)))

;; (fat32-read-bpb device) : deviceのセクタ0(ブートセクタ)を読み、BPBの各フィールドを
;; 取り出してbpb32インスタンスを返す。read-sectorが失敗した場合はnil。
;; オフセットは標準的なFAT32 BPB(共通部+拡張部)のレイアウト通り:
;;   0x0B(11) bytes-per-sector (u16)
;;   0x0D(13) sectors-per-cluster (u8)
;;   0x0E(14) reserved-sectors (u16)
;;   0x10(16) num-fats (u8)
;;   0x13(19) total-sectors-16 (u16、FAT32では常に0なので0x20(32)を使う)
;;   0x20(32) total-sectors-32 (u32)
;;   0x24(36) fat-size-32 (u32、FAT16のsectors-per-fatに相当する実値)
;;   0x2C(44) root-cluster (u32、ルートディレクトリの開始クラスタ番号)
(defun fat32-read-bpb (device)
  (let ((bytes (read-sector device 0)))
    (if (null bytes)
        nil
        (let ((total-sectors-16 (%fat32-u16 bytes 19)))
          (make-instance 'bpb32
            ':bytes-per-sector (%fat32-u16 bytes 11)
            ':sectors-per-cluster (elt bytes 13)
            ':reserved-sectors (%fat32-u16 bytes 14)
            ':num-fats (elt bytes 16)
            ':total-sectors (if (= total-sectors-16 0)
                                 (%fat32-u32 bytes 32)
                                 total-sectors-16)
            ':fat-size-32 (%fat32-u32 bytes 36)
            ':root-cluster (%fat32-u32 bytes 44))))))

;;; --- FATエントリとクラスタチェイン追跡 ---
;;
;; FAT32はルートディレクトリにも固定LBA範囲が存在せず、通常のサブディレクトリと
;; 同じくクラスタチェインとして表現される。そのため、FAT16ではM2(固定ルート)→
;; M7a(クラスタチェインへの一般化)の2段階を踏んだ抽象を、FAT32では最初から
;; 統一して実装する。

;; 直前に読んだFATセクタの(lba . bytes)キャッシュ。fat16.lispと同じ理由
;; (defglobal+setqは呼び出し元のenvironmentに反映されないため、複数呼び出しを
;; 跨いで共有するにはdefdynamic+%%set-dynamicを使う)。
(defdynamic *fat32-fat-cache-lba* nil)
(defdynamic *fat32-fat-cache-bytes* nil)

;; (%fat32-fat-sector-bytes device lba) : FATテーブル中のlbaセクタの内容(512byte)
;; を返す。直前に読んだセクタと同じlbaならキャッシュを再利用する。
(defun %fat32-fat-sector-bytes (device lba)
  (if (and (dynamic *fat32-fat-cache-lba*) (= (dynamic *fat32-fat-cache-lba*) lba))
      (dynamic *fat32-fat-cache-bytes*)
      (let ((bytes (read-sector device lba)))
        (progn
          (%%set-dynamic '*fat32-fat-cache-lba* lba)
          (%%set-dynamic '*fat32-fat-cache-bytes* bytes)
          bytes))))

;; (fat32-fat-entry device bpb cluster-no) : FATテーブル中のcluster-noに対応する
;; 32bit値を返す(上位4bitの予約領域はlogandで切り捨て、実効28bitのみ)。
;; FATテーブル(1本目)はreserved-sectors番目のセクタから始まり、cluster-no*4byte目
;; の位置にLE u32として格納されている(FAT16はcluster-no*2byte)。read-sectorが
;; 失敗した場合はnil。
(defun fat32-fat-entry (device bpb cluster-no)
  (let ((byte-offset (* cluster-no 4)))
    (let ((sector-offset (div byte-offset (slot-value bpb 'bytes-per-sector)))
          (offset-in-sector (mod byte-offset (slot-value bpb 'bytes-per-sector))))
      (let ((bytes (%fat32-fat-sector-bytes device (+ (slot-value bpb 'reserved-sectors) sector-offset))))
        (if (null bytes)
            nil
            (logand (%fat32-u32 bytes offset-in-sector) #x0FFFFFFF))))))

;; (%fat32-total-cluster-count bpb) : データ領域全体のクラスタ数の概算上限
;; (total-sectors - reserved-sectors - num-fats*fat-size-32をsectors-per-clusterで
;; 割った値)。fat32-cluster-chainの探索上限にのみ使うため、厳密な最終クラスタ数
;; との差は問題にならない(FAT16の%fat16-total-cluster-countと同じ用途)。
(defun %fat32-total-cluster-count (bpb)
  (div (- (slot-value bpb 'total-sectors)
          (+ (slot-value bpb 'reserved-sectors)
             (* (slot-value bpb 'num-fats) (slot-value bpb 'fat-size-32))))
       (slot-value bpb 'sectors-per-cluster)))

;; (fat32-cluster-chain device bpb start-cluster) : start-clusterから始まるクラスタ
;; チェインを、FATエントリを辿って訪問順のリストで返す。エントリ値が0x0FFFFFF8
;; 以上なら終端(0x0FFFFFF7の不良クラスタも含め、documents/fs.mdのFAT16と同じ簡略化
;; 方針で終端扱いする)、それ未満は次クラスタへの参照として辿る。fat32-fat-entryが
;; nilを返した(読み込み失敗)場合もそこで打ち切る。クラスタ数に比例して深くなるため
;; Lisp再帰ではなくwhileで書く(eval_no_tco_interpreter_stack_limit、FAT16と同じ理由)。
;; 上限は%fat32-total-cluster-count+2(クラスタ番号は2始まり)とし、FATが破損して
;; 自己参照/循環したチェインになっていても無限ループせず打ち切る。
(defun fat32-cluster-chain (device bpb start-cluster)
  (let ((cluster start-cluster) (count 0)
        (limit (+ (%fat32-total-cluster-count bpb) 2))
        (rev-chain nil) (done nil))
    (while (and (not done) (< count limit))
      (setq rev-chain (cons cluster rev-chain))
      (setq count (+ count 1))
      (let ((entry (fat32-fat-entry device bpb cluster)))
        (if (or (null entry) (>= entry #x0FFFFFF8))
            (setq done t)
            (setq cluster entry))))
    (%fat32-reverse-iter rev-chain)))

;; (%fat32-reverse-iter list) : listを反転して返す。fat16.lispの%fat16-reverse-iter
;; と同じ理由(reverse/nreverseの再帰実装はリスト長に比例してCスタックを消費するため、
;; whileベースで書く)。
(defun %fat32-reverse-iter (list)
  (let ((remaining list) (acc nil))
    (while remaining
      (setq acc (cons (car remaining) acc))
      (setq remaining (cdr remaining)))
    acc))

;;; --- クラスタ→セクタ(LBA)変換 ---

;; (%fat32-data-start-lba bpb) : データ領域の開始LBA。予約セクタの直後にnum-fats個
;; のFATテーブルが並ぶ(FAT16と異なり、ルートディレクトリ専用領域を加算する必要が
;; 無い。ルート自体がデータ領域内の通常クラスタチェインだから)。
(defun %fat32-data-start-lba (bpb)
  (+ (slot-value bpb 'reserved-sectors)
     (* (slot-value bpb 'num-fats) (slot-value bpb 'fat-size-32))))

;; (fat32-cluster-to-lba bpb cluster-no) : cluster-noに対応するデータ領域内のLBAを
;; 返す。FAT32の仕様上クラスタ番号2がデータ領域の先頭に対応する(0/1は予約)。
(defun fat32-cluster-to-lba (bpb cluster-no)
  (+ (%fat32-data-start-lba bpb)
     (* (- cluster-no 2) (slot-value bpb 'sectors-per-cluster))))

;; (%fat32-lba-range start-lba count) : start-lbaからcount個の連続したLBA番号を
;; 並べたリストを返す(1クラスタ分=sectors-per-cluster個程度の小さいリストなので
;; 再帰で安全、fat16.lispの%fat16-lba-rangeと同じ)。
(defun %fat32-lba-range (start-lba count)
  (if (<= count 0)
      nil
      (cons start-lba (%fat32-lba-range (+ start-lba 1) (- count 1)))))

;; (%fat32-clusters-to-lbas bpb clusters) : clusters(クラスタ番号のリスト、
;; fat32-cluster-chainの戻り値)を、各クラスタを構成する全セクタのLBA番号を順に
;; 並べた1本のリストへ展開する。要素数に比例して深くなるためwhileで辿る
;; (fat16.lispの%fat16-clusters-to-lbasと同じ)。
(defun %fat32-clusters-to-lbas (bpb clusters)
  (let ((remaining clusters) (rev-lbas nil))
    (while remaining
      (let ((lba-range (%fat32-lba-range (fat32-cluster-to-lba bpb (car remaining))
                                          (slot-value bpb 'sectors-per-cluster))))
        (while lba-range
          (setq rev-lbas (cons (car lba-range) rev-lbas))
          (setq lba-range (cdr lba-range))))
      (setq remaining (cdr remaining)))
    (%fat32-reverse-iter rev-lbas)))

;; (%fat32-dir-lbas-for-cluster device bpb start-cluster) : start-clusterから始まる
;; ディレクトリ(ルートでもサブディレクトリでも同じ、FAT32ではどちらもクラスタ
;; チェイン)を構成する全セクタのLBAリスト。FAT16では%fat16-root-dir-lbas/
;; %fat16-subdir-lbasの2関数だったが、FAT32はルート・サブディレクトリの構造が
;; 同一なため1関数に統一できる。
(defun %fat32-dir-lbas-for-cluster (device bpb start-cluster)
  (%fat32-clusters-to-lbas bpb (fat32-cluster-chain device bpb start-cluster)))

;;; --- ディレクトリエントリの列挙 ---

;; dir-entry32 : ディレクトリエントリ1件のパース結果。fat16.lispのdir-entryとの
;; 違いはstart-clusterの取り出し方のみ(下記%fat32-dir-entry-at参照)。
(defclass dir-entry32 ()
  ((name :initarg :name :initform nil)
   (attr :initarg :attr :initform nil)
   (size :initarg :size :initform nil)
   (start-cluster :initarg :start-cluster :initform nil)))

;; (%fat32-dir-entry-at bytes offset) : bytes(1セクタ512byte分)のoffsetにある
;; 32byteディレクトリエントリをパースする。先頭バイトが0x00ならシンボル'endを、
;; 0xE5(削除済み)ならnilを、それ以外はdir-entry32インスタンスを返す
;; (fat16.lispの%fat16-dir-entry-atと同じ判定)。開始クラスタのみFAT16と異なり、
;; 高16bit(offset 20)と低16bit(offset 26)に分割されているため
;; (logior low (ash high 16))で合成する(このコードベース最大の相違点)。
(defun %fat32-dir-entry-at (bytes offset)
  (let ((first-byte (elt bytes offset)))
    (if (= first-byte 0)
        'end
        (if (= first-byte #xE5)
            nil
            (make-instance 'dir-entry32
              ':name (%fat32-dir-entry-name bytes offset)
              ':attr (elt bytes (+ offset 11))
              ':start-cluster (logior (%fat32-u16 bytes (+ offset 26))
                                       (ash (%fat32-u16 bytes (+ offset 20)) 16))
              ':size (%fat32-u32 bytes (+ offset 28)))))))

;; (%fat32-drop-leading-spaces bytes) : bytes先頭の連続するASCIIスペース(32)を
;; 取り除いた残りを返す(fat16.lispと同じヘルパー)。
(defun %fat32-drop-leading-spaces (bytes)
  (if (and (not (null bytes)) (= (car bytes) 32))
      (%fat32-drop-leading-spaces (cdr bytes))
      bytes))

;; (%fat32-rtrim-spaces bytes) : bytes末尾の連続するASCIIスペースを取り除いた
;; 残りを返す。
(defun %fat32-rtrim-spaces (bytes)
  (reverse (%fat32-drop-leading-spaces (reverse bytes))))

;; (%fat32-bytes-to-string bytes) : ASCIIコードのfixnumリストbytesから対応する
;; 文字を1文字ずつ持つLisp文字列を組み立てる。
(defun %fat32-bytes-to-string (bytes)
  (let ((s (create-string (length bytes))))
    (progn
      (for ((b bytes (cdr b))
            (i 0 (+ i 1)))
          ((null b) nil)
        (set-elt (code-char (car b)) s i))
      s)))

;; (%fat32-dir-entry-name bytes offset) : offsetにある32byteエントリの8+3byte名
;; フィールドから表示用文字列("HELLO.TXT"、拡張子が空なら"."無し)を組み立てる。
(defun %fat32-dir-entry-name (bytes offset)
  (let ((name-bytes (%fat32-rtrim-spaces (%ide-take (%ide-drop bytes offset) 8)))
        (ext-bytes (%fat32-rtrim-spaces (%ide-take (%ide-drop bytes (+ offset 8)) 3))))
    (if (null ext-bytes)
        (%fat32-bytes-to-string name-bytes)
        (string-append (%fat32-bytes-to-string name-bytes) "." (%fat32-bytes-to-string ext-bytes)))))

;; (%fat32-scan-dir-entries device lbas) : lbas(ディレクトリを構成するセクタLBAの
;; リスト)を先頭からwhileで走査し、有効なdir-entry32のリストを返す。0x00終端に
;; 到達した時点で走査を止める(fat16.lispの%fat16-scan-dir-entriesと同じ構造)。
;; read-sectorが失敗した場合はそれまでに集めたエントリを捨ててnil。
(defun %fat32-scan-dir-entries (device lbas)
  (let ((remaining-lbas lbas) (rev-entries nil) (stopped nil) (ok t))
    (while (and ok remaining-lbas (not stopped))
      (let ((bytes (read-sector device (car remaining-lbas))))
        (if (null bytes)
            (setq ok nil)
            (progn
              (let ((offset 0) (i 0))
                (while (and (not stopped) (< i 16))
                  (let ((parsed (%fat32-dir-entry-at bytes offset)))
                    (if (eq parsed 'end)
                        (setq stopped t)
                        (progn
                          (if (not (null parsed)) (setq rev-entries (cons parsed rev-entries)))
                          (setq offset (+ offset 32))
                          (setq i (+ i 1)))))))
              (setq remaining-lbas (cdr remaining-lbas))))))
    (if ok (%fat32-reverse-iter rev-entries) nil)))

;; (%fat32-dir-entry-kind attr) : attrバイトのディレクトリ属性ビット(0x10)を見て
;; :fileまたは:dirを返す。
(defun %fat32-dir-entry-kind (attr)
  (if (= (logand attr #x10) 0) ':file ':dir))

;; (%fat32-dir-entries-to-display-list entries) : dir-entry32のリストを
;; ("NAME" :file/:dir size)の3要素リストのリストに変換する。
(defun %fat32-dir-entries-to-display-list (entries)
  (if (null entries)
      nil
      (cons (list (slot-value (car entries) 'name)
                  (%fat32-dir-entry-kind (slot-value (car entries) 'attr))
                  (slot-value (car entries) 'size))
            (%fat32-dir-entries-to-display-list (cdr entries)))))

;; (%fat32-find-dir-entry entries name) : dir-entry32のリストentriesからnameと
;; (拡張子まで含めた)8.3名が一致するものを探す。見つからなければnil。
(defun %fat32-find-dir-entry (entries name)
  (if (null entries)
      nil
      (if (string= (slot-value (car entries) 'name) name)
          (car entries)
          (%fat32-find-dir-entry (cdr entries) name))))

;;; --- パス解決(多階層、ルート・サブディレクトリ共通) ---

;; (%fat32-split-path path) : "/DOCS/SUB/README.TXT"のようなpathを"/"区切りで
;; 非空要素のリスト("DOCS" "SUB" "README.TXT")に分解する。"/"のみのパスはnil
;; (ルート自身)。fat16.lispの%fat16-split-pathと同じ。
(defun %fat32-split-path (path)
  (let ((start 0) (len (length path)) (rev-parts nil))
    (while (< start len)
      (let* ((slash-pos (char-index #\/ path start))
             (end (if slash-pos slash-pos len)))
        (if (> end start)
            (setq rev-parts (cons (subseq path start end) rev-parts)))
        (setq start (+ end 1))))
    (%fat32-reverse-iter rev-parts)))

;; (%fat32-butlast list) : listから末尾の要素を除いた部分を返す。パス要素の
;; リスト(階層数は小さい)に対して使うためLisp再帰でよい。
(defun %fat32-butlast (list)
  (if (or (null list) (null (cdr list)))
      nil
      (cons (car list) (%fat32-butlast (cdr list)))))

;; (%fat32-last-elt list) : listの最後の要素を返す。
(defun %fat32-last-elt (list)
  (if (null (cdr list)) (car list) (%fat32-last-elt (cdr list))))

;; (%fat32-resolve-dir device bpb components) : componentsをルートから順にサブ
;; ディレクトリとして辿り、(最終ディレクトリのLBAリスト . 最終ディレクトリの
;; start-cluster)を返す。componentsがnilならルート自身
;; ((ルートLBAリスト . root-cluster))。FAT16の%fat16-resolve-dirと異なり、
;; ルートの「start-cluster」は0のような特別値ではなく実際のroot-clusterになる点に
;; 注意(ルート自体がクラスタ番号を持つFAT32の構造上の違い、後続の".."実装で
;; 重要になる)。途中のいずれかの段でエントリが見つからない・ディレクトリでない
;; 場合、または読み込み失敗の場合はnil。
(defun %fat32-resolve-dir (device bpb components)
  (let ((cluster (slot-value bpb 'root-cluster))
        (lbas (%fat32-dir-lbas-for-cluster device bpb (slot-value bpb 'root-cluster)))
        (remaining components) (ok t))
    (while (and ok remaining)
      (let ((entries (%fat32-scan-dir-entries device lbas)))
        (if (null entries)
            (setq ok nil)
            (let ((entry (%fat32-find-dir-entry entries (car remaining))))
              (if (or (null entry) (= (logand (slot-value entry 'attr) #x10) 0))
                  (setq ok nil)
                  (progn
                    (setq cluster (slot-value entry 'start-cluster))
                    (setq lbas (%fat32-dir-lbas-for-cluster device bpb cluster))
                    (setq remaining (cdr remaining))))))))
    (if ok (cons lbas cluster) nil)))

;; (%fat32-resolve-dir-lbas device bpb components) : %fat32-resolve-dirの結果から
;; LBAリストのみを取り出す薄いラッパー(fat32-read-dir用)。
(defun %fat32-resolve-dir-lbas (device bpb components)
  (let ((resolved (%fat32-resolve-dir device bpb components)))
    (if (null resolved) nil (car resolved))))

;; (%fat32-resolve-file device bpb path) : pathを分解し、最後の要素をファイル名
;; として残し、それ以前を%fat32-resolve-dir-lbasでディレクトリとして解決する。
;; 戻り値は(親ディレクトリのLBAリスト . ファイル名)。パスが"/"のみ、または親
;; ディレクトリの解決に失敗した場合はnil。
(defun %fat32-resolve-file (device bpb path)
  (let ((components (%fat32-split-path path)))
    (if (null components)
        nil
        (let ((lbas (%fat32-resolve-dir-lbas device bpb (%fat32-butlast components))))
          (if (null lbas)
              nil
              (cons lbas (%fat32-last-elt components)))))))

;; (fat32-read-dir device path) : path("/"、または"/DOCS"のような多階層パス)が
;; 指すディレクトリのエントリ一覧を("NAME" :file/:dir size)の形のリストで返す。
;; BPBが読めない・パスが解決できない場合はnil。
(defun fat32-read-dir (device path)
  (let ((bpb (fat32-read-bpb device)))
    (if (null bpb)
        nil
        (let ((lbas (%fat32-resolve-dir-lbas device bpb (%fat32-split-path path))))
          (if (null lbas)
              nil
              (%fat32-dir-entries-to-display-list (%fat32-scan-dir-entries device lbas)))))))

;;; --- ファイル本体の読み込み ---

;; (%fat32-read-lba-list device lbas) : lbasの順にセクタを読み、連結したbyteリスト
;; を返す。read-sectorが失敗した場合はそれまでに読んだ分は捨ててnil。セクタ数に
;; 比例して深くなるためwhileで書く(fat16.lispの%fat16-read-lba-listと同じ理由、
;; eval_no_tco_interpreter_stack_limit)。
(defun %fat32-read-lba-list (device lbas)
  (let ((remaining-lbas lbas) (rev-bytes nil) (ok t))
    (while (and ok remaining-lbas)
      (let ((sector-bytes (read-sector device (car remaining-lbas))))
        (if (null sector-bytes)
            (setq ok nil)
            (progn
              (let ((remaining-bytes sector-bytes))
                (while remaining-bytes
                  (setq rev-bytes (cons (car remaining-bytes) rev-bytes))
                  (setq remaining-bytes (cdr remaining-bytes))))
              (setq remaining-lbas (cdr remaining-lbas))))))
    (if ok
        (%fat32-reverse-iter rev-bytes)
        nil)))

;; (fat32-read-file device path) : path("/NAME.EXT"、または"/DOCS/NAME.EXT"のような
;; 多階層パス)のファイル本体をfixnum(0-255)のリストとして返す。パスが解決できない・
;; エントリが見つからない場合・クラスタ読み込みに失敗した場合はnil。0byteファイル
;; (size=0)はクラスタを辿らずそのままnil(=空リスト)を返す。
(defun fat32-read-file (device path)
  (let ((bpb (fat32-read-bpb device)))
    (if (null bpb)
        nil
        (let ((resolved (%fat32-resolve-file device bpb path)))
          (if (null resolved)
              nil
              (let ((entry (%fat32-find-dir-entry
                             (%fat32-scan-dir-entries device (car resolved))
                             (cdr resolved))))
                (if (null entry)
                    nil
                    (let ((size (slot-value entry 'size)))
                      (if (= size 0)
                          nil
                          (let ((bytes (%fat32-read-lba-list device
                                         (%fat32-clusters-to-lbas bpb
                                           (fat32-cluster-chain device bpb (slot-value entry 'start-cluster))))))
                            (if (null bytes)
                                nil
                                (subseq bytes 0 size))))))))))))

;;; --- FAT32-M6a: 既存ファイルの同クラスタ数上書き ---

;; (%fat32-u16-to-bytes n) : fixnumをLEの2byteリストに分解する(%fat32-u16の逆変換)。
(defun %fat32-u16-to-bytes (n)
  (list (logand n #xFF) (logand (ash n -8) #xFF)))

;; (%fat32-u32-to-bytes n) : fixnumをLEの4byteリストに分解する(%fat32-u32の逆変換)。
;; logior/ashが2引数限定のため%fat32-u16-to-bytesを2回に分けて合成する。
(defun %fat32-u32-to-bytes (n)
  (append (%fat32-u16-to-bytes (logand n #xFFFF))
          (%fat32-u16-to-bytes (logand (ash n -16) #xFFFF))))

;; (%fat32-patch-bytes! list offset value-list) : listを破壊的にパッチする。
;; fat16.lispの%fat16-patch-bytes!と同じ実装(offset/value-listの長さに関わらず
;; whileベース、Cスタックを消費しない)。
(defun %fat32-patch-bytes! (list offset value-list)
  (let ((cell list) (n offset))
    (while (> n 0)
      (setq cell (cdr cell))
      (setq n (- n 1)))
    (let ((values value-list))
      (while values
        (set-car cell (car values))
        (setq cell (cdr cell))
        (setq values (cdr values))))
    list))

;; (%fat32-split-into-chunks bytes chunk-size) : bytesをchunk-sizeごとのリストの
;; リストに分割する。最終チャンクが足りない分は0でパディングする。fat16.lispの
;; %fat16-split-into-chunksと同じ理由でwhileベース。
(defun %fat32-split-into-chunks (bytes chunk-size)
  (let ((remaining bytes) (chunks nil))
    (while remaining
      (let ((rev-chunk nil) (count 0))
        (while (and remaining (< count chunk-size))
          (setq rev-chunk (cons (car remaining) rev-chunk))
          (setq remaining (cdr remaining))
          (setq count (+ count 1)))
        (while (< count chunk-size)
          (setq rev-chunk (cons 0 rev-chunk))
          (setq count (+ count 1)))
        (setq chunks (cons (%fat32-reverse-iter rev-chunk) chunks))))
    (%fat32-reverse-iter chunks)))

;; (%fat32-write-lba-list device lbas chunks) : lbasとchunks(同じ長さ)を並行して
;; whileで辿り、対応するペアをwrite-sectorする。1つでも失敗したら残りは書かず
;; 打ち切ってnil、全部成功でt。
(defun %fat32-write-lba-list (device lbas chunks)
  (let ((remaining-lbas lbas) (remaining-chunks chunks) (ok t))
    (while (and ok remaining-lbas remaining-chunks)
      (if (write-sector device (car remaining-lbas) (car remaining-chunks))
          (progn
            (setq remaining-lbas (cdr remaining-lbas))
            (setq remaining-chunks (cdr remaining-chunks)))
          (setq ok nil)))
    ok))

;; (%fat32-find-entry-in-sector bytes entry-offset entries-remaining name) : 1セクタ
;; 分のエントリをentry-offsetから順に調べ、名前がnameと一致する最初のエントリの
;; offsetを返す。0x00終端に到達したらシンボル'end、削除済み(0xE5)はスキップし
;; 次のエントリへ進む。entries-remainingを使い切ってもマッチしなければnil。
(defun %fat32-find-entry-in-sector (bytes entry-offset entries-remaining name)
  (if (<= entries-remaining 0)
      nil
      (let ((first-byte (elt bytes entry-offset)))
        (if (= first-byte 0)
            'end
            (if (and (/= first-byte #xE5)
                     (string= (%fat32-dir-entry-name bytes entry-offset) name))
                entry-offset
                (%fat32-find-entry-in-sector bytes (+ entry-offset 32) (- entries-remaining 1) name))))))

;; (%fat32-find-entry-location-scan device lbas name) : lbasを先頭からwhileで走査し、
;; nameと一致するエントリの(lba . offset-in-sector)を返す。0x00終端に到達するか
;; 読み込みに失敗した時点で走査を止める。見つからなければnil。
(defun %fat32-find-entry-location-scan (device lbas name)
  (let ((remaining-lbas lbas) (result nil) (stopped nil))
    (while (and remaining-lbas (null result) (not stopped))
      (let ((bytes (read-sector device (car remaining-lbas))))
        (if (null bytes)
            (setq stopped t)
            (let ((found (%fat32-find-entry-in-sector bytes 0 16 name)))
              (if (eq found 'end)
                  (setq stopped t)
                  (if (null found)
                      (setq remaining-lbas (cdr remaining-lbas))
                      (setq result (cons (car remaining-lbas) found))))))))
    result))

;; (%fat32-cluster-count-for-bytes byte-length cluster-bytes) : byte-length分の
;; データを格納するのに必要なクラスタ数(切り上げ)。0byteなら0。
(defun %fat32-cluster-count-for-bytes (byte-length cluster-bytes)
  (if (= byte-length 0)
      0
      (if (= (mod byte-length cluster-bytes) 0)
          (div byte-length cluster-bytes)
          (+ (div byte-length cluster-bytes) 1))))

;; (%fat32-write-file-data device bpb start-cluster bytes) : start-clusterから
;; 始まる既存クラスタチェインへbytesを書き込む(セクタ単位に分割してwrite-sector)。
;; bytesがnil(0byte)の場合は書き込むクラスタが無いためそのままt。呼び出し元
;; (fat32-write-file)が必要クラスタ数と現クラスタ数の一致を確認済みという前提。
(defun %fat32-write-file-data (device bpb start-cluster bytes)
  (if (null bytes)
      t
      (let* ((chain (fat32-cluster-chain device bpb start-cluster))
             (lbas (%fat32-clusters-to-lbas bpb chain))
             (chunks (%fat32-split-into-chunks bytes (slot-value bpb 'bytes-per-sector))))
        (%fat32-write-lba-list device lbas chunks))))

;;; --- FAT32-M6b: クラスタ追加を伴うファイル拡張 ---

;; (%fat32-find-free-cluster device bpb) : クラスタ番号2から順にFATエントリが0
;; (未使用)になる最初のクラスタ番号をwhileで探す。上限はクラスタ総数+2(クラスタ
;; 番号は2始まりのため)。見つからなければ(ディスクフル)nil。
(defun %fat32-find-free-cluster (device bpb)
  (let ((cluster-no 2) (limit (+ (%fat32-total-cluster-count bpb) 2)) (found nil))
    (while (and (null found) (< cluster-no limit))
      (let ((entry (fat32-fat-entry device bpb cluster-no)))
        (if (and entry (= entry 0))
            (setq found cluster-no)
            (setq cluster-no (+ cluster-no 1)))))
    found))

;; (%fat32-set-fat-entry device bpb cluster-no value) : cluster-noのFATエントリを
;; valueで上書きする。num-fats全コピーへ同じ内容を書く(fat16.lispと同じ、書き込みは
;; ミラー全体を更新する方針)。FAT16と異なり、書き込むべき値はエントリの下位28bit
;; のみで、上位4bitは仕様上の予約領域(実際の使われ方はドライバ依存)のため、
;; 書き込み前に読んだ既存値の上位4bitをそのまま保持し(logandで下位28bit分だけ
;; valueを取り込み、上位4bitは既存値から合成)、意図せず上書きしないようにする。
;; 書き込んだセクタが*fat32-fat-cache-lba*のキャッシュと一致する場合はキャッシュを
;; 無効化し、次回読み込みで再読込させる。1つでも失敗したら中断してnil、全コピー
;; 成功でt。
(defun %fat32-set-fat-entry (device bpb cluster-no value)
  (let* ((byte-offset (* cluster-no 4))
         (sector-offset (div byte-offset (slot-value bpb 'bytes-per-sector)))
         (offset-in-sector (mod byte-offset (slot-value bpb 'bytes-per-sector))))
    (let ((fat-index 0) (ok t))
      (while (and ok (< fat-index (slot-value bpb 'num-fats)))
        (let* ((lba (+ (slot-value bpb 'reserved-sectors)
                        (* fat-index (slot-value bpb 'fat-size-32))
                        sector-offset))
               (sector-bytes (read-sector device lba)))
          (if (null sector-bytes)
              (setq ok nil)
              (let* ((existing (%fat32-u32 sector-bytes offset-in-sector))
                     (new-value (logior (logand existing #xF0000000) (logand value #x0FFFFFFF)))
                     (value-bytes (%fat32-u32-to-bytes new-value)))
                (progn
                  (%fat32-patch-bytes! sector-bytes offset-in-sector value-bytes)
                  (if (write-sector device lba sector-bytes)
                      (progn
                        (if (and (dynamic *fat32-fat-cache-lba*) (= (dynamic *fat32-fat-cache-lba*) lba))
                            (%%set-dynamic '*fat32-fat-cache-lba* nil))
                        (setq fat-index (+ fat-index 1)))
                      (setq ok nil)))))))
      ok)))

;; (%fat32-allocate-clusters device bpb count) : 空きクラスタをcount個確保し、
;; 発見順のクラスタ番号リストを返す。確保したクラスタは次の探索で再び「空き」と
;; 誤認されないよう、確保直後にFATエントリへ終端マーカー(#x0FFFFFFF)を仮に書き込む
;; (呼び出し元が%fat32-link-clustersで実際のチェインへ後から繋ぎ直す前提)。
;; count個確保できなかった場合(ディスクフル、または書き込み失敗)はnil。
(defun %fat32-allocate-clusters (device bpb count)
  (let ((i 0) (rev-clusters nil) (ok t))
    (while (and ok (< i count))
      (let ((cluster (%fat32-find-free-cluster device bpb)))
        (if (null cluster)
            (setq ok nil)
            (if (%fat32-set-fat-entry device bpb cluster #x0FFFFFFF)
                (progn
                  (setq rev-clusters (cons cluster rev-clusters))
                  (setq i (+ i 1)))
                (setq ok nil)))))
    (if ok
        (%fat32-reverse-iter rev-clusters)
        nil)))

;; (%fat32-link-clusters device bpb clusters) : clustersを先頭から順にFATエントリで
;; チェインする(各要素→次要素)。最後の要素のFATエントリは変更しない(呼び出し元が
;; %fat32-allocate-clustersで既に終端マーカーを設定済みという前提)。1つでも失敗
;; したら中断してnil、全部成功(または要素が1個以下で変更不要)ならt。
(defun %fat32-link-clusters (device bpb clusters)
  (let ((remaining clusters) (ok t))
    (while (and ok remaining (cdr remaining))
      (if (%fat32-set-fat-entry device bpb (car remaining) (car (cdr remaining)))
          (setq remaining (cdr remaining))
          (setq ok nil)))
    ok))

;; (%fat32-extend-file device bpb start-cluster old-cluster-count new-cluster-count
;;  bytes) : start-clusterから始まる既存チェイン(old-cluster-count個、0なら
;; まだクラスタ未確保=旧サイズ0)を、new-cluster-count個になるよう新規クラスタを
;; 確保・接続し、bytes全体を書き込む。呼び出し元はnew-cluster-count >
;; old-cluster-countであることを保証する前提(縮小は対象外)。成功時は新しい
;; start-cluster(旧チェインがあった場合は変化しないstart-cluster自身、旧チェインが
;; 無かった場合は新規確保した先頭クラスタ)を返す。失敗時はnil。
(defun %fat32-extend-file (device bpb start-cluster old-cluster-count new-cluster-count bytes)
  (let ((new-clusters (%fat32-allocate-clusters device bpb (- new-cluster-count old-cluster-count))))
    (if (null new-clusters)
        nil
        (if (not (%fat32-link-clusters device bpb new-clusters))
            nil
            (let* ((had-old-chain (> old-cluster-count 0))
                   (linked-to-old (if had-old-chain
                                       (%fat32-set-fat-entry device bpb
                                         (%fat32-last-elt (fat32-cluster-chain device bpb start-cluster))
                                         (car new-clusters))
                                       t))
                   (first-cluster (if had-old-chain start-cluster (car new-clusters))))
              (if (not linked-to-old)
                  nil
                  (if (%fat32-write-file-data device bpb first-cluster bytes)
                      first-cluster
                      nil)))))))

;; (%fat32-finish-directory-update device dir-lba dir-bytes dir-offset
;;  new-start-cluster new-size) : ディレクトリエントリのstart-cluster(高16bit@20+
;; 低16bit@26)とsize(offset+28)フィールドを書き換えてセクタを書き込む。FAT16との
;; 最大の相違点(%fat32-dir-entry-atの読み込み側と対称、開始クラスタが2フィールドに
;; 分割されている)。M6a(サイズのみ変化、start-clusterは同じ値を渡せば実質変更
;; なし)・M6b(start-clusterも変わり得る)の両方から共通で使う。
(defun %fat32-finish-directory-update (device dir-lba dir-bytes dir-offset new-start-cluster new-size)
  (progn
    (%fat32-patch-bytes! dir-bytes (+ dir-offset 20) (%fat32-u16-to-bytes (logand (ash new-start-cluster -16) #xFFFF)))
    (%fat32-patch-bytes! dir-bytes (+ dir-offset 26) (%fat32-u16-to-bytes (logand new-start-cluster #xFFFF)))
    (%fat32-patch-bytes! dir-bytes (+ dir-offset 28) (%fat32-u32-to-bytes new-size))
    (write-sector device dir-lba dir-bytes)))

;; (fat32-write-file device path bytes) : path("/NAME.EXT"、または"/DOCS/NAME.EXT"
;; のような多階層パス)の既存ファイルへbytes(fixnum 0-255のリスト)を上書きする。
;; 必要クラスタ数が現在のクラスタ数と同じ場合はFAT32-M6aの経路(データ→sizeフィールド
;; のみ更新)、必要クラスタ数が増える場合はFAT32-M6bの経路(新規クラスタ確保・FAT
;; チェイン延長→データ→start-cluster/sizeフィールド更新)で書き込む。必要クラスタ数
;; が減る場合(縮小)は対象外としてnilを返し、既存データ/ディレクトリエントリは
;; 変更しない。パスが解決できない・エントリが見つからない・各段階の書き込みに失敗
;; した場合もnil、成功時はt。
(defun fat32-write-file (device path bytes)
  (let* ((bpb (fat32-read-bpb device)))
    (if (null bpb)
        nil
        (let* ((resolved (%fat32-resolve-file device bpb path))
               (loc (if (null resolved)
                        nil
                        (%fat32-find-entry-location-scan device (car resolved) (cdr resolved)))))
          (if (null loc)
              nil
              (let* ((dir-lba (car loc))
                     (dir-offset (cdr loc))
                     (dir-bytes (read-sector device dir-lba)))
                (if (null dir-bytes)
                    nil
                    (let* ((start-cluster (logior (%fat32-u16 dir-bytes (+ dir-offset 26))
                                                   (ash (%fat32-u16 dir-bytes (+ dir-offset 20)) 16)))
                           (cluster-bytes (* (slot-value bpb 'sectors-per-cluster) (slot-value bpb 'bytes-per-sector)))
                           (current-cluster-count (if (= start-cluster 0)
                                                        0
                                                        (length (fat32-cluster-chain device bpb start-cluster))))
                           (required-cluster-count (%fat32-cluster-count-for-bytes (length bytes) cluster-bytes)))
                      (cond
                        ((= required-cluster-count current-cluster-count)
                         (if (%fat32-write-file-data device bpb start-cluster bytes)
                             (%fat32-finish-directory-update device dir-lba dir-bytes dir-offset start-cluster (length bytes))
                             nil))
                        ((> required-cluster-count current-cluster-count)
                         (let ((new-start-cluster (%fat32-extend-file device bpb start-cluster current-cluster-count required-cluster-count bytes)))
                           (if (null new-start-cluster)
                               nil
                               (%fat32-finish-directory-update device dir-lba dir-bytes dir-offset new-start-cluster (length bytes)))))
                        (t nil))))))))))

;;; --- FAT32-M6c: 新規ファイル作成 ---

;; (%fat32-upcase-char-code code) : 小文字ASCII(97-122)のcode(char-codeの戻り値)を
;; 大文字(-32)に変換する。それ以外のコードはそのまま返す(8.3名は大文字が規約)。
(defun %fat32-upcase-char-code (code)
  (if (and (>= code 97) (<= code 122))
      (- code 32)
      code))

;; (%fat32-8.3-field-bytes s field-len) : 文字列sを大文字化してfield-len(8か3)byte
;; の固定長フィールドへ変換する。sがfield-lenを超えるとnil(ロングファイルネーム
;; 相当、スコープ外)。不足分は空白(32)でパディングする。
(defun %fat32-8.3-field-bytes (s field-len)
  (let ((n (length s)))
    (if (> n field-len)
        nil
        (let ((i 0) (rev nil))
          (while (< i n)
            (setq rev (cons (%fat32-upcase-char-code (char-code (elt s i))) rev))
            (setq i (+ i 1)))
          (let ((bytes (%fat32-reverse-iter rev)) (pad (- field-len n)))
            (while (> pad 0)
              (setq bytes (append bytes (list 32)))
              (setq pad (- pad 1)))
            bytes)))))

;; (%fat32-name-to-8.3 name) : "NAME.EXT"形式のnameを、8.3形式11byteフィールド
;; (base8byte+ext3byte、大文字・空白パディング)へ変換する。最初の"."でbase/extに
;; 分割し、それぞれ%fat32-8.3-field-bytesへ渡す。2個目の"."が見つかる場合(複数
;; ドット)、またはbase>8/ext>3の場合はロングファイルネーム相当としてnilを返す。
;; "."が無い場合はext=""として扱う(拡張子無しファイル)。
(defun %fat32-name-to-8.3 (name)
  (let ((dot-pos (char-index #\. name)))
    (if (null dot-pos)
        (let ((base-bytes (%fat32-8.3-field-bytes name 8))
              (ext-bytes (%fat32-8.3-field-bytes "" 3)))
          (if (and base-bytes ext-bytes) (append base-bytes ext-bytes) nil))
        (if (char-index #\. name (+ dot-pos 1))
            nil
            (let* ((base (subseq name 0 dot-pos))
                   (ext (subseq name (+ dot-pos 1) (length name)))
                   (base-bytes (%fat32-8.3-field-bytes base 8))
                   (ext-bytes (%fat32-8.3-field-bytes ext 3)))
              (if (and base-bytes ext-bytes) (append base-bytes ext-bytes) nil))))))

;; (%fat32-find-free-slot-in-sector bytes entry-offset entries-remaining) : 1セクタ
;; 分のエントリをentry-offsetから順に調べ、先頭バイトが0x00(終端)または0xE5
;; (削除済み、再利用可)の最初のエントリのoffsetを返す。見つからなければnil。
(defun %fat32-find-free-slot-in-sector (bytes entry-offset entries-remaining)
  (if (<= entries-remaining 0)
      nil
      (let ((first-byte (elt bytes entry-offset)))
        (if (or (= first-byte 0) (= first-byte #xE5))
            entry-offset
            (%fat32-find-free-slot-in-sector bytes (+ entry-offset 32) (- entries-remaining 1))))))

;; (%fat32-find-free-slot-scan device lbas) : lbasを先頭からwhileで走査し、最初の
;; 空きスロット(削除済み0xE5、または終端0x00)の(lba . offset-in-sector)を返す。
;; 見つからなければ(ディレクトリ満杯)nil。0x00スロットを再利用しても、直後の
;; エントリは元から0x00のままなので走査の終端条件は保たれる。
(defun %fat32-find-free-slot-scan (device lbas)
  (let ((remaining-lbas lbas) (result nil))
    (while (and remaining-lbas (null result))
      (let ((bytes (read-sector device (car remaining-lbas))))
        (if (null bytes)
            (setq remaining-lbas nil)
            (let ((found (%fat32-find-free-slot-in-sector bytes 0 16)))
              (if (null found)
                  (setq remaining-lbas (cdr remaining-lbas))
                  (setq result (cons (car remaining-lbas) found)))))))
    result))

;; (%fat32-build-dir-entry-bytes name-bytes attr start-cluster size) : 32byteの
;; 新規ディレクトリエントリ全体を構築する。name-bytesは%fat32-name-to-8.3の戻り値
;; (11byte)。開始クラスタは高16bit(offset 20)・低16bit(offset 26)に分割して
;; 書き込む(FAT16との最大の相違点)。offset 12-19(NT予約・作成時刻/日付・最終
;; アクセス日、8byte)とoffset 22-25(最終更新時刻/日付、4byte)は0埋め。
(defun %fat32-build-dir-entry-bytes (name-bytes attr start-cluster size)
  (append name-bytes
          (list attr 0 0 0 0 0 0 0 0)
          (%fat32-u16-to-bytes (logand (ash start-cluster -16) #xFFFF))
          (list 0 0 0 0)
          (%fat32-u16-to-bytes (logand start-cluster #xFFFF))
          (%fat32-u32-to-bytes size)))

;; (%fat32-allocate-new-file-data device bpb required-cluster-count bytes) :
;; 新規ファイル用のクラスタを確保してbytesを書き込む。required-cluster-countが0
;; (空ファイル)ならクラスタを確保せずstart-cluster=0を返す。それ以外は
;; %fat32-extend-file(FAT32-M6b)をold-cluster-count=0として呼び、「既存チェイン無し
;; からの新規確保」を再利用する。失敗時はnil。
(defun %fat32-allocate-new-file-data (device bpb required-cluster-count bytes)
  (if (= required-cluster-count 0)
      0
      (%fat32-extend-file device bpb 0 0 required-cluster-count bytes)))

;; (fat32-create-file device path bytes) : path("/NAME.EXT"、または
;; "/DOCS/NAME.EXT"のような多階層パス)に新規ファイルを作成しbytes(fixnum 0-255の
;; リスト、空ならnil)を書き込む。親ディレクトリが解決できない場合、同名エントリが
;; 既に存在する場合(上書きはfat32-write-fileの役割)、8.3名変換に失敗した場合
;; (ロングファイルネーム相当)、空きディレクトリスロットが無い場合(ディレクトリ
;; 満杯)、クラスタ確保に失敗した場合(ディスクフル)はいずれもnilを返し、何も
;; 変更しない。属性は#x20(ARCHIVE、通常ファイルの標準値)固定。
(defun fat32-create-file (device path bytes)
  (let* ((bpb (fat32-read-bpb device)))
    (if (null bpb)
        nil
        (let ((resolved (%fat32-resolve-file device bpb path)))
          (if (null resolved)
              nil
              (let* ((lbas (car resolved))
                     (name (cdr resolved)))
                (if (%fat32-find-entry-location-scan device lbas name)
                    nil
                    (let ((name-bytes (%fat32-name-to-8.3 name)))
                      (if (null name-bytes)
                          nil
                          (let ((slot (%fat32-find-free-slot-scan device lbas)))
                            (if (null slot)
                                nil
                                (let* ((cluster-bytes (* (slot-value bpb 'sectors-per-cluster) (slot-value bpb 'bytes-per-sector)))
                                       (required-cluster-count (%fat32-cluster-count-for-bytes (length bytes) cluster-bytes))
                                       (start-cluster (%fat32-allocate-new-file-data device bpb required-cluster-count bytes)))
                                  (if (null start-cluster)
                                      nil
                                      (let* ((slot-lba (car slot))
                                             (slot-offset (cdr slot))
                                             (slot-bytes (read-sector device slot-lba))
                                             (entry-bytes (%fat32-build-dir-entry-bytes name-bytes #x20 start-cluster (length bytes))))
                                        (if (null slot-bytes)
                                            nil
                                            (progn
                                              (%fat32-patch-bytes! slot-bytes slot-offset entry-bytes)
                                              (write-sector device slot-lba slot-bytes)))))))))))))))))

;;; --- FAT32-M7: fat32-create-directory(mkdir)新設 ---
;;
;; 新規サブディレクトリは確保直後のクラスタなので、ディスク上の残存データが誤って
;; 有効なディレクトリエントリと誤認されないよう"."/".."エントリ以外を明示的に
;; ゼロ埋めしてから書き込む(fat16.lispのFAT16-M7cと同じ理由・同じ構成)。

;; (%fat32-zero-byte-list n) : 長さnの、全要素が0のfixnumリストをwhileで構築する。
(defun %fat32-zero-byte-list (n)
  (let ((i 0) (acc nil))
    (while (< i n)
      (setq acc (cons 0 acc))
      (setq i (+ i 1)))
    acc))

;; (%fat32-dot-entry-name-bytes) : "."エントリの11byte名フィールド("."+空白10個)。
(defun %fat32-dot-entry-name-bytes ()
  (list 46 32 32 32 32 32 32 32 32 32 32))

;; (%fat32-dotdot-entry-name-bytes) : ".."エントリの11byte名フィールド(".."+空白9個)。
(defun %fat32-dotdot-entry-name-bytes ()
  (list 46 46 32 32 32 32 32 32 32 32 32))

;; (%fat32-init-dir-cluster-bytes bpb own-cluster parent-cluster) : 新規ディレクトリ用
;; に確保した1クラスタ分の初期化バイト列を構築する。先頭32byteが"."エントリ
;; (start-cluster=own-cluster)、続く32byteが".."エントリ(start-cluster=
;; parent-cluster)、残りは%fat32-zero-byte-listでゼロ埋め。FAT16と異なり、親が
;; ルートの場合でもparent-clusterは0のような特別値ではなく実際のroot-cluster
;; (%fat32-resolve-dirの戻り値そのまま)になる点に注意(FAT32はルート自体が実
;; クラスタ番号を持つ構造上の違い、上のFAT32-M7ヘッダコメント・
;; %fat32-resolve-dirのコメント参照)。start-clusterは%fat32-build-dir-entry-bytes
;; 側で高16bit/低16bitに分割されるため、ここではown-cluster/parent-clusterを
;; そのまま渡すだけでよい。
(defun %fat32-init-dir-cluster-bytes (bpb own-cluster parent-cluster)
  (let* ((cluster-bytes (* (slot-value bpb 'sectors-per-cluster) (slot-value bpb 'bytes-per-sector)))
         (dot-entry (%fat32-build-dir-entry-bytes (%fat32-dot-entry-name-bytes) #x10 own-cluster 0))
         (dotdot-entry (%fat32-build-dir-entry-bytes (%fat32-dotdot-entry-name-bytes) #x10 parent-cluster 0))
         (padding (%fat32-zero-byte-list (- cluster-bytes 64))))
    (append dot-entry dotdot-entry padding)))

;; (fat32-create-directory device path) : path("/NAME"、または"/DOCS/NAME"のような
;; 多階層パス)に新規サブディレクトリを作成する。親ディレクトリが解決できない場合、
;; 同名エントリが既に存在する場合、8.3名変換に失敗した場合、空きディレクトリ
;; スロットが無い場合(ディレクトリ満杯)、クラスタ確保に失敗した場合(ディスク
;; フル)、確保したクラスタへの初期化データ書き込みに失敗した場合はいずれもnilを
;; 返し、何も変更しない。属性は#x10(ディレクトリ)固定。fat32-create-file(M6c)と
;; 同じ失敗伝播パターンで、新規クラスタ確保後は%fat32-init-dir-cluster-bytesで
;; "."/".."エントリを構築してから書き込む点のみが異なる。パスが"/"自身(ルートの
;; 作成、componentsがnil)の場合もnil。
(defun fat32-create-directory (device path)
  (let* ((bpb (fat32-read-bpb device)))
    (if (null bpb)
        nil
        (let ((components (%fat32-split-path path)))
          (if (null components)
              nil
              (let* ((name (%fat32-last-elt components))
                     (resolved-parent (%fat32-resolve-dir device bpb (%fat32-butlast components))))
                (if (null resolved-parent)
                    nil
                    (let* ((parent-lbas (car resolved-parent))
                           (parent-cluster (cdr resolved-parent)))
                      (if (%fat32-find-entry-location-scan device parent-lbas name)
                          nil
                          (let ((name-bytes (%fat32-name-to-8.3 name)))
                            (if (null name-bytes)
                                nil
                                (let ((slot (%fat32-find-free-slot-scan device parent-lbas)))
                                  (if (null slot)
                                      nil
                                      (let ((new-clusters (%fat32-allocate-clusters device bpb 1)))
                                        (if (null new-clusters)
                                            nil
                                            (let* ((new-cluster (car new-clusters))
                                                   (cluster-bytes-list (%fat32-init-dir-cluster-bytes bpb new-cluster parent-cluster))
                                                   (new-lbas (%fat32-clusters-to-lbas bpb new-clusters))
                                                   (chunks (%fat32-split-into-chunks cluster-bytes-list (slot-value bpb 'bytes-per-sector))))
                                              (if (not (%fat32-write-lba-list device new-lbas chunks))
                                                  nil
                                                  (let* ((slot-lba (car slot))
                                                         (slot-offset (cdr slot))
                                                         (slot-bytes (read-sector device slot-lba))
                                                         (entry-bytes (%fat32-build-dir-entry-bytes name-bytes #x10 new-cluster 0)))
                                                    (if (null slot-bytes)
                                                        nil
                                                        (progn
                                                          (%fat32-patch-bytes! slot-bytes slot-offset entry-bytes)
                                                          (write-sector device slot-lba slot-bytes)))))))))))))))))))))
