// SPDX-License-Identifier: MIT
pragma solidity 0.8.28;

import {Test} from "forge-std/Test.sol";
import {ImoleGrid, IAgentID} from "../src/ImoleGrid.sol";
import {IERC20} from "@openzeppelin/contracts/token/ERC20/IERC20.sol";
import {MockAgentID, MockUSDT} from "./mocks/Mocks.sol";

contract ImoleGridTest is Test {
    ImoleGrid grid;
    MockAgentID reg;
    MockUSDT usdt;

    address operator = makeAddr("operator");
    address home = makeAddr("home"); // household owner
    address friend = makeAddr("friend"); // sponsor
    address stranger = makeAddr("stranger"); // no Agent ID
    uint256 deviceKey = 0xD3B1CE;
    address device;

    bytes32 constant METER = keccak256("imole:ife-001");
    uint128 constant TARIFF = 0.2e18; // $0.20 per kWh

    function setUp() public {
        reg = new MockAgentID();
        usdt = new MockUSDT();
        grid = new ImoleGrid(IAgentID(address(reg)), IERC20(address(usdt)));
        device = vm.addr(deviceKey);

        reg.give(operator, "operator");
        reg.give(home, "onegeex");
        reg.give(friend, "friend");

        for (uint256 i; i < 3; i++) {
            address a = [home, friend, stranger][i];
            usdt.mint(a, 100e18);
        }

        vm.prank(operator);
        grid.registerMeter(METER, device, TARIFF);
    }

    // ---------------------------------------------------------------- helpers

    function _pay(address who, uint256 amount) internal {
        vm.startPrank(who);
        usdt.approve(address(grid), amount);
        grid.topUp(METER, amount);
        vm.stopPrank();
    }

    function _sign(bytes32 id, uint64 wh, uint64 nonce, uint256 key) internal view returns (bytes memory) {
        bytes32 structHash = keccak256(abi.encode(grid.USAGE_TYPEHASH(), id, wh, nonce));
        bytes32 digest = keccak256(abi.encodePacked("\x19\x01", grid.domainSeparator(), structHash));
        (uint8 v, bytes32 r, bytes32 s) = vm.sign(key, digest);
        return abi.encodePacked(r, s, v);
    }

    function _bindHome() internal {
        vm.prank(operator);
        grid.proposeOwner(METER, home);
        vm.prank(home);
        grid.bindOwner(METER);
    }

    // ---------------------------------------------------------------- registration

    function test_RegisterRequiresAgentId() public {
        vm.prank(stranger);
        vm.expectRevert(ImoleGrid.NotAgentIdHolder.selector);
        grid.registerMeter(keccak256("x"), device, TARIFF);
    }

    function test_CannotRegisterTwice() public {
        vm.prank(friend);
        vm.expectRevert(ImoleGrid.MeterExists.selector);
        grid.registerMeter(METER, device, TARIFF);
    }

    function test_OnlyOperatorManagesMeter() public {
        vm.prank(home);
        vm.expectRevert(ImoleGrid.NotOperator.selector);
        grid.setDevice(METER, home);
    }

    // ---------------------------------------------------------------- top-ups

    function test_TopUpCreditsEnergy() public {
        _pay(home, 1e18); // $1 at $0.20/kWh = 5 kWh
        assertEq(grid.creditWh(METER), 5000);
        assertEq(usdt.balanceOf(address(grid)), 1e18);
    }

    function test_TopUpRequiresAgentId() public {
        vm.startPrank(stranger);
        usdt.approve(address(grid), 1e18);
        vm.expectRevert(ImoleGrid.NotAgentIdHolder.selector);
        grid.topUp(METER, 1e18);
        vm.stopPrank();
    }

    function test_TopUpPullsExactAmountOnly() public {
        vm.startPrank(home);
        usdt.approve(address(grid), 1e18);
        grid.topUp(METER, 1e18);
        assertEq(usdt.allowance(home, address(grid)), 0); // nothing left approved
        vm.stopPrank();
    }

    function test_InactiveMeterRejectsTopUp() public {
        vm.prank(operator);
        grid.setActive(METER, false);
        vm.startPrank(home);
        usdt.approve(address(grid), 1e18);
        vm.expectRevert(ImoleGrid.MeterInactive.selector);
        grid.topUp(METER, 1e18);
        vm.stopPrank();
    }

    function test_DustTopUpReverts() public {
        vm.startPrank(home);
        usdt.approve(address(grid), 1);
        vm.expectRevert(ImoleGrid.TooSmall.selector);
        grid.topUp(METER, 1);
        vm.stopPrank();
    }

    // ---------------------------------------------------------------- names

    function test_TopUpByName() public {
        _bindHome();
        assertEq(grid.meterOfHandle("onegeex"), METER);
        vm.startPrank(friend);
        usdt.approve(address(grid), 2e18);
        grid.topUpByName("onegeex", 2e18);
        vm.stopPrank();
        assertEq(grid.creditWh(METER), 10_000);
    }

    function test_NameWithAiSuffix() public {
        _bindHome();
        assertEq(grid.meterOfHandle("onegeex.ai"), METER);
    }

    function test_UnknownNameReverts() public {
        vm.expectRevert(ImoleGrid.UnknownHandle.selector);
        grid.meterOfHandle("nobody");
    }

    function test_NameWithoutMeterReverts() public {
        vm.expectRevert(ImoleGrid.NoPrimaryMeter.selector);
        grid.meterOfHandle("friend");
    }

    function test_OnlyProposedOwnerCanBind() public {
        vm.prank(operator);
        grid.proposeOwner(METER, home);
        vm.prank(friend);
        vm.expectRevert(ImoleGrid.NotPendingOwner.selector);
        grid.bindOwner(METER);
    }

    // ---------------------------------------------------------------- policy

    function test_PolicyCapsEachTopUp() public {
        vm.prank(home);
        grid.setPolicy(METER, 2e18, 5e18);
        vm.startPrank(home);
        usdt.approve(address(grid), 3e18);
        vm.expectRevert(ImoleGrid.OverPerTopUpLimit.selector);
        grid.topUp(METER, 3e18);
        vm.stopPrank();
    }

    function test_PolicyCapsWeek_ThenResets() public {
        vm.prank(home);
        grid.setPolicy(METER, 2e18, 3e18);
        _pay(home, 2e18);
        vm.startPrank(home);
        usdt.approve(address(grid), 2e18);
        vm.expectRevert(ImoleGrid.OverWeeklyLimit.selector);
        grid.topUp(METER, 2e18);
        vm.stopPrank();

        vm.warp(block.timestamp + 7 days);
        _pay(home, 2e18); // new week
        assertEq(grid.creditWh(METER), 20_000);
    }

    function test_PolicyIncreaseIsTimelocked() public {
        vm.prank(home);
        grid.setPolicy(METER, 1e18, 1e18);
        vm.prank(home);
        grid.setPolicy(METER, 10e18, 10e18); // queued, not live

        vm.startPrank(home);
        usdt.approve(address(grid), 5e18);
        vm.expectRevert(ImoleGrid.OverPerTopUpLimit.selector);
        grid.topUp(METER, 5e18);
        vm.stopPrank();

        vm.warp(block.timestamp + 24 hours);
        _pay(home, 5e18); // now allowed
        assertEq(grid.creditWh(METER), 25_000);
    }

    function test_PolicyDecreaseIsImmediate() public {
        vm.prank(home);
        grid.setPolicy(METER, 5e18, 10e18);
        vm.prank(home);
        grid.setPolicy(METER, 1e18, 2e18);
        (, uint256 perTop,,,) = grid.effectivePolicy(home, METER);
        assertEq(perTop, 1e18);
    }

    function test_PolicyIsPerPayer() public {
        vm.prank(home);
        grid.setPolicy(METER, 1e18, 1e18);
        _pay(friend, 5e18); // friend has no policy
        assertEq(grid.creditWh(METER), 25_000);
    }

    // ---------------------------------------------------------------- sponsors

    function test_SponsorCountsOnWall() public {
        vm.startPrank(friend);
        usdt.approve(address(grid), 0.5e18);
        grid.sponsor(METER, 0.5e18);
        vm.stopPrank();
        assertEq(grid.whSponsored(friend), 2500);
        assertEq(grid.usdtSponsored(friend), 0.5e18);
    }

    // ---------------------------------------------------------------- enumeration

    function test_EnumerationTracksParticipantsSponsorsMeters() public {
        _pay(home, 1e18);
        _pay(home, 1e18); // same holder twice counts once
        vm.startPrank(friend);
        usdt.approve(address(grid), 0.5e18);
        grid.sponsor(METER, 0.5e18);
        vm.stopPrank();

        assertEq(grid.participantCount(), 3); // operator (register), home, friend
        address[] memory p = grid.participants(0, 10);
        assertEq(p[0], operator);
        assertEq(p[1], home);
        assertEq(p[2], friend);
        assertEq(grid.sponsorCount(), 1);
        assertEq(grid.sponsors(0, 10)[0], friend);
        assertEq(grid.meterIds(0, 10)[0], METER);
        assertEq(grid.meterIds(5, 10).length, 0);
        assertEq(grid.totalWhCredited(), 12_500);
        assertEq(grid.totalUsdtCredited(), 2.5e18);
    }

    function test_FailedCallDoesNotAddParticipant() public {
        vm.prank(stranger);
        vm.expectRevert(ImoleGrid.NotAgentIdHolder.selector);
        grid.topUp(METER, 1e18);
        assertEq(grid.participantCount(), 1);
    }

    // ---------------------------------------------------------------- usage

    function test_UsagePaysOperator() public {
        _pay(home, 1e18); // 5000 Wh
        grid.reportUsage(METER, 1000, 1, _sign(METER, 1000, 1, deviceKey));
        assertEq(grid.creditWh(METER), 4000);
        assertEq(usdt.balanceOf(operator), 0.2e18);
    }

    function test_UsageBadSignerRejected() public {
        _pay(home, 1e18);
        bytes memory sig = _sign(METER, 1000, 1, 0xBAD);
        vm.expectRevert(ImoleGrid.BadSignature.selector);
        grid.reportUsage(METER, 1000, 1, sig);
    }

    function test_UsageReplayRejected() public {
        _pay(home, 1e18);
        bytes memory sig = _sign(METER, 1000, 1, deviceKey);
        grid.reportUsage(METER, 1000, 1, sig);
        vm.expectRevert(ImoleGrid.BadNonce.selector);
        grid.reportUsage(METER, 1000, 1, sig);
    }

    function test_UsageTamperedAmountRejected() public {
        _pay(home, 1e18);
        bytes memory sig = _sign(METER, 100, 1, deviceKey);
        vm.expectRevert(ImoleGrid.BadSignature.selector);
        grid.reportUsage(METER, 4000, 1, sig);
    }

    function test_UsageBeyondCreditDrainsExactly() public {
        _pay(home, 1e18);
        grid.reportUsage(METER, 9999, 1, _sign(METER, 9999, 1, deviceKey));
        assertEq(grid.creditWh(METER), 0);
        assertEq(usdt.balanceOf(operator), 1e18);
        assertEq(usdt.balanceOf(address(grid)), 0);
    }

    function test_DeviceRotation() public {
        _pay(home, 1e18);
        uint256 newKey = 0xFEED;
        vm.prank(operator);
        grid.setDevice(METER, vm.addr(newKey));
        bytes memory oldSig = _sign(METER, 10, 1, deviceKey);
        vm.expectRevert(ImoleGrid.BadSignature.selector);
        grid.reportUsage(METER, 10, 1, oldSig);
        grid.reportUsage(METER, 10, 1, _sign(METER, 10, 1, newKey));
    }

    function testFuzz_EscrowNeverNegative(uint96 amount, uint64 used) public {
        amount = uint96(bound(amount, 0.01e18, 100e18));
        _pay(home, amount);
        grid.reportUsage(METER, used, 1, _sign(METER, used, 1, deviceKey));
        (,,,,, uint128 escrow, uint64 credit,,,) = grid.meters(METER);
        assertEq(usdt.balanceOf(address(grid)), escrow);
        assertLe(uint256(credit) * TARIFF / 1000, escrow);
    }
}
