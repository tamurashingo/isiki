
PWD = $(shell pwd)

TARGET = esp_dir/EFI/BOOT/BOOTX64.EFI
SRCDIR = src/c
SRC = $(SRCDIR)/main.c


.PHONY: all setup image transpile build run test

all: build


image:
	docker build -t isiki-builder .

build: $(SRC)
	mkdir -p esp_dir/EFI/BOOT
	docker run --rm --user "$$(id -u):$$(id -g)" --entrypoint x86_64-w64-mingw32-gcc -v "$(PWD)":/workspace uefi-builder \
		-nostdlib -mno-red-zone -mgeneral-regs-only -O1 -shared \
		-mno-stack-arg-probe \
		-Wl,--subsystem,10 \
		-Wl,--entry,EfiMain \
		-o $(TARGET) $(SRC)

clean:
	rm -rf esp_dir

run:
	qemu-system-x86_64 \
		-m 256M \
		-drive if=pflash,format=raw,readonly=on,file=/opt/homebrew/opt/qemu/share/qemu/edk2-x86_64-code.fd \
		-drive format=raw,file=fat:rw:./esp_dir

