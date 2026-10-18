#!/usr/bin/env bash
set -euo pipefail

target="${1:-${IDF_TARGET:-esp32}}"
if [[ $# -gt 0 ]]; then shift; fi
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/idf-env.sh" "${target}"
ensure_idf_target

release_monitor_ports
if [[ -n "${IDF_PORT:-}" ]]; then
    run_idf -p "${IDF_PORT}" flash "$@"
else
    run_idf flash "$@"
fi