# Imole meter firmware (ESP32)

`imole-meter/` runs on a real ESP32 or in the [Wokwi](https://wokwi.com) simulator (same files).

- Reads the meter's credit from ImoleGrid on BNB Chain, drives the relay, shows kWh left and the owner's `.ai` name.
- Meters energy (potentiometer = load in Wokwi; INA219 on hardware).
- Signs `Usage{meterId, wh, nonce}` (EIP-712, secp256k1, low-s, v=27/28) **on the board** and posts it to
  `https://imole-energy.vercel.app/api/report`, which only pays gas. The contract verifies the device signature.

## Run in Wokwi
1. New ESP32 project on wokwi.com. Replace `sketch.ino` with `imole-meter.ino`; add `imole_crypto.h`,
   `diagram.json` and `libraries.txt` as files.
2. Put your meter's device key in `DEVICE_KEY_HEX` (keep that copy private), or leave it empty to get a fresh key
   each run and register its address with `setDevice`.
3. Start. The LCD shows the owner's name and kWh left; the yellow "bulb" lights while there is credit.
   Turn the potentiometer to change the load; flip the switch to simulate losing internet (relay fails safe after 10 min).

## Crypto test (PC)
`test_crypto.cpp` checks keccak256, address derivation, the EIP-712 digest and signatures against ethers.js
(40/40 recover to the device key with low-s).
