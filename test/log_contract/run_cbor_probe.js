// =====================================================
// P1.4 CBOR 探针的 wasm32 运行器（零依赖，只用 node 内建模块）
//
// 用法:
//   node test/log_contract/run_cbor_probe.js <probe_cbor.wasm>
//
// 退出码: 0 = 全部检查通过；1 = 有检查失败；2 = wasm 装载失败
// =====================================================

const fs = require("fs");

const wasmPath = process.argv[2];

if (!wasmPath) {
    console.error("用法: node run_cbor_probe.js <probe_cbor.wasm>");
    process.exit(2);
}

let bytes;
try {
    bytes = fs.readFileSync(wasmPath);
} catch (e) {
    console.error("读取 wasm 失败:", e.message);
    process.exit(2);
}

WebAssembly.instantiate(bytes, {})
    .then(({ instance }) => {
        const ex = instance.exports;

        if (typeof ex.probe_run !== "function") {
            console.error("wasm 未导出 probe_run（检查 --export-all）");
            process.exit(2);
        }

        const fails = ex.probe_run();

        const ptr = ex.probe_log_ptr();
        const len = ex.probe_log_len();
        const mem = new Uint8Array(ex.memory.buffer, ptr, len);
        process.stdout.write(Buffer.from(mem).toString("utf8"));

        process.exit(fails === 0 ? 0 : 1);
    })
    .catch((e) => {
        console.error("wasm 装载失败:", e.message);
        process.exit(2);
    });
