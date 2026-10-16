// FUN_0203d488 @ 0x0203D488   entry 0203d488   body 176 bytes
// Ghidra decompiler output. READ ONLY -- see playbook/01-what-ghidra-silently-drops.md before
// treating any of this as the function's real behaviour.


void FUN_0203d488(undefined4 param_1)

{
  int iVar1;
  int iVar2;
  int iVar3;
  undefined1 auStack_60 [8];
  undefined1 auStack_58 [8];
  undefined1 auStack_50 [64];
  
  FUN_020f2ba0(auStack_58);
  FUN_020f2ba0(auStack_60);
  FUN_02038f28(param_1,auStack_58,auStack_60);
  FUN_020ef118(auStack_50,1);
  FUN_020882bc(auStack_50,auStack_58,auStack_60);
  iVar3 = 0;
  iVar1 = FUN_020ef1d0(auStack_50);
  if (0 < iVar1) {
    do {
      FUN_020ef1e0(auStack_50,iVar3);
      iVar1 = FUN_020872dc();
      if ((iVar1 != 0) && (iVar2 = FUN_02036cd0(), iVar2 != 0)) {
        FUN_0203d148(iVar1);
      }
      iVar3 = iVar3 + 1;
      iVar1 = FUN_020ef1d0(auStack_50);
    } while (iVar3 < iVar1);
  }
  FUN_020ef144(auStack_50);
  return;
}


