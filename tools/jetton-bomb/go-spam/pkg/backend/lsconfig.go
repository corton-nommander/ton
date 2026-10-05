package backend

import (
	"encoding/json"
	"os"
)

type lsConfig struct {
	Liteservers []struct{} `json:"liteservers"`
}

// readConfigServers returns the number of liteservers in a TON global config JSON file.
func readConfigServers(path string) (int, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return 0, err
	}
	var c lsConfig
	if err := json.Unmarshal(data, &c); err != nil {
		return 0, err
	}
	return len(c.Liteservers), nil
}
