obj-m += aos.o
all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) LLVM=1 modules
clean:
		make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
NOLLVM:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD)  modules
