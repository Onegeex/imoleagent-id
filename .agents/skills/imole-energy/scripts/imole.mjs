#!/usr/bin/env node
// Imole skill helper — zero dependencies (Node >= 18).
// Reads ImoleGrid state from BNB Chain and builds calldata for `baw contract-call preview`.
// It NEVER signs or sends anything: the Agentic Wallet does that, after the human confirms.
//
//   node imole.mjs status   <name.ai | 0xMeterId>
//   node imole.mjs quote    <name.ai | 0xMeterId> <kWh>
//   node imole.mjs wallet   <0xAgenticWallet>
//   node imole.mjs topup    <name.ai | 0xMeterId> <usdt> [--from 0xWallet]
//   node imole.mjs sponsor  <name.ai | 0xMeterId> <usdt> [--from 0xWallet]
//   node imole.mjs policy   <name.ai | 0xMeterId> <maxPerTopUpUsdt> <maxPerWeekUsdt>
//
// Output is JSON. Build commands return { calls: [{ step, to, inputData, value }] } to pass,
// one at a time, to: baw contract-call preview --binanceChainId 56 --from <wallet> --to <to> --value 0 --inputData <inputData> --json

const GRID = process.env.IMOLE_GRID || "0x0e5D4530b91b6c18a7c9A7831Aa1c919d624150f";
const USDT = "0x55d398326f99059fF775485246999027B3197955"; // BSC-USD, 18 decimals
const RPC = process.env.BSC_RPC_URL || "https://bsc-dataseed.bnbchain.org";
const MAX_USDT_PER_CALL = Number(process.env.IMOLE_MAX_USDT || 50); // sanity cap for a single build

const SEL = {
  approve: "095ea7b3", allowance: "dd62ed3e", balanceOf: "70a08231",
  topUp: "b67644b9", topUpByName: "b597cf44", sponsor: "1c79d925", setPolicy: "3381a72e",
  meterOfHandle: "95a58058", meters: "1850861f", effectivePolicy: "955ed30f",
};

// ---------------------------------------------------------------- ABI encoding
const word = (hex) => hex.replace(/^0x/, "").toLowerCase().padStart(64, "0");
const addr = (a) => { if (!/^0x[0-9a-fA-F]{40}$/.test(a)) fail(`bad address: ${a}`); return word(a); };
const b32 = (h) => { if (!/^0x[0-9a-fA-F]{64}$/.test(h)) fail(`bad bytes32: ${h}`); return h.slice(2).toLowerCase(); };
const uint = (n) => word(BigInt(n).toString(16));
function str(s) {
  const hex = Buffer.from(s, "utf8").toString("hex");
  return uint(hex.length / 2) + hex.padEnd(Math.ceil(hex.length / 64) * 64 || 64, "0");
}
const usdtWei = (x) => {
  if (!/^\d+(\.\d{1,18})?$/.test(String(x))) fail(`bad USDT amount: ${x}`);
  const [i, f = ""] = String(x).split(".");
  return BigInt(i) * 10n ** 18n + BigInt((f + "0".repeat(18)).slice(0, 18));
};
const fmt = (wei, d = 4) => (Number(BigInt(wei)) / 1e18).toFixed(d);

// ---------------------------------------------------------------- RPC
async function rpc(method, params) {
  const r = await fetch(RPC, { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify({ jsonrpc: "2.0", id: 1, method, params }) });
  const j = await r.json();
  if (j.error) throw new Error(j.error.message || JSON.stringify(j.error));
  return j.result;
}
const call = (to, data) => rpc("eth_call", [{ to, data: "0x" + data }, "latest"]);
const words = (hex) => hex.slice(2).match(/.{64}/g) || [];

async function resolveMeter(target) {
  if (/^0x[0-9a-fA-F]{64}$/.test(target)) return { meterId: target.toLowerCase(), name: null };
  const name = target.trim().toLowerCase();
  let res;
  try { res = await call(GRID, SEL.meterOfHandle + uint(32) + str(name)); }
  catch { fail(`"${name}" has no Imole meter (the name is unknown or no meter is bound to it)`); }
  return { meterId: res, name: name.endsWith(".ai") ? name : name + ".ai" };
}

async function meterInfo(meterId) {
  const w = words(await call(GRID, SEL.meters + b32(meterId)));
  if (!w.length || BigInt("0x" + w[0]) === 0n) fail(`meter ${meterId} is not registered`);
  const a = (x) => "0x" + x.slice(24);
  const tariff = BigInt("0x" + w[4]);
  const creditWh = Number(BigInt("0x" + w[6]));
  return {
    meterId, operator: a(w[0]), device: a(w[1]), owner: a(w[2]),
    tariffUsdtPerKwh: fmt(tariff), creditKwh: (creditWh / 1000).toFixed(3), creditWh,
    escrowUsdt: fmt("0x" + w[5]), totalKwhUsed: (Number(BigInt("0x" + w[8])) / 1000).toFixed(3),
    active: BigInt("0x" + w[9]) === 1n, _tariff: tariff,
  };
}

async function walletInfo(wallet) {
  const [bnb, usdt, allow] = await Promise.all([
    rpc("eth_getBalance", [wallet, "latest"]),
    call(USDT, SEL.balanceOf + addr(wallet)),
    call(USDT, SEL.allowance + addr(wallet) + addr(GRID)),
  ]);
  return { wallet, bnb: fmt(bnb, 6), usdt: fmt(usdt), usdtApprovedToImole: fmt(allow) };
}

