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
