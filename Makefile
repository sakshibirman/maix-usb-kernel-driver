obj-m += maix_drv.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all: module user

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

user:
	gcc -Wall -Wextra -O2 -o test_maix test_maix.c maix_lib.c

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f test_maix
