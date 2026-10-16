// FUN_020396e0 @ 0x020396E0   entry 020396e0   body 452 bytes
// Ghidra decompiler output. READ ONLY -- see playbook/01-what-ghidra-silently-drops.md before
// treating any of this as the function's real behaviour.


void FUN_020396e0(int param_1,undefined4 param_2,undefined4 param_3,undefined4 param_4)

{
  int iVar1;
  undefined4 uVar2;
  byte bVar3;
  byte bVar4;
  short *psVar5;
  int iVar6;
  undefined2 *puVar7;
  undefined1 auStack_30 [4];
  short local_2c;
  short local_2a;
  undefined1 auStack_28 [4];
  short local_24;
  short local_22;
  undefined4 uStack_20;
  
  uStack_20 = param_4;
  if (*(char *)(param_1 + 0x66) != '\0') {
    iVar6 = (int)*(short *)(param_1 + 0x5e) >> 0x1f;
    iVar1 = (int)*(short *)(param_1 + 0x60) >> 0x1f;
    FUN_020f2a50(auStack_28,
                 (int)((4 - (((uint)(*(short *)(param_1 + 0x5e) * 0x20000000 + iVar6) >> 0x1d |
                             iVar6 << 3) - iVar6)) * 0x10000) >> 0x10,
                 (int)((((uint)(*(short *)(param_1 + 0x60) * 0x10000000 + iVar1) >> 0x1c |
                        iVar1 << 4) - iVar1) * -0x10000) >> 0x10);
    FUN_0202d1b4(*(undefined4 *)(param_1 + 4),(int)local_24,(int)local_22);
    FUN_0210eaf4(DAT_020398a8,0x680);
    FUN_0210ca60(DAT_020398a8,0,0x680);
    *(undefined1 *)(param_1 + 0x66) = 0;
  }
  if (*(char *)(param_1 + 0x67) != '\0') {
    iVar6 = (int)*(short *)(param_1 + 0x5e) >> 0x1f;
    iVar1 = (int)*(short *)(param_1 + 0x60) >> 0x1f;
    FUN_020f2a50(auStack_30,
                 (int)((4 - (((uint)(*(short *)(param_1 + 0x5e) * 0x20000000 + iVar6) >> 0x1d |
                             iVar6 << 3) - iVar6)) * 0x10000) >> 0x10,
                 (int)((((uint)(*(short *)(param_1 + 0x60) * 0x10000000 + iVar1) >> 0x1c |
                        iVar1 << 4) - iVar1) * -0x10000) >> 0x10);
    FUN_0202d1b4(*(undefined4 *)(param_1 + 8),(int)local_2c,(int)local_2a);
    FUN_0210eaf4(DAT_020398ac,0x680);
    FUN_0210cb20(DAT_020398ac,0,0x680);
    *(undefined1 *)(param_1 + 0x67) = 0;
  }
  uVar2 = DAT_020398b0;
  if (*(char *)(param_1 + 0x68) != '\0') {
    bVar4 = 0;
    do {
      bVar3 = 0;
      do {
        psVar5 = (short *)FUN_0203d8e8(bVar3,bVar4);
        if ((*psVar5 != 0x1f) && (iVar6 = FUN_020eebf4(uVar2,bVar3,bVar4), iVar6 != 0)) {
          puVar7 = (undefined2 *)FUN_0203d8e8(bVar3,bVar4);
          *puVar7 = 0x1f;
        }
        bVar3 = bVar3 + 1;
      } while (bVar3 < 0x20);
      bVar4 = bVar4 + 1;
    } while (bVar4 < 0x18);
    FUN_0210eaf4(DAT_020398b4,0x680);
    FUN_0210cbe0(DAT_020398b4,0,0x680);
    *(undefined1 *)(param_1 + 0x68) = 0;
    return;
  }
  return;
}


