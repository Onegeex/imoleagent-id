# Imole: agent-paid solar energy on BNB Chain

**Your AI agent keeps the lights on.** Imole is a pay-as-you-go solar meter that Agent ID holders pay from their Binance Agentic Wallet. The agent checks the meter, tops it up in USDT under spending limits the owner set on-chain, and the meter's relay switches on. Any holder can also sponsor energy for a registered home, credited to their `.ai` name.

Built for the **Binance Wallet × BNB Chain Agent ID Hackathon**.

## Live on BNB Chain mainnet (chain 56)

| | Address |
|---|---|
| ImoleGrid | [`0x0e5D4530b91b6c18a7c9A7831Aa1c919d624150f`](https://bscscan.com/address/0x0e5D4530b91b6c18a7c9A7831Aa1c919d624150f) |
| Agent ID registry (SPACE ID) | `0x60B9262456dE011F7d15a54CdD8c48bAaa8B022F` |
| USDT (BSC-USD) | `0x55d398326f99059fF775485246999027B3197955` |
| Pilot meter `imole:ife-001` | `0x8585db12c1df78cc5d76e7bcdb56e3f44262e21844fe0cb640b399c4a42045e4`, bound to **onegeex.ai** |

First mainnet activity: meter registered by onegeex.ai, bound to the name, topped up by name (1 USDT = 5 kWh), and usage settled on-chain from device-signed reports ([tx](https://bscscan.com/tx/0x2d8f005606f334a4dd8ee2aed75cad06c8562e7f4410de1a219c1935bd0d5ad7)).

## How Agent ID is used

Agent ID is the core of the contract, not a login button:

- **Access.** Every state-changing call except usage settlement requires `balanceOf(msg.sender) > 0` on the Agent ID registry: registering a meter, binding it, topping up, sponsoring and setting limits.
- **Identity.** Each `Credited` and `OwnerBound` event carries the payer's handle from `handleOf()`, so receipts and the sponsor wall read "lit by tunde.ai", not `0x…`.
- **Routing.** `topUpByName("mama.ai", amount)` resolves the name with `ownerOfHandle()` and pays that owner's meter. A `.ai` name becomes a payable energy account.
- **Accountability.** One non-transferable ID per person means spending policies and sponsor history can't be reset by spinning up new wallets.

## How it works

1. The owner tells their agent "top up my meter with 1 USDT" (or "keep me under 5 USDT a week").
2. The agent (`skill/imole-energy`) builds an **exact-amount** `approve` and `topUpByName`, then runs `baw contract-call preview`. The human confirms, and the agent executes.
3. ImoleGrid checks Agent ID and the payer's on-chain policy, converts USDT to Wh at the meter's tariff, and holds the USDT in escrow.
4. The meter polls `creditWh(meterId)` and closes its relay while credit remains.
5. The meter signs EIP-712 `Usage{meterId, wh, nonce}` reports. Anyone can relay them; only the device key can sign. The contract deducts the Wh and releases exactly the matching USDT to the operator. At zero credit the relay opens.

## Safety and permissions

- **No owner, admin, pause, upgrade or mint functions.** Nobody can move escrowed funds except through device-signed usage, which pays only the meter's operator.
- Operators can only manage their own meter: rotate the device key, pause new top-ups, propose an owner.
- **Spending policy per payer per meter.** A limit per top-up and per week. Lowering applies immediately; **raising waits 24 hours**, so a hijacked prompt can't raise its own cap and spend at once.
- Exact approvals only. The skill never builds an unlimited approval.
- No QR or pairing-code login on any website, no stored wallet sessions, no key collection. All writes go through the user's own agent and the official `baw` preview and confirmation.
- Sponsorship is a gift of energy. There is no token, no yield and no promised return.
- Names resolve only to a meter for payment, never to balances or history.

## Repo layout

| Path | What |
|---|---|
| `src/ImoleGrid.sol` | the contract |
| `test/` | 26 Foundry tests (gating, names, policy timelock, signatures, replay, escrow fuzz) |
| `script/Deploy.s.sol` | mainnet deploy |
| `skill/imole-energy/` | Agentic Wallet skill: `SKILL.md` + zero-dependency helper `scripts/imole.mjs` |
| `meter-sim/` | software meter + relayer (same protocol as the ESP32 firmware) |
| `imole-call.sh` | preview → confirm → execute wrapper around `baw contract-call` |

## Install the skill

```bash
npx skills add Onegeex/imoleagent-id --skill imole-energy
```
Requires the Binance Agentic Wallet skill / `baw` CLI, an Agent ID, and Developer Mode on.

## Develop

```bash
bash setup.sh      # fetch OpenZeppelin + forge-std
forge test         # 26 tests
```

## Hardware (pilot)

ESP32 + INA219 (DC) or PZEM-004T (AC) energy sensor + relay + OLED showing kWh left and the owner's `.ai` name, on a small solar kit. The firmware does what `meter-sim/meter.js` does: polls credit, switches the relay, and signs usage reports with a key that never leaves the board.

## Licence

MIT
