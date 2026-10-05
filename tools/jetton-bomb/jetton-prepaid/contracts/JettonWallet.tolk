import "@stdlib/gas-payments"
import "errors"
import "fees-management"
import "jetton-utils"
import "messages"
import "sharding"
import "storage"

contract JettonWallet {
    author: "@vimlover"
    version: "1.0.0"
    description: "Jetton Wallet with prepaid balance. TEP-74 and TEP-89 compliant"
    storage: WalletStorage
    incomingMessages: AllowedMessageToWallet
}

type AllowedMessageToWallet =
    | AskToTransfer
    | AskToBurn
    | InternalTransferStep
    | TopUpGrams
    | WithdrawGrams

type BounceOpToHandle = InternalTransferStep | BurnNotificationForMinter

fun onBouncedMessage(in: InMessageBounced) {
    in.bouncedBody.skipBouncedPrefix();

    val msg = lazy BounceOpToHandle.fromSlice(in.bouncedBody);
    val restoreAmount = match (msg) {
        InternalTransferStep => msg.jettonAmount,
        BurnNotificationForMinter => msg.jettonAmount,
    };

    var storage = lazy WalletStorage.load();
    storage.jettonBalance += restoreAmount;
    storage.save();
}

fun onInternalMessage(in: InMessage) {
    val msg = lazy AllowedMessageToWallet.fromSlice(in.body);

    match (msg) {
        InternalTransferStep => {
            var storage = lazy WalletStorage.load();
            if (in.senderAddress != storage.minterAddress) {
                assert (
                    in.senderAddress ==
                    calcAddressOfJettonWallet(
                        msg.transferInitiator!,
                        storage.minterAddress,
                        contract.getCode(),
                    )
                ) throw Errors.NotValidWallet;
            }
            storage.jettonBalance += msg.jettonAmount;
            storage.save();

            if (msg.forwardTonAmount != 0) {
                val notifyOwnerMsg = createMessage({
                    bounce: BounceMode.NoBounce,
                    dest: storage.ownerAddress,
                    value: msg.forwardTonAmount,
                    body: TransferNotificationForRecipient {
                        queryId: msg.queryId,
                        jettonAmount: msg.jettonAmount,
                        transferInitiator: msg.transferInitiator,
                        forwardPayload: msg.forwardPayload,
                    },
                });
                notifyOwnerMsg.send(
                    SEND_MODE_PAY_FEES_SEPARATELY | SEND_MODE_BOUNCE_ON_ACTION_FAIL,
                );
            }

            if (msg.sendExcessesTo != null) {
                val toLeaveOnBalance = contract.getOriginalBalance() - in.valueCoins +
                contract.getStorageDuePayment();
                reserveGramsOnBalance(
                    max(toLeaveOnBalance, calculateJettonWalletMinStorageFee()),
                    RESERVE_MODE_AT_MOST,
                );

                val excessesMsg = createMessage({
                    bounce: BounceMode.NoBounce,
                    dest: msg.sendExcessesTo,
                    value: 0,
                    body: ReturnExcessesBack { queryId: msg.queryId },
                });
                // SAFETY: It is safe because of reserveGramsOnBalance() called earlier
                excessesMsg.send(SEND_MODE_CARRY_ALL_BALANCE | SEND_MODE_IGNORE_ERRORS);
            }
        }

        AskToTransfer => {
            msg.forwardPayload.checkIsCorrectTlbEither();
            assert (
                msg.transferRecipient.getWorkchain() == MY_WORKCHAIN
            ) throw Errors.WrongWorkchain;
            checkAmountIsEnoughToTransfer(
                in.valueCoins,
                msg.forwardTonAmount,
                in.originalForwardFee,
            );

            var storage = lazy WalletStorage.load();
            assert (in.senderAddress == storage.ownerAddress) throw Errors.NotOwner;
            assert (storage.jettonBalance >= msg.jettonAmount) throw Errors.BalanceError;
            storage.jettonBalance -= msg.jettonAmount;
            storage.save();

            val transferBody = InternalTransferStep {
                queryId: msg.queryId,
                jettonAmount: msg.jettonAmount,
                transferInitiator: storage.ownerAddress,
                sendExcessesTo: msg.sendExcessesTo,
                forwardTonAmount: msg.forwardTonAmount,
                forwardPayload: msg.forwardPayload,
            };
            val transferSendMode = SEND_MODE_CARRY_ALL_REMAINING_MESSAGE_VALUE |
            SEND_MODE_BOUNCE_ON_ACTION_FAIL;

            // internal_init_mode: first 2 bits of customPayload (nil => 0). Controls how the
            // recipient's jetton wallet is addressed in the transfer step:
            //   0 (default) = full state_init — deploys the recipient if absent (classic TEP-74).
            //   1 (bare)    = plain address, NO state_init — omits forwarding+importing the ~855B
            //                 wallet code+data. Same address as mode 0; correct only when the recipient
            //                 is already deployed (e.g. our pre-deployed spam pool). This is the cost win.
            // (A library-reference state_init was considered for a "mode 2" but rejected: it yields a
            //  different address AND bloats this forwarded code — both counter to the goal.)
            val initMode = msg.customPayload != null
                ? msg.customPayload!.beginParse().loadUint(2)
                : 0;

            if (initMode == 1) {
                val transferMsg = createMessage({
                    bounce: BounceMode.Only256BitsOfBody,
                    dest: calcAddressOfJettonWallet(
                        msg.transferRecipient,
                        storage.minterAddress,
                        contract.getCode(),
                    ),
                    value: 0,
                    body: transferBody,
                });
                transferMsg.send(transferSendMode);
            } else {
                val transferMsg = createMessage({
                    bounce: BounceMode.Only256BitsOfBody,
                    dest: calcDeployedJettonWallet(
                        msg.transferRecipient,
                        storage.minterAddress,
                        contract.getCode(),
                    ),
                    value: 0,
                    body: transferBody,
                });
                transferMsg.send(transferSendMode);
            }
        }

        AskToBurn => {
            checkAmountIsEnoughToBurn(in.valueCoins);

            var storage = lazy WalletStorage.load();
            assert (in.senderAddress == storage.ownerAddress) throw Errors.NotOwner;
            assert (storage.jettonBalance >= msg.jettonAmount) throw Errors.BalanceError;
            storage.jettonBalance -= msg.jettonAmount;
            storage.save();

            val notifyMinterMsg = createMessage({
                bounce: BounceMode.Only256BitsOfBody,
                dest: storage.minterAddress,
                value: 0,
                body: BurnNotificationForMinter {
                    queryId: msg.queryId,
                    jettonAmount: msg.jettonAmount,
                    burnInitiator: storage.ownerAddress,
                    sendExcessesTo: msg.sendExcessesTo,
                },
            });
            notifyMinterMsg.send(
                SEND_MODE_CARRY_ALL_REMAINING_MESSAGE_VALUE | SEND_MODE_BOUNCE_ON_ACTION_FAIL,
            );
        }

        TopUpGrams => {
            // just accept grams
        }

        WithdrawGrams => {
            // Teardown for throwaway test wallets: sweep the ENTIRE GRAM balance to
            // `to` (or the owner if omitted) and self-destruct the jetton wallet.
            val storage = lazy WalletStorage.load();
            assert (in.senderAddress == storage.ownerAddress) throw Errors.NotOwner;

            val sweepTo = msg.to != null ? msg.to! : storage.ownerAddress;
            val sweepMsg = createMessage({
                bounce: BounceMode.NoBounce,
                dest: sweepTo,
                value: 0,
                body: ReturnExcessesBack { queryId: msg.queryId },
            });
            // SAFETY: teardown of a throwaway test wallet — CARRY_ALL_BALANCE|DESTROY sweeps the ENTIRE
            // GRAM balance to `sweepTo` and deletes this jetton wallet. Only the owner reaches here (the
            // sender==owner assert above) and nothing runs afterwards, so there is no state left to
            // corrupt; IGNORE_ERRORS just keeps a failed sweep from bricking the wallet.
            sweepMsg.send(
                SEND_MODE_CARRY_ALL_BALANCE | SEND_MODE_DESTROY | SEND_MODE_IGNORE_ERRORS,
            );
        }

        else => throw 0xFFFF,
    }
}

get fun get_wallet_data(): JettonWalletDataReply {
    val storage = lazy WalletStorage.load();

    return {
        jettonBalance: storage.jettonBalance,
        ownerAddress: storage.ownerAddress,
        minterAddress: storage.minterAddress,
        jettonWalletCode: contract.getCode(),
    };
}
