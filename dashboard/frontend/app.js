"use strict";
// BMS dashboard frontend: subscribe to /ws, render the state. Plain JS + Chart.js.

const $ = (id) => document.getElementById(id);
const STATES = ["OPEN", "PRECHARGE", "CLOSED", "FAULT"];
const CAP_KIND = { info: "BMS", warn: "WARNING", bad: "FAULT", ok: "OK", ota: "UPDATE", title: "DEMO" };

// ---- cell bars: built once, updated in place ------------------------------
const cellsEl = $("cells");
const cellNodes = [];
for (let i = 0; i < 12; i++) {
  const c = document.createElement("div");
  c.className = "cell";
  c.innerHTML = `<div class="bar" style="height:40%"></div><div class="mv">--</div><div class="tc">--</div><div class="n">${i + 1}</div>`;
  cellsEl.appendChild(c);
  cellNodes.push({ root: c, bar: c.querySelector(".bar"), mv: c.querySelector(".mv"), tc: c.querySelector(".tc") });
}

// colour a cell by how close it is to its nearest limit (0 = safe, 1 = at limit)
function hazardColor(frac) {
  if (frac < 0.5) return getComputedStyle(document.documentElement).getPropertyValue("--ok");
  if (frac < 0.85) return getComputedStyle(document.documentElement).getPropertyValue("--warn");
  return getComputedStyle(document.documentElement).getPropertyValue("--bad");
}

// ---- SoC doughnut ----------------------------------------------------------
const socChart = new Chart($("socChart"), {
  type: "doughnut",
  data: { datasets: [{ data: [80, 20], backgroundColor: ["#3fb950", "#1f2937"], borderWidth: 0 }] },
  options: { cutout: "76%", rotation: -90, circumference: 360, responsive: true,
    maintainAspectRatio: false, animation: { duration: 250 }, plugins: { legend: { display: false }, tooltip: { enabled: false } } },
});

// ---- pack current trend ----------------------------------------------------
const MAXPTS = 160;
const currentChart = new Chart($("currentChart"), {
  type: "line",
  data: { labels: [], datasets: [{ data: [], borderColor: "#4da3ff", backgroundColor: "rgba(77,163,255,.12)",
    borderWidth: 3, fill: true, tension: 0.25, pointRadius: 0 }] },
  options: {
    responsive: true, maintainAspectRatio: false, animation: false,
    scales: {
      x: { display: false },
      y: { grid: { color: "#1c2736" }, ticks: { color: "#8b98a9", font: { size: 15 } }, suggestedMin: 0, suggestedMax: 60 },
    },
    plugins: { legend: { display: false }, tooltip: { enabled: false } },
  },
});
let lastCurrentT = -1;

