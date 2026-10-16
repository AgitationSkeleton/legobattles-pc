// FUN_0203a4b4 @ 0x0203A4B4   entry 0203a4b4   body 304 bytes
// Ghidra decompiler output. READ ONLY -- see playbook/01-what-ghidra-silently-drops.md before
// treating any of this as the function's real behaviour.


void FUN_0203a4b4(int param_1,int param_2)

{
  short sVar1;
  bool bVar2;
  int iVar3;
  byte bVar4;
  undefined2 *puVar5;
  uint uVar6;
  uint uVar7;
  int iVar8;
  int iVar9;
  
  iVar3 = DAT_0203a5e4;
  sVar1 = *(short *)(param_1 + 0x60);
  iVar9 = *(int *)(DAT_0203a5e4 + 0x10);
  uVar7 = *(int *)(DAT_0203a5e4 + 4) * *(int *)(DAT_0203a5e4 + 0xc);
  iVar8 = *(int *)(param_1 + 0x174);
  if ((iVar9 == 0) || (iVar8 != 0)) {
    if ((iVar9 == 0) && (iVar8 != 0)) {
      bVar2 = false;
    }
    else {
      uVar6 = 0;
      if (uVar7 != 0) {
        do {
          if (*(char *)(iVar9 + uVar6) != *(char *)(iVar8 + uVar6)) {
            bVar2 = false;
            goto LAB_0203a540;
          }
          uVar6 = uVar6 + 1;
        } while (uVar6 < uVar7);
      }
      bVar2 = true;
    }
  }
  else {
    bVar2 = false;
  }
LAB_0203a540:
  if ((bVar2) && (param_2 == 0)) {
    return;
  }
  uVar7 = 0;
  do {
    bVar4 = 0;
    do {
      iVar8 = FUN_020eebf4(iVar3,bVar4,uVar7);
      if (iVar8 != 0) {
        puVar5 = (undefined2 *)
                 FUN_0203d90c(bVar4,uVar7 + ((int)((int)sVar1 + ((uint)((int)sVar1 >> 2) >> 0x1d))
                                             >> 3 & 1U) & 0xff);
        *puVar5 = 100;
      }
      bVar4 = bVar4 + 1;
    } while (bVar4 < 0x20);
    uVar7 = uVar7 + 1 & 0xff;
  } while (uVar7 < 0x18);
  FUN_020eeb1c(param_1 + 0x164,*(undefined4 *)(iVar3 + 8),*(undefined4 *)(iVar3 + 0xc));
  FUN_02110758(*(undefined4 *)(iVar3 + 0x10),*(undefined4 *)(param_1 + 0x174),
               *(int *)(param_1 + 0x168) * *(int *)(param_1 + 0x170));
  *(undefined1 *)(param_1 + 0x67) = 1;
  return;
}


