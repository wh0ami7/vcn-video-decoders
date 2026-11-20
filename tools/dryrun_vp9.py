"""lldb dry run for VP9. The driver has no VP9 path, so nothing here is compared with an Apple message: it
dumps what vcnvp9.c built and kills the process before the VP9 command can reach the kernel.
lldb --batch -o "command script import dryrun_vp9.py" -o "dryrun_vp9 <session|decode> <out_dir> <decode_vp9> <args...>"
session: VCNVP9_SESSION=vp9, dump the create message, kill before it is submitted.
decode:  VCNVP9_SESSION=av1, so the only thing that goes out is a normal AV1 create on VCN0, which is
checked here before it is allowed. Then dump the first decode message and kill before submitting.
"""

from __future__ import annotations

import lldb
import os
import re
import traceback

_STATE = {
    "mode": None,
    "out": None,
    "phase": 0,
    "session_msg": None,
    "session_seen": False,
    "dumped": False,
    "killed": False,
    "trap": None,
}

_VCN_CODEC_AV1 = 0x13
_VCN_CODEC_VP9 = 0x11


def _safe_dump_path(path: str) -> str:
    """Allow only a predictable dump path in an lldb C expression (CWE-94)."""
    if not re.fullmatch(r"[A-Za-z0-9._/-]+", path) or ".." in path:
        raise ValueError(f"refusing dump path: {path!r}")
    return path


def _kill(frame, why):
    print(f"[dryrun] KILL: {why}", flush=True)
    _STATE["killed"] = True
    frame.GetThread().GetProcess().Kill()


def _u32(proc, addr):
    err = lldb.SBError()
    v = proc.ReadUnsignedFromMemory(addr, 4, err)
    return v if err.Success() else None


def _dump(frame, addr, size, name):
    path = os.path.abspath(os.path.join(_STATE["out"], name))
    opts = lldb.SBExpressionOptions()
    opts.SetIgnoreBreakpoints(True)
    opts.SetTryAllThreads(False)
    v = frame.EvaluateExpression(
        f'(int)vcnvp9_dump((void*){addr:#x}, {size}, "{_safe_dump_path(path)}")',
        opts,
    )
    print(
        f"[dryrun] dump {name}: {v.GetValue()} bytes ({v.GetError()})",
        flush=True,
    )


def safe(fn):
    def wrapped(frame, bp_loc, extra, d):
        try:
            return fn(frame, bp_loc, extra, d)
        except Exception:
            traceback.print_exc()
            _kill(frame, f"exception in {fn.__name__}")
            return True

    return wrapped


@safe
def on_checkpoint(frame, bp_loc, extra, d):
    phase = frame.FindRegister("rdi").GetValueAsUnsigned() & 0xFFFFFFFF
    n = _STATE["trap"].GetNumResolvedLocations()
    print(
        f"[dryrun] checkpoint {phase}: submit trap locations = {n}", flush=True
    )
    if n < 1:
        _kill(frame, "kernel submission trap not armed")
        return True
    _STATE["phase"] = phase
    return False


@safe
def on_session_message(frame, bp_loc, extra, d):
    proc = frame.GetThread().GetProcess()
    msg = frame.FindRegister("rdi").GetValueAsUnsigned()
    _STATE["session_msg"] = msg
    _STATE["session_seen"] = True
    stream = _u32(proc, msg + 40)
    print(
        f"[dryrun] create message at {msg:#x}: msg_type={_u32(proc, msg + 12)} stream_type={stream:#x} {_u32(proc, msg + 48)}x{_u32(proc, msg + 52)}",
        flush=True,
    )
    want = _VCN_CODEC_VP9 if _STATE["mode"] == "session" else _VCN_CODEC_AV1
    if stream != want:
        _kill(
            frame,
            f"create message has stream type {stream:#x}, this mode needs {want:#x}",
        )
        return True
    return False


@safe
def on_message_built(frame, bp_loc, extra, d):
    _STATE["data"] = frame.FindRegister("rdi").GetValueAsUnsigned()
    print(
        f"[dryrun] decode message built, driver data block {_STATE['data']:#x}",
        flush=True,
    )
    return False


