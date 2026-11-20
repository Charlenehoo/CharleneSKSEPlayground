#include "Probe.h"

#include "RE/Skyrim.h"
#include "REL/Relocation.h"
#include "SKSE/SKSE.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace Probe {

namespace {

std::once_flag g_veh_once;
std::atomic<bool> g_crash_reported{false};

bool IsReadable(std::uintptr_t addr, std::size_t len) {
    if (!addr || !len) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const auto region = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return addr + len <= region;
}

void HexDump(const char* tag, std::uintptr_t addr, std::size_t len) {
    if (!IsReadable(addr, len)) {
        SKSE::log::warn("[PROBE] {} dump skipped (unreadable) addr=0x{:X} len=0x{:X}", tag, addr,
                        len);
        return;
    }
    const auto* p = reinterpret_cast<const std::uint8_t*>(addr);
    char buf[180];
    for (std::size_t i = 0; i < len; i += 16) {
        int n = std::snprintf(buf, sizeof(buf), "[PROBE] %s +%04X:", tag, unsigned(i));
        for (std::size_t j = 0; j < 16 && i + j < len; ++j) {
            n += std::snprintf(buf + n, sizeof(buf) - n, " %02X", p[i + j]);
        }
        SKSE::log::info("{}", buf);
    }
}

void DumpCBCB(std::uintptr_t addr) {
    SKSE::log::info("[PROBE] CBCB addr = 0x{:X}", addr);
    if (!addr) {
        SKSE::log::error("[PROBE] CBCB addr null");
        return;
    }
    if (!IsReadable(addr, 0x40)) {
        SKSE::log::error("[PROBE] CBCB addr unreadable");
        return;
    }

    HexDump("CBCB-sig", addr, 0x40);

    for (std::size_t off : {std::size_t(0x148), std::size_t(0x164)}) {
        if (!IsReadable(addr + off, 5)) {
            SKSE::log::warn("[PROBE] CBCB +0x{:X} unreadable", off);
            continue;
        }
        const auto b = *reinterpret_cast<const std::uint8_t*>(addr + off);
        const char* tag = b == 0xE8 ? "CALL" : b == 0xE9 ? "JMP" : "?";
        SKSE::log::info("[PROBE] CBCB +0x{:X} = 0x{:02X} ({})", off, b, tag);
        if (b == 0xE8 || b == 0xE9) {
            const auto rel = *reinterpret_cast<const std::int32_t*>(addr + off + 1);
            SKSE::log::info("[PROBE]   target = 0x{:X}",
                            addr + off + 5 + static_cast<std::intptr_t>(rel));
        }
    }

    if (!IsReadable(addr, 0x200)) {
        SKSE::log::warn("[PROBE] CBCB+0x200 unreadable");
        return;
    }
    SKSE::log::info("[PROBE] full E8/E9 scan, CBCB+0..0x200:");
    const auto* p = reinterpret_cast<const std::uint8_t*>(addr);
    for (std::size_t i = 0; i < 0x200; ++i) {
        if (p[i] == 0xE8 || p[i] == 0xE9) {
            const auto rel = *reinterpret_cast<const std::int32_t*>(addr + i + 1);
            const auto tgt = addr + i + 5 + static_cast<std::intptr_t>(rel);
            SKSE::log::info("[PROBE]   +0x{:03X} {} -> 0x{:X}", i, p[i] == 0xE8 ? "call" : "jmp",
                            tgt);
        }
    }
}

void AnalyzeNode(std::uintptr_t rbx) {
    if (!IsReadable(rbx, 0x200)) {
        SKSE::log::error("[PROBE] RBX=0x{:X} not readable, skip node dump", rbx);
        return;
    }
    HexDump("RBX", rbx, 0x200);

    const auto* base = reinterpret_cast<const std::uint8_t*>(rbx);
    // wrong-offset fingerprint: the literal bytes "Combat P"
    for (std::size_t i = 0; i + 8 <= 0x200; ++i) {
        if (std::memcmp(base + i, "Combat P", 8) == 0) {
            SKSE::log::error(
                "[PROBE] *** literal \"Combat P\" at RBX + 0x{:X} (candidate WRONG offset) ***", i);
        }
    }
    // correct-offset candidates: qword slots yielding a printable C-string
    for (std::size_t i = 0; i + 8 <= 0x200; i += 8) {
        const auto v = *reinterpret_cast<const std::uint64_t*>(base + i);
        if (v < 0x10000) continue;
        if (!IsReadable(static_cast<std::uintptr_t>(v), 2)) continue;
        const auto* s = reinterpret_cast<const char*>(v);
        bool ascii = true;
        std::size_t len = 0;
        for (; len < 64; ++len) {
            if (!IsReadable(reinterpret_cast<std::uintptr_t>(s) + len, 1)) {
                ascii = false;
                break;
            }
            const char c = s[len];
            if (c == '\0') break;
            if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) {
                ascii = false;
                break;
            }
        }
        if (ascii && len > 0 && len < 64) {
            SKSE::log::error(
                "[PROBE] RBX + 0x{:X} -> \"{}\" (candidate CORRECT BSFixedString slot)", i, s);
        }
    }
}

