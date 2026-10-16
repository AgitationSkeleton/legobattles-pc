// FUN_020a3478 @ 0x020A3478   entry 020a3478   body 106 bytes
// Ghidra decompiler output. READ ONLY -- see playbook/01-what-ghidra-silently-drops.md before
// treating any of this as the function's real behaviour.


void FUN_020a3478(int param_1,undefined2 *param_2,undefined2 *param_3,ushort *param_4,
                 undefined4 param_5)

{
  undefined2 *puVar1;
  uint uVar2;
  
  uVar2 = (uint)*param_4;
  if (uVar2 < *(ushort *)(DAT_020a34e4 + 2)) {
    puVar1 = (undefined2 *)(*(int *)(param_1 + 0x338) + 2 + uVar2 * 0xc);
  }
  else {
    puVar1 = (undefined2 *)
             (*(int *)(param_1 + 0x33c) + 2 + (uVar2 - *(ushort *)(DAT_020a34e4 + 2) & 0xffff) * 0xc
             );
  }
  *param_2 = *puVar1;
  param_2[1] = puVar1[1];
  param_2[2] = puVar1[2];
  *param_3 = puVar1[3];
  param_3[1] = puVar1[4];
  param_3[2] = puVar1[5];
  FUN_02034b44(param_5,param_2,param_3);
  FUN_0200d8a8(param_5,param_2,param_3,*(undefined4 *)(param_1 + 0x338));
  return;
}


