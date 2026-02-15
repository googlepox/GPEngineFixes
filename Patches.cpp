#include "Patches.h"
#include <obse_common/SafeWrite.h>

namespace GPEngineFixes::Patches
{
    void __declspec(naked) NullNodeHandler()
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
		WriteRelJump(0x005ED1F8, (UInt32)&NullNodeHandler);
	}
}
