# SPDX-License-Identifier: GPL-2.0
#
# Makefile for s390 specific build tools
#

kapi := arch/$(ARCH)/include/generated/asm
kapi-hdrs-y := $(kapi)/facility-defs.h $(kapi)/dis-defs.h

PHONY += kapi

kapi:	$(kapi-hdrs-y)

hostprogs		    += gen_facilities
hostprogs		    += gen_opcode_table

HOSTCFLAGS_gen_facilities.o += $(LINUXINCLUDE)

filechk_facility-defs.h = $(obj)/gen_facilities

filechk_dis-defs.h = \
	$(obj)/gen_opcode_table < $(srctree)/arch/$(ARCH)/tools/opcodes.txt

$(kapi)/facility-defs.h: $(obj)/gen_facilities FORCE
	$(call filechk,facility-defs.h)

$(kapi)/dis-defs.h: $(obj)/gen_opcode_table FORCE
	$(call filechk,dis-defs.h)

hostprogs	+= relocs
PHONY		+= relocs
relocs: $(obj)/relocs
	@:
