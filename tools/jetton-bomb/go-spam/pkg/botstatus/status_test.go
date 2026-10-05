package botstatus

import "testing"

func TestSignatureBindsTimestampAndBody(t *testing.T) {
	body := []byte(`{"network":"testnet"}`)
	signature := Signature("shared secret", "123", body)
	if !Verify("shared secret", "123", body, signature) ||
		Verify("wrong secret", "123", body, signature) ||
		Verify("shared secret", "124", body, signature) ||
		Verify("shared secret", "123", []byte(`{"network":"mainnet"}`), signature) {
		t.Fatal("report authentication did not bind secret, timestamp, and body")
	}
}
