// FUN_02038f28 @ 0x02038F28   entry 02038f28   body 268 bytes
// Ghidra decompiler output. READ ONLY -- see playbook/01-what-ghidra-silently-drops.md before
// treating any of this as the function's real behaviour.


void FUN_02038f28(int param_1,int param_2,int param_3)

{
  int iVar1;
  uint uVar2;
  uint uVar3;
  uint uVar4;
  int local_50;
  int local_4c;
  int local_48;
  int local_44;
  undefined1 auStack_40 [4];
  byte local_3c;
  byte local_3b;
  undefined1 auStack_38 [4];
  char local_34;
  char local_33;
  undefined1 auStack_30 [12];
  undefined1 auStack_24 [12];
  
  iVar1 = FUN_020a4524(*DAT_02039034);
  uVar3 = iVar1 - 1U & 0xff;
  iVar1 = FUN_020a4530(*DAT_02039034);
  uVar4 = iVar1 - 1U & 0xff;
  local_44 = (int)*(short *)(param_1 + 0x5e) << 0xc;
  local_48 = (int)*(short *)(param_1 + 0x60) << 0xc;
  FUN_020f27d4(auStack_24,&local_44,&local_48);
  FUN_020085a0(auStack_38,auStack_24);
  if (local_33 == '\0') {
    local_33 = '\0';
  }
  else {
    local_33 = local_33 + -1;
  }
  if (local_34 == '\0') {
    local_34 = '\0';
  }
  else {
    local_34 = local_34 + -1;
  }
  *(char *)(param_2 + 4) = local_34;
  *(char *)(param_2 + 5) = local_33;
  local_4c = (*(short *)(param_1 + 0x5e) + 0x100) * 0x1000;
  local_50 = (*(short *)(param_1 + 0x60) + 0xc0) * 0x1000;
  FUN_020f27d4(auStack_30,&local_4c,&local_50);
  FUN_020085a0(auStack_40,auStack_30);
  uVar2 = local_3b + 3;
  if (uVar3 <= local_3b + 3) {
    uVar2 = uVar3;
  }
  uVar3 = local_3c + 1;
  if (uVar4 <= local_3c + 1) {
    uVar3 = uVar4;
  }
  *(char *)(param_3 + 4) = (char)uVar3;
  *(char *)(param_3 + 5) = (char)uVar2;
  return;
}


