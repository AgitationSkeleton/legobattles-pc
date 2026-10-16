// FUN_0202d1b4 @ 0x0202D1B4   entry 0202d1b4   body 680 bytes
// Ghidra decompiler output. READ ONLY -- see playbook/01-what-ghidra-silently-drops.md before
// treating any of this as the function's real behaviour.


void FUN_0202d1b4(int param_1,int param_2,int param_3)

{
  undefined1 auStack_20 [16];
  
  if (*(int *)(param_1 + 8) == 0) {
    switch(*(undefined4 *)(param_1 + 4)) {
    case 0:
      *DAT_0202d460 = -param_2 & DAT_0202d45c | (DAT_0202d45c & -param_3) << 0x10;
      return;
    case 1:
      *DAT_0202d464 = -param_2 & DAT_0202d45c | (DAT_0202d45c & -param_3) << 0x10;
      return;
    case 2:
      *DAT_0202d468 = -param_2 & DAT_0202d45c | (DAT_0202d45c & -param_3) << 0x10;
      return;
    case 3:
      *DAT_0202d46c = -param_2 & DAT_0202d45c | (DAT_0202d45c & -param_3) << 0x10;
      return;
    case 4:
      *DAT_0202d470 = -param_2 & DAT_0202d45c | (DAT_0202d45c & -param_3) << 0x10;
      return;
    case 5:
      *DAT_0202d474 = -param_2 & DAT_0202d45c | (DAT_0202d45c & -param_3) << 0x10;
      return;
    case 6:
      *DAT_0202d478 = -param_2 & DAT_0202d45c | (DAT_0202d45c & -param_3) << 0x10;
      return;
    case 7:
      *DAT_0202d47c = -param_2 & DAT_0202d45c | (DAT_0202d45c & -param_3) << 0x10;
      return;
    }
  }
  else {
    if (2 < *(int *)(param_1 + 8) - 1U) {
      return;
    }
    FUN_02109234(auStack_20);
    switch(*(undefined4 *)(param_1 + 4)) {
    case 0:
      break;
    case 1:
      break;
    case 2:
      FUN_0210c048(DAT_0202d480,auStack_20,param_2 + 0x40,param_3 + 0x40);
      return;
    case 3:
      FUN_0210c048(DAT_0202d484,auStack_20,param_2 + 0x40,param_3 + 0x40);
      return;
    case 4:
      break;
    case 5:
      break;
    case 6:
      FUN_0210c048(DAT_0202d488,auStack_20,param_2 + 0x40,param_3 + 0x40);
      return;
    case 7:
      FUN_0210c048(DAT_0202d48c,auStack_20,param_2 + 0x40,param_3 + 0x40);
    }
  }
  return;
}


