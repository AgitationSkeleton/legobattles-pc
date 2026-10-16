// FUN_02039148 @ 0x02039148   entry 02039148   body 724 bytes
// Ghidra decompiler output. READ ONLY -- see playbook/01-what-ghidra-silently-drops.md before
// treating any of this as the function's real behaviour.


void FUN_02039148(int param_1,undefined4 param_2,undefined4 param_3,undefined4 param_4)

{
  char cVar1;
  int iVar2;
  undefined1 *puVar3;
  int *piVar4;
  int iVar5;
  uint uVar6;
  uint uVar7;
  int iVar8;
  int iVar9;
  int iVar10;
  bool bVar11;
  bool bVar12;
  undefined1 local_c0;
  undefined1 local_bf;
  undefined4 local_bc;
  undefined4 local_b8;
  undefined1 auStack_b4 [8];
  undefined1 auStack_ac [8];
  undefined1 auStack_a4 [64];
  undefined1 auStack_64 [64];
  undefined4 uStack_24;
  
  iVar8 = *DAT_0203941c;
  uStack_24 = param_4;
  if (*(char *)(iVar8 + 0x24) != '\0') {
    FUN_02039dc0();
  }
  FUN_020f2ba0(auStack_ac);
  FUN_020f2ba0(auStack_b4);
  FUN_02038f28(param_1,auStack_ac,auStack_b4);
  FUN_020ef118(auStack_64,1);
  FUN_020882bc(auStack_64,auStack_ac,auStack_b4);
  FUN_020ef118(auStack_a4,1);
  iVar9 = 0;
  iVar2 = FUN_020ef1d0(auStack_64);
  if (0 < iVar2) {
    do {
      puVar3 = (undefined1 *)FUN_020ef1e0(auStack_64,iVar9);
      local_c0 = *puVar3;
      local_bf = puVar3[1];
      piVar4 = (int *)FUN_02087350(&local_c0);
      if (piVar4 != (int *)0x0) {
        iVar2 = (**(code **)(*piVar4 + 0xc))();
        if (iVar2 != 0) {
          FUN_020ef244(auStack_a4,&local_c0);
        }
        iVar2 = FUN_020872dc(&local_c0);
        if (iVar2 != 0) {
          *(undefined1 *)(iVar2 + 0x71) = 0;
        }
      }
      iVar9 = iVar9 + 1;
      iVar2 = FUN_020ef1d0(auStack_64);
    } while (iVar9 < iVar2);
  }
  local_b8 = 1;
  FUN_02100de0(0x10,&local_b8);
  FUN_02100de0(0x11,0,0);
  FUN_020fa75c(0xf);
  FUN_02100de0(0x15,0,0);
  iVar2 = DAT_02039420;
  iVar9 = 0;
  if (0 < *(int *)(DAT_02039420 + 8)) {
    iVar10 = 0;
    do {
      iVar5 = FUN_020872dc(*(int *)(iVar2 + 4) + iVar10);
      if (iVar5 != 0) {
        *(undefined1 *)(iVar5 + 0x71) = 1;
      }
      iVar9 = iVar9 + 1;
      iVar10 = iVar10 + 2;
    } while (iVar9 < *(int *)(iVar2 + 8));
  }
  iVar2 = DAT_02039424;
  iVar9 = 0;
  if (0 < *(int *)(DAT_02039424 + 8)) {
    iVar10 = 0;
    do {
      iVar5 = FUN_020872dc(*(int *)(iVar2 + 4) + iVar10);
      if (iVar5 != 0) {
        *(undefined1 *)(iVar5 + 0x71) = 1;
      }
      iVar9 = iVar9 + 1;
      iVar10 = iVar10 + 2;
    } while (iVar9 < *(int *)(iVar2 + 8));
  }
  local_bc = 1;
  FUN_02100de0(0x12,&local_bc);
  FUN_0203a9e8(param_1,auStack_a4);
  FUN_0203ab00(param_1,auStack_a4);
  FUN_0203ab64(param_1,auStack_a4);
  FUN_0203d6d0(param_1);
  cVar1 = *(char *)(param_1 + 0x67);
  bVar11 = cVar1 == '\0';
  if (bVar11) {
    cVar1 = *(char *)(iVar8 + 0x24);
  }
  bVar12 = bVar11 && cVar1 == '\0';
  if (bVar11 && cVar1 == '\0') {
    bVar12 = *(char *)(param_1 + 0x160) == '\0';
  }
  if (bVar12) {
    iVar8 = *(int *)(param_1 + 0x174);
    iVar2 = *(int *)(DAT_02039428 + 0x10);
    uVar7 = *(int *)(DAT_02039428 + 4) * *(int *)(DAT_02039428 + 0xc);
    if ((iVar2 == 0) || (iVar8 != 0)) {
      if ((iVar2 == 0) && (iVar8 != 0)) {
        bVar11 = false;
      }
      else {
        uVar6 = 0;
        if (uVar7 != 0) {
          do {
            if (*(char *)(iVar2 + uVar6) != *(char *)(iVar8 + uVar6)) {
              bVar11 = false;
              goto LAB_020393d8;
            }
            uVar6 = uVar6 + 1;
          } while (uVar6 < uVar7);
        }
        bVar11 = true;
      }
    }
    else {
      bVar11 = false;
    }
LAB_020393d8:
    if (bVar11) goto LAB_02039404;
  }
  uVar7 = (uint)*(byte *)(param_1 + 0x1a0);
  bVar11 = uVar7 == 0;
  if (bVar11) {
    uVar7 = *(uint *)(param_1 + 0x1ac);
  }
  if (bVar11 && uVar7 == 2) {
    FUN_02039fb4(param_1);
    *(undefined1 *)(param_1 + 0x67) = 1;
  }
LAB_02039404:
  FUN_020ef144(auStack_a4);
  FUN_020ef144(auStack_64);
  return;
}


