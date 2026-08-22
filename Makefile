K=kernel
U=user

OBJS = \
  $K/entry.o \
  $K/kernelvec.o \
  $K/trampoline.o \
  $K/specialreg.o \
  $K/start.o \
  $K/trap.o \
  $K/bootinfo.o \
  $K/cpu.o \
  $K/kcontext.o \
  $K/swtch.o \
  $K/proc.o \
  $K/syscall.o \
  $K/sysproc.o \
  $K/early_selftest.o \
  $K/intc.o \
  $K/timer.o \
  $K/kalloc.o \
  $K/spinlock.o \
  $K/sleeplock.o \
  $K/string.o \
  $K/console.o \
  $K/printk.o \
  $K/uart.o \
  $K/virtio_disk.o \
  $K/bio.o \
  $K/fs.o \
  $K/log.o \
  $K/file.o \
  $K/pipe.o \
  $K/vm.o \
  $K/diagnostic.o

CC = clang
LD = ld.lld
OBJDUMP = llvm-objdump
READOBJ = llvm-readobj
QEMU = qemu-system-mmix

CFLAGS = -Wall -Werror -Wno-unknown-attributes -O0 -fno-omit-frame-pointer
CFLAGS += --target=mmix
CFLAGS += -std=gnu99
CFLAGS += -MD
CFLAGS += -ffreestanding
CFLAGS += -fno-common -nostdlib
CFLAGS += -fno-stack-protector -fno-pie
CFLAGS += -fno-builtin-strncpy -fno-builtin-strncmp -fno-builtin-strlen -fno-builtin-memset
CFLAGS += -fno-builtin-memmove -fno-builtin-memcmp -fno-builtin-log -fno-builtin-bzero
CFLAGS += -fno-builtin-strchr -fno-builtin-exit -fno-builtin-malloc -fno-builtin-putc
CFLAGS += -fno-builtin-free
CFLAGS += -fno-builtin-memcpy -Wno-main
CFLAGS += -fno-builtin-printf -fno-builtin-fprintf -fno-builtin-vprintf
CFLAGS += -I.

ASFLAGS = --target=mmix
LDFLAGS = -m elf64mmix
# exec requires PT_LOAD offsets and alignment to match 8-KiB MMIX pages.
USER_LDFLAGS = $(LDFLAGS) -z max-page-size=8192

$K/kernel: $(OBJS) $K/kernel.ld
	$(LD) $(LDFLAGS) -T $K/kernel.ld -o $K/kernel $(OBJS) 
	$(OBJDUMP) -S $K/kernel > $K/kernel.asm
	$(OBJDUMP) -t $K/kernel | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $K/kernel.sym

$K/%.o: $K/%.S
	$(CC) $(ASFLAGS) -c -o $@ $<

QEMUOPTS = -machine virt
QEMUOPTS += -smp 1
QEMUOPTS += -display none
QEMUOPTS += -serial stdio
QEMUOPTS += -monitor none
QEMUOPTS += -global virtio-mmio.force-legacy=false
QEMUOPTS += -drive file=fs.img,if=none,format=raw,id=x0
QEMUOPTS += -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0

qemu: $K/kernel fs.img
	$(QEMU) $(QEMUOPTS) -kernel $K/kernel

tags: $(OBJS)
	etags kernel/*.S kernel/*.c

ULIB = $U/ulib.o $U/usys.o $U/printf.o $U/umalloc.o

_%: %.o $(ULIB) $U/user.ld
	$(LD) $(USER_LDFLAGS) -T $U/user.ld -o $@ $< $(ULIB)
	$(OBJDUMP) -S $@ > $*.asm
	$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $*.sym
	$(READOBJ) --file-headers --program-headers --sections --symbols \
	  --relocations $@ > $*.readobj

$U/_forktest: $U/forktest.o $U/ulib.o $U/usys.o $U/user.ld
	# Keep forktest small so it can exhaust the process table.
	$(LD) $(USER_LDFLAGS) -T $U/user.ld -o $@ $U/forktest.o $U/ulib.o $U/usys.o
	$(OBJDUMP) -S $@ > $U/forktest.asm
	$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $U/forktest.sym
	$(READOBJ) --file-headers --program-headers --sections --symbols \
	  --relocations $@ > $U/forktest.readobj

$U/%.o: $U/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

$U/%.o: $U/%.S
	$(CC) $(CFLAGS) -c -o $@ $<

$U/usys.S : $U/usys.pl
	perl $U/usys.pl > $U/usys.S

mkfs/mkfs: mkfs/mkfs.c $K/fs.h $K/param.h
	gcc -Wno-unknown-attributes -I. -o mkfs/mkfs mkfs/mkfs.c

# Prevent deletion of intermediate files, e.g. cat.o, after first build, so
# that disk image changes after first build are persistent until clean.  More
# details:
# http://www.gnu.org/software/make/manual/html_node/Chained-Rules.html
.PRECIOUS: %.o
.PRECIOUS: $U/%.o

UPROGS=\
	$U/_cat\
	$U/_echo\
	$U/_forktest\
	$U/_grep\
	$U/_init\
	$U/_kill\
	$U/_ln\
	$U/_ls\
	$U/_mkdir\
	$U/_rm\
	$U/_sh\
	$U/_stressfs\
	$U/_usertests\
	$U/_grind\
	$U/_wc\
	$U/_zombie\
	$U/_logstress\
	$U/_forphan\
	$U/_dorphan\
	$U/_sync\

fs.img: mkfs/mkfs README $(UPROGS)
	mkfs/mkfs fs.img README $(UPROGS)

-include kernel/*.d user/*.d

clean: 
	rm -f *.tex *.dvi *.idx *.aux *.log *.ind *.ilg \
	*/*.o */*.d */*.asm */*.sym */*.readobj \
	$K/kernel fs.img \
	mkfs/mkfs .gdbinit \
        $U/usys.S \
	$(UPROGS)

.PHONY: qemu fmt
fmt:
	clang-format -i $(wildcard kernel/*.[ch] user/*.[ch] mkfs/*.c)
