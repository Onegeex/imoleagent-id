// Imole meter simulator + usage relayer
// Behaves like the ESP32 meter will: reads prepaid credit on-chain, switches a (virtual) load
// on/off, meters energy, signs EIP-712 usage reports with the DEVICE key, and submits them
// via a RELAYER wallet that only pays gas (it never holds user funds).
//
// Run:  node meter.js            (settings come from .env in this folder)
import { readFileSync, existsSync } from "node:fs";
import { homedir } from "node:os";
import { createInterface } from "node:readline";
import { ethers } from "ethers";

// ---------------------------------------------------------------- config
const env = loadEnv(new URL("./.env", import.meta.url));
const cfg = {
  rpc: env.RPC_URL || "https://bsc-dataseed.bnbchain.org",
  grid: env.GRID || "0x0e5D4530b91b6c18a7c9A7831Aa1c919d624150f",
  meterLabel: env.METER_LABEL || "imole:ife-001",
  deviceKey: env.DEVICE_KEY, // private key from `cast wallet new` (meter's signing key)
  relayerKey: env.RELAYER_KEY, // optional; otherwise the Foundry keystore below is used
  relayerKeystore: env.RELAYER_KEYSTORE || `${homedir()}/.foundry/keystores/deployer`,
  loadW: Number(env.LOAD_W || 60), // simulated load in watts (a 60 W bulb)
  speed: Number(env.SPEED || 1), // time acceleration for demos (60 = one hour per minute)
  pollS: Number(env.POLL_S || 10), // how often to read credit
  reportS: Number(env.REPORT_S || 120), // how often to settle usage on-chain
  minReportWh: Number(env.MIN_REPORT_WH || 1),
};
if (!cfg.deviceKey) die("DEVICE_KEY is missing in meter-sim/.env");

const ABI = [
  "function meters(bytes32) view returns (address operator,address device,address owner,address pendingOwner,uint128 tariff,uint128 escrow,uint64 creditWh,uint64 lastNonce,uint64 totalWhUsed,bool active)",
  "function creditWh(bytes32) view returns (uint64)",
  "function reportUsage(bytes32 meterId,uint64 wh,uint64 nonce,bytes signature)",
  "event UsageSettled(bytes32 indexed meterId,uint64 nonce,uint256 wh,uint256 paidToOperator,uint256 creditWhLeft)",
];

// ---------------------------------------------------------------- setup
const provider = new ethers.JsonRpcProvider(cfg.rpc);
const device = new ethers.Wallet(cfg.deviceKey);
const meterId = ethers.id(cfg.meterLabel); // keccak256(label)
const gridRead = new ethers.Contract(cfg.grid, ABI, provider);

const net = await provider.getNetwork();
const m0 = await gridRead.meters(meterId);
if (m0.operator === ethers.ZeroAddress) die(`Meter ${cfg.meterLabel} (${meterId}) is not registered on ${cfg.grid}`);
if (m0.device.toLowerCase() !== device.address.toLowerCase())
  die(`DEVICE_KEY is ${device.address} but the meter expects ${m0.device}. Use the right key or call setDevice.`);

const relayer = (await loadRelayer()).connect(provider);
const grid = gridRead.connect(relayer);
const domain = { name: "ImoleGrid", version: "1", chainId: net.chainId, verifyingContract: cfg.grid };
const types = { Usage: [{ name: "meterId", type: "bytes32" }, { name: "wh", type: "uint64" }, { name: "nonce", type: "uint64" }] };

log(`Meter ${cfg.meterLabel}  id ${meterId.slice(0, 10)}…  chain ${net.chainId}`);
log(`Device ${device.address}   Relayer ${relayer.address}`);
log(`Load ${cfg.loadW} W  x${cfg.speed} speed  · poll ${cfg.pollS}s · settle every ${cfg.reportS}s`);

// ---------------------------------------------------------------- loop
let relayOn = false;
let pendingWh = 0; // metered but not yet settled
let credit = Number(m0.creditWh);
let last = Date.now();
let lastReport = Date.now();
let settling = false;

