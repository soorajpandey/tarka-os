# Tarka OS build.
#
#   make          build build/tarka.iso
#   make run      boot it in QEMU (serial on stdio)
#   make test     build with in-kernel self-tests and run them in QEMU
#   make clean

CC      ?= gcc
LD      ?= ld
QEMU    ?= qemu-system-x86_64

LIMINE_DIR := third_party/limine

# Hardening is not optional: these flags are part of the security model.
# See docs/SECURITY.md before removing any of them.
CFLAGS := -std=gnu11 -O2 -g -pipe \
    -Wall -Wextra -Werror -Wvla -Wformat=2 -Wimplicit-fallthrough \
    -Wstrict-prototypes -Wmissing-prototypes -Wshadow -Wundef \
    -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns \
    -fno-pic -fno-pie -mcmodel=kernel -mno-red-zone -mgeneral-regs-only \
    -fno-strict-aliasing -fno-strict-overflow -fno-delete-null-pointer-checks \
    -fstack-protector-strong -mstack-protector-guard=global \
    -fstack-clash-protection -Wframe-larger-than=1024 \
    -ftrivial-auto-var-init=zero \
    -fcf-protection=full \
    -fno-omit-frame-pointer -fno-asynchronous-unwind-tables \
    -Ikernel/include -Ikernel/third_party/limine \
    -DLIMINE_API_REVISION=3

ASFLAGS := -Wa,--noexecstack

LDFLAGS := -nostdlib -static -z max-page-size=0x1000 -z noexecstack \
    --no-dynamic-linker -T kernel/linker.ld

SRCS := $(shell find kernel -name '*.c' -o -name '*.S' | sort)

# Normal and self-test kernels are built into separate trees so a test
# build can never be mistaken for a release build.
define kernel_rules
$(1)/obj/%.c.o: %.c $$(shell find kernel -name '*.h')
	@mkdir -p $$(dir $$@)
	@echo "  CC      $$@"
	@$$(CC) $$(CFLAGS) $(2) -c $$< -o $$@

$(1)/obj/%.S.o: %.S
	@mkdir -p $$(dir $$@)
	@echo "  AS      $$@"
	@$$(CC) $$(CFLAGS) $$(ASFLAGS) $(2) -c $$< -o $$@

$(1)/tarka.elf: $$(SRCS:%=$(1)/obj/%.o) kernel/linker.ld
	@echo "  LD      $$@"
	@$$(LD) $$(LDFLAGS) $$(SRCS:%=$(1)/obj/%.o) -o $$@

$(1)/tarka.iso: $(1)/tarka.elf limine.conf $(LIMINE_DIR)/limine
	@rm -rf $(1)/iso
	@mkdir -p $(1)/iso/boot/limine $(1)/iso/EFI/BOOT
	cp $(1)/tarka.elf $(1)/iso/boot/tarka.elf
	cp limine.conf $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin \
	   $(LIMINE_DIR)/limine-uefi-cd.bin $(1)/iso/boot/limine/
	cp $(LIMINE_DIR)/BOOTX64.EFI $(1)/iso/EFI/BOOT/
	xorriso -as mkisofs -quiet -R -r -J \
	    -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table \
	    -hfsplus -apm-block-size 2048 \
	    --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
	    --protective-msdos-label $(1)/iso -o $$@
	$(LIMINE_DIR)/limine bios-install $$@ 2>/dev/null
endef

$(eval $(call kernel_rules,build/release,))
$(eval $(call kernel_rules,build/test,-DTARKA_SELFTEST))

.PHONY: all iso run test clean limine

all: iso
iso: build/release/tarka.iso

$(LIMINE_DIR)/limine:
	scripts/fetch-limine.sh $(LIMINE_DIR)
	$(MAKE) -C $(LIMINE_DIR) CC=cc

limine: $(LIMINE_DIR)/limine

run: build/release/tarka.iso
	$(QEMU) -M q35 -cpu max -m 256M -cdrom $< -serial stdio -no-reboot

test: build/test/tarka.iso
	scripts/check-address-types.sh
	scripts/run-tests.sh $<

clean:
	rm -rf build
