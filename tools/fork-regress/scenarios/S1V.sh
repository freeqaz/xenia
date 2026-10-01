#!/bin/bash
# S1V -- S1 under Vulkan on GPU 1 with headless frame dump.
# usage: S1V.sh <run-dir> <run-index>
set -uo pipefail
source "$HARNESS/lib/common.sh"
RD="$1"
TIMEOUT_S="${FR_S1_TIMEOUT_S:-240}"
DEV="${FR_VULKAN_DEVICE:-1}"
# Refuse to share a GPU someone else is computing on (other agents' native
# ports run on GPU 0; GPU 1 is ours only if it is idle).
if command -v nvidia-smi >/dev/null; then
  uuid="$(nvidia-smi --query-gpu=index,uuid --format=csv,noheader | awk -F', ' -v d="$DEV" '$1==d{print $2}')"
  busy="$(nvidia-smi --query-compute-apps=gpu_uuid,pid,process_name --format=csv,noheader | grep -c "$uuid" || true)"
  if [ -n "$uuid" ] && [ "$busy" -gt 0 ] && [ "${FR_GPU_SHARE_OK:-0}" != 1 ]; then
    mkdir -p "$RD"
    nvidia-smi --query-compute-apps=gpu_uuid,pid,process_name --format=csv,noheader > "$RD/gpu_busy.txt"
    xr_skip "$RD" "GPU $DEV busy ($busy compute apps); set FR_GPU_SHARE_OK=1 to run anyway" inconclusive
    exit 0
  fi
fi
xr_begin
dc3_original_args vulkan $(( (TIMEOUT_S - 10) * 1000 ))
mkdir -p "$RD/frames" "$RD/pcache"
xr_arg "--vulkan_device=$DEV" "--dump_frames_path=$RD/frames" --headless_capture_interval=300
# A private pipeline cache: the default is a shared /tmp path.
xr_opt vulkan_pipeline_cache_path "$RD/pcache"
xr_run "$RD" "$TIMEOUT_S" "$HARNESS/config/dc3-oracle.defaults.toml"
