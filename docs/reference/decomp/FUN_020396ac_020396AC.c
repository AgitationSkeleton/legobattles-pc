// FUN_020396ac @ 0x020396AC   entry 020396ac   body 44 bytes
// Ghidra decompiler output. READ ONLY -- see playbook/01-what-ghidra-silently-drops.md before
// treating any of this as the function's real behaviour.


void FUN_020396ac(int param_1,undefined4 param_2,undefined4 param_3,undefined4 param_4)

{
  if (*(char *)(param_1 + 0x66) == '\0') {
    return;
  }
  FUN_020a3234(*DAT_020396d8,DAT_020396dc,(int)*(short *)(param_1 + 0x5e),
               (int)*(short *)(param_1 + 0x60),param_4);
  return;
}


