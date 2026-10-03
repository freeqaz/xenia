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
# Inline render (draws + resolves every frame, so the capture reads the
# frontbuffer the title resolved) starts from a persistent warm pipeline cache
# kept with the pinned content. It is copied INTO the run's private cache dir
# here and copied back by S1V.post.sh only after a PASS, so concurrent runs never
# write the shared copy in place. With no seed the run starts cold and its PASS
# writes the seed: a fully cold start (no xenia cache, empty driver cache) does
# NOT deadlock on this capture flow, with or without async pipelines (measured
# 2026-10-03; README "S1V pipeline cache"). FR_S1V_COLD_CACHE=1 ignores the
# seed, to measure a cold start on purpose.
PCACHE_SEED="$(s1v_pcache_seed)"
PCACHE_FROM=seed
if [ "${FR_S1V_COLD_CACHE:-0}" = 1 ]; then PCACHE_FROM=cold
elif [ ! -s "$PCACHE_SEED" ]; then PCACHE_FROM=cold-no-seed; fi
xr_begin
dc3_original_args vulkan $(( (TIMEOUT_S - 10) * 1000 ))
mkdir -p "$RD/frames" "$RD/pcache"
SEED_SHA=""
if [ "$PCACHE_FROM" = seed ]; then
  cp "$PCACHE_SEED" "$RD/pcache/$(basename "$PCACHE_SEED")"
  # Hashed now: the run rewrites its copy at exit.
  SEED_SHA="$(sha256sum "$RD/pcache/$(basename "$PCACHE_SEED")" | awk '{print $1}')"
fi
export XR_META_EXTRA="{\"s1v_pcache\": \"$PCACHE_FROM\", \"s1v_pcache_seed_sha256\": \"$SEED_SHA\"}"
xr_arg "--vulkan_device=$DEV" "--dump_frames_path=$RD/frames" --headless_capture_interval=300
# The run's PRIVATE pipeline cache (seeded above): the default is a shared path.
xr_opt vulkan_pipeline_cache_path "$RD/pcache"
# Inline render: the deferred replay drops 299 of every 300 frames' draws and
# replays the 300th against guest memory the title has since rewritten; its
# gameplay captures are mostly black|blue clears (README, S1V). Older binaries
# spell it dc3_inline_render; one predating both is recorded as dropped.
if has_cvar headless_inline_render; then xr_arg --headless_inline_render=true
else xr_opt dc3_inline_render true; fi
# The capture-path cvars default to upstream behaviour (false) since Lane E;
# this scenario's measurement is the fork's capture flow, so opt in. xr_opt
# drops them on a binary that predates them (where they were already true).
xr_opt headless_skip_submission_wait true
xr_opt headless_capture_only_draws true
xr_opt headless_async_pipelines true
xr_run "$RD" "$TIMEOUT_S" "$HARNESS/config/dc3-oracle.defaults.toml"
