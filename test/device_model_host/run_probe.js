// =====================================================
// Device Model wasm 探针运行器（node）
//   用法: node run_probe.js <probe.wasm>
//   退出码 = 探针报告的 fail 数（0 = 全通过）
// =====================================================
const fs = require('fs');

(async () => {
  const wasmPath = process.argv[2];
  if (!wasmPath) {
    console.error('usage: node run_probe.js <probe.wasm>');
    process.exit(2);
  }
  const bytes = fs.readFileSync(wasmPath);
  const { instance } = await WebAssembly.instantiate(bytes, {});
  const ex = instance.exports;

  const fails = ex.probe_run();
  const len = ex.probe_log_len();
  const ptr = ex.probe_log_ptr();
  const mem = ex.memory || ex.__linear_memory;
  if (!mem) {
    console.error('no exported wasm memory');
    process.exit(2);
  }
  const view = new Uint8Array(mem.buffer, ptr, len);
  process.stdout.write(Buffer.from(view).toString('utf8'));
  console.log(`exit_failures=${fails}`);
  process.exit(fails === 0 ? 0 : 1);
})().catch((e) => {
  console.error('runner error:', e);
  process.exit(2);
});
