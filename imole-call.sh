#!/usr/bin/env bash
# Preview an Agentic Wallet contract call, show what it will do, and execute only after you type y.
# Usage: bash imole-call.sh <label> <to-address> <calldata>
set -e
FROM=0x6E1f725Cf6E062eE88f19358001bD0f79C1A2437   # onegeex.ai Agentic Wallet
LABEL=$1; TO=$2; DATA=$3
[ -z "$DATA" ] && { echo "usage: bash imole-call.sh <label> <to> <calldata>"; exit 1; }

echo "== Preview: $LABEL"
P=$(baw contract-call preview --binanceChainId 56 --from $FROM --to $TO --value 0 --inputData $DATA --json)
echo "$P" | node -e '
let j=JSON.parse(require("fs").readFileSync(0,"utf8"));
if(!j.success){console.log("PREVIEW FAILED:",JSON.stringify(j.error));process.exit(2)}
let d=j.data,s=d.simulationResult||{};
console.log("  to:               ",d.parsedTx&&d.parsedTx.contractAddress);
console.log("  simulation code:  ",s.simulationCode, s.simulationErrorDetail?("ERROR "+JSON.stringify(s.simulationErrorDetail)):"(ok)");
console.log("  balance changes:  ",JSON.stringify(s.balanceChanges||[]));
console.log("  allowance changes:",JSON.stringify(s.allowanceChanges||[]));
console.log("  risks:            ",JSON.stringify((d.risks||{}).riskDetails||[]));
console.log("  needs app confirm:",d.requireConfirmation);
' || exit 1
RID=$(echo "$P" | node -e 'console.log(JSON.parse(require("fs").readFileSync(0,"utf8")).data.requestId)')
read -p "Execute this? [y/N] " OK
[ "$OK" = "y" ] || { echo "Cancelled. Nothing sent."; exit 0; }
baw contract-call execute --requestId $RID --json
