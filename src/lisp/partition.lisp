;;;; パーティションテーブル(GPT/レガシーMBR)を解析し、パーティションを*devices*へ
;;;; blk0s0,blk0s1,...として登録するAPI。ide.lisp/device.lispと同じ理由でAOT
;;;; トランスパイル対象外の、通常のload形式(インタプリタ実行専用)ファイル。
;;;; REPLから(load "src/lisp/device.lisp")→(load "src/lisp/ide.lisp")の後に
;;;; (load "src/lisp/partition.lisp")で明示的に読み込む(fat16.lisp/fat32.lispより前)。
;;;; fat16.lisp/fat32.lispとコードを共有しない独立ファイル(ide.lisp/device.lispと
;;;; 同じ「個別ドライバは自己完結ファイル」という方針)。
;;;;
;;;; ロード時に、その時点で*devices*に登録済みの生ディスク(:type 'blk、ide.lispが
;;;; 登録したもの)全てへ対してGPT/MBRの検出・パーティション登録を1回だけ実行する。
;;;; 検出前に*devices*のスナップショット(登録済みalistそのもの)を確定させてから
;;;; 走査するため、このファイルが新規登録したパーティション自身を再度検査すること
;;;; はない。
;;;;
;;;; PART-M2でGPT対応、PART-M3でレガシーMBR対応を実装する。

;; (%partition-u16 bytes offset) (%partition-u32 bytes offset) : fat32.lispの
;; %fat32-u16/%fat32-u32と同じLE整数合成(bytesはread-sectorの戻り値、fixnum(0-255)の
;; リスト)。コードを共有しない方針のため同じ内容をここに複製する。
(defun %partition-u16 (bytes offset)
  (logior (elt bytes offset) (ash (elt bytes (+ offset 1)) 8)))

(defun %partition-u32 (bytes offset)
  (logior (%partition-u16 bytes offset) (ash (%partition-u16 bytes (+ offset 2)) 16)))

;; (%partition-bytes-zero-p bytes offset count) : bytesのoffsetからcount byte分が
;; 全てゼロかどうか(GPTパーティションエントリのtype GUIDが未使用スロットかどうかの
;; 判定に使う)。
(defun %partition-bytes-zero-p (bytes offset count)
  (let ((i 0) (ok t))
    (progn
      (while (and ok (< i count))
        (progn
          (if (= (elt bytes (+ offset i)) 0) nil (setq ok nil))
          (setq i (+ i 1))))
      ok)))

;;; --- GPT ---

;; (%gpt-signature-p handle) : handleのLBA1先頭8byteが"EFI PART"かどうか。
;; read-sectorが失敗した場合はnil。
(defun %gpt-signature-p (handle)
  (let ((bytes (read-sector handle 1)))
    (and bytes
         (= (elt bytes 0) 69) (= (elt bytes 1) 70) (= (elt bytes 2) 73) (= (elt bytes 3) 32)
         (= (elt bytes 4) 80) (= (elt bytes 5) 65) (= (elt bytes 6) 82) (= (elt bytes 7) 84))))

;; GPTパーティションエントリ配列はentry-size(通常128byte)ごとに複数セクタへ跨るため、
;; fat32.lispの%fat32-fat-sector-bytesと同じキャッシュ方式(直前に読んだセクタのlba/
;; 内容を保持し、同一セクタへの重複read-sectorを避ける)を使う。GPT解析はディスク単位で
;; しか呼ばれないため、キャッシュはグローバルに1系統のみで良い(呼び出し前に必ず
;; 初期化する)。
(defdynamic *gpt-entry-cache-lba* nil)
(defdynamic *gpt-entry-cache-bytes* nil)

;; (%gpt-entry-sector-bytes handle lba) : GPTパーティションエントリ配列中のlbaセクタの
;; 内容(512byte)を返す。直前に読んだセクタと同じlbaならキャッシュを再利用する。
(defun %gpt-entry-sector-bytes (handle lba)
  (if (and (dynamic *gpt-entry-cache-lba*) (= (dynamic *gpt-entry-cache-lba*) lba))
      (dynamic *gpt-entry-cache-bytes*)
      (let ((bytes (read-sector handle lba)))
        (progn
          (%%set-dynamic '*gpt-entry-cache-lba* lba)
          (%%set-dynamic '*gpt-entry-cache-bytes* bytes)
          bytes))))

;; (%gpt-partition-name-string bytes offset) : offsetから始まる72byte(UTF-16LE、
;; 36文字分)のパーティション名を、各文字の下位byteだけをASCIIとして復元した文字列に
;; する(ASCII名では上位byteは常に0という前提)。コード0(NUL終端)で読み取りを止める。
(defun %gpt-partition-name-string (bytes offset)
  (let ((out (create-string-output-stream)) (i 0) (done nil))
    (progn
      (while (and (not done) (< i 36))
        (let ((code (elt bytes (+ offset (* i 2)))))
          (if (= code 0)
              (setq done t)
              (progn
                (format out "~C" code)
                (setq i (+ i 1))))))
      (get-output-stream-string out))))

;; (%gpt-register-partitions parent-name handle) : handle(生ディスクハンドル)のLBA1を
;; GPTヘッダとして解析し、使用中の各パーティションエントリをparent-nameS0,
;; parent-nameS1,...(%device-register-blk-slice)として*devices*へ登録する。
;; ヘッダのentries-start-lba/num-entries/entry-sizeは実測済みの標準オフセット
;; (+72/+80/+84、documents/partition.md PART-M0参照)から読む。read-sectorが失敗した
;; 場合は何もしない。
(defun %gpt-register-partitions (parent-name handle)
  (let ((header (read-sector handle 1)))
    (if (null header)
        nil
        (let ((entries-start-lba (%partition-u32 header 72))
              (num-entries (%partition-u32 header 80))
              (entry-size (%partition-u32 header 84)))
          (progn
            (%%set-dynamic '*gpt-entry-cache-lba* nil)
            (%%set-dynamic '*gpt-entry-cache-bytes* nil)
            (let ((index 0) (slice-index 0))
              (progn
                (while (< index num-entries)
                  (let* ((byte-offset (* index entry-size))
                         (sector-offset (div byte-offset 512))
                         (offset-in-sector (mod byte-offset 512))
                         (bytes (%gpt-entry-sector-bytes handle (+ entries-start-lba sector-offset))))
                    (progn
                      (if (or (null bytes) (%partition-bytes-zero-p bytes offset-in-sector 16))
                          nil
                          (let* ((first-lba (%partition-u32 bytes (+ offset-in-sector 32)))
                                 (last-lba (%partition-u32 bytes (+ offset-in-sector 40))))
                            (progn
                              (%device-register-blk-slice parent-name slice-index
                                (list ':type 'blk
                                      ':handle (%ide-make-partition-handle handle first-lba)
                                      ':name (%gpt-partition-name-string bytes (+ offset-in-sector 56))
                                      ':model ""
                                      ':total-sectors (+ (- last-lba first-lba) 1)))
                              (setq slice-index (+ slice-index 1)))))
                      (setq index (+ index 1)))))
                nil)))))))

;;; --- レガシーMBR ---

;; (%mbr-boot-signature-p bytes) : LBA0のoffset510-511が0x55AA(boot signature)か。
;; bytesがnil(read-sector失敗)ならnil。
(defun %mbr-boot-signature-p (bytes)
  (and bytes (= (elt bytes 510) 85) (= (elt bytes 511) 170)))

;; (%mbr-entry-type-byte bytes index) : LBA0のプライマリパーティションエントリ
;; (offset446、16byte×4、index=0-3)のtypeバイト(entry+4)を返す。
(defun %mbr-entry-type-byte (bytes index)
  (elt bytes (+ 446 (* index 16) 4)))

;; (%mbr-has-nonzero-type-p bytes index) : index番目以降のエントリのいずれかに
;; typeが非ゼロのものがあるか(再帰、4エントリのみなので素の再帰で十分)。
(defun %mbr-has-nonzero-type-p (bytes index)
  (if (>= index 4)
      nil
      (if (= (%mbr-entry-type-byte bytes index) 0)
          (%mbr-has-nonzero-type-p bytes (+ index 1))
          t)))

;; (%mbr-signature-p handle) : handleのLBA0がレガシーMBRパーティションテーブルを
;; 持つかどうか。0x55AA(boot signature)だけでは無分割FATボリュームのBPBと区別
;; できないため(documents/partition.md PART-M0で実測確認)、4エントリのうち
;; 少なくとも1つのtypeバイトが非ゼロであることも必須条件にする。
(defun %mbr-signature-p (handle)
  (let ((bytes (read-sector handle 0)))
    (and (%mbr-boot-signature-p bytes) (%mbr-has-nonzero-type-p bytes 0))))

;; (%mbr-register-partitions parent-name handle) : handle(生ディスクハンドル)のLBA0を
;; MBRパーティションテーブルとして解析し、プライマリ4エントリのうち対応対象の
;; ものをparent-nameS0,parent-nameS1,...として*devices*へ登録する。type=0x00
;; (未使用)・0xEE(GPTプロテクティブ、GPT判定はこの関数より先に試すため通常は
;; 出現しないはずだが防御的にスキップ)・0x05/0x0F(拡張/論理、スコープ外)は
;; スキップする。MBRエントリにはGPTのようなパーティション名フィールドが無いため
;; :nameは空文字列にする。
(defun %mbr-register-partitions (parent-name handle)
  (let ((bytes (read-sector handle 0)))
    (if (null bytes)
        nil
        (let ((index 0) (slice-index 0))
          (progn
            (while (< index 4)
              (let ((type (%mbr-entry-type-byte bytes index)))
                (progn
                  (if (or (= type 0) (= type 238) (= type 5) (= type 15))
                      nil
                      (let* ((entry-offset (+ 446 (* index 16)))
                             (start-lba (%partition-u32 bytes (+ entry-offset 8)))
                             (num-sectors (%partition-u32 bytes (+ entry-offset 12))))
                        (progn
                          (%device-register-blk-slice parent-name slice-index
                            (list ':type 'blk
                                  ':handle (%ide-make-partition-handle handle start-lba)
                                  ':name ""
                                  ':model ""
                                  ':total-sectors num-sectors))
                          (setq slice-index (+ slice-index 1)))))
                  (setq index (+ index 1)))))
            nil)))))

;;; --- 検出の起点 ---

;; (%partition-register-for-disk name info) : name/infoが生ディスク(:type 'blk)なら
;; まずGPTを検出し(見つかればMBR判定は行わない、プロテクティブMBRの誤登録を防ぐ
;; ため)、GPTが無ければレガシーMBRを検出する。どちらも無ければ何もしない
;; (パーティション無しの既存fat16_test.img等)。
(defun %partition-register-for-disk (name info)
  (if (eq (%plist-get info ':type nil) 'blk)
      (let ((handle (%plist-get info ':handle nil)))
        (if (%gpt-signature-p handle)
            (%gpt-register-partitions name handle)
            (if (%mbr-signature-p handle)
                (%mbr-register-partitions name handle)
                nil)))
      nil))

;; (%partition-register-for-all-disks-loop alist) : alist(ロード時点の*devices*の
;; スナップショット)の各エントリへ%partition-register-for-diskを適用する再帰ヘルパー
;; (%ide-register-devices-loopと同様、デバイス数は現状最大4台程度なので素の再帰で
;; 十分)。
(defun %partition-register-for-all-disks-loop (alist)
  (if (null alist)
      nil
      (progn
        (%partition-register-for-disk (car (car alist)) (cdr (car alist)))
        (%partition-register-for-all-disks-loop (cdr alist)))))

;; (%partition-register-for-all-disks) : ロード時点で*devices*に登録済みの生ディスク
;; 全てに対してパーティション検出・登録を行う。
(defun %partition-register-for-all-disks ()
  (%partition-register-for-all-disks-loop (dynamic *devices*)))

(%partition-register-for-all-disks)
