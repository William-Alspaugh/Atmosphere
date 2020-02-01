/*
*   This file is part of Luma3DS.
*   Copyright (C) 2016-2019 Aurora Wright, TuxSH
*
*   SPDX-License-Identifier: (MIT OR GPL-2.0-or-later)
*/

#define _GNU_SOURCE // for strchrnul
#include <stdio.h>
#include <string.h>
#include "../debug_manager.h"
#include "../watchpoints.h"

#include "debug.h"
#include "net.h"

#include "context.h"
#include "verbose.h"
#include "thread.h"
#include "mem.h"
#include "hio.h"

#include <stdlib.h>
#include <signal.h>

static bool GDB_PreprocessDebugEvent(GDBContext *ctx, DebugEventInfo *info)
{
    u64 irqFlags = maskIrq();
    bool shouldSignal;

    switch (info->type) {
        case DBGEVENT_CORE_ON: {
            shouldSignal = ctx->catchThreadEvents;
            if (!info->preprocessed) {
                ctx->attachedCoreList |= BIT(info->coreId);
            }
            break;
        }
        case DBGEVENT_CORE_OFF: {
            if (!info->preprocessed) {
                u32 newLst = ctx->attachedCoreList & ~BIT(info->coreId);
                if (ctx->selectedThreadId == info->coreId && newLst != 0) {
                    ctx->selectedThreadId = __builtin_ctz(newLst);
                    GDB_MigrateRxIrq(ctx, ctx->selectedThreadId);
                }
                ctx->attachedCoreList = newLst;
                shouldSignal = ctx->catchThreadEvents || newLst == 0;
            } else {
                shouldSignal = ctx->catchThreadEvents || ctx->attachedCoreList == 0;
            }
            break;
        }

        default:
            shouldSignal = true;
            break;
    }

    info->preprocessed = true;
    restoreInterruptFlags(irqFlags);
    return shouldSignal; 
}

static inline void GDB_MarkDebugEventAcked(GDBContext *ctx, const DebugEventInfo *info)
{
    ctx->acknowledgedDebugEventCoreList |= BIT(info->coreId);
}

static int GDB_ParseExceptionFrame(char *out, const DebugEventInfo *info, int sig)
{
    u32 coreId = info->coreId;
    ExceptionStackFrame *frame = info->frame;

    int n = sprintf(out, "T%02xthread:%x;core:%x;", sig, 1 + coreId, coreId);

    // Dump the GPRs & sp & pc & cpsr (cpsr is 32-bit in the xml desc)
    // For performance reasons, we don't include the FPU registers here
    for (u32 i = 0; i < 31; i++) {
        n += sprintf(out + n, "%x:%016lx;", i, __builtin_bswap64(readFrameRegister(frame, i)));
    }

    n += sprintf(
        out + n,
        "1f:%016lx;20:%016lx;21:%08x",
        __builtin_bswap64(*exceptionGetSpPtr(frame)),
        __builtin_bswap64(frame->elr_el2),
        __builtin_bswap32((u32)frame->spsr_el2)
    );

    return n;
}

