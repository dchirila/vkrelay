#!/usr/bin/env bash
# vkrelay2: summarize VKRELAY2-PROFILE dump lines (see common/vkrpc/rpc_profile.h) from
# one or more logs (app.log carries the client end; the daemon worker log carries the worker end).
#
#   scripts/dev/profile_report.sh <log> [<log>...]
#
# Prints per-end op tables sorted by total time, the client-minus-worker per-op delta (labeled
# APPROXIMATE marshal+wire+response overhead -- the two ends are self-timed, never clock-
# correlated; negative raw deltas are clamped to 0 and flagged '*'), and the frame summary (FPS =
# frames / frame_us_total). STRICT parser (6.1): a malformed VKRELAY2-PROFILE line is
# a hard error, so dump-grammar drift breaks the profile smoke instead of degrading silently.
set -u

if [ "$#" -lt 1 ]; then
    echo "usage: profile_report.sh <log> [<log>...]" >&2
    exit 2
fi
for f in "$@"; do
    [ -r "$f" ] || { echo "profile_report.sh: cannot read $f" >&2; exit 2; }
done

cat "$@" | awk '
function die(msg) { printf("profile_report.sh: %s\n", msg) > "/dev/stderr"; exit 1 }
function getkv(tok,   i) { i = index(tok, "="); return substr(tok, i + 1) }
function keyof(tok,   i) { i = index(tok, "="); return substr(tok, 1, i - 1) }
{
    m = index($0, "VKRELAY2-PROFILE ")
    if (m == 0) { next }
    line = substr($0, m + length("VKRELAY2-PROFILE "))
    n = split(line, tok, " ")
    delete kv
    for (i = 1; i <= n; i++) {
        if (index(tok[i], "=") == 0) { die("token without = : " tok[i]) }
        kv[keyof(tok[i])] = getkv(tok[i])
    }
    if (!("end" in kv)) { die("line without end=: " line) }
    e = kv["end"]
    if (e != "client" && e != "worker") { die("unknown end: " e) }
    if ("op" in kv) {
        if (!("count" in kv) || !("bytes_out" in kv) || !("bytes_in" in kv) ||
            !("total_us" in kv) || !("min_us" in kv) || !("max_us" in kv) || !("hist" in kv)) {
            die("op line missing keys: " line)
        }
        o = kv["op"]
        ops[e, o] = 1
        opset[o] = 1
        count[e, o] += kv["count"]
        bout[e, o] += kv["bytes_out"]
        bin[e, o] += kv["bytes_in"]
        total[e, o] += kv["total_us"]
        if (!((e, o) in minu) || kv["min_us"] + 0 < minu[e, o]) { minu[e, o] = kv["min_us"] + 0 }
        if (kv["max_us"] + 0 > maxu[e, o]) { maxu[e, o] = kv["max_us"] + 0 }
        seen[e] = 1
    } else if ("frames" in kv) {
        frames[e] += kv["frames"]
        fus[e] += kv["frame_us_total"]
        fops[e] += kv["frame_ops_total"]
        fbytes[e] += kv["frame_bytes_total"]
        if (kv["frame_us_max"] + 0 > fmax[e]) { fmax[e] = kv["frame_us_max"] + 0 }
        if (!(e in fmin) || (kv["frame_us_min"] + 0 > 0 && kv["frame_us_min"] + 0 < fmin[e])) {
            fmin[e] = kv["frame_us_min"] + 0
        }
        seen[e] = 1
    } else if ("record_phases" in kv) {
        # the worker record handler internal phase split (see rpc_profile.h).
        if (!("commands" in kv) || !("json_parse_us" in kv) || !("decode_us" in kv) ||
            !("validate_us" in kv) || !("replay_us" in kv) || !("execute_us" in kv)) {
            die("record_phases line missing keys: " line)
        }
        rp_count[e] += kv["record_phases"]
        rp_cmds[e] += kv["commands"]
        rp_parse[e] += kv["json_parse_us"]
        rp_decode[e] += kv["decode_us"]
        rp_val[e] += kv["validate_us"]
        rp_replay[e] += kv["replay_us"]
        rp_exec[e] += kv["execute_us"]
        seen[e] = 1
    } else if ("upload_sweep" in kv) {
        # The ICD coherent-flush-at-submit sweep (client end; see rpc_profile.h).
        if (!("scan_bytes" in kv) || !("filtered_bytes" in kv) || !("payload_bytes" in kv) ||
            !("sweep_us" in kv)) {
            die("upload_sweep line missing keys: " line)
        }
        us_count[e] += kv["upload_sweep"]
        us_scan[e] += kv["scan_bytes"]
        us_filt[e] += kv["filtered_bytes"]
        us_payload[e] += kv["payload_bytes"]
        us_time[e] += kv["sweep_us"]
        seen[e] = 1
    } else if ("frame_ring_us" in kv) {
        ring[e] = kv["frame_ring_us"] # kept verbatim; spike analysis reads it directly
    } else {
        die("unrecognized record: " line)
    }
}
function print_table(e,   o, i, m, tmp) {
    if (!(e in seen)) { return }
    printf("== %s op table (sorted by total_us) ==\n", e)
    printf("%-42s %10s %12s %10s %9s %9s %10s %10s\n",
           "op", "count", "total_ms", "avg_us", "min_us", "max_us", "out_KB", "in_KB")
    # selection sort over ops by total desc (op counts are small)
    m = 0
    for (o in opset) { if ((e, o) in ops) { m++; sorted[m] = o } }
    for (i = 1; i <= m; i++) {
        for (j = i + 1; j <= m; j++) {
            if (total[e, sorted[j]] + 0 > total[e, sorted[i]] + 0) {
                tmp = sorted[i]; sorted[i] = sorted[j]; sorted[j] = tmp
            }
        }
    }
    for (i = 1; i <= m; i++) {
        o = sorted[i]
        printf("%-42s %10d %12.2f %10.1f %9d %9d %10.1f %10.1f\n", o, count[e, o],
               total[e, o] / 1000.0, count[e, o] > 0 ? total[e, o] / count[e, o] : 0,
               minu[e, o], maxu[e, o], bout[e, o] / 1024.0, bin[e, o] / 1024.0)
    }
    delete sorted
    printf("\n")
}
END {
    if (!("client" in seen) && !("worker" in seen)) { die("no VKRELAY2-PROFILE records found") }
    print_table("client")
    print_table("worker")
    if (("client" in seen) && ("worker" in seen)) {
        printf("== client-minus-worker per op (APPROX marshal+wire+response overhead; * = negative raw, clamped) ==\n")
        printf("%-42s %12s %10s\n", "op", "delta_ms", "flag")
        for (o in opset) {
            if (((("client") SUBSEP o) in ops) && ((("worker") SUBSEP o) in ops)) {
                d = total["client", o] - total["worker", o]
                flag = ""
                if (d < 0) { d = 0; flag = "*" }
                printf("%-42s %12.2f %10s\n", o, d / 1000.0, flag)
            }
        }
        printf("\n")
    }
    for (e in seen) {
        if (rp_count[e] + 0 > 0) {
            n = rp_count[e]
            marshal = rp_parse[e] + rp_decode[e]
            printf("== %s record phases (the validation/replay go/no-go split) ==\n", e)
            printf("records=%d commands=%d avg_commands=%.1f\n", n, rp_cmds[e], rp_cmds[e] / n)
            printf("%-14s %12s %10s %8s\n", "phase", "total_ms", "avg_us", "pct")
            rp_total = marshal + rp_exec[e]
            printf("%-14s %12.2f %10.1f %7.1f%%\n", "json_parse", rp_parse[e] / 1000.0,
                   rp_parse[e] / n, rp_total > 0 ? 100.0 * rp_parse[e] / rp_total : 0)
            printf("%-14s %12.2f %10.1f %7.1f%%\n", "decode+hex", rp_decode[e] / 1000.0,
                   rp_decode[e] / n, rp_total > 0 ? 100.0 * rp_decode[e] / rp_total : 0)
            printf("%-14s %12.2f %10.1f %7.1f%%\n", "validate", rp_val[e] / 1000.0,
                   rp_val[e] / n, rp_total > 0 ? 100.0 * rp_val[e] / rp_total : 0)
            printf("%-14s %12.2f %10.1f %7.1f%%\n", "replay", rp_replay[e] / 1000.0,
                   rp_replay[e] / n, rp_total > 0 ? 100.0 * rp_replay[e] / rp_total : 0)
            resid = rp_exec[e] - rp_val[e] - rp_replay[e]
            printf("%-14s %12.2f %10.1f %7.1f%%\n", "dispatch/other", resid / 1000.0,
                   resid / n, rp_total > 0 ? 100.0 * resid / rp_total : 0)
            printf("marshal(parse+decode)=%.1f%% of the handler; execute=%.1f%%\n\n",
                   rp_total > 0 ? 100.0 * marshal / rp_total : 0,
                   rp_total > 0 ? 100.0 * rp_exec[e] / rp_total : 0)
        }
        if (us_count[e] + 0 > 0) {
            # The sweep runs OUTSIDE every RPC op -- this is the only place
            # its cost shows. filtered==scan means no page pre-filter was active.
            printf("== %s upload sweep (ICD coherent-flush-at-submit; NOT in the op table) ==\n", e)
            printf("sweeps=%d total_ms=%.2f avg_us=%.1f scan_MB_per_sweep=%.2f filtered_MB_per_sweep=%.2f payload_KB_per_sweep=%.1f\n",
                   us_count[e], us_time[e] / 1000.0, us_time[e] / us_count[e],
                   us_scan[e] / us_count[e] / 1048576.0, us_filt[e] / us_count[e] / 1048576.0,
                   us_payload[e] / us_count[e] / 1024.0)
            printf("\n")
        }
        if (frames[e] + 0 > 0) {
            printf("== %s frames ==\n", e)
            printf("frames=%d fps=%.1f avg_frame_ms=%.2f min_ms=%.2f max_ms=%.2f avg_ops_per_frame=%.1f avg_kb_per_frame=%.1f\n",
                   frames[e], frames[e] * 1000000.0 / fus[e], fus[e] / frames[e] / 1000.0,
                   fmin[e] / 1000.0, fmax[e] / 1000.0, fops[e] / frames[e],
                   fbytes[e] / frames[e] / 1024.0)
            if (e in ring) { printf("frame_ring_us=%s\n", ring[e]) }
            printf("\n")
        }
    }
}
'
