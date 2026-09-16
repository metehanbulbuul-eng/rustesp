// offsets.h
#pragma once

// ============ Camera (UnityEngine.Camera) ============
#define RVA_Camera_get_main            0xd4b857c
#define RVA_Camera_WorldToScreenPoint  0xd4b7d80

// ============ EntityManager ============
#define RVA_EntityManager_get_Instance 0x75ff2d8
#define RVA_EntityManager_get_Entities 0x75ff530
#define RVA_EntityManager_get_Count    0x75ff360

// ============ PlayerEntity ============
#define RVA_Player_get_MyPlayerClient  0x7a2b3fc
#define RVA_Player_get_bLocalPlayer    0x7a36a84
#define RVA_Player_get_PosX_Smooth     0x7a279e0
#define RVA_Player_get_PosY_Smooth     0x7a27aec
#define RVA_Player_get_PosZ_Smooth     0x7a27bf8
#define RVA_Player_get_Hp              0x7a3e928
#define RVA_Player_get_MaxHp           0x7a3e98c
#define RVA_Player_get_Name            0x79fbc5c
#define RVA_Player_get_TeamId          0x7a03618
#define RVA_Player_get_IsDead          0x7a2f208
#define RVA_Player_get_IsAlive         0x7a3e55c
#define RVA_Player_get_RoleId          0x7a27490
#define RVA_Player_get_Level           0x79f5228

// ============ Field offsets ============
#define FIELD_EntityManager_entities   0x18
#define FIELD_Dict_entries             0x18
#define FIELD_Dict_count               0x20
#define ARRAY_FIRST_ELEMENT_OFFSET     0x20
#define ENTRY_SIZE                     0x18