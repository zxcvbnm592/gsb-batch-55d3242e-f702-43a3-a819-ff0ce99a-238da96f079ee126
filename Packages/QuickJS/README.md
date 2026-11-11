# QuickJS (vendored) + idj sandbox

- `Sources/CQuickJS/*.c|h` except `idj_sandbox.*`: quickjs-ng **v0.10.1** (commit `3c9afc9`), unmodified, MIT — see `LICENSE-quickjs-ng.txt`.
- `idj_sandbox.c` / `include/idj_sandbox.h`: i短剧 plugin sandbox. One `JSRuntime` per plugin (own heap, memory cap,
  stack cap, interrupt deadline). The sandbox context is created **without** the Eval intrinsic, so `eval`, `new Function`,
  `AsyncFunction`/`GeneratorFunction` constructors all throw "eval is not supported". Plugin source is compiled in a
  throw-away compiler context, serialized to bytecode and loaded into the sandbox context. No module loader is installed,
  so `import` of anything fails. The only native entry point is `__host(name, argsJSON)`, captured by the trusted prelude
  and deleted from the global object before plugin code runs.
- Timeout model: `timeout_sec` in `idj_load` / `idj_call` / `idj_eval_script` budgets **JS execution only** — the deadline
  is paused while a `__host` call runs and re-armed with the remaining budget (small floor) when it returns.
  `timeout_sec <= 0` disables the deadline. A budget expiry interrupts the script and reports `E_TIMEOUT`.
- `Tests/idj_sandbox_tests.c`: offline C tests for the budget/interrupt logic (mock host, no network).
  Run with `sh Packages/QuickJS/Tests/run_tests.sh`.