async function policyInfo(wallet, meterId) {
  const w = words(await call(GRID, SEL.effectivePolicy + addr(wallet) + b32(meterId)));
  return { set: BigInt("0x" + w[0]) === 1n, maxPerTopUp: fmt("0x" + w[1], 2), maxPerWeek: fmt("0x" + w[2], 2),
    spentThisWeek: fmt("0x" + w[3], 2), pendingIncreaseAt: Number(BigInt("0x" + w[4])) || null };
}

// ---------------------------------------------------------------- commands
const [cmd, ...rest] = process.argv.slice(2);
const flag = (k) => { const i = rest.indexOf(k); return i >= 0 ? rest[i + 1] : undefined; };
const args = rest.filter((x, i) => !x.startsWith("--") && !(i > 0 && rest[i - 1].startsWith("--")));

try {
  switch (cmd) {
    case "status": {
      const { meterId, name } = await resolveMeter(need(args[0], "name or meterId"));
      const m = await meterInfo(meterId);
      const hours60W = (m.creditWh / 60).toFixed(1);
      out({ name, ...strip(m), estHoursLeftAt60W: hours60W });
      break;
    }
    case "quote": {
      const { meterId, name } = await resolveMeter(need(args[0], "name or meterId"));
      const kwh = Number(need(args[1], "kWh"));
      const m = await meterInfo(meterId);
      const wei = (BigInt(Math.round(kwh * 1000)) * m._tariff + 999n) / 1000n;
      out({ name, meterId, kWh: kwh, usdt: fmt(wei, 6), tariffUsdtPerKwh: m.tariffUsdtPerKwh });
      break;
    }
    case "wallet": out(await walletInfo(need(args[0], "wallet address"))); break;
    case "topup":
    case "sponsor": {
      const { meterId, name } = await resolveMeter(need(args[0], "name or meterId"));
      const amount = need(args[1], "USDT amount");
      const wei = usdtWei(amount);
      if (Number(amount) > MAX_USDT_PER_CALL) fail(`amount above the ${MAX_USDT_PER_CALL} USDT sanity cap; ask the user to confirm and set IMOLE_MAX_USDT`);
      const m = await meterInfo(meterId);
      if (!m.active) fail("this meter is not accepting top-ups right now");
      const wh = (wei * 1000n) / m._tariff;
      if (wh === 0n) fail("amount too small to buy 1 Wh");
      const from = flag("--from");
      const checks = from ? { ...(await walletInfo(from)), ...(cmd === "topup" ? { policy: await policyInfo(from, meterId) } : {}) } : null;
      if (checks && Number(checks.usdt) < Number(amount)) fail(`wallet holds ${checks.usdt} USDT, needs ${amount}`);
      const main = cmd === "sponsor"
        ? { step: `sponsor ${name || meterId} with ${amount} USDT`, to: GRID, inputData: "0x" + SEL.sponsor + b32(meterId) + uint(wei), value: "0" }
        : name
          ? { step: `top up ${name} with ${amount} USDT`, to: GRID, inputData: "0x" + SEL.topUpByName + uint(64) + uint(wei) + str(name), value: "0" }
          : { step: `top up meter ${meterId} with ${amount} USDT`, to: GRID, inputData: "0x" + SEL.topUp + b32(meterId) + uint(wei), value: "0" };
      out({
        summary: `${cmd === "sponsor" ? "Sponsor" : "Top up"} ${name || meterId}: ${amount} USDT -> ${(Number(wh) / 1000).toFixed(3)} kWh at ${m.tariffUsdtPerKwh} USDT/kWh`,
        meterId, kWh: (Number(wh) / 1000).toFixed(3), checks,
        calls: [
          { step: `approve exactly ${amount} USDT to ImoleGrid`, to: USDT, inputData: "0x" + SEL.approve + addr(GRID) + uint(wei), value: "0" },
          main,
        ],
      });
      break;
    }
    case "policy": {
      const { meterId, name } = await resolveMeter(need(args[0], "name or meterId"));
      const per = usdtWei(need(args[1], "max per top-up")), week = usdtWei(need(args[2], "max per week"));
      if (per > week) fail("max per top-up cannot exceed max per week");
      out({
        summary: `Limit top-ups to ${name || meterId}: max ${args[1]} USDT each, ${args[2]} USDT per week. Lowering applies now; raising applies after 24 h.`,
        calls: [{ step: "set spending policy", to: GRID, inputData: "0x" + SEL.setPolicy + b32(meterId) + uint(per) + uint(week), value: "0" }],
      });
      break;
    }
    default:
      console.log("usage: node imole.mjs status|quote|wallet|topup|sponsor|policy ... (see header of this file)");
      process.exit(1);
  }
} catch (e) { fail(e.message || String(e)); }

function need(v, what) { if (v === undefined || v === "") fail(`missing ${what}`); return v; }
function strip(m) { const { _tariff, ...r } = m; return r; }
function out(o) { console.log(JSON.stringify(o, null, 2)); }
function fail(msg) { console.log(JSON.stringify({ error: msg })); process.exit(1); }
