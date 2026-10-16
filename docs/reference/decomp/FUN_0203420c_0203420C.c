// FUN_0203420c @ 0x0203420C   entry 0203420c   body 884 bytes
// Ghidra decompiler output. READ ONLY -- see playbook/01-what-ghidra-silently-drops.md before
// treating any of this as the function's real behaviour.


void FUN_0203420c(int param_1)

{
  short sVar1;
  uint uVar2;
  undefined4 uVar3;
  uint uVar4;
  int iVar5;
  int iVar6;
  int iVar7;
  uint uVar8;
  int iVar9;
  uint uVar10;
  uint uVar11;
  int iVar12;
  int iVar13;
  short sVar14;
  bool bVar15;
  int iStack_30;
  
  uVar3 = DAT_02034580;
  if (*(int *)(param_1 + 0x70) != 0) {
    FUN_0202d858();
    return;
  }
  iVar7 = *(int *)(param_1 + 0x6c);
  if (iVar7 != 0) {
    if (*(char *)(param_1 + 0x7a) == '\0') {
      sVar14 = *(short *)(iVar7 + 0x5a);
      bVar15 = *(char *)(iVar7 + 0x59) == '\0';
      sVar1 = *(short *)(iVar7 + 0x5c);
      iVar5 = (*(short *)(iVar7 + 8) + -4) * 0x10000;
      if (bVar15) {
        sVar14 = *(short *)(iVar7 + 0xc);
      }
      if (bVar15) {
        sVar1 = *(short *)(iVar7 + 0xe);
      }
      uVar10 = iVar5 >> 0x13;
      if ((int)uVar10 < 0) {
        uVar10 = 0;
      }
      uVar11 = (int)*(short *)(iVar7 + 10) >> 3;
      uVar8 = (int)sVar14 + (iVar5 >> 0x10) >> 3;
      if ((int)uVar11 < 0) {
        uVar11 = 0;
      }
      uVar4 = (int)*(short *)(iVar7 + 10) + (int)sVar1 >> 3;
      if ((int)uVar8 < 0) {
        uVar8 = 0;
      }
      if ((int)uVar4 < 0) {
        uVar4 = 0;
      }
      if (0x20 < (int)uVar8) {
        uVar8 = 0x20;
      }
      uVar10 = uVar10 & 0xff;
      if (0x18 < (int)uVar4) {
        uVar4 = 0x18;
      }
      if (uVar10 < (uVar8 & 0xff)) {
        uVar2 = uVar11;
        do {
          for (; uVar2 = uVar2 & 0xff, uVar2 < (uVar4 & 0xff); uVar2 = uVar2 + 1) {
            FUN_020eec2c(uVar3,uVar10,uVar2,1);
          }
          uVar10 = uVar10 + 1 & 0xff;
          uVar2 = uVar11;
        } while (uVar10 < (uVar8 & 0xff));
      }
    }
    FUN_0202da80(*(undefined4 *)(param_1 + 0x6c));
    return;
  }
  iVar5 = *(int *)(param_1 + 0x74);
  iVar7 = 0;
  if (iVar5 != 0) {
    iVar7 = *(int *)(iVar5 + 0x2c);
  }
  if (iVar5 == 0 || iVar7 == 0) {
    return;
  }
  if (*(char *)(param_1 + 0x79) == '\0') {
    iVar5 = 0;
    uVar8 = (uint)*(ushort *)(*(int *)(param_1 + 0x18) + (uint)*(byte *)(param_1 + 0x78) * 4);
    uVar11 = (uint)*(ushort *)(*(int *)(param_1 + 0x18) + (uint)*(byte *)(param_1 + 0x78) * 4 + 2);
    uVar10 = uVar8;
    if (uVar8 <= uVar11) {
      do {
        uVar10 = uVar10 + 0x20;
        iVar5 = iVar5 + 1;
      } while ((int)uVar10 <= (int)uVar11);
    }
    iVar9 = 0;
    iVar6 = (uVar11 - (uVar8 + (iVar5 + -1) * 0x20)) + 1;
    if (0 < iVar5) {
      sVar14 = 0;
      do {
        iVar12 = 0;
        if (0 < iVar6) {
          do {
            sVar1 = (short)iVar12;
            iVar13 = iVar12 + ((int)(uint)*(ushort *)(param_1 + 0x80) >> 3);
            iVar12 = iVar12 + 1;
            *(ushort *)
             (iVar7 + 0xc +
             (iVar13 + (iVar9 + ((int)(uint)*(ushort *)(param_1 + 0x82) >> 3)) * 0x20) * 2) =
                 sVar14 + sVar1 + *(short *)(*(int *)(param_1 + 0x18) +
                                            (uint)*(byte *)(param_1 + 0x78) * 4) +
                 (ushort)*(byte *)(param_1 + 10) * 0x1000;
          } while (iVar12 < iVar6);
        }
        iVar9 = iVar9 + 1;
        sVar14 = sVar14 + 0x20;
      } while (iVar9 < iVar5);
      return;
    }
    return;
  }
  iVar5 = FUN_02033020(param_1 + 0x10,*(undefined1 *)(param_1 + 0x78),
                       *(undefined4 *)(param_1 + 0x68));
  iVar6 = FUN_02033138(param_1 + 0x10,*(undefined1 *)(param_1 + 0x78),
                       *(undefined4 *)(param_1 + 0x68));
  iStack_30 = 0;
  uVar8 = (uint)*(ushort *)(*(int *)(param_1 + 0x18) + (uint)*(byte *)(param_1 + 0x78) * 4);
  uVar11 = (uint)*(ushort *)(*(int *)(param_1 + 0x18) + (uint)*(byte *)(param_1 + 0x78) * 4 + 2);
  uVar10 = uVar8;
  if (uVar8 <= uVar11) {
    do {
      uVar10 = uVar10 + 0x20;
      iStack_30 = iStack_30 + 1;
    } while ((int)uVar10 <= (int)uVar11);
  }
  iVar12 = 0;
  iVar9 = (uVar11 - (uVar8 + (iStack_30 + -1) * 0x20)) + 1;
  if (0 < iStack_30) {
    sVar14 = 0;
    do {
      iVar13 = 0;
      if (0 < iVar9) {
        do {
          uVar10 = (uint)*(ushort *)(param_1 + 0x80);
          if (uVar10 < uVar10 + *(ushort *)(param_1 + 0x7c)) {
            do {
              uVar11 = (uint)*(ushort *)(param_1 + 0x82);
              if (uVar11 < uVar11 + *(ushort *)(param_1 + 0x7e)) {
                do {
                  *(ushort *)
                   ((iVar12 + ((int)uVar11 >> 3)) * 0x40 +
                   iVar7 + 0xc + iVar13 * 2 + ((int)uVar10 >> 3) * 2) =
                       sVar14 + (short)iVar13 +
                                *(short *)(*(int *)(param_1 + 0x18) +
                                          (uint)*(byte *)(param_1 + 0x78) * 4) +
                       (ushort)*(byte *)(param_1 + 10) * 0x1000;
                  uVar11 = uVar11 + iVar6;
                } while ((int)uVar11 <
                         (int)((uint)*(ushort *)(param_1 + 0x82) + (uint)*(ushort *)(param_1 + 0x7e)
                              ));
              }
              uVar10 = uVar10 + iVar5;
            } while ((int)uVar10 <
                     (int)((uint)*(ushort *)(param_1 + 0x80) + (uint)*(ushort *)(param_1 + 0x7c)));
          }
          iVar13 = iVar13 + 1;
        } while (iVar13 < iVar9);
      }
      iVar12 = iVar12 + 1;
      sVar14 = sVar14 + 0x20;
    } while (iVar12 < iStack_30);
    return;
  }
  return;
}


