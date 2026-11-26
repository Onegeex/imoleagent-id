// One-shot meter tick for scheduled runs (GitHub Actions).
// Simulates a constant load on the pilot meter since its last on-chain settlement, signs a
// usage report with the DEVICE key and submits it with the RELAYER key (gas only).
// Stateless: "last settlement time" is read from the chain each run.
//
// env: DEVICE_KEY, RELAYER_KEY (required) · GRID, METER_LABEL, LOAD_W, RPC_URL, MAX_GAP_MIN (optional)
import { ethers } from "ethers";

const GRID = process.env.GRID || "0xcc3A2b650a904a64AfDcD6a3319056AAC8BBcc99";
const LABEL = process.env.METER_LABEL || "imole:ife-001";
const LOAD_W = Number(process.env.LOAD_W || 60);
const MAX_GAP_MIN = Number(process.env.MAX_GAP_MIN || 60); // never bill more than this window in one run
const RPCS = (process.env.RPC_URL || "https://bsc-rpc.publicnode.com,https://bsc-dataseed.bnbchain.org").split(",");

const ABI = [
  "function meters(bytes32) view returns (address operator,address device,address owner,address pendingOwner,uint128 tariff,uint128 escrow,uint64 creditWh,uint64 lastNonce,uint64 totalWhUsed,bool active)",
  "function reportUsage(bytes32 meterId,uint64 wh,uint64 nonce,bytes signature)",
  "event UsageSettled(bytes32 indexed meterId,uint64 nonce,uint256 wh,uint256 paidToOperator,uint256 creditWhLeft)",
  "event Credited(bytes32 indexed meterId,address indexed payer,string handle,uint256 usdtAmount,uint256 wh,bool sponsored)",
];

if (!process.env.DEVICE_KEY || !process.env.RELAYER_KEY) throw new Error("DEVICE_KEY and RELAYER_KEY are required");
const provider = await firstWorking(RPCS);
const device = new ethers.Wallet(process.env.DEVICE_KEY);
const relayer = new ethers.Wallet(process.env.RELAYER_KEY, provider);
const grid = new ethers.Contract(GRID, ABI, relayer);
const meterId = ethers.id(LABEL);

const m = await grid.meters(meterId);
if (m.device.toLowerCase() !== device.address.toLowerCase()) throw new Error(`DEVICE_KEY (${device.address}) is not this meter's device (${m.device})`);
console.log(`meter ${LABEL} credit ${m.creditWh} Wh · relayer ${relayer.address} · ${ethers.formatEther(await provider.getBalance(relayer.address))} BNB`);
if (m.creditWh === 0n) { console.log("relay OFF (no credit) - nothing to settle"); process.exit(0); }

// Since when has the light been on? The later of: last settlement, or the credit event that
// switched it on. Look back up to ~2 hours of blocks.
const latest = await provider.getBlockNumber();
let sinceTs = null;
for (let to = latest; to > latest - 20000 && sinceTs === null; to -= 9999) {
  const from = Math.max(0, to - 9998);
  const logs = await provider.getLogs({ address: GRID, topics: [null, meterId], fromBlock: from, toBlock: to });
  if (logs.length) sinceTs = (await provider.getBlock(logs[logs.length - 1].blockNumber)).timestamp;
}
const now = (await provider.getBlock("latest")).timestamp;
const gapS = Math.min(now - (sinceTs ?? now - MAX_GAP_MIN * 60), MAX_GAP_MIN * 60);
let wh = Math.floor((LOAD_W * gapS) / 3600);
if (wh > Number(m.creditWh)) wh = Number(m.creditWh);
console.log(`load ${LOAD_W} W for ${Math.round(gapS / 60)} min -> ${wh} Wh`);
if (wh < 1) { console.log("under 1 Wh - skip"); process.exit(0); }

const nonce = Number(m.lastNonce) + 1;
const { chainId } = await provider.getNetwork();
const sig = await device.signTypedData(
  { name: "ImoleGrid", version: "1", chainId, verifyingContract: GRID },
  { Usage: [{ name: "meterId", type: "bytes32" }, { name: "wh", type: "uint64" }, { name: "nonce", type: "uint64" }] },
  { meterId, wh, nonce },
);
const tx = await grid.reportUsage(meterId, wh, nonce, sig);
console.log(`sent ${wh} Wh (nonce ${nonce}) · tx ${tx.hash}`);
// Public RPCs sometimes lag on receipts; poll ourselves instead of failing the run.
let rc = null;
for (let i = 0; i < 20 && !rc; i++) {
  await new Promise((r) => setTimeout(r, 3000));
  try { rc = await provider.getTransactionReceipt(tx.hash); } catch {}
}
if (!rc) { console.log("receipt not seen yet - tx was broadcast; it will show on BscScan"); process.exit(0); }
if (rc.status !== 1) throw new Error(`tx reverted: ${tx.hash}`);
console.log(`settled ${wh} Wh · credit left ${Number(m.creditWh) - wh} Wh · block ${rc.blockNumber}`);

async function firstWorking(urls) {
  for (const u of urls) {
    try { const p = new ethers.JsonRpcProvider(u.trim(), 56, { staticNetwork: true }); await p.getBlockNumber(); return p; } catch {}
  }
  throw new Error("no RPC reachable");
}