async function tick() {
  const now = Date.now();
  const dtH = ((now - last) / 3.6e6) * cfg.speed;
  last = now;
  if (relayOn) pendingWh += cfg.loadW * dtH;

  try {
    credit = Number(await gridRead.creditWh(meterId));
  } catch (e) {
    log(`RPC read failed (${short(e)}); keeping relay ${relayOn ? "ON" : "OFF"} for now`);
  }
  if (pendingWh > credit) pendingWh = credit; // a real relay can't draw past its cut-off
  const usable = credit - pendingWh;
  const shouldBeOn = usable > 0;
  if (shouldBeOn !== relayOn) {
    relayOn = shouldBeOn;
    log(relayOn ? "⚡ RELAY ON  - credit available, power flowing" : "⏻ RELAY OFF - credit exhausted");
  }
  log(`credit ${credit} Wh · unsettled ${pendingWh.toFixed(2)} Wh · ${relayOn ? "ON" : "OFF"}`);

  const due = now - lastReport >= cfg.reportS * 1000 || (!relayOn && pendingWh >= cfg.minReportWh);
  if (due && pendingWh >= cfg.minReportWh && !settling) settle().catch((e) => log(`settle failed: ${short(e)}`));
}

async function settle() {
  settling = true;
  try {
    const wh = Math.floor(pendingWh);
    const m = await gridRead.meters(meterId);
    const nonce = Number(m.lastNonce) + 1;
    const sig = await device.signTypedData(domain, types, { meterId, wh, nonce });
    log(`→ settling ${wh} Wh (nonce ${nonce})…`);
    const tx = await grid.reportUsage(meterId, wh, nonce, sig);
    const rc = await tx.wait();
    pendingWh -= wh;
    lastReport = Date.now();
    const ev = rc.logs.map((l) => { try { return grid.interface.parseLog(l); } catch { return null; } }).find((x) => x?.name === "UsageSettled");
    const paid = ev ? ethers.formatUnits(ev.args.paidToOperator, 18) : "?";
    log(`✓ settled ${wh} Wh → operator paid ${paid} USDT · tx ${rc.hash}`);
  } finally {
    settling = false;
  }
}

await tick();
setInterval(tick, cfg.pollS * 1000);
process.on("SIGINT", async () => {
  log("Stopping - settling remaining usage first…");
  if (pendingWh >= 1 && !settling) await settle().catch((e) => log(`settle failed: ${short(e)}`));
  process.exit(0);
});

// ---------------------------------------------------------------- helpers
async function loadRelayer() {
  if (cfg.relayerKey) return new ethers.Wallet(cfg.relayerKey);
  if (!existsSync(cfg.relayerKeystore)) die(`No RELAYER_KEY and no keystore at ${cfg.relayerKeystore}`);
  const pw = env.RELAYER_PASSWORD ?? (await ask("Relayer keystore password (deployer): "));
  return ethers.Wallet.fromEncryptedJson(readFileSync(cfg.relayerKeystore, "utf8"), pw);
}
function ask(q) {
  return new Promise((res) => {
    const rl = createInterface({ input: process.stdin, output: process.stdout });
    rl.stdoutMuted = true;
    rl._writeToOutput = (s) => { if (!rl.stdoutMuted || s.includes(q)) process.stdout.write(s); };
    rl.question(q, (a) => { rl.close(); process.stdout.write("\n"); res(a); });
  });
}
function loadEnv(url) {
  const out = { ...process.env };
  try {
    for (const line of readFileSync(url, "utf8").split("\n")) {
      const m = line.match(/^\s*([A-Z0-9_]+)\s*=\s*(.*)\s*$/);
      if (m) out[m[1]] = m[2].replace(/^["']|["']$/g, "");
    }
  } catch {}
  return out;
}
function log(s) { console.log(`[${new Date().toLocaleTimeString()}] ${s}`); }
function short(e) { return (e?.shortMessage || e?.message || String(e)).slice(0, 160); }
function die(s) { console.error("✗ " + s); process.exit(1); }
