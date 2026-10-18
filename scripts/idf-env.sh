#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${PROJECT_DIR}"

if [[ $# -gt 0 ]]; then
    export IDF_TARGET="$1"
    shift
else
    export IDF_TARGET="${IDF_TARGET:-esp32}"
fi

case "${IDF_TARGET}" in
    esp32)
        target_config=""
        ;;
    esp32c6)
        target_config="ESP32c6"
        ;;
    *)
        echo "Unsupported IDF_TARGET: ${IDF_TARGET} (expected esp32 or esp32c6)" >&2
        exit 1
        ;;
esac

export IDF_PATH="${IDF_PATH:-${HOME}/.platformio/packages/framework-espidf}"
export IDF_PYTHON_ENV_PATH="${IDF_PYTHON_ENV_PATH:-${HOME}/.espressif/python_env/idf6.1_py3.11_env}"
export PATH="${HOME}/.platformio/packages/toolchain-riscv32-esp/bin:${HOME}/.platformio/packages/tool-riscv32-esp-elf-gdb/bin:${HOME}/.platformio/packages/toolchain-xtensa-esp-elf/bin:${HOME}/.platformio/packages/tool-cmake/bin:${HOME}/.platformio/packages/tool-ninja:${PATH}"

export IDF_PYTHON="${IDF_PYTHON_ENV_PATH}/bin/python"
if [[ ! -x "${IDF_PYTHON}" ]]; then
    echo "ESP-IDF Python environment not found: ${IDF_PYTHON}" >&2
    exit 1
fi
if [[ ! -f "${IDF_PATH}/tools/idf.py" ]]; then
    echo "ESP-IDF tools not found: ${IDF_PATH}" >&2
    exit 1
fi

export SDKCONFIG_DEFAULTS="${PROJECT_DIR}/sdkconfig.defaults"
if [[ -n "${target_config}" ]]; then
    if [[ ! -f "${PROJECT_DIR}/sdkconfig.${target_config}" ]]; then
        echo "Target defaults not found: ${PROJECT_DIR}/sdkconfig.${target_config}" >&2
        exit 1
    fi
    export SDKCONFIG_DEFAULTS="${SDKCONFIG_DEFAULTS};${PROJECT_DIR}/sdkconfig.${target_config}"
fi
export BUILD_DIR="${PROJECT_DIR}/build/${IDF_TARGET}"
export SDKCONFIG="${BUILD_DIR}/sdkconfig"
mkdir -p "${BUILD_DIR}"

run_idf() {
    "${IDF_PYTHON}" "${IDF_PATH}/tools/idf.py" -B "${BUILD_DIR}" -D "SDKCONFIG=${SDKCONFIG}" "$@"
}

ensure_idf_target() {
    local configured_target=""
    local custom_partition_table="false"
    if [[ -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
        configured_target="$(sed -n 's/^IDF_TARGET:STRING=//p' "${BUILD_DIR}/CMakeCache.txt")"
    fi
    if [[ -f "${SDKCONFIG}" ]] && \
        grep -Fxq 'CONFIG_PARTITION_TABLE_CUSTOM=y' "${SDKCONFIG}" && \
        grep -Fxq 'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"' "${SDKCONFIG}"; then
        custom_partition_table="true"
    fi

    if [[ "${configured_target}" != "${IDF_TARGET}" || "${custom_partition_table}" != "true" ]]; then
        echo "Refreshing ESP-IDF configuration for ${IDF_TARGET}."
        run_idf set-target "${IDF_TARGET}"
    fi
}

stop_process_tree() {
    local process_id="$1"
    local child_id
    [[ -n "${process_id}" ]] || return 0
    for child_id in $(pgrep -P "${process_id}" 2>/dev/null || true); do
        stop_process_tree "${child_id}"
    done
    kill -TERM "${process_id}" 2>/dev/null || true
}

run_monitor() {
    exec "${IDF_PYTHON}" "${IDF_PATH}/tools/idf.py" -B "${BUILD_DIR}" monitor "$@"
}

release_monitor_ports() {
    local ports=()
    if [[ -n "${IDF_PORT:-}" ]]; then
        ports=("${IDF_PORT}")
    else
        for port in /dev/cu.usbmodem* /dev/cu.usbserial*; do
            [[ -e "${port}" ]] && ports+=("${port}")
        done
    fi

    for port in "${ports[@]}"; do
        local pids
        pids="$(lsof -t "${port}" 2>/dev/null || true)"
        for pid in ${pids}; do
            local command_line
            command_line="$(ps -p "${pid}" -o command= 2>/dev/null || true)"
            if [[ "${command_line}" == *idf_monitor.py* || "${command_line}" == *esp_idf_monitor* || "${command_line}" == *platformio*monitor* ]]; then
                echo "Stopping existing monitor (pid ${pid}) on ${port}."
                stop_process_tree "${pid}"
            else
                echo "Serial port ${port} is busy: ${command_line}" >&2
                return 1
            fi
        done
    done
}