# Go load-test binaries (Telegram bot + standalone spammer).
# Run from the repo root; outputs to go-spam/build/, which the run convention expects as
# ./go-spam/build/bot (systemd WorkingDirectory / manual run are both the repo root).
#
#   make            # build all (bot + spam + deploy + tpsmeter)   <- the one-command build
#   make bot        # just the Telegram bot
#   make spam       # just the standalone spammer
#   make deploy     # just the native pool deployer (devnet)
#   make tpsmeter   # just the TPS + shard-count meter
#   make internalspam # bounded StaticTps internal-message benchmark
#   make nativespam # external native payment benchmark
#   make sidechaincheck # read-only execution-mode compatibility check
#   make genprogress # just the DAN-generation progress dashboard
#   make cockpit     # read-only mainnet/testnet load cockpit
#   make vet        # go vet ./...
#   make test       # go test ./...
#   make clean      # remove built binaries
#
# GO overrides the go toolchain (e.g. `make GO=go1.22 bot`).

GO     ?= go
GO_DIR := go-spam

.PHONY: sidechaincheck
all: sidechaincheck

sidechaincheck:
	cd $(GO_DIR) && mkdir -p build && $(GO) build -o build/sidechaincheck ./cmd/sidechaincheck
	@echo "built $(GO_DIR)/build/sidechaincheck"

.PHONY: all go bot spam deploy tpsmeter internalspam nativespam genprogress cockpit vet test clean

all: bot spam deploy tpsmeter internalspam nativespam genprogress

go: all

bot:
	cd $(GO_DIR) && mkdir -p build && $(GO) build -o build/bot ./cmd/bot
	@echo "built $(GO_DIR)/build/bot"

spam:
	cd $(GO_DIR) && mkdir -p build && $(GO) build -o build/spam ./cmd/spam
	@echo "built $(GO_DIR)/build/spam"

deploy:
	cd $(GO_DIR) && mkdir -p build && $(GO) build -o build/deploy ./cmd/deploy
	@echo "built $(GO_DIR)/build/deploy"

tpsmeter:
	cd $(GO_DIR) && mkdir -p build && $(GO) build -o build/tpsmeter ./cmd/tpsmeter
	@echo "built $(GO_DIR)/build/tpsmeter"

internalspam:
	cd $(GO_DIR) && mkdir -p build && $(GO) build -o build/internalspam ./cmd/internalspam
	@echo "built $(GO_DIR)/build/internalspam"

nativespam:
	cd $(GO_DIR) && mkdir -p build && $(GO) build -o build/nativespam ./cmd/nativespam
	@echo "built $(GO_DIR)/build/nativespam"

genprogress:
	cd $(GO_DIR) && mkdir -p build && $(GO) build -o build/genprogress ./cmd/genprogress
	@echo "built $(GO_DIR)/build/genprogress"

cockpit:
	cd $(GO_DIR) && mkdir -p build && $(GO) build -o build/cockpit ./cmd/cockpit
	@echo "built $(GO_DIR)/build/cockpit"

vet:
	cd $(GO_DIR) && $(GO) vet ./...

test:
	cd $(GO_DIR) && $(GO) test ./...

clean:
	rm -f $(GO_DIR)/build/bot $(GO_DIR)/build/spam $(GO_DIR)/build/deploy $(GO_DIR)/build/tpsmeter $(GO_DIR)/build/internalspam $(GO_DIR)/build/nativespam $(GO_DIR)/build/sidechaincheck $(GO_DIR)/build/genprogress $(GO_DIR)/build/cockpit
