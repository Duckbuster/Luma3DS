/*
*   This file is part of Luma3DS
*   Copyright (C) 2016 Aurora Wright, TuxSH
*
*   This program is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 3 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*   Additional Terms 7.b of GPLv3 applies to this file: Requiring preservation of specified
*   reasonable legal notices or author attributions in that material or in the Appropriate Legal
*   Notices displayed by works containing it.
*/

#include "exceptions.h"
#include "fs.h"
#include "strings.h"
#include "memory.h"
#include "screen.h"
#include "draw.h"
#include "utils.h"
#include "fmt.h"
#include "../build/bundled.h"

void installArm9Handlers(void)
{
    memcpy((void *)0x01FF8000, arm9_exceptions_bin + 32, arm9_exceptions_bin_size - 32);

    /* IRQHandler is at 0x08000000, but we won't handle it for some reasons
       svcHandler is at 0x08000010, but we won't handle svc either */

    const u32 offsets[] = {0x08, 0x18, 0x20, 0x28};

    for(u32 i = 0; i < 4; i++)
    {
        *(vu32 *)(0x08000000 + offsets[i]) = 0xE51FF004;
        *(vu32 *)(0x08000000 + offsets[i] + 4) = *((u32 *)arm9_exceptions_bin + 1 + i);
    }
}

u32 installArm11Handlers(u32 *exceptionsPage, u32 stackAddress, u32 codeSetOffset, u32 *dAbtHandler, u32 dAbtHandlerMemAddress)
{
    u32 *endPos = exceptionsPage + 0x400;

    u32 *initFPU;
    for(initFPU = exceptionsPage; initFPU < endPos && *initFPU != 0xE1A0D002; initFPU++);

    u32 *freeSpace;
    for(freeSpace = initFPU; freeSpace < endPos && *freeSpace != 0xFFFFFFFF; freeSpace++);

    u32 *mcuReboot;
    for(mcuReboot = exceptionsPage; mcuReboot < endPos && *mcuReboot != 0xE3A0A0C2; mcuReboot++);

    if(initFPU == endPos || freeSpace == endPos || mcuReboot == endPos || *(u32 *)((u8 *)freeSpace + arm11_exceptions_bin_size - 36) != 0xFFFFFFFF) return 1;

    initFPU += 3;
    mcuReboot -= 2;

    memcpy(freeSpace, arm11_exceptions_bin + 32, arm11_exceptions_bin_size - 32);

    exceptionsPage[1] = MAKE_BRANCH(exceptionsPage + 1, (u8 *)freeSpace + *(u32 *)(arm11_exceptions_bin + 8)  - 32); //Undefined Instruction
    exceptionsPage[3] = MAKE_BRANCH(exceptionsPage + 3, (u8 *)freeSpace + *(u32 *)(arm11_exceptions_bin + 12) - 32); //Prefetch Abort
    exceptionsPage[7] = MAKE_BRANCH(exceptionsPage + 7, (u8 *)freeSpace + *(u32 *)(arm11_exceptions_bin + 4)  - 32); //FIQ

    for(u32 *pos = dAbtHandler; *pos != stackAddress; pos++)
    {
        u32 va_dst = 0xFFFF0000 + (((u8 *)freeSpace + *(u32 *)(arm11_exceptions_bin + 4)) - (u8 *)exceptionsPage);
        u32 va_src;
        switch(*pos)
        {
            case 0xF96D0513: //srsdb sp!, 0x13
                va_src = dAbtHandlerMemAddress +  ((u8 *)pos - (u8 *)dAbtHandler);
                *pos = MAKE_BRANCH((u8 *)va_src, (u8 *)va_dst);
                break;
           case 0xE29EF004: //subs pc, lr, 4
                pos++;
                *pos++ = 0xE8BD000F;// pop {r0-r3}
                va_src = dAbtHandlerMemAddress +  ((u8 *)pos - (u8 *)dAbtHandler);
                *pos = MAKE_BRANCH((u8 *)va_src, (u8 *)va_dst);
                break;
        }
    }


    for(u32 *pos = freeSpace; pos < (u32 *)((u8 *)freeSpace + arm11_exceptions_bin_size - 32); pos++)
    {
        switch(*pos) //Perform relocations
        {
            case 0xFFFF3000: *pos = stackAddress - 0x10; break;
            case 0xEBFFFFFE: *pos = MAKE_BRANCH_LINK(pos, initFPU); break;
            case 0xEAFFFFFE: *pos = MAKE_BRANCH(pos, mcuReboot); break;
            case 0xE12FFF1C: pos[1] = 0xFFFF0000 + 4 * (u32)(freeSpace - exceptionsPage) + pos[1] - 32; break; //bx r12 (mainHandler)
            case 0xBEEFBEEF: *pos = codeSetOffset; break;
        }
    }

    return 0;
}

