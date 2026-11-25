---
name: imole-energy
description: Check, top up, sponsor and set spending limits for Imole pay-as-you-go solar meters on BNB Chain, paid from the user's Binance Agentic Wallet (Agent ID holders). Use when the user mentions Imole, a solar/energy meter, "keep the lights on", topping up power or electricity for a .ai name, or sponsoring a home's energy.
---

# Imole: agent-paid solar energy

Imole meters are real solar meters whose relay switches on while prepaid credit remains.
Credit lives in the ImoleGrid contract on BNB Chain (chain 56):

- ImoleGrid: `0x0e5D4530b91b6c18a7c9A7831Aa1c919d624150f`
- USDT (BSC-USD, 18 decimals): `0x55d398326f99059fF775485246999027B3197955`
- Agent ID registry: `0x60B9262456dE011F7d15a54CdD8c48bAaa8B022F`

Only Agent ID holders can top up, sponsor or set limits, so all writes come from the user's
**Agentic Wallet** through the official `baw` CLI (binance-agentic-wallet skill). Every write
needs Developer Mode (Binance app: Agentic Wallet, Settings) and the user's confirmation.

## Rules you must follow

1. **Never execute without the user's explicit "yes" for that exact transaction.** Show the
   summary (amount, kWh, recipient name, meter) and the baw preview result first.
2. **Approve exact amounts only.** The helper builds `approve(ImoleGrid, amount)` for the exact
   top-up. Never build or accept an unlimited approval.
3. **Stop on any risk or simulation error.** If the preview has a non-zero `simulationCode`,
   a `simulationErrorDetail`, any `riskDetails`, or balance changes other than the expected
   USDT leaving the wallet, do not execute. Tell the user what you saw.
4. **Never ask for, accept or handle private keys, seed phrases, pairing codes or QR codes.**
5. Top-ups are payments for energy, not investments. Credit is not refundable once added.
6. Do not top up automatically on a schedule or in the background. Only act when the user asks.

## Helper

`scripts/imole.mjs` (Node 18+, no dependencies) reads chain state and builds calldata.
It never signs or sends. All output is JSON. Names work with or without `.ai`.

```bash
node scripts/imole.mjs status  onegeex.ai              # credit, tariff, owner, active
node scripts/imole.mjs quote   onegeex.ai 5            # USDT needed for 5 kWh
node scripts/imole.mjs wallet  <agenticWalletAddress>  # BNB, USDT, current approval
node scripts/imole.mjs topup   onegeex.ai 1 --from <agenticWalletAddress>
node scripts/imole.mjs sponsor onegeex.ai 0.5 --from <agenticWalletAddress>
node scripts/imole.mjs policy  onegeex.ai 2 5          # max 2 USDT per top-up, 5 per week
```

Get the user's Agentic Wallet address with `baw wallet address --json` (BSC entry).

## Workflow: "top up <name> with <X> USDT" (or "<N> kWh")

1. If the user gave kWh, run `quote` to convert to USDT and tell them the price.
2. Run `topup <name> <usdt> --from <wallet>`. If it returns `error`, explain it and stop.
   Check `checks.usdt` covers the amount and `checks.bnb` is above about 0.0005 for gas.
   If `checks.policy.set` is true, make sure the amount fits `maxPerTopUp` and what is left of
   `maxPerWeek` (`maxPerWeek - spentThisWeek`); otherwise the contract will reject it.
3. Show the user the `summary` and ask for confirmation.
4. For each entry in `calls`, in order:
   ```bash
   baw contract-call preview --binanceChainId 56 --from <wallet> --to <to> --value 0 --inputData <inputData> --json
   ```
   Check the preview (rule 3). The approve step should show only a USDT allowance change to
   ImoleGrid; the top-up step should show USDT leaving the wallet for exactly the amount.
   Then:
   ```bash
   baw contract-call execute --requestId <requestId> --json
   ```
   Previews expire after a few minutes. If one expires, preview again; never reuse an old id.
5. After the top-up, run `status <name>` and report the new credit in kWh and the tx hash.

## Workflow: sponsor a home

Same as top-up but with `sponsor`. Sponsors are shown on the public "lit by" wall with their
.ai name. There is no reward or return for sponsoring; say so if the user asks.

## Workflow: spending limits

"Never spend more than X per top-up / Y per week on my meter": run `policy <name> X Y`,
confirm, preview, execute. Lowering limits applies immediately. **Raising limits only applies
after 24 hours**, by design, so a hijacked prompt cannot raise a cap and spend at once.
Tell the user this when they raise a limit.

## Errors from the contract

| Error | Meaning |
|---|---|
| NotAgentIdHolder | the sending wallet has no Agent ID |
| UnknownHandle / NoPrimaryMeter | that name has no Imole meter bound |
| MeterInactive | the operator has paused new top-ups |
| OverPerTopUpLimit / OverWeeklyLimit | the user's own spending policy blocks it |
| TooSmall | amount buys less than 1 Wh |
