// FUN_0210cb20 @ 0x0210CB20   entry 0210cb20   body 92 bytes
// Ghidra decompiler output. READ ONLY -- see playbook/01-what-ghidra-silently-drops.md before
// treating any of this as the function's real behaviour.


void FUN_0210cb20(undefined4 param_1,int param_2,uint param_3)

{
  int iVar1;
  
  iVar1 = FUN_0210bc80();
  if ((*DAT_0210cb7c != -1) && (0x1c < param_3)) {
    FUN_0210fe48(*DAT_0210cb7c,param_1,iVar1 + param_2,param_3);
    return;
  }
  FUN_021104dc(param_1,iVar1 + param_2,param_3);
  return;
}


