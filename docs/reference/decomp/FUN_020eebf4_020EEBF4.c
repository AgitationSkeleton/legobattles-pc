// FUN_020eebf4 @ 0x020EEBF4   entry 020eebf4   body 32 bytes
// Ghidra decompiler output. READ ONLY -- see playbook/01-what-ghidra-silently-drops.md before
// treating any of this as the function's real behaviour.


bool FUN_020eebf4(int param_1,uint param_2,int param_3)

{
  return ((uint)*(byte *)(*(int *)(*(int *)(param_1 + 0x14) + param_3 * 4) + (param_2 >> 3)) &
         1 << (param_2 & 7)) != 0;
}


