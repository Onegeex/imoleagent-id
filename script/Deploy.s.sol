// SPDX-License-Identifier: MIT
pragma solidity 0.8.28;

import {Script, console2} from "forge-std/Script.sol";
import {ImoleGrid, IAgentID} from "../src/ImoleGrid.sol";
import {IERC20} from "@openzeppelin/contracts/token/ERC20/IERC20.sol";

/// forge script script/Deploy.s.sol --rpc-url bsc --broadcast --verify --interactives 1
contract Deploy is Script {
    address constant AGENT_ID = 0x60B9262456dE011F7d15a54CdD8c48bAaa8B022F; // SPACE ID Agent ID (BSC)
    address constant USDT = 0x55d398326f99059fF775485246999027B3197955; // BSC-USD, 18 decimals

    function run() external returns (ImoleGrid grid) {
        vm.startBroadcast();
        grid = new ImoleGrid(IAgentID(AGENT_ID), IERC20(USDT));
        vm.stopBroadcast();
        console2.log("ImoleGrid deployed at", address(grid));
    }
}