void detectAndProcessExceptionDumps(void)
{
    volatile ExceptionDumpHeader *dumpHeader = (volatile ExceptionDumpHeader *)0x25000000;

    if(dumpHeader->magic[0] != 0xDEADC0DE || dumpHeader->magic[1] != 0xDEADCAFE || (dumpHeader->processor != 9 && dumpHeader->processor != 11)) return;

    const vu32 *regs = (vu32 *)((vu8 *)dumpHeader + sizeof(ExceptionDumpHeader));
    const vu8 *stackDump = (vu8 *)regs + dumpHeader->registerDumpSize + dumpHeader->codeDumpSize;
    const vu8 *additionalData = stackDump + dumpHeader->stackDumpSize;

    const char *handledExceptionNames[] = {
        "FIQ", "undefined instruction", "prefetch abort", "data abort"
    };

    const char *specialExceptions[] = {
        "kernel panic", "svcBreak"
    };

    const char *registerNames[] = {
        "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "R9", "R10", "R11", "R12",
        "SP", "LR", "PC", "CPSR", "FPEXC"
    };

    initScreens();

    drawString(true, 10, 10, COLOR_RED, "An exception occurred");
    u32 posY;
    if(dumpHeader->processor == 11) posY = drawFormattedString(true, 10, 30, COLOR_WHITE, "Processor:       ARM11 (core %u)", dumpHeader->core);
    else posY = drawString(true, 10, 30, COLOR_WHITE, "Processor:       ARM9"); 

    if(dumpHeader->type == 2)
    {
        if((regs[16] & 0x20) == 0 && dumpHeader->codeDumpSize >= 4)
        {
            u32 instr = *(vu32 *)(stackDump - 4);
            if(instr == 0xE12FFF7E)
                posY = drawFormattedString(true, 10, posY + SPACING_Y, COLOR_WHITE, "Exception type:  %s (%s)", handledExceptionNames[dumpHeader->type], specialExceptions[0]);
            else if(instr == 0xEF00003C)
                posY = drawFormattedString(true, 10, posY + SPACING_Y, COLOR_WHITE, "Exception type:  %s (%s)", handledExceptionNames[dumpHeader->type], specialExceptions[1]);
            else
                posY = drawFormattedString(true, 10, posY + SPACING_Y, COLOR_WHITE, "Exception type:  %s", handledExceptionNames[dumpHeader->type]);
        }
        else if((regs[16] & 0x20) != 0 && dumpHeader->codeDumpSize >= 2)
        {
            u16 instr = *(vu16 *)(stackDump - 2);
            if(instr == 0xDF3C)
                posY = drawFormattedString(true, 10, posY + SPACING_Y, COLOR_WHITE, "Exception type:  %s (%s)", handledExceptionNames[dumpHeader->type], specialExceptions[0]);
            else
                posY = drawFormattedString(true, 10, posY + SPACING_Y, COLOR_WHITE, "Exception type:  %s", handledExceptionNames[dumpHeader->type]);
        }
    }
    else
        posY = drawFormattedString(true, 10, posY + SPACING_Y, COLOR_WHITE, "Exception type:  %s", handledExceptionNames[dumpHeader->type]);

    if(dumpHeader->processor == 11 && dumpHeader->additionalDataSize != 0)
        posY = drawFormattedString(true, 10, posY + SPACING_Y, COLOR_WHITE,
                                   "Current process: %.8s (%016llX)", (const char *)additionalData, *(vu64 *)(additionalData + 8));
    posY += SPACING_Y;

    for(u32 i = 0; i < 17; i += 2)
    {
        posY = drawFormattedString(true, 10, posY + SPACING_Y, COLOR_WHITE, "%-7s%08X", registerNames[i], regs[i]);

        if(i != 16 || dumpHeader->processor != 9)
            posY = drawFormattedString(true, 10 + 22 * SPACING_X, posY, COLOR_WHITE, "%-7s%08X", registerNames[i + 1], regs[i + 1]);
    }

    posY += SPACING_Y;

    u32 mode = regs[16] & 0xF;
    if(dumpHeader->type == 3 && (mode == 7 || mode == 11))
        posY = drawString(true, 10, posY + SPACING_Y, COLOR_YELLOW, "Incorrect dump: failed to dump code and/or stack") + SPACING_Y;

    u32 posYBottom = drawString(false, 10, 10, COLOR_WHITE, "Stack dump:") + SPACING_Y;

    for(u32 line = 0; line < 19 && stackDump < additionalData; line++)
    {
        posYBottom = drawFormattedString(false, 10, posYBottom + SPACING_Y, COLOR_WHITE, "%08X:", regs[13] + 8 * line);

        for(u32 i = 0; i < 8 && stackDump < additionalData; i++, stackDump++)
            drawFormattedString(false, 10 + 10 * SPACING_X + 3 * i * SPACING_X, posYBottom, COLOR_WHITE, "%02X", *stackDump);
    }

    char folderPath[12],
         path[36],
         fileName[24];

    sprintf(folderPath, "dumps/arm%u", dumpHeader->processor);
    findDumpFile(folderPath, fileName);
    sprintf(path, "%s/%s", folderPath, fileName);

    if(fileWrite((void *)dumpHeader, path, dumpHeader->totalSize))
    {
        posY = drawString(true, 10, posY + SPACING_Y, COLOR_WHITE, "You can find a dump in the following file:");
        posY = drawString(true, 10, posY + SPACING_Y, COLOR_WHITE, path) + SPACING_Y;
    }
    else posY = drawString(true, 10, posY + SPACING_Y, COLOR_RED, "Error writing the dump file");

    drawString(true, 10, posY + SPACING_Y, COLOR_WHITE, "Press any button to shutdown");

    memset32((void *)dumpHeader, 0, dumpHeader->totalSize);

    waitInput(false);
    mcuPowerOff();
}
