// FUN_02034b44 @ 0x02034B44   entry 02034b44   body 556 bytes
// Ghidra decompiler output. READ ONLY -- see playbook/01-what-ghidra-silently-drops.md before
// treating any of this as the function's real behaviour.


void FUN_02034b44(int param_1,short *param_2,short *param_3)

{
  byte bVar1;
  short sVar2;
  int iVar3;
  int iVar4;
  char *pcVar5;
  int iVar6;
  short sVar7;
  byte *pbVar8;
  bool bVar9;
  
  FUN_020a4524(*DAT_02034d70);
  iVar3 = FUN_02034b1c(param_1);
  if (iVar3 == 0) {
    return;
  }
  iVar4 = FUN_020eec14(*(undefined4 *)(*DAT_02034d74 + 0x14),param_1);
  if (iVar4 == 0) {
    pcVar5 = (char *)FUN_020092a8();
    if ((*pcVar5 == '\0') && (iVar6 = FUN_020092a8(), *(char *)(iVar6 + 0x1f) == '\0')) {
      FUN_020a186c();
      iVar6 = FUN_020a223c();
      if (iVar6 == 0) goto LAB_02034bc0;
    }
    iVar4 = 1;
  }
LAB_02034bc0:
  if (iVar4 == 0) {
    return;
  }
  iVar6 = FUN_0205e000(iVar3);
  iVar4 = DAT_02034d78;
  if ((*(byte *)(param_1 + 4) & 1) == 0) {
    iVar4 = DAT_02034d7c;
  }
  pbVar8 = (byte *)(iVar6 * 6 + iVar4);
  bVar9 = *(char *)(iVar3 + 0x71) != '\0';
  if (bVar9) {
    param_1 = 4;
  }
  sVar7 = (short)param_1;
  if (!bVar9) {
    sVar7 = 0;
  }
  sVar2 = FUN_0211fdb4((uint)*(ushort *)(iVar3 + 0x1a0) * 100,
                       *(undefined2 *)(*(int *)(*(int *)(iVar3 + 0x160) + 8) + 0x62));
  if (sVar2 < 0x21) {
    sVar7 = sVar7 + 0x20;
  }
  else if (sVar2 < 0x42) {
    sVar7 = sVar7 + 0x18;
  }
  bVar1 = *pbVar8;
  if (bVar1 != 0xff) {
    *param_2 = sVar7 + (ushort)bVar1 +
                       *(short *)(DAT_02034d80 +
                                 (int)(short)(bVar1 & 1) *
                                 (int)(short)(ushort)*(byte *)(iVar3 + 0xe4) * 2);
  }
  bVar1 = pbVar8[1];
  if (bVar1 != 0xff) {
    param_2[1] = sVar7 + (ushort)bVar1 +
                         *(short *)(DAT_02034d80 +
                                   (int)(short)(bVar1 & 1) *
                                   (int)(short)(ushort)*(byte *)(iVar3 + 0xe4) * 2);
  }
  bVar1 = pbVar8[2];
  if (bVar1 != 0xff) {
    param_2[2] = sVar7 + (ushort)bVar1 +
                         *(short *)(DAT_02034d80 +
                                   (int)(short)(bVar1 & 1) *
                                   (int)(short)(ushort)*(byte *)(iVar3 + 0xe4) * 2);
  }
  bVar1 = pbVar8[3];
  if (bVar1 != 0xff) {
    *param_3 = sVar7 + (ushort)bVar1 +
                       *(short *)(DAT_02034d80 +
                                 (int)(short)(bVar1 & 1) *
                                 (int)(short)(ushort)*(byte *)(iVar3 + 0xe4) * 2);
  }
  bVar1 = pbVar8[4];
  if (bVar1 != 0xff) {
    param_3[1] = sVar7 + (ushort)bVar1 +
                         *(short *)(DAT_02034d80 +
                                   (int)(short)(bVar1 & 1) *
                                   (int)(short)(ushort)*(byte *)(iVar3 + 0xe4) * 2);
  }
  bVar1 = pbVar8[5];
  if (bVar1 == 0xff) {
    return;
  }
  param_3[2] = sVar7 + (ushort)bVar1 +
                       *(short *)(DAT_02034d80 +
                                 (int)(short)(bVar1 & 1) *
                                 (int)(short)(ushort)*(byte *)(iVar3 + 0xe4) * 2);
  return;
}


