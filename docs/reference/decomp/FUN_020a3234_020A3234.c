// FUN_020a3234 @ 0x020A3234   entry 020a3234   body 576 bytes
// Ghidra decompiler output. READ ONLY -- see playbook/01-what-ghidra-silently-drops.md before
// treating any of this as the function's real behaviour.


void FUN_020a3234(int param_1,undefined2 *param_2,uint param_3,uint param_4)

{
  bool bVar1;
  bool bVar2;
  char cVar3;
  undefined1 uVar4;
  char cVar5;
  int iVar6;
  undefined2 *puVar7;
  int extraout_r1;
  undefined2 *puVar8;
  uint uVar9;
  int iVar10;
  int local_5c;
  int local_58;
  int local_54;
  int local_50;
  int local_4c;
  int local_48;
  undefined1 auStack_38 [4];
  char local_34;
  char local_33;
  undefined2 local_30;
  undefined2 local_2e;
  undefined2 local_2a;
  undefined2 local_28;
  undefined1 auStack_24 [2];
  undefined2 local_22;
  undefined2 local_20;
  undefined1 auStack_1e [2];
  undefined2 local_1c;
  undefined2 local_1a;
  uint uStack_18;
  
  param_3 = param_3 >> 3;
  uVar9 = param_4 >> 3 & DAT_020a3474;
  uStack_18 = param_4;
  FUN_0211ffc0(param_3,3);
  if (extraout_r1 == 0) {
    bVar1 = false;
    bVar2 = true;
    local_48 = 0;
    local_4c = 0x1e;
  }
  else if (extraout_r1 == 1) {
    bVar1 = true;
    bVar2 = false;
    local_48 = 2;
    local_4c = 0;
  }
  else if (extraout_r1 == 2) {
    bVar1 = true;
    bVar2 = true;
    local_48 = 1;
    local_4c = 0x1f;
  }
  else {
    bVar1 = false;
    bVar2 = false;
    local_48 = 0;
    local_4c = 0;
  }
  puVar8 = param_2 + local_48;
  uVar9 = uVar9 >> 1;
  uVar4 = FUN_0211ffc0(param_3 + 2,3);
  FUN_020f2bbc(auStack_38,uVar4,uVar9 & 0xff);
  iVar6 = FUN_020a3218(param_1,auStack_38);
  local_50 = 0xc;
  if ((param_4 & 0xf) != 0) {
    local_50 = 0xd;
  }
  local_54 = 0;
  if (local_50 != 0) {
    do {
      iVar10 = 0;
      cVar3 = local_34;
      do {
        local_34 = cVar3;
        puVar7 = puVar8;
        FUN_020a3478(param_1,puVar7,puVar7 + 0x20,iVar6,auStack_38);
        iVar10 = iVar10 + 1;
        iVar6 = iVar6 + 2;
        puVar8 = puVar7 + 3;
        cVar3 = local_34 + '\x01';
      } while (iVar10 < 10);
      puVar8 = puVar7 + 0x25;
      iVar6 = iVar6 + (*(byte *)(param_1 + 0x14) - 10) * 2;
      local_33 = local_33 + '\x01';
      local_34 = local_34 + -9;
      local_54 = local_54 + 1;
    } while (local_54 < local_50);
  }
  cVar3 = (char)uVar9;
  if (bVar1) {
    local_34 = FUN_0211ffc0(param_3,3);
    local_33 = cVar3;
    iVar6 = FUN_020a3218(param_1,auStack_38);
    local_58 = 0;
    puVar8 = param_2;
    if (local_50 != 0) {
      do {
        FUN_020a3478(param_1,auStack_24,auStack_1e,iVar6,auStack_38);
        if (local_48 == 1) {
          *puVar8 = local_20;
          puVar8[0x20] = local_1a;
          puVar8 = puVar8 + 0x40;
        }
        else if (local_48 == 2) {
          *puVar8 = local_22;
          puVar8[1] = local_20;
          puVar8[0x20] = local_1c;
          puVar7 = puVar8 + 0x21;
          puVar8 = puVar8 + 0x40;
          *puVar7 = local_1a;
        }
        iVar6 = iVar6 + (uint)*(byte *)(param_1 + 0x14) * 2;
        local_33 = local_33 + '\x01';
        local_58 = local_58 + 1;
      } while (local_58 < local_50);
    }
  }
  if ((bVar2) && (param_3 + 0x20 < (uint)*(byte *)(param_1 + 0x14) * 3)) {
    cVar5 = FUN_0211ffc0(param_3,3);
    local_34 = cVar5 + '\n' + bVar1;
    local_33 = cVar3;
    iVar6 = FUN_020a3218(param_1,auStack_38);
    param_2 = param_2 + local_4c;
    local_5c = 0;
    if (local_50 != 0) {
      do {
        FUN_020a3478(param_1,&local_30,&local_2a,iVar6,auStack_38);
        if (local_48 == 0) {
          *param_2 = local_30;
          param_2[1] = local_2e;
          param_2[0x20] = local_2a;
          puVar8 = param_2 + 0x21;
          param_2 = param_2 + 0x40;
          *puVar8 = local_28;
        }
        else if (local_48 == 1) {
          *param_2 = local_30;
          param_2[0x20] = local_2a;
          param_2 = param_2 + 0x40;
        }
        iVar6 = iVar6 + (uint)*(byte *)(param_1 + 0x14) * 2;
        local_33 = local_33 + '\x01';
        local_5c = local_5c + 1;
      } while (local_5c < local_50);
    }
  }
  return;
}