int GDB_SendStopReply(GDBContext *ctx, const DebugEventInfo *info, bool asNotification)
{
    char *buf = ctx->buffer + 1;
    int n;
    bool invalid = false;

    buf[0] = 0;

    if (asNotification) {
        strcpy(buf, "Stopped:");
    }

    n = strlen(buf);

    // Even if the info is invalid:
    ctx->lastDebugEvent = info;
    ctx->sentDebugEventCoreList |= BIT(info->coreId);

    switch(info->type) {
        case DBGEVENT_DEBUGGER_BREAK: {
            n += GDB_ParseExceptionFrame(buf + n, info, 0);
            break;
        }
    
        case DBGEVENT_CORE_ON: {
            if (ctx->catchThreadEvents) {
                n += GDB_ParseExceptionFrame(buf + n, info, SIGTRAP);
                strcat(buf, "create:;");
            } else {
                invalid = true;
            }
            break;
        }

        case DBGEVENT_CORE_OFF: {
            if (ctx->attachedCoreList == 0) {
                // All cores have exited, must report an exit
                ctx->processExited = true;
                ctx->processEnded = true;
                strcat(buf, "W00");
            } else if(ctx->catchThreadEvents) {
                sprintf(buf, "w0;%x", info->coreId + 1);
            } else {
                invalid = true;
            }
            break;
        }

        case DBGEVENT_EXIT: {
            // exited (no error / unhandled exception), SIGTERM (process terminated) * 2
            static const char *processExitReplies[] = { "W00", "X0f" };
            strcat(buf, processExitReplies[ctx->processExited ? 0 : 1]);
            break;
        }

        case DBGEVENT_EXCEPTION: {
            ExceptionClass ec = info->frame->esr_el2.ec;

            // Aside from stage 2 translation faults and other pre-handled exceptions, 
            // the only notable exceptions we get are stop point/single step events from the debugee (basically classes 0x3x)
            switch(ec) {
                case Exception_BreakpointLowerEl: {
                    n += GDB_ParseExceptionFrame(buf + n, info, SIGTRAP);
                    strcat(buf, "hwbreak:;");
                    break;
                }

                case Exception_WatchpointLowerEl: {
                    static const char *kinds[] = { "", "r", "", "a" };
                    // Note: exception info doesn't provide us with the access size. Use 1.
                    bool wnr = (info->frame->esr_el2.iss & BIT(6)) != 0;
                    WatchpointLoadStoreControl dr = wnr ? WatchpointLoadStoreControl_Store : WatchpointLoadStoreControl_Load;
                    DebugControlRegister cr = retrieveSplitWatchpointConfig(info->frame->far_el2, 1, dr, false);
                    if (!cr.enabled) {
                        DEBUG("GDB: oops, unhandled watchpoint for core id %u, far=%016lx\n", info->coreId, info->frame->far_el2);
                    } else {
                        n += GDB_ParseExceptionFrame(buf + n, info, SIGTRAP);
                        sprintf(buf + n, "%swatch:%016lx;", kinds[cr.lsc], info->frame->far_el2);
                    }
                    break;
                }

                case Exception_SoftwareStepLowerEl: {
                    n += GDB_ParseExceptionFrame(buf + n, info, SIGTRAP);
                    break;
                }

                // Note: we don't really support 32-bit sw breakpoints, we'll still report them
                // if the guest has inserted some of them manually...
                case Exception_SoftwareBreakpointA64:
                case Exception_SoftwareBreakpointA32: {
                    n += GDB_ParseExceptionFrame(buf + n, info, SIGTRAP);
                    strcat(buf, "swbreak:;");
                    break;
                }
                
                default: {
                    invalid = true;
                    DEBUG("GDB: oops, unhandled exception for core id %u\n", info->coreId);
                    break;
                }
            }
            break;
        }

        case DBGEVENT_OUTPUT_STRING: {
            if (!(ctx->flags & GDB_FLAG_NONSTOP)) {
                uintptr_t addr = info->outputString.address;
                size_t remaining = info->outputString.size;
                size_t sent = 0;
                size_t total = 0;
                while (remaining > 0) {
                    size_t pending = (GDB_BUF_LEN - 1) / 2;
                    pending = pending < remaining ? pending : remaining;

                    int res = GDB_SendMemory(ctx, "O", 1, addr + sent, pending);
                    if(res < 0 || res != 5 + 2 * pending)
                        break;

                    sent += pending;
                    remaining -= pending;
                    total += res;
                }

                return (int)total;
            } else {
                invalid = true;
                break;
            }
        }

        // TODO: HIO

        default: {
            invalid = true;
            DEBUG("GDB: unknown exception type %u, core id %u\n", (u32)info->type, info->coreId);
            break;
        }
    }

    if (invalid) {
        return 0;
    } else if (asNotification) {
        return GDB_SendNotificationPacket(ctx, buf, strlen(buf));
    } else {
        if (!(ctx->flags & GDB_FLAG_NONSTOP)) {
            GDB_MarkDebugEventAcked(ctx, info);
        }
        return GDB_SendPacket(ctx, buf, strlen(buf));
    }
}

/*
    Non-stop mode:
        -> %Stop:<info>
        <- $vStopped
        -> $<info>
        <- vStopped, etc.
        -> $OK
    If we're the first to try to send a notification, send it.
    Otherwise don't, the core which will handle the GDB packets then will see the changes.

    GDB can also send the "?" packet. This aborts the current notfication/vStopped sequence,
    and asks to resend the events for each stopped core, no matter if already sent before.

    Full-stop mode (default):

    If we lose the race, we have to wait until we're continued to send the remaining events...
*/

int GDB_TrySignalDebugEvent(GDBContext *ctx, DebugEventInfo *info)
{
    int ret = 0;

    // Acquire the gdb lock/disable rx irq. We most likely block here.
    GDB_AcquireContext(ctx);

    // Need to put it here otherwise core on/off would never be seen
    bool shouldSignal = GDB_PreprocessDebugEvent(ctx, info);

    // Are we still paused & has the packet not been handled & are we allowed to send on our own?

    if (shouldSignal && !ctx->sendOwnDebugEventDisallowed && !info->handled && debugManagerIsCorePaused(info->coreId)) {
        bool nonStop = (ctx->flags & GDB_FLAG_NONSTOP) != 0;
        info->handled = true;

        // Full-stop mode: stop other cores
        if (!nonStop) {
            debugManagerPauseCores(ctx->attachedCoreList & ~BIT(info->coreId));
        }

        ctx->sendOwnDebugEventDisallowed = true;
        ret = GDB_SendStopReply(ctx, info, nonStop);
    }

    if (!shouldSignal) {
        debugManagerContinueCores(BIT(currentCoreCtx->coreId));
    }

    GDB_ReleaseContext(ctx);

    return ret;
}