@safe
def on_submit(frame, bp_loc, extra, d):
    proc = frame.GetThread().GetProcess()
    err = lldb.SBError()
    th = frame.GetThread()
    bt = " <- ".join(
        (th.GetFrameAtIndex(i).GetFunctionName() or "?")
        for i in range(1, min(6, th.GetNumFrames()))
    )
    if _STATE["dumped"]:
        return True
    if _STATE["phase"] == 0:
        print(f"[dryrun] driver setup submit allowed: {bt}", flush=True)
        return False
    if _STATE["mode"] == "session":
        if not _STATE["session_seen"]:
            print(
                f"[dryrun] submit before the create message allowed: {bt}",
                flush=True,
            )
            return False
        _dump(frame, _STATE["session_msg"], 4096, "create_msg.bin")
        _STATE["dumped"] = True
        _kill(
            frame, "stopping before the VP9 create message reaches the kernel"
        )
        return True
    # decode mode
    if _STATE["phase"] == 1:
        if _STATE["session_seen"]:
            stream = _u32(proc, _STATE["session_msg"] + 40)
            if stream != _VCN_CODEC_AV1:
                _kill(frame, f"create message changed to {stream:#x}")
                return True
        print(f"[dryrun] AV1 session submit allowed: {bt}", flush=True)
        return False
    data = _STATE.get("data")
    if not data:
        _kill(
            frame, "decode submit reached without our message builder running"
        )
        return True
    msg = proc.ReadPointerFromMemory(data + 224, err)
    it = proc.ReadPointerFromMemory(data + 632, err)
    ctx_tex = proc.ReadPointerFromMemory(data + 464, err)
    ctx = proc.ReadPointerFromMemory(ctx_tex + 152, err) if ctx_tex else 0
    _dump(frame, msg, 4096, "vcn_msg.bin")
    _dump(frame, it, 2560, "it_probs.bin")
    if ctx:
        _dump(frame, ctx, 2304, "ctx_probs.bin")
    _STATE["dumped"] = True
    _kill(frame, "stopping before the VP9 decode message reaches the kernel")
    return True


def dryrun_vp9(debugger, command, result, d):
    args = command.split()
    _STATE["mode"], _STATE["out"] = args[0], args[1]
    exe, prog_args = args[2], args[3:]
    os.makedirs(_STATE["out"], exist_ok=True)
    target = debugger.CreateTarget(exe)
    trap = target.BreakpointCreateByName("VAContext::submitDataBuffers()")
    if trap.GetNumLocations() == 0:
        trap = target.BreakpointCreateByRegex(r"^VAContext::submitDataBuffers")
    trap.SetScriptCallbackFunction("dryrun_vp9.on_submit")
    _STATE["trap"] = trap
    for name, fn in (
        ("vcnvp9_dryrun_checkpoint", "on_checkpoint"),
        ("vcnvp9_session_message", "on_session_message"),
        ("vcnvp9_message_built", "on_message_built"),
    ):
        b = target.BreakpointCreateByName(name)
        b.SetScriptCallbackFunction(f"dryrun_vp9.{fn}")
    session = "vp9" if _STATE["mode"] == "session" else "av1"
    # decode_vp9 opens the driver under a debugger only with VCNVP9_DRYRUN=1,
    env = [
        f"{k}={v}"
        for k, v in os.environ.items()
        if k not in ("VCNVP9_ALLOW_GPU", "VCNVP9_SESSION", "VCNVP9_DRYRUN")
    ] + [f"VCNVP9_SESSION={session}", "VCNVP9_DRYRUN=1"]
    li = lldb.SBLaunchInfo(prog_args)
    li.SetEnvironmentEntries(env, False)
    err = lldb.SBError()
    debugger.SetAsync(False)
    proc = target.Launch(li, err)
    print(f"[dryrun] launch: {err}", flush=True)
    while (
        not _STATE["killed"]
        and proc.IsValid()
        and proc.GetState() == lldb.eStateStopped
    ):
        proc.Continue()
    print(
        f"[dryrun] final state={proc.GetState()} exit={proc.GetExitStatus()} dumped={_STATE['dumped']}",
        flush=True,
    )


def __lldb_init_module(debugger, d):
    debugger.HandleCommand(
        "command script add -f dryrun_vp9.dryrun_vp9 dryrun_vp9"
    )
