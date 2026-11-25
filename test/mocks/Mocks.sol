// SPDX-License-Identifier: MIT
pragma solidity 0.8.28;

import {ERC20} from "@openzeppelin/contracts/token/ERC20/ERC20.sol";

/// @dev Stand-in for the Agent ID registry (not deployed on BSC testnet).
contract MockAgentID {
    mapping(address => string) public handleOf;
    mapping(string => address) internal _owners;

    function give(address who, string calldata handle) external {
        handleOf[who] = handle;
        _owners[handle] = who;
    }

    function balanceOf(address who) external view returns (uint256) {
        return bytes(handleOf[who]).length > 0 ? 1 : 0;
    }

    function ownerOfHandle(string calldata handle) external view returns (address) {
        return _owners[handle];
    }
}

contract MockUSDT is ERC20 {
    constructor() ERC20("Tether USD", "USDT") {}

    function mint(address to, uint256 amount) external {
        _mint(to, amount);
    }
}
