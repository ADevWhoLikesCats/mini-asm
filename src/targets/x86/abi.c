#include "targets/x86.h"
const TargetDesc target_x86 = { "x86", "i386-linux-gnu", "as --32", ".o", "ld -m elf_i386", 4, 16, true };
