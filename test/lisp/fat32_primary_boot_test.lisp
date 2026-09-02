;; FAT32-M9: Primary IDE HDD(BOOT_FAT32_IMG)がos_block_device_probe_allから
;; ide-primary-masterとして検出できていること、およびPrimary追加によって
;; 既存のblk0=ide-secondary-master(fat16_test.lisp/fat32_test.lispが
;; (%device-handle 'blk0)で前提にしている登録順)が壊れていないことを確認する。

;; (%fat32-primary-boot-find-by-name name alist) : *devices*と同形式のalistから
;; :nameがnameと一致する最初のinfoプリストを返す(無ければnil)。
(defun %fat32-primary-boot-find-by-name (name alist)
  (if (null alist)
      nil
      (if (equal (%plist-get (cdr (car alist)) ':name "") name)
          (cdr (car alist))
          (%fat32-primary-boot-find-by-name name (cdr alist)))))

;; ide-secondary-masterが引き続きblk0であること(既存テストの前提の回帰確認)。
(assert-equal "ide-secondary-master"
              (%plist-get (cdr (assoc 'blk0 (dynamic *devices*))) ':name ""))

;; ide-primary-masterがPrimaryチャネルのプローブにより*devices*へ登録されていること。
(let ((primary (%fat32-primary-boot-find-by-name "ide-primary-master" (dynamic *devices*))))
  (progn
    (assert-equal t (if primary t nil))
    (assert-equal t (if (%plist-get primary ':handle nil) t nil))
    (assert-equal t (> (%plist-get primary ':total-sectors 0) 0))))

;; PART-M4: images/boot_fat32.img(GPT+ESP+FAT32、FAT32-M9で作成)がPrimaryチャネルに
;; アタッチされている前提で、partition.lispがESPパーティションをスライスとして
;; 検出・登録し、そのスライス経由でfat32-read-fileが実際に/EFI/BOOT/BOOTX64.EFIを
;; 読めること、および親の生ディスクハンドルはFAT16/FAT32どちらでもないことを実証する
;; (documents/fat32.md FAT32-M9で「スコープ外」としていたギャップの解消)。

;; (%fat32-primary-boot-find-slice-by-base base-handle alist) : alist(*devices*と
;; 同形式)の中から、:handleがbase-handle上のパーティションハンドル
;; (%ide-partition-handle-p)であるものを探し、そのハンドルを返す(無ければnil)。
(defun %fat32-primary-boot-find-slice-by-base (base-handle alist)
  (if (null alist)
      nil
      (let ((h (%plist-get (cdr (car alist)) ':handle nil)))
        (if (and (%ide-partition-handle-p h) (eq (%ide-partition-base-handle h) base-handle))
            h
            (%fat32-primary-boot-find-slice-by-base base-handle (cdr alist))))))

(let* ((primary (%fat32-primary-boot-find-by-name "ide-primary-master" (dynamic *devices*)))
       (primary-handle (%plist-get primary ':handle nil))
       (esp-handle (%fat32-primary-boot-find-slice-by-base primary-handle (dynamic *devices*))))
  (progn
    ;; ESPパーティションがpartition.lispによりスライスとして検出されていること。
    (assert-equal t (if esp-handle t nil))
    (assert-equal t (if (%device-fat32-uuid esp-handle) t nil))
    ;; ESPスライス経由でfat32-read-fileが実際にBOOTX64.EFI(PE実行ファイル、
    ;; 先頭2byteは"MZ"=77,90)を読めること。
    (let ((bytes (fat32-read-file esp-handle "/EFI/BOOT/BOOTX64.EFI")))
      (progn
        (assert-equal t (if bytes t nil))
        (assert-equal 77 (car bytes))
        (assert-equal 90 (car (cdr bytes)))))
    ;; 親の生ディスクハンドル(ide-primary-master自身)は、LBA0がGPTのプロテクティブ
    ;; MBRであるためFAT16/FAT32どちらでもない(「ディスク全体はFAT16/FAT32では
    ;; アクセスできない」、documents/devices.mdの実証)。
    (assert-equal nil (%device-fat16-uuid primary-handle))
    (assert-equal nil (%device-fat32-uuid primary-handle))))
