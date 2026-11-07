# -*- coding: utf-8 -*-
import json, os, shutil
from pathlib import Path

ROOT = Path(r"c:\Users\zjy11\Desktop\ecsp")
REV = "1db2b2e845db65848b67437cf08dea2e519340d2"
REPO = {
    "url": "https://github.com/zhengjy926/ecsp.git",
    "revision": REV,
    "provider": "github",
}
TS = "20261103-001037"
ARCHIFY = Path(r"C:\Users\zjy11\.agents\skills\archify\bin\archify.mjs")


def write(folder, name, obj):
    d = ROOT / ".archify" / folder
    d.mkdir(parents=True, exist_ok=True)
    p = d / name
    p.write_text(json.dumps(obj, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return p


# ---- 1 architecture: reuse prior successful candidate, retarget paths ----
prev = json.loads((ROOT / ".archify" / "_arch_prev.json").read_text(encoding="utf-8-sig"))
folder = f"architecture-ecsp-layers-{TS}"
prev["meta"]["title"] = "ECSP 分层与边界"
prev["meta"]["locale"] = "zh-CN"
prev["meta"]["output"] = f".archify/{folder}/ecsp-layers.html"
prev["meta"]["quality_profile"] = "showcase"
prev["meta"]["repository"] = REPO
write(folder, "candidate.json", prev)

# ---- 2 host session lifecycle ----
folder = f"lifecycle-host-session-{TS}"
sess = {
  "schema_version": 3,
  "diagram_type": "lifecycle",
  "meta": {
    "title": "主机会话轴状态机",
    "locale": "zh-CN",
    "output": f".archify/{folder}/host-session.html",
    "quality_profile": "showcase",
    "repository": REPO
  },
  "mainPath": ["disconnected", "handshaking", "handshaken", "normal"],
  "states": [
    {"id": "disconnected", "type": "start", "label": "未连接", "sublabel": "DISCONNECTED", "tag": "等待探测", "step": "01",
     "sources": [{"path": "include/ecsp/host.h", "line": 43, "end_line": 50, "label": "ecsp_session_t"},
                 {"path": "src/ecsp/host/link.c", "line": 213, "end_line": 217, "label": "init slot"}]},
    {"id": "handshaking", "type": "active", "label": "握手中", "sublabel": "发 0x01", "tag": "HANDSHAKING", "step": "02",
     "sources": [{"path": "src/ecsp/host/link.c", "line": 400, "end_line": 406, "label": "queue 0x01"}]},
    {"id": "handshaken", "type": "waiting", "label": "已握手", "sublabel": "发/等 0x02", "tag": "HANDSHAKEN", "step": "03",
     "sources": [{"path": "src/ecsp/host/link.c", "line": 665, "end_line": 672, "label": "0x01 OK"}]},
    {"id": "normal", "type": "success", "label": "正常通信", "sublabel": "可 submit", "tag": "NORMAL", "step": "04",
     "sources": [{"path": "src/ecsp/host/link.c", "line": 693, "end_line": 698, "label": "version match"},
                 {"path": "src/ecsp/host/link.c", "line": 260, "end_line": 264, "label": "submit gate"}]},
    {"id": "version_mismatch", "type": "failure", "label": "版本不符", "sublabel": "0x02 字节不等", "tag": "终端",
     "sources": [{"path": "src/ecsp/host/link.c", "line": 693, "end_line": 698, "label": "mismatch"}]},
    {"id": "offline", "type": "failure", "label": "掉线", "sublabel": "静默/建立超时", "tag": "OFFLINE",
     "sources": [{"path": "src/ecsp/host/link.c", "line": 450, "end_line": 456, "label": "txn silence"},
                 {"path": "src/ecsp/host/link.c", "line": 501, "end_line": 505, "label": "heartbeat drop"}]}
  ],
  "transitions": [
    {"id": "t1", "from": "disconnected", "to": "handshaking", "label": "首拍探测 0x01"},
    {"id": "t2", "from": "handshaking", "to": "handshaken", "label": "0x01 应答 0x00"},
    {"id": "t3", "from": "handshaken", "to": "normal", "label": "0x02 版本相等"},
    {"id": "t4", "from": "handshaken", "to": "version_mismatch", "label": "版本不等", "variant": "security"},
    {"id": "t5", "from": "handshaking", "to": "disconnected", "label": "三次无应答", "variant": "dashed"},
    {"id": "t6", "from": "handshaking", "to": "offline", "label": "建立总时限", "variant": "security"},
    {"id": "t7", "from": "handshaken", "to": "disconnected", "label": "0x02 三次无应答", "variant": "dashed"},
    {"id": "t8", "from": "normal", "to": "offline", "label": "业务静默耗尽", "variant": "security"},
    {"id": "t9", "from": "normal", "to": "disconnected", "label": "对端 0x01 上报重启", "variant": "dashed", "note": "先结束在途事务"},
    {"id": "t10", "from": "offline", "to": "handshaking", "label": "收到 0x01 上报再探测"},
    {"id": "t11", "from": "handshaking", "to": "disconnected", "label": "非 0x00 拒绝", "variant": "security"}
  ],
  "cards": [
    {"dot": "cyan", "title": "双轴之一", "items": ["会话轴与事务轴互不折叠（ADR-0003）", "仅 NORMAL 才允许业务 submit"]},
    {"dot": "rose", "title": "失败出口", "items": ["版本不符停在 VERSION_MISMATCH", "静默耗尽进 OFFLINE，需对端上报再探测"]}
  ]
}
write(folder, "candidate.json", sess)

# ---- 3 host txn lifecycle ----
folder = f"lifecycle-host-txn-{TS}"
txn = {
  "schema_version": 3,
  "diagram_type": "lifecycle",
  "meta": {
    "title": "主机事务轴状态机",
    "locale": "zh-CN",
    "output": f".archify/{folder}/host-txn.html",
    "quality_profile": "showcase",
    "repository": REPO
  },
  "mainPath": ["idle", "queued", "wait_resp", "idle_done"],
  "states": [
    {"id": "idle", "type": "start", "label": "空闲", "sublabel": "TXN_IDLE", "step": "01",
     "sources": [{"path": "include/ecsp/host.h", "line": 67, "end_line": 72, "label": "ecsp_txn_state_t"}]},
    {"id": "queued", "type": "waiting", "label": "待发送", "sublabel": "submit 入队", "step": "02",
     "sources": [{"path": "src/ecsp/host/link.c", "line": 283, "end_line": 295, "label": "QUEUED"}]},
    {"id": "wait_resp", "type": "active", "label": "等待应答", "sublabel": "含初发最多 3 次", "step": "03",
     "sources": [{"path": "src/ecsp/host/link.c", "line": 462, "end_line": 483, "label": "emit_queued"},
                 {"path": "src/ecsp/host/link.c", "line": 438, "end_line": 459, "label": "progress_txn"}]},
    {"id": "wait_result", "type": "waiting", "label": "等待结果", "sublabel": "长耗时受理后", "tag": "0x08/0x0C",
     "sources": [{"path": "src/ecsp/host/link.c", "line": 776, "end_line": 783, "label": "accept+T_exec_max"}]},
    {"id": "idle_done", "type": "success", "label": "回空闲", "sublabel": "回调恰一次", "step": "04",
     "sources": [{"path": "include/ecsp/host.h", "line": 77, "end_line": 94, "label": "txn callback"}]}
  ],
  "transitions": [
    {"id": "a1", "from": "idle", "to": "queued", "label": "submit（须 NORMAL）"},
    {"id": "a2", "from": "queued", "to": "wait_resp", "label": "step 写入 FIFO"},
    {"id": "a3", "from": "wait_resp", "to": "idle_done", "label": "普通应答到达"},
    {"id": "a4", "from": "wait_resp", "to": "wait_result", "label": "0x00 + T_exec_max", "variant": "emphasis"},
    {"id": "a5", "from": "wait_result", "to": "idle_done", "label": "结果事件/获取结果"},
    {"id": "a6", "from": "wait_resp", "to": "idle_done", "label": "重发耗尽 ECSP_E_TIMEOUT", "variant": "security"},
    {"id": "a7", "from": "wait_resp", "to": "idle_done", "label": "掉线 ECSP_E_OFFLINE", "variant": "dashed"},
    {"id": "a8", "from": "wait_result", "to": "queued", "label": "在途 0x07 抢占", "variant": "security", "note": "长耗时回调仍挂起直至复位应答"},
    {"id": "a9", "from": "wait_result", "to": "wait_resp", "label": "超时后发获取结果", "variant": "dashed"},
    {"id": "a10", "from": "idle_done", "to": "idle", "label": "同轴复位"}
  ],
  "cards": [
    {"dot": "emerald", "title": "单从机单事务", "items": ["第二笔 submit 返回 ECSP_E_CAP", "回调可再 submit，但不得重入本 host_step"]},
    {"dot": "amber", "title": "长耗时", "items": ["受理不结束事务，进入 WAIT_RESULT", "丢失结果事件后用目录三元组的获取结果兜底"]}
  ]
}
write(folder, "candidate.json", txn)

# ---- 4 sequence handshake + business ----
folder = f"sequence-handshake-business-{TS}"
seq = {
  "schema_version": 1,
  "diagram_type": "sequence",
  "meta": {
    "title": "握手、版本核对与业务往返",
    "locale": "zh-CN",
    "output": f".archify/{folder}/handshake-business.html",
    "quality_profile": "showcase",
    "repository": REPO
  },
  "participants": [
    {"id": "app", "type": "external", "label": "主机应用", "sublabel": "submit / step",
     "sources": [{"path": "include/ecsp/host.h", "line": 170, "end_line": 176, "label": "host API"}]},
    {"id": "host", "type": "backend", "label": "L4 Host", "sublabel": "link.c",
     "sources": [{"path": "src/ecsp/host/link.c", "line": 823, "end_line": 863, "label": "host_step"}]},
    {"id": "wire", "type": "messagebus", "label": "字节链路", "sublabel": "FIFO / USART",
     "sources": [{"path": "test/integ/two_instance.c", "line": 302, "end_line": 320, "label": "deliver_host"}]},
    {"id": "slave", "type": "backend", "label": "L3 Slave", "sublabel": "engine.c",
     "sources": [{"path": "src/ecsp/slave/engine.c", "line": 506, "end_line": 535, "label": "slave_step"}]}
  ],
  "segments": [
    {"from": 140, "to": 300, "label": "上电建立"},
    {"from": 310, "to": 430, "label": "业务命令"},
    {"from": 440, "to": 520, "label": "心跳"}
  ],
  "messages": [
    {"id": "m1", "from": "app", "to": "host", "y": 160, "label": "host_step", "variant": "emphasis",
     "note": "首次 step 对每槽主动发 0x01"},
    {"id": "m2", "from": "host", "to": "wire", "y": 210, "label": "0x01 握手请求"},
    {"id": "m3", "from": "wire", "to": "slave", "y": 238, "label": "slave_rx"},
    {"id": "m4", "from": "app", "to": "slave", "y": 266, "label": "slave_step", "variant": "dashed",
     "note": "示意：产品/测试推进从机"},
    {"id": "m5", "from": "slave", "to": "wire", "y": 320, "label": "0x01 / 0x00"},
    {"id": "m6", "from": "wire", "to": "host", "y": 348, "label": "host_rx → HANDSHAKEN"},
    {"id": "m7", "from": "host", "to": "wire", "y": 376, "label": "0x02 取版本", "variant": "emphasis"},
    {"id": "m8", "from": "wire", "to": "slave", "y": 404, "label": "slave_rx"},
    {"id": "m9", "from": "slave", "to": "wire", "y": 432, "label": "0x02 + 版本"},
    {"id": "m10", "from": "wire", "to": "host", "y": 460, "label": "核对 → NORMAL"},
    {"id": "m11", "from": "app", "to": "host", "y": 500, "label": "submit 业务 cmd", "variant": "emphasis"},
    {"id": "m12", "from": "host", "to": "wire", "y": 528, "label": "下一拍发出"},
    {"id": "m13", "from": "wire", "to": "slave", "y": 556, "label": "管线处理"},
    {"id": "m14", "from": "slave", "to": "wire", "y": 584, "label": "应答帧"},
    {"id": "m15", "from": "wire", "to": "host", "y": 612, "label": "txn 回调一次", "variant": "return"},
    {"id": "m16", "from": "slave", "to": "wire", "y": 650, "label": "0x10 心跳", "variant": "dashed"},
    {"id": "m17", "from": "wire", "to": "host", "y": 678, "label": "刷新 last_rx", "variant": "dashed"}
  ],
  "activations": [
    {"participant": "host", "from": 160, "to": 630, "type": "backend"},
    {"participant": "slave", "from": 238, "to": 690, "type": "backend"},
    {"participant": "wire", "from": 210, "to": 690, "type": "messagebus"}
  ],
  "cards": [
    {"dot": "cyan", "title": "建立顺序", "items": ["0x01 成功后同一拍进入发 0x02", "版本 7 字节原始比较，相符才 NORMAL"]},
    {"dot": "emerald", "title": "业务闸门", "items": ["非 NORMAL 的 submit 本地拒绝", "应答到达立即结束事务并回调"]}
  ]
}
# Fix note spacing - m1 note needs next at 160+44=204, m2 at 210 OK; m4 note needs 266+44=310, m5 at 320 OK
write(folder, "candidate.json", seq)

# ---- 5 workflow slave pipeline ----
folder = f"workflow-slave-pipeline-{TS}"
wf = {
  "schema_version": 2,
  "diagram_type": "workflow",
  "meta": {
    "title": "从机决策管线",
    "locale": "zh-CN",
    "output": f".archify/{folder}/slave-pipeline.html",
    "quality_profile": "showcase",
    "repository": REPO
  },
  "lanes": [
    {"id": "frame", "label": "成帧"},
    {"id": "gate", "label": "准入闸门"},
    {"id": "dedupe", "label": "去重 / 忙"},
    {"id": "exec", "label": "校验与执行"}
  ],
  "phases": [
    {"id": "p1", "label": "收帧", "fromCol": 0, "toCol": 1},
    {"id": "p2", "label": "策略闸", "fromCol": 2, "toCol": 4, "variant": "emphasis"},
    {"id": "p3", "label": "执行", "fromCol": 5, "toCol": 7}
  ],
  "groups": [
    {"id": "silent", "label": "静默丢弃", "lane": "frame", "fromCol": 0, "toCol": 1, "variant": "dashed"},
    {"id": "reject", "label": "拒绝应答", "lane": "gate", "fromCol": 2, "toCol": 4, "variant": "security"}
  ],
  "mainPath": ["scan", "addr", "cmd", "session", "len", "cache", "busy", "validate", "handler"],
  "nodes": [
    {"id": "scan", "lane": "frame", "col": 0, "type": "backend", "label": "1 扫帧/CRC", "sublabel": "坏帧静默",
     "sources": [{"path": "src/ecsp/slave/engine.c", "line": 534, "end_line": 546, "label": "step1"}]},
    {"id": "drop", "lane": "frame", "col": 1, "type": "security", "label": "丢弃", "sublabel": "SKIPPED"},
    {"id": "addr", "lane": "gate", "col": 1, "type": "security", "label": "2 地址", "sublabel": "不匹配静默",
     "sources": [{"path": "src/ecsp/slave/engine.c", "line": 548, "end_line": 554, "label": "step2"}]},
    {"id": "cmd", "lane": "gate", "col": 2, "type": "decision", "label": "3 命令", "sublabel": "目录或板卡表",
     "sources": [{"path": "src/ecsp/slave/engine.c", "line": 556, "end_line": 579, "label": "step3"}]},
    {"id": "rs04", "lane": "gate", "col": 3, "type": "security", "label": "0x04", "sublabel": "未定义"},
    {"id": "session", "lane": "gate", "col": 3, "type": "decision", "label": "4 会话", "sublabel": "非 0x01 需就绪",
     "sources": [{"path": "src/ecsp/slave/engine.c", "line": 582, "end_line": 590, "label": "step4"}]},
    {"id": "rs10", "lane": "gate", "col": 4, "type": "security", "label": "0x10", "sublabel": "未握手"},
    {"id": "len", "lane": "dedupe", "col": 4, "type": "decision", "label": "5 长度", "sublabel": "req_len",
     "sources": [{"path": "src/ecsp/slave/engine.c", "line": 592, "end_line": 604, "label": "step5"}]},
    {"id": "rs02", "lane": "dedupe", "col": 5, "type": "security", "label": "0x02", "sublabel": "长度错"},
    {"id": "cache", "lane": "dedupe", "col": 5, "type": "database", "label": "6 事务缓存", "sublabel": "(cmd,seq)",
     "sources": [{"path": "src/ecsp/slave/engine.c", "line": 606, "end_line": 631, "label": "step6"}]},
    {"id": "busy", "lane": "dedupe", "col": 6, "type": "decision", "label": "7 长耗时忙", "sublabel": "放行标志",
     "sources": [{"path": "src/ecsp/slave/engine.c", "line": 634, "end_line": 649, "label": "step7"}]},
    {"id": "rs11", "lane": "dedupe", "col": 7, "type": "security", "label": "0x11", "sublabel": "忙"},
    {"id": "validate", "lane": "exec", "col": 6, "type": "decision", "label": "8 参数", "sublabel": "validate",
     "sources": [{"path": "src/ecsp/slave/engine.c", "line": 652, "end_line": 669, "label": "step8"}]},
    {"id": "rs03", "lane": "exec", "col": 7, "type": "security", "label": "0x03", "sublabel": "参数错"},
    {"id": "handler", "lane": "exec", "col": 7, "type": "backend", "label": "9 handler", "sublabel": "返回码即应答",
     "sources": [{"path": "src/ecsp/slave/engine.c", "line": 672, "end_line": 698, "label": "step9"}]}
  ],
  "edges": [
    {"id": "e1", "from": "scan", "to": "addr", "variant": "emphasis"},
    {"id": "e2", "from": "scan", "to": "drop", "label": "坏帧", "variant": "dashed", "role": "error"},
    {"id": "e3", "from": "addr", "to": "cmd", "variant": "emphasis"},
    {"id": "e4", "from": "cmd", "to": "session", "variant": "emphasis"},
    {"id": "e5", "from": "cmd", "to": "rs04", "label": "无入口", "variant": "security", "role": "error"},
    {"id": "e6", "from": "session", "to": "len", "variant": "emphasis"},
    {"id": "e7", "from": "session", "to": "rs10", "label": "未就绪", "variant": "security", "role": "error"},
    {"id": "e8", "from": "len", "to": "cache", "variant": "emphasis"},
    {"id": "e9", "from": "len", "to": "rs02", "label": "长度", "variant": "security", "role": "error"},
    {"id": "e10", "from": "cache", "to": "busy", "label": "未命中", "variant": "emphasis"},
    {"id": "e11", "from": "busy", "to": "validate", "variant": "emphasis"},
    {"id": "e12", "from": "busy", "to": "rs11", "label": "拒绝", "variant": "security", "role": "error"},
    {"id": "e13", "from": "validate", "to": "handler", "variant": "emphasis"},
    {"id": "e14", "from": "validate", "to": "rs03", "label": "非法值", "variant": "security", "role": "error"}
  ],
  "cards": [
    {"dot": "rose", "title": "顺序固定", "items": ["ADR-0002：九步顺序不可重排", "成帧失败与地址错均静默，不产生应答"]},
    {"dot": "amber", "title": "缓存优先于忙", "items": ["命中 (cmd,seq) 整帧回放，不进 handler", "忙门只挡无放行标志的命令"]}
  ]
}
write(folder, "candidate.json", wf)

# ---- 6 tree modules ----
folder = f"tree-source-modules-{TS}"
tree = {
  "schema_version": 1,
  "diagram_type": "tree",
  "meta": {
    "title": "ECSP 源码模块树",
    "locale": "zh-CN",
    "output": f".archify/{folder}/source-modules.html",
    "quality_profile": "showcase",
    "repository": REPO
  },
  "layout": {"direction": "right"},
  "nodes": [
    {"id": "root", "label": "ecsp", "sublabel": "协议参考实现",
     "sources": [{"path": "docs/specs/ecsp-impl-spec.md", "line": 31, "end_line": 62, "label": "layout"}]},
    {"id": "include", "label": "include/ecsp", "parent": "root", "sublabel": "公共头"},
    {"id": "src", "label": "src/ecsp", "parent": "root", "sublabel": "库实现"},
    {"id": "board", "label": "board/f429", "parent": "root", "sublabel": "板卡注入",
     "sources": [{"path": "board/f429/bringup.h", "line": 4, "end_line": 12, "label": "board cmds"}]},
    {"id": "test", "label": "test", "parent": "root", "sublabel": "单测/互操作"},
    {"id": "h_frame", "label": "frame/bytes/crc", "parent": "include", "sublabel": "L1"},
    {"id": "h_cmds", "label": "cmds.h", "parent": "include", "sublabel": "L2"},
    {"id": "h_slave", "label": "slave.h", "parent": "include", "sublabel": "L3"},
    {"id": "h_host", "label": "host.h", "parent": "include", "sublabel": "L4"},
    {"id": "core", "label": "core/", "parent": "src", "sublabel": "L1+L2",
     "sources": [{"path": "src/ecsp/core/frame.c", "line": 7, "end_line": 20, "label": "frame.c"}]},
    {"id": "slave_dir", "label": "slave/", "parent": "src", "sublabel": "引擎/会话/缓存",
     "sources": [{"path": "src/ecsp/slave/engine.c", "line": 506, "end_line": 520, "label": "engine"}]},
    {"id": "host_dir", "label": "host/", "parent": "src", "sublabel": "link.c",
     "sources": [{"path": "src/ecsp/host/link.c", "line": 167, "end_line": 180, "label": "host_init"}]},
    {"id": "bringup", "label": "bringup 0x30-33", "parent": "board"},
    {"id": "fw", "label": "fw/ 从机固件", "parent": "board",
     "sources": [{"path": "board/f429/fw/main.c", "line": 69, "end_line": 95, "label": "main"}]},
    {"id": "unit", "label": "unit/", "parent": "test"},
    {"id": "integ", "label": "integ/two_instance", "parent": "test",
     "sources": [{"path": "test/integ/two_instance.c", "line": 1, "end_line": 18, "label": "virtual link"}]},
    {"id": "vectors", "label": "vectors/", "parent": "test"}
  ]
}
write(folder, "candidate.json", tree)

# ---- 7 class core types ----
folder = f"class-core-types-{TS}"
cls = {
  "schema_version": 1,
  "diagram_type": "class",
  "meta": {
    "title": "ECSP 核心类型",
    "locale": "zh-CN",
    "output": f".archify/{folder}/core-types.html",
    "quality_profile": "showcase",
    "repository": REPO
  },
  "layout": {"mode": "grid"},
  "types": [
    {"id": "frame", "label": "ecsp_frame_t", "kind": "record", "sublabel": "零拷贝帧视图", "row": 0, "col": 1,
     "attributes": [
       {"name": "product_addr", "type": "uint8_t", "visibility": "public"},
       {"name": "board_addr", "type": "uint8_t", "visibility": "public"},
       {"name": "cmd", "type": "uint8_t", "visibility": "public"},
       {"name": "payload", "type": "const uint8_t*", "visibility": "public"},
       {"name": "payload_len", "type": "uint16_t", "visibility": "public"}
     ],
     "sources": [{"path": "include/ecsp/frame.h", "line": 14, "end_line": 20, "label": "frame_t"}]},
    {"id": "cmd_table", "label": "ecsp_cmd_table_t", "kind": "record", "sublabel": "板卡命令表", "row": 0, "col": 2,
     "sources": [{"path": "include/ecsp/cmds.h", "line": 1, "end_line": 24, "label": "catalog docs"}]},
    {"id": "slave_cfg", "label": "ecsp_slave_cfg_t", "kind": "record", "sublabel": "调用方配置", "row": 1, "col": 0,
     "attributes": [
       {"name": "rx_buf/tx_buf", "type": "uint8_t*", "visibility": "public"},
       {"name": "identity", "type": "ecsp_slave_identity_t*", "visibility": "public"},
       {"name": "cmds", "type": "ecsp_cmd_table_t", "visibility": "public"}
     ],
     "sources": [{"path": "include/ecsp/slave.h", "line": 104, "end_line": 116, "label": "cfg"}]},
    {"id": "slave", "label": "ecsp_slave_t", "kind": "class", "sublabel": "L3 上下文", "row": 1, "col": 1,
     "attributes": [
       {"name": "ready", "type": "uint8_t", "visibility": "private"},
       {"name": "txn_cache", "type": "entry[]", "visibility": "private"},
       {"name": "lr_cmd", "type": "uint8_t", "visibility": "private"}
     ],
     "methods": [
       {"name": "ecsp_slave_init", "visibility": "public"},
       {"name": "ecsp_slave_rx", "visibility": "public"},
       {"name": "ecsp_slave_step", "visibility": "public"},
       {"name": "ecsp_slave_tx_drain", "visibility": "public"}
     ],
     "sources": [{"path": "include/ecsp/slave.h", "line": 119, "end_line": 157, "label": "struct"},
                 {"path": "include/ecsp/slave.h", "line": 167, "end_line": 183, "label": "API"}]},
    {"id": "host_cfg", "label": "ecsp_host_cfg_t", "kind": "record", "sublabel": "多从机槽", "row": 2, "col": 0,
     "attributes": [
       {"name": "slaves", "type": "ecsp_host_slave_cfg_t*", "visibility": "public"},
       {"name": "board_cmds", "type": "ecsp_cmd_table_t", "visibility": "public"}
     ],
     "sources": [{"path": "include/ecsp/host.h", "line": 111, "end_line": 122, "label": "cfg"}]},
    {"id": "host", "label": "ecsp_host_t", "kind": "class", "sublabel": "L4 上下文", "row": 2, "col": 1,
     "attributes": [
       {"name": "slots", "type": "ecsp_host_slot_t[]", "visibility": "private"},
       {"name": "in_step", "type": "uint8_t", "visibility": "private"}
     ],
     "methods": [
       {"name": "ecsp_host_init", "visibility": "public"},
       {"name": "ecsp_host_submit", "visibility": "public"},
       {"name": "ecsp_host_step", "visibility": "public"},
       {"name": "ecsp_host_session", "visibility": "public"}
     ],
     "sources": [{"path": "include/ecsp/host.h", "line": 159, "end_line": 186, "label": "host API"}]},
    {"id": "slot", "label": "ecsp_host_slot_t", "kind": "record", "sublabel": "会话+事务轴", "row": 2, "col": 2,
     "attributes": [
       {"name": "session", "type": "ecsp_session_t", "visibility": "private"},
       {"name": "txn_state", "type": "ecsp_txn_state_t", "visibility": "private"}
     ],
     "sources": [{"path": "include/ecsp/host.h", "line": 126, "end_line": 157, "label": "slot"}]}
  ],
  "relationships": [
    {"id": "r1", "from": "slave", "to": "slave_cfg", "kind": "composition", "label": "持有 cfg"},
    {"id": "r2", "from": "slave_cfg", "to": "cmd_table", "kind": "association", "label": "板卡表"},
    {"id": "r3", "from": "slave", "to": "frame", "kind": "dependency", "label": "扫帧产出"},
    {"id": "r4", "from": "host", "to": "host_cfg", "kind": "composition", "label": "持有 cfg"},
    {"id": "r5", "from": "host", "to": "slot", "kind": "composition", "label": "每从机一槽"},
    {"id": "r6", "from": "host", "to": "frame", "kind": "dependency", "label": "编解码"},
    {"id": "r7", "from": "host_cfg", "to": "cmd_table", "kind": "association", "label": "应答时限"}
  ]
}
write(folder, "candidate.json", cls)

manifest = [
  ("architecture", f"architecture-ecsp-layers-{TS}", "ecsp-layers.html"),
  ("lifecycle", f"lifecycle-host-session-{TS}", "host-session.html"),
  ("lifecycle", f"lifecycle-host-txn-{TS}", "host-txn.html"),
  ("sequence", f"sequence-handshake-business-{TS}", "handshake-business.html"),
  ("workflow", f"workflow-slave-pipeline-{TS}", "slave-pipeline.html"),
  ("tree", f"tree-source-modules-{TS}", "source-modules.html"),
  ("class", f"class-core-types-{TS}", "core-types.html"),
]
(ROOT / ".archify" / f"_manifest-{TS}.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
print("wrote", len(manifest), "candidates")
for t, f, h in manifest:
    print(t, f)
