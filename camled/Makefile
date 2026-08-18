KDIR ?= /usr/src/linux-cachyos
PREFIX ?= /usr/local

obj-m := asus-camled.o

all:
	make -C $(KDIR) M=$(PWD) modules CC=clang LLVM=1

clean:
	make -C $(KDIR) M=$(PWD) clean

install:
	install -D -m 644 asus-camled.ko $(DESTDIR)$(PREFIX)/lib/modules/asus-camled.ko
	install -D -m 755 camled.py $(DESTDIR)$(PREFIX)/bin/camled.py
	install -D -m 644 camled.service $(DESTDIR)$(PREFIX)/lib/systemd/system/camled.service
	systemctl daemon-reload

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/lib/modules/asus-camled.ko
	rm -f $(DESTDIR)$(PREFIX)/bin/camled.py
	rm -f $(DESTDIR)$(PREFIX)/lib/systemd/system/camled.service
	systemctl daemon-reload