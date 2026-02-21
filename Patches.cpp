#include "Patches.h"
#include <obse_common/SafeWrite.h>

namespace GPEngineFixes::Patches
{
    namespace ApplyTrapDamageFix
    {
        void __declspec(naked) ApplyTrapDamageHook()
        {
            __asm {
                test eax, eax           // Check if GetNiNode returned NULL
                jnz node_valid          // If not null, continue normally

                // NULL case - skip all the node access
                // Jump to safe code at 0x5ED21D
                mov eax, 0x005ED21D
                jmp eax

                node_valid :
                // Original instruction that was at 0x5ED1F8
                fld dword ptr[eax + 88h]
                    // Return to next instruction
                    mov eax, 0x005ED1FE
                    jmp eax
            }
        }

        void Install()
        {
            WriteRelJump(0x005ED1F8, (UInt32)&ApplyTrapDamageHook);
            _MESSAGE("Actor::ApplyTrapDamage hook installed");
        }
    }

    namespace GetDamageFix
    {
        void __declspec(naked) GetDamageHook()
        {
            __asm {
                // EDI contains the weapon pointer at this point
                test edi, edi           // Check if NULL
                jz no_weapon_equipped   // Jump if no weapon

                // Original instruction
                mov al, [edi + 4]         // Get form type byte

                // Return to next instruction (0x484F9C)
                push 0x00484F9C
                ret

                no_weapon_equipped :
                // No weapon - clean return
                // Need to restore stack and return
                pop edi                 // Restore edi (was pushed at 0x484F95)
                    pop esi                 // Restore esi (was pushed at 0x484F8A)
                    pop ebx                 // Restore ebx (was pushed at 0x484F89)
                    add esp, 18h            // Clean up local vars (from 0x484F80)
                    ret                     // Return to caller
            }
        }

        void Install()
        {
            WriteRelJump(0x00484F99, (UInt32)&GetDamageHook);
            _MESSAGE("EquippedWeaponData::GetDamage hook installed");
        }

    }

	void Install()
	{
        ApplyTrapDamageFix::Install();
        GetDamageFix::Install();
        _MESSAGE("All hooks installed");
	}
}
