#!/usr/bin/env bash
# Read-only checks against the live Agent ID registry on BNB Chain. Sends no transactions.
REG=0x60B9262456dE011F7d15a54CdD8c48bAaa8B022F
ME=${1:-0x6E1f725Cf6E062eE88f19358001bD0f79C1A2437}
NAME=${2:-onegeex}
RPC=${BSC_RPC_URL:-https://bsc-dataseed.bnbchain.org}
echo "balanceOf($ME):";        cast call $REG "balanceOf(address)(uint256)" $ME --rpc-url $RPC
echo "handleOf($ME):";         cast call $REG "handleOf(address)(string)" $ME --rpc-url $RPC
echo "ownerOfHandle($NAME):";  cast call $REG "ownerOfHandle(string)(address)" $NAME --rpc-url $RPC
echo "name/symbol:";           cast call $REG "name()(string)" --rpc-url $RPC; cast call $REG "symbol()(string)" --rpc-url $RPC