void GDB_BreakAllCores(GDBContext *ctx)
{
    if (ctx->flags & GDB_FLAG_NONSTOP) {
        debugManagerBreakCores(ctx->attachedCoreList);
    } else {
        // Break all cores too, but mark everything but the first has handled
        debugManagerBreakCores(ctx->attachedCoreList);
        u32 rem = ctx->attachedCoreList & ~BIT(currentCoreCtx->coreId);
        FOREACH_BIT (tmp, coreId, rem) {
            DebugEventInfo *info = debugManagerGetCoreDebugEvent(coreId);
            info->handled = true;
            info->preprocessed = true;
        }
    }
}

GDB_DECLARE_VERBOSE_HANDLER(Stopped)
{
    u32 coreList = debugManagerGetPausedCoreList() & ctx->attachedCoreList;
    u32 remaining = coreList & ~ctx->sentDebugEventCoreList;

    // Ack
    if (ctx->lastDebugEvent != NULL) {
        GDB_MarkDebugEventAcked(ctx, ctx->lastDebugEvent);
    }

    for (;;) {
        if (remaining != 0) {
            // Send one more debug event (marking it as handled)
            u32 coreId = __builtin_ctz(remaining);
            DebugEventInfo *info = debugManagerGetCoreDebugEvent(coreId);

            if (GDB_PreprocessDebugEvent(ctx, info)) {
                ctx->sendOwnDebugEventDisallowed = true;
                return GDB_SendStopReply(ctx, info, false);
            } else {
                remaining &= ~BIT(coreId);
            }
        } else {
            // vStopped sequenced finished
            ctx->sendOwnDebugEventDisallowed = false;
            return GDB_ReplyOk(ctx);
        }
    }
}

GDB_DECLARE_HANDLER(GetStopReason)
{
    bool nonStop = (ctx->flags & GDB_FLAG_NONSTOP) != 0;
    if (!nonStop) {
        // Full-stop:
        return GDB_SendStopReply(ctx, ctx->lastDebugEvent, true);
    } else {
        // Non-stop, start new vStopped sequence
        ctx->sentDebugEventCoreList = 0;
        ctx->acknowledgedDebugEventCoreList = 0;
        ctx->lastDebugEvent = NULL;
        ctx->sendOwnDebugEventDisallowed = true;
        return GDB_HandleVerboseStopped(ctx);
    }
}

GDB_DECLARE_HANDLER(Detach)
{
    ctx->state = GDB_STATE_DETACHING;
    return GDB_ReplyOk(ctx);
}

GDB_DECLARE_HANDLER(Kill)
{
    ctx->state = GDB_STATE_DETACHING;
    ctx->flags |= GDB_FLAG_TERMINATE;

    return 0;
}

GDB_DECLARE_VERBOSE_HANDLER(CtrlC)
{
    int ret = GDB_ReplyOk(ctx);
    GDB_BreakAllCores(ctx);
    return ret;
}

GDB_DECLARE_HANDLER(ContinueOrStepDeprecated)
{
    char *addrStart = NULL;

    char cmd = ctx->commandData[-1];

    // This deprecated command should not be permitted in non-stop mode
    if (ctx->flags & GDB_FLAG_NONSTOP) {
        return GDB_ReplyErrno(ctx, EPERM);
    }

    if(cmd == 'C' || cmd == 'S') {
        // Check the presence of the two-digit signature, even if we ignore it.
        u8 sg;
        if (GDB_DecodeHex(&sg, ctx->commandData, 1) != 1) {
            return GDB_ReplyErrno(ctx, EILSEQ);
        }

        // Check: [;addr] or [nothing]
        if (ctx->commandData[2] != 0 && ctx->commandData[2] != ';') {
            return GDB_ReplyErrno(ctx, EILSEQ);
        }

        if(ctx->commandData[2] == ';') {
            addrStart = ctx->commandData + 3;
        }
    }
    else {
        // 'c', 's'
        if (ctx->commandData[0] != 0) {
            addrStart = ctx->commandData;
        }
    }

    // Only support the simplest form, with no address
    // Only degenerate clients will use ;addr, anyway (and the packets are deprecated in favor
    // of vCont anyway)

    if (addrStart != NULL) {
        return GDB_ReplyErrno(ctx, ENOSYS);
    }

    u32 coreList = ctx->selectedThreadIdForContinuing == -1 ? ctx->attachedCoreList : BIT(ctx->selectedThreadIdForContinuing);
    u32 ssMask = (cmd == 's' || cmd == 'S') ? coreList : 0;

    FOREACH_BIT (tmp, coreId, ssMask) {
        debugManagerSetSteppingRange(coreId, 0, 0);
    }

    u32 mask = ctx->acknowledgedDebugEventCoreList;
    debugManagerSetSingleStepCoreList(ssMask & mask);
    debugManagerUnpauseCores(coreList & mask);
    return 0;
}

