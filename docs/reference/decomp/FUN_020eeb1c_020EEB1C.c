// FUN_020eeb1c @ 0x020EEB1C   entry 020eeb1c   body 152 bytes
// Ghidra decompiler output. READ ONLY -- see playbook/01-what-ghidra-silently-drops.md before
// treating any of this as the function's real behaviour.


void FUN_020eeb1c(int param_1,int param_2,undefined4 param_3)

{
  undefined4 uVar1;
  uint uVar2;
  
  FUN_020eebb8();
  *(int *)(param_1 + 8) = param_2;
  uVar2 = param_2 + 7U >> 3;
  *(undefined4 *)(param_1 + 0xc) = param_3;
  *(uint *)(param_1 + 4) = uVar2;
  uVar1 = FUN_0204c6e4(uVar2 * *(int *)(param_1 + 0xc),DAT_020eebb4,0x2e);
  *(undefined4 *)(param_1 + 0x10) = uVar1;
  uVar1 = FUN_0204c6e4(*(int *)(param_1 + 0xc) << 2,DAT_020eebb4,0x31);
  *(undefined4 *)(param_1 + 0x14) = uVar1;
  uVar2 = 0;
  if (*(int *)(param_1 + 0xc) != 0) {
    do {
      *(uint *)(*(int *)(param_1 + 0x14) + uVar2 * 4) =
           *(int *)(param_1 + 4) * uVar2 + *(int *)(param_1 + 0x10);
      uVar2 = uVar2 + 1;
    } while (uVar2 < *(uint *)(param_1 + 0xc));
  }
  FUN_020eeaf0(param_1);
  return;
}


