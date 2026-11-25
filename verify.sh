#!/usr/bin/env zsh
# Verify ImoleGrid v2 (and v1) source on BscScan. Asks for your etherscan.io API key (hidden).
cd "$(dirname "$0")"
read -s "KEY?Paste your etherscan.io API key then press Enter: "; echo
ARGS=$(cast abi-encode "constructor(address,address)" 0x60B9262456dE011F7d15a54CdD8c48bAaa8B022F 0x55d398326f99059fF775485246999027B3197955)
for ADDR in 0xcc3A2b650a904a64AfDcD6a3319056AAC8BBcc99; do
  echo "Verifying $ADDR ..."
  forge verify-contract $ADDR src/ImoleGrid.sol:ImoleGrid --chain 56 --constructor-args $ARGS --etherscan-api-key "$KEY" --watch
done
unset KEY