GDB_DECLARE_VERBOSE_HANDLER(Continue)
{
    u32 parsedCoreList = 0;
    u32 continueCoreList = 0;
    u32 stepCoreList = 0;
    u32 stopCoreList = 0;

    char *cmd = ctx->commandData;

    while (cmd != NULL) {
        char *nextCmd;
        char *threadIdPart;
        int threadId;
        u32 curMask = 0;
        const char *cmdEnd;

        // It it always fine if we set the single-stepping range to 0,0 by default
        // Because the fields we set are the shadow fields copied to the real fields after debug unpause
        uintptr_t ssStartAddr = 0;
        uintptr_t ssEndAddr = 0;

        // Locate next command, replace delimiter by NUL
        nextCmd = strchr(cmd, ';');
        if (nextCmd != NULL && *nextCmd == ';') {
            *nextCmd++ = 0;
        }

        // Locate thread-id part, parse thread id
        threadIdPart = strchr(cmd, ':');
        if (threadIdPart != NULL) {
            *threadIdPart++ = 0;
        }
        if (threadIdPart == NULL || strcmp(threadIdPart, "-1") == 0) {
            // Default action...
            threadId = -1;
            curMask = ctx->attachedCoreList;
        } else {
            unsigned long id;
            if(GDB_ParseHexIntegerList(&id, threadIdPart, 1, 0) == NULL) {
                return GDB_ReplyErrno(ctx, EILSEQ);
            } else if (id >= MAX_CORE + 1) {
                return GDB_ReplyErrno(ctx, EINVAL);
            }

            threadId = id == 0 ? (int)currentCoreCtx->coreId : (int)id;
            curMask = BIT(threadId - 1) & ctx->attachedCoreList;
        }

        // Parse the command itself
        // Note that we may already have handled that thread in a previous command
        curMask &= ~parsedCoreList;
        switch (cmd[0]) {
            case 'S':
            case 'C': {
                // Check the presence of the two-digit signature, even if we ignore it.
                u8 sg;
                if (GDB_DecodeHex(&sg, cmd + 1, 1) != 1) {
                    return GDB_ReplyErrno(ctx, EILSEQ);
                }
                stepCoreList |= cmd[0] == 'S' ? curMask : 0;
                continueCoreList |= curMask;
                cmdEnd = cmd + 3;
                break;
            }
            case 's':
                stepCoreList |= curMask;
                continueCoreList |= curMask;
                cmdEnd = cmd + 1;
                break;
            case 'c':
                continueCoreList |= curMask;
                cmdEnd = cmd + 1;
                break;
            case 't':
                stopCoreList |= curMask;
                cmdEnd = cmd + 1;
                break;
            case 'r': {
                // Range step
                unsigned long tmp[2];
                cmdEnd = GDB_ParseHexIntegerList(tmp, cmd + 1, 2, 0);
                if (cmdEnd == NULL) {
                    return GDB_ReplyErrno(ctx, EILSEQ);
                }

                ssStartAddr = tmp[0];
                ssEndAddr = tmp[1];
                stepCoreList |= curMask;
                continueCoreList |= curMask;
                break;
            }

            default:
                return GDB_ReplyErrno(ctx, EILSEQ);
        }

        if (*cmdEnd != 0) {
            // We've got garbage data...
            return GDB_ReplyErrno(ctx, EILSEQ);
        }

        FOREACH_BIT (tmp, t, curMask) {
            // Set/unset stepping range for all threads affected by this command
            debugManagerSetSteppingRange(t - 1, ssStartAddr, ssEndAddr);
        }

        parsedCoreList |= curMask;
        cmd = nextCmd;
    }

    // "Note: In non-stop mode, a thread is considered running until GDB acknowledges 
    // an asynchronous stop notification for it with the ‘vStopped’ packet (see Remote Non-Stop)."
    u32 mask = ctx->acknowledgedDebugEventCoreList;

    debugManagerSetSingleStepCoreList(stepCoreList & mask);
    debugManagerBreakCores(stopCoreList & ~mask);
    debugManagerContinueCores(continueCoreList & mask);

    return 0;
}
