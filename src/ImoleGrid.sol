// SPDX-License-Identifier: MIT
pragma solidity 0.8.28;

import {IERC20} from "@openzeppelin/contracts/token/ERC20/IERC20.sol";
import {SafeERC20} from "@openzeppelin/contracts/token/ERC20/utils/SafeERC20.sol";
import {ReentrancyGuard} from "@openzeppelin/contracts/utils/ReentrancyGuard.sol";
import {EIP712} from "@openzeppelin/contracts/utils/cryptography/EIP712.sol";
import {ECDSA} from "@openzeppelin/contracts/utils/cryptography/ECDSA.sol";

/// @notice Minimal view of the SPACE ID Agent ID registry on BNB Chain
///         (0x60B9262456dE011F7d15a54CdD8c48bAaa8B022F).
interface IAgentID {
    function balanceOf(address owner) external view returns (uint256);
    function handleOf(address owner) external view returns (string memory);
    function ownerOfHandle(string calldata handle) external view returns (address);
}

/// @title ImoleGrid — agent-paid pay-as-you-go solar energy
/// @notice Agent ID holders prepay solar meters in USDT. Each meter's device signs
///         usage reports (EIP-712); the contract releases escrowed USDT to the
///         meter's operator only against signed usage.
/// @dev There is NO owner, admin, pause, mint or upgrade function in this contract.
///      Operators control only their own meters (device key, active flag, owner binding).
contract ImoleGrid is EIP712, ReentrancyGuard {
    using SafeERC20 for IERC20;

    // ------------------------------------------------------------------ constants

    uint256 public constant POLICY_DELAY = 24 hours;
    uint256 public constant WEEK = 7 days;
    bytes32 public constant USAGE_TYPEHASH = keccak256("Usage(bytes32 meterId,uint64 wh,uint64 nonce)");

    IAgentID public immutable agentId;
    IERC20 public immutable usdt;

    // ------------------------------------------------------------------ storage

    struct Meter {
        address operator; // installer; receives USDT as energy is used
        address device; // meter's signing key for usage reports
        address owner; // household's Agent ID wallet (0 until bound)
        address pendingOwner; // set by operator, accepted by owner
        uint128 tariff; // USDT base units (18 dp on BSC) per kWh
        uint128 escrow; // USDT held for this meter's unused credit
        uint64 creditWh; // prepaid energy remaining
        uint64 lastNonce; // replay protection for usage reports
        uint64 totalWhUsed;
        bool active; // operator can stop new top-ups (usage still settles)
    }

    struct Policy {
        uint128 maxPerTopUp;
        uint128 maxPerWeek;
        uint128 spentThisWeek;
        uint64 weekStart;
        bool set;
        // pending increase, effective after POLICY_DELAY
        uint128 pendingMaxPerTopUp;
        uint128 pendingMaxPerWeek;
        uint64 pendingAt;
    }

    mapping(bytes32 => Meter) public meters;
    mapping(address => bytes32) public primaryMeter; // owner => meterId
    mapping(address => mapping(bytes32 => Policy)) public policies; // payer => meter => policy
    mapping(address => uint256) public whSponsored; // sponsor leaderboard
    mapping(address => uint256) public usdtSponsored;
    uint256 public meterCount;

    // ------------------------------------------------------------------ events

    event MeterRegistered(bytes32 indexed meterId, address indexed operator, address device, uint256 tariff);
    event DeviceChanged(bytes32 indexed meterId, address device);
    event MeterActive(bytes32 indexed meterId, bool active);
    event OwnerProposed(bytes32 indexed meterId, address indexed owner);
    event OwnerBound(bytes32 indexed meterId, address indexed owner, string handle);
    event PolicySet(address indexed payer, bytes32 indexed meterId, uint256 maxPerTopUp, uint256 maxPerWeek);
    event PolicyIncreaseQueued(
        address indexed payer, bytes32 indexed meterId, uint256 maxPerTopUp, uint256 maxPerWeek, uint256 effectiveAt
    );
    event Credited(
        bytes32 indexed meterId, address indexed payer, string handle, uint256 usdtAmount, uint256 wh, bool sponsored
    );
    event UsageSettled(bytes32 indexed meterId, uint64 nonce, uint256 wh, uint256 paidToOperator, uint256 creditWhLeft);

    // ------------------------------------------------------------------ errors

    error NotAgentIdHolder();
    error NotOperator();
    error MeterExists();
    error UnknownMeter();
    error MeterInactive();
    error InvalidParams();
    error NotPendingOwner();
    error TooSmall();
    error OverPerTopUpLimit();
    error OverWeeklyLimit();
    error BadNonce();
    error BadSignature();
    error UnknownHandle();
    error NoPrimaryMeter();

    // ------------------------------------------------------------------ modifiers

    modifier onlyAgent() {
        if (agentId.balanceOf(msg.sender) == 0) revert NotAgentIdHolder();
        _;
    }

    modifier onlyOperator(bytes32 meterId) {
        if (meters[meterId].operator != msg.sender) revert NotOperator();
        _;
    }

    constructor(IAgentID agentId_, IERC20 usdt_) EIP712("ImoleGrid", "1") {
        if (address(agentId_) == address(0) || address(usdt_) == address(0)) revert InvalidParams();
        agentId = agentId_;
        usdt = usdt_;
    }

    // ================================================================== operators

    /// @notice Register a meter. Caller becomes its operator.
    /// @param tariff USDT base units per kWh (e.g. 0.20e18 for $0.20/kWh).
    function registerMeter(bytes32 meterId, address device, uint128 tariff) external onlyAgent {
        if (meterId == bytes32(0) || device == address(0) || tariff == 0) revert InvalidParams();
        if (meters[meterId].operator != address(0)) revert MeterExists();
        Meter storage m = meters[meterId];
        m.operator = msg.sender;
        m.device = device;
        m.tariff = tariff;
        m.active = true;
        unchecked {
            ++meterCount;
        }
        emit MeterRegistered(meterId, msg.sender, device, tariff);
    }

    /// @notice Rotate the meter's signing key (e.g. replaced board).
    function setDevice(bytes32 meterId, address device) external onlyOperator(meterId) {
        if (device == address(0)) revert InvalidParams();
        meters[meterId].device = device;
        emit DeviceChanged(meterId, device);
    }

    /// @notice Stop or resume new top-ups. Existing credit still settles normally.
    function setActive(bytes32 meterId, bool active) external onlyOperator(meterId) {
        meters[meterId].active = active;
        emit MeterActive(meterId, active);
    }

    /// @notice Propose the household wallet that may bind this meter to its .ai name.
    function proposeOwner(bytes32 meterId, address owner) external onlyOperator(meterId) {
        meters[meterId].pendingOwner = owner;
        emit OwnerProposed(meterId, owner);
    }

    // ================================================================== households

    /// @notice Accept ownership; makes this the meter reached by your .ai name.
    function bindOwner(bytes32 meterId) external onlyAgent {
        Meter storage m = meters[meterId];
        if (m.pendingOwner != msg.sender) revert NotPendingOwner();
        m.owner = msg.sender;
        m.pendingOwner = address(0);
        primaryMeter[msg.sender] = meterId;
        emit OwnerBound(meterId, msg.sender, _handle(msg.sender));
    }

    /// @notice Set your own spending limits for top-ups to one meter.
    ///         First policy and any decrease apply at once; any increase waits POLICY_DELAY,
    ///         so a hijacked agent cannot raise its own cap and spend in the same breath.
    function setPolicy(bytes32 meterId, uint128 maxPerTopUp, uint128 maxPerWeek) external onlyAgent {
        if (maxPerTopUp == 0 || maxPerWeek == 0 || maxPerTopUp > maxPerWeek) revert InvalidParams();
        Policy storage p = policies[msg.sender][meterId];
        _applyPending(p);
        if (!p.set || (maxPerTopUp <= p.maxPerTopUp && maxPerWeek <= p.maxPerWeek)) {
            p.maxPerTopUp = maxPerTopUp;
            p.maxPerWeek = maxPerWeek;
            p.set = true;
            p.pendingAt = 0;
            p.pendingMaxPerTopUp = 0;
            p.pendingMaxPerWeek = 0;
            emit PolicySet(msg.sender, meterId, maxPerTopUp, maxPerWeek);
        } else {
            uint64 at = uint64(block.timestamp + POLICY_DELAY);
            p.pendingMaxPerTopUp = maxPerTopUp;
            p.pendingMaxPerWeek = maxPerWeek;
            p.pendingAt = at;
            emit PolicyIncreaseQueued(msg.sender, meterId, maxPerTopUp, maxPerWeek, at);
        }
    }

    // ================================================================== payments

    /// @notice Prepay energy for a meter. Pulls exactly `amount` USDT (approve that exact amount first).
    function topUp(bytes32 meterId, uint256 amount) external nonReentrant onlyAgent {
        _enforcePolicy(msg.sender, meterId, amount);
        _credit(meterId, amount, false);
    }

    /// @notice Prepay energy for the primary meter of a .ai name (pass the name without ".ai").
    function topUpByName(string calldata handle, uint256 amount) external nonReentrant onlyAgent {
        bytes32 meterId = meterOfHandle(handle);
        _enforcePolicy(msg.sender, meterId, amount);
        _credit(meterId, amount, false);
    }

    /// @notice Gift energy to any active meter. Counted on the public sponsor wall.
    function sponsor(bytes32 meterId, uint256 amount) external nonReentrant onlyAgent {
        uint256 wh = _credit(meterId, amount, true);
        whSponsored[msg.sender] += wh;
        usdtSponsored[msg.sender] += amount;
    }

    /// @notice Settle device-signed usage. Anyone may submit (e.g. a relayer); only the
    ///         meter's device key can sign. Pays the operator for the energy used.
    function reportUsage(bytes32 meterId, uint64 wh, uint64 nonce, bytes calldata signature) external nonReentrant {
        Meter storage m = meters[meterId];
        if (m.operator == address(0)) revert UnknownMeter();
        if (nonce <= m.lastNonce) revert BadNonce();

        bytes32 digest = _hashTypedDataV4(keccak256(abi.encode(USAGE_TYPEHASH, meterId, wh, nonce)));
        if (ECDSA.recover(digest, signature) != m.device) revert BadSignature();

        m.lastNonce = nonce;
        uint64 used = wh > m.creditWh ? m.creditWh : wh;
        uint256 pay;
        if (used == m.creditWh) {
            pay = m.escrow; // credit exhausted: settle rounding dust too
        } else {
            pay = (uint256(used) * m.tariff) / 1000;
            if (pay > m.escrow) pay = m.escrow;
        }
        m.creditWh -= used;
        m.escrow -= uint128(pay);
        m.totalWhUsed += used;

        if (pay > 0) usdt.safeTransfer(m.operator, pay);
        emit UsageSettled(meterId, nonce, used, pay, m.creditWh);
    }

    // ================================================================== views

    function creditWh(bytes32 meterId) external view returns (uint64) {
        return meters[meterId].creditWh;
    }

    /// @notice USDT needed to buy `wh` watt-hours on this meter.
    function quote(bytes32 meterId, uint256 wh) external view returns (uint256) {
        uint256 t = meters[meterId].tariff;
        if (t == 0) revert UnknownMeter();
        return (wh * t + 999) / 1000;
    }

    /// @param handle The .ai name, with or without the ".ai" suffix (registry stores it without).
    function meterOfHandle(string calldata handle) public view returns (bytes32 meterId) {
        bytes calldata b = bytes(handle);
        string calldata name = handle;
        if (b.length > 3 && b[b.length - 3] == "." && b[b.length - 2] == "a" && b[b.length - 1] == "i") {
            name = handle[:b.length - 3];
        }
        address owner;
        try agentId.ownerOfHandle(name) returns (address o) {
            owner = o;
        } catch {
            revert UnknownHandle();
        }
        if (owner == address(0)) revert UnknownHandle();
        meterId = primaryMeter[owner];
        if (meterId == bytes32(0)) revert NoPrimaryMeter();
    }

    /// @notice Limits as they apply right now (a matured increase counts as applied).
    function effectivePolicy(address payer, bytes32 meterId)
        external
        view
        returns (bool set, uint256 maxPerTopUp, uint256 maxPerWeek, uint256 spentThisWeek, uint256 pendingAt)
    {
        Policy memory p = policies[payer][meterId];
        if (p.pendingAt != 0 && block.timestamp >= p.pendingAt) {
            p.maxPerTopUp = p.pendingMaxPerTopUp;
            p.maxPerWeek = p.pendingMaxPerWeek;
            p.pendingAt = 0;
        }
        if (block.timestamp >= uint256(p.weekStart) + WEEK) p.spentThisWeek = 0;
        return (p.set, p.maxPerTopUp, p.maxPerWeek, p.spentThisWeek, p.pendingAt);
    }

    function domainSeparator() external view returns (bytes32) {
        return _domainSeparatorV4();
    }

    // ================================================================== internal

    function _credit(bytes32 meterId, uint256 amount, bool sponsored) internal returns (uint256 wh) {
        Meter storage m = meters[meterId];
        if (m.operator == address(0)) revert UnknownMeter();
        if (!m.active) revert MeterInactive();
        wh = (amount * 1000) / m.tariff;
        if (wh == 0 || wh > type(uint64).max - m.creditWh) revert TooSmall();

        usdt.safeTransferFrom(msg.sender, address(this), amount);
        m.escrow += uint128(amount);
        m.creditWh += uint64(wh);
        emit Credited(meterId, msg.sender, _handle(msg.sender), amount, wh, sponsored);
    }

    function _enforcePolicy(address payer, bytes32 meterId, uint256 amount) internal {
        Policy storage p = policies[payer][meterId];
        if (!p.set) return; // no limits chosen; every transaction is still confirmed by the human in-app
        _applyPending(p);
        if (amount > p.maxPerTopUp) revert OverPerTopUpLimit();
        if (block.timestamp >= uint256(p.weekStart) + WEEK) {
            p.weekStart = uint64(block.timestamp);
            p.spentThisWeek = 0;
        }
        if (uint256(p.spentThisWeek) + amount > p.maxPerWeek) revert OverWeeklyLimit();
        p.spentThisWeek += uint128(amount);
    }

    function _applyPending(Policy storage p) internal {
        if (p.pendingAt != 0 && block.timestamp >= p.pendingAt) {
            p.maxPerTopUp = p.pendingMaxPerTopUp;
            p.maxPerWeek = p.pendingMaxPerWeek;
            p.pendingAt = 0;
            p.pendingMaxPerTopUp = 0;
            p.pendingMaxPerWeek = 0;
        }
    }

    function _handle(address who) internal view returns (string memory) {
        try agentId.handleOf(who) returns (string memory h) {
            return h;
        } catch {
            return "";
        }
    }
}
