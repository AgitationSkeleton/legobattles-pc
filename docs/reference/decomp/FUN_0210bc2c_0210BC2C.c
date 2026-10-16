// FUN_0210bc2c @ 0x0210BC2C   entry 0210bc2c   body 48 bytes
// Ghidra decompiler output. READ ONLY -- see playbook/01-what-ghidra-silently-drops.md before
// treating any of this as the function's real behaviour.


/* WARNING: Globals starting with '_' overlap smaller symbols at the same address */

int FUN_0210bc2c(void)

{
  return ((_DAT_04000000 & 0x38000000) >> 0x1b) * 0x10000 + 0x6000000 +
         ((int)(*DAT_0210bc5c & 0x1f00) >> 8) * 0x800;
}


