# milohax Xenia fork

Fork of xenia-project/xenia that runs Dance Central 3 and Rock Band 3 headless
on Linux. **Read [docs/fork/README.md](docs/fork/README.md) first**: it indexes
every fork doc, the current cleanup plan and the regression harness.

- Build: `./xb premake && make -C build xenia-headless config=checked_linux -j12`
  (windowed: `xenia-app`, binary `xenia`).
- Test: `tools/fork-regress/run.sh` (see its README); wrap every run in
  `flock /home/free/tmp/fork-regress.lock`, and never conclude from one run.
- Title code goes under `src/xenia/titles/`; keep the diff against upstream
  minimal; new mitigation cvars default to upstream behaviour.
- Runs need the sandbox off (GPU, sockets). By hand, always pass a private
  `--storage_root` so the shared `~/.local/share/Xenia` config is not rewritten.
