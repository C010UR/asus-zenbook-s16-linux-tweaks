KDIR ?= /usr/src/linux-cachyos
PREFIX ?= /usr/local
DKMS_PKG ?= asus-camled/1.0

obj-m := asus-camled.o

all:
	make -C $(KDIR) M=$(PWD) modules CC=clang LLVM=1

clean:
	make -C $(KDIR) M=$(PWD) clean

install:
	install -D -m 755 camled.py $(DESTDIR)$(PREFIX)/bin/camled.py
	install -D -m 644 camled.service $(DESTDIR)$(PREFIX)/lib/systemd/system/camled.service
	systemctl daemon-reload

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/camled.py
	rm -f $(DESTDIR)$(PREFIX)/lib/systemd/system/camled.service
	systemctl daemon-reload

dkms-add:
	sudo dkms add $(PWD)

dkms-build:
	sudo dkms build $(DKMS_PKG)

dkms-install:
	sudo dkms install $(DKMS_PKG)

dkms-remove:
	sudo dkms remove $(DKMS_PKG) --all

dkms: dkms-add dkms-build dkms-install

.PHONY: all clean install uninstall dkms-add dkms-build dkms-install dkms-remove dkms