LONG CALLBACK AccessViolationHandler(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    if (g_crash_reported.exchange(true)) return EXCEPTION_CONTINUE_SEARCH;

    auto* c = ep->ContextRecord;
    if (!c) return EXCEPTION_CONTINUE_SEARCH;

    const auto rip = static_cast<std::uintptr_t>(c->Rip);
    const auto rsp = static_cast<std::uintptr_t>(c->Rsp);
    const auto rbx = static_cast<std::uintptr_t>(c->Rbx);
    const auto rcx = static_cast<std::uintptr_t>(c->Rcx);
    const auto r8 = static_cast<std::uintptr_t>(c->R8);

    const auto kind = ep->ExceptionRecord->ExceptionInformation[0];
    const auto at = static_cast<std::uintptr_t>(ep->ExceptionRecord->ExceptionInformation[1]);

    SKSE::log::error("========== [PROBE] ACCESS VIOLATION ==========");
    SKSE::log::error("[PROBE] access={} at=0x{:X}",
                     kind == 0   ? "read"
                     : kind == 1 ? "write"
                                 : "exec",
                     at);
    SKSE::log::error("[PROBE] RIP=0x{:X} RSP=0x{:X}", rip, rsp);
    SKSE::log::error("[PROBE] RAX=0x{:X} RBX=0x{:X} RCX=0x{:X} RDX=0x{:X}", (std::uintptr_t)c->Rax,
                     rbx, rcx, (std::uintptr_t)c->Rdx);
    SKSE::log::error("[PROBE] RSI=0x{:X} RDI=0x{:X} R8=0x{:X} R9=0x{:X}", (std::uintptr_t)c->Rsi,
                     (std::uintptr_t)c->Rdi, r8, (std::uintptr_t)c->R9);

    if (rcx == 0x50207461626D6F43ull) {
        SKSE::log::error("[PROBE] RCX == \"Combat P\" garbage -- confirmed same crash signature");
    }

    AnalyzeNode(rbx);

    SKSE::log::error("========== [PROBE] END ==========");
    return EXCEPTION_CONTINUE_SEARCH;  // 不改行为: 让游戏正常崩
}

}  // namespace

void Install() {
    // 一次性挂 VEH (First=1 → 先于 SEH 运行)
    std::call_once(g_veh_once, [] {
        const auto h = AddVectoredExceptionHandler(1, AccessViolationHandler);
        SKSE::log::info("[PROBE] VEH installed, handle=0x{:X}",
                        reinterpret_cast<std::uintptr_t>(h));
    });

    // 一次性 dump CBCB 代码
    try {
        REL::Relocation<std::uintptr_t> base{RELOCATION_ID(48147, 49178)};
        DumpCBCB(base.address());
    } catch (const std::exception& e) {
        SKSE::log::error("[PROBE] RELOCATION_ID(48147, 49178) lookup failed: {}", e.what());
    }
}

}  // namespace Probe
