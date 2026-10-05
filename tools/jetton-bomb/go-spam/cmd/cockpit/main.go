// cockpit serves a lightweight, read-only load-test dashboard. Run one instance
// per network; the two instances share the same code and use different ports.
package main

import (
	"context"
	"crypto/ed25519"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"github.com/victor/jetton-bomb-go-spam/pkg/backend"
)

type config struct {
	Network      string
	Listen       string
	ExtraListen  string
	MeterURL     string
	V3URL        string
	APIKey       string
	StatusSecret string
	Prometheus   string
	HealthURL    string
	StatsURL     string
	PoolFile     string
	LogsPython   string
	LogsModule   string
	AllowOrigin  string
}

func envOr(name, fallback string) string {
	if v := os.Getenv(name); v != "" {
		return v
	}
	return fallback
}

func main() {
	if err := run(); err != nil {
		log.Fatal(err)
	}
}

func run() error {
	network := flag.String("network", envOr("COCKPIT_NETWORK", "mainnet"), "mainnet or testnet")
	listen := flag.String("listen", envOr("COCKPIT_LISTEN", ""), "HTTP listen address")
	extraListen := flag.String("extra-listen", os.Getenv("COCKPIT_EXTRA_LISTEN"), "optional second HTTP listen address")
	poolFile := flag.String("pool-config", envOr("COCKPIT_POOL_CONFIG", ""), "public pool configuration JSON")
	printPub := flag.String("print-pool-public", "", "print the public key from a local pool.key and exit")
	flag.Parse()
	if *printPub != "" {
		pub, _, err := backend.LoadKeypairFromSeedFile(*printPub)
		if err != nil {
			return err
		}
		if len(pub) != ed25519.PublicKeySize {
			return fmt.Errorf("unexpected public key length %d", len(pub))
		}
		fmt.Println(hex.EncodeToString(pub))
		return nil
	}
	if *network != "mainnet" && *network != "testnet" {
		return fmt.Errorf("network must be mainnet or testnet")
	}
	port, meterPort, statsPort, v3 := "1900", "8092", "18000", "https://toncenter.com/api/v3"
	if *network == "testnet" {
		port, meterPort, statsPort, v3 = "1901", "8093", "18001", "https://testnet.toncenter.com/api/v3"
	}
	if *listen == "" {
		*listen = ":" + port
	}
	cfg := config{
		Network:      *network,
		Listen:       *listen,
		ExtraListen:  *extraListen,
		MeterURL:     envOr("COCKPIT_METER_URL", "http://127.0.0.1:"+meterPort),
		V3URL:        envOr("COCKPIT_V3_URL", v3),
		APIKey:       os.Getenv("TONCENTER_API_KEY"),
		StatusSecret: os.Getenv("COCKPIT_BOT_STATUS_SECRET"),
		Prometheus:   envOr("COCKPIT_PROMETHEUS_URL", "http://127.0.0.1:9090"),
		HealthURL:    envOr("COCKPIT_HEALTH_URL", "https://health.toncenter.com"),
		StatsURL:     envOr("COCKPIT_STATS_URL", "http://127.0.0.1:"+statsPort),
		PoolFile:     *poolFile,
		LogsPython:   envOr("COCKPIT_LOGS_PYTHON", "/home/yungwine/mcp-logs-server/venv/bin/python"),
		LogsModule:   envOr("COCKPIT_LOGS_MODULE", "/home/yungwine/mcp-logs-server"),
		AllowOrigin:  os.Getenv("COCKPIT_ALLOW_ORIGIN"),
	}
	if cfg.PoolFile != "" {
		if _, err := readPoolConfig(cfg.PoolFile); err != nil {
			return fmt.Errorf("pool config: %w", err)
		}
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	server := newCockpit(cfg)
	handler := server.routes()
	addresses := []string{cfg.Listen}
	if cfg.ExtraListen != "" && cfg.ExtraListen != cfg.Listen {
		addresses = append(addresses, cfg.ExtraListen)
	}
	listeners := make([]net.Listener, 0, len(addresses))
	for _, addr := range addresses {
		listener, err := net.Listen("tcp", addr)
		if err != nil {
			for _, opened := range listeners {
				_ = opened.Close()
			}
			return fmt.Errorf("listen %s: %w", addr, err)
		}
		listeners = append(listeners, listener)
	}
	server.start(ctx)
	servers := make([]*http.Server, 0, len(addresses))
	errors := make(chan error, len(addresses))
	for i, listener := range listeners {
		httpServer := &http.Server{Handler: handler, ReadHeaderTimeout: 5 * time.Second}
		servers = append(servers, httpServer)
		log.Printf("cockpit %s listening on %s", cfg.Network, addresses[i])
		go func() { errors <- httpServer.Serve(listener) }()
	}
	finished := make(chan struct{})
	go func() {
		select {
		case <-ctx.Done():
		case <-finished:
		}
		shutdown, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		for _, httpServer := range servers {
			_ = httpServer.Shutdown(shutdown)
		}
	}()
	err := <-errors
	close(finished)
	if err != nil && err != http.ErrServerClosed {
		return err
	}
	return nil
}

func decodeJSON(response *http.Response, target any) error {
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return fmt.Errorf("HTTP %d from %s", response.StatusCode, response.Request.URL.Host)
	}
	return json.NewDecoder(response.Body).Decode(target)
}

func cleanURL(base string) string { return strings.TrimRight(base, "/") }
