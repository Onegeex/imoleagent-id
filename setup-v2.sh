#!/usr/bin/env bash
# Register pilot meter imole:ife-001 on the deployed ImoleGrid, bind it to onegeex.ai and top it up.
# Each step: baw preview -> you check -> type y. Usage: bash setup-v2.sh <GRID_ADDRESS> [usdt]
set -e
G=${1:?usage: bash setup-v2.sh <GRID_ADDRESS> [usdt]}
AMT=${2:-1}
ME=0x6E1f725Cf6E062eE88f19358001bD0f79C1A2437        # onegeex.ai
DEVICE=0x11d2DA9979F0caBceB8dBf0BE06AB83430622d93    # meter signing key (temporary, until ESP32)
USDT=0x55d398326f99059fF775485246999027B3197955
ID=$(cast keccak "imole:ife-001")
WEI=$(cast to-wei $AMT)
step() { echo; echo "################ $1"; bash "$(dirname "$0")/imole-call.sh" "$1" "$2" "$3"; sleep 3; }
step "registerMeter imole:ife-001 @ 0.20 USDT/kWh" $G $(cast calldata "registerMeter(bytes32,address,uint128)" $ID $DEVICE 200000000000000000)
step "proposeOwner onegeex.ai"                     $G $(cast calldata "proposeOwner(bytes32,address)" $ID $ME)
step "bindOwner (accept as onegeex.ai)"            $G $(cast calldata "bindOwner(bytes32)" $ID)
step "approve exactly $AMT USDT to ImoleGrid"      $USDT $(cast calldata "approve(address,uint256)" $G $WEI)
step "topUpByName onegeex.ai $AMT USDT"            $G $(cast calldata "topUpByName(string,uint256)" onegeex.ai $WEI)
echo; echo "Done. Credit now: $(cast call $G 'creditWh(bytes32)(uint64)' $ID --rpc-url https://bsc-rpc.publicnode.com) Wh"
