#pragma once
//
// ELEMENTIA-HARDENING — central client protection module (anti-debug / anti-dump
// / integrity / anti-tamper / tool-detection).
// =============================================================================
// This is the ONE place that wires up client-side hardening. Everything is
// funnelled through a small public API so it can be enabled/disabled centrally
// and can never leak into the legitimate start-up path.
//
//                          !!!  CENTRAL KILL-SWITCH  !!!
// -----------------------------------------------------------------------------
// If any hardening layer ever misbehaves in production (false positives,
// launcher/handoff/TLS/update breakage, AV conflicts, ...), flip protection off
// by defining ELEMENTIA_DISABLE_PROTECTION (one line, below) and re-run CI.
// With it defined, EVERY Guard_* entry point compiles to a no-op — the client
// behaves exactly as if this module never existed.
//
//   * Debug builds:      protection is ALWAYS off (see the guard below).
//   * RelWithDebInfo /   protection is ON, unless ELEMENTIA_DISABLE_PROTECTION.
//     Release (CI ships     (CI builds RelWithDebInfo, which defines _DISTRIBUTE)
//     RelWithDebInfo)
//
//   * ELEMENTIA_GUARD_PASSIVE: build with full DETECTION + logging but NO active
//     reaction (no delayed exit). Useful as a canary / for validating there are
//     no false positives on real players before arming the reaction. Detection
//     results are still queryable via Elementia_Guard_IsCompromised().
//
// Design rules honoured here (see ElementiaGuard.cpp for the implementations):
//   (a) MUST NOT break legitimate operation (login / DirectEnter handoff / TLS /
//       auto-update / rendering keep working). Every check is read-only and
//       side-effect free except the (optional, delayed, high-threshold) reaction.
//   (b) Fail-safe: a check that cannot run (API missing, access denied, ...) is
//       treated as "no signal", never as a detection. No check crashes.
//   (c) Configurable/off-switchable via the macros above.
//   (d) Deterrent only. The server stays authoritative — that is the real
//       defence. This module just raises the cost of client tampering.
//
#include <windows.h>

// --- Master compile-time switch ------------------------------------------------
// Uncomment the next line to hard-disable ALL client hardening (kill-switch):
// #define ELEMENTIA_DISABLE_PROTECTION 1

// Detection-only mode (no active reaction). While defined, the module runs the
// full detection + integrity + tool scans and publishes Elementia_Guard_IsCompromised(),
// but NEVER self-terminates the client.
//
// >>> ARMED FOR THE FIRST SHIPPED RELEASE <<<
// The initial distribution ships in PASSIVE mode ON PURPOSE: we first want to
// collect real-world telemetry (via IsCompromised()) and confirm zero false
// positives on legitimate players BEFORE the active reaction (delayed
// TerminateProcess) is ever enabled. Only remove this define once the passive
// telemetry is clean AND the false-positive fixes in the scoring model
// (per-window multi-signal arming, category decay, capped CRC contribution,
// soft-only window/NtGlobalFlag signals) have been validated in the field.
#define ELEMENTIA_GUARD_PASSIVE 1

// Protection is only ever compiled into distribution builds (RelWithDebInfo /
// Release define _DISTRIBUTE via the root CMakeLists) and can be force-disabled.
#if defined(_DISTRIBUTE) && !defined(ELEMENTIA_DISABLE_PROTECTION)
#	define ELEMENTIA_PROTECT 1
#else
#	define ELEMENTIA_PROTECT 0
#endif

// -----------------------------------------------------------------------------
// Public API. These are always declared so call sites stay clean; when
// ELEMENTIA_PROTECT == 0 the definitions in the .cpp are compiled as no-ops.
// -----------------------------------------------------------------------------

// Call ONCE, early in start-up (after core init, before the Python UI boots).
// Installs anti-dump header scrubbing and captures the .text integrity baseline.
// Safe no-op when disabled. Never blocks or fails the launch.
void Elementia_Guard_Init();

// Call every frame from the main loop. Internally throttled + randomised so the
// real cost is a rotating subset of cheap checks a few times per minute. Safe to
// call unconditionally; returns immediately when disabled or not yet due.
void Elementia_Guard_Tick();

// True once the module has decided (with high confidence) that the process is
// being debugged / tampered / analysed. Exposed so other systems (e.g. a future
// server-side telemetry flag) can react. Always false when disabled/passive-clean.
bool Elementia_Guard_IsCompromised();
