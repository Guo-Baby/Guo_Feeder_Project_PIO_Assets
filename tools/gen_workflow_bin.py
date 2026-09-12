#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
从 data/workflow.json 生成 Workflow 初始 BIN 文件（LittleFS uploadfs 基线）。

用法:
    python tools/gen_workflow_bin.py [--src data/workflow.json] [--out data/workflow]

产出:
    <out>/meta.bin
    <out>/wfNN/stepMM.bin

为什么要这个脚本
----------------
`pio run -t uploadfs` 会用 data/ 目录整体重建 LittleFS 镜像，
板载 /workflow/*.bin 会被清空。设备虽有 JSON 回退自愈
（workflow_load_from_storage 失败 → 读 /workflow.json → migrate_to_storage），
但把 BIN 直接放进镜像可以让 uploadfs 之后 BIN 立即可用，
不依赖回退路径，也便于版本化管理"出厂基线"。

格式来源（必须与源码保持一致）
------------------------------
src/workflow_storage.cpp:
    WF_STG_STEP_HEADER_SIZE 16 / WF_STG_META_HEADER_SIZE 12
    Meta v3 entry 89B，Step payload 见 serialize_step_payload()
    CRC32: init 0xFFFFFFFF, poly 0xEDB88320 (reflected), final xor 0xFFFFFFFF

⚠️ 维护约定：
    若 src/workflow_storage.h 的 WF_STG_META_VERSION 升级（v3 → v4），
    必须同步更新本脚本的 META_VERSION，否则生成的 meta.bin 会被设备
    以 VERSION_MISMATCH / VERSION_TOO_NEW 拒绝 —— 此时设备会回退到
    /workflow.json，功能不会丢失，但 BIN 基线失效。
"""
import argparse
import json
import os
import struct
import sys

# ---- 与 src/workflow_storage.h 保持一致 ----
WF_STG_MAX_COUNT = 16
WF_STG_MAX_STEP = 16
WF_STG_MAX_PARAM = 8

WF_STG_META_MAGIC = 0x57464D54     # "WFMT"
WF_STG_STEP_MAGIC = 0x57465350     # "WFSP"

WF_STG_VERSION = 1                 # Step BIN 格式版本（不变）
WF_STG_META_VERSION = 3            # Meta 格式版本

WF_STG_META_HEADER_SIZE = 12
WF_STG_STEP_HEADER_SIZE = 16

WF_STG_ID_MAX_LEN = 32
WF_STG_NAME_MAX_LEN = 32
WF_STG_PARAM_NAME_LEN = 16
WF_STG_PARAM_STR_LEN = 32

# WorkflowStepType: 0=ACTION 1=TRIGGER
STEP_TYPE = {"action": 0, "trigger": 1}
# WorkflowInstanceType: 0=TRIGGER 1=ACTION
INSTANCE_TYPE = {"trigger": 0, "action": 1}
# WorkflowParamType: 0=int 1=float 2=bool 3=string
PARAM_INT, PARAM_FLOAT, PARAM_BOOL, PARAM_STRING = 0, 1, 2, 3


# ---------------- CRC32（与 wf_crc32_* 完全一致）----------------
def _make_crc_table():
    tbl = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0xEDB88320 if (c & 1) else (c >> 1)
        tbl.append(c & 0xFFFFFFFF)
    return tbl


_CRC_TABLE = _make_crc_table()


def crc32_init():
    return 0xFFFFFFFF


def crc32_update(crc, data):
    for b in data:
        crc = _CRC_TABLE[(crc ^ b) & 0xFF] ^ (crc >> 8)
        crc &= 0xFFFFFFFF
    return crc


def crc32_final(crc):
    return (crc ^ 0xFFFFFFFF) & 0xFFFFFFFF


def crc32_bytes(data):
    return crc32_final(crc32_update(crc32_init(), data))


# ---------------- 小端写入辅助 ----------------
class Buf:
    def __init__(self):
        self.b = bytearray()

    def u8(self, v):
        self.b += struct.pack("<B", v & 0xFF)

    def u16(self, v):
        self.b += struct.pack("<H", v & 0xFFFF)

    def u32(self, v):
        self.b += struct.pack("<I", v & 0xFFFFFFFF)

    def i32(self, v):
        self.b += struct.pack("<i", v)

    def f32(self, v):
        self.b += struct.pack("<f", v)

    def fixed(self, s, n):
        raw = s.encode("utf-8") if s else b""
        raw = raw[:n]
        self.b += raw + b"\x00" * (n - len(raw))


# ---------------- Step payload ----------------
def param_kind(value):
    """与 apply_workflow_json / workflow_parse_json 的类型判定保持一致。"""
    if isinstance(value, bool):
        return PARAM_BOOL
    if isinstance(value, int):
        return PARAM_INT
    if isinstance(value, float):
        return PARAM_FLOAT
    return PARAM_STRING


def serialize_step_payload(wf_idx, step_idx, step):
    p = Buf()
    p.u8(wf_idx)
    p.u8(step_idx)

    s_type = step.get("type")
    if s_type not in STEP_TYPE:
        raise ValueError(
            "wf%d step%d: type 缺失或非法 (%r)，必须 trigger/action"
            % (wf_idx, step_idx, s_type)
        )

    p.u8(STEP_TYPE[s_type])
    p.u8(INSTANCE_TYPE[s_type])
    p.fixed(step.get("id", ""), WF_STG_ID_MAX_LEN)

    params = step.get("params") or {}
    if not isinstance(params, dict):
        raise ValueError("wf%d step%d: params 必须是对象" % (wf_idx, step_idx))
    if len(params) > WF_STG_MAX_PARAM:
        raise ValueError(
            "wf%d step%d: params %d 个，超过上限 %d"
            % (wf_idx, step_idx, len(params), WF_STG_MAX_PARAM)
        )

    p.u8(len(params))
    for name, value in params.items():
        kind = param_kind(value)
        p.fixed(name, WF_STG_PARAM_NAME_LEN)
        p.u8(kind)
        if kind == PARAM_INT:
            p.i32(int(value))
            p.f32(0.0)
            p.u8(0)
            p.fixed("", WF_STG_PARAM_STR_LEN)
        elif kind == PARAM_FLOAT:
            p.i32(0)
            p.f32(float(value))
            p.u8(0)
            p.fixed("", WF_STG_PARAM_STR_LEN)
        elif kind == PARAM_BOOL:
            p.i32(0)
            p.f32(0.0)
            p.u8(1 if value else 0)
            p.fixed("", WF_STG_PARAM_STR_LEN)
        else:
            p.i32(0)
            p.f32(0.0)
            p.u8(0)
            p.fixed(str(value), WF_STG_PARAM_STR_LEN)

    return bytes(p.b)


def build_step_bin(wf_idx, step_idx, step):
    """返回 (bytes, step_crc)"""
    payload = serialize_step_payload(wf_idx, step_idx, step)
    step_crc = crc32_bytes(payload)

    h = Buf()
    h.u32(WF_STG_STEP_MAGIC)
    h.u16(WF_STG_VERSION)
    h.u16(WF_STG_STEP_HEADER_SIZE)
    h.u32(len(payload))
    h.u32(step_crc)
    assert len(h.b) == WF_STG_STEP_HEADER_SIZE

    return bytes(h.b) + payload, step_crc


# ---------------- Meta ----------------
def build_meta_bin(entries):
    """
    entries: list[(valid, version, step_count, update_time, crc32,
                   id, name, enable, timeout_ms, txn_id, variant)]，长度 16
    """
    body = Buf()
    for e in entries:
        (valid, version, step_count, update_time, crc32,
         wid, name, enable, timeout_ms, txn_id, variant) = e
        body.u8(1 if valid else 0)
        body.u8(version)
        body.u16(step_count)
        body.u32(update_time)
        body.u32(crc32)
        body.fixed(wid, WF_STG_ID_MAX_LEN)
        body.fixed(name, WF_STG_NAME_MAX_LEN)
        body.u8(1 if enable else 0)
        body.u32(timeout_ms)
        body.u32(txn_id)
        body.u32(variant)

    expected = (WF_STG_META_HEADER_SIZE
                + 89 * WF_STG_MAX_COUNT)
    assert len(body.b) == expected - WF_STG_META_HEADER_SIZE, \
        "meta entry 尺寸与源码不一致: %d" % len(body.b)

    crc = crc32_bytes(bytes(body.b))

    head = Buf()
    head.u32(WF_STG_META_MAGIC)
    head.u16(WF_STG_META_VERSION)
    head.u16(WF_STG_MAX_COUNT)
    head.u32(crc)
    assert len(head.b) == WF_STG_META_HEADER_SIZE

    return bytes(head.b) + bytes(body.b)


# ---------------- 主流程 ----------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=os.path.join("data", "workflow.json"))
    ap.add_argument("--out", default=os.path.join("data", "workflow"))
    args = ap.parse_args()

    with open(args.src, "r", encoding="utf-8") as f:
        doc = json.load(f)

    workflows = doc.get("workflows")
    if not isinstance(workflows, list):
        print("[ERR] %s 缺少 workflows 数组" % args.src, file=sys.stderr)
        return 1
    if len(workflows) > WF_STG_MAX_COUNT:
        print("[ERR] workflows 数量 %d 超过上限 %d"
              % (len(workflows), WF_STG_MAX_COUNT), file=sys.stderr)
        return 1

    entries = [(0, WF_STG_META_VERSION, 0, 0, 0,
                "", "", 0, 0, 0, 0)] * 0  # placeholder
    entries = []
    for i in range(WF_STG_MAX_COUNT):
        entries.append([0, WF_STG_META_VERSION, 0, 0, 0, "", "", 0, 0, 0, 0])

    os.makedirs(args.out, exist_ok=True)

    for wf_idx, wf in enumerate(workflows):
        wid = wf.get("id", "")
        if not wid:
            print("[ERR] workflows[%d] 缺少 id" % wf_idx, file=sys.stderr)
            return 1
        name = wf.get("name") or wid
        steps = wf.get("steps") or []
        if len(steps) > WF_STG_MAX_STEP:
            print("[ERR] %s: steps %d 个，超过上限 %d"
                  % (wid, len(steps), WF_STG_MAX_STEP), file=sys.stderr)
            return 1
        if len(steps) == 0:
            print("[ERR] %s: step_count 为 0 的 Workflow 不写入 BIN"
                  % wid, file=sys.stderr)
            return 1

        wf_dir = os.path.join(args.out, "wf%02d" % wf_idx)
        os.makedirs(wf_dir, exist_ok=True)

        # 与 workflow_storage_save() 一致：
        #   crc = CRC32( 各 step payload CRC 依次以 u32 LE 追加 )
        crc = crc32_init()
        for step_idx, step in enumerate(steps):
            blob, step_crc = build_step_bin(wf_idx, step_idx, step)
            crc = crc32_update(crc, struct.pack("<I", step_crc))
            path = os.path.join(wf_dir, "step%02d.bin" % step_idx)
            with open(path, "wb") as f:
                f.write(blob)
            print("[GEN] %-34s %4d B  crc=0x%08X" % (path, len(blob), step_crc))
        crc = crc32_final(crc)

        entries[wf_idx] = [
            1,                      # valid
            WF_STG_META_VERSION,    # version
            len(steps),             # step_count
            0,                      # update_time（PC 端无时间语义，置 0）
            crc,                    # crc32
            wid,
            name,
            1 if wf.get("enable", True) else 0,
            int(wf.get("timeout_ms", 600000)),
            1,                      # txn_id（首次事务）
            int(wf.get("variant", 1)),
        ]

    meta = build_meta_bin(entries)
    meta_path = os.path.join(args.out, "meta.bin")
    with open(meta_path, "wb") as f:
        f.write(meta)
    print("[GEN] %-34s %4d B" % (meta_path, len(meta)))
    print("[OK ] %d 个 Workflow 已生成到 %s" % (len(workflows), args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