// ---- render ----------------------------------------------------------------
function render(s) {
  // state badge + SM highlight
  $("state").textContent = s.state;
  const col = { OPEN: "var(--dim)", PRECHARGE: "var(--precharge)", CLOSED: "var(--ok)", FAULT: "var(--bad)" }[s.state] || "var(--ink)";
  $("state").style.color = col;
  $("state").style.borderColor = col;
  for (const st of STATES) {
    const n = $("n-" + st);
    if (!n) continue;
    n.classList.toggle("active", st === s.state);
    n.style.stroke = st === s.state ? col : "var(--line)";
  }

  // cells
  const lim = s.limits || { ov_mv: 4250, uv_mv: 2800, ot_dc: 600 };
  for (let i = 0; i < 12; i++) {
    const mv = (s.cells_mv || [])[i] || 0;
    const tc = (s.cells_temp || [])[i] || 0;
    const node = cellNodes[i];
    // bar height: map 2500..4300 mV to 8..100%
    const h = Math.max(8, Math.min(100, ((mv - 2500) / (4300 - 2500)) * 100));
    node.bar.style.height = h.toFixed(0) + "%";
    // hazard = worst of voltage-high, voltage-low, temperature-high; 0 across
    // the healthy band (green), approaching 1 near each limit (amber -> red)
    const fOV = (mv - 4050) / (lim.ov_mv - 4050);
    const fUV = (3250 - mv) / (3250 - lim.uv_mv);
    const fOT = (tc - 45) / (lim.ot_dc / 10 - 45);
    const frac = Math.max(fOV, fUV, fOT, 0);
    node.bar.style.background = hazardColor(frac);
    node.mv.textContent = mv ? (mv / 1000).toFixed(2) + " V" : "--";
    node.tc.textContent = tc ? tc.toFixed(0) + " °C" : "--";
    node.root.classList.toggle("hot", fOT > 0.85);
  }

  // SoC + pack facts
  const soc = s.soc_pct || 0;
  $("soc").textContent = soc.toFixed(0) + "%";
  socChart.data.datasets[0].data = [soc, 100 - soc];
  socChart.data.datasets[0].backgroundColor = [soc > 25 ? "#3fb950" : "#d29922", "#1f2937"];
  socChart.update();
  $("packv").textContent = s.pack_mv ? (s.pack_mv / 1000).toFixed(1) : "--";
  $("packi").textContent = (s.pack_current_a != null) ? s.pack_current_a.toFixed(0) : "--";

  // current trend
  if (s.t !== lastCurrentT) {
    lastCurrentT = s.t;
    const d = currentChart.data;
    d.labels.push("");
    d.datasets[0].data.push(s.pack_current_a || 0);
    if (d.labels.length > MAXPTS) { d.labels.shift(); d.datasets[0].data.shift(); }
    currentChart.update();
  }

  // OTA / slots
  const ota = s.ota || {};
  $("otanote").textContent = ota.note || "";
  for (const slot of ["A", "B"]) {
    const isActive = ota.active_slot === slot;
    $("slot-" + slot).classList.toggle("active", isActive);
    const ver = (isActive ? ota.version : (ota.pending_slot === slot ? ota.pending : null));
    $("slot" + slot + "-ver").textContent = ver ? "v" + ver : "—";
    let tag = "";
    if (isActive) tag = ota.status === "trial" ? "running · on trial" : "running · " + (ota.status || "");
    else if (ota.pending_slot === slot) tag = "staged (pending)";
    else tag = "idle";
    $("slot" + slot + "-tag").textContent = tag;
    $("slot" + slot + "-tag").style.color = (isActive && ota.status === "trial") ? "var(--warn)" : "var(--dim)";
  }
  const prog = ota.progress;
  $("prog").style.display = (prog != null) ? "block" : "none";
  $("proglabel").style.display = (prog != null) ? "block" : "none";
  if (prog != null) {
    $("progfill").style.width = (prog * 100).toFixed(1) + "%";
    $("proglabel").textContent = "transfer " + (prog * 100).toFixed(0) + "%";
  }

  // fault log
  const fl = $("faultlog");
  const log = s.fault_log || [];
  if (!log.length) { fl.innerHTML = '<div class="empty">no faults</div>'; }
  else {
    fl.innerHTML = log.slice(-7).map((e) =>
      `<div class="row ${e.kind || "bad"}"><span class="ts">${e.t}</span><span>${e.text}</span></div>`).join("");
  }

  // uart terminal
  const term = $("term");
  const lines = (s.uart || []).slice(-7);
  term.innerHTML = lines.map((l) => {
    const safe = l.replace(/[&<>]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;" }[c]));
    return `<div class="ln">${safe.replace(/^(BOOT|\[[^\]]+\]\s+\w+)/, '<span class="b">$1</span>')}</div>`;
  }).join("");

  // caption
  const cap = s.caption || { text: "", kind: "info" };
  const el = $("caption");
  el.className = "caption " + (cap.kind || "info");
  $("cap-chip").textContent = CAP_KIND[cap.kind] || "BMS";
  $("cap-text").innerHTML = cap.text || "";
}

// ---- websocket with auto-reconnect ----------------------------------------
function connect() {
  const ws = new WebSocket("ws://" + location.host + "/ws");
  ws.onmessage = (ev) => { try { render(JSON.parse(ev.data)); } catch (e) { /* ignore */ } };
  ws.onclose = () => setTimeout(connect, 500);
}
connect();
