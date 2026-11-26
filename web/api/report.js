// POST /api/report - relay a meter's signed usage report to ImoleGrid.
// The meter (ESP32) signs Usage{meterId, wh, nonce} with its own key; this endpoint only pays gas.
// It checks the signature and nonce before spending gas, and can't move funds any other way:
// the contract itself verifies the device signature and pays only the meter's operator.
import { ethers } from "ethers";

const GRID = process.env.IMOLE_GRID || "0xcc3A2b650a904a64AfDcD6a3319056AAC8BBcc99";
const RPCS = ["https://bsc-rpc.publicnode.com", "https://bsc-dataseed.bnbchain.org"];
const ABI = [
  "function meters(bytes32) view returns (address operator,address device,address owner,address pendingOwner,uint128 tariff,uint128 escrow,uint64 creditWh,uint64 lastNonce,uint64 totalWhUsed,bool active)",
  "function reportUsage(bytes32 meterId,uint64 wh,uint64 nonce,bytes signature)",
];
const TYPES = { Usage: [{ name: "meterId", type: "bytes32" }, { name: "wh", type: "uint64" }, { name: "nonce", type: "uint64" }] };
const MAX_WH_PER_REPORT = 5000;

async function provider() {
  for (const u of RPCS) {
    try { const p = new ethers.JsonRpcProvider(u, 56, { staticNetwork: true }); await p.getBlockNumber(); return p; } catch {}
  }
  throw new Error("BNB Chain RPC unreachable");
}

export default async function handler(req, res) {
  res.setHeader("Access-Control-Allow-Origin", "*");
  res.setHeader("Access-Control-Allow-Headers", "content-type");
  if (req.method === "OPTIONS") return res.status(204).end();
  try {
    const p = await provider();
    if (!process.env.RELAYER_KEY) return res.status(503).json({ ok: false, error: "relayer not configured" });
    const relayer = new ethers.Wallet(process.env.RELAYER_KEY, p);
    if (req.method === "GET") {
      return res.json({ ok: true, relayer: relayer.address, bnb: ethers.formatEther(await p.getBalance(relayer.address)), grid: GRID });
    }
    if (req.method !== "POST") return res.status(405).json({ ok: false, error: "POST only" });

    const body = typeof req.body === "string" ? JSON.parse(req.body) : req.body || {};
    const { meterId, signature } = body;
    const wh = Number(body.wh), nonce = Number(body.nonce);
    if (!/^0x[0-9a-fA-F]{64}$/.test(meterId || "")) return res.status(400).json({ ok: false, error: "bad meterId" });
    if (!/^0x[0-9a-fA-F]{130}$/.test(signature || "")) return res.status(400).json({ ok: false, error: "bad signature format" });
    if (!Number.isInteger(wh) || wh < 1 || wh > MAX_WH_PER_REPORT) return res.status(400).json({ ok: false, error: "bad wh" });
    if (!Number.isInteger(nonce) || nonce < 1) return res.status(400).json({ ok: false, error: "bad nonce" });

    const grid = new ethers.Contract(GRID, ABI, relayer);
    const m = await grid.meters(meterId);
    if (m.operator === ethers.ZeroAddress) return res.status(404).json({ ok: false, error: "unknown meter" });
    const domain = { name: "ImoleGrid", version: "1", chainId: 56, verifyingContract: GRID };
    const signer = ethers.verifyTypedData(domain, TYPES, { meterId, wh, nonce }, signature);
    if (signer.toLowerCase() !== m.device.toLowerCase()) return res.status(401).json({ ok: false, error: "not signed by this meter's device key" });
    if (BigInt(nonce) !== m.lastNonce + 1n) return res.status(409).json({ ok: false, error: "stale nonce", lastNonce: Number(m.lastNonce) });

    const tx = await grid.reportUsage(meterId, wh, nonce, signature);
    return res.json({ ok: true, txHash: tx.hash, wh, nonce, creditWhBefore: Number(m.creditWh) });
  } catch (e) {
    return res.status(500).json({ ok: false, error: (e.shortMessage || e.message || String(e)).slice(0, 200) });
  }
}
