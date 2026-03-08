#!/usr/bin/env bash
# Cache configuration experiment runner.
#
# Usage:
#   ./scripts/cache_experiment.sh [options] [<elf>]
#
#   <elf>              Single ELF to benchmark (default: run all rv32ui-p tests)
#   -o, --output FILE  Also write results to FILE (tab-separated, with header)
#
# Requires: build/core_sim  (make core-sim)

set -euo pipefail

CORE_SIM="./build/core_sim"

if [ ! -x "$CORE_SIM" ]; then
    echo "error: $CORE_SIM not found — run 'make core-sim' first" >&2
    exit 1
fi

# ── Argument parsing ──────────────────────────────────────────────────────────
SINGLE_ELF=""
ELFS_DIR="tests/rv32ui-p"
OUTPUT_FILE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        -o|--output)
            OUTPUT_FILE="$2"; shift 2 ;;
        -*)
            echo "error: unknown option '$1'" >&2; exit 1 ;;
        *)
            if [ -f "$1" ]; then
                SINGLE_ELF="$1"
            else
                echo "error: '$1' is not a file" >&2; exit 1
            fi
            shift ;;
    esac
done

if [ -z "$SINGLE_ELF" ] && [ ! -d "$ELFS_DIR" ]; then
    echo "error: $ELFS_DIR not found — run 'make riscv-tests-build' first" >&2
    exit 1
fi

# ── Configuration table ──────────────────────────────────────────────────────
# Format: "label  l1i_size  l1i_ways  l1d_size  l1d_ways  l2_size  l2_ways"
# Sizes in bytes.  l2_size=0 means no L2 (L1 → DRAM directly).

configs=(
    "A:small-L1        8192  2    8192  2  262144  8"
    "B:baseline       32768  4   32768  4  262144  8"
    "C:large-L1       65536  4   65536  4  262144  8"
    "D:small-L2       32768  4   32768  4   65536  8"
    "E:large-L2       32768  4   32768  4 1048576  8"
    "F:no-L2          32768  4   32768  4       0  8"
    "G:direct-mapped  32768  1   32768  1  262144  1"
    "H:8-way          32768  8   32768  8  262144  8"
)

# ── Output file setup ─────────────────────────────────────────────────────────
# Write a TSV header if an output file was requested.
if [ -n "$OUTPUT_FILE" ]; then
    printf 'config\tcycles\tinstrs\tipc\ti_stalls\td_stalls\tl1i_misses\tl1d_misses\tl2_misses\tpass\ttotal\n' \
        > "$OUTPUT_FILE"
fi

# Helper: emit one result row to stdout (formatted) and optionally to the file.
emit_row() {
    local label="$1" cycles="$2" instrs="$3" ipc="$4" \
          i_stall="$5" d_stall="$6" l1i_m="$7" l1d_m="$8" l2_m="$9" \
          pass="${10}" total="${11}"

    printf "%-20s  %10d  %10d  %6s  %10d  %10d  %d/%d\n" \
        "$label" "$cycles" "$instrs" "$ipc" "$i_stall" "$d_stall" "$pass" "$total"

    if [ -n "$OUTPUT_FILE" ]; then
        printf '%s\t%d\t%d\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n' \
            "$label" "$cycles" "$instrs" "$ipc" "$i_stall" "$d_stall" \
            "$l1i_m" "$l1d_m" "$l2_m" "$pass" "$total" \
            >> "$OUTPUT_FILE"
    fi
}

# ── Print header ─────────────────────────────────────────────────────────────
printf "\n%-20s  %10s  %10s  %6s  %10s  %10s  %8s\n" \
    "Config" "Cycles" "Instrs" "IPC" "I\$-stalls" "D\$-stalls" "Pass"
printf "%s\n" "$(printf '─%.0s' {1..82})"

# ── Run each configuration ───────────────────────────────────────────────────
for cfg_entry in "${configs[@]}"; do
    read -r label l1i_size l1i_ways l1d_size l1d_ways l2_size l2_ways \
        <<< "$cfg_entry"

    args=(
        --l1i-size "$l1i_size" --l1i-ways "$l1i_ways"
        --l1d-size "$l1d_size" --l1d-ways "$l1d_ways"
        --l2-size  "$l2_size"  --l2-ways  "$l2_ways"
    )

    total_cycles=0
    total_instrs=0
    total_i_stall=0
    total_d_stall=0
    total_l1i_misses=0
    total_l1d_misses=0
    total_l2_misses=0
    pass=0
    fail=0

    if [ -n "$SINGLE_ELF" ]; then
        elf_list=("$SINGLE_ELF")
    else
        elf_list=("$ELFS_DIR"/rv32ui-p-*)
    fi

    for elf in "${elf_list[@]}"; do
        [ -f "$elf" ] || continue
        out=$("$CORE_SIM" "${args[@]}" "$elf" 2>/dev/null) || true

        c=$(echo  "$out" | grep -oP 'Cycles:\s+\K[0-9]+'            | head -1)
        i=$(echo  "$out" | grep -oP 'Instructions:\s+\K[0-9]+'       | head -1)
        is=$(echo "$out" | grep -oP 'I\$ stall cycles:\s+\K[0-9]+'   | head -1)
        ds=$(echo "$out" | grep -oP 'D\$ stall cycles:\s+\K[0-9]+'   | head -1)

        # Extract miss counts per cache level.
        # L2 appears in both icache and dcache hierarchy output; awk sums all occurrences.
        l1i_m=$(echo "$out" | awk 'index($0,"=== L1-I$"){f=1} f && index($0,"  Misses:"){match($0,/[0-9]+/); print substr($0,RSTART,RLENGTH); f=0}')
        l1d_m=$(echo "$out" | awk 'index($0,"=== L1-D$"){f=1} f && index($0,"  Misses:"){match($0,/[0-9]+/); print substr($0,RSTART,RLENGTH); f=0}')
        l2_m=$(echo  "$out" | awk 'index($0,"=== L2"){f=1}    f && index($0,"  Misses:"){match($0,/[0-9]+/); s+=substr($0,RSTART,RLENGTH); f=0} END{print s+0}')

        total_cycles=$(( total_cycles + ${c:-0} ))
        total_instrs=$(( total_instrs + ${i:-0} ))
        total_i_stall=$(( total_i_stall + ${is:-0} ))
        total_d_stall=$(( total_d_stall + ${ds:-0} ))
        total_l1i_misses=$(( total_l1i_misses + ${l1i_m:-0} ))
        total_l1d_misses=$(( total_l1d_misses + ${l1d_m:-0} ))
        total_l2_misses=$(( total_l2_misses + ${l2_m:-0} ))

        if echo "$out" | grep -q '\[sim\] PASS'; then
            pass=$(( pass + 1 ))
        else
            fail=$(( fail + 1 ))
        fi
    done

    if [ "$total_instrs" -gt 0 ]; then
        ipc=$(awk "BEGIN { printf \"%.3f\", $total_instrs / $total_cycles }")
    else
        ipc="0.000"
    fi

    emit_row "$label" "$total_cycles" "$total_instrs" "$ipc" \
             "$total_i_stall" "$total_d_stall" \
             "$total_l1i_misses" "$total_l1d_misses" "$total_l2_misses" \
             "$pass" "$(( pass + fail ))"
done

printf "\n"

if [ -n "$OUTPUT_FILE" ]; then
    echo "Results written to $OUTPUT_FILE"
fi
